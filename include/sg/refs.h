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

#endif
