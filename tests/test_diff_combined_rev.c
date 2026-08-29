/* Phase 40: `sg diff -c/--cc <rev>` -- the two building blocks
   (sg_diff_tree_workdir's combined-aware inclusion widening, and
   sg_diff_fill_combined_from_index's stage-picking), plus the shared
   sg_diff_entry_is_combined predicate and sg_diff_reorder_combined_first.
   See Phase 40 of docs/DESIGN.md and SPEC.md (in the Phase 40 scratch
   directory) for what each of these is measured against.

   Same no-shared-header idiom as every other test in this project (see
   tests/test_confirm.c). Fixture helpers are copied from test_diff_list.c
   rather than shared, per CLAUDE.md's "there is no shared fixture helper,
   do not go looking for one". */
#include "sg/diff.h"

#include "sg/hash.h"
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
    static char template[] = "/tmp/sg_diff_combined_rev_test_XXXXXX";
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

static unsigned char *blob(const char *git_dir, const char *content, unsigned char id[SG_SHA1_RAW_LEN])
{
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, content, strlen(content), id) == 0,
         "failed to write blob %s", content);
    return id;
}

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

static const sg_diff_entry *find_entry(const sg_diff_list *list, const char *path)
{
    size_t i;

    for (i = 0; i < list->count; i++)
        if (strcmp(list->entries[i].path, path) == 0)
            return &list->entries[i];
    return NULL;
}

/* Runs the two Phase 40 passes in the same order cmd_diff.c does (builder,
   then the fill pass) and returns the single row for `path`, which must
   exist. `combined` is always 1 here -- these tests are about the FILL
   pass's own stage-picking logic, not about the "-c/--cc <rev> with no
   flag" gating cmd_diff.c does separately. */
static const sg_diff_entry *run_combined(const char *git_dir, const char *repo_root,
                                         const unsigned char *old_tree, sg_index *idx,
                                         sg_diff_list *list, const char *path)
{
    CHECK(sg_diff_tree_workdir(git_dir, repo_root, old_tree, idx, list, NULL, 1) == 0,
         "sg_diff_tree_workdir failed");
    sg_diff_fill_combined_from_index(idx, list);
    return find_entry(list, path);
}

/* SPEC section 2, stage set {0}: an ordinary tracked, unconflicted path --
   ours must be the stage-0 entry, the overwhelmingly common case. */
