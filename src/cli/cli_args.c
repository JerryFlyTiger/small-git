#include "sg/cli_args.h"

#include "sg/commit_out.h"
#include "sg/date.h"
#include "sg/hash.h"
#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/quote.h"
#include "sg/revparse.h"
#include "sg/workdir.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <assert.h>

void sg_cli_report_pathspec_error(sg_pathspec_error err, const char *arg, const char *repo_root)
{
    switch (err) {
    case SG_PATHSPEC_ERR_EMPTY:
        fprintf(stderr, "sg: an empty string is not a valid path; use . to match all paths\n");
        break;
    case SG_PATHSPEC_ERR_MAGIC:
        fprintf(stderr, "sg: unsupported pathspec magic: %s\n", sg_quote_path_delimited(arg));
        break;
    /* No `default:` on purpose -- see the header comment. */
    case SG_PATHSPEC_ERR_NONE:
    case SG_PATHSPEC_ERR_OUTSIDE:
        fprintf(stderr, "sg: %s is outside the repository %s\n",
               sg_quote_path_delimited(arg), sg_quote_path_delimited(repo_root));
        break;
    }
}

int sg_cli_arg_exists_in_worktree(const char *arg)
{
    struct stat st;

    if (sg_pathspec_looks_like_spec(arg))
        return 1;
    return lstat(arg, &st) == 0;
}

/* Re-derives the ambiguous hex run out of `as_typed` -- the same character-
   class scan sg_rev_effective_disambig performs (stop at the first ':', the
   <rev>:<path> form -- the ambiguity can only ever be in the <rev> half --
   then at the first '~'/'^'/"@{" suffix), just returning the string
   boundary rather than a mode decision. The MODE decision itself (which of
   these two triggers applies, and its priority against `disambig`) is NOT
   re-derived here -- see sg_rev_effective_disambig, the single function
   both this file and revparse.c call for that. Returns the prefix length
   (0 should not happen for an as_typed that actually produced -4 moments
   ago; defensive only). */
static size_t ambiguous_prefix_len(const char *as_typed)
{
    const char *colon = strchr(as_typed, ':');
    size_t limit = colon != NULL ? (size_t)(colon - as_typed) : strlen(as_typed);
    size_t i = 0;

    while (i < limit && as_typed[i] != '~' && as_typed[i] != '^' &&
          !(as_typed[i] == '@' && as_typed[i + 1] == '{'))
        i++;
    return i;
}

typedef struct {
    unsigned char id[SG_SHA1_RAW_LEN];
    sg_obj_type type;
} amb_candidate;

/* Section 3's ordering: tag, commit, tree, blob, then hex within a type. */
static int amb_type_rank(sg_obj_type t)
{
    switch (t) {
    case SG_OBJ_TAG:
        return 0;
    case SG_OBJ_COMMIT:
        return 1;
    case SG_OBJ_TREE:
        return 2;
    case SG_OBJ_BLOB:
        return 3;
    }
    return 4;
}

static int amb_cmp(const void *pa, const void *pb)
{
    const amb_candidate *a = pa;
    const amb_candidate *b = pb;
    int ra = amb_type_rank(a->type);
    int rb = amb_type_rank(b->type);

    if (ra != rb)
        return ra - rb;
    return memcmp(a->id, b->id, SG_SHA1_RAW_LEN);
}

/* Prints one "hint:" candidate line for a commit -- the folded subject and
   the author date in the commit's own stored offset, section 3b's rule. */
static void print_commit_candidate_line(const char *git_dir, const char *hex7,
                                        const unsigned char id[SG_SHA1_RAW_LEN])
{
    unsigned char *content;
    size_t content_len;
    sg_obj_type type;
    sg_commit commit;

    /* Split, do not merge, the two failure cases: sg_object_read leaves
       *content_out UNWRITTEN on every one of its own failure paths (loose
       and pack alike; sg_commit_tree_of / peel_to_non_tag / commit_nth_
       parent in revparse.c all follow the same "do not free on a failed
       read" convention), so a single `read() != 0 || parse() != 0` check
       followed by one unconditional free(content) is undefined behaviour
       on the read-failure side -- reachable via the enumerate-then-re-read
       race this function's own caller already documents (an object that
       vanishes, e.g. `git gc`, between the enumeration and this read). */
    if (sg_object_read(git_dir, id, &type, &content, &content_len) != 0) {
        fprintf(stderr, "hint:   %s commit\n", hex7);
        return;
    }
    if (sg_commit_parse(content, content_len, &commit) != 0) {
        free(content);
        fprintf(stderr, "hint:   %s commit\n", hex7);
        return;
    }
    free(content);
    {
        char date[SG_DATE_SHORT_MAX];
        char *subject;
        size_t subject_len;

        sg_date_format_short(commit.author_time, commit.author_tz, date, sizeof(date));
        subject = sg_commit_out_fold_subject_alloc(commit.message, &subject_len);
        fprintf(stderr, "hint:   %s commit %s - %.*s\n", hex7, date,
               (int)subject_len, subject != NULL ? subject : "");
        free(subject);
    }
    sg_commit_free(&commit);
}

