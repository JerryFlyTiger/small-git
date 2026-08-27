#include "sg/diff.h"

#include "sg/diff_out.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, ...)                                                                         \
    do {                                                                                          \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);                                   \
            fprintf(stderr, __VA_ARGS__);                                                          \
            fprintf(stderr, "\n");                                                                 \
            failures++;                                                                            \
        }                                                                                           \
    } while (0)

/* Every entry below uses SG_DIFF_SIDE_WORKDIR sides. That is deliberate: a
   WORKDIR side's id IS its content id, so sg_diff_side_effective_id answers
   from the struct alone and these tests need no repository, no object store
   and no fixture files. The BLOB path -- where a chunk pointer has to be
   resolved before two ids may be compared -- cannot be exercised that way
   and is covered end-to-end by tests/interop.sh instead, against real git.

   The pairing RULE is the same either way, and it is the rule these tests
   are about. */
static sg_diff_side side(int present, unsigned char tag)
{
    sg_diff_side s;

    memset(&s, 0, sizeof(s));
    if (!present) {
        s.kind = SG_DIFF_SIDE_ABSENT;
        return s;
    }
    s.kind = SG_DIFF_SIDE_WORKDIR;
    s.mode = 0100644;
    memset(s.id, tag, SG_SHA1_RAW_LEN);
    return s;
}

/* Appends in the caller's order; every list below is written already sorted
   by path, which is the invariant sg_diff_detect_renames relies on and
   preserves. */
static void add(sg_diff_list *list, const char *path, int old_present, unsigned char old_tag,
                int new_present, unsigned char new_tag)
{
    sg_diff_entry *grown = realloc(list->entries, (list->count + 1) * sizeof(*grown));

    if (grown == NULL) {
        fprintf(stderr, "setup failed: realloc\n");
        exit(1);
    }
    list->entries = grown;
    memset(&list->entries[list->count], 0, sizeof(list->entries[list->count]));
    list->entries[list->count].path = strdup(path);
    if (list->entries[list->count].path == NULL) {
        fprintf(stderr, "setup failed: strdup\n");
        exit(1);
    }
    list->entries[list->count].old_side = side(old_present, old_tag);
    list->entries[list->count].new_side = side(new_present, new_tag);
    list->count++;
    list->cap = list->count;
}

static void add_deletion(sg_diff_list *l, const char *path, unsigned char tag)
{
    add(l, path, 1, tag, 0, 0);
}

static void add_addition(sg_diff_list *l, const char *path, unsigned char tag)
{
    add(l, path, 0, 0, 1, tag);
}

static const sg_diff_entry *find(const sg_diff_list *l, const char *path)
{
    size_t i;

    for (i = 0; i < l->count; i++) {
        if (strcmp(l->entries[i].path, path) == 0)
            return &l->entries[i];
    }
    return NULL;
}

static void test_exact_rename_becomes_one_row(void)
{
    sg_diff_list l;
    const sg_diff_entry *e;

    memset(&l, 0, sizeof(l));
    add_deletion(&l, "old.txt", 0xAA);
    add_addition(&l, "zew.txt", 0xAA);

    CHECK(sg_diff_detect_renames("/nonexistent", "/nonexistent", &l, 50) == 0, "detection succeeds");
    CHECK(l.count == 1, "two rows collapse into one");
    e = find(&l, "zew.txt");
    CHECK(e != NULL, "the surviving row is the destination");
    if (e != NULL) {
        CHECK(e->old_path != NULL && strcmp(e->old_path, "old.txt") == 0,
              "and it remembers where the content came from");
        CHECK(e->score == 100, "an exact rename scores 100");
        CHECK(e->old_side.kind == SG_DIFF_SIDE_WORKDIR, "it took the source's old side");
        CHECK(e->new_side.kind == SG_DIFF_SIDE_WORKDIR, "and kept its own new side");
    }
    sg_diff_list_free(&l);
}

static void test_different_content_is_not_a_rename(void)
{
    sg_diff_list l;

    memset(&l, 0, sizeof(l));
    add_deletion(&l, "old.txt", 0xAA);
    add_addition(&l, "zew.txt", 0xBB);

    CHECK(sg_diff_detect_renames("/nonexistent", "/nonexistent", &l, 50) == 0, "detection succeeds");
    CHECK(l.count == 2, "unequal content stays a delete plus an add");
    CHECK(find(&l, "old.txt") != NULL && find(&l, "old.txt")->old_path == NULL,
          "the deletion is untouched");
    CHECK(find(&l, "zew.txt") != NULL && find(&l, "zew.txt")->old_path == NULL,
          "so is the addition");
    sg_diff_list_free(&l);
}

/* Measured against git 2.55.0: two identical sources and two identical
   destinations pair up in path order (a1->b1, a2->b2), not crosswise. */
static void test_pairs_in_path_order(void)
{
    sg_diff_list l;

    memset(&l, 0, sizeof(l));
    add_deletion(&l, "a1.txt", 0xAA);
    add_deletion(&l, "a2.txt", 0xAA);
    add_addition(&l, "b1.txt", 0xAA);
    add_addition(&l, "b2.txt", 0xAA);

    CHECK(sg_diff_detect_renames("/nonexistent", "/nonexistent", &l, 50) == 0, "detection succeeds");
    CHECK(l.count == 2, "both pairs collapse");
    CHECK(find(&l, "b1.txt") != NULL && find(&l, "b1.txt")->old_path != NULL &&
          strcmp(find(&l, "b1.txt")->old_path, "a1.txt") == 0, "b1 comes from a1");
    CHECK(find(&l, "b2.txt") != NULL && find(&l, "b2.txt")->old_path != NULL &&
          strcmp(find(&l, "b2.txt")->old_path, "a2.txt") == 0, "b2 comes from a2");
    sg_diff_list_free(&l);
}

