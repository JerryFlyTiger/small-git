#include "sg/ident.h"

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

/* CLAUDE.md's Phase 72 section carries the derivation for every row here;
   this file exists to pin them, not to re-derive them. */

static void expect_ok(const char *raw, long long want_time, const char *want_tz)
{
    long long t;
    char tz[8];
    int rc = sg_ident_parse_date(raw, &t, tz);

    CHECK(rc == 0, "expected ok for \"%s\", got rc=%d", raw, rc);
    if (rc == 0) {
        CHECK(t == want_time, "\"%s\": got time %lld want %lld", raw, t, want_time);
        CHECK(strcmp(tz, want_tz) == 0, "\"%s\": got tz %s want %s", raw, tz, want_tz);
    }
}

/* Same as expect_ok, but the caller does not know (or care) what the
   machine's local offset resolves to -- only that the timestamp itself
   was honoured. `reject_tz`, if non-NULL, is the malformed/out-of-range
   offset text that MUST NOT be echoed back verbatim -- without this check,
   a mutation that disabled the numeric range validation entirely (so
   "+2400" is treated as a well-formed, in-range offset instead of being
   discarded) is invisible to this function: the resulting TIMESTAMP is
   identical either way, since the offset never feeds the epoch computed
   from the epoch form. Only the tz STRING tells the two apart. */
static void expect_ok_local_time(const char *raw, long long want_time, const char *reject_tz)
{
    long long t;
    char tz[8];
    int rc = sg_ident_parse_date(raw, &t, tz);

    CHECK(rc == 0, "expected ok(local) for \"%s\", got rc=%d", raw, rc);
    if (rc == 0) {
        CHECK(t == want_time, "\"%s\": got time %lld want %lld", raw, t, want_time);
        if (reject_tz != NULL)
            CHECK(strcmp(tz, reject_tz) != 0,
                 "\"%s\": offset should have been discarded, but got it back verbatim: %s", raw,
                 tz);
    }
}

static void expect_fail(const char *raw)
{
    long long t;
    char tz[8];
    int rc = sg_ident_parse_date(raw, &t, tz);

    CHECK(rc == -1, "expected fail for \"%s\" but got rc=%d", raw, rc);
}

/* Section 2.2: accepted forms and the timestamp/timezone they resolve to.
   Every accepted row here uses an offset explicit enough that the result
   does not depend on the machine's own local zone; the local-offset rows
   are exercised separately in test_local_offset_is_the_instants below. */
static void test_2_2_accepted_and_rejected_forms(void)
{
    expect_ok("1700000000 +0800", 1700000000, "+0800");
    expect_ok("@1700000000 +0800", 1700000000, "+0800");
    expect_ok_local_time("@1700000000", 1700000000, NULL);
    expect_ok_local_time("1700000000", 1700000000, NULL);
    expect_ok("2023-11-15T06:13:20+08:00", 1700000000, "+0800");
    expect_ok("2023-11-15 06:13:20 +0800", 1700000000, "+0800");
    expect_ok("Wed, 15 Nov 2023 06:13:20 +0800", 1700000000, "+0800");

    expect_fail("2023-11-15");
    expect_fail("yesterday");
    expect_fail("2 hours ago");
    expect_fail("now");
    expect_fail("garbage");
    expect_fail("-5");
    expect_fail("99999999999999999999");
}

/* Section 2.3: the offset field is exactly sign + 4 digits, hours <= 23,
   minutes <= 59 -- anything else means "ignore the offset, use local",
   never a hard failure, and the numeric range is NOT the real-world
   +/-14:00 bound (measured: +1500/+2359 are accepted verbatim). */
static void test_2_3_offset_range(void)
{
    expect_ok("1700000000 +0000", 1700000000, "+0000");
    expect_ok("1700000000 +0800", 1700000000, "+0800");
    expect_ok("1700000000 +1400", 1700000000, "+1400");
    expect_ok("1700000000 +1500", 1700000000, "+1500");
    expect_ok("1700000000 +2359", 1700000000, "+2359");
    expect_ok("1700000000 -1400", 1700000000, "-1400");
    expect_ok("1700000000 -2359", 1700000000, "-2359");

    expect_ok_local_time("1700000000 +2400", 1700000000, "+2400");
    expect_ok_local_time("1700000000 -2400", 1700000000, "-2400");
    expect_ok_local_time("1700000000 +2360", 1700000000, "+2360");
    expect_ok_local_time("1700000000 +1260", 1700000000, "+1260");
    expect_ok_local_time("1700000000 +0860", 1700000000, "+0860");
    expect_ok_local_time("1700000000 +9999", 1700000000, "+9999");
    expect_ok_local_time("1700000000 +08", 1700000000, NULL);
    expect_ok_local_time("1700000000 +080", 1700000000, NULL);
    expect_ok_local_time("1700000000 +08000", 1700000000, NULL);
    expect_ok_local_time("1700000000 0800", 1700000000, NULL);
}

