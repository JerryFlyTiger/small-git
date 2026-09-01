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

int sg_date_format_normal(long long time_sec, const char *tz,
                          char *out, size_t out_size)
{
    long offset = 0;
    long long shifted;
    time_t t;
    struct tm tmv;
    int written;

    if (out == NULL || out_size == 0)
        return -1;
    out[0] = '\0';
    if (tz == NULL)
        tz = "+0000";
    if (parse_tz(tz, &offset) != 0)
        offset = 0;

    shifted = time_sec + offset;
    t = (time_t)shifted;
    if ((long long)t != shifted)
        return -1;
    if (gmtime_r(&t, &tmv) == NULL)
        return -1;
    if (tmv.tm_wday < 0 || tmv.tm_wday > 6 || tmv.tm_mon < 0 || tmv.tm_mon > 11)
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
