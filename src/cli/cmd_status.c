#include "sg/cli.h"

#include "sg/hash.h"
#include "sg/ignore.h"
#include "sg/index.h"
#include "sg/merge.h"
#include "sg/objstore.h"
#include "sg/object.h"
#include "sg/rebase.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/status.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *kind_label(sg_status_kind kind)
{
    switch (kind) {
    case SG_STATUS_NEW:
        return "new file:   ";
    case SG_STATUS_MODIFIED:
        return "modified:   ";
    case SG_STATUS_DELETED:
        return "deleted:    ";
    }
    return "";
}

static void print_section(const char *title, const char **hints, size_t hint_count,
                          const sg_status_list *list)
{
    size_t i;

    if (list->count == 0)
        return;
    printf("%s\n", title);
    for (i = 0; i < hint_count; i++)
        printf("  (%s)\n", hints[i]);
    for (i = 0; i < list->count; i++)
        printf("\t%s%s\n", kind_label(list->entries[i].kind), list->entries[i].path);
    printf("\n");
}

static int str_cmp(const void *a, const void *b)
{
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;

    return strcmp(sa, sb);
}

/* A path with an unresolved conflict has no stage-0 entry (only 1/2/3), so
   plain sg_index_find would wrongly call it untracked. */
static int path_tracked_any_stage(const sg_index *idx, const char *path)
{
    unsigned int stage;

    for (stage = 0; stage <= 3; stage++) {
        if (sg_index_find_stage(idx, path, stage) >= 0)
            return 1;
    }
    return 0;
}

/* Prints every distinct path carrying a stage 1/2/3 entry (idx is sorted by
   (path, stage), so duplicates for a path are contiguous). Returns the
   number of distinct unmerged paths found. */
static size_t print_unmerged(const sg_index *idx, int rebase_in_progress)
{
    size_t i;
    size_t count = 0;

    for (i = 0; i < idx->count; i++) {
        if (idx->entries[i].stage == 0)
            continue;
        if (i > 0 && strcmp(idx->entries[i].path, idx->entries[i - 1].path) == 0)
            continue;
        count++;
    }
    if (count == 0)
        return 0;

    printf("Unmerged paths:\n");
    printf("  (use \"sg add <file>...\" to mark resolution)\n");
    if (rebase_in_progress)
        printf("  (use \"sg rebase --abort\" to check out the original branch)\n");
    else
        printf("  (use \"sg merge --abort\" to abort the merge)\n");
    for (i = 0; i < idx->count; i++) {
        if (idx->entries[i].stage == 0)
            continue;
        if (i > 0 && strcmp(idx->entries[i].path, idx->entries[i - 1].path) == 0)
            continue;
        printf("\tboth modified:   %s\n", idx->entries[i].path);
    }
    printf("\n");
    return count;
}

/* Walks the worktree collecting untracked files, filtered through the
   .gitignore engine (ig): ignored directories are pruned outright -- nothing
   under an ignored directory can be re-included -- and ignored files are
   skipped. Tracked paths are checked first, since tracked files are never
   ignore-filtered. Returns 0 on success, -1 on allocation failure; callers
   must treat that as an error, never as "no untracked files" (silently
   swallowing an alloc failure here would let sg status claim a clean tree it
   never actually examined). */
/* Joins base and rel as "base/rel", returning -1 rather than a truncated
   result if it does not fit -- see the identically-named helper in
   cmd_add.c for why truncation must never be acted on. */
static int path_join(char *out, size_t out_size, const char *base, const char *rel)
{
    int n;

    if (rel == NULL || rel[0] == '\0')
        n = snprintf(out, out_size, "%s", base);
    else if (base == NULL || base[0] == '\0')
        n = snprintf(out, out_size, "%s", rel);
    else
        n = snprintf(out, out_size, "%s/%s", base, rel);

    if (n < 0 || (size_t)n >= out_size)
        return -1;
    return 0;
}

