/* sg_parse_ref_advertisement and demux_upload_pack_response are defined
   (non-static, but not part of the public sg/transport.h surface) in
   src/net/transport.c; declared here via extern to keep them unit-testable
   without a real network round trip, the same convention
   src/storage/pack.c's varint helpers use. */
#include "sg/transport.h"

#include "sg/hash.h"
#include "sg/http.h"
#include "sg/pktline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int sg_parse_ref_advertisement(const unsigned char *data, size_t len, sg_ref_adv *adv_out);
extern int demux_upload_pack_response(const unsigned char *data, size_t len, sg_buf *pack_out);

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

/* Appends a ref-advertisement line "<hex id> <name>" as its own pkt-line,
   optionally with a NUL-separated capabilities string tacked on (used only
   for the first ref, per the wire format). */
static void append_ref_line(unsigned char **buf, size_t *len, size_t *cap, const char *id_hex,
                            const char *name, const char *capabilities)
{
    char line[1024];
    size_t n;

    n = (size_t)snprintf(line, sizeof(line), "%s %s", id_hex, name);
    if (capabilities != NULL) {
        line[n++] = '\0';
        n += (size_t)snprintf(line + n, sizeof(line) - n, "%s", capabilities);
    }
    line[n++] = '\n';
    if (sg_pkt_append(buf, len, cap, line, n) != 0) {
        fprintf(stderr, "append_ref_line: out of memory\n");
        abort();
    }
}

static void build_service_header(unsigned char **buf, size_t *len, size_t *cap)
{
    if (sg_pkt_append_str(buf, len, cap, "# service=git-upload-pack\n") != 0 ||
       sg_pkt_append_flush(buf, len, cap) != 0) {
        fprintf(stderr, "build_service_header: out of memory\n");
        abort();
    }
}

