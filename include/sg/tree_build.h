#ifndef SG_TREE_BUILD_H
#define SG_TREE_BUILD_H

#include <stddef.h>

#include "sg/hash.h"
#include "sg/index.h"
#include "sg/pathspec.h"

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
   (subdirectories are walked into but not themselves present in the output).
   Returns 0 on success, -1 if an object is missing or corrupt, -2 if some
   entry name in the tree fails sg_path_component_is_safe -- e.g. a crafted
   or foreign commit naming an entry ".git". The two are kept apart on
   purpose: -1 means "this repository is broken", -2 means "this tree is
   hostile"; reporting the former's message for the latter would describe a
   thwarted attack as data corruption. -2 is still non-zero, so every
   existing "!= 0" caller stays fail-closed without changes.
   If bad_path is non-NULL and the call returns -2, the offending
   repo-relative path is copied into it (needs SG_PATH_MAX bytes). */
int sg_tree_flatten(const char *git_dir, const unsigned char tree_id[SG_SHA1_RAW_LEN],
                    sg_flat_list *out, char *bad_path);

void sg_flat_list_free(sg_flat_list *list);

/* Phase 52 test hook: sg_tree_flatten_test_count returns how many times
   sg_tree_flatten has been called (process lifetime, or since the last
   reset); sg_tree_flatten_test_reset zeroes it. These exist purely for
   observability -- to let a test prove a refactor actually removed redundant
   flattening -- and nothing in src/ may branch on the count. Not
   thread-safe: the counter is a plain file-scope size_t with no atomics or
   locking. */
size_t sg_tree_flatten_test_count(void);
void sg_tree_flatten_test_reset(void);

/* Builds a tree from the index's stage-0 entries exactly as they stand (no
   working-tree access, no re-hashing). Returns 0, or -1 on allocation
   failure, if sg_tree_build itself fails (e.g. a loose object write fails),
   or if idx contains any stage 1/2/3 entry -- an unmerged index has
   no single tree, and silently picking one stage would record a tree the
   caller never asked for. Callers already gate on sg_index_has_unmerged;
   this makes that gate load-bearing rather than advisory. */
int sg_tree_build_from_index(const char *git_dir, const sg_index *idx,
                             unsigned char tree_id_out[SG_SHA1_RAW_LEN]);

/* What sg_tree_build_from_workdir records for an index path whose file is
   not in the working tree at all. The two callers need OPPOSITE answers, so
   this is a required argument with no default and no zero-means-something
   convention: silently picking one of the two is exactly the bug this enum
   exists to make unrepresentable (docs/DESIGN.md, Phase 20 item 1). */
typedef enum {
    /* Record the blob the index already holds, so every index entry appears
       in the resulting tree no matter what the working tree looks like.
       What sg_snapshot_create needs: a safety net that omits the file the
       user just deleted cannot restore it. */
    SG_WORKDIR_MISSING_KEEP_INDEX_BLOB = 0,
    /* Omit the path, so the resulting tree records the deletion. What
       sg_stash_push needs: under the policy above, "the only change is a
       deleted tracked file" builds a tree identical to HEAD's, and the push
       reports "No local changes to save" instead of stashing anything. */
    SG_WORKDIR_MISSING_RECORD_DELETION = 1
} sg_workdir_missing;

/* For every path idx covers, hashes the WORKING TREE's current content into
   the object store (chunk-aware, honouring the repo's chunk config exactly
   as sg add does) and builds a tree from the results. When a path's file is
   missing from the working tree, missing selects what happens to that
   entry (see sg_workdir_missing above). When idx holds several stage
   1/2/3 entries for one path, exactly one representative is emitted, so the
   result never has two entries sharing a name. Every entry resolves, or the
   call fails: a path that exists but can't be read (e.g. EISDIR, I/O
   error) is always a hard failure under either policy, never silently
   treated as "missing" -- a snapshot or stash that recorded a stale blob
   instead would be worse than no snapshot at all. Under
   RECORD_DELETION the result can cover fewer paths than idx, down to an
   empty tree if every path was deleted -- that is success, not an error.

   Phase 36: also hard-fails (before ever touching the working tree for that
   entry) if an index path fails sg_relpath_is_safe -- e.g. "../secret.txt"
   or a path under ".git/". sg_index_read validates nothing about index
   paths (by design, see CLAUDE.md), and this function is the one index
   consumer that both reads outside the repository AND turns what it read
   into a permanent loose object; every other consumer either only reads
   inside the repository already, or never persists what it read.

   `ps` (Phase 37) adds a third, ORTHOGONAL dimension on top of `missing`,
   used by sg_stash_push's partial pathspec push: for an index path `ps`
   does NOT match, the working tree is never even consulted (no lstat, no
   read, no hash) -- the index's own blob and mode are copied straight into
   the result, exactly as SG_WORKDIR_MISSING_KEEP_INDEX_BLOB would handle a
   missing file, REGARDLESS of which `missing` policy this call was given
   and regardless of whether the file on disk was deleted, unreadable, or
   simply unchanged. This is deliberately a separate parameter, not folded
   into sg_workdir_missing: "does this path count this round" (a property of
   the CALL, i.e. the pathspec) and "how to record a path whose file is
   gone" (a property of that specific path's own state) are independent --
   sg_stash_push's partial push needs BOTH RECORD_DELETION (for a matched,
   deleted path) and "ignore the working tree entirely" (for every
   unmatched path) within the same call, which a single enlarged enum could
   not express. NULL (or an empty pathspec) matches every path and
   reproduces this function's pre-Phase-37 behaviour exactly; every existing
   caller (safety/snapshot.c, and sg_stash_push's own unfiltered index-tree
   build) passes NULL. The Phase 36 path-safety guard below still runs for
   EVERY index path unconditionally, matched or not -- ps only ever narrows
   which paths touch the working tree, it must never narrow which paths get
   validated.

   Returns 0, -1 on failure. bad_path may be NULL; when non-NULL and the
   failure is specifically the Phase 36 path-safety guard above (never for
   any other -1 cause -- OOM, an unreadable file, a write failure), the
   offending repo-relative path is copied into it (needs SG_PATH_MAX bytes)
   so the caller can print an actionable message instead of a guess, same
   convention as sg_tree_flatten's bad_path and sg_status_diff_staged's. */
int sg_tree_build_from_workdir(const char *git_dir, const char *repo_root, const sg_index *idx,
                               sg_workdir_missing missing, const sg_pathspec *ps,
                               unsigned char tree_id_out[SG_SHA1_RAW_LEN], char *bad_path);

/* Builds a tree out of the working tree's untracked files (full relative
   paths, not flattened basenames) -- see sg_status_list_untracked for what
   counts as untracked and what include_ignored does. Every file's content is
   hashed and written as a blob. file_count_out, if non-NULL, reports how
   many files were included. No untracked files produces an empty tree and
   reports 0, not an error.

   `ps` (Phase 37) is forwarded straight to sg_status_list_untracked, so this
   builds the untracked parent of a partial `sg stash push -- <pathspec>`
   (PHASE37_SPEC.md B2's untracked_tree, containing ONLY the matched
   untracked files). NULL matches every path, same convention as everywhere
   else. Returns 0 on success, -1 on failure. */
int sg_tree_build_from_untracked(const char *git_dir, const char *repo_root, const sg_index *idx,
                                 const sg_pathspec *ps, int include_ignored,
                                 unsigned char tree_id_out[SG_SHA1_RAW_LEN],
                                 size_t *file_count_out);

#endif
