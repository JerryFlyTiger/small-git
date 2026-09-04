#include "sg/date.h"

#include <limits.h>
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

/* Every row below was measured against real git 2.55.0 (see
   PHASE66_SPEC.md section 1, and CLAUDE.md's --date= entry) --
   sg_date_format_relative(0, diff, ...) is exactly relative(diff) from the
   spec's own reference implementation, since time_sec == 0 makes
   `now - time_sec == now == diff`. Exact byte comparisons throughout, no
   strstr. */
static void check_relative(long long diff, const char *expected)
{
    char out[SG_DATE_RELATIVE_MAX];
    int rc = sg_date_format_relative(0, diff, out, sizeof out);

    CHECK(rc == 0, "sg_date_format_relative(diff=%lld) should succeed, got rc=%d", diff, rc);
    CHECK(strcmp(out, expected) == 0, "diff=%lld: expected \"%s\", got \"%s\"", diff, expected, out);
}

/* ---- section 1: the full boundary table, exact bytes -------------- */

static void test_boundary_table(void)
{
    /* seconds->minutes boundary at 90 (bisected exact second). */
    check_relative(0, "0 seconds ago");
    check_relative(1, "1 second ago");
    check_relative(89, "89 seconds ago");
    check_relative(90, "2 minutes ago");
    check_relative(91, "2 minutes ago");

    /* minutes->hours boundary at 5370. */
    check_relative(5369, "89 minutes ago");
    check_relative(5370, "2 hours ago");
    check_relative(5371, "2 hours ago");

    /* hours->days boundary at 127770. */
    check_relative(127769, "35 hours ago");
    check_relative(127770, "2 days ago");
    check_relative(127771, "2 days ago");

    /* days->weeks boundary at 1164570. */
    check_relative(1164569, "13 days ago");
    check_relative(1164570, "2 weeks ago");
    check_relative(1164571, "2 weeks ago");

    /* weeks->months boundary at 6002970. */
    check_relative(6002969, "10 weeks ago");
    check_relative(6002970, "2 months ago");
    check_relative(6002971, "2 months ago");

    /* months->years boundary at 31490970. */
    check_relative(31490969, "12 months ago");
    check_relative(31490970, "1 year ago");
    check_relative(31490971, "1 year ago");

    /* "1 year ago" -> "1 year, 1 month ago" boundary at 32873370. */
    check_relative(32873369, "1 year ago");
    check_relative(32873370, "1 year, 1 month ago");
    check_relative(32873371, "1 year, 1 month ago");
}

/* ---- the discriminating witness for the TWO month formulas --------
   Spec section 1's own WARNING: using `totalmonths` (the >=365-day
   formula) below 365 days too matched git on 538/539 probes -- the one
   miss was delta=6476307 (75 days), where totalmonths says "2 months" and
   git (and the correct `(days+15)/30` formula) says "3 months". Measured
   directly against real git 2.55.0 as part of this phase. */
static void test_two_month_formulas_witness(void)
{
    check_relative(6476307, "3 months ago");
}

/* ---- the LLONG_MIN/LLONG_MAX saturating-clamp branches in
   sg_date_format_relative itself, not relative_diff_to_words --
   test_boundary_table above only ever calls sg_date_format_relative(0,
   diff, ...), so `diff` is always exactly the caller-supplied value and
   NEITHER of the two saturating `if`/`else if` branches (nor the
   post-subtraction re-clamp inside the final `else`) is ever taken --
   every one of those rows exercises relative_diff_to_words alone. These
   three calls drive time_sec/now combinations that hit each arithmetic
   path in sg_date_format_relative directly:
     - time_sec = LLONG_MAX, now = LLONG_MIN: `time_sec > 0 && now <
       LLONG_MIN + time_sec` (the first branch) fires, clamping to
       LLONG_MIN/4 -- a huge negative diff, rendered as "in the future"
       regardless of magnitude.
     - time_sec = LLONG_MIN, now = LLONG_MAX: the `else if` branch fires,
       clamping to LLONG_MAX/4 -- a huge positive diff. Verified against an
       independent Python re-derivation of relative_diff_to_words's own
       algorithm (division truncation matches C's for positive operands):
       diff = LLONG_MAX/4 = 2305843009213693951 renders "73117802169 years
       ago".
     - time_sec = LLONG_MAX, now = 0: neither saturating branch fires
       (time_sec > 0 but now = 0 is NOT < LLONG_MIN + LLONG_MAX = -1), so
       diff = now - time_sec = -LLONG_MAX computes without overflow, then
       hits the innermost `else` block's own re-clamp
       (`diff < LLONG_MIN/4`), landing on the same LLONG_MIN/4 as the first
       branch -- "in the future". This is the one path that reaches
       relative_diff_to_words WITHOUT going through either saturating
       `if`/`else if` at the top, so it needs its own witness.
   Each of these calls with a raw diff (not run through the clamp) would be
   signed-integer-overflow UB, catchable only under ASan/UBSan (`make
   sanitize`) -- a plain `make test` mutation that deletes the clamp and
   computes `diff = now - time_sec` unconditionally will not go red under
   an ordinary build. */
