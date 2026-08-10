#include "sg/objstore.h"

#include "sg/hash.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/repo.h"

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
    static char template[] = "/tmp/sg_objstore_test_XXXXXX";
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

/* Writes an empty tree object and returns its id via tree_id_out. */
static void write_empty_tree(const char *git_dir, unsigned char tree_id_out[SG_SHA1_RAW_LEN])
{
    unsigned char *content = NULL;
    size_t content_len = 0;

    CHECK(sg_tree_serialize(NULL, 0, &content, &content_len) == 0, "sg_tree_serialize failed");
    CHECK(sg_loose_write(git_dir, SG_OBJ_TREE, content, content_len, tree_id_out) == 0,
         "sg_loose_write(tree) failed");
    free(content);
}

/* Writes a root (no-parent) commit whose tree is tree_id, returns its id via
   commit_id_out. */
static void write_commit(const char *git_dir, const unsigned char tree_id[SG_SHA1_RAW_LEN],
                         unsigned char commit_id_out[SG_SHA1_RAW_LEN])
{
    sg_commit commit;
    unsigned char *content = NULL;
    size_t content_len = 0;

    memset(&commit, 0, sizeof(commit));
    memcpy(commit.tree, tree_id, SG_SHA1_RAW_LEN);
    commit.author_name = (char *)"test";
    commit.author_email = (char *)"test@test";
    commit.author_time = 0;
    strcpy(commit.author_tz, "+0000");
    commit.committer_name = (char *)"test";
    commit.committer_email = (char *)"test@test";
    commit.committer_time = 0;
    strcpy(commit.committer_tz, "+0000");
    commit.message = (char *)"test commit\n";

    CHECK(sg_commit_serialize(&commit, &content, &content_len) == 0, "sg_commit_serialize failed");
    CHECK(sg_loose_write(git_dir, SG_OBJ_COMMIT, content, content_len, commit_id_out) == 0,
         "sg_loose_write(commit) failed");
    free(content);
}

static void test_normal_commit(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    unsigned char tree_id_out[SG_SHA1_RAW_LEN];
    int rc;

    write_empty_tree(git_dir, tree_id);
    write_commit(git_dir, tree_id, commit_id);

    rc = sg_commit_tree_of(git_dir, commit_id, tree_id_out);
    CHECK(rc == 0, "expected 0, got %d", rc);
    CHECK(memcmp(tree_id, tree_id_out, SG_SHA1_RAW_LEN) == 0,
         "returned tree id does not match the commit's tree");

    free(git_dir);
}

/* commit_id actually points at a tree, not a commit -- must be rejected.
   Under ASan/UBSan this is also the regression case for the leak this test
   was added to cover: sg_object_read succeeds (content is malloc'd) but the
   type check then fails, so the early-return path must free content before
   returning. */
static void test_wrong_type(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    unsigned char tree_id_out[SG_SHA1_RAW_LEN];
    int rc;

    write_empty_tree(git_dir, tree_id);

    rc = sg_commit_tree_of(git_dir, tree_id, tree_id_out);
    CHECK(rc == -1, "expected -1 for a non-commit object, got %d", rc);

    free(git_dir);
}

static void test_missing_object(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char bogus_id[SG_SHA1_RAW_LEN];
    unsigned char tree_id_out[SG_SHA1_RAW_LEN];
    int rc;

    memset(bogus_id, 0xAB, sizeof(bogus_id));

    rc = sg_commit_tree_of(git_dir, bogus_id, tree_id_out);
    CHECK(rc == -1, "expected -1 for a missing object, got %d", rc);

    free(git_dir);
}

static void test_malformed_commit(void)
{
    char *git_dir = make_tmp_repo();
    const char *garbage = "this is not a valid commit body\n";
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    unsigned char tree_id_out[SG_SHA1_RAW_LEN];
    int rc;

    CHECK(sg_loose_write(git_dir, SG_OBJ_COMMIT, garbage, strlen(garbage), commit_id) == 0,
         "sg_loose_write(garbage commit) failed");

    rc = sg_commit_tree_of(git_dir, commit_id, tree_id_out);
    CHECK(rc == -1, "expected -1 for a malformed commit, got %d", rc);

    free(git_dir);
}

int main(void)
{
    test_normal_commit();
    test_wrong_type();
    test_missing_object();
    test_malformed_commit();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all objstore tests passed\n");
    return 0;
}
