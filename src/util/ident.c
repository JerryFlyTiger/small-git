#include "sg/ident.h"

#include "sg/date.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Phase 72 round 2 (SPEC-CORRECTION.md): a bare-form or offset-less
   timestamp needs at least this many digits, and must land strictly before
   the upper bound below -- both measured against real git 2.55.0 via
   `git commit-tree`. The ONE exemption is the `@<digits>` form when it is
   followed by a syntactically valid (shape-wise) explicit offset token:
   that combination has no digit-count floor and no upper bound at all,
   because git normalizes the offset arithmetically instead of validating
   the timestamp itself. */
#define SG_IDENT_MIN_EPOCH_DIGITS 9
/* 2100-01-01T00:00:00Z, exclusive upper bound (epoch >= this is rejected). */
#define SG_IDENT_EPOCH_UPPER_BOUND 4102444800LL
/* 1970-01-01T00:00:00Z, inclusive lower bound (epoch < this is rejected).
   Phase 72 round 3: measured directly against real git, which rejects
   EVERY pre-1970 instant on every calendar form, not just the one value
   ("1969-12-31T23:59:59+00:00") an earlier round of this same phase's own
   spec happened to probe. That value is rejected by git for the ordinary
   reason (epoch -1 < 0), but was ACCIDENTALLY rejected by this file for a
   DIFFERENT reason before this bound existed: it collides with timegm's own
   error sentinel `(time_t)-1` in calendar_to_epoch, so
   "1969-12-31T23:59:58+00:00" (epoch -2, no such collision) sailed straight
   through and got written as a real object with a negative author time.
   This constant makes the rejection an explicit, checked property instead
   of a coincidence -- applied everywhere SG_IDENT_EPOCH_UPPER_BOUND already
   is, on the same computed epoch RESULT rather than the year field, so all
   three calendar forms and both offset branches within each are covered by
   one rule. The epoch/`@<digits>` forms need no such check here at all:
   their own digit scanner only ever accepts UNSIGNED decimal digits (no
   leading '-'), so a negative epoch is structurally unreachable through
   them -- matching git, which rejects "@-5"/"-1" for the same underlying
   reason (measured). */
#define SG_IDENT_EPOCH_LOWER_BOUND 0LL

static const char *env_or(const char *name, const char *fallback)
{
    const char *v = getenv(name);

    return (v != NULL && v[0] != '\0') ? v : fallback;
}

/* Formats a signed offset (seconds) as "+HHMM"/"-HHMM" into a buffer of at
   least 6 bytes. Truncates toward zero the same way git's own
   tm_gmtoff/60 division does -- the object format can only ever hold a
   whole minute anyway, so there is nothing further to lose here. */
static void format_offset(long offset, char out[8])
{
    char sign = offset < 0 ? '-' : '+';
    long mag = offset < 0 ? -offset : offset;

    snprintf(out, 8, "%c%02ld%02ld", sign, mag / 3600, (mag % 3600) / 60);
}

/* Fills tz_out with the machine's own offset at time_sec's instant.
   Returns 0 on success, -1 if the underlying localtime_r-based lookup
   fails (an out-of-range time_sec). */
static int fill_local_offset(long long time_sec, char tz_out[8])
{
    long off;

    if (sg_date_local_offset_seconds(time_sec, &off) != 0)
        return -1;
    format_offset(off, tz_out);
    return 0;
}

