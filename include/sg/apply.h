#ifndef SG_APPLY_H
#define SG_APPLY_H

#include "sg/hash.h"

/* Makes the working directory and index match tree_id exactly: writes/
   overwrites every path in the tree, removes currently-tracked files absent
   from the tree, and rebuilds the index. Never touches HEAD/refs -- this is
   the logic factored out of `sg switch`, also reused by `sg undo`. Returns 0
   on success, -1 on failure (this phase does not add transactional rollback:
   on failure the working tree may already be partially updated, same as
   switch's pre-existing behavior). */
int sg_apply_tree_to_workdir(const char *git_dir, const char *repo_root,
                            const unsigned char tree_id[SG_SHA1_RAW_LEN]);

/* The full safety wrapper shared by switch/restore/undo: checks whether the
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
   sg_apply_tree_to_workdir applies). */
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
