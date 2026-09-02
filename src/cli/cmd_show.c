#include "sg/cli.h"

#include "sg/chunk.h"
#include "sg/commit_out.h"
#include "sg/date.h"
#include "sg/diff.h"
#include "sg/diff_out.h"
#include "sg/hash.h"
#include "sg/objstore.h"
#include "sg/object.h"
#include "sg/quote.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/revparse.h"
#include "sg/similarity.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char USAGE[] =
    "usage: sg show [-s] [-p|--patch] [--stat] [--name-only] [--name-status] "
    "[--oneline] [--pretty[=<fmt>]|--format=<fmt>] [<object>...]\n";

/* Phase 59 spec section 2: five bits, NOT Phase 55a's "last one wins" over
   just {patch, stat}. Measured over 29 flag combinations, and one detail
   the spec's prose glosses over turned out to be load-bearing (found by
   probing real git directly, since two of the spec's own example rows --
   "-s --name-only" errors, but "-s -p --name-only" does NOT -- are
   otherwise unreconcilable): `-s` clears every bit below and then sets
   no_output; `-p`/`--stat` each set their own bit AND clear no_output (an
   explicit format request cancels "no output"); `--name-only`/
   `--name-status` each set their own bit and do NOT touch no_output or one
   another's bit. All 16 of the spec's falsifying rows were reproduced
   against real git 2.55.0 with this exact rule and none other. */
typedef struct {
    int format_seen; /* any of -s/-p/--stat/--name-only/--name-status seen */
    int oneline;
    int patch;
    int stat;
    int name_only;
    int name_status;
    int no_output; /* -s, and only while nothing since has cleared it */
    /* Phase 60: --pretty/--format. Deliberately NOT one of the format_seen
       bits above -- measured, `git show --pretty=oneline` with no other
       flag still defaults to printing a patch, and `-s` still suppresses it
       exactly as it would without --pretty. pretty_set tells
       resolve_commit_out_opts whether to hand out a pointer to `pretty`;
       `pretty` itself lives here (not on sg_cmd_show's stack as a bare
       local) so its address stays valid for as long as `flags` does, which
       is the entire render loop. */
    int pretty_set;
    sg_pretty_format pretty;
} show_flags;

/* Mirrors cmd_log.c's own resolve_pretty_arg (not shared via a header --
   each command's diagnostic differs only in the exit convention, and
   there's no third caller to justify extracting a fourth file for two
   nearly-identical wrappers around sg_pretty_parse). As of Phase 60b, a
   FORMAT/TFORMAT user_format's placeholders are validated against
   sg_pretty_validate_format's table up front -- section 5.3 of the Phase
   60 spec. Returns 0 with *out filled, or -1 having already printed a
   diagnostic. */
static int resolve_pretty_arg(const char *raw, sg_pretty_format *out)
{
    if (sg_pretty_parse(raw, out) != 0) {
        fprintf(stderr, "sg: invalid --pretty format: %s\n", raw);
        return -1;
    }
    if (out->kind == SG_PRETTY_FORMAT || out->kind == SG_PRETTY_TFORMAT) {
        const char *bad = NULL;
        size_t bad_len = 0;

        if (sg_pretty_validate_format(out->user_format, &bad, &bad_len) != 0) {
            fprintf(stderr, "sg: unsupported --pretty placeholder '%.*s'\n", (int)bad_len, bad);
            return -1;
        }
    }
    return 0;
}

/* Same bound sg_rev_parse_commit's own tag-peeling loop uses, for the same
   reason: a self-referential or cyclic chain of tag objects must fail
   cleanly instead of recursing forever. */
#define SG_SHOW_MAX_TAG_HOPS 10

/* Commit ids already rendered in this invocation -- measured against real
   git: `sg show HEAD HEAD` and `sg show lw v2` (a lightweight tag and an
   annotated tag pointing at the SAME commit) print that commit only ONCE,
   with no trace -- not even a blank line -- of the second reference. This
   is real git's commit revision walker's "already seen" flag, which only
   commits pass through: TREES, BLOBS, and -- easy to get backwards, also
   measured -- TAG OBJECTS THEMSELVES never dedupe this way. `sg show <tree>
   <tree>` prints the listing twice, and `sg show v2 v2` prints the tag's own
   header/message block twice in full; only the COMMIT each v2 unwraps to is
   deduped away the second time (so the second v2 prints its header, then
   stops -- no blank line, no commit block). */
typedef struct {
    unsigned char (*ids)[SG_SHA1_RAW_LEN];
    size_t count;
    size_t cap;
} seen_ids;

static void seen_ids_free(seen_ids *s)
{
    free(s->ids);
    s->ids = NULL;
    s->count = 0;
    s->cap = 0;
}

static int seen_contains(const seen_ids *s, const unsigned char id[SG_SHA1_RAW_LEN])
{
    size_t i;

    for (i = 0; i < s->count; i++)
        if (memcmp(s->ids[i], id, SG_SHA1_RAW_LEN) == 0)
            return 1;
    return 0;
}

/* Returns 0, or -1 on OOM. */
static int seen_add(seen_ids *s, const unsigned char id[SG_SHA1_RAW_LEN])
{
    if (s->count == s->cap) {
        size_t new_cap = s->cap == 0 ? 8 : s->cap * 2;
        void *p = realloc(s->ids, new_cap * sizeof(*s->ids));

        if (p == NULL)
            return -1;
        s->ids = p;
        s->cap = new_cap;
    }
    memcpy(s->ids[s->count], id, SG_SHA1_RAW_LEN);
    s->count++;
    return 0;
}

