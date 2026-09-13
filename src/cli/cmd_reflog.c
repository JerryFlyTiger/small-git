#include "sg/cli.h"

#include "sg/cli_args.h"
#include "sg/hash.h"
#include "sg/reflog.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/revparse.h"
#include "sg/workdir.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Phase75 round3 fix B, widened in round4 fix 1/fix 2: splits a typed
   argument of the exact shape "<base>@{<digits>}<suffix>" into `base` (the
   part before "@{", echoed back unchanged, same convention as the rest of
   this file), `*n_out`, and `*bare_out` (1 iff base is empty, i.e. the
   argument itself started with "@{"). `<suffix>`, if present, must be a
   "~"/"^" run (each optionally followed by decimal digits, chained/repeated
   any number of times) -- round4 fix 2 measured that git's reflog LISTING
   ignores such a suffix entirely (`HEAD@{1}^` and `HEAD@{0}~1` print
   byte-identical output to `HEAD@{1}`/`HEAD@{0}`), so this function does
   not walk it, only recognizes its shape and otherwise discards it. Any
   OTHER trailing text (a second "@{", stray characters, a trailing space --
   the last is git's OWN "@{<date>}" grammar, deliberately not implemented,
   see revparse.h) is NOT this shape at all and the match fails, letting the
   caller fall through to its existing resolution path unchanged -- this is
   what keeps "HEAD@{2}@{1}" (ambiguous-argument) and "HEAD@{1 }" (date
   selector, record-and-pinned, not fixed) refusing exactly as before.
   `base_len == 0` (round4 fix 1: the bare "@{N}" shorthand) is now a match
   too, unlike round3 -- revparse.h documents that a bare "@{N}" reads the
   CURRENT BRANCH's log, a distinct operation the caller must not collapse
   into the named-base case. Returns 0 on a match, -1 otherwise. */
static int parse_reflog_at_n(const char *arg, char *base_out, size_t base_out_size,
                             unsigned long *n_out, int *bare_out)
{
    size_t base_len = 0;
    size_t i;
    char *end;
    unsigned long n;

    while (arg[base_len] != '\0' && !(arg[base_len] == '@' && arg[base_len + 1] == '{'))
        base_len++;
    if (arg[base_len] == '\0')
        return -1;
    if (base_len >= base_out_size)
        return -1;

    i = base_len + 2; /* past "@{" */
    if (!isdigit((unsigned char)arg[i]))
        return -1; /* rejects "@{}" (empty) and "@{-1}"/"@{u}" (non-digit) */

    n = strtoul(arg + i, &end, 10);
    if (*end != '}')
        return -1; /* trailing content before any closing brace, or none at all */

    /* round4 fix 2: recognize (and ignore) a "~"/"^" suffix chain after the
       closing brace. `end[0]` is '}' itself, so the scan starts at 1. */
    for (i = 1; end[i] != '\0';) {
        if (end[i] == '~' || end[i] == '^') {
            i++;
            while (isdigit((unsigned char)end[i]))
                i++;
        } else {
            return -1;
        }
    }

    memcpy(base_out, arg, base_len);
    base_out[base_len] = '\0';
    *n_out = n;
    if (bare_out != NULL)
        *bare_out = (base_len == 0);
    return 0;
}

/* Supports only the read-only subset real git's `git reflog` offers most
   often (Phase 17 scope): `sg reflog`/`sg reflog show` with an optional
   <ref> (defaulting to HEAD) and an optional `-n <count>` cap, in either
   order. No expire/delete/--date/--format/--all. */
