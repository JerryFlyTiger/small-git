/* Phase 50: stash's two rename dimensions, neither of which had any
   regression test.

   `sg_stash_apply` and `sg_stash_apply_check_dirty` are two of the FOUR call
   sites that pass SG_SIMILARITY_DEFAULT to sg_merge_trees (the other two,
   `sg merge` and `sg rebase`, are covered by tests/fuzz_merge_rename.py).
   Nothing covered these two: fuzz_merge_rename.py never runs `sg stash`, and
   interop's only stash+rename group (Phase 31) tests `sg stash show`, which
   goes through sg_diff_detect_renames -- an entirely different code path
   from the three-way merge's own rename unit.

   The two tests below are deliberately different dimensions, not two
   spellings of one:

     1. the STASH'S OWN CONTENT contains a rename relative to the commit it
        was taken from, and HEAD has since edited the old name. This is the
        one that needs rename detection to come out right: without it the
        edit and the rename are a modify/delete conflict.
     2. the INDEX holds a staged rename at apply time that the stash never
        touches. This one never reaches the merge at all -- it exercises
        sg_stash_apply's re-stage loop, which has no concept of a rename and
        handles the two halves as unrelated paths. It came out right by
        construction rather than by design, which is exactly why it needs
        pinning: nothing would report it if that stopped being true.

   Both expectations were MEASURED against real git 2.55.0 rather than
   reasoned out (see docs/DESIGN.md Phase 50). git leaves case 1 with b.txt
   carrying HEAD's edit and a.txt gone from the working tree, and case 2 with
   the staged rename intact. */

#include "sg/stash.h"

#include "sg/hash.h"
#include "sg/index.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, ...)                                                                         \
    do {                                                                                         \
        if (!(cond)) {                                                                            \
            fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);                                  \
            fprintf(stderr, __VA_ARGS__);                                                         \
            fprintf(stderr, "\n");                                                                \
            failures++;                                                                           \
        }                                                                                          \
    } while (0)

/* 20 lines, long enough that rename scoring has something to work with and
   that a content merge has room for a non-overlapping edit. */
#define BASE_TEXT                                                                                \
    "line 00 of the file with enough text to be scored\n"                                        \
    "line 01 of the file with enough text to be scored\n"                                        \
    "line 02 of the file with enough text to be scored\n"                                        \
    "line 03 of the file with enough text to be scored\n"                                        \
    "line 04 of the file with enough text to be scored\n"                                        \
    "line 05 of the file with enough text to be scored\n"                                        \
    "line 06 of the file with enough text to be scored\n"                                        \
    "line 07 of the file with enough text to be scored\n"                                        \
    "line 08 of the file with enough text to be scored\n"                                        \
    "line 09 of the file with enough text to be scored\n"                                        \
    "line 10 of the file with enough text to be scored\n"                                        \
    "line 11 of the file with enough text to be scored\n"                                        \
    "line 12 of the file with enough text to be scored\n"                                        \
    "line 13 of the file with enough text to be scored\n"                                        \
    "line 14 of the file with enough text to be scored\n"                                        \
    "line 15 of the file with enough text to be scored\n"                                        \
    "line 16 of the file with enough text to be scored\n"                                        \
    "line 17 of the file with enough text to be scored\n"                                        \
    "line 18 of the file with enough text to be scored\n"                                        \
    "line 19 of the file with enough text to be scored\n"

/* Same file with one line rewritten -- HEAD's own edit in test 1. */
#define OURS_TEXT                                                                                \
    "line 00 of the file with enough text to be scored\n"                                        \
    "line 01 of the file with enough text to be scored\n"                                        \
    "line 02 of the file with enough text to be scored\n"                                        \
    "OURS-EDIT 03 rewritten by the commit made after the push\n"                                 \
    "line 04 of the file with enough text to be scored\n"                                        \
    "line 05 of the file with enough text to be scored\n"                                        \
    "line 06 of the file with enough text to be scored\n"                                        \
    "line 07 of the file with enough text to be scored\n"                                        \
    "line 08 of the file with enough text to be scored\n"                                        \
    "line 09 of the file with enough text to be scored\n"                                        \
    "line 10 of the file with enough text to be scored\n"                                        \
    "line 11 of the file with enough text to be scored\n"                                        \
    "line 12 of the file with enough text to be scored\n"                                        \
    "line 13 of the file with enough text to be scored\n"                                        \
    "line 14 of the file with enough text to be scored\n"                                        \
    "line 15 of the file with enough text to be scored\n"                                        \
    "line 16 of the file with enough text to be scored\n"                                        \
    "line 17 of the file with enough text to be scored\n"                                        \
    "line 18 of the file with enough text to be scored\n"                                        \
    "line 19 of the file with enough text to be scored\n"

