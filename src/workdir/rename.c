#include "sg/diff.h"

#include "sg/similarity.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Rename detection, as a pass over an already-built change list rather than
   something the four builders each do for themselves -- the same reasoning
   as sg_diff_list_filter: one place that decides cannot disagree with
   itself. See include/sg/diff.h for the ordering rule against filtering,
   which is measured, not assumed.

   Three passes, in git's order, because the ORDER is part of the answer and
   not an optimization (git 2.55.0, diffcore-rename.c):

     1. exact    -- identical content, settled by object id, always 100%.
     2. basename -- a source and a destination whose file names (after the
                    last '/') are unique on both sides pair up if they clear a
                    RAISED threshold, without ever being compared to anything
                    else.
     3. matrix   -- everything still unpaired is scored against everything
                    else, best pairs first.

   Pass 2 is why this cannot be collapsed into "score every pair and take the
   best": it can settle on a pair that pass 3 would have rejected in favour of
   a better-scoring one elsewhere. git's own comment admits the result may be
   sub-optimal and keeps it anyway, so reproducing git means reproducing that. */

/* git's diff.renameLimit default: above this many candidates on either axis
   the matrix is abandoned entirely and those files stay adds and deletes. */
#define SG_RENAME_LIMIT 1000
/* git's NUM_CANDIDATE_PER_DST: only the best few sources per destination
   survive into the global ranking. */
#define SG_RENAME_CANDIDATES 4
/* git's cap on how many identically-contented sources it will look at before
   settling for the best seen so far. */
#define SG_RENAME_EXACT_ALTERNATIVES 100

static int is_deletion(const sg_diff_entry *e)
{
    return !e->unmerged && e->old_side.kind != SG_DIFF_SIDE_ABSENT &&
           e->new_side.kind == SG_DIFF_SIDE_ABSENT;
}

static int is_addition(const sg_diff_entry *e)
{
    return !e->unmerged && e->old_side.kind == SG_DIFF_SIDE_ABSENT &&
           e->new_side.kind != SG_DIFF_SIDE_ABSENT;
}

/* A path that changed but is still there. Only copy detection cares: it is
   what lets `sg diff -C` find a copy taken from a file that was merely
   edited, which plain rename detection has no use for. */
static int is_modification(const sg_diff_entry *e)
{
    return !e->unmerged && e->old_side.kind != SG_DIFF_SIDE_ABSENT &&
           e->new_side.kind != SG_DIFF_SIDE_ABSENT;
}

/* Mode 0 means "unknown" in sg_diff_side, and no builder in this codebase
   ever produces a non-regular mode (symlinks are excluded from every
   tracked-file walk). So the only question worth asking is whether a mode
   that IS known says non-regular; unknown is treated as regular, because
   rejecting it would turn "we did not record a mode" into "not a rename". */
static int mode_is_regular(unsigned int mode)
{
    return mode == 0 || (mode & S_IFMT) == S_IFREG;
}

/* git's basename_same: do the two paths end in the same file name? */
static int basename_same(const char *a, const char *b)
{
    size_t alen = strlen(a), blen = strlen(b);

    while (alen != 0 && blen != 0) {
        char c1 = a[--alen];
        char c2 = b[--blen];

        if (c1 != c2)
            return 0;
        if (c1 == '/')
            return 1;
    }
    return (alen == 0 || a[alen - 1] == '/') && (blen == 0 || b[blen - 1] == '/');
}

static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash != NULL ? slash + 1 : path;
}

/* ------------------------------------------------------------------ state */

/* One candidate: a list entry that is a pure deletion (a source) or a pure
   addition (a destination), plus whatever has been loaded about it so far.
   Content is loaded at most once and thrown away as soon as it has been
   hashed -- the span table is all any comparison needs, and holding every
   candidate's bytes at once is what would make a big rename set unaffordable. */
typedef struct {
    size_t entry;        /* index into list->entries */
    unsigned char id[SG_SHA1_RAW_LEN];
    int id_ok;           /* 1 when the effective id was verified */
    int used;            /* source: claimed at least once; destination: paired */
    /* git's rename_used. A source that exists on both sides starts at 1
       (git increments when registering it), so every pairing off it leaves a
       use behind and reads as a copy; a deletion starts at 0. Each paired
       destination consumes one, in path order, at the very end. */
    int uses;
    int loaded;          /* 0 not yet, 1 loaded, -1 unreadable */
    sg_spanhash *hash;
    size_t size;
} rename_cand;

