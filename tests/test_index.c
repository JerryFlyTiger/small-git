#include "sg/index.h"

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

static char *make_tmp_dir(void)
{
    static char template[] = "/tmp/sg_index_test_XXXXXX";
    char *path = strdup(template);

    if (mkdtemp(path) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        exit(1);
    }
    return path;
}

static void fill_entry(sg_index_entry *e, const char *path, unsigned int mode, unsigned char fill)
{
    memset(e, 0, sizeof(*e));
    e->ctime_sec = 111;
    e->ctime_nsec = 222;
    e->mtime_sec = 333;
    e->mtime_nsec = 444;
    e->dev = 5;
    e->ino = 6;
    e->mode = mode;
    e->uid = 7;
    e->gid = 8;
    e->file_size = 9;
    memset(e->sha1, fill, SG_SHA1_RAW_LEN);
    e->path = (char *)path;
}

static void test_roundtrip(void)
{
    char *dir = make_tmp_dir();
    sg_index idx;
    sg_index_entry e;
    sg_index reloaded;

    memset(&idx, 0, sizeof(idx));

    fill_entry(&e, "b.txt", 0100644, 0xAA);
    CHECK(sg_index_upsert(&idx, &e) == 0, "upsert b.txt failed");

    fill_entry(&e, "a/c.txt", 0100755, 0xBB);
    CHECK(sg_index_upsert(&idx, &e) == 0, "upsert a/c.txt failed");

    fill_entry(&e, "a.txt", 0100644, 0xCC);
    CHECK(sg_index_upsert(&idx, &e) == 0, "upsert a.txt failed");

    CHECK(idx.count == 3, "expected 3 entries, got %zu", idx.count);
    CHECK(strcmp(idx.entries[0].path, "a.txt") == 0, "sort order[0] = %s", idx.entries[0].path);
    CHECK(strcmp(idx.entries[1].path, "a/c.txt") == 0, "sort order[1] = %s", idx.entries[1].path);
    CHECK(strcmp(idx.entries[2].path, "b.txt") == 0, "sort order[2] = %s", idx.entries[2].path);

    CHECK(sg_index_write(dir, &idx) == 0, "write failed");

    CHECK(sg_index_read(dir, &reloaded) == 0, "read failed");
    CHECK(reloaded.count == 3, "reloaded count %zu", reloaded.count);
    if (reloaded.count == 3) {
        size_t i;

        for (i = 0; i < 3; i++) {
            CHECK(strcmp(idx.entries[i].path, reloaded.entries[i].path) == 0, "path[%zu] mismatch: %s vs %s",
                 i, idx.entries[i].path, reloaded.entries[i].path);
            CHECK(idx.entries[i].mode == reloaded.entries[i].mode, "mode[%zu] mismatch", i);
            CHECK(idx.entries[i].ctime_sec == reloaded.entries[i].ctime_sec, "ctime_sec[%zu] mismatch", i);
            CHECK(idx.entries[i].ctime_nsec == reloaded.entries[i].ctime_nsec, "ctime_nsec[%zu] mismatch", i);
            CHECK(idx.entries[i].mtime_sec == reloaded.entries[i].mtime_sec, "mtime_sec[%zu] mismatch", i);
            CHECK(idx.entries[i].mtime_nsec == reloaded.entries[i].mtime_nsec, "mtime_nsec[%zu] mismatch", i);
            CHECK(idx.entries[i].dev == reloaded.entries[i].dev, "dev[%zu] mismatch", i);
            CHECK(idx.entries[i].ino == reloaded.entries[i].ino, "ino[%zu] mismatch", i);
            CHECK(idx.entries[i].uid == reloaded.entries[i].uid, "uid[%zu] mismatch", i);
            CHECK(idx.entries[i].gid == reloaded.entries[i].gid, "gid[%zu] mismatch", i);
            CHECK(idx.entries[i].file_size == reloaded.entries[i].file_size, "file_size[%zu] mismatch", i);
            CHECK(memcmp(idx.entries[i].sha1, reloaded.entries[i].sha1, SG_SHA1_RAW_LEN) == 0,
                 "sha1[%zu] mismatch", i);
        }
    }

    CHECK(sg_index_find(&idx, "a/c.txt") == 1, "find a/c.txt");
    CHECK(sg_index_find(&idx, "nope") == -1, "find missing should be -1");

    CHECK(sg_index_remove(&idx, "a/c.txt") == 0, "remove a/c.txt failed");
    CHECK(idx.count == 2, "count after remove %zu", idx.count);
    CHECK(sg_index_remove(&idx, "a/c.txt") == -1, "remove again should fail");

    sg_index_free(&idx);
    sg_index_free(&reloaded);

    {
        char path[4096];

        snprintf(path, sizeof(path), "%s/index", dir);
        remove(path);
        rmdir(dir);
    }
    free(dir);
}

static void test_missing_index_is_empty(void)
{
    char *dir = make_tmp_dir();
    sg_index idx;

    CHECK(sg_index_read(dir, &idx) == 0, "missing index should not be an error");
    CHECK(idx.count == 0, "missing index should have 0 entries, got %zu", idx.count);

    rmdir(dir);
    free(dir);
}

/* the trickiest real-world failure mode: a real `git add`-produced index
   carries a TREE (cache-tree) extension after the entries, which a naive
   reader that doesn't know to skip it will misparse as corrupt data */
static void test_reads_real_git_index_with_extension(void)
{
    char *dir = make_tmp_dir();
    char cmd[8192];
    char index_path[4096];
    struct stat st;
    sg_index idx;

    snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email a@b.c && git config user.name t && "
            "printf 'hello\\n' > file1.txt && mkdir sub && printf 'world\\n' > sub/file2.txt && "
            "git add file1.txt sub/file2.txt >/dev/null",
            dir);
    if (system(cmd) != 0) {
        fprintf(stderr, "skip: git not available to produce a real index\n");
        free(dir);
        return;
    }

    snprintf(index_path, sizeof(index_path), "%s/.git/index", dir);
    CHECK(stat(index_path, &st) == 0, "real git index should exist");

    {
        char git_dir[4096];

        snprintf(git_dir, sizeof(git_dir), "%s/.git", dir);
        CHECK(sg_index_read(git_dir, &idx) == 0, "reading a real git index (with extensions) failed");
        CHECK(idx.count == 2, "expected 2 entries from real git index, got %zu", idx.count);
        if (idx.count == 2) {
            CHECK(strcmp(idx.entries[0].path, "file1.txt") == 0, "entry0 path %s", idx.entries[0].path);
            CHECK(strcmp(idx.entries[1].path, "sub/file2.txt") == 0, "entry1 path %s",
                 idx.entries[1].path);
        }
        sg_index_free(&idx);
    }

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "warning: cleanup of %s failed\n", dir);
    free(dir);
}

int main(void)
{
    test_roundtrip();
    test_missing_index_is_empty();
    test_reads_real_git_index_with_extension();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all index tests passed\n");
    return 0;
}
