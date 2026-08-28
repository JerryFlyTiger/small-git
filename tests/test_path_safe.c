#include "sg/workdir.h"

#include "sg/apply.h"
#include "sg/cli.h"
#include "sg/diff.h"
#include "sg/diff_out.h"
#include "sg/hash.h"
#include "sg/index.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/repo.h"
#include "sg/tree_build.h"

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

static char *make_tmp_repo(const char *tag)
{
    char template[4096];
    char *path;
    char git_dir[4096];

    snprintf(template, sizeof(template), "/tmp/sg_%s_test_XXXXXX", tag);
    path = strdup(template);
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

/* --- sg_path_component_is_safe / sg_relpath_is_safe: direct unit tests --- */

static void test_component_rejects(void)
{
    /* The last group folds away code points HFS+ ignores when comparing
       names, so ".g<U+200C>it" can BE ".git" on such a volume. The exact
       set was measured against git 2.55.0 by feeding ".g<cp>it" to git add
       -- see the accept list for the control group that pins it as a
       specific set rather than "anything invisible". The two mixed forms
       at the end matter because the trailing-'.'-and-space fold and the
       ignorable fold have to compose: stripping only one of them leaves
       the other as a way through. */
    const char *bad[] = {
        "",       ".",      "..",     "a/b",  ".git",  ".GIT",
        ".Git",   ".git.",  ".git ",  ".git..", ".git  ",
        ".g\u200cit", ".g\u200dit", ".g\u200eit", ".g\u200fit",
        ".g\u202ait", ".g\u202eit", ".g\u206ait", ".g\u206fit",
        ".git\u200c", "\ufeff.git", ".git.\u200c", ".git\u200c.",
    };
    size_t i;

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(sg_path_component_is_safe(bad[i]) == 0, "expected \"%s\" to be rejected", bad[i]);
}

static void test_component_accepts(void)
{
    /* ".gitignore"/".gitmodules"/"..a"/"a.." prove the comparison is on the
       whole component, never a prefix or substring match -- a strstr(name,
       "git") or strncmp(name, ".git", 4) implementation would wrongly
       reject some of these. "git~1" (NTFS 8.3 short name) is a perfectly
       ordinary filename on macOS/Linux, the only platforms sg supports, and
       must not be caught by an over-broad rule aimed at a Windows-only
       concern sg does not have. A name with an embedded control character
       must also be accepted: real git accepts it in tree entries and
       defends at the display layer instead (measured against git 2.55.0),
       so rejecting it here would make sg refuse a tree real git considers
       perfectly legal. */
    const char *good[] = {
        "ok.txt", ".gitignore", ".gitmodules", "..a", "a..", "git~1", "has\ttab", "has\x01ctrl",
        /* Control group for the ignorable-code-point fold: these are just
           as invisible as U+200C but real git ACCEPTS them (measured), so
           they pin the fold to a specific list. Widening it to "anything
           zero-width" would reject these and this is what would notice. */
        ".g\u200bit", ".g\u2060it", ".g\u00a0it", ".g\u3000it", ".gitx", "....",
    };
    size_t i;

    for (i = 0; i < sizeof(good) / sizeof(good[0]); i++)
        CHECK(sg_path_component_is_safe(good[i]) == 1, "expected \"%s\" to be accepted", good[i]);
}

static void test_relpath_rejects(void)
{
    const char *bad[] = {
        "", "/a", "a/", "a//b", "a/.git/b", "a/../b", ".git", "a/..",
    };
    size_t i;

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(sg_relpath_is_safe(bad[i]) == 0, "expected \"%s\" to be rejected", bad[i]);
}

static void test_relpath_accepts(void)
{
    const char *good[] = {
        "a", "a/b", "a/b/c", ".gitignore", "d/..a", "d/a..",
    };
    size_t i;

    for (i = 0; i < sizeof(good) / sizeof(good[0]); i++)
        CHECK(sg_relpath_is_safe(good[i]) == 1, "expected \"%s\" to be accepted", good[i]);
}

/* --- sg_tree_flatten: the tree-object-side guard --- */

static void write_blob(const char *git_dir, const char *content, unsigned char sha1_out[SG_SHA1_RAW_LEN])
{
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, (const unsigned char *)content, strlen(content),
                         sha1_out) == 0,
         "sg_loose_write(blob) failed");
}

/* Hand-builds a tree { ".git"(040000) -> { "hacked.txt" -> blob }, "normal.txt"
   -> blob } exactly the way a hostile/foreign commit could, bypassing sg's
   own tree builder entirely (sg_tree_serialize does not itself reject
   ".git" -- see docs/DESIGN.md, "not the parse layer" rationale). Mirrors
   the manual-tree-object recipe in tests/test_object.c. */