/* Round 6 (SPEC-CORRECTION-2.md): the offset-token GRAMMAR, shared by
   every caller in this file (both timestamp forms -- bare and `@` -- and
   every calendar form, which reuses the bare form's policy). Every earlier
   table in this phase was measured on a machine whose own local zone is
   +0800; for any offset that PARSES as +0800, "parsed" and "fell back to
   local" render byte-identical output there, so the two were
   indistinguishable and half the grammar was recorded wrong. Re-measured
   under both TZ=UTC and TZ=Asia/Kolkata (+0530, equal to nothing in any
   table, so a parsed value can never be mistaken for a local one) via
   `git commit-tree`. The corrected grammar:

       <sign> <2 digits>        +08     (sign REQUIRED for this shape)
       [<sign>] <4 digits>      +0800, 0800
       [<sign>] <2>:<2>         +08:00, 00:00

   Anything else -- 1 digit, 3 digits, 5+ digits, or exactly 2 digits with
   NO sign -- is not a token at all. The 2-vs-4-vs-5-digit distinction is
   why the digit run is counted in full before deciding which shape (if
   any) matched: a naive "match the first 2 (or 4) digits and ignore what
   follows" would treat "+08000" (5 digits, NOT a token, falls back to
   local) as if it were "+0800" followed by ignorable trailing junk, which
   is exactly backwards -- git falls back to local for that value.

   Range-checking and the policy on a recognized-but-out-of-range token are
   the CALLER's job (the two timestamp forms disagree on it: the bare form
   discards it for local, the `@` form normalizes it arithmetically) --
   this function only recognizes SHAPE. Returns 0 and fills
   *negative_out, *hh_out and *mm_out (unreduced -- MM can be entered as high as
   59 by construction here, but a colon token like "08:99" would still
   reach this far; range-checking still belongs to the caller) and
   *len_out (bytes consumed) on a shape match; -1 otherwise. */
static int match_offset_shape(const char *s, int *negative_out, int *hh_out, int *mm_out,
                              size_t *len_out)
{
    const char *p = s;
    int has_sign = 0;
    int ndigits;

    *negative_out = 0;
    if (*p == '+') {
        has_sign = 1;
        p++;
    } else if (*p == '-') {
        has_sign = 1;
        *negative_out = 1;
        p++;
    }

    /* Colon shape checked first: a ':' can only ever appear here, and
       checking it first keeps the plain-digit-run count below from ever
       having to special-case it. */
    if (isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1]) && p[2] == ':' &&
       isdigit((unsigned char)p[3]) && isdigit((unsigned char)p[4])) {
        *hh_out = (p[0] - '0') * 10 + (p[1] - '0');
        *mm_out = (p[3] - '0') * 10 + (p[4] - '0');
        *len_out = (size_t)(p - s) + 5;
        return 0;
    }

    ndigits = 0;
    while (isdigit((unsigned char)p[ndigits]))
        ndigits++;
    if (ndigits == 2 && has_sign) {
        *hh_out = (p[0] - '0') * 10 + (p[1] - '0');
        *mm_out = 0;
        *len_out = (size_t)(p - s) + 2;
        return 0;
    }
    if (ndigits == 4) {
        *hh_out = (p[0] - '0') * 10 + (p[1] - '0');
        *mm_out = (p[2] - '0') * 10 + (p[3] - '0');
        *len_out = (size_t)(p - s) + 4;
        return 0;
    }
    return -1;
}

/* The bare/calendar-form POLICY on a shape-matched token: range-checked
   (HH <= 23, MM <= 59), an out-of-range match reported via *out_of_range
   so the caller can discard it for local rather than treating it as "did
   not match at all". `len_out` receives the consumed length so a caller
   that requires the token to be the LAST thing in the string (every
   calendar form) can check that; a caller that tolerates trailing junk
   after a successfully-applied token (the bare epoch form) is free to
   ignore it. */
static int match_offset_token(const char *s, long *offset_out, int *out_of_range,
                              size_t *len_out)
{
    int negative, hh, mm;

    *out_of_range = 0;
    if (match_offset_shape(s, &negative, &hh, &mm, len_out) != 0)
        return -1;
    if (hh > 23 || mm > 59) {
        *out_of_range = 1;
        return -1;
    }
    *offset_out = (hh * 3600 + mm * 60) * (negative ? -1 : 1);
    return 0;
}

/* Renders an offset that has already been reduced to (sign, total minutes)
   as "<sign><HH><MM>", HH zero-padded to a MINIMUM of 2 digits but never
   truncated -- git's own `@<epoch> <offset>` arithmetic normalization can
   produce an hours field of 3 digits ("+10039" from "+9999"), and "%02ld"
   already does exactly the right thing here: it pads short values and
   never clips long ones. */
