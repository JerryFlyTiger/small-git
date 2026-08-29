#include "sg/refs.h"

#include <stdio.h>

/* All 21 rows measured against real git 2.55.0 (LC_ALL=C), see the Phase 39
   review round's report: `git branch <name>`/`git tag <name>` for the
   accept/reject verdict, `git check-ref-format refs/heads/<name>` for the
   ones expressed as a full ref path. Only the ".hidden" rule (a path
   component starting with '.') was found to disagree with sg's existing
   sg_ref_name_valid_for_create; every other row here is a fixed-regression
   guard for a rule that was already correct. */

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

static void test_dot_leading_component_rejected(void)
{
    /* The two rows this test file exists for: sg used to accept both. */
    CHECK(sg_ref_name_valid_for_create(".hidden") == 0,
         "'.hidden' must be rejected (a leading-dot component)");
    CHECK(sg_ref_name_valid_for_create("x/.hidden") == 0,
         "'x/.hidden' must be rejected (a leading-dot component after a slash)");
}

static void test_accepted_names_still_accepted(void)
{
    CHECK(sg_ref_name_valid_for_create("ok") == 1, "'ok' must still be accepted");
    CHECK(sg_ref_name_valid_for_create("@") == 1, "'@' must still be accepted");
    CHECK(sg_ref_name_valid_for_create("refs/heads/nested") == 1,
         "'refs/heads/nested' must still be accepted");
}

static void test_already_rejected_names_stay_rejected(void)
{
    static const char *bad[] = {
        "a.lock", "x/a.lock", "a..b", "a b", "a~1", "a^", "a:b", "a\\b",
        "a/", "/a", "a//b", "-dash", "a@{b", "a.", "x/a.",
    };
    size_t i;

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(sg_ref_name_valid_for_create(bad[i]) == 0, "'%s' must be rejected", bad[i]);
    }
}

int main(void)
{
    test_dot_leading_component_rejected();
    test_accepted_names_still_accepted();
    test_already_rejected_names_stay_rejected();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all ref_name_dot tests passed\n");
    return 0;
}
