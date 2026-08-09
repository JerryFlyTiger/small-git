#include "sg/refs.h"

#include "sg/hash.h"
#include "sg/repo.h"
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
    static char template[] = "/tmp/sg_refs_test_XXXXXX";
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

static void fill_id(unsigned char id[SG_SHA1_RAW_LEN], unsigned char byte)
{
    memset(id, byte, SG_SHA1_RAW_LEN);
}

/* Writes a minimal packed-refs file (no '#' header line -- read_packed_ref/
   list_packed_under skip those but don't require one) with exactly the
   given "<hex> <refname>\n" lines, replacing whatever packed-refs already
   existed. */
static void write_packed_refs(const char *git_dir, const char *const *lines, size_t count)
{
    char path[4096];
    char *buf;
    size_t cap = 4096;
    size_t used = 0;
    size_t i;

    buf = malloc(cap);
    CHECK(buf != NULL, "oom building packed-refs content");
    for (i = 0; i < count; i++) {
        size_t len = strlen(lines[i]);

        while (used + len + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            CHECK(buf != NULL, "oom growing packed-refs content");
        }
        memcpy(buf + used, lines[i], len);
        used += len;
        buf[used++] = '\n';
    }

    snprintf(path, sizeof(path), "%s/packed-refs", git_dir);
    CHECK(sg_write_file_mkdirs(path, (const unsigned char *)buf, used, 0644) == 0,
         "failed to write packed-refs");
    free(buf);
}

static void hex_line(char *out, size_t out_len, const unsigned char id[SG_SHA1_RAW_LEN],
                     const char *ref_name)
{
    char hex[SG_SHA1_HEX_LEN + 1];

    sg_sha1_to_hex(id, hex);
    snprintf(out, out_len, "%s %s", hex, ref_name);
}

static int names_contains(char **names, size_t count, const char *name)
{
    size_t i;

    for (i = 0; i < count; i++) {
        if (strcmp(names[i], name) == 0)
            return 1;
    }
    return 0;
}

static void free_names(char **names, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++)
        free(names[i]);
    free(names);
}

/* refs/headsX/somebranch shares the literal string "refs/heads" as a
   PREFIX of its path, but it is NOT under the refs/heads/ namespace (there
   is no '/' right after "refs/heads"). A prefix filter that compared only
   strlen("refs/heads/") - 1 bytes (dropping the trailing slash) would let
   this line leak into `sg branch`'s listing; comparing the full
   "refs/heads/" (trailing slash included) correctly excludes it. */
static void test_list_excludes_prefix_collision(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char collision_id[SG_SHA1_RAW_LEN];
    unsigned char nested_id[SG_SHA1_RAW_LEN];
    char collision_line[256];
    char nested_line[256];
    const char *lines[2];
    char **names;
    size_t count;

    fill_id(collision_id, 0x11);
    fill_id(nested_id, 0x22);

    hex_line(collision_line, sizeof(collision_line), collision_id, "refs/headsX/somebranch");
    hex_line(nested_line, sizeof(nested_line), nested_id, "refs/heads/feature/x");
    lines[0] = collision_line;
    lines[1] = nested_line;
    write_packed_refs(git_dir, lines, 2);

    CHECK(sg_ref_list_branches(git_dir, &names, &count) == 0, "sg_ref_list_branches failed");

    /* Exactly one legitimate branch was packed (feature/x); anything else
       in the result -- regardless of exactly how a broken prefix filter
       mis-slices the colliding refs/headsX/somebranch line's suffix --
       means the collision leaked through. Checking the count (not just
       specific guessed-wrong names) is what actually catches an off-by-one
       in the slice offset, whatever spurious string it happens to produce. */
    CHECK(count == 1, "exactly one branch (feature/x) should be listed, got %zu", count);
    CHECK(names_contains(names, count, "feature/x"),
         "a legitimately nested branch name (refs/heads/feature/x) must still be listed");
    CHECK(!names_contains(names, count, "X/somebranch") && !names_contains(names, count, "somebranch") &&
         !names_contains(names, count, "/somebranch"),
         "a packed line under refs/headsX/ (prefix collision, not a real subtree) must not be listed "
         "as a branch");

    free_names(names, count);
    free(git_dir);
}

/* Same prefix-collision hazard, but for sg_ref_delete_under: deleting
   "somebranch" under prefix "refs/heads/" must not touch a packed line
   filed under the colliding "refs/headsX/" namespace. */
static void test_delete_does_not_touch_prefix_collision(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char collision_id[SG_SHA1_RAW_LEN];
    unsigned char real_id[SG_SHA1_RAW_LEN];
    char collision_line[256];
    char real_line[256];
    const char *lines[2];
    unsigned char out[SG_SHA1_RAW_LEN];

    fill_id(collision_id, 0x33);
    fill_id(real_id, 0x44);

    /* "somebranch" under the colliding "refs/headsX/" namespace ... */
    hex_line(collision_line, sizeof(collision_line), collision_id, "refs/headsX/somebranch");
    /* ... and a REAL "refs/heads/somebranch", the one actually targeted. */
    hex_line(real_line, sizeof(real_line), real_id, "refs/heads/somebranch");
    lines[0] = collision_line;
    lines[1] = real_line;
    write_packed_refs(git_dir, lines, 2);

    CHECK(sg_ref_delete_branch(git_dir, "somebranch") == 0, "delete of the real branch should succeed");

    /* The colliding refs/headsX/somebranch line must survive: read it back
       directly through the generic ref-path reader. */
    CHECK(sg_ref_read_path(git_dir, "refs/headsX/somebranch", out) == 0,
         "the prefix-colliding packed line must not have been removed");
    CHECK(memcmp(out, collision_id, SG_SHA1_RAW_LEN) == 0,
         "the surviving colliding line must still point at its original id");

    /* And the real branch really is gone from both stores. */
    CHECK(sg_ref_branch_exists(git_dir, "somebranch") == 0,
         "the real refs/heads/somebranch should no longer exist after delete");

    free(git_dir);
}

int main(void)
{
    test_list_excludes_prefix_collision();
    test_delete_does_not_touch_prefix_collision();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all refs tests passed\n");
    return 0;
}
