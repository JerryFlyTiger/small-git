#include "sg/diff.h"

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
    static char template[] = "/tmp/sg_diff_list_test_XXXXXX";
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

static void remove_workdir_file(const char *repo_root, const char *rel)
{
    char abspath[4096];

    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, rel);
    CHECK(remove(abspath) == 0, "failed to remove workdir file %s", rel);
}

static unsigned char *blob(const char *git_dir, const char *content, unsigned char id[SG_SHA1_RAW_LEN])
{
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, content, strlen(content), id) == 0,
         "failed to write blob %s", content);
    return id;
}

/* Builds a tree out of {path, mode, content} triples, already caller-sorted
   by path (sg_tree_build requires that). */
typedef struct {
    const char *path;
    unsigned int mode;
    const char *content;
} tree_spec;

static void build_tree(const char *git_dir, const tree_spec *specs, size_t count,
                       unsigned char tree_id[SG_SHA1_RAW_LEN])
{
    sg_flat_entry *entries = malloc(count * sizeof(*entries));
    size_t i;

    for (i = 0; i < count; i++) {
        entries[i].path = strdup(specs[i].path);
        entries[i].mode = specs[i].mode;
        blob(git_dir, specs[i].content, entries[i].sha1);
    }
    CHECK(sg_tree_build(git_dir, entries, count, tree_id) == 0, "sg_tree_build failed");
    for (i = 0; i < count; i++)
        free(entries[i].path);
    free(entries);
}

static void index_upsert_blob(sg_index *idx, const char *path, unsigned int mode,
                              const unsigned char id[SG_SHA1_RAW_LEN])
{
    sg_index_entry e;

    memset(&e, 0, sizeof(e));
    e.mode = mode;
    memcpy(e.sha1, id, SG_SHA1_RAW_LEN);
    e.path = (char *)path;
    CHECK(sg_index_upsert(idx, &e) == 0, "upsert %s into index", path);
}

/* Finds the entry for path in list, or NULL. */
static const sg_diff_entry *find_entry(const sg_diff_list *list, const char *path)
{
    size_t i;

    for (i = 0; i < list->count; i++) {
        if (strcmp(list->entries[i].path, path) == 0)
            return &list->entries[i];
    }
    return NULL;
}

/* 1. tree vs tree: additions, deletions, modifications classified correctly,
      and a path whose content is identical on both sides is left out. */
static void test_trees_add_delete_modify(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char old_tree[SG_SHA1_RAW_LEN];
    unsigned char new_tree[SG_SHA1_RAW_LEN];
    tree_spec old_specs[] = {
        {"deleted.txt", 0100644, "gone soon\n"},
        {"same.txt", 0100644, "unchanged\n"},
        {"changed.txt", 0100644, "before\n"},
    };
    tree_spec new_specs[] = {
        {"added.txt", 0100644, "brand new\n"},
        {"changed.txt", 0100644, "after\n"},
        {"same.txt", 0100644, "unchanged\n"},
    };
    sg_diff_list list;
    const sg_diff_entry *e;

    build_tree(git_dir, old_specs, 3, old_tree);
    build_tree(git_dir, new_specs, 3, new_tree);

    CHECK(sg_diff_trees(git_dir, old_tree, new_tree, &list, NULL) == 0, "sg_diff_trees failed");
    CHECK(list.count == 3, "expected 3 changed paths (add/delete/modify), got %zu", list.count);

    e = find_entry(&list, "added.txt");
    CHECK(e != NULL, "added.txt missing from diff list");
    if (e != NULL) {
        CHECK(e->old_side.kind == SG_DIFF_SIDE_ABSENT, "added.txt old side should be ABSENT");
        CHECK(e->new_side.kind == SG_DIFF_SIDE_BLOB, "added.txt new side should be BLOB");
    }

    e = find_entry(&list, "deleted.txt");
    CHECK(e != NULL, "deleted.txt missing from diff list");
    if (e != NULL) {
        CHECK(e->old_side.kind == SG_DIFF_SIDE_BLOB, "deleted.txt old side should be BLOB");
        CHECK(e->new_side.kind == SG_DIFF_SIDE_ABSENT, "deleted.txt new side should be ABSENT");
    }

    e = find_entry(&list, "changed.txt");
    CHECK(e != NULL, "changed.txt missing from diff list");
    if (e != NULL) {
        CHECK(e->old_side.kind == SG_DIFF_SIDE_BLOB && e->new_side.kind == SG_DIFF_SIDE_BLOB,
             "changed.txt should have BLOB on both sides");
        CHECK(memcmp(e->old_side.id, e->new_side.id, SG_SHA1_RAW_LEN) != 0,
             "changed.txt should have different blob ids on the two sides");
    }

    CHECK(find_entry(&list, "same.txt") == NULL,
         "same.txt has identical content on both sides and must not appear in the diff list");

    sg_diff_list_free(&list);
    free(git_dir);
}

