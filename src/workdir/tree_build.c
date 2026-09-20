#include "sg/tree_build.h"

#include "sg/chunk.h"
#include "sg/loose.h"
#include "sg/objstore.h"
#include "sg/object.h"
#include "sg/repo.h"
#include "sg/status.h"
#include "sg/workdir.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SG_TREE_DIR_MODE 040000

static int build_level(const char *git_dir, const sg_flat_entry *entries, size_t count,
                       unsigned char tree_id_out[SG_SHA1_RAW_LEN])
{
    sg_tree_entry *level = NULL;
    size_t level_count = 0;
    size_t level_cap = 0;
    size_t i = 0;
    unsigned char *serialized;
    size_t serialized_len;
    int rc = -1;

    while (i < count) {
        const char *path = entries[i].path;
        const char *slash = strchr(path, '/');
        sg_tree_entry *slot;

        if (level_count == level_cap) {
            size_t new_cap = level_cap == 0 ? 8 : level_cap * 2;
            sg_tree_entry *grown = realloc(level, new_cap * sizeof(*grown));

            if (grown == NULL)
                goto done;
            level = grown;
            level_cap = new_cap;
        }
        slot = &level[level_count];

        if (slash == NULL) {
            slot->name = strdup(path);
            if (slot->name == NULL)
                goto done;
            slot->mode = entries[i].mode;
            memcpy(slot->sha1, entries[i].sha1, SG_SHA1_RAW_LEN);
            level_count++;
            i++;
        } else {
            size_t comp_len = (size_t)(slash - path);
            size_t j = i + 1;
            sg_flat_entry *sub;
            size_t sub_count;
            size_t k;

            while (j < count) {
                const char *p2 = entries[j].path;

                if (strncmp(p2, path, comp_len) != 0 || p2[comp_len] != '/')
                    break;
                j++;
            }
            sub_count = j - i;
            sub = malloc(sub_count * sizeof(*sub));
            if (sub == NULL)
                goto done;
            for (k = 0; k < sub_count; k++) {
                sub[k].path = entries[i + k].path + comp_len + 1; /* not owned, transient */
                sub[k].mode = entries[i + k].mode;
                memcpy(sub[k].sha1, entries[i + k].sha1, SG_SHA1_RAW_LEN);
            }

            slot->name = malloc(comp_len + 1);
            if (slot->name == NULL) {
                free(sub);
                goto done;
            }
            memcpy(slot->name, path, comp_len);
            slot->name[comp_len] = '\0';
            slot->mode = SG_TREE_DIR_MODE;

            if (build_level(git_dir, sub, sub_count, slot->sha1) != 0) {
                free(sub);
                free(slot->name);
                goto done;
            }
            free(sub);

            level_count++;
            i = j;
        }
    }

    /* Because entries arrive sorted by full path, a name collision between a
       leaf ("foo") and a directory group ("foo/bar") always lands on
       adjacent slots here -- catch it before it turns into a tree object
       with two entries sharing the same name, which git itself rejects. */
    for (i = 1; i < level_count; i++) {
        if (strcmp(level[i - 1].name, level[i].name) == 0)
            goto done;
    }

    if (sg_tree_serialize(level, level_count, &serialized, &serialized_len) != 0)
        goto done;
    rc = sg_loose_write(git_dir, SG_OBJ_TREE, serialized, serialized_len, tree_id_out);
    free(serialized);

done:
    {
        size_t n;

        for (n = 0; n < level_count; n++)
            free(level[n].name);
        free(level);
    }
    return rc;
}

int sg_tree_build(const char *git_dir, const sg_flat_entry *entries, size_t count,
                  unsigned char tree_id_out[SG_SHA1_RAW_LEN])
{
    return build_level(git_dir, entries, count, tree_id_out);
}

