#include "sg/hash.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/pack.h"
#include "sg/repo.h"
#include "sg/workdir.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static char *make_tmp_repo(void)
{
    static char template[] = "/tmp/sg_oid_prefix_test_XXXXXX";
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

static void write_blob(const char *git_dir, const char *content, unsigned char id_out[SG_SHA1_RAW_LEN])
{
    if (sg_loose_write(git_dir, SG_OBJ_BLOB, content, strlen(content), id_out) != 0) {
        fprintf(stderr, "setup failed: sg_loose_write\n");
        exit(1);
    }
}

/* uppercases every hex digit in-place, for the case-insensitivity check */
static void hex_upper(char *hex)
{
    for (; *hex; hex++)
        *hex = (char)toupper((unsigned char)*hex);
}

/* mixed-case: alternates lower/upper starting from lower */
static void hex_mixed(char *hex)
{
    int i;

    for (i = 0; hex[i]; i++)
        hex[i] = (char)((i % 2 == 0) ? tolower((unsigned char)hex[i]) : toupper((unsigned char)hex[i]));
}

/* Byte-for-byte copies src to dst via plain fopen/fread/fwrite -- no sg API
   involved, used to move a pack file between two repos the way an external
   `git gc` would drop one into place. Returns 0 on success, -1 on any I/O
   failure. */
static int copy_file(const char *src, const char *dst)
{
    FILE *in, *out;
    char buf[65536];
    size_t n;
    int ok = 1;

    in = fopen(src, "rb");
    if (in == NULL)
        return -1;
    out = fopen(dst, "wb");
    if (out == NULL) {
        fclose(in);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
    }
    if (ferror(in))
        ok = 0;
    fclose(in);
    if (fclose(out) != 0)
        ok = 0;
    return ok ? 0 : -1;
}

/* Counts *.pack files directly under git_dir/objects/pack/ -- used to
   verify a fixture actually landed two objects in a SINGLE pack, rather
   than assuming sg_pack_write's behavior. */
static int count_pack_files(const char *git_dir)
{
    char pack_dir[4096];
    DIR *d;
    struct dirent *e;
    int n = 0;

    snprintf(pack_dir, sizeof(pack_dir), "%s/objects/pack", git_dir);
    d = opendir(pack_dir);
    if (d == NULL)
        return 0;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);

        if (len > 5 && strcmp(e->d_name + len - 5, ".pack") == 0)
            n++;
    }
    closedir(d);
    return n;
}

static int list_contains(const sg_oid_list *l, const unsigned char id[SG_SHA1_RAW_LEN])
{
    size_t i;

    for (i = 0; i < l->count; i++) {
        if (memcmp(l->ids[i], id, SG_SHA1_RAW_LEN) == 0)
            return 1;
    }
    return 0;
}

/* Asserts *l is sorted ascending by raw id and contains no adjacent
   duplicate -- the two properties sg_object_find_prefix's header comment
   promises. */
static void check_sorted_and_unique(const sg_oid_list *l, const char *ctx)
{
    size_t i;

    for (i = 1; i < l->count; i++) {
        int cmp = memcmp(l->ids[i - 1], l->ids[i], SG_SHA1_RAW_LEN);

        CHECK(cmp < 0, "%s: ids[%zu] does not sort strictly before ids[%zu] (cmp=%d)", ctx, i - 1, i,
             cmp);
    }
}

/* 4-hex, 7-hex, 39-hex prefixes (even and odd lengths both covered) of a
   single known object all resolve to exactly that object, given a handful
   of unrelated blobs are also present in the store. */
static void test_basic_prefix_lengths(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char target[SG_SHA1_RAW_LEN];
    unsigned char other[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];
    size_t lens[3] = {4, 7, 39};
    size_t i;

    write_blob(git_dir, "phase68-basic-target", target);
    write_blob(git_dir, "phase68-basic-other-1", other);
    write_blob(git_dir, "phase68-basic-other-2", other);
    write_blob(git_dir, "phase68-basic-other-3", other);

    sg_sha1_to_hex(target, hex);

    for (i = 0; i < 3; i++) {
        char prefix[SG_SHA1_HEX_LEN + 1];
        sg_oid_list out;

        memcpy(prefix, hex, lens[i]);
        prefix[lens[i]] = '\0';

        CHECK(sg_object_find_prefix(git_dir, prefix, &out) == 0, "find_prefix(%s) should succeed",
             prefix);
        CHECK(list_contains(&out, target), "find_prefix(%s) should contain the target id", prefix);
        check_sorted_and_unique(&out, prefix);
        sg_oid_list_free(&out);
    }

    free(git_dir);
}

/* The same prefix, spelled lowercase / uppercase / mixed-case, must resolve
   to the identical answer. */
