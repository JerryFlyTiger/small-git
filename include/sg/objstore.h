#ifndef SG_OBJSTORE_H
#define SG_OBJSTORE_H

#include <stddef.h>

#include "sg/hash.h"
#include "sg/object.h"

/* The general-purpose object lookup: tries loose storage first, then falls
   back to scanning git_dir/objects/pack/. Everything outside of loose.c and
   pack.c themselves should read objects through this rather than calling
   sg_loose_read directly, so objects that have been packed (by `sg repack`
   or by a real git gc) are still found. *content_out is malloc'd, caller
   frees. Returns 0 on success, -1 if the object isn't found anywhere or is
   malformed. */
int sg_object_read(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                   sg_obj_type *type_out, unsigned char **content_out, size_t *content_len_out);

#endif
