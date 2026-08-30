#include "sg/cli.h"

#include "sg/chunk.h"
#include "sg/hash.h"
#include "sg/http.h"
#include "sg/merge.h"
#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/pack.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/revparse.h"
#include "sg/transport.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* mode of a tree entry pointing at a submodule's own commit in another repo
   -- there is nothing to read here, the id isn't an object in this repo */
#define SG_GITLINK_MODE 0160000
#define SG_PUSH_LOST_COMMIT_DISPLAY_MAX 10

/* ---- a small hash set of object ids, bucketed on the id's first byte (same
   technique as pack.c's idx_build_bucket_*) -- both the "have" and "to send"
   object sets can run into the thousands for a real repo, so a linear scan
   per lookup (as cmd_fetch.c's id_array_contains does for its much smaller
   have-negotiation list) would get quadratic. ---- */

typedef struct {
    unsigned char (*ids)[SG_SHA1_RAW_LEN];
    size_t *next; /* parallel to ids: index of the next id in the same bucket, or (size_t)-1 */
    size_t count, cap;
    size_t bucket_head[256];
} id_set;

#define ID_SET_NONE ((size_t)-1)

static void id_set_init(id_set *s)
{
    size_t i;

    s->ids = NULL;
    s->next = NULL;
    s->count = 0;
    s->cap = 0;
    for (i = 0; i < 256; i++)
        s->bucket_head[i] = ID_SET_NONE;
}

static void id_set_free(id_set *s)
{
    free(s->ids);
    free(s->next);
}

static int id_set_contains(const id_set *s, const unsigned char id[SG_SHA1_RAW_LEN])
{
    size_t idx = s->bucket_head[id[0]];

    while (idx != ID_SET_NONE) {
        if (memcmp(s->ids[idx], id, SG_SHA1_RAW_LEN) == 0)
            return 1;
        idx = s->next[idx];
    }
    return 0;
}

/* Returns 1 if newly added, 0 if already present, -1 on allocation failure. */
static int id_set_add(id_set *s, const unsigned char id[SG_SHA1_RAW_LEN])
{
    unsigned char b = id[0];

    if (id_set_contains(s, id))
        return 0;

    if (s->count == s->cap) {
        size_t new_cap = s->cap == 0 ? 64 : s->cap * 2;
        unsigned char(*grown_ids)[SG_SHA1_RAW_LEN] = realloc(s->ids, new_cap * sizeof(*grown_ids));
        size_t *grown_next;

        if (grown_ids == NULL)
            return -1;
        s->ids = grown_ids;
        grown_next = realloc(s->next, new_cap * sizeof(*grown_next));
        if (grown_next == NULL)
            return -1;
        s->next = grown_next;
        s->cap = new_cap;
    }

    memcpy(s->ids[s->count], id, SG_SHA1_RAW_LEN);
    s->next[s->count] = s->bucket_head[b];
    s->bucket_head[b] = s->count;
    s->count++;
    return 1;
}

/* ---- plain growable id array, for BFS queues (no membership tracking) ---- */

static int id_array_push(unsigned char (**arr)[SG_SHA1_RAW_LEN], size_t *len, size_t *cap,
                         const unsigned char id[SG_SHA1_RAW_LEN])
{
    if (*len == *cap) {
        size_t new_cap = (*cap == 0) ? 64 : *cap * 2;
        unsigned char(*grown)[SG_SHA1_RAW_LEN] = realloc(*arr, new_cap * sizeof(**arr));

        if (grown == NULL)
            return -1;
        *arr = grown;
        *cap = new_cap;
    }
    memcpy((*arr)[*len], id, SG_SHA1_RAW_LEN);
    (*len)++;
    return 0;
}

/* ---- reachable-object walk (commit -> parents/tree, tree -> entries,
   tag -> tagged object) -- shared by both the "what does the remote already
   have" and "what do we need to send" sides of the diff. ---- */

typedef struct {
    unsigned char (*ids)[SG_SHA1_RAW_LEN];
    size_t count;
    size_t cap;
} id_queue;

static int id_queue_push(id_queue *q, const unsigned char id[SG_SHA1_RAW_LEN])
{
    if (q->count == q->cap) {
        size_t new_cap = q->cap == 0 ? 64 : q->cap * 2;
        unsigned char(*grown)[SG_SHA1_RAW_LEN] = realloc(q->ids, new_cap * sizeof(*grown));

        if (grown == NULL)
            return -1;
        q->ids = grown;
        q->cap = new_cap;
    }
    memcpy(q->ids[q->count++], id, SG_SHA1_RAW_LEN);
    return 0;
}

/* Marks id visited and queues it, unless it was already seen.
   Returns 1 = newly queued, 0 = already visited, -1 = allocation failure. */
static int walk_enqueue(id_queue *q, id_set *seen, const unsigned char id[SG_SHA1_RAW_LEN])
{
    int added = id_set_add(seen, id);

    if (added <= 0)
        return added;
    return id_queue_push(q, id) == 0 ? 1 : -1;
}

/* Collects everything reachable from id into `out`. Deliberately an explicit
   worklist rather than recursion: the commit chain walked here can be as long
   as the entire history when first pushing an existing repo to a new remote,
   which would blow the call stack. Same reason cmd_fetch.c's have-walk and
   collect_commit_ancestors below are iterative. */
static int walk_add_object(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN], id_set *out)
{
    id_queue q = {NULL, 0, 0};
    size_t head = 0;
    int rc;

    rc = walk_enqueue(&q, out, id);
    if (rc <= 0) {
        free(q.ids);
        return rc; /* already visited, or allocation failure */
    }
    rc = 0;

    while (head < q.count && rc == 0) {
        unsigned char cur[SG_SHA1_RAW_LEN];
        sg_obj_type type;
        unsigned char *content;
        size_t content_len;

        memcpy(cur, q.ids[head++], SG_SHA1_RAW_LEN);

        if (sg_object_read(git_dir, cur, &type, &content, &content_len) != 0) {
            rc = -1;
            break;
        }

        switch (type) {
        case SG_OBJ_BLOB: {
            /* A chunked-storage pointer blob's declared chunk ids are, from
               git's own object-model perspective, invisible -- they're only
               named as plain hex text inside this blob's content, not a real
               tree/commit graph edge. A push that only sent this blob would
               silently leave the actual file data unreachable on the remote
               (fsck-clean, but useless), so walk the chunk ids too, exactly
               like sg_chunk_effective_id/sg_chunk_read_blob's own hash-verified
               notion of "is this really a pointer" (format alone is not
               enough -- see sg_chunk_pointer_parse's own doc comment). */
            unsigned char effective_id[SG_SHA1_RAW_LEN];
            int eff_rc = sg_chunk_effective_id(git_dir, cur, effective_id);

            /* -2 means cur is a genuine chunk pointer (chunk_resolve's
               discriminator recognized its first chunk id as real) whose
               data is missing or corrupt -- e.g. a chunk that was never
               kept alive, or was collected by a gc that raced with this
               push. Silently falling through to "not a pointer, push cur's
               739-ish raw bytes as-is" here would be exactly the bug this
               phase fixed for restore, just replayed on the push path: the
               remote would end up with a pointer blob that looks complete
               (fsck-clean) but can never be reassembled. Abort the whole
               push instead. */
            if (eff_rc == -2) {
                char hex[SG_SHA1_HEX_LEN + 1];

                sg_sha1_to_hex(cur, hex);
                fprintf(stderr,
                       "sg: push aborted: chunked object %s has missing or corrupt chunk data in the local object "
                       "store, cannot guarantee complete data is pushed\n",
                       hex);
                free(content);
                rc = -1;
                break;
            }

            if (eff_rc == 0 && memcmp(effective_id, cur, SG_SHA1_RAW_LEN) != 0) {
                sg_chunk_pointer ptr;

                if (sg_chunk_pointer_parse(content, content_len, &ptr)) {
                    size_t i;

                    for (i = 0; i < ptr.chunk_count && rc == 0; i++) {
                        if (walk_enqueue(&q, out, ptr.chunk_ids[i]) < 0)
                            rc = -1;
                    }
                    sg_chunk_pointer_free(&ptr);
                }
            }
            free(content);
            break;
        }

        case SG_OBJ_TREE: {
            sg_tree tree;
            size_t i;

            if (sg_tree_parse(content, content_len, &tree) != 0) {
                free(content);
                rc = -1;
                break;
            }
            free(content);
            for (i = 0; i < tree.count && rc == 0; i++) {
                if (tree.entries[i].mode == SG_GITLINK_MODE)
                    continue;
                if (walk_enqueue(&q, out, tree.entries[i].sha1) < 0)
                    rc = -1;
            }
            sg_tree_free(&tree);
            break;
        }

        case SG_OBJ_COMMIT: {
            sg_commit commit;
            size_t i;

            if (sg_commit_parse(content, content_len, &commit) != 0) {
                free(content);
                rc = -1;
                break;
            }
            free(content);
            if (walk_enqueue(&q, out, commit.tree) < 0)
                rc = -1;
            for (i = 0; i < commit.parent_count && rc == 0; i++) {
                if (walk_enqueue(&q, out, commit.parents[i]) < 0)
                    rc = -1;
            }
            sg_commit_free(&commit);
            break;
        }

        case SG_OBJ_TAG: {
            sg_tag tag;

            if (sg_tag_parse(content, content_len, &tag) != 0) {
                free(content);
                rc = -1;
                break;
            }
            free(content);
            if (walk_enqueue(&q, out, tag.object) < 0)
                rc = -1;
            sg_tag_free(&tag);
            break;
        }
        }
    }

    free(q.ids);
    return rc;
}

/* For every ref the remote advertised whose target object we also have
   locally, walk everything reachable from it into `have`. A remote ref
   pointing at a commit we don't know about is simply skipped -- we have no
   way to know what it contains. */
static int build_have_set(const char *git_dir, const sg_ref_adv *adv, id_set *have)
{
    size_t i;

    for (i = 0; i < adv->count; i++) {
        sg_obj_type type;
        unsigned char *content;
        size_t content_len;

        if (sg_object_read(git_dir, adv->refs[i].id, &type, &content, &content_len) != 0)
            continue;
        free(content);
        if (walk_add_object(git_dir, adv->refs[i].id, have) < 0)
            return -1;
    }
    return 0;
}

