#ifndef SG_MERGE_H
#define SG_MERGE_H

#include <stddef.h>

#include "sg/hash.h"
#include "sg/index.h"

/* ---- merge base (common ancestor) ---- */

/* Finds the closest common ancestor(s) of commits a and b, walking ALL
   parents (not just first-parent) via sg_object_read (so packed commits are
   found too). Returns 0 and fills out with the single best common ancestor;
   -1 if a and b share no common history at all (unrelated histories); -2 if
   there are multiple, mutually independent best common ancestors
   (criss-cross merge history) -- callers must not silently pick one of
   those, this is reported as an error instead. */
int sg_merge_base(const char *git_dir, const unsigned char a[SG_SHA1_RAW_LEN],
                  const unsigned char b[SG_SHA1_RAW_LEN], unsigned char out[SG_SHA1_RAW_LEN]);

/* ---- three-way file content merge (diff3-lite) ---- */

/* Merges base/ours/theirs file content line-by-line. The merged result
   (which may contain "<<<<<<< / ======= / >>>>>>>" conflict markers) is
   written to a malloc'd *out (out_len bytes); caller frees. ours_label/
   theirs_label are used verbatim on the marker lines. Returns 0 if the merge
   is clean (no conflict), 1 if there is a conflict (markers are present in
   *out), -1 on error (allocation failure). If any of base/ours/theirs
   contains a NUL byte, no line-based merge is attempted -- this is
   immediately treated as a conflict, with *out set to a copy of ours
   (out_len = ours_len), matching how a binary conflict is left in the
   working tree. */
int sg_merge_content(const unsigned char *base, size_t base_len, const unsigned char *ours,
                     size_t ours_len, const unsigned char *theirs, size_t theirs_len,
                     const char *ours_label, const char *theirs_label, unsigned char **out,
                     size_t *out_len);

/* ---- three-way tree merge ---- */

typedef struct {
    char *path; /* malloc'd, owned */

    int conflict; /* 1 = unresolved */

    /* Valid when !conflict: the resolved outcome for this path. */
    int deleted; /* 1 = path does not exist in the merge result */
    unsigned int mode;
    unsigned char sha1[SG_SHA1_RAW_LEN]; /* valid iff !conflict && !deleted */

    /* Valid when conflict: which of base/ours/theirs have this path, and
       their mode/blob -- used to write index stages 1/2/3 (only for the
       parties that are actually present) and, for a modify/delete conflict,
       to know which single side's content survives in the working tree. */
    int base_present, ours_present, theirs_present;
    unsigned int base_mode, ours_mode, theirs_mode;
    unsigned char base_sha1[SG_SHA1_RAW_LEN];
    unsigned char ours_sha1[SG_SHA1_RAW_LEN];
    unsigned char theirs_sha1[SG_SHA1_RAW_LEN];

    /* Valid when conflict: the bytes to write into the working tree (either
       sg_merge_content's marker-laden output, or -- for a modify/delete
       conflict where only one side still has the file -- that side's raw
       content as-is). malloc'd, owned. */
    unsigned char *conflict_content;
    size_t conflict_content_len;
} sg_merge_result_entry;

typedef struct {
    sg_merge_result_entry *entries; /* sorted by path */
    size_t count;
} sg_merge_result;

/* Three-way merges base_tree/ours_tree/theirs_tree (flattened via
   sg_tree_flatten and unioned by path). Any newly-merged blob content (the
   clean multi-line-merge case) is written to the object store via
   sg_loose_write as part of computing the result. ours_label/theirs_label
   are forwarded to sg_merge_content for conflict markers. Returns 0 on
   success (out->entries may still contain conflicts -- check each entry's
   .conflict field, or just look for any conflict to know whether the merge
   as a whole is clean), -1 on error (I/O/allocation/corrupt tree). */
int sg_merge_trees(const char *git_dir, const unsigned char base_tree[SG_SHA1_RAW_LEN],
                   const unsigned char ours_tree[SG_SHA1_RAW_LEN],
                   const unsigned char theirs_tree[SG_SHA1_RAW_LEN], const char *ours_label,
                   const char *theirs_label, sg_merge_result *out);

void sg_merge_result_free(sg_merge_result *result);

/* Materializes a sg_merge_result into the working tree and into a fresh
   in-memory index: clean entries are written as files and staged at stage 0,
   deleted entries are removed, conflicted entries get their marker-laden
   content written and index stages 1/2/3 (only for the sides actually
   present). Does NOT write the index to disk, does not touch HEAD/refs, and
   does not write MERGE_HEAD -- callers own all of that. *index_out is owned
   by the caller (sg_index_free). *conflict_count_out is the number of
   conflicted paths; *conflict_paths_out is a malloc'd array of malloc'd
   paths (caller frees both) or NULL when there are none. Returns 0 on
   success, -1 if a chunked blob's data is unrecoverable or the index could
   not be built completely -- in which case the working tree may already be
   partially updated (same caveat as sg_apply_tree_to_workdir) and the caller
   must abort rather than record a state that silently dropped paths. */
int sg_merge_result_apply(const char *git_dir, const char *repo_root, const sg_merge_result *result,
                          sg_index *index_out, char ***conflict_paths_out, size_t *conflict_count_out);

/* ---- MERGE_HEAD (in-progress merge state) ---- */

/* True iff git_dir/MERGE_HEAD exists in any form (a merge is in progress).
   Deliberately does not parse, or even stat through, the path: real git
   gates on file_exists(), an lstat that only asks whether something is
   there, and refuses when MERGE_HEAD is empty, malformed, or a directory
   (all three measured, git 2.55.0). Use this -- not sg_merge_head_read --
   to decide whether a merge is in flight: the read below cannot tell "no
   merge" from "corrupt", so a gate built on it would let a corrupt merge
   state through. */
int sg_merge_head_exists(const char *git_dir);

/* Reads git_dir/MERGE_HEAD (the "theirs" commit id of an in-progress merge)
   into out. Returns 0 on success, -1 if there is no merge in progress or the
   file is malformed. */
int sg_merge_head_read(const char *git_dir, unsigned char out[SG_SHA1_RAW_LEN]);

/* Writes git_dir/MERGE_HEAD, recording id as the "theirs" commit of an
   in-progress merge. Returns 0 on success, -1 on I/O failure. */
int sg_merge_head_write(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN]);

/* Removes git_dir/MERGE_HEAD, ending an in-progress merge. Not an error if
   it doesn't exist. Returns 0 on success, -1 on I/O failure. */
int sg_merge_head_remove(const char *git_dir);

#endif
