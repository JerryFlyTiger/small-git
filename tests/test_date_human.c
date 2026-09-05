#include "sg/date.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
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

/* Every case below was measured against real git 2.55.0 first (see
   PHASE67_SPEC.md sections 1-2, and CLAUDE.md's --date=human bullet), with
   `GIT_TEST_DATE_NOW` injected, `LC_ALL=C`, no config files, argv passed
   directly (never through a shell). This file re-derives none of the
   algorithm from memory -- every literal string here is a byte copied from
   an actual git invocation.

   "now"'s local offset is controlled by setting TZ and calling tzset()
   before each check (same idiom as tests/test_date_mode.c's with_tz), so
   the tests are deterministic regardless of the machine running them. */
static void with_tz(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();
}

/* A few fixed reference epochs, all UTC midnight or a stated offset from
   it -- verified against real git during measurement (a git commit-tree
   using these exact values rendered the expected calendar date). */
#define NOV14_2023_2213Z 1700000000LL /* 2023-11-14 22:13:20 UTC, a Tuesday */
#define NOV14_2023_0000Z 1699920000LL
#define OCT30_2023_0000Z 1698624000LL
#define NOV10_2023_0000Z 1699574400LL
#define DEC31_2023_0000Z 1703980800LL
#define JAN15_2023_0000Z 1673740800LL
#define DEC20_2023_0000Z 1703030400LL
#define NOV14_2023_1200Z 1699963200LL /* 2023-11-14 12:00:00 UTC */
/* 1970-06-01 12:44:30 UTC, i.e. exactly 12:00:00 local in a zone whose
   offset is -2670s, and the UTC instant of the NEXT local midnight there. */
#define SUBMIN_COMMIT     13092270LL
#define SUBMIN_MIDNIGHT   13135470LL
#define JAN20_2024_0000Z  1705708800LL

static void check_human(long long time_sec, const char *tz, long long now,
                        int local_mode, const char *expected)
{
    char out[SG_DATE_HUMAN_MAX];
    int rc = sg_date_format_human(time_sec, tz, now, local_mode, out, sizeof out);

    CHECK(rc == 0, "sg_date_format_human(%lld, \"%s\", now=%lld, local=%d) should succeed, got %d",
         time_sec, tz, now, local_mode, rc);
    if (rc != 0)
        return;
    CHECK(strcmp(out, expected) == 0,
         "time_sec=%lld tz=%s now=%lld local=%d: expected \"%s\", got \"%s\"",
         time_sec, tz, now, local_mode, expected, out);
}

/* ---- the four output shapes -------------------------------------- */

static void test_four_shapes(void)
{
    with_tz("UTC");

    /* same calendar day -> the existing relative string, verbatim
       (including "in the future"). */
    check_human(NOV14_2023_2213Z, "+0000", NOV14_2023_2213Z + 3600, 0, "60 minutes ago");
    check_human(NOV14_2023_2213Z, "+0000", NOV14_2023_2213Z, 0, "0 seconds ago");
    check_human(NOV14_2023_2213Z, "+0000", NOV14_2023_2213Z - 3600, 0, "in the future");

    /* same year+month, mday < now.mday < mday+5 -> "Www HH:MM" (measured:
       git 2.55.0 on Oct 30 vs Oct 31). */
    check_human(OCT30_2023_0000Z + 12 * 3600, "+0000", OCT30_2023_0000Z + 12 * 3600 + 86400, 0,
               "Mon 12:00");

    /* same year, anything else (here: the month changed) -> full
       "Www Mmm D HH:MM", no year (measured: the very next calendar day
       after the row above, purely because the month rolled over). */
    check_human(OCT30_2023_0000Z + 12 * 3600, "+0000",
               OCT30_2023_0000Z + 12 * 3600 + 2 * 86400, 0, "Mon Oct 30 12:00");

    /* different year (either direction) -> "Mmm D YYYY". */
    check_human(DEC31_2023_0000Z, "+0000", DEC31_2023_0000Z + 86400, 0, "Dec 31 2023");
}

/* ---- section 1's two discriminating pairs -------------------------- */

