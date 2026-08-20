#include "sg/workdir.h"

#include "sg/apply.h"
#include "sg/cli.h"
#include "sg/hash.h"
#include "sg/index.h"
#include "sg/loose.h"
#include "sg/object.h"
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
    const char *bad[] = {
        "",       ".",      "..",     "a/b",  ".git",  ".GIT",
        ".Git",   ".git.",  ".git ",  ".git..", ".git  ",
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

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all path_safe tests passed\n");
    return 0;
}
