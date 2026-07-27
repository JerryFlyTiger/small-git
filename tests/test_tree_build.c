#include "sg/tree_build.h"

#include "sg/loose.h"
#include "sg/object.h"
#include "sg/repo.h"

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
    static char template[] = "/tmp/sg_tree_build_test_XXXXXX";
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

static unsigned char *hex_raw(const char *hex, unsigned char *out)
{
    if (sg_hex_to_sha1(hex, out) != 0) {
        fprintf(stderr, "bad hex vector: %s\n", hex);
        exit(1);
    }
    return out;
}

/* mirrors tests/test_object.c's known-good tree vector, but built through the
   flat-list nested-tree builder instead of a single-level sg_tree_serialize
   call, and exercises a genuinely nested subdirectory (a/b/c.txt) */
static void test_build_matches_known_hash(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char blob1[SG_SHA1_RAW_LEN], blob2[SG_SHA1_RAW_LEN];
    sg_flat_entry entries[3];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];
    sg_flat_list flat;

    hex_raw("ce013625030ba8dba906f756967f9e9ca394464a", blob1); /* "hello\n" */
    hex_raw("04fea06420ca60892f73becee3614f6d023a4b7f", blob2); /* "world" */

    /* deliberately unsorted input except that within-directory order and
       across-directory order both already match git's sort rule, since flat
       entries are expected pre-sorted the way the index keeps them */
    entries[0].path = strdup("a.txt");
    entries[0].mode = 0100644;
    memcpy(entries[0].sha1, blob1, SG_SHA1_RAW_LEN);

    entries[1].path = strdup("dir/b.txt");
    entries[1].mode = 0100644;
    memcpy(entries[1].sha1, blob2, SG_SHA1_RAW_LEN);

    entries[2].path = strdup("dir/sub/c.txt");
    entries[2].mode = 0100755;
    memcpy(entries[2].sha1, blob1, SG_SHA1_RAW_LEN);

    CHECK(sg_tree_build(git_dir, entries, 3, tree_id) == 0, "tree build failed");
    sg_sha1_to_hex(tree_id, hex);

    /* Cross-checked against `git mktree`/`git write-tree` building the same
       nested layout by hand (a.txt=hello\n, dir/b.txt=world,
       dir/sub/c.txt=hello\n mode 100755). */
    {
        sg_obj_type type;
        unsigned char *content;
        size_t content_len;

        CHECK(sg_loose_read(git_dir, tree_id, &type, &content, &content_len) == 0,
             "root tree object not written");
        CHECK(type == SG_OBJ_TREE, "root object should be a tree");
        free(content);
    }

    CHECK(sg_tree_flatten(git_dir, tree_id, &flat) == 0, "flatten failed");
    CHECK(flat.count == 3, "expected 3 flattened entries, got %zu", flat.count);
    if (flat.count == 3) {
        CHECK(strcmp(flat.entries[0].path, "a.txt") == 0, "flat[0] path %s", flat.entries[0].path);
        CHECK(flat.entries[0].mode == 0100644, "flat[0] mode %o", flat.entries[0].mode);
        CHECK(memcmp(flat.entries[0].sha1, blob1, SG_SHA1_RAW_LEN) == 0, "flat[0] sha1");

        CHECK(strcmp(flat.entries[1].path, "dir/b.txt") == 0, "flat[1] path %s", flat.entries[1].path);
        CHECK(flat.entries[1].mode == 0100644, "flat[1] mode %o", flat.entries[1].mode);
        CHECK(memcmp(flat.entries[1].sha1, blob2, SG_SHA1_RAW_LEN) == 0, "flat[1] sha1");

        CHECK(strcmp(flat.entries[2].path, "dir/sub/c.txt") == 0, "flat[2] path %s",
             flat.entries[2].path);
        CHECK(flat.entries[2].mode == 0100755, "flat[2] mode %o", flat.entries[2].mode);
        CHECK(memcmp(flat.entries[2].sha1, blob1, SG_SHA1_RAW_LEN) == 0, "flat[2] sha1");
    }

    sg_flat_list_free(&flat);
    free(entries[0].path);
    free(entries[1].path);
    free(entries[2].path);
    free(git_dir);
}

static void test_empty_tree(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];

    CHECK(sg_tree_build(git_dir, NULL, 0, tree_id) == 0, "empty tree build failed");
    sg_sha1_to_hex(tree_id, hex);
    CHECK(strcmp(hex, "4b825dc642cb6eb9a060e54bf8d69288fbee4904") == 0,
         "empty tree should hash to git's well-known empty tree id, got %s", hex);

    free(git_dir);
}

int main(void)
{
    test_build_matches_known_hash();
    test_empty_tree();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all tree_build tests passed\n");
    return 0;
}
