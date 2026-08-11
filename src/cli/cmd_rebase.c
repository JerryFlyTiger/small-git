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

static void short_hex(const unsigned char id[SG_SHA1_RAW_LEN], char out[8])
{
    char hex[SG_SHA1_HEX_LEN + 1];

    sg_sha1_to_hex(id, hex);
    memcpy(out, hex, 7);
    out[7] = '\0';
}

static char *first_line_dup(const char *message)
{
    const char *end = strchr(message, '\n');
    size_t len = end != NULL ? (size_t)(end - message) : strlen(message);
    char *out = malloc(len + 1);

    if (out == NULL)
        return NULL;
    memcpy(out, message, len);
    out[len] = '\0';
    return out;
}

static void print_conflict_message(const unsigned char commit_id[SG_SHA1_RAW_LEN],
                                   const char *commit_message, char **conflict_paths,
                                   size_t conflict_count)
{
    char short_sha[8];
    char *summary = first_line_dup(commit_message);
    size_t i;

    short_hex(commit_id, short_sha);
    fprintf(stderr, "無法自動套用 %s %s\n", short_sha, summary != NULL ? summary : "");
    free(summary);
    fprintf(stderr, "以下檔案有衝突:\n");
    for (i = 0; i < conflict_count; i++)
        fprintf(stderr, "    %s\n", conflict_paths[i]);
    fprintf(stderr,
           "請解決衝突後:\n"
           "  sg add <file>...      標記為已解決\n"
           "  sg rebase --continue  繼續 rebase\n"
           "或:\n"
           "  sg rebase --skip      跳過這個 commit\n"
           "  sg rebase --abort     放棄並回到原本狀態\n");
}

/* ==================== single-commit cherry-pick ==================== */

typedef enum {
    PICK_OK,      /* committed a new commit, branch ref advanced */
    PICK_EMPTY,   /* change already present upstream, silently advanced (nothing written) */
    PICK_CONFLICT, /* paused: workdir/index left with conflict markers/stages */
    PICK_ERROR    /* I/O error, nothing changed for this step */
} pick_rc;

/* Cherry-picks commit_id onto whatever refs/heads/<current_branch> (== HEAD)
   currently points to. This is exactly `sg merge`'s three-way merge, reused
   with base = commit_id's own parent tree, ours = the branch tip we're
   rebuilding, theirs = commit_id's tree -- the definition of "replay the
   change commit_id introduced on top of what's already here". */