/* Measured: one source and two identical destinations gives ONE rename and
   leaves the second destination an ordinary addition -- git does not report
   a copy unless asked, and even `-C` did not report one here. */
static void test_a_source_is_used_once(void)
{
    sg_diff_list l;

    memset(&l, 0, sizeof(l));
    add_deletion(&l, "src.txt", 0xAA);
    add_addition(&l, "d1.txt", 0xAA);
    add_addition(&l, "d2.txt", 0xAA);

    CHECK(sg_diff_detect_renames("/nonexistent", "/nonexistent", &l, 50) == 0, "detection succeeds");
    CHECK(l.count == 2, "one rename plus one addition");
    CHECK(find(&l, "d1.txt") != NULL && find(&l, "d1.txt")->old_path != NULL,
          "the first destination in path order claims the source");
    CHECK(find(&l, "d2.txt") != NULL && find(&l, "d2.txt")->old_path == NULL,
          "the second is still a plain addition");
    CHECK(find(&l, "src.txt") == NULL, "the source row is gone");
    sg_diff_list_free(&l);
}

static void test_modifications_and_unmerged_never_pair(void)
{
    sg_diff_list l;

    memset(&l, 0, sizeof(l));
    /* A modification has both sides present, so it is neither a source nor
       a destination however its ids compare. */
    add(&l, "mod.txt", 1, 0xAA, 1, 0xCC);
    add_addition(&l, "new.txt", 0xAA);
    memset(&l.entries[0].new_side, 0, sizeof(l.entries[0].new_side));
    l.entries[0].new_side.kind = SG_DIFF_SIDE_BLOB;

    CHECK(sg_diff_detect_renames("/nonexistent", "/nonexistent", &l, 50) == 0, "detection succeeds");
    CHECK(l.count == 2, "a modification is not a rename source");
    sg_diff_list_free(&l);

    memset(&l, 0, sizeof(l));
    add_deletion(&l, "a.txt", 0xAA);
    add_addition(&l, "b.txt", 0xAA);
    l.entries[0].unmerged = 1;
    CHECK(sg_diff_detect_renames("/nonexistent", "/nonexistent", &l, 50) == 0, "detection succeeds");
    CHECK(l.count == 2, "an unmerged row is never a rename source");
    sg_diff_list_free(&l);
}

/* git's --no-renames. Passing 0 must leave the list exactly as it was, which
   is the only way `sg diff --no-renames` can keep matching git. */
static void test_zero_score_disables(void)
{
    sg_diff_list l;

    memset(&l, 0, sizeof(l));
    add_deletion(&l, "old.txt", 0xAA);
    add_addition(&l, "zew.txt", 0xAA);

    CHECK(sg_diff_detect_renames("/nonexistent", "/nonexistent", &l, 0) == 0, "detection succeeds");
    CHECK(l.count == 2, "--no-renames leaves both rows");
    CHECK(l.entries[0].old_path == NULL && l.entries[1].old_path == NULL,
          "and marks neither as a rename");
    sg_diff_list_free(&l);
}

/* Only source rows are removed and destinations keep the path they already
   had, so the list must still be sorted afterwards -- every renderer and
   `git diff --name-only`'s output order depend on it. */
static void test_list_stays_sorted(void)
{
    sg_diff_list l;
    size_t i;

    memset(&l, 0, sizeof(l));
    add_deletion(&l, "a.txt", 0xAA);
    add_addition(&l, "b.txt", 0xBB);
    add_deletion(&l, "c.txt", 0xCC);
    add_addition(&l, "d.txt", 0xCC);
    add_addition(&l, "e.txt", 0xAA);

    CHECK(sg_diff_detect_renames("/nonexistent", "/nonexistent", &l, 50) == 0, "detection succeeds");
    CHECK(l.count == 3, "two pairs collapse, one addition stands alone");
    for (i = 1; i < l.count; i++)
        CHECK(strcmp(l.entries[i - 1].path, l.entries[i].path) < 0,
              "still sorted at %zu (%s then %s)", i, l.entries[i - 1].path, l.entries[i].path);
    sg_diff_list_free(&l);
}

static void test_empty_and_single_lists(void)
{
    sg_diff_list l;

    memset(&l, 0, sizeof(l));
    CHECK(sg_diff_detect_renames("/nonexistent", "/nonexistent", &l, 50) == 0, "an empty list is fine");
    CHECK(l.count == 0, "and stays empty");
    CHECK(sg_diff_detect_renames("/nonexistent", "/nonexistent", NULL, 50) == 0, "so is no list at all");

    add_deletion(&l, "only.txt", 0xAA);
    CHECK(sg_diff_detect_renames("/nonexistent", "/nonexistent", &l, 50) == 0, "one row is fine");
    CHECK(l.count == 1 && l.entries[0].old_path == NULL, "a lone deletion is not a rename");
    sg_diff_list_free(&l);
}

/* The safety gate with no coverage until now: a side whose effective id
   could not be VERIFIED must never be paired. sg_diff_side_effective_id
   returns -1 for exactly that case, and two unverified ids that happen to be
   byte-equal are not proof the content matches -- pairing on them would
   invent a rename that never happened.

   Reaching it needs a BLOB side (a WORKDIR side's id is its own content
   hash, always verified) whose id cannot be resolved through the object
   store. Pointing git_dir at a directory that does not exist does that
   without a fixture: the object read fails, so the id stays unverified.

   Note the ids here are deliberately IDENTICAL. That is the whole point: a
   version of this code that skipped the verification check would pair them
   and report a rename, and every other test in this file and in interop.sh
   would still pass. */
