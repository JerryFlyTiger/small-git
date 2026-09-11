/* Phase 68b: sg_rev_parse_commit_ex's two disambiguation modes, its two
   commit-ish dwim triggers (caller-requested, and a "~"/"^"/"@{" suffix
   forcing it regardless of the caller), the new -4 return code, and
   sg_rev_parse_object's own always-STRICT prefix branch. Phase 68a's own
   tests/test_oid_prefix.c already covers sg_object_find_prefix itself
   (enumeration, dedup, case-insensitivity, pack/loose) -- this file is
   about the layer built ON TOP of it. */
#include "sg/revparse.h"

#include "sg/cli_args.h"
#include "sg/loose.h"
#include "sg/object.h"
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
static long long time_seq = 3000000;

#define CHECK(cond, ...)                                                                         \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);                                  \
            fprintf(stderr, __VA_ARGS__);                                                         \
            fprintf(stderr, "\n");                                                                \
            failures++;                                                                           \
        }                                                                                          \
    } while (0)

static char *make_tmp_repo(void)
{
    static char template[] = "/tmp/sg_revparse_abbrev_test_XXXXXX";
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

/* Same shape as test_revparse.c's own make_commit -- see its comment. */
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

/* Brute-forces a BLOB whose id begins with `prefix` (4 hex chars),
   guaranteed to terminate within 65536+slack tries by the pigeonhole
   principle (same technique test_oid_prefix.c's own
   test_multiple_candidates uses) -- hashes are only computed in memory via
   sg_object_hash until a match is found, so this does not spam the loose
   store with near-misses. */
static void brute_force_blob(const char *git_dir, const char *prefix,
                             unsigned char id_out[SG_SHA1_RAW_LEN])
{
    int i;

    for (i = 0; i < 300000; i++) {
        char content[64];
        unsigned char id[SG_SHA1_RAW_LEN];
        char hex[SG_SHA1_HEX_LEN + 1];

        snprintf(content, sizeof(content), "p68abbrev-blob-%d", i);
        sg_object_hash(SG_OBJ_BLOB, content, strlen(content), id);
        sg_sha1_to_hex(id, hex);
        if (memcmp(hex, prefix, 4) == 0) {
            CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, content, strlen(content), id_out) == 0,
                 "loose_write failed for winning blob content");
            CHECK(memcmp(id_out, id, SG_SHA1_RAW_LEN) == 0, "written blob id mismatch");
            return;
        }
    }
    CHECK(0, "brute_force_blob: no match for prefix %s within 300000 tries", prefix);
    memset(id_out, 0, SG_SHA1_RAW_LEN);
}

/* Same idea for a bare TREE: a single entry pointing at a fixed anchor
   blob, only the entry NAME varies, hashed via sg_tree_serialize before
   ever writing. */
static void brute_force_tree(const char *git_dir, const char *prefix,
                             unsigned char id_out[SG_SHA1_RAW_LEN])
{
    unsigned char anchor_blob[SG_SHA1_RAW_LEN];
    int i;

    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "p68abbrev-tree-entry-anchor", 27, anchor_blob) == 0,
         "anchor blob write failed");

    for (i = 0; i < 300000; i++) {
        char name[64];
        sg_tree_entry entry;
        unsigned char *serialized;
        size_t serialized_len;
        unsigned char id[SG_SHA1_RAW_LEN];
        char hex[SG_SHA1_HEX_LEN + 1];

        snprintf(name, sizeof(name), "p68tree-entry-%d.txt", i);
        entry.mode = 0100644;
        entry.name = name;
        memcpy(entry.sha1, anchor_blob, SG_SHA1_RAW_LEN);

        CHECK(sg_tree_serialize(&entry, 1, &serialized, &serialized_len) == 0,
             "tree serialize failed for entry %d", i);
        sg_object_hash(SG_OBJ_TREE, serialized, serialized_len, id);
        sg_sha1_to_hex(id, hex);
        if (memcmp(hex, prefix, 4) == 0) {
            CHECK(sg_loose_write(git_dir, SG_OBJ_TREE, serialized, serialized_len, id_out) == 0,
                 "loose_write failed for winning tree content");
            CHECK(memcmp(id_out, id, SG_SHA1_RAW_LEN) == 0, "written tree id mismatch");
            free(serialized);
            return;
        }
        free(serialized);
    }
    CHECK(0, "brute_force_tree: no match for prefix %s within 300000 tries", prefix);
    memset(id_out, 0, SG_SHA1_RAW_LEN);
}

/* Same idea for a COMMIT: fixed tree/no parents, only the message varies,
   hashed via sg_commit_serialize + sg_object_hash before ever writing. */
