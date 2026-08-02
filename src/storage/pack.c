#include "sg/pack.h"

#include "sg/loose.h"
#include "sg/objstore.h"
#include "sg/zutil.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define SG_PATH_MAX 4096

/* git's own pack object type numbering -- 5 is intentionally unused */
#define SG_PACK_TYPE_COMMIT 1
#define SG_PACK_TYPE_TREE 2
#define SG_PACK_TYPE_BLOB 3
#define SG_PACK_TYPE_TAG 4
#define SG_PACK_TYPE_OFS_DELTA 6
#define SG_PACK_TYPE_REF_DELTA 7

/* Generous bound on delta chain length -- real packs never chain anywhere
   near this deep. Exists purely so a crafted/corrupt pack (e.g. a REF_DELTA
   cycle A->B->A, or thousands of sequentially-offset OFS_DELTA entries)
   can't force unbounded recursion and crash via stack overflow; it turns
   that into a clean decode error instead. */
#define SG_PACK_MAX_DELTA_DEPTH 300

static int read_entry_at(const char *git_dir, const unsigned char *pack_data, size_t pack_len,
                         size_t offset, int depth, sg_obj_type *type_out,
                         unsigned char **content_out, size_t *content_len_out);
static int pack_read_depth(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN], int depth,
                          sg_obj_type *type_out, unsigned char **content_out,
                          size_t *content_len_out);

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
          (uint32_t)p[3];
}

static void put_be32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

/* ---- variable-length encodings (see docs/pack format notes in the task spec) ---- */

/* Entry header: bit7 of each byte is a continuation flag. First byte carries
   3-bit type (bits 6-4) and the low 4 bits of size; each continuation byte
   contributes 7 more bits, least-significant chunk first. */
size_t sg_pack_decode_obj_header(const unsigned char *p, size_t avail, int *type_out,
                                 uint64_t *size_out)
{
    size_t i = 0;
    unsigned char b;
    uint64_t size;
    unsigned shift;

    if (avail < 1)
        return 0;
    b = p[i++];
    *type_out = (b >> 4) & 0x7;
    size = b & 0x0F;
    shift = 4;
    while (b & 0x80) {
        if (i >= avail)
            return 0;
        b = p[i++];
        /* shifting a uint64_t by >=64 is undefined behavior in C -- a
           crafted/corrupt header with enough continuation bytes could
           otherwise drive shift past 63. Real sizes never need this many
           bytes, so treat it as a decode error. */
        if (shift >= 64)
            return 0;
        size |= (uint64_t)(b & 0x7F) << shift;
        shift += 7;
    }
    *size_out = size;
    return i;
}

/* out must have room for the worst case (10 bytes covers a full 64-bit size). */
size_t sg_pack_encode_obj_header(int type, uint64_t size, unsigned char *out)
{
    size_t i = 0;
    unsigned char b = (unsigned char)(((type & 0x7) << 4) | (size & 0x0F));

    size >>= 4;
    if (size != 0)
        b |= 0x80;
    out[i++] = b;
    while (size != 0) {
        b = (unsigned char)(size & 0x7F);
        size >>= 7;
        if (size != 0)
            b |= 0x80;
        out[i++] = b;
    }
    return i;
}

/* OFS_DELTA's backwards-offset encoding: unlike the header/delta-size
   varints, each continuation byte implicitly adds 1 before shifting in --
   this lets every offset value have a unique encoding (see git's
   encode_in_pack_object_header / decode counterpart). */
size_t sg_pack_decode_ofs_delta_offset(const unsigned char *p, size_t avail, uint64_t *offset_out)
{
    size_t i = 0;
    unsigned char b;
    uint64_t offset;

    if (avail < 1)
        return 0;
    b = p[i++];
    offset = b & 0x7F;
    while (b & 0x80) {
        if (i >= avail)
            return 0;
        offset += 1;
        b = p[i++];
        offset = (offset << 7) | (b & 0x7F);
    }
    *offset_out = offset;
    return i;
}

/* Plain 7-bit-per-byte little-endian varint, used for the base/target size
   fields at the start of a delta instruction stream -- no +1 trick here. */
size_t sg_pack_decode_delta_size(const unsigned char *p, size_t avail, uint64_t *size_out)
{
    size_t i = 0;
    unsigned char b;
    uint64_t size = 0;
    unsigned shift = 0;

    do {
        if (i >= avail)
            return 0;
        b = p[i++];
        if (shift >= 64) /* same UB hazard as sg_pack_decode_obj_header above */
            return 0;
        size |= (uint64_t)(b & 0x7F) << shift;
        shift += 7;
    } while (b & 0x80);
    *size_out = size;
    return i;
}

/* Reconstructs a target object from a base object plus a decompressed delta
   instruction stream. *out is malloc'd, caller frees. Returns 0 on success,
   -1 on any malformed/out-of-range instruction. */
