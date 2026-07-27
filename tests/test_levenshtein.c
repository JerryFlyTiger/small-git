#include "sg/levenshtein.h"

#include <stdio.h>

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

static void check_dist(const char *a, const char *b, size_t expected)
{
    size_t got = sg_levenshtein(a, b);

    CHECK(got == expected, "sg_levenshtein(\"%s\", \"%s\") = %zu, expected %zu", a, b, got,
         expected);
}

int main(void)
{
    check_dist("", "", 0);
    check_dist("", "abc", 3);
    check_dist("abc", "", 3);
    check_dist("status", "status", 0);
    check_dist("stat", "status", 2); /* two insertions */
    check_dist("stauts", "status", 2); /* transposition = 2 substitutions under this metric */
    check_dist("switch", "swithc", 2);
    check_dist("kitten", "sitting", 3); /* classic textbook example */
    check_dist("restore", "restor", 1);
    check_dist("commit", "log", 5);

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all levenshtein tests passed\n");
    return 0;
}
