#include "sg/chunk.h"

#include "sg/hash.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/refs.h"
#include "sg/repo.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

static void fill_random(unsigned char *buf, size_t len, uint32_t seed)
{
    uint32_t state = seed;
    size_t i;

    for (i = 0; i < len; i++) {
        state = state * 1103515245u + 12345u;
        buf[i] = (unsigned char)(state >> 16);
    }
}

/* checks that offsets/lengths tile [0, len) with no gaps or overlaps */
static int check_coverage(size_t len, const size_t *offsets, const size_t *lengths, size_t count)
{
    size_t expect = 0;
    size_t i;

    if (count == 0)
        return len == 0;

    for (i = 0; i < count; i++) {
        if (offsets[i] != expect)
            return 0;
        expect += lengths[i];
    }
    return expect == len;
}

static void test_determinism(void)
{
    size_t len = 500000;
    unsigned char *data = malloc(len);
    size_t *off1 = NULL, *len1 = NULL, *off2 = NULL, *len2 = NULL;
    size_t cnt1 = 0, cnt2 = 0;

    if (data == NULL) {
        fprintf(stderr, "FAIL determinism: out of memory\n");
        failures++;
        return;
    }
    fill_random(data, len, 42);

    if (sg_chunk_split(data, len, &off1, &len1, &cnt1) != 0 ||
        sg_chunk_split(data, len, &off2, &len2, &cnt2) != 0) {
        fprintf(stderr, "FAIL determinism: sg_chunk_split failed\n");
        failures++;
    } else if (cnt1 != cnt2 || cnt1 == 0 ||
              memcmp(off1, off2, cnt1 * sizeof(*off1)) != 0 ||
              memcmp(len1, len2, cnt1 * sizeof(*len1)) != 0) {
        fprintf(stderr, "FAIL determinism: two splits of the same data disagree\n");
        failures++;
    } else if (!check_coverage(len, off1, len1, cnt1)) {
        fprintf(stderr, "FAIL determinism: chunks don't tile the input\n");
        failures++;
    } else {
        printf("PASS determinism: %zu chunks, stable across two runs\n", cnt1);
    }

    free(data);
    free(off1);
    free(len1);
    free(off2);
    free(len2);
}

static void test_empty(void)
{
    size_t *offsets = (size_t *)1; /* poison to make sure it gets set to NULL */
    size_t *lengths = (size_t *)1;
    size_t count = 99;

    if (sg_chunk_split(NULL, 0, &offsets, &lengths, &count) != 0 || count != 0 ||
        offsets != NULL || lengths != NULL) {
        fprintf(stderr, "FAIL empty input: expected count=0, offsets=NULL, lengths=NULL\n");
        failures++;
        return;
    }
    printf("PASS empty input\n");
}

static void test_small(void)
{
    size_t len = SG_CHUNK_MIN_SIZE - 1000;
    unsigned char *data = malloc(len);
    size_t *offsets = NULL;
    size_t *lengths = NULL;
    size_t count = 0;

    if (data == NULL) {
        fprintf(stderr, "FAIL small input: out of memory\n");
        failures++;
        return;
    }
    fill_random(data, len, 7);

    if (sg_chunk_split(data, len, &offsets, &lengths, &count) != 0 || count != 1 ||
        offsets[0] != 0 || lengths[0] != len) {
        fprintf(stderr, "FAIL small input: expected exactly one chunk covering everything\n");
        failures++;
    } else {
        printf("PASS small input (< MIN_SIZE): one chunk\n");
    }

    free(data);
    free(offsets);
    free(lengths);
}

static void test_exact_min_size(void)
{
    size_t len = SG_CHUNK_MIN_SIZE;
    unsigned char *data = malloc(len);
    size_t *offsets = NULL;
    size_t *lengths = NULL;
    size_t count = 0;

    if (data == NULL) {
        fprintf(stderr, "FAIL exact MIN_SIZE: out of memory\n");
        failures++;
        return;
    }
    fill_random(data, len, 11);

    if (sg_chunk_split(data, len, &offsets, &lengths, &count) != 0 ||
        !check_coverage(len, offsets, lengths, count)) {
        fprintf(stderr, "FAIL exact MIN_SIZE: chunks don't tile the input\n");
        failures++;
    } else {
        printf("PASS exact MIN_SIZE: %zu chunk(s)\n", count);
    }

    free(data);
    free(offsets);
    free(lengths);
}

static void test_exact_max_size(void)
{
    size_t len = SG_CHUNK_MAX_SIZE;
    unsigned char *data = malloc(len);
    size_t *offsets = NULL;
    size_t *lengths = NULL;
    size_t count = 0;
    size_t i;
    int ok = 1;

    if (data == NULL) {
        fprintf(stderr, "FAIL exact MAX_SIZE: out of memory\n");
        failures++;
        return;
    }
    fill_random(data, len, 13);

    if (sg_chunk_split(data, len, &offsets, &lengths, &count) != 0 ||
        !check_coverage(len, offsets, lengths, count)) {
        ok = 0;
    }
    for (i = 0; ok && i < count; i++) {
        if (lengths[i] > SG_CHUNK_MAX_SIZE)
            ok = 0;
    }

    if (!ok) {
        fprintf(stderr, "FAIL exact MAX_SIZE: chunks don't tile input or exceed MAX_SIZE\n");
        failures++;
    } else {
        printf("PASS exact MAX_SIZE: %zu chunk(s)\n", count);
    }

    free(data);
    free(offsets);
    free(lengths);
}

static void test_over_max_size(void)
{
    size_t len = SG_CHUNK_MAX_SIZE + 1000;
    unsigned char *data = malloc(len);
    size_t *offsets = NULL;
    size_t *lengths = NULL;
    size_t count = 0;
    size_t i;
    int ok = 1;

    if (data == NULL) {
        fprintf(stderr, "FAIL over MAX_SIZE: out of memory\n");
        failures++;
        return;
    }
    fill_random(data, len, 17);

    if (sg_chunk_split(data, len, &offsets, &lengths, &count) != 0 ||
        !check_coverage(len, offsets, lengths, count) || count < 2) {
        ok = 0;
    }
    for (i = 0; ok && i < count; i++) {
        if (lengths[i] > SG_CHUNK_MAX_SIZE)
            ok = 0;
        if (i + 1 < count && lengths[i] < SG_CHUNK_MIN_SIZE)
            ok = 0;
    }

    if (!ok) {
        fprintf(stderr, "FAIL over MAX_SIZE: expected >=2 chunks each within bounds\n");
        failures++;
    } else {
        printf("PASS over MAX_SIZE: %zu chunks, all within bounds\n", count);
    }

    free(data);
    free(offsets);
    free(lengths);
}

static void test_pathological_uniform(void)
{
    size_t len = 1024 * 1024;
    unsigned char *data = malloc(len);
    size_t *offsets = NULL;
    size_t *lengths = NULL;
    size_t count = 0;
    size_t max_allowed = len / SG_CHUNK_MIN_SIZE;

    if (data == NULL) {
        fprintf(stderr, "FAIL pathological uniform: out of memory\n");
        failures++;
        return;
    }
    memset(data, 0, len);

    if (sg_chunk_split(data, len, &offsets, &lengths, &count) != 0 ||
        !check_coverage(len, offsets, lengths, count) || count > max_allowed) {
        fprintf(stderr, "FAIL pathological uniform: got %zu chunks, expected <= %zu\n", count,
                max_allowed);
        failures++;
    } else {
        printf("PASS pathological uniform (all-zero 1MiB): %zu chunk(s) (<= %zu)\n", count,
              max_allowed);
    }

    free(data);
    free(offsets);
    free(lengths);
}

static void test_roundtrip_reassembly(void)
{
    size_t len = 2 * 1024 * 1024;
    unsigned char *data = malloc(len);
    unsigned char *rebuilt = malloc(len);
    size_t *offsets = NULL;
    size_t *lengths = NULL;
    size_t count = 0;
    size_t i;
    size_t pos = 0;
    int ok;

    if (data == NULL || rebuilt == NULL) {
        fprintf(stderr, "FAIL roundtrip: out of memory\n");
        failures++;
        free(data);
        free(rebuilt);
        return;
    }
    fill_random(data, len, 99);

    ok = sg_chunk_split(data, len, &offsets, &lengths, &count) == 0 && count > 1;
    if (ok) {
        for (i = 0; i < count; i++) {
            memcpy(rebuilt + pos, data + offsets[i], lengths[i]);
            pos += lengths[i];
        }
        ok = pos == len && memcmp(rebuilt, data, len) == 0;
    }

    if (!ok) {
        fprintf(stderr, "FAIL roundtrip: reassembled data doesn't match original\n");
        failures++;
    } else {
        printf("PASS roundtrip reassembly: %zu chunks reassemble exactly\n", count);
    }

    free(data);
    free(rebuilt);
    free(offsets);
    free(lengths);
}

