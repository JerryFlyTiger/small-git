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

/* Reads commit_id and returns its tree id. Returns 0, or -1 if the object is
   missing, unreadable, not a commit, or malformed. */
int sg_commit_tree_of(const char *git_dir, const unsigned char commit_id[SG_SHA1_RAW_LEN],
                      unsigned char tree_id_out[SG_SHA1_RAW_LEN]);

#endif