/* 2. tree vs tree: mode-only change (same blob id, different mode) still
      shows up. */
static void test_trees_mode_only_change(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id[SG_SHA1_RAW_LEN];
    unsigned char old_tree[SG_SHA1_RAW_LEN];
    unsigned char new_tree[SG_SHA1_RAW_LEN];
    sg_flat_entry entries[1];
    sg_diff_list list;
    const sg_diff_entry *e;

    blob(git_dir, "script\n", id);

    entries[0].path = strdup("run.sh");
    entries[0].mode = 0100644;
    memcpy(entries[0].sha1, id, SG_SHA1_RAW_LEN);
    CHECK(sg_tree_build(git_dir, entries, 1, old_tree) == 0, "old tree build failed");

    entries[0].mode = 0100755;
    CHECK(sg_tree_build(git_dir, entries, 1, new_tree) == 0, "new tree build failed");
    free(entries[0].path);

    CHECK(sg_diff_trees(git_dir, old_tree, new_tree, &list, NULL) == 0, "sg_diff_trees failed");
    CHECK(list.count == 1, "expected exactly the mode-only change, got %zu entries", list.count);
    e = find_entry(&list, "run.sh");
    CHECK(e != NULL, "run.sh missing from diff list");
    if (e != NULL) {
        CHECK(e->old_side.kind == SG_DIFF_SIDE_BLOB && e->new_side.kind == SG_DIFF_SIDE_BLOB,
             "run.sh should have BLOB on both sides");
        CHECK(memcmp(e->old_side.id, e->new_side.id, SG_SHA1_RAW_LEN) == 0,
             "run.sh's blob id should be identical on both sides");
        CHECK(e->old_side.mode == 0100644 && e->new_side.mode == 0100755,
             "run.sh's modes should differ (0%o vs 0%o)", e->old_side.mode, e->new_side.mode);
    }

    sg_diff_list_free(&list);
    free(git_dir);
}

/* 3. NULL old_tree means the empty tree: everything in new_tree is an
      addition; both NULL means an empty diff list. */
static void test_null_tree_is_empty_tree(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char new_tree[SG_SHA1_RAW_LEN];
    tree_spec specs[] = {
        {"a.txt", 0100644, "aaa\n"},
        {"b.txt", 0100644, "bbb\n"},
    };
    sg_diff_list list;

    build_tree(git_dir, specs, 2, new_tree);

    CHECK(sg_diff_trees(git_dir, NULL, new_tree, &list, NULL) == 0, "sg_diff_trees with NULL old_tree failed");
    CHECK(list.count == 2, "NULL old_tree should make everything in new_tree an addition, got %zu",
         list.count);
    if (list.count == 2) {
        CHECK(list.entries[0].old_side.kind == SG_DIFF_SIDE_ABSENT &&
                 list.entries[1].old_side.kind == SG_DIFF_SIDE_ABSENT,
             "both entries should have an ABSENT old side");
    }
    sg_diff_list_free(&list);

    CHECK(sg_diff_trees(git_dir, NULL, NULL, &list, NULL) == 0, "sg_diff_trees with both NULL failed");
    CHECK(list.count == 0, "both trees NULL should produce an empty diff list, got %zu", list.count);
    sg_diff_list_free(&list);

    free(git_dir);
}

/* 4. Output is sorted by path bytes, not by tree traversal order. "a.txt",
      "a/b.txt" and "b.txt" sort differently as plain strings (where '.' < '/')
      than a real tree object would traverse them (directories and files are
      siblings under the root, ordered by name -- "a" the directory entry vs
      "a.txt" the file entry), so this fixture can tell the two orderings
      apart. */
