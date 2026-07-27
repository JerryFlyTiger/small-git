#include "sg/cli.h"

#include "sg/confirm.h"
#include "sg/hash.h"
#include "sg/index.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/snapshot.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int flat_find(const sg_flat_list *list, const char *path)
{
    size_t lo = 0;
    size_t hi = list->count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(list->entries[mid].path, path);

        if (cmp == 0)
            return (int)mid;
        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

static int restore_worktree(const char *git_dir, const char *repo_root, sg_index *idx,
                            const char *arg, const char *rel)
{
    int pos = sg_index_find(idx, rel);
    sg_obj_type type;
    unsigned char *content;
    size_t content_len;
    char abspath[4096];
    int rc;

    if (pos < 0) {
        fprintf(stderr, "sg: '%s' is not tracked in the index\n", arg);
        return -1;
    }
    if (sg_loose_read(git_dir, idx->entries[pos].sha1, &type, &content, &content_len) != 0) {
        fprintf(stderr, "sg: failed to read staged content for '%s'\n", arg);
        return -1;
    }

    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, rel);
    rc = sg_write_file_mkdirs(abspath, content, content_len, (int)(idx->entries[pos].mode & 0777));
    free(content);
    if (rc != 0)
        fprintf(stderr, "sg: failed to restore '%s'\n", arg);
    return rc;
}

static void restore_staged(sg_index *idx, int has_head, const sg_flat_list *head_flat,
                           const char *rel)
{
    int head_pos = has_head ? flat_find(head_flat, rel) : -1;

    if (head_pos < 0) {
        sg_index_remove(idx, rel); /* not tracked by HEAD: unstaging removes it */
    } else {
        sg_index_entry entry;
        int existing = sg_index_find(idx, rel);

        memset(&entry, 0, sizeof(entry));
        if (existing >= 0)
            entry = idx->entries[existing];
        entry.mode = head_flat->entries[head_pos].mode;
        memcpy(entry.sha1, head_flat->entries[head_pos].sha1, SG_SHA1_RAW_LEN);
        entry.path = (char *)rel;
        sg_index_upsert(idx, &entry);
    }
}

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} strbuf;

static void strbuf_append(strbuf *b, const char *s)
{
    size_t slen = strlen(s);
    size_t needed = b->len + slen + 1;

    if (needed > b->cap) {
        size_t new_cap = b->cap == 0 ? 256 : b->cap * 2;
        char *grown;

        while (new_cap < needed)
            new_cap *= 2;
        grown = realloc(b->buf, new_cap);
        if (grown == NULL)
            return; /* best effort: message may end up truncated */
        b->buf = grown;
        b->cap = new_cap;
    }
    memcpy(b->buf + b->len, s, slen + 1);
    b->len += slen;
}

static void strbuf_append_path(strbuf *b, const char *path)
{
    char line[4200];

    snprintf(line, sizeof(line), "\t%s\n", path);
    strbuf_append(b, line);
}

/* Determines whether restoring rel from the index would actually discard
   working-directory content: a missing file or one whose content already
   matches the index entry loses nothing. */
static int would_lose_content(const char *repo_root, const sg_index *idx, const char *rel)
{
    int pos = sg_index_find(idx, rel);
    char abspath[4096];
    struct stat st;
    unsigned char wd_sha1[SG_SHA1_RAW_LEN];

    if (pos < 0)
        return 0; /* not tracked: restore will error out, nothing to lose */

    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, rel);
    if (stat(abspath, &st) != 0)
        return 0;
    if (sg_hash_file_blob(abspath, wd_sha1) != 0)
        return 0;
    return memcmp(wd_sha1, idx->entries[pos].sha1, SG_SHA1_RAW_LEN) != 0;
}

