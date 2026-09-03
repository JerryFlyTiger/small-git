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

#endif /* SG_DATE_H */
