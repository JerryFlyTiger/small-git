#include "sg/cli.h"

#include "sg/hash.h"
#include "sg/index.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/repo.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int add_one(const char *git_dir, const char *repo_root, sg_index *idx, const char *arg)
{
    char *rel_path;
    char abs_path[4096];
    struct stat st;
    unsigned char *content;
    size_t content_len;
    sg_index_entry entry;
    unsigned int mode;

    rel_path = sg_resolve_repo_path(repo_root, arg);
    if (rel_path == NULL) {
        fprintf(stderr, "sg: '%s' is outside the repository\n", arg);
        return -1;
    }
    snprintf(abs_path, sizeof(abs_path), "%s/%s", repo_root, rel_path);

    if (sg_is_symlink(abs_path)) {
        fprintf(stderr, "sg: warning: '%s' is a symlink, skipping (unsupported in phase 2)\n", arg);
        free(rel_path);
        return 0;
    }

    if (stat(abs_path, &st) != 0) {
        fprintf(stderr, "sg: cannot stat '%s': no such file\n", arg);
        free(rel_path);
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        fprintf(stderr, "sg: '%s' is a directory, adding directories is not supported\n", arg);
        free(rel_path);
        return -1;
    }

    if (sg_read_file(abs_path, &content, &content_len) != 0) {
        fprintf(stderr, "sg: failed to read '%s'\n", arg);
        free(rel_path);
        return -1;
    }

    mode = (st.st_mode & 0111) ? 0100755 : 0100644;

    memset(&entry, 0, sizeof(entry));
    entry.ctime_sec = (unsigned int)st.st_ctime;
#if defined(__APPLE__)
    entry.ctime_nsec = (unsigned int)st.st_ctimespec.tv_nsec;
    entry.mtime_nsec = (unsigned int)st.st_mtimespec.tv_nsec;
#else
    entry.ctime_nsec = (unsigned int)st.st_ctim.tv_nsec;
    entry.mtime_nsec = (unsigned int)st.st_mtim.tv_nsec;
#endif
    entry.mtime_sec = (unsigned int)st.st_mtime;
    entry.dev = (unsigned int)st.st_dev;
    entry.ino = (unsigned int)st.st_ino;
    entry.mode = mode;
    entry.uid = (unsigned int)st.st_uid;
    entry.gid = (unsigned int)st.st_gid;
    entry.file_size = (unsigned int)content_len;
    entry.path = rel_path;

    if (sg_loose_write(git_dir, SG_OBJ_BLOB, content, content_len, entry.sha1) != 0) {
        fprintf(stderr, "sg: failed to write blob for '%s'\n", arg);
        free(content);
        free(rel_path);
        return -1;
    }
    free(content);

    /* If rel_path currently carries stage 1/2/3 entries (an unresolved
       merge conflict), staging it now means the user has resolved it:
       clear those before writing the ordinary stage-0 entry below. */
    sg_index_remove_all_stages(idx, rel_path);

    if (sg_index_upsert(idx, &entry) != 0) {
        fprintf(stderr, "sg: failed to stage '%s'\n", arg);
        free(rel_path);
        return -1;
    }

    free(rel_path);
    return 0;
}

int sg_cmd_add(int argc, char **argv)
{
    char *git_dir;
    char *repo_root;
    sg_index idx;
    int i;
    int rc = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: sg add <path>...\n");
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

    for (i = 1; i < argc; i++) {
        if (add_one(git_dir, repo_root, &idx, argv[i]) != 0)
            rc = 1;
    }

    if (rc == 0 && sg_index_write(git_dir, &idx) != 0) {
        fprintf(stderr, "sg: failed to write index\n");
        rc = 1;
    }

    sg_index_free(&idx);
    free(git_dir);
    free(repo_root);
    return rc;
}
