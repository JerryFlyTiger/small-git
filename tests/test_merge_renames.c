/* Phase 49: rename detection in the three-way merge. Each test builds base/
   ours/theirs trees directly (no working directory, no sg_diff_index -- just
   sg_tree_build over hand-picked blobs) and calls sg_merge_trees, checking
   the resulting sg_merge_result shape against docs/DESIGN.md Phase 49's
   measured tables (sec 3.1-3.6). */

#include "sg/merge.h"

#include "sg/index.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/repo.h"
#include "sg/similarity.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

#include <errno.h>
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
    static char template[] = "/tmp/sg_merge_renames_test_XXXXXX";
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

/* git_dir is always "<repo_root>/.git" (make_tmp_repo's own convention);
   strips the suffix back off rather than tracking a second string. */
static char *repo_root_of(const char *git_dir)
{
    char *root = strdup(git_dir);
    size_t len = strlen(root);

    if (len > 5 && strcmp(root + len - 5, "/.git") == 0)
        root[len - 5] = '\0';
    return root;
}

static int file_exists(const char *repo_root, const char *rel)
{
    char abspath[4096];
    struct stat st;

    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, rel);
    return stat(abspath, &st) == 0;
}

static const sg_merge_result_entry *find_entry(const sg_merge_result *r, const char *path)
{
    size_t i;

    for (i = 0; i < r->count; i++) {
        if (strcmp(r->entries[i].path, path) == 0)
            return &r->entries[i];
    }
    return NULL;
}

static int contains(const unsigned char *hay, size_t hay_len, const char *needle)
{
    size_t needle_len = strlen(needle);
    size_t i;

    if (needle_len == 0 || needle_len > hay_len)
        return 0;
    for (i = 0; i + needle_len <= hay_len; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0)
            return 1;
    }
    return 0;
}

/* ---- 3.1: one-sided rename, other side still has the path ---- */

/* clean sub-case: ours renames a.txt -> b.txt with no edit; theirs edits
   a.txt in place. Expect a single clean entry at b.txt carrying theirs'
   edited content, and no entry at all for a.txt. */
static void test_rename_clean_other_side_edits(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char base_tree[SG_SHA1_RAW_LEN], ours_tree[SG_SHA1_RAW_LEN], theirs_tree[SG_SHA1_RAW_LEN];
    unsigned char theirs_blob[SG_SHA1_RAW_LEN];
    tree_spec base_specs[] = {{"a.txt", "line1\nline2\nline3\n"}};
    tree_spec ours_specs[] = {{"b.txt", "line1\nline2\nline3\n"}};
    tree_spec theirs_specs[] = {{"a.txt", "line1\nline2\nEDITED\n"}};
    sg_merge_result result;
    const sg_merge_result_entry *e;

    build_tree(git_dir, base_specs, 1, base_tree);
    build_tree(git_dir, ours_specs, 1, ours_tree);
    build_tree(git_dir, theirs_specs, 1, theirs_tree);
    blob(git_dir, "line1\nline2\nEDITED\n", theirs_blob);

    CHECK(sg_merge_trees(git_dir, base_tree, ours_tree, theirs_tree, "ours", "theirs",
                        SG_SIMILARITY_DEFAULT, &result) == 0,
         "sg_merge_trees failed");
    CHECK(find_entry(&result, "a.txt") == NULL, "a.txt (rename source) must have no index entry");
    e = find_entry(&result, "b.txt");
    CHECK(e != NULL, "expected an entry at b.txt");
    if (e != NULL) {
        CHECK(!e->conflict, "expected a clean landing, not a conflict");
        CHECK(!e->deleted, "landing must not be deleted");
        CHECK(memcmp(e->sha1, theirs_blob, SG_SHA1_RAW_LEN) == 0,
             "b.txt should carry theirs' edited content");
    }
    sg_merge_result_free(&result);
    free(git_dir);
}

/* conflict sub-case: ours renames a.txt -> b.txt AND edits it; theirs edits
   a.txt differently. Expect stages 1/2/3 all at b.txt, working-tree bytes
   carrying 7-char markers labelled "ours:b.txt" / "theirs:a.txt". */
