#include "sg/date.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* Every row here was measured against real git 2.55.0 -- see
   PHASE64_SPEC.md section 1 (grammar) and section 3 (byte specs). */

/* ---- section 1: grammar -------------------------------------------- */

static void check_parse_ok(const char *arg, sg_date_kind kind, int local)
{
    sg_date_mode m;
    int rc = sg_date_parse_mode(arg, &m);

    CHECK(rc == 0, "sg_date_parse_mode(\"%s\") should succeed, got %d", arg, rc);
    if (rc != 0)
        return;
    CHECK(m.kind == kind, "\"%s\": expected kind %d, got %d", arg, (int)kind, (int)m.kind);
    CHECK(m.local == local, "\"%s\": expected local=%d, got %d", arg, local, m.local);
}

static void check_parse_err(const char *arg)
{
    sg_date_mode m;
    int rc = sg_date_parse_mode(arg, &m);

    CHECK(rc == -1, "sg_date_parse_mode(\"%s\") should fail, got %d", arg, rc);
}

static void test_grammar(void)
{
    /* rule 2/3: literal names, no suffix. */
    check_parse_ok("default", SG_DATE_DEFAULT, 0);
    check_parse_ok("iso", SG_DATE_ISO, 0);
    check_parse_ok("iso8601", SG_DATE_ISO, 0);
    check_parse_ok("iso-strict", SG_DATE_ISO_STRICT, 0);
    check_parse_ok("iso8601-strict", SG_DATE_ISO_STRICT, 0);
    check_parse_ok("rfc", SG_DATE_RFC2822, 0);
    check_parse_ok("rfc2822", SG_DATE_RFC2822, 0);
    check_parse_ok("short", SG_DATE_SHORT, 0);
    check_parse_ok("raw", SG_DATE_RAW, 0);
    check_parse_ok("unix", SG_DATE_UNIX, 0);

    /* rule 5: -local suffix, stripped once. */
    check_parse_ok("default-local", SG_DATE_DEFAULT, 1);
    check_parse_ok("local", SG_DATE_DEFAULT, 1); /* alias, not general suffix path */
    check_parse_ok("iso-local", SG_DATE_ISO, 1);
    check_parse_ok("iso8601-local", SG_DATE_ISO, 1);
    check_parse_ok("iso-strict-local", SG_DATE_ISO_STRICT, 1);
    check_parse_ok("rfc-local", SG_DATE_RFC2822, 1);
    check_parse_ok("short-local", SG_DATE_SHORT, 1);
    check_parse_ok("raw-local", SG_DATE_RAW, 1);
    check_parse_ok("unix-local", SG_DATE_UNIX, 1);

    /* rule 5: the suffix is stripped AT MOST ONCE -- the remainder must
       itself be a valid base name. */
    check_parse_err("local-local");
    check_parse_err("default-local-local");
    check_parse_err("-local");
    check_parse_err("local-");
    check_parse_err("iso-strict-loca");

    /* rule 2: case-sensitive. */
    check_parse_err("DEFAULT");
    check_parse_err("Short");
    check_parse_err("ISO");
    check_parse_err("AUTO:short");

    /* rule 3: empty value. */
    check_parse_err("");

    /* relative/relative-local are implemented as of Phase 66 -- see
       test_date_relative.c for the byte-level boundary table. Only the
       grammar (kind/local) is asserted here. */
    check_parse_ok("relative", SG_DATE_RELATIVE, 0);
    check_parse_ok("relative-local", SG_DATE_RELATIVE, 1);

    /* human/human-local are implemented as of Phase 67 -- see
       test_date_human.c for the byte-level shape table. Only the grammar
       (kind/local) is asserted here. */
    check_parse_ok("human", SG_DATE_HUMAN, 0);
    check_parse_ok("human-local", SG_DATE_HUMAN, 1);

    /* section 0: deliberately still unimplemented -- auto: depends on
       isatty(1), no oracle without a pty. */
    check_parse_err("auto:short");
    check_parse_err("auto:");

    /* rule 6: format:/format-local: are prefixes checked BEFORE the -local
       suffix logic -- "-local" inside the format string is verbatim, not
       stripped. */
    {
        sg_date_mode m;

        CHECK(sg_date_parse_mode("format:x-local", &m) == 0, "format:x-local should parse");
        CHECK(m.kind == SG_DATE_FORMAT && m.local == 0, "format:x-local: wrong kind/local");
        CHECK(strcmp(m.strftime_fmt, "x-local") == 0, "format:x-local: fmt not verbatim, got \"%s\"",
             m.strftime_fmt);

        CHECK(sg_date_parse_mode("format-local:%Y", &m) == 0, "format-local:%%Y should parse");
        CHECK(m.kind == SG_DATE_FORMAT && m.local == 1, "format-local:%%Y: wrong kind/local");
        CHECK(strcmp(m.strftime_fmt, "%Y") == 0, "format-local:%%Y: wrong fmt, got \"%s\"",
             m.strftime_fmt);

        CHECK(sg_date_parse_mode("format:", &m) == 0, "format: (empty) should parse");
        CHECK(m.strftime_fmt[0] == '\0', "format: (empty) should have empty fmt");
    }
}

