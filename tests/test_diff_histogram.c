/* The alignment algorithm is a parameter of sg_diff_build_script (Phase 42),
   and these pin that the parameter is actually honoured -- not merely
   accepted -- by checking the two algorithms produce the two DIFFERENT edit
   scripts real git produces for the same input.

   Both expectations were measured, not derived:

       git diff --diff-algorithm=myers        -> " P / -R / P / +R"
       git diff --diff-algorithm=histogram    -> "+P /  P /  R / -P"

   on a = [P, R, P], b = [P, P, R] with P = "_helper = None" and
   R = "        return None". The two are equal-cost alignments of the same
   three lines, which is exactly why the algorithm choice is observable here
   and invisible on most inputs. */

#include "sg/diff_lcs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, ...)                                                                         \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);                                 \
            fprintf(stderr, __VA_ARGS__);                                                        \
            fprintf(stderr, "\n");                                                               \
            failures++;                                                                          \
        }                                                                                        \
    } while (0)

static const unsigned char A_DATA[] = "_helper = None\n        return None\n_helper = None\n";
static const unsigned char B_DATA[] = "_helper = None\n_helper = None\n        return None\n";

/* Prints a script as "a_off,a_len,b_off,b_len;..." so a failure message names
   the actual alignment instead of just "not equal". */
static void describe(const sg_diff_script *s, char *buf, size_t cap)
{
    size_t i, n = 0;

    buf[0] = '\0';
    for (i = 0; i < s->count && n + 32 < cap; i++)
        n += (size_t)snprintf(buf + n, cap - n, "%zu,%zu,%zu,%zu;", s->groups[i].a_off,
                             s->groups[i].a_len, s->groups[i].b_off, s->groups[i].b_len);
}

static sg_diff_script *build(sg_diff_algorithm algo, sg_diff_line **a_out, sg_diff_line **b_out)
{
    size_t na = 0, nb = 0;
    sg_diff_line *a = sg_diff_split_lines(A_DATA, sizeof(A_DATA) - 1, &na);
    sg_diff_line *b = sg_diff_split_lines(B_DATA, sizeof(B_DATA) - 1, &nb);
    sg_diff_script *s;

    if (a == NULL || b == NULL) {
        fprintf(stderr, "out of memory splitting lines\n");
        exit(1);
    }
    s = sg_diff_build_script(a, na, b, nb, 1, algo);
    *a_out = a;
    *b_out = b;
    return s;
}

static void test_myers_alignment(void)
{
    sg_diff_line *a, *b;
    sg_diff_script *s = build(SG_DIFF_ALGO_MYERS, &a, &b);
    char got[256];

    CHECK(s != NULL, "myers script should build");
    if (s == NULL)
        return;
    describe(s, got, sizeof(got));
    /* delete a[1], insert b[2] -- git's Myers answer. */
    CHECK(strcmp(got, "1,1,1,0;3,0,2,1;") == 0, "myers alignment is '%s'", got);
    sg_diff_script_free(s);
    free(a);
    free(b);
}

static void test_histogram_alignment(void)
{
    sg_diff_line *a, *b;
    sg_diff_script *s = build(SG_DIFF_ALGO_HISTOGRAM, &a, &b);
    char got[256];

    CHECK(s != NULL, "histogram script should build");
    if (s == NULL)
        return;
    describe(s, got, sizeof(got));
    /* insert b[0] at the top, delete a[2] at the end -- git's histogram
       answer, and NOT the Myers one above. A mutation that ignores the
       algorithm parameter makes this string equal the other test's. */
    CHECK(strcmp(got, "0,0,0,1;2,1,3,0;") == 0, "histogram alignment is '%s'", got);
    sg_diff_script_free(s);
    free(a);
    free(b);
}

/* Identical inputs must produce an empty script under BOTH algorithms: the
   histogram path has its own trim-free entry and its own recursion, so
   "nothing changed" is not shared code with Myers and needs its own check. */
static void test_identical_inputs(void)
{
    size_t na = 0, nb = 0;
    sg_diff_line *a = sg_diff_split_lines(A_DATA, sizeof(A_DATA) - 1, &na);
    sg_diff_line *b = sg_diff_split_lines(A_DATA, sizeof(A_DATA) - 1, &nb);
    sg_diff_script *m, *h;

    if (a == NULL || b == NULL)
        exit(1);
    m = sg_diff_build_script(a, na, b, nb, 1, SG_DIFF_ALGO_MYERS);
    h = sg_diff_build_script(a, na, b, nb, 1, SG_DIFF_ALGO_HISTOGRAM);
    CHECK(m != NULL && m->count == 0, "myers on identical input should be empty");
    CHECK(h != NULL && h->count == 0, "histogram on identical input should be empty");
    sg_diff_script_free(m);
    sg_diff_script_free(h);
    free(a);
    free(b);
}

/* One side empty is its own branch in the histogram recursion (count1 == 0 /
   count2 == 0), reached before any indexing happens. */
static void test_one_side_empty(void)
{
    size_t na = 0, nb = 0;
    sg_diff_line *a = sg_diff_split_lines(A_DATA, sizeof(A_DATA) - 1, &na);
    sg_diff_line *b = sg_diff_split_lines((const unsigned char *)"", 0, &nb);
    sg_diff_script *h;

    if (a == NULL)
        exit(1);
    h = sg_diff_build_script(a, na, b, nb, 1, SG_DIFF_ALGO_HISTOGRAM);
    CHECK(h != NULL && h->count == 1, "histogram: one group for a wholly deleted file");
    if (h != NULL && h->count == 1)
        CHECK(h->groups[0].a_off == 0 && h->groups[0].a_len == 3 && h->groups[0].b_len == 0,
             "histogram: the group should cover all of a");
    sg_diff_script_free(h);
    free(a);
    free(b);
}

