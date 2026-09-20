/* Phase 81c: restore_worktree (cmd_restore.c, static) had no unit test at
   all before this phase, and this phase changed how it passes mode to
   sg_write_file_worktree (stopped masking with & 0777). Goes through the
   public sg_cmd_add/sg_cmd_commit/sg_cmd_restore entry points and inspects
   the working tree directly -- restore_worktree's own logic cannot be
   linked from a separate test TU (static). */
#include "sg/cli.h"

#include "sg/repo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, ...)                                                                        \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);                                 \
            fprintf(stderr, __VA_ARGS__);                                                        \
            fprintf(stderr, "\n");                                                               \
            failures++;                                                                          \
        }                                                                                         \
    } while (0)

static char *make_tmp_repo_and_cd(void)
{
    char template[] = "/tmp/sg_restore_symlink_test_XXXXXX";
    char *path = strdup(template);

    if (mkdtemp(path) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        exit(1);
    }
    if (sg_repo_init(path) != 0) {
        fprintf(stderr, "sg_repo_init failed\n");
        exit(1);
    }
    if (chdir(path) != 0) {
        fprintf(stderr, "chdir failed\n");
        exit(1);
    }
    return path;
}

static void run_add(const char *path)
{
    char *argv[2];

    argv[0] = "add";
    argv[1] = (char *)path;
    CHECK(sg_cmd_add(2, argv) == 0, "sg add %s failed", path);
}

static void run_commit(const char *message)
{
    char *argv[3];

    argv[0] = "commit";
    argv[1] = "-m";
    argv[2] = (char *)message;
    CHECK(sg_cmd_commit(3, argv) == 0, "sg commit -m '%s' failed", message);
}

static int is_symlink(const char *path)
{
    struct stat st;

    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

/* `sg restore lf` after the tracked symlink was deleted must recreate a
   REAL symlink, not a mode-000 regular file containing the target text
   (ORACLE.md c3). */
static void test_restore_deleted_symlink(void)
{
    char *root = make_tmp_repo_and_cd();
    char target[64];

    CHECK(symlink("f.txt", "lf") == 0, "failed to create lf -> f.txt");
    run_add("lf");
    run_commit("add symlink");

    CHECK(unlink("lf") == 0, "failed to delete lf before restore");

    {
        char *argv[2];

        argv[0] = "restore";
        argv[1] = "lf";
        CHECK(sg_cmd_restore(2, argv) == 0, "sg restore lf failed");
    }

    CHECK(is_symlink("lf"), "lf must be restored as a real symlink");
    CHECK(readlink("lf", target, sizeof(target)) == 5 && memcmp(target, "f.txt", 5) == 0,
         "restored symlink target is wrong");

    free(root);
}

/* `sg restore --force lf` where lf is currently a regular file must rewrite
   it as a real symlink (ORACLE.md c15 / control B). */
static void test_restore_force_over_regular_file(void)
{
    char *root = make_tmp_repo_and_cd();
    FILE *f;
    char target[64];

    CHECK(symlink("f.txt", "lf") == 0, "failed to create lf -> f.txt");
    run_add("lf");
    run_commit("add symlink");

    CHECK(unlink("lf") == 0, "failed to remove lf before replacing with a regular file");
    f = fopen("lf", "wb");
    CHECK(f != NULL, "failed to create regular lf");
    if (f != NULL) {
        fputs("now a file\n", f);
        fclose(f);
    }

    {
        char *argv[3];

        argv[0] = "restore";
        argv[1] = "--force";
        argv[2] = "lf";
        CHECK(sg_cmd_restore(3, argv) == 0, "sg restore --force lf failed");
    }

    CHECK(is_symlink("lf"), "lf must be rewritten as a real symlink");
    CHECK(readlink("lf", target, sizeof(target)) == 5 && memcmp(target, "f.txt", 5) == 0,
         "restored symlink target is wrong");

    free(root);
}

int main(void)
{
    test_restore_deleted_symlink();
    test_restore_force_over_regular_file();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all restore_symlink tests passed\n");
    return 0;
}
