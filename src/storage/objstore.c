#include "sg/objstore.h"

#include <stdlib.h>
#include <string.h>

#include "sg/loose.h"
#include "sg/pack.h"

int sg_object_read(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                   sg_obj_type *type_out, unsigned char **content_out, size_t *content_len_out)
{
    if (sg_loose_read(git_dir, id, type_out, content_out, content_len_out) == 0)
        return 0;
    return sg_pack_read(git_dir, id, type_out, content_out, content_len_out);
}

int sg_commit_tree_of(const char *git_dir, const unsigned char commit_id[SG_SHA1_RAW_LEN],
                      unsigned char tree_id_out[SG_SHA1_RAW_LEN])
{
    sg_obj_type type;
    unsigned char *content;
    size_t content_len;
    sg_commit commit;

    if (sg_object_read(git_dir, commit_id, &type, &content, &content_len) != 0)
        return -1;
    if (type != SG_OBJ_COMMIT) {
        free(content);
        return -1;
    }
    if (sg_commit_parse(content, content_len, &commit) != 0) {
        free(content);
        return -1;
    }
    free(content);
    memcpy(tree_id_out, commit.tree, SG_SHA1_RAW_LEN);
    sg_commit_free(&commit);
    return 0;
}

void sg_oid_list_free(sg_oid_list *l)
{
    free(l->ids);
    l->ids = NULL;
    l->count = 0;
    l->cap = 0;
}

int sg_oid_list_append(sg_oid_list *l, const unsigned char id[SG_SHA1_RAW_LEN])
{
    if (l->count == l->cap) {
        size_t new_cap = l->cap == 0 ? 16 : l->cap * 2;
        unsigned char(*grown)[SG_SHA1_RAW_LEN] = realloc(l->ids, new_cap * sizeof(*grown));

        if (grown == NULL)
            return -1;
        l->ids = grown;
        l->cap = new_cap;
    }
    memcpy(l->ids[l->count], id, SG_SHA1_RAW_LEN);
    l->count++;
    return 0;
}

static int oid_raw_cmp(const void *a, const void *b)
{
    return memcmp(a, b, SG_SHA1_RAW_LEN);
}

int sg_object_find_prefix(const char *git_dir, const char *hex_prefix, sg_oid_list *out)
{
    size_t len = strlen(hex_prefix);
    unsigned char prefix[SG_SHA1_RAW_LEN];
    size_t write_idx, i;

    out->ids = NULL;
    out->count = 0;
    out->cap = 0;

    if (len < SG_OID_MIN_ABBREV || len > SG_SHA1_HEX_LEN - 1)
        return -1;
    if (sg_hex_prefix_to_sha1(hex_prefix, len, prefix) != 0)
        return -1;

    if (sg_loose_find_prefix(git_dir, prefix, len, out) != 0)
        goto fail;
    if (sg_pack_find_prefix(git_dir, prefix, len, out) != 0)
        goto fail;

    if (out->count > 1) {
        qsort(out->ids, out->count, sizeof(*out->ids), oid_raw_cmp);
        write_idx = 1;
        for (i = 1; i < out->count; i++) {
            if (memcmp(out->ids[i], out->ids[write_idx - 1], SG_SHA1_RAW_LEN) != 0) {
                if (write_idx != i)
                    memcpy(out->ids[write_idx], out->ids[i], SG_SHA1_RAW_LEN);
                write_idx++;
            }
        }
        out->count = write_idx;
    }
    return 0;

fail:
    sg_oid_list_free(out);
    return -1;
}
