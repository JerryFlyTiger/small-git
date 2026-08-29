/* Pins Phase 38's two ordering rules in `sg status`'s long-format closing
   summary line, both discovered by the skeleton oracle in tests/interop.sh's
   phase38: group (mkfixtures.sh / cmp_skel.py in the milestone scratchpad,
   real git 2.55.0 as the oracle):

   - Bug C: the five-way priority order of the closing "nothing to commit"
     family, in particular that an unborn HEAD (row 3) sits BETWEEN
     "untracked files present" (row 2) and the -uno hedge (row 4) -- not
     after row 4, which is what sg did before this phase.
   - Bug E: once "All conflicts fixed but you are still merging." has
     printed (merge in progress, no unmerged entries left), git suppresses
     the closing summary line entirely, even in the -uno branch; an
     in-progress merge that still HAS unmerged entries prints the summary
     as usual.

   Conflicts are built directly as raw sg_index stage entries and MERGE_HEAD
   is written directly via sg_merge_head_write, the same technique
   test_status_unmerged.c uses -- no real merge/rebase needed to reach any of
   these states. Goes through the public sg_cmd_status entry point and
   captures stdout, same pattern as test_status_untracked_mode.c and
   test_status_unmerged.c (the option parsing and closing-line logic live in
   cmd_status.c's static helpers, not linkable directly from a separate test
   TU). */
#include "sg/cli.h"

#include "sg/hash.h"
#include "sg/index.h"
#include "sg/merge.h"
#include "sg/repo.h"

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

static char *make_tmp_repo_and_cd(const char *tag)
{
    char template[] = "/tmp/sg_status_longfmt_test_XXXXXX";
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
        fprintf(stderr, "fopen(%s) failed\n", path);
        exit(1);
    }
    fputs(content, f);
    fclose(f);
}

/* Runs sg_cmd_status with the given argv, capturing everything it writes to
   stdout into out (NUL-terminated, truncated to out_size - 1 if needed). */
static void run_status_capture(int argc, char **argv, char *out, size_t out_size)
{
    char tmpl[] = "/tmp/sg_status_longfmt_capture_XXXXXX";
    int tmp_fd;
    int saved_stdout;
    off_t len;
    ssize_t n;

    tmp_fd = mkstemp(tmpl);
    if (tmp_fd < 0) {
        fprintf(stderr, "mkstemp failed\n");
        exit(1);
    }
    unlink(tmpl);

    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    dup2(tmp_fd, STDOUT_FILENO);

    CHECK(sg_cmd_status(argc, argv) == 0, "sg status failed");

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
}

static void commit_file(const char *path, const char *content, const char *message)
{
    char argv0[] = "add";
    char *add_argv[2];
    char argv1[] = "-m";
    char *commit_argv[3];
    char msg_buf[128];

    write_file(path, content);
    add_argv[0] = argv0;
    add_argv[1] = (char *)path;
    CHECK(sg_cmd_add(2, add_argv) == 0, "add %s failed", path);

    snprintf(msg_buf, sizeof(msg_buf), "%s", message);
    commit_argv[0] = "commit";
    commit_argv[1] = argv1;
    commit_argv[2] = msg_buf;
    CHECK(sg_cmd_commit(3, commit_argv) == 0, "commit failed");
}

/* Adds one conflicted entry at "conflict.txt" carrying exactly stages 1/2/3
   (an unresolved conflict) on top of whatever is already in the index --
   unlike test_status_unmerged.c's version (which starts from a fresh, empty
   index), this one reads the existing index first so a prior commit_file()
   call's stage-0 entries are not wiped out from under it. */
static void write_conflict_index(void)
{
    sg_index idx;
    unsigned char blob_id[SG_SHA1_RAW_LEN];
    char *git_dir = sg_require_git_dir();
    int i;

    memset(&idx, 0, sizeof(idx));
    CHECK(sg_index_read(git_dir, &idx) == 0, "sg_index_read failed");
    memset(blob_id, 0, sizeof(blob_id));

    for (i = 1; i <= 3; i++) {
        sg_index_entry entry;

        memset(&entry, 0, sizeof(entry));
        entry.path = strdup("conflict.txt");
        entry.mode = 0100644;
        entry.stage = (unsigned int)i;
        memcpy(entry.sha1, blob_id, SG_SHA1_RAW_LEN);
        CHECK(sg_index_upsert(&idx, &entry) == 0, "upsert stage %d failed", i);
        free(entry.path);
    }
    CHECK(sg_index_write(git_dir, &idx) == 0, "index write failed");
    sg_index_free(&idx);
    free(git_dir);
}

