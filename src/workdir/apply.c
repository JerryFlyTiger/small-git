#include "sg/apply.h"

#include "sg/chunk.h"
#include "sg/confirm.h"
#include "sg/ignore.h"
#include "sg/index.h"
#include "sg/merge.h"
#include "sg/objstore.h"
#include "sg/object.h"
#include "sg/quote.h"
#include "sg/rebase.h"
#include "sg/refs.h"
#include "sg/sequencer.h"
#include "sg/snapshot.h"
#include "sg/status.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int flat_find(const sg_flat_list *list, const char *path)
{
    size_t lo = 0;
    size_t hi = list->count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(list->entries[mid].path, path);

        if (cmp == 0)
            return (int)mid;
        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

int sg_apply_tree_to_workdir(const char *git_dir, const char *repo_root,
                            const unsigned char tree_id[SG_SHA1_RAW_LEN])
{
    sg_flat_list target_flat;
    sg_index old_idx;
    sg_index new_idx;
    char bad_path[SG_PATH_MAX];
    size_t i;
    int rc = 0;
    int flatten_rc;
    sg_ignore *ig;

    /* Phase 80 (F1/F2): one sg_ignore for the whole apply, reused by
       sg_worktree_clear_write_path for every path below -- opening a fresh
       one per file would re-read every .gitignore on every single write. */
    if (sg_ignore_open(&ig, git_dir, repo_root) != 0) {
        fprintf(stderr, "sg: out of memory\n");
        return -1;
    }

    flatten_rc = sg_tree_flatten(git_dir, tree_id, &target_flat, bad_path);
    if (flatten_rc == -2) {
        fprintf(stderr, "sg: path %s is invalid, refusing to flatten this tree into file paths\n",
               sg_quote_path_delimited(bad_path));
        sg_ignore_free(ig);
        return -1;
    }
    if (flatten_rc != 0) {
        fprintf(stderr, "sg: failed to read target tree\n");
        sg_ignore_free(ig);
        return -1;
    }

    if (sg_index_read(git_dir, &old_idx) != 0) {
        fprintf(stderr, "sg: failed to read index (corrupt?)\n");
        sg_flat_list_free(&target_flat);
        sg_ignore_free(ig);
        return -1;
    }

    /* remove working-tree files that are currently tracked but absent from
       the target tree. old_idx may hold multiple stage 1/2/3 entries for the
       same path (an unresolved conflict this apply is about to blow away) --
       skip the duplicates so each path is only checked/removed once. */
    for (i = 0; i < old_idx.count; i++) {
        if (i > 0 && strcmp(old_idx.entries[i].path, old_idx.entries[i - 1].path) == 0)
            continue;
        if (flat_find(&target_flat, old_idx.entries[i].path) < 0) {
            /* The index entry being removed here did not necessarily come
               through sg_tree_flatten's own guard above: it may have been
               written by an sg build that predates this check (or by `sg
               add`, before Phase 22's cmd_add guard existed), and that
               guard does not retroactively clean up what is already on
               disk. Without this, a path like "../victim.txt" or ".git/x"
               left in the index by an old bug would still reach the
               guarded delete here even after the tree-side hole is
               closed. */
            if (!sg_relpath_is_safe(old_idx.entries[i].path)) {
                fprintf(stderr, "sg: path %s in index is invalid, refusing to delete\n",
                       sg_quote_path_delimited(old_idx.entries[i].path));
                rc = -1;
                continue;
            }
            /* Phase 80 (fix round, finding 1): sg_remove_file_worktree
               fails closed on a symlinked ancestor instead of resolving
               through it the way a bare remove() would -- that failure
               must abort this apply the same way a failed write already
               does, not be silently swallowed the way a plain remove()
               failure used to be here. */
            if (sg_remove_file_worktree(repo_root, old_idx.entries[i].path) != 0) {
                fprintf(stderr, "sg: cannot remove %s\n",
                       sg_quote_path_delimited(old_idx.entries[i].path));
                rc = -1;
                continue;
            }
            sg_prune_empty_parents(repo_root, old_idx.entries[i].path);
        }
    }
    sg_index_free(&old_idx);

    memset(&new_idx, 0, sizeof(new_idx));
    for (i = 0; i < target_flat.count; i++) {
        char abspath[SG_PATH_MAX];
        unsigned char *blob_content;
        size_t blob_len;
        struct stat st;
        sg_index_entry entry;

        if (sg_path_join(abspath, sizeof(abspath), repo_root, target_flat.entries[i].path) != 0) {
            fprintf(stderr, "sg: path too long, cannot write %s\n",
                   sg_quote_path_delimited(target_flat.entries[i].path));
            rc = -1;
            continue;
        }
        {
            sg_chunk_missing_info missing;
            int read_rc = sg_chunk_read_blob(git_dir, target_flat.entries[i].sha1, &blob_content,
                                             &blob_len, &missing);

            if (read_rc == -2) {
                sg_chunk_print_missing_error(target_flat.entries[i].path, &missing);
                rc = -1;
                continue;
            }
            if (read_rc != 0) {
                fprintf(stderr, "sg: missing blob for %s\n",
                       sg_quote_path_delimited(target_flat.entries[i].path));
                rc = -1;
                continue;
            }
        }
        if (sg_worktree_clear_write_path(ig, repo_root, target_flat.entries[i].path, NULL) != 0 ||
           sg_write_file_worktree(repo_root, target_flat.entries[i].path, blob_content, blob_len,
                                  (int)target_flat.entries[i].mode) != 0) {
            fprintf(stderr, "sg: failed to write %s\n",
                   sg_quote_path_delimited(target_flat.entries[i].path));
            free(blob_content);
            rc = -1;
            continue;
        }
        {
            size_t written_len = blob_len;

            free(blob_content);
            blob_content = NULL;

            /* Phase 81c (item 5): lstat, never stat -- a stat() here would
               follow a freshly created symlink and record the TARGET's
               metadata (size included), so a second `sg status` right after
               a checkout would see the link's own size disagree with the
               index and report a bogus modification (ORACLE.md c1's "status
               empty AGAIN on a second call" row). file_size comes from the
               content just written (cmd_add.c's own template), not from
               st_size, so it is right for a symlink and a regular file
               alike without depending on lstat's st_size formula. */
            if (lstat(abspath, &st) != 0) {
                rc = -1;
                continue;
            }

            memset(&entry, 0, sizeof(entry));
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
            entry.mode = target_flat.entries[i].mode;
            entry.uid = (unsigned int)st.st_uid;
            entry.gid = (unsigned int)st.st_gid;
            /* Phase 81c (cold-read round 1, MEASURED against git 2.55.0):
               neither "what we wrote" nor "what lstat says" -- git's own
               rule. On a hand-built 120000 blob, git records a link's true
               lstat size when the target landed intact (size 5 for
               "f.txt"), and records 0 when the target was truncated at an
               embedded NUL. The 0 is deliberate: size, mtime, ino and mode
               would otherwise ALL match the link git just wrote, and a
               stat-only check would then call a link whose content does not
               match its blob CLEAN. Recording st_size here would
               reintroduce exactly that hole; recording the pre-truncation
               blob length merely happens to differ. Follow git: on any
               mismatch between what we asked for and what landed, zero the
               cached size so the next reader must compare content. */
            entry.file_size = ((off_t)written_len == st.st_size)
                                  ? (unsigned int)st.st_size
                                  : 0;
        }
        memcpy(entry.sha1, target_flat.entries[i].sha1, SG_SHA1_RAW_LEN);
        entry.path = target_flat.entries[i].path;

        if (sg_index_upsert(&new_idx, &entry) != 0) {
            fprintf(stderr, "sg: failed to stage %s\n", sg_quote_path_delimited(target_flat.entries[i].path));
            rc = -1;
        }
    }
    sg_flat_list_free(&target_flat);
    sg_ignore_free(ig);

    if (rc == 0 && sg_index_write(git_dir, &new_idx) != 0) {
        fprintf(stderr, "sg: failed to write index\n");
        rc = -1;
    }
    sg_index_free(&new_idx);

    return rc;
}

int sg_index_reset_to_tree(const char *git_dir, const unsigned char tree_id[SG_SHA1_RAW_LEN])
{
    sg_flat_list target_flat;
    sg_index old_idx;
    sg_index new_idx;
    char bad_path[SG_PATH_MAX];
    size_t i;
    int rc = 0;
    int flatten_rc;

    flatten_rc = sg_tree_flatten(git_dir, tree_id, &target_flat, bad_path);
    if (flatten_rc == -2) {
        fprintf(stderr, "sg: path %s is invalid, refusing to flatten this tree into file paths\n",
               sg_quote_path_delimited(bad_path));
        return -1;
    }
    if (flatten_rc != 0) {
        fprintf(stderr, "sg: failed to read target tree\n");
        return -1;
    }

    if (sg_index_read(git_dir, &old_idx) != 0) {
        fprintf(stderr, "sg: failed to read index (corrupt?)\n");
        sg_flat_list_free(&target_flat);
        return -1;
    }

    memset(&new_idx, 0, sizeof(new_idx));
    for (i = 0; i < target_flat.count; i++) {
        sg_index_entry entry;
        int old_pos = sg_index_find(&old_idx, target_flat.entries[i].path);

        memset(&entry, 0, sizeof(entry));
        if (old_pos >= 0 &&
           memcmp(old_idx.entries[old_pos].sha1, target_flat.entries[i].sha1, SG_SHA1_RAW_LEN) == 0) {
            entry.ctime_sec = old_idx.entries[old_pos].ctime_sec;
            entry.ctime_nsec = old_idx.entries[old_pos].ctime_nsec;
            entry.mtime_sec = old_idx.entries[old_pos].mtime_sec;
            entry.mtime_nsec = old_idx.entries[old_pos].mtime_nsec;
            entry.dev = old_idx.entries[old_pos].dev;
            entry.ino = old_idx.entries[old_pos].ino;
            entry.uid = old_idx.entries[old_pos].uid;
            entry.gid = old_idx.entries[old_pos].gid;
            entry.file_size = old_idx.entries[old_pos].file_size;
        }
        entry.mode = target_flat.entries[i].mode;
        memcpy(entry.sha1, target_flat.entries[i].sha1, SG_SHA1_RAW_LEN);
        entry.path = target_flat.entries[i].path;

        if (sg_index_upsert(&new_idx, &entry) != 0) {
            fprintf(stderr, "sg: failed to stage %s\n", sg_quote_path_delimited(target_flat.entries[i].path));
            rc = -1;
        }
    }
    sg_index_free(&old_idx);
    sg_flat_list_free(&target_flat);

    if (rc == 0 && sg_index_write(git_dir, &new_idx) != 0) {
        fprintf(stderr, "sg: failed to write index\n");
        rc = -1;
    }
    sg_index_free(&new_idx);

    return rc;
}

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} strbuf;

