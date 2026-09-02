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
     itself would not have written survives round-trip.

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

#endif /* SG_DATE_H */
