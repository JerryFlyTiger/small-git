#ifndef SG_REFS_H
#define SG_REFS_H

#include <stddef.h>

#include "sg/hash.h"

/* Branch names get concatenated straight into a filesystem path by the
   functions below; a name like "../../../tmp/evil" would otherwise write/
   read outside refs/heads. Exposed (not just internal to refs.c) so other
   validated on-disk state (e.g. sg-rebase's orig-branch) can reuse the same
   check. */
int sg_ref_branch_name_is_safe(const char *name);

/* The subset of git-check-ref-format rules that git enforces when CREATING a
   ref, measured against real git (leading '-', "a..b", "a b", "a.lock",
   "HEAD", "a/", "@{x}" all rejected there). Strictly stricter than
   sg_ref_branch_name_is_safe above, which only guards against path
   traversal and is applied to every ref access (existing refs, however they
   got that name, must still list/read/delete); this one is applied only at
   creation time (sg branch <name>, and later sg tag <name>), so a
   pre-existing ref that wouldn't pass this check today is still reachable.
   Distinct again from sg_ref_name_is_safe in transport.h, which validates a
   REMOTE-advertised full ref path (must start with "refs/") before it is
   used to build a filesystem path during fetch/push -- that one guards
   against a hostile server, this one against a local user typo. Returns
   non-zero if name would be accepted as a new branch/tag name. */
int sg_ref_name_valid_for_create(const char *name);

/* Resolves HEAD to a commit id, through refs/heads/<branch> when HEAD is
   symbolic and directly when it is detached (a raw object id).
   Returns -1 only when there is no commit to resolve -- an unborn HEAD (a
   brand new repo), or a HEAD that is neither form -- which callers should
   treat as "no parent commit", not an error. Since Phase 18 a detached HEAD
   resolves successfully; do NOT reintroduce code that reads -1 here as
   "not on a branch", because those two states are no longer the same. */
int sg_ref_resolve_head(const char *git_dir, unsigned char id_out[SG_SHA1_RAW_LEN]);

/* The branch name HEAD currently points to (e.g. "master"), malloc'd.
   Returns NULL when HEAD is detached, unreadable, or points outside
   refs/heads/ -- use sg_ref_head_is_detached to tell those apart rather
   than assuming NULL means detached. */
char *sg_ref_current_branch(const char *git_dir);

/* 1 if .git/HEAD holds a raw object id (detached), 0 if it is the usual
   "ref: refs/heads/<b>" indirection, -1 if HEAD is missing, unreadable, or
   holds something that is neither. The corrupt case is deliberately kept
   distinct from the detached one: a caller that acts on "detached" may
   overwrite HEAD with a raw id, which would launder a corrupt HEAD into a
   valid-looking state instead of surfacing it. */
int sg_ref_head_is_detached(const char *git_dir);

/* Fills buf with real git's one-line description of a detached HEAD:
   "HEAD detached at <label>" while HEAD still sits on the commit it was
   detached at, "HEAD detached from <label>" once it has moved on. <label> is
   the detach POINT, never HEAD's current position.

   The detach point is recovered the way git does it -- the newest
   "checkout: moving from <x> to <y>" line in logs/HEAD -- so <label> is the
   token the user originally named, kept only while it still resolves to that
   same commit. It falls back to the abbreviated object id otherwise: a tag
   since moved, an expression like "HEAD~1" that was never a ref, the literal
   "HEAD" (which names no fixed commit, and which real git does not use as a
   label either), or a name too long for buf. refs/tags/ and refs/remotes/
   are stripped from it; refs/heads/ deliberately is not.

   Returns -1 only when logs/HEAD records no checkout at all, which is NOT an
   error: it means git's own fallback wording applies, and the two callers
   differ there -- `status` says "Not currently on any branch." and `branch`
   says "(no branch)" -- so the caller supplies it. A buffer too small for
   even the abbreviated form also returns -1, but any buf of ~40 bytes or
   more is past that.
   Says nothing about whether HEAD is detached; ask sg_ref_head_is_detached
   first. */