static void test_output_sorted_by_path(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char new_tree[SG_SHA1_RAW_LEN];
    tree_spec specs[] = {
        {"a.txt", 0100644, "top-level a\n"},
        {"a/b.txt", 0100644, "nested a/b\n"},
        {"b.txt", 0100644, "top-level b\n"},
    };
    sg_diff_list list;
    size_t i;

    build_tree(git_dir, specs, 3, new_tree);

    CHECK(sg_diff_trees(git_dir, NULL, new_tree, &list, NULL) == 0, "sg_diff_trees failed");
    CHECK(list.count == 3, "expected 3 entries, got %zu", list.count);
    if (list.count == 3) {
        CHECK(strcmp(list.entries[0].path, "a.txt") == 0, "entry 0 should be a.txt, got %s",
             list.entries[0].path);
        CHECK(strcmp(list.entries[1].path, "a/b.txt") == 0, "entry 1 should be a/b.txt, got %s",
             list.entries[1].path);
        CHECK(strcmp(list.entries[2].path, "b.txt") == 0, "entry 2 should be b.txt, got %s",
             list.entries[2].path);
    }
    for (i = 1; i < list.count; i++) {
        CHECK(strcmp(list.entries[i - 1].path, list.entries[i].path) < 0,
             "output must be strictly increasing by path bytes: %s then %s",
             list.entries[i - 1].path, list.entries[i].path);
    }

    sg_diff_list_free(&list);
    free(git_dir);
}

/* 5. sg_diff_tree_index skips stage 1/2/3 entries -- an unresolved conflict
      has no single staged blob to diff against. */
static void test_tree_index_skips_unmerged_stages(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char old_tree[SG_SHA1_RAW_LEN];
    unsigned char id1[SG_SHA1_RAW_LEN], id2[SG_SHA1_RAW_LEN], id3[SG_SHA1_RAW_LEN];
    tree_spec specs[] = {
        {"conflict.txt", 0100644, "base content\n"},
    };
    sg_index idx;
    sg_index_entry e;
    sg_diff_list list;

    build_tree(git_dir, specs, 1, old_tree);

    blob(git_dir, "base\n", id1);
    blob(git_dir, "ours\n", id2);
    blob(git_dir, "theirs\n", id3);

    memset(&idx, 0, sizeof(idx));
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    e.path = (char *)"conflict.txt";
    e.stage = 1;
    memcpy(e.sha1, id1, SG_SHA1_RAW_LEN);
    CHECK(sg_index_upsert(&idx, &e) == 0, "upsert stage 1");
    e.stage = 2;
    memcpy(e.sha1, id2, SG_SHA1_RAW_LEN);
    CHECK(sg_index_upsert(&idx, &e) == 0, "upsert stage 2");
    e.stage = 3;
    memcpy(e.sha1, id3, SG_SHA1_RAW_LEN);
    CHECK(sg_index_upsert(&idx, &e) == 0, "upsert stage 3");

    CHECK(sg_diff_tree_index(git_dir, old_tree, &idx, &list, NULL) == 0, "sg_diff_tree_index failed");
    /* conflict.txt has no stage-0 entry, so from this function's point of
       view the tree's path simply has nothing to compare against and is
       reported as a deletion -- it must not appear as a stage 1/2/3 blob. */
    CHECK(list.count == 1, "expected exactly one entry (conflict.txt as a deletion), got %zu",
         list.count);
    if (list.count == 1) {
        CHECK(strcmp(list.entries[0].path, "conflict.txt") == 0, "unexpected path %s",
             list.entries[0].path);
        CHECK(list.entries[0].new_side.kind == SG_DIFF_SIDE_ABSENT,
             "conflict.txt's new side must be ABSENT: no stage-0 entry exists to diff against");
    }

    sg_diff_list_free(&list);
    sg_index_free(&idx);
    free(git_dir);
}

/* 6. sg_diff_index_workdir: deleted-on-disk -> ABSENT new side; modified
      content -> both sides present; untouched -> absent from the list. */
