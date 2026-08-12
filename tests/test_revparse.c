#include "sg/revparse.h"

#include "sg/loose.h"
#include "sg/object.h"
#include "sg/reflog.h"
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

/* A full 40-hex sha1 wins over EVERY ref, even a branch whose name happens
   to literally be that hex string pointing somewhere else -- the most
   counter-intuitive corner of the precedence order. Measured against real
   git: `git branch "$C1_hex" "$C2"` (a branch literally named after C1's
   own hex, but pointing at C2) followed by `git rev-parse "$C1_hex"`
   returns C1_hex itself, not C2. */
static void test_precedence_hex_over_branch_with_colliding_name(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN], c2[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];
    char c1_hex[SG_SHA1_HEX_LEN + 1];

    make_commit(git_dir, "c1", NULL, 0, c1);
    make_commit(git_dir, "c2", NULL, 0, c2);
    sg_sha1_to_hex(c1, c1_hex);

    /* A branch literally named after c1's own hex, pointing at c2. */
    write_branch(git_dir, c1_hex, c2);

    CHECK(sg_rev_parse_commit(git_dir, c1_hex, out) == 0, "hex-named base should still resolve");
    CHECK(memcmp(out, c1, SG_SHA1_RAW_LEN) == 0,
         "the literal hex must win over a same-named branch pointing elsewhere");

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

/* Leading zeros in a ~/^ suffix are legal, not a length violation -- the
   9-significant-digit overflow guard applies to the number's value, not
   its raw character count. Measured against real git: `git rev-parse
   HEAD~0000000001` succeeds and means the same as `HEAD~1` (10 raw
   characters, only 1 significant digit). */
static void test_leading_zeros_in_suffix(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN], c2[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "c1", NULL, 0, c1);
    make_commit(git_dir, "c2", (const unsigned char (*)[SG_SHA1_RAW_LEN])c1, 1, c2);
    set_master(git_dir, c2);

    CHECK(sg_rev_parse_commit(git_dir, "HEAD~0000000001", out) == 0,
         "a 10-char, 1-significant-digit suffix should resolve");
    CHECK(memcmp(out, c1, SG_SHA1_RAW_LEN) == 0, "HEAD~0000000001 should equal HEAD~1");

    free(git_dir);
}

/* ~0 and ^0 both mean "this commit itself" -- measured against real git,
   `git rev-parse HEAD^0` and `git rev-parse HEAD~0` both print HEAD's own
   id. They must also compose with further suffixes (e.g. HEAD~1^0). */
static void test_zero_suffix_is_identity(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN], c2[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "c1", NULL, 0, c1);
    make_commit(git_dir, "c2", (const unsigned char (*)[SG_SHA1_RAW_LEN])c1, 1, c2);
    set_master(git_dir, c2);

    CHECK(sg_rev_parse_commit(git_dir, "HEAD^0", out) == 0, "HEAD^0 should resolve");
    CHECK(memcmp(out, c2, SG_SHA1_RAW_LEN) == 0, "HEAD^0 should be HEAD itself");

    CHECK(sg_rev_parse_commit(git_dir, "HEAD~0", out) == 0, "HEAD~0 should resolve");
    CHECK(memcmp(out, c2, SG_SHA1_RAW_LEN) == 0, "HEAD~0 should be HEAD itself");

    CHECK(sg_rev_parse_commit(git_dir, "HEAD~1^0", out) == 0, "HEAD~1^0 should resolve");
    CHECK(memcmp(out, c1, SG_SHA1_RAW_LEN) == 0, "HEAD~1^0 should be c1 (^0 is a no-op)");

    free(git_dir);
}

/* An oversized rev string (bigger than resolve_base's fixed-size `base`
   buffer) must fail cleanly, not overrun the stack buffer that holds the
   base name -- this is exactly the check ASan is best positioned to catch,
   so it's worth having even though the assertion itself is trivial. */
