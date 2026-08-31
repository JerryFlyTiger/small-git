#include "sg/merge.h"

#include "sg/chunk.h"
#include "sg/diff.h"
#include "sg/diff_lcs.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/quote.h"
#include "sg/repo.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ==================== merge base ==================== */

typedef struct {
    unsigned char (*ids)[SG_SHA1_RAW_LEN];
    size_t count, cap;
} id_set;

static void id_set_free(id_set *s)
{
    free(s->ids);
    s->ids = NULL;
    s->count = 0;
    s->cap = 0;
}

static int id_set_contains(const id_set *s, const unsigned char id[SG_SHA1_RAW_LEN])
{
    size_t i;

    for (i = 0; i < s->count; i++) {
        if (memcmp(s->ids[i], id, SG_SHA1_RAW_LEN) == 0)
            return 1;
    }
    return 0;
}

/* Returns 1 if id was newly added, 0 if already present, -1 on OOM. */
static int id_set_add(id_set *s, const unsigned char id[SG_SHA1_RAW_LEN])
{
    if (id_set_contains(s, id))
        return 0;
    if (s->count == s->cap) {
        size_t new_cap = s->cap == 0 ? 16 : s->cap * 2;
        unsigned char(*grown)[SG_SHA1_RAW_LEN] = realloc(s->ids, new_cap * sizeof(*grown));

        if (grown == NULL)
            return -1;
        s->ids = grown;
        s->cap = new_cap;
    }
    memcpy(s->ids[s->count], id, SG_SHA1_RAW_LEN);
    s->count++;
    return 1;
}

/* Collects start and every ancestor of start (walking ALL parents) into
   *out, including start itself -- matching git's convention that a commit
   is its own merge-base with one of its descendants. Returns 0 on success,
   -1 on allocation failure. An unreadable/corrupt commit along the way is
   treated as a dead end (best effort) rather than a hard failure, since a
   partially-packed or shallow history shouldn't make merge-base impossible
   for the reachable part of the graph. */
static int collect_ancestors(const char *git_dir, const unsigned char start[SG_SHA1_RAW_LEN],
                             id_set *out)
{
    size_t qi = 0;

    memset(out, 0, sizeof(*out));
    if (id_set_add(out, start) < 0)
        return -1;

    while (qi < out->count) {
        unsigned char cur[SG_SHA1_RAW_LEN];
        sg_obj_type type;
        unsigned char *content;
        size_t content_len;
        sg_commit commit;
        size_t i;

        memcpy(cur, out->ids[qi], SG_SHA1_RAW_LEN); /* out->ids may realloc below */
        qi++;

        if (sg_object_read(git_dir, cur, &type, &content, &content_len) != 0 || type != SG_OBJ_COMMIT)
            continue;
        if (sg_commit_parse(content, content_len, &commit) != 0) {
            free(content);
            continue;
        }
        free(content);

        for (i = 0; i < commit.parent_count; i++) {
            if (id_set_add(out, commit.parents[i]) < 0) {
                sg_commit_free(&commit);
                id_set_free(out);
                return -1;
            }
        }
        sg_commit_free(&commit);
    }
    return 0;
}

/* Returns 1 if candidate is an ancestor of (or equal to) other, 0 if not,
   -1 on allocation failure. */
static int is_ancestor(const char *git_dir, const unsigned char candidate[SG_SHA1_RAW_LEN],
                       const unsigned char other[SG_SHA1_RAW_LEN])
{
    id_set anc;
    int result;

    if (collect_ancestors(git_dir, other, &anc) != 0)
        return -1;
    result = id_set_contains(&anc, candidate);
    id_set_free(&anc);
    return result;
}

int sg_merge_base(const char *git_dir, const unsigned char a[SG_SHA1_RAW_LEN],
                  const unsigned char b[SG_SHA1_RAW_LEN], unsigned char out[SG_SHA1_RAW_LEN])
{
    id_set anc_a, anc_b;
    id_set candidates = {0};
    int *dominated = NULL;
    size_t i, j;
    size_t remaining;
    int rc;

    if (collect_ancestors(git_dir, a, &anc_a) != 0)
        return -1;
    if (collect_ancestors(git_dir, b, &anc_b) != 0) {
        id_set_free(&anc_a);
        return -1;
    }

    for (i = 0; i < anc_a.count; i++) {
        if (id_set_contains(&anc_b, anc_a.ids[i])) {
            if (id_set_add(&candidates, anc_a.ids[i]) < 0) {
                id_set_free(&anc_a);
                id_set_free(&anc_b);
                id_set_free(&candidates);
                return -1;
            }
        }
    }
    id_set_free(&anc_a);
    id_set_free(&anc_b);

    if (candidates.count == 0) {
        id_set_free(&candidates);
        return -1;
    }

    /* Reduce to the best (most-derived) candidates: drop any candidate that
       is itself an ancestor of another candidate -- it isn't "closest". */
    dominated = calloc(candidates.count, sizeof(*dominated));
    if (dominated == NULL) {
        id_set_free(&candidates);
        return -1;
    }

    remaining = candidates.count;
    for (i = 0; i < candidates.count; i++) {
        for (j = 0; j < candidates.count; j++) {
            int anc_result;

            if (i == j)
                continue;
            anc_result = is_ancestor(git_dir, candidates.ids[i], candidates.ids[j]);
            if (anc_result < 0) {
                free(dominated);
                id_set_free(&candidates);
                return -1;
            }
            if (anc_result == 1) {
                dominated[i] = 1;
                remaining--;
                break;
            }
        }
    }

    if (remaining == 1) {
        for (i = 0; i < candidates.count; i++) {
            if (!dominated[i]) {
                memcpy(out, candidates.ids[i], SG_SHA1_RAW_LEN);
                break;
            }
        }
        rc = 0;
    } else if (remaining == 0) {
        rc = -1; /* defensive: shouldn't happen given candidates.count > 0 */
    } else {
        rc = -2; /* criss-cross: multiple independent best common ancestors */
    }

    free(dominated);
    id_set_free(&candidates);
    return rc;
}

/* ==================== three-way content merge (diff3-lite) ==================== */

typedef struct {
    unsigned char *buf;
    size_t len;
    size_t cap;
} bytebuf;

static int bytebuf_append(bytebuf *b, const void *data, size_t len)
{
    if (b->len + len > b->cap) {
        size_t new_cap = b->cap == 0 ? 256 : b->cap * 2;
        unsigned char *grown;

        while (new_cap < b->len + len)
            new_cap *= 2;
        grown = realloc(b->buf, new_cap);
        if (grown == NULL)
            return -1;
        b->buf = grown;
        b->cap = new_cap;
    }
    if (len > 0)
        memcpy(b->buf + b->len, data, len);
    b->len += len;
    return 0;
}

static int bytebuf_append_str(bytebuf *b, const char *s)
{
    return bytebuf_append(b, s, strlen(s));
}

static int bytebuf_append_line(bytebuf *b, sg_diff_line line)
{
    if (bytebuf_append(b, line.ptr, line.len) != 0)
        return -1;
    if (line.has_nl && bytebuf_append(b, "\n", 1) != 0)
        return -1;
    return 0;
}

/* Terminates whatever was written last, so the next thing appended starts on
   a line of its own. A line with has_nl == 0 is by construction the LAST line
   of its own file, but the merged output interleaves three files, so that
   line can still be followed by another side's lines or by a conflict marker
   -- and then it has to grow the newline it never had. Measured against real
   git: with ours ending "base14" (no trailing newline) inside a conflict,
   git writes "base14\n=======", sg used to write "base14=======", inventing a
   line that appears in none of the three inputs. git does this in xdl_merge
   the same way, by passing add_nl to xdl_recs_copy when it copies a side into
   a conflict region.

   Called before every append that follows content, never after the last one,
   so a file that legitimately ends without a newline still ends without one. */
static int bytebuf_ensure_nl(bytebuf *b)
{
    if (b->len > 0 && b->buf[b->len - 1] != '\n')
        return bytebuf_append(b, "\n", 1);
    return 0;
}

static int bytebuf_append_lines(bytebuf *b, const sg_diff_line *lines, size_t start, size_t end)
{
    size_t k;

    for (k = start; k < end; k++) {
        if (bytebuf_append_line(b, lines[k]) != 0)
            return -1;
    }
    return 0;
}

static int content_has_nul(const unsigned char *data, size_t len)
{
    return len > 0 && memchr(data, '\0', len) != NULL;
}

/* Compares two spans line for line, has_nl INCLUDED (Phase 41). Before the
   Myers swap this used the has_nl-blind sg_diff_lines_equal, and that single
   choice was the dominant source of merge mismatch against real git: with
   base "x\ny\nz\n" and ours "x\ny\nz" (the only edit being the removal of
   the trailing newline), ours_eq_base came out TRUE, so the span was resolved
   as "ours changed nothing", theirs was taken, and the user's edit was
   silently discarded while the merge reported success. Measured against git,
   which conflicts there. The alignment above is has_nl-aware now, so leaving
   this one blind would keep the same span misclassified even after the two
   sides stopped being matched. */
static int segment_equal(const sg_diff_line *a, size_t as, size_t ae, const sg_diff_line *b,
                         size_t bs, size_t be)
{
    size_t na = ae - as;
    size_t nb = be - bs;
    size_t k;

    if (na != nb)
        return 0;
    for (k = 0; k < na; k++) {
        if (!sg_diff_lines_equal_exact(a[as + k], b[bs + k]))
            return 0;
    }
    return 1;
}

/* Aligns a against b with sg_diff_build_script (git's Myers algorithm, the
   same aligner `sg diff`'s patch bodies have used since Phase 35) and returns
   a match array of size na: match[i] = the b-index a[i] was aligned to, or -1
   if a[i] is inside a changed run. Returns NULL on allocation failure.

   This is the adapter Phase 41 exists to add: sg_diff_build_script speaks
   sg_diff_group (a_off/a_len paired with b_off/b_len), while the sync-point
   pass below needs the per-line match array the old LCS backtrack produced.
   The conversion is a single walk because outside the groups the two sides
   run in lockstep -- that is exactly what "between two consecutive groups,
   a[..]==b[..] line for line" in sg_diff_build_script's contract means.

   The lockstep walk is written to stop on EITHER side's bound rather than
   just a's, so a group list that ever violated that contract would leave
   lines unmatched (-1, "not aligned") instead of writing a b-index past nb
   into match[] -- the failure direction has to be "no sync point here",
   never an out-of-range index that the sync-point pass would then use to
   subscript ours_lines/theirs_lines. */
static long *script_matches(const sg_diff_line *a, size_t na, const sg_diff_line *b, size_t nb)
{
    sg_diff_script *script = sg_diff_build_script(a, na, b, nb, 0, SG_DIFF_ALGO_HISTOGRAM);
    long *match;
    size_t i, ai = 0, bi = 0;

    if (script == NULL)
        return NULL;
    match = malloc((na > 0 ? na : 1) * sizeof(*match));
    if (match == NULL) {
        sg_diff_script_free(script);
        return NULL;
    }
    for (i = 0; i < na; i++)
        match[i] = -1;

    for (i = 0; i < script->count; i++) {
        const sg_diff_group *g = &script->groups[i];

        while (ai < g->a_off && bi < g->b_off) {
            match[ai] = (long)bi;
            ai++;
            bi++;
        }
        ai = g->a_off + g->a_len;
        bi = g->b_off + g->b_len;
    }
    while (ai < na && bi < nb) {
        match[ai] = (long)bi;
        ai++;
        bi++;
    }

    sg_diff_script_free(script);
    return match;
}

/* ---- the merged output as a region list (Phase 41) ----

   git's xdl_merge does not write bytes as it classifies. It builds a list of
   regions and then post-processes that list TWICE -- xdl_refine_conflict,
   then xdl_simplify_non_conflicts -- and both passes need to see a region's
   neighbours. sg's sync-point pass used to append straight into the output
   buffer, which made both passes inexpressible; this list is what makes them
   expressible, and both are implemented below.

   Every region carries BOTH sides' line ranges, not just the text that will
   be printed: merging two conflicts across a gap has to reproduce the gap
   inside each side of the combined conflict, in that side's own words, and
   the gap is not necessarily the same text on the two sides. */