static void format_offset_normalized(int negative, long total_minutes, char out[8])
{
    long hh = total_minutes / 60;
    long mm = total_minutes % 60;

    snprintf(out, 8, "%c%02ld%02ld", negative ? '-' : '+', hh, mm);
}

/* The `@` form's POLICY on a shape-matched token: no range check at all --
   matching the shape is enough for git to treat it as an explicit offset
   and normalize it arithmetically; "+9999" is just as much a valid TOKEN
   as "+0800", it renders differently, not "invalidly". Returns 0 and
   fills *negative and *total_minutes (HH times 60 plus MM, unreduced --
   the caller re-derives HH/MM from the sum) on a shape match, -1
   otherwise. Unlike match_offset_token, trailing content past the token is
   never tolerated here -- every call site needs the token to be the whole
   remaining string, so this checks `len_out` against the actual length
   itself rather than handing it back. */
static int match_offset_shape_loose(const char *s, int *negative, long *total_minutes)
{
    int hh, mm;
    size_t len;

    if (match_offset_shape(s, negative, &hh, &mm, &len) != 0)
        return -1;
    if (s[len] != '\0')
        return -1;
    *total_minutes = (long)hh * 60 + mm;
    return 0;
}

/* The "[@]<digits>[ <tz>]" unix-timestamp form. See ident.h's own header
   comment for the exact, measured whitespace/junk rules -- reproduced here
   rather than re-derived, do not rewrite this from first principles. */
static int try_epoch(const char *s, long long *time_out, char tz_out[8])
{
    const char *p = s;
    const char *digits_start;
    char *endptr;
    long long epoch;
    size_t ndigits;
    int has_at = 0;
    long offset;
    int out_of_range;
    /* Set when the `@` form found a shape-valid explicit offset -- the ONE
       combination SPEC-CORRECTION.md exempts from both the digit-count
       floor and the upper bound, because git normalizes the offset
       arithmetically instead of validating the timestamp. */
    int at_form_offset_applies = 0;
    int at_negative = 0;
    long at_total_minutes = 0;
    /* Set when the bare form found an offset token that is IN RANGE
       (kept, not discarded) -- unaffected by this correction, same rule
       as before. */
    int bare_offset_applies = 0;
    long bare_offset_seconds = 0;

    if (*p == '@') {
        has_at = 1;
        p++;
    }
    if (!isdigit((unsigned char)*p))
        return -1;
    digits_start = p;
    while (isdigit((unsigned char)*p))
        p++;
    ndigits = (size_t)(p - digits_start);

    errno = 0;
    epoch = strtoll(digits_start, &endptr, 10);
    if (errno == ERANGE || endptr != p)
        return -1;

    if (*p != '\0') {
        int had_space = isspace((unsigned char)*p);
        const char *tail = p;
        int shape_matched = 0;
        size_t token_len;

        while (isspace((unsigned char)*tail))
            tail++;

        /* Round 6 (SPEC-CORRECTION-2.md): whitespace before the offset is
           NOT required for it to be RECOGNIZED -- "1700000000+0800" (no
           space at all) is PARSED by git, not merely tolerated-and-
           discarded the way an earlier round of this same phase believed
           (measured on a +0800 machine, where "parsed +0800" and "fell
           back to local +0800" render identically and so could not be
           told apart). Whitespace's only remaining role is deciding what
           happens when the trailing content does NOT match the offset
           grammar SHAPE at all: WITH a space, unrecognized content is
           tolerated as junk (a single token; "@1700000000 x"); WITHOUT
           one, it is a hard parse failure ("2023-11-15", where the
           attached "-11-15" never matches any offset shape either, before
           or after this round). A SHAPE-VALID but numerically
           out-of-range bare-form token (out_of_range == 1) is a THIRD
           case, unconditionally tolerated regardless of whitespace -- this
           was already the pre-round-6 rule for the attached form
           ("1700000000+9999"-shaped input) and stays that way; only a
           genuine shape mismatch is newly affected by `had_space`. */
        if (*tail != '\0') {
            if (has_at) {
                shape_matched =
                    (match_offset_shape_loose(tail, &at_negative, &at_total_minutes) == 0);
                if (shape_matched)
                    at_form_offset_applies = 1;
            } else if (match_offset_token(tail, &offset, &out_of_range, &token_len) == 0) {
                shape_matched = 1;
                bare_offset_applies = 1;
                bare_offset_seconds = offset;
            } else if (out_of_range) {
                shape_matched = 1;
            }
        }

        if (*tail != '\0' && !shape_matched) {
            if (!had_space)
                return -1;
            /* Preceded by whitespace: tolerate a single token of junk
               ("@1700000000 x"), but not a second space-separated token
               past it -- git's own relative-date phrases ("2 hours ago")
               are not junk this parser tolerates, they are a parse
               failure. */
            {
                const char *scan = tail;

                while (*scan != '\0' && !isspace((unsigned char)*scan))
                    scan++;
                while (isspace((unsigned char)*scan))
                    scan++;
                if (*scan != '\0')
                    return -1;
            }
        }
    }

    /* The digit-count floor and the upper bound apply to EVERY combination
       except `@<digits>` followed by a shape-valid explicit offset. There is
       no lower-bound check here at all, on either branch: `epoch` was parsed
       by the unsigned digit scanner above, which never accepts a leading
       '-', so a negative value is structurally unreachable through this
       function -- matching git's own rejection of "@-5"/"-1" (measured).
       SG_IDENT_EPOCH_LOWER_BOUND only needs to be checked in the calendar
       forms below, where an explicit year/month/day can compute one. */
    if (!(has_at && at_form_offset_applies)) {
        if (ndigits < SG_IDENT_MIN_EPOCH_DIGITS)
            return -1;
        if (epoch >= SG_IDENT_EPOCH_UPPER_BOUND)
            return -1;
    }

    *time_out = epoch;
    if (at_form_offset_applies) {
        format_offset_normalized(at_negative, at_total_minutes, tz_out);
        return 0;
    }
    if (bare_offset_applies) {
        format_offset(bare_offset_seconds, tz_out);
        return 0;
    }
    return fill_local_offset(epoch, tz_out);
}