static int flatten_append(sg_flat_list *out, size_t *cap, const char *path, unsigned int mode,
                          const unsigned char sha1[SG_SHA1_RAW_LEN])
{
    if (out->count == *cap) {
        size_t new_cap = *cap == 0 ? 8 : *cap * 2;
        sg_flat_entry *grown = realloc(out->entries, new_cap * sizeof(*grown));

        if (grown == NULL)
            return -1;
        out->entries = grown;
        *cap = new_cap;
    }
    out->entries[out->count].path = strdup(path);
    if (out->entries[out->count].path == NULL)
        return -1;
    out->entries[out->count].mode = mode;
    memcpy(out->entries[out->count].sha1, sha1, SG_SHA1_RAW_LEN);
    out->count++;
    return 0;
}

static int flatten_into(const char *git_dir, const unsigned char tree_id[SG_SHA1_RAW_LEN],
                        const char *prefix, sg_flat_list *out, size_t *cap, char *bad_path)
{
    sg_obj_type type;
    unsigned char *content;
    size_t content_len;
    sg_tree tree;
    size_t i;
    int rc = 0;

    if (sg_object_read(git_dir, tree_id, &type, &content, &content_len) != 0 || type != SG_OBJ_TREE)
        return -1;

    if (sg_tree_parse(content, content_len, &tree) != 0) {
        free(content);
        return -1;
    }
    free(content);

    for (i = 0; i < tree.count && rc == 0; i++) {
        const sg_tree_entry *e = &tree.entries[i];
        char *full_path;
        size_t prefix_len = strlen(prefix);
        size_t name_len = strlen(e->name);

        /* A tree object's entry names come straight from object content,
           which may originate from a crafted/foreign commit (not just sg's
           own tree builder). Without this check, an entry named ".git"
           would let `sg switch`/`sg reset --hard` write into the
           repository's own .git directory via the full_path built below
           (measured against real git 2.55.0: it refuses the same tree at
           read-tree time). See sg_path_component_is_safe for exactly what
           it rejects and why. */
        if (!sg_path_component_is_safe(e->name)) {
            if (bad_path != NULL) {
                if (prefix_len > 0)
                    snprintf(bad_path, SG_PATH_MAX, "%s/%s", prefix, e->name);
                else
                    snprintf(bad_path, SG_PATH_MAX, "%s", e->name);
            }
            rc = -2;
            break;
        }

        /* flatten_into recurses one stack frame per directory level with no
           depth limit of its own; bounding path length here also bounds
           recursion depth to roughly SG_PATH_MAX / 2 (the shortest possible
           non-root component is "x/"), and any path this long would fail
           the sg_path_join callers below anyway -- better to refuse it here
           than after exhausting the stack. */
        if (prefix_len + name_len + 1 >= SG_PATH_MAX) {
            if (bad_path != NULL)
                snprintf(bad_path, SG_PATH_MAX, "%s", prefix_len > 0 ? prefix : e->name);
            rc = -2;
            break;
        }

        full_path = malloc(prefix_len + name_len + 2);
        if (full_path == NULL) {
            rc = -1;
            break;
        }
        if (prefix_len > 0) {
            memcpy(full_path, prefix, prefix_len);
            full_path[prefix_len] = '/';
            memcpy(full_path + prefix_len + 1, e->name, name_len + 1);
        } else {
            memcpy(full_path, e->name, name_len + 1);
        }

        if (e->mode == SG_TREE_DIR_MODE)
            rc = flatten_into(git_dir, e->sha1, full_path, out, cap, bad_path);
        else
            rc = flatten_append(out, cap, full_path, e->mode, e->sha1);

        free(full_path);
    }

    sg_tree_free(&tree);
    return rc;
}

/* Phase 52 observability hook: counts calls to sg_tree_flatten, nothing else.
   See the header comment on sg_tree_flatten_test_count for the contract. */
static size_t sg_tree_flatten_call_count = 0;

