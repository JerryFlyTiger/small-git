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

/* Minimum length, in hex digits, of an abbreviated object id prefix this
   project accepts anywhere (Phase 68); shorter than this is always rejected,
   even when it happens to be unique -- matches git's own minimum. */
#define SG_OID_MIN_ABBREV 4

/* A growable list of raw object ids. ids is owned (malloc'd), NULL when
   empty; free with sg_oid_list_free. */
typedef struct {
    unsigned char (*ids)[SG_SHA1_RAW_LEN]; /* owned */
    size_t count, cap;
} sg_oid_list;

void sg_oid_list_free(sg_oid_list *l);

/* Appends id to *l, growing it as needed (doubling, starting at 16). Returns
   0 on success, -1 on malloc failure (l is left unchanged on failure). */
int sg_oid_list_append(sg_oid_list *l, const unsigned char id[SG_SHA1_RAW_LEN]);

/* Finds every object (loose or packed) whose id begins with hex_prefix,
   which must be SG_OID_MIN_ABBREV..39 hex characters (case-insensitive) --
   40 is an exact id, not a prefix, and belongs to sg_object_read instead.
   *out is populated fresh (any prior contents are overwritten, not freed by
   this call -- caller must not pass a list still holding a previous
   allocation). Results are deduplicated (the same object can be both loose
   and packed at once) and sorted ascending by raw id. Returns 0 on success
   -- count 0 (no object has this prefix) is a success, not an error -- or
   -1 if hex_prefix is malformed (wrong length or contains a non-hex
   character) or the object store itself is unreadable. */
int sg_object_find_prefix(const char *git_dir, const char *hex_prefix, sg_oid_list *out);

#endif
