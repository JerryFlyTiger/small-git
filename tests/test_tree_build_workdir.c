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
                                     NULL, keep_tree, NULL) == 0,
         "KEEP_INDEX_BLOB build failed");
    CHECK(sg_tree_build_from_workdir(git_dir, repo_root, &idx, SG_WORKDIR_MISSING_RECORD_DELETION,
                                     NULL, del_tree, NULL) == 0,
         "RECORD_DELETION build failed");

    CHECK(memcmp(keep_tree, del_tree, SG_SHA1_RAW_LEN) != 0,
         "the two policies produced the SAME tree -- the missing path was treated identically");

    CHECK(sg_tree_flatten(git_dir, keep_tree, &keep_flat, NULL) == 0, "flatten KEEP_INDEX_BLOB tree");
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

    CHECK(sg_tree_flatten(git_dir, del_tree, &del_flat, NULL) == 0, "flatten RECORD_DELETION tree");
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
                                     NULL, tree_id, NULL) == 0,
         "building a tree where every path was deleted must succeed, not fail");

    CHECK(sg_hex_to_sha1(EMPTY_TREE_HEX, expected) == 0, "bad empty-tree vector");
    CHECK(memcmp(tree_id, expected, SG_SHA1_RAW_LEN) == 0,
         "an all-deleted working tree must build git's empty tree");

    CHECK(sg_tree_flatten(git_dir, tree_id, &flat, NULL) == 0, "flatten empty tree");
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
                                     NULL, tree_id, NULL) == 0,
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
                                     NULL, tree_id, NULL) == -1,
         "RECORD_DELETION must fail on an unreadable path, not record it as deleted");
    CHECK(sg_tree_build_from_workdir(git_dir, repo_root, &idx, SG_WORKDIR_MISSING_KEEP_INDEX_BLOB,
                                     NULL, tree_id, NULL) == -1,
         "KEEP_INDEX_BLOB must fail on an unreadable path, not fall back to the stale blob");

    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* Phase 81b round 2 (item 1, regression A): the index says 120000 (a
   symlink) but the working tree now holds an ORDINARY REGULAR FILE at that
   path -- readlink() on a non-symlink fails EINVAL, so a build that decides
   readlink-vs-fopen from the INDEX's mode (round 1's bug) hard-fails the
   whole build even though the file is perfectly readable via fopen. Must
   succeed and record the file's OWN observed content and mode (100644),
   never the stale index mode -- matching `git stash push` (measured:
   records 100644, not 120000, in this exact shape). */
static void test_index_symlink_worktree_now_regular_file(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    sg_index_entry e;
    unsigned char stale_blob[SG_SHA1_RAW_LEN];
    unsigned char expected_content[SG_SHA1_RAW_LEN];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    sg_flat_list flat;
    int pos;

    memset(&idx, 0, sizeof(idx));

    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "old-target", strlen("old-target"), stale_blob) == 0,
         "sg_loose_write for the stale symlink-target blob failed");
    memset(&e, 0, sizeof(e));
    e.mode = 0120000;
    memcpy(e.sha1, stale_blob, SG_SHA1_RAW_LEN);
    e.path = (char *)"s";
    CHECK(sg_index_upsert(&idx, &e) == 0, "sg_index_upsert for s (120000) failed");

    write_workdir_file(repo_root, "s", "now a regular file\n");
    sg_object_hash(SG_OBJ_BLOB, "now a regular file\n", strlen("now a regular file\n"),
                   expected_content);

    CHECK(sg_tree_build_from_workdir(git_dir, repo_root, &idx, SG_WORKDIR_MISSING_KEEP_INDEX_BLOB,
                                     NULL, tree_id, NULL) == 0,
         "build must succeed when the on-disk type differs from the index (120000 -> regular)");

    CHECK(sg_tree_flatten(git_dir, tree_id, &flat, NULL) == 0, "flatten failed");
    pos = flat_find(&flat, "s");
    CHECK(pos >= 0, "s missing from the resulting tree");
    if (pos >= 0) {
        CHECK(flat.entries[pos].mode == 0100644, "expected mode 100644 (observed), got %o",
             flat.entries[pos].mode);
        CHECK(memcmp(flat.entries[pos].sha1, expected_content, SG_SHA1_RAW_LEN) == 0,
             "expected the file's OWN content hash, not the stale index blob");
    }

    sg_flat_list_free(&flat);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* Phase 81b round 2 (item 1, regression B): the index says 100644 (a
   regular file) but the working tree now holds a SYMLINK to a separate,
   readable, tracked file -- a build that decides readlink-vs-fopen from
   the INDEX's mode (round 1's bug) calls fopen(), which FOLLOWS the
   symlink and hashes the TARGET file's content under mode 100644 -- the
   exact pre-Phase-81b bug this whole phase exists to fix, reintroduced by
   round 1 for this one direction. Must record the symlink's OWN readlink
   target text under mode 120000, matching `git stash push` (measured:
   records 120000 with the target TEXT as content, in this exact shape,
   even though target.txt itself is a separate, unrelated tracked file). */
