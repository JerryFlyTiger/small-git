#include "sg/revparse.h"

#include "sg/loose.h"
#include "sg/object.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"
#include "sg/zutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
static long long time_seq = 2000000;

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
    static char template[] = "/tmp/sg_revparse_test_XXXXXX";
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

/* Same shape as test_merge_base.c's make_commit: each commit gets its own
   single-blob tree so distinct commits never share a tree id; the content
   itself is irrelevant here, which only exercises parent-chain walking. */
static void make_commit(const char *git_dir, const char *message,
                        const unsigned char (*parents)[SG_SHA1_RAW_LEN], size_t parent_count,
                        unsigned char commit_id_out[SG_SHA1_RAW_LEN])
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
    if (parent_count > 0) {
        commit.parents = malloc(parent_count * sizeof(*commit.parents));
        CHECK(commit.parents != NULL, "oom");
        memcpy(commit.parents, parents, parent_count * SG_SHA1_RAW_LEN);
        commit.parent_count = parent_count;
    }
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
    free(commit.parents);
    CHECK(sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len, commit_id_out) == 0,
         "commit write failed for '%s'", message);
    free(serialized);
}

static void set_master(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN])
{
    CHECK(sg_ref_update_branch(git_dir, "master", id) == 0, "failed to update master");
}

static void write_branch(const char *git_dir, const char *name, const unsigned char id[SG_SHA1_RAW_LEN])
{
    CHECK(sg_ref_update_branch(git_dir, name, id) == 0, "failed to write branch '%s'", name);
}

static void write_lightweight_tag(const char *git_dir, const char *name,
                                  const unsigned char id[SG_SHA1_RAW_LEN])
{
    char ref_path[256];

    snprintf(ref_path, sizeof(ref_path), "refs/tags/%s", name);
    CHECK(sg_ref_write_path(git_dir, ref_path, id) == 0, "failed to write tag '%s'", name);
}

/* Writes an annotated tag object pointing at target/target_type, then points
   refs/tags/<name> at the new tag object. Returns the tag object's own id
   via tag_id_out (needed by tests that peel through more than one hop). */
static void write_annotated_tag(const char *git_dir, const char *name,
                                const unsigned char target[SG_SHA1_RAW_LEN], sg_obj_type target_type,
                                unsigned char tag_id_out[SG_SHA1_RAW_LEN])
{
    sg_tag tag;
    unsigned char *serialized;
    size_t serialized_len;
    char ref_path[256];

    memset(&tag, 0, sizeof(tag));
    memcpy(tag.object, target, SG_SHA1_RAW_LEN);
    tag.object_type = target_type;
    tag.tag_name = (char *)name;
    tag.tagger_name = (char *)"tester";
    tag.tagger_email = (char *)"tester@example.com";
    tag.tagger_time = time_seq++;
    strcpy(tag.tagger_tz, "+0000");
    tag.message = (char *)"annotated\n";

    CHECK(sg_tag_serialize(&tag, &serialized, &serialized_len) == 0, "tag serialize failed for '%s'",
         name);
    CHECK(sg_loose_write(git_dir, SG_OBJ_TAG, serialized, serialized_len, tag_id_out) == 0,
         "tag write failed for '%s'", name);
    free(serialized);

    snprintf(ref_path, sizeof(ref_path), "refs/tags/%s", name);
    CHECK(sg_ref_write_path(git_dir, ref_path, tag_id_out) == 0, "failed to write tag ref '%s'", name);
}

/* Writes content, compressed and header-wrapped exactly like sg_loose_write
   does, straight to the loose object path for `id` -- WITHOUT going through
   sg_object_hash, so `id` need not be (and for the forged-cycle test,
   deliberately isn't) the real hash of content. This is only reachable by
   directly poking bytes onto disk; there's no such call in the library
   itself, which is the point -- it simulates a corrupted or maliciously
   crafted .git that sg_object_read must not crash or loop on. */
static void write_loose_object_raw(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                                   sg_obj_type type, const void *content, size_t content_len)
{
    char hex[SG_SHA1_HEX_LEN + 1];
    char path[512];
    unsigned char *formatted;
    size_t formatted_len;
    unsigned char *compressed;
    size_t compressed_len;

    sg_sha1_to_hex(id, hex);
    snprintf(path, sizeof(path), "%s/objects/%.2s/%s", git_dir, hex, hex + 2);

    CHECK(sg_object_format(type, content, content_len, &formatted, &formatted_len) == 0,
         "object_format failed for forged object %s", hex);
    CHECK(sg_compress(formatted, formatted_len, &compressed, &compressed_len) == 0,
         "compress failed for forged object %s", hex);
    free(formatted);
    CHECK(sg_write_file_mkdirs(path, compressed, compressed_len, 0444) == 0,
         "write_file_mkdirs failed for forged object %s", hex);
    free(compressed);
}