/* SPEC-CORRECTION.md: section 2.3 above was measured on the BARE
   `<digits> <offset>` form only. The `@<digits> <offset>` form is
   different -- git normalizes the offset ARITHMETICALLY instead of
   discarding an out-of-range one, and this is the one place a divergence
   is SILENT (both tools exit 0, the bytes differ), so it is the one this
   round exists to close. Rows below are the exact table from
   SPEC-CORRECTION.md's "Offset normalization, @ form only" section. */
static void test_2_3_correction_at_form_offset_normalization(void)
{
    expect_ok("@1700000000 +0000", 1700000000, "+0000");
    expect_ok("@1700000000 0100", 1700000000, "+0100");
    expect_ok("@1700000000 +0060", 1700000000, "+0100");
    expect_ok("@1700000000 +00:00", 1700000000, "+0000");
    expect_ok("@1700000000 +0099", 1700000000, "+0139");
    expect_ok("@1700000000 +2400", 1700000000, "+2400");
    expect_ok("@1700000000 +9999", 1700000000, "+10039");
    expect_ok("@1700000000 -9999", 1700000000, "-10039");
    /* Anything that does not match the loose shape (optional sign + exactly
       4 digits, or +HH:MM) is not an offset at all for THIS form either --
       falls back to local, same as the "no valid offset" column. */
    expect_ok_local_time("@1700000000 +0", 1700000000, NULL);
    expect_ok_local_time("@1700000000 +060", 1700000000, NULL);
    expect_ok_local_time("@1700000000 +00000", 1700000000, NULL);
    expect_ok_local_time("@1700000000 abc", 1700000000, NULL);
}

/* SPEC-CORRECTION.md's ">= 9 digit" rule: a timestamp with NO valid
   explicit offset needs at least 9 digits, on EITHER form -- the one
   exemption is `@<digits>` WITH a shape-valid offset (tested above), which
   has no digit floor at all. */
static void test_2_3_correction_min_digit_count(void)
{
    expect_fail("99999999");
    expect_ok_local_time("100000000", 100000000, NULL);
    expect_fail("@99999999");
    expect_ok_local_time("@100000000", 100000000, NULL);
    /* the bare form's floor applies EVEN WITH a valid offset attached --
       unlike the `@` form, a bare timestamp is never exempted. */
    expect_fail("99999999 +0000");
    expect_ok("@99999999 +0000", 99999999, "+0000");
    expect_fail("@0");
    expect_ok("@0 +0000", 0, "+0000");
}

/* SPEC-CORRECTION.md's upper bound: every form except `@<digits>` WITH a
   valid explicit offset must reject a timestamp at or after
   2100-01-01T00:00:00Z (epoch 4102444800). */
static void test_2_3_correction_upper_bound(void)
{
    expect_ok("4102444799 +0000", 4102444799, "+0000");
    expect_fail("4102444800 +0000");
    expect_ok("@4102444800 +0000", 4102444800, "+0000");
    expect_fail("@4102444800");
    expect_fail("2100-01-01T00:00:00+00:00");
    expect_fail("2100-01-01 00:00:00 +0000");
    expect_fail("Sat, 1 Jan 2100 00:00:00 +0000");
}

/* Round 3: the pre-1970 lower bound. SPEC-CORRECTION.md's own first pass
   said this "needs no work" because sg already rejected
   "1969-12-31T23:59:59+00:00" -- true, but for the WRONG reason: that value
   converts to epoch -1, which collides with timegm's own error sentinel
   `(time_t)-1` in calendar_to_epoch, so it was rejected BY ACCIDENT. Every
   OTHER pre-1970 instant sailed straight through and got written as a real
   object with a negative author/committer time. Measured directly against
   real git via `git commit-tree`: every one of these is REJECTED on every
   calendar form, and the boundary is the computed epoch RESULT (>= 0
   accepted), not the year field -- a non-UTC offset can still push an
   ostensibly-1970 wall clock across the line, which the last two rows below
   exercise. The epoch/`@<digits>` forms need no such check: their digit
   scanner only accepts unsigned digits, so a negative epoch is structurally
   unreachable there, matching git's own rejection of "@-5"/"-1". */
