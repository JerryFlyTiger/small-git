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

/* Same object encoding as sg_pack_write (no delta compression; each object
   read via sg_object_read and stored as its own literal zlib-compressed
   entry), but built entirely in memory and handed back rather than written
   to objects/pack/ -- for a push, where the bytes need to go straight into
   an HTTP request body instead of onto local disk. *out is malloc'd, caller
   frees; it always ends with the pack's own trailing 20-byte SHA-1 checksum,
   same as an on-disk pack. Returns 0 on success, -1 on failure. */
int sg_pack_build_buf(const char *git_dir, const unsigned char (*ids)[SG_SHA1_RAW_LEN], size_t count,
                      unsigned char **out, size_t *out_len);

/* Writes already-formed packfile bytes (as received over the network, e.g.
   by sg_transport_fetch_pack) to git_dir/objects/pack/pack-<hex>.pack, where
   <hex> is the pack's own trailing 20-byte checksum -- not recomputed here,
   just read off the end of `data` (sg_pack_index_existing independently
   verifies it against a hash of the rest of the file). *pack_path_out is
   malloc'd, caller frees. Returns 0 on success, -1 on failure (including
   `data` not starting with the "PACK" magic). */
int sg_pack_store_raw(const char *git_dir, const unsigned char *data, size_t len,
                      char **pack_path_out);

/* Scans an existing *.pack file already placed at pack_path (which must be
   under some git_dir's objects/pack/ directory) and writes the matching
   *.idx next to it, in the same version-2 format sg_pack_write produces.
   Validates the pack's "PACK" magic, version, and whole-file trailer
   checksum before trusting any of its contents -- this data typically just
   came off the network. Every object's id, CRC32, and offset are recomputed
   from scratch (OFS_DELTA/REF_DELTA chains are reconstructed exactly like
   sg_pack_read, including the same SG_PACK_MAX_DELTA_DEPTH guard); a
   REF_DELTA base not found within this pack falls back to sg_object_read
   against git_dir's existing loose/pack storage. Returns 0 on success, -1 on
   any validation or I/O failure. */
int sg_pack_index_existing(const char *pack_path);

#endif
