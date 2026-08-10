#include "sg/stash.h"

#include "sg/hash.h"
#include "sg/index.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/reflog.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

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
    static char template[] = "/tmp/sg_stash_test_XXXXXX";
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

static char *read_workdir_file(const char *repo_root, const char *rel)
{
    char abspath[4096];
    unsigned char *buf;
    size_t len;
    char *s;

    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, rel);
    if (sg_read_file(abspath, &buf, &len) != 0)
        return NULL;
    s = malloc(len + 1);
    memcpy(s, buf, len);
    s[len] = '\0';
    free(buf);
    return s;
}

/* Builds a single-file initial commit (a.txt = "hello\n") on the "master"
   branch, staged and committed, so push tests have a real HEAD to diff
   against. */
static void commit_initial(const char *git_dir, const char *repo_root)
{
    unsigned char blob[SG_SHA1_RAW_LEN];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    sg_flat_entry entries[1];
    sg_index idx;
    sg_index_entry e;
    sg_commit commit;
    unsigned char *serialized;
    size_t serialized_len;
    unsigned char commit_id[SG_SHA1_RAW_LEN];

    write_workdir_file(repo_root, "a.txt", "hello\n");
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "hello\n", 6, blob) == 0, "write blob");

    memset(&idx, 0, sizeof(idx));
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, blob, SG_SHA1_RAW_LEN);
    e.path = (char *)"a.txt";
    CHECK(sg_index_upsert(&idx, &e) == 0, "upsert a.txt");
    CHECK(sg_index_write(git_dir, &idx) == 0, "write index");
    sg_index_free(&idx);

    entries[0].path = (char *)"a.txt";
    entries[0].mode = 0100644;
    memcpy(entries[0].sha1, blob, SG_SHA1_RAW_LEN);
    CHECK(sg_tree_build(git_dir, entries, 1, tree_id) == 0, "build tree");

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

/* Asserts refs/stash equals the last reflog line's new_id, or that both are
   absent -- the one hard invariant sg_stash_push/drop/clear must never
   break (see stash-spec.md sec 1.1). */
static void assert_tip_invariant(const char *git_dir)
{
    unsigned char ref_id[SG_SHA1_RAW_LEN];
    sg_reflog log;
    int ref_exists = (sg_ref_read_path(git_dir, "refs/stash", ref_id) == 0);

    CHECK(sg_reflog_read(git_dir, "refs/stash", &log) == 0, "reflog read failed");

    if (!ref_exists) {
        CHECK(log.count == 0, "refs/stash absent but reflog has %zu entries", log.count);
    } else {
        CHECK(log.count > 0, "refs/stash exists but reflog is empty");
        if (log.count > 0)
            CHECK(memcmp(ref_id, log.entries[log.count - 1].new_id, SG_SHA1_RAW_LEN) == 0,
                 "tip invariant violated: refs/stash != last reflog line's new_id");
    }
    sg_reflog_free(&log);
}

/* ---- sg_stash_parse_spec accept/reject table -------------------------- */

static void test_parse_spec_table(void)
{
    size_t idx;

    CHECK(sg_stash_parse_spec(NULL, &idx) == 0 && idx == 0, "NULL should parse as 0");
    CHECK(sg_stash_parse_spec("", &idx) == 0 && idx == 0, "\"\" should parse as 0");
    CHECK(sg_stash_parse_spec("stash@{0}", &idx) == 0 && idx == 0, "stash@{0}");
    CHECK(sg_stash_parse_spec("stash@{12}", &idx) == 0 && idx == 12, "stash@{12}");
    CHECK(sg_stash_parse_spec("7", &idx) == 0 && idx == 7, "bare 7");

    CHECK(sg_stash_parse_spec("stash@{}", &idx) == -1, "stash@{} should be rejected");
    CHECK(sg_stash_parse_spec("stash@{-1}", &idx) == -1, "stash@{-1} should be rejected");
    CHECK(sg_stash_parse_spec("stash@{1x}", &idx) == -1, "stash@{1x} should be rejected");
    CHECK(sg_stash_parse_spec("stash@{ 1 }", &idx) == -1, "stash@{ 1 } should be rejected");
    CHECK(sg_stash_parse_spec("refs/stash", &idx) == -1, "refs/stash should be rejected");
    CHECK(sg_stash_parse_spec("stash@{99999999999999999999}", &idx) == -1,
         "overflow-sized spec should be rejected");
}

/* ---- push / list / drop / clear, asserting the tip invariant after every
   mutation ----------------------------------------------------------------- */

