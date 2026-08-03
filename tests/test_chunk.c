#include "sg/chunk.h"

#include "sg/hash.h"
#include "sg/loose.h"
#include "sg/object.h"
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
        ok = sg_chunk_read_blob(git_dir, id, &out, &out_len) == 0 && out_len == sizeof(data) &&
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
        ok = sg_chunk_read_blob(git_dir, id, &out, &out_len) == 0 && out_len == len &&
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

/* A pointer blob that is well-formed per sg_chunk_pointer_parse (its
   chunk_ids do reference real, readable chunk blobs) but whose declared
   original_sha1 is a lie -- doesn't match sg_object_hash of the reassembled
   bytes. This must be treated as a "fake pointer": sg_chunk_read_blob/
   sg_chunk_effective_id must fall back to the pointer blob's own raw bytes
   (never hand back the reassembled-but-unverified content). */
static void test_read_blob_fake_pointer_bad_hash(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char chunk0[SG_SHA1_RAW_LEN], chunk1[SG_SHA1_RAW_LEN];
    unsigned char data0[100], data1[100];
    sg_chunk_pointer ptr;
    unsigned char ids[2][SG_SHA1_RAW_LEN];
    unsigned char *pointer_content = NULL;
    size_t pointer_len = 0;
    unsigned char fake_id[SG_SHA1_RAW_LEN];
    unsigned char *out = NULL;
    size_t out_len = 0;
    unsigned char effective[SG_SHA1_RAW_LEN];
    int ok;

    fill_random(data0, sizeof(data0), 31);
    fill_random(data1, sizeof(data1), 32);
    if (sg_loose_write(git_dir, SG_OBJ_BLOB, data0, sizeof(data0), chunk0) != 0 ||
       sg_loose_write(git_dir, SG_OBJ_BLOB, data1, sizeof(data1), chunk1) != 0) {
        fprintf(stderr, "FAIL fake pointer (bad hash): setup failed writing chunk blobs\n");
        failures++;
        free(git_dir);
        return;
    }
    memcpy(ids[0], chunk0, SG_SHA1_RAW_LEN);
    memcpy(ids[1], chunk1, SG_SHA1_RAW_LEN);

    /* Bogus original_sha1 (all 0xEE), not the real hash of data0||data1: the
       chunks themselves are all readable, so reassembly succeeds, but the
       post-reassembly hash check must still catch this and reject it. */
    memset(ptr.original_sha1, 0xEE, SG_SHA1_RAW_LEN);
    ptr.original_size = sizeof(data0) + sizeof(data1);
    ptr.chunk_ids = ids;
    ptr.chunk_count = 2;

    ok = sg_chunk_pointer_format(&ptr, &pointer_content, &pointer_len) == 0;
    if (ok)
        ok = sg_loose_write(git_dir, SG_OBJ_BLOB, pointer_content, pointer_len, fake_id) == 0;
    if (ok)
        ok = sg_chunk_read_blob(git_dir, fake_id, &out, &out_len) == 0 &&
            out_len == pointer_len && memcmp(out, pointer_content, pointer_len) == 0;
    if (ok)
        ok = sg_chunk_effective_id(git_dir, fake_id, effective) == 0 &&
            memcmp(effective, fake_id, SG_SHA1_RAW_LEN) == 0;

    if (!ok) {
        fprintf(stderr, "FAIL fake pointer (bad hash): expected fallback to raw pointer bytes\n");
        failures++;
    } else {
        printf("PASS fake pointer (hash mismatch): treated as ordinary content, not reassembled\n");
    }

    free(out);
    free(pointer_content);
    free(git_dir);
}

/* A pointer blob that parses fine but references a chunk id that doesn't
   exist as an object at all -- must also fall back to raw content, same as
   the hash-mismatch case above. */
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
        ok = sg_chunk_read_blob(git_dir, fake_id, &out, &out_len) == 0 &&
            out_len == pointer_len && memcmp(out, pointer_content, pointer_len) == 0;
    if (ok)
        ok = sg_chunk_effective_id(git_dir, fake_id, effective) == 0 &&
            memcmp(effective, fake_id, SG_SHA1_RAW_LEN) == 0;

    if (!ok) {
        fprintf(stderr, "FAIL fake pointer (missing chunk): expected fallback to raw pointer bytes\n");
        failures++;
    } else {
        printf("PASS fake pointer (missing chunk object): treated as ordinary content\n");
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

    if (sg_chunk_read_blob(git_dir, bogus_id, &out, &out_len) != -1 ||
       sg_chunk_effective_id(git_dir, bogus_id, effective) != -1) {
        fprintf(stderr, "FAIL read_blob/effective_id: expected -1 for a nonexistent object\n");
        failures++;
    } else {
        printf("PASS read_blob/effective_id: -1 returned when underlying object is missing\n");
    }

    free(out);
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
    test_read_blob_fake_pointer_bad_hash();
    test_read_blob_fake_pointer_missing_chunk();
    test_read_blob_object_not_found();
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
