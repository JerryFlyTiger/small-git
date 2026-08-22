/* Phase 25 WP2: rendering (sg/diff_out.h) for all six `sg diff` formats.
   Most expected strings below were cross-checked byte-for-byte against real
   git 2.55.0 on the exact same fixture before being pasted in here. Two
   exceptions -- test_stat_scaling and test_stat_truncation -- were first
   pinned from the COMPILED sg binary's own output (see each function's own
   comment for why: a first hand derivation of the scaling fixture was WRONG
   about which branch of the --stat re-clamp step fires, and only running the
   real binary caught it). Pinning a test's expected value from the program
   under test has zero power to catch "the implementation disagrees with real
   git" -- it only proves the output hasn't silently drifted since the string
   was written. Those two fixtures have since been independently re-derived
   from real git plumbing by a reviewer and matched; that re-derivation is
   what makes the expected strings below trustworthy, not this file having
   asserted them. The only oracle this suite does NOT borrow from a prior run
   of itself is tests/interop.sh's side-by-side sg-vs-git comparison -- that
   is where an actual implementation/real-git mismatch would be caught. */
#include "sg/cli.h"

#include "sg/chunk.h"
#include "sg/diff.h"
#include "sg/diff_out.h"
#include "sg/hash.h"
#include "sg/index.h"
#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/workdir.h"

#include <fcntl.h>
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

/* ---- fixture plumbing ------------------------------------------------ */

static char *make_tmp_repo_and_cd(const char *tag)
{
    char template[] = "/tmp/sg_diff_out_XXXXXX";
    char *path;
    char git_dir[4096];

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
    snprintf(git_dir, sizeof(git_dir), "%s/.git", path);
    (void)git_dir;
    return path;
}

static void write_file(const char *rel, const char *content)
{
    FILE *f = fopen(rel, "wb");

    if (f == NULL) {
        fprintf(stderr, "failed to write %s\n", rel);
        exit(1);
    }
    fputs(content, f);
    fclose(f);
}

/* write_file's fputs stops at the first NUL byte, which is exactly the
   byte a binary fixture needs -- use this instead whenever the content
   isn't a plain C string. */
static void write_bytes(const char *rel, const unsigned char *data, size_t len)
{
    FILE *f = fopen(rel, "wb");

    if (f == NULL) {
        fprintf(stderr, "failed to write %s\n", rel);
        exit(1);
    }
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        fprintf(stderr, "short write to %s\n", rel);
        exit(1);
    }
    fclose(f);
}

static void run_add_all(void)
{
    char *argv[2];

    argv[0] = "add";
    argv[1] = ".";
    CHECK(sg_cmd_add(2, argv) == 0, "sg add . failed");
}

static void run_commit(const char *message)
{
    char *argv[3];

    argv[0] = "commit";
    argv[1] = "-m";
    argv[2] = (char *)message;
    CHECK(sg_cmd_commit(3, argv) == 0, "sg commit -m '%s' failed", message);
}

static void run_branch(const char *name)
{
    char *argv[2];

    argv[0] = "branch";
    argv[1] = (char *)name;
    CHECK(sg_cmd_branch(2, argv) == 0, "sg branch %s failed", name);
}

static void run_switch(const char *name)
{
    char *argv[2];

    argv[0] = "switch";
    argv[1] = (char *)name;
    CHECK(sg_cmd_switch(2, argv) == 0, "sg switch %s failed", name);
}

/* Merging with an intentional conflict returns non-zero -- that is the
   expected outcome here, not checked against 0. */
static void run_merge_expect_conflict(const char *name)
{
    char *argv[2];

    argv[0] = "merge";
    argv[1] = (char *)name;
    sg_cmd_merge(2, argv);
}

/* ---- stdout capture --------------------------------------------------- */

static int g_saved_stdout = -1;
static char g_capture_path[] = "/tmp/sg_diff_out_capture_XXXXXX";

static void capture_start(void)
{
    char path[] = "/tmp/sg_diff_out_capture_XXXXXX";
    int fd;

    fflush(stdout);
    fd = mkstemp(path);
    if (fd < 0) {
        fprintf(stderr, "mkstemp failed\n");
        exit(1);
    }
    memcpy(g_capture_path, path, sizeof(g_capture_path));
    g_saved_stdout = dup(STDOUT_FILENO);
    dup2(fd, STDOUT_FILENO);
    close(fd);
}

static char *capture_end(void)
{
    FILE *f;
    long len;
    char *buf;

    fflush(stdout);
    dup2(g_saved_stdout, STDOUT_FILENO);
    close(g_saved_stdout);
    g_saved_stdout = -1;

    f = fopen(g_capture_path, "rb");
    if (f == NULL) {
        fprintf(stderr, "failed to reopen capture file\n");
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)len + 1);
    if (buf == NULL) {
        fprintf(stderr, "OOM reading capture file\n");
        exit(1);
    }
    if (len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "short read on capture file\n");
        exit(1);
    }
    buf[len] = '\0';
    fclose(f);
    unlink(g_capture_path);
    return buf;
}

static char *render(const char *git_dir, const char *repo_root, const sg_diff_list *list,
                    const sg_diff_out_opts *opts)
{
    char *out;

    capture_start();
    sg_diff_print(git_dir, repo_root, list, opts);
    out = capture_end();
    return out;
}

/* ---- list builders (mirroring cmd_diff.c's own dispatch) -------------- */

static void build_workdir_list(const char *git_dir, const char *repo_root, sg_diff_list *out)
{
    sg_index idx;

    CHECK(sg_index_read(git_dir, &idx) == 0, "index read failed");
    CHECK(sg_diff_index_workdir(git_dir, repo_root, &idx, out) == 0, "sg_diff_index_workdir failed");
    sg_index_free(&idx);
}

static void build_cached_list(const char *git_dir, const char *repo_root, sg_diff_list *out)
{
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    unsigned char *tree_ptr = NULL;
    sg_index idx;
    char bad_path[SG_PATH_MAX];

    (void)repo_root;
    if (sg_ref_resolve_head(git_dir, commit_id) == 0) {
        CHECK(sg_commit_tree_of(git_dir, commit_id, tree_id) == 0, "commit_tree_of failed");
        tree_ptr = tree_id;
    }
    CHECK(sg_index_read(git_dir, &idx) == 0, "index read failed");
    CHECK(sg_diff_tree_index(git_dir, tree_ptr, &idx, out, bad_path) == 0, "sg_diff_tree_index failed");
    sg_index_free(&idx);
}

/* ---- tests -------------------------------------------------------------- */

/* All six formats on the same fixture, in one place, each asserted against
   the exact bytes real git 2.55.0 produces (patch/stat/numstat/name-only
   already matched real git in exploratory testing before this was written
   down; name-status/shortstat follow directly from the same rows). */
