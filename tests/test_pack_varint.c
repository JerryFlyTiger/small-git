/* Exercises the three distinct variable-length integer encodings used by the
   pack format. sg_pack_decode_obj_header/sg_pack_encode_obj_header/
   sg_pack_decode_ofs_delta_offset/sg_pack_decode_delta_size are defined
   (non-static, but not part of the public sg/pack.h surface) in
   src/storage/pack.c; declared here via extern to keep them unit-testable
   without exposing them as public API. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

extern size_t sg_pack_decode_obj_header(const unsigned char *p, size_t avail, int *type_out,
                                        uint64_t *size_out);
extern size_t sg_pack_encode_obj_header(int type, uint64_t size, unsigned char *out);
extern size_t sg_pack_decode_ofs_delta_offset(const unsigned char *p, size_t avail,
                                              uint64_t *offset_out);
extern size_t sg_pack_decode_delta_size(const unsigned char *p, size_t avail, uint64_t *size_out);

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

/* ---- object header (type + size) ---- */

static void test_obj_header_single_byte(void)
{
    /* type=3 (blob), size=10, fits entirely in one byte, no continuation */
    const unsigned char bytes[] = {0x3A};
    int type;
    uint64_t size;
    size_t n = sg_pack_decode_obj_header(bytes, sizeof(bytes), &type, &size);

    CHECK(n == 1, "expected 1 byte consumed, got %zu", n);
    CHECK(type == 3, "expected type 3, got %d", type);
    CHECK(size == 10, "expected size 10, got %llu", (unsigned long long)size);
}

static void test_obj_header_multi_byte(void)
{
    /* type=1 (commit), size=300, needs one continuation byte */
    const unsigned char bytes[] = {0x9C, 0x12};
    int type;
    uint64_t size;
    size_t n = sg_pack_decode_obj_header(bytes, sizeof(bytes), &type, &size);

    CHECK(n == 2, "expected 2 bytes consumed, got %zu", n);
    CHECK(type == 1, "expected type 1, got %d", type);
    CHECK(size == 300, "expected size 300, got %llu", (unsigned long long)size);
}

static void test_obj_header_real_git_ofs_delta(void)
{
    /* captured from a real `git repack -ad`-produced pack: an OFS_DELTA
       entry with delta-stream size 34 */
    const unsigned char bytes[] = {0xe2, 0x02};
    int type;
    uint64_t size;
    size_t n = sg_pack_decode_obj_header(bytes, sizeof(bytes), &type, &size);

    CHECK(n == 2, "expected 2 bytes consumed, got %zu", n);
    CHECK(type == 6, "expected type 6 (OFS_DELTA), got %d", type);
    CHECK(size == 34, "expected size 34, got %llu", (unsigned long long)size);
}

static void test_obj_header_truncated(void)
{
    const unsigned char bytes[] = {0x9C}; /* continuation bit set, no next byte */
    int type;
    uint64_t size;
    size_t n = sg_pack_decode_obj_header(bytes, sizeof(bytes), &type, &size);

    CHECK(n == 0, "expected 0 (error) for truncated header, got %zu", n);
}

static void test_obj_header_encode_roundtrip(void)
{
    unsigned char buf[16];
    size_t n = sg_pack_encode_obj_header(1, 300, buf);
    int type;
    uint64_t size;
    size_t dn;

    CHECK(n == 2, "expected 2 bytes encoded, got %zu", n);
    CHECK(buf[0] == 0x9C && buf[1] == 0x12, "encoded bytes mismatch: %02x %02x", buf[0], buf[1]);

    dn = sg_pack_decode_obj_header(buf, n, &type, &size);
    CHECK(dn == n, "decode consumed %zu, expected %zu", dn, n);
    CHECK(type == 1 && size == 300, "roundtrip mismatch: type=%d size=%llu", type,
         (unsigned long long)size);
}

static void test_obj_header_encode_single_byte(void)
{
    unsigned char buf[16];
    size_t n = sg_pack_encode_obj_header(3, 10, buf);

    CHECK(n == 1, "expected 1 byte, got %zu", n);
    CHECK(buf[0] == 0x3A, "expected 0x3A, got 0x%02x", buf[0]);
}

