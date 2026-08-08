/* Randomized + mutation-based fuzzer for sg_index_read (include/sg/index.h),
   the parser for git_dir/index -- untrusted-input territory any time an
   index could come from somewhere other than sg itself (a foreign
   .git/index dropped in, or future code that fetches one). Deterministic:
   round i uses seed (SG_FUZZ_SEED_BASE + i) to drive a small xorshift64
   PRNG, never libc rand() (implementation-defined across platforms, which
   would break reproducibility of a reported seed).

   sg_index_read verifies the whole file's trailing SHA-1 checksum BEFORE
   parsing a single entry (index.c:144-150), so pure random bit-flipping
   would spend ~100% of its time failing that checksum and never reach the
   entry-parsing logic this harness exists to exercise. Every generator here
   therefore builds its candidate bytes first and recomputes+appends a
   correct trailer with sg_sha1() as the last step, same as
   sg_index_write does -- see finalize_and_write().

   Three generation strategies, selected by seed % 3:
     0 (valid)      -- a structurally correct v2/v3 index, built from
                        scratch. Must always be accepted; a rejection here
                        is a regression, not an expected fuzz finding.
     1 (corrupted)   -- a valid index put through one of several targeted
                        field-level corruptions (oversized nentries, stray
                        bit flips in the entry/extension region, a 0xFFF
                        name-length escape with no matching NUL, a lying
                        extension size, truncation, padding misalignment).
     2 (random)      -- a valid header (magic + version + small nentries)
                        followed by an arbitrary-length run of random bytes
                        as the "entry data" -- noise aimed at the parser's
                        bounds checks rather than its happy path.

   Run `time build/tests/test_fuzz_index` after building to see actual
   wall-clock; on a 2023-class laptop the default iteration count finishes
   in well under a second, so this comfortably stays out of `make test`'s
   way.

   A second, separate, non-randomized check (test_index_fail_path_leak)
   demonstrates a known bug: sg_index_read's `goto fail` paths (index.c:227,
   239, 245) never free the whole-file `buf`, and free_entries(entries, i)'s
   upper bound excludes the very entry whose path just got malloc'd right
   before the check that sends it to `fail` (index.c:213-222). Neither shows
   up as a wrong return value -- sg_index_read still correctly reports -1 --
   so a functional/return-code fuzz loop like the one above cannot see it,
   and this project's CI runs ASan with detect_leaks=0 (see
   .github/workflows/ci.yml), so leak sanitizer coverage isn't available
   either (it's disabled globally because of two *intentional*
   process-lifetime caches elsewhere -- src/storage/pack.c's mmap registry
   and src/storage/chunk.c's keepalive cache -- not because leaks are
   welcome). test_index_fail_path_leak works around both gaps with a
   poor-man's leak check: parse the same deliberately-malformed index
   (padded_len check fails on its last entry) many times in a row and watch
   getrusage's peak RSS. A build with the leak grows RSS by tens of KB per
   call; a build that frees correctly should show only allocator noise. */
#include "sg/hash.h"
#include "sg/index.h"
#include "sg/repo.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

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

/* ---- xorshift64: a tiny, fully deterministic PRNG (not rand(), which is
   implementation-defined across platforms and would break "seed N always
   reproduces the same bytes") ---- */
static uint64_t g_rng_state;

static void seed_prng(uint64_t seed)
{
    /* xorshift64 is undefined at state 0; fold the seed through a
       fixed odd constant so seed 0 (a perfectly reasonable round index)
       still produces a valid non-zero state. */
    g_rng_state = seed ^ 0x9E3779B97F4A7C15ULL;
    if (g_rng_state == 0)
        g_rng_state = 0x9E3779B97F4A7C15ULL;
}

static uint64_t next_rand(void)
{
    uint64_t x = g_rng_state;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rng_state = x;
    return x;
}

static uint32_t rand_u32(void)
{
    return (uint32_t)(next_rand() >> 32);
}

static unsigned char rand_byte(void)
{
    return (unsigned char)next_rand();
}

static uint32_t rand_below(uint32_t n)
{
    if (n == 0)
        return 0;
    return rand_u32() % n;
}

