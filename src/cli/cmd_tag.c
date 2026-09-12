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
    "       sg tag -d <name>\n"
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

/* True if refs/tags/<name> already exists (loose or packed). */
static int tag_exists(const char *git_dir, const char *name)
{
    unsigned char id[SG_SHA1_RAW_LEN];
    char ref_path[SG_PATH_MAX];

    if (snprintf(ref_path, sizeof(ref_path), "refs/tags/%s", name) >= (int)sizeof(ref_path))
        return 0;
    return sg_ref_read_path(git_dir, ref_path, id) == 0;
}

static int create_tag(const char *git_dir, const char *name, const char *rev, int annotated,
                      const char *message, int force)
{
    unsigned char target_id[SG_SHA1_RAW_LEN];
    char ref_path[SG_PATH_MAX];

    if (!sg_ref_name_valid_for_create(name)) {
        fprintf(stderr, "sg: '%s' is not a valid tag name\n", name);
        return 1;
    }
    if (!force && tag_exists(git_dir, name)) {
        fprintf(stderr, "sg: tag '%s' already exists\n", name);
        return 1;
    }
    {
        const char *rev_or_head = rev != NULL ? rev : "HEAD";
        int prc = sg_rev_parse_commit(git_dir, rev_or_head, target_id);

        if (prc != 0) {
            if (prc == -4)
                sg_cli_report_ambiguous_oid(git_dir, rev_or_head, SG_REV_STRICT);
            fprintf(stderr, "sg: cannot resolve '%s'\n", rev_or_head);
            return 1;
        }
    }

    if (snprintf(ref_path, sizeof(ref_path), "refs/tags/%s", name) >= (int)sizeof(ref_path)) {
        fprintf(stderr, "sg: tag name too long\n");
        return 1;
    }

    if (annotated) {
        sg_tag tag;
        unsigned char *serialized;
        size_t serialized_len;
        unsigned char tag_id[SG_SHA1_RAW_LEN];
        sg_ident tagger;
        const char *bad = NULL;
        char *cleaned_message;

        /* An annotated tag's tagger line comes from the COMMITTER identity,
           the same as a commit's committer line -- not the author identity
           (Phase 72's defect C; measured against real git 2.55.0). */
        if (sg_ident_committer(&tagger, &bad) != 0) {
            fprintf(stderr, "sg: invalid date format: %s\n", bad);
            return 1;
        }

        /* Unlike `git commit`, real `git tag -a -m` does NOT refuse an
           empty (or whitespace-only, which normalizes to empty) message --
           it happily creates a tag object whose message segment is empty.
           Verified directly against git 2.55.0. So, unlike cmd_commit.c,
           there is no empty-message rejection here. */
        if (sg_message_cleanup(message, &cleaned_message) != 0) {
            fprintf(stderr, "sg: out of memory\n");
            return 1;
        }

        memset(&tag, 0, sizeof(tag));
        memcpy(tag.object, target_id, SG_SHA1_RAW_LEN);
        tag.object_type = SG_OBJ_COMMIT;
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
    } else {
        if (sg_ref_write_path(git_dir, ref_path, target_id) != 0) {
            fprintf(stderr, "sg: cannot create tag '%s'\n", name);
            return 1;
        }
    }

    return 0;
}

static int delete_tag(const char *git_dir, const char *name)
{
    int rc = sg_ref_delete_under(git_dir, "refs/tags/", name);

    if (rc == 1) {
        fprintf(stderr, "sg: tag '%s' not found\n", name);
        return 1;
    }
    if (rc != 0) {
        fprintf(stderr, "sg: failed to delete tag '%s'\n", name);
        return 1;
    }
    printf("Deleted tag '%s'\n", name);
    return 0;
}

int sg_cmd_tag(int argc, char **argv)
{
    int del = 0;
    int force = 0;
    int annotated = 0;
    int opts_done = 0;
    const char *message = NULL;
    const char *name = NULL;
    const char *rev = NULL;
    char *git_dir;
    int rc;
    int i;

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
                return 1;
            }
            message = argv[++i];
        } else if (!opts_done && argv[i][0] == '-') {
            fputs(USAGE, stderr);
            return 1;
        } else if (name == NULL) {
            name = argv[i];
        } else if (rev == NULL) {
            rev = argv[i];
        } else {
            fputs(USAGE, stderr);
            return 1;
        }
    }

    if (del && name == NULL) {
        fputs(USAGE, stderr);
        return 1;
    }
    /* -d combined with any tag-creation option (-a/-m/-f) or a second
       positional argument is a usage error in real git, not a silent
       partial action -- verified directly: `git tag -d -a -m x name`,
       `git tag -d -f name`, and `git tag -d name1 name2` (this project
       doesn't support multi-name delete) all print usage and delete
       nothing, exit 129. */
    if (del && (annotated || message != NULL || force || rev != NULL)) {
        fputs(USAGE, stderr);
        return 1;
    }
    if (annotated && message == NULL) {
        fputs(USAGE, stderr);
        return 1;
    }
    /* -m without -a implicitly means an annotated tag, matching real git. */
    if (message != NULL)
        annotated = 1;

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;

    if (del)
        rc = delete_tag(git_dir, name);
    else if (name != NULL)
        rc = create_tag(git_dir, name, rev, annotated, message, force);
    else
        rc = list_tags(git_dir);

    free(git_dir);
    return rc;
}