int sg_pack_delta_apply(const unsigned char *base, size_t base_len, const unsigned char *delta,
                        size_t delta_len, unsigned char **out, size_t *out_len)
{
    size_t pos = 0;
    uint64_t hdr_base_size, hdr_target_size;
    size_t n;
    unsigned char *result;
    size_t result_off = 0;

    n = sg_pack_decode_delta_size(delta + pos, delta_len - pos, &hdr_base_size);
    if (n == 0)
        return -1;
    pos += n;
    n = sg_pack_decode_delta_size(delta + pos, delta_len - pos, &hdr_target_size);
    if (n == 0)
        return -1;
    pos += n;

    if (hdr_base_size != (uint64_t)base_len)
        return -1;

    /* hdr_target_size is delta-stream-controlled. On a build where size_t is
       narrower than uint64_t, casting it below would truncate the allocation
       while the bounds checks further down keep using the full 64-bit value
       -- a heap overflow. Reject anything size_t can't represent. */
    if (hdr_target_size > (uint64_t)SIZE_MAX)
        return -1;

    result = malloc(hdr_target_size > 0 ? (size_t)hdr_target_size : 1);
    if (result == NULL)
        return -1;

    while (pos < delta_len) {
        unsigned char opcode = delta[pos++];

        if (opcode & 0x80) {
            uint64_t offset = 0, csize = 0;

            if (opcode & 0x01) {
                if (pos >= delta_len)
                    goto fail;
                offset |= delta[pos++];
            }
            if (opcode & 0x02) {
                if (pos >= delta_len)
                    goto fail;
                offset |= (uint64_t)delta[pos++] << 8;
            }
            if (opcode & 0x04) {
                if (pos >= delta_len)
                    goto fail;
                offset |= (uint64_t)delta[pos++] << 16;
            }
            if (opcode & 0x08) {
                if (pos >= delta_len)
                    goto fail;
                offset |= (uint64_t)delta[pos++] << 24;
            }
            if (opcode & 0x10) {
                if (pos >= delta_len)
                    goto fail;
                csize |= delta[pos++];
            }
            if (opcode & 0x20) {
                if (pos >= delta_len)
                    goto fail;
                csize |= (uint64_t)delta[pos++] << 8;
            }
            if (opcode & 0x40) {
                if (pos >= delta_len)
                    goto fail;
                csize |= (uint64_t)delta[pos++] << 16;
            }
            if (csize == 0)
                csize = 0x10000; /* the three size bytes being absent means 65536, not 0 */

            if (offset > (uint64_t)base_len || csize > (uint64_t)base_len - offset)
                goto fail;
            if (csize > hdr_target_size - result_off)
                goto fail;
            memcpy(result + result_off, base + offset, (size_t)csize);
            result_off += (size_t)csize;
        } else if (opcode != 0) {
            size_t clen = opcode;

            if (pos + clen > delta_len)
                goto fail;
            if (clen > hdr_target_size - result_off)
                goto fail;
            memcpy(result + result_off, delta + pos, clen);
            pos += clen;
            result_off += clen;
        } else {
            goto fail; /* opcode 0x00 is reserved */
        }
    }

    if (result_off != hdr_target_size)
        goto fail;

    *out = result;
    *out_len = result_off;
    return 0;

fail:
    free(result);
    return -1;
}

/* ---- zlib inflate of a single pack entry, reporting exact bytes consumed ----
   sg_decompress (sg/zutil.h) grows its output buffer dynamically because it
   doesn't know the decompressed size in advance; here we always know it
   exactly (it's the entry header's size field, for both direct objects and
   delta streams), so we can decompress straight into a fixed buffer and read
   off strm.avail_in afterwards to learn how many pack bytes the entry's
   compressed data actually occupied -- needed to locate the next entry and
   to compute the idx CRC32 span. */
static int pack_inflate(const unsigned char *src, size_t src_avail, size_t expected_len,
                        unsigned char *out_buf, size_t *consumed_out)
{
    z_stream strm;
    int zret;

    memset(&strm, 0, sizeof(strm));
    if (inflateInit(&strm) != Z_OK)
        return -1;

    strm.next_in = (unsigned char *)src;
    strm.avail_in = (uInt)src_avail;
    strm.next_out = out_buf;
    strm.avail_out = (uInt)expected_len;

    do {
        zret = inflate(&strm, Z_FINISH);
    } while (zret == Z_OK && strm.avail_out > 0);

    if (zret != Z_STREAM_END || strm.avail_out != 0) {
        inflateEnd(&strm);
        return -1;
    }

    *consumed_out = src_avail - strm.avail_in;
    inflateEnd(&strm);
    return 0;
}

static int pack_type_to_obj_type(int raw_type, sg_obj_type *out)
{
    switch (raw_type) {
    case SG_PACK_TYPE_COMMIT:
        *out = SG_OBJ_COMMIT;
        return 0;
    case SG_PACK_TYPE_TREE:
        *out = SG_OBJ_TREE;
        return 0;
    case SG_PACK_TYPE_BLOB:
        *out = SG_OBJ_BLOB;
        return 0;
    case SG_PACK_TYPE_TAG:
        *out = SG_OBJ_TAG;
        return 0;
    default:
        return -1;
    }
}

static int obj_type_to_pack_type(sg_obj_type type)
{
    switch (type) {
    case SG_OBJ_COMMIT:
        return SG_PACK_TYPE_COMMIT;
    case SG_OBJ_TREE:
        return SG_PACK_TYPE_TREE;
    case SG_OBJ_BLOB:
        return SG_PACK_TYPE_BLOB;
    case SG_OBJ_TAG:
        return SG_PACK_TYPE_TAG;
    default:
        return -1;
    }
}

/* base objects for OFS_DELTA are always within this same pack (the format's
   offset is relative to this entry's own start), so this recurses on
   pack_data/pack_len directly; REF_DELTA bases are looked up by id (loose
   first, then other packs via pack_read_depth), which may land in a
   different pack or in loose storage. `depth` is threaded through both
   paths and bounded by SG_PACK_MAX_DELTA_DEPTH -- see that macro's comment
   for why. */