int sg_cmd_reflog(int argc, char **argv)
{
    static const char usage[] = "usage: sg reflog [show] [<ref>] [-n <count>]\n";
    const char *ref_arg = NULL;
    int ref_arg_typed;
    long limit = -1; /* -1 == unlimited */
    int i0 = 1;
    int i;
    char *git_dir;
    char ref_path[SG_PATH_MAX];
    sg_reflog log;
    size_t shown;

    if (argc >= 2 && strcmp(argv[1], "show") == 0)
        i0 = 2;

    for (i = i0; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0) {
            char *end;
            long v;

            if (i + 1 >= argc) {
                fputs(usage, stderr);
                return 1;
            }
            v = strtol(argv[i + 1], &end, 10);
            if (*end != '\0' || v < 0) {
                fputs(usage, stderr);
                return 1;
            }
            limit = v;
            i++;
        } else if (ref_arg == NULL) {
            ref_arg = argv[i];
        } else {
            fputs(usage, stderr);
            return 1;
        }
    }
    ref_arg_typed = ref_arg != NULL;
    if (ref_arg == NULL)
        ref_arg = "HEAD";

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;

    /* Ref doesn't exist at all -> error (exit 1). Ref exists but has never
       been logged -> sg_reflog_read below succeeds with count 0, printing
       nothing and exiting 0 (measured against real git 2.55.0: `git reflog
       show <ref-with-no-log>` prints nothing and exits 0).

       Phase 75: `sg reflog <dual>` used to print the log where git refuses
       (class D, AMBIG-BOTH) -- a ref that resolves AND names an existing
       working-tree path is ambiguous even though the ref side resolves
       just fine, so this check runs before the ref successfully proceeds,
       not instead of it. When the ref does not resolve at all, classify
       into R/O/P the same way log/diff/show do (`sg reflog show`'s own
       oracle is identical to the bare form, measured). */
    if (sg_rev_parse_ref_path(git_dir, ref_arg, ref_path, sizeof(ref_path)) != 0) {
        unsigned char obj_id[SG_SHA1_RAW_LEN];
        sg_obj_type obj_type;
        char bad_path[SG_PATH_MAX];
        int orc;
        char at_base[SG_PATH_MAX];
        unsigned long at_n;
        int at_bare;

        /* Phase75 round3 fix A: the class-D ambiguity check must win over
           EVERY branch below, not just the ordinary "ref resolves"
           success path -- an argument that resolves as an object/revision
           but names no ref (the orc==0 branch just below, and the new
           "<ref>@{N}" branch right after it) can still collide with an
           existing working-tree file of the same name, and git refuses
           those too (measured: `HEAD~1` and a full 40-hex sha both refuse
           as "ambiguous argument ... both revision and filename" when a
           same-named untracked file exists, exit 128; `sg log`/`sg show`
           already got this right, `sg reflog` did not because this
           branch's own class-D check used to run only after this whole
           `if` block, i.e. only on the ref-resolves path). Same
           `ref_arg_typed` guard as below: never fires on this function's
           own internal "HEAD" substitute for a bare `sg reflog`. */
        if (ref_arg_typed && sg_cli_arg_exists_in_worktree(ref_arg)) {
            sg_cli_report_rev_error("reflog", SG_REV_ERR_BOTH, ref_arg, NULL, 0);
            free(git_dir);
            return 1;
        }

        /* Phase75 round3 fix B, widened in round4 (fix 1: a bare "@{N}";
           fix 2: a "~"/"^" suffix after "@{N}"; fix 3: a ref whose log is
           EMPTY): "<base>@{N}" lists `base`'s OWN reflog starting at entry N
           (N=0 is `base`'s newest entry, so "<base>@{0}" is identical to
           the bare `base` form), not a single-commit resolution -- git's
           own semantics, measured against real git 2.55.0. This is a
           distinct operation from sg_rev_parse_commit's "@{N}" (which
           yields one commit id), so it is handled here rather than via
           sg_rev_parse_object, which would collapse it into the orc==0
           branch below and print nothing (the exact round2 bug this fix
           corrects). If `base` does not itself resolve to a ref, or its
           reflog is EMPTY (round4 fix 3: a tag, or a branch whose log file
           was deleted -- `sg_reflog_read` reports ENOENT as count==0, same
           as any other empty log, and docs/RULES-refs-revparse.md already
           states that the log must EXIST for "@{N}" to resolve at all, a
           rule this branch used to bypass), fall through to the ordinary
           classification below rather than inventing a sentence for a ref
           git itself refuses to resolve. */
        if (ref_arg_typed &&
            parse_reflog_at_n(ref_arg, at_base, sizeof(at_base), &at_n, &at_bare) == 0) {
            char at_ref_path[SG_PATH_MAX];
            char at_short_name[SG_PATH_MAX];
            char at_label[SG_PATH_MAX];
            int at_have_ref = 0;

            if (at_bare) {
                /* round4 fix 1: a BARE "@{N}" reads the CURRENT BRANCH's
                   log (include/sg/revparse.h's own documented distinction
                   from "HEAD@{N}"). Entries are labeled with the branch's
                   FULL ref path ("refs/heads/<branch>@{N}", measured
                   against real git), while the out-of-range message names
                   the branch's SHORT name ("master") -- two different
                   fields, measured separately.

                   A DETACHED HEAD has no current branch, and round 4
                   guessed here: it refused outright and pinned that on
                   sg's side only, on the belief that git had not been
                   measured for the combination. It has been now, and git
                   does have an answer -- it reads HEAD's own log:
                   `git reflog @{0}` on a detached HEAD prints the same
                   four lines as `git reflog` (labeled "HEAD@{N}", not
                   "refs/heads/...@{N}"), and `@{9}` there says
                   "fatal: log for 'HEAD' only has 4 entries", naming HEAD.
                   So the fallback is HEAD, both for the log that is read
                   and for the two name fields, and the pin is now on both
                   sides.

                   The other two HEAD states reach this code by DIFFERENT
                   routes, and a cold read caught an earlier version of this
                   comment collapsing them into one:
                     - UNBORN HEAD: `sg_ref_current_branch` still returns
                       "master" (it checks only the "ref: refs/heads/"
                       prefix, never whether the target exists), so
                       `at_name` is the branch name, NOT "HEAD", and the
                       call that fails is `sg_rev_parse_ref_path(git_dir,
                       "master", ...)`.
                     - CORRUPT HEAD: `sg_ref_current_branch` returns NULL,
                       so the "HEAD" fallback applies -- and there its
                       `sg_rev_parse_ref_path` SUCCEEDS (the HEAD file
                       exists), so logs/HEAD gets listed. That is sg's
                       deliberate tolerance of a corrupt HEAD; real git
                       refuses the whole repository in that state. */
                char *branch = sg_ref_current_branch(git_dir);
                const char *at_name = branch != NULL ? branch : "HEAD";
                int at_is_branch = branch != NULL;

                if (snprintf(at_short_name, sizeof(at_short_name), "%s", at_name) <
                        (int)sizeof(at_short_name) &&
                    snprintf(at_label, sizeof(at_label), at_is_branch ? "refs/heads/%s" : "%s",
                             at_name) < (int)sizeof(at_label) &&
                    sg_rev_parse_ref_path(git_dir, at_name, at_ref_path,
                                          sizeof(at_ref_path)) == 0)
                    at_have_ref = 1;
                free(branch);
                /* No explicit guard here for `at_have_ref == 0` (a corrupt
                   or unborn HEAD), and that is a MEASURED decision, not an
                   oversight. Round 5 first wrote one, reasoning that
                   falling through would let "@{0}" resolve as a commit via
                   sg_rev_parse_object's own logs/HEAD fallback and land in
                   the "resolves as an object, print nothing" branch below
                   -- the round-2 silence bug from a new angle. A mutation
                   that deleted the guard turned ZERO checks red, and
                   measuring the two reachable states says why:
                     - unborn HEAD: `sg_rev_parse_ref_path` fails for both
                       the branch and HEAD, so this is reached -- but
                       sg_rev_parse_object fails on "@{0}" too (no log to
                       read), so the fall-through reports the identical
                       class-R message. Same answer, one layer down.
                     - corrupt HEAD: never reaches here at all.
                       `sg_ref_current_branch` returns NULL, the "HEAD"
                       fallback's own `sg_rev_parse_ref_path` SUCCEEDS (the
                       HEAD file exists), and logs/HEAD is listed -- sg's
                       deliberate tolerance of a corrupt HEAD, unlike git,
                       which refuses the whole repository there.
                   So the guard was a redundant guard in this project's own
                   three-reasons-for-a-green-mutation sense, and the rule is
                   to delete it so a future mutation lands on the real
                   defence line instead of on a duplicate of it. The ANSWER
                   is still pinned (interop's "phase75 round4 fix1 (unborn
                   HEAD)" pair), which is the part that matters. */
            } else if (sg_rev_parse_ref_path(git_dir, at_base, at_ref_path,
                                             sizeof(at_ref_path)) == 0) {
                if (snprintf(at_short_name, sizeof(at_short_name), "%s", at_base) <
                        (int)sizeof(at_short_name) &&
                    snprintf(at_label, sizeof(at_label), "%s", at_base) < (int)sizeof(at_label))
                    at_have_ref = 1;
            }

            if (at_have_ref) {
                sg_reflog at_log;

                if (sg_reflog_read(git_dir, at_ref_path, &at_log) == 0) {
                    if (at_log.count == 0 && !at_bare) {
                        /* round4 fix 3: for a NAMED base, an empty log (a
                           tag, or a branch whose log was deleted by hand)
                           does not resolve at all -- fall through to the
                           ordinary classification below, same as "base
                           didn't resolve to a ref". Measured: `git reflog
                           nolog@{0}` on such a branch gives the
                           ambiguous-argument block. */
                        sg_reflog_free(&at_log);
                    } else if (at_log.count == 0) {
                        /* round6: the BARE form on an empty log is git's
                           OWN asymmetry, not a simplification -- measured,
                           current branch `nolog` with its log file
                           deleted: `git reflog @{0}` exits 0 printing
                           NOTHING, while `git reflog @{1}` exits 128 with
                           "fatal: log for refs/heads/nolog is empty" (a
                           FIFTH sentence in this phase's matrix, and the
                           only one that names the FULL ref path rather
                           than a short name), and the spelled-out
                           `nolog@{0}` refuses with the ambiguous-argument
                           block.

                           WARNING: reproducing that asymmetry here SPLITS
                           sg's two layers apart, and an earlier version of
                           this comment claimed the exact opposite ("keeps
                           the two layers consistent"), which a cold read
                           caught. What revparse.h and
                           docs/RULES-refs-revparse.md actually record is
                           that sg's revision RESOLUTION deliberately
                           rejects BOTH spellings in this state, on the
                           stated grounds that inventing an asymmetry
                           between the two spellings is worse than a
                           uniform rejection -- itself a divergence from
                           git, which accepts the bare form there by
                           falling back to the branch tip. So after Phase
                           75 `sg log @{0}` refuses in a state where
                           `sg reflog @{0}` succeeds. That is accepted, for
                           a reason specific to each layer: resolution
                           decides which COMMIT a revision means and git's
                           fallback drags in branch-tip semantics this
                           project does not want, while the listing's only
                           question is what to print, where matching git
                           costs nothing. Do not "unify" the two on the
                           strength of one of the rules alone.

                           The two answers are NOT defended the same way,
                           and a cold read caught this comment claiming they
                           were ("both are pinned... phase48's group for
                           resolution" -- there is no such interop group).
                           The listing side, here, IS interop-pinned
                           (phase75 round6). The resolution side is covered
                           only by a UNIT test with no git oracle,
                           tests/test_revparse_at_zero.c's
                           test_bare_at_zero_still_refuses_when_current_
                           branch_log_missing, deliberately so: revparse.h
                           says that divergence is not even on CLAUDE.md's
                           list "because reaching it requires deleting a log
                           file by hand". So if you are checking whether
                           the resolution side still behaves as documented,
                           that unit test is the only thing watching it --
                           do not go looking for an interop group. */
                        if (at_n == 0) {
                            sg_reflog_free(&at_log);
                            free(git_dir);
                            return 0;
                        }
                        fprintf(stderr, "sg: log for %s is empty\n", at_ref_path);
                        sg_reflog_free(&at_log);
                        free(git_dir);
                        return 1;
                    } else if (at_n >= at_log.count) {
                        /* git's SENTENCE, under sg's own conventions: its
                           "fatal: " prefix becomes "sg: " (phase75
                           translation rule 1 -- the two are the same
                           field, so keeping both would print "sg: fatal:
                           ", which is what the first version of this line
                           did), and the exit code is 1 even though git
                           uses 128 here. */
                        fprintf(stderr, "sg: log for '%s' only has %zu entries\n",
                                at_short_name, at_log.count);
                        sg_reflog_free(&at_log);
                        free(git_dir);
                        return 1;
                    } else {
                        size_t at_shown;

                        for (at_shown = at_n; at_shown < at_log.count; at_shown++) {
                            const sg_reflog_entry *entry = sg_reflog_at(&at_log, at_shown);
                            char hex[SG_SHA1_HEX_LEN + 1];

                            if (limit >= 0 && (long)(at_shown - at_n) >= limit)
                                break;

                            sg_sha1_to_hex(entry->new_id, hex);
                            printf("%.7s %s@{%zu}: %s\n", hex, at_label, at_shown,
                                   entry->message);
                        }
                        sg_reflog_free(&at_log);
                        free(git_dir);
                        return 0;
                    }
                }
                /* log couldn't be read at all (I/O error, corrupt line) --
                   fall through, same as any other unresolvable argument. */
            }
            /* base didn't resolve to a ref (or a bare form had no current
               branch to fall back to), or its log was empty -- fall
               through, same as any other unresolvable argument. */
        }

        /* Phase75 fix round: probe sg_rev_parse_object directly (the same
           call sg_cli_classify_rev_error makes internally) rather than
           going straight to classify() -- classify()'s own contract folds
           BOTH -1 (not a rev) and -4 (ambiguous) into SG_REV_ERR_NOT_A_REV,
           and every other converted call site special-cases -4 BEFORE
           calling classify() (see cli_args.h's own comment on
           sg_cli_classify_rev_error); reflog was the one site that
           skipped this, so a 4-hex prefix shared by two objects lost
           git's error:/hint: block. This probe ALSO exposes the rc==0
           case classify() cannot: `sg_rev_parse_ref_path` has no ~N/^N
           support, so `HEAD~1` fails to resolve as a REF even though it
           is a perfectly good commit -- git's own answer there is not a
           message at all, an empty reflog and exit 0 (the resolved commit
           simply has no reflog yet). Only a genuine class R (the argument
           resolves as neither a ref nor any other revision/object) keeps
           the AMBIG-UNKNOWN block. */
        orc = sg_rev_parse_object(git_dir, ref_arg, obj_id, &obj_type, bad_path, sizeof(bad_path));
        if (orc == -4) {
            sg_cli_report_ambiguous_oid(git_dir, ref_arg, SG_REV_STRICT);
            sg_cli_report_rev_error("reflog", SG_REV_ERR_NOT_A_REV, ref_arg, NULL, 0);
            free(git_dir);
            return 1;
        }
        if (orc == 0) {
            free(git_dir);
            return 0;
        }
        {
            sg_rev_err_kind kind = sg_cli_classify_rev_error(git_dir, ref_arg, bad_path, sizeof(bad_path));

            sg_cli_report_rev_error("reflog", kind, ref_arg, bad_path, 0);
        }
        free(git_dir);
        return 1;
    }
    /* Must only apply to a <ref> the user actually typed, never to this
       function's own internal "HEAD" substitute for a bare `sg reflog` --
       see cmd_reset.c's identical `rev_arg_typed` guard and comment, same
       fixture and same false refusal before this fix. */
    if (ref_arg_typed && sg_cli_arg_exists_in_worktree(ref_arg)) {
        sg_cli_report_rev_error("reflog", SG_REV_ERR_BOTH, ref_arg, NULL, 0);
        free(git_dir);
        return 1;
    }

    if (sg_reflog_read(git_dir, ref_path, &log) != 0) {
        fprintf(stderr, "sg: failed to read reflog for '%s' (corrupt file?)\n", ref_arg);
        free(git_dir);
        return 1;
    }

    /* @{N} notation: N=0 is the newest entry, counting up as entries get
       older -- entries[] itself is oldest-first, so sg_reflog_at flips the
       index (see its header comment). The name is echoed back exactly as
       the user typed it, not normalized to the ref path used to find the
       file (measured: `git reflog show heads/topic` prints
       "heads/topic@{0}", not "refs/heads/topic@{0}"). abbrev is a fixed 7
       hex digits -- real git's core.abbrev auto-lengthens for large repos,
       sg does not. */
    for (shown = 0; shown < log.count; shown++) {
        const sg_reflog_entry *entry = sg_reflog_at(&log, shown);
        char hex[SG_SHA1_HEX_LEN + 1];

        if (limit >= 0 && (long)shown >= limit)
            break;

        sg_sha1_to_hex(entry->new_id, hex);
        printf("%.7s %s@{%zu}: %s\n", hex, ref_arg, shown, entry->message);
    }

    sg_reflog_free(&log);
    free(git_dir);
    return 0;
}
