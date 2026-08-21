/* Pins the single stage-combination -> two-letter-code/label table that
   cmd_status.c's unmerged_label() shares between `sg status`'s long format
   and `sg status --porcelain` (WP3). unmerged_label is a static helper
   inside cmd_status.c, so -- unlike a library-layer function -- it cannot
   be linked directly from a separate test TU; these tests go through the
   public sg_cmd_status entry point instead (the same pattern
   test_reflog_messages.c uses for other CLI-only assembly logic),
   capturing stdout to check the exact line each stage combination prints.
   Conflicts are built directly as raw sg_index entries (stage 1/2/3, no
   real merge/rebase needed) so all seven combinations are reachable without
   actually running a merge that produces them. */
#include "sg/cli.h"

#include "sg/hash.h"
#include "sg/index.h"
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

static char *make_tmp_repo_and_cd(void)
{
    static char template[] = "/tmp/sg_status_unmerged_test_XXXXXX";
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

/* Writes an index with one conflicted entry at "conflict.txt" carrying
   exactly the stages named 1/2/3 that are true in present1/present2/present3
   -- no stage-0 entry, so it is an unresolved conflict at every stage it
   has. */
static void write_conflict_index(int present1, int present2, int present3)
{
    sg_index idx;
    unsigned char blob_id[SG_SHA1_RAW_LEN];
    char *git_dir = sg_require_git_dir();
    int i;

    memset(&idx, 0, sizeof(idx));
    memset(blob_id, 0, sizeof(blob_id));

    for (i = 1; i <= 3; i++) {
        sg_index_entry entry;
        int present = i == 1 ? present1 : (i == 2 ? present2 : present3);

        if (!present)
            continue;
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

/* Runs sg_cmd_status with the given argv, capturing everything it writes to
   stdout into out (NUL-terminated, truncated to out_size - 1 if needed). */
static void run_status_capture(int argc, char **argv, char *out, size_t out_size)
{
    char tmpl[] = "/tmp/sg_status_unmerged_capture_XXXXXX";
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

static void check_combo(int present1, int present2, int present3, const char *expected_code,
                        const char *expected_label)
{
    char out[8192];
    char argv0[] = "status";
    char argv1[] = "--porcelain";
    char *long_argv[1];
    char *porcelain_argv[2];
    char expected_long[64];
    char expected_porcelain[64];

    long_argv[0] = argv0;
    porcelain_argv[0] = argv0;
    porcelain_argv[1] = argv1;

    write_conflict_index(present1, present2, present3);
    run_status_capture(1, long_argv, out, sizeof(out));
    snprintf(expected_long, sizeof(expected_long), "\t%-17sconflict.txt\n", expected_label);
    CHECK(strstr(out, expected_long) != NULL,
         "stages(%d,%d,%d): expected long-format line '%s' not found in:\n%s", present1, present2,
         present3, expected_long, out);

    write_conflict_index(present1, present2, present3);
    run_status_capture(2, porcelain_argv, out, sizeof(out));
    snprintf(expected_porcelain, sizeof(expected_porcelain), "%s conflict.txt\n", expected_code);
    CHECK(strstr(out, expected_porcelain) != NULL,
         "stages(%d,%d,%d): expected porcelain line '%s' not found in:\n%s", present1, present2,
         present3, expected_porcelain, out);
}

static void test_both_deleted(void)
{
    check_combo(1, 0, 0, "DD", "both deleted:");
}

static void test_deleted_by_them(void)
{
    check_combo(1, 1, 0, "UD", "deleted by them:");
}

static void test_both_modified(void)
{
    check_combo(1, 1, 1, "UU", "both modified:");
}

static void test_deleted_by_us(void)
{
    check_combo(1, 0, 1, "DU", "deleted by us:");
}

static void test_added_by_us(void)
{
    check_combo(0, 1, 0, "AU", "added by us:");
}

static void test_both_added(void)
{
    check_combo(0, 1, 1, "AA", "both added:");
}

static void test_added_by_them(void)
{
    check_combo(0, 0, 1, "UA", "added by them:");
}

int main(void)
{
    make_tmp_repo_and_cd();

    test_both_deleted();
    test_deleted_by_them();
    test_both_modified();
    test_deleted_by_us();
    test_added_by_us();
    test_both_added();
    test_added_by_them();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all status_unmerged tests passed\n");
    return 0;
}