static void test_unverified_ids_are_never_paired(void)
{
    sg_diff_list l;

    memset(&l, 0, sizeof(l));
    add_deletion(&l, "old.txt", 0xAA);
    add_addition(&l, "zew.txt", 0xAA);
    l.entries[0].old_side.kind = SG_DIFF_SIDE_BLOB;
    l.entries[1].new_side.kind = SG_DIFF_SIDE_BLOB;

    CHECK(sg_diff_detect_renames("/nonexistent", "/nonexistent", &l, 50) == 0, "detection still succeeds");
    CHECK(l.count == 2, "identical but unverified ids are not a rename");
    CHECK(l.entries[0].old_path == NULL && l.entries[1].old_path == NULL,
          "and neither row claims to be one");
    sg_diff_list_free(&l);

    /* The control that makes the check above mean something: the SAME ids on
       WORKDIR sides -- where the id is the content hash and needs no
       resolving -- do pair. Without this, "count == 2" above could equally
       be satisfied by detection that never pairs anything at all. */
    memset(&l, 0, sizeof(l));
    add_deletion(&l, "old.txt", 0xAA);
    add_addition(&l, "zew.txt", 0xAA);
    CHECK(sg_diff_detect_renames("/nonexistent", "/nonexistent", &l, 50) == 0, "detection succeeds");
    CHECK(l.count == 1, "the same ids on verified sides DO pair");
    sg_diff_list_free(&l);
}

/* ---- rendering: the one property detection alone cannot pin ------------ */

/* The score is printed zero-padded to three digits ("R093"), which is
   invisible while every rename sg finds is exact: "R%d" and "R%03d" both
   print "R100". A directed mutation proved exactly that -- swapping the
   format reddened nothing in the whole suite. So the padding gets its own
   test, driven by a hand-built score of 93 that detection cannot currently
   produce.

   --name-status is the only format renderable from a fabricated list: it
   prints paths and the status letter without ever reading content, so no
   repository, no objects and no fixture files are needed. The formats that
   do read content (--stat, --numstat, patch) are covered end-to-end against
   real git in tests/interop.sh. */
static int g_saved_stdout = -1;
static char g_capture_path[] = "/tmp/sg_rename_capture_XXXXXX";

static void capture_start(void)
{
    char path[] = "/tmp/sg_rename_capture_XXXXXX";
    int fd;

    fflush(stdout);
    fd = mkstemp(path);
    if (fd < 0) {
        fprintf(stderr, "setup failed: mkstemp\n");
        exit(1);
    }
    memcpy(g_capture_path, path, sizeof(g_capture_path));
    g_saved_stdout = dup(STDOUT_FILENO);
    dup2(fd, STDOUT_FILENO);
    close(fd);
}

static char *capture_end(void)
{
    FILE *f;
    long len;
    char *buf;

    fflush(stdout);
    dup2(g_saved_stdout, STDOUT_FILENO);
    close(g_saved_stdout);
    g_saved_stdout = -1;

    f = fopen(g_capture_path, "rb");
    if (f == NULL) {
        fprintf(stderr, "setup failed: reopen capture\n");
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)len + 1);
    if (buf == NULL || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "setup failed: read capture\n");
        exit(1);
    }
    buf[len] = '\0';
    fclose(f);
    unlink(g_capture_path);
    return buf;
}

static char *render_name_status(sg_diff_list *l)
{
    sg_diff_out_opts opts;

    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_NAME_STATUS;
    capture_start();
    sg_diff_print("/nonexistent", "/nonexistent", l, &opts);
    return capture_end();
}

static void test_score_is_zero_padded_to_three_digits(void)
{
    sg_diff_list l;
    char *out;

    memset(&l, 0, sizeof(l));
    add_addition(&l, "new.txt", 0xAA);
    l.entries[0].old_path = strdup("old.txt");
    if (l.entries[0].old_path == NULL) {
        fprintf(stderr, "setup failed: strdup\n");
        exit(1);
    }
    l.entries[0].score = 93;

    out = render_name_status(&l);
    CHECK(strcmp(out, "R093\told.txt\tnew.txt\n") == 0,
          "a score below 100 is zero-padded to three digits, got [%s]", out);
    free(out);
    sg_diff_list_free(&l);

    /* The control: 100 must NOT gain a fourth digit. */
    memset(&l, 0, sizeof(l));
    add_addition(&l, "new.txt", 0xAA);
    l.entries[0].old_path = strdup("old.txt");
    if (l.entries[0].old_path == NULL) {
        fprintf(stderr, "setup failed: strdup\n");
        exit(1);
    }
    l.entries[0].score = 100;
    out = render_name_status(&l);
    CHECK(strcmp(out, "R100\told.txt\tnew.txt\n") == 0,
          "an exact rename prints R100, got [%s]", out);
    free(out);
    sg_diff_list_free(&l);
}

/* A renamed row's OLD side must be read from the OLD path. No builder
   currently produces a WORKDIR old_side, so nothing in the normal flow can
   tell the two apart -- which is exactly why this test fabricates the
   configuration directly rather than going through a builder. Rewriting
   old_side_path to return e->path leaves the entire suite green without it.

   If the wrong path were used, both sides would be read from the SAME file
   (the destination), the contents would match, and the patch would carry no
   removal line at all. */
