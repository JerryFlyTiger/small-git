#include "sg/stash.h"

#include "sg/hash.h"
#include "sg/index.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/reflog.h"
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
    rc = sg_stash_push(git_dir, repo_root, NULL, commit1);
    CHECK(rc == 1, "expected nothing-to-save (1), got %d", rc);
    assert_tip_invariant(git_dir);
    {
        unsigned char ref_id[SG_SHA1_RAW_LEN];

        CHECK(sg_ref_read_path(git_dir, "refs/stash", ref_id) != 0, "refs/stash should not exist yet");
    }

    /* dirty the workdir, then stash it */
    write_workdir_file(repo_root, "a.txt", "changed\n");

    rc = sg_stash_push(git_dir, repo_root, "first stash", commit1);
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
    rc = sg_stash_push(git_dir, repo_root, NULL, commit2);
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
    rc = sg_stash_push(git_dir, repo_root, NULL, commit1);
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
    CHECK(sg_stash_push(git_dir, repo_root, "first", commit1) == 0, "first push failed");
    write_workdir_file(repo_root, "a.txt", "changed again\n");
    CHECK(sg_stash_push(git_dir, repo_root, "second", commit2) == 0, "second push failed");
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
    CHECK(sg_stash_push(git_dir, repo_root, "only", commit1) == 0, "push failed");
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

    rc = sg_stash_push(git_dir, repo_root, "will half-fail", commit1);
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
    rc = sg_stash_push(git_dir, repo_root, "change d", stash_commit);
    CHECK(rc == 0, "stash push failed, rc=%d", rc);

    /* replace the tracked file with a directory of the same name */
    snprintf(abspath, sizeof(abspath), "%s/d/file.txt", repo_root);
    CHECK(remove(abspath) == 0, "failed to remove d/file.txt before replacing it with a directory");
    CHECK(mkdir(abspath, 0755) == 0, "failed to mkdir in place of d/file.txt");

    rc = sg_stash_apply(git_dir, repo_root, 0);
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

int main(void)
{
    test_parse_spec_table();
    test_push_list_drop_clear();
    test_drop_rolls_back_on_ref_write_failure();
    test_clear_rolls_back_on_delete_failure();
    test_push_returns_minus_two_when_reset_fails();
    test_apply_fails_when_clean_write_target_is_a_directory();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all stash tests passed\n");
    return 0;
}
