#include "sg/cli.h"

#include "sg/cli_args.h"
#include "sg/hash.h"
#include "sg/merge.h"
#include "sg/object.h"
#include "sg/ref_delete.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/revparse.h"
#include "sg/strfmt.h"
#include "sg/workdir.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char USAGE[] =
    "usage: sg branch [--force|-f] [--] [<name> [<start-point>]]\n"
    "       sg branch (-d|--delete|-D) [-f] [-q] [--] <name>...\n";

static int list_branches(const char *git_dir)
{
    char **names;
    size_t count;
    size_t i;
    char *current;

    if (sg_ref_list_branches(git_dir, &names, &count) != 0) {
        fprintf(stderr, "sg: cannot list branches\n");
        return 1;
    }

    current = sg_ref_current_branch(git_dir);

    /* A detached HEAD is listed as its own starred pseudo-entry ahead of the
       real branches, so that `sg branch` never shows an unstarred list that
       looks like "no branch is checked out". Sorted first by real git too,
       which puts it before the alphabetical branches rather than in them. */
    if (current == NULL) {
        char detached[4160]; /* fits any ref path sg can build (SG_PATH_MAX) plus the wording */

        if (sg_ref_head_is_detached(git_dir) == 1 &&
           sg_ref_detach_description(git_dir, detached, sizeof(detached)) == 0)
            printf("* (%s)\n", detached);
        else
            printf("* (no branch)\n");
    }

    for (i = 0; i < count; i++) {
        int is_current = current != NULL && strcmp(names[i], current) == 0;

        printf("%s %s\n", is_current ? "*" : " ", names[i]);
        free(names[i]);
    }
    free(names);
    free(current);
    return 0;
}

/* Absolute path to the worktree root, used only by the two "used by
   worktree at '<path>'" messages below -- sg has no `git worktree` of its
   own, so this is always the single primary worktree containing git_dir.
   Returned string is malloc'd.

   Phase 76 fix round 3 (M11 decision): this used to also call realpath()
   on sg_repo_root's answer, with a fallback to the unresolved path on
   failure. Deleted as a REDUNDANT GUARD, not merely inert: `git_dir`
   itself is already fully resolved by the time ANY caller reaches this
   function, because `sg_find_git_dir` (storage/repo.c) obtains the
   repository root via a single `getcwd()` call, and POSIX `getcwd()`
   already returns the kernel's canonicalized (symlink-free) path, not a
   shell-style logical `$PWD` -- so a second realpath() one layer up could
   never see a different answer, from the repo root, a subdirectory, or a
   path reached through a symlink alike (measured directly, Phase 76
   round 2/3: mutating the realpath call away stayed fully green under
   root/subdirectory/symlinked-cwd interop checks that assert the FULL
   message line, path included). Round 2 left the redundant call in
   place out of caution; CLAUDE.md's own rule for a redundant guard (the
   real defense is one layer down) is to delete it, since a line that can
   never change the answer invites a future reader to believe it guards
   something. See docs/DESIGN.md's Phase 76 section for the full proof and
   -- importantly -- the CONDITION under which this stops holding: the day
   sg gains `GIT_DIR`, a `-C` flag, or `.git`-as-a-file (gitdir/worktree)
   support, `sg_find_git_dir` will no longer be the only source of
   `git_dir`, and a symlinked-path fixture must be re-run before trusting
   this comment again. */
static char *worktree_root_display(const char *git_dir)
{
    return sg_repo_root(git_dir);
}

/* Tri-state: is_loose_branch_ref used to collapse "allocation failure" into
   the same 0 as "genuinely not loose" (Phase 76 fix round 2, item R2) --
   branch_aliases_current then read that 0 as "packed-only, cannot alias"
   and let a dangerous force-update/delete through on the strength of an
   OOM, exactly the fail-OPEN direction docs/RULES-paths-strings.md's own
   OOM rule forbids. SG_LOOSE_ERROR lets a caller that cares (only
   branch_aliases_current does) fail CLOSED instead; check_df_conflict's
   two call sites still only need a wording choice AFTER a conflict has
   already been proven to exist, so they fold SG_LOOSE_ERROR into "not
   loose" for that purely cosmetic decision (see their own comment). */
typedef enum {
    SG_LOOSE_NO = 0,
    SG_LOOSE_YES = 1,
    SG_LOOSE_ERROR = -1,
} sg_loose_check;

/* Whether refs/heads/<name> (relative to git_dir) is a LOOSE ref file --
   used both to pick between the D/F conflict's two wordings (see
   check_df_conflict's own comment: the "cannot lock ref '<new>': " prefix
   only appears when the EXISTING, conflicting ref is loose; a packed-only
   conflict is detected by a different code path in real git that never
   attempts, and so never reports failing, a lock) and to gate
   branch_aliases_current (a packed-only current branch cannot alias a
   differently-cased spelling, since packed-refs lookup is an exact string
   match unaffected by filesystem case-folding).

   Phase 76 fix round 1 (item 5): built with sg_strfmt_alloc rather than a
   fixed SG_PATH_MAX buffer -- the old overflow fallback silently answered
   "not loose" for a name too long to fit that buffer, which is a SILENT
   WRONG ANSWER (it would pick the packed-only wording for a name that is
   actually loose), not a length limit git itself has. */
