/* Differential harness for the two independent "what changed between the
   index and the working tree" implementations: sg_status_diff_unstaged
   (src/workdir/status.c) and sg_diff_index_workdir (src/workdir/diff.c).
   Both answer the same question -- Phase 26 confirmed at least one place
   where they disagreed (mode-only changes) purely by accident, with no test
   catching it, plus a second divergence (a broken chunk pointer aborting the
   whole status scan) found while writing this file.

   As of the collapse of sg_status_diff_unstaged into a thin adapter over
   sg_diff_index_workdir (see status.c), only ONE divergence remains, and it
   is deliberate:
     - mode-only changes: FIXED -- sg_status_diff_unstaged now goes through
       the same builder, so it sees mode too.
     - a broken/unresolvable chunk pointer aborting the entire scan: FIXED --
       the adapter can no longer hard-fail on a single bad path, since it is
       sg_diff_index_workdir doing the walk.
     - unmerged paths: KEPT, on purpose. sg_status_diff_unstaged skips every
       row sg_diff_index_workdir marks `unmerged`, AND the row immediately
       following it when that row shares the same path (the stage-2-vs-
       workdir companion row sg_diff_index_workdir may also emit) -- `sg
       status` reports conflicts through its own "Unmerged paths" section
       instead of this list.
   This file still enumerates all of this deliberately and pins it down as a
   regression detector, so a future change to either side is forced to be a
   conscious decision rather than a silent drift back apart.

   Normalizing sg_diff_list -> sg_status_kind (derived from the two headers'
   documented contracts, sg/diff.h and sg/status.h):
     old ABSENT + new present -> SG_STATUS_NEW (not actually reachable through
                                  sg_status_diff_unstaged today, since every
                                  row it sees comes from an existing idx
                                  entry -- old is therefore always BLOB -- but
                                  documented for completeness)
     old BLOB   + new ABSENT  -> SG_STATUS_DELETED
     old BLOB   + new WORKDIR -> SG_STATUS_MODIFIED (also what a pure mode
                                  change now maps to, since both sides are
                                  "present" regardless of whether content or
                                  mode is what actually differs)
     unmerged row (and its stage-2 companion row) -> skipped entirely, no
                                  sg_status_kind equivalent.
   Both builders only ever operate on paths that already have an index entry
   (that's what they're iterating), so SG_STATUS_NEW never occurs here in
   practice -- it is sg_status_diff_staged's/sg_diff_tree_index's shape (HEAD
   vs index), not this pair's.

   Style follows tests/test_fuzz_pack.c: deterministic xorshift64 PRNG (not
   rand(), for cross-platform reproducibility), SG_FUZZ_ITERS/
   SG_FUZZ_SEED_BASE env vars, and every failure message names the exact
   seed to reproduce it. Named deterministic scenario tests pin the specific
   divergences (and now-fixed former divergences) called out in the task; the
   randomized fuzz loop combines many independent per-path scenarios in one
   repo to look for interactions or additional divergences neither of us
   anticipated. */
#include "sg/chunk.h"
#include "sg/diff.h"
#include "sg/hash.h"
#include "sg/index.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/repo.h"
#include "sg/status.h"
#include "sg/workdir.h"

#include <stdint.h>
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

/* ---- xorshift64 PRNG (deterministic, not rand()) ---- */
static uint64_t g_rng_state;

static void seed_prng(uint64_t seed)
{
    g_rng_state = seed ^ 0x9E3779B97F4A7C15ULL;
    if (g_rng_state == 0)
        g_rng_state = 0x9E3779B97F4A7C15ULL;
}

static uint64_t next_rand(void)
{
    uint64_t x = g_rng_state;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rng_state = x;
    return x;
}

static uint32_t rand_u32(void)
{
    return (uint32_t)(next_rand() >> 32);
}

static unsigned char rand_byte(void)
{
    return (unsigned char)next_rand();
}

static uint32_t rand_below(uint32_t n)
{
    if (n == 0)
        return 0;
    return rand_u32() % n;
}

static long env_long(const char *name, long def)
{
    const char *v = getenv(name);

    if (v == NULL || *v == '\0')
        return def;
    return strtol(v, NULL, 10);
}

/* ---- repo/fs helpers (same shape as tests/test_diff_list.c's) ---- */

static char *make_tmp_repo(const char *tag)
{
    char template[128];
    char *path;
    char git_dir[SG_PATH_MAX];

    snprintf(template, sizeof(template), "/tmp/sg_status_diff_parity_%s_XXXXXX", tag);
    path = strdup(template);
    if (path == NULL || mkdtemp(path) == NULL) {
        fprintf(stderr, "setup failed: mkdtemp\n");
        exit(1);
    }
    if (sg_repo_init(path) != 0) {
        fprintf(stderr, "setup failed: sg_repo_init\n");
        exit(1);
    }
    snprintf(git_dir, sizeof(git_dir), "%s/.git", path);
    free(path);
    return strdup(git_dir);
}

static void rm_rf(const char *path)
{
    char cmd[SG_PATH_MAX + 16];

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    if (system(cmd) != 0) { /* best-effort cleanup */
    }
}

static void write_workdir_bytes(const char *repo_root, const char *rel, const unsigned char *data,
                                size_t len)
{
    char abspath[SG_PATH_MAX];

    if (sg_path_join(abspath, sizeof(abspath), repo_root, rel) != 0) {
        fprintf(stderr, "setup failed: path too long for %s\n", rel);
        exit(1);
    }
    if (sg_write_file_mkdirs(abspath, data, len, 0644) != 0) {
        fprintf(stderr, "setup failed: sg_write_file_mkdirs %s\n", rel);
        exit(1);
    }
}