int sg_cmd_restore(int argc, char **argv)
{
    static const char usage[] = "usage: sg restore [--staged] [--force|-f] <path>...\n";
    int staged = 0;
    int force = 0;
    int npaths = 0;
    char *git_dir;
    char *repo_root;
    sg_index idx;
    unsigned char head_commit_id[SG_SHA1_RAW_LEN];
    int has_head = 0;
    sg_flat_list head_flat;
    int i;
    int rc = 0;

    memset(&head_flat, 0, sizeof(head_flat));

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--staged") == 0)
            staged = 1;
        else if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0)
            force = 1;
        else
            npaths++;
    }
    if (npaths == 0) {
        fputs(usage, stderr);
        return 1;
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;
    repo_root = sg_repo_root(git_dir);
    if (repo_root == NULL) {
        fprintf(stderr, "sg: failed to determine repository root\n");
        free(git_dir);
        return 1;
    }
    if (sg_index_read(git_dir, &idx) != 0) {
        fprintf(stderr, "sg: failed to read index (corrupt?)\n");
        free(git_dir);
        free(repo_root);
        return 1;
    }

    if (staged) {
        has_head = (sg_ref_resolve_head(git_dir, head_commit_id) == 0);
        if (has_head) {
            sg_obj_type type;
            unsigned char *content = NULL;
            size_t content_len;
            sg_commit commit;

            if (sg_loose_read(git_dir, head_commit_id, &type, &content, &content_len) == 0 &&
               type == SG_OBJ_COMMIT) {
                if (sg_commit_parse(content, content_len, &commit) == 0) {
                    sg_tree_flatten(git_dir, commit.tree, &head_flat);
                    sg_commit_free(&commit);
                }
                free(content);
            }
        }
    } else {
        strbuf msg = {0};
        strbuf affected = {0};
        int any_lossy = 0;

        for (i = 1; i < argc; i++) {
            char *rel;

            if (strcmp(argv[i], "--staged") == 0 || strcmp(argv[i], "--force") == 0 ||
               strcmp(argv[i], "-f") == 0)
                continue;

            rel = sg_resolve_repo_path(repo_root, argv[i]);
            if (rel == NULL)
                continue; /* reported as an error in the main loop below */

            if (would_lose_content(repo_root, &idx, rel)) {
                if (!any_lossy)
                    strbuf_append(&msg,
                                  "sg restore: 以下路徑目前的工作目錄內容跟 index 不同，"
                                  "還原後這些變更會遺失：\n");
                else
                    strbuf_append(&affected, ",");
                strbuf_append_path(&msg, rel);
                strbuf_append(&affected, rel);
                any_lossy = 1;
            }
            free(rel);
        }

        if (any_lossy && !sg_confirm_dangerous(msg.buf != NULL ? msg.buf : "", force)) {
            free(msg.buf);
            free(affected.buf);
            sg_flat_list_free(&head_flat);
            sg_index_free(&idx);
            free(repo_root);
            free(git_dir);
            fprintf(stderr, "sg: restore aborted\n");
            return 1;
        }

        /* confirmed (or nothing was actually at risk): snapshot before the
           real restore loop below touches anything -- a failed snapshot
           must abort instead of silently restoring unprotected */
        if (any_lossy) {
            char label[512];

            snprintf(label, sizeof(label), "restore %s", affected.buf != NULL ? affected.buf : "");
            if (sg_snapshot_create(git_dir, repo_root, &idx, label, NULL) != 0) {
                fprintf(stderr,
                       "sg: 自動快照失敗，為了安全起見中止這次還原（沒有做任何變更）\n");
                free(msg.buf);
                free(affected.buf);
                sg_flat_list_free(&head_flat);
                sg_index_free(&idx);
                free(repo_root);
                free(git_dir);
                return 1;
            }
        }
        free(msg.buf);
        free(affected.buf);
    }

    for (i = 1; i < argc; i++) {
        char *rel;

        if (strcmp(argv[i], "--staged") == 0 || strcmp(argv[i], "--force") == 0 ||
           strcmp(argv[i], "-f") == 0)
            continue;

        rel = sg_resolve_repo_path(repo_root, argv[i]);
        if (rel == NULL) {
            fprintf(stderr, "sg: '%s' is outside the repository\n", argv[i]);
            rc = 1;
            continue;
        }

        if (staged)
            restore_staged(&idx, has_head, &head_flat, rel);
        else if (restore_worktree(git_dir, repo_root, &idx, argv[i], rel) != 0)
            rc = 1;

        free(rel);
    }

    if (staged && sg_index_write(git_dir, &idx) != 0) {
        fprintf(stderr, "sg: failed to write index\n");
        rc = 1;
    }

    sg_flat_list_free(&head_flat);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
    return rc;
}
