#include "sg/pick.h"

#include "sg/cli.h"
#include "sg/hash.h"
#include "sg/index.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/sequencer.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static int failures = 0;
static long long time_seq = 3000000;

#define CHECK(cond, ...)                                                                        \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);                                 \
            fprintf(stderr, __VA_ARGS__);                                                        \
            fprintf(stderr, "\n");                                                               \
            failures++;                                                                          \
        }                                                                                         \
    } while (0)

static char *make_tmp_repo(char **repo_root_out)
{
    static char template[] = "/tmp/sg_pick_engine_test_XXXXXX";
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
    *repo_root_out = path;
    return strdup(git_dir);
}

static void write_workdir_file(const char *repo_root, const char *rel, const char *content)
{
    char abspath[4096];

    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, rel);
    CHECK(sg_write_file_mkdirs(abspath, (const unsigned char *)content, strlen(content), 0644) == 0,
         "failed to write workdir file %s", rel);
}

/* Builds a commit whose tree is a single file "f.txt" = content, writes the
   matching working-directory file and index (so this commit can be used as
   a synced HEAD, i.e. sg_require_clean_workdir sees it as clean), and
   updates refs/heads/<branch> to it. If parent is non-NULL, the commit gets
   that one parent. sync selects whether the working directory/index are
   also written to match (only meaningful for the branch actually checked
   out -- a sibling branch built purely as a cherry-pick/revert SOURCE
   passes sync=0, its tree only needs to exist as an object). */
static void build_commit(const char *git_dir, const char *repo_root, const char *branch,
                         const char *content, const unsigned char *parent, const char *author_name,
                         const char *author_email, const char *message, int sync,
                         unsigned char commit_id_out[SG_SHA1_RAW_LEN])
{
    unsigned char blob[SG_SHA1_RAW_LEN];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    sg_flat_entry entry;
    sg_commit commit;
    unsigned char *serialized;
    size_t serialized_len;

    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, content, strlen(content), blob) == 0,
         "blob write failed for '%s'", message);

    entry.path = (char *)"f.txt";
    entry.mode = 0100644;
    memcpy(entry.sha1, blob, SG_SHA1_RAW_LEN);
    CHECK(sg_tree_build(git_dir, &entry, 1, tree_id) == 0, "tree build failed for '%s'", message);

    memset(&commit, 0, sizeof(commit));
    memcpy(commit.tree, tree_id, SG_SHA1_RAW_LEN);
    if (parent != NULL) {
        commit.parents = malloc(sizeof(*commit.parents));
        memcpy(commit.parents[0], parent, SG_SHA1_RAW_LEN);
        commit.parent_count = 1;
    }
    commit.author_name = (char *)author_name;
    commit.author_email = (char *)author_email;
    commit.author_time = time_seq++;
    strcpy(commit.author_tz, "+0000");
    commit.committer_name = (char *)author_name;
    commit.committer_email = (char *)author_email;
    commit.committer_time = commit.author_time;
    strcpy(commit.committer_tz, "+0000");
    commit.message = (char *)message;

    CHECK(sg_commit_serialize(&commit, &serialized, &serialized_len) == 0,
         "serialize failed for '%s'", message);
    free(commit.parents);
    CHECK(sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len, commit_id_out) == 0,
         "commit write failed for '%s'", message);
    free(serialized);

    CHECK(sg_ref_update_branch(git_dir, branch, commit_id_out) == 0,
         "updating branch '%s' failed", branch);

    if (sync) {
        sg_index idx;
        sg_index_entry e;

        write_workdir_file(repo_root, "f.txt", content);
        memset(&idx, 0, sizeof(idx));
        memset(&e, 0, sizeof(e));
        e.mode = 0100644;
        memcpy(e.sha1, blob, SG_SHA1_RAW_LEN);
        e.path = (char *)"f.txt";
        CHECK(sg_index_upsert(&idx, &e) == 0, "upsert f.txt");
        CHECK(sg_index_write(git_dir, &idx) == 0, "write index");
        sg_index_free(&idx);
    }
}

/* ==================== clean pick advances the branch ==================== */

