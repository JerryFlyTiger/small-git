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
    static char template[] = "/tmp/sg_apply_tree_test_XXXXXX";
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

/* Builds a target tree by hand (kept.txt + new.txt), seeds an old index/
   workdir that also tracks a stale.txt that is NOT in the target tree, then
   asserts sg_apply_tree_to_workdir writes/overwrites the target's paths,
   deletes the now-untracked stale.txt, and rebuilds the index to match the
   target tree exactly. */
static void test_apply_writes_deletes_and_rebuilds_index(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char kept_blob[SG_SHA1_RAW_LEN];
    unsigned char new_blob[SG_SHA1_RAW_LEN];
    unsigned char stale_blob[SG_SHA1_RAW_LEN];
    sg_flat_entry entries[2];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    sg_index old_idx;
    sg_index_entry e;
    sg_index reloaded;
    char *kept_content;
    char *new_content;

    /* old on-disk state: kept.txt (present in target, different content),
       stale.txt (absent from target -- must be removed) */
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "kept old content\n", 18, kept_blob) == 0,
         "write kept blob");
    write_workdir_file(repo_root, "kept.txt", "kept old content\n");
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "stale content\n", 14, stale_blob) == 0,
         "write stale blob");
    write_workdir_file(repo_root, "stale.txt", "stale content\n");

    memset(&old_idx, 0, sizeof(old_idx));
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, kept_blob, SG_SHA1_RAW_LEN);
    e.path = (char *)"kept.txt";
    CHECK(sg_index_upsert(&old_idx, &e) == 0, "upsert kept.txt into old index");
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, stale_blob, SG_SHA1_RAW_LEN);
    e.path = (char *)"stale.txt";
    CHECK(sg_index_upsert(&old_idx, &e) == 0, "upsert stale.txt into old index");
    CHECK(sg_index_write(git_dir, &old_idx) == 0, "write old index");
    sg_index_free(&old_idx);

    /* target tree: kept.txt (new content, replacing the old blob) + new.txt */
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "kept new content\n", 18, kept_blob) == 0,
         "write kept's new blob");
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "brand new file\n", 16, new_blob) == 0,
         "write new blob");

    entries[0].path = strdup("kept.txt");
    entries[0].mode = 0100644;
    memcpy(entries[0].sha1, kept_blob, SG_SHA1_RAW_LEN);
    entries[1].path = strdup("new.txt");
    entries[1].mode = 0100644;
    memcpy(entries[1].sha1, new_blob, SG_SHA1_RAW_LEN);

    CHECK(sg_tree_build(git_dir, entries, 2, tree_id) == 0, "tree build failed");
    free(entries[0].path);
    free(entries[1].path);

    CHECK(sg_apply_tree_to_workdir(git_dir, repo_root, tree_id) == 0,
         "sg_apply_tree_to_workdir failed");

    CHECK(!file_exists(repo_root, "stale.txt"), "stale.txt should have been removed");
    CHECK(file_exists(repo_root, "kept.txt"), "kept.txt should still exist");
    CHECK(file_exists(repo_root, "new.txt"), "new.txt should have been written");

    kept_content = read_workdir_file(repo_root, "kept.txt");
    CHECK(kept_content != NULL && strcmp(kept_content, "kept new content\n") == 0,
         "kept.txt should have the target tree's content, got %s",
         kept_content != NULL ? kept_content : "(null)");
    free(kept_content);

    new_content = read_workdir_file(repo_root, "new.txt");
    CHECK(new_content != NULL && strcmp(new_content, "brand new file\n") == 0,
         "new.txt should have the target tree's content, got %s",
         new_content != NULL ? new_content : "(null)");
    free(new_content);

    CHECK(sg_index_read(git_dir, &reloaded) == 0, "reading rebuilt index failed");
    CHECK(reloaded.count == 2, "rebuilt index should have exactly 2 entries, got %zu",
         reloaded.count);
    CHECK(sg_index_find(&reloaded, "stale.txt") < 0, "stale.txt should not be in the rebuilt index");
    CHECK(sg_index_find(&reloaded, "kept.txt") >= 0, "kept.txt should be in the rebuilt index");
    CHECK(sg_index_find(&reloaded, "new.txt") >= 0, "new.txt should be in the rebuilt index");
    if (sg_index_find(&reloaded, "kept.txt") >= 0) {
        int pos = sg_index_find(&reloaded, "kept.txt");

        CHECK(memcmp(reloaded.entries[pos].sha1, kept_blob, SG_SHA1_RAW_LEN) == 0,
             "rebuilt index's kept.txt sha1 should match the target tree's blob");
    }
    sg_index_free(&reloaded);

    free(repo_root);
    free(git_dir);
}

int main(void)
{
    test_apply_writes_deletes_and_rebuilds_index();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all apply_tree tests passed\n");
    return 0;
}
