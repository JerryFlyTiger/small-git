#include "sg/refs.h"

#include "sg/hash.h"
#include "sg/reflog.h"
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
    static char template[] = "/tmp/sg_refs_test_XXXXXX";
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

/* Writes a minimal packed-refs file (no '#' header line -- read_packed_ref/
   list_packed_under skip those but don't require one) with exactly the
   given "<hex> <refname>\n" lines, replacing whatever packed-refs already
   existed. */
static void write_packed_refs(const char *git_dir, const char *const *lines, size_t count)
{
    char path[4096];
    char *buf;
    size_t cap = 4096;
    size_t used = 0;
    size_t i;

    buf = malloc(cap);
    CHECK(buf != NULL, "oom building packed-refs content");
    for (i = 0; i < count; i++) {
        size_t len = strlen(lines[i]);

        while (used + len + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            CHECK(buf != NULL, "oom growing packed-refs content");
        }
        memcpy(buf + used, lines[i], len);
        used += len;
        buf[used++] = '\n';
    }

    snprintf(path, sizeof(path), "%s/packed-refs", git_dir);
    CHECK(sg_write_file_mkdirs(path, (const unsigned char *)buf, used, 0644) == 0,
         "failed to write packed-refs");
    free(buf);
}

static void hex_line(char *out, size_t out_len, const unsigned char id[SG_SHA1_RAW_LEN],
                     const char *ref_name)
{
    char hex[SG_SHA1_HEX_LEN + 1];

    sg_sha1_to_hex(id, hex);
    snprintf(out, out_len, "%s %s", hex, ref_name);
}

static int names_contains(char **names, size_t count, const char *name)
{
    size_t i;

    for (i = 0; i < count; i++) {
        if (strcmp(names[i], name) == 0)
            return 1;
    }
    return 0;
}

static void free_names(char **names, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++)
        free(names[i]);
    free(names);
}

/* refs/headsX/somebranch shares the literal string "refs/heads" as a
   PREFIX of its path, but it is NOT under the refs/heads/ namespace (there
   is no '/' right after "refs/heads"). A prefix filter that compared only
   strlen("refs/heads/") - 1 bytes (dropping the trailing slash) would let
   this line leak into `sg branch`'s listing; comparing the full
   "refs/heads/" (trailing slash included) correctly excludes it. */
static void test_list_excludes_prefix_collision(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char collision_id[SG_SHA1_RAW_LEN];
    unsigned char nested_id[SG_SHA1_RAW_LEN];
    char collision_line[256];
    char nested_line[256];
    const char *lines[2];
    char **names;
    size_t count;

    fill_id(collision_id, 0x11);
    fill_id(nested_id, 0x22);

    hex_line(collision_line, sizeof(collision_line), collision_id, "refs/headsX/somebranch");
    hex_line(nested_line, sizeof(nested_line), nested_id, "refs/heads/feature/x");
    lines[0] = collision_line;
    lines[1] = nested_line;
    write_packed_refs(git_dir, lines, 2);

    CHECK(sg_ref_list_branches(git_dir, &names, &count) == 0, "sg_ref_list_branches failed");

    /* Exactly one legitimate branch was packed (feature/x); anything else
       in the result -- regardless of exactly how a broken prefix filter
       mis-slices the colliding refs/headsX/somebranch line's suffix --
       means the collision leaked through. Checking the count (not just
       specific guessed-wrong names) is what actually catches an off-by-one
       in the slice offset, whatever spurious string it happens to produce. */
    CHECK(count == 1, "exactly one branch (feature/x) should be listed, got %zu", count);
    CHECK(names_contains(names, count, "feature/x"),
         "a legitimately nested branch name (refs/heads/feature/x) must still be listed");
    CHECK(!names_contains(names, count, "X/somebranch") && !names_contains(names, count, "somebranch") &&
         !names_contains(names, count, "/somebranch"),
         "a packed line under refs/headsX/ (prefix collision, not a real subtree) must not be listed "
         "as a branch");

    free_names(names, count);
    free(git_dir);
}

/* Same prefix-collision hazard, but for sg_ref_delete_under: deleting
   "somebranch" under prefix "refs/heads/" must not touch a packed line
   filed under the colliding "refs/headsX/" namespace. */