static void test_index_regular_worktree_now_symlink_to_tracked_file(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    sg_index_entry e;
    unsigned char stale_blob[SG_SHA1_RAW_LEN];
    unsigned char target_blob[SG_SHA1_RAW_LEN];
    unsigned char expected_link_content[SG_SHA1_RAW_LEN];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    sg_flat_list flat;
    int pos;

    memset(&idx, 0, sizeof(idx));

    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "plain content\n", strlen("plain content\n"),
                         stale_blob) == 0,
         "sg_loose_write for the stale plain-file blob failed");
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, stale_blob, SG_SHA1_RAW_LEN);
    e.path = (char *)"s";
    CHECK(sg_index_upsert(&idx, &e) == 0, "sg_index_upsert for s (100644) failed");

    /* target.txt is a second tracked file, readable, so fopen(s) would
       "succeed" (following the symlink) if the bug were still present. */
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "target content\n", strlen("target content\n"),
                         target_blob) == 0,
         "sg_loose_write for target.txt's blob failed");
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, target_blob, SG_SHA1_RAW_LEN);
    e.path = (char *)"target.txt";
    CHECK(sg_index_upsert(&idx, &e) == 0, "sg_index_upsert for target.txt failed");
    write_workdir_file(repo_root, "target.txt", "target content\n");

    {
        char abspath[SG_PATH_MAX];

        snprintf(abspath, sizeof(abspath), "%s/s", repo_root);
        CHECK(symlink("target.txt", abspath) == 0, "symlink() for s failed");
    }
    sg_object_hash(SG_OBJ_BLOB, "target.txt", strlen("target.txt"), expected_link_content);

    CHECK(sg_tree_build_from_workdir(git_dir, repo_root, &idx, SG_WORKDIR_MISSING_KEEP_INDEX_BLOB,
                                     NULL, tree_id, NULL) == 0,
         "build must succeed when the on-disk type differs from the index (100644 -> symlink)");

    CHECK(sg_tree_flatten(git_dir, tree_id, &flat, NULL) == 0, "flatten failed");
    pos = flat_find(&flat, "s");
    CHECK(pos >= 0, "s missing from the resulting tree");
    if (pos >= 0) {
        CHECK(flat.entries[pos].mode == 0120000, "expected mode 120000 (observed), got %o",
             flat.entries[pos].mode);
        CHECK(memcmp(flat.entries[pos].sha1, expected_link_content, SG_SHA1_RAW_LEN) == 0,
             "expected the symlink's OWN readlink target text (\"target.txt\"), not target.txt's "
             "followed-through content");
    }

    sg_flat_list_free(&flat);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* Phase 81b round 3 (item 1): the mutation `entry_mode != 0120000` ->
   `idx->entries[i].mode != 0120000` in the chunk-eligibility test stayed
   green against every existing fixture, because chunking is disabled
   everywhere else in this file and in tests/interop.sh's B18/B19 -- that
   branch was never actually exercised with chunking ON. These two tests
   turn chunk storage on (append an [sg] section directly to git_dir/config,
   the same mechanism `git config -f .../config sg.chunking true` uses at
   the interop level) and use a low threshold so an ordinary-sized fixture
   crosses it, discriminating the two conditions directly. */
static void enable_chunking(const char *git_dir, size_t threshold)
{
    char path[SG_PATH_MAX];
    FILE *f;

    snprintf(path, sizeof(path), "%s/config", git_dir);
    f = fopen(path, "a");
    if (f == NULL) {
        fprintf(stderr, "setup failed: could not open %s for chunk config\n", path);
        exit(1);
    }
    fprintf(f, "[sg]\n\tchunking = true\n\tchunkthreshold = %zu\n", threshold);
    fclose(f);
}

/* A blob stored WITHOUT chunking always has its raw tree-entry sha1 equal
   to sg_object_hash of its own content directly (that is what "ordinary
   loose blob" means). A CHUNKED blob's tree-entry sha1 is the pointer
   object's own hash instead, which -- by SHA-1's collision resistance --
   can never equal the content's direct hash. So "does the entry's sha1
   equal the content's direct hash" is an exact, one-line discriminator for
   "was this chunked", with no need to reach into storage/chunk.c's pointer
   format at all. */
static int blob_is_chunked(const unsigned char entry_sha1[SG_SHA1_RAW_LEN], const void *content,
                           size_t len)
{
    unsigned char direct[SG_SHA1_RAW_LEN];

    sg_object_hash(SG_OBJ_BLOB, content, len, direct);
    return memcmp(entry_sha1, direct, SG_SHA1_RAW_LEN) != 0;
}

