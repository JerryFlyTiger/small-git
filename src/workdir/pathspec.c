#include "sg/pathspec.h"

#include "sg/wildmatch.h"
#include "sg/workdir.h"

#include <stdlib.h>
#include <string.h>

/* git's no_wildcard(): the characters that make it take the wildmatch path
   instead of a byte compare. '\\' is in the set because it is an escape for
   the matcher -- the spec "lit\*st" has to reach sg_wildmatch to match the
   file "lit*st", which a byte compare against the backslash would miss. */
static int has_wildcard(const char *s)
{
    return strpbrk(s, "*?[\\") != NULL;
}

int sg_pathspec_looks_like_spec(const char *arg)
{
    return arg[0] == ':' || has_wildcard(arg);
}

static int reserve(sg_pathspec *ps)
{
    size_t new_cap;
    char **grown;

    if (ps->count < ps->cap)
        return 0;
    new_cap = ps->cap == 0 ? 4 : ps->cap * 2;
    grown = realloc(ps->specs, new_cap * sizeof(*grown));
    if (grown == NULL)
        return -1;
    ps->specs = grown;
    ps->cap = new_cap;
    return 0;
}

int sg_pathspec_add(sg_pathspec *ps, const char *repo_root, const char *arg,
                    sg_pathspec_error *err)
{
    char *rel;
    size_t arg_len = strlen(arg);
    int dir_only;

    if (err != NULL)
        *err = SG_PATHSPEC_ERR_NONE;

    if (arg_len == 0) {
        if (err != NULL)
            *err = SG_PATHSPEC_ERR_EMPTY;
        return -1;
    }
    if (arg[0] == ':') {
        if (err != NULL)
            *err = SG_PATHSPEC_ERR_MAGIC;
        return -1;
    }

    /* Resolution is purely lexical -- sg_resolve_repo_path_allow_root
       collapses "."/".." and rejects anything outside the worktree without
       touching the filesystem, which is what lets a wildcard, or a path that
       no longer exists, survive it unchanged. */
    dir_only = arg[arg_len - 1] == '/';
    rel = sg_resolve_repo_path_allow_root(repo_root, arg);
    if (rel == NULL) {
        if (err != NULL)
            *err = SG_PATHSPEC_ERR_OUTSIDE;
        return -1;
    }

    /* Re-attach the trailing slash the normalizer dropped. Skipped when the
       spec resolved to the root itself ("./"), where "/" would be a spec
       that can never match instead of the match-everything one. */
    if (dir_only && rel[0] != '\0') {
        size_t len = strlen(rel);
        char *grown = realloc(rel, len + 2);

        if (grown == NULL) {
            free(rel);
            if (err != NULL)
                *err = SG_PATHSPEC_ERR_OUTSIDE;
            return -1;
        }
        grown[len] = '/';
        grown[len + 1] = '\0';
        rel = grown;
    }

    if (reserve(ps) != 0) {
        free(rel);
        if (err != NULL)
            *err = SG_PATHSPEC_ERR_OUTSIDE;
        return -1;
    }
    ps->specs[ps->count++] = rel;
    return 0;
}

static int spec_matches(const char *spec, const char *path)
{
    size_t slen = strlen(spec);
    size_t plen = strlen(path);

    if (slen == 0)
        return 1; /* "." at the root: everything */

    if (slen <= plen && memcmp(spec, path, slen) == 0) {
        if (plen == slen)
            return 1;                 /* exact */
        if (spec[slen - 1] == '/')
            return 1;                 /* spec already named the directory */
        if (path[slen] == '/')
            return 1;                 /* leading directory */
    }

    if (has_wildcard(spec))
        return sg_wildmatch(spec, slen, path, plen);
    return 0;
}

int sg_pathspec_matches(const sg_pathspec *ps, const char *path)
{
    size_t i;

    if (ps == NULL || ps->count == 0)
        return 1;
    for (i = 0; i < ps->count; i++) {
        if (spec_matches(ps->specs[i], path))
            return 1;
    }
    return 0;
}

void sg_pathspec_free(sg_pathspec *ps)
{
    size_t i;

    if (ps == NULL)
        return;
    for (i = 0; i < ps->count; i++)
        free(ps->specs[i]);
    free(ps->specs);
    ps->specs = NULL;
    ps->count = 0;
    ps->cap = 0;
}
