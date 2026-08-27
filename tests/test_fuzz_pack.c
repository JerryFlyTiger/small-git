/* Randomized + mutation-based fuzzer for the packfile parsing surface:
   the pure varint/delta functions (src/storage/pack.c:63,118,141,164 --
   non-static but not part of sg/pack.h's public surface, declared here via
   extern the same way tests/test_pack_varint.c and tests/test_delta_apply.c
   already do), sg_pack_index_existing (include/sg/pack.h:76 -- the gatekeeper
   for a whole packfile that just came off the network), and sg_pack_read
   (pack.h:32 -- the .idx + .pack query path, which reimplements its own
   parsing of both files rather than sharing code with the writer).

   Deterministic: round i uses seed (SG_FUZZ_SEED_BASE + i) through a small
   xorshift64 PRNG (not libc rand(), which is implementation-defined across
   platforms and would break "seed N always reproduces the same bytes").

   Three layers, cheapest first:

     1. Pure functions -- buffer in, buffer/values out, no file I/O. Fed
        random and semi-structured byte strings, including varint
        continuation-byte streams long enough to hit the shift>=64 UB guard,
        and an explicit boundary sweep of sg_pack_encode_obj_header /
        sg_pack_decode_obj_header around UINT32_MAX and UINT64_MAX (the
        magnitude at which pack.c:294's `(uInt)expected_len` truncation --
        one of this task's known ground-truth bugs -- would matter; the
        truncation itself lives in the static, non-exported pack_inflate,
        so it can't be unit-tested directly here, only through the public
        sg_pack_index_existing/sg_pack_read entry points in layers 2-3).

     2. sg_pack_index_existing -- built-from-scratch packs (real zlib
        streams via sg_compress, real varint headers via
        sg_pack_encode_obj_header), covering a literal blob, an OFS_DELTA,
        and a REF_DELTA, then mutated: header object count lied about,
        object type nibble forced to an unrecognized value, OFS_DELTA's
        backwards offset corrupted (zero, or past its own start), REF_DELTA
        base id pointed at an object that doesn't exist, truncated/extended
        zlib streams, and trailing garbage appended after the last entry.

     3. sg_pack_read -- a real (pack,idx) pair produced by sg_pack_write,
        with one or both files corrupted on disk and re-checksummed, then
        queried. Every round uses a brand-new mkdtemp git_dir: pack.c:498's
        mmap pack registry is keyed by git_dir and only rescans when
        objects/pack/'s mtime has visibly moved, and st_mtime has one-second
        resolution -- reusing a git_dir across rounds within the same second
        would silently read a stale cached pack instead of this round's
        mutation.

   Run `time build/tests/test_fuzz_pack` to see actual wall-clock; the
   defaults below were sized to keep that under a few seconds. */
#include "sg/hash.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/pack.h"
#include "sg/repo.h"
#include "sg/zutil.h"

#include <dirent.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern size_t sg_pack_decode_obj_header(const unsigned char *p, size_t avail, int *type_out,
                                        uint64_t *size_out);
extern size_t sg_pack_encode_obj_header(int type, uint64_t size, unsigned char *out);
extern size_t sg_pack_decode_ofs_delta_offset(const unsigned char *p, size_t avail,
                                              uint64_t *offset_out);
extern size_t sg_pack_decode_delta_size(const unsigned char *p, size_t avail, uint64_t *size_out);
extern int sg_pack_delta_apply(const unsigned char *base, size_t base_len,
                               const unsigned char *delta, size_t delta_len, unsigned char **out,
                               size_t *out_len);

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

/* git's own pack object type numbering, duplicated here the same way
   test_pack_varint.c/test_delta_apply.c duplicate pack.c's non-exported
   function signatures -- these constants aren't part of any header either. */
#define TT_COMMIT 1
#define TT_BLOB 3
#define TT_OFS_DELTA 6
#define TT_REF_DELTA 7

/* ---- xorshift64 PRNG (deterministic, not rand()) ---- */
static uint64_t g_rng_state;

