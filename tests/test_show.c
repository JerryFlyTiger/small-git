/* Phase 59 spec: `sg show`'s flag model (section 2) and the merge dense-set
   status letters (section 4.1). Runs through the public sg_cmd_show entry
   point (cmd_show.c's parse loop and renderer are all static), the same
   in-process stdout-capture pattern tests/test_stash_show.c already uses --
   "driven through the renderer rather than the CLI" per the spec's own
   testing note, meaning no subprocess/shelling out, just a direct call.
   Own `failures` counter and `CHECK` macro, no shared framework, per
   project convention. */
#include "sg/cli.h"

#include "sg/hash.h"
#include "sg/index.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/tree_build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, ...)                                                                         \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);                                  \
            fprintf(stderr, __VA_ARGS__);                                                         \
            fprintf(stderr, "\n");                                                                \
            failures++;                                                                           \
        }                                                                                          \
    } while (0)

static char *make_tmp_repo(char **repo_root_out)
{
    static char template[] = "/tmp/sg_show_test_XXXXXX";
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
    *repo_root_out = path;
    return strdup(git_dir);
}

/* Runs sg_cmd_show with the given argv from inside repo_root, capturing
   everything it writes to stdout into out (NUL-terminated, truncated to
   out_size - 1 if needed) -- mirrors test_stash_show.c's run_stash_capture. */
static int run_show_capture(const char *repo_root, int argc, char **argv, char *out,
                            size_t out_size)
{
    char tmpl[] = "/tmp/sg_show_test_capture_XXXXXX";
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

    rc = sg_cmd_show(argc, argv);

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

/* Builds a tree containing `count` (path, content) pairs and writes both the
   blobs and the tree object. entries must already be in git's sort order
   (all these fixtures use plain, non-nested single-component names, so
   plain lexicographic order suffices). */
static void build_tree(const char *git_dir, const char **paths, const char **contents,
                       size_t count, unsigned char tree_id_out[SG_SHA1_RAW_LEN])
{
    sg_flat_entry *entries = malloc(count * sizeof(*entries));
    size_t i;

    for (i = 0; i < count; i++) {
        unsigned char blob[SG_SHA1_RAW_LEN];

        CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, contents[i], strlen(contents[i]), blob) == 0,
             "write blob %s", paths[i]);
        entries[i].path = (char *)paths[i];
        entries[i].mode = 0100644;
        memcpy(entries[i].sha1, blob, SG_SHA1_RAW_LEN);
    }
    CHECK(sg_tree_build(git_dir, entries, count, tree_id_out) == 0, "build tree");
    free(entries);
}

static void write_commit(const char *git_dir, const unsigned char tree[SG_SHA1_RAW_LEN],
                         unsigned char (*parents)[SG_SHA1_RAW_LEN], size_t parent_count,
                         const char *message, long long time_stamp,
                         unsigned char commit_id_out[SG_SHA1_RAW_LEN])
{
    sg_commit commit;
    unsigned char *serialized;
    size_t serialized_len;

    memset(&commit, 0, sizeof(commit));
    memcpy(commit.tree, tree, SG_SHA1_RAW_LEN);
    commit.parents = parent_count > 0 ? parents : NULL;
    commit.parent_count = parent_count;
    commit.author_name = (char *)"Test";
    commit.author_email = (char *)"test@example.com";
    commit.author_time = time_stamp;
    strcpy(commit.author_tz, "+0000");
    commit.committer_name = commit.author_name;
    commit.committer_email = commit.author_email;
    commit.committer_time = time_stamp;
    strcpy(commit.committer_tz, "+0000");
    commit.message = (char *)message;

    CHECK(sg_commit_serialize(&commit, &serialized, &serialized_len) == 0, "serialize commit");
    CHECK(sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len, commit_id_out) == 0,
         "write commit");
    free(serialized);
}

/* ---- section 2: the flag model, table-driven ---------------------------- */

typedef enum { CAT_ERROR, CAT_HEADER, CAT_PATCH, CAT_NAMES, CAT_STATUS } out_category;

static const char *cat_name(out_category c)
{
    switch (c) {
    case CAT_ERROR:
        return "ERROR";
    case CAT_HEADER:
        return "HEADER";
    case CAT_PATCH:
        return "PATCH";
    case CAT_NAMES:
        return "NAMES";
    case CAT_STATUS:
        return "STATUS";
    default:
        return "?";
    }
}

