#ifndef SG_REPO_H
#define SG_REPO_H

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

#endif
