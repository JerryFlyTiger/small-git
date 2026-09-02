#include "sg/date.h"

#include <stdio.h>
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
                       tmv.tm_year + 1900, tz);
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
                       tmv.tm_year + 1900, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, tz);
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
                       tmv.tm_hour, tmv.tm_min, tmv.tm_sec, tz);
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
