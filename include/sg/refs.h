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

/* Resolves HEAD -> refs/heads/<branch> -> that branch's current commit id.
   Returns -1 if the branch has no commits yet (a brand new repo) -- callers
   should treat that as "no parent commit", not an error. */
int sg_ref_resolve_head(const char *git_dir, unsigned char id_out[SG_SHA1_RAW_LEN]);

/* The branch name HEAD currently points to (e.g. "master"), malloc'd. Phase 2
   never produces a detached HEAD, so this always succeeds for a repo made by
   sg_repo_init / sg switch. Returns NULL on failure. */
char *sg_ref_current_branch(const char *git_dir);

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

#endif
