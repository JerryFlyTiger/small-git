#include "sg/cli.h"

#include "sg/apply.h"
#include "sg/chunk.h"
#include "sg/hash.h"
#include "sg/index.h"
#include "sg/loose.h"
#include "sg/merge.h"
#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/rebase.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/snapshot.h"
#include "sg/status.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char *env_or(const char *name, const char *fallback)
{
    const char *v = getenv(name);

    return (v != NULL && v[0] != '\0') ? v : fallback;
}

static int do_fast_forward(const char *git_dir, const char *repo_root, const char *current_branch,
                           const char *branch_arg, const unsigned char theirs_commit[SG_SHA1_RAW_LEN],
                           const unsigned char theirs_tree[SG_SHA1_RAW_LEN], int force)
{
    char label[300];
    int apply_rc;

    snprintf(label, sizeof(label), "merge %s (fast-forward)", branch_arg);
    apply_rc = sg_safe_apply_tree(git_dir, repo_root, theirs_tree, label, force);
    if (apply_rc == 1) {
        fprintf(stderr, "sg: merge aborted\n");
        return 1;
    }
    if (apply_rc != 0)
        return 1;

    if (sg_ref_update_branch(git_dir, current_branch, theirs_commit) != 0) {
        fprintf(stderr, "sg: failed to update branch '%s'\n", current_branch);
        return 1;
    }

    printf("Fast-forward\n");
    return 0;
}

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
    char abspath[4096];
    struct stat st;
    sg_index_entry entry;

    memset(&entry, 0, sizeof(entry));
    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, path);
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

static void print_conflict_message(char **conflict_paths, size_t conflict_count)
{
    size_t i;

    fprintf(stderr, "自動合併失敗，以下檔案有衝突：\n");
    for (i = 0; i < conflict_count; i++)
        fprintf(stderr, "    %s\n", conflict_paths[i]);
    fprintf(stderr, "請編輯這些檔案解決衝突，然後：\n");
    fprintf(stderr, "  sg add <file>...     標記為已解決\n");
    fprintf(stderr, "  sg commit -m \"...\"   完成這次合併\n");
    fprintf(stderr, "或放棄這次合併：\n");
    fprintf(stderr, "  sg merge --abort\n");
}

