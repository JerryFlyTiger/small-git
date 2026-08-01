#include "sg/merge.h"

#include "sg/loose.h"
#include "sg/object.h"
#include "sg/repo.h"
#include "sg/tree_build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    static char template[] = "/tmp/sg_merge_base_test_XXXXXX";
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
   the tree content itself is irrelevant to merge-base, which only walks
   commit/parent links. */
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

/* P1 -> P2 -> P3 (a straight line): merge-base of the two ends is the
   earlier commit. */
static void test_linear_history(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char p1[SG_SHA1_RAW_LEN], p2[SG_SHA1_RAW_LEN], p3[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "p1", NULL, 0, p1);
    make_commit(git_dir, "p2", (const unsigned char (*)[SG_SHA1_RAW_LEN])p1, 1, p2);
    make_commit(git_dir, "p3", (const unsigned char (*)[SG_SHA1_RAW_LEN])p2, 1, p3);

    CHECK(sg_merge_base(git_dir, p1, p3, out) == 0, "merge_base should succeed on linear history");
    CHECK(memcmp(out, p1, SG_SHA1_RAW_LEN) == 0, "merge_base(p1,p3) should be p1");

    free(git_dir);
}

/* X is the common root, A and B a simple two-way split off of it: the
   merge-base of the two branch tips is X. */
static void test_simple_divergence(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char x[SG_SHA1_RAW_LEN], a[SG_SHA1_RAW_LEN], b[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "x", NULL, 0, x);
    make_commit(git_dir, "a", (const unsigned char (*)[SG_SHA1_RAW_LEN])x, 1, a);
    make_commit(git_dir, "b", (const unsigned char (*)[SG_SHA1_RAW_LEN])x, 1, b);

    CHECK(sg_merge_base(git_dir, a, b, out) == 0, "merge_base should succeed on a simple fork");
    CHECK(memcmp(out, x, SG_SHA1_RAW_LEN) == 0, "merge_base(a,b) should be x");

    free(git_dir);
}

/* If one commit is already an ancestor of the other, merge-base is that
   ancestor itself. */
static void test_already_ancestor(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char x[SG_SHA1_RAW_LEN], a[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "x", NULL, 0, x);
    make_commit(git_dir, "a", (const unsigned char (*)[SG_SHA1_RAW_LEN])x, 1, a);

    CHECK(sg_merge_base(git_dir, x, a, out) == 0, "merge_base should succeed");
    CHECK(memcmp(out, x, SG_SHA1_RAW_LEN) == 0, "merge_base(x,a) should be x itself");
    CHECK(sg_merge_base(git_dir, a, x, out) == 0, "merge_base should succeed (args swapped)");
    CHECK(memcmp(out, x, SG_SHA1_RAW_LEN) == 0, "merge_base(a,x) should still be x");

    free(git_dir);
}

/* Criss-cross history: X forks into A and B; M1 merges A and B; M2
   independently also merges A and B (as a different commit). A and B are
   then both common ancestors of M1 and M2, and neither dominates the
   other -- merge_base must report -2 rather than guessing one. */
static void test_criss_cross(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char x[SG_SHA1_RAW_LEN], a[SG_SHA1_RAW_LEN], b[SG_SHA1_RAW_LEN];
    unsigned char m1[SG_SHA1_RAW_LEN], m2[SG_SHA1_RAW_LEN];
    unsigned char ab_parents[2][SG_SHA1_RAW_LEN];
    unsigned char ba_parents[2][SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "x", NULL, 0, x);
    make_commit(git_dir, "a", (const unsigned char (*)[SG_SHA1_RAW_LEN])x, 1, a);
    make_commit(git_dir, "b", (const unsigned char (*)[SG_SHA1_RAW_LEN])x, 1, b);

    memcpy(ab_parents[0], a, SG_SHA1_RAW_LEN);
    memcpy(ab_parents[1], b, SG_SHA1_RAW_LEN);
    make_commit(git_dir, "m1", ab_parents, 2, m1);

    memcpy(ba_parents[0], b, SG_SHA1_RAW_LEN);
    memcpy(ba_parents[1], a, SG_SHA1_RAW_LEN);
    make_commit(git_dir, "m2", ba_parents, 2, m2);

    CHECK(sg_merge_base(git_dir, m1, m2, out) == -2,
         "criss-cross history should report -2 (multiple independent merge bases)");

    free(git_dir);
}

/* Two commits with no shared history at all: merge_base reports -1. */
static void test_unrelated_histories(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char a[SG_SHA1_RAW_LEN], b[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "unrelated-a", NULL, 0, a);
    make_commit(git_dir, "unrelated-b", NULL, 0, b);

    CHECK(sg_merge_base(git_dir, a, b, out) == -1,
         "commits with no shared history should report -1");

    free(git_dir);
}

int main(void)
{
    test_linear_history();
    test_simple_divergence();
    test_already_ancestor();
    test_criss_cross();
    test_unrelated_histories();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all merge_base tests passed\n");
    return 0;
}