static void chmod_workdir(const char *repo_root, const char *rel, int mode)
{
    char abspath[SG_PATH_MAX];

    if (sg_path_join(abspath, sizeof(abspath), repo_root, rel) != 0) {
        fprintf(stderr, "setup failed: path too long for %s\n", rel);
        exit(1);
    }
    if (chmod(abspath, (mode_t)mode) != 0) {
        fprintf(stderr, "setup failed: chmod %s\n", rel);
        exit(1);
    }
}

static void mkdir_workdir(const char *repo_root, const char *rel)
{
    char abspath[SG_PATH_MAX];

    if (sg_path_join(abspath, sizeof(abspath), repo_root, rel) != 0) {
        fprintf(stderr, "setup failed: path too long for %s\n", rel);
        exit(1);
    }
    if (mkdir(abspath, 0755) != 0) {
        fprintf(stderr, "setup failed: mkdir %s\n", rel);
        exit(1);
    }
}

static void blob_write(const char *git_dir, const unsigned char *content, size_t len,
                      unsigned char id[SG_SHA1_RAW_LEN])
{
    if (sg_loose_write(git_dir, SG_OBJ_BLOB, content, len, id) != 0) {
        fprintf(stderr, "setup failed: sg_loose_write\n");
        exit(1);
    }
}

static void index_upsert(sg_index *idx, const char *path, unsigned int stage, unsigned int mode,
                         const unsigned char id[SG_SHA1_RAW_LEN])
{
    sg_index_entry e;

    memset(&e, 0, sizeof(e));
    e.mode = mode;
    e.stage = stage;
    memcpy(e.sha1, id, SG_SHA1_RAW_LEN);
    e.path = (char *)path;
    if (sg_index_upsert(idx, &e) != 0) {
        fprintf(stderr, "setup failed: sg_index_upsert %s stage %u\n", path, stage);
        exit(1);
    }
}

static void rand_bytes(unsigned char *buf, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
        buf[i] = rand_byte();
}

/* Result lookup helpers -- both output lists are unordered w.r.t. each
   other's classification, so tests search by path rather than assuming
   position. */

static const sg_status_entry *status_find(const sg_status_list *list, const char *path)
{
    size_t i;

    for (i = 0; i < list->count; i++) {
        if (strcmp(list->entries[i].path, path) == 0)
            return &list->entries[i];
    }
    return NULL;
}

static size_t status_count(const sg_status_list *list, const char *path)
{
    size_t i, n = 0;

    for (i = 0; i < list->count; i++) {
        if (strcmp(list->entries[i].path, path) == 0)
            n++;
    }
    return n;
}

static size_t diff_count(const sg_diff_list *list, const char *path)
{
    size_t i, n = 0;

    for (i = 0; i < list->count; i++) {
        if (strcmp(list->entries[i].path, path) == 0)
            n++;
    }
    return n;
}

static size_t diff_count_unmerged(const sg_diff_list *list, const char *path)
{
    size_t i, n = 0;

    for (i = 0; i < list->count; i++) {
        if (strcmp(list->entries[i].path, path) == 0 && list->entries[i].unmerged)
            n++;
    }
    return n;
}

/* ==================================================================== *
 * Named deterministic scenarios -- one per documented/predicted
 * divergence, plus a couple of "these actually agree" sanity checks.
 * ==================================================================== */

/* Former divergence #1 (Phase 26), now FIXED by collapsing
   sg_status_diff_unstaged into an adapter over sg_diff_index_workdir: a bare
   chmod with unchanged content is real signal (`git status --porcelain`
   prints " M" for this, per the task's oracle), and both sides now report
   it. */
static void test_mode_only_now_reported(void)
{
    char *git_dir = make_tmp_repo("mode_only");
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    unsigned char id[SG_SHA1_RAW_LEN];
    const unsigned char content[] = "same bytes, different mode\n";
    sg_status_list slist;
    sg_diff_list dlist;

    memset(&idx, 0, sizeof(idx));
    blob_write(git_dir, content, sizeof(content) - 1, id);
    index_upsert(&idx, "exec.txt", 0, 0100644, id);
    write_workdir_bytes(repo_root, "exec.txt", content, sizeof(content) - 1);
    chmod_workdir(repo_root, "exec.txt", 0755);

    CHECK(sg_status_diff_unstaged(git_dir, repo_root, &idx, &slist) == 0,
         "sg_status_diff_unstaged failed");
    {
        const sg_status_entry *e = status_find(&slist, "exec.txt");

        CHECK(e != NULL && e->kind == SG_STATUS_MODIFIED,
             "PARITY RESTORED (expected): sg_status_diff_unstaged should now report a mode-only "
             "chmod as SG_STATUS_MODIFIED, same as sg_diff_index_workdir -- has the adapter "
             "stopped going through the diff builder?");
    }

    CHECK(sg_diff_index_workdir(git_dir, repo_root, &idx, &dlist) == 0,
         "sg_diff_index_workdir failed");
    {
        const sg_diff_entry *e = NULL;
        size_t i;

        for (i = 0; i < dlist.count; i++) {
            if (strcmp(dlist.entries[i].path, "exec.txt") == 0)
                e = &dlist.entries[i];
        }
        CHECK(e != NULL,
             "sg_diff_index_workdir should report exec.txt as changed (mode 100644 -> 100755) "
             "-- did it stop comparing mode?");
        if (e != NULL) {
            CHECK(e->old_side.mode == 0100644 && e->new_side.mode == 0100755,
                 "expected old mode 100644 / new mode 100755, got %o/%o", e->old_side.mode,
                 e->new_side.mode);
            CHECK(memcmp(e->old_side.id, e->new_side.id, SG_SHA1_RAW_LEN) == 0,
                 "content id should be identical -- only the mode changed");
        }
    }

    sg_status_list_free(&slist);
    sg_diff_list_free(&dlist);
    sg_index_free(&idx);
    rm_rf(repo_root);
    free(repo_root);
    free(git_dir);
}

