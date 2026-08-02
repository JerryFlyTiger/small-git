#include "sg/pktline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, ...)                                                                        \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);                                 \
            fprintf(stderr, __VA_ARGS__);                                                        \
            fprintf(stderr, "\n");                                                               \
            failures++;                                                                          \
        }                                                                                         \
    } while (0)

static void test_append_and_read_roundtrip(void)
{
    unsigned char *buf = NULL;
    size_t len = 0, cap = 0;
    size_t pos = 0;
    sg_pkt_type type;
    const unsigned char *payload;
    size_t payload_len;

    CHECK(sg_pkt_append_str(&buf, &len, &cap, "want abc\n") == 0, "append failed");
    /* "want abc\n" is 9 bytes -> total packet 13 = 0x000d, per the spec's example */
    CHECK(len == 13, "expected 13-byte packet, got %zu", len);
    CHECK(memcmp(buf, "000dwant abc\n", 13) == 0, "encoded bytes mismatch");

    CHECK(sg_pkt_read(buf, len, &pos, &type, &payload, &payload_len) == 0, "read failed");
    CHECK(type == SG_PKT_DATA, "expected DATA type");
    CHECK(payload_len == 9, "expected payload_len 9, got %zu", payload_len);
    CHECK(memcmp(payload, "want abc\n", 9) == 0, "payload mismatch");
    CHECK(pos == len, "pos should have advanced to end, got %zu of %zu", pos, len);

    free(buf);
}

static void test_flush_pkt(void)
{
    unsigned char *buf = NULL;
    size_t len = 0, cap = 0;
    size_t pos = 0;
    sg_pkt_type type;
    const unsigned char *payload;
    size_t payload_len;

    CHECK(sg_pkt_append_flush(&buf, &len, &cap) == 0, "append_flush failed");
    CHECK(len == 4, "expected 4-byte flush packet, got %zu", len);
    CHECK(memcmp(buf, "0000", 4) == 0, "flush bytes mismatch");

    CHECK(sg_pkt_read(buf, len, &pos, &type, &payload, &payload_len) == 0, "read failed");
    CHECK(type == SG_PKT_FLUSH, "expected FLUSH type");
    CHECK(payload == NULL, "flush payload should be NULL");
    CHECK(payload_len == 0, "flush payload_len should be 0");
    CHECK(pos == 4, "pos should be 4, got %zu", pos);

    free(buf);
}

static void test_empty_payload_pkt(void)
{
    /* "0004" -- length 4 means header only, zero-length payload */
    const unsigned char bytes[] = "0004";
    size_t pos = 0;
    sg_pkt_type type;
    const unsigned char *payload;
    size_t payload_len;

    CHECK(sg_pkt_read(bytes, sizeof(bytes) - 1, &pos, &type, &payload, &payload_len) == 0,
         "read of 0004 should succeed");
    CHECK(type == SG_PKT_DATA, "0004 should be a DATA packet");
    CHECK(payload_len == 0, "expected empty payload, got %zu", payload_len);
    CHECK(pos == 4, "pos should be 4, got %zu", pos);
}

static void test_delim_pkt(void)
{
    const unsigned char bytes[] = "0001";
    size_t pos = 0;
    sg_pkt_type type;
    const unsigned char *payload;
    size_t payload_len;

    CHECK(sg_pkt_read(bytes, sizeof(bytes) - 1, &pos, &type, &payload, &payload_len) == 0,
         "read of 0001 should succeed");
    CHECK(type == SG_PKT_DELIM, "0001 should be a DELIM packet");
    CHECK(pos == 4, "pos should be 4, got %zu", pos);
}

static void test_invalid_lengths_rejected(void)
{
    static const char *bad_lengths[] = {"0002", "0003"};
    size_t i;

    for (i = 0; i < sizeof(bad_lengths) / sizeof(bad_lengths[0]); i++) {
        size_t pos = 0;
        sg_pkt_type type;
        const unsigned char *payload;
        size_t payload_len;
        int rc = sg_pkt_read((const unsigned char *)bad_lengths[i], 4, &pos, &type, &payload,
                             &payload_len);

        CHECK(rc == -1, "length '%s' should be rejected, got rc=%d", bad_lengths[i], rc);
    }

    /* length field just above the format's cap of fff0 */
    {
        const unsigned char bytes[4] = {'f', 'f', 'f', '1'};
        /* need a buffer that's "big enough" so only the length value itself
           is what's rejected, not the truncation check */
        unsigned char big[0x10000];
        size_t pos = 0;
        sg_pkt_type type;
        const unsigned char *payload;
        size_t payload_len;
        int rc;

        memset(big, 'x', sizeof(big));
        memcpy(big, bytes, 4);
        rc = sg_pkt_read(big, sizeof(big), &pos, &type, &payload, &payload_len);
        CHECK(rc == -1, "length fff1 (> fff0) should be rejected, got rc=%d", rc);
    }
}