static void test_dedup(void)
{
    size_t len = 3 * 1024 * 1024;
    size_t edit_off = len / 2;
    size_t edit_len = 1024;
    unsigned char *a = malloc(len);
    unsigned char *b = malloc(len);
    size_t *a_off = NULL, *a_len = NULL, *b_off = NULL, *b_len = NULL;
    size_t a_cnt = 0, b_cnt = 0;
    size_t matches = 0;
    size_t i, j;
    int ok;

    if (a == NULL || b == NULL) {
        fprintf(stderr, "FAIL dedup: out of memory\n");
        failures++;
        free(a);
        free(b);
        return;
    }
    fill_random(a, len, 1234);
    memcpy(b, a, len);
    fill_random(b + edit_off, edit_len, 5678); /* differs from a in a small middle region */

    ok = sg_chunk_split(a, len, &a_off, &a_len, &a_cnt) == 0 &&
        sg_chunk_split(b, len, &b_off, &b_len, &b_cnt) == 0 && a_cnt >= 4;

    if (ok) {
        for (i = 0; i < a_cnt; i++) {
            for (j = 0; j < b_cnt; j++) {
                if (a_len[i] == b_len[j] &&
                    memcmp(a + a_off[i], b + b_off[j], a_len[i]) == 0) {
                    matches++;
                    break;
                }
            }
        }
        ok = matches * 2 >= a_cnt;
    }

    if (!ok) {
        fprintf(stderr, "FAIL dedup: only %zu/%zu of A's chunks reused in B\n", matches, a_cnt);
        failures++;
    } else {
        printf("PASS dedup: %zu/%zu of A's chunks byte-identical to some chunk in B\n", matches,
              a_cnt);
    }

    free(a);
    free(b);
    free(a_off);
    free(a_len);
    free(b_off);
    free(b_len);
}

static void make_hex(char out[41], char c)
{
    memset(out, (unsigned char)c, 40);
    out[40] = '\0';
}

static void test_pointer_parse_valid(void)
{
    char sha1_hex[41];
    char hex0[41], hex1[41], hex2[41];
    char content[512];
    int n;
    size_t len;
    sg_chunk_pointer p;

    make_hex(sha1_hex, 'd');
    make_hex(hex0, 'a');
    make_hex(hex1, 'b');
    make_hex(hex2, 'c');

    n = snprintf(content, sizeof(content),
                "sg-chunked v1\nsize 12345\nsha1 %s\nchunks 3\n%s\n%s\n%s\n", sha1_hex, hex0, hex1,
                hex2);
    len = (size_t)n;

    if (sg_chunk_pointer_parse((const unsigned char *)content, len, &p) != 1) {
        fprintf(stderr, "FAIL pointer parse valid: rejected a well-formed pointer\n");
        failures++;
        return;
    }

    if (p.original_size != 12345 || p.chunk_count != 3) {
        fprintf(stderr, "FAIL pointer parse valid: size/chunk_count mismatch\n");
        failures++;
        sg_chunk_pointer_free(&p);
        return;
    }

    {
        unsigned char expect_sha1[SG_SHA1_RAW_LEN];
        unsigned char expect0[SG_SHA1_RAW_LEN];
        unsigned char expect1[SG_SHA1_RAW_LEN];
        unsigned char expect2[SG_SHA1_RAW_LEN];

        sg_hex_to_sha1(sha1_hex, expect_sha1);
        sg_hex_to_sha1(hex0, expect0);
        sg_hex_to_sha1(hex1, expect1);
        sg_hex_to_sha1(hex2, expect2);

        if (memcmp(p.original_sha1, expect_sha1, SG_SHA1_RAW_LEN) != 0 ||
            memcmp(p.chunk_ids[0], expect0, SG_SHA1_RAW_LEN) != 0 ||
            memcmp(p.chunk_ids[1], expect1, SG_SHA1_RAW_LEN) != 0 ||
            memcmp(p.chunk_ids[2], expect2, SG_SHA1_RAW_LEN) != 0) {
            fprintf(stderr, "FAIL pointer parse valid: sha1/chunk_ids content mismatch\n");
            failures++;
            sg_chunk_pointer_free(&p);
            return;
        }
    }

    printf("PASS pointer parse valid: 3-chunk pointer parsed correctly\n");
    sg_chunk_pointer_free(&p);
}

static void test_pointer_format_roundtrip(void)
{
    sg_chunk_pointer src;
    sg_chunk_pointer parsed;
    unsigned char ids[3][SG_SHA1_RAW_LEN];
    unsigned char *content = NULL;
    size_t content_len = 0;
    int ok;

    sg_sha1("hello", 5, src.original_sha1);
    sg_sha1("chunk-a", 7, ids[0]);
    sg_sha1("chunk-b", 7, ids[1]);
    sg_sha1("chunk-c", 7, ids[2]);
    src.original_size = 987654;
    src.chunk_ids = ids;
    src.chunk_count = 3;

    ok = sg_chunk_pointer_format(&src, &content, &content_len) == 0;
    if (ok)
        ok = sg_chunk_pointer_parse(content, content_len, &parsed) == 1;
    if (ok) {
        ok = parsed.original_size == src.original_size && parsed.chunk_count == src.chunk_count &&
            memcmp(parsed.original_sha1, src.original_sha1, SG_SHA1_RAW_LEN) == 0 &&
            memcmp(parsed.chunk_ids[0], ids[0], SG_SHA1_RAW_LEN) == 0 &&
            memcmp(parsed.chunk_ids[1], ids[1], SG_SHA1_RAW_LEN) == 0 &&
            memcmp(parsed.chunk_ids[2], ids[2], SG_SHA1_RAW_LEN) == 0;
        sg_chunk_pointer_free(&parsed);
    }

    if (!ok) {
        fprintf(stderr, "FAIL pointer format/parse roundtrip\n");
        failures++;
    } else {
        printf("PASS pointer format/parse roundtrip\n");
    }

    free(content);
}

static void expect_rejected(const char *label, const char *content, size_t len)
{
    sg_chunk_pointer p;

    if (sg_chunk_pointer_parse((const unsigned char *)content, len, &p) != 0) {
        fprintf(stderr, "FAIL malformed pointer accepted: %s\n", label);
        failures++;
        sg_chunk_pointer_free(&p);
        return;
    }
    printf("PASS malformed pointer rejected: %s\n", label);
}

static void test_pointer_parse_malformed(void)
{
    char sha1_hex[41];
    char hex0[41], hex1[41], hex2[41];

    make_hex(sha1_hex, 'd');
    make_hex(hex0, 'a');
    make_hex(hex1, 'b');
    make_hex(hex2, 'c');

    {
        char content[512];
        int n = snprintf(content, sizeof(content), "sg-chunked v2\nsize 1\nsha1 %s\nchunks 0\n",
                         sha1_hex);
        expect_rejected("wrong magic", content, (size_t)n);
    }
    {
        char content[512];
        int n = snprintf(content, sizeof(content), "sg-chunked v1sha1 %s\nchunks 0\n", sha1_hex);
        expect_rejected("magic missing trailing newline", content, (size_t)n);
    }
    {
        char content[512];
        int n = snprintf(content, sizeof(content), "sg-chunked v1\nsha1 %s\nchunks 1\n%s\n",
                         sha1_hex, hex0);
        expect_rejected("missing size line", content, (size_t)n);
    }
    {
        char content[512];
        int n = snprintf(content, sizeof(content), "sg-chunked v1\nsize 1\nchunks 1\n%s\n", hex0);
        expect_rejected("missing sha1 line", content, (size_t)n);
    }
    {
        char content[512];
        int n = snprintf(content, sizeof(content), "sg-chunked v1\nsize 1\nsha1 %s\n%s\n",
                         sha1_hex, hex0);
        expect_rejected("missing chunks line", content, (size_t)n);
    }
    {
        char content[512];
        int n = snprintf(content, sizeof(content),
                         "sg-chunked v1\nsize 1\nsha1 %s\nchunks 3\n%s\n%s\n", sha1_hex, hex0,
                         hex1);
        expect_rejected("chunks count greater than actual lines", content, (size_t)n);
    }
    {
        char content[512];
        int n = snprintf(content, sizeof(content),
                         "sg-chunked v1\nsize 1\nsha1 %s\nchunks 1\n%s\n%s\n", sha1_hex, hex0,
                         hex1);
        expect_rejected("chunks count less than actual lines (trailing extra)", content,
                        (size_t)n);
    }
    {
        char content[512];
        int n = snprintf(content, sizeof(content),
                         "sg-chunked v1\nsize 1\nsha1 %s\nchunks 2\n%s\n%s\nEXTRA", sha1_hex, hex0,
                         hex1);
        expect_rejected("trailing garbage after well-formed pointer", content, (size_t)n);
    }
    {
        char content[512];
        char bad_hex[41];
        int n;

        make_hex(bad_hex, 'a');
        bad_hex[3] = 'g'; /* not a hex digit */
        n = snprintf(content, sizeof(content), "sg-chunked v1\nsize 1\nsha1 %s\nchunks 1\n%s\n",
                    sha1_hex, bad_hex);
        expect_rejected("chunk line with non-hex character", content, (size_t)n);
    }
    {
        char content[512];
        int n = snprintf(content, sizeof(content),
                         "sg-chunked v1\nsize 1\nsha1 %s\nchunks 1\n%.39s\n", sha1_hex, hex0);
        expect_rejected("chunk line too short (39 hex chars)", content, (size_t)n);
    }
    {
        char content[512];
        int n = snprintf(content, sizeof(content),
                         "sg-chunked v1\nsize 1\nsha1 %s\nchunks 1\n%saa\n", sha1_hex, hex0);
        expect_rejected("chunk line too long (42 hex chars)", content, (size_t)n);
    }
    {
        char content[512];
        char bad_sha1[41];
        int n;

        make_hex(bad_sha1, 'd');
        bad_sha1[10] = 'z'; /* not a hex digit */
        n = snprintf(content, sizeof(content), "sg-chunked v1\nsize 1\nsha1 %s\nchunks 1\n%s\n",
                    bad_sha1, hex0);
        expect_rejected("sha1 field with non-hex character", content, (size_t)n);
    }
    {
        char content[512];
        int n = snprintf(content, sizeof(content), "sg-chunked v1\nsize 1\nsha1 %.39s\nchunks 1\n%s\n",
                         sha1_hex, hex0);
        expect_rejected("sha1 field wrong length (39 hex chars)", content, (size_t)n);
    }
}