static void test_stage0_ordinary(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char old_tree[SG_SHA1_RAW_LEN];
    unsigned char id_index[SG_SHA1_RAW_LEN];
    tree_spec specs[] = {{"f.txt", 0100644, "named-tree\n"}};
    sg_index idx;
    sg_diff_list list;
    const sg_diff_entry *e;

    build_tree(git_dir, specs, 1, old_tree);
    blob(git_dir, "staged\n", id_index);
    write_workdir_file(repo_root, "f.txt", "worktree\n");

    memset(&idx, 0, sizeof(idx));
    index_upsert_stage(&idx, "f.txt", 0, 0100644, id_index);

    e = run_combined(git_dir, repo_root, old_tree, &idx, &list, "f.txt");
    CHECK(e != NULL, "f.txt missing from list");
    if (e != NULL) {
        CHECK(e->ours.kind == SG_DIFF_SIDE_BLOB, "ours should be a BLOB side");
        CHECK(memcmp(e->ours.id, id_index, SG_SHA1_RAW_LEN) == 0,
             "ours must be the stage-0 (index) blob, not some other stage");
        CHECK(sg_diff_entry_is_combined(e), "stage0 ordinary path must be combinable");
    }

    sg_diff_list_free(&list);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* SPEC section 2, stage set {1,2,3}: full three-way conflict -- ours must be
   stage 1 (the merge base), not stage 2 or 3. */
static void test_stage_123_picks_stage1(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char old_tree[SG_SHA1_RAW_LEN];
    unsigned char id1[SG_SHA1_RAW_LEN], id2[SG_SHA1_RAW_LEN], id3[SG_SHA1_RAW_LEN];
    tree_spec specs[] = {{"f.txt", 0100644, "named-tree\n"}};
    sg_index idx;
    sg_diff_list list;
    const sg_diff_entry *e;

    build_tree(git_dir, specs, 1, old_tree);
    blob(git_dir, "base\n", id1);
    blob(git_dir, "ours\n", id2);
    blob(git_dir, "theirs\n", id3);
    write_workdir_file(repo_root, "f.txt", "worktree\n");

    memset(&idx, 0, sizeof(idx));
    index_upsert_stage(&idx, "f.txt", 1, 0100644, id1);
    index_upsert_stage(&idx, "f.txt", 2, 0100644, id2);
    index_upsert_stage(&idx, "f.txt", 3, 0100644, id3);

    e = run_combined(git_dir, repo_root, old_tree, &idx, &list, "f.txt");
    CHECK(e != NULL, "f.txt missing from list");
    if (e != NULL) {
        CHECK(e->ours.kind == SG_DIFF_SIDE_BLOB, "ours should be a BLOB side");
        CHECK(memcmp(e->ours.id, id1, SG_SHA1_RAW_LEN) == 0,
             "ours must be stage 1 (the base), not stage 2 or 3");
    }

    sg_diff_list_free(&list);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* SPEC section 2, stage set {1,3} (a delete/modify conflict: ours deleted
   the path, so there is no stage 2): ours must still be stage 1. */
static void test_stage_13_picks_stage1(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char old_tree[SG_SHA1_RAW_LEN];
    unsigned char id1[SG_SHA1_RAW_LEN], id3[SG_SHA1_RAW_LEN];
    tree_spec specs[] = {{"f.txt", 0100644, "named-tree\n"}};
    sg_index idx;
    sg_diff_list list;
    const sg_diff_entry *e;

    build_tree(git_dir, specs, 1, old_tree);
    blob(git_dir, "base\n", id1);
    blob(git_dir, "theirs\n", id3);
    write_workdir_file(repo_root, "f.txt", "worktree\n");

    memset(&idx, 0, sizeof(idx));
    index_upsert_stage(&idx, "f.txt", 1, 0100644, id1);
    index_upsert_stage(&idx, "f.txt", 3, 0100644, id3);

    e = run_combined(git_dir, repo_root, old_tree, &idx, &list, "f.txt");
    CHECK(e != NULL, "f.txt missing from list");
    if (e != NULL) {
        CHECK(e->ours.kind == SG_DIFF_SIDE_BLOB, "ours should be a BLOB side");
        CHECK(memcmp(e->ours.id, id1, SG_SHA1_RAW_LEN) == 0,
             "stage set {1,3}: ours must be stage 1");
    }

    sg_diff_list_free(&list);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* SPEC section 2, stage set {2,3} (add/add: no stage 1 at all) -- the
   easiest one to get wrong, because "stage 1 when unmerged" silently picks
   the wrong parent here. ours must fall back to stage 2. */
static void test_stage_23_picks_stage2(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char id2[SG_SHA1_RAW_LEN], id3[SG_SHA1_RAW_LEN];
    sg_index idx;
    sg_diff_list list;
    const sg_diff_entry *e;

    /* f.txt is absent from the named tree entirely -- an add/add conflict
       has no base version anywhere, which is exactly why there is no
       stage 1 to fall back on. NULL old_tree means "the empty tree", so
       there is nothing else in it that could confuse the walk. */
    blob(git_dir, "ours\n", id2);
    blob(git_dir, "theirs\n", id3);
    write_workdir_file(repo_root, "f.txt", "worktree\n");

    memset(&idx, 0, sizeof(idx));
    index_upsert_stage(&idx, "f.txt", 2, 0100644, id2);
    index_upsert_stage(&idx, "f.txt", 3, 0100644, id3);

    e = run_combined(git_dir, repo_root, NULL, &idx, &list, "f.txt");
    CHECK(e != NULL, "f.txt missing from list");
    if (e != NULL) {
        CHECK(e->ours.kind == SG_DIFF_SIDE_BLOB, "ours should be a BLOB side");
        CHECK(memcmp(e->ours.id, id2, SG_SHA1_RAW_LEN) == 0,
             "stage set {2,3} (add/add, no stage 1): ours must fall back to stage 2");
        /* theirs is the named tree's blob for f.txt, which does not exist
           at all -- so this row must NOT render combined, same as SPEC
           section 3's "path absent from the named tree" fallback shape. */
        CHECK(e->theirs.kind == SG_DIFF_SIDE_ABSENT,
             "f.txt is absent from the named tree, theirs must be ABSENT");
        CHECK(!sg_diff_entry_is_combined(e),
             "an add/add row whose path is absent from the named tree must not be combinable");
    }

    sg_diff_list_free(&list);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* SPEC section 3: a working-tree deletion disqualifies a row from rendering
   combined, even though ours and theirs are both present -- the asymmetry
   documented on sg_diff_entry_is_combined (unlike a real conflict, where a
   deleted result still renders combined). */
static void test_worktree_deletion_not_combinable(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char old_tree[SG_SHA1_RAW_LEN];
    unsigned char id_index[SG_SHA1_RAW_LEN];
    tree_spec specs[] = {{"f.txt", 0100644, "named-tree\n"}};
    sg_index idx;
    sg_diff_list list;
    const sg_diff_entry *e;

    build_tree(git_dir, specs, 1, old_tree);
    blob(git_dir, "staged\n", id_index);
    /* Deliberately no write_workdir_file: the path is tracked but missing
       from disk, same as "rm f.txt" without staging the deletion. */

    memset(&idx, 0, sizeof(idx));
    index_upsert_stage(&idx, "f.txt", 0, 0100644, id_index);

    e = run_combined(git_dir, repo_root, old_tree, &idx, &list, "f.txt");
    CHECK(e != NULL, "f.txt missing from list");
    if (e != NULL) {
        CHECK(e->ours.kind == SG_DIFF_SIDE_BLOB, "ours must still be filled in");
        CHECK(e->theirs.kind == SG_DIFF_SIDE_BLOB, "theirs must still be filled in");
        CHECK(e->result.kind == SG_DIFF_SIDE_ABSENT, "result must be ABSENT (worktree deletion)");
        CHECK(!sg_diff_entry_is_combined(e),
             "a worktree deletion must not be combinable even with ours/theirs both present");
    }

    sg_diff_list_free(&list);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* SPEC section 6, fixture O's p3 shape: the named tree equals the working
   tree, but the index does not -- sg_diff_tree_workdir's ordinary
   inclusion rule alone would drop this row entirely (tree == workdir), so
   `combined=1` must widen it in. Also confirms `combined=0` reproduces the
   pre-Phase-40 omission exactly, which every other caller of this function
   still relies on. */
static void test_combined_widens_inclusion_when_tree_equals_workdir(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char old_tree[SG_SHA1_RAW_LEN];
    unsigned char id_index[SG_SHA1_RAW_LEN];
    tree_spec specs[] = {{"f.txt", 0100644, "shared\n"}};
    sg_index idx;
    sg_diff_list list_plain, list_combined;
    const sg_diff_entry *e;

    build_tree(git_dir, specs, 1, old_tree);
    blob(git_dir, "index-only\n", id_index);
    write_workdir_file(repo_root, "f.txt", "shared\n"); /* == named tree */

    memset(&idx, 0, sizeof(idx));
    index_upsert_stage(&idx, "f.txt", 0, 0100644, id_index);

    CHECK(sg_diff_tree_workdir(git_dir, repo_root, old_tree, &idx, &list_plain, NULL, 0) == 0,
         "sg_diff_tree_workdir(combined=0) failed");
    CHECK(find_entry(&list_plain, "f.txt") == NULL,
         "combined=0 must reproduce the pre-Phase-40 omission (tree == workdir)");

    e = run_combined(git_dir, repo_root, old_tree, &idx, &list_combined, "f.txt");
    CHECK(e != NULL, "combined=1 must widen inclusion when the index differs from the tree");
    if (e != NULL) {
        CHECK(memcmp(e->ours.id, id_index, SG_SHA1_RAW_LEN) == 0, "ours must be the index blob");
        CHECK(sg_diff_entry_is_combined(e), "this row must render combined");
    }

    sg_diff_list_free(&list_plain);
    sg_diff_list_free(&list_combined);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* SPEC section 4: sg_diff_reorder_combined_first is a STABLE partition --
   combined rows first (in their existing relative order), then
   non-combined rows (likewise), never resorted by anything else. */
static void test_reorder_combined_first_is_stable(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char old_tree[SG_SHA1_RAW_LEN];
    unsigned char id_a[SG_SHA1_RAW_LEN], id_c[SG_SHA1_RAW_LEN], id_e[SG_SHA1_RAW_LEN];
    tree_spec specs[] = {
        {"a.txt", 0100644, "base-a\n"},
        {"c.txt", 0100644, "base-c\n"},
        {"d.txt", 0100644, "base-d\n"},
        {"e.txt", 0100644, "base-e\n"},
    };
    sg_index idx;
    sg_diff_list list;

    /* b.txt is deliberately NOT in the named tree -- it must be a staged
       ADD (absent from the named tree), not merely present-and-unstaged,
       to be genuinely non-combinable (see test_stage0_ordinary and
       test_combined_widens_inclusion_when_tree_equals_workdir above: a
       tracked, unstaged path is still combinable, since the index always
       carries a stage-0 entry for it). */
    build_tree(git_dir, specs, 4, old_tree);
    /* a/c/e: staged AND worktree-edited -- combinable. b: staged ADD,
       absent from the named tree -- not combinable. d: tracked, deleted
       from the working tree -- not combinable. */
    blob(git_dir, "staged-a\n", id_a);
    blob(git_dir, "staged-c\n", id_c);
    blob(git_dir, "staged-e\n", id_e);
    write_workdir_file(repo_root, "a.txt", "work-a\n");
    write_workdir_file(repo_root, "c.txt", "work-c\n");
    write_workdir_file(repo_root, "e.txt", "work-e\n");
    write_workdir_file(repo_root, "b.txt", "new-b\n");

    memset(&idx, 0, sizeof(idx));
    index_upsert_stage(&idx, "a.txt", 0, 0100644, id_a);
    {
        unsigned char id_b[SG_SHA1_RAW_LEN];

        blob(git_dir, "new-b\n", id_b);
        index_upsert_stage(&idx, "b.txt", 0, 0100644, id_b);
    }
    index_upsert_stage(&idx, "c.txt", 0, 0100644, id_c);
    /* d.txt: no index entry override needed -- leave the stage-0 entry
       pointing at the named tree's own blob (never staged), and delete it
       from disk below. */
    {
        unsigned char id_d[SG_SHA1_RAW_LEN];

        blob(git_dir, "base-d\n", id_d);
        index_upsert_stage(&idx, "d.txt", 0, 0100644, id_d);
    }
    index_upsert_stage(&idx, "e.txt", 0, 0100644, id_e);
    /* d.txt deliberately never gets write_workdir_file -- deleted from disk. */

    CHECK(sg_diff_tree_workdir(git_dir, repo_root, old_tree, &idx, &list, NULL, 1) == 0,
         "sg_diff_tree_workdir failed");
    sg_diff_fill_combined_from_index(&idx, &list);
    CHECK(list.count == 5, "expected 5 rows (a..e), got %zu", list.count);

    CHECK(sg_diff_reorder_combined_first(&list) == 0, "sg_diff_reorder_combined_first failed");

    if (list.count == 5) {
        CHECK(strcmp(list.entries[0].path, "a.txt") == 0, "entry 0 should be a.txt, got %s",
             list.entries[0].path);
        CHECK(strcmp(list.entries[1].path, "c.txt") == 0, "entry 1 should be c.txt, got %s",
             list.entries[1].path);
        CHECK(strcmp(list.entries[2].path, "e.txt") == 0, "entry 2 should be e.txt, got %s",
             list.entries[2].path);
        CHECK(strcmp(list.entries[3].path, "b.txt") == 0, "entry 3 should be b.txt, got %s",
             list.entries[3].path);
        CHECK(strcmp(list.entries[4].path, "d.txt") == 0, "entry 4 should be d.txt, got %s",
             list.entries[4].path);
        CHECK(sg_diff_entry_is_combined(&list.entries[0]), "a.txt should be combined");
        CHECK(sg_diff_entry_is_combined(&list.entries[1]), "c.txt should be combined");
        CHECK(sg_diff_entry_is_combined(&list.entries[2]), "e.txt should be combined");
        CHECK(!sg_diff_entry_is_combined(&list.entries[3]), "b.txt should not be combined");
        CHECK(!sg_diff_entry_is_combined(&list.entries[4]), "d.txt should not be combined");
    }

    sg_diff_list_free(&list);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

int main(void)
{
    test_stage0_ordinary();
    test_stage_123_picks_stage1();
    test_stage_13_picks_stage1();
    test_stage_23_picks_stage2();
    test_worktree_deletion_not_combinable();
    test_combined_widens_inclusion_when_tree_equals_workdir();
    test_reorder_combined_first_is_stable();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all diff combined-rev tests passed\n");
    return 0;
}