static char *make_tmp_repo(void)
{
    static char template[] = "/tmp/sg_stash_rename_test_XXXXXX";
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
    unlink(abspath);
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

static void write_blob(const char *git_dir, const char *text, unsigned char out[SG_SHA1_RAW_LEN])
{
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, text, strlen(text), out) == 0, "write blob");
}

/* Commits `entries` on "master" with the given (optional) parent, and leaves
   the index holding exactly those entries at stage 0. */
static void commit_entries(const char *git_dir, const sg_flat_entry *entries, size_t count,
                           const unsigned char *parent, unsigned char out_commit[SG_SHA1_RAW_LEN])
{
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    sg_commit commit;
    sg_index idx;
    unsigned char *serialized;
    size_t serialized_len;
    size_t i;

    CHECK(sg_tree_build(git_dir, entries, count, tree_id) == 0, "build tree");

    memset(&idx, 0, sizeof(idx));
    for (i = 0; i < count; i++) {
        sg_index_entry e;

        memset(&e, 0, sizeof(e));
        e.mode = entries[i].mode;
        memcpy(e.sha1, entries[i].sha1, SG_SHA1_RAW_LEN);
        e.path = entries[i].path;
        CHECK(sg_index_upsert(&idx, &e) == 0, "upsert %s", entries[i].path);
    }
    CHECK(sg_index_write(git_dir, &idx) == 0, "write index");
    sg_index_free(&idx);

    memset(&commit, 0, sizeof(commit));
    memcpy(commit.tree, tree_id, SG_SHA1_RAW_LEN);
    if (parent != NULL) {
        commit.parents = malloc(SG_SHA1_RAW_LEN);
        memcpy(commit.parents[0], parent, SG_SHA1_RAW_LEN);
        commit.parent_count = 1;
    }
    commit.author_name = (char *)"Test";
    commit.author_email = (char *)"test@example.com";
    commit.author_time = 1700000000;
    strcpy(commit.author_tz, "+0000");
    commit.committer_name = commit.author_name;
    commit.committer_email = commit.author_email;
    commit.committer_time = commit.author_time;
    strcpy(commit.committer_tz, "+0000");
    commit.message = (char *)"c\n";

    CHECK(sg_commit_serialize(&commit, &serialized, &serialized_len) == 0, "serialize commit");
    CHECK(sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len, out_commit) == 0,
         "write commit");
    free(serialized);
    free(commit.parents);

    CHECK(sg_ref_update_branch(git_dir, "master", out_commit) == 0, "update branch");
}

static int stash_push(const char *git_dir, const char *repo_root, const char *message,
                      unsigned char commit_id_out[SG_SHA1_RAW_LEN])
{
    sg_stash_push_opts opts;

    memset(&opts, 0, sizeof(opts));
    opts.message = message;
    return sg_stash_push(git_dir, repo_root, &opts, commit_id_out, NULL);
}

/* Stages a rename src -> dst carrying `blob`, in the index AND on disk:
   the index loses src and gains dst, and the file moves. */
static void stage_rename(const char *git_dir, const char *repo_root, const char *src,
                         const char *dst, const unsigned char blob[SG_SHA1_RAW_LEN],
                         const char *content)
{
    sg_index idx;
    sg_index_entry e;

    CHECK(sg_index_read(git_dir, &idx) == 0, "read index for rename %s -> %s", src, dst);
    sg_index_remove(&idx, src);
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, blob, SG_SHA1_RAW_LEN);
    e.path = (char *)dst;
    CHECK(sg_index_upsert(&idx, &e) == 0, "stage %s", dst);
    CHECK(sg_index_write(git_dir, &idx) == 0, "write index after rename %s -> %s", src, dst);
    sg_index_free(&idx);

    remove_workdir_file(repo_root, src);
    write_workdir_file(repo_root, dst, content);
}