static void test_index_workdir(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char id_deleted[SG_SHA1_RAW_LEN];
    unsigned char id_modified[SG_SHA1_RAW_LEN];
    unsigned char id_unchanged[SG_SHA1_RAW_LEN];
    sg_index idx;
    sg_diff_list list;
    const sg_diff_entry *e;

    blob(git_dir, "will be deleted\n", id_deleted);
    blob(git_dir, "old content\n", id_modified);
    blob(git_dir, "same content\n", id_unchanged);

    write_workdir_file(repo_root, "deleted.txt", "will be deleted\n");
    write_workdir_file(repo_root, "modified.txt", "new content\n");
    write_workdir_file(repo_root, "unchanged.txt", "same content\n");
    remove_workdir_file(repo_root, "deleted.txt");

    memset(&idx, 0, sizeof(idx));
    index_upsert_blob(&idx, "deleted.txt", 0100644, id_deleted);
    index_upsert_blob(&idx, "modified.txt", 0100644, id_modified);
    index_upsert_blob(&idx, "unchanged.txt", 0100644, id_unchanged);

    CHECK(sg_diff_index_workdir(git_dir, repo_root, &idx, &list) == 0,
         "sg_diff_index_workdir failed");
    CHECK(list.count == 2, "expected 2 changed paths (deleted + modified), got %zu", list.count);

    e = find_entry(&list, "deleted.txt");
    CHECK(e != NULL, "deleted.txt missing from diff list");
    if (e != NULL) {
        CHECK(e->old_side.kind == SG_DIFF_SIDE_BLOB, "deleted.txt old side should be BLOB");
        CHECK(e->new_side.kind == SG_DIFF_SIDE_ABSENT,
             "deleted.txt new side should be ABSENT: the file is gone from the working tree");
    }

    e = find_entry(&list, "modified.txt");
    CHECK(e != NULL, "modified.txt missing from diff list");
    if (e != NULL) {
        CHECK(e->old_side.kind == SG_DIFF_SIDE_BLOB, "modified.txt old side should be BLOB");
        CHECK(e->new_side.kind == SG_DIFF_SIDE_WORKDIR, "modified.txt new side should be WORKDIR");
    }

    CHECK(find_entry(&list, "unchanged.txt") == NULL,
         "unchanged.txt's content did not change and must not appear in the diff list");

    sg_diff_list_free(&list);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* 7. sg_diff_tree_workdir: a path present in the tree but absent from the
      index is reported as a deletion even though the file is still
      physically on disk (measured against git 2.55.0's `git rm --cached`
      behavior, see sg/diff.h). */
static void test_tree_workdir_rm_cached_is_deletion(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char old_tree[SG_SHA1_RAW_LEN];
    tree_spec specs[] = {
        {"tracked.txt", 0100644, "tracked content\n"},
    };
    sg_index idx;
    sg_diff_list list;
    const sg_diff_entry *e;

    build_tree(git_dir, specs, 1, old_tree);
    /* The file is still on disk, unmodified, but the index (as if by
       `sg rm --cached tracked.txt`) no longer has an entry for it. */
    write_workdir_file(repo_root, "tracked.txt", "tracked content\n");

    memset(&idx, 0, sizeof(idx));

    CHECK(sg_diff_tree_workdir(git_dir, repo_root, old_tree, &idx, &list, NULL) == 0,
         "sg_diff_tree_workdir failed");
    CHECK(list.count == 1,
         "expected tracked.txt to be reported despite being unchanged on disk, got %zu entries",
         list.count);
    e = find_entry(&list, "tracked.txt");
    CHECK(e != NULL, "tracked.txt missing from diff list");
    if (e != NULL) {
        CHECK(e->old_side.kind == SG_DIFF_SIDE_BLOB, "tracked.txt old side should be BLOB");
        CHECK(e->new_side.kind == SG_DIFF_SIDE_ABSENT,
             "tracked.txt new side must be ABSENT: not in the index means deleted, regardless of "
             "what's on disk");
    }

    sg_diff_list_free(&list);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* 8. sg_diff_side_read: an ABSENT side yields (NULL, 0) and success. */
static void test_side_read_absent(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_diff_side side;
    unsigned char *data = (unsigned char *)0x1; /* poison, must be overwritten */
    size_t len = 12345;

    memset(&side, 0, sizeof(side));
    side.kind = SG_DIFF_SIDE_ABSENT;

    CHECK(sg_diff_side_read(git_dir, repo_root, "whatever.txt", &side, &data, &len, NULL) == 0,
         "sg_diff_side_read on an ABSENT side should succeed");
    CHECK(data == NULL, "ABSENT side should yield NULL data");
    CHECK(len == 0, "ABSENT side should yield len 0, got %zu", len);

    free(repo_root);
    free(git_dir);
}

int main(void)
{
    test_trees_add_delete_modify();
    test_trees_mode_only_change();
    test_null_tree_is_empty_tree();
    test_output_sorted_by_path();
    test_tree_index_skips_unmerged_stages();
    test_index_workdir();
    test_tree_workdir_rm_cached_is_deletion();
    test_side_read_absent();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all diff_list tests passed\n");
    return 0;
}