static void test_clean_pick_advances_branch(void)
{
    char *repo_root;
    char *git_dir = make_tmp_repo(&repo_root);
    unsigned char root[SG_SHA1_RAW_LEN], feat[SG_SHA1_RAW_LEN];
    unsigned char master_before[SG_SHA1_RAW_LEN], master_after[SG_SHA1_RAW_LEN];
    sg_pick_opts opts;
    int rc;

    build_commit(git_dir, repo_root, "master", "line1\n", NULL, "Root Author", "root@example.com",
                "root\n", 1, root);
    build_commit(git_dir, repo_root, "feature", "line1\nline2\n", root, "Feature Author",
                "feature@example.com", "feature commit\n", 0, feat);
    /* master must still point at root -- building the feature branch above
       moved refs/heads/feature, not refs/heads/master. */
    CHECK(sg_ref_update_branch(git_dir, "master", root) == 0, "restore master to root");
    memcpy(master_before, root, SG_SHA1_RAW_LEN);

    memset(&opts, 0, sizeof(opts));
    rc = sg_pick_start(git_dir, repo_root, SG_SEQ_CHERRY_PICK, (unsigned char (*)[SG_SHA1_RAW_LEN])feat,
                       1, &opts);
    CHECK(rc == 0, "a clean cherry-pick should succeed, got rc=%d", rc);

    CHECK(sg_ref_read_branch(git_dir, "master", master_after) == 0, "read master after pick");
    CHECK(memcmp(master_after, master_before, SG_SHA1_RAW_LEN) != 0,
         "master should have advanced to a new commit");

    {
        sg_obj_type type;
        unsigned char *content;
        size_t content_len;
        sg_commit c;

        CHECK(sg_object_read(git_dir, master_after, &type, &content, &content_len) == 0 &&
                 type == SG_OBJ_COMMIT,
             "new commit should be readable");
        CHECK(sg_commit_parse(content, content_len, &c) == 0, "new commit should parse");
        CHECK(c.parent_count == 1 && memcmp(c.parents[0], master_before, SG_SHA1_RAW_LEN) == 0,
             "new commit's sole parent should be the pre-pick HEAD");
        /* Author copied verbatim from the picked commit (spec 3.2). */
        CHECK(strcmp(c.author_name, "Feature Author") == 0,
             "author_name should be copied from the picked commit, got '%s'", c.author_name);
        CHECK(strcmp(c.author_email, "feature@example.com") == 0,
             "author_email should be copied from the picked commit, got '%s'", c.author_email);
        CHECK(strcmp(c.message, "feature commit\n") == 0,
             "message should be the picked commit's message byte-for-byte, got '%s'", c.message);
        /* Committer must NOT be the picked commit's author. */
        CHECK(strcmp(c.committer_name, "Feature Author") != 0,
             "committer_name should come from the environment, not the picked commit");
        free(content);
        sg_commit_free(&c);
    }

    free(git_dir);
    free(repo_root);
}

/* ==================== conflict leaves stage 1/2/3 plus state files ==================== */

static void test_conflict_leaves_stages_and_state(void)
{
    char *repo_root;
    char *git_dir = make_tmp_repo(&repo_root);
    unsigned char root[SG_SHA1_RAW_LEN], master_edit[SG_SHA1_RAW_LEN], feat_edit[SG_SHA1_RAW_LEN];
    sg_pick_opts opts;
    int rc;
    sg_index idx;

    build_commit(git_dir, repo_root, "master", "line1\nline2\nline3\n", NULL, "A", "a@example.com",
                "root\n", 1, root);
    build_commit(git_dir, repo_root, "feature", "line1\nFEATURE\nline3\n", root, "A", "a@example.com",
                "feature edit\n", 0, feat_edit);
    build_commit(git_dir, repo_root, "master", "line1\nMASTER\nline3\n", root, "A", "a@example.com",
                "master edit\n", 1, master_edit);

    memset(&opts, 0, sizeof(opts));
    rc = sg_pick_start(git_dir, repo_root, SG_SEQ_CHERRY_PICK,
                       (unsigned char (*)[SG_SHA1_RAW_LEN])feat_edit, 1, &opts);
    CHECK(rc == 1, "a conflicting cherry-pick should return 1, got rc=%d", rc);

    CHECK(sg_index_read(git_dir, &idx) == 0, "index should be readable after a conflict");
    CHECK(sg_index_find_stage(&idx, "f.txt", 1) >= 0, "stage 1 (base) should be present for f.txt");
    CHECK(sg_index_find_stage(&idx, "f.txt", 2) >= 0, "stage 2 (ours) should be present for f.txt");
    CHECK(sg_index_find_stage(&idx, "f.txt", 3) >= 0, "stage 3 (theirs) should be present for f.txt");
    sg_index_free(&idx);

    CHECK(sg_sequencer_kind_in_progress(git_dir) == SG_SEQ_CHERRY_PICK,
         "CHERRY_PICK_HEAD should be present after the conflict");
    {
        sg_sequencer_state st;

        CHECK(sg_sequencer_state_read(git_dir, &st) == 0, "sequencer state should be readable");
        CHECK(memcmp(st.current, feat_edit, SG_SHA1_RAW_LEN) == 0,
             "current should be the conflicting commit");
        CHECK(st.has_sequence == 0, "a single-commit pick must not create sequencer/");
        sg_sequencer_state_free(&st);
    }
    {
        char path[4096];
        unsigned char *data;
        size_t len;

        snprintf(path, sizeof(path), "%s/MERGE_MSG", git_dir);
        CHECK(sg_read_file(path, &data, &len) == 0, "MERGE_MSG should exist after a conflict");
        if (data != NULL) {
            CHECK(memmem(data, len, "# Conflicts:", strlen("# Conflicts:")) != NULL,
                 "MERGE_MSG should contain a # Conflicts: block");
            CHECK(memmem(data, len, "#\tf.txt", strlen("#\tf.txt")) != NULL,
                 "MERGE_MSG should name f.txt in the conflicts block");
            free(data);
        }
    }

    free(git_dir);
    free(repo_root);
}