static void test_rename_conflict_other_side_edits(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char base_tree[SG_SHA1_RAW_LEN], ours_tree[SG_SHA1_RAW_LEN], theirs_tree[SG_SHA1_RAW_LEN];
    unsigned char base_blob[SG_SHA1_RAW_LEN], ours_blob[SG_SHA1_RAW_LEN], theirs_blob[SG_SHA1_RAW_LEN];
    tree_spec base_specs[] = {{"a.txt", "line1\nline2\nline3\nline4\nline5\n"}};
    tree_spec ours_specs[] = {{"b.txt", "line1\nOURS\nline3\nline4\nline5\n"}};
    tree_spec theirs_specs[] = {{"a.txt", "line1\nTHEIRS\nline3\nline4\nline5\n"}};
    sg_merge_result result;
    const sg_merge_result_entry *e;

    build_tree(git_dir, base_specs, 1, base_tree);
    build_tree(git_dir, ours_specs, 1, ours_tree);
    build_tree(git_dir, theirs_specs, 1, theirs_tree);
    blob(git_dir, "line1\nline2\nline3\nline4\nline5\n", base_blob);
    blob(git_dir, "line1\nOURS\nline3\nline4\nline5\n", ours_blob);
    blob(git_dir, "line1\nTHEIRS\nline3\nline4\nline5\n", theirs_blob);

    CHECK(sg_merge_trees(git_dir, base_tree, ours_tree, theirs_tree, "ours", "theirs",
                        SG_SIMILARITY_DEFAULT, &result) == 0,
         "sg_merge_trees failed");
    CHECK(find_entry(&result, "a.txt") == NULL, "a.txt (rename source) must have no index entry");
    e = find_entry(&result, "b.txt");
    CHECK(e != NULL, "expected a conflict entry at b.txt");
    if (e != NULL) {
        CHECK(e->conflict, "expected a conflict");
        CHECK(e->base_present && memcmp(e->base_sha1, base_blob, SG_SHA1_RAW_LEN) == 0,
             "stage 1 should be base's original blob");
        CHECK(e->ours_present && memcmp(e->ours_sha1, ours_blob, SG_SHA1_RAW_LEN) == 0,
             "stage 2 should be ours' b.txt blob");
        CHECK(e->theirs_present && memcmp(e->theirs_sha1, theirs_blob, SG_SHA1_RAW_LEN) == 0,
             "stage 3 should be theirs' a.txt blob");
        CHECK(contains(e->conflict_content, e->conflict_content_len, "<<<<<<< ours:b.txt\n"),
             "expected suffixed ours label, got '%.*s'", (int)e->conflict_content_len,
             (const char *)e->conflict_content);
        CHECK(contains(e->conflict_content, e->conflict_content_len, ">>>>>>> theirs:a.txt\n"),
             "expected suffixed theirs label");
        CHECK(!contains(e->conflict_content, e->conflict_content_len, "<<<<<<<<"),
             "marker must be exactly 7 characters wide, not 8");
    }
    sg_merge_result_free(&result);
    free(git_dir);
}

/* ---- 3.2: one-sided rename, other side deleted the path ---- */

static void test_rename_delete(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char base_tree[SG_SHA1_RAW_LEN], ours_tree[SG_SHA1_RAW_LEN], theirs_tree[SG_SHA1_RAW_LEN];
    unsigned char base_blob[SG_SHA1_RAW_LEN], ours_blob[SG_SHA1_RAW_LEN];
    tree_spec base_specs[] = {{"a.txt", "hello\nworld\n"}};
    tree_spec ours_specs[] = {{"b.txt", "hello\nworld\nmore\n"}};
    sg_merge_result result;
    const sg_merge_result_entry *e;

    build_tree(git_dir, base_specs, 1, base_tree);
    build_tree(git_dir, ours_specs, 1, ours_tree);
    build_tree(git_dir, NULL, 0, theirs_tree); /* theirs deletes a.txt outright */
    blob(git_dir, "hello\nworld\n", base_blob);
    blob(git_dir, "hello\nworld\nmore\n", ours_blob);

    CHECK(sg_merge_trees(git_dir, base_tree, ours_tree, theirs_tree, "ours", "theirs",
                        SG_SIMILARITY_DEFAULT, &result) == 0,
         "sg_merge_trees failed");
    CHECK(find_entry(&result, "a.txt") == NULL, "a.txt must have no index entry");
    e = find_entry(&result, "b.txt");
    CHECK(e != NULL, "expected a rename/delete conflict at b.txt");
    if (e != NULL) {
        CHECK(e->conflict, "rename/delete is always a conflict");
        CHECK(e->base_present && memcmp(e->base_sha1, base_blob, SG_SHA1_RAW_LEN) == 0,
             "stage 1 should be base's blob");
        CHECK(e->ours_present && memcmp(e->ours_sha1, ours_blob, SG_SHA1_RAW_LEN) == 0,
             "stage 2 should be ours' b.txt blob");
        CHECK(!e->theirs_present, "no stage 3 -- theirs deleted the source");
        CHECK(e->conflict_content_len == strlen("hello\nworld\nmore\n") &&
                 memcmp(e->conflict_content, "hello\nworld\nmore\n", e->conflict_content_len) == 0,
             "working tree must be ours' content verbatim, no markers");
    }
    sg_merge_result_free(&result);
    free(git_dir);
}

/* ---- 3.3: both sides renamed p to the SAME name ---- */