/* Regression A, chunk-aware: index says 120000 (a small symlink-target
   blob) but the worktree now holds an ordinary REGULAR file large enough
   to cross the chunk threshold. Correct sg behaviour (matching what a
   REGULAR file always gets once chunking is on): the observed mode is
   100644, and the content IS chunked. */
static void test_index_symlink_worktree_now_large_regular_file_gets_chunked(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    sg_index_entry e;
    unsigned char stale_blob[SG_SHA1_RAW_LEN];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    sg_flat_list flat;
    int pos;
    char *big;
    size_t big_len = 5000;
    size_t i;

    enable_chunking(git_dir, 100); /* threshold well below big_len */

    memset(&idx, 0, sizeof(idx));
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "old-target", strlen("old-target"), stale_blob) == 0,
         "sg_loose_write for the stale symlink-target blob failed");
    memset(&e, 0, sizeof(e));
    e.mode = 0120000;
    memcpy(e.sha1, stale_blob, SG_SHA1_RAW_LEN);
    e.path = (char *)"s";
    CHECK(sg_index_upsert(&idx, &e) == 0, "sg_index_upsert for s (120000) failed");

    big = malloc(big_len);
    CHECK(big != NULL, "malloc for the large regular file failed");
    if (big == NULL) {
        sg_index_free(&idx);
        free(repo_root);
        free(git_dir);
        return;
    }
    for (i = 0; i < big_len; i++)
        big[i] = (char)('a' + (i % 26));
    {
        char abspath[SG_PATH_MAX];
        FILE *f;

        snprintf(abspath, sizeof(abspath), "%s/s", repo_root);
        f = fopen(abspath, "wb");
        CHECK(f != NULL, "fopen for the large regular file failed");
        if (f != NULL) {
            CHECK(fwrite(big, 1, big_len, f) == big_len, "fwrite for the large regular file failed");
            fclose(f);
        }
    }

    CHECK(sg_tree_build_from_workdir(git_dir, repo_root, &idx, SG_WORKDIR_MISSING_KEEP_INDEX_BLOB,
                                     NULL, tree_id, NULL) == 0,
         "build must succeed (120000 -> large regular file, chunking on)");

    CHECK(sg_tree_flatten(git_dir, tree_id, &flat, NULL) == 0, "flatten failed");
    pos = flat_find(&flat, "s");
    CHECK(pos >= 0, "s missing from the resulting tree");
    if (pos >= 0) {
        CHECK(flat.entries[pos].mode == 0100644, "expected mode 100644 (observed), got %o",
             flat.entries[pos].mode);
        CHECK(blob_is_chunked(flat.entries[pos].sha1, big, big_len),
             "a large REGULAR file above the threshold must be chunked, even though the INDEX "
             "said 120000");
    }

    sg_flat_list_free(&flat);
    free(big);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* Regression B, chunk-aware: index says 100644 (a small blob) but the
   worktree now holds a SYMLINK whose target text is long enough to cross
   the (deliberately tiny) chunk threshold. Correct sg behaviour (item 1's
   own requirement): a symlink's target text must NEVER be chunk-encoded,
   regardless of its length relative to the threshold. */
static void test_index_regular_worktree_now_symlink_target_never_chunked(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    sg_index_entry e;
    unsigned char stale_blob[SG_SHA1_RAW_LEN];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    sg_flat_list flat;
    int pos;
    char *target;
    size_t target_len = 500; /* well under macOS's ~1023-byte symlink() ceiling */
    size_t i;

    enable_chunking(git_dir, 10); /* threshold well below target_len */

    memset(&idx, 0, sizeof(idx));
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "plain content\n", strlen("plain content\n"),
                         stale_blob) == 0,
         "sg_loose_write for the stale plain-file blob failed");
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, stale_blob, SG_SHA1_RAW_LEN);
    e.path = (char *)"s";
    CHECK(sg_index_upsert(&idx, &e) == 0, "sg_index_upsert for s (100644) failed");

    target = malloc(target_len + 1);
    CHECK(target != NULL, "malloc for the long symlink target failed");
    if (target == NULL) {
        sg_index_free(&idx);
        free(repo_root);
        free(git_dir);
        return;
    }
    for (i = 0; i < target_len; i++)
        target[i] = (char)('a' + (i % 26));
    target[target_len] = '\0';

    {
        char abspath[SG_PATH_MAX];

        snprintf(abspath, sizeof(abspath), "%s/s", repo_root);
        CHECK(symlink(target, abspath) == 0, "symlink() for the long target failed");
    }

    CHECK(sg_tree_build_from_workdir(git_dir, repo_root, &idx, SG_WORKDIR_MISSING_KEEP_INDEX_BLOB,
                                     NULL, tree_id, NULL) == 0,
         "build must succeed (100644 -> long symlink, chunking on)");

    CHECK(sg_tree_flatten(git_dir, tree_id, &flat, NULL) == 0, "flatten failed");
    pos = flat_find(&flat, "s");
    CHECK(pos >= 0, "s missing from the resulting tree");
    if (pos >= 0) {
        CHECK(flat.entries[pos].mode == 0120000, "expected mode 120000 (observed), got %o",
             flat.entries[pos].mode);
        CHECK(!blob_is_chunked(flat.entries[pos].sha1, target, target_len),
             "a symlink's target text must NEVER be chunked, even above the threshold");
    }

    sg_flat_list_free(&flat);
    free(target);
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

