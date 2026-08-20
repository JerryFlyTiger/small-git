#include "sg/repo.h"

#include "sg/chunk.h"
#include "sg/refs.h"
#include "sg/workdir.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

    if (sg_ref_set_head(git_dir, "master", NULL) != 0)
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

char *sg_repo_read_remote_url(const char *git_dir, const char *remote)
{
    char path[SG_PATH_MAX];
    char header[256];
    FILE *f;
    char line[1024];
    int in_section = 0;
    char *result = NULL;

    snprintf(path, sizeof(path), "%s/config", git_dir);
    f = fopen(path, "r");
    if (f == NULL)
        return NULL;

    snprintf(header, sizeof(header), "[remote \"%s\"]", remote);

    while (fgets(line, sizeof(line), f) != NULL) {
        char *p = line;
        size_t plen;

        while (*p == ' ' || *p == '\t')
            p++;
        plen = strlen(p);
        while (plen > 0 && (p[plen - 1] == '\n' || p[plen - 1] == '\r'))
            p[--plen] = '\0';

        if (p[0] == '[') {
            in_section = (strcmp(p, header) == 0);
            continue;
        }
        if (in_section && strncmp(p, "url", 3) == 0) {
            char *eq = strchr(p, '=');

            if (eq != NULL) {
                char *val = eq + 1;

                while (*val == ' ' || *val == '\t')
                    val++;
                result = strdup(val);
                break;
            }
        }
    }
    fclose(f);
    return result;
}

int sg_repo_read_chunk_config(const char *git_dir, int *enabled_out, size_t *threshold_out)
{
    char path[SG_PATH_MAX];
    FILE *f;
    char line[1024];
    int in_section = 0;

    *enabled_out = 0;
    *threshold_out = SG_CHUNK_DEFAULT_THRESHOLD;

    snprintf(path, sizeof(path), "%s/config", git_dir);
    f = fopen(path, "r");
    if (f == NULL)
        return 0;

    while (fgets(line, sizeof(line), f) != NULL) {
        char *p = line;
        size_t plen;

        while (*p == ' ' || *p == '\t')
            p++;
        plen = strlen(p);
        while (plen > 0 && (p[plen - 1] == '\n' || p[plen - 1] == '\r'))
            p[--plen] = '\0';

        if (p[0] == '[') {
            in_section = (strcmp(p, "[sg]") == 0);
            continue;
        }
        if (!in_section)
            continue;

        if (strncmp(p, "chunking", 8) == 0) {
            char *eq = strchr(p, '=');

            if (eq != NULL) {
                char *val = eq + 1;
                size_t vlen;

                while (*val == ' ' || *val == '\t')
                    val++;
                vlen = strlen(val);
                while (vlen > 0 && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t'))
                    val[--vlen] = '\0';
                *enabled_out = (strcmp(val, "true") == 0);
            }
        } else if (strncmp(p, "chunkthreshold", 14) == 0) {
            char *eq = strchr(p, '=');

            if (eq != NULL) {
                char *val = eq + 1;
                char *endp = NULL;
                unsigned long parsed;

                while (*val == ' ' || *val == '\t')
                    val++;
                errno = 0;
                parsed = strtoul(val, &endp, 10);
                if (endp != val && errno == 0 && parsed > 0)
                    *threshold_out = (size_t)parsed;
            }
        }
    }
    fclose(f);
    return 0;
}

/* Config key for sg_repo_mark_chunking_used/sg_repo_chunking_was_used.
   Deliberately NOT named anything starting with "chunking" (e.g. the
   phase 6 report's own suggested "chunkingused"): sg_repo_read_chunk_config
   above tests its `chunking` flag with strncmp(p, "chunking", 8) == 0, which
   would also match a line like "chunkingused = true" (its first 8 characters
   are literally "chunking") and silently turn chunking on even when the user
   never set `chunking = true` themselves. "everchunked" shares no prefix
   with either "chunking" or "chunkthreshold", so it can never be
   misinterpreted by that parser. */
#define SG_CHUNKING_USED_KEY "everchunked"

int sg_repo_chunking_was_used(const char *git_dir)
{
    char path[SG_PATH_MAX];
    FILE *f;
    char line[1024];
    int in_section = 0;
    int used = 0;
    size_t key_len = strlen(SG_CHUNKING_USED_KEY);

    snprintf(path, sizeof(path), "%s/config", git_dir);
    f = fopen(path, "r");
    if (f == NULL)
        return 0;

    while (fgets(line, sizeof(line), f) != NULL) {
        char *p = line;
        size_t plen;

        while (*p == ' ' || *p == '\t')
            p++;
        plen = strlen(p);
        while (plen > 0 && (p[plen - 1] == '\n' || p[plen - 1] == '\r'))
            p[--plen] = '\0';

        if (p[0] == '[') {
            in_section = (strcmp(p, "[sg]") == 0);
            continue;
        }
        if (!in_section)
            continue;

        if (strncmp(p, SG_CHUNKING_USED_KEY, key_len) == 0) {
            char *eq = strchr(p, '=');

            if (eq != NULL) {
                char *val = eq + 1;
                size_t vlen;

                while (*val == ' ' || *val == '\t')
                    val++;
                vlen = strlen(val);
                while (vlen > 0 && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t'))
                    val[--vlen] = '\0';
                if (strcmp(val, "true") == 0)
                    used = 1;
            }
        }
    }
    fclose(f);
    return used;
}

int sg_repo_mark_chunking_used(const char *git_dir)
{
    char path[SG_PATH_MAX];
    FILE *f;

    /* Idempotent: an already-set marker is a cheap no-op rather than an
       ever-growing pile of duplicate [sg] stanzas appended on every single
       chunked write. */
    if (sg_repo_chunking_was_used(git_dir))
        return 0;

    snprintf(path, sizeof(path), "%s/config", git_dir);
    f = fopen(path, "a");
    if (f == NULL)
        return -1;
    if (fprintf(f, "[sg]\n\t%s = true\n", SG_CHUNKING_USED_KEY) < 0) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}
