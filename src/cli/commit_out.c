#include "sg/commit_out.h"

#include "sg/date.h"
#include "sg/diff.h"
#include "sg/diff_out.h"
#include "sg/objstore.h"
#include "sg/similarity.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <string.h>

/* git EXPANDS TABS in the message body -- `--expand-tabs=8` is the default
   for the medium format (and only for it: `--oneline` and `%s` leave the tab
   alone, measured). The column is counted from the start of the MESSAGE
   line, NOT from the indented output column: a line of two tabs comes out at
   column 20, which is 16 expanded columns plus the four-space indent, and a
   line of "1234567\tx" comes out with a single space. Getting this from the
   output column instead would put the second one at a different stop.

   Found by `sg show` on a merge whose auto-generated message contains
   "#\tboth.txt"; it was wrong for `sg log` too, and had gone unnoticed
   because no fixture had ever put a tab in a commit message. */
static void print_message_line(const char *line, size_t len)
{
    size_t i;
    size_t col = 0;

    printf("    ");
    for (i = 0; i < len; i++) {
        if (line[i] == '\t') {
            size_t stop = (col / 8 + 1) * 8;

            while (col < stop) {
                putchar(' ');
                col++;
            }
        } else {
            putchar(line[i]);
            col++;
        }
    }
    putchar('\n');
}

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
            print_message_line(p, strlen(p));
            break;
        }
        print_message_line(p, (size_t)(nl - p));
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
                             const sg_commit_out_opts *o)
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
       introduces neither, and never prints `---`.

       WARNING (found by a 159-probe oracle, Phase 59 round 2): the `---`
       rule is NOT just "stat && patch" -- o->stat/o->patch are no longer
       forced to 0 when a name format is active (see sg/commit_out.h's own
       WARNING on that point, and cmd_show.c's resolve_commit_out_opts,
       which stopped zeroing them once the redundant-guard mutation showed
       the renderer was the real enforcement point). So a caller reaching
       here with --name-only/--name-status can still have o->stat and
       o->patch both nonzero underneath (e.g. `-p --name-only --stat`), and
       NAME must win the separator decision the same way it already wins
       which sg_diff_print format gets called below -- an ordinary blank
       line, never `---`. */
    if (!o->oneline)
        printf((o->stat && o->patch && !o->name_only && !o->name_status) ? "---\n" : "\n");

    memset(&opts, 0, sizeof opts);
    opts.algorithm = SG_DIFF_ALGO_MYERS;
    if (o->name_only || o->name_status) {
        /* Section 3 of the Phase 59 spec: same blank-line rule as patch/
           stat above (already printed), same empty-diff-prints-nothing
           rule (already handled above), just a different sg_diff_print
           format -- reuses the same rename-detected `list`. */
        opts.format = o->name_status ? SG_DIFF_FORMAT_NAME_STATUS : SG_DIFF_FORMAT_NAME_ONLY;
        if (sg_diff_print(git_dir, NULL, &list, &opts) != 0)
            rc = -1;
    } else {
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
    }

    sg_diff_list_free(&list);
    return rc;
}

int sg_commit_out_entry(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                        const sg_commit *commit, const sg_commit_out_opts *opts)
{
    char hex[SG_SHA1_HEX_LEN + 1];
    char timebuf[SG_DATE_NORMAL_MAX];

    sg_sha1_to_hex(id, hex);

    if (opts->oneline) {
        /* No blank line between --oneline entries, with or without a diff
           attached (measured). */
        printf("%.*s ", SG_COMMIT_OUT_ABBREV, hex);
        print_subject(commit->message);
    } else {
        /* The stored offset is part of the answer, not decoration: the
           wall clock git shows is the timestamp SHIFTED INTO that offset.
           Reading it as UTC while still printing "+0800" beside it was
           wrong by the offset -- eight hours here -- and said so in its
           own output. */
        if (sg_date_format_normal(commit->author_time, commit->author_tz,
                                  timebuf, sizeof(timebuf)) != 0)
            timebuf[0] = '\0';

        printf("commit %s\n", hex);
        if (commit->parent_count > 1) {
            /* first-parent-only walk (Phase 2 scope): the other parents are
               never themselves visited, but a merge commit is still flagged
               here so its extra history isn't silently invisible. */
            size_t pi;

            printf("Merge:");
            for (pi = 0; pi < commit->parent_count; pi++) {
                char phex[SG_SHA1_HEX_LEN + 1];

                sg_sha1_to_hex(commit->parents[pi], phex);
                printf(" %.*s", SG_COMMIT_OUT_ABBREV, phex);
            }
            printf("\n");
        }
        printf("Author: %s <%s>\n", commit->author_name, commit->author_email);
        printf("Date:   %s\n", timebuf);
        print_message(commit->message);
    }

    if ((opts->patch || opts->stat || opts->name_only || opts->name_status) &&
       print_commit_diff(git_dir, commit, opts) != 0)
        return -1;

    return 0;
}