static void test_rename_rename_same_destination(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char base_tree[SG_SHA1_RAW_LEN], ours_tree[SG_SHA1_RAW_LEN], theirs_tree[SG_SHA1_RAW_LEN];
    unsigned char base_blob[SG_SHA1_RAW_LEN], ours_blob[SG_SHA1_RAW_LEN], theirs_blob[SG_SHA1_RAW_LEN];
    tree_spec base_specs[] = {{"a.txt", "line1\nline2\nline3\n"}};
    tree_spec ours_specs[] = {{"c.txt", "line1\nOURS\nline3\n"}};
    tree_spec theirs_specs[] = {{"c.txt", "line1\nTHEIRS\nline3\n"}};
    sg_merge_result result;
    const sg_merge_result_entry *e;

    build_tree(git_dir, base_specs, 1, base_tree);
    build_tree(git_dir, ours_specs, 1, ours_tree);
    build_tree(git_dir, theirs_specs, 1, theirs_tree);
    blob(git_dir, "line1\nline2\nline3\n", base_blob);
    blob(git_dir, "line1\nOURS\nline3\n", ours_blob);
    blob(git_dir, "line1\nTHEIRS\nline3\n", theirs_blob);

    CHECK(sg_merge_trees(git_dir, base_tree, ours_tree, theirs_tree, "ours", "theirs",
                        SG_SIMILARITY_DEFAULT, &result) == 0,
         "sg_merge_trees failed");
    CHECK(find_entry(&result, "a.txt") == NULL, "a.txt must have no index entry");
    CHECK(result.count == 1, "expected exactly one entry (at the shared destination), got %zu",
         result.count);
    e = find_entry(&result, "c.txt");
    CHECK(e != NULL, "expected a single conflict entry at c.txt");
    if (e != NULL) {
        CHECK(e->conflict, "expected a conflict");
        CHECK(e->base_present && memcmp(e->base_sha1, base_blob, SG_SHA1_RAW_LEN) == 0,
             "stage 1 should be base's blob relocated to c.txt");
        CHECK(e->ours_present && memcmp(e->ours_sha1, ours_blob, SG_SHA1_RAW_LEN) == 0,
             "stage 2 should be ours' c.txt blob");
        CHECK(e->theirs_present && memcmp(e->theirs_sha1, theirs_blob, SG_SHA1_RAW_LEN) == 0,
             "stage 3 should be theirs' c.txt blob");
        CHECK(contains(e->conflict_content, e->conflict_content_len, "<<<<<<< ours\n"),
             "no suffix expected -- both sides' own path is c.txt");
        CHECK(!contains(e->conflict_content, e->conflict_content_len, ":c.txt"),
             "no :path suffix expected when both sides land at the same name");
    }
    sg_merge_result_free(&result);
    free(git_dir);
}

/* ---- 3.4: both sides renamed p to DIFFERENT names (rename/rename 1to2) ---- */

static void test_rename_rename_1to2(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char base_tree[SG_SHA1_RAW_LEN], ours_tree[SG_SHA1_RAW_LEN], theirs_tree[SG_SHA1_RAW_LEN];
    unsigned char base_blob[SG_SHA1_RAW_LEN];
    tree_spec base_specs[] = {{"a.txt", "line1\nline2\nline3\nline4\nline5\n"}};
    tree_spec ours_specs[] = {{"b.txt", "line1\nOURS\nline3\nline4\nline5\n"}};
    tree_spec theirs_specs[] = {{"c.txt", "line1\nTHEIRS\nline3\nline4\nline5\n"}};
    sg_merge_result result;
    const sg_merge_result_entry *ep, *eo, *et;

    build_tree(git_dir, base_specs, 1, base_tree);
    build_tree(git_dir, ours_specs, 1, ours_tree);
    build_tree(git_dir, theirs_specs, 1, theirs_tree);
    blob(git_dir, "line1\nline2\nline3\nline4\nline5\n", base_blob);

    CHECK(sg_merge_trees(git_dir, base_tree, ours_tree, theirs_tree, "ours", "theirs",
                        SG_SIMILARITY_DEFAULT, &result) == 0,
         "sg_merge_trees failed");
    CHECK(result.count == 3, "expected 3 entries (p, o_dst, t_dst), got %zu", result.count);
    ep = find_entry(&result, "a.txt");
    eo = find_entry(&result, "b.txt");
    et = find_entry(&result, "c.txt");
    CHECK(ep != NULL && eo != NULL && et != NULL, "expected entries at a.txt, b.txt and c.txt");
    if (ep != NULL) {
        CHECK(ep->conflict, "a.txt should be an unresolved stage-1-only entry");
        CHECK(ep->conflict_no_workdir_file, "a.txt must have no working-tree file");
        CHECK(ep->base_present && !ep->ours_present && !ep->theirs_present,
             "a.txt should carry only stage 1");
        CHECK(memcmp(ep->base_sha1, base_blob, SG_SHA1_RAW_LEN) == 0,
             "a.txt's stage 1 should be base's own blob");
    }
    if (eo != NULL && et != NULL) {
        CHECK(eo->conflict && et->conflict, "b.txt and c.txt should both be conflicts");
        CHECK(!eo->base_present && eo->ours_present && !eo->theirs_present,
             "b.txt should carry only stage 2");
        CHECK(!et->base_present && !et->ours_present && et->theirs_present,
             "c.txt should carry only stage 3");
        CHECK(memcmp(eo->ours_sha1, et->theirs_sha1, SG_SHA1_RAW_LEN) == 0,
             "b.txt's stage 2 and c.txt's stage 3 must be the SAME blob");
        CHECK(eo->conflict_content != NULL &&
                 contains(eo->conflict_content, eo->conflict_content_len, "<<<<<<<<"),
             "marker size must be 8 for rename/rename-1to2's inner merge");
        CHECK(contains(eo->conflict_content, eo->conflict_content_len, "ours:b.txt"),
             "expected suffixed ours label");
        CHECK(contains(eo->conflict_content, eo->conflict_content_len, "theirs:c.txt"),
             "expected suffixed theirs label");
        CHECK(et->conflict_content_len == eo->conflict_content_len &&
                 memcmp(et->conflict_content, eo->conflict_content, eo->conflict_content_len) == 0,
             "b.txt and c.txt must carry the identical working-tree bytes");
    }
    sg_merge_result_free(&result);
    free(git_dir);
}

/* Same shape, but the inner three-way merge itself comes out CLEAN (no
   marker text at all -- both sides only renamed, neither edited). Still an
   unresolved conflict at the index level (see the function's own comment),
   but there is no earlier test that checks the working-tree bytes are the
   real merged content and not an accidentally-empty file -- exactly the
   shape a premature free() of the clean bytes would produce while every
   other assertion above (which all use content that conflicts) stays
   green. */