static int compute_diff_ids(const id_set *send, const id_set *have,
                            unsigned char (**out)[SG_SHA1_RAW_LEN], size_t *out_count)
{
    unsigned char(*result)[SG_SHA1_RAW_LEN];
    size_t count = 0;
    size_t i;

    result = malloc(send->count > 0 ? send->count * sizeof(*result) : 1);
    if (result == NULL)
        return -1;
    for (i = 0; i < send->count; i++) {
        if (!id_set_contains(have, send->ids[i]))
            memcpy(result[count++], send->ids[i], SG_SHA1_RAW_LEN);
    }
    *out = result;
    *out_count = count;
    return 0;
}

/* ---- non-fast-forward protection ---- */

typedef enum {
    SG_PUSH_NEW_BRANCH,     /* remote has no such ref yet */
    SG_PUSH_FAST_FORWARD,   /* remote's current commit is an ancestor of ours */
    SG_PUSH_NON_FF,         /* would discard commits the remote has and we'd lose track of */
    SG_PUSH_UNKNOWN_REMOTE, /* remote's current commit isn't one we have at all */
} sg_push_ff_status;

/* The only source of truth for the ff-safety decision: if we don't have the
   remote's current commit, we cannot know whether overwriting it discards
   anyone's work, so that case is rejected unconditionally -- not even
   --force can override it, since --force means "I know what I'm overwriting
   and accept it," which isn't true here. */
static sg_push_ff_status check_fast_forward(const char *git_dir,
                                            const unsigned char old_id[SG_SHA1_RAW_LEN],
                                            const unsigned char new_id[SG_SHA1_RAW_LEN],
                                            int old_exists)
{
    unsigned char base[SG_SHA1_RAW_LEN];
    sg_obj_type type;
    unsigned char *content;
    size_t content_len;

    if (!old_exists)
        return SG_PUSH_NEW_BRANCH;

    if (sg_object_read(git_dir, old_id, &type, &content, &content_len) != 0)
        return SG_PUSH_UNKNOWN_REMOTE;
    free(content);

    /* fast-forward iff old_id is an ancestor of new_id, i.e. their merge base
       is old_id itself; -1 (unrelated) and -2 (criss-cross) are both treated
       conservatively as non-fast-forward, same as any other non-ancestor base */
    if (sg_merge_base(git_dir, old_id, new_id, base) == 0 &&
       memcmp(base, old_id, SG_SHA1_RAW_LEN) == 0)
        return SG_PUSH_FAST_FORWARD;
    return SG_PUSH_NON_FF;
}

static int collect_commit_ancestors(const char *git_dir, const unsigned char start[SG_SHA1_RAW_LEN],
                                    id_set *out)
{
    unsigned char(*queue)[SG_SHA1_RAW_LEN] = NULL;
    size_t qlen = 0, qcap = 0, qhead = 0;
    int rc = -1;

    if (id_array_push(&queue, &qlen, &qcap, start) != 0)
        goto done;
    if (id_set_add(out, start) < 0)
        goto done;

    while (qhead < qlen) {
        unsigned char id[SG_SHA1_RAW_LEN];
        sg_obj_type type;
        unsigned char *content;
        size_t content_len;
        sg_commit commit;
        size_t i;

        memcpy(id, queue[qhead], SG_SHA1_RAW_LEN);
        qhead++;

        if (sg_object_read(git_dir, id, &type, &content, &content_len) != 0 || type != SG_OBJ_COMMIT)
            continue;
        if (sg_commit_parse(content, content_len, &commit) != 0) {
            free(content);
            continue;
        }
        free(content);

        for (i = 0; i < commit.parent_count; i++) {
            int added = id_set_add(out, commit.parents[i]);

            if (added < 0) {
                sg_commit_free(&commit);
                goto done;
            }
            if (added == 1 && id_array_push(&queue, &qlen, &qcap, commit.parents[i]) != 0) {
                sg_commit_free(&commit);
                goto done;
            }
        }
        sg_commit_free(&commit);
    }
    rc = 0;

done:
    free(queue);
    return rc;
}

/* Prints the commits reachable from old_id (the remote's current tip) that
   are NOT ancestors of new_id (ours) -- i.e. exactly what a non-force push
   would strand unreachable from any ref once the remote's ref is moved. */
static void print_lost_commits(const char *git_dir, const unsigned char old_id[SG_SHA1_RAW_LEN],
                               const unsigned char new_id[SG_SHA1_RAW_LEN])
{
    id_set local_ancestors;
    id_set visited;
    unsigned char(*queue)[SG_SHA1_RAW_LEN] = NULL;
    size_t qlen = 0, qcap = 0, qhead = 0;
    size_t shown = 0, total_lost = 0;

    id_set_init(&local_ancestors);
    id_set_init(&visited);

    if (collect_commit_ancestors(git_dir, new_id, &local_ancestors) != 0)
        goto done; /* best-effort listing; the reject decision is already made */
    if (id_array_push(&queue, &qlen, &qcap, old_id) != 0)
        goto done;
    if (id_set_add(&visited, old_id) < 0)
        goto done;

    while (qhead < qlen) {
        unsigned char id[SG_SHA1_RAW_LEN];
        sg_obj_type type;
        unsigned char *content;
        size_t content_len;
        sg_commit commit;
        int have_commit;
        size_t i;

        memcpy(id, queue[qhead], SG_SHA1_RAW_LEN);
        qhead++;

        have_commit = 0;
        if (sg_object_read(git_dir, id, &type, &content, &content_len) == 0) {
            if (type == SG_OBJ_COMMIT && sg_commit_parse(content, content_len, &commit) == 0)
                have_commit = 1;
            free(content); /* sg_commit_parse copies everything it needs, win or lose */
        }

        if (!id_set_contains(&local_ancestors, id)) {
            total_lost++;
            if (shown < SG_PUSH_LOST_COMMIT_DISPLAY_MAX) {
                char hex[SG_SHA1_HEX_LEN + 1];

                sg_sha1_to_hex(id, hex);
                if (have_commit) {
                    const char *nl = strchr(commit.message, '\n');
                    int msg_len = nl != NULL ? (int)(nl - commit.message) : (int)strlen(commit.message);

                    fprintf(stderr, "  %.7s %.*s\n", hex, msg_len, commit.message);
                } else {
                    fprintf(stderr, "  %.7s\n", hex);
                }
                shown++;
            }
        }

        if (!have_commit)
            continue;
        for (i = 0; i < commit.parent_count; i++) {
            int added = id_set_add(&visited, commit.parents[i]);

            if (added == 1 && id_array_push(&queue, &qlen, &qcap, commit.parents[i]) != 0) {
                sg_commit_free(&commit);
                goto done;
            }
        }
        sg_commit_free(&commit);
    }

    if (total_lost > shown)
        fprintf(stderr, "  ...and %zu more\n", total_lost - shown);

done:
    free(queue);
    id_set_free(&local_ancestors);
    id_set_free(&visited);
}

/* ---- what to push: one or more refs (a single branch, a single tag, or
   every tag under refs/tags/ with --tags) -- decided before any network
   round trip or pack is built. `entries` (the final send list, built after
   the remote's ref advertisement is known) each own their own malloc'd
   name/ref_path, kept alive until sg_transport_push returns since
   sg_push_ref_update.ref_name is borrowed (transport.h:88-89); they are
   freed only at the very end of sg_cmd_push. */
typedef struct {
    char *name;     /* malloc'd short name (branch or tag), owned; NULL until
                       known (a pending explicit-dst/delete candidate fills
                       this in only after dst completion, see complete_dst) */
    char *ref_path; /* malloc'd full ref path, owned; NULL until known, same
                       caveat as name */
    int is_tag;
    int is_new;    /* remote had no such ref before this push */
    int forced;    /* overwrote a differing remote ref via --force (tag: any
                      differing id; branch/other: a non-fast-forward allowed
                      through --force or a leading '+' on this refspec) */
    int is_delete; /* Phase 39: this candidate/entry is a ":dst" or
                      "--delete <name>" deletion -- new_id is always zero,
                      never fast-forward-checked, never walked, never part
                      of the pack (see the module note in CLAUDE.md) */
    int explicit_dst; /* Phase 39: true for a "<src>:<dst>" candidate still
                      awaiting dst completion (name/ref_path/is_tag are NULL/
                      unset until complete_dst runs, after the advertisement
                      is known) -- never true on a fully-built push_entry */
    int refspec_force; /* Phase 39: this specific refspec had a leading '+'
                      (as opposed to a blanket --force/-f); OR'd with the
                      global force flag at fast-forward-check time */
    char *dst_raw;    /* Phase 39: malloc'd raw dst text (post-colon, or the
                      bare name given to --delete), owned; only meaningful
                      while explicit_dst || is_delete, NULL afterwards */
    char *raw_arg;    /* Phase 39: malloc'd, owned; the whole original
                      command-line argument this candidate came from
                      ("<src>:<dst>", ":<dst>", or a bare --delete name),
                      used only for complete_dst's invalid-refspec message;
                      NULL when explicit_dst == is_delete == 0 (nothing in
                      that path is unvalidated free-form user input) */
    char *src_exact_ref_path; /* Phase 39: malloc'd, owned; the src's own
                      exact ref path from resolve_refspec_src (NULL if src
                      wasn't a literal ref, or this candidate has no src at
                      all), consumed by complete_dst's rule 2, unused
                      afterwards */
    unsigned char old_id[SG_SHA1_RAW_LEN];
    unsigned char new_id[SG_SHA1_RAW_LEN]; /* meaningless when is_delete */
} push_entry;

static void push_entry_free_all(push_entry *entries, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++) {
        free(entries[i].name);
        free(entries[i].ref_path);
        free(entries[i].dst_raw);
        free(entries[i].src_exact_ref_path);
        free(entries[i].raw_arg);
    }
    free(entries);
}

static int push_entries_push(push_entry **entries, size_t *count, size_t *cap, const push_entry *e)
{
    if (*count == *cap) {
        size_t new_cap = *cap == 0 ? 4 : *cap * 2;
        push_entry *grown = realloc(*entries, new_cap * sizeof(*grown));

        if (grown == NULL)
            return -1;
        *entries = grown;
        *cap = new_cap;
    }
    (*entries)[(*count)++] = *e;
    return 0;
}

/* Builds a single-branch candidate (name + "refs/heads/<name>" + its current
   commit id) and appends it to *candidates. Shared by the "explicit branch
   name given" and "no name given, use the current branch" cases, which used
   to duplicate this. Returns 0 on success, -1 on failure (message already
   printed to stderr, matching the pre-Phase-13 wording exactly). */
