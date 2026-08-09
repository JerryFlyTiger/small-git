#include "sg/object.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *sg_obj_type_name(sg_obj_type type)
{
    switch (type) {
    case SG_OBJ_BLOB:
        return "blob";
    case SG_OBJ_TREE:
        return "tree";
    case SG_OBJ_COMMIT:
        return "commit";
    case SG_OBJ_TAG:
        return "tag";
    }
    return NULL;
}

int sg_obj_type_from_name(const char *name, sg_obj_type *out)
{
    if (strcmp(name, "blob") == 0) {
        *out = SG_OBJ_BLOB;
        return 0;
    }
    if (strcmp(name, "tree") == 0) {
        *out = SG_OBJ_TREE;
        return 0;
    }
    if (strcmp(name, "commit") == 0) {
        *out = SG_OBJ_COMMIT;
        return 0;
    }
    if (strcmp(name, "tag") == 0) {
        *out = SG_OBJ_TAG;
        return 0;
    }
    return -1;
}

int sg_object_format(sg_obj_type type, const void *content, size_t content_len,
                     unsigned char **out, size_t *out_len)
{
    const char *type_name = sg_obj_type_name(type);
    char header[64];
    int header_len;
    unsigned char *buf;
    size_t total;

    header_len = snprintf(header, sizeof(header), "%s %zu", type_name, content_len);
    if (header_len < 0 || (size_t)header_len >= sizeof(header))
        return -1;

    total = (size_t)header_len + 1 + content_len;
    buf = malloc(total);
    if (buf == NULL)
        return -1;

    memcpy(buf, header, (size_t)header_len);
    buf[header_len] = '\0';
    memcpy(buf + header_len + 1, content, content_len);

    *out = buf;
    *out_len = total;
    return 0;
}

void sg_object_hash(sg_obj_type type, const void *content, size_t content_len,
                    unsigned char id[SG_SHA1_RAW_LEN])
{
    unsigned char *formatted;
    size_t formatted_len;

    /* header is bounded (type name + decimal size), never fails in practice */
    sg_object_format(type, content, content_len, &formatted, &formatted_len);
    sg_sha1(formatted, formatted_len, id);
    free(formatted);
}

int sg_message_cleanup(const char *msg, char **out)
{
    size_t len = strlen(msg);
    size_t out_cap = len + 2;
    char *buf;
    size_t out_len = 0;
    size_t i = 0;
    int have_content = 0;
    int pending_blank = 0;

    buf = malloc(out_cap);
    if (buf == NULL)
        return -1;

    while (i < len) {
        size_t line_start = i;
        size_t line_end = line_start;
        size_t trimmed_end;
        int is_blank;

        while (line_end < len && msg[line_end] != '\n')
            line_end++;

        trimmed_end = line_end;
        while (trimmed_end > line_start &&
              (msg[trimmed_end - 1] == ' ' || msg[trimmed_end - 1] == '\t' ||
               msg[trimmed_end - 1] == '\r' || msg[trimmed_end - 1] == '\v' ||
               msg[trimmed_end - 1] == '\f'))
            trimmed_end--;

        is_blank = (trimmed_end == line_start);

        if (is_blank) {
            if (have_content)
                pending_blank = 1;
            /* leading blank lines (have_content == 0) are simply dropped */
        } else {
            size_t line_len = trimmed_end - line_start;
            size_t need = out_len + (pending_blank ? 1 : 0) + line_len + 1;

            if (need > out_cap) {
                char *tmp;

                out_cap = need + 64;
                tmp = realloc(buf, out_cap);
                if (tmp == NULL) {
                    free(buf);
                    return -1;
                }
                buf = tmp;
            }
            if (pending_blank)
                buf[out_len++] = '\n';
            pending_blank = 0;
            memcpy(buf + out_len, msg + line_start, line_len);
            out_len += line_len;
            buf[out_len++] = '\n';
            have_content = 1;
        }

        i = (line_end < len) ? line_end + 1 : line_end;
    }

    if (out_len + 1 > out_cap) {
        char *tmp = realloc(buf, out_len + 1);

        if (tmp == NULL) {
            free(buf);
            return -1;
        }
        buf = tmp;
    }
    buf[out_len] = '\0';

    *out = buf;
    return 0;
}

int sg_object_parse_header(const unsigned char *data, size_t data_len, sg_obj_type *type_out,
                           size_t *header_len_out, size_t *declared_size_out)
{
    const unsigned char *space;
    const unsigned char *nul;
    size_t type_len;
    char type_buf[16];
    sg_obj_type type;
    size_t declared_size;
    char *end;

    space = memchr(data, ' ', data_len);
    if (space == NULL)
        return -1;

    type_len = (size_t)(space - data);
    if (type_len >= sizeof(type_buf))
        return -1;
    memcpy(type_buf, data, type_len);
    type_buf[type_len] = '\0';
    if (sg_obj_type_from_name(type_buf, &type) != 0)
        return -1;

    nul = memchr(space + 1, '\0', data_len - type_len - 1);
    if (nul == NULL)
        return -1;

    declared_size = strtoul((const char *)space + 1, &end, 10);
    if (end != (const char *)nul)
        return -1;

    *type_out = type;
    *header_len_out = (size_t)(nul + 1 - data);
    *declared_size_out = declared_size;
    return 0;
}

int sg_object_parse(const unsigned char *data, size_t data_len, sg_object *obj)
{
    sg_obj_type type;
    size_t header_len;
    size_t declared_size;

    if (sg_object_parse_header(data, data_len, &type, &header_len, &declared_size) != 0)
        return -1;

    if (header_len + declared_size != data_len)
        return -1;

    obj->type = type;
    obj->content = data + header_len;
    obj->content_len = declared_size;
    return 0;
}