static const char ID_A[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char ID_B[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static const char ID_ZERO[] = "0000000000000000000000000000000000000000";

static void test_basic_advertisement_with_symref(void)
{
    unsigned char *buf = NULL;
    size_t len = 0, cap = 0;
    sg_ref_adv adv;

    build_service_header(&buf, &len, &cap);
    append_ref_line(&buf, &len, &cap, ID_A, "refs/heads/main",
                    "side-band-64k ofs-delta symref=HEAD:refs/heads/main agent=git/2.40.0");
    append_ref_line(&buf, &len, &cap, ID_B, "refs/heads/other", NULL);
    if (sg_pkt_append_flush(&buf, &len, &cap) != 0)
        abort();

    CHECK(sg_parse_ref_advertisement(buf, len, &adv) == 0, "parse should succeed");
    CHECK(adv.count == 2, "expected 2 refs, got %zu", adv.count);
    if (adv.count == 2) {
        CHECK(strcmp(adv.refs[0].name, "refs/heads/main") == 0, "ref 0 name mismatch: %s",
             adv.refs[0].name);
        CHECK(strcmp(adv.refs[1].name, "refs/heads/other") == 0, "ref 1 name mismatch: %s",
             adv.refs[1].name);

        {
            unsigned char expected_a[SG_SHA1_RAW_LEN];

            sg_hex_to_sha1(ID_A, expected_a);
            CHECK(memcmp(adv.refs[0].id, expected_a, SG_SHA1_RAW_LEN) == 0, "ref 0 id mismatch");
        }
    }
    CHECK(adv.capabilities != NULL && strstr(adv.capabilities, "side-band-64k") != NULL,
         "capabilities should contain side-band-64k");
    CHECK(adv.head_symref != NULL && strcmp(adv.head_symref, "refs/heads/main") == 0,
         "head_symref should be refs/heads/main, got %s",
         adv.head_symref != NULL ? adv.head_symref : "(null)");

    sg_ref_adv_free(&adv);
    free(buf);
}

static void test_empty_repository_placeholder(void)
{
    unsigned char *buf = NULL;
    size_t len = 0, cap = 0;
    sg_ref_adv adv;

    build_service_header(&buf, &len, &cap);
    append_ref_line(&buf, &len, &cap, ID_ZERO, "capabilities^{}", "side-band-64k ofs-delta");
    if (sg_pkt_append_flush(&buf, &len, &cap) != 0)
        abort();

    CHECK(sg_parse_ref_advertisement(buf, len, &adv) == 0, "parse should succeed");
    CHECK(adv.count == 0, "an empty repo's placeholder ref must not count as a real ref, got %zu",
         adv.count);
    CHECK(adv.capabilities != NULL && strstr(adv.capabilities, "side-band-64k") != NULL,
         "capabilities should still be captured from the placeholder line");

    sg_ref_adv_free(&adv);
    free(buf);
}

static void test_malicious_ref_names_are_skipped(void)
{
    unsigned char *buf = NULL;
    size_t len = 0, cap = 0;
    sg_ref_adv adv;
    static const char *bad_names[] = {
        "refs/../../evil",
        "refs/heads/a/../../../x",
        "refs/heads/x\ty", /* tab is a control character */
        "notrefs/heads/x", /* must start with refs/ */
        "refs/heads/",     /* trailing slash */
        "refs/heads//x",   /* double slash */
    };
    size_t i;

    build_service_header(&buf, &len, &cap);
    /* first line carries the capabilities marker regardless of name validity */
    append_ref_line(&buf, &len, &cap, ID_A, bad_names[0], "side-band-64k");
    for (i = 1; i < sizeof(bad_names) / sizeof(bad_names[0]); i++)
        append_ref_line(&buf, &len, &cap, ID_B, bad_names[i], NULL);
    /* one valid ref mixed in, to prove good refs still get through */
    append_ref_line(&buf, &len, &cap, ID_A, "refs/heads/good", NULL);
    if (sg_pkt_append_flush(&buf, &len, &cap) != 0)
        abort();

    CHECK(sg_parse_ref_advertisement(buf, len, &adv) == 0, "parse should succeed");
    CHECK(adv.count == 1, "all malicious names should be skipped, leaving 1, got %zu", adv.count);
    if (adv.count == 1)
        CHECK(strcmp(adv.refs[0].name, "refs/heads/good") == 0,
             "the surviving ref should be refs/heads/good, got %s", adv.refs[0].name);

    sg_ref_adv_free(&adv);
    free(buf);
}

static void test_not_a_smart_http_response_is_rejected(void)
{
    unsigned char *buf = NULL;
    size_t len = 0, cap = 0;
    sg_ref_adv adv;

    /* dumb-http-style response: no "# service=..." line at all */
    if (sg_pkt_append_str(&buf, &len, &cap, "not a service line\n") != 0)
        abort();

    CHECK(sg_parse_ref_advertisement(buf, len, &adv) == -1,
         "a non-smart-HTTP response should be rejected");

    free(buf);
}

static void test_service_line_without_trailing_newline_accepted(void)
{
    unsigned char *buf = NULL;
    size_t len = 0, cap = 0;
    sg_ref_adv adv;

    /* some servers omit the trailing '\n' on the service line -- both must work */
    if (sg_pkt_append_str(&buf, &len, &cap, "# service=git-upload-pack") != 0 ||
       sg_pkt_append_flush(&buf, &len, &cap) != 0)
        abort();
    append_ref_line(&buf, &len, &cap, ID_A, "refs/heads/main", "side-band-64k");
    if (sg_pkt_append_flush(&buf, &len, &cap) != 0)
        abort();

    CHECK(sg_parse_ref_advertisement(buf, len, &adv) == 0,
         "service line without trailing newline should still parse");
    CHECK(adv.count == 1, "expected 1 ref, got %zu", adv.count);

    sg_ref_adv_free(&adv);
    free(buf);
}

static void test_ref_name_is_safe_direct(void)
{
    CHECK(sg_ref_name_is_safe("refs/heads/main") == 1, "a normal branch ref should be safe");
    CHECK(sg_ref_name_is_safe("refs/tags/v1.0") == 1, "a normal tag ref should be safe");
    CHECK(sg_ref_name_is_safe("refs/heads/feature/x") == 1, "a nested branch name should be safe");

    CHECK(sg_ref_name_is_safe("heads/main") == 0, "must start with refs/");
    CHECK(sg_ref_name_is_safe("refs/../etc/passwd") == 0, "must reject ..");
    CHECK(sg_ref_name_is_safe("refs/heads/a..b") == 0, "must reject .. anywhere");
    CHECK(sg_ref_name_is_safe("refs/heads//x") == 0, "must reject //");
    CHECK(sg_ref_name_is_safe("refs/heads/x/") == 0, "must reject trailing /");
    CHECK(sg_ref_name_is_safe("refs/heads/x y") == 0, "must reject embedded space");
    CHECK(sg_ref_name_is_safe("refs/heads/x~1") == 0, "must reject ~");
    CHECK(sg_ref_name_is_safe("refs/heads/x^") == 0, "must reject ^");
    CHECK(sg_ref_name_is_safe("refs/heads/x:y") == 0, "must reject :");
    CHECK(sg_ref_name_is_safe("refs/heads/x?y") == 0, "must reject ?");
    CHECK(sg_ref_name_is_safe("refs/heads/x*y") == 0, "must reject *");
    CHECK(sg_ref_name_is_safe("refs/heads/x[y") == 0, "must reject [");
    CHECK(sg_ref_name_is_safe("refs/heads/x\\y") == 0, "must reject backslash");
    {
        char with_control[] = "refs/heads/x\x01y";

        CHECK(sg_ref_name_is_safe(with_control) == 0, "must reject control characters");
    }
    CHECK(sg_ref_name_is_safe("") == 0, "must reject empty string");
}

/* ---- sideband-64k demux (demux_upload_pack_response) ---- */

static void test_demux_valid_pack_band_accumulates_and_returns_success(void)
{
    unsigned char *buf = NULL;
    size_t len = 0, cap = 0;
    sg_buf pack_out;
    unsigned char pkt1[] = {1, 'P', 'A', 'C', 'K'};
    unsigned char pkt2[] = {1, 0, 0, 0, 2};
    unsigned char progress_pkt[] = {2, 'h', 'i', '\n'};
    int rc;

    memset(&pack_out, 0, sizeof(pack_out));

    if (sg_pkt_append_str(&buf, &len, &cap, "NAK\n") != 0)
        abort();
    if (sg_pkt_append(&buf, &len, &cap, pkt1, sizeof(pkt1)) != 0)
        abort();
    if (sg_pkt_append(&buf, &len, &cap, progress_pkt, sizeof(progress_pkt)) != 0)
        abort();
    if (sg_pkt_append(&buf, &len, &cap, pkt2, sizeof(pkt2)) != 0)
        abort();
    if (sg_pkt_append_flush(&buf, &len, &cap) != 0)
        abort();

    rc = demux_upload_pack_response(buf, len, &pack_out);
    CHECK(rc == 0, "a well-formed sideband response should demux successfully, got rc=%d", rc);
    CHECK(pack_out.len == 8, "expected 8 accumulated pack bytes, got %zu", pack_out.len);
    CHECK(pack_out.len == 8 && memcmp(pack_out.data, "PACK\0\0\0\x02", 8) == 0,
         "accumulated pack bytes mismatch");

    sg_buf_free(&pack_out);
    free(buf);
}

static void test_demux_band3_is_fatal(void)
{
    unsigned char *buf = NULL;
    size_t len = 0, cap = 0;
    sg_buf pack_out;
    unsigned char err_pkt[] = {3, 'b', 'o', 'o', 'm'};
    int rc;

    memset(&pack_out, 0, sizeof(pack_out));

    if (sg_pkt_append(&buf, &len, &cap, err_pkt, sizeof(err_pkt)) != 0)
        abort();
    if (sg_pkt_append_flush(&buf, &len, &cap) != 0)
        abort();

    rc = demux_upload_pack_response(buf, len, &pack_out);
    CHECK(rc == -1, "a band-3 fatal error message should fail the whole demux, got rc=%d", rc);

    sg_buf_free(&pack_out);
    free(buf);
}

static void test_demux_unknown_band_after_multiplex_is_protocol_error(void)
{
    unsigned char *buf = NULL;
    size_t len = 0, cap = 0;
    sg_buf pack_out;
    unsigned char pack_pkt[] = {1, 'P', 'A', 'C', 'K'};
    unsigned char bogus_pkt[] = {4, 'x', 'x'}; /* band 4 does not exist */
    int rc;

    memset(&pack_out, 0, sizeof(pack_out));

    if (sg_pkt_append_str(&buf, &len, &cap, "NAK\n") != 0)
        abort();
    if (sg_pkt_append(&buf, &len, &cap, pack_pkt, sizeof(pack_pkt)) != 0)
        abort();
    if (sg_pkt_append(&buf, &len, &cap, bogus_pkt, sizeof(bogus_pkt)) != 0)
        abort();
    if (sg_pkt_append_flush(&buf, &len, &cap) != 0)
        abort();

    rc = demux_upload_pack_response(buf, len, &pack_out);
    CHECK(rc == -1,
         "an unknown band byte encountered after multiplexing has started must be a protocol "
         "error (spec: \"其他值 = 協定錯誤\"), got rc=%d",
         rc);

    sg_buf_free(&pack_out);
    free(buf);
}

static void test_demux_pre_multiplex_unknown_first_byte_is_tolerated(void)
{
    unsigned char *buf = NULL;
    size_t len = 0, cap = 0;
    sg_buf pack_out;
    /* before any real band byte has been seen, a packet is assumed to be a
       pre-multiplex negotiation line (NAK/ACK) and is skipped regardless of
       its first byte -- this is the existing behavior the band-byte fix
       must NOT break */
    unsigned char odd_first_pkt[] = {9, 'w', 'h', 'a', 't', 'e', 'v', 'e', 'r'};
    unsigned char pack_pkt[] = {1, 'P', 'A', 'C', 'K'};
    int rc;

    memset(&pack_out, 0, sizeof(pack_out));

    if (sg_pkt_append(&buf, &len, &cap, odd_first_pkt, sizeof(odd_first_pkt)) != 0)
        abort();
    if (sg_pkt_append(&buf, &len, &cap, pack_pkt, sizeof(pack_pkt)) != 0)
        abort();
    if (sg_pkt_append_flush(&buf, &len, &cap) != 0)
        abort();

    rc = demux_upload_pack_response(buf, len, &pack_out);
    CHECK(rc == 0,
         "a pre-multiplex packet with an unexpected first byte should still be tolerated, got "
         "rc=%d",
         rc);
    CHECK(pack_out.len == 4, "expected the band-1 packet's payload to still be accumulated, got %zu",
         pack_out.len);

    sg_buf_free(&pack_out);
    free(buf);
}

int main(void)
{
    test_basic_advertisement_with_symref();
    test_empty_repository_placeholder();
    test_malicious_ref_names_are_skipped();
    test_not_a_smart_http_response_is_rejected();
    test_service_line_without_trailing_newline_accepted();
    test_ref_name_is_safe_direct();

    test_demux_valid_pack_band_accumulates_and_returns_success();
    test_demux_band3_is_fatal();
    test_demux_unknown_band_after_multiplex_is_protocol_error();
    test_demux_pre_multiplex_unknown_first_byte_is_tolerated();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all refadv tests passed\n");
    return 0;
}
