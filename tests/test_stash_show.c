/* `sg stash show` (Phase 25 WP4): format selection and the tracked/untracked
   comparison rules sg_stash_load_trees + sg_diff_trees implement. Goes
   through the public sg_cmd_stash entry point (cmd_stash_show is a static
   helper inside cmd_stash.c, same pattern test_status_unmerged.c uses),
   capturing stdout to check the rendered text -- this project's sg_diff_print
   is already covered format-by-format in tests/test_diff_out.c, so these
   tests only need to pin which format/tree-pair `sg stash show` picked, not
   re-verify the renderer's own byte-for-byte output. */
#include "sg/cli.h"

#include "sg/hash.h"
#include "sg/index.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/stash.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

#include <fcntl.h>
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

static char *make_tmp_repo(void)
{
    static char template[] = "/tmp/sg_stash_show_test_XXXXXX";
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

/* Builds an initial commit on "master" out of `count` (path, content) pairs,
   staged and committed, so `sg stash show` has a real base_tree to diff
   against. Mirrors tests/test_stash.c's commit_initial, generalized to more
   than one file for the interleaving test below. */
static void commit_initial_files(const char *git_dir, const char *repo_root, const char **paths,
                                 const char **contents, size_t count)
{
    sg_index idx;
    sg_flat_entry *entries;
    sg_commit commit;
    unsigned char *serialized;
    size_t serialized_len;
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    size_t i;

    entries = malloc(count * sizeof(*entries));
    memset(&idx, 0, sizeof(idx));

    for (i = 0; i < count; i++) {
        unsigned char blob[SG_SHA1_RAW_LEN];
        sg_index_entry e;

        write_workdir_file(repo_root, paths[i], contents[i]);
        CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, contents[i], strlen(contents[i]), blob) == 0,
             "write blob %s", paths[i]);

        memset(&e, 0, sizeof(e));
        e.mode = 0100644;
        memcpy(e.sha1, blob, SG_SHA1_RAW_LEN);
        e.path = (char *)paths[i];
        CHECK(sg_index_upsert(&idx, &e) == 0, "upsert %s", paths[i]);

        entries[i].path = (char *)paths[i];
        entries[i].mode = 0100644;
        memcpy(entries[i].sha1, blob, SG_SHA1_RAW_LEN);
    }
    CHECK(sg_index_write(git_dir, &idx) == 0, "write index");
    sg_index_free(&idx);

    CHECK(sg_tree_build(git_dir, entries, count, tree_id) == 0, "build tree");
    free(entries);

    memset(&commit, 0, sizeof(commit));
    memcpy(commit.tree, tree_id, SG_SHA1_RAW_LEN);
    commit.parent_count = 0;
    commit.author_name = (char *)"Test";
    commit.author_email = (char *)"test@example.com";
    commit.author_time = 1700000000;
    strcpy(commit.author_tz, "+0000");
    commit.committer_name = commit.author_name;
    commit.committer_email = commit.author_email;
    commit.committer_time = commit.author_time;
    strcpy(commit.committer_tz, "+0000");
    commit.message = (char *)"initial\n";

    CHECK(sg_commit_serialize(&commit, &serialized, &serialized_len) == 0, "serialize commit");
    CHECK(sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len, commit_id) == 0,
         "write commit");
    free(serialized);

    CHECK(sg_ref_update_branch(git_dir, "master", commit_id) == 0, "update branch");
}

static int stash_push_plain(const char *git_dir, const char *repo_root, const char *message,
                            unsigned char commit_id_out[SG_SHA1_RAW_LEN])
{
    sg_stash_push_opts opts;

    memset(&opts, 0, sizeof(opts));
    opts.message = message;
    return sg_stash_push(git_dir, repo_root, &opts, commit_id_out);
}

static int stash_push_untracked(const char *git_dir, const char *repo_root, const char *message,
                                unsigned char commit_id_out[SG_SHA1_RAW_LEN])
{
    sg_stash_push_opts opts;

    memset(&opts, 0, sizeof(opts));
    opts.message = message;
    opts.include_untracked = 1;
    return sg_stash_push(git_dir, repo_root, &opts, commit_id_out);
}

