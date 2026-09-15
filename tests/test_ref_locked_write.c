/* Phase 77: the locked-ref-write primitive shared by sg_ref_update /
   sg_ref_write_path / sg_ref_set_head / sg_ref_set_head_detached /
   sg_ref_delete_under -- locking (a foreign .lock refuses the write, ref
   unchanged, no stray .lock left behind), the D1a empty-directory fix, and
   atomicity (the write always lands via a rename, never a truncate-in-
   place). See CLAUDE.md's "Testing conventions" for the mutate.sh
   discipline this file must survive; docs/RULES-refs-revparse.md and
   docs/RULES-paths-strings.md cover this scope. */

#include "sg/refs.h"

#include "sg/repo.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

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
    static char template[] = "/tmp/sg_ref_locked_write_test_XXXXXX";
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

static int lock_file_exists(const char *git_dir, const char *rel_lock_path)
{
    char path[4096];
    struct stat st;

    snprintf(path, sizeof(path), "%s/%s", git_dir, rel_lock_path);
    return stat(path, &st) == 0;
}

/* ---- 1: a foreign .lock refuses the write, ref unchanged, no leftover -- */

static void test_foreign_lock_refuses_write(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id_a[SG_SHA1_RAW_LEN];
    unsigned char id_b[SG_SHA1_RAW_LEN];
    unsigned char read_back[SG_SHA1_RAW_LEN];
    char lock_path[4096];
    int fd;

    fill_id(id_a, 0x11);
    fill_id(id_b, 0x22);

    CHECK(sg_ref_update(git_dir, "refs/heads/x", id_a, NULL) == 0, "baseline write failed");

    snprintf(lock_path, sizeof(lock_path), "%s/refs/heads/x.lock", git_dir);
    fd = open(lock_path, O_CREAT | O_EXCL | O_WRONLY, 0666);
    CHECK(fd >= 0, "planting a foreign lock failed");
    close(fd);

    CHECK(sg_ref_update(git_dir, "refs/heads/x", id_b, NULL) == -1,
         "a foreign .lock must refuse the write");
    CHECK(sg_ref_last_lock_err()->kind == SG_REF_LOCK_ERR_LOCKED,
         "the recorded failure kind must be SG_REF_LOCK_ERR_LOCKED, got %d",
         sg_ref_last_lock_err()->kind);

    CHECK(sg_ref_read_path(git_dir, "refs/heads/x", read_back) == 0, "ref must still read back");
    CHECK(memcmp(read_back, id_a, SG_SHA1_RAW_LEN) == 0,
         "ref must be UNCHANGED after a refused write");

    CHECK(unlink(lock_path) == 0, "cleanup: removing the foreign lock failed");
    free(git_dir);
}

/* ---- 2: a successful write leaves no .lock file behind ------------------ */

static void test_success_leaves_no_lock_file(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id_a[SG_SHA1_RAW_LEN];

    fill_id(id_a, 0x33);

    CHECK(sg_ref_update(git_dir, "refs/heads/y", id_a, NULL) == 0, "write failed");
    CHECK(!lock_file_exists(git_dir, "refs/heads/y.lock"),
         "a successful write must not leave refs/heads/y.lock behind");

    free(git_dir);
}

/* ---- 3: a failed write (unsafe path) leaves no .lock file behind -------- */

static void test_failure_leaves_no_lock_file(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id_a[SG_SHA1_RAW_LEN];

    fill_id(id_a, 0x44);

    CHECK(sg_ref_update(git_dir, "refs/heads/../evil", id_a, NULL) == -1,
         "a path-unsafe ref_path must be refused");
    CHECK(!lock_file_exists(git_dir, "refs/heads/../evil.lock"),
         "a refused write must not leave a stray .lock file");

    free(git_dir);
}

/* ---- 4: D1a -- an empty directory at the ref path does not block the
   write, and the write still succeeds ------------------------------------- */

