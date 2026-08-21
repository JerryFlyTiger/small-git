/* Pins `sg status`'s -u<mode>/--untracked-files=<mode> handling (WP3
   follow-up): -uall lists untracked files individually (never folds),
   -unormal (also the flagless default) folds a wholly-untracked directory
   into one "dir/" line, and -uno lists neither untracked nor ignored paths
   at all -- plus the three different closing-line variants -uno prints
   depending on what else is going on in the tree. Goes through the public
   sg_cmd_status entry point and captures stdout, for the same reason
   test_status_unmerged.c does: the option parsing and closing-line logic
   this test pins live in cmd_status.c's static helpers, which cannot be
   linked directly from a separate test TU. */
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
    char template[] = "/tmp/sg_status_umode_test_XXXXXX";
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

/* Runs sg_cmd_status with the given argv, capturing everything it writes to
   stdout into out (NUL-terminated, truncated to out_size - 1 if needed).
   Returns sg_cmd_status's own return code. */
static int run_status_capture(int argc, char **argv, char *out, size_t out_size)
{
    char tmpl[] = "/tmp/sg_status_umode_capture_XXXXXX";
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

/* -uall must list every untracked file individually, never folding a
   wholly-untracked directory into "dir/" -- the opposite of the default. */
static void test_uall_lists_files_individually(void)
{
    char out[8192];
    char *argv[2];
    char *repo_root = make_tmp_repo_and_cd("uall");
    int rc;

    if (mkdir("un", 0755) != 0) {
        fprintf(stderr, "mkdir un failed\n");
        exit(1);
    }
    write_file("un/a.txt", "a\n");
    write_file("un/b.txt", "b\n");

    argv[0] = "status";
    argv[1] = "-uall";
    rc = run_status_capture(2, argv, out, sizeof(out));
    CHECK(rc == 0, "sg status -uall failed");
    CHECK(strstr(out, "un/a.txt") != NULL, "-uall must list un/a.txt individually, got:\n%s", out);
    CHECK(strstr(out, "un/b.txt") != NULL, "-uall must list un/b.txt individually, got:\n%s", out);
    CHECK(strstr(out, "\tun/\n") == NULL, "-uall must NOT fold to 'un/', got:\n%s", out);

    free(repo_root);
}

/* -unormal (and the flagless default) must fold a wholly-untracked
   directory into a single "dir/" line -- pinned here so a future change to
   -uall's implementation cannot silently make -unormal behave like it too. */
static void test_unormal_folds(void)
{
    char out_explicit[8192];
    char out_default[8192];
    char *argv_explicit[2];
    char *argv_default[1];
    char *repo_root = make_tmp_repo_and_cd("unormal");
    int rc;

    if (mkdir("un", 0755) != 0) {
        fprintf(stderr, "mkdir un failed\n");
        exit(1);
    }
    write_file("un/a.txt", "a\n");
    write_file("un/b.txt", "b\n");

    argv_explicit[0] = "status";
    argv_explicit[1] = "-unormal";
    rc = run_status_capture(2, argv_explicit, out_explicit, sizeof(out_explicit));
    CHECK(rc == 0, "sg status -unormal failed");
    CHECK(strstr(out_explicit, "\tun/\n") != NULL, "-unormal must fold to 'un/', got:\n%s",
         out_explicit);
    CHECK(strstr(out_explicit, "un/a.txt") == NULL,
         "-unormal must NOT list un/a.txt individually, got:\n%s", out_explicit);

    argv_default[0] = "status";
    rc = run_status_capture(1, argv_default, out_default, sizeof(out_default));
    CHECK(rc == 0, "sg status (no -u flag) failed");
    CHECK(strcmp(out_default, out_explicit) == 0,
         "the flagless default must match -unormal exactly:\ndefault:\n%s\n-unormal:\n%s",
         out_default, out_explicit);

    free(repo_root);
}

/* -uno must list neither untracked nor ignored paths at all -- not even
   folded. */
static void test_uno_lists_nothing(void)
{
    char out[8192];
    char *argv[3];
    char *repo_root = make_tmp_repo_and_cd("uno");
    int rc;

    write_file(".gitignore", "*.log\n");
    if (mkdir("un", 0755) != 0) {
        fprintf(stderr, "mkdir un failed\n");
        exit(1);
    }
    write_file("un/a.txt", "a\n");
    write_file("ign.log", "ignored\n");

    argv[0] = "status";
    argv[1] = "-uno";
    argv[2] = "--ignored";
    rc = run_status_capture(3, argv, out, sizeof(out));
    CHECK(rc == 0, "sg status -uno --ignored failed");
    CHECK(strstr(out, "Untracked files:") == NULL,
         "-uno must not print an Untracked files: section, got:\n%s", out);
    CHECK(strstr(out, "Ignored files:") == NULL,
         "-uno must not print an Ignored files: section, got:\n%s", out);
    CHECK(strstr(out, "un/") == NULL, "-uno must not mention un/ at all, got:\n%s", out);
    CHECK(strstr(out, "ign.log") == NULL, "-uno must not mention ign.log at all, got:\n%s", out);

    free(repo_root);
}

/* Bare "-u" (no attached mode) behaves like "-uall", not like the flagless
   default -- measured against git 2.55.0. */
static void test_bare_u_equals_uall(void)
{
    char out_bare[8192];
    char out_uall[8192];
    char *argv_bare[2];
    char *argv_uall[2];
    char *repo_root = make_tmp_repo_and_cd("bareu");
    int rc;

    if (mkdir("un", 0755) != 0) {
        fprintf(stderr, "mkdir un failed\n");
        exit(1);
    }
    write_file("un/a.txt", "a\n");

    argv_bare[0] = "status";
    argv_bare[1] = "-u";
    rc = run_status_capture(2, argv_bare, out_bare, sizeof(out_bare));
    CHECK(rc == 0, "sg status -u failed");

    argv_uall[0] = "status";
    argv_uall[1] = "-uall";
    rc = run_status_capture(2, argv_uall, out_uall, sizeof(out_uall));
    CHECK(rc == 0, "sg status -uall failed");

    CHECK(strcmp(out_bare, out_uall) == 0, "bare -u must match -uall exactly:\n-u:\n%s\n-uall:\n%s",
         out_bare, out_uall);
    CHECK(strstr(out_bare, "un/a.txt") != NULL,
         "bare -u must list un/a.txt individually (like -uall), got:\n%s", out_bare);

    free(repo_root);
}

/* A SEPARATE argv token "no" after a bare "-u" must NOT be reinterpreted as
   "-uno" -- sg has no pathspec support, so this must fall through to the
   usage error, not silently become an untracked-files mode. */
static void test_separate_u_no_is_not_uno(void)
{
    char out_split[8192];
    char out_joined[8192];
    char *argv_split[3];
    char *argv_joined[2];
    char *repo_root = make_tmp_repo_and_cd("splitu");
    int rc_split;
    int rc_joined;

    argv_split[0] = "status";
    argv_split[1] = "-u";
    argv_split[2] = "no";
    rc_split = run_status_capture(3, argv_split, out_split, sizeof(out_split));
    CHECK(rc_split == 1, "separate '-u' 'no' must be rejected (usage error), got rc=%d", rc_split);

    argv_joined[0] = "status";
    argv_joined[1] = "-uno";
    rc_joined = run_status_capture(2, argv_joined, out_joined, sizeof(out_joined));
    CHECK(rc_joined == 0, "'-uno' (joined) must be accepted, got rc=%d", rc_joined);

    free(repo_root);
}

/* -uno's three different closing lines, each pinned with its own stdout
   assertion -- this project has shipped a message-formatting bug before
   (Phase 19's "Fast-forwarded (null) to master.") that every non-stdout
   check let straight through, so a new message always gets one of these. */
static void test_uno_closing_line_clean_except_untracked(void)
{
    char out[8192];
    char *argv[2];
    char *repo_root = make_tmp_repo_and_cd("uno_clean");
    int rc;

    write_file("tracked.txt", "content\n");
    run_add("tracked.txt");
    run_commit("init");
    write_file("untracked.txt", "u\n");

    argv[0] = "status";
    argv[1] = "-uno";
    rc = run_status_capture(2, argv, out, sizeof(out));
    CHECK(rc == 0, "sg status -uno failed");
    CHECK(strstr(out, "nothing to commit (use -u to show untracked files)") != NULL,
         "expected the -uno 'clean except untracked' closing line, got:\n%s", out);

    free(repo_root);
}

static void test_uno_closing_line_unstaged_changes(void)
{
    char out[8192];
    char *argv[2];
    char *repo_root = make_tmp_repo_and_cd("uno_unstaged");
    int rc;

    write_file("tracked.txt", "content\n");
    run_add("tracked.txt");
    run_commit("init");
    write_file("tracked.txt", "changed\n");

    argv[0] = "status";
    argv[1] = "-uno";
    rc = run_status_capture(2, argv, out, sizeof(out));
    CHECK(rc == 0, "sg status -uno failed");
    CHECK(strstr(out, "no changes added to commit (use \"git add\" and/or \"git commit -a\")") !=
             NULL,
         "expected the -uno 'unstaged changes present' closing line, got:\n%s", out);

    free(repo_root);
}

static void test_uno_closing_line_staged_changes(void)
{
    char out[8192];
    char *argv[2];
    char *repo_root = make_tmp_repo_and_cd("uno_staged");
    int rc;

    write_file("tracked.txt", "content\n");
    run_add("tracked.txt");
    run_commit("init");
    write_file("tracked.txt", "changed\n");
    run_add("tracked.txt");

    argv[0] = "status";
    argv[1] = "-uno";
    rc = run_status_capture(2, argv, out, sizeof(out));
    CHECK(rc == 0, "sg status -uno failed");
    CHECK(strstr(out, "Untracked files not listed (use -u option to show untracked files)") !=
             NULL,
         "expected the -uno 'staged changes present' extra line, got:\n%s", out);
    CHECK(strstr(out, "Changes to be committed:") != NULL,
         "the normal staged section must still print under -uno, got:\n%s", out);

    free(repo_root);
}

int main(void)
{
    test_uall_lists_files_individually();
    test_unormal_folds();
    test_uno_lists_nothing();
    test_bare_u_equals_uall();
    test_separate_u_no_is_not_uno();
    test_uno_closing_line_clean_except_untracked();
    test_uno_closing_line_unstaged_changes();
    test_uno_closing_line_staged_changes();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all status_untracked_mode tests passed\n");
    return 0;
}
