#ifndef SG_IGNORE_H
#define SG_IGNORE_H

/* gitignore matching engine. Answers "given this repo-root-relative path, and
   whether it is a directory, is it ignored?" while the caller walks the tree
   pushing/popping per-directory .gitignore files.

   Sources and precedence (highest wins): per-directory .gitignore files with
   deeper directories beating shallower ones, then .git/info/exclude.
   core.excludesFile / ~/.gitconfig is not supported. Within one file the last
   matching pattern wins; a matching negation ("!") makes the path
   not-ignored. If a directory is ignored, nothing under it can be
   re-included by any pattern. */

typedef struct sg_ignore sg_ignore;

/* Creates a matcher, loading .git/info/exclude (from git_dir) and the
   worktree root's .gitignore. Missing or unreadable files are normal and
   treated as empty. Returns 0 on success, -1 on allocation failure. */
int sg_ignore_open(sg_ignore **out, const char *git_dir, const char *worktree_root);

/* Enters a directory: pushes its .gitignore (if present) onto the source
   stack. dir_relpath is repo-root-relative, '/'-separated, no leading "./"
   and no trailing '/'. Pushes must be nested (push "a" before "a/b") and
   balanced with sg_ignore_pop_dir. A missing or unreadable .gitignore still
   pushes an (empty) frame, so push/pop stay balanced either way. Returns 0
   on success, -1 on allocation failure (in which case nothing was pushed). */
int sg_ignore_push_dir(sg_ignore *ig, const char *dir_relpath);

/* Leaves the most recently pushed directory. Popping with no pushed
   directory left is a caller bug and is silently ignored. */
void sg_ignore_pop_dir(sg_ignore *ig);

/* 1 if relpath is ignored, 0 if not. relpath is repo-root-relative,
   '/'-separated, no leading "./" and no trailing '/'; is_dir says whether it
   names a directory. Ignore rules only make sense for untracked-file
   discovery: tracked files are never affected and callers must not consult
   this for them. */
int sg_ignore_is_ignored(const sg_ignore *ig, const char *relpath, int is_dir);

void sg_ignore_free(sg_ignore *ig);

#endif
