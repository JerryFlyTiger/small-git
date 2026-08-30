/* sg_parse_push_report_status and sg_demux_sideband_response are defined
   (non-static, but not part of the public sg/transport.h surface) in
   src/net/transport.c; declared here via extern to keep them unit-testable
   without a real network round trip, the same convention
   tests/test_refadv.c uses for the ref-advertisement/demux parsers. */
#include "sg/transport.h"

#include "sg/hash.h"
#include "sg/http.h"
#include "sg/pktline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int sg_parse_push_report_status(const unsigned char *data, size_t len, sg_push_report *out);
extern int sg_demux_sideband_response(const unsigned char *data, size_t len, sg_buf *out_band1);

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

static void append_line(unsigned char **buf, size_t *len, size_t *cap, const char *s)
{
    if (sg_pkt_append_str(buf, len, cap, s) != 0) {
        fprintf(stderr, "append_line: out of memory\n");
        abort();
    }
}

/* ---- plain (non side-band) report-status ---- */

static void test_all_ok(void)
{
    unsigned char *buf = NULL;
    size_t len = 0, cap = 0;
    sg_push_report report;

    append_line(&buf, &len, &cap, "unpack ok\n");
    append_line(&buf, &len, &cap, "ok refs/heads/main\n");
    if (sg_pkt_append_flush(&buf, &len, &cap) != 0)
        abort();

    CHECK(sg_parse_push_report_status(buf, len, &report) == 0, "parse should succeed");
    CHECK(report.unpack_ok == 1, "unpack should be ok");
    CHECK(report.unpack_error == NULL, "unpack_error should be NULL on success");
    CHECK(report.ref_count == 1, "expected 1 ref result, got %zu", report.ref_count);
    if (report.ref_count == 1) {
        CHECK(report.refs[0].ok == 1, "ref should be ok");
        CHECK(strcmp(report.refs[0].ref_name, "refs/heads/main") == 0, "ref name mismatch: %s",
             report.refs[0].ref_name);
        CHECK(report.refs[0].message == NULL, "message should be NULL for an ok ref");
    }

    sg_push_report_free(&report);
    free(buf);
}

static void test_unpack_failure(void)
{
    unsigned char *buf = NULL;
    size_t len = 0, cap = 0;
    sg_push_report report;

    append_line(&buf, &len, &cap, "unpack index-pack failed\n");
    if (sg_pkt_append_flush(&buf, &len, &cap) != 0)
        abort();

    CHECK(sg_parse_push_report_status(buf, len, &report) == 0, "parse should succeed");
    CHECK(report.unpack_ok == 0, "unpack should not be ok");
    CHECK(report.unpack_error != NULL && strcmp(report.unpack_error, "index-pack failed") == 0,
         "unpack_error mismatch: %s", report.unpack_error != NULL ? report.unpack_error : "(null)");
    CHECK(report.ref_count == 0, "no ref lines should follow an unpack failure, got %zu",
         report.ref_count);

    sg_push_report_free(&report);
    free(buf);
}

static void test_single_ref_ng_with_reason(void)
{
    unsigned char *buf = NULL;
    size_t len = 0, cap = 0;
    sg_push_report report;

    append_line(&buf, &len, &cap, "unpack ok\n");
    append_line(&buf, &len, &cap, "ng refs/heads/main non-fast-forward\n");
    if (sg_pkt_append_flush(&buf, &len, &cap) != 0)
        abort();

    CHECK(sg_parse_push_report_status(buf, len, &report) == 0, "parse should succeed");
    CHECK(report.unpack_ok == 1, "unpack should still be ok even if a ref is rejected");
    CHECK(report.ref_count == 1, "expected 1 ref result, got %zu", report.ref_count);
    if (report.ref_count == 1) {
        CHECK(report.refs[0].ok == 0, "ref should be rejected (ng)");
        CHECK(strcmp(report.refs[0].ref_name, "refs/heads/main") == 0, "ref name mismatch: %s",
             report.refs[0].ref_name);
        CHECK(report.refs[0].message != NULL && strcmp(report.refs[0].message, "non-fast-forward") == 0,
             "ng reason mismatch: %s", report.refs[0].message != NULL ? report.refs[0].message : "(null)");
    }

    sg_push_report_free(&report);
    free(buf);
}

