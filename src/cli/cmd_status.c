#include "sg/cli.h"

#include "sg/hash.h"
#include "sg/index.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    char **lines;
    size_t count;
    size_t cap;
} line_list;

static void line_list_add(line_list *list, const char *fmt, const char *path)
{
    char buf[4200];

    if (list->count == list->cap) {
        size_t new_cap = list->cap == 0 ? 8 : list->cap * 2;
        char **grown = realloc(list->lines, new_cap * sizeof(*grown));

        if (grown == NULL)
            return;
        list->lines = grown;
        list->cap = new_cap;
    }
    snprintf(buf, sizeof(buf), fmt, path);
    list->lines[list->count++] = strdup(buf);
}

static void line_list_free(line_list *list)
{
    size_t i;

    for (i = 0; i < list->count; i++)
        free(list->lines[i]);
    free(list->lines);
    list->lines = NULL;
    list->count = 0;
    list->cap = 0;
}

static void print_section(const char *title, const char *hint, line_list *list)
{
    size_t i;

    if (list->count == 0)
        return;
    printf("%s\n", title);
    printf("  (%s)\n", hint);
    for (i = 0; i < list->count; i++)
        printf("\t%s\n", list->lines[i]);
    printf("\n");
}

static int str_cmp(const void *a, const void *b)
{
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;

    return strcmp(sa, sb);
}

static void collect_untracked(const char *repo_root, const char *reldir, const sg_index *idx,
                              char ***out, size_t *count, size_t *cap)
{
    char absdir[4096];
    DIR *d;
    struct dirent *ent;

    if (reldir[0] != '\0')
        snprintf(absdir, sizeof(absdir), "%s/%s", repo_root, reldir);
    else
        snprintf(absdir, sizeof(absdir), "%s", repo_root);

    d = opendir(absdir);
    if (d == NULL)
        return;

    while ((ent = readdir(d)) != NULL) {
        char relpath[4096];
        char abspath[4096];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (reldir[0] == '\0' && strcmp(ent->d_name, ".git") == 0)
            continue;

        if (reldir[0] != '\0')
            snprintf(relpath, sizeof(relpath), "%s/%s", reldir, ent->d_name);
        else
            snprintf(relpath, sizeof(relpath), "%s", ent->d_name);
        snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, relpath);

        if (lstat(abspath, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            collect_untracked(repo_root, relpath, idx, out, count, cap);
        } else if (S_ISREG(st.st_mode)) {
            if (sg_index_find(idx, relpath) < 0) {
                if (*count == *cap) {
                    size_t new_cap = *cap == 0 ? 16 : *cap * 2;
                    char **grown = realloc(*out, new_cap * sizeof(*grown));

                    if (grown == NULL)
                        continue;
                    *out = grown;
                    *cap = new_cap;
                }
                (*out)[(*count)++] = strdup(relpath);
            }
        }
    }
    closedir(d);
}

int sg_cmd_status(int argc, char **argv)
{
    char *git_dir;
    char *repo_root;
    char *branch;
    sg_index idx;
    unsigned char head_commit_id[SG_SHA1_RAW_LEN];
    int has_head;
    sg_flat_list head_flat;
    line_list staged = {0};
    line_list unstaged = {0};
    char **untracked = NULL;
    size_t untracked_count = 0;
    size_t untracked_cap = 0;
    size_t hi, ii, i;

    (void)argv;
    if (argc != 1) {
        fprintf(stderr, "usage: sg status\n");
        return 1;
    }

    git_dir = sg_find_git_dir();
    if (git_dir == NULL) {
        fprintf(stderr, "sg: not a git repository (or any parent up to the root)\n");
        return 1;
    }
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

    branch = sg_ref_current_branch(git_dir);
    printf("On branch %s\n", branch != NULL ? branch : "?");
    free(branch);

    has_head = (sg_ref_resolve_head(git_dir, head_commit_id) == 0);
    memset(&head_flat, 0, sizeof(head_flat));
    if (has_head) {
        sg_obj_type type;
        unsigned char *content = NULL;
        size_t content_len;
        sg_commit commit;

        if (sg_loose_read(git_dir, head_commit_id, &type, &content, &content_len) == 0 &&
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

    /* staged: HEAD tree vs index, merged since both lists are path-sorted */
    hi = 0;
    ii = 0;
    while (hi < head_flat.count || ii < idx.count) {
        int cmp;

        if (hi >= head_flat.count)
            cmp = 1;
        else if (ii >= idx.count)
            cmp = -1;
        else
            cmp = strcmp(head_flat.entries[hi].path, idx.entries[ii].path);

        if (cmp == 0) {
            if (memcmp(head_flat.entries[hi].sha1, idx.entries[ii].sha1, SG_SHA1_RAW_LEN) != 0 ||
               head_flat.entries[hi].mode != idx.entries[ii].mode)
                line_list_add(&staged, "modified:   %s", idx.entries[ii].path);
            hi++;
            ii++;
        } else if (cmp < 0) {
            line_list_add(&staged, "deleted:    %s", head_flat.entries[hi].path);
            hi++;
        } else {
            line_list_add(&staged, "new file:   %s", idx.entries[ii].path);
            ii++;
        }
    }
    sg_flat_list_free(&head_flat);

    /* unstaged: index vs working directory */
    for (i = 0; i < idx.count; i++) {
        char abspath[4096];
        unsigned char wd_sha1[SG_SHA1_RAW_LEN];
        struct stat st;

        snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, idx.entries[i].path);
        if (stat(abspath, &st) != 0) {
            line_list_add(&unstaged, "deleted:    %s", idx.entries[i].path);
            continue;
        }
        if (sg_hash_file_blob(abspath, wd_sha1) != 0) {
            line_list_add(&unstaged, "deleted:    %s", idx.entries[i].path);
            continue;
        }
        if (memcmp(wd_sha1, idx.entries[i].sha1, SG_SHA1_RAW_LEN) != 0)
            line_list_add(&unstaged, "modified:   %s", idx.entries[i].path);
    }

    collect_untracked(repo_root, "", &idx, &untracked, &untracked_count, &untracked_cap);
    if (untracked_count > 0)
        qsort(untracked, untracked_count, sizeof(*untracked), str_cmp);

    print_section("Changes to be committed:", "use \"sg restore --staged <file>...\" to unstage",
                  &staged);
    print_section("Changes not staged for commit:", "use \"sg add <file>...\" to update what will be committed",
                  &unstaged);

    if (untracked_count > 0) {
        printf("Untracked files:\n");
        printf("  (use \"sg add <file>...\" to include in what will be committed)\n");
        for (i = 0; i < untracked_count; i++)
            printf("\t%s\n", untracked[i]);
        printf("\n");
    }

    if (staged.count == 0 && unstaged.count == 0 && untracked_count == 0)
        printf("nothing to commit, working tree clean\n");

    for (i = 0; i < untracked_count; i++)
        free(untracked[i]);
    free(untracked);
    line_list_free(&staged);
    line_list_free(&unstaged);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
    return 0;
}
