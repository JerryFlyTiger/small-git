#include "sg/object.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

static void expect_hash(sg_obj_type type, const void *content, size_t len, const char *expected_hex)
{
    unsigned char id[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];

    sg_object_hash(type, content, len, id);
    sg_sha1_to_hex(id, hex);
    CHECK(strcmp(hex, expected_hex) == 0, "expected %s got %s", expected_hex, hex);
}

/* known-good vectors produced with the real `git hash-object`/`git mktree`/`git commit-tree` */
static void test_blob_vectors(void)
{
    expect_hash(SG_OBJ_BLOB, "hello\n", 6, "ce013625030ba8dba906f756967f9e9ca394464a");
    expect_hash(SG_OBJ_BLOB, "world", 5, "04fea06420ca60892f73becee3614f6d023a4b7f");
}

static void test_blob_roundtrip(void)
{
    unsigned char *formatted;
    size_t formatted_len;
    sg_object obj;
    const char *content = "some blob content\nwith a newline";
    size_t content_len = strlen(content);

    CHECK(sg_object_format(SG_OBJ_BLOB, content, content_len, &formatted, &formatted_len) == 0,
         "format failed");
    CHECK(sg_object_parse(formatted, formatted_len, &obj) == 0, "parse failed");
    CHECK(obj.type == SG_OBJ_BLOB, "wrong type");
    CHECK(obj.content_len == content_len, "wrong content_len");
    CHECK(memcmp(obj.content, content, content_len) == 0, "content mismatch");
    free(formatted);
}

static unsigned char *hex_raw(const char *hex, unsigned char *out)
{
    if (sg_hex_to_sha1(hex, out) != 0) {
        fprintf(stderr, "bad hex vector in test: %s\n", hex);
        exit(1);
    }
    return out;
}

static void test_tree_vectors(void)
{
    unsigned char blob1[SG_SHA1_RAW_LEN], blob2[SG_SHA1_RAW_LEN], subtree[SG_SHA1_RAW_LEN];
    sg_tree_entry entries[3];
    unsigned char *out;
    size_t out_len;
    unsigned char id[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];
    sg_tree parsed;

    hex_raw("ce013625030ba8dba906f756967f9e9ca394464a", blob1);
    hex_raw("04fea06420ca60892f73becee3614f6d023a4b7f", blob2);
    hex_raw("9fa6a6d71d42445400aef1e5fda85a9cd64eb517", subtree);

    /* deliberately out of order, to exercise sg_tree_serialize's own sort,
       including the directory-as-if-trailing-slash rule ("b" vs "b.txt") */
    entries[0].mode = 0100755;
    entries[0].name = (char *)"exec.sh";
    memcpy(entries[0].sha1, blob2, SG_SHA1_RAW_LEN);

    entries[1].mode = 040000;
    entries[1].name = (char *)"b";
    memcpy(entries[1].sha1, subtree, SG_SHA1_RAW_LEN);

    entries[2].mode = 0100644;
    entries[2].name = (char *)"b.txt";
    memcpy(entries[2].sha1, blob1, SG_SHA1_RAW_LEN);

    CHECK(sg_tree_serialize(entries, 3, &out, &out_len) == 0, "serialize failed");

    sg_object_hash(SG_OBJ_TREE, out, out_len, id);
    sg_sha1_to_hex(id, hex);
    CHECK(strcmp(hex, "3e8e527c1dd03f20265760d7c4aee5c70ba791c2") == 0,
         "tree hash mismatch, got %s", hex);

    CHECK(sg_tree_parse(out, out_len, &parsed) == 0, "tree parse failed");
    CHECK(parsed.count == 3, "expected 3 entries, got %zu", parsed.count);
    if (parsed.count == 3) {
        CHECK(strcmp(parsed.entries[0].name, "b.txt") == 0, "entry0 name %s",
             parsed.entries[0].name);
        CHECK(parsed.entries[0].mode == 0100644, "entry0 mode %o", parsed.entries[0].mode);
        CHECK(memcmp(parsed.entries[0].sha1, blob1, SG_SHA1_RAW_LEN) == 0, "entry0 sha1");

        CHECK(strcmp(parsed.entries[1].name, "b") == 0, "entry1 name %s", parsed.entries[1].name);
        CHECK(parsed.entries[1].mode == 040000, "entry1 mode %o", parsed.entries[1].mode);
        CHECK((parsed.entries[1].mode & S_IFMT) == S_IFDIR, "entry1 should be a directory");

        CHECK(strcmp(parsed.entries[2].name, "exec.sh") == 0, "entry2 name %s",
             parsed.entries[2].name);
        CHECK(parsed.entries[2].mode == 0100755, "entry2 mode %o", parsed.entries[2].mode);
    }

    sg_tree_free(&parsed);
    free(out);
}

