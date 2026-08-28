/* WHAT THIS FILE IS NOW, AND WHAT IT WAS.

   It was written in Phase 32 BEFORE the convergence, as a differential
   harness between two genuinely independent "what changed between HEAD and
   the index" implementations: sg_status_diff_staged, then a hand-rolled
   merge-join over a flattened tree and the index, and sg_diff_tree_index,
   the general builder behind `sg diff --cached`. That is Phase 27's process
   applied to the other half of `sg status`, and its job was to enumerate
   divergences rather than fix any -- a truthful list to hand to whoever
   decided what to converge. It found none, across 12 named shapes and 5000
   fuzz rounds, and four mutations against the product code confirmed it
   would have seen one.

   THE CONVERGENCE HAS SINCE HAPPENED. sg_status_diff_staged is now a thin
   adapter over sg_diff_tree_index, so the two sides below are no longer
   independent: this compares the adapter against the function the adapter
   itself calls. That is not a tautology -- every expectation here is
   hand-derived and hard-coded, never taken from either side, so the checks
   still fail if the ADAPTER's normalisation is wrong (mapping ABSENT sides
   to the wrong kind, or failing to drop unmerged rows). It is a regression
   test for that normalisation, and it is no longer evidence that two
   implementations agree, because there are no longer two.

   It also does not cover renames: every call below passes rename_score 0 on
   purpose, so that what it compares stays the plain classification. Rename
   pairing is covered end-to-end against real git in tests/interop.sh.

   Normalizing sg_diff_list -> sg_status_kind (mirrors test_status_diff_
   parity.c's mapping, adapted for tree-vs-index instead of index-vs-
   workdir):
     old_side ABSENT, new_side present -> SG_STATUS_NEW
     old_side present, new_side ABSENT -> SG_STATUS_DELETED
     old_side present, new_side present -> SG_STATUS_MODIFIED
     entries[i].unmerged true          -> SKIPPED. sg_status_diff_staged
                                           never produces a row for an
                                           unresolved conflict (no stage-0
                                           entry to compare); the task's own
                                           instructions call for skipping
                                           these during normalization rather
                                           than inventing a status_kind that
                                           does not exist.
   Unlike the unstaged pair, sg_diff_tree_index never emits a second,
   "stage-2 vs other side" companion row for the same path -- its own header
   comment says a path with any non-zero stage "has no single staged blob to
   diff against and yields exactly one row, with `unmerged` set" -- so there
   is no stage-2-companion-row filtering to reproduce here, unlike test_
   status_diff_parity.c's adapter-specific logic.

   Style follows test_status_diff_parity.c: deterministic xorshift64 PRNG,
   SG_FUZZ_ITERS/SG_FUZZ_SEED_BASE env vars, every failure message names the
   seed. Named deterministic scenarios pin down the shapes called out in the
   task (mode-only, three unmerged shapes, a corrupt duplicate-path index,
   unborn HEAD, and the '.' vs '/' byte-ordering boundary); the randomized
   fuzz loop combines many independent per-path scenarios in one repo,
   looking for interactions neither of us anticipated. */
#include "sg/diff.h"
#include "sg/hash.h"
#include "sg/index.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/repo.h"
#include "sg/status.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

/* Set only while self_check_harness_can_detect_divergence() runs. Its whole
   point is to deliberately manufacture failures, so CHECK must not print
   the literal string "FAIL" for them -- tests/gates.sh's `make test` gate
   greps raw logs for "FAIL" lines and would otherwise report this file as
   broken even though it exits 0 on purpose (the self-check isolates its
   failure count into its own return value, see main()). */
static int g_self_check_mode = 0;

#define CHECK(cond, ...)                                                                        \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            fprintf(stderr, "%s %s:%d: ", g_self_check_mode ? "EXPECTED-SELFCHECK-DIVERGENCE" : "FAIL", \
                   __func__, __LINE__);                                                          \
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

static void rand_bytes(unsigned char *buf, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
        buf[i] = rand_byte();
}

static long env_long(const char *name, long def)
{
    const char *v = getenv(name);

    if (v == NULL || *v == '\0')
        return def;
    return strtol(v, NULL, 10);
}

/* ---- repo/fs helpers ---- */

