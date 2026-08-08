#include "sg/index.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SG_INDEX_HEADER_LEN 12
#define SG_INDEX_ENTRY_FIXED_LEN 62 /* 10*4 (stat fields) + 20 (sha1) + 2 (flags) */
#define SG_INDEX_CHECKSUM_LEN 20

static char *index_path(const char *git_dir)
{
    size_t len = strlen(git_dir) + strlen("/index") + 1;
    char *path = malloc(len);

    if (path == NULL)
        return NULL;
    snprintf(path, len, "%s/index", git_dir);
    return path;
}

static unsigned int read_be32(const unsigned char **p)
{
    unsigned int v;

    memcpy(&v, *p, 4);
    *p += 4;
    return ntohl(v);
}

static unsigned int read_be16(const unsigned char **p)
{
    unsigned short v;

    memcpy(&v, *p, 2);
    *p += 2;
    return ntohs(v);
}

static void write_be32(unsigned char **p, unsigned int v)
{
    unsigned int be = htonl(v);

    memcpy(*p, &be, 4);
    *p += 4;
}

static void write_be16(unsigned char **p, unsigned short v)
{
    unsigned short be = htons(v);

    memcpy(*p, &be, 2);
    *p += 2;
}

static void free_entries(sg_index_entry *entries, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++)
        free(entries[i].path);
    free(entries);
}

int sg_index_read(const char *git_dir, sg_index *out)
{
    char *path = index_path(git_dir);
    FILE *f;
    unsigned char *buf = NULL;
    size_t len = 0;
    size_t cap = 65536;
    unsigned char version;
    unsigned int version_num;
    unsigned int nentries;
    const unsigned char *p;
    const unsigned char *content_end; /* start of the trailing checksum */
    unsigned char checksum[SG_SHA1_RAW_LEN];
    sg_index_entry *entries = NULL;
    unsigned int i;

    out->entries = NULL;
    out->count = 0;

    if (path == NULL)
        return -1;

    f = fopen(path, "rb");
    free(path);
    if (f == NULL)
        return 0; /* no index yet: an empty one */

    buf = malloc(cap);
    if (buf == NULL) {
        fclose(f);
        return -1;
    }
    for (;;) {
        size_t n;

        if (len == cap) {
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
        n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0)
            break;
    }
    if (ferror(f)) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    if (len < SG_INDEX_HEADER_LEN + SG_INDEX_CHECKSUM_LEN) {
        free(buf);
        return -1;
    }

    p = buf;
    if (memcmp(p, "DIRC", 4) != 0) {
        free(buf);
        return -1;
    }
    p += 4;
    version_num = read_be32(&p);
    if (version_num != 2 && version_num != 3) {
        free(buf);
        return -1;
    }
    version = (unsigned char)version_num;
    nentries = read_be32(&p);

    content_end = buf + len - SG_INDEX_CHECKSUM_LEN;

    sg_sha1(buf, (size_t)(content_end - buf), checksum);
    if (memcmp(checksum, content_end, SG_SHA1_RAW_LEN) != 0) {
        free(buf);
        return -1;
    }

    /* nentries is an attacker-controlled u32 straight off disk (read above
       at :142). Without a sanity check, a crafted 30-byte index declaring
       nentries = 0xFFFFFFFF (its checksum can trivially be made to match,
       since the check above covers the whole file including this field)
       would drive `malloc((size_t)nentries * sizeof(*entries))` to request
       hundreds of GB. Bound it instead by how many entries the remaining
       file bytes could possibly hold: every entry occupies at least
       padded_len bytes, whose minimum is reached at name_len == 0 and
       non-extended (entry_len = SG_INDEX_ENTRY_FIXED_LEN = 62, padded to a
       multiple of 8 -> 64). So nentries can never legitimately exceed
       (bytes remaining before the trailing checksum) / 64; anything larger
       is proof the file is malformed and can be rejected before the
       allocation, mirroring the existing pack object-count bound in
       src/storage/pack.c (sg_pack_index_existing). */
    {
        const size_t min_padded_len = ((SG_INDEX_ENTRY_FIXED_LEN + 8) / 8) * 8;
        size_t max_possible_entries = (size_t)(content_end - p) / min_padded_len;

        if ((size_t)nentries > max_possible_entries) {
            free(buf);
            return -1;
        }
    }

    if (nentries > 0) {
        entries = malloc((size_t)nentries * sizeof(*entries));
        if (entries == NULL) {
            free(buf);
            return -1;
        }
    }

    for (i = 0; i < nentries; i++) {
        const unsigned char *entry_start = p;
        sg_index_entry *e = &entries[i];
        unsigned int flags;
        int extended = 0;
        unsigned int name_len;
        size_t fixed_len;
        size_t entry_len;
        size_t padded_len;
        const unsigned char *name_start;

        e->path = NULL;

        if ((size_t)(content_end - p) < SG_INDEX_ENTRY_FIXED_LEN)
            goto fail;

        e->ctime_sec = read_be32(&p);
        e->ctime_nsec = read_be32(&p);
        e->mtime_sec = read_be32(&p);
        e->mtime_nsec = read_be32(&p);
        e->dev = read_be32(&p);
        e->ino = read_be32(&p);
        e->mode = read_be32(&p);
        e->uid = read_be32(&p);
        e->gid = read_be32(&p);
        e->file_size = read_be32(&p);
        memcpy(e->sha1, p, SG_SHA1_RAW_LEN);
        p += SG_SHA1_RAW_LEN;
        flags = read_be16(&p);

        /* stage bits 13-12: 0 = ordinary entry, 1/2/3 = base/ours/theirs
           while a conflict at this path is unresolved. Phase 4b onwards
           supports multiple entries sharing a path, one per stage. */
        e->stage = (flags >> 12) & 0x3;

        if (flags & 0x4000) { /* extended flag, bit 14 */
            extended = 1;
            if (version != 3 || (size_t)(content_end - p) < 2)
                goto fail;
            p += 2; /* extra flags: not used in phase 2, skip */
        }

        name_start = p;
        name_len = flags & 0x0FFF;
        if (name_len == 0x0FFF) {
            const unsigned char *nul = memchr(name_start, '\0', (size_t)(content_end - name_start));

            if (nul == NULL)
                goto fail;
            name_len = (unsigned int)(nul - name_start);
        } else {
            if ((size_t)(content_end - name_start) < name_len)
                goto fail;
        }

        e->path = malloc((size_t)name_len + 1);
        if (e->path == NULL)
            goto fail;
        memcpy(e->path, name_start, name_len);
        e->path[name_len] = '\0';

        fixed_len = SG_INDEX_ENTRY_FIXED_LEN + (extended ? 2 : 0);
        entry_len = fixed_len + name_len;
        padded_len = ((entry_len + 8) / 8) * 8;
        if ((size_t)(content_end - entry_start) < padded_len)
            goto fail;
        p = entry_start + padded_len;
        continue;

    fail:
        /* e->path (if malloc'd just above) is freed here too: free_entries'
           upper bound is i + 1, not i, so it covers the in-progress entry
           as well as every fully-parsed one before it. */
        free_entries(entries, i + 1);
        free(buf);
        return -1;
    }

    /* Skip extensions: each is a 4-byte signature + 4-byte big-endian size +
       size bytes of data we don't need to interpret (e.g. git's TREE
       cache-tree extension). */
    while (p < content_end) {
        unsigned int ext_size;

        if ((size_t)(content_end - p) < 8) {
            free_entries(entries, nentries);
            free(buf);
            return -1;
        }
        p += 4; /* signature */
        ext_size = read_be32(&p);
        if ((size_t)(content_end - p) < ext_size) {
            free_entries(entries, nentries);
            free(buf);
            return -1;
        }
        p += ext_size;
    }

    free(buf);
    out->entries = entries;
    out->count = nentries;
    return 0;
}

