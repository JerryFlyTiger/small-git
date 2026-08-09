#include "sg/cli.h"

#include "sg/levenshtein.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    const char *desc;
} sg_command_info;

static const sg_command_info COMMANDS[] = {
    {"init", "建立新的 repository"},
    {"hash-object", "計算(並可選擇寫入)物件的雜湊"},
    {"cat-file", "檢視 object 內容/型別/大小"},
    {"add", "將檔案加入暫存區"},
    {"commit", "建立一個 commit"},
    {"log", "顯示 commit 歷史"},
    {"status", "顯示工作目錄狀態"},
    {"diff", "顯示尚未暫存的變更"},
    {"branch", "列出、建立或刪除分支"},
    {"tag", "列出、建立或刪除標籤"},
    {"switch", "切換分支"},
    {"restore", "還原檔案或取消暫存"},
    {"reset", "把目前分支、index（與可選的工作目錄）重設到指定的 commit"},
    {"undo", "列出或還原自動快照"},
    {"repack", "將 loose object 打包成 packfile"},
    {"merge", "合併另一個分支"},
    {"merge-base", "找出兩個 commit 的最近共同祖先"},
    {"rebase", "將目前分支重新套用到另一個分支之上"},
    {"clone", "從遠端複製一個 repository"},
    {"fetch", "從遠端取得新的 commit 與 ref"},
    {"push", "將本地分支推送到遠端"},
    {"chunk-info", "顯示檔案/物件的分塊儲存診斷資訊"},
};
#define COMMANDS_COUNT (sizeof(COMMANDS) / sizeof(COMMANDS[0]))

static void print_help(FILE *out)
{
    size_t i;

    fprintf(out, "usage: sg <command> [<args>]\n\nCommands:\n");
    for (i = 0; i < COMMANDS_COUNT; i++)
        fprintf(out, "  %-13s %s\n", COMMANDS[i].name, COMMANDS[i].desc);
}

int sg_cli_run(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 ||
       strcmp(argv[1], "help") == 0) {
        print_help(stdout);
        return 0;
    }

    /* Phrased like `git version` so scripts that already scrape that shape
       need no special case; SG_VERSION is the single definition shared with
       the man page's .TH line and the packaging targets. */
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0 ||
       strcmp(argv[1], "version") == 0) {
        printf("sg version %s\n", SG_VERSION);
        return 0;
    }

    if (strcmp(argv[1], "init") == 0)
        return sg_cmd_init(argc - 1, argv + 1);
    if (strcmp(argv[1], "hash-object") == 0)
        return sg_cmd_hash_object(argc - 1, argv + 1);
    if (strcmp(argv[1], "cat-file") == 0)
        return sg_cmd_cat_file(argc - 1, argv + 1);
    if (strcmp(argv[1], "add") == 0)
        return sg_cmd_add(argc - 1, argv + 1);
    if (strcmp(argv[1], "commit") == 0)
        return sg_cmd_commit(argc - 1, argv + 1);
    if (strcmp(argv[1], "log") == 0)
        return sg_cmd_log(argc - 1, argv + 1);
    if (strcmp(argv[1], "status") == 0)
        return sg_cmd_status(argc - 1, argv + 1);
    if (strcmp(argv[1], "diff") == 0)
        return sg_cmd_diff(argc - 1, argv + 1);
    if (strcmp(argv[1], "branch") == 0)
        return sg_cmd_branch(argc - 1, argv + 1);
    if (strcmp(argv[1], "tag") == 0)
        return sg_cmd_tag(argc - 1, argv + 1);
    if (strcmp(argv[1], "switch") == 0)
        return sg_cmd_switch(argc - 1, argv + 1);
    if (strcmp(argv[1], "restore") == 0)
        return sg_cmd_restore(argc - 1, argv + 1);
    if (strcmp(argv[1], "reset") == 0)
        return sg_cmd_reset(argc - 1, argv + 1);
    if (strcmp(argv[1], "undo") == 0)
        return sg_cmd_undo(argc - 1, argv + 1);
    if (strcmp(argv[1], "repack") == 0)
        return sg_cmd_repack(argc - 1, argv + 1);
    if (strcmp(argv[1], "merge-base") == 0)
        return sg_cmd_merge_base(argc - 1, argv + 1);
    if (strcmp(argv[1], "merge") == 0)
        return sg_cmd_merge(argc - 1, argv + 1);
    if (strcmp(argv[1], "rebase") == 0)
        return sg_cmd_rebase(argc - 1, argv + 1);
    if (strcmp(argv[1], "clone") == 0)
        return sg_cmd_clone(argc - 1, argv + 1);
    if (strcmp(argv[1], "fetch") == 0)
        return sg_cmd_fetch(argc - 1, argv + 1);
    if (strcmp(argv[1], "push") == 0)
        return sg_cmd_push(argc - 1, argv + 1);
    if (strcmp(argv[1], "chunk-info") == 0)
        return sg_cmd_chunk_info(argc - 1, argv + 1);

    {
        size_t i;
        size_t best_idx = 0;
        size_t best_dist = (size_t)-1;

        for (i = 0; i < COMMANDS_COUNT; i++) {
            size_t dist = sg_levenshtein(argv[1], COMMANDS[i].name);

            if (dist < best_dist) {
                best_dist = dist;
                best_idx = i;
            }
        }

        fprintf(stderr, "sg: '%s' is not a sg command\n", argv[1]);
        if (best_dist <= 2) {
            fprintf(stderr, "\n你是不是想輸入 '%s'?\n", COMMANDS[best_idx].name);
        } else {
            fprintf(stderr, "\n");
            print_help(stderr);
        }
    }
    return 1;
}