/* ==================== empty pick detected ==================== */

static void test_empty_pick_detected(void)
{
    char *repo_root;
    char *git_dir = make_tmp_repo(&repo_root);
    unsigned char root[SG_SHA1_RAW_LEN], master_tip[SG_SHA1_RAW_LEN], feat_same_change[SG_SHA1_RAW_LEN];
    sg_pick_opts opts;
    int rc;

    build_commit(git_dir, repo_root, "master", "line1\n", NULL, "A", "a@example.com", "root\n", 1,
                root);
    build_commit(git_dir, repo_root, "master", "line1\nline2\n", root, "A", "a@example.com",
                "add line2\n", 1, master_tip);
    /* Same resulting content as master_tip (so its tree is byte-identical
       to master_tip's, even though the commit OBJECT is distinct: different
       message/author/parent-chain-position), branched from the same root. */
    build_commit(git_dir, repo_root, "feature", "line1\nline2\n", root, "B", "b@example.com",
                "same change, different commit\n", 0, feat_same_change);

    memset(&opts, 0, sizeof(opts));
    rc = sg_pick_start(git_dir, repo_root, SG_SEQ_CHERRY_PICK,
                       (unsigned char (*)[SG_SHA1_RAW_LEN])feat_same_change, 1, &opts);
    CHECK(rc == 1, "an empty cherry-pick should stop (rc=1), got rc=%d", rc);

    CHECK(sg_sequencer_kind_in_progress(git_dir) == SG_SEQ_CHERRY_PICK,
         "CHERRY_PICK_HEAD should be left so --skip/--abort work");

    {
        unsigned char master_after[SG_SHA1_RAW_LEN];

        CHECK(sg_ref_read_branch(git_dir, "master", master_after) == 0, "read master");
        CHECK(memcmp(master_after, master_tip, SG_SHA1_RAW_LEN) == 0,
             "an empty pick must not move the branch");
    }

    free(git_dir);
    free(repo_root);
}

/* ==================== revert authors from the environment, not the reverted commit ==================== */

static void test_revert_author_from_environment(void)
{
    char *repo_root;
    char *git_dir = make_tmp_repo(&repo_root);
    unsigned char root[SG_SHA1_RAW_LEN], edit[SG_SHA1_RAW_LEN];
    sg_pick_opts opts;
    int rc;

    build_commit(git_dir, repo_root, "master", "line1\nline2\nline3\n", NULL, "Root", "root@example.com",
                "root\n", 0, root);
    build_commit(git_dir, repo_root, "master", "line1\nCHANGED\nline3\n", root, "Someone Else",
                "someone@example.com", "an edit\n", 1, edit);

    memset(&opts, 0, sizeof(opts));
    rc = sg_pick_start(git_dir, repo_root, SG_SEQ_REVERT, (unsigned char (*)[SG_SHA1_RAW_LEN])edit, 1,
                       &opts);
    CHECK(rc == 0, "a clean revert should succeed, got rc=%d", rc);

    {
        unsigned char master_after[SG_SHA1_RAW_LEN];
        sg_obj_type type;
        unsigned char *content;
        size_t content_len;
        sg_commit c;

        CHECK(sg_ref_read_branch(git_dir, "master", master_after) == 0, "read master after revert");
        CHECK(sg_object_read(git_dir, master_after, &type, &content, &content_len) == 0 &&
                 type == SG_OBJ_COMMIT,
             "new commit should be readable");
        CHECK(sg_commit_parse(content, content_len, &c) == 0, "new commit should parse");
        CHECK(strcmp(c.author_name, "Someone Else") != 0,
             "revert's author must NOT be copied from the reverted commit, got '%s'",
             c.author_name);
        CHECK(strcmp(c.author_email, "someone@example.com") != 0,
             "revert's author email must NOT be copied from the reverted commit, got '%s'",
             c.author_email);
        CHECK(strstr(c.message, "Revert \"an edit\"") != NULL,
             "revert message should wrap the original subject, got '%s'", c.message);
        free(content);
        sg_commit_free(&c);
    }

    free(git_dir);
    free(repo_root);
}



/* ==================== a review-found bug: --continue's message must NOT
   gain a trailing blank line beyond the picked commit's own ==================== */

/* Regression test for a bug a review caught: read_message_from_merge_msg
   (src/cli/pick.c) stripped the "\n# Conflicts:\n..." tail one byte too
   short, leaving the blank SEPARATOR line (spec section 2.2's own format,
   "<message ending in exactly one \n>\n# Conflicts:\n...") attached to the
   recovered message -- so a --continue'd commit's message came out as
   "<message>\n\n" instead of "<message>\n", a different commit than git
   would have produced for the identical conflict. */