static int month_from_name(const char *tok, size_t len)
{
    static const char *const MON3[12] = {
        "jan", "feb", "mar", "apr", "may", "jun",
        "jul", "aug", "sep", "oct", "nov", "dec"
    };
    int i;
    char lower[4];

    if (len != 3)
        return -1;
    lower[0] = (char)tolower((unsigned char)tok[0]);
    lower[1] = (char)tolower((unsigned char)tok[1]);
    lower[2] = (char)tolower((unsigned char)tok[2]);
    lower[3] = '\0';
    for (i = 0; i < 12; i++) {
        if (strcmp(lower, MON3[i]) == 0)
            return i;
    }
    return -1;
}

/* Converts a UTC calendar y/m/d h:m:s into epoch seconds via timegm (a
   POSIX extension available on both platforms this project supports),
   then applies `offset` to get the true UTC instant the wall clock +
   offset actually named.

   Round 4: git rejects if EITHER the RAW calendar reading (the wall clock
   read as if it were already UTC, i.e. `t` below, before `offset` is
   subtracted) OR the offset-adjusted final epoch (`*out`) falls outside
   [SG_IDENT_EPOCH_LOWER_BOUND, SG_IDENT_EPOCH_UPPER_BOUND) -- measured
   directly against `git commit-tree`: "1969-12-31T23:30:00-01:00" has a
   raw reading of 1969-12-31 23:30 UTC (out of range) but a final instant of
   1970-01-01 00:30 UTC (in range, since -01:00 shifts it forward), and git
   still rejects it. Checking only the final value (the round-3 fix) left
   this direction open, letting sg write an object git could never have
   produced from the same env -- rule 2 of the scope line, same class as
   the bugs sections 4b/5 already closed. The reverse direction (raw in
   range, final out of range) was already correctly rejected by the final-
   value check alone, so this bound is genuinely ADDITIONAL, not a
   replacement -- both checks stay. Deliberately checked HERE, once, on
   `t` itself using the SAME two constants every other bound in this file
   uses, rather than at each of the three calendar-form call sites: that is
   what makes all three forms and both of their offset branches inherit it
   for free instead of needing three more copies of the same two lines.

   Round 5: a leap second (ss == 60) that crosses the 1970 boundary defeats
   BOTH of round 4's checks. `timegm` normalizes the carry before either
   check ever runs: "1969-12-31T23:59:60+00:00" -> `t` = 0, not -1, because
   the leap second rolls the wall clock over into 1970-01-01T00:00:00 UTC
   during normalization -- measured directly. `t` = 0 satisfies both the
   raw check (0 is not < SG_IDENT_EPOCH_LOWER_BOUND) and, with offset 0,
   the final check too, so the date sails through even though git rejects
   it. The fix checks the TYPED year field -- as parsed, before `timegm`
   ever runs, so no carry can have happened yet -- against the calendar
   year the lower bound represents (1970). This needs no equivalent on the
   upper side: measured, "2099-12-31T23:59:60+00:00" already computes
   `t` = 4102444800 (the carry lands exactly ON the excluded upper bound),
   which round 4's existing `t >= SG_IDENT_EPOCH_UPPER_BOUND` check already
   catches -- there is no matching hole to close there. (git itself
   accepts that date anyway, writing the very epoch it refuses when typed
   directly -- an inconsistency in git's own parser this project is not
   reproducing; see docs/DESIGN.md's Phase 72 round 5 section. sg's
   rejection of it is pinned as a deliberate divergence, not read as a
   coincidence to "fix" into agreement.) */
