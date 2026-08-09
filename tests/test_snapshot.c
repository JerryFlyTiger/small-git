#include "sg/snapshot.h"

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
    static char template[] = "/tmp/sg_snapshot_test_XXXXXX";
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

static int flat_find(const sg_flat_list *list, const char *path)
{
    size_t i;

    for (i = 0; i < list->count; i++) {
        if (strcmp(list->entries[i].path, path) == 0)
            return (int)i;
    }
    return -1;
}

/* Covers both fallback modes required by the spec: a workdir file whose
   content was modified after being staged (snapshot must use the NEW
   workdir content, not the stale index blob) and a workdir file that no
   longer exists at all (snapshot must fall back to the index-recorded
   blob so the entry still resolves). */
static void test_create_uses_workdir_or_falls_back_to_index(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    sg_index_entry e;
    unsigned char orig_a_blob[SG_SHA1_RAW_LEN];
    unsigned char orig_b_blob[SG_SHA1_RAW_LEN];
    unsigned char expected_new_a_blob[SG_SHA1_RAW_LEN];
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    sg_obj_type type;
    unsigned char *content;
    size_t content_len;
    sg_commit commit;
    sg_flat_list flat;
    int a_pos, b_pos;

    memset(&idx, 0, sizeof(idx));

    /* a.txt: staged as "original a\n", but the workdir now has different,
       uncommitted content -- the snapshot must capture the NEW content */
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "original a\n", 11, orig_a_blob) == 0,
         "write original a blob");
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, orig_a_blob, SG_SHA1_RAW_LEN);
    e.path = (char *)"a.txt";
    CHECK(sg_index_upsert(&idx, &e) == 0, "upsert a.txt");
    write_workdir_file(repo_root, "a.txt", "modified a, not committed\n");
    sg_object_hash(SG_OBJ_BLOB, "modified a, not committed\n",
                  strlen("modified a, not committed\n"), expected_new_a_blob);

    /* b.txt: staged as "original b\n", but the workdir file has since been
       deleted entirely -- the snapshot must fall back to the index blob */
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "original b\n", 11, orig_b_blob) == 0,
         "write original b blob");
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, orig_b_blob, SG_SHA1_RAW_LEN);
    e.path = (char *)"b.txt";
    CHECK(sg_index_upsert(&idx, &e) == 0, "upsert b.txt");
    /* deliberately never written to repo_root/b.txt */

    CHECK(sg_snapshot_create(git_dir, repo_root, &idx, "test snapshot", commit_id) == 0,
         "sg_snapshot_create failed");

    CHECK(sg_loose_read(git_dir, commit_id, &type, &content, &content_len) == 0,
         "snapshot commit not readable");
    CHECK(type == SG_OBJ_COMMIT, "snapshot object should be a commit");
    CHECK(sg_commit_parse(content, content_len, &commit) == 0, "snapshot commit malformed");
    free(content);

    /* sg_snapshot_create runs the label through sg_message_cleanup, the same
       normalization real git applies to commit messages, so a trailing '\n'
       gets appended here. */
    CHECK(strcmp(commit.message, "test snapshot\n") == 0, "message mismatch: %s", commit.message);
    CHECK(commit.parent_count == 0, "brand new repo has no HEAD, snapshot should have no parent");

    CHECK(sg_tree_flatten(git_dir, commit.tree, &flat) == 0, "flatten snapshot tree failed");
    CHECK(flat.count == 2, "expected 2 entries, got %zu", flat.count);

    a_pos = flat_find(&flat, "a.txt");
    CHECK(a_pos >= 0, "a.txt missing from snapshot tree");
    if (a_pos >= 0)
        CHECK(memcmp(flat.entries[a_pos].sha1, expected_new_a_blob, SG_SHA1_RAW_LEN) == 0,
             "a.txt should snapshot the NEW workdir content, not the stale index blob");

    b_pos = flat_find(&flat, "b.txt");
    CHECK(b_pos >= 0, "b.txt missing from snapshot tree");
    if (b_pos >= 0)
        CHECK(memcmp(flat.entries[b_pos].sha1, orig_b_blob, SG_SHA1_RAW_LEN) == 0,
             "b.txt should fall back to the index blob when the workdir file is gone");

    sg_flat_list_free(&flat);
    sg_commit_free(&commit);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* Creating several snapshots back-to-back (almost certainly within the same
   wall-clock second on any reasonably fast machine) must never let a later
   snapshot's ref clobber an earlier one. */
static void test_same_second_snapshots_do_not_collide(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    sg_index_entry e;
    unsigned char blob[SG_SHA1_RAW_LEN];
    sg_snapshot_list list;
    const int n = 5;
    int i, j;

    memset(&idx, 0, sizeof(idx));
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "content\n", 8, blob) == 0, "write blob");
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, blob, SG_SHA1_RAW_LEN);
    e.path = (char *)"f.txt";
    CHECK(sg_index_upsert(&idx, &e) == 0, "upsert f.txt");
    write_workdir_file(repo_root, "f.txt", "content\n");

    for (i = 0; i < n; i++) {
        unsigned char commit_id[SG_SHA1_RAW_LEN];

        CHECK(sg_snapshot_create(git_dir, repo_root, &idx, "same label", commit_id) == 0,
             "snapshot #%d create failed", i);
    }

    CHECK(sg_snapshot_list_read(git_dir, &list) == 0, "listing failed");
    CHECK(list.count == (size_t)n, "expected %d snapshots, got %zu", n, list.count);

    for (i = 0; i < (int)list.count; i++) {
        for (j = i + 1; j < (int)list.count; j++) {
            CHECK(strcmp(list.entries[i].ref_name, list.entries[j].ref_name) != 0,
                 "duplicate ref_name '%s' -- a snapshot overwrote another", list.entries[i].ref_name);
        }
    }

    sg_snapshot_list_free(&list);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

static void test_list_empty_when_no_snapshots_yet(void)
{
    char *git_dir = make_tmp_repo();
    sg_snapshot_list list;

    CHECK(sg_snapshot_list_read(git_dir, &list) == 0, "listing an untouched repo should not fail");
    CHECK(list.count == 0, "expected no snapshots, got %zu", list.count);

    sg_snapshot_list_free(&list);
    free(git_dir);
}

int main(void)
{
    test_create_uses_workdir_or_falls_back_to_index();
    test_same_second_snapshots_do_not_collide();
    test_list_empty_when_no_snapshots_yet();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all snapshot tests passed\n");
    return 0;
}
