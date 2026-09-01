#include "sg/diff.h"

#include "sg/chunk.h"
#include "sg/hash.h"
#include "sg/index.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void sg_diff_list_free(sg_diff_list *list)
{
    size_t i;

    for (i = 0; i < list->count; i++) {
        free(list->entries[i].path);
        free(list->entries[i].old_path);
    }
    free(list->entries);
    list->entries = NULL;
    list->count = 0;
    list->cap = 0;
}

/* index/mode-line plumbing and rename detection share this: git's "index <old>..<new>" line and
   an entry's "old/new mode" lines want the id/mode an ordinary tree/index
   entry would carry, resolved off whichever kind of side actually produced
   it. See sg/diff.h's sg_diff_side contract for why BLOB and WORKDIR need
   different treatment here.

   Always fills *out with SOMETHING displayable, falling back to the side's
   raw id when resolution fails, but returns 0 only when that id is actually
   verified -- -1 means "could not verify" (a broken/missing chunk pointer).
   That distinction matters to the caller beyond cosmetics: two "unverified"
   ids that happen to come out byte-equal are NOT proof the content matches,
   and neither print_patch's mode-only row nor sg_diff_detect_renames'
   pairing may be decided on that basis --
   doing so would silently skip the sg_diff_side_read call that is the only
   place this failure gets reported to the user (measured: the
   append_index_entry_vs_workdir builder deliberately force-appends a row
   whenever chunk resolution fails, precisely so the renderer gets a chance
   to hit and report the same failure with the path in hand). */
int sg_diff_side_effective_id(const char *git_dir, const sg_diff_side *side,
                             unsigned char out[SG_SHA1_RAW_LEN])
{
    if (side->kind == SG_DIFF_SIDE_BLOB) {
        if (sg_chunk_effective_id(git_dir, side->id, out) == 0)
            return 0;
        memcpy(out, side->id, SG_SHA1_RAW_LEN);
        return -1;
    }
    if (side->kind == SG_DIFF_SIDE_WORKDIR) {
        memcpy(out, side->id, SG_SHA1_RAW_LEN);
        return 0;
    }
    /* ABSENT: git's "0000000" -- the hex of an all-zero id happens to start
       with 7 zeros, so no special-casing is needed at the print site. */
    memset(out, 0, SG_SHA1_RAW_LEN);
    return 0;
}

void sg_diff_list_filter(sg_diff_list *list, const sg_pathspec *ps)
{
    size_t read;
    size_t write = 0;

    if (list == NULL || ps == NULL || ps->count == 0)
        return;

    for (read = 0; read < list->count; read++) {
        if (sg_pathspec_matches(ps, list->entries[read].path)) {
            if (write != read)
                list->entries[write] = list->entries[read];
            write++;
        } else {
            free(list->entries[read].path);
            /* Always NULL in practice -- filtering is defined to run before
               rename detection (see sg_diff_detect_renames) -- but owning it
               here keeps the two passes independent of each other's order
               for the purpose of who frees what. */
            free(list->entries[read].old_path);
        }
    }
    list->count = write;
}

int sg_diff_reorder_combined_first(sg_diff_list *list)
{
    sg_diff_entry *reordered;
    size_t i, w = 0;

    if (list->count == 0)
        return 0;
    reordered = malloc(list->count * sizeof(*reordered));
    if (reordered == NULL)
        return -1;
    for (i = 0; i < list->count; i++)
        if (sg_diff_entry_is_combined(&list->entries[i]))
            reordered[w++] = list->entries[i];
    for (i = 0; i < list->count; i++)
        if (!sg_diff_entry_is_combined(&list->entries[i]))
            reordered[w++] = list->entries[i];
    free(list->entries);
    list->entries = reordered;
    list->cap = list->count;
    return 0;
}

static sg_diff_side side_absent(void);

static int list_append(sg_diff_list *list, const char *path, const sg_diff_side *old_side,
                       const sg_diff_side *new_side)
{
    if (list->count == list->cap) {
        size_t new_cap = list->cap == 0 ? 8 : list->cap * 2;
        sg_diff_entry *grown = realloc(list->entries, new_cap * sizeof(*grown));

        if (grown == NULL)
            return -1;
        list->entries = grown;
        list->cap = new_cap;
    }
    list->entries[list->count].path = strdup(path);
    if (list->entries[list->count].path == NULL)
        return -1;
    list->entries[list->count].old_path = NULL;
    list->entries[list->count].score = 0;
    /* Set even though nothing reads it without old_path first: the storage
       comes from realloc, so leaving it out means every entry in the codebase
       carries garbage here, and the next reader to check it on its own would
       be reading uninitialized memory that no gate this project has can see
       (ASan is not MSan). Phase 29 lost a day to the same shape from the
       other direction -- fixtures built without the factory. */
    list->entries[list->count].is_copy = 0;
    list->entries[list->count].old_side = *old_side;
    list->entries[list->count].new_side = *new_side;
    list->entries[list->count].unmerged = 0;
    /* ABSENT for every builder except sg_diff_index_workdir, which
       overwrites these on the unmerged row it just appended -- see
       sg/diff.h's sg_diff_entry contract. */
    list->entries[list->count].ours = side_absent();
    list->entries[list->count].theirs = side_absent();
    list->entries[list->count].result = side_absent();
    /* Overwritten by callers that append an unchanged row for -C -C's copy
       source pool (Phase 51) -- see sg/diff.h's sg_diff_entry contract. */
    list->entries[list->count].unchanged = 0;
    /* Overwritten only by sg_diff_combined_from_trees (Phase 55b) -- see
       sg/diff.h's sg_diff_entry contract. */
    list->entries[list->count].combined_row = 0;
    list->count++;
    return 0;
}