static void test_commit_vectors(void)
{
    sg_commit commit;
    unsigned char *out;
    size_t out_len;
    unsigned char id[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];
    sg_commit parsed;

    memset(&commit, 0, sizeof(commit));
    hex_raw("3e8e527c1dd03f20265760d7c4aee5c70ba791c2", commit.tree);
    commit.parents = NULL;
    commit.parent_count = 0;
    commit.author_name = (char *)"Test Author";
    commit.author_email = (char *)"author@example.com";
    commit.author_time = 1700000000;
    strcpy(commit.author_tz, "+0800");
    commit.committer_name = (char *)"Test Committer";
    commit.committer_email = (char *)"committer@example.com";
    commit.committer_time = 1700000100;
    strcpy(commit.committer_tz, "+0000");
    commit.message = (char *)"Initial commit\n";

    CHECK(sg_commit_serialize(&commit, &out, &out_len) == 0, "serialize failed");
    sg_object_hash(SG_OBJ_COMMIT, out, out_len, id);
    sg_sha1_to_hex(id, hex);
    CHECK(strcmp(hex, "9a332327433eafb975c37791fa45884e794e990a") == 0,
         "commit hash mismatch, got %s", hex);

    CHECK(sg_commit_parse(out, out_len, &parsed) == 0, "commit parse failed");
    CHECK(memcmp(parsed.tree, commit.tree, SG_SHA1_RAW_LEN) == 0, "tree mismatch");
    CHECK(parsed.parent_count == 0, "expected no parents, got %zu", parsed.parent_count);
    CHECK(strcmp(parsed.author_name, "Test Author") == 0, "author_name %s", parsed.author_name);
    CHECK(strcmp(parsed.author_email, "author@example.com") == 0, "author_email %s",
         parsed.author_email);
    CHECK(parsed.author_time == 1700000000, "author_time %lld", parsed.author_time);
    CHECK(strcmp(parsed.author_tz, "+0800") == 0, "author_tz %s", parsed.author_tz);
    CHECK(strcmp(parsed.committer_name, "Test Committer") == 0, "committer_name %s",
         parsed.committer_name);
    CHECK(strcmp(parsed.committer_tz, "+0000") == 0, "committer_tz %s", parsed.committer_tz);
    CHECK(strcmp(parsed.message, "Initial commit\n") == 0, "message %s", parsed.message);

    sg_commit_free(&parsed);
    free(out);
}