static void test_calendar_day_not_delta(void)
{
    with_tz("UTC");

    /* Nov 14 00:01 vs Nov 14 23:59 -- 23h58m apart, SAME calendar day ->
       relative ("24 hours ago", the existing rounding rule). */
    check_human(NOV14_2023_0000Z + 60, "+0000",
               NOV14_2023_0000Z + 23 * 3600 + 59 * 60, 0, "24 hours ago");

    /* Nov 14 23:59 vs Nov 15 00:01 -- 2 minutes apart, DIFFERENT calendar
       day -> "Www HH:MM" (mday 14 < 15 < 14+5). A "< 24h means relative"
       rule gets this backwards. */
    check_human(NOV14_2023_0000Z + 23 * 3600 + 59 * 60, "+0000",
               NOV14_2023_0000Z + 86400 + 60, 0, "Tue 23:59");
}

static void test_month_boundary_pair(void)
{
    with_tz("UTC");

    /* commit Oct 30, now Oct 31 -> window shape (already in test_four_shapes,
       repeated here as the paired control). */
    check_human(OCT30_2023_0000Z + 12 * 3600, "+0000", OCT30_2023_0000Z + 12 * 3600 + 86400, 0,
               "Mon 12:00");
    /* one calendar day later, now Nov 1 -> full shape, purely because the
       MONTH changed, not because the day-count grew. A "difference in
       days < 5" rule gets this backwards. */
    check_human(OCT30_2023_0000Z + 12 * 3600, "+0000",
               OCT30_2023_0000Z + 12 * 3600 + 2 * 86400, 0, "Mon Oct 30 12:00");
}

/* ---- the mday+5 boundary, exactly 4 and 5 days --------------------- */

static void test_mday_plus_5_boundary(void)
{
    with_tz("UTC");

    /* commit Nov 10, now Nov 14 (4 days later): 10+5 > 14 is true ->
       still the window shape. */
    check_human(NOV10_2023_0000Z + 12 * 3600, "+0000",
               NOV10_2023_0000Z + 12 * 3600 + 4 * 86400, 0, "Fri 12:00");
    /* now Nov 15 (5 days later): 10+5 > 15 is FALSE (the comparison is
       strictly-greater, not >=) -> falls through to the full shape. */
    check_human(NOV10_2023_0000Z + 12 * 3600, "+0000",
               NOV10_2023_0000Z + 12 * 3600 + 5 * 86400, 0, "Fri Nov 10 12:00");
}

/* ---- the offset suffix: present only on "Www HH:MM", absent on
   "Www Mmm D HH:MM" with the identical stored offset ------------------ */

static void test_offset_suffix(void)
{
    /* "now"'s local offset is UTC (0); the commit is stored +0800, so the
       two differ and the suffix must appear -- but ONLY in the window
       shape. */
    with_tz("UTC");

    check_human(NOV14_2023_0000Z + 12 * 3600, "+0800",
               NOV14_2023_0000Z + 12 * 3600 + 86400, 0, "Tue 20:00 +0800");

    /* same stored offset, same "now" zone, but far enough apart (>5 days)
       to land in the full "Www Mmm D HH:MM" shape -- no offset field at
       all, even though the offsets still differ. */
    check_human(NOV14_2023_0000Z + 12 * 3600, "+0800",
               NOV14_2023_0000Z + 12 * 3600 + 10 * 86400, 0, "Tue Nov 14 20:00");
}

static void test_offset_suffix_absent_when_equal(void)
{
    /* stored offset equals "now"'s local offset -- no suffix even in the
       window shape, since tz != L is false. */
    with_tz("UTC");
    check_human(NOV14_2023_0000Z + 12 * 3600, "+0000",
               NOV14_2023_0000Z + 12 * 3600 + 86400, 0, "Tue 12:00");
}

/* ---- human-local NEVER prints an offset, even across a DST change
   inside the 5-day window (measured: 1080 probes across 8 zones, zero
   offsets) ------------------------------------------------------------ */