static void seed_prng(uint64_t seed)
{
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

/* ---- growable byte buffer (same shape as tests/test_fuzz_index.c's; kept
   as a separate copy rather than shared, per this project's convention of
   small per-file helpers -- see CLAUDE.md's note on path_join/strbuf) ---- */
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

static void put_be32_at(bytebuf *b, size_t off, uint32_t v)
{
    if (off + 4 > b->len)
        return;
    b->data[off] = (unsigned char)(v >> 24);
    b->data[off + 1] = (unsigned char)(v >> 16);
    b->data[off + 2] = (unsigned char)(v >> 8);
    b->data[off + 3] = (unsigned char)v;
}

static void dump_and_report(const char *file_label, const char *what, uint64_t seed,
                            const unsigned char *data, size_t len)
{
    char path[256];
    FILE *f;

    snprintf(path, sizeof(path), "/tmp/sg_fuzz_pack_%s_seed_%llu.bin", file_label,
            (unsigned long long)seed);
    f = fopen(path, "wb");
    if (f != NULL) {
        if (len > 0)
            fwrite(data, 1, len, f);
        fclose(f);
    }
    fprintf(stderr, "FAIL round seed=%llu (%s): dumped %zu bytes to %s\n",
           (unsigned long long)seed, what, len, path);
    failures++;
}

/* ==================================================================== *
 * Layer 1: pure functions (no file I/O)
 * ==================================================================== */

static void test_obj_header_boundary_roundtrip(void)
{
    /* deliberately spans values on both sides of the 32-bit boundary that
       pack_inflate's `(uInt)expected_len` truncation (pack.c:294) cares
       about -- pack_inflate itself is static and can't be reached directly
       from here, but this pins that the varint layer underneath it encodes
       and decodes these magnitudes correctly, so any future truncation bug
       is provably in the inflate call and not in this layer. */
    static const uint64_t values[] = {
        0,          1,          15,         16,          127,
        128,        (uint64_t)UINT32_MAX - 1, (uint64_t)UINT32_MAX, (uint64_t)UINT32_MAX + 1,
        ((uint64_t)UINT32_MAX) * 2, UINT64_MAX / 2, UINT64_MAX,
    };
    size_t n = sizeof(values) / sizeof(values[0]);
    size_t i;

    for (i = 0; i < n; i++) {
        unsigned char buf[16];
        size_t elen = sg_pack_encode_obj_header(TT_BLOB, values[i], buf);
        int type;
        uint64_t size;
        size_t dlen;

        CHECK(elen > 0 && elen <= 16, "encode produced implausible length %zu for value %llu",
             elen, (unsigned long long)values[i]);
        dlen = sg_pack_decode_obj_header(buf, elen, &type, &size);
        CHECK(dlen == elen && type == TT_BLOB && size == values[i],
             "roundtrip mismatch at value %llu: dlen=%zu elen=%zu type=%d size=%llu",
             (unsigned long long)values[i], dlen, elen, type, (unsigned long long)size);
    }
}

/* Random and continuation-heavy buffers through all three decoders. The
   only real assertion is "never reads past avail and never crashes" --
   correctness of well-formed input is already covered by
   tests/test_pack_varint.c's hand-verified vectors. */
static void fuzz_decode_functions_round(uint64_t seed)
{
    unsigned char buf[24];
    size_t avail = 1 + rand_below(sizeof(buf) - 1);
    size_t i;

    seed_prng(seed);
    /* bias heavily toward the continuation bit being set, so most rounds
       exercise the shift>=64 UB guard rather than terminating on byte 1 */
    for (i = 0; i < avail; i++)
        buf[i] = (rand_below(10) < 8) ? (unsigned char)(0x80 | rand_byte()) : rand_byte();

    {
        int type;
        uint64_t size;
        size_t n = sg_pack_decode_obj_header(buf, avail, &type, &size);

        CHECK(n <= avail, "decode_obj_header consumed %zu > avail %zu (seed %llu)", n, avail,
             (unsigned long long)seed);
    }
    {
        uint64_t off;
        size_t n = sg_pack_decode_ofs_delta_offset(buf, avail, &off);

        CHECK(n <= avail, "decode_ofs_delta_offset consumed %zu > avail %zu (seed %llu)", n,
             avail, (unsigned long long)seed);
    }
    {
        uint64_t sz;
        size_t n = sg_pack_decode_delta_size(buf, avail, &sz);

        CHECK(n <= avail, "decode_delta_size consumed %zu > avail %zu (seed %llu)", n, avail,
             (unsigned long long)seed);
    }
}

/* Both a semi-structured delta instruction stream (random opcodes, whose
   argument byte counts are derived correctly from the opcode so the
   fuzzer explores real control flow rather than bailing out on the first
   byte) and pure noise, against sg_pack_delta_apply. Base/target sizes are
   kept small and single-byte so the base_size cross-check has a chance to
   pass at least sometimes. */
static void fuzz_delta_apply_round(uint64_t seed)
{
    unsigned char base[64];
    unsigned char delta[128];
    size_t base_len = rand_below(64);
    size_t delta_len;
    size_t i;
    unsigned char *out = NULL;
    size_t out_len = 0;
    int rc;

    seed_prng(seed);
    for (i = 0; i < base_len; i++)
        base[i] = rand_byte();

    if (rand_below(2) == 0) {
        /* structured: valid base/target size varints (both < 128, so a
           single byte each), then a run of syntactically-plausible
           instructions */
        unsigned int nops = rand_below(6);
        unsigned int k;

        delta_len = 0;
        delta[delta_len++] = (unsigned char)(base_len & 0x7f);
        delta[delta_len++] = (unsigned char)rand_below(128);
        for (k = 0; k < nops && delta_len < 120; k++) {
            unsigned char op = rand_byte();

            delta[delta_len++] = op;
            if (op & 0x80) {
                int bit;

                for (bit = 0; bit < 7 && delta_len < 126; bit++) {
                    if ((op >> bit) & 1)
                        delta[delta_len++] = rand_byte();
                }
            } else if (op != 0) {
                unsigned int lit = op, b;

                for (b = 0; b < lit && delta_len < 126; b++)
                    delta[delta_len++] = rand_byte();
            }
        }
    } else {
        delta_len = 1 + rand_below(sizeof(delta) - 1);
        for (i = 0; i < delta_len; i++)
            delta[i] = rand_byte();
    }

    rc = sg_pack_delta_apply(base_len > 0 ? base : NULL, base_len, delta, delta_len, &out,
                             &out_len);
    if (rc == 0) {
        CHECK(out != NULL, "seed %llu: success but *out left NULL", (unsigned long long)seed);
        free(out);
    } else {
        CHECK(out == NULL, "seed %llu: failure but *out was set (leak/dangling risk)",
             (unsigned long long)seed);
    }
}

/* ==================================================================== *
 * Layer 2: sg_pack_index_existing -- whole-pack validation
 * ==================================================================== */

static void append_literal_entry(bytebuf *b, int raw_type, const unsigned char *content,
                                 size_t len)
{
    unsigned char hdr[16];
    size_t hdr_len = sg_pack_encode_obj_header(raw_type, (uint64_t)len, hdr);
    unsigned char *compressed;
    size_t compressed_len;

    if (sg_compress(content, len, &compressed, &compressed_len) != 0) {
        fprintf(stderr, "setup failed: sg_compress\n");
        exit(1);
    }
    bb_bytes(b, hdr, hdr_len);
    bb_bytes(b, compressed, compressed_len);
    free(compressed);
}

/* Encodes an OFS_DELTA backwards offset for rel < 128 only (single byte,
   no continuation) -- sufficient because build_pack keeps every object
   small enough that consecutive entries are always under 128 bytes apart;
   see build_pack's comment. */
static void encode_ofs_offset_small(bytebuf *b, size_t rel)
{
    if (rel >= 128) {
        fprintf(stderr, "setup failed: rel offset %zu too large for the simplified encoder\n",
               rel);
        exit(1);
    }
    bb_u8(b, (unsigned char)rel);
}

static void append_ofs_delta_entry(bytebuf *b, size_t rel, const unsigned char *delta,
                                   size_t delta_len)
{
    unsigned char hdr[16];
    size_t hdr_len = sg_pack_encode_obj_header(TT_OFS_DELTA, (uint64_t)delta_len, hdr);
    unsigned char *compressed;
    size_t compressed_len;

    if (sg_compress(delta, delta_len, &compressed, &compressed_len) != 0) {
        fprintf(stderr, "setup failed: sg_compress\n");
        exit(1);
    }
    bb_bytes(b, hdr, hdr_len);
    encode_ofs_offset_small(b, rel);
    bb_bytes(b, compressed, compressed_len);
    free(compressed);
}

static void append_ref_delta_entry(bytebuf *b, const unsigned char base_id[SG_SHA1_RAW_LEN],
                                   const unsigned char *delta, size_t delta_len)
{
    unsigned char hdr[16];
    size_t hdr_len = sg_pack_encode_obj_header(TT_REF_DELTA, (uint64_t)delta_len, hdr);
    unsigned char *compressed;
    size_t compressed_len;

    if (sg_compress(delta, delta_len, &compressed, &compressed_len) != 0) {
        fprintf(stderr, "setup failed: sg_compress\n");
        exit(1);
    }
    bb_bytes(b, hdr, hdr_len);
    bb_bytes(b, base_id, SG_SHA1_RAW_LEN);
    bb_bytes(b, compressed, compressed_len);
    free(compressed);
}

/* A "copy the whole base verbatim" delta instruction stream, same encoding
   family as tests/test_delta_apply.c's test_pure_copy: base/target size
   varints (single byte each, since base_len < 128 here) followed by one
   copy opcode with an explicit one-byte size and no offset bytes (offset
   defaults to 0). */
static size_t make_pure_copy_delta(unsigned char *out, size_t base_len)
{
    out[0] = (unsigned char)base_len;
    out[1] = (unsigned char)base_len;
    out[2] = 0x90; /* copy, one size byte, no offset bytes -> offset 0 */
    out[3] = (unsigned char)base_len;
    return 4;
}

/* Builds a small, fully valid pack: two literal blobs, an OFS_DELTA that
   reconstructs the first verbatim, and a REF_DELTA that reconstructs the
   second verbatim. Content lengths are kept small (<64 bytes, mostly
   incompressible ASCII) so every entry stays comfortably under 128 bytes
   and encode_ofs_offset_small's single-byte assumption holds. Returns the
   byte offset of each of the 4 entries in entry_off, and the two literal
   blobs' content/length for callers that want to query them back. No
   trailer yet -- caller finalizes. */
static void build_pack(bytebuf *b, size_t entry_off[4], unsigned char content0[64],
                       size_t *len0_out, unsigned char content1[64], size_t *len1_out)
{
    size_t len0 = 5 + rand_below(26);
    size_t len1 = 5 + rand_below(26);
    unsigned char c0[64], c1[64];
    unsigned char delta0[4], delta1[4];
    unsigned char base1_id[SG_SHA1_RAW_LEN];
    size_t i;

    for (i = 0; i < len0; i++)
        c0[i] = (unsigned char)('a' + rand_below(26));
    for (i = 0; i < len1; i++)
        c1[i] = (unsigned char)('A' + rand_below(26));

    bb_bytes(b, "PACK", 4);
    bb_be32(b, 2);
    bb_be32(b, 4);

    entry_off[0] = b->len;
    append_literal_entry(b, TT_BLOB, c0, len0);

    entry_off[1] = b->len;
    append_literal_entry(b, TT_BLOB, c1, len1);

    entry_off[2] = b->len;
    make_pure_copy_delta(delta0, len0);
    append_ofs_delta_entry(b, entry_off[2] - entry_off[0], delta0, 4);

    entry_off[3] = b->len;
    sg_object_hash(SG_OBJ_BLOB, c1, len1, base1_id);
    make_pure_copy_delta(delta1, len1);
    append_ref_delta_entry(b, base1_id, delta1, 4);

    if (content0 != NULL)
        memcpy(content0, c0, len0);
    if (len0_out != NULL)
        *len0_out = len0;
    if (content1 != NULL)
        memcpy(content1, c1, len1);
    if (len1_out != NULL)
        *len1_out = len1;
}

static void gen_random_pack(bytebuf *b)
{
    unsigned int count = rand_below(4);
    unsigned int body_len = rand_below(1500);
    unsigned int i;

    bb_bytes(b, "PACK", 4);
    bb_be32(b, 2);
    bb_be32(b, count);
    for (i = 0; i < body_len; i++)
        bb_u8(b, rand_byte());
}

/* One targeted corruption of an otherwise-valid build_pack() output,
   covering: a lying object count, an unrecognized object type nibble, a
   corrupted OFS_DELTA backwards offset (zero or past-its-own-start),
   a REF_DELTA base id that doesn't resolve to anything, a truncated
   entry (cuts the zlib stream short), and trailing garbage appended after
   the last entry. */
static void corrupt_pack(bytebuf *b, const size_t entry_off[4])
{
    unsigned int strategy = rand_below(6);

    switch (strategy) {
    case 0: /* header count lies: too high (past what the bytes can hold) */
        put_be32_at(b, 8, 4 + 1000 + rand_below(5000));
        break;
    case 1: { /* force one entry's type nibble to an unrecognized value */
        size_t off = entry_off[rand_below(2)]; /* the two literal blobs */
        unsigned char bad_type = rand_below(2) ? 0 : 5; /* both unused by git */

        b->data[off] = (unsigned char)((b->data[off] & 0x8F) | (bad_type << 4));
        break;
    }
    case 2: { /* OFS_DELTA offset corrupted: 0 (explicitly rejected) or a
                 value past this entry's own start (also rejected) */
        size_t off = entry_off[2] + 1; /* header is 1 byte for our small sizes */

        b->data[off] = rand_below(2) ? 0x00 : 0x7F;
        break;
    }
    case 3: /* REF_DELTA base id corrupted to something that resolves to
               nothing -- neither an earlier in-pack entry nor a loose object
               in the (freshly created, otherwise-empty) git_dir */
        if (b->len >= entry_off[3] + 21) {
            size_t k;

            for (k = 0; k < SG_SHA1_RAW_LEN; k++)
                b->data[entry_off[3] + 1 + k] = rand_byte();
        }
        break;
    case 4: /* truncate mid-stream: cuts one entry's zlib data short */
        if (b->len > entry_off[3] + 2) {
            size_t cut = entry_off[3] + 2 + rand_below((uint32_t)(b->len - entry_off[3] - 2));

            b->len = cut;
        }
        break;
    default: /* trailing garbage appended after the last real entry */
        {
            unsigned int extra = 1 + rand_below(16);
            unsigned int k;

            for (k = 0; k < extra; k++)
                bb_u8(b, rand_byte());
        }
        break;
    }
}

static void write_whole_file(const char *path, const unsigned char *data, size_t len)
{
    FILE *f;

    /* sg_pack_write's write_atomic leaves both the .pack and .idx it
       produces chmod 0444 (deliberately read-only on disk, like real git);
       loosen that before overwriting in place with a mutated copy. */
    chmod(path, 0644);
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

static char *make_tmp_repo(const char *tag)
{
    char template[128];
    char *path;
    char git_dir[4096];

    snprintf(template, sizeof(template), "/tmp/sg_fuzz_pack_%s_XXXXXX", tag);
    path = strdup(template);
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

static void rm_rf(const char *path)
{
    char cmd[4300];

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    if (system(cmd) != 0) { /* best-effort cleanup */
    }
}

static char *repo_root_of_git_dir(const char *git_dir)
{
    size_t len = strlen(git_dir);
    char *root;

    if (len > 5 && strcmp(git_dir + len - 5, "/.git") == 0) {
        root = malloc(len - 4);
        memcpy(root, git_dir, len - 5);
        root[len - 5] = '\0';
        return root;
    }
    return strdup(git_dir);
}

/* mode: 0 = valid (must be accepted), 1 = one targeted corruption,
   2 = random noise body. git_dir/objects/pack/fuzz.pack is reused across
   rounds (overwritten each time); sg_pack_index_existing doesn't consult
   the mmap pack registry to READ this file (only pack_read* does, via
   sg_pack_read), so reuse here is safe and much cheaper than a fresh
   mkdtemp per round. */
static void fuzz_pack_index_existing_round(uint64_t seed, const char *git_dir,
                                           const char *pack_path)
{
    unsigned int mode = (unsigned int)(seed % 3);
    bytebuf b;
    size_t entry_off[4];
    unsigned char content0[64], content1[64];
    size_t len0 = 0, len1 = 0;
    unsigned char trailer[SG_SHA1_RAW_LEN];
    int rc;

    seed_prng(seed);
    bb_init(&b);

    switch (mode) {
    case 0:
        build_pack(&b, entry_off, content0, &len0, content1, &len1);
        break;
    case 1:
        build_pack(&b, entry_off, content0, &len0, content1, &len1);
        corrupt_pack(&b, entry_off);
        break;
    default:
        gen_random_pack(&b);
        break;
    }

    sg_sha1(b.data, b.len, trailer);
    bb_bytes(&b, trailer, SG_SHA1_RAW_LEN);

    write_whole_file(pack_path, b.data, b.len);

    rc = sg_pack_index_existing(pack_path);

    if (mode == 0) {
        if (rc != 0) {
            dump_and_report("index_existing", "valid pack rejected", seed, b.data, b.len);
        } else {
            unsigned char id0[SG_SHA1_RAW_LEN];
            sg_obj_type type;
            unsigned char *content;
            size_t content_len;

            sg_object_hash(SG_OBJ_BLOB, content0, len0, id0);
            if (sg_pack_read(git_dir, id0, &type, &content, &content_len) != 0) {
                dump_and_report("index_existing", "valid pack indexed but object0 unreadable back",
                               seed, b.data, b.len);
            } else {
                if (content_len != len0 || memcmp(content, content0, len0) != 0)
                    dump_and_report("index_existing", "valid pack round-trip content mismatch",
                                   seed, b.data, b.len);
                free(content);
            }
        }
    }
    /* mode 1/2: rc == -1 is the expected, healthy outcome. rc == 0 is not
       automatically a bug (some mutations are semantically harmless, e.g.
       trailing-garbage-then-truncated-back-off edge cases), but the
       resulting content must still be sane -- guards against a future
       regression shaped like the known expected_len-truncation bug, where
       a malformed size field could make content_len wildly exceed what was
       actually decompressed. */
    else if (rc == 0) {
        unsigned char id0[SG_SHA1_RAW_LEN];
        sg_obj_type type;
        unsigned char *content;
        size_t content_len;

        sg_object_hash(SG_OBJ_BLOB, content0, len0, id0);
        if (sg_pack_read(git_dir, id0, &type, &content, &content_len) == 0) {
            if (content_len > 1000000)
                dump_and_report("index_existing", "accepted mutant produced implausible content_len",
                               seed, b.data, b.len);
            free(content);
        }
    }

    bb_free(&b);
}

/* ==================================================================== *
 * Layer 3: sg_pack_read -- .idx + .pack query path
 * ==================================================================== */

static int find_pack_files(const char *git_dir, char *pack_path, size_t pack_path_sz,
                           char *idx_path, size_t idx_path_sz)
{
    char pack_dir[4096];
    DIR *d;
    struct dirent *de;
    int found = -1;

    snprintf(pack_dir, sizeof(pack_dir), "%s/objects/pack", git_dir);
    d = opendir(pack_dir);
    if (d == NULL)
        return -1;
    while ((de = readdir(d)) != NULL) {
        size_t l = strlen(de->d_name);

        if (l > 4 && strcmp(de->d_name + l - 4, ".idx") == 0) {
            snprintf(idx_path, idx_path_sz, "%s/%s", pack_dir, de->d_name);
            snprintf(pack_path, pack_path_sz, "%s/%.*s.pack", pack_dir, (int)(l - 4), de->d_name);
            found = 0;
            break;
        }
    }
    closedir(d);
    return found;
}

static int read_whole_file(const char *path, unsigned char **out, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    long len;
    unsigned char *buf;

    if (f == NULL)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    buf = malloc((size_t)len > 0 ? (size_t)len : 1);
    if (buf == NULL) {
        fclose(f);
        return -1;
    }
    if (len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    *out = buf;
    *out_len = (size_t)len;
    return 0;
}

/* In-place mutation of a whole file's bytes (pack or idx), then -- if
   enough bytes remain -- refreshes the trailing 20-byte self-checksum so
   the mutated file still "looks like" something that was legitimately
   produced and then bit-rotted, per the task's "recompute each file's own
   trailer" instruction. Neither sg_pack_read's pack decoding (no whole-pack trailer
   check on that path) nor idx_parse (never reads its own trailing
   checksum) actually gate on this, but keeping it internally consistent
   makes for a more realistic mutant and costs nothing. */
static void mutate_and_refix_trailer(unsigned char **data, size_t *len)
{
    unsigned int strategy = rand_below(4);

    switch (strategy) {
    case 0: { /* scattered bit flips */
        unsigned int flips = 1 + rand_below(6);
        unsigned int k;

        for (k = 0; k < flips; k++) {
            if (*len > 0) {
                size_t off = rand_below((uint32_t)*len);

                (*data)[off] ^= (unsigned char)(1u << rand_below(8));
            }
        }
        break;
    }
    case 1: /* truncate */
        if (*len > 8)
            *len = 8 + rand_below((uint32_t)(*len - 8));
        break;
    case 2: { /* extend with garbage */
        size_t extra = 1 + rand_below(64);
        unsigned char *grown = realloc(*data, *len + extra);
        size_t k;

        if (grown != NULL) {
            *data = grown;
            for (k = 0; k < extra; k++)
                (*data)[*len + k] = rand_byte();
            *len += extra;
        }
        break;
    }
    default: { /* overwrite a run with a fixed byte */
        if (*len > 0) {
            size_t off = rand_below((uint32_t)*len);
            size_t run = 1 + rand_below(16);
            size_t k;

            for (k = 0; k < run && off + k < *len; k++)
                (*data)[off + k] = 0xFF;
        }
        break;
    }
    }

    if (*len >= SG_SHA1_RAW_LEN) {
        unsigned char trailer[SG_SHA1_RAW_LEN];

        sg_sha1(*data, *len - SG_SHA1_RAW_LEN, trailer);
        memcpy(*data + *len - SG_SHA1_RAW_LEN, trailer, SG_SHA1_RAW_LEN);
    }
}

/* Builds a real (pack,idx) pair via sg_loose_write + sg_pack_write, then
   corrupts the pack, the idx, or both on disk, and queries sg_pack_read.
   Every round gets its own fresh mkdtemp git_dir -- see the file header
   comment on why reusing one within the same wall-clock second would be
   unsafe with pack.c's mtime-keyed registry cache. */
static void fuzz_pack_read_round(uint64_t seed)
{
    char *git_dir;
    unsigned char id0[SG_SHA1_RAW_LEN], id1[SG_SHA1_RAW_LEN];
    unsigned char ids[2][SG_SHA1_RAW_LEN];
    char content0[40], content1[40];
    size_t len0, len1, i;
    char pack_path[4096], idx_path[4096];
    unsigned char *pack_bytes = NULL, *idx_bytes = NULL;
    size_t pack_len = 0, idx_len = 0;
    unsigned int target;
    char *repo_root;

    seed_prng(seed);
    len0 = 5 + rand_below(30);
    len1 = 5 + rand_below(30);
    for (i = 0; i < len0; i++)
        content0[i] = (char)('a' + rand_below(26));
    for (i = 0; i < len1; i++)
        content1[i] = (char)('A' + rand_below(26));

    git_dir = make_tmp_repo("read");
    repo_root = repo_root_of_git_dir(git_dir);

    if (sg_loose_write(git_dir, SG_OBJ_BLOB, content0, len0, id0) != 0 ||
       sg_loose_write(git_dir, SG_OBJ_BLOB, content1, len1, id1) != 0) {
        fprintf(stderr, "setup failed: sg_loose_write\n");
        exit(1);
    }
    memcpy(ids[0], id0, SG_SHA1_RAW_LEN);
    memcpy(ids[1], id1, SG_SHA1_RAW_LEN);
    if (sg_pack_write(git_dir, ids, 2) != 0) {
        fprintf(stderr, "setup failed: sg_pack_write\n");
        exit(1);
    }
    if (find_pack_files(git_dir, pack_path, sizeof(pack_path), idx_path, sizeof(idx_path)) != 0) {
        fprintf(stderr, "setup failed: no pack/idx produced\n");
        exit(1);
    }
    if (read_whole_file(pack_path, &pack_bytes, &pack_len) != 0 ||
       read_whole_file(idx_path, &idx_bytes, &idx_len) != 0) {
        fprintf(stderr, "setup failed: cannot re-read pack/idx\n");
        exit(1);
    }

    target = rand_below(3); /* 0 = pack only, 1 = idx only, 2 = both */
    if (target != 1)
        mutate_and_refix_trailer(&pack_bytes, &pack_len);
    if (target != 0)
        mutate_and_refix_trailer(&idx_bytes, &idx_len);

    write_whole_file(pack_path, pack_bytes, pack_len);
    write_whole_file(idx_path, idx_bytes, idx_len);

    {
        sg_obj_type type;
        unsigned char *content;
        size_t content_len;
        int rc = sg_pack_read(git_dir, id0, &type, &content, &content_len);

        if (rc == 0) {
            if (content_len > 1000000)
                dump_and_report("read_pack", "mutant produced implausible content_len", seed,
                               pack_bytes, pack_len);
            free(content);
        }
    }
    {
        unsigned char bogus[SG_SHA1_RAW_LEN];
        sg_obj_type type;
        unsigned char *content;
        size_t content_len;

        for (i = 0; i < SG_SHA1_RAW_LEN; i++)
            bogus[i] = rand_byte();
        if (sg_pack_read(git_dir, bogus, &type, &content, &content_len) == 0)
            free(content); /* not a failure by itself: a colliding id is astronomically
                              unlikely, but nothing here should crash regardless */
    }

    free(pack_bytes);
    free(idx_bytes);
    free(git_dir);
    rm_rf(repo_root);
    free(repo_root);
}

/* Demonstrates pack.c's `strm.avail_out = (uInt)expected_len;` truncation
   directly (the fourth ground-truth bug): a blob entry whose declared
   decompressed size is 2^32 + 5 (5 real bytes past the 32-bit wraparound
   point), compressed from just 5 real bytes of content. `(uInt)(2^32 + 5)`
   truncates to exactly 5, so zlib is told to expect only 5 bytes of output
   -- which is exactly how many the real compressed stream produces -- so a
   single-shot, non-chunked pack_inflate reports success. Everything
   downstream then trusts the untruncated 64-bit size
   (metas[i].decompressed_size = (size_t)size in sg_pack_index_existing, or
   `size` in read_entry_at) as the object's real length: a ~4 GiB buffer
   gets allocated and handed back as this object's content, with only the
   first 5 bytes ever actually written to.

   The lying entry alone is only a few dozen compressed bytes, which is far
   too little `avail` (compressed bytes available to inflate from, per the
   entry's own header) to clear decompressed_size_is_plausible's ratio gate
   against a 2^32+5 declared size at SG_PACK_MAX_INFLATE_RATIO (pack.c) --
   it would be rejected before ever reaching pack_inflate, which defeats the
   point of this test (regressing this specific bug requires actually
   exercising pack_inflate's chunking, not just the unrelated ratio check
   added afterward). So a second, ordinary literal entry with large,
   genuinely-declared, close-to-incompressible content is appended right
   after the lying one: it pads out `avail` (which extends from the lying
   entry's compressed-data start all the way to the end of the pack file,
   covering every byte after it) past the ratio gate's threshold, without
   affecting the lying entry's own tiny compressed stream at all. The filler
   entry's own declared size stays truthful, so it doesn't trip the ratio
   check on itself as long as its content resists compression (see
   filler_len's comment below for the actual arithmetic).

   This requires two transient ~4 GiB allocations (one in
   sg_pack_index_existing's metadata pass, one in its resolve pass) and
   hashing ~4 GiB of mostly-uninitialized memory, which is a bad default
   for `make test`/CI (slow, and risky on memory-constrained runners or
   under ASan's redzone overhead) -- so unlike every other check in this
   file, it does NOT run by default. Opt in with SG_FUZZ_BIG=1. */
static void test_expected_len_truncation_bug(void)
{
    static const unsigned char real_content[] = "HELLO";
    const size_t real_len = sizeof(real_content) - 1;
    const uint64_t lying_size = ((uint64_t)1 << 32) + real_len;
    /* SG_PACK_MAX_INFLATE_RATIO is 1032*4 = 4128 after this task's widening
       (see pack.c). The lying entry's `avail` -- everything in the pack file
       from its compressed-data start through the filler entry and the
       trailer -- must clear lying_size / 4128 (~1,040,301 bytes) for
       decompressed_size_is_plausible to let it through to pack_inflate at
       all. filler_len is chosen with a comfortable margin above that: at
       close to 1:1 compression (see below) the filler's compressed bytes
       alone already exceed the threshold, before even counting the trailer
       or the lying entry's own header/data. */
    const size_t filler_len = 1300000;
    unsigned char *filler;
    unsigned char filler_id[SG_SHA1_RAW_LEN];
    bytebuf b;
    unsigned char hdr[16];
    size_t hdr_len;
    unsigned char *compressed;
    size_t compressed_len;
    unsigned char trailer[SG_SHA1_RAW_LEN];
    char *git_dir;
    char *repo_root;
    char pack_dir[4096];
    char pack_path[4096];
    int rc;
    size_t k;

    if (sg_compress(real_content, real_len, &compressed, &compressed_len) != 0) {
        fprintf(stderr, "setup failed: sg_compress\n");
        exit(1);
    }
    hdr_len = sg_pack_encode_obj_header(TT_BLOB, lying_size, hdr);

    bb_init(&b);
    bb_bytes(&b, "PACK", 4);
    bb_be32(&b, 2);
    bb_be32(&b, 2);
    bb_bytes(&b, hdr, hdr_len);
    bb_bytes(&b, compressed, compressed_len);
    free(compressed);

    /* Filler content: deterministic pseudo-random bytes, chosen precisely
       so deflate can't meaningfully compress them -- if it could, the
       filler's own declared-size-vs-avail ratio might itself approach
       SG_PACK_MAX_INFLATE_RATIO and get rejected, which would defeat this
       entry's only purpose (padding `avail` for the *lying* entry). */
    filler = malloc(filler_len);
    if (filler == NULL) {
        fprintf(stderr, "setup failed: out of memory allocating filler content\n");
        exit(1);
    }
    seed_prng(0xF117E2ULL);
    for (k = 0; k < filler_len; k++)
        filler[k] = rand_byte();
    sg_object_hash(SG_OBJ_BLOB, filler, filler_len, filler_id);
    append_literal_entry(&b, TT_BLOB, filler, filler_len);
    free(filler);

    sg_sha1(b.data, b.len, trailer);
    bb_bytes(&b, trailer, SG_SHA1_RAW_LEN);

    git_dir = make_tmp_repo("bigbug");
    repo_root = repo_root_of_git_dir(git_dir);
    snprintf(pack_dir, sizeof(pack_dir), "%s/objects/pack", git_dir);
    mkdir(pack_dir, 0755);
    snprintf(pack_path, sizeof(pack_path), "%s/bigbug.pack", pack_dir);
    write_whole_file(pack_path, b.data, b.len);

    fprintf(stderr, "SG_FUZZ_BIG=1: attempting a ~4 GiB expected_len-truncation repro, "
                   "this may take a while and use significant memory...\n");
    rc = sg_pack_index_existing(pack_path);
    if (rc != 0) {
        fprintf(stderr, "test_expected_len_truncation_bug: sg_pack_index_existing rejected the "
                       "crafted pack (rc=%d) -- the truncation did not manifest as an accepted "
                       "object on this build/platform; not a failure by itself\n",
               rc);
    } else {
        /* Two objects now, sorted by id in the .idx -- can't assume which
           slot is which. Read both ids out of the sha1 table (header(8) +
           fanout(256*4) = 1032 bytes in, 20 bytes each) and pick whichever
           one does NOT match the filler's own (independently computable)
           id; that's the lying entry's id. Don't try to guess the lying
           entry's id directly: sg_object_hash(type, content, content_len)
           is computed by pack.c over whatever it decided content_len was --
           the whole point under test -- so if the bug is present that's the
           huge untruncated size over mostly-uninitialized memory, not
           real_len over "HELLO", and there is no way to predict that hash
           from here. */
        char idx_path[4096];
        FILE *f;
        unsigned char ids[2][SG_SHA1_RAW_LEN];
        unsigned char lying_id[SG_SHA1_RAW_LEN];
        int have_lying_id = 0;

        snprintf(idx_path, sizeof(idx_path), "%s/bigbug.idx", pack_dir);
        f = fopen(idx_path, "rb");
        CHECK(f != NULL, "expected sg_pack_index_existing to have written %s", idx_path);
        if (f != NULL) {
            CHECK(fseek(f, 8 + 256 * 4, SEEK_SET) == 0 &&
                     fread(ids, 1, sizeof(ids), f) == sizeof(ids),
                 "failed to read both object ids out of %s", idx_path);
            fclose(f);

            if (memcmp(ids[0], filler_id, SG_SHA1_RAW_LEN) != 0) {
                memcpy(lying_id, ids[0], SG_SHA1_RAW_LEN);
                have_lying_id = 1;
            } else if (memcmp(ids[1], filler_id, SG_SHA1_RAW_LEN) != 0) {
                memcpy(lying_id, ids[1], SG_SHA1_RAW_LEN);
                have_lying_id = 1;
            } else {
                CHECK(0, "neither id in %s differs from the filler entry's own id -- can't "
                        "identify the lying entry",
                     idx_path);
            }

            if (have_lying_id) {
                sg_obj_type type;
                unsigned char *content;
                size_t content_len;

                if (sg_pack_read(git_dir, lying_id, &type, &content, &content_len) == 0) {
                    CHECK(content_len == real_len,
                         "expected_len truncation bug reproduced: sg_pack_read reported "
                         "content_len=%zu for an object whose real decompressed content is "
                         "%zu bytes (declared pack size %llu truncated to %u by a non-chunked "
                         "pack_inflate's `(uInt)expected_len`)",
                         content_len, real_len, (unsigned long long)lying_size,
                         (unsigned int)lying_size);
                    free(content);
                } else {
                    CHECK(0, "pack indexed the crafted object but sg_pack_read couldn't read it "
                            "back by the id its own .idx recorded");
                }
            }
        }
    }

    bb_free(&b);
    free(git_dir);
    rm_rf(repo_root);
    free(repo_root);
}

/* Deterministic regression for decompressed_size_is_plausible (pack.c): a
   pack entry that declares an absurd decompressed size (5 billion bytes)
   backed by only a handful of real compressed bytes. Without this check,
   sg_pack_index_existing would proceed straight to a malloc() sized off
   that declared size -- exactly the AddressSanitizer-abort scenario
   documented on SG_PACK_MAX_INFLATE_RATIO in pack.c. No fuzz round above
   reaches this deterministically -- corrupt_pack's mutation strategies never
   touch the declared-size field at all -- so nothing here would notice a
   future regression that turned this check into a no-op. */
static void test_implausible_declared_size_rejected(void)
{
    static const unsigned char tiny_content[] = "hi";
    const size_t tiny_len = sizeof(tiny_content) - 1;
    const uint64_t absurd_size = (uint64_t)5000000000ULL;
    bytebuf b;
    unsigned char hdr[16];
    size_t hdr_len;
    unsigned char *compressed;
    size_t compressed_len;
    unsigned char trailer[SG_SHA1_RAW_LEN];
    char *git_dir;
    char *repo_root;
    char pack_dir[4096];
    char pack_path[4096];
    int rc;

    if (sg_compress(tiny_content, tiny_len, &compressed, &compressed_len) != 0) {
        fprintf(stderr, "setup failed: sg_compress\n");
        exit(1);
    }
    hdr_len = sg_pack_encode_obj_header(TT_BLOB, absurd_size, hdr);

    bb_init(&b);
    bb_bytes(&b, "PACK", 4);
    bb_be32(&b, 2);
    bb_be32(&b, 1);
    bb_bytes(&b, hdr, hdr_len);
    bb_bytes(&b, compressed, compressed_len);
    free(compressed);
    sg_sha1(b.data, b.len, trailer);
    bb_bytes(&b, trailer, SG_SHA1_RAW_LEN);

    git_dir = make_tmp_repo("implausible");
    repo_root = repo_root_of_git_dir(git_dir);
    snprintf(pack_dir, sizeof(pack_dir), "%s/objects/pack", git_dir);
    mkdir(pack_dir, 0755);
    snprintf(pack_path, sizeof(pack_path), "%s/implausible.pack", pack_dir);
    write_whole_file(pack_path, b.data, b.len);

    rc = sg_pack_index_existing(pack_path);
    CHECK(rc != 0,
         "expected sg_pack_index_existing to reject a declared decompressed size of %llu "
         "backed by only %zu compressed bytes (got rc=0, accepted)",
         (unsigned long long)absurd_size, compressed_len);

    bb_free(&b);
    free(git_dir);
    rm_rf(repo_root);
    free(repo_root);
}

/* Deterministic regression for pack_inflate's expected_len == 0 escape hatch
   (pack.c): a pack entry that declares decompressed size 0 while its
   compressed stream actually holds real data ("HELLO"). This case's failure
   mode, if the escape check regressed, is not a wrong return value -- it's
   an infinite loop (avail_out stuck at 0 forever, never refilled, see the
   comment on that check in pack.c). A plain CHECK() around a direct call
   would just hang forever alongside the rest of the suite, turning a real
   regression into an unobservable stall instead of a red build. So the
   risky call runs in a forked child with its own wall-clock timeout: a hang
   becomes a killed child and an observable FAIL instead of a stuck test
   binary. */
static void test_zero_declared_size_with_real_data(void)
{
    static const unsigned char real_content[] = "HELLO";
    const size_t real_len = sizeof(real_content) - 1;
    bytebuf b;
    unsigned char hdr[16];
    size_t hdr_len;
    unsigned char *compressed;
    size_t compressed_len;
    unsigned char trailer[SG_SHA1_RAW_LEN];
    char *git_dir;
    char *repo_root;
    char pack_dir[4096];
    char pack_path[4096];
    pid_t pid;
    int status;

    if (sg_compress(real_content, real_len, &compressed, &compressed_len) != 0) {
        fprintf(stderr, "setup failed: sg_compress\n");
        exit(1);
    }
    hdr_len = sg_pack_encode_obj_header(TT_BLOB, 0, hdr);

    bb_init(&b);
    bb_bytes(&b, "PACK", 4);
    bb_be32(&b, 2);
    bb_be32(&b, 1);
    bb_bytes(&b, hdr, hdr_len);
    bb_bytes(&b, compressed, compressed_len);
    free(compressed);
    sg_sha1(b.data, b.len, trailer);
    bb_bytes(&b, trailer, SG_SHA1_RAW_LEN);

    git_dir = make_tmp_repo("zerosize");
    repo_root = repo_root_of_git_dir(git_dir);
    snprintf(pack_dir, sizeof(pack_dir), "%s/objects/pack", git_dir);
    mkdir(pack_dir, 0755);
    snprintf(pack_path, sizeof(pack_path), "%s/zerosize.pack", pack_dir);
    write_whole_file(pack_path, b.data, b.len);

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "setup failed: fork\n");
        exit(1);
    }
    if (pid == 0) {
        int rc;

        /* fork() does NOT inherit the parent's pending alarm() countdown
           (verified empirically: a child immediately after fork() reports
           alarm(0) == 0 regardless of what the parent had armed), but it
           DOES inherit the parent's SIGALRM handler *disposition* -- and
           main() below installs a whole-binary watchdog on SIGALRM. Reset
           to the default disposition before arming this call's own 10s
           timeout, so a hang here again terminates the child by signal
           (caught below via WIFSIGNALED) instead of exiting through the
           parent's watchdog handler with a less specific message. */
        signal(SIGALRM, SIG_DFL);
        alarm(10);
        rc = sg_pack_index_existing(pack_path);
        _exit(rc != 0 ? 0 : 1);
    }

    if (waitpid(pid, &status, 0) != pid) {
        CHECK(0, "waitpid failed for the zero-declared-size child process");
    } else if (WIFSIGNALED(status)) {
        CHECK(0, "sg_pack_index_existing appears to have hung (child killed by signal %d, "
                "likely SIGALRM after a 10s timeout) parsing a pack entry that declares "
                "decompressed size 0 while real zlib data is present -- expected a clean "
                "rejection instead",
             WTERMSIG(status));
    } else if (WIFEXITED(status)) {
        CHECK(WEXITSTATUS(status) == 0,
             "expected sg_pack_index_existing to reject a pack entry that declares "
             "decompressed size 0 while real zlib data is present (child reported acceptance)");
    } else {
        CHECK(0, "zero-declared-size child process ended abnormally (status=%d)", status);
    }

    bb_free(&b);
    free(git_dir);
    rm_rf(repo_root);
    free(repo_root);
}

/* ---- whole-binary watchdog ----
   test_zero_declared_size_with_real_data already forks its one specifically
   risky call under its own 10s alarm (see the comment on that function), but
   that only covers that single call site. This project's ground-truth bugs
   list includes a real hang (pack_inflate's expected_len==0 escape hatch
   removed, or its progress guard removed -- either alone spins forever), and
   nothing guarantees the next regression's hang is confined to that one
   already-protected call. CI's job-level timeout is 6 hours (see
   .github/workflows/ci.yml); without a whole-binary bound a hang anywhere
   else in this file would burn that entire budget before turning the build
   red. alarm()+SIGALRM here catches a hang at any call site, not just the
   one already wrapped in fork()+alarm(10).

   The handler only calls write() (async-signal-safe) on a buffer formatted
   *before* the alarm was armed, then _exit() -- never fprintf/snprintf from
   inside the handler itself, since those aren't guaranteed async-signal-safe.

   Default timeout: measured on a 2023-class laptop (clang, no sanitizer,
   this repo's default `make` build), `SG_FUZZ_ITERS=20000
   build/tests/test_fuzz_pack` (the exact invocation CI's fuzz-parse job
   uses) completes in ~31 seconds real time. 600s leaves well over an order
   of magnitude of headroom for slower/loaded CI runners or ASan/UBSan
   instrumentation overhead, while staying far short of the 6-hour job
   timeout. Override with SG_FUZZ_TIMEOUT (seconds) for a deliberately
   slower/bigger local run (e.g. alongside SG_FUZZ_BIG=1). */
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
    long iters = env_long("SG_FUZZ_ITERS", 400);
    long seed_base = env_long("SG_FUZZ_SEED_BASE", 0);
    long watchdog_timeout = env_long("SG_FUZZ_TIMEOUT", 600);
    long fuzz_big = env_long("SG_FUZZ_BIG", 0);
    long i;
    int n;
    char *idx_existing_git_dir;
    char *idx_existing_repo_root;
    char pack_path[4096];

    n = snprintf(g_watchdog_msg, sizeof(g_watchdog_msg),
                "sg: test_fuzz_pack watchdog fired after %ld second(s) with no completion "
                "(override via SG_FUZZ_TIMEOUT) -- SG_FUZZ_ITERS=%ld SG_FUZZ_SEED_BASE=%ld "
                "SG_FUZZ_BIG=%ld; this almost certainly means something under test hung "
                "somewhere in this run (e.g. pack_inflate spinning), not that the machine is "
                "merely slow; aborting\n",
                watchdog_timeout, iters, seed_base, fuzz_big);
    g_watchdog_msg_len =
        (n > 0 && (size_t)n < sizeof(g_watchdog_msg)) ? (size_t)n : sizeof(g_watchdog_msg) - 1;
    signal(SIGALRM, watchdog_fire);
    alarm((unsigned int)(watchdog_timeout > 0 ? watchdog_timeout : 0));

    test_obj_header_boundary_roundtrip();

    for (i = 0; i < iters * 5; i++)
        fuzz_decode_functions_round((uint64_t)(seed_base + i));
    for (i = 0; i < iters * 5; i++)
        fuzz_delta_apply_round((uint64_t)(seed_base + i));

    idx_existing_git_dir = make_tmp_repo("idxexisting");
    idx_existing_repo_root = repo_root_of_git_dir(idx_existing_git_dir);
    {
        char pack_dir[4096];

        snprintf(pack_dir, sizeof(pack_dir), "%s/objects/pack", idx_existing_git_dir);
        mkdir(pack_dir, 0755);
        snprintf(pack_path, sizeof(pack_path), "%s/fuzz.pack", pack_dir);
    }
    for (i = 0; i < iters; i++)
        fuzz_pack_index_existing_round((uint64_t)(seed_base + i), idx_existing_git_dir, pack_path);
    free(idx_existing_git_dir);
    rm_rf(idx_existing_repo_root);
    free(idx_existing_repo_root);

    for (i = 0; i < (iters / 4 > 20 ? iters / 4 : 20); i++)
        fuzz_pack_read_round((uint64_t)(seed_base + i));

    test_implausible_declared_size_rejected();
    test_zero_declared_size_with_real_data();

    if (fuzz_big != 0)
        test_expected_len_truncation_bug();

    alarm(0);

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all pack fuzz checks passed (%ld base iterations, seed_base %ld)\n", iters, seed_base);
    return 0;
}
