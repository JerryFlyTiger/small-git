#include "sg/apply.h"

#include "sg/index.h"
#include "sg/repo.h"
#include "sg/workdir.h"

#include <errno.h>
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

static char *make_tmp_repo(void)
{
    static char template[] = "/tmp/sg_untracked_overwrite_test_XXXXXX";
    char *path = strdup(template);
    char git_dir[4096];

    if (mkdtemp(path) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        exit(1);
    }
    if (sg_repo_init(path) != 0) {
        fprintf(stderr, "sg_repo_init failed\n");
        exit(1);
    }
    snprintf(git_dir, sizeof(git_dir), "%s/.git", path);
    free(path);
    return strdup(git_dir);
}

static void write_workdir_file(const char *repo_root, const char *rel, const char *content)
{
    char abspath[4096];

    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, rel);
    CHECK(sg_write_file_mkdirs(abspath, (const unsigned char *)content, strlen(content), 0644) == 0,
         "failed to write workdir file %s", rel);
}

static void mkdir_workdir(const char *repo_root, const char *rel)
{
    char abspath[4096];

    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, rel);
    CHECK(mkdir(abspath, 0755) == 0, "failed to mkdir %s", rel);
}

static void free_result(char **out, size_t out_count)
{
    size_t i;

    for (i = 0; i < out_count; i++)
        free(out[i]);
    free(out);
}

/* An untracked FILE sitting at an ancestor of the candidate path blocks it,
   and is named itself -- not the deeper candidate path. */
