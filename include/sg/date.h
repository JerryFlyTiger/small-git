#ifndef SG_DATE_H
#define SG_DATE_H

#include <stddef.h>

/* Longest string sg_date_format_normal can produce, including the NUL.
   "Www Mmm D HH:MM:SS YYYY +ZZZZ" is 29 for a four-digit year; the slack
   covers a year outside that range. */
#define SG_DATE_NORMAL_MAX 64

/* Renders a commit/tag timestamp the way git's DATE_NORMAL does -- the
   `Date:` line of `git log`, and `%ad`. Writes e.g.
   "Tue Nov 14 22:13:20 2023 +0000", INCLUDING the timezone, so that this
   function owns the whole format rather than half of it.

   `time_sec` is the epoch seconds as stored in the object, `tz` the offset
   exactly as stored ("+0800"). Measured against git 2.55.0:

   - the wall clock shown is `time_sec` SHIFTED INTO `tz`, not UTC and not
     the machine's local time: 1700000000 renders as 22:13:20 at +0000 and
     as 06:13:20 (the next day) at +0800. Half-hour offsets and negative
     ones behave as the arithmetic says (+0530 -> 03:43:20, -1100 ->
     11:13:20).
   - the day of month is NOT padded: "Nov 14" but "Jan 1".
   - `tz` is echoed back verbatim, never recomputed, so an offset git
     itself would not have written survives round-trip -- with ONE
     measured exception: the exact stored value "-0000" is normalized to
     "+0000" (git's own DATE_NORMAL does this; `--pretty=raw` and
     `sg cat-file -p`, which print the object's stored bytes directly
     rather than going through this function, do NOT, and still show
     "-0000" verbatim -- see `sg_date_format_rfc2822`/`_iso`'s own header
     comments, which share this exact rule via `normalize_tz_for_display`
     in date.c).

   Returns 0 on success, -1 if the result does not fit or the shifted time
   cannot be converted, in which case `out` is set to the empty string. */
int sg_date_format_normal(long long time_sec, const char *tz,
                          char *out, size_t out_size);

/* Longest string sg_date_format_short can produce, including the NUL.
   "YYYY-MM-DD" is 11 for a four-digit year; the slack covers a year outside
   that range. */
#define SG_DATE_SHORT_MAX 16

/* Renders a commit/tag timestamp as git's `%as` / `--pretty=reference`'s
   date field does: "YYYY-MM-DD", the same SHIFTED-INTO-`tz` wall clock
   `sg_date_format_normal` uses (Phase 60), not UTC and not the machine's
   local time. Returns 0 on success, -1 on the same failures
   `sg_date_format_normal` has (`out` set to the empty string). */
int sg_date_format_short(long long time_sec, const char *tz,
                         char *out, size_t out_size);

/* Longest string sg_date_format_rfc2822 can produce, including the NUL. */
#define SG_DATE_RFC2822_MAX 48

/* Renders `%aD`/`%cD`: "Wed, 15 Nov 2023 06:13:20 +0800" -- git's RFC2822
   date format. Same SHIFTED-INTO-`tz` wall clock and hard-coded English
   weekday/month tables as sg_date_format_normal; the day of month is NOT
   zero-padded either (measured: day 4 renders "Sat, 4 Nov 2023", not
   "Sat, 04 ..."). Shares sg_date_format_normal's "-0000" -> "+0000"
   normalization (see that function's own header comment) via the same
   `normalize_tz_for_display` helper. Returns 0 on success, -1 on the same
   failures sg_date_format_normal has. */
int sg_date_format_rfc2822(long long time_sec, const char *tz,
                           char *out, size_t out_size);

/* Longest string sg_date_format_iso/_strict can produce, including the NUL. */
#define SG_DATE_ISO_MAX 32

/* Renders `%ai`/`%ci`: "2023-11-15 06:13:20 +0800" (a space between the date
   and time halves, `tz` echoed verbatim as stored, same shifted wall clock,
   same "-0000" -> "+0000" normalization as sg_date_format_normal/_rfc2822 --
   see sg_date_format_normal's own header comment for the exception this
   does NOT apply to). Returns 0 on success, -1 on the same failures
   sg_date_format_normal has. */
int sg_date_format_iso(long long time_sec, const char *tz,
                       char *out, size_t out_size);

/* Renders `%aI`/`%cI`: "2023-11-15T06:13:20+08:00" -- a literal 'T'
   separator and the timezone written with a colon between hours and
   minutes. The colon form is only inserted when `tz` has the expected
   "+HHMM"/"-HHMM" shape (5 bytes, sign + 4 digits); anything else is echoed
   verbatim with no colon inserted, the same "never recomputed" rule
   sg_date_format_normal documents for a malformed tz. A ZERO offset is a
   further exception on top of that (measured against real git 2.55.0):
   "+0000" and "-0000" both render as a literal "Z", never "+00:00" or
   "-00:00". Returns 0 on success, -1 on the same failures
   sg_date_format_normal has. */
