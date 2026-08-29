#include "sg/diff_lcs.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

sg_diff_line *sg_diff_split_lines(const unsigned char *data, size_t len, size_t *count_out)
{
    size_t cap = 16;
    size_t count = 0;
    sg_diff_line *lines = malloc(cap * sizeof(*lines));
    size_t pos = 0;

    if (lines == NULL) {
        *count_out = 0;
        return NULL;
    }

    while (pos < len) {
        const unsigned char *nl = memchr(data + pos, '\n', len - pos);
        size_t linelen = nl != NULL ? (size_t)(nl - (data + pos)) : (len - pos);

        if (count == cap) {
            sg_diff_line *grown;

            cap *= 2;
            grown = realloc(lines, cap * sizeof(*lines));
            if (grown == NULL)
                break;
            lines = grown;
        }
        lines[count].ptr = (const char *)(data + pos);
        lines[count].len = linelen;
        lines[count].has_nl = (nl != NULL);
        count++;

        pos += linelen;
        if (nl != NULL)
            pos++;
    }

    *count_out = count;
    return lines;
}

int sg_diff_lines_equal(sg_diff_line a, sg_diff_line b)
{
    return a.len == b.len && memcmp(a.ptr, b.ptr, a.len) == 0;
}

int sg_diff_lines_equal_exact(sg_diff_line a, sg_diff_line b)
{
    return a.has_nl == b.has_nl && sg_diff_lines_equal(a, b);
}

/* ---- minimal edit script: Myers alignment + group compaction + indent heuristic */

typedef struct {
    sg_diff_group *groups;
    size_t count;
    size_t cap;
} group_builder;

static int gb_push(group_builder *gb, size_t a_off, size_t a_len, size_t b_off, size_t b_len)
{
    if (gb->count == gb->cap) {
        size_t new_cap = gb->cap == 0 ? 8 : gb->cap * 2;
        sg_diff_group *grown = realloc(gb->groups, new_cap * sizeof(*grown));

        if (grown == NULL)
            return -1;
        gb->groups = grown;
        gb->cap = new_cap;
    }
    gb->groups[gb->count].a_off = a_off;
    gb->groups[gb->count].a_len = a_len;
    gb->groups[gb->count].b_off = b_off;
    gb->groups[gb->count].b_len = b_len;
    gb->count++;
    return 0;
}

/* ---- Myers diff (a direct port of git's xdiff/xdiffi.c + xprepare.c,
   v2.55.0), replacing the old LCS-backtrack alignment. See Phase 35 of
   docs/DESIGN.md for the full rationale and the measurements that pinned
   down each of git's constants. sg_diff_lcs_table/_exact and
   backtrack_into_groups (the old alignment this replaces) are gone
   entirely: Phase 35 left src/workdir/merge.c's three-way merge on the LCS
   table on purpose (no differential coverage there, so no net to change its
   behaviour under), and Phase 41 built that net -- tests/fuzz_merge.py --
   and then moved merge onto this aligner too, which removed the table's
   last caller. */

/* Each unique line (by sg_diff_lines_equal_exact) gets one class id,
   mirroring git's xdlclass_t / xdl_classify_record: the point is turning
   "are these two lines the same" into an O(1) integer comparison instead
   of a memcmp, which is what makes the O((N+M)D) time bound on the Myers
   core actually O((N+M)D) instead of O((N+M)D * line length). Any hash
   works here (unlike git's xdl_hash_record_verbatim, ours is not required
   to match git's byte-for-byte) as long as collisions are always resolved
   by an exact content compare before two lines share a class -- that is
   the only property the rest of the algorithm depends on. */
typedef struct {
    sg_diff_line line;
    size_t count_a; /* occurrences of this class in file a (git: len1) */
    size_t count_b; /* occurrences of this class in file b (git: len2) */
    long next;      /* next class chained in the same hash bucket, -1 terminated */
} diff_class;

typedef struct {
    diff_class *classes;
    size_t count, cap;
    long *buckets;
    size_t hsize;
} classifier;

static size_t line_hash(sg_diff_line ln)
{
    size_t h = 5381;
    size_t i;

    for (i = 0; i < ln.len; i++)
        h = h * 33 + (unsigned char)ln.ptr[i];
    h = h * 33 + (size_t)(ln.has_nl ? 1 : 0);
    return h;
}

static int classifier_init(classifier *cf, size_t total_hint)
{
    size_t hbits = 1;

    while (((size_t)1 << hbits) < total_hint && hbits < 24)
        hbits++;
    cf->hsize = (size_t)1 << hbits;
    cf->buckets = malloc(cf->hsize * sizeof(*cf->buckets));
    if (cf->buckets == NULL)
        return -1;
    /* Every byte 0xff -> every long reads back as -1 on a two's complement
       platform (the only kind sg supports, per CLAUDE.md's platform note),
       used here as the bucket-chain terminator. */
    memset(cf->buckets, 0xff, cf->hsize * sizeof(*cf->buckets));
    cf->classes = NULL;
    cf->count = 0;
    cf->cap = 0;
    return 0;
}

static void classifier_free(classifier *cf)
{
    free(cf->buckets);
    free(cf->classes);
}

/* Finds or creates the class for ln, returns its id, or -1 on allocation
   failure. */
