/* sg_tree_build_from_workdir's two missing-file policies, and the empty-parent
   pruning that only became reachable once RECORD_DELETION could produce a tree
   that actually deletes something. */

#include "sg/tree_build.h"

#include "sg/hash.h"
#include "sg/index.h"
#include "sg/loose.h"
#include "sg/object.h"
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

/* git's empty tree, the object RECORD_DELETION must produce when every path
   the index covers has been deleted from the working tree. */
#define EMPTY_TREE_HEX "4b825dc642cb6eb9a060e54bf8d69288fbee4904"

static char *make_tmp_repo(void)
{
    static char template[] = "/tmp/sg_tree_build_workdir_test_XXXXXX";
    char *path = strdup(template);
    char git_dir[SG_PATH_MAX];

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
    char abspath[SG_PATH_MAX];

    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, rel);
    if (sg_write_file_mkdirs(abspath, (const unsigned char *)content, strlen(content), 0644) != 0) {
        fprintf(stderr, "write_workdir_file failed for %s\n", rel);
        exit(1);
    }
}

/* Stages rel at the blob for `staged`, without writing anything to the
   working tree: callers decide separately what (if anything) lives on disk. */
static void stage_entry(const char *git_dir, sg_index *idx, const char *rel, const char *staged,
                        unsigned char blob_out[SG_SHA1_RAW_LEN])
{
    sg_index_entry e;

    if (sg_loose_write(git_dir, SG_OBJ_BLOB, staged, strlen(staged), blob_out) != 0) {
        fprintf(stderr, "sg_loose_write failed for %s\n", rel);
        exit(1);
    }
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, blob_out, SG_SHA1_RAW_LEN);
    e.path = (char *)rel;
    if (sg_index_upsert(idx, &e) != 0) {
        fprintf(stderr, "sg_index_upsert failed for %s\n", rel);
        exit(1);
    }
}

static int flat_find(const sg_flat_list *list, const char *path)
{
    size_t i;

    for (i = 0; i < list->count; i++) {
        if (strcmp(list->entries[i].path, path) == 0)
            return (int)i;
    }
    return -1;
}

static int dir_exists(const char *repo_root, const char *rel)
{
    char abspath[SG_PATH_MAX];
    struct stat st;

    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, rel);
    return stat(abspath, &st) == 0 && S_ISDIR(st.st_mode);
}

/* The two policies must disagree about exactly one thing -- the deleted path --
   and agree about everything else, so both are built from ONE fixture here.
   Asserting them side by side is what makes a regression that collapses the
   two policies back into one behaviour visible: a test that only ever built
   one of them would stay green if the enum stopped being consulted. */
