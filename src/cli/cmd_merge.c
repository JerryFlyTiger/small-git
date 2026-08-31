#include "sg/cli.h"

#include "sg/apply.h"
#include "sg/chunk.h"
#include "sg/diff.h"
#include "sg/diff_out.h"
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
#include "sg/revparse.h"
#include "sg/similarity.h"
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

/* The report git prints after a fast-forward, measured against git 2.55.0:

     Updating <7hex>..<7hex>
     Fast-forward
     <exactly `git diff --stat --summary <old> <new>`>

   Rename detection is ON, because that is `git diff`'s own default and the
   diffstat here is an ordinary one (measured: a renamed file shows as
   `a => b | 0` plus a `rename` summary line, not as an add and a delete).
   A tree pair that differs in nothing prints the two header lines and
   nothing else -- an empty --stat is empty, not " 0 files changed".

   None of this is fatal: the fast-forward itself has already happened and
   been recorded by the time any of it runs, so a diff that cannot be built
   costs the user the report, not the merge. */
static void print_fast_forward_report(const char *git_dir, const char *repo_root,
                                      const unsigned char ours_commit[SG_SHA1_RAW_LEN],
                                      const unsigned char theirs_commit[SG_SHA1_RAW_LEN],
                                      const unsigned char theirs_tree[SG_SHA1_RAW_LEN])
{
    char old_hex[SG_SHA1_HEX_LEN + 1];
    char new_hex[SG_SHA1_HEX_LEN + 1];
    unsigned char ours_tree[SG_SHA1_RAW_LEN];
    char bad_path[SG_PATH_MAX];
    sg_diff_list list;
    sg_diff_out_opts opts;

    sg_sha1_to_hex(ours_commit, old_hex);
    sg_sha1_to_hex(theirs_commit, new_hex);
    printf("Updating %.7s..%.7s\n", old_hex, new_hex);
    printf("Fast-forward\n");

    if (sg_commit_tree_of(git_dir, ours_commit, ours_tree) != 0)
        return;
    memset(&list, 0, sizeof(list));
    if (sg_diff_trees(git_dir, ours_tree, theirs_tree, &list, bad_path) != 0) {
        sg_diff_list_free(&list);
        return;
    }
    if (sg_diff_detect_renames(git_dir, repo_root, &list, SG_SIMILARITY_DEFAULT, 0) != 0) {
        sg_diff_list_free(&list);
        return;
    }
    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_STAT;
    opts.summary = 1;
    sg_diff_print(git_dir, repo_root, &list, &opts);
    sg_diff_list_free(&list);
}

/* current_branch may be NULL (detached HEAD): sg_ref_move_head then moves
   HEAD itself instead of a branch, leaving every branch ref untouched, same
   as real git measured against a detached fast-forward merge.

   ours_commit is NULL when HEAD is UNBORN, and that case prints NOTHING AT
   ALL -- not even `Fast-forward`. Measured against git 2.55.0: merging a
   branch into a branch with no commits yet fast-forwards silently, rc 0,
   empty stdout and empty stderr, with HEAD genuinely moved. It is not that
   the header is skipped for want of an old id; git prints no report at all
   there. */
