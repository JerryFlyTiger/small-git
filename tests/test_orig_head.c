/* Phase 78: sg_cli_write_orig_head (cli/cli_args.c) -- the shared writer for
   git's ORIG_HEAD (reset/merge/rebase; stash push calls sg_ref_write_path
   directly instead, since safety/ may not depend on cli/, see
   sg_cli_write_orig_head's own header comment in cli_args.h).
   docs/RULES-refs-revparse.md and CLAUDE.md's "Testing conventions" cover
   the mutate.sh discipline this file must survive. */

#include "sg/cli_args.h"

#include "sg/refs.h"
#include "sg/repo.h"

#include <fcntl.h>
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
    static char template[] = "/tmp/sg_orig_head_test_XXXXXX";
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

static void fill_id(unsigned char id[SG_SHA1_RAW_LEN], unsigned char b)
{
    memset(id, b, SG_SHA1_RAW_LEN);
}

/* ---- 1: a successful write is exactly 41 bytes (40 hex + '\n'), reads
   back the id, and never creates logs/ORIG_HEAD (no reflog by default --
   ORACLE-orig-head.md section 1: ORIG_HEAD is not on ref_path_reflog_allowed's
   namespace list, and real git itself writes no reflog for it without
   core.logAllRefUpdates=always, which sg has no equivalent of). ---- */

static void test_write_byte_shape_and_no_reflog(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id_a[SG_SHA1_RAW_LEN];
    unsigned char read_back[SG_SHA1_RAW_LEN];
    char orig_head_path[4096];
    char logs_path[4096];
    struct stat st;

    fill_id(id_a, 0x11);

    CHECK(sg_cli_write_orig_head(git_dir, id_a) == 0, "write failed");

    snprintf(orig_head_path, sizeof(orig_head_path), "%s/ORIG_HEAD", git_dir);
    CHECK(stat(orig_head_path, &st) == 0, "ORIG_HEAD must exist after a successful write");
    CHECK(st.st_size == SG_SHA1_HEX_LEN + 1,
         "ORIG_HEAD must be exactly hex+newline bytes (%d), got %lld", SG_SHA1_HEX_LEN + 1,
         (long long)st.st_size);

    CHECK(sg_ref_read_path(git_dir, "ORIG_HEAD", read_back) == 0, "ORIG_HEAD must read back");
    CHECK(memcmp(read_back, id_a, SG_SHA1_RAW_LEN) == 0, "ORIG_HEAD must hold the written id");

    snprintf(logs_path, sizeof(logs_path), "%s/logs/ORIG_HEAD", git_dir);
    CHECK(stat(logs_path, &st) != 0, "logs/ORIG_HEAD must NOT be created (no reflog by default)");

    free(git_dir);
}

/* ---- 2: a second write overwrites the first (this is a real ref update,
   not an append-only log). ---- */

static void test_repeated_write_overwrites(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id_a[SG_SHA1_RAW_LEN];
    unsigned char id_b[SG_SHA1_RAW_LEN];
    unsigned char read_back[SG_SHA1_RAW_LEN];

    fill_id(id_a, 0x22);
    fill_id(id_b, 0x33);

    CHECK(sg_cli_write_orig_head(git_dir, id_a) == 0, "first write failed");
    CHECK(sg_cli_write_orig_head(git_dir, id_b) == 0, "second write failed");

    CHECK(sg_ref_read_path(git_dir, "ORIG_HEAD", read_back) == 0, "ORIG_HEAD must read back");
    CHECK(memcmp(read_back, id_b, SG_SHA1_RAW_LEN) == 0,
         "ORIG_HEAD must hold the SECOND write's id, not the first");

    free(git_dir);
}

/* ---- 3: a foreign ORIG_HEAD.lock refuses the write (-1), records
   SG_REF_LOCK_ERR_LOCKED, leaves the lock file in place (this project's own
   convention -- a caller wanting the diagnostic still needs lock_path), and
   leaves ORIG_HEAD itself untouched (nothing existed before, so it must
   still not exist after the refusal). ---- */

static void test_foreign_lock_refuses_write(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id_a[SG_SHA1_RAW_LEN];
    char lock_path[4096];
    char orig_head_path[4096];
    struct stat st;
    int fd;

    fill_id(id_a, 0x44);

    snprintf(lock_path, sizeof(lock_path), "%s/ORIG_HEAD.lock", git_dir);
    fd = open(lock_path, O_CREAT | O_EXCL | O_WRONLY, 0666);
    CHECK(fd >= 0, "planting a foreign lock failed");
    close(fd);

    CHECK(sg_cli_write_orig_head(git_dir, id_a) == -1, "a foreign ORIG_HEAD.lock must refuse the write");
    CHECK(sg_ref_last_lock_err()->kind == SG_REF_LOCK_ERR_LOCKED,
         "the recorded failure kind must be SG_REF_LOCK_ERR_LOCKED, got %d",
         sg_ref_last_lock_err()->kind);

    CHECK(stat(lock_path, &st) == 0, "the foreign lock file must still be there after the refusal");

    snprintf(orig_head_path, sizeof(orig_head_path), "%s/ORIG_HEAD", git_dir);
    CHECK(stat(orig_head_path, &st) != 0,
         "ORIG_HEAD itself must not have been created by the refused write");

    unlink(lock_path);
    free(git_dir);
}

int main(void)
{
    test_write_byte_shape_and_no_reflog();
    test_repeated_write_overwrites();
    test_foreign_lock_refuses_write();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all orig_head tests passed\n");
    return 0;
}