int sg_tree_flatten(const char *git_dir, const unsigned char tree_id[SG_SHA1_RAW_LEN], sg_flat_list *out,
                    char *bad_path)
{
    size_t cap = 0;
    int rc;

    sg_tree_flatten_call_count++;
    out->entries = NULL;
    out->count = 0;
    rc = flatten_into(git_dir, tree_id, "", out, &cap, bad_path);
    if (rc != 0) {
        sg_flat_list_free(out);
        return rc;
    }
    return 0;
}

size_t sg_tree_flatten_test_count(void)
{
    return sg_tree_flatten_call_count;
}

void sg_tree_flatten_test_reset(void)
{
    sg_tree_flatten_call_count = 0;
}

void sg_flat_list_free(sg_flat_list *list)
{
    size_t i;

    for (i = 0; i < list->count; i++)
        free(list->entries[i].path);
    free(list->entries);
    list->entries = NULL;
    list->count = 0;
}

int sg_tree_build_from_index(const char *git_dir, const sg_index *idx,
                             unsigned char tree_id_out[SG_SHA1_RAW_LEN])
{
    sg_flat_entry *flat = NULL;
    size_t i;
    int rc;

    for (i = 0; i < idx->count; i++) {
        if (idx->entries[i].stage != 0)
            return -1;
    }

    if (idx->count > 0) {
        flat = malloc(idx->count * sizeof(*flat));
        if (flat == NULL)
            return -1;
        for (i = 0; i < idx->count; i++) {
            flat[i].path = idx->entries[i].path; /* not owned, transient view */
            flat[i].mode = idx->entries[i].mode;
            memcpy(flat[i].sha1, idx->entries[i].sha1, SG_SHA1_RAW_LEN);
        }
    }

    rc = sg_tree_build(git_dir, flat, idx->count, tree_id_out);
    free(flat);
    return rc;
}

