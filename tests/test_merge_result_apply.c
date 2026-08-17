/* Direct unit coverage of sg_merge_trees + sg_merge_result_apply's Phase 20
   change: a clean entry whose resolved outcome already equals ours is left
   alone on disk (not rewritten) and a "deleted" entry that equals ours (ours
   never had the path) is not remove()'d -- while the index built alongside
   still ends up complete, with a stage-0 entry for every path the merge
   result carries, touched or not. sg_stash_apply (tests/test_stash.c) covers
   the same change through the stash CLI's own dirty-workdir gate; this file
   isolates sg_merge_trees/sg_merge_result_apply themselves so a regression
   here does not depend on stash's extra layers. */

#include "sg/merge.h"

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
    static char template[] = "/tmp/sg_merge_apply_test_XXXXXX";
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

static char *read_workdir_file(const char *repo_root, const char *rel)
{
    char abspath[4096];
    unsigned char *buf;
    size_t len;
    char *s;

    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, rel);
    if (sg_read_file(abspath, &buf, &len) != 0)
        return NULL;
    s = malloc(len + 1);
    memcpy(s, buf, len);
    s[len] = '\0';
    free(buf);
    return s;
}

static int file_exists(const char *repo_root, const char *rel)
{
    char abspath[4096];
    struct stat st;

    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, rel);
    return lstat(abspath, &st) == 0;
}

static void write_blob(const char *git_dir, const char *content, unsigned char sha1_out[SG_SHA1_RAW_LEN])
{
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, content, strlen(content), sha1_out) == 0,
         "failed to write blob %s", content);
}

/* Two-file merge where only ONE file (b.txt) actually changes relative to
   ours (theirs modifies it away from base; ours itself never moved from
   base) -- a.txt stays untouched throughout, on both sides. Asserts the
   resulting in-memory index (index_out) still carries stage-0 entries for
   BOTH files, not just the one the merge touched (spec sec 6.5: the merge
   commit's tree is built straight from this index in cmd_merge.c/
   cmd_rebase.c, so a missing entry here would silently drop a file from the
   merge commit) -- and that only the touched path (b.txt) actually gets
   fresh content written to the working tree; the untouched path (a.txt) is
   left exactly as it already was there (pre-seeded, never rewritten). */