static int build_branch_candidate(const char *git_dir, const char *branch, push_entry **candidates,
                                  size_t *count, size_t *cap)
{
    unsigned char id[SG_SHA1_RAW_LEN];
    push_entry cand;
    char path[SG_PATH_MAX];

    if (sg_ref_read_branch(git_dir, branch, id) != 0) {
        fprintf(stderr, "sg: branch '%s' has no commits yet, nothing to push\n", branch);
        return -1;
    }
    snprintf(path, sizeof(path), "refs/heads/%s", branch);
    memset(&cand, 0, sizeof(cand));
    cand.name = strdup(branch);
    cand.ref_path = strdup(path);
    cand.is_tag = 0;
    memcpy(cand.new_id, id, SG_SHA1_RAW_LEN);
    if (cand.name == NULL || cand.ref_path == NULL ||
       push_entries_push(candidates, count, cap, &cand) != 0) {
        free(cand.name);
        free(cand.ref_path);
        fprintf(stderr, "sg: out of memory\n");
        return -1;
    }
    return 0;
}

static void free_string_array(char **arr, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++)
        free(arr[i]);
    free(arr);
}

/* ---- Phase 39: refspec support ----

   Stage A (pure syntax, no I/O): sg_push_refspec_parse below.
   Stage B ("<src>" resolution, done BEFORE any network round trip -- a
   src that resolves to nothing aborts the ENTIRE push, not just that one
   ref, see CLAUDE.md's push module note) is resolve_refspec_src.
   Stage C ("<dst>" completion, done AFTER the remote advertisement is
   known) is complete_dst. See docs/DESIGN.md Phase 39 for the three
   measured tables (syntax, dst completion, dst format validation) these
   three functions each implement. */

/* [+]<src>[:<dst>] split into {force, src, dst, is_delete}, no I/O at all.
   Non-static (but intentionally not declared in any public header, same
   convention as sg_parse_push_report_status in src/net/transport.c) so
   tests/test_refspec.c can call it directly via its own `extern`
   declaration. */
typedef struct {
    int force;     /* leading '+' */
    char *src;     /* malloc'd, owned; "" (empty, never NULL) for a ":dst"
                      delete form */
    char *dst;     /* malloc'd, owned; NULL if the argument had no ':' at
                      all (dst is derived some other way by the caller) */
    int is_delete; /* src is empty, i.e. ":dst" or "+:dst" */
    /* Phase 46. Wildcard: src and dst are patterns carrying exactly one '*'
       each, expanded against the LOCAL refs before any network round trip.
       Matching: the bare "[+]:" form, which can only be expanded AFTER the
       advertisement arrives because it pushes exactly the branches that
       already exist on the remote. The two flags are mutually exclusive and
       both are 0 for every form Phase 39 already handled. */
    int is_wildcard;
    int is_matching;
} sg_push_refspec;

/* Number of '*' characters in s. git allows exactly one per side of a
   wildcard refspec: measured, a side carrying two stars is
   `fatal: invalid refspec`, and so is a star on only one side. */
static size_t star_count(const char *s)
{
    size_t n = 0;

    for (; *s != '\0'; s++) {
        if (*s == '*')
            n++;
    }
    return n;
}

void sg_push_refspec_free(sg_push_refspec *r)
{
    if (r == NULL)
        return;
    free(r->src);
    free(r->dst);
}

/* Returns 0 on success (fields of *out filled in, caller must still free
   them via sg_push_refspec_free), -1 on a syntax error -- a message is
   already printed to stderr in every -1 case, matching real git's wording
   where it was measured (docs/DESIGN.md Phase 39 section 0). *out is
   zeroed but not otherwise meaningfully filled on -1.

   Splits on the LAST ':' (strrchr, not strchr) -- measured against real
   git 2.55.0: "a:b:c:d" reports src "a:b:c", dst "d". Also rejects, with
   their own named messages and never approximated, the two forms this
   milestone deliberately does not implement (docs/DESIGN.md Phase 39
   section 6): a wildcard anywhere in the argument, and the bare
   push-matching "[+]:" form. */
int sg_push_refspec_parse(const char *raw_arg, sg_push_refspec *out)
{
    const char *p = raw_arg;
    const char *colon;

    memset(out, 0, sizeof(*out));

    if (*p == '+')
        p++;

    out->force = (raw_arg[0] == '+');

    /* The bare "[+]:" push-matching form (Phase 46). It carries no src and
       no dst at all: which refs it means is a question only the remote's
       advertisement can answer, so the expansion happens later. */
    if (strcmp(p, ":") == 0) {
        out->is_matching = 1;
        return 0;
    }

    colon = strrchr(p, ':');
    if (colon == NULL) {
        out->src = strdup(p);
        out->dst = NULL;
        out->is_delete = 0;
        if (out->src == NULL)
            return -1;
        /* A wildcard with no ':' mirrors itself -- measured: a bare
           pattern argument pushes every matching ref to the SAME name, i.e.
           it behaves as though the same pattern had been written on both
           sides of a colon. That is a different dst rule from the no-colon
           NON-wildcard form just below, which goes through this command's
           literal-name lookup instead. */
        if (star_count(out->src) > 0) {
            if (star_count(out->src) != 1) {
                fprintf(stderr, "fatal: invalid refspec '%s'\n", raw_arg);
                sg_push_refspec_free(out);
                memset(out, 0, sizeof(*out));
                return -1;
            }
            out->dst = strdup(out->src);
            if (out->dst == NULL) {
                sg_push_refspec_free(out);
                memset(out, 0, sizeof(*out));
                return -1;
            }
            out->is_wildcard = 1;
        }
        return 0;
    }

    if (colon[1] == '\0') {
        fprintf(stderr, "fatal: invalid refspec '%s'\n", raw_arg);
        return -1;
    }

    out->src = strndup(p, (size_t)(colon - p));
    out->dst = strdup(colon + 1);
    out->is_delete = (colon == p);
    if (out->src == NULL || out->dst == NULL) {
        /* Round 3 fix: whichever of src/dst DID allocate must not leak --
           the header comment promises "*out is zeroed but not otherwise
           meaningfully filled on -1", so honor that here too. */
        sg_push_refspec_free(out);
        memset(out, 0, sizeof(*out));
        return -1;
    }

    /* Exactly one '*' on EACH side, or none at all -- measured against git
       2.55.0: two stars on a side, a star on only one side, and a star in a
       ":dst" deletion are each `fatal: invalid refspec`. Checked here rather
       than at expansion time so the whole batch aborts before any network
       round trip, the same shape as the empty-dst check above. */
    {
        size_t src_stars = star_count(out->src);
        size_t dst_stars = star_count(out->dst);

        if (src_stars != 0 || dst_stars != 0) {
            if (src_stars != 1 || dst_stars != 1 || out->is_delete) {
                fprintf(stderr, "fatal: invalid refspec '%s'\n", raw_arg);
                sg_push_refspec_free(out);
                memset(out, 0, sizeof(*out));
                return -1;
            }
            out->is_wildcard = 1;
        }
    }

    /* Stage C's format validation (docs/DESIGN.md Phase 39 section 3) is
       mostly deferred until the advertisement is known (a dst that doesn't
       start with "refs/" isn't a full ref path yet, see complete_dst's own
       rule 1/2 guessing). But every measured invalid-format example in
       section 3 is already "refs/"-prefixed as given, which makes it a pure
       string check with no dependency on the remote at all -- so it is
       done right here, BEFORE any network round trip, same batch-abort
       shape as the empty-dst check just above. complete_dst still
       revalidates its own guessed/rule-2-constructed path too, as
       defense in depth against a hostile/corrupt advertisement. */
    if (!out->is_wildcard && strncmp(out->dst, "refs/", 5) == 0 &&
       !sg_ref_name_valid_for_create(out->dst)) {
        fprintf(stderr, "fatal: invalid refspec '%s'\n", raw_arg);
        /* Round 3 fix: both src and dst are already allocated at this
           point -- same leak as above. */
        sg_push_refspec_free(out);
        memset(out, 0, sizeof(*out));
        return -1;
    }
    return 0;
}

/* The display name a report line and the remote-tracking ref use, derived
   from a full ref path the same way the explicit-dst path derives it (the
   WHOLE remainder after the namespace prefix, not just the last segment --
   see complete_dst's caller for why truncating there was a bug). */
static char *ref_display_name(const char *ref_path)
{
    if (strncmp(ref_path, "refs/heads/", 11) == 0)
        return strdup(ref_path + 11);
    if (strncmp(ref_path, "refs/tags/", 10) == 0)
        return strdup(ref_path + 10);
    /* Neither well-known namespace: keep the WHOLE path rather than the last
       segment. A wildcard can aim anywhere -- a pattern into refs/remotes/
       matched a nested branch and the last-segment rule would have reported
       "sub" for refs/remotes/up/topic/sub, the same truncation Phase 39's
       round 3 fixed for explicit multi-segment destinations. */
    return strdup(ref_path);
}

/* Expands a wildcard refspec against the LOCAL refs (Phase 46).

   The source set is local, not remote -- measured: pushing a pattern creates
   remote branches that did not exist there, so this is not an intersection
   with the advertisement and there is no prune semantics. That is why the
   expansion runs BEFORE the network round trip, keeping Phase 39's rule that
   a src resolving to nothing aborts the whole batch before anything lands.

   Matching is a plain prefix/suffix comparison around the single star, which
   IS git's rule: the star may sit anywhere, including mid-segment, and it
   CROSSES '/' (measured -- a pattern rooted at refs/ matched
   refs/remotes/origin/topic/sub). Whatever the star stood for is substituted
   into the dst pattern's star.

   The expanded dst is used VERBATIM, with no dwim completion -- also
   measured, and the opposite of what an explicit dst does: git sent the
   uncompleted name to the remote, which refused it as a "funny refname".
   sg refuses it locally instead, with its own message; both exit 1. */
