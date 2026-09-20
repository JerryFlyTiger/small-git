#include "sg/hash.h"
#include "sg/object.h"
#include "sg/workdir.h"

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

/* Phase 81b unit tests for the shared worktree READ helper
   (sg_worktree_classify/sg_worktree_read_entry/sg_worktree_hash_entry,
   include/sg/workdir.h) -- classification of every fs entry type the spec
   calls out: regular, exec, symlink to file, dir, dangling symlink, a long
   symlink target (to prove the readlink buffer actually grows), and
   fifo/other -> "not a blob". No git repository is needed: repo_root here
   is just a scratch directory, since these helpers only ever lstat/readlink
   below it. */

static char *make_tmp_root(void)
{
    static char tmpl[] = "/tmp/sg_worktree_symlink_test_XXXXXX";
    char *dir = strdup(tmpl);

    if (dir == NULL || mkdtemp(dir) == NULL) {
        fprintf(stderr, "setup failed: mkdtemp\n");
        exit(1);
    }
    return dir;
}

static void write_file(const char *root, const char *rel, const char *content, int exec)
{
    char abs[4096];
    FILE *f;

    snprintf(abs, sizeof(abs), "%s/%s", root, rel);
    f = fopen(abs, "wb");
    if (f == NULL) {
        fprintf(stderr, "setup failed: fopen %s\n", abs);
        exit(1);
    }
    fputs(content, f);
    fclose(f);
    if (exec)
        chmod(abs, 0755);
}

static void make_symlink(const char *root, const char *rel, const char *target)
{
    char abs[4096];

    snprintf(abs, sizeof(abs), "%s/%s", root, rel);
    if (symlink(target, abs) != 0) {
        fprintf(stderr, "setup failed: symlink %s -> %s\n", abs, target);
        exit(1);
    }
}

static void test_regular_and_exec(void)
{
    char *root = make_tmp_root();
    unsigned int mode = 0;
    unsigned char *data;
    size_t len;
    sg_wt_kind kind;

    write_file(root, "plain.txt", "hello", 0);
    kind = sg_worktree_read_entry(root, "plain.txt", &mode, &data, &len);
    CHECK(kind == SG_WT_REGULAR, "expected SG_WT_REGULAR, got %d", (int)kind);
    CHECK(mode == 0100644, "expected mode 100644, got %o", mode);
    CHECK(len == 5 && memcmp(data, "hello", 5) == 0, "unexpected content");
    free(data);

    write_file(root, "exec.sh", "#!/bin/sh\n", 1);
    kind = sg_worktree_read_entry(root, "exec.sh", &mode, &data, &len);
    CHECK(kind == SG_WT_REGULAR, "expected SG_WT_REGULAR, got %d", (int)kind);
    CHECK(mode == 0100755, "expected mode 100755, got %o", mode);
    free(data);

    free(root);
}

static void test_symlink_to_file(void)
{
    char *root = make_tmp_root();
    unsigned int mode = 0;
    unsigned char *data;
    size_t len;
    sg_wt_kind kind;

    write_file(root, "target.txt", "world", 0);
    make_symlink(root, "link", "target.txt");

    kind = sg_worktree_read_entry(root, "link", &mode, &data, &len);
    CHECK(kind == SG_WT_SYMLINK, "expected SG_WT_SYMLINK, got %d", (int)kind);
    CHECK(mode == 0120000, "expected mode 120000, got %o", mode);
    CHECK(len == strlen("target.txt") && memcmp(data, "target.txt", len) == 0,
         "expected content to be the readlink target, not the file it points at");
    free(data);

    free(root);
}

static void test_dangling_symlink_exists(void)
{
    char *root = make_tmp_root();
    unsigned int mode = 0;
    unsigned char *data;
    size_t len;
    sg_wt_kind kind;

    make_symlink(root, "dangling", "nowhere");

    /* ORACLE.md item 2: existence is decided by lstat -- a dangling symlink
       EXISTS, it is not treated as absent. */
    kind = sg_worktree_read_entry(root, "dangling", &mode, &data, &len);
    CHECK(kind == SG_WT_SYMLINK, "expected SG_WT_SYMLINK for a dangling link, got %d", (int)kind);
    CHECK(mode == 0120000, "expected mode 120000, got %o", mode);
    CHECK(len == strlen("nowhere") && memcmp(data, "nowhere", len) == 0, "unexpected content");
    free(data);

    free(root);
}

