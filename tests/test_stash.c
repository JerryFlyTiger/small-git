#include "sg/stash.h"

#include "sg/hash.h"
#include "sg/index.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/reflog.h"
#include "sg/refs.h"
#include "sg/repo.h"
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
    static char template[] = "/tmp/sg_stash_test_XXXXXX";
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

/* sg_stash_push now takes an options struct; this wrapper keeps the many
   existing "just a message, no flags" call sites below a one-liner. */
static int stash_push(const char *git_dir, const char *repo_root, const char *message,
                      unsigned char commit_id_out[SG_SHA1_RAW_LEN])
{
    sg_stash_push_opts opts;

    memset(&opts, 0, sizeof(opts));
    opts.message = message;
    return sg_stash_push(git_dir, repo_root, &opts, commit_id_out, NULL);
}

/* Same, but for a call site that also needs opts.keep_index set. */
static int stash_push_keep_index(const char *git_dir, const char *repo_root, const char *message,
                                 unsigned char commit_id_out[SG_SHA1_RAW_LEN])
{
    sg_stash_push_opts opts;

    memset(&opts, 0, sizeof(opts));
    opts.message = message;
    opts.keep_index = 1;
    return sg_stash_push(git_dir, repo_root, &opts, commit_id_out, NULL);
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

/* Builds a single-file initial commit (a.txt = "hello\n") on the "master"
   branch, staged and committed, so push tests have a real HEAD to diff
   against. */
static void commit_initial(const char *git_dir, const char *repo_root)
{
    unsigned char blob[SG_SHA1_RAW_LEN];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    sg_flat_entry entries[1];
    sg_index idx;
    sg_index_entry e;
    sg_commit commit;
    unsigned char *serialized;
    size_t serialized_len;
    unsigned char commit_id[SG_SHA1_RAW_LEN];

    write_workdir_file(repo_root, "a.txt", "hello\n");
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "hello\n", 6, blob) == 0, "write blob");

    memset(&idx, 0, sizeof(idx));
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, blob, SG_SHA1_RAW_LEN);
    e.path = (char *)"a.txt";
    CHECK(sg_index_upsert(&idx, &e) == 0, "upsert a.txt");
    CHECK(sg_index_write(git_dir, &idx) == 0, "write index");
    sg_index_free(&idx);

    entries[0].path = (char *)"a.txt";
    entries[0].mode = 0100644;
    memcpy(entries[0].sha1, blob, SG_SHA1_RAW_LEN);
    CHECK(sg_tree_build(git_dir, entries, 1, tree_id) == 0, "build tree");

    memset(&commit, 0, sizeof(commit));
    memcpy(commit.tree, tree_id, SG_SHA1_RAW_LEN);
    commit.parent_count = 0;
    commit.author_name = (char *)"Test";
    commit.author_email = (char *)"test@example.com";
    commit.author_time = 1700000000;
    strcpy(commit.author_tz, "+0000");
    commit.committer_name = commit.author_name;
    commit.committer_email = commit.author_email;
    commit.committer_time = commit.author_time;
    strcpy(commit.committer_tz, "+0000");
    commit.message = (char *)"initial\n";

    CHECK(sg_commit_serialize(&commit, &serialized, &serialized_len) == 0, "serialize commit");
    CHECK(sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len, commit_id) == 0,
         "write commit");
    free(serialized);

    CHECK(sg_ref_update_branch(git_dir, "master", commit_id) == 0, "update branch");
}

/* Asserts refs/stash equals the last reflog line's new_id, or that both are
   absent -- the one hard invariant sg_stash_push/drop/clear must never
   break (see stash-spec.md sec 1.1). */
static void assert_tip_invariant(const char *git_dir)
{
    unsigned char ref_id[SG_SHA1_RAW_LEN];
    sg_reflog log;
    int ref_exists = (sg_ref_read_path(git_dir, "refs/stash", ref_id) == 0);

    CHECK(sg_reflog_read(git_dir, "refs/stash", &log) == 0, "reflog read failed");

    if (!ref_exists) {
        CHECK(log.count == 0, "refs/stash absent but reflog has %zu entries", log.count);
    } else {
        CHECK(log.count > 0, "refs/stash exists but reflog is empty");
        if (log.count > 0)
            CHECK(memcmp(ref_id, log.entries[log.count - 1].new_id, SG_SHA1_RAW_LEN) == 0,
                 "tip invariant violated: refs/stash != last reflog line's new_id");
    }
    sg_reflog_free(&log);
}

/* ---- sg_stash_parse_spec accept/reject table -------------------------- */

static void test_parse_spec_table(void)
{
    size_t idx;

    CHECK(sg_stash_parse_spec(NULL, &idx) == 0 && idx == 0, "NULL should parse as 0");
    CHECK(sg_stash_parse_spec("", &idx) == 0 && idx == 0, "\"\" should parse as 0");
    CHECK(sg_stash_parse_spec("stash@{0}", &idx) == 0 && idx == 0, "stash@{0}");
    CHECK(sg_stash_parse_spec("stash@{12}", &idx) == 0 && idx == 12, "stash@{12}");
    CHECK(sg_stash_parse_spec("7", &idx) == 0 && idx == 7, "bare 7");

    CHECK(sg_stash_parse_spec("stash@{}", &idx) == -1, "stash@{} should be rejected");
    CHECK(sg_stash_parse_spec("stash@{-1}", &idx) == -1, "stash@{-1} should be rejected");
    CHECK(sg_stash_parse_spec("stash@{1x}", &idx) == -1, "stash@{1x} should be rejected");
    CHECK(sg_stash_parse_spec("stash@{ 1 }", &idx) == -1, "stash@{ 1 } should be rejected");
    CHECK(sg_stash_parse_spec("refs/stash", &idx) == -1, "refs/stash should be rejected");
    CHECK(sg_stash_parse_spec("stash@{99999999999999999999}", &idx) == -1,
         "overflow-sized spec should be rejected");
}

/* ---- push / list / drop / clear, asserting the tip invariant after every
   mutation ----------------------------------------------------------------- */

static void test_push_list_drop_clear(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char commit1[SG_SHA1_RAW_LEN];
    unsigned char commit2[SG_SHA1_RAW_LEN];
    int rc;

    commit_initial(git_dir, repo_root);

    /* nothing to save yet: exit 1, disk untouched */
    rc = stash_push(git_dir, repo_root, NULL, commit1);
    CHECK(rc == 1, "expected nothing-to-save (1), got %d", rc);
    assert_tip_invariant(git_dir);
    {
        unsigned char ref_id[SG_SHA1_RAW_LEN];

        CHECK(sg_ref_read_path(git_dir, "refs/stash", ref_id) != 0, "refs/stash should not exist yet");
    }

    /* dirty the workdir, then stash it */
    write_workdir_file(repo_root, "a.txt", "changed\n");

    rc = stash_push(git_dir, repo_root, "first stash", commit1);
    CHECK(rc == 0, "expected a stash to be created, got %d", rc);
    assert_tip_invariant(git_dir);

    {
        char *content = read_workdir_file(repo_root, "a.txt");

        CHECK(content != NULL && strcmp(content, "hello\n") == 0,
             "workdir should be reset to HEAD after push, got %s", content != NULL ? content : "(null)");
        free(content);
    }

    /* second stash, default WIP message */
    write_workdir_file(repo_root, "a.txt", "changed again\n");
    rc = stash_push(git_dir, repo_root, NULL, commit2);
    CHECK(rc == 0, "expected a second stash, got %d", rc);
    assert_tip_invariant(git_dir);

    {
        sg_stash_list list;

        CHECK(sg_stash_list_read(git_dir, &list) == 0, "list read failed");
        CHECK(list.count == 2, "expected 2 stashes, got %zu", list.count);
        if (list.count == 2) {
            CHECK(memcmp(list.entries[0].commit_id, commit2, SG_SHA1_RAW_LEN) == 0,
                 "stash@{0} should be the most recently pushed stash");
            CHECK(memcmp(list.entries[1].commit_id, commit1, SG_SHA1_RAW_LEN) == 0,
                 "stash@{1} should be the first pushed stash");
            CHECK(strstr(list.entries[1].message, "first stash") != NULL,
                 "stash@{1} message should contain the -m text: %s", list.entries[1].message);
            CHECK(strstr(list.entries[0].message, "WIP on") != NULL,
                 "stash@{0} message should be the WIP form: %s", list.entries[0].message);
        }
        sg_stash_list_free(&list);
    }

    /* drop the newest (stash@{0}) -- refs/stash must re-point at stash@{1} */
    CHECK(sg_stash_drop(git_dir, 0) == 0, "drop stash@{0} failed");
    assert_tip_invariant(git_dir);
    {
        sg_stash_list list;

        CHECK(sg_stash_list_read(git_dir, &list) == 0, "list read failed after drop");
        CHECK(list.count == 1, "expected 1 stash left, got %zu", list.count);
        if (list.count == 1)
            CHECK(memcmp(list.entries[0].commit_id, commit1, SG_SHA1_RAW_LEN) == 0,
                 "remaining stash should be the first pushed one");
        sg_stash_list_free(&list);
    }
    {
        unsigned char ref_id[SG_SHA1_RAW_LEN];

        CHECK(sg_ref_read_path(git_dir, "refs/stash", ref_id) == 0, "refs/stash should still exist");
        CHECK(memcmp(ref_id, commit1, SG_SHA1_RAW_LEN) == 0,
             "refs/stash should now equal the surviving stash's commit");
    }

    /* drop the last entry -> stack empties, refs/stash and the reflog vanish */
    CHECK(sg_stash_drop(git_dir, 0) == 0, "drop of the last entry failed");
    assert_tip_invariant(git_dir);
    {
        unsigned char ref_id[SG_SHA1_RAW_LEN];

        CHECK(sg_ref_read_path(git_dir, "refs/stash", ref_id) != 0,
             "refs/stash should be gone once the stack is empty");
    }

    /* push once more, then clear */
    write_workdir_file(repo_root, "a.txt", "one more change\n");
    rc = stash_push(git_dir, repo_root, NULL, commit1);
    CHECK(rc == 0, "expected a stash before clear, got %d", rc);
    assert_tip_invariant(git_dir);

    CHECK(sg_stash_clear(git_dir) == 0, "clear failed");
    assert_tip_invariant(git_dir);
    {
        unsigned char ref_id[SG_SHA1_RAW_LEN];

        CHECK(sg_ref_read_path(git_dir, "refs/stash", ref_id) != 0, "refs/stash should be gone after clear");
    }

    /* clear on an already-empty stack is a no-op, not an error */
    CHECK(sg_stash_clear(git_dir) == 0, "clear on an empty stack should still succeed");

    free(repo_root);
    free(git_dir);
}

/* ---- drop/clear roll back the reflog when the follow-up ref op fails ---- */

/* Forces sg_stash_drop's non-empty path (rewrite succeeds, then
   sg_ref_write_path fails) by chmod'ing refs/stash itself read-only right
   before the drop: sg_ref_write_path truncates an EXISTING file in place, so
   only the file's own write bit -- not the containing directory's -- needs
   to be revoked. Asserts the reflog is put back so the tip invariant holds,
   exactly what stash.c's rollback comment above sg_reflog_rewrite(git_dir,
   "refs/stash", log.entries, log.count) promises. */
