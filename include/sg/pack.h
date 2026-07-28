#ifndef SG_PACK_H
#define SG_PACK_H

#include <stddef.h>

#include "sg/hash.h"
#include "sg/object.h"

/* Scans git_dir/objects/pack/ for a *.idx + *.pack pair containing `id`,
   reading it and reconstructing OFS_DELTA/REF_DELTA chains as needed (a
   REF_DELTA base -- and, transitively, any base reached that way -- is
   looked up in loose storage or other packs, so it's found regardless of
   where it actually lives; OFS_DELTA bases are always within the same pack,
   per the format). Delta chain length is capped (SG_PACK_MAX_DELTA_DEPTH in
   pack.c) so a crafted/corrupt pack with a delta cycle or an absurdly long
   chain fails cleanly instead of recursing until the stack overflows.
   *content_out is malloc'd, caller frees. Returns 0 on success, -1 if not
   found in any pack, malformed, or the chain depth limit was hit -- this is
   not by itself proof the object doesn't exist at all, since loose storage
   hasn't necessarily been checked (see sg_object_read). */
int sg_pack_read(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                 sg_obj_type *type_out, unsigned char **content_out, size_t *content_len_out);

/* Writes `count` objects (read via sg_object_read, so already-packed or
   loose objects both work) into a single new pack with no delta compression
   -- every object is stored as its own literal zlib-compressed content. This
   is a valid, fully git-compatible pack, just not a space-optimized one.
   Writes git_dir/objects/pack/pack-<40 hex>.pack and the matching .idx,
   named after the pack's own trailer checksum. Returns 0 on success, -1 on
   failure (including a pack that would need the >2GB large-offset .idx
   format, which isn't supported). */
int sg_pack_write(const char *git_dir, const unsigned char (*ids)[SG_SHA1_RAW_LEN], size_t count);

#endif
