#include "sg/refs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SG_PATH_MAX 4096
#define HEAD_PREFIX "ref: "
#define BRANCH_PREFIX "refs/heads/"

/* Branch names get concatenated straight into a filesystem path below; a
   name like "../../../tmp/evil" (e.g. via `sg switch -c`) would otherwise
   write/read outside refs/heads. */
static int branch_name_is_safe(const char *name)
{
    if (name[0] == '\0' || name[0] == '/')
        return 0;
    if (strstr(name, "..") != NULL)
        return 0;
    return 1;
}

static char *read_small_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    size_t cap = 256;
    size_t used = 0;

    if (f == NULL)
        return NULL;

    buf = malloc(cap);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }

    for (;;) {
        size_t n;

        if (used == cap) {
            size_t new_cap = cap * 2;
            char *grown = realloc(buf, new_cap);

            if (grown == NULL) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = grown;
            cap = new_cap;
        }
        n = fread(buf + used, 1, cap - used, f);
        used += n;
        if (n == 0)
            break;
    }
    if (ferror(f)) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);

    buf[used] = '\0'; /* cap always leaves room: used < cap right after growth check, or grows */
    if (used == cap) {
        char *grown = realloc(buf, cap + 1);

        if (grown == NULL) {
            free(buf);
            return NULL;
        }
        buf = grown;
        buf[used] = '\0';
    }
    return buf;
}

char *sg_ref_current_branch(const char *git_dir)
{
    char path[SG_PATH_MAX];
    char *content;
    char *nl;
    char *branch;

    snprintf(path, sizeof(path), "%s/HEAD", git_dir);
    content = read_small_file(path);
    if (content == NULL)
        return NULL;

    if (strncmp(content, HEAD_PREFIX, strlen(HEAD_PREFIX)) != 0) {
        free(content);
        return NULL;
    }

    nl = strchr(content, '\n');
    if (nl != NULL)
        *nl = '\0';

    {
        const char *ref = content + strlen(HEAD_PREFIX);

        if (strncmp(ref, BRANCH_PREFIX, strlen(BRANCH_PREFIX)) != 0) {
            free(content);
            return NULL;
        }
        branch = strdup(ref + strlen(BRANCH_PREFIX));
    }
    free(content);
    return branch;
}

int sg_ref_read_branch(const char *git_dir, const char *branch, unsigned char id_out[SG_SHA1_RAW_LEN])
{
    char path[SG_PATH_MAX];
    char *content;
    char *nl;
    int rc;

    if (!branch_name_is_safe(branch))
        return -1;

    snprintf(path, sizeof(path), "%s/refs/heads/%s", git_dir, branch);
    content = read_small_file(path);
    if (content == NULL)
        return -1;

    nl = strchr(content, '\n');
    if (nl != NULL)
        *nl = '\0';

    rc = sg_hex_to_sha1(content, id_out);
    free(content);
    return rc;
}

int sg_ref_resolve_head(const char *git_dir, unsigned char id_out[SG_SHA1_RAW_LEN])
{
    char *branch = sg_ref_current_branch(git_dir);
    int rc;

    if (branch == NULL)
        return -1;
    rc = sg_ref_read_branch(git_dir, branch, id_out);
    free(branch);
    return rc;
}

int sg_ref_update_branch(const char *git_dir, const char *branch, const unsigned char id[SG_SHA1_RAW_LEN])
{
    char path[SG_PATH_MAX];
    char hex[SG_SHA1_HEX_LEN + 1];
    FILE *f;

    if (!branch_name_is_safe(branch))
        return -1;

    snprintf(path, sizeof(path), "%s/refs/heads/%s", git_dir, branch);
    sg_sha1_to_hex(id, hex);

    f = fopen(path, "wb");
    if (f == NULL)
        return -1;
    if (fprintf(f, "%s\n", hex) < 0) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

int sg_ref_branch_exists(const char *git_dir, const char *branch)
{
    char path[SG_PATH_MAX];
    struct stat st;

    if (!branch_name_is_safe(branch))
        return 0;

    snprintf(path, sizeof(path), "%s/refs/heads/%s", git_dir, branch);
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}