static char **check_dirty_paths = NULL;
static size_t check_dirty_count = 0;

static void free_check_dirty_result(void)
{
    size_t i;

    for (i = 0; i < check_dirty_count; i++)
        free(check_dirty_paths[i]);
    free(check_dirty_paths);
    check_dirty_paths = NULL;
    check_dirty_count = 0;
}

/* Dimension 1: the stash's own content renamed a.txt -> b.txt, and HEAD then
   edited a.txt. Rename detection is what turns this into "the edit follows
   the rename"; without it, a.txt is modified on one side and deleted on the
   other, i.e. a conflict, and b.txt lands carrying the PRE-edit bytes.

   Measured with real git 2.55.0: pop is clean, the working tree holds b.txt
   only, and b.txt carries HEAD's edit. */
static void test_stashed_rename_follows_head_edit(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char blob_base[SG_SHA1_RAW_LEN];
    unsigned char blob_ours[SG_SHA1_RAW_LEN];
    unsigned char base_commit[SG_SHA1_RAW_LEN];
    unsigned char ours_commit[SG_SHA1_RAW_LEN];
    unsigned char stash_id[SG_SHA1_RAW_LEN];
    sg_flat_entry entries[1];
    sg_index idx;
    char *content;
    int rc;
    int pos;

    write_blob(git_dir, BASE_TEXT, blob_base);
    entries[0].path = (char *)"a.txt";
    entries[0].mode = 0100644;
    memcpy(entries[0].sha1, blob_base, SG_SHA1_RAW_LEN);
    commit_entries(git_dir, entries, 1, NULL, base_commit);
    write_workdir_file(repo_root, "a.txt", BASE_TEXT);

    /* Stage the rename, then stash it: the stash's tree holds b.txt where
       its base commit holds a.txt. */
    stage_rename(git_dir, repo_root, "a.txt", "b.txt", blob_base, BASE_TEXT);
    rc = stash_push(git_dir, repo_root, "renamed", stash_id);
    CHECK(rc == 0, "stash push of a staged rename should succeed, rc=%d", rc);
    CHECK(!file_exists(repo_root, "b.txt"), "push must reset b.txt away");
    CHECK(file_exists(repo_root, "a.txt"), "push must restore a.txt from HEAD");

    /* HEAD now edits the OLD name and commits, so ours != base. */
    write_blob(git_dir, OURS_TEXT, blob_ours);
    entries[0].path = (char *)"a.txt";
    entries[0].mode = 0100644;
    memcpy(entries[0].sha1, blob_ours, SG_SHA1_RAW_LEN);
    commit_entries(git_dir, entries, 1, base_commit, ours_commit);
    write_workdir_file(repo_root, "a.txt", OURS_TEXT);

    rc = sg_stash_apply_check_dirty(git_dir, repo_root, 0, &check_dirty_paths, &check_dirty_count);
    CHECK(rc == 0, "a clean working tree must not block the apply, rc=%d", rc);
    free_check_dirty_result();

    rc = sg_stash_apply(git_dir, repo_root, 0, 0);
    CHECK(rc == 0, "the rename and the edit must merge cleanly, rc=%d", rc);

    CHECK(file_exists(repo_root, "b.txt"), "b.txt must be created by the apply");
    CHECK(!file_exists(repo_root, "a.txt"),
         "a.txt must be gone from the working tree -- the rename consumed it");

    content = read_workdir_file(repo_root, "b.txt");
    CHECK(content != NULL, "b.txt must be readable");
    if (content != NULL) {
        /* The discriminating assertion: rename detection is the only thing
           that carries HEAD's edit across to the new name. */
        CHECK(strstr(content, "OURS-EDIT 03") != NULL,
             "b.txt must carry HEAD's own edit, i.e. the edit followed the rename");
        CHECK(strstr(content, "<<<<<<<") == NULL,
             "b.txt must not contain conflict markers");
        free(content);
    }

    CHECK(sg_index_read(git_dir, &idx) == 0, "read index after apply");
    pos = sg_index_find(&idx, "b.txt");
    CHECK(pos >= 0, "b.txt must have an index entry after the apply");
    if (pos >= 0)
        CHECK(idx.entries[pos].stage == 0, "b.txt's entry must be stage 0, not a conflict stage");
    sg_index_free(&idx);

    free(repo_root);
    free(git_dir);
}

