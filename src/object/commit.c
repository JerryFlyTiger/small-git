#include "sg/object.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dup_range(const char *start, size_t len)
{
    char *s = malloc(len + 1);

    if (s == NULL)
        return NULL;
    memcpy(s, start, len);
    s[len] = '\0';
    return s;
}

/* finds the end of the current line (exclusive), or NULL if no '\n' before end */
static const char *find_eol(const char *p, const char *end)
{
    return memchr(p, '\n', (size_t)(end - p));
}

/* bounds-checked prefix test: content is not guaranteed NUL-terminated, so a
   plain strncmp could read past `end` when the remaining bytes are short */
static int has_prefix(const char *p, const char *end, const char *prefix)
{
    size_t len = strlen(prefix);

    if ((size_t)(end - p) < len)
        return 0;
    return memcmp(p, prefix, len) == 0;
}

/* parses "{name} <{email}> {time} {tz}" as found after "author "/"committer "/"tagger " */
static int parse_signature(const char *start, const char *line_end, char **name_out,
                           char **email_out, long long *time_out, char tz_out[8])
{
    const char *lt = memchr(start, '<', (size_t)(line_end - start));
    const char *gt;
    const char *name_end;
    const char *num_start;
    char *end;
    long long time_val;
    size_t tz_len;

    if (lt == NULL)
        return -1;
    gt = memchr(lt, '>', (size_t)(line_end - lt));
    if (gt == NULL)
        return -1;

    name_end = lt;
    while (name_end > start && name_end[-1] == ' ')
        name_end--;

    num_start = gt + 1;
    while (num_start < line_end && *num_start == ' ')
        num_start++;

    time_val = strtoll(num_start, &end, 10);
    if (end == num_start || end >= line_end || *end != ' ')
        return -1;
    end++;

    tz_len = (size_t)(line_end - end);
    if (tz_len == 0 || tz_len >= 8)
        return -1;

    *name_out = dup_range(start, (size_t)(name_end - start));
    *email_out = dup_range(lt + 1, (size_t)(gt - lt - 1));
    if (*name_out == NULL || *email_out == NULL) {
        free(*name_out);
        free(*email_out);
        return -1;
    }
    *time_out = time_val;
    memcpy(tz_out, end, tz_len);
    tz_out[tz_len] = '\0';
    return 0;
}

int sg_commit_serialize(const sg_commit *commit, unsigned char **out, size_t *out_len)
{
    char tree_hex[SG_SHA1_HEX_LEN + 1];
    size_t cap;
    char *buf;
    size_t used = 0;
    size_t i;
    int n;

    sg_sha1_to_hex(commit->tree, tree_hex);

    cap = 512 + commit->parent_count * 64 + strlen(commit->author_name) +
        strlen(commit->author_email) + strlen(commit->committer_name) +
        strlen(commit->committer_email) + strlen(commit->message);
    buf = malloc(cap);
    if (buf == NULL)
        return -1;

    n = snprintf(buf + used, cap - used, "tree %s\n", tree_hex);
    used += (size_t)n;

    for (i = 0; i < commit->parent_count; i++) {
        char parent_hex[SG_SHA1_HEX_LEN + 1];

        sg_sha1_to_hex(commit->parents[i], parent_hex);
        n = snprintf(buf + used, cap - used, "parent %s\n", parent_hex);
        used += (size_t)n;
    }

    n = snprintf(buf + used, cap - used, "author %s <%s> %lld %s\n", commit->author_name,
                commit->author_email, commit->author_time, commit->author_tz);
    used += (size_t)n;

    n = snprintf(buf + used, cap - used, "committer %s <%s> %lld %s\n", commit->committer_name,
                commit->committer_email, commit->committer_time, commit->committer_tz);
    used += (size_t)n;

    n = snprintf(buf + used, cap - used, "\n");
    used += (size_t)n;

    memcpy(buf + used, commit->message, strlen(commit->message));
    used += strlen(commit->message);

    *out = (unsigned char *)buf;
    *out_len = used;
    return 0;
}

int sg_commit_parse(const unsigned char *content, size_t content_len, sg_commit *out)
{
    const char *p = (const char *)content;
    const char *end = (const char *)content + content_len;
    const char *line_end;
    size_t parent_cap = 4;

    memset(out, 0, sizeof(*out));
    out->parents = malloc(parent_cap * sizeof(*out->parents));
    if (out->parents == NULL)
        return -1;

    line_end = find_eol(p, end);
    if (line_end == NULL || !has_prefix(p, end, "tree ") ||
        (size_t)(line_end - p) != 5 + SG_SHA1_HEX_LEN)
        goto fail;
    {
        char hex[SG_SHA1_HEX_LEN + 1];

        memcpy(hex, p + 5, SG_SHA1_HEX_LEN);
        hex[SG_SHA1_HEX_LEN] = '\0';
        if (sg_hex_to_sha1(hex, out->tree) != 0)
            goto fail;
    }
    p = line_end + 1;

    while (p < end && has_prefix(p, end, "parent ")) {
        char hex[SG_SHA1_HEX_LEN + 1];

        line_end = find_eol(p, end);
        if (line_end == NULL || (size_t)(line_end - p) != 7 + SG_SHA1_HEX_LEN)
            goto fail;
        memcpy(hex, p + 7, SG_SHA1_HEX_LEN);
        hex[SG_SHA1_HEX_LEN] = '\0';

        if (out->parent_count == parent_cap) {
            unsigned char (*grown)[SG_SHA1_RAW_LEN];

            parent_cap *= 2;
            grown = realloc(out->parents, parent_cap * sizeof(*out->parents));
            if (grown == NULL)
                goto fail;
            out->parents = grown;
        }
        if (sg_hex_to_sha1(hex, out->parents[out->parent_count]) != 0)
            goto fail;
        out->parent_count++;
        p = line_end + 1;
    }

    if (p >= end || !has_prefix(p, end, "author "))
        goto fail;
    line_end = find_eol(p, end);
    if (line_end == NULL)
        goto fail;
    if (parse_signature(p + 7, line_end, &out->author_name, &out->author_email,
                        &out->author_time, out->author_tz) != 0)
        goto fail;
    p = line_end + 1;

    if (p >= end || !has_prefix(p, end, "committer "))
        goto fail;
    line_end = find_eol(p, end);
    if (line_end == NULL)
        goto fail;
    if (parse_signature(p + 10, line_end, &out->committer_name, &out->committer_email,
                        &out->committer_time, out->committer_tz) != 0)
        goto fail;
    p = line_end + 1;

    /* Unknown trailing headers (e.g. "gpgsig", "encoding") are skipped over
       but their raw bytes are kept verbatim in out->extra_headers -- git
       accepts arbitrarily many of these after the known ones, continuation
       lines included, and the blank line that ends the header section is
       what terminates them, not the leading space of a continuation line.
       See docs/DESIGN.md Phase 61: the only current reader is
       `--pretty=raw`, which reproduces this block byte for byte. */
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
    sg_commit_free(out);
    return -1;
}

void sg_commit_free(sg_commit *commit)
{
    free(commit->parents);
    free(commit->author_name);
    free(commit->author_email);
    free(commit->committer_name);
    free(commit->committer_email);
    free(commit->message);
    free(commit->extra_headers);
    memset(commit, 0, sizeof(*commit));
}
