#include "sg/cli.h"

#include "sg/apply.h"
#include "sg/diff.h"
#include "sg/diff_out.h"
#include "sg/hash.h"
#include "sg/index.h"
#include "sg/merge.h"
#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/quote.h"
#include "sg/rebase.h"
#include "sg/pathspec.h"
#include "sg/repo.h"
#include "sg/stash.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Writes the "Dropped ..." line for pop and drop. Real git echoes the user's
   spec back only when it already reads as stash@{N}; a bare "0" or no
   argument at all resolves to the fully-qualified ref instead. Measured on
   2.55.0:

     git stash drop            -> Dropped refs/stash@{0} (...)
     git stash drop stash@{0}  -> Dropped stash@{0} (...)
     git stash drop 0          -> Dropped refs/stash@{0} (...)
     git stash pop             -> Dropped refs/stash@{0} (...)
     git stash pop stash@{0}   -> Dropped stash@{0} (...)

   So this is not a pop-versus-drop distinction, which is how it first looked
   when only the no-argument pop and the explicit-spec drop had been sampled.
   Both subcommands share the one rule below. */
static void print_dropped(const char *spec, size_t index, const char *hex)
{
    if (spec != NULL && strncmp(spec, "stash@{", 7) == 0)
        printf("Dropped %s (%s)\n", spec, hex);
    else
        printf("Dropped refs/stash@{%zu} (%s)\n", index, hex);
}

/* Phase 37: `sg stash push -- <pathspec>...` -- a third, deliberate copy of
   this function (cmd_diff.c, cmd_status.c already each have one). CLAUDE.md
   names the precedent (report_bad_tree_path/report_bad_stash_tree_path,
   Phase 25): converge once interop covers all three, not before. */
static void report_pathspec_error(sg_pathspec_error err, const char *arg, const char *repo_root)
{
    switch (err) {
    case SG_PATHSPEC_ERR_EMPTY:
        fprintf(stderr, "sg: an empty string is not a valid path; use . to match all paths\n");
        break;
    case SG_PATHSPEC_ERR_MAGIC:
        fprintf(stderr, "sg: unsupported pathspec magic: %s\n", sg_quote_path_delimited(arg));
        break;
    case SG_PATHSPEC_ERR_NONE:
    case SG_PATHSPEC_ERR_OUTSIDE:
        fprintf(stderr, "sg: %s is outside the repository %s\n",
               sg_quote_path_delimited(arg), sg_quote_path_delimited(repo_root));
        break;
    }
}