static sg_diff_side side_absent(void)
{
    sg_diff_side s;

    memset(&s, 0, sizeof(s));
    s.kind = SG_DIFF_SIDE_ABSENT;
    return s;
}

static sg_diff_side side_blob(unsigned int mode, const unsigned char id[SG_SHA1_RAW_LEN])
{
    sg_diff_side s;

    memset(&s, 0, sizeof(s));
    s.kind = SG_DIFF_SIDE_BLOB;
    s.mode = mode;
    memcpy(s.id, id, SG_SHA1_RAW_LEN);
    return s;
}

/* mode is expected already normalized to 100644/100755 by
   workdir_entry_mode() below; id is the file's own content hash
   (sg_hash_file_blob), reused rather than recomputed -- see sg/diff.h's
   sg_diff_side contract for why that is already the "effective" id for a
   WORKDIR side. */
static sg_diff_side side_workdir(const unsigned char id[SG_SHA1_RAW_LEN], unsigned int mode)
{
    sg_diff_side s;

    memset(&s, 0, sizeof(s));
    s.kind = SG_DIFF_SIDE_WORKDIR;
    s.mode = mode;
    if (id != NULL)
        memcpy(s.id, id, SG_SHA1_RAW_LEN);
    return s;
}

int sg_diff_entry_is_combined(const sg_diff_entry *e)
{
    if (e->combined_row)
        return 1;
    if (e->ours.kind == SG_DIFF_SIDE_ABSENT || e->theirs.kind == SG_DIFF_SIDE_ABSENT)
        return 0;
    if (e->unmerged)
        return 1;
    return e->result.kind != SG_DIFF_SIDE_ABSENT;
}

void sg_diff_fill_combined_from_index(const sg_index *idx, sg_diff_list *list)
{
    size_t i;

    for (i = 0; i < list->count; i++) {
        sg_diff_entry *e = &list->entries[i];
        int stage;
        sg_diff_side ours = side_absent();

        /* Phase 51: an unchanged row exists only to be offered to copy
           detection as a source, and filling it here would make
           sg_diff_entry_is_combined answer yes for it -- rename.c's three
           source predicates all refuse a combined row, so the row would be
           silently dropped from the source pool and the copy never found.
           Measured: `sg diff -c -C -C <rev>` printed `A copy.txt` where real
           git prints `C094 src.txt copy.txt`, while plain `-C -C` and plain
           `-c` were each correct on their own. */
        if (e->unchanged)
            continue;

        /* Lowest stage present, not "stage 1" -- an add/add conflict has no
           stage 1 at all and git falls back to stage 2 there (SPEC section
           2, fixture C). Stage 0 is the ordinary, unconflicted case, which
           is why it is checked first and covers almost every row. */
        for (stage = 0; stage <= 3; stage++) {
            int pos = sg_index_find_stage(idx, e->path, (unsigned int)stage);

            if (pos >= 0) {
                ours = side_blob(idx->entries[pos].mode, idx->entries[pos].sha1);
                break;
            }
        }
        e->ours = ours;
        e->theirs = e->old_side;
        e->result = e->new_side;
    }
}

/* Normalizes path's on-disk permission bits to a tree/index-entry mode.
   Deliberately lstat, not stat: a following stat() would report the
   *target's* mode for a symlink, which this codebase has no way to act on
   correctly here (a symlink has no exec bit of its own to read, and 120000
   is out of scope this round -- see sg/diff.h).

   The exec bit is only read off a *regular* file. Anything else -- a
   symlink, a directory, a fifo standing where a tracked file should be, or
   a path lstat can't reach at all -- falls back to 100644. Those all carry
   permission bits that mean something other than "git should record this
   as executable", and a directory in particular is almost always exec for
   reasons that have nothing to do with the blob that was supposed to be
   there; reading its 0755 would put a mode in the patch header that no
   content ever justified. */
static unsigned int workdir_entry_mode(const char *abspath)
{
    struct stat lst;

    if (lstat(abspath, &lst) != 0 || !S_ISREG(lst.st_mode))
        return 0100644;
    return (lst.st_mode & S_IXUSR) ? 0100755 : 0100644;
}

/* Appends the fixed "unresolved conflict" row: both sides ABSENT, unmerged
   set. Shared by sg_diff_tree_index and sg_diff_index_workdir -- see
   sg/diff.h for what each measured against real git. */
static int list_append_unmerged(sg_diff_list *list, const char *path)
{
    sg_diff_side absent = side_absent();
    int rc;

    rc = list_append(list, path, &absent, &absent);
    if (rc == 0)
        list->entries[list->count - 1].unmerged = 1;
    return rc;
}

/* Two sides differ if their ids differ, or if both modes are known and
   differ -- an unknown (0) mode on either side skips the mode comparison, per
   sg/diff.h's sg_diff_side contract. Despite the name, this is also used to
   compare a BLOB side against a WORKDIR side (both now carry a real mode as
   of Phase 26) -- id/mode are read the same way off either kind. */
static int blob_sides_differ(const sg_diff_side *a, const sg_diff_side *b)
{
    if (memcmp(a->id, b->id, SG_SHA1_RAW_LEN) != 0)
        return 1;
    if (a->mode != 0 && b->mode != 0 && a->mode != b->mode)
        return 1;
    return 0;
}