static void test_drop_rolls_back_on_ref_write_failure(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char commit1[SG_SHA1_RAW_LEN];
    unsigned char commit2[SG_SHA1_RAW_LEN];
    char ref_path[4096];
    unsigned char old_tip[SG_SHA1_RAW_LEN];

    if (geteuid() == 0) {
        /* root ignores file modes; the test would be meaningless */
        free(repo_root);
        free(git_dir);
        return;
    }

    commit_initial(git_dir, repo_root);

    write_workdir_file(repo_root, "a.txt", "changed\n");
    CHECK(stash_push(git_dir, repo_root, "first", commit1) == 0, "first push failed");
    write_workdir_file(repo_root, "a.txt", "changed again\n");
    CHECK(stash_push(git_dir, repo_root, "second", commit2) == 0, "second push failed");
    assert_tip_invariant(git_dir);

    CHECK(sg_ref_read_path(git_dir, "refs/stash", old_tip) == 0, "refs/stash should exist before drop");
    CHECK(memcmp(old_tip, commit2, SG_SHA1_RAW_LEN) == 0, "refs/stash should point at the newest stash");

    snprintf(ref_path, sizeof(ref_path), "%s/refs/stash", git_dir);
    CHECK(chmod(ref_path, 0444) == 0, "chmod refs/stash read-only failed");

    CHECK(sg_stash_drop(git_dir, 0) == -1,
         "drop should fail once refs/stash cannot be rewritten (read-only)");

    chmod(ref_path, 0644); /* restore before any further reads/writes */

    /* Rollback must have restored the reflog so it still backs the
       untouched refs/stash (still commit2 -- the write never landed). */
    assert_tip_invariant(git_dir);
    {
        unsigned char ref_id[SG_SHA1_RAW_LEN];

        CHECK(sg_ref_read_path(git_dir, "refs/stash", ref_id) == 0, "refs/stash should still exist");
        CHECK(memcmp(ref_id, commit2, SG_SHA1_RAW_LEN) == 0,
             "refs/stash should be unchanged by the failed drop");
    }
    {
        sg_stash_list list;

        CHECK(sg_stash_list_read(git_dir, &list) == 0, "list read failed after failed drop");
        CHECK(list.count == 2, "both stashes should still be listed after a failed drop, got %zu",
             list.count);
        sg_stash_list_free(&list);
    }

    free(repo_root);
    free(git_dir);
}

/* Forces sg_stash_clear's failure path (reflog wiped, then
   sg_ref_delete_under fails to unlink the loose refs/stash file) by
   chmod'ing the CONTAINING refs/ directory read-only: unlink needs write
   permission on the directory, not the file. Asserts the wiped reflog is put
   back so refs/stash (still present, untouched by the failed unlink) keeps
   its backing reflog line. */
static void test_clear_rolls_back_on_delete_failure(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char commit1[SG_SHA1_RAW_LEN];
    char refs_dir[4096];
    unsigned char old_tip[SG_SHA1_RAW_LEN];

    if (geteuid() == 0) {
        free(repo_root);
        free(git_dir);
        return;
    }

    commit_initial(git_dir, repo_root);

    write_workdir_file(repo_root, "a.txt", "changed\n");
    CHECK(stash_push(git_dir, repo_root, "only", commit1) == 0, "push failed");
    assert_tip_invariant(git_dir);

    CHECK(sg_ref_read_path(git_dir, "refs/stash", old_tip) == 0, "refs/stash should exist before clear");

    snprintf(refs_dir, sizeof(refs_dir), "%s/refs", git_dir);
    CHECK(chmod(refs_dir, 0555) == 0, "chmod refs/ read-only failed");

    CHECK(sg_stash_clear(git_dir) == -1,
         "clear should fail once refs/stash cannot be unlinked (read-only refs/ dir)");

    chmod(refs_dir, 0755); /* restore before any further reads/writes */

    /* Rollback must have restored the wiped reflog so it still backs the
       untouched refs/stash. */
    assert_tip_invariant(git_dir);
    {
        unsigned char ref_id[SG_SHA1_RAW_LEN];

        CHECK(sg_ref_read_path(git_dir, "refs/stash", ref_id) == 0,
             "refs/stash should still exist (the unlink never landed)");
        CHECK(memcmp(ref_id, old_tip, SG_SHA1_RAW_LEN) == 0, "refs/stash should be unchanged");
    }
    {
        sg_stash_list list;

        CHECK(sg_stash_list_read(git_dir, &list) == 0, "list read failed after failed clear");
        CHECK(list.count == 1, "the stash should still be listed after a failed clear, got %zu",
             list.count);
        sg_stash_list_free(&list);
    }

    free(repo_root);
    free(git_dir);
}

/* ---- sg_stash_push's -2: durable stash, but the post-write reset failed - */

/* Forces sg_stash_push past its "destructive from here on" comment (the
   commit + refs/stash are already written) and then fails the
   sg_apply_tree_to_workdir step that resets a.txt back to HEAD's content,
   by chmod'ing a.txt read-only right before the push: reading the dirty
   content for the stash tree still works (read permission is untouched),
   but writing HEAD's content back over it does not. Confirms the caller
   gets -2 (not -1) and that the stash is durably listed despite the
   partial failure -- exactly what fix 3 in the milestone plan calls for. */
static void test_push_returns_minus_two_when_reset_fails(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char commit1[SG_SHA1_RAW_LEN];
    char abspath[4096];
    int rc;

    if (geteuid() == 0) {
        free(repo_root);
        free(git_dir);
        return;
    }

    commit_initial(git_dir, repo_root); /* HEAD: a.txt = "hello\n" */
    write_workdir_file(repo_root, "a.txt", "changed\n");

    snprintf(abspath, sizeof(abspath), "%s/a.txt", repo_root);
    CHECK(chmod(abspath, 0444) == 0, "chmod a.txt read-only failed");

    rc = stash_push(git_dir, repo_root, "will half-fail", commit1);
    CHECK(rc == -2, "expected -2 (stash durable, reset failed), got %d", rc);

    chmod(abspath, 0644); /* restore before any further reads/writes */

    {
        sg_stash_list list;

        /* commit_id_out (commit1) is deliberately left unfilled on the -2
           path -- only the 0 path fills it (see sg_stash_push) -- so the
           only thing to check here is that the stash it wrote before
           failing is durably listed. */
        CHECK(sg_stash_list_read(git_dir, &list) == 0, "list read failed after the -2 push");
        CHECK(list.count == 1, "the stash must be durably listed even though the reset failed, got %zu",
             list.count);
        if (list.count == 1)
            CHECK(strstr(list.entries[0].message, "will half-fail") != NULL,
                 "the listed entry should carry the -m message sg_stash_push was given: %s",
                 list.entries[0].message);
        sg_stash_list_free(&list);
    }

    free(repo_root);
    free(git_dir);
}

/* ---- sg_merge_result_apply's "clean entry write failed" branch -------- */

/* Forces the one branch in sg_merge_result_apply that phase15's interop
   coverage never reached: a clean (non-conflict) merge-result entry whose
   target path fails to write. Replacing the tracked target with a
   same-named directory makes sg_write_file_mkdirs's fopen(path, "wb") fail
   with EISDIR -- a directory is never mistaken for a writable regular file
   on any of the platforms this project supports. Calls sg_stash_apply
   directly (bypassing the CLI's sg_require_clean_workdir gate, which would
   otherwise refuse before this code path is ever reached) since the
   library function itself only guards untracked-in-HEAD collisions, not an
   existing tracked path being replaced by a directory. */
static void test_apply_fails_when_clean_write_target_is_a_directory(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char head1[SG_SHA1_RAW_LEN];
    unsigned char blob_d1[SG_SHA1_RAW_LEN];
    unsigned char tree2[SG_SHA1_RAW_LEN];
    unsigned char head2[SG_SHA1_RAW_LEN];
    unsigned char stash_commit[SG_SHA1_RAW_LEN];
    sg_index idx;
    sg_index_entry e;
    sg_commit commit;
    unsigned char *serialized;
    size_t serialized_len;
    char abspath[4096];
    int rc;

    commit_initial(git_dir, repo_root); /* HEAD: a.txt = "hello\n" */
    CHECK(sg_ref_resolve_head(git_dir, head1) == 0, "resolve head1 failed");

    /* second commit adds d/file.txt = "one\n" */
    write_workdir_file(repo_root, "d/file.txt", "one\n");
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "one\n", 4, blob_d1) == 0, "write blob d1 failed");
    CHECK(sg_index_read(git_dir, &idx) == 0, "read index failed");
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, blob_d1, SG_SHA1_RAW_LEN);
    e.path = (char *)"d/file.txt";
    CHECK(sg_index_upsert(&idx, &e) == 0, "upsert d/file.txt failed");
    CHECK(sg_index_write(git_dir, &idx) == 0, "write index failed");
    CHECK(sg_tree_build_from_index(git_dir, &idx, tree2) == 0, "build tree2 failed");
    sg_index_free(&idx);

    memset(&commit, 0, sizeof(commit));
    memcpy(commit.tree, tree2, SG_SHA1_RAW_LEN);
    commit.parents = malloc(sizeof(*commit.parents));
    CHECK(commit.parents != NULL, "alloc parents failed");
    memcpy(commit.parents[0], head1, SG_SHA1_RAW_LEN);
    commit.parent_count = 1;
    commit.author_name = (char *)"Test";
    commit.author_email = (char *)"test@example.com";
    commit.author_time = 1700000100;
    strcpy(commit.author_tz, "+0000");
    commit.committer_name = commit.author_name;
    commit.committer_email = commit.author_email;
    commit.committer_time = commit.author_time;
    strcpy(commit.committer_tz, "+0000");
    commit.message = (char *)"add d/file.txt\n";
    CHECK(sg_commit_serialize(&commit, &serialized, &serialized_len) == 0, "serialize commit2 failed");
    CHECK(sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len, head2) == 0,
         "write commit2 failed");
    free(serialized);
    free(commit.parents);
    CHECK(sg_ref_update_branch(git_dir, "master", head2) == 0, "update branch to head2 failed");

    /* stash a change to d/file.txt (push resets the workdir back to HEAD,
       so d/file.txt is "one\n" again afterward) */
    write_workdir_file(repo_root, "d/file.txt", "two\n");
    rc = stash_push(git_dir, repo_root, "change d", stash_commit);
    CHECK(rc == 0, "stash push failed, rc=%d", rc);

    /* replace the tracked file with a directory of the same name */
    snprintf(abspath, sizeof(abspath), "%s/d/file.txt", repo_root);
    CHECK(remove(abspath) == 0, "failed to remove d/file.txt before replacing it with a directory");
    CHECK(mkdir(abspath, 0755) == 0, "failed to mkdir in place of d/file.txt");

    rc = sg_stash_apply(git_dir, repo_root, 0, 0);
    CHECK(rc == -1, "expected sg_stash_apply to fail when a clean write target is a directory, got %d",
         rc);

    {
        sg_stash_list list;

        CHECK(sg_stash_list_read(git_dir, &list) == 0, "list read failed after the failed apply");
        CHECK(list.count == 1, "the stash must not be dropped after a failed apply, got %zu", list.count);
        sg_stash_list_free(&list);
    }

    free(repo_root);
    free(git_dir);
}

/* ---- --keep-index: reset to the index tree instead of HEAD's ------------ */

/* Builds the five-file fixture from the Phase 20 spec's --keep-index table
   (root commit: tracked.txt=v1, staged.txt=s1, staged_del.txt and wt_del.txt
   both present) directly through the index/tree/commit APIs, mirroring
   commit_initial but for four files instead of one. */
static void keep_index_base_repo(const char *git_dir, const char *repo_root)
{
    sg_index idx;
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    sg_commit commit;
    unsigned char *serialized;
    size_t serialized_len;
    struct {
        const char *path;
        const char *content;
    } files[4] = {
        {"staged.txt", "s1\n"},
        {"staged_del.txt", "del content\n"},
        {"tracked.txt", "v1\n"},
        {"wt_del.txt", "wtdel content\n"},
    };
    size_t i;

    memset(&idx, 0, sizeof(idx));
    for (i = 0; i < 4; i++) {
        unsigned char blob[SG_SHA1_RAW_LEN];
        sg_index_entry e;

        write_workdir_file(repo_root, files[i].path, files[i].content);
        CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, files[i].content, strlen(files[i].content), blob) == 0,
             "write blob for %s", files[i].path);
        memset(&e, 0, sizeof(e));
        e.mode = 0100644;
        memcpy(e.sha1, blob, SG_SHA1_RAW_LEN);
        e.path = (char *)files[i].path;
        CHECK(sg_index_upsert(&idx, &e) == 0, "upsert %s", files[i].path);
    }
    CHECK(sg_index_write(git_dir, &idx) == 0, "write initial index");

    CHECK(sg_tree_build_from_index(git_dir, &idx, tree_id) == 0, "build initial tree");
    sg_index_free(&idx);

    memset(&commit, 0, sizeof(commit));
    memcpy(commit.tree, tree_id, SG_SHA1_RAW_LEN);
    commit.parent_count = 0;
    commit.author_name = (char *)"Test";
    commit.author_email = (char *)"test@example.com";
    commit.author_time = 1700000200;
    strcpy(commit.author_tz, "+0000");
    commit.committer_name = commit.author_name;
    commit.committer_email = commit.author_email;
    commit.committer_time = commit.author_time;
    strcpy(commit.committer_tz, "+0000");
    commit.message = (char *)"initial\n";
    CHECK(sg_commit_serialize(&commit, &serialized, &serialized_len) == 0, "serialize initial commit");
    CHECK(sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len, commit_id) == 0,
         "write initial commit");
    free(serialized);
    CHECK(sg_ref_update_branch(git_dir, "master", commit_id) == 0, "update branch to initial commit");
}

