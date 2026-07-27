#include "sg/confirm.h"

#include <stdio.h>
#include <unistd.h>

int sg_confirm_dangerous(const char *message, int force)
{
    char line[64];

    if (force) {
        printf("sg: --force，略過確認直接執行\n");
        return 1;
    }

    /* stdin is not a tty (e.g. a test script or CI pipe): fgets would block
       forever waiting for input that will never arrive, so never call it
       here -- just refuse and tell the caller how to force it. */
    if (!isatty(STDIN_FILENO)) {
        fputs(message, stderr);
        fputs("sg: 這是危險操作，如果你確定要繼續，請加上 --force\n", stderr);
        return 0;
    }

    fputs(message, stdout);
    fputs("繼續嗎? (y/N) ", stdout);
    fflush(stdout);
    if (fgets(line, sizeof(line), stdin) == NULL)
        return 0;
    return line[0] == 'y' || line[0] == 'Y';
}