static int flatten_or_empty(const char *git_dir, const unsigned char *tree_id, sg_flat_list *out,
                            char *bad_path)
{
    if (tree_id == NULL) {
        out->entries = NULL;
        out->count = 0;
        return 0;
    }
    return sg_tree_flatten(git_dir, tree_id, out, bad_path);
}

/* idx is sorted by (path, stage) -- sg/index.h's invariant -- so every entry
   sharing one path is contiguous, and stage 0 (if present at all) is always
   the first of the group. Returns the index one past the last entry sharing
   idx->entries[i].path.

   Every caller of this function (and of the "does this group start with
   stage 0" check right before each call) assumes a path's group is either
   ALL stage 0 or ALL stage 1/2/3, never a mix. That holds for any index sg
   itself writes: cmd_add.c always calls sg_index_remove_all_stages before
   upserting a fresh stage-0 entry (src/cli/cmd_add.c:151), so resolving a
   conflict at a path removes every stage 1/2/3 entry there before stage 0
   is (re)written. It is NOT enforced by sg_index_read or by this file --
   a hand-edited or corrupted .git/index that keeps both a stage-0 entry and
   leftover stage 1/2/3 entries for the same path will have the leftover
   entries silently skipped (index_group_end folds stage 0 in with whatever
   nonzero-stage entries happen to sort after it, and every caller treats
   the group as "stage 0, ordinary" once it sees a stage-0 entry first).
   That failure direction is safe but incomplete -- no crash, no path
   misreported as deleted or invented as unmerged, just those extra stale
   entries never surfacing in the diff -- so this is a deliberate choice not
   to add handling for a case sg's own write path cannot produce, not a
   missed check. */
static size_t index_group_end(const sg_index *idx, size_t i)
{
    const char *path = idx->entries[i].path;
    size_t j = i;

    while (j < idx->count && strcmp(idx->entries[j].path, path) == 0)
        j++;
    return j;
}

/* Phase 52: the union-walk body of sg_diff_trees, extracted so a caller that
   already has both trees flattened (sg_merge_trees' build_rename_map, as of
   this phase) does not have to pay for a redundant re-flatten. old_flat/
   new_flat are BORROWED -- this function never frees them, the caller must
   keep them alive until it returns -- and NULL means "the empty tree",
   exactly as old_tree/new_tree == NULL means to sg_diff_trees itself. Unlike
   sg_diff_trees, this can never return -2: whoever flattened the trees
   already owns reporting that failure, bad_path is not this function's to
   fill. Returns 0 or -1. */
int sg_diff_from_flat_lists(const sg_flat_list *old_flat, const sg_flat_list *new_flat,
                            sg_diff_list *out, int include_unchanged)
{
    static const sg_flat_list empty_flat = { NULL, 0 };
    const sg_flat_list *ol = old_flat != NULL ? old_flat : &empty_flat;
    const sg_flat_list *nl = new_flat != NULL ? new_flat : &empty_flat;
    size_t oi = 0;
    size_t ni = 0;
    int rc;

    memset(out, 0, sizeof(*out));

    while (oi < ol->count || ni < nl->count) {
        int cmp;

        if (oi >= ol->count)
            cmp = 1;
        else if (ni >= nl->count)
            cmp = -1;
        else
            cmp = strcmp(ol->entries[oi].path, nl->entries[ni].path);

        if (cmp == 0) {
            sg_diff_side os = side_blob(ol->entries[oi].mode, ol->entries[oi].sha1);
            sg_diff_side ns = side_blob(nl->entries[ni].mode, nl->entries[ni].sha1);

            if (blob_sides_differ(&os, &ns)) {
                if (list_append(out, ol->entries[oi].path, &os, &ns) != 0) {
                    rc = -1;
                    goto done;
                }
            } else if (include_unchanged) {
                if (list_append(out, ol->entries[oi].path, &os, &ns) != 0) {
                    rc = -1;
                    goto done;
                }
                out->entries[out->count - 1].unchanged = 1;
            }
            oi++;
            ni++;
        } else if (cmp < 0) {
            sg_diff_side os = side_blob(ol->entries[oi].mode, ol->entries[oi].sha1);
            sg_diff_side ns = side_absent();

            if (list_append(out, ol->entries[oi].path, &os, &ns) != 0) {
                rc = -1;
                goto done;
            }
            oi++;
        } else {
            sg_diff_side os = side_absent();
            sg_diff_side ns = side_blob(nl->entries[ni].mode, nl->entries[ni].sha1);

            if (list_append(out, nl->entries[ni].path, &os, &ns) != 0) {
                rc = -1;
                goto done;
            }
            ni++;
        }
    }
    rc = 0;

done:
    if (rc != 0)
        sg_diff_list_free(out);
    return rc;
}

int sg_diff_trees(const char *git_dir, const unsigned char *old_tree,
                  const unsigned char *new_tree, sg_diff_list *out, char *bad_path,
                  int include_unchanged)
{
    sg_flat_list old_flat;
    sg_flat_list new_flat;
    int rc;

    rc = flatten_or_empty(git_dir, old_tree, &old_flat, bad_path);
    if (rc != 0)
        return rc;
    rc = flatten_or_empty(git_dir, new_tree, &new_flat, bad_path);
    if (rc != 0) {
        sg_flat_list_free(&old_flat);
        return rc;
    }

    rc = sg_diff_from_flat_lists(&old_flat, &new_flat, out, include_unchanged);

    sg_flat_list_free(&old_flat);
    sg_flat_list_free(&new_flat);
    return rc;
}