static void brute_force_commit(const char *git_dir, const char *prefix,
                               unsigned char id_out[SG_SHA1_RAW_LEN])
{
    unsigned char blob_id[SG_SHA1_RAW_LEN];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    sg_flat_entry entry;
    int i;

    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "p68abbrev-tree-anchor", 21, blob_id) == 0,
         "anchor blob write failed");
    entry.path = (char *)"anchor.txt";
    entry.mode = 0100644;
    memcpy(entry.sha1, blob_id, SG_SHA1_RAW_LEN);
    CHECK(sg_tree_build(git_dir, &entry, 1, tree_id) == 0, "anchor tree build failed");

    for (i = 0; i < 300000; i++) {
        char message[64];
        sg_commit commit;
        unsigned char *serialized;
        size_t serialized_len;
        unsigned char id[SG_SHA1_RAW_LEN];
        char hex[SG_SHA1_HEX_LEN + 1];

        snprintf(message, sizeof(message), "p68abbrev commit msg %d\n", i);
        memset(&commit, 0, sizeof(commit));
        memcpy(commit.tree, tree_id, SG_SHA1_RAW_LEN);
        commit.author_name = (char *)"tester";
        commit.author_email = (char *)"tester@example.com";
        commit.author_time = 1700000000;
        strcpy(commit.author_tz, "+0000");
        commit.committer_name = commit.author_name;
        commit.committer_email = commit.author_email;
        commit.committer_time = commit.author_time;
        strcpy(commit.committer_tz, "+0000");
        commit.message = message;

        CHECK(sg_commit_serialize(&commit, &serialized, &serialized_len) == 0,
             "serialize failed for msg %d", i);
        sg_object_hash(SG_OBJ_COMMIT, serialized, serialized_len, id);
        sg_sha1_to_hex(id, hex);
        if (memcmp(hex, prefix, 4) == 0) {
            CHECK(sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len, id_out) == 0,
                 "loose_write failed for winning commit content");
            CHECK(memcmp(id_out, id, SG_SHA1_RAW_LEN) == 0, "written commit id mismatch");
            free(serialized);
            return;
        }
        free(serialized);
    }
    CHECK(0, "brute_force_commit: no match for prefix %s within 300000 tries", prefix);
    memset(id_out, 0, SG_SHA1_RAW_LEN);
}

/* Same idea for an annotated TAG pointing at `target`, whose type is
   `target_type` -- Phase 68b review round 3 needs a tag pointing at a
   BLOB (target_type == SG_OBJ_BLOB) to prove membership is decided by the
   PEELED type, so this is no longer hardcoded to SG_OBJ_COMMIT. */
static void brute_force_tag(const char *git_dir, const char *prefix,
                            const unsigned char target[SG_SHA1_RAW_LEN], sg_obj_type target_type,
                            unsigned char id_out[SG_SHA1_RAW_LEN])
{
    int i;

    for (i = 0; i < 300000; i++) {
        char tag_name[32];
        sg_tag tag;
        unsigned char *serialized;
        size_t serialized_len;
        unsigned char id[SG_SHA1_RAW_LEN];
        char hex[SG_SHA1_HEX_LEN + 1];

        snprintf(tag_name, sizeof(tag_name), "p68t%d", i);
        memset(&tag, 0, sizeof(tag));
        memcpy(tag.object, target, SG_SHA1_RAW_LEN);
        tag.object_type = target_type;
        tag.tag_name = tag_name;
        tag.tagger_name = (char *)"tester";
        tag.tagger_email = (char *)"tester@example.com";
        tag.tagger_time = 1700000000;
        strcpy(tag.tagger_tz, "+0000");
        tag.message = (char *)"tag message\n";

        CHECK(sg_tag_serialize(&tag, &serialized, &serialized_len) == 0,
             "tag serialize failed for %s", tag_name);
        sg_object_hash(SG_OBJ_TAG, serialized, serialized_len, id);
        sg_sha1_to_hex(id, hex);
        if (memcmp(hex, prefix, 4) == 0) {
            CHECK(sg_loose_write(git_dir, SG_OBJ_TAG, serialized, serialized_len, id_out) == 0,
                 "loose_write failed for winning tag content");
            CHECK(memcmp(id_out, id, SG_SHA1_RAW_LEN) == 0, "written tag id mismatch");
            free(serialized);
            return;
        }
        free(serialized);
    }
    CHECK(0, "brute_force_tag: no match for prefix %s within 300000 tries", prefix);
    memset(id_out, 0, SG_SHA1_RAW_LEN);
}

/* Exactly one commit-ish candidate (the commit itself) among {commit, blob}
   sharing a 4-hex prefix: STRICT refuses (-4), COMMITTISH resolves. */
static void test_committish_resolves_single_commitish(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    unsigned char blob_id[SG_SHA1_RAW_LEN];
    char prefix[5];
    char hex[SG_SHA1_HEX_LEN + 1];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "solo commit", NULL, 0, commit_id);
    sg_sha1_to_hex(commit_id, hex);
    memcpy(prefix, hex, 4);
    prefix[4] = '\0';
    brute_force_blob(git_dir, prefix, blob_id);

    CHECK(sg_rev_parse_commit_ex(git_dir, prefix, SG_REV_STRICT, out) == -4,
         "STRICT on a commit+blob collision should be -4");
    CHECK(sg_rev_parse_commit_ex(git_dir, prefix, SG_REV_COMMITTISH, out) == 0,
         "COMMITTISH on a commit+blob collision (exactly one commit-ish) should resolve");
    CHECK(memcmp(out, commit_id, SG_SHA1_RAW_LEN) == 0,
         "COMMITTISH should resolve to the commit, not the blob");

    /* sg_rev_parse_commit (no _ex) is always STRICT. */
    CHECK(sg_rev_parse_commit(git_dir, prefix, out) == -4,
         "plain sg_rev_parse_commit must default to STRICT");

    free(git_dir);
}

/* Two commits sharing a prefix: COMMITTISH still refuses, same as STRICT --
   "exactly one" commit-ish, not "at least one". */
