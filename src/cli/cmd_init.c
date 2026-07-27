#include "sg/cli.h"

#include "sg/repo.h"

#include <stdio.h>

int sg_cmd_init(int argc, char **argv)
{
    const char *dir = NULL;

    if (argc > 2) {
        fprintf(stderr, "usage: sg init [<directory>]\n");
        return 1;
    }
    if (argc == 2)
        dir = argv[1];

    if (sg_repo_init(dir) != 0) {
        fprintf(stderr, "sg: failed to initialize repository in '%s'\n", dir != NULL ? dir : ".");
        return 1;
    }

    printf("Initialized empty Git repository in %s/.git/\n", dir != NULL ? dir : ".");
    return 0;
}