static int wildcard_expand_candidates(const char *git_dir, const sg_push_refspec *rs,
                                      const char *raw_arg, push_entry **candidates, size_t *count,
                                      size_t *cap)
{
    const char *src_star = strchr(rs->src, '*');
    const char *dst_star = strchr(rs->dst, '*');
    size_t src_pre_len = (size_t)(src_star - rs->src);
    const char *src_suf = src_star + 1;
    size_t src_suf_len = strlen(src_suf);
    size_t dst_pre_len = (size_t)(dst_star - rs->dst);
    const char *dst_suf = dst_star + 1;
    char listdir[SG_PATH_MAX];
    char **names = NULL;
    size_t name_count = 0;
    size_t i;
    size_t cut = 0;
    int rc = -1;

    /* The namespace to enumerate: everything up to and including the last
       '/' that precedes the star. With no such '/', the pattern is matched
       against whole ref paths and "refs/" is the widest thing sg can list --
       which is also why a pattern that is not rooted at refs/ simply matches
       nothing, exactly as git behaves (measured: a bare "m*:m*" is
       "Everything up-to-date", not an error). */
    for (i = 0; i < src_pre_len; i++) {
        if (rs->src[i] == '/')
            cut = i + 1;
    }
    if (cut == 0) {
        snprintf(listdir, sizeof(listdir), "refs/");
    } else if (snprintf(listdir, sizeof(listdir), "%.*s", (int)cut, rs->src) >= (int)sizeof(listdir)) {
        fprintf(stderr, "sg: refspec '%s' is too long\n", raw_arg);
        return -1;
    }

    if (sg_ref_list_under(git_dir, listdir, &names, &name_count) != 0) {
        fprintf(stderr, "sg: cannot list local refs under '%s'\n", listdir);
        return -1;
    }

    for (i = 0; i < name_count; i++) {
        char full[SG_PATH_MAX];
        char dst[SG_PATH_MAX];
        size_t full_len;
        push_entry cand;

        /* A name too long to reassemble is REPORTED, not skipped: this
           project's path-truncation rule is that a reporting path must
           never silently drop a ref, and the two other over-long checks in
           this function already abort. Silently skipping would push some of
           the pattern's matches and not others, with nothing said. */
        if (snprintf(full, sizeof(full), "%s%s", listdir, names[i]) >= (int)sizeof(full)) {
            fprintf(stderr, "sg: ref name under '%s' is too long to expand\n", listdir);
            goto out;
        }
        full_len = strlen(full);
        if (full_len < src_pre_len + src_suf_len)
            continue;
        if (strncmp(full, rs->src, src_pre_len) != 0)
            continue;
        if (strcmp(full + full_len - src_suf_len, src_suf) != 0)
            continue;

        if (snprintf(dst, sizeof(dst), "%.*s%.*s%s", (int)dst_pre_len, rs->dst,
                    (int)(full_len - src_pre_len - src_suf_len), full + src_pre_len,
                    dst_suf) >= (int)sizeof(dst)) {
            fprintf(stderr, "sg: expanded refspec for '%s' is too long\n", full);
            goto out;
        }
        /* refs/sg/chunks belongs to the keepalive propagation block further
           down, which computes its own old/new pair for every push and may
           MERGE the two sides into a fresh commit. A pattern wide enough to
           match it (a mirror of the whole refs namespace is the obvious
           one) would queue a second,
           independently computed update for the same ref name in the same
           request -- and the remote refuses that outright: measured,
           "error: multiple updates for ref 'refs/sg/chunks' not allowed",
           and because the push is atomic NOTHING lands, not even the branch
           the user actually meant. Skipping it here leaves that ref with
           exactly one owner. */
        if (strcmp(full, SG_CHUNK_KEEPALIVE_REF) == 0)
            continue;
        if (strncmp(dst, "refs/", 5) != 0 || !sg_ref_name_valid_for_create(dst)) {
            fprintf(stderr, "sg: refspec '%s' expands to invalid destination '%s'\n", raw_arg, dst);
            goto out;
        }

        memset(&cand, 0, sizeof(cand));
        /* Read the ref EXACTLY, not through sg_rev_parse_commit: an
           annotated tag matched by a pattern must reach the remote as a tag
           object, the same trap resolve_refspec_src documents for an
           explicit src (measured: git keeps it a tag). */
        if (sg_ref_read_path(git_dir, full, cand.new_id) != 0) {
            fprintf(stderr, "sg: cannot read ref '%s'\n", full);
            goto out;
        }
        cand.name = ref_display_name(dst);
        cand.ref_path = strdup(dst);
        cand.is_tag = strncmp(dst, "refs/tags/", 10) == 0;
        cand.refspec_force = rs->force;
        if (cand.name == NULL || cand.ref_path == NULL ||
           push_entries_push(candidates, count, cap, &cand) != 0) {
            free(cand.name);
            free(cand.ref_path);
            fprintf(stderr, "sg: out of memory\n");
            goto out;
        }
    }
    rc = 0;

out:
    free_string_array(names, name_count);
    return rc;
}

/* Expands the bare "[+]:" push-matching form (Phase 46). Unlike a wildcard
   this CANNOT run before the network round trip: it means "every local
   branch that already exists on the remote", so the advertisement is half
   of its input. It never creates a remote ref -- measured: a local-only
   branch is not pushed -- and it never touches tags, even one that moved
   locally. */
static int matching_expand_candidates(const char *git_dir, const sg_ref_adv *adv, int force,
                                      push_entry **candidates, size_t *count, size_t *cap)
{
    char **names = NULL;
    size_t name_count = 0;
    size_t i, j;
    int rc = -1;

    if (sg_ref_list_branches(git_dir, &names, &name_count) != 0) {
        fprintf(stderr, "sg: cannot list local branches\n");
        return -1;
    }
    for (i = 0; i < name_count; i++) {
        char path[SG_PATH_MAX];
        push_entry cand;
        int on_remote = 0;

        if (snprintf(path, sizeof(path), "refs/heads/%s", names[i]) >= (int)sizeof(path)) {
            fprintf(stderr, "sg: branch name '%s' is too long\n", names[i]);
            goto out;
        }
        for (j = 0; j < adv->count && !on_remote; j++) {
            if (strcmp(adv->refs[j].name, path) == 0)
                on_remote = 1;
        }
        if (!on_remote)
            continue;

        memset(&cand, 0, sizeof(cand));
        if (sg_ref_read_path(git_dir, path, cand.new_id) != 0) {
            fprintf(stderr, "sg: cannot read branch '%s'\n", names[i]);
            goto out;
        }
        cand.name = strdup(names[i]);
        cand.ref_path = strdup(path);
        cand.refspec_force = force;
        if (cand.name == NULL || cand.ref_path == NULL ||
           push_entries_push(candidates, count, cap, &cand) != 0) {
            free(cand.name);
            free(cand.ref_path);
            fprintf(stderr, "sg: out of memory\n");
            goto out;
        }
    }
    rc = 0;

out:
    free_string_array(names, name_count);
    return rc;
}

/* Resolves an explicit-dst refspec's <src> (Stage B, before any network
   round trip -- CLAUDE.md's push module note explains why this specific
   function must not be sg_rev_parse_commit): a literal ref name --
   "refs/tags/<src>", "refs/heads/<src>", or src itself if it already
   starts with "refs/" -- is looked up by EXACT ref read, id NOT peeled, so
   an annotated tag given as src stays a tag object all the way to the
   remote. Anything else (HEAD, "~"/"^"/"@{N}" suffixes, a full hex id)
   falls back to the full sg_rev_parse_commit grammar, which does peel.

   Returns 0 with id_out and *exact_ref_path_out (malloc'd, or NULL if src
   was resolved via the sg_rev_parse_commit fallback rather than a literal
   ref) filled in. Returns -1 if src matches nothing at all (nothing
   printed; the caller reports "src refspec ... does not match any" once it
   knows whether to keep resolving the rest of the batch). Returns -2 if
   src matches both a local tag and a local branch of that name (message
   already printed: "src refspec '%s' matches more than one", same wording
   as the pre-Phase-39 no-colon path). Returns -3 on allocation failure. */
static int resolve_refspec_src(const char *git_dir, const char *src,
                               unsigned char id_out[SG_SHA1_RAW_LEN], char **exact_ref_path_out)
{
    char tag_path[SG_PATH_MAX];
    unsigned char tag_id[SG_SHA1_RAW_LEN];
    int tag_exists;
    int br_exists;

    *exact_ref_path_out = NULL;

    if (strncmp(src, "refs/", 5) == 0 && sg_ref_read_path(git_dir, src, id_out) == 0) {
        *exact_ref_path_out = strdup(src);
        return *exact_ref_path_out != NULL ? 0 : -3;
    }

    snprintf(tag_path, sizeof(tag_path), "refs/tags/%s", src);
    tag_exists = (sg_ref_read_path(git_dir, tag_path, tag_id) == 0);
    br_exists = sg_ref_branch_exists(git_dir, src);

    if (tag_exists && br_exists) {
        fprintf(stderr, "sg: src refspec '%s' matches more than one\n", src);
        return -2;
    }
    if (tag_exists) {
        memcpy(id_out, tag_id, SG_SHA1_RAW_LEN);
        *exact_ref_path_out = strdup(tag_path);
        return *exact_ref_path_out != NULL ? 0 : -3;
    }
    if (br_exists) {
        char br_path[SG_PATH_MAX];
        unsigned char br_id[SG_SHA1_RAW_LEN];

        if (sg_ref_read_branch(git_dir, src, br_id) != 0)
            return -1;
        snprintf(br_path, sizeof(br_path), "refs/heads/%s", src);
        memcpy(id_out, br_id, SG_SHA1_RAW_LEN);
        *exact_ref_path_out = strdup(br_path);
        return *exact_ref_path_out != NULL ? 0 : -3;
    }

    if (sg_rev_parse_commit(git_dir, src, id_out) != 0)
        return -1;
    return 0;
}

/* Stage C: completes a "<dst>" (already known not to be empty) into a full
   ref path, once the remote's advertisement is known -- see
   docs/DESIGN.md Phase 39 section 2 for the exact three-rule table this
   implements and the measurements pinning rule 1 ahead of rule 2.
   src_exact_ref_path is the src's own exact ref path from
   resolve_refspec_src (NULL if none), used only by rule 2; pass NULL here
   unconditionally for a deletion (deletion has no src, so rule 2 can never
   apply to it, only rule 1).

   On success, returns 0 and fills *ref_path_out (malloc'd). Also fills
   *old_id_out / *remote_exists_out by scanning adv for the completed path
   (a candidate for a NEW ref simply has *remote_exists_out == 0).
   Returns -1 if dst cannot be resolved into any full ref name at all
   (rules 1 and 2 both fail) -- the message differs by caller (a plain
   push names the generic "not a full refname" error, a delete names its
   own "remote ref does not exist" message), so nothing is printed here.
   Returns -2 if the completed path fails sg_ref_name_valid_for_create
   (message already printed: "fatal: invalid refspec '%s'\n" against
   raw_arg, the whole original command-line argument, not just dst).
   Returns -3 on allocation failure. Returns -4 if rule 1 alone matches MORE
   THAN ONE advertised ref across the different guessed prefixes (message
   already printed: "sg: dst refspec '%s' matches more than one\n", wording
   borrowed from real git's own "error: dst refspec %s matches more than
   one" -- measured against git 2.55.0: with the remote holding both
   refs/heads/dup and refs/tags/dup, `topic:dup` is REJECTED outright, git
   does not silently pick one guessed prefix over another in this case). */