static pick_rc rebase_pick_one(const char *git_dir, const char *repo_root,
                               const char *current_branch,
                               const unsigned char commit_id[SG_SHA1_RAW_LEN])
{
    unsigned char new_head[SG_SHA1_RAW_LEN];
    unsigned char parent_id[SG_SHA1_RAW_LEN];
    unsigned char base_tree[SG_SHA1_RAW_LEN];
    unsigned char ours_tree[SG_SHA1_RAW_LEN];
    sg_obj_type type;
    unsigned char *content = NULL;
    size_t content_len;
    sg_commit commit;
    sg_merge_result result;
    sg_index new_idx;
    char **conflict_paths = NULL;
    size_t conflict_count = 0;
    int has_conflict;
    char short_sha[8];
    char theirs_label[300];
    pick_rc rc = PICK_ERROR;
    size_t i;

    if (sg_ref_resolve_head(git_dir, new_head) != 0)
        return PICK_ERROR;
    if (sg_object_read(git_dir, commit_id, &type, &content, &content_len) != 0 ||
       type != SG_OBJ_COMMIT)
        return PICK_ERROR;
    if (sg_commit_parse(content, content_len, &commit) != 0) {
        free(content);
        return PICK_ERROR;
    }
    free(content);

    if (commit.parent_count != 1) {
        /* The todo list is built to exclude merge commits and never
           includes the (0-parent) root commit -- see cmd_rebase's list
           computation. Reaching here would mean that invariant broke. */
        sg_commit_free(&commit);
        return PICK_ERROR;
    }
    memcpy(parent_id, commit.parents[0], SG_SHA1_RAW_LEN);

    if (sg_commit_tree_of(git_dir, parent_id, base_tree) != 0 ||
       sg_commit_tree_of(git_dir, new_head, ours_tree) != 0) {
        sg_commit_free(&commit);
        return PICK_ERROR;
    }

    short_hex(commit_id, short_sha);
    {
        char *summary = first_line_dup(commit.message);

        snprintf(theirs_label, sizeof(theirs_label), "%s %s", short_sha,
                summary != NULL ? summary : "");
        free(summary);
    }

    if (sg_merge_trees(git_dir, base_tree, ours_tree, commit.tree, "HEAD", theirs_label, &result) !=
       0) {
        sg_commit_free(&commit);
        return PICK_ERROR;
    }

    /* Materializes the merge result into the working tree and a fresh
       index; a -1 here means either a chunked blob's data was unrecoverable
       or the index couldn't be built completely -- refuse to record a
       commit/index that silently drops content or paths. */
    if (sg_merge_result_apply(git_dir, repo_root, &result, &new_idx, &conflict_paths,
                              &conflict_count) != 0) {
        rc = PICK_ERROR;
        goto done;
    }
    has_conflict = conflict_count > 0;

    if (has_conflict) {
        if (sg_index_write(git_dir, &new_idx) != 0) {
            fprintf(stderr, "sg: failed to write index\n");
            rc = PICK_ERROR;
            goto done;
        }
        print_conflict_message(commit_id, commit.message, conflict_paths, conflict_count);
        rc = PICK_CONFLICT;
        goto done;
    }

    /* Clean merge: build the resulting tree. If it's byte-identical to what
       HEAD already is, commit_id's change is already present upstream (the
       classic "already cherry-picked" case) -- skip it rather than record an
       empty commit. Nothing was written to the workdir/index above besides
       content that already matched, so there is nothing to undo here. */
    {
        unsigned char merged_tree[SG_SHA1_RAW_LEN];

        if (sg_tree_build_from_index(git_dir, &new_idx, merged_tree) != 0) {
            fprintf(stderr, "sg: failed to build tree while cherry-picking %s\n", short_sha);
            rc = PICK_ERROR;
            goto done;
        }

        if (memcmp(merged_tree, ours_tree, SG_SHA1_RAW_LEN) == 0) {
            char *summary = first_line_dup(commit.message);

            printf("已跳過 %s %s（變更已存在於 upstream）\n", short_sha, summary != NULL ? summary : "");
            free(summary);
            rc = PICK_EMPTY;
            goto done;
        }

        if (sg_index_write(git_dir, &new_idx) != 0) {
            fprintf(stderr, "sg: failed to write index\n");
            rc = PICK_ERROR;
            goto done;
        }

        {
            sg_commit new_commit;
            unsigned char *serialized;
            size_t serialized_len;
            unsigned char new_commit_id[SG_SHA1_RAW_LEN];
            const char *committer_name = env_or("GIT_AUTHOR_NAME", "small_git");
            const char *committer_email = env_or("GIT_AUTHOR_EMAIL", "sg@localhost");

            memset(&new_commit, 0, sizeof(new_commit));
            memcpy(new_commit.tree, merged_tree, SG_SHA1_RAW_LEN);
            new_commit.parents = malloc(sizeof(*new_commit.parents));
            if (new_commit.parents == NULL) {
                fprintf(stderr, "sg: out of memory\n");
                rc = PICK_ERROR;
                goto done;
            }
            memcpy(new_commit.parents[0], new_head, SG_SHA1_RAW_LEN);
            new_commit.parent_count = 1;

            /* rebase's defining property: the author (who/when) of the
               replayed commit is carried over verbatim; only the committer
               (who/when it was rebuilt) reflects the here-and-now. */
            new_commit.author_name = commit.author_name;
            new_commit.author_email = commit.author_email;
            new_commit.author_time = commit.author_time;
            memcpy(new_commit.author_tz, commit.author_tz, sizeof(new_commit.author_tz));
            new_commit.committer_name = (char *)committer_name;
            new_commit.committer_email = (char *)committer_email;
            new_commit.committer_time = (long long)time(NULL);
            strcpy(new_commit.committer_tz, "+0000");
            new_commit.message = commit.message;

            if (sg_commit_serialize(&new_commit, &serialized, &serialized_len) != 0) {
                fprintf(stderr, "sg: failed to serialize commit\n");
                free(new_commit.parents);
                rc = PICK_ERROR;
                goto done;
            }
            free(new_commit.parents);

            if (sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len, new_commit_id) !=
               0) {
                fprintf(stderr, "sg: failed to write commit object\n");
                free(serialized);
                rc = PICK_ERROR;
                goto done;
            }
            free(serialized);

            if (sg_ref_update_branch(git_dir, current_branch, new_commit_id) != 0) {
                fprintf(stderr, "sg: failed to update branch '%s'\n", current_branch);
                rc = PICK_ERROR;
                goto done;
            }
            rc = PICK_OK;
        }
    }

