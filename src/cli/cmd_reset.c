#include "sg/cli.h"

#include "sg/apply.h"
#include "sg/hash.h"
#include "sg/index.h"
#include "sg/merge.h"
#include "sg/objstore.h"
#include "sg/object.h"
#include "sg/quote.h"
#include "sg/rebase.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/revparse.h"
#include "sg/sequencer.h"
#include "sg/snapshot.h"
#include "sg/strfmt.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { RESET_SOFT, RESET_MIXED, RESET_HARD } sg_reset_mode;

/* All three reset modes end by moving whatever HEAD names to the target
   commit and differ only in what they do to the index and working tree
   first -- that shared final step is sg_ref_move_head (include/sg/refs.h):
   it moves the current branch, or -- when HEAD is detached -- HEAD itself,
   leaving every branch alone. current_branch NULL means detached; a corrupt
   HEAD is rejected by this file before that point is reached. */

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
               "sg: reset does not support a pathspec; if you only want to unstage a file, "
               "use `sg restore --staged <path>` instead\n");
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

    /* A detached HEAD resets HEAD itself; only a corrupt one is refused.
       Refusing detached outright was tenable while sg could not produce that
       state, but rebase now replays on a detached HEAD, and refusing here
       would take away resetting during a paused rebase -- a capability
       Phase 14 established and measured against real git, which allows it
       precisely because its own rebase is detached too. */
    current_branch = sg_ref_current_branch(git_dir);
    if (current_branch == NULL && sg_ref_head_is_detached(git_dir) != 1) {
        fprintf(stderr, "sg: cannot read HEAD (.git/HEAD is neither a branch nor a commit id)\n");
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
            fprintf(stderr, "sg: a merge or rebase is in progress, cannot do a soft reset\n");
            free(current_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }
        {
            sg_seq_kind seq_kind = sg_sequencer_kind_in_progress(git_dir);

            if (seq_kind != 0) {
                fprintf(stderr, "sg: a %s is in progress, cannot do a soft reset\n",
                       seq_kind == SG_SEQ_CHERRY_PICK ? "cherry-pick" : "revert");
                free(current_branch);
                free(git_dir);
                free(repo_root);
                return 1;
            }
        }

        /* Neither the index nor the working directory is touched: nothing
           uncommitted is ever at risk, so there is no confirmation gate and
           no automatic snapshot -- same rule real git follows. */
        {
            /* Phase 65: heap, not a fixed 512-byte buffer. Measured against
               git 2.55.0: this line ("reset: moving to <rev_arg>") is
               byte-for-byte what git itself writes to logs/HEAD, and
               rev_arg is a ref name -- a PATH whose per-component length is
               capped but whose total is not (measured with a four-
               component, 803-char branch name: git wrote 939 bytes, sg's
               fixed buffer wrote a silently truncated 630). */
            char *reflog_msg = sg_strfmt_alloc("reset: moving to %s", rev_arg);

            if (reflog_msg == NULL) {
                fprintf(stderr, "sg: out of memory\n");
                free(current_branch);
                free(git_dir);
                free(repo_root);
                return 1;
            }
            if (sg_ref_move_head(git_dir, current_branch, target_commit_id, reflog_msg) != 0) {
                fprintf(stderr, "sg: cannot update %s\n",
                       current_branch != NULL ? current_branch : "HEAD");
                free(reflog_msg);
                free(current_branch);
                free(git_dir);
                free(repo_root);
                return 1;
            }
            free(reflog_msg);
        }
    } else if (mode == RESET_MIXED) {
        sg_index idx;
        char *label;
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
        /* Phase 65: heap, not a fixed 256-byte buffer -- snapshot label,
           sg's own feature (no real-git oracle), but sized to rev_arg
           rather than truncated. */
        label = sg_strfmt_alloc("reset --mixed to '%s'", rev_arg);
        if (label == NULL) {
            fprintf(stderr, "sg: out of memory\n");
            sg_index_free(&idx);
            free(current_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }
        {
            char snap_bad_path[SG_PATH_MAX];

            snap_bad_path[0] = '\0';
            if (sg_snapshot_create(git_dir, repo_root, &idx, label, NULL, snap_bad_path) != 0) {
                if (snap_bad_path[0] != '\0')
                    fprintf(stderr, "sg: automatic snapshot failed: the index names an invalid "
                                    "path (%s), aborting this operation for safety (no changes "
                                    "made)\n",
                           sg_quote_path_delimited(snap_bad_path));
                else
                    fprintf(stderr, "sg: automatic snapshot failed, aborting this operation for "
                                    "safety (no changes made)\n");
                free(label);
                sg_index_free(&idx);
                free(current_branch);
                free(git_dir);
                free(repo_root);
                return 1;
            }
        }
        free(label);
        sg_index_free(&idx);

        merge_in_progress = sg_merge_head_exists(git_dir);

        if (sg_index_reset_to_tree(git_dir, target_tree_id) != 0) {
            free(current_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }

        {
            char *reflog_msg = sg_strfmt_alloc("reset: moving to %s", rev_arg);

            if (reflog_msg == NULL) {
                fprintf(stderr, "sg: out of memory\n");
                free(current_branch);
                free(git_dir);
                free(repo_root);
                return 1;
            }
            if (sg_ref_move_head(git_dir, current_branch, target_commit_id, reflog_msg) != 0) {
                fprintf(stderr, "sg: cannot update %s\n",
                       current_branch != NULL ? current_branch : "HEAD");
                free(reflog_msg);
                free(current_branch);
                free(git_dir);
                free(repo_root);
                return 1;
            }
            free(reflog_msg);
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
            fprintf(stderr, "sg: warning: failed to remove MERGE_HEAD\n");
    } else {
        char *label;
        int apply_rc;

        if (sg_commit_tree_of(git_dir, target_commit_id, target_tree_id) != 0) {
            fprintf(stderr, "sg: corrupt commit for '%s'\n", rev_arg);
            free(current_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }

        /* Phase 65: heap, not a fixed 256-byte buffer -- snapshot label, no
           real-git oracle, sized instead of truncated. */
        label = sg_strfmt_alloc("reset --hard to '%s'", rev_arg);
        if (label == NULL) {
            fprintf(stderr, "sg: out of memory\n");
            free(current_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }
        apply_rc = sg_safe_apply_tree(git_dir, repo_root, target_tree_id, label, force);
        free(label);
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
            char *reflog_msg = sg_strfmt_alloc("reset: moving to %s", rev_arg);

            if (reflog_msg == NULL) {
                fprintf(stderr, "sg: out of memory\n");
                free(current_branch);
                free(git_dir);
                free(repo_root);
                return 1;
            }
            if (sg_ref_move_head(git_dir, current_branch, target_commit_id, reflog_msg) != 0) {
                fprintf(stderr, "sg: cannot update %s\n",
                       current_branch != NULL ? current_branch : "HEAD");
                free(reflog_msg);
                free(current_branch);
                free(git_dir);
                free(repo_root);
                return 1;
            }
            free(reflog_msg);
        }
    }

    free(current_branch);
    free(git_dir);
    free(repo_root);
    return 0;
}