static void test_oversized_rev_string_fails(void)
{
    char *git_dir = make_tmp_repo();
    char huge[4200];
    unsigned char out[SG_SHA1_RAW_LEN];

    memset(huge, 'a', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';

    CHECK(sg_rev_parse_commit(git_dir, huge, out) != 0,
         "a rev string longer than the internal base buffer must fail cleanly");

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

/* ---- @{N} reflog notation --------------------------------------------- */

/* The core semantic to get right: @{N} names entry N's NEW_id, not its
   old_id -- easy to get backwards (@{0} sounds like "the value right
   before this ref last moved", but it is in fact "the value the ref was
   moved TO"). alpha/beta are two ordinary, independently-hashed commits so
   a get-it-backwards mutation returns a DIFFERENT (wrong) answer instead of
   accidentally matching. */
static void test_at_notation_uses_new_id(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char alpha[SG_SHA1_RAW_LEN], beta[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "alpha", NULL, 0, alpha);
    make_commit(git_dir, "beta", NULL, 0, beta);
    write_branch(git_dir, "solo", beta);
    CHECK(sg_reflog_append(git_dir, "refs/heads/solo", alpha, beta, "move", NULL) == 0,
         "reflog append failed");

    CHECK(sg_rev_parse_commit(git_dir, "solo@{0}", out) == 0, "solo@{0} should resolve");
    CHECK(memcmp(out, beta, SG_SHA1_RAW_LEN) == 0,
         "solo@{0} must be the entry's NEW id (beta), not its old id (alpha)");

    free(git_dir);
}

/* Builds a branch "topic" with three logged moves (root -> c1 -> c2 -> c3)
   so @{0}/@{1}/@{2} are three genuinely different, order-sensitive answers,
   and exercises the grammar table from the phase spec against it. */
static void test_at_notation_grammar(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char root[SG_SHA1_RAW_LEN], c1[SG_SHA1_RAW_LEN], c2[SG_SHA1_RAW_LEN], c3[SG_SHA1_RAW_LEN];
    unsigned char zero[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    memset(zero, 0, SG_SHA1_RAW_LEN);
    make_commit(git_dir, "root", NULL, 0, root);
    make_commit(git_dir, "c1", (const unsigned char (*)[SG_SHA1_RAW_LEN])root, 1, c1);
    make_commit(git_dir, "c2", (const unsigned char (*)[SG_SHA1_RAW_LEN])c1, 1, c2);
    make_commit(git_dir, "c3", (const unsigned char (*)[SG_SHA1_RAW_LEN])c2, 1, c3);
    write_branch(git_dir, "topic", c3);
    CHECK(sg_reflog_append(git_dir, "refs/heads/topic", zero, root, "e0", NULL) == 0, "append e0 failed");
    CHECK(sg_reflog_append(git_dir, "refs/heads/topic", root, c1, "e1", NULL) == 0, "append e1 failed");
    CHECK(sg_reflog_append(git_dir, "refs/heads/topic", c1, c2, "e2", NULL) == 0, "append e2 failed");
    CHECK(sg_reflog_append(git_dir, "refs/heads/topic", c2, c3, "e3", NULL) == 0, "append e3 failed");

    /* @{0} newest .. @{3} oldest */
    CHECK(sg_rev_parse_commit(git_dir, "topic@{0}", out) == 0 && memcmp(out, c3, SG_SHA1_RAW_LEN) == 0,
         "topic@{0} should be c3");
    CHECK(sg_rev_parse_commit(git_dir, "topic@{1}", out) == 0 && memcmp(out, c2, SG_SHA1_RAW_LEN) == 0,
         "topic@{1} should be c2");
    CHECK(sg_rev_parse_commit(git_dir, "topic@{2}", out) == 0 && memcmp(out, c1, SG_SHA1_RAW_LEN) == 0,
         "topic@{2} should be c1");
    CHECK(sg_rev_parse_commit(git_dir, "topic@{3}", out) == 0 && memcmp(out, root, SG_SHA1_RAW_LEN) == 0,
         "topic@{3} should be root");

    /* "@{N}" composes with a trailing ~/^ chain: topic@{1}~1 -> c2's parent -> c1 */
    CHECK(sg_rev_parse_commit(git_dir, "topic@{1}~1", out) == 0 && memcmp(out, c1, SG_SHA1_RAW_LEN) == 0,
         "topic@{1}~1 should be c1");

    /* leading zeros: "@{01}" == "@{1}" */
    CHECK(sg_rev_parse_commit(git_dir, "topic@{01}", out) == 0 && memcmp(out, c2, SG_SHA1_RAW_LEN) == 0,
         "topic@{01} should equal topic@{1}");

    /* out of range */
    CHECK(sg_rev_parse_commit(git_dir, "topic@{4}", out) != 0, "topic@{4} is out of range, must fail");

    /* "@{N}" must be adjacent to the base -- a "~"/"^" suffix already
       consumed BEFORE the "@{N}" makes it unparsable, not reinterpreted. */
    CHECK(sg_rev_parse_commit(git_dir, "topic~1@{1}", out) != 0, "topic~1@{1} must be rejected");
    CHECK(sg_rev_parse_commit(git_dir, "topic^@{1}", out) != 0, "topic^@{1} must be rejected");

    /* malformed / unsupported brace contents */
    CHECK(sg_rev_parse_commit(git_dir, "topic@{}", out) != 0, "topic@{} must be rejected");
    CHECK(sg_rev_parse_commit(git_dir, "topic@{-1}", out) != 0, "topic@{-1} must be rejected");
    CHECK(sg_rev_parse_commit(git_dir, "topic@{u}", out) != 0,
         "topic@{u} (upstream selector) is unsupported and must be rejected");
    CHECK(sg_rev_parse_commit(git_dir, "topic@{now}", out) != 0,
         "topic@{now} (date selector) is unsupported and must be rejected");

    /* a bare "@{N}"/"@" with no base is deliberately unsupported (real git's
       meaning -- current branch / HEAD -- is measurably NOT the same value
       as spelling the ref out, so guessing would be a wrong answer, not a
       missing feature). */
    CHECK(sg_rev_parse_commit(git_dir, "@{0}", out) != 0, "a bare @{0} with no base must be rejected");
    CHECK(sg_rev_parse_commit(git_dir, "@", out) != 0, "a bare @ with no base must be rejected");

    free(git_dir);
}

/* A tag has no reflog at all -- an absent logs/refs/tags/<name> file reads
   back as zero entries (sg_reflog_read's documented "missing file" case),
   which is out of range for any N, so "<tag>@{0}" fails the same way
   "topic@{4}" (out of range) does, without needing a special "is this a
   tag" check anywhere in the @{N} code path. */
static void test_at_notation_tag_has_no_reflog(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "c1", NULL, 0, c1);
    write_lightweight_tag(git_dir, "vtag", c1);

    CHECK(sg_rev_parse_commit(git_dir, "vtag@{0}", out) != 0, "a tag's @{0} must fail: tags have no reflog");

    free(git_dir);
}

/* HEAD@{N} and <branch>@{N} read different files (logs/HEAD vs.
   logs/refs/heads/<branch>) and can legitimately disagree at the same
   moment -- proven here by giving them independently-built, disjoint
   histories of entries. */
static void test_at_notation_head_and_branch_logs_differ(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN], c2[SG_SHA1_RAW_LEN];
    unsigned char zero[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    memset(zero, 0, SG_SHA1_RAW_LEN);
    make_commit(git_dir, "c1", NULL, 0, c1);
    make_commit(git_dir, "c2", (const unsigned char (*)[SG_SHA1_RAW_LEN])c1, 1, c2);
    set_master(git_dir, c2);

    /* logs/HEAD gets one entry (-> c1); logs/refs/heads/master gets a
       DIFFERENT one (-> c2), so "HEAD@{0}" and "master@{0}" must disagree. */
    CHECK(sg_reflog_append(git_dir, "HEAD", zero, c1, "head move", NULL) == 0, "HEAD append failed");
    CHECK(sg_reflog_append(git_dir, "refs/heads/master", zero, c2, "branch move", NULL) == 0,
         "branch append failed");

    CHECK(sg_rev_parse_commit(git_dir, "HEAD@{0}", out) == 0 && memcmp(out, c1, SG_SHA1_RAW_LEN) == 0,
         "HEAD@{0} should read logs/HEAD (c1)");
    CHECK(sg_rev_parse_commit(git_dir, "master@{0}", out) == 0 && memcmp(out, c2, SG_SHA1_RAW_LEN) == 0,
         "master@{0} should read logs/refs/heads/master (c2), not logs/HEAD");

    free(git_dir);
}

/* A digit-only check that's merely "reachable" is not the same as one that
   actually matters: with a SHORT reflog, a loosened "any non-empty content"
   check would still get rejected by the out-of-range test below it purely
   by coincidence (a non-digit byte like 'u' or '-' fed straight through
   would produce a huge/negative-wrapped value, which then fails the
   idx < log.count bound anyway -- masking a missing whitelist as a passing
   test). This test defeats that coincidence on purpose: it builds a branch
   with enough reflog entries that "@{A}" (a single non-digit byte whose
   (byte - '0') arithmetic happens to equal 17, a genuinely in-range index
   for a 20-entry log) would resolve successfully if the digit-only check
   were ever loosened to "just non-empty".

   Measured directly (by removing the check and rerunning): the digit
   whitelist actually enforced here lives in parse_suffix_number's own
   per-character scan (revparse.c, shared by the "~"/"^" suffix parser too),
   NOT in a separate loop local to the "@{N}" branch -- an earlier version of
   this comment claimed the latter, which is wrong. Removing
   parse_suffix_number's digit check is what turns this test red; nothing
   else in the "@{N}" code path duplicates that check. */
static void test_at_notation_rejects_non_digit_even_when_in_range(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c[SG_SHA1_RAW_LEN];
    unsigned char zero[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];
    size_t i;

    memset(zero, 0, SG_SHA1_RAW_LEN);
    make_commit(git_dir, "c", NULL, 0, c);
    write_branch(git_dir, "wideranger", c);
    for (i = 0; i < 20; i++)
        CHECK(sg_reflog_append(git_dir, "refs/heads/wideranger", zero, c, "e", NULL) == 0,
             "reflog append %zu failed", i);

    CHECK(sg_rev_parse_commit(git_dir, "wideranger@{A}", out) != 0,
         "a non-digit brace content ('A') must be rejected even though (byte - '0') would land at a "
         "genuinely in-range index (17 of 20 entries) if the digit-only check were ever loosened to "
         "just \"non-empty\"");

    free(git_dir);
}

/* Bug caught in cold-read review: the ~/^ suffix loop (revparse.c, right
   after "@{N}" parsing) only special-cased '~' and treated any OTHER
   character reaching it as '^' without ever checking that it actually was
   '^'. Before this was fixed, a single garbage character immediately after
   "@{N}" -- with nothing after it, or a run of digits after it -- fell
   through into that unchecked branch and got silently parsed as an implicit
   "^1", so e.g. "master@{0}x" resolved to master@{0}'s PARENT instead of
   being rejected. Measured against real git: `git rev-parse
   'master@{0}x'` is a fatal "ambiguous argument" error. Each of these must
   be rejected outright, not silently reinterpreted. */
static void test_at_notation_trailing_garbage_rejected(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char root[SG_SHA1_RAW_LEN], c1[SG_SHA1_RAW_LEN], c2[SG_SHA1_RAW_LEN];
    unsigned char zero[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    /* A THREE-commit chain, not two -- with only two commits, "master@{1}"
       lands on the root commit, which has no parent, so a buggy
       "master@{1}5" -> implicit "^1" misparse would (coincidentally) still
       get rejected, just for the unrelated reason "root has no parent",
       masking the missing operator-character check as a passing test. With
       three commits, master@{1} == c1, which DOES have a parent (root), so
       the buggy misparse resolves successfully (to root) instead of
       failing -- a genuine, non-coincidental red before the fix. */
    memset(zero, 0, SG_SHA1_RAW_LEN);
    make_commit(git_dir, "root", NULL, 0, root);
    make_commit(git_dir, "c1", (const unsigned char (*)[SG_SHA1_RAW_LEN])root, 1, c1);
    make_commit(git_dir, "c2", (const unsigned char (*)[SG_SHA1_RAW_LEN])c1, 1, c2);
    set_master(git_dir, c2);
    CHECK(sg_reflog_append(git_dir, "refs/heads/master", zero, root, "e0", NULL) == 0,
         "append e0 failed");
    CHECK(sg_reflog_append(git_dir, "refs/heads/master", root, c1, "e1", NULL) == 0,
         "append e1 failed");
    CHECK(sg_reflog_append(git_dir, "refs/heads/master", c1, c2, "e2", NULL) == 0,
         "append e2 failed");

    /* master@{0} == c2, which has a parent (c1) -- so a buggy misparse of
       "@{0}x"/"@{0}0" as an implicit "^1" would succeed (wrongly) rather
       than fail for an unrelated reason. */
    CHECK(sg_rev_parse_commit(git_dir, "master@{0}x", out) != 0,
         "master@{0}x: a lone non-~/^ garbage byte right after @{N} must be rejected, not "
         "silently read as an implicit ^1");
    CHECK(sg_rev_parse_commit(git_dir, "master@{0}0", out) != 0,
         "master@{0}0: garbage byte followed by a single digit must be rejected, not read as ^0");
    /* master@{1} == c1, which has a parent (root) -- see the chain-length
       comment above this function. */
    CHECK(sg_rev_parse_commit(git_dir, "master@{1}5", out) != 0,
         "master@{1}5: garbage byte followed by digits must be rejected, not read as ^5");

    /* These were already correctly rejected before the fix (the base scan
       stops at '~'/'^', so "@{N}" ends up dangling after a suffix and fails
       parse_suffix_number) -- re-asserted here so the fix above doesn't
       accidentally start ACCEPTING them. */
    CHECK(sg_rev_parse_commit(git_dir, "master~1@{1}", out) != 0,
         "master~1@{1} must still be rejected after the fix");
    CHECK(sg_rev_parse_commit(git_dir, "master^@{1}", out) != 0,
         "master^@{1} must still be rejected after the fix");

    free(git_dir);
}

/* "refs/<rest>" is accepted as a <base> directly, used as-is provided the
   path actually exists (sg_rev_parse_ref_path, revparse.c). Real git
   accepts this too (gitrevisions' "refs/<name>" disambiguation rule), and
   it must not disturb the existing precedence (40-hex still wins, tag
   still beats a same-named branch looked up by bare name). */
static void test_refs_prefix_passthrough(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN], c2[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "c1", NULL, 0, c1);
    make_commit(git_dir, "c2", NULL, 0, c2);
    write_branch(git_dir, "topic", c1);
    write_lightweight_tag(git_dir, "vtag", c2);

    CHECK(sg_rev_parse_commit(git_dir, "refs/heads/topic", out) == 0,
         "refs/heads/topic should resolve");
    CHECK(memcmp(out, c1, SG_SHA1_RAW_LEN) == 0, "refs/heads/topic should be c1");

    CHECK(sg_rev_parse_commit(git_dir, "refs/tags/vtag", out) == 0,
         "refs/tags/vtag should resolve");
    CHECK(memcmp(out, c2, SG_SHA1_RAW_LEN) == 0, "refs/tags/vtag should be c2");

    /* a "refs/..." path that doesn't exist on disk must fail, not be
       silently accepted just because it has the right shape. */
    CHECK(sg_rev_parse_commit(git_dir, "refs/heads/does-not-exist", out) != 0,
         "a nonexistent refs/... path must be rejected");

    /* "~"/"^" suffixes still apply on top of a "refs/..." base. */
    CHECK(sg_rev_parse_commit(git_dir, "refs/heads/topic~0", out) == 0 &&
         memcmp(out, c1, SG_SHA1_RAW_LEN) == 0,
         "refs/heads/topic~0 should resolve to c1 (the ~0 identity suffix)");

    free(git_dir);
}

int main(void)
{
    test_head_and_branch_and_hex();
    test_precedence_tag_over_branch();
    test_precedence_hex_over_branch_with_colliding_name();
    test_tilde_and_caret_chains();
    test_leading_zeros_in_suffix();
    test_zero_suffix_is_identity();
    test_oversized_rev_string_fails();
    test_annotated_tag_peel();
    test_broken_tag_chain_fails();
    test_forged_tag_cycle_fails();
    test_root_commit_tilde_fails();
    test_missing_second_parent_fails();
    test_tree_and_blob_targets_fail();
    test_malformed_and_unknown_revs_fail();
    test_at_notation_uses_new_id();
    test_at_notation_grammar();
    test_at_notation_tag_has_no_reflog();
    test_at_notation_head_and_branch_logs_differ();
    test_at_notation_rejects_non_digit_even_when_in_range();
    test_at_notation_trailing_garbage_rejected();
    test_refs_prefix_passthrough();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all revparse tests passed\n");
    return 0;
}