static sg_loose_check is_loose_branch_ref(const char *git_dir, const char *name)
{
    char *path = sg_strfmt_alloc("%s/refs/heads/%s", git_dir, name);
    struct stat st;
    sg_loose_check result;

    if (path == NULL)
        return SG_LOOSE_ERROR;
    result = (stat(path, &st) == 0 && S_ISREG(st.st_mode)) ? SG_LOOSE_YES : SG_LOOSE_NO;
    free(path);
    return result;
}

/* Phase 76 fix round 2 (deliberate divergence #10): whether `name`
   ALIASES the CURRENT branch on this filesystem (resolves to the same
   loose ref file after OS-level case-folding OR Unicode normalization --
   measured, see docs/DESIGN.md's Phase 76 K1 section: with
   `core.precomposeUnicode` false or unset, an NFD-spelled argv name
   aliases an NFC-named checked-out branch on APFS the identical way a
   differently-cased spelling does) without being textually equal to it.
   Measured against real git
   2.55.0: `git branch -d Master` (current branch "master",
   case-insensitive FS) exits 0 and deletes it, leaving HEAD dangling --
   sg's own decision is to REFUSE this instead (a refusal is a strictly
   safer answer than leaving HEAD pointing at nothing), which needs a way
   to detect the alias that is neither `strcasecmp` (wrong on a
   case-SENSITIVE filesystem, and does not fold Unicode normalization
   forms at all) nor an inode compare of the ref files themselves (Phase
   74 round 1 measured that wrong in both directions -- see
   ref_delete.c's own history for the same reasoning applied to a
   different collision).

   Instead this asks the filesystem the SAME question git's own
   ref-transaction lock would: does creating `refs/heads/<current>.lock`
   and then `refs/heads/<name>.lock` (both O_CREAT|O_EXCL) collide? Every
   lock this creates is removed again before returning, on every path.
   Only meaningful when `current` is backed by a LOOSE ref file (see
   is_loose_branch_ref's own comment on why a packed-only current branch
   is excluded before this is even called). If the current branch's OWN
   lock is already held by someone else (a foreign stale lock, unrelated
   to this call, measured via the oracle's own `stale` fixture), this
   cannot safely probe at all and falls through as "not aliased" rather
   than guessing -- the same "cannot safely tell" precedent
   ref_delete.c's own stale-lock handling already uses; this is a
   documented AMBIGUOUS case, not a resource failure, so it is NOT part
   of the tri-state error return below.

   Returns 1 (aliased), 0 (not aliased, including the foreign-stale-lock
   fallthrough above), or -1 on a genuine resource failure (allocation or
   mkdir) -- Phase 76 fix round 2 (R2): every one of THESE failures used
   to collapse into 0 ("not aliased"), which is fail-OPEN (a force-update
   or delete of the checked-out branch would go through on the strength
   of an OOM). Both callers now fail CLOSED on -1 instead of reading it as
   "no alias". */