static int do_three_way_merge(const char *git_dir, const char *repo_root, const char *current_branch,
                              const char *branch_arg, const unsigned char ours_commit[SG_SHA1_RAW_LEN],
                              const unsigned char theirs_commit[SG_SHA1_RAW_LEN],
                              const unsigned char base_commit[SG_SHA1_RAW_LEN],
                              const unsigned char theirs_tree[SG_SHA1_RAW_LEN])
{
    unsigned char ours_tree[SG_SHA1_RAW_LEN];
    unsigned char base_tree[SG_SHA1_RAW_LEN];
    sg_index idx;
    sg_merge_result result;
    sg_index new_idx;
    sg_flat_entry *flat_entries = NULL;
    size_t flat_count = 0;
    char **conflict_paths = NULL;
    size_t conflict_count = 0;
    size_t i;
    int has_conflict = 0;
    int index_ok = 1;
    int content_missing = 0;
    int rc = 1;

    if (sg_commit_tree_of(git_dir, ours_commit, ours_tree) != 0 ||
       sg_commit_tree_of(git_dir, base_commit, base_tree) != 0) {
        fprintf(stderr, "sg: corrupt commit while resolving merge\n");
        return 1;
    }

    if (sg_index_read(git_dir, &idx) != 0) {
        fprintf(stderr, "sg: failed to read index (corrupt?)\n");
        return 1;
    }

    /* The working tree is already known clean here: sg_cmd_merge gates every
       non-abort path through merge_require_clean before getting this far. */

    /* Merging always risks the working tree (conflicts may appear even from
       a clean start), so -- unlike switch/restore -- the snapshot here is
       unconditional, not gated on `dirty`. A failed snapshot must abort the
       whole operation rather than proceed unprotected. */
    {
        char label[300];

        snprintf(label, sizeof(label), "merge %s", branch_arg);
        if (sg_snapshot_create(git_dir, repo_root, &idx, label, NULL) != 0) {
            fprintf(stderr, "sg: 自動快照失敗，為了安全起見中止這次合併（沒有做任何變更）\n");
            sg_index_free(&idx);
            return 1;
        }
    }
    sg_index_free(&idx);

    if (sg_merge_trees(git_dir, base_tree, ours_tree, theirs_tree, current_branch, branch_arg,
                       &result) != 0) {
        fprintf(stderr, "sg: 合併過程中發生錯誤\n");
        return 1;
    }

    memset(&new_idx, 0, sizeof(new_idx));
    flat_entries = result.count > 0 ? malloc(result.count * sizeof(*flat_entries)) : NULL;
    if (result.count > 0 && flat_entries == NULL) {
        fprintf(stderr, "sg: out of memory\n");
        sg_merge_result_free(&result);
        return 1;
    }

    for (i = 0; i < result.count; i++) {
        sg_merge_result_entry *e = &result.entries[i];
        char abspath[4096];

        snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, e->path);

        if (e->conflict) {
            int mode = e->ours_present ? (int)(e->ours_mode & 0777)
                                       : (e->theirs_present ? (int)(e->theirs_mode & 0777) : 0644);
            char **grown;

            has_conflict = 1;
            if (sg_write_file_mkdirs(abspath, e->conflict_content, e->conflict_content_len, mode) != 0)
                fprintf(stderr, "sg: failed to write conflicted '%s'\n", e->path);

            if (e->base_present && add_stage_entry(&new_idx, e->path, 1, e->base_mode,
                                                   e->base_sha1) != 0)
                index_ok = 0;
            if (e->ours_present && add_stage_entry(&new_idx, e->path, 2, e->ours_mode,
                                                   e->ours_sha1) != 0)
                index_ok = 0;
            if (e->theirs_present && add_stage_entry(&new_idx, e->path, 3, e->theirs_mode,
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
            remove(abspath);
        } else {
            unsigned char *content;
            size_t content_len;
            sg_chunk_missing_info missing;
            int read_rc = sg_chunk_read_blob(git_dir, e->sha1, &content, &content_len, &missing);

            if (read_rc == 0) {
                if (sg_write_file_mkdirs(abspath, content, content_len, (int)(e->mode & 0777)) != 0)
                    fprintf(stderr, "sg: failed to write '%s'\n", e->path);
                free(content);
            } else if (read_rc == -2) {
                sg_chunk_print_missing_error(e->path, &missing);
                content_missing = 1;
            } else {
                fprintf(stderr, "sg: missing blob for '%s'\n", e->path);
            }
            if (add_resolved_entry(repo_root, &new_idx, e->path, e->mode, e->sha1) != 0)
                index_ok = 0;

            flat_entries[flat_count].path = e->path; /* transient view, not owned */
            flat_entries[flat_count].mode = e->mode;
            memcpy(flat_entries[flat_count].sha1, e->sha1, SG_SHA1_RAW_LEN);
            flat_count++;
        }
    }

    /* A chunked file whose data actually can't be recovered must abort the
       merge outright -- sg_chunk_print_missing_error already explained why
       for every affected path above; there's nothing left to do here except
       refuse to record an index/commit that silently drops the content. */
    if (content_missing) {
        sg_index_free(&new_idx);
        rc = 1;
        goto done;
    }

    /* An index missing entries would be silently wrong on disk -- refuse to
       write a partial one rather than record a merge state that lost paths. */
    if (!index_ok) {
        fprintf(stderr, "sg: out of memory building the merged index; 沒有寫入 index\n");
        sg_index_free(&new_idx);
        rc = 1;
        goto done;
    }

    if (sg_index_write(git_dir, &new_idx) != 0) {
        fprintf(stderr, "sg: failed to write index\n");
        sg_index_free(&new_idx);
        sg_merge_result_free(&result);
        free(flat_entries);
        for (i = 0; i < conflict_count; i++)
            free(conflict_paths[i]);
        free(conflict_paths);
        return 1;
    }
    sg_index_free(&new_idx);

    if (sg_merge_head_write(git_dir, theirs_commit) != 0) {
        fprintf(stderr, "sg: failed to write MERGE_HEAD\n");
        sg_merge_result_free(&result);
        free(flat_entries);
        for (i = 0; i < conflict_count; i++)
            free(conflict_paths[i]);
        free(conflict_paths);
        return 1;
    }

    if (has_conflict) {
        print_conflict_message(conflict_paths, conflict_count);
        rc = 1;
    } else {
        sg_commit commit;
        unsigned char *serialized;
        size_t serialized_len;
        unsigned char new_commit_id[SG_SHA1_RAW_LEN];
        char message[512];
        char *cleaned_message;
        const char *name = env_or("GIT_AUTHOR_NAME", "small_git");
        const char *email = env_or("GIT_AUTHOR_EMAIL", "sg@localhost");

        snprintf(message, sizeof(message), "Merge branch '%s' into %s\n", branch_arg, current_branch);
        if (sg_message_cleanup(message, &cleaned_message) != 0) {
            fprintf(stderr, "sg: out of memory\n");
            rc = 1;
            goto done;
        }

        memset(&commit, 0, sizeof(commit));
        if (sg_tree_build(git_dir, flat_entries, flat_count, commit.tree) != 0) {
            fprintf(stderr, "sg: failed to build merge tree\n");
            free(cleaned_message);
            rc = 1;
            goto done;
        }
        commit.parents = malloc(2 * sizeof(*commit.parents));
        if (commit.parents == NULL) {
            fprintf(stderr, "sg: out of memory\n");
            free(cleaned_message);
            rc = 1;
            goto done;
        }
        memcpy(commit.parents[0], ours_commit, SG_SHA1_RAW_LEN);
        memcpy(commit.parents[1], theirs_commit, SG_SHA1_RAW_LEN);
        commit.parent_count = 2;
        commit.author_name = (char *)name;
        commit.author_email = (char *)email;
        commit.author_time = (long long)time(NULL);
        strcpy(commit.author_tz, "+0000");
        commit.committer_name = (char *)name;
        commit.committer_email = (char *)email;
        commit.committer_time = commit.author_time;
        strcpy(commit.committer_tz, "+0000");
        commit.message = cleaned_message;

        if (sg_commit_serialize(&commit, &serialized, &serialized_len) != 0) {
            fprintf(stderr, "sg: failed to serialize merge commit\n");
            free(commit.parents);
            free(cleaned_message);
            rc = 1;
            goto done;
        }
        free(commit.parents);
        free(cleaned_message);

        if (sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len, new_commit_id) != 0) {
            fprintf(stderr, "sg: failed to write merge commit\n");
            free(serialized);
            rc = 1;
            goto done;
        }
        free(serialized);

        if (sg_ref_update_branch(git_dir, current_branch, new_commit_id) != 0) {
            fprintf(stderr, "sg: failed to update branch '%s'\n", current_branch);
            rc = 1;
            goto done;
        }

        if (sg_merge_head_remove(git_dir) != 0)
            fprintf(stderr, "sg: warning: merge succeeded but failed to remove MERGE_HEAD\n");

        {
            char commit_hex[SG_SHA1_HEX_LEN + 1];
            char short_hex[8];

            sg_sha1_to_hex(new_commit_id, commit_hex);
            memcpy(short_hex, commit_hex, 7);
            short_hex[7] = '\0';
            printf("Merge made by '%s' [%s] into '%s'.\n", branch_arg, short_hex, current_branch);
        }
        rc = 0;
    }

done:
    sg_merge_result_free(&result);
    free(flat_entries);
    for (i = 0; i < conflict_count; i++)
        free(conflict_paths[i]);
    free(conflict_paths);
    return rc;
}

