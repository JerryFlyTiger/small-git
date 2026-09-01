#include "sg/cli.h"

#include "sg/date.h"
#include "sg/hash.h"
#include "sg/objstore.h"
#include "sg/object.h"
#include "sg/refs.h"
#include "sg/repo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* git indents EVERY message line by four spaces, a blank one included (it
   emits "    \n", measured), and prints the block -- leading blank line and
   all -- only when the message is non-empty: a commit with an empty message
   renders as its header lines and nothing else. */
static void print_message(const char *msg)
{
    const char *p = msg;

    if (msg == NULL || *msg == '\0')
        return;
    printf("\n");
    while (*p != '\0') {
        const char *nl = strchr(p, '\n');

        if (nl == NULL) {
            printf("    %s\n", p);
            break;
        }
        printf("    %.*s\n", (int)(nl - p), p);
        p = nl + 1;
    }
}

int sg_cmd_log(int argc, char **argv)
{
    char *git_dir;
    unsigned char id[SG_SHA1_RAW_LEN];
    int rc = 0;
    int first = 1;

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
        char timebuf[SG_DATE_NORMAL_MAX];

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
        /* The stored offset is part of the answer, not decoration: the wall
           clock git shows is the timestamp SHIFTED INTO that offset. Reading
           it as UTC while still printing "+0800" beside it was wrong by the
           offset -- eight hours here -- and said so in its own output. */
        if (sg_date_format_normal(commit.author_time, commit.author_tz,
                                  timebuf, sizeof(timebuf)) != 0)
            timebuf[0] = '\0';

        /* One blank line BETWEEN entries and none after the last, so the
           separator belongs before every entry but the first. Measured: an
           entry whose message is empty still gets it, which is why this
           cannot be folded into the message block below. Keeping it here
           also leaves the first line of `sg log` a bare `commit <sha>`,
           which interop already parses with `head -1`. */
        if (!first)
            printf("\n");
        first = 0;

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
        printf("Date:   %s\n", timebuf);
        print_message(commit.message);

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