static void test_untouched_path_stays_in_index(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char blob_a[SG_SHA1_RAW_LEN];
    unsigned char blob_b1[SG_SHA1_RAW_LEN];
    unsigned char blob_b2[SG_SHA1_RAW_LEN];
    sg_flat_entry base_entries[2];
    sg_flat_entry theirs_entries[2];
    unsigned char base_tree[SG_SHA1_RAW_LEN];
    unsigned char theirs_tree[SG_SHA1_RAW_LEN];
    sg_merge_result result;
    sg_index index_out;
    char **conflict_paths = NULL;
    size_t conflict_count = 0;
    int pos;

    write_blob(git_dir, "a1\n", blob_a);
    write_blob(git_dir, "b1\n", blob_b1);
    write_blob(git_dir, "b2\n", blob_b2);

    base_entries[0].path = (char *)"a.txt";
    base_entries[0].mode = 0100644;
    memcpy(base_entries[0].sha1, blob_a, SG_SHA1_RAW_LEN);
    base_entries[1].path = (char *)"b.txt";
    base_entries[1].mode = 0100644;
    memcpy(base_entries[1].sha1, blob_b1, SG_SHA1_RAW_LEN);
    CHECK(sg_tree_build(git_dir, base_entries, 2, base_tree) == 0, "build base tree");

    theirs_entries[0] = base_entries[0]; /* a.txt unchanged */
    theirs_entries[1].path = (char *)"b.txt";
    theirs_entries[1].mode = 0100644;
    memcpy(theirs_entries[1].sha1, blob_b2, SG_SHA1_RAW_LEN); /* b.txt modified by theirs */
    CHECK(sg_tree_build(git_dir, theirs_entries, 2, theirs_tree) == 0, "build theirs tree");

    /* Pre-seed the working tree the way an already-checked-out repo would
       have it: a.txt at ours's own (unchanged) content. b.txt is left
       absent -- it must come from theirs during this apply. */
    write_workdir_file(repo_root, "a.txt", "a1\n");

    /* ours == base: only theirs actually did anything. */
    CHECK(sg_merge_trees(git_dir, base_tree, base_tree, theirs_tree, "ours", "theirs", &result) == 0,
         "sg_merge_trees failed");
    CHECK(result.count == 2, "expected 2 entries in the merge result, got %zu", result.count);

    CHECK(sg_merge_result_apply(git_dir, repo_root, &result, &index_out, &conflict_paths,
                                &conflict_count) == 0,
         "sg_merge_result_apply failed");
    CHECK(conflict_count == 0, "expected a clean merge, got %zu conflicts", conflict_count);

    pos = sg_index_find(&index_out, "a.txt");
    CHECK(pos >= 0, "a.txt (untouched by either side) must still be in the resulting index");
    if (pos >= 0)
        CHECK(memcmp(index_out.entries[pos].sha1, blob_a, SG_SHA1_RAW_LEN) == 0,
             "a.txt's index entry must carry its own (unchanged) blob");

    pos = sg_index_find(&index_out, "b.txt");
    CHECK(pos >= 0, "b.txt (the path the merge actually touched) must be in the resulting index");
    if (pos >= 0)
        CHECK(memcmp(index_out.entries[pos].sha1, blob_b2, SG_SHA1_RAW_LEN) == 0,
             "b.txt's index entry must carry theirs's blob");

    {
        char *on_disk_a = read_workdir_file(repo_root, "a.txt");
        char *on_disk_b = read_workdir_file(repo_root, "b.txt");

        CHECK(on_disk_a != NULL && strcmp(on_disk_a, "a1\n") == 0,
             "a.txt (untouched) must stay exactly as it was pre-seeded, got %s",
             on_disk_a != NULL ? on_disk_a : "(missing)");
        CHECK(on_disk_b != NULL && strcmp(on_disk_b, "b2\n") == 0,
             "b.txt (touched) must be freshly written from theirs's blob, got %s",
             on_disk_b != NULL ? on_disk_b : "(missing)");
        free(on_disk_a);
        free(on_disk_b);
    }

    sg_index_free(&index_out);
    sg_merge_result_free(&result);
    free(repo_root);
    free(git_dir);
}

/* A clean entry whose resolved outcome equals ours must NOT be rewritten to
   disk -- verified directly by planting dirty (non-ours) content at the
   path before calling sg_merge_result_apply and confirming it survives. */
static void test_equal_to_ours_not_rewritten(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char blob_c[SG_SHA1_RAW_LEN];
    sg_flat_entry entries[1];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    sg_merge_result result;
    sg_index index_out;
    char **conflict_paths = NULL;
    size_t conflict_count = 0;
    int pos;
    char *on_disk;

    write_blob(git_dir, "c1\n", blob_c);
    entries[0].path = (char *)"c.txt";
    entries[0].mode = 0100644;
    memcpy(entries[0].sha1, blob_c, SG_SHA1_RAW_LEN);
    CHECK(sg_tree_build(git_dir, entries, 1, tree_id) == 0, "build tree");

    /* base == ours == theirs: c.txt is untouched by the merge in every
       sense, so its resolved outcome trivially equals ours. */
    write_workdir_file(repo_root, "c.txt", "DIRTY-UNCOMMITTED\n");

    CHECK(sg_merge_trees(git_dir, tree_id, tree_id, tree_id, "ours", "theirs", &result) == 0,
         "sg_merge_trees failed");
    CHECK(sg_merge_result_apply(git_dir, repo_root, &result, &index_out, &conflict_paths,
                                &conflict_count) == 0,
         "sg_merge_result_apply failed");

    on_disk = read_workdir_file(repo_root, "c.txt");
    CHECK(on_disk != NULL && strcmp(on_disk, "DIRTY-UNCOMMITTED\n") == 0,
         "c.txt's dirty on-disk content must survive: an entry equal to ours must not be "
         "rewritten, got %s",
         on_disk != NULL ? on_disk : "(missing)");
    free(on_disk);

    pos = sg_index_find(&index_out, "c.txt");
    CHECK(pos >= 0 && memcmp(index_out.entries[pos].sha1, blob_c, SG_SHA1_RAW_LEN) == 0,
         "c.txt must still get a correct stage-0 index entry even though its on-disk content "
         "was left alone");

    sg_index_free(&index_out);
    sg_merge_result_free(&result);
    free(repo_root);
    free(git_dir);
}