static long classify_get(classifier *cf, sg_diff_line ln)
{
    size_t h = line_hash(ln);
    size_t bucket = h & (cf->hsize - 1);
    long idx = cf->buckets[bucket];

    while (idx != -1) {
        if (sg_diff_lines_equal_exact(cf->classes[idx].line, ln))
            return idx;
        idx = cf->classes[idx].next;
    }

    if (cf->count == cf->cap) {
        size_t new_cap = cf->cap == 0 ? 16 : cf->cap * 2;
        diff_class *grown = realloc(cf->classes, new_cap * sizeof(*grown));

        if (grown == NULL)
            return -1;
        cf->classes = grown;
        cf->cap = new_cap;
    }
    idx = (long)cf->count++;
    cf->classes[idx].line = ln;
    cf->classes[idx].count_a = 0;
    cf->classes[idx].count_b = 0;
    cf->classes[idx].next = cf->buckets[bucket];
    cf->buckets[bucket] = idx;
    return idx;
}

/* Classifies every line of both files, filling *acls_out[0..na) /
   *bcls_out[0..nb) with class ids and cf's per-class count_a/count_b.
   Returns -1 on allocation failure (cf is left in a freeable state either
   way, caller always calls classifier_free). */
static int classify_all(const sg_diff_line *a, size_t na, const sg_diff_line *b, size_t nb, classifier *cf,
                        long **acls_out, long **bcls_out)
{
    long *acls, *bcls;
    size_t i;

    if (classifier_init(cf, na + nb + 1) != 0)
        return -1;

    acls = malloc((na > 0 ? na : 1) * sizeof(*acls));
    bcls = malloc((nb > 0 ? nb : 1) * sizeof(*bcls));
    if (acls == NULL || bcls == NULL) {
        free(acls);
        free(bcls);
        return -1;
    }

    for (i = 0; i < na; i++) {
        long idx = classify_get(cf, a[i]);

        if (idx < 0) {
            free(acls);
            free(bcls);
            return -1;
        }
        acls[i] = idx;
        cf->classes[idx].count_a++;
    }
    for (i = 0; i < nb; i++) {
        long idx = classify_get(cf, b[i]);

        if (idx < 0) {
            free(acls);
            free(bcls);
            return -1;
        }
        bcls[i] = idx;
        cf->classes[idx].count_b++;
    }

    *acls_out = acls;
    *bcls_out = bcls;
    return 0;
}

/* Port of git's xdl_trim_ends (xprepare.c): strips the common leading and
   trailing run (by class id, i.e. exact content match) shared by both
   files, so the expensive part of the algorithm only ever looks at the
   middle. dend1/dend2 mirror git's xdf1->dend/xdf2->dend: the LAST index
   (inclusive) still inside the middle region for each file, which can be
   less than dstart (an empty middle, e.g. when one file is wholly a
   prefix/suffix of the other). */
static void trim_ends(const long *acls, size_t na, const long *bcls, size_t nb, long *dstart, long *dend1,
                      long *dend2)
{
    long i, lim;

    lim = (long)(na < nb ? na : nb);
    for (i = 0; i < lim; i++)
        if (acls[i] != bcls[i])
            break;
    *dstart = i;

    lim -= i;
    for (i = 0; i < lim; i++)
        if (acls[(long)na - 1 - i] != bcls[(long)nb - 1 - i])
            break;
    *dend1 = (long)na - i - 1;
    *dend2 = (long)nb - i - 1;
}

#define SG_XDL_KPDIS_RUN 4
#define SG_XDL_MAX_EQLIMIT 1024
#define SG_XDL_SIMSCAN_WINDOW 100
#define SG_XDL_MAX_COST_MIN 256
#define SG_XDL_HEUR_MIN_COST 256
#define SG_XDL_SNAKE_CNT 20
#define SG_XDL_K_HEUR 4
/* git's XDL_LINE_MAX is (1UL << (CHAR_BIT*sizeof(long)-1)) - 1, i.e. exactly
   LONG_MAX on any two's complement platform -- which is the only kind sg
   supports (CLAUDE.md's platform note). */
#define SG_XDL_LINE_MAX LONG_MAX

#define SG_MYERS_DISCARD 0
#define SG_MYERS_KEEP 1
#define SG_MYERS_INVESTIGATE 2

/* Not a real square root -- a shift-based approximation, matching git's
   xdl_bogosqrt (xutils.c) exactly, including its use as a NON-monotonic-
   feeling but deterministic size limit rather than an actual sqrt(). Using
   a real sqrt() here would introduce floating point into a path that
   currently has none (CLAUDE.md's "no floating point" note for this
   phase). */
static uint64_t bogosqrt(uint64_t n)
{
    uint64_t i;

    for (i = 1; n > 0; n >>= 2)
        i <<= 1;
    return i;
}

/* Port of git's xdl_clean_mmatch (xprepare.c): decides whether an
   INVESTIGATE line at position i (within a length-len action[] window)
   should be demoted to DISCARD because it sits inside a run of otherwise-
   discardable lines. */
static int clean_mmatch(const unsigned char *action, long i, long len)
{
    long r, rdis0, rpdis0, rdis1, rpdis1;
    long s = 0, e = len - 1;

    if (i - s > SG_XDL_SIMSCAN_WINDOW)
        s = i - SG_XDL_SIMSCAN_WINDOW;
    if (e - i > SG_XDL_SIMSCAN_WINDOW)
        e = i + SG_XDL_SIMSCAN_WINDOW;

    for (r = 1, rdis0 = 0, rpdis0 = 1; (i - r) >= s; r++) {
        if (action[i - r] == SG_MYERS_DISCARD)
            rdis0++;
        else if (action[i - r] == SG_MYERS_INVESTIGATE)
            rpdis0++;
        else
            break;
    }
    if (rdis0 == 0)
        return 0;
    for (r = 1, rdis1 = 0, rpdis1 = 1; (i + r) <= e; r++) {
        if (action[i + r] == SG_MYERS_DISCARD)
            rdis1++;
        else if (action[i + r] == SG_MYERS_INVESTIGATE)
            rpdis1++;
        else
            break;
    }
    if (rdis1 == 0)
        return 0;
    rdis1 += rdis0;
    rpdis1 += rpdis0;

    return rpdis1 * SG_XDL_KPDIS_RUN < (rpdis1 + rdis1);
}