/* Divergence #2: an unresolved conflict with only stage 1/3 (no stage 2).
   sg_status_diff_unstaged silently skips every non-zero-stage entry (its own
   documented contract -- "sg status"'s separate "Unmerged paths" section is
   what surfaces these). sg_diff_index_workdir always emits the fixed
   "unmerged" row. This is very likely a DELIBERATE divergence (both
   functions say so in their own header comments), not a bug -- included
   here so the deliberate-ness is pinned down as an assertion, not just a
   comment someone could stop believing. */
static void test_unmerged_no_stage2(void)
{
    char *git_dir = make_tmp_repo("unmerged_ns2");
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    unsigned char base_id[SG_SHA1_RAW_LEN], theirs_id[SG_SHA1_RAW_LEN];
    const unsigned char base_content[] = "base\n";
    const unsigned char theirs_content[] = "theirs\n";
    const unsigned char wd_content[] = "whatever is on disk\n";
    sg_status_list slist;
    sg_diff_list dlist;

    memset(&idx, 0, sizeof(idx));
    blob_write(git_dir, base_content, sizeof(base_content) - 1, base_id);
    blob_write(git_dir, theirs_content, sizeof(theirs_content) - 1, theirs_id);
    index_upsert(&idx, "conflict.txt", 1, 0100644, base_id);
    index_upsert(&idx, "conflict.txt", 3, 0100644, theirs_id);
    write_workdir_bytes(repo_root, "conflict.txt", wd_content, sizeof(wd_content) - 1);

    CHECK(sg_status_diff_unstaged(git_dir, repo_root, &idx, &slist) == 0,
         "sg_status_diff_unstaged failed");
    CHECK(status_count(&slist, "conflict.txt") == 0,
         "sg_status_diff_unstaged should report nothing for an unresolved conflict path "
         "(no stage-0 entry) -- got %zu entries",
         status_count(&slist, "conflict.txt"));

    CHECK(sg_diff_index_workdir(git_dir, repo_root, &idx, &dlist) == 0,
         "sg_diff_index_workdir failed");
    CHECK(diff_count(&dlist, "conflict.txt") == 1,
         "sg_diff_index_workdir should report exactly one row (the unmerged row, no stage-2 "
         "row since there is no stage 2) for conflict.txt, got %zu",
         diff_count(&dlist, "conflict.txt"));
    CHECK(diff_count_unmerged(&dlist, "conflict.txt") == 1, "the one row should be unmerged");

    sg_status_list_free(&slist);
    sg_diff_list_free(&dlist);
    sg_index_free(&idx);
    rm_rf(repo_root);
    free(repo_root);
    free(git_dir);
}

/* Divergence #3: unresolved conflict WITH a stage 2 whose content differs
   from the working tree. Per sg/diff.h's measured-against-git-2.55.0
   contract, sg_diff_index_workdir emits TWO rows here (unmerged + a second
   ordinary "stage 2 vs workdir" row). sg_status_diff_unstaged still reports
   nothing at all, for the same reason as test_unmerged_no_stage2. */
static void test_unmerged_stage2_differs(void)
{
    char *git_dir = make_tmp_repo("unmerged_s2d");
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    unsigned char base_id[SG_SHA1_RAW_LEN], ours_id[SG_SHA1_RAW_LEN], theirs_id[SG_SHA1_RAW_LEN];
    const unsigned char base_content[] = "base\n";
    const unsigned char ours_content[] = "ours\n";
    const unsigned char theirs_content[] = "theirs\n";
    const unsigned char wd_content[] = "neither -- still mid-edit\n";
    sg_status_list slist;
    sg_diff_list dlist;

    memset(&idx, 0, sizeof(idx));
    blob_write(git_dir, base_content, sizeof(base_content) - 1, base_id);
    blob_write(git_dir, ours_content, sizeof(ours_content) - 1, ours_id);
    blob_write(git_dir, theirs_content, sizeof(theirs_content) - 1, theirs_id);
    index_upsert(&idx, "conflict.txt", 1, 0100644, base_id);
    index_upsert(&idx, "conflict.txt", 2, 0100644, ours_id);
    index_upsert(&idx, "conflict.txt", 3, 0100644, theirs_id);
    write_workdir_bytes(repo_root, "conflict.txt", wd_content, sizeof(wd_content) - 1);

    CHECK(sg_status_diff_unstaged(git_dir, repo_root, &idx, &slist) == 0,
         "sg_status_diff_unstaged failed");
    CHECK(status_count(&slist, "conflict.txt") == 0,
         "sg_status_diff_unstaged should report nothing for an unresolved conflict path -- got "
         "%zu entries",
         status_count(&slist, "conflict.txt"));

    CHECK(sg_diff_index_workdir(git_dir, repo_root, &idx, &dlist) == 0,
         "sg_diff_index_workdir failed");
    CHECK(diff_count(&dlist, "conflict.txt") == 2,
         "sg_diff_index_workdir should report two rows (unmerged + stage-2-vs-workdir, since "
         "stage 2's content differs from what's on disk) for conflict.txt, got %zu",
         diff_count(&dlist, "conflict.txt"));
    CHECK(diff_count_unmerged(&dlist, "conflict.txt") == 1, "exactly one of the two rows should be unmerged");

    sg_status_list_free(&slist);
    sg_diff_list_free(&dlist);
    sg_index_free(&idx);
    rm_rf(repo_root);
    free(repo_root);
    free(git_dir);
}

