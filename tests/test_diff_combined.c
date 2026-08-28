/* Phase 34: combined diff (`sg diff -c` / `--cc`) for an unmerged path.
   Expected strings for the hand-built fixtures were cross-checked against
   real git 2.55.0 in a scratch repo before being written down here (same
   discipline as tests/test_diff_out.c's header comment); blob ids are
   never hardcoded, they come back from sg_loose_write itself so a test
   never silently drifts from the content it actually wrote. */
#include "sg/cli.h"

#include "sg/diff.h"
#include "sg/diff_out.h"
#include "sg/hash.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/refs.h"
#include "sg/repo.h"
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

/* ---- fixture plumbing (mirrors tests/test_diff_out.c) ----------------- */

static char *make_tmp_repo_and_cd(const char *tag)
{
    char template[] = "/tmp/sg_diff_combined_XXXXXX";
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

static int g_saved_stdout = -1;
static char g_capture_path[] = "/tmp/sg_diff_combined_capture_XXXXXX";

static void capture_start(void)
{
    char path[] = "/tmp/sg_diff_combined_capture_XXXXXX";
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

/* ---- manual sg_diff_list construction (mirrors test_diff_out.c's
   "ghost.bin" fixture): a single unmerged, combinable row whose ours/theirs
   are real loose blobs (so sg_diff_side_read can actually resolve them) and
   whose result is a real working-tree file at `path` (or absent, for a
   deleted-result fixture). ------------------------------------------- */

static void build_unmerged_list(sg_diff_list *list, const char *git_dir, const char *path,
                                const char *ours_content, unsigned int ours_mode,
                                const char *theirs_content, unsigned int theirs_mode,
                                const char *result_content /* NULL = deleted */,
                                unsigned int result_mode)
{
    unsigned char ours_id[SG_SHA1_RAW_LEN], theirs_id[SG_SHA1_RAW_LEN];
    sg_diff_entry *e;

    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, ours_content, strlen(ours_content), ours_id) == 0,
         "write ours blob failed");
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, theirs_content, strlen(theirs_content), theirs_id) == 0,
         "write theirs blob failed");

    memset(list, 0, sizeof(*list));
    list->entries = malloc(sizeof(*list->entries));
    CHECK(list->entries != NULL, "OOM building manual diff list");
    e = &list->entries[0];
    memset(e, 0, sizeof(*e));
    e->path = strdup(path);
    e->unmerged = 1;
    e->ours.kind = SG_DIFF_SIDE_BLOB;
    e->ours.mode = ours_mode;
    memcpy(e->ours.id, ours_id, SG_SHA1_RAW_LEN);
    e->theirs.kind = SG_DIFF_SIDE_BLOB;
    e->theirs.mode = theirs_mode;
    memcpy(e->theirs.id, theirs_id, SG_SHA1_RAW_LEN);
    if (result_content != NULL) {
        write_file(path, result_content);
        e->result.kind = SG_DIFF_SIDE_WORKDIR;
        e->result.mode = result_mode;
    } else {
        unlink(path);
        e->result.kind = SG_DIFF_SIDE_ABSENT;
    }
    list->count = 1;
    list->cap = 1;
}

static char *abbrev7(const unsigned char id[SG_SHA1_RAW_LEN])
{
    char hex[SG_SHA1_HEX_LEN + 1];

    sg_sha1_to_hex(id, hex);
    hex[7] = '\0';
    return strdup(hex);
}

/* ---- (A) typical conflict, column order (ours = column 1) ------------- */

static void test_combined_typical_conflict_column_order(void)
{
    char *root = make_tmp_repo_and_cd("A");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;
    char *ours_abbrev, *theirs_abbrev;
    char expected[2048];

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);

    build_unmerged_list(&list, git_dir, "f.txt", "a\nb\nOURS\nd\ne\nf\ng\n", 0100644,
                        "a\nb\nSIDE\nd\ne\nf\ng\n", 0100644,
                        "a\nb\n<<<<<<< HEAD\nOURS\n=======\nSIDE\n>>>>>>> side\nd\ne\nf\ng\n", 0100644);

    ours_abbrev = abbrev7(list.entries[0].ours.id);
    theirs_abbrev = abbrev7(list.entries[0].theirs.id);

    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH; /* combined == 0: PATCH's implicit dense default */
    out = render(git_dir, root, &list, &opts);

    snprintf(expected, sizeof(expected),
            "diff --cc f.txt\n"
            "index %s,%s..0000000\n"
            "--- a/f.txt\n"
            "+++ b/f.txt\n"
            "@@@ -1,6 -1,6 +1,10 @@@\n"
            "  a\n"
            "  b\n"
            "++<<<<<<< HEAD\n"
            " +OURS\n"
            "++=======\n"
            "+ SIDE\n"
            "++>>>>>>> side\n"
            "  d\n"
            "  e\n"
            "  f\n",
            ours_abbrev, theirs_abbrev);
    CHECK(strcmp(out, expected) == 0, "typical conflict mismatch:\n%s", out);

    free(out);
    free(ours_abbrev);
    free(theirs_abbrev);
    sg_diff_list_free(&list);
    free(root);
}

