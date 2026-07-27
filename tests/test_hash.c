#include "sg/hash.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check_hex(const char *label, const void *data, size_t len, const char *expected_hex)
{
    unsigned char raw[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];

    sg_sha1(data, len, raw);
    sg_sha1_to_hex(raw, hex);

    if (strcmp(hex, expected_hex) != 0) {
        fprintf(stderr, "FAIL %s: got %s, expected %s\n", label, hex, expected_hex);
        failures++;
        return;
    }

    {
        unsigned char roundtrip[SG_SHA1_RAW_LEN];

        if (sg_hex_to_sha1(hex, roundtrip) != 0 || memcmp(roundtrip, raw, SG_SHA1_RAW_LEN) != 0) {
            fprintf(stderr, "FAIL %s: hex_to_sha1 roundtrip mismatch\n", label);
            failures++;
            return;
        }
    }

    printf("PASS %s: %s\n", label, hex);
}

int main(void)
{
    check_hex("empty string", "", 0, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    /* NOTE: verified independently against `openssl dgst -sha1`, `shasum -a1`, and
       Python's hashlib -- all three agree on this 40-char value */
    check_hex("\"abc\"", "abc", 3, "a9993e364706816aba3e25717850c26c9cd0d89d");

    {
        unsigned char bad[SG_SHA1_RAW_LEN];

        if (sg_hex_to_sha1("not-valid-hex-not-valid-hex-not-valid-h", bad) == 0) {
            fprintf(stderr, "FAIL: sg_hex_to_sha1 accepted malformed hex\n");
            failures++;
        } else {
            printf("PASS malformed hex rejected\n");
        }
    }

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all hash tests passed\n");
    return 0;
}