static void test_flatten_rejects_dotgit_entry(void)
{
    char *git_dir = make_tmp_repo("flatten_dotgit");
    unsigned char hacked_blob[SG_SHA1_RAW_LEN], normal_blob[SG_SHA1_RAW_LEN];
    unsigned char dotgit_tree[SG_SHA1_RAW_LEN], root_tree[SG_SHA1_RAW_LEN];
    sg_tree_entry inner[1];
    sg_tree_entry outer[2];
    unsigned char *serialized;
    size_t serialized_len;
    sg_flat_list flat;
    char bad_path[SG_PATH_MAX];
    int rc;

    write_blob(git_dir, "PWNED", hacked_blob);
    write_blob(git_dir, "hello\n", normal_blob);

    inner[0].mode = 0100644;
    inner[0].name = (char *)"hacked.txt";
    memcpy(inner[0].sha1, hacked_blob, SG_SHA1_RAW_LEN);
    CHECK(sg_tree_serialize(inner, 1, &serialized, &serialized_len) == 0, "serialize inner failed");
    CHECK(sg_loose_write(git_dir, SG_OBJ_TREE, serialized, serialized_len, dotgit_tree) == 0,
         "write inner tree failed");
    free(serialized);

    outer[0].mode = 040000;
    outer[0].name = (char *)".git";
    memcpy(outer[0].sha1, dotgit_tree, SG_SHA1_RAW_LEN);
    outer[1].mode = 0100644;
    outer[1].name = (char *)"normal.txt";
    memcpy(outer[1].sha1, normal_blob, SG_SHA1_RAW_LEN);
    CHECK(sg_tree_serialize(outer, 2, &serialized, &serialized_len) == 0, "serialize outer failed");
    CHECK(sg_loose_write(git_dir, SG_OBJ_TREE, serialized, serialized_len, root_tree) == 0,
         "write outer tree failed");
    free(serialized);

    memset(&flat, 0, sizeof(flat));
    rc = sg_tree_flatten(git_dir, root_tree, &flat, bad_path);
    CHECK(rc == -2, "expected sg_tree_flatten to return -2 for a hostile .git entry, got %d", rc);
    /* The ".git" entry itself fails sg_path_component_is_safe before
       flatten_into ever recurses into it, so the reported path is ".git",
       not ".git/hacked.txt" -- the guard never needs to look inside a
       directory entry it has already rejected. */
    CHECK(strcmp(bad_path, ".git") == 0, "expected bad_path \".git\", got \"%s\"", bad_path);

    free(git_dir);
}

/* The length bound in flatten_into, which is also what keeps its recursion
   from running the stack out on a deeply nested hostile tree. A single
   4095-byte entry name is enough to cross it: prefix_len(0) + name_len +
   1 >= SG_PATH_MAX. Nothing else reaches this branch -- the component guard
   has no opinion on length, so without this test the bound could be deleted
   and every other check would stay green. */
static void test_flatten_rejects_an_overlong_path(void)
{
    char *git_dir = make_tmp_repo("flatten_long");
    unsigned char blob[SG_SHA1_RAW_LEN], root_tree[SG_SHA1_RAW_LEN];
    sg_tree_entry entry[1];
    unsigned char *serialized;
    size_t serialized_len;
    sg_flat_list flat;
    char bad_path[SG_PATH_MAX];
    char *huge = malloc(SG_PATH_MAX);
    int rc;

    if (huge == NULL) {
        fprintf(stderr, "malloc failed\n");
        exit(1);
    }
    memset(huge, 'a', SG_PATH_MAX - 1);
    huge[SG_PATH_MAX - 1] = '\0';

    write_blob(git_dir, "x\n", blob);
    entry[0].mode = 0100644;
    entry[0].name = huge;
    memcpy(entry[0].sha1, blob, SG_SHA1_RAW_LEN);
    CHECK(sg_tree_serialize(entry, 1, &serialized, &serialized_len) == 0, "serialize failed");
    CHECK(sg_loose_write(git_dir, SG_OBJ_TREE, serialized, serialized_len, root_tree) == 0,
         "write tree failed");
    free(serialized);

    /* The name itself is a perfectly ordinary component -- no slash, not
       ".git" -- so a -2 here can only have come from the length bound. */
    CHECK(sg_path_component_is_safe(huge) == 1,
         "precondition: the name must pass the component guard, so that -2 pins the length bound");

    memset(&flat, 0, sizeof(flat));
    rc = sg_tree_flatten(git_dir, root_tree, &flat, bad_path);
    CHECK(rc == -2, "expected -2 for a path at SG_PATH_MAX, got %d", rc);

    free(huge);
    free(git_dir);
}