static void test_2_3_correction_lower_bound(void)
{
    expect_fail("1969-12-31T23:59:59+00:00");
    expect_fail("1969-12-31T23:59:58+00:00");
    expect_fail("1969-12-31 23:59:58 +0000");
    expect_fail("Wed, 31 Dec 1969 23:59:58 +0000");
    expect_fail("1950-06-15T12:00:00+00:00");
    expect_fail("1900-01-01T00:00:00+00:00");
    expect_fail("@-1");
    expect_fail("@-5");
    expect_fail("-1");

    expect_ok("1970-01-01T00:00:00+00:00", 0, "+0000");
    expect_ok("1970-01-01 00:00:00 +0000", 0, "+0000");
    expect_ok("Thu, 1 Jan 1970 00:00:00 +0000", 0, "+0000");
    /* Same wall-clock DIGITS ("1970-01-01T08:00:00"), but the +08:00 offset
       shifts the computed UTC instant to exactly epoch 0 -- accepted; one
       second earlier, at the same offset, crosses back below zero and is
       rejected. This pair proves the bound is checked on a computed
       RESULT, not the year text -- but round 4 (below) found it does NOT,
       by itself, prove WHICH result: here the raw calendar reading
       (1970-01-01 08:00 UTC / 1969-12-31 23:59:59 UTC) and the final,
       offset-adjusted instant point the SAME direction (both in-range /
       both out-of-range), so a version of this parser that checked only
       one of the two would still pass this exact pair. It is a genuine
       "control whose two arms agree by construction" -- kept for what it
       DOES prove, not relied on for what it cannot. */
    expect_ok("1970-01-01T08:00:00+08:00", 0, "+0800");
    expect_fail("1969-12-31T23:59:59+08:00");
}

/* Round 4: git rejects a calendar date if EITHER the RAW reading (the wall
   clock as if it were already UTC) OR the offset-adjusted FINAL instant
   falls outside [0, 4102444800) -- not just the final one. Measured
   directly against `git commit-tree`: each row below has a raw reading
   outside the bound but a final instant the offset pulls back INSIDE it,
   and git still rejects every one. Round 3's fix (test_2_3_correction_lower_bound
   above) checked only the final value, so it missed exactly this direction
   -- letting sg accept a date real git refuses, rule 2 of the scope line.
   The reverse direction (raw inside, final outside) was ALREADY correctly
   rejected by the final-value check alone; the two controls at the end
   confirm that direction is still covered, so the rule reads as genuinely
   two-sided rather than "replaced by" the new one. */
static void test_2_3_correction_raw_vs_final_bound(void)
{
    /* raw out of range, final pulled back in range -- must still REJECT */
    expect_fail("1969-12-31T23:30:00-01:00");
    expect_fail("1969-12-31T23:50:00-01:00"); /* a real Azores offset */
    expect_fail("1969-12-31 23:30:00 -0100");
    expect_fail("Wed, 31 Dec 1969 23:30:00 -0100");
    expect_fail("2100-01-01T00:00:30+00:01");
    expect_fail("2100-01-01T00:04:00+09:00"); /* a real Tokyo offset */

    /* controls: raw in range, final pulled OUT of range -- already
       rejected before this round, must stay rejected */
    expect_fail("1970-01-01T00:00:00+00:01");
    expect_fail("2099-12-31T23:59:00-01:01");
}

/* Round 5, finding 1 (+ finding 2, which the same rule closes): a calendar
   form (any of the three) carrying a shape-valid but numerically
   OUT-OF-RANGE offset is now REFUSED outright in every form, uniformly.
   This used to be a SILENT divergence: sg discarded the offset and read
   the wall clock as literal UTC, git re-reads it as LOCAL time -- both
   exit 0, different epoch, different object id, on an utterly ordinary
   date ("2023-11-15 06:13:20 +9999"), not a boundary case. Reproducing
   git's actual "reinterpret as local" behaviour is deliberately NOT done:
   it is ill-defined during a DST gap and ambiguous during a DST overlap, a
   whole new class of corner cases to gain fidelity on an input nobody
   sensible writes (a real UTC offset never exceeds +/-14:00). Refusing is
   rule 3 of the scope line -- loud, and a refusal can never write a wrong
   object. try_iso_t already refused an out-of-range COLON offset before
   this round, by accident (see match_offset_token_colon's own comment);
   it is now refused on purpose, consistently with its two siblings, which
   is what makes finding 2 (try_iso_t alone disagreeing with the other two)
   disappear as an inconsistency rather than needing a separate fix. Each
   form also gets an IN-RANGE control proving this is a refusal of the
   specific out-of-range case, not a refusal of every offset. */