done:
    sg_merge_result_free(&result);
    for (i = 0; i < conflict_count; i++)
        free(conflict_paths[i]);
    free(conflict_paths);
    sg_commit_free(&commit);
    sg_index_free(&new_idx);
    return rc;
}

/* ==================== todo loop shared by start/continue/skip ==================== */

/* Processes state->todo (oldest first) one commit at a time, persisting
   state to disk after every step so progress survives a crash between
   invocations. Returns 0 if the whole rebase finished (state removed and
   success already printed), 1 if paused on a new conflict (state left with
   current/todo updated, conflict message already printed), -1 on error. */
static int run_rebase_todo(const char *git_dir, const char *repo_root, const char *current_branch,
                           const char *upstream_arg, sg_rebase_state *state)
{
    while (state->todo_count > 0) {
        unsigned char commit_id[SG_SHA1_RAW_LEN];
        pick_rc rc;

        memcpy(commit_id, state->todo[0], SG_SHA1_RAW_LEN);

        rc = rebase_pick_one(git_dir, repo_root, current_branch, commit_id);

        if (rc == PICK_ERROR) {
            fprintf(stderr, "sg: rebase 過程中發生錯誤\n");
            return -1;
        }

        if (rc == PICK_CONFLICT) {
            memcpy(state->current, commit_id, SG_SHA1_RAW_LEN);
            state->has_current = 1;
            memmove(state->todo, state->todo + 1, (state->todo_count - 1) * sizeof(*state->todo));
            state->todo_count--;
            if (sg_rebase_state_write(git_dir, state) != 0)
                fprintf(stderr, "sg: warning: 無法寫入 rebase 狀態\n");
            return 1;
        }

        /* PICK_OK or PICK_EMPTY: advance past this commit either way. */
        memmove(state->todo, state->todo + 1, (state->todo_count - 1) * sizeof(*state->todo));
        state->todo_count--;
        state->has_current = 0;
        if (sg_rebase_state_write(git_dir, state) != 0)
            fprintf(stderr, "sg: warning: 無法寫入 rebase 狀態\n");
    }

    /* The commits are all replayed at this point, but a leftover
       .git/sg-rebase/ still reads as "a rebase is in progress" to every
       later command -- and its state files are already gone, so --abort
       can't clean it up either. Don't call that success. */
    if (sg_rebase_state_remove(git_dir) != 0) {
        fprintf(stderr,
               "sg: commit 都已重放完成，但未能清除 .git/sg-rebase/\n"
               "sg: 在手動刪除該目錄之前，rebase 會被視為仍在進行中\n");
        return 1;
    }
    printf("Successfully rebased and updated '%s' onto '%s'.\n", current_branch, upstream_arg);
    return 0;
}

/* ==================== start ==================== */