static long env_long(const char *name, long def)
{
    const char *v = getenv(name);

    if (v == NULL || *v == '\0')
        return def;
    return strtol(v, NULL, 10);
}

/* ---- growable byte buffer ---- */
typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} bytebuf;

static void bb_init(bytebuf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static void bb_free(bytebuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static void bb_reserve(bytebuf *b, size_t extra)
{
    size_t need = b->len + extra;
    size_t new_cap;
    unsigned char *grown;

    if (need <= b->cap)
        return;
    new_cap = (b->cap == 0) ? 256 : b->cap * 2;
    while (new_cap < need)
        new_cap *= 2;
    grown = realloc(b->data, new_cap);
    if (grown == NULL) {
        fprintf(stderr, "setup failed: out of memory building fuzz input\n");
        exit(1);
    }
    b->data = grown;
    b->cap = new_cap;
}

static void bb_bytes(bytebuf *b, const void *p, size_t n)
{
    bb_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static void bb_u8(bytebuf *b, unsigned char v)
{
    bb_bytes(b, &v, 1);
}

static void bb_be32(bytebuf *b, uint32_t v)
{
    unsigned char t[4] = {(unsigned char)(v >> 24), (unsigned char)(v >> 16),
                          (unsigned char)(v >> 8), (unsigned char)v};

    bb_bytes(b, t, 4);
}

static void bb_be16(bytebuf *b, uint16_t v)
{
    unsigned char t[2] = {(unsigned char)(v >> 8), (unsigned char)v};

    bb_bytes(b, t, 2);
}

static void put_be32_at(bytebuf *b, size_t off, uint32_t v)
{
    if (off + 4 > b->len)
        return;
    b->data[off] = (unsigned char)(v >> 24);
    b->data[off + 1] = (unsigned char)(v >> 16);
    b->data[off + 2] = (unsigned char)(v >> 8);
    b->data[off + 3] = (unsigned char)v;
}

/* ---- index-format entry writer, mirrors src/index/index.c's own layout ---- */

static void write_valid_entry(bytebuf *b, const char *path, unsigned int stage, int extended)
{
    size_t name_len = strlen(path);
    uint16_t flags;
    size_t fixed_len, entry_len, padded_len, pad, k;
    unsigned char sha1[SG_SHA1_RAW_LEN];

    bb_be32(b, rand_u32()); /* ctime_sec */
    bb_be32(b, rand_u32()); /* ctime_nsec */
    bb_be32(b, rand_u32()); /* mtime_sec */
    bb_be32(b, rand_u32()); /* mtime_nsec */
    bb_be32(b, rand_u32()); /* dev */
    bb_be32(b, rand_u32()); /* ino */
    bb_be32(b, 0100644);    /* mode */
    bb_be32(b, rand_u32()); /* uid */
    bb_be32(b, rand_u32()); /* gid */
    bb_be32(b, rand_u32()); /* file_size */
    for (k = 0; k < SG_SHA1_RAW_LEN; k++)
        sha1[k] = rand_byte();
    bb_bytes(b, sha1, SG_SHA1_RAW_LEN);

    flags = (uint16_t)((name_len < 0x0FFF ? name_len : 0x0FFF) | ((stage & 0x3) << 12) |
                       (extended ? 0x4000 : 0));
    bb_be16(b, flags);
    if (extended)
        bb_be16(b, 0); /* extra flags word; parser skips it unconditionally */

    bb_bytes(b, path, name_len);

    fixed_len = 62 + (extended ? 2 : 0);
    entry_len = fixed_len + name_len;
    padded_len = ((entry_len + 8) / 8) * 8;
    pad = padded_len - entry_len;
    for (k = 0; k < pad; k++)
        bb_u8(b, 0);
}

/* Builds a fully valid v2/v3 index (header + N entries + an occasional
   well-formed extension block), no trailer yet. *count_out receives N. */
static void gen_valid_index(bytebuf *b, unsigned int *count_out)
{
    unsigned int n = 1 + rand_below(12);
    unsigned int version = rand_below(2) ? 3 : 2;
    unsigned int i;
    char pathbuf[64];

    bb_bytes(b, "DIRC", 4);
    bb_be32(b, version);
    bb_be32(b, n);
    for (i = 0; i < n; i++) {
        int extended = (version == 3) && rand_below(2);

        snprintf(pathbuf, sizeof(pathbuf), "dir%u/file_%u.txt", rand_below(5), i);
        write_valid_entry(b, pathbuf, 0, extended);
    }
    if (rand_below(3) == 0) {
        unsigned int dlen = rand_below(40);
        unsigned int k;

        bb_bytes(b, "TREE", 4);
        bb_be32(b, dlen);
        for (k = 0; k < dlen; k++)
            bb_u8(b, rand_byte());
    }
    if (count_out != NULL)
        *count_out = n;
}

/* Takes a valid index and applies exactly one targeted corruption, covering
   the mutation families the task calls out: an oversized nentries lie,
   scattered bit flips anywhere in the entry/extension region, a 0xFFF
   name-length escape with no NUL to terminate it, a lying extension size,
   truncation (simulating a network cut mid-file), and padding
   misalignment. */
static void gen_corrupted_index(bytebuf *b)
{
    unsigned int n;
    unsigned int strategy = rand_below(6);

    gen_valid_index(b, &n);

    switch (strategy) {
    case 0: /* nentries lies far beyond what the body actually contains */
        put_be32_at(b, 8, n + 1000 + rand_below(5000));
        break;
    case 1: { /* scattered single-bit flips anywhere past the header */
        unsigned int flips = 1 + rand_below(5);
        unsigned int k;

        for (k = 0; k < flips; k++) {
            if (b->len > 12) {
                size_t off = 12 + rand_below((uint32_t)(b->len - 12));

                b->data[off] ^= (unsigned char)(1u << rand_below(8));
            }
        }
        break;
    }
    case 2: /* first entry's name_len escapes to 0xFFF with no NUL anywhere
               after it -- the memchr scan for the terminator must fail cleanly */
        if (b->len >= 12 + 62 + 2) {
            b->data[12 + 60] = 0x0F;
            b->data[12 + 61] = 0xFF;
        }
        break;
    case 3: /* an extension's declared size claims far more than remains */
        if (b->len > 8)
            put_be32_at(b, b->len - 4, 0x7FFFFFFF);
        break;
    case 4: /* truncate at a random point -- header, mid-entry, or right at
               an entry boundary are all reachable */
        if (b->len > 12) {
            size_t cut = 12 + rand_below((uint32_t)(b->len - 12));

            b->len = cut;
        }
        break;
    default: /* header undercounts: nentries says 0 while the body still has
                real entries, so they're all reinterpreted as "extension" bytes */
        put_be32_at(b, 8, 0);
        break;
    }
}

/* A valid header (real magic, real version, small attacker-controlled
   nentries) followed by pure noise for the entry/extension region -- this
   is the "make the bounds checks earn their keep" bucket. */
static void gen_random_index(bytebuf *b)
{
    unsigned int version = rand_below(2) ? 3 : 2;
    unsigned int nentries = rand_below(8);
    unsigned int body_len = rand_below(2000);
    unsigned int i;

    bb_bytes(b, "DIRC", 4);
    bb_be32(b, version);
    bb_be32(b, nentries);
    for (i = 0; i < body_len; i++)
        bb_u8(b, rand_byte());
}

static void write_index_file(const char *git_dir, const unsigned char *data, size_t len)
{
    char path[4300];
    FILE *f;

    snprintf(path, sizeof(path), "%s/index", git_dir);
    f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "setup failed: cannot write %s\n", path);
        exit(1);
    }
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        fprintf(stderr, "setup failed: short write to %s\n", path);
        exit(1);
    }
    fclose(f);
}

