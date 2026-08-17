#include "sg/cli.h"

#include "sg/apply.h"
#include "sg/hash.h"
#include "sg/index.h"
#include "sg/merge.h"
#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/rebase.h"
#include "sg/repo.h"
#include "sg/stash.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Writes the "Dropped ..." line for pop and drop. Real git echoes the user's
   spec back only when it already reads as stash@{N}; a bare "0" or no
   argument at all resolves to the fully-qualified ref instead. Measured on
   2.55.0:

     git stash drop            -> Dropped refs/stash@{0} (...)
     git stash drop stash@{0}  -> Dropped stash@{0} (...)
     git stash drop 0          -> Dropped refs/stash@{0} (...)
     git stash pop             -> Dropped refs/stash@{0} (...)
     git stash pop stash@{0}   -> Dropped stash@{0} (...)

   So this is not a pop-versus-drop distinction, which is how it first looked
   when only the no-argument pop and the explicit-spec drop had been sampled.
   Both subcommands share the one rule below. */
static void print_dropped(const char *spec, size_t index, const char *hex)
{
    if (spec != NULL && strncmp(spec, "stash@{", 7) == 0)
        printf("Dropped %s (%s)\n", spec, hex);
    else
        printf("Dropped refs/stash@{%zu} (%s)\n", index, hex);
}

static int cmd_stash_push(int argc, char **argv, const char *usage)
{
    sg_stash_push_opts opts;
    int i0 = 1;
    int i;
    char *git_dir;
    char *repo_root;
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    int rc;

    memset(&opts, 0, sizeof(opts));

    if (argc >= 2 && strcmp(argv[1], "push") == 0)
        i0 = 2;

    for (i = i0; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0) {
            if (i + 1 >= argc) {
                fputs(usage, stderr);
                return 1;
            }
            opts.message = argv[++i];
        } else if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--include-untracked") == 0) {
            opts.include_untracked = 1;
        } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--all") == 0) {
            opts.include_ignored = 1;
        } else if (strcmp(argv[i], "--keep-index") == 0) {
            opts.keep_index = 1;
        } else {
            fputs(usage, stderr);
            return 1;
        }
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

    /* sg_stash_push refuses an unmerged index, but it is a library function
       and reports that as a bare -1 alongside every other failure. Ask the
       question here so the answer can be stated instead of guessed: a user
       sitting in a conflicted merge should be told what is wrong, not handed
       the fallback below with a question mark on the end. */
    {
        sg_index idx;

        if (sg_index_read(git_dir, &idx) == 0) {
            int unmerged = sg_index_has_unmerged(&idx);

            sg_index_free(&idx);
            if (unmerged) {
                fprintf(stderr, "sg: 尚有未解決的衝突，無法 stash push\n");
                free(git_dir);
                free(repo_root);
                return 1;
            }
        }
    }

    /* Real git behaves identically here (measured against git 2.55.0): a
       stash push while a rebase is paused resets the working tree and index
       back to HEAD, which can make `sg rebase --continue` decide the paused
       commit's change is already upstream and silently skip it. sg does not
       change that behavior -- it matches git -- but it does not warn about
       it either, so say so. The rebase sequencer state itself is untouched
       by this (see sg_stash_push's header comment). */
    if (sg_rebase_state_exists(git_dir)) {
        fprintf(stderr, "sg: 目前有一個進行中的 rebase；這次 stash push 會把工作目錄與 index 重設回 "
                        "HEAD，之後 `sg rebase --continue` 可能因此略過目前這個 commit\n");
    }

    rc = sg_stash_push(git_dir, repo_root, &opts, commit_id);
    if (rc == 1) {
        printf("No local changes to save\n");
        free(git_dir);
        free(repo_root);
        return 0;
    }
    if (rc == -2) {
        /* The stash commit + refs/stash were already written durably (it IS
           on `sg stash list`) -- only the snapshot or the working-tree reset
           back to HEAD failed after that. Saying "無法建立 stash" here would
           be a lie: the entry exists, only its cleanup step is in doubt. */
        sg_stash_list list;

        fprintf(stderr, "sg: stash 已建立，但後續步驟失敗（快照或還原工作目錄回 HEAD）；"
                        "請自行確認工作目錄狀態\n");
        if (sg_stash_list_read(git_dir, &list) == 0 && list.count > 0) {
            fprintf(stderr, "sg: 該 stash 為 stash@{0}: %s\n", list.entries[0].message);
            sg_stash_list_free(&list);
        }
        free(git_dir);
        free(repo_root);
        return 1;
    }
    if (rc != 0) {
        fprintf(stderr, "sg: 無法建立 stash（未初始化的 HEAD，或 index 有未解決的衝突？）\n");
        free(git_dir);
        free(repo_root);
        return 1;
    }

    /* MERGE_HEAD cleanup is deliberately the CLI layer's job -- see the
       sg_stash_push header comment in sg/stash.h. */
    {
        if (sg_merge_head_exists(git_dir)) {
            if (sg_merge_head_remove(git_dir) != 0)
                fprintf(stderr, "sg: warning: stash 成功，但清除 MERGE_HEAD 失敗\n");
            else
                fprintf(stderr, "sg: 已清除進行中的合併狀態（MERGE_HEAD）\n");
        }
    }

    {
        sg_stash_list list;

        if (sg_stash_list_read(git_dir, &list) == 0 && list.count > 0) {
            printf("Saved working directory and index state %s\n", list.entries[0].message);
            sg_stash_list_free(&list);
        } else {
            char hex[SG_SHA1_HEX_LEN + 1];

            sg_sha1_to_hex(commit_id, hex);
            printf("Saved working directory and index state %s\n", hex);
        }
    }

    free(git_dir);
    free(repo_root);
    return 0;
}

