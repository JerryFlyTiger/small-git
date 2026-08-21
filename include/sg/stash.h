#ifndef SG_STASH_H
#define SG_STASH_H

#include <stddef.h>

#include "sg/hash.h"

typedef struct {
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    char *message; /* malloc'd, owned. The reflog subject verbatim, e.g.
                      "On master: fix" -- exactly what `git stash list`
                      prints after the "stash@{N}: " prefix. */
} sg_stash_entry;

typedef struct {
    sg_stash_entry *entries; /* NEWEST FIRST: entries[N] is stash@{N}, the
                                reverse of the reflog's file order */
    size_t count;
} sg_stash_list;

/* Reads the stash stack from logs/refs/stash. No reflog (or an empty one)
   means an empty stack, returns 0 -- `git stash list` on a repo that never
   stashed prints nothing and exits 0. Deliberately does NOT consult
   refs/stash: real git addresses stash@{N} purely by reflog line, and
   ignores refs/stash when listing (measured -- deleting the reflog while
   keeping refs/stash makes `git stash list` print nothing). Returns -1 only
   on a corrupt reflog. */
int sg_stash_list_read(const char *git_dir, sg_stash_list *out);
void sg_stash_list_free(sg_stash_list *list);

/* Parses a user-supplied stash spec into a stack index: "stash@{N}", a bare
   "N", or NULL/"" meaning 0 (real git accepts all three -- measured
   `git stash drop 0`). Returns 0 and sets *index_out, or -1 on any other
   shape or on numeric overflow. Deliberately does NOT range-check against
   the stack: the caller owns the "only has N entries" message, which needs
   the count. Prints nothing. */
int sg_stash_parse_spec(const char *spec, size_t *index_out);

/* Options for sg_stash_push. A NULL opts pointer is equivalent to every
   field zeroed (message NULL, all three flags 0).

   include_untracked (-u) and include_ignored (-a, which implies -u --
   passing include_ignored alone has the same effect as passing both) give
   the stash commit a THIRD parent: a root commit (no parent of its own)
   whose tree holds only the untracked files (include_ignored also pulls in
   ignored ones), full relative paths. The stash's own tree (parent commit's
   tree, i.e. the "tracked" half) is IDENTICAL whether or not either flag is
   set -- untracked files live only in the third parent (measured against
   real git 2.55.0, Phase 20 spec sec 1.1). Once the stash has anything to
   save at all, the third parent is written unconditionally, even when the
   untracked file list is empty (an empty tree) -- there is no "fall back to
   2 parents" optimization (spec sec 1.6). */
typedef struct {
    const char *message;   /* NULL selects the "WIP on <branch>: ..." form */
    int include_untracked; /* -u */
    int include_ignored;   /* -a (implies -u) */
    int keep_index;        /* --keep-index: reset the working tree to the
                               INDEX tree instead of HEAD's tree, leaving
                               staged changes staged (measured against real
                               git: this is not a separate code path, just a
                               different tree passed to the same reset). */
} sg_stash_push_opts;

/* Creates a new stash from the current index and working tree and resets
   the working tree back to either HEAD's tree, or (opts->keep_index) the
   index tree. opts->message NULL selects the "WIP on <branch>: <short>
   <subj>" form; otherwise the "On <branch>: <message>" form is used. Writes
   the index commit (parent 2) unconditionally -- real git refuses to treat a
   commit with fewer than two parents as a stash at all (measured: a forged
   1-parent stash lists, but `show`/`apply`/`pop` die with "not a stash-like
   commit"), so it is not optional.

   When include_untracked or include_ignored is set, "nothing to save" also
   requires the untracked file list (filtered the same way the third parent
   would be) to be empty -- a worktree that is clean except for an untracked
   file is NOT nothing-to-save under -u/-a even though it is under plain
   push (measured, spec sec 1.5).

   With either flag set, after the working tree is reset (to head_tree, then
   to index_tree too if keep_index), every file that went into the third
   parent's tree is removed from the working tree, and any directory left
   physically empty by that removal is pruned (recursively upward). This
   step does NOT consult ignore status again -- a directory that still holds
   an ignored file under -u (untracked, but not swept up) is not empty and
   is left alone; the same directory under -a (which does sweep it) is
   removed once its last file is gone (measured, spec sec 1.4).

   Returns 0 when a stash was created, 1 when there was nothing to save
   (working tree and index both already equal HEAD, and -- see above -- no
   untracked files if -u/-a was given -- nothing on disk was touched, and
   the caller prints "No local changes to save" and exits 0, matching git),
   -1 on failure before anything durable was written (refuses this way on an
   unborn HEAD or an unmerged index -- the caller may report "nothing
   happened"). Returns -2 when the stash commit + refs/stash were already
   written durably (so it IS on `sg stash list`) but a later step -- the
   snapshot, the working-tree reset, or (under -u/-a) removing the taken
   untracked files/pruning now-empty directories -- failed; the caller must
   not claim nothing was created, and should tell the user the entry exists
   and the working tree may not have been reset. Deliberately does NOT touch
   a paused rebase's sequencer state, and does NOT remove MERGE_HEAD -- both
   are the CLI layer's job (see cmd_stash.c). */
