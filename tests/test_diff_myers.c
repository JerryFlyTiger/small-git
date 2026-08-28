/* Phase 35: sg_diff_build_script had no direct test before this file (the
   old LCS-backtrack alignment it replaced was only exercised indirectly,
   through tests/test_diff_out.c's CLI-level checks). This file targets the
   Myers port itself: divide-and-conquer/snake basic shapes, xdl_trim_ends'
   boundaries, and -- the easiest place to get this port wrong, per the
   Phase 35 spec -- coordinate mapping back to original line numbers. That
   last category has TWO distinct paths, each with its own dedicated test
   below (confirmed via tests/mutate.sh that each only catches its own
   path's bug, not the other's): Myers' own aref/bref translation out of
   its compacted index space (test_myers_aref_mapping_second_dup_deleted /
   test_myers_bref_mapping_second_dup_inserted), and xdl_cleanup_records'
   direct changed[i + off] write for a line DISCARDed before Myers ever
   runs (test_discard_path_coordinate_mapping). */

#include "sg/diff_lcs.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, ...)                                                                       \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);                                \
            fprintf(stderr, __VA_ARGS__);                                                       \
            fprintf(stderr, "\n");                                                              \
            failures++;                                                                         \
        }                                                                                        \
    } while (0)

/* Builds an sg_diff_line array from a NULL-terminated list of C strings
   (borrowed pointers, has_nl always 1 -- none of these tests care about
   the has_nl distinction). */
static size_t lines_from(sg_diff_line *out, const char *const *strs)
{
    size_t n = 0;

    while (strs[n] != NULL) {
        out[n].ptr = strs[n];
        out[n].len = strlen(strs[n]);
        out[n].has_nl = 1;
        n++;
    }
    return n;
}

static void check_group_contract(const sg_diff_script *script, const char *ctx)
{
    size_t i;

    for (i = 0; i < script->count; i++) {
        const sg_diff_group *g = &script->groups[i];

        CHECK(g->a_len != 0 || g->b_len != 0, "%s: group %zu has both a_len and b_len 0", ctx, i);
        if (i > 0) {
            const sg_diff_group *prev = &script->groups[i - 1];

            CHECK(g->a_off >= prev->a_off + prev->a_len, "%s: group %zu not ascending/non-overlapping on a_off",
                 ctx, i);
            CHECK(g->b_off >= prev->b_off + prev->b_len, "%s: group %zu not ascending/non-overlapping on b_off",
                 ctx, i);
        }
    }
}

static void expect_one_group(const sg_diff_script *script, const char *ctx, size_t a_off, size_t a_len,
                             size_t b_off, size_t b_len)
{
    CHECK(script->count == 1, "%s: expected 1 group, got %zu", ctx, script->count);
    if (script->count != 1)
        return;
    CHECK(script->groups[0].a_off == a_off && script->groups[0].a_len == a_len &&
             script->groups[0].b_off == b_off && script->groups[0].b_len == b_len,
         "%s: expected {a_off=%zu a_len=%zu b_off=%zu b_len=%zu}, got {a_off=%zu a_len=%zu b_off=%zu b_len=%zu}",
         ctx, a_off, a_len, b_off, b_len, script->groups[0].a_off, script->groups[0].a_len,
         script->groups[0].b_off, script->groups[0].b_len);
}

/* ---- divide-and-conquer / snake basic shapes ---- */

static void test_pure_addition(void)
{
    sg_diff_line b[8];
    const char *bs[] = {"x", "y", NULL};
    size_t nb = lines_from(b, bs);
    sg_diff_script *script = sg_diff_build_script(NULL, 0, b, nb, 0);

    CHECK(script != NULL, "build_script returned NULL");
    if (script == NULL)
        return;
    expect_one_group(script, "pure_addition", 0, 0, 0, 2);
    check_group_contract(script, "pure_addition");
    sg_diff_script_free(script);
}

static void test_pure_deletion(void)
{
    sg_diff_line a[8];
    const char *as[] = {"x", "y", NULL};
    size_t na = lines_from(a, as);
    sg_diff_script *script = sg_diff_build_script(a, na, NULL, 0, 0);

    CHECK(script != NULL, "build_script returned NULL");
    if (script == NULL)
        return;
    expect_one_group(script, "pure_deletion", 0, 2, 0, 0);
    check_group_contract(script, "pure_deletion");
    sg_diff_script_free(script);
}

static void test_replace(void)
{
    sg_diff_line a[8], b[8];
    const char *as[] = {"x", NULL};
    const char *bs[] = {"y", NULL};
    size_t na = lines_from(a, as);
    size_t nb = lines_from(b, bs);
    sg_diff_script *script = sg_diff_build_script(a, na, b, nb, 0);

    CHECK(script != NULL, "build_script returned NULL");
    if (script == NULL)
        return;
    expect_one_group(script, "replace", 0, 1, 0, 1);
    check_group_contract(script, "replace");
    sg_diff_script_free(script);
}