static void strbuf_append(strbuf *b, const char *s)
{
    size_t slen = strlen(s);
    size_t needed = b->len + slen + 1;

    if (needed > b->cap) {
        size_t new_cap = b->cap == 0 ? 256 : b->cap * 2;
        char *grown;

        while (new_cap < needed)
            new_cap *= 2;
        grown = realloc(b->buf, new_cap);
        if (grown == NULL)
            return; /* best effort: message may end up truncated */
        b->buf = grown;
        b->cap = new_cap;
    }
    memcpy(b->buf + b->len, s, slen + 1);
    b->len += slen;
}

/* Appends directly rather than formatting into a fixed buffer first: a
   quoted path can reach four times its original length (every byte an octal
   escape), so the old char[4200] silently truncated the very paths most
   worth showing -- the ones full of control characters. strbuf_append
   already grows on demand. */
static void strbuf_append_path(strbuf *b, const char *prefix, const char *path)
{
    strbuf_append(b, "\t");
    strbuf_append(b, prefix);
    strbuf_append(b, sg_quote_path(path));
    strbuf_append(b, "\n");
}

int sg_safe_apply_tree(const char *git_dir, const char *repo_root,
                       const unsigned char tree_id[SG_SHA1_RAW_LEN],
                       const char *label, int force)
{
    unsigned char head_id[SG_SHA1_RAW_LEN];
    unsigned char head_tree[SG_SHA1_RAW_LEN];
    const unsigned char *head_tree_ptr = NULL; /* NULL == unborn HEAD == empty tree */
    sg_index idx;
    sg_status_list staged;
    sg_status_list unstaged;
    int has_head;
    int staged_rc, staged_ok, unstaged_ok;
    int dirty;
    char staged_bad_path[SG_PATH_MAX];
    size_t i;

    has_head = (sg_ref_resolve_head(git_dir, head_id) == 0);
    if (has_head && sg_commit_tree_of(git_dir, head_id, head_tree) == 0)
        head_tree_ptr = head_tree;

    if (sg_index_read(git_dir, &idx) != 0) {
        fprintf(stderr, "sg: failed to read index (corrupt?)\n");
        return -1;
    }

    /* Renames deliberately OFF: this list is enumerated below to tell the
       user what is uncommitted, and a rename row carries two paths where
       this loop prints one. See sg/status.h -- the parameter is mandatory
       precisely so this choice is made here rather than inherited. */
    staged_bad_path[0] = '\0';
    staged_rc = sg_status_diff_staged(git_dir, repo_root, head_tree_ptr, &idx, 0, NULL, &staged,
                                      staged_bad_path);
    staged_ok = staged_rc == 0;
    unstaged_ok = sg_status_diff_unstaged(git_dir, repo_root, &idx, &unstaged) == 0;

    /* A failed diff (e.g. out of memory) must NOT be read as "clean" -- that
       would silently defeat the whole point of this safety gate. An
       in-progress, unresolved merge or rebase is also "dirty": overwriting
       it here would silently discard the conflict resolution work (or the
       whole rebase sequence) in progress. */
    dirty = !staged_ok || !unstaged_ok || staged.count > 0 || unstaged.count > 0 ||
        sg_index_has_unmerged(&idx) || sg_rebase_state_exists(git_dir) ||
        sg_sequencer_kind_in_progress(git_dir) != 0;

    if (dirty) {
        strbuf msg = {0};
        int confirmed;

        strbuf_append(&msg, "sg: '");
        strbuf_append(&msg, label);
        strbuf_append(&msg,
                      "' will overwrite the current working directory and index with new "
                      "content, and the following uncommitted changes will be lost:\n");
        for (i = 0; i < unstaged.count; i++)
            strbuf_append_path(&msg, "modified (unstaged): ", unstaged.entries[i].path);
        for (i = 0; i < staged.count; i++)
            strbuf_append_path(&msg, "staged: ", staged.entries[i].path);
        if (staged_rc == -2 && staged_bad_path[0] != '\0') {
            strbuf_append(&msg, "sg: warning: HEAD's tree names an invalid path (");
            strbuf_append(&msg, sg_quote_path_delimited(staged_bad_path));
            strbuf_append(&msg, "), could not fully determine the working directory state; "
                          "requiring confirmation to be safe\n");
        } else if (!staged_ok || !unstaged_ok) {
            strbuf_append(&msg, "sg: warning: could not fully determine the working directory "
                          "state (possibly out of memory, or path too long); requiring "
                          "confirmation to be safe\n");
        }
        if (sg_index_has_unmerged(&idx))
            strbuf_append(&msg,
                          "sg: an unfinished merge is currently in progress; continuing will "
                          "abandon it\n");
        if (sg_rebase_state_exists(git_dir))
            strbuf_append(&msg, "sg: a rebase is in progress; continuing will overwrite the "
                          "conflict resolution content in the working directory\n"
                          "sg: to end this rebase, use `sg rebase --abort`\n");
        {
            sg_seq_kind seq_kind = sg_sequencer_kind_in_progress(git_dir);

            if (seq_kind != 0) {
                const char *op = seq_kind == SG_SEQ_CHERRY_PICK ? "cherry-pick" : "revert";

                strbuf_append(&msg, "sg: a ");
                strbuf_append(&msg, op);
                strbuf_append(&msg, " is in progress; continuing will overwrite the "
                              "conflict resolution content in the working directory\n"
                              "sg: to end it, use `sg ");
                strbuf_append(&msg, op);
                strbuf_append(&msg, " --abort`\n");
            }
        }

        confirmed = sg_confirm_dangerous(msg.buf != NULL ? msg.buf : "", force);
        free(msg.buf);

        if (!confirmed) {
            sg_status_list_free(&staged);
            sg_status_list_free(&unstaged);
            sg_index_free(&idx);
            return 1;
        }

        /* --force only skips the interactive prompt above; it must never
           skip taking the safety snapshot */
        {
            char snap_bad_path[SG_PATH_MAX];

            snap_bad_path[0] = '\0';
            if (sg_snapshot_create(git_dir, repo_root, &idx, label, NULL, snap_bad_path) != 0) {
                /* Phase 36 round 2: this is the first and only place in this
                   call that ever reaches sg_tree_build_from_workdir, unlike
                   sg_stash_push's own two calls where the earlier one always
                   fires first -- so a hostile index path can genuinely
                   surface HERE, and the generic message below used to be
                   the only thing printed for it. */
                if (snap_bad_path[0] != '\0')
                    fprintf(stderr, "sg: automatic snapshot failed: the index names an invalid "
                                    "path (%s); aborting this operation to be safe (no changes "
                                    "were made)\n",
                           sg_quote_path_delimited(snap_bad_path));
                else
                    fprintf(stderr, "sg: automatic snapshot failed; aborting this operation to be "
                                    "safe (no changes were made)\n");
                sg_status_list_free(&staged);
                sg_status_list_free(&unstaged);
                sg_index_free(&idx);
                return -1;
            }
        }
    }

    sg_status_list_free(&staged);
    sg_status_list_free(&unstaged);
    sg_index_free(&idx);

    {
        /* Existence, not parseability -- the value is never used, only the
           fact that a merge is in flight. Measured against real git 2.55.0:
           `reset --hard` clears a malformed MERGE_HEAD just as readily as a
           well-formed one. sg_merge_head_read would silently leave a corrupt
           one behind, which `switch` then refuses to move past forever. */
        int merge_in_progress = sg_merge_head_exists(git_dir);

        if (sg_apply_tree_to_workdir(git_dir, repo_root, tree_id) != 0)
            return -1;

        /* The apply above rebuilt the index from tree_id, wiping any conflict
           stages -- whatever merge was in flight is over. Leaving MERGE_HEAD
           behind would make the next unrelated `sg commit` silently record a
           bogus merge commit. Real git 2.55.0 behaves the same way: any
           operation that resets the working directory (e.g. `reset --hard`)
           clears MERGE_HEAD.

           A paused rebase's sequencer state, in contrast, is deliberately
           left alone here. Measured against real git 2.55.0: `reset --hard`
           during a paused rebase keeps `.git/rebase-merge` intact (and a
           later `rebase --abort`/`--continue` still works), while `switch`
           (even with `--force`) is refused outright instead of clobbering it.
           Only rebase's own subcommands (--abort, a completed run, --quit)
           are allowed to end a sequence. Callers of sg_safe_apply_tree that
           need the old "always wipe rebase state" behavior (currently only
           `sg undo`, which has no git equivalent to use as an oracle) clear
           it themselves after this call returns. */
        if (merge_in_progress && sg_merge_head_remove(git_dir) != 0)
            fprintf(stderr, "sg: warning: failed to clear MERGE_HEAD\n");
        return 0;
    }
}