static int complete_dst(const sg_ref_adv *adv, const char *dst_raw, const char *src_exact_ref_path,
                        const char *raw_arg, char **ref_path_out,
                        unsigned char old_id_out[SG_SHA1_RAW_LEN], int *remote_exists_out)
{
    static const char *const guess_prefixes[] = {"refs/", "refs/tags/", "refs/heads/", "refs/remotes/"};
    size_t i;

    *ref_path_out = NULL;

    if (strncmp(dst_raw, "refs/", 5) == 0) {
        *ref_path_out = strdup(dst_raw);
    } else {
        size_t k;
        size_t match_count = 0;
        char matched_path[SG_PATH_MAX];

        matched_path[0] = '\0';
        /* Round 3 fix: this must scan EVERY guessed prefix, not stop at the
           first match -- an early "*ref_path_out == NULL" loop condition
           (the pre-round-3 shape) silently picks whichever guess prefix
           comes first in guess_prefixes[] when more than one guess matches
           an advertised ref, which is exactly the "matches more than one"
           case real git refuses instead of guessing. */
        for (k = 0; k < sizeof(guess_prefixes) / sizeof(guess_prefixes[0]); k++) {
            char candidate[SG_PATH_MAX];
            size_t j;

            snprintf(candidate, sizeof(candidate), "%s%s", guess_prefixes[k], dst_raw);
            for (j = 0; j < adv->count; j++) {
                if (strcmp(adv->refs[j].name, candidate) == 0) {
                    match_count++;
                    if (match_count == 1)
                        snprintf(matched_path, sizeof(matched_path), "%s", candidate);
                    break;
                }
            }
        }
        if (match_count > 1) {
            fprintf(stderr, "sg: dst refspec '%s' matches more than one\n", dst_raw);
            return -4;
        }
        if (match_count == 1) {
            *ref_path_out = strdup(matched_path);
            if (*ref_path_out == NULL)
                return -3;
        }
        if (*ref_path_out == NULL && src_exact_ref_path != NULL) {
            const char *prefix = NULL;

            if (strncmp(src_exact_ref_path, "refs/heads/", 11) == 0)
                prefix = "refs/heads/";
            else if (strncmp(src_exact_ref_path, "refs/tags/", 10) == 0)
                prefix = "refs/tags/";
            if (prefix != NULL) {
                char buf[SG_PATH_MAX];

                snprintf(buf, sizeof(buf), "%s%s", prefix, dst_raw);
                *ref_path_out = strdup(buf);
            }
        }
        if (*ref_path_out == NULL)
            return -1;
    }
    if (*ref_path_out == NULL)
        return -3;

    if (!sg_ref_name_valid_for_create(*ref_path_out)) {
        fprintf(stderr, "fatal: invalid refspec '%s'\n", raw_arg);
        free(*ref_path_out);
        *ref_path_out = NULL;
        return -2;
    }

    *remote_exists_out = 0;
    memset(old_id_out, 0, SG_SHA1_RAW_LEN); /* a not-found match means a brand new ref: old_id
                                               must be all-zero, not whatever was on the stack */
    for (i = 0; i < adv->count; i++) {
        if (strcmp(adv->refs[i].name, *ref_path_out) == 0) {
            memcpy(old_id_out, adv->refs[i].id, SG_SHA1_RAW_LEN);
            *remote_exists_out = 1;
            break;
        }
    }
    return 0;
}

/* Phase 39: usage is duplicated at three call sites below (argument-parsing
   failure, --tags/refspec mutual exclusion, --delete misuse) -- kept as one
   macro so all three stay in sync, the way CLAUDE.md's module notes call
   out for exactly this command. */
#define SG_PUSH_USAGE \
    "usage: sg push [<remote>] [<refspec>...] [--tags] [--force|-f] [--delete <name>...]\n"

