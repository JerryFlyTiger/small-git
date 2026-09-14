#include "sg/cli.h"

#include "sg/cli_args.h"
#include "sg/hash.h"
#include "sg/ident.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/ref_delete.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/revparse.h"
#include "sg/workdir.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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
        /* Phase 75: git's own wordings, measured against real git 2.55.0 --
           see the "tag" row of the table in cli_args.c. class O names the
           REF being created (git's own line does too), so `name` is
           threaded through as the reporter's `detail`; class P is NOT the
           standard "path does not exist" message here -- git answers with
           its class-R wording instead ("Failed to resolve ... as a valid
           ref."), which is why -2 is folded into the same branch as the
           generic failure rather than getting its own. */
        if (prc == -3) {
            sg_cli_report_rev_error("tag", SG_REV_ERR_MISSING_OBJ, rev_or_head, name, 0);
            return 1;
        }
        if (prc != 0) {
            if (prc == -4)
                sg_cli_report_ambiguous_oid(git_dir, rev_or_head, SG_REV_STRICT);
            sg_cli_report_rev_error("tag", SG_REV_ERR_NOT_A_REV, rev_or_head, NULL, 0);
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

/* Phase 76: the batch-delete engine itself moved to cli/ref_delete.c
   (sg_ref_delete_batch) so `sg branch -d`/-D can share it -- see that
   file for the full Phase 73/74 history this preserves unchanged. `sg tag
   -d` has no per-name gate beyond existence, so `spec.gate` is NULL. */
static int delete_tags(const char *git_dir, const char **names, int count)
{
    sg_ref_delete_spec spec;

    memset(&spec, 0, sizeof(spec));
    spec.prefix = "refs/tags/";
    spec.not_found_fmt = "tag '%s' not found.";
    spec.deleted_fmt = "Deleted tag '%s' (was %.7s)\n";
    spec.too_long_fmt = "tag name too long: '%s'";
    spec.delete_fail_fmt = "failed to delete tag '%s'";
    spec.precheck = NULL;
    spec.precheck_ctx = NULL;
    spec.gate = NULL;
    spec.gate_ctx = NULL;
    spec.quiet = 0;
    return sg_ref_delete_batch(git_dir, names, count, &spec);
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