static void test_case_insensitivity(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char target[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];
    char lower[8], upper[8], mixed[8];
    sg_oid_list out_lower, out_upper, out_mixed;

    write_blob(git_dir, "phase68-case-target", target);
    sg_sha1_to_hex(target, hex);

    memcpy(lower, hex, 7);
    lower[7] = '\0';
    memcpy(upper, lower, sizeof(lower));
    hex_upper(upper);
    memcpy(mixed, lower, sizeof(lower));
    hex_mixed(mixed);

    CHECK(sg_object_find_prefix(git_dir, lower, &out_lower) == 0, "lowercase prefix should succeed");
    CHECK(sg_object_find_prefix(git_dir, upper, &out_upper) == 0, "uppercase prefix should succeed");
    CHECK(sg_object_find_prefix(git_dir, mixed, &out_mixed) == 0, "mixed-case prefix should succeed");

    CHECK(out_lower.count == out_upper.count && out_lower.count == out_mixed.count,
         "case must not change the candidate count: lower=%zu upper=%zu mixed=%zu", out_lower.count,
         out_upper.count, out_mixed.count);
    CHECK(list_contains(&out_lower, target), "lowercase result should contain target");
    CHECK(list_contains(&out_upper, target), "uppercase result should contain target");
    CHECK(list_contains(&out_mixed, target), "mixed-case result should contain target");

    sg_oid_list_free(&out_lower);
    sg_oid_list_free(&out_upper);
    sg_oid_list_free(&out_mixed);
    free(git_dir);
}

/* A prefix that no object begins with is a success with zero candidates. */
static void test_zero_candidates(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char ids[4][SG_SHA1_RAW_LEN];
    char prefix[5];
    int val;
    sg_oid_list out;

    write_blob(git_dir, "phase68-zero-1", ids[0]);
    write_blob(git_dir, "phase68-zero-2", ids[1]);
    write_blob(git_dir, "phase68-zero-3", ids[2]);
    write_blob(git_dir, "phase68-zero-4", ids[3]);

    /* find a 4-hex value none of the four ids begins with */
    for (val = 0; val < 0x10000; val++) {
        int i, hit = 0;

        snprintf(prefix, sizeof(prefix), "%04x", val);
        for (i = 0; i < 4; i++) {
            char h[SG_SHA1_HEX_LEN + 1];

            sg_sha1_to_hex(ids[i], h);
            if (memcmp(h, prefix, 4) == 0) {
                hit = 1;
                break;
            }
        }
        if (!hit)
            break;
    }
    CHECK(val < 0x10000, "setup failed: every 4-hex value collided (impossible with 4 objects)");

    CHECK(sg_object_find_prefix(git_dir, prefix, &out) == 0, "find_prefix(%s) should succeed",
         prefix);
    CHECK(out.count == 0, "find_prefix(%s) should have zero candidates, got %zu", prefix, out.count);

    sg_oid_list_free(&out);
    free(git_dir);
}

/* Two objects deliberately made to share a 4-hex prefix both come back --
   this is the "two or more candidates" case. Uses the pigeonhole principle
   (only 65536 possible 4-hex values) to *guarantee* termination: content is
   varied until some two distinct hashes land in the same bucket, which must
   happen within 65537 tries. */
static void test_multiple_candidates(void)
{
    char *git_dir = make_tmp_repo();
    static unsigned char bucket_id[65536][SG_SHA1_RAW_LEN];
    static int bucket_seen[65536];
    unsigned char id_a[SG_SHA1_RAW_LEN], id_b[SG_SHA1_RAW_LEN];
    char prefix[5];
    int i, found = 0;

    memset(bucket_seen, 0, sizeof(bucket_seen));

    for (i = 0; i < 70000 && !found; i++) {
        char content[64];
        unsigned char id[SG_SHA1_RAW_LEN];
        char hex[SG_SHA1_HEX_LEN + 1];
        char hex4[5];
        unsigned int val;

        snprintf(content, sizeof(content), "phase68-collide-%d", i);
        sg_object_hash(SG_OBJ_BLOB, content, strlen(content), id);
        sg_sha1_to_hex(id, hex);
        memcpy(hex4, hex, 4);
        hex4[4] = '\0';
        val = (unsigned int)strtoul(hex4, NULL, 16);

        if (bucket_seen[val]) {
            if (memcmp(bucket_id[val], id, SG_SHA1_RAW_LEN) != 0) {
                memcpy(id_a, bucket_id[val], SG_SHA1_RAW_LEN);
                memcpy(id_b, id, SG_SHA1_RAW_LEN);
                memcpy(prefix, hex, 4);
                prefix[4] = '\0';
                found = 1;
            }
        } else {
            bucket_seen[val] = 1;
            memcpy(bucket_id[val], id, SG_SHA1_RAW_LEN);
        }
    }

    CHECK(found, "setup failed: no 4-hex collision found within 70000 tries (should be impossible)");

    if (found) {
        sg_oid_list out;
        char content_a[64], content_b[64];
        unsigned char written_a[SG_SHA1_RAW_LEN], written_b[SG_SHA1_RAW_LEN];
        int j;

        /* re-derive which content strings produced id_a/id_b by re-hashing;
           simplest: just re-run the same loop bodies is wasteful, so instead
           re-search 0..i for the two indices whose hash equals id_a/id_b */
        for (j = 0; j <= i; j++) {
            char c[64];
            unsigned char id[SG_SHA1_RAW_LEN];

            snprintf(c, sizeof(c), "phase68-collide-%d", j);
            sg_object_hash(SG_OBJ_BLOB, c, strlen(c), id);
            if (memcmp(id, id_a, SG_SHA1_RAW_LEN) == 0)
                memcpy(content_a, c, sizeof(content_a));
            if (memcmp(id, id_b, SG_SHA1_RAW_LEN) == 0)
                memcpy(content_b, c, sizeof(content_b));
        }

        write_blob(git_dir, content_a, written_a);
        write_blob(git_dir, content_b, written_b);
        CHECK(memcmp(written_a, id_a, SG_SHA1_RAW_LEN) == 0, "re-written content_a hash mismatch");
        CHECK(memcmp(written_b, id_b, SG_SHA1_RAW_LEN) == 0, "re-written content_b hash mismatch");

        CHECK(sg_object_find_prefix(git_dir, prefix, &out) == 0, "find_prefix(%s) should succeed",
             prefix);
        CHECK(out.count >= 2, "find_prefix(%s) should have >= 2 candidates, got %zu", prefix,
             out.count);
        CHECK(list_contains(&out, id_a), "result should contain id_a");
        CHECK(list_contains(&out, id_b), "result should contain id_b");
        check_sorted_and_unique(&out, prefix);

        sg_oid_list_free(&out);
    }

    free(git_dir);
}