/* Runs sg_cmd_stash with the given argv from inside repo_root, capturing
   everything it writes to stdout into out (NUL-terminated, truncated to
   out_size - 1 if needed). Mirrors test_status_unmerged.c's
   run_status_capture. sg_cmd_stash's subcommands find the repo via
   sg_require_git_dir(), which walks up from cwd, so the working directory
   is switched to repo_root for the duration of the call. */
static int run_stash_capture(const char *repo_root, int argc, char **argv, char *out, size_t out_size)
{
    char tmpl[] = "/tmp/sg_stash_show_capture_XXXXXX";
    int tmp_fd;
    int saved_stdout;
    off_t len;
    ssize_t n;
    char cwd[4096];
    int rc;

    CHECK(getcwd(cwd, sizeof(cwd)) != NULL, "getcwd failed");
    CHECK(chdir(repo_root) == 0, "chdir to repo_root failed");

    tmp_fd = mkstemp(tmpl);
    if (tmp_fd < 0) {
        fprintf(stderr, "mkstemp failed\n");
        exit(1);
    }
    unlink(tmpl);

    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    dup2(tmp_fd, STDOUT_FILENO);

    rc = sg_cmd_stash(argc, argv);

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

    CHECK(chdir(cwd) == 0, "chdir back failed");
    return rc;
}

/* Same as run_stash_capture, but captures stderr instead of stdout -- for
   the error-message assertions below (out-of-range spec, malformed
   --stat=). */
static int run_stash_capture_stderr(const char *repo_root, int argc, char **argv, char *err, size_t err_size)
{
    char tmpl[] = "/tmp/sg_stash_show_stderr_XXXXXX";
    int tmp_fd;
    int saved_stderr;
    off_t len;
    ssize_t n;
    char cwd[4096];
    int rc;

    CHECK(getcwd(cwd, sizeof(cwd)) != NULL, "getcwd failed");
    CHECK(chdir(repo_root) == 0, "chdir to repo_root failed");

    tmp_fd = mkstemp(tmpl);
    if (tmp_fd < 0) {
        fprintf(stderr, "mkstemp failed\n");
        exit(1);
    }
    unlink(tmpl);

    fflush(stderr);
    saved_stderr = dup(STDERR_FILENO);
    dup2(tmp_fd, STDERR_FILENO);

    rc = sg_cmd_stash(argc, argv);

    fflush(stderr);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);

    len = lseek(tmp_fd, 0, SEEK_CUR);
    if (len < 0 || (size_t)len >= err_size)
        len = (off_t)err_size - 1;
    lseek(tmp_fd, 0, SEEK_SET);
    n = read(tmp_fd, err, (size_t)len);
    err[n > 0 ? n : 0] = '\0';
    close(tmp_fd);

    CHECK(chdir(cwd) == 0, "chdir back failed");
    return rc;
}

/* ---- default format is --stat, not patch -------------------------------- */

static void test_default_is_stat(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    const char *paths[1] = {"a.txt"};
    const char *contents[1] = {"hello\n"};
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    char out[4096];
    char *argv[2];
    int rc;

    commit_initial_files(git_dir, repo_root, paths, contents, 1);
    write_workdir_file(repo_root, "a.txt", "hello\nworld\n");
    CHECK(stash_push_plain(git_dir, repo_root, "wip", commit_id) == 0, "stash push failed");

    argv[0] = (char *)"stash";
    argv[1] = (char *)"show";
    rc = run_stash_capture(repo_root, 2, argv, out, sizeof(out));
    CHECK(rc == 0, "sg stash show failed: %d", rc);
    CHECK(strstr(out, "file changed") != NULL, "default output missing diffstat summary: %s", out);
    CHECK(strstr(out, "diff --git") == NULL, "default output should not be a patch: %s", out);

    free(git_dir);
    free(repo_root);
}

/* ---- -p/--patch prints a patch -------------------------------------------- */

