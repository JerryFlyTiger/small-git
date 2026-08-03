#include "sg/refs.h"

#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SG_PATH_MAX 4096
#define HEAD_PREFIX "ref: "
#define BRANCH_PREFIX "refs/heads/"

int sg_ref_branch_name_is_safe(const char *name)
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

/* defined below, next to sg_ref_read_path -- `git gc` packs refs away
   from their loose files, and a lookup that misses that concludes the
   branch has no commits, which turns the next commit into a root commit
   and orphans the whole history. */
static int read_packed_ref(const char *git_dir, const char *ref_path,
                           unsigned char id_out[SG_SHA1_RAW_LEN]);

int sg_ref_read_branch(const char *git_dir, const char *branch, unsigned char id_out[SG_SHA1_RAW_LEN])
{
    char path[SG_PATH_MAX];
    char ref_name[SG_PATH_MAX];
    char *content;
    char *nl;
    int rc;

    if (!sg_ref_branch_name_is_safe(branch))
        return -1;

    snprintf(path, sizeof(path), "%s/refs/heads/%s", git_dir, branch);
    content = read_small_file(path);
    if (content == NULL) {
        snprintf(ref_name, sizeof(ref_name), "refs/heads/%s", branch);
        return read_packed_ref(git_dir, ref_name, id_out);
    }

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

    if (!sg_ref_branch_name_is_safe(branch))
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
    unsigned char id[SG_SHA1_RAW_LEN];

    if (!sg_ref_branch_name_is_safe(branch))
        return 0;

    snprintf(path, sizeof(path), "%s/refs/heads/%s", git_dir, branch);
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
        return 1;

    /* a packed branch is still a branch -- see read_packed_ref */
    return sg_ref_read_branch(git_dir, branch, id) == 0;
}

int sg_ref_write_path(const char *git_dir, const char *ref_path, const unsigned char id[SG_SHA1_RAW_LEN])
{
    char full_path[SG_PATH_MAX];
    char content[SG_SHA1_HEX_LEN + 2];

    if (!sg_ref_branch_name_is_safe(ref_path))
        return -1;

    snprintf(full_path, sizeof(full_path), "%s/%s", git_dir, ref_path);
    sg_sha1_to_hex(id, content);
    content[SG_SHA1_HEX_LEN] = '\n';
    content[SG_SHA1_HEX_LEN + 1] = '\0';
    return sg_write_file_mkdirs(full_path, (const unsigned char *)content, SG_SHA1_HEX_LEN + 1, 0644);
}

/* Parses git's packed-refs file (a plain "<40-hex-sha1> <ref-name>\n" list,
   with an optional leading '#'-comment header line and, for annotated tags
   only, '^'-prefixed peeled-object lines following the ref they annotate)
   looking for exactly ref_path. `git gc` (via the `git pack-refs` step it
   runs) routinely moves refs out of their loose per-ref file and into this
   single file instead -- including SG_CHUNK_KEEPALIVE_REF, since by design
   it's an ordinary ref from git's own perspective (see that ref's doc
   comment in chunk.h) -- so a reader that only ever checks the loose path
   would start reporting "ref doesn't exist" right after a real `git gc`,
   even though the ref is still there. Returns 0 with id_out filled in if
   found, -1 if packed-refs is missing/unreadable or doesn't list ref_path. */
static int read_packed_ref(const char *git_dir, const char *ref_path,
                           unsigned char id_out[SG_SHA1_RAW_LEN])
{
    char path[SG_PATH_MAX];
    char *content;
    char *saveptr = NULL;
    char *line;
    int rc = -1;

    snprintf(path, sizeof(path), "%s/packed-refs", git_dir);
    content = read_small_file(path);
    if (content == NULL)
        return -1;

    for (line = strtok_r(content, "\n", &saveptr); line != NULL;
        line = strtok_r(NULL, "\n", &saveptr)) {
        char *space;

        if (line[0] == '#' || line[0] == '^')
            continue;
        space = strchr(line, ' ');
        if (space == NULL || (size_t)(space - line) != SG_SHA1_HEX_LEN)
            continue;
        if (strcmp(space + 1, ref_path) == 0) {
            char hex[SG_SHA1_HEX_LEN + 1];

            memcpy(hex, line, SG_SHA1_HEX_LEN);
            hex[SG_SHA1_HEX_LEN] = '\0';
            rc = sg_hex_to_sha1(hex, id_out);
            break;
        }
    }

    free(content);
    return rc;
}

int sg_ref_read_path(const char *git_dir, const char *ref_path, unsigned char id_out[SG_SHA1_RAW_LEN])
{
    char full_path[SG_PATH_MAX];
    unsigned char *content;
    size_t content_len;
    char hex[SG_SHA1_HEX_LEN + 1];
    int rc;

    if (!sg_ref_branch_name_is_safe(ref_path))
        return -1;

    snprintf(full_path, sizeof(full_path), "%s/%s", git_dir, ref_path);
    if (sg_read_file(full_path, &content, &content_len) != 0)
        return read_packed_ref(git_dir, ref_path, id_out); /* loose ref file absent: maybe packed by `git gc` */
    if (content_len < SG_SHA1_HEX_LEN) {
        free(content);
        return -1;
    }
    memcpy(hex, content, SG_SHA1_HEX_LEN);
    hex[SG_SHA1_HEX_LEN] = '\0';
    rc = sg_hex_to_sha1(hex, id_out);
    free(content);
    return rc;
}