static int calendar_to_epoch(int year, int mon0, int day, int hh, int mi, int ss, long offset,
                             long long *out)
{
    struct tm tmv;
    time_t t;

    if (year < 1970)
        return -1;
    if (mon0 < 0 || mon0 > 11 || day < 1 || day > 31 || hh < 0 || hh > 23 || mi < 0 || mi > 59 ||
       ss < 0 || ss > 60)
        return -1;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = year - 1900;
    tmv.tm_mon = mon0;
    tmv.tm_mday = day;
    tmv.tm_hour = hh;
    tmv.tm_min = mi;
    tmv.tm_sec = ss;
    tmv.tm_isdst = 0;
    t = timegm(&tmv);
    if (t == (time_t)-1)
        return -1;
    if ((long long)t < SG_IDENT_EPOCH_LOWER_BOUND || (long long)t >= SG_IDENT_EPOCH_UPPER_BOUND)
        return -1;
    *out = (long long)t - (long long)offset;
    return 0;
}

/* "YYYY-MM-DDTHH:MM:SS[+-]HH:MM" -- ISO 8601 strict. Shares
   match_offset_token with its two siblings as of round 6 (it used to have
   its own match_offset_token_colon wrapper, needed only because
   match_offset_token was hardcoded to the old 5-byte sign+4-digit shape;
   the unified match_offset_shape underneath handles the colon shape for
   every caller now, so the wrapper is gone).

   Round 5: a shape-valid but numerically out-of-range offset (e.g.
   "+99:00") is REFUSED outright, not discarded for local -- see the
   decision recorded at try_iso_space's own comment, which all three
   calendar forms now share uniformly. This function already refused such
   an offset before round 5, by accident (the out_of_range result was
   computed but never consulted here); it is refused on purpose now, and
   looks identical to its two siblings. */
static int try_iso_t(const char *s, long long *time_out, char tz_out[8])
{
    int year, mon, day, hh, mi, ss;
    int n = 0;
    long offset;
    int out_of_range;
    size_t tzlen;

    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%n", &year, &mon, &day, &hh, &mi, &ss, &n) != 6)
        return -1;
    if (match_offset_token(s + n, &offset, &out_of_range, &tzlen) != 0)
        return -1;
    if (s[(size_t)n + tzlen] != '\0')
        return -1;
    if (calendar_to_epoch(year, mon - 1, day, hh, mi, ss, offset, time_out) != 0)
        return -1;
    if (*time_out < SG_IDENT_EPOCH_LOWER_BOUND || *time_out >= SG_IDENT_EPOCH_UPPER_BOUND)
        return -1;
    format_offset(offset, tz_out);
    return 0;
}

