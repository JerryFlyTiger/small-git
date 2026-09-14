#include "sg/ref_delete.h"

#include "sg/hash.h"
#include "sg/refs.h"
#include "sg/strfmt.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Phase 76 extraction of cmd_tag.c's Phase 74 delete_tags -- see that
   function's own history (Phase 73/74 rounds 1-4) for how this shape was
   arrived at; this file renames "tag" to a parameterized "prefix", adds a
   precheck+gate pair that tag's own call passes as NULL, and parameterizes
   the per-kind message strings. The overall pass structure is otherwise
   UNCHANGED:

     pass 1  -- precheck, existence, and gate, MERGED into one loop so a
                mixed batch's diagnostics come out in argv order across
                all three (see the loop's own comment for the measurement
                that makes this one loop rather than three)
     pass 2  -- in-batch literal-duplicate refusal (argv-equality, smallest
                name by strcmp, only among ELIGIBLE names)
     pass 2b -- lock-file collision detection (git's own mechanism: create
                refs/<prefix><name>.lock with O_CREAT|O_EXCL for every
                eligible name, in argv order, before deleting anything)
     pass 3  -- actual deletion of every name that survived all of the
                above

   See cmd_tag.c's retained comments (docs/RULES-refs-revparse.md now points
   here) for the full reasoning behind pass 2/2b/3; nothing about that
   reasoning changed, only WHERE it lives and which names are "eligible"
   (precheck && exists && gate) rather than merely "exists".

   Phase 76 fix round 1: the EEXIST branch in pass 2b used to fork on an
   `eexist_style` flag between an sg-authored "'X' and 'Y' are the same
   ref" sentence (believed to be tag's own wording) and git's raw
   lockfile.c sentence (believed to be branch-only). Both halves of that
   belief were wrong -- see ref_delete.h's own comment for the
   measurement -- so there is now exactly ONE EEXIST wording, used by both
   tag and branch. */