static int cmd_stash_push(int argc, char **argv, const char *usage)
{
    sg_stash_push_opts opts;
    sg_pathspec pathspec;
    int i0 = 1;
    int i;
    char *git_dir;
    char *repo_root;
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    char bad_path[SG_PATH_MAX];
    int rc;
    char **pos;
    int n_pos = 0;
    int dashdash = -1; /* index into pos[] where "--" split the line */

    memset(&opts, 0, sizeof(opts));
    memset(&pathspec, 0, sizeof(pathspec));
    bad_path[0] = '\0';

    if (argc >= 2 && strcmp(argv[1], "push") == 0)
        i0 = 2;

    pos = malloc((size_t)(argc > 0 ? argc : 1) * sizeof(*pos));
    if (pos == NULL) {
        fprintf(stderr, "sg: out of memory\n");
        return 1;
    }

    for (i = i0; i < argc; i++) {
        /* Past "--" nothing is an option any more, same convention as
           cmd_diff.c/cmd_status.c. */
        if (dashdash >= 0) {
            pos[n_pos++] = argv[i];
            continue;
        }
        if (strcmp(argv[i], "--") == 0) {
            dashdash = n_pos;
        } else if (strcmp(argv[i], "-m") == 0) {
            if (i + 1 >= argc) {
                fputs(usage, stderr);
                free(pos);
                return 1;
            }
            opts.message = argv[++i];
        } else if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--include-untracked") == 0) {
            opts.include_untracked = 1;
        } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--all") == 0) {
            opts.include_ignored = 1;
        } else if (strcmp(argv[i], "--keep-index") == 0) {
            opts.keep_index = 1;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fputs(usage, stderr);
            free(pos);
            return 1;
        } else {
            pos[n_pos++] = argv[i];
        }
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL) {
        free(pos);
        return 1;
    }
    repo_root = sg_repo_root(git_dir);
    if (repo_root == NULL) {
        fprintf(stderr, "sg: failed to determine repository root\n");
        free(pos);
        free(git_dir);
        return 1;
    }

    for (i = 0; i < n_pos; i++) {
        sg_pathspec_error perr;

        if (sg_pathspec_add(&pathspec, repo_root, pos[i], &perr) != 0) {
            report_pathspec_error(perr, pos[i], repo_root);
            sg_pathspec_free(&pathspec);
            free(pos);
            free(repo_root);
            free(git_dir);
            return 1;
        }
    }
    free(pos);
    opts.pathspec = &pathspec;

    /* sg_stash_push refuses an unmerged index, but it is a library function
       and reports that as a bare -1 alongside every other failure. Ask the
       question here so the answer can be stated instead of guessed: a user
       sitting in a conflicted merge should be told what is wrong, not handed
       the fallback below with a question mark on the end. */
    {
        sg_index idx;

        if (sg_index_read(git_dir, &idx) == 0) {
            int unmerged = sg_index_has_unmerged(&idx);

            sg_index_free(&idx);
            if (unmerged) {
                fprintf(stderr, "sg: unresolved conflicts remain, cannot stash push\n");
                sg_pathspec_free(&pathspec);
                free(git_dir);
                free(repo_root);
                return 1;
            }
        }
    }

    /* Real git behaves identically here (measured against git 2.55.0): a
       stash push while a rebase is paused resets the working tree and index
       back to HEAD, which can make `sg rebase --continue` decide the paused
       commit's change is already upstream and silently skip it. sg does not
       change that behavior -- it matches git -- but it does not warn about
       it either, so say so. The rebase sequencer state itself is untouched
       by this (see sg_stash_push's header comment). */
    if (sg_rebase_state_exists(git_dir)) {
        fprintf(stderr, "sg: a rebase is currently in progress; this stash push will reset the working directory and "
                        "index back to HEAD, after which `sg rebase --continue` may skip the current commit as a result\n");
    }

    rc = sg_stash_push(git_dir, repo_root, &opts, commit_id, bad_path);
    if (rc == 1) {
        printf("No local changes to save\n");
        sg_pathspec_free(&pathspec);
        free(git_dir);
        free(repo_root);
        return 0;
    }
    if (rc == 2) {
        /* Phase 37: opts.pathspec matched nothing at all -- nothing durable
           was written, unlike -2 below. Real git's wording is "pathspec ...
           did not match any file(s) known to git"; sg's own wording is
           enough since this is a brand-new rejection with no oracle
           byte-format to match (only the exit code + "nothing created"
           behavior is pinned against real git, per PHASE37_SPEC.md B1). */
        fprintf(stderr, "sg: pathspec did not match any files; no stash created\n");
        sg_pathspec_free(&pathspec);
        free(git_dir);
        free(repo_root);
        return 1;
    }
    if (rc == -2) {
        /* The stash commit + refs/stash were already written durably (it IS
           on `sg stash list`) -- only the snapshot or the working-tree reset
           back to HEAD failed after that. Saying "cannot create stash" here would
           be a lie: the entry exists, only its cleanup step is in doubt. */
        sg_stash_list list;

        fprintf(stderr, "sg: stash was created, but a later step failed (snapshot, restoring the working directory to "
                        "HEAD, or (under -u/-a) removing untracked files now in the stash / pruning empty "
                        "directories); please check the working directory state yourself\n");
        if (sg_stash_list_read(git_dir, &list) == 0 && list.count > 0) {
            fprintf(stderr, "sg: that stash is stash@{0}: %s\n", list.entries[0].message);
            sg_stash_list_free(&list);
        }
        sg_pathspec_free(&pathspec);
        free(git_dir);
        free(repo_root);
        return 1;
    }
    if (rc != 0) {
        /* Phase 36 follow-up: bad_path is set specifically when the working
           tree's own sg_tree_build_from_workdir call refused a hostile index
           path (e.g. "../secret.txt") -- the generic "unborn HEAD, or ...
           unresolved conflicts" guess below was previously printed for this
           case too, discarding the actual reason (measured: HEAD was not
           unborn and the index had no real conflict). */
        if (bad_path[0] != '\0')
            fprintf(stderr, "sg: cannot create stash: the index names an invalid path (%s)\n",
                   sg_quote_path_delimited(bad_path));
        else
            fprintf(stderr,
                   "sg: cannot create stash (unborn HEAD, or the index has unresolved conflicts?)\n");
        sg_pathspec_free(&pathspec);
        free(git_dir);
        free(repo_root);
        return 1;
    }

    /* MERGE_HEAD cleanup is deliberately the CLI layer's job -- see the
       sg_stash_push header comment in sg/stash.h. */
    {
        if (sg_merge_head_exists(git_dir)) {
            if (sg_merge_head_remove(git_dir) != 0)
                fprintf(stderr, "sg: warning: stash succeeded, but failed to remove MERGE_HEAD\n");
            else
                fprintf(stderr, "sg: cleared the in-progress merge state (MERGE_HEAD)\n");
        }
    }

    {
        sg_stash_list list;

        if (sg_stash_list_read(git_dir, &list) == 0 && list.count > 0) {
            printf("Saved working directory and index state %s\n", list.entries[0].message);
            sg_stash_list_free(&list);
        } else {
            char hex[SG_SHA1_HEX_LEN + 1];

            sg_sha1_to_hex(commit_id, hex);
            printf("Saved working directory and index state %s\n", hex);
        }
    }

    sg_pathspec_free(&pathspec);
    free(git_dir);
    free(repo_root);
    return 0;
}