static int branch_aliases_current(const char *git_dir, const char *current, const char *name)
{
    sg_ref_lock cur_lock, typed_lock;
    sg_ref_lock_result cur_rc, typed_rc;
    int aliases = 0;

    if (strcmp(current, name) == 0)
        return 0;
    switch (is_loose_branch_ref(git_dir, current)) {
    case SG_LOOSE_ERROR:
        return -1;
    case SG_LOOSE_NO:
        return 0;
    case SG_LOOSE_YES:
        break;
    }

    /* Phase 76 fix round 4 (R3-1/D1b/D1c): both locks use the PURE-QUERY
       variant (sg_ref_lock_try_query) -- this probe must never create a
       directory on disk, since it runs for every batch-delete name
       regardless of whether that name even exists. Round 3's own fix
       (sg_ref_lock_try, which DOES mkdir) leaked an empty
       "refs/heads/<name>/" for any nonexistent nested name being deleted
       (measured: `sg branch -d nope/sub` -- correctly "not found" -- also
       planted an empty refs/heads/nope/), and separately mis-took
       "sg: out of memory" for `-d merged/sub`/`-d heads/x/y` (a SIBLING
       component that is itself an existing loose ref FILE, not a
       directory): sg_mkdir_parents tolerates EEXIST on the parent
       WITHOUT checking S_ISDIR, so it reports success, and the
       subsequent open() then fails with ENOTDIR -- which round 3's own
       R2-2 fix (correctly, for every OTHER errno) turned into a fail-
       closed ERROR. The right classification for ENOENT/ENOTDIR
       specifically is "this spelling cannot resolve to anything on
       disk", i.e. clean "not aliased" -- see sg_ref_lock_try_query's own
       header comment for why that decision belongs to this ONE caller,
       not to the lock primitive itself.

       The CURRENT branch's own lock also uses the query variant now:
       `current` is already confirmed to be an existing loose ref
       (is_loose_branch_ref above), so its directory chain necessarily
       already exists and mkdir_parents there was always a no-op --
       switching it to the query variant changes nothing observable and
       keeps this whole probe consistently "creates nothing, ever". */
    cur_rc = sg_ref_lock_try_query(git_dir, "refs/heads/", current, &cur_lock);
    if (cur_rc == SG_REFLOCK_EEXIST) {
        /* Foreign stale lock on the CURRENT branch's own path -- ambiguous,
           not an error, same documented fall-through as before. */
        sg_ref_lock_release(&cur_lock);
        return 0;
    }
    if (cur_rc == SG_REFLOCK_ERROR) {
        /* Phase 76 fix round 2/3 (R2-2): ANY non-EEXIST failure on this
           FIRST lock (allocation or a non-EEXIST open() errno) fails
           CLOSED -- unreachable in practice (see above: the directory is
           already known to exist), kept for the same defense-in-depth
           reason round 2/3 argued for it. */
        sg_ref_lock_release(&cur_lock);
        return -1;
    }

    typed_rc = sg_ref_lock_try_query(git_dir, "refs/heads/", name, &typed_lock);
    if (typed_rc == SG_REFLOCK_ERROR &&
       (typed_lock.open_errno == ENOENT || typed_lock.open_errno == ENOTDIR)) {
        /* Phase 76 fix round 4: the typed spelling cannot resolve to
           anything on disk at all (a missing parent directory, or a
           SIBLING path component that is itself a regular file) -- clean
           "not aliased", not a resource failure. */
        sg_ref_lock_release(&cur_lock);
        sg_ref_lock_release(&typed_lock);
        return 0;
    }
    if (typed_rc == SG_REFLOCK_EEXIST) {
        /* EEXIST here has TWO different causes that must not be confused
           (measured via the oracle's own `stale` fixture, which plants an
           unrelated foreign .lock on a name that does NOT alias the
           current branch at all): a TRUE filesystem alias, where this
           path and cur_lock's path are literally the SAME FILE on a
           case-insensitive/normalization-folding FS (same st_dev/st_ino);
           or a genuinely different, pre-existing foreign lock that just
           happens to sit at the typed path, unrelated to the current
           branch. Only the former counts as aliasing. */
        struct stat cur_st, typed_st;

        if (stat(cur_lock.path, &cur_st) == 0 && stat(typed_lock.path, &typed_st) == 0 &&
           cur_st.st_dev == typed_st.st_dev && cur_st.st_ino == typed_st.st_ino)
            aliases = 1;
        sg_ref_lock_release(&typed_lock);
    } else if (typed_rc == SG_REFLOCK_ACQUIRED) {
        sg_ref_lock_release(&typed_lock);
    } else {
        /* Phase 76 fix round 2 (R2-2): the SECOND open()'s non-EEXIST,
           non-ENOENT/ENOTDIR failure used to fall through as "not
           aliased" (aliases stayed 0) -- the exact fail-open direction
           round 2 claimed it had already closed. Fails CLOSED here too. */
        sg_ref_lock_release(&cur_lock);
        sg_ref_lock_release(&typed_lock);
        return -1;
    }
    sg_ref_lock_release(&cur_lock);
    return aliases;
}

/* Combines the exact-name compare with branch_aliases_current into the
   single tri-state both create's force-gate and delete's precheck need:
   1 (this name IS the checked-out branch, exactly or by alias), 0 (it is
   not), -1 (could not determine, a resource failure -- caller must fail
   CLOSED, see branch_aliases_current's own comment for why). Factored out
   so the two call sites cannot independently drift on how they combine
   the strcmp with the alias probe or on which failure direction to take. */
static int branch_is_current_or_alias(const char *git_dir, const char *current, const char *name)
{
    int alias_rc;

    if (strcmp(current, name) == 0)
        return 1;
    alias_rc = branch_aliases_current(git_dir, current, name);
    if (alias_rc < 0)
        return -1;
    return alias_rc ? 1 : 0;
}

static void report_df_conflict(const char *new_name, const char *existing_name, int existing_is_loose)
{
    if (existing_is_loose) {
        fprintf(stderr,
               "sg: cannot lock ref 'refs/heads/%s': 'refs/heads/%s' exists; cannot create "
               "'refs/heads/%s'\n",
               new_name, existing_name, new_name);
    } else {
        fprintf(stderr, "sg: 'refs/heads/%s' exists; cannot create 'refs/heads/%s'\n", existing_name,
                new_name);
    }
}

