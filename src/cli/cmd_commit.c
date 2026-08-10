#include "sg/cli.h"

#include "sg/hash.h"
#include "sg/index.h"
#include "sg/loose.h"
#include "sg/merge.h"
#include "sg/object.h"
#include "sg/rebase.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/tree_build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *env_or(const char *name, const char *fallback)
{
    const char *v = getenv(name);

    return (v != NULL && v[0] != '\0') ? v : fallback;
}

/* Prints every distinct path carrying a stage 1/2/3 entry (idx is sorted by
   (path, stage), so duplicates for one path are contiguous). */
static void print_unmerged_paths(const sg_index *idx)
{
    size_t i;

    fprintf(stderr, "sg: 尚有未解決的衝突，無法 commit：\n");
    for (i = 0; i < idx->count; i++) {
        if (idx->entries[i].stage == 0)
            continue;
        if (i > 0 && strcmp(idx->entries[i].path, idx->entries[i - 1].path) == 0)
            continue;
        fprintf(stderr, "\t%s\n", idx->entries[i].path);
    }
    fprintf(stderr, "請先解決衝突並執行 `sg add <file>...` 標記為已解決，再重新 commit。\n");
}

int sg_cmd_commit(int argc, char **argv)
{
    static const char usage[] = "usage: sg commit -m <message>\n";
    const char *message = NULL;
    char *cleaned_message = NULL;
    char *git_dir;
    sg_index idx;
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    unsigned char parent_id[SG_SHA1_RAW_LEN];
    int has_parent;
    char *branch;
    sg_commit commit;
    unsigned char *serialized;
    size_t serialized_len;
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    char commit_hex[SG_SHA1_HEX_LEN + 1];
    const char *name;
    const char *email;
    unsigned char merge_head_id[SG_SHA1_RAW_LEN];
    int is_merge_commit;
    size_t i;
    int rc = 0;

    for (i = 1; (int)i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0) {
            if ((int)i + 1 >= argc) {
                fputs(usage, stderr);
                return 1;
            }
            message = argv[++i];
        } else {
            fputs(usage, stderr);
            return 1;
        }
    }
    if (message == NULL) {
        fputs(usage, stderr);
        return 1;
    }

    if (sg_message_cleanup(message, &cleaned_message) != 0) {
        fprintf(stderr, "sg: out of memory\n");
        return 1;
    }
    if (cleaned_message[0] == '\0') {
        fprintf(stderr, "sg: aborting commit due to empty commit message\n");
        free(cleaned_message);
        return 1;
    }
    message = cleaned_message;

    git_dir = sg_require_git_dir();
    if (git_dir == NULL) {
        free(cleaned_message);
        return 1;
    }

    /* A rebase advances the current branch ref commit-by-commit as it
       replays; an ordinary `sg commit` here would create a normal commit on
       top of whatever partial progress exists, leaving the sequencer's
       todo/current state pointing at a sequence that no longer makes sense.
       Route the user through the rebase's own continuation instead. */
    if (sg_rebase_state_exists(git_dir)) {
        fprintf(stderr,
               "sg: 目前有一個進行中的 rebase，無法直接 commit\n"
               "請解決衝突並 `sg add <file>...` 後執行 `sg rebase --continue`，"
               "或執行 `sg rebase --abort` 放棄\n");
        free(git_dir);
        free(cleaned_message);
        return 1;
    }

    if (sg_index_read(git_dir, &idx) != 0) {
        fprintf(stderr, "sg: failed to read index (corrupt?)\n");
        free(git_dir);
        free(cleaned_message);
        return 1;
    }

    if (sg_index_has_unmerged(&idx)) {
        print_unmerged_paths(&idx);
        sg_index_free(&idx);
        free(git_dir);
        free(cleaned_message);
        return 1;
    }

    is_merge_commit = (sg_merge_head_read(git_dir, merge_head_id) == 0);

    if (sg_tree_build_from_index(git_dir, &idx, tree_id) != 0) {
        fprintf(stderr, "sg: failed to build tree from index\n");
        sg_index_free(&idx);
        free(git_dir);
        free(cleaned_message);
        return 1;
    }
    sg_index_free(&idx);

    has_parent = (sg_ref_resolve_head(git_dir, parent_id) == 0);

    if (is_merge_commit && !has_parent) {
        fprintf(stderr, "sg: 損壞的合併狀態（MERGE_HEAD 存在，但目前分支還沒有任何 commit）\n");
        free(git_dir);
        free(cleaned_message);
        return 1;
    }

    branch = sg_ref_current_branch(git_dir);
    if (branch == NULL) {
        fprintf(stderr, "sg: failed to determine current branch\n");
        free(git_dir);
        free(cleaned_message);
        return 1;
    }

    name = env_or("GIT_AUTHOR_NAME", "small_git");
    email = env_or("GIT_AUTHOR_EMAIL", "sg@localhost");

    memset(&commit, 0, sizeof(commit));
    memcpy(commit.tree, tree_id, SG_SHA1_RAW_LEN);
    if (has_parent) {
        commit.parents = malloc((is_merge_commit ? 2 : 1) * sizeof(*commit.parents));
        if (commit.parents == NULL) {
            fprintf(stderr, "sg: out of memory\n");
            free(branch);
            free(git_dir);
            free(cleaned_message);
            return 1;
        }
        memcpy(commit.parents[0], parent_id, SG_SHA1_RAW_LEN);
        commit.parent_count = 1;
        if (is_merge_commit) {
            memcpy(commit.parents[1], merge_head_id, SG_SHA1_RAW_LEN);
            commit.parent_count = 2;
        }
    }
    commit.author_name = (char *)name;
    commit.author_email = (char *)email;
    commit.author_time = (long long)time(NULL);
    strcpy(commit.author_tz, "+0000");
    commit.committer_name = (char *)name;
    commit.committer_email = (char *)email;
    commit.committer_time = commit.author_time;
    strcpy(commit.committer_tz, "+0000");
    commit.message = (char *)message;

    if (sg_commit_serialize(&commit, &serialized, &serialized_len) != 0) {
        fprintf(stderr, "sg: failed to serialize commit\n");
        free(commit.parents);
        free(branch);
        free(git_dir);
        free(cleaned_message);
        return 1;
    }
    free(commit.parents);

    if (sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len, commit_id) != 0) {
        fprintf(stderr, "sg: failed to write commit object\n");
        free(serialized);
        free(branch);
        free(git_dir);
        free(cleaned_message);
        return 1;
    }
    free(serialized);

    if (sg_ref_update_branch(git_dir, branch, commit_id) != 0) {
        fprintf(stderr, "sg: failed to update branch '%s'\n", branch);
        rc = 1;
    }

    if (rc == 0 && is_merge_commit && sg_merge_head_remove(git_dir) != 0)
        fprintf(stderr, "sg: warning: commit succeeded but failed to remove MERGE_HEAD\n");

    sg_sha1_to_hex(commit_id, commit_hex);
    {
        char short_hex[8];
        const char *first_line_end = strchr(message, '\n');
        size_t first_line_len = first_line_end != NULL ? (size_t)(first_line_end - message)
                                                        : strlen(message);

        memcpy(short_hex, commit_hex, 7);
        short_hex[7] = '\0';
        printf("[%s%s %s] %.*s\n", branch, has_parent ? "" : " (root-commit)", short_hex,
              (int)first_line_len, message);
    }

    free(branch);
    free(git_dir);
    free(cleaned_message);
    return rc;
}