static void test_rename_rename_1to2_clean_inner_merge(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char base_tree[SG_SHA1_RAW_LEN], ours_tree[SG_SHA1_RAW_LEN], theirs_tree[SG_SHA1_RAW_LEN];
    tree_spec base_specs[] = {{"a.txt", "line1\nline2\nline3\n"}};
    tree_spec ours_specs[] = {{"b.txt", "line1\nline2\nline3\n"}};
    tree_spec theirs_specs[] = {{"c.txt", "line1\nline2\nline3\n"}};
    sg_merge_result result;
    const sg_merge_result_entry *eo, *et;

    build_tree(git_dir, base_specs, 1, base_tree);
    build_tree(git_dir, ours_specs, 1, ours_tree);
    build_tree(git_dir, theirs_specs, 1, theirs_tree);

    CHECK(sg_merge_trees(git_dir, base_tree, ours_tree, theirs_tree, "ours", "theirs",
                        SG_SIMILARITY_DEFAULT, &result) == 0,
         "sg_merge_trees failed");
    CHECK(result.count == 3, "expected 3 entries, got %zu", result.count);
    eo = find_entry(&result, "b.txt");
    et = find_entry(&result, "c.txt");
    CHECK(eo != NULL && et != NULL, "expected entries at b.txt and c.txt");
    if (eo != NULL && et != NULL) {
        CHECK(eo->conflict_content_len == strlen("line1\nline2\nline3\n") &&
                 memcmp(eo->conflict_content, "line1\nline2\nline3\n", eo->conflict_content_len) == 0,
             "b.txt's working-tree bytes must be the real (unmarked) merged content, got %zu bytes",
             eo->conflict_content_len);
        CHECK(et->conflict_content_len == eo->conflict_content_len &&
                 memcmp(et->conflict_content, eo->conflict_content, eo->conflict_content_len) == 0,
             "c.txt must carry the identical bytes");
        CHECK(!contains(eo->conflict_content, eo->conflict_content_len, "<<<<<<<"),
             "a clean inner merge must not contain marker text");
    }
    sg_merge_result_free(&result);
    free(git_dir);
}

/* sg_merge_result_apply's own handling of conflict_no_workdir_file (sec 1.3
   of the spec): the rename/rename-1to2 source path must end up with NO file
   on disk at all after materialization, while its two destinations DO get
   files, with matching content. This is the one place the field is actually
   consumed -- test_rename_rename_1to2 above only checks the sg_merge_result
   shape sg_merge_trees hands back, never sg_merge_result_apply's own branch
   for it, so a bug there (e.g. writing 0 bytes to the old path instead of
   skipping it) would go uncaught by that test alone. */
static void test_rename_rename_1to2_apply_leaves_no_file_at_source(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = repo_root_of(git_dir);
    unsigned char base_tree[SG_SHA1_RAW_LEN], ours_tree[SG_SHA1_RAW_LEN], theirs_tree[SG_SHA1_RAW_LEN];
    tree_spec base_specs[] = {{"a.txt", "line1\nline2\nline3\nline4\nline5\n"}};
    tree_spec ours_specs[] = {{"b.txt", "line1\nOURS\nline3\nline4\nline5\n"}};
    tree_spec theirs_specs[] = {{"c.txt", "line1\nTHEIRS\nline3\nline4\nline5\n"}};
    sg_merge_result result;
    sg_index new_idx;
    char **conflict_paths = NULL;
    size_t conflict_count = 0;

    build_tree(git_dir, base_specs, 1, base_tree);
    build_tree(git_dir, ours_specs, 1, ours_tree);
    build_tree(git_dir, theirs_specs, 1, theirs_tree);

    /* Plant a stale a.txt before apply -- without this, "the file doesn't
       exist" would be true whether or not the code actively removes it,
       since nothing ever put one there in the first place. Planting one
       first makes the assertion below distinguish "correctly removed" from
       "conflict_no_workdir_file silently ignored, nothing touched a.txt". */
    {
        char abspath[4096];

        snprintf(abspath, sizeof(abspath), "%s/a.txt", repo_root);
        CHECK(sg_write_file_mkdirs(abspath, (const unsigned char *)"stale", 5, 0644) == 0,
             "failed to plant stale a.txt");
    }

    CHECK(sg_merge_trees(git_dir, base_tree, ours_tree, theirs_tree, "ours", "theirs",
                        SG_SIMILARITY_DEFAULT, &result) == 0,
         "sg_merge_trees failed");
    CHECK(sg_merge_result_apply(git_dir, repo_root, &result, &new_idx, &conflict_paths,
                               &conflict_count) == 0,
         "sg_merge_result_apply failed");
    CHECK(!file_exists(repo_root, "a.txt"),
         "the rename source's stale file must be actively removed, not left in place");
    CHECK(file_exists(repo_root, "b.txt"), "b.txt should exist");
    CHECK(file_exists(repo_root, "c.txt"), "c.txt should exist");
    /* Regression guard: the conflicting sub-case of rename/rename-1to2 used
       to leave merged_mode at its zero-initialized value (merge_blob_content
       only fills *out_mode on its CLEAN path), so both files landed with
       mode 000 -- readable only as root. Caught by this repo running as a
       normal user: sg_read_file below failed with EACCES, not a wrong-bytes
       mismatch, so the mode is checked directly too. */
    {
        char abspath[4096];
        struct stat st;

        snprintf(abspath, sizeof(abspath), "%s/b.txt", repo_root);
        CHECK(stat(abspath, &st) == 0 && (st.st_mode & 0777) == 0644,
             "b.txt should be mode 0644, got 0%o", (unsigned)(st.st_mode & 0777));
        snprintf(abspath, sizeof(abspath), "%s/c.txt", repo_root);
        CHECK(stat(abspath, &st) == 0 && (st.st_mode & 0777) == 0644,
             "c.txt should be mode 0644, got 0%o", (unsigned)(st.st_mode & 0777));
    }
    if (file_exists(repo_root, "b.txt") && file_exists(repo_root, "c.txt")) {
        unsigned char *b_content = NULL, *c_content = NULL;
        size_t b_len = 0, c_len = 0;
        char abspath[4096];

        snprintf(abspath, sizeof(abspath), "%s/b.txt", repo_root);
        CHECK(sg_read_file(abspath, &b_content, &b_len) == 0, "failed to read b.txt (errno=%d %s)",
             errno, strerror(errno));
        snprintf(abspath, sizeof(abspath), "%s/c.txt", repo_root);
        CHECK(sg_read_file(abspath, &c_content, &c_len) == 0, "failed to read c.txt (errno=%d %s)",
             errno, strerror(errno));
        if (b_content != NULL && c_content != NULL) {
            CHECK(b_len == c_len && memcmp(b_content, c_content, b_len) == 0,
                 "b.txt and c.txt should carry identical bytes on disk");
        }
        free(b_content);
        free(c_content);
    }
    CHECK(conflict_count == 3, "expected 3 conflicted paths (a.txt, b.txt, c.txt), got %zu",
         conflict_count);
    {
        size_t i;

        for (i = 0; i < conflict_count; i++)
            free(conflict_paths[i]);
        free(conflict_paths);
    }
    sg_index_free(&new_idx);
    sg_merge_result_free(&result);
    free(repo_root);
    free(git_dir);
}