/* A directory/file (D/F) conflict in the refs/heads/ namespace: creating
   `name` would either need an existing LEAF ref as one of its own path
   components (forward direction, e.g. creating "merged/sub" when "merged"
   already exists), or would itself need to become a directory containing
   an existing ref (reverse direction, e.g. creating "heads" when
   "heads/x" already exists). Measured against real git 2.55.0 in both
   directions and both loose/packed combinations (Phase 76). Only relevant
   when `name` does not already exist as a leaf ref itself -- the caller is
   expected to have already handled that case. Prints its own diagnostic
   and returns 1 on a conflict, 0 if clear. */
static int check_df_conflict(const char *git_dir, const char *name)
{
    size_t len = strlen(name);
    size_t i;
    char **names;
    size_t count;
    char *nested_prefix;
    int found = 0;

    /* Phase 76 fix round 1 (item 5): every ancestor component of `name`
       is checked (not just "one level up or down" -- docs/sg.1 used to
       undersell this), and each candidate prefix is a heap allocation
       sized exactly to fit, not a fixed SG_PATH_MAX buffer -- the old
       `if (i >= sizeof(prefix)) continue;` SILENTLY skipped checking a
       component once the name got long enough, which could let a real
       D/F conflict through uncaught. Phase 76 fix round 2 (R4): this is
       NOT "no length limit exists" -- `sg_mkdir_parents`
       (`workdir.c:sg_mkdir_parents`) still caps the full path it builds
       at the shared `SG_PATH_MAX` ceiling and returns an explicit -1
       there (CLAUDE.md's own bug #3, an 803-char branch name, only shows
       there is no PER-COMPONENT or per-name limit shorter than that). The
       point this fix actually makes is narrower: no SILENT truncation
       before that shared ceiling -- an allocation failure here is treated
       as "cannot safely rule out a conflict" and fails closed, the same
       direction ref_delete.c's own OOM paths already take. */
    for (i = 0; i < len; i++) {
        char *prefix;

        if (name[i] != '/')
            continue;
        prefix = malloc(i + 1);
        if (prefix == NULL) {
            fprintf(stderr, "sg: out of memory\n");
            return 1;
        }
        memcpy(prefix, name, i);
        prefix[i] = '\0';
        if (sg_ref_branch_exists(git_dir, prefix)) {
            report_df_conflict(name, prefix, is_loose_branch_ref(git_dir, prefix) == SG_LOOSE_YES);
            free(prefix);
            return 1;
        }
        /* Phase 76 fix round 4 tried detecting a PACKED-only, case-folded
           D/F conflict here by checking whether logs/refs/heads/<prefix>
           exists as a file (a reflog, which is never packed) -- REMOVED
           in round 5 (Q2): measured against real git 2.55.0 with the
           branch's reflog file deleted (a real, reachable state --
           `core.logAllRefUpdates=false`, or simply an older repo git
           itself created without one), git STILL refuses via the SAME
           D/F wording, keyed on `core.ignorecase`, not on whether a
           reflog file happens to exist. The reflog heuristic agreed with
           git only on the one fixture it was measured against and
           silently let a real conflict through everywhere else (sg
           created the ref where git refuses) -- see docs/DESIGN.md's
           Phase 76 round-5 Q2 section for the measured table and the
           residual this leaves (sg has no `core.ignorecase` reader; a
           one-off check here would be inventing a second, narrower
           mechanism instead of implementing git's actual one). */
        free(prefix);
    }

    nested_prefix = sg_strfmt_alloc("%s/", name);
    if (nested_prefix == NULL) {
        fprintf(stderr, "sg: out of memory\n");
        return 1;
    }
    if (sg_ref_list_under(git_dir, "refs/heads/", &names, &count) != 0) {
        free(nested_prefix);
        return 0;
    }
    for (i = 0; i < count && !found; i++) {
        if (strncmp(names[i], nested_prefix, strlen(nested_prefix)) == 0) {
            report_df_conflict(name, names[i], is_loose_branch_ref(git_dir, names[i]) == SG_LOOSE_YES);
            found = 1;
        }
    }
    for (i = 0; i < count; i++)
        free(names[i]);
    free(names);
    free(nested_prefix);
    return found;
}

/* Resolves the <start-point> (or its default) to a commit id, matching
   real git's own STRICT-mode messages byte for byte (Phase 76, measured
   against git 2.55.0 -- see the "branch" row in cli_args.c's REV_ERR_TABLE
   for the R/O/P classes this reuses, and this function's own comment for
   the one class the table cannot express). Returns 0 with commit_id_out
   filled in; -1 after already printing a diagnostic. */
