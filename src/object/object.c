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

int sg_object_parse(const unsigned char *data, size_t data_len, sg_object *obj)
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

    if ((size_t)(nul + 1 - data) + declared_size != data_len)
        return -1;

    obj->type = type;
    obj->content = nul + 1;
    obj->content_len = declared_size;
    return 0;
}