/* ---- near-collision fixture: exercises the ODD-NIBBLE comparison branch ----

   Every test above only ever compares whole hex bytes (4/7/39-char prefixes
   are all even... except 7 and 39, which DO touch the odd branch, but never
   in a way that changes the ANSWER -- there is never a second object whose
   first n-1 hex chars match and whose n-th differs, so shaving off that last
   half-byte comparison is unobservable: fewer or more bits compared, the
   candidate set is identical either way. A directed mutation confirmed this
   is a real, observable blind spot: deleting the odd-nibble half of
   sg_sha1_has_prefix (loose.c's comparator) or of cmp_id_to_prefix (pack.c's
   comparator) left this whole file green.

   This fixture closes that gap by constructing two objects whose ids share
   their first 4 hex characters (2 full bytes) but differ at the 5th (the
   high nibble of the 3rd byte) -- the exact shape where "compare one nibble
   short" changes the answer: a 4-hex query must return BOTH, but a 5-hex
   query naming one of them must return EXACTLY that one, not both. */

#define SG_TEST_HEX4_BUCKETS 65536
#define SG_TEST_HEX4_BUCKET_CAP 4

struct near_collision_pair {
    unsigned char id_a[SG_SHA1_RAW_LEN];
    char content_a[64];
    unsigned char id_b[SG_SHA1_RAW_LEN];
    char content_b[64];
    char prefix4[5];
    char prefix5[6];
};

/* Brute-forces two blob contents whose ids share their first 4 hex chars
   but differ at the 5th, bucketing by the first 4 hex chars the same way
   test_multiple_candidates does.

   This is NOT the strict pigeonhole guarantee test_multiple_candidates has,
   and the difference is worth stating rather than glossing: there, one
   entry per bucket over 65536 buckets makes a collision certain by trial
   65537; here a pair only counts when the two entries in a bucket also
   DISAGREE at the 5th nibble, so a bucket could in principle fill its cap
   with entries that all agree there and stop contributing. In practice two
   same-bucket entries differ at that nibble with probability 15/16, so the
   birthday bound still finds a pair within a few thousand trials, far
   under this loop's hard 500000-trial ceiling -- which is why that
   ceiling exists and reports rather than looping forever. Measured: this
   search is not a visible cost in make test's runtime. */
