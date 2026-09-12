/* Phase 70b: "@{0}" names a ref's CURRENT value, not the reflog's own last
   new_id -- these differ whenever the ref file was moved without a
   matching reflog append (a hand-edited ref file, or a symref target moved
   out from under a stale log entry). Measured against real git 2.55.0;
   see CLAUDE.md's sg_rev_parse_commit bullet and docs/DESIGN.md's Phase
   70b section for the full measured tables these fixtures are drawn from.

   Two boundaries this fix must NOT cross, each with its own dedicated
   test below: (1) the reflog must still be required to EXIST -- @{0} on a
   ref with no reflog at all still refuses; (2) the bare "@{0}" spelling's
   pre-existing, deliberate divergence from real git when the CURRENT
   branch's reflog has been deleted entirely is left exactly as it was
   (CLAUDE.md: "do not invent an asymmetry between sg's own two
   spellings" -- this is the opposite instruction, don't fix it). */
#include "sg/revparse.h"

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
#include <unistd.h>

static int failures = 0;
static long long time_seq = 5000000;

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
    static char template[] = "/tmp/sg_revparse_at0_test_XXXXXX";
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

static void make_commit(const char *git_dir, const char *message, unsigned char commit_id_out[SG_SHA1_RAW_LEN])
{
    unsigned char blob_id[SG_SHA1_RAW_LEN];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    sg_flat_entry entry;
    sg_commit commit;
    unsigned char *serialized;
    size_t serialized_len;

    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, message, strlen(message), blob_id) == 0,
         "blob write failed for '%s'", message);

    entry.path = (char *)"file.txt";
    entry.mode = 0100644;
    memcpy(entry.sha1, blob_id, SG_SHA1_RAW_LEN);
    CHECK(sg_tree_build(git_dir, &entry, 1, tree_id) == 0, "tree build failed for '%s'", message);

    memset(&commit, 0, sizeof(commit));
    memcpy(commit.tree, tree_id, SG_SHA1_RAW_LEN);
    commit.author_name = (char *)"tester";
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
    CHECK(sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len, commit_id_out) == 0,
         "commit write failed for '%s'", message);
    free(serialized);
}

/* ---- 1. Branch hand-edited past its reflog (coordinator's own primary
   fixture): reflog has two entries (0->c1, c1->c2); the ref FILE is then
   overwritten directly to c5, bypassing the log entirely. ---- */

static void test_at_zero_branch_hand_edited_uses_current_value(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN], c2[SG_SHA1_RAW_LEN], c5[SG_SHA1_RAW_LEN];
    unsigned char zero[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    memset(zero, 0, SG_SHA1_RAW_LEN);
    make_commit(git_dir, "c1", c1);
    make_commit(git_dir, "c2", c2);
    make_commit(git_dir, "c5", c5);

    CHECK(sg_ref_write_path(git_dir, "refs/heads/b", c2) == 0, "initial branch write failed");
    CHECK(sg_reflog_append(git_dir, "refs/heads/b", zero, c1, "first", NULL) == 0, "append 1 failed");
    CHECK(sg_reflog_append(git_dir, "refs/heads/b", c1, c2, "second", NULL) == 0, "append 2 failed");

    /* The hand edit: ref file moved to c5 with NO matching log entry. */
    CHECK(sg_ref_write_path(git_dir, "refs/heads/b", c5) == 0, "hand-edit write failed");

    CHECK(sg_rev_parse_commit(git_dir, "b", out) == 0 && memcmp(out, c5, SG_SHA1_RAW_LEN) == 0,
         "bare 'b' should read the current ref file (c5)");
    CHECK(sg_rev_parse_commit(git_dir, "b@{0}", out) == 0 && memcmp(out, c5, SG_SHA1_RAW_LEN) == 0,
         "'b@{0}' should be the CURRENT value (c5), not the log's own last new_id (c2)");
    /* Control: N>=1 is completely unaffected by this fix. */
    CHECK(sg_rev_parse_commit(git_dir, "b@{1}", out) == 0 && memcmp(out, c1, SG_SHA1_RAW_LEN) == 0,
         "control: 'b@{1}' should still read the log's own entry (c1), unaffected by the fix");
    CHECK(sg_rev_parse_commit(git_dir, "b@{2}", out) != 0,
         "control: 'b@{2}' is out of range (log has 2 entries) and must still refuse");

    free(git_dir);
}

/* ---- 2. Boundary #1: the reflog must still be required to EXIST. ---- */

