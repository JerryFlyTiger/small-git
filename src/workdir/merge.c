#include "sg/merge.h"

#include "sg/chunk.h"
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

static int segment_equal(const sg_diff_line *a, size_t as, size_t ae, const sg_diff_line *b,
                         size_t bs, size_t be)
{
    size_t na = ae - as;
    size_t nb = be - bs;
    size_t k;

    if (na != nb)
        return 0;
    for (k = 0; k < na; k++) {
        if (!sg_diff_lines_equal(a[as + k], b[bs + k]))
            return 0;
    }
    return 1;
}

/* Backtracks an LCS table (built over a as rows, b as columns) into a
   match array of size na: match[i] = the b-index a[i] was aligned to, or -1
   if a[i] wasn't part of the alignment. This is exactly the walk `sg diff`
   uses to classify +/-/context lines, reused here to find base<->ours and
   base<->theirs alignments. Returns NULL on allocation failure. */
static long *lcs_matches(const sg_diff_line *a, size_t na, const sg_diff_line *b, size_t nb,
                        size_t **dp)
{
    long *match = malloc((na > 0 ? na : 1) * sizeof(*match));
    size_t i, j;

    if (match == NULL)
        return NULL;
    for (i = 0; i < na; i++)
        match[i] = -1;

    i = 0;
    j = 0;
    while (i < na && j < nb) {
        if (sg_diff_lines_equal(a[i], b[j])) {
            match[i] = (long)j;
            i++;
            j++;
        } else if (dp[i + 1][j] >= dp[i][j + 1]) {
            i++;
        } else {
            j++;
        }
    }
    return match;
}

typedef struct {
    long base_idx, ours_idx, theirs_idx;
} sync_point;

int sg_merge_content(const unsigned char *base, size_t base_len, const unsigned char *ours,
                     size_t ours_len, const unsigned char *theirs, size_t theirs_len,
                     const char *ours_label, const char *theirs_label, unsigned char **out,
                     size_t *out_len)
{
    size_t nbase = 0, nours = 0, ntheirs = 0;
    sg_diff_line *base_lines = NULL;
    sg_diff_line *ours_lines = NULL;
    sg_diff_line *theirs_lines = NULL;
    size_t **dp_bo = NULL;
    size_t **dp_bt = NULL;
    long *bo_match = NULL;
    long *bt_match = NULL;
    sync_point *syncs = NULL;
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

    dp_bo = sg_diff_lcs_table(base_lines, nbase, ours_lines, nours);
    dp_bt = sg_diff_lcs_table(base_lines, nbase, theirs_lines, ntheirs);
    if (dp_bo == NULL || dp_bt == NULL)
        goto done;

    bo_match = lcs_matches(base_lines, nbase, ours_lines, nours, dp_bo);
    bt_match = lcs_matches(base_lines, nbase, theirs_lines, ntheirs, dp_bt);
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

        if (ours_eq_base) {
            if (bytebuf_append_lines(&outbuf, theirs_lines, ts, te) != 0)
                goto done;
        } else if (theirs_eq_base) {
            if (bytebuf_append_lines(&outbuf, ours_lines, os, oe) != 0)
                goto done;
        } else if (ours_eq_theirs) {
            if (bytebuf_append_lines(&outbuf, ours_lines, os, oe) != 0)
                goto done;
        } else {
            conflict = 1;
            if (bytebuf_append_str(&outbuf, "<<<<<<< ") != 0 ||
               bytebuf_append_str(&outbuf, ours_label) != 0 ||
               bytebuf_append_str(&outbuf, "\n") != 0 ||
               bytebuf_append_lines(&outbuf, ours_lines, os, oe) != 0 ||
               bytebuf_append_str(&outbuf, "=======\n") != 0 ||
               bytebuf_append_lines(&outbuf, theirs_lines, ts, te) != 0 ||
               bytebuf_append_str(&outbuf, ">>>>>>> ") != 0 ||
               bytebuf_append_str(&outbuf, theirs_label) != 0 ||
               bytebuf_append_str(&outbuf, "\n") != 0)
                goto done;
        }

        if (idx < sync_count - 1) {
            /* sg_diff_lines_equal ignores has_nl, so a sync point's base line
               may carry a has_nl that doesn't match the position it's being
               emitted at (e.g. base's final, newline-less line matching
               identical content earlier in ours/theirs). Only trust base's
               own has_nl when this anchor is truly the last line of all
               three inputs; otherwise more content follows, so force a
               newline to avoid gluing lines together. */
            sg_diff_line anchor = base_lines[cur.base_idx];
            int is_final_line = cur.base_idx == (long)nbase - 1 &&
                                cur.ours_idx == (long)nours - 1 &&
                                cur.theirs_idx == (long)ntheirs - 1;

            if (!is_final_line)
                anchor.has_nl = 1;
            if (bytebuf_append_line(&outbuf, anchor) != 0)
                goto done;
        }
    }

    *out = outbuf.buf;
    *out_len = outbuf.len;
    rc = conflict;

done:
    if (rc < 0)
        free(outbuf.buf);
    free(syncs);
    free(bo_match);
    free(bt_match);
    sg_diff_lcs_free_table(dp_bo, nbase);
    sg_diff_lcs_free_table(dp_bt, nbase);
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

/* Handles the "all three present" (modify/modify) and "base absent, both
   added" (add/add) conflict candidates by calling sg_merge_content; a clean
   result (rc 0) turns out not to be a conflict after all and gets a freshly
   written blob. */