static void find_near_collision(struct near_collision_pair *out)
{
    /* static: too large for the stack, and only needed for this one search */
    static unsigned char bucket_ids[SG_TEST_HEX4_BUCKETS][SG_TEST_HEX4_BUCKET_CAP][SG_SHA1_RAW_LEN];
    static int bucket_count[SG_TEST_HEX4_BUCKETS];
    int i, found = 0;
    unsigned char id_a[SG_SHA1_RAW_LEN], id_b[SG_SHA1_RAW_LEN];
    char hex_a[SG_SHA1_HEX_LEN + 1];
    int last_i = -1;

    memset(bucket_count, 0, sizeof(bucket_count));

    for (i = 0; i < 500000 && !found; i++) {
        char content[64];
        unsigned char id[SG_SHA1_RAW_LEN];
        char hex[SG_SHA1_HEX_LEN + 1];
        char hex4[5];
        unsigned int val;
        unsigned char nib5;
        int j;

        snprintf(content, sizeof(content), "phase68-near-%d", i);
        sg_object_hash(SG_OBJ_BLOB, content, strlen(content), id);
        sg_sha1_to_hex(id, hex);
        memcpy(hex4, hex, 4);
        hex4[4] = '\0';
        val = (unsigned int)strtoul(hex4, NULL, 16);
        nib5 = (unsigned char)(id[2] >> 4);

        for (j = 0; j < bucket_count[val]; j++) {
            if ((unsigned char)(bucket_ids[val][j][2] >> 4) != nib5) {
                memcpy(id_a, bucket_ids[val][j], SG_SHA1_RAW_LEN);
                memcpy(id_b, id, SG_SHA1_RAW_LEN);
                sg_sha1_to_hex(id_a, hex_a);
                found = 1;
                last_i = i;
                break;
            }
        }
        if (!found && bucket_count[val] < SG_TEST_HEX4_BUCKET_CAP) {
            memcpy(bucket_ids[val][bucket_count[val]], id, SG_SHA1_RAW_LEN);
            bucket_count[val]++;
        }
    }

    if (!found) {
        fprintf(stderr, "setup failed: no near-collision found within 500000 tries\n");
        exit(1);
    }

    /* re-derive which content strings produced id_a/id_b */
    for (i = 0; i <= last_i; i++) {
        char c[64];
        unsigned char id[SG_SHA1_RAW_LEN];

        snprintf(c, sizeof(c), "phase68-near-%d", i);
        sg_object_hash(SG_OBJ_BLOB, c, strlen(c), id);
        if (memcmp(id, id_a, SG_SHA1_RAW_LEN) == 0)
            memcpy(out->content_a, c, sizeof(out->content_a));
        if (memcmp(id, id_b, SG_SHA1_RAW_LEN) == 0)
            memcpy(out->content_b, c, sizeof(out->content_b));
    }

    memcpy(out->id_a, id_a, SG_SHA1_RAW_LEN);
    memcpy(out->id_b, id_b, SG_SHA1_RAW_LEN);
    memcpy(out->prefix4, hex_a, 4);
    out->prefix4[4] = '\0';
    memcpy(out->prefix5, hex_a, 5);
    out->prefix5[5] = '\0';
}

/* LOOSE side: both near-colliding objects are loose-only (never packed), so
   this can only be satisfied by sg_loose_find_prefix / sg_sha1_has_prefix --
   it must be able to go red on its own, without touching the pack path. */
static void test_near_collision_disambiguates_loose(void)
{
    struct near_collision_pair p;
    char *git_dir;
    unsigned char written_a[SG_SHA1_RAW_LEN], written_b[SG_SHA1_RAW_LEN];
    sg_oid_list out4, out5;

    find_near_collision(&p);
    git_dir = make_tmp_repo();

    write_blob(git_dir, p.content_a, written_a);
    write_blob(git_dir, p.content_b, written_b);
    CHECK(memcmp(written_a, p.id_a, SG_SHA1_RAW_LEN) == 0, "loose: content_a hash mismatch");
    CHECK(memcmp(written_b, p.id_b, SG_SHA1_RAW_LEN) == 0, "loose: content_b hash mismatch");

    CHECK(sg_object_find_prefix(git_dir, p.prefix4, &out4) == 0,
         "loose: find_prefix(%s) should succeed", p.prefix4);
    CHECK(out4.count == 2, "loose: 4-hex prefix %s should have exactly 2 candidates, got %zu",
         p.prefix4, out4.count);
    CHECK(list_contains(&out4, p.id_a) && list_contains(&out4, p.id_b),
         "loose: 4-hex prefix %s should contain both ids", p.prefix4);

    CHECK(sg_object_find_prefix(git_dir, p.prefix5, &out5) == 0,
         "loose: find_prefix(%s) should succeed", p.prefix5);
    CHECK(out5.count == 1, "loose: 5-hex prefix %s should have exactly 1 candidate, got %zu",
         p.prefix5, out5.count);
    if (out5.count == 1)
        CHECK(memcmp(out5.ids[0], p.id_a, SG_SHA1_RAW_LEN) == 0,
             "loose: 5-hex prefix %s should resolve to id_a specifically", p.prefix5);

    sg_oid_list_free(&out4);
    sg_oid_list_free(&out5);
    free(git_dir);
}

/* PACK side: identical shape, but both objects are pack-only (loose copies
   removed after packing, same technique as test_pack_only), so this can
   only be satisfied by sg_pack_find_prefix / cmp_id_to_prefix -- a broken
   loose comparator cannot hide behind it, and vice versa. */