/* Formerly a static helper duplicated in cmd_merge.c; extracted here so
   rebase can require the same precondition without a copy. */
int sg_require_clean_workdir(const char *git_dir, const char *repo_root, const char *what)
{
    unsigned char head_id[SG_SHA1_RAW_LEN];
    unsigned char head_tree[SG_SHA1_RAW_LEN];
    const unsigned char *head_tree_ptr = NULL; /* NULL == unborn HEAD == empty tree */
    sg_index idx;
    sg_status_list staged = {0};
    sg_status_list unstaged = {0};
    int staged_rc, staged_ok, unstaged_ok, dirty;
    char staged_bad_path[SG_PATH_MAX];
    size_t i;

    if (sg_index_read(git_dir, &idx) != 0) {
        fprintf(stderr, "sg: failed to read index (corrupt?)\n");
        return 1;
    }

    if (sg_ref_resolve_head(git_dir, head_id) == 0 &&
        sg_commit_tree_of(git_dir, head_id, head_tree) == 0)
        head_tree_ptr = head_tree;

    /* Renames deliberately OFF, same reason as the gate above: the rejection
       message below enumerates these rows one path at a time. */
    staged_bad_path[0] = '\0';
    staged_rc = sg_status_diff_staged(git_dir, repo_root, head_tree_ptr, &idx, 0, NULL, &staged,
                                      staged_bad_path);
    staged_ok = staged_rc == 0;
    unstaged_ok = sg_status_diff_unstaged(git_dir, repo_root, &idx, &unstaged) == 0;

    /* A failed diff must never read as "clean" -- same rule as the rest of
       the safety gates. */
    dirty = !staged_ok || !unstaged_ok || staged.count > 0 || unstaged.count > 0;

    if (dirty) {
        fprintf(stderr, "sg: %s requires a clean working directory, but the following changes "
                        "are not yet committed:\n", what);
        for (i = 0; i < staged.count; i++)
            fprintf(stderr, "\tstaged:              %s\n", sg_quote_path(staged.entries[i].path));
        for (i = 0; i < unstaged.count; i++)
            fprintf(stderr, "\tmodified (unstaged): %s\n", sg_quote_path(unstaged.entries[i].path));
        if (staged_rc == -2 && staged_bad_path[0] != '\0')
            fprintf(stderr, "sg: warning: HEAD's tree names an invalid path (%s), could not "
                            "fully determine the working directory state\n",
                   sg_quote_path_delimited(staged_bad_path));
        else if (!staged_ok || !unstaged_ok)
            fprintf(stderr, "sg: warning: could not fully determine the working directory "
                            "state (possibly out of memory, or path too long)\n");
        fprintf(stderr,
               "Please resolve these changes, then run again:\n"
               "  sg commit -m \"...\"      to commit them\n"
               "  sg restore <file>...    to discard the working directory changes\n");
    }

    sg_status_list_free(&staged);
    sg_status_list_free(&unstaged);
    sg_index_free(&idx);
    return dirty ? 1 : 0;
}