typedef enum {
    /* Identical on both sides. Does NOT block merging two conflicts across
       it -- git keeps such lines out of its changes list entirely, so its
       simplify pass never sees them as an obstacle, only as distance.

       "Identical on both sides" is the whole test, and it is NOT the same
       as "equal to base" (Phase 50): a span BOTH sides changed the same way
       is also SAME. git records no changes-list entry for one of those
       either -- there is nothing left to resolve -- so it does not block,
       and it contributes its OURS-side length as distance like any other
       SAME region. Two measured rows that a "SAME means untouched" reading
       gets wrong, with a gap of one line either side of it: a line deleted
       by both sides (git merges the two conflicts; that span is 0 ours
       lines long, so a 4-base-line gap still fits under the limit) and a
       line edited identically by both sides (git merges at a 3-line gap and
       splits at 4, because that span is 1 ours line long). refine_conflicts
       has always relied on this reading -- the common text it hoists out of
       a conflict is SAME while differing from base. */
    REGION_SAME,
    /* Exactly ONE side changed and its version won (git's mode != 0). This
       DOES block merging two conflicts across it, however short it is --
       measured against git 2.55.0, see the gap table in Phase 41 of
       docs/DESIGN.md: with a one-line ours-only change inside a 3-line gap,
       git leaves two conflicts where a purely distance-based rule would
       have produced one. The emphasis on ONE is load-bearing; see
       REGION_SAME above for the both-sides case, which is the other half of
       the same rule and goes the other way. */
    REGION_RESOLVED,
    REGION_CONFLICT
} region_kind;

typedef struct {
    size_t os, oe; /* ours lines [os, oe) */
    size_t ts, te; /* theirs lines [ts, te) */
    region_kind kind;
    int take_theirs; /* SAME/RESOLVED: whose text gets printed */
} merge_region;

typedef struct {
    merge_region *v;
    size_t count, cap;
} region_list;

static void region_list_free(region_list *l)
{
    free(l->v);
    l->v = NULL;
    l->count = 0;
    l->cap = 0;
}

static int region_push(region_list *l, size_t os, size_t oe, size_t ts, size_t te,
                       region_kind kind, int take_theirs)
{
    merge_region *r;

    if (l->count == l->cap) {
        size_t cap = l->cap == 0 ? 16 : l->cap * 2;
        merge_region *grown = realloc(l->v, cap * sizeof(*grown));

        if (grown == NULL)
            return -1;
        l->v = grown;
        l->cap = cap;
    }
    r = &l->v[l->count++];
    r->os = os;
    r->oe = oe;
    r->ts = ts;
    r->te = te;
    r->kind = kind;
    r->take_theirs = take_theirs;
    return 0;
}

/* git's "zealous" conflict refinement (xdiff/xmerge.c's xdl_refine_conflict):
   diff the two conflicting sides against EACH OTHER and hoist whatever they
   agree on out of the conflict, leaving only the runs they actually disagree
   about between the markers.

   sg's three-way layer got by without this until Phase 41 because its
   alignment was has_nl-blind, which happened to place many of these agreed
   lines outside the conflict as sync points instead. Making the alignment
   exact is what exposed the gap. Measured, base "A\nB" (no trailing newline)
   against ours "A\nB\nC\n" and theirs "A\nB\nD\n": base's "B" no longer
   matches either side's "B\n", so the whole tail becomes one conflict, yet
   real git still prints "B" as ordinary context above the marker, because it
   refines. Without this pass sg printed "B" twice, once inside each side --
   which is what tests/test_merge_content.c's test_anchor_newline_not_glued
   caught, and it is the reason that test was written to look at bytes.

   Like git, a conflict with an empty side is not refined: there is nothing to
   agree on. The hoisted runs are taken from ours, which is sound only because
   the aligner matched them has_nl-aware, so the two sides are identical
   bytes. */
static int refine_conflicts(const region_list *in, region_list *out, const sg_diff_line *ours,
                            const sg_diff_line *theirs)
{
    size_t i;

    for (i = 0; i < in->count; i++) {
        const merge_region *r = &in->v[i];
        sg_diff_script *script;
        size_t j, ai, bi;
        int rc = -1;

        if (r->kind != REGION_CONFLICT || r->os == r->oe || r->ts == r->te) {
            if (region_push(out, r->os, r->oe, r->ts, r->te, r->kind, r->take_theirs) != 0)
                return -1;
            continue;
        }

        script = sg_diff_build_script(ours + r->os, r->oe - r->os, theirs + r->ts,
                                     r->te - r->ts, 0, SG_DIFF_ALGO_HISTOGRAM);
        if (script == NULL)
            return -1;

        ai = r->os;
        bi = r->ts;
        for (j = 0; j < script->count; j++) {
            const sg_diff_group *g = &script->groups[j];
            size_t ca = r->os + g->a_off, cb = r->ts + g->b_off;

            if (ca > ai && region_push(out, ai, ca, bi, cb, REGION_SAME, 0) != 0)
                goto group_done;
            if (region_push(out, ca, ca + g->a_len, cb, cb + g->b_len, REGION_CONFLICT, 0) != 0)
                goto group_done;
            ai = ca + g->a_len;
            bi = cb + g->b_len;
        }
        if (ai < r->oe && region_push(out, ai, r->oe, bi, r->te, REGION_SAME, 0) != 0)
            goto group_done;
        rc = 0;

group_done:
        sg_diff_script_free(script);
        if (rc != 0)
            return -1;
    }
    return 0;
}

/* Two conflicts separated by at most this many identical lines are printed as
   one conflict. git's own constant (xdl_simplify_non_conflicts' `end - begin
   > 3`), re-measured here rather than recalled: with a plain gap of 0-3 lines
   real git prints one conflict block, at 4 it prints two. */
#define SG_MERGE_CONFLICT_GAP 3

/* git's xdl_simplify_non_conflicts. Runs AFTER refinement, on purpose and in
   that order: refinement splits conflicts apart, and this pass then decides
   which of the pieces are too close together to be worth separating. Doing it
   the other way round would let a gap that refinement is about to create
   escape the rule.

   REGION_RESOLVED blocks a merge no matter how short the gap is, which is why
   the distance is not the only thing tracked -- see region_kind's comment for
   the measurement. */
static void simplify_conflicts(region_list *l)
{
    size_t i;

    for (i = 0; i < l->count; i++) {
        size_t j, gap = 0, merge_to = i;

        if (l->v[i].kind != REGION_CONFLICT)
            continue;
        for (j = i + 1; j < l->count; j++) {
            if (l->v[j].kind == REGION_CONFLICT) {
                merge_to = j;
                gap = 0;
                continue;
            }
            if (l->v[j].kind != REGION_SAME)
                break;
            gap += l->v[j].oe - l->v[j].os;
            if (gap > SG_MERGE_CONFLICT_GAP)
                break;
        }
        if (merge_to > i) {
            l->v[i].oe = l->v[merge_to].oe;
            l->v[i].te = l->v[merge_to].te;
            memmove(&l->v[i + 1], &l->v[merge_to + 1],
                   (l->count - merge_to - 1) * sizeof(*l->v));
            l->count -= merge_to - i;
        }
    }
}

/* Appends `count` copies of `ch` (7 normally, 8 for the two widened shapes
   Phase 49 introduces -- see sg_merge_content's header comment). */
static int bytebuf_append_marker(bytebuf *out, char ch, int count)
{
    char buf[16];

    if (count < 1)
        count = 1;
    if ((size_t)count >= sizeof(buf))
        count = (int)sizeof(buf) - 1;
    memset(buf, ch, (size_t)count);
    return bytebuf_append(out, buf, (size_t)count);
}

static int emit_regions(bytebuf *out, const region_list *l, const sg_diff_line *ours,
                        const sg_diff_line *theirs, const char *ours_label,
                        const char *theirs_label, int marker_size, int *conflict_out)
{
    size_t i;

    for (i = 0; i < l->count; i++) {
        const merge_region *r = &l->v[i];

        if (r->kind != REGION_CONFLICT) {
            const sg_diff_line *src = r->take_theirs ? theirs : ours;
            size_t from = r->take_theirs ? r->ts : r->os;
            size_t to = r->take_theirs ? r->te : r->oe;

            /* An empty region must not even terminate the previous line.
               Zero-length regions are ordinary here -- the sync-point pass
               emits one for every empty span, including the one after the
               final anchor -- and calling bytebuf_ensure_nl before writing
               nothing would give a file that legitimately ends without a
               newline one it never had. Measured: merging three identical
               copies of "x\ny" returned "x\ny\n", 4 bytes for a 3-byte
               input, on a merge that changed nothing at all. */
            if (from == to)
                continue;
            if (bytebuf_ensure_nl(out) != 0 || bytebuf_append_lines(out, src, from, to) != 0)
                return -1;
            continue;
        }
        if (bytebuf_ensure_nl(out) != 0)
            return -1;
        *conflict_out = 1;
        if (bytebuf_append_marker(out, '<', marker_size) != 0 ||
           bytebuf_append_str(out, " ") != 0 || bytebuf_append_str(out, ours_label) != 0 ||
           bytebuf_append_str(out, "\n") != 0 ||
           bytebuf_append_lines(out, ours, r->os, r->oe) != 0 || bytebuf_ensure_nl(out) != 0 ||
           bytebuf_append_marker(out, '=', marker_size) != 0 ||
           bytebuf_append_str(out, "\n") != 0 ||
           bytebuf_append_lines(out, theirs, r->ts, r->te) != 0 || bytebuf_ensure_nl(out) != 0 ||
           bytebuf_append_marker(out, '>', marker_size) != 0 ||
           bytebuf_append_str(out, " ") != 0 || bytebuf_append_str(out, theirs_label) != 0 ||
           bytebuf_append_str(out, "\n") != 0)
            return -1;
    }
    return 0;
}

typedef struct {
    long base_idx, ours_idx, theirs_idx;
} sync_point;