static void resolve_commit_out_opts(const show_flags *f, sg_commit_out_opts *o)
{
    o->oneline = f->oneline;
    o->name_only = f->name_only;
    o->name_status = f->name_status;
    o->patch = f->patch;
    o->stat = f->stat;
    /* Phase 60: --pretty/--format, borrowed from `flags` itself (see
       show_flags's own comment on why `pretty` lives there rather than on
       some caller's stack frame) -- deliberately NOT part of format_seen
       below, measured (`git show --pretty=oneline` alone still defaults to
       a patch). */
    o->pretty = f->pretty_set ? &f->pretty : NULL;
    /* Default is a patch -- measured, and differs from `sg stash show`,
       whose default is --stat. --oneline does NOT count as a format
       selector here: `git show --oneline` still prints the patch. */
    if (!f->format_seen) {
        o->patch = 1;
        o->stat = 0;
    }
    /* Section 2: NAME or NAME_STATUS suppresses PATCH and STAT entirely, in
       either order. This is deliberately NOT done by zeroing o->patch/
       o->stat here -- both render paths (commit_out.c's print_commit_diff
       and this file's render_merge_diff) already check o->name_only/
       o->name_status FIRST and dispatch the name format instead, ignoring
       patch/stat's value entirely, so a second zeroing here would be a
       redundant guard one layer above the real one (measured by mutation:
       breaking this exact suppression left every check green, because
       nothing downstream ever reads a stale o->patch/o->stat once a name
       format is active). no_output does NOT suppress patch/stat (`-s -p`
       still prints a patch) -- the argv loop's own error check already
       guarantees at most one of {name_only, name_status, no_output}
       survives to here, so there is no simultaneous-both-set case to
       arbitrate. */
}

/* Phase 60a oracle round 2: an ANNOTATED TAG's header follows a DIFFERENT
   rule per builtin -- not just oneline. Measured against real git 2.55.0,
   all seven rows (fixture: an annotated tag on a merge commit):

     format     Tagger:   date line          blank before target
     oneline    no        no                 no
     short      yes       no                 yes
     medium     yes       Date:              yes
     full       yes       no                 yes
     fuller     yes       TaggerDate: (12)   yes
     raw        yes       no                 yes
     reference  yes       no                 no

   The pattern (deliberately captured as data, not seven `if`s, so a future
   format only has to add one table row): a date line appears ONLY for
   medium/fuller (the same two that show a date line on a COMMIT header),
   and the post-message blank line before the target is suppressed ONLY for
   oneline/reference (the same two whose ENTRY rendering carries no
   separator elsewhere in this file). fuller additionally right-pads each
   label to a 12-column field and calls the date line "TaggerDate:", same
   as its commit-header counterpart. */
typedef struct {
    int show_tagger;
    int show_date;      /* Date:/TaggerDate: line */
    int fuller_pad;      /* pad label to 12 cols, use "TaggerDate:" */
    int blank_before_target;
} tag_header_shape;

/* Legacy --oneline (no --pretty at all) reuses the builtin oneline row, and
   legacy medium (neither --oneline nor --pretty) reuses the builtin medium
   row -- both are the pre-Phase-60 behavior, unchanged. */
static tag_header_shape resolve_tag_header_shape(const show_flags *f)
{
    sg_pretty_kind kind;
    tag_header_shape s;

    if (f->pretty_set)
        kind = f->pretty.kind;
    else
        kind = f->oneline ? SG_PRETTY_ONELINE : SG_PRETTY_MEDIUM;

    s.show_tagger = 1;
    s.show_date = 0;
    s.fuller_pad = 0;
    s.blank_before_target = 1;

    switch (kind) {
    case SG_PRETTY_ONELINE:
        s.show_tagger = 0;
        s.blank_before_target = 0;
        break;
    case SG_PRETTY_REFERENCE:
        s.blank_before_target = 0;
        break;
    case SG_PRETTY_MEDIUM:
        s.show_date = 1;
        break;
    case SG_PRETTY_FULLER:
        s.show_date = 1;
        s.fuller_pad = 1;
        break;
    case SG_PRETTY_SHORT:
    case SG_PRETTY_FULL:
    case SG_PRETTY_RAW:
    case SG_PRETTY_FORMAT:
    case SG_PRETTY_TFORMAT:
    case SG_PRETTY_LEGACY:
    default:
        /* short/full/raw's measured row, and the fallback for a user
           format (FORMAT/TFORMAT) pointing at a tag -- untested by the
           Phase 60a oracle (that is 60b's placeholder-rendering territory),
           kept at this shape rather than guessed further. */
        break;
    }
    return s;
}

/* True when a NESTED target (immediately following an annotated tag) must
   NOT get its leading blank line. Phase 59 measured this for legacy
   --oneline; Phase 60a's oracle additionally measured it for the
   `reference` builtin (both oneline and reference tag headers omit the
   blank line described in tag_header_shape's own comment above; every
   other one of the seven builtins does not). Kept as its own predicate
   (mirroring resolve_tag_header_shape's blank_before_target, but callable
   from the COMMIT/TAG/TREE cases below, which do not have a
   tag_header_shape in hand -- only the tag case does). */
static int nested_target_suppresses_blank(const show_flags *f)
{
    if (f->pretty_set)
        return f->pretty.kind == SG_PRETTY_ONELINE || f->pretty.kind == SG_PRETTY_REFERENCE;
    return f->oneline;
}

