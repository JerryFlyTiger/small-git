#include "sg/cli.h"

#include "sg/commit_out.h"
#include "sg/hash.h"
#include "sg/objstore.h"
#include "sg/object.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/revparse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char USAGE[] =
    "usage: sg log [-n <count>|-<count>|--max-count=<count>] [--oneline] "
    "[-p|--patch] [--stat] [<rev>]\n";

/* "-n <count>", "--max-count=<count>" and the bare "-<count>" all mean the
   same thing; 0 is legal and prints nothing (measured: git exits 0). */
static int parse_count(const char *s, long *out)
{
    char *end;
    long v;

    if (s == NULL || *s == '\0')
        return -1;
    v = strtol(s, &end, 10);
    if (*end != '\0' || v < 0)
        return -1;
    *out = v;
    return 0;
}

int sg_cmd_log(int argc, char **argv)
{
    char *git_dir;
    unsigned char id[SG_SHA1_RAW_LEN];
    const char *rev = NULL;
    sg_commit_out_opts o;
    long max_count = -1;
    int rc = 0;
    int first = 1;
    long shown = 0;
    int i;

    o.oneline = 0;
    o.patch = 0;
    o.stat = 0;
    /* Phase 59 added name_only/name_status to this shared struct
       (sg_commit_out_opts) -- `sg log` does not implement either flag, but
       print_commit_diff now checks them FIRST, before patch/stat, so
       leaving these as uninitialized stack garbage would route every `sg
       log -p`/`--stat` call down the wrong branch whenever the garbage bit
       happened to be nonzero (CLAUDE.md's Phase 29 shared-struct warning:
       search every construction site that assigns fields one at a time
       rather than through a single factory). */
    o.name_only = 0;
    o.name_status = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--oneline") == 0) {
            o.oneline = 1;
        } else if (strcmp(a, "-p") == 0 || strcmp(a, "--patch") == 0) {
            o.patch = 1;
        } else if (strcmp(a, "--stat") == 0) {
            o.stat = 1;
        } else if (strcmp(a, "-n") == 0) {
            if (i + 1 >= argc || parse_count(argv[i + 1], &max_count) != 0) {
                fprintf(stderr, "%s", USAGE);
                return 1;
            }
            i++;
        } else if (strncmp(a, "--max-count=", 12) == 0) {
            if (parse_count(a + 12, &max_count) != 0) {
                fprintf(stderr, "%s", USAGE);
                return 1;
            }
        } else if (a[0] == '-' && a[1] >= '0' && a[1] <= '9') {
            if (parse_count(a + 1, &max_count) != 0) {
                fprintf(stderr, "%s", USAGE);
                return 1;
            }
        } else if (a[0] == '-') {
            fprintf(stderr, "%s", USAGE);
            return 1;
        } else if (rev != NULL) {
            fprintf(stderr, "%s", USAGE);
            return 1;
        } else {
            rev = a;
        }
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;

    if (rev != NULL) {
        if (sg_rev_parse_commit(git_dir, rev, id) != 0) {
            fprintf(stderr, "sg: not a valid revision '%s'\n", rev);
            free(git_dir);
            return 1;
        }
    } else if (sg_ref_resolve_head(git_dir, id) != 0) {
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

        if (max_count >= 0 && shown >= max_count)
            break;

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

        shown++;

        /* One blank line BETWEEN entries and none after the last, so the
           separator belongs before every entry but the first. Measured: an
           entry whose message is empty still gets it, which is why this
           cannot be folded into the message block inside the shared
           renderer. Keeping it here also leaves the first line of `sg log`
           a bare `commit <sha>`, which interop already parses with
           `head -1`. --oneline gets no separator at all (measured). */
        if (!first && !o.oneline)
            printf("\n");
        first = 0;

        if (sg_commit_out_entry(git_dir, id, &commit, &o) != 0) {
            fprintf(stderr, "sg: cannot render this commit's diff\n");
            rc = 1;
            sg_commit_free(&commit);
            break;
        }

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