static void test_patch_flag(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    const char *paths[1] = {"a.txt"};
    const char *contents[1] = {"hello\n"};
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    char out[4096];
    char *argv[3];
    int rc;

    commit_initial_files(git_dir, repo_root, paths, contents, 1);
    write_workdir_file(repo_root, "a.txt", "hello\nworld\n");
    CHECK(stash_push_plain(git_dir, repo_root, "wip", commit_id) == 0, "stash push failed");

    argv[0] = (char *)"stash";
    argv[1] = (char *)"show";
    argv[2] = (char *)"-p";
    rc = run_stash_capture(repo_root, 3, argv, out, sizeof(out));
    CHECK(rc == 0, "sg stash show -p failed: %d", rc);
    CHECK(strstr(out, "diff --git a/a.txt b/a.txt") != NULL, "-p output missing patch header: %s", out);

    free(git_dir);
    free(repo_root);
}

/* ---- every format flag maps to the format sg_diff_print actually
   renders -- markers pinned against tests/test_diff_out.c's own fixtures
   for the same content change, not re-derived here. ---------------------- */

static void test_format_flags_map(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    const char *paths[1] = {"a.txt"};
    const char *contents[1] = {"line1\nline2\nline3\n"};
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    char out[4096];
    char *argv[3];

    commit_initial_files(git_dir, repo_root, paths, contents, 1);
    write_workdir_file(repo_root, "a.txt", "line1\nCHANGED\nline3\nline4\n");
    CHECK(stash_push_plain(git_dir, repo_root, "wip", commit_id) == 0, "stash push failed");

    argv[0] = (char *)"stash";
    argv[1] = (char *)"show";

    argv[2] = (char *)"--numstat";
    CHECK(run_stash_capture(repo_root, 3, argv, out, sizeof(out)) == 0, "--numstat failed");
    CHECK(strcmp(out, "2\t1\ta.txt\n") == 0, "--numstat mismatch: %s", out);

    argv[2] = (char *)"--name-only";
    CHECK(run_stash_capture(repo_root, 3, argv, out, sizeof(out)) == 0, "--name-only failed");
    CHECK(strcmp(out, "a.txt\n") == 0, "--name-only mismatch: %s", out);

    argv[2] = (char *)"--name-status";
    CHECK(run_stash_capture(repo_root, 3, argv, out, sizeof(out)) == 0, "--name-status failed");
    CHECK(strcmp(out, "M\ta.txt\n") == 0, "--name-status mismatch: %s", out);

    argv[2] = (char *)"--shortstat";
    CHECK(run_stash_capture(repo_root, 3, argv, out, sizeof(out)) == 0, "--shortstat failed");
    CHECK(strcmp(out, " 1 file changed, 2 insertions(+), 1 deletion(-)\n") == 0,
         "--shortstat mismatch: %s", out);
    CHECK(strstr(out, "a.txt") == NULL, "--shortstat should not name the file: %s", out);

    argv[2] = (char *)"--stat";
    CHECK(run_stash_capture(repo_root, 3, argv, out, sizeof(out)) == 0, "--stat failed");
    CHECK(strstr(out, "a.txt") != NULL && strstr(out, "file changed") != NULL,
         "--stat mismatch: %s", out);

    /* --stat=<width>[,<name-width>] -- sg_diff_print's own byte-exact
       layout for a given width is already pinned in test_diff_out.c; this
       only needs to prove sg_stash_show's argv loop actually recognizes
       "--stat=..." (it did not, before this was wired to
       sg_diff_parse_stat_arg -- it fell into the unrecognized-flag branch
       and printed usage instead). */
    argv[2] = (char *)"--stat=40,10";
    CHECK(run_stash_capture(repo_root, 3, argv, out, sizeof(out)) == 0, "--stat=40,10 failed");
    CHECK(strstr(out, "a.txt") != NULL && strstr(out, "file changed") != NULL,
         "--stat=40,10 mismatch: %s", out);

    {
        char errbuf[1024];
        int rc;

        argv[2] = (char *)"--stat=bogus";
        rc = run_stash_capture_stderr(repo_root, 3, argv, errbuf, sizeof(errbuf));
        CHECK(rc == 1, "--stat=bogus should be a usage error, got %d", rc);
        CHECK(strstr(errbuf, "usage: sg stash show") != NULL,
             "--stat=bogus should print usage, got: %s", errbuf);
    }

    free(git_dir);
    free(repo_root);
}

