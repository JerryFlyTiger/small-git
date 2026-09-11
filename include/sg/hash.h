#ifndef SG_HASH_H
#define SG_HASH_H

#include <stddef.h>

#define SG_SHA1_RAW_LEN 20
#define SG_SHA1_HEX_LEN 40

/* raw SHA-1 digest (20 bytes) of data into out */
void sg_sha1(const void *data, size_t len, unsigned char out[SG_SHA1_RAW_LEN]);

/* 20 raw bytes -> 40 char lowercase hex string, out must hold at least 41 bytes (incl NUL) */
void sg_sha1_to_hex(const unsigned char raw[SG_SHA1_RAW_LEN], char out[SG_SHA1_HEX_LEN + 1]);

/* 40 char hex string -> 20 raw bytes. returns 0 on success, -1 on malformed hex */
int sg_hex_to_sha1(const char *hex, unsigned char out[SG_SHA1_RAW_LEN]);

/* Parses a hex object-id PREFIX -- hex_len hex digits (upper or lower case,
   the string need not be NUL-terminated beyond hex_len), 1..40 -- into raw
   bytes: full byte pairs are filled two digits at a time, and for an odd
   hex_len the last used byte's low nibble is zeroed (only its high nibble,
   the final parsed digit, is meaningful). out must hold SG_SHA1_RAW_LEN
   bytes; only the first (hex_len + 1) / 2 of them are written, the rest are
   left untouched. Returns 0 on success, -1 if hex_len is 0 or > 40, or if
   any of the first hex_len characters is not a hex digit. */
int sg_hex_prefix_to_sha1(const char *hex_prefix, size_t hex_len, unsigned char out[SG_SHA1_RAW_LEN]);

/* Returns nonzero iff raw's leading `nibbles` hex digits equal prefix's (both
   compared as raw bytes produced by sg_hex_prefix_to_sha1 with the same
   nibbles count, or prefix may be any fully-populated id if nibbles <= 40).
   A nibbles value above 40 is treated as exactly 40 (full-id comparison). */
int sg_sha1_has_prefix(const unsigned char raw[SG_SHA1_RAW_LEN],
                       const unsigned char prefix[SG_SHA1_RAW_LEN], size_t nibbles);

#endif