static void test_commit_with_parents_roundtrip(void)
{
    sg_commit commit;
    unsigned char parents[2][SG_SHA1_RAW_LEN];
    unsigned char *out;
    size_t out_len;
    sg_commit parsed;

    memset(&commit, 0, sizeof(commit));
    hex_raw("3e8e527c1dd03f20265760d7c4aee5c70ba791c2", commit.tree);
    hex_raw("ce013625030ba8dba906f756967f9e9ca394464a", parents[0]);
    hex_raw("04fea06420ca60892f73becee3614f6d023a4b7f", parents[1]);
    commit.parents = parents;
    commit.parent_count = 2;
    commit.author_name = (char *)"A";
    commit.author_email = (char *)"a@example.com";
    commit.author_time = 1;
    strcpy(commit.author_tz, "+0000");
    commit.committer_name = (char *)"C";
    commit.committer_email = (char *)"c@example.com";
    commit.committer_time = 2;
    strcpy(commit.committer_tz, "-0700");
    commit.message = (char *)"Merge\n";

    CHECK(sg_commit_serialize(&commit, &out, &out_len) == 0, "serialize failed");
    CHECK(sg_commit_parse(out, out_len, &parsed) == 0, "parse failed");
    CHECK(parsed.parent_count == 2, "expected 2 parents, got %zu", parsed.parent_count);
    if (parsed.parent_count == 2) {
        CHECK(memcmp(parsed.parents[0], parents[0], SG_SHA1_RAW_LEN) == 0, "parent0 mismatch");
        CHECK(memcmp(parsed.parents[1], parents[1], SG_SHA1_RAW_LEN) == 0, "parent1 mismatch");
    }
    CHECK(strcmp(parsed.committer_tz, "-0700") == 0, "negative tz roundtrip");

    sg_commit_free(&parsed);
    free(out);
}

/* Each row reproduces a case verified directly against real git's
   `--cleanup=whitespace` (the default for `git commit -m`) by committing the
   input and inspecting the raw bytes with `git cat-file commit` -- see the
   table in the phase12 task writeup. */
static void expect_cleanup(const char *input, const char *expected, const char *label)
{
    char *out = NULL;
    int rc = sg_message_cleanup(input, &out);

    CHECK(rc == 0, "%s: sg_message_cleanup returned %d", label, rc);
    if (rc == 0)
        CHECK(strcmp(out, expected) == 0, "%s: got %s expected %s", label, out, expected);
    free(out);
}

static void test_message_cleanup(void)
{
    expect_cleanup("x", "x\n", "plain");
    expect_cleanup("  x  ", "  x\n", "leading whitespace preserved, trailing stripped");
    expect_cleanup("x\n", "x\n", "already-terminated message unchanged");
    expect_cleanup("x\n\n\n", "x\n", "trailing blank lines removed");
    expect_cleanup("\n\nx", "x\n", "leading blank lines removed");
    expect_cleanup("a\n\nb", "a\n\nb\n", "single interior blank line preserved");
    expect_cleanup("a\n\n\nb", "a\n\nb\n", "consecutive blank lines collapsed to one");
    expect_cleanup("a   \nb", "a\nb\n", "trailing whitespace stripped per line");
    expect_cleanup("\tx\t", "\tx\n", "leading tab preserved, trailing tab stripped");
    expect_cleanup("", "", "empty input normalizes to empty output");
    expect_cleanup("   \n\n  \n", "", "whitespace-only input normalizes to empty output");

    /* \v (0x0B) and \f (0x0C) are NOT trailing whitespace to real git's
       --cleanup=whitespace, unlike space/tab/\r -- verified directly against
       git 2.55.0. A message consisting of nothing but \f must NOT normalize
       to empty (it would if \f were treated as strippable whitespace, which
       would then make sg_cmd_commit wrongly reject it as an empty message). */
    expect_cleanup("a\f", "a\f\n", "trailing form feed preserved, not stripped");
    expect_cleanup("a\v", "a\v\n", "trailing vertical tab preserved, not stripped");
    expect_cleanup("\f", "\f\n", "message consisting only of \\f is not treated as empty");
}