static void test_ancestor_blocker(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    const char *paths[1];
    char **out_files = NULL;
    size_t out_files_count = 0;
    char **out_dirs = NULL;
    size_t out_dirs_count = 0;

    memset(&idx, 0, sizeof(idx));
    write_workdir_file(repo_root, "dir", "not a directory\n");

    paths[0] = "dir/deep.txt";
    CHECK(sg_untracked_would_be_overwritten(git_dir, repo_root, &idx, paths, 1, &out_files,
                                            &out_files_count, &out_dirs, &out_dirs_count,
                                            NULL, NULL) == 0,
         "call failed");
    CHECK(out_files_count == 1, "expected exactly one file collision, got %zu", out_files_count);
    CHECK(out_dirs_count == 0, "expected no directory collision, got %zu", out_dirs_count);
    if (out_files_count == 1)
        CHECK(strcmp(out_files[0], "dir") == 0, "expected blocker 'dir', got '%s'", out_files[0]);

    free_result(out_files, out_files_count);
    free_result(out_dirs, out_dirs_count);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* Two candidates blocked by the SAME ancestor each report that ancestor
   separately, in candidate order -- measured against git 2.55.0 (see
   docs/RULES-merge.md's Phase 79 cold-read correction): git does NOT
   de-duplicate, so this must produce the blocker's name TWICE, not once. */
static void test_no_dedup_same_blocker(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    const char *paths[2];
    char **out_files = NULL;
    size_t out_files_count = 0;
    char **out_dirs = NULL;
    size_t out_dirs_count = 0;

    memset(&idx, 0, sizeof(idx));
    write_workdir_file(repo_root, "a", "not a directory\n");

    paths[0] = "a/b.txt";
    paths[1] = "a/c.txt";
    CHECK(sg_untracked_would_be_overwritten(git_dir, repo_root, &idx, paths, 2, &out_files,
                                            &out_files_count, &out_dirs, &out_dirs_count,
                                            NULL, NULL) == 0,
         "call failed");
    CHECK(out_files_count == 2, "expected two collisions (one per candidate, not de-duplicated), "
                                "got %zu",
         out_files_count);
    CHECK(out_dirs_count == 0, "expected no directory collision, got %zu", out_dirs_count);
    if (out_files_count == 2) {
        CHECK(strcmp(out_files[0], "a") == 0, "expected blocker 'a' (1st, for a/b.txt), got '%s'",
             out_files[0]);
        CHECK(strcmp(out_files[1], "a") == 0, "expected blocker 'a' (2nd, for a/c.txt), got '%s'",
             out_files[1]);
    }

    free_result(out_files, out_files_count);
    free_result(out_dirs, out_dirs_count);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* An untracked file the .gitignore rules cover is not reported -- git
   overwrites an ignored file without a word. */
static void test_ignored_not_reported(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    const char *paths[1];
    char **out_files = NULL;
    size_t out_files_count = 0;
    char **out_dirs = NULL;
    size_t out_dirs_count = 0;

    memset(&idx, 0, sizeof(idx));
    write_workdir_file(repo_root, ".gitignore", "ignored.txt\n");
    write_workdir_file(repo_root, "ignored.txt", "user data\n");

    paths[0] = "ignored.txt";
    CHECK(sg_untracked_would_be_overwritten(git_dir, repo_root, &idx, paths, 1, &out_files,
                                            &out_files_count, &out_dirs, &out_dirs_count,
                                            NULL, NULL) == 0,
         "call failed");
    CHECK(out_files_count == 0, "an ignored file must not be reported, got %zu", out_files_count);
    CHECK(out_dirs_count == 0, "expected no directory collision, got %zu", out_dirs_count);

    free_result(out_files, out_files_count);
    free_result(out_dirs, out_dirs_count);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* A path already tracked in the index is never an "untracked collision",
   regardless of what actually sits on disk at that path. */
static void test_tracked_not_reported(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    sg_index_entry e;
    const char *paths[1];
    char **out_files = NULL;
    size_t out_files_count = 0;
    char **out_dirs = NULL;
    size_t out_dirs_count = 0;

    memset(&idx, 0, sizeof(idx));
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    e.path = (char *)"tracked.txt";
    CHECK(sg_index_upsert(&idx, &e) == 0, "upsert failed");
    write_workdir_file(repo_root, "tracked.txt", "on disk\n");

    paths[0] = "tracked.txt";
    CHECK(sg_untracked_would_be_overwritten(git_dir, repo_root, &idx, paths, 1, &out_files,
                                            &out_files_count, &out_dirs, &out_dirs_count,
                                            NULL, NULL) == 0,
         "call failed");
    CHECK(out_files_count == 0, "a tracked path must not be reported, got %zu", out_files_count);
    CHECK(out_dirs_count == 0, "expected no directory collision, got %zu", out_dirs_count);

    free_result(out_files, out_files_count);
    free_result(out_dirs, out_dirs_count);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* A path too long to join with repo_root fails closed: reported as a
   collision rather than silently read as "clear". */
static void test_truncation_fails_closed(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    char long_path[5000];
    const char *paths[1];
    char **out_files = NULL;
    size_t out_files_count = 0;
    char **out_dirs = NULL;
    size_t out_dirs_count = 0;
    size_t i;

    memset(&idx, 0, sizeof(idx));
    for (i = 0; i < sizeof(long_path) - 1; i++)
        long_path[i] = 'x';
    long_path[sizeof(long_path) - 1] = '\0';

    paths[0] = long_path;
    CHECK(sg_untracked_would_be_overwritten(git_dir, repo_root, &idx, paths, 1, &out_files,
                                            &out_files_count, &out_dirs, &out_dirs_count,
                                            NULL, NULL) == 0,
         "call failed");
    CHECK(out_files_count == 1, "a truncated path must fail closed (be reported), got %zu",
         out_files_count);
    CHECK(out_dirs_count == 0, "expected no directory collision, got %zu", out_dirs_count);

    free_result(out_files, out_files_count);
    free_result(out_dirs, out_dirs_count);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* An untracked EMPTY directory sitting exactly at the candidate path does
   not block it -- git silently removes it. A NON-EMPTY one, holding at
   least one non-ignored file, is reported through the DIRECTORY bucket
   (Phase 79b), not the file one. */
static void test_empty_directory(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    const char *paths[1];
    char **out_files = NULL;
    size_t out_files_count = 0;
    char **out_dirs = NULL;
    size_t out_dirs_count = 0;

    memset(&idx, 0, sizeof(idx));
    mkdir_workdir(repo_root, "empty_dir");

    paths[0] = "empty_dir";
    CHECK(sg_untracked_would_be_overwritten(git_dir, repo_root, &idx, paths, 1, &out_files,
                                            &out_files_count, &out_dirs, &out_dirs_count,
                                            NULL, NULL) == 0,
         "call failed");
    CHECK(out_files_count == 0, "an empty untracked directory must not be reported, got %zu",
         out_files_count);
    CHECK(out_dirs_count == 0, "an empty untracked directory must not be reported, got %zu",
         out_dirs_count);
    free_result(out_files, out_files_count);
    free_result(out_dirs, out_dirs_count);

    write_workdir_file(repo_root, "nonempty_dir/leftover.txt", "user data\n");
    paths[0] = "nonempty_dir";
    out_files = NULL;
    out_files_count = 0;
    out_dirs = NULL;
    out_dirs_count = 0;
    CHECK(sg_untracked_would_be_overwritten(git_dir, repo_root, &idx, paths, 1, &out_files,
                                            &out_files_count, &out_dirs, &out_dirs_count,
                                            NULL, NULL) == 0,
         "call failed");
    CHECK(out_files_count == 0, "must be reported in the DIRECTORY bucket, not the file one, got "
                                "%zu file collisions",
         out_files_count);
    CHECK(out_dirs_count == 1, "a non-empty untracked directory must be reported, got %zu",
         out_dirs_count);
    if (out_dirs_count == 1)
        CHECK(strcmp(out_dirs[0], "nonempty_dir") == 0, "expected 'nonempty_dir', got '%s'",
             out_dirs[0]);

    free_result(out_files, out_files_count);
    free_result(out_dirs, out_dirs_count);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* Phase 79b: a non-empty untracked directory whose entire contents (at any
   depth) are ignored is NOT a collision -- measured, git merges through it
   cleanly, the same "ignored beats blocking" rule as a plain ignored file
   already gets. */
static void test_directory_all_contents_ignored(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    const char *paths[1];
    char **out_files = NULL;
    size_t out_files_count = 0;
    char **out_dirs = NULL;
    size_t out_dirs_count = 0;

    memset(&idx, 0, sizeof(idx));
    write_workdir_file(repo_root, ".gitignore", "dir/\n");
    write_workdir_file(repo_root, "dir/leaf.txt", "user data\n");
    write_workdir_file(repo_root, "dir/sub/deep.txt", "user data\n");

    paths[0] = "dir";
    CHECK(sg_untracked_would_be_overwritten(git_dir, repo_root, &idx, paths, 1, &out_files,
                                            &out_files_count, &out_dirs, &out_dirs_count,
                                            NULL, NULL) == 0,
         "call failed");
    CHECK(out_files_count == 0, "expected no file collision, got %zu", out_files_count);
    CHECK(out_dirs_count == 0, "a directory whose own pattern ignores it entirely must not be "
                               "reported, got %zu",
         out_dirs_count);

    free_result(out_files, out_files_count);
    free_result(out_dirs, out_dirs_count);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* Same as above, but the DIRECTORY itself is not matched by any pattern --
   only its individual files are, at two different depths. This is the one
   that actually exercises the recursive scan rather than a single
   is_ignored(dir) check: "dir/" above matches the directory's OWN name, but
   here nothing matches "dir" itself, only "dir/leaf.txt" and
   "dir/sub/deep.txt" via a "*.txt" pattern. */
static void test_directory_contents_ignored_by_pattern(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    const char *paths[1];
    char **out_files = NULL;
    size_t out_files_count = 0;
    char **out_dirs = NULL;
    size_t out_dirs_count = 0;

    memset(&idx, 0, sizeof(idx));
    write_workdir_file(repo_root, ".gitignore", "*.txt\n");
    write_workdir_file(repo_root, "dir/leaf.txt", "user data\n");
    write_workdir_file(repo_root, "dir/sub/deep.txt", "user data\n");

    paths[0] = "dir";
    CHECK(sg_untracked_would_be_overwritten(git_dir, repo_root, &idx, paths, 1, &out_files,
                                            &out_files_count, &out_dirs, &out_dirs_count,
                                            NULL, NULL) == 0,
         "call failed");
    CHECK(out_files_count == 0, "expected no file collision, got %zu", out_files_count);
    CHECK(out_dirs_count == 0,
         "a directory whose contents are ALL ignored (even nested) must not be reported, got %zu",
         out_dirs_count);

    free_result(out_files, out_files_count);
    free_result(out_dirs, out_dirs_count);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* A NESTED non-ignored file (two levels deep) still makes the outer
   directory a collision -- the scan must recurse, not just look at direct
   children. */
static void test_directory_nested_nonignored_file(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    const char *paths[1];
    char **out_files = NULL;
    size_t out_files_count = 0;
    char **out_dirs = NULL;
    size_t out_dirs_count = 0;

    memset(&idx, 0, sizeof(idx));
    write_workdir_file(repo_root, ".gitignore", "*.log\n");
    write_workdir_file(repo_root, "dir/sub/ignored.log", "noise\n");
    write_workdir_file(repo_root, "dir/sub/deep/deep.txt", "user data\n");

    paths[0] = "dir";
    CHECK(sg_untracked_would_be_overwritten(git_dir, repo_root, &idx, paths, 1, &out_files,
                                            &out_files_count, &out_dirs, &out_dirs_count,
                                            NULL, NULL) == 0,
         "call failed");
    CHECK(out_files_count == 0, "expected no file collision, got %zu", out_files_count);
    CHECK(out_dirs_count == 1, "a nested non-ignored file must still make the outer directory a "
                               "collision, got %zu",
         out_dirs_count);
    if (out_dirs_count == 1)
        CHECK(strcmp(out_dirs[0], "dir") == 0, "expected 'dir', got '%s'", out_dirs[0]);

    free_result(out_files, out_files_count);
    free_result(out_dirs, out_dirs_count);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* A candidate path with nothing at all along it (ENOENT the whole way) is
   simply clear -- the control every other test relies on to prove the
   function does not over-report. */
static void test_clear_path_not_reported(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    const char *paths[1];
    char **out_files = NULL;
    size_t out_files_count = 0;
    char **out_dirs = NULL;
    size_t out_dirs_count = 0;

    memset(&idx, 0, sizeof(idx));

    paths[0] = "brand/new/path.txt";
    CHECK(sg_untracked_would_be_overwritten(git_dir, repo_root, &idx, paths, 1, &out_files,
                                            &out_files_count, &out_dirs, &out_dirs_count,
                                            NULL, NULL) == 0,
         "call failed");
    CHECK(out_files_count == 0, "a wholly-absent path must not be reported, got %zu",
         out_files_count);
    CHECK(out_dirs_count == 0, "a wholly-absent path must not be reported, got %zu",
         out_dirs_count);

    free_result(out_files, out_files_count);
    free_result(out_dirs, out_dirs_count);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* Phase 79c (F4): sg_untracked_would_be_overwritten's out_first_is_dir
   out-param names the FIRST collision in CANDIDATE order across BOTH
   buckets, not "the directory bucket whenever it is non-empty" -- measured
   against git 2.55.0 (U1b in the Phase 79c oracle) at the cmd_merge.c
   level; this exercises the same rule at the level of the underlying data
   both call sites actually consume. */
static void test_first_collision_across_buckets(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    const char *paths[2];
    char **out_files = NULL;
    size_t out_files_count = 0;
    char **out_dirs = NULL;
    size_t out_dirs_count = 0;
    int first_is_dir;

    memset(&idx, 0, sizeof(idx));
    write_workdir_file(repo_root, "a", "blocker file\n");
    write_workdir_file(repo_root, "z/leftover.txt", "user data\n");

    paths[0] = "a";
    paths[1] = "z";
    first_is_dir = -1;
    CHECK(sg_untracked_would_be_overwritten(git_dir, repo_root, &idx, paths, 2, &out_files,
                                            &out_files_count, &out_dirs, &out_dirs_count,
                                            &first_is_dir, NULL) == 0,
         "call failed");
    CHECK(out_files_count == 1 && out_dirs_count == 1,
         "expected one collision in each bucket, got %zu files / %zu dirs", out_files_count,
         out_dirs_count);
    CHECK(first_is_dir == 0,
         "expected the FIRST collision in candidate order to be the FILE ('a' before 'z'), got "
         "first_is_dir=%d",
         first_is_dir);
    free_result(out_files, out_files_count);
    free_result(out_dirs, out_dirs_count);

    out_files = NULL;
    out_files_count = 0;
    out_dirs = NULL;
    out_dirs_count = 0;
    paths[0] = "z";
    paths[1] = "a";
    first_is_dir = -1;
    CHECK(sg_untracked_would_be_overwritten(git_dir, repo_root, &idx, paths, 2, &out_files,
                                            &out_files_count, &out_dirs, &out_dirs_count,
                                            &first_is_dir, NULL) == 0,
         "call failed");
    CHECK(first_is_dir == 1,
         "expected the FIRST collision in candidate order to be the DIRECTORY this time ('z' "
         "before 'a'), got first_is_dir=%d",
         first_is_dir);
    free_result(out_files, out_files_count);
    free_result(out_dirs, out_dirs_count);

    /* The first collision is a FILE reached through the ancestor walk
       (untracked file 'a' blocks candidate 'a/x'), not a direct hit. That
       branch sets x_is_dir itself, so it needs its own case. */
    out_files = NULL;
    out_files_count = 0;
    out_dirs = NULL;
    out_dirs_count = 0;
    paths[0] = "a/x";
    paths[1] = "z";
    first_is_dir = -1;
    CHECK(sg_untracked_would_be_overwritten(git_dir, repo_root, &idx, paths, 2, &out_files,
                                            &out_files_count, &out_dirs, &out_dirs_count,
                                            &first_is_dir, NULL) == 0,
         "call failed");
    CHECK(out_files_count == 1 && out_dirs_count == 1,
         "expected ancestor-blocked 'a/x' plus directory 'z', got %zu files / %zu dirs",
         out_files_count, out_dirs_count);
    CHECK(first_is_dir == 0,
         "expected the FIRST collision to be the ancestor-blocked FILE ('a/x' before 'z'), got "
         "first_is_dir=%d",
         first_is_dir);

    free_result(out_files, out_files_count);
    free_result(out_dirs, out_dirs_count);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* Phase 79c (F2): an opendir() failure partway through the recursive scan
   (a chmod-000 subdirectory) must fail closed as a distinguishable I/O
   error, not the generic OOM state -- measured against git 2.55.0 (S6 in
   the Phase 79c oracle). Skipped when running as root: chmod 000 does not
   block root's own access, same guard this project already uses elsewhere
   (see interop.sh's `id -u = 0` checks). */
static void test_scan_opendir_permission_denied(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    const char *paths[1];
    char **out_files = NULL;
    size_t out_files_count = 0;
    char **out_dirs = NULL;
    size_t out_dirs_count = 0;
    sg_untracked_overwrite_error err;
    char locked_abs[4096];
    int rc;

    if (geteuid() == 0) {
        printf("skip test_scan_opendir_permission_denied: running as root, chmod 000 has no "
              "effect\n");
        free(repo_root);
        free(git_dir);
        return;
    }

    memset(&idx, 0, sizeof(idx));
    memset(&err, 0, sizeof(err));
    write_workdir_file(repo_root, "new/locked/secret.txt", "user data\n");
    snprintf(locked_abs, sizeof(locked_abs), "%s/new/locked", repo_root);
    CHECK(chmod(locked_abs, 0000) == 0, "chmod 000 failed");

    paths[0] = "new";
    rc = sg_untracked_would_be_overwritten(git_dir, repo_root, &idx, paths, 1, &out_files,
                                           &out_files_count, &out_dirs, &out_dirs_count, NULL,
                                           &err);
    CHECK(rc == -1, "expected the scan to fail closed on a permission error, got rc=%d", rc);
    CHECK(err.kind == SG_UNTRACKED_ERR_OPENDIR, "expected SG_UNTRACKED_ERR_OPENDIR, got kind=%d",
         (int)err.kind);
    CHECK(err.path != NULL && strcmp(err.path, "new/locked") == 0,
         "expected the error path to name 'new/locked', got '%s'",
         err.path != NULL ? err.path : "(null)");
    CHECK(err.saved_errno == EACCES, "expected EACCES, got errno %d", err.saved_errno);
    CHECK(out_files == NULL && out_files_count == 0, "an error must leave *out_files NULL/0");
    CHECK(out_dirs == NULL && out_dirs_count == 0, "an error must leave *out_dirs NULL/0");

    CHECK(chmod(locked_abs, 0755) == 0, "failed to restore permissions for cleanup");

    sg_untracked_overwrite_error_free(&err);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* Phase 79c (F3): each untracked_overwrite_dir_scan recursion frame used to
   hold ~3 char[SG_PATH_MAX] (4096-byte) arrays; a chain of ~2000
   one-letter untracked directories is reachable and would have needed
   ~24MB of stack. Builds such a chain (via mkdir+chdir, not by ever
   materializing the full path string, so this is not itself bounded by
   SG_PATH_MAX) and proves the scan does not crash. The ANSWER differs by
   platform and both are acceptable (see the comment inline below) -- only
   Linux CI's real PATH_MAX (4096, equal to this project's own
   SG_PATH_MAX) actually exercises full-depth recursion; this test's
   requirement is "did not crash", the same "collision OR error" shape
   this phase's own spec asked for. */
static void test_deep_recursion_stack_safety(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    const char *paths[1];
    char **out_files = NULL;
    size_t out_files_count = 0;
    char **out_dirs = NULL;
    size_t out_dirs_count = 0;
    sg_untracked_overwrite_error err;
    char cwd_save[4096];
    char start_abs[4096];
    char cleanup_cmd[4200];
    int depth;
    const int max_depth = 1900;
    int rc;

    memset(&idx, 0, sizeof(idx));
    memset(&err, 0, sizeof(err));

    CHECK(getcwd(cwd_save, sizeof(cwd_save)) != NULL, "getcwd failed");

    mkdir_workdir(repo_root, "d");
    snprintf(start_abs, sizeof(start_abs), "%s/d", repo_root);
    CHECK(chdir(start_abs) == 0, "chdir into start dir failed");

    for (depth = 0; depth < max_depth; depth++) {
        if (mkdir("a", 0755) != 0)
            break;
        if (chdir("a") != 0)
            break;
    }
    CHECK(depth > 100, "expected to create a deep chain, only got depth %d", depth);

    {
        FILE *f = fopen("leaf.txt", "w");

        if (f != NULL) {
            fputs("user data\n", f);
            fclose(f);
        }
    }

    CHECK(chdir(cwd_save) == 0, "chdir back to the test's own cwd failed");

    paths[0] = "d";
    rc = sg_untracked_would_be_overwritten(git_dir, repo_root, &idx, paths, 1, &out_files,
                                           &out_files_count, &out_dirs, &out_dirs_count, NULL,
                                           &err);
    /* Linux: real PATH_MAX is 4096 (== SG_PATH_MAX), the joined path fits,
       and the scan genuinely walks all the way down -- a directory
       collision on "d" is reported (the leaf file is not ignored). macOS:
       real PATH_MAX is 1024, so the OS-level opendir() fails partway down
       with ENAMETOOLONG well before this project's own SG_PATH_MAX bound
       would ever trip -- reported as a scan I/O error instead. Both are
       the fail-closed direction (never "no collision" nor a crash). */
    CHECK(rc == 0 || rc == -1, "unexpected return value %d", rc);
    if (rc == 0) {
        CHECK(out_dirs_count == 1 && out_files_count == 0,
             "expected a directory collision on a platform where the full path fits under "
             "PATH_MAX, got %zu files / %zu dirs",
             out_files_count, out_dirs_count);
    } else {
        CHECK(err.kind != SG_UNTRACKED_ERR_NONE, "expected a filled error state, got NONE");
        CHECK(err.kind != SG_UNTRACKED_ERR_ALLOC,
             "expected a path-length/opendir scan error, not an allocation failure (kind=%d)",
             (int)err.kind);
    }

    free_result(out_files, out_files_count);
    free_result(out_dirs, out_dirs_count);
    sg_untracked_overwrite_error_free(&err);
    sg_index_free(&idx);

    /* Remove the whole repo, not just git_dir: the deep chain lives under
       repo_root, and deleting only ".git" would leave ~1900 nested
       directories in /tmp on every run. */
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf '%s'", repo_root);
    CHECK(system(cleanup_cmd) == 0, "failed to remove the deep test repo %s", repo_root);
    CHECK(access(repo_root, F_OK) != 0, "deep test repo %s still exists after cleanup", repo_root);

    free(repo_root);
    free(git_dir);
}

/* Phase 80 (F3a): a SYMLINK ancestor (not a regular file) blocking a deep
   candidate must be caught even when lstat(P) itself does NOT fail ENOTDIR
   -- the pre-Phase-80 code only walked ancestors when lstat(candidate)
   failed with ENOTDIR, but lstat resolves an intermediate symlink
   component, so a symlink pointing at some OTHER real directory that does
   not itself contain the rest of the candidate's path makes lstat(P) fail
   with plain ENOENT (or even succeed), not ENOTDIR -- the exact shape that
   made the pre-Phase-80 classifier silently answer "no collision" for a
   security-relevant blocker. */
static void test_ancestor_symlink_blocker(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    const char *paths[1];
    char **out_files = NULL;
    size_t out_files_count = 0;
    char **out_dirs = NULL;
    size_t out_dirs_count = 0;
    char elsewhere[4096];
    char link_path[4096];

    memset(&idx, 0, sizeof(idx));
    snprintf(elsewhere, sizeof(elsewhere), "%s/elsewhere", repo_root);
    CHECK(mkdir(elsewhere, 0755) == 0, "failed to mkdir elsewhere");
    snprintf(link_path, sizeof(link_path), "%s/a", repo_root);
    CHECK(symlink(elsewhere, link_path) == 0, "failed to symlink a -> elsewhere");

    /* "elsewhere" has no "b" component at all, so lstat("a/b/c.txt")
       (which DOES follow the "a" symlink, since only the FINAL component
       of an lstat path is left unresolved) fails with plain ENOENT, not
       ENOTDIR -- proving this test actually discriminates the fix rather
       than accidentally reproducing the old ENOTDIR-only path. */
    paths[0] = "a/b/c.txt";
    CHECK(sg_untracked_would_be_overwritten(git_dir, repo_root, &idx, paths, 1, &out_files,
                                            &out_files_count, &out_dirs, &out_dirs_count,
                                            NULL, NULL) == 0,
         "call failed");
    CHECK(out_files_count == 1, "expected the symlink 'a' to be reported as the blocker, got %zu",
         out_files_count);
    CHECK(out_dirs_count == 0, "expected no directory collision, got %zu", out_dirs_count);
    if (out_files_count == 1)
        CHECK(strcmp(out_files[0], "a") == 0, "expected blocker 'a', got '%s'", out_files[0]);

    free_result(out_files, out_files_count);
    free_result(out_dirs, out_dirs_count);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* Phase 80 (F3b): a path tracked in idx found while recursively scanning a
   directory's contents is NOT untracked content -- the pre-flight's own
   directory bucket must not refuse a merge just because a file the SAME
   merge is about to delete still physically sits on disk. Run as a pair
   with the tracked entry present (no collision) and absent (a genuine
   collision), so the tracked-path exemption itself is what's proven,
   not merely "an empty directory doesn't collide". */
static void test_directory_scan_tracked_exemption(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    sg_index_entry e;
    const char *paths[1];
    char **out_files = NULL;
    size_t out_files_count = 0;
    char **out_dirs = NULL;
    size_t out_dirs_count = 0;

    memset(&idx, 0, sizeof(idx));
    mkdir_workdir(repo_root, "d");
    write_workdir_file(repo_root, "d/x.txt", "tracked content\n");

    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    e.path = (char *)"d/x.txt";
    CHECK(sg_index_upsert(&idx, &e) == 0, "upsert failed");

    paths[0] = "d";
    CHECK(sg_untracked_would_be_overwritten(git_dir, repo_root, &idx, paths, 1, &out_files,
                                            &out_files_count, &out_dirs, &out_dirs_count,
                                            NULL, NULL) == 0,
         "call failed");
    CHECK(out_files_count == 0, "expected no file collision, got %zu", out_files_count);
    CHECK(out_dirs_count == 0,
         "a directory whose only content is tracked (and about to be deleted by the same "
         "operation) must not collide, got %zu",
         out_dirs_count);
    free_result(out_files, out_files_count);
    free_result(out_dirs, out_dirs_count);

    /* Control: the SAME on-disk shape, but idx has nothing tracked at
       d/x.txt -- this must now genuinely collide (directory bucket), or
       the first half of this test would be vacuous (e.g. if the scan
       always answered "no collision" for any single-file directory
       regardless of idx). */
    out_files = NULL;
    out_files_count = 0;
    out_dirs = NULL;
    out_dirs_count = 0;
    sg_index_free(&idx);
    memset(&idx, 0, sizeof(idx));
    CHECK(sg_untracked_would_be_overwritten(git_dir, repo_root, &idx, paths, 1, &out_files,
                                            &out_files_count, &out_dirs, &out_dirs_count,
                                            NULL, NULL) == 0,
         "call failed (control)");
    CHECK(out_dirs_count == 1,
         "control: without the tracked exemption this directory must collide, got %zu",
         out_dirs_count);
    CHECK(out_files_count == 0, "expected no file collision (control), got %zu", out_files_count);

    free_result(out_files, out_files_count);
    free_result(out_dirs, out_dirs_count);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

int main(void)
{
    test_ancestor_blocker();
    test_no_dedup_same_blocker();
    test_ignored_not_reported();
    test_tracked_not_reported();
    test_truncation_fails_closed();
    test_empty_directory();
    test_directory_all_contents_ignored();
    test_directory_contents_ignored_by_pattern();
    test_directory_nested_nonignored_file();
    test_clear_path_not_reported();
    test_first_collision_across_buckets();
    test_scan_opendir_permission_denied();
    test_deep_recursion_stack_safety();
    test_ancestor_symlink_blocker();
    test_directory_scan_tracked_exemption();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all untracked_overwrite tests passed\n");
    return 0;
}