/* A tree containing an entry with an embedded control character must flatten
   successfully -- real git accepts these (measured), and the display-layer
   defense is Phase 23's job, not this guard's. */
static void test_flatten_accepts_control_char_entry(void)
{
    char *git_dir = make_tmp_repo("flatten_ctrl");
    unsigned char blob_id[SG_SHA1_RAW_LEN];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    sg_tree_entry entries[1];
    unsigned char *serialized;
    size_t serialized_len;
    sg_flat_list flat;
    int rc;

    write_blob(git_dir, "content", blob_id);
    entries[0].mode = 0100644;
    entries[0].name = (char *)"weird\x01name";
    memcpy(entries[0].sha1, blob_id, SG_SHA1_RAW_LEN);
    CHECK(sg_tree_serialize(entries, 1, &serialized, &serialized_len) == 0, "serialize failed");
    CHECK(sg_loose_write(git_dir, SG_OBJ_TREE, serialized, serialized_len, tree_id) == 0,
         "write tree failed");
    free(serialized);

    memset(&flat, 0, sizeof(flat));
    rc = sg_tree_flatten(git_dir, tree_id, &flat, NULL);
    CHECK(rc == 0, "expected control-char entry to flatten cleanly, got %d", rc);
    CHECK(flat.count == 1, "expected 1 entry, got %zu", flat.count);
    sg_flat_list_free(&flat);

    free(git_dir);
}

/* --- apply.c's remove() guard: the index-side guard ---

   Deliberately NOT built by first applying a hostile tree: with the
   flatten-side guard above in place, that step would itself be refused and
   the index would come out clean -- a test built that way would pass for
   the wrong reason (nothing ever reached the remove() guard at all). The
   fixture instead writes a malicious entry ("../victim.txt") straight into
   the on-disk index with sg_index_upsert/sg_index_write, modelling an index
   left behind by an sg build that predates this guard.

   The target tree passed to sg_apply_tree_to_workdir must also NOT contain
   "../victim.txt": if it did, flat_find would report it present and the
   removal branch -- the one this guard protects -- would never run at all,
   again passing for the wrong reason. An empty tree guarantees the
   stale-and-malicious index entry takes the removal path. */
static void test_apply_remove_guard_skips_escaping_index_entry(void)
{
    char outer_template[] = "/tmp/sg_apply_escape_test_XXXXXX";
    char *outer;
    char repo_path[4096];
    char git_dir[4096];
    char victim_path[4096];
    unsigned char empty_tree[SG_SHA1_RAW_LEN];
    sg_index idx;
    sg_index_entry entry;
    unsigned char fake_sha1[SG_SHA1_RAW_LEN];
    /* Initialised because the failure path below reads it: sg_read_file
       leaves *out untouched when it fails, which is exactly what happens
       when a mutation disables the guard and victim.txt really is deleted.
       Left uninitialised, the free() below would abort on a garbage pointer
       and bury the two assertions that had already reported correctly. */
    unsigned char *victim_content = NULL;
    size_t victim_len = 0;

    outer = strdup(outer_template);
    if (mkdtemp(outer) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        exit(1);
    }
    snprintf(repo_path, sizeof(repo_path), "%s/repo", outer);
    CHECK(mkdir(repo_path, 0755) == 0, "mkdir repo failed");
    CHECK(sg_repo_init(repo_path) == 0, "sg_repo_init failed");
    snprintf(git_dir, sizeof(git_dir), "%s/.git", repo_path);

    /* victim.txt sits OUTSIDE the repo (a sibling of repo/), reachable from
       repo_path only via "../victim.txt". */
    snprintf(victim_path, sizeof(victim_path), "%s/victim.txt", outer);
    CHECK(sg_write_file_mkdirs(victim_path, (const unsigned char *)"SAFE", 4, 0644) == 0,
         "failed to write victim fixture");

    memset(fake_sha1, 0xAB, sizeof(fake_sha1));
    memset(&idx, 0, sizeof(idx));
    memset(&entry, 0, sizeof(entry));
    entry.mode = 0100644;
    memcpy(entry.sha1, fake_sha1, SG_SHA1_RAW_LEN);
    entry.path = (char *)"../victim.txt";
    CHECK(sg_index_upsert(&idx, &entry) == 0, "sg_index_upsert failed");
    CHECK(sg_index_write(git_dir, &idx) == 0, "sg_index_write failed");
    sg_index_free(&idx);

    CHECK(sg_tree_build(git_dir, NULL, 0, empty_tree) == 0, "failed to build empty tree");

    /* Return value is not asserted here: sg_apply_tree_to_workdir reports -1
       either way once the guard fires (it also skips the index rewrite), and
       that -1 alone would not distinguish "the guard fired" from "some
       unrelated failure happened". The property this test exists to prove
       is entirely about the filesystem: victim.txt, outside the repository,
       must survive untouched. */
    sg_apply_tree_to_workdir(git_dir, repo_path, empty_tree);

    CHECK(sg_read_file(victim_path, &victim_content, &victim_len) == 0,
         "victim.txt outside the repo was removed");
    if (victim_content != NULL) {
        CHECK(victim_len == 4 && memcmp(victim_content, "SAFE", 4) == 0,
             "victim.txt outside the repo was overwritten");
        free(victim_content);
    }

    free(outer);
}