int sg_merge_content(const unsigned char *base, size_t base_len, const unsigned char *ours,
                     size_t ours_len, const unsigned char *theirs, size_t theirs_len,
                     const char *ours_label, const char *theirs_label, int marker_size,
                     unsigned char **out, size_t *out_len)
{
    size_t nbase = 0, nours = 0, ntheirs = 0;
    sg_diff_line *base_lines = NULL;
    sg_diff_line *ours_lines = NULL;
    sg_diff_line *theirs_lines = NULL;
    long *bo_match = NULL;
    long *bt_match = NULL;
    sync_point *syncs = NULL;
    region_list raw = {0};
    region_list regions = {0};
    size_t sync_cap;
    size_t sync_count = 0;
    bytebuf outbuf = {0};
    int conflict = 0;
    int rc = -1;
    size_t k, idx;

    *out = NULL;
    *out_len = 0;

    if (content_has_nul(base, base_len) || content_has_nul(ours, ours_len) ||
       content_has_nul(theirs, theirs_len)) {
        unsigned char *copy = NULL;

        if (ours_len > 0) {
            copy = malloc(ours_len);
            if (copy == NULL)
                return -1;
            memcpy(copy, ours, ours_len);
        }
        *out = copy;
        *out_len = ours_len;
        return 1;
    }

    base_lines = sg_diff_split_lines(base, base_len, &nbase);
    ours_lines = sg_diff_split_lines(ours, ours_len, &nours);
    theirs_lines = sg_diff_split_lines(theirs, theirs_len, &ntheirs);
    if (base_lines == NULL || ours_lines == NULL || theirs_lines == NULL)
        goto done;

    bo_match = script_matches(base_lines, nbase, ours_lines, nours);
    bt_match = script_matches(base_lines, nbase, theirs_lines, ntheirs);
    if (bo_match == NULL || bt_match == NULL)
        goto done;

    /* sync points: base lines matched on both sides ("the same" per the
       spec), bracketed by sentinels for the start/end of the whole file. */
    sync_cap = nbase + 2;
    syncs = malloc(sync_cap * sizeof(*syncs));
    if (syncs == NULL)
        goto done;
    syncs[sync_count].base_idx = -1;
    syncs[sync_count].ours_idx = -1;
    syncs[sync_count].theirs_idx = -1;
    sync_count++;
    for (k = 0; k < nbase; k++) {
        if (bo_match[k] != -1 && bt_match[k] != -1) {
            syncs[sync_count].base_idx = (long)k;
            syncs[sync_count].ours_idx = bo_match[k];
            syncs[sync_count].theirs_idx = bt_match[k];
            sync_count++;
        }
    }
    syncs[sync_count].base_idx = (long)nbase;
    syncs[sync_count].ours_idx = (long)nours;
    syncs[sync_count].theirs_idx = (long)ntheirs;
    sync_count++;

    for (idx = 1; idx < sync_count; idx++) {
        sync_point prev = syncs[idx - 1];
        sync_point cur = syncs[idx];
        size_t bs = (size_t)(prev.base_idx + 1), be = (size_t)cur.base_idx;
        size_t os = (size_t)(prev.ours_idx + 1), oe = (size_t)cur.ours_idx;
        size_t ts = (size_t)(prev.theirs_idx + 1), te = (size_t)cur.theirs_idx;
        int ours_eq_base = segment_equal(base_lines, bs, be, ours_lines, os, oe);
        int theirs_eq_base = segment_equal(base_lines, bs, be, theirs_lines, ts, te);
        int ours_eq_theirs = segment_equal(ours_lines, os, oe, theirs_lines, ts, te);

        if (ours_eq_base && theirs_eq_base) {
            /* Neither side touched this span, so it is distance, not a
               change: SAME rather than RESOLVED, or it would block the
               simplify pass for no reason. */
            if (region_push(&raw, os, oe, ts, te, REGION_SAME, 0) != 0)
                goto done;
        } else if (ours_eq_base) {
            if (region_push(&raw, os, oe, ts, te, REGION_RESOLVED, 1) != 0)
                goto done;
        } else if (theirs_eq_base) {
            if (region_push(&raw, os, oe, ts, te, REGION_RESOLVED, 0) != 0)
                goto done;
        } else if (ours_eq_theirs) {
            /* BOTH sides made the same change. That is SAME, not RESOLVED:
               only ONE side changing blocks the simplify pass (Phase 50,
               measured -- see region_kind's comment). Emission is unaffected,
               since take_theirs 0 prints ours and ours equals theirs here. */
            if (region_push(&raw, os, oe, ts, te, REGION_SAME, 0) != 0)
                goto done;
        } else if (region_push(&raw, os, oe, ts, te, REGION_CONFLICT, 0) != 0) {
            goto done;
        }

        if (idx < sync_count - 1) {
            /* The anchor. Its has_nl can be trusted, and that is a property of
               the alignment, not an assumption (Phase 41): a sync point is a
               base line matched on BOTH sides, script_matches matches
               has_nl-aware, so all three lines agree on it. A line with
               has_nl == 0 is by construction the last line of its own file
               (sg_diff_split_lines only clears the flag when no '\n' was found
               before the end), so an anchor carrying has_nl == 0 is
               simultaneously the last line of base, ours and theirs.

               Until Phase 41 the alignment was has_nl-blind, so base's final
               newline-less line could match an identically spelled line in the
               MIDDLE of ours/theirs, and emitting base's has_nl verbatim then
               glued that line onto the next one. The repair for that was an
               explicit is_final_line test forcing has_nl to 1 everywhere else;
               it is unreachable now -- the case it repaired can no longer be
               produced -- so it is removed rather than kept as a comforting
               no-op. bytebuf_ensure_nl in emit_regions is a different guard
               with a different job: it terminates a line that legitimately
               lacks a newline when more output follows it. */
            if (region_push(&raw, (size_t)cur.ours_idx, (size_t)cur.ours_idx + 1,
                           (size_t)cur.theirs_idx, (size_t)cur.theirs_idx + 1, REGION_SAME,
                           0) != 0)
                goto done;
        }
    }

    if (refine_conflicts(&raw, &regions, ours_lines, theirs_lines) != 0)
        goto done;
    simplify_conflicts(&regions);
    if (emit_regions(&outbuf, &regions, ours_lines, theirs_lines, ours_label, theirs_label,
                    marker_size, &conflict) != 0)
        goto done;

    *out = outbuf.buf;
    *out_len = outbuf.len;
    rc = conflict;

done:
    if (rc < 0)
        free(outbuf.buf);
    region_list_free(&raw);
    region_list_free(&regions);
    free(syncs);
    free(bo_match);
    free(bt_match);
    free(base_lines);
    free(ours_lines);
    free(theirs_lines);
    return rc;
}

/* ==================== three-way tree merge ==================== */

typedef struct {
    const char *path;
    int base_present, ours_present, theirs_present;
    const sg_flat_entry *base_e, *ours_e, *theirs_e;
} triple;

static const char *min_path3(const char *a, const char *b, const char *c)
{
    const char *m = NULL;

    if (a != NULL)
        m = a;
    if (b != NULL && (m == NULL || strcmp(b, m) < 0))
        m = b;
    if (c != NULL && (m == NULL || strcmp(c, m) < 0))
        m = c;
    return m;
}

static int blob_eq(int a_present, const sg_flat_entry *a, int b_present, const sg_flat_entry *b)
{
    if (a_present != b_present)
        return 0;
    if (!a_present)
        return 1; /* both absent */
    return a->mode == b->mode && memcmp(a->sha1, b->sha1, SG_SHA1_RAW_LEN) == 0;
}

static void fill_conflict_sides(sg_merge_result_entry *e, const triple *t)
{
    e->base_present = t->base_present;
    e->ours_present = t->ours_present;
    e->theirs_present = t->theirs_present;
    if (t->base_present) {
        e->base_mode = t->base_e->mode;
        memcpy(e->base_sha1, t->base_e->sha1, SG_SHA1_RAW_LEN);
    }
    if (t->ours_present) {
        e->ours_mode = t->ours_e->mode;
        memcpy(e->ours_sha1, t->ours_e->sha1, SG_SHA1_RAW_LEN);
    }
    if (t->theirs_present) {
        e->theirs_mode = t->theirs_e->mode;
        memcpy(e->theirs_sha1, t->theirs_e->sha1, SG_SHA1_RAW_LEN);
    }
}

/* Reads the content a tree entry's blob id actually names, transparently
   reassembling it if id is a chunked-storage pointer (see sg/chunk.h) --
   using sg_object_read here instead would hand back the pointer's own
   ~500-byte text (magic/size/sha1/chunk-id-list) as if it were the file's
   real content, which is exactly the bug this function exists to avoid: a
   large file's real bytes would never even be looked at by the diff3/binary
   logic below, and a "conflict" would end up being two branches' pointer
   texts diffed against each other instead of the actual file. path is used
   only for sg_chunk_print_missing_error's message. Returns 0 on success, -1
   on any failure -- including a -2 ("real pointer, but the data is missing
   or corrupt") from sg_chunk_read_blob, which is deliberately turned into a
   hard failure here rather than falling back to raw pointer text or
   silently skipping the path: losing a chunked file's data must abort the
   whole merge, never be papered over. */
static int read_blob_chunk_aware(const char *git_dir, const unsigned char sha1[SG_SHA1_RAW_LEN],
                                 const char *path, unsigned char **content_out, size_t *len_out)
{
    sg_chunk_missing_info missing;
    int rc = sg_chunk_read_blob(git_dir, sha1, content_out, len_out, &missing);

    if (rc == -2) {
        sg_chunk_print_missing_error(path, &missing);
        return -1;
    }
    return rc;
}

/* Writes `len` bytes as a new blob object, chunk-aware per this repo's own
   configured threshold (same policy sg add and a clean multi-line merge
   already use) so a large rename-merge result -- clean or, since Phase 49,
   the marker-laden text of a NESTED conflict that itself has to become a
   real object -- isn't silently forced back into full-size, non-deduplicated
   storage. Returns 0 on success, -1 on failure. Does not free `bytes`. */
static int store_new_blob(const char *git_dir, const unsigned char *bytes, size_t len,
                          unsigned char out_sha1[SG_SHA1_RAW_LEN])
{
    int enabled = 0;
    size_t threshold = SG_CHUNK_DEFAULT_THRESHOLD;
    int chunked;

    sg_repo_read_chunk_config(git_dir, &enabled, &threshold);
    if (enabled)
        return sg_chunk_store_blob(git_dir, bytes, len, threshold, out_sha1, &chunked);
    return sg_loose_write(git_dir, SG_OBJ_BLOB, bytes, len, out_sha1);
}

/* The engine behind resolve_via_content_merge (the ordinary modify/modify
   and add/add conflict candidates), generalized to take explicit mode/sha1
   operands rather than a triple pointing into a flattened tree -- Phase 49's
   rename-aware paths need to feed it SYNTHETIC operands (a rename's landing
   content, possibly itself the freshly-written result of an inner merge)
   that have no sg_flat_entry of their own to point at.

   Runs sg_merge_content(base?, ours, theirs) and either writes a freshly
   merged clean blob (returning it via *out_mode/out_sha1, *out_conflict left
   at 0) or hands back the conflict bytes verbatim (*out_conflict_content,
   *out_conflict_content_len, *out_conflict set to 1) -- exactly
   resolve_via_content_merge's old two outcomes, just factored so both
   callers can reuse the chunk-aware reads, the sg_merge_content call, and
   the clean-path chunk-or-loose write. Returns 0 on success, -1 on error
   (I/O/allocation) -- on -1 nothing has been written and *out_conflict is
   unspecified. */
static int merge_blob_content(const char *git_dir, const char *path, int base_present,
                              unsigned int base_mode, const unsigned char *base_sha1,
                              unsigned int ours_mode, const unsigned char *ours_sha1,
                              unsigned int theirs_mode, const unsigned char *theirs_sha1,
                              const char *ours_label, const char *theirs_label, int marker_size,
                              int *out_conflict, unsigned int *out_mode,
                              unsigned char out_sha1[SG_SHA1_RAW_LEN],
                              unsigned char **out_conflict_content,
                              size_t *out_conflict_content_len)
{
    unsigned char *base_content = NULL;
    size_t base_len = 0;
    unsigned char *ours_content = NULL;
    size_t ours_len = 0;
    unsigned char *theirs_content = NULL;
    size_t theirs_len = 0;
    unsigned char *merged = NULL;
    size_t merged_len = 0;
    int rc;

    *out_conflict_content = NULL;
    *out_conflict_content_len = 0;

    if (base_present && read_blob_chunk_aware(git_dir, base_sha1, path, &base_content, &base_len) != 0)
        return -1;
    if (read_blob_chunk_aware(git_dir, ours_sha1, path, &ours_content, &ours_len) != 0) {
        free(base_content);
        return -1;
    }
    if (read_blob_chunk_aware(git_dir, theirs_sha1, path, &theirs_content, &theirs_len) != 0) {
        free(base_content);
        free(ours_content);
        return -1;
    }

    rc = sg_merge_content(base_content, base_len, ours_content, ours_len, theirs_content, theirs_len,
                          ours_label, theirs_label, marker_size, &merged, &merged_len);
    free(base_content);
    free(ours_content);
    free(theirs_content);
    if (rc < 0)
        return -1;

    if (rc == 0) {
        unsigned char new_sha1[SG_SHA1_RAW_LEN];

        if (store_new_blob(git_dir, merged, merged_len, new_sha1) != 0) {
            free(merged);
            return -1;
        }
        free(merged);
        *out_conflict = 0;
        memcpy(out_sha1, new_sha1, SG_SHA1_RAW_LEN);
        if (base_present) {
            if (ours_mode != base_mode)
                *out_mode = ours_mode;
            else if (theirs_mode != base_mode)
                *out_mode = theirs_mode;
            else
                *out_mode = base_mode;
        } else {
            *out_mode = ours_mode;
        }
        return 0;
    }

    *out_conflict = 1;
    *out_conflict_content = merged;
    *out_conflict_content_len = merged_len;
    return 0;
}

/* Handles the "all three present" (modify/modify) and "base absent, both
   added" (add/add) conflict candidates by calling sg_merge_content; a clean
   result (rc 0) turns out not to be a conflict after all and gets a freshly
   written blob. Always uses the ordinary 7-character marker width -- the
   ordinary union walk never produces one of Phase 49's two widened shapes. */