/* Finds `path`'s entry in a flat list (sorted by path) starting the linear
   scan at *cursor, advancing *cursor past it. Every caller of this in
   sg_diff_combined_from_trees visits paths in sorted order exactly once
   each, so the whole union walk is O(paths * (parent_count + 1)) rather
   than O(paths * (parent_count + 1) * log n). Returns the entry, or NULL if
   this list has no entry for `path`. */
static const sg_flat_entry *flat_advance_to(const sg_flat_list *list, size_t *cursor,
                                            const char *path)
{
    while (*cursor < list->count && strcmp(list->entries[*cursor].path, path) < 0)
        (*cursor)++;
    if (*cursor < list->count && strcmp(list->entries[*cursor].path, path) == 0)
        return &list->entries[*cursor];
    return NULL;
}

int sg_diff_combined_from_trees(const char *git_dir,
                                const unsigned char (*parent_trees)[SG_SHA1_RAW_LEN],
                                size_t parent_count,
                                const unsigned char result_tree[SG_SHA1_RAW_LEN],
                                sg_diff_list *out, size_t *row_count, char *bad_path)
{
    sg_flat_list *parents;
    sg_flat_list result_flat;
    size_t *parent_cursor;
    size_t result_cursor = 0;
    size_t i;
    int rc = 0;
    int result_flattened = 0;

    memset(out, 0, sizeof(*out));
    *row_count = 0;

    parents = calloc(parent_count, sizeof(*parents));
    parent_cursor = calloc(parent_count, sizeof(*parent_cursor));
    if (parents == NULL || parent_cursor == NULL) {
        free(parents);
        free(parent_cursor);
        return -1;
    }

    for (i = 0; i < parent_count; i++) {
        rc = sg_tree_flatten(git_dir, parent_trees[i], &parents[i], bad_path);
        if (rc != 0)
            goto done;
    }
    rc = sg_tree_flatten(git_dir, result_tree, &result_flat, bad_path);
    if (rc != 0)
        goto done;
    result_flattened = 1;

    for (;;) {
        const char *min_path = NULL;

        for (i = 0; i < parent_count; i++) {
            size_t c = parent_cursor[i];

            if (c < parents[i].count &&
               (min_path == NULL || strcmp(parents[i].entries[c].path, min_path) < 0))
                min_path = parents[i].entries[c].path;
        }
        if (result_cursor < result_flat.count &&
           (min_path == NULL || strcmp(result_flat.entries[result_cursor].path, min_path) < 0))
            min_path = result_flat.entries[result_cursor].path;
        if (min_path == NULL)
            break;

        {
            /* Own a copy: `min_path` currently points into one of the flat
               lists we are about to advance cursors through below, and
               list_append itself may reallocate `out->entries`, neither of
               which may outlive the pointer. */
            char path[SG_PATH_MAX];
            const sg_flat_entry *result_e;
            /* Captured for parent_count == 2's sake as the loop below walks
               every parent anyway -- avoids a second lookup per parent. Only
               meaningful once the loop has actually reached that index,
               which "differs_from_all still 1" guarantees for every i (a
               `break` on a match would short-circuit it, but that also
               means the row is excluded and p0e/p1e are never used). */
            const sg_flat_entry *p0e = NULL;
            const sg_flat_entry *p1e = NULL;
            int differs_from_all = 1;

            if (strlen(min_path) >= sizeof(path)) {
                rc = -1;
                goto done;
            }
            strcpy(path, min_path);

            result_e = flat_advance_to(&result_flat, &result_cursor, path);

            for (i = 0; i < parent_count; i++) {
                const sg_flat_entry *pe = flat_advance_to(&parents[i], &parent_cursor[i], path);
                int equal;

                if (i == 0)
                    p0e = pe;
                else if (i == 1)
                    p1e = pe;

                if (pe == NULL && result_e == NULL)
                    equal = 1;
                else if (pe == NULL || result_e == NULL)
                    equal = 0;
                else
                    equal = pe->mode == result_e->mode &&
                           memcmp(pe->sha1, result_e->sha1, SG_SHA1_RAW_LEN) == 0;
                if (equal) {
                    differs_from_all = 0;
                    break;
                }
            }

            if (differs_from_all) {
                (*row_count)++;
                if (parent_count == 2) {
                    sg_diff_side p0_side = p0e != NULL ? side_blob(p0e->mode, p0e->sha1) : side_absent();
                    sg_diff_side p1_side = p1e != NULL ? side_blob(p1e->mode, p1e->sha1) : side_absent();
                    sg_diff_side result_side = result_e != NULL
                                                    ? side_blob(result_e->mode, result_e->sha1)
                                                    : side_absent();

                    if (list_append(out, path, &p0_side, &result_side) != 0) {
                        rc = -1;
                        goto done;
                    }
                    out->entries[out->count - 1].ours = p0_side;
                    out->entries[out->count - 1].theirs = p1_side;
                    out->entries[out->count - 1].result = result_side;
                    out->entries[out->count - 1].combined_row = 1;
                }
            }
        }

        for (i = 0; i < parent_count; i++) {
            size_t c = parent_cursor[i];

            if (c < parents[i].count && strcmp(parents[i].entries[c].path, min_path) == 0)
                parent_cursor[i]++;
        }
        if (result_cursor < result_flat.count &&
           strcmp(result_flat.entries[result_cursor].path, min_path) == 0)
            result_cursor++;
    }
    rc = 0;

done:
    for (i = 0; i < parent_count; i++)
        sg_flat_list_free(&parents[i]);
    free(parents);
    free(parent_cursor);
    if (result_flattened)
        sg_flat_list_free(&result_flat);
    if (rc != 0)
        sg_diff_list_free(out);
    return rc;
}