static void write_merge_head(void)
{
    char *git_dir = sg_require_git_dir();
    unsigned char id[SG_SHA1_RAW_LEN];

    memset(id, 0, sizeof(id));
    CHECK(sg_merge_head_write(git_dir, id) == 0, "sg_merge_head_write failed");
    free(git_dir);
}

/* ---- Bug C: five-way priority order of the closing summary line ---- */

/* Priority 1: unstaged/unmerged beats everything, including untracked files
   sitting right next to it. */
static void test_priority1_unstaged_beats_untracked(void)
{
    char out[8192];
    char argv0[] = "status";
    char *argv[1];
    char *repo_root = make_tmp_repo_and_cd("p1");

    commit_file("f.txt", "base\n", "base");
    write_file("f.txt", "changed\n");
    write_file("u.txt", "untracked\n");

    argv[0] = argv0;
    run_status_capture(1, argv, out, sizeof(out));
    CHECK(strstr(out, "no changes added to commit (use \"git add\" and/or \"git commit -a\")") !=
             NULL,
         "priority 1 (unstaged beats untracked) not found in:\n%s", out);
    free(repo_root);
}

/* Priority 2: untracked files beat unborn HEAD -- the "unbornu" fixture from
   PHASE38_SPEC.md, where sg was already correct before this phase; pinned
   here so a future regression on priority 3 cannot silently swap the two. */
static void test_priority2_untracked_beats_unborn(void)
{
    char out[8192];
    char argv0[] = "status";
    char *argv[1];
    char *repo_root = make_tmp_repo_and_cd("p2");

    write_file("u.txt", "untracked\n");

    argv[0] = argv0;
    run_status_capture(1, argv, out, sizeof(out));
    CHECK(strstr(out, "nothing added to commit but untracked files present (use \"git add\" to "
                     "track)") != NULL,
         "priority 2 (untracked beats unborn) not found in:\n%s", out);
    free(repo_root);
}

/* Priority 3, default branch: unborn HEAD beats "working tree clean". */
static void test_priority3_unborn_beats_clean(void)
{
    char out[8192];
    char argv0[] = "status";
    char *argv[1];
    char *repo_root = make_tmp_repo_and_cd("p3clean");

    argv[0] = argv0;
    run_status_capture(1, argv, out, sizeof(out));
    CHECK(strstr(out, "nothing to commit (create/copy files and use \"git add\" to track)") !=
             NULL,
         "priority 3 (unborn beats clean) not found in:\n%s", out);
    CHECK(strstr(out, "nothing to commit, working tree clean") == NULL,
         "priority 3 must not ALSO print the clean-tree line:\n%s", out);
    free(repo_root);
}

/* Priority 3, -uno branch: unborn HEAD beats the -uno hedge line (row 4).
   This is the specific regression Bug C fixed -- before Phase 38, sg printed
   row 4 here. */
static void test_priority3_unborn_beats_uno_hedge(void)
{
    char out[8192];
    char argv0[] = "status";
    char argv1[] = "-uno";
    char *argv[2];
    char *repo_root = make_tmp_repo_and_cd("p3uno");

    argv[0] = argv0;
    argv[1] = argv1;
    run_status_capture(2, argv, out, sizeof(out));
    CHECK(strstr(out, "nothing to commit (create/copy files and use \"git add\" to track)") !=
             NULL,
         "priority 3 under -uno (unborn beats the -uno hedge) not found in:\n%s", out);
    CHECK(strstr(out, "nothing to commit (use -u to show untracked files)") == NULL,
         "priority 3 under -uno must not ALSO print row 4's hedge line:\n%s", out);
    free(repo_root);
}

/* Priority 4: with a real HEAD and nothing else going on, -uno's hedge line
   wins over "working tree clean" (which -uno can never actually claim, since
   it never scans untracked files). */
