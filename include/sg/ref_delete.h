#ifndef SG_REF_DELETE_H
#define SG_REF_DELETE_H

#include "sg/hash.h"

/* Phase 76: `sg tag -d`'s Phase 74 batch-delete engine, pulled out of
   cmd_tag.c so `sg branch -d`/-D can share it -- see docs/RULES-refs-revparse.md
   for the mechanism this reproduces (per-name refs/<prefix><name>.lock,
   O_CREAT|O_EXCL, EEXIST collision handling, singular/plural wording keyed
   on how many names actually entered the transaction). This header only
   documents what changed in the EXTRACTION; the mechanism itself is
   unchanged from Phase 74/74-round-4 and is still explained in full in
   cli/ref_delete.c's own comments (moved verbatim from cmd_tag.c), with
   ONE genuine behavior change measured while extending it to branch (not
   present in tag at all): a per-name `precheck`, run BEFORE existence is
   even tested, because git's "used by worktree" refusal for `sg branch -d`
   fires on an UNBORN branch that is currently checked out (no ref file
   exists at all yet) -- existence cannot be the first thing decided for
   every command sharing this engine, only for the ones (tag) that have no
   such precheck.

   Namespace and per-kind wording (not-found, deleted, too-long, delete-
   failed) are parameterized because tag and branch disagree on every one
   of those strings:
     - tag:    "tag '%s' not found.", "Deleted tag '%s' (was %.7s)\n",
               "tag name too long: '%s'", "failed to delete tag '%s'"
     - branch: "branch '%s' not found", "Deleted branch %s (was %.7s).\n",
               "branch name too long: '%s'", "failed to delete branch '%s'"
   (branch's own four strings have no git oracle -- sg's own wording,
   modeled on tag's shape.)

   The EEXIST lock-collision wording, by contrast, IS now fully shared
   (Phase 76 fix round 1) -- this REVERSES this phase's own first attempt,
   which invented an `eexist_style` split believing `git tag -d` and `git
   branch -d` printed different EEXIST wording. That belief was FALSE: the
   "'X' and 'Y' are the same ref" sentence pinned by the Phase 74 interop
   checks (case2j/case2j2) was an SG-AUTHORED literal from Phase 74 round
   1, made when sg had no lock files of its own and needed SOME wording
   for the case; it was never git's own text, and nothing had re-measured
   git's actual `tag -d` EEXIST wording since. Measured directly (real git
   2.55.0, LC_ALL=C, case-insensitive loose refs): `git tag -d Foo foo`
   prints the exact same raw lockfile.c sentence `git branch -d` does
   ("could not delete reference(s): cannot lock ref '<ref>': Unable to
   create '<abs-lock-path>': File exists.\n\nAnother git process seems to
   be running in this repository, or the lock file may be stale"), for
   BOTH an in-batch case-fold alias and a foreign stale lock alike -- there
   is no special "same ref" sentence anywhere in real git for either
   command. `sg_ref_delete_batch` now prints this one wording
   unconditionally for every EEXIST, matching both commands. */

/* Runs BEFORE existence is even tested, for every name in argv order --
   NULL means "no precheck, go straight to existence" (tag's own gate-free
   behavior; also what real git does for tag). Returns 1 to let `name`
   proceed to the existence check, 0 to reject it outright (a rejection
   must have already printed its own diagnostic; the engine does not
   report "not found" for a name a precheck rejected). */
typedef int (*sg_ref_delete_precheck_fn)(void *ctx, const char *git_dir, const char *name);

/* Returns 1 if `name` (already known to exist, at `tip`) is allowed to
   proceed into the delete transaction; 0 if it must be rejected. A
   rejection is expected to have already printed its own "sg: ..."
   diagnostic -- the engine does not print anything of its own for a gate
   rejection, only counts it as a failure for the final exit code. `ctx`
   is whatever the caller passed to sg_ref_delete_batch. Called once per
   name, in argv order, immediately after that SAME name is found to
   exist (interleaved with existence, not as a separate later pass -- see
   the precheck's own comment for why the interleaving matters: real
   git's per-name diagnostics for a mixed batch appear in strict argv
   order across ALL of precheck/existence/gate, not grouped by which check
   produced them). Never called for a name already rejected by `precheck`
   or found missing by the existence check. */
typedef int (*sg_ref_delete_gate_fn)(void *ctx, const char *git_dir, const char *name,
                                     const unsigned char tip[SG_SHA1_RAW_LEN]);

typedef struct {
    /* "refs/tags/" or "refs/heads/", trailing slash included. */
    const char *prefix;
    /* printf format embedding one %s (the name), WITHOUT a leading "sg: "
       or a trailing newline -- the engine adds both. Each caller's own
       string carries whatever git prints after that (tag's ends with '.',
       branch's does not). */
    const char *not_found_fmt;
    /* printf format embedding (name, 7-hex-prefix-of-old-tip) in that
       order, printed to stdout on a successful delete, suppressed
       entirely when `quiet` is set. */
    const char *deleted_fmt;
    /* printf format embedding one %s (the name), same "no sg:, no
       newline" convention as not_found_fmt -- printed when prefix+name
       does not fit the engine's path buffer. sg-only wording, no git
       oracle (an argv-length name this long has no measured git
       behavior to match). */
    const char *too_long_fmt;
    /* printf format embedding one %s (the name), same convention --
       printed when the actual delete syscall fails after every earlier
       check passed (should not happen in practice; sg-only wording). */
    const char *delete_fail_fmt;
    sg_ref_delete_precheck_fn precheck; /* NULL: skip straight to existence */
    void *precheck_ctx;
    sg_ref_delete_gate_fn gate; /* NULL: every existing name is eligible */
    void *gate_ctx;
    int quiet;
} sg_ref_delete_spec;

/* Runs the full Phase 74 batch-delete engine (precheck, existence, gate,
   in-batch duplicate detection, lock-collision detection, then actual
   deletion) over `names[0..count)` -- see cli/ref_delete.c for the full
   phase-by-phase commentary this was moved from. Returns 0 if every name
   was deleted, 1 if any name failed at any stage (precheck, not found,
   gate rejection, transaction collision, or a delete error) -- also 0 for
   an empty name list. */
int sg_ref_delete_batch(const char *git_dir, const char **names, int count,
                        const sg_ref_delete_spec *spec);

#endif