/* ---- 3.5: a rename destination collides with an addition (rename/add) ---- */

static void test_rename_add_collision(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char base_tree[SG_SHA1_RAW_LEN], ours_tree[SG_SHA1_RAW_LEN], theirs_tree[SG_SHA1_RAW_LEN];
    unsigned char ours_blob[SG_SHA1_RAW_LEN], theirs_blob[SG_SHA1_RAW_LEN];
    tree_spec base_specs[] = {{"a.txt", "hello\nworld\n"}};
    tree_spec ours_specs[] = {{"x.txt", "hello\nworld\nOURS\n"}};
    tree_spec theirs_specs[] = {{"x.txt", "brand\nnew\nfile\n"}};
    sg_merge_result result;
    const sg_merge_result_entry *e;

    build_tree(git_dir, base_specs, 1, base_tree);
    build_tree(git_dir, ours_specs, 1, ours_tree);
    build_tree(git_dir, theirs_specs, 1, theirs_tree); /* deletes a.txt, adds x.txt fresh */
    blob(git_dir, "hello\nworld\nOURS\n", ours_blob);
    blob(git_dir, "brand\nnew\nfile\n", theirs_blob);

    CHECK(sg_merge_trees(git_dir, base_tree, ours_tree, theirs_tree, "ours", "theirs",
                        SG_SIMILARITY_DEFAULT, &result) == 0,
         "sg_merge_trees failed");
    CHECK(find_entry(&result, "a.txt") == NULL, "a.txt must have no index entry");
    CHECK(result.count == 1, "expected exactly one entry (at x.txt), got %zu", result.count);
    e = find_entry(&result, "x.txt");
    CHECK(e != NULL, "expected an add/add conflict at x.txt");
    if (e != NULL) {
        CHECK(e->conflict, "expected a conflict");
        CHECK(!e->base_present, "no stage 1 for an add/add collision");
        CHECK(e->ours_present && memcmp(e->ours_sha1, ours_blob, SG_SHA1_RAW_LEN) == 0,
             "stage 2 should be ours' landing content verbatim (theirs deleted the source)");
        CHECK(e->theirs_present && memcmp(e->theirs_sha1, theirs_blob, SG_SHA1_RAW_LEN) == 0,
             "stage 3 should be theirs' plain addition");
        CHECK(contains(e->conflict_content, e->conflict_content_len, "<<<<<<< ours\n"),
             "no suffix expected for the add/add collision");
        CHECK(!contains(e->conflict_content, e->conflict_content_len, "<<<<<<<<"),
             "outer marker size must be 7, not 8");
    }
    sg_merge_result_free(&result);
    free(git_dir);
}

/* ---- 3.6: two different sources renamed to the SAME destination ---- */