/* ---- --only-untracked lists only the untracked half ---------------------- */

static void test_only_untracked(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    const char *paths[1] = {"a.txt"};
    const char *contents[1] = {"hello\n"};
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    char out[4096];
    char *argv[3];

    commit_initial_files(git_dir, repo_root, paths, contents, 1);
    write_workdir_file(repo_root, "a.txt", "hello\nworld\n");
    write_workdir_file(repo_root, "u.txt", "untracked\n");
    CHECK(stash_push_untracked(git_dir, repo_root, "wip", commit_id) == 0, "stash push -u failed");

    argv[0] = (char *)"stash";
    argv[1] = (char *)"show";
    argv[2] = (char *)"--only-untracked";
    CHECK(run_stash_capture(repo_root, 3, argv, out, sizeof(out)) == 0, "--only-untracked failed");
    CHECK(strstr(out, "u.txt") != NULL, "--only-untracked missing u.txt: %s", out);
    CHECK(strstr(out, "a.txt") == NULL, "--only-untracked should not list a.txt: %s", out);

    free(git_dir);
    free(repo_root);
}

/* --only-untracked with no untracked parent (a plain 2-parent stash):
   real git prints nothing and exits 0 (measured, see cmd_stash.c). */
static void test_only_untracked_no_untracked_parent(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    const char *paths[1] = {"a.txt"};
    const char *contents[1] = {"hello\n"};
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    char out[4096];
    char *argv[3];
    int rc;

    commit_initial_files(git_dir, repo_root, paths, contents, 1);
    write_workdir_file(repo_root, "a.txt", "hello\nworld\n");
    CHECK(stash_push_plain(git_dir, repo_root, "wip", commit_id) == 0, "stash push failed");

    argv[0] = (char *)"stash";
    argv[1] = (char *)"show";
    argv[2] = (char *)"--only-untracked";
    rc = run_stash_capture(repo_root, 3, argv, out, sizeof(out));
    CHECK(rc == 0, "--only-untracked with no untracked parent should exit 0, got %d", rc);
    CHECK(out[0] == '\0', "--only-untracked with no untracked parent should print nothing: %s", out);

    free(git_dir);
    free(repo_root);
}

/* ---- -u is the union of tracked and untracked, path-sorted --------------- */

static void test_include_untracked_is_sorted_union(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    const char *paths[2] = {"a.txt", "c.txt"};
    const char *contents[2] = {"hello\n", "world\n"};
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    char out[4096];
    char *argv[3];

    commit_initial_files(git_dir, repo_root, paths, contents, 2);
    /* Both tracked files change, and an untracked b.txt sits alphabetically
       between them -- if the "-u" merge forgot to sort (e.g. concatenated
       the tracked list followed by the untracked one instead of a real
       merge), this would come out as "a.txt\nc.txt\nb.txt\n" instead of the
       path-sorted "a.txt\nb.txt\nc.txt\n". */
    write_workdir_file(repo_root, "a.txt", "hello\nchanged\n");
    write_workdir_file(repo_root, "c.txt", "world\nchanged\n");
    write_workdir_file(repo_root, "b.txt", "new\n");
    CHECK(stash_push_untracked(git_dir, repo_root, "wip", commit_id) == 0, "stash push -u failed");

    argv[0] = (char *)"stash";
    argv[1] = (char *)"show";
    argv[2] = (char *)"-u";
    CHECK(run_stash_capture(repo_root, 3, argv, out, sizeof(out)) == 0, "-u failed");
    CHECK(strstr(out, "a.txt") != NULL && strstr(out, "b.txt") != NULL && strstr(out, "c.txt") != NULL,
         "-u --stat should list all three files: %s", out);

    {
        char *name_only_argv[4];

        name_only_argv[0] = (char *)"stash";
        name_only_argv[1] = (char *)"show";
        name_only_argv[2] = (char *)"-u";
        name_only_argv[3] = (char *)"--name-only";
        CHECK(run_stash_capture(repo_root, 4, name_only_argv, out, sizeof(out)) == 0,
             "-u --name-only failed");
        CHECK(strcmp(out, "a.txt\nb.txt\nc.txt\n") == 0,
             "-u should list a path-sorted union, got: %s", out);
    }

    free(git_dir);
    free(repo_root);
}

