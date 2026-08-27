#include "sg/cli.h"

#include "sg/apply.h"
#include "sg/chunk.h"
#include "sg/hash.h"
#include "sg/index.h"
#include "sg/loose.h"
#include "sg/merge.h"
#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/quote.h"
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

/* current_branch may be NULL (detached HEAD): sg_ref_move_head then moves
   HEAD itself instead of a branch, leaving every branch ref untouched, same
   as real git measured against a detached fast-forward merge. */
static int do_fast_forward(const char *git_dir, const char *repo_root, const char *current_branch,
                           const char *branch_arg, const unsigned char theirs_commit[SG_SHA1_RAW_LEN],
                           const unsigned char theirs_tree[SG_SHA1_RAW_LEN], int force)
{
    char label[300];
    int apply_rc;
    char reflog_msg[400];

    snprintf(label, sizeof(label), "merge %s (fast-forward)", branch_arg);
    apply_rc = sg_safe_apply_tree(git_dir, repo_root, theirs_tree, label, force);
    if (apply_rc == 1) {
        fprintf(stderr, "sg: merge aborted\n");
        return 1;
    }
    if (apply_rc != 0)
        return 1;

    snprintf(reflog_msg, sizeof(reflog_msg), "merge %s: Fast-forward", branch_arg);
    if (sg_ref_move_head(git_dir, current_branch, theirs_commit, reflog_msg) != 0) {
        fprintf(stderr, "sg: failed to update HEAD\n");
        return 1;
    }

    printf("Fast-forward\n");
    return 0;
}

static void print_conflict_message(char **conflict_paths, size_t conflict_count)
{
    size_t i;

    fprintf(stderr, "Automatic merge failed; the following files have conflicts:\n");
    for (i = 0; i < conflict_count; i++)
        fprintf(stderr, "    %s\n", sg_quote_path(conflict_paths[i]));
    fprintf(stderr, "Edit these files to resolve the conflicts, then:\n");
    fprintf(stderr, "  sg add <file>...     mark as resolved\n");
    fprintf(stderr, "  sg commit -m \"...\"   finish this merge\n");
    fprintf(stderr, "Or give up this merge:\n");
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
    char **conflict_paths = NULL;
    size_t conflict_count = 0;
    size_t i;
    int has_conflict;
    int rc = 1;
    /* What "our" side is called: in conflict markers, in the generated merge
       message, and in the summary line. sg names the branch where real git
       always writes HEAD -- a pre-existing divergence pinned by phase4b --
       but with HEAD detached there is no branch to name and git's own answer
       is the only one available. Computed once: passing current_branch
       straight through is what made a conflicting detached merge write NULL
       into the "<<<<<<< %s" marker and crash. */
    const char *ours_label = (current_branch != NULL) ? current_branch : "HEAD";

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
            fprintf(stderr, "sg: automatic snapshot failed, aborting this merge for safety (no changes made)\n");
            sg_index_free(&idx);
            return 1;
        }
    }
    sg_index_free(&idx);

    if (sg_merge_trees(git_dir, base_tree, ours_tree, theirs_tree, ours_label, branch_arg,
                       &result) != 0) {
        fprintf(stderr, "sg: an error occurred while merging\n");
        return 1;
    }

    /* Materializes the merge result into the working tree and a fresh
       index; a -1 here means either a chunked blob's data was unrecoverable
       or the index couldn't be built completely -- refuse to record an
       index/commit that silently drops content or paths. */
    if (sg_merge_result_apply(git_dir, repo_root, &result, &new_idx, &conflict_paths,
                              &conflict_count) != 0) {
        sg_merge_result_free(&result);
        return 1;
    }
    has_conflict = conflict_count > 0;

    if (sg_index_write(git_dir, &new_idx) != 0) {
        fprintf(stderr, "sg: failed to write index\n");
        sg_index_free(&new_idx);
        sg_merge_result_free(&result);
        for (i = 0; i < conflict_count; i++)
            free(conflict_paths[i]);
        free(conflict_paths);
        return 1;
    }

    if (sg_merge_head_write(git_dir, theirs_commit) != 0) {
        fprintf(stderr, "sg: failed to write MERGE_HEAD\n");
        sg_index_free(&new_idx);
        sg_merge_result_free(&result);
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

        snprintf(message, sizeof(message), "Merge branch '%s' into %s\n", branch_arg, ours_label);
        if (sg_message_cleanup(message, &cleaned_message) != 0) {
            fprintf(stderr, "sg: out of memory\n");
            rc = 1;
            goto done;
        }

        memset(&commit, 0, sizeof(commit));
        if (sg_tree_build_from_index(git_dir, &new_idx, commit.tree) != 0) {
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

        {
            /* Real git logs "Merge made by the 'ort' strategy." here (its
               merge strategy name has changed release to release --
               'recursive', then 'ort'). sg keeps that phrasing but is
               honest about which engine actually ran: it isn't ort, so
               claiming so would be a straight-up lie the moment someone
               ran `git reflog` against an sg-built repo. This is a
               deliberate divergence from real git's exact text, not a gap
               -- interop coverage for this line asserts sg's own string. */
            char reflog_msg[400];

            snprintf(reflog_msg, sizeof(reflog_msg), "merge %s: Merge made by the 'sg-3way' strategy.",
                    branch_arg);
            if (sg_ref_move_head(git_dir, current_branch, new_commit_id, reflog_msg) != 0) {
                fprintf(stderr, "sg: failed to update HEAD\n");
                rc = 1;
                goto done;
            }
        }

        if (sg_merge_head_remove(git_dir) != 0)
            fprintf(stderr, "sg: warning: merge succeeded but failed to remove MERGE_HEAD\n");

        {
            char commit_hex[SG_SHA1_HEX_LEN + 1];
            char short_hex[8];

            sg_sha1_to_hex(new_commit_id, commit_hex);
            memcpy(short_hex, commit_hex, 7);
            short_hex[7] = '\0';
            if (current_branch != NULL)
                printf("Merge made by '%s' [%s] into '%s'.\n", branch_arg, short_hex, current_branch);
            else
                printf("Merge made by '%s' [%s] into HEAD.\n", branch_arg, short_hex);
        }
        rc = 0;
    }

done:
    sg_index_free(&new_idx);
    sg_merge_result_free(&result);
    for (i = 0; i < conflict_count; i++)
        free(conflict_paths[i]);
    free(conflict_paths);
    return rc;
}