/* Companion to the above: stage 2's content EXACTLY matches the working
   tree (mode included). The second row must then disappear -- this is what
   sg/diff.h's contract uses to distinguish "still conflicted" from
   "resolved to stage 2's exact bytes". Included as its own test since it is
   the one case where diff_count could silently regress from 1 to 2 without
   test_unmerged_no_stage2/test_unmerged_stage2_differs (which use different
   stage combinations) ever noticing. */
static void test_unmerged_stage2_matches_workdir(void)
{
    char *git_dir = make_tmp_repo("unmerged_s2m");
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    unsigned char base_id[SG_SHA1_RAW_LEN], ours_id[SG_SHA1_RAW_LEN], theirs_id[SG_SHA1_RAW_LEN];
    const unsigned char base_content[] = "base\n";
    const unsigned char ours_content[] = "ours -- exactly what's on disk\n";
    const unsigned char theirs_content[] = "theirs\n";
    sg_status_list slist;
    sg_diff_list dlist;

    memset(&idx, 0, sizeof(idx));
    blob_write(git_dir, base_content, sizeof(base_content) - 1, base_id);
    blob_write(git_dir, ours_content, sizeof(ours_content) - 1, ours_id);
    blob_write(git_dir, theirs_content, sizeof(theirs_content) - 1, theirs_id);
    index_upsert(&idx, "conflict.txt", 1, 0100644, base_id);
    index_upsert(&idx, "conflict.txt", 2, 0100644, ours_id);
    index_upsert(&idx, "conflict.txt", 3, 0100644, theirs_id);
    write_workdir_bytes(repo_root, "conflict.txt", ours_content, sizeof(ours_content) - 1);

    CHECK(sg_status_diff_unstaged(git_dir, repo_root, &idx, &slist) == 0,
         "sg_status_diff_unstaged failed");
    CHECK(status_count(&slist, "conflict.txt") == 0, "sg_status_diff_unstaged should report nothing");

    CHECK(sg_diff_index_workdir(git_dir, repo_root, &idx, &dlist) == 0,
         "sg_diff_index_workdir failed");
    CHECK(diff_count(&dlist, "conflict.txt") == 1,
         "stage 2 matches the working tree exactly, so only the unmerged row should remain -- "
         "got %zu rows",
         diff_count(&dlist, "conflict.txt"));
    CHECK(diff_count_unmerged(&dlist, "conflict.txt") == 1, "the remaining row should be unmerged");

    sg_status_list_free(&slist);
    sg_diff_list_free(&dlist);
    sg_index_free(&idx);
    rm_rf(repo_root);
    free(repo_root);
    free(git_dir);
}

/* Former divergence #4 (most severe found before this task), now FIXED by
   the same collapse as the mode-only case: an index entry whose sha1 does
   not resolve to any object at all (simulating a broken/garbage chunk
   pointer id -- sg_chunk_effective_id's -1 path, "the underlying object read
   of id itself fails", is reached the same way whether the id is a genuinely
   corrupt chunk pointer or simply garbage; both hit the identical
   `sg_object_read` failure inside chunk_resolve).

   sg_status_diff_unstaged used to hard-fail (-1) here, aborting the ENTIRE
   call and silently discarding whatever it already found for every other
   path -- since it iterated idx in (sorted) path order, a broken entry
   sorting BEFORE another, perfectly ordinary modified path meant that
   modified path was never even reached, let alone reported.

   Now that sg_status_diff_unstaged is a thin adapter over
   sg_diff_index_workdir, it inherits that builder's resilience (sg/diff.h:
   "must NOT fail the whole call... records the path as changed and leaves
   the complaining to the renderer"): the broken path is reported as an
   ordinary SG_STATUS_MODIFIED entry, and the unrelated, real modification is
   still found too. */
static void test_chunked_broken_pointer_now_resilient(void)
{
    char *git_dir = make_tmp_repo("broken_chunk");
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    unsigned char garbage_id[SG_SHA1_RAW_LEN];
    unsigned char old_id[SG_SHA1_RAW_LEN];
    const unsigned char broken_wd_content[] = "whatever -- the pointer itself can't be resolved\n";
    const unsigned char old_content[] = "before\n";
    const unsigned char new_content[] = "after -- a perfectly ordinary modification\n";
    sg_status_list slist;
    sg_diff_list dlist;
    int status_rc;

    memset(&idx, 0, sizeof(idx));
    memset(garbage_id, 0xAB, sizeof(garbage_id)); /* not any object this fresh repo has */

    /* "aaa_broken.txt" sorts before "zzz_modified.txt", so if the broken
       entry aborted the whole scan, zzz_modified.txt's real, unrelated
       modification would never even get a chance to be seen. */
    index_upsert(&idx, "aaa_broken.txt", 0, 0100644, garbage_id);
    write_workdir_bytes(repo_root, "aaa_broken.txt", broken_wd_content, sizeof(broken_wd_content) - 1);

    blob_write(git_dir, old_content, sizeof(old_content) - 1, old_id);
    index_upsert(&idx, "zzz_modified.txt", 0, 0100644, old_id);
    write_workdir_bytes(repo_root, "zzz_modified.txt", new_content, sizeof(new_content) - 1);

    status_rc = sg_status_diff_unstaged(git_dir, repo_root, &idx, &slist);
    CHECK(status_rc == 0,
         "PARITY RESTORED (expected): sg_status_diff_unstaged should no longer hard-fail when an "
         "index entry's chunk pointer can't be resolved -- it returned %d",
         status_rc);
    if (status_rc == 0) {
        const sg_status_entry *broken = status_find(&slist, "aaa_broken.txt");
        const sg_status_entry *modified = status_find(&slist, "zzz_modified.txt");

        CHECK(broken != NULL && broken->kind == SG_STATUS_MODIFIED,
             "the broken-pointer path itself should still be reported as changed (renderer's job "
             "to diagnose further)");
        CHECK(modified != NULL && modified->kind == SG_STATUS_MODIFIED,
             "the unrelated, perfectly ordinary modification must still be reported -- a "
             "hard-failing status.c would have silently dropped this");
        sg_status_list_free(&slist);
    }

    CHECK(sg_diff_index_workdir(git_dir, repo_root, &idx, &dlist) == 0,
         "sg_diff_index_workdir should NOT fail the whole call over one broken pointer");
    CHECK(diff_count(&dlist, "aaa_broken.txt") == 1,
         "the broken-pointer path itself should still be reported as changed (renderer's job to "
         "diagnose further), got %zu rows",
         diff_count(&dlist, "aaa_broken.txt"));
    CHECK(diff_count(&dlist, "zzz_modified.txt") == 1,
         "sg_diff_index_workdir must still find the unrelated, perfectly ordinary modification "
         "that a hard-failing status.c would have silently dropped -- got %zu rows",
         diff_count(&dlist, "zzz_modified.txt"));

    sg_diff_list_free(&dlist);
    sg_index_free(&idx);
    rm_rf(repo_root);
    free(repo_root);
    free(git_dir);
}