static int resolve_via_content_merge(const char *git_dir, const triple *t, const char *ours_label,
                                     const char *theirs_label, sg_merge_result_entry *out)
{
    int conflict = 0;
    int rc = merge_blob_content(
       git_dir, t->path, t->base_present, t->base_present ? t->base_e->mode : 0,
       t->base_present ? t->base_e->sha1 : NULL, t->ours_e->mode, t->ours_e->sha1,
       t->theirs_e->mode, t->theirs_e->sha1, ours_label, theirs_label, 7, &conflict, &out->mode,
       out->sha1, &out->conflict_content, &out->conflict_content_len);

    if (rc != 0)
        return -1;
    if (!conflict) {
        out->conflict = 0;
        out->deleted = 0;
    }
    return 0;
}

static int process_path(const char *git_dir, const triple *t, const char *ours_label,
                        const char *theirs_label, sg_merge_result_entry *out)
{
    int eq_ob = blob_eq(t->ours_present, t->ours_e, t->base_present, t->base_e);
    int eq_tb = blob_eq(t->theirs_present, t->theirs_e, t->base_present, t->base_e);
    int eq_ot = blob_eq(t->ours_present, t->ours_e, t->theirs_present, t->theirs_e);

    memset(out, 0, sizeof(*out));
    out->path = strdup(t->path);
    if (out->path == NULL)
        return -1;

    /* Filled unconditionally, not just on the conflict path below: Phase 20
       (stash apply/pop's index touched-check) needs to know each CLEAN
       entry's ours side too, to tell "the merge outcome happens to equal
       what's already at ours" apart from "the merge actually changed this
       path" -- see sg_merge_entry_touches_ours's header comment in
       merge.h. Harmless for every existing reader: grep confirms
       ours_present/base_present/theirs_present are read only inside this
       file, and only from the two call sites gated on e->conflict. */
    fill_conflict_sides(out, t);

    if (eq_ob && eq_tb) {
        out->deleted = !t->base_present;
        if (t->base_present) {
            out->mode = t->base_e->mode;
            memcpy(out->sha1, t->base_e->sha1, SG_SHA1_RAW_LEN);
        }
        return 0;
    }
    if (eq_ob) { /* ours unchanged from base, theirs changed: take theirs */
        out->deleted = !t->theirs_present;
        if (t->theirs_present) {
            out->mode = t->theirs_e->mode;
            memcpy(out->sha1, t->theirs_e->sha1, SG_SHA1_RAW_LEN);
        }
        return 0;
    }
    if (eq_tb) { /* theirs unchanged from base, ours changed: take ours */
        out->deleted = !t->ours_present;
        if (t->ours_present) {
            out->mode = t->ours_e->mode;
            memcpy(out->sha1, t->ours_e->sha1, SG_SHA1_RAW_LEN);
        }
        return 0;
    }
    if (eq_ot) { /* both sides agree, whatever base said */
        out->deleted = !t->ours_present;
        if (t->ours_present) {
            out->mode = t->ours_e->mode;
            memcpy(out->sha1, t->ours_e->sha1, SG_SHA1_RAW_LEN);
        }
        return 0;
    }

    /* genuine conflict candidate -- base/ours/theirs sides already filled
       above (unconditionally, for every entry). */
    out->conflict = 1;

    if (t->ours_present && t->theirs_present) {
        /* modify/modify (base present) or add/add (base absent) */
        if (resolve_via_content_merge(git_dir, t, ours_label, theirs_label, out) != 0) {
            free(out->path);
            return -1;
        }
        return 0;
    }

    /* modify/delete: base present, exactly one of ours/theirs still has it.
       The surviving side's real content (chunk-aware, not its raw pointer
       text if it's a chunked file) is what gets left in the working tree
       for the user to resolve. */
    {
        const sg_flat_entry *present_e = t->ours_present ? t->ours_e : t->theirs_e;

        if (read_blob_chunk_aware(git_dir, present_e->sha1, t->path, &out->conflict_content,
                                  &out->conflict_content_len) != 0) {
            free(out->path);
            return -1;
        }
    }
    return 0;
}

/* ==================== Phase 49: rename-aware tree merge ====================

   The overall shape (docs/DESIGN.md Phase 49 has the measured tables this
   ports): a base path that is a rename SOURCE on either side, and every
   ours/theirs path that is a rename DESTINATION, is pulled out of the
   ordinary union walk above and handled here instead; the ordinary walk is
   otherwise unchanged (rename_score == 0 skips all of this and reproduces
   pre-Phase-49 behaviour exactly, since neither list below is ever built).

   Two base paths independently renamed to the SAME destination by opposite
   sides (rename/rename "2 to 1") is why this cannot just process one base
   path at a time and emit immediately: the destination has to be seen as a
   single collision point that MAY be fed by two entirely different source
   paths, one from each side. So this is a two-pass "contribution map" keyed
   by destination path: pass one classifies every rename source and records
   what each side WANTS to land at its destination (an inner three-way merge
   against the other side's unmoved copy, or a raw verbatim blob when the
   other side deleted the source outright); pass two looks at each
   destination once, decides whether only one side showed up (plain
   rename-vs-KEPT or rename-vs-DELETE) or both did (an add/add collision,
   with or without an inner merge nested inside one or both sides), and
   emits exactly one sg_merge_result_entry (or, for rename/rename-1to2,
   three, at three different paths) per destination. */

typedef struct {
    const char *base_path; /* borrowed, owned by the sg_diff_list this came from */
    const char *dst_path;  /* likewise */
} rename_pair;

typedef struct {
    rename_pair *v;
    size_t count;
} rename_map;

/* Builds base_path -> new_path out of a base-vs-side tree diff: renames only
   (old_path != NULL && !is_copy -- sg_merge_trees never asks for copy
   detection, so is_copy is always 0 here; checked anyway so this reads
   correctly if that default is ever revisited). *list_out is kept alive
   alongside *map_out (whose pairs borrow strings out of it) and must be
   freed with sg_diff_list_free once the caller is done with the map.
   Returns 0 on success, -1 on error (already reported to stderr for a -2
   from sg_diff_trees, matching how the plain sg_tree_flatten calls above
   report the same failure). */
static int build_rename_map(const char *git_dir, const unsigned char base_tree[SG_SHA1_RAW_LEN],
                            const unsigned char side_tree[SG_SHA1_RAW_LEN], int rename_score,
                            sg_diff_list *list_out, rename_map *map_out)
{
    char bad_path[SG_PATH_MAX];
    int rc;
    size_t i;

    memset(list_out, 0, sizeof(*list_out));
    memset(map_out, 0, sizeof(*map_out));

    rc = sg_diff_trees(git_dir, base_tree, side_tree, list_out, bad_path);
    if (rc == -2) {
        fprintf(stderr, "sg: path %s is invalid, refusing to flatten this tree into file paths\n",
               sg_quote_path_delimited(bad_path));
        return -1;
    }
    if (rc != 0)
        return -1;

    /* sg_diff_detect_renames' pairing only ever reads through
       sg_diff_side_read for scoring, and that function's SG_DIFF_SIDE_BLOB
       branch (verified by reading src/workdir/diff.c) goes straight to
       sg_chunk_read_blob without ever touching repo_root -- confirmed here
       rather than assumed, per Phase 49 sec 2. sg_diff_trees only ever
       produces BLOB or ABSENT sides, never WORKDIR, so repo_root can never
       actually be dereferenced on this call path. Passed as "" rather than
       NULL anyway: a future change that did need it would get a path-join
       failure instead of a NULL dereference. detect_copies is 0 -- a copy
       still has its source present at its old path too, which is not a
       rename Phase 49 is in scope for (see docs/DESIGN.md). */
    if (sg_diff_detect_renames(git_dir, "", list_out, rename_score, 0) != 0) {
        sg_diff_list_free(list_out);
        memset(list_out, 0, sizeof(*list_out));
        return -1;
    }

    map_out->v = calloc(list_out->count > 0 ? list_out->count : 1, sizeof(*map_out->v));
    if (map_out->v == NULL) {
        sg_diff_list_free(list_out);
        memset(list_out, 0, sizeof(*list_out));
        return -1;
    }
    for (i = 0; i < list_out->count; i++) {
        const sg_diff_entry *e = &list_out->entries[i];

        if (e->old_path != NULL && !e->is_copy) {
            map_out->v[map_out->count].base_path = e->old_path;
            map_out->v[map_out->count].dst_path = e->path;
            map_out->count++;
        }
    }
    return 0;
}

static void rename_map_free(rename_map *m)
{
    free(m->v);
    m->v = NULL;
    m->count = 0;
}

static const char *rename_map_dst(const rename_map *m, const char *base_path)
{
    size_t i;

    for (i = 0; i < m->count; i++) {
        if (strcmp(m->v[i].base_path, base_path) == 0)
            return m->v[i].dst_path;
    }
    return NULL;
}

/* Reverse lookup: is `dst` some p's destination, and if so which p. Used to
   tell a genuine rename destination apart from an unrelated addition that
   happens to share its name (Phase 49 sec 3.5's "not itself a rename
   destination" clause). */
static const char *rename_map_src(const rename_map *m, const char *dst)
{
    size_t i;

    for (i = 0; i < m->count; i++) {
        if (strcmp(m->v[i].dst_path, dst) == 0)
            return m->v[i].base_path;
    }
    return NULL;
}

