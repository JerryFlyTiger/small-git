#ifndef SG_STATUS_H
#define SG_STATUS_H

#include <stddef.h>

#include "sg/index.h"
#include "sg/pathspec.h"
#include "sg/similarity.h"
#include "sg/tree_build.h"

typedef enum {
    SG_STATUS_NEW,
    SG_STATUS_MODIFIED,
    SG_STATUS_DELETED,
} sg_status_kind;

typedef struct {
    char *path; /* malloc'd, owned; the NEW path when this row is a rename */
    /* Non-NULL only for a rename: where the content came from. Owned.
       A rename is spelled this way rather than as a fourth sg_status_kind so
       that every existing switch over kind stays exhaustive and correct --
       the same shape sg_diff_entry uses, and for the same reason. A renamed
       row's `kind` is SG_STATUS_MODIFIED, so a caller that does not know
       about renames still sees "this path changed", never "nothing here". */
    char *old_path;
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

/* Changes staged relative to HEAD: what `git status` lists under "Changes to
   be committed". Since Phase 32 this is a thin adapter over sg_diff_tree_index
   rather than a second walk of its own -- one place that decides cannot
   disagree with itself, the same reasoning as sg_status_diff_unstaged. The
   two implementations were first proven equivalent by a differential harness
   (tests/test_status_staged_parity.c) across every shape either could see,
   so the convergence changed no behaviour at all.

   `head_tree` may be NULL for an unborn HEAD, which reads as an empty tree.

   Unmerged paths are left out, exactly as before: they have no single staged
   blob and `sg status` reports them in its own "Unmerged paths" section.

   `rename_score` is on git's 0..SG_SIMILARITY_MAX scale (see sg/similarity.h)
   and has NO default on purpose -- every caller must say which it wants:

     0                      every rename stays a deletion plus an addition.
     SG_SIMILARITY_DEFAULT  pair them, git's 50% threshold, as `git status` does.

   The reason it is mandatory is that the two answers are not
   interchangeable for the callers that already exist. src/workdir/apply.c's
   two safety gates enumerate this list to tell the user what is uncommitted;
   a rename row carries TWO paths, and a gate that reads only `path` would
   quietly stop naming the old one. Those gates therefore pass 0 and keep the
   list they have always had. Silently picking a side here would be exactly
   the bug this parameter exists to prevent.

   `ps` (Phase 37) filters the finished list, applied AFTER
   sg_diff_tree_index but BEFORE sg_diff_detect_renames -- CLAUDE.md's rule
   for sg_diff_list_filter (Phase 29: filtering after rename detection can
   turn a real rename into a plain "A" because only half the pair survives)
   applies here for exactly the same reason. NULL matches everything, same
   as sg_diff_list_filter's own NULL convention. apply.c's two safety gates
   always pass NULL: they need the unfiltered list to tell the user
   everything that is uncommitted.

   Returns 0, -1 on failure (including a HEAD tree that cannot be read), or
   -2 if head_tree names a hostile entry (sg_tree_flatten's own -2 -- e.g. a
   crafted commit with a ".." or ".git" path component), same convention as
   sg_diff_tree_index/sg_tree_flatten. A failure must never be read as "no
   changes": every caller here treats both -1 and -2 as dirty, which is the
   safe direction. bad_path may be NULL; when non-NULL and the return is -2,
   the offending repo-relative path is copied into it (needs SG_PATH_MAX
   bytes) so the caller can print an actionable message instead of a guess. */
int sg_status_diff_staged(const char *git_dir, const char *repo_root,
                          const unsigned char *head_tree, const sg_index *idx,
                          int rename_score, const sg_pathspec *ps, sg_status_list *out,
                          char *bad_path);

/* Removes every entry (old_path checked too, for a rename row) that does not
   match ps from an already-built sg_status_list. NULL ps leaves the list
   untouched -- same "NULL matches everything" convention as
   sg_diff_list_filter/sg_pathspec_matches. Used by callers (e.g. `sg
   status`'s unstaged list) where filtering has no rename-detection ordering
   concern to worry about, unlike sg_status_diff_staged's `ps` parameter
   above. */
void sg_status_list_filter(sg_status_list *list, const sg_pathspec *ps);

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

   `ps` (Phase 37) is the one deliberate, named exception to CLAUDE.md's
   "filter after the builder, never inside it" rule: how deep a wholly-
   untracked directory folds is itself a function of the pathspec (measured
   against git 2.55.0 -- `-- wholly/u1.txt` lists that one file instead of
   folding to "wholly/", while `-- 'wholly/star'` still folds to "wholly/"
   despite matching individual files below it), so filtering the flat result
   of an already-folded walk cannot recover the right answer: a folded entry
   "wholly/" cannot be matched against a spec naming a file below it, and the
   file would silently vanish instead of being unfolded. NULL matches
   everything and reproduces the pre-Phase-37 walk exactly; every existing
   caller (safety/stash.c, workdir/tree_build.c) passes NULL, since they need
   real per-file names regardless of any pathspec.

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
                             const sg_pathspec *ps, int include_ignored,
                             sg_status_untracked_fold fold, char ***out, size_t *count);

#endif
