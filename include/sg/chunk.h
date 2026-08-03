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
   compared byte-for-byte against content as a round-trip safety check. Once
   that passes, the new chunk ids are merged into the SG_CHUNK_KEEPALIVE_REF
   tree (see sg_chunk.c's keep_alive_add) so they stay reachable from git's
   object graph -- only then is a pointer blob (see SG_CHUNK_MAGIC) written
   and its id returned in id_out with *chunked_out = 1. If len < threshold,
   the round-trip check fails, or the keep-alive update fails, content is
   written as a single ordinary loose blob instead, id_out receives its id,
   and *chunked_out = 0 (the round-trip/keep-alive fallback cases additionally
   print a warning to stderr, since they mean chunking was unsafe for this
   content). Returns 0 on success, -1 on I/O/allocation failure (a round-trip
   mismatch or keep-alive failure is not a failure of this function -- both
   are handled internally by falling back to an ordinary blob). */
int sg_chunk_store_blob(const char *git_dir, const unsigned char *content, size_t len,
                        size_t threshold, unsigned char id_out[SG_SHA1_RAW_LEN],
                        int *chunked_out);

/* Filled in on a -2 ("real pointer, but data missing/corrupt") return from
   sg_chunk_read_blob/sg_chunk_effective_id below, so the caller can print an
   actionable message naming how bad the damage is. missing_count counts
   chunk ids that failed to read at all; it can be 0 even though the overall
   result is still "broken" -- that means every chunk read back fine but the
   reassembled bytes didn't hash-verify against the pointer's declared sha1
   (corruption, not loss).

   keepalive_lost is a distinct flavor of -2, set instead of the above: it
   means SG_CHUNK_KEEPALIVE_REF itself is gone (deleted, never fetched, or
   otherwise unreadable) while this repository's own .git/config records
   having used chunked storage before (see sg_repo_mark_chunking_used /
   sg_repo_chunking_was_used in sg/repo.h). Without the ref there is no way
   left to tell "a pointer this repo's own sg_chunk_store_blob really
   produced" apart from "content that coincidentally looks pointer-shaped",
   so every well-formed pointer in a repo that's lost its keep-alive ref this
   way is treated as broken -- chunk_count/missing_count are not populated in
   this case (there is no way to know how much, if anything, is actually
   lost without the ref). See chunk_resolve's discriminator comment in
   chunk.c for the full reasoning. */
typedef struct {
    size_t chunk_count;
    size_t missing_count;
    int keepalive_lost;
} sg_chunk_missing_info;

/* Prints a standard, actionable stderr message for a real-but-broken chunk
   pointer at `path` (a display name -- a repo-relative path, or anything
   else identifying what's being read), using *info as filled by a -2 return
   from sg_chunk_read_blob/sg_chunk_effective_id. Shared by every write path
   (checkout/restore/merge/rebase) so the message is worded consistently. */
void sg_chunk_print_missing_error(const char *path, const sg_chunk_missing_info *info);

/* Reads out the blob id points to. Three possible outcomes:

   1. id is not a chunk pointer, or is pointer-shaped text whose first
      declared chunk id isn't listed in the SG_CHUNK_KEEPALIVE_REF tree --
      i.e. not a pointer this repo's own sg_chunk_store_blob ever produced
      (see the discriminator comment on chunk_resolve in chunk.c for why
      only the first chunk's *identity*, not its data, is checked here, and
      what that trades off): returns 0, with *content_out and *len_out set
      to the blob's own raw bytes, unmodified. Deliberately does not depend
      on whether the first chunk's object file still exists -- see outcome 3
      below for what happens when it doesn't.

   2. id is a genuine chunk pointer (first chunk id is a keep-alive tree
      member) and every chunk reads back and the reassembled bytes
      hash-verify against the pointer's declared sha1: returns 0, with
      *content_out and *len_out set to the reassembled original content.

   3. id is a genuine chunk pointer (first chunk id is a keep-alive tree
      member) but some chunk -- including possibly the first one -- failed
      to read, or the reassembled bytes don't hash-verify: returns -2. This
      is a hard error -- data is missing or corrupt, never to be silently
      papered over by handing back the pointer's own raw text as if it were
      the file's content. *missing_out (if non-NULL) is filled in;
      *content_out and *len_out are left untouched and must not be used.

   Returns -1 only when the underlying object read of id itself fails (id
   not found/corrupt) -- distinct from outcome 3 above, where id itself
   reads fine and only its declared chunks are the problem. Never returns
   partially reassembled or unvalidated content. */
int sg_chunk_read_blob(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                       unsigned char **content_out, size_t *len_out,
                       sg_chunk_missing_info *missing_out);

/* If id points to a well-formed, hash-verified chunk pointer, fills out with
   the original content's sha1 as recorded in the pointer. If id is not a
   chunk pointer at all (including one whose first declared chunk isn't a
   keep-alive tree member -- see sg_chunk_read_blob's outcome 1), copies id
   into out unchanged. Used to normalize an id read from the index/tree into
   the id of its *content*, so it can be compared against sg_hash_file_blob()'s result
   for a working-directory file (status/diff). Unlike sg_chunk_read_blob,
   this does not need to hand back the reassembled bytes, only verify them.
   Returns -2 for the same "genuine pointer, but broken" case as
   sg_chunk_read_blob (out is left untouched); -1 only when the underlying
   object read of id itself fails; otherwise (including "not a pointer")
   returns 0 with out filled. */
int sg_chunk_effective_id(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                          unsigned char out[SG_SHA1_RAW_LEN]);

/* The ref every chunk sg_chunk_store_blob writes gets merged into, keeping
   it reachable from git's object graph so `git gc` (manual or gc.auto)
   doesn't collect chunk blobs as garbage -- see the phase 6b durability fix.
   A commit (not a bare tree) so ref/gc/fsck tooling that expects a ref to
   point at a commit has nothing unusual to special-case. */
#define SG_CHUNK_KEEPALIVE_REF "refs/sg/chunks"

/* Merges the chunk ids declared by keepalive_commit_id's tree (a
   SG_CHUNK_KEEPALIVE_REF-shaped commit -- see sg_chunk_store_blob) into this
   repo's own SG_CHUNK_KEEPALIVE_REF, deduplicating against whatever this
   repo already keeps alive on its own account. Used by fetch/clone when the
   remote advertises its own SG_CHUNK_KEEPALIVE_REF: merging rather than
   overwriting means a local chunk this repo produced (and hasn't pushed
   yet) doesn't lose its keep-alive protection just because a fetch pulled
   in the remote's ref too. keepalive_commit_id and everything its tree
   references must already be present in this repo's object store (true
   right after a fetch/clone that wanted this ref, since its objects come
   down in the same pack). Returns 0 on success, -1 if keepalive_commit_id's
   commit/tree can't be read/parsed, or if merging fails (matches
   sg_chunk_store_blob's own keep-alive failure handling -- callers should
   treat this as a soft failure worth warning about, not a hard error, since
   the remote's chunks are still physically present locally either way). */
int sg_chunk_keepalive_merge_commit(const char *git_dir,
                                    const unsigned char keepalive_commit_id[SG_SHA1_RAW_LEN]);

#endif
