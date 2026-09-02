#ifndef SG_SEQUENCER_H
#define SG_SEQUENCER_H

#include <stddef.h>

#include "sg/hash.h"

/* On-disk sequencer state for `sg cherry-pick` / `sg revert`, kept in the
   exact layout real git uses (CHERRY_PICK_HEAD / REVERT_HEAD / MERGE_MSG /
   sequencer/head / sequencer/abort-safety / sequencer/todo directly under
   git_dir) -- the OPPOSITE decision from sg-rebase/, which is deliberately
   sg's own incompatible namespace. Reason: git's cherry-pick/revert state is
   small, fully understood, and this project's stated goal is disk-format
   compatibility with real git; a real git binary run against a repository
   sg left mid-pick must be able to read (and finish, abort, etc.) it.

   Two things are asymmetric with sg-rebase/ on purpose, both measured
   against git 2.55.0:

     - CHERRY_PICK_HEAD/REVERT_HEAD/MERGE_MSG are written unconditionally
       whenever a pick stops (conflict or empty result), for EVERY commit
       count -- but sequencer/ itself (head/abort-safety/todo) is written
       ONLY when more than one commit was requested. A single-commit
       `git cherry-pick` that conflicts creates no sequencer/ directory at
       all; --continue/--abort work off CHERRY_PICK_HEAD alone in that case.
       sg_sequencer_state's `has_sequence` flag carries this distinction.

     - sequencer/todo's id field is a full 40-hex string where git writes an
       abbreviated 7-hex one. sg has no abbreviated-object-name resolution
       at all, so writing 7 hex chars would produce a file sg itself could
       not read back; writing 40 is still readable by real git (its own
       parser accepts any length prefix >= 4), so the compatibility
       direction that actually matters -- sg's own output must remain
       readable, by both tools -- is preserved. See docs/DESIGN.md's Phase
       57 section. */
typedef enum { SG_SEQ_CHERRY_PICK = 1, SG_SEQ_REVERT = 2 } sg_seq_kind;

typedef struct {
    sg_seq_kind kind;
    unsigned char current[SG_SHA1_RAW_LEN];      /* the stopped commit */
    unsigned char orig_head[SG_SHA1_RAW_LEN];     /* valid iff has_sequence */
    unsigned char abort_safety[SG_SHA1_RAW_LEN];  /* valid iff has_sequence */
    unsigned char (*todo)[SG_SHA1_RAW_LEN];       /* malloc'd, todo[0] == current, valid iff has_sequence */
    size_t todo_count;
    int has_sequence; /* sequencer/ directory present */
} sg_sequencer_state;

/* True iff a cherry-pick or revert is stopped: CHERRY_PICK_HEAD or
   REVERT_HEAD exists (lstat-only existence, same convention as
   sg_merge_head_exists/sg_rebase_state_exists -- a corrupt file must still
   count as "in progress", or the state becomes unclearable). Returns
   SG_SEQ_CHERRY_PICK, SG_SEQ_REVERT, or 0 if neither exists. If somehow
   BOTH exist (external corruption -- nothing sg itself ever writes both),
   CHERRY_PICK_HEAD wins; every gate in this project (see CLAUDE.md section
   6 of the Phase 57 spec) is meant to use THIS function, never the reader,
   to decide "is one in progress". */
sg_seq_kind sg_sequencer_kind_in_progress(const char *git_dir);

/* Reads and validates the full state for whichever of CHERRY_PICK_HEAD /
   REVERT_HEAD is present. Every field is format-checked (hex fields exactly
   40 hex chars; each todo line exactly "<verb> " + 40 hex chars + " " +
   subject, verb "pick" for a cherry-pick sequence and "revert" for a
   revert one; todo[0] must equal the id read from CHERRY_PICK_HEAD/
   REVERT_HEAD) -- any violation is treated as corrupt. Returns 0 on
   success, -1 if neither file is present or the state is malformed. */
int sg_sequencer_state_read(const char *git_dir, sg_sequencer_state *out);