static int cmd_stash_list(int argc, char **argv)
{
    char *git_dir;
    sg_stash_list list;
    size_t i;

    (void)argv;
    if (argc != 2) {
        fputs("usage: sg stash list\n", stderr);
        return 1;
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;

    if (sg_stash_list_read(git_dir, &list) != 0) {
        fprintf(stderr, "sg: failed to read stash list（reflog 損壞？）\n");
        free(git_dir);
        return 1;
    }

    for (i = 0; i < list.count; i++)
        printf("stash@{%zu}: %s\n", i, list.entries[i].message);

    sg_stash_list_free(&list);
    free(git_dir);
    return 0;
}

static int cmd_stash_clear(int argc, char **argv)
{
    char *git_dir;

    (void)argv;
    if (argc != 2) {
        fputs("usage: sg stash clear\n", stderr);
        return 1;
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;

    if (sg_stash_clear(git_dir) != 0) {
        fprintf(stderr, "sg: failed to clear stash\n");
        free(git_dir);
        return 1;
    }

    free(git_dir);
    return 0;
}

static int cmd_stash_drop(int argc, char **argv)
{
    static const char usage[] = "usage: sg stash drop [<stash>]\n";
    const char *spec = NULL;
    char *git_dir;
    size_t index;
    sg_stash_list list;
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];

    if (argc == 3)
        spec = argv[2];
    else if (argc != 2) {
        fputs(usage, stderr);
        return 1;
    }

    if (sg_stash_parse_spec(spec, &index) != 0) {
        fprintf(stderr, "sg: invalid stash spec: %s\n", spec != NULL ? spec : "");
        return 1;
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;

    if (sg_stash_list_read(git_dir, &list) != 0) {
        fprintf(stderr, "sg: failed to read stash list\n");
        free(git_dir);
        return 1;
    }
    if (index >= list.count) {
        fprintf(stderr, "sg: %s: log for 'stash' only has %zu entries\n",
               spec != NULL ? spec : "stash@{0}", list.count);
        sg_stash_list_free(&list);
        free(git_dir);
        return 1;
    }
    memcpy(commit_id, list.entries[index].commit_id, SG_SHA1_RAW_LEN);
    sg_stash_list_free(&list);

    if (sg_stash_drop(git_dir, index) != 0) {
        fprintf(stderr, "sg: failed to drop stash@{%zu}\n", index);
        free(git_dir);
        return 1;
    }

    sg_sha1_to_hex(commit_id, hex);
    print_dropped(spec, index, hex);

    free(git_dir);
    return 0;
}

static int cmd_stash_apply_or_pop(int argc, char **argv, int is_pop)
{
    const char *cmd_name = is_pop ? "pop" : "apply";
    char usage[64];
    const char *spec = NULL;
    char *git_dir;
    char *repo_root;
    sg_index idx;
    size_t index;
    sg_stash_list list;
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    sg_obj_type type;
    unsigned char *content = NULL;
    size_t content_len;
    sg_commit commit;
    int rc;

    snprintf(usage, sizeof(usage), "usage: sg stash %s [<stash>]\n", cmd_name);

    if (argc == 3)
        spec = argv[2];
    else if (argc != 2) {
        fputs(usage, stderr);
        return 1;
    }

    if (sg_stash_parse_spec(spec, &index) != 0) {
        fprintf(stderr, "sg: invalid stash spec: %s\n", spec != NULL ? spec : "");
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
    if (sg_index_has_unmerged(&idx)) {
        fprintf(stderr, "sg: 尚有未解決的衝突，無法 stash %s\n", cmd_name);
        sg_index_free(&idx);
        free(git_dir);
        free(repo_root);
        return 1;
    }
    sg_index_free(&idx);

    {
        char what[64];

        snprintf(what, sizeof(what), "sg stash %s", cmd_name);
        if (sg_require_clean_workdir(git_dir, repo_root, what) != 0) {
            free(git_dir);
            free(repo_root);
            return 1;
        }
    }

    /* Deliberate divergence from real git (which allows this): pop/apply
       during a paused rebase is refused, consistent with switch/merge, and
       leaves the sequencer state untouched. See CLAUDE.md's rebase-gate
       rule and sg/stash.h's sg_stash_apply header comment. */
    if (sg_rebase_state_exists(git_dir)) {
        fprintf(stderr,
               "sg: 目前有一個進行中的 rebase，無法 stash %s\n"
               "請先完成它（sg rebase --continue）或執行 sg rebase --abort 放棄\n",
               cmd_name);
        free(git_dir);
        free(repo_root);
        return 1;
    }

    if (sg_stash_list_read(git_dir, &list) != 0) {
        fprintf(stderr, "sg: failed to read stash list\n");
        free(git_dir);
        free(repo_root);
        return 1;
    }
    if (index >= list.count) {
        fprintf(stderr, "sg: %s: log for 'stash' only has %zu entries\n",
               spec != NULL ? spec : "stash@{0}", list.count);
        sg_stash_list_free(&list);
        free(git_dir);
        free(repo_root);
        return 1;
    }
    memcpy(commit_id, list.entries[index].commit_id, SG_SHA1_RAW_LEN);
    sg_stash_list_free(&list);

    if (sg_object_read(git_dir, commit_id, &type, &content, &content_len) != 0 || type != SG_OBJ_COMMIT) {
        fprintf(stderr, "sg: stash@{%zu} 已損壞（不是有效的 commit）\n", index);
        free(content);
        free(git_dir);
        free(repo_root);
        return 1;
    }
    if (sg_commit_parse(content, content_len, &commit) != 0) {
        fprintf(stderr, "sg: stash@{%zu} 已損壞（無法解析 commit）\n", index);
        free(content);
        free(git_dir);
        free(repo_root);
        return 1;
    }
    free(content);
    if (commit.parent_count < 2 || commit.parent_count > 3) {
        fprintf(stderr, "sg: stash@{%zu}: not a stash-like commit\n", index);
        sg_commit_free(&commit);
        free(git_dir);
        free(repo_root);
        return 1;
    }
    sg_commit_free(&commit);

    rc = sg_stash_apply(git_dir, repo_root, index);
    if (rc == 1) {
        fprintf(stderr, "自動合併失敗，工作目錄與 index 留下衝突標記（Updated upstream / Stashed "
                        "changes）：\n"
                        "請編輯衝突檔案並執行 `sg add <file>...` 標記為已解決；stash 本身沒有被丟棄\n");
        free(git_dir);
        free(repo_root);
        return 1;
    }
    if (rc != 0) {
        fprintf(stderr, "sg: stash %s 失敗\n", cmd_name);
        free(git_dir);
        free(repo_root);
        return 1;
    }

    /* Real git prints the working-tree status (identical to `git status`)
       after a clean apply or pop (measured `git stash apply`/`git stash
       pop` against git 2.55.0) -- sg previously printed nothing at all on a
       clean apply. Reuse sg_cmd_status wholesale rather than duplicating its
       formatting. */
    {
        char *status_argv[1];

        status_argv[0] = (char *)"status";
        sg_cmd_status(1, status_argv);
    }

    if (is_pop) {
        char hex[SG_SHA1_HEX_LEN + 1];

        sg_sha1_to_hex(commit_id, hex);
        if (sg_stash_drop(git_dir, index) != 0) {
            fprintf(stderr, "sg: 套用成功，但刪除 stash@{%zu} 失敗\n", index);
            free(git_dir);
            free(repo_root);
            return 1;
        }
        print_dropped(spec, index, hex);
    }

    free(git_dir);
    free(repo_root);
    return 0;
}

int sg_cmd_stash(int argc, char **argv)
{
    static const char usage[] = "usage: sg stash [push] [-m <msg>] [-u|--include-untracked] [-a|--all] "
                                "[--keep-index]\n"
                                "   or: sg stash list\n"
                                "   or: sg stash apply [<stash>]\n"
                                "   or: sg stash pop [<stash>]\n"
                                "   or: sg stash drop [<stash>]\n"
                                "   or: sg stash clear\n";

    if (argc >= 2 && strcmp(argv[1], "list") == 0)
        return cmd_stash_list(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "clear") == 0)
        return cmd_stash_clear(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "apply") == 0)
        return cmd_stash_apply_or_pop(argc, argv, 0);
    if (argc >= 2 && strcmp(argv[1], "pop") == 0)
        return cmd_stash_apply_or_pop(argc, argv, 1);
    if (argc >= 2 && strcmp(argv[1], "drop") == 0)
        return cmd_stash_drop(argc, argv);

    return cmd_stash_push(argc, argv, usage);
}