static void test_committish_ambiguous_with_two_commits(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN];
    char prefix[5];
    char hex[SG_SHA1_HEX_LEN + 1];
    unsigned char c2[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "first of two", NULL, 0, c1);
    sg_sha1_to_hex(c1, hex);
    memcpy(prefix, hex, 4);
    prefix[4] = '\0';
    brute_force_commit(git_dir, prefix, c2);
    CHECK(memcmp(c1, c2, SG_SHA1_RAW_LEN) != 0, "the two brute-forced commits must differ");

    CHECK(sg_rev_parse_commit_ex(git_dir, prefix, SG_REV_COMMITTISH, out) == -4,
         "two commit-ish candidates must still refuse under COMMITTISH");
    CHECK(sg_rev_parse_commit_ex(git_dir, prefix, SG_REV_STRICT, out) == -4,
         "and under STRICT");

    free(git_dir);
}

/* A tag counts as commit-ish and is PEELED: tag+blob collision resolves
   under COMMITTISH to the tag's TARGET commit, not the tag object itself. */
static void test_committish_peels_tag(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char target[SG_SHA1_RAW_LEN];
    unsigned char tag_id[SG_SHA1_RAW_LEN];
    unsigned char blob_id[SG_SHA1_RAW_LEN];
    char prefix[5];
    char hex[SG_SHA1_HEX_LEN + 1];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "tag target", NULL, 0, target);
    /* The prefix must NOT be `target`'s own -- target is itself a commit
       sitting in the store, so reusing its prefix would make it a THIRD
       commit-ish candidate (target + the crafted tag), turning this into
       the "two commit-ish candidates" case test_committish_ambiguous_
       commit_and_tag already covers, not the single-commit-ish case this
       test means to exercise. Derive an unrelated prefix instead, from a
       hash that is never written to the store at all. */
    {
        unsigned char seed[SG_SHA1_RAW_LEN];

        sg_object_hash(SG_OBJ_BLOB, "p68abbrev-tag-peel-seed", 24, seed);
        sg_sha1_to_hex(seed, hex);
    }
    memcpy(prefix, hex, 4);
    prefix[4] = '\0';
    brute_force_tag(git_dir, prefix, target, SG_OBJ_COMMIT, tag_id);
    brute_force_blob(git_dir, prefix, blob_id);

    CHECK(sg_rev_parse_commit_ex(git_dir, prefix, SG_REV_COMMITTISH, out) == 0,
         "tag+blob collision (one commit-ish: the tag) should resolve under COMMITTISH");
    CHECK(memcmp(out, target, SG_SHA1_RAW_LEN) == 0,
         "COMMITTISH must resolve to the tag's PEELED target, not the tag object");
    CHECK(sg_rev_parse_commit_ex(git_dir, prefix, SG_REV_STRICT, out) == -4,
         "the same collision must still refuse under STRICT");

    free(git_dir);
}

/* A commit colliding with a tag (pointing elsewhere) is TWO commit-ish
   candidates and refuses, even though every candidate is commit-ish. */
static void test_committish_ambiguous_commit_and_tag(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN];
    unsigned char c2[SG_SHA1_RAW_LEN];
    unsigned char tag_id[SG_SHA1_RAW_LEN];
    char prefix[5];
    char hex[SG_SHA1_HEX_LEN + 1];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "c1", NULL, 0, c1);
    make_commit(git_dir, "c2 (tag target)", NULL, 0, c2);
    sg_sha1_to_hex(c1, hex);
    memcpy(prefix, hex, 4);
    prefix[4] = '\0';
    brute_force_tag(git_dir, prefix, c2, SG_OBJ_COMMIT, tag_id);

    CHECK(sg_rev_parse_commit_ex(git_dir, prefix, SG_REV_COMMITTISH, out) == -4,
         "commit+tag collision is two commit-ish candidates, must still refuse");

    free(git_dir);
}

/* Trigger 2: a "~"/"^"/"@{" suffix forces the BASE to resolve as
   COMMITTISH regardless of what the caller asked for. */
static void test_suffix_forces_committish_regardless_of_caller(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char parent[SG_SHA1_RAW_LEN];
    unsigned char child[SG_SHA1_RAW_LEN];
    unsigned char blob_id[SG_SHA1_RAW_LEN];
    char prefix[5];
    char hex[SG_SHA1_HEX_LEN + 1];
    char rev[16];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "parent", NULL, 0, parent);
    make_commit(git_dir, "child", &parent, 1, child);
    sg_sha1_to_hex(child, hex);
    memcpy(prefix, hex, 4);
    prefix[4] = '\0';
    brute_force_blob(git_dir, prefix, blob_id);

    /* bare prefix under STRICT: refuses */
    CHECK(sg_rev_parse_commit_ex(git_dir, prefix, SG_REV_STRICT, out) == -4,
         "bare ambiguous prefix under STRICT must refuse");

    /* the SAME prefix, with "~1", under the SAME STRICT mode: resolves,
       because the suffix escalates the base to COMMITTISH internally. */
    snprintf(rev, sizeof(rev), "%s~1", prefix);
    CHECK(sg_rev_parse_commit_ex(git_dir, rev, SG_REV_STRICT, out) == 0,
         "a '~1' suffix must force the ambiguous base to commit-ish even under STRICT");
    CHECK(memcmp(out, parent, SG_SHA1_RAW_LEN) == 0,
         "the resolved commit's parent must be `parent` (child~1)");

    /* plain sg_rev_parse_commit (STRICT) shows the identical behavior,
       since it is nothing more than sg_rev_parse_commit_ex(..., STRICT, ...). */
    CHECK(sg_rev_parse_commit(git_dir, rev, out) == 0,
         "plain sg_rev_parse_commit must inherit trigger 2 as well");

    free(git_dir);
}

