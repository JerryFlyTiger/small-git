#ifndef SG_APPLY_H
#define SG_APPLY_H

#include "sg/hash.h"
#include "sg/index.h"

/* Makes the working directory and index match tree_id exactly: writes/
   overwrites every path in the tree, removes currently-tracked files absent
   from the tree, and rebuilds the index. Never touches HEAD/refs -- this is
   the logic factored out of `sg switch`, also reused by `sg undo`. Returns 0
   on success, -1 on failure (this phase does not add transactional rollback:
   on failure the working tree may already be partially updated, same as
   switch's pre-existing behavior). */
int sg_apply_tree_to_workdir(const char *git_dir, const char *repo_root,
                            const unsigned char tree_id[SG_SHA1_RAW_LEN]);

/* The full safety wrapper shared by switch/reset --hard/merge (fast-forward)/
   undo: checks whether the
   working directory is dirty (same sg_status_diff_staged/unstaged logic as
   the Phase 3 confirmation gate, including its out-of-memory-means-dirty
   fallback) -> if dirty, gates on sg_confirm_dangerous (force only skips the
   interactive prompt, *never* the snapshot below) -> if anything would
   actually be lost, calls sg_snapshot_create first -> then
   sg_apply_tree_to_workdir. label is used both as the snapshot commit
   message and in the confirmation prompt. Returns 0 on success; 1 if the
   user declined or a non-interactive run had no --force (nothing was
   changed); -1 if the snapshot or apply step hit an I/O error (if
   sg_confirm_dangerous was never satisfied, nothing was changed either; if
   the failure happened during apply, the same partial-change caveat as
   sg_apply_tree_to_workdir applies).

   After a successful apply, MERGE_HEAD is cleared if present (matches real
   git 2.55.0: any operation that resets the working directory clears an
   in-progress merge). A paused rebase's sequencer state is deliberately
   left untouched -- also matching real git, which only lets rebase's own
   subcommands (--abort, a completed run, --quit) end a sequence; even
   `switch --force` is refused rather than clobbering it (see cmd_switch.c).
   Callers that need the old "always wipe rebase state" behavior must do so
   themselves after this call returns. */
int sg_safe_apply_tree(const char *git_dir, const char *repo_root,
                       const unsigned char tree_id[SG_SHA1_RAW_LEN],
                       const char *label, int force);

/* Requires a perfectly clean working directory (no staged or unstaged
   changes) as a precondition for operations that would silently drop
   uncommitted state if allowed to proceed (merge, rebase) -- unlike
   sg_safe_apply_tree, there is no confirm-and-snapshot-then-overwrite path
   here, the operation is refused outright. `what` names the operation in the
   error message (e.g. "sg merge"). A failed staged/unstaged diff is treated
   as dirty, same fail-safe convention as the rest of the safety gates.
   Returns 0 if clean, 1 otherwise (message already printed to stderr). */
int sg_require_clean_workdir(const char *git_dir, const char *repo_root, const char *what);

/* Phase 79c: distinguishes an actual OOM from an I/O failure encountered
   while recursively scanning a candidate directory (opendir/readdir/lstat),
   so the caller can print a truthful message instead of always claiming
   "out of memory" -- measured against git 2.55.0 (S6 in the Phase 79c
   oracle): a chmod-000 subdirectory makes git print
   "warning: could not open directory '<dir>/': Permission denied" then
   "fatal: cannot opendir '<dir>': Permission denied", not an allocation
   failure at all. SG_UNTRACKED_ERR_PATH_TOO_LONG covers a path that
   overflows the SG_PATH_MAX join bound partway through the scan (this is
   the fail-closed branch for a path that could not even be verified, not a
   real OS errno, so `saved_errno` is reported as ENAMETOOLONG by
   convention). `path` is repo-root-relative and malloc'd (NULL if the
   allocation to record it itself failed, or if the kind is
   SG_UNTRACKED_ERR_ALLOC, which never has a path). Freed with
   sg_untracked_overwrite_error_free. */