static int resolve_branch_point(const char *git_dir, const char *start_str,
                                unsigned char commit_id_out[SG_SHA1_RAW_LEN])
{
    unsigned char id[SG_SHA1_RAW_LEN];
    sg_obj_type type;
    char bad_path[SG_PATH_MAX];
    int prc;

    /* sg_rev_parse_commit_ex already peels an annotated tag chain to its
       commit and applies the full ~/^/@{N} grammar -- this succeeds for
       every case except "resolves to a non-commit object" and "does not
       resolve at all", which is exactly the split the code below exists
       to classify. */
    if (sg_rev_parse_commit_ex(git_dir, start_str, SG_REV_STRICT, commit_id_out) == 0)
        return 0;

    bad_path[0] = '\0';
    prc = sg_rev_parse_object(git_dir, start_str, id, &type, bad_path, sizeof(bad_path));
    if (prc == 0 && type != SG_OBJ_COMMIT && type != SG_OBJ_TAG) {
        /* Resolves, but to a tree or blob -- git's own two-line wording,
           not expressible as a REV_ERR_TABLE row (see that row's own
           comment). */
        char hex[SG_SHA1_HEX_LEN + 1];

        sg_sha1_to_hex(id, hex);
        fprintf(stderr, "sg: object %s is a %s, not a commit\n", hex, sg_obj_type_name(type));
        fprintf(stderr, "sg: not a valid branch point: '%s'\n", start_str);
        return -1;
    }
    if (prc == -4) {
        sg_cli_report_ambiguous_oid(git_dir, start_str, SG_REV_STRICT);
        sg_cli_report_rev_error("branch", SG_REV_ERR_NOT_A_REV, start_str, NULL, 0);
        return -1;
    }
    {
        char bp[SG_PATH_MAX];
        sg_rev_err_kind kind;

        bp[0] = '\0';
        kind = sg_cli_classify_rev_error(git_dir, start_str, bp, sizeof(bp));
        sg_cli_report_rev_error("branch", kind, start_str, bp, 0);
    }
    return -1;
}