int sg_tree_build_from_workdir(const char *git_dir, const char *repo_root, const sg_index *idx,
                               sg_workdir_missing missing, const sg_pathspec *ps,
                               unsigned char tree_id_out[SG_SHA1_RAW_LEN], char *bad_path)
{
    sg_flat_entry *entries = NULL;
    size_t entry_count = 0;
    size_t i;
    int rc = -1;
    int chunk_enabled = 0;
    size_t chunk_threshold = SG_CHUNK_DEFAULT_THRESHOLD;

    if (idx->count > 0) {
        entries = malloc(idx->count * sizeof(*entries));
        if (entries == NULL)
            return -1;
    }

    sg_repo_read_chunk_config(git_dir, &chunk_enabled, &chunk_threshold);

    for (i = 0; i < idx->count; i++) {
        char abspath[SG_PATH_MAX];
        unsigned char *content = NULL;
        size_t content_len = 0;
        unsigned char blob_id[SG_SHA1_RAW_LEN];

        /* idx may hold several stage 1/2/3 entries for the same path while a
           conflict is unresolved (there is no separate stage-0 entry then).
           Entries are sorted by (path, stage), so duplicates are contiguous
           and the first one seen is stage 0 if one exists, otherwise the
           lowest of whatever conflict stages are present -- either way,
           exactly one representative per path is emitted, using whatever
           content currently sits in the working tree (e.g. the
           conflict-marked content), never producing a tree with two entries
           sharing a name. */
        if (i > 0 && strcmp(idx->entries[i].path, idx->entries[i - 1].path) == 0)
            continue;

        /* Phase 36: idx->entries[i].path comes straight off a parsed .git/index
           entry, and sg_index_read validates none of it (see index.c and
           CLAUDE.md's rationale for keeping it that way -- an index consumer
           other than this one, e.g. `sg status`, still has to be able to
           list a hostile entry). This is the one consumer that turns the
           path into a read PLUS a permanent write (a loose object below),
           which is a strictly worse outcome than the write-side guards this
           project already has: a path like "../secret.txt" would hash and
           store a file outside the repository entirely, and it does so
           whether or not anything downstream later fails to delete/apply it
           (measured: a stash push against a crafted index wrote the outside
           file's blob before its own later, unrelated apply step failed).
           Must hard-fail the whole build, not silently skip the entry --
           skipping would make sg_snapshot_create/sg_stash_push produce a
           tree that quietly omits a legitimate index path too, the same
           silent-data-loss shape the truncation check below already
           guards against. */
        if (!sg_relpath_is_safe(idx->entries[i].path)) {
            if (bad_path != NULL)
                snprintf(bad_path, SG_PATH_MAX, "%s", idx->entries[i].path);
            goto out_free_entries;
        }

        /* Phase 37: a path the pathspec does not match is handled exactly
           like SG_WORKDIR_MISSING_KEEP_INDEX_BLOB, regardless of `missing`
           and regardless of what is actually on disk -- the working tree is
           never even looked at for this path this call. This must run
           AFTER the path-safety guard above (every index path still gets
           validated, matched or not) and BEFORE any lstat/read below (the
           whole point is to skip touching the working tree at all). */
        if (ps != NULL && !sg_pathspec_matches(ps, idx->entries[i].path)) {
            entries[entry_count].path = idx->entries[i].path;
            entries[entry_count].mode = idx->entries[i].mode;
            memcpy(entries[entry_count].sha1, idx->entries[i].sha1, SG_SHA1_RAW_LEN);
            entry_count++;
            continue;
        }

        /* A truncated path must hard-fail here, before any classification
           or read ever runs: a truncated buffer usually still names some
           real, unrelated path higher up the tree, and letting
           sg_worktree_classify/sg_worktree_read_entry silently redo (and
           fail) the same join internally would fall through to this loop's
           "gone" fallback below and silently record the entry as deleted
           from the working tree using the index's own blob -- the exact
           silent-data-loss shape this function must never produce. abspath
           itself is otherwise unused: every actual worktree access below
           goes through repo_root + idx->entries[i].path instead. */
        if (sg_path_join(abspath, sizeof(abspath), repo_root, idx->entries[i].path) != 0)
            goto out_free_entries;

        /* Round 2 fix (item 1): the tree recorded here must reflect the
           ON-DISK type/mode, not the index's, exactly like real git's own
           stash/snapshot tree does (measured: `git stash push` on a
           file->symlink swap records 120000 in the STASH's own tree even
           though the index -- stash@{0}^2 -- still says 100644; likewise a
           bare chmod +x on a tracked 100644 file is recorded as 100755).
           entry_mode defaults to the index's own mode, and is the value
           used whenever the working tree is not actually consulted (a
           blocked ancestor, a pathspec miss above, or the KEEP_INDEX_BLOB
           fallback below) -- only a SUCCESSFUL worktree read overrides it
           with the OBSERVED mode. */
        unsigned int entry_mode = idx->entries[i].mode;

        /* Item 4: an ancestor beyond a symlink (or any other non-directory)
           makes this path "gone" for read purposes, exactly like a missing
           file -- this is a STRUCTURAL fact about the fixture (an ancestor
           component's type), not something that changes between two
           syscalls a moment apart, so checking it here, before the
           race-sensitive read attempt below, does not reintroduce the race
           that attempt is designed to avoid (see the comment above it). */
        if (sg_worktree_ancestor_blocked(repo_root, idx->entries[i].path)) {
            if (missing == SG_WORKDIR_MISSING_RECORD_DELETION)
                continue;
            memcpy(blob_id, idx->entries[i].sha1, SG_SHA1_RAW_LEN);
        } else {
            /* Round 2 fix (item 1, regression from round 1): round 1 chose
               readlink-vs-fopen from the INDEX's mode, not the ON-DISK
               type -- wrong in both directions (measured by the main
               conversation): index 120000 but the worktree is now a plain
               file makes readlink() fail EINVAL even though the file is
               perfectly readable via fopen, hard-failing the whole
               build/stash/snapshot; index 100644 but the worktree is now a
               symlink to a readable tracked file makes fopen() follow the
               link and hash the TARGET's content, exactly the pre-Phase-81b
               bug this phase exists to fix.

               Fixed by a single lstat PROBE of the actual on-disk entry,
               deciding both which read to attempt AND the mode to record
               from what is REALLY there -- never from the index. This adds
               one syscall the pre-81b single-type code did not have in its
               happy path (it called sg_read_file directly, no probe), but
               is what "decide by a single probe" requires once two
               different read functions exist to choose between; the race
               tolerance the pre-81b ordering protected is preserved
               one level down instead: a read failure AFTER a successful
               probe (the file vanished, or is a race-y readlink) still
               falls through to the SAME lstat-after-failure classification
               below as before, so "existed at the probe, gone by the read"
               is still an ordinary deletion, not a hard failure -- only
               "still there and still unreadable" (or a real, non-ENOENT
               error) is fatal, exactly as it always was. */
            struct stat probe_st;
            int rc_read = -1;

            if (lstat(abspath, &probe_st) == 0) {
                /* sg_worktree_mode_from_stat (workdir.h) is the SAME mode
                   formula sg_worktree_classify uses -- shared rather than
                   re-derived here, since this probe cannot use
                   sg_worktree_classify itself (that would lstat a SECOND
                   time, reintroducing exactly the extra probe this
                   function's single-lstat design avoids). */
                sg_wt_kind probe_kind = sg_worktree_mode_from_stat(&probe_st, &entry_mode);

                if (probe_kind == SG_WT_SYMLINK)
                    rc_read = sg_worktree_readlink(abspath, &content, &content_len);
                else if (probe_kind == SG_WT_REGULAR)
                    rc_read = sg_read_file(abspath, &content, &content_len);
                /* Any other on-disk type (directory, fifo, ...) leaves
                   rc_read at -1 with no read attempted at all -- the
                   fallback lstat below will find it still there and hard
                   fail, matching the pre-81b "lstat succeeded on a
                   non-regular type" rule. */
            }

            if (rc_read == 0) {
                int write_ok;

                /* Item 1: a symlink's target text never goes through
                   content-defined chunking -- only a REGULAR file's bytes
                   are eligible. */
                if (entry_mode != 0120000 && chunk_enabled) {
                    int chunked;

                    write_ok = sg_chunk_store_blob(git_dir, content, content_len, chunk_threshold,
                                                   blob_id, &chunked) == 0;
                } else {
                    write_ok =
                        sg_loose_write(git_dir, SG_OBJ_BLOB, content, content_len, blob_id) == 0;
                }
                free(content);
                content = NULL;
                if (!write_ok)
                    goto out_free_entries;
            } else {
                /* The probe found nothing to read (rc_read still -1: no
                   entry, or a type neither branch above handles), or the
                   matching read attempt itself failed. Classify with a
                   SECOND lstat -- the common race this whole two-lstat
                   shape exists to tolerate is "existed at the first probe,
                   gone by the time of the read"; the only race this
                   ordering can still be fooled by is the rare opposite
                   direction (absent at the probe, present and unreadable
                   by the time of this second lstat), which is exactly the
                   direction that is safe to hard-fail on.

                   If this lstat finds something there, or fails for a
                   reason other than "no such path", something IS there and
                   unreadable: always a hard failure regardless of policy,
                   under both KEEP_INDEX_BLOB and RECORD_DELETION.
                   Recording the index's stale blob for a file that
                   exists-but-can't-be-read would produce a "snapshot" that
                   silently omits or misrepresents that file's real content
                   -- worse than no snapshot at all. entry_mode is reset to
                   the index's own mode here: any observed on-disk mode
                   from a since-invalidated probe must not survive into the
                   KEEP_INDEX_BLOB fallback below. */
                struct stat st;

                if (lstat(abspath, &st) == 0 || (errno != ENOENT && errno != ENOTDIR))
                    goto out_free_entries;
                entry_mode = idx->entries[i].mode;
                if (missing == SG_WORKDIR_MISSING_RECORD_DELETION)
                    continue; /* omitted: the resulting tree records the deletion */
                /* KEEP_INDEX_BLOB: fall back to the blob the index already
                   recorded, so this entry still resolves. */
                memcpy(blob_id, idx->entries[i].sha1, SG_SHA1_RAW_LEN);
            }
        }

        entries[entry_count].path = idx->entries[i].path; /* transient view, not owned */
        entries[entry_count].mode = entry_mode;
        memcpy(entries[entry_count].sha1, blob_id, SG_SHA1_RAW_LEN);
        entry_count++;
    }

    rc = sg_tree_build(git_dir, entries, entry_count, tree_id_out);

out_free_entries:
    free(entries);
    return rc;
}