static void test_near_collision_disambiguates_pack(void)
{
    struct near_collision_pair p;
    char *git_dir;
    unsigned char written_a[SG_SHA1_RAW_LEN], written_b[SG_SHA1_RAW_LEN];
    unsigned char pack_ids[2][SG_SHA1_RAW_LEN];
    char hex_a[SG_SHA1_HEX_LEN + 1], hex_b[SG_SHA1_HEX_LEN + 1];
    char loose_path[4096];
    sg_oid_list out4, out5;

    find_near_collision(&p);
    git_dir = make_tmp_repo();

    write_blob(git_dir, p.content_a, written_a);
    write_blob(git_dir, p.content_b, written_b);
    CHECK(memcmp(written_a, p.id_a, SG_SHA1_RAW_LEN) == 0, "pack: content_a hash mismatch");
    CHECK(memcmp(written_b, p.id_b, SG_SHA1_RAW_LEN) == 0, "pack: content_b hash mismatch");

    /* Deliberately ONE sg_pack_write call carrying BOTH ids, not two calls
       each writing its own pack: idx_find_prefix's forward-scan-while-
       matching loop (the code path that collects more than one candidate
       out of a single pack's sha1_table) is only exercised when two
       prefix-sharing objects sit in the SAME pack's idx. Two separate
       single-object packs would each answer "1 candidate" independently,
       silently reducing this test to test_pack_only run twice and never
       touching that loop at all. */
    memcpy(pack_ids[0], p.id_a, SG_SHA1_RAW_LEN);
    memcpy(pack_ids[1], p.id_b, SG_SHA1_RAW_LEN);
    CHECK(sg_pack_write(git_dir, pack_ids, 2) == 0,
         "pack: setup failed writing one pack containing both id_a and id_b");
    /* Fixture check, not an assumption: confirm there is exactly ONE pack
       file, so both near-colliding ids are genuinely in the same idx and
       the forward-scan-collects-multiple-matches loop is really exercised
       below (not just two independent single-object packs). */
    CHECK(count_pack_files(git_dir) == 1,
         "pack: fixture check failed -- expected exactly 1 pack file, got %d",
         count_pack_files(git_dir));

    sg_sha1_to_hex(p.id_a, hex_a);
    sg_sha1_to_hex(p.id_b, hex_b);
    snprintf(loose_path, sizeof(loose_path), "%s/objects/%.2s/%s", git_dir, hex_a, hex_a + 2);
    CHECK(unlink(loose_path) == 0, "pack: setup failed removing loose copy of id_a at %s",
         loose_path);
    snprintf(loose_path, sizeof(loose_path), "%s/objects/%.2s/%s", git_dir, hex_b, hex_b + 2);
    CHECK(unlink(loose_path) == 0, "pack: setup failed removing loose copy of id_b at %s",
         loose_path);

    CHECK(sg_object_find_prefix(git_dir, p.prefix4, &out4) == 0,
         "pack: find_prefix(%s) should succeed", p.prefix4);
    CHECK(out4.count == 2, "pack: 4-hex prefix %s should have exactly 2 candidates, got %zu",
         p.prefix4, out4.count);
    CHECK(list_contains(&out4, p.id_a) && list_contains(&out4, p.id_b),
         "pack: 4-hex prefix %s should contain both ids", p.prefix4);

    CHECK(sg_object_find_prefix(git_dir, p.prefix5, &out5) == 0,
         "pack: find_prefix(%s) should succeed", p.prefix5);
    CHECK(out5.count == 1, "pack: 5-hex prefix %s should have exactly 1 candidate, got %zu",
         p.prefix5, out5.count);
    if (out5.count == 1)
        CHECK(memcmp(out5.ids[0], p.id_a, SG_SHA1_RAW_LEN) == 0,
             "pack: 5-hex prefix %s should resolve to id_a specifically", p.prefix5);

    sg_oid_list_free(&out4);
    sg_oid_list_free(&out5);
    free(git_dir);
}

/* An object present as BOTH a loose file and inside a pack must be reported
   exactly once. Verifies the fixture itself actually lands in both places
   (via sg_loose_read / sg_pack_read directly) before trusting the dedup
   result -- this shape has no real-git oracle, so the fixture correctness
   matters more here than anywhere else in this file. */
static void test_dedup_loose_and_packed(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];
    char prefix[7];
    sg_obj_type type;
    unsigned char *content = NULL;
    size_t content_len;
    sg_oid_list out;

    write_blob(git_dir, "phase68-dedup-target", id);

    CHECK(sg_pack_write(git_dir, (const unsigned char (*)[SG_SHA1_RAW_LEN])id, 1) == 0,
         "setup failed: sg_pack_write");

    /* verify the fixture: the object must be readable from BOTH storage
       layers independently before the dedup claim means anything */
    CHECK(sg_loose_read(git_dir, id, &type, &content, &content_len) == 0,
         "fixture check failed: object not readable as loose after packing");
    if (content != NULL)
        free(content);
    CHECK(sg_pack_read(git_dir, id, &type, &content, &content_len) == 0,
         "fixture check failed: object not readable from the new pack");
    if (content != NULL)
        free(content);

    sg_sha1_to_hex(id, hex);
    memcpy(prefix, hex, 6);
    prefix[6] = '\0';

    CHECK(sg_object_find_prefix(git_dir, prefix, &out) == 0, "find_prefix(%s) should succeed",
         prefix);
    CHECK(out.count == 1, "a loose+packed object must be reported exactly once, got %zu",
         out.count);
    if (out.count >= 1)
        CHECK(memcmp(out.ids[0], id, SG_SHA1_RAW_LEN) == 0, "the one candidate must be the target id");

    sg_oid_list_free(&out);
    free(git_dir);
}

/* An object that exists ONLY inside a pack (loose copy removed after
   packing) is still found -- exercises sg_pack_find_prefix in isolation. */
