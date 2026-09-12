/* Phase 70: sg_rev_parse_ref_path implements git's full six-rule
   gitrevisions lookup table (ref_rev_parse_rules), tried in order with no
   early return, plus a revparse-only name gate and a symref-following
   reader (sg_ref_read_path_resolved). See CLAUDE.md's sg_rev_parse_commit
   bullet and docs/DESIGN.md's Phase 70 section for the measured tables
   this file's fixtures are drawn from. */
#include "sg/revparse.h"

#include "sg/hash.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    static char template[] = "/tmp/sg_revparse_refpath_test_XXXXXX";
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
    static long long time_seq = 3000000;

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

static void write_branch(const char *git_dir, const char *name, const unsigned char id[SG_SHA1_RAW_LEN])
{
    char ref_path[256];

    snprintf(ref_path, sizeof(ref_path), "refs/heads/%s", name);
    CHECK(sg_ref_write_path(git_dir, ref_path, id) == 0, "failed to write branch '%s'", name);
}

static void write_tag(const char *git_dir, const char *name, const unsigned char id[SG_SHA1_RAW_LEN])
{
    char ref_path[256];

    snprintf(ref_path, sizeof(ref_path), "refs/tags/%s", name);
    CHECK(sg_ref_write_path(git_dir, ref_path, id) == 0, "failed to write tag '%s'", name);
}

/* Writes an annotated tag OBJECT pointing at target (a commit), returning
   the tag object's own id -- does NOT create any refs/tags/ ref, since the
   caller here wants to point a refs/remotes/... entry directly at the tag
   object itself (to prove sg_rev_parse_object does not peel it). */
static void make_annotated_tag_object(const char *git_dir, const char *name,
                                      const unsigned char target[SG_SHA1_RAW_LEN],
                                      unsigned char tag_id_out[SG_SHA1_RAW_LEN])
{
    sg_tag tag;
    unsigned char *serialized;
    size_t serialized_len;

    memset(&tag, 0, sizeof(tag));
    memcpy(tag.object, target, SG_SHA1_RAW_LEN);
    tag.object_type = SG_OBJ_COMMIT;
    tag.tag_name = (char *)name;
    tag.tagger_name = (char *)"tester";
    tag.tagger_email = (char *)"tester@example.com";
    tag.tagger_time = 4000000;
    strcpy(tag.tagger_tz, "+0000");
    tag.message = (char *)"annotated\n";

    CHECK(sg_tag_serialize(&tag, &serialized, &serialized_len) == 0, "tag serialize failed for '%s'", name);
    CHECK(sg_loose_write(git_dir, SG_OBJ_TAG, serialized, serialized_len, tag_id_out) == 0,
         "tag write failed for '%s'", name);
    free(serialized);
}

/* Writes ordinary hex-oid content straight to an arbitrary path under
   git_dir, exercising rule 1's general form (any file under $GIT_DIR whose
   first 40 bytes are hex) without going through any of the ref-writing
   helpers, which all live under refs/. */
static void write_raw_oid_file(const char *git_dir, const char *rel_path, const unsigned char id[SG_SHA1_RAW_LEN])
{
    char hex[SG_SHA1_HEX_LEN + 2];
    char full_path[SG_PATH_MAX];

    sg_sha1_to_hex(id, hex);
    hex[SG_SHA1_HEX_LEN] = '\n';
    hex[SG_SHA1_HEX_LEN + 1] = '\0';
    snprintf(full_path, sizeof(full_path), "%s/%s", git_dir, rel_path);
    CHECK(sg_write_file_mkdirs(full_path, (const unsigned char *)hex, strlen(hex), 0644) == 0,
         "failed to write raw oid file '%s'", rel_path);
}

/* ---- rule 1: a general "any file under $GIT_DIR" lookup, no early return */

static void test_rule1_general_form(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];
    char ref_path[SG_PATH_MAX];

    make_commit(git_dir, "c1", c1);
    write_raw_oid_file(git_dir, "MERGE_HEAD", c1);

    CHECK(sg_rev_parse_ref_path(git_dir, "MERGE_HEAD", ref_path, sizeof(ref_path)) == 0 &&
             strcmp(ref_path, "MERGE_HEAD") == 0,
         "rule 1 should match a bare file under $GIT_DIR verbatim");
    CHECK(sg_rev_parse_commit(git_dir, "MERGE_HEAD", out) == 0 && memcmp(out, c1, SG_SHA1_RAW_LEN) == 0,
         "sg_rev_parse_commit should resolve MERGE_HEAD through rule 1");

    /* A non-hex file under $GIT_DIR must not match rule 1. */
    CHECK(sg_rev_parse_ref_path(git_dir, "config", ref_path, sizeof(ref_path)) != 0,
         "config (not a hex file) must not resolve via rule 1");

    free(git_dir);
}