static void test_overflow_clamp(void)
{
    char out[SG_DATE_RELATIVE_MAX];
    int rc;

    rc = sg_date_format_relative(LLONG_MAX, LLONG_MIN, out, sizeof out);
    CHECK(rc == 0, "sg_date_format_relative(LLONG_MAX, LLONG_MIN) should succeed, got rc=%d", rc);
    CHECK(strcmp(out, "in the future") == 0,
         "sg_date_format_relative(LLONG_MAX, LLONG_MIN): expected \"in the future\", got \"%s\"", out);

    rc = sg_date_format_relative(LLONG_MIN, LLONG_MAX, out, sizeof out);
    CHECK(rc == 0, "sg_date_format_relative(LLONG_MIN, LLONG_MAX) should succeed, got rc=%d", rc);
    CHECK(strcmp(out, "73117802169 years ago") == 0,
         "sg_date_format_relative(LLONG_MIN, LLONG_MAX): expected \"73117802169 years ago\", got \"%s\"", out);

    rc = sg_date_format_relative(LLONG_MAX, 0, out, sizeof out);
    CHECK(rc == 0, "sg_date_format_relative(LLONG_MAX, 0) should succeed, got rc=%d", rc);
    CHECK(strcmp(out, "in the future") == 0,
         "sg_date_format_relative(LLONG_MAX, 0): expected \"in the future\", got \"%s\"", out);
}

/* ---- a commit in the future prints the literal, no number, no "ago" -- */
static void test_future(void)
{
    check_relative(-1, "in the future");
    check_relative(-60, "in the future");
    check_relative(-3600, "in the future");
    check_relative(-400LL * 86400, "in the future");
}

/* ---- a handful of mid-range, non-boundary values, so a mutation that
   only breaks the FIRST row of a unit's branch cannot hide behind these
   boundary-only checks. ---- */
static void test_mid_range(void)
{
    check_relative(50, "50 seconds ago");
    check_relative(300, "5 minutes ago");
    check_relative(36000, "10 hours ago");
    check_relative(5LL * 86400, "5 days ago");
    check_relative(21LL * 86400, "3 weeks ago");
    check_relative(15552000, "6 months ago");
    check_relative(70956000, "2 years, 3 months ago");
    check_relative(189216000, "6 years ago");
}

/* ---- relative-local is byte-identical to relative, in every zone ----
   (CLAUDE.md's --date= entry: "the answer is a duration, so the tz has
   nothing to change"). Goes through sg_date_parse_mode + sg_date_format_
   mode -- the actual dispatch path -- rather than calling
   sg_date_format_relative directly, so it also proves the RELATIVE case
   bypasses tz resolution (sg_date_format_mode's own SG_DATE_RELATIVE
   fast path, ahead of resolve_mode_tz) cleanly under a real TZ. */