/* Forges a tag object -- via write_loose_object_raw, so its filename need
   not match its content's real hash -- claiming "object <target>", stored
   at the loose-object path for `id` (which callers pick, e.g. all-0x11
   bytes, independent of what the content actually hashes to). Two calls
   with id/target swapped produce a genuine tag->tag cycle on disk. */
static void write_forged_tag(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                             const unsigned char target[SG_SHA1_RAW_LEN], const char *tag_name)
{
    sg_tag tag;
    unsigned char *content;
    size_t content_len;

    memset(&tag, 0, sizeof(tag));
    memcpy(tag.object, target, SG_SHA1_RAW_LEN);
    tag.object_type = SG_OBJ_TAG;
    tag.tag_name = (char *)tag_name;
    tag.tagger_name = (char *)"tester";
    tag.tagger_email = (char *)"tester@example.com";
    tag.tagger_time = time_seq++;
    strcpy(tag.tagger_tz, "+0000");
    tag.message = (char *)"forged\n";

    CHECK(sg_tag_serialize(&tag, &content, &content_len) == 0, "forged tag serialize failed for '%s'",
         tag_name);
    write_loose_object_raw(git_dir, id, SG_OBJ_TAG, content, content_len);
    free(content);
}

static void test_head_and_branch_and_hex(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];

    make_commit(git_dir, "c1", NULL, 0, c1);
    set_master(git_dir, c1);

    CHECK(sg_rev_parse_commit(git_dir, "HEAD", out) == 0, "HEAD should resolve");
    CHECK(memcmp(out, c1, SG_SHA1_RAW_LEN) == 0, "HEAD should resolve to c1");

    CHECK(sg_rev_parse_commit(git_dir, "master", out) == 0, "branch name should resolve");
    CHECK(memcmp(out, c1, SG_SHA1_RAW_LEN) == 0, "master should resolve to c1");

    sg_sha1_to_hex(c1, hex);
    CHECK(sg_rev_parse_commit(git_dir, hex, out) == 0, "full hex sha should resolve");
    CHECK(memcmp(out, c1, SG_SHA1_RAW_LEN) == 0, "hex sha should resolve to c1");

    free(git_dir);
}

/* HEAD -> tag -> branch -> hex is git's own gitrevisions precedence
   (refs/<name> -> refs/tags/<name> -> refs/heads/<name> -> ...): a branch
   and a tag sharing a name must both be reachable via their fully-qualified
   path, but the bare name hits the TAG first (measured against real git:
   `git rev-parse foo` with both a branch and a tag named "foo" resolves to
   the tag's target, with a "refname is ambiguous" warning). */
static void test_precedence_tag_over_branch(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char branch_tip[SG_SHA1_RAW_LEN];
    unsigned char tag_tip[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "branch-tip", NULL, 0, branch_tip);
    make_commit(git_dir, "tag-tip", NULL, 0, tag_tip);
    write_branch(git_dir, "dup", branch_tip);
    write_lightweight_tag(git_dir, "dup", tag_tip);

    CHECK(sg_rev_parse_commit(git_dir, "dup", out) == 0, "ambiguous name should still resolve");
    CHECK(memcmp(out, tag_tip, SG_SHA1_RAW_LEN) == 0,
         "tag must win over a same-named branch");

    free(git_dir);
}

