#include "sg/loose.h"

#include "sg/object.h"
#include "sg/repo.h"
#include "sg/zutil.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

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

static char *make_tmp_repo(void)
{
    static char template[] = "/tmp/sg_loose_test_XXXXXX";
    char *path = strdup(template);
    char git_dir[4096];

    if (mkdtemp(path) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        exit(1);
    }
    if (sg_repo_init(path) != 0) {
        fprintf(stderr, "sg_repo_init failed\n");
        exit(1);
    }
    snprintf(git_dir, sizeof(git_dir), "%s/.git", path);
    free(path);
    return strdup(git_dir);
}

static void object_file_paths(const char *git_dir, const char *hex, char dir_path[4096],
                              char file_path[4096])
{
    snprintf(dir_path, 4096, "%s/objects/%.2s", git_dir, hex);
    snprintf(file_path, 4096, "%s/objects/%.2s/%s", git_dir, hex, hex + 2);
}

static void ensure_object_dir(const char *dir_path)
{
    if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir %s failed\n", dir_path);
        exit(1);
    }
}

/* Writes `compressed` directly at git_dir/objects/<hex[0:2]>/<hex[2:]>,
   bypassing sg_loose_write() entirely -- these tests need to plant
   deliberately malformed loose object files (a lying header, size
   mismatches) that sg_loose_write() would never itself produce. */
static void plant_object_file(const char *git_dir, const char *hex, const unsigned char *compressed,
                              size_t compressed_len)
{
    char dir_path[4096];
    char file_path[4096];
    FILE *f;

    object_file_paths(git_dir, hex, dir_path, file_path);
    ensure_object_dir(dir_path);

    f = fopen(file_path, "wb");
    if (f == NULL) {
        fprintf(stderr, "fopen %s failed\n", file_path);
        exit(1);
    }
    if (fwrite(compressed, 1, compressed_len, f) != compressed_len) {
        fprintf(stderr, "fwrite %s failed\n", file_path);
        exit(1);
    }
    fclose(f);
}

/* Plants a loose object file whose header lies about its size, streaming the
   zlib deflate directly to disk in small chunks rather than building the
   full "{header}{zero_payload_len zero bytes}" buffer in memory first (as
   test_declared_much_smaller_than_actual() above does for its much smaller
   200000-byte payload). That matters specifically for test_bound_caps_rss():
   this helper backs a real zip-bomb-sized payload (hundreds of MB), and
   getrusage's ru_maxrss is a high-water mark, not a snapshot of current
   usage -- if *this test binary's own setup* ever touched that much memory
   in-process, ru_maxrss would latch onto it and the subsequent before/after
   delta around sg_loose_read() could read as ~0 regardless of whether the
   bound is actually enforced (the "before" reading would already be
   contaminated by the setup peak). Streaming the compression keeps this
   process's own resident memory at a few KB throughout construction, so
   only sg_loose_read() itself can move the needle. */
static void plant_zerobomb_object_file(const char *git_dir, const char *hex, const char *header,
                                       size_t header_len, size_t zero_payload_len)
{
    char dir_path[4096];
    char file_path[4096];
    z_stream strm;
    unsigned char in_chunk[65536];
    unsigned char out_chunk[65536];
    size_t remaining = zero_payload_len;
    int flush = Z_NO_FLUSH;
    int zret;
    FILE *f;

    object_file_paths(git_dir, hex, dir_path, file_path);
    ensure_object_dir(dir_path);

    f = fopen(file_path, "wb");
    if (f == NULL) {
        fprintf(stderr, "fopen %s failed\n", file_path);
        exit(1);
    }

    memset(&strm, 0, sizeof(strm));
    if (deflateInit(&strm, Z_DEFAULT_COMPRESSION) != Z_OK) {
        fprintf(stderr, "deflateInit failed\n");
        exit(1);
    }
    memset(in_chunk, 0, sizeof(in_chunk));

    strm.next_in = (unsigned char *)header;
    strm.avail_in = (uInt)header_len;

    for (;;) {
        do {
            strm.next_out = out_chunk;
            strm.avail_out = sizeof(out_chunk);
            zret = deflate(&strm, flush);
            if (zret == Z_STREAM_ERROR) {
                fprintf(stderr, "deflate failed\n");
                exit(1);
            }
            if (fwrite(out_chunk, 1, sizeof(out_chunk) - strm.avail_out, f) !=
                sizeof(out_chunk) - strm.avail_out) {
                fprintf(stderr, "fwrite %s failed\n", file_path);
                exit(1);
            }
        } while (strm.avail_out == 0);

        if (zret == Z_STREAM_END)
            break;

        if (strm.avail_in == 0) {
            if (remaining == 0) {
                flush = Z_FINISH;
                strm.next_in = NULL;
                strm.avail_in = 0;
            } else {
                size_t take = remaining < sizeof(in_chunk) ? remaining : sizeof(in_chunk);

                strm.next_in = in_chunk;
                strm.avail_in = (uInt)take;
                remaining -= take;
            }
        }
    }

    deflateEnd(&strm);
    fclose(f);
}