/* ---- section 3/4: byte specs ----------------------------------------- */

/* Fixture commit: epoch 1704067200 (2024-01-01 00:00:00 UTC), stored tz
   "+0000", per PHASE64_SPEC.md section 3's table. */
#define FIXTURE_EPOCH 1704067200LL
#define FIXTURE_TZ "+0000"

/* A second fixture, in July, to prove the DST offset used is the one AT
   THE COMMIT'S OWN INSTANT, not a fixed value -- section 4. */
#define FIXTURE_EPOCH_JULY 1720000000LL

/* Goes through sg_date_format_mode_alloc, NOT a fixed SG_DATE_MODE_MAX
   stack buffer -- a review round found that this test file's own skeleton
   used to hardcode `char buf[SG_DATE_MODE_MAX]`, the exact same shape as
   the bug in the FIVE production call sites this phase fixed (see
   SG_DATE_MODE_MAX's own header comment in date.h). A fixed buffer here
   would make this test structurally unable to ever see that bug, no
   matter how the `format:` row below was written. */
static void check_render(const char *name, long long epoch, const char *tz,
                         const char *expected)
{
    sg_date_mode m;
    char *buf = NULL;
    int rc;

    CHECK(sg_date_parse_mode(name, &m) == 0, "sg_date_parse_mode(\"%s\") should succeed", name);
    rc = sg_date_format_mode_alloc(&m, epoch, tz, &buf);
    CHECK(rc == 0, "sg_date_format_mode_alloc(\"%s\") should succeed, got %d", name, rc);
    if (rc != 0)
        return;
    CHECK(strcmp(buf, expected) == 0, "\"%s\": expected \"%s\", got \"%s\"", name, expected, buf);
    free(buf);
}

static void with_tz(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();
}

static void test_byte_specs_tokyo(void)
{
    with_tz("Asia/Tokyo"); /* +0900, no DST */

    check_render("default", FIXTURE_EPOCH, FIXTURE_TZ, "Mon Jan 1 00:00:00 2024 +0000");
    check_render("default-local", FIXTURE_EPOCH, FIXTURE_TZ, "Mon Jan 1 09:00:00 2024");
    check_render("local", FIXTURE_EPOCH, FIXTURE_TZ, "Mon Jan 1 09:00:00 2024");
    check_render("iso", FIXTURE_EPOCH, FIXTURE_TZ, "2024-01-01 00:00:00 +0000");
    check_render("iso8601", FIXTURE_EPOCH, FIXTURE_TZ, "2024-01-01 00:00:00 +0000");
    check_render("iso-local", FIXTURE_EPOCH, FIXTURE_TZ, "2024-01-01 09:00:00 +0900");
    check_render("iso-strict", FIXTURE_EPOCH, FIXTURE_TZ, "2024-01-01T00:00:00Z");
    check_render("iso8601-strict", FIXTURE_EPOCH, FIXTURE_TZ, "2024-01-01T00:00:00Z");
    check_render("iso-strict-local", FIXTURE_EPOCH, FIXTURE_TZ, "2024-01-01T09:00:00+09:00");
    check_render("rfc", FIXTURE_EPOCH, FIXTURE_TZ, "Mon, 1 Jan 2024 00:00:00 +0000");
    check_render("rfc2822", FIXTURE_EPOCH, FIXTURE_TZ, "Mon, 1 Jan 2024 00:00:00 +0000");
    check_render("rfc-local", FIXTURE_EPOCH, FIXTURE_TZ, "Mon, 1 Jan 2024 09:00:00 +0900");
    check_render("short", FIXTURE_EPOCH, FIXTURE_TZ, "2024-01-01");
    check_render("short-local", FIXTURE_EPOCH, FIXTURE_TZ, "2024-01-01");
    check_render("raw", FIXTURE_EPOCH, FIXTURE_TZ, "1704067200 +0000");
    check_render("raw-local", FIXTURE_EPOCH, FIXTURE_TZ, "1704067200 +0900");
    check_render("unix", FIXTURE_EPOCH, FIXTURE_TZ, "1704067200");
    check_render("unix-local", FIXTURE_EPOCH, FIXTURE_TZ, "1704067200");

    /* section 5: format:/format-local:, the three rewritten specifiers. */
    check_render("format:%s", FIXTURE_EPOCH, FIXTURE_TZ, "1704067200");
    check_render("format-local:%s", FIXTURE_EPOCH, FIXTURE_TZ, "1704067200"); /* %s never shifts */
    check_render("format:%z", FIXTURE_EPOCH, FIXTURE_TZ, "+0000");
    check_render("format-local:%z", FIXTURE_EPOCH, FIXTURE_TZ, "+0900");
    check_render("format:%Z", FIXTURE_EPOCH, FIXTURE_TZ, ""); /* empty for non-local */
    check_render("format-local:%Z", FIXTURE_EPOCH, FIXTURE_TZ, "JST");
    check_render("format:%%z", FIXTURE_EPOCH, FIXTURE_TZ, "%z"); /* %% is its own token */
    check_render("format:%z%%z", FIXTURE_EPOCH, FIXTURE_TZ, "+0000%z");
    check_render("format:abc%", FIXTURE_EPOCH, FIXTURE_TZ, "abc%"); /* trailing lone % */
    check_render("format:", FIXTURE_EPOCH, FIXTURE_TZ, ""); /* empty format prints nothing */
    check_render("format-local:", FIXTURE_EPOCH, FIXTURE_TZ, "");
    check_render("format:%Y-%m-%d", FIXTURE_EPOCH, FIXTURE_TZ, "2024-01-01");
}