static void test_round5_calendar_offset_out_of_range(void)
{
    /* finding 1: an ordinary date, shape-valid absurd offset */
    expect_fail("2023-11-15 06:13:20 +9999");
    expect_fail("Thu, 1 Jan 1970 00:00:00 +9999");
    /* finding 2: the colon form specifically */
    expect_fail("2023-11-15T06:13:20+99:00");

    /* in-range controls, one per form -- must still work */
    expect_ok("2023-11-15T06:13:20+08:00", 1700000000, "+0800");
    expect_ok("2023-11-15 06:13:20 +0800", 1700000000, "+0800");
    expect_ok("Wed, 15 Nov 2023 06:13:20 +0800", 1700000000, "+0800");
}

/* Round 5, finding 3: a leap second (ss == 60) crossing the 1970 boundary.
   `timegm` normalizes the carry BEFORE round 4's bound checks ever see it:
   "1969-12-31T23:59:60+00:00" computes epoch 0, not -1, because the leap
   second rolls the wall clock into 1970-01-01T00:00:00 UTC during
   normalization -- 0 is comfortably inside [0, upper), so it sailed
   through. The fix checks the TYPED year field itself, before `timegm`
   ever runs, so no carry can have happened yet. This needs no matching
   check on the upper side: "2099-12-31T23:59:60+00:00" already computed
   epoch 4102444800 (the carry lands exactly ON the excluded upper bound),
   which round 4's existing final-value check already caught -- sg already
   rejected it before this round. git itself ACCEPTS that one anyway,
   writing the very epoch it refuses when typed directly (`git commit-tree`
   measured both, 2026); this project is not reproducing git's own
   inconsistency, so that row is pinned as a deliberate divergence rather
   than "fixed" into agreement. An ordinary leap second far from either
   boundary is unaffected and stays accepted, identically on both tools. */
static void test_round5_leap_second_boundary(void)
{
    expect_fail("1969-12-31T23:59:60+00:00");
    expect_fail("2099-12-31T23:59:60+00:00"); /* PIN: git accepts, sg refuses */
    expect_ok("2023-12-31T23:59:60+00:00", 1704067200, "+0000");
}

/* SPEC-CORRECTION.md section "the scope line", case 3: sg MAY reject what
   git accepts, provided it is loud (exit 1) and pinned. Deliberately NOT
   implemented -- no fractional seconds, no named time zones OTHER THAN the
   Phase 75a UTC spellings (Z/UTC/GMT -- see test_phase75a_utc_zone_names
   below; "Wed, 15 Nov 2023 06:13:20 GMT" used to be pinned right here as a
   rejection and is now one of that function's accepted rows instead, moved
   rather than duplicated), no extra internal whitespace before an ISO
   offset (this one is UNCHANGED by Phase 75a: "T...HH:MM:SS Z", offset
   token preceded by a space in the strict ISO-T form, stays refused --
   see test_phase75a_utc_zone_names's own note on why), no tolerance for
   trailing garbage after an otherwise-complete RFC2822 date, no RFC2822
   two-digit-year century inference, and (round 5) no reinterpretation of
   an out-of-range calendar offset as local time (which also covers the
   2099-12-31T23:59:60 leap-second divergence -- see
   test_round5_leap_second_boundary above). Pinned here as "this parser
   refuses", and pinned again in interop as a head-on git-accepts/
   sg-refuses pair so both sides of the divergence are verified, not just
   this one. */
static void test_2_3_correction_deliberately_rejected(void)
{
    expect_fail("2023-11-15T06:13:20.123+08:00");
    expect_fail("2023-11-15T06:13:20 +08:00");
    expect_fail("Wed, 15 Nov 2023 06:13:20 +0800 extra garbage");
    /* Round 4, fifth deliberate rejection: RFC2822 two-digit years. git
       infers the century ("70" -> 1970, "04" -> 2004, "99" -> 1999, all
       measured); this parser does not implement century inference (it
       would add a grammar branch for no bit-compatibility benefit, same
       reasoning as the four shapes above). At round 4 this rejection was
       NOT deliberate grammar -- a two-digit year parsed as e.g.
       tm_year = 70 - 1900 = -1830, which collided with calendar_to_epoch's
       own `timegm` error sentinel (the same accident section 4b of
       docs/DESIGN.md documents for the pre-1970 lower bound), so it
       happened to fail before ever reaching either explicit bound check.
       UPDATE (round 5): calendar_to_epoch now checks the typed year field
       against 1970 directly, before `timegm` ever runs at all (added to
       close the leap-second-crossing gap -- see
       test_round5_leap_second_boundary above), so a two-digit year is now
       REJECTED by that explicit, deliberate check first. The outcome is
       unchanged; the reason is no longer an accident. */
    expect_fail("Wed, 15 Nov 70 06:13:20 +0800");
    expect_fail("Wed, 15 Nov 04 06:13:20 +0800");
    expect_fail("Wed, 15 Nov 99 06:13:20 +0800");
}

