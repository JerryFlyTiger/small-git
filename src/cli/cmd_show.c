#include "sg/cli.h"

#include "sg/chunk.h"
#include "sg/commit_out.h"
#include "sg/date.h"
#include "sg/hash.h"
#include "sg/objstore.h"
#include "sg/object.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/revparse.h"
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

/* Follows a tag chain to the object it ultimately names and reports whether
   that object is a merge commit. Exists only because merges are refused for
   now: the tag header is printed before its target is ever read, so without
   looking ahead, `sg show <tag-pointing-at-a-merge>` writes an entry to
   stdout and THEN exits non-zero. A command that reports failure should not
   have dirtied stdout on the way. Returns 1 for merge, 0 otherwise, and 0
   for anything it cannot resolve -- the ordinary path reports those errors
   with a better message than a look-ahead could. */
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
                answer = commit.parent_count > 1;
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

        if (commit.parent_count > 1) {
            fprintf(stderr, "sg: showing a merge commit is not supported yet\n");
            sg_commit_free(&commit);
            return -1;
        }

        if (entry_like && (nested || *shown))
            printf("\n");
        resolve_commit_out_opts(flags, &o);
        rc = sg_commit_out_entry(git_dir, id, &commit, &o);
        sg_commit_free(&commit);
        *shown = 1;
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
            fprintf(stderr, "sg: showing a merge commit is not supported yet\n");
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

/* Splits `arg` at the first ':' and resolves the left side as a commit, then
   walks its tree component by component to find the entry named by the
   right side (an empty right side means the commit's own tree). Returns 0
   with *id_out and *type_out filled in, or -1 having already printed the
   "path does not exist" message. */
static int resolve_rev_path(const char *git_dir, const char *arg, const char *colon,
                            unsigned char id_out[SG_SHA1_RAW_LEN], sg_obj_type *type_out)
{
    char rev[SG_PATH_MAX];
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    const char *path;
    size_t rev_len = (size_t)(colon - arg);

    if (rev_len >= sizeof(rev)) {
        fprintf(stderr, "sg: not a valid object name '%s'\n", arg);
        return -1;
    }
    memcpy(rev, arg, rev_len);
    rev[rev_len] = '\0';
    path = colon + 1;

    if (sg_rev_parse_commit(git_dir, rev, commit_id) != 0) {
        fprintf(stderr, "sg: not a valid object name '%s'\n", arg);
        return -1;
    }
    if (sg_commit_tree_of(git_dir, commit_id, tree_id) != 0) {
        fprintf(stderr, "sg: not a valid object name '%s'\n", arg);
        return -1;
    }

    if (path[0] == '\0') {
        memcpy(id_out, tree_id, SG_SHA1_RAW_LEN);
        *type_out = SG_OBJ_TREE;
        return 0;
    }

    {
        char pathbuf[SG_PATH_MAX];
        char *saveptr = NULL;
        char *comp;
        unsigned char cur_id[SG_SHA1_RAW_LEN];

        if (strlen(path) >= sizeof(pathbuf)) {
            fprintf(stderr, "sg: path '%s' does not exist in '%s'\n", path, rev);
            return -1;
        }
        strcpy(pathbuf, path);
        memcpy(cur_id, tree_id, SG_SHA1_RAW_LEN);

        for (comp = strtok_r(pathbuf, "/", &saveptr); comp != NULL;
            comp = strtok_r(NULL, "/", &saveptr)) {
            unsigned char *content;
            size_t content_len;
            sg_obj_type type;
            sg_tree tree;
            size_t i;
            int found = 0;

            if (sg_object_read(git_dir, cur_id, &type, &content, &content_len) != 0) {
                fprintf(stderr, "sg: path '%s' does not exist in '%s'\n", path, rev);
                return -1;
            }
            /* A SUCCESSFUL read of a non-tree still owns `content`: reached
               whenever a middle component names a file rather than a
               directory (`HEAD:f.txt/x`, an ordinary typo), so folding this
               into the condition above leaked the whole decompressed blob. */
            if (type != SG_OBJ_TREE) {
                free(content);
                fprintf(stderr, "sg: path '%s' does not exist in '%s'\n", path, rev);
                return -1;
            }
            if (sg_tree_parse(content, content_len, &tree) != 0) {
                free(content);
                fprintf(stderr, "sg: path '%s' does not exist in '%s'\n", path, rev);
                return -1;
            }
            free(content);

            for (i = 0; i < tree.count; i++) {
                if (strcmp(tree.entries[i].name, comp) == 0) {
                    memcpy(cur_id, tree.entries[i].sha1, SG_SHA1_RAW_LEN);
                    found = 1;
                    break;
                }
            }
            sg_tree_free(&tree);

            if (!found) {
                fprintf(stderr, "sg: path '%s' does not exist in '%s'\n", path, rev);
                return -1;
            }
        }

        {
            unsigned char *content;
            size_t content_len;

            if (sg_object_read(git_dir, cur_id, type_out, &content, &content_len) != 0) {
                fprintf(stderr, "sg: path '%s' does not exist in '%s'\n", path, rev);
                return -1;
            }
            free(content);
        }
        memcpy(id_out, cur_id, SG_SHA1_RAW_LEN);
        return 0;
    }
}

/* Resolves `arg` to an object id/type per the three-step order documented
   in cmd_show.c's header: <rev>:<path>, a full 40-hex object id, otherwise
   a revision name (with tags deliberately left unpeeled). Returns 0 with
   *id_out and *type_out filled in, -1 having already printed a diagnostic. */
static int resolve_object(const char *git_dir, const char *arg,
                          unsigned char id_out[SG_SHA1_RAW_LEN], sg_obj_type *type_out)
{
    const char *colon = strchr(arg, ':');
    char ref_path[SG_PATH_MAX];

    if (colon != NULL)
        return resolve_rev_path(git_dir, arg, colon, id_out, type_out);

    if (strlen(arg) == SG_SHA1_HEX_LEN && sg_hex_to_sha1(arg, id_out) == 0) {
        unsigned char *content;
        size_t content_len;

        if (sg_object_read(git_dir, id_out, type_out, &content, &content_len) != 0) {
            fprintf(stderr, "sg: not a valid object name '%s'\n", arg);
            return -1;
        }
        free(content);
        return 0;
    }

    /* A tag must not be peeled: resolve to a ref path and read the id it
       names directly (whatever object that turns out to be), only falling
       back to sg_rev_parse_commit (which does peel, and understands
       ~/^/@{N} suffixes) when the name isn't a ref at all. */
    if (sg_rev_parse_ref_path(git_dir, arg, ref_path, sizeof(ref_path)) == 0) {
        int rc;

        /* HEAD is a symref, not a raw-oid file -- same special case
           sg_rev_parse_commit's own caller makes (revparse.c), for the
           same reason: sg_ref_read_path would try to hex-decode the
           "ref: ..." line and fail. */
        if (strcmp(ref_path, "HEAD") == 0)
            rc = sg_ref_resolve_head(git_dir, id_out);
        else
            rc = sg_ref_read_path(git_dir, ref_path, id_out);

        if (rc == 0) {
            unsigned char *content;
            size_t content_len;

            if (sg_object_read(git_dir, id_out, type_out, &content, &content_len) != 0) {
                fprintf(stderr, "sg: not a valid object name '%s'\n", arg);
                return -1;
            }
            free(content);
            return 0;
        }
    }

    if (sg_rev_parse_commit(git_dir, arg, id_out) == 0) {
        *type_out = SG_OBJ_COMMIT;
        return 0;
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
