#include "sg/similarity.h"

#include <stdlib.h>
#include <string.h>

/* Port of git 2.55.0's diffcore-delta.c (plus estimate_similarity's arithmetic
   and parse_rename_score's grammar from diffcore-rename.c / diff.c). Read
   include/sg/similarity.h first: the contract there says why this may not be
   improved, only reproduced. */

/* git's HASHBASE. It is prime and sits between 2^16 and 2^17; the exact value
   is part of the answer, not a tuning knob. */
#define SG_SPANHASH_BASE 107927
/* git's FIRST_FEW_BYTES: only this much of a buffer is searched for a NUL
   before deciding it is text. */
#define SG_SPANHASH_BINARY_SCAN 8000
/* git cuts a chunk after a newline or once it has this many bytes. */
#define SG_SPANHASH_MAX_CHUNK 64

typedef struct {
    unsigned int hashval;
    unsigned int cnt;
} sg_span;

struct sg_spanhash {
    sg_span *spans; /* sorted by hashval, ascending, one entry per value */
    size_t count;
};

static int span_cmp(const void *a_, const void *b_)
{
    const sg_span *a = a_;
    const sg_span *b = b_;

    return a->hashval < b->hashval ? -1 : a->hashval > b->hashval ? 1 : 0;
}

static int buffer_is_binary(const unsigned char *buf, size_t len)
{
    size_t scan = len < SG_SPANHASH_BINARY_SCAN ? len : SG_SPANHASH_BINARY_SCAN;

    return scan != 0 && memchr(buf, 0, scan) != NULL;
}

sg_spanhash *sg_spanhash_build(const unsigned char *buf, size_t len)
{
    sg_spanhash *h;
    sg_span *raw;
    size_t cap, nr = 0;
    size_t i = 0;
    size_t write;
    /* An 8-byte shift register made of two 32-bit halves; git relies on
       these wrapping, so they must be exactly 32 bits wide and unsigned. */
    unsigned int accum1 = 0, accum2 = 0;
    unsigned int n = 0;
    int is_text;

    h = calloc(1, sizeof(*h));
    if (h == NULL)
        return NULL;
    if (len == 0)
        return h; /* an empty buffer has no chunks, and that is a valid table */

    /* A chunk ends every 64 bytes OR at every newline, so the count is not
       bounded by len/64 -- a file of nothing but newlines makes one chunk
       per byte. Start from the 64-byte guess and grow. */
    cap = len / SG_SPANHASH_MAX_CHUNK + 16;
    raw = malloc(cap * sizeof(*raw));
    if (raw == NULL) {
        free(h);
        return NULL;
    }

    is_text = !buffer_is_binary(buf, len);

    while (i < len) {
        unsigned int c = buf[i++];
        unsigned int old_1;

        /* Ignore the CR of a CRLF pair, but only in text. */
        if (is_text && c == '\r' && i < len && buf[i] == '\n')
            continue;

        old_1 = accum1;
        accum1 = (accum1 << 7) ^ (accum2 >> 25);
        accum2 = (accum2 << 7) ^ (old_1 >> 25);
        accum1 += c;
        if (++n < SG_SPANHASH_MAX_CHUNK && c != '\n')
            continue;
        if (nr == cap) {
            sg_span *grown = realloc(raw, cap * 2 * sizeof(*raw));
            if (grown == NULL) {
                free(raw);
                free(h);
                return NULL;
            }
            raw = grown;
            cap *= 2;
        }
        raw[nr].hashval = (accum1 + accum2 * 0x61) % SG_SPANHASH_BASE;
        raw[nr].cnt = n;
        nr++;
        n = 0;
        accum1 = accum2 = 0;
    }
    if (n > 0) {
        if (nr == cap) {
            sg_span *grown = realloc(raw, (cap + 1) * sizeof(*raw));
            if (grown == NULL) {
                free(raw);
                free(h);
                return NULL;
            }
            raw = grown;
            cap++;
        }
        raw[nr].hashval = (accum1 + accum2 * 0x61) % SG_SPANHASH_BASE;
        raw[nr].cnt = n;
        nr++;
    }

    /* git accumulates into an open-addressed table keyed by hash value and
       then sorts it; sorting first and summing runs of equal keys gives the
       same table, which is all diffcore_count_changes ever looks at. */
    qsort(raw, nr, sizeof(*raw), span_cmp);
    write = 0;
    for (i = 0; i < nr; i++) {
        if (write > 0 && raw[write - 1].hashval == raw[i].hashval)
            raw[write - 1].cnt += raw[i].cnt;
        else
            raw[write++] = raw[i];
    }

    h->spans = raw;
    h->count = write;
    return h;
}