/* Sanity check: a tracked path replaced on disk by a directory. Both
   builders document treating "exists but unreadable as a file" as absent
   (status.c: SG_STATUS_DELETED; diff.c: SG_DIFF_SIDE_ABSENT, per sg/diff.h's
   explicit cross-reference to sg_status_diff_unstaged) -- this is a place
   they are DELIBERATELY kept in sync, so it's asserted here as a
   non-divergence, to catch a future edit that breaks the symmetry the
   header comments claim. Directory substitution, not chmod 000: this
   project's own convention (CLAUDE.md/Phase 26) since chmod 000 is a no-op
   under root. */
static void test_unreadable_path_agrees(void)
{
    char *git_dir = make_tmp_repo("unreadable");
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    unsigned char id[SG_SHA1_RAW_LEN];
    const unsigned char content[] = "used to be a file\n";
    sg_status_list slist;
    sg_diff_list dlist;

    memset(&idx, 0, sizeof(idx));
    blob_write(git_dir, content, sizeof(content) - 1, id);
    index_upsert(&idx, "was_a_file", 0, 0100644, id);
    mkdir_workdir(repo_root, "was_a_file");

    CHECK(sg_status_diff_unstaged(git_dir, repo_root, &idx, &slist) == 0,
         "sg_status_diff_unstaged failed");
    {
        const sg_status_entry *e = status_find(&slist, "was_a_file");

        CHECK(e != NULL && e->kind == SG_STATUS_DELETED,
             "expected was_a_file to be reported SG_STATUS_DELETED once it's a directory");
    }

    CHECK(sg_diff_index_workdir(git_dir, repo_root, &idx, &dlist) == 0,
         "sg_diff_index_workdir failed");
    {
        size_t i;
        const sg_diff_entry *e = NULL;

        for (i = 0; i < dlist.count; i++) {
            if (strcmp(dlist.entries[i].path, "was_a_file") == 0)
                e = &dlist.entries[i];
        }
        CHECK(e != NULL && e->new_side.kind == SG_DIFF_SIDE_ABSENT,
             "expected was_a_file's diff row to have new_side ABSENT (matching status's "
             "DELETED), i.e. the two builders should agree here");
    }

    sg_status_list_free(&slist);
    sg_diff_list_free(&dlist);
    sg_index_free(&idx);
    rm_rf(repo_root);
    free(repo_root);
    free(git_dir);
}

/* Sanity check: a chunked blob (content stored via sg_chunk_store_blob,
   forced to actually chunk with a tiny threshold) that hasn't changed must
   normalize to "no change" on both sides -- each has its own independent
   sg_chunk_effective_id call site (status.c and diff.c), and a fixture that
   only exercises one leaves the other's normalization completely
   unmeasured, per tests/test_diff_list.c's own comment on this. */
static void test_chunked_unmodified_agrees(void)
{
    char *git_dir = make_tmp_repo("chunk_ok");
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    size_t len = SG_CHUNK_MIN_SIZE * 2;
    unsigned char *data = malloc(len);
    unsigned char pointer_id[SG_SHA1_RAW_LEN];
    int chunked = -1;
    sg_status_list slist;
    sg_diff_list dlist;

    CHECK(data != NULL, "malloc failed for chunk fixture");
    if (data == NULL) {
        free(repo_root);
        free(git_dir);
        return;
    }
    seed_prng(0xC0FFEEULL);
    rand_bytes(data, len);

    CHECK(sg_chunk_store_blob(git_dir, data, len, 1024, pointer_id, &chunked) == 0 && chunked == 1,
         "sg_chunk_store_blob should have produced a chunk pointer");

    memset(&idx, 0, sizeof(idx));
    index_upsert(&idx, "big.bin", 0, 0100644, pointer_id);
    write_workdir_bytes(repo_root, "big.bin", data, len);

    CHECK(sg_status_diff_unstaged(git_dir, repo_root, &idx, &slist) == 0,
         "sg_status_diff_unstaged failed");
    CHECK(status_count(&slist, "big.bin") == 0,
         "unmodified chunked content should not be reported by sg_status_diff_unstaged");

    CHECK(sg_diff_index_workdir(git_dir, repo_root, &idx, &dlist) == 0,
         "sg_diff_index_workdir failed");
    CHECK(diff_count(&dlist, "big.bin") == 0,
         "unmodified chunked content should not be reported by sg_diff_index_workdir");

    sg_status_list_free(&slist);
    sg_diff_list_free(&dlist);
    sg_index_free(&idx);
    free(data);
    rm_rf(repo_root);
    free(repo_root);
    free(git_dir);
}