int sg_cmd_push(int argc, char **argv)
{
    const char *remote = "origin";
    const char **refspec_args = NULL; /* borrowed pointers into argv */
    size_t refspec_arg_count = 0, refspec_arg_cap = 0;
    int force = 0;
    int tags_flag = 0;
    int delete_flag = 0;
    char *git_dir = NULL;
    char *url = NULL;
    char *safe_url = NULL;
    char *current_branch = NULL;
    char **tag_names = NULL;
    size_t tag_name_count = 0;
    sg_ref_adv adv;
    int have_adv = 0;
    unsigned char(*diff_ids)[SG_SHA1_RAW_LEN] = NULL;
    push_entry *candidates = NULL;
    size_t candidate_count = 0, candidate_cap = 0;
    push_entry *entries = NULL;
    size_t entry_count = 0, entry_cap = 0;
    int had_rejection = 0;
    /* Phase 46: "[+]:" cannot be expanded until the advertisement arrives,
       so the parse loop only records that it was asked for. */
    int matching_requested = 0;
    int matching_force = 0;
    int src_resolve_failed = 0; /* Phase 39: an explicit-dst refspec's <src>
                                   matched nothing -- aborts the ENTIRE push
                                   before any network round trip, see
                                   CLAUDE.md's push module note */
    /* refs/sg/chunks propagation (see the phase 6 push-side durability fix):
       populated below, right after the ref-update decisions, only when this
       repo has ever genuinely used chunked storage locally. */
    int send_chunks_update = 0;
    unsigned char chunks_old_id[SG_SHA1_RAW_LEN];
    unsigned char chunks_new_id[SG_SHA1_RAW_LEN];
    int rc = 1;

    {
        int i;
        int have_remote = 0;

        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0) {
                force = 1;
            } else if (strcmp(argv[i], "--tags") == 0) {
                tags_flag = 1;
            } else if (strcmp(argv[i], "--delete") == 0) {
                delete_flag = 1;
            } else if (argv[i][0] == '-') {
                fprintf(stderr, SG_PUSH_USAGE);
                free(refspec_args);
                return 1;
            } else if (!have_remote) {
                remote = argv[i];
                have_remote = 1;
            } else {
                if (refspec_arg_count == refspec_arg_cap) {
                    size_t new_cap = refspec_arg_cap == 0 ? 4 : refspec_arg_cap * 2;
                    const char **grown = realloc(refspec_args, new_cap * sizeof(*grown));

                    if (grown == NULL) {
                        fprintf(stderr, "sg: out of memory\n");
                        free(refspec_args);
                        return 1;
                    }
                    refspec_args = grown;
                    refspec_arg_cap = new_cap;
                }
                refspec_args[refspec_arg_count++] = argv[i];
            }
        }
    }

    if (delete_flag) {
        if (tags_flag || refspec_arg_count == 0) {
            fprintf(stderr, SG_PUSH_USAGE);
            free(refspec_args);
            return 1;
        }
        /* "Any one of them containing ':' rejects the whole command" --
           checked up front, before touching the network, same batch-abort
           shape as a refspec syntax error (docs/DESIGN.md Phase 39
           section 0.7). */
        {
            size_t i;

            for (i = 0; i < refspec_arg_count; i++) {
                if (strchr(refspec_args[i], ':') != NULL) {
                    fprintf(stderr, "fatal: --delete only accepts plain target ref names\n");
                    free(refspec_args);
                    return 1;
                }
                /* No patterns here either (Phase 46). --delete builds its
                   candidates directly and never goes through
                   sg_push_refspec_parse, so the star check that rejects a
                   wildcard deletion written as ":<pattern>" does not cover
                   this spelling -- and this is the one place where guessing
                   wrong deletes other people's refs. Measured: git rejects
                   BOTH spellings as `fatal: invalid refspec ':<name>'`
                   before connecting, whether or not the name is
                   refs/-qualified. Without this, an unqualified pattern got
                   as far as the network and was only stopped later by dst
                   completion finding no match. */
                if (strchr(refspec_args[i], '*') != NULL) {
                    fprintf(stderr, "fatal: invalid refspec ':%s'\n", refspec_args[i]);
                    free(refspec_args);
                    return 1;
                }
            }
        }
    } else if (tags_flag && refspec_arg_count > 0) {
        fprintf(stderr, SG_PUSH_USAGE);
        free(refspec_args);
        return 1;
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL) {
        free(refspec_args);
        return 1;
    }

    url = sg_repo_read_remote_url(git_dir, remote);
    if (url == NULL) {
        fprintf(stderr, "sg: remote '%s' is not configured (no [remote \"%s\"] url found in .git/config)\n", remote,
               remote);
        goto done;
    }
    safe_url = sg_url_redact(url);

    /* ---- decide what to push, entirely from local state ---- */

    if (tags_flag) {
        size_t i;

        if (sg_ref_list_under(git_dir, "refs/tags/", &tag_names, &tag_name_count) != 0) {
            fprintf(stderr, "sg: cannot list local tags\n");
            goto done;
        }
        /* tag_name_count == 0 is deliberately NOT an early return here: even
           with no tags to push, this is a real push invocation that must
           still contact the remote (sg_transport_ls_refs_push below) and
           still evaluate refs/sg/chunks propagation -- returning early would
           silently skip both, exactly like `sg push origin` with nothing new
           to send still round-trips to the remote instead of no-opping
           purely from local state. candidates simply stays empty; the
           unified "nothing to send" check after ls_refs_push (further down)
           is what actually prints "Everything up-to-date." here. */
        for (i = 0; i < tag_name_count; i++) {
            push_entry cand;
            char path[SG_PATH_MAX];

            snprintf(path, sizeof(path), "refs/tags/%s", tag_names[i]);
            memset(&cand, 0, sizeof(cand));
            if (sg_ref_read_path(git_dir, path, cand.new_id) != 0) {
                fprintf(stderr, "sg: cannot read tag '%s'\n", tag_names[i]);
                goto done;
            }
            cand.name = strdup(tag_names[i]);
            cand.ref_path = strdup(path);
            cand.is_tag = 1;
            if (cand.name == NULL || cand.ref_path == NULL ||
               push_entries_push(&candidates, &candidate_count, &candidate_cap, &cand) != 0) {
                free(cand.name);
                free(cand.ref_path);
                fprintf(stderr, "sg: out of memory\n");
                goto done;
            }
        }
    } else if (delete_flag) {
        /* --delete <name>...: each name is a bare target, already checked
           above to contain no ':'. Nothing more can be decided from local
           state alone -- dst completion (rule 1 only, no rule 2: there is
           no src) needs the remote's advertisement, so these candidates
           stay pending (explicit_dst) until that arrives. */
        size_t i;

        for (i = 0; i < refspec_arg_count; i++) {
            push_entry cand;

            /* Same pre-connection format check as sg_push_refspec_parse's
               explicit-dst case (docs/DESIGN.md Phase 39 section 3) --
               only meaningful when already "refs/"-prefixed, same reason. */
            if (strncmp(refspec_args[i], "refs/", 5) == 0 &&
               !sg_ref_name_valid_for_create(refspec_args[i])) {
                fprintf(stderr, "fatal: invalid refspec '%s'\n", refspec_args[i]);
                goto done;
            }

            memset(&cand, 0, sizeof(cand));
            cand.is_delete = 1;
            cand.explicit_dst = 1;
            cand.dst_raw = strdup(refspec_args[i]);
            cand.raw_arg = strdup(refspec_args[i]);
            if (cand.dst_raw == NULL || cand.raw_arg == NULL ||
               push_entries_push(&candidates, &candidate_count, &candidate_cap, &cand) != 0) {
                free(cand.dst_raw);
                free(cand.raw_arg);
                fprintf(stderr, "sg: out of memory\n");
                goto done;
            }
        }
    } else if (refspec_arg_count > 0) {
        size_t i;

        for (i = 0; i < refspec_arg_count; i++) {
            sg_push_refspec parsed;

            if (sg_push_refspec_parse(refspec_args[i], &parsed) != 0) {
                /* Round 3 fix: defense in depth alongside the fix inside
                   sg_push_refspec_parse itself -- *out is documented to be
                   zeroed on failure, so this is a no-op free today, but a
                   future change to that contract must not silently start
                   leaking again just because this call site trusted it. */
                sg_push_refspec_free(&parsed);
                goto done; /* message already printed */
            }

            if (parsed.is_matching) {
                /* Deferred: which branches "[+]:" means is a question only
                   the advertisement can answer, so nothing is built here.
                   Two "[+]:" arguments in one invocation collapse into one
                   expansion, forced if EITHER carried the '+'. */
                matching_requested = 1;
                matching_force = matching_force || parsed.force;
                sg_push_refspec_free(&parsed);
                continue;
            }

            if (parsed.is_wildcard) {
                /* Expanded from LOCAL refs, before the network round trip --
                   see wildcard_expand_candidates for why the source set is
                   not an intersection with the remote. A pattern matching
                   nothing is not an error: it simply contributes no
                   candidates and the invocation goes on to say "Everything
                   up-to-date." like any other push with nothing to send. */
                int wrc = wildcard_expand_candidates(git_dir, &parsed, refspec_args[i], &candidates,
                                                    &candidate_count, &candidate_cap);

                sg_push_refspec_free(&parsed);
                if (wrc != 0)
                    goto done; /* message already printed */
                continue;
            }

            if (parsed.is_delete) {
                push_entry cand;

                memset(&cand, 0, sizeof(cand));
                cand.is_delete = 1;
                cand.explicit_dst = 1;
                cand.refspec_force = parsed.force;
                cand.dst_raw = strdup(parsed.dst);
                cand.raw_arg = strdup(refspec_args[i]);
                if (cand.dst_raw == NULL || cand.raw_arg == NULL ||
                   push_entries_push(&candidates, &candidate_count, &candidate_cap, &cand) != 0) {
                    free(cand.dst_raw);
                    free(cand.raw_arg);
                    sg_push_refspec_free(&parsed);
                    fprintf(stderr, "sg: out of memory\n");
                    goto done;
                }
            } else if (parsed.dst == NULL) {
                /* No colon: dst is derived from the SAME literal-name rule
                   this command has always used (docs/DESIGN.md Phase 39
                   section 0.2), not the full rev-parse grammar below --
                   see resolve_refspec_src's own header comment for why
                   those two are deliberately different code paths. */
                unsigned char tag_id[SG_SHA1_RAW_LEN];
                char tag_path[SG_PATH_MAX];
                int tag_exists;
                int br_exists;

                snprintf(tag_path, sizeof(tag_path), "refs/tags/%s", parsed.src);
                tag_exists = (sg_ref_read_path(git_dir, tag_path, tag_id) == 0);
                br_exists = sg_ref_branch_exists(git_dir, parsed.src);

                if (tag_exists && br_exists) {
                    fprintf(stderr, "sg: src refspec '%s' matches more than one\n", parsed.src);
                    sg_push_refspec_free(&parsed);
                    goto done;
                } else if (tag_exists) {
                    push_entry cand;

                    memset(&cand, 0, sizeof(cand));
                    cand.name = strdup(parsed.src);
                    cand.ref_path = strdup(tag_path);
                    cand.is_tag = 1;
                    cand.refspec_force = parsed.force;
                    memcpy(cand.new_id, tag_id, SG_SHA1_RAW_LEN);
                    if (cand.name == NULL || cand.ref_path == NULL ||
                       push_entries_push(&candidates, &candidate_count, &candidate_cap, &cand) != 0) {
                        free(cand.name);
                        free(cand.ref_path);
                        sg_push_refspec_free(&parsed);
                        fprintf(stderr, "sg: out of memory\n");
                        goto done;
                    }
                } else {
                    if (build_branch_candidate(git_dir, parsed.src, &candidates, &candidate_count,
                                               &candidate_cap) != 0) {
                        sg_push_refspec_free(&parsed);
                        goto done;
                    }
                    candidates[candidate_count - 1].refspec_force = parsed.force;
                }
            } else {
                /* Explicit dst: full rev-parse grammar for src (Stage B),
                   done now, before any network round trip -- a failure
                   here aborts the WHOLE push, not just this one ref (see
                   CLAUDE.md's push module note and docs/DESIGN.md Phase 39
                   section 1). Dst completion (Stage C) needs the
                   advertisement, so it is deferred (explicit_dst). */
                push_entry cand;
                int src_rc;

                memset(&cand, 0, sizeof(cand));
                src_rc = resolve_refspec_src(git_dir, parsed.src, cand.new_id, &cand.src_exact_ref_path);
                if (src_rc == -2) {
                    sg_push_refspec_free(&parsed);
                    goto done; /* message already printed */
                }
                if (src_rc == -3) {
                    /* Round 3 fix: an allocation failure is not the same
                       kind of thing as "src matched nothing" -- every other
                       OOM path in this function aborts immediately
                       (fprintf + goto done), continuing to parse the rest of
                       the batch on top of a failed allocation is exactly the
                       inconsistency this fix removes. */
                    sg_push_refspec_free(&parsed);
                    fprintf(stderr, "sg: out of memory\n");
                    goto done;
                }
                if (src_rc != 0) {
                    fprintf(stderr, "error: src refspec %s does not match any\n", parsed.src);
                    src_resolve_failed = 1;
                    sg_push_refspec_free(&parsed);
                    continue;
                }

                cand.is_delete = 0;
                cand.explicit_dst = 1;
                cand.refspec_force = parsed.force;
                cand.dst_raw = strdup(parsed.dst);
                cand.raw_arg = strdup(refspec_args[i]);
                if (cand.dst_raw == NULL || cand.raw_arg == NULL ||
                   push_entries_push(&candidates, &candidate_count, &candidate_cap, &cand) != 0) {
                    free(cand.dst_raw);
                    free(cand.src_exact_ref_path);
                    free(cand.raw_arg);
                    sg_push_refspec_free(&parsed);
                    fprintf(stderr, "sg: out of memory\n");
                    goto done;
                }
            }
            sg_push_refspec_free(&parsed);
        }

        if (src_resolve_failed) {
            fprintf(stderr, "error: failed to push some refs to '%s'\n",
                   safe_url != NULL ? safe_url : "(remote)");
            goto done;
        }
    } else {
        /* See cmd_reset.c: a corrupt HEAD is not a detached one. Here the
           advice differs too -- naming a branch explicitly does work around a
           detached HEAD, but not a HEAD that cannot be read at all. */
        current_branch = sg_ref_current_branch(git_dir);
        if (current_branch == NULL) {
            if (sg_ref_head_is_detached(git_dir) == 1)
                fprintf(stderr,
                       "sg: currently in detached HEAD, please name the branch to push explicitly: sg push %s <branch>\n",
                       remote);
            else
                fprintf(stderr, "sg: cannot read HEAD (.git/HEAD is neither a branch nor a commit id)\n");
            goto done;
        }
        if (build_branch_candidate(git_dir, current_branch, &candidates, &candidate_count,
                                   &candidate_cap) != 0)
            goto done;
    }

    if (sg_transport_ls_refs_push(url, &adv) != 0)
        goto done;
    have_adv = 1;

    /* The deferred half of Phase 46: "[+]:" means every local branch that
       already exists on the remote, so it can only be expanded now that the
       advertisement is in hand. Appended to `candidates` BEFORE the
       candidate->entry loop below, so every rule that loop already
       implements -- fast-forward checks, per-ref rejection, the report --
       applies unchanged. */
    if (matching_requested &&
       matching_expand_candidates(git_dir, &adv, matching_force, &candidates, &candidate_count,
                                 &candidate_cap) != 0)
        goto done;

    /* ---- resolve each candidate against the remote's advertisement into
       the final list of ref updates to actually send ---- */
    {
        size_t ci;

        for (ci = 0; ci < candidate_count; ci++) {
            push_entry *cand = &candidates[ci];
            unsigned char remote_old_id[SG_SHA1_RAW_LEN];
            int remote_ref_exists = 0;
            int cand_force;
            size_t j;

            if (cand->explicit_dst) {
                /* Stage C: dst completion, deferred until now because it
                   needs the advertisement (docs/DESIGN.md Phase 39 section
                   2). Covers both a "<src>:<dst>" candidate and a
                   "--delete"/":dst" one -- the latter passes NULL for
                   src_exact_ref_path, so rule 2 can never fire for it. */
                char *completed_path = NULL;
                int comp_rc = complete_dst(&adv, cand->dst_raw, cand->src_exact_ref_path, cand->raw_arg,
                                           &completed_path, remote_old_id, &remote_ref_exists);

                if (comp_rc == -1) {
                    if (cand->is_delete) {
                        fprintf(stderr, "sg: unable to delete '%s': remote ref does not exist\n",
                               cand->dst_raw);
                        had_rejection = 1;
                        continue; /* per-ref failure (docs/DESIGN.md Phase 39 section 5) */
                    }
                    fprintf(stderr,
                           "error: The destination you provided is not a full refname (i.e., starting "
                           "with \"refs/\").\n");
                    goto done;
                }
                if (comp_rc == -2)
                    goto done; /* message already printed by complete_dst */
                if (comp_rc == -4)
                    goto done; /* message already printed by complete_dst, round 3 */
                if (comp_rc == -3) {
                    fprintf(stderr, "sg: out of memory\n");
                    goto done;
                }

                cand->ref_path = completed_path;
                cand->is_tag = strncmp(completed_path, "refs/tags/", 10) == 0;
                /* Round 3 fix: `name` must be the WHOLE remainder after
                   stripping the completed path's own namespace prefix, not
                   just the last path segment -- a multi-segment dst like
                   "master:notrefs/x" completes (via rule 2) to
                   "refs/heads/notrefs/x", and taking only the last segment
                   (the old `strrchr(..., '/')` behavior) silently truncated
                   both the remote-tracking ref this builds later
                   ("refs/remotes/<remote>/<name>") and the report line to
                   ".../x" instead of ".../notrefs/x". For a completed path
                   under neither refs/heads/ nor refs/tags/ (an explicit
                   refs/remotes/... dst, or some other refs/... path), there
                   is no well-known namespace prefix to strip, so this falls
                   back to the last path segment, same as before. */
                if (strncmp(completed_path, "refs/heads/", 11) == 0)
                    cand->name = strdup(completed_path + 11);
                else if (strncmp(completed_path, "refs/tags/", 10) == 0)
                    cand->name = strdup(completed_path + 10);
                else {
                    const char *slash = strrchr(completed_path, '/');

                    cand->name = strdup(slash != NULL ? slash + 1 : completed_path);
                }
                if (cand->name == NULL) {
                    fprintf(stderr, "sg: out of memory\n");
                    goto done;
                }
            } else {
                for (j = 0; j < adv.count; j++) {
                    if (strcmp(adv.refs[j].name, cand->ref_path) == 0) {
                        memcpy(remote_old_id, adv.refs[j].id, SG_SHA1_RAW_LEN);
                        remote_ref_exists = 1;
                        break;
                    }
                }
                if (!remote_ref_exists)
                    memset(remote_old_id, 0, sizeof(remote_old_id));
            }

            cand_force = force || cand->refspec_force;

            if (cand->is_delete) {
                /* Deletion candidate protocol: new_id all-zero, never
                   fast-forward-checked, never object-walked, never part of
                   the pack (docs/DESIGN.md Phase 39 section 4 -- the three
                   named callers that assume new_id is readable must never
                   see one of these). remote_ref_exists == 0 here can only
                   happen for a "refs/..."-prefixed dst that complete_dst
                   accepted syntactically but which the remote does not
                   actually have. */
                push_entry e;

                if (!remote_ref_exists) {
                    fprintf(stderr, "sg: unable to delete '%s': remote ref does not exist\n",
                           cand->dst_raw);
                    had_rejection = 1;
                    continue;
                }

                memset(&e, 0, sizeof(e));
                e.name = strdup(cand->name);
                e.ref_path = strdup(cand->ref_path);
                e.is_tag = cand->is_tag;
                e.is_delete = 1;
                memcpy(e.old_id, remote_old_id, SG_SHA1_RAW_LEN);
                memset(e.new_id, 0, sizeof(e.new_id));
                if (e.name == NULL || e.ref_path == NULL ||
                   push_entries_push(&entries, &entry_count, &entry_cap, &e) != 0) {
                    free(e.name);
                    free(e.ref_path);
                    fprintf(stderr, "sg: out of memory\n");
                    goto done;
                }
                continue;
            }

            if (!cand->is_tag) {
                sg_push_ff_status status =
                    check_fast_forward(git_dir, remote_old_id, cand->new_id, remote_ref_exists);
                push_entry e;

                /* Phase 39: both of these used to be a whole-batch abort
                   (goto done) -- safe when a push could only ever carry one
                   non-tag ref, but with refspecs now able to name several,
                   docs/DESIGN.md Phase 39 section 5 (measured against real
                   git: "topic -> newbr" lands while "master -> fromhead" is
                   refused IN THE SAME invocation) requires this to be a
                   per-ref skip instead, same shape the tag path and the
                   deletion path already use just above. A single-ref push's
                   own observable behavior (message, exit code) is
                   unaffected by this change. */
                if (status == SG_PUSH_UNKNOWN_REMOTE) {
                    fprintf(stderr,
                           "sg: push refused: the remote's %s has commits we don't know about, "
                           "cannot tell whether this would overwrite someone else's work; run sg fetch first\n",
                           cand->name);
                    had_rejection = 1;
                    continue;
                }
                if (status == SG_PUSH_NON_FF && !cand_force) {
                    fprintf(stderr,
                           "sg: push refused: not a fast-forward, this would lose the following commits on the remote's %s:\n",
                           cand->name);
                    print_lost_commits(git_dir, remote_old_id, cand->new_id);
                    fprintf(stderr,
                           "sg: run sg fetch then sg merge to integrate the changes, or re-run with --force if you really want to overwrite\n");
                    had_rejection = 1;
                    continue;
                }
                if (remote_ref_exists && memcmp(remote_old_id, cand->new_id, SG_SHA1_RAW_LEN) == 0)
                    continue; /* already up to date -- nothing to send for this ref */

                memset(&e, 0, sizeof(e));
                e.name = strdup(cand->name);
                e.ref_path = strdup(cand->ref_path);
                e.is_tag = 0;
                e.is_new = !remote_ref_exists;
                e.forced = (status == SG_PUSH_NON_FF); /* Phase 39: only reachable via cand_force above */
                memcpy(e.old_id, remote_old_id, SG_SHA1_RAW_LEN);
                memcpy(e.new_id, cand->new_id, SG_SHA1_RAW_LEN);
                if (e.name == NULL || e.ref_path == NULL ||
                   push_entries_push(&entries, &entry_count, &entry_cap, &e) != 0) {
                    free(e.name);
                    free(e.ref_path);
                    fprintf(stderr, "sg: out of memory\n");
                    goto done;
                }
                continue;
            }

            /* tag: unlike a branch, never fast-forward-checked or merge-based
               -- an existing remote tag pointing somewhere else is always
               rejected outright unless --force says to overwrite it, and a
               rejection here only skips this one tag rather than aborting
               the whole push (so `--tags` can still land the rest). */
            if (!remote_ref_exists || memcmp(remote_old_id, cand->new_id, SG_SHA1_RAW_LEN) != 0) {
                if (remote_ref_exists && !cand_force) {
                    fprintf(stderr,
                           "sg: tag '%s' already exists on the remote (use --force to overwrite)\n",
                           cand->name);
                    had_rejection = 1;
                    continue;
                }

                {
                    push_entry e;

                    memset(&e, 0, sizeof(e));
                    e.name = strdup(cand->name);
                    e.ref_path = strdup(cand->ref_path);
                    e.is_tag = 1;
                    e.is_new = !remote_ref_exists;
                    e.forced = remote_ref_exists;
                    memcpy(e.old_id, remote_old_id, SG_SHA1_RAW_LEN);
                    memcpy(e.new_id, cand->new_id, SG_SHA1_RAW_LEN);
                    if (e.name == NULL || e.ref_path == NULL ||
                       push_entries_push(&entries, &entry_count, &entry_cap, &e) != 0) {
                        free(e.name);
                        free(e.ref_path);
                        fprintf(stderr, "sg: out of memory\n");
                        goto done;
                    }
                }
            }
            /* else: remote already has this exact tag id -- already up to
               date, nothing to send. */
        }
    }

    /* Round 3 fix: had_rejection with an empty entries list means EVERY
       candidate was rejected (non-fast-forward, unknown remote, etc, all of
       which "continue" instead of aborting -- see docs/DESIGN.md Phase 39
       section 5), not "nothing needed sending". Before this fix such a push
       fell through into the unconditional chunks-propagation block below,
       which still builds a pack and calls sg_transport_push whenever this
       repo has ever used chunked storage locally -- an actual write to the
       remote (and a spurious "To <url>" line) for a push whose sole refspec
       was refused outright, contradicting pre-Phase-39 behavior where a
       rejected single-ref push never touched the remote beyond the
       read-only advertisement. Do NOT fold this into the entry_count == 0
       check just below: "everything already up to date" is ALSO
       entry_count == 0, but had_rejection == 0 there, and that case must
       still be allowed to propagate refs/sg/chunks. */
    if (had_rejection && entry_count == 0)
        goto done;

    /* refs/sg/chunks propagation: if this repo has never chunked anything
       locally, SG_CHUNK_KEEPALIVE_REF simply doesn't exist here and there is
       nothing to protect on the remote -- skip all of the below entirely.
       Otherwise, compare our local keep-alive commit against whatever the
       remote already advertised for this same ref name (adv, from the
       sg_transport_ls_refs_push call above, includes it like any other ref
       that passes sg_ref_name_is_safe). */
    {
        unsigned char local_keepalive_id[SG_SHA1_RAW_LEN];

        if (sg_ref_read_path(git_dir, SG_CHUNK_KEEPALIVE_REF, local_keepalive_id) == 0) {
            int remote_has_chunks_ref = 0;
            unsigned char remote_chunks_old_id[SG_SHA1_RAW_LEN];
            size_t i;

            for (i = 0; i < adv.count; i++) {
                if (strcmp(adv.refs[i].name, SG_CHUNK_KEEPALIVE_REF) == 0) {
                    memcpy(remote_chunks_old_id, adv.refs[i].id, SG_SHA1_RAW_LEN);
                    remote_has_chunks_ref = 1;
                    break;
                }
            }

            if (!remote_has_chunks_ref) {
                /* The remote has never seen this ref at all: create it. */
                memset(chunks_old_id, 0, sizeof(chunks_old_id));
                memcpy(chunks_new_id, local_keepalive_id, SG_SHA1_RAW_LEN);
                send_chunks_update = 1;
            } else if (memcmp(remote_chunks_old_id, local_keepalive_id, SG_SHA1_RAW_LEN) == 0) {
                /* Already in sync -- nothing to send for this ref. */
            } else {
                sg_obj_type remote_commit_type;
                unsigned char *remote_commit_content = NULL;
                size_t remote_commit_content_len = 0;

                /* The remote's current tip differs from ours. If we happen
                   to already have that exact commit locally (e.g. from an
                   earlier `sg fetch`/`sg clone` that merged it in, or just a
                   loose object still lying around), merge it into our own
                   keep-alive tree -- a deduped union, same as
                   sg_chunk_keepalive_merge_commit's fetch-side use in
                   cmd_fetch.c -- rather than clobbering either side's set of
                   protected chunks. If we can't read it at all, we have no
                   way to know what the remote's chunk set even contains, so
                   there's no safe way to proceed: abort the whole push
                   rather than risk silently dropping the remote's existing
                   protection for chunks we don't know about. */
                if (sg_object_read(git_dir, remote_chunks_old_id, &remote_commit_type,
                                   &remote_commit_content, &remote_commit_content_len) == 0) {
                    free(remote_commit_content);

                    if (sg_chunk_keepalive_merge_commit(git_dir, remote_chunks_old_id) != 0 ||
                       sg_ref_read_path(git_dir, SG_CHUNK_KEEPALIVE_REF, local_keepalive_id) != 0) {
                        fprintf(stderr,
                               "sg: push refused: the remote's refs/sg/chunks has content the local side cannot read, "
                               "cannot merge safely; run sg fetch first\n");
                        goto done;
                    }
                    memcpy(chunks_old_id, remote_chunks_old_id, SG_SHA1_RAW_LEN);
                    memcpy(chunks_new_id, local_keepalive_id, SG_SHA1_RAW_LEN);
                    send_chunks_update = 1;
                } else {
                    fprintf(stderr,
                           "sg: push refused: the remote's refs/sg/chunks has content the local side cannot read, "
                           "cannot merge safely; run sg fetch first\n");
                    goto done;
                }
            }
        }
    }

    if (entry_count == 0 && !send_chunks_update) {
        if (!had_rejection) {
            printf("Everything up-to-date.\n");
            rc = 0;
        }
        goto done;
    }

    {
        id_set have_set, send_set;
        size_t diff_count = 0;
        int build_ok = 1;
        size_t i;

        id_set_init(&have_set);
        id_set_init(&send_set);

        if (build_have_set(git_dir, &adv, &have_set) != 0)
            build_ok = 0;
        for (i = 0; build_ok && i < entry_count; i++) {
            /* Phase 39: a deletion's new_id is all-zero, not a real object
               -- must never reach walk_add_object (see the module note in
               CLAUDE.md for the three call sites that assume new_id is
               readable). */
            if (entries[i].is_delete)
                continue;
            if (walk_add_object(git_dir, entries[i].new_id, &send_set) < 0)
                build_ok = 0;
        }
        /* The (possibly newly merged) keep-alive commit is deliberately
           parentless and ref-only -- never reachable from any branch/tag
           history walked just above -- so its tree/entries (the actual raw
           chunk blobs, not the pointer blobs that name them) must be walked
           into send_set separately, or the remote would end up with
           refs/sg/chunks pointing at objects it was never sent. */
        if (build_ok && send_chunks_update && walk_add_object(git_dir, chunks_new_id, &send_set) < 0)
            build_ok = 0;
        if (build_ok && compute_diff_ids(&send_set, &have_set, &diff_ids, &diff_count) != 0)
            build_ok = 0;

        id_set_free(&have_set);
        id_set_free(&send_set);

        if (!build_ok) {
            fprintf(stderr, "sg: failed to compute the objects to push\n");
            goto done;
        }

        {
            unsigned char *pack_data = NULL;
            size_t pack_len = 0;
            int use_sb64k = adv.capabilities != NULL &&
                            strstr(adv.capabilities, "side-band-64k") != NULL;
            int use_atomic = 0;
            sg_push_report report;
            int push_rc;
            sg_push_ref_update *updates;
            size_t update_count = 0;

            if (sg_pack_build_buf(git_dir, diff_ids, diff_count, &pack_data, &pack_len) != 0) {
                fprintf(stderr, "sg: failed to build the packfile to push\n");
                goto done;
            }

            updates = malloc((entry_count + (send_chunks_update ? 1 : 0)) * sizeof(*updates));
            if (updates == NULL) {
                fprintf(stderr, "sg: out of memory\n");
                free(pack_data);
                goto done;
            }

            for (i = 0; i < entry_count; i++) {
                memcpy(updates[update_count].old_id, entries[i].old_id, SG_SHA1_RAW_LEN);
                memcpy(updates[update_count].new_id, entries[i].new_id, SG_SHA1_RAW_LEN);
                updates[update_count].ref_name = entries[i].ref_path;
                update_count++;
            }
            if (send_chunks_update) {
                memcpy(updates[update_count].old_id, chunks_old_id, SG_SHA1_RAW_LEN);
                memcpy(updates[update_count].new_id, chunks_new_id, SG_SHA1_RAW_LEN);
                updates[update_count].ref_name = SG_CHUNK_KEEPALIVE_REF;
                update_count++;
            }

            /* Only meaningful with more than one command, and only when the
               server offered it: several refs (a branch, refs/sg/chunks, or
               multiple tags with --tags) must not be able to land
               separately. */
            use_atomic = update_count > 1 && adv.capabilities != NULL &&
                         strstr(adv.capabilities, "atomic") != NULL;

            memset(&report, 0, sizeof(report));
            push_rc = sg_transport_push(url, updates, update_count, use_sb64k, use_atomic, pack_data,
                                        pack_len, &report);
            free(updates);
            free(pack_data);

            if (push_rc != 0) {
                fprintf(stderr, "sg: push to %s failed\n", safe_url != NULL ? safe_url : "(remote)");
                goto done;
            }

            if (!report.unpack_ok) {
                fprintf(stderr, "sg: remote unpack failed: %s\n",
                       report.unpack_error != NULL ? report.unpack_error : "(unknown error)");
                sg_push_report_free(&report);
                goto done;
            }

            {
                int any_ng = 0;
                int chunks_ref_ok = 0;

                printf("To %s\n", safe_url != NULL ? safe_url : "(remote)");
                for (i = 0; i < entry_count; i++) {
                    push_entry *e = &entries[i];
                    size_t j;
                    int found = 0;

                    for (j = 0; j < report.ref_count; j++) {
                        if (strcmp(report.refs[j].ref_name, e->ref_path) != 0)
                            continue;
                        found = 1;

                        if (report.refs[j].ok) {
                            if (e->is_delete) {
                                /* Phase 39: sg's own report styling (no byte-for-byte git
                                   output requirement for this line, see docs/DESIGN.md
                                   Phase 39 section 6.3). */
                                printf(" - [deleted]           %s -> %s/%s\n", e->name, remote, e->name);
                            } else if (e->is_new) {
                                printf(" * [new %s]%s%s -> %s/%s\n", e->is_tag ? "tag" : "branch",
                                      e->is_tag ? "         " : "      ", e->name, remote, e->name);
                            } else {
                                char old_hex[SG_SHA1_HEX_LEN + 1];
                                char new_hex[SG_SHA1_HEX_LEN + 1];

                                sg_sha1_to_hex(e->old_id, old_hex);
                                sg_sha1_to_hex(e->new_id, new_hex);
                                printf("   %.7s..%.7s  %s -> %s/%s%s\n", old_hex, new_hex, e->name,
                                      remote, e->name, e->forced ? " (forced update)" : "");
                            }
                        } else {
                            const char *why = report.refs[j].message;

                            any_ng = 1;
                            fprintf(stderr, "sg: remote rejected update of %s: ", e->ref_path);
                            if (why != NULL)
                                sg_print_remote_text((const unsigned char *)why, strlen(why), stderr);
                            else
                                fputs("(unknown reason)", stderr);
                            fputc('\n', stderr);
                        }
                        break;
                    }

                    if (!found) {
                        fprintf(stderr, "sg: remote response did not include a result for %s\n", e->ref_path);
                        any_ng = 1;
                    }
                }

                for (i = 0; i < report.ref_count; i++) {
                    if (send_chunks_update && strcmp(report.refs[i].ref_name, SG_CHUNK_KEEPALIVE_REF) == 0) {
                        if (report.refs[i].ok) {
                            chunks_ref_ok = 1;
                        } else {
                            const char *why = report.refs[i].message;

                            fprintf(stderr, "sg: remote rejected update of %s: ", SG_CHUNK_KEEPALIVE_REF);
                            if (why != NULL)
                                sg_print_remote_text((const unsigned char *)why, strlen(why), stderr);
                            else
                                fputs("(unknown reason)", stderr);
                            fputc('\n', stderr);
                        }
                    }
                }

                sg_push_report_free(&report);

                if (any_ng)
                    goto done;
                /* Both a missing entry (server silently didn't apply it) and
                   an explicit "ng" (already printed above) must be treated
                   as failure here -- this is the CRITICAL bug this whole
                   change exists to fix: a chunked file whose protective ref
                   didn't verifiably land on the remote must never be
                   reported as a successful push, even though the other refs
                   may well have gone through (git applies each ref update
                   independently). */
                if (send_chunks_update && !chunks_ref_ok) {
                    fprintf(stderr,
                           "sg: the chunk-data protection ref (%s) failed to push, the remote's chunked files cannot be restored\n",
                           SG_CHUNK_KEEPALIVE_REF);
                    goto done;
                }
            }

            /* Real git never creates a remote-tracking ref for a pushed tag.
               Phase 39: a single push can now update more than one branch
               (multiple refspecs), so every non-tag, non-delete entry gets
               its own remote-tracking ref update -- no longer just the
               first one (a deletion has no meaningful new_id to record
               here, and this milestone doesn't remove the local
               remote-tracking ref either, see the Phase 39 write-up). */
            for (i = 0; i < entry_count; i++) {
                /* Only a destination under refs/heads/ gets a
                   remote-tracking ref. Before Phase 46 every non-tag,
                   non-delete entry did, which was harmless while a dst could
                   only be a branch or a tag; a wildcard can aim at any
                   namespace, and mirroring refs/remotes/up/topic/sub into
                   refs/remotes/origin/... would invent a tracking ref for
                   something that is not a remote branch at all. */
                if (!entries[i].is_tag && !entries[i].is_delete &&
                   strncmp(entries[i].ref_path, "refs/heads/", 11) == 0) {
                    char remote_ref_path[SG_PATH_MAX];

                    snprintf(remote_ref_path, sizeof(remote_ref_path), "refs/remotes/%s/%s", remote,
                            entries[i].name);
                    /* Fixed message, no remote name embedded (measured
                       against real git 2.55.0: "update by push", not
                       "push origin: ..."). entries[i].old_id is the
                       REMOTE-advertised old value, not necessarily this
                       local remote-tracking ref's current value, so it must
                       not be passed here -- sg_ref_update reads the real
                       local old value itself. */
                    if (sg_ref_update(git_dir, remote_ref_path, entries[i].new_id,
                                      "update by push") != 0) {
                        fprintf(stderr, "sg: push succeeded, but failed to update local %s\n", remote_ref_path);
                        goto done;
                    }
                }
            }
        }
    }

    rc = had_rejection ? 1 : 0;

done:
    if (have_adv)
        sg_ref_adv_free(&adv);
    free(diff_ids);
    push_entry_free_all(candidates, candidate_count);
    push_entry_free_all(entries, entry_count);
    free_string_array(tag_names, tag_name_count);
    free(current_branch);
    free(refspec_args);
    free(safe_url);
    free(url);
    free(git_dir);
    return rc;
}