int sg_diff_tree_index(const char *git_dir, const unsigned char *old_tree,
                       const sg_index *idx, sg_diff_list *out, char *bad_path,
                       int include_unchanged)
{
    sg_flat_list old_flat;
    size_t oi = 0;
    size_t ii = 0;
    int rc;

    memset(out, 0, sizeof(*out));

    rc = flatten_or_empty(git_dir, old_tree, &old_flat, bad_path);
    if (rc != 0)
        return rc;

    while (oi < old_flat.count || ii < idx->count) {
        int cmp;
        const char *idx_path = NULL;
        int idx_unmerged = 0;
        size_t group_end = ii;

        /* idx's cursor advances by whole path-groups: an unresolved conflict
           (stage 0 absent, only 1/2/3 present) has no single staged blob, so
           it is never compared entry-by-entry the way a stage-0 entry is --
           it always yields the single fixed "unmerged" row instead, per
           sg/diff.h. */
        if (ii < idx->count) {
            idx_path = idx->entries[ii].path;
            idx_unmerged = idx->entries[ii].stage != 0;
            group_end = index_group_end(idx, ii);
        }

        if (oi >= old_flat.count)
            cmp = 1;
        else if (ii >= idx->count)
            cmp = -1;
        else
            cmp = strcmp(old_flat.entries[oi].path, idx_path);

        if (cmp == 0) {
            if (idx_unmerged) {
                if (list_append_unmerged(out, idx_path) != 0) {
                    rc = -1;
                    goto done;
                }
            } else {
                sg_diff_side os = side_blob(old_flat.entries[oi].mode, old_flat.entries[oi].sha1);
                sg_diff_side ns = side_blob(idx->entries[ii].mode, idx->entries[ii].sha1);

                if (blob_sides_differ(&os, &ns)) {
                    if (list_append(out, idx_path, &os, &ns) != 0) {
                        rc = -1;
                        goto done;
                    }
                } else if (include_unchanged) {
                    if (list_append(out, idx_path, &os, &ns) != 0) {
                        rc = -1;
                        goto done;
                    }
                    out->entries[out->count - 1].unchanged = 1;
                }
            }
            oi++;
            ii = group_end;
        } else if (cmp < 0) {
            sg_diff_side os = side_blob(old_flat.entries[oi].mode, old_flat.entries[oi].sha1);
            sg_diff_side ns = side_absent();

            if (list_append(out, old_flat.entries[oi].path, &os, &ns) != 0) {
                rc = -1;
                goto done;
            }
            oi++;
        } else {
            if (idx_unmerged) {
                if (list_append_unmerged(out, idx_path) != 0) {
                    rc = -1;
                    goto done;
                }
            } else {
                sg_diff_side os = side_absent();
                sg_diff_side ns = side_blob(idx->entries[ii].mode, idx->entries[ii].sha1);

                if (list_append(out, idx_path, &os, &ns) != 0) {
                    rc = -1;
                    goto done;
                }
            }
            ii = group_end;
        }
    }
    rc = 0;

done:
    sg_flat_list_free(&old_flat);
    if (rc != 0)
        sg_diff_list_free(out);
    return rc;
}

/* Compares one index entry's blob (chunk-normalized) against the working
   tree file at its path, appending an ordinary (non-unmerged) row if they
   differ -- including "the file is gone" as a difference. Shared by the
   stage-0 path and by the stage-2-vs-workdir second row an unmerged path can
   produce (see sg/diff.h's sg_diff_index_workdir contract): both are exactly
   this same comparison, just against a different index entry. Returns 0 on
   success (whether or not a row was appended), -1 on failure. */