static int do_rebase_start(const char *git_dir, const char *repo_root, const char *upstream_arg)
{
    char *current_branch;
    unsigned char head_commit[SG_SHA1_RAW_LEN];
    unsigned char upstream_commit[SG_SHA1_RAW_LEN];
    unsigned char base_commit[SG_SHA1_RAW_LEN];
    int mb_rc;
    sg_rebase_state state;
    unsigned char merge_commit[SG_SHA1_RAW_LEN];
    int found_merge = 0;
    int rc;

    if (sg_rebase_state_exists(git_dir)) {
        fprintf(stderr,
               "sg: 已有一個進行中的 rebase\n"
               "請先執行 sg rebase --continue 完成它，或 sg rebase --abort 放棄\n");
        return 1;
    }
    /* Existence, not parseability -- see sg_merge_head_exists. This
       particular refusal has no real-git oracle: measured against git
       2.55.0, `git rebase` with a clean working tree ignores MERGE_HEAD
       entirely (valid or malformed) and simply clears it. sg refuses on
       purpose instead, and that refusal must cover a corrupt MERGE_HEAD
       too, or the two states disagree about whether a merge is in flight. */
    if (sg_merge_head_exists(git_dir)) {
        fprintf(stderr,
               "sg: 目前有一個尚未完成的合併\n"
               "請先完成它，或執行 sg merge --abort 放棄\n");
        return 1;
    }

    current_branch = sg_ref_current_branch(git_dir);
    if (current_branch == NULL) {
        fprintf(stderr, "sg: 目前是 detached HEAD，無法 rebase（HEAD 必須指向一個分支）\n");
        return 1;
    }

    if (sg_ref_resolve_head(git_dir, head_commit) != 0) {
        fprintf(stderr, "sg: 目前分支沒有任何 commit，無法 rebase\n");
        free(current_branch);
        return 1;
    }

    if (!sg_ref_branch_exists(git_dir, upstream_arg)) {
        fprintf(stderr, "sg: invalid reference: %s\n", upstream_arg);
        free(current_branch);
        return 1;
    }
    if (sg_ref_read_branch(git_dir, upstream_arg, upstream_commit) != 0) {
        fprintf(stderr, "sg: failed to read branch '%s'\n", upstream_arg);
        free(current_branch);
        return 1;
    }

    if (sg_require_clean_workdir(git_dir, repo_root, "sg rebase") != 0) {
        free(current_branch);
        return 1;
    }

    mb_rc = sg_merge_base(git_dir, head_commit, upstream_commit, base_commit);
    if (mb_rc == -2) {
        fprintf(stderr, "sg: 找到多個彼此獨立的共同祖先（criss-cross 歷史），無法 rebase\n");
        free(current_branch);
        return 1;
    }
    if (mb_rc == -1) {
        fprintf(stderr, "sg: '%s' 與目前分支沒有共同的歷史，無法 rebase\n", upstream_arg);
        free(current_branch);
        return 1;
    }

    if (memcmp(base_commit, upstream_commit, SG_SHA1_RAW_LEN) == 0) {
        /* upstream is already an ancestor of HEAD: every commit here is
           already built directly on top of it, nothing to replay. */
        printf("Current branch %s is up to date.\n", current_branch);
        free(current_branch);
        return 0;
    }

    if (memcmp(base_commit, head_commit, SG_SHA1_RAW_LEN) == 0) {
        /* HEAD is an ancestor of upstream: a pure fast-forward. */
        unsigned char upstream_tree[SG_SHA1_RAW_LEN];
        sg_index idx;
        char label[300];

        if (sg_commit_tree_of(git_dir, upstream_commit, upstream_tree) != 0) {
            fprintf(stderr, "sg: corrupt commit for branch '%s'\n", upstream_arg);
            free(current_branch);
            return 1;
        }

        if (sg_index_read(git_dir, &idx) != 0) {
            fprintf(stderr, "sg: failed to read index (corrupt?)\n");
            free(current_branch);
            return 1;
        }
        snprintf(label, sizeof(label), "rebase onto %s", upstream_arg);
        if (sg_snapshot_create(git_dir, repo_root, &idx, label, NULL) != 0) {
            fprintf(stderr, "sg: 自動快照失敗，為了安全起見中止這次 rebase（沒有做任何變更）\n");
            sg_index_free(&idx);
            free(current_branch);
            return 1;
        }
        sg_index_free(&idx);

        if (sg_apply_tree_to_workdir(git_dir, repo_root, upstream_tree) != 0) {
            fprintf(stderr, "sg: 還原工作目錄失敗\n");
            free(current_branch);
            return 1;
        }
        if (sg_ref_update_branch(git_dir, current_branch, upstream_commit) != 0) {
            fprintf(stderr, "sg: failed to update branch '%s'\n", current_branch);
            free(current_branch);
            return 1;
        }

        printf("Fast-forwarded %s to %s.\n", current_branch, upstream_arg);
        free(current_branch);
        return 0;
    }

    rc = sg_rebase_compute_todo(git_dir, head_commit, base_commit, &state.todo, &state.todo_count,
                                merge_commit, &found_merge);
    if (rc != 0) {
        fprintf(stderr, "sg: 計算要重放的 commit 清單時發生錯誤\n");
        free(current_branch);
        return 1;
    }
    if (found_merge) {
        char short_sha[8];

        short_hex(merge_commit, short_sha);
        fprintf(stderr,
               "sg: 尚不支援含 merge commit 的 rebase（%s 是一個 merge commit）\n"
               "請改用 sg merge\n",
               short_sha);
        free(state.todo);
        free(current_branch);
        return 1;
    }
    if (state.todo_count == 0) {
        /* Not expected given the base==upstream / base==head checks above
           already cover every case where nothing needs replaying, but stay
           safe rather than fall through into an empty rebase. */
        printf("Current branch %s is up to date.\n", current_branch);
        free(state.todo);
        free(current_branch);
        return 0;
    }

    {
        sg_index idx;
        char label[300];

        if (sg_index_read(git_dir, &idx) != 0) {
            fprintf(stderr, "sg: failed to read index (corrupt?)\n");
            free(state.todo);
            free(current_branch);
            return 1;
        }
        snprintf(label, sizeof(label), "rebase onto %s", upstream_arg);
        if (sg_snapshot_create(git_dir, repo_root, &idx, label, NULL) != 0) {
            fprintf(stderr, "sg: 自動快照失敗，為了安全起見中止這次 rebase（沒有做任何變更）\n");
            sg_index_free(&idx);
            free(state.todo);
            free(current_branch);
            return 1;
        }
        sg_index_free(&idx);
    }

    memcpy(state.onto, upstream_commit, SG_SHA1_RAW_LEN);
    memcpy(state.orig_head, head_commit, SG_SHA1_RAW_LEN);
    state.orig_branch = current_branch; /* ownership moves to state, freed via sg_rebase_state_free */
    state.has_current = 0;

    if (sg_rebase_state_write(git_dir, &state) != 0) {
        fprintf(stderr, "sg: 無法寫入 rebase 狀態\n");
        sg_rebase_state_free(&state);
        return 1;
    }

    {
        unsigned char upstream_tree[SG_SHA1_RAW_LEN];

        if (sg_commit_tree_of(git_dir, upstream_commit, upstream_tree) != 0 ||
           sg_apply_tree_to_workdir(git_dir, repo_root, upstream_tree) != 0) {
            fprintf(stderr, "sg: 還原工作目錄失敗\n");
            sg_rebase_state_free(&state);
            return 1;
        }
    }
    if (sg_ref_update_branch(git_dir, state.orig_branch, upstream_commit) != 0) {
        fprintf(stderr, "sg: failed to update branch '%s'\n", state.orig_branch);
        sg_rebase_state_free(&state);
        return 1;
    }

    rc = run_rebase_todo(git_dir, repo_root, state.orig_branch, upstream_arg, &state);
    sg_rebase_state_free(&state);
    return rc < 0 ? 1 : rc;
}