static void test_tilde_and_caret_chains(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char root[SG_SHA1_RAW_LEN], mid[SG_SHA1_RAW_LEN];
    unsigned char left[SG_SHA1_RAW_LEN], right[SG_SHA1_RAW_LEN], tip[SG_SHA1_RAW_LEN];
    unsigned char merge_parents[2][SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "root", NULL, 0, root);
    make_commit(git_dir, "mid", (const unsigned char (*)[SG_SHA1_RAW_LEN])root, 1, mid);
    make_commit(git_dir, "left", (const unsigned char (*)[SG_SHA1_RAW_LEN])mid, 1, left);
    make_commit(git_dir, "right", (const unsigned char (*)[SG_SHA1_RAW_LEN])mid, 1, right);
    memcpy(merge_parents[0], left, SG_SHA1_RAW_LEN);
    memcpy(merge_parents[1], right, SG_SHA1_RAW_LEN);
    make_commit(git_dir, "tip", merge_parents, 2, tip);
    set_master(git_dir, tip);

    /* tip~1 == first parent == left */
    CHECK(sg_rev_parse_commit(git_dir, "HEAD~1", out) == 0, "HEAD~1 should resolve");
    CHECK(memcmp(out, left, SG_SHA1_RAW_LEN) == 0, "HEAD~1 should be left");

    /* bare ~ == ~1 */
    CHECK(sg_rev_parse_commit(git_dir, "HEAD~", out) == 0, "HEAD~ should resolve");
    CHECK(memcmp(out, left, SG_SHA1_RAW_LEN) == 0, "HEAD~ should be left");

    /* tip~2 == left's first (only) parent == mid */
    CHECK(sg_rev_parse_commit(git_dir, "HEAD~2", out) == 0, "HEAD~2 should resolve");
    CHECK(memcmp(out, mid, SG_SHA1_RAW_LEN) == 0, "HEAD~2 should be mid");

    /* tip^2 == second parent == right */
    CHECK(sg_rev_parse_commit(git_dir, "HEAD^2", out) == 0, "HEAD^2 should resolve");
    CHECK(memcmp(out, right, SG_SHA1_RAW_LEN) == 0, "HEAD^2 should be right");

    /* bare ^ == ^1 == first parent == left */
    CHECK(sg_rev_parse_commit(git_dir, "HEAD^", out) == 0, "HEAD^ should resolve");
    CHECK(memcmp(out, left, SG_SHA1_RAW_LEN) == 0, "HEAD^ should be left");

    /* chained: tip^2~1 -> right's first parent -> mid */
    CHECK(sg_rev_parse_commit(git_dir, "HEAD^2~1", out) == 0, "HEAD^2~1 should resolve");
    CHECK(memcmp(out, mid, SG_SHA1_RAW_LEN) == 0, "HEAD^2~1 should be mid");

    /* chained further: mid~1 -> root */
    CHECK(sg_rev_parse_commit(git_dir, "master^2~1~1", out) == 0, "master^2~1~1 should resolve");
    CHECK(memcmp(out, root, SG_SHA1_RAW_LEN) == 0, "master^2~1~1 should be root");

    free(git_dir);
}

static void test_annotated_tag_peel(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN];
    unsigned char tag1[SG_SHA1_RAW_LEN], tag2[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "c1", NULL, 0, c1);

    /* one hop: tag -> commit */
    write_annotated_tag(git_dir, "v1", c1, SG_OBJ_COMMIT, tag1);
    CHECK(sg_rev_parse_commit(git_dir, "v1", out) == 0, "annotated tag should peel to commit");
    CHECK(memcmp(out, c1, SG_SHA1_RAW_LEN) == 0, "v1 should peel to c1");

    /* two hops: tag-of-tag -> tag -> commit */
    write_annotated_tag(git_dir, "v2", tag1, SG_OBJ_TAG, tag2);
    CHECK(sg_rev_parse_commit(git_dir, "v2", out) == 0, "tag-of-tag should peel through both hops");
    CHECK(memcmp(out, c1, SG_SHA1_RAW_LEN) == 0, "v2 should peel all the way to c1");

    free(git_dir);
}

/* A tag object is content-addressed, so a tag genuinely can't name its own
   (not-yet-known) id, and a chain built purely out of sg_tag_serialize +
   sg_loose_write (which always computes the REAL hash of what it writes)
   can't loop back on itself either -- each new tag's object field can only
   point at ids that already exist, so the chain necessarily bottoms out at
   whatever non-tag (or nonexistent) id the first tag names. This test
   builds exactly such a chain, two tags deep, where the second tag's
   `object` field names an id nothing was ever written at: peeling must
   fail cleanly (unreadable object), not succeed or crash. A genuine tag
   CYCLE -- which requires two loose objects whose on-disk filename doesn't
   match their real content hash, since sg_loose_read/sg_object_read never
   verify that (neither does real git) -- is exercised separately by
   test_forged_tag_cycle_fails below. */
