/* Boundary tests for the CLI-level reflog messages wired up in Phase 17
   (cmd_commit.c / cmd_branch.c / cmd_switch.c / cmd_reset.c). Exact wording
   for every message is real git's own oracle, checked by tests/interop.sh's
   phase17 section -- these tests instead pin the ASSEMBLY boundaries that
   interop can't cheaply probe: which branch of a conditional a given
   repository shape falls into. */
#include "sg/cli.h"

#include "sg/hash.h"
#include "sg/reflog.h"
#include "sg/refs.h"
#include "sg/repo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    static char template[] = "/tmp/sg_reflog_msgs_test_XXXXXX";
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

    argv[0] = "sg";
    argv[1] = "-m";
    argv[2] = (char *)message;
    CHECK(sg_cmd_commit(3, argv) == 0, "sg commit -m '%s' failed", message);
}

/* Reads the LAST line of a reflog and returns just its message field
   (everything after the last '\t'), malloc'd. Fails the calling CHECK with a
   NULL return if the log is empty or missing -- callers must check. */
static char *last_message(const char *git_dir, const char *ref_path)
{
    sg_reflog log;
    char *msg;

    if (sg_reflog_read(git_dir, ref_path, &log) != 0 || log.count == 0) {
        sg_reflog_free(&log);
        return NULL;
    }
    msg = strdup(log.entries[log.count - 1].message);
    sg_reflog_free(&log);
    return msg;
}

static size_t reflog_count(const char *git_dir, const char *ref_path)
{
    sg_reflog log;
    size_t n;

    if (sg_reflog_read(git_dir, ref_path, &log) != 0)
        return 0;
    n = log.count;
    sg_reflog_free(&log);
    return n;
}

/* ---- commit (initial) fires on "branch has no tip yet", not "repo has no
   commits" -- verified against real git 2.55.0: a SECOND branch, created
   from an already-committed first branch, gets an ordinary "commit:" for
   its own first commit, not "commit (initial):". ---- */
static void test_commit_initial_is_per_branch_not_per_repo(void)
{
    char *repo_root = make_tmp_repo_and_cd();
    char git_dir[4096];
    char *msg;
    char *argv[3];

    snprintf(git_dir, sizeof(git_dir), "%s/.git", repo_root);

    write_file("a.txt", "one\n");
    run_add("a.txt");
    run_commit("first commit");

    msg = last_message(git_dir, "refs/heads/master");
    CHECK(msg != NULL, "expected a reflog entry on refs/heads/master");
    if (msg != NULL)
        CHECK(strcmp(msg, "commit (initial): first commit") == 0,
             "root commit on a brand new branch should log \"commit (initial): ...\", got \"%s\"", msg);
    free(msg);

    /* Now branch off, switch, and commit again -- the new branch already
       has a tip (inherited at `branch` time), so this commit is NOT the
       first thing to happen to a bare/tipless ref. */
    argv[0] = "sg";
    argv[1] = "second";
    CHECK(sg_cmd_branch(2, argv) == 0, "sg branch second failed");
    argv[1] = "second";
    CHECK(sg_cmd_switch(2, argv) == 0, "sg switch second failed");

    write_file("b.txt", "two\n");
    run_add("b.txt");
    run_commit("second commit");

    msg = last_message(git_dir, "refs/heads/second");
    CHECK(msg != NULL, "expected a reflog entry on refs/heads/second");
    if (msg != NULL)
        CHECK(strcmp(msg, "commit: second commit") == 0,
             "a commit on a branch that already had a tip should log \"commit: ...\", not "
             "\"commit (initial): ...\", got \"%s\"",
             msg);
    free(msg);

    free(repo_root);
}

/* ---- `sg branch <n>` logs "Created from <current branch>" (the branch
   active when the new one was created), not "Created from HEAD". ---- */
