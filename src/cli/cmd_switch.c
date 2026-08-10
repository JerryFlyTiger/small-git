#include "sg/cli.h"

#include "sg/apply.h"
#include "sg/hash.h"
#include "sg/objstore.h"
#include "sg/object.h"
#include "sg/rebase.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int write_head(const char *git_dir, const char *branch)
{
    char path[4096];
    FILE *f;

    snprintf(path, sizeof(path), "%s/HEAD", git_dir);
    f = fopen(path, "wb");
    if (f == NULL)
        return -1;
    if (fprintf(f, "ref: refs/heads/%s\n", branch) < 0) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

static int resolve_commit_tree(const char *git_dir, const unsigned char commit_id[SG_SHA1_RAW_LEN],
                               unsigned char tree_id_out[SG_SHA1_RAW_LEN])
{
    sg_obj_type type;
    unsigned char *content;
    size_t content_len;
    sg_commit commit;

    if (sg_object_read(git_dir, commit_id, &type, &content, &content_len) != 0 ||
       type != SG_OBJ_COMMIT)
        return -1;
    if (sg_commit_parse(content, content_len, &commit) != 0) {
        free(content);
        return -1;
    }
    free(content);
    memcpy(tree_id_out, commit.tree, SG_SHA1_RAW_LEN);
    sg_commit_free(&commit);
    return 0;
}

int sg_cmd_switch(int argc, char **argv)
{
    int create = 0;
    int force = 0;
    const char *branch_arg = NULL;
    char *git_dir;
    char *repo_root;
    unsigned char target_commit_id[SG_SHA1_RAW_LEN];
    unsigned char target_tree_id[SG_SHA1_RAW_LEN];
    char label[256];
    int apply_rc;
    size_t i;

    for (i = 1; (int)i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            create = 1;
        } else if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0) {
            force = 1;
        } else if (branch_arg == NULL) {
            branch_arg = argv[i];
        } else {
            fprintf(stderr, "usage: sg switch [-c] [--force|-f] <branch>\n");
            return 1;
        }
    }
    if (branch_arg == NULL) {
        fprintf(stderr, "usage: sg switch [-c] [--force|-f] <branch>\n");
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

    /* Measured against real git 2.55.0: `switch` during a paused rebase is
       refused outright, even with --force -- rebase's sequencer state can
       only be ended by rebase's own subcommands (--abort, a completed run,
       --quit). This must run before any side effect, in particular before
       -c below would create a new branch: real git also leaves the new
       branch un-created when the switch is refused. */
    if (sg_rebase_state_exists(git_dir)) {
        fprintf(stderr,
               "sg: 目前有一個進行中的 rebase，無法切換分支\n"
               "請先完成它（sg rebase --continue）或執行 sg rebase --abort 放棄\n");
        free(git_dir);
        free(repo_root);
        return 1;
    }

    /* Only reads/validates so far -- nothing is created yet. A new branch
       must not be written until sg_safe_apply_tree below actually succeeds,
       otherwise a cancelled switch would leave a dangling empty branch. */
    if (create) {
        if (sg_ref_branch_exists(git_dir, branch_arg)) {
            fprintf(stderr, "sg: a branch named '%s' already exists\n", branch_arg);
            free(git_dir);
            free(repo_root);
            return 1;
        }
        if (sg_ref_resolve_head(git_dir, target_commit_id) != 0) {
            fprintf(stderr, "sg: cannot create branch '%s': current branch has no commits yet\n",
                   branch_arg);
            free(git_dir);
            free(repo_root);
            return 1;
        }
    } else {
        if (!sg_ref_branch_exists(git_dir, branch_arg)) {
            fprintf(stderr, "sg: invalid reference: %s\n", branch_arg);
            free(git_dir);
            free(repo_root);
            return 1;
        }
        if (sg_ref_read_branch(git_dir, branch_arg, target_commit_id) != 0) {
            fprintf(stderr, "sg: failed to read branch '%s'\n", branch_arg);
            free(git_dir);
            free(repo_root);
            return 1;
        }
    }

    if (resolve_commit_tree(git_dir, target_commit_id, target_tree_id) != 0) {
        fprintf(stderr, "sg: corrupt commit for branch '%s'\n", branch_arg);
        free(git_dir);
        free(repo_root);
        return 1;
    }

    snprintf(label, sizeof(label), "switch to '%s'", branch_arg);
    apply_rc = sg_safe_apply_tree(git_dir, repo_root, target_tree_id, label, force);
    if (apply_rc == 1) {
        fprintf(stderr, "sg: switch aborted\n");
        free(git_dir);
        free(repo_root);
        return 1;
    }
    if (apply_rc != 0) {
        free(git_dir);
        free(repo_root);
        return 1;
    }

    if (create && sg_ref_update_branch(git_dir, branch_arg, target_commit_id) != 0) {
        fprintf(stderr, "sg: failed to create branch '%s'\n", branch_arg);
        free(git_dir);
        free(repo_root);
        return 1;
    }

    if (write_head(git_dir, branch_arg) != 0) {
        fprintf(stderr, "sg: failed to update HEAD\n");
        free(git_dir);
        free(repo_root);
        return 1;
    }

    printf("Switched to branch '%s'\n", branch_arg);
    free(git_dir);
    free(repo_root);
    return 0;
}
