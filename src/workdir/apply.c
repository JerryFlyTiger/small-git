#include "sg/apply.h"

#include "sg/chunk.h"
#include "sg/confirm.h"
#include "sg/index.h"
#include "sg/merge.h"
#include "sg/objstore.h"
#include "sg/object.h"
#include "sg/rebase.h"
#include "sg/refs.h"
#include "sg/snapshot.h"
#include "sg/status.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
    size_t i;
    int rc = 0;

    if (sg_tree_flatten(git_dir, tree_id, &target_flat) != 0) {
        fprintf(stderr, "sg: failed to read target tree\n");
        return -1;
    }

    if (sg_index_read(git_dir, &old_idx) != 0) {
        fprintf(stderr, "sg: failed to read index (corrupt?)\n");
        sg_flat_list_free(&target_flat);
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
            char abspath[4096];

            snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, old_idx.entries[i].path);
            remove(abspath);
        }
    }
    sg_index_free(&old_idx);

    memset(&new_idx, 0, sizeof(new_idx));
    for (i = 0; i < target_flat.count; i++) {
        char abspath[4096];
        unsigned char *blob_content;
        size_t blob_len;
        struct stat st;
        sg_index_entry entry;

        snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, target_flat.entries[i].path);
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
                fprintf(stderr, "sg: missing blob for '%s'\n", target_flat.entries[i].path);
                rc = -1;
                continue;
            }
        }
        if (sg_write_file_mkdirs(abspath, blob_content, blob_len,
                                 (int)(target_flat.entries[i].mode & 0777)) != 0) {
            fprintf(stderr, "sg: failed to write '%s'\n", target_flat.entries[i].path);
            free(blob_content);
            rc = -1;
            continue;
        }
        free(blob_content);

        if (stat(abspath, &st) != 0) {
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
        entry.file_size = (unsigned int)st.st_size;
        memcpy(entry.sha1, target_flat.entries[i].sha1, SG_SHA1_RAW_LEN);
        entry.path = target_flat.entries[i].path;

        if (sg_index_upsert(&new_idx, &entry) != 0) {
            fprintf(stderr, "sg: failed to stage '%s'\n", target_flat.entries[i].path);
            rc = -1;
        }
    }
    sg_flat_list_free(&target_flat);

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
    size_t i;
    int rc = 0;

    if (sg_tree_flatten(git_dir, tree_id, &target_flat) != 0) {
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
            fprintf(stderr, "sg: failed to stage '%s'\n", target_flat.entries[i].path);
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

static void strbuf_append_path(strbuf *b, const char *prefix, const char *path)
{
    char line[4200];

    snprintf(line, sizeof(line), "\t%s%s\n", prefix, path);
    strbuf_append(b, line);
}

int sg_safe_apply_tree(const char *git_dir, const char *repo_root,
                       const unsigned char tree_id[SG_SHA1_RAW_LEN],
                       const char *label, int force)
{
    unsigned char head_id[SG_SHA1_RAW_LEN];
    sg_flat_list head_flat;
    sg_index idx;
    sg_status_list staged;
    sg_status_list unstaged;
    int has_head;
    int staged_ok, unstaged_ok;
    int dirty;
    size_t i;

    memset(&head_flat, 0, sizeof(head_flat));
    has_head = (sg_ref_resolve_head(git_dir, head_id) == 0);
    if (has_head) {
        sg_obj_type type;
        unsigned char *content = NULL;
        size_t content_len;
        sg_commit commit;

        if (sg_object_read(git_dir, head_id, &type, &content, &content_len) == 0 &&
           type == SG_OBJ_COMMIT) {
            if (sg_commit_parse(content, content_len, &commit) == 0) {
                sg_tree_flatten(git_dir, commit.tree, &head_flat);
                sg_commit_free(&commit);
            }
            free(content);
        }
    }

    if (sg_index_read(git_dir, &idx) != 0) {
        sg_flat_list_free(&head_flat);
        fprintf(stderr, "sg: failed to read index (corrupt?)\n");
        return -1;
    }

    staged_ok = sg_status_diff_staged(&head_flat, &idx, &staged) == 0;
    sg_flat_list_free(&head_flat);
    unstaged_ok = sg_status_diff_unstaged(git_dir, repo_root, &idx, &unstaged) == 0;

    /* A failed diff (e.g. out of memory) must NOT be read as "clean" -- that
       would silently defeat the whole point of this safety gate. An
       in-progress, unresolved merge or rebase is also "dirty": overwriting
       it here would silently discard the conflict resolution work (or the
       whole rebase sequence) in progress. */
    dirty = !staged_ok || !unstaged_ok || staged.count > 0 || unstaged.count > 0 ||
        sg_index_has_unmerged(&idx) || sg_rebase_state_exists(git_dir);

    if (dirty) {
        strbuf msg = {0};
        int confirmed;

        strbuf_append(&msg, "sg: '");
        strbuf_append(&msg, label);
        strbuf_append(&msg,
                      "' 會用新的內容覆蓋目前的工作目錄與 index，"
                      "下列尚未提交的變更會遺失：\n");
        for (i = 0; i < unstaged.count; i++)
            strbuf_append_path(&msg, "modified (unstaged): ", unstaged.entries[i].path);
        for (i = 0; i < staged.count; i++)
            strbuf_append_path(&msg, "staged: ", staged.entries[i].path);
        if (!staged_ok || !unstaged_ok)
            strbuf_append(&msg, "sg: 警告：無法完整判斷工作目錄狀態（可能記憶體不足），為安全起見要求確認\n");
        if (sg_index_has_unmerged(&idx))
            strbuf_append(&msg, "sg: 目前有一個尚未完成的合併，繼續會放棄它\n");
        if (sg_rebase_state_exists(git_dir))
            strbuf_append(&msg, "sg: 目前有一個進行中的 rebase，繼續會覆蓋工作目錄裡的衝突解決內容\n"
                          "sg: 要結束這個 rebase 請用 `sg rebase --abort`\n");

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
        if (sg_snapshot_create(git_dir, repo_root, &idx, label, NULL) != 0) {
            fprintf(stderr, "sg: 自動快照失敗，為了安全起見中止這次操作（沒有做任何變更）\n");
            sg_status_list_free(&staged);
            sg_status_list_free(&unstaged);
            sg_index_free(&idx);
            return -1;
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
            fprintf(stderr, "sg: warning: 未能清除 MERGE_HEAD\n");
        return 0;
    }
}

/* Formerly a static helper duplicated in cmd_merge.c; extracted here so
   rebase can require the same precondition without a copy. */
int sg_require_clean_workdir(const char *git_dir, const char *repo_root, const char *what)
{
    unsigned char head_id[SG_SHA1_RAW_LEN];
    sg_flat_list head_flat;
    sg_index idx;
    sg_status_list staged = {0};
    sg_status_list unstaged = {0};
    int staged_ok, unstaged_ok, dirty;
    size_t i;

    if (sg_index_read(git_dir, &idx) != 0) {
        fprintf(stderr, "sg: failed to read index (corrupt?)\n");
        return 1;
    }

    memset(&head_flat, 0, sizeof(head_flat));
    if (sg_ref_resolve_head(git_dir, head_id) == 0) {
        unsigned char tree_id[SG_SHA1_RAW_LEN];

        if (sg_commit_tree_of(git_dir, head_id, tree_id) == 0)
            sg_tree_flatten(git_dir, tree_id, &head_flat);
    }

    staged_ok = sg_status_diff_staged(&head_flat, &idx, &staged) == 0;
    sg_flat_list_free(&head_flat);
    unstaged_ok = sg_status_diff_unstaged(git_dir, repo_root, &idx, &unstaged) == 0;

    /* A failed diff must never read as "clean" -- same rule as the rest of
       the safety gates. */
    dirty = !staged_ok || !unstaged_ok || staged.count > 0 || unstaged.count > 0;

    if (dirty) {
        fprintf(stderr, "sg: %s 需要乾淨的工作目錄，但下列變更尚未提交：\n", what);
        for (i = 0; i < staged.count; i++)
            fprintf(stderr, "\tstaged:              %s\n", staged.entries[i].path);
        for (i = 0; i < unstaged.count; i++)
            fprintf(stderr, "\tmodified (unstaged): %s\n", unstaged.entries[i].path);
        if (!staged_ok || !unstaged_ok)
            fprintf(stderr, "sg: 警告：無法完整判斷工作目錄狀態（可能記憶體不足）\n");
        fprintf(stderr,
               "請先處理這些變更，再重新執行：\n"
               "  sg commit -m \"...\"      把它們提交進來\n"
               "  sg restore <file>...    丟棄工作目錄的修改\n");
    }

    sg_status_list_free(&staged);
    sg_status_list_free(&unstaged);
    sg_index_free(&idx);
    return dirty ? 1 : 0;
}
