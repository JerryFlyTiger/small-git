#include "sg/cli.h"

#include "sg/apply.h"
#include "sg/hash.h"
#include "sg/merge.h"
#include "sg/objstore.h"
#include "sg/object.h"
#include "sg/rebase.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/revparse.h"
#include "sg/sequencer.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* "<abbrev> <subject>", real git's way of naming a commit when there is no
   branch name to use ("HEAD is now at 18e16a1 C2" -- measured, 2.55.0).
   Best-effort: an unreadable commit degrades to the abbreviated id alone
   rather than failing the switch, since this is only a status line. */
static void print_commit_line(const char *git_dir, const char *prefix,
                              const unsigned char id[SG_SHA1_RAW_LEN])
{
    char hex[SG_SHA1_HEX_LEN + 1];
    sg_obj_type type;
    unsigned char *content;
    size_t content_len;
    sg_commit commit;

    sg_sha1_to_hex(id, hex);
    hex[7] = '\0';

    if (sg_object_read(git_dir, id, &type, &content, &content_len) != 0 || type != SG_OBJ_COMMIT) {
        printf("%s %s\n", prefix, hex);
        return;
    }
    if (sg_commit_parse(content, content_len, &commit) != 0) {
        free(content);
        printf("%s %s\n", prefix, hex);
        return;
    }
    free(content);
    printf("%s %s %.*s\n", prefix, hex, (int)strcspn(commit.message, "\n"), commit.message);
    sg_commit_free(&commit);
}

