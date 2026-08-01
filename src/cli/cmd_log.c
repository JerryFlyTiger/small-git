#include "sg/cli.h"

#include "sg/hash.h"
#include "sg/objstore.h"
#include "sg/object.h"
#include "sg/refs.h"
#include "sg/repo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int sg_cmd_log(int argc, char **argv)
{
    char *git_dir;
    unsigned char id[SG_SHA1_RAW_LEN];
    int rc = 0;

    (void)argv;
    if (argc != 1) {
        fprintf(stderr, "usage: sg log\n");
        return 1;
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;

    if (sg_ref_resolve_head(git_dir, id) != 0) {
        fprintf(stderr, "fatal: your current branch does not have any commits yet\n");
        free(git_dir);
        return 1;
    }

    /* first-parent only: merge commits' other parents are not walked (Phase 2 scope) */
    for (;;) {
        sg_obj_type type;
        unsigned char *content;
        size_t content_len;
        sg_commit commit;
        char hex[SG_SHA1_HEX_LEN + 1];
        time_t t;
        char timebuf[64];
        struct tm tmv;

        if (sg_object_read(git_dir, id, &type, &content, &content_len) != 0 || type != SG_OBJ_COMMIT) {
            fprintf(stderr, "sg: corrupt commit object\n");
            rc = 1;
            break;
        }
        if (sg_commit_parse(content, content_len, &commit) != 0) {
            fprintf(stderr, "sg: malformed commit object\n");
            free(content);
            rc = 1;
            break;
        }
        free(content);

        sg_sha1_to_hex(id, hex);
        t = (time_t)commit.author_time;
        gmtime_r(&t, &tmv);
        strftime(timebuf, sizeof(timebuf), "%a %b %d %H:%M:%S %Y", &tmv);

        printf("commit %s\n", hex);
        if (commit.parent_count > 1) {
            /* first-parent-only walk (Phase 2 scope): the other parents are
               never themselves visited, but a merge commit is still flagged
               here so its extra history isn't silently invisible. */
            size_t pi;

            printf("Merge:");
            for (pi = 0; pi < commit.parent_count; pi++) {
                char phex[SG_SHA1_HEX_LEN + 1];

                sg_sha1_to_hex(commit.parents[pi], phex);
                printf(" %.7s", phex);
            }
            printf("\n");
        }
        printf("Author: %s <%s>\n", commit.author_name, commit.author_email);
        printf("Date:   %s %s\n", timebuf, commit.author_tz);
        printf("\n    %s\n\n", commit.message);

        if (commit.parent_count == 0) {
            sg_commit_free(&commit);
            break;
        }
        memcpy(id, commit.parents[0], SG_SHA1_RAW_LEN);
        sg_commit_free(&commit);
    }

    free(git_dir);
    return rc;
}
