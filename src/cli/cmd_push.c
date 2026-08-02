#include "sg/cli.h"

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

#define SG_PATH_MAX 4096
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
        case SG_OBJ_BLOB:
            free(content);
            break;

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

/* Writes a single ref file (e.g. refs/remotes/<remote>/<branch>) as
   "<40-hex-id>\n" -- same shape as cmd_fetch.c's/cmd_clone.c's own copy of
   this helper. */
static int write_ref_file(const char *git_dir, const char *ref_path,
                          const unsigned char id[SG_SHA1_RAW_LEN])
{
    char full_path[SG_PATH_MAX];
    char content[SG_SHA1_HEX_LEN + 2];

    snprintf(full_path, sizeof(full_path), "%s/%s", git_dir, ref_path);
    sg_sha1_to_hex(id, content);
    content[SG_SHA1_HEX_LEN] = '\n';
    content[SG_SHA1_HEX_LEN + 1] = '\0';
    return sg_write_file_mkdirs(full_path, (const unsigned char *)content, SG_SHA1_HEX_LEN + 1, 0644);
}

int sg_cmd_push(int argc, char **argv)
{
    const char *remote = "origin";
    char *branch_arg = NULL;
    int force = 0;
    char *git_dir = NULL;
    char *url = NULL;
    char *safe_url = NULL;
    char *current_branch = NULL;
    const char *branch;
    char ref_name[SG_PATH_MAX];
    unsigned char local_id[SG_SHA1_RAW_LEN];
    unsigned char remote_old_id[SG_SHA1_RAW_LEN];
    int remote_ref_exists = 0;
    sg_ref_adv adv;
    int have_adv = 0;
    unsigned char(*diff_ids)[SG_SHA1_RAW_LEN] = NULL;
    int rc = 1;

    {
        int i;
        const char *positional[2];
        int npos = 0;

        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0) {
                force = 1;
            } else if (argv[i][0] == '-') {
                fprintf(stderr, "usage: sg push [<remote>] [<branch>] [--force|-f]\n");
                return 1;
            } else if (npos < 2) {
                positional[npos++] = argv[i];
            } else {
                fprintf(stderr, "usage: sg push [<remote>] [<branch>] [--force|-f]\n");
                return 1;
            }
        }
        if (npos >= 1)
            remote = positional[0];
        if (npos >= 2) {
            branch_arg = strdup(positional[1]);
            if (branch_arg == NULL) {
                fprintf(stderr, "sg: out of memory\n");
                return 1;
            }
        }
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;

    url = sg_repo_read_remote_url(git_dir, remote);
    if (url == NULL) {
        fprintf(stderr, "sg: remote '%s' 未設定 (找不到 .git/config 裡的 [remote \"%s\"] url)\n", remote,
               remote);
        goto done;
    }
    safe_url = sg_url_redact(url);

    if (branch_arg != NULL) {
        branch = branch_arg;
    } else {
        current_branch = sg_ref_current_branch(git_dir);
        if (current_branch == NULL) {
            fprintf(stderr, "sg: 目前是 detached HEAD，請明確指定要推送的分支：sg push %s <branch>\n",
                   remote);
            goto done;
        }
        branch = current_branch;
    }

    if (sg_ref_read_branch(git_dir, branch, local_id) != 0) {
        fprintf(stderr, "sg: 分支 '%s' 尚無任何 commit，沒有東西可以推送\n", branch);
        goto done;
    }
    snprintf(ref_name, sizeof(ref_name), "refs/heads/%s", branch);

    if (sg_transport_ls_refs_push(url, &adv) != 0)
        goto done;
    have_adv = 1;

    {
        size_t i;

        for (i = 0; i < adv.count; i++) {
            if (strcmp(adv.refs[i].name, ref_name) == 0) {
                memcpy(remote_old_id, adv.refs[i].id, SG_SHA1_RAW_LEN);
                remote_ref_exists = 1;
                break;
            }
        }
    }
    if (!remote_ref_exists)
        memset(remote_old_id, 0, sizeof(remote_old_id));

    {
        sg_push_ff_status status = check_fast_forward(git_dir, remote_old_id, local_id, remote_ref_exists);

        if (status == SG_PUSH_UNKNOWN_REMOTE) {
            fprintf(stderr,
                   "sg: 拒絕推送：遠端的 %s 有我們不知道的 commit，"
                   "無法判斷是否會覆蓋別人的工作，請先執行 sg fetch\n",
                   branch);
            goto done;
        }
        if (status == SG_PUSH_NON_FF && !force) {
            fprintf(stderr, "sg: 拒絕推送：這不是 fast-forward，會讓遠端 %s 上的以下 commit 遺失：\n",
                   branch);
            print_lost_commits(git_dir, remote_old_id, local_id);
            fprintf(stderr, "sg: 請先 sg fetch 後 sg merge 整合變更，或確定要覆蓋就加上 --force 重新執行\n");
            goto done;
        }
    }

    {
        id_set have_set, send_set;
        size_t diff_count = 0;
        int build_ok = 1;

        id_set_init(&have_set);
        id_set_init(&send_set);

        if (build_have_set(git_dir, &adv, &have_set) != 0)
            build_ok = 0;
        if (build_ok && walk_add_object(git_dir, local_id, &send_set) < 0)
            build_ok = 0;
        if (build_ok && compute_diff_ids(&send_set, &have_set, &diff_ids, &diff_count) != 0)
            build_ok = 0;

        id_set_free(&have_set);
        id_set_free(&send_set);

        if (!build_ok) {
            fprintf(stderr, "sg: 計算要推送的物件時失敗\n");
            goto done;
        }

        if (diff_count == 0 && remote_ref_exists && memcmp(remote_old_id, local_id, SG_SHA1_RAW_LEN) == 0) {
            printf("Everything up-to-date.\n");
            rc = 0;
            goto done;
        }

        {
            unsigned char *pack_data = NULL;
            size_t pack_len = 0;
            int use_sb64k = adv.capabilities != NULL &&
                            strstr(adv.capabilities, "side-band-64k") != NULL;
            sg_push_report report;
            int push_rc;

            if (sg_pack_build_buf(git_dir, diff_ids, diff_count, &pack_data, &pack_len) != 0) {
                fprintf(stderr, "sg: 建立要推送的 packfile 失敗\n");
                goto done;
            }

            memset(&report, 0, sizeof(report));
            push_rc = sg_transport_push(url, remote_old_id, local_id, ref_name, use_sb64k, pack_data,
                                        pack_len, &report);
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
                size_t i;
                int any_ng = 0;
                int found_our_ref = 0;

                printf("To %s\n", safe_url != NULL ? safe_url : "(remote)");
                for (i = 0; i < report.ref_count; i++) {
                    if (strcmp(report.refs[i].ref_name, ref_name) != 0)
                        continue;
                    found_our_ref = 1;

                    if (report.refs[i].ok) {
                        if (!remote_ref_exists) {
                            printf(" * [new branch]      %s -> %s/%s\n", branch, remote, branch);
                        } else {
                            char old_hex[SG_SHA1_HEX_LEN + 1];
                            char new_hex[SG_SHA1_HEX_LEN + 1];

                            sg_sha1_to_hex(remote_old_id, old_hex);
                            sg_sha1_to_hex(local_id, new_hex);
                            printf("   %.7s..%.7s  %s -> %s/%s\n", old_hex, new_hex, branch, remote,
                                  branch);
                        }
                    } else {
                        const char *why = report.refs[i].message;

                        any_ng = 1;
                        fprintf(stderr, "sg: 遠端拒絕更新 %s: ", ref_name);
                        if (why != NULL)
                            sg_print_remote_text((const unsigned char *)why, strlen(why), stderr);
                        else
                            fputs("(未知原因)", stderr);
                        fputc('\n', stderr);
                    }
                }

                sg_push_report_free(&report);

                if (!found_our_ref) {
                    fprintf(stderr, "sg: 遠端回應未包含 %s 的結果\n", ref_name);
                    goto done;
                }
                if (any_ng)
                    goto done;
            }

            {
                char remote_ref_path[SG_PATH_MAX];

                snprintf(remote_ref_path, sizeof(remote_ref_path), "refs/remotes/%s/%s", remote, branch);
                if (write_ref_file(git_dir, remote_ref_path, local_id) != 0) {
                    fprintf(stderr, "sg: push 成功，但更新本地的 %s 失敗\n", remote_ref_path);
                    goto done;
                }
            }
        }
    }

    rc = 0;

done:
    if (have_adv)
        sg_ref_adv_free(&adv);
    free(diff_ids);
    free(current_branch);
    free(branch_arg);
    free(safe_url);
    free(url);
    free(git_dir);
    return rc;
}
