#include "sg/cli.h"

#include "sg/hash.h"
#include "sg/index.h"
#include "sg/objstore.h"
#include "sg/object.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/status.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

#include <dirent.h>
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
    sg_status_list staged;
    sg_status_list unstaged;
    char **untracked = NULL;
    size_t untracked_count = 0;
    size_t untracked_cap = 0;
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

    if (sg_status_diff_unstaged(repo_root, &idx, &unstaged) != 0)
        fprintf(stderr, "sg: warning: out of memory computing unstaged changes\n");

    collect_untracked(repo_root, "", &idx, &untracked, &untracked_count, &untracked_cap);
    if (untracked_count > 0)
        qsort(untracked, untracked_count, sizeof(*untracked), str_cmp);

    print_section("Changes to be committed:", staged_hints, 1, &staged);
    print_section("Changes not staged for commit:", unstaged_hints, 2, &unstaged);

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
    sg_status_list_free(&staged);
    sg_status_list_free(&unstaged);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
    return 0;
}