/* Phase 55b: `sg show <merge>` renders a 2-parent merge (a dense combined
   diff) but still refuses an OCTOPUS (>2 parents) merge whose combined diff
   is non-empty -- the renderer is fixed at two parents, and there is no
   N-way `diff --cc` here. A clean octopus (every path already agrees with
   at least one parent, i.e. the combined row count is 0) is NOT refused:
   git itself prints just the entry header for one, with no diff to render
   at all, so there is nothing this command cannot do.

   Returns 1 when the commit must be refused, 0 otherwise -- including
   every case this cannot resolve (a corrupt tree, an OOM), the same
   fail-open-to-the-ordinary-path convention target_is_merge documents
   below: those get a better error message once the ordinary path actually
   tries to read them. Returns 0 outright for a 2-or-fewer-parent commit,
   since only an octopus can ever need refusing. */
static int commit_needs_refusal(const char *git_dir, const sg_commit *commit)
{
    unsigned char (*parent_trees)[SG_SHA1_RAW_LEN];
    size_t i;
    size_t row_count = 0;
    sg_diff_list list;
    char bad_path[SG_PATH_MAX];
    int refuse = 0;

    if (commit->parent_count <= 2)
        return 0;

    parent_trees = malloc(commit->parent_count * sizeof(*parent_trees));
    if (parent_trees == NULL)
        return 0;

    for (i = 0; i < commit->parent_count; i++) {
        if (sg_commit_tree_of(git_dir, commit->parents[i], parent_trees[i]) != 0) {
            free(parent_trees);
            return 0;
        }
    }

    bad_path[0] = '\0';
    if (sg_diff_combined_from_trees(git_dir, parent_trees, commit->parent_count, commit->tree,
                                    &list, &row_count, bad_path) == 0) {
        refuse = row_count > 0;
        sg_diff_list_free(&list);
    }
    free(parent_trees);
    return refuse;
}

/* Follows a tag chain to the object it ultimately names and reports whether
   showing that object must be refused. Exists only because a refused merge
   is refused before anything is printed for it: the tag header is printed
   before its target is ever read, so without looking ahead, `sg show
   <tag-pointing-at-a-refused-merge>` writes an entry to stdout and THEN
   exits non-zero. A command that reports failure should not have dirtied
   stdout on the way. Returns 1 when the target must be refused, 0
   otherwise, and 0 for anything it cannot resolve -- the ordinary path
   reports those errors with a better message than a look-ahead could. */
static int target_is_merge(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                           int hops, int check_refusal)
{
    unsigned char cur[SG_SHA1_RAW_LEN];
    int i;

    memcpy(cur, id, SG_SHA1_RAW_LEN);
    for (i = hops; i < SG_SHOW_MAX_TAG_HOPS; i++) {
        unsigned char *content;
        size_t content_len;
        sg_obj_type type;
        int answer = 0;

        if (sg_object_read(git_dir, cur, &type, &content, &content_len) != 0)
            return 0;
        if (type == SG_OBJ_COMMIT) {
            sg_commit commit;

            /* check_refusal is precomputed by the caller from show_flags,
               mirroring render_id's own "commit.parent_count > 2 && o.patch
               && !(o.name_only || o.name_status)" gate exactly (Phase 59
               round 2): --name-only/--name-status never refuse an octopus
               at all (they route through render_octopus_names, not the
               fixed-at-2-parents dense renderer this refusal protects), and
               neither does any request that would not actually render the
               dense PATCH (a bare --stat, or -s). */
            if (!check_refusal) {
                free(content);
                return 0;
            }
            if (sg_commit_parse(content, content_len, &commit) == 0) {
                answer = commit_needs_refusal(git_dir, &commit);
                sg_commit_free(&commit);
            }
            free(content);
            return answer;
        }
        if (type != SG_OBJ_TAG) {
            free(content);
            return 0;
        }
        {
            sg_tag tag;

            if (sg_tag_parse(content, content_len, &tag) != 0) {
                free(content);
                return 0;
            }
            memcpy(cur, tag.object, SG_SHA1_RAW_LEN);
            sg_tag_free(&tag);
            free(content);
        }
    }
    return 0;
}

/* --name-only/--name-status for a merge whose parent count is NOT two
   (Phase 59 section 4.1): sg_diff_combined_from_trees's `out` list is only
   ever populated at parent_count == 2 (the diff --cc renderer it exists for
   needs exactly two sides), so for any other count this walks every parent
   tree plus the result tree directly instead, duplicating that function's
   "differs from every parent" union-walk rule (mode AND id both compared)
   rather than extending it -- extending it is out of this phase's budget
   (workdir/diff.c is not one of the files this phase may touch), and the
   two rules must stay identical, which is why the comments here explicitly
   mirror sg_diff_combined_from_trees's own.

   One status letter per parent: 'A' when the path is absent from that
   parent, 'D' when present there but absent from the result, 'M'
   otherwise -- measured against a real N-column merge (section 4.1's
   table). --name-only prints the path once regardless of parent count.

   Returns 0, -1, or -2 with bad_path filled (SG_PATH_MAX bytes) --
   sg_tree_flatten's own contract, propagated the same way
   sg_diff_combined_from_trees propagates it. */
