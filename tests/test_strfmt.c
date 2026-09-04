#include "sg/strfmt.h"

#include <stdio.h>
#include <stdlib.h>
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

static void test_empty_result(void)
{
    char *s = sg_strfmt_alloc("%s", "");

    CHECK(s != NULL, "expected a non-NULL allocation for an empty result");
    if (s != NULL) {
        CHECK(s[0] == '\0', "expected an empty string, got %s", s);
        CHECK(strlen(s) == 0, "expected length 0, got %zu", strlen(s));
    }
    free(s);
}

static void test_no_format_specifiers(void)
{
    char *s = sg_strfmt_alloc("plain text, no specifiers");

    CHECK(s != NULL, "expected a non-NULL allocation");
    if (s != NULL)
        CHECK(strcmp(s, "plain text, no specifiers") == 0, "got %s", s);
    free(s);
}

/* Crosses whatever internal initial-size guess an implementation might use
   (there is none here -- vsnprintf(NULL, 0, ...) sizes exactly -- but this
   pins that a long result is not silently truncated regardless). */
static void test_result_crossing_typical_stack_buffer_sizes(void)
{
    size_t sizes[] = {8, 64, 255, 256, 300, 511, 512, 1024, 4096, 10000};
    size_t i;

    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        size_t n = sizes[i];
        char *input = malloc(n + 1);
        char *s;

        CHECK(input != NULL, "setup: malloc failed for n=%zu", n);
        if (input == NULL)
            continue;
        memset(input, 'x', n);
        input[n] = '\0';

        s = sg_strfmt_alloc("prefix-%s-suffix", input);
        CHECK(s != NULL, "expected a non-NULL allocation for n=%zu", n);
        if (s != NULL) {
            size_t expect_len = strlen("prefix-") + n + strlen("-suffix");

            CHECK(strlen(s) == expect_len, "n=%zu: expected length %zu, got %zu", n, expect_len,
                 strlen(s));
            CHECK(strncmp(s, "prefix-", 7) == 0, "n=%zu: missing prefix", n);
            CHECK(strcmp(s + strlen(s) - 7, "-suffix") == 0, "n=%zu: missing/truncated suffix", n);
        }
        free(s);
        free(input);
    }
}

static void test_embedded_percent(void)
{
    char *s = sg_strfmt_alloc("100%% done: %s", "yes");

    CHECK(s != NULL, "expected a non-NULL allocation");
    if (s != NULL)
        CHECK(strcmp(s, "100% done: yes") == 0, "got %s", s);
    free(s);
}

static void test_multiple_arguments(void)
{
    char *s = sg_strfmt_alloc("%s (%s) [%d]", "a", "b", 3);

    CHECK(s != NULL, "expected a non-NULL allocation");
    if (s != NULL)
        CHECK(strcmp(s, "a (b) [3]") == 0, "got %s", s);
    free(s);
}

int main(void)
{
    test_empty_result();
    test_no_format_specifiers();
    test_result_crossing_typical_stack_buffer_sizes();
    test_embedded_percent();
    test_multiple_arguments();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all strfmt tests passed\n");
    return 0;
}
