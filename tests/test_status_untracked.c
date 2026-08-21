#include "sg/status.h"

#include "sg/index.h"
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
    static char template[] = "/tmp/sg_status_untracked_test_XXXXXX";
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

static void mkdir_p(const char *repo_root, const char *rel)
{
    char abspath[4096];

    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, rel);
    CHECK(mkdir(abspath, 0755) == 0, "failed to mkdir %s", rel);
}

static int list_contains(char **list, size_t count, const char *path)
{
    size_t i;

    for (i = 0; i < count; i++) {
        if (strcmp(list[i], path) == 0)
            return 1;
    }
    return 0;
}

static void free_list(char **list, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++)
        free(list[i]);
    free(list);
}

/* include_ignored = 0 must skip files matched by .gitignore; = 1 must
   include them. Asserting only "the non-ignored file shows up" would miss an
   implementation that ignores nothing at all, so both directions are
   checked, and specifically that the ignored file is ABSENT under = 0. */
static void test_include_ignored_flag(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    char **untracked = NULL;
    size_t count = 0;

    write_workdir_file(repo_root, ".gitignore", "*.log\n");
    write_workdir_file(repo_root, "fresh.txt", "fresh\n");
    write_workdir_file(repo_root, "keep.log", "log\n");

    memset(&idx, 0, sizeof(idx));

    CHECK(sg_status_list_untracked(git_dir, repo_root, &idx, 0, SG_STATUS_UNTRACKED_LIST_FILES, &untracked, &count) == 0,
         "list_untracked (include_ignored=0) failed");
    CHECK(list_contains(untracked, count, "fresh.txt"), "fresh.txt missing under include_ignored=0");
    CHECK(list_contains(untracked, count, ".gitignore"),
         ".gitignore itself is untracked and unmatched, should appear");
    CHECK(!list_contains(untracked, count, "keep.log"),
         "keep.log must NOT appear under include_ignored=0 (it matches *.log)");
    free_list(untracked, count);

    untracked = NULL;
    count = 0;
    CHECK(sg_status_list_untracked(git_dir, repo_root, &idx, 1, SG_STATUS_UNTRACKED_LIST_FILES, &untracked, &count) == 0,
         "list_untracked (include_ignored=1) failed");
    CHECK(list_contains(untracked, count, "fresh.txt"), "fresh.txt missing under include_ignored=1");
    CHECK(list_contains(untracked, count, "keep.log"),
         "keep.log must appear under include_ignored=1");
    free_list(untracked, count);

    free(repo_root);
    free(git_dir);
}

/* Untracked files in nested directories must be reported with their full
   repo-root-relative path, never folded down to a basename. */
static void test_nested_dirs_use_full_path(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    char **untracked = NULL;
    size_t count = 0;

    mkdir_p(repo_root, "sub");
    mkdir_p(repo_root, "sub/dir");
    write_workdir_file(repo_root, "sub/dir/file.txt", "nested\n");

    memset(&idx, 0, sizeof(idx));
    CHECK(sg_status_list_untracked(git_dir, repo_root, &idx, 0, SG_STATUS_UNTRACKED_LIST_FILES, &untracked, &count) == 0,
         "list_untracked failed");
    CHECK(list_contains(untracked, count, "sub/dir/file.txt"),
         "nested untracked file must use full relative path, not basename");
    CHECK(!list_contains(untracked, count, "file.txt"),
         "nested untracked file must NOT be reported as a bare basename");
    free_list(untracked, count);

    free(repo_root);
    free(git_dir);
}

/* A path removed from the index while the file remains on disk (the
   "staged-delete" case: `sg rm --cached`) counts as untracked -- this falls
   naturally out of "not present in idx", no special-casing needed, but it's
   easy to get backwards, so pin it directly. */