int sg_date_format_iso_strict(long long time_sec, const char *tz,
                              char *out, size_t out_size);

/* Phase 64: the `--date=<name>` model shared by `sg log`/`sg show`, both
   for the header date line(s) and for %ad/%cd. CLAUDE.md's `--date=` entry
   has the full measured grammar and byte tables; the short version:
   - DEFAULT/ISO/ISO_STRICT/RFC2822/SHORT dispatch to the five renderers
     above, unchanged, when `local` is 0 (byte-identical to calling them
     directly with the object's own stored tz).
   - `local` ignores the stored tz completely and shifts by the machine's
     OWN offset at time_sec's instant (localtime_r on time_sec, never a
     cached value or "now" -- this is a genuine per-commit computation,
     verified across a DST boundary). DEFAULT+local additionally SUPPRESSES
     the offset field entirely ("Mon Jan 1 09:00:00 2024", no "+0900") --
     the one shape none of the five existing renderers can produce.
   - RAW ("<epoch> <tz>") and UNIX ("<epoch>") are new, trivial formats.
     RAW shares the "-0000" -> "+0000" normalization the five existing
     renderers apply (measured against real git with a hand-crafted
     commit object storing a literal "-0000": every rendering normalizes
     it, RAW and FORMAT's %z included -- only `sg cat-file -p`/
     `--pretty=raw` echo the stored bytes verbatim, and neither of those
     goes through this function at all).
   - FORMAT is git's `strbuf_addftime`: %s/%z/%Z/%% are substituted by hand
     before the remaining bytes reach the system `strftime` (see date.c for
     the measured substitution rules) -- %s is always the UNSHIFTED epoch,
     %z is the offset actually being rendered (stored tz, or the local
     offset under `-local`), %Z is the zone name (empty unless `local`). */
typedef enum {
    SG_DATE_DEFAULT,
    SG_DATE_ISO,
    SG_DATE_ISO_STRICT,
    SG_DATE_RFC2822,
    SG_DATE_SHORT,
    SG_DATE_RAW,
    SG_DATE_UNIX,
    SG_DATE_FORMAT,
    SG_DATE_RELATIVE /* Phase 66: "relative" / "relative-local" -- see
                         sg_date_format_relative below. */
} sg_date_kind;

typedef struct {
    sg_date_kind kind;
    int local; /* the "-local" suffix / "format-local:" prefix */
    /* Borrowed pointer into the caller's argv string, past "format:"/
       "format-local:" -- only meaningful when kind == SG_DATE_FORMAT. May
       be "" (an empty format prints zero bytes, not even under -local). */
    const char *strftime_fmt;
} sg_date_mode;

/* A generous buffer size for sg_date_format_mode when kind is anything
   other than SG_DATE_FORMAT: every one of the other seven kinds has an
   output length bounded by this project's own tables (weekday/month names,
   a four-digit-or-so year, a fixed number of separator bytes), so a stack
   buffer of this size is always big enough for them.

   SG_DATE_FORMAT's output length is bounded only by the caller's own
   format string, which is unbounded user input (Phase 64 review: a
   `--pretty=format:` string that is itself thousands of bytes long, or a
   strftime expansion of one, silently truncated to an empty string here --
   `sg_date_format_mode` correctly returned -1 for "does not fit", but
   every one of its FORMAT-mode callers turned that -1 into an empty
   string rather than growing the buffer). **Do not raise this constant to
   paper over that** -- it only moves the same silent-truncation bug to a
   longer input, it does not fix it. A FORMAT-mode caller must use
   sg_date_format_mode_alloc below instead of a fixed buffer of this size. */
#define SG_DATE_MODE_MAX 1024

/* Parses the text after "--date="/"--date " (CLAUDE.md's --date= entry has
   the full grammar): a case-SENSITIVE match against one of `default` /
   `iso`|`iso8601` / `iso-strict`|`iso8601-strict` / `rfc`|`rfc2822` /
   `short` / `raw` / `unix`, each optionally suffixed with "-local" (the
   suffix is stripped AT MOST ONCE, and the remainder must itself be one of
   those names -- "local-local" and "-local" are both errors); the bare
   literal `local` is an alias for `default-local`; and "format:"/
   "format-local:" are prefixes checked FIRST (before any suffix-stripping)
   with everything after the colon taken verbatim, "-local" appearing
   inside the format string included.

   `relative`/`relative-local` are implemented as of Phase 66 (see
   sg_date_format_relative below) -- both parse to SG_DATE_RELATIVE, and
   `-local` is accepted but changes nothing about the rendering (a
   duration has no timezone to shift). Deliberately still unimplemented
   and always rejected: `human`/`human-local` (measured to be
   CALENDAR-driven, not duration-driven -- a genuinely different algorithm,
   deferred to its own phase) and `auto:<anything>` (its meaning depends on
   isatty(1), no oracle without a pty). Returns 0 with *out filled, or -1
   for any of those, an unknown name, or an empty string. */