static int create_branch(const char *git_dir, const char *name, const char *start_arg, int force)
{
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    char ref_path[SG_PATH_MAX];
    char *current;
    const char *start_str;
    char *reflog_msg;
    int already_exists;
    int rc;

    if (!sg_ref_name_valid_for_create(name)) {
        fprintf(stderr, "sg: '%s' is not a valid branch name\n", name);
        return 1;
    }

    already_exists = sg_ref_branch_exists(git_dir, name);
    if (already_exists && !force) {
        fprintf(stderr, "sg: a branch named '%s' already exists\n", name);
        return 1;
    }

    current = sg_ref_current_branch(git_dir);
    /* Deliberate divergence #10 (Phase 76 fix round 2): a case- or
       Unicode-normalization-aliased spelling of the checked-out branch is
       refused too, not just an exact-name match -- see
       branch_is_current_or_alias's own comment. A -1 (resource failure)
       fails CLOSED with its own diagnostic, never silently as "no
       alias". */
    if (force && already_exists && current != NULL) {
        int m = branch_is_current_or_alias(git_dir, current, name);

        if (m < 0) {
            fprintf(stderr, "sg: out of memory\n");
            free(current);
            return 1;
        }
        if (m) {
            char *wt = worktree_root_display(git_dir);

            fprintf(stderr, "sg: cannot force update the branch '%s' used by worktree at '%s'\n", name,
                    wt != NULL ? wt : "?");
            free(wt);
            free(current);
            return 1;
        }
    }

    /* Default start point: the CURRENT branch's own name (not the literal
       "HEAD"), or "HEAD" itself on a detached checkout -- measured to
       matter on an unborn HEAD, where the error names whichever of the two
       git actually tried to resolve ("not a valid object name: 'master'"
       vs "...: 'HEAD'"). This same string is reused for resolution, for
       any error message, and for the reflog line below. */
    start_str = start_arg != NULL ? start_arg : (current != NULL ? current : "HEAD");

    if (resolve_branch_point(git_dir, start_str, commit_id) != 0) {
        free(current);
        return 1;
    }

    if (!already_exists && check_df_conflict(git_dir, name)) {
        free(current);
        return 1;
    }

    if (snprintf(ref_path, sizeof(ref_path), "refs/heads/%s", name) >= (int)sizeof(ref_path)) {
        fprintf(stderr, "sg: branch name too long\n");
        free(current);
        return 1;
    }

    /* Measured against real git 2.55.0: creating logs "Created from
       <start-point>", force-resetting an EXISTING branch logs "Reset to
       <start-point>" instead -- both using the start point exactly as
       typed (or the same default derived above). sg_ref_update's own
       old-vs-new suppression (see refs.h) already matches git's "no-op
       force update logs nothing" behavior, so there is no separate check
       for that here. */
    reflog_msg = sg_strfmt_alloc(already_exists ? "branch: Reset to %s" : "branch: Created from %s",
                                 start_str);
    free(current);
    if (reflog_msg == NULL) {
        fprintf(stderr, "sg: out of memory\n");
        return 1;
    }

    /* Phase 76 fix round 3 (L1): the create/-f write holds
       refs/heads/<name>.lock (name AS TYPED) as the LAST check before
       touching the ref -- measured check order against real git 2.55.0:
       name validity -> exists-without-force -> exact-name worktree
       refusal -> start-point resolution -> D/F conflict -> LOCK -> write.
       A foreign lock here (planted by a crashed process or another tool)
       must refuse the write with git's own wording and leave that lock
       untouched -- this closes a real hole divergence #10's alias probe
       cannot: `-f Master HEAD~1` with a foreign `refs/heads/master.lock`
       used to move the checked-out `master` anyway (round 2's own
       flagged-but-unfixed measurement), because the alias probe's documented
       "cannot safely tell" fall-through let the name through to a create
       path that had no lock of its own. Real git refuses this cell too
       (`fatal: cannot lock ref ...`), so closing it is plain git parity,
       not a NEW divergence -- divergence #10 itself is still exactly the
       case with NO foreign lock present, where this lock always succeeds
       and the alias probe above is what refuses. */
    {
        sg_ref_lock lock;
        sg_ref_lock_result lrc = sg_ref_lock_try(git_dir, "refs/heads/", name, &lock);

        if (lrc == SG_REFLOCK_EEXIST) {
            fprintf(stderr,
                    "sg: cannot lock ref 'refs/heads/%s': Unable to create '%s': File exists.\n\n"
                    "Another git process seems to be running in this repository, or the lock "
                    "file may be stale\n",
                    name, lock.path);
            sg_ref_lock_release(&lock);
            free(reflog_msg);
            return 1;
        }
        if (lrc == SG_REFLOCK_ERROR) {
            if (lock.path == NULL)
                fprintf(stderr, "sg: out of memory\n");
            else if (lock.mkdir_failed)
                fprintf(stderr, "sg: failed to lock ref 'refs/heads/%s': could not create lock directory\n",
                        name);
            else
                fprintf(stderr, "sg: failed to lock ref 'refs/heads/%s': %s\n", name,
                        strerror(lock.open_errno));
            sg_ref_lock_release(&lock);
            free(reflog_msg);
            return 1;
        }

        /* Phase 76 fix round 5 (Q1): a force-update to the value the
           branch ALREADY has must still take the lock FIRST -- measured
           against real git 2.55.0: with a foreign `refs/heads/
           topic.lock` present, `git branch -f topic topic` (topic
           already at that value) exits 128 with git's own lock-collision
           wording, identical on a loose or packed `topic`. Round 4's own
           no-op short-circuit sat BEFORE this lock (returning 0 without
           ever trying to acquire it), so a foreign lock never got a
           chance to refuse -- confirmed by the oracle to diverge from
           git there (sg exited 0). The short-circuit itself is
           unchanged otherwise: still no write, still no reflog line,
           the lock is acquired only to prove nothing else holds it, then
           released without ever calling sg_ref_update. */
        if (already_exists) {
            unsigned char existing_tip[SG_SHA1_RAW_LEN];

            if (sg_ref_read_branch(git_dir, name, existing_tip) == 0 &&
               memcmp(existing_tip, commit_id, SG_SHA1_RAW_LEN) == 0) {
                sg_ref_lock_release(&lock);
                free(reflog_msg);
                return 0;
            }
        }

        rc = sg_ref_update(git_dir, ref_path, commit_id, reflog_msg);
        sg_ref_lock_release(&lock);
    }
    free(reflog_msg);
    if (rc != 0) {
        fprintf(stderr, "sg: cannot create branch '%s'\n", name);
        /* Phase 76 fix round 5 (Q2 floor requirement): a FAILED write must
           leave NO empty directory behind, even though sg does not yet
           detect every D/F conflict up front (see check_df_conflict's own
           comment on the packed/case-folding residual this leaves --
           sg has no core.ignorecase reader). The create-path lock above
           already creates refs/heads/<name>'s parent directories before
           the write is even attempted (needed for the ordinary success
           case), so a failure partway through -- this project's own
           EEXIST-without-S_ISDIR gap in sg_mkdir_parents lets a
           case-folded ancestor look like a pre-existing directory when it
           is really a colliding FILE, see the R3-1/D1c/D1e sections of
           docs/DESIGN.md -- can leave that freshly-created directory
           behind with nothing in it. Reusing sg_prune_empty_parents
           (workdir.h, same helper D1d's delete-side pruning uses) cleans
           up both the ref and reflog directory chains.

           Phase 76 fix round 6/7 (see docs/DESIGN.md's Phase 76 F1a
           section for the full measured table): the previous version of
           this comment claimed the prune "is a no-op for every OTHER
           failure reason, since those never create a directory that
           stays empty" -- that is only true for the failure paths
           actually measured, and even among those, the directory
           REMOVAL only happens for ONE of the three kinds. Measured
           against real git 2.55.0, per failure kind: for a D/F conflict,
           git removes a pre-existing empty directory that sits exactly
           on the path it was trying to use, even though it then refuses
           the create (it does not recurse into an empty CHILD, so a
           non-empty ancestor survives). A bad start-point and a lock
           EEXIST touch NOTHING -- git never reaches the directory-
           clearing step because it fails before any path work or at the
           lock. sg's own prune call here matches all three: it removes
           the same pre-existing empty directory on a D/F failure (the
           call reaches an empty ancestor and rmdir succeeds), and is a
           genuine no-op on a start-point or lock failure (sg's own early
           returns for those never reach this prune call at all). What is
           NOT covered: sg_ref_update (storage/refs.c) appends the reflog
           line BEFORE writing the ref file, and on a ref-write failure
           only truncates that append back (sg_reflog_truncate), it never
           unlinks a reflog file it just created. For a nested name like
           sub/leaf, if the reflog append succeeds and the ref write then
           fails for an I/O reason (disk full, EACCES -- not a D/F
           conflict), sg_prune_empty_parents's rmdir of logs/refs/heads/sub
           fails because a 0-byte leaf file is still sitting in it: a
           directory AND a 0-byte reflog file survive this call. The
           prune also has no memory of which directories THIS call
           created, so in that same scenario it can remove a PRE-EXISTING
           empty ancestor that had nothing to do with this call. No
           fixture reaches this ordering without fault injection, and
           git's own behaviour in this exact scenario has not been
           measured; see docs/DESIGN.md's Phase 76 F1a section. */
        {
            char *ref_ns_root = sg_strfmt_alloc("%s/refs/heads", git_dir);
            char *log_ns_root = sg_strfmt_alloc("%s/logs/refs/heads", git_dir);

            if (ref_ns_root != NULL)
                sg_prune_empty_parents(ref_ns_root, name);
            if (log_ns_root != NULL)
                sg_prune_empty_parents(log_ns_root, name);
            free(ref_ns_root);
            free(log_ns_root);
        }
        return 1;
    }
    return 0;
}