/* ==================== --continue / --skip / --abort ==================== */

static void print_unmerged_paths(const sg_index *idx)
{
    size_t i;

    fprintf(stderr, "sg: 尚有未解決的衝突，無法繼續 rebase：\n");
    for (i = 0; i < idx->count; i++) {
        if (idx->entries[i].stage == 0)
            continue;
        if (i > 0 && strcmp(idx->entries[i].path, idx->entries[i - 1].path) == 0)
            continue;
        fprintf(stderr, "\t%s\n", idx->entries[i].path);
    }
    fprintf(stderr, "請先解決衝突並執行 `sg add <file>...` 標記為已解決，再重新 sg rebase --continue。\n");
}

static int do_rebase_continue(const char *git_dir, const char *repo_root)
{
    sg_rebase_state state;
    sg_index idx;
    unsigned char new_head[SG_SHA1_RAW_LEN];
    sg_obj_type type;
    unsigned char *content = NULL;
    size_t content_len;
    sg_commit orig_commit;
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    int rc;

    if (!sg_rebase_state_exists(git_dir)) {
        fprintf(stderr, "sg: 沒有進行中的 rebase\n");
        return 1;
    }
    if (sg_rebase_state_read(git_dir, &state) != 0) {
        fprintf(stderr, "sg: rebase 狀態損毀，請執行 sg rebase --abort 清理\n");
        return 1;
    }
    if (!state.has_current) {
        fprintf(stderr, "sg: rebase 狀態損毀（沒有正在處理中的 commit），請執行 sg rebase --abort 清理\n");
        sg_rebase_state_free(&state);
        return 1;
    }

    if (sg_index_read(git_dir, &idx) != 0) {
        fprintf(stderr, "sg: failed to read index (corrupt?)\n");
        sg_rebase_state_free(&state);
        return 1;
    }
    if (sg_index_has_unmerged(&idx)) {
        print_unmerged_paths(&idx);
        sg_index_free(&idx);
        sg_rebase_state_free(&state);
        return 1;
    }

    if (sg_object_read(git_dir, state.current, &type, &content, &content_len) != 0 ||
       type != SG_OBJ_COMMIT || sg_commit_parse(content, content_len, &orig_commit) != 0) {
        free(content);
        fprintf(stderr, "sg: 無法讀取原始 commit\n");
        sg_index_free(&idx);
        sg_rebase_state_free(&state);
        return 1;
    }
    free(content);

    if (sg_ref_resolve_head(git_dir, new_head) != 0) {
        fprintf(stderr, "sg: 無法讀取目前分支\n");
        sg_commit_free(&orig_commit);
        sg_index_free(&idx);
        sg_rebase_state_free(&state);
        return 1;
    }

    if (sg_tree_build_from_index(git_dir, &idx, tree_id) != 0) {
        fprintf(stderr, "sg: failed to build tree from index\n");
        sg_commit_free(&orig_commit);
        sg_index_free(&idx);
        sg_rebase_state_free(&state);
        return 1;
    }
    sg_index_free(&idx);

    {
        unsigned char ours_tree[SG_SHA1_RAW_LEN];

        if (sg_commit_tree_of(git_dir, new_head, ours_tree) == 0 &&
           memcmp(tree_id, ours_tree, SG_SHA1_RAW_LEN) == 0) {
            /* Conflict resolved down to "no actual change" -- same empty-
               commit rule as the automatic path: don't record a no-op. */
            char short_sha[8];
            char *summary = first_line_dup(orig_commit.message);

            short_hex(state.current, short_sha);
            printf("已跳過 %s %s（變更已存在於 upstream）\n", short_sha,
                  summary != NULL ? summary : "");
            free(summary);
            sg_commit_free(&orig_commit);
            state.has_current = 0;
            {
                char onto_label[8];

                short_hex(state.onto, onto_label);
                rc = run_rebase_todo(git_dir, repo_root, state.orig_branch, onto_label, &state);
            }
            sg_rebase_state_free(&state);
            return rc < 0 ? 1 : rc;
        }
    }

    {
        sg_commit new_commit;
        unsigned char *serialized;
        size_t serialized_len;
        unsigned char new_commit_id[SG_SHA1_RAW_LEN];
        const char *committer_name = env_or("GIT_AUTHOR_NAME", "small_git");
        const char *committer_email = env_or("GIT_AUTHOR_EMAIL", "sg@localhost");

        memset(&new_commit, 0, sizeof(new_commit));
        memcpy(new_commit.tree, tree_id, SG_SHA1_RAW_LEN);
        new_commit.parents = malloc(sizeof(*new_commit.parents));
        if (new_commit.parents == NULL) {
            fprintf(stderr, "sg: out of memory\n");
            sg_commit_free(&orig_commit);
            sg_rebase_state_free(&state);
            return 1;
        }
        memcpy(new_commit.parents[0], new_head, SG_SHA1_RAW_LEN);
        new_commit.parent_count = 1;
        new_commit.author_name = orig_commit.author_name;
        new_commit.author_email = orig_commit.author_email;
        new_commit.author_time = orig_commit.author_time;
        memcpy(new_commit.author_tz, orig_commit.author_tz, sizeof(new_commit.author_tz));
        new_commit.committer_name = (char *)committer_name;
        new_commit.committer_email = (char *)committer_email;
        new_commit.committer_time = (long long)time(NULL);
        strcpy(new_commit.committer_tz, "+0000");
        new_commit.message = orig_commit.message;

        if (sg_commit_serialize(&new_commit, &serialized, &serialized_len) != 0) {
            fprintf(stderr, "sg: failed to serialize commit\n");
            free(new_commit.parents);
            sg_commit_free(&orig_commit);
            sg_rebase_state_free(&state);
            return 1;
        }
        free(new_commit.parents);

        if (sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len, new_commit_id) != 0) {
            fprintf(stderr, "sg: failed to write commit object\n");
            free(serialized);
            sg_commit_free(&orig_commit);
            sg_rebase_state_free(&state);
            return 1;
        }
        free(serialized);

        if (sg_ref_update_branch(git_dir, state.orig_branch, new_commit_id) != 0) {
            fprintf(stderr, "sg: failed to update branch '%s'\n", state.orig_branch);
            sg_commit_free(&orig_commit);
            sg_rebase_state_free(&state);
            return 1;
        }
    }

    sg_commit_free(&orig_commit);
    state.has_current = 0;
    {
        char onto_label[8];

        short_hex(state.onto, onto_label);
        rc = run_rebase_todo(git_dir, repo_root, state.orig_branch, onto_label, &state);
    }
    sg_rebase_state_free(&state);
    return rc < 0 ? 1 : rc;
}