static void test_rename_reads_its_old_side_from_the_old_path(void)
{
    char tmpl[] = "/tmp/sg_rename_oldside_XXXXXX";
    char oldp[512], newp[512];
    sg_diff_list l;
    sg_diff_out_opts opts;
    char *out;
    FILE *f;

    if (mkdtemp(tmpl) == NULL) {
        fprintf(stderr, "setup failed: mkdtemp\n");
        exit(1);
    }
    snprintf(oldp, sizeof(oldp), "%s/old.txt", tmpl);
    snprintf(newp, sizeof(newp), "%s/new.txt", tmpl);
    f = fopen(oldp, "w");
    if (f == NULL) {
        fprintf(stderr, "setup failed: write old.txt\n");
        exit(1);
    }
    fputs("ONLY-IN-THE-OLD-FILE\n", f);
    fclose(f);
    f = fopen(newp, "w");
    if (f == NULL) {
        fprintf(stderr, "setup failed: write new.txt\n");
        exit(1);
    }
    fputs("only-in-the-new-file\n", f);
    fclose(f);

    memset(&l, 0, sizeof(l));
    /* Both sides WORKDIR, with different ids so the row counts as a content
       change and a body actually gets rendered. */
    add(&l, "new.txt", 1, 0xAA, 1, 0xBB);
    l.entries[0].old_side.kind = SG_DIFF_SIDE_WORKDIR;
    l.entries[0].new_side.kind = SG_DIFF_SIDE_WORKDIR;
    l.entries[0].old_path = strdup("old.txt");
    if (l.entries[0].old_path == NULL) {
        fprintf(stderr, "setup failed: strdup\n");
        exit(1);
    }
    l.entries[0].score = 100;

    memset(&opts, 0, sizeof(opts));
    opts.format = SG_DIFF_FORMAT_PATCH;
    capture_start();
    sg_diff_print(tmpl, tmpl, &l, &opts);
    out = capture_end();

    CHECK(strstr(out, "-ONLY-IN-THE-OLD-FILE") != NULL,
          "the old side's content comes from the old path, got [%s]", out);
    CHECK(strstr(out, "+only-in-the-new-file") != NULL,
          "and the new side's from the new path");
    CHECK(strstr(out, "rename from old.txt") != NULL, "the header names the old path");

    free(out);
    sg_diff_list_free(&l);
    unlink(oldp);
    unlink(newp);
    rmdir(tmpl);
}


/* ---- inexact renames: the score itself (Phase 30) --------------------- */

/* These need real bytes on disk. The score comes from the CONTENT, so the
   fabricated-id trick every test above relies on cannot reach this code at
   all -- an id says nothing about how similar two files are. The sides stay
   WORKDIR sides, so the fixtures are ordinary files under a scratch
   directory and still no object store is involved.

   Every expected score below was produced by running real git 2.55.0 on
   exactly this content, not by reading this implementation back to itself. */

static char *scratch_dir(char *tmpl)
{
    if (mkdtemp(tmpl) == NULL) {
        fprintf(stderr, "setup failed: mkdtemp\n");
        exit(1);
    }
    return tmpl;
}

/* Writes the lines "L-1" .. "L-<count>", one per line. Two such files share
   the shorter one's lines exactly, which is what makes the resulting scores
   easy to state and easy to reproduce with git. */
static void write_lines(const char *root, const char *rel, int count)
{
    char path[512];
    FILE *f;
    int i;

    snprintf(path, sizeof(path), "%s/%s", root, rel);
    f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "setup failed: fopen %s\n", path);
        exit(1);
    }
    for (i = 1; i <= count; i++)
        fprintf(f, "L-%d\n", i);
    fclose(f);
}

static void make_subdir(const char *root, const char *rel)
{
    char path[512];

    snprintf(path, sizeof(path), "%s/%s", root, rel);
    if (mkdir(path, 0700) != 0) {
        fprintf(stderr, "setup failed: mkdir %s\n", path);
        exit(1);
    }
}

static void remove_subdir(const char *root, const char *rel)
{
    char path[512];

    snprintf(path, sizeof(path), "%s/%s", root, rel);
    rmdir(path);
}

static void remove_file(const char *root, const char *rel)
{
    char path[512];

    snprintf(path, sizeof(path), "%s/%s", root, rel);
    unlink(path);
}

/* Writes the same 80 shared lines as write_lines, then pads with lines
   unique to `tag` until the file reaches `want` bytes. Since every source
   contains all of the destination's lines, the score is decided purely by
   the source's SIZE -- which is what makes a set of exact, predictable
   scores (and an exact tie) constructible at all. */
static void write_padded(const char *root, const char *rel, int tag, size_t want)
{
    char path[512];
    FILE *f;
    size_t size = 0;
    int i;

    snprintf(path, sizeof(path), "%s/%s", root, rel);
    f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "setup failed: fopen %s\n", path);
        exit(1);
    }
    for (i = 1; i <= 80; i++)
        size += (size_t)fprintf(f, "L-%d\n", i);
    for (i = 1; size < want; i++)
        size += (size_t)fprintf(f, "f%d-%d\n", tag, i);
    fclose(f);
}

static void test_inexact_rename_carries_gits_score(void)
{
    char tmpl[] = "/tmp/sg_rename_inexact_XXXXXX";
    char *root = scratch_dir(tmpl);
    sg_diff_list l;
    const sg_diff_entry *e;

    write_lines(root, "old.txt", 100);
    write_lines(root, "new.txt", 80);

    memset(&l, 0, sizeof(l));
    /* Deliberately DIFFERENT tags: the ids do not match, so the exact pass
       has nothing to do and only scoring can produce a rename here. The list
       is written already sorted by path, so the addition comes first. */
    add_addition(&l, "new.txt", 0xBB);
    add_deletion(&l, "old.txt", 0xAA);

    CHECK(sg_diff_detect_renames("/nonexistent", root, &l, SG_SIMILARITY_DEFAULT) == 0,
          "detection succeeds");
    CHECK(l.count == 1, "the two rows become one, got %zu", l.count);
    e = find(&l, "new.txt");
    CHECK(e != NULL && e->old_path != NULL && strcmp(e->old_path, "old.txt") == 0,
          "the surviving row is a rename from old.txt");
    /* 79, not 100: dropping the last fifth of the file is exactly what
       separates an inexact rename from an exact one. */
    CHECK(e != NULL && e->score == 79, "git scores this pair 79, got %d",
          e != NULL ? e->score : -1);

    sg_diff_list_free(&l);
    remove_file(root, "old.txt");
    remove_file(root, "new.txt");
    rmdir(root);
}