int sg_date_parse_mode(const char *arg, sg_date_mode *out);

/* Renders time_sec/tz per *mode -- see the type's own comment above for
   the dispatch. Returns 0 on success, -1 on the same failures the five
   renderers above report (out set to the empty string), or if the result
   does not fit in out_size. */
int sg_date_format_mode(const sg_date_mode *mode, long long time_sec,
                        const char *tz, char *out, size_t out_size);

/* Same rendering as sg_date_format_mode, but into a MALLOC'd buffer sized
   to fit -- the caller frees *out. For every kind other than SG_DATE_FORMAT
   this is equivalent to sg_date_format_mode into an SG_DATE_MODE_MAX
   buffer, just heap-allocated; for SG_DATE_FORMAT it grows the buffer as
   many times as needed (bounded only by an internal sanity cap, the same
   one strftime_grow uses internally in date.c) rather than failing past a
   fixed ceiling. Every caller that renders a FORMAT-mode --date= (i.e.
   every one of the four reach points CLAUDE.md's --date= entry names) MUST
   use this function, not a fixed SG_DATE_MODE_MAX stack buffer -- see that
   constant's own comment for the bug this exists to fix.

   Returns 0 with *out set (caller frees), or -1 on the same failures
   sg_date_format_mode reports (*out left NULL) or on allocation failure. */
int sg_date_format_mode_alloc(const sg_date_mode *mode, long long time_sec,
                              const char *tz, char **out);

/* Longest string sg_date_format_relative can produce, including the NUL.
   "in the future" is 14; the numeric forms are bounded by
   date.c's own overflow clamp on the diff it computes, which keeps every
   count well under 20 digits -- 64 bytes covers every case with slack to
   spare. */
#define SG_DATE_RELATIVE_MAX 64

/* Phase 66: git's `--date=relative` / `%ar` / `%cr` algorithm -- a
   DURATION between `time_sec` and `now`, not a wall-clock rendering, so it
   takes no `tz` at all (`relative-local` is measured byte-identical to
   `relative` in every zone, CLAUDE.md's `sg log` --date= entry has the
   measured proof). Re-implemented independently in Python and
   cross-checked against real git 2.55.0 over 1239 probes (0 mismatches);
   see CLAUDE.md / docs/DESIGN.md's Phase 66 entry for the full
   two-formula, unit-threshold table -- do not re-derive it from memory,
   there are two different month formulas and using either one everywhere
   silently produces a wrong answer 1 probe out of 539.

   `diff = now - time_sec` is computed WITH SATURATION, not signed-overflow
   UB: a `time_sec`/`now` pair whose difference cannot be represented is
   clamped to a very large finite value in the appropriate direction (huge
   past, or negative -> "in the future") rather than wrapping. This is a
   defensive net for a hand-crafted or corrupt commit object, not a real
   git behavior to match -- it is unreachable through interop, which only
   ever feeds a legitimate epoch pair.

   `now` is deliberately a caller-supplied parameter rather than sampled
   internally, so both reach points (sg_date_format_mode's SG_DATE_RELATIVE
   case, and %ar/%cr's own call sites in commit_out.c) go through the SAME
   clock decision -- sg_date_now() below -- without this function itself
   depending on getenv/time(2) at all. Returns 0 on success, -1 if the
   result does not fit in out_size (out set to the empty string). */
int sg_date_format_relative(long long time_sec, long long now,
                            char *out, size_t out_size);

/* The single clock accessor for `--date=relative` and `%ar`/`%cr`: reads
   `GIT_TEST_DATE_NOW` (seconds since the epoch, the same variable real git
   honours for its own relative-date rendering -- CLAUDE.md's --date= entry
   has the measurement) and falls back to the real wall clock (`time(NULL)`)
   when it is absent OR malformed.

   Deliberately NOT a byte-for-byte port of git's own parsing of this
   variable: git's test hook does unsigned-wraparound arithmetic on a bad
   value (measured: "abc" and "" both become 0, "1700000000x" parses only
   the numeric prefix, "-5" wraps to a 584942417301-years-ago answer) --
   that is a test hook's incidental behavior, not an interface worth
   reproducing. sg parses a strict decimal integer (optional leading sign,
   every remaining byte a digit, the whole string consumed, no overflow)
   and treats anything else as ABSENT, falling back to the real clock
   rather than silently rendering "in the future" for a typo. This
   divergence is unobservable in interop, which only ever passes a valid
   integer; documented, not pinned (docs/DESIGN.md's Phase 66 entry has the
   measurement).

   This function does not cache its result -- GIT_TEST_DATE_NOW is stable
   for a process's whole lifetime, and the real-clock fallback's drift
   across one invocation's several calls is far below relative's own
   granularity (seconds), so re-reading the environment/clock at each of
   the few call sites that need "now" is simpler than threading a
   once-computed value through every renderer, and was the explicit design
   choice for this phase (see docs/DESIGN.md). */
long long sg_date_now(void);

#endif /* SG_DATE_H */