void sg_spanhash_free(sg_spanhash *h)
{
    if (h == NULL)
        return;
    free(h->spans);
    free(h);
}

void sg_spanhash_count_changes(const sg_spanhash *src, const sg_spanhash *dst,
                               unsigned long *src_copied,
                               unsigned long *literal_added)
{
    unsigned long sc = 0, la = 0;
    size_t si = 0, di = 0;

    while (si < src->count) {
        unsigned int src_cnt = src->spans[si].cnt;
        unsigned int dst_cnt = 0;

        /* Everything the destination has below this hash value is material
           the source does not account for. */
        while (di < dst->count && dst->spans[di].hashval < src->spans[si].hashval)
            la += dst->spans[di++].cnt;

        if (di < dst->count && dst->spans[di].hashval == src->spans[si].hashval)
            dst_cnt = dst->spans[di++].cnt;

        if (src_cnt < dst_cnt) {
            la += dst_cnt - src_cnt;
            sc += src_cnt;
        } else {
            sc += dst_cnt;
        }
        si++;
    }
    while (di < dst->count)
        la += dst->spans[di++].cnt;

    if (src_copied != NULL)
        *src_copied = sc;
    if (literal_added != NULL)
        *literal_added = la;
}

int sg_similarity_size_rejects(size_t src_len, size_t dst_len, int min_score)
{
    size_t max_size = src_len > dst_len ? src_len : dst_len;
    size_t base_size = src_len < dst_len ? src_len : dst_len;
    size_t delta_size = max_size - base_size;

    /* git's comment explains the intent as a bound on how much the size may
       change, but the arithmetic it actually runs divides by the LARGER
       size, and it is the arithmetic that decides the answer. Written in
       double, as git writes it, because MAX_SCORE is a double there and an
       integer rewrite would round differently at the boundary.

       This also disposes of base_size == 0 before the score's division. */
    return (double)max_size * (SG_SIMILARITY_MAX - min_score) <
           (double)delta_size * SG_SIMILARITY_MAX;
}

int sg_similarity_score(const sg_spanhash *src, size_t src_len,
                        const sg_spanhash *dst, size_t dst_len)
{
    size_t max_size = src_len > dst_len ? src_len : dst_len;
    unsigned long src_copied;

    if (dst_len == 0 || max_size == 0)
        return 0;

    sg_spanhash_count_changes(src, dst, &src_copied, NULL);
    return (int)((double)src_copied * SG_SIMILARITY_MAX / (double)max_size);
}

int sg_similarity_percent(int score)
{
    return (int)(score * 100 / (double)SG_SIMILARITY_MAX);
}

int sg_similarity_parse_score(const char **cp_p)
{
    unsigned long num = 0, scale = 1;
    int dot = 0;
    const char *cp = *cp_p;

    for (;;) {
        int ch = *cp;

        if (!dot && ch == '.') {
            /* A decimal point RESTARTS the scale rather than continuing it,
               which is what makes "0.5" and "5" both mean one half. */
            scale = 1;
            dot = 1;
        } else if (ch == '%') {
            scale = dot ? scale * 100 : 100;
            cp++; /* '%' is always last */
            break;
        } else if (ch >= '0' && ch <= '9') {
            /* Digits past the fifth are consumed but not counted, so an
               absurdly long number can neither overflow nor be rejected. */
            if (scale < 100000) {
                scale *= 10;
                num = (num * 10) + (unsigned long)(ch - '0');
            }
        } else {
            break;
        }
        cp++;
    }
    *cp_p = cp;

    /* The user said num/scale; internally that is MAX_SCORE * num / scale. */
    return (int)(num >= scale ? SG_SIMILARITY_MAX
                              : SG_SIMILARITY_MAX * (double)num / (double)scale);
}