/* Branch X is provably merged iff merge_base(HEAD, X) == tip(X) (X is an
   ancestor of HEAD, or HEAD itself). Everything unprovable -- unrelated
   history, criss-cross bases, unresolvable HEAD -- counts as NOT merged and
   is refused outright (Phase 76: git REFUSES here, it does not prompt --
   see the delete gate below). Returns 1 merged, 0 not provably merged, -1
   on a real error. */
static int branch_is_merged(const char *git_dir, const unsigned char tip[SG_SHA1_RAW_LEN])
{
    unsigned char head_id[SG_SHA1_RAW_LEN];
    unsigned char base[SG_SHA1_RAW_LEN];
    int rc;

    if (sg_ref_resolve_head(git_dir, head_id) != 0)
        return 0;
    if (memcmp(head_id, tip, SG_SHA1_RAW_LEN) == 0)
        return 1;
    rc = sg_merge_base(git_dir, head_id, tip, base);
    if (rc == 0)
        return memcmp(base, tip, SG_SHA1_RAW_LEN) == 0;
    if (rc == -1 || rc == -2)
        return 0;
    return -1;
}

typedef struct {
    int force;
    const char *current_branch; /* NULL if HEAD is detached */
    char *worktree; /* lazily filled by branch_delete_gate, owned here */
} branch_delete_ctx;

/* Phase 76: the sg_ref_delete_batch PRECHECK -- runs even before existence
   is tested (see ref_delete.h's own comment on why: measured, git refuses
   `branch -d master` on an UNBORN repo with the worktree message, not
   "not found", even though refs/heads/master has no file at all yet, so
   this cannot be gated on "the ref exists"). Order matches real git
   2.55.0: checked-out/worktree refusal fires even under -D (force does
   not bypass it). */
static int branch_delete_precheck(void *vctx, const char *git_dir, const char *name)
{
    branch_delete_ctx *ctx = vctx;

    /* Deliberate divergence #10 (Phase 76 fix round 2): a case- or
       Unicode-normalization-aliased spelling of the checked-out branch is
       refused too -- see branch_is_current_or_alias's own comment. A -1
       (resource failure) fails CLOSED with its own diagnostic. */
    if (ctx->current_branch != NULL) {
        int m = branch_is_current_or_alias(git_dir, ctx->current_branch, name);

        if (m < 0) {
            fprintf(stderr, "sg: out of memory\n");
            return 0;
        }
        if (m) {
            if (ctx->worktree == NULL)
                ctx->worktree = worktree_root_display(git_dir);
            fprintf(stderr, "sg: cannot delete branch '%s' used by worktree at '%s'\n", name,
                    ctx->worktree != NULL ? ctx->worktree : "?");
            return 0;
        }
    }
    return 1;
}

