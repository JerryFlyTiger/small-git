#include "sg/cli.h"

#include <stdio.h>
#include <string.h>

int sg_cli_run(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: sg <command> [<args>]\n");
        return 1;
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
    if (strcmp(argv[1], "switch") == 0)
        return sg_cmd_switch(argc - 1, argv + 1);
    if (strcmp(argv[1], "restore") == 0)
        return sg_cmd_restore(argc - 1, argv + 1);

    fprintf(stderr, "sg: '%s' is not a sg command\n", argv[1]);
    return 1;
}