int sg_stash_push(const char *git_dir, const char *repo_root, const sg_stash_push_opts *opts,
                  unsigned char commit_id_out[SG_SHA1_RAW_LEN]);

/* Checks whether apply/pop of stash entry `index` would be forced to
   overwrite an uncommitted change -- the replacement for the old blanket
   "working directory must be perfectly clean" precondition (Phase 20 spec
   sec 4). Only paths the merge (base = the stash's first parent's tree,
   ours = HEAD's tree, theirs = the stash's tree) actually TOUCHES are
   examined; a path it leaves alone is never flagged even if it is dirty.
   Two kinds of collision are checked:

     - the working tree still has the touched path and its content differs
       from HEAD (this includes the case where that content happens to
       already equal what the stash would write -- the rule looks at
       whether the path differs from HEAD, not from the stash);
     - the index already differs from HEAD at that touched path.

   A touched path that is simply missing from the working tree is NOT
   flagged (nothing there to overwrite). An untracked file blocking a path
   the stash would newly CREATE is NOT flagged here either -- that is
   sg_stash_apply's own pre-flight collision check's job (see its header
   comment below), which already exists and already covers both the tracked
   and the -u/-a untracked half; duplicating it here would just produce a
   second, differently-worded rejection for the same condition.

   The index-differs-from-HEAD check is a DELIBERATE DIVERGENCE from real
   git (measured, Phase 20 spec sec 4.2 row 8): real git's "ours" is the
   index, so it can three-way-merge a staged change against the stash (and
   possibly conflict); sg's "ours" is HEAD, so letting a staged change on a
   touched path through here would silently overwrite it. sg refuses
   instead -- strictly safer, at the cost of not matching real git's
   more permissive behavior on that one row.

   On success (or on a genuine collision), *dirty_paths_out is a malloc'd,
   path-sorted array of malloc'd path strings, *dirty_count_out its length;
   both are NULL/0 when nothing collided. Caller frees each entry, then the
   array. Returns 0 when it is safe to proceed, 1 when at least one path
   collided, -1 on error (bad index/out-of-range index/corrupt stash commit/
   unreadable tree or object/allocation failure -- same causes as
   sg_stash_apply below; *dirty_paths_out and *dirty_count_out are left at
   NULL/0). */
int sg_stash_apply_check_dirty(const char *git_dir, const char *repo_root, size_t index,
                               char ***dirty_paths_out, size_t *dirty_count_out);