static void test_head_and_tail_all_same(void)
{
    sg_diff_line a[8], b[8];
    const char *as[] = {"x", "y", "z", NULL};
    const char *bs[] = {"x", "y", "z", NULL};
    size_t na = lines_from(a, as);
    size_t nb = lines_from(b, bs);
    sg_diff_script *script = sg_diff_build_script(a, na, b, nb, 0);

    CHECK(script != NULL, "build_script returned NULL");
    if (script == NULL)
        return;
    CHECK(script->count == 0, "head_and_tail_all_same: expected 0 groups, got %zu", script->count);
    check_group_contract(script, "head_and_tail_all_same");
    sg_diff_script_free(script);
}

/* ---- xdl_trim_ends boundaries ---- */

static void test_trim_both_empty(void)
{
    sg_diff_script *script = sg_diff_build_script(NULL, 0, NULL, 0, 0);

    CHECK(script != NULL, "build_script returned NULL");
    if (script == NULL)
        return;
    CHECK(script->count == 0, "trim_both_empty: expected 0 groups, got %zu", script->count);
    sg_diff_script_free(script);
}

static void test_trim_only_start_differs(void)
{
    sg_diff_line a[8], b[8];
    const char *as[] = {"A", "x", "y", NULL};
    const char *bs[] = {"B", "x", "y", NULL};
    size_t na = lines_from(a, as);
    size_t nb = lines_from(b, bs);
    sg_diff_script *script = sg_diff_build_script(a, na, b, nb, 0);

    CHECK(script != NULL, "build_script returned NULL");
    if (script == NULL)
        return;
    expect_one_group(script, "trim_only_start_differs", 0, 1, 0, 1);
    check_group_contract(script, "trim_only_start_differs");
    sg_diff_script_free(script);
}

static void test_trim_only_end_differs(void)
{
    sg_diff_line a[8], b[8];
    const char *as[] = {"x", "y", "A", NULL};
    const char *bs[] = {"x", "y", "B", NULL};
    size_t na = lines_from(a, as);
    size_t nb = lines_from(b, bs);
    sg_diff_script *script = sg_diff_build_script(a, na, b, nb, 0);

    CHECK(script != NULL, "build_script returned NULL");
    if (script == NULL)
        return;
    expect_one_group(script, "trim_only_end_differs", 2, 1, 2, 1);
    check_group_contract(script, "trim_only_end_differs");
    sg_diff_script_free(script);
}

/* ---- coordinate mapping, KEEP/aref path: Myers itself runs in the
   COMPACTED index space xdl_cleanup_records built (only KEEP-classified
   lines get a reference_index/aref/bref entry -- see cleanup_side), so
   every changed[] write the Myers core does (myers_recs_cmp's
   off1==lim1/off2==lim2 branches) must translate that compacted index back
   through aref/bref to the ORIGINAL index. These two tests are named and
   scoped for exactly that: mutating `mc->achanged[mc->aref[off1]]` down to
   bare `mc->achanged[off1]` (confirmed with tests/mutate.sh) turns them
   red, not the DISCARD-path tests below.

   IMPORTANT, checked with tests/mutate.sh: these two do NOT exercise the
   DISCARD path (xdl_cleanup_records writing changed[i+off] directly,
   before Myers ever runs). Both fixtures' repeated "m" line has a nonzero
   match count in the other file (nm=1 < mlim), so cleanup_side classifies
   every "m" occurrence KEEP, never DISCARD -- mutating
   `changed[i + off] = 1` down to `changed[i] = 1` (the DISCARD-path bug)
   leaves both of these tests green; see
   test_discard_path_coordinate_mapping below for that path's dedicated
   witness.

   a = { pre, m, m, post }, b = { pre, m, post } -- verified against real
   git (`git diff --no-index` on these exact four/three lines): the SECOND
   "m" (original index 2 in a) is the one deleted, not the first. This is
   exactly the off2==lim2 branch of myers_recs_cmp firing after eating one
   matching snake, so aref[1] must resolve to original index 2, not 1. */
static void test_myers_aref_mapping_second_dup_deleted(void)
{
    sg_diff_line a[8], b[8];
    const char *as[] = {"pre", "m", "m", "post", NULL};
    const char *bs[] = {"pre", "m", "post", NULL};
    size_t na = lines_from(a, as);
    size_t nb = lines_from(b, bs);
    sg_diff_script *script = sg_diff_build_script(a, na, b, nb, 0);

    CHECK(script != NULL, "build_script returned NULL");
    if (script == NULL)
        return;
    expect_one_group(script, "myers_aref_mapping_second_dup_deleted", 2, 1, 2, 0);
    check_group_contract(script, "myers_aref_mapping_second_dup_deleted");
    sg_diff_script_free(script);
}