static void test_staged_delete_counts_as_untracked(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    sg_index_entry entry;
    unsigned char blob_id[SG_SHA1_RAW_LEN];
    char **untracked = NULL;
    size_t count = 0;

    write_workdir_file(repo_root, "tracked_then_uncached.txt", "content\n");

    memset(&idx, 0, sizeof(idx));
    memset(&entry, 0, sizeof(entry));
    entry.path = strdup("tracked_then_uncached.txt");
    entry.mode = 0100644;
    entry.stage = 0;
    memset(blob_id, 0, sizeof(blob_id));
    memcpy(entry.sha1, blob_id, SG_SHA1_RAW_LEN);
    CHECK(sg_index_upsert(&idx, &entry) == 0, "upsert failed");
    free(entry.path);

    /* First: while it IS in the index, it must NOT show up as untracked. */
    CHECK(sg_status_list_untracked(git_dir, repo_root, &idx, 0, SG_STATUS_UNTRACKED_LIST_FILES, &untracked, &count) == 0,
         "list_untracked failed (tracked case)");
    CHECK(!list_contains(untracked, count, "tracked_then_uncached.txt"),
         "a file present in the index must not be reported as untracked");
    free_list(untracked, count);

    /* Now simulate `rm --cached`: drop it from the index, leave the file on
       disk. */
    CHECK(sg_index_remove(&idx, "tracked_then_uncached.txt") == 0, "index remove failed");

    untracked = NULL;
    count = 0;
    CHECK(sg_status_list_untracked(git_dir, repo_root, &idx, 0, SG_STATUS_UNTRACKED_LIST_FILES, &untracked, &count) == 0,
         "list_untracked failed (staged-delete case)");
    CHECK(list_contains(untracked, count, "tracked_then_uncached.txt"),
         "a file dropped from the index but still on disk must count as untracked");
    free_list(untracked, count);

    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* An untracked directory holding nothing (no files anywhere beneath it)
   contributes zero entries -- an empty directory is not itself a file. */
static void test_empty_dir_produces_no_entries(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    char **untracked = NULL;
    size_t count = 0;

    mkdir_p(repo_root, "emptydir");

    memset(&idx, 0, sizeof(idx));
    CHECK(sg_status_list_untracked(git_dir, repo_root, &idx, 0, SG_STATUS_UNTRACKED_LIST_FILES, &untracked, &count) == 0,
         "list_untracked failed");
    CHECK(count == 0, "an empty untracked directory must not produce any entries, got %zu", count);
    free_list(untracked, count);

    free(repo_root);
    free(git_dir);
}

/* .git's own metadata must never be walked or reported, at any depth. */
static void test_git_dir_never_appears(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    char **untracked = NULL;
    size_t count = 0;
    size_t i;

    memset(&idx, 0, sizeof(idx));
    CHECK(sg_status_list_untracked(git_dir, repo_root, &idx, 1, SG_STATUS_UNTRACKED_LIST_FILES, &untracked, &count) == 0,
         "list_untracked failed");
    for (i = 0; i < count; i++)
        CHECK(strncmp(untracked[i], ".git/", 5) != 0 && strcmp(untracked[i], ".git") != 0,
             "entry '%s' should never come from inside .git", untracked[i]);
    free_list(untracked, count);

    free(repo_root);
    free(git_dir);
}

/* When there are no untracked files, sg_tree_build_from_untracked must
   produce git's well-known empty tree id and report a zero file count --
   not fail, and not skip building a tree object at all. */
static void test_tree_build_from_untracked_empty(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];
    size_t file_count = 999;

    memset(&idx, 0, sizeof(idx));
    CHECK(sg_tree_build_from_untracked(git_dir, repo_root, &idx, 0, tree_id, &file_count) == 0,
         "tree build from untracked (empty) failed");
    CHECK(file_count == 0, "expected file_count 0 for no untracked files, got %zu", file_count);
    sg_sha1_to_hex(tree_id, hex);
    CHECK(strcmp(hex, "4b825dc642cb6eb9a060e54bf8d69288fbee4904") == 0,
         "empty untracked tree should hash to git's well-known empty tree id, got %s", hex);

    free(repo_root);
    free(git_dir);
}

/* With untracked files present, the resulting tree must actually contain
   them under their full relative path, hashed as real blobs -- not just
   report a plausible-looking count. */