int sg_ref_delete_batch(const char *git_dir, const char **names, int count,
                        const sg_ref_delete_spec *spec)
{
    int i;
    int had_failure = 0;
    int *eligible;
    int existing_count;
    const char *smallest_dup = NULL;

    eligible = malloc(sizeof(*eligible) * (size_t)(count > 0 ? count : 1));
    if (eligible == NULL) {
        fprintf(stderr, "sg: out of memory\n");
        return 1;
    }

    /* Pass 1: precheck, existence, and gate -- MERGED into one loop over
       names in argv order (Phase 76: real git's own diagnostics for a
       mixed batch appear in strict argv order across all three checks,
       not grouped by which check produced them -- measured `sg branch -d
       master topic nope merged` interleaves master's worktree refusal,
       topic's not-merged refusal, and nope's not-found in exactly argv
       order, which a "resolve all existence first, THEN gate" split
       cannot reproduce). Nothing is deleted here, so this pass and every
       later one see the SAME pre-deletion state.

       Phase 76 fix round 1 (item 5): `ref_path` is built with
       sg_strfmt_alloc rather than a fixed SG_PATH_MAX buffer -- silently
       refusing a name merely because prefix+name did not fit some
       arbitrary SMALLER buffer would be inventing a tighter limit than
       this project actually has (CLAUDE.md's own bug #3, an 803-char
       branch name, is proof the per-name/per-component limit is looser
       than SG_PATH_MAX, not that there is NO limit at all -- see
       `sg_mkdir_parents`, which still caps the full path at that shared
       ceiling with an explicit -1). Only a real allocation failure is
       treated as a (still explicit, still reported) failure here. */
    for (i = 0; i < count; i++) {
        char *ref_path;
        unsigned char tip[SG_SHA1_RAW_LEN];

        eligible[i] = 0;

        /* Phase 76 fix round 5 (P1/R4-1): the FIRST gate, before the
           precheck, the existence check, or any filesystem call at all --
           a name containing a "."/".." component, an empty component, or
           a trailing '/' is rejected with the ordinary not-found message,
           the same answer git gives for every one of these spellings.
           This is a SHIPPED, PRE-EXISTING bug this fix closes, not a new
           one: on master, `sg branch -d ./merged` and `sg tag -d ./lt`
           both DELETE a ref git says does not exist at all, because the
           OS resolves "refs/heads/./merged" to "refs/heads/merged" while
           git's own lookup treats the two spellings as different names.
           Rejecting here also means `branch_aliases_current` and every
           `sg_ref_lock_try`/`sg_ref_lock_try_query` caller downstream of
           this pass ONLY EVER see an already-validated name -- see
           refs.h's own comment on the lock API for why that invariant
           matters (a `..`-laden name reaching `sg_ref_lock_try_query`
           unfiltered can build a path OUTSIDE this repository, and
           O_CREAT|O_EXCL there creates, however transiently, a real file
           there). */
        if (!sg_ref_path_components_are_safe(names[i])) {
            fprintf(stderr, "sg: ");
            fprintf(stderr, spec->not_found_fmt, names[i]);
            fputc('\n', stderr);
            had_failure = 1;
            continue;
        }
        if (spec->precheck != NULL && !spec->precheck(spec->precheck_ctx, git_dir, names[i])) {
            had_failure = 1;
            continue;
        }
        ref_path = sg_strfmt_alloc("%s%s", spec->prefix, names[i]);
        if (ref_path == NULL) {
            fprintf(stderr, "sg: ");
            fprintf(stderr, spec->too_long_fmt, names[i]);
            fputc('\n', stderr);
            had_failure = 1;
            continue;
        }
        if (sg_ref_read_path(git_dir, ref_path, tip) != 0) {
            free(ref_path);
            fprintf(stderr, "sg: ");
            fprintf(stderr, spec->not_found_fmt, names[i]);
            fputc('\n', stderr);
            had_failure = 1;
            continue;
        }
        free(ref_path);
        if (spec->gate != NULL && !spec->gate(spec->gate_ctx, git_dir, names[i], tip)) {
            had_failure = 1;
            continue;
        }
        eligible[i] = 1;
    }

    /* How many names actually ENTER git's ref transaction -- i.e. how many
       are eligible -- decides singular vs plural wording in the stray-lock
       message below (pass 2b), keyed on ELIGIBILITY, not raw argv count or
       bare existence (Phase 74 round 4's rule, generalized the same way
       existence -> eligibility is generalized everywhere else in this
       function). */
    existing_count = 0;
    for (i = 0; i < count; i++) {
        if (eligible[i])
            existing_count++;
    }

    /* Pass 2: a repeated name only triggers the transaction refusal when it
       is ELIGIBLE -- a repeated name that is missing, or that a gate
       rejected, already got its own diagnostic above and blocks nothing
       here. */
    for (i = 0; i < count; i++) {
        int j;

        if (!eligible[i])
            continue;
        for (j = i + 1; j < count; j++) {
            if (!eligible[j] || strcmp(names[i], names[j]) != 0)
                continue;
            if (smallest_dup == NULL || strcmp(names[i], smallest_dup) < 0)
                smallest_dup = names[i];
            break;
        }
    }
    if (smallest_dup != NULL) {
        fprintf(stderr,
               "sg: could not delete references: multiple updates for ref '%s%s' not allowed\n",
               spec->prefix, smallest_dup);
        free(eligible);
        return 1;
    }

    /* Pass 2b: reproduce git's own per-ref lock mechanism as a pure
       collision detector (see cmd_tag.c's Phase 74 round 2/3 comments for
       the full reasoning -- unchanged here beyond prefix and "eligible"
       replacing "exists"). Phase 76 fix round 3 (L1): the actual
       O_CREAT|O_EXCL acquire/release moved to the shared
       `sg_ref_lock_try`/`sg_ref_lock_release` (refs.h), used identically
       by cmd_branch.c's create-path lock and its alias probe -- this
       function's own messages and control flow are UNCHANGED, only the
       mechanism producing `lock.path`/the acquired/EEXIST/ERROR
       distinction moved out from under it (interop's phase73/74/76 groups
       pin this file's output byte-for-byte, and stayed green across this
       move). */
    {
        sg_ref_lock *locks;
        int lock_count = 0;
        int collision = 0;

        locks = malloc(sizeof(*locks) * (size_t)(count > 0 ? count : 1));
        if (locks == NULL) {
            fprintf(stderr, "sg: out of memory\n");
            free(eligible);
            return 1;
        }

        for (i = 0; i < count && !collision; i++) {
            sg_ref_lock lock;
            sg_ref_lock_result rc;

            if (!eligible[i])
                continue;
            rc = sg_ref_lock_try(git_dir, spec->prefix, names[i], &lock);
            if (rc == SG_REFLOCK_ACQUIRED) {
                locks[lock_count] = lock;
                lock_count++;
                continue;
            }
            if (rc == SG_REFLOCK_EEXIST) {
                /* git's own raw lockfile.c wording, embedding the
                   absolute lock path we just tried to create -- measured
                   verbatim against real git 2.55.0 for BOTH `git tag -d`
                   and `git branch -d` (Phase 76 fix round 1), including
                   the blank line before the "Another git process" hint.
                   Identical whether the collision is an in-batch
                   case-fold alias or a foreign stale lock -- git does not
                   distinguish the two in its wording, and neither does
                   this. */
                if (existing_count == 1) {
                    fprintf(stderr,
                            "sg: could not delete reference %s%s: cannot lock ref '%s%s': "
                            "Unable to create '%s': File exists.\n\n"
                            "Another git process seems to be running in this repository, or "
                            "the lock file may be stale\n",
                            spec->prefix, names[i], spec->prefix, names[i], lock.path);
                } else {
                    fprintf(stderr,
                            "sg: could not delete references: cannot lock ref '%s%s': "
                            "Unable to create '%s': File exists.\n\n"
                            "Another git process seems to be running in this repository, or "
                            "the lock file may be stale\n",
                            spec->prefix, names[i], lock.path);
                }
            } else if (lock.path == NULL) {
                fprintf(stderr, "sg: out of memory\n");
            } else if (lock.mkdir_failed) {
                fprintf(stderr, "sg: failed to lock ref '%s%s': could not create lock directory\n",
                        spec->prefix, names[i]);
            } else {
                fprintf(stderr, "sg: failed to lock ref '%s%s': %s\n", spec->prefix, names[i],
                        strerror(lock.open_errno));
            }
            sg_ref_lock_release(&lock);
            collision = 1;
        }

        for (i = 0; i < lock_count; i++)
            sg_ref_lock_release(&locks[i]);
        free(locks);

        if (collision) {
            free(eligible);
            return 1;
        }
    }

    /* Pass 3: no transaction conflict -- actually delete every eligible
       name. Re-deriving the ref path and re-reading the old id is not a
       check-then-use gap: nothing has been deleted by any earlier pass. */
    for (i = 0; i < count; i++) {
        const char *name = names[i];
        char *ref_path;
        unsigned char old_id[SG_SHA1_RAW_LEN];
        char old_hex[SG_SHA1_HEX_LEN + 1];
        int rc;

        if (!eligible[i])
            continue;

        ref_path = sg_strfmt_alloc("%s%s", spec->prefix, name);
        if (ref_path == NULL) {
            /* Phase 76 fix round 2 (R4): distinct from "the ref does not
               exist" -- this is an allocation failure on the SAME name
               pass 1 already read successfully moments ago, not a
               not-found condition, and must not be reported as one. */
            fprintf(stderr, "sg: out of memory\n");
            had_failure = 1;
            continue;
        }
        if (sg_ref_read_path(git_dir, ref_path, old_id) != 0) {
            free(ref_path);
            fprintf(stderr, "sg: ");
            fprintf(stderr, spec->not_found_fmt, name);
            fputc('\n', stderr);
            had_failure = 1;
            continue;
        }
        free(ref_path);
        rc = sg_ref_delete_under(git_dir, spec->prefix, name);
        if (rc != 0) {
            fprintf(stderr, "sg: ");
            fprintf(stderr, spec->delete_fail_fmt, name);
            fputc('\n', stderr);
            had_failure = 1;
            continue;
        }
        /* Phase 76 fix round 4 (D1d): git prunes now-empty ancestor
           directories after a delete (measured: `git branch -d heads/x`
           removes refs/heads/heads/ AND logs/refs/heads/heads/, plain
           rmdir semantics -- never recurses into a still-populated
           sibling, stops at the FIRST non-empty ancestor, and never
           touches refs/heads/ or refs/tags/ themselves). sg's own
           sg_ref_delete_under left both empty, which is how a name this
           project's own delete engine emptied out could later poison a
           create at the SAME path (see docs/DESIGN.md's Phase 76 D1a
           residual). Reuses sg_prune_empty_parents (workdir.h) rather
           than a second pruner -- it already has exactly this stopping
           rule (rmdir until the first failure) for the working tree, and
           the "repo_root" it stops AT is simply set to the namespace
           root here (<git_dir>/refs/heads or <git_dir>/refs/tags, not
           the worktree root) so it can never remove that either. The two
           chains (ref file, reflog) are pruned INDEPENDENTLY -- measured,
           they can genuinely diverge (an empty sibling directory under
           refs/ that logs/ does not have, or vice versa). */
        {
            char *ref_ns_root = sg_strfmt_alloc("%s/%.*s", git_dir, (int)strlen(spec->prefix) - 1,
                                                spec->prefix);
            char *log_ns_root = sg_strfmt_alloc("%s/logs/%.*s", git_dir, (int)strlen(spec->prefix) - 1,
                                                spec->prefix);

            if (ref_ns_root != NULL)
                sg_prune_empty_parents(ref_ns_root, name);
            if (log_ns_root != NULL)
                sg_prune_empty_parents(log_ns_root, name);
            free(ref_ns_root);
            free(log_ns_root);
        }
        sg_sha1_to_hex(old_id, old_hex);
        if (!spec->quiet) {
            printf(spec->deleted_fmt, name, old_hex);
        }
    }

    free(eligible);
    return had_failure ? 1 : 0;
}