int sg_index_write(const char *git_dir, const sg_index *index)
{
    char *path = index_path(git_dir);
    size_t total = SG_INDEX_HEADER_LEN;
    size_t i;
    unsigned char *buf;
    unsigned char *p;
    unsigned char checksum[SG_SHA1_RAW_LEN];
    FILE *f;
    int rc = 0;

    if (path == NULL)
        return -1;

    for (i = 0; i < index->count; i++) {
        size_t name_len = strlen(index->entries[i].path);
        size_t entry_len = SG_INDEX_ENTRY_FIXED_LEN + name_len;
        size_t padded_len = ((entry_len + 8) / 8) * 8;

        total += padded_len;
    }
    total += SG_INDEX_CHECKSUM_LEN;

    buf = malloc(total);
    if (buf == NULL) {
        free(path);
        return -1;
    }

    p = buf;
    memcpy(p, "DIRC", 4);
    p += 4;
    write_be32(&p, 2);
    write_be32(&p, (unsigned int)index->count);

    for (i = 0; i < index->count; i++) {
        const sg_index_entry *e = &index->entries[i];
        unsigned char *entry_start = p;
        size_t name_len = strlen(e->path);
        size_t entry_len = SG_INDEX_ENTRY_FIXED_LEN + name_len;
        size_t padded_len = ((entry_len + 8) / 8) * 8;
        size_t padding = padded_len - entry_len;
        unsigned short flags = (unsigned short)((name_len < 0x0FFF ? name_len : 0x0FFF) |
                                                ((e->stage & 0x3) << 12));

        write_be32(&p, e->ctime_sec);
        write_be32(&p, e->ctime_nsec);
        write_be32(&p, e->mtime_sec);
        write_be32(&p, e->mtime_nsec);
        write_be32(&p, e->dev);
        write_be32(&p, e->ino);
        write_be32(&p, e->mode);
        write_be32(&p, e->uid);
        write_be32(&p, e->gid);
        write_be32(&p, e->file_size);
        memcpy(p, e->sha1, SG_SHA1_RAW_LEN);
        p += SG_SHA1_RAW_LEN;
        write_be16(&p, flags);

        memcpy(p, e->path, name_len);
        p += name_len;
        memset(p, 0, padding);
        p += padding;

        (void)entry_start;
    }

    sg_sha1(buf, total - SG_INDEX_CHECKSUM_LEN, checksum);
    memcpy(p, checksum, SG_SHA1_RAW_LEN);

    f = fopen(path, "wb");
    free(path);
    if (f == NULL) {
        free(buf);
        return -1;
    }
    if (fwrite(buf, 1, total, f) != total)
        rc = -1;
    if (fclose(f) != 0)
        rc = -1;

    free(buf);
    return rc;
}

