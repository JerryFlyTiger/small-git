#include "sg/zutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void check_roundtrip(const char *label, const unsigned char *data, size_t len)
{
    unsigned char *compressed = NULL;
    size_t compressed_len = 0;
    unsigned char *decompressed = NULL;
    size_t decompressed_len = 0;

    if (sg_compress(data, len, &compressed, &compressed_len) != 0) {
        fprintf(stderr, "FAIL %s: sg_compress failed\n", label);
        failures++;
        return;
    }

    if (sg_decompress(compressed, compressed_len, &decompressed, &decompressed_len) != 0) {
        fprintf(stderr, "FAIL %s: sg_decompress failed\n", label);
        free(compressed);
        return;
    }

    if (decompressed_len != len || memcmp(decompressed, data, len) != 0) {
        fprintf(stderr, "FAIL %s: roundtrip mismatch (len %zu vs %zu)\n", label, decompressed_len,
                len);
        failures++;
    } else {
        printf("PASS %s (%zu bytes -> %zu compressed -> %zu)\n", label, len, compressed_len,
              decompressed_len);
    }

    free(compressed);
    free(decompressed);
}

int main(void)
{
    check_roundtrip("empty buffer", (const unsigned char *)"", 0);

    {
        const char *text = "hello, world\nthis has\nseveral lines\n";

        check_roundtrip("text with newlines", (const unsigned char *)text, strlen(text));
    }

    {
        /* binary data containing embedded NUL bytes -- must not be treated as a C string */
        unsigned char binary[256];
        size_t i;

        for (i = 0; i < sizeof(binary); i++)
            binary[i] = (unsigned char)i;

        check_roundtrip("binary with embedded NULs", binary, sizeof(binary));
    }

    {
        unsigned char *large = malloc(200000);
        size_t i;

        if (large == NULL) {
            fprintf(stderr, "FAIL: malloc failed for large buffer test\n");
            failures++;
        } else {
            for (i = 0; i < 200000; i++)
                large[i] = (unsigned char)(i * 7 + 3);
            check_roundtrip("large buffer", large, 200000);
            free(large);
        }
    }

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all zutil tests passed\n");
    return 0;
}