static int do_merge_abort(const char *git_dir, const char *repo_root)
{
    unsigned char merge_head_id[SG_SHA1_RAW_LEN];
    unsigned char head_id[SG_SHA1_RAW_LEN];
    unsigned char head_tree[SG_SHA1_RAW_LEN];
    sg_index idx;

    if (sg_merge_head_read(git_dir, merge_head_id) != 0) {
        fprintf(stderr, "sg: 目前不在合併狀態（找不到 MERGE_HEAD）\n");
        return 1;
    }
    if (sg_ref_resolve_head(git_dir, head_id) != 0 ||
       sg_commit_tree_of(git_dir, head_id, head_tree) != 0) {
        fprintf(stderr, "sg: 無法讀取目前分支的 commit\n");
        return 1;
    }
    if (sg_index_read(git_dir, &idx) != 0) {
        fprintf(stderr, "sg: failed to read index (corrupt?)\n");
        return 1;
    }

    /* This overwrites the working tree just like a dangerous switch/restore
       does -- take a safety snapshot first, and abort outright if that
       fails instead of proceeding unprotected. */
    if (sg_snapshot_create(git_dir, repo_root, &idx, "merge --abort", NULL) != 0) {
        fprintf(stderr, "sg: 自動快照失敗，為了安全起見中止 abort（沒有做任何變更）\n");
        sg_index_free(&idx);
        return 1;
    }
    sg_index_free(&idx);

    if (sg_apply_tree_to_workdir(git_dir, repo_root, head_tree) != 0) {
        fprintf(stderr, "sg: 還原工作目錄失敗\n");
        return 1;
    }
    if (sg_merge_head_remove(git_dir) != 0)
        fprintf(stderr, "sg: warning: 無法移除 MERGE_HEAD\n");

    printf("Merge aborted.\n");
    return 0;
}