typedef struct {
    int src;   /* index into the source array, -1 when unused */
    int dst;   /* index into the destination array, -1 when unused */
    int score;
    int name_score;
    size_t seq; /* the slot's own position in the matrix; see slot_cmp_stable */
} rename_slot;

/* Loads and hashes one candidate's content, once. Returns 0 with the span
   table ready, -1 when the content could not be read, or -2 when an
   allocation failed.

   Those last two are deliberately NOT the same answer. Content that cannot
   be read is a candidate that cannot be scored, and the pair simply is not a
   rename -- the same failure direction as an unverified id. A failed
   allocation is not an answer at all, and must become the -1 that
   sg_diff_detect_renames is documented to return, or detection would quietly
   report FEWER renames than it should and call that success.

   sg_diff_side_read's -2 (a chunk pointer whose data is missing or corrupt)
   is folded into "cannot be read" on purpose, which is the one place this
   file departs from that function's usual contract. The reason that contract
   exists is that diffing a pointer's raw bytes produces meaningless hunks,
   and nothing here diffs anything -- and no diagnostic is lost, because the
   formats that read content read the same side again through diff_out.c,
   which does report it, while the formats that do not read content never
   report corruption for any path, renamed or not. */
static int load_cand(const char *git_dir, const char *repo_root,
                     const sg_diff_list *list, rename_cand *c, int is_src)
{
    const sg_diff_entry *e = &list->entries[c->entry];
    const sg_diff_side *side = is_src ? &e->old_side : &e->new_side;
    unsigned char *data = NULL;
    size_t len = 0;

    if (c->loaded != 0)
        return c->loaded == 1 ? 0 : c->loaded;

    if (sg_diff_side_read(git_dir, repo_root, e->path, side, &data, &len, NULL) != 0) {
        c->loaded = -1;
        return -1;
    }
    c->hash = sg_spanhash_build(data, len);
    free(data);
    if (c->hash == NULL) {
        c->loaded = -2;
        return -2;
    }
    c->size = len;
    c->loaded = 1;
    return 0;
}

/* git's estimate_similarity, in SG_SIMILARITY_MAX units -- or **-1 when an
   allocation failed**, which every caller must propagate instead of reading
   it as "not similar at all". A pair that merely cannot be read scores 0. */
static int score_pair(const char *git_dir, const char *repo_root,
                      const sg_diff_list *list, rename_cand *src,
                      rename_cand *dst, int min_score)
{
    const sg_diff_entry *se = &list->entries[src->entry];
    const sg_diff_entry *de = &list->entries[dst->entry];
    int rc;

    if (!mode_is_regular(se->old_side.mode) || !mode_is_regular(de->new_side.mode))
        return 0;

    /* Sizes decide it on their own often enough that git checks them before
       it will read either blob, and so does this. Loading is what makes the
       size known, though, so the cheap check can only skip the hashing. */
    rc = load_cand(git_dir, repo_root, list, src, 1);
    if (rc != 0)
        return rc == -2 ? -1 : 0;
    rc = load_cand(git_dir, repo_root, list, dst, 0);
    if (rc != 0)
        return rc == -2 ? -1 : 0;
    if (sg_similarity_size_rejects(src->size, dst->size, min_score))
        return 0;
    return sg_similarity_score(src->hash, src->size, dst->hash, dst->size);
}

/* ------------------------------------------------------- pass 1: exact id */