/* Section 2/5b: a ref literally named with a valid hex prefix wins over the
   prefix interpretation, and this must hold regardless of `disambig` --
   the ref lookup happens before any prefix code runs at all. */
static void test_ref_beats_prefix(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char commit_a[SG_SHA1_RAW_LEN];
    unsigned char commit_b[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];
    char prefix6[7];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "a", NULL, 0, commit_a);
    make_commit(git_dir, "b (branch target)", NULL, 0, commit_b);
    sg_sha1_to_hex(commit_a, hex);
    memcpy(prefix6, hex, 6);
    prefix6[6] = '\0';

    /* A branch literally named with commit_a's own 6-hex prefix, but
       pointing at commit_b -- if the ref lookup did not win, this would
       resolve to commit_a via the prefix interpretation instead. */
    CHECK(sg_ref_update_branch(git_dir, prefix6, commit_b) == 0, "branch write failed");

    CHECK(sg_rev_parse_commit_ex(git_dir, prefix6, SG_REV_STRICT, out) == 0,
         "a ref named like a prefix must resolve (not -4, even though the prefix ITSELF might "
         "also be ambiguous or unique as an object id)");
    CHECK(memcmp(out, commit_b, SG_SHA1_RAW_LEN) == 0,
         "the ref must win: resolves to commit_b (the branch's target), not commit_a (what the "
         "hex would otherwise abbreviate)");

    free(git_dir);
}

/* sg_rev_parse_object's own prefix branch: resolves ANY object type (not
   just commits), always SG_REV_STRICT regardless of anything -- and
   returns -4 the same way sg_rev_parse_commit does. */
static void test_object_prefix_any_type_and_dash4(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char blob_id[SG_SHA1_RAW_LEN];
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];
    char prefix7[8];
    char prefix4[5];
    unsigned char out[SG_SHA1_RAW_LEN];
    sg_obj_type type;
    char bad_path[256];

    /* a UNIQUE blob prefix resolves to a BLOB, not forced through the
       commit-only grammar */
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "solo blob content", 18, blob_id) == 0,
         "blob write failed");
    sg_sha1_to_hex(blob_id, hex);
    memcpy(prefix7, hex, 7);
    prefix7[7] = '\0';

    bad_path[0] = '\0';
    CHECK(sg_rev_parse_object(git_dir, prefix7, out, &type, bad_path, sizeof(bad_path)) == 0,
         "unique blob prefix should resolve via sg_rev_parse_object");
    CHECK(type == SG_OBJ_BLOB, "resolved type must be BLOB, got %d", (int)type);
    CHECK(memcmp(out, blob_id, SG_SHA1_RAW_LEN) == 0, "resolved id must equal the blob's id");

    /* an ambiguous commit+blob prefix: sg_rev_parse_object is STRICT, must
       return -4 (it does NOT dwim to the commit the way sg log would). */
    make_commit(git_dir, "obj prefix commit", NULL, 0, commit_id);
    sg_sha1_to_hex(commit_id, hex);
    memcpy(prefix4, hex, 4);
    prefix4[4] = '\0';
    {
        unsigned char blob2[SG_SHA1_RAW_LEN];

        brute_force_blob(git_dir, prefix4, blob2);
    }
    bad_path[0] = '\0';
    CHECK(sg_rev_parse_object(git_dir, prefix4, out, &type, bad_path, sizeof(bad_path)) == -4,
         "sg_rev_parse_object must return -4 on an ambiguous prefix, never dwim");

    free(git_dir);
}

/* A prefix matching zero objects is "not found" (-1), never -4 -- git's own
   "ambiguous argument" wording (section 5 of the spec), distinct from
   "short object ID is ambiguous". */
static void test_no_match_is_dash1_not_dash4(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char out[SG_SHA1_RAW_LEN];

    /* four hex chars that (with overwhelming probability) match nothing in
       a freshly-initialized, otherwise-empty repo */
    CHECK(sg_rev_parse_commit_ex(git_dir, "0000", SG_REV_STRICT, out) == -1,
         "a well-formed but unmatched prefix must be -1, not -4");

    free(git_dir);
}

/* SG_REV_TREEISH review round: a commit counts toward TREEISH too (it is
   the SAME "commit or tag" set plus tree), so a commit+blob collision --
   already proven to resolve under COMMITTISH -- must resolve under TREEISH
   as well, and to the SAME id. */
static void test_treeish_resolves_commit_candidate_too(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    unsigned char blob_id[SG_SHA1_RAW_LEN];
    char prefix[5];
    char hex[SG_SHA1_HEX_LEN + 1];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "treeish solo commit", NULL, 0, commit_id);
    sg_sha1_to_hex(commit_id, hex);
    memcpy(prefix, hex, 4);
    prefix[4] = '\0';
    brute_force_blob(git_dir, prefix, blob_id);

    CHECK(sg_rev_parse_commit_ex(git_dir, prefix, SG_REV_TREEISH, out) == 0,
         "TREEISH on a commit+blob collision (one tree-ish: the commit) should resolve");
    CHECK(memcmp(out, commit_id, SG_SHA1_RAW_LEN) == 0, "TREEISH must resolve to the commit");

    free(git_dir);
}