/* ---- (B) dense drops a hunk whose result equals one parent verbatim --- */

static void test_combined_dense_omission(void)
{
    char *root = make_tmp_repo_and_cd("B");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;
    char *ours_abbrev, *theirs_abbrev;
    char expected[2048];

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);

    /* result == ours verbatim: dense must print ONLY the four header
       lines, no hunk at all. */
    build_unmerged_list(&list, git_dir, "f.txt", "a\nb\nOURS\nd\ne\nf\ng\n", 0100644,
                        "a\nb\nSIDE\nd\ne\nf\ng\n", 0100644, "a\nb\nOURS\nd\ne\nf\ng\n", 0100644);
    ours_abbrev = abbrev7(list.entries[0].ours.id);
    theirs_abbrev = abbrev7(list.entries[0].theirs.id);

    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    out = render(git_dir, root, &list, &opts);
    snprintf(expected, sizeof(expected),
            "diff --cc f.txt\n"
            "index %s,%s..0000000\n"
            "--- a/f.txt\n"
            "+++ b/f.txt\n",
            ours_abbrev, theirs_abbrev);
    CHECK(strcmp(out, expected) == 0, "dense omission (--cc) mismatch:\n%s", out);
    free(out);

    /* -c (non-dense) must NOT prune it -- the full hunk shows, with the
       lost SIDE line (parent 2 only) then the added OURS line (new only
       relative to parent 2). */
    opts.combined = 2;
    out = render(git_dir, root, &list, &opts);
    snprintf(expected, sizeof(expected),
            "diff --combined f.txt\n"
            "index %s,%s..0000000\n"
            "--- a/f.txt\n"
            "+++ b/f.txt\n"
            "@@@ -1,6 -1,6 +1,6 @@@\n"
            "  a\n"
            "  b\n"
            " -SIDE\n"
            " +OURS\n"
            "  d\n"
            "  e\n"
            "  f\n",
            ours_abbrev, theirs_abbrev);
    CHECK(strcmp(out, expected) == 0, "-c must not prune the same hunk:\n%s", out);

    free(out);
    free(ours_abbrev);
    free(theirs_abbrev);
    sg_diff_list_free(&list);
    free(root);
}

/* ---- (C) dense pruning is per-hunk, not per-file ----------------------- */