static int render_octopus_names(const char *git_dir,
                                const unsigned char (*parent_trees)[SG_SHA1_RAW_LEN],
                                size_t parent_count, const unsigned char result_tree[SG_SHA1_RAW_LEN],
                                int name_status, char *bad_path)
{
    sg_flat_list *parents;
    size_t *parent_cursor;
    const sg_flat_entry **parent_entry;
    sg_flat_list result_flat;
    size_t result_cursor = 0;
    size_t i;
    int rc = 0;
    int result_flattened = 0;

    parents = calloc(parent_count, sizeof(*parents));
    parent_cursor = calloc(parent_count, sizeof(*parent_cursor));
    parent_entry = calloc(parent_count, sizeof(*parent_entry));
    if (parents == NULL || parent_cursor == NULL || parent_entry == NULL) {
        free(parents);
        free(parent_cursor);
        free(parent_entry);
        return -1;
    }

    for (i = 0; i < parent_count; i++) {
        rc = sg_tree_flatten(git_dir, parent_trees[i], &parents[i], bad_path);
        if (rc != 0)
            goto done;
    }
    rc = sg_tree_flatten(git_dir, result_tree, &result_flat, bad_path);
    if (rc != 0)
        goto done;
    result_flattened = 1;

    for (;;) {
        const char *min_path = NULL;
        const sg_flat_entry *result_e;
        int differs_from_all = 1;
        char path[SG_PATH_MAX];

        for (i = 0; i < parent_count; i++) {
            size_t c = parent_cursor[i];

            if (c < parents[i].count &&
               (min_path == NULL || strcmp(parents[i].entries[c].path, min_path) < 0))
                min_path = parents[i].entries[c].path;
        }
        if (result_cursor < result_flat.count &&
           (min_path == NULL || strcmp(result_flat.entries[result_cursor].path, min_path) < 0))
            min_path = result_flat.entries[result_cursor].path;
        if (min_path == NULL)
            break;

        if (strlen(min_path) >= sizeof(path)) {
            rc = -1;
            goto done;
        }
        strcpy(path, min_path);

        result_e = (result_cursor < result_flat.count &&
                   strcmp(result_flat.entries[result_cursor].path, path) == 0)
                       ? &result_flat.entries[result_cursor]
                       : NULL;

        for (i = 0; i < parent_count; i++) {
            size_t c = parent_cursor[i];
            const sg_flat_entry *pe = (c < parents[i].count &&
                                       strcmp(parents[i].entries[c].path, path) == 0)
                                          ? &parents[i].entries[c]
                                          : NULL;
            int equal;

            parent_entry[i] = pe;
            if (pe == NULL && result_e == NULL)
                equal = 1;
            else if (pe == NULL || result_e == NULL)
                equal = 0;
            else
                equal = pe->mode == result_e->mode &&
                       memcmp(pe->sha1, result_e->sha1, SG_SHA1_RAW_LEN) == 0;
            if (equal) {
                differs_from_all = 0;
                break;
            }
        }

        if (differs_from_all) {
            if (name_status) {
                for (i = 0; i < parent_count; i++) {
                    char letter = parent_entry[i] == NULL
                                      ? 'A'
                                      : (result_e == NULL ? 'D' : 'M');

                    putchar(letter);
                }
                printf("\t%s\n", sg_quote_path(path));
            } else {
                printf("%s\n", sg_quote_path(path));
            }
        }

        for (i = 0; i < parent_count; i++) {
            size_t c = parent_cursor[i];

            if (c < parents[i].count && strcmp(parents[i].entries[c].path, min_path) == 0)
                parent_cursor[i]++;
        }
        if (result_cursor < result_flat.count &&
           strcmp(result_flat.entries[result_cursor].path, min_path) == 0)
            result_cursor++;
    }
    rc = 0;

done:
    for (i = 0; i < parent_count; i++)
        sg_flat_list_free(&parents[i]);
    free(parents);
    free(parent_cursor);
    free(parent_entry);
    if (result_flattened)
        sg_flat_list_free(&result_flat);
    return rc;
}

/* The diff half of `sg show` on a genuine 2-parent merge -- everything
   AFTER the entry header sg_commit_out_entry already printed (with patch
   and stat forced off; see the SG_OBJ_COMMIT case in render_id). Not routed
   through commit_out.c's print_commit_diff: that function's separator rules
   (`---` when both --stat and -p are on, no blank line at all under
   --oneline) are print_commit_diff's OWN measured rules for a first-parent
   diff, and a merge's combined diff measures differently on both counts --
   see the two WARNINGs below.

   Returns 0, or -1 having printed a partial diff (the caller reports it). */