/* Section 2.4: whitespace and trailing-junk tolerance, reproduced exactly
   from the measured rows. This project's earlier belief that
   "1700000000+0800" (no space) falls back to LOCAL, discarding the
   attached offset "for want of the required space", was ITSELF an
   artifact of measuring on a +0800 machine (round 6,
   SPEC-CORRECTION-2.md): "parsed +0800" and "fell back to local +0800"
   render identically there, so the two were indistinguishable. Measured
   properly under both TZ=UTC and TZ=Asia/Kolkata: git PARSES the attached
   offset with no space at all -- whitespace is not required for
   RECOGNITION, only for deciding what happens when the trailing content
   does NOT match the offset grammar (see test_round6_offset_grammar
   below, which is where this correction's own new rows live). The
   remaining distinction this test file guards is "2023-11-15" (rejected:
   the attached "-11-15" does not look like an offset at ALL, under either
   round's rule). */
static void test_2_4_whitespace_and_trailing_junk(void)
{
    const char *old_tz = getenv("TZ");
    char *saved_tz = old_tz != NULL ? strdup(old_tz) : NULL;

    setenv("TZ", "America/New_York", 1);
    tzset();

    expect_ok("1700000000  +0800", 1700000000, "+0800");
    expect_ok("  1700000000 +0800  ", 1700000000, "+0800");
    expect_ok("1700000000 +0800 x", 1700000000, "+0800");
    expect_ok("1700000000+0800", 1700000000, "+0800");
    expect_ok_local_time("@1700000000 x", 1700000000, NULL);

    if (saved_tz != NULL) {
        setenv("TZ", saved_tz, 1);
        free(saved_tz);
    } else {
        unsetenv("TZ");
    }
    tzset();
}

/* Round 6 (SPEC-CORRECTION-2.md): the corrected offset-token grammar,
   shared by every caller in the file. Every row is checked under BOTH
   TZ=UTC and TZ=Asia/Kolkata (+0530, equal to no offset value tested
   anywhere in this phase) -- a PARSED answer must be identical under both
   zones; a LOCAL-fallback answer must TRACK the zone. This is the same
   discipline SPEC-CORRECTION-2.md itself used to find the bug: every
   earlier table in this phase was measured on a single +0800 machine,
   where "parsed +0800" and "fell back to local +0800" are
   indistinguishable, and half of the earlier grammar rows recorded the
   wrong rule as a result. */
static void test_round6_offset_grammar(void)
{
    const char *old_tz = getenv("TZ");
    char *saved_tz = old_tz != NULL ? strdup(old_tz) : NULL;

    /* Recognized shapes -- must be IDENTICAL under both zones (parsed,
       not local). */
    setenv("TZ", "UTC", 1);
    tzset();
    expect_ok("1700000000 +08", 1700000000, "+0800"); /* sign + 2 digits */
    expect_ok("1700000000 0800", 1700000000, "+0800"); /* unsigned 4 digits */
    expect_ok("1700000000+0800", 1700000000, "+0800"); /* attached, signed 4 digits */
    expect_ok("@1700000000 +08", 1700000000, "+0800"); /* @ form, sign + 2 digits */
    expect_ok("2023-11-15 06:13:20 +08", 1700000000, "+0800"); /* calendar form */
    expect_ok("2023-11-15 06:13:20 0800", 1700000000, "+0800");
    expect_ok("Wed, 15 Nov 2023 06:13:20 +08", 1700000000, "+0800");
    expect_ok("Wed, 15 Nov 2023 06:13:20 0800", 1700000000, "+0800");

    setenv("TZ", "Asia/Kolkata", 1);
    tzset();
    expect_ok("1700000000 +08", 1700000000, "+0800");
    expect_ok("1700000000 0800", 1700000000, "+0800");
    expect_ok("1700000000+0800", 1700000000, "+0800");
    expect_ok("@1700000000 +08", 1700000000, "+0800");
    expect_ok("2023-11-15 06:13:20 +08", 1700000000, "+0800");
    expect_ok("2023-11-15 06:13:20 0800", 1700000000, "+0800");
    expect_ok("Wed, 15 Nov 2023 06:13:20 +08", 1700000000, "+0800");
    expect_ok("Wed, 15 Nov 2023 06:13:20 0800", 1700000000, "+0800");

    /* NOT tokens -- must TRACK the zone (local fallback) under both. */
    setenv("TZ", "UTC", 1);
    tzset();
    expect_ok("1700000000 +08000", 1700000000, "+0000"); /* 5 digits */
    expect_ok("1700000000 +080", 1700000000, "+0000"); /* 3 digits */
    expect_ok("1700000000 08", 1700000000, "+0000"); /* 2 digits, no sign */
    expect_ok("1700000000 +8", 1700000000, "+0000"); /* 1 digit */

    setenv("TZ", "Asia/Kolkata", 1);
    tzset();
    expect_ok("1700000000 +08000", 1700000000, "+0530");
    expect_ok("1700000000 +080", 1700000000, "+0530");
    expect_ok("1700000000 08", 1700000000, "+0530");
    expect_ok("1700000000 +8", 1700000000, "+0530");

    if (saved_tz != NULL) {
        setenv("TZ", saved_tz, 1);
        free(saved_tz);
    } else {
        unsetenv("TZ");
    }
    tzset();
}