static void test_rename_rename_2to1(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char base_tree[SG_SHA1_RAW_LEN], ours_tree[SG_SHA1_RAW_LEN], theirs_tree[SG_SHA1_RAW_LEN];
    unsigned char ours_blob[SG_SHA1_RAW_LEN], theirs_blob[SG_SHA1_RAW_LEN];
    tree_spec base_specs[] = {{"a.txt", "alpha\nblob\ncontent\n"}, {"q.txt", "quebec\nblob\ncontent\n"}};
    tree_spec ours_specs[] = {{"z.txt", "alpha\nblob\ncontent\nOURS\n"}};
    tree_spec theirs_specs[] = {{"z.txt", "quebec\nblob\ncontent\nTHEIRS\n"}};
    sg_merge_result result;
    const sg_merge_result_entry *e;

    build_tree(git_dir, base_specs, 2, base_tree);
    build_tree(git_dir, ours_specs, 1, ours_tree);     /* renames a.txt->z.txt, deletes q.txt */
    build_tree(git_dir, theirs_specs, 1, theirs_tree); /* renames q.txt->z.txt, deletes a.txt */
    blob(git_dir, "alpha\nblob\ncontent\nOURS\n", ours_blob);
    blob(git_dir, "quebec\nblob\ncontent\nTHEIRS\n", theirs_blob);

    CHECK(sg_merge_trees(git_dir, base_tree, ours_tree, theirs_tree, "ours", "theirs",
                        SG_SIMILARITY_DEFAULT, &result) == 0,
         "sg_merge_trees failed");
    CHECK(find_entry(&result, "a.txt") == NULL, "a.txt must have no index entry");
    CHECK(find_entry(&result, "q.txt") == NULL, "q.txt must have no index entry");
    CHECK(result.count == 1, "expected exactly one entry (at z.txt), got %zu", result.count);
    e = find_entry(&result, "z.txt");
    CHECK(e != NULL, "expected an add/add conflict at z.txt");
    if (e != NULL) {
        CHECK(e->conflict, "expected a conflict");
        CHECK(!e->base_present, "no stage 1 for a rename/rename-2to1 collision");
        CHECK(e->ours_present && memcmp(e->ours_sha1, ours_blob, SG_SHA1_RAW_LEN) == 0,
             "stage 2 should be ours' full landing content");
        CHECK(e->theirs_present && memcmp(e->theirs_sha1, theirs_blob, SG_SHA1_RAW_LEN) == 0,
             "stage 3 should be theirs' full landing content");
        CHECK(!contains(e->conflict_content, e->conflict_content_len, ":z.txt"),
             "no :path suffix expected for a 2-to-1 collision");
    }
    sg_merge_result_free(&result);
    free(git_dir);
}

/* ---- control: rename_score == 0 reproduces pre-Phase-49 behaviour ---- */

static void test_rename_score_zero_is_unaffected(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char base_tree[SG_SHA1_RAW_LEN], ours_tree[SG_SHA1_RAW_LEN], theirs_tree[SG_SHA1_RAW_LEN];
    unsigned char ours_blob[SG_SHA1_RAW_LEN], theirs_blob[SG_SHA1_RAW_LEN];
    tree_spec base_specs[] = {{"a.txt", "line1\nline2\nline3\n"}};
    tree_spec ours_specs[] = {{"b.txt", "line1\nline2\nline3\n"}};
    tree_spec theirs_specs[] = {{"a.txt", "line1\nline2\nEDITED\n"}};
    sg_merge_result result;
    const sg_merge_result_entry *ea, *eb;

    build_tree(git_dir, base_specs, 1, base_tree);
    build_tree(git_dir, ours_specs, 1, ours_tree);
    build_tree(git_dir, theirs_specs, 1, theirs_tree);
    blob(git_dir, "line1\nline2\nline3\n", ours_blob);
    blob(git_dir, "line1\nline2\nEDITED\n", theirs_blob);

    /* With detection off, this is just an ordinary modify/delete conflict at
       a.txt (ours "deleted" it by renaming away, theirs edited it) plus an
       ordinary clean addition at b.txt (ours' new file, no relation drawn to
       a.txt at all) -- exactly what sg_merge_trees produced before Phase 49
       ever ran a rename detector. */
    CHECK(sg_merge_trees(git_dir, base_tree, ours_tree, theirs_tree, "ours", "theirs", 0, &result) ==
             0,
         "sg_merge_trees failed");
    CHECK(result.count == 2, "expected 2 independent entries (a.txt and b.txt), got %zu",
         result.count);
    ea = find_entry(&result, "a.txt");
    eb = find_entry(&result, "b.txt");
    CHECK(ea != NULL && eb != NULL, "expected entries at both a.txt and b.txt");
    if (ea != NULL) {
        CHECK(ea->conflict, "a.txt should be an ordinary modify/delete conflict");
        CHECK(!ea->ours_present && ea->theirs_present, "ours has no a.txt, theirs does");
        CHECK(ea->conflict_content_len == strlen("line1\nline2\nEDITED\n") &&
                 memcmp(ea->conflict_content, "line1\nline2\nEDITED\n", ea->conflict_content_len) ==
                    0,
             "a.txt's conflict content should be theirs' content verbatim");
    }
    if (eb != NULL) {
        CHECK(!eb->conflict && !eb->deleted, "b.txt should be a clean, unrelated addition");
        CHECK(memcmp(eb->sha1, ours_blob, SG_SHA1_RAW_LEN) == 0,
             "b.txt should carry ours' own content");
    }
    sg_merge_result_free(&result);
    free(git_dir);
}


/* build_tree above hard-codes 0100644; the mode-pinning test below needs a
   per-entry mode, and giving tree_spec a third field would silently
   zero-init every existing fixture's mode instead. */
typedef struct {
    const char *path;
    const char *content;
    unsigned int mode;
} tree_spec_mode;

