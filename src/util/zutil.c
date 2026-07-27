#include "sg/zutil.h"

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

int sg_decompress(const void *src, size_t src_len, unsigned char **out, size_t *out_len)
{
    z_stream strm;
    unsigned char *buf;
    size_t cap = SG_INFLATE_CHUNK;
    size_t used = 0;
    int zret;

    memset(&strm, 0, sizeof(strm));
    if (inflateInit(&strm) != Z_OK)
        return -1;

    buf = malloc(cap);
    if (buf == NULL) {
        inflateEnd(&strm);
        return -1;
    }

    strm.next_in = (unsigned char *)src;
    strm.avail_in = (uInt)src_len;

    do {
        if (used == cap) {
            size_t new_cap = cap * 2;
            unsigned char *grown = realloc(buf, new_cap);

            if (grown == NULL) {
                free(buf);
                inflateEnd(&strm);
                return -1;
            }
            buf = grown;
            cap = new_cap;
        }

        strm.next_out = buf + used;
        strm.avail_out = (uInt)(cap - used);

        zret = inflate(&strm, Z_NO_FLUSH);
        if (zret != Z_OK && zret != Z_STREAM_END) {
            free(buf);
            inflateEnd(&strm);
            return -1;
        }

        used = cap - strm.avail_out;
    } while (zret != Z_STREAM_END);

    inflateEnd(&strm);

    *out = buf;
    *out_len = used;
    return 0;
}