/* ---- rule 2 beats rules 3 and 4 ---- */

static void test_rule2_beats_rules_3_and_4(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN], c2[SG_SHA1_RAW_LEN], c3[SG_SHA1_RAW_LEN], c4[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];
    char ref_path[SG_PATH_MAX];

    make_commit(git_dir, "c1", c1);
    make_commit(git_dir, "c2", c2);
    make_commit(git_dir, "c3", c3);
    make_commit(git_dir, "c4", c4);

    /* refs/x = c4, tag x = c3, branch x = c2 -- rule 2 (refs/x) must win. */
    CHECK(sg_ref_write_path(git_dir, "refs/x", c4) == 0, "failed to write refs/x");
    write_tag(git_dir, "x", c3);
    write_branch(git_dir, "x", c2);

    CHECK(sg_rev_parse_ref_path(git_dir, "x", ref_path, sizeof(ref_path)) == 0 &&
             strcmp(ref_path, "refs/x") == 0,
         "rule 2 (refs/x) should win over rule 3 (tag) and rule 4 (branch), got '%s'", ref_path);
    CHECK(sg_rev_parse_commit(git_dir, "x", out) == 0 && memcmp(out, c4, SG_SHA1_RAW_LEN) == 0,
         "sg_rev_parse_commit(\"x\") should resolve to c4 via rule 2");

    /* A branch LITERALLY named "tags/v1" must lose to the ordinary tag
       "v1" once rule 2 (refs/tags/v1) is tried -- this used to be exactly
       backwards (sg_ref_read_branch found the literal branch before rule
       2 was ever tried). */
    write_tag(git_dir, "v1", c1);
    write_branch(git_dir, "tags/v1", c2);
    CHECK(sg_rev_parse_ref_path(git_dir, "tags/v1", ref_path, sizeof(ref_path)) == 0 &&
             strcmp(ref_path, "refs/tags/v1") == 0,
         "rule 2 (refs/tags/v1, the tag) should win over the literal branch, got '%s'", ref_path);
    CHECK(sg_rev_parse_commit(git_dir, "tags/v1", out) == 0 && memcmp(out, c1, SG_SHA1_RAW_LEN) == 0,
         "sg_rev_parse_commit(\"tags/v1\") should resolve to the tag (c1)");

    free(git_dir);
}

/* ---- rule 3 beats rule 4 (the pre-existing tag-over-branch precedence,
   pinned again here directly against sg_rev_parse_ref_path rather than
   only through sg_rev_parse_commit) ---- */

static void test_rule3_beats_rule4(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char ctag[SG_SHA1_RAW_LEN], cbranch[SG_SHA1_RAW_LEN];
    char ref_path[SG_PATH_MAX];

    make_commit(git_dir, "tag-target", ctag);
    make_commit(git_dir, "branch-target", cbranch);
    write_tag(git_dir, "y", ctag);
    write_branch(git_dir, "y", cbranch);

    CHECK(sg_rev_parse_ref_path(git_dir, "y", ref_path, sizeof(ref_path)) == 0 &&
             strcmp(ref_path, "refs/tags/y") == 0,
         "rule 3 (tag) should win over rule 4 (branch) when there is no refs/y, got '%s'", ref_path);

    free(git_dir);
}

/* ---- rule 4 in isolation ---- */

static void test_rule4_isolation(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN];
    char ref_path[SG_PATH_MAX];

    make_commit(git_dir, "c1", c1);
    write_branch(git_dir, "onlybranch", c1);

    CHECK(sg_rev_parse_ref_path(git_dir, "onlybranch", ref_path, sizeof(ref_path)) == 0 &&
             strcmp(ref_path, "refs/heads/onlybranch") == 0,
         "rule 4 alone should resolve a plain branch name, got '%s'", ref_path);

    free(git_dir);
}

/* ---- rule 5 in isolation (an ordinary, non-symref refs/remotes/<name>) ---- */

