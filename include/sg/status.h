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
   unreadable) counts as SG_STATUS_DELETED. git_dir is used to normalize a
   chunked-storage pointer id from idx into the id of its actual content (see
   sg/chunk.h's sg_chunk_effective_id) before comparing against the working
   file's hash, so a chunked file that hasn't actually changed doesn't show up
   as permanently modified. Returns 0 on success, -1 on allocation failure. */
int sg_status_diff_unstaged(const char *git_dir, const char *repo_root, const sg_index *idx,
                            sg_status_list *out);

/* Lists every untracked file under repo_root, one entry per file (never
   folded into a "dir/" line the way some porcelain output does). A path
   already tracked at idx (any stage 0/1/2/3 -- see path_tracked_any_stage in
   the .c) never appears here, including a staged-delete: the file may still
   sit on disk with nothing left in the index, in which case it counts as
   untracked exactly like git does. include_ignored = 0 skips paths matched
   by the repo's .gitignore rules (opening and closing the ignore engine
   internally -- the caller does not manage one); 1 includes them too. *out
   is a malloc'd array of malloc'd strings, sorted by path (the walk itself
   is readdir order, which is unspecified; callers such as
   sg_tree_build_from_untracked rely on this being sorted rather than each
   having to sort it themselves -- sg_tree_build's flat-list contract
   requires it). On success the caller frees each entry and then the array.
   On the -1 (allocation) return, the function has already freed whatever it
   collected before failing and sets *out to NULL, *count to 0 -- callers
   must not free anything themselves in that case, and must never treat -1
   as "no untracked files", since silently under-reporting is how
   uncommitted work gets lost. Returns 0 on success, -1 on allocation
   failure. */
int sg_status_list_untracked(const char *git_dir, const char *repo_root, const sg_index *idx,
                             int include_ignored, char ***out, size_t *count);

#endif