static char *make_tmp_repo(void)
{
    static char template[] = "/tmp/sg_chunk_test_XXXXXX";
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

static void test_store_blob_below_threshold(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char data[1000];
    unsigned char id[SG_SHA1_RAW_LEN];
    unsigned char expect_id[SG_SHA1_RAW_LEN];
    int chunked = -1;

    fill_random(data, sizeof(data), 3);
    sg_object_hash(SG_OBJ_BLOB, data, sizeof(data), expect_id);

    if (sg_chunk_store_blob(git_dir, data, sizeof(data), 4096, id, &chunked) != 0 || chunked != 0 ||
       memcmp(id, expect_id, SG_SHA1_RAW_LEN) != 0) {
        fprintf(stderr, "FAIL store_blob below threshold: expected unchunked ordinary blob\n");
        failures++;
    } else {
        printf("PASS store_blob below threshold: stored as an ordinary blob\n");
    }

    free(git_dir);
}

static void test_store_blob_above_threshold(void)
{
    char *git_dir = make_tmp_repo();
    size_t len = 3 * 1024 * 1024;
    unsigned char *data = malloc(len);
    unsigned char id[SG_SHA1_RAW_LEN];
    sg_obj_type type;
    unsigned char *pointer_content = NULL;
    size_t pointer_len = 0;
    sg_chunk_pointer p;
    int chunked = -1;
    int ok;

    if (data == NULL) {
        fprintf(stderr, "FAIL store_blob above threshold: out of memory\n");
        failures++;
        free(git_dir);
        return;
    }
    fill_random(data, len, 5);

    ok = sg_chunk_store_blob(git_dir, data, len, 1024 * 1024, id, &chunked) == 0 && chunked == 1;
    if (ok)
        ok = sg_loose_read(git_dir, id, &type, &pointer_content, &pointer_len) == 0 &&
            type == SG_OBJ_BLOB;
    if (ok)
        ok = sg_chunk_pointer_parse(pointer_content, pointer_len, &p) == 1;
    if (ok) {
        unsigned char expect_sha1[SG_SHA1_RAW_LEN];
        unsigned char *rebuilt = malloc(len);
        size_t pos = 0;
        size_t i;

        sg_object_hash(SG_OBJ_BLOB, data, len, expect_sha1);
        ok = rebuilt != NULL && p.original_size == len && p.chunk_count > 1 &&
            memcmp(p.original_sha1, expect_sha1, SG_SHA1_RAW_LEN) == 0;

        for (i = 0; ok && i < p.chunk_count; i++) {
            unsigned char *buf = NULL;
            size_t buf_len = 0;
            sg_obj_type chunk_type;

            if (sg_loose_read(git_dir, p.chunk_ids[i], &chunk_type, &buf, &buf_len) != 0 ||
               chunk_type != SG_OBJ_BLOB || pos + buf_len > len) {
                free(buf);
                ok = 0;
                break;
            }
            memcpy(rebuilt + pos, buf, buf_len);
            pos += buf_len;
            free(buf);
        }
        if (ok)
            ok = pos == len && memcmp(rebuilt, data, len) == 0;

        free(rebuilt);
        sg_chunk_pointer_free(&p);
    }

    if (!ok) {
        fprintf(stderr, "FAIL store_blob above threshold: pointer/chunks don't reproduce data\n");
        failures++;
    } else {
        printf("PASS store_blob above threshold: pointer blob reassembles to original data\n");
    }

    free(pointer_content);
    free(data);
    free(git_dir);
}

static void test_read_blob_ordinary(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char data[2000];
    unsigned char id[SG_SHA1_RAW_LEN];
    unsigned char effective[SG_SHA1_RAW_LEN];
    unsigned char *out = NULL;
    size_t out_len = 0;
    int ok;

    fill_random(data, sizeof(data), 55);
    ok = sg_loose_write(git_dir, SG_OBJ_BLOB, data, sizeof(data), id) == 0;
    if (ok)
        ok = sg_chunk_read_blob(git_dir, id, &out, &out_len, NULL) == 0 && out_len == sizeof(data) &&
            memcmp(out, data, sizeof(data)) == 0;
    if (ok)
        ok = sg_chunk_effective_id(git_dir, id, effective) == 0 &&
            memcmp(effective, id, SG_SHA1_RAW_LEN) == 0;

    if (!ok) {
        fprintf(stderr, "FAIL read_blob/effective_id ordinary blob\n");
        failures++;
    } else {
        printf("PASS read_blob/effective_id: ordinary (non-pointer) blob passes through unchanged\n");
    }

    free(out);
    free(git_dir);
}

static void test_read_blob_valid_pointer(void)
{
    char *git_dir = make_tmp_repo();
    size_t len = 3 * 1024 * 1024;
    unsigned char *data = malloc(len);
    unsigned char id[SG_SHA1_RAW_LEN];
    unsigned char effective[SG_SHA1_RAW_LEN];
    unsigned char expect_sha1[SG_SHA1_RAW_LEN];
    unsigned char *out = NULL;
    size_t out_len = 0;
    int chunked = -1;
    int ok;

    if (data == NULL) {
        fprintf(stderr, "FAIL read_blob valid pointer: out of memory\n");
        failures++;
        free(git_dir);
        return;
    }
    fill_random(data, len, 21);
    sg_object_hash(SG_OBJ_BLOB, data, len, expect_sha1);

    ok = sg_chunk_store_blob(git_dir, data, len, 1024 * 1024, id, &chunked) == 0 && chunked == 1;
    if (ok)
        ok = sg_chunk_read_blob(git_dir, id, &out, &out_len, NULL) == 0 && out_len == len &&
            memcmp(out, data, len) == 0;
    if (ok)
        ok = sg_chunk_effective_id(git_dir, id, effective) == 0 &&
            memcmp(effective, expect_sha1, SG_SHA1_RAW_LEN) == 0;

    if (!ok) {
        fprintf(stderr, "FAIL read_blob valid pointer: reassembly/effective id mismatch\n");
        failures++;
    } else {
        printf("PASS read_blob/effective_id: valid pointer reassembles & normalizes correctly\n");
    }

    free(out);
    free(data);
    free(git_dir);
}

/* Registers ids (count of them) as members of git_dir's SG_CHUNK_KEEPALIVE_REF
   tree, via the same public merge API fetch/clone use for a remote's
   keep-alive commit (sg_chunk_keepalive_merge_commit): builds a tree with
   one mode-100644 entry per id (name = the id's own 40-hex form, matching
   keep_alive_add's layout in chunk.c) wrapped in a no-parent commit, then
   merges it in. Needed by tests below that hand-build a pointer blob's
   chunks via sg_loose_write directly (bypassing sg_chunk_store_blob, which
   would otherwise register them itself): since chunk_resolve's discriminator
   decides "is this a real pointer of ours" by keep-alive tree membership,
   not raw object existence (see chunk.c), a hand-built pointer's first chunk
   id must be registered here to be recognized as "ours" at all. Returns 0 on
   success, -1 on failure. */
static int add_chunks_to_keepalive(const char *git_dir, unsigned char (*ids)[SG_SHA1_RAW_LEN],
                                   size_t count)
{
    sg_tree_entry *entries = malloc(count * sizeof(*entries));
    unsigned char *tree_content = NULL;
    size_t tree_len = 0;
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    sg_commit commit;
    unsigned char *commit_content = NULL;
    size_t commit_len = 0;
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    size_t i;
    int rc = -1;

    if (entries == NULL)
        return -1;

    for (i = 0; i < count; i++) {
        char hex[SG_SHA1_HEX_LEN + 1];

        sg_sha1_to_hex(ids[i], hex);
        entries[i].name = strdup(hex);
        if (entries[i].name == NULL) {
            size_t j;

            for (j = 0; j < i; j++)
                free(entries[j].name);
            free(entries);
            return -1;
        }
        entries[i].mode = 0100644;
        memcpy(entries[i].sha1, ids[i], SG_SHA1_RAW_LEN);
    }

    if (sg_tree_serialize(entries, count, &tree_content, &tree_len) != 0)
        goto done;
    if (sg_loose_write(git_dir, SG_OBJ_TREE, tree_content, tree_len, tree_id) != 0)
        goto done;

    memset(&commit, 0, sizeof(commit));
    memcpy(commit.tree, tree_id, SG_SHA1_RAW_LEN);
    commit.author_name = (char *)"test";
    commit.author_email = (char *)"test@test";
    commit.author_time = 0;
    strcpy(commit.author_tz, "+0000");
    commit.committer_name = (char *)"test";
    commit.committer_email = (char *)"test@test";
    commit.committer_time = 0;
    strcpy(commit.committer_tz, "+0000");
    commit.message = (char *)"test keep-alive\n";

    if (sg_commit_serialize(&commit, &commit_content, &commit_len) != 0)
        goto done;
    if (sg_loose_write(git_dir, SG_OBJ_COMMIT, commit_content, commit_len, commit_id) != 0)
        goto done;

    rc = sg_chunk_keepalive_merge_commit(git_dir, commit_id);

done:
    free(tree_content);
    free(commit_content);
    for (i = 0; i < count; i++)
        free(entries[i].name);
    free(entries);
    return rc;
}

/* A pointer blob that is well-formed per sg_chunk_pointer_parse AND whose
   first declared chunk id is registered in the SG_CHUNK_KEEPALIVE_REF tree
   -- so per the discriminator in chunk_resolve (chunk.c), this counts as a
   genuine chunk pointer, not a coincidental look-alike. Its declared
   original_sha1 is a lie, though (doesn't match sg_object_hash of the
   reassembled bytes): both chunks are readable, so reassembly succeeds, but
   the post-reassembly hash check must still catch this. This is the case
   the phase 6b durability fix changed: previously this fell back silently
   to the pointer's own raw bytes (masking data corruption as a successful,
   if odd-looking, read); now it must be a hard -2 error instead, never
   silently papered over. */
static void test_read_blob_real_pointer_hash_mismatch(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char chunk0[SG_SHA1_RAW_LEN], chunk1[SG_SHA1_RAW_LEN];
    unsigned char data0[100], data1[100];
    sg_chunk_pointer ptr;
    unsigned char ids[2][SG_SHA1_RAW_LEN];
    unsigned char *pointer_content = NULL;
    size_t pointer_len = 0;
    unsigned char broken_id[SG_SHA1_RAW_LEN];
    unsigned char *out = NULL;
    size_t out_len = 0;
    sg_chunk_missing_info missing;
    unsigned char effective[SG_SHA1_RAW_LEN];
    int ok;

    fill_random(data0, sizeof(data0), 31);
    fill_random(data1, sizeof(data1), 32);
    if (sg_loose_write(git_dir, SG_OBJ_BLOB, data0, sizeof(data0), chunk0) != 0 ||
       sg_loose_write(git_dir, SG_OBJ_BLOB, data1, sizeof(data1), chunk1) != 0) {
        fprintf(stderr, "FAIL real pointer (hash mismatch): setup failed writing chunk blobs\n");
        failures++;
        free(git_dir);
        return;
    }
    memcpy(ids[0], chunk0, SG_SHA1_RAW_LEN);
    memcpy(ids[1], chunk1, SG_SHA1_RAW_LEN);

    /* Bogus original_sha1 (all 0xEE), not the real hash of data0||data1. */
    memset(ptr.original_sha1, 0xEE, SG_SHA1_RAW_LEN);
    ptr.original_size = sizeof(data0) + sizeof(data1);
    ptr.chunk_ids = ids;
    ptr.chunk_count = 2;

    /* Register chunk0 (the first declared chunk id) as keep-alive: without
       this, chunk_resolve's discriminator would not recognize the pointer
       built below as one of ours at all, regardless of chunk0 being a real,
       readable object. */
    if (add_chunks_to_keepalive(git_dir, ids, 2) != 0) {
        fprintf(stderr, "FAIL real pointer (hash mismatch): setup failed registering keep-alive\n");
        failures++;
        free(git_dir);
        return;
    }

    ok = sg_chunk_pointer_format(&ptr, &pointer_content, &pointer_len) == 0;
    if (ok)
        ok = sg_loose_write(git_dir, SG_OBJ_BLOB, pointer_content, pointer_len, broken_id) == 0;
    if (ok) {
        memset(&missing, 0xff, sizeof(missing)); /* poison: must be overwritten on -2 */
        ok = sg_chunk_read_blob(git_dir, broken_id, &out, &out_len, &missing) == -2 &&
            missing.chunk_count == 2 && missing.missing_count == 0;
    }
    if (ok)
        ok = sg_chunk_effective_id(git_dir, broken_id, effective) == -2;

    if (!ok) {
        fprintf(stderr,
               "FAIL real pointer (hash mismatch): expected a hard -2 error, not a silent fallback\n");
        failures++;
    } else {
        printf("PASS real pointer (hash mismatch): hard error, not silently treated as ordinary "
              "content\n");
    }

    free(out);
    free(pointer_content);
    free(git_dir);
}

/* Same idea as the hash-mismatch case above, but the corruption is an
   actually-missing chunk (index 1 of 2) rather than a hash lie -- chunk 0
   still exists, so this is recognized as a genuine pointer, and the missing
   chunk must be reported precisely (1 missing out of 2 declared). */
static void test_read_blob_real_pointer_missing_middle_chunk(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char chunk0[SG_SHA1_RAW_LEN], missing_chunk[SG_SHA1_RAW_LEN];
    unsigned char data0[100];
    sg_chunk_pointer ptr;
    unsigned char ids[2][SG_SHA1_RAW_LEN];
    unsigned char *pointer_content = NULL;
    size_t pointer_len = 0;
    unsigned char broken_id[SG_SHA1_RAW_LEN];
    unsigned char *out = NULL;
    size_t out_len = 0;
    sg_chunk_missing_info missing;
    unsigned char effective[SG_SHA1_RAW_LEN];
    int ok;

    fill_random(data0, sizeof(data0), 41);
    sg_sha1("this-chunk-was-gc-d-away", 24, missing_chunk);
    if (sg_loose_write(git_dir, SG_OBJ_BLOB, data0, sizeof(data0), chunk0) != 0) {
        fprintf(stderr, "FAIL real pointer (missing middle chunk): setup failed\n");
        failures++;
        free(git_dir);
        return;
    }
    memcpy(ids[0], chunk0, SG_SHA1_RAW_LEN);
    memcpy(ids[1], missing_chunk, SG_SHA1_RAW_LEN);

    memset(ptr.original_sha1, 0xCD, SG_SHA1_RAW_LEN);
    ptr.original_size = 5000; /* doesn't matter -- reassembly can never finish */
    ptr.chunk_ids = ids;
    ptr.chunk_count = 2;

    /* Register chunk0 (the first declared chunk id) as keep-alive -- same
       reasoning as the hash-mismatch test above. missing_chunk is
       deliberately NOT registered (and was never written as an object
       either): it's the one this test wants to be genuinely absent. */
    if (add_chunks_to_keepalive(git_dir, ids, 1) != 0) {
        fprintf(stderr,
               "FAIL real pointer (missing middle chunk): setup failed registering keep-alive\n");
        failures++;
        free(git_dir);
        return;
    }

    ok = sg_chunk_pointer_format(&ptr, &pointer_content, &pointer_len) == 0;
    if (ok)
        ok = sg_loose_write(git_dir, SG_OBJ_BLOB, pointer_content, pointer_len, broken_id) == 0;
    if (ok) {
        ok = sg_chunk_read_blob(git_dir, broken_id, &out, &out_len, &missing) == -2 &&
            missing.chunk_count == 2 && missing.missing_count == 1;
    }
    if (ok)
        ok = sg_chunk_effective_id(git_dir, broken_id, effective) == -2;

    if (!ok) {
        fprintf(stderr,
               "FAIL real pointer (missing middle chunk): expected -2 with missing_count == 1\n");
        failures++;
    } else {
        printf("PASS real pointer (missing chunk): hard error, missing_count reported precisely\n");
    }

    free(out);
    free(pointer_content);
    free(git_dir);
}

/* A pointer blob that parses fine per sg_chunk_pointer_parse but whose
   *first* declared chunk id was never registered in the SG_CHUNK_KEEPALIVE_REF
   tree (indeed, was never written as an object at all here) -- per the
   discriminator, this is indistinguishable from an ordinary small file that
   coincidentally looks like a pointer, so it must fall back to raw content,
   not be treated as a broken real pointer. */
static void test_read_blob_fake_pointer_missing_chunk(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char missing_id[SG_SHA1_RAW_LEN];
    sg_chunk_pointer ptr;
    unsigned char ids[1][SG_SHA1_RAW_LEN];
    unsigned char *pointer_content = NULL;
    size_t pointer_len = 0;
    unsigned char fake_id[SG_SHA1_RAW_LEN];
    unsigned char *out = NULL;
    size_t out_len = 0;
    unsigned char effective[SG_SHA1_RAW_LEN];
    int ok;

    sg_sha1("nonexistent-chunk", 17, missing_id);
    memcpy(ids[0], missing_id, SG_SHA1_RAW_LEN);

    memset(ptr.original_sha1, 0xAB, SG_SHA1_RAW_LEN);
    ptr.original_size = 12345;
    ptr.chunk_ids = ids;
    ptr.chunk_count = 1;

    ok = sg_chunk_pointer_format(&ptr, &pointer_content, &pointer_len) == 0;
    if (ok)
        ok = sg_loose_write(git_dir, SG_OBJ_BLOB, pointer_content, pointer_len, fake_id) == 0;
    if (ok)
        ok = sg_chunk_read_blob(git_dir, fake_id, &out, &out_len, NULL) == 0 &&
            out_len == pointer_len && memcmp(out, pointer_content, pointer_len) == 0;
    if (ok)
        ok = sg_chunk_effective_id(git_dir, fake_id, effective) == 0 &&
            memcmp(effective, fake_id, SG_SHA1_RAW_LEN) == 0;

    if (!ok) {
        fprintf(stderr, "FAIL fake pointer (missing chunk): expected fallback to raw pointer bytes\n");
        failures++;
    } else {
        printf("PASS fake pointer (missing first chunk): treated as ordinary content, not a broken "
              "pointer\n");
    }

    free(out);
    free(pointer_content);
    free(git_dir);
}

static void test_read_blob_object_not_found(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char bogus_id[SG_SHA1_RAW_LEN];
    unsigned char *out = NULL;
    size_t out_len = 0;
    unsigned char effective[SG_SHA1_RAW_LEN];

    sg_sha1("does-not-exist", 14, bogus_id);

    if (sg_chunk_read_blob(git_dir, bogus_id, &out, &out_len, NULL) != -1 ||
       sg_chunk_effective_id(git_dir, bogus_id, effective) != -1) {
        fprintf(stderr, "FAIL read_blob/effective_id: expected -1 for a nonexistent object\n");
        failures++;
    } else {
        printf("PASS read_blob/effective_id: -1 returned when underlying object is missing\n");
    }

    free(out);
    free(git_dir);
}

/* End-to-end version of the missing-chunk hard-error case above, going
   through the real sg_chunk_store_blob write path (not a hand-built
   pointer): store a large file chunked, delete one of its actual chunk
   object files from disk (simulating exactly what `git gc --prune=now`
   would do to an unreachable chunk), and confirm sg_chunk_read_blob fails
   closed with an accurate missing count instead of silently handing back
   the pointer's own text as if it were the file's content -- this is
   problem 2 from the phase 6b spec, reproduced directly. */
static void test_read_blob_store_then_delete_one_chunk(void)
{
    char *git_dir = make_tmp_repo();
    size_t len = 3 * 1024 * 1024;
    unsigned char *data = malloc(len);
    unsigned char id[SG_SHA1_RAW_LEN];
    sg_obj_type type;
    unsigned char *pointer_content = NULL;
    size_t pointer_len = 0;
    sg_chunk_pointer ptr;
    int chunked = -1;
    unsigned char *out = NULL;
    size_t out_len = 0;
    sg_chunk_missing_info missing;
    int ok;

    if (data == NULL) {
        fprintf(stderr, "FAIL store-then-delete-chunk: out of memory\n");
        failures++;
        free(git_dir);
        return;
    }
    fill_random(data, len, 61);

    ok = sg_chunk_store_blob(git_dir, data, len, 1024 * 1024, id, &chunked) == 0 && chunked == 1;
    if (ok)
        ok = sg_loose_read(git_dir, id, &type, &pointer_content, &pointer_len) == 0;
    if (ok)
        ok = sg_chunk_pointer_parse(pointer_content, pointer_len, &ptr) == 1 && ptr.chunk_count > 1;
    if (ok) {
        char hex[SG_SHA1_HEX_LEN + 1];
        char loose_path[4096];

        /* Delete exactly one chunk's loose object file straight off disk --
           the same end state `git gc --prune=now` leaves an unreachable
           chunk in. Deliberately not chunk_ids[0]: the discriminator in
           chunk_resolve (chunk.c) decides "is this a real pointer" by
           checking whether the *first* declared chunk id exists, so
           deleting that one specifically would (correctly, per that
           documented trade-off) make this look like an unrecognized,
           coincidentally pointer-shaped ordinary file rather than a broken
           real pointer -- deleting a later chunk instead exercises the
           "recognized as a real pointer, then found broken" path this test
           is actually after. */
        sg_sha1_to_hex(ptr.chunk_ids[1], hex);
        snprintf(loose_path, sizeof(loose_path), "%s/objects/%.2s/%s", git_dir, hex, hex + 2);
        ok = remove(loose_path) == 0;

        if (ok) {
            ok = sg_chunk_read_blob(git_dir, id, &out, &out_len, &missing) == -2 &&
                missing.chunk_count == ptr.chunk_count && missing.missing_count == 1;
        }
        sg_chunk_pointer_free(&ptr);
    }

    if (!ok) {
        fprintf(stderr, "FAIL store-then-delete-chunk: expected a hard -2 error after gc-like "
               "deletion\n");
        failures++;
    } else {
        printf("PASS store-then-delete-chunk: deleting one chunk file fails closed with an "
              "accurate missing count (not silent corruption)\n");
    }

    free(out);
    free(pointer_content);
    free(data);
    free(git_dir);
}

/* Regression test for the residual silent-corruption bug this discriminator
   change fixes: deleting the *first* declared chunk's loose object file
   (rather than a later one, as the sibling test above does) used to make a
   genuine chunk pointer misdiagnosed as "not a pointer at all", because the
   old discriminator decided identity by checking whether that exact chunk's
   object file existed -- so losing it flipped the verdict from "real
   pointer, broken" to "ordinary content", and sg_chunk_read_blob handed back
   the pointer's own ~450-byte text as if it were the file's multi-megabyte
   content (outcome 1 instead of outcome 3). Now that identity is decided by
   SG_CHUNK_KEEPALIVE_REF tree membership -- which does not care whether the
   chunk's object file still exists -- deleting the first chunk must still
   be recognized as a genuine (if broken) pointer and fail with -2, exactly
   like deleting any other chunk. */
static void test_read_blob_store_then_delete_first_chunk(void)
{
    char *git_dir = make_tmp_repo();
    size_t len = 3 * 1024 * 1024;
    unsigned char *data = malloc(len);
    unsigned char id[SG_SHA1_RAW_LEN];
    sg_obj_type type;
    unsigned char *pointer_content = NULL;
    size_t pointer_len = 0;
    sg_chunk_pointer ptr;
    int chunked = -1;
    unsigned char *out = NULL;
    size_t out_len = 0;
    sg_chunk_missing_info missing;
    int ok;

    if (data == NULL) {
        fprintf(stderr, "FAIL store-then-delete-first-chunk: out of memory\n");
        failures++;
        free(git_dir);
        return;
    }
    fill_random(data, len, 62);

    ok = sg_chunk_store_blob(git_dir, data, len, 1024 * 1024, id, &chunked) == 0 && chunked == 1;
    if (ok)
        ok = sg_loose_read(git_dir, id, &type, &pointer_content, &pointer_len) == 0;
    if (ok)
        ok = sg_chunk_pointer_parse(pointer_content, pointer_len, &ptr) == 1 && ptr.chunk_count > 1;
    if (ok) {
        char hex[SG_SHA1_HEX_LEN + 1];
        char loose_path[4096];

        /* Delete chunk_ids[0]'s loose object file specifically -- the exact
           case that used to fall through to raw-content fallback instead of
           a hard error. */
        sg_sha1_to_hex(ptr.chunk_ids[0], hex);
        snprintf(loose_path, sizeof(loose_path), "%s/objects/%.2s/%s", git_dir, hex, hex + 2);
        ok = remove(loose_path) == 0;

        if (ok) {
            ok = sg_chunk_read_blob(git_dir, id, &out, &out_len, &missing) == -2 &&
                missing.chunk_count == ptr.chunk_count && missing.missing_count == 1;
        }
        /* Must never fall back to "not a pointer": the pointer's own raw
           text must not be handed back as if it were the file's content. */
        if (ok)
            ok = out == NULL;
        sg_chunk_pointer_free(&ptr);
    }

    if (!ok) {
        fprintf(stderr, "FAIL store-then-delete-first-chunk: expected a hard -2 error, not a "
               "silent fallback to raw pointer text\n");
        failures++;
    } else {
        printf("PASS store-then-delete-first-chunk: deleting the FIRST chunk file still fails "
              "closed with an accurate missing count (not silently misdiagnosed as an ordinary "
              "file)\n");
    }

    free(out);
    free(pointer_content);
    free(data);
    free(git_dir);
}

/* ---- refs/sg/chunks keep-alive tree tests ---- */

/* Reads SG_CHUNK_KEEPALIVE_REF -> commit -> tree via the same public API a
   real caller (fsck-adjacent tooling, or `git gc` itself) would use, and
   returns its entries' sha1s as a raw array. Returns 0 with *count_out == 0
   if the ref doesn't exist. Returns -1 on any read/parse failure. */
static int read_keepalive_tree_ids(const char *git_dir, unsigned char (**ids_out)[SG_SHA1_RAW_LEN],
                                   size_t *count_out)
{
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    sg_obj_type type;
    unsigned char *content = NULL;
    size_t content_len = 0;
    sg_commit commit;
    sg_tree tree;
    unsigned char (*ids)[SG_SHA1_RAW_LEN] = NULL;
    size_t i;

    *ids_out = NULL;
    *count_out = 0;

    if (sg_ref_read_path(git_dir, SG_CHUNK_KEEPALIVE_REF, commit_id) != 0)
        return 0;
    if (sg_object_read(git_dir, commit_id, &type, &content, &content_len) != 0 || type != SG_OBJ_COMMIT)
        return -1;
    if (sg_commit_parse(content, content_len, &commit) != 0) {
        free(content);
        return -1;
    }
    free(content);

    if (sg_object_read(git_dir, commit.tree, &type, &content, &content_len) != 0 || type != SG_OBJ_TREE) {
        sg_commit_free(&commit);
        return -1;
    }
    sg_commit_free(&commit);

    if (sg_tree_parse(content, content_len, &tree) != 0) {
        free(content);
        return -1;
    }
    free(content);

    if (tree.count > 0) {
        ids = malloc(tree.count * sizeof(*ids));
        if (ids == NULL) {
            sg_tree_free(&tree);
            return -1;
        }
    }
    for (i = 0; i < tree.count; i++) {
        /* Each entry's name is expected to be the 40-hex form of its own
           sha1 (that's how keep_alive_add builds it) -- check that here too,
           not just the sha1 itself, since a mismatch would still let a chunk
           through reachability-wise but would be a bug in how the tree is
           built. */
        char hex[SG_SHA1_HEX_LEN + 1];

        sg_sha1_to_hex(tree.entries[i].sha1, hex);
        if (tree.entries[i].mode != 0100644 || strcmp(tree.entries[i].name, hex) != 0) {
            free(ids);
            sg_tree_free(&tree);
            return -1;
        }
        memcpy(ids[i], tree.entries[i].sha1, SG_SHA1_RAW_LEN);
    }
    /* sg_tree_free zeroes tree.count -- capture it first (same fix as
       chunk.c's own keep_alive_read_existing needed). */
    *count_out = tree.count;
    sg_tree_free(&tree);

    *ids_out = ids;
    return 0;
}

static int ids_contains(const unsigned char (*ids)[SG_SHA1_RAW_LEN], size_t count,
                        const unsigned char id[SG_SHA1_RAW_LEN])
{
    size_t i;

    for (i = 0; i < count; i++) {
        if (memcmp(ids[i], id, SG_SHA1_RAW_LEN) == 0)
            return 1;
    }
    return 0;
}

static int ids_has_duplicates(const unsigned char (*ids)[SG_SHA1_RAW_LEN], size_t count)
{
    size_t i, j;

    for (i = 0; i < count; i++) {
        for (j = i + 1; j < count; j++) {
            if (memcmp(ids[i], ids[j], SG_SHA1_RAW_LEN) == 0)
                return 1;
        }
    }
    return 0;
}

/* Core durability-fix test: every chunk written by sg_chunk_store_blob ends
   up in the SG_CHUNK_KEEPALIVE_REF tree, across several separate store
   calls (not just the first), with no duplicate entries even though later
   calls re-read and rebuild the whole tree from scratch each time. */
static void test_keep_alive_tree_incremental(void)
{
    char *git_dir = make_tmp_repo();
    size_t len_a = 3 * 1024 * 1024;
    size_t len_b = 2 * 1024 * 1024;
    unsigned char *data_a = malloc(len_a);
    unsigned char *data_b = malloc(len_b);
    unsigned char id_a[SG_SHA1_RAW_LEN], id_b[SG_SHA1_RAW_LEN];
    int chunked_a = -1, chunked_b = -1;
    sg_obj_type type;
    unsigned char *ptr_a_content = NULL, *ptr_b_content = NULL;
    size_t ptr_a_len = 0, ptr_b_len = 0;
    sg_chunk_pointer ptr_a = {0}, ptr_b = {0};
    unsigned char (*tree_ids)[SG_SHA1_RAW_LEN] = NULL;
    size_t tree_count = 0;
    int ok;
    size_t i;

    if (data_a == NULL || data_b == NULL) {
        fprintf(stderr, "FAIL keep-alive incremental: out of memory\n");
        failures++;
        free(data_a);
        free(data_b);
        free(git_dir);
        return;
    }
    fill_random(data_a, len_a, 71);
    fill_random(data_b, len_b, 72);

    /* Batch 1: store data_a, chunked. */
    ok = sg_chunk_store_blob(git_dir, data_a, len_a, 1024 * 1024, id_a, &chunked_a) == 0 &&
        chunked_a == 1;
    if (ok)
        ok = sg_loose_read(git_dir, id_a, &type, &ptr_a_content, &ptr_a_len) == 0 &&
            sg_chunk_pointer_parse(ptr_a_content, ptr_a_len, &ptr_a) == 1 && ptr_a.chunk_count > 1;

    /* After batch 1: every chunk of data_a must already be kept alive. */
    if (ok) {
        ok = read_keepalive_tree_ids(git_dir, &tree_ids, &tree_count) == 0 &&
            tree_count == ptr_a.chunk_count && !ids_has_duplicates(tree_ids, tree_count);
        for (i = 0; ok && i < ptr_a.chunk_count; i++) {
            if (!ids_contains(tree_ids, tree_count, ptr_a.chunk_ids[i]))
                ok = 0;
        }
        if (!ok)
            fprintf(stderr, "FAIL keep-alive incremental: batch 1 chunks not all kept alive\n");
    }
    free(tree_ids);
    tree_ids = NULL;

    /* Batch 2: store data_b, chunked -- a second, later call. */
    if (ok)
        ok = sg_chunk_store_blob(git_dir, data_b, len_b, 1024 * 1024, id_b, &chunked_b) == 0 &&
            chunked_b == 1;
    if (ok)
        ok = sg_loose_read(git_dir, id_b, &type, &ptr_b_content, &ptr_b_len) == 0 &&
            sg_chunk_pointer_parse(ptr_b_content, ptr_b_len, &ptr_b) == 1 && ptr_b.chunk_count > 1;

    /* After batch 2: chunks from BOTH batch 1 and batch 2 must be present,
       with no duplicates -- this is the "rebuild the whole tree each time,
       but stay correct and non-duplicated" requirement. */
    if (ok) {
        ok = read_keepalive_tree_ids(git_dir, &tree_ids, &tree_count) == 0 &&
            tree_count == ptr_a.chunk_count + ptr_b.chunk_count &&
            !ids_has_duplicates(tree_ids, tree_count);
        for (i = 0; ok && i < ptr_a.chunk_count; i++) {
            if (!ids_contains(tree_ids, tree_count, ptr_a.chunk_ids[i]))
                ok = 0;
        }
        for (i = 0; ok && i < ptr_b.chunk_count; i++) {
            if (!ids_contains(tree_ids, tree_count, ptr_b.chunk_ids[i]))
                ok = 0;
        }
        if (!ok)
            fprintf(stderr,
                   "FAIL keep-alive incremental: batch 1+2 chunks not all present without "
                   "duplicates\n");
    }

    /* Batch 3: re-store the exact same data_a bytes again (content-addressed,
       so this produces the exact same chunk ids as batch 1) -- must not
       grow the tree at all; this is the dedup-on-re-add requirement. */
    if (ok) {
        unsigned char id_a2[SG_SHA1_RAW_LEN];
        int chunked_a2 = -1;
        size_t tree_count2 = 0;
        unsigned char (*tree_ids2)[SG_SHA1_RAW_LEN] = NULL;

        ok = sg_chunk_store_blob(git_dir, data_a, len_a, 1024 * 1024, id_a2, &chunked_a2) == 0 &&
            chunked_a2 == 1 && memcmp(id_a2, id_a, SG_SHA1_RAW_LEN) == 0;
        if (ok)
            ok = read_keepalive_tree_ids(git_dir, &tree_ids2, &tree_count2) == 0 &&
                tree_count2 == tree_count && !ids_has_duplicates(tree_ids2, tree_count2);
        if (!ok)
            fprintf(stderr,
                   "FAIL keep-alive incremental: re-adding identical content grew the tree "
                   "(dedup not working)\n");
        free(tree_ids2);
    }

    if (ok) {
        printf("PASS keep-alive tree incremental: %zu chunks across 3 store_blob calls, all kept "
              "alive, no duplicates\n",
              tree_count);
    } else {
        failures++;
    }

    sg_chunk_pointer_free(&ptr_a);
    sg_chunk_pointer_free(&ptr_b);
    free(ptr_a_content);
    free(ptr_b_content);
    free(tree_ids);
    free(data_a);
    free(data_b);
    free(git_dir);
}


static void write_repo_config(const char *git_dir, const char *content)
{
    char path[4096];
    FILE *f;

    snprintf(path, sizeof(path), "%s/config", git_dir);
    f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "FAIL: could not write test config at %s\n", path);
        failures++;
        return;
    }
    fputs(content, f);
    fclose(f);
}