/* Sanity check: an untracked file (no index entry at all) must never appear
   in either builder's output -- both only ever iterate the index, so this
   should be structurally impossible on either side, but pinning it down
   costs nothing and documents the shared assumption. */
static void test_untracked_file_not_reported(void)
{
    char *git_dir = make_tmp_repo("untracked");
    char *repo_root = sg_repo_root(git_dir);
    sg_index idx;
    const unsigned char content[] = "nobody asked to track me\n";
    sg_status_list slist;
    sg_diff_list dlist;

    memset(&idx, 0, sizeof(idx));
    write_workdir_bytes(repo_root, "loose.txt", content, sizeof(content) - 1);

    CHECK(sg_status_diff_unstaged(git_dir, repo_root, &idx, &slist) == 0,
         "sg_status_diff_unstaged failed");
    CHECK(status_count(&slist, "loose.txt") == 0, "untracked file must not appear in status diff");

    CHECK(sg_diff_index_workdir(git_dir, repo_root, &idx, &dlist) == 0,
         "sg_diff_index_workdir failed");
    CHECK(diff_count(&dlist, "loose.txt") == 0, "untracked file must not appear in index/workdir diff");

    sg_status_list_free(&slist);
    sg_diff_list_free(&dlist);
    sg_index_free(&idx);
    rm_rf(repo_root);
    free(repo_root);
    free(git_dir);
}

/* ==================================================================== *
 * Randomized parity fuzzer: many independent per-path scenarios combined
 * into one repo per round, cross-checked against the mapping documented at
 * the top of this file. There is no SC_CHUNKED_BROKEN scenario in this pool
 * (unlike an earlier version of this file): now that
 * sg_status_diff_unstaged no longer hard-fails the whole call over one
 * broken chunk pointer, it could in principle be mixed in like any other
 * scenario, but it stays covered on its own, deterministically, above
 * (test_chunked_broken_pointer_now_resilient) rather than being added here.
 * SC_UNTRACKED is fine to mix in (it never touches the index).
 * ==================================================================== */

typedef enum {
    SC_CLEAN,
    SC_MODIFIED,
    SC_DELETED,
    SC_MODE_ONLY,
    SC_EMPTY_UNCHANGED,
    SC_BINARY_MODIFIED,
    SC_CHUNKED_UNMODIFIED,
    SC_CHUNKED_MODIFIED,
    SC_UNTRACKED,
    SC_UNMERGED_NO_STAGE2,
    SC_UNMERGED_STAGE2_MATCH,
    SC_UNMERGED_STAGE2_DIFFER,
    SC_COUNT
} scenario;

/* What each side is expected to report for one scenario's path, filled in by
   setup_one(). diff_rows[i].unmerged/changed describe row i (order: the
   unmerged row, if any, always comes first, matching sg_diff_index_workdir's
   documented emission order). */
typedef struct {
    int status_reported; /* 0 or 1 */
    int diff_row_count;  /* 0, 1, or 2 */
    int diff_row_unmerged[2];
} expectation;