static void test_empty_dir_at_ref_path_removed(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id_a[SG_SHA1_RAW_LEN];
    unsigned char read_back[SG_SHA1_RAW_LEN];
    char dir_path[4096];
    char nested_path[4096];

    fill_id(id_a, 0x55);

    snprintf(dir_path, sizeof(dir_path), "%s/refs/heads/nope", git_dir);
    snprintf(nested_path, sizeof(nested_path), "%s/refs/heads/nope/a/b", git_dir);
    CHECK(mkdir(dir_path, 0755) == 0, "mkdir refs/heads/nope failed");
    {
        char a_path[4096];

        snprintf(a_path, sizeof(a_path), "%s/refs/heads/nope/a", git_dir);
        CHECK(mkdir(a_path, 0755) == 0, "mkdir refs/heads/nope/a failed");
        CHECK(mkdir(nested_path, 0755) == 0, "mkdir refs/heads/nope/a/b failed");
    }

    CHECK(sg_ref_update(git_dir, "refs/heads/nope", id_a, NULL) == 0,
         "write through a nested-empty directory must succeed");
    CHECK(sg_ref_read_path(git_dir, "refs/heads/nope", read_back) == 0,
         "ref should read back after the write");
    CHECK(memcmp(read_back, id_a, SG_SHA1_RAW_LEN) == 0, "ref should hold the written id");

    free(git_dir);
}

/* ---- 5: a REAL file under the directory is a D/F conflict, refused ------ */

static void test_nonempty_dir_at_ref_path_refused(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id_a[SG_SHA1_RAW_LEN];
    char dir_path[4096];
    char file_path[4096];
    FILE *f;

    fill_id(id_a, 0x66);

    snprintf(dir_path, sizeof(dir_path), "%s/refs/heads/blocked", git_dir);
    snprintf(file_path, sizeof(file_path), "%s/refs/heads/blocked/f", git_dir);
    CHECK(mkdir(dir_path, 0755) == 0, "mkdir refs/heads/blocked failed");
    f = fopen(file_path, "w");
    CHECK(f != NULL, "creating the blocking file failed");
    if (f != NULL) {
        fputs("x\n", f);
        fclose(f);
    }

    CHECK(sg_ref_update(git_dir, "refs/heads/blocked", id_a, NULL) == -1,
         "a real file under the ref path must be a D/F conflict, refused");
    CHECK(sg_ref_last_lock_err()->kind == SG_REF_LOCK_ERR_DF,
         "the recorded failure kind must be SG_REF_LOCK_ERR_DF, got %d",
         sg_ref_last_lock_err()->kind);
    {
        struct stat st;

        CHECK(stat(file_path, &st) == 0, "the blocking file itself must survive the refusal");
    }

    free(git_dir);
}

/* ---- 6: atomicity -- the write always lands via rename, never a
   truncate-in-place. Asserts BOTH that the ref file is exactly
   hex+newline bytes (never zero-length or mid-write-length -- not, by
   itself, proof of a rename: an in-place fopen("wb") also ends at the
   right final size once it completes) AND that the file's INODE changes
   across the write -- rename() always replaces the directory entry with
   a different inode (the lock file's own), while an in-place
   fopen(existing_path, "wb") truncates and rewrites the SAME inode. This
   second assertion is what actually distinguishes "write through a
   rename" from "write in place, but happened to finish successfully"
   (F9, Phase 77 fix round 2) -- named accordingly, since the original
   name ("never truncated") only covered the first, weaker property. ---- */