/* Port of git's xdl_cleanup_records for ONE side (called once for a
   against b's counts, once for b against a's counts -- git does the same
   pair of loops inline in one function, split here because sg's "which
   count field" differs per side, not because the algorithm differs).
   `cls` is the full per-original-index class array for THIS file (size n);
   `other_count_of(class)` reads the OTHER file's occurrence count for that
   class (count_b when processing file a, count_a when processing file b).
   [dstart, dend] is the middle region trim_ends left; anything outside it
   is common prefix/suffix and never touched (changed[] stays 0, as
   calloc'd by the caller). A line judged DISCARD gets changed[i]=1
   immediately (git: "obviously changed, no candidate for alignment"); a
   line judged KEEP gets appended (by ORIGINAL index) to *ref_out, which is
   the coordinate space the Myers core actually runs in. Returns -1 only on
   allocation failure. */
static int cleanup_side(const long *cls, size_t n, const diff_class *classes, int other_is_b, long dstart,
                        long dend, unsigned char *changed, long **ref_out, size_t *nreff_out)
{
    long off = dstart;
    long len = dend - off + 1;
    unsigned char *action;
    long *refidx;
    long i;
    long mlim;
    size_t nreff = 0;

    if (len <= 0) {
        *ref_out = NULL;
        *nreff_out = 0;
        return 0;
    }

    action = calloc((size_t)len, 1);
    refidx = malloc((size_t)len * sizeof(*refidx));
    if (action == NULL || refidx == NULL) {
        free(action);
        free(refidx);
        return -1;
    }

    mlim = (long)bogosqrt((uint64_t)n);
    if (mlim > SG_XDL_MAX_EQLIMIT)
        mlim = SG_XDL_MAX_EQLIMIT;

    for (i = 0; i < len; i++) {
        long cls_idx = cls[i + off];
        size_t nm = other_is_b ? classes[cls_idx].count_b : classes[cls_idx].count_a;

        if (nm == 0)
            action[i] = SG_MYERS_DISCARD;
        else if ((long)nm < mlim)
            action[i] = SG_MYERS_KEEP;
        else
            action[i] = SG_MYERS_INVESTIGATE;
    }

    for (i = 0; i < len; i++) {
        unsigned char act = action[i];

        if (act == SG_MYERS_INVESTIGATE)
            act = clean_mmatch(action, i, len) ? SG_MYERS_DISCARD : SG_MYERS_KEEP;

        if (act == SG_MYERS_KEEP)
            refidx[nreff++] = i + off;
        else
            changed[i + off] = 1;
    }

    free(action);
    *ref_out = refidx;
    *nreff_out = nreff;
    return 0;
}

/* The Myers core (xdl_recs_cmp/xdl_split in git) runs entirely in
   "compacted" coordinates: index k into file a means aref[k] in the
   original array, and get_hash_* looks up that original line's class id.
   changed[] is written back at the ORIGINAL index -- this is exactly the
   coordinate mapping flagged in the spec as the easiest place to get
   wrong, and it is why aref/bref exist as a separate array instead of
   folding cleanup_side's KEEP/DISCARD decision directly into a boolean. */
typedef struct {
    const long *acls, *bcls;
    const long *aref, *bref;
    unsigned char *achanged, *bchanged;
} myers_ctx;

static long get_hash_a(const myers_ctx *mc, long idx)
{
    return mc->acls[mc->aref[idx]];
}

static long get_hash_b(const myers_ctx *mc, long idx)
{
    return mc->bcls[mc->bref[idx]];
}

typedef struct {
    long i1, i2;
    int min_lo, min_hi;
} myers_split;

typedef struct {
    long mxcost;
    long snake_cnt;
    long heur_min;
} myers_env;

/* Direct port of git's xdl_split (xdiffi.c): Myers's O(ND) forward/backward
   search for the box (off1,lim1)x(off2,lim2), including both heuristics
   (opportunistic snake sampling and forced convergence past mxcost). See
   Phase 35 of docs/DESIGN.md for why both are kept despite having no
   measured witness on this project's fixtures: omitting them is a
   real, if rare, divergence from git, and the project's standard is
   byte-for-byte compatibility, not "matches on everything we happened to
   test". */