static void test_directory_is_other(void)
{
    char *root = make_tmp_root();
    unsigned int mode = 0;
    unsigned char *data;
    size_t len;
    char sub[4096];
    sg_wt_kind kind;

    snprintf(sub, sizeof(sub), "%s/adir", root);
    if (mkdir(sub, 0755) != 0) {
        fprintf(stderr, "setup failed: mkdir\n");
        exit(1);
    }

    kind = sg_worktree_read_entry(root, "adir", &mode, &data, &len);
    CHECK(kind == SG_WT_OTHER, "expected SG_WT_OTHER for a directory, got %d", (int)kind);
    CHECK(data == NULL && len == 0, "expected no content for SG_WT_OTHER");

    free(root);
}

static void test_absent(void)
{
    char *root = make_tmp_root();
    unsigned int mode = 0;
    unsigned char *data;
    size_t len;
    sg_wt_kind kind;

    kind = sg_worktree_read_entry(root, "nope", &mode, &data, &len);
    CHECK(kind == SG_WT_ABSENT, "expected SG_WT_ABSENT for a missing path, got %d", (int)kind);
    CHECK(data == NULL && len == 0, "expected no content for SG_WT_ABSENT");

    free(root);
}

static void test_fifo_is_other(void)
{
    char *root = make_tmp_root();
    char abs[4096];
    unsigned int mode = 0;
    unsigned char *data;
    size_t len;
    sg_wt_kind kind;

    snprintf(abs, sizeof(abs), "%s/fifo", root);
    if (mkfifo(abs, 0644) != 0) {
        fprintf(stderr, "setup failed: mkfifo (skipping, not all filesystems support it)\n");
        free(root);
        return;
    }

    kind = sg_worktree_read_entry(root, "fifo", &mode, &data, &len);
    CHECK(kind == SG_WT_OTHER, "expected SG_WT_OTHER for a fifo, got %d", (int)kind);
    CHECK(data == NULL && len == 0, "expected no content for SG_WT_OTHER");

    free(root);
}

/* A target longer than a typical PATH_MAX (4096 on Linux, 1024 on macOS)
   must be read in full, never truncated at the growing readlink buffer's
   first guess (256 bytes). */
static void test_long_symlink_target(void)
{
    char *root = make_tmp_root();
    /* macOS's own symlink() refuses a target over ~1023 bytes, so 1000 is
       used here (matching the interop fixture's own choice, ORACLE.md's
       "81b spec" tests section) rather than something that would fail at
       setup time regardless of what sg_worktree_read_entry does. */
    char *target = malloc(1001);
    unsigned int mode = 0;
    unsigned char *data;
    size_t len;
    sg_wt_kind kind;
    size_t i;

    if (target == NULL) {
        fprintf(stderr, "setup failed: malloc\n");
        exit(1);
    }
    for (i = 0; i < 1000; i += 2) {
        target[i] = 'a';
        target[i + 1] = '/';
    }
    target[1000] = '\0';

    make_symlink(root, "longlink", target);
    kind = sg_worktree_read_entry(root, "longlink", &mode, &data, &len);
    CHECK(kind == SG_WT_SYMLINK, "expected SG_WT_SYMLINK, got %d", (int)kind);
    CHECK(len == 1000, "expected a 1000-byte target, got %zu", len);
    CHECK(memcmp(data, target, 1000) == 0, "long target content mismatch (possible truncation)");
    free(data);
    free(target);
    free(root);
}

/* Phase 81b round 1 (item 5): exact boundary lengths around
   sg_worktree_readlink's growing buffer -- initial capacity 256, then
   doubling (512, 1024, ...). cap-1/cap/cap+1 for the first two capacities,
   plus the platform maximum a real symlink() will accept (macOS refuses a
   target over roughly 1023 bytes; Linux's is PATH_MAX, comfortably above
   every length tried here). A truncating implementation would most likely
   fail exactly AT a capacity boundary (cap or cap+1), which is why those are
   tested individually rather than only a single "long" length. */
