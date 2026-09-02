/* Phase 57 spec section 4: `sg revert`'s message construction. All expected
   strings are transcribed literally from the spec's measured tables (4.1,
   4.2, and the 7-row 4.3 "Reapply" table, including its 3 negative rows) --
   this test does not reimplement the wrapping rule, it pins it. Own
   `failures` counter and `CHECK` macro, no shared framework, per project
   convention. */

#include "sg/pick.h"

#include "sg/hash.h"
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
#include <unistd.h>

static int failures = 0;
static long long time_seq = 4000000;

#define CHECK(cond, ...)                                                                         \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);                                  \
            fprintf(stderr, __VA_ARGS__);                                                         \
            fprintf(stderr, "\n");                                                                \
            failures++;                                                                           \
        }                                                                                          \
    } while (0)

static char *make_tmp_repo(char **repo_root_out)
{
    static char template[] = "/tmp/sg_revert_msg_test_XXXXXX";
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

/* Builds a commit with a single file f.txt = content, optionally 0, 1 or 2
   parents (parent2 == NULL means single-parent or root). sync writes the
   working directory + index to match (only meaningful for the branch
   actually checked out). */
static void build_commit_np(const char *git_dir, const char *repo_root, const char *branch,
                            const char *content, const unsigned char *parent1,
                            const unsigned char *parent2, const char *message, int sync,
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
    if (parent2 != NULL) {
        commit.parents = malloc(2 * sizeof(*commit.parents));
        memcpy(commit.parents[0], parent1, SG_SHA1_RAW_LEN);
        memcpy(commit.parents[1], parent2, SG_SHA1_RAW_LEN);
        commit.parent_count = 2;
    } else if (parent1 != NULL) {
        commit.parents = malloc(sizeof(*commit.parents));
        memcpy(commit.parents[0], parent1, SG_SHA1_RAW_LEN);
        commit.parent_count = 1;
    }
    commit.author_name = (char *)"Tester";
    commit.author_email = (char *)"tester@example.com";
    commit.author_time = time_seq++;
    strcpy(commit.author_tz, "+0000");
    commit.committer_name = commit.author_name;
    commit.committer_email = commit.author_email;
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

static char *read_head_message(const char *git_dir)
{
    unsigned char head[SG_SHA1_RAW_LEN];
    sg_obj_type type;
    unsigned char *content;
    size_t content_len;
    sg_commit c;
    char *out;

    CHECK(sg_ref_read_branch(git_dir, "master", head) == 0, "read master after revert");
    CHECK(sg_object_read(git_dir, head, &type, &content, &content_len) == 0 && type == SG_OBJ_COMMIT,
         "new commit should be readable");
    CHECK(sg_commit_parse(content, content_len, &c) == 0, "new commit should parse");
    out = strdup(c.message);
    free(content);
    sg_commit_free(&c);
    return out;
}

/* ==================== 4.3's 7-row Reapply table, plus 4.1 ==================== */

static void run_one_row(const char *subject, const char *expected_new_subject)
{
    char *repo_root;
    char *git_dir = make_tmp_repo(&repo_root);
    unsigned char edit_id_buf[SG_SHA1_RAW_LEN];
    unsigned char root_id[SG_SHA1_RAW_LEN];
    char *msgbuf;
    unsigned char edit_id[SG_SHA1_RAW_LEN];
    sg_pick_opts opts;
    int rc;
    char edit_hex[SG_SHA1_HEX_LEN + 1];
    char expected[4096];
    char *got;

    msgbuf = malloc(strlen(subject) + 2);
    snprintf(msgbuf, strlen(subject) + 2, "%s\n", subject);

    build_commit_np(git_dir, repo_root, "master", "line1\nline2\n", NULL, NULL, "root\n", 1, root_id);
    build_commit_np(git_dir, repo_root, "master", "line1\nCHANGED\n", root_id, NULL, msgbuf, 1,
                    edit_id_buf);
    memcpy(edit_id, edit_id_buf, SG_SHA1_RAW_LEN);
    free(msgbuf);

    memset(&opts, 0, sizeof(opts));
    rc = sg_pick_start(git_dir, repo_root, SG_SEQ_REVERT, (unsigned char (*)[SG_SHA1_RAW_LEN])edit_id,
                       1, &opts);
    CHECK(rc == 0, "reverting subject '%s' should succeed cleanly, got rc=%d", subject, rc);

    sg_sha1_to_hex(edit_id, edit_hex);
    snprintf(expected, sizeof(expected), "%s\n\nThis reverts commit %s.\n", expected_new_subject,
            edit_hex);

    got = read_head_message(git_dir);
    CHECK(got != NULL && strcmp(got, expected) == 0,
         "subject '%s': expected message %s, got %s", subject, expected, got != NULL ? got : "(null)");
    free(got);

    free(git_dir);
    free(repo_root);
}

static void test_revert_message_rows(void)
{
    /* Positive: subject already begins with the literal 8 bytes
       `Revert "` -> Reapply, rest kept verbatim, no closing-quote or
       balance check. */
    run_one_row("Revert \"x y\"", "Reapply \"x y\"");
    run_one_row("Revert \"x\" and more", "Reapply \"x\" and more");
    run_one_row("Revert \"unclosed", "Reapply \"unclosed");
    run_one_row("Revert \"\"", "Reapply \"\"");

    /* Negative controls: each falsifies a different looser rule. */
    run_one_row("Reapply \"x y\"", "Revert \"Reapply \"x y\"\"");
    run_one_row("revert \"lower\"", "Revert \"revert \"lower\"\"");
    run_one_row("Revert  \"two spaces\"", "Revert \"Revert  \"two spaces\"\"");

    /* 4.1's plain wrapping case, an ordinary subject. */
    run_one_row("an ordinary change", "Revert \"an ordinary change\"");

    /* Boundary rows (review finding, Phase 57b round 2): the 7 rows above
       are all either well past 8 bytes or an obvious non-match; none of
       them exercises the boundary of the 8-byte literal-prefix comparison
       itself. Each row below was statically traced against
       build_revert_subject_line before being added -- this is filling in
       missing WITNESSES for already-correct behaviour, not chasing a bug
       (see build_revert_subject_line's own comment in pick.c). */
    /* Subject IS the 8-byte prefix, with nothing after it: still matches
       (no closing-quote/balance check), rest is the empty string, so the
       swap produces `Reapply "` with nothing following the opening quote. */
    run_one_row("Revert \"", "Reapply \"");
    /* Subject shorter than 8 bytes: strncmp's own length guard means this
       can never match the prefix, so it falls through to plain wrapping. */
    run_one_row("Rev", "Revert \"Rev\"");
    /* Empty subject: also shorter than 8 bytes, same fallthrough, and
       exercises wrapping an empty string specifically. */
    run_one_row("", "Revert \"\"");
}

/* ==================== 4.2: merge, with -m ==================== */

static void test_revert_message_merge(void)
{
    char *repo_root;
    char *git_dir = make_tmp_repo(&repo_root);
    unsigned char root_id[SG_SHA1_RAW_LEN];
    unsigned char p1_id[SG_SHA1_RAW_LEN];
    unsigned char dummy_id[SG_SHA1_RAW_LEN];
    unsigned char merge_id[SG_SHA1_RAW_LEN];
    sg_pick_opts opts;
    int rc;
    char merge_hex[SG_SHA1_HEX_LEN + 1];
    char p1_hex[SG_SHA1_HEX_LEN + 1];
    char expected[4096];
    char *got;

    build_commit_np(git_dir, repo_root, "master", "root\n", NULL, NULL, "root\n", 0, root_id);
    build_commit_np(git_dir, repo_root, "master", "root\n", root_id, NULL, "side\n", 0, p1_id);
    build_commit_np(git_dir, repo_root, "master", "root\n", root_id, NULL, "other side\n", 0,
                    dummy_id);
    /* M's own tree ("merged\n") differs from both parents, so reverting -m1
       (taking parent 1's tree, "root\n") is a real, non-empty change
       against HEAD == M. */
    build_commit_np(git_dir, repo_root, "master", "merged\n", p1_id, dummy_id, "a merge subject\n", 1,
                    merge_id);

    memset(&opts, 0, sizeof(opts));
    opts.mainline = 1;
    rc = sg_pick_start(git_dir, repo_root, SG_SEQ_REVERT, (unsigned char (*)[SG_SHA1_RAW_LEN])merge_id,
                       1, &opts);
    CHECK(rc == 0, "reverting a merge with -m 1 should succeed cleanly, got rc=%d", rc);

    sg_sha1_to_hex(merge_id, merge_hex);
    sg_sha1_to_hex(p1_id, p1_hex);
    snprintf(expected, sizeof(expected),
            "Revert \"a merge subject\"\n\nThis reverts commit %s, reversing\nchanges made to %s.\n",
            merge_hex, p1_hex);

    got = read_head_message(git_dir);
    CHECK(got != NULL && strcmp(got, expected) == 0, "expected merge revert message %s, got %s",
         expected, got != NULL ? got : "(null)");
    free(got);

    free(git_dir);
    free(repo_root);
}

int main(void)
{
    test_revert_message_rows();
    test_revert_message_merge();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
