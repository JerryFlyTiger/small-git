/* sg_path_join, the one place the tree-walking code decides whether a path
   fits. Its callers act on the answer by unlinking, writing and gating, so a
   wrong "it fits" is how sg would touch a file the user never named.

   These tests pass tiny out_size values rather than building real 4096-byte
   paths on disk, and that is deliberate: on macOS the kernel's PATH_MAX
   (1024) is far below SG_PATH_MAX (4096), so a real filesystem path long
   enough to truncate the buffer would be rejected by the kernel first and the
   truncation branch would never run. A test built out of real deep
   directories would therefore be green on this platform for a reason that has
   nothing to do with the code under test. */

#include "sg/workdir.h"

#include <stdio.h>
#include <string.h>

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

static void test_joins_base_and_rel(void)
{
    char out[64];

    CHECK(sg_path_join(out, sizeof(out), "/repo", "a/b.txt") == 0, "join should succeed");
    CHECK(strcmp(out, "/repo/a/b.txt") == 0, "got '%s'", out);
}

/* Either side being absent means "just the other one" -- callers rely on this
   for top-level entries, where reldir is "". */
static void test_empty_or_null_sides(void)
{
    char out[64];

    CHECK(sg_path_join(out, sizeof(out), "/repo", "") == 0, "empty rel should succeed");
    CHECK(strcmp(out, "/repo") == 0, "empty rel: got '%s'", out);

    CHECK(sg_path_join(out, sizeof(out), "/repo", NULL) == 0, "NULL rel should succeed");
    CHECK(strcmp(out, "/repo") == 0, "NULL rel: got '%s'", out);

    CHECK(sg_path_join(out, sizeof(out), "", "a.txt") == 0, "empty base should succeed");
    CHECK(strcmp(out, "a.txt") == 0, "empty base: got '%s'", out);

    CHECK(sg_path_join(out, sizeof(out), NULL, "a.txt") == 0, "NULL base should succeed");
    CHECK(strcmp(out, "a.txt") == 0, "NULL base: got '%s'", out);
}

/* The boundary, spelled out on both sides of it. "ab/cd" is 5 bytes plus a
   NUL, so 6 is exactly enough and 5 is one too few. Off by one here is the
   difference between refusing a path and handing back a truncated one. */
static void test_exact_fit_and_one_byte_short(void)
{
    char out[8];

    memset(out, 'X', sizeof(out));
    CHECK(sg_path_join(out, 6, "ab", "cd") == 0, "6 bytes is exactly enough for 'ab/cd'");
    CHECK(strcmp(out, "ab/cd") == 0, "exact fit: got '%s'", out);

    CHECK(sg_path_join(out, 5, "ab", "cd") == -1, "5 bytes must be refused, not truncated");
}

/* The failure that matters: a path too long must never come back as a
   shorter, real one. "/repo/subdir/deep.txt" truncated into 8 bytes would be
   "/repo/s", and a caller that acted on it could unlink or overwrite whatever
   that names. */
static void test_truncation_is_refused_not_silently_shortened(void)
{
    char out[8];

    CHECK(sg_path_join(out, sizeof(out), "/repo", "subdir/deep.txt") == -1,
         "a path that does not fit must return -1");

    CHECK(sg_path_join(out, sizeof(out), "/repo/very/long/base", "") == -1,
         "a base that alone does not fit must return -1");

    CHECK(sg_path_join(out, sizeof(out), "", "very/long/relative/path.txt") == -1,
         "a rel that alone does not fit must return -1");
}

/* out_size 0 leaves nowhere to write even a NUL, so it cannot be a success. */
static void test_zero_out_size(void)
{
    char out[8];

    memset(out, 'X', sizeof(out));
    CHECK(sg_path_join(out, 0, "/repo", "a.txt") == -1, "out_size 0 must return -1");
    CHECK(out[0] == 'X', "out_size 0 must not write to the buffer");
}

int main(void)
{
    test_joins_base_and_rel();
    test_empty_or_null_sides();
    test_exact_fit_and_one_byte_short();
    test_truncation_is_refused_not_silently_shortened();
    test_zero_out_size();

    if (failures > 0)
        return 1;
    printf("all path_join tests passed\n");
    return 0;
}