/* relpath comes from an index entry or a merge result, which came out of a
   tree object, and sg does not validate entry names when parsing one. Before
   the confinement guard existed this was measured, not theorised: an
   ".." component rmdir'd a directory OUTSIDE the repository, and a leading
   "/" reached rmdir(repo_root) itself. */
static void test_prune_refuses_to_escape_repo_root(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    char sibling[SG_PATH_MAX];
    struct stat st;

    /* An empty directory alongside the repository, which an unconfined
       "../<name>/f.txt" would resolve to and remove. */
    snprintf(sibling, sizeof(sibling), "%s.sibling", repo_root);
    if (mkdir(sibling, 0755) != 0) {
        fprintf(stderr, "mkdir failed for %s\n", sibling);
        exit(1);
    }

    {
        char escaping[SG_PATH_MAX];
        const char *base = strrchr(sibling, '/');

        snprintf(escaping, sizeof(escaping), "../%s/f.txt", base != NULL ? base + 1 : sibling);
        sg_prune_empty_parents(repo_root, escaping);
    }
    CHECK(stat(sibling, &st) == 0 && S_ISDIR(st.st_mode),
         "a '..' component must never reach a directory outside the repository");

    sg_prune_empty_parents(repo_root, "/f.txt");
    CHECK(stat(repo_root, &st) == 0 && S_ISDIR(st.st_mode),
         "an absolute relpath must never reach rmdir(repo_root)");

    /* A name that merely starts with dots is an ordinary name, not an escape:
       the guard must not refuse it and silently stop pruning real work. */
    write_workdir_file(repo_root, "...dots/t.txt", "t\n");
    {
        char abspath[SG_PATH_MAX];

        snprintf(abspath, sizeof(abspath), "%s/...dots/t.txt", repo_root);
        CHECK(remove(abspath) == 0, "could not remove the file under test");
    }
    sg_prune_empty_parents(repo_root, "...dots/t.txt");
    CHECK(!dir_exists(repo_root, "...dots"),
         "'...dots' is an ordinary directory name and must still be pruned");

    rmdir(sibling);
    free(repo_root);
    free(git_dir);
}

/* The length check guarding the strcpy below it. relpath is EXACTLY
   SG_PATH_MAX bytes, the first length the check must refuse: one byte
   shorter fits with room for the NUL, so this is the boundary and not just
   "something long". Relaxing >= to > here writes one byte past cur, which
   only a sanitizer build reliably reports -- make test alone is not
   sufficient evidence for this assertion. */
static void test_prune_refuses_a_relpath_at_the_buffer_boundary(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    char *huge = malloc(SG_PATH_MAX + 1);
    struct stat st;

    if (huge == NULL) {
        fprintf(stderr, "malloc failed\n");
        exit(1);
    }
    memset(huge, 'a', SG_PATH_MAX);
    huge[SG_PATH_MAX] = '\0';
    huge[8] = '/';

    sg_prune_empty_parents(repo_root, huge);
    CHECK(stat(repo_root, &st) == 0 && S_ISDIR(st.st_mode),
         "a relpath at the buffer boundary must be refused, leaving the repository intact");

    free(huge);
    free(repo_root);
    free(git_dir);
}

int main(void)
{
    test_policies_differ_only_on_the_missing_path();
    test_record_deletion_can_build_the_empty_tree();
    test_record_deletion_leaves_no_empty_subtree();
    test_exists_but_unreadable_is_a_hard_failure();
    test_index_symlink_worktree_now_regular_file();
    test_index_regular_worktree_now_symlink_to_tracked_file();
    test_index_symlink_worktree_now_large_regular_file_gets_chunked();
    test_index_regular_worktree_now_symlink_target_never_chunked();
    test_prune_empty_parents_walks_up_to_repo_root();
    test_prune_stops_at_a_directory_that_is_not_empty();
    test_prune_of_a_top_level_path_is_a_no_op();
    test_prune_refuses_to_escape_repo_root();
    test_prune_refuses_a_relpath_at_the_buffer_boundary();

    if (failures > 0)
        return 1;
    printf("all tree_build workdir tests passed\n");
    return 0;
}
