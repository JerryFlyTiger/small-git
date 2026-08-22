#ifndef SG_PATHSPEC_H
#define SG_PATHSPEC_H

#include <stddef.h>

/* Pathspec: the "-- <path>..." half of a command line, turned into a
   predicate over repo-relative paths. `sg diff` filters its change list with
   one; nothing else does yet.

   Only the plain flavour is implemented -- literal paths, leading-directory
   matches, and wildcards. Pathspec *magic* (":(icase)", ":!exclude", ":/",
   ":(glob)") is rejected outright rather than taken literally: a spec
   starting with ':' that silently matched nothing, or matched a file
   genuinely named ":!sub", would answer a question the user did not ask, and
   a diff that quietly omits paths is the worst possible failure here. */

typedef struct {
    char **specs; /* owned; repo-relative, '/' separated, already normalized */
    size_t count;
    size_t cap;
} sg_pathspec;

typedef enum {
    SG_PATHSPEC_ERR_NONE = 0,
    SG_PATHSPEC_ERR_EMPTY,   /* "" -- git rejects it, and so do we */
    SG_PATHSPEC_ERR_OUTSIDE, /* resolves outside the worktree, or out of memory */
    SG_PATHSPEC_ERR_MAGIC    /* leading ':' -- see above */
} sg_pathspec_error;

/* Resolves one command-line argument against the current working directory
   (arg may be relative or absolute, exactly as typed) and appends it. The
   argument does NOT have to exist: `sg diff -- deleted.txt` is how you ask
   about a file that is gone, which is precisely when it cannot be stat'd.

   "." at the repository root resolves to the empty spec, which matches every
   path -- the same escape hatch git names in its own empty-pathspec error.

   A trailing '/' is preserved rather than normalized away, because git
   distinguishes them: measured against git 2.55.0, `git diff -- sub/` lists
   sub's contents while `git diff -- a.txt/` matches nothing at all, since a
   trailing slash asks for entries *under* the name and a regular file has
   none.

   Returns 0, or -1 with *err set (err may be NULL). SG_PATHSPEC_ERR_OUTSIDE
   doubles as the allocation-failure code: sg_resolve_repo_path_allow_root
   reports both as NULL and this layer cannot tell them apart. That is the
   safe conflation -- both refuse to run rather than proceeding with a spec
   that means something else. */
int sg_pathspec_add(sg_pathspec *ps, const char *repo_root, const char *arg,
                    sg_pathspec_error *err);

/* 1 if an argument is unmistakably a pathspec rather than a revision: it
   carries a wildcard character ('*', '?', '[', '\\') or the ':' that
   introduces pathspec magic.

   This is git's looks_like_pathspec(), and it is what stops `sg diff '*.zzz'`
   -- no such file -- from being rejected as an unknown revision. Measured
   against git 2.55.0: that command exits 0 with no output, while the
   wildcard-free `git diff nosuch` is a hard error. The character set lives
   here, next to the matcher that gives those same characters their meaning,
   so the two can never drift apart. */
int sg_pathspec_looks_like_spec(const char *arg);

/* 1 if path (repo-relative) is covered. An empty pathspec -- no arguments at
   all -- matches everything, which is what makes a bare `sg diff` fall out of
   the same code path as a filtered one.

   Three rules, in this order, measured against git 2.55.0:
     1. exact match;
     2. leading directory -- "sub" covers "sub/deep/c.txt";
     3. wildmatch, but ONLY for a spec containing '*', '?', '[' or '\\'.
   Rules 1 and 2 are a plain byte compare that treats wildcard characters as
   themselves, which is how a file literally named "lit*st" is matched by the
   spec "lit*st".

   Rule 3 does NOT get rule 2's leading-directory extension, and that is not
   an oversight: `git diff -- 'o[tx]her'` reports nothing even though "other"
   is a directory with changes under it, and `su?`, `s*b` and `sub/dee?` are
   all empty for the same reason. What makes "sub*" work is not directory
   recursion but sg_wildmatch's '*' crossing '/' on its own. */
int sg_pathspec_matches(const sg_pathspec *ps, const char *path);

void sg_pathspec_free(sg_pathspec *ps);

#endif /* SG_PATHSPEC_H */