static void test_human_local_never_offset(void)
{
    /* America/New_York crossed its 2023-11-05 fall-back at 2am local,
       -0400 -> -0500. A commit rendered in ITS OWN local offset just
       before the transition, viewed from just after it, is exactly the
       shape where `tz != L` would otherwise fire. */
    long local_off_at_commit;
    long local_off_at_now;
    struct tm tmv;
    char tzbuf[8];
    time_t commit_epoch = 1699156800; /* 2023-11-05 04:00:00 UTC == 00:00 EDT */
    time_t now_epoch = 1699156800 + 3 * 86400;

    with_tz("America/New_York");
    /* Resolve the commit's own local offset at its instant, the same way
       Phase 64's -local callers do (localtime_r), so this test does not
       depend on knowing New York's exact DST transition instant. */
    CHECK(localtime_r(&commit_epoch, &tmv) != NULL, "localtime_r(commit) should succeed");
    local_off_at_commit = tmv.tm_gmtoff;
    CHECK(localtime_r(&now_epoch, &tmv) != NULL, "localtime_r(now) should succeed");
    local_off_at_now = tmv.tm_gmtoff;
    /* The fixture is only meaningful if the two offsets actually differ --
       assert that precondition rather than silently passing on a machine
       whose zoneinfo disagrees. */
    CHECK(local_off_at_commit != local_off_at_now,
         "fixture precondition: commit and now offsets must differ (got %ld and %ld)",
         local_off_at_commit, local_off_at_now);

    snprintf(tzbuf, sizeof tzbuf, "%c%02ld%02ld", local_off_at_commit < 0 ? '-' : '+',
            (local_off_at_commit < 0 ? -local_off_at_commit : local_off_at_commit) / 3600,
            ((local_off_at_commit < 0 ? -local_off_at_commit : local_off_at_commit) % 3600) / 60);

    check_human((long long)commit_epoch, tzbuf, (long long)now_epoch, 1, "Sun 00:00");
}

/* ---- a future commit in each of the three non-relative shapes -------- */

static void test_future_commit(void)
{
    with_tz("UTC");

    /* future within the same month, more than one calendar day ahead ->
       full "Www Mmm D HH:MM" (a future commit is NOT "in the future"
       unless it is the same calendar day). */
    check_human(NOV14_2023_2213Z, "+0000", NOV14_2023_2213Z - 86400 - 10, 0,
               "Tue Nov 14 22:13");

    /* future in a different month, same year -> also full shape (there is
       no "window" shape for a future commit; that branch only ever fires
       when ct.mon == nt.mon and ct.mday < nt.mday). */
    check_human(DEC31_2023_0000Z + 12 * 3600, "+0000", NOV14_2023_0000Z, 0, "Sun Dec 31 12:00");

    /* future in a different year -> "Mmm D YYYY". */
    check_human(NOV14_2023_2213Z, "+0000", NOV14_2023_2213Z - 400LL * 86400, 0, "Nov 14 2023");
}

/* ---- year-boundary pair from section 1 (kept as its own named check,
   distinct from test_four_shapes' single year-boundary row) ----------- */

static void test_year_boundary_vs_within_year(void)
{
    with_tz("UTC");

    check_human(DEC31_2023_0000Z, "+0000", DEC31_2023_0000Z + 86400, 0, "Dec 31 2023");
    check_human(JAN15_2023_0000Z + 12 * 3600, "+0000", DEC20_2023_0000Z, 0, "Sun Jan 15 12:00");
}

/* ---- "now" is taken in the MACHINE'S LOCAL zone, never in the commit's own
   render offset --------------------------------------------------------

   This is the one rule of --date=human that has no analogue anywhere else in
   date.c: the commit is rendered in `tz` while the calendar date it is
   COMPARED against comes from localtime.  Every other row in this file was
   blind to it, because they all run under a TZ whose offset equals the
   commit's stored one, where the two readings of "now" agree by
   construction -- a mutation swapping localtime for `tz` at that call site
   left this whole file green and turned 10 interop checks red instead.

   One commit and one `now`, read from three zones, give three DIFFERENT
   answers, and each is a byte copy of real git 2.55.0's output:
   the commit is 2023-11-14 12:00:00Z stored +0000, `now` is 13 hours later
   (2023-11-15 01:00:00Z).  Under UTC and Tokyo `now` has already rolled over
   to Nov 15, so the shape is the 5-day-window one; under New York it is
   still Nov 14 locally, so the SAME inputs are the same calendar day and
   render as a relative duration instead.  Nothing but the local zone
   distinguishes the three. */
