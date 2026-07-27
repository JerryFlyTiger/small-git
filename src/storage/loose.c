#include "sg/loose.h"

#include "sg/zutil.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SG_PATH_MAX 4096

static void object_paths(const char *git_dir, const char hex[SG_SHA1_HEX_LEN + 1],
                         char dir_path[SG_PATH_MAX], char file_path[SG_PATH_MAX])
{
    snprintf(dir_path, SG_PATH_MAX, "%s/objects/%.2s", git_dir, hex);
    snprintf(file_path, SG_PATH_MAX, "%s/objects/%.2s/%s", git_dir, hex, hex + 2);
}

/* retries on EINTR/short writes so a signal or large buffer can't leave a
   partially-written file */
static int write_all(int fd, const unsigned char *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

int sg_loose_write(const char *git_dir, sg_obj_type type, const void *content,
                   size_t content_len, unsigned char id_out[SG_SHA1_RAW_LEN])
{
    char hex[SG_SHA1_HEX_LEN + 1];
    char dir_path[SG_PATH_MAX];
    char file_path[SG_PATH_MAX];
    unsigned char *formatted;
    size_t formatted_len;
    unsigned char *compressed;
    size_t compressed_len;
    char tmp_path[SG_PATH_MAX];
    int fd;

    sg_object_hash(type, content, content_len, id_out);
    sg_sha1_to_hex(id_out, hex);
    object_paths(git_dir, hex, dir_path, file_path);

    /* content-addressed: an existing file is already correct, skip rewriting */
    if (access(file_path, F_OK) == 0)
        return 0;

    if (mkdir(dir_path, 0755) != 0 && errno != EEXIST)
        return -1;

    if (sg_object_format(type, content, content_len, &formatted, &formatted_len) != 0)
        return -1;

    if (sg_compress(formatted, formatted_len, &compressed, &compressed_len) != 0) {
        free(formatted);
        return -1;
    }
    free(formatted);

    /* write to a temp file and rename into place: rename() is atomic on the
       same filesystem, so file_path only ever exists fully-written or not
       at all -- a crash/short write/ENOSPC mid-write can never leave a
       corrupt object at the content-addressed path above */
    snprintf(tmp_path, SG_PATH_MAX, "%s/tmp_obj_XXXXXX", dir_path);
    fd = mkstemp(tmp_path);
    if (fd < 0) {
        free(compressed);
        return -1;
    }

    if (write_all(fd, compressed, compressed_len) != 0) {
        close(fd);
        unlink(tmp_path);
        free(compressed);
        return -1;
    }
    close(fd);
    free(compressed);

    if (chmod(tmp_path, 0444) != 0 || rename(tmp_path, file_path) != 0) {
        unlink(tmp_path);
        return -1;
    }

    return 0;
}

int sg_loose_read(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                  sg_obj_type *type_out, unsigned char **content_out, size_t *content_len_out)
{
    char hex[SG_SHA1_HEX_LEN + 1];
    char dir_path[SG_PATH_MAX];
    char file_path[SG_PATH_MAX];
    struct stat st;
    unsigned char *raw;
    size_t raw_len;
    unsigned char *decompressed;
    size_t decompressed_len;
    sg_object obj;
    FILE *f;

    sg_sha1_to_hex(id, hex);
    object_paths(git_dir, hex, dir_path, file_path);

    if (stat(file_path, &st) != 0)
        return -1;

    raw = malloc((size_t)st.st_size > 0 ? (size_t)st.st_size : 1);
    if (raw == NULL)
        return -1;

    f = fopen(file_path, "rb");
    if (f == NULL) {
        free(raw);
        return -1;
    }
    raw_len = fread(raw, 1, (size_t)st.st_size, f);
    fclose(f);
    if (raw_len != (size_t)st.st_size) {
        free(raw);
        return -1;
    }

    if (sg_decompress(raw, raw_len, &decompressed, &decompressed_len) != 0) {
        free(raw);
        return -1;
    }
    free(raw);

    if (sg_object_parse(decompressed, decompressed_len, &obj) != 0) {
        free(decompressed);
        return -1;
    }

    *content_out = malloc(obj.content_len > 0 ? obj.content_len : 1);
    if (*content_out == NULL) {
        free(decompressed);
        return -1;
    }
    memcpy(*content_out, obj.content, obj.content_len);
    *type_out = obj.type;
    *content_len_out = obj.content_len;

    free(decompressed);
    return 0;
}
