#include "sg/commit_out.h"

#include "sg/date.h"
#include "sg/diff.h"
#include "sg/diff_out.h"
#include "sg/objstore.h"
#include "sg/similarity.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <string.h>

/* ASCII-only case fold, deliberately not strcasecmp -- same reasoning as
   sg_path_component_is_safe's own comment in workdir.h: strcasecmp is
   locale-dependent, and the builtin format names are ASCII identifiers, so
   there is no reason to let a locale's idea of casing anywhere near this. */
static int ascii_ci_equal(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        char ca = *a, cb = *b;

        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static const struct {
    const char *name;
    sg_pretty_kind kind;
} PRETTY_BUILTINS[] = {
    { "oneline", SG_PRETTY_ONELINE },
    { "short", SG_PRETTY_SHORT },
    { "medium", SG_PRETTY_MEDIUM },
    { "full", SG_PRETTY_FULL },
    { "fuller", SG_PRETTY_FULLER },
    { "raw", SG_PRETTY_RAW },
    { "reference", SG_PRETTY_REFERENCE },
};

/* The five ordered grammar rules, CLAUDE.md's `sg log` entry has the full
   measured table. Rule order matters: "format:"/"tformat:" (case-SENSITIVE)
   are checked before the case-INSENSITIVE builtin lookup, which is in turn
   checked before the "contains a %" fallback -- e.g. "oneline%H" must NOT
   match rule 3 (whole-string equality, not a prefix). */
int sg_pretty_parse(const char *arg, sg_pretty_format *out)
{
    size_t i;

    if (arg == NULL || out == NULL)
        return -1;
    out->user_format = NULL;

    if (strncmp(arg, "format:", 7) == 0) {
        out->kind = SG_PRETTY_FORMAT;
        out->user_format = arg + 7;
        return 0;
    }
    if (strncmp(arg, "tformat:", 8) == 0) {
        out->kind = SG_PRETTY_TFORMAT;
        out->user_format = arg + 8;
        return 0;
    }
    for (i = 0; i < sizeof(PRETTY_BUILTINS) / sizeof(PRETTY_BUILTINS[0]); i++) {
        if (ascii_ci_equal(arg, PRETTY_BUILTINS[i].name)) {
            out->kind = PRETTY_BUILTINS[i].kind;
            return 0;
        }
    }
    if (strchr(arg, '%') != NULL) {
        out->kind = SG_PRETTY_TFORMAT;
        out->user_format = arg;
        return 0;
    }
    return -1;
}

/* git EXPANDS TABS in the message body for medium/full/fuller (measured in
   Phase 60 -- CLAUDE.md's earlier "only medium" note undersold this: `git
   log --pretty=full`/`fuller` on a message containing a tab also expand it,
   `--oneline`/`short`/`raw`/`%s` all leave the tab alone). `--expand-tabs=8`
   is the default for that bucket. The column is counted from the start of
   the MESSAGE line, NOT from the indented output column: a line of two tabs
   comes out at column 20, which is 16 expanded columns plus the four-space
   indent, and a line of "1234567\tx" comes out with a single space. Getting
   this from the output column instead would put the second one at a
   different stop.

   Found by `sg show` on a merge whose auto-generated message contains
   "#\tboth.txt"; it was wrong for `sg log` too, and had gone unnoticed
   because no fixture had ever put a tab in a commit message. */
static void print_message_line(const char *line, size_t len, int expand_tabs)
{
    size_t i;
    size_t col = 0;

    printf("    ");
    for (i = 0; i < len; i++) {
        if (expand_tabs && line[i] == '\t') {
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
static void print_message(const char *msg, int expand_tabs)
{
    const char *p = msg;

    if (msg == NULL || *msg == '\0')
        return;
    printf("\n");
    while (*p != '\0') {
        const char *nl = strchr(p, '\n');

        if (nl == NULL) {
            print_message_line(p, strlen(p), expand_tabs);
            break;
        }
        print_message_line(p, (size_t)(nl - p), expand_tabs);
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

/* short/reference print the SUBJECT ONLY, never the body -- measured
   (Phase 60a's oracle matrix): `short` looked like it was byte-exact when
   tested only against body-less fixtures, but a real body ("body first")
   showed up verbatim underneath it, which git never does. Reuses
   print_message_line's blank-line + four-space-indent shape (so a `raw`-
   or `medium`-shaped consumer sees the same left margin), just stops after
   the first line instead of walking the rest of the message. */
static void print_message_subject_only(const char *msg, int expand_tabs)
{
    const char *nl;
    size_t len;

    if (msg == NULL || *msg == '\0')
        return;
    nl = strchr(msg, '\n');
    len = nl == NULL ? strlen(msg) : (size_t)(nl - msg);
    printf("\n");
    print_message_line(msg, len, expand_tabs);
}

/* "Merge: <7hex> <7hex> ..." -- shared by short/medium/full/fuller (all
   measured to print it, Phase 60); `oneline`/`raw`/`reference` do not:
   `raw` prints a full "parent <40hex>" line per parent instead, and
   `oneline`/`reference` carry no parent info at all. */
static void print_merge_line(const sg_commit *commit)
{
    size_t pi;

    if (commit->parent_count <= 1)
        return;
    printf("Merge:");
    for (pi = 0; pi < commit->parent_count; pi++) {
        char phex[SG_SHA1_HEX_LEN + 1];

        sg_sha1_to_hex(commit->parents[pi], phex);
        printf(" %.*s", SG_COMMIT_OUT_ABBREV, phex);
    }
    printf("\n");
}

/* The seven builtins, section 3 of the Phase 60 spec -- byte-exact against
   real git 2.55.0, measured with commit-tree-built fixtures (including a
   2-parent merge and a root commit). `hex` is the already-computed 40-hex
   id, `author_date`/`committer_date` the already-rendered DATE_NORMAL
   strings (empty string on a formatting failure, same fallback
   sg_commit_out_entry's pre-Phase-60 code used). */
static void print_pretty_oneline(const char *hex, const sg_commit *commit)
{
    printf("%s ", hex);
    print_subject(commit->message);
}

static void print_pretty_short(const char *hex, const sg_commit *commit)
{
    printf("commit %s\n", hex);
    print_merge_line(commit);
    printf("Author: %s <%s>\n", commit->author_name, commit->author_email);
    /* short does NOT expand tabs (measured) -- only medium/full/fuller do.
       short is also SUBJECT-ONLY (measured, Phase 60a oracle round 2): the
       body, if any, is never printed. */
    print_message_subject_only(commit->message, 0);
}

static void print_pretty_medium(const char *hex, const sg_commit *commit,
                                const char *author_date)
{
    printf("commit %s\n", hex);
    print_merge_line(commit);
    printf("Author: %s <%s>\n", commit->author_name, commit->author_email);
    printf("Date:   %s\n", author_date);
    print_message(commit->message, 1);
}

static void print_pretty_full(const char *hex, const sg_commit *commit)
{
    printf("commit %s\n", hex);
    print_merge_line(commit);
    printf("Author: %s <%s>\n", commit->author_name, commit->author_email);
    printf("Commit: %s <%s>\n", commit->committer_name, commit->committer_email);
    print_message(commit->message, 1);
}

static void print_pretty_fuller(const char *hex, const sg_commit *commit,
                                const char *author_date, const char *committer_date)
{
    printf("commit %s\n", hex);
    print_merge_line(commit);
    /* Each of the four labels is padded to a 12-column field, "%-12s" does
       exactly that: "Author:" (7 chars) + 5 spaces, "AuthorDate:" (11
       chars) + 1 space, measured. */
    printf("%-12s%s <%s>\n", "Author:", commit->author_name, commit->author_email);
    printf("%-12s%s\n", "AuthorDate:", author_date);
    printf("%-12s%s <%s>\n", "Commit:", commit->committer_name, commit->committer_email);
    printf("%-12s%s\n", "CommitDate:", committer_date);
    print_message(commit->message, 1);
}

static void print_pretty_raw(const char *hex, const sg_commit *commit)
{
    char tree_hex[SG_SHA1_HEX_LEN + 1];
    size_t pi;

    sg_sha1_to_hex(commit->tree, tree_hex);
    printf("commit %s\n", hex);
    printf("tree %s\n", tree_hex);
    for (pi = 0; pi < commit->parent_count; pi++) {
        char phex[SG_SHA1_HEX_LEN + 1];

        sg_sha1_to_hex(commit->parents[pi], phex);
        printf("parent %s\n", phex);
    }
    printf("author %s <%s> %lld %s\n", commit->author_name, commit->author_email,
          commit->author_time, commit->author_tz);
    printf("committer %s <%s> %lld %s\n", commit->committer_name, commit->committer_email,
          commit->committer_time, commit->committer_tz);
    /* raw does NOT expand tabs (measured), unlike medium/full/fuller --
       print_message's own blank-line + four-space-indent shape still
       applies, that part is common to every builtin that shows a message. */
    print_message(commit->message, 0);
}

static void print_pretty_reference(const unsigned char id[SG_SHA1_RAW_LEN],
                                   const sg_commit *commit)
{
    char hex7[SG_SHA1_HEX_LEN + 1];
    char short_date[SG_DATE_SHORT_MAX];
    const char *msg = commit->message;
    const char *nl;
    int subject_len;

    sg_sha1_to_hex(id, hex7);
    if (msg == NULL) {
        subject_len = 0;
        msg = "";
    } else {
        nl = strchr(msg, '\n');
        subject_len = nl == NULL ? (int)strlen(msg) : (int)(nl - msg);
    }
    /* `reference` uses the AUTHOR date (short form, "YYYY-MM-DD"), NOT the
       committer's -- measured with a fixture whose two dates fall on
       different days. */
    if (sg_date_format_short(commit->author_time, commit->author_tz,
                             short_date, sizeof(short_date)) != 0)
        short_date[0] = '\0';
    printf("%.*s (%.*s, %s)\n", SG_COMMIT_OUT_ABBREV, hex7, subject_len, msg, short_date);
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
       line, never `---`.

       Phase 60: builtin `oneline` (opts->pretty->kind ==
       SG_PRETTY_ONELINE) joins --oneline in this exemption -- measured, no
       separator at all, same as legacy --oneline. Every other kind
       (including FORMAT/TFORMAT and every other builtin) falls through to
       the SAME blank-line/`---` print as before: FORMAT's own entry text
       has no trailing newline of its own, so the identical bytes read as
       "no blank line" there (measured -- see CLAUDE.md's `sg log` Phase 60
       entry for the byte-level derivation), while every terminator-bearing
       kind (TFORMAT, and the six other builtins) reads it as an actual
       blank line, exactly as it always has. */
    if (!o->oneline && !(o->pretty != NULL && o->pretty->kind == SG_PRETTY_ONELINE))
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

    if (opts->pretty != NULL) {
        char committer_timebuf[SG_DATE_NORMAL_MAX];

        switch (opts->pretty->kind) {
        case SG_PRETTY_ONELINE:
            print_pretty_oneline(hex, commit);
            break;
        case SG_PRETTY_SHORT:
            print_pretty_short(hex, commit);
            break;
        case SG_PRETTY_MEDIUM:
            if (sg_date_format_normal(commit->author_time, commit->author_tz,
                                      timebuf, sizeof(timebuf)) != 0)
                timebuf[0] = '\0';
            print_pretty_medium(hex, commit, timebuf);
            break;
        case SG_PRETTY_FULL:
            print_pretty_full(hex, commit);
            break;
        case SG_PRETTY_FULLER:
            if (sg_date_format_normal(commit->author_time, commit->author_tz,
                                      timebuf, sizeof(timebuf)) != 0)
                timebuf[0] = '\0';
            if (sg_date_format_normal(commit->committer_time, commit->committer_tz,
                                      committer_timebuf, sizeof(committer_timebuf)) != 0)
                committer_timebuf[0] = '\0';
            print_pretty_fuller(hex, commit, timebuf, committer_timebuf);
            break;
        case SG_PRETTY_RAW:
            print_pretty_raw(hex, commit);
            break;
        case SG_PRETTY_REFERENCE:
            print_pretty_reference(id, commit);
            break;
        case SG_PRETTY_FORMAT:
            /* No terminator -- the entry text is exactly opts->pretty->
               user_format, byte for byte (round 2 wires up placeholder
               expansion; the CLI layer has already rejected a `%` in this
               string before this function is ever reached, see this
               function's own header-comment precondition). */
            fputs(opts->pretty->user_format, stdout);
            break;
        case SG_PRETTY_TFORMAT:
            /* Always terminates with exactly one '\n' -- this is what makes
               tformat: differ byte-for-byte from format: on the exact same
               literal string (measured: "plain" -> "plain" vs "plain\n"). */
            fputs(opts->pretty->user_format, stdout);
            putchar('\n');
            break;
        case SG_PRETTY_LEGACY:
        default:
            /* Unreachable: a non-NULL opts->pretty is always one of the
               above. Falls through to the legacy branch below only if this
               ever changes without updating this switch. */
            goto legacy;
        }
    } else {
legacy:
        if (opts->oneline) {
            /* No blank line between --oneline entries, with or without a
               diff attached (measured). */
            printf("%.*s ", SG_COMMIT_OUT_ABBREV, hex);
            print_subject(commit->message);
        } else {
            /* The stored offset is part of the answer, not decoration: the
               wall clock git shows is the timestamp SHIFTED INTO that
               offset. Reading it as UTC while still printing "+0800" beside
               it was wrong by the offset -- eight hours here -- and said so
               in its own output. */
            if (sg_date_format_normal(commit->author_time, commit->author_tz,
                                      timebuf, sizeof(timebuf)) != 0)
                timebuf[0] = '\0';

            printf("commit %s\n", hex);
            print_merge_line(commit);
            printf("Author: %s <%s>\n", commit->author_name, commit->author_email);
            printf("Date:   %s\n", timebuf);
            print_message(commit->message, 1);
        }
    }

    if ((opts->patch || opts->stat || opts->name_only || opts->name_status) &&
       print_commit_diff(git_dir, commit, opts) != 0)
        return -1;

    return 0;
}
