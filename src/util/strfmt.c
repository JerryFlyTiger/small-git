#include "sg/strfmt.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

char *sg_strfmt_alloc(const char *fmt, ...)
{
    va_list ap;
    int need;
    char *out;

    va_start(ap, fmt);
    need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0)
        return NULL;

    out = malloc((size_t)need + 1);
    if (out == NULL)
        return NULL;

    va_start(ap, fmt);
    vsnprintf(out, (size_t)need + 1, fmt, ap);
    va_end(ap);
    return out;
}