static void test_write_is_atomic_rename_not_in_place_truncate(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id_a[SG_SHA1_RAW_LEN];
    unsigned char id_b[SG_SHA1_RAW_LEN];
    char ref_file[4096];
    struct stat st_before, st_after;

    fill_id(id_a, 0x77);
    fill_id(id_b, 0x88);

    CHECK(sg_ref_update(git_dir, "refs/heads/z", id_a, NULL) == 0, "first write failed");
    snprintf(ref_file, sizeof(ref_file), "%s/refs/heads/z", git_dir);
    CHECK(stat(ref_file, &st_before) == 0, "ref file must exist after the first write");
    CHECK(st_before.st_size == SG_SHA1_HEX_LEN + 1,
         "ref file must be exactly hex+newline bytes (%d), got %lld", SG_SHA1_HEX_LEN + 1,
         (long long)st_before.st_size);

    CHECK(sg_ref_update(git_dir, "refs/heads/z", id_b, NULL) == 0, "second write (overwriting) failed");
    CHECK(stat(ref_file, &st_after) == 0, "ref file must exist after the second write");
    CHECK(st_after.st_size == SG_SHA1_HEX_LEN + 1,
         "ref file must still be exactly hex+newline bytes after being overwritten, got %lld",
         (long long)st_after.st_size);
    CHECK(st_before.st_ino != st_after.st_ino,
         "overwriting an EXISTING ref must change its inode (proves rename-replace, not an "
         "in-place truncate+rewrite of the same file)");

    free(git_dir);
}

/* ---- 7 (F1, Phase 77 fix round 2): the side channel must not leak a
   stale LOCKED/DF kind from one call into the NEXT call's failure, if
   that next failure has nothing to do with a lock. Provokes a real
   LOCKED failure first (so the channel actually holds something to leak),
   clears the foreign lock, then forces an ordinary reflog-I/O failure on
   an UNRELATED ref and asserts the recorded kind is NONE, not a stale
   LOCKED carried over from the first call. ---------------------------- */

static void test_lock_err_does_not_leak_across_calls(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id_a[SG_SHA1_RAW_LEN];
    unsigned char id_b[SG_SHA1_RAW_LEN];
    char lock_path[4096];
    char logs_heads_path[4096];
    int fd;

    if (geteuid() == 0) {
        /* root ignores file modes; the test would be meaningless */
        free(git_dir);
        return;
    }

    fill_id(id_a, 0x99);
    fill_id(id_b, 0xaa);

    /* First call: a real LOCKED failure, to populate the side channel
       with something that WOULD leak if the next call's entry-clear were
       missing or misplaced. */
    snprintf(lock_path, sizeof(lock_path), "%s/refs/heads/x.lock", git_dir);
    fd = open(lock_path, O_CREAT | O_EXCL | O_WRONLY, 0666);
    CHECK(fd >= 0, "planting a foreign lock failed");
    close(fd);
    CHECK(sg_ref_update(git_dir, "refs/heads/x", id_a, "msg") == -1,
         "first call (foreign lock) should fail");
    CHECK(sg_ref_last_lock_err()->kind == SG_REF_LOCK_ERR_LOCKED,
         "first call must record SG_REF_LOCK_ERR_LOCKED (setup precondition)");
    CHECK(unlink(lock_path) == 0, "cleanup: removing the foreign lock failed");

    /* Second call: a DIFFERENT, brand-new ref (no baseline write first --
       an intervening SUCCESSFUL write would itself clear the side channel
       via locked_ref_write's own entry clear, masking a missing/removed
       clear in sg_ref_update itself behind a redundant one; this must be
       the very NEXT lock_err-touching call after the LOCKED failure
       above, mutation-verified: with sg_ref_update's own entry clear
       removed via `bash tests/mutate.sh`, an EARLIER version of this test
       that DID do a baseline write first stayed green, a false negative).
       old_id reads as all-zeros (ref does not exist yet) and id_b is
       nonzero, so this is not a suppressed no-op -- sg_reflog_append is
       reached and fails for an ORDINARY I/O reason that has nothing to do
       with a lock (logs/refs/heads/ made read-only). That branch of
       sg_ref_update returns -1 without ever calling lock_err_set, so the
       correct recorded kind is NONE, not a leftover LOCKED from the call
       above. */
    {
        char logs_path[4096];
        char logs_refs_path[4096];

        snprintf(logs_path, sizeof(logs_path), "%s/logs", git_dir);
        snprintf(logs_refs_path, sizeof(logs_refs_path), "%s/logs/refs", git_dir);
        snprintf(logs_heads_path, sizeof(logs_heads_path), "%s/logs/refs/heads", git_dir);
        mkdir(logs_path, 0755); /* ignore EEXIST */
        mkdir(logs_refs_path, 0755);
        CHECK(mkdir(logs_heads_path, 0755) == 0 || errno == EEXIST, "mkdir logs/refs/heads failed");
    }
    CHECK(chmod(logs_heads_path, 0555) == 0, "chmod logs/refs/heads read-only failed");

    CHECK(sg_ref_update(git_dir, "refs/heads/w", id_b, "msg") == -1,
         "second call (read-only logs dir, unrelated to any lock) should fail");
    chmod(logs_heads_path, 0755); /* restore before any further reads/writes */

    CHECK(sg_ref_last_lock_err()->kind == SG_REF_LOCK_ERR_NONE,
         "second call's failure has nothing to do with a lock; a stale LOCKED from the FIRST "
         "call must not leak through -- got kind %d", sg_ref_last_lock_err()->kind);

    free(git_dir);
}