/* Classifies a successful sg_cmd_show run's stdout into one of the four
   non-error shapes reachable in this fixture (single-parent commit, one
   changed file f.txt, modified so its status letter is 'M'). The three
   markers are mutually exclusive by construction: only the patch body
   contains "diff --git", only --name-status prefixes the path with a
   status letter and a tab, and --name-only is exactly "f.txt" on its own
   line with neither of the other two markers. */
static out_category classify(const char *out)
{
    if (strstr(out, "diff --git") != NULL)
        return CAT_PATCH;
    if (strstr(out, "M\tf.txt\n") != NULL)
        return CAT_STATUS;
    if (strstr(out, "\nf.txt\n") != NULL)
        return CAT_NAMES;
    return CAT_HEADER;
}

typedef struct {
    const char *label;
    const char *argv_tail[4]; /* NULL-terminated, up to 3 flags */
    out_category expect;
} flag_row;

/* Every row of the Phase 59 spec section 2 table, plus a handful of the
   already-established single-flag baselines the spec's prose calls out
   (`--stat --name-only` etc reuse the same table so both flag orders are
   covered, not just the ones the spec singled out as falsifying). */
static const flag_row FLAG_ROWS[] = {
    { "-s --name-only", { "-s", "--name-only", NULL }, CAT_ERROR },
    { "--name-only -s", { "--name-only", "-s", NULL }, CAT_HEADER },
    { "-s --name-status", { "-s", "--name-status", NULL }, CAT_ERROR },
    { "--name-status -s", { "--name-status", "-s", NULL }, CAT_HEADER },
    { "--name-only --name-status", { "--name-only", "--name-status", NULL }, CAT_ERROR },
    { "--name-status --name-only", { "--name-status", "--name-only", NULL }, CAT_ERROR },
    { "--name-only --name-only", { "--name-only", "--name-only", NULL }, CAT_NAMES },
    { "--name-only -p", { "--name-only", "-p", NULL }, CAT_NAMES },
    { "-p --name-only", { "-p", "--name-only", NULL }, CAT_NAMES },
    { "--name-only --stat", { "--name-only", "--stat", NULL }, CAT_NAMES },
    { "--stat --name-only", { "--stat", "--name-only", NULL }, CAT_NAMES },
    { "-p --name-only --stat", { "-p", "--name-only", "--stat" }, CAT_NAMES },
    { "--name-only -s -p", { "--name-only", "-s", "-p" }, CAT_PATCH },
    { "--name-only -p -s", { "--name-only", "-p", "-s" }, CAT_HEADER },
    { "-s -p --name-only", { "-s", "-p", "--name-only" }, CAT_NAMES },
    { "--stat --name-status -p", { "--stat", "--name-status", "-p" }, CAT_STATUS },
};

static void test_flag_model_table(void)
{
    char *repo_root;
    char *git_dir = make_tmp_repo(&repo_root);
    const char *paths[1] = { "f.txt" };
    const char *base_content[1] = { "base\n" };
    const char *head_content[1] = { "b1\n" };
    unsigned char base_tree[SG_SHA1_RAW_LEN], head_tree[SG_SHA1_RAW_LEN];
    unsigned char base_commit[SG_SHA1_RAW_LEN], head_commit[SG_SHA1_RAW_LEN];
    size_t i;

    build_tree(git_dir, paths, base_content, 1, base_tree);
    write_commit(git_dir, base_tree, NULL, 0, "base\n", 1700000000, base_commit);
    build_tree(git_dir, paths, head_content, 1, head_tree);
    {
        unsigned char parents[1][SG_SHA1_RAW_LEN];

        memcpy(parents[0], base_commit, SG_SHA1_RAW_LEN);
        write_commit(git_dir, head_tree, parents, 1, "b1\n", 1700000100, head_commit);
    }
    CHECK(sg_ref_update_branch(git_dir, "master", head_commit) == 0, "update branch");

    for (i = 0; i < sizeof(FLAG_ROWS) / sizeof(FLAG_ROWS[0]); i++) {
        const flag_row *row = &FLAG_ROWS[i];
        char *argv[8];
        int argc = 0;
        char out[65536];
        int rc;

        argv[argc++] = (char *)"show";
        {
            int j;

            for (j = 0; row->argv_tail[j] != NULL && j < 4; j++)
                argv[argc++] = (char *)row->argv_tail[j];
        }
        argv[argc] = NULL;

        rc = run_show_capture(repo_root, argc, argv, out, sizeof(out));

        if (row->expect == CAT_ERROR) {
            CHECK(rc == 1, "%s: expected exit 1 (error), got %d", row->label, rc);
        } else {
            out_category got;

            CHECK(rc == 0, "%s: expected exit 0, got %d, output:\n%s", row->label, rc, out);
            got = classify(out);
            CHECK(got == row->expect, "%s: expected %s, got %s, output:\n%s", row->label,
                 cat_name(row->expect), cat_name(got), out);
        }
    }
    /* CI's ubuntu ASan job runs with detect_leaks=1 and these two strdups
       are what it caught: macOS's ASan does no leak detection at all, and
       gates.sh --leaks cannot see them either -- a returned pointer still
       held in the caller's live frame is exactly the shape its conservative
       scan reports as reachable. */
    free(git_dir);
    free(repo_root);
}