typedef enum {
    SG_UNTRACKED_ERR_NONE = 0,
    SG_UNTRACKED_ERR_ALLOC,
    SG_UNTRACKED_ERR_PATH_TOO_LONG,
    SG_UNTRACKED_ERR_OPENDIR,
    SG_UNTRACKED_ERR_READDIR,
    SG_UNTRACKED_ERR_LSTAT
} sg_untracked_err_kind;

typedef struct {
    sg_untracked_err_kind kind;
    char *path;
    int saved_errno;
} sg_untracked_overwrite_error;

/* Frees err->path and resets *err to the all-zero/SG_UNTRACKED_ERR_NONE
   state. Safe to call on an err that was never filled in (path == NULL). */
void sg_untracked_overwrite_error_free(sg_untracked_overwrite_error *err);

/* Phase 79 (Phase 79b split the single result list into two buckets, see
   below). Of the repo-root-relative paths in `paths` (each one a path the
   caller is about to CREATE in the working tree, i.e. one the current
   HEAD/index does not have), reports those that cannot be created without
   destroying something untracked -- the check real git runs before a merge
   ("The following untracked working tree files would be overwritten by
   merge" / "Updating the following directories would lose untracked files
   in them"). This is deliberately NOT folded into sg_safe_apply_tree: `sg
   switch` and `sg reset --hard` share that function, and real git's answer
   for those two differs from merge's (reset --hard overwrites an untracked
   file without complaint). Callers opt in individually.

   Per candidate path P: lstat(P). If P itself exists (or lstat fails with
   ENOTDIR, meaning some ancestor component of P exists but is not a
   directory -- walk P's proper ancestors shortest-first and take the first
   one that exists and is not a directory as the blocker), let X be P or the
   blocker; otherwise (ENOENT, nothing anywhere along the path) P is clear.
   If X is P itself and is an EMPTY directory, there is also no collision --
   measured, git silently removes an empty untracked directory sitting
   exactly at a path it needs to create a file at. If X is tracked in `idx`
   (any stage) or is ignored, there is no untracked collision. Otherwise X
   itself is reported -- naming the blocker rather than P is what reproduces
   git's answer for "dir blocks dir/deep.txt" (names "dir"), and naming P
   rather than a case/normalization-aliased on-disk spelling is what
   reproduces git's answer for a folding filesystem (lstat resolves the
   alias, so X ends up being the caller's own spelling of P, not whatever is
   actually on disk).

   Phase 79b, measured against git 2.55.0: when X is a NON-EMPTY directory
   (not itself ignored), git does not always treat it as an unconditional
   collision the way a blocking FILE is -- it additionally requires that the
   directory contain, RECURSIVELY, at least one file that is not ignored. A
   non-empty directory whose entire contents (at any depth) are ignored
   merges through cleanly, exactly like an ignored file would. When it DOES
   contain a non-ignored file, git reports it through a DIFFERENT sentence
   from the file case ("Updating the following directories would lose
   untracked files in them", not "The following untracked working tree
   files..."), so such an X is placed in *out_dirs, never *out_files. An
   ignored subdirectory found during the recursive scan is not descended
   into (its own contents cannot make the outer directory "have a
   non-ignored file" if the subdirectory itself is ignored) -- git's ignore
   walk does not report through an ignored directory either. A symlink
   encountered during the scan is treated as a file (never descended into).

   The ignore query is done with a direct sg_ignore_push_dir/pop_dir walk of
   each candidate's own ancestor chain (opening one sg_ignore for the whole
   call), not by cross-referencing sg_status_list_untracked's output: that
   list holds the on-disk spelling of a name, so an exact string compare
   against it would silently miss the case/normalization-aliased row above,
   which is exactly a data-loss bug on the user's own machine.

   *out_files and *out_dirs are each a malloc'd array of malloc'd strings,
   in the SAME order as `paths` was walked (i.e. candidate order), NEITHER
   sorted NOR de-duplicated (corrected from this comment's own earlier,
   wrong claim: measured against git 2.55.0, two candidates blocked by the
   same ancestor each get their own line, so the SAME blocker name can
   appear more than once in a bucket -- git does not de-duplicate, and
   ordering a caller must not assume is byte-sorted, only that it tracks
   candidate order). A path can only ever land in one of the two buckets,
   since X is either a file/blocker-file or a non-empty directory, never
   both. On success (including "no collisions", which sets both *out to
   NULL and both *out_count to 0) the caller frees each string and then the
   array, in each bucket independently. Returns 0 on success, -1 on
   allocation failure
   (or a query against the ignore engine, or the recursive directory scan,
   failing on OOM/opendir/readdir/lstat) -- -1 must never be read as "no
   collisions" by any caller, same contract wording as
   sg_status_list_untracked; on -1, both *out are left NULL, nothing to
   free. A path too long to join with repo_root, or an lstat failure other
   than ENOENT/ENOTDIR (EACCES, ENAMETOOLONG, ELOOP), fails closed: P is
   reported as a collision (in *out_files) without a tracked/ignored check,
   rather than risk answering "clear" about something that could not
   actually be verified. Likewise, a failure while recursively scanning a
   candidate directory (opendir/readdir/lstat/ignore-engine error partway
   through, as opposed to a clean allocation failure) fails the whole call
   with -1 rather than guessing whether the unscanned remainder holds a
   non-ignored file.

   `out_first_is_dir` (Phase 79c, F4) is optional (NULL is fine). On
   success, if at least one collision was found, it is set to 1 if the
   FIRST collision in candidate order (across BOTH buckets) was a
   directory, 0 if it was a file; left at 0 (its initial value) if there
   were no collisions at all. This is what lets a caller reproduce git's
   unborn-HEAD wording, which names exactly one collision -- the first one
   in candidate order, not "directory always wins" (measured against git
   2.55.0: a topic adding both a file-colliding path and a
   directory-colliding path later than it in candidate order refuses on the
   FILE wording).

   `out_err` (Phase 79c) is optional (NULL is fine) and is only ever WRITTEN
   on a -1 return -- a caller that passes NULL still gets -1, it just cannot
   tell an OOM from a scan I/O error. On -1 with out_err != NULL, *out_err
   is filled with the kind of failure and, where applicable, the
   repo-root-relative path and errno involved; see sg_untracked_err_kind's
   own comment for exactly which kinds carry which fields. The caller owns
   the result and must free it with sg_untracked_overwrite_error_free. */