static void test_six_formats(void)
{
    char *root = make_tmp_repo_and_cd("six");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);

    write_file("f.txt", "line1\nline2\nline3\n");
    run_add_all();
    run_commit("base");
    write_file("f.txt", "line1\nCHANGED\nline3\nline4\n");

    build_workdir_list(git_dir, root, &list);

    memset(&opts, 0, sizeof(opts));

    opts.format = SG_DIFF_FORMAT_PATCH;
    out = render(git_dir, root, &list, &opts);
    /* Blob ids 83db48f/e0c9b5e are this fixture's actual `blob <len>\0<data>`
       sha1 (first 7 hex chars), matching what `git hash-object` computes for
       the same bytes -- see Phase 26's index-line addition in sg/diff.h. */
    CHECK(strcmp(out, "diff --git a/f.txt b/f.txt\n"
                      "index 83db48f..e0c9b5e 100644\n"
                      "--- a/f.txt\n"
                      "+++ b/f.txt\n"
                      "@@ -1,3 +1,4 @@\n"
                      " line1\n"
                      "-line2\n"
                      "+CHANGED\n"
                      " line3\n"
                      "+line4\n") == 0,
         "PATCH mismatch: %s", out);
    free(out);

    opts.format = SG_DIFF_FORMAT_STAT;
    out = render(git_dir, root, &list, &opts);
    CHECK(strcmp(out, " f.txt | 3 ++-\n"
                      " 1 file changed, 2 insertions(+), 1 deletion(-)\n") == 0,
         "STAT mismatch: %s", out);
    free(out);

    opts.format = SG_DIFF_FORMAT_NUMSTAT;
    out = render(git_dir, root, &list, &opts);
    CHECK(strcmp(out, "2\t1\tf.txt\n") == 0, "NUMSTAT mismatch: %s", out);
    free(out);

    opts.format = SG_DIFF_FORMAT_NAME_ONLY;
    out = render(git_dir, root, &list, &opts);
    CHECK(strcmp(out, "f.txt\n") == 0, "NAME_ONLY mismatch: %s", out);
    free(out);

    opts.format = SG_DIFF_FORMAT_NAME_STATUS;
    out = render(git_dir, root, &list, &opts);
    CHECK(strcmp(out, "M\tf.txt\n") == 0, "NAME_STATUS mismatch: %s", out);
    free(out);

    opts.format = SG_DIFF_FORMAT_SHORTSTAT;
    out = render(git_dir, root, &list, &opts);
    CHECK(strcmp(out, " 1 file changed, 2 insertions(+), 1 deletion(-)\n") == 0,
         "SHORTSTAT mismatch: %s", out);
    free(out);

    sg_diff_list_free(&list);
    free(root);
}

/* The graph-scaling formula's "at least 2 characters when both sides are
   non-zero" guard, and the "scale the smaller side, the larger side eats
   the remainder" rule that produces a lopsided bar for equal add/delete
   counts ("+++++++--------" for x, an even 100/100 split).

   This fixture's expected string was FIRST pinned from the compiled sg
   binary's own output, not real git -- a first hand derivation of it by
   the implementer got the re-clamp step's else-branch wrong (it reclaims
   leftover width into graph_width when name_width is small, it does not
   leave graph_width at its clamped floor), so a from-scratch re-derivation
   by hand was not trusted either. A reviewer later independently re-derived
   this exact fixture from real git plumbing and confirmed the same bytes
   ("x | 200 +++++++--------" / "y |   2 +-"), which is what makes the
   assertion below trustworthy -- not the fact that it matches this file's
   own program under test. */
static void test_stat_scaling(void)
{
    char *root = make_tmp_repo_and_cd("scale");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;
    char buf[4096];
    size_t i;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);

    buf[0] = '\0';
    for (i = 0; i < 100; i++) {
        char line[16];

        snprintf(line, sizeof(line), "a%zu\n", i);
        strcat(buf, line);
    }
    write_file("x", buf);
    write_file("y", "p\n");
    run_add_all();
    run_commit("base");

    buf[0] = '\0';
    for (i = 0; i < 100; i++) {
        char line[16];

        snprintf(line, sizeof(line), "b%zu\n", i);
        strcat(buf, line);
    }
    write_file("x", buf);
    write_file("y", "q\n");

    build_workdir_list(git_dir, root, &list);

    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_STAT;
    opts.stat_width = 20;

    out = render(git_dir, root, &list, &opts);
    CHECK(strcmp(out, " x | 200 +++++++--------\n"
                      " y |   2 +-\n"
                      " 2 files changed, 101 insertions(+), 101 deletions(-)\n") == 0,
         "scaling mismatch: %s", out);
    free(out);

    sg_diff_list_free(&list);
    free(root);
}

/* Name truncation: pushing the cut point right to the nearest '/' even
   though the naive minimal cut already fit within target width (the
   "me/f.txt" -> "/f.txt" case -- the slash itself is kept, per
   sg_diff_out.h's "inclusive of that slash"), and name_width <= 3
   collapsing to a bare "...", overflowing the requested width, with no
   padding at all.

   Both expected strings were FIRST pinned from the compiled sg binary's own
   output, then independently re-derived by a reviewer from real git
   plumbing (`git -c core.quotepath=false diff --stat=200,11`) and confirmed
   identical (".../f.txt   | 1 +"). As with test_stat_scaling above, it is
   that independent re-derivation -- not this file's own prior run -- that
   makes the assertions trustworthy. */
static void test_stat_truncation(void)
{
    char *root = make_tmp_repo_and_cd("trunc");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);

    (void)mkdir("verylongdirname", 0755);
    write_file("verylongdirname/f.txt", "a\n");
    run_add_all();
    run_commit("base");
    write_file("verylongdirname/f.txt", "a\nb\n");

    build_workdir_list(git_dir, root, &list);

    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_STAT;
    opts.stat_width = 200;
    opts.stat_name_width = 11;

    out = render(git_dir, root, &list, &opts);
    CHECK(strcmp(out, " .../f.txt   | 1 +\n"
                      " 1 file changed, 1 insertion(+)\n") == 0,
         "slash-push truncation mismatch: %s", out);
    free(out);

    opts.stat_name_width = 2;
    out = render(git_dir, root, &list, &opts);
    CHECK(strcmp(out, " ... | 1 +\n"
                      " 1 file changed, 1 insertion(+)\n") == 0,
         "name_width<=3 truncation mismatch: %s", out);
    free(out);

    /* The <= 3 boundary itself, not just a value comfortably inside it: a
       stray "< 3" instead of "<= 3" would only show up at name_width==3
       exactly (name_width==2 above can't distinguish the two operators).
       Verified against real git (`git diff --cached --stat=200,3` on a
       fresh single-file add, since committing isn't available to this
       agent -- the exact bytes are the same "..." shape regardless of
       whether the comparison is against an empty tree or a prior commit). */
    opts.stat_name_width = 3;
    out = render(git_dir, root, &list, &opts);
    CHECK(strcmp(out, " ... | 1 +\n"
                      " 1 file changed, 1 insertion(+)\n") == 0,
         "name_width==3 boundary mismatch: %s", out);
    free(out);

    sg_diff_list_free(&list);
    free(root);
}