static void test_pack_only(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];
    char loose_path[4096];
    char prefix[8];
    sg_obj_type type;
    unsigned char *content = NULL;
    size_t content_len;
    sg_oid_list out;

    write_blob(git_dir, "phase68-pack-only-target", id);
    CHECK(sg_pack_write(git_dir, (const unsigned char (*)[SG_SHA1_RAW_LEN])id, 1) == 0,
         "setup failed: sg_pack_write");

    sg_sha1_to_hex(id, hex);
    snprintf(loose_path, sizeof(loose_path), "%s/objects/%.2s/%s", git_dir, hex, hex + 2);
    CHECK(unlink(loose_path) == 0, "setup failed: could not remove loose copy at %s", loose_path);

    CHECK(sg_loose_read(git_dir, id, &type, &content, &content_len) != 0,
         "fixture check failed: loose copy should be gone");
    CHECK(sg_pack_read(git_dir, id, &type, &content, &content_len) == 0,
         "fixture check failed: pack copy should still be readable");
    if (content != NULL)
        free(content);

    memcpy(prefix, hex, 7);
    prefix[7] = '\0';

    CHECK(sg_object_find_prefix(git_dir, prefix, &out) == 0, "find_prefix(%s) should succeed",
         prefix);
    CHECK(out.count == 1, "pack-only object should be found exactly once, got %zu", out.count);
    if (out.count >= 1)
        CHECK(memcmp(out.ids[0], id, SG_SHA1_RAW_LEN) == 0, "the one candidate must be the target id");

    sg_oid_list_free(&out);
    free(git_dir);
}

/* An object that exists ONLY as a loose file (never packed) is found --
   exercises sg_loose_find_prefix in isolation. */
static void test_loose_only(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];
    char prefix[5];
    sg_oid_list out;

    write_blob(git_dir, "phase68-loose-only-target", id);
    sg_sha1_to_hex(id, hex);
    memcpy(prefix, hex, 4);
    prefix[4] = '\0';

    CHECK(sg_object_find_prefix(git_dir, prefix, &out) == 0, "find_prefix(%s) should succeed",
         prefix);
    CHECK(out.count >= 1, "loose-only object should be found");
    CHECK(list_contains(&out, id), "result should contain the loose-only target id");

    sg_oid_list_free(&out);
    free(git_dir);
}

/* The mmap'd pack registry (pack.c's g_pack_dirs) is cached per git_dir for
   the lifetime of the process. sg_pack_find_prefix must rescan it when
   objects/pack/ has changed since it was last scanned -- the same
   staleness rule sg_pack_read (pack_read_depth_inner) already applies on a
   miss -- or a pack that appeared after the first scan stays invisible.

   IMPORTANT: this must be an EXTERNAL-appearance fixture, not "write a pack
   into the SAME repo from this process after the first scan" --
   sg_pack_write itself calls pack_cache_invalidate(git_dir) on success,
   which resets that git_dir's own pd->scanned to 0 regardless of whether
   the staleness check under test is even present. An earlier draft of this
   test did exactly that (same repo, same process) and stayed green under
   the very mutation it was meant to catch, because sg_pack_write's own
   self-invalidation was quietly doing the rescanning instead of the mtime
   check.

   A LATER draft used fork() (repo A cached in the parent, packed by a
   child) to sidestep that -- pack_cache_invalidate is looked up by
   pack_dir_lookup(git_dir), so a child's call only ever touches the
   child's own copy-on-write memory, never the parent's. That worked
   (mutation-verified at the time) but broke this project's `--leaks`
   gate: `/usr/bin/leaks --atExit` is incompatible with a program that
   forks -- it hung the full 120s timeout with zero output for exactly this
   binary, while the binary itself runs in 0.5s normally. `--leaks` is not
   part of this project's completion criteria, but CLAUDE.md's own rule is
   that a gate must never be *structurally* red (see gates.sh's own
   comments) -- and CI's ASan job runs with detect_leaks=1, where a forked
   child's behavior under LeakSanitizer was unmeasured local risk on top of
   that. So: no fork here.

   This draft instead uses TWO REPOS and a raw file copy, mirroring what an
   external `git gc` actually does to a repo whose pack registry this
   process already cached: pack_cache_invalidate is keyed by git_dir, so
   packing the object into a SEPARATE repo B invalidates only B's own
   (nonexistent-until-now) registry entry, never A's; the resulting
   .pack/.idx are then moved into A's objects/pack/ via plain
   fopen/fread/fwrite (copy_file), never through any sg pack API, so
   nothing in A's cache is touched by the copy itself -- A's registry stays
   exactly as stale as it would after a real external `git gc`.

   The mtimes still can't accidentally agree regardless of clock
   resolution: A's one and only scan happens before A's objects/pack/
   exists at all (recorded as the sentinel (time_t)-1), and the copy is
   what creates it, so A's post-copy stat() is guaranteed != -1 -- no
   dependency on the wall clock ticking over a one-second boundary the way
   pack_read_depth_inner's own "racily clean" branch has. A never receives
   a loose copy of the object at all (it is written and packed entirely
   under B), which is confirmed explicitly below rather than assumed, so a
   stale pack registry under A cannot be masked by a loose hit instead. */
