#ifndef SG_ZUTIL_H
#define SG_ZUTIL_H

#include <stddef.h>

/* zlib-compress src into a newly malloc'd buffer, written to *out with length *out_len.
   caller owns *out and must free() it. returns 0 on success, -1 on failure. */
int sg_compress(const void *src, size_t src_len, unsigned char **out, size_t *out_len);

/* zlib-decompress src into a newly malloc'd buffer that grows dynamically as needed
   up to max_out bytes (the uncompressed size need not be known in advance), written to
   *out with length *out_len. caller owns *out and must free() it. returns 0 on success,
   -1 on failure (corrupt stream, trailing garbage, or the decompressed output would
   exceed max_out -- callers that know an upper bound on the expected size, e.g. from a
   trusted or format-declared length, should pass it here rather than SIZE_MAX to avoid
   unbounded memory growth on hostile input). */
int sg_decompress_bounded(const void *src, size_t src_len, size_t max_out, unsigned char **out,
                          size_t *out_len);

/* zlib-decompress src into a newly malloc'd buffer that grows dynamically as needed
   (the uncompressed size need not be known in advance), written to *out with length
   *out_len. caller owns *out and must free() it. returns 0 on success, -1 on failure
   (corrupt stream, trailing garbage, etc).

   equivalent to sg_decompress_bounded() with max_out == SIZE_MAX -- i.e. no cap on the
   decompressed size. prefer sg_decompress_bounded() with a real bound wherever the
   caller can derive one; this unbounded form exists for callers that genuinely can't. */
int sg_decompress(const void *src, size_t src_len, unsigned char **out, size_t *out_len);

/* Decompresses at most out_cap bytes of src into the caller-owned buffer out_buf,
   stopping either because out_buf is full or the stream reached its natural end,
   whichever comes first -- unlike sg_decompress[_bounded], the stream is NOT required
   to be fully consumed. *out_len is set to however many bytes were actually produced
   (which may be less than out_cap). Returns 0 on success, -1 on a genuine zlib error
   (corrupt stream). Intended for peeking at the first few decompressed bytes of a
   stream without committing to decompressing all of it, e.g. reading a loose object's
   "{type} {size}\0" header before its declared content size is known and a real bound
   for sg_decompress_bounded() can be computed. */
int sg_decompress_prefix(const void *src, size_t src_len, unsigned char *out_buf, size_t out_cap,
                         size_t *out_len);

#endif