/* Reads only what `--abort` needs, DELIBERATELY skipping sequencer/todo
   entirely (Phase 57 spec section 5b): which kind is stopped (existence
   only), and -- iff a sequencer/ directory is present -- orig_head and
   abort_safety from sequencer/head and sequencer/abort-safety, both a
   plain full 40-hex line in EITHER tool's own writing (git and sg agree on
   this format exactly; only sequencer/todo's id width diverges, see the
   header comment above). This is what lets `sg cherry-pick --abort` work
   on a sequence a REAL GIT binary paused: git's sequencer/todo holds
   abbreviated 7-hex ids sg's todo parser cannot read, but abort was never
   supposed to need todo at all -- git's own cherry-pick --abort does not
   read it either, it only needs where to reset back to.

   Without this, `sg_sequencer_state_read`'s all-or-nothing contract makes
   `--abort` fail on exactly the input it exists to recover from: a
   sequencer/todo damaged by a partial write, a full disk during this
   phase's own per-step state write, or (as above) git's 7-hex format --
   with every other subcommand also refusing (`--continue`/`--skip` need
   the todo for real; the old `--quit` used to go through the same full
   read too, which `sg_pick_quit` no longer does either, see sequencer.c),
   the repository was left with NO exit short of deleting `.git` files by
   hand. See docs/DESIGN.md's Phase 57 section for the measured shape of
   that dead end.

   Returns 0 with *kind_out and *has_sequence_out filled in (and
   *orig_head_out and *abort_safety_out filled in iff *has_sequence_out is 1),
   -1 if nothing is in progress at all, or if a PRESENT sequencer/head or
   sequencer/abort-safety is itself malformed -- exactly as simple a format
   as CHERRY_PICK_HEAD's own single hex line, so that really is corruption,
   not the todo-specific damage this function exists to route around. */
int sg_sequencer_abort_target(const char *git_dir, sg_seq_kind *kind_out, int *has_sequence_out,
                              unsigned char orig_head_out[SG_SHA1_RAW_LEN],
                              unsigned char abort_safety_out[SG_SHA1_RAW_LEN]);

/* Reads just CHERRY_PICK_HEAD/REVERT_HEAD's own hex value (the stopped
   commit), independent of sequencer/'s existence or parseability -- this
   is the second half of the same "an escape hatch, or a status line, must
   not need a parseable state" rule sg_sequencer_abort_target documents:
   `sg status`'s banner (spec section 7 / 5b) needs the 7-hex-prefix commit
   name to print `You are currently cherry-picking commit <7hex>.`, and
   that must not disappear just because sequencer/todo is damaged. Returns
   0 with *kind_out/current_out filled in, -1 if nothing is in progress or
   CHERRY_PICK_HEAD/REVERT_HEAD itself is malformed. */
int sg_sequencer_current_commit(const char *git_dir, sg_seq_kind *kind_out,
                                unsigned char current_out[SG_SHA1_RAW_LEN]);

/* Writes st: the CHERRY_PICK_HEAD or REVERT_HEAD file (chosen by st->kind)
   holding st->current, and -- iff st->has_sequence -- creates sequencer/
   and writes head/abort-safety/todo from st->orig_head/st->abort_safety/
   st->todo. Each todo entry's subject is looked up by reading that commit
   object fresh (sg_object_read + sg_commit_parse) rather than being carried
   in sg_sequencer_state, since the subject is derived data, not state.
   Does NOT write MERGE_MSG (see sg_sequencer_write_merge_msg below -- its
   content depends on data st does not carry, namely the message text and
   the conflicted path list). Returns 0 on success, -1 on I/O failure
   (including a todo entry whose commit object cannot be read). */
int sg_sequencer_state_write(const char *git_dir, const sg_sequencer_state *st);

/* Writes git_dir/MERGE_MSG in git's own byte-for-byte format (Phase 57
   spec section 2.2): `message` (must already end in exactly one '\n'),
   then -- iff conflict_count > 0 -- a blank line, "# Conflicts:\n", and one
   "#\t<path>\n" line per entry of conflict_paths (already in the merge
   result's own path-sorted order; this function does not sort). Paths are
   NOT quoted (this is git's own file format, not sg's own output).
   Returns 0 on success, -1 on I/O failure. */
int sg_sequencer_write_merge_msg(const char *git_dir, const char *message,
                                 char **conflict_paths, size_t conflict_count);

/* Removes CHERRY_PICK_HEAD, REVERT_HEAD, MERGE_MSG and the sequencer/
   directory (sweeping any stray leftover entries first, same convention as
   sg_rebase_state_remove, so a stray file can never leave the directory
   permanently un-rmdir-able). Not an error if any of them are already
   absent. Returns 0 on success, -1 on I/O failure. */
int sg_sequencer_state_remove(const char *git_dir);

void sg_sequencer_state_free(sg_sequencer_state *st);

#endif