static void test_combined_dense_prunes_per_hunk(void)
{
    char *root = make_tmp_repo_and_cd("C");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;
    char ours_content[1024], theirs_content[1024], result_content[1024];

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);

    /* Two independent conflicts far enough apart that they can never merge
       into one hunk: the first resolved to ours verbatim (must be pruned
       in dense mode), the second still carrying real conflict markers
       (must survive). */
    snprintf(ours_content, sizeof(ours_content),
            "H1\nH2\nH3\nH4\nOURS_A\nH6\nH7\nH8\nH9\nH10\nH11\nH12\nH13\nH14\nH15\nOURS_B\nH17\nH18\nH19\nH20\n");
    snprintf(theirs_content, sizeof(theirs_content),
            "H1\nH2\nH3\nH4\nSIDE_A\nH6\nH7\nH8\nH9\nH10\nH11\nH12\nH13\nH14\nH15\nSIDE_B\nH17\nH18\nH19\nH20\n");
    snprintf(result_content, sizeof(result_content),
            "H1\nH2\nH3\nH4\nOURS_A\nH6\nH7\nH8\nH9\nH10\nH11\nH12\nH13\nH14\nH15\n"
            "<<<<<<< HEAD\nOURS_B\n=======\nSIDE_B\n>>>>>>> side\nH17\nH18\nH19\nH20\n");

    build_unmerged_list(&list, git_dir, "f.txt", ours_content, 0100644, theirs_content, 0100644,
                        result_content, 0100644);

    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH; /* dense default */
    out = render(git_dir, root, &list, &opts);

    CHECK(strstr(out, "OURS_A") == NULL, "dense must have pruned the resolved-to-ours hunk:\n%s", out);
    CHECK(strstr(out, "<<<<<<< HEAD") != NULL, "dense must keep the still-conflicted hunk:\n%s", out);
    CHECK(strstr(out, "SIDE_B") != NULL, "dense must keep the still-conflicted hunk's lost line:\n%s", out);
    {
        const char *p = out;
        int hunks = 0;

        while ((p = strstr(p, "@@@")) != NULL) {
            hunks++;
            p += 3;
        }
        /* Each "@@@ ... @@@" hunk header contains the marker TWICE (open
           and close), so a single surviving hunk counts 2 here. */
        CHECK(hunks == 2, "dense should keep exactly one hunk (2 \"@@@\" markers), got %d:\n%s", hunks, out);
    }
    free(out);

    /* -c keeps BOTH hunks -- no pruning at all. */
    opts.combined = 2;
    out = render(git_dir, root, &list, &opts);
    CHECK(strstr(out, "OURS_A") != NULL, "-c must keep the resolved-to-ours hunk too:\n%s", out);
    CHECK(strstr(out, "<<<<<<< HEAD") != NULL, "-c must keep the still-conflicted hunk:\n%s", out);
    {
        const char *p = out;
        int hunks = 0;

        while ((p = strstr(p, "@@@")) != NULL) {
            hunks++;
            p += 3;
        }
        CHECK(hunks == 4, "-c should keep both hunks (4 \"@@@\" markers), got %d:\n%s", hunks, out);
    }
    free(out);

    sg_diff_list_free(&list);
    free(root);
}

/* ---- (D) result emptied entirely: both parents fully lost, coalesced -- */

static void test_combined_empty_result(void)
{
    char *root = make_tmp_repo_and_cd("D");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;
    char *ours_abbrev, *theirs_abbrev;
    char expected[2048];

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);

    build_unmerged_list(&list, git_dir, "f.txt", "a\nO\nc\n", 0100644, "a\nS\nc\n", 0100644, "", 0100644);
    ours_abbrev = abbrev7(list.entries[0].ours.id);
    theirs_abbrev = abbrev7(list.entries[0].theirs.id);

    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    out = render(git_dir, root, &list, &opts);
    /* Both --cc and -c print the same thing here (PHASE34_ORACLE.md sample
       D): the LCS-coalesced lost lines interleave "a" (both), "O"
       (ours-only), "S" (theirs-only), "c" (both). */
    snprintf(expected, sizeof(expected),
            "diff --cc f.txt\n"
            "index %s,%s..0000000\n"
            "--- a/f.txt\n"
            "+++ b/f.txt\n"
            "@@@ -1,3 -1,3 +1,0 @@@\n"
            "--a\n"
            "- O\n"
            " -S\n"
            "--c\n",
            ours_abbrev, theirs_abbrev);
    CHECK(strcmp(out, expected) == 0, "empty-result --cc mismatch:\n%s", out);
    free(out);

    opts.combined = 2;
    out = render(git_dir, root, &list, &opts);
    expected[0] = '\0';
    snprintf(expected, sizeof(expected),
            "diff --combined f.txt\n"
            "index %s,%s..0000000\n"
            "--- a/f.txt\n"
            "+++ b/f.txt\n"
            "@@@ -1,3 -1,3 +1,0 @@@\n"
            "--a\n"
            "- O\n"
            " -S\n"
            "--c\n",
            ours_abbrev, theirs_abbrev);
    CHECK(strcmp(out, expected) == 0, "empty-result -c mismatch:\n%s", out);

    free(out);
    free(ours_abbrev);
    free(theirs_abbrev);
    sg_diff_list_free(&list);
    free(root);
}

/* ---- (E) deleted result: header + mode line only, no hunk body -------- */

static void test_combined_deleted_result(void)
{
    char *root = make_tmp_repo_and_cd("E");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;
    char *ours_abbrev, *theirs_abbrev;
    char expected[2048];

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);

    build_unmerged_list(&list, git_dir, "f.txt", "a\nb\nc\n", 0100644, "a\nx\nc\n", 0100644, NULL, 0);
    ours_abbrev = abbrev7(list.entries[0].ours.id);
    theirs_abbrev = abbrev7(list.entries[0].theirs.id);

    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    out = render(git_dir, root, &list, &opts);
    snprintf(expected, sizeof(expected),
            "diff --cc f.txt\n"
            "index %s,%s..0000000\n"
            "deleted file mode 100644,100644\n"
            "--- a/f.txt\n"
            "+++ /dev/null\n",
            ours_abbrev, theirs_abbrev);
    CHECK(strcmp(out, expected) == 0, "deleted-result mismatch:\n%s", out);

    free(out);
    free(ours_abbrev);
    free(theirs_abbrev);
    sg_diff_list_free(&list);
    free(root);
}

