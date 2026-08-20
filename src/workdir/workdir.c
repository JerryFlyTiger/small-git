#include "sg/workdir.h"

#include "sg/object.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SG_MAX_PATH_COMPONENTS 512

char *sg_repo_root(const char *git_dir)
{
    size_t len = strlen(git_dir);
    static const char suffix[] = "/.git";
    size_t suffix_len = strlen(suffix);
    char *root;

    if (len < suffix_len || strcmp(git_dir + len - suffix_len, suffix) != 0)
        return strdup(git_dir); /* unexpected shape, best effort */

    if (len == suffix_len)
        return strdup("/"); /* git_dir == "/.git" */

    root = malloc(len - suffix_len + 1);
    if (root == NULL)
        return NULL;
    memcpy(root, git_dir, len - suffix_len);
    root[len - suffix_len] = '\0';
    return root;
}

/* Collapses "." / ".." / repeated slashes out of an absolute path. out is
   always either "/" or "/comp1/comp2/...". */
static int normalize_abs_path(const char *abs, char *out, size_t out_size)
{
    char *copy = strdup(abs);
    char *tokens[SG_MAX_PATH_COMPONENTS];
    int ntok = 0;
    char *saveptr = NULL;
    char *tok;
    size_t pos = 0;
    int i;

    if (copy == NULL)
        return -1;

    tok = strtok_r(copy, "/", &saveptr);
    while (tok != NULL) {
        if (strcmp(tok, ".") == 0) {
            /* skip */
        } else if (strcmp(tok, "..") == 0) {
            if (ntok > 0)
                ntok--;
        } else {
            if (ntok >= SG_MAX_PATH_COMPONENTS) {
                free(copy);
                return -1;
            }
            tokens[ntok++] = tok;
        }
        tok = strtok_r(NULL, "/", &saveptr);
    }

    if (ntok == 0) {
        if (out_size < 2) {
            free(copy);
            return -1;
        }
        strcpy(out, "/");
        free(copy);
        return 0;
    }

    out[0] = '\0';
    for (i = 0; i < ntok; i++) {
        int n = snprintf(out + pos, out_size - pos, "/%s", tokens[i]);

        if (n < 0 || (size_t)n >= out_size - pos) {
            free(copy);
            return -1;
        }
        pos += (size_t)n;
    }
    free(copy);
    return 0;
}

/* Shared body of sg_resolve_repo_path and its allow-root sibling. When
   allow_root is set, an argument that names the repository root itself
   resolves to "" instead of being rejected. */
static char *resolve_repo_path_internal(const char *repo_root, const char *arg, int allow_root)
{
    char abs[SG_PATH_MAX];
    char normalized[SG_PATH_MAX];
    char root_norm[SG_PATH_MAX];

    if (arg[0] == '/') {
        if (snprintf(abs, sizeof(abs), "%s", arg) >= (int)sizeof(abs))
            return NULL;
    } else {
        char cwd[SG_PATH_MAX];

        if (getcwd(cwd, sizeof(cwd)) == NULL)
            return NULL;
        if (snprintf(abs, sizeof(abs), "%s/%s", cwd, arg) >= (int)sizeof(abs))
            return NULL;
    }

    if (normalize_abs_path(abs, normalized, sizeof(normalized)) != 0)
        return NULL;
    if (normalize_abs_path(repo_root, root_norm, sizeof(root_norm)) != 0)
        return NULL;

    if (strcmp(root_norm, "/") == 0) {
        if (strcmp(normalized, "/") == 0)
            return allow_root ? strdup("") : NULL;
        return strdup(normalized + 1);
    }

    {
        size_t root_len = strlen(root_norm);

        if (strncmp(normalized, root_norm, root_len) != 0)
            return NULL;
        if (normalized[root_len] == '\0')
            return allow_root ? strdup("") : NULL;
        if (normalized[root_len] != '/')
            return NULL;
        return strdup(normalized + root_len + 1);
    }
}

char *sg_resolve_repo_path(const char *repo_root, const char *arg)
{
    return resolve_repo_path_internal(repo_root, arg, 0);
}

char *sg_resolve_repo_path_allow_root(const char *repo_root, const char *arg)
{
    return resolve_repo_path_internal(repo_root, arg, 1);
}

int sg_mkdir_parents(const char *path)
{
    char buf[SG_PATH_MAX];
    size_t len = strlen(path);
    size_t i;

    if (len >= sizeof(buf))
        return -1;
    memcpy(buf, path, len + 1);

    for (i = 1; i < len; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
                buf[i] = '/';
                return -1;
            }
            buf[i] = '/';
        }
    }
    return 0;
}

int sg_read_file(const char *path, unsigned char **out, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    size_t cap = 65536;
    unsigned char *buf;
    size_t used = 0;

    if (f == NULL)
        return -1;

    buf = malloc(cap);
    if (buf == NULL) {
        fclose(f);
        return -1;
    }

    for (;;) {
        size_t n;

        if (used == cap) {
            size_t new_cap = cap * 2;
            unsigned char *grown = realloc(buf, new_cap);

            if (grown == NULL) {
                free(buf);
                fclose(f);
                return -1;
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
        return -1;
    }
    fclose(f);

    *out = buf;
    *out_len = used;
    return 0;
}

int sg_write_file_mkdirs(const char *path, const unsigned char *data, size_t len, int mode)
{
    FILE *f;

    if (sg_mkdir_parents(path) != 0)
        return -1;

    f = fopen(path, "wb");
    if (f == NULL)
        return -1;
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0)
        return -1;
    if (chmod(path, (mode_t)mode) != 0)
        return -1;
    return 0;
}

int sg_is_symlink(const char *path)
{
    struct stat st;

    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

int sg_path_join(char *out, size_t out_size, const char *base, const char *rel)
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

int sg_hash_file_blob(const char *path, unsigned char sha1_out[SG_SHA1_RAW_LEN])
{
    unsigned char *buf;
    size_t len;

    if (sg_read_file(path, &buf, &len) != 0)
        return -1;
    sg_object_hash(SG_OBJ_BLOB, buf, len, sha1_out);
    free(buf);
    return 0;
}
