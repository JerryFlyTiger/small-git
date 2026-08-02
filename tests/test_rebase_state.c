#include "sg/rebase.h"

#include "sg/loose.h"
#include "sg/object.h"
#include "sg/repo.h"
#include "sg/tree_build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
static long long time_seq = 1000000;

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
    static char template[] = "/tmp/sg_rebase_state_test_XXXXXX";
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

/* Every commit gets its own single-blob tree (content = the commit's
   message) purely so distinct commits never accidentally share a tree id;
   the tree content itself is irrelevant to these tests, which only care
   about commit/parent links. */
static void make_commit(const char *git_dir, const char *message,
                        const unsigned char (*parents)[SG_SHA1_RAW_LEN], size_t parent_count,
                        unsigned char commit_id_out[SG_SHA1_RAW_LEN])
{
    unsigned char blob_id[SG_SHA1_RAW_LEN];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    sg_flat_entry entry;
    sg_commit commit;
    unsigned char *serialized;
    size_t serialized_len;

    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, message, strlen(message), blob_id) == 0,
         "blob write failed for '%s'", message);

    entry.path = (char *)"file.txt";
    entry.mode = 0100644;
    memcpy(entry.sha1, blob_id, SG_SHA1_RAW_LEN);
    CHECK(sg_tree_build(git_dir, &entry, 1, tree_id) == 0, "tree build failed for '%s'", message);

    memset(&commit, 0, sizeof(commit));
    memcpy(commit.tree, tree_id, SG_SHA1_RAW_LEN);
    if (parent_count > 0) {
        commit.parents = malloc(parent_count * sizeof(*commit.parents));
        CHECK(commit.parents != NULL, "oom");
        memcpy(commit.parents, parents, parent_count * SG_SHA1_RAW_LEN);
        commit.parent_count = parent_count;
    }
    commit.author_name = (char *)"tester";
    commit.author_email = (char *)"tester@example.com";
    commit.author_time = time_seq++;
    strcpy(commit.author_tz, "+0000");
    commit.committer_name = commit.author_name;
    commit.committer_email = commit.author_email;
    commit.committer_time = commit.author_time;
    strcpy(commit.committer_tz, "+0000");
    commit.message = (char *)message;

    CHECK(sg_commit_serialize(&commit, &serialized, &serialized_len) == 0,
         "serialize failed for '%s'", message);
    free(commit.parents);
    CHECK(sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len, commit_id_out) == 0,
         "commit write failed for '%s'", message);
    free(serialized);
}

static void write_raw_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");

    CHECK(f != NULL, "failed to open '%s'", path);
    if (f == NULL)
        return;
    fputs(content, f);
    fclose(f);
}

/* ==================== state read/write round-trip ==================== */