static int cmd_stash_list(int argc, char **argv)
{
    char *git_dir;
    sg_stash_list list;
    size_t i;

    (void)argv;
    if (argc != 2) {
        fputs("usage: sg stash list\n", stderr);
        return 1;
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;

    if (sg_stash_list_read(git_dir, &list) != 0) {
        fprintf(stderr, "sg: failed to read stash list (corrupt reflog?)\n");
        free(git_dir);
        return 1;
    }

    for (i = 0; i < list.count; i++)
        printf("stash@{%zu}: %s\n", i, list.entries[i].message);

    sg_stash_list_free(&list);
    free(git_dir);
    return 0;
}

static int cmd_stash_clear(int argc, char **argv)
{
    char *git_dir;

    (void)argv;
    if (argc != 2) {
        fputs("usage: sg stash clear\n", stderr);
        return 1;
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;

    if (sg_stash_clear(git_dir) != 0) {
        fprintf(stderr, "sg: failed to clear stash\n");
        free(git_dir);
        return 1;
    }

    free(git_dir);
    return 0;
}

/* Which half(s) of a stash's changes `sg stash show` compares against
   base_tree. Measured against real git 2.55.0 in a scratch repo (three
   separate findings, kept together since the first two are easy to
   misread as two independent boolean flags):

     1. `-u` and `--only-untracked` are NOT independent toggles that
        combine -- they are one mode selector, and whichever one is named
        LAST on the command line decides. Measured by running the same
        3-parent stash (tracked change to f.txt + untracked u.txt) through
        both orderings:
          `git stash show -u --only-untracked`  -> only "u.txt" (only-untracked wins)
          `git stash show --only-untracked -u`  -> both "f.txt" and "u.txt" (-u wins)
        If they were independent flags both orderings would print the same
        thing; they don't, so this file tracks a single last-write-wins
        enum instead of two separate booleans.
     2. `--only-untracked` on a stash with NO untracked parent (a plain
        2-parent `stash push`, no `-u`/`-a`): prints nothing and exits 0 --
        not an error, not a fallback to the tracked diff.
     3. `-u` on a stash with no untracked parent: falls back to exactly the
        tracked-only diff (identical to no flag at all), also exits 0. */
typedef enum {
    SHOW_UNTRACKED_NONE = 0,   /* default: base_tree vs theirs_tree only */
    SHOW_UNTRACKED_INCLUDE,    /* -u/--include-untracked: tracked + untracked */
    SHOW_UNTRACKED_ONLY        /* --only-untracked: untracked half only */
} show_untracked_mode;

/* Merges a (tracked half: base_tree vs theirs_tree) and b (untracked half:
   the EMPTY tree vs untracked_tree, not base_tree vs untracked_tree -- see
   the call site's comment for why: every path in b is therefore an
   addition) into one path-sorted list,
   transferring ownership of every entry from a/b into *out. sg_diff_print
   assumes its input is already sorted by path (diff_out.h); real git's own
   `stash show -u` output interleaves the two halves by name rather than
   printing the tracked block followed by the untracked block, so a plain
   concatenation would not match it (measured).

   Two sg_diff_trees calls plus this merge, rather than building one merged
   tree object first (via sg_tree_build) and diffing that against base_tree
   once, was chosen so that `sg stash show` never writes anything to the
   object store just to render a diff.

   On success, a and b are left zeroed (their arrays freed here, without
   touching the path strings, which now belong to *out); a caller must still
   NOT call sg_diff_list_free on them afterward. On the (out-of-memory)
   failure path, a and b are left untouched and *out is zeroed -- the caller
   is responsible for freeing a and b itself.

   INVARIANT this merge relies on and does NOT defend against: a and b never
   share a path. b only ever comes from diffing the empty tree against a
   stash's untracked_tree (parents[2]), and sg_stash_push builds that tree
   solely from sg_status_list_untracked's output -- paths the INDEX does not
   already have (see sg_tree_build_from_untracked's header comment) -- so a
   path a (built from base_tree/theirs_tree, both real trees the index and
   HEAD agree are tracked) reports can never also be a path b reports. The
   `<= 0` tie-break below exists only to make the merge deterministic if
   that invariant is ever violated (e.g. by a future caller constructing b
   from something other than an untracked tree) -- it does NOT make a
   same-path collision correct: sg_diff_print would receive two rows for
   that one path and silently print it twice, the same class of bug as the
   base_tree-vs-untracked_tree "phantom deletion" this file's git history
   already fixed once. */
static int merge_diff_lists(sg_diff_list *a, sg_diff_list *b, sg_diff_list *out)
{
    size_t ia = 0, ib = 0, n = 0;
    sg_diff_entry *merged;

    memset(out, 0, sizeof(*out));
    if (a->count + b->count == 0)
        return 0;

    merged = malloc((a->count + b->count) * sizeof(*merged));
    if (merged == NULL)
        return -1;

    while (ia < a->count && ib < b->count) {
        if (strcmp(a->entries[ia].path, b->entries[ib].path) <= 0)
            merged[n++] = a->entries[ia++];
        else
            merged[n++] = b->entries[ib++];
    }
    while (ia < a->count)
        merged[n++] = a->entries[ia++];
    while (ib < b->count)
        merged[n++] = b->entries[ib++];

    out->entries = merged;
    out->count = n;
    out->cap = n;

    free(a->entries);
    a->entries = NULL;
    a->count = 0;
    a->cap = 0;
    free(b->entries);
    b->entries = NULL;
    b->count = 0;
    b->cap = 0;

    return 0;
}

static void report_bad_stash_tree_path(const char *bad_path)
{
    fprintf(stderr, "sg: path %s is invalid, refusing to expand this tree into file paths\n",
           sg_quote_path_delimited(bad_path));
}

static int cmd_stash_show(int argc, char **argv)
{
    static const char usage[] =
        "usage: sg stash show [-p|--patch] [--stat[=<w>[,<n>]]] [--numstat] [--shortstat] "
        "[--name-only]\n"
        "                      [--name-status] [-M[<n>]|--find-renames[=<n>]|--no-renames]\n"
        "                      [--histogram] [-u|--include-untracked] [--only-untracked] "
        "[<stash>]\n";
    sg_diff_out_opts opts;
    const char *spec = NULL;
    show_untracked_mode umode = SHOW_UNTRACKED_NONE;
    int i;
    char *git_dir;
    char *repo_root;
    size_t index;
    sg_stash_list list;
    sg_stash_trees trees;
    sg_diff_list diff_list;
    char bad_path[SG_PATH_MAX];
    int rc;
    int exit_rc;
    /* Same scale and same default as `sg diff -M` -- see sg/similarity.h. */
    int rename_score = SG_SIMILARITY_DEFAULT;
    /* git's implied -p (Phase 44), measured against git 2.55.0: `git stash
       show` defaults to a diffstat, but any DIFF option that is not itself a
       format selector switches it to a patch -- -M, -C, --no-renames,
       --histogram, --patience and -U<n> all do. The stash-specific flags do
       NOT (-u / --include-untracked / --only-untracked stay on the stat, and
       -u -M still switches, so -u neither implies nor suppresses), and an
       explicit format always wins REGARDLESS OF ORDER (`-M --stat` and
       `--stat -M` both print a stat). That last part is why these are two
       independent flags resolved after the loop instead of assignments
       inside it: an in-loop "last one wins" would make the order matter. */
    int format_given = 0;
    int diff_opt_given = 0;

    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_STAT; /* the default -- measured, `git stash show` prints a diffstat */

    for (i = 2; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "-p") == 0 || strcmp(a, "--patch") == 0) {
            opts.format = SG_DIFF_FORMAT_PATCH;
            format_given = 1;
        } else if (strcmp(a, "--stat") == 0) {
            opts.format = SG_DIFF_FORMAT_STAT;
            opts.stat_width = 0;
            opts.stat_name_width = 0;
            format_given = 1;
        } else if (strncmp(a, "--stat=", 7) == 0) {
            opts.format = SG_DIFF_FORMAT_STAT;
            format_given = 1;
            if (sg_diff_parse_stat_arg(a + 7, &opts.stat_width, &opts.stat_name_width) != 0) {
                fputs(usage, stderr);
                return 1;
            }
        } else if (strcmp(a, "--numstat") == 0) {
            opts.format = SG_DIFF_FORMAT_NUMSTAT;
            format_given = 1;
        } else if (strcmp(a, "--shortstat") == 0) {
            opts.format = SG_DIFF_FORMAT_SHORTSTAT;
            format_given = 1;
        } else if (strcmp(a, "--name-only") == 0) {
            opts.format = SG_DIFF_FORMAT_NAME_ONLY;
            format_given = 1;
        } else if (strcmp(a, "--name-status") == 0) {
            opts.format = SG_DIFF_FORMAT_NAME_STATUS;
            format_given = 1;
        } else if (strcmp(a, "--no-renames") == 0) {
            rename_score = 0;
            diff_opt_given = 1;
        } else if (strcmp(a, "--histogram") == 0) {
            opts.algorithm = SG_DIFF_ALGO_HISTOGRAM;
            diff_opt_given = 1;
        } else if (strcmp(a, "-M") == 0 || strcmp(a, "--find-renames") == 0) {
            rename_score = SG_SIMILARITY_DEFAULT;
            diff_opt_given = 1;
        } else if (strncmp(a, "-M", 2) == 0 || strncmp(a, "--find-renames=", 15) == 0) {
            const char *v = a[1] == 'M' ? a + 2 : a + 15;

            if (sg_similarity_parse_score_arg(v, &rename_score) != 0) {
                fputs(usage, stderr);
                return 1;
            }
            diff_opt_given = 1;
        } else if (strcmp(a, "-u") == 0 || strcmp(a, "--include-untracked") == 0) {
            umode = SHOW_UNTRACKED_INCLUDE;
        } else if (strcmp(a, "--only-untracked") == 0) {
            umode = SHOW_UNTRACKED_ONLY;
        } else if (a[0] == '-' && a[1] != '\0') {
            fputs(usage, stderr);
            return 1;
        } else if (spec == NULL) {
            spec = a;
        } else {
            fputs(usage, stderr);
            return 1;
        }
    }

    /* Resolved after the loop, not inside it -- see the comment on
       format_given for why the order must not matter. */
    if (!format_given && diff_opt_given)
        opts.format = SG_DIFF_FORMAT_PATCH;

    if (sg_stash_parse_spec(spec, &index) != 0) {
        fprintf(stderr, "sg: invalid stash spec: %s\n", spec != NULL ? spec : "");
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

    if (sg_stash_list_read(git_dir, &list) != 0) {
        fprintf(stderr, "sg: failed to read stash list\n");
        free(git_dir);
        free(repo_root);
        return 1;
    }
    if (index >= list.count) {
        fprintf(stderr, "sg: %s: log for 'stash' only has %zu entries\n",
               spec != NULL ? spec : "stash@{0}", list.count);
        sg_stash_list_free(&list);
        free(git_dir);
        free(repo_root);
        return 1;
    }
    sg_stash_list_free(&list);

    if (sg_stash_load_trees(git_dir, index, &trees) != 0) {
        fprintf(stderr, "sg: stash@{%zu} is corrupt (not a valid stash commit)\n", index);
        free(git_dir);
        free(repo_root);
        return 1;
    }

    memset(&diff_list, 0, sizeof(diff_list));
    bad_path[0] = '\0';
    rc = 0;

    if (umode == SHOW_UNTRACKED_ONLY) {
        /* No untracked parent: real git prints nothing and exits 0 rather
           than erroring (measured) -- passing NULL for both sides yields
           exactly that, an empty diff list. */
        const unsigned char *new_tree = trees.has_untracked ? trees.untracked_tree : NULL;

        rc = sg_diff_trees(git_dir, NULL, new_tree, &diff_list, bad_path);
    } else if (umode == SHOW_UNTRACKED_INCLUDE && trees.has_untracked) {
        sg_diff_list tracked_list;
        sg_diff_list untracked_list;

        memset(&tracked_list, 0, sizeof(tracked_list));
        memset(&untracked_list, 0, sizeof(untracked_list));

        rc = sg_diff_trees(git_dir, trees.base_tree, trees.theirs_tree, &tracked_list, bad_path);
        if (rc == 0)
            /* NULL (empty tree), not trees.base_tree, on the old side here:
               diffing base_tree vs untracked_tree would also report every
               path base_tree has that untracked_tree lacks (i.e. every
               tracked path the untracked half never touches) as a spurious
               DELETION -- untracked_tree only ever holds untracked paths,
               so base_tree's own tracked paths are never in it by
               construction. Comparing against the empty tree instead is
               exactly what --only-untracked (below) already does, and
               yields only additions, one per untracked path -- caught by a
               real-repo repro (`sg stash show -u --name-only` printed
               "a.txt\na.txt\nb.txt\nc.txt\nc.txt" -- a.txt/c.txt doubled,
               once as the tracked_list's genuine modification and once as
               a phantom "deletion" from this call -- before this fix). */
            rc = sg_diff_trees(git_dir, NULL, trees.untracked_tree, &untracked_list, bad_path);
        if (rc == 0 && merge_diff_lists(&tracked_list, &untracked_list, &diff_list) != 0)
            rc = -1;
        if (rc != 0) {
            sg_diff_list_free(&tracked_list);
            sg_diff_list_free(&untracked_list);
        }
    } else {
        /* Default, and -u with no untracked parent (measured: falls back to
           the tracked-only diff rather than erroring, same as
           --only-untracked's fallback above). */
        rc = sg_diff_trees(git_dir, trees.base_tree, trees.theirs_tree, &diff_list, bad_path);
    }

    if (rc == -2) {
        report_bad_stash_tree_path(bad_path);
        free(repo_root);
        free(git_dir);
        return 1;
    }
    if (rc != 0) {
        fprintf(stderr, "sg: failed to compute diff\n");
        free(repo_root);
        free(git_dir);
        return 1;
    }

    /* One call, on the final list, AFTER -u's merge above has combined
       tracked and untracked into a single diff_list -- so renames are found
       across both halves in one pass rather than within each half.

       That is not a detail: measured against git 2.55.0, an untracked file
       whose content is byte-identical to a deleted tracked file STEALS the
       source through the exact pass, and the real inexact rename next to it
       is demoted to a plain addition. Detecting per-half, or before the
       merge, would quietly give a different answer. It needs no special case
       here, only this ordering -- it falls out of the pass order documented
       in sg/diff.h. */
    if (sg_diff_detect_renames(git_dir, repo_root, &diff_list, rename_score, 0) != 0) {
        fprintf(stderr, "sg: out of memory, cannot detect renames\n");
        sg_diff_list_free(&diff_list);
        free(repo_root);
        free(git_dir);
        return 1;
    }

    exit_rc = sg_diff_print(git_dir, repo_root, &diff_list, &opts) != 0 ? 1 : 0;

    sg_diff_list_free(&diff_list);
    free(repo_root);
    free(git_dir);
    return exit_rc;
}

static int cmd_stash_drop(int argc, char **argv)
{
    static const char usage[] = "usage: sg stash drop [<stash>]\n";
    const char *spec = NULL;
    char *git_dir;
    size_t index;
    sg_stash_list list;
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];

    if (argc == 3)
        spec = argv[2];
    else if (argc != 2) {
        fputs(usage, stderr);
        return 1;
    }

    if (sg_stash_parse_spec(spec, &index) != 0) {
        fprintf(stderr, "sg: invalid stash spec: %s\n", spec != NULL ? spec : "");
        return 1;
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;

    if (sg_stash_list_read(git_dir, &list) != 0) {
        fprintf(stderr, "sg: failed to read stash list\n");
        free(git_dir);
        return 1;
    }
    if (index >= list.count) {
        fprintf(stderr, "sg: %s: log for 'stash' only has %zu entries\n",
               spec != NULL ? spec : "stash@{0}", list.count);
        sg_stash_list_free(&list);
        free(git_dir);
        return 1;
    }
    memcpy(commit_id, list.entries[index].commit_id, SG_SHA1_RAW_LEN);
    sg_stash_list_free(&list);

    if (sg_stash_drop(git_dir, index) != 0) {
        fprintf(stderr, "sg: failed to drop stash@{%zu}\n", index);
        free(git_dir);
        return 1;
    }

    sg_sha1_to_hex(commit_id, hex);
    print_dropped(spec, index, hex);

    free(git_dir);
    return 0;
}

static int cmd_stash_apply_or_pop(int argc, char **argv, int is_pop)
{
    const char *cmd_name = is_pop ? "pop" : "apply";
    char usage[80];
    const char *spec = NULL;
    int restore_index = 0;
    int i;
    char *git_dir;
    char *repo_root;
    sg_index idx;
    size_t index;
    sg_stash_list list;
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    sg_obj_type type;
    unsigned char *content = NULL;
    size_t content_len;
    sg_commit commit;
    char **dirty_paths = NULL;
    size_t dirty_count = 0;
    int rc;

    snprintf(usage, sizeof(usage), "usage: sg stash %s [--index] [<stash>]\n", cmd_name);

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--index") == 0) {
            restore_index = 1;
        } else if (spec == NULL) {
            spec = argv[i];
        } else {
            fputs(usage, stderr);
            return 1;
        }
    }

    if (sg_stash_parse_spec(spec, &index) != 0) {
        fprintf(stderr, "sg: invalid stash spec: %s\n", spec != NULL ? spec : "");
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

    if (sg_index_read(git_dir, &idx) != 0) {
        fprintf(stderr, "sg: failed to read index (corrupt?)\n");
        free(git_dir);
        free(repo_root);
        return 1;
    }
    if (sg_index_has_unmerged(&idx)) {
        fprintf(stderr, "sg: unresolved conflicts remain, cannot stash %s\n", cmd_name);
        sg_index_free(&idx);
        free(git_dir);
        free(repo_root);
        return 1;
    }
    sg_index_free(&idx);

    /* Deliberate divergence from real git (which allows this): pop/apply
       during a paused rebase is refused, consistent with switch/merge, and
       leaves the sequencer state untouched. See CLAUDE.md's rebase-gate
       rule and sg/stash.h's sg_stash_apply header comment. */
    if (sg_rebase_state_exists(git_dir)) {
        fprintf(stderr,
               "sg: a rebase is currently in progress, cannot stash %s\n"
               "Finish it first (sg rebase --continue) or run sg rebase --abort to give up\n",
               cmd_name);
        free(git_dir);
        free(repo_root);
        return 1;
    }

    if (sg_stash_list_read(git_dir, &list) != 0) {
        fprintf(stderr, "sg: failed to read stash list\n");
        free(git_dir);
        free(repo_root);
        return 1;
    }
    if (index >= list.count) {
        fprintf(stderr, "sg: %s: log for 'stash' only has %zu entries\n",
               spec != NULL ? spec : "stash@{0}", list.count);
        sg_stash_list_free(&list);
        free(git_dir);
        free(repo_root);
        return 1;
    }
    memcpy(commit_id, list.entries[index].commit_id, SG_SHA1_RAW_LEN);
    sg_stash_list_free(&list);

    if (sg_object_read(git_dir, commit_id, &type, &content, &content_len) != 0 || type != SG_OBJ_COMMIT) {
        fprintf(stderr, "sg: stash@{%zu} is corrupt (not a valid commit)\n", index);
        free(content);
        free(git_dir);
        free(repo_root);
        return 1;
    }
    if (sg_commit_parse(content, content_len, &commit) != 0) {
        fprintf(stderr, "sg: stash@{%zu} is corrupt (cannot parse commit)\n", index);
        free(content);
        free(git_dir);
        free(repo_root);
        return 1;
    }
    free(content);
    if (commit.parent_count < 2 || commit.parent_count > 3) {
        fprintf(stderr, "sg: stash@{%zu}: not a stash-like commit\n", index);
        sg_commit_free(&commit);
        free(git_dir);
        free(repo_root);
        return 1;
    }
    sg_commit_free(&commit);

    /* Phase 20 sec 4: targeted dirty-workdir gate, replacing the old blanket
       sg_require_clean_workdir call -- only paths this stash's merge
       actually touches are checked (see sg_stash_apply_check_dirty's header
       comment for the exact rule, including the row-8 divergence from real
       git). */
    rc = sg_stash_apply_check_dirty(git_dir, repo_root, index, &dirty_paths, &dirty_count);
    if (rc != 0) {
        if (rc == 1) {
            size_t j;

            fprintf(stderr, "sg: local changes to the following paths would be overwritten by this stash %s; deal with them "
                            "first (commit them or stash them separately):\n",
                   cmd_name);
            for (j = 0; j < dirty_count; j++)
                fprintf(stderr, "\t%s\n", sg_quote_path(dirty_paths[j]));
            for (j = 0; j < dirty_count; j++)
                free(dirty_paths[j]);
            free(dirty_paths);
        } else {
            fprintf(stderr, "sg: cannot check the working directory status\n");
        }
        free(git_dir);
        free(repo_root);
        return 1;
    }

    rc = sg_stash_apply(git_dir, repo_root, index, restore_index);
    if (rc == 1) {
        fprintf(stderr, "Automatic merge failed, the working directory and index are left with conflict markers "
                        "(Updated upstream / Stashed changes):\n"
                        "Edit the conflicted files and run `sg add <file>...` to mark them resolved; "
                        "the stash itself was not dropped\n");
        if (restore_index)
            fprintf(stderr, "sg: because of the conflicts, the index was not restored to how it looked when the stash was "
                            "created (Index was not unstashed)\n");
        free(git_dir);
        free(repo_root);
        return 1;
    }
    if (rc != 0) {
        fprintf(stderr, "sg: stash %s failed\n", cmd_name);
        free(git_dir);
        free(repo_root);
        return 1;
    }

    /* Real git prints the working-tree status (identical to `git status`)
       after a clean apply or pop (measured `git stash apply`/`git stash
       pop` against git 2.55.0) -- sg previously printed nothing at all on a
       clean apply. Reuse sg_cmd_status wholesale rather than duplicating its
       formatting. */
    {
        char *status_argv[1];

        status_argv[0] = (char *)"status";
        sg_cmd_status(1, status_argv);
    }

    if (is_pop) {
        char hex[SG_SHA1_HEX_LEN + 1];

        sg_sha1_to_hex(commit_id, hex);
        if (sg_stash_drop(git_dir, index) != 0) {
            fprintf(stderr, "sg: apply succeeded, but failed to drop stash@{%zu}\n", index);
            free(git_dir);
            free(repo_root);
            return 1;
        }
        print_dropped(spec, index, hex);
    }

    free(git_dir);
    free(repo_root);
    return 0;
}

