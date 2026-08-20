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

static int ascii_tolower(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 'a';
    return c;
}

/* The code points real git ignores when deciding whether a name aliases
   ".git" -- HFS+ folds them away, so ".g<U+200C>it" and ".git" can be the
   same directory there. Measured against git 2.55.0 by feeding ".g<cp>it"
   to git add: these sixteen are refused while U+200B, U+2060, U+00A0 and
   U+3000 are accepted, so the set is specific and NOT "anything
   invisible" -- do not widen it by intuition.

   Matched as raw UTF-8 byte sequences rather than decoded: all sixteen are
   three bytes, so a decoder would buy nothing here and would need its own
   rules for malformed input, which this has no opinion about. Returns the
   sequence's length, or 0 if p does not start with one. */
static size_t ignorable_utf8_len(const unsigned char *p, size_t remaining)
{
    if (remaining < 3)
        return 0;
    /* U+200C..U+200F and U+202A..U+202E */
    if (p[0] == 0xE2 && p[1] == 0x80 &&
       ((p[2] >= 0x8C && p[2] <= 0x8F) || (p[2] >= 0xAA && p[2] <= 0xAE)))
        return 3;
    /* U+206A..U+206F */
    if (p[0] == 0xE2 && p[1] == 0x81 && p[2] >= 0xAA && p[2] <= 0xAF)
        return 3;
    /* U+FEFF */
    if (p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
        return 3;
    return 0;
}

/* Whether name aliases ".git" on some filesystem sg might be running on.
   Two independent folds, both of which real git applies:

     - a trailing run of '.' and ' ' is dropped, because a filesystem that
       trims those on write (NTFS does) would let ".git." reach ".git";
     - the ignorable code points above are dropped wherever they appear,
       because HFS+ compares as if they were not there.

   The trailing run is stripped first and includes ignorables, so
   ".git.<U+200C>" and ".git<U+200C>." both fold to ".git" -- stripping only
   one of the two kinds would leave the other as a way through. */
static int aliases_dotgit(const char *name)
{
    const unsigned char *p = (const unsigned char *)name;
    size_t len = strlen(name);
    size_t i;
    size_t matched = 0;
    static const char want[] = ".git";

    for (;;) {
        if (len > 0 && (name[len - 1] == '.' || name[len - 1] == ' ')) {
            len--;
            continue;
        }
        if (len >= 3 && ignorable_utf8_len(p + len - 3, 3) == 3) {
            len -= 3;
            continue;
        }
        break;
    }

    i = 0;
    while (i < len) {
        size_t skip = ignorable_utf8_len(p + i, len - i);

        if (skip > 0) {
            i += skip;
            continue;
        }
        if (matched == 4)
            return 0; /* something beyond ".git" -- ".gitx", not an alias */
        if (ascii_tolower(p[i]) != (unsigned char)want[matched])
            return 0;
        matched++;
        i++;
    }
    return matched == 4;
}

int sg_path_component_is_safe(const char *name)
{
    if (name[0] == '\0')
        return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    if (strchr(name, '/') != NULL)
        return 0;
    if (aliases_dotgit(name))
        return 0;

    return 1;
}

int sg_relpath_is_safe(const char *relpath)
{
    const char *p = relpath;
    size_t len = strlen(relpath);

    if (len == 0 || relpath[0] == '/' || relpath[len - 1] == '/')
        return 0;
    while (*p != '\0') {
        const char *start = p;
        char comp[SG_PATH_MAX];
        size_t comp_len;

        while (*p != '\0' && *p != '/')
            p++;
        comp_len = (size_t)(p - start);
        if (comp_len == 0 || comp_len >= sizeof(comp))
            return 0; /* empty component ("a//b"), or a single component too long to check */
        memcpy(comp, start, comp_len);
        comp[comp_len] = '\0';
        if (!sg_path_component_is_safe(comp))
            return 0;
        if (*p == '/')
            p++;
    }
    return 1;
}

void sg_prune_empty_parents(const char *repo_root, const char *relpath)
{
    char cur[SG_PATH_MAX];

    if (!sg_relpath_is_safe(relpath))
        return; /* would resolve at or above repo_root, or names a hostile
                    component such as ".git" -- never act on it */
    if (strlen(relpath) >= sizeof(cur))
        return; /* truncated -- never act on a truncated path */
    strcpy(cur, relpath);

    for (;;) {
        char *slash = strrchr(cur, '/');
        char absdir[SG_PATH_MAX];

        if (slash == NULL)
            return; /* cur is now a top-level name; its parent is repo_root, never removed */
        *slash = '\0';
        if (sg_path_join(absdir, sizeof(absdir), repo_root, cur) != 0)
            return; /* truncated -- never act on a truncated path */
        if (rmdir(absdir) != 0)
            return; /* not empty (or some other reason) -- ancestors won't be empty either */
    }
}