static void test_state_roundtrip(void)
{
    char *git_dir = make_tmp_repo();
    sg_rebase_state st, read_back;
    unsigned char onto[SG_SHA1_RAW_LEN], orig_head[SG_SHA1_RAW_LEN];
    unsigned char c1[SG_SHA1_RAW_LEN], c2[SG_SHA1_RAW_LEN], c3[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "onto", NULL, 0, onto);
    make_commit(git_dir, "orig-head", NULL, 0, orig_head);
    make_commit(git_dir, "c1", NULL, 0, c1);
    make_commit(git_dir, "c2", NULL, 0, c2);
    make_commit(git_dir, "c3", NULL, 0, c3);

    CHECK(sg_rebase_state_exists(git_dir) == 0, "no rebase state should exist yet");

    memset(&st, 0, sizeof(st));
    memcpy(st.onto, onto, SG_SHA1_RAW_LEN);
    memcpy(st.orig_head, orig_head, SG_SHA1_RAW_LEN);
    st.orig_branch = strdup("feature/my-branch");
    st.todo = malloc(3 * sizeof(*st.todo));
    memcpy(st.todo[0], c1, SG_SHA1_RAW_LEN);
    memcpy(st.todo[1], c2, SG_SHA1_RAW_LEN);
    memcpy(st.todo[2], c3, SG_SHA1_RAW_LEN);
    st.todo_count = 3;
    st.has_current = 0;

    CHECK(sg_rebase_state_write(git_dir, &st) == 0, "state write should succeed");
    CHECK(sg_rebase_state_exists(git_dir) == 1, "rebase state should now exist");

    memset(&read_back, 0, sizeof(read_back));
    CHECK(sg_rebase_state_read(git_dir, &read_back) == 0, "state read should succeed");
    CHECK(memcmp(read_back.onto, onto, SG_SHA1_RAW_LEN) == 0, "onto round-trips");
    CHECK(memcmp(read_back.orig_head, orig_head, SG_SHA1_RAW_LEN) == 0, "orig_head round-trips");
    CHECK(read_back.orig_branch != NULL && strcmp(read_back.orig_branch, "feature/my-branch") == 0,
         "orig_branch round-trips");
    CHECK(read_back.todo_count == 3, "todo_count round-trips (got %zu)", read_back.todo_count);
    CHECK(read_back.todo != NULL && memcmp(read_back.todo[0], c1, SG_SHA1_RAW_LEN) == 0,
         "todo[0] round-trips");
    CHECK(read_back.todo != NULL && memcmp(read_back.todo[1], c2, SG_SHA1_RAW_LEN) == 0,
         "todo[1] round-trips");
    CHECK(read_back.todo != NULL && memcmp(read_back.todo[2], c3, SG_SHA1_RAW_LEN) == 0,
         "todo[2] round-trips");
    CHECK(read_back.has_current == 0, "has_current should be 0 (no current file written)");
    sg_rebase_state_free(&read_back);

    /* Now record a conflict pause: current set, todo shrunk. */
    memcpy(st.current, c1, SG_SHA1_RAW_LEN);
    st.has_current = 1;
    memmove(st.todo, st.todo + 1, 2 * sizeof(*st.todo));
    st.todo_count = 2;
    CHECK(sg_rebase_state_write(git_dir, &st) == 0, "state re-write should succeed");

    memset(&read_back, 0, sizeof(read_back));
    CHECK(sg_rebase_state_read(git_dir, &read_back) == 0, "state read (with current) should succeed");
    CHECK(read_back.has_current == 1, "has_current should now be 1");
    CHECK(memcmp(read_back.current, c1, SG_SHA1_RAW_LEN) == 0, "current round-trips");
    CHECK(read_back.todo_count == 2, "todo_count should have shrunk to 2");
    sg_rebase_state_free(&read_back);

    /* has_current going back to 0 must remove the stale current file. */
    st.has_current = 0;
    CHECK(sg_rebase_state_write(git_dir, &st) == 0, "clearing current should succeed");
    memset(&read_back, 0, sizeof(read_back));
    CHECK(sg_rebase_state_read(git_dir, &read_back) == 0, "state read (current cleared) should succeed");
    CHECK(read_back.has_current == 0, "has_current should be back to 0");
    sg_rebase_state_free(&read_back);

    CHECK(sg_rebase_state_remove(git_dir) == 0, "state remove should succeed");
    CHECK(sg_rebase_state_exists(git_dir) == 0, "rebase state should be gone");
    CHECK(sg_rebase_state_read(git_dir, &read_back) == -1, "reading a removed state should fail");

    free(st.orig_branch);
    free(st.todo);
    free(git_dir);
}

/* ==================== corrupted state is rejected ==================== */