/* ---- 8 (A, Phase 77 fix round 3): sg_ref_update_locked must validate
   ref_path itself on the `existing`-lock path -- round 2 moved that
   check into locked_ref_acquire, which this path never calls (the caller
   already holds the lock), so an unvalidated ref_path reached
   locked_ref_clear_blocking_dir's lstat/opendir/rmdir walk unchecked.
   Proves no filesystem side effect reaches outside git_dir. Per CLAUDE.md
   Phase 76's R4-1 lesson, "plant something AT the escape target" is the
   WRONG fixture here -- an existing file/dir there makes O_CREAT|O_EXCL
   (or, for this bug, D1a's own directory check) return EEXIST/behave
   identically whether or not the guard ran, so it cannot discriminate.
   Instead: an EMPTY directory at the escape target, asserted to SURVIVE
   -- without the guard, locked_ref_clear_blocking_dir treats it as "just
   an empty ref-path directory" and rmdir's it. ---- */

static void test_update_locked_rejects_unsafe_ref_path(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id[SG_SHA1_RAW_LEN];
    sg_ref_lock fake_lock;
    char marker_dir[4096];
    char marker_inner[4096];
    char escape_ref_path[256];
    struct stat st;

    fill_id(id, 0xbb);
    memset(&fake_lock, 0, sizeof(fake_lock));

    /* make_tmp_repo's git_dir is always "<mkdtemp under /tmp>/.git" --
       exactly TWO path components below /tmp -- so "../../<marker>/inner"
       always resolves to "/tmp/<marker>/inner" regardless of the
       mkdtemp-generated suffix. */
    snprintf(marker_dir, sizeof(marker_dir), "/tmp/sg_p77_escape_marker_%ld", (long)getpid());
    snprintf(marker_inner, sizeof(marker_inner), "%s/inner", marker_dir);
    snprintf(escape_ref_path, sizeof(escape_ref_path), "../../sg_p77_escape_marker_%ld/inner",
             (long)getpid());

    rmdir(marker_inner);
    rmdir(marker_dir);
    CHECK(mkdir(marker_dir, 0755) == 0, "mkdir marker dir failed");
    CHECK(mkdir(marker_inner, 0755) == 0, "mkdir marker inner dir failed");

    CHECK(sg_ref_update_locked(git_dir, escape_ref_path, &fake_lock, id, NULL) == -1,
         "an unsafe (..-escaping) ref_path must be refused");
    CHECK(sg_ref_last_lock_err()->kind == SG_REF_LOCK_ERR_OTHER,
         "the recorded failure kind must be SG_REF_LOCK_ERR_OTHER, got %d",
         sg_ref_last_lock_err()->kind);
    CHECK(stat(marker_inner, &st) == 0 && S_ISDIR(st.st_mode),
         "the escape target's empty directory must SURVIVE -- without the restored "
         "path-safety check, locked_ref_clear_blocking_dir would have rmdir'd it as an "
         "ordinary empty ref-path directory");

    rmdir(marker_inner);
    rmdir(marker_dir);
    free(git_dir);
}

