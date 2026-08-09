#include "sg/apply.h"

#include "sg/index.h"
#include "sg/loose.h"
#include "sg/object.h"
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
    static char template[] = "/tmp/sg_index_reset_test_XXXXXX";
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

/* Old index has two paths: unchanged.txt (same sha1 as the target tree) and
   changed.txt (different sha1 in the target tree). A third path,
   removed.txt, is in the old index but absent from the target tree. After
   sg_index_reset_to_tree:
     - unchanged.txt keeps the old entry's stat fields (sha1 match -> reuse)
     - changed.txt has every stat field zeroed (sha1 mismatch -> zero out)
     - removed.txt is gone from the rebuilt index
   The working directory is never touched: no files are written for this
   test, sg_index_reset_to_tree must not need or use one. */
static void test_reset_reuses_stat_only_when_sha_matches(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char unchanged_blob[SG_SHA1_RAW_LEN];
    unsigned char changed_old_blob[SG_SHA1_RAW_LEN];
    unsigned char changed_new_blob[SG_SHA1_RAW_LEN];
    unsigned char removed_blob[SG_SHA1_RAW_LEN];
    sg_flat_entry target_entries[2];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    sg_index old_idx;
    sg_index_entry e;
    sg_index result;
    int pos;

    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "unchanged\n", 10, unchanged_blob) == 0,
         "write unchanged blob");
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "old content\n", 13, changed_old_blob) == 0,
         "write changed's old blob");
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "new content\n", 13, changed_new_blob) == 0,
         "write changed's new blob");
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "removed\n", 8, removed_blob) == 0,
         "write removed blob");

    memset(&old_idx, 0, sizeof(old_idx));

    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    e.ctime_sec = 111;
    e.ctime_nsec = 112;
    e.mtime_sec = 222;
    e.mtime_nsec = 223;
    e.dev = 3;
    e.ino = 4;
    e.uid = 5;
    e.gid = 6;
    e.file_size = 10;
    memcpy(e.sha1, unchanged_blob, SG_SHA1_RAW_LEN);
    e.path = (char *)"unchanged.txt";
    CHECK(sg_index_upsert(&old_idx, &e) == 0, "upsert unchanged.txt");

    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    e.ctime_sec = 333;
    e.ctime_nsec = 334;
    e.mtime_sec = 444;
    e.mtime_nsec = 445;
    e.dev = 7;
    e.ino = 8;
    e.uid = 9;
    e.gid = 10;
    e.file_size = 13;
    memcpy(e.sha1, changed_old_blob, SG_SHA1_RAW_LEN);
    e.path = (char *)"changed.txt";
    CHECK(sg_index_upsert(&old_idx, &e) == 0, "upsert changed.txt");

    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, removed_blob, SG_SHA1_RAW_LEN);
    e.path = (char *)"removed.txt";
    CHECK(sg_index_upsert(&old_idx, &e) == 0, "upsert removed.txt");

    CHECK(sg_index_write(git_dir, &old_idx) == 0, "write old index");
    sg_index_free(&old_idx);

    /* target tree: unchanged.txt (same blob) + changed.txt (different blob).
       removed.txt is intentionally absent. */
    target_entries[0].path = strdup("changed.txt");
    target_entries[0].mode = 0100644;
    memcpy(target_entries[0].sha1, changed_new_blob, SG_SHA1_RAW_LEN);
    target_entries[1].path = strdup("unchanged.txt");
    target_entries[1].mode = 0100644;
    memcpy(target_entries[1].sha1, unchanged_blob, SG_SHA1_RAW_LEN);

    CHECK(sg_tree_build(git_dir, target_entries, 2, tree_id) == 0, "tree build failed");
    free(target_entries[0].path);
    free(target_entries[1].path);

    CHECK(sg_index_reset_to_tree(git_dir, tree_id) == 0, "sg_index_reset_to_tree failed");

    CHECK(sg_index_read(git_dir, &result) == 0, "reading reset index failed");
    CHECK(result.count == 2, "reset index should have exactly 2 entries, got %zu", result.count);
    CHECK(sg_index_find(&result, "removed.txt") < 0, "removed.txt should not be in the reset index");

    pos = sg_index_find(&result, "unchanged.txt");
    CHECK(pos >= 0, "unchanged.txt should be in the reset index");
    if (pos >= 0) {
        CHECK(memcmp(result.entries[pos].sha1, unchanged_blob, SG_SHA1_RAW_LEN) == 0,
             "unchanged.txt sha1 should match the target tree's blob");
        CHECK(result.entries[pos].ctime_sec == 111, "unchanged.txt should keep old ctime_sec, got %u",
             result.entries[pos].ctime_sec);
        CHECK(result.entries[pos].ctime_nsec == 112, "unchanged.txt should keep old ctime_nsec, got %u",
             result.entries[pos].ctime_nsec);
        CHECK(result.entries[pos].mtime_sec == 222, "unchanged.txt should keep old mtime_sec, got %u",
             result.entries[pos].mtime_sec);
        CHECK(result.entries[pos].mtime_nsec == 223, "unchanged.txt should keep old mtime_nsec, got %u",
             result.entries[pos].mtime_nsec);
        CHECK(result.entries[pos].dev == 3, "unchanged.txt should keep old dev, got %u",
             result.entries[pos].dev);
        CHECK(result.entries[pos].ino == 4, "unchanged.txt should keep old ino, got %u",
             result.entries[pos].ino);
        CHECK(result.entries[pos].uid == 5, "unchanged.txt should keep old uid, got %u",
             result.entries[pos].uid);
        CHECK(result.entries[pos].gid == 6, "unchanged.txt should keep old gid, got %u",
             result.entries[pos].gid);
        CHECK(result.entries[pos].file_size == 10, "unchanged.txt should keep old file_size, got %u",
             result.entries[pos].file_size);
    }

    pos = sg_index_find(&result, "changed.txt");
    CHECK(pos >= 0, "changed.txt should be in the reset index");
    if (pos >= 0) {
        CHECK(memcmp(result.entries[pos].sha1, changed_new_blob, SG_SHA1_RAW_LEN) == 0,
             "changed.txt sha1 should match the target tree's (new) blob");
        CHECK(result.entries[pos].ctime_sec == 0, "changed.txt ctime_sec should be zeroed, got %u",
             result.entries[pos].ctime_sec);
        CHECK(result.entries[pos].ctime_nsec == 0, "changed.txt ctime_nsec should be zeroed, got %u",
             result.entries[pos].ctime_nsec);
        CHECK(result.entries[pos].mtime_sec == 0, "changed.txt mtime_sec should be zeroed, got %u",
             result.entries[pos].mtime_sec);
        CHECK(result.entries[pos].mtime_nsec == 0, "changed.txt mtime_nsec should be zeroed, got %u",
             result.entries[pos].mtime_nsec);
        CHECK(result.entries[pos].dev == 0, "changed.txt dev should be zeroed, got %u",
             result.entries[pos].dev);
        CHECK(result.entries[pos].ino == 0, "changed.txt ino should be zeroed, got %u",
             result.entries[pos].ino);
        CHECK(result.entries[pos].uid == 0, "changed.txt uid should be zeroed, got %u",
             result.entries[pos].uid);
        CHECK(result.entries[pos].gid == 0, "changed.txt gid should be zeroed, got %u",
             result.entries[pos].gid);
        CHECK(result.entries[pos].file_size == 0, "changed.txt file_size should be zeroed, got %u",
             result.entries[pos].file_size);
    }

    sg_index_free(&result);
    free(git_dir);
}

int main(void)
{
    test_reset_reuses_stat_only_when_sha_matches();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all index_reset_to_tree tests passed\n");
    return 0;
}
