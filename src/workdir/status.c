#include "sg/status.h"

#include "sg/chunk.h"
#include "sg/hash.h"
#include "sg/ignore.h"
#include "sg/quote.h"
#include "sg/workdir.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int status_list_add(sg_status_list *list, const char *path, sg_status_kind kind)
{
    if (list->count == list->cap) {
        size_t new_cap = list->cap == 0 ? 8 : list->cap * 2;
        sg_status_entry *grown = realloc(list->entries, new_cap * sizeof(*grown));

        if (grown == NULL)
            return -1;
        list->entries = grown;
        list->cap = new_cap;
    }
    list->entries[list->count].path = strdup(path);
    if (list->entries[list->count].path == NULL)
        return -1;
    list->entries[list->count].kind = kind;
    list->count++;
    return 0;
}

void sg_status_list_free(sg_status_list *list)
{
    size_t i;

    for (i = 0; i < list->count; i++)
        free(list->entries[i].path);
    free(list->entries);
    list->entries = NULL;
    list->count = 0;
    list->cap = 0;
}

/* True if idx has any stage 1/2/3 entry at path -- i.e. path is an
   unresolved conflict, already surfaced by `sg status`'s "Unmerged paths"
   section rather than here. */
static int path_has_unmerged_stage(const sg_index *idx, const char *path)
{
    return sg_index_find_stage(idx, path, 1) >= 0 || sg_index_find_stage(idx, path, 2) >= 0 ||
        sg_index_find_stage(idx, path, 3) >= 0;
}

/* Both loops below only ever look at stage-0 entries: a path with an
   unresolved conflict has no stage-0 entry (only 1/2/3), so it is simply
   absent from the staged/unstaged comparison here -- `sg status`'s
   "Unmerged paths" section is what reports those separately, and treating
   stage 1/2/3 rows as ordinary entries here would both misreport them and
   break this loop's assumption of at most one idx entry per path. A HEAD
   path with an unresolved conflict (no stage-0 counterpart) must likewise
   not be reported as "deleted" here -- path_has_unmerged_stage catches that,
   since otherwise it would look exactly like index really did drop it. */
int sg_status_diff_staged(const sg_flat_list *head_flat, const sg_index *idx, sg_status_list *out)
{
    size_t hi = 0;
    size_t ii = 0;

    memset(out, 0, sizeof(*out));

    while (hi < head_flat->count || ii < idx->count) {
        int cmp;

        if (ii < idx->count && idx->entries[ii].stage != 0) {
            ii++;
            continue;
        }

        if (hi >= head_flat->count)
            cmp = 1;
        else if (ii >= idx->count)
            cmp = -1;
        else
            cmp = strcmp(head_flat->entries[hi].path, idx->entries[ii].path);

        if (cmp == 0) {
            if (memcmp(head_flat->entries[hi].sha1, idx->entries[ii].sha1, SG_SHA1_RAW_LEN) != 0 ||
               head_flat->entries[hi].mode != idx->entries[ii].mode) {
                if (status_list_add(out, idx->entries[ii].path, SG_STATUS_MODIFIED) != 0)
                    return -1;
            }
            hi++;
            ii++;
        } else if (cmp < 0) {
            if (!path_has_unmerged_stage(idx, head_flat->entries[hi].path)) {
                if (status_list_add(out, head_flat->entries[hi].path, SG_STATUS_DELETED) != 0)
                    return -1;
            }
            hi++;
        } else {
            if (status_list_add(out, idx->entries[ii].path, SG_STATUS_NEW) != 0)
                return -1;
            ii++;
        }
    }
    return 0;
}