int sg_cmd_switch(int argc, char **argv)
{
    int create = 0;
    int force = 0;
    int detach = 0;
    const char *branch_arg = NULL;
    char *git_dir;
    char *repo_root;
    unsigned char target_commit_id[SG_SHA1_RAW_LEN];
    unsigned char target_tree_id[SG_SHA1_RAW_LEN];
    /* Where HEAD was, captured before anything writes to it -- the "Previous
       HEAD position was" line is printed after HEAD has already moved. */
    unsigned char prev_commit_id[SG_SHA1_RAW_LEN];
    int have_prev_commit = 0;
    char label[256];
    int apply_rc;
    char *old_branch;
    char *checkout_msg = NULL;
    size_t i;

    for (i = 1; (int)i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            create = 1;
        } else if (strcmp(argv[i], "--detach") == 0) {
            detach = 1;
        } else if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0) {
            force = 1;
        } else if (branch_arg == NULL) {
            branch_arg = argv[i];
        } else {
            fprintf(stderr, "usage: sg switch [-c] [--detach] [--force|-f] <branch>\n");
            return 1;
        }
    }
    if (branch_arg == NULL) {
        fprintf(stderr, "usage: sg switch [-c] [--detach] [--force|-f] <branch>\n");
        return 1;
    }
    /* -c names a branch to create, --detach means "point HEAD at a commit and
       at no branch" -- asking for both is asking for two different HEADs. */
    if (create && detach) {
        fprintf(stderr, "sg: -c and --detach cannot be used together\n");
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
               "sg: a rebase is in progress, cannot switch branches\n"
               "Finish it first (sg rebase --continue) or run sg rebase --abort to give up\n");
        free(git_dir);
        free(repo_root);
        return 1;
    }

    /* Same shape as the rebase gate above, for the third subsystem that
       leaves its own recoverable state behind a conflict. */
    {
        sg_seq_kind seq_kind = sg_sequencer_kind_in_progress(git_dir);

        if (seq_kind != 0) {
            const char *op = seq_kind == SG_SEQ_CHERRY_PICK ? "cherry-pick" : "revert";

            fprintf(stderr,
                   "sg: a %s is in progress, cannot switch branches\n"
                   "Finish it first (sg %s --continue) or run sg %s --abort to give up\n",
                   op, op, op);
            free(git_dir);
            free(repo_root);
            return 1;
        }
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
               "sg: a merge is in progress, cannot switch branches\n"
               "Finish it first (sg commit) or run sg merge --abort to give up\n");
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

       When HEAD is already detached there is no branch name to use, and real
       git writes the full 40-hex of the commit being left instead
       ("checkout: moving from 69cceca9a5...aa5ab to master" -- measured,
       full id, not abbreviated). Before Phase 18 that could not be produced
       (sg_ref_resolve_head returned -1 for a detached HEAD) and the line was
       skipped entirely; it now resolves, so the divergence is gone.

       The "to" half is always argv verbatim -- the token the user typed, not
       what it resolves to (measured: a branch name stays a branch name, a
       sha stays that sha). sg_ref_detach_description depends on that, since
       it recovers the detach point's label from exactly this text.

       A HEAD that is neither symbolic nor a well-formed id leaves from_label
       unset, and then no line is written at all: inventing an all-zeros
       "from" would put a fabricated chain link into logs/HEAD. */
    if (old_branch == NULL && sg_ref_head_is_detached(git_dir) == 1 &&
       sg_ref_resolve_head(git_dir, prev_commit_id) == 0)
        have_prev_commit = 1;

    {
        char from_hex[SG_SHA1_HEX_LEN + 1];
        const char *from = NULL;

        if (old_branch != NULL) {
            from = old_branch;
        } else if (have_prev_commit) {
            sg_sha1_to_hex(prev_commit_id, from_hex);
            from = from_hex;
        }

        if (from != NULL) {
            int need = snprintf(NULL, 0, "checkout: moving from %s to %s", from, branch_arg);

            if (need >= 0) {
                checkout_msg = malloc((size_t)need + 1);
                if (checkout_msg == NULL) {
                    fprintf(stderr, "sg: out of memory\n");
                    free(old_branch);
                    free(git_dir);
                    free(repo_root);
                    return 1;
                }
                snprintf(checkout_msg, (size_t)need + 1, "checkout: moving from %s to %s", from,
                        branch_arg);
            }
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
    } else if (detach) {
        /* Any revision, not just a branch: --detach's whole point is to check
           out something that has no branch name. */
        if (sg_rev_parse_commit(git_dir, branch_arg, target_commit_id) != 0) {
            fprintf(stderr, "sg: invalid reference: %s\n", branch_arg);
            free(checkout_msg);
            free(old_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }
    } else {
        if (!sg_ref_branch_exists(git_dir, branch_arg)) {
            unsigned char probe[SG_SHA1_RAW_LEN];

            /* A resolvable commit that simply isn't a branch is a different
               mistake from a typo, and real git says so rather than calling
               it invalid ("fatal: a branch is expected, got commit '<id>'"
               plus a --detach hint -- measured). Refusing without --detach is
               git's behaviour too: `git switch <sha>` does not silently
               detach. */
            if (sg_rev_parse_commit(git_dir, branch_arg, probe) == 0)
                fprintf(stderr,
                       "sg: '%s' is a commit, not a branch\n"
                       "To point HEAD directly at it (detached HEAD), use sg switch --detach %s\n",
                       branch_arg, branch_arg);
            else
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

    snprintf(label, sizeof(label), detach ? "detach at '%s'" : "switch to '%s'", branch_arg);
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
        char ref_path[SG_PATH_MAX];

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
           comment where old_branch is captured. NULL here means HEAD was in a
           state with no honest "moving from" label, and the line is skipped.

           Detaching writes the raw id; both paths log to HEAD identically, so
           the reflog does not record which of the two shapes HEAD ended in --
           only the commit. That is what makes the detach point recoverable
           from the "to" text alone. */
        int rc = detach ? sg_ref_set_head_detached(git_dir, target_commit_id, checkout_msg)
                        : sg_ref_set_head(git_dir, branch_arg, checkout_msg);

        if (rc != 0) {
            fprintf(stderr, "sg: failed to update HEAD\n");
            free(checkout_msg);
            free(old_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }
        free(checkout_msg);
    }

    /* Real git names the commit, not the argument, once HEAD has no branch:
       "HEAD is now at <abbrev> <subject>" on detaching, and "Previous HEAD
       position was <abbrev> <subject>" before the ordinary line when leaving
       a detached HEAD (measured, 2.55.0). */
    /* The "Previous HEAD position" line belongs to LEAVING a detached HEAD,
       so git prints it for detach-to-detach moves too, not only when
       arriving on a branch -- but only when the commit actually CHANGES.
       Measured, 2.55.0, all five combinations: detached->branch at the same
       commit, detached-at-a-branch-tip->that branch, and detach->detach at
       the same commit all print nothing extra; only a move that lands
       somewhere else does. */
    if (have_prev_commit && memcmp(prev_commit_id, target_commit_id, SG_SHA1_RAW_LEN) != 0)
        print_commit_line(git_dir, "Previous HEAD position was", prev_commit_id);
    if (detach)
        print_commit_line(git_dir, "HEAD is now at", target_commit_id);
    else
        printf("Switched to branch '%s'\n", branch_arg);
    free(old_branch);
    free(git_dir);
    free(repo_root);
    return 0;
}