static void test_pack_registry_rescans_after_external_pack_write(void)
{
    static const char content[] = "phase68-pack-freshness-external-target";
    char *git_dir_a = make_tmp_repo();
    char *git_dir_b = make_tmp_repo();
    unsigned char id[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];
    char prefix[5];
    char pack_dir_a[4096], pack_dir_b[4096];
    DIR *d;
    struct dirent *e;
    int copied = 0;
    sg_obj_type type;
    unsigned char *content_out = NULL;
    size_t content_len;
    sg_oid_list out1, out2;

    /* First call on A: objects/pack/ does not exist under A yet, so this
       caches A's registry as "scanned, pack dir absent" -- in THIS
       process, well before B's pack is even built. */
    CHECK(sg_object_find_prefix(git_dir_a, "abcd", &out1) == 0,
         "first (pre-pack) find_prefix call on A should succeed");
    CHECK(out1.count == 0, "no packs exist under A yet, expected 0 candidates, got %zu",
         out1.count);
    sg_oid_list_free(&out1);

    /* Pack the object under a COMPLETELY SEPARATE repo B. */
    write_blob(git_dir_b, content, id);
    CHECK(sg_pack_write(git_dir_b, (const unsigned char (*)[SG_SHA1_RAW_LEN])id, 1) == 0,
         "setup failed: sg_pack_write on B");

    /* Move B's .pack/.idx into A's objects/pack/ via raw file copy only. */
    snprintf(pack_dir_a, sizeof(pack_dir_a), "%s/objects/pack", git_dir_a);
    snprintf(pack_dir_b, sizeof(pack_dir_b), "%s/objects/pack", git_dir_b);
    CHECK(mkdir(pack_dir_a, 0755) == 0 || errno == EEXIST, "setup failed: mkdir %s", pack_dir_a);

    d = opendir(pack_dir_b);
    CHECK(d != NULL, "setup failed: opendir %s", pack_dir_b);
    if (d != NULL) {
        while ((e = readdir(d)) != NULL) {
            size_t len = strlen(e->d_name);

            if (len > 5 && (strcmp(e->d_name + len - 5, ".pack") == 0 ||
                           strcmp(e->d_name + len - 4, ".idx") == 0)) {
                char src[4096], dst[4096];

                snprintf(src, sizeof(src), "%s/%s", pack_dir_b, e->d_name);
                snprintf(dst, sizeof(dst), "%s/%s", pack_dir_a, e->d_name);
                CHECK(copy_file(src, dst) == 0, "setup failed: copying %s to %s", src, dst);
                copied++;
            }
        }
        closedir(d);
    }
    CHECK(copied == 2, "setup failed: expected to copy exactly 2 files (.pack + .idx) from B "
         "to A, copied %d", copied);

    /* Fixture check, not an assumption: A must have no loose copy of the
       object at all, or a stale pack registry would be masked by the
       loose half finding it instead, and this test would pass for the
       wrong reason. */
    CHECK(sg_loose_read(git_dir_a, id, &type, &content_out, &content_len) != 0,
         "fixture check failed: A should have no loose copy of the object");
    if (content_out != NULL)
        free(content_out);

    sg_sha1_to_hex(id, hex);
    memcpy(prefix, hex, 4);
    prefix[4] = '\0';

    CHECK(sg_object_find_prefix(git_dir_a, prefix, &out2) == 0,
         "second find_prefix call on A (after an externally-built pack was copied in) should "
         "succeed");
    CHECK(list_contains(&out2, id),
         "a pack registry cached before an external pack appeared must rescan via the mtime "
         "check and find it -- a stale registry silently drops a candidate, which can turn a "
         "genuinely ambiguous prefix into a falsely unique one");

    sg_oid_list_free(&out2);
    free(git_dir_a);
    free(git_dir_b);
}

/* sg_loose_find_prefix's opendir() failure handling must fail CLOSED
   (-1) for anything other than ENOENT/ENOTDIR, never silently report "zero
   candidates" -- the loose half going unscanned can turn a genuinely
   ambiguous prefix into a falsely unique one (resolving to the wrong
   object), which is worse than "not found". Exercised by chmod'ing the
   target's own bucket directory to 000 so opendir() fails with EACCES.

   Two things make this check self-verifying rather than silently
   toothless on some machine: running as root bypasses permission checks
   entirely (common in CI containers), so euid is checked and the SKIP is
   printed loudly rather than the check just quietly passing; and even at a
   non-root euid, chmod 000 might not be honored by every filesystem/ACL
   combination, so the test itself opendir()s the bucket right after
   chmod'ing it and only proceeds if that probe actually failed. */