/* Prints one "hint:" candidate line for a tag -- the TAGGER date and the
   tag's own NAME (not its message), section 3's rule. */
static void print_tag_candidate_line(const char *git_dir, const char *hex7,
                                     const unsigned char id[SG_SHA1_RAW_LEN])
{
    unsigned char *content;
    size_t content_len;
    sg_obj_type type;
    sg_tag tag;

    /* Same split as print_commit_candidate_line's own -- see its comment. */
    if (sg_object_read(git_dir, id, &type, &content, &content_len) != 0) {
        fprintf(stderr, "hint:   %s tag\n", hex7);
        return;
    }
    if (sg_tag_parse(content, content_len, &tag) != 0) {
        free(content);
        fprintf(stderr, "hint:   %s tag\n", hex7);
        return;
    }
    free(content);
    {
        char date[SG_DATE_SHORT_MAX];

        sg_date_format_short(tag.tagger_time, tag.tagger_tz, date, sizeof(date));
        fprintf(stderr, "hint:   %s tag %s - %s\n", hex7, date, tag.tag_name);
    }
    sg_tag_free(&tag);
}

void sg_cli_report_ambiguous_oid(const char *git_dir, const char *as_typed, sg_rev_disambig disambig)
{
    size_t prefix_len = ambiguous_prefix_len(as_typed);
    char prefix[SG_SHA1_HEX_LEN + 1];
    char lower[SG_SHA1_HEX_LEN + 1];
    sg_oid_list list;
    amb_candidate *cands;
    size_t i;
    sg_rev_disambig effective;
    size_t match_count;
    int filtering;

    if (prefix_len < SG_OID_MIN_ABBREV || prefix_len >= SG_SHA1_HEX_LEN)
        return;
    memcpy(prefix, as_typed, prefix_len);
    prefix[prefix_len] = '\0';
    for (i = 0; i < prefix_len; i++)
        lower[i] = (char)tolower((unsigned char)prefix[i]);
    lower[prefix_len] = '\0';

    if (sg_object_find_prefix(git_dir, prefix, &list) != 0 || list.count == 0)
        return;

    cands = malloc(list.count * sizeof(*cands));
    if (cands == NULL) {
        sg_oid_list_free(&list);
        return;
    }
    /* The single shared decision -- see sg_rev_effective_disambig's own
       comment for why this must not be re-derived independently here
       (trigger 1: a suffix; trigger 2: a colon -- both override whatever
       `disambig` the caller passed). */
    effective = sg_rev_effective_disambig(as_typed, disambig);
    /* Membership (match_count, and the per-row filter below) is by PEELED
       type (sg_rev_object_matches_disambig), never raw type -- see its own
       comment. This means a tag pointing at a blob can sit ANYWHERE in the
       sorted (by RAW type rank -- amb_type_rank, unaffected by peeling)
       list and still need to be individually excluded: it is no longer
       true that the excluded rows form one contiguous run at the tail, so
       the print loop below filters row by row rather than stopping at a
       rank cutoff (a review-round regression that WOULD have shipped: the
       first draft of this fix kept a "stop at the first non-matching rank"
       loop, which is correct for the type-only rule but wrong once
       membership can skip around inside the sorted order). */
    match_count = 0;
    for (i = 0; i < list.count; i++) {
        unsigned char *content;
        size_t content_len;

        memcpy(cands[i].id, list.ids[i], SG_SHA1_RAW_LEN);
        /* An object that vanished between the enumeration above and this
           read is folded to SG_OBJ_BLOB (sorts and prints last, and never
           matches any filter) rather than dropped -- the count of
           candidates git would report and sg would report should not
           silently differ. */
        if (sg_object_read(git_dir, list.ids[i], &cands[i].type, &content, &content_len) != 0) {
            cands[i].type = SG_OBJ_BLOB;
            continue;
        }
        free(content);
        if (sg_rev_object_matches_disambig(git_dir, list.ids[i], effective))
            match_count++;
    }
    qsort(cands, list.count, sizeof(*cands), amb_cmp);

    /* Measured against real git 2.55.0: under COMMITTISH or TREEISH
       disambiguation, once at least one candidate MATCHES that mode's
       filter, the hint list is NARROWED to just the matching ones (rows
       outside the filter are dropped entirely) -- e.g. `git log -1 <amb>`
       on a tag+commit+tree+blob collision lists only the tag and the
       commit (COMMITTISH), while `git cat-file -p <amb>:f.txt` on the SAME
       collision lists the tag, the commit, AND the tree (TREEISH) -- three
       different filter widths, not two. With zero matching candidates, or
       under STRICT (where match_count is always 0, since
       sg_rev_object_matches_disambig never matches anything under STRICT),
       the full list is shown -- `filtering` collapses both of those into
       the same "no filter" branch, no separate STRICT case needed. */
    filtering = match_count > 0;

    fprintf(stderr, "error: short object ID %s is ambiguous\n", lower);
    fprintf(stderr, "hint: The candidates are:\n");
    for (i = 0; i < list.count; i++) {
        char hex[SG_SHA1_HEX_LEN + 1];

        if (filtering && !sg_rev_object_matches_disambig(git_dir, cands[i].id, effective))
            continue;

        sg_sha1_to_hex(cands[i].id, hex);
        hex[7] = '\0';

        switch (cands[i].type) {
        case SG_OBJ_COMMIT:
            print_commit_candidate_line(git_dir, hex, cands[i].id);
            break;
        case SG_OBJ_TAG:
            print_tag_candidate_line(git_dir, hex, cands[i].id);
            break;
        case SG_OBJ_TREE:
        case SG_OBJ_BLOB:
            fprintf(stderr, "hint:   %s %s\n", hex, sg_obj_type_name(cands[i].type));
            break;
        }
    }

    free(cands);
    sg_oid_list_free(&list);
}