static const sg_flat_entry *flat_find(const sg_flat_list *list, const char *path)
{
    size_t lo = 0, hi = list->count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(list->entries[mid].path, path);

        if (cmp == 0)
            return &list->entries[mid];
        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

/* True iff `path` takes part in rename handling at all -- either as a base
   source on either side, or as a destination on either side -- and must
   therefore be skipped by the ordinary union walk (which advances its own
   list pointers first and only then checks this, exactly like every other
   per-path decision it already makes). */
static int path_is_rename_consumed(const rename_map *ro, const rename_map *rt, const char *path)
{
    return rename_map_dst(ro, path) != NULL || rename_map_dst(rt, path) != NULL ||
          rename_map_src(ro, path) != NULL || rename_map_src(rt, path) != NULL;
}

/* Appends a fully-formed entry (already owning path/conflict_content) to
   *out, growing *cap exactly like sg_merge_trees' own union-walk loop does.
   On allocation failure the entry's own memory is freed (matching that
   loop's existing convention) and -1 is returned. */
static int append_result_entry(sg_merge_result *out, size_t *cap, sg_merge_result_entry *entry)
{
    if (out->count == *cap) {
        size_t new_cap = *cap == 0 ? 16 : *cap * 2;
        sg_merge_result_entry *grown = realloc(out->entries, new_cap * sizeof(*grown));

        if (grown == NULL) {
            free(entry->path);
            free(entry->conflict_content);
            return -1;
        }
        out->entries = grown;
        *cap = new_cap;
    }
    out->entries[out->count] = *entry;
    out->count++;
    return 0;
}

static int result_entry_path_cmp(const void *a, const void *b)
{
    return strcmp(((const sg_merge_result_entry *)a)->path, ((const sg_merge_result_entry *)b)->path);
}

/* "<label>:<path>" (Phase 49 sec 4), malloc'd, caller frees. Only ever
   called when the two sides' own paths differ -- the caller decides that,
   this just formats. */
static char *compose_conflict_label(const char *label, const char *path)
{
    size_t len = strlen(label) + 1 + strlen(path) + 1;
    char *out = malloc(len);

    if (out == NULL)
        return NULL;
    snprintf(out, len, "%s:%s", label, path);
    return out;
}

/* One side's contribution to a destination path, built while classifying
   rename sources (pass one) and consumed while resolving destinations
   (pass two). */
typedef enum {
    LANDING_NONE = 0,
    /* The other side still has the source path unchanged (relative to this
       classification -- its OWN content may have been edited, "KEPT" only
       means "not renamed away"): landing needs an inner three-way merge of
       base[src]/this_side[dst]/other_side[src]. */
    LANDING_KEPT,
    /* The other side deleted the source outright: landing is this side's
       own dst content verbatim, no merge (Phase 49 sec 3.2). */
    LANDING_DELETED,
    /* Not from any rename at all -- an ordinary addition that happens to
       land on the same path as the OTHER side's rename destination (Phase
       49 sec 3.5). Landing is this side's own dst content verbatim. */
    LANDING_PLAIN
} landing_kind;

typedef struct {
    landing_kind kind;
    const char *src_path; /* valid iff kind == LANDING_KEPT || LANDING_DELETED */
} landing;

typedef struct {
    const char *dst;
    landing ours;
    landing theirs;
} contribution;

typedef struct {
    contribution *v;
    size_t count, cap;
} contribution_list;

static contribution *contribution_find_or_add(contribution_list *cl, const char *dst)
{
    size_t i;
    contribution *grown;

    for (i = 0; i < cl->count; i++) {
        if (strcmp(cl->v[i].dst, dst) == 0)
            return &cl->v[i];
    }
    if (cl->count == cl->cap) {
        size_t new_cap = cl->cap == 0 ? 8 : cl->cap * 2;

        grown = realloc(cl->v, new_cap * sizeof(*grown));
        if (grown == NULL)
            return NULL;
        cl->v = grown;
        cl->cap = new_cap;
    }
    cl->v[cl->count].dst = dst;
    cl->v[cl->count].ours.kind = LANDING_NONE;
    cl->v[cl->count].theirs.kind = LANDING_NONE;
    return &cl->v[cl->count++];
}

/* Runs the inner three-way content merge for a KEPT landing and turns it
   into a real object either way: store_new_blob for a clean result (same as
   an ordinary content merge), and -- because this landing may end up
   nested inside an outer add/add collision, where "the inner conflicted
   text is what gets hashed into stage 2" (Phase 49 sec 3.5) -- ALSO for a
   conflicting one, which an ordinary standalone conflict never does. The
   caller decides marker_size and whether nesting is actually happening (a
   non-colliding, standalone caller still routes through here so the
   KEPT-vs-DELETED classification stays in one place, and eagerly writing a
   throwaway object on the non-colliding conflict path is a correctness
   no-op, not a bug -- the object is unreferenced but the store already
   tolerates that same shape from every other conflict-turned-clean path).
   Returns 0 on success (*out_mode/out_sha1 filled, *out_conflict tells
   whether the object holds marker text), -1 on error. */
/* is_ours says which side the LANDING belongs to; ours_label/theirs_label
   are already in ours/theirs order. Both are needed because the operands
   must be swapped in step with the labels -- see the long comment at
   emit_standalone_landing's merge_blob_content call for what taking
   "side" as "ours" unconditionally does to the marker body. */
static int compute_kept_landing(const char *git_dir, const sg_flat_entry *base_e,
                                unsigned int side_mode, const unsigned char *side_sha1,
                                const sg_flat_entry *other_e, int is_ours, const char *ours_label,
                                const char *theirs_label, int marker_size, int *out_conflict,
                                unsigned int *out_mode, unsigned char out_sha1[SG_SHA1_RAW_LEN])
{
    unsigned char *conflict_content = NULL;
    size_t conflict_content_len = 0;
    int conflict = 0;
    int rc = merge_blob_content(git_dir, base_e->path, 1, base_e->mode, base_e->sha1,
                                is_ours ? side_mode : other_e->mode,
                                is_ours ? side_sha1 : other_e->sha1,
                                is_ours ? other_e->mode : side_mode,
                                is_ours ? other_e->sha1 : side_sha1, ours_label, theirs_label,
                                marker_size, &conflict, out_mode, out_sha1, &conflict_content,
                                &conflict_content_len);

    if (rc != 0)
        return -1;
    *out_conflict = conflict;
    if (conflict) {
        rc = store_new_blob(git_dir, conflict_content, conflict_content_len, out_sha1);
        free(conflict_content);
        if (rc != 0)
            return -1;
        *out_mode = side_mode;
    }
    return 0;
}

/* Emits the standalone (non-colliding) result of one side's rename landing:
   KEPT is exactly Phase 49 sec 3.1 (its own inline merge_blob_content at
   marker size 7, then fills either a clean entry or the
   stages-1/2/3-all-at-dst conflict shape). It deliberately does NOT reuse
   compute_kept_landing: that one is the COLLISION path's helper and merges
   at marker size 8, which is the wrong width for a landing nothing collides
   with -- resolve_landing_for_collision is its only caller;
   DELETED is exactly sec 3.2 (always a conflict, never a merge -- see the
   big comment block below sg_merge_trees' rename section intro for why the
   ordinary blob_eq shortcuts are unsafe here and this cannot reuse
   process_path). is_ours selects which of base/ours/theirs role this side
   plays. Returns 0 on success (an entry has been appended to *out or
   allocation failed -- see append_result_entry), -1 on error. */
static int emit_standalone_landing(const char *git_dir, const sg_flat_list *base_flat,
                                   const sg_flat_list *ours_flat, const sg_flat_list *theirs_flat,
                                   const char *dst, int is_ours, const landing *l,
                                   const char *ours_label, const char *theirs_label,
                                   sg_merge_result *out, size_t *cap)
{
    const sg_flat_entry *base_e = flat_find(base_flat, l->src_path);
    const sg_flat_entry *side_e = flat_find(is_ours ? ours_flat : theirs_flat, dst);
    sg_merge_result_entry entry;

    memset(&entry, 0, sizeof(entry));
    entry.path = strdup(dst);
    if (entry.path == NULL || base_e == NULL || side_e == NULL) {
        free(entry.path);
        return -1;
    }
    entry.conflict = 1;
    entry.base_present = 1;
    entry.base_mode = base_e->mode;
    memcpy(entry.base_sha1, base_e->sha1, SG_SHA1_RAW_LEN);

    if (l->kind == LANDING_DELETED) {
        /* Phase 49 sec 3.2: always a conflict, ours'/theirs' own content
           verbatim, no marker text at all. */
        if (is_ours) {
            entry.ours_present = 1;
            entry.ours_mode = side_e->mode;
            memcpy(entry.ours_sha1, side_e->sha1, SG_SHA1_RAW_LEN);
        } else {
            entry.theirs_present = 1;
            entry.theirs_mode = side_e->mode;
            memcpy(entry.theirs_sha1, side_e->sha1, SG_SHA1_RAW_LEN);
        }
        if (read_blob_chunk_aware(git_dir, side_e->sha1, dst, &entry.conflict_content,
                                  &entry.conflict_content_len) != 0) {
            free(entry.path);
            return -1;
        }
        return append_result_entry(out, cap, &entry);
    }

    /* LANDING_KEPT: Phase 49 sec 3.1. The other side's own path (l->src_path)
       still differs from dst, so the marker labels get the ":path" suffix
       (sec 4) regardless of clean/conflict -- discarded silently if clean. */
    {
        const sg_flat_entry *other_e = flat_find(is_ours ? theirs_flat : ours_flat, l->src_path);
        char *own_label = compose_conflict_label(is_ours ? ours_label : theirs_label, dst);
        char *other_label_c = compose_conflict_label(is_ours ? theirs_label : ours_label, l->src_path);
        int conflict = 0;
        unsigned int mode = 0;
        unsigned char sha1[SG_SHA1_RAW_LEN];
        unsigned char *conflict_content = NULL;
        size_t conflict_content_len = 0;
        int rc;

        if (other_e == NULL || own_label == NULL || other_label_c == NULL) {
            free(own_label);
            free(other_label_c);
            free(entry.path);
            return -1;
        }
        /* The OPERANDS have to be swapped by is_ours exactly like the
           labels below them, not left as side/other. `side_e` is the
           RENAMING side, which is theirs when is_ours == 0 -- feeding it to
           merge_blob_content's ours slot while the ours LABEL correctly
           names ours puts each side's text under the other's marker. The
           merge stays a merge (three-way is near-symmetric, so the same
           lines conflict either way) and every stage is filled correctly by
           the code below, so nothing but the marker body shows it: what the
           user sees above `=======`, next to their own branch name, is the
           other branch's edit. Whoever resolves that by hand keeps the
           wrong half. */
        rc = merge_blob_content(git_dir, dst, 1, base_e->mode, base_e->sha1,
                                is_ours ? side_e->mode : other_e->mode,
                                is_ours ? side_e->sha1 : other_e->sha1,
                                is_ours ? other_e->mode : side_e->mode,
                                is_ours ? other_e->sha1 : side_e->sha1,
                                is_ours ? own_label : other_label_c,
                                is_ours ? other_label_c : own_label, 7, &conflict, &mode, sha1,
                                &conflict_content, &conflict_content_len);
        free(own_label);
        free(other_label_c);
        if (rc != 0) {
            free(entry.path);
            return -1;
        }
        if (!conflict) {
            entry.conflict = 0;
            entry.deleted = 0;
            entry.mode = mode;
            memcpy(entry.sha1, sha1, SG_SHA1_RAW_LEN);
            /* fill_conflict_sides' job, done by hand since there is no
               triple here -- and these three describe THIS ENTRY'S PATH
               (dst), not the rename's source. Only the renaming side's tree
               actually has dst; base and the other side have the SOURCE
               path, which is a different entry.

               This is load-bearing, not bookkeeping:
               sg_merge_entry_touches_ours reads exactly ours_present /
               ours_mode / ours_sha1 to decide whether
               sg_merge_result_apply may SKIP writing this path on the
               grounds that ours already has this content here. Filling the
               ours side from the source path made that check answer "yes"
               for a file ours does not have at all whenever theirs did the
               renaming and nobody edited the content -- so the renamed-to
               file was never created in the working tree, while the merge
               COMMIT's tree was correct. All four gates stayed green;
               tests/fuzz_merge_rename.py caught it. */
            if (is_ours) {
                entry.ours_present = 1;
                entry.ours_mode = side_e->mode;
                memcpy(entry.ours_sha1, side_e->sha1, SG_SHA1_RAW_LEN);
            } else {
                entry.theirs_present = 1;
                entry.theirs_mode = side_e->mode;
                memcpy(entry.theirs_sha1, side_e->sha1, SG_SHA1_RAW_LEN);
            }
            return append_result_entry(out, cap, &entry);
        }
        if (is_ours) {
            entry.ours_present = 1;
            entry.ours_mode = side_e->mode;
            memcpy(entry.ours_sha1, side_e->sha1, SG_SHA1_RAW_LEN);
            entry.theirs_present = 1;
            entry.theirs_mode = other_e->mode;
            memcpy(entry.theirs_sha1, other_e->sha1, SG_SHA1_RAW_LEN);
        } else {
            entry.theirs_present = 1;
            entry.theirs_mode = side_e->mode;
            memcpy(entry.theirs_sha1, side_e->sha1, SG_SHA1_RAW_LEN);
            entry.ours_present = 1;
            entry.ours_mode = other_e->mode;
            memcpy(entry.ours_sha1, other_e->sha1, SG_SHA1_RAW_LEN);
        }
        entry.conflict_content = conflict_content;
        entry.conflict_content_len = conflict_content_len;
        return append_result_entry(out, cap, &entry);
    }
}

/* Resolves one landing (whichever kind) into a real, already-stored
   (mode,sha1) pair, for use as one side of an outer add/add collision (Phase
   49 sec 3.5/3.6). PLAIN and DELETED are already real tree objects, no work
   needed; KEPT runs the inner merge via compute_kept_landing with
   marker_size 8 (nested) and writes even a conflicting result to the store,
   because it is about to become one side's content in the entry the outer
   merge produces. */
static int resolve_landing_for_collision(const char *git_dir, const sg_flat_list *base_flat,
                                         const sg_flat_list *ours_flat,
                                         const sg_flat_list *theirs_flat, const char *dst,
                                         int is_ours, const landing *l, const char *ours_label,
                                         const char *theirs_label, unsigned int *out_mode,
                                         unsigned char out_sha1[SG_SHA1_RAW_LEN])
{
    const sg_flat_entry *side_e = flat_find(is_ours ? ours_flat : theirs_flat, dst);

    if (side_e == NULL)
        return -1;
    if (l->kind != LANDING_KEPT) {
        *out_mode = side_e->mode;
        memcpy(out_sha1, side_e->sha1, SG_SHA1_RAW_LEN);
        return 0;
    }
    {
        const sg_flat_entry *base_e = flat_find(base_flat, l->src_path);
        const sg_flat_entry *other_e = flat_find(is_ours ? theirs_flat : ours_flat, l->src_path);
        char *own_label = compose_conflict_label(is_ours ? ours_label : theirs_label, dst);
        char *other_label_c = compose_conflict_label(is_ours ? theirs_label : ours_label, l->src_path);
        int conflict = 0;
        int rc;

        if (base_e == NULL || other_e == NULL || own_label == NULL || other_label_c == NULL) {
            free(own_label);
            free(other_label_c);
            return -1;
        }
        rc = compute_kept_landing(git_dir, base_e, side_e->mode, side_e->sha1, other_e, is_ours,
                                  is_ours ? own_label : other_label_c,
                                  is_ours ? other_label_c : own_label, 8, &conflict, out_mode,
                                  out_sha1);
        free(own_label);
        free(other_label_c);
        return rc;
    }
}

/* Phase 49 sec 3.5/3.6: both sides want to land content at the same
   destination. Builds a synthetic triple (base absent) around each side's
   already-resolved (mode,sha1) landing and reuses process_path unchanged --
   its ordinary add/add branch already produces exactly this shape (no
   stage 1, stage 2/3 only on conflict, a plain clean entry when the two
   landings happen to agree). No ":path" suffix: both sides' own path at
   THIS comparison is the same dst (Phase 49 sec 4's rule falls out on its
   own, not a special case). */
static int emit_collision(const char *git_dir, const sg_flat_list *base_flat,
                          const sg_flat_list *ours_flat, const sg_flat_list *theirs_flat,
                          const char *dst, const contribution *c, const char *ours_label,
                          const char *theirs_label, sg_merge_result *out, size_t *cap)
{
    sg_flat_entry synthetic_ours, synthetic_theirs;
    triple t;
    sg_merge_result_entry entry;

    memset(&synthetic_ours, 0, sizeof(synthetic_ours));
    memset(&synthetic_theirs, 0, sizeof(synthetic_theirs));
    if (resolve_landing_for_collision(git_dir, base_flat, ours_flat, theirs_flat, dst, 1, &c->ours,
                                      ours_label, theirs_label, &synthetic_ours.mode,
                                      synthetic_ours.sha1) != 0)
        return -1;
    if (resolve_landing_for_collision(git_dir, base_flat, ours_flat, theirs_flat, dst, 0, &c->theirs,
                                      ours_label, theirs_label, &synthetic_theirs.mode,
                                      synthetic_theirs.sha1) != 0)
        return -1;

    memset(&t, 0, sizeof(t));
    t.path = dst;
    t.ours_present = 1;
    t.ours_e = &synthetic_ours;
    t.theirs_present = 1;
    t.theirs_e = &synthetic_theirs;

    if (process_path(git_dir, &t, ours_label, theirs_label, &entry) != 0)
        return -1;
    return append_result_entry(out, cap, &entry);
}

/* Phase 49 sec 3.3: both sides renamed p to the SAME name. Collapses to an
   ordinary content merge at that destination via process_path, unwidened
   (marker size 7) and unsuffixed (both sides' own path IS the destination).
   Stage 1 (when conflicting) is process_path's own base_present path,
   naturally relocated to the new name since t.path is the destination, not
   p. */
static int emit_same_destination(const char *git_dir, const sg_flat_list *base_flat,
                                 const sg_flat_list *ours_flat, const sg_flat_list *theirs_flat,
                                 const char *p, const char *dst, const char *ours_label,
                                 const char *theirs_label, sg_merge_result *out, size_t *cap)
{
    const sg_flat_entry *base_e = flat_find(base_flat, p);
    const sg_flat_entry *ours_e = flat_find(ours_flat, dst);
    const sg_flat_entry *theirs_e = flat_find(theirs_flat, dst);
    triple t;
    sg_merge_result_entry entry;

    if (base_e == NULL || ours_e == NULL || theirs_e == NULL)
        return -1;
    memset(&t, 0, sizeof(t));
    t.path = dst;
    t.base_present = 1;
    t.base_e = base_e;
    t.ours_present = 1;
    t.ours_e = ours_e;
    t.theirs_present = 1;
    t.theirs_e = theirs_e;

    if (process_path(git_dir, &t, ours_label, theirs_label, &entry) != 0)
        return -1;
    return append_result_entry(out, cap, &entry);
}

/* Phase 49 sec 3.4: both sides renamed p to DIFFERENT names -- rename/rename
   "1 to 2". Always an unresolved conflict (the destination NAME itself is
   ambiguous, independent of whether the content trivially merges), laid out
   as three index-only paths: p keeps only a stage-1 entry and no working-tree
   file at all (conflict_no_workdir_file), o_dst and t_dst each keep only
   their own single stage but share the SAME freshly-written blob -- the
   result of a marker_size-8 three-way merge of base[p]/ours[o_dst]/
   theirs[t_dst], written to the store even when clean (the shared blob has
   to exist as a real object regardless). Both marker labels get the ":path"
   suffix (o_dst != t_dst by construction of this branch). Appends 3 entries. */
static int emit_rename_rename_1to2(const char *git_dir, const sg_flat_list *base_flat,
                                   const sg_flat_list *ours_flat, const sg_flat_list *theirs_flat,
                                   const char *p, const char *o_dst, const char *t_dst,
                                   const char *ours_label, const char *theirs_label,
                                   sg_merge_result *out, size_t *cap)
{
    const sg_flat_entry *base_e = flat_find(base_flat, p);
    const sg_flat_entry *ours_e = flat_find(ours_flat, o_dst);
    const sg_flat_entry *theirs_e = flat_find(theirs_flat, t_dst);
    char *ours_label_c = NULL;
    char *theirs_label_c = NULL;
    unsigned char *conflict_content = NULL;
    size_t conflict_content_len = 0;
    unsigned int merged_mode = 0;
    unsigned char merged_sha1[SG_SHA1_RAW_LEN];
    int conflict = 0;
    int rc;
    sg_merge_result_entry e_p, e_o, e_t;

    if (base_e == NULL || ours_e == NULL || theirs_e == NULL)
        return -1;
    ours_label_c = compose_conflict_label(ours_label, o_dst);
    theirs_label_c = compose_conflict_label(theirs_label, t_dst);
    if (ours_label_c == NULL || theirs_label_c == NULL) {
        free(ours_label_c);
        free(theirs_label_c);
        return -1;
    }
    rc = merge_blob_content(git_dir, p, 1, base_e->mode, base_e->sha1, ours_e->mode, ours_e->sha1,
                            theirs_e->mode, theirs_e->sha1, ours_label_c, theirs_label_c, 8,
                            &conflict, &merged_mode, merged_sha1, &conflict_content,
                            &conflict_content_len);
    free(ours_label_c);
    free(theirs_label_c);
    if (rc != 0)
        return -1;
    /* merge_blob_content's *out_mode is only ever meaningful on its CLEAN
       path (its conflict branch leaves it untouched, since an ordinary
       conflict entry has no single resolved mode), and every entry this
       function builds is a conflict at the INDEX level regardless -- so the
       mode is decided here, for both sub-cases at once.

       MEASURED, and it is not "ours' own mode": with base 644, ours 644 and
       theirs 755, real git writes 100755 at stage 2 AND at stage 3, and
       leaves both working-tree files executable. The reversed fixture (ours
       755, theirs 644) also gives 100755 at both stages. So both stages
       carry the ORDINARY merged mode -- whichever side changed it away from
       base wins -- and the two stages always agree, exactly as they already
       do about the blob. Taking ours' mode passes the first fixture and
       fails the second, which is why the pair is pinned together. */
    if (ours_e->mode != base_e->mode)
        merged_mode = ours_e->mode;
    else if (theirs_e->mode != base_e->mode)
        merged_mode = theirs_e->mode;
    else
        merged_mode = base_e->mode;
    if (!conflict) {
        /* merge_blob_content already stored the clean bytes as an object
           (merged_sha1 is already valid) and, being an ordinary CLEAN
           outcome, deliberately did NOT hand the bytes back in
           conflict_content (NULL here) -- ordinary callers never need them,
           they already have the sha1. This path is different: the content
           merged cleanly, but rename/rename-1to2 is STILL an unresolved
           conflict at the index level (see the function comment), so the
           two entries below are built through the conflict branches, which
           write conflict_content verbatim to the working tree -- there is
           no stage-0 entry here for the ordinary non-conflict branch to
           read the blob back through. Read the same object straight back
           rather than re-deriving it. */
        if (read_blob_chunk_aware(git_dir, merged_sha1, o_dst, &conflict_content,
                                  &conflict_content_len) != 0)
            return -1;
    } else if (store_new_blob(git_dir, conflict_content, conflict_content_len, merged_sha1) != 0) {
        /* merge_blob_content leaves out_sha1 UNTOUCHED on a conflict -- an
           ordinary conflict has no resolved blob, so no caller of it ever
           needed one. This shape does: measured against real git, stage 2
           and stage 3 both hold the marker-laden merge RESULT as a real
           blob (cat-file on either stage prints exactly the working-tree
           file). Without storing it here, merged_sha1 is still the
           uninitialized stack array and both stages name an object that
           does not exist -- a corrupt index, not merely a wrong answer.
           make/make test/interop/sanitize were all green for it, because
           interop's phase45 fixture is a PURE rename whose inner merge is
           clean, and the clean branch above does fill merged_sha1;
           tests/fuzz_merge_rename.py is what caught it. */
        free(conflict_content);
        return -1;
    }

    memset(&e_p, 0, sizeof(e_p));
    e_p.path = strdup(p);
    if (e_p.path == NULL)
        return -1;
    e_p.conflict = 1;
    e_p.conflict_no_workdir_file = 1;
    e_p.base_present = 1;
    e_p.base_mode = base_e->mode;
    memcpy(e_p.base_sha1, base_e->sha1, SG_SHA1_RAW_LEN);
    if (append_result_entry(out, cap, &e_p) != 0) {
        free(conflict_content);
        return -1;
    }

    memset(&e_o, 0, sizeof(e_o));
    e_o.path = strdup(o_dst);
    if (e_o.path == NULL) {
        free(conflict_content);
        return -1;
    }
    e_o.conflict = 1;
    e_o.ours_present = 1;
    e_o.ours_mode = merged_mode;
    memcpy(e_o.ours_sha1, merged_sha1, SG_SHA1_RAW_LEN);
    if (conflict_content != NULL) {
        e_o.conflict_content = malloc(conflict_content_len > 0 ? conflict_content_len : 1);
        if (e_o.conflict_content == NULL) {
            free(e_o.path);
            free(conflict_content);
            return -1;
        }
        memcpy(e_o.conflict_content, conflict_content, conflict_content_len);
        e_o.conflict_content_len = conflict_content_len;
    }
    if (append_result_entry(out, cap, &e_o) != 0) {
        free(conflict_content);
        return -1;
    }

    memset(&e_t, 0, sizeof(e_t));
    e_t.path = strdup(t_dst);
    if (e_t.path == NULL) {
        free(conflict_content);
        return -1;
    }
    e_t.conflict = 1;
    e_t.theirs_present = 1;
    e_t.theirs_mode = merged_mode;
    memcpy(e_t.theirs_sha1, merged_sha1, SG_SHA1_RAW_LEN);
    e_t.conflict_content = conflict_content;
    e_t.conflict_content_len = conflict_content_len;
    return append_result_entry(out, cap, &e_t);
}

/* Pass one (classification) + pass two (resolution) of the rename-aware
   pipeline described in the section comment above. Appends every entry it
   produces directly to *out (growing *cap the same way the ordinary walk
   does); the caller sorts *out by path afterward, since these entries land
   at destinations that are not necessarily where the union walk's cursor
   currently is. Returns 0 on success, -1 on error. */
/* A rename source that the OTHER side never renamed away is still physically
   present in ours' tree, and therefore in the pre-merge working tree -- but
   the merge result must not contain it (measured: git's index has no entry
   at the old name and the file is gone from disk). Nothing else emits it:
   the ordinary union walk skips every rename source
   (path_is_rename_consumed), and the landing entry lives at the DESTINATION
   path. So it needs a deleted entry of its own.

   ours' side is filled in deliberately: that is exactly what
   sg_merge_entry_touches_ours reads for a deleted entry, and returning 0
   there is what stops sg_merge_result_apply from unlinking a same-named
   UNTRACKED file on a path ours never had (Phase 20). Here ours did have
   it, so the remove() is the right answer.

   Reached whenever ours kept the source and theirs renamed it (sec 3.1's
   mirror, sec 3.2's mirror, and both collision shapes -- in a 2-to-1
   collision ours keeps THEIRS' source). Never reached for sec 3.4, where
   both sides renamed p away, so it cannot double-emit against
   emit_rename_rename_1to2's own stage-1 entry at p. */
static int emit_consumed_source_deletion(const sg_flat_list *base_flat,
                                         const sg_flat_list *ours_flat, const char *p,
                                         sg_merge_result *out, size_t *cap)
{
    const sg_flat_entry *ours_e = flat_find(ours_flat, p);
    const sg_flat_entry *base_e = flat_find(base_flat, p);
    sg_merge_result_entry entry;

    if (ours_e == NULL)
        return 0;
    memset(&entry, 0, sizeof(entry));
    entry.path = strdup(p);
    if (entry.path == NULL)
        return -1;
    entry.deleted = 1;
    entry.ours_present = 1;
    entry.ours_mode = ours_e->mode;
    memcpy(entry.ours_sha1, ours_e->sha1, SG_SHA1_RAW_LEN);
    if (base_e != NULL) {
        entry.base_present = 1;
        entry.base_mode = base_e->mode;
        memcpy(entry.base_sha1, base_e->sha1, SG_SHA1_RAW_LEN);
    }
    return append_result_entry(out, cap, &entry);
}

static int handle_renames(const char *git_dir, const sg_flat_list *base_flat,
                          const sg_flat_list *ours_flat, const sg_flat_list *theirs_flat,
                          const rename_map *ro, const rename_map *rt, const char *ours_label,
                          const char *theirs_label, sg_merge_result *out, size_t *cap)
{
    const char **s_base = NULL;
    size_t n_s_base = 0;
    contribution_list contribs;
    size_t i;
    int rc = -1;

    memset(&contribs, 0, sizeof(contribs));

    s_base = calloc((ro->count + rt->count) > 0 ? ro->count + rt->count : 1, sizeof(*s_base));
    if (s_base == NULL)
        goto done;
    for (i = 0; i < ro->count; i++)
        s_base[n_s_base++] = ro->v[i].base_path;
    for (i = 0; i < rt->count; i++) {
        size_t j;
        int dup = 0;

        for (j = 0; j < n_s_base; j++) {
            if (strcmp(s_base[j], rt->v[i].base_path) == 0) {
                dup = 1;
                break;
            }
        }
        if (!dup)
            s_base[n_s_base++] = rt->v[i].base_path;
    }

    /* Pass one: classify every base source. Self-contained combos (both
       sides renamed p, sec 3.3/3.4) are emitted immediately; the other four
       combos register a contribution and are resolved in pass two, once
       every source has been seen (a 2-to-1 collision needs both sides'
       sources classified before either can be resolved). */
    for (i = 0; i < n_s_base; i++) {
        const char *p = s_base[i];
        const char *o_dst = rename_map_dst(ro, p);
        const char *t_dst = rename_map_dst(rt, p);
        int o_kept = o_dst == NULL && flat_find(ours_flat, p) != NULL;
        int t_kept = t_dst == NULL && flat_find(theirs_flat, p) != NULL;

        if (o_dst != NULL && t_dst != NULL) {
            if (strcmp(o_dst, t_dst) == 0) {
                if (emit_same_destination(git_dir, base_flat, ours_flat, theirs_flat, p, o_dst,
                                          ours_label, theirs_label, out, cap) != 0)
                    goto done;
            } else if (emit_rename_rename_1to2(git_dir, base_flat, ours_flat, theirs_flat, p, o_dst,
                                              t_dst, ours_label, theirs_label, out, cap) != 0) {
                goto done;
            }
            continue;
        }
        if (o_dst != NULL) {
            contribution *c = contribution_find_or_add(&contribs, o_dst);

            if (c == NULL)
                goto done;
            c->ours.kind = t_kept ? LANDING_KEPT : LANDING_DELETED;
            c->ours.src_path = p;
        }
        if (t_dst != NULL) {
            contribution *c = contribution_find_or_add(&contribs, t_dst);

            if (c == NULL)
                goto done;
            c->theirs.kind = o_kept ? LANDING_KEPT : LANDING_DELETED;
            c->theirs.src_path = p;
            /* o_kept means ours still physically has p; theirs renamed it
               away, so p has to be removed from the working tree. */
            if (o_kept &&
               emit_consumed_source_deletion(base_flat, ours_flat, p, out, cap) != 0)
                goto done;
        }
    }

    /* Fold in plain (non-rename) additions that happen to collide with a
       rename destination registered above (Phase 49 sec 3.5) -- the other
       side of a 2-to-1 collision (sec 3.6) is already present from pass one
       and is never touched here. */
    for (i = 0; i < contribs.count; i++) {
        contribution *c = &contribs.v[i];

        if (c->ours.kind == LANDING_NONE && flat_find(base_flat, c->dst) == NULL &&
           flat_find(ours_flat, c->dst) != NULL) {
            c->ours.kind = LANDING_PLAIN;
            c->ours.src_path = NULL;
        }
        if (c->theirs.kind == LANDING_NONE && flat_find(base_flat, c->dst) == NULL &&
           flat_find(theirs_flat, c->dst) != NULL) {
            c->theirs.kind = LANDING_PLAIN;
            c->theirs.src_path = NULL;
        }
    }

    /* Pass two: resolve every destination exactly once. */
    for (i = 0; i < contribs.count; i++) {
        const contribution *c = &contribs.v[i];
        int has_ours = c->ours.kind != LANDING_NONE;
        int has_theirs = c->theirs.kind != LANDING_NONE;

        if (has_ours && has_theirs) {
            if (emit_collision(git_dir, base_flat, ours_flat, theirs_flat, c->dst, c, ours_label,
                               theirs_label, out, cap) != 0)
                goto done;
        } else if (has_ours) {
            if (emit_standalone_landing(git_dir, base_flat, ours_flat, theirs_flat, c->dst, 1,
                                        &c->ours, ours_label, theirs_label, out, cap) != 0)
                goto done;
        } else if (has_theirs) {
            if (emit_standalone_landing(git_dir, base_flat, ours_flat, theirs_flat, c->dst, 0,
                                        &c->theirs, ours_label, theirs_label, out, cap) != 0)
                goto done;
        }
    }
    rc = 0;

done:
    free(s_base);
    free(contribs.v);
    return rc;
}

int sg_merge_trees(const char *git_dir, const unsigned char base_tree[SG_SHA1_RAW_LEN],
                   const unsigned char ours_tree[SG_SHA1_RAW_LEN],
                   const unsigned char theirs_tree[SG_SHA1_RAW_LEN], const char *ours_label,
                   const char *theirs_label, int rename_score, sg_merge_result *out)
{
    sg_flat_list base_flat, ours_flat, theirs_flat;
    sg_diff_list ro_list, rt_list;
    rename_map ro, rt;
    int have_renames = 0;
    size_t bi = 0, oi = 0, ti = 0;
    size_t cap = 0;
    int rc = 0;
    char bad_path[SG_PATH_MAX];
    int flatten_rc;

    memset(out, 0, sizeof(*out));
    memset(&base_flat, 0, sizeof(base_flat));
    memset(&ours_flat, 0, sizeof(ours_flat));
    memset(&theirs_flat, 0, sizeof(theirs_flat));
    memset(&ro_list, 0, sizeof(ro_list));
    memset(&rt_list, 0, sizeof(rt_list));
    memset(&ro, 0, sizeof(ro));
    memset(&rt, 0, sizeof(rt));

    flatten_rc = sg_tree_flatten(git_dir, base_tree, &base_flat, bad_path);
    if (flatten_rc == -2) {
        fprintf(stderr, "sg: path %s is invalid, refusing to flatten this tree into file paths\n",
               sg_quote_path_delimited(bad_path));
        return -1;
    }
    if (flatten_rc != 0)
        return -1;
    flatten_rc = sg_tree_flatten(git_dir, ours_tree, &ours_flat, bad_path);
    if (flatten_rc == -2) {
        fprintf(stderr, "sg: path %s is invalid, refusing to flatten this tree into file paths\n",
               sg_quote_path_delimited(bad_path));
        sg_flat_list_free(&base_flat);
        return -1;
    }
    if (flatten_rc != 0) {
        sg_flat_list_free(&base_flat);
        return -1;
    }
    flatten_rc = sg_tree_flatten(git_dir, theirs_tree, &theirs_flat, bad_path);
    if (flatten_rc == -2) {
        fprintf(stderr, "sg: path %s is invalid, refusing to flatten this tree into file paths\n",
               sg_quote_path_delimited(bad_path));
        sg_flat_list_free(&base_flat);
        sg_flat_list_free(&ours_flat);
        return -1;
    }
    if (flatten_rc != 0) {
        sg_flat_list_free(&base_flat);
        sg_flat_list_free(&ours_flat);
        return -1;
    }

    /* Phase 49: detect renames before the union walk, so the walk can skip
       every path this pulls out for special handling (see the "Phase 49"
       section comment above sg_merge_trees for the full shape). 0 disables
       this outright and reproduces pre-Phase-49 behaviour byte for byte --
       neither build_rename_map call runs, ro/rt stay empty, and
       path_is_rename_consumed is always false. */
    if (rename_score > 0) {
        if (build_rename_map(git_dir, base_tree, ours_tree, rename_score, &ro_list, &ro) != 0) {
            sg_flat_list_free(&base_flat);
            sg_flat_list_free(&ours_flat);
            sg_flat_list_free(&theirs_flat);
            return -1;
        }
        if (build_rename_map(git_dir, base_tree, theirs_tree, rename_score, &rt_list, &rt) != 0) {
            sg_diff_list_free(&ro_list);
            rename_map_free(&ro);
            sg_flat_list_free(&base_flat);
            sg_flat_list_free(&ours_flat);
            sg_flat_list_free(&theirs_flat);
            return -1;
        }
        have_renames = 1;
    }

    while (bi < base_flat.count || oi < ours_flat.count || ti < theirs_flat.count) {
        const char *bp = bi < base_flat.count ? base_flat.entries[bi].path : NULL;
        const char *op = oi < ours_flat.count ? ours_flat.entries[oi].path : NULL;
        const char *tp = ti < theirs_flat.count ? theirs_flat.entries[ti].path : NULL;
        const char *path = min_path3(bp, op, tp);
        triple t;
        sg_merge_result_entry entry;
        int consumed;

        memset(&t, 0, sizeof(t));
        t.path = path;
        if (bp != NULL && strcmp(bp, path) == 0) {
            t.base_present = 1;
            t.base_e = &base_flat.entries[bi];
            bi++;
        }
        if (op != NULL && strcmp(op, path) == 0) {
            t.ours_present = 1;
            t.ours_e = &ours_flat.entries[oi];
            oi++;
        }
        if (tp != NULL && strcmp(tp, path) == 0) {
            t.theirs_present = 1;
            t.theirs_e = &theirs_flat.entries[ti];
            ti++;
        }

        consumed = have_renames && path_is_rename_consumed(&ro, &rt, path);
        if (consumed)
            continue;

        if (process_path(git_dir, &t, ours_label, theirs_label, &entry) != 0) {
            rc = -1;
            break;
        }

        if (out->count == cap) {
            size_t new_cap = cap == 0 ? 16 : cap * 2;
            sg_merge_result_entry *grown = realloc(out->entries, new_cap * sizeof(*grown));

            if (grown == NULL) {
                free(entry.path);
                free(entry.conflict_content);
                rc = -1;
                break;
            }
            out->entries = grown;
            cap = new_cap;
        }
        out->entries[out->count] = entry;
        out->count++;
    }

    if (rc == 0 && have_renames) {
        rc = handle_renames(git_dir, &base_flat, &ours_flat, &theirs_flat, &ro, &rt, ours_label,
                            theirs_label, out, &cap);
    }
    if (rc == 0 && out->count > 0)
        qsort(out->entries, out->count, sizeof(*out->entries), result_entry_path_cmp);

    sg_diff_list_free(&ro_list);
    sg_diff_list_free(&rt_list);
    rename_map_free(&ro);
    rename_map_free(&rt);
    sg_flat_list_free(&base_flat);
    sg_flat_list_free(&ours_flat);
    sg_flat_list_free(&theirs_flat);

    if (rc != 0) {
        sg_merge_result_free(out);
        return -1;
    }
    return 0;
}

void sg_merge_result_free(sg_merge_result *result)
{
    size_t i;

    for (i = 0; i < result->count; i++) {
        free(result->entries[i].path);
        free(result->entries[i].conflict_content);
    }
    free(result->entries);
    result->entries = NULL;
    result->count = 0;
}

/* ==================== materializing a merge result ==================== */

static int add_stage_entry(sg_index *idx, const char *path, unsigned int stage, unsigned int mode,
                           const unsigned char sha1[SG_SHA1_RAW_LEN])
{
    sg_index_entry entry;

    memset(&entry, 0, sizeof(entry));
    entry.mode = mode;
    entry.stage = stage;
    memcpy(entry.sha1, sha1, SG_SHA1_RAW_LEN);
    entry.path = (char *)path;
    return sg_index_upsert(idx, &entry);
}

static int add_resolved_entry(const char *repo_root, sg_index *idx, const char *path,
                              unsigned int mode, const unsigned char sha1[SG_SHA1_RAW_LEN])
{
    char abspath[SG_PATH_MAX];
    struct stat st;
    sg_index_entry entry;

    memset(&entry, 0, sizeof(entry));
    /* sg_merge_result_apply already ran this exact join, with the same
       repo_root and path, at the top of the loop that calls us, so from that
       (currently only) call site this check is unreachable: the observable
       defence is the caller's, and a mutation aimed at truncation handling
       has to be planted there, not here -- a redundant guard hides the
       verification point. It stays because the alternative is an unchecked
       join, and because if this static ever grows a second caller the right
       behaviour is the one below: fail the entry outright rather than stat a
       wrong path and hand back an index carrying bogus data for it. The
       caller treats a non-zero return like an sg_index_upsert failure and
       aborts the whole apply. */
    if (sg_path_join(abspath, sizeof(abspath), repo_root, path) != 0) {
        fprintf(stderr, "sg: path too long, cannot resolve %s\n", sg_quote_path_delimited(path));
        return -1;
    }
    if (stat(abspath, &st) == 0) {
        entry.ctime_sec = (unsigned int)st.st_ctime;
        entry.mtime_sec = (unsigned int)st.st_mtime;
#if defined(__APPLE__)
        entry.ctime_nsec = (unsigned int)st.st_ctimespec.tv_nsec;
        entry.mtime_nsec = (unsigned int)st.st_mtimespec.tv_nsec;
#else
        entry.ctime_nsec = (unsigned int)st.st_ctim.tv_nsec;
        entry.mtime_nsec = (unsigned int)st.st_mtim.tv_nsec;
#endif
        entry.dev = (unsigned int)st.st_dev;
        entry.ino = (unsigned int)st.st_ino;
        entry.uid = (unsigned int)st.st_uid;
        entry.gid = (unsigned int)st.st_gid;
        entry.file_size = (unsigned int)st.st_size;
    }
    entry.mode = mode;
    entry.stage = 0;
    memcpy(entry.sha1, sha1, SG_SHA1_RAW_LEN);
    entry.path = (char *)path;
    return sg_index_upsert(idx, &entry);
}

int sg_merge_entry_touches_ours(const sg_merge_result_entry *e)
{
    if (e->conflict)
        return 1;
    if (e->deleted)
        return e->ours_present;
    return !e->ours_present || e->ours_mode != e->mode ||
          memcmp(e->ours_sha1, e->sha1, SG_SHA1_RAW_LEN) != 0;
}

int sg_merge_result_apply(const char *git_dir, const char *repo_root, const sg_merge_result *result,
                          sg_index *index_out, char ***conflict_paths_out, size_t *conflict_count_out)
{
    char **conflict_paths = NULL;
    size_t conflict_count = 0;
    size_t i;
    int index_ok = 1;
    int content_missing = 0;

    memset(index_out, 0, sizeof(*index_out));

    for (i = 0; i < result->count; i++) {
        sg_merge_result_entry *e = &result->entries[i];
        char abspath[SG_PATH_MAX];

        /* A truncated path here can't safely be used for any of the three
           branches below (conflict write, deletion, or content write), so
           this entry must count as a failure the same way a missing chunk
           or unreadable object does -- never silently skipped. */
        if (sg_path_join(abspath, sizeof(abspath), repo_root, e->path) != 0) {
            fprintf(stderr, "sg: path too long, cannot process %s\n",
                   sg_quote_path_delimited(e->path));
            content_missing = 1;
            continue;
        }

        if (e->conflict) {
            int mode = e->ours_present ? (int)(e->ours_mode & 0777)
                                       : (e->theirs_present ? (int)(e->theirs_mode & 0777) : 0644);
            char **grown;

            /* Phase 49: rename/rename-1to2's original path keeps only a
               stage-1 index entry and has no working-tree file at all --
               git leaves no file at the old name. sg_merge_entry_touches_ours
               always answers 1 for a conflict, so the guard here is just the
               remove() itself, same as the `deleted` branch below. */
            if (e->conflict_no_workdir_file) {
                if (remove(abspath) == 0)
                    sg_prune_empty_parents(repo_root, e->path);
            } else if (sg_write_file_mkdirs(abspath, e->conflict_content, e->conflict_content_len,
                                            mode) != 0) {
                fprintf(stderr, "sg: failed to write conflicted %s\n",
                       sg_quote_path_delimited(e->path));
            }

            if (e->base_present && add_stage_entry(index_out, e->path, 1, e->base_mode,
                                                   e->base_sha1) != 0)
                index_ok = 0;
            if (e->ours_present && add_stage_entry(index_out, e->path, 2, e->ours_mode,
                                                   e->ours_sha1) != 0)
                index_ok = 0;
            if (e->theirs_present && add_stage_entry(index_out, e->path, 3, e->theirs_mode,
                                                     e->theirs_sha1) != 0)
                index_ok = 0;

            grown = realloc(conflict_paths, (conflict_count + 1) * sizeof(*grown));
            if (grown != NULL) {
                conflict_paths = grown;
                conflict_paths[conflict_count] = strdup(e->path);
                if (conflict_paths[conflict_count] != NULL)
                    conflict_count++;
            }
        } else if (e->deleted) {
            /* Skip the remove() when ours never had this path in the first
               place (sg_merge_entry_touches_ours) -- otherwise this would
               unlink an unrelated, unversioned file that happens to share
               the name (Phase 20: base had the path, ours and theirs both
               deleted it, and some untracked file with the same name now
               sits at abspath).

               Phase 36: unlike apply.c's equivalent remove(), e->path has no
               guard of its own here -- same situation add_resolved_entry's
               comment above already documents for its own sg_path_join, and
               deliberately not fixed the same way (no guard added) for the
               same reason: it would be a redundant defence that hides the
               real one. This is safe today ONLY because e->path is a
               structural fact, not an enforced invariant: sg_merge_result's
               entries are built exclusively by sg_merge_trees out of three
               trees that all went through sg_tree_flatten first (each
               returns -2 and aborts the merge outright on a path that fails
               sg_path_component_is_safe), so nothing reaches this loop that
               flatten did not already clear. If sg_merge_result ever grows a
               second producer -- e.g. building one by hand for a test
               fixture, or a future caller assembling a result outside
               sg_merge_trees -- that producer becomes responsible for the
               same validation, or this remove() reopens exactly the
               tree-build hole Phase 36 closed elsewhere (see
               sg_tree_build_from_workdir in tree_build.c and
               docs/DESIGN.md's Phase 36 section for the full writeup). */
            if (sg_merge_entry_touches_ours(e) && remove(abspath) == 0)
                sg_prune_empty_parents(repo_root, e->path);
        } else {
            /* Skip re-reading/rewriting when the resolved outcome already
               equals ours -- this is the untouched-path case (Phase 20 spec
               sec 4.2/4.4): leaves whatever is on disk there alone instead
               of clobbering a dirty-but-untouched working-tree file with
               ours's own (identical) content. add_resolved_entry still runs
               unconditionally below: the index must stay complete even for
               paths this merge never touched (see this function's header
               comment) -- cmd_merge.c and cmd_rebase.c build the merge
               commit's tree straight from new_idx, so a missing path here
               would silently drop a file from the resulting commit. */
            if (sg_merge_entry_touches_ours(e)) {
                unsigned char *content;
                size_t content_len;
                sg_chunk_missing_info missing;
                int read_rc = sg_chunk_read_blob(git_dir, e->sha1, &content, &content_len, &missing);

                if (read_rc == 0) {
                    if (sg_write_file_mkdirs(abspath, content, content_len, (int)(e->mode & 0777)) != 0) {
                        fprintf(stderr, "sg: failed to write %s\n", sg_quote_path_delimited(e->path));
                        content_missing = 1;
                    }
                    free(content);
                } else if (read_rc == -2) {
                    sg_chunk_print_missing_error(e->path, &missing);
                    content_missing = 1;
                } else {
                    /* -1: the object behind this sha1 could not be read at
                       all (see sg/chunk.h). The file never reached the
                       working tree, yet the index entry below records it as
                       present at this sha1 -- exactly the "silently dropped
                       a path" state this function promises never to hand
                       back. Fatal, like -2. */
                    fprintf(stderr, "sg: missing blob for %s\n", sg_quote_path_delimited(e->path));
                    content_missing = 1;
                }
            }
            if (add_resolved_entry(repo_root, index_out, e->path, e->mode, e->sha1) != 0)
                index_ok = 0;
        }
    }

    /* Any path that failed to reach the working tree -- unrecoverable chunk
       data, an unreadable object, or a failed write -- or an index that
       couldn't be built completely (allocation failure), must abort the
       whole apply. There's nothing left to do except refuse to hand back a
       state that silently drops content or paths. The distinction matters
       most to sg_stash_apply's caller: `sg stash pop` drops the entry once
       apply reports success, so a per-path failure reported as success
       would cost the user both the file and its only backup. */
    if (content_missing || !index_ok) {
        sg_index_free(index_out);
        memset(index_out, 0, sizeof(*index_out));
        for (i = 0; i < conflict_count; i++)
            free(conflict_paths[i]);
        free(conflict_paths);
        return -1;
    }

    if (conflict_count == 0) {
        free(conflict_paths);
        conflict_paths = NULL;
    }
    *conflict_paths_out = conflict_paths;
    *conflict_count_out = conflict_count;
    return 0;
}

/* ==================== MERGE_HEAD ==================== */

static int merge_head_path(const char *git_dir, char *out, size_t out_size)
{
    return (size_t)snprintf(out, out_size, "%s/MERGE_HEAD", git_dir) < out_size ? 0 : -1;
}

int sg_merge_head_exists(const char *git_dir)
{
    char path[SG_PATH_MAX];
    struct stat st;

    if (merge_head_path(git_dir, path, sizeof(path)) != 0)
        return 0;
    /* lstat, and no S_ISREG filter: real git gates on file_exists(), which is
       an lstat that only asks whether *something* is there. Measured -- git
       refuses to switch even when MERGE_HEAD is a directory. A dangling
       symlink is the same story, which is why this is lstat and not stat. */
    return lstat(path, &st) == 0;
}

int sg_merge_head_read(const char *git_dir, unsigned char out[SG_SHA1_RAW_LEN])
{
    char path[SG_PATH_MAX];
    FILE *f;
    char hexbuf[SG_SHA1_HEX_LEN + 2];
    char *nl;

    if (merge_head_path(git_dir, path, sizeof(path)) != 0)
        return -1;
    f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    if (fgets(hexbuf, sizeof(hexbuf), f) == NULL) {
        fclose(f);
        return -1;
    }
    fclose(f);
    nl = strchr(hexbuf, '\n');
    if (nl != NULL)
        *nl = '\0';
    return sg_hex_to_sha1(hexbuf, out);
}

int sg_merge_head_write(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN])
{
    char path[SG_PATH_MAX];
    char hex[SG_SHA1_HEX_LEN + 1];
    FILE *f;

    if (merge_head_path(git_dir, path, sizeof(path)) != 0)
        return -1;
    sg_sha1_to_hex(id, hex);
    f = fopen(path, "wb");
    if (f == NULL)
        return -1;
    if (fprintf(f, "%s\n", hex) < 0) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

int sg_merge_head_remove(const char *git_dir)
{
    char path[SG_PATH_MAX];

    if (merge_head_path(git_dir, path, sizeof(path)) != 0)
        return -1;
    if (remove(path) != 0 && errno != ENOENT)
        return -1;
    return 0;
}
