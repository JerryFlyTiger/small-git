#include "sg/object.h"

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

static unsigned char *hex_raw(const char *hex, unsigned char *out)
{
    sg_hex_to_sha1(hex, out);
    return out;
}

/* Known-good vector produced with real `git hash-object -t tag --stdin` fed
   the exact byte stream below -- pins sg_tag_serialize's field order/format
   against real git, not just against sg_tag_parse's own inverse. */
static void test_tag_vector(void)
{
    sg_tag tag;
    unsigned char *out;
    size_t out_len;
    unsigned char id[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];

    memset(&tag, 0, sizeof(tag));
    hex_raw("3e8e527c1dd03f20265760d7c4aee5c70ba791c2", tag.object);
    tag.object_type = SG_OBJ_COMMIT;
    tag.tag_name = (char *)"v1.0";
    tag.tagger_name = (char *)"Test Tagger";
    tag.tagger_email = (char *)"tagger@example.com";
    tag.tagger_time = 1700000200;
    strcpy(tag.tagger_tz, "+0530");
    tag.message = (char *)"Release message\n";

    CHECK(sg_tag_serialize(&tag, &out, &out_len) == 0, "serialize failed");
    sg_object_hash(SG_OBJ_TAG, out, out_len, id);
    sg_sha1_to_hex(id, hex);
    CHECK(strcmp(hex, "7b67b85b2f3cff50ec7bc644ee5fcc028a428b32") == 0, "tag hash mismatch, got %s",
         hex);

    free(out);
}

static void test_tag_roundtrip(void)
{
    sg_tag tag;
    unsigned char *out;
    size_t out_len;
    sg_tag parsed;

    memset(&tag, 0, sizeof(tag));
    hex_raw("ce013625030ba8dba906f756967f9e9ca394464a", tag.object);
    tag.object_type = SG_OBJ_COMMIT;
    tag.tag_name = (char *)"release/v2.0";
    tag.tagger_name = (char *)"A Tagger";
    tag.tagger_email = (char *)"a@example.com";
    tag.tagger_time = 42;
    strcpy(tag.tagger_tz, "-0700");
    tag.message = (char *)"multi\nline\nmessage\n";

    CHECK(sg_tag_serialize(&tag, &out, &out_len) == 0, "serialize failed");
    CHECK(sg_tag_parse(out, out_len, &parsed) == 0, "parse failed");

    CHECK(memcmp(parsed.object, tag.object, SG_SHA1_RAW_LEN) == 0, "object mismatch");
    CHECK(parsed.object_type == SG_OBJ_COMMIT, "object_type mismatch, got %d",
         (int)parsed.object_type);
    CHECK(strcmp(parsed.tag_name, "release/v2.0") == 0, "tag_name %s", parsed.tag_name);
    CHECK(strcmp(parsed.tagger_name, "A Tagger") == 0, "tagger_name %s", parsed.tagger_name);
    CHECK(strcmp(parsed.tagger_email, "a@example.com") == 0, "tagger_email %s",
         parsed.tagger_email);
    CHECK(parsed.tagger_time == 42, "tagger_time %lld", parsed.tagger_time);
    CHECK(strcmp(parsed.tagger_tz, "-0700") == 0, "tagger_tz %s (negative tz roundtrip)",
         parsed.tagger_tz);
    CHECK(strcmp(parsed.message, "multi\nline\nmessage\n") == 0, "message %s", parsed.message);

    sg_tag_free(&parsed);
    free(out);
}

/* sg_tag_parse must reject malformed content instead of silently accepting a
   partial parse -- e.g. a stream with no blank-line separator before the
   message, or a bogus "type" field. */
static void test_tag_parse_rejects_malformed(void)
{
    static const char no_blank_line[] =
        "object 3e8e527c1dd03f20265760d7c4aee5c70ba791c2\n"
        "type commit\n"
        "tag v1\n"
        "tagger A <a@example.com> 1 +0000\n"
        "no blank line here";
    static const char bad_type[] =
        "object 3e8e527c1dd03f20265760d7c4aee5c70ba791c2\n"
        "type not-a-type\n"
        "tag v1\n"
        "tagger A <a@example.com> 1 +0000\n"
        "\n"
        "msg\n";
    sg_tag parsed;

    CHECK(sg_tag_parse((const unsigned char *)no_blank_line, strlen(no_blank_line), &parsed) != 0,
         "expected rejection of a tag with no blank-line separator");
    CHECK(sg_tag_parse((const unsigned char *)bad_type, strlen(bad_type), &parsed) != 0,
         "expected rejection of a tag with an unrecognized type field");
}

/* Phase 61: sg_tag_parse must skip unknown trailing headers up to the first
   blank line, same rule and same reasoning as sg_commit_parse. */
static void test_tag_unknown_header(void)
{
    const char *raw = "object 3e8e527c1dd03f20265760d7c4aee5c70ba791c2\n"
                       "type commit\n"
                       "tag v1\n"
                       "tagger A <a@example.com> 1700000000 +0000\n"
                       "gpgsig -----BEGIN PGP SIGNATURE-----\n"
                       " fakefakefake\n"
                       " -----END PGP SIGNATURE-----\n"
                       "\n"
                       "signed tag message\n";
    sg_tag parsed;
    int rc = sg_tag_parse((const unsigned char *)raw, strlen(raw), &parsed);

    CHECK(rc == 0, "tag with gpgsig header should parse, got %d", rc);
    if (rc == 0) {
        CHECK(strcmp(parsed.tag_name, "v1") == 0, "tag_name %s", parsed.tag_name);
        CHECK(strcmp(parsed.tagger_name, "A") == 0, "tagger_name %s", parsed.tagger_name);
        CHECK(strcmp(parsed.message, "signed tag message\n") == 0, "message %s", parsed.message);
        sg_tag_free(&parsed);
    }
}

/* Negative rows, same reasoning as sg_commit_parse's: real git refuses to
   create these, so tolerating unknown TRAILING headers must not relax the
   ORDER of the known ones. */
static void test_tag_unknown_header_before_object_fails(void)
{
    const char *raw = "gpgsig fake\n"
                       "object 3e8e527c1dd03f20265760d7c4aee5c70ba791c2\n"
                       "type commit\n"
                       "tag v1\n"
                       "tagger A <a@example.com> 1700000000 +0000\n"
                       "\n"
                       "msg\n";
    sg_tag parsed;

    CHECK(sg_tag_parse((const unsigned char *)raw, strlen(raw), &parsed) != 0,
         "a header before object must still fail to parse");
}

static void test_tag_missing_tagger_fails(void)
{
    const char *raw = "object 3e8e527c1dd03f20265760d7c4aee5c70ba791c2\n"
                       "type commit\n"
                       "tag v1\n"
                       "gpgsig fake\n"
                       "\n"
                       "msg\n";
    sg_tag parsed;

    CHECK(sg_tag_parse((const unsigned char *)raw, strlen(raw), &parsed) != 0,
         "a tag missing tagger must still fail to parse");
}

int main(void)
{
    test_tag_vector();
    test_tag_roundtrip();
    test_tag_parse_rejects_malformed();
    test_tag_unknown_header();
    test_tag_unknown_header_before_object_fails();
    test_tag_missing_tagger_fails();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all tag tests passed\n");
    return 0;
}