/* -u and --only-untracked are a single mode selector, not two independent
   toggles -- whichever is named LAST on the command line wins (measured
   against real git 2.55.0, see cmd_stash.c's show_untracked_mode comment).
   Both orderings are exercised on the same 3-parent stash so a regression
   that made them "combine" (or made the first one always win) would show up
   as one of these two assertions failing. */
static void test_untracked_mode_last_flag_wins(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    const char *paths[2] = {"a.txt", "c.txt"};
    const char *contents[2] = {"hello\n", "world\n"};
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    char out[4096];
    char *argv[4];

    commit_initial_files(git_dir, repo_root, paths, contents, 2);
    write_workdir_file(repo_root, "a.txt", "hello\nchanged\n");
    write_workdir_file(repo_root, "c.txt", "world\nchanged\n");
    write_workdir_file(repo_root, "b.txt", "new\n");
    CHECK(stash_push_untracked(git_dir, repo_root, "wip", commit_id) == 0, "stash push -u failed");

    argv[0] = (char *)"stash";
    argv[1] = (char *)"show";
    argv[2] = (char *)"-u";
    argv[3] = (char *)"--only-untracked";
    CHECK(run_stash_capture(repo_root, 4, argv, out, sizeof(out)) == 0, "-u --only-untracked failed");
    CHECK(strstr(out, "b.txt") != NULL, "-u --only-untracked (only-untracked wins) missing b.txt: %s", out);
    CHECK(strstr(out, "a.txt") == NULL && strstr(out, "c.txt") == NULL,
         "-u --only-untracked (only-untracked wins) should not list tracked paths: %s", out);

    argv[2] = (char *)"--only-untracked";
    argv[3] = (char *)"-u";
    CHECK(run_stash_capture(repo_root, 4, argv, out, sizeof(out)) == 0, "--only-untracked -u failed");
    CHECK(strstr(out, "a.txt") != NULL && strstr(out, "b.txt") != NULL && strstr(out, "c.txt") != NULL,
         "--only-untracked -u (-u wins) should list the full tracked+untracked union: %s", out);

    free(git_dir);
    free(repo_root);
}

/* -u with no untracked parent falls back to the tracked-only diff. */
static void test_include_untracked_no_untracked_parent_falls_back(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    const char *paths[1] = {"a.txt"};
    const char *contents[1] = {"hello\n"};
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    char out[4096];
    char *argv[4];
    int rc;

    commit_initial_files(git_dir, repo_root, paths, contents, 1);
    write_workdir_file(repo_root, "a.txt", "hello\nworld\n");
    CHECK(stash_push_plain(git_dir, repo_root, "wip", commit_id) == 0, "stash push failed");

    argv[0] = (char *)"stash";
    argv[1] = (char *)"show";
    argv[2] = (char *)"-u";
    argv[3] = (char *)"--name-only";
    rc = run_stash_capture(repo_root, 4, argv, out, sizeof(out));
    CHECK(rc == 0, "-u with no untracked parent should exit 0, got %d", rc);
    CHECK(strcmp(out, "a.txt\n") == 0, "-u with no untracked parent should fall back to tracked diff: %s",
         out);

    free(git_dir);
    free(repo_root);
}

/* ---- stash@{N} range checking matches drop/apply's message --------------- */