/* "YYYY-MM-DD HH:MM:SS [+-]HHMM" -- no colon in the offset.

   Round 5 decision: a calendar form (this one, try_iso_t, try_rfc2822)
   carrying a shape-valid but numerically OUT-OF-RANGE offset (e.g.
   "+9999") is REFUSED outright -- sg does NOT discard the offset and
   re-read the wall clock as UTC (what it used to do here) or reinterpret
   it as LOCAL time (what git does). Measured directly against
   `git commit-tree`: git's local-time reinterpretation makes
   "2023-11-15 06:13:20 +9999" resolve to a DIFFERENT epoch than sg's old
   discard-to-UTC behaviour, both tools exiting 0 -- a SILENT divergence,
   rule 2 of the scope line, on an utterly ordinary date (not a boundary
   case). Reproducing git's actual rule would mean resolving a wall clock
   against a LOCAL zone, which is ill-defined during a DST gap and
   ambiguous during a DST overlap -- a whole new class of corner cases, for
   an input nobody sensible writes (a real UTC offset is at most +/-14:00;
   "+9999" is nonsense). Refusing is rule 3 instead: loud, and a refusal
   can never write a wrong object. This is why the fallback branch that
   used to sit here (mirroring the BARE epoch form's own "discard, fall
   back to local" rule) is gone rather than fixed -- the two forms are
   deliberately no longer symmetric: try_epoch's own out-of-range handling
   is untouched, because git's bare-epoch-form behaviour there was already
   measured and pinned in round 2/3, and is not part of this decision. */
static int try_iso_space(const char *s, long long *time_out, char tz_out[8])
{
    int year, mon, day, hh, mi, ss;
    int n = 0;
    long offset;
    int out_of_range;
    size_t token_len;
    const char *p;

    if (sscanf(s, "%4d-%2d-%2d %2d:%2d:%2d%n", &year, &mon, &day, &hh, &mi, &ss, &n) != 6)
        return -1;
    p = s + n;
    while (isspace((unsigned char)*p))
        p++;
    if (match_offset_token(p, &offset, &out_of_range, &token_len) != 0)
        return -1;
    if (p[token_len] != '\0')
        return -1;
    if (calendar_to_epoch(year, mon - 1, day, hh, mi, ss, offset, time_out) != 0)
        return -1;
    if (*time_out < SG_IDENT_EPOCH_LOWER_BOUND || *time_out >= SG_IDENT_EPOCH_UPPER_BOUND)
        return -1;
    format_offset(offset, tz_out);
    return 0;
}

/* "[Www, ]DD Mon YYYY HH:MM:SS [+-]HHMM" -- RFC 2822. The weekday, if
   present, is skipped unvalidated (git does not check it against the
   actual day of week either for this purpose). A shape-valid but
   out-of-range offset is refused outright, same as its two siblings --
   see try_iso_space's own comment for the round-5 decision and why. */
