#include "sg/transport.h"

#include "sg/http.h"
#include "sg/pktline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SG_PATH_MAX 4096
/* Well under SG_PATH_MAX so "<git_dir>/refs/remotes/<remote>/<name>" always
   fits without snprintf truncation. */
#define SG_REF_NAME_MAX 1024

int sg_ref_name_is_safe(const char *name)
{
    size_t len = strlen(name);
    size_t i;

    if (len == 0)
        return 0;
    /* A pkt-line can carry a ~65KB ref name, but every writer below composes
       it into a fixed SG_PATH_MAX buffer with snprintf. Without this bound,
       an overlong name is silently truncated into a different path -- and two
       names sharing a long prefix would collide onto the same ref file. */
    if (len > SG_REF_NAME_MAX)
        return 0;
    if (strncmp(name, "refs/", 5) != 0)
        return 0;
    if (strstr(name, "..") != NULL)
        return 0;
    if (strstr(name, "//") != NULL)
        return 0;
    if (name[len - 1] == '/')
        return 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];

        if (c < 0x20)
            return 0;
        if (c == '\\' || c == '~' || c == '^' || c == ':' || c == '?' || c == '*' || c == '[' ||
           c == ' ')
            return 0;
    }
    return 1;
}

void sg_ref_adv_free(sg_ref_adv *adv)
{
    size_t i;

    for (i = 0; i < adv->count; i++)
        free(adv->refs[i].name);
    free(adv->refs);
    free(adv->capabilities);
    free(adv->head_symref);
    adv->refs = NULL;
    adv->count = 0;
    adv->capabilities = NULL;
    adv->head_symref = NULL;
}

static int refs_grow(sg_remote_ref **refs, size_t *cap, size_t need)
{
    size_t new_cap;
    sg_remote_ref *grown;

    if (need <= *cap)
        return 0;
    new_cap = (*cap == 0) ? 8 : *cap * 2;
    while (new_cap < need)
        new_cap *= 2;
    grown = realloc(*refs, new_cap * sizeof(**refs));
    if (grown == NULL)
        return -1;
    *refs = grown;
    *cap = new_cap;
    return 0;
}

/* Parses "<40-hex> <refname>" (optionally followed by a trailing '\n', which
   is stripped) out of payload[0..line_len). Returns 0 on success. */
static int parse_ref_id_and_name(const unsigned char *payload, size_t line_len,
                                 unsigned char id_out[SG_SHA1_RAW_LEN], char **name_out)
{
    char hex[SG_SHA1_HEX_LEN + 1];
    size_t name_len;

    if (line_len > 0 && payload[line_len - 1] == '\n')
        line_len--;

    if (line_len <= SG_SHA1_HEX_LEN + 1 || payload[SG_SHA1_HEX_LEN] != ' ')
        return -1;

    memcpy(hex, payload, SG_SHA1_HEX_LEN);
    hex[SG_SHA1_HEX_LEN] = '\0';
    if (sg_hex_to_sha1(hex, id_out) != 0)
        return -1;

    name_len = line_len - (SG_SHA1_HEX_LEN + 1);
    *name_out = malloc(name_len + 1);
    if (*name_out == NULL)
        return -1;
    memcpy(*name_out, payload + SG_SHA1_HEX_LEN + 1, name_len);
    (*name_out)[name_len] = '\0';
    return 0;
}

/* Finds "symref=HEAD:<target>" in a space-separated capability string and
   returns a malloc'd copy of <target>, or NULL if absent/malformed. */