static long myers_split_box(const myers_ctx *mc, long off1, long lim1, long off2, long lim2, long *kvdf,
                            long *kvdb, int need_min, myers_split *spl, const myers_env *env)
{
    long dmin = off1 - lim2, dmax = lim1 - off2;
    long fmid = off1 - off2, bmid = lim1 - lim2;
    long odd = (fmid - bmid) & 1;
    long fmin = fmid, fmax = fmid;
    long bmin = bmid, bmax = bmid;
    long ec, d, i1, i2, prev1, best, dd, v, k;

    kvdf[fmid] = off1;
    kvdb[bmid] = lim1;

    for (ec = 1;; ec++) {
        int got_snake = 0;

        if (fmin > dmin)
            kvdf[--fmin - 1] = -1;
        else
            ++fmin;
        if (fmax < dmax)
            kvdf[++fmax + 1] = -1;
        else
            --fmax;

        for (d = fmax; d >= fmin; d -= 2) {
            if (kvdf[d - 1] >= kvdf[d + 1])
                i1 = kvdf[d - 1] + 1;
            else
                i1 = kvdf[d + 1];
            prev1 = i1;
            i2 = i1 - d;
            while (i1 < lim1 && i2 < lim2 && get_hash_a(mc, i1) == get_hash_b(mc, i2)) {
                i1++;
                i2++;
            }
            if (i1 - prev1 > env->snake_cnt)
                got_snake = 1;
            kvdf[d] = i1;
            if (odd && bmin <= d && d <= bmax && kvdb[d] <= i1) {
                spl->i1 = i1;
                spl->i2 = i2;
                spl->min_lo = spl->min_hi = 1;
                return ec;
            }
        }

        if (bmin > dmin)
            kvdb[--bmin - 1] = SG_XDL_LINE_MAX;
        else
            ++bmin;
        if (bmax < dmax)
            kvdb[++bmax + 1] = SG_XDL_LINE_MAX;
        else
            --bmax;

        for (d = bmax; d >= bmin; d -= 2) {
            if (kvdb[d - 1] < kvdb[d + 1])
                i1 = kvdb[d - 1];
            else
                i1 = kvdb[d + 1] - 1;
            prev1 = i1;
            i2 = i1 - d;
            while (i1 > off1 && i2 > off2 && get_hash_a(mc, i1 - 1) == get_hash_b(mc, i2 - 1)) {
                i1--;
                i2--;
            }
            if (prev1 - i1 > env->snake_cnt)
                got_snake = 1;
            kvdb[d] = i1;
            if (!odd && fmin <= d && d <= fmax && i1 <= kvdf[d]) {
                spl->i1 = i1;
                spl->i2 = i2;
                spl->min_lo = spl->min_hi = 1;
                return ec;
            }
        }

        if (need_min)
            continue;

        if (got_snake && ec > env->heur_min) {
            for (best = 0, d = fmax; d >= fmin; d -= 2) {
                dd = d > fmid ? d - fmid : fmid - d;
                i1 = kvdf[d];
                i2 = i1 - d;
                v = (i1 - off1) + (i2 - off2) - dd;

                if (v > SG_XDL_K_HEUR * ec && v > best && off1 + env->snake_cnt <= i1 && i1 < lim1 &&
                    off2 + env->snake_cnt <= i2 && i2 < lim2) {
                    for (k = 1; get_hash_a(mc, i1 - k) == get_hash_b(mc, i2 - k); k++)
                        if (k == env->snake_cnt) {
                            best = v;
                            spl->i1 = i1;
                            spl->i2 = i2;
                            break;
                        }
                }
            }
            if (best > 0) {
                spl->min_lo = 1;
                spl->min_hi = 0;
                return ec;
            }

            for (best = 0, d = bmax; d >= bmin; d -= 2) {
                dd = d > bmid ? d - bmid : bmid - d;
                i1 = kvdb[d];
                i2 = i1 - d;
                v = (lim1 - i1) + (lim2 - i2) - dd;

                if (v > SG_XDL_K_HEUR * ec && v > best && off1 < i1 && i1 <= lim1 - env->snake_cnt &&
                    off2 < i2 && i2 <= lim2 - env->snake_cnt) {
                    for (k = 0; get_hash_a(mc, i1 + k) == get_hash_b(mc, i2 + k); k++)
                        if (k == env->snake_cnt - 1) {
                            best = v;
                            spl->i1 = i1;
                            spl->i2 = i2;
                            break;
                        }
                }
            }
            if (best > 0) {
                spl->min_lo = 0;
                spl->min_hi = 1;
                return ec;
            }
        }

        if (ec >= env->mxcost) {
            long fbest, fbest1, bbest, bbest1;

            fbest = fbest1 = -1;
            for (d = fmax; d >= fmin; d -= 2) {
                i1 = kvdf[d] < lim1 ? kvdf[d] : lim1;
                i2 = i1 - d;
                if (lim2 < i2) {
                    i1 = lim2 + d;
                    i2 = lim2;
                }
                if (fbest < i1 + i2) {
                    fbest = i1 + i2;
                    fbest1 = i1;
                }
            }

            bbest = bbest1 = SG_XDL_LINE_MAX;
            for (d = bmax; d >= bmin; d -= 2) {
                i1 = off1 > kvdb[d] ? off1 : kvdb[d];
                i2 = i1 - d;
                if (i2 < off2) {
                    i1 = off2 + d;
                    i2 = off2;
                }
                if (i1 + i2 < bbest) {
                    bbest = i1 + i2;
                    bbest1 = i1;
                }
            }

            if ((lim1 + lim2) - bbest < fbest - (off1 + off2)) {
                spl->i1 = fbest1;
                spl->i2 = fbest - fbest1;
                spl->min_lo = 1;
                spl->min_hi = 0;
            } else {
                spl->i1 = bbest1;
                spl->i2 = bbest - bbest1;
                spl->min_lo = 0;
                spl->min_hi = 1;
            }
            return ec;
        }
    }
}

/* Direct port of git's xdl_recs_cmp (xdiffi.c): shrink the box by eating
   matching snakes off both ends, then either mark one whole side changed
   (the other side is empty) or split-and-recurse. */
