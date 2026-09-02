#include "sg/cli.h"

#include "sg/pick.h"
#include "sg/repo.h"
#include "sg/revparse.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char usage[] = "usage: sg cherry-pick [-n|--no-commit] [-m <parent-number>] <commit>...\n"
                            "       sg cherry-pick (--continue | --skip | --abort | --quit)\n";

int sg_cmd_cherry_pick(int argc, char **argv)
{
    int continue_flag = 0, skip_flag = 0, abort_flag = 0, quit_flag = 0;
    sg_pick_opts opts;
    char **commit_args;
    int commit_count = 0;
    int i;
    int mode_count;
    char *git_dir;
    char *repo_root;
    int rc;

    memset(&opts, 0, sizeof(opts));

    commit_args = malloc((size_t)(argc > 0 ? argc : 1) * sizeof(*commit_args));
    if (commit_args == NULL) {
        fprintf(stderr, "sg: out of memory\n");
        return 1;
    }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--continue") == 0) {
            continue_flag = 1;
        } else if (strcmp(argv[i], "--skip") == 0) {
            skip_flag = 1;
        } else if (strcmp(argv[i], "--abort") == 0) {
            abort_flag = 1;
        } else if (strcmp(argv[i], "--quit") == 0) {
            quit_flag = 1;
        } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--no-commit") == 0) {
            opts.no_commit = 1;
        } else if (strcmp(argv[i], "--commit") == 0) {
            opts.no_commit = 0;
        } else if (strcmp(argv[i], "--no-edit") == 0) {
            /* sg never opens an editor, so this asks for exactly what sg
               already does -- accepted as a no-op. */
        } else if (strcmp(argv[i], "-m") == 0) {
            long n;
            char *end;

            if (i + 1 >= argc) {
                fputs(usage, stderr);
                free(commit_args);
                return 1;
            }
            i++;
            n = strtol(argv[i], &end, 10);
            if (*end != '\0' || end == argv[i] || n <= 0) {
                fputs(usage, stderr);
                free(commit_args);
                return 1;
            }
            opts.mainline = (int)n;
        } else if (argv[i][0] == '-') {
            /* Every deliberately-unimplemented flag from the Phase 57 spec
               (-e/--edit, -x, -s/--signoff, -S, --ff, --allow-empty,
               --allow-empty-message, --keep-redundant-commits, --empty=,
               --cleanup=, --strategy, -X, --rerere-autoupdate,
               --reference, and anything else unrecognized) falls through
               to here: rejected with the usage line, never approximated. */
            fputs(usage, stderr);
            free(commit_args);
            return 1;
        } else {
            commit_args[commit_count++] = argv[i];
        }
    }

    mode_count = continue_flag + skip_flag + abort_flag + quit_flag + (commit_count > 0 ? 1 : 0);
    if (mode_count != 1) {
        fputs(usage, stderr);
        free(commit_args);
        return 1;
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL) {
        free(commit_args);
        return 1;
    }
    repo_root = sg_repo_root(git_dir);
    if (repo_root == NULL) {
        fprintf(stderr, "sg: failed to determine repository root\n");
        free(git_dir);
        free(commit_args);
        return 1;
    }

    if (continue_flag) {
        rc = sg_pick_continue(git_dir, repo_root, SG_SEQ_CHERRY_PICK);
    } else if (skip_flag) {
        rc = sg_pick_skip(git_dir, repo_root, SG_SEQ_CHERRY_PICK);
    } else if (abort_flag) {
        rc = sg_pick_abort(git_dir, repo_root, SG_SEQ_CHERRY_PICK);
    } else if (quit_flag) {
        rc = sg_pick_quit(git_dir, repo_root, SG_SEQ_CHERRY_PICK);
    } else {
        unsigned char(*ids)[SG_SHA1_RAW_LEN] = malloc((size_t)commit_count * sizeof(*ids));

        if (ids == NULL) {
            fprintf(stderr, "sg: out of memory\n");
            free(git_dir);
            free(repo_root);
            free(commit_args);
            return 1;
        }
        for (i = 0; i < commit_count; i++) {
            if (sg_rev_parse_commit(git_dir, commit_args[i], ids[i]) != 0) {
                fprintf(stderr, "sg: '%s' is not a valid object id\n", commit_args[i]);
                free(ids);
                free(git_dir);
                free(repo_root);
                free(commit_args);
                return 1;
            }
        }
        rc = sg_pick_start(git_dir, repo_root, SG_SEQ_CHERRY_PICK, ids, (size_t)commit_count, &opts);
        free(ids);
    }

    free(git_dir);
    free(repo_root);
    free(commit_args);
    return rc;
}
