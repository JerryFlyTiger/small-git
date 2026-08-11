/* sg_merge_head_exists vs sg_merge_head_read.
 *
 * These two answer different questions and the difference is load-bearing:
 * `read` folds "there is no merge" and "the merge state is corrupt" into the
 * same -1, so a gate built on it waves a corrupt merge straight through.
 * `exists` asks only whether something is at the path -- which is what real
 * git's file_exists() does, measured against git 2.55.0: `git switch` during
 * a merge is refused even when MERGE_HEAD is empty, malformed, or a
 * directory. cmd_switch.c's gate depends on that distinction; so does any
 * future caller, hence the pinning here rather than only in interop.sh.
 */

#include "sg/merge.h"

#include "sg/hash.h"
#include "sg/repo.h"

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

static char *make_tmp_repo(void)
{
    /* strdup first, same as every other test here: mkdtemp rewrites its
       argument in place, so calling this twice against the static template
       directly would hand the second call a string that no longer ends in
       XXXXXX. */
    static char template[] = "/tmp/sg_merge_head_test_XXXXXX";
    char *dir = strdup(template);

    if (dir == NULL || mkdtemp(dir) == NULL) {
        fprintf(stderr, "setup failed: mkdtemp\n");
        exit(1);
    }
    if (sg_repo_init(dir) != 0) {
        fprintf(stderr, "setup failed: sg_repo_init\n");
        exit(1);
    }
    return dir;
}

static void write_merge_head_raw(const char *git_dir, const char *contents)
{
    char path[4096];
    FILE *f;

    snprintf(path, sizeof(path), "%s/MERGE_HEAD", git_dir);
    f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "setup failed: fopen MERGE_HEAD\n");
        exit(1);
    }
    if (contents[0] != '\0' && fputs(contents, f) == EOF) {
        fprintf(stderr, "setup failed: write MERGE_HEAD\n");
        exit(1);
    }
    fclose(f);
}

static void remove_merge_head(const char *git_dir)
{
    char path[4096];

    /* remove() covers both cases on POSIX: unlink for a file, rmdir for a
       directory. Either way the path is gone afterwards. */
    snprintf(path, sizeof(path), "%s/MERGE_HEAD", git_dir);
    remove(path);
}

int main(void)
{
    char *repo = make_tmp_repo();
    char git_dir[4096];
    char path[4096];
    unsigned char id[SG_SHA1_RAW_LEN];
    unsigned char scratch[SG_SHA1_RAW_LEN];
    size_t i;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", repo);
    for (i = 0; i < SG_SHA1_RAW_LEN; i++)
        id[i] = (unsigned char)(i + 1);

    /* no merge: both agree */
    CHECK(sg_merge_head_exists(git_dir) == 0, "exists should be 0 with no MERGE_HEAD");
    CHECK(sg_merge_head_read(git_dir, scratch) != 0, "read should fail with no MERGE_HEAD");

    /* a well-formed merge: both agree the other way, and read round-trips */
    if (sg_merge_head_write(git_dir, id) != 0) {
        fprintf(stderr, "setup failed: sg_merge_head_write\n");
        exit(1);
    }
    CHECK(sg_merge_head_exists(git_dir) != 0, "exists should be true after write");
    CHECK(sg_merge_head_read(git_dir, scratch) == 0, "read should succeed after write");
    CHECK(memcmp(scratch, id, SG_SHA1_RAW_LEN) == 0, "read should round-trip the written id");

    /* corrupt merge state: this is the whole point -- exists still reports a
       merge in flight while read cannot, so only exists is safe as a gate */
    write_merge_head_raw(git_dir, "not-a-sha\n");
    CHECK(sg_merge_head_exists(git_dir) != 0, "exists should be true for a malformed MERGE_HEAD");
    CHECK(sg_merge_head_read(git_dir, scratch) != 0, "read should fail for a malformed MERGE_HEAD");

    write_merge_head_raw(git_dir, "");
    CHECK(sg_merge_head_exists(git_dir) != 0, "exists should be true for an empty MERGE_HEAD");
    CHECK(sg_merge_head_read(git_dir, scratch) != 0, "read should fail for an empty MERGE_HEAD");

    /* short but otherwise hex: still not a merge id, still a merge in flight */
    write_merge_head_raw(git_dir, "abc123\n");
    CHECK(sg_merge_head_exists(git_dir) != 0, "exists should be true for a truncated MERGE_HEAD");
    CHECK(sg_merge_head_read(git_dir, scratch) != 0, "read should fail for a truncated MERGE_HEAD");

    /* a directory at the path: real git refuses here too (measured), so this
       must not be filtered out by an S_ISREG-style check */
    remove_merge_head(git_dir);
    snprintf(path, sizeof(path), "%s/MERGE_HEAD", git_dir);
    if (mkdir(path, 0777) != 0) {
        fprintf(stderr, "setup failed: mkdir MERGE_HEAD\n");
        exit(1);
    }
    CHECK(sg_merge_head_exists(git_dir) != 0, "exists should be true for a directory at MERGE_HEAD");
    CHECK(sg_merge_head_read(git_dir, scratch) != 0, "read should fail for a directory at MERGE_HEAD");
    rmdir(path);

    /* removal ends the merge for both */
    if (sg_merge_head_write(git_dir, id) != 0) {
        fprintf(stderr, "setup failed: sg_merge_head_write (second)\n");
        exit(1);
    }
    CHECK(sg_merge_head_exists(git_dir) != 0, "exists should be true before remove");
    CHECK(sg_merge_head_remove(git_dir) == 0, "remove should succeed");
    CHECK(sg_merge_head_exists(git_dir) == 0, "exists should be 0 after remove");
    CHECK(sg_merge_head_remove(git_dir) == 0, "remove should be a no-op when already gone");
    CHECK(sg_merge_head_exists(git_dir) == 0, "exists should still be 0 after the no-op remove");

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all merge_head tests passed\n");
    return 0;
}