static int do_fast_forward(const char *git_dir, const char *repo_root, const char *current_branch,
                           const char *branch_arg, const unsigned char *ours_commit,
                           const unsigned char theirs_commit[SG_SHA1_RAW_LEN],
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

    if (ours_commit != NULL)
        print_fast_forward_report(git_dir, repo_root, ours_commit, theirs_commit, theirs_tree);
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

/* git's merge_name() (builtin/merge.c), measured against git 2.55.0 rather
   than recalled -- every row below is a `git merge --no-commit` whose
   .git/MERGE_MSG was read back:

     topic              -> Merge branch 'topic'
     refs/heads/topic   -> Merge branch 'refs/heads/topic'   (NOT shortened)
     v1 / av1 (a tag)   -> Merge tag 'v1'
     topic~0            -> Merge branch 'topic'              (suffix stripped)
     <40-hex>           -> Merge commit '<40-hex>'

   Two things are easy to get backwards. The name printed is the argument AS
   TYPED, not the ref that was found -- which is why `refs/heads/topic` keeps
   its prefix. And a trailing run of `^` or a trailing `~<digits>` is stripped
   before classifying, then the SHORTENED name is what gets printed, so
   `topic~0` reads as a branch merge. Anything that is neither a branch nor a
   tag after stripping is a commit. */
static void build_merge_name(const char *git_dir, const char *arg, char *out, size_t out_size)
{
    char base[SG_PATH_MAX];
    size_t len;
    unsigned char id[SG_SHA1_RAW_LEN];
    char tag_path[SG_PATH_MAX];

    if (snprintf(base, sizeof(base), "%s", arg) >= (int)sizeof(base)) {
        snprintf(out, out_size, "commit '%s'", arg);
        return;
    }
    for (;;) {
        len = strlen(base);
        while (len > 1 && base[len - 1] == '^')
            len--;
        if (len == strlen(base)) {
            size_t digits = 0;

            while (len > 1 && base[len - 1] >= '0' && base[len - 1] <= '9') {
                len--;
                digits++;
            }
            if (digits == 0 || len <= 1 || base[len - 1] != '~') {
                /* nothing stripped this round -- the name is as short as it
                   gets, so stop rather than loop forever */
                len = strlen(base);
                break;
            }
            len--;
        }
        base[len] = '\0';
    }
    if (sg_ref_branch_exists(git_dir, base) ||
       (strncmp(base, "refs/heads/", 11) == 0 && sg_ref_read_path(git_dir, base, id) == 0)) {
        snprintf(out, out_size, "branch '%s'", base);
        return;
    }
    if (snprintf(tag_path, sizeof(tag_path), "refs/tags/%s", base) < (int)sizeof(tag_path) &&
       sg_ref_read_path(git_dir, tag_path, id) == 0) {
        snprintf(out, out_size, "tag '%s'", base);
        return;
    }
    if (strncmp(base, "refs/tags/", 10) == 0 && sg_ref_read_path(git_dir, base, id) == 0) {
        snprintf(out, out_size, "tag '%s'", base);
        return;
    }
    snprintf(out, out_size, "commit '%s'", base);
}

/* git omits the " into <branch>" suffix on exactly two branch names,
   measured: `master` and `main`. It is NOT the configured
   init.defaultBranch -- setting that to `trunk` and merging on `trunk` still
   produced " into trunk". A detached HEAD gets " into HEAD". */
static int merge_msg_names_target(const char *ours_label)
{
    return strcmp(ours_label, "master") != 0 && strcmp(ours_label, "main") != 0;
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
       always writes HEAD -- a deliberate divergence -- but with HEAD
       detached there is no branch to name and git's own answer is the only
       one available. Computed once: passing current_branch straight through
       is what made a conflicting detached merge write NULL into the
       "<<<<<<< %s" marker and crash.

       This comment used to claim the divergence was "pinned by phase4b".
       It was not: measured in Phase 41, interop's only three mentions of
       "<<<<<<<" assert the marker is ABSENT after an abort (twice) and pin
       the STASH labels (once, a different call site), and
       tests/test_merge_content.c pins only that sg_merge_content writes
       whatever label its caller hands it -- never which label this function
       picks. The pin the comment promised now exists, in interop's phase41
       group, on both sides as this project's convention for a deliberate
       divergence requires. A comment asserting a guard that is not there is
       worse than no comment: it stops the next reader adding one. */
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

        char snap_bad_path[SG_PATH_MAX];

        snprintf(label, sizeof(label), "merge %s", branch_arg);
        snap_bad_path[0] = '\0';
        if (sg_snapshot_create(git_dir, repo_root, &idx, label, NULL, snap_bad_path) != 0) {
            if (snap_bad_path[0] != '\0')
                fprintf(stderr, "sg: automatic snapshot failed: the index names an invalid path "
                                "(%s), aborting this merge for safety (no changes made)\n",
                       sg_quote_path_delimited(snap_bad_path));
            else
                fprintf(stderr, "sg: automatic snapshot failed, aborting this merge for safety "
                                "(no changes made)\n");
            sg_index_free(&idx);
            return 1;
        }
    }
    sg_index_free(&idx);

    if (sg_merge_trees(git_dir, base_tree, ours_tree, theirs_tree, ours_label, branch_arg,
                       SG_SIMILARITY_DEFAULT, &result) != 0) {
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

        char merge_name[SG_PATH_MAX];

        build_merge_name(git_dir, branch_arg, merge_name, sizeof(merge_name));
        if (merge_msg_names_target(ours_label))
            snprintf(message, sizeof(message), "Merge %s into %s\n", merge_name, ours_label);
        else
            snprintf(message, sizeof(message), "Merge %s\n", merge_name);
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
    {
        char snap_bad_path[SG_PATH_MAX];

        snap_bad_path[0] = '\0';
        if (sg_snapshot_create(git_dir, repo_root, &idx, "merge --abort", NULL, snap_bad_path) !=
           0) {
            if (snap_bad_path[0] != '\0')
                fprintf(stderr, "sg: automatic snapshot failed: the index names an invalid path "
                                "(%s), aborting the abort for safety (no changes made)\n",
                       sg_quote_path_delimited(snap_bad_path));
            else
                fprintf(stderr, "sg: automatic snapshot failed, aborting the abort for safety (no "
                                "changes made)\n");
            sg_index_free(&idx);
            return 1;
        }
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

        /* Any revision sg_rev_parse_commit understands, not just a bare
           branch name (Phase 43). The old code called sg_ref_branch_exists
           directly, which made `sg merge v1` / `refs/heads/topic` /
           `topic~0` fail with "invalid reference" while the equivalent `git
           merge` succeeded -- and contradicted this project's own rule that
           a user-supplied revision always goes through sg_rev_parse_commit.
           That function peels annotated tags, which is what merge wants:
           measured, `git merge <annotated-tag>` merges the tagged COMMIT. */
        if (sg_rev_parse_commit(git_dir, branch_arg, theirs_commit) != 0) {
            fprintf(stderr, "sg: %s - not something we can merge\n", branch_arg);
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
            rc = do_fast_forward(git_dir, repo_root, current_branch, branch_arg, NULL,
                                 theirs_commit, theirs_tree, force);
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
            rc = do_fast_forward(git_dir, repo_root, current_branch, branch_arg, ours_commit,
                                 theirs_commit, theirs_tree, force);
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