int sg_cli_split_revs_and_paths(const char *git_dir, char **pos, int n_pos, const char *cmd_name,
                                sg_rev_disambig disambig)
{
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    int rev_count = 0;
    int i;

    while (rev_count < n_pos) {
        const char *arg = pos[rev_count];
        int prc = sg_rev_parse_commit_ex(git_dir, arg, disambig, commit_id);
        int is_rev;
        int is_path;

        /* -4 is still a revision ATTEMPT (section 7.4 of the Phase 68
           spec) -- report the ambiguity and fail outright, do not fall
           through to "maybe it is a pathspec". Real git agrees: `git diff
           --stat <amb>` prints the error:/hint: block rather than treating
           the string as a path. */
        if (prc == -4) {
            /* Class S: sg already reproduces git's error:/hint: block byte
               for byte; only the final line differs, and it is the
               class-R line for the calling command (phase75). */
            sg_cli_report_ambiguous_oid(git_dir, arg, disambig);
            sg_cli_report_rev_error(cmd_name, SG_REV_ERR_NOT_A_REV, arg, NULL, 0);
            return -1;
        }

        is_rev = prc == 0;
        is_path = sg_cli_arg_exists_in_worktree(arg);

        if (is_rev && is_path) {
            sg_cli_report_rev_error(cmd_name, SG_REV_ERR_BOTH, arg, NULL, 0);
            return -1;
        }
        if (is_rev) {
            rev_count++;
            continue;
        }
        if (is_path)
            break;
        {
            char bad_path[SG_PATH_MAX];
            sg_rev_err_kind kind = sg_cli_classify_rev_error(git_dir, arg, bad_path, sizeof(bad_path));

            sg_cli_report_rev_error(cmd_name, kind, arg, bad_path, 0);
        }
        return -1;
    }

    for (i = rev_count; i < n_pos; i++) {
        if (!sg_cli_arg_exists_in_worktree(pos[i])) {
            fprintf(stderr, "sg: %s: no such path in the working directory; "
                           "to name a path that does not exist, use sg %s -- <path>\n",
                   sg_quote_path_delimited(pos[i]), cmd_name);
            return -1;
        }
    }
    return rev_count;
}

