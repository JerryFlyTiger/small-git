#include "sg/status.h"

#include "sg/chunk.h"
#include "sg/hash.h"
#include "sg/ignore.h"
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
        fprintf(stderr, "sg: warning: 路徑過長,略過目錄 '%s'(未追蹤清單可能不完整)\n", reldir);
        return 0;
    }

    d = opendir(absdir);
    if (d == NULL) {
        /* Not fatal -- the caller still reports everything it did manage to
           see -- but never silent: an unreadable directory means the
           untracked list below is incomplete, and under-reporting without
           saying so is how uncommitted work gets lost. */
        fprintf(stderr, "sg: warning: 無法讀取目錄 '%s': %s（未追蹤清單可能不完整）\n",
                reldir[0] != '\0' ? reldir : ".", strerror(errno));
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
            fprintf(stderr, "sg: warning: 路徑過長,略過 '%s/%s'(未追蹤清單可能不完整)\n",
                    reldir[0] != '\0' ? reldir : ".", ent->d_name);
            continue;
        }

        if (lstat(abspath, &st) != 0) {
            /* ENOENT = the entry vanished between readdir and now, benign.
               Anything else (ENAMETOOLONG on a tree deeper than the
               platform's PATH_MAX, EACCES, ELOOP) means we are dropping a
               real entry, so say so rather than under-report in silence. */
            if (errno != ENOENT)
                fprintf(stderr, "sg: warning: 無法讀取 '%s': %s（未追蹤清單可能不完整）\n",
                        relpath, strerror(errno));
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

int sg_status_list_untracked(const char *git_dir, const char *repo_root, const sg_index *idx,
                             int include_ignored, char ***out, size_t *count)
{
    sg_ignore *ig = NULL;
    size_t cap = 0;
    int rc;

    *out = NULL;
    *count = 0;

    if (sg_ignore_open(&ig, git_dir, repo_root) != 0)
        return -1;

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