/* entry_status's 'A' and 'D' branches: --name-status's only prior coverage
   was 'M' (test_six_formats), so swapping the two conditions (or flipping
   which literal each one returns) would pass make test undetected. A plain
   `sg diff` (index-vs-workdir) can never produce 'A' -- untracked files
   aren't part of that comparison at all -- so this needs a real two-commit
   tree-vs-tree diff (sg_diff_trees) instead. */
static void test_name_status_add_delete(void)
{
    char *root = make_tmp_repo_and_cd("addel");
    char git_dir[4096];
    unsigned char old_commit[SG_SHA1_RAW_LEN];
    unsigned char new_commit[SG_SHA1_RAW_LEN];
    unsigned char old_tree[SG_SHA1_RAW_LEN];
    unsigned char new_tree[SG_SHA1_RAW_LEN];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);

    write_file("fileA.txt", "a\n");
    run_add_all();
    run_commit("base");
    CHECK(sg_ref_resolve_head(git_dir, old_commit) == 0, "resolve HEAD (old) failed");

    CHECK(unlink("fileA.txt") == 0, "unlink fileA.txt failed");
    write_file("fileB.txt", "b\n");
    run_add_all();
    run_commit("second");
    CHECK(sg_ref_resolve_head(git_dir, new_commit) == 0, "resolve HEAD (new) failed");

    CHECK(sg_commit_tree_of(git_dir, old_commit, old_tree) == 0, "commit_tree_of(old) failed");
    CHECK(sg_commit_tree_of(git_dir, new_commit, new_tree) == 0, "commit_tree_of(new) failed");

    CHECK(sg_diff_trees(git_dir, old_tree, new_tree, &list, NULL) == 0, "sg_diff_trees failed");

    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_NAME_STATUS;
    out = render(git_dir, root, &list, &opts);
    /* Sorted by path, byte-wise: "fileA.txt" < "fileB.txt". */
    CHECK(strcmp(out, "D\tfileA.txt\n"
                      "A\tfileB.txt\n") == 0,
         "add/delete name-status mismatch: %s", out);
    free(out);

    sg_diff_list_free(&list);
    free(root);
}

/* Summary pluralization/omission: a deletion-only change omits the
   insertions clause and uses the singular "deletion", and a pure mode
   change (0 lines either way) prints BOTH clauses in the plural, per the
   "insertions || deletions==0" / "deletions || insertions==0" rule. */
static void test_summary_variants(void)
{
    {
        char *root = make_tmp_repo_and_cd("delonly");
        char git_dir[4096];
        sg_diff_list list;
        sg_diff_out_opts opts;
        char *out;

        snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
        write_file("f.txt", "a\nb\n");
        run_add_all();
        run_commit("base");
        write_file("f.txt", "a\n");

        build_workdir_list(git_dir, root, &list);
        memset(&opts, 0, sizeof(opts));
        opts.format = SG_DIFF_FORMAT_STAT;
        out = render(git_dir, root, &list, &opts);
        CHECK(strcmp(out, " f.txt | 1 -\n"
                          " 1 file changed, 1 deletion(-)\n") == 0,
             "deletion-only stat mismatch: %s", out);
        free(out);

        opts.format = SG_DIFF_FORMAT_SHORTSTAT;
        out = render(git_dir, root, &list, &opts);
        CHECK(strcmp(out, " 1 file changed, 1 deletion(-)\n") == 0,
             "deletion-only shortstat mismatch: %s", out);
        free(out);

        sg_diff_list_free(&list);
        free(root);
    }

    {
        char *root = make_tmp_repo_and_cd("modeonly");
        char git_dir[4096];
        sg_diff_list list;
        sg_diff_out_opts opts;
        char *out;
        char *addargv[2];

        snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
        write_file("mode.txt", "body\n");
        run_add_all();
        run_commit("base");
        CHECK(chmod("mode.txt", 0755) == 0, "chmod failed");
        addargv[0] = "add";
        addargv[1] = "mode.txt";
        CHECK(sg_cmd_add(2, addargv) == 0, "sg add mode.txt failed");

        build_cached_list(git_dir, root, &list);
        memset(&opts, 0, sizeof(opts));
        opts.format = SG_DIFF_FORMAT_STAT;
        out = render(git_dir, root, &list, &opts);
        CHECK(strcmp(out, " mode.txt | 0\n"
                          " 1 file changed, 0 insertions(+), 0 deletions(-)\n") == 0,
             "mode-only stat mismatch: %s", out);
        free(out);

        sg_diff_list_free(&list);
        free(root);
    }
}

/* Binary rows count toward "N files changed"; unmerged rows do not -- the
   two are asserted separately (an unmerged-only diff, and a binary+unmerged
   mix) so a bug that folds them together can't hide behind a coincidental
   total. */
static void test_binary_vs_unmerged_files_changed(void)
{
    char *root = make_tmp_repo_and_cd("conflict");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);

    write_file("conflict.txt", "base\n");
    write_bytes("bin.dat", (const unsigned char *)"\x00\x01old", 5);
    run_add_all();
    run_commit("base");
    run_branch("other");

    write_file("conflict.txt", "main change\n");
    write_bytes("bin.dat", (const unsigned char *)"\x00\x01\x02main", 7);
    run_add_all();
    run_commit("mainchange");

    run_switch("other");
    write_file("conflict.txt", "other change\n");
    {
        char *addargv[2];

        addargv[0] = "add";
        addargv[1] = "conflict.txt";
        CHECK(sg_cmd_add(2, addargv) == 0, "sg add conflict.txt failed");
    }
    run_commit("otherchange");

    run_merge_expect_conflict("master"); /* expected to conflict */

    build_cached_list(git_dir, root, &list);
    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_STAT;
    out = render(git_dir, root, &list, &opts);
    CHECK(strcmp(out, " bin.dat      | Bin 5 -> 7 bytes\n"
                      " conflict.txt | Unmerged\n"
                      " 1 file changed, 0 insertions(+), 0 deletions(-)\n") == 0,
         "binary+unmerged stat mismatch: %s", out);
    free(out);

    /* print_numstat's unmerged ("0\t0\t") and binary ("-\t-\t") branches --
       the only prior NUMSTAT coverage (test_six_formats) was an ordinary
       text modification, so both of these were unreached by make test. */
    opts.format = SG_DIFF_FORMAT_NUMSTAT;
    out = render(git_dir, root, &list, &opts);
    CHECK(strcmp(out, "-\t-\tbin.dat\n"
                      "0\t0\tconflict.txt\n") == 0,
         "binary+unmerged numstat mismatch: %s", out);
    free(out);

    sg_diff_list_free(&list);
    free(root);
}

