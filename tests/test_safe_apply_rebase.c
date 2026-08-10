#include "sg/apply.h"

#include "sg/index.h"
#include "sg/loose.h"
#include "sg/merge.h"
#include "sg/object.h"
#include "sg/rebase.h"
#include "sg/repo.h"
#include "sg/tree_build.h"
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
    static char template[] = "/tmp/sg_safe_apply_rebase_test_XXXXXX";
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

static int file_exists(const char *repo_root, const char *rel)
{
    char abspath[4096];
    struct stat st;

    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, rel);
    return stat(abspath, &st) == 0;
}

static char *read_workdir_file(const char *repo_root, const char *rel)
{
    char abspath[4096];
    unsigned char *buf;
    size_t len;

    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, rel);
    if (sg_read_file(abspath, &buf, &len) != 0)
        return NULL;
    {
        char *s = malloc(len + 1);

        memcpy(s, buf, len);
        s[len] = '\0';
        free(buf);
        return s;
    }
}

/* Builds a single-file target tree ("target.txt") and writes it via
   sg_repo_init's initial (empty) index, so sg_safe_apply_tree has something
   real to apply. Returns the tree id. */
static void build_target_tree(const char *git_dir, unsigned char tree_id_out[SG_SHA1_RAW_LEN])
{
    unsigned char blob_id[SG_SHA1_RAW_LEN];
    sg_flat_entry entry;

    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "target content\n", 15, blob_id) == 0,
         "write target blob");
    entry.path = (char *)"target.txt";
    entry.mode = 0100644;
    memcpy(entry.sha1, blob_id, SG_SHA1_RAW_LEN);
    CHECK(sg_tree_build(git_dir, &entry, 1, tree_id_out) == 0, "tree build failed");
}

/* An in-progress rebase must survive sg_safe_apply_tree: not just the
   directory's existence, every field read back afterward must equal what was
   written -- and the working tree must actually have been overwritten (not
   "preserved" only because apply never ran). */
static void test_rebase_state_survives_safe_apply(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    sg_rebase_state st, read_back;
    unsigned char onto[SG_SHA1_RAW_LEN], orig_head[SG_SHA1_RAW_LEN];
    unsigned char c1[SG_SHA1_RAW_LEN], c2[SG_SHA1_RAW_LEN];
    int apply_rc;
    char *content;

    build_target_tree(git_dir, tree_id);

    memset(&st, 0, sizeof(st));
    /* Arbitrary but distinct 40-hex-valid ids: reuse hashes we already
       computed above so we don't need extra loose objects for this. */
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "onto\n", 5, onto) == 0, "write onto blob");
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "orig-head\n", 10, orig_head) == 0,
         "write orig_head blob");
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "c1\n", 3, c1) == 0, "write c1 blob");
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "c2\n", 3, c2) == 0, "write c2 blob");

    memcpy(st.onto, onto, SG_SHA1_RAW_LEN);
    memcpy(st.orig_head, orig_head, SG_SHA1_RAW_LEN);
    st.orig_branch = strdup("feature/topic");
    st.todo = malloc(1 * sizeof(*st.todo));
    memcpy(st.todo[0], c2, SG_SHA1_RAW_LEN);
    st.todo_count = 1;
    memcpy(st.current, c1, SG_SHA1_RAW_LEN);
    st.has_current = 1;

    CHECK(sg_rebase_state_write(git_dir, &st) == 0, "rebase state write should succeed");
    CHECK(sg_rebase_state_exists(git_dir) == 1, "rebase state should exist before apply");

    apply_rc = sg_safe_apply_tree(git_dir, repo_root, tree_id, "test apply", 1 /* force */);
    CHECK(apply_rc == 0, "sg_safe_apply_tree should succeed, got %d", apply_rc);

    /* The working tree really was overwritten -- otherwise "state survived"
       would be true for the trivial (and wrong) reason that apply never
       ran. */
    CHECK(file_exists(repo_root, "target.txt"), "target.txt should have been written by apply");
    content = read_workdir_file(repo_root, "target.txt");
    CHECK(content != NULL && strcmp(content, "target content\n") == 0,
         "target.txt should have the target tree's content, got %s",
         content != NULL ? content : "(null)");
    free(content);

    CHECK(sg_rebase_state_exists(git_dir) == 1, "rebase state should still exist after apply");
    memset(&read_back, 0, sizeof(read_back));
    CHECK(sg_rebase_state_read(git_dir, &read_back) == 0, "rebase state read should succeed");
    CHECK(memcmp(read_back.onto, onto, SG_SHA1_RAW_LEN) == 0, "onto should be unchanged");
    CHECK(memcmp(read_back.orig_head, orig_head, SG_SHA1_RAW_LEN) == 0,
         "orig_head should be unchanged");
    CHECK(read_back.orig_branch != NULL && strcmp(read_back.orig_branch, "feature/topic") == 0,
         "orig_branch should be unchanged, got %s",
         read_back.orig_branch != NULL ? read_back.orig_branch : "(null)");
    CHECK(read_back.todo_count == 1, "todo_count should be unchanged, got %zu",
         read_back.todo_count);
    CHECK(read_back.todo != NULL && memcmp(read_back.todo[0], c2, SG_SHA1_RAW_LEN) == 0,
         "todo[0] should be unchanged");
    CHECK(read_back.has_current == 1, "has_current should be unchanged");
    CHECK(memcmp(read_back.current, c1, SG_SHA1_RAW_LEN) == 0, "current should be unchanged");

    sg_rebase_state_free(&read_back);
    free(st.orig_branch);
    free(st.todo);
    free(repo_root);
    free(git_dir);
}

/* Control: MERGE_HEAD must still be cleared by sg_safe_apply_tree. This
   guards against a fix that accidentally removes the MERGE_HEAD cleanup
   along with the rebase-state cleanup it's replacing. */
static void test_merge_head_still_cleared_by_safe_apply(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    unsigned char merge_commit_id[SG_SHA1_RAW_LEN];
    unsigned char merge_head_out[SG_SHA1_RAW_LEN];
    int apply_rc;

    build_target_tree(git_dir, tree_id);

    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "merge commit stand-in\n", 22, merge_commit_id) == 0,
         "write stand-in blob for MERGE_HEAD");
    CHECK(sg_merge_head_write(git_dir, merge_commit_id) == 0, "MERGE_HEAD write should succeed");
    CHECK(sg_merge_head_read(git_dir, merge_head_out) == 0, "MERGE_HEAD should exist before apply");

    apply_rc = sg_safe_apply_tree(git_dir, repo_root, tree_id, "test apply", 1 /* force */);
    CHECK(apply_rc == 0, "sg_safe_apply_tree should succeed, got %d", apply_rc);

    CHECK(sg_merge_head_read(git_dir, merge_head_out) != 0,
         "MERGE_HEAD should have been cleared by sg_safe_apply_tree");

    free(repo_root);
    free(git_dir);
}

int main(void)
{
    test_rebase_state_survives_safe_apply();
    test_merge_head_still_cleared_by_safe_apply();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all safe_apply_rebase tests passed\n");
    return 0;
}