/* Applies the five per-file mutations the spec's table is built on:
   tracked.txt unstaged-modified, staged.txt staged-modified, new_staged.txt
   staged as a brand new file, staged_del.txt staged for deletion (removed
   from the index, left on disk -- what `git rm --cached` produces), and
   wt_del.txt deleted from the working tree without being staged. Callable
   more than once against the same repo as long as the tree has been reset
   back to HEAD in between (sg_stash_push's own job). */
static void apply_five_state_mutations(const char *git_dir, const char *repo_root)
{
    sg_index idx;
    unsigned char blob_s2[SG_SHA1_RAW_LEN];
    unsigned char blob_ns[SG_SHA1_RAW_LEN];
    sg_index_entry e;
    char abspath[4096];

    CHECK(sg_index_read(git_dir, &idx) == 0, "read index before mutating");

    write_workdir_file(repo_root, "tracked.txt", "v2\n");

    write_workdir_file(repo_root, "staged.txt", "s2\n");
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "s2\n", 3, blob_s2) == 0, "write s2 blob");
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, blob_s2, SG_SHA1_RAW_LEN);
    e.path = (char *)"staged.txt";
    CHECK(sg_index_upsert(&idx, &e) == 0, "restage staged.txt at s2");

    write_workdir_file(repo_root, "new_staged.txt", "ns1\n");
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "ns1\n", 4, blob_ns) == 0, "write ns1 blob");
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, blob_ns, SG_SHA1_RAW_LEN);
    e.path = (char *)"new_staged.txt";
    CHECK(sg_index_upsert(&idx, &e) == 0, "stage new_staged.txt");

    CHECK(sg_index_remove(&idx, "staged_del.txt") == 0, "stage removal of staged_del.txt");

    CHECK(sg_index_write(git_dir, &idx) == 0, "write mutated index");
    sg_index_free(&idx);

    snprintf(abspath, sizeof(abspath), "%s/wt_del.txt", repo_root);
    CHECK(remove(abspath) == 0, "delete wt_del.txt from the working tree");
}

static int file_exists(const char *repo_root, const char *rel)
{
    char abspath[4096];
    struct stat st;

    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, rel);
    return stat(abspath, &st) == 0;
}

static void test_keep_index_table(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char stash_default[SG_SHA1_RAW_LEN];
    unsigned char stash_keep[SG_SHA1_RAW_LEN];

    keep_index_base_repo(git_dir, repo_root);

    /* ---- control: no flag, resets to HEAD's tree ---- */
    apply_five_state_mutations(git_dir, repo_root);
    CHECK(stash_push(git_dir, repo_root, "control", stash_default) == 0, "control push failed");
    {
        char *tracked = read_workdir_file(repo_root, "tracked.txt");
        char *staged = read_workdir_file(repo_root, "staged.txt");
        char *wtdel = read_workdir_file(repo_root, "wt_del.txt");
        sg_index idx;

        CHECK(tracked != NULL && strcmp(tracked, "v1\n") == 0,
             "no-flag: tracked.txt should be reset to HEAD (v1), got %s",
             tracked != NULL ? tracked : "(null)");
        CHECK(staged != NULL && strcmp(staged, "s1\n") == 0,
             "no-flag: staged.txt should be reset to HEAD (s1), not kept at s2, got %s",
             staged != NULL ? staged : "(null)");
        CHECK(!file_exists(repo_root, "new_staged.txt"),
             "no-flag: new_staged.txt should not exist (it was never in HEAD)");
        CHECK(file_exists(repo_root, "staged_del.txt"),
             "no-flag: staged_del.txt should still exist (HEAD has it)");
        {
            char *delcontent = read_workdir_file(repo_root, "staged_del.txt");

            CHECK(delcontent != NULL && strcmp(delcontent, "del content\n") == 0,
                 "no-flag: staged_del.txt should carry HEAD's content, got %s",
                 delcontent != NULL ? delcontent : "(null)");
            free(delcontent);
        }
        CHECK(wtdel != NULL && strcmp(wtdel, "wtdel content\n") == 0,
             "no-flag: wt_del.txt should be restored, got %s", wtdel != NULL ? wtdel : "(null)");

        CHECK(sg_index_read(git_dir, &idx) == 0, "read index after no-flag push");
        {
            int pos = sg_index_find(&idx, "staged.txt");
            unsigned char blob_s1[SG_SHA1_RAW_LEN];

            CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "s1\n", 3, blob_s1) == 0,
                 "recompute the s1 blob id for comparison");
            CHECK(pos >= 0 && memcmp(idx.entries[pos].sha1, blob_s1, SG_SHA1_RAW_LEN) == 0,
                 "no-flag: staged.txt's index entry should be back at the s1 (HEAD) blob");
        }
        CHECK(sg_index_find(&idx, "new_staged.txt") < 0,
             "no-flag: new_staged.txt should not be in the index");
        CHECK(sg_index_find(&idx, "staged_del.txt") >= 0,
             "no-flag: staged_del.txt should be re-staged (reset to HEAD restages everything)");
        sg_index_free(&idx);

        free(tracked);
        free(staged);
        free(wtdel);
    }

    /* ---- --keep-index: resets to the INDEX tree, keeping staged changes
       staged (and re-applying a staged deletion for real) ---- */
    apply_five_state_mutations(git_dir, repo_root);
    CHECK(stash_push_keep_index(git_dir, repo_root, "keepidx", stash_keep) == 0, "--keep-index push failed");
    {
        char *tracked = read_workdir_file(repo_root, "tracked.txt");
        char *staged = read_workdir_file(repo_root, "staged.txt");
        char *newstaged = read_workdir_file(repo_root, "new_staged.txt");
        char *wtdel = read_workdir_file(repo_root, "wt_del.txt");
        sg_index idx;

        CHECK(tracked != NULL && strcmp(tracked, "v1\n") == 0,
             "--keep-index: tracked.txt should still be reset to HEAD (v1), got %s",
             tracked != NULL ? tracked : "(null)");
        CHECK(staged != NULL && strcmp(staged, "s2\n") == 0,
             "--keep-index: staged.txt should be kept at the staged content (s2), got %s",
             staged != NULL ? staged : "(null)");
        CHECK(newstaged != NULL && strcmp(newstaged, "ns1\n") == 0,
             "--keep-index: new_staged.txt should be kept, got %s",
             newstaged != NULL ? newstaged : "(null)");
        CHECK(!file_exists(repo_root, "staged_del.txt"),
             "--keep-index: staged_del.txt should be DELETED (the staged removal is re-applied), "
             "not left behind the way the no-flag run leaves it");
        CHECK(wtdel != NULL && strcmp(wtdel, "wtdel content\n") == 0,
             "--keep-index: wt_del.txt should be restored, got %s", wtdel != NULL ? wtdel : "(null)");

        CHECK(sg_index_read(git_dir, &idx) == 0, "read index after --keep-index push");
        CHECK(sg_index_find(&idx, "staged.txt") >= 0, "--keep-index: staged.txt should still be tracked");
        {
            int pos = sg_index_find(&idx, "staged.txt");
            unsigned char blob_s2[SG_SHA1_RAW_LEN];

            CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "s2\n", 3, blob_s2) == 0,
                 "recompute the s2 blob id for comparison");
            CHECK(pos >= 0 && memcmp(idx.entries[pos].sha1, blob_s2, SG_SHA1_RAW_LEN) == 0,
                 "--keep-index: staged.txt's index entry should point at the s2 blob");
        }
        CHECK(sg_index_find(&idx, "new_staged.txt") >= 0,
             "--keep-index: new_staged.txt should still be staged in the index");
        CHECK(sg_index_find(&idx, "staged_del.txt") < 0,
             "--keep-index: staged_del.txt should stay absent from the index");
        sg_index_free(&idx);

        free(tracked);
        free(staged);
        free(newstaged);
        free(wtdel);
    }

    free(repo_root);
    free(git_dir);
}

/* index_tree == head_tree (no staged changes at all) -- --keep-index must be
   a no-op that behaves exactly like the no-flag path: both reset the same
   single tree. */
static void test_keep_index_noop_when_index_matches_head(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char stash_id[SG_SHA1_RAW_LEN];

    commit_initial(git_dir, repo_root); /* a.txt = "hello\n" */

    write_workdir_file(repo_root, "a.txt", "unstaged only\n");
    CHECK(stash_push_keep_index(git_dir, repo_root, "noop case", stash_id) == 0,
         "--keep-index push failed when index == HEAD");
    {
        char *content = read_workdir_file(repo_root, "a.txt");

        CHECK(content != NULL && strcmp(content, "hello\n") == 0,
             "--keep-index with a clean index should reset a.txt to HEAD just like the no-flag path, "
             "got %s",
             content != NULL ? content : "(null)");
        free(content);
    }

    free(repo_root);
    free(git_dir);
}

/* Same wrapper family as stash_push/stash_push_keep_index, for -u/-a. */
static int stash_push_u(const char *git_dir, const char *repo_root, const char *message,
                        unsigned char commit_id_out[SG_SHA1_RAW_LEN])
{
    sg_stash_push_opts opts;

    memset(&opts, 0, sizeof(opts));
    opts.message = message;
    opts.include_untracked = 1;
    return sg_stash_push(git_dir, repo_root, &opts, commit_id_out, NULL);
}

static int stash_push_a(const char *git_dir, const char *repo_root, const char *message,
                        unsigned char commit_id_out[SG_SHA1_RAW_LEN])
{
    sg_stash_push_opts opts;

    memset(&opts, 0, sizeof(opts));
    opts.message = message;
    opts.include_ignored = 1;
    return sg_stash_push(git_dir, repo_root, &opts, commit_id_out, NULL);
}

static int stash_push_u_keep_index(const char *git_dir, const char *repo_root, const char *message,
                                   unsigned char commit_id_out[SG_SHA1_RAW_LEN])
{
    sg_stash_push_opts opts;

    memset(&opts, 0, sizeof(opts));
    opts.message = message;
    opts.include_untracked = 1;
    opts.keep_index = 1;
    return sg_stash_push(git_dir, repo_root, &opts, commit_id_out, NULL);
}

static void read_commit(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN], sg_commit *out)
{
    sg_obj_type type;
    unsigned char *content = NULL;
    size_t content_len;

    CHECK(sg_object_read(git_dir, id, &type, &content, &content_len) == 0 && type == SG_OBJ_COMMIT,
         "failed to read commit object");
    CHECK(sg_commit_parse(content, content_len, out) == 0, "failed to parse commit object");
    free(content);
}

static int dir_exists(const char *repo_root, const char *rel)
{
    char abspath[4096];
    struct stat st;

    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, rel);
    return stat(abspath, &st) == 0 && S_ISDIR(st.st_mode);
}

static const sg_flat_entry *flat_find(const sg_flat_list *list, const char *path)
{
    size_t i;

    for (i = 0; i < list->count; i++) {
        if (strcmp(list->entries[i].path, path) == 0)
            return &list->entries[i];
    }
    return NULL;
}

