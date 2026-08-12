#include "sg/cli.h"

#include "sg/apply.h"
#include "sg/hash.h"
#include "sg/index.h"
#include "sg/merge.h"
#include "sg/objstore.h"
#include "sg/object.h"
#include "sg/rebase.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/revparse.h"
#include "sg/snapshot.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { RESET_SOFT, RESET_MIXED, RESET_HARD } sg_reset_mode;

int sg_cmd_reset(int argc, char **argv)
{
    static const char usage[] = "usage: sg reset [--soft|--mixed|--hard] [--force|-f] [<rev>]\n";
    int mode_set = 0;
    sg_reset_mode mode = RESET_MIXED;
    int force = 0;
    const char *rev_arg = NULL;
    int saw_dashdash = 0;
    int pathspec_given = 0;
    char *git_dir;
    char *repo_root;
    char *current_branch;
    unsigned char target_commit_id[SG_SHA1_RAW_LEN];
    unsigned char target_tree_id[SG_SHA1_RAW_LEN];
    int i;

    for (i = 1; i < argc; i++) {
        if (!saw_dashdash && strcmp(argv[i], "--") == 0) {
            saw_dashdash = 1;
        } else if (!saw_dashdash && strcmp(argv[i], "--soft") == 0) {
            if (mode_set && mode != RESET_SOFT) {
                fputs(usage, stderr);
                return 1;
            }
            mode = RESET_SOFT;
            mode_set = 1;
        } else if (!saw_dashdash && strcmp(argv[i], "--mixed") == 0) {
            if (mode_set && mode != RESET_MIXED) {
                fputs(usage, stderr);
                return 1;
            }
            mode = RESET_MIXED;
            mode_set = 1;
        } else if (!saw_dashdash && strcmp(argv[i], "--hard") == 0) {
            if (mode_set && mode != RESET_HARD) {
                fputs(usage, stderr);
                return 1;
            }
            mode = RESET_HARD;
            mode_set = 1;
        } else if (!saw_dashdash && (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0)) {
            force = 1;
        } else if (!saw_dashdash && argv[i][0] == '-') {
            /* Unrecognized flag: reject explicitly rather than falling
               through to the rev_arg slot, where a typo like "--Hard" would
               be reported as "invalid reference: --Hard" and mislead the
               user into thinking their rev was wrong. */
            fputs(usage, stderr);
            return 1;
        } else if (saw_dashdash) {
            /* This project's `sg reset` deliberately has no pathspec form
               (that overlaps `sg restore --staged`) -- anything after "--"
               is a path the user meant to reset, not a rev. */
            pathspec_given = 1;
        } else if (rev_arg == NULL) {
            rev_arg = argv[i];
        } else {
            fputs(usage, stderr);
            return 1;
        }
    }

    if (pathspec_given) {
        fprintf(stderr,
               "sg: reset 不支援指定路徑 (pathspec)；如果只是想取消暫存某個檔案，"
               "請改用 `sg restore --staged <path>`\n");
        return 1;
    }
    if (rev_arg == NULL)
        rev_arg = "HEAD";

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;
    repo_root = sg_repo_root(git_dir);
    if (repo_root == NULL) {
        fprintf(stderr, "sg: failed to determine repository root\n");
        free(git_dir);
        return 1;
    }

    current_branch = sg_ref_current_branch(git_dir);
    if (current_branch == NULL) {
        fprintf(stderr, "sg: 目前是 detached HEAD，無法 reset（HEAD 必須指向一個分支）\n");
        free(git_dir);
        free(repo_root);
        return 1;
    }

    if (sg_rev_parse_commit(git_dir, rev_arg, target_commit_id) != 0) {
        fprintf(stderr, "sg: invalid reference: %s\n", rev_arg);
        free(current_branch);
        free(git_dir);
        free(repo_root);
        return 1;
    }

    if (mode == RESET_SOFT) {

        /* Real git refuses `reset --soft` outright while a merge or rebase
           is in progress ("cannot do a soft reset in the middle of a
           merge"), rather than silently abandoning MERGE_HEAD / the rebase
           sequencer state the way --mixed/--hard do. Verified directly
           against git: both an unresolved merge conflict and a paused
           rebase produce this same rejection, exit 128. */
        if (sg_merge_head_exists(git_dir) || sg_rebase_state_exists(git_dir)) {
            fprintf(stderr, "sg: 合併或 rebase 進行中，無法執行 soft reset\n");
            free(current_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }

        /* Neither the index nor the working directory is touched: nothing
           uncommitted is ever at risk, so there is no confirmation gate and
           no automatic snapshot -- same rule real git follows. */
        {
            char ref_path[4096];
            char reflog_msg[512];

            snprintf(reflog_msg, sizeof(reflog_msg), "reset: moving to %s", rev_arg);
            if (snprintf(ref_path, sizeof(ref_path), "refs/heads/%s", current_branch) >=
                   (int)sizeof(ref_path) ||
               sg_ref_update(git_dir, ref_path, target_commit_id, reflog_msg) != 0) {
                fprintf(stderr, "sg: failed to update branch '%s'\n", current_branch);
                free(current_branch);
                free(git_dir);
                free(repo_root);
                return 1;
            }
        }
    } else if (mode == RESET_MIXED) {
        sg_index idx;
        char label[256];
        int merge_in_progress;

        if (sg_commit_tree_of(git_dir, target_commit_id, target_tree_id) != 0) {
            fprintf(stderr, "sg: corrupt commit for '%s'\n", rev_arg);
            free(current_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }

        if (sg_index_read(git_dir, &idx) != 0) {
            fprintf(stderr, "sg: failed to read index (corrupt?)\n");
            free(current_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }

        /* --mixed discards staged content (the index is rebuilt from
           target_tree_id below), so unlike --soft it can lose work -- but
           unlike --hard it never touches the working directory, so real git
           doesn't prompt for it either. Split the difference: snapshot
           automatically, without an interactive confirmation. */
        snprintf(label, sizeof(label), "reset --mixed to '%s'", rev_arg);
        if (sg_snapshot_create(git_dir, repo_root, &idx, label, NULL) != 0) {
            fprintf(stderr, "sg: 自動快照失敗，為了安全起見中止這次操作（沒有做任何變更）\n");
            sg_index_free(&idx);
            free(current_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }
        sg_index_free(&idx);

        merge_in_progress = sg_merge_head_exists(git_dir);

        if (sg_index_reset_to_tree(git_dir, target_tree_id) != 0) {
            free(current_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }

        {
            char ref_path[4096];
            char reflog_msg[512];

            snprintf(reflog_msg, sizeof(reflog_msg), "reset: moving to %s", rev_arg);
            if (snprintf(ref_path, sizeof(ref_path), "refs/heads/%s", current_branch) >=
                   (int)sizeof(ref_path) ||
               sg_ref_update(git_dir, ref_path, target_commit_id, reflog_msg) != 0) {
                fprintf(stderr, "sg: failed to update branch '%s'\n", current_branch);
                free(current_branch);
                free(git_dir);
                free(repo_root);
                return 1;
            }
        }

        /* Leaving MERGE_HEAD behind would make a later, unrelated `sg
           commit` silently record a bogus merge commit for a merge the user
           just abandoned via reset. Verified against real git: `git reset`
           (mixed) during a conflicted merge clears MERGE_HEAD. Unlike
           --hard's sg_safe_apply_tree, this deliberately does NOT touch an
           in-progress rebase's sequencer state -- verified against real
           git, `git reset` (mixed) during a paused rebase leaves
           rebase-merge/ untouched, only a soft-reset-style rejection or
           `rebase --abort` ends it. */
        if (merge_in_progress && sg_merge_head_remove(git_dir) != 0)
            fprintf(stderr, "sg: warning: 未能清除 MERGE_HEAD\n");
    } else {
        char label[256];
        int apply_rc;

        if (sg_commit_tree_of(git_dir, target_commit_id, target_tree_id) != 0) {
            fprintf(stderr, "sg: corrupt commit for '%s'\n", rev_arg);
            free(current_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }

        snprintf(label, sizeof(label), "reset --hard to '%s'", rev_arg);
        apply_rc = sg_safe_apply_tree(git_dir, repo_root, target_tree_id, label, force);
        if (apply_rc == 1) {
            fprintf(stderr, "sg: reset aborted\n");
            free(current_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }
        if (apply_rc != 0) {
            free(current_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }

        {
            char ref_path[4096];
            char reflog_msg[512];

            snprintf(reflog_msg, sizeof(reflog_msg), "reset: moving to %s", rev_arg);
            if (snprintf(ref_path, sizeof(ref_path), "refs/heads/%s", current_branch) >=
                   (int)sizeof(ref_path) ||
               sg_ref_update(git_dir, ref_path, target_commit_id, reflog_msg) != 0) {
                fprintf(stderr, "sg: failed to update branch '%s'\n", current_branch);
                free(current_branch);
                free(git_dir);
                free(repo_root);
                return 1;
            }
        }
    }

    free(current_branch);
    free(git_dir);
    free(repo_root);
    return 0;
}