static void exact_pass(rename_cand *srcs, size_t nsrc, rename_cand *dsts,
                       size_t ndst, const sg_diff_list *list, int *pair_src,
                       int *pair_score, int detect_copies)
{
    size_t di;

    for (di = 0; di < ndst; di++) {
        rename_cand *d = &dsts[di];
        const sg_diff_entry *de = &list->entries[d->entry];
        int best = -1, best_score = -1, seen = 0;
        size_t si;

        if (!d->id_ok)
            continue;

        for (si = 0; si < nsrc; si++) {
            rename_cand *s = &srcs[si];
            const sg_diff_entry *se = &list->entries[s->entry];
            int score;

            /* Two ids that were never verified are not proof of identical
               content even when they are equal (sg_diff_side_effective_id's
               contract), so such a side never takes part. */
            if (!s->id_ok)
                continue;
            /* Outside copy detection a source is spent once it is claimed.
               Copy detection lets it be claimed again, but still prefers one
               that has not been -- that is what makes the score below 1
               rather than 0 for a fresh source. */
            if (s->used && !detect_copies)
                continue;
            if (memcmp(s->id, d->id, SG_SHA1_RAW_LEN) != 0)
                continue;
            /* Identical content with differing modes is still a rename as
               long as both are regular files -- a rename may chmod. */
            if (!mode_is_regular(se->old_side.mode) ||
                !mode_is_regular(de->new_side.mode)) {
                if (se->old_side.mode != de->new_side.mode)
                    continue;
            }

            /* Among identical sources git prefers the one that already has
               the destination's file name; otherwise the first in path
               order, which is what a strictly-greater comparison keeps. */
            score = (s->used ? 0 : 1) + basename_same(se->path, de->path);
            if (score > best_score) {
                best = (int)si;
                best_score = score;
                if (score == 2)
                    break;
            }
            if (++seen == SG_RENAME_EXACT_ALTERNATIVES)
                break;
        }
        if (best >= 0) {
            srcs[best].used = 1;
            srcs[best].uses++;
            d->used = 1;
            pair_src[di] = best;
            pair_score[di] = SG_SIMILARITY_MAX;
        }
    }
}

/* ---------------------------------------------------- pass 2: by basename */

typedef struct {
    const char *base;
    size_t slot;
} base_ref;

static int base_ref_cmp(const void *a_, const void *b_)
{
    const base_ref *a = a_;
    const base_ref *b = b_;
    int r = strcmp(a->base, b->base);

    if (r != 0)
        return r;
    return a->slot < b->slot ? -1 : a->slot > b->slot ? 1 : 0;
}

/* The slot owning `base`, but only when exactly one does. git keys a map on
   the basename and stores -1 once a second file claims it, then refuses to
   guess between them; a name nobody has and a name two files have are both
   "no answer", and both mean the same thing here. */
static int unique_slot(const base_ref *refs, size_t n, const char *base)
{
    size_t lo = 0, hi = n;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int r = strcmp(refs[mid].base, base);

        if (r < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == n || strcmp(refs[lo].base, base) != 0)
        return -1;
    if (lo + 1 < n && strcmp(refs[lo + 1].base, base) == 0)
        return -1;
    return (int)refs[lo].slot;
}

static int basename_pass(const char *git_dir, const char *repo_root,
                         const sg_diff_list *list, rename_cand *srcs,
                         size_t nsrc, rename_cand *dsts, size_t ndst,
                         int *pair_src, int *pair_score, int min_score)
{
    base_ref *sref = NULL, *dref = NULL;
    size_t sn = 0, dn = 0, i;
    /* git's min_basename_score: halfway between the requested threshold and
       a perfect match. A pair matched on its name alone has to be markedly
       more similar than one that had to win a comparison. */
    int min_basename = min_score + (int)(0.5 * (SG_SIMILARITY_MAX - min_score));

    sref = malloc((nsrc + 1) * sizeof(*sref));
    dref = malloc((ndst + 1) * sizeof(*dref));
    if (sref == NULL || dref == NULL) {
        free(sref);
        free(dref);
        return -1;
    }
    for (i = 0; i < nsrc; i++) {
        if (srcs[i].used)
            continue;
        sref[sn].base = basename_of(list->entries[srcs[i].entry].path);
        sref[sn].slot = i;
        sn++;
    }
    for (i = 0; i < ndst; i++) {
        if (dsts[i].used)
            continue;
        dref[dn].base = basename_of(list->entries[dsts[i].entry].path);
        dref[dn].slot = i;
        dn++;
    }
    qsort(sref, sn, sizeof(*sref), base_ref_cmp);
    qsort(dref, dn, sizeof(*dref), base_ref_cmp);

    /* Sources in path order, exactly as git walks them: the first source
       with a given name gets the destination of that name, and a later
       source can only find it already taken. */
    for (i = 0; i < nsrc; i++) {
        const char *base;
        int si, di, score;

        if (srcs[i].used)
            continue;
        base = basename_of(list->entries[srcs[i].entry].path);
        si = unique_slot(sref, sn, base);
        di = unique_slot(dref, dn, base);
        if (si != (int)i || di < 0)
            continue;
        if (dsts[di].used)
            continue;

        score = score_pair(git_dir, repo_root, list, &srcs[i], &dsts[di],
                           min_basename);
        if (score < 0) {
            free(sref);
            free(dref);
            return -1;
        }
        if (score < min_basename)
            continue;
        srcs[i].used = 1;
        srcs[i].uses++;
        dsts[di].used = 1;
        pair_src[di] = (int)i;
        pair_score[di] = score;
    }

    free(sref);
    free(dref);
    return 0;
}

