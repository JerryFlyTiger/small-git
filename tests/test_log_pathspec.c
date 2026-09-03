#include "sg/commit_out.h"

#include "sg/loose.h"
#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/pathspec.h"
#include "sg/repo.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
static long long time_seq = 1000000;

#define CHECK(cond, ...)                                                                        \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);                                 \
            fprintf(stderr, __VA_ARGS__);                                                        \
            fprintf(stderr, "\n");                                                               \
            failures++;                                                                          \
        }                                                                                         \
    } while (0)

/* repo_root_out receives the repository root (the directory containing
   .git) -- sg_pathspec_add resolves a relative argument against the
   process's actual cwd, so tests that do not chdir into the repo pass
   ABSOLUTE arguments (repo_root + "/" + path) to sidestep that entirely;
   see add_spec below. */
static char *make_tmp_repo(char **repo_root_out)
{
    static char template[] = "/tmp/sg_log_pathspec_test_XXXXXX";
    char *path = strdup(template);
    char git_dir[SG_PATH_MAX];

    if (path == NULL) {
        fprintf(stderr, "oom\n");
        exit(1);
    }
    if (mkdtemp(path) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        exit(1);
    }
    if (sg_repo_init(path) != 0) {
        fprintf(stderr, "sg_repo_init failed\n");
        exit(1);
    }
    snprintf(git_dir, sizeof(git_dir), "%s/.git", path);
    *repo_root_out = path;
    return strdup(git_dir);
}

/* Builds a tree containing exactly the given (path, content, mode) rows and
   a commit whose tree is that, with a single optional parent -- everything
   this test needs `sg_commit_out_touches_pathspec` to compare, without
   touching a working directory at all (the function only ever reads
   objects). */
typedef struct {
    const char *path;
    const char *content;
    int mode; /* e.g. 0100644 / 0100755 */
} row;

static void make_commit(const char *git_dir, const row *rows, size_t n_rows,
                        const unsigned char *parent, /* NULL for a root commit */
                        unsigned char commit_id_out[SG_SHA1_RAW_LEN])
{
    sg_flat_entry *entries;
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    sg_commit commit;
    unsigned char *serialized;
    size_t serialized_len;
    size_t i;

    entries = malloc(n_rows * sizeof(*entries));
    CHECK(entries != NULL, "oom");
    for (i = 0; i < n_rows; i++) {
        unsigned char blob_id[SG_SHA1_RAW_LEN];

        CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, rows[i].content, strlen(rows[i].content),
                            blob_id) == 0,
             "blob write failed for '%s'", rows[i].path);
        entries[i].path = (char *)rows[i].path;
        entries[i].mode = rows[i].mode;
        memcpy(entries[i].sha1, blob_id, SG_SHA1_RAW_LEN);
    }
    CHECK(sg_tree_build(git_dir, entries, n_rows, tree_id) == 0, "tree build failed");
    free(entries);

    memset(&commit, 0, sizeof(commit));
    memcpy(commit.tree, tree_id, SG_SHA1_RAW_LEN);
    if (parent != NULL) {
        commit.parents = malloc(SG_SHA1_RAW_LEN);
        CHECK(commit.parents != NULL, "oom");
        memcpy(commit.parents, parent, SG_SHA1_RAW_LEN);
        commit.parent_count = 1;
    }
    commit.author_name = (char *)"tester";
    commit.author_email = (char *)"tester@example.com";
    commit.author_time = time_seq++;
    strcpy(commit.author_tz, "+0000");
    commit.committer_name = commit.author_name;
    commit.committer_email = commit.author_email;
    commit.committer_time = commit.author_time;
    strcpy(commit.committer_tz, "+0000");
    commit.message = (char *)"msg";

    CHECK(sg_commit_serialize(&commit, &serialized, &serialized_len) == 0, "serialize failed");
    free(commit.parents);
    CHECK(sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len, commit_id_out) == 0,
         "commit write failed");
    free(serialized);
}

/* Re-reads a commit id back into a parsed sg_commit -- the function under
   test takes a parsed commit, not an id, mirroring print_commit_diff's own
   signature. */
static void read_commit(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                        sg_commit *out)
{
    sg_obj_type type;
    unsigned char *content;
    size_t content_len;

    CHECK(sg_object_read(git_dir, id, &type, &content, &content_len) == 0, "commit read failed");
    CHECK(type == SG_OBJ_COMMIT, "wrong object type");
    CHECK(sg_commit_parse(content, content_len, out) == 0, "commit parse failed");
    free(content);
}

/* `rel` is a repo-relative path; passed to sg_pathspec_add as an ABSOLUTE
   argument (repo_root + "/" + rel) so its resolution never depends on the
   test binary's own process-wide cwd (see make_tmp_repo's comment). */
static void add_spec(sg_pathspec *ps, const char *repo_root, const char *rel)
{
    sg_pathspec_error err;
    char abs_arg[SG_PATH_MAX];

    snprintf(abs_arg, sizeof(abs_arg), "%s/%s", repo_root, rel);
    CHECK(sg_pathspec_add(ps, repo_root, abs_arg, &err) == 0,
         "sg_pathspec_add('%s') failed, err=%d", rel, err);
}

/* ps == NULL must answer 1 without reading a single tree -- a pathspec-less
   `sg log` must not pay this function's cost. Using a deliberately corrupt
   commit tree id would prove "no tree was read", but sg_commit_out_touches_
   pathspec's own NULL branch already returns before touching commit->tree
   at all, so an ordinary well-formed commit already exercises the early
   return; there is nothing left for a corrupt tree to additionally prove. */
