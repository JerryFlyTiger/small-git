#include "sg/cli.h"

#include "sg/diff.h"
#include "sg/diff_out.h"
#include "sg/hash.h"
#include "sg/index.h"
#include "sg/objstore.h"
#include "sg/pathspec.h"
#include "sg/quote.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/revparse.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char USAGE[] =
    "usage: sg diff [--cached|--staged] [--stat[=<w>[,<n>]]|--numstat|--shortstat|--name-only|"
    "--name-status] [-M[<n>]|--no-renames] [<rev> [<rev>]] [--] [<path>...]\n";

/* Resolves rev to a tree id via sg_rev_parse_commit + sg_commit_tree_of --
   the one path every rev argument in this command goes through, per
   CLAUDE.md's "use sg_rev_parse_commit, don't hand-roll rev resolution"
   rule. Prints its own error and returns -1 on failure. */
static int resolve_rev_tree(const char *git_dir, const char *rev, unsigned char tree_id_out[SG_SHA1_RAW_LEN])
{
    unsigned char commit_id[SG_SHA1_RAW_LEN];

    if (sg_rev_parse_commit(git_dir, rev, commit_id) != 0) {
        fprintf(stderr, "sg: invalid reference: %s\n", rev);
        return -1;
    }
    if (sg_commit_tree_of(git_dir, commit_id, tree_id_out) != 0) {
        fprintf(stderr, "sg: corrupt commit for '%s'\n", rev);
        return -1;
    }
    return 0;
}

/* --cached with no explicit rev diffs against HEAD's tree, or the empty tree
   when HEAD is unborn (sg_ref_resolve_head's -1 covers both "unborn" and "a
   corrupt HEAD that names neither a branch nor a commit" -- see refs.h;
   either way there is no parent commit to diff against, so NULL/the empty
   tree is the right answer, matching `git diff --cached` on a fresh repo). */
static int resolve_head_tree(const char *git_dir, unsigned char tree_id_out[SG_SHA1_RAW_LEN], int *has_tree)
{
    unsigned char commit_id[SG_SHA1_RAW_LEN];

    if (sg_ref_resolve_head(git_dir, commit_id) != 0) {
        *has_tree = 0;
        return 0;
    }
    if (sg_commit_tree_of(git_dir, commit_id, tree_id_out) != 0) {
        fprintf(stderr, "sg: corrupt commit for 'HEAD'\n");
        return -1;
    }
    *has_tree = 1;
    return 0;
}

static void report_bad_tree_path(const char *bad_path)
{
    fprintf(stderr, "sg: path %s is invalid, refusing to expand this tree into file paths\n",
           sg_quote_path_delimited(bad_path));
}

/* git's -M<n>/--find-renames=<n> threshold. The grammar lives in
   sg_similarity_parse_score because every one of its rules is
   counter-intuitive and belongs next to the scale it produces; all this adds
   is git's own rule that anything left over is an error rather than
   something to guess at. An argument that parses to 0 -- "-M0", "-M%" --
   means "use the default", exactly as it does in git. (A bare "-M" means the
   same thing but never arrives here; it is matched a few lines below.) */
static int parse_rename_score(const char *arg, int *out)
{
    const char *end = arg;
    int score = sg_similarity_parse_score(&end);

    if (*end != '\0')
        return -1;
    *out = score != 0 ? score : SG_SIMILARITY_DEFAULT;
    return 0;
}

static void report_pathspec_error(sg_pathspec_error err, const char *arg, const char *repo_root)
{
    switch (err) {
    case SG_PATHSPEC_ERR_EMPTY:
        fprintf(stderr, "sg: an empty string is not a valid path; use . to match all paths\n");
        break;
    case SG_PATHSPEC_ERR_MAGIC:
        fprintf(stderr, "sg: unsupported pathspec magic: %s\n", sg_quote_path_delimited(arg));
        break;
    /* No `default:` on purpose. NONE cannot reach here -- sg_pathspec_add
       fills err only when it fails -- but naming every value keeps the
       switch exhaustive, so -Wswitch complains if a future error code is
       added and nobody teaches this function to print it. A `default` would
       silently render it as "outside the repository". */
    case SG_PATHSPEC_ERR_NONE:
    case SG_PATHSPEC_ERR_OUTSIDE:
        fprintf(stderr, "sg: %s is outside the repository %s\n",
               sg_quote_path_delimited(arg), sg_quote_path_delimited(repo_root));
        break;
    }
}

/* Whether an argument names something that exists in the working tree, which
   is what decides a bare (no "--") argument's fate. A wildcard argument is
   accepted without asking the filesystem -- measured against git 2.55.0,
   `git diff '*.zzz'` matching nothing exits 0 while `git diff nosuch` is a
   hard error. lstat, not stat: a dangling symlink is still a path the user
   named. */
static int arg_exists_in_worktree(const char *arg)
{
    struct stat st;

    if (sg_pathspec_looks_like_spec(arg))
        return 1;
    return lstat(arg, &st) == 0;
}