/* The discriminating case the review round exists for: a commit+tree
   collision is exactly ONE commit-ish candidate (COMMITTISH would resolve)
   but exactly TWO tree-ish candidates (the commit AND the tree both count
   under TREEISH) -- so TREEISH must REFUSE where COMMITTISH would have
   resolved. Using COMMITTISH here instead of TREEISH is the "wrong answer"
   failure mode the review round found: it would silently resolve an input
   real git refuses. */
static void test_treeish_ambiguous_with_commit_and_tree(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    char prefix[5];
    char hex[SG_SHA1_HEX_LEN + 1];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "commit+tree collision", NULL, 0, commit_id);
    sg_sha1_to_hex(commit_id, hex);
    memcpy(prefix, hex, 4);
    prefix[4] = '\0';
    brute_force_tree(git_dir, prefix, tree_id);

    CHECK(sg_rev_parse_commit_ex(git_dir, prefix, SG_REV_COMMITTISH, out) == 0,
         "COMMITTISH control: only the commit counts, so this must resolve");
    CHECK(memcmp(out, commit_id, SG_SHA1_RAW_LEN) == 0, "COMMITTISH must resolve to the commit");

    CHECK(sg_rev_parse_commit_ex(git_dir, prefix, SG_REV_TREEISH, out) == -4,
         "TREEISH must REFUSE the SAME prefix -- both the commit AND the tree count, two matches, "
         "not one; resolving here would be a WRONG ANSWER (real git also refuses)");

    free(git_dir);
}

/* When the unique TREE-ish candidate is itself a bare tree (not a commit
   or tag), sg_rev_parse_commit_ex must still fail -- it only ever yields a
   commit, and "does this resolve to a commit" is a DIFFERENT question from
   "did disambiguation pick a unique candidate". This is the pre-existing
   gap (CLAUDE.md / revparse.c's own comments): resolve_ambiguous_prefix
   correctly narrows down to the tree, but the final peel-and-require-
   commit step then rejects it, exactly reproducing the same failure a
   full 40-hex tree id already has via this path. Do NOT invent new
   behavior that makes this resolve. */
static void test_treeish_unique_bare_tree_still_fails(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    unsigned char blob_id[SG_SHA1_RAW_LEN];
    char prefix[5];
    char hex[SG_SHA1_HEX_LEN + 1];
    unsigned char out[SG_SHA1_RAW_LEN];

    /* An arbitrary tree, hashed but not anchored to any commit/tag. */
    {
        unsigned char seed[SG_SHA1_RAW_LEN];

        sg_object_hash(SG_OBJ_BLOB, "p68abbrev-bare-tree-seed", 25, seed);
        sg_sha1_to_hex(seed, hex);
    }
    memcpy(prefix, hex, 4);
    prefix[4] = '\0';
    brute_force_tree(git_dir, prefix, tree_id);
    brute_force_blob(git_dir, prefix, blob_id);

    CHECK(sg_rev_parse_commit_ex(git_dir, prefix, SG_REV_TREEISH, out) == -1,
         "a unique tree-ish candidate that is itself a bare TREE must still fail with -1 "
         "(sg_rev_parse_commit_ex only ever yields a commit) -- NOT resolve, and NOT -4");

    free(git_dir);
}

/* Review round 3 (found by an independent cold review, then measured
   against real git 2.55.0): membership under COMMITTISH/TREEISH must be
   decided by a candidate's PEELED type, not its own raw type. Decisive
   fixture: a tag pointing at a BLOB, colliding with a REAL commit. If a
   tag counted unconditionally (raw type), that would be two commit-ish
   candidates (the tag AND the commit) and COMMITTISH would refuse; git
   resolves instead, so it must be excluding the tag once its target turns
   out not to be a commit. */
static void test_committish_excludes_tag_to_blob(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    unsigned char blob_id[SG_SHA1_RAW_LEN];
    unsigned char tag_id[SG_SHA1_RAW_LEN];
    char prefix[5];
    char hex[SG_SHA1_HEX_LEN + 1];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "real commit, tag points elsewhere", NULL, 0, commit_id);
    sg_sha1_to_hex(commit_id, hex);
    memcpy(prefix, hex, 4);
    prefix[4] = '\0';
    brute_force_blob(git_dir, prefix, blob_id);
    brute_force_tag(git_dir, prefix, blob_id, SG_OBJ_BLOB, tag_id);

    CHECK(sg_rev_object_matches_disambig(git_dir, tag_id, SG_REV_COMMITTISH) == 0,
         "a tag pointing at a blob must NOT match COMMITTISH (peeled type is BLOB)");
    CHECK(sg_rev_object_matches_disambig(git_dir, commit_id, SG_REV_COMMITTISH) == 1,
         "the real commit must still match COMMITTISH");

    CHECK(sg_rev_parse_commit_ex(git_dir, prefix, SG_REV_COMMITTISH, out) == 0,
         "COMMITTISH must resolve (exactly one PEELED commit-ish candidate: the real commit, "
         "not two -- the tag->blob does not count)");
    CHECK(memcmp(out, commit_id, SG_SHA1_RAW_LEN) == 0,
         "COMMITTISH must resolve to the real commit");

    free(git_dir);
}