static void test_tree_build_from_untracked_nonempty(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    size_t file_count = 0;
    sg_flat_list flat;

    mkdir_p(repo_root, "sub");
    write_workdir_file(repo_root, "top.txt", "top\n");
    write_workdir_file(repo_root, "sub/inner.txt", "inner\n");

    memset(&idx, 0, sizeof(idx));
    CHECK(sg_tree_build_from_untracked(git_dir, repo_root, &idx, 0, tree_id, &file_count) == 0,
         "tree build from untracked (nonempty) failed");
    CHECK(file_count == 2, "expected file_count 2, got %zu", file_count);

    CHECK(sg_tree_flatten(git_dir, tree_id, &flat, NULL) == 0, "flatten failed");
    CHECK(flat.count == 2, "expected 2 flattened entries, got %zu", flat.count);
    if (flat.count == 2) {
        CHECK(strcmp(flat.entries[0].path, "sub/inner.txt") == 0, "flat[0] path %s",
             flat.entries[0].path);
        CHECK(strcmp(flat.entries[1].path, "top.txt") == 0, "flat[1] path %s",
             flat.entries[1].path);
    }
    sg_flat_list_free(&flat);

    free(repo_root);
    free(git_dir);
}

/* sg_status_list_untracked walks the filesystem via readdir, whose order is
   unspecified and not lexicographic, so it sorts before returning. This is
   the ONLY guard on that sort: `sg status` prints the list verbatim (its own
   qsort was removed when this API took over the sorting), and
   sg_tree_build's flat-list contract (tree_build.h) requires path-sorted
   input from every caller. Files are created in deliberately
   reverse-lexicographic order, so a readdir-order (i.e. creation-order)
   result fails this -- verified by deleting the qsort, which reddens exactly
   these checks. */
static void test_list_untracked_is_sorted(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    char **untracked = NULL;
    size_t count = 0;
    size_t i;

    mkdir_p(repo_root, "sub");
    write_workdir_file(repo_root, "z.txt", "z\n");
    write_workdir_file(repo_root, "sub/b.txt", "sub-b\n");
    write_workdir_file(repo_root, "m.txt", "m\n");
    write_workdir_file(repo_root, "sub/a.txt", "sub-a\n");
    write_workdir_file(repo_root, "a.txt", "a\n");

    memset(&idx, 0, sizeof(idx));
    CHECK(sg_status_list_untracked(git_dir, repo_root, &idx, 0, SG_STATUS_UNTRACKED_LIST_FILES, &untracked, &count) == 0,
         "list_untracked failed");
    CHECK(count == 5, "expected 5 untracked files, got %zu", count);
    for (i = 0; i + 1 < count; i++) {
        CHECK(strcmp(untracked[i], untracked[i + 1]) < 0,
             "untracked list is not sorted by path: '%s' should come after '%s'", untracked[i],
             untracked[i + 1]);
    }
    free_list(untracked, count);

    free(repo_root);
    free(git_dir);
}

/* Checks the built tree against a reference built from the identical rows in
   sorted order -- paths, modes and blob ids all reach sg_tree_build intact.

   Do NOT read this as the regression test for the missing sort: deleting the
   qsort in sg_status_list_untracked leaves this check GREEN (measured). Two
   properties of the existing code cancel the unsortedness out, and neither
   is obvious: collect_untracked recurses inline, so one directory's entries
   are always contiguous in the output no matter what readdir returned, which
   is the only ordering build_level actually needs (it groups by adjacency);
   and sg_tree_serialize sorts each level itself (tree.c), so a permutation
   within a level cannot change the object. The sort is load-bearing for the
   contract and for `sg status`'s output order, not for this tree id --
   test_list_untracked_is_sorted is what guards it. */