/* ---- 9 (F7 ordering, Phase 77 fix round 3): the ref's OWN lock must be
   taken (and checked) BEFORE any reflog append, not after. Plants a
   foreign refs/heads/x.lock AND makes logs/refs/heads read-only (so the
   reflog append would ALSO fail, for an unrelated reason, if it were
   ever reached) -- with the correct order, the lock check fires first
   and the reflog append is never attempted at all. ---- */

static void test_update_locks_ref_before_reflog_append(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id_a[SG_SHA1_RAW_LEN];
    char lock_path[4096];
    char logs_heads_path[4096];
    int fd;

    if (geteuid() == 0) {
        printf("SKIP test_update_locks_ref_before_reflog_append: running as root, chmod is "
              "ineffective\n");
        free(git_dir);
        return;
    }

    fill_id(id_a, 0xcc);

    snprintf(lock_path, sizeof(lock_path), "%s/refs/heads/x.lock", git_dir);
    fd = open(lock_path, O_CREAT | O_EXCL | O_WRONLY, 0666);
    CHECK(fd >= 0, "planting a foreign lock failed");
    close(fd);

    {
        char logs_path[4096];
        char logs_refs_path[4096];

        snprintf(logs_path, sizeof(logs_path), "%s/logs", git_dir);
        snprintf(logs_refs_path, sizeof(logs_refs_path), "%s/logs/refs", git_dir);
        snprintf(logs_heads_path, sizeof(logs_heads_path), "%s/logs/refs/heads", git_dir);
        mkdir(logs_path, 0755);
        mkdir(logs_refs_path, 0755);
        CHECK(mkdir(logs_heads_path, 0755) == 0 || errno == EEXIST, "mkdir logs/refs/heads failed");
    }
    CHECK(chmod(logs_heads_path, 0555) == 0, "chmod logs/refs/heads read-only failed");

    CHECK(sg_ref_update(git_dir, "refs/heads/x", id_a, "msg") == -1,
         "update with BOTH a foreign ref lock and an unwritable logs dir must fail");
    chmod(logs_heads_path, 0755); /* restore before any further reads/writes */

    CHECK(sg_ref_last_lock_err()->kind == SG_REF_LOCK_ERR_LOCKED,
         "the ref's own lock must be acquired/checked BEFORE the reflog append -- with the "
         "wrong order the reflog append fails first (an unrelated I/O error) and the "
         "recorded kind would be NONE, not LOCKED; got kind %d",
         sg_ref_last_lock_err()->kind);

    unlink(lock_path);
    free(git_dir);
}

/* ---- 10 (entry-clear coverage, Phase 77 fix round 3): every OTHER
   public writer that has its own entry clear() -- individually proven
   not to leak a stale LOCKED from an earlier, unrelated call into its
   OWN failure. Each case goes STRAIGHT from the stale-LOCKED setup call
   to the writer under test, with no intervening SUCCESSFUL write (an
   intervening success would re-clear the channel via its own entry
   clear, masking a missing clear in the function actually under test --
   see test_lock_err_does_not_leak_across_calls's own comment on this,
   confirmed the hard way there). ---- */