static void test_at_zero_refuses_when_log_missing_entirely(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "c1", c1);
    CHECK(sg_ref_write_path(git_dir, "refs/heads/nolog", c1) == 0, "branch write failed");
    /* Deliberately no sg_reflog_append call at all -- logs/refs/heads/nolog
       never exists. */

    CHECK(sg_rev_parse_commit(git_dir, "nolog", out) == 0 && memcmp(out, c1, SG_SHA1_RAW_LEN) == 0,
         "bare 'nolog' should still resolve (current value, no log needed)");
    CHECK(sg_rev_parse_commit(git_dir, "nolog@{0}", out) != 0,
         "'nolog@{0}' must refuse when the reflog does not exist at all -- @{0} still needs the "
         "EXISTENCE of the log, only the VALUE source changed");

    free(git_dir);
}

/* ---- 3. Boundary #2 (preserved, NOT fixed): bare "@{0}" still refuses
   when the CURRENT branch's reflog has been deleted, even though real git
   resolves it there (CLAUDE.md's own recorded, deliberate divergence --
   "do not invent an asymmetry between sg's own two spellings"). A NAMED
   spelling of the SAME branch is not part of that divergence; it already
   agrees with git (both refuse), so it is pinned here too as a control
   showing bullet 1's existence rule, not bullet 2's asymmetry, is what is
   firing. ---- */

static void test_bare_at_zero_still_refuses_when_current_branch_log_missing(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "c1", c1);
    CHECK(sg_ref_write_path(git_dir, "refs/heads/master", c1) == 0, "branch write failed");
    CHECK(sg_ref_set_head(git_dir, "master", NULL) == 0, "failed to point HEAD at master");
    /* No reflog for "refs/heads/master" at all. */

    CHECK(sg_rev_parse_commit(git_dir, "@{0}", out) != 0,
         "bare '@{0}' must still refuse when the current branch's log is missing -- this is a "
         "deliberate, pre-existing divergence from real git (CLAUDE.md), NOT fixed by Phase 70b");
    CHECK(sg_rev_parse_commit(git_dir, "master@{0}", out) != 0,
         "control: the NAMED spelling 'master@{0}' also refuses here -- both tools agree on this "
         "one (bullet 1's existence rule), so this is not the same divergence as the bare form");

    free(git_dir);
}

/* ---- 4. Symbolic HEAD: master's file hand-edited past both HEAD's own
   and master's own reflog -- HEAD@{0}, bare @{0} and master@{0} must all
   three agree on the CURRENT value (c3), even though each of the first two
   reads a DIFFERENT log file for its existence check. ---- */