int sg_status_diff_unstaged(const char *git_dir, const char *repo_root, const sg_index *idx,
                            sg_status_list *out)
{
    size_t i;

    memset(out, 0, sizeof(*out));

    for (i = 0; i < idx->count; i++) {
        char abspath[SG_PATH_MAX];
        unsigned char wd_sha1[SG_SHA1_RAW_LEN];
        unsigned char effective_sha1[SG_SHA1_RAW_LEN];
        struct stat st;

        if (idx->entries[i].stage != 0)
            continue;

        /* A truncated path must not be silently skipped: that would drop a
           path from the unstaged-diff list, which `sg status` reports as
           "nothing changed" for a file that may well be dirty. */
        if (sg_path_join(abspath, sizeof(abspath), repo_root, idx->entries[i].path) != 0)
            return -1;
        if (stat(abspath, &st) != 0) {
            if (status_list_add(out, idx->entries[i].path, SG_STATUS_DELETED) != 0)
                return -1;
            continue;
        }
        if (sg_hash_file_blob(abspath, wd_sha1) != 0) {
            if (status_list_add(out, idx->entries[i].path, SG_STATUS_DELETED) != 0)
                return -1;
            continue;
        }
        /* idx's id may be a chunked-storage pointer's id rather than the
           content's own id -- normalize before comparing, or a chunked file
           that never actually changed would show up as modified forever. */
        if (sg_chunk_effective_id(git_dir, idx->entries[i].sha1, effective_sha1) != 0)
            return -1;
        if (memcmp(wd_sha1, effective_sha1, SG_SHA1_RAW_LEN) != 0) {
            if (status_list_add(out, idx->entries[i].path, SG_STATUS_MODIFIED) != 0)
                return -1;
        }
    }
    return 0;
}

/* True if idx has an entry for path at any stage (0, or an unresolved
   conflict's 1/2/3) -- a path with only 1/2/3 entries would otherwise be
   wrongly reported as untracked, and a staged-delete (no entry at any stage,
   file still on disk) must fall through and count as untracked. */
static int path_tracked_any_stage(const sg_index *idx, const char *path)
{
    unsigned int stage;

    for (stage = 0; stage <= 3; stage++) {
        if (sg_index_find_stage(idx, path, stage) >= 0)
            return 1;
    }
    return 0;
}

/* Walks the worktree collecting untracked files, filtered through the
   .gitignore engine (ig) unless include_ignored makes every path pass:
   ignored directories are pruned outright -- nothing under an ignored
   directory can be re-included -- and ignored files are skipped. Tracked
   paths are checked first, since tracked files are never ignore-filtered.
   Returns 0 on success, -1 on allocation failure; callers must treat that as
   an error, never as "no untracked files" (silently swallowing an alloc
   failure here would let a caller claim a clean tree it never actually
   examined). */
static int collect_untracked(const char *repo_root, const char *reldir, const sg_index *idx,
                             sg_ignore *ig, int include_ignored, char ***out, size_t *count,
                             size_t *cap)
{
    char absdir[SG_PATH_MAX];
    DIR *d;
    struct dirent *ent;

    if (sg_path_join(absdir, sizeof(absdir), repo_root, reldir) != 0) {
        fprintf(stderr, "sg: warning: 路徑過長,略過目錄 %s(未追蹤清單可能不完整)\n",
               sg_quote_path_delimited(reldir));
        return 0;
    }

    d = opendir(absdir);
    if (d == NULL) {
        /* Not fatal -- the caller still reports everything it did manage to
           see -- but never silent: an unreadable directory means the
           untracked list below is incomplete, and under-reporting without
           saying so is how uncommitted work gets lost. */
        fprintf(stderr, "sg: warning: 無法讀取目錄 %s: %s（未追蹤清單可能不完整）\n",
                sg_quote_path_delimited(reldir[0] != '\0' ? reldir : "."), strerror(errno));
        return 0;
    }

    while ((ent = readdir(d)) != NULL) {
        char relpath[SG_PATH_MAX];
        char abspath[SG_PATH_MAX];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        /* Skip the gitdir itself. Only an exact ".git" is skipped, NOT
           every name sg_path_component_is_safe would reject: real git
           (2.55.0, measured) lists ".git." as an untracked directory and
           refuses it only when adding, so folding this skip into the safety
           predicate makes sg UNDER-REPORT a path git reports -- a status
           listing quietly missing a file is the hardest kind of error to
           notice. The gitdir sg creates is always literally ".git", so an
           exact compare cannot miss it; anything else by that name is an
           ordinary directory whose files must be listed, and refused later
           at the point they would actually enter the index. */
        if (strcmp(ent->d_name, ".git") == 0)
            continue;

        /* Never act on a truncated path: it usually still names a real
           directory further up, so lstat would succeed against the wrong
           entry and this would report a path that is not the one on disk. */
        if (sg_path_join(relpath, sizeof(relpath), reldir, ent->d_name) != 0 ||
           sg_path_join(abspath, sizeof(abspath), repo_root, relpath) != 0) {
            /* Can't join reldir and ent->d_name into one path to quote as a
               unit -- that's exactly the join sg_path_join above just
               failed at, because the combined path is too long to fit a
               buffer. Quoting the two pieces separately and naming the
               relationship in words avoids reproducing that same overflow
               here, and avoids gluing two independently-quoted strings
               together with a literal "/" (which is exactly how a
               control-character name turns "dir"/"x\ty" into something that
               reads as two paths instead of one). */
            fprintf(stderr, "sg: warning: 路徑過長,略過目錄 %s 底下的項目 %s(未追蹤清單可能不完整)\n",
                    sg_quote_path_delimited(reldir[0] != '\0' ? reldir : "."),
                    sg_quote_path_delimited(ent->d_name));
            continue;
        }

        if (lstat(abspath, &st) != 0) {
            /* ENOENT = the entry vanished between readdir and now, benign.
               Anything else (ENAMETOOLONG on a tree deeper than the
               platform's PATH_MAX, EACCES, ELOOP) means we are dropping a
               real entry, so say so rather than under-report in silence. */
            if (errno != ENOENT)
                fprintf(stderr, "sg: warning: 無法讀取 %s: %s（未追蹤清單可能不完整）\n",
                        sg_quote_path_delimited(relpath), strerror(errno));
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (!include_ignored && sg_ignore_is_ignored(ig, relpath, 1))
                continue; /* prune: nothing below can be re-included */
            if (sg_ignore_push_dir(ig, relpath) != 0) {
                closedir(d);
                return -1;
            }
            if (collect_untracked(repo_root, relpath, idx, ig, include_ignored, out, count,
                                  cap) != 0) {
                sg_ignore_pop_dir(ig);
                closedir(d);
                return -1;
            }
            sg_ignore_pop_dir(ig);
        } else if (S_ISREG(st.st_mode)) {
            if (path_tracked_any_stage(idx, relpath))
                continue;
            if (!include_ignored && sg_ignore_is_ignored(ig, relpath, 0))
                continue;
            if (*count == *cap) {
                size_t new_cap = *cap == 0 ? 16 : *cap * 2;
                char **grown = realloc(*out, new_cap * sizeof(*grown));

                if (grown == NULL) {
                    closedir(d);
                    return -1;
                }
                *out = grown;
                *cap = new_cap;
            }
            (*out)[*count] = strdup(relpath);
            if ((*out)[*count] == NULL) {
                closedir(d);
                return -1;
            }
            (*count)++;
        }
    }
    closedir(d);
    return 0;
}

