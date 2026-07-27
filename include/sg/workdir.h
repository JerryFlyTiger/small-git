#ifndef SG_WORKDIR_H
#define SG_WORKDIR_H

#include <stddef.h>

#include "sg/hash.h"

/* The repo root is git_dir's parent directory (git_dir is ".../.git"). Result
   is malloc'd, caller frees. */
char *sg_repo_root(const char *git_dir);

/* Resolves a CLI-supplied path argument (relative to the current working
   directory, or absolute) into a repo-root-relative, '/'-separated path.
   Result is malloc'd. Returns NULL if the path falls outside repo_root or on
   allocation failure. */
char *sg_resolve_repo_path(const char *repo_root, const char *arg);

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

/* SHA-1 object id of path's content as a blob, without writing any object. */
int sg_hash_file_blob(const char *path, unsigned char sha1_out[SG_SHA1_RAW_LEN]);

#endif