static void test_symlink_target_boundary_lengths(void)
{
    static const size_t lengths[] = {255, 256, 257, 511, 512, 513, 1023};
    size_t li;

    for (li = 0; li < sizeof(lengths) / sizeof(lengths[0]); li++) {
        size_t n = lengths[li];
        char *root = make_tmp_root();
        char *target = malloc(n + 1);
        unsigned int mode = 0;
        unsigned char *data;
        size_t len;
        sg_wt_kind kind;
        size_t i;

        if (target == NULL) {
            fprintf(stderr, "setup failed: malloc\n");
            exit(1);
        }
        for (i = 0; i < n; i++)
            target[i] = (char)('a' + (i % 26));
        target[n] = '\0';

        make_symlink(root, "l", target);
        kind = sg_worktree_read_entry(root, "l", &mode, &data, &len);
        CHECK(kind == SG_WT_SYMLINK, "length %zu: expected SG_WT_SYMLINK, got %d", n, (int)kind);
        CHECK(len == n, "length %zu: expected a %zu-byte target, got %zu", n, n, len);
        if (kind == SG_WT_SYMLINK && len == n)
            CHECK(memcmp(data, target, n) == 0, "length %zu: content mismatch (possible truncation)",
                 n);
        if (kind == SG_WT_SYMLINK)
            free(data);
        free(target);
        free(root);
    }
}

static void test_hash_entry_matches_sg_object_hash(void)
{
    char *root = make_tmp_root();
    unsigned int mode = 0;
    unsigned char got[SG_SHA1_RAW_LEN];
    unsigned char want[SG_SHA1_RAW_LEN];

    make_symlink(root, "l", "a/b/c");
    CHECK(sg_worktree_hash_entry(root, "l", &mode, got) == 0, "sg_worktree_hash_entry failed");
    CHECK(mode == 0120000, "expected mode 120000, got %o", mode);
    sg_object_hash(SG_OBJ_BLOB, (const unsigned char *)"a/b/c", 5, want);
    CHECK(memcmp(got, want, SG_SHA1_RAW_LEN) == 0, "hash mismatch for a symlink blob");

    free(root);
}

static void test_ancestor_symlink_blocks(void)
{
    char *root = make_tmp_root();
    char sub[4096];
    unsigned int mode = 0;
    unsigned char *data;
    size_t len;
    sg_wt_kind kind;

    snprintf(sub, sizeof(sub), "%s/real", root);
    if (mkdir(sub, 0755) != 0) {
        fprintf(stderr, "setup failed: mkdir\n");
        exit(1);
    }
    write_file(root, "real/a.txt", "hi", 0);
    make_symlink(root, "dirlink", "real");

    CHECK(sg_worktree_ancestor_blocked(root, "dirlink/a.txt") != 0,
         "expected a symlinked ancestor to be reported as blocked");

    /* Item 4/X01-X03: a path beyond a blocked ancestor is ABSENT even
       though a plain stat() through the symlink would find the file. */
    kind = sg_worktree_read_entry(root, "dirlink/a.txt", &mode, &data, &len);
    CHECK(kind == SG_WT_ABSENT, "expected SG_WT_ABSENT beyond a symlinked ancestor, got %d",
         (int)kind);
    CHECK(data == NULL && len == 0, "expected no content beyond a blocked ancestor");

    /* A real, non-symlinked ancestor is never blocked. */
    CHECK(sg_worktree_ancestor_blocked(root, "real/a.txt") == 0,
         "a real directory ancestor must not be reported as blocked");

    free(root);
}

int main(void)
{
    test_regular_and_exec();
    test_symlink_to_file();
    test_dangling_symlink_exists();
    test_directory_is_other();
    test_absent();
    test_fifo_is_other();
    test_long_symlink_target();
    test_symlink_target_boundary_lengths();
    test_hash_entry_matches_sg_object_hash();
    test_ancestor_symlink_blocks();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all worktree_symlink tests passed\n");
    return 0;
}