static void test_policies_differ_only_on_the_missing_path(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    unsigned char kept_blob[SG_SHA1_RAW_LEN], gone_blob[SG_SHA1_RAW_LEN];
    unsigned char expected_kept[SG_SHA1_RAW_LEN];
    unsigned char keep_tree[SG_SHA1_RAW_LEN], del_tree[SG_SHA1_RAW_LEN];
    sg_flat_list keep_flat, del_flat;
    int pos;

    memset(&idx, 0, sizeof(idx));

    /* kept.txt is staged with one content and has DIFFERENT content on disk:
       both policies must hash what the working tree holds now. */
    stage_entry(git_dir, &idx, "kept.txt", "staged kept\n", kept_blob);
    write_workdir_file(repo_root, "kept.txt", "workdir kept\n");
    sg_object_hash(SG_OBJ_BLOB, "workdir kept\n", strlen("workdir kept\n"), expected_kept);

    /* gone.txt is staged but deliberately never written to disk. */
    stage_entry(git_dir, &idx, "gone.txt", "staged gone\n", gone_blob);

    CHECK(sg_tree_build_from_workdir(git_dir, repo_root, &idx, SG_WORKDIR_MISSING_KEEP_INDEX_BLOB,
                                     keep_tree) == 0,
         "KEEP_INDEX_BLOB build failed");
    CHECK(sg_tree_build_from_workdir(git_dir, repo_root, &idx, SG_WORKDIR_MISSING_RECORD_DELETION,
                                     del_tree) == 0,
         "RECORD_DELETION build failed");

    CHECK(memcmp(keep_tree, del_tree, SG_SHA1_RAW_LEN) != 0,
         "the two policies produced the SAME tree -- the missing path was treated identically");

    CHECK(sg_tree_flatten(git_dir, keep_tree, &keep_flat) == 0, "flatten KEEP_INDEX_BLOB tree");
    CHECK(keep_flat.count == 2, "KEEP_INDEX_BLOB should cover both paths, got %zu",
          keep_flat.count);
    pos = flat_find(&keep_flat, "gone.txt");
    CHECK(pos >= 0, "KEEP_INDEX_BLOB dropped gone.txt");
    if (pos >= 0)
        CHECK(memcmp(keep_flat.entries[pos].sha1, gone_blob, SG_SHA1_RAW_LEN) == 0,
             "KEEP_INDEX_BLOB must record the INDEX blob for a file that is gone");
    pos = flat_find(&keep_flat, "kept.txt");
    CHECK(pos >= 0, "KEEP_INDEX_BLOB dropped kept.txt");
    if (pos >= 0)
        CHECK(memcmp(keep_flat.entries[pos].sha1, expected_kept, SG_SHA1_RAW_LEN) == 0,
             "kept.txt must hash the WORKING TREE content, not the staged blob");

    CHECK(sg_tree_flatten(git_dir, del_tree, &del_flat) == 0, "flatten RECORD_DELETION tree");
    CHECK(del_flat.count == 1, "RECORD_DELETION should cover only the surviving path, got %zu",
          del_flat.count);
    CHECK(flat_find(&del_flat, "gone.txt") < 0,
         "RECORD_DELETION still records gone.txt -- the deletion is not represented");
    pos = flat_find(&del_flat, "kept.txt");
    CHECK(pos >= 0, "RECORD_DELETION dropped kept.txt, which is still on disk");
    if (pos >= 0)
        CHECK(memcmp(del_flat.entries[pos].sha1, expected_kept, SG_SHA1_RAW_LEN) == 0,
             "kept.txt must hash the WORKING TREE content under RECORD_DELETION too");

    sg_flat_list_free(&keep_flat);
    sg_flat_list_free(&del_flat);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* Every tracked file deleted: the result is the empty tree, and that is a
   SUCCESS. A build that treated "no entries left" as an error would make
   `sg stash push` fail on a working tree where everything was removed. */
static void test_record_deletion_can_build_the_empty_tree(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    unsigned char blob[SG_SHA1_RAW_LEN];
    unsigned char tree_id[SG_SHA1_RAW_LEN], expected[SG_SHA1_RAW_LEN];
    sg_flat_list flat;

    memset(&idx, 0, sizeof(idx));
    stage_entry(git_dir, &idx, "only.txt", "staged only\n", blob);
    /* never written to disk */

    CHECK(sg_tree_build_from_workdir(git_dir, repo_root, &idx, SG_WORKDIR_MISSING_RECORD_DELETION,
                                     tree_id) == 0,
         "building a tree where every path was deleted must succeed, not fail");

    CHECK(sg_hex_to_sha1(EMPTY_TREE_HEX, expected) == 0, "bad empty-tree vector");
    CHECK(memcmp(tree_id, expected, SG_SHA1_RAW_LEN) == 0,
         "an all-deleted working tree must build git's empty tree");

    CHECK(sg_tree_flatten(git_dir, tree_id, &flat) == 0, "flatten empty tree");
    CHECK(flat.count == 0, "empty tree should flatten to 0 entries, got %zu", flat.count);

    sg_flat_list_free(&flat);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* Deleting every file inside a directory must leave NO subtree behind for it.
   Real git's trees never contain an empty subtree, so an sg tree that did
   would not round-trip through git. sg_tree_flatten only reports blobs, so
   this reads the top-level tree object directly -- flattening alone cannot
   tell an absent subtree from an empty one. */
static void test_record_deletion_leaves_no_empty_subtree(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    unsigned char b1[SG_SHA1_RAW_LEN], b2[SG_SHA1_RAW_LEN], b3[SG_SHA1_RAW_LEN];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    sg_obj_type type;
    unsigned char *content;
    size_t content_len;
    sg_tree tree;
    size_t i;
    int saw_dir = 0, saw_top = 0;

    memset(&idx, 0, sizeof(idx));
    stage_entry(git_dir, &idx, "dir/a.txt", "a\n", b1);
    stage_entry(git_dir, &idx, "dir/b.txt", "b\n", b2);
    stage_entry(git_dir, &idx, "top.txt", "top\n", b3);
    write_workdir_file(repo_root, "top.txt", "top\n");
    /* dir/a.txt and dir/b.txt deliberately absent from the working tree */

    CHECK(sg_tree_build_from_workdir(git_dir, repo_root, &idx, SG_WORKDIR_MISSING_RECORD_DELETION,
                                     tree_id) == 0,
         "RECORD_DELETION build failed");

    CHECK(sg_loose_read(git_dir, tree_id, &type, &content, &content_len) == 0,
         "top-level tree not readable");
    CHECK(type == SG_OBJ_TREE, "expected a tree object");
    CHECK(sg_tree_parse(content, content_len, &tree) == 0, "tree malformed");
    free(content);

    for (i = 0; i < tree.count; i++) {
        if (strcmp(tree.entries[i].name, "dir") == 0)
            saw_dir = 1;
        if (strcmp(tree.entries[i].name, "top.txt") == 0)
            saw_top = 1;
    }
    CHECK(!saw_dir, "an empty 'dir' subtree survived -- real git's trees never hold one");
    CHECK(saw_top, "top.txt is missing from the tree");
    CHECK(tree.count == 1, "expected exactly one entry, got %zu", tree.count);

    sg_tree_free(&tree);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* A path that EXISTS but cannot be read is not a deletion, and must not be
   resolved silently by either policy: recording the index's stale blob would
   produce a snapshot that claims to hold content it never read, and omitting
   the path would drop a file the user still has. A directory standing where a
   file is expected is used rather than chmod 000, which does nothing when the
   tests run as root. */
static void test_exists_but_unreadable_is_a_hard_failure(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    unsigned char blob[SG_SHA1_RAW_LEN];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    char abspath[SG_PATH_MAX];

    memset(&idx, 0, sizeof(idx));
    stage_entry(git_dir, &idx, "wedged.txt", "staged wedged\n", blob);

    snprintf(abspath, sizeof(abspath), "%s/wedged.txt", repo_root);
    if (mkdir(abspath, 0755) != 0) {
        fprintf(stderr, "mkdir failed for %s\n", abspath);
        exit(1);
    }

    CHECK(sg_tree_build_from_workdir(git_dir, repo_root, &idx, SG_WORKDIR_MISSING_RECORD_DELETION,
                                     tree_id) == -1,
         "RECORD_DELETION must fail on an unreadable path, not record it as deleted");
    CHECK(sg_tree_build_from_workdir(git_dir, repo_root, &idx, SG_WORKDIR_MISSING_KEEP_INDEX_BLOB,
                                     tree_id) == -1,
         "KEEP_INDEX_BLOB must fail on an unreadable path, not fall back to the stale blob");

    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* Measured against real git 2.55.0: removing a/b/c/t.txt prunes a, b and c,
   and stops at repo_root. */
static void test_prune_empty_parents_walks_up_to_repo_root(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    char abspath[SG_PATH_MAX];
    struct stat st;

    write_workdir_file(repo_root, "a/b/c/t.txt", "t\n");
    write_workdir_file(repo_root, "top.txt", "top\n");

    snprintf(abspath, sizeof(abspath), "%s/a/b/c/t.txt", repo_root);
    CHECK(remove(abspath) == 0, "could not remove the file under test");

    sg_prune_empty_parents(repo_root, "a/b/c/t.txt");

    CHECK(!dir_exists(repo_root, "a/b/c"), "a/b/c should have been pruned");
    CHECK(!dir_exists(repo_root, "a/b"), "a/b should have been pruned");
    CHECK(!dir_exists(repo_root, "a"), "a should have been pruned");
    CHECK(stat(repo_root, &st) == 0 && S_ISDIR(st.st_mode),
         "repo_root itself must never be removed");
    CHECK(dir_exists(repo_root, ".git"), "pruning must not touch anything outside the chain");

    free(repo_root);
    free(git_dir);
}

/* A directory that still holds ANYTHING is left alone -- including a file git
   would ignore, since this prune is deliberately not ignore-aware. Verified
   against real git 2.55.0, which likewise spares such a directory.

   The chain is nested (d/sub/t.txt, with the leftover one level up) so that
   this asserts BOTH halves: d/sub must go, d must stay. Asserting only that
   d survives would also pass if the function did nothing at all -- the same
   outcome for the opposite reason. */
static void test_prune_stops_at_a_directory_that_is_not_empty(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    char abspath[SG_PATH_MAX];

    write_workdir_file(repo_root, "d/sub/t.txt", "t\n");
    write_workdir_file(repo_root, "d/leftover.log", "noise\n");

    snprintf(abspath, sizeof(abspath), "%s/d/sub/t.txt", repo_root);
    CHECK(remove(abspath) == 0, "could not remove the file under test");

    sg_prune_empty_parents(repo_root, "d/sub/t.txt");

    CHECK(!dir_exists(repo_root, "d/sub"), "d/sub is empty now and should have been pruned");
    CHECK(dir_exists(repo_root, "d"), "d/ still holds leftover.log and must survive");

    free(repo_root);
    free(git_dir);
}

/* A top-level file has no ancestor to prune: the loop must stop before ever
   calling rmdir on repo_root itself. */
static void test_prune_of_a_top_level_path_is_a_no_op(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    char abspath[SG_PATH_MAX];
    struct stat st;

    write_workdir_file(repo_root, "solo.txt", "solo\n");
    snprintf(abspath, sizeof(abspath), "%s/solo.txt", repo_root);
    CHECK(remove(abspath) == 0, "could not remove the file under test");

    sg_prune_empty_parents(repo_root, "solo.txt");

    CHECK(stat(repo_root, &st) == 0 && S_ISDIR(st.st_mode),
         "repo_root must survive pruning a top-level path");

    free(repo_root);
    free(git_dir);
}

int main(void)
{
    test_policies_differ_only_on_the_missing_path();
    test_record_deletion_can_build_the_empty_tree();
    test_record_deletion_leaves_no_empty_subtree();
    test_exists_but_unreadable_is_a_hard_failure();
    test_prune_empty_parents_walks_up_to_repo_root();
    test_prune_stops_at_a_directory_that_is_not_empty();
    test_prune_of_a_top_level_path_is_a_no_op();

    if (failures > 0)
        return 1;
    printf("all tree_build workdir tests passed\n");
    return 0;
}