static void myers_recs_cmp(const myers_ctx *mc, long off1, long lim1, long off2, long lim2, long *kvdf,
                           long *kvdb, int need_min, const myers_env *env)
{
    for (; off1 < lim1 && off2 < lim2 && get_hash_a(mc, off1) == get_hash_b(mc, off2); off1++, off2++)
        ;
    for (; off1 < lim1 && off2 < lim2 && get_hash_a(mc, lim1 - 1) == get_hash_b(mc, lim2 - 1); lim1--, lim2--)
        ;

    if (off1 == lim1) {
        for (; off2 < lim2; off2++)
            mc->bchanged[mc->bref[off2]] = 1;
    } else if (off2 == lim2) {
        for (; off1 < lim1; off1++)
            mc->achanged[mc->aref[off1]] = 1;
    } else {
        myers_split spl;

        myers_split_box(mc, off1, lim1, off2, lim2, kvdf, kvdb, need_min, &spl, env);
        myers_recs_cmp(mc, off1, spl.i1, off2, spl.i2, kvdf, kvdb, spl.min_lo, env);
        myers_recs_cmp(mc, spl.i1, lim1, spl.i2, lim2, kvdf, kvdb, spl.min_hi, env);
    }
}

/* Top-level driver: fills achanged[0..na) / bchanged[0..nb) (both the
   offset-by-1 buffers sg_diff_build_script owns, so index -1 and index n
   are both valid and read as 0) exactly like the old backtrack_into_groups
   used to, but via classify -> trim -> cleanup -> Myers instead of an
   O(na*nb) LCS table. need_min is always 0 at the top level: sg has no
   --minimal equivalent, matching git's own default (xpp->flags &
   XDF_NEED_MINIMAL) being unset for a plain `git diff`. Returns -1 only on
   allocation failure. */
static int myers_diff(const sg_diff_line *a, size_t na, const sg_diff_line *b, size_t nb, unsigned char *achanged,
                      unsigned char *bchanged)
{
    classifier cf;
    long *acls = NULL, *bcls = NULL;
    long dstart, dend1, dend2;
    long *aref = NULL, *bref = NULL;
    size_t nreffa = 0, nreffb = 0;
    long ndiags;
    long *kvd = NULL, *kvdf, *kvdb;
    myers_ctx mc;
    myers_env env;
    int rc = -1;

    memset(&cf, 0, sizeof(cf));
    if (classify_all(a, na, b, nb, &cf, &acls, &bcls) != 0) {
        classifier_free(&cf);
        return -1;
    }

    trim_ends(acls, na, bcls, nb, &dstart, &dend1, &dend2);

    if (cleanup_side(acls, na, cf.classes, 1, dstart, dend1, achanged, &aref, &nreffa) != 0)
        goto out;
    if (cleanup_side(bcls, nb, cf.classes, 0, dstart, dend2, bchanged, &bref, &nreffb) != 0)
        goto out;

    ndiags = (long)nreffa + (long)nreffb + 3;
    kvd = malloc((size_t)(2 * ndiags + 2) * sizeof(*kvd));
    if (kvd == NULL)
        goto out;
    kvdf = kvd;
    kvdb = kvdf + ndiags;
    kvdf += (long)nreffb + 1;
    kvdb += (long)nreffb + 1;

    env.mxcost = (long)bogosqrt((uint64_t)ndiags);
    if (env.mxcost < SG_XDL_MAX_COST_MIN)
        env.mxcost = SG_XDL_MAX_COST_MIN;
    env.snake_cnt = SG_XDL_SNAKE_CNT;
    env.heur_min = SG_XDL_HEUR_MIN_COST;

    mc.acls = acls;
    mc.bcls = bcls;
    mc.aref = aref;
    mc.bref = bref;
    mc.achanged = achanged;
    mc.bchanged = bchanged;

    myers_recs_cmp(&mc, 0, (long)nreffa, 0, (long)nreffb, kvdf, kvdb, 0, &env);
    rc = 0;

out:
    free(kvd);
    free(aref);
    free(bref);
    free(acls);
    free(bcls);
    classifier_free(&cf);
    return rc;
}

/* ---- indentation heuristic (git's diff.indentHeuristic, reconstructed) --

   Checked against git's xdiff/xdiffi.c (get_indent / measure_split /
   score_add_split / score_cmp, current `master` as of this check) line by
   line. Every constant below now matches git's #define of the same
   quantity (renamed with the SG_INDENT_ prefix; see each line for the
   git name being mirrored). None of them are "from memory" any more. */

#define SG_INDENT_MAX 200      /* git: MAX_INDENT */
#define SG_INDENT_MAX_BLANKS 20 /* git: MAX_BLANKS */

#define SG_INDENT_START_OF_FILE_PENALTY 1  /* git: START_OF_FILE_PENALTY */
#define SG_INDENT_END_OF_FILE_PENALTY 21   /* git: END_OF_FILE_PENALTY */
#define SG_INDENT_TOTAL_BLANK_WEIGHT (-30) /* git: TOTAL_BLANK_WEIGHT */
#define SG_INDENT_POST_BLANK_WEIGHT 6      /* git: POST_BLANK_WEIGHT */
/* git's RELATIVE_INDENT_PENALTY is (-4) -- a BONUS, not a penalty (it makes
   splitting after a more-indented line more, not less, favored). An earlier
   revision of this file had it as a memory-reconstructed +4 (a penalty),
   which happened to pass the fuzzer's A1 anchor but produced the wrong
   position on A2 (tests/test_diff_out.c's no-blank-separator duplicate
   block) -- see that test's comment for the exact wrong output this sign
   error reproduced. Corrected to git's actual -4 here. */