static void test_tree_build_from_untracked_matches_sorted_reference(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    unsigned char reference_id[SG_SHA1_RAW_LEN];
    size_t file_count = 0;
    static const char *sorted_paths[] = {"a.txt", "m.txt", "sub/a.txt", "sub/b.txt", "z.txt"};
    sg_flat_entry flat[5];
    size_t i;
    char abspath[4096];

    mkdir_p(repo_root, "sub");
    write_workdir_file(repo_root, "z.txt", "z\n");
    write_workdir_file(repo_root, "sub/b.txt", "sub-b\n");
    write_workdir_file(repo_root, "m.txt", "m\n");
    write_workdir_file(repo_root, "sub/a.txt", "sub-a\n");
    write_workdir_file(repo_root, "a.txt", "a\n");

    memset(&idx, 0, sizeof(idx));
    CHECK(sg_tree_build_from_untracked(git_dir, repo_root, &idx, 0, tree_id, &file_count) == 0,
         "tree build from untracked failed");
    CHECK(file_count == 5, "expected file_count 5, got %zu", file_count);

    for (i = 0; i < 5; i++) {
        flat[i].path = strdup(sorted_paths[i]);
        flat[i].mode = 0100644;
        snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, sorted_paths[i]);
        CHECK(sg_hash_file_blob(abspath, flat[i].sha1) == 0, "failed to hash %s", sorted_paths[i]);
    }
    CHECK(sg_tree_build(git_dir, flat, 5, reference_id) == 0, "reference tree build failed");
    CHECK(memcmp(tree_id, reference_id, SG_SHA1_RAW_LEN) == 0,
         "sg_tree_build_from_untracked's tree must match the tree built from the same rows "
         "in sorted order -- an unsorted input would silently build a different (or in some "
         "layouts, differently-shaped) tree");

    for (i = 0; i < 5; i++)
        free(flat[i].path);

    free(repo_root);
    free(git_dir);
}

/* A directory that holds no tracked path anywhere below it, and at least
   one non-ignored file, folds into a single "dir/" line -- individual files
   inside it must NOT appear. */
static void test_fold_wholly_untracked_dir(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    char **untracked = NULL;
    size_t count = 0;

    mkdir_p(repo_root, "un");
    write_workdir_file(repo_root, "un/a.txt", "a\n");
    write_workdir_file(repo_root, "un/b.txt", "b\n");

    memset(&idx, 0, sizeof(idx));
    CHECK(sg_status_list_untracked(git_dir, repo_root, &idx, 0, SG_STATUS_UNTRACKED_FOLD_DIRS,
                                   &untracked, &count) == 0,
         "list_untracked (fold) failed");
    CHECK(count == 1, "expected exactly one folded entry, got %zu", count);
    CHECK(list_contains(untracked, count, "un/"), "expected folded entry 'un/'");
    CHECK(!list_contains(untracked, count, "un/a.txt"), "un/a.txt must not appear individually");
    CHECK(!list_contains(untracked, count, "un/b.txt"), "un/b.txt must not appear individually");
    free_list(untracked, count);

    free(repo_root);
    free(git_dir);
}

/* A directory that DOES hold a tracked path somewhere below it must be
   walked, not folded -- but a further-nested directory below it that is
   itself wholly untracked still gets its own independent fold. */
