#include "sg/diff.h"

#include "sg/diff_out.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

    CHECK(sg_diff_detect_renames("/nonexistent", &l, 50) == 0, "detection succeeds");
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

    CHECK(sg_diff_detect_renames("/nonexistent", &l, 50) == 0, "detection succeeds");
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

    CHECK(sg_diff_detect_renames("/nonexistent", &l, 50) == 0, "detection succeeds");
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

    CHECK(sg_diff_detect_renames("/nonexistent", &l, 50) == 0, "detection succeeds");
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

    CHECK(sg_diff_detect_renames("/nonexistent", &l, 50) == 0, "detection succeeds");
    CHECK(l.count == 2, "a modification is not a rename source");
    sg_diff_list_free(&l);

    memset(&l, 0, sizeof(l));
    add_deletion(&l, "a.txt", 0xAA);
    add_addition(&l, "b.txt", 0xAA);
    l.entries[0].unmerged = 1;
    CHECK(sg_diff_detect_renames("/nonexistent", &l, 50) == 0, "detection succeeds");
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

    CHECK(sg_diff_detect_renames("/nonexistent", &l, 0) == 0, "detection succeeds");
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

    CHECK(sg_diff_detect_renames("/nonexistent", &l, 50) == 0, "detection succeeds");
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
    CHECK(sg_diff_detect_renames("/nonexistent", &l, 50) == 0, "an empty list is fine");
    CHECK(l.count == 0, "and stays empty");
    CHECK(sg_diff_detect_renames("/nonexistent", NULL, 50) == 0, "so is no list at all");

    add_deletion(&l, "only.txt", 0xAA);
    CHECK(sg_diff_detect_renames("/nonexistent", &l, 50) == 0, "one row is fine");
    CHECK(l.count == 1 && l.entries[0].old_path == NULL, "a lone deletion is not a rename");
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
    test_score_is_zero_padded_to_three_digits();

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_rename: all checks passed\n");
    return 0;
}