static void test_rule5_isolation(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN];
    char ref_path[SG_PATH_MAX];

    make_commit(git_dir, "c1", c1);
    CHECK(sg_ref_write_path(git_dir, "refs/remotes/foo", c1) == 0, "failed to write refs/remotes/foo");

    CHECK(sg_rev_parse_ref_path(git_dir, "foo", ref_path, sizeof(ref_path)) == 0 &&
             strcmp(ref_path, "refs/remotes/foo") == 0,
         "rule 5 alone should resolve refs/remotes/<name>, got '%s'", ref_path);

    free(git_dir);
}

/* ---- rule 6 + symref-following: refs/remotes/<name>/HEAD is ordinarily a
   symref (exactly the shape sg clone itself creates via sg_ref_set_symref),
   and the resolver must follow it all the way to a commit. ---- */

static void test_rule6_symref_following(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];
    char ref_path[SG_PATH_MAX];

    make_commit(git_dir, "c1", c1);
    CHECK(sg_ref_write_path(git_dir, "refs/remotes/origin/rmt", c1) == 0,
         "failed to write refs/remotes/origin/rmt");
    CHECK(sg_ref_set_symref(git_dir, "refs/remotes/origin/HEAD", "refs/remotes/origin/rmt", NULL) == 0,
         "failed to write refs/remotes/origin/HEAD symref");

    /* "origin" only matches at rule 6 -- refs/remotes/origin itself is not
       a ref (only refs/remotes/origin/rmt and refs/remotes/origin/HEAD
       exist), so rule 5 must miss before rule 6 is tried. */
    CHECK(sg_rev_parse_ref_path(git_dir, "origin", ref_path, sizeof(ref_path)) == 0 &&
             strcmp(ref_path, "refs/remotes/origin/HEAD") == 0,
         "rule 6 should match 'origin' via refs/remotes/origin/HEAD, got '%s'", ref_path);

    /* And the whole thing must resolve end to end through
       sg_rev_parse_commit -> resolve_base -> sg_ref_read_path_resolved,
       following the symref rather than failing to hex-decode "ref: ...". */
    CHECK(sg_rev_parse_commit(git_dir, "origin", out) == 0 && memcmp(out, c1, SG_SHA1_RAW_LEN) == 0,
         "sg_rev_parse_commit(\"origin\") should follow the symref to c1");

    /* sg_ref_read_path_resolved directly, the same way. */
    CHECK(sg_ref_read_path_resolved(git_dir, "refs/remotes/origin/HEAD", out) == 0 &&
             memcmp(out, c1, SG_SHA1_RAW_LEN) == 0,
         "sg_ref_read_path_resolved should follow the symref directly");

    /* sg_ref_read_path itself (the non-following sibling) must still fail
       on the same symref -- this function is a NEW entry point, not a
       widening of the old one. */
    CHECK(sg_ref_read_path(git_dir, "refs/remotes/origin/HEAD", out) != 0,
         "sg_ref_read_path must NOT follow the symref (unchanged, non-following behavior)");

    free(git_dir);
}

/* sg_rev_parse_object has its own copy of the "HEAD is special, everything
   else re-reads ref_path" logic (a separate call site from resolve_base),
   and needs the identical sg_ref_read_path_resolved fix -- an annotated
   tag must not be peeled, but a symref result from rules 5/6 still has to
   be followed.

   The target is deliberately an ANNOTATED TAG OBJECT, not a commit: if the
   direct path were broken (e.g. reverted to the non-following
   sg_ref_read_path) it would fail silently and fall through to
   sg_rev_parse_commit's own fallback lower in this function -- which DOES
   correctly follow the symref via the already-fixed resolve_base, but ALSO
   peels the tag, giving the same commit id sg_rev_parse_object is supposed
   to refuse to peel. A plain commit target cannot tell the two paths
   apart; only a tag target can, since peeling changes both the id AND the
   type. */