int sg_ref_detach_description(const char *git_dir, char *buf, size_t buf_size);

int sg_ref_update_branch(const char *git_dir, const char *branch,
                         const unsigned char id[SG_SHA1_RAW_LEN]);

/* Returns -1 if the branch does not exist (no commits on it yet). */
int sg_ref_read_branch(const char *git_dir, const char *branch, unsigned char id_out[SG_SHA1_RAW_LEN]);

int sg_ref_branch_exists(const char *git_dir, const char *branch);

/* Like sg_ref_update_branch, but for an arbitrary ref path under git_dir
   (e.g. "refs/sg/chunks"), not just "refs/heads/<name>" -- used for internal
   refs that don't fit the branch namespace. Creates any missing parent
   directories. Applies the same path-safety check as
   sg_ref_branch_name_is_safe (no leading '/', no ".."), applied to the whole
   ref_path this time, so a caller-supplied path still can't escape
   .git/refs (or .git itself). Returns 0 on success, -1 on an unsafe path or
   I/O failure. */
int sg_ref_write_path(const char *git_dir, const char *ref_path,
                      const unsigned char id[SG_SHA1_RAW_LEN]);

/* Reads a ref written by sg_ref_write_path. Returns -1 if it doesn't exist,
   is unsafe, or is malformed (same "doesn't exist yet" vs "corrupt" caveat
   as sg_ref_read_branch -- callers that treat a missing ref as "nothing
   there yet" rather than an error should keep doing so here too). Falls
   back to git_dir/packed-refs if there's no loose file at ref_path -- `git
   gc` (via `git pack-refs`) can move any ref, including ones this project
   writes itself (e.g. SG_CHUNK_KEEPALIVE_REF), out of its loose per-ref file
   and into packed-refs instead, and that must not look like the ref no
   longer exists. */
int sg_ref_read_path(const char *git_dir, const char *ref_path,
                     unsigned char id_out[SG_SHA1_RAW_LEN]);

/* Enumerates every branch, merging the loose refs/heads/ files with the
   packed-refs entries (loose wins on duplicates -- the same precedence
   sg_ref_read_branch applies when reading a single branch, and packed-only
   branches left behind by `git pack-refs` must still be listed). Fills
   *names_out with a malloc'd array of malloc'd names (no "refs/heads/"
   prefix, slash-containing names kept whole), sorted byte-wise, and
   *count_out with its length. count 0 leaves *names_out NULL. Caller frees
   each name and then the array. Returns 0 on success, -1 on error (outputs
   are then NULL/0). */
int sg_ref_list_branches(const char *git_dir, char ***names_out, size_t *count_out);

/* Deletes refs/heads/<branch> from BOTH stores: unlinks the loose file and
   atomically rewrites packed-refs without that branch's line (and its peel
   line, if any). Removing only the loose file is not a deletion -- a stale
   packed entry would silently resurrect the branch at an old commit.
   Returns 0 on success, 1 if the branch did not exist, -1 on an unsafe name
   or I/O error. */
int sg_ref_delete_branch(const char *git_dir, const char *branch);

/* Generalizations of sg_ref_list_branches / sg_ref_delete_branch to an
   arbitrary ref namespace prefix (e.g. "refs/heads/" or "refs/tags/"),
   for callers (sg tag, in a later phase) that need the same loose+packed
   merge/purge logic but under a different subtree. prefix must end in '/'.
   Same semantics, output conventions, and loose-wins-over-packed precedence
   as the branch-specific versions, which are now thin wrappers around
   these with prefix "refs/heads/". */
int sg_ref_list_under(const char *git_dir, const char *prefix, char ***names_out, size_t *count_out);
int sg_ref_delete_under(const char *git_dir, const char *prefix, const char *name);