static int untracked_str_cmp(const void *a, const void *b)
{
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;

    return strcmp(sa, sb);
}

/* Grows *out and *cap as needed and appends a strdup'd copy of path. Shared by
   every collector below -- the folding walk and the ignored-files-within-a-
   folded-dir walk (collect_untracked above keeps its own inline copy of
   this same growth logic for the unfolded case). Returns 0 on success,
   -1 on allocation failure. */
static int untracked_append(char ***out, size_t *count, size_t *cap, const char *path)
{
    if (*count == *cap) {
        size_t new_cap = *cap == 0 ? 16 : *cap * 2;
        char **grown = realloc(*out, new_cap * sizeof(*grown));

        if (grown == NULL)
            return -1;
        *out = grown;
        *cap = new_cap;
    }
    (*out)[*count] = strdup(path);
    if ((*out)[*count] == NULL)
        return -1;
    (*count)++;
    return 0;
}

/* Like untracked_append, but appends reldir with a trailing '/' -- the
   folded-directory line. Silently drops (does not append, does not fail)
   a reldir long enough that reldir + "/" would not fit SG_PATH_MAX; this
   mirrors collect_untracked's own truncation convention elsewhere in this
   file except that folded-dir names are vanishingly unlikely to approach
   that limit, so a warning was judged not worth the noise here. Returns 0
   on success (including the silently-skipped case), -1 on allocation
   failure. */
static int untracked_append_folded(char ***out, size_t *count, size_t *cap, const char *reldir)
{
    char folded[SG_PATH_MAX];
    size_t len = strlen(reldir);

    if (len + 2 > sizeof(folded))
        return 0;
    memcpy(folded, reldir, len);
    folded[len] = '/';
    folded[len + 1] = '\0';
    return untracked_append(out, count, cap, folded);
}

/* True if idx has any entry (any stage) whose path lies strictly below
   reldir ("a/b" is below "a", "a" itself is not). reldir must not have a
   trailing '/'. Used only by the folding walk to decide whether a
   directory may be collapsed into a single "dir/" line: a directory that
   still holds a tracked path anywhere below it must be walked entry by
   entry, never folded, or a tracked (or conflicted) file would silently
   disappear from the listing. */