/* Splits the positional arguments into revisions and pathspecs the way git
   does when no "--" was given, and returns how many leading arguments are
   revisions (-1 after printing an error).

   The rules, measured against git 2.55.0:
     - an argument that is both a valid revision and an existing file is
       rejected outright rather than guessed at;
     - the first argument that is a path ends the revision list, and from
       there on EVERY remaining argument must exist -- `git diff a.txt HEAD`
       fails naming HEAD, even though HEAD is a perfectly good revision;
     - an argument that is neither is the "ambiguous argument" error, which
       is what `git diff nosuch` prints.
   Each message names the offending argument and points at "--", because the
   whole point of these errors is that "--" is the way to say what you meant. */
static int split_revs_and_paths(const char *git_dir, char **pos, int n_pos)
{
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    int rev_count = 0;
    int i;

    while (rev_count < n_pos) {
        const char *arg = pos[rev_count];
        int is_rev = sg_rev_parse_commit(git_dir, arg, commit_id) == 0;
        int is_path = arg_exists_in_worktree(arg);

        if (is_rev && is_path) {
            fprintf(stderr, "sg: ambiguous argument %s: could be both a revision and a file; use -- to separate revisions from paths\n",
                   sg_quote_path_delimited(arg));
            return -1;
        }
        if (is_rev) {
            rev_count++;
            continue;
        }
        if (is_path)
            break;
        fprintf(stderr, "sg: ambiguous argument %s: not a revision, and no such path in the working directory; "
                       "use -- to separate revisions from paths\n",
               sg_quote_path_delimited(arg));
        return -1;
    }

    for (i = rev_count; i < n_pos; i++) {
        if (!arg_exists_in_worktree(pos[i])) {
            fprintf(stderr, "sg: %s: no such path in the working directory; "
                           "to name a path that does not exist, use sg diff -- <path>\n",
                   sg_quote_path_delimited(pos[i]));
            return -1;
        }
    }
    return rev_count;
}