static void provoke_stale_locked(char *git_dir)
{
    unsigned char id[SG_SHA1_RAW_LEN];
    char lock_path[4096];
    int fd;

    fill_id(id, 0xee);
    snprintf(lock_path, sizeof(lock_path), "%s/refs/heads/leak_probe.lock", git_dir);
    fd = open(lock_path, O_CREAT | O_EXCL | O_WRONLY, 0666);
    CHECK(fd >= 0, "planting a foreign lock (leak-probe setup) failed");
    close(fd);
    CHECK(sg_ref_update(git_dir, "refs/heads/leak_probe", id, "msg") == -1,
         "leak-probe setup call should fail (foreign lock)");
    CHECK(sg_ref_last_lock_err()->kind == SG_REF_LOCK_ERR_LOCKED,
         "leak-probe setup call must record LOCKED (precondition)");
    CHECK(unlink(lock_path) == 0, "cleanup: removing the leak-probe lock failed");
}

static void test_update_locked_entry_clear(void)
{
    char *git_dir = make_tmp_repo();
    sg_ref_lock fake_lock;
    unsigned char id[SG_SHA1_RAW_LEN];

    fill_id(id, 0xff);
    memset(&fake_lock, 0, sizeof(fake_lock));
    provoke_stale_locked(git_dir);

    /* refs/tags/... is not in the reflog-allowed namespace -- refused
       without any lock ever being attempted. What this proves is that the
       namespace branch's OWN inline clear+set(OTHER) replaces the stale
       LOCKED; it does NOT prove an entry clear, because
       sg_ref_update_locked has none (removed in Phase 77 round 4 as a
       redundant guard: every exit clears for itself, so an entry clear
       was unobservable through any input). */
    CHECK(sg_ref_update_locked(git_dir, "refs/tags/nt", &fake_lock, id, "msg") == -1,
         "a disallowed reflog namespace must be refused");
    CHECK(sg_ref_last_lock_err()->kind == SG_REF_LOCK_ERR_OTHER,
         "must not leak the earlier LOCKED through -- got kind %d",
         sg_ref_last_lock_err()->kind);

    free(git_dir);
}

static void test_delete_under_entry_clear(void)
{
    char *git_dir = make_tmp_repo();

    provoke_stale_locked(git_dir);

    CHECK(sg_ref_delete_under(git_dir, "refs/heads/", "..") == -1,
         "an unsafe name must be refused");
    CHECK(sg_ref_last_lock_err()->kind == SG_REF_LOCK_ERR_NONE,
         "must not leak the earlier LOCKED through -- got kind %d",
         sg_ref_last_lock_err()->kind);

    free(git_dir);
}

static void test_set_head_entry_clear(void)
{
    char *git_dir = make_tmp_repo();
    char logs_path[4096];

    if (geteuid() == 0) {
        printf("SKIP test_set_head_entry_clear: running as root, chmod is ineffective\n");
        free(git_dir);
        return;
    }

    provoke_stale_locked(git_dir);

    snprintf(logs_path, sizeof(logs_path), "%s/logs", git_dir);
    CHECK(mkdir(logs_path, 0755) == 0 || errno == EEXIST, "mkdir logs failed");
    CHECK(chmod(logs_path, 0555) == 0, "chmod logs read-only failed");

    CHECK(sg_ref_set_head(git_dir, "master", "msg") == -1,
         "reflog append into a read-only logs dir must fail");
    chmod(logs_path, 0755);

    CHECK(sg_ref_last_lock_err()->kind == SG_REF_LOCK_ERR_NONE,
         "must not leak the earlier LOCKED through -- got kind %d",
         sg_ref_last_lock_err()->kind);

    free(git_dir);
}

static void test_set_head_detached_entry_clear(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id[SG_SHA1_RAW_LEN];
    char logs_path[4096];

    if (geteuid() == 0) {
        printf("SKIP test_set_head_detached_entry_clear: running as root, chmod is ineffective\n");
        free(git_dir);
        return;
    }

    fill_id(id, 0x11);
    provoke_stale_locked(git_dir);

    snprintf(logs_path, sizeof(logs_path), "%s/logs", git_dir);
    CHECK(mkdir(logs_path, 0755) == 0 || errno == EEXIST, "mkdir logs failed");
    CHECK(chmod(logs_path, 0555) == 0, "chmod logs read-only failed");

    CHECK(sg_ref_set_head_detached(git_dir, id, "msg") == -1,
         "reflog append into a read-only logs dir must fail");
    chmod(logs_path, 0755);

    CHECK(sg_ref_last_lock_err()->kind == SG_REF_LOCK_ERR_NONE,
         "must not leak the earlier LOCKED through -- got kind %d",
         sg_ref_last_lock_err()->kind);

    free(git_dir);
}