static void test_null_pathspec_always_touches(void)
{
    char *repo_root;
    char *git_dir = make_tmp_repo(&repo_root);
    row rows[] = {{"a.txt", "a1", 0100644}};
    unsigned char root_id[SG_SHA1_RAW_LEN];
    sg_commit commit;
    int touches = -1;
    char bad_path[SG_PATH_MAX];

    make_commit(git_dir, rows, 1, NULL, root_id);
    read_commit(git_dir, root_id, &commit);

    CHECK(sg_commit_out_touches_pathspec(git_dir, &commit, NULL, &touches, bad_path) == 0,
         "call failed");
    CHECK(touches == 1, "NULL pathspec must always touch, got %d", touches);

    sg_commit_free(&commit);
    free(git_dir);
    free(repo_root);
}

/* A root commit (parent_count == 0) is compared against the empty tree --
   every one of its files is "added", so a pathspec naming any of them must
   answer touches=1, and a pathspec naming something else must answer 0. */
static void test_root_commit_against_empty_tree(void)
{
    char *repo_root;
    char *git_dir = make_tmp_repo(&repo_root);
    row rows[] = {{"a.txt", "a1", 0100644}, {"sub/b.txt", "b1", 0100644}};
    unsigned char root_id[SG_SHA1_RAW_LEN];
    sg_commit commit;
    int touches;
    char bad_path[SG_PATH_MAX];
    sg_pathspec hit, miss;

    memset(&hit, 0, sizeof(hit));
    memset(&miss, 0, sizeof(miss));

    make_commit(git_dir, rows, 2, NULL, root_id);
    read_commit(git_dir, root_id, &commit);

    add_spec(&hit, repo_root, "a.txt");
    touches = -1;
    CHECK(sg_commit_out_touches_pathspec(git_dir, &commit, &hit, &touches, bad_path) == 0,
         "call failed (hit)");
    CHECK(touches == 1, "root commit vs matching pathspec should touch, got %d", touches);

    add_spec(&miss, repo_root, "nosuch.txt");
    touches = -1;
    CHECK(sg_commit_out_touches_pathspec(git_dir, &commit, &miss, &touches, bad_path) == 0,
         "call failed (miss)");
    CHECK(touches == 0, "root commit vs non-matching pathspec should not touch, got %d", touches);

    sg_pathspec_free(&hit);
    sg_pathspec_free(&miss);
    sg_commit_free(&commit);
    free(git_dir);
    free(repo_root);
}

/* A commit that ONLY chmods a.txt (content unchanged, mode 644 -> 755) must
   still be judged as touching a.txt -- CLAUDE.md section 0.2: sg_diff_trees
   already emits a row for a mode-only change, this judgment is built
   directly on top of it, not on a from-scratch tree comparison. */
static void test_mode_only_change_touches(void)
{
    char *repo_root;
    char *git_dir = make_tmp_repo(&repo_root);
    row rows1[] = {{"a.txt", "a1", 0100644}};
    row rows2[] = {{"a.txt", "a1", 0100755}};
    unsigned char c1[SG_SHA1_RAW_LEN], c2[SG_SHA1_RAW_LEN];
    sg_commit commit;
    int touches = -1;
    char bad_path[SG_PATH_MAX];
    sg_pathspec ps;

    memset(&ps, 0, sizeof(ps));

    make_commit(git_dir, rows1, 1, NULL, c1);
    make_commit(git_dir, rows2, 1, c1, c2);
    read_commit(git_dir, c2, &commit);

    add_spec(&ps, repo_root, "a.txt");
    CHECK(sg_commit_out_touches_pathspec(git_dir, &commit, &ps, &touches, bad_path) == 0,
         "call failed");
    CHECK(touches == 1, "mode-only change must count as touching, got %d", touches);

    sg_pathspec_free(&ps);
    sg_commit_free(&commit);
    free(git_dir);
    free(repo_root);
}

/* A commit that changes X while the pathspec names only Y must answer
   touches=0. */
static void test_unrelated_change_does_not_touch(void)
{
    char *repo_root;
    char *git_dir = make_tmp_repo(&repo_root);
    row rows1[] = {{"x.txt", "x1", 0100644}, {"y.txt", "y1", 0100644}};
    row rows2[] = {{"x.txt", "x2", 0100644}, {"y.txt", "y1", 0100644}};
    unsigned char c1[SG_SHA1_RAW_LEN], c2[SG_SHA1_RAW_LEN];
    sg_commit commit;
    int touches = -1;
    char bad_path[SG_PATH_MAX];
    sg_pathspec ps;

    memset(&ps, 0, sizeof(ps));

    make_commit(git_dir, rows1, 2, NULL, c1);
    make_commit(git_dir, rows2, 2, c1, c2); /* only x.txt changes */
    read_commit(git_dir, c2, &commit);

    add_spec(&ps, repo_root, "y.txt");
    CHECK(sg_commit_out_touches_pathspec(git_dir, &commit, &ps, &touches, bad_path) == 0,
         "call failed");
    CHECK(touches == 0, "a change to x.txt must not touch a pathspec naming only y.txt, got %d",
         touches);

    sg_pathspec_free(&ps);
    sg_commit_free(&commit);
    free(git_dir);
    free(repo_root);
}

int main(void)
{
    test_null_pathspec_always_touches();
    test_root_commit_against_empty_tree();
    test_mode_only_change_touches();
    test_unrelated_change_does_not_touch();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all log_pathspec tests passed\n");
    return 0;
}