/* ---- -u's stash tree must be BYTE-FOR-BYTE identical to the no-flag tree
   (spec sec 1.1) -- untracked files live ONLY in the third parent. Two
   parallel repos, identical fixture, differing only in the flag. ---------- */
static void test_u_stash_tree_matches_noflag_tree(void)
{
    char *git_dir_a = make_tmp_repo();
    char *repo_root_a = sg_repo_root(git_dir_a);
    char *git_dir_b = make_tmp_repo();
    char *repo_root_b = sg_repo_root(git_dir_b);
    unsigned char commit_noflag[SG_SHA1_RAW_LEN];
    unsigned char commit_u[SG_SHA1_RAW_LEN];
    sg_commit c_noflag, c_u;

    commit_initial(git_dir_a, repo_root_a);
    write_workdir_file(repo_root_a, "a.txt", "changed\n");
    CHECK(stash_push(git_dir_a, repo_root_a, "ctl", commit_noflag) == 0, "no-flag push failed");

    commit_initial(git_dir_b, repo_root_b);
    write_workdir_file(repo_root_b, "a.txt", "changed\n");
    write_workdir_file(repo_root_b, "fresh/inner.txt", "f1\n");
    CHECK(stash_push_u(git_dir_b, repo_root_b, "ctl", commit_u) == 0, "-u push failed");

    read_commit(git_dir_a, commit_noflag, &c_noflag);
    read_commit(git_dir_b, commit_u, &c_u);
    CHECK(memcmp(c_noflag.tree, c_u.tree, SG_SHA1_RAW_LEN) == 0,
         "the stash's own tree must be identical with and without -u (untracked files must not leak "
         "into it)");
    CHECK(c_u.parent_count == 3, "expected a 3-parent stash under -u, got %zu", c_u.parent_count);
    sg_commit_free(&c_noflag);
    sg_commit_free(&c_u);

    free(repo_root_a);
    free(git_dir_a);
    free(repo_root_b);
    free(git_dir_b);
}

/* ---- the third parent must be a ROOT commit (no parent of its own) ------ */
static void test_third_parent_is_root_commit(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char stash_id[SG_SHA1_RAW_LEN];
    sg_commit stash_commit, untracked_commit;

    commit_initial(git_dir, repo_root);
    write_workdir_file(repo_root, "a.txt", "changed\n");
    write_workdir_file(repo_root, "u.txt", "untracked\n");
    CHECK(stash_push_u(git_dir, repo_root, "msg", stash_id) == 0, "-u push failed");

    read_commit(git_dir, stash_id, &stash_commit);
    CHECK(stash_commit.parent_count == 3, "expected 3 parents, got %zu", stash_commit.parent_count);
    if (stash_commit.parent_count == 3) {
        read_commit(git_dir, stash_commit.parents[2], &untracked_commit);
        CHECK(untracked_commit.parent_count == 0,
             "the third (untracked) parent must be a root commit, got %zu parents",
             untracked_commit.parent_count);
        sg_commit_free(&untracked_commit);
    }
    sg_commit_free(&stash_commit);

    free(repo_root);
    free(git_dir);
}

/* ---- negative assertion: -u must NOT pull in ignored files; -a must ----- */
static void test_u_excludes_ignored_a_includes(void)
{
    char *git_dir_u = make_tmp_repo();
    char *repo_root_u = sg_repo_root(git_dir_u);
    char *git_dir_a = make_tmp_repo();
    char *repo_root_a = sg_repo_root(git_dir_a);
    unsigned char stash_u[SG_SHA1_RAW_LEN];
    unsigned char stash_a[SG_SHA1_RAW_LEN];
    sg_commit commit_u, commit_a;
    unsigned char tree_u[SG_SHA1_RAW_LEN];
    unsigned char tree_a[SG_SHA1_RAW_LEN];
    sg_flat_list flat_u, flat_a;

    commit_initial(git_dir_u, repo_root_u);
    write_workdir_file(repo_root_u, ".gitignore", "keep.log\n");
    write_workdir_file(repo_root_u, "take.txt", "t\n");
    write_workdir_file(repo_root_u, "keep.log", "k\n");
    CHECK(stash_push_u(git_dir_u, repo_root_u, "u", stash_u) == 0, "-u push failed");

    commit_initial(git_dir_a, repo_root_a);
    write_workdir_file(repo_root_a, ".gitignore", "keep.log\n");
    write_workdir_file(repo_root_a, "take.txt", "t\n");
    write_workdir_file(repo_root_a, "keep.log", "k\n");
    CHECK(stash_push_a(git_dir_a, repo_root_a, "a", stash_a) == 0, "-a push failed");

    read_commit(git_dir_u, stash_u, &commit_u);
    read_commit(git_dir_a, stash_a, &commit_a);
    CHECK(commit_u.parent_count == 3 && commit_a.parent_count == 3, "expected 3-parent stashes");
    memcpy(tree_u, commit_u.tree, SG_SHA1_RAW_LEN); /* silence unused warnings if parent_count check fails */
    (void)tree_u;
    if (commit_u.parent_count == 3) {
        unsigned char untracked_id[SG_SHA1_RAW_LEN];
        sg_commit untracked;

        memcpy(untracked_id, commit_u.parents[2], SG_SHA1_RAW_LEN);
        read_commit(git_dir_u, untracked_id, &untracked);
        memcpy(tree_u, untracked.tree, SG_SHA1_RAW_LEN);
        sg_commit_free(&untracked);
        CHECK(sg_tree_flatten(git_dir_u, tree_u, &flat_u, NULL) == 0, "flatten -u tree failed");
        CHECK(flat_find(&flat_u, "take.txt") != NULL, "-u must include take.txt");
        CHECK(flat_find(&flat_u, "keep.log") == NULL, "-u must NOT include the ignored keep.log");
        sg_flat_list_free(&flat_u);
    }
    if (commit_a.parent_count == 3) {
        unsigned char untracked_id[SG_SHA1_RAW_LEN];
        sg_commit untracked;

        memcpy(untracked_id, commit_a.parents[2], SG_SHA1_RAW_LEN);
        read_commit(git_dir_a, untracked_id, &untracked);
        memcpy(tree_a, untracked.tree, SG_SHA1_RAW_LEN);
        sg_commit_free(&untracked);
        CHECK(sg_tree_flatten(git_dir_a, tree_a, &flat_a, NULL) == 0, "flatten -a tree failed");
        CHECK(flat_find(&flat_a, "take.txt") != NULL, "-a must include take.txt");
        CHECK(flat_find(&flat_a, "keep.log") != NULL, "-a MUST include the ignored keep.log too");
        sg_flat_list_free(&flat_a);
    }
    sg_commit_free(&commit_u);
    sg_commit_free(&commit_a);

    free(repo_root_u);
    free(git_dir_u);
    free(repo_root_a);
    free(git_dir_a);
}

/* ---- -m must not leak into the index/untracked parents' subjects (spec
   sec 1.2) --------------------------------------------------------------- */
static void test_message_does_not_affect_index_and_untracked_subjects(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char stash_id[SG_SHA1_RAW_LEN];
    sg_commit stash_commit, index_commit, untracked_commit;

    commit_initial(git_dir, repo_root);
    write_workdir_file(repo_root, "a.txt", "changed\n");
    write_workdir_file(repo_root, "u.txt", "untracked\n");
    CHECK(stash_push_u(git_dir, repo_root, "my custom message", stash_id) == 0, "-u push failed");

    read_commit(git_dir, stash_id, &stash_commit);
    CHECK(strstr(stash_commit.message, "my custom message") != NULL,
         "the stash's own subject should carry -m's text: %s", stash_commit.message);
    if (stash_commit.parent_count == 3) {
        read_commit(git_dir, stash_commit.parents[1], &index_commit);
        CHECK(strstr(index_commit.message, "my custom message") == NULL,
             "-m must not leak into the index parent's subject: %s", index_commit.message);
        CHECK(strstr(index_commit.message, "index on") != NULL,
             "the index parent should keep its default subject form: %s", index_commit.message);
        sg_commit_free(&index_commit);

        read_commit(git_dir, stash_commit.parents[2], &untracked_commit);
        CHECK(strstr(untracked_commit.message, "my custom message") == NULL,
             "-m must not leak into the untracked parent's subject: %s", untracked_commit.message);
        CHECK(strstr(untracked_commit.message, "untracked files on") != NULL,
             "the untracked parent should keep its default subject form: %s", untracked_commit.message);
        sg_commit_free(&untracked_commit);
    }
    sg_commit_free(&stash_commit);

    free(repo_root);
    free(git_dir);
}

/* ---- an empty untracked list still produces 3 parents, empty tree (spec
   sec 1.6) ----------------------------------------------------------------- */
static void test_empty_untracked_list_still_three_parents(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char stash_id[SG_SHA1_RAW_LEN];
    sg_commit stash_commit, untracked_commit;
    char hex[SG_SHA1_HEX_LEN + 1];

    commit_initial(git_dir, repo_root);
    write_workdir_file(repo_root, "a.txt", "changed\n"); /* tracked change only, no untracked files */
    CHECK(stash_push_u(git_dir, repo_root, "msg", stash_id) == 0, "-u push failed");

    read_commit(git_dir, stash_id, &stash_commit);
    CHECK(stash_commit.parent_count == 3,
         "expected 3 parents even with zero untracked files, got %zu", stash_commit.parent_count);
    if (stash_commit.parent_count == 3) {
        read_commit(git_dir, stash_commit.parents[2], &untracked_commit);
        sg_sha1_to_hex(untracked_commit.tree, hex);
        CHECK(strcmp(hex, "4b825dc642cb6eb9a060e54bf8d69288fbee4904") == 0,
             "the third parent's tree should be git's well-known empty tree, got %s", hex);
        sg_commit_free(&untracked_commit);
    }
    sg_commit_free(&stash_commit);

    free(repo_root);
    free(git_dir);
}

/* ---- nothing-to-save boundaries under -u/-a (spec sec 1.5) -------------- */
static void test_nothing_to_do_boundaries(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char id[SG_SHA1_RAW_LEN];

    commit_initial(git_dir, repo_root);

    /* clean tree + -u -> nothing to save */
    CHECK(stash_push_u(git_dir, repo_root, NULL, id) == 1,
         "clean tree + -u should report nothing to save");

    /* only an ignored file present + -u -> nothing to save; the SAME fixture
       + -a DOES create a stash */
    write_workdir_file(repo_root, ".gitignore", "ignored.txt\n");
    /* .gitignore itself is untracked here too, so ignore it in this probe by
       committing it first, leaving only ignored.txt untracked */
    {
        unsigned char blob[SG_SHA1_RAW_LEN];
        sg_index idx;
        sg_index_entry e;
        unsigned char tree_id[SG_SHA1_RAW_LEN];
        sg_commit c;
        unsigned char *serialized;
        size_t serialized_len;
        unsigned char commit_id[SG_SHA1_RAW_LEN];
        unsigned char head_before[SG_SHA1_RAW_LEN];
        sg_flat_entry entries[2];
        unsigned char blob_a[SG_SHA1_RAW_LEN];

        CHECK(sg_ref_resolve_head(git_dir, head_before) == 0, "resolve head failed");
        CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "ignored.txt\n", 12, blob) == 0, "write gi blob");
        CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "hello\n", 6, blob_a) == 0, "write a blob");
        memset(&idx, 0, sizeof(idx));
        memset(&e, 0, sizeof(e));
        e.mode = 0100644;
        memcpy(e.sha1, blob_a, SG_SHA1_RAW_LEN);
        e.path = (char *)"a.txt";
        CHECK(sg_index_upsert(&idx, &e) == 0, "upsert a.txt");
        memset(&e, 0, sizeof(e));
        e.mode = 0100644;
        memcpy(e.sha1, blob, SG_SHA1_RAW_LEN);
        e.path = (char *)".gitignore";
        CHECK(sg_index_upsert(&idx, &e) == 0, "upsert .gitignore");
        CHECK(sg_index_write(git_dir, &idx) == 0, "write index");

        entries[0].path = (char *)".gitignore";
        entries[0].mode = 0100644;
        memcpy(entries[0].sha1, blob, SG_SHA1_RAW_LEN);
        entries[1].path = (char *)"a.txt";
        entries[1].mode = 0100644;
        memcpy(entries[1].sha1, blob_a, SG_SHA1_RAW_LEN);
        CHECK(sg_tree_build(git_dir, entries, 2, tree_id) == 0, "build tree");
        sg_index_free(&idx);

        memset(&c, 0, sizeof(c));
        memcpy(c.tree, tree_id, SG_SHA1_RAW_LEN);
        c.parents = malloc(sizeof(*c.parents));
        memcpy(c.parents[0], head_before, SG_SHA1_RAW_LEN);
        c.parent_count = 1;
        c.author_name = (char *)"Test";
        c.author_email = (char *)"test@example.com";
        c.author_time = 1700000300;
        strcpy(c.author_tz, "+0000");
        c.committer_name = c.author_name;
        c.committer_email = c.author_email;
        c.committer_time = c.author_time;
        strcpy(c.committer_tz, "+0000");
        c.message = (char *)"add gitignore\n";
        CHECK(sg_commit_serialize(&c, &serialized, &serialized_len) == 0, "serialize");
        CHECK(sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len, commit_id) == 0,
             "write commit");
        free(serialized);
        free(c.parents);
        CHECK(sg_ref_update_branch(git_dir, "master", commit_id) == 0, "update branch");
    }
    write_workdir_file(repo_root, "ignored.txt", "i\n");
    CHECK(stash_push_u(git_dir, repo_root, NULL, id) == 1,
         "clean tree + only an ignored untracked file + -u should report nothing to save");
    CHECK(stash_push_a(git_dir, repo_root, NULL, id) == 0,
         "the SAME fixture with -a should create a stash (the ignored file counts)");

    free(repo_root);
    free(git_dir);
}