static void test_fold_is_per_directory_not_global(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    sg_index_entry entry;
    unsigned char blob_id[SG_SHA1_RAW_LEN];
    char **untracked = NULL;
    size_t count = 0;

    mkdir_p(repo_root, "hastracked");
    mkdir_p(repo_root, "hastracked/sub");
    write_workdir_file(repo_root, "hastracked/tracked.txt", "t\n");
    write_workdir_file(repo_root, "hastracked/sub/x.txt", "x\n");
    write_workdir_file(repo_root, "hastracked/sub/y.txt", "y\n");

    memset(&idx, 0, sizeof(idx));
    memset(&entry, 0, sizeof(entry));
    entry.path = strdup("hastracked/tracked.txt");
    entry.mode = 0100644;
    entry.stage = 0;
    memset(blob_id, 0, sizeof(blob_id));
    memcpy(entry.sha1, blob_id, SG_SHA1_RAW_LEN);
    CHECK(sg_index_upsert(&idx, &entry) == 0, "upsert failed");
    free(entry.path);

    CHECK(sg_status_list_untracked(git_dir, repo_root, &idx, 0, SG_STATUS_UNTRACKED_FOLD_DIRS,
                                   &untracked, &count) == 0,
         "list_untracked (fold) failed");
    CHECK(count == 1, "expected exactly one entry (the folded sub/), got %zu", count);
    CHECK(list_contains(untracked, count, "hastracked/sub/"),
         "hastracked/sub/ must be folded since it has no tracked descendant");
    CHECK(!list_contains(untracked, count, "hastracked/"),
         "hastracked/ itself must NOT be folded -- it has a tracked descendant");
    free_list(untracked, count);

    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* dir_has_tracked_descendant's prefix check must require the byte right
   after the shared text prefix to be '/', not just any byte at all -- a
   tracked path that is merely a lexical near-miss for the untracked
   directory "un/" must NOT be mistaken for a tracked descendant of it. The
   fixture specifically needs the near-miss's next byte to sort AFTER '/'
   (0x2F) in the index's own (path, stage) order: "unrelated.txt" ('r' =
   0x72 > '/') sorts to the right of the search key "un/" and so is a real
   candidate the binary search's lower_bound can land on, whereas something
   like "un-related.txt" ('-' = 0x2D < '/') sorts to the LEFT of "un/" and
   is never examined at all -- a fixture built from the latter would pass
   even with the boundary check completely missing (measured: it did, while
   this fixture catches it). If the trailing '/' is ever dropped from the
   comparison, "unrelated.txt" wrongly blocks "un/" from folding. */
static void test_fold_prefix_boundary_is_not_a_lexical_near_miss(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    sg_index_entry entry;
    unsigned char blob_id[SG_SHA1_RAW_LEN];
    char **untracked = NULL;
    size_t count = 0;

    mkdir_p(repo_root, "un");
    write_workdir_file(repo_root, "unrelated.txt", "t\n");
    write_workdir_file(repo_root, "un/a.txt", "a\n");

    memset(&idx, 0, sizeof(idx));
    memset(&entry, 0, sizeof(entry));
    entry.path = strdup("unrelated.txt");
    entry.mode = 0100644;
    entry.stage = 0;
    memset(blob_id, 0, sizeof(blob_id));
    memcpy(entry.sha1, blob_id, SG_SHA1_RAW_LEN);
    CHECK(sg_index_upsert(&idx, &entry) == 0, "upsert failed");
    free(entry.path);

    CHECK(sg_status_list_untracked(git_dir, repo_root, &idx, 0, SG_STATUS_UNTRACKED_FOLD_DIRS,
                                   &untracked, &count) == 0,
         "list_untracked (fold) failed");
    CHECK(list_contains(untracked, count, "un/"),
         "un/ must still fold -- 'unrelated.txt' is a lexical near-miss, not a tracked "
         "descendant of un/");
    CHECK(!list_contains(untracked, count, "un/a.txt"),
         "un/a.txt must not be listed individually once un/ folds");
    free_list(untracked, count);

    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
}

/* A folded directory that mixes non-ignored and ignored files: default
   (include_ignored=0) shows only the folded "mixed/" line; include_ignored=1
   ADDITIONALLY lists the ignored file individually alongside the fold. */
static void test_fold_mixed_dir_lists_ignored_individually(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    char **untracked = NULL;
    size_t count = 0;

    write_workdir_file(repo_root, ".gitignore", "*.log\n");
    mkdir_p(repo_root, "mixed");
    write_workdir_file(repo_root, "mixed/keep.txt", "keep\n");
    write_workdir_file(repo_root, "mixed/x.log", "log\n");

    memset(&idx, 0, sizeof(idx));

    CHECK(sg_status_list_untracked(git_dir, repo_root, &idx, 0, SG_STATUS_UNTRACKED_FOLD_DIRS,
                                   &untracked, &count) == 0,
         "list_untracked (fold, include_ignored=0) failed");
    CHECK(count == 2, "expected 'mixed/' and '.gitignore', got %zu", count);
    CHECK(list_contains(untracked, count, "mixed/"), "expected folded entry 'mixed/'");
    CHECK(!list_contains(untracked, count, "mixed/x.log"),
         "ignored file must not appear under include_ignored=0");
    free_list(untracked, count);

    untracked = NULL;
    count = 0;
    CHECK(sg_status_list_untracked(git_dir, repo_root, &idx, 1, SG_STATUS_UNTRACKED_FOLD_DIRS,
                                   &untracked, &count) == 0,
         "list_untracked (fold, include_ignored=1) failed");
    CHECK(list_contains(untracked, count, "mixed/"),
         "'mixed/' must still be folded under include_ignored=1");
    CHECK(list_contains(untracked, count, "mixed/x.log"),
         "the ignored file must ALSO be listed individually under include_ignored=1");
    free_list(untracked, count);

    free(repo_root);
    free(git_dir);
}