/* Same fixture family, widened with a TREE: now the real commit AND the
   tree both count toward TREEISH (two matches), so it must refuse -- the
   tag->blob is excluded the same way, but that alone is not enough to make
   this resolve, since a genuine two-way tree-ish collision remains. This
   is also the "wrong answer" control: an implementation that (incorrectly)
   let the raw-type tag count toward COMMITTISH would see THREE commit-ish
   candidates here and refuse for the WRONG reason; this test's assertion
   is on TREEISH, where refusing is the CORRECT answer for a different,
   deliberate reason (commit+tree, not tag+commit+tree). */
static void test_treeish_excludes_tag_to_blob_but_still_ambiguous(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    unsigned char blob_id[SG_SHA1_RAW_LEN];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    unsigned char tag_id[SG_SHA1_RAW_LEN];
    char prefix[5];
    char hex[SG_SHA1_HEX_LEN + 1];
    unsigned char out[SG_SHA1_RAW_LEN];

    make_commit(git_dir, "commit+tree, tag points at a blob", NULL, 0, commit_id);
    sg_sha1_to_hex(commit_id, hex);
    memcpy(prefix, hex, 4);
    prefix[4] = '\0';
    brute_force_blob(git_dir, prefix, blob_id);
    brute_force_tree(git_dir, prefix, tree_id);
    brute_force_tag(git_dir, prefix, blob_id, SG_OBJ_BLOB, tag_id);

    CHECK(sg_rev_object_matches_disambig(git_dir, tag_id, SG_REV_TREEISH) == 0,
         "the tag->blob must NOT match TREEISH either");
    CHECK(sg_rev_object_matches_disambig(git_dir, commit_id, SG_REV_TREEISH) == 1,
         "the commit must match TREEISH");
    CHECK(sg_rev_object_matches_disambig(git_dir, tree_id, SG_REV_TREEISH) == 1,
         "the bare tree must match TREEISH");

    CHECK(sg_rev_parse_commit_ex(git_dir, prefix, SG_REV_TREEISH, out) == -4,
         "TREEISH must still refuse -- excluding the tag leaves exactly TWO tree-ish "
         "candidates (the commit and the tree), not one");

    free(git_dir);
}

static int g_saved_stderr = -1;
static char g_stderr_capture_path[] = "/tmp/sg_revparse_abbrev_stderr_XXXXXX";

/* Redirects stderr to a temp file so sg_cli_report_ambiguous_oid's output
   (it only ever writes to stderr) can be inspected -- same dup2 technique
   test_diff_combined.c's own capture_start/capture_end use for stdout. */
static void capture_stderr_start(void)
{
    char path[] = "/tmp/sg_revparse_abbrev_stderr_XXXXXX";
    int fd;

    fflush(stderr);
    fd = mkstemp(path);
    if (fd < 0) {
        fprintf(stderr, "mkstemp failed\n");
        exit(1);
    }
    memcpy(g_stderr_capture_path, path, sizeof(g_stderr_capture_path));
    g_saved_stderr = dup(STDERR_FILENO);
    /* Round 5 review: an unchecked dup() failure (-1) would make
       capture_stderr_end's dup2(-1, STDERR_FILENO) fail too, leaving
       stderr permanently redirected into the (by then unlinked) capture
       file -- every subsequent CHECK failure in this whole binary would
       print its "FAIL ..." line into nothing, a silent false-green. Setup
       failure, so exit(1) immediately, same convention as the mkstemp
       check just above. */
    if (g_saved_stderr < 0) {
        fprintf(stderr, "dup(STDERR_FILENO) failed\n");
        exit(1);
    }
    dup2(fd, STDERR_FILENO);
    close(fd);
}

static char *capture_stderr_end(void)
{
    FILE *f;
    long len;
    char *buf;

    fflush(stderr);
    dup2(g_saved_stderr, STDERR_FILENO);
    close(g_saved_stderr);
    g_saved_stderr = -1;

    f = fopen(g_stderr_capture_path, "rb");
    if (f == NULL) {
        fprintf(stderr, "failed to reopen stderr capture file\n");
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)len + 1);
    if (buf == NULL) {
        fprintf(stderr, "OOM reading stderr capture file\n");
        exit(1);
    }
    if (len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "short read on stderr capture file\n");
        exit(1);
    }
    buf[len] = '\0';
    fclose(f);
    unlink(g_stderr_capture_path);
    return buf;
}

/* Counts occurrences of `needle` in `haystack` (non-overlapping). */
static int count_occurrences(const char *haystack, const char *needle)
{
    int n = 0;
    const char *p = haystack;
    size_t needle_len = strlen(needle);

    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += needle_len;
    }
    return n;
}