/* ---- Phase 79: untracked-overwrite pre-flight ---- */

static int path_tracked_any_stage(const sg_index *idx, const char *path)
{
    unsigned int stage;

    for (stage = 0; stage <= 3; stage++) {
        if (sg_index_find_stage(idx, path, stage) >= 0)
            return 1;
    }
    return 0;
}

void sg_untracked_overwrite_error_free(sg_untracked_overwrite_error *err)
{
    free(err->path);
    err->path = NULL;
    err->kind = SG_UNTRACKED_ERR_NONE;
    err->saved_errno = 0;
}

/* Pushes every proper ancestor of `path` (shortest first) onto `ig`, queries
   sg_ignore_is_ignored for `path` itself, then pops everything it pushed.
   Returns 0 and fills *out_ignored on success, -1 on an sg_ignore_push_dir
   allocation failure (everything already pushed is popped before returning,
   *out_ignored is left untouched). */
static int untracked_overwrite_is_ignored(sg_ignore *ig, const char *path, int is_dir,
                                          int *out_ignored)
{
    size_t i;
    size_t pushed = 0;
    int rc = 0;

    for (i = 0; path[i] != '\0'; i++) {
        if (path[i] == '/') {
            char prefix[SG_PATH_MAX];

            if (i >= sizeof(prefix)) {
                rc = -1;
                break;
            }
            memcpy(prefix, path, i);
            prefix[i] = '\0';
            if (sg_ignore_push_dir(ig, prefix) != 0) {
                rc = -1;
                break;
            }
            pushed++;
        }
    }

    if (rc == 0)
        *out_ignored = sg_ignore_is_ignored(ig, path, is_dir);

    while (pushed-- > 0)
        sg_ignore_pop_dir(ig);

    return rc;
}

/* Appends `x` to *arr (growing it). Measured against git 2.55.0 (correcting
   this function's own earlier "de-duplicated" doc comment): git does NOT
   de-duplicate -- two candidates blocked by the same ancestor each print
   that ancestor's name once, so a blocker named twice in the input produces
   TWO lines of output, not one. Returns 0 on success, -1 on allocation
   failure. */
static int untracked_overwrite_bucket_add(char ***arr, size_t *count, size_t *cap, const char *x)
{
    if (*count == *cap) {
        size_t new_cap = *cap == 0 ? 8 : *cap * 2;
        char **grown = realloc(*arr, new_cap * sizeof(**arr));

        if (grown == NULL)
            return -1;
        *arr = grown;
        *cap = new_cap;
    }
    (*arr)[*count] = strdup(x);
    if ((*arr)[*count] == NULL)
        return -1;
    (*count)++;
    return 0;
}

