#ifndef SG_TREE_BUILD_H
#define SG_TREE_BUILD_H

#include <stddef.h>

#include "sg/hash.h"
#include "sg/index.h"

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

/* Builds a tree from the index's stage-0 entries exactly as they stand (no
   working-tree access, no re-hashing). Returns 0, or -1 on allocation
   failure, if sg_tree_build itself fails (e.g. a loose object write fails),
   or if idx contains any stage 1/2/3 entry -- an unmerged index has
   no single tree, and silently picking one stage would record a tree the
   caller never asked for. Callers already gate on sg_index_has_unmerged;
   this makes that gate load-bearing rather than advisory. */
int sg_tree_build_from_index(const char *git_dir, const sg_index *idx,
                             unsigned char tree_id_out[SG_SHA1_RAW_LEN]);

/* For every path idx covers, hashes the WORKING TREE's current content into
   the object store (chunk-aware, honouring the repo's chunk config exactly
   as sg add does) and builds a tree from the results, falling back to the
   blob the index already records when the working-tree file is missing or
   unreadable, so every entry always resolves. When idx holds several stage
   1/2/3 entries for one path, exactly one representative is emitted, so the
   result never has two entries sharing a name. Returns 0, -1 on failure. */
int sg_tree_build_from_workdir(const char *git_dir, const char *repo_root, const sg_index *idx,
                               unsigned char tree_id_out[SG_SHA1_RAW_LEN]);

/* Builds a tree out of the working tree's untracked files (full relative
   paths, not flattened basenames) -- see sg_status_list_untracked for what
   counts as untracked and what include_ignored does. Every file's content is
   hashed and written as a blob. file_count_out, if non-NULL, reports how
   many files were included. No untracked files produces an empty tree and
   reports 0, not an error. Returns 0 on success, -1 on failure. */
int sg_tree_build_from_untracked(const char *git_dir, const char *repo_root, const sg_index *idx,
                                 int include_ignored,
                                 unsigned char tree_id_out[SG_SHA1_RAW_LEN],
                                 size_t *file_count_out);

#endif
