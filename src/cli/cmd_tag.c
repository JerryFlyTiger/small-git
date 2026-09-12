#include "sg/cli.h"

#include "sg/cli_args.h"
#include "sg/hash.h"
#include "sg/ident.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/revparse.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char USAGE[] =
    "usage: sg tag [-a] [-m <msg>] [-f|--force] [--] <name> [<rev>]\n"
    "       sg tag -d <name>...\n"
    "       sg tag\n";

static int list_tags(const char *git_dir)
{
    char **names;
    size_t count;
    size_t i;

    if (sg_ref_list_under(git_dir, "refs/tags/", &names, &count) != 0) {
        fprintf(stderr, "sg: cannot list tags\n");
        return 1;
    }

    for (i = 0; i < count; i++) {
        printf("%s\n", names[i]);
        free(names[i]);
    }
    free(names);
    return 0;
}

static int create_tag(const char *git_dir, const char *name, const char *rev, int annotated,
                      const char **messages, int message_count, int force)
{
    unsigned char target_id[SG_SHA1_RAW_LEN];
    sg_obj_type target_type;
    char ref_path[SG_PATH_MAX];
    unsigned char old_id[SG_SHA1_RAW_LEN];
    int had_old;

    if (!sg_ref_name_valid_for_create(name)) {
        fprintf(stderr, "sg: '%s' is not a valid tag name.\n", name);
        return 1;
    }

    if (snprintf(ref_path, sizeof(ref_path), "refs/tags/%s", name) >= (int)sizeof(ref_path)) {
        fprintf(stderr, "sg: tag name too long\n");
        return 1;
    }

    had_old = sg_ref_read_path(git_dir, ref_path, old_id) == 0;
    if (!force && had_old) {
        fprintf(stderr, "sg: tag '%s' already exists\n", name);
        return 1;
    }

    {
        /* Phase 73 fix: <rev> must NOT be peeled -- sg_rev_parse_commit
           peels an annotated tag to its underlying commit by contract,
           which is wrong here (measured against real git 2.55.0:
           `tag t atag` makes a lightweight ref to the TAG OBJECT, not to
           the commit it names). sg_rev_parse_object resolves any object
           without peeling and reports its actual type, which also makes
           tagging a tree or a blob (and a "<rev>:<path>" argument) work
           for free -- both are git's real behaviour, not an extension. */
        const char *rev_or_head = rev != NULL ? rev : "HEAD";
        char bad_path[SG_PATH_MAX];
        int prc;

        bad_path[0] = '\0';
        prc = sg_rev_parse_object(git_dir, rev_or_head, target_id, &target_type, bad_path,
                                  sizeof(bad_path));
        /* A well-formed id whose object cannot be read is missing or
           corrupt, NOT an invalid name -- same distinction cmd_cat_file.c
           and cmd_show.c make for the identical -3 return. */
        if (prc == -3) {
            fprintf(stderr, "sg: object '%s' not found or corrupt\n", rev_or_head);
            return 1;
        }
        if (prc == -2) {
            const char *colon = strchr(rev_or_head, ':');
            char rev_part[SG_PATH_MAX];
            size_t rev_len = colon != NULL ? (size_t)(colon - rev_or_head) : 0;

            if (rev_len >= sizeof(rev_part))
                rev_len = sizeof(rev_part) - 1;
            memcpy(rev_part, rev_or_head, rev_len);
            rev_part[rev_len] = '\0';
            fprintf(stderr, "sg: path '%s' does not exist in '%s'\n", bad_path, rev_part);
            return 1;
        }
        if (prc != 0) {
            if (prc == -4)
                sg_cli_report_ambiguous_oid(git_dir, rev_or_head, SG_REV_STRICT);
            fprintf(stderr, "sg: cannot resolve '%s'\n", rev_or_head);
            return 1;
        }
    }

    if (annotated) {
        sg_tag tag;
        unsigned char *serialized;
        size_t serialized_len;
        unsigned char tag_id[SG_SHA1_RAW_LEN];
        sg_ident tagger;
        const char *bad = NULL;
        char *joined_message;
        char *cleaned_message;

        /* An annotated tag's tagger line comes from the COMMITTER identity,
           the same as a commit's committer line -- not the author identity
           (Phase 72's defect C; measured against real git 2.55.0). */
        if (sg_ident_committer(&tagger, &bad) != 0) {
            fprintf(stderr, "sg: invalid date format: %s\n", bad);
            return 1;
        }

        /* Phase 73 addendum: repeated -m is NOT last-one-wins (measured:
           `-m one -m two` -> message "one\n\ntwo", each -m becomes its
           own paragraph). Join first, then clean up exactly as a single
           -m value already was. */
        if (sg_message_join(messages, (size_t)message_count, &joined_message) != 0) {
            fprintf(stderr, "sg: out of memory\n");
            return 1;
        }

        /* Unlike `git commit`, real `git tag -a -m` does NOT refuse an
           empty (or whitespace-only, which normalizes to empty) message --
           it happily creates a tag object whose message segment is empty.
           Verified directly against git 2.55.0. So, unlike cmd_commit.c,
           there is no empty-message rejection here. */
        if (sg_message_cleanup(joined_message, &cleaned_message) != 0) {
            fprintf(stderr, "sg: out of memory\n");
            free(joined_message);
            return 1;
        }
        free(joined_message);

        memset(&tag, 0, sizeof(tag));
        memcpy(tag.object, target_id, SG_SHA1_RAW_LEN);
        /* Phase 73 fix: the target's ACTUAL type, not a hardcoded commit --
           git writes `type tag`/`type tree`/`type blob` when <rev> names
           one of those. */
        tag.object_type = target_type;
        tag.tag_name = (char *)name;
        tag.tagger_name = tagger.name;
        tag.tagger_email = tagger.email;
        tag.tagger_time = tagger.when;
        strcpy(tag.tagger_tz, tagger.tz);
        tag.message = cleaned_message;

        if (sg_tag_serialize(&tag, &serialized, &serialized_len) != 0) {
            fprintf(stderr, "sg: cannot serialize tag object\n");
            free(cleaned_message);
            return 1;
        }
        free(cleaned_message);
        if (sg_loose_write(git_dir, SG_OBJ_TAG, serialized, serialized_len, tag_id) != 0) {
            fprintf(stderr, "sg: cannot write tag object\n");
            free(serialized);
            return 1;
        }
        free(serialized);

        if (sg_ref_write_path(git_dir, ref_path, tag_id) != 0) {
            fprintf(stderr, "sg: cannot create tag '%s'\n", name);
            return 1;
        }

        /* "Updated tag" is about the ref's VALUE moving, not about -f
           overwriting something -- measured: `-f` re-pointing a tag at the
           commit it already names prints nothing at all. Compare against
           the newly written ref target (the tag object id here), not the
           underlying commit. */
        if (had_old && memcmp(old_id, tag_id, SG_SHA1_RAW_LEN) != 0) {
            char old_hex[SG_SHA1_HEX_LEN + 1];

            sg_sha1_to_hex(old_id, old_hex);
            printf("Updated tag '%s' (was %.7s)\n", name, old_hex);
        }
    } else {
        if (sg_ref_write_path(git_dir, ref_path, target_id) != 0) {
            fprintf(stderr, "sg: cannot create tag '%s'\n", name);
            return 1;
        }

        if (had_old && memcmp(old_id, target_id, SG_SHA1_RAW_LEN) != 0) {
            char old_hex[SG_SHA1_HEX_LEN + 1];

            sg_sha1_to_hex(old_id, old_hex);
            printf("Updated tag '%s' (was %.7s)\n", name, old_hex);
        }
    }

    return 0;
}

