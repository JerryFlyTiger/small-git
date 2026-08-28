/* Phase 37, Part A: `sg status -- <pathspec>...`. Pins:
     - A2's untracked-fold table (measured against git 2.55.0, PHASE37_SPEC.md):
       how deep a wholly-untracked directory folds is itself a function of
       the pathspec, not a filter applied after an already-folded walk.
     - A3's five filter points, one assertion each -- staged, unstaged,
       untracked (via A2), ignored, and unmerged (the two easiest to miss,
       per the task brief: the unmerged rows are read directly off idx by
       cmd_status.c's own static printers, bypassing every sg_status_list).
     - staged filtering runs BEFORE rename detection, never after (same
       CLAUDE.md/Phase 29 rule sg_diff_list_filter follows) -- naming only
       the OLD half of a staged rename must report a plain "A" for the new
       path, not "R100", because filtering too late would let only half the
       pair survive.
     - pathspec magic is rejected (same divergence from git as `sg diff`).
     - a pathspec matching nothing is silent and exits 0 (A1). */
#include "sg/cli.h"

#include "sg/hash.h"
#include "sg/index.h"
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
    char template[] = "/tmp/sg_status_pathspec_test_XXXXXX";
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

static void mkdir_p1(const char *path)
{
    if (mkdir(path, 0755) != 0) {
        fprintf(stderr, "mkdir %s failed\n", path);
        exit(1);
    }
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

/* Same technique as test_status_unmerged.c: an unresolved conflict is built
   directly as raw stage 1/2/3 sg_index entries, no real merge needed. */
static void write_conflict_entry(sg_index *idx, const char *path)
{
    unsigned char blob_id[SG_SHA1_RAW_LEN];
    int i;

    memset(blob_id, 0, sizeof(blob_id));
    for (i = 1; i <= 3; i++) {
        sg_index_entry entry;

        memset(&entry, 0, sizeof(entry));
        entry.path = strdup(path);
        entry.mode = 0100644;
        entry.stage = (unsigned int)i;
        memcpy(entry.sha1, blob_id, SG_SHA1_RAW_LEN);
        CHECK(sg_index_upsert(idx, &entry) == 0, "upsert %s stage %d failed", path, i);
        free(entry.path);
    }
}

/* Runs sg_cmd_status with the given argv, capturing everything it writes to
   stdout into out (NUL-terminated, truncated to out_size - 1 if needed).
   Returns sg_cmd_status's own return code. */
static int run_status_capture(int argc, char **argv, char *out, size_t out_size)
{
    char tmpl[] = "/tmp/sg_status_pathspec_capture_XXXXXX";
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

static int run_porcelain(const char *spec, char *out, size_t out_size)
{
    char *argv[4];
    int argc = 0;

    argv[argc++] = "status";
    argv[argc++] = "--porcelain";
    if (spec != NULL) {
        argv[argc++] = "--";
        argv[argc++] = (char *)spec;
    }
    return run_status_capture(argc, argv, out, out_size);
}

/* A2: the untracked-fold table, one wholly-untracked directory
   ("wholly/u1.txt", "wholly/deep/u2.txt"), 7 pathspecs, each checked against
   its measured (git 2.55.0) expected porcelain line. */
static void test_a2_fold_table(void)
{
    static const struct {
        const char *spec; /* NULL = no pathspec at all */
        const char *expect_line;
    } cases[] = {
        {NULL, "?? wholly/\n"},
        {"wholly", "?? wholly/\n"},
        {"wholly/", "?? wholly/\n"},
        {"wholly/u1.txt", "?? wholly/u1.txt\n"},
        {"wholly/deep", "?? wholly/deep/\n"},
        {"wholly/deep/u2.txt", "?? wholly/deep/u2.txt\n"},
        {"wholly/*", "?? wholly/\n"},
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char out[8192];
        char *repo_root = make_tmp_repo_and_cd("a2");
        int rc;

        mkdir_p1("wholly");
        mkdir_p1("wholly/deep");
        write_file("wholly/u1.txt", "u1\n");
        write_file("wholly/deep/u2.txt", "u2\n");

        rc = run_porcelain(cases[i].spec, out, sizeof(out));
        CHECK(rc == 0, "case %zu (spec=%s): rc=%d", i, cases[i].spec ? cases[i].spec : "(none)",
             rc);
        CHECK(strcmp(out, cases[i].expect_line) == 0,
             "case %zu (spec=%s): expected %s got %s", i, cases[i].spec ? cases[i].spec : "(none)",
             cases[i].expect_line, out);
        free(repo_root);
    }
}

/* A3 point 1: staged filter must run BEFORE rename detection. b1.txt (staged
   rename source) -> b2.txt (staged rename dest), pathspec names only the OLD
   half. Expected (Phase 29's rule, same divergence CLAUDE.md documents for
   `sg diff`): filtering removes b2.txt (the new half) before rename
   detection ever runs, so nothing is left to pair with -- b1.txt reports as
   a plain staged deletion ("D"), never "R100", and b2.txt must not appear
   at all (it does not match the pathspec). */
static void test_a3_staged_filter_before_rename(void)
{
    char out[8192];
    char *repo_root = make_tmp_repo_and_cd("a3staged");
    char *git_dir;
    sg_index idx;
    int rc;
    const char *body =
        "line one\nline two\nline three\nline four\nline five\nline six\nline seven\n";

    write_file("b1.txt", body);
    run_add("b1.txt");
    run_commit("base");

    CHECK(rename("b1.txt", "b2.txt") == 0, "rename failed");
    run_add("b2.txt");
    /* sg add does not know the old path is gone from disk on its own -- sg
       has no `add -A`/`rm`, so stage the deletion directly through the index
       API, the same technique write_conflict_entry uses above for building
       a state no single CLI command can reach in one step. */
    git_dir = sg_require_git_dir();
    CHECK(sg_index_read(git_dir, &idx) == 0, "index read failed");
    CHECK(sg_index_remove(&idx, "b1.txt") == 0, "index remove b1.txt failed");
    CHECK(sg_index_write(git_dir, &idx) == 0, "index write failed");
    sg_index_free(&idx);
    free(git_dir);

    rc = run_porcelain("b1.txt", out, sizeof(out));
    CHECK(rc == 0, "rc=%d", rc);
    CHECK(strstr(out, "R100") == NULL && strstr(out, "-> ") == NULL,
         "naming only the old half of a rename must not print a paired rename row, got %s", out);
    CHECK(strstr(out, "D  b1.txt") != NULL,
         "naming only the old half of a rename must report it as a plain staged deletion, got %s",
         out);
    CHECK(strstr(out, "b2.txt") == NULL,
         "b2.txt does not match the pathspec and must not appear at all, got %s", out);
    free(repo_root);
}

/* A3 point 2: unstaged filter. Two tracked files modified in the working
   directory, pathspec names only one. */
static void test_a3_unstaged_filter(void)
{
    char out[8192];
    char *repo_root = make_tmp_repo_and_cd("a3unstaged");
    int rc;

    write_file("x.txt", "x\n");
    write_file("y.txt", "y\n");
    run_add("x.txt");
    run_add("y.txt");
    run_commit("base");

    write_file("x.txt", "x changed\n");
    write_file("y.txt", "y changed\n");

    rc = run_porcelain("x.txt", out, sizeof(out));
    CHECK(rc == 0, "rc=%d", rc);
    CHECK(strstr(out, " M x.txt") != NULL, "x.txt must be reported, got %s", out);
    CHECK(strstr(out, "y.txt") == NULL, "y.txt must be filtered out, got %s", out);
    free(repo_root);
}

/* A3 point 4: --ignored is built from two sg_status_list_untracked calls
   (the set difference), both pathspec-aware since A2 -- a pathspec that
   excludes an ignored file must exclude it from --ignored too. */
static void test_a3_ignored_filter(void)
{
    char out[8192];
    char *argv[5];
    char *repo_root = make_tmp_repo_and_cd("a3ignored");
    int rc;

    write_file(".gitignore", "*.log\n");
    run_add(".gitignore");
    run_commit("base");
    write_file("keep.log", "keep\n");
    write_file("drop.log", "drop\n");

    argv[0] = "status";
    argv[1] = "--porcelain";
    argv[2] = "--ignored";
    argv[3] = "--";
    argv[4] = "keep.log";
    rc = run_status_capture(5, argv, out, sizeof(out));
    CHECK(rc == 0, "rc=%d", rc);
    CHECK(strstr(out, "!! keep.log") != NULL, "keep.log must be reported ignored, got %s", out);
    CHECK(strstr(out, "drop.log") == NULL, "drop.log must be filtered out, got %s", out);
    free(repo_root);
}

/* A3 point 5, the easiest one to miss: print_unmerged/print_porcelain_tracked
   read idx directly, bypassing every sg_status_list -- two conflicted paths,
   pathspec names only one. */
static void test_a3_unmerged_filter(void)
{
    char out[8192];
    sg_index idx;
    char *git_dir;
    char *repo_root = make_tmp_repo_and_cd("a3unmerged");
    int rc;

    memset(&idx, 0, sizeof(idx));
    write_conflict_entry(&idx, "conflict1.txt");
    write_conflict_entry(&idx, "conflict2.txt");
    git_dir = sg_require_git_dir();
    CHECK(sg_index_write(git_dir, &idx) == 0, "index write failed");
    sg_index_free(&idx);
    free(git_dir);

    rc = run_porcelain("conflict1.txt", out, sizeof(out));
    CHECK(rc == 0, "rc=%d", rc);
    CHECK(strstr(out, "conflict1.txt") != NULL, "conflict1.txt must be reported, got %s", out);
    CHECK(strstr(out, "conflict2.txt") == NULL, "conflict2.txt must be filtered out, got %s", out);
    free(repo_root);
}

/* Same fixture as above, but through the LONG format: print_unmerged has its
   OWN pair of scan loops (a counting pass, then a printing pass), entirely
   separate from print_porcelain_tracked's. The counting pass's own filter
   was measured to be a blind spot a "does path X still show up" assertion
   cannot see (the printing pass alone is enough to make that pass) -- it
   only shows up when NO conflicted path matches: the count must reach 0,
   suppressing the entire "Unmerged paths:" section, not just hiding printed
   rows while the header (whose gate is the unfiltered count) still fires. */
static void test_a3_unmerged_filter_long_format(void)
{
    char out[8192];
    char *argv[4];
    sg_index idx;
    char *git_dir;
    char *repo_root = make_tmp_repo_and_cd("a3unmergedlong");
    int rc;

    memset(&idx, 0, sizeof(idx));
    write_conflict_entry(&idx, "conflict1.txt");
    write_conflict_entry(&idx, "conflict2.txt");
    git_dir = sg_require_git_dir();
    CHECK(sg_index_write(git_dir, &idx) == 0, "index write failed");
    sg_index_free(&idx);
    free(git_dir);

    argv[0] = "status";
    argv[1] = "--";
    argv[2] = "nosuch";
    rc = run_status_capture(3, argv, out, sizeof(out));
    CHECK(rc == 0, "rc=%d", rc);
    CHECK(strstr(out, "conflict1.txt") == NULL && strstr(out, "conflict2.txt") == NULL,
         "neither conflict must be reported when the pathspec matches neither, got %s", out);
    CHECK(strstr(out, "Unmerged paths:") == NULL,
         "the section header's own gate is the FILTERED count, not the raw one -- got %s", out);

    /* A second call, spec matching exactly one of the two, so the count
       loop's filter alone (count == 1, section header fires) cannot short-
       circuit past the printing loop's own filter the way the all-excluded
       case above does -- this is what actually exercises the printing
       loop's separate "if (!sg_pathspec_matches(...)) continue;". */
    argv[2] = "conflict1.txt";
    rc = run_status_capture(3, argv, out, sizeof(out));
    CHECK(rc == 0, "rc=%d", rc);
    CHECK(strstr(out, "conflict1.txt") != NULL, "conflict1.txt must be reported, got %s", out);
    CHECK(strstr(out, "conflict2.txt") == NULL,
         "conflict2.txt must not leak through the printing loop, got %s", out);
    free(repo_root);
}

/* A1: pathspec magic is rejected outright, same divergence from git as
   `sg diff` (git supports :(icase)/:!, sg refuses both). */
static void test_magic_rejected(void)
{
    char out[8192];
    char *repo_root = make_tmp_repo_and_cd("magic");
    int rc;

    rc = run_porcelain(":(icase)FOO", out, sizeof(out));
    CHECK(rc == 1, "pathspec magic must be rejected, got rc=%d", rc);
    free(repo_root);
}

/* A1: a pathspec matching nothing at all is silent and exits 0 -- the
   opposite of `sg stash push`'s exit 1 (Part B), a deliberate divergence
   this test exists to pin so nobody "unifies" the two rules later. */
static void test_no_match_is_silent_exit_0(void)
{
    char out[8192];
    char *repo_root = make_tmp_repo_and_cd("nomatch");
    int rc;

    write_file("tracked.txt", "a\n");
    run_add("tracked.txt");
    run_commit("base");
    write_file("tracked.txt", "a changed\n");
    write_file("untracked.txt", "u\n");

    rc = run_porcelain("nosuch", out, sizeof(out));
    CHECK(rc == 0, "rc=%d", rc);
    CHECK(out[0] == '\0', "a pathspec matching nothing must print nothing, got %s", out);
    free(repo_root);
}

/* -uall (SG_STATUS_UNTRACKED_LIST_FILES) is collect_untracked's own site,
   entirely separate from collect_untracked_folded's -- a directory that
   never folds (every file listed individually, per-file always) still has
   to apply the pathspec at the point each file is appended, or -uall would
   silently ignore the pathspec while the default folding mode honors it. */
static void test_a2_uall_filter(void)
{
    char out[8192];
    char *argv[5];
    char *repo_root = make_tmp_repo_and_cd("a2uall");
    int rc;

    mkdir_p1("wholly");
    write_file("wholly/u1.txt", "u1\n");
    write_file("wholly/u2.txt", "u2\n");

    argv[0] = "status";
    argv[1] = "--porcelain";
    argv[2] = "-uall";
    argv[3] = "--";
    argv[4] = "wholly/u1.txt";
    rc = run_status_capture(5, argv, out, sizeof(out));
    CHECK(rc == 0, "rc=%d", rc);
    CHECK(strcmp(out, "?? wholly/u1.txt\n") == 0,
         "-uall with a pathspec must still filter individually-listed files, got %s", out);
    free(repo_root);
}

/* Coordinator follow-up: the long format's closing summary line was
   missing a whole branch (pre-existing gap, not introduced by Phase 37, but
   made much easier to hit by pathspec filtering -- see docs/DESIGN.md).
   Measured against real git 2.55.0 (LC_ALL=C): nothing staged, nothing
   unstaged, but untracked files present prints
   "nothing added to commit but untracked files present (use \"git add\" to
   track)", a DIFFERENT line from the fully-clean case. `sg status --
   wholly` on a fixture with only an untracked wholly/ directory is exactly
   the shape that exposes it: staged/unstaged/unmerged are all empty after
   filtering, only untracked survives. */
static void test_summary_untracked_only(void)
{
    char out[8192];
    char *repo_root = make_tmp_repo_and_cd("summary1");
    char *argv[1];
    int rc;

    mkdir_p1("wholly");
    write_file("wholly/u1.txt", "u1\n");

    argv[0] = "status";
    rc = run_status_capture(1, argv, out, sizeof(out));
    CHECK(rc == 0, "rc=%d", rc);
    CHECK(strstr(out, "nothing added to commit but untracked files present (use \"git add\" "
                     "to track)") != NULL,
         "untracked-only must print the DEDICATED closing line, not silence, got %s", out);
    CHECK(strstr(out, "nothing to commit, working tree clean") == NULL,
         "untracked-only must NOT print the fully-clean line, got %s", out);
    free(repo_root);
}

/* The sibling branch of the same fix: nothing staged, something UNstaged
   (tracked, modified) -- must print "no changes added to commit ...", the
   line the -uno branch already had but the default branch's old (buggy)
   condition could never reach (it required unstaged.count == 0 too).
   Measured against real git 2.55.0 the same way. */
static void test_summary_unstaged_only(void)
{
    char out[8192];
    char *repo_root = make_tmp_repo_and_cd("summary2");
    char *argv[1];
    int rc;

    write_file("tracked.txt", "a\n");
    run_add("tracked.txt");
    run_commit("base");
    write_file("tracked.txt", "a changed\n");

    argv[0] = "status";
    rc = run_status_capture(1, argv, out, sizeof(out));
    CHECK(rc == 0, "rc=%d", rc);
    CHECK(strstr(out, "no changes added to commit (use \"git add\" and/or \"git commit "
                     "-a\")") != NULL,
         "unstaged-only must print the DEDICATED closing line, not silence, got %s", out);
    free(repo_root);
}

int main(void)
{
    test_a2_fold_table();
    test_a2_uall_filter();
    test_a3_staged_filter_before_rename();
    test_a3_unstaged_filter();
    test_a3_ignored_filter();
    test_a3_unmerged_filter();
    test_a3_unmerged_filter_long_format();
    test_magic_rejected();
    test_no_match_is_silent_exit_0();
    test_summary_untracked_only();
    test_summary_unstaged_only();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all status pathspec tests passed\n");
    return 0;
}
