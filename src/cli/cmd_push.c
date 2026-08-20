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
                       "sg: push 中止：分塊檔案的物件 %s 在本地物件庫中有缺失或損毀的資料塊，"
                       "無法確保推送完整資料\n",
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
        fprintf(stderr, "  ...還有 %zu 個\n", total_lost - shown);

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
    char *name;     /* malloc'd short name (branch or tag), owned */
    char *ref_path; /* malloc'd "refs/heads/<name>" or "refs/tags/<name>", owned */
    int is_tag;
    int is_new;  /* remote had no such ref before this push */
    int forced;  /* tag only: overwrote a differing remote tag via --force */
    unsigned char old_id[SG_SHA1_RAW_LEN];
    unsigned char new_id[SG_SHA1_RAW_LEN];
} push_entry;

static void push_entry_free_all(push_entry *entries, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++) {
        free(entries[i].name);
        free(entries[i].ref_path);
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
        fprintf(stderr, "sg: 分支 '%s' 尚無任何 commit，沒有東西可以推送\n", branch);
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

int sg_cmd_push(int argc, char **argv)
{
    const char *remote = "origin";
    char *name_arg = NULL;
    int force = 0;
    int tags_flag = 0;
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
    /* refs/sg/chunks propagation (see the phase 6 push-side durability fix):
       populated below, right after the ref-update decisions, only when this
       repo has ever genuinely used chunked storage locally. */
    int send_chunks_update = 0;
    unsigned char chunks_old_id[SG_SHA1_RAW_LEN];
    unsigned char chunks_new_id[SG_SHA1_RAW_LEN];
    int rc = 1;

    {
        int i;
        const char *positional[2];
        int npos = 0;

        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0) {
                force = 1;
            } else if (strcmp(argv[i], "--tags") == 0) {
                tags_flag = 1;
            } else if (argv[i][0] == '-') {
                fprintf(stderr, "usage: sg push [<remote>] [<name>] [--tags] [--force|-f]\n");
                return 1;
            } else if (npos < 2) {
                positional[npos++] = argv[i];
            } else {
                fprintf(stderr, "usage: sg push [<remote>] [<name>] [--tags] [--force|-f]\n");
                return 1;
            }
        }
        if (npos >= 1)
            remote = positional[0];
        if (npos >= 2) {
            name_arg = strdup(positional[1]);
            if (name_arg == NULL) {
                fprintf(stderr, "sg: out of memory\n");
                return 1;
            }
        }
    }

    if (tags_flag && name_arg != NULL) {
        fprintf(stderr, "usage: sg push [<remote>] [<name>] [--tags] [--force|-f]\n");
        free(name_arg);
        return 1;
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL) {
        free(name_arg);
        return 1;
    }

    url = sg_repo_read_remote_url(git_dir, remote);
    if (url == NULL) {
        fprintf(stderr, "sg: remote '%s' 未設定 (找不到 .git/config 裡的 [remote \"%s\"] url)\n", remote,
               remote);
        goto done;
    }
    safe_url = sg_url_redact(url);

    /* ---- decide what to push, entirely from local state ---- */

    if (tags_flag) {
        size_t i;

        if (sg_ref_list_under(git_dir, "refs/tags/", &tag_names, &tag_name_count) != 0) {
            fprintf(stderr, "sg: 無法列出本地的 tag\n");
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
                fprintf(stderr, "sg: 無法讀取 tag '%s'\n", tag_names[i]);
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
    } else if (name_arg != NULL) {
        unsigned char tag_id[SG_SHA1_RAW_LEN];
        char tag_path[SG_PATH_MAX];
        int tag_exists;
        int br_exists;

        snprintf(tag_path, sizeof(tag_path), "refs/tags/%s", name_arg);
        tag_exists = (sg_ref_read_path(git_dir, tag_path, tag_id) == 0);
        br_exists = sg_ref_branch_exists(git_dir, name_arg);

        if (tag_exists && br_exists) {
            fprintf(stderr, "sg: src refspec '%s' matches more than one\n", name_arg);
            goto done;
        } else if (tag_exists) {
            push_entry cand;

            memset(&cand, 0, sizeof(cand));
            cand.name = strdup(name_arg);
            cand.ref_path = strdup(tag_path);
            cand.is_tag = 1;
            memcpy(cand.new_id, tag_id, SG_SHA1_RAW_LEN);
            if (cand.name == NULL || cand.ref_path == NULL ||
               push_entries_push(&candidates, &candidate_count, &candidate_cap, &cand) != 0) {
                free(cand.name);
                free(cand.ref_path);
                fprintf(stderr, "sg: out of memory\n");
                goto done;
            }
        } else {
            if (build_branch_candidate(git_dir, name_arg, &candidates, &candidate_count,
                                       &candidate_cap) != 0)
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
                       "sg: 目前是 detached HEAD，請明確指定要推送的分支：sg push %s <branch>\n",
                       remote);
            else
                fprintf(stderr, "sg: 無法讀取 HEAD（.git/HEAD 的內容既不是分支也不是 commit id）\n");
            goto done;
        }
        if (build_branch_candidate(git_dir, current_branch, &candidates, &candidate_count,
                                   &candidate_cap) != 0)
            goto done;
    }

    if (sg_transport_ls_refs_push(url, &adv) != 0)
        goto done;
    have_adv = 1;

    /* ---- resolve each candidate against the remote's advertisement into
       the final list of ref updates to actually send ---- */
    {
        size_t ci;

        for (ci = 0; ci < candidate_count; ci++) {
            push_entry *cand = &candidates[ci];
            unsigned char remote_old_id[SG_SHA1_RAW_LEN];
            int remote_ref_exists = 0;
            size_t j;

            for (j = 0; j < adv.count; j++) {
                if (strcmp(adv.refs[j].name, cand->ref_path) == 0) {
                    memcpy(remote_old_id, adv.refs[j].id, SG_SHA1_RAW_LEN);
                    remote_ref_exists = 1;
                    break;
                }
            }
            if (!remote_ref_exists)
                memset(remote_old_id, 0, sizeof(remote_old_id));

            if (!cand->is_tag) {
                sg_push_ff_status status =
                    check_fast_forward(git_dir, remote_old_id, cand->new_id, remote_ref_exists);
                push_entry e;

                if (status == SG_PUSH_UNKNOWN_REMOTE) {
                    fprintf(stderr,
                           "sg: 拒絕推送：遠端的 %s 有我們不知道的 commit，"
                           "無法判斷是否會覆蓋別人的工作，請先執行 sg fetch\n",
                           cand->name);
                    goto done;
                }
                if (status == SG_PUSH_NON_FF && !force) {
                    fprintf(stderr,
                           "sg: 拒絕推送：這不是 fast-forward，會讓遠端 %s 上的以下 commit 遺失：\n",
                           cand->name);
                    print_lost_commits(git_dir, remote_old_id, cand->new_id);
                    fprintf(stderr,
                           "sg: 請先 sg fetch 後 sg merge 整合變更，或確定要覆蓋就加上 --force 重新執行\n");
                    goto done;
                }
                if (remote_ref_exists && memcmp(remote_old_id, cand->new_id, SG_SHA1_RAW_LEN) == 0)
                    continue; /* already up to date -- nothing to send for this ref */

                memset(&e, 0, sizeof(e));
                e.name = strdup(cand->name);
                e.ref_path = strdup(cand->ref_path);
                e.is_tag = 0;
                e.is_new = !remote_ref_exists;
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
                if (remote_ref_exists && !force) {
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
                               "sg: 拒絕推送：遠端的 refs/sg/chunks 有本地無法讀取的內容，"
                               "無法安全合併，請先執行 sg fetch\n");
                        goto done;
                    }
                    memcpy(chunks_old_id, remote_chunks_old_id, SG_SHA1_RAW_LEN);
                    memcpy(chunks_new_id, local_keepalive_id, SG_SHA1_RAW_LEN);
                    send_chunks_update = 1;
                } else {
                    fprintf(stderr,
                           "sg: 拒絕推送：遠端的 refs/sg/chunks 有本地無法讀取的內容，"
                           "無法安全合併，請先執行 sg fetch\n");
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
            fprintf(stderr, "sg: 計算要推送的物件時失敗\n");
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
                fprintf(stderr, "sg: 建立要推送的 packfile 失敗\n");
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
                fprintf(stderr, "sg: push 到 %s 失敗\n", safe_url != NULL ? safe_url : "(remote)");
                goto done;
            }

            if (!report.unpack_ok) {
                fprintf(stderr, "sg: 遠端 unpack 失敗: %s\n",
                       report.unpack_error != NULL ? report.unpack_error : "(未知錯誤)");
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
                            if (e->is_new) {
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
                            fprintf(stderr, "sg: 遠端拒絕更新 %s: ", e->ref_path);
                            if (why != NULL)
                                sg_print_remote_text((const unsigned char *)why, strlen(why), stderr);
                            else
                                fputs("(未知原因)", stderr);
                            fputc('\n', stderr);
                        }
                        break;
                    }

                    if (!found) {
                        fprintf(stderr, "sg: 遠端回應未包含 %s 的結果\n", e->ref_path);
                        any_ng = 1;
                    }
                }

                for (i = 0; i < report.ref_count; i++) {
                    if (send_chunks_update && strcmp(report.refs[i].ref_name, SG_CHUNK_KEEPALIVE_REF) == 0) {
                        if (report.refs[i].ok) {
                            chunks_ref_ok = 1;
                        } else {
                            const char *why = report.refs[i].message;

                            fprintf(stderr, "sg: 遠端拒絕更新 %s: ", SG_CHUNK_KEEPALIVE_REF);
                            if (why != NULL)
                                sg_print_remote_text((const unsigned char *)why, strlen(why), stderr);
                            else
                                fputs("(未知原因)", stderr);
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
                           "sg: chunk 資料的保護 ref (%s) 未能推送，遠端的分塊檔案將無法還原\n",
                           SG_CHUNK_KEEPALIVE_REF);
                    goto done;
                }
            }

            /* Real git never creates a remote-tracking ref for a pushed tag
               -- only for the (at most one) branch entry in this push. */
            for (i = 0; i < entry_count; i++) {
                if (!entries[i].is_tag) {
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
                        fprintf(stderr, "sg: push 成功，但更新本地的 %s 失敗\n", remote_ref_path);
                        goto done;
                    }
                    break;
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
    free(name_arg);
    free(safe_url);
    free(url);
    free(git_dir);
    return rc;
}
