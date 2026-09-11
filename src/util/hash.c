#include "sg/hash.h"

#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

void sg_sha1(const void *data, size_t len, unsigned char out[SG_SHA1_RAW_LEN])
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned int digest_len = 0;

    EVP_DigestInit_ex(ctx, EVP_sha1(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, out, &digest_len);
    EVP_MD_CTX_free(ctx);
}

void sg_sha1_to_hex(const unsigned char raw[SG_SHA1_RAW_LEN], char out[SG_SHA1_HEX_LEN + 1])
{
    static const char digits[] = "0123456789abcdef";
    int i;

    for (i = 0; i < SG_SHA1_RAW_LEN; i++) {
        out[i * 2] = digits[(raw[i] >> 4) & 0xf];
        out[i * 2 + 1] = digits[raw[i] & 0xf];
    }
    out[SG_SHA1_HEX_LEN] = '\0';
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int sg_hex_to_sha1(const char *hex, unsigned char out[SG_SHA1_RAW_LEN])
{
    int i;

    for (i = 0; i < SG_SHA1_RAW_LEN; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo;

        /* short-circuit before reading the next byte: a NUL at hi means the
           string ended early, and reading past it would be out of bounds */
        if (hi < 0)
            return -1;
        lo = hex_nibble(hex[i * 2 + 1]);
        if (lo < 0)
            return -1;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return 0;
}

int sg_hex_prefix_to_sha1(const char *hex_prefix, size_t hex_len, unsigned char out[SG_SHA1_RAW_LEN])
{
    size_t i;

    if (hex_len == 0 || hex_len > SG_SHA1_HEX_LEN)
        return -1;

    for (i = 0; i < hex_len; i += 2) {
        int hi = hex_nibble(hex_prefix[i]);
        int lo;

        if (hi < 0)
            return -1;
        if (i + 1 < hex_len) {
            lo = hex_nibble(hex_prefix[i + 1]);
            if (lo < 0)
                return -1;
        } else {
            lo = 0;
        }
        out[i / 2] = (unsigned char)((hi << 4) | lo);
    }
    return 0;
}

int sg_sha1_has_prefix(const unsigned char raw[SG_SHA1_RAW_LEN],
                       const unsigned char prefix[SG_SHA1_RAW_LEN], size_t nibbles)
{
    size_t full_bytes;

    if (nibbles > SG_SHA1_HEX_LEN)
        nibbles = SG_SHA1_HEX_LEN;
    full_bytes = nibbles / 2;

    if (full_bytes > 0 && memcmp(raw, prefix, full_bytes) != 0)
        return 0;
    if (nibbles % 2 != 0) {
        if ((raw[full_bytes] & 0xf0) != (prefix[full_bytes] & 0xf0))
            return 0;
    }
    return 1;
}
