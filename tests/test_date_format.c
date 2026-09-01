/* sg_date_format_normal: git's DATE_NORMAL rendering.
 *
 * Every expectation in the measured table below came from real git 2.55.0
 * (`git log -1 --date=default --format=%ad` on a commit created with an
 * explicit GIT_AUTHOR_DATE), never from sg. The bug this function exists to
 * fix was invisible for exactly as long as nothing compared the two: sg used
 * to render the epoch as UTC while printing the stored offset next to it, so
 * every timestamp was wrong by that offset and contradicted itself.
 */
#include "sg/date.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d ", __FILE__, __LINE__);                        \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

struct measured_case {
    long long time_sec;
    const char *tz;
    const char *expect;
    const char *why;
};

static const struct measured_case CASES[] = {
    {1700000000LL, "+0000", "Tue Nov 14 22:13:20 2023 +0000", "UTC baseline"},
    {1700000000LL, "+0800", "Wed Nov 15 06:13:20 2023 +0800", "positive offset rolls the day forward"},
    {1700000000LL, "-0500", "Tue Nov 14 17:13:20 2023 -0500", "negative offset subtracts"},
    {1700000000LL, "+0530", "Wed Nov 15 03:43:20 2023 +0530", "half-hour offset"},
    {1700000000LL, "-1100", "Tue Nov 14 11:13:20 2023 -1100", "large negative offset"},
    {0LL,          "+0000", "Thu Jan 1 00:00:00 1970 +0000",  "the epoch itself, single-digit day"},
    {0LL,          "+0100", "Thu Jan 1 01:00:00 1970 +0100",  "offset applied at the epoch"},
    {3600LL,       "-0100", "Thu Jan 1 00:00:00 1970 -0100",  "shifts exactly onto zero"},
    {1788258269LL, "+0800", "Tue Sep 1 18:24:29 2026 +0800",  "the case that first exposed the bug"},
    {1046547722LL, "+0100", "Sat Mar 1 20:42:02 2003 +0100",  "single-digit day, non-UTC"},
    {2147483647LL, "+0000", "Tue Jan 19 03:14:07 2038 +0000", "32-bit time_t boundary"},
    {1699920000LL, "+0800", "Tue Nov 14 08:00:00 2023 +0800", "midnight UTC plus eight"},
    {1725148800LL, "+0000", "Sun Sep 1 00:00:00 2024 +0000",  "day 1 is not padded to 01"},
    {1735689599LL, "+0100", "Wed Jan 1 00:59:59 2025 +0100",  "offset rolls across the year"},
    {1751328000LL, "-0700", "Mon Jun 30 17:00:00 2025 -0700", "negative offset rolls back a day"},
};

int main(void)
{
    char buf[SG_DATE_NORMAL_MAX];
    size_t i;

    for (i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        const struct measured_case *c = &CASES[i];

        buf[0] = 'X';
        CHECK(sg_date_format_normal(c->time_sec, c->tz, buf, sizeof(buf)) == 0,
              "case %zu (%s) returned non-zero", i, c->why);
        CHECK(strcmp(buf, c->expect) == 0,
              "case %zu (%s): got \"%s\", real git says \"%s\"",
              i, c->why, buf, c->expect);
    }

    /* The offset is echoed back, never recomputed: the same instant rendered
       under two offsets differs in BOTH the clock and the printed suffix. A
       formatter that ignored the offset would still pass every single-case
       assertion above if it happened to be written for UTC, so this pair is
       what makes "wrong by the offset" observable as a difference. */
    {
        char utc[SG_DATE_NORMAL_MAX], plus8[SG_DATE_NORMAL_MAX];

        CHECK(sg_date_format_normal(1700000000LL, "+0000", utc, sizeof(utc)) == 0, "utc");
        CHECK(sg_date_format_normal(1700000000LL, "+0800", plus8, sizeof(plus8)) == 0, "+0800");
        CHECK(strcmp(utc, plus8) != 0, "the same instant rendered identically under two offsets");
    }

    /* Not a git comparison -- sg's own choices for input git would not have
       written. A malformed offset shifts by nothing and is still echoed
       verbatim, because the ident line is stored bytes and this function is
       not the place that decides they are wrong. */
    CHECK(sg_date_format_normal(1700000000LL, "+08", buf, sizeof(buf)) == 0, "short tz rejected outright");
    CHECK(strcmp(buf, "Tue Nov 14 22:13:20 2023 +08") == 0, "short tz: got \"%s\"", buf);
    CHECK(sg_date_format_normal(1700000000LL, "abcde", buf, sizeof(buf)) == 0, "non-numeric tz rejected outright");
    CHECK(strcmp(buf, "Tue Nov 14 22:13:20 2023 abcde") == 0, "non-numeric tz: got \"%s\"", buf);
    CHECK(sg_date_format_normal(1700000000LL, NULL, buf, sizeof(buf)) == 0, "NULL tz");
    CHECK(strcmp(buf, "Tue Nov 14 22:13:20 2023 +0000") == 0, "NULL tz: got \"%s\"", buf);

    /* Failure leaves an empty string, not a truncated date: the caller
       prints the buffer either way, and half a timestamp is worse than none. */
    {
        char tiny[10];

        CHECK(sg_date_format_normal(1700000000LL, "+0000", tiny, sizeof(tiny)) == -1,
              "a buffer too small must fail");
        CHECK(tiny[0] == '\0', "a failed format must leave the buffer empty, got \"%s\"", tiny);
        CHECK(sg_date_format_normal(1700000000LL, "+0000", buf, 0) == -1, "zero-size buffer");
    }

    if (failures > 0) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("test_date_format: all checks passed\n");
    return 0;
}