/* ---- section 4.1: per-parent status letters ------------------------------ */

/* Two-parent merge reproducing the spec's own measured table:
     mod.txt         present p1, present p2, present result  -> MM
     delone.txt      absent  p1, present p2, present result  -> AM
     newbymerge.txt  absent  p1, absent  p2, present result  -> AA
     gone.txt        present p1, present p2, absent  result  -> DD
   Built directly from crafted trees (not through sg_merge_trees), the same
   way test_diff_combined.c and cmd_show.c's own render_merge_diff exercise
   sg_diff_combined_from_trees -- this is testing the RENDERER's letter
   computation, not merge resolution. */
static void test_merge_two_parent_letters(void)
{
    char *repo_root;
    char *git_dir = make_tmp_repo(&repo_root);
    unsigned char p1_tree[SG_SHA1_RAW_LEN], p2_tree[SG_SHA1_RAW_LEN], result_tree[SG_SHA1_RAW_LEN];
    unsigned char p1_commit[SG_SHA1_RAW_LEN], p2_commit[SG_SHA1_RAW_LEN], merge_commit[SG_SHA1_RAW_LEN];
    unsigned char parents[2][SG_SHA1_RAW_LEN];
    char out[65536];
    char *argv[4];
    char hex[SG_SHA1_HEX_LEN + 1];
    int rc;

    {
        const char *paths[2] = { "gone.txt", "mod.txt" };
        const char *contents[2] = { "p1\n", "p1\n" };

        build_tree(git_dir, paths, contents, 2, p1_tree);
    }
    {
        const char *paths[3] = { "delone.txt", "gone.txt", "mod.txt" };
        const char *contents[3] = { "p2\n", "p2\n", "p2\n" };

        build_tree(git_dir, paths, contents, 3, p2_tree);
    }
    {
        const char *paths[3] = { "delone.txt", "mod.txt", "newbymerge.txt" };
        const char *contents[3] = { "r\n", "r\n", "r\n" };

        build_tree(git_dir, paths, contents, 3, result_tree);
    }

    write_commit(git_dir, p1_tree, NULL, 0, "p1\n", 1700000000, p1_commit);
    write_commit(git_dir, p2_tree, NULL, 0, "p2\n", 1700000001, p2_commit);
    memcpy(parents[0], p1_commit, SG_SHA1_RAW_LEN);
    memcpy(parents[1], p2_commit, SG_SHA1_RAW_LEN);
    write_commit(git_dir, result_tree, parents, 2, "Merge\n", 1700000002, merge_commit);
    CHECK(sg_ref_update_branch(git_dir, "master", merge_commit) == 0, "update branch");

    sg_sha1_to_hex(merge_commit, hex);
    argv[0] = (char *)"show";
    argv[1] = (char *)"--name-status";
    argv[2] = hex;
    argv[3] = NULL;
    rc = run_show_capture(repo_root, 3, argv, out, sizeof(out));
    CHECK(rc == 0, "--name-status on 2-parent merge: expected exit 0, got %d:\n%s", rc, out);
    CHECK(strstr(out, "AM\tdelone.txt\n") != NULL, "delone.txt should be AM, got:\n%s", out);
    CHECK(strstr(out, "DD\tgone.txt\n") != NULL, "gone.txt should be DD, got:\n%s", out);
    CHECK(strstr(out, "MM\tmod.txt\n") != NULL, "mod.txt should be MM, got:\n%s", out);
    CHECK(strstr(out, "AA\tnewbymerge.txt\n") != NULL, "newbymerge.txt should be AA, got:\n%s",
         out);

    argv[1] = (char *)"--name-only";
    rc = run_show_capture(repo_root, 3, argv, out, sizeof(out));
    CHECK(rc == 0, "--name-only on 2-parent merge: expected exit 0, got %d:\n%s", rc, out);
    CHECK(strstr(out, "\ndelone.txt\n") != NULL, "--name-only missing delone.txt:\n%s", out);
    CHECK(strstr(out, "\ngone.txt\n") != NULL, "--name-only missing gone.txt:\n%s", out);
    CHECK(strstr(out, "\nmod.txt\n") != NULL, "--name-only missing mod.txt:\n%s", out);
    CHECK(strstr(out, "\nnewbymerge.txt\n") != NULL, "--name-only missing newbymerge.txt:\n%s",
         out);
    CHECK(strstr(out, "\t") == NULL, "--name-only must never print a tab, got:\n%s", out);
    /* see the note in the first test: CI-only leak detection caught these. */
    free(git_dir);
    free(repo_root);
}