#define SG_INDENT_RELATIVE_INDENT_PENALTY (-4) /* git: RELATIVE_INDENT_PENALTY */
#define SG_INDENT_RELATIVE_INDENT_WITH_BLANK_PENALTY 10 /* git: RELATIVE_INDENT_WITH_BLANK_PENALTY */
#define SG_INDENT_RELATIVE_OUTDENT_PENALTY 24 /* git: RELATIVE_OUTDENT_PENALTY */
#define SG_INDENT_RELATIVE_OUTDENT_WITH_BLANK_PENALTY 17 /* git: RELATIVE_OUTDENT_WITH_BLANK_PENALTY */
#define SG_INDENT_RELATIVE_DEDENT_PENALTY 23 /* git: RELATIVE_DEDENT_PENALTY */
#define SG_INDENT_RELATIVE_DEDENT_WITH_BLANK_PENALTY 17 /* git: RELATIVE_DEDENT_WITH_BLANK_PENALTY */

/* Verified against git's xdiff/xdiffi.c (score_cmp): the comparison between
   two candidate split positions is NOT the sum of the two penalty scalars
   alone. It is dominated by "effective_indent" -- the sum, over the two
   splits bracketing the group, of each split's indent (or its post_indent
   when the split itself falls on a blank/EOF line) -- weighted by
   SG_INDENT_WEIGHT, with `penalty` only used to break ties on
   effective_indent. Omitting effective_indent entirely (an earlier version
   of this file did) picks the wrong position whenever the two candidates
   differ in indentation, which given INDENT_WEIGHT's size is most of the
   time -- confirmed against git 2.55.0 via tests/fuzz_diff.py. */
#define SG_INDENT_WEIGHT 60

/* -1 for a blank (all-whitespace, including empty) line; otherwise the
   leading-whitespace width (space = 1, tab = round up to next multiple of
   8), capped at SG_INDENT_MAX. */
static int line_indent(sg_diff_line ln)
{
    size_t i;
    int indent = 0;

    for (i = 0; i < ln.len; i++) {
        char c = ln.ptr[i];

        if (c == ' ')
            indent += 1;
        else if (c == '\t')
            indent += 8 - indent % 8;
        else if (c == '\v' || c == '\f' || c == '\r')
            continue;
        else
            return indent;
        if (indent >= SG_INDENT_MAX)
            return SG_INDENT_MAX;
    }
    return -1;
}

typedef struct {
    int end_of_file;
    int indent;
    long pre_blank;
    int pre_indent;
    long post_blank;
    int post_indent;
} split_measurement;

static void measure_split(const sg_diff_line *rec, size_t n, size_t split, split_measurement *m)
{
    size_t i;

    if (split >= n) {
        m->end_of_file = 1;
        m->indent = -1;
    } else {
        m->end_of_file = 0;
        m->indent = line_indent(rec[split]);
    }

    m->pre_blank = 0;
    m->pre_indent = -1;
    for (i = split; i-- > 0;) {
        m->pre_indent = line_indent(rec[i]);
        if (m->pre_indent != -1)
            break;
        m->pre_blank++;
        if (m->pre_blank == SG_INDENT_MAX_BLANKS) {
            m->pre_indent = 0;
            break;
        }
    }

    m->post_blank = 0;
    m->post_indent = -1;
    for (i = split + 1; i < n; i++) {
        m->post_indent = line_indent(rec[i]);
        if (m->post_indent != -1)
            break;
        m->post_blank++;
        if (m->post_blank == SG_INDENT_MAX_BLANKS) {
            m->post_indent = 0;
            break;
        }
    }
}

typedef struct {
    int effective_indent;
    int penalty;
} split_score;

/* Mirrors git's score_add_split(): ACCUMULATES into *s rather than returning
   a fresh value, because a position's score is the sum of the two splits
   that bracket the group (see score_position below), not either split in
   isolation. */
static void score_add_split(const sg_diff_line *rec, size_t n, size_t split, split_score *s)
{
    split_measurement m;
    int post_blank, total_blank, indent, any_blanks;

    measure_split(rec, n, split, &m);

    if (m.pre_indent == -1 && m.pre_blank == 0)
        s->penalty += SG_INDENT_START_OF_FILE_PENALTY;
    if (m.end_of_file)
        s->penalty += SG_INDENT_END_OF_FILE_PENALTY;

    post_blank = (m.indent == -1) ? (int)(1 + m.post_blank) : 0;
    total_blank = (int)m.pre_blank + post_blank;

    s->penalty += SG_INDENT_TOTAL_BLANK_WEIGHT * total_blank;
    s->penalty += SG_INDENT_POST_BLANK_WEIGHT * post_blank;

    indent = (m.indent != -1) ? m.indent : m.post_indent;
    any_blanks = (total_blank != 0);

    s->effective_indent += indent;

    if (indent == -1 || m.pre_indent == -1) {
        /* no adjustment */
    } else if (indent > m.pre_indent) {
        s->penalty += any_blanks ? SG_INDENT_RELATIVE_INDENT_WITH_BLANK_PENALTY : SG_INDENT_RELATIVE_INDENT_PENALTY;
    } else if (indent == m.pre_indent) {
        /* no adjustment */
    } else if (m.post_indent != -1 && m.post_indent > indent) {
        s->penalty += any_blanks ? SG_INDENT_RELATIVE_OUTDENT_WITH_BLANK_PENALTY : SG_INDENT_RELATIVE_OUTDENT_PENALTY;
    } else {
        s->penalty += any_blanks ? SG_INDENT_RELATIVE_DEDENT_WITH_BLANK_PENALTY : SG_INDENT_RELATIVE_DEDENT_PENALTY;
    }
}