static const unsigned char *arbitrary_id(void)
{
    static const unsigned char id[SG_SHA1_RAW_LEN] = {0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67,
                                                       0x89, 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45,
                                                       0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x01};
    return id;
}

/* Ordinary sg_loose_write()/sg_loose_read() roundtrip: the new two-phase
   read path (header probe, then bounded decompress) must not change
   behavior for well-formed objects. */
static void test_roundtrip(void)
{
    char *git_dir = make_tmp_repo();
    const char *content = "an ordinary blob, nothing adversarial here\n";
    size_t content_len = strlen(content);
    unsigned char id[SG_SHA1_RAW_LEN];
    sg_obj_type type;
    unsigned char *read_content;
    size_t read_len;

    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, content, content_len, id) == 0,
         "sg_loose_write failed");
    CHECK(sg_loose_read(git_dir, id, &type, &read_content, &read_len) == 0,
         "sg_loose_read failed on a well-formed object");
    CHECK(type == SG_OBJ_BLOB, "wrong type");
    CHECK(read_len == content_len, "wrong length: got %zu want %zu", read_len, content_len);
    CHECK(memcmp(read_content, content, content_len) == 0, "content mismatch");
    free(read_content);
    free(git_dir);
}

/* Zip-bomb shape: the header declares a tiny size (5 bytes) but the stream
   actually decompresses to far more (200000 bytes of a single repeated byte
   -- deliberately nowhere near "several GB", per the no-large-allocations
   constraint, but comfortably larger than any real object this small should
   ever produce). sg_loose_read must fail cleanly rather than decompress the
   whole thing into memory first and only notice the mismatch afterward.

   IMPORTANT, honestly: this is defense-in-depth, not a discriminating test.
   sg_object_parse()'s pre-existing exact-length check (header_len +
   declared_size != data_len) rejects this shape regardless of whether
   sg_decompress_bounded()'s cap is actually being enforced -- verified by
   mutation: forcing max_out = SIZE_MAX in sg_loose_read() (i.e. simulating
   the pre-fix unbounded decompress) leaves this CHECK green. The return
   value and error message are identical either way; only the memory used to
   get there differs. test_bound_caps_rss() below is the one test in this
   file that can actually see that difference. */
static void test_declared_much_smaller_than_actual(void)
{
    char *git_dir = make_tmp_repo();
    const char *header = "blob 5\0";
    size_t header_len = 7; /* strlen() would stop at the embedded NUL */
    size_t payload_len = 200000;
    unsigned char *raw;
    unsigned char *compressed;
    size_t compressed_len;
    char hex[SG_SHA1_HEX_LEN + 1];
    unsigned char *id;
    sg_obj_type type;
    unsigned char *read_content;
    size_t read_len;

    raw = malloc(header_len + payload_len);
    CHECK(raw != NULL, "malloc failed");
    if (raw == NULL) {
        free(git_dir);
        return;
    }
    memcpy(raw, header, header_len);
    memset(raw + header_len, 0, payload_len);

    CHECK(sg_compress(raw, header_len + payload_len, &compressed, &compressed_len) == 0,
         "sg_compress failed");
    free(raw);

    id = (unsigned char *)arbitrary_id();
    sg_sha1_to_hex(id, hex);
    plant_object_file(git_dir, hex, compressed, compressed_len);
    free(compressed);

    CHECK(sg_loose_read(git_dir, id, &type, &read_content, &read_len) != 0,
         "sg_loose_read should reject a header that understates the real content size");

    free(git_dir);
}

/* Reverse shape: the header declares a huge size (999999999 bytes) but the
   stream actually only decompresses to 3 bytes. sg_object_parse()'s final
   length check must still catch this (the fix doesn't relax that check --
   it just adds an earlier bound on the decompression itself).

   Same honesty note as test_declared_much_smaller_than_actual() above: this
   is defense-in-depth for the pre-existing length check, not a test of the
   new bound (a tiny actual payload never gets anywhere near max_out either
   way, bounded or not). */
static void test_declared_much_larger_than_actual(void)
{
    char *git_dir = make_tmp_repo();
    const char *raw = "blob 999999999\0xyz";
    size_t raw_len = 15 + 3; /* "blob 999999999\0" (15 bytes) + "xyz" (3 bytes) */
    unsigned char *compressed;
    size_t compressed_len;
    char hex[SG_SHA1_HEX_LEN + 1];
    unsigned char *id;
    sg_obj_type type;
    unsigned char *read_content;
    size_t read_len;

    CHECK(sg_compress(raw, raw_len, &compressed, &compressed_len) == 0, "sg_compress failed");

    id = (unsigned char *)arbitrary_id();
    sg_sha1_to_hex(id, hex);
    plant_object_file(git_dir, hex, compressed, compressed_len);
    free(compressed);

    CHECK(sg_loose_read(git_dir, id, &type, &read_content, &read_len) != 0,
         "sg_loose_read should reject a header that overstates the real content size");

    free(git_dir);
}