/* Records the FIRST failure only: every call site below either bails
   immediately on -1 (nothing further can overwrite what was already
   recorded) or, in the recursive case, simply propagates a child's -1
   upward without calling this again -- so `err` is filled exactly once per
   top-level sg_untracked_would_be_overwritten call. `path` may be NULL
   (SG_UNTRACKED_ERR_ALLOC never has one; a strdup failure while trying to
   record some other kind's path also leaves it NULL rather than losing the
   original error to a second allocation failure). Always returns -1, so
   call sites can `return set_untracked_err(...)`. */
static int set_untracked_err(sg_untracked_overwrite_error *err, sg_untracked_err_kind kind,
                             const char *path, int saved_errno)
{
    err->kind = kind;
    free(err->path);
    err->path = path != NULL ? strdup(path) : NULL;
    err->saved_errno = saved_errno;
    return -1;
}

/* Phase 79c (F3): a growable heap buffer holding the CURRENT relative path
   being scanned, shared across an entire untracked_overwrite_dir_scan
   recursion instead of each stack frame holding its own SG_PATH_MAX-sized
   copy -- a chain of ~2000 one-letter untracked directories is a real,
   reachable shape (SG_PATH_MAX == PATH_MAX == 4096 on Linux) and used to
   need ~24MB of stack for that alone. push()/pop() extend and truncate the
   SAME buffer in place; only one exists for the whole call, not one per
   recursion level. */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} untracked_path_accum;

static int untracked_path_accum_init(untracked_path_accum *pa, const char *start)
{
    pa->len = strlen(start);
    pa->cap = pa->len + 1 > 256 ? pa->len + 1 : 256;
    pa->buf = malloc(pa->cap);
    if (pa->buf == NULL)
        return -1;
    memcpy(pa->buf, start, pa->len + 1);
    return 0;
}

static void untracked_path_accum_free(untracked_path_accum *pa)
{
    free(pa->buf);
    pa->buf = NULL;
}

/* Appends "/name" to the buffer (growing it if needed), returning the
   PREVIOUS length in *out_old_len so the caller can truncate back with
   untracked_path_accum_pop. Returns -1 on allocation failure, leaving the
   buffer at its old content/length. */
static int untracked_path_accum_push(untracked_path_accum *pa, const char *name,
                                     size_t *out_old_len)
{
    size_t nlen = strlen(name);
    size_t need = pa->len + 1 + nlen + 1;

    *out_old_len = pa->len;
    if (need > pa->cap) {
        size_t new_cap = pa->cap * 2;
        char *grown;

        if (new_cap < need)
            new_cap = need;
        grown = realloc(pa->buf, new_cap);
        if (grown == NULL)
            return -1;
        pa->buf = grown;
        pa->cap = new_cap;
    }
    pa->buf[pa->len] = '/';
    memcpy(pa->buf + pa->len + 1, name, nlen + 1);
    pa->len += 1 + nlen;
    return 0;
}

static void untracked_path_accum_pop(untracked_path_accum *pa, size_t old_len)
{
    pa->len = old_len;
    pa->buf[pa->len] = '\0';
}

static int untracked_overwrite_dir_scan(sg_ignore *ig, const char *repo_root,
                                        const sg_index *idx, untracked_path_accum *pa,
                                        char *abs_scratch, int *out_found,
                                        sg_untracked_overwrite_error *err);

/* Pushes proper ancestors of `path` (shortest first, same convention as
   untracked_overwrite_is_ignored above), then recursively scans `path`
   itself for at least one entry that is not ignored, then pops everything
   it pushed. See sg_untracked_would_be_overwritten's own header comment
   (Phase 79b) for the exact rules this scan follows.

   Phase 80 (F3b/F2): `idx` is an optional tracked-path exemption -- when
   non-NULL, an entry tracked at any stage does not count toward "found",
   the same way an ignored one does not (but, unlike an ignored directory,
   a tracked path is still just skipped, not a reason to avoid recursing
   into a directory that has other, non-tracked content). Pass NULL for the
   Phase 79/79b/79c write-time behavior (also what sg_worktree_clear_write_
   path passes); sg_untracked_would_be_overwritten's own pre-flight caller
   passes its own idx.

   Returns 0 and fills *out_found on success; -1 on any opendir/readdir/
   lstat/ignore-engine failure encountered anywhere in the scan (all pushed
   ancestors are popped before returning either way), with `err` describing
   which. */
static int untracked_overwrite_dir_has_nonignored(sg_ignore *ig, const char *repo_root,
                                                   const char *path, const sg_index *idx,
                                                   int *out_found,
                                                   sg_untracked_overwrite_error *err)
{
    untracked_path_accum pa;
    char *abs_scratch;
    size_t i;
    size_t pushed = 0;
    int rc = 0;

    if (untracked_path_accum_init(&pa, path) != 0)
        return set_untracked_err(err, SG_UNTRACKED_ERR_ALLOC, NULL, 0);
    /* Phase 79c (F3): one shared SG_PATH_MAX scratch buffer for every
       opendir()/lstat() absolute path built anywhere in the recursion below
       -- heap-allocated once here rather than a fresh stack array per
       frame, same rationale as untracked_path_accum above. sg_path_join
       still enforces the SG_PATH_MAX bound on every use, so a path that
       does not fit still fails closed exactly as before. */
    abs_scratch = malloc(SG_PATH_MAX);
    if (abs_scratch == NULL) {
        untracked_path_accum_free(&pa);
        return set_untracked_err(err, SG_UNTRACKED_ERR_ALLOC, NULL, 0);
    }

    for (i = 0; path[i] != '\0'; i++) {
        if (path[i] == '/') {
            char prefix[SG_PATH_MAX];

            if (i >= sizeof(prefix)) {
                rc = set_untracked_err(err, SG_UNTRACKED_ERR_PATH_TOO_LONG, path, ENAMETOOLONG);
                break;
            }
            memcpy(prefix, path, i);
            prefix[i] = '\0';
            if (sg_ignore_push_dir(ig, prefix) != 0) {
                rc = set_untracked_err(err, SG_UNTRACKED_ERR_ALLOC, NULL, 0);
                break;
            }
            pushed++;
        }
    }

    if (rc == 0)
        rc = untracked_overwrite_dir_scan(ig, repo_root, idx, &pa, abs_scratch, out_found, err);

    while (pushed-- > 0)
        sg_ignore_pop_dir(ig);

    free(abs_scratch);
    untracked_path_accum_free(&pa);
    return rc;
}