static void test_the_threshold_is_a_real_comparison(void)
{
    char tmpl[] = "/tmp/sg_rename_thresh_XXXXXX";
    char *root = scratch_dir(tmpl);
    sg_diff_list l;

    /* 100 lines down to 51 scores exactly 30000, git's default threshold, so
       it pairs -- and one point above that threshold it must not. A constant
       standing in for the comparison would pass one of these and fail the
       other. */
    write_lines(root, "old.txt", 100);
    write_lines(root, "new.txt", 51);

    memset(&l, 0, sizeof(l));
    add_addition(&l, "new.txt", 0xBB);
    add_deletion(&l, "old.txt", 0xAA);
    CHECK(sg_diff_detect_renames("/nonexistent", root, &l, SG_SIMILARITY_DEFAULT) == 0,
          "detection succeeds at the default threshold");
    CHECK(l.count == 1, "a pair exactly at the threshold is a rename");
    CHECK(l.count == 1 && l.entries[0].score == 50, "and it scores 50, got %d",
          l.count == 1 ? l.entries[0].score : -1);
    sg_diff_list_free(&l);

    memset(&l, 0, sizeof(l));
    add_addition(&l, "new.txt", 0xBB);
    add_deletion(&l, "old.txt", 0xAA);
    CHECK(sg_diff_detect_renames("/nonexistent", root, &l, SG_SIMILARITY_DEFAULT + 1) == 0,
          "detection succeeds one point higher");
    CHECK(l.count == 2, "one point above it, the same pair is an add and a delete");
    sg_diff_list_free(&l);

    /* Halving the file is ruled out on size alone, before either side is
       even hashed -- the guard that also keeps an empty destination away
       from the score's division. */
    write_lines(root, "new.txt", 50);
    memset(&l, 0, sizeof(l));
    add_addition(&l, "new.txt", 0xBB);
    add_deletion(&l, "old.txt", 0xAA);
    CHECK(sg_diff_detect_renames("/nonexistent", root, &l, SG_SIMILARITY_DEFAULT) == 0,
          "detection succeeds");
    CHECK(l.count == 2, "one line further and the size guard rules it out");
    sg_diff_list_free(&l);

    remove_file(root, "old.txt");
    remove_file(root, "new.txt");
    rmdir(root);
}

static void test_exact_only_threshold_still_finds_exact_renames(void)
{
    char tmpl[] = "/tmp/sg_rename_exactonly_XXXXXX";
    char *root = scratch_dir(tmpl);
    sg_diff_list l;

    /* git's -M100%. It must not mean "find nothing": the exact pass still
       runs, and only the scoring passes are skipped. Checking just the
       inexact half would pass equally well if the whole function returned
       early. */
    write_lines(root, "old.txt", 100);
    write_lines(root, "new.txt", 80);
    memset(&l, 0, sizeof(l));
    add_addition(&l, "new.txt", 0xBB);
    add_deletion(&l, "old.txt", 0xAA);
    CHECK(sg_diff_detect_renames("/nonexistent", root, &l, SG_SIMILARITY_MAX) == 0,
          "detection succeeds");
    CHECK(l.count == 2, "a 79%% pair is not a rename under -M100%%");
    sg_diff_list_free(&l);

    memset(&l, 0, sizeof(l));
    add_addition(&l, "new.txt", 0xAA);
    add_deletion(&l, "old.txt", 0xAA);
    CHECK(sg_diff_detect_renames("/nonexistent", root, &l, SG_SIMILARITY_MAX) == 0,
          "detection succeeds");
    CHECK(l.count == 1, "but identical content still is one under -M100%%");
    CHECK(l.count == 1 && l.entries[0].score == 100, "and still scores 100");
    sg_diff_list_free(&l);

    remove_file(root, "old.txt");
    remove_file(root, "new.txt");
    rmdir(root);
}

static void test_the_file_name_beats_a_better_score(void)
{
    char tmpl[] = "/tmp/sg_rename_basename_XXXXXX";
    char *root = scratch_dir(tmpl);
    sg_diff_list l;
    const sg_diff_entry *e;

    /* The pass order made visible. dir1/foo.txt matches other.txt far more
       closely (98%) than it matches dir2/foo.txt (79%), but a shared file
       name pairs the latter first and takes the source off the table, so
       other.txt is left an ordinary addition. Measured against git 2.55.0.

       Collapse the three passes into "score every pair and keep the best"
       and this test pairs the other way round -- which is still a perfectly
       reasonable answer, just not git's. */
    make_subdir(root, "dir1");
    make_subdir(root, "dir2");
    write_lines(root, "dir1/foo.txt", 100);
    write_lines(root, "dir2/foo.txt", 80);
    write_lines(root, "other.txt", 99);

    memset(&l, 0, sizeof(l));
    add_deletion(&l, "dir1/foo.txt", 0xAA);
    add_addition(&l, "dir2/foo.txt", 0xBB);
    add_addition(&l, "other.txt", 0xCC);

    CHECK(sg_diff_detect_renames("/nonexistent", root, &l, SG_SIMILARITY_DEFAULT) == 0,
          "detection succeeds");
    CHECK(l.count == 2, "one of the two additions becomes the rename, got %zu rows",
          l.count);
    e = find(&l, "dir2/foo.txt");
    CHECK(e != NULL && e->old_path != NULL &&
          strcmp(e->old_path, "dir1/foo.txt") == 0,
          "the same file name wins the source");
    CHECK(e != NULL && e->score == 79, "at its own 79, not the better pair's 98, got %d",
          e != NULL ? e->score : -1);
    e = find(&l, "other.txt");
    CHECK(e != NULL && e->old_path == NULL,
          "the closer match is left an ordinary addition");

    sg_diff_list_free(&l);
    remove_file(root, "dir1/foo.txt");
    remove_file(root, "dir2/foo.txt");
    remove_file(root, "other.txt");
    remove_subdir(root, "dir1");
    remove_subdir(root, "dir2");
    rmdir(root);
}

