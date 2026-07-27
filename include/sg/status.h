#ifndef SG_STATUS_H
#define SG_STATUS_H

#include <stddef.h>

#include "sg/index.h"
#include "sg/tree_build.h"

typedef enum {
    SG_STATUS_NEW,
    SG_STATUS_MODIFIED,
    SG_STATUS_DELETED,
} sg_status_kind;

typedef struct {
    char *path; /* malloc'd, owned */
    sg_status_kind kind;
} sg_status_entry;

typedef struct {
    sg_status_entry *entries;
    size_t count;
    size_t cap;
} sg_status_list;

void sg_status_list_free(sg_status_list *list);

/* Changes staged relative to HEAD: compares head_flat (the flattened HEAD
   tree; pass a zeroed/empty sg_flat_list when there is no commit yet, in
   which case every index entry shows up as SG_STATUS_NEW) against idx. Both
   inputs must be sorted by path, which is always true for sg_tree_flatten's
   output and for an sg_index. Returns 0 on success, -1 on allocation
   failure. */
int sg_status_diff_staged(const sg_flat_list *head_flat, const sg_index *idx, sg_status_list *out);

/* Changes not yet staged: compares idx against the actual working directory
   contents under repo_root. A path missing from the working directory (or
   unreadable) counts as SG_STATUS_DELETED. Returns 0 on success, -1 on
   allocation failure. */
int sg_status_diff_unstaged(const char *repo_root, const sg_index *idx, sg_status_list *out);

#endif