/* --- cmd_add.c's guard: the command-line-argument guard --- */

static void test_cmd_add_rejects_dotgit_arg(void)
{
    char template[] = "/tmp/sg_add_dotgit_test_XXXXXX";
    char *path = strdup(template);
    sg_index idx;
    char *argv[3];
    int rc;

    if (mkdtemp(path) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        exit(1);
    }
    CHECK(sg_repo_init(path) == 0, "sg_repo_init failed");
    CHECK(chdir(path) == 0, "chdir failed");

    CHECK(mkdir("d", 0755) == 0, "mkdir d failed");
    CHECK(mkdir("d/.git", 0755) == 0, "mkdir d/.git failed");
    CHECK(sg_write_file_mkdirs("d/.git/evil", (const unsigned char *)"evil", 4, 0644) == 0,
         "failed to write d/.git/evil fixture");

    argv[0] = (char *)"add";
    argv[1] = (char *)"-f";
    argv[2] = (char *)"d/.git/evil";
    rc = sg_cmd_add(3, argv);
    CHECK(rc != 0, "expected `sg add -f d/.git/evil` to exit non-zero, got %d", rc);

    {
        char git_dir[4096];

        snprintf(git_dir, sizeof(git_dir), "%s/.git", path);
        CHECK(sg_index_read(git_dir, &idx) == 0, "sg_index_read failed");
        CHECK(sg_index_find(&idx, "d/.git/evil") < 0, "d/.git/evil must not be staged");
        sg_index_free(&idx);
    }

    free(path);
}

/* The per-component length bound in sg_relpath_is_safe, which guards the
   memcpy into its fixed stack buffer. A component of exactly SG_PATH_MAX
   bytes is the first length that must be refused: one byte shorter still
   leaves room for the NUL. Nothing else in the suite feeds this function a
   component anywhere near that size -- the other overlong test exercises
   flatten_into's cumulative-path bound, which is a different check. */
static void test_relpath_rejects_an_oversized_component(void)
{
    char *huge = malloc(SG_PATH_MAX + 8);
    size_t i;

    if (huge == NULL) {
        fprintf(stderr, "malloc failed\n");
        exit(1);
    }
    for (i = 0; i < SG_PATH_MAX; i++)
        huge[i] = 'a';
    huge[SG_PATH_MAX] = '\0';
    CHECK(sg_relpath_is_safe(huge) == 0,
         "a component of exactly SG_PATH_MAX bytes must be refused");

    huge[SG_PATH_MAX - 1] = '\0';
    CHECK(sg_relpath_is_safe(huge) == 1,
         "one byte shorter still fits and must be accepted -- this is what pins the bound "
         "to the boundary rather than to \"something long\"");

    free(huge);
}

/* sg restore writes blob content to the working tree using a path taken
   straight from the index, and the index is not a trusted source: an sg
   predating the add-side guard staged ".git/..." paths happily, and those
   entries are still there afterwards. Measured before the guard existed:
   `sg restore .git/hooks/evil` wrote PWNED into the real gitdir, exit 0.
   That is strictly worse than the delete-side hole in apply.c -- this one
   writes chosen bytes rather than only removing. */
static void test_restore_refuses_a_dotgit_index_entry(void)
{
    char template[] = "/tmp/sg_restore_dotgit_test_XXXXXX";
    char *path = strdup(template);
    char git_dir[SG_PATH_MAX];
    sg_index idx;
    sg_index_entry e;
    unsigned char blob[SG_SHA1_RAW_LEN];
    struct stat st;
    char *argv[2];
    int rc;

    if (mkdtemp(path) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        exit(1);
    }
    CHECK(sg_repo_init(path) == 0, "sg_repo_init failed");
    CHECK(chdir(path) == 0, "chdir failed");
    snprintf(git_dir, sizeof(git_dir), "%s/.git", path);

    /* The index is written directly rather than through `sg add`, because
       the add-side guard now refuses this path -- staging it first would
       make the fixture a no-op and the test would pass for the wrong
       reason. An index like this is what an sg predating that guard left
       behind, and it survives the upgrade. */
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, (const unsigned char *)"PWNED\n", 6, blob) == 0,
         "sg_loose_write failed");
    memset(&idx, 0, sizeof(idx));
    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, blob, SG_SHA1_RAW_LEN);
    e.path = (char *)".git/hooks/evil";
    CHECK(sg_index_upsert(&idx, &e) == 0, "sg_index_upsert failed");
    CHECK(sg_index_write(git_dir, &idx) == 0, "sg_index_write failed");
    sg_index_free(&idx);

    argv[0] = (char *)"restore";
    argv[1] = (char *)".git/hooks/evil";
    rc = sg_cmd_restore(2, argv);
    CHECK(rc != 0, "sg restore must refuse a .git path from the index, got exit %d", rc);
    CHECK(stat(".git/hooks/evil", &st) != 0, "sg restore wrote into the gitdir");

    free(path);
}