static int append_index_entry_vs_workdir(const char *git_dir, const char *repo_root,
                                         const sg_index_entry *entry, sg_diff_list *out)
{
    char abspath[SG_PATH_MAX];
    unsigned char wd_sha1[SG_SHA1_RAW_LEN];
    unsigned char effective_sha1[SG_SHA1_RAW_LEN];
    struct stat st;
    unsigned int wd_mode;
    sg_diff_side os = side_blob(entry->mode, entry->sha1);

    /* Phase 36: entry->path comes straight off a parsed .git/index entry,
       validated by nobody (sg_index_read validates nothing about index
       paths, by design -- see CLAUDE.md). Without this check a path like
       "../secret.txt" would be stat()/read through sg_path_join + the calls
       below, reading a file outside the repository into a diff/status row.
       The failure direction here is the SAME as "existing but unreadable"
       just below, not a hard failure of the whole call: `sg status` must
       still be able to list this path (git does too, as the staged half of
       the same diff, which never reaches this function at all), it just
       must never read ITS CONTENT. Treating it as ABSENT, exactly like a
       permission-denied read, achieves both at once without a new case. */
    if (!sg_relpath_is_safe(entry->path)) {
        sg_diff_side ns = side_absent();

        return list_append(out, entry->path, &os, &ns);
    }

    /* A truncated path must not be silently skipped: sg/diff.h and
       CLAUDE.md both require a hard failure here rather than quietly
       dropping a path from `sg diff`. */
    if (sg_path_join(abspath, sizeof(abspath), repo_root, entry->path) != 0)
        return -1;

    if (stat(abspath, &st) != 0) {
        sg_diff_side ns = side_absent();

        return list_append(out, entry->path, &os, &ns);
    }

    wd_mode = workdir_entry_mode(abspath);

    /* Existing-but-unreadable (permission denied, race with a delete, ...)
       is treated the same as "not there at all" -- ABSENT, not a WORKDIR
       side with a placeholder zero id. Same convention as
       sg_status_diff_unstaged (src/workdir/status.c), which reports this
       exact failure as SG_STATUS_DELETED. A placeholder zero id would
       instead print as a real BLOB/WORKDIR pair whose "new" id happens to be
       all-zero -- indistinguishable, at render time, from git's genuine
       0000000 (which only ever appears on an add/delete row, never with a
       mode suffix) -- see sg/diff.h's sg_diff_side contract. */
    if (sg_hash_file_blob(abspath, wd_sha1) != 0) {
        sg_diff_side ns = side_absent();

        return list_append(out, entry->path, &os, &ns);
    }

    /* idx's id may be a chunked-storage pointer's id rather than the
       content's own id -- normalize before comparing, same as
       sg_status_diff_unstaged. A failure here (the object itself is
       unreadable, or it's a genuine chunk pointer whose data is missing or
       corrupt -- sg_chunk_effective_id's -1/-2, sg/chunk.h) must NOT fail
       this whole call: this builder cannot answer "did this path change",
       so it reports the path as changed instead and leaves the complaining
       to the renderer, which re-reads the same blob through
       sg_diff_side_read and, holding the path, can name it in an
       actionable message (see sg/diff.h's sg_diff_index_workdir contract). */
    if (sg_chunk_effective_id(git_dir, entry->sha1, effective_sha1) != 0) {
        sg_diff_side ns = side_workdir(wd_sha1, wd_mode);

        return list_append(out, entry->path, &os, &ns);
    }

    /* Compared against effective_sha1 (content-normalized), never entry->sha1
       directly -- os itself still carries the raw/pointer id for
       sg_diff_side_read's sake, so the comparison uses a throwaway side
       built with the effective id instead of mutating os. Mode is compared
       here too (Phase 26): a bare chmod with unchanged content used to be
       invisible to this builder, since WORKDIR sides carried no mode at all.
       This is what makes `sg diff` start reporting mode-only changes -- NOT
       `sg status`, which walks a completely separate implementation
       (sg_status_diff_unstaged in src/workdir/status.c) that never goes
       through sg/diff.h's builders at all and does not compare .mode. That
       second implementation's own mode-only blind spot is unchanged by this
       file (measured: `sg diff` correctly shows old/new mode lines after a
       bare chmod, `sg status --porcelain` still prints nothing for the same
       change) -- see sg/diff.h's sg_diff_side contract and blob_sides_differ. */
    {
        sg_diff_side os_effective = side_blob(entry->mode, effective_sha1);
        sg_diff_side ns = side_workdir(wd_sha1, wd_mode);

        if (blob_sides_differ(&os_effective, &ns))
            return list_append(out, entry->path, &os, &ns);
    }
    return 0;
}

/* Builds the `ours`/`theirs` side of an unmerged sg_diff_entry straight off
   the matching stage entry -- ABSENT when that stage does not exist (e.g. an
   add/add conflict has no stage 1, and a delete/modify conflict is missing
   one of stage 2/3). Mirrors side_blob's convention: stores the entry's raw
   id, chunk-pointer or not, resolved to the effective id only at render time
   via sg_diff_side_effective_id -- same as every other BLOB side in this
   file. */
static sg_diff_side side_from_stage_entry(const sg_index *idx, const char *path, int stage)
{
    int pos = sg_index_find_stage(idx, path, stage);

    if (pos < 0)
        return side_absent();
    return side_blob(idx->entries[pos].mode, idx->entries[pos].sha1);
}

/* Builds the `result` side of an unmerged sg_diff_entry: the working-tree
   file at path, or ABSENT when it is missing or unreadable. Deliberately
   simpler than append_index_entry_vs_workdir -- a working-tree file is never
   itself a chunk pointer, so there is no effective-id resolution to do, only
   stat + hash. "Exists but unreadable" collapses into ABSENT, same
   convention as append_index_entry_vs_workdir uses for the same case. */
static sg_diff_side build_result_side(const char *repo_root, const char *path)
{
    char abspath[SG_PATH_MAX];
    struct stat st;
    unsigned char wd_sha1[SG_SHA1_RAW_LEN];
    unsigned int wd_mode;

    /* Phase 36: path is an unmerged index entry's path, same untrusted
       source and same reasoning as append_index_entry_vs_workdir above --
       collapse into ABSENT rather than reading outside the repository. */
    if (!sg_relpath_is_safe(path))
        return side_absent();
    if (sg_path_join(abspath, sizeof(abspath), repo_root, path) != 0)
        return side_absent();
    if (stat(abspath, &st) != 0)
        return side_absent();
    wd_mode = workdir_entry_mode(abspath);
    if (sg_hash_file_blob(abspath, wd_sha1) != 0)
        return side_absent();
    return side_workdir(wd_sha1, wd_mode);
}

