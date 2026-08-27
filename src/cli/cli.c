#include "sg/cli.h"

#include "sg/levenshtein.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    const char *desc;
} sg_command_info;

static const sg_command_info COMMANDS[] = {
    {"init", "create a new repository"},
    {"hash-object", "compute (and optionally write) an object's hash"},
    {"cat-file", "inspect an object's content/type/size"},
    {"add", "add files to the index"},
    {"commit", "create a commit"},
    {"log", "show commit history"},
    {"status", "show the working directory status"},
    {"diff", "show changes (index/working dir/between revs), several formats"},
    {"branch", "list, create, or delete branches"},
    {"tag", "list, create, or delete tags"},
    {"switch", "switch branches"},
    {"restore", "restore files or unstage them"},
    {"reset", "reset branch, index (and optionally working dir) to a commit"},
    {"undo", "list or restore automatic snapshots"},
    {"repack", "pack loose objects into a packfile"},
    {"merge", "merge another branch"},
    {"merge-base", "find the nearest common ancestor of two commits"},
    {"rebase", "reapply the current branch on top of another branch"},
    {"clone", "clone a repository from a remote"},
    {"fetch", "fetch new commits and refs from a remote"},
    {"push", "push the local branch to a remote"},
    {"chunk-info", "show chunk-storage diagnostics for a file/object"},
    {"stash", "stash working directory and index changes for later restore"},
    {"reflog", "show a ref's update history"},
};
#define COMMANDS_COUNT (sizeof(COMMANDS) / sizeof(COMMANDS[0]))

static void print_help(FILE *out)
{
    size_t i;

    fprintf(out, "usage: sg <command> [<args>]\n\nCommands:\n");
    for (i = 0; i < COMMANDS_COUNT; i++)
        fprintf(out, "  %-13s %s\n", COMMANDS[i].name, COMMANDS[i].desc);
}

int sg_cli_run(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 ||
       strcmp(argv[1], "help") == 0) {
        print_help(stdout);
        return 0;
    }

    /* Phrased like `git version` so scripts that already scrape that shape
       need no special case; SG_VERSION is the single definition shared with
       the man page's .TH line and the packaging targets. */
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0 ||
       strcmp(argv[1], "version") == 0) {
        printf("sg version %s\n", SG_VERSION);
        return 0;
    }

    if (strcmp(argv[1], "init") == 0)
        return sg_cmd_init(argc - 1, argv + 1);
    if (strcmp(argv[1], "hash-object") == 0)
        return sg_cmd_hash_object(argc - 1, argv + 1);
    if (strcmp(argv[1], "cat-file") == 0)
        return sg_cmd_cat_file(argc - 1, argv + 1);
    if (strcmp(argv[1], "add") == 0)
        return sg_cmd_add(argc - 1, argv + 1);
    if (strcmp(argv[1], "commit") == 0)
        return sg_cmd_commit(argc - 1, argv + 1);
    if (strcmp(argv[1], "log") == 0)
        return sg_cmd_log(argc - 1, argv + 1);
    if (strcmp(argv[1], "status") == 0)
        return sg_cmd_status(argc - 1, argv + 1);
    if (strcmp(argv[1], "diff") == 0)
        return sg_cmd_diff(argc - 1, argv + 1);
    if (strcmp(argv[1], "branch") == 0)
        return sg_cmd_branch(argc - 1, argv + 1);
    if (strcmp(argv[1], "tag") == 0)
        return sg_cmd_tag(argc - 1, argv + 1);
    if (strcmp(argv[1], "switch") == 0)
        return sg_cmd_switch(argc - 1, argv + 1);
    if (strcmp(argv[1], "restore") == 0)
        return sg_cmd_restore(argc - 1, argv + 1);
    if (strcmp(argv[1], "reset") == 0)
        return sg_cmd_reset(argc - 1, argv + 1);
    if (strcmp(argv[1], "undo") == 0)
        return sg_cmd_undo(argc - 1, argv + 1);
    if (strcmp(argv[1], "repack") == 0)
        return sg_cmd_repack(argc - 1, argv + 1);
    if (strcmp(argv[1], "merge-base") == 0)
        return sg_cmd_merge_base(argc - 1, argv + 1);
    if (strcmp(argv[1], "merge") == 0)
        return sg_cmd_merge(argc - 1, argv + 1);
    if (strcmp(argv[1], "rebase") == 0)
        return sg_cmd_rebase(argc - 1, argv + 1);
    if (strcmp(argv[1], "clone") == 0)
        return sg_cmd_clone(argc - 1, argv + 1);
    if (strcmp(argv[1], "fetch") == 0)
        return sg_cmd_fetch(argc - 1, argv + 1);
    if (strcmp(argv[1], "push") == 0)
        return sg_cmd_push(argc - 1, argv + 1);
    if (strcmp(argv[1], "chunk-info") == 0)
        return sg_cmd_chunk_info(argc - 1, argv + 1);
    if (strcmp(argv[1], "stash") == 0)
        return sg_cmd_stash(argc - 1, argv + 1);
    if (strcmp(argv[1], "reflog") == 0)
        return sg_cmd_reflog(argc - 1, argv + 1);

    {
        size_t i;
        size_t best_idx = 0;
        size_t best_dist = (size_t)-1;

        for (i = 0; i < COMMANDS_COUNT; i++) {
            size_t dist = sg_levenshtein(argv[1], COMMANDS[i].name);

            if (dist < best_dist) {
                best_dist = dist;
                best_idx = i;
            }
        }

        fprintf(stderr, "sg: '%s' is not a sg command\n", argv[1]);
        if (best_dist <= 2) {
            fprintf(stderr, "\nDid you mean '%s'?\n", COMMANDS[best_idx].name);
        } else {
            fprintf(stderr, "\n");
            print_help(stderr);
        }
    }
    return 1;
}
