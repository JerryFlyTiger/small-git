#ifndef SG_REPO_H
#define SG_REPO_H

#include <stddef.h>

/* Creates a .git skeleton under dir (HEAD, objects/, refs/heads, refs/tags,
   config, description). dir may be NULL to mean the current directory.
   Mirrors `git init`'s directory layout closely enough that a real git binary
   recognizes the result as a valid repository. Returns 0 on success, -1 on
   failure (errno / message already reported to stderr by the caller's CLI
   layer is NOT done here -- this is a library-level function). */
int sg_repo_init(const char *dir);

/* Walks up from the current working directory looking for a ".git" directory,
   the same way git locates the repository root. Returns a malloc'd path to
   the .git directory, or NULL if none was found. Caller frees the result. */
char *sg_find_git_dir(void);

/* Same as sg_find_git_dir, but prints a consistent "not a git repository"
   message (with a suggestion to run `sg init`) to stderr when none is found,
   so callers don't each need their own copy of that message. Returns NULL on
   failure (message already printed); a malloc'd path (caller frees) on
   success, same as sg_find_git_dir. */
char *sg_require_git_dir(void);

/* Minimal "just enough" .git/config reader: finds `[remote "<remote>"]` and
   returns the url = value in that section, malloc'd (caller frees). Not a
   general gitconfig parser (no quoting/escaping/multi-line support) -- this
   is all `sg fetch`/`sg push` need. Returns NULL if the section or its url
   key is missing, or the file can't be read. */
char *sg_repo_read_remote_url(const char *git_dir, const char *remote);

/* Reads the `[sg]` section of git_dir/config for chunked-blob-storage
   settings (see sg/chunk.h): `chunking = true` and `chunkthreshold = <n>`.
   *enabled_out is 1 only when chunking is present and its value is exactly
   "true" (after trimming surrounding whitespace); any other case -- missing
   section, missing key, a different value, or the file not being readable --
   leaves it 0, since chunking defaults to off. *threshold_out is set to
   chunkthreshold's parsed value when present and a valid positive integer,
   and to SG_CHUNK_DEFAULT_THRESHOLD otherwise, regardless of *enabled_out --
   callers can always use it directly. Always returns 0: an absent [sg]
   section/config file is not an error, just the default (chunking off,
   default threshold). */
int sg_repo_read_chunk_config(const char *git_dir, int *enabled_out, size_t *threshold_out);

/* Records, in the `[sg]` section of git_dir/config, that this repository has
   used chunked-blob storage (see sg/chunk.h) at least once -- either by
   producing a chunk pointer itself (sg_chunk_store_blob) or by receiving one
   via `sg clone`/`sg fetch` merging in a remote's SG_CHUNK_KEEPALIVE_REF.
   This is deliberately local, per-repository state that a `git clone` of
   this repo does NOT carry forward (unlike SG_CHUNK_KEEPALIVE_REF, which
   *is* a ref and so does travel with a real clone/fetch) -- that asymmetry
   is exactly the point: it lets sg_chunk_read_blob's discriminator (see
   chunk.c's chunk_resolve) tell apart "this repo never used chunking, so an
   absent keep-alive ref is just the ordinary state of a plain `git clone`"
   from "this repo used chunking and its keep-alive ref specifically went
   missing afterward -- a hard failure, not business as usual". Idempotent:
   safe to call after the marker is already set (a cheap no-op, not a
   duplicate write) -- see sg_repo_chunking_was_used. Returns 0 on success
   (including the already-set case), -1 on I/O failure. */
int sg_repo_mark_chunking_used(const char *git_dir);

/* Reads whether sg_repo_mark_chunking_used has ever been called for this
   repository (i.e. whether git_dir/config's `[sg]` section has its marker
   key set to exactly "true", after trimming whitespace). Mirrors
   sg_repo_read_chunk_config's "absence is not an error, just false"
   convention: a missing section/key, a different value, or an unreadable
   config file all return 0, never an error. */
int sg_repo_chunking_was_used(const char *git_dir);

#endif