static void test_out_of_range_spec(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    const char *paths[1] = {"a.txt"};
    const char *contents[1] = {"hello\n"};
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    char *argv[3];
    char errbuf[4096];
    int rc;

    commit_initial_files(git_dir, repo_root, paths, contents, 1);
    write_workdir_file(repo_root, "a.txt", "hello\nworld\n");
    CHECK(stash_push_plain(git_dir, repo_root, "wip", commit_id) == 0, "stash push failed");

    argv[0] = (char *)"stash";
    argv[1] = (char *)"show";
    argv[2] = (char *)"stash@{5}";
    rc = run_stash_capture_stderr(repo_root, 3, argv, errbuf, sizeof(errbuf));

    CHECK(rc == 1, "out-of-range stash@{5} should fail, got %d", rc);
    CHECK(strstr(errbuf, "log for 'stash' only has") != NULL,
         "out-of-range message should match drop/apply's wording: %s", errbuf);

    free(git_dir);
    free(repo_root);
}

/* The precise off-by-one boundary: stash@{1} against a stack that has
   EXACTLY 1 entry (index == count, not index > count). A range check
   mistakenly written as `index > list.count` instead of `index >=
   list.count` would let this one through -- stash@{5} above (index way
   past count) cannot tell the two apart, since 5 > 1 either way. */
static void test_out_of_range_spec_exact_boundary(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    const char *paths[1] = {"a.txt"};
    const char *contents[1] = {"hello\n"};
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    char *argv[3];
    char errbuf[4096];
    int rc;

    commit_initial_files(git_dir, repo_root, paths, contents, 1);
    write_workdir_file(repo_root, "a.txt", "hello\nworld\n");
    CHECK(stash_push_plain(git_dir, repo_root, "wip", commit_id) == 0, "stash push failed");
    /* Exactly one entry on the stack now: stash@{0}. stash@{1} is the
       precise boundary. */

    argv[0] = (char *)"stash";
    argv[1] = (char *)"show";
    argv[2] = (char *)"stash@{1}";
    rc = run_stash_capture_stderr(repo_root, 3, argv, errbuf, sizeof(errbuf));

    CHECK(rc == 1, "stash@{1} on a 1-entry stack should fail, got %d", rc);
    CHECK(strstr(errbuf, "log for 'stash' only has") != NULL,
         "boundary out-of-range message should match drop/apply's wording: %s", errbuf);

    free(git_dir);
    free(repo_root);
}

/* ---- sg_stash_load_trees reports has_untracked correctly for 2- and
   3-parent stashes -------------------------------------------------------- */

static void test_load_trees_has_untracked(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    const char *paths[1] = {"a.txt"};
    const char *contents[1] = {"hello\n"};
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    sg_stash_trees trees;

    commit_initial_files(git_dir, repo_root, paths, contents, 1);

    write_workdir_file(repo_root, "a.txt", "hello\nworld\n");
    CHECK(stash_push_plain(git_dir, repo_root, "2-parent", commit_id) == 0, "2-parent stash push failed");
    CHECK(sg_stash_load_trees(git_dir, 0, &trees) == 0, "load_trees failed for 2-parent stash");
    CHECK(trees.has_untracked == 0, "2-parent stash should report has_untracked == 0");

    write_workdir_file(repo_root, "a.txt", "hello\nworld\nagain\n");
    write_workdir_file(repo_root, "u.txt", "untracked\n");
    CHECK(stash_push_untracked(git_dir, repo_root, "3-parent", commit_id) == 0,
         "3-parent stash push failed");
    CHECK(sg_stash_load_trees(git_dir, 0, &trees) == 0, "load_trees failed for 3-parent stash");
    CHECK(trees.has_untracked == 1, "3-parent stash should report has_untracked == 1");

    free(git_dir);
    free(repo_root);
}

int main(void)
{
    test_default_is_stat();
    test_patch_flag();
    test_format_flags_map();
    test_only_untracked();
    test_only_untracked_no_untracked_parent();
    test_include_untracked_is_sorted_union();
    test_include_untracked_no_untracked_parent_falls_back();
    test_out_of_range_spec();
    test_out_of_range_spec_exact_boundary();
    test_untracked_mode_last_flag_wins();
    test_load_trees_has_untracked();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
