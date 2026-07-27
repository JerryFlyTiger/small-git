#include "sg/cli.h"

#include "sg/apply.h"
#include "sg/hash.h"
#include "sg/repo.h"
#include "sg/snapshot.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void print_list(const sg_snapshot_list *list)
{
    size_t i;

    if (list->count == 0) {
        printf("目前沒有任何自動快照\n");
        return;
    }

    printf("自動快照（最新在前）：\n");
    for (i = 0; i < list->count; i++) {
        time_t t = (time_t)list->entries[i].timestamp;
        struct tm tmv;
        char timebuf[32];

        localtime_r(&t, &tmv);
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tmv);
        printf("  %zu) %s  %s\n", i + 1, timebuf, list->entries[i].message);
    }
    printf("使用 `sg undo <編號>` 還原\n");
}

int sg_cmd_undo(int argc, char **argv)
{
    static const char usage[] = "usage: sg undo [<N>] [--force|-f]\n";
    char *git_dir;
    sg_snapshot_list list;
    int force = 0;
    const char *num_arg = NULL;
    int i;

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0) {
            force = 1;
        } else if (num_arg == NULL) {
            num_arg = argv[i];
        } else {
            fputs(usage, stderr);
            free(git_dir);
            return 1;
        }
    }

    if (num_arg == NULL) {
        if (sg_snapshot_list_read(git_dir, &list) != 0) {
            fprintf(stderr, "sg: failed to list snapshots\n");
            free(git_dir);
            return 1;
        }
        print_list(&list);
        sg_snapshot_list_free(&list);
        free(git_dir);
        return 0;
    }

    {
        char *repo_root;
        char *end;
        long n;
        unsigned char tree_id[SG_SHA1_RAW_LEN];
        char label[64];
        int apply_rc;
        int rc = 0;

        n = strtol(num_arg, &end, 10);
        if (*end != '\0' || end == num_arg || n <= 0) {
            fprintf(stderr, "sg: invalid snapshot number '%s'\n", num_arg);
            free(git_dir);
            return 1;
        }

        if (sg_snapshot_list_read(git_dir, &list) != 0) {
            fprintf(stderr, "sg: failed to list snapshots\n");
            free(git_dir);
            return 1;
        }

        if ((size_t)n > list.count) {
            fprintf(stderr, "sg: no snapshot #%ld (only %zu available)\n", n, list.count);
            sg_snapshot_list_free(&list);
            free(git_dir);
            return 1;
        }

        if (sg_snapshot_get_tree(git_dir, &list, (size_t)(n - 1), tree_id) != 0) {
            fprintf(stderr, "sg: failed to read snapshot #%ld\n", n);
            sg_snapshot_list_free(&list);
            free(git_dir);
            return 1;
        }
        sg_snapshot_list_free(&list);

        repo_root = sg_repo_root(git_dir);
        if (repo_root == NULL) {
            fprintf(stderr, "sg: failed to determine repository root\n");
            free(git_dir);
            return 1;
        }

        snprintf(label, sizeof(label), "undo (restore snapshot #%ld)", n);
        apply_rc = sg_safe_apply_tree(git_dir, repo_root, tree_id, label, force);
        if (apply_rc == 1) {
            fprintf(stderr, "sg: undo aborted\n");
            rc = 1;
        } else if (apply_rc != 0) {
            rc = 1;
        } else {
            printf("Restored snapshot #%ld\n", n);
        }

        free(repo_root);
        free(git_dir);
        return rc;
    }
}