/* ---- refs/sg/chunks *deletion* tests (the second CRITICAL bug this report
   fixes): losing the keep-alive ref must be told apart from a repo that
   simply never used chunking (a plain `git clone`), via the .git/config
   marker sg_repo_mark_chunking_used/sg_repo_chunking_was_used writes/reads.
   ---- */

/* Core regression test for the fix: a repo that has genuinely chunked a
   file (so sg_chunk_store_blob already wrote the .git/config marker) whose
   SG_CHUNK_KEEPALIVE_REF is later deleted straight off disk -- exactly what
   `git update-ref -d refs/sg/chunks` does -- must fail hard (-2, with
   missing.keepalive_lost set) on every subsequent read of any chunked file
   in that repo, never silently fall back to handing back the pointer's own
   raw text as if it were the file's content. Before this fix,
   keep_alive_is_member (and thus chunk_resolve's discriminator) treated "no
   keep-alive ref" as flatly equivalent to "not a member", which made a
   pointer whose ref just went missing indistinguishable from ordinary
   content coming out of a plain, non-sg `git clone`. */
static void test_read_blob_keepalive_ref_deleted_with_marker_hard_error(void)
{
    char *git_dir = make_tmp_repo();
    size_t len = 3 * 1024 * 1024;
    unsigned char *data = malloc(len);
    unsigned char id[SG_SHA1_RAW_LEN];
    int chunked = -1;
    unsigned char *out = NULL;
    size_t out_len = 0;
    unsigned char effective[SG_SHA1_RAW_LEN];
    sg_chunk_missing_info missing;
    char keepalive_path[4096];
    int ok;

    if (data == NULL) {
        fprintf(stderr, "FAIL keepalive-ref-deleted: out of memory\n");
        failures++;
        free(git_dir);
        return;
    }
    fill_random(data, len, 91);

    ok = sg_chunk_store_blob(git_dir, data, len, 1024 * 1024, id, &chunked) == 0 && chunked == 1;

    /* sg_chunk_store_blob must have marked this repo as having used chunking
       -- that marker is exactly what this test depends on surviving the ref
       deletion below. */
    if (ok)
        ok = sg_repo_chunking_was_used(git_dir) == 1;

    /* A sanity check that the pointer is readable before we break anything --
       isolates a failure here from a failure in the actual regression being
       tested below. */
    if (ok)
        ok = sg_chunk_read_blob(git_dir, id, &out, &out_len, NULL) == 0 && out_len == len &&
            memcmp(out, data, len) == 0;
    free(out);
    out = NULL;
    out_len = 0;

    /* Delete refs/sg/chunks straight off disk -- the same effect `git
       update-ref -d refs/sg/chunks` has on a never-packed loose ref (see
       sg_ref_write_path: it always writes ref_path as git_dir/ref_path). */
    if (ok) {
        memset(&missing, 0, sizeof(missing));
        snprintf(keepalive_path, sizeof(keepalive_path), "%s/%s", git_dir, SG_CHUNK_KEEPALIVE_REF);
        ok = remove(keepalive_path) == 0;
    }

    if (ok) {
        ok = sg_chunk_read_blob(git_dir, id, &out, &out_len, &missing) == -2 &&
            missing.keepalive_lost == 1 && out == NULL;
    }
    if (ok)
        ok = sg_chunk_effective_id(git_dir, id, effective) == -2;

    if (!ok) {
        fprintf(stderr,
               "FAIL keepalive-ref-deleted: expected a hard -2/keepalive_lost error once "
               "refs/sg/chunks is gone from a repo that used chunking, not a silent fallback\n");
        failures++;
    } else {
        printf("PASS keepalive-ref-deleted: deleting refs/sg/chunks from a repo that used "
              "chunking fails closed (keepalive_lost) instead of silently returning pointer "
              "text\n");
    }

    free(out);
    free(data);
    free(git_dir);
}