static int do_rebase_skip(const char *git_dir, const char *repo_root)
{
    sg_rebase_state state;
    unsigned char head_commit[SG_SHA1_RAW_LEN];
    unsigned char head_tree[SG_SHA1_RAW_LEN];
    int rc;

    if (!sg_rebase_state_exists(git_dir)) {
        fprintf(stderr, "sg: 沒有進行中的 rebase\n");
        return 1;
    }
    if (sg_rebase_state_read(git_dir, &state) != 0) {
        fprintf(stderr, "sg: rebase 狀態損毀，請執行 sg rebase --abort 清理\n");
        return 1;
    }
    if (!state.has_current) {
        fprintf(stderr, "sg: 沒有正在處理中的 commit 可以跳過\n");
        sg_rebase_state_free(&state);
        return 1;
    }

    if (sg_ref_resolve_head(git_dir, head_commit) != 0 ||
       sg_commit_tree_of(git_dir, head_commit, head_tree) != 0) {
        fprintf(stderr, "sg: 無法讀取目前分支\n");
        sg_rebase_state_free(&state);
        return 1;
    }

    /* Rebuilding the working tree below deletes any path the target tree
       doesn't have and overwrites the rest -- so anything the user staged or
       edited while resolving this conflict is about to go. Same protection
       (and same abort-on-failure rule) as --abort and rebase start. */
    {
        sg_index idx;

        if (sg_index_read(git_dir, &idx) != 0) {
            fprintf(stderr, "sg: failed to read index (corrupt?)\n");
            sg_rebase_state_free(&state);
            return 1;
        }
        if (sg_snapshot_create(git_dir, repo_root, &idx, "rebase --skip", NULL) != 0) {
            fprintf(stderr, "sg: 自動快照失敗，為了安全起見中止 skip（沒有做任何變更）\n");
            sg_index_free(&idx);
            sg_rebase_state_free(&state);
            return 1;
        }
        sg_index_free(&idx);
    }

    if (sg_apply_tree_to_workdir(git_dir, repo_root, head_tree) != 0) {
        fprintf(stderr, "sg: 還原工作目錄失敗\n");
        sg_rebase_state_free(&state);
        return 1;
    }

    state.has_current = 0;
    {
        char onto_label[8];

        short_hex(state.onto, onto_label);
        rc = run_rebase_todo(git_dir, repo_root, state.orig_branch, onto_label, &state);
    }
    sg_rebase_state_free(&state);
    return rc < 0 ? 1 : rc;
}