static void test_loose_opendir_failure_is_fatal(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];
    char bucket_dir[4096];
    char prefix[5];
    sg_oid_list out;
    DIR *probe;

    write_blob(git_dir, "phase68-loose-failopen-target", id);
    sg_sha1_to_hex(id, hex);
    snprintf(bucket_dir, sizeof(bucket_dir), "%s/objects/%.2s", git_dir, hex);
    memcpy(prefix, hex, 4);
    prefix[4] = '\0';

    if (geteuid() == 0) {
        fprintf(stderr,
               "SKIP test_loose_opendir_failure_is_fatal: running as root, chmod 000 does not "
               "deny access -- this check does NOT run on this machine\n");
        free(git_dir);
        return;
    }

    CHECK(chmod(bucket_dir, 0) == 0, "setup failed: chmod 000 on %s", bucket_dir);

    /* precondition: opendir must actually fail here, or the assertion below
       verifies nothing (some filesystem/ACL setup could make chmod 000
       toothless even at a non-root euid) */
    probe = opendir(bucket_dir);
    if (probe != NULL) {
        closedir(probe);
        chmod(bucket_dir, 0755);
        fprintf(stderr,
               "SKIP test_loose_opendir_failure_is_fatal: chmod 000 did not actually deny "
               "opendir() on this machine/filesystem -- this check does NOT run here\n");
        free(git_dir);
        return;
    }

    CHECK(sg_object_find_prefix(git_dir, prefix, &out) == -1,
         "an unreadable loose bucket directory must fail CLOSED (-1), not silently report "
         "zero or incomplete candidates");

    chmod(bucket_dir, 0755);
    free(git_dir);
}

/* Malformed inputs: too short, too long, or containing a non-hex byte. */
static void test_malformed_inputs(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char dummy[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];
    sg_oid_list out;

    write_blob(git_dir, "phase68-malformed-anchor", dummy);
    sg_sha1_to_hex(dummy, hex);

    /* too short: 3 hex chars, below SG_OID_MIN_ABBREV */
    CHECK(sg_object_find_prefix(git_dir, "abc", &out) == -1,
         "a 3-char prefix must be rejected (below the minimum of %d)", SG_OID_MIN_ABBREV);

    /* too long: 41 hex chars */
    {
        char too_long[42];

        memcpy(too_long, hex, 40);
        too_long[40] = hex[0];
        too_long[41] = '\0';
        CHECK(sg_object_find_prefix(git_dir, too_long, &out) == -1,
             "a 41-char prefix must be rejected");
    }

    /* exactly 40 hex chars: an EXACT id, not a prefix -- belongs to
       sg_object_read per spec 7.1, and must be rejected here even though
       it's a well-formed, existing, unambiguous id. Distinct from the
       41-char check above: a mutation widening the bound from "> 39" to
       "> 40" passes the 41-char check (41 is still > 40) but must fail
       this one, so this is the only check in this file that pins the
       upper boundary at exactly the right value. */
    {
        char exact[SG_SHA1_HEX_LEN + 1];

        memcpy(exact, hex, SG_SHA1_HEX_LEN);
        exact[SG_SHA1_HEX_LEN] = '\0';
        CHECK(sg_object_find_prefix(git_dir, exact, &out) == -1,
             "a full 40-char id must be rejected by find_prefix (exact ids go through "
             "sg_object_read, not this function)");
    }

    /* non-hex character within an otherwise valid length */
    CHECK(sg_object_find_prefix(git_dir, "12g4", &out) == -1,
         "a prefix containing a non-hex character must be rejected");

    free(git_dir);
}

/* Direct unit test of sg_oid_list_append's growth path (initial cap 16,
   doubling): appends 100 distinct ids -- crossing the 16/32/64 growth
   boundaries several times over -- and asserts both COUNT and CONTENT (in
   append order) survive every growth. Content, not just count, matters: a
   growth bug that reallocates without copying old entries (e.g. `cap * 2`
   mutated to `cap`, silently corrupting/overwriting memory in a non-ASan
   build) would still often leave count correct while destroying the
   earliest entries' bytes. No repo is needed -- sg_oid_list_append has
   nothing to do with the object store. */
static void test_oid_list_append_growth(void)
{
    sg_oid_list l;
    unsigned char expected[100][SG_SHA1_RAW_LEN];
    int i;

    memset(&l, 0, sizeof(l));

    for (i = 0; i < 100; i++) {
        char content[64];

        snprintf(content, sizeof(content), "phase68-oid-list-growth-%d", i);
        sg_object_hash(SG_OBJ_BLOB, content, strlen(content), expected[i]);
        CHECK(sg_oid_list_append(&l, expected[i]) == 0, "append #%d should succeed", i);
    }

    CHECK(l.count == 100, "expected count 100 after 100 appends, got %zu", l.count);
    for (i = 0; i < 100 && (size_t)i < l.count; i++) {
        CHECK(memcmp(l.ids[i], expected[i], SG_SHA1_RAW_LEN) == 0,
             "entry %d does not match what was appended (growth must preserve prior entries)", i);
    }

    sg_oid_list_free(&l);
}

int main(void)
{
    test_basic_prefix_lengths();
    test_case_insensitivity();
    test_zero_candidates();
    test_multiple_candidates();
    test_near_collision_disambiguates_loose();
    test_near_collision_disambiguates_pack();
    test_dedup_loose_and_packed();
    test_pack_only();
    test_loose_only();
    test_pack_registry_rescans_after_external_pack_write();
    test_loose_opendir_failure_is_fatal();
    test_malformed_inputs();
    test_oid_list_append_growth();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all oid prefix tests passed\n");
    return 0;
}