static void dump_and_report(const char *label, uint64_t seed, const unsigned char *data,
                            size_t len)
{
    char path[256];
    FILE *f;

    snprintf(path, sizeof(path), "/tmp/sg_fuzz_index_seed_%llu.bin", (unsigned long long)seed);
    f = fopen(path, "wb");
    if (f != NULL) {
        if (len > 0)
            fwrite(data, 1, len, f);
        fclose(f);
    }
    fprintf(stderr, "FAIL round seed=%llu (%s): dumped %zu bytes to %s\n",
           (unsigned long long)seed, label, len, path);
    failures++;
}

static void fuzz_index_round(uint64_t seed, const char *git_dir)
{
    unsigned int mode = (unsigned int)(seed % 3);
    unsigned int expected_count = 0;
    bytebuf b;
    unsigned char trailer[SG_SHA1_RAW_LEN];
    sg_index idx;
    int rc;

    seed_prng(seed);
    bb_init(&b);

    switch (mode) {
    case 0:
        gen_valid_index(&b, &expected_count);
        break;
    case 1:
        gen_corrupted_index(&b);
        break;
    default:
        gen_random_index(&b);
        break;
    }

    /* the whole point: without a correct trailer, sg_index_read rejects
       everything at the checksum gate before entry parsing ever runs */
    sg_sha1(b.data, b.len, trailer);
    bb_bytes(&b, trailer, SG_SHA1_RAW_LEN);

    write_index_file(git_dir, b.data, b.len);

    rc = sg_index_read(git_dir, &idx);

    if (mode == 0) {
        if (rc != 0) {
            dump_and_report("valid input rejected", seed, b.data, b.len);
        } else if (idx.count != expected_count) {
            dump_and_report("valid input parsed with wrong entry count", seed, b.data, b.len);
            sg_index_free(&idx);
        } else {
            sg_index_free(&idx);
        }
    } else if (rc == 0) {
        size_t i;
        int bad = 0;

        for (i = 0; i < idx.count; i++) {
            if (idx.entries[i].path == NULL) {
                bad = 1;
                break;
            }
        }
        if (bad)
            dump_and_report("parsed entry with NULL path", seed, b.data, b.len);
        sg_index_free(&idx);
    }
    /* rc == -1 for a corrupted/random sample is the expected, healthy
       outcome -- nothing to check beyond "it didn't crash". */

    bb_free(&b);
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

/* AddressSanitizer's allocator runs a quarantine: freed chunks are held back
   from real reuse for a while (to catch use-after-free), which inflates
   process RSS independent of whether anything actually leaked. Measured
   directly on this repo's ASan build: this exact probe reports "RSS grew by
   38064 KB" under ASan even though the code it's exercising here frees
   correctly -- a false positive, not evidence of a leak (the same probe
   passes cleanly, and passes with quarantine disabled via
   ASAN_OPTIONS=quarantine_size_mb=0, on the very same build). Since CI's
   sanitizers job runs `make CC=clang sanitize`, which depends on `test`,
   this false positive would otherwise turn that job red on every run. Skip
   the RSS measurement under ASan; leaks on that build are ASan's job to
   catch via its own instrumentation (moot here anyway, since CI runs it
   with detect_leaks=0 -- see .github/workflows/ci.yml -- because of the two
   *intentional* process-lifetime caches documented in the file header
   comment above; this probe was never meant to substitute for leak
   sanitizer, only to cover the gap while it's off). */
#if defined(__SANITIZE_ADDRESS__)
#define SG_FUZZ_INDEX_ASAN_BUILD 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SG_FUZZ_INDEX_ASAN_BUILD 1
#endif
#endif
#ifndef SG_FUZZ_INDEX_ASAN_BUILD
#define SG_FUZZ_INDEX_ASAN_BUILD 0
#endif

/* See the file header comment for why this check exists and how it works.

   The final, deliberately-unpadded entry's path used to be a fixed 100
   bytes. That made this probe blind to the free_entries(entries, i) vs.
   free_entries(entries, i + 1) off-by-one specifically (index.c:257): with
   only 100 bytes leaked per parse, SG_FUZZ_LEAK_ITERS's default 1000 rounds
   tops out at ~100 KB of leaked path memory, far under this function's own
   8 MiB growth_threshold_kb -- so reverting the `i + 1` fix back to `i`
   passed this check right along with a correctly-freeing build (verified by
   mutation: see the implementer's report for this task). The path is now
   tens of KB instead, via the 0x0FFF name-length escape (index.c:227-233,
   which scans for a NUL terminator rather than trusting the 12-bit flags
   field, so it isn't bounded by flags' own 4094-byte ceiling) -- at
   SG_FUZZ_LEAK_PATH_LEN's default of 65536 bytes, 1000 rounds of the actual
   leak is ~64 MiB, a full order of magnitude past the threshold, while a
   correctly-freeing build still shows only allocator noise. */
static void test_index_fail_path_leak(const char *git_dir)
{
    bytebuf b;
    unsigned int n_valid = 40;
    unsigned int j;
    char pathbuf[32];
    long iters = env_long("SG_FUZZ_LEAK_ITERS", 1000);
    long leak_path_len = env_long("SG_FUZZ_LEAK_PATH_LEN", 65536);
#if !SG_FUZZ_INDEX_ASAN_BUILD
    long growth_threshold_kb = 8192; /* 8 MiB -- real leak is tens of MB at these counts */
#endif
    long before, after, delta;
    long i;

    bb_init(&b);
    bb_bytes(&b, "DIRC", 4);
    bb_be32(&b, 2);
    bb_be32(&b, n_valid + 1);
    for (j = 0; j < n_valid; j++) {
        snprintf(pathbuf, sizeof(pathbuf), "f%u.txt", j);
        write_valid_entry(&b, pathbuf, 0, 0);
    }
    {
        unsigned char sha1[SG_SHA1_RAW_LEN];
        uint16_t escape_flags = 0x0FFF; /* name_len escape: parser scans for a
                                            NUL terminator instead of trusting
                                            a length carried in these 12 bits,
                                            so the real path below can be far
                                            longer than 0x0FFE bytes */
        char *name = malloc((size_t)leak_path_len + 1);

        if (name == NULL) {
            fprintf(stderr, "setup failed: out of memory building leak-probe path (%ld bytes)\n",
                   leak_path_len);
            exit(1);
        }
        memset(name, 'x', (size_t)leak_path_len);
        name[leak_path_len] = '\0'; /* the escape scan's terminator */
        memset(sha1, 0, sizeof(sha1));

        bb_be32(&b, 0); /* ctime_sec */
        bb_be32(&b, 0); /* ctime_nsec */
        bb_be32(&b, 0); /* mtime_sec */
        bb_be32(&b, 0); /* mtime_nsec */
        bb_be32(&b, 0); /* dev */
        bb_be32(&b, 0); /* ino */
        bb_be32(&b, 0100644); /* mode */
        bb_be32(&b, 0); /* uid */
        bb_be32(&b, 0); /* gid */
        bb_be32(&b, 0); /* file_size */
        bb_bytes(&b, sha1, SG_SHA1_RAW_LEN);
        bb_be16(&b, escape_flags);
        bb_bytes(&b, name, (size_t)leak_path_len + 1); /* path bytes + the NUL terminator
                                                            the escape scan looks for */
        /* deliberately: no padding bytes here, so padded_len check fails */
        free(name);
    }

    {
        unsigned char trailer[SG_SHA1_RAW_LEN];

        sg_sha1(b.data, b.len, trailer);
        bb_bytes(&b, trailer, SG_SHA1_RAW_LEN);
    }

    write_index_file(git_dir, b.data, b.len);

    /* one warm-up parse so the allocator's steady-state page footprint is
       established before the baseline measurement */
    {
        sg_index idx;

        if (sg_index_read(git_dir, &idx) == 0)
            sg_index_free(&idx);
    }
    before = rss_kb();

    for (i = 0; i < iters; i++) {
        sg_index idx;
        int rc = sg_index_read(git_dir, &idx);

        CHECK(rc == -1,
             "expected the deliberately unpadded final entry to be rejected, got %d", rc);
        if (rc == 0)
            sg_index_free(&idx);
    }
    after = rss_kb();
    delta = after - before;

#if SG_FUZZ_INDEX_ASAN_BUILD
    fprintf(stderr,
           "test_index_fail_path_leak: skipping the RSS growth assertion under "
           "AddressSanitizer (RSS grew by %ld KB, but ASan's allocator quarantine inflates "
           "RSS independent of real leaks -- see the comment above this function). Leak "
           "coverage on this build is ASan's own instrumentation's job, not this probe's; "
           "note CI runs it with detect_leaks=0 for unrelated reasons (see "
           ".github/workflows/ci.yml and this file's header comment).\n",
           delta);
#else
    CHECK(delta <= growth_threshold_kb,
         "sg_index_read's failure path appears to leak memory: RSS grew by %ld KB over %ld "
         "repeated parses of the same deliberately-malformed index (threshold %ld KB) -- "
         "consistent with the known unfreed whole-file buf / entry path on index.c's `goto "
         "fail` paths (index.c:227,239,245 don't free buf; free_entries(entries, i) at :228 "
         "excludes the very entry whose path was just malloc'd)",
         delta, iters, growth_threshold_kb);
#endif

    bb_free(&b);
}

/* Deterministic, hand-crafted regression for index.c's nentries upper bound
   (the `max_possible_entries` check documented above it in index.c): a
   30-byte file (just the 12-byte header plus the 20-byte trailer, no entry
   bytes at all) that declares nentries = 0xFFFFFFFF, with the trailer
   checksum computed correctly over exactly those bytes so the file clears
   the checksum gate and reaches the nentries check. Without that check,
   sg_index_read would proceed to `malloc((size_t)0xFFFFFFFF *
   sizeof(sg_index_entry))` -- hundreds of GB for an entry struct this size
   -- off a 30-byte attacker-controlled file. No fuzz round reaches this: the
   random-generator strategies above only ever grow nentries by a few
   thousand past a real body (gen_corrupted_index's case 0), never anywhere
   near a bare u32's max.

   IMPORTANT, honestly: this is a smoke test, not a discriminating one. It
   does NOT distinguish "the max_possible_entries check rejected this" from
   "the check is gone and malloc((size_t)0xFFFFFFFF * sizeof(sg_index_entry))
   itself failed" -- both paths return rc == -1 from sg_index_read, and the
   CHECK below only looks at rc. Verified by mutation: deleting the
   max_possible_entries check in index.c and rebuilding still passes this
   test, because 0xFFFFFFFF * sizeof(sg_index_entry) (>=176 GB on this
   struct's layout) simply fails to allocate on ordinary hardware, producing
   the same rc == -1 by a completely different route. This also holds under
   ASan: 0xFFFFFFFF * sizeof(sg_index_entry) stays under ASan's ~1 TB
   allocation-size-too-big threshold, so removing the check doesn't trip
   that instrumentation either -- confirmed by actually building ASan with
   the check removed and observing no sanitizer report. A machine with
   enough RAM (or overcommit) to satisfy that malloc could observably differ,
   but that's not this test rig, and there is no public-API way from here to
   force the distinction on this platform. The check's value is therefore
   NOT proven by this test -- it's real for a different reason entirely:
   avoiding the attempt to make a many-hundred-GB allocation request off a
   30-byte attacker-controlled file in the first place (slow, and a
   footgun on systems where an allocation that large doesn't cleanly fail).
   Don't read a green result here as "the upper bound is covered" -- it
   isn't, and no fix to this test file is known to make it so, since the
   two code paths are observationally identical through sg_index_read's
   public return value on every platform this project builds for. */
static void test_oversized_nentries_rejected(const char *git_dir)
{
    bytebuf b;
    unsigned char trailer[SG_SHA1_RAW_LEN];
    sg_index idx;
    int rc;

    bb_init(&b);
    bb_bytes(&b, "DIRC", 4);
    bb_be32(&b, 2);
    bb_be32(&b, 0xFFFFFFFFu);
    sg_sha1(b.data, b.len, trailer);
    bb_bytes(&b, trailer, SG_SHA1_RAW_LEN);

    write_index_file(git_dir, b.data, b.len);

    rc = sg_index_read(git_dir, &idx);
    CHECK(rc == -1,
         "expected sg_index_read to reject nentries=0xFFFFFFFF declared against a 30-byte "
         "file with no room for even one entry (got rc=%d)",
         rc);
    if (rc == 0)
        sg_index_free(&idx);

    bb_free(&b);
}

static char *make_tmp_repo(void)
{
    static char template[] = "/tmp/sg_fuzz_index_test_XXXXXX";
    char *path = strdup(template);
    char git_dir[4096];

    if (path == NULL || mkdtemp(path) == NULL) {
        fprintf(stderr, "setup failed: mkdtemp\n");
        exit(1);
    }
    if (sg_repo_init(path) != 0) {
        fprintf(stderr, "setup failed: sg_repo_init\n");
        exit(1);
    }
    snprintf(git_dir, sizeof(git_dir), "%s/.git", path);
    free(path);
    return strdup(git_dir);
}

/* ---- whole-binary watchdog ----
   A single call anywhere in sg_index_read hanging (rather than returning a
   wrong value) would otherwise wedge this entire binary indefinitely: unlike
   test_fuzz_pack.c's test_zero_declared_size_with_real_data, nothing here
   runs its risky calls in a forked child, so there's no per-call timeout to
   catch it, and CI's job-level timeout is 6 hours (see
   .github/workflows/ci.yml) -- a hang would burn that whole budget before
   turning the build red. alarm()+SIGALRM turns any hang, at any call site in
   this binary, into a bounded, clearly-diagnosed failure instead.

   The handler only calls write() (async-signal-safe) on a buffer formatted
   *before* the alarm was armed, then _exit() -- never fprintf/snprintf from
   inside the handler itself, since those aren't guaranteed async-signal-safe.

   Default timeout: measured on a 2023-class laptop (clang, no sanitizer,
   this repo's default `make` build), `SG_FUZZ_ITERS=100000
   build/tests/test_fuzz_index` (the exact invocation CI's fuzz-parse job
   uses) completes in ~5 seconds. 600s leaves two orders of magnitude of
   headroom for slower/loaded CI runners, while staying far short of the
   6-hour job timeout. Override with SG_FUZZ_TIMEOUT (seconds) for a
   deliberately slower/bigger local run. */
static char g_watchdog_msg[512];
static size_t g_watchdog_msg_len;

static void watchdog_fire(int sig)
{
    (void)sig;
    write(STDERR_FILENO, g_watchdog_msg, g_watchdog_msg_len);
    _exit(1);
}

int main(void)
{
    long iters = env_long("SG_FUZZ_ITERS", 1500);
    long seed_base = env_long("SG_FUZZ_SEED_BASE", 0);
    long watchdog_timeout = env_long("SG_FUZZ_TIMEOUT", 600);
    char *git_dir;
    long i;
    int n;

    n = snprintf(g_watchdog_msg, sizeof(g_watchdog_msg),
                "sg: test_fuzz_index watchdog fired after %ld second(s) with no completion "
                "(override via SG_FUZZ_TIMEOUT) -- SG_FUZZ_ITERS=%ld SG_FUZZ_SEED_BASE=%ld; "
                "this almost certainly means sg_index_read (or something it calls) hung "
                "somewhere in this run, not that the machine is merely slow; aborting\n",
                watchdog_timeout, iters, seed_base);
    g_watchdog_msg_len =
        (n > 0 && (size_t)n < sizeof(g_watchdog_msg)) ? (size_t)n : sizeof(g_watchdog_msg) - 1;
    signal(SIGALRM, watchdog_fire);
    alarm((unsigned int)(watchdog_timeout > 0 ? watchdog_timeout : 0));

    git_dir = make_tmp_repo();

    for (i = 0; i < iters; i++)
        fuzz_index_round((uint64_t)(seed_base + i), git_dir);

    test_index_fail_path_leak(git_dir);
    test_oversized_nentries_rejected(git_dir);

    free(git_dir);
    alarm(0);

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all index fuzz checks passed (%ld rounds, seed_base %ld)\n", iters, seed_base);
    return 0;
}