int sg_diff_index_workdir(const char *git_dir, const char *repo_root, const sg_index *idx,
                          sg_diff_list *out)
{
    size_t i = 0;

    memset(out, 0, sizeof(*out));

    while (i < idx->count) {
        const char *path = idx->entries[i].path;
        size_t group_end = index_group_end(idx, i);

        if (idx->entries[i].stage != 0) {
            /* Unresolved conflict: always the fixed "unmerged" row, plus --
               measured against git 2.55.0 -- a second, ordinary row for
               stage 2 (ours) vs the working tree, but only when stage 2
               exists and its content actually differs from what is on
               disk. */
            int stage2_pos;

            if (list_append_unmerged(out, path) != 0) {
                sg_diff_list_free(out);
                return -1;
            }
            {
                sg_diff_entry *ue = &out->entries[out->count - 1];

                ue->ours = side_from_stage_entry(idx, path, 2);
                ue->theirs = side_from_stage_entry(idx, path, 3);
                ue->result = build_result_side(repo_root, path);
            }

            stage2_pos = sg_index_find_stage(idx, path, 2);
            if (stage2_pos >= 0) {
                if (append_index_entry_vs_workdir(git_dir, repo_root, &idx->entries[stage2_pos],
                                                  out) != 0) {
                    sg_diff_list_free(out);
                    return -1;
                }
            }
        } else {
            if (append_index_entry_vs_workdir(git_dir, repo_root, &idx->entries[i], out) != 0) {
                sg_diff_list_free(out);
                return -1;
            }
        }
        i = group_end;
    }
    return 0;
}