/* --- tree_build.c's sg_tree_build_from_workdir guard: the read-side guard
   that permanently writes an object (Phase 36) ---

   Built the same way as test_apply_remove_guard_skips_escaping_index_entry
   above: sg_index_upsert/sg_index_write straight onto disk, modelling a
   .git/index an attacker (or an sg predating this guard) left behind --
   sg_index_read validates nothing about entry paths, by design.

   The property under test is NOT the return code alone: before Phase 36,
   sg_tree_build_from_workdir's caller (sg_stash_push) already failed with a
   non-zero exit further down its own pipeline (the remove-side guard in
   apply.c rejects the same path when the stash tries to reset the working
   directory back to HEAD) while STILL having permanently written the
   external file's content as a loose object first. A test that only checked
   "did the call fail" would have stayed green through that entire bug --
   the assertion has to be that the blob naming the external content's hash
   never lands in the object store. */
static void test_tree_build_from_workdir_refuses_escaping_index_entry(void)
{
    char outer_template[] = "/tmp/sg_treebuild_escape_test_XXXXXX";
    char *outer;
    char repo_path[4096];
    char git_dir[SG_PATH_MAX];
    char secret_path[4096];
    static const char secret_content[] = "TOP-SECRET-API-KEY-abc123";
    unsigned char secret_blob_id[SG_SHA1_RAW_LEN];
    unsigned char fake_sha1[SG_SHA1_RAW_LEN];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    sg_index idx;
    sg_index_entry entry;
    unsigned char *readback = NULL;
    size_t readback_len = 0;
    sg_obj_type readback_type;
    char bad_path[SG_PATH_MAX];
    int rc;

    outer = strdup(outer_template);
    if (mkdtemp(outer) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        exit(1);
    }
    snprintf(repo_path, sizeof(repo_path), "%s/repo", outer);
    CHECK(mkdir(repo_path, 0755) == 0, "mkdir repo failed");
    CHECK(sg_repo_init(repo_path) == 0, "sg_repo_init failed");
    snprintf(git_dir, sizeof(git_dir), "%s/.git", repo_path);

    /* secret.txt sits OUTSIDE the repo, reachable only via "../secret.txt",
       exactly the p36c reproducer's fixture. */
    snprintf(secret_path, sizeof(secret_path), "%s/secret.txt", outer);
    CHECK(sg_write_file_mkdirs(secret_path, (const unsigned char *)secret_content,
                               strlen(secret_content), 0644) == 0,
         "failed to write secret fixture");
    /* What the guard must prevent from ever reaching the object store --
       computed independently of sg_tree_build_from_workdir so the assertion
       below is not just "the function's own idea of the hash". */
    CHECK(sg_hash_file_blob(secret_path, secret_blob_id) == 0,
         "failed to hash the secret fixture");

    memset(fake_sha1, 0xCD, sizeof(fake_sha1));
    memset(&idx, 0, sizeof(idx));
    memset(&entry, 0, sizeof(entry));
    entry.mode = 0100644;
    memcpy(entry.sha1, fake_sha1, SG_SHA1_RAW_LEN);
    entry.path = (char *)"../secret.txt";
    CHECK(sg_index_upsert(&idx, &entry) == 0, "sg_index_upsert failed");

    bad_path[0] = '\0';
    rc = sg_tree_build_from_workdir(git_dir, repo_path, &idx, SG_WORKDIR_MISSING_KEEP_INDEX_BLOB,
                                    NULL, tree_id, bad_path);
    CHECK(rc != 0, "expected sg_tree_build_from_workdir to refuse an escaping index path, got %d",
         rc);
    /* Phase 36 follow-up: bad_path must name the actual offending path, not
       be left empty -- this is what lets sg_stash_push/cmd_stash.c print an
       actionable message instead of a guess. */
    CHECK(strcmp(bad_path, "../secret.txt") == 0,
         "expected bad_path to be \"../secret.txt\", got \"%s\"", bad_path);

    /* The load-bearing assertion: the secret's content must never have been
       written as a loose object, escaping-path guard or not. */
    rc = sg_object_read(git_dir, secret_blob_id, &readback_type, &readback, &readback_len);
    CHECK(rc != 0, "the external file's content was written into the object store");
    free(readback);

    sg_index_free(&idx);
    free(outer);
}

