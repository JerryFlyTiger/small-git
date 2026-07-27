#include "sg/repo.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SG_PATH_MAX 4096

static int write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");

    if (f == NULL)
        return -1;
    if (fputs(content, f) == EOF) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

static int mkdir_ignore_existing(const char *path)
{
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

int sg_repo_init(const char *dir)
{
    const char *base = (dir != NULL) ? dir : ".";
    char git_dir[SG_PATH_MAX];
    char path[SG_PATH_MAX];

    if (mkdir_ignore_existing(base) != 0)
        return -1;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", base);
    if (mkdir_ignore_existing(git_dir) != 0)
        return -1;

    snprintf(path, sizeof(path), "%s/objects", git_dir);
    if (mkdir_ignore_existing(path) != 0)
        return -1;

    snprintf(path, sizeof(path), "%s/refs", git_dir);
    if (mkdir_ignore_existing(path) != 0)
        return -1;

    snprintf(path, sizeof(path), "%s/refs/heads", git_dir);
    if (mkdir_ignore_existing(path) != 0)
        return -1;

    snprintf(path, sizeof(path), "%s/refs/tags", git_dir);
    if (mkdir_ignore_existing(path) != 0)
        return -1;

    snprintf(path, sizeof(path), "%s/HEAD", git_dir);
    if (write_file(path, "ref: refs/heads/master\n") != 0)
        return -1;

    snprintf(path, sizeof(path), "%s/config", git_dir);
    if (write_file(path,
                   "[core]\n"
                   "\trepositoryformatversion = 0\n"
                   "\tfilemode = true\n"
                   "\tbare = false\n"
                   "\tlogallrefupdates = true\n") != 0)
        return -1;

    snprintf(path, sizeof(path), "%s/description", git_dir);
    if (write_file(path,
                   "Unnamed repository; edit this file 'description' to name the "
                   "repository.\n") != 0)
        return -1;

    return 0;
}

char *sg_find_git_dir(void)
{
    char cwd[SG_PATH_MAX];
    char candidate[SG_PATH_MAX + 8];

    if (getcwd(cwd, sizeof(cwd)) == NULL)
        return NULL;

    for (;;) {
        struct stat st;
        char *slash;

        snprintf(candidate, sizeof(candidate), "%s/.git", cwd);
        if (stat(candidate, &st) == 0 && S_ISDIR(st.st_mode))
            return strdup(candidate);

        if (strcmp(cwd, "/") == 0)
            break;

        slash = strrchr(cwd, '/');
        if (slash == NULL)
            break;
        /* slash == cwd means cwd is "/something": truncate to root "/"
           itself for one final check, instead of stopping one level early */
        *(slash == cwd ? slash + 1 : slash) = '\0';
    }

    return NULL;
}

char *sg_require_git_dir(void)
{
    char *git_dir = sg_find_git_dir();

    if (git_dir == NULL) {
        fprintf(stderr, "sg: not a git repository (or any parent up to the root)\n");
        fprintf(stderr, "sg: 執行 `sg init` 建立一個新的 repository\n");
    }
    return git_dir;
}