/* Pushes `pa->buf` itself onto `ig` (so a .gitignore inside it applies to
   its own children), then walks its direct entries: a file (or symlink,
   treated as a file, never descended into) that is not ignored makes the
   whole scan report "found" immediately; a subdirectory that is itself
   ignored is skipped without recursing into it (an ignored subtree can
   never contribute a non-ignored file to the outer answer); any other
   subdirectory is recursed into. Pops `pa->buf` before returning. Returns 0
   and fills *out_found on success, -1 on any opendir/readdir/lstat/
   ignore-engine failure, filling `err` with which one and (where
   applicable) the repo-relative path and errno involved -- see
   sg_untracked_err_kind's own comment.

   Phase 79c (F2): an opendir/readdir/lstat failure here used to propagate
   as a bare -1 that cmd_merge.c printed as "sg: out of memory" regardless
   of the real cause -- measured against git 2.55.0 (S6 in the Phase 79c
   oracle), a chmod-000 subdirectory is a PERMISSION error, not an
   allocation one, and git names the directory and the real reason. `err`
   is how that distinction survives the trip back up the recursion.

   readdir()'s own failure is detected the POSIX way: errno is cleared right
   before each call, and a NULL return with errno left non-zero afterward is
   a real failure rather than "no more entries" (readdir does not guarantee
   errno is left unchanged on a clean end-of-directory, only that it is set
   on failure). */
static int untracked_overwrite_dir_scan(sg_ignore *ig, const char *repo_root,
                                        const sg_index *idx, untracked_path_accum *pa,
                                        char *abs_scratch, int *out_found,
                                        sg_untracked_overwrite_error *err)
{
    DIR *d;
    struct dirent *ent;
    int rc = 0;
    int found = 0;

    if (sg_path_join(abs_scratch, SG_PATH_MAX, repo_root, pa->buf) != 0)
        return set_untracked_err(err, SG_UNTRACKED_ERR_PATH_TOO_LONG, pa->buf, ENAMETOOLONG);

    d = opendir(abs_scratch);
    if (d == NULL)
        return set_untracked_err(err, SG_UNTRACKED_ERR_OPENDIR, pa->buf, errno);

    if (sg_ignore_push_dir(ig, pa->buf) != 0) {
        closedir(d);
        return set_untracked_err(err, SG_UNTRACKED_ERR_ALLOC, NULL, 0);
    }

    errno = 0;
    while (!found && (ent = readdir(d)) != NULL) {
        struct stat st;
        int ignored;
        size_t old_len;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            errno = 0;
            continue;
        }

        if (untracked_path_accum_push(pa, ent->d_name, &old_len) != 0) {
            rc = set_untracked_err(err, SG_UNTRACKED_ERR_ALLOC, NULL, 0);
            break;
        }

        if (sg_path_join(abs_scratch, SG_PATH_MAX, repo_root, pa->buf) != 0) {
            rc = set_untracked_err(err, SG_UNTRACKED_ERR_PATH_TOO_LONG, pa->buf, ENAMETOOLONG);
            untracked_path_accum_pop(pa, old_len);
            break;
        }
        if (lstat(abs_scratch, &st) != 0) {
            rc = set_untracked_err(err, SG_UNTRACKED_ERR_LSTAT, pa->buf, errno);
            untracked_path_accum_pop(pa, old_len);
            break;
        }

        ignored = sg_ignore_is_ignored(ig, pa->buf, S_ISDIR(st.st_mode) ? 1 : 0);
        if (S_ISDIR(st.st_mode)) {
            int sub_found = 0;

            if (!ignored &&
                untracked_overwrite_dir_scan(ig, repo_root, idx, pa, abs_scratch, &sub_found,
                                             err) != 0) {
                rc = -1;
                untracked_path_accum_pop(pa, old_len);
                break;
            }
            if (sub_found)
                found = 1;
        } else if (!ignored && !(idx != NULL && path_tracked_any_stage(idx, pa->buf))) {
            /* Phase 80 (F3b): a path tracked in idx (any stage) is not
               untracked content, so it does not make this directory
               "blocking" -- the same directory can still be reported for a
               DIFFERENT, genuinely untracked file elsewhere inside it. */
            found = 1;
        }
        untracked_path_accum_pop(pa, old_len);
        errno = 0;
    }
    if (rc == 0 && ent == NULL && errno != 0)
        rc = set_untracked_err(err, SG_UNTRACKED_ERR_READDIR, pa->buf, errno);

    sg_ignore_pop_dir(ig);
    closedir(d);

    if (rc != 0)
        return -1;

    *out_found = found;
    return 0;
}