static int read_entry_at(const char *git_dir, const unsigned char *pack_data, size_t pack_len,
                         size_t offset, int depth, sg_obj_type *type_out,
                         unsigned char **content_out, size_t *content_len_out)
{
    int raw_type;
    uint64_t size;
    size_t pos = offset;
    size_t n;

    if (depth > SG_PACK_MAX_DELTA_DEPTH)
        return -1;
    if (offset >= pack_len)
        return -1;
    n = sg_pack_decode_obj_header(pack_data + pos, pack_len - pos, &raw_type, &size);
    if (n == 0)
        return -1;
    pos += n;

    if (raw_type == SG_PACK_TYPE_OFS_DELTA || raw_type == SG_PACK_TYPE_REF_DELTA) {
        unsigned char base_id[SG_SHA1_RAW_LEN];
        size_t base_offset = 0;
        unsigned char *delta_data;
        size_t consumed;
        sg_obj_type base_type;
        unsigned char *base_content;
        size_t base_len;
        int rc;

        if (raw_type == SG_PACK_TYPE_OFS_DELTA) {
            uint64_t rel;

            n = sg_pack_decode_ofs_delta_offset(pack_data + pos, pack_len - pos, &rel);
            if (n == 0 || rel == 0 || rel > offset)
                return -1;
            pos += n;
            base_offset = offset - (size_t)rel;
        } else {
            if (pack_len - pos < SG_SHA1_RAW_LEN)
                return -1;
            memcpy(base_id, pack_data + pos, SG_SHA1_RAW_LEN);
            pos += SG_SHA1_RAW_LEN;
        }

        delta_data = malloc(size > 0 ? (size_t)size : 1);
        if (delta_data == NULL)
            return -1;
        if (pack_inflate(pack_data + pos, pack_len - pos, (size_t)size, delta_data, &consumed) !=
           0) {
            free(delta_data);
            return -1;
        }

        if (raw_type == SG_PACK_TYPE_OFS_DELTA)
            rc = read_entry_at(git_dir, pack_data, pack_len, base_offset, depth + 1, &base_type,
                               &base_content, &base_len);
        else
            /* Loose objects are never deltas, so they terminate the chain
               immediately without consuming depth; only a pack-resident base
               continues the recursion, and it must keep threading `depth`
               through rather than re-entering via the public sg_object_read
               (which has no depth parameter and would reset tracking,
               defeating the whole guard). */
            rc = sg_loose_read(git_dir, base_id, &base_type, &base_content, &base_len) == 0
                     ? 0
                     : pack_read_depth(git_dir, base_id, depth + 1, &base_type, &base_content,
                                       &base_len);

        if (rc != 0) {
            free(delta_data);
            return -1;
        }

        rc = sg_pack_delta_apply(base_content, base_len, delta_data, (size_t)size, content_out,
                                 content_len_out);
        free(delta_data);
        free(base_content);
        if (rc != 0)
            return -1;

        *type_out = base_type;
        return 0;
    }

    {
        sg_obj_type mapped;
        unsigned char *content;
        size_t consumed;

        if (pack_type_to_obj_type(raw_type, &mapped) != 0)
            return -1;

        content = malloc(size > 0 ? (size_t)size : 1);
        if (content == NULL)
            return -1;
        if (pack_inflate(pack_data + pos, pack_len - pos, (size_t)size, content, &consumed) != 0) {
            free(content);
            return -1;
        }

        *type_out = mapped;
        *content_out = content;
        *content_len_out = (size_t)size;
        return 0;
    }
}

/* ---- .idx (version 2) reading ---- */

typedef struct {
    unsigned char *raw;
    size_t raw_len;
    size_t count;
    const unsigned char *sha1_table;   /* count * 20 bytes, sorted */
    const unsigned char *offset_table; /* count * 4 bytes, big-endian */
} sg_idx;

static int idx_load(const char *idx_path, sg_idx *out)
{
    FILE *f;
    struct stat st;
    unsigned char *buf;
    size_t len;
    size_t fanout_off, sha1_off, crc_off, offset_off;
    uint32_t count;

    f = fopen(idx_path, "rb");
    if (f == NULL)
        return -1;
    if (fstat(fileno(f), &st) != 0) {
        fclose(f);
        return -1;
    }
    len = (size_t)st.st_size;
    buf = malloc(len > 0 ? len : 1);
    if (buf == NULL) {
        fclose(f);
        return -1;
    }
    if (fread(buf, 1, len, f) != len) {
        fclose(f);
        free(buf);
        return -1;
    }
    fclose(f);

    if (len < 8 + 256 * 4 + 40 || buf[0] != 0xff || buf[1] != 0x74 || buf[2] != 0x4f ||
       buf[3] != 0x63 || be32(buf + 4) != 2) {
        free(buf);
        return -1;
    }

    fanout_off = 8;
    count = be32(buf + fanout_off + 255 * 4);
    sha1_off = fanout_off + 256 * 4;
    crc_off = sha1_off + (size_t)count * SG_SHA1_RAW_LEN;
    offset_off = crc_off + (size_t)count * 4;

    if (offset_off + (size_t)count * 4 + 2 * SG_SHA1_RAW_LEN != len) {
        free(buf);
        return -1;
    }

    out->raw = buf;
    out->raw_len = len;
    out->count = count;
    out->sha1_table = buf + sha1_off;
    out->offset_table = buf + offset_off;
    return 0;
}

static void idx_free(sg_idx *idx)
{
    free(idx->raw);
}

/* binary search over the sorted sha1 table; the fanout table exists in the
   format for git's own two-level lookup, but a plain binary search over the
   already-sorted table below is just as correct, so it's parsed above only
   to locate `count` and validate the file's shape, not used to narrow the
   search range here. */
static int idx_find(const sg_idx *idx, const unsigned char id[SG_SHA1_RAW_LEN], size_t *offset_out)
{
    size_t lo = 0, hi = idx->count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = memcmp(idx->sha1_table + mid * SG_SHA1_RAW_LEN, id, SG_SHA1_RAW_LEN);

        if (cmp == 0) {
            uint32_t off = be32(idx->offset_table + mid * 4);

            if (off & 0x80000000u)
                return -1; /* 8-byte large-offset table entries aren't supported */
            *offset_out = off;
            return 0;
        }
        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 1;
}

