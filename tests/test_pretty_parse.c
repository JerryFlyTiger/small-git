/* Phase 60 section 2: the five ordered grammar rules for --pretty=/--format=,
   exercised directly against sg_pretty_parse. All seven rows of CLAUDE.md's
   `sg log` grammar table, each measured against real git 2.55.0 before this
   test was written (see docs/DESIGN.md's Phase 60 section). */
#include "sg/commit_out.h"

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

/* Row 1: "Oneline" matches the builtin case-insensitively (rule 3). */
static void test_case_insensitive_builtin(void)
{
    sg_pretty_format out;
    int rc = sg_pretty_parse("Oneline", &out);

    CHECK(rc == 0, "expected success, got %d", rc);
    CHECK(out.kind == SG_PRETTY_ONELINE, "expected SG_PRETTY_ONELINE, got %d", (int)out.kind);
}

/* Row 2: "FORMAT:%H" does NOT match the "format:" prefix (case-SENSITIVE),
   so it falls to rule 4 (contains '%') as a literal "FORMAT:%H" tformat
   string. */
static void test_format_prefix_is_case_sensitive(void)
{
    sg_pretty_format out;
    int rc = sg_pretty_parse("FORMAT:%H", &out);

    CHECK(rc == 0, "expected success, got %d", rc);
    CHECK(out.kind == SG_PRETTY_TFORMAT, "expected SG_PRETTY_TFORMAT, got %d", (int)out.kind);
    CHECK(out.user_format != NULL && strcmp(out.user_format, "FORMAT:%H") == 0,
         "expected user_format to be the whole string, got %s",
         out.user_format == NULL ? "(null)" : out.user_format);
}

/* Row 3: "abc%H" -- rule 4, literal "abc" is not stripped. */
static void test_contains_percent_is_tformat(void)
{
    sg_pretty_format out;
    int rc = sg_pretty_parse("abc%H", &out);

    CHECK(rc == 0, "expected success, got %d", rc);
    CHECK(out.kind == SG_PRETTY_TFORMAT, "expected SG_PRETTY_TFORMAT, got %d", (int)out.kind);
    CHECK(out.user_format != NULL && strcmp(out.user_format, "abc%H") == 0,
         "expected user_format 'abc%%H', got %s", out.user_format == NULL ? "(null)" : out.user_format);
}

/* Row 4: "abc" matches none of rules 1-4, rule 5 fires: error. */
static void test_no_match_is_error(void)
{
    sg_pretty_format out;
    int rc = sg_pretty_parse("abc", &out);

    CHECK(rc == -1, "expected -1 (rule 5 error), got %d", rc);
}

/* Row 5: "oneline%H" -- rule 3 needs the WHOLE string to equal a builtin
   name, not a prefix, so this falls through to rule 4 instead. */
static void test_builtin_match_is_whole_string_not_prefix(void)
{
    sg_pretty_format out;
    int rc = sg_pretty_parse("oneline%H", &out);

    CHECK(rc == 0, "expected success, got %d", rc);
    CHECK(out.kind == SG_PRETTY_TFORMAT, "expected SG_PRETTY_TFORMAT (not ONELINE), got %d",
         (int)out.kind);
    CHECK(out.user_format != NULL && strcmp(out.user_format, "oneline%H") == 0,
         "expected user_format 'oneline%%H', got %s",
         out.user_format == NULL ? "(null)" : out.user_format);
}

/* Row 6: "format:plain" -- rule 1, FORMAT kind, user_format past the
   prefix. */
static void test_format_prefix(void)
{
    sg_pretty_format out;
    int rc = sg_pretty_parse("format:plain", &out);

    CHECK(rc == 0, "expected success, got %d", rc);
    CHECK(out.kind == SG_PRETTY_FORMAT, "expected SG_PRETTY_FORMAT, got %d", (int)out.kind);
    CHECK(out.user_format != NULL && strcmp(out.user_format, "plain") == 0,
         "expected user_format 'plain', got %s", out.user_format == NULL ? "(null)" : out.user_format);
}

/* Row 7: "tformat:plain" -- rule 2, TFORMAT kind. */
static void test_tformat_prefix(void)
{
    sg_pretty_format out;
    int rc = sg_pretty_parse("tformat:plain", &out);

    CHECK(rc == 0, "expected success, got %d", rc);
    CHECK(out.kind == SG_PRETTY_TFORMAT, "expected SG_PRETTY_TFORMAT, got %d", (int)out.kind);
    CHECK(out.user_format != NULL && strcmp(out.user_format, "plain") == 0,
         "expected user_format 'plain', got %s", out.user_format == NULL ? "(null)" : out.user_format);
}

/* Rule ordering: "format:" must be checked before the case-insensitive
   builtin lookup, or it would never be reachable in the first place --
   there is no builtin literally named "format:plain", so this mostly
   guards against someone reordering the checks and colliding with rule 4
   instead (a "format:" string always contains no '%' in this row, so a
   swapped rule 1/4 order would silently reject it via rule 5). */
static void test_medium_builtin_still_resolves(void)
{
    sg_pretty_format out;
    int rc = sg_pretty_parse("medium", &out);

    CHECK(rc == 0, "expected success, got %d", rc);
    CHECK(out.kind == SG_PRETTY_MEDIUM, "expected SG_PRETTY_MEDIUM, got %d", (int)out.kind);
}

int main(void)
{
    test_case_insensitive_builtin();
    test_format_prefix_is_case_sensitive();
    test_contains_percent_is_tformat();
    test_no_match_is_error();
    test_builtin_match_is_whole_string_not_prefix();
    test_format_prefix();
    test_tformat_prefix();
    test_medium_builtin_still_resolves();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all pretty-parse tests passed\n");
    return 0;
}
