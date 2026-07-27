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
#include <sys/stat.h>

static int write_head(const char *git_dir, const char *branch)
{
    char path[4096];
    FILE *f;

    snprintf(path, sizeof(path), "%s/HEAD", git_dir);
    f = fopen(path, "wb");
    if (f == NULL)
        return -1;
    if (fprintf(f, "ref: refs/heads/%s\n", branch) < 0) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

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

int sg_cmd_switch(int argc, char **argv)
{
    int create = 0;
    const char *branch_arg = NULL;
    char *git_dir;
    char *repo_root;
    unsigned char target_commit_id[SG_SHA1_RAW_LEN];
    sg_obj_type type;
    unsigned char *content;
    size_t content_len;
    sg_commit commit;
    sg_flat_list target_flat;
    sg_index old_idx;
    sg_index new_idx;
    size_t i;
    int rc = 0;

    for (i = 1; (int)i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            create = 1;
        } else if (branch_arg == NULL) {
            branch_arg = argv[i];
        } else {
            fprintf(stderr, "usage: sg switch [-c] <branch>\n");
            return 1;
        }
    }
    if (branch_arg == NULL) {
        fprintf(stderr, "usage: sg switch [-c] <branch>\n");
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

    fprintf(stderr,
           "sg switch 目前會直接覆蓋工作目錄與 index，尚未提供未提交變更保護 "
           "(no safety net for uncommitted changes yet)\n");

    if (create) {
        unsigned char head_id[SG_SHA1_RAW_LEN];

        if (sg_ref_branch_exists(git_dir, branch_arg)) {
            fprintf(stderr, "sg: a branch named '%s' already exists\n", branch_arg);
            free(git_dir);
            free(repo_root);
            return 1;
        }
        if (sg_ref_resolve_head(git_dir, head_id) != 0) {
            fprintf(stderr, "sg: cannot create branch '%s': current branch has no commits yet\n",
                   branch_arg);
            free(git_dir);
            free(repo_root);
            return 1;
        }
        if (sg_ref_update_branch(git_dir, branch_arg, head_id) != 0) {
            fprintf(stderr, "sg: failed to create branch '%s'\n", branch_arg);
            free(git_dir);
            free(repo_root);
            return 1;
        }
    } else if (!sg_ref_branch_exists(git_dir, branch_arg)) {
        fprintf(stderr, "sg: invalid reference: %s\n", branch_arg);
        free(git_dir);
        free(repo_root);
        return 1;
    }

    if (sg_ref_read_branch(git_dir, branch_arg, target_commit_id) != 0) {
        fprintf(stderr, "sg: failed to read branch '%s'\n", branch_arg);
        free(git_dir);
        free(repo_root);
        return 1;
    }
    if (sg_loose_read(git_dir, target_commit_id, &type, &content, &content_len) != 0 ||
       type != SG_OBJ_COMMIT) {
        fprintf(stderr, "sg: corrupt commit for branch '%s'\n", branch_arg);
        free(git_dir);
        free(repo_root);
        return 1;
    }
    if (sg_commit_parse(content, content_len, &commit) != 0) {
        fprintf(stderr, "sg: malformed commit for branch '%s'\n", branch_arg);
        free(content);
        free(git_dir);
        free(repo_root);
        return 1;
    }
    free(content);

    if (sg_tree_flatten(git_dir, commit.tree, &target_flat) != 0) {
        fprintf(stderr, "sg: failed to read tree for branch '%s'\n", branch_arg);
        sg_commit_free(&commit);
        free(git_dir);
        free(repo_root);
        return 1;
    }
    sg_commit_free(&commit);

    if (sg_index_read(git_dir, &old_idx) != 0) {
        fprintf(stderr, "sg: failed to read index (corrupt?)\n");
        sg_flat_list_free(&target_flat);
        free(git_dir);
        free(repo_root);
        return 1;
    }

    /* remove working-tree files that are currently tracked but absent from
       the target tree */
    for (i = 0; i < old_idx.count; i++) {
        if (flat_find(&target_flat, old_idx.entries[i].path) < 0) {
            char abspath[4096];

            snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, old_idx.entries[i].path);
            remove(abspath);
        }
    }
    sg_index_free(&old_idx);

    memset(&new_idx, 0, sizeof(new_idx));
    for (i = 0; i < target_flat.count; i++) {
        char abspath[4096];
        unsigned char *blob_content;
        size_t blob_len;
        sg_obj_type blob_type;
        struct stat st;
        sg_index_entry entry;

        snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, target_flat.entries[i].path);
        if (sg_loose_read(git_dir, target_flat.entries[i].sha1, &blob_type, &blob_content, &blob_len) !=
           0) {
            fprintf(stderr, "sg: missing blob for '%s'\n", target_flat.entries[i].path);
            rc = 1;
            continue;
        }
        if (sg_write_file_mkdirs(abspath, blob_content, blob_len,
                                 (int)(target_flat.entries[i].mode & 0777)) != 0) {
            fprintf(stderr, "sg: failed to write '%s'\n", target_flat.entries[i].path);
            free(blob_content);
            rc = 1;
            continue;
        }
        free(blob_content);

        if (stat(abspath, &st) != 0) {
            rc = 1;
            continue;
        }

        memset(&entry, 0, sizeof(entry));
        entry.ctime_sec = (unsigned int)st.st_ctime;
        entry.mtime_sec = (unsigned int)st.st_mtime;
#if defined(__APPLE__)
        entry.ctime_nsec = (unsigned int)st.st_ctimespec.tv_nsec;
        entry.mtime_nsec = (unsigned int)st.st_mtimespec.tv_nsec;
#else
        entry.ctime_nsec = (unsigned int)st.st_ctim.tv_nsec;
        entry.mtime_nsec = (unsigned int)st.st_mtim.tv_nsec;
#endif
        entry.dev = (unsigned int)st.st_dev;
        entry.ino = (unsigned int)st.st_ino;
        entry.mode = target_flat.entries[i].mode;
        entry.uid = (unsigned int)st.st_uid;
        entry.gid = (unsigned int)st.st_gid;
        entry.file_size = (unsigned int)st.st_size;
        memcpy(entry.sha1, target_flat.entries[i].sha1, SG_SHA1_RAW_LEN);
        entry.path = target_flat.entries[i].path;

        if (sg_index_upsert(&new_idx, &entry) != 0) {
            fprintf(stderr, "sg: failed to stage '%s'\n", target_flat.entries[i].path);
            rc = 1;
        }
    }
    sg_flat_list_free(&target_flat);

    if (rc == 0 && sg_index_write(git_dir, &new_idx) != 0) {
        fprintf(stderr, "sg: failed to write index\n");
        rc = 1;
    }
    sg_index_free(&new_idx);

    if (rc == 0 && write_head(git_dir, branch_arg) != 0) {
        fprintf(stderr, "sg: failed to update HEAD\n");
        rc = 1;
    }

    if (rc == 0)
        printf("Switched to branch '%s'\n", branch_arg);

    free(git_dir);
    free(repo_root);
    return rc;
}
