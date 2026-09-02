#ifndef SG_PICK_H
#define SG_PICK_H

#include <stddef.h>

#include "sg/hash.h"
#include "sg/sequencer.h"

/* The replay engine shared by `sg cherry-pick` and `sg revert`
   (src/cli/cmd_cherry_pick.c and src/cli/cmd_revert.c are thin argument-
   parsing shells over these five entry points). All of these print their
   own diagnostics (this is CLI-layer code, like cmd_rebase.c's engine
   halves, not a lower layer) and return a process exit code (0 or 1),
   never a third value. */

typedef struct {
    int no_commit; /* -n / --no-commit: only ever valid with a single commit */
    int mainline;  /* -m <n>: 1-based parent number, 0 means "not given" */
} sg_pick_opts;

/* Starts a fresh sequence: commits[0..count-1] (in the order the user typed
   them, already resolved via sg_rev_parse_commit) are replayed one at a
   time via the three-way merge described in the Phase 57 spec section 3.1.
   Refuses (prints a message, returns 1) if a rebase, a merge, or another
   cherry-pick/revert is already in progress, or if the working directory
   is not clean (sg_require_clean_workdir). Returns 0 if every commit
   replayed cleanly, 1 if the sequence stopped on a conflict/empty result or
   any argument/gate was refused. */
int sg_pick_start(const char *git_dir, const char *repo_root, sg_seq_kind kind,
                  unsigned char (*commits)[SG_SHA1_RAW_LEN], size_t count,
                  const sg_pick_opts *opts);

/* Resumes a stopped sequence after the user has resolved conflicts and
   staged the result. `kind` is the operation the user actually invoked
   (`sg cherry-pick --continue` vs `sg revert --continue`) -- if a sequence
   of the OTHER kind is the one actually in progress, this refuses, naming
   the operation actually stopped, rather than silently acting on it. */
int sg_pick_continue(const char *git_dir, const char *repo_root, sg_seq_kind kind);

/* Drops the currently-stopped commit (snapshotting first) and resumes with
   whatever is left of the sequence. Same kind-mismatch refusal as above. */
int sg_pick_skip(const char *git_dir, const char *repo_root, sg_seq_kind kind);

/* Snapshots, restores the working directory/HEAD to where they were before
   the sequence started (or refuses if HEAD moved since the stop, for a
   multi-commit sequence), and removes the state. Same kind-mismatch
   refusal as above. */
int sg_pick_abort(const char *git_dir, const char *repo_root, sg_seq_kind kind);

/* Removes the sequencer state and touches nothing else -- the conflicted
   index and working tree are left exactly as they were. Same kind-mismatch
   refusal as above. */
int sg_pick_quit(const char *git_dir, const char *repo_root, sg_seq_kind kind);

#endif