static void test_branch_created_from_names_the_source_branch(void)
{
    char *repo_root = make_tmp_repo_and_cd();
    char git_dir[4096];
    char *msg;
    char *argv[3];

    snprintf(git_dir, sizeof(git_dir), "%s/.git", repo_root);

    write_file("a.txt", "one\n");
    run_add("a.txt");
    run_commit("first commit");

    argv[0] = "sg";
    argv[1] = "feature";
    CHECK(sg_cmd_branch(2, argv) == 0, "sg branch feature failed");

    msg = last_message(git_dir, "refs/heads/feature");
    CHECK(msg != NULL, "expected a reflog entry on refs/heads/feature");
    if (msg != NULL)
        CHECK(strcmp(msg, "branch: Created from master") == 0,
             "expected \"branch: Created from master\", got \"%s\"", msg);
    free(msg);

    free(repo_root);
}

/* ---- `sg switch -c <n>` logs "Created from HEAD" for the new branch's own
   log (the asymmetric case against plain `sg branch`), and separately logs
   "checkout: moving from <old> to <new>" onto logs/HEAD. ---- */
static void test_switch_create_uses_head_not_branch_name(void)
{
    char *repo_root = make_tmp_repo_and_cd();
    char git_dir[4096];
    char *branch_msg;
    char *head_msg;
    char *argv[3];

    snprintf(git_dir, sizeof(git_dir), "%s/.git", repo_root);

    write_file("a.txt", "one\n");
    run_add("a.txt");
    run_commit("first commit");

    argv[0] = "sg";
    argv[1] = "-c";
    argv[2] = "feature";
    CHECK(sg_cmd_switch(3, argv) == 0, "sg switch -c feature failed");

    branch_msg = last_message(git_dir, "refs/heads/feature");
    CHECK(branch_msg != NULL, "expected a reflog entry on refs/heads/feature");
    if (branch_msg != NULL)
        CHECK(strcmp(branch_msg, "branch: Created from HEAD") == 0,
             "switch -c's new-branch line should say \"branch: Created from HEAD\" (not the branch "
             "name), got \"%s\"",
             branch_msg);
    free(branch_msg);

    head_msg = last_message(git_dir, "HEAD");
    CHECK(head_msg != NULL, "expected a reflog entry on HEAD");
    if (head_msg != NULL)
        CHECK(strcmp(head_msg, "checkout: moving from master to feature") == 0,
             "expected \"checkout: moving from master to feature\", got \"%s\"", head_msg);
    free(head_msg);

    free(repo_root);
}

/* ---- rule 1's asymmetry, exercised end to end: `sg reset` with no args
   (a no-op on the branch it targets) still appends to logs/HEAD, but NOT to
   the branch's own log. Also checks the message carries the literal
   argument text ("HEAD", not a resolved sha). ---- */
static void test_reset_noop_only_logs_to_head(void)
{
    char *repo_root = make_tmp_repo_and_cd();
    char git_dir[4096];
    size_t branch_count_before, branch_count_after;
    char *head_msg;
    char *argv[1];

    snprintf(git_dir, sizeof(git_dir), "%s/.git", repo_root);

    write_file("a.txt", "one\n");
    run_add("a.txt");
    run_commit("first commit");

    branch_count_before = reflog_count(git_dir, "refs/heads/master");
    CHECK(branch_count_before == 1, "expected 1 branch log entry after the initial commit, got %zu",
         branch_count_before);

    argv[0] = "sg";
    CHECK(sg_cmd_reset(1, argv) == 0, "sg reset (no args) failed");

    branch_count_after = reflog_count(git_dir, "refs/heads/master");
    CHECK(branch_count_after == branch_count_before,
         "a no-op reset must NOT append to the branch's own reflog (rule 1): before=%zu after=%zu",
         branch_count_before, branch_count_after);

    head_msg = last_message(git_dir, "HEAD");
    CHECK(head_msg != NULL, "expected a reflog entry on HEAD after a no-op reset");
    if (head_msg != NULL)
        CHECK(strcmp(head_msg, "reset: moving to HEAD") == 0,
             "expected \"reset: moving to HEAD\" (the literal default arg), got \"%s\"", head_msg);
    free(head_msg);

    free(repo_root);
}

/* ---- `sg reset <rev>` records the ORIGINAL argv text, not a resolved
   sha or normalized form. ---- */