/* Dimension 2: the rename is only in the INDEX, staged after the push, on a
   path the stash never touches. This never reaches sg_merge_trees at all;
   it exercises sg_stash_apply's re-stage loop, which treats the deleted
   source and the added destination as two unrelated paths.

   Measured with real git 2.55.0: pop is clean and `git status` still reports
   the staged rename afterwards. */
static void test_staged_rename_survives_apply(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char blob_s_base[SG_SHA1_RAW_LEN];
    unsigned char blob_s_change[SG_SHA1_RAW_LEN];
    unsigned char blob_moved[SG_SHA1_RAW_LEN];
    unsigned char base_commit[SG_SHA1_RAW_LEN];
    unsigned char stash_id[SG_SHA1_RAW_LEN];
    sg_flat_entry entries[2];
    sg_index idx;
    char *content;
    int rc;
    int pos;

    write_blob(git_dir, "stashed base\n", blob_s_base);
    write_blob(git_dir, BASE_TEXT, blob_moved);
    entries[0].path = (char *)"a2.txt";
    entries[0].mode = 0100644;
    memcpy(entries[0].sha1, blob_moved, SG_SHA1_RAW_LEN);
    entries[1].path = (char *)"s.txt";
    entries[1].mode = 0100644;
    memcpy(entries[1].sha1, blob_s_base, SG_SHA1_RAW_LEN);
    commit_entries(git_dir, entries, 2, NULL, base_commit);
    write_workdir_file(repo_root, "a2.txt", BASE_TEXT);
    write_workdir_file(repo_root, "s.txt", "stashed base\n");

    /* The stash's ONLY change is to s.txt. */
    write_workdir_file(repo_root, "s.txt", "stashed CHANGE\n");
    write_blob(git_dir, "stashed CHANGE\n", blob_s_change);
    rc = stash_push(git_dir, repo_root, "only s", stash_id);
    CHECK(rc == 0, "stash push should succeed, rc=%d", rc);

    /* Only NOW stage the rename, on a path the stash does not mention. */
    stage_rename(git_dir, repo_root, "a2.txt", "b2.txt", blob_moved, BASE_TEXT);

    rc = sg_stash_apply_check_dirty(git_dir, repo_root, 0, &check_dirty_paths, &check_dirty_count);
    CHECK(rc == 0, "a staged rename the stash never touches must not block apply, rc=%d", rc);
    free_check_dirty_result();

    rc = sg_stash_apply(git_dir, repo_root, 0, 0);
    CHECK(rc == 0, "the apply itself should succeed, rc=%d", rc);

    content = read_workdir_file(repo_root, "s.txt");
    CHECK(content != NULL && strcmp(content, "stashed CHANGE\n") == 0,
         "s.txt must carry the stashed content after the apply");
    free(content);

    CHECK(file_exists(repo_root, "b2.txt"), "b2.txt must still be on disk");
    CHECK(!file_exists(repo_root, "a2.txt"), "a2.txt must stay gone from the working tree");

    CHECK(sg_index_read(git_dir, &idx) == 0, "read index after apply");
    pos = sg_index_find(&idx, "b2.txt");
    CHECK(pos >= 0, "the staged rename's destination must still be in the index");
    if (pos >= 0)
        CHECK(idx.entries[pos].stage == 0 &&
             memcmp(idx.entries[pos].sha1, blob_moved, SG_SHA1_RAW_LEN) == 0,
             "b2.txt must still be stage 0 carrying its own blob");
    CHECK(sg_index_find(&idx, "a2.txt") < 0,
         "the staged rename's source must NOT be resurrected in the index");
    sg_index_free(&idx);

    free(repo_root);
    free(git_dir);
}

int main(void)
{
    test_stashed_rename_follows_head_edit();
    test_staged_rename_survives_apply();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_stash_rename: all checks passed\n");
    return 0;
}
