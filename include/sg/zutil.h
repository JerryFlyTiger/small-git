#ifndef SG_ZUTIL_H
#define SG_ZUTIL_H

#include <stddef.h>

/* zlib-compress src into a newly malloc'd buffer, written to *out with length *out_len.
   caller owns *out and must free() it. returns 0 on success, -1 on failure. */
int sg_compress(const void *src, size_t src_len, unsigned char **out, size_t *out_len);

/* zlib-decompress src into a newly malloc'd buffer that grows dynamically as needed
   (the uncompressed size need not be known in advance), written to *out with length
   *out_len. caller owns *out and must free() it. returns 0 on success, -1 on failure
   (corrupt stream, trailing garbage, etc). */
int sg_decompress(const void *src, size_t src_len, unsigned char **out, size_t *out_len);

#endif