/* Mirrors git's score_cmp(): effective_indent (weighted) dominates, penalty
   only breaks ties. Negative means s1 is the better (more favored) split. */
static int score_cmp(const split_score *s1, const split_score *s2)
{
    int cmp_indents = (s1->effective_indent > s2->effective_indent) - (s1->effective_indent < s2->effective_indent);

    return SG_INDENT_WEIGHT * cmp_indents + (s1->penalty - s2->penalty);
}

/* Scores candidate position p (an index into the group's OWN axis: a_off
   for a pure delete, b_off for a pure insert) by summing the two splits
   just above and just below the group's content in that axis's line array. */
static split_score score_position(const sg_diff_line *rec, size_t n, size_t pos, size_t len)
{
    split_score s = {0, 0};

    score_add_split(rec, n, pos + len, &s);
    score_add_split(rec, n, pos, &s);
    return s;
}

/* ---- per-file group sliding (mirrors git's struct xdlgroup / group_*) --

   Unlike the paired sg_diff_group the alignment step produces, sliding operates
   on ONE file's own "changed" bitmap at a time: changed[i] is true iff
   line i of THIS file was touched by some edit (deleted, for the a-side;
   inserted, for the b-side -- a "replace" simply means both sides have a
   changed run at once, with no requirement that the runs line up). A
   group is a maximal run of changed lines, or the (possibly empty) gap
   between two runs -- there is one such gap immediately before every
   changed[i]==false position, including one at index 0 and one at n, so
   group_next/group_previous can always find a next/previous group by
   walking exactly one context line at a time.

   `changed` must point at a buffer of n+2 bytes offset by 1 (index -1 and
   index n both readable and zero) -- see sg_diff_build_script, which owns
   the allocation. start/end are `long`, not size_t, purely so start-1 and
   end-1 can be formed and compared without wraparound; git's own xdlgroup
   uses the same signed-index trick for the same reason. */
typedef struct {
    long start, end;
} slide_group;

static void group_init(const unsigned char *changed, slide_group *g)
{
    g->start = g->end = 0;
    while (changed[g->end])
        g->end++;
}

static int group_next(const unsigned char *changed, long n, slide_group *g)
{
    if (g->end == n)
        return -1;
    g->start = g->end + 1;
    for (g->end = g->start; changed[g->end]; g->end++)
        ;
    return 0;
}

static int group_previous(const unsigned char *changed, slide_group *g)
{
    if (g->start == 0)
        return -1;
    g->end = g->start - 1;
    for (g->start = g->end; changed[g->start - 1]; g->start--)
        ;
    return 0;
}

/* If g can be slid one line toward the end of its own file, do so (merging
   into whatever changed run it bumps into). Only valid within THIS file:
   the line released (rec[g->start]) and the line absorbed (rec[g->end])
   must be equal, because releasing/absorbing swaps which of two
   identical-content lines plays "changed" vs "context" -- it never changes
   what the diff represents. */
static int group_slide_down(const sg_diff_line *rec, long n, unsigned char *changed, slide_group *g)
{
    if (g->end < n && sg_diff_lines_equal_exact(rec[g->start], rec[g->end])) {
        changed[g->start++] = 0;
        changed[g->end++] = 1;
        while (changed[g->end])
            g->end++;
        return 0;
    }
    return -1;
}

static int group_slide_up(const sg_diff_line *rec, unsigned char *changed, slide_group *g)
{
    if (g->start > 0 && sg_diff_lines_equal_exact(rec[g->start - 1], rec[g->end - 1])) {
        changed[--g->start] = 1;
        changed[--g->end] = 0;
        while (changed[g->start - 1])
            g->start--;
        return 0;
    }
    return -1;
}

#define SG_INDENT_HEURISTIC_MAX_SLIDING 100 /* verified: xdiffi.c's own constant, same name */

/* Mirrors git's xdl_change_compact(): slides every maximal changed run in
   `changed` (this file's own bitmap, scored against `rec`/`n`, this file's
   own lines) as far up and down as group_slide_up/down allow, merging same
   -file neighbours it bumps into along the way. `other_changed`/`on` is the
   OTHER file's bitmap -- git's xdf1/xdf2 pairing guarantees the two files
   have the same number of unchanged (context) lines in the same relative
   order, so walking group_next/group_previous on both in lockstep visits
   the "same" gap index in each; that is used ONLY as a tie-breaker: once a
   group has been slid to its full up+down reachable range, prefer whatever
   position (if any) still has a changed run in the other file at the same
   gap (avoiding splitting what was really one edit into a separate
   delete-then-add), and only fall back to the indent heuristic when no
   such alignment exists anywhere in range. */
