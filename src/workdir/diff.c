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

    for (i = 0; i < list->count; i++)
        free(list->entries[i].path);
    free(list->entries);
    list->entries = NULL;
    list->count = 0;
    list->cap = 0;
}

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
    list->entries[list->count].old_side = *old_side;
    list->entries[list->count].new_side = *new_side;
    list->entries[list->count].unmerged = 0;
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

static sg_diff_side side_workdir(void)
{
    sg_diff_side s;

    memset(&s, 0, sizeof(s));
    s.kind = SG_DIFF_SIDE_WORKDIR;
    return s;
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

/* Two BLOB sides differ if their ids differ, or if both modes are known and
   differ -- an unknown (0) mode on either side skips the mode comparison, per
   sg/diff.h's sg_diff_side contract. */
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
   idx->entries[i].path. */
static size_t index_group_end(const sg_index *idx, size_t i)
{
    const char *path = idx->entries[i].path;
    size_t j = i;

    while (j < idx->count && strcmp(idx->entries[j].path, path) == 0)
        j++;
    return j;
}

int sg_diff_trees(const char *git_dir, const unsigned char *old_tree,
                  const unsigned char *new_tree, sg_diff_list *out, char *bad_path)
{
    sg_flat_list old_flat;
    sg_flat_list new_flat;
    size_t oi = 0;
    size_t ni = 0;
    int rc;

    memset(out, 0, sizeof(*out));

    rc = flatten_or_empty(git_dir, old_tree, &old_flat, bad_path);
    if (rc != 0)
        return rc;
    rc = flatten_or_empty(git_dir, new_tree, &new_flat, bad_path);
    if (rc != 0) {
        sg_flat_list_free(&old_flat);
        return rc;
    }

    while (oi < old_flat.count || ni < new_flat.count) {
        int cmp;

        if (oi >= old_flat.count)
            cmp = 1;
        else if (ni >= new_flat.count)
            cmp = -1;
        else
            cmp = strcmp(old_flat.entries[oi].path, new_flat.entries[ni].path);

        if (cmp == 0) {
            sg_diff_side os = side_blob(old_flat.entries[oi].mode, old_flat.entries[oi].sha1);
            sg_diff_side ns = side_blob(new_flat.entries[ni].mode, new_flat.entries[ni].sha1);

            if (blob_sides_differ(&os, &ns)) {
                if (list_append(out, old_flat.entries[oi].path, &os, &ns) != 0) {
                    rc = -1;
                    goto done;
                }
            }
            oi++;
            ni++;
        } else if (cmp < 0) {
            sg_diff_side os = side_blob(old_flat.entries[oi].mode, old_flat.entries[oi].sha1);
            sg_diff_side ns = side_absent();

            if (list_append(out, old_flat.entries[oi].path, &os, &ns) != 0) {
                rc = -1;
                goto done;
            }
            oi++;
        } else {
            sg_diff_side os = side_absent();
            sg_diff_side ns = side_blob(new_flat.entries[ni].mode, new_flat.entries[ni].sha1);

            if (list_append(out, new_flat.entries[ni].path, &os, &ns) != 0) {
                rc = -1;
                goto done;
            }
            ni++;
        }
    }
    rc = 0;

done:
    sg_flat_list_free(&old_flat);
    sg_flat_list_free(&new_flat);
    if (rc != 0)
        sg_diff_list_free(out);
    return rc;
}

int sg_diff_tree_index(const char *git_dir, const unsigned char *old_tree,
                       const sg_index *idx, sg_diff_list *out, char *bad_path)
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
    sg_diff_side os = side_blob(entry->mode, entry->sha1);

    /* A truncated path must not be silently skipped: sg/diff.h and
       CLAUDE.md both require a hard failure here rather than quietly
       dropping a path from `sg diff`. */
    if (sg_path_join(abspath, sizeof(abspath), repo_root, entry->path) != 0)
        return -1;

    if (stat(abspath, &st) != 0) {
        sg_diff_side ns = side_absent();

        return list_append(out, entry->path, &os, &ns);
    }

    if (sg_hash_file_blob(abspath, wd_sha1) != 0) {
        sg_diff_side ns = side_absent();

        return list_append(out, entry->path, &os, &ns);
    }

    /* idx's id may be a chunked-storage pointer's id rather than the
       content's own id -- normalize before comparing, same as
       sg_status_diff_unstaged. */
    if (sg_chunk_effective_id(git_dir, entry->sha1, effective_sha1) != 0)
        return -1;

    if (memcmp(wd_sha1, effective_sha1, SG_SHA1_RAW_LEN) != 0) {
        sg_diff_side ns = side_workdir();

        return list_append(out, entry->path, &os, &ns);
    }
    return 0;
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
                         sg_diff_list *out, char *bad_path)
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

                if (sg_hash_file_blob(abspath, wd_sha1) != 0) {
                    sg_diff_side ns = side_absent();

                    if (list_append(out, old_flat.entries[oi].path, &os, &ns) != 0) {
                        rc = -1;
                        goto done;
                    }
                } else {
                    if (sg_chunk_effective_id(git_dir, old_flat.entries[oi].sha1,
                                              effective_sha1) != 0) {
                        rc = -1;
                        goto done;
                    }
                    if (memcmp(wd_sha1, effective_sha1, SG_SHA1_RAW_LEN) != 0) {
                        sg_diff_side ns = side_workdir();

                        if (list_append(out, old_flat.entries[oi].path, &os, &ns) != 0) {
                            rc = -1;
                            goto done;
                        }
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
               sourced from the working tree if the file is actually there;
               if it was deleted from disk after being staged, both sides
               are absent and nothing is reported. */
            char abspath[SG_PATH_MAX];
            struct stat st;
            sg_diff_side os = side_absent();

            if (sg_path_join(abspath, sizeof(abspath), repo_root, idx_path) != 0) {
                rc = -1;
                goto done;
            }
            if (stat(abspath, &st) == 0) {
                sg_diff_side ns = side_workdir();

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
