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

/* Creates a new stash from the current index and working tree and resets
   both to HEAD. message NULL selects the "WIP on <branch>: <short> <subj>"
   form; otherwise the "On <branch>: <message>" form is used. Writes the
   index commit (parent 2) unconditionally -- real git refuses to treat a
   commit with fewer than two parents as a stash at all (measured: a forged
   1-parent stash lists, but `show`/`apply`/`pop` die with "not a stash-like
   commit"), so it is not optional.

   Returns 0 when a stash was created, 1 when there was nothing to save
   (working tree and index both already equal HEAD -- nothing on disk was
   touched, and the caller prints "No local changes to save" and exits 0,
   matching git), -1 on failure. Refuses (-1) on an unborn HEAD or an
   unmerged index. Deliberately does NOT touch a paused rebase's sequencer
   state, and does NOT remove MERGE_HEAD -- both are the CLI layer's job (see
   cmd_stash.c). */
int sg_stash_push(const char *git_dir, const char *repo_root, const char *message,
                  unsigned char commit_id_out[SG_SHA1_RAW_LEN]);

/* Three-way merges stash entry `index` into the current HEAD (base = the
   stash's first parent's tree, ours = HEAD's tree, theirs = the stash's
   tree), writing the result into the working tree and the index. The caller
   must already have verified that the working tree is clean; that is what
   makes "ours" equal to HEAD's tree. Conflict markers are labelled
   "Updated upstream" / "Stashed changes" (measured -- git uses those literal
   strings here, not branch names).

   On a clean merge the index is left as real git leaves it without --index:
   equal to HEAD's tree, except that paths present in the merge result but
   absent from HEAD stay staged (measured: ` M tracked.txt` but
   `A  newfile.txt`). On conflict the merge-result index is written as-is
   (stages 1/2/3 for conflicted paths, stage 0 for cleanly merged ones --
   measured `UU c.txt` alongside `M  clean.txt`), and MERGE_HEAD is NOT
   written (git does not create one here).

   Returns 0 on a clean apply, 1 when there were conflicts (working tree and
   index are left in the conflicted state, the stash entry is untouched and
   the caller must not drop it), -1 on failure. Returns -1 if the entry has
   fewer than 2 or more than 2 parents -- a 3-parent entry is a `git stash
   -u` stash whose untracked half Phase 15 cannot restore, and applying only
   the tracked half would silently lose files. */
int sg_stash_apply(const char *git_dir, const char *repo_root, size_t index);

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

#endif
