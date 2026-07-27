#ifndef SG_LOOSE_H
#define SG_LOOSE_H

#include <stddef.h>

#include "sg/hash.h"
#include "sg/object.h"

/* Writes {type, content} as a loose object under git_dir/objects/, computing
   its id and returning it in id_out. If the object already exists on disk,
   the write is skipped (content-addressed, so the existing file is already
   correct) -- this mirrors git's behavior. Returns 0 on success, -1 on I/O
   failure. */
int sg_loose_write(const char *git_dir, sg_obj_type type, const void *content,
                   size_t content_len, unsigned char id_out[SG_SHA1_RAW_LEN]);

/* Reads and decompresses the loose object identified by id under git_dir,
   validating its header. *content_out is malloc'd, caller frees. Returns 0 on
   success, -1 if the object doesn't exist or is malformed. */
int sg_loose_read(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                  sg_obj_type *type_out, unsigned char **content_out, size_t *content_len_out);

#endif