static void test_continue_message_has_no_extra_blank_line(void)
{
    char *repo_root;
    char *git_dir = make_tmp_repo(&repo_root);
    unsigned char root[SG_SHA1_RAW_LEN], master_edit[SG_SHA1_RAW_LEN], feat_edit[SG_SHA1_RAW_LEN];
    sg_pick_opts opts;
    int rc;

    build_commit(git_dir, repo_root, "master", "line1\nline2\nline3\n", NULL, "A", "a@example.com",
                "root\n", 1, root);
    build_commit(git_dir, repo_root, "feature", "line1\nFEATURE\nline3\n", root, "A", "a@example.com",
                "feature edit\n", 0, feat_edit);
    build_commit(git_dir, repo_root, "master", "line1\nMASTER\nline3\n", root, "A", "a@example.com",
                "master edit\n", 1, master_edit);

    memset(&opts, 0, sizeof(opts));
    rc = sg_pick_start(git_dir, repo_root, SG_SEQ_CHERRY_PICK,
                       (unsigned char (*)[SG_SHA1_RAW_LEN])feat_edit, 1, &opts);
    CHECK(rc == 1, "the pick should conflict and stop, got rc=%d", rc);

    /* Resolve the conflict: write a resolved blob and stage it at stage 0. */
    {
        unsigned char resolved_blob[SG_SHA1_RAW_LEN];
        sg_index idx;
        sg_index_entry e;
        static const char resolved[] = "line1\nRESOLVED\nline3\n";

        CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, resolved, strlen(resolved), resolved_blob) == 0,
             "write resolved blob");
        CHECK(sg_index_read(git_dir, &idx) == 0, "read index after conflict");
        CHECK(sg_index_remove_all_stages(&idx, "f.txt") > 0, "remove conflicted stages");
        memset(&e, 0, sizeof(e));
        e.mode = 0100644;
        memcpy(e.sha1, resolved_blob, SG_SHA1_RAW_LEN);
        e.path = (char *)"f.txt";
        CHECK(sg_index_upsert(&idx, &e) == 0, "stage resolved f.txt");
        CHECK(sg_index_write(git_dir, &idx) == 0, "write resolved index");
        sg_index_free(&idx);
    }

    rc = sg_pick_continue(git_dir, repo_root, SG_SEQ_CHERRY_PICK);
    CHECK(rc == 0, "--continue should finish the pick, got rc=%d", rc);

    {
        unsigned char master_after[SG_SHA1_RAW_LEN];
        sg_obj_type type;
        unsigned char *content;
        size_t content_len;
        sg_commit c;

        CHECK(sg_ref_read_branch(git_dir, "master", master_after) == 0, "read master after continue");
        CHECK(sg_object_read(git_dir, master_after, &type, &content, &content_len) == 0 &&
                 type == SG_OBJ_COMMIT,
             "the finished commit should be readable");
        CHECK(sg_commit_parse(content, content_len, &c) == 0, "the finished commit should parse");
        CHECK(strcmp(c.message, "feature edit\n") == 0,
             "the --continue'd commit's message should be exactly the picked commit's own message, "
             "byte for byte, got '%s' (%zu bytes)", c.message, strlen(c.message));
        free(content);
        sg_commit_free(&c);
    }

    free(git_dir);
    free(repo_root);
}


/* ==================== revert's --continue author must ALSO come from the
   environment, not the reverted commit (review finding, Phase 57b) ====
   The direct-apply case (test_revert_author_from_environment above) only
   exercises attempt_one's kind branch; sg_pick_continue builds the finished
   commit through a COMPLETELY SEPARATE code path (it cannot re-run
   attempt_one -- the conflict has already been resolved by hand), and had
   its own, independently-written kind branch making the same choice. A
   review found this second branch had never been exercised: mutating its
   `if (kind == SG_SEQ_CHERRY_PICK)` to `if (1)` (always copy the reverted
   commit's author) left `make test` and `interop.sh` both fully green. */
