#include "sg/objstore.h"

#include "sg/loose.h"
#include "sg/pack.h"

int sg_object_read(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                   sg_obj_type *type_out, unsigned char **content_out, size_t *content_len_out)
{
    if (sg_loose_read(git_dir, id, type_out, content_out, content_len_out) == 0)
        return 0;
    return sg_pack_read(git_dir, id, type_out, content_out, content_len_out);
}
