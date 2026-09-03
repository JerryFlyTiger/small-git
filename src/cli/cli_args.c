#include "sg/cli_args.h"

#include "sg/hash.h"
#include "sg/quote.h"
#include "sg/revparse.h"

#include <stdio.h>
#include <sys/stat.h>

void sg_cli_report_pathspec_error(sg_pathspec_error err, const char *arg, const char *repo_root)
{
    switch (err) {
    case SG_PATHSPEC_ERR_EMPTY:
        fprintf(stderr, "sg: an empty string is not a valid path; use . to match all paths\n");
        break;
    case SG_PATHSPEC_ERR_MAGIC:
        fprintf(stderr, "sg: unsupported pathspec magic: %s\n", sg_quote_path_delimited(arg));
        break;
    /* No `default:` on purpose -- see the header comment. */
    case SG_PATHSPEC_ERR_NONE:
    case SG_PATHSPEC_ERR_OUTSIDE:
        fprintf(stderr, "sg: %s is outside the repository %s\n",
               sg_quote_path_delimited(arg), sg_quote_path_delimited(repo_root));
        break;
    }
}

int sg_cli_arg_exists_in_worktree(const char *arg)
{
    struct stat st;

    if (sg_pathspec_looks_like_spec(arg))
        return 1;
    return lstat(arg, &st) == 0;
}

int sg_cli_split_revs_and_paths(const char *git_dir, char **pos, int n_pos, const char *cmd_name)
{
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    int rev_count = 0;
    int i;

    while (rev_count < n_pos) {
        const char *arg = pos[rev_count];
        int is_rev = sg_rev_parse_commit(git_dir, arg, commit_id) == 0;
        int is_path = sg_cli_arg_exists_in_worktree(arg);

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
        if (!sg_cli_arg_exists_in_worktree(pos[i])) {
            fprintf(stderr, "sg: %s: no such path in the working directory; "
                           "to name a path that does not exist, use sg %s -- <path>\n",
                   sg_quote_path_delimited(pos[i]), cmd_name);
            return -1;
        }
    }
    return rev_count;
}
