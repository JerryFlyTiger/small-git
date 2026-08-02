#ifndef SG_REBASE_H
#define SG_REBASE_H

#include <stddef.h>

#include "sg/hash.h"

/* On-disk sequencer state for a non-interactive `sg rebase`, kept under
   git_dir/sg-rebase/ (deliberately NOT git's own rebase-merge/rebase-apply
   layout: our format isn't compatible with git's, and a half-matching
   directory there would make a real git binary misinterpret it. Our own
   namespace is safely ignored by real git instead). */
typedef struct {
    unsigned char onto[SG_SHA1_RAW_LEN];      /* rebase target base commit */
    unsigned char orig_head[SG_SHA1_RAW_LEN]; /* HEAD before the rebase started, for --abort */
    char *orig_branch;                        /* malloc'd: branch rebase was started on */
    unsigned char (*todo)[SG_SHA1_RAW_LEN];   /* malloc'd, oldest-to-newest, not yet replayed */
    size_t todo_count;
    unsigned char current[SG_SHA1_RAW_LEN]; /* commit paused on a conflict, valid iff has_current */
    int has_current;
} sg_rebase_state;

/* True iff git_dir/sg-rebase/ exists (a rebase is in progress). */
int sg_rebase_state_exists(const char *git_dir);

/* Reads and validates git_dir/sg-rebase/. Every field is format-checked (hex
   fields must be exactly 40 hex chars, todo lines each exactly 40 hex chars,
   orig-branch must pass the same branch-name safety check refs.c uses) --
   any violation is treated as a corrupted sequencer state. Returns 0 on
   success, -1 if no rebase is in progress or the state is malformed/corrupt
   (caller should tell the user to run `sg rebase --abort`). */
int sg_rebase_state_read(const char *git_dir, sg_rebase_state *out);

/* Writes st to git_dir/sg-rebase/, creating the directory if needed.
   Overwrites any previous state. If st->has_current is 0, an existing
   `current` file is removed so a stale value can't linger across a
   --skip/--continue. Returns 0 on success, -1 on I/O failure. */
int sg_rebase_state_write(const char *git_dir, const sg_rebase_state *st);

/* Removes git_dir/sg-rebase/ entirely (the rebase is finished or aborted).
   Not an error if it doesn't exist. Returns 0 on success, -1 on I/O failure. */
int sg_rebase_state_remove(const char *git_dir);

void sg_rebase_state_free(sg_rebase_state *st);

/* Computes the commit list `sg rebase <upstream>` needs to replay: walks
   head_commit's first-parent chain back to (excluding) base_commit --
   base_commit is expected to be sg_merge_base(upstream, head_commit) -- and
   collects the visited commits oldest-to-newest into *out (malloc'd,
   caller frees). If any visited commit (other than base_commit itself) has
   more than one parent, non-interactive rebase refuses to flatten that
   history: *found_merge_commit_out is set to 1, merge_commit_out receives
   its id, and *out / *out_count are left empty (NULL/0) -- callers must
   check *found_merge_commit_out before trusting the list. Returns 0 on success
   (which still requires checking *found_merge_commit_out), -1 on I/O
   failure or if base_commit is never reached by following first-parent
   links from head_commit (a corrupt/unexpected graph, since callers are
   expected to have already validated base_commit via sg_merge_base). */
int sg_rebase_compute_todo(const char *git_dir, const unsigned char head_commit[SG_SHA1_RAW_LEN],
                          const unsigned char base_commit[SG_SHA1_RAW_LEN],
                          unsigned char (**out)[SG_SHA1_RAW_LEN], size_t *out_count,
                          unsigned char merge_commit_out[SG_SHA1_RAW_LEN],
                          int *found_merge_commit_out);

#endif