static void test_revert_continue_author_from_environment(void)
{
    char *repo_root;
    char *git_dir = make_tmp_repo(&repo_root);
    unsigned char root[SG_SHA1_RAW_LEN], edit1[SG_SHA1_RAW_LEN], edit2[SG_SHA1_RAW_LEN];
    sg_pick_opts opts;
    int rc;

    build_commit(git_dir, repo_root, "master", "line1\nline2\nline3\n", NULL, "A", "a@example.com",
                "root\n", 0, root);
    build_commit(git_dir, repo_root, "master", "line1\nEDIT1\nline3\n", root, "Someone Else",
                "someone@example.com", "edit1\n", 0, edit1);
    build_commit(git_dir, repo_root, "master", "line1\nEDIT2\nline3\n", edit1, "A", "a@example.com",
                "edit2\n", 1, edit2);

    memset(&opts, 0, sizeof(opts));
    rc = sg_pick_start(git_dir, repo_root, SG_SEQ_REVERT, (unsigned char (*)[SG_SHA1_RAW_LEN])edit1, 1,
                       &opts);
    CHECK(rc == 1, "reverting edit1 while HEAD is edit2 should conflict, got rc=%d", rc);
    CHECK(sg_sequencer_kind_in_progress(git_dir) == SG_SEQ_REVERT,
         "REVERT_HEAD should be present after the conflict");

    /* Resolve the conflict: write a resolved blob and stage it at stage 0. */
    {
        unsigned char resolved_blob[SG_SHA1_RAW_LEN];
        sg_index idx;
        sg_index_entry e;
        static const char resolved[] = "line1\nRESOLVED\nline3\n";

        CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, resolved, strlen(resolved), resolved_blob) == 0,
             "write resolved blob");
        CHECK(sg_index_read(git_dir, &idx) == 0, "read index after conflict");
        CHECK(sg_index_remove_all_stages(&idx, "f.txt") > 0, "remove conflicted stages");
        memset(&e, 0, sizeof(e));
        e.mode = 0100644;
        memcpy(e.sha1, resolved_blob, SG_SHA1_RAW_LEN);
        e.path = (char *)"f.txt";
        CHECK(sg_index_upsert(&idx, &e) == 0, "stage resolved f.txt");
        CHECK(sg_index_write(git_dir, &idx) == 0, "write resolved index");
        sg_index_free(&idx);
    }

    rc = sg_pick_continue(git_dir, repo_root, SG_SEQ_REVERT);
    CHECK(rc == 0, "--continue should finish the revert, got rc=%d", rc);

    {
        unsigned char master_after[SG_SHA1_RAW_LEN];
        sg_obj_type type;
        unsigned char *content;
        size_t content_len;
        sg_commit c;

        CHECK(sg_ref_read_branch(git_dir, "master", master_after) == 0, "read master after continue");
        CHECK(sg_object_read(git_dir, master_after, &type, &content, &content_len) == 0 &&
                 type == SG_OBJ_COMMIT,
             "the finished commit should be readable");
        CHECK(sg_commit_parse(content, content_len, &c) == 0, "the finished commit should parse");
        /* Positive assertion, not just "not equal to X": the author must be
           the same env-derived fallback attempt_one's own else-branch uses
           (env_or("GIT_AUTHOR_NAME", "small_git")), so a mutation that
           substitutes some OTHER wrong value still gets caught. */
        CHECK(strcmp(c.author_name, "small_git") == 0,
             "--continue'd revert's author must come from the environment fallback, not edit1's "
             "author 'Someone Else'; got '%s'", c.author_name);
        CHECK(strcmp(c.author_email, "sg@localhost") == 0,
             "--continue'd revert's author email must come from the environment fallback, not "
             "edit1's 'someone@example.com'; got '%s'", c.author_email);
        CHECK(strcmp(c.author_name, "Someone Else") != 0,
             "--continue'd revert's author must NOT be copied from the reverted commit (edit1)");
        free(content);
        sg_commit_free(&c);
    }

    free(git_dir);
    free(repo_root);
}

/* ==================== a second review round: escape hatches must not need
   a parseable state (Phase 57 spec section 5b) ==================== */

/* Builds root -> {feat1 (conflicting edit), feat2 (clean addition)} on a
   "feature" branch and {master edit} on master (conflicting with feat1),
   same shape as test_conflict_leaves_stages_and_state but stacking a SECOND,
   clean commit after the conflicting one so a 2-commit pick creates a real
   sequencer/ directory. Starts the pick (which conflicts on feat1, leaving
   feat2 in sequencer/todo), then OVERWRITES sequencer/todo with a malformed
   line -- deliberately in the shape a real git binary would actually leave
   (a 7-hex id, unreadable by sg's fixed-40-hex parser; see the
   sequencer/todo divergence in sequencer.h), simulating both that case and
   the more general "any damaged sequencer/todo" trigger (partial write,
   full disk mid sg_sequencer_state_write) the spec names. Returns via
   out-params the commit the whole sequence should revert back to. */