static void test_non_hex_length_rejected(void)
{
    const unsigned char bytes[] = "xxxxpayload data here";
    size_t pos = 0;
    sg_pkt_type type;
    const unsigned char *payload;
    size_t payload_len;
    int rc = sg_pkt_read(bytes, sizeof(bytes) - 1, &pos, &type, &payload, &payload_len);

    CHECK(rc == -1, "non-hex length digits should be rejected, got rc=%d", rc);
}

static void test_declared_length_exceeds_buffer_rejected(void)
{
    /* claims a packet of length 0x0100 (256 bytes) but only 8 bytes actually follow --
       must not read out of bounds (run under ASan to be sure) */
    const unsigned char bytes[] = "0100short";
    size_t pos = 0;
    sg_pkt_type type;
    const unsigned char *payload;
    size_t payload_len;
    int rc = sg_pkt_read(bytes, sizeof(bytes) - 1, &pos, &type, &payload, &payload_len);

    CHECK(rc == -1, "declared length exceeding the buffer should be rejected, got rc=%d", rc);
}

static void test_read_needs_at_least_four_bytes(void)
{
    const unsigned char bytes[] = "00";
    size_t pos = 0;
    sg_pkt_type type;
    const unsigned char *payload;
    size_t payload_len;
    int rc = sg_pkt_read(bytes, sizeof(bytes) - 1, &pos, &type, &payload, &payload_len);

    CHECK(rc == -1, "a 2-byte buffer (less than the 4-byte length prefix) should be rejected");
}

static void test_multiple_packets_in_sequence(void)
{
    unsigned char *buf = NULL;
    size_t len = 0, cap = 0;
    size_t pos = 0;
    sg_pkt_type type;
    const unsigned char *payload;
    size_t payload_len;

    CHECK(sg_pkt_append_str(&buf, &len, &cap, "have deadbeef\n") == 0, "append 1 failed");
    CHECK(sg_pkt_append_str(&buf, &len, &cap, "done\n") == 0, "append 2 failed");
    CHECK(sg_pkt_append_flush(&buf, &len, &cap) == 0, "append flush failed");

    CHECK(sg_pkt_read(buf, len, &pos, &type, &payload, &payload_len) == 0, "read 1 failed");
    CHECK(type == SG_PKT_DATA && payload_len == 14, "packet 1 mismatch");

    CHECK(sg_pkt_read(buf, len, &pos, &type, &payload, &payload_len) == 0, "read 2 failed");
    CHECK(type == SG_PKT_DATA && payload_len == 5, "packet 2 mismatch");
    CHECK(memcmp(payload, "done\n", 5) == 0, "packet 2 payload mismatch");

    CHECK(sg_pkt_read(buf, len, &pos, &type, &payload, &payload_len) == 0, "read 3 failed");
    CHECK(type == SG_PKT_FLUSH, "packet 3 should be flush");
    CHECK(pos == len, "should have consumed the whole buffer");

    free(buf);
}

static void test_oversized_payload_rejected_on_append(void)
{
    unsigned char *buf = NULL;
    size_t len = 0, cap = 0;
    /* one byte over the 65516 max payload */
    static unsigned char big[65517];
    int rc;

    memset(big, 'a', sizeof(big));
    rc = sg_pkt_append(&buf, &len, &cap, big, sizeof(big));
    CHECK(rc == -1, "oversized payload should be rejected, got rc=%d", rc);
    CHECK(buf == NULL, "buf should remain untouched on rejection");

    free(buf);
}

int main(void)
{
    test_append_and_read_roundtrip();
    test_flush_pkt();
    test_empty_payload_pkt();
    test_delim_pkt();
    test_invalid_lengths_rejected();
    test_non_hex_length_rejected();
    test_declared_length_exceeds_buffer_rejected();
    test_read_needs_at_least_four_bytes();
    test_multiple_packets_in_sequence();
    test_oversized_payload_rejected_on_append();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all pktline tests passed\n");
    return 0;
}