/* A folded directory whose files are ALL ignored: omitted entirely by
   default; folded to a single "dir/" line (no individual files) under
   include_ignored=1. */
static void test_fold_all_ignored_dir(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    char **untracked = NULL;
    size_t count = 0;

    write_workdir_file(repo_root, ".gitignore", "*.log\n");
    mkdir_p(repo_root, "allignored");
    write_workdir_file(repo_root, "allignored/a.log", "a\n");
    write_workdir_file(repo_root, "allignored/b.log", "b\n");

    memset(&idx, 0, sizeof(idx));

    CHECK(sg_status_list_untracked(git_dir, repo_root, &idx, 0, SG_STATUS_UNTRACKED_FOLD_DIRS,
                                   &untracked, &count) == 0,
         "list_untracked (fold, include_ignored=0) failed");
    CHECK(!list_contains(untracked, count, "allignored/"),
         "a wholly-ignored directory must not appear at all under include_ignored=0");
    CHECK(!list_contains(untracked, count, "allignored/a.log"),
         "no individual file from a wholly-ignored directory should appear either");
    free_list(untracked, count);

    untracked = NULL;
    count = 0;
    CHECK(sg_status_list_untracked(git_dir, repo_root, &idx, 1, SG_STATUS_UNTRACKED_FOLD_DIRS,
                                   &untracked, &count) == 0,
         "list_untracked (fold, include_ignored=1) failed");
    CHECK(list_contains(untracked, count, "allignored/"),
         "a wholly-ignored directory must fold to one 'dir/' line under include_ignored=1");
    CHECK(!list_contains(untracked, count, "allignored/a.log"),
         "a wholly-ignored directory's files must NOT be listed individually, only folded");
    free_list(untracked, count);

    free(repo_root);
    free(git_dir);
}

/* An untracked directory with nothing under it at all (recursively) must
   not appear even when folding is enabled and include_ignored is set --
   there is nothing to fold. */
static void test_fold_empty_dir_produces_no_entries(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    char **untracked = NULL;
    size_t count = 0;

    mkdir_p(repo_root, "emptydir");

    memset(&idx, 0, sizeof(idx));
    CHECK(sg_status_list_untracked(git_dir, repo_root, &idx, 1, SG_STATUS_UNTRACKED_FOLD_DIRS,
                                   &untracked, &count) == 0,
         "list_untracked (fold) failed");
    CHECK(count == 0, "an empty untracked directory must not produce any entries, got %zu", count);
    free_list(untracked, count);

    free(repo_root);
    free(git_dir);
}

int main(void)
{
    test_include_ignored_flag();
    test_nested_dirs_use_full_path();
    test_staged_delete_counts_as_untracked();
    test_empty_dir_produces_no_entries();
    test_git_dir_never_appears();
    test_list_untracked_is_sorted();
    test_tree_build_from_untracked_empty();
    test_tree_build_from_untracked_nonempty();
    test_tree_build_from_untracked_matches_sorted_reference();
    test_fold_wholly_untracked_dir();
    test_fold_is_per_directory_not_global();
    test_fold_prefix_boundary_is_not_a_lexical_near_miss();
    test_fold_mixed_dir_lists_ignored_individually();
    test_fold_all_ignored_dir();
    test_fold_empty_dir_produces_no_entries();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all status_untracked tests passed\n");
    return 0;
}