static void test_rev_parse_object_follows_symref_too(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN];
    unsigned char tag_id[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];
    sg_obj_type type;
    char bad_path[SG_PATH_MAX];

    make_commit(git_dir, "c1", c1);
    make_annotated_tag_object(git_dir, "origin-tag", c1, tag_id);
    CHECK(sg_ref_write_path(git_dir, "refs/remotes/origin/rmt", tag_id) == 0,
         "failed to write refs/remotes/origin/rmt");
    CHECK(sg_ref_set_symref(git_dir, "refs/remotes/origin/HEAD", "refs/remotes/origin/rmt", NULL) == 0,
         "failed to write refs/remotes/origin/HEAD symref");

    CHECK(sg_rev_parse_object(git_dir, "origin", out, &type, bad_path, sizeof(bad_path)) == 0 &&
             type == SG_OBJ_TAG && memcmp(out, tag_id, SG_SHA1_RAW_LEN) == 0,
         "sg_rev_parse_object(\"origin\") should follow the symref to the TAG OBJECT itself "
         "(unpeeled), not fall through to a peeled commit");

    free(git_dir);
}

/* ---- section 2.4: rule 1 (and every rule) must not early-return ---- */

static void test_fallthrough_of_missing_refs_prefix(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN];
    char ref_path[SG_PATH_MAX];

    make_commit(git_dir, "c1", c1);
    /* "refs/foo" does not exist as a literal ref, but refs/tags/refs/foo
       does -- rule 3 applied to the whole string "refs/foo". The old code
       hard-refused the moment it saw a "refs/" prefix. */
    CHECK(sg_ref_write_path(git_dir, "refs/tags/refs/foo", c1) == 0,
         "failed to write refs/tags/refs/foo");

    CHECK(sg_rev_parse_ref_path(git_dir, "refs/foo", ref_path, sizeof(ref_path)) == 0 &&
             strcmp(ref_path, "refs/tags/refs/foo") == 0,
         "a missing literal 'refs/foo' should fall through to rule 3, got '%s'", ref_path);

    free(git_dir);
}

/* ---- section 2.5: the symref-hop bound (4 hops resolve, 5 do not) ---- */

static void test_symref_chain_bound(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "c1", c1);
    CHECK(sg_ref_write_path(git_dir, "refs/remotes/target", c1) == 0, "failed to write target");
    CHECK(sg_ref_set_symref(git_dir, "refs/remotes/s1", "refs/remotes/target", NULL) == 0, "s1 failed");
    CHECK(sg_ref_set_symref(git_dir, "refs/remotes/s2", "refs/remotes/s1", NULL) == 0, "s2 failed");
    CHECK(sg_ref_set_symref(git_dir, "refs/remotes/s3", "refs/remotes/s2", NULL) == 0, "s3 failed");
    CHECK(sg_ref_set_symref(git_dir, "refs/remotes/s4", "refs/remotes/s3", NULL) == 0, "s4 failed");
    CHECK(sg_ref_set_symref(git_dir, "refs/remotes/s5", "refs/remotes/s4", NULL) == 0, "s5 failed");

    /* direct target: 1 read */
    CHECK(sg_ref_read_path_resolved(git_dir, "refs/remotes/target", out) == 0 &&
             memcmp(out, c1, SG_SHA1_RAW_LEN) == 0,
         "direct target should resolve");
    /* s1..s4: 2..5 reads, all within the bound */
    CHECK(sg_ref_read_path_resolved(git_dir, "refs/remotes/s1", out) == 0 &&
             memcmp(out, c1, SG_SHA1_RAW_LEN) == 0,
         "s1 (2 reads) should resolve");
    CHECK(sg_ref_read_path_resolved(git_dir, "refs/remotes/s2", out) == 0 &&
             memcmp(out, c1, SG_SHA1_RAW_LEN) == 0,
         "s2 (3 reads) should resolve");
    CHECK(sg_ref_read_path_resolved(git_dir, "refs/remotes/s3", out) == 0 &&
             memcmp(out, c1, SG_SHA1_RAW_LEN) == 0,
         "s3 (4 reads) should resolve");
    CHECK(sg_ref_read_path_resolved(git_dir, "refs/remotes/s4", out) == 0 &&
             memcmp(out, c1, SG_SHA1_RAW_LEN) == 0,
         "s4 (5 reads, exactly at the bound) should resolve");
    /* s5: 6 reads, exceeds the bound -- must fail, matching git's
       "ignoring dangling symref" cutoff. */
    CHECK(sg_ref_read_path_resolved(git_dir, "refs/remotes/s5", out) != 0,
         "s5 (6 reads, past the bound) must fail");

    free(git_dir);
}