int sg_tree_build_from_untracked(const char *git_dir, const char *repo_root, const sg_index *idx,
                                 const sg_pathspec *ps, int include_ignored,
                                 unsigned char tree_id_out[SG_SHA1_RAW_LEN],
                                 size_t *file_count_out)
{
    char **paths = NULL;
    size_t count = 0;
    sg_flat_entry *entries = NULL;
    size_t i;
    int rc = -1;
    int chunk_enabled = 0;
    size_t chunk_threshold = SG_CHUNK_DEFAULT_THRESHOLD;

    if (sg_status_list_untracked(git_dir, repo_root, idx, ps, include_ignored,
                                 SG_STATUS_UNTRACKED_LIST_FILES, &paths, &count) != 0)
        return -1;

    if (file_count_out != NULL)
        *file_count_out = count;

    if (count > 0) {
        entries = malloc(count * sizeof(*entries));
        if (entries == NULL)
            goto out_free_paths;
    }

    sg_repo_read_chunk_config(git_dir, &chunk_enabled, &chunk_threshold);

    for (i = 0; i < count; i++) {
        char abspath[SG_PATH_MAX];
        unsigned char *content = NULL;
        size_t content_len = 0;
        unsigned char blob_id[SG_SHA1_RAW_LEN];
        unsigned int mode = 0100644;
        sg_wt_kind kind;

        /* sg_status_list_untracked only emits a path after collect_untracked
           has already joined repo_root with it into a buffer this same size
           and checked the result, so truncation here is not reachable today.
           Check anyway rather than depending on an invariant established in
           another module and recorded nowhere near this line: if that
           enumerator ever gains a second source of paths, the failure here
           would be silent and would hash some unrelated file's content into
           this path's blob. abspath is otherwise unused -- every actual
           access below goes through repo_root + paths[i]. */
        if (sg_path_join(abspath, sizeof(abspath), repo_root, paths[i]) != 0)
            goto out_free_entries;

        /* Phase 81b: sg_worktree_read_entry replaces stat()+sg_read_file --
           an untracked symlink (now listed by sg_status_list_untracked,
           status.c's four traversal copies) reads as its own 120000 blob
           (readlink target) instead of being followed by fopen. Item 1: its
           bytes never go through content-defined chunking below, only a
           REGULAR file's do. */
        kind = sg_worktree_read_entry(repo_root, paths[i], &mode, &content, &content_len);
        if (kind != SG_WT_REGULAR && kind != SG_WT_SYMLINK)
            goto out_free_entries;

        if (kind == SG_WT_REGULAR && chunk_enabled) {
            int chunked;

            if (sg_chunk_store_blob(git_dir, content, content_len, chunk_threshold, blob_id,
                                    &chunked) != 0) {
                free(content);
                goto out_free_entries;
            }
        } else {
            if (sg_loose_write(git_dir, SG_OBJ_BLOB, content, content_len, blob_id) != 0) {
                free(content);
                goto out_free_entries;
            }
        }
        free(content);

        entries[i].path = paths[i]; /* transient view, not owned here */
        entries[i].mode = mode;
        memcpy(entries[i].sha1, blob_id, SG_SHA1_RAW_LEN);
    }

    rc = sg_tree_build(git_dir, entries, count, tree_id_out);

out_free_entries:
    free(entries);
out_free_paths:
    for (i = 0; i < count; i++)
        free(paths[i]);
    free(paths);
    return rc;
}