/* A "deleted" entry that equals ours (ours never had the path at all) must
   NOT remove() anything -- verified by planting an untracked file with the
   same name and confirming it survives. This is the "base had it, ours AND
   theirs both already lack it" case: sg_merge_trees resolves it via the
   eq_ot ("both sides agree") branch as deleted, but since ours already
   lacked the path, sg_merge_entry_touches_ours must say "not touched". */
static void test_delete_equal_to_ours_does_not_remove_untracked(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char blob_d[SG_SHA1_RAW_LEN];
    sg_flat_entry base_entries[1];
    unsigned char base_tree[SG_SHA1_RAW_LEN];
    unsigned char empty_tree[SG_SHA1_RAW_LEN];
    sg_merge_result result;
    sg_index index_out;
    char **conflict_paths = NULL;
    size_t conflict_count = 0;
    char *on_disk;

    write_blob(git_dir, "d1\n", blob_d);
    base_entries[0].path = (char *)"d.txt";
    base_entries[0].mode = 0100644;
    memcpy(base_entries[0].sha1, blob_d, SG_SHA1_RAW_LEN);
    CHECK(sg_tree_build(git_dir, base_entries, 1, base_tree) == 0, "build base tree");
    CHECK(sg_tree_build(git_dir, NULL, 0, empty_tree) == 0, "build empty tree");

    /* An unrelated, never-versioned file happens to sit at the same path. */
    write_workdir_file(repo_root, "d.txt", "UNTRACKED-UNRELATED\n");

    /* base has d.txt; ours (empty_tree) and theirs (empty_tree) both lack
       it -- a delete/delete-from-base agreement, resolved clean, not a
       conflict. */
    CHECK(sg_merge_trees(git_dir, base_tree, empty_tree, empty_tree, "ours", "theirs", &result) == 0,
         "sg_merge_trees failed");
    CHECK(result.count == 1 && result.entries[0].deleted && !result.entries[0].conflict,
         "expected a single clean 'deleted' entry for d.txt");
    CHECK(!result.entries[0].ours_present,
         "precondition: ours must not have d.txt either, or this isn't testing the untouched case");

    CHECK(sg_merge_result_apply(git_dir, repo_root, &result, &index_out, &conflict_paths,
                                &conflict_count) == 0,
         "sg_merge_result_apply failed");

    CHECK(file_exists(repo_root, "d.txt"),
         "the untracked file at d.txt must survive: a delete outcome that equals ours (ours never "
         "had the path) must not remove() anything");
    on_disk = read_workdir_file(repo_root, "d.txt");
    CHECK(on_disk != NULL && strcmp(on_disk, "UNTRACKED-UNRELATED\n") == 0,
         "d.txt's untracked content must be untouched, got %s", on_disk != NULL ? on_disk : "(missing)");
    free(on_disk);

    CHECK(sg_index_find(&index_out, "d.txt") < 0,
         "d.txt must not be staged -- the merge result says it is deleted");

    sg_index_free(&index_out);
    sg_merge_result_free(&result);
    free(repo_root);
    free(git_dir);
}

int main(void)
{
    test_untouched_path_stays_in_index();
    test_equal_to_ours_not_rewritten();
    test_delete_equal_to_ours_does_not_remove_untracked();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all merge_result_apply tests passed\n");
    return 0;
}