/* Section 2.5: the local offset used when *_DATE is absent/malformed must
   be the offset AT THE COMMIT'S OWN INSTANT, not "now"'s -- the DST pair
   CLAUDE.md's Phase 72 section quotes directly. Skips outright if this
   machine's zoneinfo lacks the zone, rather than silently passing (the same
   discipline test_date_relative.c and friends use for zone-dependent rows). */
static void test_local_offset_is_the_instants(void)
{
    const char *old_tz = getenv("TZ");
    char *saved_tz = old_tz != NULL ? strdup(old_tz) : NULL;
    long long t;
    char tz[8];

    setenv("TZ", "America/New_York", 1);
    tzset();

    CHECK(sg_ident_parse_date("@1700000000", &t, tz) == 0, "expected ok");
    CHECK(strcmp(tz, "-0500") == 0, "November instant: got %s want -0500", tz);

    CHECK(sg_ident_parse_date("@1688000000", &t, tz) == 0, "expected ok");
    CHECK(strcmp(tz, "-0400") == 0, "June instant: got %s want -0400", tz);

    if (saved_tz != NULL) {
        setenv("TZ", saved_tz, 1);
        free(saved_tz);
    } else {
        unsetenv("TZ");
    }
    tzset();
}

/* sg_ident_author/_committer: name/email fallback and independence -- an
   unset GIT_COMMITTER_NAME must NOT fall back to whatever GIT_AUTHOR_NAME
   resolved to (that was a pre-existing bug in storage/chunk.c, fixed as
   part of this same phase), it must fall back to the SAME hardcoded
   default the author side uses absent its own env var. */
static void test_author_committer_independent_fallback(void)
{
    sg_ident author, committer;
    const char *bad = NULL;

    unsetenv("GIT_AUTHOR_NAME");
    unsetenv("GIT_AUTHOR_EMAIL");
    unsetenv("GIT_AUTHOR_DATE");
    unsetenv("GIT_COMMITTER_NAME");
    unsetenv("GIT_COMMITTER_EMAIL");
    unsetenv("GIT_COMMITTER_DATE");

    CHECK(sg_ident_author(&author, &bad) == 0, "author resolve should succeed");
    CHECK(sg_ident_committer(&committer, &bad) == 0, "committer resolve should succeed");
    CHECK(strcmp(author.name, "small_git") == 0, "author fallback name: got %s", author.name);
    CHECK(strcmp(author.email, "sg@localhost") == 0, "author fallback email: got %s", author.email);
    CHECK(strcmp(committer.name, "small_git") == 0, "committer fallback name: got %s", committer.name);
    CHECK(strcmp(committer.email, "sg@localhost") == 0, "committer fallback email: got %s",
         committer.email);

    setenv("GIT_AUTHOR_NAME", "Only Author", 1);
    CHECK(sg_ident_committer(&committer, &bad) == 0, "committer resolve should succeed");
    CHECK(strcmp(committer.name, "small_git") == 0,
         "GIT_AUTHOR_NAME being set must not leak into the committer fallback, got %s",
         committer.name);
    unsetenv("GIT_AUTHOR_NAME");
}

static void test_invalid_date_reports_bad_value(void)
{
    sg_ident author;
    const char *bad = NULL;

    setenv("GIT_AUTHOR_DATE", "yesterday", 1);
    CHECK(sg_ident_author(&author, &bad) == -1, "expected failure");
    CHECK(bad != NULL && strcmp(bad, "yesterday") == 0, "bad value should echo the raw input");
    unsetenv("GIT_AUTHOR_DATE");
}