/* ------------------------------------------------------ pass 3: full matrix */

/* git's score_compare, reproduced exactly, including the fact that it
   returns 0 for two slots nothing has been recorded into yet -- that zero is
   what makes git fill the four slots left to right, and which slot a
   candidate lands in is visible in the final ordering. */
static int slot_cmp_raw(const rename_slot *a, const rename_slot *b)
{
    if (a->dst < 0)
        return b->dst >= 0;
    if (b->dst < 0)
        return -1;
    if (a->score == b->score)
        return b->name_score - a->name_score;
    return b->score - a->score;
}

/* git sorts the matrix with a STABLE qsort, so pairs it calls equal keep
   their positions. qsort is not stable, so position is spelled out as the
   final tiebreak: `seq` is the slot's own index in the matrix and is kept
   when a candidate is written into the slot, never taken from the candidate.
   Ranking by the candidates' discovery order instead would give a different
   answer whenever an eviction has left a slot out of order. */
static int slot_cmp_stable(const void *a_, const void *b_)
{
    const rename_slot *a = a_;
    const rename_slot *b = b_;
    int r = slot_cmp_raw(a, b);

    if (r != 0)
        return r;
    return a->seq < b->seq ? -1 : a->seq > b->seq ? 1 : 0;
}

/* git's record_if_better: keep only the best few sources per destination,
   evicting the current worst and only when the newcomer beats it outright,
   so equal scores leave the earlier candidate in place. */
static void record_if_better(rename_slot *m, const rename_slot *o)
{
    int worst = 0, i;

    for (i = 1; i < SG_RENAME_CANDIDATES; i++)
        if (slot_cmp_raw(&m[i], &m[worst]) > 0)
            worst = i;
    if (slot_cmp_raw(&m[worst], o) > 0) {
        /* Field by field, deliberately, so that `seq` -- the slot's own
           position, and the whole basis of the stable ordering below -- is
           structurally incapable of being taken from the candidate. */
        m[worst].src = o->src;
        m[worst].dst = o->dst;
        m[worst].score = o->score;
        m[worst].name_score = o->name_score;
    }
}

/* git's find_renames, run once over the ranked matrix. `copies` is its
   parameter of the same name: the first walk refuses a source that is
   already claimed, so it produces real renames; copy detection then walks
   the very same ranking a second time with that refusal lifted. Running one
   walk that allowed reuse from the start would hand a destination to an
   already-claimed source before an unclaimed one had its turn. */
static void claim_from_matrix(const rename_slot *mx, size_t used_slots,
                              rename_cand *srcs, rename_cand *dsts, int *pair_src,
                              int *pair_score, int min_score, int copies)
{
    size_t i;

    for (i = 0; i < used_slots; i++) {
        int di = mx[i].dst, si = mx[i].src;

        /* Sorted best first, so the first slot below the threshold ends it. */
        if (di < 0 || mx[i].score < min_score)
            break;
        if (dsts[di].used)
            continue;
        if (!copies && srcs[si].used)
            continue;
        srcs[si].used = 1;
        srcs[si].uses++;
        dsts[di].used = 1;
        pair_src[di] = si;
        pair_score[di] = mx[i].score;
    }
}