sg_rev_err_kind sg_cli_classify_rev_error(const char *git_dir, const char *arg,
                                          char *bad_path, size_t bad_path_size)
{
    unsigned char id[SG_SHA1_RAW_LEN];
    sg_obj_type type;
    int rc;

    if (bad_path != NULL && bad_path_size > 0)
        bad_path[0] = '\0';
    rc = sg_rev_parse_object(git_dir, arg, id, &type, bad_path, bad_path_size);
    if (rc == -2)
        return SG_REV_ERR_MISSING_PATH;
    if (rc == -3)
        return SG_REV_ERR_MISSING_OBJ;
    /* -1 (not a rev at all) and -4 (ambiguous, which the caller is expected
       to have already handled via sg_cli_report_ambiguous_oid before ever
       reaching here) both fold to "not a revision" -- there is no fifth
       wording for a classify() call that raced its own caller's -4. */
    return SG_REV_ERR_NOT_A_REV;
}

/* Control-byte sanitizer, phase75 translation rule 6, measured byte-by-byte
   against `git cat-file -p`'s own vreportf: every byte 0x01-0x08,
   0x0b-0x1f, and 0x7f becomes '?'; tab (0x09), newline (0x0a), space
   (0x20), and every byte >= 0x80 pass through raw. Takes an explicit
   caller-owned buffer rather than a static one -- a message like "path 'p'
   does not exist in 'r'" embeds two independently-sanitized arguments in
   the same fprintf, and a single static buffer would silently alias them
   (a mutation test proves this: see tests/test_cli_rev_err.c). */
static char *sanitize_rev_err_arg(const char *s, char *buf, size_t buflen)
{
    size_t i = 0;

    /* buflen == 0 would make the unconditional buf[i] = '\0' below a
       one-byte overflow -- no call site can reach it today (every one
       passes a real stack array via `sizeof`), but the guard costs
       nothing and removes the possibility outright rather than leaving it
       to keep being true by convention. */
    if (buflen == 0)
        return buf;
    for (; s[i] != '\0' && i + 1 < buflen; i++) {
        unsigned char c = (unsigned char)s[i];

        buf[i] = (c <= 0x1f && c != 0x09 && c != 0x0a) || c == 0x7f ? '?' : (char)c;
    }
    buf[i] = '\0';
    return buf;
}

/* Deliberately its OWN named constant, not SG_PATH_MAX: a revision
   argument is arbitrary user input sized to match git's own vreportf
   message buffer (see docs/DESIGN.md's Phase 75 long-argument-truncation
   note), not a filesystem path -- docs/RULES-duplication.md's convergence
   onto SG_PATH_MAX is specifically about buffers that hold a path. */
#define SG_REV_ERR_ARG_MAX 4096

typedef struct {
    const char *cmd;
    /* NULL means "print the three-line AMBIG-UNKNOWN block instead of a
       one-line fprintf". */
    const char *r;
    /* The class-O wording. o_literal means it embeds no argument at all
       (cat-file -t/-s's "could not get object info", which git's own
       sha1_object_info path never names the object in). o_tag_style means
       it takes TWO embedded strings in (detail, arg) order -- only `tag`'s
       row uses this, git's own line names the ref being created before the
       object that does not exist. */
    const char *o;
    int o_literal;
    int o_tag_style;
    /* Class P: 1 means "use `r`'s formatting with the whole `arg`" (tag's
       and merge-base's and cherry-pick/revert's oddity: their P column is
       byte-identical to their R column, not the two-part path message);
       0 means the standard "path '%s' does not exist in '%s'" message. */
    int p_uses_r;
    /* Class D: 1 means an AMBIG-BOTH block; 0 means "no check" (the caller
       never asks this table for a D verdict on that command, so this flag
       only documents the row -- it isn't tested by the print function). */
    int has_d;
    /* dashdash-present variants; NULL means "this command has no `--`-
       present row in the phase75 table" (r_dd doubles as "does this
       command's grammar even have a rev/path '--' separator", so a NULL
       here also means class P is NEVER collapsed into it, dashdash=1 is
       simply not reachable for that command's caller today). */
    const char *r_dd;
    const char *o_dd;
} sg_rev_err_row;