static int do_rebase_abort(const char *git_dir, const char *repo_root)
{
    sg_rebase_state state;
    sg_index idx;
    unsigned char orig_head_tree[SG_SHA1_RAW_LEN];
    char short_sha[8];

    if (!sg_rebase_state_exists(git_dir)) {
        fprintf(stderr, "sg: 沒有進行中的 rebase\n");
        return 1;
    }
    if (sg_rebase_state_read(git_dir, &state) != 0) {
        fprintf(stderr, "sg: rebase 狀態損毀，無法安全 abort；請手動檢查 .git/sg-rebase/\n");
        return 1;
    }

    if (sg_index_read(git_dir, &idx) != 0) {
        fprintf(stderr, "sg: failed to read index (corrupt?)\n");
        sg_rebase_state_free(&state);
        return 1;
    }
    /* The user may have edited files while resolving a conflict -- protect
       that work the same way merge --abort and switch/undo do, and refuse
       to abort at all if the snapshot itself can't be taken. */
    if (sg_snapshot_create(git_dir, repo_root, &idx, "rebase --abort", NULL) != 0) {
        fprintf(stderr, "sg: 自動快照失敗，為了安全起見中止 abort（沒有做任何變更）\n");
        sg_index_free(&idx);
        sg_rebase_state_free(&state);
        return 1;
    }
    sg_index_free(&idx);

    if (sg_commit_tree_of(git_dir, state.orig_head, orig_head_tree) != 0) {
        fprintf(stderr, "sg: 無法讀取 rebase 前的 commit\n");
        sg_rebase_state_free(&state);
        return 1;
    }
    if (sg_apply_tree_to_workdir(git_dir, repo_root, orig_head_tree) != 0) {
        fprintf(stderr, "sg: 還原工作目錄失敗\n");
        sg_rebase_state_free(&state);
        return 1;
    }
    if (sg_ref_update_branch(git_dir, state.orig_branch, state.orig_head) != 0) {
        fprintf(stderr, "sg: failed to update branch '%s'\n", state.orig_branch);
        sg_rebase_state_free(&state);
        return 1;
    }
    if (sg_rebase_state_remove(git_dir) != 0) {
        fprintf(stderr,
               "sg: 分支與工作目錄都已還原，但未能清除 .git/sg-rebase/\n"
               "sg: 在手動刪除該目錄之前，rebase 會被視為仍在進行中\n");
        sg_rebase_state_free(&state);
        return 1;
    }

    short_hex(state.orig_head, short_sha);
    printf("Rebase aborted; '%s' is back at %s.\n", state.orig_branch, short_sha);
    sg_rebase_state_free(&state);
    return 0;
}

