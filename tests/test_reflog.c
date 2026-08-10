#include "sg/reflog.h"

#include "sg/hash.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/workdir.h"

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
    static char template[] = "/tmp/sg_reflog_test_XXXXXX";
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

static void fill_id(unsigned char id[SG_SHA1_RAW_LEN], unsigned char b)
{
    memset(id, b, SG_SHA1_RAW_LEN);
}

/* ---- append -> read round trip -------------------------------------- */

static void test_append_read_round_trip(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char old_id[SG_SHA1_RAW_LEN];
    unsigned char new_id[SG_SHA1_RAW_LEN];
    sg_reflog log;
    long long appended_at = -1;

    fill_id(old_id, 0x00);
    fill_id(new_id, 0xab);

    CHECK(sg_reflog_append(git_dir, "refs/stash", old_id, new_id, "first entry", &appended_at) == 0,
         "append failed");
    CHECK(appended_at == 0, "expected append_at 0 for a fresh file, got %lld", appended_at);

    CHECK(sg_reflog_read(git_dir, "refs/stash", &log) == 0, "read failed");
    CHECK(log.count == 1, "expected 1 entry, got %zu", log.count);
    if (log.count == 1) {
        CHECK(memcmp(log.entries[0].old_id, old_id, SG_SHA1_RAW_LEN) == 0, "old_id mismatch");
        CHECK(memcmp(log.entries[0].new_id, new_id, SG_SHA1_RAW_LEN) == 0, "new_id mismatch");
        CHECK(strcmp(log.entries[0].message, "first entry") == 0, "message mismatch: %s",
             log.entries[0].message);
        CHECK(strstr(log.entries[0].ident, "small_git") != NULL || strstr(log.entries[0].ident, "<") != NULL,
             "ident looks wrong: %s", log.entries[0].ident);
    }
    sg_reflog_free(&log);

    /* second append -- appended_at should reflect the first line's length */
    fill_id(old_id, 0xab);
    fill_id(new_id, 0xcd);
    CHECK(sg_reflog_append(git_dir, "refs/stash", old_id, new_id, "second entry", &appended_at) == 0,
         "second append failed");
    CHECK(appended_at > 0, "expected appended_at > 0 on second append, got %lld", appended_at);

    CHECK(sg_reflog_read(git_dir, "refs/stash", &log) == 0, "read failed after second append");
    CHECK(log.count == 2, "expected 2 entries, got %zu", log.count);
    if (log.count == 2) {
        CHECK(strcmp(log.entries[1].message, "second entry") == 0, "second message mismatch: %s",
             log.entries[1].message);
    }
    sg_reflog_free(&log);

    free(git_dir);
}

/* ---- reading a hand-written real-git-format file ---------------------- */

static void test_read_hand_written_git_format(void)
{
    char *git_dir = make_tmp_repo();
    char path[4096];
    FILE *f;
    sg_reflog log;
    unsigned char expect_old[SG_SHA1_RAW_LEN];
    unsigned char expect_new[SG_SHA1_RAW_LEN];

    memset(expect_old, 0, SG_SHA1_RAW_LEN);
    memset(expect_new, 0x11, SG_SHA1_RAW_LEN);

    snprintf(path, sizeof(path), "%s/logs/refs/stash", git_dir);
    CHECK(sg_mkdir_parents(path) == 0, "mkdir_parents failed");
    f = fopen(path, "wb");
    CHECK(f != NULL, "failed to open %s for writing", path);
    if (f != NULL) {
        fprintf(f, "0000000000000000000000000000000000000000 "
                   "1111111111111111111111111111111111111111 "
                   "A Name With Spaces <a@e.com> 1786357285 +0800\tOn master: msg: with colon\n");
        fclose(f);
    }

    CHECK(sg_reflog_read(git_dir, "refs/stash", &log) == 0, "read failed");
    CHECK(log.count == 1, "expected 1 entry, got %zu", log.count);
    if (log.count == 1) {
        CHECK(memcmp(log.entries[0].old_id, expect_old, SG_SHA1_RAW_LEN) == 0, "old_id mismatch");
        CHECK(memcmp(log.entries[0].new_id, expect_new, SG_SHA1_RAW_LEN) == 0, "new_id mismatch");
        CHECK(strcmp(log.entries[0].ident, "A Name With Spaces <a@e.com> 1786357285 +0800") == 0,
             "ident mismatch: %s", log.entries[0].ident);
        CHECK(strcmp(log.entries[0].message, "On master: msg: with colon") == 0, "message mismatch: %s",
             log.entries[0].message);
    }
    sg_reflog_free(&log);
    free(git_dir);
}

/* ---- last line without a trailing newline ----------------------------- */