/* Every row measured against real git 2.55.0, LC_ALL=C -- see docs/DESIGN.md's
   Phase 75 section for the full table this mirrors. Do not add a strcmp
   chain inside a class instead of a new row here: a missing command must be
   a missing ROW (caught by the assert in sg_cli_report_rev_error), not a
   silently inherited default. */
static const sg_rev_err_row REV_ERR_TABLE[] = {
    { "tag", "Failed to resolve '%s' as a valid ref.",
      "trying to write ref 'refs/tags/%s' with nonexistent object %s", 0, 1,
      /*p_uses_r*/ 1, /*has_d*/ 0, NULL, NULL },
    { "show", NULL, "bad object %s", 0, 0,
      /*p_uses_r*/ 0, /*has_d*/ 1, "bad revision '%s'", "bad object %s" },
    { "cat-file-p", "Not a valid object name %s", "Not a valid object name %s", 0, 0,
      /*p_uses_r*/ 0, /*has_d*/ 0, NULL, NULL },
    { "cat-file-ts", "Not a valid object name %s", "sg cat-file: could not get object info", 1, 0,
      /*p_uses_r*/ 0, /*has_d*/ 0, NULL, NULL },
    { "log", NULL, "bad object %s", 0, 0,
      /*p_uses_r*/ 0, /*has_d*/ 1, "bad revision '%s'", "bad object %s" },
    { "diff", NULL, "bad object %s", 0, 0,
      /*p_uses_r*/ 0, /*has_d*/ 1, "bad revision '%s'", "bad object %s" },
    { "reset", NULL, "Could not parse object '%s'.", 0, 0,
      /*p_uses_r*/ 0, /*has_d*/ 1, "Failed to resolve '%s' as a valid revision.",
      "Could not parse object '%s'." },
    { "reflog", NULL, "bad object %s", 0, 0,
      /*p_uses_r*/ 0, /*has_d*/ 1, "bad revision '%s'", "bad object %s" },
    { "merge-base", "Not a valid object name %s", "Not a valid commit name %s", 0, 0,
      /*p_uses_r*/ 1, /*has_d*/ 0, NULL, NULL },
    { "cherry-pick", "bad revision '%s'", "bad object %s", 0, 0,
      /*p_uses_r*/ 1, /*has_d*/ 0, NULL, NULL },
    { "revert", "bad revision '%s'", "bad object %s", 0, 0,
      /*p_uses_r*/ 1, /*has_d*/ 0, NULL, NULL },
    /* switch's R column already matched git before this phase; only its O
       column did not (measured: `git switch <40-hex-with-no-object>` and
       `git switch --detach` of the same both say "unable to read tree",
       while a name, a short id, an out-of-range HEAD~n and a <rev>:<path>
       all stay "invalid reference"). `git restore --source` and `git
       checkout` give the identical "unable to read tree" line, which is
       why this row is named for the tree read rather than for switch. */
    { "switch", "invalid reference: %s", "unable to read tree (%s)", 0, 0,
      /*p_uses_r*/ 1, /*has_d*/ 0, NULL, NULL },
    /* Phase 76: `sg branch`'s <start-point>. Measured against real git
       2.55.0 -- class R covers BOTH "does not resolve at all" (a
       nonexistent name, an empty string) AND "resolved but the path half
       of a <rev>:<path> is missing" (`branch new HEAD:nosuch.txt` ->
       "not a valid object name: 'HEAD:nosuch.txt'", the WHOLE arg, hence
       p_uses_r=1, same shape as tag/merge-base/cherry-pick/revert/switch).
       Class O is for a well-formed 40-hex id whose object is missing --
       "not a valid branch point: '<hex>'", with NO preceding "error:
       object ... is a X" line (that line only appears for the DIFFERENT
       "resolves, but to a tree/blob" case, which this table has no cell
       for at all: cmd_branch.c prints that one directly, since it needs
       the object's actual type name and there is no fifth sg_rev_err_kind
       for "wrong type" today). */
    { "branch", "not a valid object name: '%s'", "not a valid branch point: '%s'", 0, 0,
      /*p_uses_r*/ 1, /*has_d*/ 0, NULL, NULL },
};

static const sg_rev_err_row *find_rev_err_row(const char *cmd)
{
    size_t i;

    for (i = 0; i < sizeof(REV_ERR_TABLE) / sizeof(REV_ERR_TABLE[0]); i++) {
        if (strcmp(REV_ERR_TABLE[i].cmd, cmd) == 0)
            return &REV_ERR_TABLE[i];
    }
    return NULL;
}