static int render_merge_diff(const char *git_dir, const sg_commit *commit,
                             const sg_commit_out_opts *o)
{
    unsigned char (*parent_trees)[SG_SHA1_RAW_LEN];
    sg_diff_list combined_list;
    size_t combined_row_count = 0;
    size_t i;
    char bad_path[SG_PATH_MAX];
    sg_diff_out_opts opts;
    int rc;

    /* Takes any parent count, not just two. An octopus reaches here whenever
       it was not refused, and the builder then leaves the row list empty
       while still reporting the count -- which is exactly right, because
       --stat is a FIRST-PARENT diff at any parent count (measured:
       `git show --stat <octopus>` equals `git diff --stat <parent1>
       <octopus>`), and only the dense patch needs two parents to render. */
    parent_trees = malloc(commit->parent_count * sizeof(*parent_trees));
    if (parent_trees == NULL)
        return -1;
    for (i = 0; i < commit->parent_count; i++) {
        if (sg_commit_tree_of(git_dir, commit->parents[i], parent_trees[i]) != 0) {
            free(parent_trees);
            return -1;
        }
    }

    bad_path[0] = '\0';
    rc = sg_diff_combined_from_trees(git_dir, parent_trees, commit->parent_count,
                                     commit->tree, &combined_list,
                                     &combined_row_count, bad_path);
    if (rc == -2) {
        fprintf(stderr, "sg: tree contains an unsafe path '%s'\n", bad_path);
        free(parent_trees);
        return -1;
    }
    if (rc != 0) {
        free(parent_trees);
        return -1;
    }

    /* WARNING: a merge's diff section opens with this blank line WHENEVER a
       diff was asked for, even when the dense combined row set is EMPTY --
       measured on a clean 2-parent merge and on a clean octopus, where git
       prints the header, this blank line, and nothing else. An ordinary
       (non-merge) commit with an empty diff prints NO blank line at all
       (measured too), so the two rules are genuinely different and an
       early return on `combined_row_count == 0` gets the merge one wrong.
       It also skipped --stat, which has its own row set entirely. */

    /* WARNING: unlike commit_out.c's print_commit_diff, this blank line is
       NOT conditioned on `!o->oneline` -- measured, both directions: `git
       show --oneline <merge>` still prints a blank line before `diff --cc`,
       where an ordinary (non-merge, first-parent) `--oneline` diff prints
       none at all. Merge's separator rules are their own, not a special
       case of the ordinary ones. */
    printf("\n");

    memset(&opts, 0, sizeof opts);
    opts.algorithm = SG_DIFF_ALGO_MYERS;

    if (o->name_only || o->name_status) {
        /* section 4/4.1: attaches to the DENSE branch, not --stat's
           first-parent row set. resolve_commit_out_opts already guarantees
           o->stat and o->patch are both 0 whenever this fires. */
        if (commit->parent_count == 2) {
            opts.format = o->name_status ? SG_DIFF_FORMAT_NAME_STATUS : SG_DIFF_FORMAT_NAME_ONLY;
            /* Dense, same reasoning as the patch branch below: there is no
               non-dense entry point into `sg show` at all. */
            opts.combined = 1;
            rc = sg_diff_print(git_dir, NULL, &combined_list, &opts);
        } else {
            /* combined_list is empty here (sg_diff_combined_from_trees only
               fills `out` at parent_count == 2), so the dense set has to be
               recomputed from the parent trees directly -- see
               render_octopus_names's own comment for why this cannot reuse
               that function's `out`. */
            rc = render_octopus_names(git_dir, parent_trees, commit->parent_count, commit->tree,
                                      o->name_status, bad_path);
            if (rc == -2) {
                fprintf(stderr, "sg: tree contains an unsafe path '%s'\n", bad_path);
                rc = -1;
            }
        }
        if (rc != 0) {
            sg_diff_list_free(&combined_list);
            free(parent_trees);
            return -1;
        }
        sg_diff_list_free(&combined_list);
        free(parent_trees);
        return 0;
    }

    if (o->stat) {
        unsigned char stat_tree[SG_SHA1_RAW_LEN];
        sg_diff_list stat_list;

        /* WARNING: `git show --stat <merge>` is measured BYTE-IDENTICAL to
           `git diff --stat <parent1> <merge>` -- a completely different row
           set from the dense combined rule above (it includes a path like
           `theirs_only.txt`, touched only by parent 2, that the dense rule
           excludes because it still agrees with parent 1). Do not reuse
           combined_list here. */
        memcpy(stat_tree, parent_trees[0], SG_SHA1_RAW_LEN);
        if (sg_diff_trees(git_dir, stat_tree, commit->tree, &stat_list, bad_path, 0) != 0) {
            sg_diff_list_free(&combined_list);
            free(parent_trees);
            return -1;
        }
        /* git's own default since 2.9: diff.renames is on -- same call
           commit_out.c's print_commit_diff makes for an ordinary commit,
           and measured to hold here too (a rename across parent 1 and the
           merge result collapses to a single "a => b" stat row). */
        if (sg_diff_detect_renames(git_dir, NULL, &stat_list, SG_SIMILARITY_DEFAULT, 0) != 0) {
            sg_diff_list_free(&stat_list);
            sg_diff_list_free(&combined_list);
            free(parent_trees);
            return -1;
        }
        opts.format = SG_DIFF_FORMAT_STAT;
        rc = sg_diff_print(git_dir, NULL, &stat_list, &opts);
        sg_diff_list_free(&stat_list);
        if (rc != 0) {
            sg_diff_list_free(&combined_list);
            free(parent_trees);
            return -1;
        }
    }

    if (o->patch) {
        /* WARNING: measured -- with BOTH --stat and -p on a merge there is
           NO `---` separator line, just this one blank line, unlike an
           ordinary commit (print_commit_diff prints a literal "---\n"
           there instead). */
        /* ...and it belongs to a patch that exists: with an EMPTY combined
           row set this separator would be a trailing blank line git does
           not print (measured on a clean merge with -p --stat, whose stat
           is non-empty -- a first-parent diff -- while its dense patch is
           not). */
        if (o->stat && combined_row_count > 0)
            printf("\n");
        opts.format = SG_DIFF_FORMAT_PATCH;
        /* Dense (diff --cc), same as PATCH's ordinary default -- there is
           no non-dense (-c/--combined) entry point into `sg show` at all,
           so this is unconditional, not read off a CLI flag. */
        opts.combined = 1;
        rc = sg_diff_print(git_dir, NULL, &combined_list, &opts);
        if (rc != 0) {
            sg_diff_list_free(&combined_list);
            free(parent_trees);
            return -1;
        }
    }

    sg_diff_list_free(&combined_list);
    free(parent_trees);
    return 0;
}