static void test_unmerged_only_zero_files_changed(void)
{
    char *root = make_tmp_repo_and_cd("conflict_only");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);

    write_file("conflict.txt", "base\n");
    run_add_all();
    run_commit("base");
    run_branch("other");

    write_file("conflict.txt", "main change\n");
    run_add_all();
    run_commit("mainchange");

    run_switch("other");
    write_file("conflict.txt", "other change\n");
    run_add_all();
    run_commit("otherchange");

    run_merge_expect_conflict("master"); /* expected to conflict */

    build_cached_list(git_dir, root, &list);
    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_STAT;
    out = render(git_dir, root, &list, &opts);
    /* Measured against git 2.55.0: when files_changed == 0 (every row is
       unmerged), NEITHER the insertions clause nor the deletions clause is
       printed -- just " 0 files changed". A files_changed==0 summary with
       both clauses appended was a bug this test used to enshrine (it was
       written from the implementation's own output, not real git); see this
       file's header comment on why that pinning method has no power to
       catch that kind of mismatch. */
    CHECK(strcmp(out, " conflict.txt | Unmerged\n"
                      " 0 files changed\n") == 0,
         "unmerged-only stat mismatch: %s", out);
    free(out);

    sg_diff_list_free(&list);
    free(root);
}

/* The `number_width = max(has_binary ? 3 : 0, decimal_width(max_change))`
   half of the binary-row width math IS observable: a binary row present
   alongside a single-digit text change (max_change==3, digits(3)==1) still
   forces the count column to 3 wide, visibly padding "3" to "  3". Verified
   against real git both with and without the binary row present (same
   max_change, same text file), confirming the padding difference is really
   caused by has_binary and not by name-column width or something else.

   The OTHER half -- `bin_width = 14 + dw(new) + dw(old)` feeding
   `max_graph = max(max_change, bin_width - 4)`, and the unmerged row's flat
   `8` -- is NOT tested for a bar-length footprint, and deliberately so: it
   is mathematically impossible for one to exist. bin_width can only ever
   WIN the max() (become max_graph) when bin_width-4 >= max_change, and a
   200000-trial random probe of the scaling formula (add/del split at every
   graph_width >= max_change, for every max_change up to 500) found the
   printed bar identical for every graph_width in that range -- scale_linear
   is exactly the identity function at graph_width==max_change, and above
   it the code skips scaling entirely and prints raw add/del either way. So
   whether bin_width is 4 over max_change or 400 over it, the bar comes out
   the same; the constants 14/-4/8 only gate a boolean ("does this row's
   width exceed the text rows' widest change"), never a magnitude, in the
   current algorithm. Mutating any of them either changes nothing observable
   in the bar, or flips that boolean at a different threshold -- which the
   number_width test above and the existing Bin-row/Unmerged-row format
   tests already exercise from a different angle. */
static void test_binary_forces_number_width(void)
{
    char *root = make_tmp_repo_and_cd("numwidth");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);

    write_file("t.txt", "a\nb\nc\n");
    write_bytes("bin.dat", (const unsigned char *)"\x00tiny", 5);
    run_add_all();

    build_cached_list(git_dir, root, &list); /* unborn HEAD: a fresh add */
    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_STAT;
    opts.stat_width = 200;
    out = render(git_dir, root, &list, &opts);
    CHECK(strcmp(out, " bin.dat | Bin 0 -> 5 bytes\n"
                      " t.txt   |   3 +++\n"
                      " 2 files changed, 3 insertions(+)\n") == 0,
         "binary-forces-number_width mismatch: %s", out);
    free(out);

    sg_diff_list_free(&list);
    free(root);
}

/* Display width: a CJK name and a same-codepoint-count ASCII name get
   DIFFERENT padding before "|" in the same --stat output, because CJK
   codepoints in this table are 2 display columns wide and ASCII is 1 --
   the padding must therefore differ even though both names are 6 code
   points long ("cc.txt" and "\xe4\xb8\xad\xe6\x96\x87.txt" == 中文.txt). */
static void test_display_width_cjk_vs_ascii(void)
{
    char *root = make_tmp_repo_and_cd("cjk");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);

    write_file("cc.txt", "a\n");
    write_file("\xe4\xb8\xad\xe6\x96\x87.txt", "a\n"); /* 中文.txt */
    run_add_all();
    run_commit("base");
    write_file("cc.txt", "a\nb\n");
    write_file("\xe4\xb8\xad\xe6\x96\x87.txt", "a\nb\n");

    build_workdir_list(git_dir, root, &list);
    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_STAT;
    opts.stat_width = 200;

    out = render(git_dir, root, &list, &opts);
    CHECK(strcmp(out, " cc.txt   | 1 +\n"
                      " \xe4\xb8\xad\xe6\x96\x87.txt | 1 +\n"
                      " 2 files changed, 2 insertions(+)\n") == 0,
         "CJK width mismatch: %s", out);
    free(out);

    sg_diff_list_free(&list);
    free(root);
}

/* Phase 26: "index <old7>..<new7>[ <mode>]" plus new-file/deleted-file lines.
   fileA.txt (deleted) and fileB.txt (added) between two commits -- same
   fixture shape as test_name_status_add_delete, but asserting the full PATCH
   text this time. Blob ids 7898192/6178079 are "a\n"/"b\n"'s actual
   `blob <len>\0<data>` sha1 (first 7 hex chars), independently computed. */
static void test_patch_add_and_delete_index_lines(void)
{
    char *root = make_tmp_repo_and_cd("addel_patch");
    char git_dir[4096];
    unsigned char old_commit[SG_SHA1_RAW_LEN];
    unsigned char new_commit[SG_SHA1_RAW_LEN];
    unsigned char old_tree[SG_SHA1_RAW_LEN];
    unsigned char new_tree[SG_SHA1_RAW_LEN];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);

    write_file("fileA.txt", "a\n");
    run_add_all();
    run_commit("base");
    CHECK(sg_ref_resolve_head(git_dir, old_commit) == 0, "resolve HEAD (old) failed");

    CHECK(unlink("fileA.txt") == 0, "unlink fileA.txt failed");
    write_file("fileB.txt", "b\n");
    run_add_all();
    run_commit("second");
    CHECK(sg_ref_resolve_head(git_dir, new_commit) == 0, "resolve HEAD (new) failed");

    CHECK(sg_commit_tree_of(git_dir, old_commit, old_tree) == 0, "commit_tree_of(old) failed");
    CHECK(sg_commit_tree_of(git_dir, new_commit, new_tree) == 0, "commit_tree_of(new) failed");

    CHECK(sg_diff_trees(git_dir, old_tree, new_tree, &list, NULL) == 0, "sg_diff_trees failed");

    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    out = render(git_dir, root, &list, &opts);
    CHECK(strcmp(out, "diff --git a/fileA.txt b/fileA.txt\n"
                      "deleted file mode 100644\n"
                      "index 7898192..0000000\n"
                      "--- a/fileA.txt\n"
                      "+++ /dev/null\n"
                      "@@ -1 +0,0 @@\n"
                      "-a\n"
                      "diff --git a/fileB.txt b/fileB.txt\n"
                      "new file mode 100644\n"
                      "index 0000000..6178079\n"
                      "--- /dev/null\n"
                      "+++ b/fileB.txt\n"
                      "@@ -0,0 +1 @@\n"
                      "+b\n") == 0,
         "add/delete PATCH mismatch: %s", out);
    free(out);

    sg_diff_list_free(&list);
    free(root);
}