static int collect_untracked(const char *repo_root, const char *reldir, const sg_index *idx,
                             sg_ignore *ig, char ***out, size_t *count, size_t *cap)
{
    char absdir[4096];
    DIR *d;
    struct dirent *ent;

    if (path_join(absdir, sizeof(absdir), repo_root, reldir) != 0) {
        fprintf(stderr, "sg: warning: 路徑過長,略過目錄 '%s'(未追蹤清單可能不完整)\n", reldir);
        return 0;
    }

    d = opendir(absdir);
    if (d == NULL) {
        /* Not fatal -- status still reports everything it did manage to see
           -- but never silent: an unreadable directory means the untracked
           list below is incomplete, and a status that under-reports without
           saying so is how uncommitted work gets lost. */
        fprintf(stderr, "sg: warning: 無法讀取目錄 '%s': %s（未追蹤清單可能不完整）\n",
                reldir[0] != '\0' ? reldir : ".", strerror(errno));
        return 0;
    }

    while ((ent = readdir(d)) != NULL) {
        char relpath[4096];
        char abspath[4096];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        /* skip any git dir at ANY depth, not just the top level: there is no
           submodule support, and an embedded repo's metadata must never be
           walked or reported */
        if (strcmp(ent->d_name, ".git") == 0)
            continue;

        /* Never act on a truncated path: it usually still names a real
           directory further up, so lstat would succeed against the wrong
           entry and this would report a path that is not the one on disk. */
        if (path_join(relpath, sizeof(relpath), reldir, ent->d_name) != 0 ||
           path_join(abspath, sizeof(abspath), repo_root, relpath) != 0) {
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
            if (sg_ignore_is_ignored(ig, relpath, 1))
                continue; /* prune: nothing below can be re-included */
            if (sg_ignore_push_dir(ig, relpath) != 0) {
                closedir(d);
                return -1;
            }
            if (collect_untracked(repo_root, relpath, idx, ig, out, count, cap) != 0) {
                sg_ignore_pop_dir(ig);
                closedir(d);
                return -1;
            }
            sg_ignore_pop_dir(ig);
        } else if (S_ISREG(st.st_mode)) {
            if (path_tracked_any_stage(idx, relpath))
                continue;
            if (sg_ignore_is_ignored(ig, relpath, 0))
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

int sg_cmd_status(int argc, char **argv)
{
    char *git_dir;
    char *repo_root;
    char *branch;
    sg_index idx;
    sg_ignore *ig = NULL;
    unsigned char head_commit_id[SG_SHA1_RAW_LEN];
    int has_head;
    sg_flat_list head_flat;
    sg_status_list staged;
    sg_status_list unstaged;
    char **untracked = NULL;
    size_t untracked_count = 0;
    size_t untracked_cap = 0;
    size_t unmerged_count;
    size_t i;
    static const char *staged_hints[] = {
        "use \"sg restore --staged <file>...\" to unstage",
    };
    static const char *unstaged_hints[] = {
        "use \"sg add <file>...\" to update what will be committed",
        "use \"sg restore <file>...\" to discard changes in working directory",
    };

    (void)argv;
    if (argc != 1) {
        fprintf(stderr, "usage: sg status\n");
        return 1;
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;
    repo_root = sg_repo_root(git_dir);
    if (repo_root == NULL) {
        fprintf(stderr, "sg: failed to determine repository root\n");
        free(git_dir);
        return 1;
    }
    if (sg_index_read(git_dir, &idx) != 0) {
        fprintf(stderr, "sg: failed to read index (corrupt?)\n");
        free(git_dir);
        free(repo_root);
        return 1;
    }
    /* Load the ignore rules up front: a failure here must fail the whole
       command -- silently showing everything (or nothing) on an alloc
       failure is a bug class this codebase has already been bitten by. */
    if (sg_ignore_open(&ig, git_dir, repo_root) != 0) {
        fprintf(stderr, "sg: 記憶體不足，無法載入 .gitignore 規則\n");
        sg_index_free(&idx);
        free(git_dir);
        free(repo_root);
        return 1;
    }

    branch = sg_ref_current_branch(git_dir);
    if (branch != NULL) {
        printf("On branch %s\n", branch);
    } else {
        char detached[4160]; /* fits any ref path sg can build (SG_PATH_MAX) plus the wording */

        /* Only a real detached HEAD gets git's detached wording; a HEAD that
           is neither a branch symref nor a raw id is corrupt, and claiming
           it is detached would describe a broken repository as a normal
           state. Both of the remaining cases land on git's own fallback
           line, which it also uses for a detached HEAD whose reflog records
           no checkout (measured, 2.55.0). */
        if (sg_ref_head_is_detached(git_dir) == 1 &&
           sg_ref_detach_description(git_dir, detached, sizeof(detached)) == 0)
            printf("%s\n", detached);
        else
            printf("Not currently on any branch.\n");
    }
    free(branch);

    {
        sg_rebase_state rstate;

        if (sg_rebase_state_exists(git_dir) && sg_rebase_state_read(git_dir, &rstate) == 0) {
            char onto_hex[SG_SHA1_HEX_LEN + 1];
            char onto_short[8];
            size_t remaining = rstate.todo_count + (rstate.has_current ? 1 : 0);

            sg_sha1_to_hex(rstate.onto, onto_hex);
            memcpy(onto_short, onto_hex, 7);
            onto_short[7] = '\0';
            if (rstate.orig_branch != NULL)
                printf("You are currently rebasing branch '%s' onto %s.\n", rstate.orig_branch,
                      onto_short);
            else
                printf("You are currently rebasing.\n");
            printf("（還剩 %zu 個 commit 待處理）\n", remaining);
            if (rstate.has_current) {
                printf("  (fix conflicts and run \"sg rebase --continue\")\n"
                      "  (use \"sg rebase --skip\" to skip this patch)\n"
                      "  (use \"sg rebase --abort\" to check out the original branch)\n");
            }
            printf("\n");
            sg_rebase_state_free(&rstate);
        }
    }

    /* Existence, not parseability: real git reports a corrupt MERGE_HEAD as
       an ongoing merge in `status` just like a well-formed one (measured,
       2.55.0), and status must agree with the gates in switch/commit about
       whether a merge is in flight. */
    if (sg_merge_head_exists(git_dir)) {
        if (sg_index_has_unmerged(&idx))
            printf("You have unmerged paths.\n");
        else
            printf("All conflicts fixed but you are still merging.\n");
    }

    has_head = (sg_ref_resolve_head(git_dir, head_commit_id) == 0);
    memset(&head_flat, 0, sizeof(head_flat));
    if (has_head) {
        sg_obj_type type;
        unsigned char *content = NULL;
        size_t content_len;
        sg_commit commit;

        if (sg_object_read(git_dir, head_commit_id, &type, &content, &content_len) == 0 &&
           type == SG_OBJ_COMMIT) {
            if (sg_commit_parse(content, content_len, &commit) == 0) {
                sg_tree_flatten(git_dir, commit.tree, &head_flat);
                sg_commit_free(&commit);
            }
            free(content);
        }
    } else {
        printf("\nNo commits yet\n");
    }

    if (sg_status_diff_staged(&head_flat, &idx, &staged) != 0)
        fprintf(stderr, "sg: warning: out of memory computing staged changes\n");
    sg_flat_list_free(&head_flat);

    if (sg_status_diff_unstaged(git_dir, repo_root, &idx, &unstaged) != 0)
        fprintf(stderr,
               "sg: warning: failed to compute unstaged changes (out of memory, or a chunked "
               "file's data is missing/corrupt -- see sg restore for details)\n");

    if (collect_untracked(repo_root, "", &idx, ig, &untracked, &untracked_count,
                          &untracked_cap) != 0) {
        fprintf(stderr, "sg: 記憶體不足，無法掃描未追蹤的檔案\n");
        for (i = 0; i < untracked_count; i++)
            free(untracked[i]);
        free(untracked);
        sg_status_list_free(&staged);
        sg_status_list_free(&unstaged);
        sg_ignore_free(ig);
        sg_index_free(&idx);
        free(repo_root);
        free(git_dir);
        return 1;
    }
    if (untracked_count > 0)
        qsort(untracked, untracked_count, sizeof(*untracked), str_cmp);

    unmerged_count = print_unmerged(&idx, sg_rebase_state_exists(git_dir));

    print_section("Changes to be committed:", staged_hints, 1, &staged);
    print_section("Changes not staged for commit:", unstaged_hints, 2, &unstaged);

    if (untracked_count > 0) {
        printf("Untracked files:\n");
        printf("  (use \"sg add <file>...\" to include in what will be committed)\n");
        for (i = 0; i < untracked_count; i++)
            printf("\t%s\n", untracked[i]);
        printf("\n");
    }

    if (staged.count == 0 && unstaged.count == 0 && untracked_count == 0 && unmerged_count == 0)
        printf("nothing to commit, working tree clean\n");

    for (i = 0; i < untracked_count; i++)
        free(untracked[i]);
    free(untracked);
    sg_status_list_free(&staged);
    sg_status_list_free(&unstaged);
    sg_ignore_free(ig);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
    return 0;
}