/* raw and format:%z share the "-0000" -> "+0000" normalization every other
   renderer applies (measured against real git via a hand-crafted commit
   object storing a literal "-0000" -- only cat-file -p/--pretty=raw, which
   never reach this function, echo it verbatim). */
static void test_minus_zero_normalization(void)
{
    with_tz("Asia/Tokyo");
    check_render("raw", FIXTURE_EPOCH, "-0000", "1704067200 +0000");
    check_render("format:%z", FIXTURE_EPOCH, "-0000", "+0000");
    check_render("default", FIXTURE_EPOCH, "-0000", "Mon Jan 1 00:00:00 2024 +0000");
}

/* section 4: the DST pair -- New York's offset for the 2024-01-01 fixture
   (-0500) differs from the 2024-07-03 fixture (-0400), so the offset used
   must be computed AT THE COMMIT'S OWN INSTANT via localtime_r, never a
   cached value. This is also the row that makes short-local differ from
   short at all: New York crosses the day boundary backwards for the
   January fixture. */
static void test_dst_pair_new_york(void)
{
    with_tz("America/New_York");

    check_render("default-local", FIXTURE_EPOCH, FIXTURE_TZ, "Sun Dec 31 19:00:00 2023");
    check_render("default-local", FIXTURE_EPOCH_JULY, FIXTURE_TZ, "Wed Jul 3 05:46:40 2024");
    check_render("raw-local", FIXTURE_EPOCH, FIXTURE_TZ, "1704067200 -0500");
    check_render("raw-local", FIXTURE_EPOCH_JULY, FIXTURE_TZ, "1720000000 -0400");
    check_render("short-local", FIXTURE_EPOCH, FIXTURE_TZ, "2023-12-31");
    check_render("short", FIXTURE_EPOCH, FIXTURE_TZ, "2024-01-01");
}

static void test_dst_pair_kolkata(void)
{
    with_tz("Asia/Kolkata"); /* +0530, no DST */

    check_render("short-local", FIXTURE_EPOCH, FIXTURE_TZ, "2024-01-01");
    check_render("iso-local", FIXTURE_EPOCH, FIXTURE_TZ, "2024-01-01 05:30:00 +0530");
}

/* The bug this phase fixes: sg_date_format_mode_alloc must grow its buffer
   past SG_DATE_MODE_MAX (1024) rather than silently returning an empty
   string for a `format:` string whose expansion does not fit a fixed
   ceiling -- an unbounded, user-controlled format string is exactly the
   shape a fixed buffer is the wrong tool for. 2000 bytes is comfortably
   past 1024 with room to spare; the fix is a growth loop, not a bigger
   constant, so this also exercises more than one doubling. */