/* --- diff.c's index-vs-workdir guard: content must not be read, but the
   path must still be reportable (Phase 36) ---

   Unlike the tree_build guard above, this one must NOT hard-fail the whole
   call: `sg status`/`sg diff` still need to be able to list a path like this
   (real git does too, from the staged half of the same status, which never
   reaches this code at all). The property under test is that the working
   tree file's content is never read: sg_diff_index_workdir must report the
   row (if any) with new_side ABSENT, the same convention already used for
   "existing but unreadable", rather than a WORKDIR side carrying the
   external file's real hash. */
static void test_diff_index_workdir_refuses_escaping_index_entry(void)
{
    char outer_template[] = "/tmp/sg_diffworkdir_escape_test_XXXXXX";
    char *outer;
    char repo_path[4096];
    char git_dir[SG_PATH_MAX];
    char secret_path[4096];
    static const char secret_content[] = "TOP-SECRET-API-KEY-abc123";
    unsigned char secret_blob_id[SG_SHA1_RAW_LEN];
    unsigned char fake_sha1[SG_SHA1_RAW_LEN];
    sg_index idx;
    sg_index_entry entry;
    sg_diff_list dl;
    int rc;
    size_t i;

    outer = strdup(outer_template);
    if (mkdtemp(outer) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        exit(1);
    }
    snprintf(repo_path, sizeof(repo_path), "%s/repo", outer);
    CHECK(mkdir(repo_path, 0755) == 0, "mkdir repo failed");
    CHECK(sg_repo_init(repo_path) == 0, "sg_repo_init failed");
    snprintf(git_dir, sizeof(git_dir), "%s/.git", repo_path);

    snprintf(secret_path, sizeof(secret_path), "%s/secret.txt", outer);
    CHECK(sg_write_file_mkdirs(secret_path, (const unsigned char *)secret_content,
                               strlen(secret_content), 0644) == 0,
         "failed to write secret fixture");
    CHECK(sg_hash_file_blob(secret_path, secret_blob_id) == 0,
         "failed to hash the secret fixture");

    /* fake_sha1 deliberately does NOT equal secret_blob_id: if the guard
       failed and the file were actually read+hashed, the row's new_side
       would carry secret_blob_id, which the assertion below checks for
       directly -- a stronger property than "some row got appended". */
    memset(fake_sha1, 0xCD, sizeof(fake_sha1));
    memset(&idx, 0, sizeof(idx));
    memset(&entry, 0, sizeof(entry));
    entry.mode = 0100644;
    memcpy(entry.sha1, fake_sha1, SG_SHA1_RAW_LEN);
    entry.path = (char *)"../secret.txt";
    CHECK(sg_index_upsert(&idx, &entry) == 0, "sg_index_upsert failed");

    rc = sg_diff_index_workdir(git_dir, repo_path, &idx, &dl);
    CHECK(rc == 0, "expected sg_diff_index_workdir to succeed (never a hard failure), got %d", rc);

    for (i = 0; i < dl.count; i++) {
        const sg_diff_entry *e = &dl.entries[i];

        if (strcmp(e->path, "../secret.txt") != 0)
            continue;
        CHECK(e->new_side.kind != SG_DIFF_SIDE_WORKDIR ||
                 memcmp(e->new_side.id, secret_blob_id, SG_SHA1_RAW_LEN) != 0,
             "the external file's real content hash leaked into the diff row");
    }

    sg_diff_list_free(&dl);
    sg_index_free(&idx);
    free(outer);
}

/* --- diff.c's tree-vs-workdir guard: the third, separately-guarded call
   site (Phase 36) ---

   sg_diff_tree_workdir's "in the index, not in the tree" branch is a THIRD
   place that reads a working-tree file at an untrusted index path, distinct
   from both guards above: sg_diff_index_workdir (index-vs-workdir, what `sg
   status`/plain `sg diff` use) never runs this function at all, and
   sg_tree_build_from_workdir never runs this comparison logic either. A
   directed mutation that deletes ONLY this guard leaves the other two
   fully intact and green -- proving this test is not redundant with either
   of the tests above. old_tree is NULL (empty tree), so the malicious path
   necessarily takes the "in index, not in tree" branch, never the
   "present in both" branch the other two guards would also protect. */