static void test_last_line_without_newline(void)
{
    char *git_dir = make_tmp_repo();
    char path[4096];
    FILE *f;
    sg_reflog log;

    snprintf(path, sizeof(path), "%s/logs/refs/stash", git_dir);
    CHECK(sg_mkdir_parents(path) == 0, "mkdir_parents failed");
    f = fopen(path, "wb");
    CHECK(f != NULL, "failed to open %s for writing", path);
    if (f != NULL) {
        /* no trailing \n */
        fprintf(f, "0000000000000000000000000000000000000000 "
                   "2222222222222222222222222222222222222222 "
                   "T U <t@e.com> 1786357285 +0000\tno newline here");
        fclose(f);
    }

    CHECK(sg_reflog_read(git_dir, "refs/stash", &log) == 0, "read failed");
    CHECK(log.count == 1, "expected 1 entry, got %zu", log.count);
    if (log.count == 1)
        CHECK(strcmp(log.entries[0].message, "no newline here") == 0, "message mismatch: %s",
             log.entries[0].message);
    sg_reflog_free(&log);
    free(git_dir);
}

/* ---- missing file -> 0, empty ------------------------------------------ */

static void test_missing_file_is_not_an_error(void)
{
    char *git_dir = make_tmp_repo();
    sg_reflog log;

    log.entries = (sg_reflog_entry *)0x1; /* poison, to prove it gets reset */
    log.count = 99;

    CHECK(sg_reflog_read(git_dir, "refs/stash", &log) == 0, "missing file should not be an error");
    CHECK(log.entries == NULL, "expected NULL entries for missing file");
    CHECK(log.count == 0, "expected count 0 for missing file, got %zu", log.count);

    sg_reflog_free(&log);
    free(git_dir);
}

/* ---- unreadable (not missing) file -> -1, never treated as "empty" ----- */

/* A file that EXISTS but cannot be opened (permission denied) is a real I/O
   failure, not "no reflog yet" -- sg_reflog_read must distinguish the two
   (ENOENT vs. everything else), per its header comment. Chmod'ing the file
   itself to 0000 forces exactly that: sg_read_file's fopen fails with
   EACCES, not ENOENT. */
static void test_unreadable_file_is_an_error(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char old_id[SG_SHA1_RAW_LEN];
    unsigned char new_id[SG_SHA1_RAW_LEN];
    char path[4096];
    sg_reflog log;

    if (geteuid() == 0) {
        /* root ignores file modes; the test would be meaningless */
        free(git_dir);
        return;
    }

    fill_id(old_id, 0x00);
    fill_id(new_id, 0x01);
    CHECK(sg_reflog_append(git_dir, "refs/stash", old_id, new_id, "entry", NULL) == 0, "append failed");

    snprintf(path, sizeof(path), "%s/logs/refs/stash", git_dir);
    CHECK(chmod(path, 0000) == 0, "chmod 0000 failed");

    log.entries = (sg_reflog_entry *)0x1; /* poison */
    log.count = 99;
    CHECK(sg_reflog_read(git_dir, "refs/stash", &log) == -1,
         "an unreadable-but-present reflog file must be reported as -1, not treated as empty");

    chmod(path, 0644); /* restore before cleanup */
    free(git_dir);
}

/* ---- 39-hex oid -> -1 --------------------------------------------------- */

static void test_short_oid_is_malformed(void)
{
    char *git_dir = make_tmp_repo();
    char path[4096];
    FILE *f;
    sg_reflog log;

    snprintf(path, sizeof(path), "%s/logs/refs/stash", git_dir);
    CHECK(sg_mkdir_parents(path) == 0, "mkdir_parents failed");
    f = fopen(path, "wb");
    CHECK(f != NULL, "failed to open %s for writing", path);
    if (f != NULL) {
        /* old-oid has only 39 hex digits */
        fprintf(f, "000000000000000000000000000000000000 "
                   "1111111111111111111111111111111111111111 "
                   "T U <t@e.com> 1786357285 +0000\tbad\n");
        fclose(f);
    }

    CHECK(sg_reflog_read(git_dir, "refs/stash", &log) == -1, "expected -1 for a 39-hex oid");
    free(git_dir);
}

/* ---- message normalization table --------------------------------------- */

static void check_normalized(const char *git_dir, const char *input, const char *expected)
{
    unsigned char old_id[SG_SHA1_RAW_LEN];
    unsigned char new_id[SG_SHA1_RAW_LEN];
    sg_reflog log;

    fill_id(old_id, 0x00);
    fill_id(new_id, 0x01);

    CHECK(sg_reflog_append(git_dir, "refs/norm", old_id, new_id, input, NULL) == 0,
         "append failed for input %s", input);
    CHECK(sg_reflog_read(git_dir, "refs/norm", &log) == 0, "read failed");
    CHECK(log.count >= 1, "expected at least 1 entry");
    if (log.count >= 1) {
        sg_reflog_entry *last = &log.entries[log.count - 1];

        CHECK(strcmp(last->message, expected) == 0, "normalize(%s): expected \"%s\", got \"%s\"", input,
             expected, last->message);
    }
    sg_reflog_free(&log);
}

