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
   relpath must stay under repo_root: an absolute path, or one with a ".."
   component, is refused outright rather than resolved, since these paths
   come from tree objects whose entry names sg does not validate.
   Best effort: every failure is ignored, because a leftover empty directory
   is invisible to sg status and to real git alike.

   Deliberately NOT ignore-aware, unlike safety/stash.c's
   prune_empty_untracked_dirs: real git (2.55.0, verified) removes a
   now-empty directory here even if that directory is itself covered by
   .gitignore, whereas the untracked-file sweep leaves an empty-but-ignored
   directory (e.g. "build/") alone. The two functions' rules are opposite by
   design -- do not "unify" them. */
void sg_prune_empty_parents(const char *repo_root, const char *relpath);

/* Whether a single path component -- a tree entry name, or one
   slash-separated piece of a repo-relative path -- may be turned into a
   working-tree path. Returns 1 if safe, 0 if not.

   Rejects: "", ".", "..", anything containing '/', and any case variant of
   ".git" including the trailing-'.'-or-space forms (".GIT", ".git.",
   ".git ") -- on a case-insensitive filesystem (the macOS default) ".GIT"
   names the very same directory, and real git refuses ".git." / ".git " on
   every platform, so accepting them would only ever produce a repository
   real git cannot check out.

   The case fold is ASCII-only and deliberately not strcasecmp: strcasecmp
   is locale-dependent and does not fold 'I' to 'i' under a Turkish locale,
   which would let ".GIT" through on exactly the filesystem this rule exists
   for.

   Accepts ".gitignore", ".gitmodules", "..a", "a.." -- the comparison is on
   the whole component, never a prefix or substring match. Control characters
   are accepted: real git accepts them in tree entries and defends at the
   display layer instead (measured; core.quotePath does NOT turn that off).

   NOT applied when sg parses a tree object: like git, sg can read and print
   a hostile tree (`sg cat-file -p`), it just refuses to turn one into
   files. */
int sg_path_component_is_safe(const char *name);

/* Whether an already-assembled repo-relative path may be turned into a
   working-tree path: non-empty, not absolute, no empty component ("a//b",
   "a/"), and every component passes sg_path_component_is_safe.
   For the two sources that arrive assembled rather than one component at a
   time -- .git/index entries and command-line arguments. */
int sg_relpath_is_safe(const char *relpath);

#endif