static void test_delete_does_not_touch_prefix_collision(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char collision_id[SG_SHA1_RAW_LEN];
    unsigned char real_id[SG_SHA1_RAW_LEN];
    char collision_line[256];
    char real_line[256];
    const char *lines[2];
    unsigned char out[SG_SHA1_RAW_LEN];

    fill_id(collision_id, 0x33);
    fill_id(real_id, 0x44);

    /* "somebranch" under the colliding "refs/headsX/" namespace ... */
    hex_line(collision_line, sizeof(collision_line), collision_id, "refs/headsX/somebranch");
    /* ... and a REAL "refs/heads/somebranch", the one actually targeted. */
    hex_line(real_line, sizeof(real_line), real_id, "refs/heads/somebranch");
    lines[0] = collision_line;
    lines[1] = real_line;
    write_packed_refs(git_dir, lines, 2);

    CHECK(sg_ref_delete_branch(git_dir, "somebranch") == 0, "delete of the real branch should succeed");

    /* The colliding refs/headsX/somebranch line must survive: read it back
       directly through the generic ref-path reader. */
    CHECK(sg_ref_read_path(git_dir, "refs/headsX/somebranch", out) == 0,
         "the prefix-colliding packed line must not have been removed");
    CHECK(memcmp(out, collision_id, SG_SHA1_RAW_LEN) == 0,
         "the surviving colliding line must still point at its original id");

    /* And the real branch really is gone from both stores. */
    CHECK(sg_ref_branch_exists(git_dir, "somebranch") == 0,
         "the real refs/heads/somebranch should no longer exist after delete");

    free(git_dir);
}

static int file_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

/* Deleting a branch must also unlink its own reflog file (logs/<ref_path>),
   matching real git's `git branch -D` (measured against 2.55.0). Builds the
   log via a real sg_ref_update(..., "test msg") call first -- a hand-forged
   file would only prove the unlink path works, not that it targets the
   SAME path sg_reflog_append actually writes to. */
static void test_delete_branch_removes_reflog_file(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id[SG_SHA1_RAW_LEN];
    char log_path[4096];

    fill_id(id, 0x55);
    CHECK(sg_ref_update(git_dir, "refs/heads/tobedeleted", id, "test msg") == 0,
         "sg_ref_update with a reflog message should succeed");

    snprintf(log_path, sizeof(log_path), "%s/logs/refs/heads/tobedeleted", git_dir);
    CHECK(file_exists(log_path), "precondition: the branch's reflog file must exist before deletion");

    CHECK(sg_ref_delete_branch(git_dir, "tobedeleted") == 0, "branch deletion should succeed");
    CHECK(!file_exists(log_path), "the branch's reflog file must be gone after sg_ref_delete_branch");
    CHECK(sg_ref_branch_exists(git_dir, "tobedeleted") == 0, "the branch itself must be gone too");

    free(git_dir);
}

/* sg_ref_delete_under also serves tag deletion (cmd_tag.c's `sg tag -d`),
   and a tag never gets a reflog at all (ref_path_reflog_allowed excludes
   refs/tags/...). Deleting a tag whose logs/refs/tags/<name> file was NEVER
   created must still return 0 -- unlinking a path that was never there is
   not a failure. This is the one most likely to be silently "always green":
   a mutant that turns the ENOENT-tolerant unlink into an unconditional
   failure would only be caught by asserting the return value here, not by
   any check on file state (there is no file to check). */
static void test_delete_under_tag_missing_reflog_still_succeeds(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id[SG_SHA1_RAW_LEN];
    char log_path[4096];

    fill_id(id, 0x66);
    /* sg_ref_write_path -> sg_ref_update(..., NULL): no reflog message, so
       (unlike test_delete_branch_removes_reflog_file above) no log file is
       ever written for this tag -- exactly the state a real `sg tag` leaves
       behind. */
    CHECK(sg_ref_write_path(git_dir, "refs/tags/sometag", id) == 0, "writing the tag ref should succeed");

    snprintf(log_path, sizeof(log_path), "%s/logs/refs/tags/sometag", git_dir);
    CHECK(!file_exists(log_path), "precondition: a tag must never have a reflog file to begin with");

    CHECK(sg_ref_delete_under(git_dir, "refs/tags/", "sometag") == 0,
         "deleting a tag whose reflog file never existed must still return 0, not -1");

    free(git_dir);
}

/* Same "missing log file is not an error" guarantee, but for a branch whose
   reflog was never written in the first place (sg_ref_update_branch with no
   message, i.e. sg_branch's actual code path) -- a second, narrower proof
   that ENOENT tolerance isn't accidentally scoped to only the tag case. */
static void test_delete_branch_missing_reflog_still_succeeds(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id[SG_SHA1_RAW_LEN];
    char log_path[4096];

    fill_id(id, 0x77);
    CHECK(sg_ref_update_branch(git_dir, "nolog", id) == 0, "creating the branch should succeed");

    snprintf(log_path, sizeof(log_path), "%s/logs/refs/heads/nolog", git_dir);
    CHECK(!file_exists(log_path), "precondition: this branch's reflog file must not exist");

    CHECK(sg_ref_delete_branch(git_dir, "nolog") == 0,
         "deleting a branch whose reflog file never existed must still return 0");

    free(git_dir);
}

/* sg_ref_set_symref writes "ref: <target>" and, when given a message, one
   reflog line -- reusing sg_ref_set_head's shape. The three properties worth
   pinning are the file's exact bytes, that old_id/new_id come from the two
   refs rather than from thin air, and that the reflog namespace policy still
   applies (Phase 17): a message for a ref outside the four logged namespaces
   is refused OUTRIGHT, writing nothing at all -- not "written without a
   log". */