/* Phase 68b review round 3: sg_cli_report_ambiguous_oid's own filtering
   path (the "hint:" narrowing) had ZERO test coverage -- neither this
   file nor interop's phase68 group ever drove it through anything other
   than STRICT (the 4-way fixture is only ever read with `cat-file -t`)
   until this test.

   Round 5 review (a second cold reader, independently mutation-verified
   by the main conversation): the FIRST version of this fixture -- a tag
   pointing at a REAL commit (so it counted toward COMMITTISH), plus a
   tree and a blob that did not -- could not tell a correct per-row
   `continue` (skip the excluded row, keep scanning) apart from the
   ORIGINAL round-3 bug's `break` (stop at the first excluded row,
   assuming the excluded rows are a contiguous tail): amb_type_rank sorts
   tag/commit ahead of tree/blob regardless of peeling, so in that
   fixture the two EXCLUDED rows (tree, blob) already sorted as a
   contiguous tail after the two INCLUDED ones (tag, commit) -- `break`
   and `continue` produce byte-identical output there. Measured directly:
   reverting the print loop's `continue` back to `break` left this test
   fully green while interop's own tag-to-blob group did catch it.

   This fixture is built to make that reversion OBSERVABLE HERE too: the
   EXCLUDED candidate is a tag pointing at a BLOB (raw type TAG, so it
   sorts FIRST -- amb_type_rank 0 -- same as any other tag), while the
   INCLUDED candidate (a real commit) sorts SECOND (rank 1). A `break`-based
   loop stops at the very first row -- the excluded tag -- and prints ZERO
   hint lines; the correct per-row `continue` skips it and still prints
   the commit. Verified (per this project's mutate-and-confirm-red
   convention, in a /private/tmp scratch copy, not in this tree): reverting
   `continue` to `break` in cli_args.c's print loop turns this exact
   CHECK red (0 lines, not 1). */
static void test_report_ambiguous_oid_filters_by_peeled_type(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    unsigned char blob_id[SG_SHA1_RAW_LEN]; /* the tag's own target -- must NOT count */
    unsigned char tag_id[SG_SHA1_RAW_LEN];  /* tag -> blob_id: raw type TAG sorts FIRST, excluded */
    char prefix[5];
    char hex[SG_SHA1_HEX_LEN + 1];
    char *out;

    make_commit(git_dir, "report filter fixture (peeled-exclusion ordering)", NULL, 0, commit_id);
    sg_sha1_to_hex(commit_id, hex);
    memcpy(prefix, hex, 4);
    prefix[4] = '\0';
    brute_force_blob(git_dir, prefix, blob_id);
    brute_force_tag(git_dir, prefix, blob_id, SG_OBJ_BLOB, tag_id);

    capture_stderr_start();
    sg_cli_report_ambiguous_oid(git_dir, prefix, SG_REV_COMMITTISH);
    out = capture_stderr_end();

    CHECK(count_occurrences(out, "hint:   ") == 1,
         "COMMITTISH must print exactly 1 hint line (the commit only) -- the tag->blob sorts "
         "FIRST but is excluded, and must not stop the scan before the commit is reached, got:\n%s",
         out);
    CHECK(strstr(out, " commit ") != NULL, "the commit row must be present, got:\n%s", out);
    CHECK(strstr(out, " tag ") == NULL, "the tag(->blob) row must be EXCLUDED, got:\n%s", out);
    CHECK(strncmp(out, "error: short object ID ", strlen("error: short object ID ")) == 0,
         "must start with the borrowed error: line, got:\n%s", out);

    free(out);
    free(git_dir);
}

/* Round 5 review, third ask: sg_rev_object_matches_disambig's "peel chain
   fails -> excluded, not a resolution failure" branch had ZERO coverage.
   Deterministic, no race needed: a tag pointing at an id that exists
   NOWHERE in the store fails peel_to_non_tag's own sg_object_read on
   every call, for a reason that has nothing to do with timing. */
static void test_matches_disambig_excludes_broken_tag_chain(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    unsigned char bogus_target[SG_SHA1_RAW_LEN];
    unsigned char broken_tag_id[SG_SHA1_RAW_LEN];
    char prefix[5];
    char hex[SG_SHA1_HEX_LEN + 1];
    unsigned char out[SG_SHA1_RAW_LEN];
    size_t i;

    make_commit(git_dir, "peel chain failure fixture", NULL, 0, commit_id);
    sg_sha1_to_hex(commit_id, hex);
    memcpy(prefix, hex, 4);
    prefix[4] = '\0';

    for (i = 0; i < SG_SHA1_RAW_LEN; i++)
        bogus_target[i] = (unsigned char)(0xAB ^ i);
    brute_force_tag(git_dir, prefix, bogus_target, SG_OBJ_COMMIT, broken_tag_id);

    CHECK(sg_rev_object_matches_disambig(git_dir, broken_tag_id, SG_REV_COMMITTISH) == 0,
         "a tag whose target does not exist anywhere must NOT match (excluded, not an error)");
    CHECK(sg_rev_object_matches_disambig(git_dir, broken_tag_id, SG_REV_TREEISH) == 0,
         "...and not under TREEISH either");

    /* The broken tag is excluded from the count, not a hard failure --
       COMMITTISH still resolves cleanly to the real commit despite it
       sharing the prefix. */
    CHECK(sg_rev_parse_commit_ex(git_dir, prefix, SG_REV_COMMITTISH, out) == 0,
         "COMMITTISH must still resolve to the real commit despite the broken-chain tag sharing "
         "the prefix");
    CHECK(memcmp(out, commit_id, SG_SHA1_RAW_LEN) == 0, "must resolve to the real commit");

    free(git_dir);
}

/* Round 5 review: places a loose object file whose NAME matches a target
   prefix (so sg_object_find_prefix's directory enumeration finds it as a
   candidate) but whose CONTENT is garbage (not a valid zlib stream) --
   directly at the filesystem level, since sg_loose_write always writes
   valid content and cannot build this. */
