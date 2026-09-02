#include "sg/cli.h"

#include "sg/chunk.h"
#include "sg/commit_out.h"
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

static const char USAGE[] =
    "usage: sg show [-s] [-p|--patch] [--stat] [--oneline] [<object>...]\n";

typedef struct {
    int format_seen; /* -s, -p or --stat appeared at all */
    int oneline;
    int patch;
    int stat;
} show_flags;

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
    o->patch = f->patch;
    o->stat = f->stat;
    /* Default is a patch -- measured, and differs from `sg stash show`,
       whose default is --stat. --oneline does NOT count as a format
       selector here: `git show --oneline` still prints the patch. */
    if (!f->format_seen) {
        o->patch = 1;
        o->stat = 0;
    }
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
                           int hops)
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

        if (commit.parent_count > 2) {
            /* Octopus: the renderer below is fixed at two parents, so this
               is the one shape still refused outright -- but only when it
               would actually need to print something (see
               commit_needs_refusal's own comment). A clean octopus falls
               straight through to the ordinary header-only print below,
               with patch/stat forced off: there is no diff to attach, and
               attaching print_commit_diff's own first-parent diff instead
               would print something real git does not (measured). */
            if (commit_needs_refusal(git_dir, &commit)) {
                fprintf(stderr,
                       "sg: showing an octopus merge with a non-empty combined diff is not "
                       "supported yet\n");
                sg_commit_free(&commit);
                return -1;
            }
        }

        if (entry_like && (nested || *shown))
            printf("\n");

        if (commit.parent_count > 1) {
            sg_commit_out_opts header_o = o;

            header_o.patch = 0;
            header_o.stat = 0;
            rc = sg_commit_out_entry(git_dir, id, &commit, &header_o);
            *shown = 1;
            if (rc == 0 && (o.patch || o.stat))
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

        if (target_is_merge(git_dir, tag.object, hops + 1)) {
            fprintf(stderr,
                   "sg: showing an octopus merge with a non-empty combined diff is not "
                   "supported yet\n");
            sg_tag_free(&tag);
            return -1;
        }

        if (sg_date_format_normal(tag.tagger_time, tag.tagger_tz, timebuf, sizeof(timebuf)) != 0)
            timebuf[0] = '\0';

        if (entry_like && (nested || *shown))
            printf("\n");

        /* Unlike a commit's message, a tag's message is NOT indented four
           spaces (measured). */
        printf("tag %s\n", tag.tag_name);
        printf("Tagger: %s <%s>\n", tag.tagger_name, tag.tagger_email);
        printf("Date:   %s\n", timebuf);
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
    flags.format_seen = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "-s") == 0) {
            /* -s CLEARS what came before rather than outranking it, and -p
               and --stat each set their own bit and accumulate. Measured
               over 11 combinations, and one model explains all of them:
               `-s -p` prints a patch while `-p -s` prints nothing, `-s
               --stat` prints a stat while `--stat -s` prints nothing, and
               `--stat -p` prints BOTH. Giving -s a fixed priority passes
               every single-flag case and gets `-s -p` backwards -- the same
               last-one-wins shape CLAUDE.md records for -M/-C and -c/--cc. */
            flags.patch = 0;
            flags.stat = 0;
            flags.format_seen = 1;
        } else if (strcmp(a, "-p") == 0 || strcmp(a, "--patch") == 0) {
            flags.patch = 1;
            flags.format_seen = 1;
        } else if (strcmp(a, "--stat") == 0) {
            flags.stat = 1;
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