static void test_self_referencing_symref(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char out[SG_SHA1_RAW_LEN];

    CHECK(sg_ref_set_symref(git_dir, "refs/remotes/loop", "refs/remotes/loop", NULL) == 0,
         "failed to write a self-referencing symref");

    CHECK(sg_ref_read_path_resolved(git_dir, "refs/remotes/loop", out) != 0,
         "a symref pointing at itself must fail, not spin forever");

    free(git_dir);
}

/* ---- section 2.7: hostile spellings, loose and packed ---- */

static void test_hostile_spellings_refused(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN];
    char ref_path[SG_PATH_MAX];

    make_commit(git_dir, "c1", c1);
    write_branch(git_dir, "a/b", c1);

    CHECK(sg_rev_parse_ref_path(git_dir, "a//b", ref_path, sizeof(ref_path)) != 0,
         "'a//b' (empty component) must be refused");
    CHECK(sg_rev_parse_ref_path(git_dir, "a/./b", ref_path, sizeof(ref_path)) != 0,
         "'a/./b' (a '.' component) must be refused");
    CHECK(sg_rev_parse_ref_path(git_dir, "refs/heads/a//b", ref_path, sizeof(ref_path)) != 0,
         "'refs/heads/a//b' must be refused");
    CHECK(sg_rev_parse_ref_path(git_dir, "refs/heads/a/./b", ref_path, sizeof(ref_path)) != 0,
         "'refs/heads/a/./b' must be refused");
    CHECK(sg_rev_parse_ref_path(git_dir, "heads/../../EVIL", ref_path, sizeof(ref_path)) != 0,
         "a '..' component must be refused");
    CHECK(sg_rev_parse_ref_path(git_dir, "heads/master/", ref_path, sizeof(ref_path)) != 0,
         "a trailing '/' must be refused");
    CHECK(sg_rev_parse_ref_path(git_dir, "heads/", ref_path, sizeof(ref_path)) != 0,
         "'heads/' alone (empty final component) must be refused");
    CHECK(sg_rev_parse_ref_path(git_dir, "", ref_path, sizeof(ref_path)) != 0,
         "an empty name must be refused");
    CHECK(sg_rev_parse_ref_path(git_dir, "/heads/master", ref_path, sizeof(ref_path)) != 0,
         "a leading '/' must be refused");

    free(git_dir);
}

/* A legal '.' INSIDE a component (not a whole component) must keep
   resolving -- only a component that IS "." or ".." is rejected. */
static void test_legal_dots_still_resolve(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN];
    char ref_path[SG_PATH_MAX];

    make_commit(git_dir, "c1", c1);
    write_tag(git_dir, "v1.0", c1);

    CHECK(sg_rev_parse_ref_path(git_dir, "v1.0", ref_path, sizeof(ref_path)) == 0 &&
             strcmp(ref_path, "refs/tags/v1.0") == 0,
         "'v1.0' (a legal dotted name) should still resolve, got '%s'", ref_path);

    free(git_dir);
}

/* ---- truncation: a candidate that would not fit in `out` must count as
   "this rule missed", never probed, never silently cut. ---- */

static void test_truncation_is_treated_as_a_miss(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN];
    char tiny_out[6]; /* too small for "refs/heads/topic" */
    char big_out[SG_PATH_MAX];

    make_commit(git_dir, "c1", c1);
    write_branch(git_dir, "topic", c1);

    CHECK(sg_rev_parse_ref_path(git_dir, "topic", tiny_out, sizeof(tiny_out)) != 0,
         "an out buffer too small for the resolved path must fail, not truncate silently");
    CHECK(sg_rev_parse_ref_path(git_dir, "topic", big_out, sizeof(big_out)) == 0 &&
             strcmp(big_out, "refs/heads/topic") == 0,
         "the same lookup with a big enough buffer should succeed");

    free(git_dir);
}

int main(void)
{
    test_rule1_general_form();
    test_rule2_beats_rules_3_and_4();
    test_rule3_beats_rule4();
    test_rule4_isolation();
    test_rule5_isolation();
    test_rule6_symref_following();
    test_rev_parse_object_follows_symref_too();
    test_fallthrough_of_missing_refs_prefix();
    test_symref_chain_bound();
    test_self_referencing_symref();
    test_hostile_spellings_refused();
    test_legal_dots_still_resolve();
    test_truncation_is_treated_as_a_miss();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all revparse ref_path tests passed\n");
    return 0;
}