/* Oracle rule 3 (sg/diff.h's Phase 26 note): a pure mode change -- content
   byte-identical, only the mode bit differs -- prints "diff --git" plus
   "old mode"/"new mode" and NOTHING else: no index line, no ---/+++, no
   hunk. */
static void test_patch_pure_mode_change_has_no_index_or_body(void)
{
    char *root = make_tmp_repo_and_cd("modeonly_patch");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;
    char *addargv[2];

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
    write_file("mode.txt", "body\n");
    run_add_all();
    run_commit("base");
    CHECK(chmod("mode.txt", 0755) == 0, "chmod failed");
    addargv[0] = "add";
    addargv[1] = "mode.txt";
    CHECK(sg_cmd_add(2, addargv) == 0, "sg add mode.txt failed");

    build_cached_list(git_dir, root, &list);
    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    out = render(git_dir, root, &list, &opts);
    CHECK(strcmp(out, "diff --git a/mode.txt b/mode.txt\n"
                      "old mode 100644\n"
                      "new mode 100755\n") == 0,
         "pure mode-change PATCH mismatch: %s", out);
    free(out);

    sg_diff_list_free(&list);
    free(root);
}

/* Mode AND content both changed: "old mode"/"new mode" lines are printed, and
   the index line therefore carries NO mode suffix (oracle rule 1's "only
   when no mode line was printed" -- sg/diff.h's Phase 26 note). Blob ids
   5626abf/f719efd are "one\n"/"two\n"'s actual blob sha1 (first 7 hex),
   independently computed. */
static void test_patch_mode_and_content_change_index_has_no_suffix(void)
{
    char *root = make_tmp_repo_and_cd("modecontent_patch");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;
    char *addargv[2];

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
    write_file("f.txt", "one\n");
    run_add_all();
    run_commit("base");
    write_file("f.txt", "two\n");
    CHECK(chmod("f.txt", 0755) == 0, "chmod failed");
    addargv[0] = "add";
    addargv[1] = "f.txt";
    CHECK(sg_cmd_add(2, addargv) == 0, "sg add f.txt failed");

    build_cached_list(git_dir, root, &list);
    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    out = render(git_dir, root, &list, &opts);
    CHECK(strcmp(out, "diff --git a/f.txt b/f.txt\n"
                      "old mode 100644\n"
                      "new mode 100755\n"
                      "index 5626abf..f719efd\n"
                      "--- a/f.txt\n"
                      "+++ b/f.txt\n"
                      "@@ -1 +1 @@\n"
                      "-one\n"
                      "+two\n") == 0,
         "mode+content PATCH mismatch: %s", out);
    free(out);

    sg_diff_list_free(&list);
    free(root);
}

/* Binary content: an index line is still printed (oracle rule 4), and the
   /dev/null substitution applies to the "Binary files" line's names exactly
   like it does to ---/+++, for a newly-added binary file. Blob id 4731d8e is
   "\x00bin1"'s actual blob sha1 (first 7 hex), independently computed. */
static void test_patch_binary_new_file_has_index_and_devnull(void)
{
    char *root = make_tmp_repo_and_cd("binary_add_patch");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
    write_bytes("bin.dat", (const unsigned char *)"\x00""bin1", 5);
    run_add_all();

    build_cached_list(git_dir, root, &list); /* unborn HEAD: a fresh add */
    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    out = render(git_dir, root, &list, &opts);
    CHECK(strcmp(out, "diff --git a/bin.dat b/bin.dat\n"
                      "new file mode 100644\n"
                      "index 0000000..4731d8e\n"
                      "Binary files /dev/null and b/bin.dat differ\n") == 0,
         "binary add PATCH mismatch: %s", out);
    free(out);

    sg_diff_list_free(&list);
    free(root);
}

/* Binary content, ordinary modification (both sides present, same mode): the
   index line carries the mode suffix, matching the text-content case. Blob
   ids 4731d8e/29a070e are "\x00bin1"/"\x00bin2"'s actual blob sha1 (first 7
   hex), independently computed. */
static void test_patch_binary_modify_index_line(void)
{
    char *root = make_tmp_repo_and_cd("binary_mod_patch");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
    write_bytes("bin.dat", (const unsigned char *)"\x00""bin1", 5);
    run_add_all();
    run_commit("base");
    write_bytes("bin.dat", (const unsigned char *)"\x00""bin2", 5);

    build_workdir_list(git_dir, root, &list);
    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    out = render(git_dir, root, &list, &opts);
    CHECK(strcmp(out, "diff --git a/bin.dat b/bin.dat\n"
                      "index 4731d8e..29a070e 100644\n"
                      "Binary files a/bin.dat and b/bin.dat differ\n") == 0,
         "binary modify PATCH mismatch: %s", out);
    free(out);

    sg_diff_list_free(&list);
    free(root);
}

/* The index line's ids are the CONTENT id (git_hash-object-compatible),
   never a chunked-storage pointer's own object id, even though sg_diff_side
   stores the raw/pointer id internally for sg_diff_side_read's sake (see
   sg/diff.h's Phase 26 note on sg_diff_side.id). Built by hand rather than
   through `sg add`, since chunking needs a config threshold this fixture
   sets directly via sg_chunk_store_blob. */