static void test_corrupt_onto_rejected(void)
{
    char *git_dir = make_tmp_repo();
    char path[4096];
    sg_rebase_state st;

    snprintf(path, sizeof(path), "%s/sg-rebase", git_dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/sg-rebase/onto", git_dir);
    write_raw_file(path, "not-valid-hex\n");
    snprintf(path, sizeof(path), "%s/sg-rebase/orig-head", git_dir);
    write_raw_file(path, "0000000000000000000000000000000000000000\n");
    snprintf(path, sizeof(path), "%s/sg-rebase/orig-branch", git_dir);
    write_raw_file(path, "master\n");
    snprintf(path, sizeof(path), "%s/sg-rebase/todo", git_dir);
    write_raw_file(path, "");

    CHECK(sg_rebase_state_exists(git_dir) == 1, "directory exists even though contents are bad");
    CHECK(sg_rebase_state_read(git_dir, &st) == -1, "malformed onto hex should be rejected");

    free(git_dir);
}

static void test_corrupt_todo_line_length_rejected(void)
{
    char *git_dir = make_tmp_repo();
    char path[4096];
    sg_rebase_state st;

    snprintf(path, sizeof(path), "%s/sg-rebase", git_dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/sg-rebase/onto", git_dir);
    write_raw_file(path, "0000000000000000000000000000000000000000\n");
    snprintf(path, sizeof(path), "%s/sg-rebase/orig-head", git_dir);
    write_raw_file(path, "0000000000000000000000000000000000000000\n");
    snprintf(path, sizeof(path), "%s/sg-rebase/orig-branch", git_dir);
    write_raw_file(path, "master\n");
    snprintf(path, sizeof(path), "%s/sg-rebase/todo", git_dir);
    /* one line too short */
    write_raw_file(path, "deadbeef\n");

    CHECK(sg_rebase_state_read(git_dir, &st) == -1, "todo line of the wrong length should be rejected");

    free(git_dir);
}

static void test_corrupt_orig_branch_dotdot_rejected(void)
{
    char *git_dir = make_tmp_repo();
    char path[4096];
    sg_rebase_state st;

    snprintf(path, sizeof(path), "%s/sg-rebase", git_dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/sg-rebase/onto", git_dir);
    write_raw_file(path, "0000000000000000000000000000000000000000\n");
    snprintf(path, sizeof(path), "%s/sg-rebase/orig-head", git_dir);
    write_raw_file(path, "0000000000000000000000000000000000000000\n");
    snprintf(path, sizeof(path), "%s/sg-rebase/orig-branch", git_dir);
    write_raw_file(path, "../../../tmp/evil\n");
    snprintf(path, sizeof(path), "%s/sg-rebase/todo", git_dir);
    write_raw_file(path, "");

    CHECK(sg_rebase_state_read(git_dir, &st) == -1,
         "orig-branch containing '..' should be rejected by the branch-name safety check");

    free(git_dir);
}

static void test_corrupt_current_malformed_rejected(void)
{
    char *git_dir = make_tmp_repo();
    char path[4096];
    sg_rebase_state st;

    snprintf(path, sizeof(path), "%s/sg-rebase", git_dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/sg-rebase/onto", git_dir);
    write_raw_file(path, "0000000000000000000000000000000000000000\n");
    snprintf(path, sizeof(path), "%s/sg-rebase/orig-head", git_dir);
    write_raw_file(path, "0000000000000000000000000000000000000000\n");
    snprintf(path, sizeof(path), "%s/sg-rebase/orig-branch", git_dir);
    write_raw_file(path, "master\n");
    snprintf(path, sizeof(path), "%s/sg-rebase/todo", git_dir);
    write_raw_file(path, "");
    snprintf(path, sizeof(path), "%s/sg-rebase/current", git_dir);
    write_raw_file(path, "not-hex-at-all\n");

    CHECK(sg_rebase_state_read(git_dir, &st) == -1,
         "a present-but-malformed 'current' file must be treated as corruption, not as 'no conflict'");

    free(git_dir);
}

/* ==================== commit-list computation ==================== */

/* X -> A -> B -> C (linear): rebasing onto X should replay A, B, C in that
   (oldest-to-newest) order. */
static void test_compute_todo_linear(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char x[SG_SHA1_RAW_LEN], a[SG_SHA1_RAW_LEN], b[SG_SHA1_RAW_LEN], c[SG_SHA1_RAW_LEN];
    unsigned char(*todo)[SG_SHA1_RAW_LEN] = NULL;
    size_t todo_count = 0;
    unsigned char merge_commit[SG_SHA1_RAW_LEN];
    int found_merge = 0;

    make_commit(git_dir, "x", NULL, 0, x);
    make_commit(git_dir, "a", (const unsigned char (*)[SG_SHA1_RAW_LEN])x, 1, a);
    make_commit(git_dir, "b", (const unsigned char (*)[SG_SHA1_RAW_LEN])a, 1, b);
    make_commit(git_dir, "c", (const unsigned char (*)[SG_SHA1_RAW_LEN])b, 1, c);

    CHECK(sg_rebase_compute_todo(git_dir, c, x, &todo, &todo_count, merge_commit, &found_merge) == 0,
         "compute_todo should succeed");
    CHECK(found_merge == 0, "no merge commit should be found");
    CHECK(todo_count == 3, "expected 3 commits to replay, got %zu", todo_count);
    if (todo_count == 3) {
        CHECK(memcmp(todo[0], a, SG_SHA1_RAW_LEN) == 0, "todo[0] should be a (oldest)");
        CHECK(memcmp(todo[1], b, SG_SHA1_RAW_LEN) == 0, "todo[1] should be b");
        CHECK(memcmp(todo[2], c, SG_SHA1_RAW_LEN) == 0, "todo[2] should be c (newest)");
    }

    free(todo);
    free(git_dir);
}

/* base == head: nothing to replay, empty list. */
static void test_compute_todo_empty_when_base_is_head(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char x[SG_SHA1_RAW_LEN];
    unsigned char(*todo)[SG_SHA1_RAW_LEN] = NULL;
    size_t todo_count = 0;
    unsigned char merge_commit[SG_SHA1_RAW_LEN];
    int found_merge = 0;

    make_commit(git_dir, "x", NULL, 0, x);

    CHECK(sg_rebase_compute_todo(git_dir, x, x, &todo, &todo_count, merge_commit, &found_merge) == 0,
         "compute_todo should succeed when base==head");
    CHECK(found_merge == 0, "no merge commit found");
    CHECK(todo_count == 0, "todo should be empty when base==head");

    free(todo);
    free(git_dir);
}

/* X -> A -> M(A, S) -> C, where M is a merge commit (second parent S is some
   side branch): rebasing C onto X must report the merge commit rather than
   silently flattening history. */
static void test_compute_todo_rejects_merge_commit(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char x[SG_SHA1_RAW_LEN], a[SG_SHA1_RAW_LEN], s[SG_SHA1_RAW_LEN];
    unsigned char m[SG_SHA1_RAW_LEN], c[SG_SHA1_RAW_LEN];
    unsigned char m_parents[2][SG_SHA1_RAW_LEN];
    unsigned char(*todo)[SG_SHA1_RAW_LEN] = NULL;
    size_t todo_count = 0;
    unsigned char merge_commit[SG_SHA1_RAW_LEN];
    int found_merge = 0;

    make_commit(git_dir, "x", NULL, 0, x);
    make_commit(git_dir, "a", (const unsigned char (*)[SG_SHA1_RAW_LEN])x, 1, a);
    make_commit(git_dir, "s", (const unsigned char (*)[SG_SHA1_RAW_LEN])x, 1, s);
    memcpy(m_parents[0], a, SG_SHA1_RAW_LEN);
    memcpy(m_parents[1], s, SG_SHA1_RAW_LEN);
    make_commit(git_dir, "m", m_parents, 2, m);
    make_commit(git_dir, "c", (const unsigned char (*)[SG_SHA1_RAW_LEN])m, 1, c);

    CHECK(sg_rebase_compute_todo(git_dir, c, x, &todo, &todo_count, merge_commit, &found_merge) == 0,
         "compute_todo should not itself error out on a merge commit");
    CHECK(found_merge == 1, "a merge commit in the range should be detected");
    CHECK(memcmp(merge_commit, m, SG_SHA1_RAW_LEN) == 0, "the reported commit should be m");
    CHECK(todo == NULL && todo_count == 0, "no todo list should be returned when a merge is found");

    free(todo);
    free(git_dir);
}

int main(void)
{
    test_state_roundtrip();
    test_corrupt_onto_rejected();
    test_corrupt_todo_line_length_rejected();
    test_corrupt_orig_branch_dotdot_rejected();
    test_corrupt_current_malformed_rejected();
    test_compute_todo_linear();
    test_compute_todo_empty_when_base_is_head();
    test_compute_todo_rejects_merge_commit();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all rebase_state tests passed\n");
    return 0;
}