/* Three-parent (octopus) merge whose dense set is non-empty: section 4.1's
   "MMM" case -- one letter per parent, not clamped at two. Also proves the
   refusal-skip: an ordinary format (here the default, patch) on the SAME
   commit must still be refused (section 4.1's octopus rule is exclusive to
   --name-only/--name-status), so this doubles as the regression pin for
   "the refusal check still fires when names_mode is false". */
static void test_merge_three_parent_mmm(void)
{
    char *repo_root;
    char *git_dir = make_tmp_repo(&repo_root);
    unsigned char p_tree[3][SG_SHA1_RAW_LEN];
    unsigned char result_tree[SG_SHA1_RAW_LEN];
    unsigned char p_commit[3][SG_SHA1_RAW_LEN];
    unsigned char parents[3][SG_SHA1_RAW_LEN];
    unsigned char merge_commit[SG_SHA1_RAW_LEN];
    char out[65536];
    char *argv[4];
    char hex[SG_SHA1_HEX_LEN + 1];
    int rc;
    int i;
    const char *paths[1] = { "file.txt" };

    for (i = 0; i < 3; i++) {
        char content[8];

        snprintf(content, sizeof(content), "p%d\n", i + 1);
        {
            const char *contents[1] = { content };

            build_tree(git_dir, paths, contents, 1, p_tree[i]);
        }
        {
            char msg[8];

            snprintf(msg, sizeof(msg), "p%d\n", i + 1);
            write_commit(git_dir, p_tree[i], NULL, 0, msg, 1700000010 + i, p_commit[i]);
        }
        memcpy(parents[i], p_commit[i], SG_SHA1_RAW_LEN);
    }
    {
        const char *contents[1] = { "r\n" };

        build_tree(git_dir, paths, contents, 1, result_tree);
    }
    write_commit(git_dir, result_tree, parents, 3, "Octopus merge\n", 1700000020, merge_commit);
    CHECK(sg_ref_update_branch(git_dir, "master", merge_commit) == 0, "update branch");

    sg_sha1_to_hex(merge_commit, hex);

    /* Regression half: no --name-only/--name-status, so this octopus's
       non-empty dense set must still be refused (Phase 55b's pre-existing
       rule, untouched by this phase). */
    argv[0] = (char *)"show";
    argv[1] = hex;
    argv[2] = NULL;
    rc = run_show_capture(repo_root, 2, argv, out, sizeof(out));
    CHECK(rc == 1, "plain `sg show` on a non-empty-dense octopus should still refuse, got %d:\n%s",
         rc, out);

    /* The new half: --name-status must NOT refuse, and must print one
       letter PER PARENT (MMM, not MM). */
    argv[1] = (char *)"--name-status";
    argv[2] = hex;
    argv[3] = NULL;
    rc = run_show_capture(repo_root, 3, argv, out, sizeof(out));
    CHECK(rc == 0, "--name-status on a 3-parent merge should not refuse, got %d:\n%s", rc, out);
    CHECK(strstr(out, "MMM\tfile.txt\n") != NULL,
         "expected \"MMM\\tfile.txt\", got:\n%s", out);
    /* see the note in the first test: CI-only leak detection caught these. */
    free(git_dir);
    free(repo_root);
}

int main(void)
{
    test_flag_model_table();
    test_merge_two_parent_letters();
    test_merge_three_parent_mmm();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all show tests passed\n");
    return 0;
}