static void test_multiple_refs_mixed(void)
{
    unsigned char *buf = NULL;
    size_t len = 0, cap = 0;
    sg_push_report report;

    append_line(&buf, &len, &cap, "unpack ok\n");
    append_line(&buf, &len, &cap, "ok refs/heads/main\n");
    append_line(&buf, &len, &cap, "ng refs/heads/other some reason here\n");
    if (sg_pkt_append_flush(&buf, &len, &cap) != 0)
        abort();

    CHECK(sg_parse_push_report_status(buf, len, &report) == 0, "parse should succeed");
    CHECK(report.ref_count == 2, "expected 2 ref results, got %zu", report.ref_count);
    if (report.ref_count == 2) {
        CHECK(report.refs[0].ok == 1, "first ref should be ok");
        CHECK(report.refs[1].ok == 0, "second ref should be ng");
        CHECK(report.refs[1].message != NULL && strcmp(report.refs[1].message, "some reason here") == 0,
             "ng reason mismatch: %s", report.refs[1].message != NULL ? report.refs[1].message : "(null)");
    }

    sg_push_report_free(&report);
    free(buf);
}

static void test_malformed_first_line_rejected(void)
{
    unsigned char *buf = NULL;
    size_t len = 0, cap = 0;
    sg_push_report report;

    append_line(&buf, &len, &cap, "not-unpack-at-all\n");
    if (sg_pkt_append_flush(&buf, &len, &cap) != 0)
        abort();

    CHECK(sg_parse_push_report_status(buf, len, &report) == -1,
         "a response not starting with 'unpack ' should be rejected");

    free(buf);
}

/* ---- side-band-64k-wrapped report-status ---- */

static void test_side_band_wrapped_report_status(void)
{
    unsigned char *inner = NULL;
    size_t inner_len = 0, inner_cap = 0;
    unsigned char *outer = NULL;
    size_t outer_len = 0, outer_cap = 0;
    sg_buf band1;
    sg_push_report report;
    unsigned char *band_pkt;

    append_line(&inner, &inner_len, &inner_cap, "unpack ok\n");
    append_line(&inner, &inner_len, &inner_cap, "ok refs/heads/main\n");
    if (sg_pkt_append_flush(&inner, &inner_len, &inner_cap) != 0)
        abort();

    /* re-wrap the whole inner pkt-line stream as a single band-1 packet,
       same shape real git's side-band-64k report-status response uses */
    band_pkt = malloc(inner_len + 1);
    if (band_pkt == NULL)
        abort();
    band_pkt[0] = 1;
    memcpy(band_pkt + 1, inner, inner_len);
    if (sg_pkt_append(&outer, &outer_len, &outer_cap, band_pkt, inner_len + 1) != 0)
        abort();
    if (sg_pkt_append_flush(&outer, &outer_len, &outer_cap) != 0)
        abort();
    free(band_pkt);

    memset(&band1, 0, sizeof(band1));
    CHECK(sg_demux_sideband_response(outer, outer_len, &band1) == 0, "demux should succeed");
    CHECK(sg_parse_push_report_status(band1.data, band1.len, &report) == 0,
         "parsing the demuxed band-1 content should succeed");
    CHECK(report.unpack_ok == 1, "unpack should be ok");
    CHECK(report.ref_count == 1, "expected 1 ref result, got %zu", report.ref_count);
    if (report.ref_count == 1)
        CHECK(report.refs[0].ok == 1, "ref should be ok");

    sg_push_report_free(&report);
    sg_buf_free(&band1);
    free(outer);
    free(inner);
}

/* ---- URL redaction (sg_url_redact, declared in sg/http.h) ---- */

static void test_url_redact_strips_userinfo(void)
{
    char *redacted = sg_url_redact("https://user:token@host.example.com/x.git");

    CHECK(redacted != NULL, "redact should not fail");
    if (redacted != NULL) {
        CHECK(strstr(redacted, "token") == NULL, "redacted URL must not contain the token: %s",
             redacted);
        CHECK(strstr(redacted, "user:token") == NULL, "redacted URL must not contain user:token: %s",
             redacted);
        CHECK(strcmp(redacted, "https://***@host.example.com/x.git") == 0,
             "unexpected redacted form: %s", redacted);
    }
    free(redacted);
}

