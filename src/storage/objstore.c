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