static void test_move_head_entry_clear(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id[SG_SHA1_RAW_LEN];
    char long_branch[5000];

    fill_id(id, 0x22);
    provoke_stale_locked(git_dir);

    memset(long_branch, 'a', sizeof(long_branch) - 1);
    long_branch[sizeof(long_branch) - 1] = '\0';

    CHECK(sg_ref_move_head(git_dir, long_branch, id, "msg") == -1,
         "an overlong branch name must be refused");
    CHECK(sg_ref_last_lock_err()->kind == SG_REF_LOCK_ERR_NONE,
         "must not leak the earlier LOCKED through -- got kind %d",
         sg_ref_last_lock_err()->kind);

    free(git_dir);
}

static void test_update_branch_entry_clear(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id[SG_SHA1_RAW_LEN];

    fill_id(id, 0x33);
    provoke_stale_locked(git_dir);

    CHECK(sg_ref_update_branch(git_dir, "..", id) == -1,
         "an unsafe branch name must be refused");
    CHECK(sg_ref_last_lock_err()->kind == SG_REF_LOCK_ERR_NONE,
         "must not leak the earlier LOCKED through -- got kind %d",
         sg_ref_last_lock_err()->kind);

    free(git_dir);
}

static void test_set_symref_entry_clear(void)
{
    char *git_dir = make_tmp_repo();

    provoke_stale_locked(git_dir);

    CHECK(sg_ref_set_symref(git_dir, "..", "refs/heads/master", NULL) == -1,
         "an unsafe ref_path must be refused");
    CHECK(sg_ref_last_lock_err()->kind == SG_REF_LOCK_ERR_NONE,
         "must not leak the earlier LOCKED through -- got kind %d",
         sg_ref_last_lock_err()->kind);

    free(git_dir);
}

/* ---- 11: sg_ref_set_symref's own dedicated lock test -- its ONE caller
   (sg clone, refs/remotes/<remote>/HEAD) was the last ref write in this
   project still bypassing the lock before F2 (Phase 77 fix round 2). ---- */

static void test_set_symref_foreign_lock_refuses(void)
{
    char *git_dir = make_tmp_repo();
    char remotes_dir[4096];
    char origin_dir[4096];
    char lock_path[4096];
    char ref_file[4096];
    int fd;
    struct stat st;

    snprintf(remotes_dir, sizeof(remotes_dir), "%s/refs/remotes", git_dir);
    snprintf(origin_dir, sizeof(origin_dir), "%s/refs/remotes/origin", git_dir);
    mkdir(remotes_dir, 0755);
    CHECK(mkdir(origin_dir, 0755) == 0, "mkdir refs/remotes/origin failed");

    snprintf(lock_path, sizeof(lock_path), "%s/refs/remotes/origin/HEAD.lock", git_dir);
    fd = open(lock_path, O_CREAT | O_EXCL | O_WRONLY, 0666);
    CHECK(fd >= 0, "planting a foreign lock failed");
    close(fd);

    CHECK(sg_ref_set_symref(git_dir, "refs/remotes/origin/HEAD", "refs/remotes/origin/master", NULL) ==
             -1,
         "a foreign lock must refuse the write");
    CHECK(sg_ref_last_lock_err()->kind == SG_REF_LOCK_ERR_LOCKED,
         "the recorded failure kind must be SG_REF_LOCK_ERR_LOCKED, got %d",
         sg_ref_last_lock_err()->kind);
    CHECK(stat(lock_path, &st) == 0, "the foreign lock itself must survive the refusal");

    snprintf(ref_file, sizeof(ref_file), "%s/refs/remotes/origin/HEAD", git_dir);
    CHECK(stat(ref_file, &st) != 0, "the ref file must NOT have been created by the refused write");

    unlink(lock_path);
    free(git_dir);
}

