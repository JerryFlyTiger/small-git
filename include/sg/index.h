#ifndef SG_INDEX_H
#define SG_INDEX_H

#include <stddef.h>

#include "sg/hash.h"

typedef struct {
    unsigned int ctime_sec, ctime_nsec;
    unsigned int mtime_sec, mtime_nsec;
    unsigned int dev, ino;
    unsigned int mode;
    unsigned int uid, gid;
    unsigned int file_size;
    unsigned char sha1[SG_SHA1_RAW_LEN];
    char *path; /* malloc'd, owned */
} sg_index_entry;

typedef struct {
    sg_index_entry *entries; /* sorted by path (byte-wise) */
    size_t count;
} sg_index;

/* Reads git_dir/index. A missing index file is a normal, empty index (count
   0, returns 0) -- a brand new repo has no index yet. Returns -1 on
   malformed/corrupt content (bad signature, unsupported version, checksum
   mismatch, truncated data). */
int sg_index_read(const char *git_dir, sg_index *out);

/* Writes index to git_dir/index in git's version-2 on-disk format. Entries
   must already be sorted by path (sg_index_upsert/sg_index_remove maintain
   this invariant). */
int sg_index_write(const char *git_dir, const sg_index *index);

void sg_index_free(sg_index *index);

/* Binary search by path. Returns the entry's position, or -1 if not found. */
int sg_index_find(const sg_index *index, const char *path);

/* Inserts a new entry or overwrites the existing entry at the same path,
   keeping entries sorted by path. Copies entry->path; caller retains
   ownership of the sg_index_entry passed in. Returns 0 on success, -1 on
   allocation failure. */
int sg_index_upsert(sg_index *index, const sg_index_entry *entry);

/* Removes the entry at path, if present. Returns 0 on success, -1 if path
   was not found in the index. */
int sg_index_remove(sg_index *index, const char *path);

#endif