static int render_id(const char *git_dir, const char *display_arg,
                     const unsigned char id[SG_SHA1_RAW_LEN], const show_flags *flags,
                     int nested, int hops, int *shown, seen_ids *seen);

static int render_tree(const char *display_arg, const unsigned char *content, size_t content_len)
{
    sg_tree tree;
    size_t i;

    if (sg_tree_parse(content, content_len, &tree) != 0) {
        fprintf(stderr, "sg: malformed tree object\n");
        return -1;
    }

    printf("tree %s\n\n", display_arg);
    for (i = 0; i < tree.count; i++) {
        const sg_tree_entry *e = &tree.entries[i];

        printf("%s%s\n", e->name, e->mode == 040000 ? "/" : "");
    }

    sg_tree_free(&tree);
    return 0;
}

static int render_blob(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN])
{
    unsigned char *content;
    size_t content_len;
    sg_chunk_missing_info missing;
    int rc;

    rc = sg_chunk_read_blob(git_dir, id, &content, &content_len, &missing);
    if (rc != 0) {
        if (rc == -2)
            sg_chunk_print_missing_error("<object>", &missing);
        else
            fprintf(stderr, "sg: cannot read blob\n");
        return -1;
    }

    fwrite(content, 1, content_len, stdout);
    free(content);
    return 0;
}

/* Prints one object (commit/tag/tree/blob), following any tag chain.

   *shown tracks "has anything been printed yet", for the leading blank line
   between top-level multi-object entries. *seen tracks which COMMIT ids
   have already been rendered anywhere in this invocation (see seen_ids's own
   comment for why only that one type dedupes).

   `nested` is 1 exactly while rendering the object a TAG points at: that
   render always gets a mandatory single leading blank line (the tag's own
   "one blank line, then the tagged object" rule), regardless of *shown --
   UNLESS the target is a COMMIT id already in *seen, in which case NOTHING
   is printed at all, not even that blank line (measured: `sg show lw v2`,
   where lw and v2 name the same commit, prints the tag header but then
   stops dead after the tag message -- no blank, no commit block). */