static void test_priority4_uno_hedge_when_head_exists(void)
{
    char out[8192];
    char argv0[] = "status";
    char argv1[] = "-uno";
    char *argv[2];
    char *repo_root = make_tmp_repo_and_cd("p4");

    commit_file("f.txt", "base\n", "base");

    argv[0] = argv0;
    argv[1] = argv1;
    run_status_capture(2, argv, out, sizeof(out));
    CHECK(strstr(out, "nothing to commit (use -u to show untracked files)") != NULL,
         "priority 4 (-uno hedge with a real HEAD) not found in:\n%s", out);
    free(repo_root);
}

/* Priority 5: the default branch's "working tree clean", reached only when
   every earlier row is false. */
static void test_priority5_working_tree_clean(void)
{
    char out[8192];
    char argv0[] = "status";
    char *argv[1];
    char *repo_root = make_tmp_repo_and_cd("p5");

    commit_file("f.txt", "base\n", "base");

    argv[0] = argv0;
    run_status_capture(1, argv, out, sizeof(out));
    CHECK(strstr(out, "nothing to commit, working tree clean") != NULL,
         "priority 5 (working tree clean) not found in:\n%s", out);
    free(repo_root);
}

/* ---- Bug E: closing summary suppression while merging ---- */

/* A merge in progress with no unmerged entries left ("All conflicts fixed
   but you are still merging.") must print NO closing summary line at all --
   none of the five priority-table strings should appear anywhere in the
   output. */
static void test_bug_e_resolved_merge_suppresses_summary(void)
{
    char out[8192];
    char argv0[] = "status";
    char *argv[1];
    char *repo_root = make_tmp_repo_and_cd("e_resolved");

    commit_file("f.txt", "base\n", "base");
    write_merge_head();

    argv[0] = argv0;
    run_status_capture(1, argv, out, sizeof(out));
    CHECK(strstr(out, "All conflicts fixed but you are still merging.") != NULL,
         "expected the resolved-merge banner in:\n%s", out);
    CHECK(strstr(out, "nothing to commit") == NULL &&
             strstr(out, "no changes added to commit") == NULL,
         "a resolved in-progress merge must print NO closing summary line, got:\n%s", out);
    free(repo_root);
}

/* The same resolved-merge state under -uno must ALSO suppress the summary --
   the -uno branch is a separate code path and needs its own coverage, not
   just the default branch above. */
static void test_bug_e_resolved_merge_suppresses_summary_uno(void)
{
    char out[8192];
    char argv0[] = "status";
    char argv1[] = "-uno";
    char *argv[2];
    char *repo_root = make_tmp_repo_and_cd("e_resolved_uno");

    commit_file("f.txt", "base\n", "base");
    write_merge_head();

    argv[0] = argv0;
    argv[1] = argv1;
    run_status_capture(2, argv, out, sizeof(out));
    CHECK(strstr(out, "All conflicts fixed but you are still merging.") != NULL,
         "expected the resolved-merge banner in:\n%s", out);
    CHECK(strstr(out, "nothing to commit") == NULL &&
             strstr(out, "no changes added to commit") == NULL,
         "a resolved in-progress merge under -uno must print NO closing summary line, got:\n%s",
         out);
    free(repo_root);
}

/* A merge in progress that STILL has unmerged entries ("You have unmerged
   paths.") must keep printing the closing summary as usual -- Bug E's
   suppression is specific to the "conflicts fixed" state, not to "a merge
   is in progress" in general. */
static void test_bug_e_unresolved_merge_still_prints_summary(void)
{
    char out[8192];
    char argv0[] = "status";
    char *argv[1];
    char *repo_root = make_tmp_repo_and_cd("e_unresolved");

    commit_file("f.txt", "base\n", "base");
    write_merge_head();
    write_conflict_index();

    argv[0] = argv0;
    run_status_capture(1, argv, out, sizeof(out));
    CHECK(strstr(out, "You have unmerged paths.") != NULL,
         "expected the unresolved-merge banner in:\n%s", out);
    CHECK(strstr(out, "no changes added to commit (use \"git add\" and/or \"git commit -a\")") !=
             NULL,
         "an unresolved merge must still print the closing summary line, got:\n%s", out);
    free(repo_root);
}

/* ---- Round 2, item A: the merge banner must use the SAME filtered
   unmerged count as Bug E's closing-line suppression, not an unfiltered
   sg_index_has_unmerged. Both are exercised on the same "conflict.txt is the
   only conflicted path, plus a clean tracked other.txt" shape, matching the
   interop phase38 B2 fixture. ---- */

