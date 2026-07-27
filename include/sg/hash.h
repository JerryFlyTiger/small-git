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

#endif