static int pack_load(const char *pack_path, unsigned char **data_out, size_t *len_out)
{
    FILE *f;
    struct stat st;
    unsigned char *buf;

    f = fopen(pack_path, "rb");
    if (f == NULL)
        return -1;
    if (fstat(fileno(f), &st) != 0) {
        fclose(f);
        return -1;
    }
    *len_out = (size_t)st.st_size;
    buf = malloc(*len_out > 0 ? *len_out : 1);
    if (buf == NULL) {
        fclose(f);
        return -1;
    }
    if (fread(buf, 1, *len_out, f) != *len_out) {
        fclose(f);
        free(buf);
        return -1;
    }
    fclose(f);
    *data_out = buf;
    return 0;
}

static int pack_read_depth(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN], int depth,
                          sg_obj_type *type_out, unsigned char **content_out,
                          size_t *content_len_out)
{
    char pack_dir[SG_PATH_MAX];
    DIR *d;
    struct dirent *de;
    int found = -1;

    snprintf(pack_dir, sizeof(pack_dir), "%s/objects/pack", git_dir);
    d = opendir(pack_dir);
    if (d == NULL)
        return -1;

    while ((de = readdir(d)) != NULL) {
        size_t name_len = strlen(de->d_name);
        char idx_path[SG_PATH_MAX];
        char pack_path[SG_PATH_MAX];
        sg_idx idx;
        size_t offset;
        unsigned char *pack_data;
        size_t pack_len;
        struct stat st;

        if (name_len < 4 || strcmp(de->d_name + name_len - 4, ".idx") != 0)
            continue;

        snprintf(idx_path, sizeof(idx_path), "%s/%s", pack_dir, de->d_name);
        snprintf(pack_path, sizeof(pack_path), "%s/%.*s.pack", pack_dir, (int)(name_len - 4),
                de->d_name);

        if (stat(pack_path, &st) != 0)
            continue;

        if (idx_load(idx_path, &idx) != 0)
            continue;

        if (idx_find(&idx, id, &offset) != 0) {
            idx_free(&idx);
            continue;
        }
        idx_free(&idx);

        if (pack_load(pack_path, &pack_data, &pack_len) != 0)
            continue;

        if (read_entry_at(git_dir, pack_data, pack_len, offset, depth, type_out, content_out,
                          content_len_out) == 0) {
            free(pack_data);
            found = 0;
            break;
        }
        free(pack_data);
    }

    closedir(d);
    return found;
}

int sg_pack_read(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                 sg_obj_type *type_out, unsigned char **content_out, size_t *content_len_out)
{
    return pack_read_depth(git_dir, id, 0, type_out, content_out, content_len_out);
}

/* ---- writing (no delta compression -- every object stored literally) ---- */

typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} byte_buf;