static int render_id(const char *git_dir, const char *display_arg,
                     const unsigned char id[SG_SHA1_RAW_LEN], const show_flags *flags,
                     int nested, int hops, int *shown, seen_ids *seen)
{
    unsigned char *content;
    size_t content_len;
    sg_obj_type type;
    int entry_like; /* commit or tag: participates in the leading separator */

    if (sg_object_read(git_dir, id, &type, &content, &content_len) != 0) {
        fprintf(stderr, "sg: object not found or corrupt\n");
        return -1;
    }

    /* Everything except a blob takes a leading blank line -- when something
       was already shown, or when it is a tag's target. Measured in both
       directions: `sg show <commit> <tree>` DOES separate them (the tree's
       own "never separates" behaviour was only ever true first, with
       nothing to separate from), a tag's commit, tree and nested tag
       targets all take one, and a tag pointing at a BLOB takes none.
       The print is deliberately NOT done here but inside each case: a merge
       commit is refused, and refusing after emitting a blank line leaves
       stdout dirty for a command that reported failure. */
    entry_like = (type != SG_OBJ_BLOB);
    if (type == SG_OBJ_COMMIT) {
        if (seen_contains(seen, id)) {
            free(content);
            return 0;
        }
        if (seen_add(seen, id) != 0) {
            free(content);
            fprintf(stderr, "sg: out of memory\n");
            return -1;
        }
    }

    switch (type) {
    case SG_OBJ_COMMIT: {
        sg_commit commit;
        sg_commit_out_opts o;
        int rc;

        if (sg_commit_parse(content, content_len, &commit) != 0) {
            fprintf(stderr, "sg: malformed commit object\n");
            free(content);
            return -1;
        }
        free(content);

        resolve_commit_out_opts(flags, &o);

        /* Octopus: the DENSE PATCH renderer is fixed at two parents, so
           only rendering that patch is refused -- and only when it would
           actually produce something (see commit_needs_refusal's own
           comment). Found by a 159-probe oracle (Phase 59 round 2): this
           used to fire on ANY output request at all (`o.patch` was not
           part of the condition), which wrongly refused `--stat` too --
           `--stat` never touches the dense set, it is a first-parent diff
           at any parent count (CLAUDE.md says exactly this, and used to
           get the octopus case wrong right next to saying it). The same
           bug also blocked plain `-s` (header only, no diff at all) on
           this shape, real git 2.55.0 measured to accept both. Pre-existing
           since Phase 55b -- unreachable by any fixture an ordinary `sg
           merge`/`git merge` can build (a real octopus merge tool refuses
           on the same conflict), so it needed a `commit-tree`-built
           fixture to ever become observable at all.
           `o.name_only`/`o.name_status` still bypass this entirely (Phase
           59 section 4.1): they carry one status letter per parent instead
           of being pinned at two, routing through render_octopus_names
           rather than the fixed-2-parent combined_list path, at any parent
           count -- checked here even though `o.patch` might ALSO be
           nonzero (e.g. `-p --name-only --stat`, where a stale `o.patch`
           survives resolve_commit_out_opts because that function no longer
           zeroes it, see its own comment), because name mode always wins
           regardless of what else was asked for. */
        if (commit.parent_count > 2 && o.patch && !(o.name_only || o.name_status)) {
            if (commit_needs_refusal(git_dir, &commit)) {
                fprintf(stderr,
                       "sg: showing an octopus merge with a non-empty combined diff is not "
                       "supported yet\n");
                sg_commit_free(&commit);
                return -1;
            }
        }

        /* WARNING: nested is ONLY ever 1 here when this commit is a tag's
           target (the sole caller that passes nested=1) -- Phase 55a's
           "everything but a blob takes a leading blank line" rule holds
           except for exactly this one case: real git's --oneline prints NO
           blank line between an annotated tag's message and the oneline
           commit header that follows it (measured; the same fixture without
           --oneline DOES get the blank line, so this is not a general
           --oneline rule, only a tag-then-oneline-commit one). Scoped as
           tightly as the condition can express: nested_target_suppresses_blank(flags)
           can only ever be true immediately below an annotated tag.

           Phase 60a: the builtin `reference` format joins legacy --oneline
           here too -- see nested_target_suppresses_blank's own comment and
           tag_header_shape's truth table just above resolve_commit_out_opts. */
        if (entry_like && (nested || *shown) && !(nested && nested_target_suppresses_blank(flags)))
            printf("\n");

        if (commit.parent_count > 1) {
            sg_commit_out_opts header_o = o;

            header_o.patch = 0;
            header_o.stat = 0;
            header_o.name_only = 0;
            header_o.name_status = 0;
            rc = sg_commit_out_entry(git_dir, id, &commit, &header_o);
            *shown = 1;
            if (rc == 0 && (o.patch || o.stat || o.name_only || o.name_status))
                rc = render_merge_diff(git_dir, &commit, &o);
        } else {
            rc = sg_commit_out_entry(git_dir, id, &commit, &o);
            *shown = 1;
        }
        sg_commit_free(&commit);
        if (rc != 0) {
            fprintf(stderr, "sg: cannot render this commit's diff\n");
            return -1;
        }
        return 0;
    }
    case SG_OBJ_TAG: {
        sg_tag tag;
        char timebuf[SG_DATE_NORMAL_MAX];
        int rc;

        if (sg_tag_parse(content, content_len, &tag) != 0) {
            fprintf(stderr, "sg: malformed tag object\n");
            free(content);
            return -1;
        }
        free(content);

        /* Mirrors resolve_commit_out_opts' own "wants_patch" derivation
           (Phase 59 round 2): a bare --stat or -s never asks for the dense
           patch, so a tag pointing at a non-empty-dense octopus must not be
           refused on their account either. */
        {
            int p59_wants_patch = flags->patch || !flags->format_seen;
            int p59_check_refusal =
                p59_wants_patch && !(flags->name_only || flags->name_status);

            if (target_is_merge(git_dir, tag.object, hops + 1, p59_check_refusal)) {
                fprintf(stderr,
                       "sg: showing an octopus merge with a non-empty combined diff is not "
                       "supported yet\n");
                sg_tag_free(&tag);
                return -1;
            }
        }

        if (sg_date_format_normal(tag.tagger_time, tag.tagger_tz, timebuf, sizeof(timebuf)) != 0)
            timebuf[0] = '\0';

        if (entry_like && (nested || *shown))
            printf("\n");

        /* Unlike a commit's message, a tag's message is NOT indented four
           spaces (measured).

           WARNING: **--oneline drops the Tagger:/Date: lines entirely**
           (measured against real git 2.55.0: `git show --oneline
           <annotated tag>` prints "tag v1", a blank line, then the message,
           with NO Tagger:/Date: lines in between) -- a pre-existing Phase
           55a bug, not introduced by Phase 59: the 50 flag combinations
           that phase claimed to have pinned byte-for-byte never actually
           combined --oneline with an annotated tag target. Same old/new
           behavior reproduced on a pre-Phase-59 build, confirming this.

           Phase 60a: every one of the seven --pretty builtins has its OWN
           row of this table, not just oneline -- see resolve_tag_header_shape's
           own comment for the measured truth table and the reasoning for
           keeping it data-driven rather than one `if` per format. */
        {
            tag_header_shape shape = resolve_tag_header_shape(flags);

            printf("tag %s\n", tag.tag_name);
            if (shape.show_tagger) {
                if (shape.fuller_pad)
                    printf("%-12s%s <%s>\n", "Tagger:", tag.tagger_name, tag.tagger_email);
                else
                    printf("Tagger: %s <%s>\n", tag.tagger_name, tag.tagger_email);
            }
            if (shape.show_date) {
                if (shape.fuller_pad)
                    printf("%-12s%s\n", "TaggerDate:", timebuf);
                else
                    printf("Date:   %s\n", timebuf);
            }
        }
        if (tag.message != NULL && tag.message[0] != '\0')
            printf("\n%s", tag.message);
        *shown = 1;

        if (hops >= SG_SHOW_MAX_TAG_HOPS) {
            fprintf(stderr, "sg: tag chain too deep\n");
            sg_tag_free(&tag);
            return -1;
        }

        rc = render_id(git_dir, display_arg, tag.object, flags, 1, hops + 1, shown, seen);
        sg_tag_free(&tag);
        return rc;
    }
    case SG_OBJ_TREE: {
        int rc;

        if (entry_like && (nested || *shown))
            printf("\n");
        rc = render_tree(display_arg, content, content_len);

        free(content);
        if (rc == 0)
            *shown = 1;
        return rc;
    }
    case SG_OBJ_BLOB:
        free(content);
        return render_blob(git_dir, id);
    default:
        free(content);
        fprintf(stderr, "sg: unknown object type\n");
        return -1;
    }
}

