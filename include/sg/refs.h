#ifndef SG_REFS_H
#define SG_REFS_H

#include "sg/hash.h"

/* Branch names get concatenated straight into a filesystem path by the
   functions below; a name like "../../../tmp/evil" would otherwise write/
   read outside refs/heads. Exposed (not just internal to refs.c) so other
   validated on-disk state (e.g. sg-rebase's orig-branch) can reuse the same
   check. */
int sg_ref_branch_name_is_safe(const char *name);

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

#endif