static void test_bug_a_pathspec_matching_conflict_shows_unmerged_banner(void)
{
    char out[8192];
    char argv0[] = "status";
    char argv1[] = "--";
    char argv2[] = "conflict.txt";
    char *argv[3];
    char *repo_root = make_tmp_repo_and_cd("a_match");

    commit_file("other.txt", "base\n", "base");
    write_merge_head();
    write_conflict_index();

    argv[0] = argv0;
    argv[1] = argv1;
    argv[2] = argv2;
    run_status_capture(3, argv, out, sizeof(out));
    CHECK(strstr(out, "You have unmerged paths.") != NULL,
         "a pathspec matching the conflicted path must still show the unmerged banner, got:\n%s",
         out);
    free(repo_root);
}

/* A pathspec that does NOT match the conflicted path must see the same
   filtered unmerged count as print_unmerged: 0. Before this round's fix,
   the banner scanned idx unfiltered and printed "You have unmerged paths."
   here regardless of the pathspec. */
static void test_bug_a_pathspec_missing_conflict_shows_resolved_banner(void)
{
    char out[8192];
    char argv0[] = "status";
    char argv1[] = "--";
    char argv2[] = "other.txt";
    char *argv[3];
    char *repo_root = make_tmp_repo_and_cd("a_nomatch");

    commit_file("other.txt", "base\n", "base");
    write_merge_head();
    write_conflict_index();

    argv[0] = argv0;
    argv[1] = argv1;
    argv[2] = argv2;
    run_status_capture(3, argv, out, sizeof(out));
    CHECK(strstr(out, "All conflicts fixed but you are still merging.") != NULL,
         "a pathspec NOT matching the conflicted path must show the resolved banner, got:\n%s",
         out);
    CHECK(strstr(out, "You have unmerged paths.") == NULL,
         "a pathspec NOT matching the conflicted path must not show the unmerged banner, got:\n%s",
         out);
    /* Bug E's suppression also applies here: the filtered unmerged count is
       0, so no closing summary line either. */
    CHECK(strstr(out, "nothing to commit") == NULL &&
             strstr(out, "no changes added to commit") == NULL,
         "a resolved-by-pathspec merge must print NO closing summary line, got:\n%s", out);
    free(repo_root);
}

/* ---- Round 2, item B1's underlying fix: -uno's "Untracked files not
   listed" hedge must also print when a merge just resolved to a clean
   state (staged.count == 0 too), not only when something is staged --
   otherwise Bug E's suppression of the closing line leaves nothing printed
   at all in that state, which real git never does (measured against git
   2.55.0 on the mergebare/mergeuntr/mergeunst fixtures). ---- */
static void test_uno_untracked_hedge_prints_on_bare_resolved_merge(void)
{
    char out[8192];
    char argv0[] = "status";
    char argv1[] = "-uno";
    char *argv[2];
    char *repo_root = make_tmp_repo_and_cd("uno_bare_resolved");

    commit_file("f.txt", "base\n", "base");
    write_merge_head();

    argv[0] = argv0;
    argv[1] = argv1;
    run_status_capture(2, argv, out, sizeof(out));
    CHECK(strstr(out, "All conflicts fixed but you are still merging.") != NULL,
         "expected the resolved-merge banner in:\n%s", out);
    CHECK(strstr(out, "Untracked files not listed (use -u option to show untracked files)") !=
             NULL,
         "a bare resolved merge under -uno must still hedge about untracked files, got:\n%s",
         out);
    free(repo_root);
}

int main(void)
{
    test_priority1_unstaged_beats_untracked();
    test_priority2_untracked_beats_unborn();
    test_priority3_unborn_beats_clean();
    test_priority3_unborn_beats_uno_hedge();
    test_priority4_uno_hedge_when_head_exists();
    test_priority5_working_tree_clean();

    test_bug_e_resolved_merge_suppresses_summary();
    test_bug_e_resolved_merge_suppresses_summary_uno();
    test_bug_e_unresolved_merge_still_prints_summary();

    test_bug_a_pathspec_matching_conflict_shows_unmerged_banner();
    test_bug_a_pathspec_missing_conflict_shows_resolved_banner();
    test_uno_untracked_hedge_prints_on_bare_resolved_merge();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all status_long_format tests passed\n");
    return 0;
}
