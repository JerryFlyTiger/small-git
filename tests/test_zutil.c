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

static void test_decompress_bounded_within_cap(void)
{
    unsigned char *compressed = NULL;
    size_t compressed_len = 0;
    unsigned char *decompressed = NULL;
    size_t decompressed_len = 0;
    const char *text = "some short text that easily fits under any real cap";
    size_t len = strlen(text);

    if (sg_compress(text, len, &compressed, &compressed_len) != 0) {
        fprintf(stderr, "FAIL bounded-within-cap: sg_compress failed\n");
        failures++;
        return;
    }

    /* max_out well above the true decompressed size must succeed, exactly
       like unbounded sg_decompress. */
    if (sg_decompress_bounded(compressed, compressed_len, len + 1000, &decompressed,
                              &decompressed_len) != 0) {
        fprintf(stderr, "FAIL bounded-within-cap: sg_decompress_bounded failed\n");
        failures++;
    } else if (decompressed_len != len || memcmp(decompressed, text, len) != 0) {
        fprintf(stderr, "FAIL bounded-within-cap: content mismatch\n");
        failures++;
    } else {
        printf("PASS bounded decompress succeeds when max_out comfortably exceeds actual size\n");
    }
    free(decompressed);
    decompressed = NULL;

    /* max_out exactly equal to the true decompressed size must also succeed
       (the cap is inclusive, not exclusive). */
    if (sg_decompress_bounded(compressed, compressed_len, len, &decompressed,
                              &decompressed_len) != 0) {
        fprintf(stderr, "FAIL bounded-within-cap: sg_decompress_bounded failed at exact cap\n");
        failures++;
    } else if (decompressed_len != len || memcmp(decompressed, text, len) != 0) {
        fprintf(stderr, "FAIL bounded-within-cap: content mismatch at exact cap\n");
        failures++;
    } else {
        printf("PASS bounded decompress succeeds when max_out exactly equals actual size\n");
    }

    free(compressed);
    free(decompressed);
}

/* This is the zip-bomb regression case: a small, highly compressible source
   whose true decompressed size (200000 bytes -- deliberately nowhere near
   "several GB", per the no-large-allocations-in-tests constraint) comfortably
   exceeds a caller-supplied cap that's far smaller (100 bytes). The old
   unbounded sg_decompress() would happily grow its buffer to 200000 bytes and
   return success; sg_decompress_bounded() must instead refuse to grow past
   max_out and fail cleanly. Because 200000 bytes is a perfectly reasonable
   allocation on its own, a passing result here only comes from max_out
   actually being enforced, not from an incidental allocation failure --
   which is exactly what makes this test mutation-sensitive (verified: removing
   the `cap >= max_out` rejection in sg_decompress_bounded turns this CHECK
   red with out_len==200000).

   At this layer (sg_decompress_bounded called directly) that's the whole
   story: there's no downstream caller re-checking the output length, so a
   wrong return value is all it takes to catch a regression. That stops being
   true one layer up, at sg_loose_read() -- see tests/test_loose.c's
   test_bound_caps_rss() for why the loose-object read path needs an RSS
   probe instead of a return-value assertion to catch the same class of
   regression. */
static void test_decompress_bounded_over_cap(void)
{
    unsigned char *large = malloc(200000);
    unsigned char *compressed = NULL;
    size_t compressed_len = 0;
    unsigned char *decompressed = NULL;
    size_t decompressed_len = 0;

    if (large == NULL) {
        fprintf(stderr, "FAIL bounded-over-cap: malloc failed\n");
        failures++;
        return;
    }
    memset(large, 0, 200000);

    if (sg_compress(large, 200000, &compressed, &compressed_len) != 0) {
        fprintf(stderr, "FAIL bounded-over-cap: sg_compress failed\n");
        failures++;
        free(large);
        return;
    }
    free(large);

    if (sg_decompress_bounded(compressed, compressed_len, 100, &decompressed,
                              &decompressed_len) == 0) {
        fprintf(stderr,
               "FAIL bounded-over-cap: sg_decompress_bounded should have rejected output "
               "exceeding max_out, but it succeeded (out_len=%zu)\n",
               decompressed_len);
        failures++;
        free(decompressed);
    } else {
        printf("PASS bounded decompress rejects output exceeding max_out (200000-byte payload, "
              "100-byte cap)\n");
    }

    free(compressed);
}

static void test_decompress_prefix(void)
{
    unsigned char *compressed = NULL;
    size_t compressed_len = 0;
    unsigned char probe[8];
    size_t probe_len = 0;
    const char *text = "hello, world -- this is longer than the probe window";
    size_t len = strlen(text);

    if (sg_compress(text, len, &compressed, &compressed_len) != 0) {
        fprintf(stderr, "FAIL prefix: sg_compress failed\n");
        failures++;
        return;
    }

    /* probe buffer (8 bytes) is smaller than the true decompressed size:
       must stop at the buffer boundary without requiring the stream to be
       fully consumed, and return exactly what fit. */
    if (sg_decompress_prefix(compressed, compressed_len, probe, sizeof(probe), &probe_len) != 0) {
        fprintf(stderr, "FAIL prefix: sg_decompress_prefix failed (small cap)\n");
        failures++;
    } else if (probe_len != sizeof(probe) || memcmp(probe, text, sizeof(probe)) != 0) {
        fprintf(stderr, "FAIL prefix: wrong prefix bytes (small cap), probe_len=%zu\n", probe_len);
        failures++;
    } else {
        printf("PASS prefix decompress stops cleanly at a cap smaller than the stream\n");
    }

    /* probe buffer larger than the true decompressed size: the stream ends
       first, so out_len must be the full (short) length, not the cap. */
    {
        unsigned char big_probe[256];
        size_t big_probe_len = 0;

        if (sg_decompress_prefix(compressed, compressed_len, big_probe, sizeof(big_probe),
                                 &big_probe_len) != 0) {
            fprintf(stderr, "FAIL prefix: sg_decompress_prefix failed (large cap)\n");
            failures++;
        } else if (big_probe_len != len || memcmp(big_probe, text, len) != 0) {
            fprintf(stderr, "FAIL prefix: wrong prefix bytes (large cap), got len %zu want %zu\n",
                   big_probe_len, len);
            failures++;
        } else {
            printf("PASS prefix decompress returns the full short stream when cap exceeds it\n");
        }
    }

    free(compressed);
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

    test_decompress_bounded_within_cap();
    test_decompress_bounded_over_cap();
    test_decompress_prefix();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all zutil tests passed\n");
    return 0;
}