/* ---- workdir cleanup boundaries (spec sec 1.4) --------------------------- */
static void test_workdir_cleanup_boundaries(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char id[SG_SHA1_RAW_LEN];

    commit_initial(git_dir, repo_root);
    write_workdir_file(repo_root, "a.txt", "changed\n");
    write_workdir_file(repo_root, ".gitignore", "ignored_only/\nmixed/keep.log\nbuild/\n");
    write_workdir_file(repo_root, "ignored_only/x.txt", "x\n");
    write_workdir_file(repo_root, "mixed/keep.log", "k\n");
    write_workdir_file(repo_root, "mixed/take.txt", "t\n");
    {
        char abspath[4096];

        snprintf(abspath, sizeof(abspath), "%s/emptydir", repo_root);
        CHECK(mkdir(abspath, 0755) == 0 || errno == EEXIST, "failed to create emptydir");
        /* build/ is itself matched by .gitignore AND physically empty --
           the boundary a bare "physically empty -> remove" rule (with no
           ignore check at all) would get wrong: this dir must be left alone
           under -u (it is ignored, and -u never touches ignored paths),
           unlike emptydir/ above which is empty but NOT ignored. Measured
           against real git 2.55.0. */
        snprintf(abspath, sizeof(abspath), "%s/build", repo_root);
        CHECK(mkdir(abspath, 0755) == 0 || errno == EEXIST, "failed to create build/ (ignored, empty)");
    }

    CHECK(stash_push_u(git_dir, repo_root, "cleanup", id) == 0, "-u push failed");

    CHECK(dir_exists(repo_root, "ignored_only"),
         "-u must NOT remove ignored_only/ (x.txt, ignored, is still there)");
    CHECK(file_exists(repo_root, "ignored_only/x.txt"), "-u must leave ignored_only/x.txt on disk");
    CHECK(dir_exists(repo_root, "mixed"), "-u must NOT remove mixed/ (keep.log, ignored, is still there)");
    CHECK(file_exists(repo_root, "mixed/keep.log"), "-u must leave mixed/keep.log on disk");
    CHECK(!file_exists(repo_root, "mixed/take.txt"), "-u must remove the taken mixed/take.txt");
    CHECK(!dir_exists(repo_root, "emptydir"), "-u must remove a directory that was already fully empty");
    CHECK(dir_exists(repo_root, "build"),
         "-u must NOT remove build/ even though it is physically empty: it is itself ignored, and -u "
         "never touches ignored paths (this is the boundary a missing ignore-check regresses)");

    free(repo_root);
    free(git_dir);
}

/* Same fixture, -a: ignored_only/ loses its only file and must be removed,
   and build/ (ignored AND empty) must be removed too -- -a sweeps ignored
   paths, so the ignore check that spares it under -u must not spare it
   here. */
static void test_workdir_cleanup_ignored_dir_removed_by_a(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char id[SG_SHA1_RAW_LEN];

    commit_initial(git_dir, repo_root);
    write_workdir_file(repo_root, "a.txt", "changed\n");
    write_workdir_file(repo_root, ".gitignore", "ignored_only/\nmixed/keep.log\nbuild/\n");
    write_workdir_file(repo_root, "ignored_only/x.txt", "x\n");
    write_workdir_file(repo_root, "mixed/keep.log", "k\n");
    write_workdir_file(repo_root, "mixed/take.txt", "t\n");
    {
        char abspath[4096];

        snprintf(abspath, sizeof(abspath), "%s/build", repo_root);
        CHECK(mkdir(abspath, 0755) == 0 || errno == EEXIST, "failed to create build/ (ignored, empty)");
    }

    CHECK(stash_push_a(git_dir, repo_root, "cleanup-a", id) == 0, "-a push failed");

    CHECK(!dir_exists(repo_root, "ignored_only"),
         "-a must remove ignored_only/ once its only (ignored) file is taken too");
    CHECK(!dir_exists(repo_root, "mixed"), "-a must remove mixed/ once both files are taken");
    CHECK(!dir_exists(repo_root, "build"), "-a must remove build/ (ignored and empty)");

    free(repo_root);
    free(git_dir);
}

/* ---- -u composes with --keep-index: index stays staged, untracked files
   are still taken and swept from the working tree -------------------------- */
static void test_u_with_keep_index(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char id[SG_SHA1_RAW_LEN];
    sg_index idx;

    keep_index_base_repo(git_dir, repo_root);
    apply_five_state_mutations(git_dir, repo_root);
    write_workdir_file(repo_root, "fresh/inner.txt", "f1\n");

    CHECK(stash_push_u_keep_index(git_dir, repo_root, "u+keep", id) == 0, "-u --keep-index push failed");

    CHECK(!dir_exists(repo_root, "fresh"), "-u should remove fresh/ once inner.txt is taken");
    {
        char *staged = read_workdir_file(repo_root, "staged.txt");

        CHECK(staged != NULL && strcmp(staged, "s2\n") == 0,
             "--keep-index should still keep staged.txt at s2 alongside -u, got %s",
             staged != NULL ? staged : "(null)");
        free(staged);
    }
    CHECK(sg_index_read(git_dir, &idx) == 0, "read index after -u --keep-index push");
    CHECK(sg_index_find(&idx, "staged.txt") >= 0, "staged.txt should still be tracked in the index");
    sg_index_free(&idx);

    free(repo_root);
    free(git_dir);
}

/* ---- apply/pop restore the untracked half to disk, unstaged, nested dirs
   included; 2-or-3-parent widening (spec sec 1.7) -------------------------- */
static void test_apply_restores_untracked_unstaged(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char stash_id[SG_SHA1_RAW_LEN];
    sg_index idx;
    int rc;

    commit_initial(git_dir, repo_root);
    write_workdir_file(repo_root, "a.txt", "changed\n");
    write_workdir_file(repo_root, "fresh/inner.txt", "f1\n");
    CHECK(stash_push_u(git_dir, repo_root, "u", stash_id) == 0, "-u push failed");
    CHECK(!file_exists(repo_root, "fresh/inner.txt"), "push should have removed fresh/inner.txt");

    rc = sg_stash_apply(git_dir, repo_root, 0, 0);
    CHECK(rc == 0, "expected a clean apply of a 3-parent stash, got %d", rc);

    {
        char *content = read_workdir_file(repo_root, "fresh/inner.txt");

        CHECK(content != NULL && strcmp(content, "f1\n") == 0,
             "fresh/inner.txt should be restored with its original content, got %s",
             content != NULL ? content : "(null)");
        free(content);
    }
    CHECK(sg_index_read(git_dir, &idx) == 0, "read index after apply");
    CHECK(sg_index_find(&idx, "fresh/inner.txt") < 0,
         "the restored untracked file must NOT be staged");
    sg_index_free(&idx);

    free(repo_root);
    free(git_dir);
}

/* ---- collision on the untracked half rejects the WHOLE apply (spec sec
   1.7's deliberate all-or-nothing divergence): a.txt must stay at its
   pre-apply content, not the stash's -------------------------------------- */
static void test_apply_untracked_collision_rejects_whole_apply(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char stash_id[SG_SHA1_RAW_LEN];
    int rc;

    commit_initial(git_dir, repo_root); /* HEAD: a.txt = "hello\n" */
    write_workdir_file(repo_root, "a.txt", "changed\n");
    write_workdir_file(repo_root, "u.txt", "original\n");
    CHECK(stash_push_u(git_dir, repo_root, "u", stash_id) == 0, "-u push failed");
    CHECK(!file_exists(repo_root, "u.txt"), "push should have removed u.txt");

    /* something else now occupies the path the untracked half would restore */
    write_workdir_file(repo_root, "u.txt", "COLLIDE\n");

    rc = sg_stash_apply(git_dir, repo_root, 0, 0);
    CHECK(rc == -1, "expected the whole apply to be rejected on an untracked collision, got %d", rc);

    {
        char *a = read_workdir_file(repo_root, "a.txt");
        char *u = read_workdir_file(repo_root, "u.txt");

        CHECK(a != NULL && strcmp(a, "hello\n") == 0,
             "a.txt must stay at HEAD's content (nothing written at all), got %s",
             a != NULL ? a : "(null)");
        CHECK(u != NULL && strcmp(u, "COLLIDE\n") == 0,
             "u.txt must keep the colliding content, untouched, got %s", u != NULL ? u : "(null)");
        free(a);
        free(u);
    }
    {
        sg_stash_list list;

        CHECK(sg_stash_list_read(git_dir, &list) == 0, "list read failed");
        CHECK(list.count == 1, "the stash must survive a rejected apply, got %zu", list.count);
        sg_stash_list_free(&list);
    }

    free(repo_root);
    free(git_dir);
}

/* ---- -2 branch: the second (keep-index) sg_apply_tree_to_workdir call
   fails after the first one already succeeded ---------------------------- */

/* "blocked/c.txt" is staged (so it is part of index_tree, the --keep-index
   re-apply's target) but absent from HEAD (so the first, head_tree, apply
   does not need to touch "blocked" at all -- it only ever tries to *remove*
   the working-tree copy, and that removal's return value is discarded, see
   sg_apply_tree_to_workdir). The working-tree copy is deleted and "blocked"
   is left as an empty, write-less directory, so the SECOND apply -- which
   must create c.txt fresh -- fails opening it while the first apply, which
   never needed to create anything under "blocked", still succeeds. */
