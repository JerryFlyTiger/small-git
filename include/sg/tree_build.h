#ifndef SG_TREE_BUILD_H
#define SG_TREE_BUILD_H

#include <stddef.h>

#include "sg/hash.h"

typedef struct {
    char *path; /* malloc'd, owned; repo-root-relative, '/'-separated */
    unsigned int mode;
    unsigned char sha1[SG_SHA1_RAW_LEN];
} sg_flat_entry;

typedef struct {
    sg_flat_entry *entries; /* sorted by path */
    size_t count;
} sg_flat_list;

/* Recursively builds nested tree objects out of a flat, path-sorted list of
   blob entries (the same order as the index), writing every tree object via
   sg_loose_write, and returns the root tree id. */
int sg_tree_build(const char *git_dir, const sg_flat_entry *entries, size_t count,
                  unsigned char tree_id_out[SG_SHA1_RAW_LEN]);

/* Recursively expands tree_id into a flat, path-sorted list of blob entries
   (subdirectories are walked into but not themselves present in the output). */
int sg_tree_flatten(const char *git_dir, const unsigned char tree_id[SG_SHA1_RAW_LEN],
                    sg_flat_list *out);

void sg_flat_list_free(sg_flat_list *list);

#endif