static void build_tree_with_modes(const char *git_dir, const tree_spec_mode *specs, size_t count,
                                  unsigned char tree_id[SG_SHA1_RAW_LEN])
{
    sg_flat_entry *entries = malloc((count > 0 ? count : 1) * sizeof(*entries));
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

/* rename/rename-1to2's two stages carry the ORDINARY merged mode, not ours'
   own -- measured against real git 2.55.0 in BOTH directions, and the pair
   is the whole point: base 644 / ours 755 / theirs 644 and base 644 /
   ours 644 / theirs 755 both put 100755 at stage 2 AND stage 3 (and leave
   both working-tree files executable). "Use ours' mode" satisfies the first
   fixture and fails the second, so a single-direction fixture would call
   that rule green. */
static void check_1to2_modes(unsigned int ours_mode, unsigned int theirs_mode,
                             unsigned int want, const char *direction)
{
    char *git_dir = make_tmp_repo();
    unsigned char base_tree[SG_SHA1_RAW_LEN], ours_tree[SG_SHA1_RAW_LEN], theirs_tree[SG_SHA1_RAW_LEN];
    tree_spec_mode base_specs[] = {{"a.txt", "l1\nl2\nl3\nl4\nl5\n", 0100644}};
    tree_spec_mode ours_specs[] = {{"b.txt", "l1\nl2\nOURS\nl4\nl5\n", 0}};
    tree_spec_mode theirs_specs[] = {{"c.txt", "l1\nl2\nTHEIRS\nl4\nl5\n", 0}};
    sg_merge_result result;
    const sg_merge_result_entry *eo, *et;

    ours_specs[0].mode = ours_mode;
    theirs_specs[0].mode = theirs_mode;
    build_tree_with_modes(git_dir, base_specs, 1, base_tree);
    build_tree_with_modes(git_dir, ours_specs, 1, ours_tree);
    build_tree_with_modes(git_dir, theirs_specs, 1, theirs_tree);

    CHECK(sg_merge_trees(git_dir, base_tree, ours_tree, theirs_tree, "ours", "theirs",
                        SG_SIMILARITY_DEFAULT, &result) == 0,
         "sg_merge_trees failed (%s)", direction);
    eo = find_entry(&result, "b.txt");
    et = find_entry(&result, "c.txt");
    CHECK(eo != NULL && et != NULL, "expected entries at both destinations (%s)", direction);
    if (eo != NULL && et != NULL) {
        CHECK(eo->ours_mode == want, "%s: stage 2 mode should be %o, got %o", direction, want,
             eo->ours_mode);
        CHECK(et->theirs_mode == want, "%s: stage 3 mode should be %o, got %o", direction, want,
             et->theirs_mode);
        CHECK(eo->ours_mode == et->theirs_mode,
             "%s: the two stages must agree about the mode, like they do about the blob",
             direction);
    }
    sg_merge_result_free(&result);
    free(git_dir);
}

static void test_rename_rename_1to2_merged_mode(void)
{
    check_1to2_modes(0100755, 0100644, 0100755, "ours changed the mode");
    check_1to2_modes(0100644, 0100755, 0100755, "theirs changed the mode");
    check_1to2_modes(0100644, 0100644, 0100644, "neither changed the mode");
}


/* Regression, cold-review + fuzzer finding #1. THEIRS does the renaming and
   ours leaves the source alone -- the single most common real-world shape,
   and the one direction none of the tests above exercises (they all rename
   on OURS' side). Two independent halves, and each needs its own assertion
   because fixing one leaves the other broken:

     a) the landing must actually be WRITTEN. The entry at the destination
        used to carry ours' blob AT THE SOURCE PATH in ours_present/
        ours_sha1, so sg_merge_entry_touches_ours answered "ours already has
        this exact content here" for a path ours does not have at all, and
        sg_merge_result_apply skipped the write. The merge COMMIT's tree was
        correct the whole time, so only a working-tree assertion can see it.
     b) the source must actually be REMOVED. Nothing emitted an entry for a
        consumed rename source that ours still had, so no remove() ever ran
        and the old file stayed behind as an untracked leftover.

   All four gates were green for both halves; tests/fuzz_merge_rename.py is
   what caught them. */
static void test_theirs_side_rename_lands_and_removes_source(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = repo_root_of(git_dir);
    unsigned char base_tree[SG_SHA1_RAW_LEN], ours_tree[SG_SHA1_RAW_LEN], theirs_tree[SG_SHA1_RAW_LEN];
    const char *body = "line1\nline2\nline3\nline4\nline5\n";
    tree_spec base_specs[] = {{"p.txt", "line1\nline2\nline3\nline4\nline5\n"}};
    tree_spec ours_specs[] = {{"p.txt", "line1\nline2\nline3\nline4\nline5\n"}};
    tree_spec theirs_specs[] = {{"np.txt", "line1\nline2\nline3\nline4\nline5\n"}};
    sg_merge_result result;
    sg_index new_idx;
    char **conflict_paths = NULL;
    size_t conflict_count = 0;
    char abspath[4096];
    size_t i;
    int saw_np = 0, saw_p = 0;

    build_tree(git_dir, base_specs, 1, base_tree);
    build_tree(git_dir, ours_specs, 1, ours_tree);
    build_tree(git_dir, theirs_specs, 1, theirs_tree);

    /* The pre-merge working tree is ours', so the source really is on disk;
       without planting it, "p.txt is gone" would be true even if no code
       ever removed anything. */
    snprintf(abspath, sizeof(abspath), "%s/p.txt", repo_root);
    CHECK(sg_write_file_mkdirs(abspath, (const unsigned char *)body, strlen(body), 0644) == 0,
         "failed to plant the pre-merge p.txt");

    CHECK(sg_merge_trees(git_dir, base_tree, ours_tree, theirs_tree, "ours", "theirs",
                        SG_SIMILARITY_DEFAULT, &result) == 0,
         "sg_merge_trees failed");
    CHECK(sg_merge_result_apply(git_dir, repo_root, &result, &new_idx, &conflict_paths,
                               &conflict_count) == 0,
         "sg_merge_result_apply failed");
    CHECK(conflict_count == 0, "a pure one-sided rename must merge cleanly, got %zu conflict(s)",
         conflict_count);

    CHECK(file_exists(repo_root, "np.txt"),
         "half (a): the renamed-to file must be written to the working tree");
    CHECK(!file_exists(repo_root, "p.txt"),
         "half (b): the consumed rename source must be removed from the working tree");
    if (file_exists(repo_root, "np.txt")) {
        unsigned char *got = NULL;
        size_t got_len = 0;

        snprintf(abspath, sizeof(abspath), "%s/np.txt", repo_root);
        CHECK(sg_read_file(abspath, &got, &got_len) == 0, "failed to read np.txt");
        if (got != NULL)
            CHECK(got_len == strlen(body) && memcmp(got, body, got_len) == 0,
                 "np.txt must carry the merged content");
        free(got);
    }
    for (i = 0; i < new_idx.count; i++) {
        if (strcmp(new_idx.entries[i].path, "np.txt") == 0)
            saw_np = 1;
        if (strcmp(new_idx.entries[i].path, "p.txt") == 0)
            saw_p = 1;
    }
    CHECK(saw_np, "the index must carry the destination path");
    CHECK(!saw_p, "the index must not carry the consumed rename source");

    free(conflict_paths);
    sg_index_free(&new_idx);
    sg_merge_result_free(&result);
    free(repo_root);
    free(git_dir);
}

/* Regression, fuzzer finding #2. rename/rename-1to2 whose INNER content
   merge conflicts: merge_blob_content deliberately leaves *out_sha1
   untouched on a conflict (an ordinary conflict has no resolved blob), so
   the two stages used to be handed an uninitialized stack array and the
   index named an object that does not exist -- a corrupt repository, not a
   merely wrong answer. Measured against real git: both stages hold the
   marker-laden merge result as a REAL blob, byte-identical to the
   working-tree file.

   interop's phase45 fixture cannot see this: its renames are pure, so the
   inner merge is CLEAN and takes the branch that does fill the sha1. */
static void test_rename_rename_1to2_conflict_stage_blob_exists(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char base_tree[SG_SHA1_RAW_LEN], ours_tree[SG_SHA1_RAW_LEN], theirs_tree[SG_SHA1_RAW_LEN];
    tree_spec base_specs[] = {{"a.txt", "line1\nline2\nline3\nline4\nline5\n"}};
    tree_spec ours_specs[] = {{"b.txt", "line1\nOURS\nline3\nline4\nline5\n"}};
    tree_spec theirs_specs[] = {{"c.txt", "line1\nTHEIRS\nline3\nline4\nline5\n"}};
    sg_merge_result result;
    const sg_merge_result_entry *eo, *et;

    build_tree(git_dir, base_specs, 1, base_tree);
    build_tree(git_dir, ours_specs, 1, ours_tree);
    build_tree(git_dir, theirs_specs, 1, theirs_tree);

    CHECK(sg_merge_trees(git_dir, base_tree, ours_tree, theirs_tree, "ours", "theirs",
                        SG_SIMILARITY_DEFAULT, &result) == 0,
         "sg_merge_trees failed");
    eo = find_entry(&result, "b.txt");
    et = find_entry(&result, "c.txt");
    CHECK(eo != NULL && et != NULL, "expected entries at both destinations");
    if (eo != NULL && et != NULL) {
        unsigned char *stored = NULL;
        size_t stored_len = 0;
        sg_obj_type type;

        CHECK(memcmp(eo->ours_sha1, et->theirs_sha1, SG_SHA1_RAW_LEN) == 0,
             "stage 2 and stage 3 must name the SAME blob");
        CHECK(sg_object_read(git_dir, eo->ours_sha1, &type, &stored, &stored_len) == 0,
             "stage 2's blob must actually exist in the object store");
        if (stored != NULL) {
            CHECK(type == SG_OBJ_BLOB, "stage 2 must name a blob");
            CHECK(stored_len == eo->conflict_content_len &&
                     memcmp(stored, eo->conflict_content, stored_len) == 0,
                 "the stored blob must be byte-identical to the working-tree conflict content");
            free(stored);
        }
    }
    sg_merge_result_free(&result);
    free(git_dir);
}

int main(void)
{
    test_rename_clean_other_side_edits();
    test_rename_conflict_other_side_edits();
    test_rename_delete();
    test_rename_rename_same_destination();
    test_rename_rename_1to2();
    test_rename_rename_1to2_clean_inner_merge();
    test_rename_rename_1to2_merged_mode();
    test_rename_rename_1to2_apply_leaves_no_file_at_source();
    test_rename_add_collision();
    test_rename_rename_2to1();
    test_theirs_side_rename_lands_and_removes_source();
    test_rename_rename_1to2_conflict_stage_blob_exists();
    test_rename_score_zero_is_unaffected();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all merge_renames tests passed\n");
    return 0;
}