static void setup_one(const char *git_dir, const char *repo_root, sg_index *idx, const char *path,
                      scenario sc, expectation *exp)
{
    memset(exp, 0, sizeof(*exp));

    switch (sc) {
    case SC_CLEAN: {
        unsigned char id[SG_SHA1_RAW_LEN];
        const unsigned char content[] = "clean, unmodified content\n";

        blob_write(git_dir, content, sizeof(content) - 1, id);
        index_upsert(idx, path, 0, 0100644, id);
        write_workdir_bytes(repo_root, path, content, sizeof(content) - 1);
        exp->status_reported = 0;
        exp->diff_row_count = 0;
        break;
    }
    case SC_MODIFIED: {
        unsigned char id[SG_SHA1_RAW_LEN];
        const unsigned char old_content[] = "before the edit\n";
        const unsigned char new_content[] = "after the edit, definitely different\n";

        blob_write(git_dir, old_content, sizeof(old_content) - 1, id);
        index_upsert(idx, path, 0, 0100644, id);
        write_workdir_bytes(repo_root, path, new_content, sizeof(new_content) - 1);
        exp->status_reported = 1;
        exp->diff_row_count = 1;
        break;
    }
    case SC_DELETED: {
        unsigned char id[SG_SHA1_RAW_LEN];
        const unsigned char content[] = "will be deleted\n";

        blob_write(git_dir, content, sizeof(content) - 1, id);
        index_upsert(idx, path, 0, 0100644, id);
        /* deliberately never written to the working tree */
        exp->status_reported = 1;
        exp->diff_row_count = 1;
        break;
    }
    case SC_MODE_ONLY: {
        unsigned char id[SG_SHA1_RAW_LEN];
        const unsigned char content[] = "same bytes, different mode bit\n";

        blob_write(git_dir, content, sizeof(content) - 1, id);
        index_upsert(idx, path, 0, 0100644, id);
        write_workdir_bytes(repo_root, path, content, sizeof(content) - 1);
        chmod_workdir(repo_root, path, 0755);
        /* PARITY RESTORED: status now goes through the same builder, so it
           reports this row too. */
        exp->status_reported = 1;
        exp->diff_row_count = 1;
        break;
    }
    case SC_EMPTY_UNCHANGED: {
        unsigned char id[SG_SHA1_RAW_LEN];

        blob_write(git_dir, (const unsigned char *)"", 0, id);
        index_upsert(idx, path, 0, 0100644, id);
        write_workdir_bytes(repo_root, path, (const unsigned char *)"", 0);
        exp->status_reported = 0;
        exp->diff_row_count = 0;
        break;
    }
    case SC_BINARY_MODIFIED: {
        unsigned char id[SG_SHA1_RAW_LEN];
        unsigned char old_content[40];
        unsigned char new_content[40];

        rand_bytes(old_content, sizeof(old_content));
        do {
            rand_bytes(new_content, sizeof(new_content));
        } while (memcmp(old_content, new_content, sizeof(old_content)) == 0);
        blob_write(git_dir, old_content, sizeof(old_content), id);
        index_upsert(idx, path, 0, 0100644, id);
        write_workdir_bytes(repo_root, path, new_content, sizeof(new_content));
        exp->status_reported = 1;
        exp->diff_row_count = 1;
        break;
    }
    case SC_CHUNKED_UNMODIFIED: {
        size_t len = SG_CHUNK_MIN_SIZE * 2;
        unsigned char *data = malloc(len);
        unsigned char pointer_id[SG_SHA1_RAW_LEN];
        int chunked = -1;

        if (data == NULL) {
            fprintf(stderr, "setup failed: OOM building chunk fixture\n");
            exit(1);
        }
        rand_bytes(data, len);
        if (sg_chunk_store_blob(git_dir, data, len, 1024, pointer_id, &chunked) != 0 ||
           chunked != 1) {
            fprintf(stderr, "setup failed: sg_chunk_store_blob did not chunk as expected\n");
            exit(1);
        }
        index_upsert(idx, path, 0, 0100644, pointer_id);
        write_workdir_bytes(repo_root, path, data, len);
        free(data);
        exp->status_reported = 0;
        exp->diff_row_count = 0;
        break;
    }
    case SC_CHUNKED_MODIFIED: {
        size_t len = SG_CHUNK_MIN_SIZE * 2;
        unsigned char *data = malloc(len);
        unsigned char pointer_id[SG_SHA1_RAW_LEN];
        int chunked = -1;

        if (data == NULL) {
            fprintf(stderr, "setup failed: OOM building chunk fixture\n");
            exit(1);
        }
        rand_bytes(data, len);
        if (sg_chunk_store_blob(git_dir, data, len, 1024, pointer_id, &chunked) != 0 ||
           chunked != 1) {
            fprintf(stderr, "setup failed: sg_chunk_store_blob did not chunk as expected\n");
            exit(1);
        }
        index_upsert(idx, path, 0, 0100644, pointer_id);
        data[0] ^= 0xFF; /* modify after storing, so workdir != stored content */
        write_workdir_bytes(repo_root, path, data, len);
        free(data);
        exp->status_reported = 1;
        exp->diff_row_count = 1;
        break;
    }
    case SC_UNTRACKED: {
        const unsigned char content[] = "untracked, not in the index at all\n";

        write_workdir_bytes(repo_root, path, content, sizeof(content) - 1);
        exp->status_reported = 0;
        exp->diff_row_count = 0;
        break;
    }
    case SC_UNMERGED_NO_STAGE2: {
        unsigned char base_id[SG_SHA1_RAW_LEN], theirs_id[SG_SHA1_RAW_LEN];
        const unsigned char base_content[] = "base\n";
        const unsigned char theirs_content[] = "theirs\n";
        const unsigned char wd_content[] = "mid-conflict-edit\n";

        blob_write(git_dir, base_content, sizeof(base_content) - 1, base_id);
        blob_write(git_dir, theirs_content, sizeof(theirs_content) - 1, theirs_id);
        index_upsert(idx, path, 1, 0100644, base_id);
        index_upsert(idx, path, 3, 0100644, theirs_id);
        write_workdir_bytes(repo_root, path, wd_content, sizeof(wd_content) - 1);
        exp->status_reported = 0;
        exp->diff_row_count = 1;
        exp->diff_row_unmerged[0] = 1;
        break;
    }
    case SC_UNMERGED_STAGE2_MATCH: {
        unsigned char base_id[SG_SHA1_RAW_LEN], ours_id[SG_SHA1_RAW_LEN], theirs_id[SG_SHA1_RAW_LEN];
        const unsigned char base_content[] = "base\n";
        const unsigned char ours_content[] = "ours, matches disk exactly\n";
        const unsigned char theirs_content[] = "theirs\n";

        blob_write(git_dir, base_content, sizeof(base_content) - 1, base_id);
        blob_write(git_dir, ours_content, sizeof(ours_content) - 1, ours_id);
        blob_write(git_dir, theirs_content, sizeof(theirs_content) - 1, theirs_id);
        index_upsert(idx, path, 1, 0100644, base_id);
        index_upsert(idx, path, 2, 0100644, ours_id);
        index_upsert(idx, path, 3, 0100644, theirs_id);
        write_workdir_bytes(repo_root, path, ours_content, sizeof(ours_content) - 1);
        exp->status_reported = 0;
        exp->diff_row_count = 1;
        exp->diff_row_unmerged[0] = 1;
        break;
    }
    case SC_UNMERGED_STAGE2_DIFFER: {
        unsigned char base_id[SG_SHA1_RAW_LEN], ours_id[SG_SHA1_RAW_LEN], theirs_id[SG_SHA1_RAW_LEN];
        const unsigned char base_content[] = "base\n";
        const unsigned char ours_content[] = "ours\n";
        const unsigned char theirs_content[] = "theirs\n";
        const unsigned char wd_content[] = "neither -- still being edited\n";

        blob_write(git_dir, base_content, sizeof(base_content) - 1, base_id);
        blob_write(git_dir, ours_content, sizeof(ours_content) - 1, ours_id);
        blob_write(git_dir, theirs_content, sizeof(theirs_content) - 1, theirs_id);
        index_upsert(idx, path, 1, 0100644, base_id);
        index_upsert(idx, path, 2, 0100644, ours_id);
        index_upsert(idx, path, 3, 0100644, theirs_id);
        write_workdir_bytes(repo_root, path, wd_content, sizeof(wd_content) - 1);
        exp->status_reported = 0;
        exp->diff_row_count = 2;
        exp->diff_row_unmerged[0] = 1;
        exp->diff_row_unmerged[1] = 0;
        break;
    }
    default:
        fprintf(stderr, "setup failed: unknown scenario %d\n", (int)sc);
        exit(1);
    }
}

