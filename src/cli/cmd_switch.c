#include "sg/cli.h"

#include "sg/apply.h"
#include "sg/hash.h"
#include "sg/merge.h"
#include "sg/objstore.h"
#include "sg/object.h"
#include "sg/rebase.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int sg_cmd_switch(int argc, char **argv)
{
    int create = 0;
    int force = 0;
    const char *branch_arg = NULL;
    char *git_dir;
    char *repo_root;
    unsigned char target_commit_id[SG_SHA1_RAW_LEN];
    unsigned char target_tree_id[SG_SHA1_RAW_LEN];
    char label[256];
    int apply_rc;
    char *old_branch;
    char *checkout_msg = NULL;
    size_t i;

    for (i = 1; (int)i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            create = 1;
        } else if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0) {
            force = 1;
        } else if (branch_arg == NULL) {
            branch_arg = argv[i];
        } else {
            fprintf(stderr, "usage: sg switch [-c] [--force|-f] <branch>\n");
            return 1;
        }
    }
    if (branch_arg == NULL) {
        fprintf(stderr, "usage: sg switch [-c] [--force|-f] <branch>\n");
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

    /* Measured against real git 2.55.0: `switch` during a paused rebase is
       refused outright, even with --force -- rebase's sequencer state can
       only be ended by rebase's own subcommands (--abort, a completed run,
       --quit). This must run before any side effect, in particular before
       -c below would create a new branch: real git also leaves the new
       branch un-created when the switch is refused. */
    if (sg_rebase_state_exists(git_dir)) {
        fprintf(stderr,
               "sg: 目前有一個進行中的 rebase，無法切換分支\n"
               "請先完成它（sg rebase --continue）或執行 sg rebase --abort 放棄\n");
        free(git_dir);
        free(repo_root);
        return 1;
    }

    /* Same shape as the rebase gate above, one subsystem over. Measured
       against real git 2.55.0: `switch` during an in-progress merge is
       refused ("cannot switch branch while merging"), and --force does not
       override it -- unlike `checkout -f`, which does succeed and clears
       MERGE_HEAD. sg has no `checkout`, so `switch`'s rule is the one to
       match. The refusal is unconditional on MERGE_HEAD's mere existence:
       it fires whether or not the index still has conflicts, with -c, and
       even when the target is the branch already checked out.

       Without this, sg only refused by accident -- via sg_safe_apply_tree's
       dirty-worktree confirmation, which is exactly what --force bypasses,
       so `sg switch --force` would silently clear MERGE_HEAD and abandon
       the merge. Like the rebase gate, this must run before any side
       effect. The binding one is sg_safe_apply_tree below, which both
       overwrites the working tree and clears MERGE_HEAD; the -c block in
       between only validates, and does not write the new branch ref until
       after that call succeeds. */
    if (sg_merge_head_exists(git_dir)) {
        fprintf(stderr,
               "sg: 目前有一個進行中的合併，無法切換分支\n"
               "請先完成它（sg commit）或執行 sg merge --abort 放棄\n");
        free(git_dir);
        free(repo_root);
        return 1;
    }

    /* Captured before any write below so the reflog's "moving from <X>" can
       name the branch HEAD pointed at before this switch. */
    old_branch = sg_ref_current_branch(git_dir);

    /* Measured against real git 2.55.0: "checkout: moving from <old> to
       <new>" lands on logs/HEAD for both a plain switch and switch -c. Built
       here, before sg_safe_apply_tree below touches anything, because an
       allocation failure must not abort this command *after* the working
       tree has been overwritten -- that would leave the files on the new
       branch while HEAD still named the old one, and the next commit on the
       old branch would swallow the other branch's tree. The message depends
       only on old_branch and argv, so there is no reason to build it late.
       Same principle the -c block below already follows.

       old_branch is NULL only for a detached HEAD, and then no line is
       written at all. Real git names the commit there instead, as a full
       40-hex ("checkout: moving from 69cceca9a5...aa5ab to master" --
       measured), and matching that was tried. It does not work and is
       deliberately not pursued: sg_ref_resolve_head only understands a
       symbolic HEAD and returns -1 for a detached one, so the label would
       have to come from a private parse of .git/HEAD, and sg is already
       incoherent in that state well beyond the reflog (`sg status` reports
       "On branch ?" and "No commits yet"). Detached HEAD is Phase 18's
       subject as a whole; writing a correct reflog line for it while the
       rest of the command still misreads the state would be polishing one
       corner of something unsupported. Recorded as a known divergence
       rather than half-supported. */
    if (old_branch != NULL) {
        int need = snprintf(NULL, 0, "checkout: moving from %s to %s", old_branch, branch_arg);

        if (need >= 0) {
            checkout_msg = malloc((size_t)need + 1);
            if (checkout_msg == NULL) {
                fprintf(stderr, "sg: out of memory\n");
                free(old_branch);
                free(git_dir);
                free(repo_root);
                return 1;
            }
            snprintf(checkout_msg, (size_t)need + 1, "checkout: moving from %s to %s", old_branch,
                    branch_arg);
        }
    }

    /* Only reads/validates so far -- nothing is created yet. A new branch
       must not be written until sg_safe_apply_tree below actually succeeds,
       otherwise a cancelled switch would leave a dangling empty branch. */
    if (create) {
        if (sg_ref_branch_exists(git_dir, branch_arg)) {
            fprintf(stderr, "sg: a branch named '%s' already exists\n", branch_arg);
            free(checkout_msg);
            free(old_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }
        if (sg_ref_resolve_head(git_dir, target_commit_id) != 0) {
            fprintf(stderr, "sg: cannot create branch '%s': current branch has no commits yet\n",
                   branch_arg);
            free(checkout_msg);
            free(old_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }
    } else {
        if (!sg_ref_branch_exists(git_dir, branch_arg)) {
            fprintf(stderr, "sg: invalid reference: %s\n", branch_arg);
            free(checkout_msg);
            free(old_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }
        if (sg_ref_read_branch(git_dir, branch_arg, target_commit_id) != 0) {
            fprintf(stderr, "sg: failed to read branch '%s'\n", branch_arg);
            free(checkout_msg);
            free(old_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }
    }

    if (sg_commit_tree_of(git_dir, target_commit_id, target_tree_id) != 0) {
        fprintf(stderr, "sg: corrupt commit for branch '%s'\n", branch_arg);
        free(checkout_msg);
        free(old_branch);
        free(git_dir);
        free(repo_root);
        return 1;
    }

    snprintf(label, sizeof(label), "switch to '%s'", branch_arg);
    apply_rc = sg_safe_apply_tree(git_dir, repo_root, target_tree_id, label, force);
    if (apply_rc == 1) {
        fprintf(stderr, "sg: switch aborted\n");
        free(checkout_msg);
        free(old_branch);
        free(git_dir);
        free(repo_root);
        return 1;
    }
    if (apply_rc != 0) {
        free(checkout_msg);
        free(old_branch);
        free(git_dir);
        free(repo_root);
        return 1;
    }

    if (create) {
        /* Measured against real git 2.55.0: `git switch -c <name>` logs
           "Created from HEAD" for the new branch's own reflog -- literally
           the string "HEAD", not the branch name HEAD currently resolves
           to. This is the one asymmetry against `git branch <name>`, which
           logs "Created from <current branch name>" instead (see
           cmd_branch.c's create_branch). Written before HEAD moves, same
           order real git uses. */
        char ref_path[4096];

        if (snprintf(ref_path, sizeof(ref_path), "refs/heads/%s", branch_arg) >= (int)sizeof(ref_path) ||
           sg_ref_update(git_dir, ref_path, target_commit_id, "branch: Created from HEAD") != 0) {
            fprintf(stderr, "sg: failed to create branch '%s'\n", branch_arg);
            free(checkout_msg);
            free(old_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }
    }

    {
        /* checkout_msg was built up front, before any side effect -- see the
           comment where old_branch is captured. NULL here means a detached
           HEAD, which deliberately gets no reflog line. */
        if (sg_ref_set_head(git_dir, branch_arg, checkout_msg) != 0) {
            fprintf(stderr, "sg: failed to update HEAD\n");
            free(checkout_msg);
            free(old_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }
        free(checkout_msg);
    }

    printf("Switched to branch '%s'\n", branch_arg);
    free(old_branch);
    free(git_dir);
    free(repo_root);
    return 0;
}