static int do_merge_abort(const char *git_dir, const char *repo_root)
{
    unsigned char head_id[SG_SHA1_RAW_LEN];
    unsigned char head_tree[SG_SHA1_RAW_LEN];
    sg_index idx;

    /* Existence, not parseability: abort never needs MERGE_HEAD's value (it
       resets to HEAD), and a corrupt MERGE_HEAD is precisely the state a
       user most needs to abort out of. Real git 2.55.0 clears a malformed
       MERGE_HEAD here without complaint (measured). Using
       sg_merge_head_read would refuse instead, and -- now that `switch`
       gates on the same file -- would leave the repository with no way out
       short of deleting .git/MERGE_HEAD by hand. */
    if (!sg_merge_head_exists(git_dir)) {
        fprintf(stderr, "sg: not currently merging (MERGE_HEAD not found)\n");
        return 1;
    }
    if (sg_ref_resolve_head(git_dir, head_id) != 0 ||
       sg_commit_tree_of(git_dir, head_id, head_tree) != 0) {
        fprintf(stderr, "sg: cannot read the current branch's commit\n");
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
        fprintf(stderr, "sg: automatic snapshot failed, aborting the abort for safety (no changes made)\n");
        sg_index_free(&idx);
        return 1;
    }
    sg_index_free(&idx);

    if (sg_apply_tree_to_workdir(git_dir, repo_root, head_tree) != 0) {
        fprintf(stderr, "sg: failed to restore the working directory\n");
        return 1;
    }
    if (sg_merge_head_remove(git_dir) != 0)
        fprintf(stderr, "sg: warning: failed to remove MERGE_HEAD\n");

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
        int has_head;
        char *current_branch;
        int mb_rc;

        /* Starting a second merge on top of an unfinished one would drop the
           first one's MERGE_HEAD and conflict staging on the floor. */
        if (sg_merge_head_exists(git_dir)) {
            fprintf(stderr,
                   "sg: an unfinished merge is in progress\n"
                   "Finish it first (resolve conflicts, sg add <file>..., then sg commit), "
                   "or run sg merge --abort to give up\n");
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
                   "sg: a rebase is currently in progress\n"
                   "Finish it first (sg rebase --continue) or run sg rebase --abort to give up\n");
            free(git_dir);
            free(repo_root);
            return 1;
        }

        /* Real git merges fine on a detached HEAD (measured, git 2.55.0):
           it just moves HEAD itself and leaves every branch ref alone.
           current_branch NULL from here on means exactly that -- "detached,
           legitimate" -- not a failure; only a corrupt HEAD (neither a
           branch nor a resolvable commit) is refused, same phrasing as
           reset's and rebase's refusals so phase18e's loop can tell the two
           apart by prefix.

           This runs BEFORE the clean-work-tree check, not after it as the
           refusal it replaced did. Everything that compares the work tree
           against HEAD has to read HEAD first, so with HEAD corrupt the
           comparison comes back "every tracked file is a new addition" and
           the user is told their work tree is dirty -- blaming the one part
           of the repository that is fine. Diagnosing HEAD first is what
           makes the message name the actual problem. */
        current_branch = sg_ref_current_branch(git_dir);
        if (current_branch == NULL && sg_ref_head_is_detached(git_dir) != 1) {
            fprintf(stderr, "sg: cannot read HEAD (.git/HEAD is neither a branch nor a commit id)\n");
            free(git_dir);
            free(repo_root);
            return 1;
        }

        if (sg_require_clean_workdir(git_dir, repo_root, "sg merge") != 0) {
            free(current_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }

        if (!sg_ref_branch_exists(git_dir, branch_arg)) {
            fprintf(stderr, "sg: invalid reference: %s\n", branch_arg);
            free(current_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }
        if (sg_ref_read_branch(git_dir, branch_arg, theirs_commit) != 0) {
            fprintf(stderr, "sg: failed to read branch '%s'\n", branch_arg);
            free(current_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }

        has_head = (sg_ref_resolve_head(git_dir, ours_commit) == 0);

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
            fprintf(stderr, "sg: found multiple unrelated common ancestors (criss-cross history), cannot merge automatically\n");
            free(current_branch);
            free(git_dir);
            free(repo_root);
            return 1;
        }
        if (mb_rc == -1) {
            fprintf(stderr, "sg: '%s' has no common history with the current branch, cannot merge\n", branch_arg);
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
