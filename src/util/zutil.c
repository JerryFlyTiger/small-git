#include "sg/zutil.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

int sg_compress(const void *src, size_t src_len, unsigned char **out, size_t *out_len)
{
    uLongf bound = compressBound((uLong)src_len);
    unsigned char *buf = malloc(bound);
    uLongf dest_len = bound;

    if (buf == NULL)
        return -1;

    if (compress2(buf, &dest_len, src, (uLong)src_len, Z_DEFAULT_COMPRESSION) != Z_OK) {
        free(buf);
        return -1;
    }

    *out = buf;
    *out_len = (size_t)dest_len;
    return 0;
}

#define SG_INFLATE_CHUNK 65536

/* zlib's avail_in/avail_out are 32-bit uInt, but src_len/max_out/cap are size_t and
   can exceed UINT32_MAX (a hostile or genuinely huge object). Feeding either straight
   to zlib truncates it mod 2^32 -- see pack.c's pack_inflate() for the full writeup of
   why that's a real bug and not just a theoretical one (this is the same class of
   truncation, just on the loose-object read path instead of pack entries). So both are
   fed in chunks no larger than this, refilled as zlib consumes/produces them, until the
   real (untruncated) amounts are exhausted. */
#define SG_ZUTIL_IO_CHUNK ((size_t)1 << 30)

int sg_decompress_bounded(const void *src, size_t src_len, size_t max_out, unsigned char **out,
                          size_t *out_len)
{
    z_stream strm;
    unsigned char *buf;
    size_t cap;
    size_t used = 0;
    size_t in_remaining = src_len;
    int zret;

    memset(&strm, 0, sizeof(strm));
    if (inflateInit(&strm) != Z_OK)
        return -1;

    cap = SG_INFLATE_CHUNK < max_out ? SG_INFLATE_CHUNK : max_out;
    buf = malloc(cap > 0 ? cap : 1);
    if (buf == NULL) {
        inflateEnd(&strm);
        return -1;
    }

    strm.next_in = (unsigned char *)src;
    strm.avail_in = (uInt)(in_remaining < SG_ZUTIL_IO_CHUNK ? in_remaining : SG_ZUTIL_IO_CHUNK);
    in_remaining -= strm.avail_in;

    for (;;) {
        uLong prev_total_in;
        uLong prev_total_out;
        size_t room;
        uInt offered_out;

        /* out of buffer space: grow it, unless we're already at the hard cap, in
           which case the decompressed output has exceeded the caller's bound and
           we reject it rather than growing further (this is the zip-bomb guard). */
        if (used == cap) {
            size_t new_cap;

            if (cap >= max_out) {
                free(buf);
                inflateEnd(&strm);
                return -1;
            }
            new_cap = cap == 0 ? SG_INFLATE_CHUNK : cap * 2;
            if (new_cap < cap || new_cap > max_out) /* overflow, or past the hard cap */
                new_cap = max_out;

            {
                unsigned char *grown = realloc(buf, new_cap);

                if (grown == NULL) {
                    free(buf);
                    inflateEnd(&strm);
                    return -1;
                }
                buf = grown;
                cap = new_cap;
            }
        }

        room = cap - used;
        offered_out = (uInt)(room < SG_ZUTIL_IO_CHUNK ? room : SG_ZUTIL_IO_CHUNK);
        strm.next_out = buf + used;
        strm.avail_out = offered_out;

        prev_total_in = strm.total_in;
        prev_total_out = strm.total_out;

        zret = inflate(&strm, Z_NO_FLUSH);
        used += offered_out - strm.avail_out;

        if (zret == Z_STREAM_END)
            break;
        if (zret != Z_OK && zret != Z_BUF_ERROR) {
            free(buf);
            inflateEnd(&strm);
            return -1;
        }

        /* Defense-in-depth against this loop spinning forever if inflate() were to
           report Z_OK/Z_BUF_ERROR without making progress despite having both
           avail_in>0 and avail_out>0 available -- see pack_inflate() in pack.c for
           the full rationale, this mirrors that check. */
        if (strm.total_in == prev_total_in && strm.total_out == prev_total_out) {
            free(buf);
            inflateEnd(&strm);
            return -1;
        }

        /* out of input with none left to refill, but the stream isn't done --
           truncated/malformed compressed data. */
        if (strm.avail_in == 0 && in_remaining == 0) {
            free(buf);
            inflateEnd(&strm);
            return -1;
        }

        if (strm.avail_in == 0) {
            strm.avail_in = (uInt)(in_remaining < SG_ZUTIL_IO_CHUNK ? in_remaining
                                                                     : SG_ZUTIL_IO_CHUNK);
            in_remaining -= strm.avail_in;
        }
    }

    inflateEnd(&strm);

    *out = buf;
    *out_len = used;
    return 0;
}

int sg_decompress(const void *src, size_t src_len, unsigned char **out, size_t *out_len)
{
    return sg_decompress_bounded(src, src_len, SIZE_MAX, out, out_len);
}

int sg_decompress_prefix(const void *src, size_t src_len, unsigned char *out_buf, size_t out_cap,
                         size_t *out_len)
{
    z_stream strm;
    int zret;

    memset(&strm, 0, sizeof(strm));
    if (inflateInit(&strm) != Z_OK)
        return -1;

    strm.next_in = (unsigned char *)src;
    strm.avail_in = (uInt)(src_len < SG_ZUTIL_IO_CHUNK ? src_len : SG_ZUTIL_IO_CHUNK);
    strm.next_out = out_buf;
    strm.avail_out = (uInt)(out_cap < SG_ZUTIL_IO_CHUNK ? out_cap : SG_ZUTIL_IO_CHUNK);

    zret = inflate(&strm, Z_NO_FLUSH);
    if (zret != Z_OK && zret != Z_STREAM_END && zret != Z_BUF_ERROR) {
        inflateEnd(&strm);
        return -1;
    }

    *out_len = out_cap - strm.avail_out;
    inflateEnd(&strm);
    return 0;
}