/* Mirror image: the extra duplicate is on the b side instead of a, so the
   coordinate mapping being checked is bref[] instead of aref[] (the
   off1==lim1 branch instead of off2==lim2). Verified against real git the
   same way: `git diff --no-index` on b={pre,m,post} -> a={pre,m,m,post}
   inserts the second "m" right after the first, at original b-index 2.
   Same DISCARD-path caveat as the sibling test above applies here too. */
static void test_myers_bref_mapping_second_dup_inserted(void)
{
    sg_diff_line a[8], b[8];
    const char *as[] = {"pre", "m", "post", NULL};
    const char *bs[] = {"pre", "m", "m", "post", NULL};
    size_t na = lines_from(a, as);
    size_t nb = lines_from(b, bs);
    sg_diff_script *script = sg_diff_build_script(a, na, b, nb, 0);

    CHECK(script != NULL, "build_script returned NULL");
    if (script == NULL)
        return;
    expect_one_group(script, "myers_bref_mapping_second_dup_inserted", 2, 0, 2, 1);
    check_group_contract(script, "myers_bref_mapping_second_dup_inserted");
    sg_diff_script_free(script);
}

/* ---- coordinate mapping, DISCARD path: cleanup_side classifies a line
   DISCARD (nm == 0, no match at all in the other file) and writes
   changed[i + off] = 1 directly, before Myers ever runs -- off must be
   xdl_trim_ends' dstart, not 0, or this write lands on the wrong line
   whenever the common prefix is non-empty.

   a = { ctx1, ctx2, UNIQUE_A, ctx3, ctx4 }
   b = { ctx1, ctx2, UNIQUE_B, ctx3, ctx4 }

   Common prefix ctx1/ctx2 trims dstart to 2; common suffix ctx3/ctx4 trims
   the back. UNIQUE_A/UNIQUE_B each occur zero times in the other file, so
   both are DISCARDed straight away (mlim from bogosqrt is never even
   consulted) -- Myers itself runs on an empty middle and does nothing.
   Unambiguous single-line replace, no oracle needed: with a `dstart=2`
   offset, `changed[i + off]` written as `changed[i]` would mark ctx2
   changed instead of UNIQUE_A/UNIQUE_B, which check_group_contract's
   ascending/off-by-a-lot assertion below would also catch, but
   expect_one_group's exact-offset check is what actually pins it down.

   NOTE, checked with tests/mutate.sh: test_trim_only_end_differs
   (dstart=2 there too, by coincidence of that fixture's own common prefix)
   INCIDENTALLY also turns red under the same changed[i+off]->changed[i]
   mutation -- it was written to pin xdl_trim_ends' boundary, not this
   path, so treat that as a lucky second witness, not the primary one.
   test_trim_only_start_differs (dstart=0 there) CANNOT distinguish this
   bug at all, since i+off == i when off is 0. */
static void test_discard_path_coordinate_mapping(void)
{
    sg_diff_line a[8], b[8];
    const char *as[] = {"ctx1", "ctx2", "UNIQUE_A", "ctx3", "ctx4", NULL};
    const char *bs[] = {"ctx1", "ctx2", "UNIQUE_B", "ctx3", "ctx4", NULL};
    size_t na = lines_from(a, as);
    size_t nb = lines_from(b, bs);
    sg_diff_script *script = sg_diff_build_script(a, na, b, nb, 0);

    CHECK(script != NULL, "build_script returned NULL");
    if (script == NULL)
        return;
    expect_one_group(script, "discard_path_coordinate_mapping", 2, 1, 2, 1);
    check_group_contract(script, "discard_path_coordinate_mapping");
    sg_diff_script_free(script);
}

/* ---- group contract on a multi-hunk case (ascending, non-overlapping,
   never both-zero) ---- */

static void test_multi_hunk_group_contract(void)
{
    sg_diff_line a[16], b[16];
    const char *as[] = {"1", "2", "DEL", "3", "4", "OLD", "5", "6", NULL};
    const char *bs[] = {"1", "2", "3", "4", "NEW", "5", "6", "ADDED", NULL};
    size_t na = lines_from(a, as);
    size_t nb = lines_from(b, bs);
    sg_diff_script *script = sg_diff_build_script(a, na, b, nb, 0);

    CHECK(script != NULL, "build_script returned NULL");
    if (script == NULL)
        return;
    CHECK(script->count >= 2, "multi_hunk_group_contract: expected at least 2 groups, got %zu", script->count);
    check_group_contract(script, "multi_hunk_group_contract");
    sg_diff_script_free(script);
}

int main(void)
{
    test_pure_addition();
    test_pure_deletion();
    test_replace();
    test_head_and_tail_all_same();
    test_trim_both_empty();
    test_trim_only_start_differs();
    test_trim_only_end_differs();
    test_myers_aref_mapping_second_dup_deleted();
    test_myers_bref_mapping_second_dup_inserted();
    test_discard_path_coordinate_mapping();
    test_multi_hunk_group_contract();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all diff_myers tests passed\n");
    return 0;
}
