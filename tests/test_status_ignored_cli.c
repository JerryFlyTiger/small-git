/* Two death zones the review of e1fb5c8 found untested:

   1. print_porcelain_branch_header (cmd_status.c) had zero tests covering
      any of its three output shapes ("## <branch>", "## No commits yet on
      <branch>", "## HEAD (no branch)").

   2. The --ignored CLI integration path (list_diff_sorted, the long-format
      "Ignored files:" section, porcelain's "!!") had zero tests going
      through the actual sg_cmd_status entry point -- and, per the review,
      needs to cover the nested-fold case that Phase 25's collect_ignored_
      within bug (real-git-measured: a subdirectory whose files are all
      ignored, but whose own name matches no directory pattern, must still
      fold to one "subdir/" line, not list every file individually) lived
      in exactly this kind of path.

   Both go through sg_cmd_status and capture stdout via dup2, the same
   pattern test_status_unmerged.c and test_status_untracked_mode.c use --
   the logic pinned here lives in cmd_status.c's static helpers, which
   cannot be linked directly from a separate test TU. */
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

static char *make_tmp_repo_and_cd(const char *tag)
{
    char template[] = "/tmp/sg_status_ign_cli_test_XXXXXX";
    char *path;

    (void)tag;
    path = strdup(template);
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

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");

    if (f == NULL) {
        fprintf(stderr, "failed to write %s\n", path);
        exit(1);
    }
    fputs(content, f);
    fclose(f);
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

static int run_status_capture(int argc, char **argv, char *out, size_t out_size)
{
    char tmpl[] = "/tmp/sg_status_ign_cli_capture_XXXXXX";
    int tmp_fd;
    int saved_stdout;
    off_t len;
    ssize_t n;
    int rc;

    tmp_fd = mkstemp(tmpl);
    if (tmp_fd < 0) {
        fprintf(stderr, "mkstemp failed\n");
        exit(1);
    }
    unlink(tmpl);

    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    dup2(tmp_fd, STDOUT_FILENO);

    rc = sg_cmd_status(argc, argv);

    fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);

    len = lseek(tmp_fd, 0, SEEK_CUR);
    if (len < 0 || (size_t)len >= out_size)
        len = (off_t)out_size - 1;
    lseek(tmp_fd, 0, SEEK_SET);
    n = read(tmp_fd, out, (size_t)len);
    out[n > 0 ? n : 0] = '\0';
    close(tmp_fd);
    return rc;
}

/* ---- print_porcelain_branch_header: three shapes, zero coverage before ---- */

static void test_branch_header_unborn(void)
{
    char out[4096];
    char *argv[3];
    char *repo_root = make_tmp_repo_and_cd("branch_unborn");
    int rc;

    argv[0] = "status";
    argv[1] = "--porcelain";
    argv[2] = "-b";
    rc = run_status_capture(3, argv, out, sizeof(out));
    CHECK(rc == 0, "sg status --porcelain -b failed on an unborn HEAD");
    CHECK(strncmp(out, "## No commits yet on master\n", strlen("## No commits yet on master\n")) ==
             0,
         "expected '## No commits yet on master' on unborn HEAD, got:\n%s", out);

    free(repo_root);
}

static void test_branch_header_normal(void)
{
    char out[4096];
    char *argv[3];
    char *repo_root = make_tmp_repo_and_cd("branch_normal");
    int rc;

    write_file("f.txt", "content\n");
    run_add("f.txt");
    run_commit("init");

    argv[0] = "status";
    argv[1] = "--porcelain";
    argv[2] = "-b";
    rc = run_status_capture(3, argv, out, sizeof(out));
    CHECK(rc == 0, "sg status --porcelain -b failed after a commit");
    CHECK(strncmp(out, "## master\n", strlen("## master\n")) == 0,
         "expected '## master' after a commit, got:\n%s", out);

    free(repo_root);
}

static void test_branch_header_detached(void)
{
    char out[4096];
    char *argv[3];
    char *status_argv[3];
    char *repo_root = make_tmp_repo_and_cd("branch_detached");
    int rc;

    write_file("f.txt", "content\n");
    run_add("f.txt");
    run_commit("init");

    argv[0] = "switch";
    argv[1] = "--detach";
    argv[2] = "master";
    CHECK(sg_cmd_switch(3, argv) == 0, "sg switch --detach master failed");

    status_argv[0] = "status";
    status_argv[1] = "--porcelain";
    status_argv[2] = "-b";
    rc = run_status_capture(3, status_argv, out, sizeof(out));
    CHECK(rc == 0, "sg status --porcelain -b failed on a detached HEAD");
    CHECK(strncmp(out, "## HEAD (no branch)\n", strlen("## HEAD (no branch)\n")) == 0,
         "expected '## HEAD (no branch)' when detached, got:\n%s", out);

    free(repo_root);
}

/* ---- --ignored CLI integration: list_diff_sorted, "Ignored files:",
   porcelain "!!" -- zero coverage before, and must cover the nested-fold
   case the review's bug (collect_ignored_within using a name-only ignore
   check instead of dir_scan_flags) actually lived in. */

/* Flat case: an ignored file sits directly in an otherwise-non-ignored
   folded directory (mirrors the "d/x.tmp" shape already covered at the
   library level by test_status_untracked.c, but this time through the CLI
   entry point in both output formats). */