/* ---- binary: any of the three sides being binary routes here ---------- */

static void test_combined_binary(void)
{
    char *root = make_tmp_repo_and_cd("BIN");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;
    char *ours_abbrev, *theirs_abbrev;
    char expected[2048];

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);

    /* Both parents are text; only the working-tree result is binary
       (NUL byte) -- PHASE34_ORACLE.md's "B2" fixture note: this alone is
       enough to route the whole row through the binary branch. */
    build_unmerged_list(&list, git_dir, "f.bin", "a\nb\nc\n", 0100644, "a\nx\nc\n", 0100644, "a\0b\0c", 0100644);
    ours_abbrev = abbrev7(list.entries[0].ours.id);
    theirs_abbrev = abbrev7(list.entries[0].theirs.id);
    /* build_unmerged_list's write_file uses fputs, which stops at the NUL --
       overwrite with the real 5 bytes directly. */
    {
        FILE *f = fopen("f.bin", "wb");

        CHECK(f != NULL, "reopen f.bin failed");
        fwrite("a\0b\0c", 1, 5, f);
        fclose(f);
    }

    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    out = render(git_dir, root, &list, &opts);
    snprintf(expected, sizeof(expected),
            "diff --cc f.bin\n"
            "index %s,%s..0000000\n"
            "Binary files differ\n",
            ours_abbrev, theirs_abbrev);
    CHECK(strcmp(out, expected) == 0, "binary mismatch:\n%s", out);

    free(out);
    free(ours_abbrev);
    free(theirs_abbrev);
    sg_diff_list_free(&list);
    free(root);
}

/* ---- funcname suffix: the off-by-one and the 40-byte scan cap --------- */

static void test_combined_funcname_off_by_one(void)
{
    char *root = make_tmp_repo_and_cd("FN");
    char git_dir[4096];
    sg_diff_list list;
    sg_diff_out_opts opts;
    char *out;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);

    /* "L16" preceding a conflict prints "L1" (last byte '6' dropped);
       needs >=3 lines of plain context between the candidate and the
       conflict, otherwise give_context's leading-context painting pulls
       the candidate INSIDE the hunk and it stops being a valid comment
       source (measured against this file's own combine_dump loop, which
       only records a candidate while walking lines NOT yet marked). */
    build_unmerged_list(&list, git_dir, "f.c", "L16\nX1\nX2\nX3\nOURS\nEND\n", 0100644,
                        "L16\nX1\nX2\nX3\nSIDE\nEND\n", 0100644,
                        "L16\nX1\nX2\nX3\n<<<<<<< HEAD\nOURS\n=======\nSIDE\n>>>>>>> side\nEND\n", 0100644);

    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    out = render(git_dir, root, &list, &opts);
    CHECK(strstr(out, "@@@") != NULL && strstr(strstr(out, "@@@") + 3, "@@@ L1\n") != NULL,
         "expected the funcname suffix truncated to \"L1\" (last byte of \"L16\" dropped): %s", out);
    free(out);
    sg_diff_list_free(&list);
    free(root);

    /* A single-byte candidate ("L") must print comment_end == 0: no
       comment at all, not even the separating space. */
    root = make_tmp_repo_and_cd("FN2");
    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
    build_unmerged_list(&list, git_dir, "f.c", "L\nX1\nX2\nX3\nOURS\nEND\n", 0100644,
                        "L\nX1\nX2\nX3\nSIDE\nEND\n", 0100644,
                        "L\nX1\nX2\nX3\n<<<<<<< HEAD\nOURS\n=======\nSIDE\n>>>>>>> side\nEND\n", 0100644);
    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    out = render(git_dir, root, &list, &opts);
    CHECK(strstr(out, "@@@ L") == NULL, "single-byte candidate must print no comment suffix at all: %s", out);
    free(out);
    sg_diff_list_free(&list);
    free(root);

    /* A 70-byte candidate prints only the first 39 bytes (scan capped at
       40 bytes; comment_end is the index of the last non-space byte
       within that window, byte 39 here -- see PHASE34_ORACLE.md #4). */
    root = make_tmp_repo_and_cd("FN3");
    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
    {
        const char *longname = "int a_very_long_function_name_that_exceeds_forty_bytes_of_scan(void) {";
        char ours_c[512], theirs_c[512], result_c[512];

        CHECK(strlen(longname) == 70, "test fixture bug: expected a 70-byte candidate, got %zu",
             strlen(longname));
        snprintf(ours_c, sizeof(ours_c), "%s\nX1\nX2\nX3\nOURS\nEND\n", longname);
        snprintf(theirs_c, sizeof(theirs_c), "%s\nX1\nX2\nX3\nSIDE\nEND\n", longname);
        snprintf(result_c, sizeof(result_c), "%s\nX1\nX2\nX3\n<<<<<<< HEAD\nOURS\n=======\nSIDE\n>>>>>>> side\nEND\n",
                longname);
        build_unmerged_list(&list, git_dir, "f.c", ours_c, 0100644, theirs_c, 0100644, result_c, 0100644);
    }
    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    out = render(git_dir, root, &list, &opts);
    {
        char expected_suffix[64];

        snprintf(expected_suffix, sizeof(expected_suffix), "@@@ %.39s\n",
                "int a_very_long_function_name_that_exceeds_forty_bytes_of_scan(void) {");
        CHECK(strstr(out, expected_suffix) != NULL, "expected the funcname capped at 39 bytes: %s", out);
        CHECK(strstr(out, "exceeds_forty_bytes_of_scan") == NULL,
             "must NOT print past the 40-byte scan window: %s", out);
    }
    free(out);
    sg_diff_list_free(&list);
    free(root);
}