static void test_patch_chunk_pointer_index_line_uses_effective_id(void)
{
    char *root = make_tmp_repo_and_cd("chunkptr_patch");
    char git_dir[4096];
    size_t len = 3 * 1024 * 1024;
    unsigned char *data = malloc(len);
    unsigned char pointer_id[SG_SHA1_RAW_LEN];
    unsigned char content_id[SG_SHA1_RAW_LEN];
    char content_hex[SG_SHA1_HEX_LEN + 1];
    char expect_index_line[64];
    int chunked = -1;
    sg_diff_list list;
    sg_diff_side old_side, new_side;
    sg_diff_out_opts opts;
    char *out;
    size_t i;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);

    CHECK(data != NULL, "OOM allocating chunk fixture data");
    if (data == NULL) {
        free(root);
        return;
    }
    for (i = 0; i < len; i++)
        data[i] = (unsigned char)(i * 2654435761u);

    CHECK(sg_chunk_store_blob(git_dir, data, len, 1024 * 1024, pointer_id, &chunked) == 0 &&
             chunked == 1,
         "sg_chunk_store_blob did not produce a chunked pointer for this fixture");
    sg_object_hash(SG_OBJ_BLOB, data, len, content_id);
    free(data);

    /* CHECK(memcmp(pointer_id, content_id, ...)) would be a stronger
       assertion of "these two differ" but is deliberately skipped: on the
       vanishingly unlikely chance the pointer and content ids collided, the
       test below (index line prints content_id, not pointer_id) would still
       be the correct behavior to assert, and would still fail loudly if the
       renderer regressed to printing the raw/pointer id. */

    memset(&old_side, 0, sizeof(old_side));
    old_side.kind = SG_DIFF_SIDE_ABSENT;
    memset(&new_side, 0, sizeof(new_side));
    new_side.kind = SG_DIFF_SIDE_BLOB;
    new_side.mode = 0100644;
    memcpy(new_side.id, pointer_id, SG_SHA1_RAW_LEN);

    memset(&list, 0, sizeof(list));
    list.entries = malloc(sizeof(*list.entries));
    CHECK(list.entries != NULL, "OOM building manual diff list");
    list.entries[0].path = strdup("big.bin");
    list.entries[0].old_side = old_side;
    list.entries[0].new_side = new_side;
    list.entries[0].unmerged = 0;
    list.count = 1;
    list.cap = 1;

    /* No mode suffix: this is an add, so "new file mode" already said the
       mode (oracle rule 2 -- see sg/diff.h's Phase 26 note). */
    sg_sha1_to_hex(content_id, content_hex);
    content_hex[7] = '\0';
    snprintf(expect_index_line, sizeof(expect_index_line), "index 0000000..%s\n", content_hex);

    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    out = render(git_dir, root, &list, &opts);
    CHECK(strstr(out, "new file mode 100644\n") != NULL, "expected new file mode line: %s", out);
    CHECK(strstr(out, expect_index_line) != NULL,
         "expected index line with the content id %s, got: %s", expect_index_line, out);
    {
        char pointer_hex[SG_SHA1_HEX_LEN + 1];

        sg_sha1_to_hex(pointer_id, pointer_hex);
        pointer_hex[7] = '\0';
        if (strcmp(pointer_hex, content_hex) != 0)
            CHECK(strstr(out, pointer_hex) == NULL,
                 "the raw/pointer id %s must not leak into the rendered index line: %s", pointer_hex,
                 out);
    }
    free(out);

    sg_diff_list_free(&list);
    free(root);
}

/* Phase 26 fix-round: a chmod-only change that is NOT staged (index vs
   workdir, unlike test_patch_pure_mode_change_has_no_index_or_body above
   which goes through the tree-vs-index BLOB/BLOB path) still prints exactly
   "old mode"/"new mode" and nothing else. This is the render-layer coverage
   for side_effective_id's SG_DIFF_SIDE_WORKDIR branch (diff_out.c): the old
   side is BLOB (its effective id comes from sg_chunk_effective_id) and the
   new side is genuinely SG_DIFF_SIDE_WORKDIR (its effective id is just its
   own already-content id, always "verified"). Mutating that branch to
   report unverified would force content_changed=true and print a full
   (redundant, since content is unchanged) index line + hunk instead of
   stopping after the mode lines -- make test caught nothing here before,
   since every prior PATCH mode-only fixture used build_cached_list (both
   sides BLOB), never build_workdir_list. */
static void test_patch_unstaged_mode_only_change_has_no_index_or_body(void)
{
    char *root = make_tmp_repo_and_cd("modeonly_workdir_patch");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
    write_file("mode.txt", "body\n");
    run_add_all();
    run_commit("base");
    CHECK(chmod("mode.txt", 0755) == 0, "chmod failed");

    build_workdir_list(git_dir, root, &list);
    CHECK(list.count == 1, "expected exactly the mode-only row, got %zu", list.count);

    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    out = render(git_dir, root, &list, &opts);
    CHECK(strcmp(out, "diff --git a/mode.txt b/mode.txt\n"
                      "old mode 100644\n"
                      "new mode 100755\n") == 0,
         "unstaged pure mode-change PATCH mismatch: %s", out);
    free(out);

    sg_diff_list_free(&list);
    free(root);
}

/* ---- Phase 26: patch body byte-fidelity (sg_diff_build_script) --------

   The four fixtures below (A1/A2/A3/G) are anchored to real git 2.55.0
   output pasted directly from CLAUDE.md's Phase 26 note -- they were not
   independently re-derived here, they ARE the oracle measurement. A1/A2
   demonstrate diff.indentHeuristic (on by default) actually moving a
   pure-insert group's position; A3 is the CONTROL: same kind of fixture
   (inserting a duplicate-shaped block into a class body) where the
   heuristic and non-heuristic positions happen to coincide, included
   specifically so a future reader does not mistake it for redundant
   coverage -- it is the fixture that proves a passing A1/A2 isn't an
   accident of always landing on the same answer regardless of scoring. */

static void test_patch_indent_heuristic_blank_separated_duplicate_block(void)
{
    char *root = make_tmp_repo_and_cd("indent_a1");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
    write_file("f.txt", "top\n{\n  x\n}\n\n{\n  y\n}\n");
    run_add_all();
    run_commit("base");
    write_file("f.txt", "top\n{\n  x\n}\n\n{\n  NEW\n}\n\n{\n  y\n}\n");

    build_workdir_list(git_dir, root, &list);
    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    out = render(git_dir, root, &list, &opts);
    CHECK(strstr(out, "@@ -3,6 +3,10 @@ top\n"
                     "   x\n"
                     " }\n"
                     " \n"
                     "+{\n"
                     "+  NEW\n"
                     "+}\n"
                     "+\n"
                     " {\n"
                     "   y\n"
                     " }\n") != NULL,
         "A1 (blank-separated duplicate block) mismatch: %s", out);
    free(out);

    sg_diff_list_free(&list);
    free(root);
}

static void test_patch_indent_heuristic_no_blank_duplicate_block(void)
{
    char *root = make_tmp_repo_and_cd("indent_a2");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
    write_file("f.txt", "top\n{\n  x\n}\n{\n  y\n}\n");
    run_add_all();
    run_commit("base");
    write_file("f.txt", "top\n{\n  x\n}\n{\n  NEW\n}\n{\n  y\n}\n");

    build_workdir_list(git_dir, root, &list);
    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    out = render(git_dir, root, &list, &opts);
    /* Without the heuristic this group compacts to the BOTTOM of its slide
       range, printing "+  NEW / +} / +{" (the inserted block shifted down
       by one line) instead -- that is the OFF form CLAUDE.md's Phase 26
       note records for this exact fixture. A wrong sign on the indent
       heuristic's scoring constants reproduces exactly that OFF form even
       with the heuristic "on" (Phase 26 implementation note: this is what
       actually happened here before the constant's sign was corrected). */
    CHECK(strstr(out, "@@ -2,6 +2,9 @@ top\n"
                     " {\n"
                     "   x\n"
                     " }\n"
                     "+{\n"
                     "+  NEW\n"
                     "+}\n"
                     " {\n"
                     "   y\n"
                     " }\n") != NULL,
         "A2 (no-blank duplicate block) mismatch: %s", out);
    free(out);

    sg_diff_list_free(&list);
    free(root);
}

/* A3: the CONTROL fixture (see the file-level comment above) -- inserting a
   sibling method into a class body lands in the same place whether or not
   the heuristic runs, so a regression that always picks the "off" position
   would NOT be caught by A1/A2 alone if their answers ever happened to
   coincide too; A3 pins that this fixture's ON and OFF answers are the
   SAME known value, so it stays green under either behaviour and is not,
   by itself, evidence the heuristic is doing anything -- A1/A2 are what
   carry that signal. */