/* The flip side, spelled out explicitly: a repo that never used chunking at
   all (no SG_CHUNK_KEEPALIVE_REF, no .git/config marker -- exactly what a
   real `git clone` produces) must keep treating pointer-shaped content as
   ordinary content, unaffected by the fix above. Functionally the same
   invariant test_read_blob_fake_pointer_missing_chunk already exercises, but
   named and asserted here against the specific three-way discriminator this
   report adds to chunk_resolve (ref exists / ref absent+marker / ref
   absent+no marker), so that discriminator's "no ref, no marker" branch has
   its own explicit regression coverage. */
static void test_read_blob_no_ref_no_marker_still_ordinary(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char missing_id[SG_SHA1_RAW_LEN];
    sg_chunk_pointer ptr;
    unsigned char ids[1][SG_SHA1_RAW_LEN];
    unsigned char *pointer_content = NULL;
    size_t pointer_len = 0;
    unsigned char fake_id[SG_SHA1_RAW_LEN];
    unsigned char *out = NULL;
    size_t out_len = 0;
    unsigned char effective[SG_SHA1_RAW_LEN];
    int ok;

    /* Precondition this test actually depends on: a freshly sg_repo_init'd
       repo has never called sg_chunk_store_blob, so neither the keep-alive
       ref nor the .git/config marker exist yet. */
    ok = sg_repo_chunking_was_used(git_dir) == 0;

    sg_sha1("another-nonexistent-chunk", 25, missing_id);
    memcpy(ids[0], missing_id, SG_SHA1_RAW_LEN);

    memset(ptr.original_sha1, 0xCD, SG_SHA1_RAW_LEN);
    ptr.original_size = 54321;
    ptr.chunk_ids = ids;
    ptr.chunk_count = 1;

    if (ok)
        ok = sg_chunk_pointer_format(&ptr, &pointer_content, &pointer_len) == 0;
    if (ok)
        ok = sg_loose_write(git_dir, SG_OBJ_BLOB, pointer_content, pointer_len, fake_id) == 0;
    if (ok)
        ok = sg_chunk_read_blob(git_dir, fake_id, &out, &out_len, NULL) == 0 &&
            out_len == pointer_len && memcmp(out, pointer_content, pointer_len) == 0;
    if (ok)
        ok = sg_chunk_effective_id(git_dir, fake_id, effective) == 0 &&
            memcmp(effective, fake_id, SG_SHA1_RAW_LEN) == 0;

    if (!ok) {
        fprintf(stderr,
               "FAIL no-ref-no-marker: expected pointer-shaped content in a repo that never used "
               "chunking to still fall back to ordinary content\n");
        failures++;
    } else {
        printf("PASS no-ref-no-marker: a repo with neither refs/sg/chunks nor the .git/config "
              "marker (e.g. a plain `git clone`) still treats pointer-shaped content as "
              "ordinary, unaffected by the keepalive-loss hard-failure path\n");
    }

    free(out);
    free(pointer_content);
    free(git_dir);
}