static void print_ambig_block(const char *arg, int both)
{
    char san[SG_REV_ERR_ARG_MAX];

    sanitize_rev_err_arg(arg, san, sizeof(san));
    fprintf(stderr, "sg: ambiguous argument '%s': %s\n", san,
           both ? "both revision and filename"
                : "unknown revision or path not in the working tree.");
    fprintf(stderr, "Use '--' to separate paths from revisions, like this:\n");
    /* "<command>" is a literal in git's own hint line too -- NOT replaced
       by the actual subcommand name (phase75 translation rule 3). */
    fprintf(stderr, "'sg <command> [<revision>...] -- [<file>...]'\n");
}

void sg_cli_report_rev_error(const char *cmd, sg_rev_err_kind kind, const char *arg,
                             const char *detail, int dashdash)
{
    const sg_rev_err_row *row = find_rev_err_row(cmd);
    char san_arg[SG_REV_ERR_ARG_MAX];
    char san_detail[SG_REV_ERR_ARG_MAX];

    /* An unrecognized cmd name is a programmer error (a new call site that
       forgot to add its row), not a possible user input -- fail loudly in
       a debug build rather than silently borrowing another command's
       wording, and fall back to the class-R wording in a release build so
       a NDEBUG run at least prints something recognizable as an error. */
    assert(row != NULL);
    if (row == NULL) {
        sanitize_rev_err_arg(arg, san_arg, sizeof(san_arg));
        fprintf(stderr, "sg: not a valid revision '%s'\n", san_arg);
        return;
    }

    sanitize_rev_err_arg(arg, san_arg, sizeof(san_arg));

    if (kind == SG_REV_ERR_BOTH) {
        print_ambig_block(arg, 1);
        return;
    }

    if (kind == SG_REV_ERR_MISSING_PATH) {
        if (dashdash && row->r_dd != NULL) {
            /* With "--" present, class P collapses into the dashdash-R
               wording using the WHOLE argument (measured: `log
               HEAD:nosuchfile --` -> "bad revision 'HEAD:nosuchfile'", not
               a path message at all). */
            fprintf(stderr, "sg: ");
            fprintf(stderr, row->r_dd, san_arg);
            fputc('\n', stderr);
            return;
        }
        if (row->p_uses_r) {
            fprintf(stderr, "sg: ");
            fprintf(stderr, row->r, san_arg);
            fputc('\n', stderr);
            return;
        }
        {
            const char *colon = strchr(arg, ':');
            size_t rev_len = colon != NULL ? (size_t)(colon - arg) : strlen(arg);
            char rev_part[SG_REV_ERR_ARG_MAX];
            size_t copy_len = rev_len < sizeof(rev_part) - 1 ? rev_len : sizeof(rev_part) - 1;

            memcpy(rev_part, arg, copy_len);
            rev_part[copy_len] = '\0';
            sanitize_rev_err_arg(detail != NULL ? detail : "", san_detail, sizeof(san_detail));
            sanitize_rev_err_arg(rev_part, san_arg, sizeof(san_arg));
            fprintf(stderr, "sg: path '%s' does not exist in '%s'\n", san_detail, san_arg);
        }
        return;
    }

    if (kind == SG_REV_ERR_MISSING_OBJ) {
        const char *fmt = dashdash && row->o_dd != NULL ? row->o_dd : row->o;

        if (row->o_literal) {
            fprintf(stderr, "sg: %s\n", fmt);
            return;
        }
        fprintf(stderr, "sg: ");
        if (row->o_tag_style) {
            char san_tag[SG_REV_ERR_ARG_MAX];

            sanitize_rev_err_arg(detail != NULL ? detail : "", san_tag, sizeof(san_tag));
            fprintf(stderr, fmt, san_tag, san_arg);
        } else {
            fprintf(stderr, fmt, san_arg);
        }
        fputc('\n', stderr);
        return;
    }

    /* SG_REV_ERR_NOT_A_REV */
    if (dashdash && row->r_dd != NULL) {
        fprintf(stderr, "sg: ");
        fprintf(stderr, row->r_dd, san_arg);
        fputc('\n', stderr);
        return;
    }
    if (row->r == NULL) {
        print_ambig_block(arg, 0);
        return;
    }
    fprintf(stderr, "sg: ");
    fprintf(stderr, row->r, san_arg);
    fputc('\n', stderr);
}