static void test_broken_tag_chain_fails(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char dangling_id[SG_SHA1_RAW_LEN];
    unsigned char tag1_id[SG_SHA1_RAW_LEN], tag2_id[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    /* An id nothing was ever written at -- sg_object_read on it fails. */
    memset(dangling_id, 0x33, sizeof(dangling_id));

    write_annotated_tag(git_dir, "t1", dangling_id, SG_OBJ_COMMIT, tag1_id);
    write_annotated_tag(git_dir, "t2", tag1_id, SG_OBJ_TAG, tag2_id);

    CHECK(sg_rev_parse_commit(git_dir, "t2", out) != 0,
         "a tag chain ending in a dangling object must fail, not crash");

    free(git_dir);
}

/* sg_loose_write is content-addressed -- the filename it picks is always
   the real hash of what's written underneath, so nothing built through it
   can ever reference itself or form a cycle (see the comment above
   test_broken_tag_chain_fails). But sg_loose_read/sg_object_read never
   check that a loose object's on-disk filename matches the hash of its
   decompressed content -- neither does real git -- so a corrupted or
   maliciously crafted .git can have two loose objects whose filenames are
   swapped relative to their content. This forges exactly that: X's file
   holds a tag claiming "object Y", Y's file holds a tag claiming "object
   X", producing a genuine X -> Y -> X -> ... cycle that peel_to_non_tag's
   hop limit (SG_REVPARSE_MAX_TAG_HOPS) must cut off. */
static void test_forged_tag_cycle_fails(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char x[SG_SHA1_RAW_LEN], y[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    memset(x, 0x11, sizeof(x));
    memset(y, 0x22, sizeof(y));

    write_forged_tag(git_dir, x, y, "cyc-x");
    write_forged_tag(git_dir, y, x, "cyc-y");

    CHECK(sg_ref_write_path(git_dir, "refs/tags/cyc", x) == 0, "failed to write tag ref");

    CHECK(sg_rev_parse_commit(git_dir, "cyc", out) != 0,
         "a genuine tag->tag->tag... cycle must fail, not loop forever");

    free(git_dir);
}

static void test_root_commit_tilde_fails(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char root[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "root", NULL, 0, root);
    set_master(git_dir, root);

    CHECK(sg_rev_parse_commit(git_dir, "HEAD~1", out) != 0,
         "root commit has no parent, ~1 must fail");
    CHECK(sg_rev_parse_commit(git_dir, "HEAD^1", out) != 0,
         "root commit has no parent, ^1 must fail");

    free(git_dir);
}

static void test_missing_second_parent_fails(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN], c2[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "c1", NULL, 0, c1);
    make_commit(git_dir, "c2", (const unsigned char (*)[SG_SHA1_RAW_LEN])c1, 1, c2);
    set_master(git_dir, c2);

    CHECK(sg_rev_parse_commit(git_dir, "HEAD^2", out) != 0,
         "only one parent exists, ^2 must fail");

    free(git_dir);
}

static void test_tree_and_blob_targets_fail(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char blob_id[SG_SHA1_RAW_LEN];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    unsigned char tag_id[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];
    sg_flat_entry entry;
    char blob_hex[SG_SHA1_HEX_LEN + 1];
    char tree_hex[SG_SHA1_HEX_LEN + 1];

    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "hello\n", 6, blob_id) == 0, "blob write failed");
    entry.path = (char *)"file.txt";
    entry.mode = 0100644;
    memcpy(entry.sha1, blob_id, SG_SHA1_RAW_LEN);
    CHECK(sg_tree_build(git_dir, &entry, 1, tree_id) == 0, "tree build failed");

    sg_sha1_to_hex(blob_id, blob_hex);
    sg_sha1_to_hex(tree_id, tree_hex);

    CHECK(sg_rev_parse_commit(git_dir, blob_hex, out) != 0, "a blob id must not resolve as a commit");
    CHECK(sg_rev_parse_commit(git_dir, tree_hex, out) != 0, "a tree id must not resolve as a commit");

    /* Same check through an annotated tag that peels straight to a blob. */
    write_annotated_tag(git_dir, "blob-tag", blob_id, SG_OBJ_BLOB, tag_id);
    CHECK(sg_rev_parse_commit(git_dir, "blob-tag", out) != 0,
         "an annotated tag peeling to a blob must not resolve as a commit");

    free(git_dir);
}

static void test_malformed_and_unknown_revs_fail(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "c1", NULL, 0, c1);
    set_master(git_dir, c1);

    CHECK(sg_rev_parse_commit(git_dir, "", out) != 0, "empty rev must fail");
    CHECK(sg_rev_parse_commit(git_dir, "nonexistent-branch", out) != 0,
         "unknown branch/tag/hex name must fail");
    CHECK(sg_rev_parse_commit(git_dir, "deadbeef", out) != 0,
         "a too-short hex string must fail (abbreviations unsupported)");
    CHECK(sg_rev_parse_commit(git_dir, "HEAD~abc", out) != 0, "non-numeric ~ suffix must fail");
    CHECK(sg_rev_parse_commit(git_dir, "HEAD^-1", out) != 0, "negative ^ suffix must fail");
    CHECK(sg_rev_parse_commit(git_dir, "~1", out) != 0, "a rev with no base must fail");

    free(git_dir);
}

int main(void)
{
    test_head_and_branch_and_hex();
    test_precedence_tag_over_branch();
    test_tilde_and_caret_chains();
    test_annotated_tag_peel();
    test_broken_tag_chain_fails();
    test_forged_tag_cycle_fails();
    test_root_commit_tilde_fails();
    test_missing_second_parent_fails();
    test_tree_and_blob_targets_fail();
    test_malformed_and_unknown_revs_fail();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all revparse tests passed\n");
    return 0;
}