int sg_cmd_merge(int argc, char **argv)
{
    static const char usage[] = "usage: sg merge [--force|-f] <branch>\n       sg merge --abort\n";
    int abort_flag = 0;
    int force = 0;
    const char *branch_arg = NULL;
    char *git_dir;
    char *repo_root;
    int i;
    int rc;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--abort") == 0) {
            abort_flag = 1;
        } else if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0) {
            force = 1;
        } else if (branch_arg == NULL) {
            branch_arg = argv[i];
        } else {
            fputs(usage, stderr);
            return 1;
        }
    }
    if (abort_flag == (branch_arg != NULL)) {
        fputs(usage, stderr);
        return 1;
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;
    repo_root = sg_repo_root(git_dir);
    if (repo_root == NULL) {
        fprintf(stderr, "sg: failed to determine repository root\n");
        free(git_dir);
        return 1;
    }

    if (abort_flag) {
        rc = do_merge_abort(git_dir, repo_root);
        free(git_dir);
        free(repo_root);
        return rc;
    }

    {
        unsigned char theirs_commit[SG_SHA1_RAW_LEN];
        unsigned char ours_commit[SG_SHA1_RAW_LEN];
        unsigned char theirs_tree[SG_SHA1_RAW_LEN];
        unsigned char base_commit[SG_SHA1_RAW_LEN];
        unsigned char in_progress[SG_SHA1_RAW_LEN];
        int has_head;
        char *current_branch;
        int mb_rc;

        /* Starting a second merge on top of an unfinished one would drop the
           first one's MERGE_HEAD and conflict staging on the floor. */
        if (sg_merge_head_read(git_dir, in_progress) == 0) {
            fprintf(stderr,
                   "sg: 目前有一個尚未完成的合併\n"
                   "請先完成它（解決衝突後 sg add <file>... 再 sg commit），"
                   "或執行 sg merge --abort 放棄\n");
            free(git_dir);
            free(repo_root);
            return 1;
        }

        /* Starting a merge on top of an in-progress rebase would let the
           rebase's advancing branch ref and this merge's own commit graph
           surgery trample each other -- same reasoning as the MERGE_HEAD
           check above, just for the other direction. */
        if (sg_rebase_state_exists(git_dir)) {
            fprintf(stderr,
                   "sg: 目前有一個進行中的 rebase\n"
                   "請先完成它（sg rebase --continue）或執行 sg rebase --abort 放棄\n");
            free(git_dir);
            free(repo_root);
            return 1;
        }

        if (sg_require_clean_workdir(git_dir, repo_root, "sg merge") != 0) {
            free(git_dir);
            free(repo_root);
            return 1;
        }

        if (!sg_ref_branch_exists(git_dir, branch_arg)) {
            fprintf(stderr, "sg: invalid reference: %s\n", branch_arg);
            free(git_dir);
            free(repo_root);
            return 1;
        }
        if (sg_ref_read_branch(git_dir, branch_arg, theirs_commit) != 0) {
            fprintf(stderr, "sg: failed to read branch '%s'\n", branch_arg);
            free(git_dir);
            free(repo_root);
            return 1;
        }

        has_head = (sg_ref_resolve_head(git_dir, ours_commit) == 0);

        current_branch = sg_ref_current_branch(git_dir);
        if (current_branch == NULL) {
            fprintf(stderr, "sg: failed to determine current branch\n");
            free(git_dir);
            free(repo_root);
            return 1;
        }

        if (sg_commit_tree_of(git_dir, theirs_commit, theirs_tree) != 0) {
            fprintf(stderr, "sg: corrupt commit for branch '%s'\n", branch_arg);
            free(current_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }

        if (!has_head) {
            /* current branch has no commits yet: fast-forwarding onto
               theirs is always safe, there is nothing of ours to lose */
            rc = do_fast_forward(git_dir, repo_root, current_branch, branch_arg, theirs_commit,
                                 theirs_tree, force);
            free(current_branch);
            free(git_dir);
            free(repo_root);
            return rc;
        }

        mb_rc = sg_merge_base(git_dir, ours_commit, theirs_commit, base_commit);
        if (mb_rc == -2) {
            fprintf(stderr, "sg: 找到多個彼此獨立的共同祖先（criss-cross 歷史），無法自動合併\n");
            free(current_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }
        if (mb_rc == -1) {
            fprintf(stderr, "sg: '%s' 與目前分支沒有共同的歷史，無法合併\n", branch_arg);
            free(current_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }

        if (memcmp(base_commit, theirs_commit, SG_SHA1_RAW_LEN) == 0) {
            printf("Already up to date.\n");
            free(current_branch);
            free(git_dir);
            free(repo_root);
            return 0;
        }

        if (memcmp(base_commit, ours_commit, SG_SHA1_RAW_LEN) == 0) {
            rc = do_fast_forward(git_dir, repo_root, current_branch, branch_arg, theirs_commit,
                                 theirs_tree, force);
            free(current_branch);
            free(git_dir);
            free(repo_root);
            return rc;
        }

        rc = do_three_way_merge(git_dir, repo_root, current_branch, branch_arg, ours_commit,
                                theirs_commit, base_commit, theirs_tree);
        free(current_branch);
        free(git_dir);
        free(repo_root);
        return rc;
    }
}