/* Resolves `arg` to an object id/type via the shared sg_rev_parse_object
   (see revparse.h for the exact grammar and order). Returns 0 with *id_out
   and *type_out filled in, -1 having already printed a diagnostic. */
static int resolve_object(const char *git_dir, const char *arg,
                          unsigned char id_out[SG_SHA1_RAW_LEN], sg_obj_type *type_out)
{
    char bad_path[SG_PATH_MAX];
    int rc;

    bad_path[0] = '\0';
    rc = sg_rev_parse_object(git_dir, arg, id_out, type_out, bad_path, sizeof(bad_path));
    if (rc == 0)
        return 0;
    /* A well-formed id whose object cannot be read is missing or corrupt,
       NOT an invalid name -- interop pins this wording, because naming the
       wrong problem sends the reader to the wrong place (the fixture is a
       packed REF_DELTA whose base object is gone). */
    if (rc == -3) {
        fprintf(stderr, "sg: object '%s' not found or corrupt\n", arg);
        return -1;
    }
    if (rc == -2) {
        const char *colon = strchr(arg, ':');
        char rev[SG_PATH_MAX];
        size_t rev_len = colon != NULL ? (size_t)(colon - arg) : 0;

        if (rev_len >= sizeof(rev))
            rev_len = sizeof(rev) - 1;
        memcpy(rev, arg, rev_len);
        rev[rev_len] = '\0';
        fprintf(stderr, "sg: path '%s' does not exist in '%s'\n", bad_path, rev);
        return -1;
    }
    fprintf(stderr, "sg: not a valid object name '%s'\n", arg);
    return -1;
}

int sg_cmd_show(int argc, char **argv)
{
    char *git_dir;
    show_flags flags;
    const char *objects[256];
    int object_count = 0;
    int shown = 0;
    seen_ids seen;
    int rc = 0;
    int i;

    seen.ids = NULL;
    seen.count = 0;
    seen.cap = 0;

    flags.oneline = 0;
    flags.patch = 0;
    flags.stat = 0;
    flags.name_only = 0;
    flags.name_status = 0;
    flags.no_output = 0;
    flags.format_seen = 0;
    flags.pretty_set = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--pretty") == 0) {
            /* Bare --pretty (no '=') is legal and means medium -- measured
               asymmetry with bare --format below, CLAUDE.md's `sg log`
               grammar section has the full table (shared with `sg show`). */
            if (resolve_pretty_arg("medium", &flags.pretty) != 0)
                return 1;
            flags.pretty_set = 1;
        } else if (strncmp(a, "--pretty=", 9) == 0) {
            if (resolve_pretty_arg(a + 9, &flags.pretty) != 0)
                return 1;
            flags.pretty_set = 1;
        } else if (strncmp(a, "--format=", 9) == 0) {
            if (resolve_pretty_arg(a + 9, &flags.pretty) != 0)
                return 1;
            flags.pretty_set = 1;
        } else if (strcmp(a, "-s") == 0) {
            /* -s CLEARS every one of the five bits, then sets no_output --
               see show_flags's own comment for the full model and why an
               explicit format request (-p/--stat, but NOT --name-only/
               --name-status) also clears no_output below. */
            flags.patch = 0;
            flags.stat = 0;
            flags.name_only = 0;
            flags.name_status = 0;
            flags.no_output = 1;
            flags.format_seen = 1;
        } else if (strcmp(a, "-p") == 0 || strcmp(a, "--patch") == 0) {
            flags.patch = 1;
            flags.no_output = 0;
            flags.format_seen = 1;
        } else if (strcmp(a, "--stat") == 0) {
            flags.stat = 1;
            flags.no_output = 0;
            flags.format_seen = 1;
        } else if (strcmp(a, "--name-only") == 0) {
            flags.name_only = 1;
            flags.format_seen = 1;
        } else if (strcmp(a, "--name-status") == 0) {
            flags.name_status = 1;
            flags.format_seen = 1;
        } else if (strcmp(a, "--oneline") == 0) {
            flags.oneline = 1;
        } else if (a[0] == '-') {
            fprintf(stderr, "%s", USAGE);
            return 1;
        } else {
            if ((size_t)object_count >= sizeof(objects) / sizeof(objects[0])) {
                fprintf(stderr, "%s", USAGE);
                return 1;
            }
            objects[object_count++] = a;
        }
    }

    /* Section 2: more than one of {--name-only, --name-status, -s} surviving
       to here is an error -- git: "fatal: options '--name-only',
       '--name-status', '--check', and '-s' cannot be used together" (exit
       128, not one of --check since sg has no --check). sg keeps this
       project's own exit-0/1 convention (see CLAUDE.md's divergence #3) and
       prints the usage line, same as any other rejected flag combination
       here. */
    if (flags.name_only + flags.name_status + flags.no_output > 1) {
        fprintf(stderr, "%s", USAGE);
        return 1;
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;

    if (object_count == 0)
        objects[object_count++] = "HEAD";

    for (i = 0; i < object_count; i++) {
        unsigned char id[SG_SHA1_RAW_LEN];
        sg_obj_type type;

        if (resolve_object(git_dir, objects[i], id, &type) != 0) {
            rc = 1;
            break;
        }
        if (render_id(git_dir, objects[i], id, &flags, 0, 0, &shown, &seen) != 0) {
            rc = 1;
            break;
        }
    }

    seen_ids_free(&seen);

    free(git_dir);
    return rc;
}