int sg_untracked_would_be_overwritten(const char *git_dir, const char *repo_root,
                                      const sg_index *idx,
                                      const char *const *paths, size_t count,
                                      char ***out_files, size_t *out_files_count,
                                      char ***out_dirs, size_t *out_dirs_count,
                                      int *out_first_is_dir,
                                      sg_untracked_overwrite_error *out_err)
{
    sg_ignore *ig;
    char **file_collisions = NULL;
    size_t file_count = 0;
    size_t file_cap = 0;
    char **dir_collisions = NULL;
    size_t dir_count = 0;
    size_t dir_cap = 0;
    size_t i;
    int rc = 0;
    int first_collision_seen = 0;
    sg_untracked_overwrite_error err_local;
    sg_untracked_overwrite_error *err = out_err != NULL ? out_err : &err_local;

    *out_files = NULL;
    *out_files_count = 0;
    *out_dirs = NULL;
    *out_dirs_count = 0;
    if (out_first_is_dir != NULL)
        *out_first_is_dir = 0;
    err->kind = SG_UNTRACKED_ERR_NONE;
    err->path = NULL;
    err->saved_errno = 0;

    if (count == 0)
        return 0;

    if (sg_ignore_open(&ig, git_dir, repo_root) != 0) {
        set_untracked_err(err, SG_UNTRACKED_ERR_ALLOC, NULL, 0);
        if (out_err == NULL)
            free(err_local.path);
        return -1;
    }

    for (i = 0; i < count && rc == 0; i++) {
        const char *p = paths[i];
        char abspath[SG_PATH_MAX];
        struct stat st;
        int have_x = 0;
        int check_ignore = 1;
        char x_buf[SG_PATH_MAX];
        const char *x = NULL;
        int x_is_dir = 0;

        /* Phase 80 (F3a): walk P's proper ancestors shortest-first FIRST,
           unconditionally -- not only when lstat(P) itself fails ENOTDIR.
           A symlinked ancestor (A4/A6/A7/A8's shape: "a" is a symlink,
           candidate is "a/b/c.txt") makes lstat(P) silently follow it and
           answer about whatever "a" points at instead of about "a" itself
           -- ENOENT if the target doesn't have "b/c.txt", or even success
           if it does -- neither of which is "no collision along this
           path", the wrong direction for a security-relevant check. The
           ancestor's own lstat is never followed either (it inspects the
           symlink, not its target), so this loop finds "a" itself as the
           blocker regardless of what it points at or whether the target
           exists at all. */
        {
            size_t j;

            for (j = 0; p[j] != '\0' && !have_x; j++) {
                if (p[j] != '/')
                    continue;
                {
                    char prefix[SG_PATH_MAX];
                    char anc_abs[SG_PATH_MAX];
                    struct stat anc_st;

                    if (j >= sizeof(prefix)) {
                        x = p;
                        have_x = 1;
                        check_ignore = 0;
                        break;
                    }
                    memcpy(prefix, p, j);
                    prefix[j] = '\0';
                    if (sg_path_join(anc_abs, sizeof(anc_abs), repo_root, prefix) != 0) {
                        x = p;
                        have_x = 1;
                        check_ignore = 0;
                        break;
                    }
                    if (lstat(anc_abs, &anc_st) != 0) {
                        /* ENOENT: nothing here yet, keep walking (P itself
                           might still exist relative to this missing
                           ancestor being about to be created, handled
                           below). Anything else (EACCES, ...) could not be
                           verified -- leave it to the P-based fallback
                           below, which will hit the identical lstat error
                           on P (or an ancestor of it) and fail closed
                           there. */
                        continue;
                    }
                    if (!S_ISDIR(anc_st.st_mode)) {
                        memcpy(x_buf, prefix, j + 1);
                        x = x_buf;
                        have_x = 1;
                        x_is_dir = 0;
                        break;
                    }
                    /* real directory: keep walking */
                }
            }
        }

        if (have_x) {
            /* found via the ancestor walk above */
        } else if (sg_path_join(abspath, sizeof(abspath), repo_root, p) != 0) {
            /* Truncated -- can't even be verified, same fail-closed
               direction as every other path-joining site in this project:
               report it rather than risk "clear". */
            x = p;
            have_x = 1;
            check_ignore = 0;
        } else if (lstat(abspath, &st) == 0) {
            /* Row #7 of the oracle: an untracked EMPTY directory sitting
               exactly at the path the merge wants to create does not block
               it -- measured, git removes the empty directory without a
               word. This used to be decided HERE, by a dedicated
               dir_is_empty() call that cleared have_x on an empty directory
               before the tracked/ignored/report path below ever ran.
               Phase 79 round 3's mutation battery (u06) found that guard
               redundant: since Phase 79b, x_is_dir falls through to the
               recursive untracked_overwrite_dir_has_nonignored() scan
               below, and an empty directory's scan finds nothing at any
               depth (ignored or not) either -- found stays 0, so the
               "not a collision" answer is now produced by that scan
               instead, with no separate check needed here. Verified
               (Phase 76 R4-1's two-part test): same final answer AND no
               side effect depended on the deleted guard, since the scan
               only reads (opendir + .gitignore queries). */
            x = p;
            have_x = 1;
            x_is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
        } else if (errno == ENOENT || errno == ENOTDIR) {
            /* ENOENT: nothing along this path at all -- clear. ENOTDIR
               should already have been caught by the ancestor walk above;
               kept here as a fail-safe (a race could remove the ancestor
               between the two lstats) rather than reported, matching this
               function's own pre-Phase-80 "loop ran to completion" note. */
            have_x = 0;
        } else {
            /* EACCES, ENAMETOOLONG, ELOOP, ... -- could not be verified,
               same fail-closed direction as the truncation case above. */
            x = p;
            have_x = 1;
            check_ignore = 0;
        }

        if (!have_x)
            continue;

        if (path_tracked_any_stage(idx, x))
            continue;

        if (check_ignore) {
            int ignored = 0;

            if (untracked_overwrite_is_ignored(ig, x, x_is_dir, &ignored) != 0) {
                rc = set_untracked_err(err, SG_UNTRACKED_ERR_ALLOC, NULL, 0);
                break;
            }
            if (ignored)
                continue;
        }

        /* Phase 79b: a non-empty untracked directory is only a collision if
           it recursively contains at least one non-ignored file -- unlike a
           blocking file, which is an unconditional collision once it is
           reached here. check_ignore is only 0 on a fail-closed path
           (truncation/EACCES/...), none of which can have x_is_dir set, so
           this scan only ever runs when x really was lstat'd as a
           directory. */
        if (x_is_dir) {
            int found = 0;

            if (untracked_overwrite_dir_has_nonignored(ig, repo_root, x, idx, &found, err) != 0) {
                rc = -1;
                break;
            }
            if (!found)
                continue;
        }

        /* Phase 79c (F4): record which BUCKET the very first collision (in
           candidate order) landed in, before adding it -- this is what lets
           the unborn-HEAD caller in cmd_merge.c pick the correct one of the
           four wordings when both buckets would otherwise fire. Measured
           against git 2.55.0 (U1b in the Phase 79c oracle): git reports
           exactly the FIRST collision across BOTH kinds, not "directory
           wins" -- a topic that adds "a" then "z", with a local file "a"
           and a local non-empty dir "z", refuses on the FILE wording
           naming "a", not the directory wording. */
        if (!first_collision_seen && out_first_is_dir != NULL) {
            *out_first_is_dir = x_is_dir;
            first_collision_seen = 1;
        }

        if (x_is_dir) {
            if (untracked_overwrite_bucket_add(&dir_collisions, &dir_count, &dir_cap, x) != 0) {
                rc = set_untracked_err(err, SG_UNTRACKED_ERR_ALLOC, NULL, 0);
                break;
            }
        } else {
            if (untracked_overwrite_bucket_add(&file_collisions, &file_count, &file_cap, x) != 0) {
                rc = set_untracked_err(err, SG_UNTRACKED_ERR_ALLOC, NULL, 0);
                break;
            }
        }
    }

    sg_ignore_free(ig);

    if (rc != 0) {
        size_t k;

        for (k = 0; k < file_count; k++)
            free(file_collisions[k]);
        free(file_collisions);
        for (k = 0; k < dir_count; k++)
            free(dir_collisions[k]);
        free(dir_collisions);
        if (out_err == NULL)
            free(err_local.path);
        return -1;
    }

    /* Neither bucket is sorted or de-duplicated here -- each is reported in
       the CALLER's candidate order, one entry per candidate that collided
       (see untracked_overwrite_bucket_add's own comment on why not
       de-duplicating matches git). A caller whose candidates happen to
       already be in path order (e.g. sg_tree_flatten's output) gets output
       that looks sorted as a side effect, but that is not a guarantee this
       function makes. */
    *out_files = file_collisions;
    *out_files_count = file_count;
    *out_dirs = dir_collisions;
    *out_dirs_count = dir_count;
    return 0;
}