static void write_corrupt_loose_object(const char *git_dir, const char *hex40)
{
    char dir_path[4096];
    char file_path[4096];
    FILE *f;

    snprintf(dir_path, sizeof(dir_path), "%s/objects/%.2s", git_dir, hex40);
    CHECK(mkdir(dir_path, 0755) == 0 || errno == EEXIST, "mkdir failed for corrupt object dir");
    snprintf(file_path, sizeof(file_path), "%s/objects/%.2s/%s", git_dir, hex40, hex40 + 2);
    f = fopen(file_path, "wb");
    CHECK(f != NULL, "fopen failed for corrupt object file");
    if (f != NULL) {
        static const char garbage[] = "not a valid zlib stream, deliberately garbage bytes";

        fwrite(garbage, 1, sizeof(garbage) - 1, f);
        fclose(f);
    }
}

/* Round 5 review: a crash-safety test for an UNREADABLE candidate sharing
   an ambiguous prefix with a real commit.

   MEASURED, not assumed: this does NOT exercise print_commit_candidate_
   line/print_tag_candidate_line's own read-failure branch -- the bug
   round 3 actually fixed. sg_cli_report_ambiguous_oid reads a given
   candidate TWICE: once in its own candidate-building loop (just to learn
   the type), and again, independently, inside print_commit_candidate_line/
   print_tag_candidate_line (to render the date/subject fields). Both
   calls run the exact same sg_object_read against the exact same
   UNCHANGED bytes, so for a file that is corrupt from the start, they
   always agree: the FIRST read (in the candidate-building loop) already
   fails and folds the candidate to SG_OBJ_BLOB (this file's own existing
   fallback), which prints through the plain "hint:   <hex> blob" branch --
   print_commit_candidate_line is never even reached for it. Confirmed by
   directly inspecting the captured output below (it contains " blob",
   never triggers the commit-row formatter). Reproducing the ACTUAL bug
   (a read that SUCCEEDS once, revealing type, then FAILS on a second,
   later read of the same id) needs the object to change state BETWEEN
   those two reads -- a genuine TOCTOU race (the original bug comment's own
   example: a concurrent `git gc`) that a statically-corrupt file cannot
   reproduce, and this project's testing conventions have no hook for
   simulating.

   Kept anyway, because it is still a real, useful, independently-worded
   property: an unreadable candidate must not crash
   sg_cli_report_ambiguous_oid, checked under `make sanitize` in
   particular, where an actual uninitialized-pointer free WOULD abort.
   It is a safety net, not a regression guard for round 3's specific bug --
   see the module's own git history / CLAUDE.md notes for that guard's
   actual status (currently: unreachable by any known deterministic
   construction). */
static void test_report_ambiguous_oid_survives_unreadable_candidate(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];
    char corrupt_hex[SG_SHA1_HEX_LEN + 1];
    char prefix[5];
    char *out;

    make_commit(git_dir, "unreadable candidate safety fixture", NULL, 0, commit_id);
    sg_sha1_to_hex(commit_id, hex);
    memcpy(prefix, hex, 4);
    prefix[4] = '\0';

    memcpy(corrupt_hex, hex, SG_SHA1_HEX_LEN);
    corrupt_hex[SG_SHA1_HEX_LEN] = '\0';
    /* Flip two trailing hex digits so this names a DIFFERENT (and
       nonexistent, until written below) object than the real commit,
       while keeping the same 4-hex prefix. */
    corrupt_hex[39] = corrupt_hex[39] == '0' ? '1' : '0';
    corrupt_hex[38] = corrupt_hex[38] == '0' ? '1' : '0';
    write_corrupt_loose_object(git_dir, corrupt_hex);

    capture_stderr_start();
    sg_cli_report_ambiguous_oid(git_dir, prefix, SG_REV_STRICT);
    out = capture_stderr_end();

    /* Reaching this line at all -- especially under `make sanitize` -- IS
       the assertion that matters here. The content checks below just
       confirm the SHAPE this project's existing fallback rule produces. */
    CHECK(count_occurrences(out, "hint:   ") == 2,
         "STRICT must still list both candidates (the real commit and the unreadable one, "
         "folded to blob), got:\n%s", out);
    CHECK(strstr(out, " commit ") != NULL, "the real commit's row must still print, got:\n%s", out);
    CHECK(strstr(out, " blob\n") != NULL,
         "the unreadable candidate must print via the plain blob fallback (confirms it never "
         "reached print_commit_candidate_line), got:\n%s", out);

    free(out);
    free(git_dir);
}

int main(void)
{
    test_committish_resolves_single_commitish();
    test_committish_ambiguous_with_two_commits();
    test_committish_peels_tag();
    test_committish_ambiguous_commit_and_tag();
    test_suffix_forces_committish_regardless_of_caller();
    test_ref_beats_prefix();
    test_object_prefix_any_type_and_dash4();
    test_no_match_is_dash1_not_dash4();
    test_treeish_resolves_commit_candidate_too();
    test_treeish_ambiguous_with_commit_and_tree();
    test_treeish_unique_bare_tree_still_fails();
    test_committish_excludes_tag_to_blob();
    test_treeish_excludes_tag_to_blob_but_still_ambiguous();
    test_report_ambiguous_oid_filters_by_peeled_type();
    test_matches_disambig_excludes_broken_tag_chain();
    test_report_ambiguous_oid_survives_unreadable_candidate();

    if (failures == 0) {
        printf("all revparse abbrev tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d failures\n", failures);
    return 1;
}
