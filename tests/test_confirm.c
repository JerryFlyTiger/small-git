#include "sg/confirm.h"

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, ...)                                                                        \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);                                 \
            fprintf(stderr, __VA_ARGS__);                                                        \
            fprintf(stderr, "\n");                                                               \
            failures++;                                                                          \
        }                                                                                         \
    } while (0)

/* Redirects stdin to /dev/null for the duration of a call, so this test
   never blocks even if run interactively -- isatty(STDIN_FILENO) reports
   false against /dev/null, exercising the same "non-interactive" path a CI
   pipe would take. The original stdin fd is restored afterwards. */
static int call_with_dev_null_stdin(const char *message, int force)
{
    int saved_stdin = dup(STDIN_FILENO);
    int devnull = open("/dev/null", O_RDONLY);
    int result;

    if (saved_stdin < 0 || devnull < 0) {
        fprintf(stderr, "setup failed: could not redirect stdin\n");
        if (saved_stdin >= 0)
            close(saved_stdin);
        if (devnull >= 0)
            close(devnull);
        return -1;
    }

    dup2(devnull, STDIN_FILENO);
    close(devnull);

    result = sg_confirm_dangerous(message, force);

    dup2(saved_stdin, STDIN_FILENO);
    close(saved_stdin);
    return result;
}

static void test_force_always_proceeds(void)
{
    int rc = call_with_dev_null_stdin("this would overwrite something\n", 1);

    CHECK(rc == 1, "force=1 should always return 1, got %d", rc);
}

static void test_non_tty_without_force_refuses_without_blocking(void)
{
    /* the critical correctness property: this call must return promptly
       (no fgets on a stream that will never produce a line) */
    int rc = call_with_dev_null_stdin("this would overwrite something\n", 0);

    CHECK(rc == 0, "non-tty stdin without --force should refuse (0), got %d", rc);
}

int main(void)
{
    test_force_always_proceeds();
    test_non_tty_without_force_refuses_without_blocking();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all confirm tests passed\n");
    return 0;
}