/* Phase 80 (F2): removes relpath's WHOLE subtree, lstat-based -- unlinks
   every file and symlink it finds (never opendir/stat THROUGH a symlink, so
   an ignored symlink pointing outside the repo is unlinked itself and its
   target is never touched), then rmdir's every directory bottom-up,
   including relpath itself. pa must already be initialized to relpath;
   abs_scratch is a shared SG_PATH_MAX scratch buffer, same convention as
   untracked_overwrite_dir_scan. Returns 0, or -1 on the first removal
   failure (leaves whatever could not be removed; everything already removed
   stays removed, no rollback). */
static int untracked_overwrite_remove_subtree(const char *repo_root, untracked_path_accum *pa,
                                              char *abs_scratch)
{
    DIR *d;
    struct dirent *ent;
    int rc = 0;

    if (sg_path_join(abs_scratch, SG_PATH_MAX, repo_root, pa->buf) != 0)
        return -1;

    d = opendir(abs_scratch);
    if (d == NULL)
        return -1;

    errno = 0;
    while ((ent = readdir(d)) != NULL) {
        struct stat st;
        size_t old_len;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            errno = 0;
            continue;
        }

        if (untracked_path_accum_push(pa, ent->d_name, &old_len) != 0) {
            rc = -1;
            break;
        }
        if (sg_path_join(abs_scratch, SG_PATH_MAX, repo_root, pa->buf) != 0) {
            rc = -1;
            untracked_path_accum_pop(pa, old_len);
            break;
        }
        if (lstat(abs_scratch, &st) != 0) {
            rc = -1;
            untracked_path_accum_pop(pa, old_len);
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            if (untracked_overwrite_remove_subtree(repo_root, pa, abs_scratch) != 0) {
                rc = -1;
                untracked_path_accum_pop(pa, old_len);
                break;
            }
        } else if (unlink(abs_scratch) != 0) {
            rc = -1;
            untracked_path_accum_pop(pa, old_len);
            break;
        }
        untracked_path_accum_pop(pa, old_len);
        errno = 0;
    }
    if (rc == 0 && ent == NULL && errno != 0)
        rc = -1;

    closedir(d);
    if (rc != 0)
        return -1;

    if (sg_path_join(abs_scratch, SG_PATH_MAX, repo_root, pa->buf) != 0)
        return -1;
    return rmdir(abs_scratch) == 0 ? 0 : -1;
}

int sg_worktree_clear_write_path(sg_ignore *ig, const char *repo_root, const char *relpath,
                                 const sg_index *idx)
{
    size_t i;

    /* (a) ancestors, shortest first -- same walk sg_untracked_would_be_
       overwritten's own F3a fix uses, but here a blocker is either cleared
       (ignored) or fails the whole call outright, never merely reported. */
    for (i = 0; relpath[i] != '\0'; i++) {
        char prefix[SG_PATH_MAX];
        char anc_abs[SG_PATH_MAX];
        struct stat anc_st;
        int ignored = 0;

        if (relpath[i] != '/')
            continue;

        if (i >= sizeof(prefix))
            return -1;
        memcpy(prefix, relpath, i);
        prefix[i] = '\0';
        if (sg_path_join(anc_abs, sizeof(anc_abs), repo_root, prefix) != 0)
            return -1;

        if (lstat(anc_abs, &anc_st) != 0) {
            if (errno == ENOENT)
                return 0; /* nothing here yet: sg_write_file_worktree mkdir's the rest */
            return -1;
        }
        if (S_ISDIR(anc_st.st_mode))
            continue; /* real directory, keep walking */

        if (untracked_overwrite_is_ignored(ig, prefix, 0, &ignored) != 0)
            return -1;
        if (!ignored)
            return -1; /* real blocker: fail closed, do not remove it */
        if (unlink(anc_abs) != 0)
            return -1;
        return 0; /* cleared; nothing below it existed */
    }

    /* (b) relpath itself. */
    {
        char abs[SG_PATH_MAX];
        struct stat st;
        int self_ignored = 0;
        int found = 0;

        if (sg_path_join(abs, sizeof(abs), repo_root, relpath) != 0)
            return -1;
        if (lstat(abs, &st) != 0)
            return 0; /* ENOENT, or unverifiable -- the write's own open() will fail loudly */
        if (!S_ISDIR(st.st_mode))
            return 0; /* symlink or regular file: sg_write_file_worktree unlinks it */

        if (untracked_overwrite_is_ignored(ig, relpath, 1, &self_ignored) != 0)
            return -1;
        if (!self_ignored) {
            sg_untracked_overwrite_error scratch_err;

            scratch_err.kind = SG_UNTRACKED_ERR_NONE;
            scratch_err.path = NULL;
            scratch_err.saved_errno = 0;
            if (untracked_overwrite_dir_has_nonignored(ig, repo_root, relpath, idx, &found,
                                                       &scratch_err) != 0) {
                sg_untracked_overwrite_error_free(&scratch_err);
                return -1;
            }
            if (found)
                return -1;
        }

        {
            untracked_path_accum pa;
            char *abs_scratch;
            int rc;

            if (untracked_path_accum_init(&pa, relpath) != 0)
                return -1;
            abs_scratch = malloc(SG_PATH_MAX);
            if (abs_scratch == NULL) {
                untracked_path_accum_free(&pa);
                return -1;
            }
            rc = untracked_overwrite_remove_subtree(repo_root, &pa, abs_scratch);
            free(abs_scratch);
            untracked_path_accum_free(&pa);
            return rc;
        }
    }
}