/* The sg_ref_delete_batch GATE -- runs only for a name the precheck let
   through and that exists. Only without --force: the not-fully-merged
   refusal, with sg's own rewritten hint (cmd_switch.c's precedent for
   rewriting a hint into sg's terms). There is no interactive prompt here
   at all any more -- a refusal is strictly safer than the old
   sg_confirm_dangerous "--force skips it" shape, see docs/DESIGN.md's
   Phase 76 section for why it was removed rather than kept. */
static int branch_delete_gate(void *vctx, const char *git_dir, const char *name,
                              const unsigned char tip[SG_SHA1_RAW_LEN])
{
    branch_delete_ctx *ctx = vctx;
    int merged;

    if (!ctx->force) {
        merged = branch_is_merged(git_dir, tip);
        if (merged < 0) {
            fprintf(stderr, "sg: cannot determine whether branch '%s' is merged\n", name);
            return 0;
        }
        if (!merged) {
            fprintf(stderr, "sg: the branch '%s' is not fully merged\n", name);
            fprintf(stderr, "hint: If you are sure you want to delete it, run 'sg branch -D %s'\n", name);
            return 0;
        }
    }
    return 1;
}

static int delete_branches(const char *git_dir, const char **names, int count, int force, int quiet)
{
    branch_delete_ctx ctx;
    sg_ref_delete_spec spec;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.force = force;
    ctx.current_branch = sg_ref_current_branch(git_dir);

    memset(&spec, 0, sizeof(spec));
    spec.prefix = "refs/heads/";
    spec.not_found_fmt = "branch '%s' not found";
    spec.deleted_fmt = "Deleted branch %s (was %.7s).\n";
    /* sg-only wording, no git oracle -- modeled on tag's own shape (see
       ref_delete.h's own comment). */
    spec.too_long_fmt = "branch name too long: '%s'";
    spec.delete_fail_fmt = "failed to delete branch '%s'";
    spec.precheck = branch_delete_precheck;
    spec.precheck_ctx = &ctx;
    spec.gate = branch_delete_gate;
    spec.gate_ctx = &ctx;
    spec.quiet = quiet;

    rc = sg_ref_delete_batch(git_dir, names, count, &spec);
    free(ctx.worktree);
    free((char *)ctx.current_branch);
    return rc;
}

int sg_cmd_branch(int argc, char **argv)
{
    int del = 0;
    int force = 0;
    int quiet = 0;
    int opts_done = 0;
    const char **positional;
    int npositional = 0;
    char *git_dir;
    int rc;
    int i;

    /* Upper-bounded by argc, same convention as cmd_tag.c. */
    positional = malloc(sizeof(*positional) * (size_t)(argc > 0 ? argc : 1));
    if (positional == NULL) {
        fprintf(stderr, "sg: out of memory\n");
        return 1;
    }

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!opts_done && strcmp(a, "--") == 0) {
            opts_done = 1;
        } else if (!opts_done && strcmp(a, "--delete") == 0) {
            del = 1;
        } else if (!opts_done && strcmp(a, "--force") == 0) {
            force = 1;
        } else if (!opts_done && strcmp(a, "--quiet") == 0) {
            quiet = 1;
        } else if (!opts_done && a[0] == '-' && a[1] != '\0' && a[1] != '-') {
            /* A run of bundled short options -- measured against real git
               2.55.0, `-df` == `-d -f`. -D is its own single-letter
               shorthand for `-d -f` (not two SEPARATE flags spelled with
               one letter), so it is handled the same way inside the loop:
               'D' sets both del and force. */
            const char *p = a + 1;
            int bad = 0;

            for (; *p != '\0'; p++) {
                if (*p == 'd')
                    del = 1;
                else if (*p == 'D') {
                    del = 1;
                    force = 1;
                } else if (*p == 'f')
                    force = 1;
                else if (*p == 'q')
                    quiet = 1;
                else {
                    bad = 1;
                    break;
                }
            }
            if (bad) {
                fputs(USAGE, stderr);
                free(positional);
                return 1;
            }
        } else if (!opts_done && a[0] == '-') {
            fputs(USAGE, stderr);
            free(positional);
            return 1;
        } else {
            positional[npositional++] = a;
        }
    }

    if (del) {
        if (npositional == 0) {
            fprintf(stderr, "sg: branch name required\n");
            free(positional);
            return 1;
        }
    } else if (npositional > 2) {
        fputs(USAGE, stderr);
        free(positional);
        return 1;
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL) {
        free(positional);
        return 1;
    }

    if (del)
        rc = delete_branches(git_dir, positional, npositional, force, quiet);
    else if (npositional >= 1)
        rc = create_branch(git_dir, positional[0], npositional >= 2 ? positional[1] : NULL, force);
    else
        rc = list_branches(git_dir);

    free(positional);
    free(git_dir);
    return rc;
}