static char *make_tmp_repo(const char *tag)
{
    char template[128];
    char *path;
    char git_dir[SG_PATH_MAX];

    snprintf(template, sizeof(template), "/tmp/sg_status_staged_parity_%s_XXXXXX", tag);
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

/* ---- HEAD-tree builder: a plain array of (path, mode, id) collected during
   fixture setup, sorted by path, and turned into a real tree object via
   sg_tree_build -- exactly what a commit's tree would look like, without
   needing to actually create a commit. Both implementations under test are
   fed from the SAME tree id / flattened list, so any divergence found is
   about the comparison logic, never about the two sides seeing different
   data. ---- */

#define MAX_TREE_ENTRIES 32

typedef struct {
    sg_flat_entry entries[MAX_TREE_ENTRIES];
    size_t count;
} head_builder;

static void head_builder_init(head_builder *hb)
{
    hb->count = 0;
}

static void head_builder_add(head_builder *hb, const char *path, unsigned int mode,
                             const unsigned char id[SG_SHA1_RAW_LEN])
{
    if (hb->count >= MAX_TREE_ENTRIES) {
        fprintf(stderr, "setup failed: head_builder overflow\n");
        exit(1);
    }
    hb->entries[hb->count].path = strdup(path);
    if (hb->entries[hb->count].path == NULL) {
        fprintf(stderr, "setup failed: strdup\n");
        exit(1);
    }
    hb->entries[hb->count].mode = mode;
    memcpy(hb->entries[hb->count].sha1, id, SG_SHA1_RAW_LEN);
    hb->count++;
}

static int flat_entry_cmp(const void *a, const void *b)
{
    return strcmp(((const sg_flat_entry *)a)->path, ((const sg_flat_entry *)b)->path);
}

/* Builds a real tree object from hb (or leaves *has_tree = 0, meaning
   "unborn HEAD / empty tree", if hb is empty), and independently obtains the
   flattened list sg_status_diff_staged wants by reading the tree back --
   the same round-trip cmd_status.c itself does (sg_tree_flatten after
   resolving commit.tree), not a shortcut that reuses hb's own entries. */
static void head_builder_finish(head_builder *hb, const char *git_dir,
                                unsigned char tree_id_out[SG_SHA1_RAW_LEN], int *has_tree,
                                sg_flat_list *head_flat)
{
    size_t i;

    if (hb->count == 0) {
        *has_tree = 0;
        head_flat->entries = NULL;
        head_flat->count = 0;
    } else {
        qsort(hb->entries, hb->count, sizeof(hb->entries[0]), flat_entry_cmp);
        if (sg_tree_build(git_dir, hb->entries, hb->count, tree_id_out) != 0) {
            fprintf(stderr, "setup failed: sg_tree_build\n");
            exit(1);
        }
        *has_tree = 1;
        if (sg_tree_flatten(git_dir, tree_id_out, head_flat, NULL) != 0) {
            fprintf(stderr, "setup failed: sg_tree_flatten\n");
            exit(1);
        }
    }
    for (i = 0; i < hb->count; i++)
        free(hb->entries[i].path);
    hb->count = 0;
}

/* ---- Result lookup helpers -- both output lists are unordered w.r.t. each
   other's classification, so tests search by path. ---- */

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

/* Normalizes sg_diff_list -> the sg_status_kind space, per the mapping
   documented at the top of this file. Rows with `unmerged` set are skipped
   entirely (no sg_status_kind equivalent), per the task's instructions. */
static sg_status_kind normalize_diff_kind(const sg_diff_entry *e)
{
    if (e->old_side.kind == SG_DIFF_SIDE_ABSENT)
        return SG_STATUS_NEW;
    if (e->new_side.kind == SG_DIFF_SIDE_ABSENT)
        return SG_STATUS_DELETED;
    return SG_STATUS_MODIFIED;
}

/* Self-verification knob (see main()'s self-check): when non-zero,
   normalize_diff_kind's NEW/DELETED answers are swapped, to prove the
   harness can actually go red when a real divergence is introduced. Left at
   0 for every real assertion in this file. */
static int g_mutate_normalize = 0;

static sg_status_kind normalize_diff_kind_checked(const sg_diff_entry *e)
{
    sg_status_kind k = normalize_diff_kind(e);

    if (g_mutate_normalize) {
        if (k == SG_STATUS_NEW)
            return SG_STATUS_DELETED;
        if (k == SG_STATUS_DELETED)
            return SG_STATUS_NEW;
    }
    return k;
}

static size_t diff_count_normalized(const sg_diff_list *list, const char *path)
{
    size_t i, n = 0;

    for (i = 0; i < list->count; i++) {
        if (strcmp(list->entries[i].path, path) == 0 && !list->entries[i].unmerged)
            n++;
    }
    return n;
}

static int diff_find_normalized(const sg_diff_list *list, const char *path, sg_status_kind *kind_out)
{
    size_t i;

    for (i = 0; i < list->count; i++) {
        if (strcmp(list->entries[i].path, path) == 0 && !list->entries[i].unmerged) {
            *kind_out = normalize_diff_kind_checked(&list->entries[i]);
            return 1;
        }
    }
    return 0;
}

/* Runs both implementations against (hb, idx) and checks each reported path
   against a hand-derived expectation. exp_reported[i]/exp_kind[i] must be
   computed by the caller from the fixture's own construction, never from
   either implementation's output -- see the file header and CLAUDE.md's
   "differential harness must not borrow its expectation" rule. Divergences
   between the two implementations show up as ONE of the two CHECKs failing
   while the other passes for the same path. */
static void run_and_check(const char *git_dir, head_builder *hb, sg_index *idx,
                          const char *const *paths, const int *exp_reported,
                          const sg_status_kind *exp_kind, size_t n_paths, const char *label)
{
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    int has_tree;
    sg_flat_list head_flat;
    sg_status_list slist;
    sg_diff_list dlist;
    size_t i;

    head_builder_finish(hb, git_dir, tree_id, &has_tree, &head_flat);

    CHECK(sg_status_diff_staged(git_dir, git_dir, has_tree ? tree_id : NULL, idx, 0, &slist, NULL) == 0,
         "%s: sg_status_diff_staged failed", label);
    CHECK(sg_diff_tree_index(git_dir, has_tree ? tree_id : NULL, idx, &dlist, NULL) == 0,
         "%s: sg_diff_tree_index failed", label);

    for (i = 0; i < n_paths; i++) {
        size_t sc = status_count(&slist, paths[i]);
        size_t wantc = exp_reported[i] ? 1 : 0;

        CHECK(sc == wantc,
             "%s path %s: sg_status_diff_staged reported %zu entries, expected %zu", label,
             paths[i], sc, wantc);
        if (sc == 1 && wantc == 1) {
            const sg_status_entry *e = status_find(&slist, paths[i]);

            CHECK(e != NULL, "%s path %s: status_find NULL despite status_count == 1", label,
                 paths[i]);
            if (e != NULL)
                CHECK(e->kind == exp_kind[i],
                     "%s path %s: sg_status_diff_staged reported kind %d, expected %d", label,
                     paths[i], (int)e->kind, (int)exp_kind[i]);
        }

        {
            sg_status_kind got_kind;
            size_t dc = diff_count_normalized(&dlist, paths[i]);
            int got = diff_find_normalized(&dlist, paths[i], &got_kind);

            CHECK(dc == wantc,
                 "%s path %s: sg_diff_tree_index (normalized) reported %zu entries, expected %zu",
                 label, paths[i], dc, wantc);
            if (exp_reported[i]) {
                CHECK(got == 1, "%s path %s: sg_diff_tree_index (normalized) missing row", label,
                     paths[i]);
                if (got)
                    CHECK(got_kind == exp_kind[i],
                         "%s path %s: sg_diff_tree_index (normalized) reported kind %d, expected "
                         "%d",
                         label, paths[i], (int)got_kind, (int)exp_kind[i]);
            }
        }
    }

    sg_status_list_free(&slist);
    sg_diff_list_free(&dlist);
    sg_flat_list_free(&head_flat);
}

/* ==================================================================== *
 * Named deterministic scenarios.
 * ==================================================================== */

static void test_clean_agrees(void)
{
    char *git_dir = make_tmp_repo("clean");
    char *repo_root = sg_repo_root(git_dir);
    head_builder hb;
    sg_index idx;
    unsigned char id[SG_SHA1_RAW_LEN];
    const unsigned char content[] = "clean, unmodified content\n";
    const char *paths[] = {"clean.txt"};
    int exp_reported[] = {0};
    sg_status_kind exp_kind[] = {SG_STATUS_NEW};

    head_builder_init(&hb);
    memset(&idx, 0, sizeof(idx));
    blob_write(git_dir, content, sizeof(content) - 1, id);
    head_builder_add(&hb, "clean.txt", 0100644, id);
    index_upsert(&idx, "clean.txt", 0, 0100644, id);

    run_and_check(git_dir, &hb, &idx, paths, exp_reported, exp_kind, 1, "clean");

    sg_index_free(&idx);
    rm_rf(repo_root);
    free(repo_root);
    free(git_dir);
}

static void test_modified_agrees(void)
{
    char *git_dir = make_tmp_repo("modified");
    char *repo_root = sg_repo_root(git_dir);
    head_builder hb;
    sg_index idx;
    unsigned char old_id[SG_SHA1_RAW_LEN], new_id[SG_SHA1_RAW_LEN];
    const unsigned char old_content[] = "before\n";
    const unsigned char new_content[] = "after, definitely different\n";
    const char *paths[] = {"m.txt"};
    int exp_reported[] = {1};
    sg_status_kind exp_kind[] = {SG_STATUS_MODIFIED};

    head_builder_init(&hb);
    memset(&idx, 0, sizeof(idx));
    blob_write(git_dir, old_content, sizeof(old_content) - 1, old_id);
    blob_write(git_dir, new_content, sizeof(new_content) - 1, new_id);
    head_builder_add(&hb, "m.txt", 0100644, old_id);
    index_upsert(&idx, "m.txt", 0, 0100644, new_id);

    run_and_check(git_dir, &hb, &idx, paths, exp_reported, exp_kind, 1, "modified");

    sg_index_free(&idx);
    rm_rf(repo_root);
    free(repo_root);
    free(git_dir);
}

/* Mode-only change: sha1 identical, mode 100644 -> 100755. sg_diff_tree_index's
   blob_sides_differ compares id OR mode, so this is a change; the hand-rolled
   walk this file was written against compared the same two things, which is
   why the pre-convergence run agreed here. Expected: MODIFIED from both. */
static void test_mode_only(void)
{
    char *git_dir = make_tmp_repo("mode_only");
    char *repo_root = sg_repo_root(git_dir);
    head_builder hb;
    sg_index idx;
    unsigned char id[SG_SHA1_RAW_LEN];
    const unsigned char content[] = "same bytes, different mode\n";
    const char *paths[] = {"exec.txt"};
    int exp_reported[] = {1};
    sg_status_kind exp_kind[] = {SG_STATUS_MODIFIED};

    head_builder_init(&hb);
    memset(&idx, 0, sizeof(idx));
    blob_write(git_dir, content, sizeof(content) - 1, id);
    head_builder_add(&hb, "exec.txt", 0100644, id);
    index_upsert(&idx, "exec.txt", 0, 0100755, id);

    run_and_check(git_dir, &hb, &idx, paths, exp_reported, exp_kind, 1, "mode_only");

    sg_index_free(&idx);
    rm_rf(repo_root);
    free(repo_root);
    free(git_dir);
}

static void test_deleted_agrees(void)
{
    char *git_dir = make_tmp_repo("deleted");
    char *repo_root = sg_repo_root(git_dir);
    head_builder hb;
    sg_index idx;
    unsigned char id[SG_SHA1_RAW_LEN];
    const unsigned char content[] = "will be deleted from the index\n";
    const char *paths[] = {"d.txt"};
    int exp_reported[] = {1};
    sg_status_kind exp_kind[] = {SG_STATUS_DELETED};

    head_builder_init(&hb);
    memset(&idx, 0, sizeof(idx));
    blob_write(git_dir, content, sizeof(content) - 1, id);
    head_builder_add(&hb, "d.txt", 0100644, id);
    /* no index entry at all */

    run_and_check(git_dir, &hb, &idx, paths, exp_reported, exp_kind, 1, "deleted");

    sg_index_free(&idx);
    rm_rf(repo_root);
    free(repo_root);
    free(git_dir);
}

static void test_new_agrees(void)
{
    char *git_dir = make_tmp_repo("new");
    char *repo_root = sg_repo_root(git_dir);
    head_builder hb;
    sg_index idx;
    unsigned char id[SG_SHA1_RAW_LEN];
    const unsigned char content[] = "freshly staged, not in HEAD\n";
    const char *paths[] = {"n.txt"};
    int exp_reported[] = {1};
    sg_status_kind exp_kind[] = {SG_STATUS_NEW};

    head_builder_init(&hb);
    memset(&idx, 0, sizeof(idx));
    blob_write(git_dir, content, sizeof(content) - 1, id);
    index_upsert(&idx, "n.txt", 0, 0100644, id);
    /* no head entry at all */

    run_and_check(git_dir, &hb, &idx, paths, exp_reported, exp_kind, 1, "new");

    sg_index_free(&idx);
    rm_rf(repo_root);
    free(repo_root);
    free(git_dir);
}

/* Unmerged shape #1: tree HAS the path, index carries only stage 1 and 3
   (no stage 0). This must NOT be reported as a deletion. The hand-rolled
   walk needed a helper of its own for that (path_has_unmerged_stage, gone
   with the walk); sg_diff_tree_index's cmp==0 branch sees idx_unmerged and
   emits the single fixed unmerged row instead, which the normalisation
   drops. This case is the reason that helper could be deleted safely.
   Expected: not reported by either. */
static void test_unmerged_no_stage2_tree_has_path(void)
{
    char *git_dir = make_tmp_repo("unmerged_ns2_thp");
    char *repo_root = sg_repo_root(git_dir);
    head_builder hb;
    sg_index idx;
    unsigned char head_id[SG_SHA1_RAW_LEN], base_id[SG_SHA1_RAW_LEN], theirs_id[SG_SHA1_RAW_LEN];
    const unsigned char head_content[] = "what HEAD has\n";
    const unsigned char base_content[] = "base\n";
    const unsigned char theirs_content[] = "theirs\n";
    const char *paths[] = {"conflict.txt"};
    int exp_reported[] = {0};
    sg_status_kind exp_kind[] = {SG_STATUS_NEW};

    head_builder_init(&hb);
    memset(&idx, 0, sizeof(idx));
    blob_write(git_dir, head_content, sizeof(head_content) - 1, head_id);
    blob_write(git_dir, base_content, sizeof(base_content) - 1, base_id);
    blob_write(git_dir, theirs_content, sizeof(theirs_content) - 1, theirs_id);
    head_builder_add(&hb, "conflict.txt", 0100644, head_id);
    index_upsert(&idx, "conflict.txt", 1, 0100644, base_id);
    index_upsert(&idx, "conflict.txt", 3, 0100644, theirs_id);

    run_and_check(git_dir, &hb, &idx, paths, exp_reported, exp_kind, 1, "unmerged_ns2_tree_has_path");

    sg_index_free(&idx);
    rm_rf(repo_root);
    free(repo_root);
    free(git_dir);
}

/* Unmerged shape #2: full stage 1/2/3, tree HAS the path. */
static void test_unmerged_all_stages_tree_has_path(void)
{
    char *git_dir = make_tmp_repo("unmerged_all_thp");
    char *repo_root = sg_repo_root(git_dir);
    head_builder hb;
    sg_index idx;
    unsigned char head_id[SG_SHA1_RAW_LEN], base_id[SG_SHA1_RAW_LEN], ours_id[SG_SHA1_RAW_LEN],
        theirs_id[SG_SHA1_RAW_LEN];
    const unsigned char head_content[] = "what HEAD has\n";
    const unsigned char base_content[] = "base\n";
    const unsigned char ours_content[] = "ours\n";
    const unsigned char theirs_content[] = "theirs\n";
    const char *paths[] = {"conflict.txt"};
    int exp_reported[] = {0};
    sg_status_kind exp_kind[] = {SG_STATUS_NEW};

    head_builder_init(&hb);
    memset(&idx, 0, sizeof(idx));
    blob_write(git_dir, head_content, sizeof(head_content) - 1, head_id);
    blob_write(git_dir, base_content, sizeof(base_content) - 1, base_id);
    blob_write(git_dir, ours_content, sizeof(ours_content) - 1, ours_id);
    blob_write(git_dir, theirs_content, sizeof(theirs_content) - 1, theirs_id);
    head_builder_add(&hb, "conflict.txt", 0100644, head_id);
    index_upsert(&idx, "conflict.txt", 1, 0100644, base_id);
    index_upsert(&idx, "conflict.txt", 2, 0100644, ours_id);
    index_upsert(&idx, "conflict.txt", 3, 0100644, theirs_id);

    run_and_check(git_dir, &hb, &idx, paths, exp_reported, exp_kind, 1, "unmerged_all_tree_has_path");

    sg_index_free(&idx);
    rm_rf(repo_root);
    free(repo_root);
    free(git_dir);
}

/* Unmerged shape #3: full stage 1/2/3, tree does NOT have the path (e.g. the
   conflict is "added by both", HEAD never had it). Expected: not reported
   by either -- sg_status_diff_staged's ii-filtering loop skips every
   nonzero-stage idx entry before the merge-join even sees it, so this path
   never gets compared against hi at all (it is simply absent from the
   comparison, not a false NEW); sg_diff_tree_index's cmp>0 branch sees
   idx_unmerged and emits the single unmerged row (normalized away). */
static void test_unmerged_all_stages_tree_no_path(void)
{
    char *git_dir = make_tmp_repo("unmerged_all_tnp");
    char *repo_root = sg_repo_root(git_dir);
    head_builder hb;
    sg_index idx;
    unsigned char base_id[SG_SHA1_RAW_LEN], ours_id[SG_SHA1_RAW_LEN], theirs_id[SG_SHA1_RAW_LEN];
    const unsigned char base_content[] = "base\n";
    const unsigned char ours_content[] = "ours\n";
    const unsigned char theirs_content[] = "theirs\n";
    const char *paths[] = {"conflict.txt"};
    int exp_reported[] = {0};
    sg_status_kind exp_kind[] = {SG_STATUS_NEW};

    head_builder_init(&hb);
    memset(&idx, 0, sizeof(idx));
    blob_write(git_dir, base_content, sizeof(base_content) - 1, base_id);
    blob_write(git_dir, ours_content, sizeof(ours_content) - 1, ours_id);
    blob_write(git_dir, theirs_content, sizeof(theirs_content) - 1, theirs_id);
    index_upsert(&idx, "conflict.txt", 1, 0100644, base_id);
    index_upsert(&idx, "conflict.txt", 2, 0100644, ours_id);
    index_upsert(&idx, "conflict.txt", 3, 0100644, theirs_id);
    /* no head entry at all */

    run_and_check(git_dir, &hb, &idx, paths, exp_reported, exp_kind, 1, "unmerged_all_tree_no_path");

    sg_index_free(&idx);
    rm_rf(repo_root);
    free(repo_root);
    free(git_dir);
}

/* Unmerged shape #4: only a stage-2 entry (no stage 1, no stage 3) -- e.g.
   "we added it, they deleted it" style conflicts. path_has_unmerged_stage
   checks stage 1 OR 2 OR 3, so stage 2 alone must still suppress the
   deletion/new answer; sg_diff_tree_index's group-first-entry check
   (idx->entries[ii].stage != 0) is likewise satisfied by a lone stage-2
   entry. Tree has the path here. */
static void test_unmerged_stage2_only_tree_has_path(void)
{
    char *git_dir = make_tmp_repo("unmerged_s2only");
    char *repo_root = sg_repo_root(git_dir);
    head_builder hb;
    sg_index idx;
    unsigned char head_id[SG_SHA1_RAW_LEN], ours_id[SG_SHA1_RAW_LEN];
    const unsigned char head_content[] = "what HEAD has\n";
    const unsigned char ours_content[] = "ours, added independently\n";
    const char *paths[] = {"conflict.txt"};
    int exp_reported[] = {0};
    sg_status_kind exp_kind[] = {SG_STATUS_NEW};

    head_builder_init(&hb);
    memset(&idx, 0, sizeof(idx));
    blob_write(git_dir, head_content, sizeof(head_content) - 1, head_id);
    blob_write(git_dir, ours_content, sizeof(ours_content) - 1, ours_id);
    head_builder_add(&hb, "conflict.txt", 0100644, head_id);
    index_upsert(&idx, "conflict.txt", 2, 0100644, ours_id);

    run_and_check(git_dir, &hb, &idx, paths, exp_reported, exp_kind, 1, "unmerged_stage2_only");

    sg_index_free(&idx);
    rm_rf(repo_root);
    free(repo_root);
    free(git_dir);
}

/* A corrupt index: the same path appears in TWO non-contiguous groups (a
   stage-0 entry, and, appended after some other lexically-greater path, a
   second stage-0 entry for the same name) -- built by bypassing
   sg_index_upsert exactly like tests/test_status_diff_parity.c's
   test_corrupt_index_duplicate_path_is_not_dropped, adapted for the tree
   side. sg_index_read does not validate ordering or dedupe (src/index/
   index.c), so this is a real on-disk shape, not a fixture-only fiction.

   sg_status_diff_staged's loop advances ii one entry at a time and hi one
   entry at a time, comparing via strcmp -- it does NOT assume idx is
   strictly increasing beyond "the very next ii". sg_diff_tree_index's
   index_group_end, by contrast, groups by scanning forward while the path
   stays EQUAL, then the outer loop's cmp advances oi/ii independently --
   it also does not require global strict ordering beyond the immediate
   comparison. Given how different the two loops' shapes are, this is
   exactly the kind of place they could parse the same corrupt index
   differently -- included as a named scenario specifically to force the
   question rather than leave it to chance in the fuzzer. */
static void test_corrupt_index_duplicate_path(void)
{
    char *git_dir = make_tmp_repo("dup_path");
    char *repo_root = sg_repo_root(git_dir);
    head_builder hb;
    sg_index idx;
    unsigned char head_id[SG_SHA1_RAW_LEN], staged_id[SG_SHA1_RAW_LEN], other_id[SG_SHA1_RAW_LEN];
    const unsigned char head_content[] = "head a\n";
    const unsigned char staged_content[] = "staged a, CHANGED\n";
    const unsigned char other_content[] = "unrelated, unchanged\n";
    sg_status_list slist;
    sg_diff_list dlist;
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    int has_tree;
    sg_flat_list head_flat;

    head_builder_init(&hb);
    memset(&idx, 0, sizeof(idx));
    blob_write(git_dir, head_content, sizeof(head_content) - 1, head_id);
    blob_write(git_dir, staged_content, sizeof(staged_content) - 1, staged_id);
    blob_write(git_dir, other_content, sizeof(other_content) - 1, other_id);

    head_builder_add(&hb, "a.txt", 0100644, head_id);
    head_builder_add(&hb, "z_other.txt", 0100644, other_id);

    index_upsert(&idx, "a.txt", 0, 0100644, staged_id);
    index_upsert(&idx, "z_other.txt", 0, 0100644, other_id); /* unchanged, contributes no row */

    /* Append a second a.txt AFTER z_other.txt, bypassing sg_index_upsert, so
       the array is no longer sorted -- the exact corrupt-on-disk shape
       sg_index_read would hand us verbatim. */
    {
        sg_index_entry *grown = realloc(idx.entries, (idx.count + 1) * sizeof(*grown));

        if (grown == NULL) {
            fprintf(stderr, "setup failed: realloc\n");
            exit(1);
        }
        idx.entries = grown;
        idx.entries[idx.count] = idx.entries[0]; /* copies the a.txt stage-0 entry */
        idx.entries[idx.count].path = strdup("a.txt");
        if (idx.entries[idx.count].path == NULL) {
            fprintf(stderr, "setup failed: strdup\n");
            exit(1);
        }
        idx.count++;
    }

    head_builder_finish(&hb, git_dir, tree_id, &has_tree, &head_flat);

    CHECK(sg_status_diff_staged(git_dir, git_dir, has_tree ? tree_id : NULL, &idx, 0, &slist, NULL) == 0,
         "corrupt_dup: sg_status_diff_staged failed");
    CHECK(sg_diff_tree_index(git_dir, has_tree ? tree_id : NULL, &idx, &dlist, NULL) == 0,
         "corrupt_dup: sg_diff_tree_index failed");

    fprintf(stderr,
           "INFO corrupt_index_duplicate_path: sg_status_diff_staged reports a.txt %zu time(s); "
           "sg_diff_tree_index (raw, including unmerged rows) reports a.txt %zu time(s)\n",
           status_count(&slist, "a.txt"), (size_t)0);
    {
        size_t i, raw_a = 0;

        for (i = 0; i < dlist.count; i++) {
            if (strcmp(dlist.entries[i].path, "a.txt") == 0)
                raw_a++;
        }
        fprintf(stderr, "INFO corrupt_index_duplicate_path: sg_diff_tree_index raw a.txt rows = %zu\n",
               raw_a);
    }

    /* No CHECK asserting a specific count here on purpose: this scenario's
       job is to surface whatever the two implementations actually do (via
       the INFO lines above, always printed), not to declare one particular
       answer "correct" -- that call belongs to whoever decides how to
       converge sg_status_diff_staged, not to this enumeration pass. */

    sg_status_list_free(&slist);
    sg_diff_list_free(&dlist);
    sg_flat_list_free(&head_flat);
    sg_index_free(&idx);
    rm_rf(repo_root);
    free(repo_root);
    free(git_dir);
}

/* Unborn HEAD (old_tree == NULL / an empty head_flat) with a non-empty
   index: every stage-0 index entry must show up as NEW on both sides. */
static void test_unborn_head_nonempty_index(void)
{
    char *git_dir = make_tmp_repo("unborn");
    char *repo_root = sg_repo_root(git_dir);
    head_builder hb;
    sg_index idx;
    unsigned char id1[SG_SHA1_RAW_LEN], id2[SG_SHA1_RAW_LEN];
    const unsigned char c1[] = "first new file\n";
    const unsigned char c2[] = "second new file\n";
    const char *paths[] = {"one.txt", "two.txt"};
    int exp_reported[] = {1, 1};
    sg_status_kind exp_kind[] = {SG_STATUS_NEW, SG_STATUS_NEW};

    head_builder_init(&hb);
    memset(&idx, 0, sizeof(idx));
    blob_write(git_dir, c1, sizeof(c1) - 1, id1);
    blob_write(git_dir, c2, sizeof(c2) - 1, id2);
    index_upsert(&idx, "one.txt", 0, 0100644, id1);
    index_upsert(&idx, "two.txt", 0, 0100644, id2);
    /* hb stays empty: unborn HEAD */

    run_and_check(git_dir, &hb, &idx, paths, exp_reported, exp_kind, 2, "unborn_head");

    sg_index_free(&idx);
    rm_rf(repo_root);
    free(repo_root);
    free(git_dir);
}

/* Path-ordering boundary: 'a', 'a/b', 'a.txt', 'ab' -- '.' is 0x2E and '/' is
   0x2F, both sitting right next to the alphanumeric range, so a merge-join
   that gets byte-wise comparison wrong (e.g. accidentally treating '/' as
   "less than everything" the way a segment-aware comparator might) could
   misorder these relative to each other and skip or duplicate a path. All
   four are independently modified so a misordering would surface as a
   missing or duplicated report, not just a silent no-op. */
static void test_path_ordering_boundary(void)
{
    char *git_dir = make_tmp_repo("path_order");
    char *repo_root = sg_repo_root(git_dir);
    head_builder hb;
    sg_index idx;
    const char *names[] = {"a", "a/b", "a.txt", "ab"};
    unsigned char old_ids[4][SG_SHA1_RAW_LEN];
    unsigned char new_ids[4][SG_SHA1_RAW_LEN];
    int exp_reported[4] = {1, 1, 1, 1};
    sg_status_kind exp_kind[4] = {SG_STATUS_MODIFIED, SG_STATUS_MODIFIED, SG_STATUS_MODIFIED,
                                  SG_STATUS_MODIFIED};
    size_t i;

    head_builder_init(&hb);
    memset(&idx, 0, sizeof(idx));
    for (i = 0; i < 4; i++) {
        unsigned char old_content[16];
        unsigned char new_content[16];

        snprintf((char *)old_content, sizeof(old_content), "old-%zu", i);
        snprintf((char *)new_content, sizeof(new_content), "new-%zu", i);
        blob_write(git_dir, old_content, strlen((char *)old_content), old_ids[i]);
        blob_write(git_dir, new_content, strlen((char *)new_content), new_ids[i]);
        head_builder_add(&hb, names[i], 0100644, old_ids[i]);
        index_upsert(&idx, names[i], 0, 0100644, new_ids[i]);
    }

    run_and_check(git_dir, &hb, &idx, names, exp_reported, exp_kind, 4, "path_ordering_boundary");

    sg_index_free(&idx);
    rm_rf(repo_root);
    free(repo_root);
    free(git_dir);
}

/* ==================================================================== *
 * Randomized parity fuzzer.
 * ==================================================================== */

typedef enum {
    SC_CLEAN,
    SC_MODIFIED,
    SC_MODE_ONLY,
    SC_BINARY_MODIFIED,
    SC_DELETED,
    SC_NEW,
    SC_UNMERGED_NO_STAGE2_TREE_HAS_PATH,
    SC_UNMERGED_ALL_STAGES_TREE_HAS_PATH,
    SC_UNMERGED_ALL_STAGES_TREE_NO_PATH,
    SC_UNMERGED_STAGE2_ONLY_TREE_HAS_PATH,
    SC_COUNT
} scenario;

typedef struct {
    int reported; /* 0 or 1 */
    sg_status_kind kind; /* meaningful only when reported */
} expectation;

static void setup_one(head_builder *hb, sg_index *idx, const char *git_dir, const char *path,
                      scenario sc, expectation *exp)
{
    memset(exp, 0, sizeof(*exp));

    switch (sc) {
    case SC_CLEAN: {
        unsigned char id[SG_SHA1_RAW_LEN];
        const unsigned char content[] = "clean, unmodified content\n";

        blob_write(git_dir, content, sizeof(content) - 1, id);
        head_builder_add(hb, path, 0100644, id);
        index_upsert(idx, path, 0, 0100644, id);
        exp->reported = 0;
        break;
    }
    case SC_MODIFIED: {
        unsigned char old_id[SG_SHA1_RAW_LEN], new_id[SG_SHA1_RAW_LEN];
        const unsigned char old_content[] = "before the edit\n";
        const unsigned char new_content[] = "after the edit, definitely different\n";

        blob_write(git_dir, old_content, sizeof(old_content) - 1, old_id);
        blob_write(git_dir, new_content, sizeof(new_content) - 1, new_id);
        head_builder_add(hb, path, 0100644, old_id);
        index_upsert(idx, path, 0, 0100644, new_id);
        exp->reported = 1;
        exp->kind = SG_STATUS_MODIFIED;
        break;
    }
    case SC_MODE_ONLY: {
        unsigned char id[SG_SHA1_RAW_LEN];
        const unsigned char content[] = "same bytes, different mode bit\n";

        blob_write(git_dir, content, sizeof(content) - 1, id);
        head_builder_add(hb, path, 0100644, id);
        index_upsert(idx, path, 0, 0100755, id);
        exp->reported = 1;
        exp->kind = SG_STATUS_MODIFIED;
        break;
    }
    case SC_BINARY_MODIFIED: {
        unsigned char old_id[SG_SHA1_RAW_LEN], new_id[SG_SHA1_RAW_LEN];
        unsigned char old_content[40];
        unsigned char new_content[40];

        rand_bytes(old_content, sizeof(old_content));
        do {
            rand_bytes(new_content, sizeof(new_content));
        } while (memcmp(old_content, new_content, sizeof(old_content)) == 0);
        blob_write(git_dir, old_content, sizeof(old_content), old_id);
        blob_write(git_dir, new_content, sizeof(new_content), new_id);
        head_builder_add(hb, path, 0100644, old_id);
        index_upsert(idx, path, 0, 0100644, new_id);
        exp->reported = 1;
        exp->kind = SG_STATUS_MODIFIED;
        break;
    }
    case SC_DELETED: {
        unsigned char id[SG_SHA1_RAW_LEN];
        const unsigned char content[] = "will be deleted from the index\n";

        blob_write(git_dir, content, sizeof(content) - 1, id);
        head_builder_add(hb, path, 0100644, id);
        /* no index entry */
        exp->reported = 1;
        exp->kind = SG_STATUS_DELETED;
        break;
    }
    case SC_NEW: {
        unsigned char id[SG_SHA1_RAW_LEN];
        const unsigned char content[] = "freshly staged, not in HEAD\n";

        blob_write(git_dir, content, sizeof(content) - 1, id);
        index_upsert(idx, path, 0, 0100644, id);
        /* no head entry */
        exp->reported = 1;
        exp->kind = SG_STATUS_NEW;
        break;
    }
    case SC_UNMERGED_NO_STAGE2_TREE_HAS_PATH: {
        unsigned char head_id[SG_SHA1_RAW_LEN], base_id[SG_SHA1_RAW_LEN], theirs_id[SG_SHA1_RAW_LEN];
        const unsigned char head_content[] = "what HEAD has\n";
        const unsigned char base_content[] = "base\n";
        const unsigned char theirs_content[] = "theirs\n";

        blob_write(git_dir, head_content, sizeof(head_content) - 1, head_id);
        blob_write(git_dir, base_content, sizeof(base_content) - 1, base_id);
        blob_write(git_dir, theirs_content, sizeof(theirs_content) - 1, theirs_id);
        head_builder_add(hb, path, 0100644, head_id);
        index_upsert(idx, path, 1, 0100644, base_id);
        index_upsert(idx, path, 3, 0100644, theirs_id);
        exp->reported = 0;
        break;
    }
    case SC_UNMERGED_ALL_STAGES_TREE_HAS_PATH: {
        unsigned char head_id[SG_SHA1_RAW_LEN], base_id[SG_SHA1_RAW_LEN], ours_id[SG_SHA1_RAW_LEN],
            theirs_id[SG_SHA1_RAW_LEN];
        const unsigned char head_content[] = "what HEAD has\n";
        const unsigned char base_content[] = "base\n";
        const unsigned char ours_content[] = "ours\n";
        const unsigned char theirs_content[] = "theirs\n";

        blob_write(git_dir, head_content, sizeof(head_content) - 1, head_id);
        blob_write(git_dir, base_content, sizeof(base_content) - 1, base_id);
        blob_write(git_dir, ours_content, sizeof(ours_content) - 1, ours_id);
        blob_write(git_dir, theirs_content, sizeof(theirs_content) - 1, theirs_id);
        head_builder_add(hb, path, 0100644, head_id);
        index_upsert(idx, path, 1, 0100644, base_id);
        index_upsert(idx, path, 2, 0100644, ours_id);
        index_upsert(idx, path, 3, 0100644, theirs_id);
        exp->reported = 0;
        break;
    }
    case SC_UNMERGED_ALL_STAGES_TREE_NO_PATH: {
        unsigned char base_id[SG_SHA1_RAW_LEN], ours_id[SG_SHA1_RAW_LEN], theirs_id[SG_SHA1_RAW_LEN];
        const unsigned char base_content[] = "base\n";
        const unsigned char ours_content[] = "ours\n";
        const unsigned char theirs_content[] = "theirs\n";

        blob_write(git_dir, base_content, sizeof(base_content) - 1, base_id);
        blob_write(git_dir, ours_content, sizeof(ours_content) - 1, ours_id);
        blob_write(git_dir, theirs_content, sizeof(theirs_content) - 1, theirs_id);
        index_upsert(idx, path, 1, 0100644, base_id);
        index_upsert(idx, path, 2, 0100644, ours_id);
        index_upsert(idx, path, 3, 0100644, theirs_id);
        /* no head entry */
        exp->reported = 0;
        break;
    }
    case SC_UNMERGED_STAGE2_ONLY_TREE_HAS_PATH: {
        unsigned char head_id[SG_SHA1_RAW_LEN], ours_id[SG_SHA1_RAW_LEN];
        const unsigned char head_content[] = "what HEAD has\n";
        const unsigned char ours_content[] = "ours, added independently\n";

        blob_write(git_dir, head_content, sizeof(head_content) - 1, head_id);
        blob_write(git_dir, ours_content, sizeof(ours_content) - 1, ours_id);
        head_builder_add(hb, path, 0100644, head_id);
        index_upsert(idx, path, 2, 0100644, ours_id);
        exp->reported = 0;
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
    case SC_MODE_ONLY:
        return "MODE_ONLY";
    case SC_BINARY_MODIFIED:
        return "BINARY_MODIFIED";
    case SC_DELETED:
        return "DELETED";
    case SC_NEW:
        return "NEW";
    case SC_UNMERGED_NO_STAGE2_TREE_HAS_PATH:
        return "UNMERGED_NO_STAGE2_TREE_HAS_PATH";
    case SC_UNMERGED_ALL_STAGES_TREE_HAS_PATH:
        return "UNMERGED_ALL_STAGES_TREE_HAS_PATH";
    case SC_UNMERGED_ALL_STAGES_TREE_NO_PATH:
        return "UNMERGED_ALL_STAGES_TREE_NO_PATH";
    case SC_UNMERGED_STAGE2_ONLY_TREE_HAS_PATH:
        return "UNMERGED_STAGE2_ONLY_TREE_HAS_PATH";
    default:
        return "?";
    }
}

#define MAX_PATHS 16

/* Tracks distinct divergence "shapes" seen across the whole fuzz run, keyed
   by (scenario, which side was wrong, which kind of mismatch) -- printed
   once at the end so the report can state how many DISTINCT divergence
   classes were found, not just a raw failure count that double-counts the
   same bug across many seeds. */
#define MAX_DIVERGENCE_CLASSES 64
static char g_divergence_classes[MAX_DIVERGENCE_CLASSES][256];
static int g_divergence_first_seed[MAX_DIVERGENCE_CLASSES];
static int g_divergence_class_count = 0;

static void record_divergence(const char *desc, long seed)
{
    int i;

    for (i = 0; i < g_divergence_class_count; i++) {
        if (strcmp(g_divergence_classes[i], desc) == 0)
            return;
    }
    if (g_divergence_class_count < MAX_DIVERGENCE_CLASSES) {
        snprintf(g_divergence_classes[g_divergence_class_count], sizeof(g_divergence_classes[0]),
                "%s", desc);
        g_divergence_first_seed[g_divergence_class_count] = (int)seed;
        g_divergence_class_count++;
    }
}

static void fuzz_parity_round(uint64_t seed)
{
    char *git_dir;
    char *repo_root;
    head_builder hb;
    sg_index idx;
    unsigned int n_paths;
    unsigned int i;
    char paths[MAX_PATHS][32];
    scenario scenarios[MAX_PATHS];
    expectation expectations[MAX_PATHS];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    int has_tree;
    sg_flat_list head_flat;
    sg_status_list slist;
    sg_diff_list dlist;
    int status_rc, diff_rc;

    seed_prng(seed);
    n_paths = 3 + rand_below(MAX_PATHS - 3);

    git_dir = make_tmp_repo("fuzz");
    repo_root = sg_repo_root(git_dir);
    head_builder_init(&hb);
    memset(&idx, 0, sizeof(idx));

    for (i = 0; i < n_paths; i++) {
        snprintf(paths[i], sizeof(paths[i]), "p%02u_%s.txt", i, (rand_below(2) == 0) ? "a" : "b");
        scenarios[i] = (scenario)rand_below(SC_COUNT);
        setup_one(&hb, &idx, git_dir, paths[i], scenarios[i], &expectations[i]);
    }

    head_builder_finish(&hb, git_dir, tree_id, &has_tree, &head_flat);

    status_rc = sg_status_diff_staged(git_dir, git_dir, has_tree ? tree_id : NULL, &idx, 0,
                                     &slist, NULL);
    diff_rc = sg_diff_tree_index(git_dir, has_tree ? tree_id : NULL, &idx, &dlist, NULL);

    CHECK(status_rc == 0, "seed %llu: sg_status_diff_staged failed unexpectedly",
         (unsigned long long)seed);
    CHECK(diff_rc == 0, "seed %llu: sg_diff_tree_index failed unexpectedly",
         (unsigned long long)seed);

    if (status_rc == 0 && diff_rc == 0) {
        for (i = 0; i < n_paths; i++) {
            size_t sc = status_count(&slist, paths[i]);
            size_t dc = diff_count_normalized(&dlist, paths[i]);
            size_t want = expectations[i].reported ? 1 : 0;
            int status_ok = (sc == want);
            int diff_ok = (dc == want);
            char class_desc[256];

            if (!status_ok) {
                CHECK(0,
                     "seed %llu path %s scenario %s: sg_status_diff_staged reported %zu entries, "
                     "expected %zu",
                     (unsigned long long)seed, paths[i], scenario_name(scenarios[i]), sc, want);
                snprintf(class_desc, sizeof(class_desc),
                        "status_diff_staged count mismatch, scenario=%s expected_reported=%d",
                        scenario_name(scenarios[i]), expectations[i].reported);
                record_divergence(class_desc, (long)seed);
            }
            if (!diff_ok) {
                CHECK(0,
                     "seed %llu path %s scenario %s: sg_diff_tree_index (normalized) reported "
                     "%zu entries, expected %zu",
                     (unsigned long long)seed, paths[i], scenario_name(scenarios[i]), dc, want);
                snprintf(class_desc, sizeof(class_desc),
                        "diff_tree_index count mismatch, scenario=%s expected_reported=%d",
                        scenario_name(scenarios[i]), expectations[i].reported);
                record_divergence(class_desc, (long)seed);
            }

            if (status_ok && sc == 1 && want == 1) {
                const sg_status_entry *e = status_find(&slist, paths[i]);

                if (e != NULL && e->kind != expectations[i].kind) {
                    CHECK(0,
                         "seed %llu path %s scenario %s: sg_status_diff_staged reported kind %d, "
                         "expected %d",
                         (unsigned long long)seed, paths[i], scenario_name(scenarios[i]),
                         (int)e->kind, (int)expectations[i].kind);
                    snprintf(class_desc, sizeof(class_desc),
                            "status_diff_staged kind mismatch, scenario=%s expected_kind=%d",
                            scenario_name(scenarios[i]), (int)expectations[i].kind);
                    record_divergence(class_desc, (long)seed);
                }
            }
            if (diff_ok && dc == 1 && want == 1) {
                sg_status_kind got_kind;

                if (diff_find_normalized(&dlist, paths[i], &got_kind) &&
                   got_kind != expectations[i].kind) {
                    CHECK(0,
                         "seed %llu path %s scenario %s: sg_diff_tree_index (normalized) reported "
                         "kind %d, expected %d",
                         (unsigned long long)seed, paths[i], scenario_name(scenarios[i]),
                         (int)got_kind, (int)expectations[i].kind);
                    snprintf(class_desc, sizeof(class_desc),
                            "diff_tree_index kind mismatch, scenario=%s expected_kind=%d",
                            scenario_name(scenarios[i]), (int)expectations[i].kind);
                    record_divergence(class_desc, (long)seed);
                }
            }
        }
    }

    if (status_rc == 0)
        sg_status_list_free(&slist);
    if (diff_rc == 0)
        sg_diff_list_free(&dlist);
    sg_flat_list_free(&head_flat);
    sg_index_free(&idx);
    rm_rf(repo_root);
    free(repo_root);
    free(git_dir);
}

/* Self-verification: with g_mutate_normalize forced on, normalize_diff_kind
   swaps NEW <-> DELETED, which must turn the deterministic DELETED and NEW
   scenarios red. Proves this harness can actually detect a divergence,
   rather than the absence of failures meaning nothing (see CLAUDE.md's
   "reverse mutations catch over-broad rules" / "a harness that cannot
   detect divergence looks identical to a system with none"). Runs in an
   isolated sub-check so it never contaminates the real failure count. */
static int self_check_harness_can_detect_divergence(void)
{
    int saved_failures = failures;
    int caught;

    failures = 0;
    g_mutate_normalize = 1;
    g_self_check_mode = 1;
    test_deleted_agrees();
    test_new_agrees();
    g_self_check_mode = 0;
    caught = failures;
    g_mutate_normalize = 0;
    failures = saved_failures;
    return caught;
}

int main(void)
{
    long iters = env_long("SG_FUZZ_ITERS", 200);
    long seed_base = env_long("SG_FUZZ_SEED_BASE", 0);
    long i;
    int self_check_caught;

    test_clean_agrees();
    test_modified_agrees();
    test_mode_only();
    test_deleted_agrees();
    test_new_agrees();
    test_unmerged_no_stage2_tree_has_path();
    test_unmerged_all_stages_tree_has_path();
    test_unmerged_all_stages_tree_no_path();
    test_unmerged_stage2_only_tree_has_path();
    test_corrupt_index_duplicate_path();
    test_unborn_head_nonempty_index();
    test_path_ordering_boundary();

    for (i = 0; i < iters; i++)
        fuzz_parity_round((uint64_t)(seed_base + i));

    fprintf(stderr, "distinct divergence classes found by the fuzz loop: %d\n",
           g_divergence_class_count);
    for (i = 0; i < g_divergence_class_count; i++) {
        fprintf(stderr, "  [%ld] %s (first seed_base-relative seed: %d)\n", i,
               g_divergence_classes[i], g_divergence_first_seed[i]);
    }

    self_check_caught = self_check_harness_can_detect_divergence();
    fprintf(stderr,
           "self-check: forcing normalize_diff_kind to swap NEW/DELETED caught %d failure(s) "
           "(expected > 0 -- proves the harness can detect a divergence)\n",
           self_check_caught);

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    if (self_check_caught == 0) {
        fprintf(stderr,
               "self-check FAILED: forcing a NEW/DELETED swap did not turn anything red -- this "
               "harness cannot be trusted to detect a real divergence\n");
        return 1;
    }
    printf("all status/diff staged-parity checks passed (%ld fuzz rounds, seed_base %ld, %d "
          "distinct divergence classes found)\n",
          iters, seed_base, g_divergence_class_count);
    return 0;
}
