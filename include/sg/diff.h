#ifndef SG_DIFF_H
#define SG_DIFF_H

#include <stddef.h>

#include "sg/chunk.h"
#include "sg/hash.h"
#include "sg/index.h"

/* Building a list of changed paths, decoupled from where the two sides come
   from. Before this existed `sg diff` could only ever compare the index to the
   working tree, because the loop that found the changed paths and the loop that
   printed them were the same loop. Everything here is about *which paths
   changed and what is on each side*; rendering lives in sg/diff_out.h. */

typedef enum {
    SG_DIFF_SIDE_ABSENT = 0, /* the path does not exist on this side */
    SG_DIFF_SIDE_BLOB,       /* content is the object named by `id` */
    SG_DIFF_SIDE_WORKDIR     /* content is the file at <repo_root>/<path> */
} sg_diff_side_kind;

typedef struct {
    sg_diff_side_kind kind;
    /* The tree/index entry mode, or 0 for "unknown". WORKDIR sides are always
       0: the index-vs-working-tree comparison has never looked at the mode bit
       on disk, and teaching it to is a separate change. Mode comparison is
       therefore skipped whenever either side is 0 -- a silently-assumed 100644
       would report a spurious mode change on every workdir diff. */
    unsigned int mode;
    unsigned char id[SG_SHA1_RAW_LEN]; /* valid iff kind == SG_DIFF_SIDE_BLOB */
} sg_diff_side;

typedef struct {
    char *path; /* owned, repo-relative, '/' separated */
    sg_diff_side old_side;
    sg_diff_side new_side;
    /* 1 when this row stands for an unresolved conflict rather than a content
       change. Both sides are ABSENT: there is no single "before" blob to show.
       Renderers print git's "U"/"Unmerged" for it and, measured against git
       2.55.0, leave it OUT of the "N files changed" total -- a diff whose only
       row is unmerged prints " 0 files changed".

       Without this flag the merge-join would see a path that is in the tree but
       has no stage-0 index entry and call it a deletion, inventing a deletion
       that never happened. That is worse than the old behaviour of skipping
       unmerged paths outright, which is why the flag exists rather than a
       `continue`. */
    int unmerged;
} sg_diff_entry;

typedef struct {
    sg_diff_entry *entries; /* sorted by path, byte-wise */
    size_t count;
    size_t cap;
} sg_diff_list;

/* Frees every owned path and the array itself, and zeroes the list. */
void sg_diff_list_free(sg_diff_list *list);

/* The four builders below all produce a list holding only paths that actually
   differ: a path whose two sides carry the same content (and the same mode,
   when both modes are known) is left out entirely.

   `old_tree`/`new_tree` may be NULL, which means "the empty tree". That is what
   an unborn HEAD looks like, and taking NULL here saves every caller from
   having to write an empty tree object just to diff against it.

   Return 0 on success, -1 on failure, and -2 when a tree entry name fails
   sg_path_component_is_safe -- the -2 and the `bad_path` buffer (SG_PATH_MAX,
   may be NULL) are sg_tree_flatten's contract, propagated rather than
   flattened into -1 so the CLI can name the offending path. */

/* tree vs tree -- `sg diff <rev> <rev>`. */
int sg_diff_trees(const char *git_dir, const unsigned char *old_tree,
                  const unsigned char *new_tree, sg_diff_list *out, char *bad_path);

/* tree vs index -- `sg diff --cached`. A path carrying stage 1/2/3 entries has
   no single staged blob to diff against and yields exactly one row, with
   `unmerged` set. Measured: `git diff --cached --name-status` prints "U". */
int sg_diff_tree_index(const char *git_dir, const unsigned char *old_tree,
                       const sg_index *idx, sg_diff_list *out, char *bad_path);

/* index vs working tree -- plain `sg diff`. Never fails on a missing working
   tree file: that is a deletion, not an error.

   A blob that cannot be read at all -- a deleted object, or a chunk pointer
   whose data is gone -- must NOT fail the whole call either. The builder
   cannot answer "did this path change", so it records the path as changed and
   leaves the complaining to the renderer, which is holding the path and can
   name it. Aborting the list instead loses two things at once: every other
   path's diff, and the actionable message itself, since nothing downstream
   ever gets far enough to know which file was broken.

   An unmerged path produces up to TWO rows, in this order -- measured against
   git 2.55.0, which prints "U conflict.txt" and "M conflict.txt" for the same
   path:
     1. the `unmerged` row;
     2. a normal row comparing the path's *stage 2* (ours) blob against the
        working tree, emitted only when a stage-2 entry exists and its content
        differs from the file on disk. Writing the working tree back to exactly
        the stage-2 bytes makes this second row disappear, which is how the
        stage was identified -- stage 1 and stage 3 both leave it in place. */
int sg_diff_index_workdir(const char *git_dir, const char *repo_root,
                          const sg_index *idx, sg_diff_list *out);

/* tree vs working tree -- `sg diff <rev>`.

   The *index* decides which paths take part, not the working tree. Measured
   against git 2.55.0: after `git rm --cached f` the file is still on disk, and
   `git diff HEAD` reports it as deleted -- so a path in the tree but not in the
   index is a deletion even though the bytes are right there, and an untracked
   file is not reported at all. Getting this from the working tree instead would
   turn every untracked file into an addition.

   An unmerged path is NOT special here: the index only decides membership, and
   the content still comes from the working tree, so such a path takes part as
   an ordinary modification against the tree's blob. Measured: `git diff HEAD
   --name-status` prints "M" for a conflicted path, not "U". */
int sg_diff_tree_workdir(const char *git_dir, const char *repo_root,
                         const unsigned char *old_tree, const sg_index *idx,
                         sg_diff_list *out, char *bad_path);

/* Loads one side's bytes. An ABSENT side yields (NULL, 0), which is what the
   renderers want for an addition or a deletion. BLOB sides go through
   sg_chunk_read_blob, so a chunk pointer is resolved to the real content.

   Returns 0, -1 (unreadable), or -2 for a genuine chunk pointer whose data is
   missing or corrupt, filling *missing (may be NULL). -2 must not be flattened
   into -1: diffing the pointer's own raw text against the other side produces
   meaningless hunks instead of an error. */
int sg_diff_side_read(const char *git_dir, const char *repo_root, const char *path,
                      const sg_diff_side *side, unsigned char **data, size_t *len,
                      sg_chunk_missing_info *missing);

#endif /* SG_DIFF_H */