static int resolve_via_content_merge(const char *git_dir, const triple *t, const char *ours_label,
                                     const char *theirs_label, sg_merge_result_entry *out)
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

    if (t->base_present &&
       read_blob_chunk_aware(git_dir, t->base_e->sha1, t->path, &base_content, &base_len) != 0)
        return -1;
    if (read_blob_chunk_aware(git_dir, t->ours_e->sha1, t->path, &ours_content, &ours_len) != 0) {
        free(base_content);
        return -1;
    }
    if (read_blob_chunk_aware(git_dir, t->theirs_e->sha1, t->path, &theirs_content, &theirs_len) !=
       0) {
        free(base_content);
        free(ours_content);
        return -1;
    }

    rc = sg_merge_content(base_content, base_len, ours_content, ours_len, theirs_content, theirs_len,
                          ours_label, theirs_label, &merged, &merged_len);
    free(base_content);
    free(ours_content);
    free(theirs_content);
    if (rc < 0)
        return -1;

    if (rc == 0) {
        unsigned char new_sha1[SG_SHA1_RAW_LEN];
        int enabled = 0;
        size_t threshold = SG_CHUNK_DEFAULT_THRESHOLD;
        int chunked;

        /* A clean multi-line merge can itself produce a file large enough to
           deserve chunking (e.g. two branches each touched a different part
           of a multi-megabyte text file) -- store it the same chunk-aware
           way `sg add` does, using this repo's own configured threshold, so
           the merge result isn't silently forced back into full-size,
           non-deduplicated storage. */
        sg_repo_read_chunk_config(git_dir, &enabled, &threshold);
        if (enabled) {
            if (sg_chunk_store_blob(git_dir, merged, merged_len, threshold, new_sha1, &chunked) !=
               0) {
                free(merged);
                return -1;
            }
        } else if (sg_loose_write(git_dir, SG_OBJ_BLOB, merged, merged_len, new_sha1) != 0) {
            free(merged);
            return -1;
        }
        free(merged);
        out->conflict = 0;
        out->deleted = 0;
        memcpy(out->sha1, new_sha1, SG_SHA1_RAW_LEN);
        if (t->base_present) {
            if (t->ours_e->mode != t->base_e->mode)
                out->mode = t->ours_e->mode;
            else if (t->theirs_e->mode != t->base_e->mode)
                out->mode = t->theirs_e->mode;
            else
                out->mode = t->base_e->mode;
        } else {
            out->mode = t->ours_e->mode;
        }
        return 0;
    }

    out->conflict_content = merged;
    out->conflict_content_len = merged_len;
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

int sg_merge_trees(const char *git_dir, const unsigned char base_tree[SG_SHA1_RAW_LEN],
                   const unsigned char ours_tree[SG_SHA1_RAW_LEN],
                   const unsigned char theirs_tree[SG_SHA1_RAW_LEN], const char *ours_label,
                   const char *theirs_label, sg_merge_result *out)
{
    sg_flat_list base_flat, ours_flat, theirs_flat;
    size_t bi = 0, oi = 0, ti = 0;
    size_t cap = 0;
    int rc = 0;
    char bad_path[SG_PATH_MAX];
    int flatten_rc;

    memset(out, 0, sizeof(*out));
    memset(&base_flat, 0, sizeof(base_flat));
    memset(&ours_flat, 0, sizeof(ours_flat));
    memset(&theirs_flat, 0, sizeof(theirs_flat));

    flatten_rc = sg_tree_flatten(git_dir, base_tree, &base_flat, bad_path);
    if (flatten_rc == -2) {
        fprintf(stderr, "sg: 路徑 %s 無效,拒絕將這棵 tree 展開成檔案路徑\n",
               sg_quote_path_delimited(bad_path));
        return -1;
    }
    if (flatten_rc != 0)
        return -1;
    flatten_rc = sg_tree_flatten(git_dir, ours_tree, &ours_flat, bad_path);
    if (flatten_rc == -2) {
        fprintf(stderr, "sg: 路徑 %s 無效,拒絕將這棵 tree 展開成檔案路徑\n",
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
        fprintf(stderr, "sg: 路徑 %s 無效,拒絕將這棵 tree 展開成檔案路徑\n",
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

    while (bi < base_flat.count || oi < ours_flat.count || ti < theirs_flat.count) {
        const char *bp = bi < base_flat.count ? base_flat.entries[bi].path : NULL;
        const char *op = oi < ours_flat.count ? ours_flat.entries[oi].path : NULL;
        const char *tp = ti < theirs_flat.count ? theirs_flat.entries[ti].path : NULL;
        const char *path = min_path3(bp, op, tp);
        triple t;
        sg_merge_result_entry entry;

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
        fprintf(stderr, "sg: 路徑過長,無法解析 %s\n", sg_quote_path_delimited(path));
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
            fprintf(stderr, "sg: 路徑過長,無法處理 %s\n", sg_quote_path_delimited(e->path));
            content_missing = 1;
            continue;
        }

        if (e->conflict) {
            int mode = e->ours_present ? (int)(e->ours_mode & 0777)
                                       : (e->theirs_present ? (int)(e->theirs_mode & 0777) : 0644);
            char **grown;

            if (sg_write_file_mkdirs(abspath, e->conflict_content, e->conflict_content_len, mode) != 0)
                fprintf(stderr, "sg: failed to write conflicted %s\n", sg_quote_path_delimited(e->path));

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
               sits at abspath). */
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