static void test_diff_tree_workdir_refuses_escaping_index_entry(void)
{
    char outer_template[] = "/tmp/sg_difftreewd_escape_test_XXXXXX";
    char *outer;
    char repo_path[4096];
    char git_dir[SG_PATH_MAX];
    char secret_path[4096];
    static const char secret_content[] = "TOP-SECRET-API-KEY-abc123";
    unsigned char secret_blob_id[SG_SHA1_RAW_LEN];
    unsigned char fake_sha1[SG_SHA1_RAW_LEN];
    sg_index idx;
    sg_index_entry entry;
    sg_diff_list dl;
    int rc;
    size_t i;

    outer = strdup(outer_template);
    if (mkdtemp(outer) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        exit(1);
    }
    snprintf(repo_path, sizeof(repo_path), "%s/repo", outer);
    CHECK(mkdir(repo_path, 0755) == 0, "mkdir repo failed");
    CHECK(sg_repo_init(repo_path) == 0, "sg_repo_init failed");
    snprintf(git_dir, sizeof(git_dir), "%s/.git", repo_path);

    snprintf(secret_path, sizeof(secret_path), "%s/secret.txt", outer);
    CHECK(sg_write_file_mkdirs(secret_path, (const unsigned char *)secret_content,
                               strlen(secret_content), 0644) == 0,
         "failed to write secret fixture");
    CHECK(sg_hash_file_blob(secret_path, secret_blob_id) == 0,
         "failed to hash the secret fixture");

    memset(fake_sha1, 0xCD, sizeof(fake_sha1));
    memset(&idx, 0, sizeof(idx));
    memset(&entry, 0, sizeof(entry));
    entry.mode = 0100644;
    memcpy(entry.sha1, fake_sha1, SG_SHA1_RAW_LEN);
    entry.path = (char *)"../secret.txt";
    CHECK(sg_index_upsert(&idx, &entry) == 0, "sg_index_upsert failed");

    rc = sg_diff_tree_workdir(git_dir, repo_path, NULL, &idx, &dl, NULL);
    CHECK(rc == 0, "expected sg_diff_tree_workdir to succeed (never a hard failure), got %d", rc);

    for (i = 0; i < dl.count; i++) {
        const sg_diff_entry *e = &dl.entries[i];

        if (strcmp(e->path, "../secret.txt") != 0)
            continue;
        CHECK(e->new_side.kind != SG_DIFF_SIDE_WORKDIR ||
                 memcmp(e->new_side.id, secret_blob_id, SG_SHA1_RAW_LEN) != 0,
             "the external file's real content hash leaked into the diff row");
    }

    sg_diff_list_free(&dl);
    sg_index_free(&idx);
    free(outer);
}

/* --- diff.c's build_result_side guard: the FOURTH, separately-guarded
   call site (reviewer-found blind spot, Phase 36 follow-up) ---

   build_result_side is only reached through the UNMERGED branch of
   sg_diff_index_workdir (idx->entries[i].stage != 0), building the
   "result" side of a conflict row -- i.e. what a combined diff (`sg diff
   -c`/`--cc`) shows as the resolved/working-tree content. None of the
   other three Phase 36 tests exercise this: they all use a stage-0 entry,
   which never reaches this function at all (confirmed by running this
   exact mutation BEFORE this test existed: `tests/mutate.sh` reported exit
   code 0, no FAIL lines -- a real blind spot, not a redundant guard).

   The consequence of that blind spot is worse than the other three: this
   is the one guard whose failure prints the external file's ACTUAL BYTES
   into a diff hunk, not just a content hash. The assertion below has to
   look at the rendered combined-diff TEXT, not a struct field, to catch
   that. */