void sg_index_free(sg_index *index)
{
    free_entries(index->entries, index->count);
    index->entries = NULL;
    index->count = 0;
}

/* Sort/lookup key is (path, stage): path compares first (byte-wise), and
   only entries sharing a path are then ordered by stage -- this matches
   git's on-disk ordering exactly (required for a real git to read the index
   correctly) and keeps every stage of a conflicted path contiguous. */
static int find_insert_pos_stage(const sg_index *index, const char *path, unsigned int stage,
                                 size_t *pos_out)
{
    size_t lo = 0;
    size_t hi = index->count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(index->entries[mid].path, path);

        if (cmp == 0) {
            if (index->entries[mid].stage == stage) {
                *pos_out = mid;
                return 1;
            }
            cmp = index->entries[mid].stage < stage ? -1 : 1;
        }
        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    *pos_out = lo;
    return 0;
}

/* Path-only search, used by sg_index_remove_all_stages to find the
   contiguous run of every stage sharing a path. */
static int find_path_range(const sg_index *index, const char *path, size_t *lo_out, size_t *hi_out)
{
    size_t lo = 0;
    size_t hi = index->count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(index->entries[mid].path, path);

        if (cmp == 0) {
            size_t range_lo = mid;
            size_t range_hi = mid + 1;

            while (range_lo > 0 && strcmp(index->entries[range_lo - 1].path, path) == 0)
                range_lo--;
            while (range_hi < index->count && strcmp(index->entries[range_hi].path, path) == 0)
                range_hi++;
            *lo_out = range_lo;
            *hi_out = range_hi;
            return 1;
        }
        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

int sg_index_find_stage(const sg_index *index, const char *path, unsigned int stage)
{
    size_t pos;

    if (find_insert_pos_stage(index, path, stage, &pos))
        return (int)pos;
    return -1;
}

int sg_index_find(const sg_index *index, const char *path)
{
    return sg_index_find_stage(index, path, 0);
}

int sg_index_upsert(sg_index *index, const sg_index_entry *entry)
{
    size_t pos;
    char *path_copy = strdup(entry->path);

    if (path_copy == NULL)
        return -1;

    if (find_insert_pos_stage(index, entry->path, entry->stage, &pos)) {
        free(index->entries[pos].path);
        index->entries[pos] = *entry;
        index->entries[pos].path = path_copy;
        return 0;
    }

    {
        sg_index_entry *grown = realloc(index->entries, (index->count + 1) * sizeof(*grown));

        if (grown == NULL) {
            free(path_copy);
            return -1;
        }
        index->entries = grown;
        memmove(&index->entries[pos + 1], &index->entries[pos],
               (index->count - pos) * sizeof(*grown));
        index->entries[pos] = *entry;
        index->entries[pos].path = path_copy;
        index->count++;
    }
    return 0;
}

int sg_index_remove(sg_index *index, const char *path)
{
    size_t pos;

    if (!find_insert_pos_stage(index, path, 0, &pos))
        return -1;

    free(index->entries[pos].path);
    memmove(&index->entries[pos], &index->entries[pos + 1],
           (index->count - pos - 1) * sizeof(*index->entries));
    index->count--;
    if (index->count == 0) {
        free(index->entries);
        index->entries = NULL;
    }
    return 0;
}

int sg_index_remove_all_stages(sg_index *index, const char *path)
{
    size_t lo, hi, n, i;

    if (!find_path_range(index, path, &lo, &hi))
        return 0;

    n = hi - lo;
    for (i = lo; i < hi; i++)
        free(index->entries[i].path);
    memmove(&index->entries[lo], &index->entries[hi], (index->count - hi) * sizeof(*index->entries));
    index->count -= n;
    if (index->count == 0) {
        free(index->entries);
        index->entries = NULL;
    }
    return (int)n;
}

int sg_index_has_unmerged(const sg_index *index)
{
    size_t i;

    for (i = 0; i < index->count; i++) {
        if (index->entries[i].stage != 0)
            return 1;
    }
    return 0;
}
