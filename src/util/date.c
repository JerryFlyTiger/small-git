#include "sg/date.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* git's own tables (`date.c`'s weekday_names/month_names, abbreviated), NOT
   strftime's %a/%b. strftime follows the locale, and git's names are
   hard-coded English: on a localized machine strftime would quietly answer
   in another language while real git kept printing "Tue", and the whole
   point of this function is to agree with real git byte for byte. */
static const char *const WDAY[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};
static const char *const MON[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/* "+HHMM" / "-HHMM" -> seconds. Anything else fails; the caller then shifts
   by nothing rather than guessing, and still echoes the stored string. */
static int parse_tz(const char *tz, long *out)
{
    long hours, minutes;
    size_t i;

    if (tz == NULL || strlen(tz) != 5)
        return -1;
    if (tz[0] != '+' && tz[0] != '-')
        return -1;
    for (i = 1; i < 5; i++) {
        if (tz[i] < '0' || tz[i] > '9')
            return -1;
    }
    hours = (tz[1] - '0') * 10 + (tz[2] - '0');
    minutes = (tz[3] - '0') * 10 + (tz[4] - '0');
    *out = (hours * 3600 + minutes * 60) * (tz[0] == '-' ? -1 : 1);
    return 0;
}

/* Shared by sg_date_format_normal and sg_date_format_short: shifts
   time_sec into tz and fills *tmv. Returns 0, or -1 on the same overflow/
   invalid-conversion failures both callers report. */
static int shift_tm(long long time_sec, const char *tz, struct tm *tmv)
{
    long offset = 0;
    long long shifted;
    time_t t;

    if (tz == NULL)
        tz = "+0000";
    if (parse_tz(tz, &offset) != 0)
        offset = 0;

    /* `time_sec + offset` can overflow when time_sec is itself an
       out-of-range value -- a hand-crafted or injected loose object whose
       author/committer line lets strtoll saturate to LLONG_MAX/LLONG_MIN
       is enough (real git's `hash-object -w` rejects such an object via
       fsck; sg's own commit parser does not reject the timestamp field at
       all, so this function has no way to tell such an object apart from
       a legitimate one). Signed integer overflow is undefined behavior in
       C, caught by UBSan -- check the bounds before adding instead of
       after. `offset` is at most a few hundred thousand seconds in
       magnitude (parse_tz's widest legal input is "+9999"/"-9999"), so
       computing `LLONG_MAX - offset` / `LLONG_MIN - offset` cannot itself
       overflow. */
    if (offset >= 0) {
        if (time_sec > LLONG_MAX - offset)
            return -1;
    } else {
        if (time_sec < LLONG_MIN - offset)
            return -1;
    }
    shifted = time_sec + offset;
    t = (time_t)shifted;
    if ((long long)t != shifted)
        return -1;
    if (gmtime_r(&t, tmv) == NULL)
        return -1;
    if (tmv->tm_wday < 0 || tmv->tm_wday > 6 || tmv->tm_mon < 0 || tmv->tm_mon > 11)
        return -1;
    return 0;
}

/* git normalizes exactly the stored value "-0000" to "+0000" when
   rendering a date for a human -- DATE_NORMAL (%ad), RFC2822 (%aD), and
   ISO (%ai) all do this; measured against real git 2.55.0. Every OTHER
   value, including the ordinary "+0000" and every non-zero offset
   ("-0030", "-0100", "+0030"), is echoed completely unchanged -- this is
   NOT a general sign-normalization, only that one exact 5-byte sequence is
   special. `--pretty=raw` and `sg cat-file -p` do NOT go through this (or
   any) rendering function at all -- they print the commit object's own
   stored bytes directly, so they echo a stored "-0000" verbatim, matching
   real git measured the same way; do not "fix" that path to match this
   one, they answer different questions. `%aI` (ISO strict) also does not
   call this -- it already collapses BOTH "+0000" and "-0000" to a literal
   "Z" before ever reaching a tz-string branch, so this rule never applies
   to it. Shared by all three affected renderers so a future date format
   cannot independently re-derive (and get wrong) this same one-line rule
   the way sg_date_format_rfc2822/_iso did in Phase 60b -- both were newly
   written alongside this exact "-0000" bug already present in
   sg_date_format_normal since Phase 54, and both copied its "echo tz
   verbatim" comment without noticing the one designed exception to it.
   Returns tz unchanged unless it is exactly "-0000", in which case it
   returns the string literal "+0000". */
static const char *normalize_tz_for_display(const char *tz)
{
    if (tz != NULL && strcmp(tz, "-0000") == 0)
        return "+0000";
    return tz;
}

int sg_date_format_normal(long long time_sec, const char *tz,
                          char *out, size_t out_size)
{
    struct tm tmv;
    int written;

    if (out == NULL || out_size == 0)
        return -1;
    out[0] = '\0';
    if (tz == NULL)
        tz = "+0000";
    if (shift_tm(time_sec, tz, &tmv) != 0)
        return -1;

    /* Day of month with %d, not %02d: git pads neither. */
    written = snprintf(out, out_size, "%s %s %d %02d:%02d:%02d %d %s",
                       WDAY[tmv.tm_wday], MON[tmv.tm_mon], tmv.tm_mday,
                       tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                       tmv.tm_year + 1900, normalize_tz_for_display(tz));
    if (written < 0 || (size_t)written >= out_size) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

int sg_date_format_short(long long time_sec, const char *tz,
                         char *out, size_t out_size)
{
    struct tm tmv;
    int written;

    if (out == NULL || out_size == 0)
        return -1;
    out[0] = '\0';
    if (shift_tm(time_sec, tz, &tmv) != 0)
        return -1;

    written = snprintf(out, out_size, "%04d-%02d-%02d",
                       tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    if (written < 0 || (size_t)written >= out_size) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

int sg_date_format_rfc2822(long long time_sec, const char *tz,
                           char *out, size_t out_size)
{
    struct tm tmv;
    int written;

    if (out == NULL || out_size == 0)
        return -1;
    out[0] = '\0';
    if (tz == NULL)
        tz = "+0000";
    if (shift_tm(time_sec, tz, &tmv) != 0)
        return -1;

    /* Day of month with %d, not %02d -- measured: day 4 renders as
       "Sat, 4 Nov 2023", not "Sat, 04 ...". */
    written = snprintf(out, out_size, "%s, %d %s %d %02d:%02d:%02d %s",
                       WDAY[tmv.tm_wday], tmv.tm_mday, MON[tmv.tm_mon],
                       tmv.tm_year + 1900, tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                       normalize_tz_for_display(tz));
    if (written < 0 || (size_t)written >= out_size) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

int sg_date_format_iso(long long time_sec, const char *tz,
                       char *out, size_t out_size)
{
    struct tm tmv;
    int written;

    if (out == NULL || out_size == 0)
        return -1;
    out[0] = '\0';
    if (tz == NULL)
        tz = "+0000";
    if (shift_tm(time_sec, tz, &tmv) != 0)
        return -1;

    written = snprintf(out, out_size, "%04d-%02d-%02d %02d:%02d:%02d %s",
                       tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                       tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                       normalize_tz_for_display(tz));
    if (written < 0 || (size_t)written >= out_size) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

int sg_date_format_iso_strict(long long time_sec, const char *tz,
                              char *out, size_t out_size)
{
    struct tm tmv;
    char tz_colon[7];
    const char *tz_out;
    int written;

    if (out == NULL || out_size == 0)
        return -1;
    out[0] = '\0';
    if (tz == NULL)
        tz = "+0000";
    if (shift_tm(time_sec, tz, &tmv) != 0)
        return -1;

    /* Insert a colon between hours and minutes only when tz has the
       expected "+HHMM"/"-HHMM" shape -- a malformed tz is echoed verbatim,
       same "never recomputed" rule as sg_date_format_normal. A ZERO offset
       (either sign, "+0000" or "-0000") is the one exception: measured
       against real git, it renders as a literal "Z", not "+00:00". */
    tz_out = tz;
    if (strlen(tz) == 5 && (tz[0] == '+' || tz[0] == '-') &&
       tz[1] >= '0' && tz[1] <= '9' && tz[2] >= '0' && tz[2] <= '9' &&
       tz[3] >= '0' && tz[3] <= '9' && tz[4] >= '0' && tz[4] <= '9') {
        if (tz[1] == '0' && tz[2] == '0' && tz[3] == '0' && tz[4] == '0') {
            tz_out = "Z";
        } else {
            tz_colon[0] = tz[0];
            tz_colon[1] = tz[1];
            tz_colon[2] = tz[2];
            tz_colon[3] = ':';
            tz_colon[4] = tz[3];
            tz_colon[5] = tz[4];
            tz_colon[6] = '\0';
            tz_out = tz_colon;
        }
    }

    written = snprintf(out, out_size, "%04d-%02d-%02dT%02d:%02d:%02d%s",
                       tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                       tmv.tm_hour, tmv.tm_min, tmv.tm_sec, tz_out);
    if (written < 0 || (size_t)written >= out_size) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------------------
   Phase 64: --date=<name> model.
   -------------------------------------------------------------------- */

/* "+HHMM"/"-HHMM" from a signed offset in seconds. `buf` must be at least
   6 bytes. Zero is always rendered with a '+' sign (matches parse_tz's own
   round trip, and means a computed local offset of exactly zero can never
   collide with the "-0000" normalization rule below). */
static void format_offset_str(long offset, char *buf, size_t size)
{
    char sign = offset < 0 ? '-' : '+';
    long mag = offset < 0 ? -offset : offset;

    snprintf(buf, size, "%c%02ld%02ld", sign, mag / 3600, (mag % 3600) / 60);
}

/* The machine's own offset (and zone abbreviation, when available) at
   time_sec's instant -- localtime_r, never a cached value or "now", so a
   DST boundary is answered correctly per commit. zone_out may be NULL;
   when non-NULL it is always NUL-terminated, empty if tm_zone is
   unavailable. Returns -1 if time_sec does not fit a time_t or
   localtime_r itself fails. */
static int local_offset_at(long long time_sec, long *offset_out,
                           char *zone_out, size_t zone_out_size)
{
    time_t t = (time_t)time_sec;
    struct tm tmv;

    if ((long long)t != time_sec)
        return -1;
    if (localtime_r(&t, &tmv) == NULL)
        return -1;
    *offset_out = tmv.tm_gmtoff;
    if (zone_out != NULL) {
        if (tmv.tm_zone != NULL)
            snprintf(zone_out, zone_out_size, "%s", tmv.tm_zone);
        else
            zone_out[0] = '\0';
    }
    return 0;
}

/* SG_DATE_DEFAULT with `local` set: same shape as sg_date_format_normal,
   but the offset field is omitted entirely -- the one shape none of the
   five original renderers can produce (they all always print a tz). */
static int format_default_no_offset(long long time_sec, const char *tz,
                                    char *out, size_t out_size)
{
    struct tm tmv;
    int written;

    if (shift_tm(time_sec, tz, &tmv) != 0)
        return -1;
    written = snprintf(out, out_size, "%s %s %d %02d:%02d:%02d %d",
                       WDAY[tmv.tm_wday], MON[tmv.tm_mon], tmv.tm_mday,
                       tmv.tm_hour, tmv.tm_min, tmv.tm_sec, tmv.tm_year + 1900);
    if (written < 0 || (size_t)written >= out_size) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

/* A tiny growable byte buffer, local to this file -- used only to build the
   intermediate string for FORMAT mode (%s/%z/%Z substituted, everything
   else passed through verbatim for the system strftime to interpret). Not
   the project-wide sg_buf (that lives in http.h, a layering this file has
   no business depending on for three lines of string building). */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} date_dstr;

static int date_dstr_init(date_dstr *d)
{
    d->cap = 64;
    d->len = 0;
    d->buf = malloc(d->cap);
    if (d->buf == NULL)
        return -1;
    d->buf[0] = '\0';
    return 0;
}

static int date_dstr_append(date_dstr *d, const char *s, size_t n)
{
    if (d->len + n + 1 > d->cap) {
        size_t new_cap = d->cap;
        char *p;

        while (d->len + n + 1 > new_cap)
            new_cap *= 2;
        p = realloc(d->buf, new_cap);
        if (p == NULL)
            return -1;
        d->buf = p;
        d->cap = new_cap;
    }
    memcpy(d->buf + d->len, s, n);
    d->len += n;
    d->buf[d->len] = '\0';
    return 0;
}

/* git's strbuf_addftime: %s/%z/%Z/%% (and every other %-sequence) are
   scanned in ONE pass -- %s/%z/%Z are replaced with plain text right here
   (a hand-built struct tm cannot carry any of the three portably to a
   system strftime), everything else -- including "%%" itself and an
   unrecognized specifier -- is copied through unchanged for strftime to
   interpret on its own. This is why "%%z" -> intermediate "%%z" ->
   strftime -> "%z" (the %% is never treated as "protecting" a %z that
   follows it; it is simply its own two-byte token), while "%z%%z" ->
   intermediate "<offset>%%z" -> strftime -> "<offset>%z". A trailing lone
   '%' is copied through as a single byte (p[1] == '\0'), matching the
   measured "prints %" rule. Returns 0 with *out (malloc'd, caller frees)
   set, or -1 on OOM. */
static int build_format_intermediate(const char *fmt, long long time_sec,
                                     const char *tz_str, const char *zone_name,
                                     char **out)
{
    date_dstr d;
    const char *p = fmt;

    if (date_dstr_init(&d) != 0)
        return -1;

    while (*p != '\0') {
        if (p[0] == '%') {
            char c = p[1];

            if (c == 's') {
                char buf[32];
                int n = snprintf(buf, sizeof buf, "%lld", time_sec);

                if (n < 0 || date_dstr_append(&d, buf, (size_t)n) != 0) {
                    free(d.buf);
                    return -1;
                }
                p += 2;
            } else if (c == 'z') {
                if (date_dstr_append(&d, tz_str, strlen(tz_str)) != 0) {
                    free(d.buf);
                    return -1;
                }
                p += 2;
            } else if (c == 'Z') {
                if (date_dstr_append(&d, zone_name, strlen(zone_name)) != 0) {
                    free(d.buf);
                    return -1;
                }
                p += 2;
            } else if (c == '\0') {
                if (date_dstr_append(&d, "%", 1) != 0) {
                    free(d.buf);
                    return -1;
                }
                p += 1;
            } else {
                if (date_dstr_append(&d, p, 2) != 0) {
                    free(d.buf);
                    return -1;
                }
                p += 2;
            }
        } else {
            if (date_dstr_append(&d, p, 1) != 0) {
                free(d.buf);
                return -1;
            }
            p++;
        }
    }

    *out = d.buf;
    return 0;
}

/* Sanity cap for the growing strftime buffer below -- the ONLY growth
   ceiling in this file for SG_DATE_FORMAT rendering (see the "one growth
   ceiling, not two" note below for why a second one used to exist and
   what broke because of it).

   Derived, not arbitrary -- both numbers below were measured, not
   assumed:
     - Under `LC_ALL=C`, the largest expansion ratio of any single
       printable-ASCII strftime conversion is 14x: `%+` maps 2 input bytes
       to a 28-byte "date(1) format" expansion, `%c` is next at 24 bytes
       (a full sweep of every specifier was run against this project's own
       `sg_date_format_mode` fixture and cross-checked with glibc's
       strftime(3) man page).
     - The largest single argv string reachable from the CLI is bounded by
       the OS: this machine's `getconf ARG_MAX` is 1048576 (1 MiB, and
       that figure covers argv+environ combined, not just one string);
       Linux additionally caps any ONE argv string at `MAX_ARG_STRLEN`,
       128 KiB. So a `--date=format:<fmt>` argument arriving through argv
       is comfortably under 1 MiB either way.
   1 MiB of format text at a 14x ratio is at most ~14 MiB of rendered
   output. The cap below is set to 1 << 28 (256 MiB), about 19x that
   figure -- enough headroom that the cap is never the thing an ordinary
   `--date=format:` invocation can hit, while still bounding the loop
   below at a size this project's other large-input paths (e.g. the pack
   parser) already treat as ordinary. */
#define DATE_STRFTIME_GROW_MAX (1u << 28)

/* strftime returning 0 is genuinely ambiguous on its own: POSIX gives it
   that exact return value both for "the buffer was too small" and for "the
   conversion legitimately produced zero bytes" (e.g. a lone "%p" with a
   "C" locale that has no AM/PM designation, or "%n"/"%t" landing on a
   platform-specific empty expansion -- measured to differ between macOS
   and glibc, which is exactly why this project cannot settle the question
   by testing on one platform, see CLAUDE.md's local-sanitizers-differ note
   for the same shape of trap). Real git resolves this ambiguity instead of
   trusting a platform assumption: it prepends one sentinel byte to the
   format string before calling strftime, then discards that byte (and only
   that byte) from the result. With the sentinel present, ANY successful
   render is at least 1 byte long, so `n == 0` can only ever mean "did not
   fit" -- there is no longer a legitimate-empty-output case to confuse it
   with. The sentinel is an ordinary printed-nowhere-else control byte; it
   is copied through literally by strftime (a `%`-format only ever touches
   bytes at or after a `%`, so a leading non-`%` byte is untouched) and
   never reaches the caller. */
#define DATE_STRFTIME_SENTINEL '\x01'

/* Grows a buffer until strftime succeeds (or the intermediate format is
   itself empty, handled by the caller before this is reached) or the
   sanity cap above is hit. Returns 0 with *out (malloc'd, caller frees,
   the sentinel byte already stripped) and *out_len set, or -1. */
static int strftime_grow(const char *fmt, const struct tm *tmv,
                         char **out, size_t *out_len)
{
    size_t fmt_len = strlen(fmt);
    char *sentinel_fmt = malloc(fmt_len + 2);
    size_t cap = 128;

    if (sentinel_fmt == NULL)
        return -1;
    sentinel_fmt[0] = DATE_STRFTIME_SENTINEL;
    memcpy(sentinel_fmt + 1, fmt, fmt_len + 1); /* +1 copies the NUL too */

    for (;;) {
        char *p = malloc(cap);
        size_t n;

        if (p == NULL) {
            free(sentinel_fmt);
            return -1;
        }
        n = strftime(p, cap, sentinel_fmt, tmv);
        if (n > 0) {
            /* n counts the sentinel byte too (it was copied through as
               ordinary output); strip it before handing the result back. */
            char *stripped = malloc(n);

            if (stripped == NULL) {
                free(p);
                free(sentinel_fmt);
                return -1;
            }
            memcpy(stripped, p + 1, n - 1);
            stripped[n - 1] = '\0';
            free(p);
            free(sentinel_fmt);
            *out = stripped;
            *out_len = n - 1;
            return 0;
        }
        free(p);
        if (cap >= DATE_STRFTIME_GROW_MAX) {
            free(sentinel_fmt);
            return -1;
        }
        cap *= 2;
    }
}

/* Resolves the tz string and (for -local) the zone abbreviation a mode
   needs to render with. `local_tz_buf`/`local_zone_buf` are caller-owned
   scratch space (at least 8 / 64 bytes respectively, matching the stack
   buffers every pre-Phase-64-review caller used inline); `*tz_str_out` and
   `*zone_name_out` point into either those buffers or the caller's own
   `tz` argument, and are only valid as long as the scratch buffers are.
   Factored out so sg_date_format_mode and sg_date_format_mode_alloc's
   SG_DATE_FORMAT fast path (which bypasses sg_date_format_mode entirely,
   see render_format_mode_alloc below) cannot independently re-derive --
   and risk drifting apart on -- this exact "-local" resolution rule.
   Returns -1 if local_offset_at fails, matching sg_date_format_mode's own
   pre-existing failure there. */
static int resolve_mode_tz(const sg_date_mode *mode, long long time_sec,
                           const char *tz, char *local_tz_buf, size_t local_tz_size,
                           char *local_zone_buf, size_t local_zone_size,
                           const char **tz_str_out, const char **zone_name_out)
{
    if (tz == NULL)
        tz = "+0000";
    if (mode->local) {
        long local_off = 0;

        if (local_offset_at(time_sec, &local_off, local_zone_buf, local_zone_size) != 0)
            return -1;
        format_offset_str(local_off, local_tz_buf, local_tz_size);
        *tz_str_out = local_tz_buf;
        *zone_name_out = local_zone_buf;
    } else {
        *tz_str_out = tz;
        *zone_name_out = "";
    }
    return 0;
}

/* Renders SG_DATE_FORMAT mode (`fmt` may be "") into a freshly malloc'd,
   NUL-terminated string -- the ONE place that calls strftime_grow, shared
   by sg_date_format_mode's fixed-buffer FORMAT case (which copies the
   result into the caller's buffer, failing if it does not fit) and
   sg_date_format_mode_alloc's FORMAT case (which takes ownership of the
   buffer directly). Before this refactor the two had SEPARATE growth
   ceilings -- sg_date_format_mode_alloc doubled ITS OWN buffer up to a
   16 MiB cap while calling sg_date_format_mode at each new size, but
   sg_date_format_mode's FORMAT case unconditionally deferred to
   strftime_grow's OWN, independent 1 MiB cap; growing the outer buffer
   past 1 MiB could never help, because the inner call would fail
   identically at every size past that point. Measured directly: a
   --date=format: string whose rendered output is ~1.05 MiB (`%c` x
   43691) rendered as a silently empty string, exit 0 -- the exact bug
   this project's own Phase 64 review round had just fixed for the
   *shorter* 1024-byte ceiling, reappearing one boundary up. There must be
   exactly one growth ceiling for this rendering path, enforced in exactly
   one place (DATE_STRFTIME_GROW_MAX, inside strftime_grow itself).
   Returns 0 with *out and *out_len set (out is "", out_len 0, for an
   empty `fmt` or an intermediate that substitutes to nothing -- matching
   sg_date_format_mode's own pre-existing early-return rule), or -1. */
static int render_format_mode_alloc(long long time_sec, const char *tz_str,
                                    const char *zone_name, const char *fmt,
                                    char **out, size_t *out_len)
{
    char *intermediate = NULL;
    struct tm tmv;
    int rc;

    if (fmt[0] == '\0') {
        *out = strdup("");
        *out_len = 0;
        return *out != NULL ? 0 : -1;
    }
    if (shift_tm(time_sec, tz_str, &tmv) != 0)
        return -1;
    if (build_format_intermediate(fmt, time_sec, normalize_tz_for_display(tz_str),
                                  zone_name, &intermediate) != 0)
        return -1;
    if (intermediate[0] == '\0') {
        free(intermediate);
        *out = strdup("");
        *out_len = 0;
        return *out != NULL ? 0 : -1;
    }
    rc = strftime_grow(intermediate, &tmv, out, out_len);
    free(intermediate);
    return rc;
}

static const struct {
    const char *name;
    sg_date_kind kind;
} DATE_MODE_NAMES[] = {
    { "default", SG_DATE_DEFAULT },
    { "iso", SG_DATE_ISO },
    { "iso8601", SG_DATE_ISO },
    { "iso-strict", SG_DATE_ISO_STRICT },
    { "iso8601-strict", SG_DATE_ISO_STRICT },
    { "rfc", SG_DATE_RFC2822 },
    { "rfc2822", SG_DATE_RFC2822 },
    { "short", SG_DATE_SHORT },
    { "raw", SG_DATE_RAW },
    { "unix", SG_DATE_UNIX },
};

int sg_date_parse_mode(const char *arg, sg_date_mode *out)
{
    const char *base;
    size_t base_len;
    size_t i;

    if (arg == NULL || out == NULL || arg[0] == '\0')
        return -1;
    out->local = 0;
    out->strftime_fmt = NULL;

    /* "format:"/"format-local:" are prefixes, checked BEFORE any
       "-local"-suffix logic -- the format string itself may legitimately
       contain "-local" as literal text ("format:x-local" prints
       "x-local", not "x"), so it must never be stripped. */
    if (strncmp(arg, "format-local:", 13) == 0) {
        out->kind = SG_DATE_FORMAT;
        out->local = 1;
        out->strftime_fmt = arg + 13;
        return 0;
    }
    if (strncmp(arg, "format:", 7) == 0) {
        out->kind = SG_DATE_FORMAT;
        out->strftime_fmt = arg + 7;
        return 0;
    }

    /* The bare literal "local" is an alias for "default-local" -- it does
       NOT go through the general suffix-stripping path below (it has no
       leading '-' to strip). */
    if (strcmp(arg, "local") == 0) {
        out->kind = SG_DATE_DEFAULT;
        out->local = 1;
        return 0;
    }

    /* The "-local" suffix is stripped AT MOST ONCE: the remainder must
       itself match one of the base names below, so "local-local" (strips
       to "local", not a base name) and "-local" (strips to "", not a base
       name either) are both errors, and "default-local-local" (strips to
       "default-local", also not a base name) never gets a second strip. */
    base = arg;
    base_len = strlen(arg);
    if (base_len > 6 && strcmp(arg + base_len - 6, "-local") == 0) {
        out->local = 1;
        base_len -= 6;
    }

    for (i = 0; i < sizeof(DATE_MODE_NAMES) / sizeof(DATE_MODE_NAMES[0]); i++) {
        if (strlen(DATE_MODE_NAMES[i].name) == base_len &&
           strncmp(DATE_MODE_NAMES[i].name, base, base_len) == 0) {
            out->kind = DATE_MODE_NAMES[i].kind;
            return 0;
        }
    }
    return -1;
}

int sg_date_format_mode(const sg_date_mode *mode, long long time_sec,
                        const char *tz, char *out, size_t out_size)
{
    char local_tz[8];
    char local_zone[64];
    const char *tz_str;
    const char *zone_name;
    int written;

    if (out == NULL || out_size == 0 || mode == NULL)
        return -1;
    out[0] = '\0';

    if (resolve_mode_tz(mode, time_sec, tz, local_tz, sizeof local_tz,
                        local_zone, sizeof local_zone, &tz_str, &zone_name) != 0)
        return -1;

    switch (mode->kind) {
    case SG_DATE_DEFAULT:
        if (mode->local)
            return format_default_no_offset(time_sec, tz_str, out, out_size);
        return sg_date_format_normal(time_sec, tz_str, out, out_size);
    case SG_DATE_ISO:
        return sg_date_format_iso(time_sec, tz_str, out, out_size);
    case SG_DATE_ISO_STRICT:
        return sg_date_format_iso_strict(time_sec, tz_str, out, out_size);
    case SG_DATE_RFC2822:
        return sg_date_format_rfc2822(time_sec, tz_str, out, out_size);
    case SG_DATE_SHORT:
        return sg_date_format_short(time_sec, tz_str, out, out_size);
    case SG_DATE_RAW:
        /* Trivial, new: "<epoch> <tz>". Shares the "-0000" -> "+0000"
           normalization every other renderer applies (measured against a
           hand-crafted commit object storing a literal "-0000" -- only
           cat-file -p / --pretty=raw, neither of which reaches this
           function, echo it verbatim). */
        written = snprintf(out, out_size, "%lld %s", time_sec,
                           normalize_tz_for_display(tz_str));
        if (written < 0 || (size_t)written >= out_size) {
            out[0] = '\0';
            return -1;
        }
        return 0;
    case SG_DATE_UNIX:
        /* "-local" changes nothing -- there is no offset field to shift. */
        written = snprintf(out, out_size, "%lld", time_sec);
        if (written < 0 || (size_t)written >= out_size) {
            out[0] = '\0';
            return -1;
        }
        return 0;
    case SG_DATE_FORMAT: {
        const char *fmt = mode->strftime_fmt != NULL ? mode->strftime_fmt : "";
        char *rendered = NULL;
        size_t rendered_len = 0;
        int rc;

        rc = render_format_mode_alloc(time_sec, tz_str, zone_name, fmt,
                                      &rendered, &rendered_len);
        if (rc != 0)
            return -1;
        if (rendered_len >= out_size) {
            free(rendered);
            out[0] = '\0';
            return -1;
        }
        memcpy(out, rendered, rendered_len + 1);
        free(rendered);
        return 0;
    }
    default:
        return -1;
    }
}

int sg_date_format_mode_alloc(const sg_date_mode *mode, long long time_sec,
                              const char *tz, char **out)
{
    char stackbuf[SG_DATE_MODE_MAX];

    if (out == NULL)
        return -1;
    *out = NULL;
    if (mode == NULL)
        return -1;

    if (mode->kind != SG_DATE_FORMAT) {
        /* Every other kind's output fits in SG_DATE_MODE_MAX, per that
           constant's own header comment -- render into a stack buffer and
           copy once, no growth loop needed. */
        if (sg_date_format_mode(mode, time_sec, tz, stackbuf, sizeof stackbuf) != 0)
            return -1;
        *out = strdup(stackbuf);
        return *out != NULL ? 0 : -1;
    }

    /* SG_DATE_FORMAT: go straight to render_format_mode_alloc rather than
       looping over sg_date_format_mode at ever-larger fixed buffer sizes
       -- that used to be a SECOND, independent growth ceiling layered on
       top of strftime_grow's own, and the outer one could never actually
       help past strftime_grow's limit (see render_format_mode_alloc's own
       header comment for the bug that shipped because of it). There is
       now exactly one growth loop for this rendering path, inside
       strftime_grow, bounded by DATE_STRFTIME_GROW_MAX. */
    {
        char local_tz[8];
        char local_zone[64];
        const char *tz_str;
        const char *zone_name;
        const char *fmt = mode->strftime_fmt != NULL ? mode->strftime_fmt : "";
        char *rendered = NULL;
        size_t rendered_len = 0;

        if (resolve_mode_tz(mode, time_sec, tz, local_tz, sizeof local_tz,
                            local_zone, sizeof local_zone, &tz_str, &zone_name) != 0)
            return -1;
        if (render_format_mode_alloc(time_sec, tz_str, zone_name, fmt,
                                     &rendered, &rendered_len) != 0)
            return -1;
        *out = rendered;
        return 0;
    }
}