/* Phase 73: -d now takes many names, processed in argv order, deleting
   each one that exists and reporting one error line per name that does
   not -- NOT all-or-nothing (measured: `git tag -d atag nosuch lw` deletes
   both atag and lw, prints one error for nosuch, and exits 1). Returns 0
   only if every name was deleted (also 0 for an empty list -- see the
   `-d` with no names note at the call site).

   Phase 73 review round 2: a name appearing MORE THAN ONCE in this same
   invocation is a ref-transaction property in real git, not a per-name
   one -- `git tag -d lw lw` and `git tag -d lw atag lw` both refuse the
   WHOLE batch and delete NOTHING ("could not delete references: multiple
   updates for ref 'refs/tags/lw' not allowed", the identical shape
   cmd_push.c documents for the refs/sg/chunks keepalive). This has to be
   checked BEFORE any deletion runs: `-d lw atag lw` deleting "everything
   except the duplicate" (which is what happens if the check runs
   per-name during the loop, or after it) is a WORSE answer than either
   all-or-nothing extreme, because git's answer is "nothing" and the
   naive answer silently deletes atag too.

   Phase 73 review round 3: the rule above OVER-REFUSES -- it is a
   property of git's ref TRANSACTION, so it only applies to a name that
   actually ENTERS that transaction, i.e. one that resolves to an
   existing tag. `-d nosuch atag nosuch` deletes atag and reports
   "not found" twice on real git (measured); a scan keyed on bare argv
   equality wrongly refused the whole batch here, leaving atag undeleted.
   Existence has to be resolved for every name FIRST (before any
   deletion, so a later name's existence can't have been changed by an
   earlier one in the same call), and only names that both repeat AND
   currently exist can trigger the refusal.

   Also per review round 3: when two DIFFERENT existing names are each
   duplicated (`-d lw lw atag2 atag2`), real git's message names the
   SMALLEST one by strcmp ('atag2'), independent of argv order (measured
   both orderings, including a v1.9/v1.10 fixture where strcmp picks
   v1.10 against intuition, and a fixture where the smallest name sits
   LAST in argv). Error wording is interface in this project, so this is
   matched rather than left as a divergence.

   Phase 73 review round 4: round 3's early return on the refusal
   SWALLOWED the "not found" diagnostics for any missing name in the same
   call -- it decided existence and refused in one pass that returned
   before ever printing anything for a missing name. Measured: `git tag
   -d nosuch lw lw` prints "not found" for nosuch, THEN the refusal for
   lw (both lines, in that order); `-d nosuch nosuch lw lw` prints
   "not found" TWICE (once per occurrence, in argv order) before the
   refusal. So existence resolution, the "not found" reporting, and the
   refusal decision are now three things done in that ORDER, over the
   WHOLE name list, with nothing deleted until all three have run:
   resolve+report first (one line per missing occurrence, as encountered,
   never re-checked against the ORIGINAL argv-equality rule so a
   duplicated MISSING name still doesn't trigger a refusal), then decide
   the refusal from the recorded existence flags, then -- only if no
   refusal fires -- actually delete. */