static void test_push_returns_minus_two_when_keep_index_second_apply_fails(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char blob[SG_SHA1_RAW_LEN];
    unsigned char commit1[SG_SHA1_RAW_LEN];
    sg_index idx;
    sg_index_entry e;
    char blocked_dir[4096];
    char blocked_file[4096];
    int rc;

    if (geteuid() == 0) {
        free(repo_root);
        free(git_dir);
        return;
    }

    commit_initial(git_dir, repo_root); /* HEAD: a.txt = "hello\n" */

    write_workdir_file(repo_root, "blocked/c.txt", "orig\n");
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "orig\n", 5, blob) == 0, "write blob for blocked/c.txt");
    CHECK(sg_index_read(git_dir, &idx) == 0, "read index before staging blocked/c.txt");
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, blob, SG_SHA1_RAW_LEN);
    e.path = (char *)"blocked/c.txt";
    CHECK(sg_index_upsert(&idx, &e) == 0, "stage blocked/c.txt");
    CHECK(sg_index_write(git_dir, &idx) == 0, "write index with blocked/c.txt staged");
    sg_index_free(&idx);

    snprintf(blocked_file, sizeof(blocked_file), "%s/blocked/c.txt", repo_root);
    CHECK(remove(blocked_file) == 0, "delete the working-tree copy of blocked/c.txt");
    snprintf(blocked_dir, sizeof(blocked_dir), "%s/blocked", repo_root);
    CHECK(chmod(blocked_dir, 0555) == 0, "chmod blocked/ read-only failed");

    rc = stash_push_keep_index(git_dir, repo_root, "keep-index will half-fail", commit1);
    CHECK(rc == -2, "expected -2 (stash durable, --keep-index re-apply failed), got %d", rc);

    chmod(blocked_dir, 0755); /* restore before any further reads/writes */

    {
        sg_stash_list list;

        CHECK(sg_stash_list_read(git_dir, &list) == 0, "list read failed after the -2 push");
        CHECK(list.count == 1,
             "the stash must be durably listed even though the --keep-index re-apply failed, got %zu",
             list.count);
        if (list.count == 1)
            CHECK(strstr(list.entries[0].message, "keep-index will half-fail") != NULL,
                 "the listed entry should carry the -m message sg_stash_push was given: %s",
                 list.entries[0].message);
        sg_stash_list_free(&list);
    }

    free(repo_root);
    free(git_dir);
}

/* ---- -2 branch: remove_untracked_files fails after the stash object,
   refs/stash and the head_tree reset already succeeded ------------------- */
static void test_push_returns_minus_two_when_untracked_removal_fails(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char commit1[SG_SHA1_RAW_LEN];
    char udir[4096];
    int rc;

    if (geteuid() == 0) {
        free(repo_root);
        free(git_dir);
        return;
    }

    commit_initial(git_dir, repo_root); /* HEAD: a.txt = "hello\n" */
    write_workdir_file(repo_root, "udir/u.txt", "extra\n"); /* untracked */

    snprintf(udir, sizeof(udir), "%s/udir", repo_root);
    CHECK(chmod(udir, 0555) == 0, "chmod udir/ read-only failed");

    rc = stash_push_u(git_dir, repo_root, "untracked removal will half-fail", commit1);
    CHECK(rc == -2, "expected -2 (stash durable, untracked removal failed), got %d", rc);

    chmod(udir, 0755); /* restore before any further reads/writes */

    {
        sg_stash_list list;

        CHECK(sg_stash_list_read(git_dir, &list) == 0, "list read failed after the -2 push");
        CHECK(list.count == 1,
             "the stash must be durably listed even though untracked removal failed, got %zu",
             list.count);
        if (list.count == 1)
            CHECK(strstr(list.entries[0].message, "untracked removal will half-fail") != NULL,
                 "the listed entry should carry the -m message sg_stash_push was given: %s",
                 list.entries[0].message);
        sg_stash_list_free(&list);
    }

    free(repo_root);
    free(git_dir);
}

/* ---- collision on the untracked half rejects the whole apply when the
   thing in the way is a DIRECTORY, not just a file (oracle scenario E,
   oracle4.txt) -- lstat doesn't care which, so this is expected to already
   work, but nothing pinned it down before ------------------------------- */
static void test_apply_untracked_collision_with_directory_rejects_whole_apply(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char stash_id[SG_SHA1_RAW_LEN];
    int rc;

    commit_initial(git_dir, repo_root); /* HEAD: a.txt = "hello\n" */
    write_workdir_file(repo_root, "a.txt", "changed\n");
    write_workdir_file(repo_root, "u.txt", "original\n");
    CHECK(stash_push_u(git_dir, repo_root, "u", stash_id) == 0, "-u push failed");
    CHECK(!file_exists(repo_root, "u.txt"), "push should have removed u.txt");

    /* a DIRECTORY now occupies the path the untracked half would restore */
    write_workdir_file(repo_root, "u.txt/inner.txt", "in the way\n");

    rc = sg_stash_apply(git_dir, repo_root, 0, 0);
    CHECK(rc == -1, "expected the whole apply to be rejected on a directory collision, got %d", rc);

    {
        char *a = read_workdir_file(repo_root, "a.txt");

        CHECK(a != NULL && strcmp(a, "hello\n") == 0,
             "a.txt must stay at HEAD's content (nothing written at all), got %s",
             a != NULL ? a : "(null)");
        CHECK(file_exists(repo_root, "u.txt/inner.txt"),
             "the colliding directory and its content must be untouched");
        free(a);
    }
    {
        sg_stash_list list;

        CHECK(sg_stash_list_read(git_dir, &list) == 0, "list read failed");
        CHECK(list.count == 1, "the stash must survive a rejected apply, got %zu", list.count);
        sg_stash_list_free(&list);
    }

    free(repo_root);
    free(git_dir);
}

/* ---- Phase 20 sec 3: --index -------------------------------------------- */

/* Control (no --index) vs --index on the SAME five-mutation fixture already
   used by the --keep-index table (spec sec 3.1's contrast) -- two separate
   repos, not the same repo twice, because sg_stash_apply (unlike
   sg_stash_push) leaves the working tree dirty afterward, and a second
   apply on top of that residue would not be testing the same starting
   state. */
static void test_index_flag_table(void)
{
    /* ---- control: apply without --index ---- */
    {
        char *git_dir = make_tmp_repo();
        char *repo_root = sg_repo_root(git_dir);
        unsigned char stash_id[SG_SHA1_RAW_LEN];
        sg_index idx;
        int pos;
        unsigned char blob_s1[SG_SHA1_RAW_LEN];

        keep_index_base_repo(git_dir, repo_root);
        apply_five_state_mutations(git_dir, repo_root);
        CHECK(stash_push(git_dir, repo_root, "for control", stash_id) == 0, "push failed (control)");

        CHECK(sg_stash_apply(git_dir, repo_root, 0, 0) == 0, "no-index apply failed");

        CHECK(sg_index_read(git_dir, &idx) == 0, "read index after no-index apply");
        CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "s1\n", 3, blob_s1) == 0, "recompute s1 blob");
        pos = sg_index_find(&idx, "staged.txt");
        CHECK(pos >= 0 && memcmp(idx.entries[pos].sha1, blob_s1, SG_SHA1_RAW_LEN) == 0,
             "no-index apply: staged.txt should be re-staged at HEAD's content (s1), not the "
             "stash's staged s2");
        CHECK(sg_index_find(&idx, "new_staged.txt") >= 0,
             "no-index apply: new_staged.txt (absent from HEAD) should stay staged");
        sg_index_free(&idx);

        free(repo_root);
        free(git_dir);
    }

    /* ---- --index: index restored to the stash's OWN index tree ---- */
    {
        char *git_dir = make_tmp_repo();
        char *repo_root = sg_repo_root(git_dir);
        unsigned char stash_id[SG_SHA1_RAW_LEN];
        sg_index idx;
        int pos;
        unsigned char blob_s2[SG_SHA1_RAW_LEN];
        unsigned char blob_ns[SG_SHA1_RAW_LEN];
        char *tracked;
        char *staged;

        keep_index_base_repo(git_dir, repo_root);
        apply_five_state_mutations(git_dir, repo_root);
        CHECK(stash_push(git_dir, repo_root, "for --index", stash_id) == 0, "push failed (--index)");

        CHECK(sg_stash_apply(git_dir, repo_root, 0, 1) == 0, "--index apply failed");

        CHECK(sg_index_read(git_dir, &idx) == 0, "read index after --index apply");
        CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "s2\n", 3, blob_s2) == 0, "recompute s2 blob");
        pos = sg_index_find(&idx, "staged.txt");
        CHECK(pos >= 0 && memcmp(idx.entries[pos].sha1, blob_s2, SG_SHA1_RAW_LEN) == 0,
             "--index apply: staged.txt should be restored to the stash's own staged content (s2)");
        CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "ns1\n", 4, blob_ns) == 0, "recompute ns1 blob");
        pos = sg_index_find(&idx, "new_staged.txt");
        CHECK(pos >= 0 && memcmp(idx.entries[pos].sha1, blob_ns, SG_SHA1_RAW_LEN) == 0,
             "--index apply: new_staged.txt should be restored staged");
        CHECK(sg_index_find(&idx, "staged_del.txt") < 0,
             "--index apply: staged_del.txt should stay absent from the index (it was staged for "
             "removal at push time)");

        /* --index only ever touches the index -- the working tree must be
           exactly what a no-index apply would have written there too. */
        tracked = read_workdir_file(repo_root, "tracked.txt");
        staged = read_workdir_file(repo_root, "staged.txt");
        CHECK(tracked != NULL && strcmp(tracked, "v2\n") == 0,
             "--index apply: working tree tracked.txt should be the stash's own content (v2), got %s",
             tracked != NULL ? tracked : "(null)");
        CHECK(staged != NULL && strcmp(staged, "s2\n") == 0,
             "--index apply: working tree staged.txt should be the stash's own content (s2), got %s",
             staged != NULL ? staged : "(null)");
        free(tracked);
        free(staged);
        sg_index_free(&idx);

        free(repo_root);
        free(git_dir);
    }
}

/* --index must be skipped ENTIRELY on a conflicting merge (spec sec 3.2):
   the index is left exactly as the no-"--index" rules leave it (conflict
   stages 1/2/3), never replaced with the stash's own index tree. */