/* scanA walks its region BACKWARDS so that each line's record points at its
   FIRST occurrence and the chain runs downward; try_lcs's `np` walk depends on
   that direction. Reversing it changes the answer here (measured: real git
   deletes the second "cc" and the blank, sg agrees), and this fixture exists
   because the alignment fixtures above do NOT notice the reversal -- a
   mutation flipping the loop left them green. Coverage of a direction needs a
   line that occurs more than twice with the occurrences NOT interchangeable. */
static void test_scan_direction_is_observable(void)
{
    static const unsigned char a_data[] = "cc\ncc\n\ncc\n";
    static const unsigned char b_data[] = "cc\naa\ncc\n";
    size_t na = 0, nb = 0;
    sg_diff_line *a = sg_diff_split_lines(a_data, sizeof(a_data) - 1, &na);
    sg_diff_line *b = sg_diff_split_lines(b_data, sizeof(b_data) - 1, &nb);
    sg_diff_script *s;
    char got[256];

    if (a == NULL || b == NULL)
        exit(1);
    s = sg_diff_build_script(a, na, b, nb, 1, SG_DIFF_ALGO_HISTOGRAM);
    CHECK(s != NULL, "script should build");
    if (s != NULL) {
        describe(s, got, sizeof(got));
        CHECK(strcmp(got, "1,2,1,1;") == 0, "histogram alignment is '%s'", got);
        sg_diff_script_free(s);
    }
    free(a);
    free(b);
}

/* Phase 52: compact_one_side's histogram-only rerun (git's
   xdl_change_compact, xdiffi.c:940-958). After sliding a changed group, if
   its position or size moved AND the opposite group is non-empty, git reruns
   Myers on just that pair of groups so newly-revealed matching lines fall
   back to unchanged. Without this rerun sg's histogram output disagreed with
   git's on this exact fixture (measured: sg gave a single 2-line replacement
   covering both blank-line slots, git gives a single 1-line deletion).
   old = "R\n\nR\n\n" (4 lines: R, blank, R, blank)
   new = "R\nR\n\n" (3 lines: R, R, blank)
   git's answer (both --histogram and --diff-algorithm=myers, measured with
   `git diff --no-index`): delete the first blank line only. */
static const unsigned char RECOMPACT_A[] = "R\n\nR\n\n";
static const unsigned char RECOMPACT_B[] = "R\nR\n\n";

static void test_histogram_recompact_rerun(void)
{
    size_t na = 0, nb = 0;
    sg_diff_line *a = sg_diff_split_lines(RECOMPACT_A, sizeof(RECOMPACT_A) - 1, &na);
    sg_diff_line *b = sg_diff_split_lines(RECOMPACT_B, sizeof(RECOMPACT_B) - 1, &nb);
    sg_diff_script *s;
    char got[256];

    if (a == NULL || b == NULL)
        exit(1);
    s = sg_diff_build_script(a, na, b, nb, 1, SG_DIFF_ALGO_HISTOGRAM);
    CHECK(s != NULL, "histogram script should build");
    if (s != NULL) {
        describe(s, got, sizeof(got));
        /* delete a[1] (the first blank line) only -- the recompact rerun
           output. A mutation removing the rerun gives "0,2,0,1;" instead,
           a single group covering both blank-line slots. */
        CHECK(strcmp(got, "1,1,1,0;") == 0, "histogram recompact alignment is '%s'", got);
        sg_diff_script_free(s);
    }
    free(a);
    free(b);
}

/* The recompact rerun is gated on `histogram` (compact_one_side's own
   parameter): Myers must produce the exact same, unaffected answer on this
   fixture, since a mutation that runs the rerun unconditionally for BOTH
   algorithms would leave this test unable to tell the two apart. */
/* NOT a witness for the `histogram` gate on the recompact rerun, despite how
   the obvious name for it would read. Measured 2026-09-01: with that gate
   removed -- so the rerun applies to Myers too -- this test stays green, and
   so does every other named check, because git's Myers answer on this fixture
   is the same one histogram gives. The gate was classified by fuzzing instead
   (1500 Myers rounds across three seed ranges, 0 mismatches with it removed),
   and is kept for faithfulness to git rather than because anything can see
   it. What this test actually pins is narrower and still worth having: that
   the Myers path returns git's answer on the very fixture the histogram fix
   is built around, so a future change to the shared compaction code cannot
   quietly move Myers while the histogram assertion stays green. */
static void test_myers_answer_on_the_recompact_fixture(void)
{
    size_t na = 0, nb = 0;
    sg_diff_line *a = sg_diff_split_lines(RECOMPACT_A, sizeof(RECOMPACT_A) - 1, &na);
    sg_diff_line *b = sg_diff_split_lines(RECOMPACT_B, sizeof(RECOMPACT_B) - 1, &nb);
    sg_diff_script *s;
    char got[256];

    if (a == NULL || b == NULL)
        exit(1);
    s = sg_diff_build_script(a, na, b, nb, 1, SG_DIFF_ALGO_MYERS);
    CHECK(s != NULL, "myers script should build");
    if (s != NULL) {
        describe(s, got, sizeof(got));
        CHECK(strcmp(got, "1,1,1,0;") == 0, "myers recompact-fixture alignment is '%s'", got);
        sg_diff_script_free(s);
    }
    free(a);
    free(b);
}

int main(void)
{
    test_myers_alignment();
    test_histogram_alignment();
    test_identical_inputs();
    test_one_side_empty();
    test_scan_direction_is_observable();
    test_histogram_recompact_rerun();
    test_myers_answer_on_the_recompact_fixture();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all diff_histogram tests passed\n");
    return 0;
}