/* ---- OFS_DELTA backwards-offset varint (distinct +1 continuation rule) ---- */

static void test_ofs_delta_offset_single_byte(void)
{
    const unsigned char bytes[] = {0x2A}; /* 42, no continuation */
    uint64_t offset;
    size_t n = sg_pack_decode_ofs_delta_offset(bytes, sizeof(bytes), &offset);

    CHECK(n == 1, "expected 1 byte, got %zu", n);
    CHECK(offset == 42, "expected 42, got %llu", (unsigned long long)offset);
}

static void test_ofs_delta_offset_real_git_two_byte(void)
{
    /* captured from the same real OFS_DELTA entry as above: relative
       offset 5583 back to the base object's entry */
    const unsigned char bytes[] = {0xaa, 0x4f};
    uint64_t offset;
    size_t n = sg_pack_decode_ofs_delta_offset(bytes, sizeof(bytes), &offset);

    CHECK(n == 2, "expected 2 bytes, got %zu", n);
    CHECK(offset == 5583, "expected 5583, got %llu", (unsigned long long)offset);
}

static void test_ofs_delta_offset_three_byte(void)
{
    /* hand-verified against git's own encode_in_pack_object_header/
       decode logic: encodes to 1000000 with the +1-per-continuation rule */
    const unsigned char bytes[] = {0xBC, 0x83, 0x40};
    uint64_t offset;
    size_t n = sg_pack_decode_ofs_delta_offset(bytes, sizeof(bytes), &offset);

    CHECK(n == 3, "expected 3 bytes, got %zu", n);
    CHECK(offset == 1000000, "expected 1000000, got %llu", (unsigned long long)offset);
}

/* ---- delta instruction stream's plain 7-bit-per-byte size varint ---- */

static void test_delta_size_single_byte(void)
{
    const unsigned char bytes[] = {0x7F};
    uint64_t size;
    size_t n = sg_pack_decode_delta_size(bytes, sizeof(bytes), &size);

    CHECK(n == 1, "expected 1 byte, got %zu", n);
    CHECK(size == 127, "expected 127, got %llu", (unsigned long long)size);
}

static void test_delta_size_zero(void)
{
    const unsigned char bytes[] = {0x00};
    uint64_t size;
    size_t n = sg_pack_decode_delta_size(bytes, sizeof(bytes), &size);

    CHECK(n == 1, "expected 1 byte, got %zu", n);
    CHECK(size == 0, "expected 0, got %llu", (unsigned long long)size);
}

static void test_delta_size_two_byte(void)
{
    const unsigned char bytes[] = {0x80, 0x01}; /* 128, no +1 trick here */
    uint64_t size;
    size_t n = sg_pack_decode_delta_size(bytes, sizeof(bytes), &size);

    CHECK(n == 2, "expected 2 bytes, got %zu", n);
    CHECK(size == 128, "expected 128, got %llu", (unsigned long long)size);
}

static void test_delta_size_real_git(void)
{
    /* captured base_size (142906) from the delta stream in the same real
       OFS_DELTA entry used above */
    const unsigned char bytes[] = {0xba, 0xdc, 0x08};
    uint64_t size;
    size_t n = sg_pack_decode_delta_size(bytes, sizeof(bytes), &size);

    CHECK(n == 3, "expected 3 bytes, got %zu", n);
    CHECK(size == 142906, "expected 142906, got %llu", (unsigned long long)size);
}

int main(void)
{
    test_obj_header_single_byte();
    test_obj_header_multi_byte();
    test_obj_header_real_git_ofs_delta();
    test_obj_header_truncated();
    test_obj_header_encode_roundtrip();
    test_obj_header_encode_single_byte();

    test_ofs_delta_offset_single_byte();
    test_ofs_delta_offset_real_git_two_byte();
    test_ofs_delta_offset_three_byte();

    test_delta_size_single_byte();
    test_delta_size_zero();
    test_delta_size_two_byte();
    test_delta_size_real_git();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all pack varint tests passed\n");
    return 0;
}