static void build_corrupted_multi_conflict(char **git_dir_out, char **repo_root_out,
                                           unsigned char master_edit_out[SG_SHA1_RAW_LEN])
{
    char *repo_root;
    char *git_dir = make_tmp_repo(&repo_root);
    unsigned char root[SG_SHA1_RAW_LEN], feat1[SG_SHA1_RAW_LEN], feat2[SG_SHA1_RAW_LEN];
    sg_pick_opts opts;
    int rc;
    char path[4096];
    FILE *f;

    build_commit(git_dir, repo_root, "master", "line1\nline2\nline3\n", NULL, "A", "a@example.com",
                "root\n", 1, root);
    build_commit(git_dir, repo_root, "feature", "line1\nFEATURE\nline3\n", root, "A", "a@example.com",
                "feat1\n", 0, feat1);
    build_commit(git_dir, repo_root, "feature", "line1\nFEATURE\nline3\nline4\n", feat1, "A",
                "a@example.com", "feat2\n", 0, feat2);
    build_commit(git_dir, repo_root, "master", "line1\nMASTER\nline3\n", root, "A", "a@example.com",
                "master edit\n", 1, master_edit_out);

    memset(&opts, 0, sizeof(opts));
    {
        unsigned char todo[2][SG_SHA1_RAW_LEN];

        memcpy(todo[0], feat1, SG_SHA1_RAW_LEN);
        memcpy(todo[1], feat2, SG_SHA1_RAW_LEN);
        rc = sg_pick_start(git_dir, repo_root, SG_SEQ_CHERRY_PICK, todo, 2, &opts);
    }
    CHECK(rc == 1, "the multi-commit pick should conflict and stop, got rc=%d", rc);
    CHECK(sg_sequencer_kind_in_progress(git_dir) == SG_SEQ_CHERRY_PICK,
         "CHERRY_PICK_HEAD should be present");

    snprintf(path, sizeof(path), "%s/sequencer/todo", git_dir);
    f = fopen(path, "wb");
    CHECK(f != NULL, "should be able to reopen sequencer/todo to corrupt it");
    if (f != NULL) {
        /* A 7-hex id (git's own abbreviated width) -- unreadable by
           parse_todo_line, which requires the id field to be exactly 40
           hex characters. */
        fputs("pick 1234567 feat1\n", f);
        fclose(f);
    }

    *git_dir_out = git_dir;
    *repo_root_out = repo_root;
}

/* Runs fn(git_dir, repo_root, kind) with stderr captured into err_buf
   (truncated to fit; NUL-terminated). Same technique as
   tests/test_stash_show.c's capture helper. */
static int call_capturing_stderr(int (*fn)(const char *, const char *, sg_seq_kind),
                                 const char *git_dir, const char *repo_root, sg_seq_kind kind,
                                 char *err_buf, size_t err_buf_size)
{
    char tmpl[] = "/tmp/sg_pick_engine_stderr_XXXXXX";
    int tmp_fd;
    int saved_stderr;
    off_t len;
    ssize_t n;
    int rc;

    tmp_fd = mkstemp(tmpl);
    if (tmp_fd < 0) {
        fprintf(stderr, "mkstemp failed\n");
        exit(1);
    }
    unlink(tmpl);

    fflush(stderr);
    saved_stderr = dup(STDERR_FILENO);
    dup2(tmp_fd, STDERR_FILENO);

    rc = fn(git_dir, repo_root, kind);

    fflush(stderr);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);

    len = lseek(tmp_fd, 0, SEEK_CUR);
    if (len < 0 || (size_t)len >= err_buf_size)
        len = (off_t)err_buf_size - 1;
    lseek(tmp_fd, 0, SEEK_SET);
    n = read(tmp_fd, err_buf, (size_t)len);
    if (n < 0)
        n = 0;
    err_buf[n] = '\0';
    close(tmp_fd);
    return rc;
}

static void test_quit_parses_nothing_and_clears_corrupted_state(void)
{
    char *git_dir, *repo_root;
    unsigned char master_edit[SG_SHA1_RAW_LEN];
    int rc;

    build_corrupted_multi_conflict(&git_dir, &repo_root, master_edit);

    rc = sg_pick_quit(git_dir, repo_root, SG_SEQ_CHERRY_PICK);
    CHECK(rc == 0, "--quit must succeed even with a malformed sequencer/todo, got rc=%d", rc);
    CHECK(sg_sequencer_kind_in_progress(git_dir) == 0,
         "--quit should leave nothing 'in progress' behind");

    free(git_dir);
    free(repo_root);
}

static void test_abort_never_reads_sequencer_todo(void)
{
    char *git_dir, *repo_root;
    unsigned char master_edit[SG_SHA1_RAW_LEN];
    unsigned char master_after[SG_SHA1_RAW_LEN];
    int rc;

    build_corrupted_multi_conflict(&git_dir, &repo_root, master_edit);

    rc = sg_pick_abort(git_dir, repo_root, SG_SEQ_CHERRY_PICK);
    CHECK(rc == 0, "--abort must succeed even with a malformed sequencer/todo, got rc=%d", rc);
    CHECK(sg_sequencer_kind_in_progress(git_dir) == 0,
         "--abort should leave nothing 'in progress' behind");
    CHECK(sg_ref_read_branch(git_dir, "master", master_after) == 0, "read master after abort");
    CHECK(memcmp(master_after, master_edit, SG_SHA1_RAW_LEN) == 0,
         "--abort should restore master to the pre-pick commit (sequencer/head's own value), "
         "without ever needing sequencer/todo");

    free(git_dir);
    free(repo_root);
}