/* ---- -c/--cc: last one on the command line wins ------------------------ */

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

static void run_merge_expect_conflict(const char *name)
{
    char *argv[2];

    argv[0] = "merge";
    argv[1] = (char *)name;
    sg_cmd_merge(2, argv);
}

static char *run_diff_cli(int argc, char **argv)
{
    char *out;

    capture_start();
    sg_cmd_diff(argc, argv);
    out = capture_end();
    return out;
}

static void make_conflict_repo(void)
{
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

    run_merge_expect_conflict("master");
}

static void test_cli_c_cc_last_one_wins(void)
{
    char *root = make_tmp_repo_and_cd("cli_order");
    char *out;
    char *argv1[4] = {"diff", "-c", "--cc", NULL};
    char *argv2[4] = {"diff", "--cc", "-c", NULL};

    (void)root;
    make_conflict_repo();

    out = run_diff_cli(3, argv1);
    CHECK(strncmp(out, "diff --cc ", 10) == 0, "\"-c --cc\" (last wins) should be dense: %s", out);
    free(out);

    out = run_diff_cli(3, argv2);
    CHECK(strncmp(out, "diff --combined ", 16) == 0, "\"--cc -c\" (last wins) should be non-dense: %s", out);
    free(out);

    free(root);
}

/* ---- -c/--cc rejected together with a <rev> argument ------------------- */

static void test_cli_rejects_combined_with_rev(void)
{
    char *root = make_tmp_repo_and_cd("cli_rev_reject");
    char *out;
    char *argv1[4] = {"diff", "-c", "HEAD", NULL};
    int rc;

    (void)root;
    make_conflict_repo();

    capture_start();
    rc = sg_cmd_diff(3, argv1);
    out = capture_end();
    CHECK(rc == 1, "-c with a <rev> must exit 1, got %d", rc);
    free(out);

    free(root);
}

/* ---- --cached ignores -c/--cc entirely: always "* Unmerged path" ------ */

static void test_cached_ignores_combined_flags(void)
{
    char *root = make_tmp_repo_and_cd("cached");
    char *out;
    char *argv[4] = {"diff", "--cached", "--cc", NULL};

    (void)root;
    make_conflict_repo();

    out = run_diff_cli(3, argv);
    CHECK(strcmp(out, "* Unmerged path conflict.txt\n") == 0,
         "--cached must ignore --cc and print \"* Unmerged path\": %s", out);
    free(out);

    free(root);
}

int main(void)
{
    test_combined_typical_conflict_column_order();
    test_combined_dense_omission();
    test_combined_dense_prunes_per_hunk();
    test_combined_empty_result();
    test_combined_deleted_result();
    test_combined_binary();
    test_combined_funcname_off_by_one();
    test_cli_c_cc_last_one_wins();
    test_cli_rejects_combined_with_rev();
    test_cached_ignores_combined_flags();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all diff-combined tests passed\n");
    return 0;
}
