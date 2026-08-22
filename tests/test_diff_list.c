#include "sg/diff.h"

#include "sg/chunk.h"
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

static void write_workdir_file_bytes(const char *repo_root, const char *rel,
                                     const unsigned char *content, size_t len)
{
    char abspath[4096];

    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, rel);
    CHECK(sg_write_file_mkdirs(abspath, content, len, 0644) == 0,
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

static void index_upsert_stage(sg_index *idx, const char *path, unsigned int stage,
                               unsigned int mode, const unsigned char id[SG_SHA1_RAW_LEN])
{
    sg_index_entry e;

    memset(&e, 0, sizeof(e));
    e.mode = mode;
    e.stage = stage;
    memcpy(e.sha1, id, SG_SHA1_RAW_LEN);
    e.path = (char *)path;
    CHECK(sg_index_upsert(idx, &e) == 0, "upsert %s (stage %u) into index", path, stage);
}

/* Finds the entry for path in list, or NULL. Several tests below need every
   row for a path (an unresolved conflict produces up to two), so this
   returns the n'th (0-based) match rather than assuming at most one. */
static const sg_diff_entry *find_entry_nth(const sg_diff_list *list, const char *path, size_t n)
{
    size_t i;
    size_t seen = 0;

    for (i = 0; i < list->count; i++) {
        if (strcmp(list->entries[i].path, path) == 0) {
            if (seen == n)
                return &list->entries[i];
            seen++;
        }
    }
    return NULL;
}

static const sg_diff_entry *find_entry(const sg_diff_list *list, const char *path)
{
    return find_entry_nth(list, path, 0);
}

/* Counts how many rows in list carry path. */
static size_t count_entries(const sg_diff_list *list, const char *path)
{
    size_t i;
    size_t n = 0;

    for (i = 0; i < list->count; i++) {
        if (strcmp(list->entries[i].path, path) == 0)
            n++;
    }
    return n;
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

/* 4. Output is sorted by path bytes. A tree object's own entry order and a
      plain byte-wise sort of full paths can never actually disagree -- git's
      tree-entry comparison virtually appends '/' when comparing a directory
      name, which exists precisely so a depth-first tree walk always yields
      full paths in byte order. So this fixture cannot tell "did the merge-
      join preserve tree order" apart from "did it byte-sort" -- no
      tree-based fixture can, by construction. What it DOES pin down is that
      the merge-join itself doesn't scramble the order sg_tree_flatten hands
      it (e.g. by inserting the tree-only/index-only branches out of step
      with the increasing cursor), which the strictly-increasing loop below
      checks directly. */
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

/* 5. sg_diff_tree_index: ordinary stage-0 entries produce addition/deletion/
      modification rows exactly like sg_diff_trees does, and an unchanged
      path is left out -- this exercises cmp==0/cmp<0/cmp>0 against REAL
      stage-0 index entries (the old regression here used an index holding
      only stage 1/2/3 entries, so none of these branches were ever taken
      against a stage-0 entry). */
static void test_tree_index_stage0_add_delete_modify(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char old_tree[SG_SHA1_RAW_LEN];
    unsigned char id_added[SG_SHA1_RAW_LEN];
    unsigned char id_changed_new[SG_SHA1_RAW_LEN];
    unsigned char id_same[SG_SHA1_RAW_LEN];
    tree_spec old_specs[] = {
        {"changed.txt", 0100644, "before\n"},
        {"deleted.txt", 0100644, "gone soon\n"},
        {"same.txt", 0100644, "unchanged\n"},
    };
    sg_index idx;
    sg_diff_list list;
    const sg_diff_entry *e;

    build_tree(git_dir, old_specs, 3, old_tree);

    blob(git_dir, "brand new\n", id_added);
    blob(git_dir, "after\n", id_changed_new);
    blob(git_dir, "unchanged\n", id_same);

    memset(&idx, 0, sizeof(idx));
    index_upsert_blob(&idx, "added.txt", 0100644, id_added);
    index_upsert_blob(&idx, "changed.txt", 0100644, id_changed_new);
    index_upsert_blob(&idx, "same.txt", 0100644, id_same);

    CHECK(sg_diff_tree_index(git_dir, old_tree, &idx, &list, NULL) == 0, "sg_diff_tree_index failed");
    CHECK(list.count == 3, "expected 3 changed paths (add/delete/modify), got %zu", list.count);

    e = find_entry(&list, "added.txt");
    CHECK(e != NULL, "added.txt missing from diff list");
    if (e != NULL) {
        CHECK(e->old_side.kind == SG_DIFF_SIDE_ABSENT, "added.txt old side should be ABSENT");
        CHECK(e->new_side.kind == SG_DIFF_SIDE_BLOB, "added.txt new side should be BLOB");
        CHECK(!e->unmerged, "added.txt is an ordinary stage-0 row, not a conflict");
    }

    e = find_entry(&list, "deleted.txt");
    CHECK(e != NULL, "deleted.txt missing from diff list");
    if (e != NULL) {
        CHECK(e->old_side.kind == SG_DIFF_SIDE_BLOB, "deleted.txt old side should be BLOB");
        CHECK(e->new_side.kind == SG_DIFF_SIDE_ABSENT, "deleted.txt new side should be ABSENT");
        CHECK(!e->unmerged, "deleted.txt is an ordinary stage-0 row, not a conflict");
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
         "same.txt's staged content matches HEAD and must not appear in the diff list");

    sg_diff_list_free(&list);
    sg_index_free(&idx);
    free(git_dir);
}

/* 6. sg_diff_tree_index: an unresolved conflict (stage 1/2/3, no stage 0)
      yields exactly one row with both sides ABSENT and unmerged set --
      measured against git 2.55.0: `git diff --cached --name-status` prints
      "U conflict.txt", not a deletion. Covers both a conflicted path that
      also exists in HEAD (the common case) and one that does not (a
      conflicting add with no prior history), since sg_diff_tree_index's
      merge-join must recognize an unmerged path by its own index group
      rather than only when it happens to line up with a tree entry. */
static void test_tree_index_unmerged_is_single_row(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char old_tree[SG_SHA1_RAW_LEN];
    unsigned char id1[SG_SHA1_RAW_LEN], id2[SG_SHA1_RAW_LEN], id3[SG_SHA1_RAW_LEN];
    tree_spec specs[] = {
        {"conflict.txt", 0100644, "base content\n"},
    };
    sg_index idx;
    sg_diff_list list;
    const sg_diff_entry *e;

    build_tree(git_dir, specs, 1, old_tree);

    blob(git_dir, "base\n", id1);
    blob(git_dir, "ours\n", id2);
    blob(git_dir, "theirs\n", id3);

    memset(&idx, 0, sizeof(idx));
    index_upsert_stage(&idx, "conflict.txt", 1, 0100644, id1);
    index_upsert_stage(&idx, "conflict.txt", 2, 0100644, id2);
    index_upsert_stage(&idx, "conflict.txt", 3, 0100644, id3);
    /* Conflicting add: no HEAD entry at all for this path. */
    index_upsert_stage(&idx, "new_conflict.txt", 2, 0100644, id2);
    index_upsert_stage(&idx, "new_conflict.txt", 3, 0100644, id3);

    CHECK(sg_diff_tree_index(git_dir, old_tree, &idx, &list, NULL) == 0, "sg_diff_tree_index failed");
    CHECK(list.count == 2, "expected exactly one row per conflicted path, got %zu", list.count);

    e = find_entry(&list, "conflict.txt");
    CHECK(e != NULL, "conflict.txt missing from diff list");
    if (e != NULL) {
        CHECK(e->unmerged, "conflict.txt should be reported as unmerged, not a deletion");
        CHECK(e->old_side.kind == SG_DIFF_SIDE_ABSENT, "conflict.txt's old side must be ABSENT");
        CHECK(e->new_side.kind == SG_DIFF_SIDE_ABSENT, "conflict.txt's new side must be ABSENT");
    }
    CHECK(count_entries(&list, "conflict.txt") == 1,
         "conflict.txt must produce exactly one row, not one per stage");

    e = find_entry(&list, "new_conflict.txt");
    CHECK(e != NULL, "new_conflict.txt (conflicting add, no HEAD entry) missing from diff list");
    if (e != NULL)
        CHECK(e->unmerged, "new_conflict.txt should be reported as unmerged");

    sg_diff_list_free(&list);
    sg_index_free(&idx);
    free(git_dir);
}

/* 7. sg_diff_index_workdir: deleted-on-disk -> ABSENT new side; modified
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

/* Phase 26: a WORKDIR side now carries a real mode and id (previously always
   0/unset -- see sg/diff.h). This is what makes a bare chmod, with content
   unchanged, show up at all: mode==0 on the WORKDIR side used to make
   blob_sides_differ() skip the mode comparison entirely, so
   append_index_entry_vs_workdir's final memcmp (content only) found nothing
   and the path never made it into the list. */
static void test_index_workdir_mode_only_change(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char id[SG_SHA1_RAW_LEN];
    unsigned char expect_content_id[SG_SHA1_RAW_LEN];
    char abspath[4096];
    sg_index idx;
    sg_diff_list list;
    const sg_diff_entry *e;

    blob(git_dir, "same bytes\n", id);
    sg_object_hash(SG_OBJ_BLOB, "same bytes\n", strlen("same bytes\n"), expect_content_id);

    write_workdir_file(repo_root, "exe.sh", "same bytes\n");
    snprintf(abspath, sizeof(abspath), "%s/exe.sh", repo_root);
    CHECK(chmod(abspath, 0755) == 0, "chmod +x failed");

    memset(&idx, 0, sizeof(idx));
    index_upsert_blob(&idx, "exe.sh", 0100644, id);

    CHECK(sg_diff_index_workdir(git_dir, repo_root, &idx, &list) == 0,
         "sg_diff_index_workdir failed");
    CHECK(list.count == 1, "expected exe.sh's mode-only change to appear, got %zu rows", list.count);

    e = find_entry(&list, "exe.sh");
    CHECK(e != NULL, "exe.sh missing from diff list");
    if (e != NULL) {
        CHECK(e->old_side.mode == 0100644, "exe.sh old mode should be 100644, got 0%o",
             e->old_side.mode);
        CHECK(e->new_side.kind == SG_DIFF_SIDE_WORKDIR, "exe.sh new side should be WORKDIR");
        CHECK(e->new_side.mode == 0100755, "exe.sh new (workdir) mode should be 100755, got 0%o",
             e->new_side.mode);
        CHECK(memcmp(e->new_side.id, expect_content_id, SG_SHA1_RAW_LEN) == 0,
             "exe.sh new (workdir) side's id should be the file's own content hash");
    }

    sg_diff_list_free(&list);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* A WORKDIR side's id is the file's own content hash, not left zeroed, when
   content (not just mode) actually changed. */
static void test_index_workdir_side_carries_content_id(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char id_old[SG_SHA1_RAW_LEN];
    unsigned char expect_new_id[SG_SHA1_RAW_LEN];
    sg_index idx;
    sg_diff_list list;
    const sg_diff_entry *e;

    blob(git_dir, "old content\n", id_old);
    sg_object_hash(SG_OBJ_BLOB, "new content\n", strlen("new content\n"), expect_new_id);

    write_workdir_file(repo_root, "f.txt", "new content\n");

    memset(&idx, 0, sizeof(idx));
    index_upsert_blob(&idx, "f.txt", 0100644, id_old);

    CHECK(sg_diff_index_workdir(git_dir, repo_root, &idx, &list) == 0,
         "sg_diff_index_workdir failed");

    e = find_entry(&list, "f.txt");
    CHECK(e != NULL, "f.txt missing from diff list");
    if (e != NULL) {
        CHECK(e->new_side.kind == SG_DIFF_SIDE_WORKDIR, "f.txt new side should be WORKDIR");
        CHECK(e->new_side.mode == 0100644, "f.txt new (workdir) mode should be 100644, got 0%o",
             e->new_side.mode);
        CHECK(memcmp(e->new_side.id, expect_new_id, SG_SHA1_RAW_LEN) == 0,
             "f.txt new (workdir) side's id should be the new content's hash, not left zeroed");
    }

    sg_diff_list_free(&list);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* Phase 26: sg_diff_tree_workdir's addition branch (index-only path present
   on disk) used to build a WORKDIR side with no id/mode at all -- confirm it
   now fills both. */
static void test_tree_workdir_addition_side_has_id_and_mode(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char old_tree[SG_SHA1_RAW_LEN];
    unsigned char id[SG_SHA1_RAW_LEN];
    unsigned char expect_id[SG_SHA1_RAW_LEN];
    char abspath[4096];
    tree_spec specs[] = {
        {"unrelated.txt", 0100644, "unrelated\n"},
    };
    sg_index idx;
    sg_diff_list list;
    const sg_diff_entry *e;

    build_tree(git_dir, specs, 1, old_tree);
    blob(git_dir, "brand new\n", id);
    sg_object_hash(SG_OBJ_BLOB, "brand new\n", strlen("brand new\n"), expect_id);

    write_workdir_file(repo_root, "added.sh", "brand new\n");
    snprintf(abspath, sizeof(abspath), "%s/added.sh", repo_root);
    CHECK(chmod(abspath, 0755) == 0, "chmod +x failed");

    memset(&idx, 0, sizeof(idx));
    index_upsert_blob(&idx, "added.sh", 0100755, id);

    CHECK(sg_diff_tree_workdir(git_dir, repo_root, old_tree, &idx, &list, NULL) == 0,
         "sg_diff_tree_workdir failed");

    e = find_entry(&list, "added.sh");
    CHECK(e != NULL, "added.sh missing from diff list");
    if (e != NULL) {
        CHECK(e->new_side.kind == SG_DIFF_SIDE_WORKDIR, "added.sh new side should be WORKDIR");
        CHECK(e->new_side.mode == 0100755, "added.sh new (workdir) mode should be 100755, got 0%o",
             e->new_side.mode);
        CHECK(memcmp(e->new_side.id, expect_id, SG_SHA1_RAW_LEN) == 0,
             "added.sh new (workdir) side's id should be its content hash");
    }

    sg_diff_list_free(&list);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* 8. sg_diff_index_workdir: an unresolved conflict whose stage-2 (ours) blob
      differs from the working tree's actual bytes produces TWO rows, in
      order: the fixed unmerged row, then an ordinary row comparing stage 2
      against the working tree -- measured against git 2.55.0 printing "U
      conflict.txt" followed by "M conflict.txt" for the same path. */
static void test_index_workdir_unmerged_two_rows(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char id1[SG_SHA1_RAW_LEN], id2[SG_SHA1_RAW_LEN], id3[SG_SHA1_RAW_LEN];
    sg_index idx;
    sg_diff_list list;
    const sg_diff_entry *first;
    const sg_diff_entry *second;

    blob(git_dir, "base\n", id1);
    blob(git_dir, "ours\n", id2);
    blob(git_dir, "theirs\n", id3);

    /* The file on disk still carries conflict markers -- neither stage 2
       (ours) nor stage 3 (theirs), so the second row must appear. */
    write_workdir_file(repo_root, "conflict.txt",
                       "<<<<<<< ours\nours\n=======\ntheirs\n>>>>>>> theirs\n");

    memset(&idx, 0, sizeof(idx));
    index_upsert_stage(&idx, "conflict.txt", 1, 0100644, id1);
    index_upsert_stage(&idx, "conflict.txt", 2, 0100644, id2);
    index_upsert_stage(&idx, "conflict.txt", 3, 0100644, id3);

    CHECK(sg_diff_index_workdir(git_dir, repo_root, &idx, &list) == 0,
         "sg_diff_index_workdir failed");
    CHECK(list.count == 2, "expected the unmerged row plus the stage-2-vs-workdir row, got %zu",
         list.count);
    CHECK(count_entries(&list, "conflict.txt") == 2, "both rows should be for conflict.txt");

    first = find_entry_nth(&list, "conflict.txt", 0);
    CHECK(first != NULL, "first conflict.txt row missing");
    if (first != NULL) {
        CHECK(first->unmerged, "the first row must be the unmerged row");
        CHECK(first->old_side.kind == SG_DIFF_SIDE_ABSENT && first->new_side.kind == SG_DIFF_SIDE_ABSENT,
             "the unmerged row's sides must both be ABSENT");
    }

    second = find_entry_nth(&list, "conflict.txt", 1);
    CHECK(second != NULL, "second conflict.txt row missing");
    if (second != NULL) {
        CHECK(!second->unmerged, "the second row is an ordinary modification, not unmerged");
        CHECK(second->old_side.kind == SG_DIFF_SIDE_BLOB, "the second row's old side should be BLOB");
        CHECK(memcmp(second->old_side.id, id2, SG_SHA1_RAW_LEN) == 0,
             "the second row's old side must be stage 2 (ours), not stage 1 or 3");
        CHECK(second->new_side.kind == SG_DIFF_SIDE_WORKDIR, "the second row's new side should be WORKDIR");
    }

    sg_diff_list_free(&list);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* 9. sg_diff_index_workdir: the second row is suppressed when stage 2's
      content is byte-identical to what is actually on disk (resolved in
      favor of "ours" but not yet staged), and likewise suppressed when there
      is no stage 2 at all (e.g. only stage 1/3, an add/add or delete/modify
      conflict without an "ours" blob) -- in both cases only the fixed
      unmerged row is produced. */
static void test_index_workdir_unmerged_no_second_row(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char id1[SG_SHA1_RAW_LEN], id2[SG_SHA1_RAW_LEN], id3[SG_SHA1_RAW_LEN];
    sg_index idx;
    sg_diff_list list;

    blob(git_dir, "base\n", id1);
    blob(git_dir, "ours\n", id2);
    blob(git_dir, "theirs\n", id3);

    /* resolved.txt: the working tree already matches stage 2 exactly. */
    write_workdir_file(repo_root, "resolved.txt", "ours\n");
    /* no_ours.txt: only stage 1 and 3 -- no "ours" blob to compare against. */
    write_workdir_file(repo_root, "no_ours.txt", "whatever the user left here\n");

    memset(&idx, 0, sizeof(idx));
    index_upsert_stage(&idx, "resolved.txt", 1, 0100644, id1);
    index_upsert_stage(&idx, "resolved.txt", 2, 0100644, id2);
    index_upsert_stage(&idx, "resolved.txt", 3, 0100644, id3);
    index_upsert_stage(&idx, "no_ours.txt", 1, 0100644, id1);
    index_upsert_stage(&idx, "no_ours.txt", 3, 0100644, id3);

    CHECK(sg_diff_index_workdir(git_dir, repo_root, &idx, &list) == 0,
         "sg_diff_index_workdir failed");
    CHECK(list.count == 2, "expected exactly one unmerged row per path, got %zu", list.count);
    CHECK(count_entries(&list, "resolved.txt") == 1,
         "resolved.txt's disk content matches stage 2 -- the second row must not appear");
    CHECK(count_entries(&list, "no_ours.txt") == 1,
         "no_ours.txt has no stage 2 -- there is nothing to produce a second row from");

    sg_diff_list_free(&list);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* 10. sg_diff_tree_workdir: a path present in the tree but absent from the
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

/* 11. sg_diff_tree_workdir: the cmp==0 branch (path in both tree and index)
       compares the TREE's blob against the actual working-tree bytes, and
       skips the path when they match; the cmp>0 branch (path in index only)
       sources its content from the working tree when the file exists, and
       reports nothing when it doesn't. */
static void test_tree_workdir_content_compare_and_addition(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char old_tree[SG_SHA1_RAW_LEN];
    unsigned char id_unchanged[SG_SHA1_RAW_LEN];
    tree_spec specs[] = {
        {"changed.txt", 0100644, "before\n"},
        {"unchanged.txt", 0100644, "steady state\n"},
    };
    sg_index idx;
    sg_diff_list list;
    const sg_diff_entry *e;

    build_tree(git_dir, specs, 2, old_tree);
    blob(git_dir, "brand new\n", id_unchanged); /* reused below just to get an id */

    write_workdir_file(repo_root, "changed.txt", "after\n");
    write_workdir_file(repo_root, "unchanged.txt", "steady state\n");
    write_workdir_file(repo_root, "added.txt", "brand new\n");
    /* staged.txt is in the index but was deleted from disk after staging;
       it must not appear at all (both sides absent). */

    memset(&idx, 0, sizeof(idx));
    index_upsert_blob(&idx, "added.txt", 0100644, id_unchanged);
    index_upsert_blob(&idx, "changed.txt", 0100644, id_unchanged); /* content irrelevant here */
    index_upsert_blob(&idx, "unchanged.txt", 0100644, id_unchanged);
    index_upsert_blob(&idx, "staged.txt", 0100644, id_unchanged);

    CHECK(sg_diff_tree_workdir(git_dir, repo_root, old_tree, &idx, &list, NULL) == 0,
         "sg_diff_tree_workdir failed");

    e = find_entry(&list, "changed.txt");
    CHECK(e != NULL, "changed.txt missing from diff list");
    if (e != NULL) {
        CHECK(e->old_side.kind == SG_DIFF_SIDE_BLOB, "changed.txt's old side should be the tree's blob");
        CHECK(e->new_side.kind == SG_DIFF_SIDE_WORKDIR, "changed.txt's new side should be WORKDIR");
    }

    CHECK(find_entry(&list, "unchanged.txt") == NULL,
         "unchanged.txt's working-tree bytes match the tree's blob and must not appear");

    e = find_entry(&list, "added.txt");
    CHECK(e != NULL, "added.txt (index-only, present on disk) missing from diff list");
    if (e != NULL) {
        CHECK(e->old_side.kind == SG_DIFF_SIDE_ABSENT, "added.txt's old side should be ABSENT");
        CHECK(e->new_side.kind == SG_DIFF_SIDE_WORKDIR,
             "added.txt's content should be sourced from the working tree");
    }

    CHECK(find_entry(&list, "staged.txt") == NULL,
         "staged.txt is index-only AND missing from disk -- both sides are absent, so it must not "
         "appear");

    sg_diff_list_free(&list);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* 12. sg_diff_tree_workdir: an unresolved conflict is NOT special here --
       the index only decides membership, so a conflicted path is compared
       as an ordinary tree-vs-workdir row (measured: `git diff HEAD
       --name-status` prints "M", not "U", for a conflicted path). */
static void test_tree_workdir_unmerged_not_special(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char old_tree[SG_SHA1_RAW_LEN];
    unsigned char id1[SG_SHA1_RAW_LEN], id2[SG_SHA1_RAW_LEN], id3[SG_SHA1_RAW_LEN];
    tree_spec specs[] = {
        {"conflict.txt", 0100644, "base content\n"},
        {"resolved.txt", 0100644, "base content\n"},
    };
    sg_index idx;
    sg_diff_list list;
    const sg_diff_entry *e;

    build_tree(git_dir, specs, 2, old_tree);
    blob(git_dir, "base\n", id1);
    blob(git_dir, "ours\n", id2);
    blob(git_dir, "theirs\n", id3);

    write_workdir_file(repo_root, "conflict.txt", "conflict markers here\n");
    /* resolved.txt: still has conflict stages recorded, but the user has
       already put the file back to exactly what HEAD had -- must not
       appear at all, unmerged or otherwise. */
    write_workdir_file(repo_root, "resolved.txt", "base content\n");

    memset(&idx, 0, sizeof(idx));
    index_upsert_stage(&idx, "conflict.txt", 1, 0100644, id1);
    index_upsert_stage(&idx, "conflict.txt", 2, 0100644, id2);
    index_upsert_stage(&idx, "conflict.txt", 3, 0100644, id3);
    index_upsert_stage(&idx, "resolved.txt", 1, 0100644, id1);
    index_upsert_stage(&idx, "resolved.txt", 2, 0100644, id2);
    index_upsert_stage(&idx, "resolved.txt", 3, 0100644, id3);

    CHECK(sg_diff_tree_workdir(git_dir, repo_root, old_tree, &idx, &list, NULL) == 0,
         "sg_diff_tree_workdir failed");
    CHECK(list.count == 1, "expected exactly conflict.txt's row, got %zu", list.count);

    e = find_entry(&list, "conflict.txt");
    CHECK(e != NULL, "conflict.txt missing from diff list");
    if (e != NULL) {
        CHECK(!e->unmerged, "conflict.txt must be an ordinary row here, not unmerged");
        CHECK(e->old_side.kind == SG_DIFF_SIDE_BLOB, "conflict.txt's old side should be the tree's blob");
        CHECK(e->new_side.kind == SG_DIFF_SIDE_WORKDIR, "conflict.txt's new side should be WORKDIR");
    }

    CHECK(find_entry(&list, "resolved.txt") == NULL,
         "resolved.txt's disk content matches the tree and must not appear, conflict stages or not");

    sg_diff_list_free(&list);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* 13. sg_diff_side_read: an ABSENT side yields (NULL, 0) and success. */
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

/* 14. sg_diff_side_read: a BLOB side reads the object's content via
       sg_chunk_read_blob, and a WORKDIR side reads the file at
       <repo_root>/<path>. */
static void test_side_read_blob_and_workdir(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char id[SG_SHA1_RAW_LEN];
    sg_diff_side side;
    unsigned char *data;
    size_t len;

    blob(git_dir, "blob side content\n", id);
    memset(&side, 0, sizeof(side));
    side.kind = SG_DIFF_SIDE_BLOB;
    memcpy(side.id, id, SG_SHA1_RAW_LEN);

    data = NULL;
    len = 0;
    CHECK(sg_diff_side_read(git_dir, repo_root, "blob.txt", &side, &data, &len, NULL) == 0,
         "sg_diff_side_read on a BLOB side failed");
    CHECK(data != NULL && len == strlen("blob side content\n") &&
             memcmp(data, "blob side content\n", len) == 0,
         "BLOB side should read back the blob's exact content");
    free(data);

    write_workdir_file(repo_root, "wd.txt", "workdir side content\n");
    memset(&side, 0, sizeof(side));
    side.kind = SG_DIFF_SIDE_WORKDIR;

    data = NULL;
    len = 0;
    CHECK(sg_diff_side_read(git_dir, repo_root, "wd.txt", &side, &data, &len, NULL) == 0,
         "sg_diff_side_read on a WORKDIR side failed");
    CHECK(data != NULL && len == strlen("workdir side content\n") &&
             memcmp(data, "workdir side content\n", len) == 0,
         "WORKDIR side should read back the file's exact content");
    free(data);

    free(repo_root);
    free(git_dir);
}

/* 15. The -2/bad_path contract from sg_tree_flatten is propagated, not
       flattened into -1. Hand-builds a tree with a hostile ".git" entry the
       same way tests/test_path_safe.c does (sg_tree_build itself refuses
       that name, so a hostile tree can only come from bypassing it). */
static void test_flatten_bad_path_propagates(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char hacked_blob[SG_SHA1_RAW_LEN];
    unsigned char dotgit_tree[SG_SHA1_RAW_LEN], root_tree[SG_SHA1_RAW_LEN];
    sg_tree_entry inner[1];
    sg_tree_entry outer[1];
    unsigned char *serialized;
    size_t serialized_len;
    sg_diff_list list;
    char bad_path[SG_PATH_MAX];
    int rc;

    blob(git_dir, "PWNED", hacked_blob);

    inner[0].mode = 0100644;
    inner[0].name = (char *)"hacked.txt";
    memcpy(inner[0].sha1, hacked_blob, SG_SHA1_RAW_LEN);
    CHECK(sg_tree_serialize(inner, 1, &serialized, &serialized_len) == 0, "serialize inner failed");
    CHECK(sg_loose_write(git_dir, SG_OBJ_TREE, serialized, serialized_len, dotgit_tree) == 0,
         "write inner tree failed");
    free(serialized);

    outer[0].mode = 040000;
    outer[0].name = (char *)".git";
    memcpy(outer[0].sha1, dotgit_tree, SG_SHA1_RAW_LEN);
    CHECK(sg_tree_serialize(outer, 1, &serialized, &serialized_len) == 0, "serialize outer failed");
    CHECK(sg_loose_write(git_dir, SG_OBJ_TREE, serialized, serialized_len, root_tree) == 0,
         "write outer tree failed");
    free(serialized);

    memset(bad_path, 0, sizeof(bad_path));
    rc = sg_diff_trees(git_dir, root_tree, NULL, &list, bad_path);
    CHECK(rc == -2, "expected sg_diff_trees to propagate sg_tree_flatten's -2, got %d", rc);
    CHECK(strcmp(bad_path, ".git") == 0, "expected bad_path \".git\", got \"%s\"", bad_path);

    free(git_dir);
}

/* 16. Chunk pointer normalization: sg_diff_index_workdir must compare
       against the chunk pointer's ORIGINAL content id (via
       sg_chunk_effective_id), not the pointer blob's own id -- a fixture
       using ordinary sg_loose_write blobs can't tell this apart from a bug
       that skips normalization entirely, since both would produce the same
       result. This forces real chunking via sg_chunk_store_blob. */
static void fill_pseudo_random(unsigned char *buf, size_t len, unsigned int seed)
{
    size_t i;
    unsigned int state = seed;

    for (i = 0; i < len; i++) {
        state = state * 1103515245u + 12345u;
        buf[i] = (unsigned char)(state >> 16);
    }
}

static void test_chunk_pointer_normalization(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    size_t len = SG_CHUNK_MIN_SIZE * 3;
    unsigned char *data = malloc(len);
    unsigned char pointer_id[SG_SHA1_RAW_LEN];
    int chunked = -1;
    sg_index idx;
    sg_diff_list list;

    CHECK(data != NULL, "malloc failed for chunk fixture data");
    if (data == NULL) {
        free(git_dir);
        free(repo_root);
        return;
    }
    fill_pseudo_random(data, len, 42);

    CHECK(sg_chunk_store_blob(git_dir, data, len, 1024, pointer_id, &chunked) == 0 && chunked == 1,
         "sg_chunk_store_blob should have produced a chunk pointer for this fixture");

    write_workdir_file_bytes(repo_root, "big.bin", data, len);

    memset(&idx, 0, sizeof(idx));
    index_upsert_blob(&idx, "big.bin", 0100644, pointer_id);

    CHECK(sg_diff_index_workdir(git_dir, repo_root, &idx, &list) == 0,
         "sg_diff_index_workdir failed");
    CHECK(list.count == 0,
         "unmodified chunked content must normalize to the same id as the working tree's hash "
         "(got %zu spurious changes) -- sg_chunk_effective_id is not being applied",
         list.count);
    sg_diff_list_free(&list);

    /* Now actually modify the file: the pointer's normalized id must differ
       from the new content, so this positive case rules out a normalization
       bug that always reports "unchanged". */
    data[0] ^= 0xFF;
    write_workdir_file_bytes(repo_root, "big.bin", data, len);

    CHECK(sg_diff_index_workdir(git_dir, repo_root, &idx, &list) == 0,
         "sg_diff_index_workdir failed on the modified fixture");
    CHECK(list.count == 1, "modified chunked content should be reported as changed, got %zu",
         list.count);
    if (list.count == 1)
        CHECK(strcmp(list.entries[0].path, "big.bin") == 0, "unexpected path %s",
             list.entries[0].path);
    sg_diff_list_free(&list);

    sg_index_free(&idx);
    free(data);
    free(repo_root);
    free(git_dir);
}

/* 17. Chunk pointer normalization at sg_diff_tree_workdir's OWN call site
       (distinct from sg_diff_index_workdir's -- diff.c has two independent
       sg_chunk_effective_id call sites, and a fixture that only exercises
       one leaves the other completely unmeasured). Puts the chunk pointer
       in a TREE entry, the same shape `sg commit` produces for a large
       file, and diffs it against the working tree. */
static void test_chunk_pointer_normalization_tree_workdir(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    size_t len = SG_CHUNK_MIN_SIZE * 3;
    unsigned char *data = malloc(len);
    unsigned char pointer_id[SG_SHA1_RAW_LEN];
    int chunked = -1;
    sg_flat_entry entries[1];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    sg_index empty_idx;
    sg_diff_list list;

    CHECK(data != NULL, "malloc failed for chunk fixture data");
    if (data == NULL) {
        free(git_dir);
        free(repo_root);
        return;
    }
    fill_pseudo_random(data, len, 99);

    CHECK(sg_chunk_store_blob(git_dir, data, len, 1024, pointer_id, &chunked) == 0 && chunked == 1,
         "sg_chunk_store_blob should have produced a chunk pointer for this fixture");

    entries[0].path = strdup("big.bin");
    entries[0].mode = 0100644;
    memcpy(entries[0].sha1, pointer_id, SG_SHA1_RAW_LEN);
    CHECK(sg_tree_build(git_dir, entries, 1, tree_id) == 0, "sg_tree_build failed");
    free(entries[0].path);

    write_workdir_file_bytes(repo_root, "big.bin", data, len);

    /* sg_diff_tree_workdir's cmp==0 branch (tree-vs-workdir content compare)
       is only taken for a path present in BOTH the tree and the index --
       an index-less path falls into the cmp<0 "deletion" branch instead, per
       sg/diff.h. The index entry's own sha1/mode are irrelevant here: this
       function never reads content from the index, only from the tree and
       the working tree, so any stage-0 entry at this path is enough to put
       it in the participating set. */
    memset(&empty_idx, 0, sizeof(empty_idx));
    index_upsert_blob(&empty_idx, "big.bin", 0100644, pointer_id);

    CHECK(sg_diff_tree_workdir(git_dir, repo_root, tree_id, &empty_idx, &list, NULL) == 0,
         "sg_diff_tree_workdir failed");
    CHECK(list.count == 0,
         "unmodified chunked content in a TREE entry must normalize to the working tree's hash "
         "(got %zu spurious changes) -- sg_diff_tree_workdir's own sg_chunk_effective_id call is "
         "not being applied",
         list.count);
    sg_diff_list_free(&list);

    /* Now actually modify the file: the pointer's normalized id must differ
       from the new content, ruling out a normalization bug that always
       reports "unchanged" regardless of what it's fed. */
    data[0] ^= 0xFF;
    write_workdir_file_bytes(repo_root, "big.bin", data, len);

    CHECK(sg_diff_tree_workdir(git_dir, repo_root, tree_id, &empty_idx, &list, NULL) == 0,
         "sg_diff_tree_workdir failed on the modified fixture");
    CHECK(list.count == 1, "modified chunked content should be reported as changed, got %zu",
         list.count);
    if (list.count == 1) {
        CHECK(strcmp(list.entries[0].path, "big.bin") == 0, "unexpected path %s",
             list.entries[0].path);
        CHECK(list.entries[0].new_side.kind == SG_DIFF_SIDE_WORKDIR,
             "modified path's new side should be WORKDIR");
    }
    sg_diff_list_free(&list);

    sg_index_free(&empty_idx);
    free(data);
    free(repo_root);
    free(git_dir);
}

/* Deletes a loose object file straight off disk, simulating "the object
   this id names is gone" -- the same end state a lost pack/prune leaves
   behind, and what makes sg_chunk_effective_id return -1 (not a chunk
   pointer at all, just an ordinary blob whose bytes are unreadable). */
static void delete_loose_object(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN])
{
    char hex[SG_SHA1_HEX_LEN + 1];
    char path[4096];

    sg_sha1_to_hex(id, hex);
    snprintf(path, sizeof(path), "%s/objects/%.2s/%s", git_dir, hex, hex + 2);
    CHECK(remove(path) == 0, "failed to delete loose object %s", hex);
}

/* 18. sg_diff_index_workdir: a blob that can't be read at all must not fail
       the whole call -- the builder can't answer "did this path change", so
       it reports the path as changed (old_side BLOB, new_side WORKDIR) and
       leaves the complaining to the renderer's own sg_diff_side_read, which
       holds the path. Directly exercises the sg_chunk_effective_id failure
       branch inside append_index_entry_vs_workdir (see sg/diff.h's
       sg_diff_index_workdir contract). The second assertion group is the
       one that matters: a broken blob must not silence any OTHER path in
       the same diff. */
static void test_index_workdir_unreadable_blob_does_not_silence_others(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char id_broken[SG_SHA1_RAW_LEN];
    unsigned char id_modified[SG_SHA1_RAW_LEN];
    sg_index idx;
    sg_diff_list list;
    const sg_diff_entry *e;

    blob(git_dir, "will vanish\n", id_broken);
    blob(git_dir, "old content\n", id_modified);

    write_workdir_file(repo_root, "broken.txt", "on disk, blob gone\n");
    write_workdir_file(repo_root, "modified.txt", "new content\n");

    delete_loose_object(git_dir, id_broken);

    memset(&idx, 0, sizeof(idx));
    index_upsert_blob(&idx, "broken.txt", 0100644, id_broken);
    index_upsert_blob(&idx, "modified.txt", 0100644, id_modified);

    CHECK(sg_diff_index_workdir(git_dir, repo_root, &idx, &list) == 0,
         "sg_diff_index_workdir must return 0, not -1, when one path's blob is unreadable");
    CHECK(list.count == 2, "expected both paths to be reported, got %zu", list.count);

    e = find_entry(&list, "broken.txt");
    CHECK(e != NULL, "broken.txt (unreadable blob) missing from diff list -- an unreadable blob "
         "must be reported as changed, not silently dropped");
    if (e != NULL) {
        CHECK(e->old_side.kind == SG_DIFF_SIDE_BLOB, "broken.txt's old side should still be BLOB");
        CHECK(e->new_side.kind == SG_DIFF_SIDE_WORKDIR, "broken.txt's new side should be WORKDIR");
    }

    e = find_entry(&list, "modified.txt");
    CHECK(e != NULL,
         "modified.txt must still be reported: one broken blob must not silence the REST of the "
         "diff");

    sg_diff_list_free(&list);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* 19. sg_diff_tree_workdir: the same rule at its OWN sg_chunk_effective_id
       call site (the cmp==0 branch -- this is the m7b site the reviewer's
       mutation found completely uncovered in an earlier round). Both
       broken.txt and other.txt get a stage-0 index entry so the cmp==0
       content-compare branch actually runs for both, not just membership. */
static void test_tree_workdir_unreadable_blob_does_not_silence_others(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char id_broken[SG_SHA1_RAW_LEN];
    unsigned char id_other[SG_SHA1_RAW_LEN];
    unsigned char old_tree[SG_SHA1_RAW_LEN];
    sg_flat_entry entries[2];
    sg_index idx;
    sg_diff_list list;
    const sg_diff_entry *e;

    blob(git_dir, "will vanish\n", id_broken);
    blob(git_dir, "before\n", id_other);

    entries[0].path = strdup("broken.txt");
    entries[0].mode = 0100644;
    memcpy(entries[0].sha1, id_broken, SG_SHA1_RAW_LEN);
    entries[1].path = strdup("other.txt");
    entries[1].mode = 0100644;
    memcpy(entries[1].sha1, id_other, SG_SHA1_RAW_LEN);
    CHECK(sg_tree_build(git_dir, entries, 2, old_tree) == 0, "sg_tree_build failed");
    free(entries[0].path);
    free(entries[1].path);

    write_workdir_file(repo_root, "broken.txt", "on disk, blob gone\n");
    write_workdir_file(repo_root, "other.txt", "after\n");

    delete_loose_object(git_dir, id_broken);

    /* The cmp==0 branch only runs for a path present in BOTH the tree and
       the index -- see sg_diff_tree_workdir's membership rule in
       sg/diff.h. The index entries' own ids are irrelevant: this function
       never reads content from the index. */
    memset(&idx, 0, sizeof(idx));
    index_upsert_blob(&idx, "broken.txt", 0100644, id_broken);
    index_upsert_blob(&idx, "other.txt", 0100644, id_other);

    CHECK(sg_diff_tree_workdir(git_dir, repo_root, old_tree, &idx, &list, NULL) == 0,
         "sg_diff_tree_workdir must return 0, not -1, when one path's tree blob is unreadable");
    CHECK(list.count == 2, "expected both paths to be reported, got %zu", list.count);

    e = find_entry(&list, "broken.txt");
    CHECK(e != NULL, "broken.txt (unreadable tree blob) missing from diff list");
    if (e != NULL) {
        CHECK(e->old_side.kind == SG_DIFF_SIDE_BLOB, "broken.txt's old side should still be BLOB");
        CHECK(e->new_side.kind == SG_DIFF_SIDE_WORKDIR, "broken.txt's new side should be WORKDIR");
    }

    e = find_entry(&list, "other.txt");
    CHECK(e != NULL,
         "other.txt must still be reported: one broken tree blob must not silence the REST of the "
         "diff");

    sg_diff_list_free(&list);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* workdir_entry_mode's S_ISLNK guard (diff.c): a tracked path that is
   actually a symlink on disk must be reported as mode 100644, never 100755,
   regardless of the symlink's OWN permission bits -- which on both Linux and
   macOS are conventionally lrwxrwxrwx (all exec bits set) no matter what the
   *target* file's permissions are. Without the guard, workdir_entry_mode
   would read the symlink's own S_IXUSR bit (always set) via lstat and
   compute 100755, a mode this codebase never intends to produce for a
   symlink (120000 is out of scope this round -- see sg/diff.h). This is the
   codebase's ACTUAL current behavior, confirmed by this test, not merely the
   author's expectation: it is what makes the row below stay absent from the
   diff list even though the symlink's own mode bits look executable. */
static void test_symlink_guard_reports_fixed_mode(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char id[SG_SHA1_RAW_LEN];
    char target_abspath[4096];
    char link_abspath[4096];
    sg_index idx;
    sg_diff_list list;

    blob(git_dir, "target content\n", id);
    write_workdir_file(repo_root, "target.txt", "target content\n");

    snprintf(target_abspath, sizeof(target_abspath), "%s/target.txt", repo_root);
    snprintf(link_abspath, sizeof(link_abspath), "%s/link.txt", repo_root);
    CHECK(symlink("target.txt", link_abspath) == 0, "symlink() failed");
    (void)target_abspath;

    /* link.txt is tracked at mode 100644 with the SAME content id as the
       symlink's target -- sg_hash_file_blob follows the symlink via fopen(),
       so its computed content matches. If workdir_entry_mode reported
       100755 for this path (the mutated, guard-less behavior), the mode
       mismatch alone would put link.txt in the list even though its content
       is unchanged. */
    memset(&idx, 0, sizeof(idx));
    index_upsert_blob(&idx, "link.txt", 0100644, id);

    CHECK(sg_diff_index_workdir(git_dir, repo_root, &idx, &list) == 0,
         "sg_diff_index_workdir failed");
    CHECK(list.count == 0,
         "a tracked symlink whose target content is unchanged must not appear in the diff list "
         "(got %zu rows) -- workdir_entry_mode must report 100644 for a symlink, not its own "
         "(always-executable) permission bits",
         list.count);

    sg_diff_list_free(&list);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* Phase 26 fix-round regression (F1, site A): a tracked file that stat()s
   successfully but cannot actually be read (here: the path is a directory,
   not chmod 000, since a test running as root would bypass permission bits
   entirely -- fopen()+fread() on a directory reliably fails with ferror set
   on both Linux and macOS, exercising the exact same sg_hash_file_blob
   failure without depending on the test's uid) must be reported as an
   ordinary deletion (new_side ABSENT), not a WORKDIR side carrying a
   placeholder all-zero id -- see append_index_entry_vs_workdir in
   src/workdir/diff.c and sg/diff.h's sg_diff_index_workdir contract. */
static void test_index_workdir_unreadable_workdir_file_is_absent(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char id[SG_SHA1_RAW_LEN];
    char abspath[4096];
    sg_index idx;
    sg_diff_list list;
    const sg_diff_entry *e;

    blob(git_dir, "was a file\n", id);

    snprintf(abspath, sizeof(abspath), "%s/was_file.txt", repo_root);
    CHECK(mkdir(abspath, 0755) == 0, "mkdir (standing in for an unreadable file) failed");

    memset(&idx, 0, sizeof(idx));
    index_upsert_blob(&idx, "was_file.txt", 0100644, id);

    CHECK(sg_diff_index_workdir(git_dir, repo_root, &idx, &list) == 0,
         "sg_diff_index_workdir failed");
    CHECK(list.count == 1, "expected exactly one row, got %zu", list.count);

    e = find_entry(&list, "was_file.txt");
    CHECK(e != NULL, "was_file.txt missing from diff list");
    if (e != NULL) {
        CHECK(e->old_side.kind == SG_DIFF_SIDE_BLOB, "was_file.txt's old side should be BLOB");
        CHECK(e->new_side.kind == SG_DIFF_SIDE_ABSENT,
             "was_file.txt's new side must be ABSENT when the workdir path exists but its content "
             "cannot be read, not a WORKDIR side with a placeholder id (got kind %d)",
             e->new_side.kind);
    }

    sg_diff_list_free(&list);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* Phase 26 fix-round regression (F1, site B): the same rule at
   sg_diff_tree_workdir's OWN cmp==0 content-compare call site, independent
   of the append_index_entry_vs_workdir site above. */
static void test_tree_workdir_unreadable_workdir_file_is_absent(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char id[SG_SHA1_RAW_LEN];
    unsigned char old_tree[SG_SHA1_RAW_LEN];
    tree_spec specs[] = {
        {"was_file.txt", 0100644, "was a file\n"},
    };
    char abspath[4096];
    sg_index idx;
    sg_diff_list list;
    const sg_diff_entry *e;

    build_tree(git_dir, specs, 1, old_tree);
    blob(git_dir, "was a file\n", id);

    snprintf(abspath, sizeof(abspath), "%s/was_file.txt", repo_root);
    CHECK(mkdir(abspath, 0755) == 0, "mkdir (standing in for an unreadable file) failed");

    /* cmp==0 requires the path present in the index too, per
       sg_diff_tree_workdir's membership rule (sg/diff.h). */
    memset(&idx, 0, sizeof(idx));
    index_upsert_blob(&idx, "was_file.txt", 0100644, id);

    CHECK(sg_diff_tree_workdir(git_dir, repo_root, old_tree, &idx, &list, NULL) == 0,
         "sg_diff_tree_workdir failed");
    CHECK(list.count == 1, "expected exactly one row, got %zu", list.count);

    e = find_entry(&list, "was_file.txt");
    CHECK(e != NULL, "was_file.txt missing from diff list");
    if (e != NULL) {
        CHECK(e->old_side.kind == SG_DIFF_SIDE_BLOB, "was_file.txt's old side should be BLOB");
        CHECK(e->new_side.kind == SG_DIFF_SIDE_ABSENT,
             "was_file.txt's new side must be ABSENT when the workdir path exists but its content "
             "cannot be read, not a WORKDIR side with a placeholder id (got kind %d)",
             e->new_side.kind);
    }

    sg_diff_list_free(&list);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

int main(void)
{
    test_trees_add_delete_modify();
    test_trees_mode_only_change();
    test_null_tree_is_empty_tree();
    test_output_sorted_by_path();
    test_tree_index_stage0_add_delete_modify();
    test_tree_index_unmerged_is_single_row();
    test_index_workdir();
    test_index_workdir_mode_only_change();
    test_index_workdir_side_carries_content_id();
    test_tree_workdir_addition_side_has_id_and_mode();
    test_index_workdir_unmerged_two_rows();
    test_index_workdir_unmerged_no_second_row();
    test_tree_workdir_rm_cached_is_deletion();
    test_tree_workdir_content_compare_and_addition();
    test_tree_workdir_unmerged_not_special();
    test_side_read_absent();
    test_side_read_blob_and_workdir();
    test_flatten_bad_path_propagates();
    test_chunk_pointer_normalization();
    test_chunk_pointer_normalization_tree_workdir();
    test_index_workdir_unreadable_blob_does_not_silence_others();
    test_tree_workdir_unreadable_blob_does_not_silence_others();
    test_symlink_guard_reports_fixed_mode();
    test_index_workdir_unreadable_workdir_file_is_absent();
    test_tree_workdir_unreadable_workdir_file_is_absent();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all diff_list tests passed\n");
    return 0;
}