static void test_index_flag_skipped_on_conflict(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char head1[SG_SHA1_RAW_LEN];
    unsigned char blob_hello[SG_SHA1_RAW_LEN];
    unsigned char blob_headside[SG_SHA1_RAW_LEN];
    unsigned char blob_stagedside[SG_SHA1_RAW_LEN];
    unsigned char tree2[SG_SHA1_RAW_LEN];
    unsigned char head2[SG_SHA1_RAW_LEN];
    unsigned char stash_id[SG_SHA1_RAW_LEN];
    sg_flat_entry tentries[1];
    sg_index idx;
    sg_index_entry e;
    sg_commit commit;
    unsigned char *serialized;
    size_t serialized_len;
    int rc;

    commit_initial(git_dir, repo_root); /* HEAD: a.txt = "hello\n" */
    CHECK(sg_ref_resolve_head(git_dir, head1) == 0, "resolve head1 failed");
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "hello\n", 6, blob_hello) == 0, "write hello blob");

    /* Working tree gets one content, the INDEX gets a DIFFERENT one -- so if
       the bug under test let --index proceed anyway, the wrongly-restored
       index would show "staged-side", not the conflict stages. */
    write_workdir_file(repo_root, "a.txt", "stash-side\n");
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "staged-side\n", 12, blob_stagedside) == 0,
         "write staged-side blob");
    CHECK(sg_index_read(git_dir, &idx) == 0, "read index before staging a.txt differently");
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, blob_stagedside, SG_SHA1_RAW_LEN);
    e.path = (char *)"a.txt";
    CHECK(sg_index_upsert(&idx, &e) == 0, "restage a.txt at staged-side");
    CHECK(sg_index_write(git_dir, &idx) == 0, "write index with staged-side a.txt");
    sg_index_free(&idx);

    rc = stash_push(git_dir, repo_root, "conflict setup", stash_id);
    CHECK(rc == 0, "stash push failed, rc=%d", rc);

    /* Advance HEAD past the stash's base, on the SAME path, with yet a
       THIRD content -- guarantees a genuine three-way conflict on apply. */
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "head-side\n", 10, blob_headside) == 0,
         "write head-side blob");
    tentries[0].path = (char *)"a.txt";
    tentries[0].mode = 0100644;
    memcpy(tentries[0].sha1, blob_headside, SG_SHA1_RAW_LEN);
    CHECK(sg_tree_build(git_dir, tentries, 1, tree2) == 0, "build tree2 failed");

    memset(&commit, 0, sizeof(commit));
    memcpy(commit.tree, tree2, SG_SHA1_RAW_LEN);
    commit.parents = malloc(sizeof(*commit.parents));
    CHECK(commit.parents != NULL, "alloc parents failed");
    memcpy(commit.parents[0], head1, SG_SHA1_RAW_LEN);
    commit.parent_count = 1;
    commit.author_name = (char *)"Test";
    commit.author_email = (char *)"test@example.com";
    commit.author_time = 1700000400;
    strcpy(commit.author_tz, "+0000");
    commit.committer_name = commit.author_name;
    commit.committer_email = commit.author_email;
    commit.committer_time = commit.author_time;
    strcpy(commit.committer_tz, "+0000");
    commit.message = (char *)"advance HEAD past the stash's base\n";
    CHECK(sg_commit_serialize(&commit, &serialized, &serialized_len) == 0, "serialize commit2 failed");
    CHECK(sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len, head2) == 0,
         "write commit2 failed");
    free(serialized);
    free(commit.parents);
    CHECK(sg_ref_update_branch(git_dir, "master", head2) == 0, "update branch to head2 failed");

    rc = sg_stash_apply(git_dir, repo_root, 0, 1 /* --index */);
    CHECK(rc == 1, "expected a conflicted apply, got %d", rc);

    CHECK(sg_index_read(git_dir, &idx) == 0, "read index after conflicted --index apply");
    CHECK(sg_index_find_stage(&idx, "a.txt", 0) < 0,
         "--index must NOT have written a stage-0 entry on conflict (that would mean the index "
         "restore ran anyway)");
    CHECK(sg_index_find_stage(&idx, "a.txt", 1) >= 0, "conflict stage 1 (base) missing");
    CHECK(sg_index_find_stage(&idx, "a.txt", 2) >= 0, "conflict stage 2 (ours) missing");
    CHECK(sg_index_find_stage(&idx, "a.txt", 3) >= 0, "conflict stage 3 (theirs) missing");
    {
        int p2 = sg_index_find_stage(&idx, "a.txt", 2);
        int p3 = sg_index_find_stage(&idx, "a.txt", 3);

        CHECK(p2 >= 0 && memcmp(idx.entries[p2].sha1, blob_headside, SG_SHA1_RAW_LEN) == 0,
             "conflict stage 2 should be HEAD's own content (head-side)");
        CHECK(p3 >= 0 && memcmp(idx.entries[p3].sha1, blob_stagedside, SG_SHA1_RAW_LEN) != 0,
             "conflict stage 3 should be the stash's tree content (stash-side), not the "
             "unrelated staged-side blob that --index would have wrongly restored");
    }
    sg_index_free(&idx);

    {
        sg_stash_list list;

        CHECK(sg_stash_list_read(git_dir, &list) == 0, "list read failed after conflicted apply");
        CHECK(list.count == 1, "a conflicted apply must not drop the stash entry, got %zu", list.count);
        sg_stash_list_free(&list);
    }

    free(repo_root);
    free(git_dir);
}

/* ---- Phase 20 sec 4: sg_stash_apply_check_dirty's targeted gate -------- */

/* HEAD: del_me.txt = "d1\n", mod.txt = "m1\n", untouched.txt = "u1\n". */
static void dirty_gate_base_repo(const char *git_dir, const char *repo_root)
{
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    sg_flat_entry entries[3];
    unsigned char blob_del[SG_SHA1_RAW_LEN];
    unsigned char blob_mod[SG_SHA1_RAW_LEN];
    unsigned char blob_unt[SG_SHA1_RAW_LEN];
    sg_index idx;
    sg_index_entry e;
    sg_commit commit;
    unsigned char *serialized;
    size_t serialized_len;

    write_workdir_file(repo_root, "del_me.txt", "d1\n");
    write_workdir_file(repo_root, "mod.txt", "m1\n");
    write_workdir_file(repo_root, "untouched.txt", "u1\n");
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "d1\n", 3, blob_del) == 0, "write d1 blob");
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "m1\n", 3, blob_mod) == 0, "write m1 blob");
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "u1\n", 3, blob_unt) == 0, "write u1 blob");

    memset(&idx, 0, sizeof(idx));
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, blob_del, SG_SHA1_RAW_LEN);
    e.path = (char *)"del_me.txt";
    CHECK(sg_index_upsert(&idx, &e) == 0, "upsert del_me.txt");
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, blob_mod, SG_SHA1_RAW_LEN);
    e.path = (char *)"mod.txt";
    CHECK(sg_index_upsert(&idx, &e) == 0, "upsert mod.txt");
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, blob_unt, SG_SHA1_RAW_LEN);
    e.path = (char *)"untouched.txt";
    CHECK(sg_index_upsert(&idx, &e) == 0, "upsert untouched.txt");
    CHECK(sg_index_write(git_dir, &idx) == 0, "write index");

    entries[0].path = (char *)"del_me.txt";
    entries[0].mode = 0100644;
    memcpy(entries[0].sha1, blob_del, SG_SHA1_RAW_LEN);
    entries[1].path = (char *)"mod.txt";
    entries[1].mode = 0100644;
    memcpy(entries[1].sha1, blob_mod, SG_SHA1_RAW_LEN);
    entries[2].path = (char *)"untouched.txt";
    entries[2].mode = 0100644;
    memcpy(entries[2].sha1, blob_unt, SG_SHA1_RAW_LEN);
    CHECK(sg_tree_build(git_dir, entries, 3, tree_id) == 0, "build tree");
    sg_index_free(&idx);

    memset(&commit, 0, sizeof(commit));
    memcpy(commit.tree, tree_id, SG_SHA1_RAW_LEN);
    commit.parent_count = 0;
    commit.author_name = (char *)"Test";
    commit.author_email = (char *)"test@example.com";
    commit.author_time = 1700000500;
    strcpy(commit.author_tz, "+0000");
    commit.committer_name = commit.author_name;
    commit.committer_email = commit.author_email;
    commit.committer_time = commit.author_time;
    strcpy(commit.committer_tz, "+0000");
    commit.message = (char *)"base\n";
    CHECK(sg_commit_serialize(&commit, &serialized, &serialized_len) == 0, "serialize base commit");
    CHECK(sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len, commit_id) == 0,
         "write base commit");
    free(serialized);
    CHECK(sg_ref_update_branch(git_dir, "master", commit_id) == 0, "update branch to base commit");
}

/* Stashes: deletes del_me.txt, modifies mod.txt to "m2\n", adds
   created.txt = "new\n". Leaves untouched.txt alone entirely -- the whole
   point of the fixture. After this, the working tree is back at
   dirty_gate_base_repo's HEAD state (push always resets). */
static void push_dirty_gate_stash(const char *git_dir, const char *repo_root,
                                  unsigned char stash_id_out[SG_SHA1_RAW_LEN])
{
    char abspath[4096];
    sg_index idx;
    unsigned char blob_mod2[SG_SHA1_RAW_LEN];
    unsigned char blob_new[SG_SHA1_RAW_LEN];
    sg_index_entry e;

    snprintf(abspath, sizeof(abspath), "%s/del_me.txt", repo_root);
    CHECK(remove(abspath) == 0, "delete del_me.txt from the working tree before stashing");
    CHECK(sg_index_read(git_dir, &idx) == 0, "read index before removing del_me.txt");
    CHECK(sg_index_remove(&idx, "del_me.txt") == 0, "unstage del_me.txt");

    write_workdir_file(repo_root, "mod.txt", "m2\n");
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "m2\n", 3, blob_mod2) == 0, "write m2 blob");
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, blob_mod2, SG_SHA1_RAW_LEN);
    e.path = (char *)"mod.txt";
    CHECK(sg_index_upsert(&idx, &e) == 0, "restage mod.txt at m2");

    write_workdir_file(repo_root, "created.txt", "new\n");
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "new\n", 4, blob_new) == 0, "write new blob");
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, blob_new, SG_SHA1_RAW_LEN);
    e.path = (char *)"created.txt";
    CHECK(sg_index_upsert(&idx, &e) == 0, "stage created.txt");

    CHECK(sg_index_write(git_dir, &idx) == 0, "write mutated index");
    sg_index_free(&idx);

    CHECK(stash_push(git_dir, repo_root, "dirty gate base", stash_id_out) == 0,
         "stash push for the dirty-gate fixture failed");
}

static char **check_dirty_paths;
static size_t check_dirty_count;

static void free_check_dirty_result(void)
{
    size_t i;

    for (i = 0; i < check_dirty_count; i++)
        free(check_dirty_paths[i]);
    free(check_dirty_paths);
    check_dirty_paths = NULL;
    check_dirty_count = 0;
}

/* Row 1/control: a dirty path the merge never touches must be let through. */
static void test_dirty_gate_untouched_path_allowed(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char stash_id[SG_SHA1_RAW_LEN];
    int rc;

    dirty_gate_base_repo(git_dir, repo_root);
    push_dirty_gate_stash(git_dir, repo_root, stash_id);

    write_workdir_file(repo_root, "untouched.txt", "DIRTY\n");
    rc = sg_stash_apply_check_dirty(git_dir, repo_root, 0, &check_dirty_paths, &check_dirty_count);
    CHECK(rc == 0, "row 1: dirtying a path the stash never touches must not block apply, rc=%d", rc);
    CHECK(check_dirty_count == 0, "row 1: expected no dirty paths, got %zu", check_dirty_count);
    free_check_dirty_result();

    free(repo_root);
    free(git_dir);
}

/* Row 2: dirty on a path the stash MODIFIES, content differs from both HEAD
   and the stash. */
static void test_dirty_gate_modified_path_rejected(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char stash_id[SG_SHA1_RAW_LEN];
    int rc;

    dirty_gate_base_repo(git_dir, repo_root);
    push_dirty_gate_stash(git_dir, repo_root, stash_id);

    write_workdir_file(repo_root, "mod.txt", "CONFLICT\n");
    rc = sg_stash_apply_check_dirty(git_dir, repo_root, 0, &check_dirty_paths, &check_dirty_count);
    CHECK(rc == 1, "row 2: dirty content on a modified path must block apply, rc=%d", rc);
    CHECK(check_dirty_count == 1 && strcmp(check_dirty_paths[0], "mod.txt") == 0,
         "row 2: expected mod.txt alone in the dirty list, got %zu entries (%s)", check_dirty_count,
         check_dirty_count > 0 ? check_dirty_paths[0] : "(none)");
    free_check_dirty_result();

    free(repo_root);
    free(git_dir);
}

/* Row 3: dirty content on a modified path that HAPPENS to equal what the
   stash would write there -- must STILL be rejected (the rule looks at
   whether the working tree differs from HEAD, not from the stash). */
static void test_dirty_gate_content_matches_stash_still_rejected(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char stash_id[SG_SHA1_RAW_LEN];
    int rc;

    dirty_gate_base_repo(git_dir, repo_root);
    push_dirty_gate_stash(git_dir, repo_root, stash_id);

    write_workdir_file(repo_root, "mod.txt", "m2\n"); /* == the stash's own target content */
    rc = sg_stash_apply_check_dirty(git_dir, repo_root, 0, &check_dirty_paths, &check_dirty_count);
    CHECK(rc == 1,
         "row 3: dirty content coincidentally equal to the stash's own target must still be "
         "rejected (the gate looks at HEAD, not the stash), rc=%d",
         rc);
    CHECK(check_dirty_count == 1 && strcmp(check_dirty_paths[0], "mod.txt") == 0,
         "row 3: expected mod.txt in the dirty list");
    free_check_dirty_result();

    free(repo_root);
    free(git_dir);
}

/* Row 4: dirty on a path the stash DELETES. */
static void test_dirty_gate_deleted_path_dirty_rejected(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char stash_id[SG_SHA1_RAW_LEN];
    int rc;

    dirty_gate_base_repo(git_dir, repo_root);
    push_dirty_gate_stash(git_dir, repo_root, stash_id);

    write_workdir_file(repo_root, "del_me.txt", "DIRTY\n");
    rc = sg_stash_apply_check_dirty(git_dir, repo_root, 0, &check_dirty_paths, &check_dirty_count);
    CHECK(rc == 1, "row 4: dirty content on a path the stash deletes must block apply, rc=%d", rc);
    CHECK(check_dirty_count == 1 && strcmp(check_dirty_paths[0], "del_me.txt") == 0,
         "row 4: expected del_me.txt in the dirty list");
    free_check_dirty_result();

    free(repo_root);
    free(git_dir);
}

