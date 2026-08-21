#include "sg/quote.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fixed fallback returned instead of the raw, unescaped path when the ring
   cannot grow to fit the escaped result. See quote.h for why this must
   never be the raw path itself. */
static const char *OOM_FALLBACK = "(unprintable path)";

typedef struct {
    char *buf;
    size_t cap;
} quote_slot;

/* Process-lifetime, file-scope buffers -- deliberately never freed. See
   quote.h: LSan treats globals as roots, so these are still-reachable, not
   leaks, under detect_leaks=1. */
static quote_slot g_slots[SG_QUOTE_SLOTS];
static int g_slot_next = 0;

/* Rotates to the next ring slot and makes sure it has room for `needed`
   bytes (including the terminating NUL). Returns NULL on allocation
   failure; the slot's previous contents/capacity are left untouched by
   realloc failure, but the caller must treat this as OOM regardless. */
static char *quote_slot_reserve(size_t needed)
{
    quote_slot *s = &g_slots[g_slot_next];

    g_slot_next = (g_slot_next + 1) % SG_QUOTE_SLOTS;

    if (s->cap < needed) {
        char *grown = realloc(s->buf, needed);

        if (grown == NULL)
            return NULL;
        s->buf = grown;
        s->cap = needed;
    }
    return s->buf;
}

static int byte_needs_escape(unsigned char c)
{
    /* Only backslash and double quote need naming here. The seven named
       control escapes escape_append also knows about (\a \b \t \n \v \f \r)
       are all below 0x20, so the range test already covers them -- listing
       them again would be a branch no test could ever distinguish, which is
       how a guard stops being a verification point. What each byte turns
       INTO is escape_append's business; this function only answers whether
       the path has to be quoted at all.

       Bytes >= 0x80 are deliberately printed as-is (sg's chosen equivalent
       of git's core.quotepath=false); see quote.h. Space (0x20) needs no
       escaping and must not trigger quoting on its own. */
    if (c == '\\' || c == '"')
        return 1;
    if (c < 0x20 || c == 0x7f)
        return 1;
    return 0;
}

static int path_needs_quote(const char *path)
{
    const unsigned char *p = (const unsigned char *)path;

    for (; *p != '\0'; p++) {
        if (byte_needs_escape(*p))
            return 1;
    }
    return 0;
}

/* Writes the escaped form of the NUL-terminated `path` (without the
   surrounding quotes) into `out`, which must be at least 4*strlen(path)
   bytes -- the worst case, where every byte expands to a 4-character octal
   escape `\ggg`. Returns the number of bytes written. */
static size_t escape_append(const unsigned char *path, char *out)
{
    size_t n = 0;

    for (; *path != '\0'; path++) {
        unsigned char c = *path;

        switch (c) {
        case '\\':
            out[n++] = '\\';
            out[n++] = '\\';
            break;
        case '"':
            out[n++] = '\\';
            out[n++] = '"';
            break;
        case '\a':
            out[n++] = '\\';
            out[n++] = 'a';
            break;
        case '\b':
            out[n++] = '\\';
            out[n++] = 'b';
            break;
        case '\t':
            out[n++] = '\\';
            out[n++] = 't';
            break;
        case '\n':
            out[n++] = '\\';
            out[n++] = 'n';
            break;
        case '\v':
            out[n++] = '\\';
            out[n++] = 'v';
            break;
        case '\f':
            out[n++] = '\\';
            out[n++] = 'f';
            break;
        case '\r':
            out[n++] = '\\';
            out[n++] = 'r';
            break;
        default:
            if (c < 0x20 || c == 0x7f) {
                /* Written out digit by digit rather than through sprintf,
                   which macOS deprecates -- and which the ordinary build does
                   not warn about while the sanitizer build does, so "make
                   reported zero warnings" would have hidden it. Three digits
                   always fit: c is below 0x80 here.

                   Zero-padded, fixed three-digit octal. %o (no width/zero
                   padding) would print 0x01 as "\1", and if the next
                   literal byte in the path happens to be an ASCII digit,
                   the result re-parses as a different escape entirely --
                   e.g. 0x01 followed by '7' must round-trip as "\0017",
                   not "\17". */
                out[n++] = '\\';
                out[n++] = (char)('0' + ((c >> 6) & 7));
                out[n++] = (char)('0' + ((c >> 3) & 7));
                out[n++] = (char)('0' + (c & 7));
            } else {
                out[n++] = (char)c;
            }
            break;
        }
    }
    return n;
}

const char *sg_quote_path_prefixed(const char *prefix, const char *path)
{
    size_t plen, qlen;
    char *buf;

    if (prefix == NULL)
        prefix = "";
    if (path == NULL)
        path = "";
    plen = strlen(prefix);
    qlen = strlen(path);

    if (!path_needs_quote(path)) {
        buf = quote_slot_reserve(plen + qlen + 1);
        if (buf == NULL)
            return OOM_FALLBACK;
        memcpy(buf, prefix, plen);
        memcpy(buf + plen, path, qlen + 1);
        return buf;
    }

    {
        size_t worst = 1 + plen + qlen * 4 + 1 + 1;
        size_t pos = 0;

        buf = quote_slot_reserve(worst);
        if (buf == NULL)
            return OOM_FALLBACK;
        buf[pos++] = '"';
        memcpy(buf + pos, prefix, plen);
        pos += plen;
        pos += escape_append((const unsigned char *)path, buf + pos);
        buf[pos++] = '"';
        buf[pos] = '\0';
    }
    return buf;
}

const char *sg_quote_path(const char *path)
{
    return sg_quote_path_prefixed("", path);
}

static int path_needs_quote_porcelain(const char *path)
{
    const unsigned char *p = (const unsigned char *)path;

    for (; *p != '\0'; p++) {
        if (byte_needs_escape(*p) || *p == ' ')
            return 1;
    }
    return 0;
}

const char *sg_quote_path_porcelain(const char *path)
{
    size_t qlen;
    char *buf;

    if (path == NULL)
        path = "";
    qlen = strlen(path);

    if (!path_needs_quote_porcelain(path)) {
        buf = quote_slot_reserve(qlen + 1);
        if (buf == NULL)
            return OOM_FALLBACK;
        memcpy(buf, path, qlen + 1);
        return buf;
    }

    {
        size_t worst = 1 + qlen * 4 + 1 + 1;
        size_t pos = 0;

        buf = quote_slot_reserve(worst);
        if (buf == NULL)
            return OOM_FALLBACK;
        buf[pos++] = '"';
        pos += escape_append((const unsigned char *)path, buf + pos);
        buf[pos++] = '"';
        buf[pos] = '\0';
    }
    return buf;
}

const char *sg_quote_path_delimited(const char *path)
{
    size_t qlen, worst, pos;
    char *buf;

    if (path == NULL)
        path = "";
    qlen = strlen(path);
    worst = 1 + qlen * 4 + 1 + 1;

    buf = quote_slot_reserve(worst);
    if (buf == NULL)
        return OOM_FALLBACK;

    pos = 0;
    buf[pos++] = '"';
    pos += escape_append((const unsigned char *)path, buf + pos);
    buf[pos++] = '"';
    buf[pos] = '\0';
    return buf;
}
