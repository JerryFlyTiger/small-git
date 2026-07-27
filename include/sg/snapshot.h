#ifndef SG_SNAPSHOT_H
#define SG_SNAPSHOT_H

#include <stddef.h>

#include "sg/hash.h"
#include "sg/index.h"

/* For every path idx covers, packs the working directory's current content
   (falling back to the index's recorded blob if the working-tree file is
   missing or unreadable, so every resulting tree entry always resolves to a
   real, existing blob) into a tree, then a commit (parent = current HEAD, or
   no parent if there is none yet), written under a new, collision-free ref
   under refs/small-git/undo/ named after the current timestamp and a slug
   derived from label. Purely reads + writes objects/refs -- never touches
   HEAD, the current index, or any working-tree file. commit_id_out is filled
   with the new commit's id when non-NULL. Returns 0 on success, -1 on
   failure. Callers must treat a -1 here as "this dangerous operation cannot
   be performed safely" and abort rather than proceeding anyway -- a failed
   snapshot must not be treated as protection having happened. */
int sg_snapshot_create(const char *git_dir, const char *repo_root, const sg_index *idx,
                       const char *label, unsigned char commit_id_out[SG_SHA1_RAW_LEN]);

typedef struct {
    char *ref_name;    /* last path segment only, e.g. "1721000000-switch-to-feature", malloc'd */
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    long long timestamp; /* the snapshot commit's committer time */
    char *message;      /* the label it was created with, malloc'd */
} sg_snapshot_entry;

typedef struct {
    sg_snapshot_entry *entries; /* newest first */
    size_t count;
} sg_snapshot_list;

/* Named sg_snapshot_list_read rather than sg_snapshot_list to avoid clashing
   with the sg_snapshot_list typedef above -- C's ordinary identifier
   namespace is shared between typedefs and functions. Lists every snapshot
   under refs/small-git/undo/, newest first. A missing directory (no
   snapshots taken yet) is not an error: returns 0 with an empty *out. */
int sg_snapshot_list_read(const char *git_dir, sg_snapshot_list *out);
void sg_snapshot_list_free(sg_snapshot_list *list);

/* Looks up the tree id of the commit at list[index] (0 = newest, matching
   sg_snapshot_list_read's ordering). Returns -1 if index is out of range or
   the commit can't be read. */
int sg_snapshot_get_tree(const char *git_dir, const sg_snapshot_list *list, size_t index,
                         unsigned char tree_id_out[SG_SHA1_RAW_LEN]);

#endif