/* Phase 75a: the UTC-only zone-NAME spellings, measured against real git
   2.55.0 -- see docs/phase75a-spec.md and docs/RULES-date.md's own
   phase75a table for the full oracle. Motivated by tests/fuzz_diff.py and
   tests/fuzz_rename.py, which set GIT_AUTHOR_DATE=...Z and had been dying
   in their own fixture setup, unrun, since Phase 72. Every row here is one
   the fuzzers' own value or a shape from the same measured table -- not
   re-derived. */
static void test_phase75a_utc_zone_names(void)
{
    /* In scope: Z/UTC/GMT, case-insensitive, in every form where sg
       already recognizes an offset token. */
    expect_ok("2026-01-01T00:00:00Z", 1767225600, "+0000");
    expect_ok("2026-01-01T00:00:00z", 1767225600, "+0000");
    expect_ok("2026-01-01 00:00:00Z", 1767225600, "+0000");
    expect_ok("Thu, 1 Jan 2026 00:00:00Z", 1767225600, "+0000");
    expect_ok("1767225600 Z", 1767225600, "+0000");
    expect_ok("1767225600Z", 1767225600, "+0000");
    expect_ok("@1767225600 Z", 1767225600, "+0000");
    expect_ok("@1767225600Z", 1767225600, "+0000");
    expect_ok("1767225600 UTC", 1767225600, "+0000");
    expect_ok("1767225600 utc", 1767225600, "+0000");
    expect_ok("1767225600 Utc", 1767225600, "+0000");
    expect_ok("1767225600 GMT", 1767225600, "+0000");
    expect_ok("1767225600 gmt", 1767225600, "+0000");
    /* Moved here from test_2_3_correction_deliberately_rejected, where it
       used to be pinned as a refusal (see that function's own updated
       comment). */
    expect_ok("Wed, 15 Nov 2023 06:13:20 GMT", 1700028800, "+0000");

    /* WARNING: "2026-01-01T00:00:00 Z" (a SPACE between the seconds and
       the zone name, strict ISO-T form) is deliberately NOT implemented,
       even though it is in scope by the letter of "every form" above and
       measured (git accepts it as +0000) -- it collides head-on with a
       PRE-EXISTING, already-pinned deliberate rejection one function up
       ("no extra internal whitespace before an ISO offset",
       expect_fail("2023-11-15T06:13:20 +08:00")): the strict ISO-T
       grammar has never tolerated ANY offset token, digit or name,
       preceded by a space, and this phase does not carve out an exception
       for names only. Pinned here as a refusal, and pinned again in
       interop's phase75a group as a head-on git-accepts/sg-refuses pair,
       same shape as every other entry in that older deliberate-rejection
       list. */
    expect_fail("2026-01-01T00:00:00 Z");

    /* Out of scope, pinned so a future change does not "converge" these
       by accident. Every other zone name git implements: sg still has no
       table for it, so EST-shaped input falls through to the ordinary
       "unrecognized trailing content after a space is tolerated as junk"
       rule and resolves to LOCAL time, exactly as before this phase. */
    /* TZ is pinned for this one, the way every other local-offset-sensitive
       test in this file already pins it, and for a reason a cold read had
       to reproduce before anyone believed it: epoch 1767225600 is
       2026-01-01 00:00:00 UTC, i.e. winter, so on a machine whose own zone
       IS US Eastern the local offset really is -0500 -- the exact string
       this assertion demands NOT be echoed back. The first version of this
       line left TZ alone and failed on demand under
       `TZ=America/New_York ./build/tests/test_ident`, while passing on CI
       (UTC) and on the author's machine. A contributor in New York would
       have read it as a real regression. */
    {
        char *saved_tz = getenv("TZ");
        char *saved_copy = saved_tz != NULL ? strdup(saved_tz) : NULL;

        setenv("TZ", "Asia/Taipei", 1);
        tzset();
        expect_ok_local_time("1767225600 EST", 1767225600, "-0500");
        /* Names git accepts but itself resolves to local: unaffected,
           still local here too. Each one names the offset it must NOT
           echo back, so a broken whole-word boundary (Z matching inside
           ZULU, UTC inside UTC1) turns them red -- passing NULL here, as
           the first version did, made all of them inspect nothing but the
           timestamp, which the offset never feeds. */
        expect_ok_local_time("1767225600 ZULU", 1767225600, "+0000");
        expect_ok_local_time("1767225600 UTC1", 1767225600, "+0000");
        expect_ok_local_time("1767225600 GMT0", 1767225600, "+0000");
    /* Trailing junk glued onto a recognized name: git ignores it (or
       falls back to local); sg keeps REFUSING rather than guess -- a
       refusal can never write a wrong object id (rule 3 of the scope
       line). Attached (no space): a hard parse failure, same rule as
       every other attached-and-unrecognized case in this file. */
    expect_fail("1767225600ZZ");
    expect_fail("1767225600Zx");
    expect_fail("1767225600UTC+1");
    /* Preceded by whitespace: tolerated as a single token of junk,
       falling back to local -- the SAME pre-existing rule
       test_2_4_whitespace_and_trailing_junk already pins for a numeric
       token ("@1700000000 x"), now confirmed to also cover a
       name-shaped one. "Z followed by more bytes must never silently
       read as Z" -- these still fall back to local, not to +0000. */
        expect_ok_local_time("1767225600 ZZ", 1767225600, "+0000");
        expect_ok_local_time("1767225600 Zx", 1767225600, "+0000");
        expect_ok_local_time("1767225600 UTC+1", 1767225600, "+0000");
        expect_ok_local_time("1767225600 GMT+0", 1767225600, "+0000");
        expect_ok_local_time("1767225600 Z+1", 1767225600, "+0000");

        if (saved_copy != NULL) {
            setenv("TZ", saved_copy, 1);
            free(saved_copy);
        } else {
            unsetenv("TZ");
        }
        tzset();
    }

    /* Attached (no space) UTC and GMT, measured after a cold read pointed
       out that only attached `Z` had ever been: the shared matcher accepts
       all three names in every form, so these were live and untested.
       Measured against git 2.55.0 under TZ=Asia/Taipei (local +0800, so
       +0000 proves the NAME was parsed): all ten spellings agree with git. */
    expect_ok("1767225600UTC", 1767225600, "+0000");
    expect_ok("1767225600GMT", 1767225600, "+0000");
    expect_ok("@1767225600UTC", 1767225600, "+0000");
    expect_ok("1767225600utc", 1767225600, "+0000");
    expect_ok("2026-01-01T00:00:00UTC", 1767225600, "+0000");
    expect_ok("2026-01-01T00:00:00GMT", 1767225600, "+0000");
    expect_ok("2026-01-01T00:00:00gmt", 1767225600, "+0000");
    expect_ok("2026-01-01 00:00:00UTC", 1767225600, "+0000");
    expect_ok("Thu, 1 Jan 2026 00:00:00UTC", 1767225600, "+0000");
    expect_ok("Thu, 1 Jan 2026 00:00:00GMT", 1767225600, "+0000");

    /* REGRESSION PIN. The attached-zone-name branch above must FALL
       THROUGH when the attached content is not a zone name, not fail:
       this row is accepted by git (measured, 1767230000 +0500) and was
       accepted identically by sg BEFORE Phase 75a -- sscanf's "%n" stops
       at the junk and the offset comes from the next token. Phase 75a's
       first version returned -1 there and silently took this agreement
       away; a cold read found it, and every one of the phase's own new
       rows used shapes where the attached content IS a zone name, so
       nothing else here would have caught it. */
    expect_ok("Thu, 1 Jan 2026 06:13:20foo +0500", 1767230000, "+0500");
    /* ...and the shapes around it that sg refuses, pinned so the fall-
       through cannot quietly widen either: junk with no following offset
       token at all, a name with junk glued to it (the whole-word rule),
       and junk as its own space-separated token. All three were refusals
       before Phase 75a too -- measured on master -- and git accepts all
       three, a pre-existing divergence this phase does not change. */
    expect_fail("Thu, 1 Jan 2026 06:13:20foo");
    expect_fail("Thu, 1 Jan 2026 06:13:20Zfoo");
    expect_fail("Thu, 1 Jan 2026 06:13:20 foo +0500");
}

int main(void)
{
    test_2_2_accepted_and_rejected_forms();
    test_2_3_offset_range();
    test_2_3_correction_at_form_offset_normalization();
    test_2_3_correction_min_digit_count();
    test_2_3_correction_upper_bound();
    test_2_3_correction_lower_bound();
    test_2_3_correction_raw_vs_final_bound();
    test_round5_calendar_offset_out_of_range();
    test_round5_leap_second_boundary();
    test_2_3_correction_deliberately_rejected();
    test_2_4_whitespace_and_trailing_junk();
    test_round6_offset_grammar();
    test_local_offset_is_the_instants();
    test_author_committer_independent_fallback();
    test_invalid_date_reports_bad_value();
    test_phase75a_utc_zone_names();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all ident tests passed\n");
    return 0;
}