static void test_continue_error_names_a_working_command(void)
{
    char *git_dir, *repo_root;
    unsigned char master_edit[SG_SHA1_RAW_LEN];
    int rc;
    char err[4096];

    build_corrupted_multi_conflict(&git_dir, &repo_root, master_edit);

    rc = call_capturing_stderr(sg_pick_continue, git_dir, repo_root, SG_SEQ_CHERRY_PICK, err,
                               sizeof(err));
    CHECK(rc == 1, "--continue must still refuse on a malformed sequencer/todo, got rc=%d", rc);
    CHECK(strstr(err, "--abort") != NULL,
         "the refusal message must name --abort, a command that actually works on this input, "
         "got '%s'",
         err);
    CHECK(strstr(err, "--continue") == NULL && strstr(err, "--skip") == NULL,
         "the refusal message must NOT tell the user to re-run --continue or --skip -- both fail "
         "identically on this input, got '%s'",
         err);
    CHECK(sg_sequencer_kind_in_progress(git_dir) == SG_SEQ_CHERRY_PICK,
         "the paused state must survive the refusal (still recoverable via --abort/--quit)");

    free(git_dir);
    free(repo_root);
}


/* Runs `sg status` (via sg_cmd_status, same technique as
   tests/test_stash_show.c's stdout capture) inside repo_root, capturing
   stdout into out (truncated to fit; NUL-terminated). */
static int run_status_capture(const char *repo_root, char *out, size_t out_size)
{
    char cwd[4096];
    char tmpl[] = "/tmp/sg_pick_engine_stdout_XXXXXX";
    int tmp_fd;
    int saved_stdout;
    off_t len;
    ssize_t n;
    int rc;
    char *argv[2];

    CHECK(getcwd(cwd, sizeof(cwd)) != NULL, "getcwd failed");
    CHECK(chdir(repo_root) == 0, "chdir to repo_root failed");

    tmp_fd = mkstemp(tmpl);
    if (tmp_fd < 0) {
        fprintf(stderr, "mkstemp failed\n");
        exit(1);
    }
    unlink(tmpl);

    argv[0] = (char *)"status";
    argv[1] = NULL;

    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    dup2(tmp_fd, STDOUT_FILENO);

    rc = sg_cmd_status(1, argv);

    fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);

    len = lseek(tmp_fd, 0, SEEK_CUR);
    if (len < 0 || (size_t)len >= out_size)
        len = (off_t)out_size - 1;
    lseek(tmp_fd, 0, SEEK_SET);
    n = read(tmp_fd, out, (size_t)len);
    out[n > 0 ? n : 0] = '\0';
    close(tmp_fd);

    CHECK(chdir(cwd) == 0, "chdir back failed");
    return rc;
}

static void test_status_banner_survives_corrupted_todo(void)
{
    char *git_dir, *repo_root;
    unsigned char master_edit[SG_SHA1_RAW_LEN];
    char out[8192];

    build_corrupted_multi_conflict(&git_dir, &repo_root, master_edit);

    run_status_capture(repo_root, out, sizeof(out));
    CHECK(strstr(out, "You are currently cherry-picking commit ") != NULL,
         "sg status must still print the banner (with its 7-hex commit name) when "
         "sequencer/todo is malformed, got:\n%s",
         out);

    free(git_dir);
    free(repo_root);
}

static void test_skip_error_names_a_working_command(void)
{
    char *git_dir, *repo_root;
    unsigned char master_edit[SG_SHA1_RAW_LEN];
    int rc;
    char err[4096];

    build_corrupted_multi_conflict(&git_dir, &repo_root, master_edit);

    rc = call_capturing_stderr(sg_pick_skip, git_dir, repo_root, SG_SEQ_CHERRY_PICK, err,
                               sizeof(err));
    CHECK(rc == 1, "--skip must still refuse on a malformed sequencer/todo, got rc=%d", rc);
    CHECK(strstr(err, "--abort") != NULL,
         "the refusal message must name --abort, got '%s'", err);
    CHECK(strstr(err, "--continue") == NULL && strstr(err, "--skip") == NULL,
         "the refusal message must NOT name --continue or --skip, got '%s'", err);

    free(git_dir);
    free(repo_root);
}

/* ==================== a THIRD review round: -m dropped by --continue/
   --skip, and the abort-safety invariant every exit path must hold
   (Phase 57 spec section 5b's addendum) ==================== */

/* Removes a loose object's file from disk, so sg_object_read on it fails --
   simulates ANY mid-sequence I/O error (a partial write, a full disk), not
   specifically the mainline-forgotten path this bug was first found
   through (that path is now closed entirely by the -m + multi-commit
   rejection tested separately below). */
static void corrupt_object(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN])
{
    char hex[SG_SHA1_HEX_LEN + 1];
    char path[4096];

    sg_sha1_to_hex(id, hex);
    snprintf(path, sizeof(path), "%s/objects/%.2s/%s", git_dir, hex, hex + 2);
    CHECK(remove(path) == 0, "should be able to remove the object file to corrupt it");
}