int sg_untracked_would_be_overwritten(const char *git_dir, const char *repo_root,
                                      const sg_index *idx,
                                      const char *const *paths, size_t count,
                                      char ***out_files, size_t *out_files_count,
                                      char ***out_dirs, size_t *out_dirs_count,
                                      int *out_first_is_dir,
                                      sg_untracked_overwrite_error *out_err);

/* Rewrites the on-disk index to exactly match tree_id (sha1 + mode of every
   path), WITHOUT touching the working directory or HEAD/refs -- the "only
   rewrite the index" half of sg_apply_tree_to_workdir that `sg reset --mixed`
   needs and nothing existing exposes. Paths tracked in the current index but
   absent from tree_id are dropped.

   Per-entry stat metadata (ctime/mtime/dev/ino/uid/gid/file_size) is copied
   from the current index's entry at that path only if that entry's sha1
   already equals the tree's -- content genuinely didn't change, so the old
   stat is still accurate. Otherwise every stat field is zeroed rather than
   populated from a workdir stat() call: this function never reads the
   working tree, and stat()-ing the workdir file here would be actively wrong
   whenever it differs from the tree being reset to (it would record the
   *new* sha1 next to the *current file's* mtime/size, and any future
   consumer that trusts stat over content would then see a false "clean").
   As of this writing sg_status_diff_unstaged always re-hashes file content
   and never shortcuts on stat alone, so zeroing is a defensive convention
   rather than a fix for an observed bug -- kept anyway in case that changes.

   Returns 0 on success (index file updated), -1 on failure (nothing
   written: the tree/index reads happen before any write). */
int sg_index_reset_to_tree(const char *git_dir, const unsigned char tree_id[SG_SHA1_RAW_LEN]);

#endif