/* ---- sg_repo_mark_chunking_used / sg_repo_chunking_was_used ---- */

static void test_chunking_used_marker_roundtrip(void)
{
    char *git_dir = make_tmp_repo();
    int ok;

    ok = sg_repo_chunking_was_used(git_dir) == 0;
    if (ok)
        ok = sg_repo_mark_chunking_used(git_dir) == 0;
    if (ok)
        ok = sg_repo_chunking_was_used(git_dir) == 1;
    /* Idempotent: calling it again must not error, and (checked below) must
       not append a second stanza. */
    if (ok)
        ok = sg_repo_mark_chunking_used(git_dir) == 0;
    if (ok)
        ok = sg_repo_chunking_was_used(git_dir) == 1;

    if (ok) {
        char path[4096];
        FILE *f;
        char line[1024];
        int count = 0;

        snprintf(path, sizeof(path), "%s/config", git_dir);
        f = fopen(path, "r");
        ok = f != NULL;
        if (ok) {
            while (fgets(line, sizeof(line), f) != NULL) {
                if (strstr(line, "everchunked") != NULL)
                    count++;
            }
            fclose(f);
            ok = count == 1;
        }
    }

    if (!ok) {
        fprintf(stderr,
               "FAIL chunking-used marker roundtrip: expected unset -> mark -> set, with a "
               "second mark call staying idempotent (exactly one stanza written)\n");
        failures++;
    } else {
        printf("PASS chunking-used marker roundtrip: unset by default, set after "
              "sg_repo_mark_chunking_used, and idempotent on a second call\n");
    }

    free(git_dir);
}