static void test_at_zero_symbolic_head_uses_current_value(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c2[SG_SHA1_RAW_LEN], c3[SG_SHA1_RAW_LEN];
    unsigned char zero[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    memset(zero, 0, SG_SHA1_RAW_LEN);
    make_commit(git_dir, "c2", c2);
    make_commit(git_dir, "c3", c3);

    CHECK(sg_ref_write_path(git_dir, "refs/heads/master", c2) == 0, "initial branch write failed");
    CHECK(sg_ref_set_head(git_dir, "master", NULL) == 0, "failed to point HEAD at master");
    CHECK(sg_reflog_append(git_dir, "HEAD", zero, c2, "head", NULL) == 0, "HEAD append failed");
    CHECK(sg_reflog_append(git_dir, "refs/heads/master", zero, c2, "branch", NULL) == 0,
         "branch append failed");

    /* The hand edit: master's ref FILE moved to c3, neither log touched. */
    CHECK(sg_ref_write_path(git_dir, "refs/heads/master", c3) == 0, "hand-edit write failed");

    CHECK(sg_rev_parse_commit(git_dir, "HEAD@{0}", out) == 0 && memcmp(out, c3, SG_SHA1_RAW_LEN) == 0,
         "'HEAD@{0}' should be the CURRENT value (c3, via the symbolic link to master)");
    CHECK(sg_rev_parse_commit(git_dir, "@{0}", out) == 0 && memcmp(out, c3, SG_SHA1_RAW_LEN) == 0,
         "bare '@{0}' should also be c3 (the current branch's current value)");
    CHECK(sg_rev_parse_commit(git_dir, "master@{0}", out) == 0 && memcmp(out, c3, SG_SHA1_RAW_LEN) == 0,
         "'master@{0}' should also be c3 -- all three spellings now agree, matching git 2.55.0 "
         "measured on this exact fixture shape");
    /* Control: N>=1 unaffected -- this log has only ONE entry, so index 1
       is out of range and must still refuse, exactly as it did before the
       fix (the existence/bounds check inside sg_reflog_at is untouched). */
    CHECK(sg_rev_parse_commit(git_dir, "HEAD@{1}", out) != 0,
         "control: 'HEAD@{1}' is out of range (this log has only one entry) and must still refuse");

    free(git_dir);
}

/* ---- 5. Detached HEAD: the raw HEAD file itself hand-edited past its own
   reflog. ---- */

static void test_at_zero_detached_head_uses_current_value(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c4[SG_SHA1_RAW_LEN], c3[SG_SHA1_RAW_LEN];
    unsigned char zero[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    memset(zero, 0, SG_SHA1_RAW_LEN);
    make_commit(git_dir, "c4", c4);
    make_commit(git_dir, "c3", c3);

    CHECK(sg_ref_set_head_detached(git_dir, c4, NULL) == 0, "failed to detach HEAD at c4");
    CHECK(sg_reflog_append(git_dir, "HEAD", zero, c4, "detach", NULL) == 0, "HEAD append failed");

    /* The hand edit: HEAD's raw content moved to c3, log not touched --
       sg_ref_set_head_detached with reflog_msg == NULL writes only the
       file, exactly the same shape as any other hand edit in this file. */
    CHECK(sg_ref_set_head_detached(git_dir, c3, NULL) == 0, "hand-edit detach write failed");

    CHECK(sg_rev_parse_commit(git_dir, "HEAD@{0}", out) == 0 && memcmp(out, c3, SG_SHA1_RAW_LEN) == 0,
         "'HEAD@{0}' should be the CURRENT value (c3) while detached");
    CHECK(sg_rev_parse_commit(git_dir, "@{0}", out) == 0 && memcmp(out, c3, SG_SHA1_RAW_LEN) == 0,
         "bare '@{0}' should also be c3 while detached (falls back to logs/HEAD, same as HEAD@{0})");
    /* This log also has only ONE entry, so index 1 is out of range --
       unaffected by the fix, same bounds check as every other case. */
    CHECK(sg_rev_parse_commit(git_dir, "HEAD@{1}", out) != 0,
         "control: 'HEAD@{1}' is out of range (this log has only one entry) and must still refuse");

    free(git_dir);
}

/* ---- 6. The most valuable fixture (per the coordinator): a MOVED SYMREF
   TARGET, reachable with sg's own commands alone (sg clone's
   sg_ref_set_symref leaves a reflog entry on refs/remotes/<remote>/HEAD;
   a later sg fetch moves the target ref without touching that entry) --
   no hand-editing of any file required. ---- */

static void test_at_zero_tracks_a_moved_symref_target(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN], c2[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "c1", c1);
    make_commit(git_dir, "c2", c2);

    CHECK(sg_ref_write_path(git_dir, "refs/remotes/origin/rmt", c1) == 0,
         "failed to write refs/remotes/origin/rmt");
    /* A non-NULL reflog_msg here is exactly what sg clone does -- it is
       what gives refs/remotes/origin/HEAD its own (soon-to-be-stale) log
       entry in the first place. */
    CHECK(sg_ref_set_symref(git_dir, "refs/remotes/origin/HEAD", "refs/remotes/origin/rmt",
                            "clone: from test") == 0,
         "failed to write refs/remotes/origin/HEAD symref with a reflog entry");

    /* Sanity: before the target moves, @{0} already agrees with the
       current (and logged) value. */
    CHECK(sg_rev_parse_commit(git_dir, "origin@{0}", out) == 0 && memcmp(out, c1, SG_SHA1_RAW_LEN) == 0,
         "sanity: 'origin@{0}' should be c1 before the target moves");

    /* The move: refs/remotes/origin/rmt goes from c1 to c2 (an ordinary
       "sg fetch" doing its job), and refs/remotes/origin/HEAD's OWN log
       entry from the clone above is never touched. */
    CHECK(sg_ref_write_path(git_dir, "refs/remotes/origin/rmt", c2) == 0, "failed to move rmt to c2");

    CHECK(sg_rev_parse_commit(git_dir, "origin@{0}", out) == 0 && memcmp(out, c2, SG_SHA1_RAW_LEN) == 0,
         "'origin@{0}' should now track the MOVED target (c2), not the stale clone-time log entry "
         "(c1) -- this is the bug the coordinator's review found reachable via sg clone + sg fetch "
         "alone, no hand-edited file needed");

    free(git_dir);
}

int main(void)
{
    test_at_zero_branch_hand_edited_uses_current_value();
    test_at_zero_refuses_when_log_missing_entirely();
    test_bare_at_zero_still_refuses_when_current_branch_log_missing();
    test_at_zero_symbolic_head_uses_current_value();
    test_at_zero_detached_head_uses_current_value();
    test_at_zero_tracks_a_moved_symref_target();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all revparse at-zero tests passed\n");
    return 0;
}