static const char *scenario_name(scenario sc)
{
    switch (sc) {
    case SC_CLEAN:
        return "CLEAN";
    case SC_MODIFIED:
        return "MODIFIED";
    case SC_DELETED:
        return "DELETED";
    case SC_MODE_ONLY:
        return "MODE_ONLY";
    case SC_EMPTY_UNCHANGED:
        return "EMPTY_UNCHANGED";
    case SC_BINARY_MODIFIED:
        return "BINARY_MODIFIED";
    case SC_CHUNKED_UNMODIFIED:
        return "CHUNKED_UNMODIFIED";
    case SC_CHUNKED_MODIFIED:
        return "CHUNKED_MODIFIED";
    case SC_UNTRACKED:
        return "UNTRACKED";
    case SC_UNMERGED_NO_STAGE2:
        return "UNMERGED_NO_STAGE2";
    case SC_UNMERGED_STAGE2_MATCH:
        return "UNMERGED_STAGE2_MATCH";
    case SC_UNMERGED_STAGE2_DIFFER:
        return "UNMERGED_STAGE2_DIFFER";
    default:
        return "?";
    }
}

#define MAX_PATHS 16

static void fuzz_parity_round(uint64_t seed)
{
    char *git_dir;
    char *repo_root;
    sg_index idx;
    unsigned int n_paths;
    unsigned int i;
    char paths[MAX_PATHS][32];
    scenario scenarios[MAX_PATHS];
    expectation expectations[MAX_PATHS];
    sg_status_list slist;
    sg_diff_list dlist;

    seed_prng(seed);
    n_paths = 3 + rand_below(MAX_PATHS - 3);

    git_dir = make_tmp_repo("fuzz");
    repo_root = sg_repo_root(git_dir);
    memset(&idx, 0, sizeof(idx));

    for (i = 0; i < n_paths; i++) {
        snprintf(paths[i], sizeof(paths[i]), "p%02u_%s.txt", i,
                (rand_below(2) == 0) ? "a" : "b"); /* mixes directory depth a little */
        scenarios[i] = (scenario)rand_below(SC_COUNT);
        setup_one(git_dir, repo_root, &idx, paths[i], scenarios[i], &expectations[i]);
    }

    if (sg_status_diff_unstaged(git_dir, repo_root, &idx, &slist) != 0) {
        CHECK(0,
             "seed %llu: sg_status_diff_unstaged failed unexpectedly (no scenario in this round's "
             "pool should cause a hard failure)",
             (unsigned long long)seed);
        /* status.c does not clean up whatever it accumulated before failing
           (unlike sg_status_list_untracked's documented cleanup-on-failure
           contract) -- free it defensively so an unexpected failure here
           doesn't also leak under ASan. */
        sg_status_list_free(&slist);
        goto done_no_status;
    }

    for (i = 0; i < n_paths; i++) {
        size_t got = status_count(&slist, paths[i]);
        size_t want = expectations[i].status_reported ? 1 : 0;

        CHECK(got == want,
             "seed %llu path %s scenario %s: sg_status_diff_unstaged reported %zu entries, "
             "expected %zu",
             (unsigned long long)seed, paths[i], scenario_name(scenarios[i]), got, want);
    }
    sg_status_list_free(&slist);

done_no_status:
    if (sg_diff_index_workdir(git_dir, repo_root, &idx, &dlist) != 0) {
        CHECK(0, "seed %llu: sg_diff_index_workdir failed unexpectedly", (unsigned long long)seed);
    } else {
        for (i = 0; i < n_paths; i++) {
            size_t got = diff_count(&dlist, paths[i]);
            size_t want = (size_t)expectations[i].diff_row_count;
            size_t got_unmerged = diff_count_unmerged(&dlist, paths[i]);
            size_t want_unmerged = (size_t)(expectations[i].diff_row_unmerged[0] +
                                            expectations[i].diff_row_unmerged[1]);

            CHECK(got == want,
                 "seed %llu path %s scenario %s: sg_diff_index_workdir reported %zu rows, "
                 "expected %zu",
                 (unsigned long long)seed, paths[i], scenario_name(scenarios[i]), got, want);
            CHECK(got_unmerged == want_unmerged,
                 "seed %llu path %s scenario %s: sg_diff_index_workdir reported %zu unmerged "
                 "rows, expected %zu",
                 (unsigned long long)seed, paths[i], scenario_name(scenarios[i]), got_unmerged,
                 want_unmerged);
        }
        sg_diff_list_free(&dlist);
    }

    sg_index_free(&idx);
    rm_rf(repo_root);
    free(repo_root);
    free(git_dir);
}

int main(void)
{
    long iters = env_long("SG_FUZZ_ITERS", 200);
    long seed_base = env_long("SG_FUZZ_SEED_BASE", 0);
    long i;

    test_mode_only_now_reported();
    test_unmerged_no_stage2();
    test_unmerged_stage2_differs();
    test_unmerged_stage2_matches_workdir();
    test_chunked_broken_pointer_now_resilient();
    test_unreadable_path_agrees();
    test_chunked_unmodified_agrees();
    test_untracked_file_not_reported();

    for (i = 0; i < iters; i++)
        fuzz_parity_round((uint64_t)(seed_base + i));

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all status/diff parity checks passed (%ld fuzz rounds, seed_base %ld)\n", iters,
          seed_base);
    return 0;
}