static int matrix_pass(const char *git_dir, const char *repo_root,
                       const sg_diff_list *list, rename_cand *srcs,
                       size_t nsrc, rename_cand *dsts, size_t ndst,
                       int *pair_src, int *pair_score, int min_score,
                       int detect_copies)
{
    rename_slot *mx;
    size_t live_src = 0, live_dst = 0, i, j, used_slots = 0;

    for (i = 0; i < nsrc; i++)
        if (!srcs[i].used || detect_copies)
            live_src++;
    for (i = 0; i < ndst; i++)
        if (!dsts[i].used)
            live_dst++;
    if (live_src == 0 || live_dst == 0)
        return 0;
    /* git abandons inexact detection outright rather than spend the time,
       and the files stay an add and a delete. Reproduced so a huge rename
       set does not quietly diverge in the expensive direction. */
    if (live_dst * live_src > (size_t)SG_RENAME_LIMIT * SG_RENAME_LIMIT)
        return 0;

    mx = malloc(live_dst * SG_RENAME_CANDIDATES * sizeof(*mx));
    if (mx == NULL)
        return -1;

    for (i = 0; i < ndst; i++) {
        rename_slot *m;

        if (dsts[i].used)
            continue;
        m = &mx[used_slots];
        for (j = 0; j < SG_RENAME_CANDIDATES; j++) {
            m[j].src = -1;
            m[j].dst = -1;
            m[j].score = 0;
            m[j].name_score = 0;
            m[j].seq = used_slots + j; /* the slot's position, not a candidate's */
        }
        used_slots += SG_RENAME_CANDIDATES;

        for (j = 0; j < nsrc; j++) {
            rename_slot cand;

            /* A claimed source stays in the running under copy detection --
               the second walk above is what may hand it another
               destination, and it can only do that if it was ranked. */
            if (srcs[j].used && !detect_copies)
                continue;
            cand.score = score_pair(git_dir, repo_root, list, &srcs[j], &dsts[i],
                                    min_score);
            if (cand.score < 0) {
                free(mx);
                return -1;
            }
            cand.name_score = basename_same(list->entries[srcs[j].entry].path,
                                            list->entries[dsts[i].entry].path);
            cand.src = (int)j;
            cand.dst = (int)i;
            cand.seq = 0; /* never read: a slot keeps its own position */
            record_if_better(m, &cand);
        }
    }

    qsort(mx, used_slots, sizeof(*mx), slot_cmp_stable);

    claim_from_matrix(mx, used_slots, srcs, dsts, pair_src, pair_score, min_score, 0);
    if (detect_copies)
        claim_from_matrix(mx, used_slots, srcs, dsts, pair_src, pair_score, min_score, 1);

    free(mx);
    return 0;
}

/* --------------------------------------------------------------- driver */