/* The marker's config key must not collide with sg_repo_read_chunk_config's
   own prefix-based parsing of `chunking`/`chunkthreshold` -- see
   SG_CHUNKING_USED_KEY's doc comment in repo.c for the exact collision this
   guards against ("chunkingused" would have been misread as the `chunking`
   flag itself). Exercises both directions: existing chunking config survives
   the marker being added, and adding the marker doesn't fabricate a
   `chunking = true` that was never set. */
static void test_chunking_used_marker_no_collision_with_chunk_config(void)
{
    char *git_dir = make_tmp_repo();
    int enabled = -1;
    size_t threshold = 0;
    int ok;

    write_repo_config(git_dir, "[sg]\n\tchunking = true\n\tchunkthreshold = 1048576\n");

    ok = sg_repo_mark_chunking_used(git_dir) == 0;
    if (ok)
        ok = sg_repo_chunking_was_used(git_dir) == 1;
    if (ok)
        ok = sg_repo_read_chunk_config(git_dir, &enabled, &threshold) == 0 && enabled == 1 &&
            threshold == 1048576;

    if (!ok) {
        fprintf(stderr,
               "FAIL chunking-used marker vs chunk config: adding the marker must not disturb "
               "an existing chunking=true/chunkthreshold config\n");
        failures++;
    } else {
        printf("PASS chunking-used marker vs chunk config: marker key does not collide with "
              "sg_repo_read_chunk_config's chunking/chunkthreshold parsing\n");
    }

    /* And the reverse: chunking was never explicitly enabled here, only the
       marker was set -- sg_repo_read_chunk_config must still report it as
       disabled (default), not accidentally turned on by the marker line. */
    {
        char *git_dir2 = make_tmp_repo();
        int enabled2 = -1;
        size_t threshold2 = 0;
        int ok2;

        ok2 = sg_repo_mark_chunking_used(git_dir2) == 0;
        if (ok2)
            ok2 = sg_repo_read_chunk_config(git_dir2, &enabled2, &threshold2) == 0 &&
                enabled2 == 0 && threshold2 == SG_CHUNK_DEFAULT_THRESHOLD;

        if (!ok2) {
            fprintf(stderr,
                   "FAIL chunking-used marker vs chunk config: setting only the marker must not "
                   "make sg_repo_read_chunk_config report chunking as enabled\n");
            failures++;
        } else {
            printf("PASS chunking-used marker vs chunk config: setting only the marker leaves "
                  "chunking reported as disabled (no false-positive prefix match)\n");
        }
        free(git_dir2);
    }

    free(git_dir);
}