static void test_push_list_drop_clear(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char commit1[SG_SHA1_RAW_LEN];
    unsigned char commit2[SG_SHA1_RAW_LEN];
    int rc;

    commit_initial(git_dir, repo_root);

    /* nothing to save yet: exit 1, disk untouched */
    rc = sg_stash_push(git_dir, repo_root, NULL, commit1);
    CHECK(rc == 1, "expected nothing-to-save (1), got %d", rc);
    assert_tip_invariant(git_dir);
    {
        unsigned char ref_id[SG_SHA1_RAW_LEN];

        CHECK(sg_ref_read_path(git_dir, "refs/stash", ref_id) != 0, "refs/stash should not exist yet");
    }

    /* dirty the workdir, then stash it */
    write_workdir_file(repo_root, "a.txt", "changed\n");

    rc = sg_stash_push(git_dir, repo_root, "first stash", commit1);
    CHECK(rc == 0, "expected a stash to be created, got %d", rc);
    assert_tip_invariant(git_dir);

    {
        char *content = read_workdir_file(repo_root, "a.txt");

        CHECK(content != NULL && strcmp(content, "hello\n") == 0,
             "workdir should be reset to HEAD after push, got %s", content != NULL ? content : "(null)");
        free(content);
    }

    /* second stash, default WIP message */
    write_workdir_file(repo_root, "a.txt", "changed again\n");
    rc = sg_stash_push(git_dir, repo_root, NULL, commit2);
    CHECK(rc == 0, "expected a second stash, got %d", rc);
    assert_tip_invariant(git_dir);

    {
        sg_stash_list list;

        CHECK(sg_stash_list_read(git_dir, &list) == 0, "list read failed");
        CHECK(list.count == 2, "expected 2 stashes, got %zu", list.count);
        if (list.count == 2) {
            CHECK(memcmp(list.entries[0].commit_id, commit2, SG_SHA1_RAW_LEN) == 0,
                 "stash@{0} should be the most recently pushed stash");
            CHECK(memcmp(list.entries[1].commit_id, commit1, SG_SHA1_RAW_LEN) == 0,
                 "stash@{1} should be the first pushed stash");
            CHECK(strstr(list.entries[1].message, "first stash") != NULL,
                 "stash@{1} message should contain the -m text: %s", list.entries[1].message);
            CHECK(strstr(list.entries[0].message, "WIP on") != NULL,
                 "stash@{0} message should be the WIP form: %s", list.entries[0].message);
        }
        sg_stash_list_free(&list);
    }

    /* drop the newest (stash@{0}) -- refs/stash must re-point at stash@{1} */
    CHECK(sg_stash_drop(git_dir, 0) == 0, "drop stash@{0} failed");
    assert_tip_invariant(git_dir);
    {
        sg_stash_list list;

        CHECK(sg_stash_list_read(git_dir, &list) == 0, "list read failed after drop");
        CHECK(list.count == 1, "expected 1 stash left, got %zu", list.count);
        if (list.count == 1)
            CHECK(memcmp(list.entries[0].commit_id, commit1, SG_SHA1_RAW_LEN) == 0,
                 "remaining stash should be the first pushed one");
        sg_stash_list_free(&list);
    }
    {
        unsigned char ref_id[SG_SHA1_RAW_LEN];

        CHECK(sg_ref_read_path(git_dir, "refs/stash", ref_id) == 0, "refs/stash should still exist");
        CHECK(memcmp(ref_id, commit1, SG_SHA1_RAW_LEN) == 0,
             "refs/stash should now equal the surviving stash's commit");
    }

    /* drop the last entry -> stack empties, refs/stash and the reflog vanish */
    CHECK(sg_stash_drop(git_dir, 0) == 0, "drop of the last entry failed");
    assert_tip_invariant(git_dir);
    {
        unsigned char ref_id[SG_SHA1_RAW_LEN];

        CHECK(sg_ref_read_path(git_dir, "refs/stash", ref_id) != 0,
             "refs/stash should be gone once the stack is empty");
    }

    /* push once more, then clear */
    write_workdir_file(repo_root, "a.txt", "one more change\n");
    rc = sg_stash_push(git_dir, repo_root, NULL, commit1);
    CHECK(rc == 0, "expected a stash before clear, got %d", rc);
    assert_tip_invariant(git_dir);

    CHECK(sg_stash_clear(git_dir) == 0, "clear failed");
    assert_tip_invariant(git_dir);
    {
        unsigned char ref_id[SG_SHA1_RAW_LEN];

        CHECK(sg_ref_read_path(git_dir, "refs/stash", ref_id) != 0, "refs/stash should be gone after clear");
    }

    /* clear on an already-empty stack is a no-op, not an error */
    CHECK(sg_stash_clear(git_dir) == 0, "clear on an empty stack should still succeed");

    free(repo_root);
    free(git_dir);
}

int main(void)
{
    test_parse_spec_table();
    test_push_list_drop_clear();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all stash tests passed\n");
    return 0;
}
