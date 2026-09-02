#include "sg/object.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* shared with commit.c's logic but kept file-local to avoid a header-only helper module */
static char *dup_range(const char *start, size_t len)
{
    char *s = malloc(len + 1);

    if (s == NULL)
        return NULL;
    memcpy(s, start, len);
    s[len] = '\0';
    return s;
}

static const char *find_eol(const char *p, const char *end)
{
    return memchr(p, '\n', (size_t)(end - p));
}

static int has_prefix(const char *p, const char *end, const char *prefix)
{
    size_t len = strlen(prefix);

    if ((size_t)(end - p) < len)
        return 0;
    return memcmp(p, prefix, len) == 0;
}

int sg_tag_serialize(const sg_tag *tag, unsigned char **out, size_t *out_len)
{
    char object_hex[SG_SHA1_HEX_LEN + 1];
    size_t cap;
    char *buf;
    size_t used = 0;
    int n;

    sg_sha1_to_hex(tag->object, object_hex);

    cap = 512 + strlen(tag->tag_name) + strlen(tag->tagger_name) + strlen(tag->tagger_email) +
        strlen(tag->message);
    buf = malloc(cap);
    if (buf == NULL)
        return -1;

    n = snprintf(buf + used, cap - used, "object %s\n", object_hex);
    used += (size_t)n;
    n = snprintf(buf + used, cap - used, "type %s\n", sg_obj_type_name(tag->object_type));
    used += (size_t)n;
    n = snprintf(buf + used, cap - used, "tag %s\n", tag->tag_name);
    used += (size_t)n;
    n = snprintf(buf + used, cap - used, "tagger %s <%s> %lld %s\n", tag->tagger_name,
                tag->tagger_email, tag->tagger_time, tag->tagger_tz);
    used += (size_t)n;
    n = snprintf(buf + used, cap - used, "\n");
    used += (size_t)n;

    memcpy(buf + used, tag->message, strlen(tag->message));
    used += strlen(tag->message);

    *out = (unsigned char *)buf;
    *out_len = used;
    return 0;
}

int sg_tag_parse(const unsigned char *content, size_t content_len, sg_tag *out)
{
    const char *p = (const char *)content;
    const char *end = (const char *)content + content_len;
    const char *line_end;

    memset(out, 0, sizeof(*out));

    if (!has_prefix(p, end, "object "))
        goto fail;
    line_end = find_eol(p, end);
    if (line_end == NULL || (size_t)(line_end - p) != 7 + SG_SHA1_HEX_LEN)
        goto fail;
    {
        char hex[SG_SHA1_HEX_LEN + 1];

        memcpy(hex, p + 7, SG_SHA1_HEX_LEN);
        hex[SG_SHA1_HEX_LEN] = '\0';
        if (sg_hex_to_sha1(hex, out->object) != 0)
            goto fail;
    }
    p = line_end + 1;

    if (!has_prefix(p, end, "type "))
        goto fail;
    line_end = find_eol(p, end);
    if (line_end == NULL)
        goto fail;
    {
        char type_buf[16];
        size_t type_len = (size_t)(line_end - (p + 5));

        if (type_len >= sizeof(type_buf))
            goto fail;
        memcpy(type_buf, p + 5, type_len);
        type_buf[type_len] = '\0';
        if (sg_obj_type_from_name(type_buf, &out->object_type) != 0)
            goto fail;
    }
    p = line_end + 1;

    if (!has_prefix(p, end, "tag "))
        goto fail;
    line_end = find_eol(p, end);
    if (line_end == NULL)
        goto fail;
    out->tag_name = dup_range(p + 4, (size_t)(line_end - (p + 4)));
    if (out->tag_name == NULL)
        goto fail;
    p = line_end + 1;

    if (!has_prefix(p, end, "tagger "))
        goto fail;
    line_end = find_eol(p, end);
    if (line_end == NULL)
        goto fail;
    {
        const char *sig_start = p + 7;
        const char *lt = memchr(sig_start, '<', (size_t)(line_end - sig_start));
        const char *gt;
        const char *name_end;
        const char *num_start;
        char *num_end;
        size_t tz_len;

        if (lt == NULL)
            goto fail;
        gt = memchr(lt, '>', (size_t)(line_end - lt));
        if (gt == NULL)
            goto fail;

        name_end = lt;
        while (name_end > sig_start && name_end[-1] == ' ')
            name_end--;

        num_start = gt + 1;
        while (num_start < line_end && *num_start == ' ')
            num_start++;

        out->tagger_time = strtoll(num_start, &num_end, 10);
        if (num_end == num_start || num_end >= line_end || *num_end != ' ')
            goto fail;
        num_end++;

        tz_len = (size_t)(line_end - num_end);
        if (tz_len == 0 || tz_len >= sizeof(out->tagger_tz))
            goto fail;

        out->tagger_name = dup_range(sig_start, (size_t)(name_end - sig_start));
        out->tagger_email = dup_range(lt + 1, (size_t)(gt - lt - 1));
        if (out->tagger_name == NULL || out->tagger_email == NULL)
            goto fail;
        memcpy(out->tagger_tz, num_end, tz_len);
        out->tagger_tz[tz_len] = '\0';
    }
    p = line_end + 1;

    /* Unknown trailing headers are skipped over but kept verbatim in
       out->extra_headers, same rule and same reasoning as sg_commit_parse
       -- see its comment and docs/DESIGN.md Phase 61. */
    {
        const char *header_start = p;

        while (p < end && *p != '\n') {
            line_end = find_eol(p, end);
            if (line_end == NULL)
                goto fail;
            p = line_end + 1;
        }

        if (p >= end || *p != '\n')
            goto fail;

        out->extra_headers = dup_range(header_start, (size_t)(p - header_start));
        if (out->extra_headers == NULL)
            goto fail;
        p++;
    }

    out->message = dup_range(p, (size_t)(end - p));
    if (out->message == NULL)
        goto fail;

    return 0;

fail:
    sg_tag_free(out);
    return -1;
}

void sg_tag_free(sg_tag *tag)
{
    free(tag->tag_name);
    free(tag->tagger_name);
    free(tag->tagger_email);
    free(tag->message);
    free(tag->extra_headers);
    memset(tag, 0, sizeof(*tag));
}