static void test_repo_read_chunk_config_defaults(void)
{
    char *git_dir = make_tmp_repo();
    int enabled = -1;
    size_t threshold = 0;

    /* no [sg] section at all: chunking off, default threshold */
    if (sg_repo_read_chunk_config(git_dir, &enabled, &threshold) != 0 || enabled != 0 ||
       threshold != SG_CHUNK_DEFAULT_THRESHOLD) {
        fprintf(stderr, "FAIL chunk config defaults: expected disabled + default threshold\n");
        failures++;
    } else {
        printf("PASS chunk config defaults: disabled, default threshold when [sg] is absent\n");
    }

    free(git_dir);
}

static void test_repo_read_chunk_config_enabled(void)
{
    char *git_dir = make_tmp_repo();
    int enabled = -1;
    size_t threshold = 0;

    write_repo_config(git_dir, "[core]\n\tfilemode = true\n[sg]\n\tchunking = true\n"
                              "\tchunkthreshold = 1048576\n");

    if (sg_repo_read_chunk_config(git_dir, &enabled, &threshold) != 0 || enabled != 1 ||
       threshold != 1048576) {
        fprintf(stderr, "FAIL chunk config enabled: expected enabled + threshold 1048576\n");
        failures++;
    } else {
        printf("PASS chunk config enabled: chunking=true, chunkthreshold=1048576 parsed\n");
    }

    free(git_dir);
}

static void test_repo_read_chunk_config_bad_threshold(void)
{
    char *git_dir = make_tmp_repo();
    int enabled = -1;
    size_t threshold = 0;

    write_repo_config(git_dir, "[sg]\n\tchunking = false\n\tchunkthreshold = notanumber\n");

    if (sg_repo_read_chunk_config(git_dir, &enabled, &threshold) != 0 || enabled != 0 ||
       threshold != SG_CHUNK_DEFAULT_THRESHOLD) {
        fprintf(stderr,
               "FAIL chunk config bad threshold: unparseable value should fall back to default\n");
        failures++;
    } else {
        printf("PASS chunk config bad threshold: falls back to default on unparseable value\n");
    }

    free(git_dir);
}

/* Merging a set that is already fully present must leave the keep-alive ref
   exactly where it was -- not rewrite it under a fresh commit holding an
   identical tree.

   The distinction is invisible most of the time, which is what made the
   original bug intermittent: the rebuilt commit takes its timestamp from
   time(NULL), so it only gets a different object id when the rebuild lands
   in a later second than the original. This test sleeps across that boundary
   on purpose. Without the sleep it passes whether or not the fix is present,
   which would be a test that looks like coverage and is not. */
static void test_keep_alive_merge_of_subset_does_not_move_the_ref(void)
{
    char *git_dir = make_tmp_repo();
    size_t len = 3 * 1024 * 1024;
    unsigned char *data = malloc(len);
    unsigned char blob_id[SG_SHA1_RAW_LEN];
    unsigned char before[SG_SHA1_RAW_LEN], after[SG_SHA1_RAW_LEN];
    int chunked = -1;
    int ok;

    if (data == NULL) {
        fprintf(stderr, "FAIL keep-alive subset merge: out of memory\n");
        failures++;
        free(git_dir);
        return;
    }
    fill_random(data, len, 91);

    ok = sg_chunk_store_blob(git_dir, data, len, 1024 * 1024, blob_id, &chunked) == 0 &&
        chunked == 1 && sg_ref_read_path(git_dir, SG_CHUNK_KEEPALIVE_REF, before) == 0;
    if (!ok) {
        fprintf(stderr, "FAIL keep-alive subset merge: could not set up a chunked blob\n");
        failures++;
        free(data);
        free(git_dir);
        return;
    }

    /* Cross a second boundary on purpose: the rebuilt commit would take its
       timestamp from time(NULL), so within one second it lands on the same
       object id and the bug is invisible. Without this sleep the assertion
       below passes with or without the fix -- a test that looks like
       coverage and is not. */
    sleep(1);

    /* Merge the keep-alive commit into itself. Every id it names is already
       present, so nothing is added and the ref must not move. */
    ok = sg_chunk_keepalive_merge_commit(git_dir, before) == 0 &&
        sg_ref_read_path(git_dir, SG_CHUNK_KEEPALIVE_REF, after) == 0 &&
        memcmp(before, after, SG_SHA1_RAW_LEN) == 0;
    if (!ok) {
        fprintf(stderr,
               "FAIL keep-alive subset merge: merging an already-present set moved the ref "
               "(rebuilt an identical tree under a fresh timestamp)\n");
        failures++;
    } else {
        printf("PASS keep-alive subset merge: ref unchanged across a second boundary\n");
    }

    free(data);
    free(git_dir);
}

int main(void)
{
    test_determinism();
    test_empty();
    test_small();
    test_exact_min_size();
    test_exact_max_size();
    test_over_max_size();
    test_pathological_uniform();
    test_roundtrip_reassembly();
    test_dedup();
    test_pointer_parse_valid();
    test_pointer_format_roundtrip();
    test_pointer_parse_malformed();
    test_store_blob_below_threshold();
    test_store_blob_above_threshold();
    test_read_blob_ordinary();
    test_read_blob_valid_pointer();
    test_read_blob_real_pointer_hash_mismatch();
    test_read_blob_real_pointer_missing_middle_chunk();
    test_read_blob_fake_pointer_missing_chunk();
    test_read_blob_object_not_found();
    test_read_blob_store_then_delete_one_chunk();
    test_read_blob_store_then_delete_first_chunk();
    test_keep_alive_tree_incremental();
    test_keep_alive_merge_of_subset_does_not_move_the_ref();
    test_read_blob_keepalive_ref_deleted_with_marker_hard_error();
    test_read_blob_no_ref_no_marker_still_ordinary();
    test_chunking_used_marker_roundtrip();
    test_chunking_used_marker_no_collision_with_chunk_config();
    test_repo_read_chunk_config_defaults();
    test_repo_read_chunk_config_enabled();
    test_repo_read_chunk_config_bad_threshold();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all chunk tests passed\n");
    return 0;
}