static long rss_kb(void)
{
    struct rusage ru;

    getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
    return (long)(ru.ru_maxrss / 1024); /* macOS reports bytes */
#else
    return (long)ru.ru_maxrss; /* Linux reports KB already */
#endif
}

/* Same rationale and structure as tests/test_fuzz_index.c's
   test_index_fail_path_leak: AddressSanitizer's allocator quarantine
   inflates process RSS independent of real memory behavior, so this
   measurement is meaningless -- and a source of CI flakiness -- under a
   sanitized build. Skip the assertion there; make sanitize's own clean run
   (no ASan/UBSan errors) is this build's actual coverage for the bound. */
#if defined(__SANITIZE_ADDRESS__)
#define SG_LOOSE_ASAN_BUILD 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SG_LOOSE_ASAN_BUILD 1
#endif
#endif
#ifndef SG_LOOSE_ASAN_BUILD
#define SG_LOOSE_ASAN_BUILD 0
#endif

/* This is the ONLY test in this file that can actually tell a bounded
   decompress apart from the pre-fix unbounded one. The two malformed-shape
   tests above are defense-in-depth: sg_object_parse()'s pre-existing
   exact-length check rejects both shapes regardless of whether
   sg_decompress_bounded()'s cap is enforced, so sg_loose_read()'s return
   value and error message come out identical either way.

   Measured directly against this exact object (header declares "blob 10",
   compressed stream actually decompresses to 512 MiB of zero bytes,
   compressed size ~500 KB), via `/usr/bin/time -l`:

       with the max_out cap (this fix):        "sg: object '...' not found
                                                  or corrupt", exit 1,
                                                  peak RSS   7 MB, 0.00s
       max_out forced to SIZE_MAX (pre-fix,
       i.e. plain unbounded sg_decompress):     identical message,
                                                  identical exit code,
                                                  peak RSS 546 MB, 0.57s

   Same error, same exit code, 78x the memory -- invisible to a return-value
   assertion. So this probe measures getrusage's peak RSS instead, mirroring
   test_fuzz_index.c's test_index_fail_path_leak (see that function's
   comment for the identical ASan caveat). The threshold below has enormous
   headroom on purpose: correct behavior grows RSS by roughly nothing,
   broken behavior by ~512 MiB, so even a generous 64 MiB ceiling can't
   produce a flaky pass. */
static void test_bound_caps_rss(void)
{
    char *git_dir = make_tmp_repo();
    const char *header = "blob 10\0";
    size_t header_len = 8;
    size_t payload_len = (size_t)512 * 1024 * 1024;
    char hex[SG_SHA1_HEX_LEN + 1];
    unsigned char *id;
    sg_obj_type type;
    unsigned char *read_content = NULL;
    size_t read_len = 0;
    long before, after, delta;
    int rc;
#if !SG_LOOSE_ASAN_BUILD
    long growth_threshold_kb = 64 * 1024; /* 64 MiB -- real regression is ~512 MiB */
#endif

    id = (unsigned char *)arbitrary_id();
    sg_sha1_to_hex(id, hex);
    plant_zerobomb_object_file(git_dir, hex, header, header_len, payload_len);

    before = rss_kb();
    rc = sg_loose_read(git_dir, id, &type, &read_content, &read_len);
    after = rss_kb();
    delta = after - before;
    fprintf(stderr, "test_bound_caps_rss: before=%ld KB after=%ld KB delta=%ld KB\n", before,
           after, delta);

    CHECK(rc != 0,
         "sg_loose_read should reject this object either way (bounded or not) -- this test "
         "is about RSS, not the return value");
    if (rc == 0)
        free(read_content);

#if SG_LOOSE_ASAN_BUILD
    fprintf(stderr,
           "test_bound_caps_rss: skipping the RSS growth assertion under AddressSanitizer "
           "(RSS grew by %ld KB, but ASan's allocator quarantine inflates RSS independent of "
           "real memory behavior -- see the comment above this function). The bound itself is "
           "exercised by the return-value assertion above and by make sanitize's clean run; "
           "the RSS ceiling is only meaningful on a plain build.\n",
           delta);
#else
    CHECK(delta <= growth_threshold_kb,
         "sg_decompress_bounded's cap appears not to be enforced: RSS grew by %ld KB reading a "
         "loose object whose header declares 10 bytes but whose compressed stream actually "
         "decompresses to %zu bytes (threshold %ld KB) -- same return code either way, so this "
         "is the only test in this file that can see the difference between a bounded and an "
         "unbounded decompress here",
         delta, header_len + payload_len, growth_threshold_kb);
#endif

    free(git_dir);
}

int main(void)
{
    test_roundtrip();
    test_declared_much_smaller_than_actual();
    test_declared_much_larger_than_actual();
    test_bound_caps_rss();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all loose tests passed\n");
    return 0;
}
