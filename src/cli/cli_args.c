#include "sg/cli_args.h"

#include "sg/commit_out.h"
#include "sg/date.h"
#include "sg/hash.h"
#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/quote.h"
#include "sg/revparse.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
            sg_cli_report_ambiguous_oid(git_dir, arg, disambig);
            fprintf(stderr, "sg: ambiguous argument %s: short object ID is ambiguous; "
                           "use -- to separate revisions from paths\n",
                   sg_quote_path_delimited(arg));
            return -1;
        }

        is_rev = prc == 0;
        is_path = sg_cli_arg_exists_in_worktree(arg);

        if (is_rev && is_path) {
            fprintf(stderr, "sg: ambiguous argument %s: could be both a revision and a file; use -- to separate revisions from paths\n",
                   sg_quote_path_delimited(arg));
            return -1;
        }
        if (is_rev) {
            rev_count++;
            continue;
        }
        if (is_path)
            break;
        fprintf(stderr, "sg: ambiguous argument %s: not a revision, and no such path in the working directory; "
                       "use -- to separate revisions from paths\n",
               sg_quote_path_delimited(arg));
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