static void test_a_name_match_below_the_raised_threshold_loses(void)
{
    char tmpl[] = "/tmp/sg_rename_basename_low_XXXXXX";
    char *root = scratch_dir(tmpl);
    sg_diff_list l;
    const sg_diff_entry *e;

    /* The other side of the pass order, and the only thing that pins the
       RAISED threshold down as a number rather than as "some threshold".
       Same shape as the test above, but the same-name pair is now only 60%
       alike -- under the 75% a name match must clear, though still over the
       50% an ordinary pair needs. So the name shortcut declines it, the full
       comparison runs, and the source goes to the file it really does
       resemble. Measured against git 2.55.0.

       Halve the raise and this pairs by name; drop the raise entirely and it
       pairs by name; only the value git uses gives this answer. */
    make_subdir(root, "d1");
    make_subdir(root, "d2");
    write_lines(root, "d1/x.txt", 100);
    write_lines(root, "d2/x.txt", 60);
    write_lines(root, "zz.txt", 99);

    memset(&l, 0, sizeof(l));
    add_deletion(&l, "d1/x.txt", 0xAA);
    add_addition(&l, "d2/x.txt", 0xBB);
    add_addition(&l, "zz.txt", 0xCC);

    CHECK(sg_diff_detect_renames("/nonexistent", root, &l, SG_SIMILARITY_DEFAULT) == 0,
          "detection succeeds");
    CHECK(l.count == 2, "one rename and one addition, got %zu rows", l.count);
    e = find(&l, "zz.txt");
    CHECK(e != NULL && e->old_path != NULL && strcmp(e->old_path, "d1/x.txt") == 0,
          "the better match wins when the name match is too weak");
    CHECK(e != NULL && e->score == 98, "and scores 98, got %d", e != NULL ? e->score : -1);
    e = find(&l, "d2/x.txt");
    CHECK(e != NULL && e->old_path == NULL,
          "the same-name file is left an ordinary addition");

    sg_diff_list_free(&l);
    remove_file(root, "d1/x.txt");
    remove_file(root, "d2/x.txt");
    remove_file(root, "zz.txt");
    remove_subdir(root, "d1");
    remove_subdir(root, "d2");
    rmdir(root);
}

static void test_the_exact_pass_prefers_a_matching_file_name(void)
{
    sg_diff_list l;
    const sg_diff_entry *e;

    /* Two sources hold identical content, so either could be the source of
       the rename and content cannot break the tie. git breaks it on the file
       name, NOT on path order -- measured against git 2.55.0: with a/g.txt
       and b/f.txt both matching c/f.txt, the rename comes from b/f.txt even
       though a/g.txt is the one it would meet first.

       No files are needed: the exact pass compares ids and nothing else. */
    memset(&l, 0, sizeof(l));
    add_deletion(&l, "a/g.txt", 0xAA);
    add_deletion(&l, "b/f.txt", 0xAA);
    add_addition(&l, "c/f.txt", 0xAA);

    CHECK(sg_diff_detect_renames("/nonexistent", "/nonexistent", &l,
                                 SG_SIMILARITY_DEFAULT) == 0,
          "detection succeeds");
    CHECK(l.count == 2, "one rename plus the unused source, got %zu rows", l.count);
    e = find(&l, "c/f.txt");
    CHECK(e != NULL && e->old_path != NULL && strcmp(e->old_path, "b/f.txt") == 0,
          "the source sharing the file name wins, not the first one in path order");
    CHECK(find(&l, "a/g.txt") != NULL, "the other source stays an ordinary deletion");

    sg_diff_list_free(&l);
}

static void test_only_the_best_four_sources_per_destination_survive(void)
{
    char tmpl[] = "/tmp/sg_rename_four_XXXXXX";
    char *root = scratch_dir(tmpl);
    sg_diff_list l;
    const sg_diff_entry *e;

    /* git ranks at most FOUR sources per destination, evicting the worst as
       better ones arrive, and then sorts that table with a STABLE sort. So
       when two sources tie exactly, which one wins is decided by the SLOT a
       candidate happened to be written into -- not by the order the
       candidates were considered in. The two orders come apart precisely
       when an eviction has moved a candidate to an earlier slot.

       Five sources scoring 50, 60, 89, 80 and 89 do exactly that: the fifth
       evicts the 50 out of slot one, landing ahead of the equally-scoring
       third. Measured against git 2.55.0, the rename comes from s5.txt.
       Ranking by the order the candidates were seen instead would answer
       s3.txt -- an answer nothing else in this suite can tell apart. */
    write_lines(root, "p.txt", 80);
    write_padded(root, "s1.txt", 1, 782);
    write_padded(root, "s2.txt", 2, 652);
    write_padded(root, "s3.txt", 3, 434);
    write_padded(root, "s4.txt", 4, 489);
    write_padded(root, "s5.txt", 5, 434); /* the same size as s3, so the same score */

    memset(&l, 0, sizeof(l));
    add_addition(&l, "p.txt", 0xB0);
    add_deletion(&l, "s1.txt", 0xA1);
    add_deletion(&l, "s2.txt", 0xA2);
    add_deletion(&l, "s3.txt", 0xA3);
    add_deletion(&l, "s4.txt", 0xA4);
    add_deletion(&l, "s5.txt", 0xA5);

    CHECK(sg_diff_detect_renames("/nonexistent", root, &l, SG_SIMILARITY_DEFAULT) == 0,
          "detection succeeds");
    CHECK(l.count == 5, "one rename and four leftover deletions, got %zu rows", l.count);
    e = find(&l, "p.txt");
    CHECK(e != NULL && e->old_path != NULL && strcmp(e->old_path, "s5.txt") == 0,
          "the tie goes to s5.txt, got %s",
          e != NULL && e->old_path != NULL ? e->old_path : "(no rename)");
    CHECK(e != NULL && e->score == 89, "and it scores 89, got %d",
          e != NULL ? e->score : -1);

    sg_diff_list_free(&l);
    remove_file(root, "p.txt");
    remove_file(root, "s1.txt");
    remove_file(root, "s2.txt");
    remove_file(root, "s3.txt");
    remove_file(root, "s4.txt");
    remove_file(root, "s5.txt");
    rmdir(root);
}