static int dir_has_tracked_descendant(const sg_index *idx, const char *reldir)
{
    size_t prefix_len = strlen(reldir);
    size_t i;

    for (i = 0; i < idx->count; i++) {
        const char *p = idx->entries[i].path;

        if (strncmp(p, reldir, prefix_len) == 0 && p[prefix_len] == '/')
            return 1;
    }
    return 0;
}

/* Recursively determines, for a directory already known to hold no tracked
   descendant, whether it contains (a) any file that is not gitignore-
   matched and (b) any file at all (ignored or not). sg_ignore_is_ignored
   already walks every ancestor prefix of relpath internally (see
   ignore.h/ignore.c: "if a directory is ignored, nothing under it can be
   re-included by any pattern" is enforced inside is_ignored itself), so
   this does not need to special-case a directory that is itself ignored --
   every file below one already reads back as ignored without this function
   pruning anything. Push/pop is still needed so a subdirectory's own
   .gitignore is loaded before its files are queried. Stops as soon as a
   non-ignored file is found (both flags are already settled at that
   point). Returns 0 on success, -1 on OOM (pushing an ignore frame can
   allocate). An unreadable directory, a truncated join, or a failed lstat
   still short-circuits the classification to "nothing found here" the same
   way collect_untracked's file walk does, but -- unlike an earlier version
   of this function -- it is never SILENT about it: under-reporting here can
   make a directory that actually holds untracked content fold to nothing at
   all (or fail to fold when it should), which is exactly the kind of
   under-reporting collect_untracked's own warnings exist to prevent, so the
   same stderr convention applies here too. */
static int dir_scan_flags(const char *repo_root, const char *reldir, sg_ignore *ig,
                          int *has_nonignored, int *has_any)
{
    char absdir[SG_PATH_MAX];
    DIR *d;
    struct dirent *ent;

    if (sg_path_join(absdir, sizeof(absdir), repo_root, reldir) != 0) {
        fprintf(stderr, "sg: warning: 路徑過長,略過目錄 %s(未追蹤清單可能不完整)\n",
               sg_quote_path_delimited(reldir));
        return 0;
    }
    d = opendir(absdir);
    if (d == NULL) {
        fprintf(stderr, "sg: warning: 無法讀取目錄 %s: %s（未追蹤清單可能不完整）\n",
                sg_quote_path_delimited(reldir[0] != '\0' ? reldir : "."), strerror(errno));
        return 0;
    }

    while ((ent = readdir(d)) != NULL) {
        char relpath[SG_PATH_MAX];
        char abspath[SG_PATH_MAX];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (strcmp(ent->d_name, ".git") == 0)
            continue;
        if (sg_path_join(relpath, sizeof(relpath), reldir, ent->d_name) != 0 ||
           sg_path_join(abspath, sizeof(abspath), repo_root, relpath) != 0) {
            fprintf(stderr,
                   "sg: warning: 路徑過長,略過目錄 %s 底下的項目 %s(未追蹤清單可能不完整)\n",
                    sg_quote_path_delimited(reldir[0] != '\0' ? reldir : "."),
                    sg_quote_path_delimited(ent->d_name));
            continue;
        }
        if (lstat(abspath, &st) != 0) {
            if (errno != ENOENT)
                fprintf(stderr, "sg: warning: 無法讀取 %s: %s（未追蹤清單可能不完整）\n",
                        sg_quote_path_delimited(relpath), strerror(errno));
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            int rc;

            if (sg_ignore_push_dir(ig, relpath) != 0) {
                closedir(d);
                return -1;
            }
            rc = dir_scan_flags(repo_root, relpath, ig, has_nonignored, has_any);
            sg_ignore_pop_dir(ig);
            if (rc != 0) {
                closedir(d);
                return -1;
            }
        } else if (S_ISREG(st.st_mode)) {
            *has_any = 1;
            if (!sg_ignore_is_ignored(ig, relpath, 0))
                *has_nonignored = 1;
        }
        if (*has_nonignored) {
            closedir(d);
            return 0;
        }
    }
    closedir(d);
    return 0;
}

/* Recursively appends every ignored path under reldir individually, except
   a subdirectory that is itself entirely ignored (sg_ignore_is_ignored with
   is_dir = 1) collapses into one "subdir/" line instead of being descended
   into -- the same fold-a-wholly-ignored-directory rule applied at the top
   level (sg_status_list_untracked's own FOLD_DIRS branch below), just one
   level down. Only meaningful under a reldir already known to have no
   tracked descendant (the caller verified that before deciding to fold
   reldir itself), and only ever called when include_ignored is set. Warns
   (never silent) on the same truncation/unreadable-directory cases
   collect_untracked and dir_scan_flags do, for the same reason: dropping a
   path here silently is exactly the under-reporting this file's warnings
   exist to prevent. Returns 0 on success, -1 on allocation failure. */