static int delete_tags(const char *git_dir, const char **names, int count)
{
    int i;
    int had_failure = 0;
    int *exists;
    const char *smallest_dup = NULL;

    /* Upper-bounded by argc via the caller's own positional array. */
    exists = malloc(sizeof(*exists) * (size_t)(count > 0 ? count : 1));
    if (exists == NULL) {
        fprintf(stderr, "sg: out of memory\n");
        return 1;
    }

    /* Pass 1: resolve existence and report every missing name (or one
       whose ref path is too long) EXACTLY ONCE, in argv order, as it is
       encountered. Nothing is deleted here, so this pass and the refusal
       decision below both see the SAME pre-deletion state. */
    for (i = 0; i < count; i++) {
        char ref_path[SG_PATH_MAX];
        unsigned char id[SG_SHA1_RAW_LEN];

        if (snprintf(ref_path, sizeof(ref_path), "refs/tags/%s", names[i]) >= (int)sizeof(ref_path)) {
            fprintf(stderr, "sg: tag name too long: '%s'\n", names[i]);
            exists[i] = 0;
            had_failure = 1;
            continue;
        }
        if (sg_ref_read_path(git_dir, ref_path, id) != 0) {
            fprintf(stderr, "sg: tag '%s' not found.\n", names[i]);
            exists[i] = 0;
            had_failure = 1;
            continue;
        }
        exists[i] = 1;
    }

    /* Pass 2: a repeated name only triggers the transaction refusal when
       it EXISTS (round 3's rule, unchanged here) -- a repeated MISSING
       name already got its "not found" line above, once per occurrence,
       and blocks nothing. */
    for (i = 0; i < count; i++) {
        int j;

        if (!exists[i])
            continue;
        for (j = i + 1; j < count; j++) {
            if (!exists[j] || strcmp(names[i], names[j]) != 0)
                continue;
            if (smallest_dup == NULL || strcmp(names[i], smallest_dup) < 0)
                smallest_dup = names[i];
            break; /* names[i] is flagged; no need to find every pair naming it */
        }
    }
    if (smallest_dup != NULL) {
        fprintf(stderr,
               "sg: could not delete references: multiple updates for ref "
               "'refs/tags/%s' not allowed\n",
               smallest_dup);
        free(exists);
        return 1;
    }

    /* Pass 3: no transaction conflict -- actually delete every name that
       exists. Re-deriving the ref path and re-reading the old id is not a
       check-then-use gap: nothing has been deleted by either earlier pass,
       so the ref's value cannot have changed since pass 1 observed it. */
    for (i = 0; i < count; i++) {
        const char *name = names[i];
        char ref_path[SG_PATH_MAX];
        unsigned char old_id[SG_SHA1_RAW_LEN];
        char old_hex[SG_SHA1_HEX_LEN + 1];
        int rc;

        if (!exists[i])
            continue; /* already reported in pass 1 */

        snprintf(ref_path, sizeof(ref_path), "refs/tags/%s", name);
        if (sg_ref_read_path(git_dir, ref_path, old_id) != 0) {
            fprintf(stderr, "sg: tag '%s' not found.\n", name);
            had_failure = 1;
            continue;
        }
        rc = sg_ref_delete_under(git_dir, "refs/tags/", name);
        if (rc != 0) {
            fprintf(stderr, "sg: failed to delete tag '%s'\n", name);
            had_failure = 1;
            continue;
        }
        sg_sha1_to_hex(old_id, old_hex);
        printf("Deleted tag '%s' (was %.7s)\n", name, old_hex);
    }

    free(exists);
    return had_failure ? 1 : 0;
}