static void test_reset_records_original_rev_text(void)
{
    char *repo_root = make_tmp_repo_and_cd();
    char git_dir[4096];
    char *msg;
    char *argv[2];

    snprintf(git_dir, sizeof(git_dir), "%s/.git", repo_root);

    write_file("a.txt", "one\n");
    run_add("a.txt");
    run_commit("first commit");

    write_file("a.txt", "two\n");
    run_add("a.txt");
    run_commit("second commit");

    argv[0] = "sg";
    argv[1] = "HEAD~1";
    CHECK(sg_cmd_reset(2, argv) == 0, "sg reset HEAD~1 failed");

    msg = last_message(git_dir, "refs/heads/master");
    CHECK(msg != NULL, "expected a reflog entry on refs/heads/master");
    if (msg != NULL)
        CHECK(strcmp(msg, "reset: moving to HEAD~1") == 0,
             "expected the literal arg \"HEAD~1\" to appear verbatim, got \"%s\"", msg);
    free(msg);

    free(repo_root);
}

/* ---- `sg reset --soft` writes its own reflog block (cmd_reset.c:138-152),
   a near-verbatim copy of --mixed/--hard's -- interop only exercises
   --mixed and --hard, so this pins that the --soft copy is wired up the
   same way: it appends to the branch's own log with the "reset: moving to
   %s" wording, carrying the literal rev text. ---- */
static void test_reset_soft_logs_to_branch(void)
{
    char *repo_root = make_tmp_repo_and_cd();
    char git_dir[4096];
    char *msg;
    char *argv[3];

    snprintf(git_dir, sizeof(git_dir), "%s/.git", repo_root);

    write_file("a.txt", "one\n");
    run_add("a.txt");
    run_commit("first commit");

    write_file("a.txt", "two\n");
    run_add("a.txt");
    run_commit("second commit");

    argv[0] = "sg";
    argv[1] = "--soft";
    argv[2] = "HEAD~1";
    CHECK(sg_cmd_reset(3, argv) == 0, "sg reset --soft HEAD~1 failed");

    msg = last_message(git_dir, "refs/heads/master");
    CHECK(msg != NULL, "expected a reflog entry on refs/heads/master after --soft");
    if (msg != NULL)
        CHECK(strcmp(msg, "reset: moving to HEAD~1") == 0,
             "expected \"reset: moving to HEAD~1\" from the --soft reflog block, got \"%s\"", msg);
    free(msg);

    free(repo_root);
}

/* ---- `sg switch <branch already checked out>` still appends a line to
   HEAD's own log (measured against real git 2.55.0: it also writes
   "checkout: moving from X to X" in this case). ---- */
static void test_switch_to_current_branch_still_logs(void)
{
    char *repo_root = make_tmp_repo_and_cd();
    char git_dir[4096];
    size_t head_count_before, head_count_after;
    char *head_msg;
    char *argv[2];

    snprintf(git_dir, sizeof(git_dir), "%s/.git", repo_root);

    write_file("a.txt", "one\n");
    run_add("a.txt");
    run_commit("first commit");

    head_count_before = reflog_count(git_dir, "HEAD");

    argv[0] = "sg";
    argv[1] = "master";
    CHECK(sg_cmd_switch(2, argv) == 0, "sg switch master (already current) failed");

    head_count_after = reflog_count(git_dir, "HEAD");
    CHECK(head_count_after == head_count_before + 1,
         "switching to the already-current branch should still append one HEAD reflog line: "
         "before=%zu after=%zu",
         head_count_before, head_count_after);

    head_msg = last_message(git_dir, "HEAD");
    CHECK(head_msg != NULL, "expected a reflog entry on HEAD");
    if (head_msg != NULL)
        CHECK(strcmp(head_msg, "checkout: moving from master to master") == 0,
             "expected \"checkout: moving from master to master\", got \"%s\"", head_msg);
    free(head_msg);

    free(repo_root);
}

int main(void)
{
    test_commit_initial_is_per_branch_not_per_repo();
    test_branch_created_from_names_the_source_branch();
    test_switch_create_uses_head_not_branch_name();
    test_reset_noop_only_logs_to_head();
    test_reset_records_original_rev_text();
    test_reset_soft_logs_to_branch();
    test_switch_to_current_branch_still_logs();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all reflog message tests passed\n");
    return 0;
}