static int try_rfc2822(const char *s, long long *time_out, char tz_out[8])
{
    char buf[128];
    char *tok, *saveptr;
    long day, year;
    int mon, hh, mi, ss;
    long offset;
    int out_of_range;
    size_t token_len;
    char *end;

    if (strlen(s) >= sizeof(buf))
        return -1;
    strcpy(buf, s);

    tok = strtok_r(buf, " \t", &saveptr);
    if (tok == NULL)
        return -1;
    {
        size_t len = strlen(tok);

        if (len > 0 && tok[len - 1] == ',') {
            tok = strtok_r(NULL, " \t", &saveptr);
            if (tok == NULL)
                return -1;
        }
    }
    day = strtol(tok, &end, 10);
    if (*end != '\0' || day < 1 || day > 31)
        return -1;

    tok = strtok_r(NULL, " \t", &saveptr);
    if (tok == NULL)
        return -1;
    mon = month_from_name(tok, strlen(tok));
    if (mon < 0)
        return -1;

    tok = strtok_r(NULL, " \t", &saveptr);
    if (tok == NULL)
        return -1;
    year = strtol(tok, &end, 10);
    /* Bounded here, BEFORE the (int) cast below, rather than relying on
       SG_IDENT_EPOCH_UPPER_BOUND making an oversized year unreachable --
       calendar_to_epoch takes `int year` while this parses `long`, so an
       unguarded cast of a huge token (e.g. LONG_MAX) is truncation, not a
       range check. 9999 is generous for any calendar year worth accepting
       at all; every real rejection this phase cares about (year 2100 and
       above) is already comfortably inside this bound and is caught by the
       upper-bound check a few lines down instead. */
    if (*end != '\0' || year < 1 || year > 9999)
        return -1;

    tok = strtok_r(NULL, " \t", &saveptr);
    if (tok == NULL)
        return -1;
    if (sscanf(tok, "%2d:%2d:%2d", &hh, &mi, &ss) != 3)
        return -1;

    tok = strtok_r(NULL, " \t", &saveptr);
    if (tok == NULL)
        return -1;
    if (match_offset_token(tok, &offset, &out_of_range, &token_len) != 0)
        return -1;
    if (strlen(tok) != token_len)
        return -1;

    tok = strtok_r(NULL, " \t", &saveptr);
    if (tok != NULL)
        return -1;

    if (calendar_to_epoch((int)year, mon, (int)day, hh, mi, ss, offset, time_out) != 0)
        return -1;
    if (*time_out < SG_IDENT_EPOCH_LOWER_BOUND || *time_out >= SG_IDENT_EPOCH_UPPER_BOUND)
        return -1;
    format_offset(offset, tz_out);
    return 0;
}

int sg_ident_parse_date(const char *raw, long long *time_out, char tz_out[8])
{
    const char *p = raw;

    while (isspace((unsigned char)*p))
        p++;
    if (*p == '\0')
        return -1;

    if (try_epoch(p, time_out, tz_out) == 0)
        return 0;
    if (try_iso_t(p, time_out, tz_out) == 0)
        return 0;
    if (try_iso_space(p, time_out, tz_out) == 0)
        return 0;
    if (try_rfc2822(p, time_out, tz_out) == 0)
        return 0;
    return -1;
}

static int resolve_ident(const char *name_var, const char *email_var, const char *date_var,
                         sg_ident *out, const char **bad_value_out)
{
    const char *name = env_or(name_var, "small_git");
    const char *email = env_or(email_var, "sg@localhost");
    const char *date_raw = getenv(date_var);

    memset(out, 0, sizeof(*out));
    if (bad_value_out != NULL)
        *bad_value_out = NULL;

    snprintf(out->name, sizeof(out->name), "%s", name);
    snprintf(out->email, sizeof(out->email), "%s", email);

    if (date_raw == NULL || date_raw[0] == '\0') {
        long long now = (long long)time(NULL);

        out->when = now;
        if (fill_local_offset(now, out->tz) != 0) {
            /* localtime_r itself failing on the live wall clock would be a
               platform-level failure with no good recovery; fall back to
               "+0000" rather than propagating a spurious date error for an
               absent env var. */
            strcpy(out->tz, "+0000");
        }
        return 0;
    }

    if (sg_ident_parse_date(date_raw, &out->when, out->tz) != 0) {
        memset(out, 0, sizeof(*out));
        if (bad_value_out != NULL)
            *bad_value_out = date_raw;
        return -1;
    }
    return 0;
}

int sg_ident_author(sg_ident *out, const char **bad_value_out)
{
    return resolve_ident("GIT_AUTHOR_NAME", "GIT_AUTHOR_EMAIL", "GIT_AUTHOR_DATE", out,
                         bad_value_out);
}

int sg_ident_committer(sg_ident *out, const char **bad_value_out)
{
    return resolve_ident("GIT_COMMITTER_NAME", "GIT_COMMITTER_EMAIL", "GIT_COMMITTER_DATE", out,
                         bad_value_out);
}