static int collect_ignored_within(const char *repo_root, const char *reldir, sg_ignore *ig,
                                  char ***out, size_t *count, size_t *cap)
{
    char absdir[SG_PATH_MAX];
    DIR *d;
    struct dirent *ent;

    if (sg_path_join(absdir, sizeof(absdir), repo_root, reldir) != 0) {
        fprintf(stderr, "sg: warning: 路徑過長,略過目錄 %s(未追蹤清單可能不完整)\n",
               sg_quote_path_delimited(reldir));
        return 0;
    }
    d = opendir(absdir);
    if (d == NULL) {
        fprintf(stderr, "sg: warning: 無法讀取目錄 %s: %s（未追蹤清單可能不完整）\n",
                sg_quote_path_delimited(reldir[0] != '\0' ? reldir : "."), strerror(errno));
        return 0;
    }

    while ((ent = readdir(d)) != NULL) {
        char relpath[SG_PATH_MAX];
        char abspath[SG_PATH_MAX];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (strcmp(ent->d_name, ".git") == 0)
            continue;
        if (sg_path_join(relpath, sizeof(relpath), reldir, ent->d_name) != 0 ||
           sg_path_join(abspath, sizeof(abspath), repo_root, relpath) != 0) {
            fprintf(stderr,
                   "sg: warning: 路徑過長,略過目錄 %s 底下的項目 %s(未追蹤清單可能不完整)\n",
                    sg_quote_path_delimited(reldir[0] != '\0' ? reldir : "."),
                    sg_quote_path_delimited(ent->d_name));
            continue;
        }
        if (lstat(abspath, &st) != 0) {
            if (errno != ENOENT)
                fprintf(stderr, "sg: warning: 無法讀取 %s: %s（未追蹤清單可能不完整）\n",
                        sg_quote_path_delimited(relpath), strerror(errno));
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (sg_ignore_is_ignored(ig, relpath, 1)) {
                if (untracked_append_folded(out, count, cap, relpath) != 0) {
                    closedir(d);
                    return -1;
                }
                continue;
            }
            if (sg_ignore_push_dir(ig, relpath) != 0) {
                closedir(d);
                return -1;
            }
            {
                int rc = collect_ignored_within(repo_root, relpath, ig, out, count, cap);

                sg_ignore_pop_dir(ig);
                if (rc != 0) {
                    closedir(d);
                    return -1;
                }
            }
        } else if (S_ISREG(st.st_mode)) {
            if (sg_ignore_is_ignored(ig, relpath, 0)) {
                if (untracked_append(out, count, cap, relpath) != 0) {
                    closedir(d);
                    return -1;
                }
            }
        }
    }
    closedir(d);
    return 0;
}

/* FOLD_DIRS counterpart to collect_untracked. is_root suppresses folding at
   the top level (repo_root is never itself printed as a folded line): the
   very first call always walks children one by one, exactly like
   collect_untracked, but each untracked subdirectory it finds gets its own
   independent fold decision through recursion instead of being flattened
   into a file list here. Once a directory has no tracked descendant, its
   fold decision is: any non-ignored file anywhere below -> fold to "dir/"
   (plus, if include_ignored, list every ignored path below it
   individually via collect_ignored_within); no non-ignored file but at
   least one file (all ignored) -> fold to "dir/" only if include_ignored,
   otherwise omit entirely; no file at all -> omit entirely regardless.
   Returns 0 on success, -1 on allocation failure. */