int sg_cmd_stash(int argc, char **argv)
{
    static const char usage[] = "usage: sg stash [push] [-m <msg>] [-u|--include-untracked] [-a|--all] "
                                "[--keep-index] [--] [<pathspec>...]\n"
                                "   or: sg stash list\n"
                                "   or: sg stash show [-p|--patch] [--stat[=<w>[,<n>]]] [--numstat] "
                                "[--shortstat] [--name-only]\n"
                                "                     [--name-status]\n"
                                "                     [-u|--include-untracked] [--only-untracked] "
                                "[<stash>]\n"
                                "   or: sg stash apply [--index] [<stash>]\n"
                                "   or: sg stash pop [--index] [<stash>]\n"
                                "   or: sg stash drop [<stash>]\n"
                                "   or: sg stash clear\n";

    if (argc >= 2 && strcmp(argv[1], "list") == 0)
        return cmd_stash_list(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "show") == 0)
        return cmd_stash_show(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "clear") == 0)
        return cmd_stash_clear(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "apply") == 0)
        return cmd_stash_apply_or_pop(argc, argv, 0);
    if (argc >= 2 && strcmp(argv[1], "pop") == 0)
        return cmd_stash_apply_or_pop(argc, argv, 1);
    if (argc >= 2 && strcmp(argv[1], "drop") == 0)
        return cmd_stash_drop(argc, argv);

    return cmd_stash_push(argc, argv, usage);
}
