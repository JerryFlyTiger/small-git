/* sg_push_refspec_parse (and the sg_push_refspec type it fills in) is
   defined non-static, but not part of any public header, in
   src/cli/cmd_push.c -- declared here via extern to keep it unit-testable
   without a real network round trip, the same convention
   tests/test_push_report.c and tests/test_refadv.c use for their own
   internal-but-non-static parsers. The struct definition below must stay
   field-for-field identical to cmd_push.c's (C gives no cross-TU identity
   check for an unnamed struct type, only ABI-compatible layout matters
   here, same as those two examples). */
#include "sg/hash.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    int force;
    char *src;
    char *dst;
    int is_delete;
} sg_push_refspec;

extern int sg_push_refspec_parse(const char *raw_arg, sg_push_refspec *out);
extern void sg_push_refspec_free(sg_push_refspec *r);

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

static int streq_or_null(const char *a, const char *b)
{
    if (a == NULL || b == NULL)
        return a == b;
    return strcmp(a, b) == 0;
}

/* ---- section 0.2: no colon -- dst is derived elsewhere, not by the parser ---- */

static void test_no_colon(void)
{
    sg_push_refspec r;
    int rc = sg_push_refspec_parse("topic", &r);

    CHECK(rc == 0, "expected success, got %d", rc);
    CHECK(r.force == 0, "force should default to 0");
    CHECK(streq_or_null(r.src, "topic"), "src should be 'topic', got '%s'", r.src);
    CHECK(r.dst == NULL, "dst should be NULL (no colon), got '%s'", r.dst);
    CHECK(r.is_delete == 0, "is_delete should be 0");
    sg_push_refspec_free(&r);
}

/* ---- section 0.1: split on the LAST colon, not the first ---- */

static void test_last_colon_four_segments(void)
{
    sg_push_refspec r;
    int rc = sg_push_refspec_parse("a:b:c:d", &r);

    CHECK(rc == 0, "expected success, got %d", rc);
    CHECK(streq_or_null(r.src, "a:b:c"), "src should be 'a:b:c', got '%s'", r.src);
    CHECK(streq_or_null(r.dst, "d"), "dst should be 'd', got '%s'", r.dst);
    CHECK(r.is_delete == 0, "is_delete should be 0");
    sg_push_refspec_free(&r);
}

static void test_last_colon_three_segments(void)
{
    sg_push_refspec r;
    int rc = sg_push_refspec_parse("aaa:bbb:ccc", &r);

    CHECK(rc == 0, "expected success, got %d", rc);
    CHECK(streq_or_null(r.src, "aaa:bbb"), "src should be 'aaa:bbb', got '%s'", r.src);
    CHECK(streq_or_null(r.dst, "ccc"), "dst should be 'ccc', got '%s'", r.dst);
    sg_push_refspec_free(&r);
}

/* ---- section 0.3: ":dst" (empty src) is a delete ---- */

static void test_colon_dst_is_delete(void)
{
    sg_push_refspec r;
    int rc = sg_push_refspec_parse(":dst", &r);

    CHECK(rc == 0, "expected success, got %d", rc);
    CHECK(streq_or_null(r.src, ""), "src should be empty, got '%s'", r.src);
    CHECK(streq_or_null(r.dst, "dst"), "dst should be 'dst', got '%s'", r.dst);
    CHECK(r.is_delete == 1, "is_delete should be 1");
    CHECK(r.force == 0, "force should be 0");
    sg_push_refspec_free(&r);
}

/* ---- section 0.4: "<src>:" (empty dst) is a syntax error ---- */

static void test_empty_dst_is_error(void)
{
    sg_push_refspec r;
    int rc = sg_push_refspec_parse("src:", &r);

    CHECK(rc == -1, "expected -1 (empty dst is an error), got %d", rc);
    sg_push_refspec_free(&r);
}

/* ---- section 0.5: leading '+' forces just this one refspec, including
   "+:dst" (forced delete) ---- */

static void test_leading_plus_forces(void)
{
    sg_push_refspec r;
    int rc = sg_push_refspec_parse("+topic:dst", &r);

    CHECK(rc == 0, "expected success, got %d", rc);
    CHECK(r.force == 1, "force should be 1");
    CHECK(streq_or_null(r.src, "topic"), "src should be 'topic', got '%s'", r.src);
    CHECK(streq_or_null(r.dst, "dst"), "dst should be 'dst', got '%s'", r.dst);
    CHECK(r.is_delete == 0, "is_delete should be 0");
    sg_push_refspec_free(&r);
}

static void test_forced_delete(void)
{
    sg_push_refspec r;
    int rc = sg_push_refspec_parse("+:dst", &r);

    CHECK(rc == 0, "expected success, got %d", rc);
    CHECK(r.force == 1, "force should be 1");
    CHECK(r.is_delete == 1, "is_delete should be 1");
    CHECK(streq_or_null(r.dst, "dst"), "dst should be 'dst', got '%s'", r.dst);
    sg_push_refspec_free(&r);
}

/* ---- section 6.1/6.2: wildcard refspecs and bare push-matching ":" are
   named rejections, not approximated ---- */

static void test_wildcard_rejected(void)
{
    sg_push_refspec r;
    int rc = sg_push_refspec_parse("refs/heads/*:refs/heads/*", &r);

    CHECK(rc == -1, "wildcard refspec should be rejected, got %d", rc);
    sg_push_refspec_free(&r);
}

static void test_wildcard_in_src_only_rejected(void)
{
    sg_push_refspec r;
    /* '*' only in src, explicit non-wildcard dst -- still rejected, real
       git's refspec wildcard is a whole-refspec property, not per-side. */
    int rc = sg_push_refspec_parse("refs/heads/top*:refs/heads/rev1", &r);

    CHECK(rc == -1, "src-only wildcard refspec should be rejected, got %d", rc);
    sg_push_refspec_free(&r);
}

static void test_bare_colon_rejected(void)
{
    sg_push_refspec r;
    int rc = sg_push_refspec_parse(":", &r);

    CHECK(rc == -1, "bare ':' (push matching) should be rejected, got %d", rc);
    sg_push_refspec_free(&r);
}

static void test_forced_bare_colon_rejected(void)
{
    sg_push_refspec r;
    int rc = sg_push_refspec_parse("+:", &r);

    CHECK(rc == -1, "'+:' (forced push matching) should be rejected, got %d", rc);
    sg_push_refspec_free(&r);
}

int main(void)
{
    test_no_colon();
    test_last_colon_four_segments();
    test_last_colon_three_segments();
    test_colon_dst_is_delete();
    test_empty_dst_is_error();
    test_leading_plus_forces();
    test_forced_delete();
    test_wildcard_rejected();
    test_wildcard_in_src_only_rejected();
    test_bare_colon_rejected();
    test_forced_bare_colon_rejected();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all refspec tests passed\n");
    return 0;
}