static void test_ignored_cli_flat(void)
{
    char out_porcelain[4096];
    char out_long[4096];
    char *argv_p[3];
    char *argv_l[2];
    char *repo_root = make_tmp_repo_and_cd("ignored_flat");
    int rc;

    write_file(".gitignore", "*.tmp\n");
    if (mkdir("e", 0755) != 0) {
        fprintf(stderr, "mkdir e failed\n");
        exit(1);
    }
    write_file("e/keep.txt", "keep\n");
    write_file("e/x.tmp", "x\n");

    argv_p[0] = "status";
    argv_p[1] = "--porcelain";
    argv_p[2] = "--ignored";
    rc = run_status_capture(3, argv_p, out_porcelain, sizeof(out_porcelain));
    CHECK(rc == 0, "sg status --porcelain --ignored failed");
    CHECK(strstr(out_porcelain, "?? e/\n") != NULL, "expected folded '?? e/', got:\n%s",
         out_porcelain);
    CHECK(strstr(out_porcelain, "!! e/x.tmp\n") != NULL,
         "expected '!! e/x.tmp' listed individually, got:\n%s", out_porcelain);

    argv_l[0] = "status";
    argv_l[1] = "--ignored";
    rc = run_status_capture(2, argv_l, out_long, sizeof(out_long));
    CHECK(rc == 0, "sg status --ignored failed");
    CHECK(strstr(out_long, "Ignored files:") != NULL,
         "expected an 'Ignored files:' section, got:\n%s", out_long);
    CHECK(strstr(out_long, "e/x.tmp") != NULL, "expected e/x.tmp in the Ignored files section, "
                                               "got:\n%s",
         out_long);

    free(repo_root);
}

/* Nested case A (review-measured against git 2.55.0): a directory (d/)
   holds one non-ignored file plus a subdirectory (subignored/) whose files
   are ALL ignored by a file-pattern rule (not a directory-name rule) --
   subignored/ must fold to one line, not list a.tmp/b.tmp individually. */
static void test_ignored_cli_nested_all_ignored_subdir_folds(void)
{
    char out[4096];
    char *argv[3];
    char *repo_root = make_tmp_repo_and_cd("ignored_nested_a");
    int rc;

    write_file(".gitignore", "*.tmp\n");
    if (mkdir("d", 0755) != 0 || mkdir("d/subignored", 0755) != 0) {
        fprintf(stderr, "mkdir d/subignored failed\n");
        exit(1);
    }
    write_file("d/keep.txt", "keep\n");
    write_file("d/subignored/a.tmp", "a\n");
    write_file("d/subignored/b.tmp", "b\n");

    argv[0] = "status";
    argv[1] = "--porcelain";
    argv[2] = "--ignored";
    rc = run_status_capture(3, argv, out, sizeof(out));
    CHECK(rc == 0, "sg status --porcelain --ignored failed");
    CHECK(strstr(out, "?? d/\n") != NULL, "expected folded '?? d/', got:\n%s", out);
    CHECK(strstr(out, "!! d/subignored/\n") != NULL,
         "expected 'd/subignored/' folded to one ignored line (not name-matched, but "
         "recursively all-ignored), got:\n%s",
         out);
    CHECK(strstr(out, "d/subignored/a.tmp") == NULL,
         "d/subignored/a.tmp must NOT be listed individually once its directory folds, got:\n%s",
         out);
    CHECK(strstr(out, "d/subignored/b.tmp") == NULL,
         "d/subignored/b.tmp must NOT be listed individually once its directory folds, got:\n%s",
         out);

    free(repo_root);
}

/* Nested case B: same tree as case A, but subignored/ ALSO holds one
   non-ignored file -- now it must NOT fold, and its two ignored files must
   be listed individually instead. */
static void test_ignored_cli_nested_mixed_subdir_lists_individually(void)
{
    char out[4096];
    char *argv[3];
    char *repo_root = make_tmp_repo_and_cd("ignored_nested_b");
    int rc;

    write_file(".gitignore", "*.tmp\n");
    if (mkdir("d", 0755) != 0 || mkdir("d/subignored", 0755) != 0) {
        fprintf(stderr, "mkdir d/subignored failed\n");
        exit(1);
    }
    write_file("d/keep.txt", "keep\n");
    write_file("d/subignored/a.tmp", "a\n");
    write_file("d/subignored/b.tmp", "b\n");
    write_file("d/subignored/c.txt", "c\n");

    argv[0] = "status";
    argv[1] = "--porcelain";
    argv[2] = "--ignored";
    rc = run_status_capture(3, argv, out, sizeof(out));
    CHECK(rc == 0, "sg status --porcelain --ignored failed");
    CHECK(strstr(out, "?? d/\n") != NULL, "expected folded '?? d/', got:\n%s", out);
    CHECK(strstr(out, "!! d/subignored/\n") == NULL,
         "d/subignored/ must NOT fold once it has a non-ignored file (c.txt), got:\n%s", out);
    CHECK(strstr(out, "!! d/subignored/a.tmp\n") != NULL,
         "expected d/subignored/a.tmp listed individually, got:\n%s", out);
    CHECK(strstr(out, "!! d/subignored/b.tmp\n") != NULL,
         "expected d/subignored/b.tmp listed individually, got:\n%s", out);

    free(repo_root);
}

int main(void)
{
    test_branch_header_unborn();
    test_branch_header_normal();
    test_branch_header_detached();
    test_ignored_cli_flat();
    test_ignored_cli_nested_all_ignored_subdir_folds();
    test_ignored_cli_nested_mixed_subdir_lists_individually();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all status_ignored_cli tests passed\n");
    return 0;
}