static void test_relative_local_matches_relative_every_zone(void)
{
    static const char *const ZONES[] = { "UTC", "Asia/Tokyo", "America/New_York" };
    size_t i;
    sg_date_mode plain, local;
    char out_plain[SG_DATE_RELATIVE_MAX];
    char out_local[SG_DATE_RELATIVE_MAX];
    long long saved_now;
    char now_buf[32];

    CHECK(sg_date_parse_mode("relative", &plain) == 0, "\"relative\" should parse");
    CHECK(plain.kind == SG_DATE_RELATIVE && plain.local == 0, "\"relative\": wrong kind/local");
    CHECK(sg_date_parse_mode("relative-local", &local) == 0, "\"relative-local\" should parse");
    CHECK(local.kind == SG_DATE_RELATIVE && local.local == 1, "\"relative-local\": wrong kind/local");

    /* Fix GIT_TEST_DATE_NOW so the comparison is deterministic across the
       zone loop (sg_date_now() re-reads it every call, see its own header
       comment in date.h for why that is safe here). */
    saved_now = 32873370; /* the "1 year, 1 month ago" boundary, arbitrary but non-trivial */
    snprintf(now_buf, sizeof now_buf, "%lld", saved_now);
    setenv("GIT_TEST_DATE_NOW", now_buf, 1);

    for (i = 0; i < sizeof(ZONES) / sizeof(ZONES[0]); i++) {
        setenv("TZ", ZONES[i], 1);
        tzset();

        CHECK(sg_date_format_mode(&plain, 0, "+0000", out_plain, sizeof out_plain) == 0,
             "relative under TZ=%s should render", ZONES[i]);
        CHECK(sg_date_format_mode(&local, 0, "+0000", out_local, sizeof out_local) == 0,
             "relative-local under TZ=%s should render", ZONES[i]);
        CHECK(strcmp(out_plain, out_local) == 0,
             "TZ=%s: relative (\"%s\") != relative-local (\"%s\")", ZONES[i], out_plain, out_local);
        CHECK(strcmp(out_plain, "1 year, 1 month ago") == 0,
             "TZ=%s: relative should still read \"1 year, 1 month ago\", got \"%s\"",
             ZONES[i], out_plain);
    }

    unsetenv("GIT_TEST_DATE_NOW");
    unsetenv("TZ");
    tzset();
}

/* ---- sg_date_now(): GIT_TEST_DATE_NOW is honoured, and a malformed value
   falls back to the real clock rather than being parsed the way git's own
   test-hook unsigned-wraparound does (CLAUDE.md's --date= entry: sg
   deliberately does NOT reproduce "abc"/""->0, "1700000000x"->numeric
   prefix, "-5"->584942417301 years ago). ---- */
static void test_sg_date_now(void)
{
    long long before, now, after;

    setenv("GIT_TEST_DATE_NOW", "1700000000", 1);
    CHECK(sg_date_now() == 1700000000, "GIT_TEST_DATE_NOW=1700000000 should be honoured, got %lld",
         sg_date_now());

    setenv("GIT_TEST_DATE_NOW", "-5", 1);
    CHECK(sg_date_now() == -5, "GIT_TEST_DATE_NOW=-5 should parse as a strict signed decimal, got %lld",
         sg_date_now());

    /* Malformed values fall back to the real clock -- bracket the call with
       time(NULL) samples rather than asserting an exact value. */
    unsetenv("GIT_TEST_DATE_NOW");
    before = (long long)time(NULL);
    now = sg_date_now();
    after = (long long)time(NULL);
    CHECK(now >= before && now <= after, "unset GIT_TEST_DATE_NOW should fall back to the real clock");

    setenv("GIT_TEST_DATE_NOW", "abc", 1);
    before = (long long)time(NULL);
    now = sg_date_now();
    after = (long long)time(NULL);
    CHECK(now >= before && now <= after, "GIT_TEST_DATE_NOW=\"abc\" should fall back to the real clock (not 0)");

    setenv("GIT_TEST_DATE_NOW", "", 1);
    before = (long long)time(NULL);
    now = sg_date_now();
    after = (long long)time(NULL);
    CHECK(now >= before && now <= after, "GIT_TEST_DATE_NOW=\"\" should fall back to the real clock (not 0)");

    setenv("GIT_TEST_DATE_NOW", "1700000000x", 1);
    before = (long long)time(NULL);
    now = sg_date_now();
    after = (long long)time(NULL);
    CHECK(now >= before && now <= after,
         "GIT_TEST_DATE_NOW=\"1700000000x\" should fall back to the real clock (not the numeric prefix)");

    unsetenv("GIT_TEST_DATE_NOW");
}

int main(void)
{
    test_boundary_table();
    test_two_month_formulas_witness();
    test_overflow_clamp();
    test_future();
    test_mid_range();
    test_relative_local_matches_relative_every_zone();
    test_sg_date_now();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all date_relative tests passed\n");
    return 0;
}