/* The single convergence point for writing an arbitrary ref (branch,
   remote-tracking, refs/stash, ...), optionally recording a reflog entry.

   reflog_msg == NULL: behaves exactly like sg_ref_write_path -- no log file
   is touched at all. This is what every batch-A caller passes; the reflog
   machinery below is otherwise dead code until a later phase starts passing
   real messages.

   reflog_msg != NULL: ref_path must be "HEAD", or start with "refs/heads/"
   or "refs/remotes/", or be exactly "refs/stash" -- real git only logs
   these namespaces (measured; refs/tags/... and other internal refs are not
   logged). Any other ref_path fails with -1 and nothing is written at all
   (the ref file included). On an allowed path: the OLD value is read via
   sg_ref_read_path (missing/unreadable is treated as all-zeros, i.e. "ref
   didn't exist before"), then:
     - if old != new, one line is appended to logs/<ref_path>;
     - if ref_path is "refs/heads/<b>" and HEAD is currently the symbolic
       ref to that same branch, ANOTHER line -- old/new/message byte-for-
       byte identical to the one above -- is appended to logs/HEAD,
       regardless of whether old == new (real git's asymmetric rule: a
       branch's own log suppresses no-op updates, but logs/HEAD does not,
       measured against git 2.55.0).
   The ref file is written last; if that write fails, any reflog line(s)
   just appended are truncated back off (logs/HEAD first, then the branch
   log, undoing them in the reverse order they were added) and -1 is
   returned, leaving ref/reflog consistent with each other.

   Known limitation, not fixed here: sg_ref_read_path collapses "ref does
   not exist yet" and "ref file is corrupt" into the same -1, so a corrupt
   existing ref's reflog entry will show an all-zeros old_id as if the ref
   were being created fresh. Callers needing to tell those apart must not
   rely on this function alone. */
int sg_ref_update(const char *git_dir, const char *ref_path,
                  const unsigned char new_id[SG_SHA1_RAW_LEN], const char *reflog_msg);

/* Writes .git/HEAD as "ref: refs/heads/<branch>\n" -- the detach-free HEAD
   move used by sg switch/clone/init. reflog_msg == NULL: HEAD is written and
   nothing else happens. reflog_msg != NULL: BEFORE moving HEAD, the commit
   it currently resolves to is looked up (sg_ref_resolve_head) for old_id,
   and branch's tip (sg_ref_read_branch) is looked up for new_id -- either
   lookup failing (unborn HEAD, or `branch` has no commits yet, which is the
   normal state right after sg_repo_init) uses all-zeros instead of failing
   the whole call. One line -- old/new possibly equal -- is unconditionally
   appended to logs/HEAD (HEAD's log is never no-op-suppressed, same rule as
   sg_ref_update's asymmetric case). If writing HEAD itself then fails, that
   reflog line is truncated back off. Returns 0, -1 on failure (allocation,
   or the HEAD file write). */
int sg_ref_set_head(const char *git_dir, const char *branch, const char *reflog_msg);

/* Writes .git/HEAD as a raw 40-hex object id, i.e. detaches it. The mirror
   of sg_ref_set_head, with the same reflog contract: reflog_msg == NULL
   writes only HEAD; otherwise one line -- old/new possibly equal -- is
   appended to logs/HEAD first and truncated back off if the HEAD write then
   fails. old_id comes from sg_ref_resolve_head (all-zeros only for an
   unborn HEAD), so detaching FROM a branch correctly records the commit
   being left, which is what real git writes there (measured, git 2.55.0).

   Not expressible via sg_ref_update(git_dir, "HEAD", ...): that produces the
   same HEAD file, but reads old_id with sg_ref_read_path, which cannot parse
   a still-symbolic HEAD and so silently logs all-zeros. Returns 0, -1 on
   failure. */
int sg_ref_set_head_detached(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                             const char *reflog_msg);

#endif
