#include "sg/cli.h"

#include "sg/diff.h"
#include "sg/diff_out.h"
#include "sg/hash.h"
#include "sg/index.h"
#include "sg/objstore.h"
#include "sg/quote.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/revparse.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char USAGE[] =
    "usage: sg diff [--cached|--staged] [--stat[=<w>[,<n>]]|--numstat|--shortstat|--name-only|"
    "--name-status] [<rev> [<rev>]]\n";

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
    fprintf(stderr, "sg: 路徑 %s 無效，拒絕將這棵 tree 展開成檔案路徑\n",
           sg_quote_path_delimited(bad_path));
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
    char bad_path[SG_PATH_MAX];
    int rc;
    int exit_rc;

    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--cached") == 0 || strcmp(a, "--staged") == 0) {
            cached = 1;
        } else if (strcmp(a, "--stat") == 0) {
            opts.format = SG_DIFF_FORMAT_STAT;
            opts.stat_width = 0;
            opts.stat_name_width = 0;
        } else if (strncmp(a, "--stat=", 7) == 0) {
            opts.format = SG_DIFF_FORMAT_STAT;
            if (sg_diff_parse_stat_arg(a + 7, &opts.stat_width, &opts.stat_name_width) != 0) {
                fputs(USAGE, stderr);
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
        } else if (a[0] == '-' && a[1] != '\0') {
            fputs(USAGE, stderr);
            return 1;
        } else if (rev1 == NULL) {
            rev1 = a;
        } else if (rev2 == NULL) {
            rev2 = a;
        } else {
            fputs(USAGE, stderr);
            return 1;
        }
    }

    /* Measured against git 2.55.0: `git diff --cached <rev> <rev>` is
       flatly rejected (usage error), not silently treated as a tree-vs-tree
       diff with --cached ignored -- the ambiguity is real (--cached names
       the INDEX as one side, but two revs already name both sides). This
       project's exit codes are only 0/1 (not git's 129), per CLAUDE.md. */
    if (cached && rev2 != NULL) {
        fputs(USAGE, stderr);
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

    memset(&list, 0, sizeof(list));
    bad_path[0] = '\0';
    rc = 0;

    if (rev2 != NULL) {
        unsigned char old_tree[SG_SHA1_RAW_LEN];
        unsigned char new_tree[SG_SHA1_RAW_LEN];

        if (resolve_rev_tree(git_dir, rev1, old_tree) != 0 || resolve_rev_tree(git_dir, rev2, new_tree) != 0) {
            free(repo_root);
            free(git_dir);
            return 1;
        }
        rc = sg_diff_trees(git_dir, old_tree, new_tree, &list, bad_path);
    } else if (cached) {
        unsigned char tree_id[SG_SHA1_RAW_LEN];
        unsigned char *tree_ptr = NULL;
        sg_index idx;

        if (rev1 != NULL) {
            if (resolve_rev_tree(git_dir, rev1, tree_id) != 0) {
                free(repo_root);
                free(git_dir);
                return 1;
            }
            tree_ptr = tree_id;
        } else {
            int has_tree;

            if (resolve_head_tree(git_dir, tree_id, &has_tree) != 0) {
                free(repo_root);
                free(git_dir);
                return 1;
            }
            if (has_tree)
                tree_ptr = tree_id;
        }

        if (sg_index_read(git_dir, &idx) != 0) {
            fprintf(stderr, "sg: failed to read index (corrupt?)\n");
            free(repo_root);
            free(git_dir);
            return 1;
        }
        rc = sg_diff_tree_index(git_dir, tree_ptr, &idx, &list, bad_path);
        sg_index_free(&idx);
    } else if (rev1 != NULL) {
        unsigned char tree_id[SG_SHA1_RAW_LEN];
        sg_index idx;

        if (resolve_rev_tree(git_dir, rev1, tree_id) != 0) {
            free(repo_root);
            free(git_dir);
            return 1;
        }
        if (sg_index_read(git_dir, &idx) != 0) {
            fprintf(stderr, "sg: failed to read index (corrupt?)\n");
            free(repo_root);
            free(git_dir);
            return 1;
        }
        rc = sg_diff_tree_workdir(git_dir, repo_root, tree_id, &idx, &list, bad_path);
        sg_index_free(&idx);
    } else {
        sg_index idx;

        if (sg_index_read(git_dir, &idx) != 0) {
            fprintf(stderr, "sg: failed to read index (corrupt?)\n");
            free(repo_root);
            free(git_dir);
            return 1;
        }
        rc = sg_diff_index_workdir(git_dir, repo_root, &idx, &list);
        sg_index_free(&idx);
    }

    if (rc == -2) {
        report_bad_tree_path(bad_path);
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

    exit_rc = sg_diff_print(git_dir, repo_root, &list, &opts) != 0 ? 1 : 0;

    sg_diff_list_free(&list);
    free(repo_root);
    free(git_dir);
    return exit_rc;
}