int sg_diff_tree_workdir(const char *git_dir, const char *repo_root,
                         const unsigned char *old_tree, const sg_index *idx,
                         sg_diff_list *out, char *bad_path, int combined,
                         int include_unchanged)
{
    sg_flat_list old_flat;
    size_t oi = 0;
    size_t ii = 0;
    int rc;

    memset(out, 0, sizeof(*out));

    rc = flatten_or_empty(git_dir, old_tree, &old_flat, bad_path);
    if (rc != 0)
        return rc;

    while (oi < old_flat.count || ii < idx->count) {
        int cmp;
        const char *idx_path = NULL;
        size_t group_end = ii;

        /* Unlike sg_diff_tree_index/sg_diff_index_workdir, an unresolved
           conflict is NOT special here: the index only decides which paths
           take part, and the content compared is always the tree's blob
           against the actual working-tree bytes, regardless of which stage
           (if any) the index happens to carry for this path. So the index
           cursor advances by whole path-groups (covering every stage) purely
           for membership, never reading a stage's blob id. */
        if (ii < idx->count) {
            idx_path = idx->entries[ii].path;
            group_end = index_group_end(idx, ii);
        }

        if (oi >= old_flat.count)
            cmp = 1;
        else if (ii >= idx->count)
            cmp = -1;
        else
            cmp = strcmp(old_flat.entries[oi].path, idx_path);

        if (cmp == 0) {
            /* Present in both the tree and the index: compare the tree's
               blob against the working tree's actual bytes at this path,
               never the index's blob -- this is a tree-vs-workdir diff. */
            char abspath[SG_PATH_MAX];
            struct stat st;
            sg_diff_side os =
                side_blob(old_flat.entries[oi].mode, old_flat.entries[oi].sha1);

            if (sg_path_join(abspath, sizeof(abspath), repo_root, old_flat.entries[oi].path) !=
               0) {
                rc = -1;
                goto done;
            }

            if (stat(abspath, &st) != 0) {
                sg_diff_side ns = side_absent();

                if (list_append(out, old_flat.entries[oi].path, &os, &ns) != 0) {
                    rc = -1;
                    goto done;
                }
            } else {
                unsigned char wd_sha1[SG_SHA1_RAW_LEN];
                unsigned char effective_sha1[SG_SHA1_RAW_LEN];
                unsigned int wd_mode = workdir_entry_mode(abspath);

                if (sg_hash_file_blob(abspath, wd_sha1) != 0) {
                    /* Same convention as append_index_entry_vs_workdir above
                       and sg_status_diff_unstaged: existing-but-unreadable is
                       ABSENT, not a WORKDIR side with a placeholder zero
                       id -- see sg/diff.h's sg_diff_side contract. */
                    sg_diff_side ns = side_absent();

                    if (list_append(out, old_flat.entries[oi].path, &os, &ns) != 0) {
                        rc = -1;
                        goto done;
                    }
                } else if (sg_chunk_effective_id(git_dir, old_flat.entries[oi].sha1,
                                                    effective_sha1) != 0) {
                    /* Same rule as append_index_entry_vs_workdir above and
                       sg/diff.h's sg_diff_index_workdir contract: an
                       unreadable object or a broken chunk pointer here must
                       not fail the whole call, or every other path in this
                       diff goes silent along with it. Report the path as
                       changed and let the renderer's own sg_diff_side_read
                       hit the same failure with the path in hand. */
                    sg_diff_side ns = side_workdir(wd_sha1, wd_mode);

                    if (list_append(out, old_flat.entries[oi].path, &os, &ns) != 0) {
                        rc = -1;
                        goto done;
                    }
                } else {
                    /* Compared against effective_sha1, not old_flat's raw
                       sha1 -- os itself keeps the raw/pointer id for
                       sg_diff_side_read. Mode is compared too (Phase 26): see
                       append_index_entry_vs_workdir's matching comment. */
                    sg_diff_side os_effective =
                        side_blob(old_flat.entries[oi].mode, effective_sha1);
                    sg_diff_side ns = side_workdir(wd_sha1, wd_mode);
                    int must_append = blob_sides_differ(&os_effective, &ns);

                    /* Phase 40 SPEC section 3/6, fixture O's p3: a combined
                       row's inclusion rule is "differs from ANY parent",
                       not just "differs from the named tree" -- the ordinary
                       2-way criterion above only ever checks the tree
                       against the working tree, so a path where the named
                       tree happens to equal the working tree but the INDEX
                       does not (a staged-then-reverted edit) would otherwise
                       never even enter the list, and sg_diff_fill_combined_
                       from_index only fills rows that are already there.
                       ii still points at the group's first entry here
                       (lowest stage -- sg/index.h's (path, stage) sort
                       order, same invariant index_group_end's own comment
                       relies on), so no separate stage scan is needed. A
                       chunk-pointer resolution failure on the index side is
                       treated as "differs" (force the row in), the same
                       failure direction append_index_entry_vs_workdir uses
                       right above for the identical reason: the renderer's
                       own sg_diff_side_read is what actually reports it. */
                    if (!must_append && combined && ii < idx->count) {
                        unsigned char idx_eff[SG_SHA1_RAW_LEN];
                        sg_diff_side is;

                        if (sg_chunk_effective_id(git_dir, idx->entries[ii].sha1, idx_eff) != 0)
                            must_append = 1;
                        else {
                            is = side_blob(idx->entries[ii].mode, idx_eff);
                            if (blob_sides_differ(&is, &ns))
                                must_append = 1;
                        }
                    }

                    if (must_append) {
                        if (list_append(out, old_flat.entries[oi].path, &os, &ns) != 0) {
                            rc = -1;
                            goto done;
                        }
                    } else if (include_unchanged) {
                        /* SPEC section 2.3: only when still unchanged after
                           the combined widening right above, which is why
                           this sits after that block rather than testing
                           blob_sides_differ directly -- must_append already
                           folds in every reason this path is NOT a genuine
                           unchanged row (a differing tree/workdir pair, or a
                           differing index/workdir pair when combined is on). */
                        if (list_append(out, old_flat.entries[oi].path, &os, &ns) != 0) {
                            rc = -1;
                            goto done;
                        }
                        out->entries[out->count - 1].unchanged = 1;
                    }
                }
            }
            oi++;
            ii = group_end;
        } else if (cmp < 0) {
            /* In the tree, not in the index: a deletion, unconditionally --
               even if the file is still physically on disk (git rm --cached
               semantics; measured against git 2.55.0, see sg/diff.h). */
            sg_diff_side os =
                side_blob(old_flat.entries[oi].mode, old_flat.entries[oi].sha1);
            sg_diff_side ns = side_absent();

            if (list_append(out, old_flat.entries[oi].path, &os, &ns) != 0) {
                rc = -1;
                goto done;
            }
            oi++;
        } else {
            /* In the index (at any stage), not in the tree: an addition,
               sourced from the working tree if the file is actually there.
               Deleted from disk after being staged (stat fails) means both
               sides are absent and nothing is reported.

               Existing-but-unreadable is NOT folded into that case. The row
               is appended with a WORKDIR side carrying the lstat mode and a
               placeholder zero id, precisely so the renderer tries the read
               itself, fails, and names the file in an actionable message --
               dropping the row instead loses the only report the user would
               ever get, which is the failure mode sg/diff.h's builder
               contract exists to prevent. The header this produces is
               "new file mode <mode>" + "index 0000000..0000000": the mode
               suffix is suppressed because a mode line was written, so the
               all-zero new id is the one part git would never write. That
               is a deliberate trade -- a slightly wrong id on a row that is
               immediately followed by a warning, over a silently missing
               file. Real git does neither: it aborts the whole command
               (fatal: cannot hash <path>, exit 128). sg does not, on
               purpose; one unreadable path must not blind the user to every
               other path in the diff. */
            char abspath[SG_PATH_MAX];
            struct stat st;
            sg_diff_side os = side_absent();

            /* Phase 36: idx_path is untrusted (a raw .git/index path), same
               reasoning as append_index_entry_vs_workdir in
               sg_diff_index_workdir above. Treat it exactly like the "stat
               fails" case right below -- both sides absent, nothing
               reported -- rather than reading a file the index merely
               points at, possibly outside the repository entirely. */
            if (!sg_relpath_is_safe(idx_path)) {
                ii = group_end;
                continue;
            }
            if (sg_path_join(abspath, sizeof(abspath), repo_root, idx_path) != 0) {
                rc = -1;
                goto done;
            }
            if (stat(abspath, &st) == 0) {
                unsigned char wd_sha1[SG_SHA1_RAW_LEN];
                unsigned int wd_mode = workdir_entry_mode(abspath);

                sg_diff_side ns = sg_hash_file_blob(abspath, wd_sha1) == 0
                                      ? side_workdir(wd_sha1, wd_mode)
                                      : side_workdir(NULL, wd_mode);

                if (list_append(out, idx_path, &os, &ns) != 0) {
                    rc = -1;
                    goto done;
                }
            }
            ii = group_end;
        }
    }
    rc = 0;

done:
    sg_flat_list_free(&old_flat);
    if (rc != 0)
        sg_diff_list_free(out);
    return rc;
}

int sg_diff_side_read(const char *git_dir, const char *repo_root, const char *path,
                      const sg_diff_side *side, unsigned char **data, size_t *len,
                      sg_chunk_missing_info *missing)
{
    *data = NULL;
    *len = 0;

    if (side->kind == SG_DIFF_SIDE_ABSENT)
        return 0;

    if (side->kind == SG_DIFF_SIDE_BLOB)
        return sg_chunk_read_blob(git_dir, side->id, data, len, missing);

    /* SG_DIFF_SIDE_WORKDIR */
    {
        char abspath[SG_PATH_MAX];

        if (sg_path_join(abspath, sizeof(abspath), repo_root, path) != 0)
            return -1;
        return sg_read_file(abspath, data, len);
    }
}
