#ifndef SG_OBJECT_H
#define SG_OBJECT_H

#include <stddef.h>

#include "sg/hash.h"

typedef enum {
    SG_OBJ_BLOB,
    SG_OBJ_TREE,
    SG_OBJ_COMMIT,
    SG_OBJ_TAG,
} sg_obj_type;

/* "blob" / "tree" / "commit" / "tag" */
const char *sg_obj_type_name(sg_obj_type type);

/* returns 0 and sets *out on a recognized type name, -1 otherwise */
int sg_obj_type_from_name(const char *name, sg_obj_type *out);

/* Builds the full loose-object byte stream "{type} {size}\0{content}".
   *out is malloc'd, caller frees. */
int sg_object_format(sg_obj_type type, const void *content, size_t content_len,
                     unsigned char **out, size_t *out_len);

/* SHA-1 of the full loose-object byte stream (header + content) -- the object id. */
void sg_object_hash(sg_obj_type type, const void *content, size_t content_len,
                    unsigned char id[SG_SHA1_RAW_LEN]);

/* A parsed view over an already-decompressed loose-object byte stream. `content`
   points into the `data` buffer passed to sg_object_parse -- it is not a separate
   allocation, so `data` must outlive any use of `content`. */
typedef struct {
    sg_obj_type type;
    const unsigned char *content;
    size_t content_len;
} sg_object;

/* Parses "{type} {size}\0{content}" out of data/data_len, validating that the
   declared size matches the actual remaining content length. Returns 0 on
   success, -1 on malformed input. */
int sg_object_parse(const unsigned char *data, size_t data_len, sg_object *obj);

/* ---- tree ---- */

typedef struct {
    unsigned int mode; /* traditional octal-encoded unix mode, e.g. 0100644 */
    char *name;        /* malloc'd, owned, NUL-terminated */
    unsigned char sha1[SG_SHA1_RAW_LEN];
} sg_tree_entry;

typedef struct {
    sg_tree_entry *entries;
    size_t count;
} sg_tree;

/* Sorts a working copy of entries per git's tree-entry ordering (directories
   compare as if their name had a trailing '/') and serializes it. *out is
   malloc'd, caller frees. Does not modify the caller's `entries` array. */
int sg_tree_serialize(const sg_tree_entry *entries, size_t count, unsigned char **out,
                      size_t *out_len);

/* Parses tree content into a newly allocated sg_tree; free with sg_tree_free. */
int sg_tree_parse(const unsigned char *content, size_t content_len, sg_tree *tree_out);

void sg_tree_free(sg_tree *tree);

/* ---- commit ---- */

typedef struct {
    unsigned char tree[SG_SHA1_RAW_LEN];
    unsigned char (*parents)[SG_SHA1_RAW_LEN]; /* malloc'd, owned */
    size_t parent_count;
    char *author_name;
    char *author_email;
    long long author_time;
    char author_tz[8]; /* e.g. "+0800" */
    char *committer_name;
    char *committer_email;
    long long committer_time;
    char committer_tz[8];
    char *message; /* malloc'd, owned */
} sg_commit;

int sg_commit_serialize(const sg_commit *commit, unsigned char **out, size_t *out_len);
int sg_commit_parse(const unsigned char *content, size_t content_len, sg_commit *out);
void sg_commit_free(sg_commit *commit);

/* Applies git's default `--cleanup=whitespace` commit/tag message
   normalization: strips trailing whitespace from each line, collapses runs
   of blank lines into a single blank line, drops leading and trailing blank
   lines, and -- if anything is left -- ensures the result ends with exactly
   one trailing '\n'. Leading whitespace on a line is preserved.

   *out is malloc'd, caller frees. Returns 0 on success (including the case
   where the message normalizes to nothing, in which case *out is a malloc'd
   empty string -- callers must check (*out)[0] == '\0' to detect that case;
   it is distinct from allocation failure). Returns -1 only on OOM, in which
   case *out is left untouched. */
int sg_message_cleanup(const char *msg, char **out);

/* ---- tag (annotated) ---- */

typedef struct {
    unsigned char object[SG_SHA1_RAW_LEN];
    sg_obj_type object_type;
    char *tag_name;
    char *tagger_name;
    char *tagger_email;
    long long tagger_time;
    char tagger_tz[8];
    char *message;
} sg_tag;

int sg_tag_serialize(const sg_tag *tag, unsigned char **out, size_t *out_len);
int sg_tag_parse(const unsigned char *content, size_t content_len, sg_tag *out);
void sg_tag_free(sg_tag *tag);

#endif