static void test_set_symref_writes_ref_and_log(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id[SG_SHA1_RAW_LEN];
    char path[4096];
    char buf[256];
    sg_reflog log;
    FILE *f;
    size_t n;

    fill_id(id, 0x42);
    CHECK(sg_ref_write_path(git_dir, "refs/remotes/origin/master", id) == 0,
         "failed to write the target branch");

    CHECK(sg_ref_set_symref(git_dir, "refs/remotes/origin/HEAD", "refs/remotes/origin/master",
                           "clone: from http://example/repo.git") == 0,
         "sg_ref_set_symref failed");

    snprintf(path, sizeof(path), "%s/refs/remotes/origin/HEAD", git_dir);
    f = fopen(path, "rb");
    CHECK(f != NULL, "refs/remotes/origin/HEAD was not created");
    if (f != NULL) {
        n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
        CHECK(strcmp(buf, "ref: refs/remotes/origin/master\n") == 0,
             "expected a symref line, got \"%s\"", buf);
    }

    if (sg_reflog_read(git_dir, "refs/remotes/origin/HEAD", &log) != 0) {
        CHECK(0, "no reflog was written for refs/remotes/origin/HEAD");
    } else {
        CHECK(log.count == 1, "expected exactly 1 reflog line, got %zu", log.count);
        if (log.count == 1) {
            unsigned char zero[SG_SHA1_RAW_LEN];

            memset(zero, 0, SG_SHA1_RAW_LEN);
            /* old_id: this ref did not exist, so all-zeros. new_id: the
               TARGET's current tip, not zeros -- a symref's log records where
               it now points, and reading it off the target is the only way to
               get it. */
            CHECK(memcmp(log.entries[0].old_id, zero, SG_SHA1_RAW_LEN) == 0,
                 "old_id should be all-zeros for a ref that did not exist");
            CHECK(memcmp(log.entries[0].new_id, id, SG_SHA1_RAW_LEN) == 0,
                 "new_id should be the target ref's tip");
            CHECK(strcmp(log.entries[0].message, "clone: from http://example/repo.git") == 0,
                 "wrong reflog message: \"%s\"", log.entries[0].message);
        }
        sg_reflog_free(&log);
    }

    free(git_dir);
}

static void test_set_symref_refuses_a_message_outside_logged_namespaces(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id[SG_SHA1_RAW_LEN];
    char path[4096];
    struct stat st;

    fill_id(id, 0x11);
    CHECK(sg_ref_write_path(git_dir, "refs/heads/master", id) == 0, "failed to write the target");

    /* refs/tags/ is not a logged namespace. The refusal must be total: no
       ref file either, so a caller cannot end up with a ref whose history
       silently does not exist. */
    CHECK(sg_ref_set_symref(git_dir, "refs/tags/sym", "refs/heads/master", "some message") != 0,
         "a message for refs/tags/ must be refused");
    snprintf(path, sizeof(path), "%s/refs/tags/sym", git_dir);
    CHECK(lstat(path, &st) != 0, "the ref file must not exist after a refused write");

    /* With no message the same write is fine -- the policy is about logging,
       not about which refs may be symbolic. */
    CHECK(sg_ref_set_symref(git_dir, "refs/tags/sym", "refs/heads/master", NULL) == 0,
         "an unlogged symref outside the logged namespaces must still be allowed");
    CHECK(lstat(path, &st) == 0, "the ref file should exist now");
    snprintf(path, sizeof(path), "%s/logs/refs/tags/sym", git_dir);
    CHECK(lstat(path, &st) != 0, "no log should have been written for it");

    free(git_dir);
}

static void test_set_symref_rejects_path_traversal(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id[SG_SHA1_RAW_LEN];

    fill_id(id, 0x33);
    CHECK(sg_ref_write_path(git_dir, "refs/heads/master", id) == 0, "failed to write the target");

    CHECK(sg_ref_set_symref(git_dir, "refs/../../evil", "refs/heads/master", NULL) != 0,
         "a traversing ref_path must be rejected");
    CHECK(sg_ref_set_symref(git_dir, "refs/remotes/origin/HEAD", "refs/../../evil", NULL) != 0,
         "a traversing target must be rejected");

    free(git_dir);
}

int main(void)
{
    test_list_excludes_prefix_collision();
    test_delete_does_not_touch_prefix_collision();
    test_delete_branch_removes_reflog_file();
    test_delete_under_tag_missing_reflog_still_succeeds();
    test_delete_branch_missing_reflog_still_succeeds();
    test_set_symref_writes_ref_and_log();
    test_set_symref_refuses_a_message_outside_logged_namespaces();
    test_set_symref_rejects_path_traversal();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all refs tests passed\n");
    return 0;
}
