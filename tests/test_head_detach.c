/* The refs-layer half of Phase 18: HEAD may now legitimately hold a raw
   object id. Two properties are worth pinning here rather than only through
   the CLI, because both have a failure mode that produces a plausible file
   and no error at all:

     - sg_ref_head_is_detached must tell "detached" apart from "corrupt".
       Folding them together would let a caller overwrite a garbage HEAD with
       a raw id and call the repository repaired.
     - sg_ref_set_head_detached must log the commit being LEFT as old_id.
       Reading HEAD's own file for that (the obvious shortcut, and what
       sg_ref_update would do) cannot work while HEAD is still symbolic, and
       fails by silently logging all-zeros -- a reflog line that still parses
       and still points at the right new commit. */

#include "sg/refs.h"

#include "sg/hash.h"
#include "sg/reflog.h"
#include "sg/repo.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, ...)                                                                         \
    do {                                                                                         \
        if (!(cond)) {                                                                            \
            fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);                                  \
            fprintf(stderr, __VA_ARGS__);                                                         \
            fprintf(stderr, "\n");                                                                \
            failures++;                                                                           \
        }                                                                                          \
    } while (0)

static char *make_tmp_repo(void)
{
    static char template[] = "/tmp/sg_head_detach_test_XXXXXX";
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

static void write_head_literally(const char *git_dir, const char *text)
{
    char path[4096];

    snprintf(path, sizeof(path), "%s/HEAD", git_dir);
    if (sg_write_file_mkdirs(path, (const unsigned char *)text, strlen(text), 0644) != 0) {
        fprintf(stderr, "could not write HEAD\n");
        exit(1);
    }
}

/* Classification of the three HEAD shapes. The corrupt case is the one that
   matters: it must not answer "detached". */
static void test_head_is_detached_classifies(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id_a[SG_SHA1_RAW_LEN];
    unsigned char got[SG_SHA1_RAW_LEN];

    fill_id(id_a, 0xa1);

    CHECK(sg_ref_head_is_detached(git_dir) == 0, "fresh repo: HEAD should be symbolic");

    CHECK(sg_ref_set_head_detached(git_dir, id_a, NULL) == 0, "detach should succeed");
    CHECK(sg_ref_head_is_detached(git_dir) == 1, "after detach: HEAD should be detached");
    CHECK(sg_ref_resolve_head(git_dir, got) == 0 && memcmp(got, id_a, SG_SHA1_RAW_LEN) == 0,
         "detached HEAD should resolve to the id it was detached at");

    write_head_literally(git_dir, "not a ref and not a sha\n");
    CHECK(sg_ref_head_is_detached(git_dir) == -1, "garbage HEAD must report corrupt, not detached");
    CHECK(sg_ref_resolve_head(git_dir, got) != 0, "garbage HEAD must not resolve");

    /* Symbolic but pointing outside refs/heads/: still "not detached" (the
       file IS a symref), yet unresolvable -- the two answers are independent
       and this pins that they do not get merged. */
    write_head_literally(git_dir, "ref: refs/tags/v1\n");
    CHECK(sg_ref_head_is_detached(git_dir) == 0, "a symref to a tag is symbolic, not detached");
    CHECK(sg_ref_resolve_head(git_dir, got) != 0, "a symref to a missing tag must not resolve");

    free(git_dir);
}

/* Before Phase 18 an unborn HEAD and a detached HEAD both came back as -1
   from sg_ref_resolve_head, and every caller read that as "no commits yet".
   Detaching must not resurrect that conflation from the other side: unborn
   still has to fail. */
static void test_unborn_still_fails_to_resolve(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char got[SG_SHA1_RAW_LEN];

    CHECK(sg_ref_resolve_head(git_dir, got) != 0,
         "a fresh repo with no commits must still fail to resolve HEAD");

    free(git_dir);
}

/* The trap: old_id has to be the commit we are leaving. */
static void test_detach_logs_the_commit_it_left(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id_a[SG_SHA1_RAW_LEN];
    unsigned char id_b[SG_SHA1_RAW_LEN];
    unsigned char zeros[SG_SHA1_RAW_LEN];
    sg_reflog log;

    fill_id(id_a, 0xa1);
    fill_id(id_b, 0xb2);
    memset(zeros, 0, sizeof(zeros));

    /* Put master (which HEAD symbolically points at) on id_a. */
    CHECK(sg_ref_update(git_dir, "refs/heads/master", id_a, NULL) == 0, "seeding master failed");

    CHECK(sg_ref_set_head_detached(git_dir, id_b, "checkout: moving from master to b") == 0,
         "detach with a message should succeed");

    CHECK(sg_reflog_read(git_dir, "HEAD", &log) == 0 && log.count == 1,
         "detaching should append exactly one logs/HEAD line");
    if (log.count == 1) {
        CHECK(memcmp(log.entries[0].old_id, id_a, SG_SHA1_RAW_LEN) == 0,
             "old_id must be the commit HEAD was on (master's tip), not all-zeros -- "
             "reading HEAD's own file for this silently yields zeros while HEAD is symbolic");
        CHECK(memcmp(log.entries[0].old_id, zeros, SG_SHA1_RAW_LEN) != 0,
             "old_id must not be all-zeros");
        CHECK(memcmp(log.entries[0].new_id, id_b, SG_SHA1_RAW_LEN) == 0,
             "new_id must be the id we detached at");
        CHECK(strcmp(log.entries[0].message, "checkout: moving from master to b") == 0,
             "message should round-trip verbatim, got '%s'", log.entries[0].message);
    }
    sg_reflog_free(&log);

    free(git_dir);
}

/* Re-detaching from an already-detached HEAD: old_id now has to come from
   HEAD's own raw id. This is the mirror of the case above, and the two
   together are why old_id goes through sg_ref_resolve_head (which handles
   both shapes) rather than either single-shape reader. */
static void test_detach_from_detached_chains(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id_a[SG_SHA1_RAW_LEN];
    unsigned char id_b[SG_SHA1_RAW_LEN];
    unsigned char id_c[SG_SHA1_RAW_LEN];
    sg_reflog log;

    fill_id(id_a, 0xa1);
    fill_id(id_b, 0xb2);
    fill_id(id_c, 0xc3);

    CHECK(sg_ref_update(git_dir, "refs/heads/master", id_a, NULL) == 0, "seeding master failed");
    CHECK(sg_ref_set_head_detached(git_dir, id_b, "first detach") == 0, "first detach failed");
    CHECK(sg_ref_set_head_detached(git_dir, id_c, "second detach") == 0, "second detach failed");

    CHECK(sg_reflog_read(git_dir, "HEAD", &log) == 0 && log.count == 2,
         "two detaches should leave two logs/HEAD lines");
    if (log.count == 2) {
        CHECK(memcmp(log.entries[1].old_id, id_b, SG_SHA1_RAW_LEN) == 0,
             "second detach's old_id must chain off the first detach's new_id");
        CHECK(memcmp(log.entries[1].new_id, id_c, SG_SHA1_RAW_LEN) == 0, "second detach new_id");
        /* The chain must be unbroken: entry N's old_id == entry N-1's new_id,
           which is what makes HEAD@{N} walkable. */
        CHECK(memcmp(log.entries[0].new_id, log.entries[1].old_id, SG_SHA1_RAW_LEN) == 0,
             "reflog chain broken between the two entries");
    }
    sg_reflog_free(&log);

    free(git_dir);
}

/* NULL message means "move HEAD, log nothing" -- same contract as
   sg_ref_set_head. A stray line here would corrupt the at/from detach-point
   lookup, which reads the last checkout entry. */
static void test_null_message_writes_no_log(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id_a[SG_SHA1_RAW_LEN];
    sg_reflog log;

    fill_id(id_a, 0xa1);

    CHECK(sg_ref_set_head_detached(git_dir, id_a, NULL) == 0, "detach with NULL message failed");
    CHECK(sg_reflog_read(git_dir, "HEAD", &log) == 0 && log.count == 0,
         "a NULL reflog message must leave logs/HEAD empty");
    sg_reflog_free(&log);

    free(git_dir);
}

/* Re-attaching has to leave HEAD symbolic again, otherwise a repository
   would be a one-way trip into detached state. */
static void test_reattach_restores_symbolic_head(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id_a[SG_SHA1_RAW_LEN];
    char *branch;

    fill_id(id_a, 0xa1);

    CHECK(sg_ref_update(git_dir, "refs/heads/master", id_a, NULL) == 0, "seeding master failed");
    CHECK(sg_ref_set_head_detached(git_dir, id_a, "detach") == 0, "detach failed");
    CHECK(sg_ref_current_branch(git_dir) == NULL, "detached HEAD must have no current branch");

    CHECK(sg_ref_set_head(git_dir, "master", "checkout: moving from a to master") == 0,
         "reattach failed");
    CHECK(sg_ref_head_is_detached(git_dir) == 0, "reattached HEAD should be symbolic");
    branch = sg_ref_current_branch(git_dir);
    CHECK(branch != NULL && strcmp(branch, "master") == 0, "reattached HEAD should name master");
    free(branch);

    free(git_dir);
}

int main(void)
{
    test_head_is_detached_classifies();
    test_unborn_still_fails_to_resolve();
    test_detach_logs_the_commit_it_left();
    test_detach_from_detached_chains();
    test_null_message_writes_no_log();
    test_reattach_restores_symbolic_head();

    if (failures > 0)
        return 1;
    printf("all head detach tests passed\n");
    return 0;
}