int sg_cmd_rebase(int argc, char **argv)
{
    static const char usage[] =
        "usage: sg rebase <upstream>\n"
        "       sg rebase --continue\n"
        "       sg rebase --abort\n"
        "       sg rebase --skip\n";
    int continue_flag = 0, abort_flag = 0, skip_flag = 0;
    const char *upstream_arg = NULL;
    char *git_dir;
    char *repo_root;
    int i;
    int mode_count;
    int rc;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--continue") == 0) {
            continue_flag = 1;
        } else if (strcmp(argv[i], "--abort") == 0) {
            abort_flag = 1;
        } else if (strcmp(argv[i], "--skip") == 0) {
            skip_flag = 1;
        } else if (upstream_arg == NULL) {
            upstream_arg = argv[i];
        } else {
            fputs(usage, stderr);
            return 1;
        }
    }

    mode_count = continue_flag + abort_flag + skip_flag + (upstream_arg != NULL ? 1 : 0);
    if (mode_count != 1) {
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

    if (continue_flag)
        rc = do_rebase_continue(git_dir, repo_root);
    else if (abort_flag)
        rc = do_rebase_abort(git_dir, repo_root);
    else if (skip_flag)
        rc = do_rebase_skip(git_dir, repo_root);
    else
        rc = do_rebase_start(git_dir, repo_root, upstream_arg);

    free(git_dir);
    free(repo_root);
    return rc;
}