static void test_patch_indent_heuristic_class_method_insert_control(void)
{
    char *root = make_tmp_repo_and_cd("indent_a3");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
    write_file("f.txt", "class A:\n    def f(self):\n        pass\n");
    run_add_all();
    run_commit("base");
    write_file("f.txt", "class A:\n    def g(self):\n        pass\n\n    def f(self):\n        pass\n");

    build_workdir_list(git_dir, root, &list);
    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    out = render(git_dir, root, &list, &opts);
    /* No suffix here: the hunk starts at file offset 0 (nothing precedes
       "class A:" itself), so the backward scan has nothing to search --
       this is NOT the "not found" case (CLAUDE.md's Phase 26 note is about
       a scan that runs and fails to match a character class; this is a
       scan with an empty search space, "before == 0"). */
    CHECK(strstr(out, "@@ -1,3 +1,6 @@\n"
                     " class A:\n"
                     "+    def g(self):\n"
                     "+        pass\n"
                     "+\n"
                     "     def f(self):\n"
                     "         pass\n") != NULL,
         "A3 control fixture mismatch: %s", out);
    free(out);

    sg_diff_list_free(&list);
    free(root);
}

/* G: a pure duplication of an existing repeated pattern with no indentation
   to score on at all -- group compaction (not the heuristic, which has
   nothing to distinguish here) must still land the insertion at the
   BOTTOM of its slide range. */
static void test_patch_group_compaction_slides_to_bottom(void)
{
    char *root = make_tmp_repo_and_cd("compact_g");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
    write_file("f.txt", "h\nA\nB\nA\nB\nt\n");
    run_add_all();
    run_commit("base");
    write_file("f.txt", "h\nA\nB\nA\nB\nA\nB\nt\n");

    build_workdir_list(git_dir, root, &list);
    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    out = render(git_dir, root, &list, &opts);
    CHECK(strstr(out, "@@ -3,4 +3,6 @@ A\n"
                     " B\n"
                     " A\n"
                     " B\n"
                     "+A\n"
                     "+B\n"
                     " t\n") != NULL,
         "G (bottom-of-range compaction) mismatch: %s", out);
    free(out);

    sg_diff_list_free(&list);
    free(root);
}

/* ---- Phase 26: hunk-merge gap boundary (CLAUDE.md: <=6 merges, >=7 splits) */

static char *repeat_same_lines(size_t n)
{
    /* n lines of "same\n", used as the equal-run between two changes. */
    char *buf = malloc(n * 5 + 1);
    size_t i;

    CHECK(buf != NULL, "OOM building repeat_same_lines fixture");
    buf[0] = '\0';
    for (i = 0; i < n; i++)
        strcat(buf, "same\n");
    return buf;
}

static void build_gap_fixture(size_t gap, char **base_out, char **changed_out)
{
    char *middle = repeat_same_lines(gap);
    size_t n = strlen("c1\nc2\n") + strlen(middle) + strlen("d1\nd2\n") + 1;
    char *base = malloc(n);
    char *changed = malloc(n);

    CHECK(base != NULL && changed != NULL, "OOM building gap fixture");
    snprintf(base, n, "c1\nc2\n%sd1\nd2\n", middle);
    snprintf(changed, n, "C1\nC2\n%sD1\nD2\n", middle);
    free(middle);
    *base_out = base;
    *changed_out = changed;
}

static void test_patch_hunk_merge_boundary_six_lines_merges(void)
{
    char *root = make_tmp_repo_and_cd("gap6");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;
    char *base, *changed;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
    build_gap_fixture(6, &base, &changed);
    write_file("f.txt", base);
    run_add_all();
    run_commit("base");
    write_file("f.txt", changed);

    build_workdir_list(git_dir, root, &list);
    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    out = render(git_dir, root, &list, &opts);
    CHECK(strstr(out, "@@ -1,10 +1,10 @@\n") != NULL,
         "a 6-line gap must merge into a single hunk: %s", out);
    {
        const char *first = strstr(out, "@@ ");

        CHECK(first != NULL && strstr(first + 1, "@@ ") == NULL,
             "expected exactly one hunk (6-line gap must not split): %s", out);
    }

    free(out);
    free(base);
    free(changed);
    sg_diff_list_free(&list);
    free(root);
}

static void test_patch_hunk_split_boundary_seven_lines_splits(void)
{
    char *root = make_tmp_repo_and_cd("gap7");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;
    char *base, *changed;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
    build_gap_fixture(7, &base, &changed);
    write_file("f.txt", base);
    run_add_all();
    run_commit("base");
    write_file("f.txt", changed);

    build_workdir_list(git_dir, root, &list);
    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    out = render(git_dir, root, &list, &opts);
    CHECK(strstr(out, "@@ -1,5 +1,5 @@\n") != NULL,
         "a 7-line gap's first hunk mismatch: %s", out);
    CHECK(strstr(out, "@@ -7,5 +7,5 @@ same\n") != NULL,
         "a 7-line gap's second hunk mismatch (must split, not merge): %s", out);

    free(out);
    free(base);
    free(changed);
    sg_diff_list_free(&list);
    free(root);
}

/* ---- Phase 26: function-name hunk suffix -------------------------------- */

/* Lines 1..6 all start with a digit, so the backward scan must walk past
   all of them to reach line 0 ("def handler():", first char alnum) --
   this pins BOTH the character-class boundary (digits don't count) and
   that the search finds the NEAREST qualifying line, not just any. */
static void test_patch_function_name_suffix_found(void)
{
    char *root = make_tmp_repo_and_cd("funcname_found");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
    write_file("f.txt", "def handler():\n1a\n2b\n3c\n4d\n5e\n6f\ntarget\n8g\n9h\n10i\n");
    run_add_all();
    run_commit("base");
    write_file("f.txt", "def handler():\n1a\n2b\n3c\n4d\n5e\n6f\nTARGET\n8g\n9h\n10i\n");

    build_workdir_list(git_dir, root, &list);
    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    out = render(git_dir, root, &list, &opts);
    CHECK(strstr(out, "@@ -5,7 +5,7 @@ def handler():\n") != NULL,
         "expected the nearest preceding alnum-first line as the hunk suffix: %s", out);
    free(out);

    sg_diff_list_free(&list);
    free(root);
}

/* No line in the file starts with alnum/_/$ (every line starts with a
   digit), so no suffix is printed -- and critically, the "@@ ... @@" line
   must have NO trailing space in that case. Asserted as a full string
   (CLAUDE.md: "請斷言完整字串,不要只 strstr"), not just presence, because a
   stray trailing space would be invisible to a strstr-only check that only
   looked for the substring up to "@@". */
