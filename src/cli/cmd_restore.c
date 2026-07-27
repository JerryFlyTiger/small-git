#include "sg/cli.h"

#include "sg/hash.h"
#include "sg/index.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int flat_find(const sg_flat_list *list, const char *path)
{
    size_t lo = 0;
    size_t hi = list->count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(list->entries[mid].path, path);

        if (cmp == 0)
            return (int)mid;
        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

static int restore_worktree(const char *git_dir, const char *repo_root, sg_index *idx,
                            const char *arg, const char *rel)
{
    int pos = sg_index_find(idx, rel);
    sg_obj_type type;
    unsigned char *content;
    size_t content_len;
    char abspath[4096];
    int rc;

    if (pos < 0) {
        fprintf(stderr, "sg: '%s' is not tracked in the index\n", arg);
        return -1;
    }
    if (sg_loose_read(git_dir, idx->entries[pos].sha1, &type, &content, &content_len) != 0) {
        fprintf(stderr, "sg: failed to read staged content for '%s'\n", arg);
        return -1;
    }

    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, rel);
    rc = sg_write_file_mkdirs(abspath, content, content_len, (int)(idx->entries[pos].mode & 0777));
    free(content);
    if (rc != 0)
        fprintf(stderr, "sg: failed to restore '%s'\n", arg);
    return rc;
}

static void restore_staged(sg_index *idx, int has_head, const sg_flat_list *head_flat,
                           const char *rel)
{
    int head_pos = has_head ? flat_find(head_flat, rel) : -1;

    if (head_pos < 0) {
        sg_index_remove(idx, rel); /* not tracked by HEAD: unstaging removes it */
    } else {
        sg_index_entry entry;
        int existing = sg_index_find(idx, rel);

        memset(&entry, 0, sizeof(entry));
        if (existing >= 0)
            entry = idx->entries[existing];
        entry.mode = head_flat->entries[head_pos].mode;
        memcpy(entry.sha1, head_flat->entries[head_pos].sha1, SG_SHA1_RAW_LEN);
        entry.path = (char *)rel;
        sg_index_upsert(idx, &entry);
    }
}

int sg_cmd_restore(int argc, char **argv)
{
    static const char usage[] = "usage: sg restore [--staged] <path>...\n";
    int staged = 0;
    int npaths = 0;
    char *git_dir;
    char *repo_root;
    sg_index idx;
    unsigned char head_commit_id[SG_SHA1_RAW_LEN];
    int has_head = 0;
    sg_flat_list head_flat;
    int i;
    int rc = 0;

    memset(&head_flat, 0, sizeof(head_flat));

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--staged") == 0)
            staged = 1;
        else
            npaths++;
    }
    if (npaths == 0) {
        fputs(usage, stderr);
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

    if (staged) {
        has_head = (sg_ref_resolve_head(git_dir, head_commit_id) == 0);
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
        }
    }

    for (i = 1; i < argc; i++) {
        char *rel;

        if (strcmp(argv[i], "--staged") == 0)
            continue;

        rel = sg_resolve_repo_path(repo_root, argv[i]);
        if (rel == NULL) {
            fprintf(stderr, "sg: '%s' is outside the repository\n", argv[i]);
            rc = 1;
            continue;
        }

        if (staged)
            restore_staged(&idx, has_head, &head_flat, rel);
        else if (restore_worktree(git_dir, repo_root, &idx, argv[i], rel) != 0)
            rc = 1;

        free(rel);
    }

    if (staged && sg_index_write(git_dir, &idx) != 0) {
        fprintf(stderr, "sg: failed to write index\n");
        rc = 1;
    }

    sg_flat_list_free(&head_flat);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
    return rc;
}