static void test_mainline_rejected_with_multiple_commits(void)
{
    char *repo_root;
    char *git_dir = make_tmp_repo(&repo_root);
    unsigned char root[SG_SHA1_RAW_LEN], feat1[SG_SHA1_RAW_LEN], feat2[SG_SHA1_RAW_LEN];
    sg_pick_opts opts;
    int rc;

    build_commit(git_dir, repo_root, "master", "line1\n", NULL, "A", "a@example.com",
                "root\n", 1, root);
    build_commit(git_dir, repo_root, "feature", "line1\nline2\n", root, "A", "a@example.com",
                "feat1\n", 0, feat1);
    build_commit(git_dir, repo_root, "feature", "line1\nline2\nline3\n", feat1, "A", "a@example.com",
                "feat2\n", 0, feat2);

    memset(&opts, 0, sizeof(opts));
    opts.mainline = 1;
    {
        unsigned char todo[2][SG_SHA1_RAW_LEN];

        memcpy(todo[0], feat1, SG_SHA1_RAW_LEN);
        memcpy(todo[1], feat2, SG_SHA1_RAW_LEN);
        rc = sg_pick_start(git_dir, repo_root, SG_SEQ_CHERRY_PICK, todo, 2, &opts);
    }
    CHECK(rc == 1, "-m with more than one commit must be refused, got rc=%d", rc);
    CHECK(sg_sequencer_kind_in_progress(git_dir) == 0,
         "a refused start must not leave any paused state behind");

    free(git_dir);
    free(repo_root);
}

static void test_mid_sequence_error_keeps_abort_safety_correct(void)
{
    char *repo_root;
    char *git_dir = make_tmp_repo(&repo_root);
    unsigned char root[SG_SHA1_RAW_LEN], feat1[SG_SHA1_RAW_LEN], feat2[SG_SHA1_RAW_LEN];
    sg_pick_opts opts;
    int rc;
    unsigned char head_after_error[SG_SHA1_RAW_LEN];

    build_commit(git_dir, repo_root, "master", "line1\n", NULL, "A", "a@example.com",
                "root\n", 1, root);
    build_commit(git_dir, repo_root, "feature", "line1\nline2\n", root, "A", "a@example.com",
                "feat1\n", 0, feat1);
    build_commit(git_dir, repo_root, "feature", "line1\nline2\nline3\n", feat1, "A", "a@example.com",
                "feat2\n", 0, feat2);

    corrupt_object(git_dir, feat2);

    memset(&opts, 0, sizeof(opts));
    {
        unsigned char todo[2][SG_SHA1_RAW_LEN];

        memcpy(todo[0], feat1, SG_SHA1_RAW_LEN);
        memcpy(todo[1], feat2, SG_SHA1_RAW_LEN);
        rc = sg_pick_start(git_dir, repo_root, SG_SEQ_CHERRY_PICK, todo, 2, &opts);
    }
    CHECK(rc == 1, "the sequence should stop with an error on feat2, got rc=%d", rc);

    CHECK(sg_ref_resolve_head(git_dir, head_after_error) == 0, "read HEAD after the error");
    CHECK(memcmp(head_after_error, root, SG_SHA1_RAW_LEN) != 0,
         "HEAD should have advanced past feat1 before the error on feat2");

    {
        sg_seq_kind kind;
        int has_sequence;
        unsigned char orig_head[SG_SHA1_RAW_LEN];
        unsigned char abort_safety[SG_SHA1_RAW_LEN];

        CHECK(sg_sequencer_abort_target(git_dir, &kind, &has_sequence, orig_head, abort_safety) == 0,
             "sg_sequencer_abort_target should succeed after the mid-sequence error");
        CHECK(has_sequence == 1, "a 2-commit sequence should have has_sequence set");
        CHECK(memcmp(abort_safety, head_after_error, SG_SHA1_RAW_LEN) == 0,
             "abort_safety on disk must equal the REAL current HEAD after a mid-sequence error, "
             "not the stale value from before feat1 was picked");
    }

    /* And --abort must actually work now, not wrongly refuse with "HEAD
       has moved since the pick stopped". */
    rc = sg_pick_abort(git_dir, repo_root, SG_SEQ_CHERRY_PICK);
    CHECK(rc == 0, "--abort should succeed after the mid-sequence error, got rc=%d", rc);

    free(git_dir);
    free(repo_root);
}

int main(void)
{
    test_clean_pick_advances_branch();
    test_conflict_leaves_stages_and_state();
    test_empty_pick_detected();
    test_revert_author_from_environment();
    test_revert_continue_author_from_environment();
    test_continue_message_has_no_extra_blank_line();
    test_quit_parses_nothing_and_clears_corrupted_state();
    test_abort_never_reads_sequencer_todo();
    test_continue_error_names_a_working_command();
    test_skip_error_names_a_working_command();
    test_status_banner_survives_corrupted_todo();
    test_mainline_rejected_with_multiple_commits();
    test_mid_sequence_error_keeps_abort_safety_correct();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all pick_engine tests passed\n");
    return 0;
}