/* Row 5: the path the stash MODIFIES was deleted from the working tree --
   nothing there to overwrite, so this must be ALLOWED (the opposite
   direction from row 4). */
static void test_dirty_gate_deleted_from_worktree_allowed(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char stash_id[SG_SHA1_RAW_LEN];
    char abspath[4096];
    int rc;

    dirty_gate_base_repo(git_dir, repo_root);
    push_dirty_gate_stash(git_dir, repo_root, stash_id);

    snprintf(abspath, sizeof(abspath), "%s/mod.txt", repo_root);
    CHECK(remove(abspath) == 0, "failed to delete mod.txt from the working tree");

    rc = sg_stash_apply_check_dirty(git_dir, repo_root, 0, &check_dirty_paths, &check_dirty_count);
    CHECK(rc == 0, "row 5: a path deleted from the working tree must not block apply, rc=%d", rc);
    CHECK(check_dirty_count == 0, "row 5: expected no dirty paths, got %zu", check_dirty_count);
    free_check_dirty_result();

    free(repo_root);
    free(git_dir);
}

/* Row 7 (spec sec 4.4): a STAGED change on a path the stash never touches
   must not block apply, AND must survive the apply itself (this is the part
   the old blanket re-stage-everything-from-HEAD loop would silently wipe). */
static void test_dirty_gate_untouched_staged_change_survives(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char stash_id[SG_SHA1_RAW_LEN];
    unsigned char blob_staged[SG_SHA1_RAW_LEN];
    sg_index idx;
    sg_index_entry e;
    int rc;
    int pos;

    dirty_gate_base_repo(git_dir, repo_root);
    push_dirty_gate_stash(git_dir, repo_root, stash_id);

    /* Simulate `git add` on untouched.txt: the index gets new content, the
       working tree has that SAME content (so this is purely a staged
       change, not also an unstaged one). */
    write_workdir_file(repo_root, "untouched.txt", "staged-diff\n");
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "staged-diff\n", 12, blob_staged) == 0,
         "write staged-diff blob");
    CHECK(sg_index_read(git_dir, &idx) == 0, "read index before staging untouched.txt");
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, blob_staged, SG_SHA1_RAW_LEN);
    e.path = (char *)"untouched.txt";
    CHECK(sg_index_upsert(&idx, &e) == 0, "restage untouched.txt");
    CHECK(sg_index_write(git_dir, &idx) == 0, "write index with staged untouched.txt");
    sg_index_free(&idx);

    rc = sg_stash_apply_check_dirty(git_dir, repo_root, 0, &check_dirty_paths, &check_dirty_count);
    CHECK(rc == 0, "row 7: a staged change on an untouched path must not block apply, rc=%d", rc);
    CHECK(check_dirty_count == 0, "row 7: expected no dirty paths, got %zu", check_dirty_count);
    free_check_dirty_result();

    rc = sg_stash_apply(git_dir, repo_root, 0, 0);
    CHECK(rc == 0, "row 7: the apply itself should succeed cleanly, rc=%d", rc);

    CHECK(sg_index_read(git_dir, &idx) == 0, "read index after apply");
    pos = sg_index_find(&idx, "untouched.txt");
    CHECK(pos >= 0 && memcmp(idx.entries[pos].sha1, blob_staged, SG_SHA1_RAW_LEN) == 0,
         "row 7: untouched.txt's pre-existing staged content must survive the apply, not be "
         "silently reset back to HEAD's own version");
    sg_index_free(&idx);

    free(repo_root);
    free(git_dir);
}

/* Row 8 (deliberate divergence from real git): a STAGED difference from
   HEAD on a path the stash DOES touch must block apply, even when the
   working tree file itself still matches HEAD exactly (i.e. purely a
   staged change, no unstaged component -- isolating this from rows 2/3/4,
   which are about the working tree). */
static void test_dirty_gate_touched_staged_change_rejected(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char stash_id[SG_SHA1_RAW_LEN];
    unsigned char blob_other[SG_SHA1_RAW_LEN];
    sg_index idx;
    sg_index_entry e;
    int rc;

    dirty_gate_base_repo(git_dir, repo_root);
    push_dirty_gate_stash(git_dir, repo_root, stash_id);

    /* mod.txt on disk is untouched (still HEAD's m1\n) -- only the INDEX
       gets a different blob, simulating some unrelated `git add` that ran
       before this apply. */
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "staged-other\n", 13, blob_other) == 0,
         "write staged-other blob");
    CHECK(sg_index_read(git_dir, &idx) == 0, "read index before staging mod.txt differently");
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, blob_other, SG_SHA1_RAW_LEN);
    e.path = (char *)"mod.txt";
    CHECK(sg_index_upsert(&idx, &e) == 0, "restage mod.txt at an unrelated staged blob");
    CHECK(sg_index_write(git_dir, &idx) == 0, "write index with staged mod.txt");
    sg_index_free(&idx);

    {
        char *on_disk = read_workdir_file(repo_root, "mod.txt");

        CHECK(on_disk != NULL && strcmp(on_disk, "m1\n") == 0,
             "precondition: mod.txt on disk must still be HEAD's own content, got %s",
             on_disk != NULL ? on_disk : "(null)");
        free(on_disk);
    }

    rc = sg_stash_apply_check_dirty(git_dir, repo_root, 0, &check_dirty_paths, &check_dirty_count);
    CHECK(rc == 1,
         "row 8: a staged difference from HEAD on a touched path must block apply even though the "
         "working tree file itself matches HEAD, rc=%d",
         rc);
    CHECK(check_dirty_count == 1 && strcmp(check_dirty_paths[0], "mod.txt") == 0,
         "row 8: expected mod.txt in the dirty list");
    free_check_dirty_result();

    free(repo_root);
    free(git_dir);
}

/* Error 1 (Phase 20 fix): an untouched path that the user DELETED from the
   working tree before apply/pop must stay deleted -- not silently
   resurrected. Before the fix, sg_merge_result_apply unconditionally
   rewrote every clean, non-deleted result entry, including untouched.txt
   with HEAD's own (identical) content, even though nothing about this
   apply concerns that path at all. */
static void test_untouched_path_deleted_by_user_stays_deleted(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char stash_id[SG_SHA1_RAW_LEN];
    char abspath[4096];
    int rc;

    dirty_gate_base_repo(git_dir, repo_root);
    push_dirty_gate_stash(git_dir, repo_root, stash_id);

    snprintf(abspath, sizeof(abspath), "%s/untouched.txt", repo_root);
    CHECK(remove(abspath) == 0, "failed to delete untouched.txt from the working tree");

    rc = sg_stash_apply_check_dirty(git_dir, repo_root, 0, &check_dirty_paths, &check_dirty_count);
    CHECK(rc == 0, "deleting an untouched path must not block apply, rc=%d", rc);
    free_check_dirty_result();

    rc = sg_stash_apply(git_dir, repo_root, 0, 0);
    CHECK(rc == 0, "apply itself should succeed cleanly, rc=%d", rc);

    CHECK(!file_exists(repo_root, "untouched.txt"),
         "untouched.txt's deletion must survive the apply -- it must not be resurrected from HEAD");

    free(repo_root);
    free(git_dir);
}

/* Error 2 (Phase 20 fix): a path staged AFTER the stash was pushed, that
   HEAD never had at all and the stash never touches either, must keep its
   staged ("A ") status through apply/pop -- both on disk and in the index.
   Before the fix, sg_stash_apply's re-stage loop only walked head_flat
   (HEAD's own paths), so a stage-0 orig_idx entry for a path absent from
   HEAD had no code path putting it back into new_idx at all. */
static void test_untouched_new_staged_file_survives(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    unsigned char stash_id[SG_SHA1_RAW_LEN];
    unsigned char blob_new_staged[SG_SHA1_RAW_LEN];
    sg_index idx;
    sg_index_entry e;
    int rc;
    int pos;

    dirty_gate_base_repo(git_dir, repo_root);
    push_dirty_gate_stash(git_dir, repo_root, stash_id);

    /* Simulate `printf ... > new_staged.txt; git add new_staged.txt` run
       after the stash push, on a path the stash's own change never
       mentions at all (unlike created.txt, which push_dirty_gate_stash
       itself stages as part of the stash). */
    write_workdir_file(repo_root, "new_staged.txt", "n1\n");
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "n1\n", 3, blob_new_staged) == 0,
         "write new_staged.txt blob");
    CHECK(sg_index_read(git_dir, &idx) == 0, "read index before staging new_staged.txt");
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, blob_new_staged, SG_SHA1_RAW_LEN);
    e.path = (char *)"new_staged.txt";
    CHECK(sg_index_upsert(&idx, &e) == 0, "stage new_staged.txt");
    CHECK(sg_index_write(git_dir, &idx) == 0, "write index with staged new_staged.txt");
    sg_index_free(&idx);

    rc = sg_stash_apply_check_dirty(git_dir, repo_root, 0, &check_dirty_paths, &check_dirty_count);
    CHECK(rc == 0, "a new staged file the stash never touches must not block apply, rc=%d", rc);
    free_check_dirty_result();

    rc = sg_stash_apply(git_dir, repo_root, 0, 0);
    CHECK(rc == 0, "apply itself should succeed cleanly, rc=%d", rc);

    CHECK(file_exists(repo_root, "new_staged.txt"), "new_staged.txt must still be on disk");

    CHECK(sg_index_read(git_dir, &idx) == 0, "read index after apply");
    pos = sg_index_find(&idx, "new_staged.txt");
    CHECK(pos >= 0, "new_staged.txt must still have a stage-0 index entry after apply");
    if (pos >= 0)
        CHECK(idx.entries[pos].stage == 0 &&
             memcmp(idx.entries[pos].sha1, blob_new_staged, SG_SHA1_RAW_LEN) == 0,
             "new_staged.txt's index entry must be stage 0 with its own (staged) blob");
    sg_index_free(&idx);

    free(repo_root);
    free(git_dir);
}

int main(void)
{
    test_parse_spec_table();
    test_push_list_drop_clear();
    test_drop_rolls_back_on_ref_write_failure();
    test_clear_rolls_back_on_delete_failure();
    test_push_returns_minus_two_when_reset_fails();
    test_apply_fails_when_clean_write_target_is_a_directory();
    test_keep_index_table();
    test_keep_index_noop_when_index_matches_head();
    test_u_stash_tree_matches_noflag_tree();
    test_third_parent_is_root_commit();
    test_u_excludes_ignored_a_includes();
    test_message_does_not_affect_index_and_untracked_subjects();
    test_empty_untracked_list_still_three_parents();
    test_nothing_to_do_boundaries();
    test_workdir_cleanup_boundaries();
    test_workdir_cleanup_ignored_dir_removed_by_a();
    test_u_with_keep_index();
    test_apply_restores_untracked_unstaged();
    test_apply_untracked_collision_rejects_whole_apply();
    test_push_returns_minus_two_when_keep_index_second_apply_fails();
    test_push_returns_minus_two_when_untracked_removal_fails();
    test_apply_untracked_collision_with_directory_rejects_whole_apply();
    test_index_flag_table();
    test_index_flag_skipped_on_conflict();
    test_dirty_gate_untouched_path_allowed();
    test_dirty_gate_modified_path_rejected();
    test_dirty_gate_content_matches_stash_still_rejected();
    test_dirty_gate_deleted_path_dirty_rejected();
    test_dirty_gate_deleted_from_worktree_allowed();
    test_dirty_gate_untouched_staged_change_survives();
    test_dirty_gate_touched_staged_change_rejected();
    test_untouched_path_deleted_by_user_stays_deleted();
    test_untouched_new_staged_file_survives();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all stash tests passed\n");
    return 0;
}
