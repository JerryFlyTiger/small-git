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

/* How sg_status_list_untracked should report a directory that is entirely
   untracked (no tracked path anywhere below it). Deliberately no default --
   same reasoning as sg_workdir_missing in tree_build.h: a caller that needs
   file-per-line output (sg_stash_push and sg_tree_build_from_untracked need
   every actual file, not a directory name they cannot hash) and a caller
   that wants git's porcelain/long "dir/" folding must each say so
   explicitly, or one of them silently gets the other's behavior. */
typedef enum {
    SG_STATUS_UNTRACKED_LIST_FILES, /* never folds: one entry per file */
    SG_STATUS_UNTRACKED_FOLD_DIRS,  /* folds a wholly-untracked dir into one "dir/" entry */
} sg_status_untracked_fold;

/* Changes staged relative to HEAD: compares head_flat (the flattened HEAD
   tree; pass a zeroed/empty sg_flat_list when there is no commit yet, in
   which case every index entry shows up as SG_STATUS_NEW) against idx. Both
   inputs must be sorted by path, which is always true for sg_tree_flatten's
   output and for an sg_index. Returns 0 on success, -1 on allocation
   failure. */
int sg_status_diff_staged(const sg_flat_list *head_flat, const sg_index *idx, sg_status_list *out);

/* Changes not yet staged: a thin adapter over sg_diff_index_workdir
   (include/sg/diff.h), which does the actual index-vs-workdir walk. A path
   missing from the working directory, or that exists but is unreadable (e.g.
   permission denied), counts as SG_STATUS_DELETED -- it is never enough to
   fail this whole call, and never silently omitted either: unlike a plain
   allocation failure, one unreadable path must not take down the rest of the
   list. git_dir is passed through so the underlying builder can normalize a
   chunked-storage pointer id from idx into the id of its actual content, so a
   chunked file that hasn't actually changed doesn't show up as permanently
   modified. Mode is compared too (mode-only changes, e.g. a bare chmod with
   unchanged content, show up as SG_STATUS_MODIFIED). An unresolved conflict
   (any path with a stage 1/2/3 index entry) is deliberately excluded: `sg
   status` surfaces those through its own "Unmerged paths" section instead,
   and the associated stage-2-vs-workdir comparison row that
   sg_diff_index_workdir may also emit for the same path is likewise excluded
   -- see the .c file's comment on sg_status_diff_unstaged.

   Invariant relied on above (in the "unresolved conflict" paragraph) and by
   the adapter's own implementation: a non-unmerged path never produces more
   than one row in sg_diff_index_workdir's output, so two consecutive rows
   sharing the same path can only happen for an unmerged path's stage-2-vs-
   workdir companion row. Anything that makes sg_diff_index_workdir emit a
   second row for an ordinary path (e.g. a future rename-detection feature)
   must update this adapter's row-skipping logic in lockstep, or it will
   silently start dropping that path's second row here too.

   Returns 0 on success, -1 on allocation failure (this function's own, or
   sg_diff_index_workdir's -- see sg/diff.h; a chunk pointer whose data is
   missing or corrupt does NOT fail the call -- sg_diff_index_workdir cannot
   answer "did this path change" in that case, so it reports the path as
   modified instead of failing the whole walk, and this adapter passes that
   through unchanged; see sg_diff_index_workdir's own contract in sg/diff.h
   for the full reasoning. This is a deliberate diagnostic downgrade: before
   this adapter existed, a broken chunk pointer made this function print
   "cannot fully determine working directory status" through its callers'
   safety gates; now the same path
   is indistinguishable from an ordinary edit. Safety is unaffected --
   unstaged.count > 0 still makes every dirty-workdir gate refuse -- but the
   caller no longer learns that the underlying cause was a corrupt chunk
   pointer rather than a real edit). */
int sg_status_diff_unstaged(const char *git_dir, const char *repo_root, const sg_index *idx,
                            sg_status_list *out);

/* Lists every untracked path under repo_root. A path already tracked at idx
   (any stage 0/1/2/3 -- see path_tracked_any_stage in the .c) never appears
   here, including a staged-delete: the file may still sit on disk with
   nothing left in the index, in which case it counts as untracked exactly
   like git does. include_ignored = 0 skips paths matched by the repo's
   .gitignore rules (opening and closing the ignore engine internally -- the
   caller does not manage one); 1 includes them too.

   fold == SG_STATUS_UNTRACKED_LIST_FILES lists one entry per file, never
   folded into a "dir/" line, matching the historical behavior every
   non-status caller relies on. fold == SG_STATUS_UNTRACKED_FOLD_DIRS
   collapses a directory that holds no tracked path anywhere below it into a
   single "dir/" entry (trailing slash) once it finds any non-ignored file
   under it, matching git's porcelain/long status output; the fold decision
   is made independently per directory (a folded directory's own
   sub-directories are never walked into, but a directory that is NOT
   folded -- because something below it is tracked -- still lets its own
   untracked children fold independently). Under FOLD_DIRS, a directory
   whose files are all ignored is omitted entirely unless include_ignored is
   set, in which case it is folded to "dir/" too; a directory folded because
   it has a non-ignored file still lists each ignored file under it
   individually when include_ignored is set (the fold only ever collapses
   the non-ignored side of that directory).

   *out is a malloc'd array of malloc'd strings, sorted by path (the walk
   itself is readdir order, which is unspecified; callers such as
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
                             int include_ignored, sg_status_untracked_fold fold, char ***out,
                             size_t *count);

#endif