int sg_cmd_diff(int argc, char **argv)
{
    sg_diff_out_opts opts;
    int cached = 0;
    const char *rev1 = NULL;
    const char *rev2 = NULL;
    int i;
    char *git_dir;
    char *repo_root;
    sg_diff_list list;
    sg_pathspec pathspec;
    char **pos;
    int n_pos = 0;
    int dashdash = -1; /* index into pos[] where "--" split the line */
    /* git's -M default, 50%, on git's 0..SG_SIMILARITY_MAX scale rather than
       as a percentage -- see sg/similarity.h for why a percentage cannot hold
       every threshold the -M grammar can ask for. 0 means --no-renames. */
    int rename_score = SG_SIMILARITY_DEFAULT;
    int rev_count;
    char bad_path[SG_PATH_MAX];
    int rc;
    int exit_rc;

    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    memset(&pathspec, 0, sizeof(pathspec));

    pos = malloc((size_t)(argc > 0 ? argc : 1) * sizeof(*pos));
    if (pos == NULL) {
        fprintf(stderr, "sg: out of memory\n");
        return 1;
    }

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        /* Past "--" nothing is an option any more: `sg diff -- --stat` asks
           about a file named "--stat", which is exactly why "--" exists. */
        if (dashdash >= 0) {
            pos[n_pos++] = argv[i];
            continue;
        }
        if (strcmp(a, "--") == 0) {
            dashdash = n_pos;
        } else if (strcmp(a, "--cached") == 0 || strcmp(a, "--staged") == 0) {
            cached = 1;
        } else if (strcmp(a, "--stat") == 0) {
            opts.format = SG_DIFF_FORMAT_STAT;
            opts.stat_width = 0;
            opts.stat_name_width = 0;
        } else if (strncmp(a, "--stat=", 7) == 0) {
            opts.format = SG_DIFF_FORMAT_STAT;
            if (sg_diff_parse_stat_arg(a + 7, &opts.stat_width, &opts.stat_name_width) != 0) {
                fputs(USAGE, stderr);
                free(pos);
                return 1;
            }
        } else if (strcmp(a, "--numstat") == 0) {
            opts.format = SG_DIFF_FORMAT_NUMSTAT;
        } else if (strcmp(a, "--shortstat") == 0) {
            opts.format = SG_DIFF_FORMAT_SHORTSTAT;
        } else if (strcmp(a, "--name-only") == 0) {
            opts.format = SG_DIFF_FORMAT_NAME_ONLY;
        } else if (strcmp(a, "--name-status") == 0) {
            opts.format = SG_DIFF_FORMAT_NAME_STATUS;
        } else if (strcmp(a, "--no-renames") == 0) {
            rename_score = 0;
        } else if (strcmp(a, "-M") == 0 || strcmp(a, "--find-renames") == 0) {
            rename_score = SG_SIMILARITY_DEFAULT;
        } else if (strncmp(a, "-M", 2) == 0 || strncmp(a, "--find-renames=", 15) == 0) {
            const char *v = a[1] == 'M' ? a + 2 : a + 15;

            if (parse_rename_score(v, &rename_score) != 0) {
                fputs(USAGE, stderr);
                free(pos);
                return 1;
            }
        } else if (a[0] == '-' && a[1] != '\0') {
            fputs(USAGE, stderr);
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

    memset(&list, 0, sizeof(list));
    bad_path[0] = '\0';
    rc = 0;
    exit_rc = 1;

    /* An explicit "--" settles the split with no guessing at all -- that is
       the entire point of typing it -- so the arguments before it are taken
       as revisions without ever being stat'd. */
    rev_count = dashdash >= 0 ? dashdash : split_revs_and_paths(git_dir, pos, n_pos);
    if (rev_count < 0)
        goto done;
    if (rev_count > 2) {
        fputs(USAGE, stderr);
        goto done;
    }
    rev1 = rev_count > 0 ? pos[0] : NULL;
    rev2 = rev_count > 1 ? pos[1] : NULL;

    for (i = rev_count; i < n_pos; i++) {
        sg_pathspec_error perr;

        if (sg_pathspec_add(&pathspec, repo_root, pos[i], &perr) != 0) {
            report_pathspec_error(perr, pos[i], repo_root);
            goto done;
        }
    }

    /* Measured against git 2.55.0: `git diff --cached <rev> <rev>` is
       flatly rejected (usage error), not silently treated as a tree-vs-tree
       diff with --cached ignored -- the ambiguity is real (--cached names
       the INDEX as one side, but two revs already name both sides). This
       project's exit codes are only 0/1 (not git's 129), per CLAUDE.md. */
    if (cached && rev2 != NULL) {
        fputs(USAGE, stderr);
        goto done;
    }

    if (rev2 != NULL) {
        unsigned char old_tree[SG_SHA1_RAW_LEN];
        unsigned char new_tree[SG_SHA1_RAW_LEN];

        if (resolve_rev_tree(git_dir, rev1, old_tree) != 0 || resolve_rev_tree(git_dir, rev2, new_tree) != 0)
            goto done;
        rc = sg_diff_trees(git_dir, old_tree, new_tree, &list, bad_path);
    } else if (cached) {
        unsigned char tree_id[SG_SHA1_RAW_LEN];
        unsigned char *tree_ptr = NULL;
        sg_index idx;

        if (rev1 != NULL) {
            if (resolve_rev_tree(git_dir, rev1, tree_id) != 0)
                goto done;
            tree_ptr = tree_id;
        } else {
            int has_tree;

            if (resolve_head_tree(git_dir, tree_id, &has_tree) != 0)
                goto done;
            if (has_tree)
                tree_ptr = tree_id;
        }

        if (sg_index_read(git_dir, &idx) != 0) {
            fprintf(stderr, "sg: failed to read index (corrupt?)\n");
            goto done;
        }
        rc = sg_diff_tree_index(git_dir, tree_ptr, &idx, &list, bad_path);
        sg_index_free(&idx);
    } else if (rev1 != NULL) {
        unsigned char tree_id[SG_SHA1_RAW_LEN];
        sg_index idx;

        if (resolve_rev_tree(git_dir, rev1, tree_id) != 0)
            goto done;
        if (sg_index_read(git_dir, &idx) != 0) {
            fprintf(stderr, "sg: failed to read index (corrupt?)\n");
            goto done;
        }
        rc = sg_diff_tree_workdir(git_dir, repo_root, tree_id, &idx, &list, bad_path);
        sg_index_free(&idx);
    } else {
        sg_index idx;

        if (sg_index_read(git_dir, &idx) != 0) {
            fprintf(stderr, "sg: failed to read index (corrupt?)\n");
            goto done;
        }
        rc = sg_diff_index_workdir(git_dir, repo_root, &idx, &list);
        sg_index_free(&idx);
    }

    if (rc == -2) {
        report_bad_tree_path(bad_path);
        goto done;
    }
    if (rc != 0) {
        fprintf(stderr, "sg: failed to compute diff\n");
        goto done;
    }

    /* Filtering here, on the finished list, keeps the pathspec out of all
       four builders -- see sg_diff_list_filter in include/sg/diff.h for why
       that is deliberate and what it costs. */
    sg_diff_list_filter(&list, &pathspec);

    /* Rename detection runs AFTER the filter, never before -- measured
       against git 2.55.0, `git diff --cached --name-status -- b1.txt` on a
       renamed pair prints "A b1.txt", because git filters by pathspec first
       and a spec naming half a rename leaves nothing to pair with. */
    if (sg_diff_detect_renames(git_dir, repo_root, &list, rename_score) != 0) {
        fprintf(stderr, "sg: out of memory, cannot detect renames\n");
        goto done;
    }

    exit_rc = sg_diff_print(git_dir, repo_root, &list, &opts) != 0 ? 1 : 0;

done:
    sg_diff_list_free(&list);
    sg_pathspec_free(&pathspec);
    free(pos);
    free(repo_root);
    free(git_dir);
    return exit_rc;
}