static void test_patch_function_name_suffix_not_found_no_trailing_space(void)
{
    char *root = make_tmp_repo_and_cd("funcname_absent");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;
    const char *line;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
    write_file("f.txt", "1a\n2b\n3c\ntarget\n5e\n6f\n7g\n");
    run_add_all();
    run_commit("base");
    write_file("f.txt", "1a\n2b\n3c\nTARGET\n5e\n6f\n7g\n");

    build_workdir_list(git_dir, root, &list);
    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    out = render(git_dir, root, &list, &opts);
    line = strstr(out, "@@ ");
    CHECK(line != NULL, "expected an @@ hunk line at all: %s", out);
    CHECK(line != NULL && strncmp(line, "@@ -1,7 +1,7 @@\n", strlen("@@ -1,7 +1,7 @@\n")) == 0,
         "expected no function-name suffix and no trailing space: %s", out);
    free(out);

    sg_diff_list_free(&list);
    free(root);
}

/* ---- Phase 26: "\ No newline at end of file", all three combinations --- */

static void test_patch_no_newline_old_only(void)
{
    char *root = make_tmp_repo_and_cd("nonl_old");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
    write_file("f.txt", "keep\ntail"); /* old side: "tail" has NO trailing newline */
    run_add_all();
    run_commit("base");
    write_file("f.txt", "keep\ntail\n"); /* new side: same text, WITH a trailing newline */

    build_workdir_list(git_dir, root, &list);
    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    out = render(git_dir, root, &list, &opts);
    CHECK(strstr(out, "@@ -1,2 +1,2 @@\n"
                     " keep\n"
                     "-tail\n"
                     "\\ No newline at end of file\n"
                     "+tail\n") != NULL,
         "old-side-only missing newline mismatch: %s", out);
    free(out);

    sg_diff_list_free(&list);
    free(root);
}

static void test_patch_no_newline_new_only(void)
{
    char *root = make_tmp_repo_and_cd("nonl_new");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
    write_file("f.txt", "keep\ntail\n"); /* old side: trailing newline present */
    run_add_all();
    run_commit("base");
    write_file("f.txt", "keep\ntail"); /* new side: same text, NO trailing newline */

    build_workdir_list(git_dir, root, &list);
    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    out = render(git_dir, root, &list, &opts);
    CHECK(strstr(out, "@@ -1,2 +1,2 @@\n"
                     " keep\n"
                     "-tail\n"
                     "+tail\n"
                     "\\ No newline at end of file\n") != NULL,
         "new-side-only missing newline mismatch: %s", out);
    free(out);

    sg_diff_list_free(&list);
    free(root);
}

static void test_patch_no_newline_both(void)
{
    char *root = make_tmp_repo_and_cd("nonl_both");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
    write_file("f.txt", "keep\noldtail"); /* neither side ends in a newline, AND */
    run_add_all();
    run_commit("base");
    write_file("f.txt", "keep\nnewtail"); /* ...the final line's content also changed */

    build_workdir_list(git_dir, root, &list);
    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    out = render(git_dir, root, &list, &opts);
    CHECK(strstr(out, "@@ -1,2 +1,2 @@\n"
                     " keep\n"
                     "-oldtail\n"
                     "\\ No newline at end of file\n"
                     "+newtail\n"
                     "\\ No newline at end of file\n") != NULL,
         "both-sides missing newline mismatch: %s", out);
    free(out);

    sg_diff_list_free(&list);
    free(root);
}

/* Fix 4 regression: content_changed's "!old_verified || !new_verified"
   disjuncts must not be removable without a unit test noticing. Two BLOB
   sides sharing the exact same (nonexistent) id are constructed by hand --
   memcmp alone would call this "unchanged" and print nothing but the "diff
   --git" header with return 0, silently swallowing the fact neither side
   could actually be read. With the disjuncts intact, an unreadable id forces
   content_changed=true regardless of the coincidental id match, so the
   index line still prints and the subsequent content read fails loudly
   (stderr warning, sg_diff_print returning -1) instead of being skipped. */
static void test_patch_unreadable_blob_with_matching_ids_is_not_silently_skipped(void)
{
    char *root = make_tmp_repo_and_cd("ghostblob");
    char git_dir[4096];
    unsigned char ghost_id[SG_SHA1_RAW_LEN];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;
    int rc;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
    memset(ghost_id, 0xAB, sizeof(ghost_id));

    memset(&list, 0, sizeof(list));
    list.entries = malloc(sizeof(*list.entries));
    CHECK(list.entries != NULL, "OOM building manual diff list");
    list.entries[0].path = strdup("ghost.bin");
    memset(&list.entries[0].old_side, 0, sizeof(list.entries[0].old_side));
    list.entries[0].old_side.kind = SG_DIFF_SIDE_BLOB;
    list.entries[0].old_side.mode = 0100644;
    memcpy(list.entries[0].old_side.id, ghost_id, SG_SHA1_RAW_LEN);
    list.entries[0].new_side = list.entries[0].old_side; /* byte-identical id, on purpose */
    list.entries[0].unmerged = 0;
    list.count = 1;
    list.cap = 1;

    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;

    capture_start();
    rc = sg_diff_print(git_dir, root, &list, &opts);
    out = capture_end();

    CHECK(rc == -1, "sg_diff_print must report failure for an unreadable blob, got %d", rc);
    CHECK(strstr(out, "diff --git a/ghost.bin b/ghost.bin\n") != NULL,
         "expected the diff --git header line: %s", out);
    CHECK(strstr(out, "index abababa..abababa 100644\n") != NULL,
         "expected the index line to still print (content_changed forced true): %s", out);
    CHECK(strstr(out, "---") == NULL,
         "must not print a hunk body for content it never actually managed to read: %s", out);
    free(out);

    sg_diff_list_free(&list);
    free(root);
}

int main(void)
{
    test_six_formats();
    test_stat_scaling();
    test_stat_truncation();
    test_name_status_add_delete();
    test_summary_variants();
    test_binary_vs_unmerged_files_changed();
    test_unmerged_only_zero_files_changed();
    test_binary_forces_number_width();
    test_display_width_cjk_vs_ascii();
    test_patch_add_and_delete_index_lines();
    test_patch_pure_mode_change_has_no_index_or_body();
    test_patch_mode_and_content_change_index_has_no_suffix();
    test_patch_binary_new_file_has_index_and_devnull();
    test_patch_binary_modify_index_line();
    test_patch_chunk_pointer_index_line_uses_effective_id();
    test_patch_unstaged_mode_only_change_has_no_index_or_body();
    test_patch_indent_heuristic_blank_separated_duplicate_block();
    test_patch_indent_heuristic_no_blank_duplicate_block();
    test_patch_indent_heuristic_class_method_insert_control();
    test_patch_group_compaction_slides_to_bottom();
    test_patch_hunk_merge_boundary_six_lines_merges();
    test_patch_hunk_split_boundary_seven_lines_splits();
    test_patch_function_name_suffix_found();
    test_patch_function_name_suffix_not_found_no_trailing_space();
    test_patch_no_newline_old_only();
    test_patch_no_newline_new_only();
    test_patch_no_newline_both();
    test_patch_unreadable_blob_with_matching_ids_is_not_silently_skipped();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all diff_out tests passed\n");
    return 0;
}