static void test_parse_header(void)
{
    const char *good = "blob 33\0content-does-not-matter-here";
    sg_obj_type type;
    size_t header_len;
    size_t declared_size;

    /* memchr-based length below has to cover the embedded NUL, so build the
       buffer explicitly rather than relying on strlen() stopping early */
    unsigned char buf[64];
    size_t buf_len = 8 + 29; /* "blob 33\0" (8 bytes) + 29 bytes of arbitrary content */

    memcpy(buf, good, 8);
    memset(buf + 8, 'x', 29);

    CHECK(sg_object_parse_header(buf, buf_len, &type, &header_len, &declared_size) == 0,
         "well-formed header should parse");
    CHECK(type == SG_OBJ_BLOB, "wrong type");
    CHECK(header_len == 8, "wrong header_len: got %zu", header_len);
    CHECK(declared_size == 33, "wrong declared_size: got %zu", declared_size);

    /* header parsing only needs the header itself present -- unlike
       sg_object_parse(), it must NOT require the declared content to
       actually follow in full (that's the whole point: it lets a caller
       learn the declared size from a short prefix before deciding how much
       more to decompress). */
    CHECK(sg_object_parse_header(buf, 8, &type, &header_len, &declared_size) == 0,
         "header-only prefix (no content) should still parse");
    CHECK(declared_size == 33, "declared_size should still be 33 from header-only prefix");

    /* malformed: no space separator */
    {
        const unsigned char no_space[] = "blob33\0xxxx";
        CHECK(sg_object_parse_header(no_space, sizeof(no_space) - 1, &type, &header_len,
                                     &declared_size) != 0,
             "missing space separator should fail");
    }

    /* malformed: unknown type name */
    {
        const unsigned char bad_type[] = "bogus 5\0xxxxx";
        CHECK(sg_object_parse_header(bad_type, sizeof(bad_type) - 1, &type, &header_len,
                                     &declared_size) != 0,
             "unknown type name should fail");
    }

    /* malformed: no NUL terminator within the buffer given */
    {
        const unsigned char no_nul[] = "blob 5xxxxx";
        CHECK(sg_object_parse_header(no_nul, sizeof(no_nul) - 1, &type, &header_len,
                                     &declared_size) != 0,
             "missing NUL should fail");
    }

    /* malformed: non-numeric size field */
    {
        const unsigned char bad_size[] = "blob abc\0xxxxx";
        CHECK(sg_object_parse_header(bad_size, sizeof(bad_size) - 1, &type, &header_len,
                                     &declared_size) != 0,
             "non-numeric size should fail");
    }
}

/* sg_object_parse() must reject a stream whose declared size doesn't match the
   actual content length that follows the header -- both when there's more
   content than declared and when there's less. This guards against a loose
   object whose header lies about its size, whichever direction the lie goes
   (a corrupt/hostile stream that decompresses to more or fewer bytes than
   its own header claims). */
static void test_parse_size_mismatch(void)
{
    sg_object obj;

    /* declared size (5) is larger than the actual content that follows (3
       bytes) -- must fail, not silently accept the short content. */
    {
        const unsigned char data[] = "blob 5\0xyz";
        CHECK(sg_object_parse(data, sizeof(data) - 1, &obj) != 0,
             "declared-larger-than-actual should fail");
    }

    /* declared size (2) is smaller than the actual content that follows (5
       bytes) -- must fail, not silently truncate. */
    {
        const unsigned char data[] = "blob 2\0abcde";
        CHECK(sg_object_parse(data, sizeof(data) - 1, &obj) != 0,
             "declared-smaller-than-actual should fail");
    }

    /* exact match must still succeed */
    {
        const unsigned char data[] = "blob 3\0abc";
        CHECK(sg_object_parse(data, sizeof(data) - 1, &obj) == 0, "exact match should succeed");
        CHECK(obj.content_len == 3, "wrong content_len");
    }
}

int main(void)
{
    test_blob_vectors();
    test_blob_roundtrip();
    test_tree_vectors();
    test_commit_vectors();
    test_commit_with_parents_roundtrip();
    test_message_cleanup();
    test_parse_header();
    test_parse_size_mismatch();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all object tests passed\n");
    return 0;
}