int sg_cmd_tag(int argc, char **argv)
{
    int del = 0;
    int force = 0;
    int annotated = 0;
    int opts_done = 0;
    const char **messages;
    int message_count = 0;
    const char **positional;
    int npositional = 0;
    const char *name = NULL;
    const char *rev = NULL;
    char *git_dir;
    int rc;
    int i;

    /* Both arrays are upper-bounded by argc: every positional argument
       and every -m value comes straight from argv, so there can never be
       more of either than argv itself. */
    positional = malloc(sizeof(*positional) * (size_t)(argc > 0 ? argc : 1));
    messages = malloc(sizeof(*messages) * (size_t)(argc > 0 ? argc : 1));
    if (positional == NULL || messages == NULL) {
        fprintf(stderr, "sg: out of memory\n");
        free(positional);
        free(messages);
        return 1;
    }

    for (i = 1; i < argc; i++) {
        if (!opts_done && strcmp(argv[i], "--") == 0) {
            opts_done = 1;
        } else if (!opts_done && (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--delete") == 0)) {
            del = 1;
        } else if (!opts_done && (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0)) {
            force = 1;
        } else if (!opts_done && strcmp(argv[i], "-a") == 0) {
            annotated = 1;
        } else if (!opts_done && strcmp(argv[i], "-m") == 0) {
            if (i + 1 >= argc) {
                fputs(USAGE, stderr);
                free(positional);
                free(messages);
                return 1;
            }
            /* Phase 73 addendum: repeated -m joins into one message
               (see sg_message_join's header comment) -- it does NOT
               overwrite the previous value the way this used to read. */
            messages[message_count++] = argv[++i];
        } else if (!opts_done && argv[i][0] == '-') {
            fputs(USAGE, stderr);
            free(positional);
            free(messages);
            return 1;
        } else {
            positional[npositional++] = argv[i];
        }
    }

    if (del) {
        /* -d combined with any tag-creation option (-a/-m/-f) is a usage
           error in real git, not a silent partial action (measured:
           `git tag -d -a -m x name` and `git tag -d -f name` both print
           usage and delete nothing, exit 129). Multiple names are no
           longer special-cased here -- see delete_tags for the per-name
           partial-failure semantics Phase 73 adds.

           Phase 73 addendum: `-d` with NO names at all is NOT a usage
           error -- measured twice against real git 2.55.0, `git tag -d`
           exits 0 and prints nothing. This is the opposite direction from
           a create-only flag with no name (which IS a usage error, see
           below); the two are deliberately not unified. */
        if (annotated || message_count != 0 || force) {
            fputs(USAGE, stderr);
            free(positional);
            free(messages);
            return 1;
        }
    } else {
        if (npositional > 2) {
            fputs(USAGE, stderr);
            free(positional);
            free(messages);
            return 1;
        }
        if (npositional >= 1)
            name = positional[0];
        if (npositional >= 2)
            rev = positional[1];

        if (annotated && message_count == 0) {
            fputs(USAGE, stderr);
            free(positional);
            free(messages);
            return 1;
        }
        /* -m without -a implicitly means an annotated tag, matching real
           git. */
        if (message_count != 0)
            annotated = 1;

        /* Phase 73 fix: a create-only flag (-a/-m/-f) with no tag name is
           a usage error in real git (exit 129: `git tag -m hi`, `git tag
           -f`, `git tag -a -m hi` all refuse) -- sg used to fall through
           to list_tags, silently swallowing the flags. Bare `sg tag` with
           no arguments at all must keep listing, which this leaves
           untouched (annotated and force are both still 0 there). */
        if (name == NULL && (annotated || force)) {
            fputs(USAGE, stderr);
            free(positional);
            free(messages);
            return 1;
        }
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL) {
        free(positional);
        free(messages);
        return 1;
    }

    if (del)
        rc = delete_tags(git_dir, positional, npositional);
    else if (name != NULL)
        rc = create_tag(git_dir, name, rev, annotated, messages, message_count, force);
    else
        rc = list_tags(git_dir);

    free(positional);
    free(messages);
    free(git_dir);
    return rc;
}