static int collect_untracked_folded(const char *repo_root, const char *reldir, const sg_index *idx,
                                    sg_ignore *ig, int include_ignored, int is_root,
                                    char ***out, size_t *count, size_t *cap)
{
    if (!is_root && !dir_has_tracked_descendant(idx, reldir)) {
        int has_nonignored = 0;
        int has_any = 0;

        if (dir_scan_flags(repo_root, reldir, ig, &has_nonignored, &has_any) != 0)
            return -1;

        if (has_nonignored) {
            if (untracked_append_folded(out, count, cap, reldir) != 0)
                return -1;
            if (include_ignored)
                return collect_ignored_within(repo_root, reldir, ig, out, count, cap);
            return 0;
        }
        if (has_any && include_ignored)
            return untracked_append_folded(out, count, cap, reldir);
        return 0;
    }

    {
        char absdir[SG_PATH_MAX];
        DIR *d;
        struct dirent *ent;

        if (sg_path_join(absdir, sizeof(absdir), repo_root, reldir) != 0) {
            fprintf(stderr, "sg: warning: 路徑過長,略過目錄 %s(未追蹤清單可能不完整)\n",
                   sg_quote_path_delimited(reldir));
            return 0;
        }

        d = opendir(absdir);
        if (d == NULL) {
            fprintf(stderr, "sg: warning: 無法讀取目錄 %s: %s（未追蹤清單可能不完整）\n",
                    sg_quote_path_delimited(reldir[0] != '\0' ? reldir : "."), strerror(errno));
            return 0;
        }

        while ((ent = readdir(d)) != NULL) {
            char relpath[SG_PATH_MAX];
            char abspath[SG_PATH_MAX];
            struct stat st;

            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            if (strcmp(ent->d_name, ".git") == 0)
                continue;

            if (sg_path_join(relpath, sizeof(relpath), reldir, ent->d_name) != 0 ||
               sg_path_join(abspath, sizeof(abspath), repo_root, relpath) != 0) {
                fprintf(stderr,
                       "sg: warning: 路徑過長,略過目錄 %s 底下的項目 %s(未追蹤清單可能不完整)\n",
                        sg_quote_path_delimited(reldir[0] != '\0' ? reldir : "."),
                        sg_quote_path_delimited(ent->d_name));
                continue;
            }

            if (lstat(abspath, &st) != 0) {
                if (errno != ENOENT)
                    fprintf(stderr, "sg: warning: 無法讀取 %s: %s（未追蹤清單可能不完整）\n",
                            sg_quote_path_delimited(relpath), strerror(errno));
                continue;
            }

            if (S_ISDIR(st.st_mode)) {
                if (!include_ignored && sg_ignore_is_ignored(ig, relpath, 1))
                    continue; /* prune: nothing below can be re-included */
                if (sg_ignore_push_dir(ig, relpath) != 0) {
                    closedir(d);
                    return -1;
                }
                if (collect_untracked_folded(repo_root, relpath, idx, ig, include_ignored, 0, out,
                                             count, cap) != 0) {
                    sg_ignore_pop_dir(ig);
                    closedir(d);
                    return -1;
                }
                sg_ignore_pop_dir(ig);
            } else if (S_ISREG(st.st_mode)) {
                if (path_tracked_any_stage(idx, relpath))
                    continue;
                if (!include_ignored && sg_ignore_is_ignored(ig, relpath, 0))
                    continue;
                if (untracked_append(out, count, cap, relpath) != 0) {
                    closedir(d);
                    return -1;
                }
            }
        }
        closedir(d);
    }
    return 0;
}

int sg_status_list_untracked(const char *git_dir, const char *repo_root, const sg_index *idx,
                             int include_ignored, sg_status_untracked_fold fold, char ***out,
                             size_t *count)
{
    sg_ignore *ig = NULL;
    size_t cap = 0;
    int rc;

    *out = NULL;
    *count = 0;

    if (sg_ignore_open(&ig, git_dir, repo_root) != 0)
        return -1;

    if (fold == SG_STATUS_UNTRACKED_FOLD_DIRS)
        rc = collect_untracked_folded(repo_root, "", idx, ig, include_ignored, 1, out, count, &cap);
    else
        rc = collect_untracked(repo_root, "", idx, ig, include_ignored, out, count, &cap);
    sg_ignore_free(ig);
    if (rc != 0) {
        /* Whatever collect_untracked managed to accumulate before failing is
           not useful to any caller and would otherwise have to be freed by
           every one of them identically -- clean it up here instead so -1
           always means "nothing allocated", matching every other failure
           path in this file. */
        size_t i;

        for (i = 0; i < *count; i++)
            free((*out)[i]);
        free(*out);
        *out = NULL;
        *count = 0;
        return rc;
    }
    /* collect_untracked walks in readdir order, which is unspecified and not
       lexicographic -- callers (sg_tree_build_from_untracked in particular,
       whose sg_tree_build contract requires a path-sorted flat list) must be
       able to rely on this being sorted rather than each having to sort it
       themselves. */
    if (*count > 0)
        qsort(*out, *count, sizeof(**out), untracked_str_cmp);
    return rc;
}