static void test_now_is_read_in_the_local_zone(void)
{
    with_tz("UTC");
    check_human(NOV14_2023_1200Z, "+0000", NOV14_2023_1200Z + 13 * 3600, 0,
                "Tue 12:00");

    /* +0900: `now` is Nov 15 locally, and the stored +0000 differs from the
       local offset, so this arm also carries the suffix. */
    with_tz("Asia/Tokyo");
    check_human(NOV14_2023_1200Z, "+0000", NOV14_2023_1200Z + 13 * 3600, 0,
                "Tue 12:00 +0000");
    /* human-local renders the commit at +0900 as well; still Nov 15 for
       `now`, so the window shape, and a -local mode never shows an offset. */
    check_human(NOV14_2023_1200Z, "+0900", NOV14_2023_1200Z + 13 * 3600, 1,
                "Tue 21:00");

    /* -0500: `now` is STILL Nov 14 locally -- same calendar day, so the
       relative branch, from inputs identical to the two arms above. */
    with_tz("America/New_York");
    check_human(NOV14_2023_1200Z, "+0000", NOV14_2023_1200Z + 13 * 3600, 0,
                "13 hours ago");

    with_tz("UTC");
}

/* ---- "now" must be shifted by the EXACT local offset -----------------

   Found by review after all four gates were green. The first implementation
   computed "now"'s calendar date by formatting localtime_r's offset into a
   "+HHMM" string and shifting by that -- which truncates the SECONDS of a
   zone whose offset is not a whole number of minutes, and human's output is
   chosen by a calendar-day comparison, so the error is not a wrong digit
   but a wrong SHAPE. Measured against real git in the 30 seconds before
   local midnight: git `12 hours ago`, sg `Mon 12:44 +0000`.

   "XXX0:44:30" is a POSIX TZ string, deliberately NOT a zoneinfo name: it
   is a -2670s offset on any POSIX system, needs no tz database, and so
   cannot be quietly skipped the way an Africa/Monrovia fixture can on a
   machine whose zoneinfo lacks the historical rule. Verified in both tools
   before being written down here. */
static void test_now_uses_the_exact_local_offset(void)
{
    with_tz("XXX0:44:30");

    /* One second before local midnight: still the same local day. */
    check_human(SUBMIN_COMMIT, "+0000", SUBMIN_MIDNIGHT - 1, 0, "12 hours ago");
    check_human(SUBMIN_COMMIT, "-0044", SUBMIN_MIDNIGHT - 1, 1, "12 hours ago");
    /* Exactly at it: the day has rolled over, for both modes. */
    check_human(SUBMIN_COMMIT, "+0000", SUBMIN_MIDNIGHT, 0, "Mon 12:44 +0000");
    check_human(SUBMIN_COMMIT, "-0044", SUBMIN_MIDNIGHT, 1, "Mon 12:00");

    with_tz("UTC");
}

/* ---- the suffix ECHOES the stored offset string ----------------------

   Also found by that review. A commit object may legally carry an offset
   whose minute field is >= 60; git echoes it unchanged, exactly as sg's own
   --date=iso already did, while re-deriving it from seconds rewrites it
   ("+0165" -> "+0205"). The same string, not the duration, is also what
   decides whether the suffix appears at all: a stored "+0060" and a local
   "+0100" are the same 3600 seconds and git still prints it.

   "XXX-1" is a POSIX TZ string for +0100, chosen for the same
   no-tz-database reason as the zone above. Every expected string here is a
   byte copy of real git 2.55.0's output. */
static void test_offset_suffix_echoes_the_stored_string(void)
{
    long long now = JAN20_2024_0000Z + 2 * 86400;

    with_tz("XXX-1");

    /* Same seconds as the local zone, different encoding: still printed. */
    check_human(JAN20_2024_0000Z, "+0060", now, 0, "Sat 01:00 +0060");
    /* The canonical spelling of that same offset: suppressed. This pair is
       the control -- either one alone is satisfied by the wrong rule. */
    check_human(JAN20_2024_0000Z, "+0100", now, 0, "Sat 01:00");
    /* A minute field over 59 must survive verbatim, not be carried. */
    check_human(JAN20_2024_0000Z, "+0165", now, 0, "Sat 02:05 +0165");

    with_tz("UTC");
}

int main(void)
{
    test_four_shapes();
    test_calendar_day_not_delta();
    test_month_boundary_pair();
    test_mday_plus_5_boundary();
    test_offset_suffix();
    test_offset_suffix_absent_when_equal();
    test_human_local_never_offset();
    test_future_commit();
    test_year_boundary_vs_within_year();
    test_now_is_read_in_the_local_zone();
    test_now_uses_the_exact_local_offset();
    test_offset_suffix_echoes_the_stored_string();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
