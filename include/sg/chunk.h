#ifndef SG_CHUNK_H
#define SG_CHUNK_H

#include <stddef.h>

#include "sg/hash.h"

#define SG_CHUNK_MAGIC "sg-chunked v1\n"

#define SG_CHUNK_MIN_SIZE (64 * 1024)
#define SG_CHUNK_TARGET_SIZE (256 * 1024)
#define SG_CHUNK_MAX_SIZE (1024 * 1024)
#define SG_CHUNK_MASK 0x3FFFF

/* Default content length threshold (in bytes) above which sg_chunk_store_blob
   splits content into chunks instead of storing it as a single ordinary
   blob. Used by callers that don't have a repo-configured threshold. */
#define SG_CHUNK_DEFAULT_THRESHOLD (4 * 1024 * 1024)

typedef struct {
    size_t original_size;
    unsigned char original_sha1[SG_SHA1_RAW_LEN];
    unsigned char (*chunk_ids)[SG_SHA1_RAW_LEN]; /* malloc'd */
    size_t chunk_count;
} sg_chunk_pointer;

/* Splits data into content-defined chunks using a gear-hash rolling hash.
   Returns each chunk's offset/length within data via offsets_out/lengths_out
   (malloc'd, count entries each; caller frees). len == 0 sets *count_out to 0
   and both offsets_out/lengths_out to NULL, returning 0. Returns 0 on success,
   -1 on allocation failure. */
int sg_chunk_split(const unsigned char *data, size_t len, size_t **offsets_out,
                   size_t **lengths_out, size_t *count_out);

/* Determines whether content (len bytes) is a well-formed sg-chunked pointer.
   Strict parsing: the magic must match exactly, the size/sha1/chunks fields
   must be well-formed (sha1 is 40 hex chars), the number of chunk lines must
   equal the declared chunks value exactly, and there may be no extra content
   beyond that (including trailing bytes or a missing final newline). Returns
   0 (not a pointer) on any mismatch. On success returns 1 and fills *out
   (chunk_ids malloc'd; free with sg_chunk_pointer_free). Note: this only
   validates the pointer's *format* -- verifying that the reassembled chunks
   hash back to original_sha1 is the responsibility of the stage 2/3 read/write
   paths, which are the ones able to read each chunk blob's actual content. */
int sg_chunk_pointer_parse(const unsigned char *content, size_t len, sg_chunk_pointer *out);

/* Serializes a sg_chunk_pointer (as filled by sg_chunk_pointer_parse) back into
   the pointer blob's text format described above. *out_content is malloc'd,
   caller frees. Returns 0 on success, -1 on allocation failure. Used by the
   stage 2 write path to produce a pointer blob's content. */
int sg_chunk_pointer_format(const sg_chunk_pointer *p, unsigned char **out_content,
                           size_t *out_len);

void sg_chunk_pointer_free(sg_chunk_pointer *p);

/* Stores content (len bytes) under git_dir/objects/, chunking it when
   len >= threshold: content is split with sg_chunk_split, each chunk is
   written as its own loose blob, and the written chunks are read back and
   compared byte-for-byte against content as a round-trip safety check before
   a pointer blob (see SG_CHUNK_MAGIC) is written and its id returned in
   id_out with *chunked_out = 1. If len < threshold, or the round-trip check
   fails, content is written as a single ordinary loose blob instead, id_out
   receives its id, and *chunked_out = 0 (a round-trip failure additionally
   prints a warning to stderr, since it means chunking was silently unsafe for
   this content). Returns 0 on success, -1 on I/O/allocation failure (a
   round-trip mismatch is not a failure of this function -- it is handled
   internally by falling back to an ordinary blob). */
int sg_chunk_store_blob(const char *git_dir, const unsigned char *content, size_t len,
                        size_t threshold, unsigned char id_out[SG_SHA1_RAW_LEN],
                        int *chunked_out);

/* Reads out the blob id points to; if it strictly matches the pointer format
   (sg_chunk_pointer_parse) and the reassembled chunks hash back to the
   pointer's declared sha1, returns the reassembled original content
   (*content_out malloc'd, caller frees). Otherwise (not a pointer, a
   malformed pointer, any chunk object failing to read, or a hash mismatch
   after reassembly), returns the blob's own raw content unmodified. Only
   returns -1 when the underlying object read genuinely fails (id itself is
   not found/corrupt); a pointer-shaped blob that fails reassembly/validation
   is not an error here -- it falls back to raw content and returns 0. Never
   returns partially reassembled or unvalidated content. */
int sg_chunk_read_blob(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                       unsigned char **content_out, size_t *len_out);

/* If id points to a well-formed, hash-verified chunk pointer, fills out with
   the original content's sha1 as recorded in the pointer. Otherwise (not a
   pointer, malformed, any chunk unreadable, or a hash mismatch), copies id
   into out unchanged. Used to normalize an id read from the index/tree into
   the id of its *content*, so it can be compared against
   sg_hash_file_blob()'s result for a working-directory file (status/diff).
   Unlike sg_chunk_read_blob, this does not need to hand back the reassembled
   bytes, only verify them. Returns -1 only when the underlying object read
   fails; otherwise (including "not a pointer") returns 0 with out filled. */
int sg_chunk_effective_id(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                          unsigned char out[SG_SHA1_RAW_LEN]);

#endif