static void test_the_matrix_breaks_a_tie_on_the_file_name(void)
{
    char tmpl[] = "/tmp/sg_rename_namescore_XXXXXX";
    char *root = scratch_dir(tmpl);
    sg_diff_list l;
    const sg_diff_entry *e;

    /* Two sources score exactly the same against the destination, so only
       the second key in git's ranking can separate them: whether the source
       shares the destination's FILE NAME. b/x.txt does and a/y.txt does not,
       and b/x.txt sorts later -- so if the name were not consulted, the
       earlier one would win instead.

       The score is deliberately 59%: over the 50% an ordinary pair needs, but
       under the 75% the name shortcut demands, so this pair reaches the full
       comparison rather than being settled by name beforehand. Measured
       against git 2.55.0, which answers b/x.txt. */
    make_subdir(root, "a");
    make_subdir(root, "b");
    make_subdir(root, "c");
    write_padded(root, "a/y.txt", 1, 652);
    write_padded(root, "b/x.txt", 2, 652);
    write_lines(root, "c/x.txt", 80);

    memset(&l, 0, sizeof(l));
    add_deletion(&l, "a/y.txt", 0xA1);
    add_deletion(&l, "b/x.txt", 0xA2);
    add_addition(&l, "c/x.txt", 0xB0);

    CHECK(sg_diff_detect_renames("/nonexistent", root, &l, SG_SIMILARITY_DEFAULT) == 0,
          "detection succeeds");
    CHECK(l.count == 2, "one rename and one leftover deletion, got %zu rows", l.count);
    e = find(&l, "c/x.txt");
    CHECK(e != NULL && e->old_path != NULL && strcmp(e->old_path, "b/x.txt") == 0,
          "the equal-scoring source with the matching name wins, got %s",
          e != NULL && e->old_path != NULL ? e->old_path : "(no rename)");
    CHECK(e != NULL && e->score == 59, "and it scores 59, got %d",
          e != NULL ? e->score : -1);

    sg_diff_list_free(&l);
    remove_file(root, "a/y.txt");
    remove_file(root, "b/x.txt");
    remove_file(root, "c/x.txt");
    remove_subdir(root, "a");
    remove_subdir(root, "b");
    remove_subdir(root, "c");
    rmdir(root);
}

static void test_only_a_hundred_identical_sources_are_considered(void)
{
    sg_diff_list l;
    const sg_diff_entry *e;
    char path[32];
    int i;

    /* git stops after looking at a hundred sources holding identical content
       and settles for the best it has seen, rather than searching on for a
       better tie-break. Here the source that shares the destination's file
       name is the hundred-and-first, so the cap is the only thing standing
       between "s001.txt" and "s101/target.txt" -- and measured against git
       2.55.0, the answer is s001.txt.

       This also pins down the ORDER those sources are examined in, which is
       an assumption the port makes about a hashmap walk inside git and could
       not otherwise be checked: any order but ascending would surface a
       different one of the hundred. */
    memset(&l, 0, sizeof(l));
    for (i = 1; i <= 100; i++) {
        snprintf(path, sizeof(path), "s%03d.txt", i);
        add_deletion(&l, path, 0xAA);
    }
    add_deletion(&l, "s101/target.txt", 0xAA);
    add_addition(&l, "z/target.txt", 0xAA);

    CHECK(sg_diff_detect_renames("/nonexistent", "/nonexistent", &l,
                                 SG_SIMILARITY_DEFAULT) == 0,
          "detection succeeds");
    CHECK(l.count == 101, "one rename and a hundred leftover deletions, got %zu",
          l.count);
    e = find(&l, "z/target.txt");
    CHECK(e != NULL && e->old_path != NULL && strcmp(e->old_path, "s001.txt") == 0,
          "the search stops before reaching the matching file name, got %s",
          e != NULL && e->old_path != NULL ? e->old_path : "(no rename)");

    sg_diff_list_free(&l);
}

static void test_an_exact_tie_keeps_the_source_it_already_had(void)
{
    char tmpl[] = "/tmp/sg_rename_tie6_XXXXXX";
    char *root = scratch_dir(tmpl);
    sg_diff_list l;
    const sg_diff_entry *e;
    char rel[32];
    int i;

    /* Six sources scoring identically, which is more than the four the
       ranking holds, so two of them meet a full table of exact ties. A tie
       must NOT displace what is already there: the table keeps the first
       source it saw and the last two are dropped. Replacing on a tie instead
       would leave the SIXTH in the table and hand it the rename -- measured
       against git 2.55.0, which answers t1.txt.

       Equal scores are arranged by giving every source the same size and the
       same shared lines, differing only in padding. */
    for (i = 1; i <= 6; i++) {
        snprintf(rel, sizeof(rel), "t%d.txt", i);
        write_padded(root, rel, i, 652);
    }
    write_lines(root, "p.txt", 80);

    memset(&l, 0, sizeof(l));
    add_addition(&l, "p.txt", 0xB0);
    for (i = 1; i <= 6; i++) {
        snprintf(rel, sizeof(rel), "t%d.txt", i);
        add_deletion(&l, rel, (unsigned char)(0xA0 + i));
    }

    CHECK(sg_diff_detect_renames("/nonexistent", root, &l, SG_SIMILARITY_DEFAULT) == 0,
          "detection succeeds");
    CHECK(l.count == 6, "one rename and five leftover deletions, got %zu rows", l.count);
    e = find(&l, "p.txt");
    CHECK(e != NULL && e->old_path != NULL && strcmp(e->old_path, "t1.txt") == 0,
          "the first equal-scoring source keeps the place, got %s",
          e != NULL && e->old_path != NULL ? e->old_path : "(no rename)");

    sg_diff_list_free(&l);
    remove_file(root, "p.txt");
    for (i = 1; i <= 6; i++) {
        snprintf(rel, sizeof(rel), "t%d.txt", i);
        remove_file(root, rel);
    }
    rmdir(root);
}

