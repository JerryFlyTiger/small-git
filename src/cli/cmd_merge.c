#include "sg/cli.h"

#include "sg/apply.h"
#include "sg/cli_args.h"
#include "sg/chunk.h"
#include "sg/diff.h"
#include "sg/diff_out.h"
#include "sg/hash.h"
#include "sg/ident.h"
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
#include "sg/sequencer.h"
#include "sg/similarity.h"
#include "sg/snapshot.h"
#include "sg/status.h"
#include "sg/strfmt.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

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
    if (sg_diff_trees(git_dir, ours_tree, theirs_tree, &list, bad_path, 0) != 0) {
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

/* Phase 79 (Phase 79b added the directory bucket). Prints git's own
   untracked-overwrite refusal for BOTH buckets sg_untracked_would_be_
   overwritten can report -- a blocking FILE (`file_collisions`) and a
   blocking non-empty DIRECTORY (`dir_collisions`, Phase 79b) -- and frees
   both. There are FOUR wordings total, measured against git 2.55.0 (not
   two: the directory bucket has its own ordinary/unborn pair, distinct
   from the file bucket's):

     file, plural (ordinary merge, fast-forward and 3-way):
       error: The following untracked working tree files would be overwritten by merge:
       \t<path>
       \t<path>
       Please move or remove them before you merge.

     file, singular (unborn HEAD; `file_collisions` has exactly one entry):
       error: Untracked working tree file '<path>' would be overwritten by merge.

     dir, plural (ordinary merge, fast-forward and 3-way):
       error: Updating the following directories would lose untracked files in them:
       \t<path>
       \t<path>
       <blank line>

     dir, singular (unborn HEAD; `dir_collisions` has exactly one entry):
       error: Updating '<path>' would lose untracked files in it
       fatal: read-tree failed

   git's own "error: " becomes "sg: " on every one of those first lines
   (each is printed as its own independent line, so when BOTH buckets are
   non-empty in the ordinary shape, BOTH lines get their own "sg: "), the
   same substitution this project already makes for other borrowed wording.
   "fatal: read-tree failed" leaks git's own internal plumbing name and is
   deliberately NOT reproduced by sg, same treatment as the file bucket's
   unborn case already gets (this project has no read-tree of its own to
   leak).

   Ordinary shape, both buckets combined (measured byte-for-byte): the
   directory section (if non-empty) prints first, ending in a blank line
   -- present EVEN when there is no file section afterward -- then the file
   section (if non-empty), then exactly ONE "Aborting" line, then
   "Merge with strategy ort failed." if `three_way`. There is never more
   than one "Aborting"/strategy-failure pair even when both buckets fire.

   Unborn shape, Phase 79c (round 2) correction: when BOTH buckets are
   non-empty, git does NOT always pick the directory wording -- it reports
   exactly ONE collision, the FIRST one in CANDIDATE order across BOTH
   kinds, using that collision's own wording. Measured against git 2.55.0
   (U1b in the Phase 79c oracle): topic adds "a" then "z" to an unborn HEAD;
   locally "a" is an untracked FILE and "z" is a non-empty untracked
   DIRECTORY. Candidate order puts "a" first, and git refuses on the FILE
   wording naming "a" -- NOT the directory wording naming "z", which is
   what an earlier "directory always wins" assumption in this comment used
   to (wrongly) predict. `first_is_dir` (from
   sg_untracked_would_be_overwritten's own out-param of the same name) is
   how the caller tells this function which bucket's [0] entry is the
   actually-first one; it is meaningless when `unborn` is 0.
   WARNING: **the unborn singular forms name only the FIRST colliding entry
   of their own bucket even when several collide** (measured for the file
   bucket: three collisions, git names `f0.txt` alone and exits 128;
   assumed by symmetry, not separately measured, for the directory bucket).
   Printing all of them would be sg inventing output git does not produce.

   Paths are printed RAW in both buckets, through neither sg_quote_path nor
   sg_quote_path_delimited -- measured, git does not quote them here even for
   a space, a double quote, or UTF-8, and this project's goal is byte
   compatibility with git's own wording. Every OTHER path-printing site in
   this file quotes; do not "fix" this one to match them.

   `unborn` selects the singular wordings (used only by the unborn-HEAD
   fast-forward call site, never inferred from either bucket's count == 1
   alone -- an ordinary merge that happens to collide on exactly one path
   still uses the PLURAL wording, just with one \t line). `three_way`
   selects whether "Merge with strategy ort failed." is appended after
   "Aborting" in the ordinary shape (measured: present for a 3-way merge,
   absent for a fast-forward; mutually exclusive with `unborn` in practice,
   kept as a separate parameter rather than a combined enum so a future
   caller cannot accidentally request an unreachable "unborn 3-way"
   combination by picking the wrong enum value).

   Exit code is 1 in every shape (git: 2 for 3-way, 1 for fast-forward, 128
   for unborn HEAD) -- this project's standing "exit codes are only ever 0 or
   1" convention (CLAUDE.md's Code conventions), the same shape as deliberate
   divergence #3. Returns 1, always. */
/* Phase 79c (F2): prints a truthful reason for a -1 from
   sg_untracked_would_be_overwritten, instead of the blanket "sg: out of
   memory" every call site used to print regardless of cause. Measured
   against git 2.55.0 (S6 in the Phase 79c oracle): a chmod-000
   subdirectory makes git print
     warning: could not open directory 'new.txt/locked/': Permission denied
     fatal: cannot opendir 'new.txt/locked': Permission denied
   -- git's "warning:"/"fatal:" both become "sg: " (this project's own
   substitution, same as report_untracked_overwrite's "error:" -> "sg: ").
   git's own opendir warning ends its path in a trailing slash; its fatal
   line does not -- reproduced literally rather than "fixed" to match. There
   is no measured git wording for a bare readdir() failure reached after
   opendir already succeeded (a shape git's own code does not appear to hit
   under any of this oracle's fixtures); "cannot readdir" is this project's
   own choice of verb, not a borrowed one. SG_UNTRACKED_ERR_PATH_TOO_LONG
   (a path that overflowed the SG_PATH_MAX join bound mid-scan) is reported
   as a "cannot lstat" line with ENAMETOOLONG's own strerror text, on the
   theory that the join failure stands in for "the lstat that would have
   come next could not even be attempted" -- also not a git-measured
   wording, recorded as a residual (see docs/RULES-merge.md's Phase 79c
   section: git's own S2 answer here is a multi-line warning cascade this
   project does not reproduce). SG_UNTRACKED_ERR_ALLOC (a real allocation
   failure) is the only kind that still prints "sg: out of memory". */
static void print_untracked_scan_error(const sg_untracked_overwrite_error *err)
{
    const char *path = err->path != NULL ? err->path : "?";

    switch (err->kind) {
    case SG_UNTRACKED_ERR_OPENDIR:
        fprintf(stderr, "sg: warning: could not open directory '%s/': %s\n", path,
               strerror(err->saved_errno));
        fprintf(stderr, "sg: cannot opendir '%s': %s\n", path, strerror(err->saved_errno));
        break;
    case SG_UNTRACKED_ERR_READDIR:
        fprintf(stderr, "sg: cannot readdir '%s': %s\n", path, strerror(err->saved_errno));
        break;
    case SG_UNTRACKED_ERR_LSTAT:
        fprintf(stderr, "sg: cannot lstat '%s': %s\n", path, strerror(err->saved_errno));
        break;
    case SG_UNTRACKED_ERR_PATH_TOO_LONG:
        fprintf(stderr, "sg: cannot lstat '%s': %s\n", path, strerror(ENAMETOOLONG));
        break;
    case SG_UNTRACKED_ERR_ALLOC:
    case SG_UNTRACKED_ERR_NONE:
    default:
        fprintf(stderr, "sg: out of memory\n");
        break;
    }
}

static int report_untracked_overwrite(char **dir_collisions, size_t dir_count,
                                      char **file_collisions, size_t file_count, int unborn,
                                      int first_is_dir, int three_way)
{
    size_t i;

    if (unborn) {
        /* Phase 79c (F4): first_is_dir names which bucket's [0] entry is
           the actually-first collision in candidate order -- NOT "prefer
           the directory bucket whenever it is non-empty", see this
           function's own doc comment above. */
        if (first_is_dir && dir_count > 0) {
            fprintf(stderr, "sg: Updating '%s' would lose untracked files in it\n",
                   dir_collisions[0]);
        } else if (file_count > 0) {
            fprintf(stderr,
                   "sg: Untracked working tree file '%s' would be overwritten by merge.\n",
                   file_collisions[0]);
        } else {
            fprintf(stderr, "sg: Updating '%s' would lose untracked files in it\n",
                   dir_collisions[0]);
        }
    } else {
        if (dir_count > 0) {
            fprintf(stderr,
                   "sg: Updating the following directories would lose untracked files in "
                   "them:\n");
            for (i = 0; i < dir_count; i++)
                fprintf(stderr, "\t%s\n", dir_collisions[i]);
            fprintf(stderr, "\n");
        }
        if (file_count > 0) {
            fprintf(stderr,
                   "sg: The following untracked working tree files would be overwritten by "
                   "merge:\n");
            for (i = 0; i < file_count; i++)
                fprintf(stderr, "\t%s\n", file_collisions[i]);
            fprintf(stderr, "Please move or remove them before you merge.\n");
        }
        fprintf(stderr, "Aborting\n");
        if (three_way)
            fprintf(stderr, "Merge with strategy ort failed.\n");
    }
    for (i = 0; i < dir_count; i++)
        free(dir_collisions[i]);
    free(dir_collisions);
    for (i = 0; i < file_count; i++)
        free(file_collisions[i]);
    free(file_collisions);
    return 1;
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
    char *label;
    int apply_rc;
    char *reflog_msg;

    /* Phase 79: must run BEFORE sg_safe_apply_tree, so a refusal here writes
       and snapshots nothing. Candidate set is every path theirs_tree adds
       that the current index does not already have -- for the unborn-HEAD
       call (ours_commit == NULL) the index is empty (sg_require_clean_workdir
       already gated on that, since a non-empty index would show as staged
       changes against the unborn HEAD's virtual empty tree), so this reduces
       to exactly "every path in theirs_tree", matching the unborn case's own
       spec. */
    {
        sg_flat_list theirs_flat;
        sg_index idx;
        char bad_path[SG_PATH_MAX];
        char **candidates = NULL;
        size_t candidate_count = 0;
        size_t i;

        if (sg_tree_flatten(git_dir, theirs_tree, &theirs_flat, bad_path) != 0) {
            fprintf(stderr, "sg: failed to read target tree\n");
            return 1;
        }
        if (sg_index_read(git_dir, &idx) != 0) {
            fprintf(stderr, "sg: failed to read index (corrupt?)\n");
            sg_flat_list_free(&theirs_flat);
            return 1;
        }

        candidates = malloc(theirs_flat.count * sizeof(*candidates));
        if (theirs_flat.count > 0 && candidates == NULL) {
            fprintf(stderr, "sg: out of memory\n");
            sg_index_free(&idx);
            sg_flat_list_free(&theirs_flat);
            return 1;
        }
        for (i = 0; i < theirs_flat.count; i++) {
            if (sg_index_find(&idx, theirs_flat.entries[i].path) < 0)
                candidates[candidate_count++] = theirs_flat.entries[i].path;
        }

        if (candidate_count > 0) {
            char **file_collisions = NULL;
            size_t file_collision_count = 0;
            char **dir_collisions = NULL;
            size_t dir_collision_count = 0;
            int first_is_dir = 0;
            sg_untracked_overwrite_error scan_err;
            int rc;

            rc = sg_untracked_would_be_overwritten(git_dir, repo_root, &idx,
                                                   (const char *const *)candidates,
                                                   candidate_count, &file_collisions,
                                                   &file_collision_count, &dir_collisions,
                                                   &dir_collision_count, &first_is_dir,
                                                   &scan_err);
            sg_index_free(&idx);
            if (rc != 0) {
                print_untracked_scan_error(&scan_err);
                sg_untracked_overwrite_error_free(&scan_err);
                free(candidates);
                sg_flat_list_free(&theirs_flat);
                return 1;
            }
            if (file_collision_count > 0 || dir_collision_count > 0) {
                free(candidates);
                sg_flat_list_free(&theirs_flat);
                return report_untracked_overwrite(dir_collisions, dir_collision_count,
                                                  file_collisions, file_collision_count,
                                                  ours_commit == NULL, first_is_dir, 0);
            }
        } else {
            sg_index_free(&idx);
        }
        free(candidates);
        sg_flat_list_free(&theirs_flat);
    }

    /* Phase 65: heap, not a fixed 300-byte buffer -- this is a snapshot
       label (sg's own feature, no real-git oracle), but a truncated one
       would still silently misname the snapshot. */
    label = sg_strfmt_alloc("merge %s (fast-forward)", branch_arg);
    if (label == NULL) {
        fprintf(stderr, "sg: out of memory\n");
        return 1;
    }
    apply_rc = sg_safe_apply_tree(git_dir, repo_root, theirs_tree, label, force);
    free(label);
    if (apply_rc == 1) {
        fprintf(stderr, "sg: merge aborted\n");
        return 1;
    }
    if (apply_rc != 0)
        return 1;

    /* Phase 65: heap, not a fixed 400-byte buffer. Measured against real
       git 2.55.0: this line ("merge <branch>: Fast-forward") IS byte-for-
       byte what git itself writes to logs/HEAD on a fast-forward merge, so
       a long branch_arg silently truncating it is a real divergence, not
       just a cosmetically short reflog line. */
    reflog_msg = sg_strfmt_alloc("merge %s: Fast-forward", branch_arg);
    if (reflog_msg == NULL) {
        fprintf(stderr, "sg: out of memory\n");
        return 1;
    }
    if (sg_ref_move_head(git_dir, current_branch, theirs_commit, reflog_msg) != 0) {
        if (!sg_ref_lock_err_report_ex(stderr, "HEAD", "HEAD", NULL))
            fprintf(stderr, "sg: failed to update HEAD\n");
        free(reflog_msg);
        return 1;
    }
    free(reflog_msg);

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
/* Returns a malloc'd string the caller frees, NULL on OOM. Phase 65: `out`/
   `out_size` used to be a caller-supplied `char[SG_PATH_MAX]` buffer, and
   every branch below formats "<kind> '<base>'" straight into it -- `base`
   itself is bounded to SG_PATH_MAX-1 bytes by the first snprintf below, but
   formatting it back with a "branch '"/"tag '"/"commit '" wrapper and a
   closing quote can still overflow an SG_PATH_MAX-sized `out` for a `base`
   within a few bytes of that bound, silently truncating the merge commit
   MESSAGE. Heap avoids that the same way the other Phase 65 sites do. */
static char *build_merge_name(const char *git_dir, const char *arg)
{
    char base[SG_PATH_MAX];
    size_t len;
    unsigned char id[SG_SHA1_RAW_LEN];
    char tag_path[SG_PATH_MAX];

    if (snprintf(base, sizeof(base), "%s", arg) >= (int)sizeof(base))
        return sg_strfmt_alloc("commit '%s'", arg);
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
       (strncmp(base, "refs/heads/", 11) == 0 && sg_ref_read_path(git_dir, base, id) == 0))
        return sg_strfmt_alloc("branch '%s'", base);
    if (snprintf(tag_path, sizeof(tag_path), "refs/tags/%s", base) < (int)sizeof(tag_path) &&
       sg_ref_read_path(git_dir, tag_path, id) == 0)
        return sg_strfmt_alloc("tag '%s'", base);
    if (strncmp(base, "refs/tags/", 10) == 0 && sg_ref_read_path(git_dir, base, id) == 0)
        return sg_strfmt_alloc("tag '%s'", base);
    return sg_strfmt_alloc("commit '%s'", base);
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
        /* Phase 65: heap, not a fixed 300-byte buffer -- snapshot label,
           sg's own feature (no real-git oracle), but still worth sizing to
           branch_arg rather than truncating it silently. */
        char *label = sg_strfmt_alloc("merge %s", branch_arg);
        char snap_bad_path[SG_PATH_MAX];

        if (label == NULL) {
            fprintf(stderr, "sg: out of memory\n");
            sg_index_free(&idx);
            return 1;
        }
        snap_bad_path[0] = '\0';
        if (sg_snapshot_create(git_dir, repo_root, &idx, label, NULL, snap_bad_path) != 0) {
            if (snap_bad_path[0] != '\0')
                fprintf(stderr, "sg: automatic snapshot failed: the index names an invalid path "
                                "(%s), aborting this merge for safety (no changes made)\n",
                       sg_quote_path_delimited(snap_bad_path));
            else
                fprintf(stderr, "sg: automatic snapshot failed, aborting this merge for safety "
                                "(no changes made)\n");
            free(label);
            sg_index_free(&idx);
            return 1;
        }
        free(label);
    }
    sg_index_free(&idx);

    if (sg_merge_trees(git_dir, base_tree, ours_tree, theirs_tree, ours_label, branch_arg,
                       SG_SIMILARITY_DEFAULT, &result) != 0) {
        fprintf(stderr, "sg: an error occurred while merging\n");
        return 1;
    }

    /* Phase 79: pre-flight, after sg_merge_trees and before
       sg_merge_result_apply -- mirrors sg_stash_apply's own pre-flight
       (safety/stash.c). Candidate set: every entry the merge result will
       WRITE that ours does not already have. !ours_present rules out a path
       ours already has (not being newly created); !deleted rules out an
       entry the merge result removes; !conflict_no_workdir_file rules out
       rename/rename-1to2's original-path stage-1-only entry, which writes
       nothing to the working tree at all (see sg/merge.h). A CONFLICTED
       entry at a path ours lacks (e.g. a modify/delete conflict where ours
       deleted the file) is deliberately included by this condition, not
       excluded: sg_merge_result_apply still writes theirs' surviving content
       there. */
    {
        sg_index conflict_idx;
        char **candidates = NULL;
        size_t candidate_count = 0;
        size_t ci;

        if (sg_index_read(git_dir, &conflict_idx) != 0) {
            fprintf(stderr, "sg: failed to read index (corrupt?)\n");
            sg_merge_result_free(&result);
            return 1;
        }
        candidates = malloc(result.count * sizeof(*candidates));
        if (result.count > 0 && candidates == NULL) {
            fprintf(stderr, "sg: out of memory\n");
            sg_index_free(&conflict_idx);
            sg_merge_result_free(&result);
            return 1;
        }
        for (ci = 0; ci < result.count; ci++) {
            sg_merge_result_entry *e = &result.entries[ci];

            if (!e->ours_present && !e->deleted && !e->conflict_no_workdir_file)
                candidates[candidate_count++] = e->path;
        }
        if (candidate_count > 0) {
            char **file_collisions = NULL;
            size_t file_collision_count = 0;
            char **dir_collisions = NULL;
            size_t dir_collision_count = 0;
            int first_is_dir = 0;
            sg_untracked_overwrite_error scan_err;
            int urc;

            urc = sg_untracked_would_be_overwritten(git_dir, repo_root, &conflict_idx,
                                                    (const char *const *)candidates,
                                                    candidate_count, &file_collisions,
                                                    &file_collision_count, &dir_collisions,
                                                    &dir_collision_count, &first_is_dir,
                                                    &scan_err);
            sg_index_free(&conflict_idx);
            if (urc != 0) {
                print_untracked_scan_error(&scan_err);
                sg_untracked_overwrite_error_free(&scan_err);
                free(candidates);
                sg_merge_result_free(&result);
                return 1;
            }
            if (file_collision_count > 0 || dir_collision_count > 0) {
                free(candidates);
                sg_merge_result_free(&result);
                /* Not unborn (3-way always has HEAD), so first_is_dir is
                   irrelevant here -- passed as 0 for clarity. */
                return report_untracked_overwrite(dir_collisions, dir_collision_count,
                                                  file_collisions, file_collision_count, 0, 0, 1);
            }
        } else {
            sg_index_free(&conflict_idx);
        }
        free(candidates);
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
        char *message;
        char *cleaned_message;
        sg_ident author;
        sg_ident committer;
        const char *bad = NULL;

        char *merge_name;

        if (sg_ident_author(&author, &bad) != 0 || sg_ident_committer(&committer, &bad) != 0) {
            fprintf(stderr, "sg: invalid date format: %s\n", bad);
            rc = 1;
            goto done;
        }

        merge_name = build_merge_name(git_dir, branch_arg);
        if (merge_name == NULL) {
            fprintf(stderr, "sg: out of memory\n");
            rc = 1;
            goto done;
        }
        /* Phase 65: heap, not a fixed 512-byte buffer -- MEASURED against
           git 2.55.0 producing a genuinely different commit id, not just a
           short message: two 250-char branch names gave git a 523-byte
           message and sg a silently-truncated 513-byte one. A different
           message is a different object id, the most severe of the three
           bugs this phase exists to fix. */
        if (merge_msg_names_target(ours_label))
            message = sg_strfmt_alloc("Merge %s into %s\n", merge_name, ours_label);
        else
            message = sg_strfmt_alloc("Merge %s\n", merge_name);
        free(merge_name);
        if (message == NULL) {
            fprintf(stderr, "sg: out of memory\n");
            rc = 1;
            goto done;
        }
        if (sg_message_cleanup(message, &cleaned_message) != 0) {
            fprintf(stderr, "sg: out of memory\n");
            free(message);
            rc = 1;
            goto done;
        }
        free(message);

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
        commit.author_name = author.name;
        commit.author_email = author.email;
        commit.author_time = author.when;
        strcpy(commit.author_tz, author.tz);
        commit.committer_name = committer.name;
        commit.committer_email = committer.email;
        commit.committer_time = committer.when;
        strcpy(commit.committer_tz, committer.tz);
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
            /* Phase 65: heap. sg's own wording deliberately diverges from
               git's exact text (see above), but a long branch_arg silently
               truncating the line is still a bug -- the fixed 400-byte
               buffer is converted for the same reason as every other site
               in this phase, even though there is no real-git byte
               comparison to pin the result against (interop pins sg's own
               expected literal instead, same convention this line's own
               comment already documents). */
            char *reflog_msg = sg_strfmt_alloc(
                    "merge %s: Merge made by the 'sg-3way' strategy.", branch_arg);

            if (reflog_msg == NULL) {
                fprintf(stderr, "sg: out of memory\n");
                rc = 1;
                goto done;
            }
            if (sg_ref_move_head(git_dir, current_branch, new_commit_id, reflog_msg) != 0) {
                if (!sg_ref_lock_err_report_ex(stderr, "HEAD", "HEAD", NULL))
                    fprintf(stderr, "sg: failed to update HEAD\n");
                free(reflog_msg);
                rc = 1;
                goto done;
            }
            free(reflog_msg);
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

        /* Same reasoning as the rebase gate directly above, for the third
           subsystem that advances the branch/HEAD commit-by-commit: a
           stopped cherry-pick/revert has its own conflict resolution work
           in progress, and starting a merge on top of it would trample it. */
        {
            sg_seq_kind seq_kind = sg_sequencer_kind_in_progress(git_dir);

            if (seq_kind != 0) {
                const char *op = seq_kind == SG_SEQ_CHERRY_PICK ? "cherry-pick" : "revert";

                fprintf(stderr,
                       "sg: a %s is currently in progress\n"
                       "Finish it first (sg %s --continue) or run sg %s --abort to give up\n",
                       op, op, op);
                free(git_dir);
                free(repo_root);
                return 1;
            }
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

        /* Any revision sg_rev_parse_commit understands, not just a bare
           branch name (Phase 43). The old code called sg_ref_branch_exists
           directly, which made `sg merge v1` / `refs/heads/topic` /
           `topic~0` fail with "invalid reference" while the equivalent `git
           merge` succeeded -- and contradicted this project's own rule that
           a user-supplied revision always goes through sg_rev_parse_commit.
           That function peels annotated tags, which is what merge wants:
           measured, `git merge <annotated-tag>` merges the tagged COMMIT.

           Phase 78: this whole block, and the has_head/ORIG_HEAD write right
           after it, moved AHEAD of sg_require_clean_workdir below -- measured
           against git 2.55.0: git's own order is "rev parse -> write
           ORIG_HEAD -> dirty check", so a dirty work tree with an
           UNRESOLVABLE <rev> prints the REV error (not the dirty-workdir
           one), and a dirty work tree with a RESOLVABLE <rev> still gets
           ORIG_HEAD written even though the merge is then refused for being
           dirty. Both corners are pinned in interop's phase78 group. */
        {
            int prc = sg_rev_parse_commit(git_dir, branch_arg, theirs_commit);

            if (prc != 0) {
                if (prc == -4)
                    sg_cli_report_ambiguous_oid(git_dir, branch_arg, SG_REV_STRICT);
                fprintf(stderr, "sg: %s - not something we can merge\n", branch_arg);
                free(current_branch);
                free(git_dir);
                free(repo_root);
                return 1;
            }
        }

        has_head = (sg_ref_resolve_head(git_dir, ours_commit) == 0);

        /* HEAD unborn (has_head == 0): neither written nor deleted here --
           measured, this is the OPPOSITE of reset's unborn rule (see
           cmd_reset.c's reset_update_orig_head): `git merge` into a clean
           unborn branch succeeds (FF) and leaves a planted ORIG_HEAD
           untouched. Failure to write is FATAL for merge (unlike reset/
           rebase/stash), matching git: a foreign ORIG_HEAD.lock makes real
           git exit 128 and do nothing at all. */
        if (has_head && sg_cli_write_orig_head(git_dir, ours_commit) != 0) {
            free(current_branch);
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