static int buf_append(byte_buf *b, const void *src, size_t n)
{
    if (b->len + n > b->cap) {
        size_t new_cap = b->cap == 0 ? 4096 : b->cap * 2;
        unsigned char *grown;

        while (new_cap < b->len + n)
            new_cap *= 2;
        grown = realloc(b->data, new_cap);
        if (grown == NULL)
            return -1;
        b->data = grown;
        b->cap = new_cap;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

typedef struct {
    unsigned char id[SG_SHA1_RAW_LEN];
    uint32_t crc;
    size_t offset;
} pack_entry_meta;

static int cmp_entry_by_id(const void *a, const void *b)
{
    const pack_entry_meta *ea = a;
    const pack_entry_meta *eb = b;

    return memcmp(ea->id, eb->id, SG_SHA1_RAW_LEN);
}

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

static int write_atomic(const char *dir, const char *final_path, const unsigned char *data,
                        size_t len)
{
    char tmp_path[SG_PATH_MAX];
    int fd;

    snprintf(tmp_path, sizeof(tmp_path), "%s/tmp_pack_XXXXXX", dir);
    fd = mkstemp(tmp_path);
    if (fd < 0)
        return -1;

    if (write_all(fd, data, len) != 0) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    close(fd);

    if (chmod(tmp_path, 0444) != 0 || rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

/* Shared by sg_pack_write and sg_pack_index_existing: sorts entries by id,
   builds the fanout / sorted-sha1 / crc32 / offset tables plus trailer, and
   atomically writes the result to idx_path (same version-2 .idx format
   either caller produces). Returns 0 on success, -1 on failure. */
static int write_idx_for_pack(const char *pack_dir, const char *idx_path,
                              pack_entry_meta *entries, size_t count,
                              const unsigned char pack_sha1[SG_SHA1_RAW_LEN])
{
    byte_buf idx_buf;
    unsigned char idx_sha1[SG_SHA1_RAW_LEN];
    uint32_t counts[256];
    uint32_t fanout[256];
    size_t i;
    int ok = 0;

    memset(&idx_buf, 0, sizeof(idx_buf));

    qsort(entries, count, sizeof(*entries), cmp_entry_by_id);

    memset(counts, 0, sizeof(counts));
    for (i = 0; i < count; i++)
        counts[entries[i].id[0]]++;
    {
        uint32_t running = 0;

        for (i = 0; i < 256; i++) {
            running += counts[i];
            fanout[i] = running;
        }
    }

    {
        unsigned char idx_header[8] = {0xff, 0x74, 0x4f, 0x63, 0, 0, 0, 2};

        if (buf_append(&idx_buf, idx_header, sizeof(idx_header)) != 0)
            goto done;
    }
    for (i = 0; i < 256; i++) {
        unsigned char be[4];

        put_be32(be, fanout[i]);
        if (buf_append(&idx_buf, be, 4) != 0)
            goto done;
    }
    for (i = 0; i < count; i++) {
        if (buf_append(&idx_buf, entries[i].id, SG_SHA1_RAW_LEN) != 0)
            goto done;
    }
    for (i = 0; i < count; i++) {
        unsigned char be[4];

        put_be32(be, entries[i].crc);
        if (buf_append(&idx_buf, be, 4) != 0)
            goto done;
    }
    for (i = 0; i < count; i++) {
        unsigned char be[4];

        put_be32(be, (uint32_t)entries[i].offset);
        if (buf_append(&idx_buf, be, 4) != 0)
            goto done;
    }
    if (buf_append(&idx_buf, pack_sha1, SG_SHA1_RAW_LEN) != 0)
        goto done;

    sg_sha1(idx_buf.data, idx_buf.len, idx_sha1);
    if (buf_append(&idx_buf, idx_sha1, SG_SHA1_RAW_LEN) != 0)
        goto done;

    if (write_atomic(pack_dir, idx_path, idx_buf.data, idx_buf.len) != 0)
        goto done;

    ok = 1;

done:
    free(idx_buf.data);
    return ok ? 0 : -1;
}

int sg_pack_write(const char *git_dir, const unsigned char (*ids)[SG_SHA1_RAW_LEN], size_t count)
{
    byte_buf pack_buf;
    pack_entry_meta *entries;
    unsigned char header[12];
    unsigned char pack_sha1[SG_SHA1_RAW_LEN];
    char pack_hex[SG_SHA1_HEX_LEN + 1];
    char pack_dir[SG_PATH_MAX];
    char pack_path[SG_PATH_MAX];
    char idx_path[SG_PATH_MAX];
    size_t i;
    int ok = 0;

    memset(&pack_buf, 0, sizeof(pack_buf));

    entries = malloc(count > 0 ? count * sizeof(*entries) : 1);
    if (entries == NULL)
        return -1;

    memcpy(header, "PACK", 4);
    put_be32(header + 4, 2);
    put_be32(header + 8, (uint32_t)count);
    if (buf_append(&pack_buf, header, sizeof(header)) != 0)
        goto done;

    for (i = 0; i < count; i++) {
        sg_obj_type type;
        unsigned char *content;
        size_t content_len;
        unsigned char *compressed;
        size_t compressed_len;
        unsigned char hdr[16];
        size_t hdr_len;
        int raw_type;
        size_t entry_offset = pack_buf.len;
        uLong crc;

        if (sg_object_read(git_dir, ids[i], &type, &content, &content_len) != 0)
            goto done;

        raw_type = obj_type_to_pack_type(type);
        if (raw_type < 0) {
            free(content);
            goto done;
        }
        hdr_len = sg_pack_encode_obj_header(raw_type, (uint64_t)content_len, hdr);

        if (sg_compress(content, content_len, &compressed, &compressed_len) != 0) {
            free(content);
            goto done;
        }
        free(content);

        if (entry_offset > 0x7fffffffULL) {
            /* would need the 8-byte large-offset .idx table; not supported */
            free(compressed);
            goto done;
        }

        if (buf_append(&pack_buf, hdr, hdr_len) != 0 ||
           buf_append(&pack_buf, compressed, compressed_len) != 0) {
            free(compressed);
            goto done;
        }
        free(compressed);

        crc = crc32(0L, Z_NULL, 0);
        crc = crc32(crc, pack_buf.data + entry_offset, (uInt)(pack_buf.len - entry_offset));

        memcpy(entries[i].id, ids[i], SG_SHA1_RAW_LEN);
        entries[i].crc = (uint32_t)crc;
        entries[i].offset = entry_offset;
    }

    sg_sha1(pack_buf.data, pack_buf.len, pack_sha1);
    if (buf_append(&pack_buf, pack_sha1, SG_SHA1_RAW_LEN) != 0)
        goto done;
    sg_sha1_to_hex(pack_sha1, pack_hex);

    snprintf(pack_dir, sizeof(pack_dir), "%s/objects/pack", git_dir);
    if (mkdir(pack_dir, 0755) != 0 && errno != EEXIST)
        goto done;

    snprintf(pack_path, sizeof(pack_path), "%s/pack-%s.pack", pack_dir, pack_hex);
    snprintf(idx_path, sizeof(idx_path), "%s/pack-%s.idx", pack_dir, pack_hex);

    if (write_atomic(pack_dir, pack_path, pack_buf.data, pack_buf.len) != 0)
        goto done;
    if (write_idx_for_pack(pack_dir, idx_path, entries, count, pack_sha1) != 0)
        goto done;

    ok = 1;

done:
    free(entries);
    free(pack_buf.data);
    return ok ? 0 : -1;
}

/* ---- indexing an already-on-disk pack received over the network ---- */

/* Per-entry bookkeeping built by the metadata pass (pass 1) below and
   consumed by the resolve pass (pass 2). data_offset/decompressed_size let
   pass 2 re-inflate the entry's compressed bytes on demand without having
   kept them decompressed in memory since pass 1; crc is already final
   (computed in pass 1, since it only needs the raw compressed byte range,
   not the decompressed content). */
typedef struct {
    size_t start_offset;
    size_t data_offset;
    size_t decompressed_size;
    int raw_type;
    size_t base_offset;                    /* valid if raw_type == OFS_DELTA */
    unsigned char base_id[SG_SHA1_RAW_LEN]; /* valid if raw_type == REF_DELTA */
    uint32_t crc;
} idx_build_entry;

typedef struct {
    const char *git_dir;
    const unsigned char *pack_data;
    size_t pack_len;
    idx_build_entry *metas;
    size_t count;
    /* pass-2 resolution state, indexed in parallel with metas; content[i] ==
       NULL means "not yet resolved". Every resolved entry's full content is
       kept until the whole pass finishes (rather than freed the instant
       nothing *already processed* still needs it) -- an intentionally simple
       choice given a REF_DELTA's base object id can't be matched against an
       offset until that base is itself decoded, so knowing exactly when a
       given entry is safe to free would need a separate dependency-closure
       pre-pass; seen packs (single fetch's worth of objects) are expected to
       comfortably fit in memory for this phase. */
    sg_obj_type *type;
    unsigned char **content;
    size_t *content_len;
    unsigned char (*id)[SG_SHA1_RAW_LEN];
    /* id -> entry index, bucketed on the id's first byte and filled in as
       entries resolve. Only resolved entries are ever inserted, so lookups
       keep the "an unresolved entry can't serve as a base" property that
       breaks REF_DELTA cycles -- this is purely a faster way to ask the same
       question than scanning all `count` entries per REF_DELTA, which a
       hostile pack full of REF_DELTAs could otherwise drive to O(n^2). */
    size_t **bucket;
    size_t *bucket_len;
    size_t *bucket_cap;
} idx_build_ctx;

#define SG_IDX_BUILD_BUCKETS 256

static int idx_build_bucket_add(idx_build_ctx *ctx, size_t entry_idx)
{
    unsigned char b = ctx->id[entry_idx][0];

    if (ctx->bucket_len[b] == ctx->bucket_cap[b]) {
        size_t new_cap = ctx->bucket_cap[b] == 0 ? 8 : ctx->bucket_cap[b] * 2;
        size_t *grown = realloc(ctx->bucket[b], new_cap * sizeof(*grown));

        if (grown == NULL)
            return -1;
        ctx->bucket[b] = grown;
        ctx->bucket_cap[b] = new_cap;
    }
    ctx->bucket[b][ctx->bucket_len[b]++] = entry_idx;
    return 0;
}

/* entries are laid out in strictly increasing start_offset order (pass 1
   walks the pack linearly), so this is a binary search. */
static int idx_build_find_by_offset(const idx_build_ctx *ctx, size_t offset, size_t *idx_out)
{
    size_t lo = 0, hi = ctx->count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;

        if (ctx->metas[mid].start_offset == offset) {
            *idx_out = mid;
            return 0;
        }
        if (ctx->metas[mid].start_offset < offset)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

/* Only entries already resolved (content != NULL) have a valid id -- by the
   time this is called for entry k, every entry with index < k has already
   been fully resolved (the pass-2 driving loop processes indices in order
   and blocks on each), which is exactly what a REF_DELTA base is guaranteed
   to satisfy for a non-thin pack (its base is always written earlier). */
static int idx_build_find_by_id(const idx_build_ctx *ctx, const unsigned char id[SG_SHA1_RAW_LEN],
                                size_t *idx_out)
{
    unsigned char b = id[0];
    size_t k;

    for (k = 0; k < ctx->bucket_len[b]; k++) {
        size_t i = ctx->bucket[b][k];

        if (ctx->content[i] != NULL && memcmp(ctx->id[i], id, SG_SHA1_RAW_LEN) == 0) {
            *idx_out = i;
            return 0;
        }
    }
    return -1;
}

static int idx_build_resolve(idx_build_ctx *ctx, size_t idx, int depth)
{
    idx_build_entry *m;

    if (idx >= ctx->count)
        return -1;
    if (ctx->content[idx] != NULL)
        return 0;
    if (depth > SG_PACK_MAX_DELTA_DEPTH)
        return -1;
    m = &ctx->metas[idx];

    if (m->raw_type == SG_PACK_TYPE_OFS_DELTA || m->raw_type == SG_PACK_TYPE_REF_DELTA) {
        unsigned char *base_content;
        size_t base_len;
        sg_obj_type base_type;
        int base_is_external = 0;
        unsigned char *delta_data;
        size_t consumed;
        int rc;

        if (m->raw_type == SG_PACK_TYPE_OFS_DELTA) {
            size_t base_idx;

            if (idx_build_find_by_offset(ctx, m->base_offset, &base_idx) != 0)
                return -1;
            if (idx_build_resolve(ctx, base_idx, depth + 1) != 0)
                return -1;
            base_content = ctx->content[base_idx];
            base_len = ctx->content_len[base_idx];
            base_type = ctx->type[base_idx];
        } else {
            size_t base_idx;

            if (idx_build_find_by_id(ctx, m->base_id, &base_idx) == 0) {
                if (idx_build_resolve(ctx, base_idx, depth + 1) != 0)
                    return -1;
                base_content = ctx->content[base_idx];
                base_len = ctx->content_len[base_idx];
                base_type = ctx->type[base_idx];
            } else if (sg_object_read(ctx->git_dir, m->base_id, &base_type, &base_content,
                                      &base_len) == 0) {
                /* not in this (thin-less) pack -- shouldn't normally happen,
                   but fall back to local storage rather than fail outright */
                base_is_external = 1;
            } else {
                return -1;
            }
        }

        delta_data = malloc(m->decompressed_size > 0 ? m->decompressed_size : 1);
        if (delta_data == NULL) {
            if (base_is_external)
                free(base_content);
            return -1;
        }
        if (pack_inflate(ctx->pack_data + m->data_offset, ctx->pack_len - m->data_offset,
                         m->decompressed_size, delta_data, &consumed) != 0) {
            free(delta_data);
            if (base_is_external)
                free(base_content);
            return -1;
        }

        rc = sg_pack_delta_apply(base_content, base_len, delta_data, m->decompressed_size,
                                 &ctx->content[idx], &ctx->content_len[idx]);
        free(delta_data);
        if (base_is_external)
            free(base_content);
        if (rc != 0)
            return -1;
        ctx->type[idx] = base_type;
    } else {
        sg_obj_type t;
        unsigned char *content;
        size_t consumed;

        if (pack_type_to_obj_type(m->raw_type, &t) != 0)
            return -1;
        content = malloc(m->decompressed_size > 0 ? m->decompressed_size : 1);
        if (content == NULL)
            return -1;
        if (pack_inflate(ctx->pack_data + m->data_offset, ctx->pack_len - m->data_offset,
                         m->decompressed_size, content, &consumed) != 0) {
            free(content);
            return -1;
        }
        ctx->content[idx] = content;
        ctx->content_len[idx] = m->decompressed_size;
        ctx->type[idx] = t;
    }

    sg_object_hash(ctx->type[idx], ctx->content[idx], ctx->content_len[idx], ctx->id[idx]);
    return idx_build_bucket_add(ctx, idx);
}

/* Finds the last "/objects/pack/" component of pack_path (there should only
   ever be one, but scanning for the last occurrence is cheap insurance
   against a pathological git_dir path that itself contains that substring)
   and returns everything before it. */
static int git_dir_from_pack_path(const char *pack_path, char *out, size_t out_size)
{
    static const char marker[] = "/objects/pack/";
    const char *search = pack_path;
    const char *last = NULL;
    size_t len;

    while ((search = strstr(search, marker)) != NULL) {
        last = search;
        search++;
    }
    if (last == NULL)
        return -1;

    len = (size_t)(last - pack_path);
    if (len + 1 > out_size)
        return -1;
    memcpy(out, pack_path, len);
    out[len] = '\0';
    return 0;
}

static int idx_path_from_pack_path(const char *pack_path, char *out, size_t out_size)
{
    size_t len = strlen(pack_path);

    if (len < 5 || strcmp(pack_path + len - 5, ".pack") != 0)
        return -1;
    if (len - 5 + 4 + 1 > out_size)
        return -1;
    memcpy(out, pack_path, len - 5);
    memcpy(out + (len - 5), ".idx", 5); /* includes the NUL */
    return 0;
}

int sg_pack_index_existing(const char *pack_path)
{
    char git_dir[SG_PATH_MAX];
    char pack_dir[SG_PATH_MAX];
    char idx_path[SG_PATH_MAX];
    unsigned char *pack_data = NULL;
    size_t pack_len = 0;
    uint32_t count;
    size_t pos;
    size_t i;
    unsigned char computed_trailer[SG_SHA1_RAW_LEN];
    idx_build_entry *metas = NULL;
    sg_obj_type *types = NULL;
    unsigned char **contents = NULL;
    size_t *content_lens = NULL;
    unsigned char (*ids)[SG_SHA1_RAW_LEN] = NULL;
    size_t **buckets = NULL;
    size_t *bucket_lens = NULL;
    size_t *bucket_caps = NULL;
    pack_entry_meta *out_entries = NULL;
    idx_build_ctx ctx;
    int ok = 0;

    if (git_dir_from_pack_path(pack_path, git_dir, sizeof(git_dir)) != 0) {
        fprintf(stderr, "sg: pack path is not under .../objects/pack/: %s\n", pack_path);
        return -1;
    }
    snprintf(pack_dir, sizeof(pack_dir), "%s/objects/pack", git_dir);
    if (idx_path_from_pack_path(pack_path, idx_path, sizeof(idx_path)) != 0) {
        fprintf(stderr, "sg: not a .pack path: %s\n", pack_path);
        return -1;
    }

    if (pack_load(pack_path, &pack_data, &pack_len) != 0) {
        fprintf(stderr, "sg: failed to read %s\n", pack_path);
        return -1;
    }

    if (pack_len < 12 + SG_SHA1_RAW_LEN || memcmp(pack_data, "PACK", 4) != 0 ||
       be32(pack_data + 4) != 2) {
        fprintf(stderr, "sg: %s is not a valid version-2 pack\n", pack_path);
        goto done;
    }
    sg_sha1(pack_data, pack_len - SG_SHA1_RAW_LEN, computed_trailer);
    if (memcmp(computed_trailer, pack_data + pack_len - SG_SHA1_RAW_LEN, SG_SHA1_RAW_LEN) != 0) {
        fprintf(stderr, "sg: %s fails trailer checksum verification\n", pack_path);
        goto done;
    }

    count = be32(pack_data + 8);
    /* The count is attacker-controlled. Every entry needs at least a header
       byte plus a zlib stream, so a count exceeding the bytes actually
       available is a lie -- reject it before sizing any allocation off it. */
    if ((uint64_t)count > (uint64_t)(pack_len - 12 - SG_SHA1_RAW_LEN)) {
        fprintf(stderr, "sg: %s declares %u objects, more than its size allows\n", pack_path,
               count);
        goto done;
    }
    metas = malloc(count > 0 ? count * sizeof(*metas) : 1);
    if (metas == NULL)
        goto done;

    /* ---- pass 1: per-entry metadata (type, delta base ref, crc) only ---- */
    pos = 12;
    for (i = 0; i < count; i++) {
        size_t start = pos;
        int raw_type;
        uint64_t size;
        size_t n;
        size_t data_offset;
        unsigned char *scratch;
        size_t consumed;
        uLong crc;

        if (start > 0x7fffffffULL) {
            /* would need the 8-byte large-offset .idx table; not supported */
            fprintf(stderr, "sg: %s is too large (needs large-offset .idx, unsupported)\n",
                   pack_path);
            goto done;
        }

        n = sg_pack_decode_obj_header(pack_data + pos, pack_len - pos, &raw_type, &size);
        if (n == 0) {
            fprintf(stderr, "sg: %s: malformed entry header at offset %zu\n", pack_path, start);
            goto done;
        }
        pos += n;

        metas[i].base_offset = 0;
        if (raw_type == SG_PACK_TYPE_OFS_DELTA) {
            uint64_t rel;

            n = sg_pack_decode_ofs_delta_offset(pack_data + pos, pack_len - pos, &rel);
            if (n == 0 || rel == 0 || rel > start) {
                fprintf(stderr, "sg: %s: malformed OFS_DELTA offset at %zu\n", pack_path, start);
                goto done;
            }
            pos += n;
            metas[i].base_offset = start - (size_t)rel;
        } else if (raw_type == SG_PACK_TYPE_REF_DELTA) {
            if (pack_len - pos < SG_SHA1_RAW_LEN) {
                fprintf(stderr, "sg: %s: truncated REF_DELTA base id at %zu\n", pack_path, start);
                goto done;
            }
            memcpy(metas[i].base_id, pack_data + pos, SG_SHA1_RAW_LEN);
            pos += SG_SHA1_RAW_LEN;
        } else if (raw_type < SG_PACK_TYPE_COMMIT || raw_type > SG_PACK_TYPE_TAG) {
            fprintf(stderr, "sg: %s: unrecognized object type %d at %zu\n", pack_path, raw_type,
                   start);
            goto done;
        }

        data_offset = pos;
        scratch = malloc(size > 0 ? (size_t)size : 1);
        if (scratch == NULL)
            goto done;
        if (pack_inflate(pack_data + data_offset, pack_len - data_offset, (size_t)size, scratch,
                         &consumed) != 0) {
            free(scratch);
            fprintf(stderr, "sg: %s: zlib inflate failed for entry at %zu\n", pack_path, start);
            goto done;
        }
        free(scratch);
        pos = data_offset + consumed;

        crc = crc32(0L, Z_NULL, 0);
        crc = crc32(crc, pack_data + start, (uInt)(pos - start));

        metas[i].start_offset = start;
        metas[i].data_offset = data_offset;
        metas[i].decompressed_size = (size_t)size;
        metas[i].raw_type = raw_type;
        metas[i].crc = (uint32_t)crc;
    }

    if (pos != pack_len - SG_SHA1_RAW_LEN) {
        fprintf(stderr, "sg: %s: trailing garbage after the last object entry\n", pack_path);
        goto done;
    }

    /* ---- pass 2: resolve each entry's full content just long enough to
       hash it, recursing into delta bases (which may themselves still need
       resolving) as needed ---- */
    types = malloc(count > 0 ? count * sizeof(*types) : 1);
    contents = calloc(count > 0 ? count : 1, sizeof(*contents));
    content_lens = malloc(count > 0 ? count * sizeof(*content_lens) : 1);
    ids = malloc(count > 0 ? count * sizeof(*ids) : 1);
    buckets = calloc(SG_IDX_BUILD_BUCKETS, sizeof(*buckets));
    bucket_lens = calloc(SG_IDX_BUILD_BUCKETS, sizeof(*bucket_lens));
    bucket_caps = calloc(SG_IDX_BUILD_BUCKETS, sizeof(*bucket_caps));
    if (types == NULL || contents == NULL || content_lens == NULL || ids == NULL ||
       buckets == NULL || bucket_lens == NULL || bucket_caps == NULL)
        goto done;

    ctx.git_dir = git_dir;
    ctx.pack_data = pack_data;
    ctx.pack_len = pack_len;
    ctx.metas = metas;
    ctx.count = count;
    ctx.type = types;
    ctx.content = contents;
    ctx.content_len = content_lens;
    ctx.id = ids;
    ctx.bucket = buckets;
    ctx.bucket_len = bucket_lens;
    ctx.bucket_cap = bucket_caps;

    for (i = 0; i < count; i++) {
        if (idx_build_resolve(&ctx, i, 0) != 0) {
            fprintf(stderr, "sg: %s: failed to resolve object at offset %zu\n", pack_path,
                   metas[i].start_offset);
            goto done;
        }
    }

    out_entries = malloc(count > 0 ? count * sizeof(*out_entries) : 1);
    if (out_entries == NULL)
        goto done;
    for (i = 0; i < count; i++) {
        memcpy(out_entries[i].id, ids[i], SG_SHA1_RAW_LEN);
        out_entries[i].crc = metas[i].crc;
        out_entries[i].offset = metas[i].start_offset;
    }

    if (write_idx_for_pack(pack_dir, idx_path, out_entries, count, computed_trailer) != 0) {
        fprintf(stderr, "sg: failed to write %s\n", idx_path);
        goto done;
    }

    ok = 1;

done:
    if (contents != NULL) {
        for (i = 0; i < count; i++)
            free(contents[i]);
    }
    free(contents);
    free(content_lens);
    free(types);
    free(ids);
    if (buckets != NULL) {
        size_t b;

        for (b = 0; b < SG_IDX_BUILD_BUCKETS; b++)
            free(buckets[b]);
    }
    free(buckets);
    free(bucket_lens);
    free(bucket_caps);
    free(out_entries);
    free(metas);
    free(pack_data);
    return ok ? 0 : -1;
}

int sg_pack_store_raw(const char *git_dir, const unsigned char *data, size_t len,
                      char **pack_path_out)
{
    char pack_dir[SG_PATH_MAX];
    char pack_path[SG_PATH_MAX];
    char pack_hex[SG_SHA1_HEX_LEN + 1];
    unsigned char trailer[SG_SHA1_RAW_LEN];

    if (len < 12 + SG_SHA1_RAW_LEN || memcmp(data, "PACK", 4) != 0) {
        fprintf(stderr, "sg: refusing to store data that isn't a packfile\n");
        return -1;
    }
    memcpy(trailer, data + len - SG_SHA1_RAW_LEN, SG_SHA1_RAW_LEN);
    sg_sha1_to_hex(trailer, pack_hex);

    snprintf(pack_dir, sizeof(pack_dir), "%s/objects/pack", git_dir);
    if (mkdir(pack_dir, 0755) != 0 && errno != EEXIST)
        return -1;

    snprintf(pack_path, sizeof(pack_path), "%s/pack-%s.pack", pack_dir, pack_hex);
    if (write_atomic(pack_dir, pack_path, data, len) != 0)
        return -1;

    *pack_path_out = strdup(pack_path);
    return (*pack_path_out != NULL) ? 0 : -1;
}