static void test_a_repeated_file_name_declines_the_shortcut(void)
{
    char tmpl[] = "/tmp/sg_rename_dupname_XXXXXX";
    char *root = scratch_dir(tmpl);
    sg_diff_list l;
    const sg_diff_entry *e;

    /* The name shortcut only fires when the name identifies ONE file on each
       side. Here two sources are called x.txt, so git refuses to guess
       between them and lets the full comparison decide -- which picks
       b/x.txt at 98% over a/x.txt at 80%.

       a/x.txt is deliberately above the 75% the shortcut demands, so
       accepting the first of the two repeated names instead of declining
       would pair it and answer 80. Measured against git 2.55.0, which
       answers b/x.txt. */
    make_subdir(root, "a");
    make_subdir(root, "b");
    make_subdir(root, "c");
    write_lines(root, "a/x.txt", 80);
    write_lines(root, "b/x.txt", 100);
    write_lines(root, "c/x.txt", 99);

    memset(&l, 0, sizeof(l));
    add_deletion(&l, "a/x.txt", 0xA1);
    add_deletion(&l, "b/x.txt", 0xA2);
    add_addition(&l, "c/x.txt", 0xB0);

    CHECK(sg_diff_detect_renames("/nonexistent", root, &l, SG_SIMILARITY_DEFAULT) == 0,
          "detection succeeds");
    CHECK(l.count == 2, "one rename and one leftover deletion, got %zu rows", l.count);
    e = find(&l, "c/x.txt");
    CHECK(e != NULL && e->old_path != NULL && strcmp(e->old_path, "b/x.txt") == 0,
          "the better match wins because the repeated name decided nothing, got %s",
          e != NULL && e->old_path != NULL ? e->old_path : "(no rename)");
    CHECK(e != NULL && e->score == 98, "and it scores 98, got %d",
          e != NULL ? e->score : -1);

    sg_diff_list_free(&l);
    remove_file(root, "a/x.txt");
    remove_file(root, "b/x.txt");
    remove_file(root, "c/x.txt");
    remove_subdir(root, "a");
    remove_subdir(root, "b");
    remove_subdir(root, "c");
    rmdir(root);
}

static void test_a_side_that_cannot_be_read_is_never_paired(void)
{
    char tmpl[] = "/tmp/sg_rename_unreadable_XXXXXX";
    char *root = scratch_dir(tmpl);
    sg_diff_list l;

    /* The control group first: with both files present this pair IS a
       rename, so the check below can only be about the missing bytes and not
       about the content being too different. */
    write_lines(root, "old.txt", 100);
    write_lines(root, "new.txt", 80);
    memset(&l, 0, sizeof(l));
    add_addition(&l, "new.txt", 0xBB);
    add_deletion(&l, "old.txt", 0xAA);
    CHECK(sg_diff_detect_renames("/nonexistent", root, &l, SG_SIMILARITY_DEFAULT) == 0,
          "detection succeeds");
    CHECK(l.count == 1, "control: with both files readable this is a rename");
    sg_diff_list_free(&l);

    remove_file(root, "old.txt");
    memset(&l, 0, sizeof(l));
    add_addition(&l, "new.txt", 0xBB);
    add_deletion(&l, "old.txt", 0xAA);
    CHECK(sg_diff_detect_renames("/nonexistent", root, &l, SG_SIMILARITY_DEFAULT) == 0,
          "detection still succeeds with the source gone");
    CHECK(l.count == 2, "an unreadable side is never paired");
    sg_diff_list_free(&l);

    remove_file(root, "new.txt");
    rmdir(root);
}

int main(void)
{
    test_exact_rename_becomes_one_row();
    test_different_content_is_not_a_rename();
    test_pairs_in_path_order();
    test_a_source_is_used_once();
    test_modifications_and_unmerged_never_pair();
    test_zero_score_disables();
    test_list_stays_sorted();
    test_empty_and_single_lists();
    test_unverified_ids_are_never_paired();
    test_score_is_zero_padded_to_three_digits();
    test_rename_reads_its_old_side_from_the_old_path();
    test_inexact_rename_carries_gits_score();
    test_the_threshold_is_a_real_comparison();
    test_exact_only_threshold_still_finds_exact_renames();
    test_the_file_name_beats_a_better_score();
    test_a_name_match_below_the_raised_threshold_loses();
    test_the_exact_pass_prefers_a_matching_file_name();
    test_only_the_best_four_sources_per_destination_survive();
    test_the_matrix_breaks_a_tie_on_the_file_name();
    test_only_a_hundred_identical_sources_are_considered();
    test_an_exact_tie_keeps_the_source_it_already_had();
    test_a_repeated_file_name_declines_the_shortcut();
    test_a_side_that_cannot_be_read_is_never_paired();

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_rename: all checks passed\n");
    return 0;
}