/* Three-way merges stash entry `index` into the current HEAD (base = the
   stash's first parent's tree, ours = HEAD's tree, theirs = the stash's
   tree), writing the result into the working tree and the index. The caller
   is expected to have already run sg_stash_apply_check_dirty (or an
   equivalent gate) -- unlike before Phase 20, the working directory no
   longer needs to be perfectly clean, only clean on every path the merge
   actually touches; sg's "ours" is HEAD's tree regardless. Conflict markers
   are labelled "Updated upstream" / "Stashed changes" (measured -- git uses
   those literal strings here, not branch names).

   The working tree itself is left alone on any path the merge does NOT
   touch (sg_merge_result_apply, as of Phase 20, skips rewriting a path
   whose resolved outcome already equals HEAD's own -- see its header
   comment in merge.h): a dirty-but-untouched file's uncommitted on-disk
   content survives an apply/pop exactly as sg_stash_apply_check_dirty's
   gate above promises it will.

   On a clean merge the index is left as real git leaves it without --index:
   for every path the merge TOUCHES, equal to HEAD's tree there, except that
   a path present in the merge result but absent from HEAD stays staged
   (measured: ` M tracked.txt` but `A  newfile.txt`). For a path the merge
   does NOT touch, whatever was already staged there before this call stays
   staged untouched (Phase 20 spec sec 4.4) -- including a path staged but
   absent from HEAD entirely (e.g. `git add`ed after the stash was pushed):
   sg_merge_trees never even produces a result entry for it, so it is
   restored from a second pass over the pre-apply index rather than from
   head_flat/result. Under the old clean-workdir precondition the index was
   always identical to HEAD's own version anyway, so this is not a behavior
   change there, only where a dirty-but-untouched path is now allowed
   through at all. On conflict the merge-result index is
   written as-is (stages 1/2/3 for conflicted paths, stage 0 for cleanly
   merged ones -- measured `UU c.txt` alongside `M  clean.txt`), and
   MERGE_HEAD is NOT written (git does not create one here).

   restore_index (--index, Phase 20 spec sec 3): on a CLEAN merge only,
   after the above, the index is entirely replaced with the stash's own
   index tree (parents[1] -- the tree the index had at `stash push` time),
   restoring the exact staged state that was stashed away, independent of
   what the rules above computed. On conflict this step is skipped outright
   (measured against real git) and the index is left exactly as the rules
   above already left it -- the caller must tell the user the index was not
   restored (sg does not print this itself; see cmd_stash.c). When the
   stash's index tree never differed from its base tree (nothing was staged
   at push time), this is simply a no-op -- no special case needed.

   A 3-parent entry (a `git stash -u`/`-a` stash, see sg_stash_push_opts) is
   also accepted: the third parent's tree (full relative paths) is restored
   into the working tree as untracked files -- written to disk, never staged
   -- creating any nested directories those paths need.

   DELIBERATE DIVERGENCE from real git (measured, Phase 20 spec sec 1.7):
   when a path the untracked half would restore already exists on disk, real
   git applies the tracked half regardless and then fails the untracked half
   per-file, leaving the stash entry around with only some of its untracked
   files restored -- a state with no clean way out (popping again collides
   with the files it just restored). sg instead folds the untracked paths
   into the SAME pre-flight collision check it already runs for the tracked
   half (see the untracked-in-HEAD collision check below) and refuses the
   whole apply, writing nothing, if anything collides on either half. This
   is strictly safe (an apply either fully succeeds or touches nothing) at
   the cost of not matching real git's partial-apply behavior here.

   Returns 0 on a clean apply, 1 when there were conflicts (working tree and
   index are left in the conflicted state, the stash entry is untouched and
   the caller must not drop it -- and, if restore_index was set, the index
   restore was skipped), -1 on failure. Returns -1 if the entry has fewer
   than 2 or more than 3 parents -- that is not a shape sg (or real git)
   ever produces for a stash. */
int sg_stash_apply(const char *git_dir, const char *repo_root, size_t index, int restore_index);

/* Removes entry `index` from the stack: rewrites the reflog without that
   line (re-chaining old_ids, preserving every surviving entry's ident and
   message verbatim) and re-points refs/stash at the new newest entry. When
   the stack becomes empty, refs/stash is deleted from BOTH the loose file
   and packed-refs and the reflog file is removed, exactly as `git stash
   drop` of the last entry does.

   refs/stash MUST end up equal to the new last reflog line's new-oid: real
   git walks the reflog backwards from the ref's current value and reports
   "log for ref refs/stash unexpectedly ended" if they disagree, which makes
   every later stash@{N} unusable (measured). Returns 0, -1 on failure or if
   index is out of range. */
int sg_stash_drop(const char *git_dir, size_t index);

/* Empties the whole stack: deletes refs/stash from both stores and removes
   the reflog. Not an error when there is no stash (git stash clear on an
   empty stack prints nothing and exits 0). Returns 0, -1 on I/O failure. */
int sg_stash_clear(const char *git_dir);

/* The four trees a stash entry decomposes into (Phase 25 WP4 -- shared by
   sg_stash_apply_check_dirty, sg_stash_apply, and `sg stash show`, so the
   three never drift on what counts as a valid stash-shaped commit):

     - base_tree: parents[0]'s tree -- HEAD's tree at `stash push` time, and
       the diff base `git stash show` compares against.
     - theirs_tree: the stash commit's OWN tree -- the "tracked" half (index
       + working tree merged at push time).
     - index_tree: parents[1]'s tree -- the index exactly as it stood at
       push time.
     - untracked_tree / has_untracked: parents[2]'s tree, present only for a
       `stash -u`/`-a` entry (3 parents). has_untracked is 0 and
       untracked_tree is left unset otherwise. */
typedef struct {
    unsigned char base_tree[SG_SHA1_RAW_LEN];
    unsigned char theirs_tree[SG_SHA1_RAW_LEN];
    unsigned char index_tree[SG_SHA1_RAW_LEN];
    int has_untracked;
    unsigned char untracked_tree[SG_SHA1_RAW_LEN];
} sg_stash_trees;

/* Resolves stash entry `index` (0 == stash@{0}, newest) into the four trees
   above. Returns -1 on a bad/out-of-range index, a corrupt stash commit, an
   unreadable tree/object, or a parent count that is not 2 or 3 (never a
   shape sg -- or real git -- produces for a stash). Returns 0 on success. */
int sg_stash_load_trees(const char *git_dir, size_t index, sg_stash_trees *out);

#endif
