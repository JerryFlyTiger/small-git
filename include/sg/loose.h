#ifndef SG_LOOSE_H
#define SG_LOOSE_H

#include <stddef.h>

#include "sg/hash.h"
#include "sg/object.h"
#include "sg/objstore.h"

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

/* Appends the id of every loose object under git_dir/objects/ whose id
   begins with the given prefix (its first `nibbles` hex digits, as parsed
   by sg_hex_prefix_to_sha1) to *out, growing it via sg_oid_list_append.
   Only opendir()s git_dir/objects/<xx>/, where <xx> is the two hex
   characters named by prefix[0] -- never the whole store, and never even
   opendir()s objects/ itself. A missing bucket directory (no loose object
   has ever begun with that byte) is zero matches, not an error. Malformed
   entries (wrong filename length, non-hex) are silently skipped, same as
   the loose-object walks in cmd_repack.c/cmd_chunk_info.c. Returns 0 on
   success (including zero matches), -1 if the bucket directory exists but
   could not be scanned (anything other than ENOENT/ENOTDIR from opendir --
   see loose.c for why "couldn't check" must never be folded into "checked,
   found nothing"), or on a malloc failure.

   REQUIRES nibbles >= 2, same reason as sg_pack_find_prefix's own
   precondition: the bucket this function opens is objects/<xx>/, where <xx>
   is prefix[0] as a FULL byte -- and sg_hex_prefix_to_sha1 zero-pads an odd
   trailing nibble's low half, so a 1-nibble prefix "N" only opens objects/N0/
   and silently misses every real match under objects/N1/..objects/Nf/.
   sg_object_find_prefix never passes fewer than SG_OID_MIN_ABBREV (4)
   nibbles down to this function, so this is a documented precondition
   rather than a checked one; callers other than sg_object_find_prefix must
   uphold it themselves. */
int sg_loose_find_prefix(const char *git_dir, const unsigned char prefix[SG_SHA1_RAW_LEN],
                         size_t nibbles, sg_oid_list *out);

#endif
