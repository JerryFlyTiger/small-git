/* Phase 52 item A: sg_merge_trees must flatten base/ours/theirs exactly
   once each, not seven times -- see docs/DESIGN.md's Phase 52 section for
   where the other four calls used to come from (build_rename_map calling
   sg_diff_trees, which flattened both its sides all over again). Nothing
   about sg_merge_trees' RESULT can tell the two shapes apart -- both give
   the identical sg_merge_result -- so this test exists purely to watch the
   sg_tree_flatten_test_count hook (sg/tree_build.h) that Phase 52 added for
   exactly this purpose. */

#include "sg/merge.h"

#include "sg/index.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/repo.h"
#include "sg/similarity.h"
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
    static char template[] = "/tmp/sg_merge_flatten_count_test_XXXXXX";
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

static void blob(const char *git_dir, const char *content, unsigned char id[SG_SHA1_RAW_LEN])
{
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, (const unsigned char *)content, strlen(content), id) ==
             0,
         "failed to write blob %s", content);
}

typedef struct {
    const char *path;
    const char *content;
} tree_spec;

/* specs must already be caller-sorted by path -- sg_tree_build requires it. */
static void build_tree(const char *git_dir, const tree_spec *specs, size_t count,
                       unsigned char tree_id[SG_SHA1_RAW_LEN])
{
    sg_flat_entry *entries = malloc((count > 0 ? count : 1) * sizeof(*entries));
    size_t i;

    for (i = 0; i < count; i++) {
        entries[i].path = strdup(specs[i].path);
        entries[i].mode = 0100644;
        blob(git_dir, specs[i].content, entries[i].sha1);
    }
    CHECK(sg_tree_build(git_dir, entries, count, tree_id) == 0, "sg_tree_build failed");
    for (i = 0; i < count; i++)
        free(entries[i].path);
    free(entries);
}

int main(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char base_tree[SG_SHA1_RAW_LEN];
    unsigned char ours_tree[SG_SHA1_RAW_LEN];
    unsigned char theirs_tree[SG_SHA1_RAW_LEN];
    sg_merge_result result;

    /* No rename in sight, just three ordinary edits, so every rename-map
       call site still runs (rename_score > 0 does not require the map to
       find anything, only that build_rename_map itself executes). */
    tree_spec base_specs[] = {
        { "a.txt", "base a\n" },
        { "b.txt", "base b\n" },
    };
    tree_spec ours_specs[] = {
        { "a.txt", "ours a\n" },
        { "b.txt", "base b\n" },
    };
    tree_spec theirs_specs[] = {
        { "a.txt", "base a\n" },
        { "b.txt", "theirs b\n" },
    };

    build_tree(git_dir, base_specs, 2, base_tree);
    build_tree(git_dir, ours_specs, 2, ours_tree);
    build_tree(git_dir, theirs_specs, 2, theirs_tree);

    sg_tree_flatten_test_reset();
    CHECK(sg_merge_trees(git_dir, base_tree, ours_tree, theirs_tree, "ours", "theirs",
                         SG_SIMILARITY_DEFAULT, &result) == 0,
         "sg_merge_trees with renames on failed");
    CHECK(sg_tree_flatten_test_count() == 3,
         "expected exactly 3 flattens with rename_score = SG_SIMILARITY_DEFAULT, got %zu",
         sg_tree_flatten_test_count());
    sg_merge_result_free(&result);

    sg_tree_flatten_test_reset();
    CHECK(sg_merge_trees(git_dir, base_tree, ours_tree, theirs_tree, "ours", "theirs", 0,
                         &result) == 0,
         "sg_merge_trees with renames off failed");
    CHECK(sg_tree_flatten_test_count() == 3,
         "expected exactly 3 flattens with rename_score = 0, got %zu",
         sg_tree_flatten_test_count());
    sg_merge_result_free(&result);

    free(git_dir);

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