static void test_build_result_side_refuses_escaping_index_entry(void)
{
    char outer_template[] = "/tmp/sg_buildresult_escape_test_XXXXXX";
    char *outer;
    char repo_path[4096];
    char git_dir[SG_PATH_MAX];
    char secret_path[4096];
    static const char secret_content[] = "TOP-SECRET-COMBINED-CONTENT-xyz789";
    unsigned char ours_blob[SG_SHA1_RAW_LEN], theirs_blob[SG_SHA1_RAW_LEN];
    sg_index idx;
    sg_index_entry e2, e3;
    sg_diff_list dl;
    sg_diff_out_opts opts;
    char *rendered;
    int rc;
    int saved_stdout;
    char capture_path[] = "/tmp/sg_buildresult_capture_XXXXXX";
    int fd;
    FILE *f;
    long len;

    outer = strdup(outer_template);
    if (mkdtemp(outer) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        exit(1);
    }
    snprintf(repo_path, sizeof(repo_path), "%s/repo", outer);
    CHECK(mkdir(repo_path, 0755) == 0, "mkdir repo failed");
    CHECK(sg_repo_init(repo_path) == 0, "sg_repo_init failed");
    snprintf(git_dir, sizeof(git_dir), "%s/.git", repo_path);

    /* The escaping path's actual on-disk content -- what a broken guard
       would print verbatim into the combined-diff hunk. */
    snprintf(secret_path, sizeof(secret_path), "%s/conflict.txt", outer);
    CHECK(sg_write_file_mkdirs(secret_path, (const unsigned char *)secret_content,
                               strlen(secret_content), 0644) == 0,
         "failed to write secret fixture");

    /* Real blobs for ours/theirs, so render_combined_patch's own
       sg_diff_side_read calls for THOSE two sides succeed normally -- only
       the result side (the guarded one) is untrusted here. */
    write_blob(git_dir, "OURS\nline\n", ours_blob);
    write_blob(git_dir, "THEIRS\nline\n", theirs_blob);

    memset(&idx, 0, sizeof(idx));
    memset(&e2, 0, sizeof(e2));
    e2.mode = 0100644;
    e2.stage = 2;
    memcpy(e2.sha1, ours_blob, SG_SHA1_RAW_LEN);
    e2.path = (char *)"../conflict.txt";
    CHECK(sg_index_upsert(&idx, &e2) == 0, "sg_index_upsert(stage 2) failed");

    memset(&e3, 0, sizeof(e3));
    e3.mode = 0100644;
    e3.stage = 3;
    memcpy(e3.sha1, theirs_blob, SG_SHA1_RAW_LEN);
    e3.path = (char *)"../conflict.txt";
    CHECK(sg_index_upsert(&idx, &e3) == 0, "sg_index_upsert(stage 3) failed");

    rc = sg_diff_index_workdir(git_dir, repo_path, &idx, &dl);
    CHECK(rc == 0, "expected sg_diff_index_workdir to succeed (never a hard failure), got %d", rc);

    /* Render the combined diff to a captured file the same way
       tests/test_diff_out.c does -- the property under test lives in the
       rendered TEXT (render_combined_patch re-reads e->result's path via
       sg_diff_side_read), not in any in-memory struct field. */
    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH; /* PATCH's implicit default is dense combined */

    rendered = NULL;
    fflush(stdout);
    fd = mkstemp(capture_path);
    CHECK(fd >= 0, "mkstemp for capture failed");
    if (fd >= 0) {
        saved_stdout = dup(STDOUT_FILENO);
        CHECK(saved_stdout >= 0, "dup(STDOUT_FILENO) failed");
        if (saved_stdout >= 0) {
            dup2(fd, STDOUT_FILENO);
            close(fd);

            sg_diff_print(git_dir, repo_path, &dl, &opts);

            fflush(stdout);
            dup2(saved_stdout, STDOUT_FILENO);
            close(saved_stdout);
        } else {
            /* stdout was never redirected, so there is nothing captured to
               read back -- close fd here since the dup2/close pair above
               that would normally own it never ran. */
            close(fd);
        }
    }

    f = (fd >= 0) ? fopen(capture_path, "rb") : NULL;
    CHECK(f != NULL, "failed to reopen capture file");
    if (f != NULL) {
        fseek(f, 0, SEEK_END);
        len = ftell(f);
        fseek(f, 0, SEEK_SET);
        rendered = malloc((size_t)len + 1);
        if (rendered != NULL) {
            if (len > 0 && fread(rendered, 1, (size_t)len, f) != (size_t)len) {
                fprintf(stderr, "short read on capture file\n");
                exit(1);
            }
            rendered[len] = '\0';
        }
        fclose(f);
    }
    unlink(capture_path);

    CHECK(rendered != NULL, "no rendered output captured");
    if (rendered != NULL) {
        CHECK(strstr(rendered, secret_content) == NULL,
             "the external file's real content was printed into the combined diff");
        free(rendered);
    }

    sg_diff_list_free(&dl);
    sg_index_free(&idx);
    free(outer);
}

int main(void)
{
    test_component_rejects();
    test_component_accepts();
    test_relpath_rejects();
    test_relpath_accepts();
    test_flatten_rejects_dotgit_entry();
    test_flatten_rejects_an_overlong_path();
    test_flatten_accepts_control_char_entry();
    test_apply_remove_guard_skips_escaping_index_entry();
    test_cmd_add_rejects_dotgit_arg();
    test_relpath_rejects_an_oversized_component();
    test_restore_refuses_a_dotgit_index_entry();
    test_tree_build_from_workdir_refuses_escaping_index_entry();
    test_diff_index_workdir_refuses_escaping_index_entry();
    test_diff_tree_workdir_refuses_escaping_index_entry();
    test_build_result_side_refuses_escaping_index_entry();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all path_safe tests passed\n");
    return 0;
}