/* Phase 77 round 6: sg_ref_update_locked's reflog-append failure branch
   must record its OWN kind. The function has no entry clear (deleted in
   round 4 as redundant), so this branch's lock_err_set is the only thing
   replacing a stale record: without it the stale LOCKED planted first
   would survive and this -1 would be reported as a lock failure. */
static void test_update_locked_reflog_failure_sets_kind(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char id[SG_SHA1_RAW_LEN];
    char logs_path[4096];
    char logs_refs_path[4096];
    char logs_heads_path[4096];
    char ref_file[4096];
    sg_ref_lock lock;
    struct stat st;

    if (geteuid() == 0) {
        printf("SKIP test_update_locked_reflog_failure_sets_kind: running as root, chmod is "
              "ineffective\n");
        free(git_dir);
        return;
    }

    fill_id(id, 0xdd);
    provoke_stale_locked(git_dir);

    memset(&lock, 0, sizeof(lock));
    CHECK(sg_ref_lock_try(git_dir, "refs/heads/", "y", &lock) == SG_REFLOCK_ACQUIRED,
         "taking refs/heads/y.lock (caller-held, as cmd_branch.c does) failed");

    snprintf(logs_path, sizeof(logs_path), "%s/logs", git_dir);
    snprintf(logs_refs_path, sizeof(logs_refs_path), "%s/logs/refs", git_dir);
    snprintf(logs_heads_path, sizeof(logs_heads_path), "%s/logs/refs/heads", git_dir);
    mkdir(logs_path, 0755);
    mkdir(logs_refs_path, 0755);
    CHECK(mkdir(logs_heads_path, 0755) == 0 || errno == EEXIST, "mkdir logs/refs/heads failed");
    CHECK(chmod(logs_heads_path, 0555) == 0, "chmod logs/refs/heads read-only failed");

    CHECK(sg_ref_update_locked(git_dir, "refs/heads/y", &lock, id, "msg") == -1,
         "a reflog append into a read-only logs/refs/heads must fail the update");
    chmod(logs_heads_path, 0755); /* restore before any further reads/writes */

    CHECK(sg_ref_last_lock_err()->kind == SG_REF_LOCK_ERR_OTHER,
         "the reflog-append failure must record OTHER itself, replacing the stale LOCKED "
         "planted first -- got kind %d", sg_ref_last_lock_err()->kind);
    snprintf(ref_file, sizeof(ref_file), "%s/refs/heads/y", git_dir);
    CHECK(stat(ref_file, &st) != 0, "the refused update must not create refs/heads/y");

    sg_ref_lock_release(&lock);
    free(git_dir);
}

int main(void)
{
    test_foreign_lock_refuses_write();
    test_success_leaves_no_lock_file();
    test_failure_leaves_no_lock_file();
    test_empty_dir_at_ref_path_removed();
    test_nonempty_dir_at_ref_path_refused();
    test_write_is_atomic_rename_not_in_place_truncate();
    test_lock_err_does_not_leak_across_calls();
    test_update_locked_rejects_unsafe_ref_path();
    test_update_locks_ref_before_reflog_append();
    test_update_locked_entry_clear();
    test_update_locked_reflog_failure_sets_kind();
    test_delete_under_entry_clear();
    test_set_head_entry_clear();
    test_set_head_detached_entry_clear();
    test_move_head_entry_clear();
    test_update_branch_entry_clear();
    test_set_symref_entry_clear();
    test_set_symref_foreign_lock_refuses();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all ref locked-write tests passed\n");
    return 0;
}
