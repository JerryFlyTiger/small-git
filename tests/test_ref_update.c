#include "sg/refs.h"

#include "sg/reflog.h"
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

static char *make_tmp_repo(void)
{
    static char template[] = "/tmp/sg_ref_update_test_XXXXXX";
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

static void fill_id(unsigned char id[SG_SHA1_RAW_LEN], unsigned char byte)
{
    memset(id, byte, SG_SHA1_RAW_LEN);
}

static int reflog_count(const char *git_dir, const char *ref_path)
{
    sg_reflog log;
    int count;

    if (sg_reflog_read(git_dir, ref_path, &log) != 0)
        return -1;
    count = (int)log.count;
    sg_reflog_free(&log);
    return count;
}

static int logs_file_exists(const char *git_dir, const char *ref_path)
{
    char path[4096];
    struct stat st;

    snprintf(path, sizeof(path), "%s/logs/%s", git_dir, ref_path);
    return stat(path, &st) == 0;
}

static int logs_dir_exists(const char *git_dir)
{
    char path[4096];
    struct stat st;

    snprintf(path, sizeof(path), "%s/logs", git_dir);
    return stat(path, &st) == 0;
}

/* ---- 1: reflog_msg == NULL touches no log file at all -------------------- */

static void test_null_message_writes_no_log(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id_out[SG_SHA1_RAW_LEN];

    fill_id(id_out, 0xaa);

    CHECK(sg_ref_update(git_dir, "refs/heads/master", id_out, NULL) == 0, "sg_ref_update(NULL msg) failed");

    {
        unsigned char read_back[SG_SHA1_RAW_LEN];

        CHECK(sg_ref_read_path(git_dir, "refs/heads/master", read_back) == 0, "ref not written");
        CHECK(memcmp(read_back, id_out, SG_SHA1_RAW_LEN) == 0, "ref content mismatch");
    }

    CHECK(!logs_dir_exists(git_dir), "logs/ directory should not exist when reflog_msg is NULL");

    free(git_dir);
}

/* ---- 2: current branch + message -> branch log AND logs/HEAD, identical -- */

static void test_current_branch_updates_both_logs(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char new_id[SG_SHA1_RAW_LEN];
    sg_reflog branch_log;
    sg_reflog head_log;

    fill_id(new_id, 0x11);

    /* HEAD already points to "master" (sg_repo_init's default), and master
       has no commits yet -- old_id should read as all-zeros. */
    CHECK(sg_ref_update(git_dir, "refs/heads/master", new_id, "commit: first") == 0,
         "sg_ref_update on current branch failed");

    CHECK(reflog_count(git_dir, "refs/heads/master") == 1, "branch log should have exactly 1 line");
    CHECK(reflog_count(git_dir, "HEAD") == 1, "logs/HEAD should have exactly 1 line");

    CHECK(sg_reflog_read(git_dir, "refs/heads/master", &branch_log) == 0, "failed to read branch log");
    CHECK(sg_reflog_read(git_dir, "HEAD", &head_log) == 0, "failed to read HEAD log");

    if (branch_log.count == 1 && head_log.count == 1) {
        unsigned char zero[SG_SHA1_RAW_LEN];

        memset(zero, 0, sizeof(zero));
        CHECK(memcmp(branch_log.entries[0].old_id, zero, SG_SHA1_RAW_LEN) == 0,
             "branch log old_id should be all-zeros (ref just created)");
        CHECK(memcmp(branch_log.entries[0].new_id, new_id, SG_SHA1_RAW_LEN) == 0,
             "branch log new_id mismatch");
        CHECK(strcmp(branch_log.entries[0].message, "commit: first") == 0,
             "branch log message mismatch, got '%s'", branch_log.entries[0].message);

        CHECK(memcmp(head_log.entries[0].old_id, branch_log.entries[0].old_id, SG_SHA1_RAW_LEN) == 0,
             "HEAD log old_id should match branch log old_id exactly");
        CHECK(memcmp(head_log.entries[0].new_id, branch_log.entries[0].new_id, SG_SHA1_RAW_LEN) == 0,
             "HEAD log new_id should match branch log new_id exactly");
        CHECK(strcmp(head_log.entries[0].message, branch_log.entries[0].message) == 0,
             "HEAD log message should match branch log message exactly");
    }

    sg_reflog_free(&branch_log);
    sg_reflog_free(&head_log);
    free(git_dir);
}

/* ---- 3: non-current branch -> branch log grows, logs/HEAD untouched ------ */

static void test_non_current_branch_leaves_head_log_alone(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id2[SG_SHA1_RAW_LEN];

    fill_id(id2, 0x22);

    /* HEAD stays on "master" the whole time; "feature" is created but never
       switched to. */
    CHECK(sg_ref_update(git_dir, "refs/heads/feature", id2, "branch: feature created") == 0,
         "sg_ref_update on non-current branch failed");

    CHECK(reflog_count(git_dir, "refs/heads/feature") == 1, "feature's own log should have 1 line");
    CHECK(!logs_file_exists(git_dir, "HEAD"), "logs/HEAD should not exist: master was never touched");

    free(git_dir);
}

/* ---- 4: old == new on the current branch -> branch log unchanged, ------- */
/*         logs/HEAD still gets a line (rule 1's asymmetry)                  */

static void test_noop_on_current_branch_still_logs_to_head(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id1[SG_SHA1_RAW_LEN];

    fill_id(id1, 0x33);

    /* Set master = id1 with NO message first, so no reflog exists yet. */
    CHECK(sg_ref_update(git_dir, "refs/heads/master", id1, NULL) == 0, "initial NULL-message update failed");
    CHECK(!logs_dir_exists(git_dir), "no logs/ yet after a NULL-message update");

    /* Now update it again to the SAME value, this time with a message. */
    CHECK(sg_ref_update(git_dir, "refs/heads/master", id1, "no-op update") == 0,
         "no-op sg_ref_update with message failed");

    CHECK(reflog_count(git_dir, "refs/heads/master") == 0,
         "branch's own log must stay empty on a no-op update (rule 1)");
    CHECK(reflog_count(git_dir, "HEAD") == 1,
         "logs/HEAD must still gain a line even though the branch update was a no-op (rule 1's asymmetry)");

    {
        sg_reflog head_log;

        CHECK(sg_reflog_read(git_dir, "HEAD", &head_log) == 0, "failed to read HEAD log");
        if (head_log.count == 1) {
            CHECK(memcmp(head_log.entries[0].old_id, id1, SG_SHA1_RAW_LEN) == 0,
                 "HEAD log old_id should equal id1");
            CHECK(memcmp(head_log.entries[0].new_id, id1, SG_SHA1_RAW_LEN) == 0,
                 "HEAD log new_id should equal id1");
        }
        sg_reflog_free(&head_log);
    }

    free(git_dir);
}

/* ---- 5: policy violation -> -1, and NOTHING is written -------------------- */

static void test_policy_violation_writes_nothing(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id[SG_SHA1_RAW_LEN];

    fill_id(id, 0x44);

    CHECK(sg_ref_update(git_dir, "refs/tags/v1", id, "tag: v1") == -1,
         "sg_ref_update on refs/tags/* with a message should be rejected");
    {
        unsigned char read_back[SG_SHA1_RAW_LEN];

        CHECK(sg_ref_read_path(git_dir, "refs/tags/v1", read_back) != 0,
             "refs/tags/v1 must not exist after a rejected update");
    }
    CHECK(!logs_file_exists(git_dir, "refs/tags/v1"), "no log should be written for a rejected update");

    CHECK(sg_ref_update(git_dir, "refs/sg/chunks", id, "chunk: keepalive") == -1,
         "sg_ref_update on refs/sg/chunks with a message should be rejected");
    {
        unsigned char read_back[SG_SHA1_RAW_LEN];

        CHECK(sg_ref_read_path(git_dir, "refs/sg/chunks", read_back) != 0,
             "refs/sg/chunks must not exist after a rejected update");
    }
    CHECK(!logs_file_exists(git_dir, "refs/sg/chunks"), "no log should be written for a rejected update");

    free(git_dir);
}

/* ---- 5b: rollback when the ref file itself cannot be written ------------- */

/* Forces sg_ref_update's write_ref_path_raw failure path by chmod'ing an
   EXISTING refs/heads/master read-only right before a message-bearing call:
   write_ref_path_raw goes through sg_write_file_mkdirs, which fopen(...,
   "wb")'s the file in place (same technique as tests/test_stash.c's
   test_drop_rolls_back_on_ref_write_failure), so only the file's own write
   bit needs to be revoked. Asserts BOTH logs are rolled back to their exact
   pre-call length (not just that the call failed) and that the ref still
   reads back as the OLD value. */
static void test_ref_update_rolls_back_reflog_on_write_failure(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id_a[SG_SHA1_RAW_LEN];
    unsigned char id_b[SG_SHA1_RAW_LEN];
    char ref_path[4096];
    int branch_count_before;
    int head_count_before;

    if (geteuid() == 0) {
        /* root ignores file modes; the test would be meaningless */
        free(git_dir);
        return;
    }

    fill_id(id_a, 0x61);
    fill_id(id_b, 0x62);

    CHECK(sg_ref_update(git_dir, "refs/heads/master", id_a, NULL) == 0,
         "baseline NULL-message update failed");

    branch_count_before = reflog_count(git_dir, "refs/heads/master");
    head_count_before = reflog_count(git_dir, "HEAD");
    CHECK(branch_count_before == 0, "no branch reflog should exist yet before the chmod");
    CHECK(head_count_before == 0, "no logs/HEAD should exist yet before the chmod");

    snprintf(ref_path, sizeof(ref_path), "%s/refs/heads/master", git_dir);
    CHECK(chmod(ref_path, 0444) == 0, "chmod refs/heads/master read-only failed");

    CHECK(sg_ref_update(git_dir, "refs/heads/master", id_b, "should fail: read-only ref") == -1,
         "sg_ref_update should fail once refs/heads/master cannot be rewritten");

    chmod(ref_path, 0644); /* restore before any further reads/writes */

    CHECK(reflog_count(git_dir, "refs/heads/master") == branch_count_before,
         "branch reflog must be rolled back to its pre-call length");
    CHECK(reflog_count(git_dir, "HEAD") == head_count_before,
         "logs/HEAD must be rolled back to its pre-call length");

    {
        unsigned char read_back[SG_SHA1_RAW_LEN];

        CHECK(sg_ref_read_path(git_dir, "refs/heads/master", read_back) == 0, "ref should still be readable");
        CHECK(memcmp(read_back, id_a, SG_SHA1_RAW_LEN) == 0,
             "ref should still hold the OLD value after the rolled-back write");
    }

    free(git_dir);
}

/* ---- 5c: the three allowed-namespaces branches of ref_path_reflog_allowed
   that no caller reaches with a non-NULL message today (HEAD, refs/remotes/
   (star), refs/stash) -- batch B/D will depend on them, so pin them here instead of
   leaving them exercised only by inspection. Calling sg_ref_update directly
   on ref_path "HEAD" bypasses sg_ref_set_head's symbolic-ref handling (it
   will overwrite git_dir/HEAD with a raw oid instead of "ref: refs/heads/
   ..."), which is fine for an isolated, freed-immediately-after temp repo:
   the point here is exercising the reflog-namespace check, not HEAD's
   symbolic-ref semantics (already covered by sg_ref_set_head's own tests). */

static void test_ref_update_head_path_writes_log(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id[SG_SHA1_RAW_LEN];
    unsigned char zero[SG_SHA1_RAW_LEN];
    sg_reflog log;

    fill_id(id, 0x71);
    memset(zero, 0, sizeof(zero));

    CHECK(sg_ref_update(git_dir, "HEAD", id, "test: HEAD path") == 0,
         "sg_ref_update on ref_path \"HEAD\" should be allowed");

    CHECK(logs_file_exists(git_dir, "HEAD"), "logs/HEAD should have been created");
    CHECK(sg_reflog_read(git_dir, "HEAD", &log) == 0, "failed to read logs/HEAD");
    CHECK(log.count == 1, "logs/HEAD should have exactly 1 line, got %zu", log.count);
    if (log.count == 1) {
        CHECK(memcmp(log.entries[0].old_id, zero, SG_SHA1_RAW_LEN) == 0,
             "old_id should be all-zeros: HEAD wasn't a valid oid before");
        CHECK(memcmp(log.entries[0].new_id, id, SG_SHA1_RAW_LEN) == 0,
             "new_id should match the id just written");
        CHECK(strcmp(log.entries[0].message, "test: HEAD path") == 0,
             "message mismatch, got '%s'", log.entries[0].message);
    }
    sg_reflog_free(&log);

    free(git_dir);
}

static void test_ref_update_remote_tracking_path_writes_log(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id[SG_SHA1_RAW_LEN];
    unsigned char zero[SG_SHA1_RAW_LEN];
    sg_reflog log;

    fill_id(id, 0x72);
    memset(zero, 0, sizeof(zero));

    CHECK(sg_ref_update(git_dir, "refs/remotes/origin/master", id, "test: remote-tracking path") == 0,
         "sg_ref_update on a refs/remotes/ path should be allowed");

    CHECK(logs_file_exists(git_dir, "refs/remotes/origin/master"),
         "logs/refs/remotes/origin/master should have been created");
    CHECK(sg_reflog_read(git_dir, "refs/remotes/origin/master", &log) == 0,
         "failed to read the remote-tracking log");
    CHECK(log.count == 1, "expected exactly 1 line, got %zu", log.count);
    if (log.count == 1) {
        CHECK(memcmp(log.entries[0].old_id, zero, SG_SHA1_RAW_LEN) == 0,
             "old_id should be all-zeros: the ref didn't exist before");
        CHECK(memcmp(log.entries[0].new_id, id, SG_SHA1_RAW_LEN) == 0, "new_id mismatch");
        CHECK(strcmp(log.entries[0].message, "test: remote-tracking path") == 0,
             "message mismatch, got '%s'", log.entries[0].message);
    }
    sg_reflog_free(&log);

    free(git_dir);
}

static void test_ref_update_stash_path_writes_log(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id[SG_SHA1_RAW_LEN];
    unsigned char zero[SG_SHA1_RAW_LEN];
    sg_reflog log;

    fill_id(id, 0x73);
    memset(zero, 0, sizeof(zero));

    CHECK(sg_ref_update(git_dir, "refs/stash", id, "test: stash path") == 0,
         "sg_ref_update on \"refs/stash\" should be allowed");

    CHECK(logs_file_exists(git_dir, "refs/stash"), "logs/refs/stash should have been created");
    CHECK(sg_reflog_read(git_dir, "refs/stash", &log) == 0, "failed to read the stash log");
    CHECK(log.count == 1, "expected exactly 1 line, got %zu", log.count);
    if (log.count == 1) {
        CHECK(memcmp(log.entries[0].old_id, zero, SG_SHA1_RAW_LEN) == 0,
             "old_id should be all-zeros: refs/stash didn't exist before");
        CHECK(memcmp(log.entries[0].new_id, id, SG_SHA1_RAW_LEN) == 0, "new_id mismatch");
        CHECK(strcmp(log.entries[0].message, "test: stash path") == 0,
             "message mismatch, got '%s'", log.entries[0].message);
    }
    sg_reflog_free(&log);

    free(git_dir);
}

/* ---- 6: sg_ref_set_head --------------------------------------------------- */

static void test_set_head_no_commits_yet(void)
{
    char *git_dir = make_tmp_repo();
    char *branch;

    /* target branch has no commits at all: must not crash, must succeed. */
    CHECK(sg_ref_set_head(git_dir, "nobranch", NULL) == 0, "sg_ref_set_head to a commit-less branch failed");

    branch = sg_ref_current_branch(git_dir);
    CHECK(branch != NULL && strcmp(branch, "nobranch") == 0, "HEAD did not move to 'nobranch'");
    free(branch);

    free(git_dir);
}

static void test_set_head_noop_with_message_still_logs(void)
{
    char *git_dir = make_tmp_repo();

    /* Fresh repo: HEAD is unborn (resolves to nothing) and "master" (the
       target) also has no commits -- both old and new resolve to
       all-zeros, i.e. old == new, yet the line must still be written
       (HEAD's log is never no-op suppressed). */
    CHECK(sg_ref_set_head(git_dir, "master", "switch to 'master'") == 0,
         "sg_ref_set_head with a message failed");

    CHECK(reflog_count(git_dir, "HEAD") == 1, "logs/HEAD should gain exactly 1 line");
    {
        sg_reflog head_log;
        unsigned char zero[SG_SHA1_RAW_LEN];

        memset(zero, 0, sizeof(zero));
        CHECK(sg_reflog_read(git_dir, "HEAD", &head_log) == 0, "failed to read HEAD log");
        if (head_log.count == 1) {
            CHECK(memcmp(head_log.entries[0].old_id, zero, SG_SHA1_RAW_LEN) == 0,
                 "old_id should be all-zeros: HEAD was unborn");
            CHECK(memcmp(head_log.entries[0].new_id, zero, SG_SHA1_RAW_LEN) == 0,
                 "new_id should be all-zeros: target branch has no commits");
        }
        sg_reflog_free(&head_log);
    }

    free(git_dir);
}

/* Same rollback contract as sg_ref_update, but forcing sg_ref_set_head's own
   write failure: chmod git_dir/HEAD read-only right before a message-bearing
   call (sg_ref_set_head fopen(..., "wb")'s HEAD directly, not through
   sg_write_file_mkdirs, but the effect on an EXISTING file is identical). */
static void test_set_head_rolls_back_reflog_on_head_write_failure(void)
{
    char *git_dir = make_tmp_repo();
    char head_path[4096];
    int head_count_before;

    if (geteuid() == 0) {
        /* root ignores file modes; the test would be meaningless */
        free(git_dir);
        return;
    }

    head_count_before = reflog_count(git_dir, "HEAD");
    CHECK(head_count_before == 0, "no logs/HEAD should exist yet before the chmod");

    snprintf(head_path, sizeof(head_path), "%s/HEAD", git_dir);
    CHECK(chmod(head_path, 0444) == 0, "chmod HEAD read-only failed");

    CHECK(sg_ref_set_head(git_dir, "master", "should fail: read-only HEAD") == -1,
         "sg_ref_set_head should fail once HEAD cannot be rewritten");

    chmod(head_path, 0644); /* restore before any further reads/writes */

    CHECK(reflog_count(git_dir, "HEAD") == head_count_before,
         "logs/HEAD must be rolled back to its pre-call length");

    free(git_dir);
}

/* ---- 6b: old_id must be read BEFORE HEAD moves, not after ----------------
   test_set_head_noop_with_message_still_logs above is a false-coverage trap:
   it uses a brand new repo where HEAD is unborn and the target branch also
   has no commits, so old_id and new_id are BOTH all-zeros no matter whether
   old_id is read before or after the HEAD file is rewritten -- a mutation
   that moved the read to after the write would still pass it. This test
   gives the two branches genuinely different tips so the ordering is
   actually discriminating. */
static void test_set_head_logs_correct_old_and_new_tips(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char tip_a[SG_SHA1_RAW_LEN];
    unsigned char tip_b[SG_SHA1_RAW_LEN];

    fill_id(tip_a, 0x81);
    fill_id(tip_b, 0x82);

    /* master is HEAD's target right out of sg_repo_init. */
    CHECK(sg_ref_update(git_dir, "refs/heads/master", tip_a, NULL) == 0, "master tip setup failed");
    CHECK(sg_ref_update(git_dir, "refs/heads/other", tip_b, NULL) == 0, "other tip setup failed");

    {
        char *cur = sg_ref_current_branch(git_dir);

        CHECK(cur != NULL && strcmp(cur, "master") == 0, "HEAD should still be on 'master' before the move");
        free(cur);
    }

    CHECK(sg_ref_set_head(git_dir, "other", "switch to 'other'") == 0, "sg_ref_set_head failed");

    CHECK(reflog_count(git_dir, "HEAD") == 1, "logs/HEAD should have exactly 1 line");
    {
        sg_reflog head_log;

        CHECK(sg_reflog_read(git_dir, "HEAD", &head_log) == 0, "failed to read HEAD log");
        if (head_log.count == 1) {
            CHECK(memcmp(head_log.entries[0].old_id, tip_a, SG_SHA1_RAW_LEN) == 0,
                 "old_id should be master's tip (the branch HEAD was on before the move)");
            CHECK(memcmp(head_log.entries[0].new_id, tip_b, SG_SHA1_RAW_LEN) == 0,
                 "new_id should be other's tip (the branch HEAD moved to)");
        }
        sg_reflog_free(&head_log);
    }

    {
        char *cur = sg_ref_current_branch(git_dir);

        CHECK(cur != NULL && strcmp(cur, "other") == 0, "HEAD should now be on 'other'");
        free(cur);
    }

    free(git_dir);
}

/* ---- 7: sg_reflog_at ------------------------------------------------------ */

static void test_reflog_at(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id1[SG_SHA1_RAW_LEN];
    unsigned char id2[SG_SHA1_RAW_LEN];
    unsigned char id3[SG_SHA1_RAW_LEN];
    unsigned char zero[SG_SHA1_RAW_LEN];
    sg_reflog log;

    fill_id(id1, 0x51);
    fill_id(id2, 0x52);
    fill_id(id3, 0x53);
    memset(zero, 0, sizeof(zero));

    CHECK(sg_reflog_append(git_dir, "refs/heads/atlog", zero, id1, "first", NULL) == 0, "append 1 failed");
    CHECK(sg_reflog_append(git_dir, "refs/heads/atlog", id1, id2, "second", NULL) == 0, "append 2 failed");
    CHECK(sg_reflog_append(git_dir, "refs/heads/atlog", id2, id3, "third", NULL) == 0, "append 3 failed");

    CHECK(sg_reflog_read(git_dir, "refs/heads/atlog", &log) == 0, "failed to read atlog");
    CHECK(log.count == 3, "expected 3 entries, got %zu", log.count);

    {
        const sg_reflog_entry *e0 = sg_reflog_at(&log, 0);
        const sg_reflog_entry *e2 = sg_reflog_at(&log, 2);
        const sg_reflog_entry *e_oob = sg_reflog_at(&log, 3);

        CHECK(e0 != NULL && memcmp(e0->new_id, id3, SG_SHA1_RAW_LEN) == 0,
             "@{0} should be the most recent entry's new_id (id3)");
        CHECK(e2 != NULL && memcmp(e2->new_id, id1, SG_SHA1_RAW_LEN) == 0,
             "@{2} should be the oldest entry's new_id (id1)");
        CHECK(e_oob == NULL, "@{3} (== count) should be out of range and return NULL");
    }

    {
        sg_reflog empty_log;

        empty_log.entries = NULL;
        empty_log.count = 0;
        CHECK(sg_reflog_at(&empty_log, 0) == NULL, "@{0} on an empty log should return NULL");
    }

    sg_reflog_free(&log);
    free(git_dir);
}

int main(void)
{
    test_null_message_writes_no_log();
    test_current_branch_updates_both_logs();
    test_non_current_branch_leaves_head_log_alone();
    test_noop_on_current_branch_still_logs_to_head();
    test_policy_violation_writes_nothing();
    test_ref_update_rolls_back_reflog_on_write_failure();
    test_ref_update_head_path_writes_log();
    test_ref_update_remote_tracking_path_writes_log();
    test_ref_update_stash_path_writes_log();
    test_set_head_no_commits_yet();
    test_set_head_noop_with_message_still_logs();
    test_set_head_rolls_back_reflog_on_head_write_failure();
    test_set_head_logs_correct_old_and_new_tips();
    test_reflog_at();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all ref_update tests passed\n");
    return 0;
}
