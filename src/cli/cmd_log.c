#include "sg/cli.h"

#include "sg/date.h"
#include "sg/diff.h"
#include "sg/diff_out.h"
#include "sg/hash.h"
#include "sg/objstore.h"
#include "sg/object.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/revparse.h"
#include "sg/similarity.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* git abbreviates to core.abbrev, whose default is `auto` and scales with the
   repository's object count. sg pins 7, which is what auto yields for a small
   repository; interop declares the same on git's side rather than pretending
   the two policies agree. */
#define SG_LOG_ABBREV 7

static const char USAGE[] =
    "usage: sg log [-n <count>|-<count>|--max-count=<count>] [--oneline] "
    "[-p|--patch] [--stat] [<rev>]\n";

typedef struct {
    long max_count; /* -1 means unlimited */
    int oneline;
    int patch;
    int stat;
} log_opts;

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

/* --oneline's second field is the message's FIRST line, and the separating
   space is printed even when there is no first line at all: measured, an
   empty message renders as "<abbrev> " with a trailing space. */
static void print_subject(const char *msg)
{
    const char *nl;

    if (msg == NULL) {
        printf("\n");
        return;
    }
    nl = strchr(msg, '\n');
    if (nl == NULL)
        printf("%s\n", msg);
    else
        printf("%.*s\n", (int)(nl - msg), msg);
}

/* The commit's own diff: first parent's tree against its own, or the empty
   tree for a root commit. That this renders a diff for a MERGE at all is a
   consequence of sg's first-parent-only walk, and it agrees with git under
   the same restriction: plain `git log -p` prints nothing for a merge, while
   `git log --first-parent -p` -- the oracle this command is compared against
   -- prints the diff against parent 1 (measured, both directions).
   Returns 0, or -1 having printed nothing. */
static int print_commit_diff(const char *git_dir, const sg_commit *commit,
                             const log_opts *o)
{
    sg_diff_list list;
    unsigned char parent_tree[SG_SHA1_RAW_LEN];
    const unsigned char *old_tree = NULL;
    char bad_path[SG_PATH_MAX];
    sg_diff_out_opts opts;
    int rc = 0;

    if (commit->parent_count > 0) {
        if (sg_commit_tree_of(git_dir, commit->parents[0], parent_tree) != 0)
            return -1;
        old_tree = parent_tree;
    }

    bad_path[0] = '\0';
    if (sg_diff_trees(git_dir, old_tree, commit->tree, &list, bad_path, 0) != 0)
        return -1;

    /* git's own default since 2.9: diff.renames is on, so `git log -p` shows
       `rename from`/`rename to` rather than a delete plus an add. */
    if (sg_diff_detect_renames(git_dir, NULL, &list, SG_SIMILARITY_DEFAULT, 0) != 0) {
        sg_diff_list_free(&list);
        return -1;
    }

    /* An EMPTY diff prints nothing at all -- no blank line, no `---`
       separator (measured on an empty commit). The separators below are part
       of the diff, not of the entry. */
    if (list.count == 0) {
        sg_diff_list_free(&list);
        return 0;
    }

    /* Measured, all four combinations: in the default format the diff is
       introduced by a blank line, except when --stat and -p are BOTH on, in
       which case git prints a literal `---` line instead. --oneline
       introduces neither, and never prints `---`. */
    if (!o->oneline)
        printf(o->stat && o->patch ? "---\n" : "\n");

    memset(&opts, 0, sizeof opts);
    opts.algorithm = SG_DIFF_ALGO_MYERS;
    if (o->stat) {
        opts.format = SG_DIFF_FORMAT_STAT;
        /* repo_root is NULL deliberately: a tree-vs-tree list is entirely
           SG_DIFF_SIDE_BLOB, and that branch of sg_diff_side_read never
           touches it (the same invariant Phase 49's merge rename detection
           relies on). */
        if (sg_diff_print(git_dir, NULL, &list, &opts) != 0)
            rc = -1;
    }
    if (rc == 0 && o->patch) {
        if (o->stat)
            printf("\n");
        opts.format = SG_DIFF_FORMAT_PATCH;
        if (sg_diff_print(git_dir, NULL, &list, &opts) != 0)
            rc = -1;
    }

    sg_diff_list_free(&list);
    return rc;
}

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
    log_opts o;
    int rc = 0;
    int first = 1;
    long shown = 0;
    int i;

    o.max_count = -1;
    o.oneline = 0;
    o.patch = 0;
    o.stat = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--oneline") == 0) {
            o.oneline = 1;
        } else if (strcmp(a, "-p") == 0 || strcmp(a, "--patch") == 0) {
            o.patch = 1;
        } else if (strcmp(a, "--stat") == 0) {
            o.stat = 1;
        } else if (strcmp(a, "-n") == 0) {
            if (i + 1 >= argc || parse_count(argv[i + 1], &o.max_count) != 0) {
                fprintf(stderr, "%s", USAGE);
                return 1;
            }
            i++;
        } else if (strncmp(a, "--max-count=", 12) == 0) {
            if (parse_count(a + 12, &o.max_count) != 0) {
                fprintf(stderr, "%s", USAGE);
                return 1;
            }
        } else if (a[0] == '-' && a[1] >= '0' && a[1] <= '9') {
            if (parse_count(a + 1, &o.max_count) != 0) {
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
        char hex[SG_SHA1_HEX_LEN + 1];
        char timebuf[SG_DATE_NORMAL_MAX];

        if (o.max_count >= 0 && shown >= o.max_count)
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

        sg_sha1_to_hex(id, hex);
        shown++;

        if (o.oneline) {
            /* No blank line between --oneline entries, with or without a
               diff attached (measured). */
            printf("%.*s ", SG_LOG_ABBREV, hex);
            print_subject(commit.message);
        } else {
            /* The stored offset is part of the answer, not decoration: the
               wall clock git shows is the timestamp SHIFTED INTO that offset.
               Reading it as UTC while still printing "+0800" beside it was
               wrong by the offset -- eight hours here -- and said so in its
               own output. */
            if (sg_date_format_normal(commit.author_time, commit.author_tz,
                                      timebuf, sizeof(timebuf)) != 0)
                timebuf[0] = '\0';

            /* One blank line BETWEEN entries and none after the last, so the
               separator belongs before every entry but the first. Measured:
               an entry whose message is empty still gets it, which is why
               this cannot be folded into the message block below. Keeping it
               here also leaves the first line of `sg log` a bare
               `commit <sha>`, which interop already parses with `head -1`. */
            if (!first)
                printf("\n");

            printf("commit %s\n", hex);
            if (commit.parent_count > 1) {
                /* first-parent-only walk (Phase 2 scope): the other parents
                   are never themselves visited, but a merge commit is still
                   flagged here so its extra history isn't silently
                   invisible. */
                size_t pi;

                printf("Merge:");
                for (pi = 0; pi < commit.parent_count; pi++) {
                    char phex[SG_SHA1_HEX_LEN + 1];

                    sg_sha1_to_hex(commit.parents[pi], phex);
                    printf(" %.*s", SG_LOG_ABBREV, phex);
                }
                printf("\n");
            }
            printf("Author: %s <%s>\n", commit.author_name, commit.author_email);
            printf("Date:   %s\n", timebuf);
            print_message(commit.message);
        }
        first = 0;

        if ((o.patch || o.stat) && print_commit_diff(git_dir, &commit, &o) != 0) {
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