static void test_url_redact_username_only(void)
{
    char *redacted = sg_url_redact("https://user@host.example.com/x.git");

    CHECK(redacted != NULL, "redact should not fail");
    if (redacted != NULL) {
        CHECK(strstr(redacted, "user@") == NULL, "redacted URL must not contain the bare username: %s",
             redacted);
        CHECK(strcmp(redacted, "https://***@host.example.com/x.git") == 0,
             "unexpected redacted form: %s", redacted);
    }
    free(redacted);
}

static void test_url_redact_no_credentials_unchanged(void)
{
    const char *url = "https://host.example.com/x.git";
    char *redacted = sg_url_redact(url);

    CHECK(redacted != NULL && strcmp(redacted, url) == 0,
         "a URL with no userinfo should be returned unchanged, got %s",
         redacted != NULL ? redacted : "(null)");
    free(redacted);
}

static void test_url_redact_no_scheme_unchanged(void)
{
    const char *url = "not-a-url-at-all";
    char *redacted = sg_url_redact(url);

    /* Still true after Phase 47, but for a narrower reason than the name
       suggests: a schemeless string is now read as the scp-like ssh
       shorthand, and this one survives because it has no ':' to split on,
       not because "no scheme" means "leave it alone". */
    CHECK(redacted != NULL && strcmp(redacted, url) == 0,
         "a string with no scheme and no colon should be returned unchanged, got %s",
         redacted != NULL ? redacted : "(null)");
    free(redacted);
}

/* Phase 47: the scp-like ssh shorthand has no "scheme://", and before ssh
   existed this function returned such a string untouched -- which would print
   a user name straight into an error message. */
static void test_url_redact_scp_like(void)
{
    char *redacted = sg_url_redact("git@host.example.com:srv/repo.git");

    CHECK(redacted != NULL && strcmp(redacted, "***@host.example.com:srv/repo.git") == 0,
         "scp-like userinfo should be redacted, got '%s'", redacted ? redacted : "(null)");
    free(redacted);
}

static void test_url_redact_scp_like_without_user(void)
{
    char *redacted = sg_url_redact("host.example.com:srv/repo.git");

    CHECK(redacted != NULL && strcmp(redacted, "host.example.com:srv/repo.git") == 0,
         "a scp-like url with no userinfo is unchanged, got '%s'", redacted ? redacted : "(null)");
    free(redacted);
}

/* An '@' AFTER the colon is part of the path, not userinfo -- redacting on
   the last '@' in the whole string would eat the host name here. */
static void test_url_redact_at_sign_in_path(void)
{
    char *redacted = sg_url_redact("host.example.com:srv/we@ird.git");

    CHECK(redacted != NULL && strcmp(redacted, "host.example.com:srv/we@ird.git") == 0,
         "an '@' in the path is not userinfo, got '%s'", redacted ? redacted : "(null)");
    free(redacted);
}

/* A colon AFTER a slash makes this a local path that merely contains a
   colon -- the same rule the ssh transport routes on. Redacting it would
   corrupt a string that was never a URL. */
static void test_url_redact_path_with_colon_and_at(void)
{
    char *redacted = sg_url_redact("a/b@c:d");

    CHECK(redacted != NULL && strcmp(redacted, "a/b@c:d") == 0,
         "a path whose colon follows a slash is not scp-like, got '%s'",
         redacted ? redacted : "(null)");
    free(redacted);
}

int main(void)
{
    test_all_ok();
    test_unpack_failure();
    test_single_ref_ng_with_reason();
    test_multiple_refs_mixed();
    test_malformed_first_line_rejected();
    test_side_band_wrapped_report_status();

    test_url_redact_strips_userinfo();
    test_url_redact_username_only();
    test_url_redact_no_credentials_unchanged();
    test_url_redact_no_scheme_unchanged();
    test_url_redact_scp_like();
    test_url_redact_scp_like_without_user();
    test_url_redact_at_sign_in_path();
    test_url_redact_path_with_colon_and_at();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all push report tests passed\n");
    return 0;
}