static char *find_head_symref(const char *capabilities)
{
    static const char needle[] = "symref=HEAD:";
    const char *p = strstr(capabilities, needle);
    const char *start;
    const char *end;
    size_t len;
    char *out;

    if (p == NULL)
        return NULL;
    start = p + strlen(needle);
    end = start;
    while (*end != '\0' && *end != ' ')
        end++;
    len = (size_t)(end - start);
    if (len == 0)
        return NULL;
    out = malloc(len + 1);
    if (out == NULL)
        return NULL;
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

/* Pure parser over an already-fetched info/refs response body -- factored out
   of sg_transport_ls_refs so it's unit-testable without a network round
   trip. Not part of the public sg/transport.h surface (same convention as
   src/storage/pack.c's non-static varint helpers), declared via extern in
   tests/test_refadv.c. */
int sg_parse_ref_advertisement(const unsigned char *data, size_t len, sg_ref_adv *adv_out)
{
    static const char service_prefix[] = "# service=git-upload-pack";
    size_t pos = 0;
    sg_pkt_type type;
    const unsigned char *payload;
    size_t payload_len;
    sg_remote_ref *refs = NULL;
    size_t count = 0, cap = 0;
    char *capabilities = NULL;
    int first = 1;

    memset(adv_out, 0, sizeof(*adv_out));

    if (sg_pkt_read(data, len, &pos, &type, &payload, &payload_len) != 0 || type != SG_PKT_DATA)
        goto malformed;
    {
        size_t plen = payload_len;

        if (plen > 0 && payload[plen - 1] == '\n')
            plen--;
        if (plen != strlen(service_prefix) || memcmp(payload, service_prefix, plen) != 0)
            goto malformed;
    }

    if (sg_pkt_read(data, len, &pos, &type, &payload, &payload_len) != 0 || type != SG_PKT_FLUSH)
        goto malformed;

    for (;;) {
        unsigned char id[SG_SHA1_RAW_LEN];
        char *name;
        size_t line_len;
        const unsigned char *nul;

        if (sg_pkt_read(data, len, &pos, &type, &payload, &payload_len) != 0)
            goto malformed;
        if (type == SG_PKT_FLUSH)
            break;
        if (type != SG_PKT_DATA)
            goto malformed;

        line_len = payload_len;
        if (first) {
            nul = memchr(payload, '\0', payload_len);
            if (nul != NULL) {
                size_t caps_off = (size_t)(nul - payload) + 1;
                size_t caps_len = payload_len - caps_off;

                if (caps_len > 0 && payload[payload_len - 1] == '\n')
                    caps_len--;
                capabilities = malloc(caps_len + 1);
                if (capabilities == NULL)
                    goto fail;
                memcpy(capabilities, payload + caps_off, caps_len);
                capabilities[caps_len] = '\0';
                line_len = (size_t)(nul - payload);
            }
        }

        if (parse_ref_id_and_name(payload, line_len, id, &name) != 0)
            goto malformed;
        first = 0;

        /* the empty-repository placeholder: a single all-zero-id
           "capabilities^{}" ref and nothing else -- not a real ref */
        {
            static const unsigned char zero_id[SG_SHA1_RAW_LEN] = {0};
            int is_zero = memcmp(id, zero_id, SG_SHA1_RAW_LEN) == 0;

            if (is_zero && strcmp(name, "capabilities^{}") == 0) {
                free(name);
                continue;
            }
        }

        if (!sg_ref_name_is_safe(name)) {
            fprintf(stderr, "sg: warning: 忽略遠端不合法的 ref 名稱 '%s'\n", name);
            free(name);
            continue;
        }

        if (refs_grow(&refs, &cap, count + 1) != 0) {
            free(name);
            goto fail;
        }
        refs[count].name = name;
        memcpy(refs[count].id, id, SG_SHA1_RAW_LEN);
        count++;
    }

    adv_out->refs = refs;
    adv_out->count = count;
    adv_out->capabilities = capabilities;
    adv_out->head_symref = (capabilities != NULL) ? find_head_symref(capabilities) : NULL;
    return 0;

malformed:
    fprintf(stderr, "sg: 遠端回應不是有效的 git smart HTTP ref advertisement\n");
fail:
    {
        size_t i;

        for (i = 0; i < count; i++)
            free(refs[i].name);
        free(refs);
        free(capabilities);
    }
    return -1;
}

int sg_transport_ls_refs(const char *base_url, sg_ref_adv *adv_out)
{
    char url[SG_PATH_MAX];
    sg_buf resp;
    int rc;

    snprintf(url, sizeof(url), "%s/info/refs?service=git-upload-pack", base_url);

    if (sg_http_get(url, "*/*", &resp) != 0)
        return -1;

    rc = sg_parse_ref_advertisement(resp.data, resp.len, adv_out);
    sg_buf_free(&resp);
    return rc;
}

/* ---- sideband-64k demux + fetch negotiation ---- */

#define SG_BAND_PACK 1
#define SG_BAND_PROGRESS 2
#define SG_BAND_ERROR 3

/* Not static (but not part of the public sg/transport.h surface, same
   convention as sg_parse_ref_advertisement above) so tests/test_refadv.c can
   exercise the band-byte protocol-error path directly via extern, without
   needing a real HTTP round trip. */
int demux_upload_pack_response(const unsigned char *data, size_t len, sg_buf *pack_out)
{
    size_t pos = 0;
    sg_pkt_type type;
    const unsigned char *payload;
    size_t payload_len;
    /* Once we've seen a real band byte (1/2/3), we're past the
       pre-multiplex NAK/ACK line(s) and into genuine side-band-64k framing
       -- from that point on, every packet's first byte must be a band
       number, and anything else is a protocol error per spec section 2
       ("其他值 = 協定錯誤"), not something to silently skip. */
    int in_multiplex = 0;

    for (;;) {
        if (sg_pkt_read(data, len, &pos, &type, &payload, &payload_len) != 0) {
            fprintf(stderr, "sg: 遠端 git-upload-pack 回應格式錯誤\n");
            return -1;
        }
        if (type == SG_PKT_FLUSH)
            return 0;
        if (type == SG_PKT_DELIM) {
            fprintf(stderr, "sg: 遠端回應包含非預期的 protocol v2 delim-pkt\n");
            return -1;
        }
        if (payload_len == 0)
            continue; /* an empty DATA packet (just "0004") carries nothing */

        {
            unsigned char band = payload[0];

            switch (band) {
            case SG_BAND_PACK:
                in_multiplex = 1;
                if (sg_buf_append(pack_out, payload + 1, payload_len - 1) != 0)
                    return -1;
                break;
            case SG_BAND_PROGRESS:
                in_multiplex = 1;
                fwrite(payload + 1, 1, payload_len - 1, stderr);
                break;
            case SG_BAND_ERROR:
                in_multiplex = 1;
                fprintf(stderr, "sg: remote error: %.*s\n", (int)(payload_len - 1),
                       (const char *)payload + 1);
                return -1;
            default:
                if (in_multiplex) {
                    fprintf(stderr,
                           "sg: 遠端回應的 side-band 封包帶有未知的 band 編號 %d（協定錯誤）\n",
                           band);
                    return -1;
                }
                /* still pre-multiplex: this is a negotiation line (NAK\n /
                   ACK <sha1>...\n), which is never band-prefixed -- their
                   first byte is always the printable 'N' or 'A', which
                   can't collide with band values 1-3 */
                break;
            }
        }
    }
}

int sg_transport_fetch_pack(const char *base_url,
                            const unsigned char (*want_ids)[SG_SHA1_RAW_LEN], size_t want_count,
                            const unsigned char (*have_ids)[SG_SHA1_RAW_LEN], size_t have_count,
                            unsigned char **pack_out, size_t *pack_len_out)
{
    unsigned char *body = NULL;
    size_t body_len = 0, body_cap = 0;
    char url[SG_PATH_MAX];
    sg_buf resp;
    sg_buf pack_buf;
    size_t i;
    int rc = -1;

    if (want_count == 0) {
        fprintf(stderr, "sg: fetch-pack called with no want ids\n");
        return -1;
    }

    for (i = 0; i < want_count; i++) {
        char hex[SG_SHA1_HEX_LEN + 1];
        char line[256];

        sg_sha1_to_hex(want_ids[i], hex);
        if (i == 0)
            snprintf(line, sizeof(line), "want %s side-band-64k ofs-delta agent=small-git/0.1\n",
                    hex);
        else
            snprintf(line, sizeof(line), "want %s\n", hex);
        if (sg_pkt_append_str(&body, &body_len, &body_cap, line) != 0)
            goto done;
    }
    if (sg_pkt_append_flush(&body, &body_len, &body_cap) != 0)
        goto done;

    for (i = 0; i < have_count; i++) {
        char hex[SG_SHA1_HEX_LEN + 1];
        char line[64];

        sg_sha1_to_hex(have_ids[i], hex);
        snprintf(line, sizeof(line), "have %s\n", hex);
        if (sg_pkt_append_str(&body, &body_len, &body_cap, line) != 0)
            goto done;
    }
    if (sg_pkt_append_str(&body, &body_len, &body_cap, "done\n") != 0)
        goto done;

    snprintf(url, sizeof(url), "%s/git-upload-pack", base_url);

    memset(&resp, 0, sizeof(resp));
    memset(&pack_buf, 0, sizeof(pack_buf));

    if (sg_http_post(url, "application/x-git-upload-pack-request",
                     "application/x-git-upload-pack-result", body, body_len, &resp) != 0)
        goto done;

    rc = demux_upload_pack_response(resp.data, resp.len, &pack_buf);
    sg_buf_free(&resp);

    if (rc != 0) {
        sg_buf_free(&pack_buf);
        goto done;
    }

    *pack_out = pack_buf.data;
    *pack_len_out = pack_buf.len;

done:
    free(body);
    return rc;
}