int sg_diff_detect_renames(const char *git_dir, const char *repo_root,
                           sg_diff_list *list, int min_score, int detect_copies)
{
    rename_cand *srcs = NULL, *dsts = NULL;
    int *pair_src = NULL, *pair_score = NULL;
    char *claimed = NULL;
    char *is_copy = NULL;
    char **copied_path = NULL;
    size_t nsrc = 0, ndst = 0, i;
    int rc = -1, paired = 0;

    if (list == NULL || list->count == 0 || min_score <= 0)
        return 0;

    srcs = calloc(list->count, sizeof(*srcs));
    dsts = calloc(list->count, sizeof(*dsts));
    pair_src = malloc(list->count * sizeof(*pair_src));
    pair_score = malloc(list->count * sizeof(*pair_score));
    claimed = calloc(list->count, 1);
    is_copy = calloc(list->count, 1);
    copied_path = calloc(list->count, sizeof(*copied_path));
    if (srcs == NULL || dsts == NULL || pair_src == NULL || pair_score == NULL ||
        claimed == NULL || is_copy == NULL || copied_path == NULL)
        goto done;

    for (i = 0; i < list->count; i++) {
        const sg_diff_entry *e = &list->entries[i];
        rename_cand *c;
        const sg_diff_side *side;

        if (is_deletion(e)) {
            c = &srcs[nsrc++];
            side = &e->old_side;
        } else if (is_addition(e)) {
            c = &dsts[ndst++];
            side = &e->new_side;
        } else if (detect_copies && is_modification(e)) {
            c = &srcs[nsrc++];
            side = &e->old_side;
            /* git increments rename_used when it registers a source that is
               not a deletion, which is exactly what makes every pairing off
               such a source come out as a copy rather than a rename. */
            c->uses = 1;
        } else {
            continue;
        }
        c->entry = i;
        c->id_ok = sg_diff_side_effective_id(git_dir, side, c->id) == 0;
    }
    for (i = 0; i < ndst; i++) {
        pair_src[i] = -1;
        pair_score[i] = 0;
    }
    if (nsrc == 0 || ndst == 0) {
        rc = 0;
        goto done;
    }

    exact_pass(srcs, nsrc, dsts, ndst, list, pair_src, pair_score, detect_copies);

    /* -M100% asks for exact renames only, and lands here as a threshold no
       score short of a perfect one can reach. git stops rather than compute
       comparisons whose answers it would then throw away. */
    if (min_score < SG_SIMILARITY_MAX) {
        /* Copy detection skips the same-file-name shortcut entirely -- git
           does, and it is observable: the Phase 30 fixture built to prove
           that shortcut exists answers differently under -C. */
        if (!detect_copies &&
            basename_pass(git_dir, repo_root, list, srcs, nsrc, dsts, ndst,
                          pair_src, pair_score, min_score) != 0)
            goto done;
        if (matrix_pass(git_dir, repo_root, list, srcs, nsrc, dsts, ndst,
                        pair_src, pair_score, min_score, detect_copies) != 0)
            goto done;
    }

    /* Everything above only decided; nothing has touched the list yet, so a
       failure up to this point leaves it exactly as it was found. */
    /* Decided in two passes, and the split is the whole reason this function
       can still promise that -1 leaves the list untouched. A COPY has to
       duplicate the source's path -- the source row stays and keeps owning
       its own -- so unlike every other outcome here it can fail. Failing
       halfway through one combined pass would leave rows already rewritten
       and, worse, a source row whose path had been taken away. Pass one
       decides and allocates, touching nothing but local state; pass two
       writes the list and cannot fail.

       git spends one of the source's uses per destination, in path order,
       and calls the row a copy exactly while uses remain afterwards -- so
       with one source and two destinations the first is the copy and the
       second is the rename, however much better the second matched. dsts is
       filled in list order, so iterating it IS path order. */
    for (i = 0; i < ndst; i++) {
        if (pair_src[i] < 0)
            continue;
        if (--srcs[pair_src[i]].uses > 0) {
            is_copy[i] = 1;
            copied_path[i] = strdup(list->entries[srcs[pair_src[i]].entry].path);
            if (copied_path[i] == NULL)
                goto done; /* rc is still -1, and the list is as we found it */
        }
    }

    for (i = 0; i < ndst; i++) {
        sg_diff_entry *d;
        sg_diff_entry *s;

        if (pair_src[i] < 0)
            continue;
        d = &list->entries[dsts[i].entry];
        s = &list->entries[srcs[pair_src[i]].entry];
        d->is_copy = is_copy[i];
        if (is_copy[i]) {
            d->old_path = copied_path[i];
            copied_path[i] = NULL; /* ownership handed to the entry */
        } else {
            /* The source's LAST use: its row is going away, so the path is
               handed over rather than copied. */
            d->old_path = s->path;
            s->path = NULL;
            claimed[srcs[pair_src[i]].entry] = 1;
        }
        d->old_side = s->old_side;
        d->score = sg_similarity_percent(pair_score[i]);
        paired = 1;
    }

    if (paired) {
        size_t write = 0;

        /* Drop the claimed sources. Which ones those are is recorded
           explicitly rather than inferred from the NULL path they were left
           with, so that an entry that arrived with no path -- which would be
           a bug elsewhere, but a silent one here -- is not quietly deleted
           along with them. Destinations keep the path they already had, so
           what is left is still sorted by path. */
        for (i = 0; i < list->count; i++) {
            if (claimed[i]) {
                /* Always NULL: a claimed entry is a source, and only
                   destinations ever get an old_path. Freed anyway for the
                   same reason sg_diff_list_filter does it -- so ownership
                   here does not depend on that argument staying true. */
                free(list->entries[i].old_path);
                continue;
            }
            if (write != i)
                list->entries[write] = list->entries[i];
            write++;
        }
        list->count = write;
    }
    rc = 0;

done:
    for (i = 0; i < nsrc; i++)
        sg_spanhash_free(srcs[i].hash);
    for (i = 0; i < ndst; i++)
        sg_spanhash_free(dsts[i].hash);
    free(srcs);
    free(dsts);
    free(pair_src);
    free(pair_score);
    free(claimed);
    free(is_copy);
    /* Whatever pass one allocated that pass two did not take ownership of:
       everything on the failure path, nothing on the happy one. */
    if (copied_path != NULL) {
        for (i = 0; i < ndst; i++)
            free(copied_path[i]);
        free(copied_path);
    }
    return rc;
}