static void test_format_overflow(void)
{
    char fmt[2048];
    char name[2048 + 8];
    char expected[sizeof fmt];
    sg_date_mode m;
    char *buf = NULL;
    int rc;
    size_t i;

    with_tz("Asia/Tokyo");
    for (i = 0; i < sizeof fmt - 1; i++)
        fmt[i] = 'A';
    fmt[sizeof fmt - 1] = '\0';
    memcpy(expected, fmt, sizeof fmt);

    snprintf(name, sizeof name, "format:%s", fmt);
    CHECK(sg_date_parse_mode(name, &m) == 0, "sg_date_parse_mode of a 2047-byte format should succeed");
    rc = sg_date_format_mode_alloc(&m, FIXTURE_EPOCH, FIXTURE_TZ, &buf);
    CHECK(rc == 0, "sg_date_format_mode_alloc of a 2047-byte literal format should succeed, got %d", rc);
    if (rc == 0) {
        CHECK(strlen(buf) == strlen(expected),
             "expected a %zu-byte render, got %zu bytes", strlen(expected), strlen(buf));
        CHECK(strcmp(buf, expected) == 0, "a >1024-byte format: literal must render in full, not truncate to empty");
        free(buf);
    }
}

/* Section 4: "-local" under a ZERO local offset. Every existing DST/offset
   fixture (Tokyo +0900, New York -0500/-0400, Kolkata +0530) uses a
   NON-ZERO offset, so format_offset_str's `offset < 0 ? '-' : '+'` sign
   choice for exactly zero has no witness anywhere else in this file -- a
   mutation widening that comparison to `<=` (printing "-0000" for a zero
   offset) would leave every other row in this file green. */
static void test_local_zero_offset(void)
{
    with_tz("UTC");

    check_render("raw-local", FIXTURE_EPOCH, FIXTURE_TZ, "1704067200 +0000");
    check_render("format-local:%z", FIXTURE_EPOCH, FIXTURE_TZ, "+0000");
    check_render("iso-local", FIXTURE_EPOCH, FIXTURE_TZ, "2024-01-01 00:00:00 +0000");
}

/* Phase 67: --date=human/human-local go through sg_date_format_mode's
   dispatch, the same alloc'ing path every other name uses -- this exercises
   THAT wiring (the SG_DATE_HUMAN case, and resolve_mode_tz feeding it the
   right tz_str for human-local), not the shape algorithm itself (that is
   tests/test_date_human.c's job, calling sg_date_format_human directly).
   Bytes measured against real git 2.55.0 via python subprocess (argv, no
   shell), TZ=Asia/Tokyo, FIXTURE_EPOCH (2024-01-01 00:00:00 UTC). */
static void test_human_mode_end_to_end(void)
{
    with_tz("Asia/Tokyo");

    setenv("GIT_TEST_DATE_NOW", "1704326400" /* FIXTURE_EPOCH + 3 days */, 1);
    check_render("human", FIXTURE_EPOCH, FIXTURE_TZ, "Mon 00:00 +0000");
    check_render("human-local", FIXTURE_EPOCH, FIXTURE_TZ, "Mon 09:00");

    setenv("GIT_TEST_DATE_NOW", "1707523200" /* FIXTURE_EPOCH + 40 days */, 1);
    check_render("human", FIXTURE_EPOCH, FIXTURE_TZ, "Mon Jan 1 00:00");
    check_render("human-local", FIXTURE_EPOCH, FIXTURE_TZ, "Mon Jan 1 09:00");

    /* human-local's own no-offset rule needs a fixture where the resolved
       tz (local offset AT THE COMMIT'S instant) genuinely differs from the
       local offset at "now" -- Asia/Tokyo above has no DST, so the two
       coincide there and a wiring bug that hardcodes local_mode=0 at the
       sg_date_format_mode dispatch site (as opposed to test_date_human.c's
       own direct sg_date_format_human coverage of the SAME rule) would go
       unnoticed. Same DST-crossing fixture as
       tests/test_date_human.c's test_human_local_never_offset. */
    with_tz("America/New_York");
    setenv("GIT_TEST_DATE_NOW", "1699416000" /* 1699156800 + 3 days, crosses the fall-back */, 1);
    check_render("human-local", 1699156800, "-0400", "Sun 00:00");

    unsetenv("GIT_TEST_DATE_NOW");
}

int main(void)
{
    test_grammar();
    test_byte_specs_tokyo();
    test_minus_zero_normalization();
    test_dst_pair_new_york();
    test_dst_pair_kolkata();
    test_format_overflow();
    test_local_zero_offset();
    test_human_mode_end_to_end();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all date_mode tests passed\n");
    return 0;
}
