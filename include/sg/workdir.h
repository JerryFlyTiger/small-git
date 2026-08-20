#ifndef SG_WORKDIR_H
#define SG_WORKDIR_H

#include <stddef.h>

#include "sg/hash.h"

#define SG_PATH_MAX 4096

/* The repo root is git_dir's parent directory (git_dir is ".../.git"). Result
   is malloc'd, caller frees. */
char *sg_repo_root(const char *git_dir);

/* Resolves a CLI-supplied path argument (relative to the current working
   directory, or absolute) into a repo-root-relative, '/'-separated path.
   Result is malloc'd. Returns NULL if the path falls outside repo_root or on
   allocation failure. */
char *sg_resolve_repo_path(const char *repo_root, const char *arg);

/* Like sg_resolve_repo_path, but an argument naming the repository root
   itself (e.g. "." run from the root) resolves to "" instead of being
   rejected, so commands that accept a directory argument can take the whole
   worktree. Result is malloc'd. Returns NULL if the path falls outside
   repo_root or on allocation failure. */
char *sg_resolve_repo_path_allow_root(const char *repo_root, const char *arg);

/* mkdir -p for every directory component leading up to (but not including)
   the file named by path. */
int sg_mkdir_parents(const char *path);

/* Slurps an entire file into a malloc'd buffer. Returns 0 on success, -1 if
   the file can't be opened or read. */
int sg_read_file(const char *path, unsigned char **out, size_t *out_len);

/* Writes data to path, creating any missing parent directories first, and
   chmod's the result to mode. Returns 0 on success, -1 on I/O failure. */
int sg_write_file_mkdirs(const char *path, const unsigned char *data, size_t len, int mode);

/* Non-zero if path exists and is a symlink (checked with lstat, so it isn't
   followed). */
int sg_is_symlink(const char *path);

/* Joins base and rel as "base/rel" into out, or copies whichever side is
   non-empty when the other is NULL/"". Returns 0, or -1 if the result does
   not fit: a truncated path usually still names a real entry higher up the
   tree, so every caller must treat -1 as "this path does not exist to me"
   and never act on the buffer. Replaces the per-file path_join copies in
   status.c/cmd_add.c. prune_empty_untracked_dirs in safety/stash.c keeps its
   own inline bounds check on purpose: it skips silently where this helper's
   callers report, an empty directory left behind being invisible to sg and
   to real git alike. */
int sg_path_join(char *out, size_t out_size, const char *base, const char *rel);

/* SHA-1 object id of path's content as a blob, without writing any object. */
int sg_hash_file_blob(const char *path, unsigned char sha1_out[SG_SHA1_RAW_LEN]);

/* After a tracked file has been removed, removes each ancestor directory
   that is now empty, up to but never including repo_root -- rmdir only, so
   a directory still holding anything at all (tracked or not) is left alone.
   Best effort: every failure is ignored, because a leftover empty directory
   is invisible to sg status and to real git alike.

   Deliberately NOT ignore-aware, unlike safety/stash.c's
   prune_empty_untracked_dirs: real git (2.55.0, verified) removes a
   now-empty directory here even if that directory is itself covered by
   .gitignore, whereas the untracked-file sweep leaves an empty-but-ignored
   directory (e.g. "build/") alone. The two functions' rules are opposite by
   design -- do not "unify" them. */
void sg_prune_empty_parents(const char *repo_root, const char *relpath);

#endif