static void compact_one_side(const sg_diff_line *rec, long n, unsigned char *changed,
                             const unsigned char *other_changed, long on, int indent_heuristic)
{
    slide_group g, go;

    (void)on;
    group_init(changed, &g);
    group_init(other_changed, &go);

    for (;;) {
        long earliest_end, end_matching_other, groupsize;

        if (g.end == g.start)
            goto next;

        do {
            groupsize = g.end - g.start;
            end_matching_other = -1;

            while (group_slide_up(rec, changed, &g) == 0)
                group_previous(other_changed, &go);

            earliest_end = g.end;
            if (go.end > go.start)
                end_matching_other = g.end;

            for (;;) {
                if (group_slide_down(rec, n, changed, &g) != 0)
                    break;
                group_next(other_changed, on, &go);
                if (go.end > go.start)
                    end_matching_other = g.end;
            }
        } while (groupsize != g.end - g.start);

        if (g.end == earliest_end) {
            /* fully unmovable -- nothing to decide */
        } else if (end_matching_other != -1) {
            while (go.end == go.start) {
                group_slide_up(rec, changed, &g);
                group_previous(other_changed, &go);
            }
        } else if (indent_heuristic) {
            long shift, best_shift = -1;
            split_score best_score = {0, 0};

            shift = earliest_end;
            if (g.end - groupsize - 1 > shift)
                shift = g.end - groupsize - 1;
            if (g.end - SG_INDENT_HEURISTIC_MAX_SLIDING > shift)
                shift = g.end - SG_INDENT_HEURISTIC_MAX_SLIDING;
            /* Ascending, <=0 (not <0): among equally-good splits the LAST
               (highest, i.e. furthest down) one wins, matching git and this
               module's own no-heuristic default of sliding to the bottom. */
            for (; shift <= g.end; shift++) {
                split_score score = score_position(rec, (size_t)n, (size_t)(shift - groupsize),
                                                   (size_t)groupsize);

                if (best_shift == -1 || score_cmp(&score, &best_score) <= 0) {
                    best_score = score;
                    best_shift = shift;
                }
            }
            while (g.end > best_shift) {
                group_slide_up(rec, changed, &g);
                group_previous(other_changed, &go);
            }
        }

    next:
        if (group_next(changed, n, &g) != 0)
            break;
        group_next(other_changed, on, &go);
    }
}

/* Rebuilds the final paired sg_diff_group list from the two (now
   compacted) per-file bitmaps. Because achanged/bchanged share the same
   count of unchanged positions in the same order (see compact_one_side's
   comment), walking both index cursors forward together and only
   advancing one past a changed run when THAT file's bitmap says so keeps
   them synchronized: whenever neither is changed at the current pair, the
   two positions are the same matched context line and both advance
   together; whenever either is changed, the full run in each file
   (independently -- possibly zero-length in one of them) becomes one
   sg_diff_group, which is a plain "replace" only when both runs happen to
   be non-empty at the same synchronized position, not a distinguished
   case the caller has to special-case. */
static int build_groups_from_changed(const unsigned char *achanged, size_t na, const unsigned char *bchanged,
                                     size_t nb, group_builder *gb)
{
    size_t i = 0, j = 0;

    while (i < na || j < nb) {
        if ((i < na && achanged[i]) || (j < nb && bchanged[j])) {
            size_t a_off = i, b_off = j;

            while (i < na && achanged[i])
                i++;
            while (j < nb && bchanged[j])
                j++;
            if (gb_push(gb, a_off, i - a_off, b_off, j - b_off) != 0)
                return -1;
        } else {
            i++;
            j++;
        }
    }
    return 0;
}

sg_diff_script *sg_diff_build_script(const sg_diff_line *a, size_t na, const sg_diff_line *b, size_t nb,
                                    int indent_heuristic)
{
    group_builder final_gb;
    sg_diff_script *script;
    unsigned char *achanged_buf, *bchanged_buf, *achanged, *bchanged;

    /* Two independent per-file "changed" bitmaps, offset by 1 so index -1
       and index n both read as 0 (see compact_one_side/group_*'s own
       comments for why that sentinel matters). Filled directly by
       myers_diff -- see that function's own comment for the coordinate
       mapping (compacted Myers space back to these original indices). */
    achanged_buf = calloc(na + 2, 1);
    bchanged_buf = calloc(nb + 2, 1);
    if (achanged_buf == NULL || bchanged_buf == NULL) {
        free(achanged_buf);
        free(bchanged_buf);
        return NULL;
    }
    achanged = achanged_buf + 1;
    bchanged = bchanged_buf + 1;

    if (myers_diff(a, na, b, nb, achanged, bchanged) != 0) {
        free(achanged_buf);
        free(bchanged_buf);
        return NULL;
    }

    /* Mirrors git's xdl_build_script call order: xdf1 (a's deletions) is
       compacted first, consulting b's bitmap as myers_diff just left it;
       THEN xdf2 (b's insertions) is compacted consulting a's
       bitmap as compact_one_side above just left it. Swapping this order
       changes output on any hunk where both sides have room to slide --
       matching git means matching the order, not just the algorithm. */
    compact_one_side(a, (long)na, achanged, bchanged, (long)nb, indent_heuristic);
    compact_one_side(b, (long)nb, bchanged, achanged, (long)na, indent_heuristic);

    final_gb.groups = NULL;
    final_gb.count = 0;
    final_gb.cap = 0;
    if (build_groups_from_changed(achanged, na, bchanged, nb, &final_gb) != 0) {
        free(achanged_buf);
        free(bchanged_buf);
        free(final_gb.groups);
        return NULL;
    }
    free(achanged_buf);
    free(bchanged_buf);

    script = malloc(sizeof(*script));
    if (script == NULL) {
        free(final_gb.groups);
        return NULL;
    }
    script->groups = final_gb.groups;
    script->count = final_gb.count;
    return script;
}

void sg_diff_script_free(sg_diff_script *script)
{
    if (script == NULL)
        return;
    free(script->groups);
    free(script);
}