static void test_message_normalization(void)
{
    char *git_dir = make_tmp_repo();

    check_normalized(git_dir, "a\nb", "a b");
    check_normalized(git_dir, "  a  \t b  ", "a b");
    check_normalized(git_dir, "\n\n", "");

    free(git_dir);
}

/* ---- rewrite re-chains old-ids and preserves ident/message verbatim --- */

static void test_rewrite_rechains_and_preserves_ident(void)
{
    char *git_dir = make_tmp_repo();
    sg_reflog_entry entries[3];
    sg_reflog log;
    unsigned char zero_id[SG_SHA1_RAW_LEN];
    size_t i;

    memset(zero_id, 0, SG_SHA1_RAW_LEN);

    for (i = 0; i < 3; i++) {
        fill_id(entries[i].old_id, 0x00); /* garbage on input -- rewrite must regenerate */
        fill_id(entries[i].new_id, (unsigned char)(0x10 + i));
        entries[i].ident = strdup("Weird Git Author <weird@example.com> 1700000000 +0800");
        entries[i].message = strdup("kept verbatim");
    }

    CHECK(sg_reflog_rewrite(git_dir, "refs/stash", entries, 3) == 0, "rewrite failed");

    CHECK(sg_reflog_read(git_dir, "refs/stash", &log) == 0, "read after rewrite failed");
    CHECK(log.count == 3, "expected 3 entries after rewrite, got %zu", log.count);
    if (log.count == 3) {
        CHECK(memcmp(log.entries[0].old_id, zero_id, SG_SHA1_RAW_LEN) == 0,
             "entry 0's old_id should be all zero after rechaining");
        for (i = 1; i < 3; i++) {
            CHECK(memcmp(log.entries[i].old_id, entries[i - 1].new_id, SG_SHA1_RAW_LEN) == 0,
                 "entry %zu's old_id should equal entry %zu's new_id", i, i - 1);
        }
        for (i = 0; i < 3; i++) {
            CHECK(strcmp(log.entries[i].ident, "Weird Git Author <weird@example.com> 1700000000 +0800") ==
                     0,
                 "entry %zu's ident was not preserved verbatim: %s", i, log.entries[i].ident);
            CHECK(strcmp(log.entries[i].message, "kept verbatim") == 0, "entry %zu's message mismatch: %s",
                 i, log.entries[i].message);
        }
    }
    sg_reflog_free(&log);

    for (i = 0; i < 3; i++) {
        free(entries[i].ident);
        free(entries[i].message);
    }
    free(git_dir);
}

/* ---- count 0 removes the file ------------------------------------------ */

static void test_rewrite_count_zero_removes_file(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char old_id[SG_SHA1_RAW_LEN];
    unsigned char new_id[SG_SHA1_RAW_LEN];
    char path[4096];
    sg_reflog log;
    FILE *f;

    fill_id(old_id, 0x00);
    fill_id(new_id, 0x01);
    CHECK(sg_reflog_append(git_dir, "refs/stash", old_id, new_id, "one entry", NULL) == 0,
         "append failed");

    snprintf(path, sizeof(path), "%s/logs/refs/stash", git_dir);
    f = fopen(path, "rb");
    CHECK(f != NULL, "expected the reflog file to exist before rewrite");
    if (f != NULL)
        fclose(f);

    CHECK(sg_reflog_rewrite(git_dir, "refs/stash", NULL, 0) == 0, "rewrite with count 0 failed");

    f = fopen(path, "rb");
    CHECK(f == NULL, "expected the reflog file to be removed after count-0 rewrite");
    if (f != NULL)
        fclose(f);

    CHECK(sg_reflog_read(git_dir, "refs/stash", &log) == 0, "read after count-0 rewrite failed");
    CHECK(log.count == 0, "expected count 0 after count-0 rewrite, got %zu", log.count);
    sg_reflog_free(&log);

    free(git_dir);
}

int main(void)
{
    test_append_read_round_trip();
    test_read_hand_written_git_format();
    test_last_line_without_newline();
    test_missing_file_is_not_an_error();
    test_unreadable_file_is_an_error();
    test_short_oid_is_malformed();
    test_message_normalization();
    test_rewrite_rechains_and_preserves_ident();
    test_rewrite_count_zero_removes_file();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all reflog tests passed\n");
    return 0;
}
