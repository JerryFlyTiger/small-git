#include "sg/confirm.h"

#include <stdio.h>
#include <unistd.h>

int sg_confirm_dangerous(const char *message, int force)
{
    char line[64];

    if (force) {
        printf("sg: --force, skipping confirmation and proceeding\n");
        return 1;
    }

    /* stdin is not a tty (e.g. a test script or CI pipe): fgets would block
       forever waiting for input that will never arrive, so never call it
       here -- just refuse and tell the caller how to force it. */
    if (!isatty(STDIN_FILENO)) {
        fputs(message, stderr);
        fputs("sg: this is a destructive operation; add --force if you are sure you want to "
              "continue\n", stderr);
        return 0;
    }

    fputs(message, stdout);
    fputs("Continue? (y/N) ", stdout);
    fflush(stdout);
    if (fgets(line, sizeof(line), stdin) == NULL)
        return 0;
    return line[0] == 'y' || line[0] == 'Y';
}
