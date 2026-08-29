#include "sg/merge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static const unsigned char BASE[] = "line1\nline2\nline3\nline4\nline5\n";
#define BASE_LEN (sizeof(BASE) - 1)

/* Naive substring search over a non-NUL-terminated haystack, avoiding a
   dependency on the non-standard memmem(). Returns a pointer to the first
   match, or NULL. */
static const char *memmem_str(const char *haystack, size_t haystack_len, const char *needle)
{
    size_t needle_len = strlen(needle);
    size_t i;

    if (needle_len == 0 || needle_len > haystack_len)
        return NULL;
    for (i = 0; i + needle_len <= haystack_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0)
            return haystack + i;
    }
    return NULL;
}

/* (a) only ours changed: clean merge, result is exactly ours */
static void test_only_ours_changed(void)
{
    static const unsigned char ours[] = "line1\nCHANGED2\nline3\nline4\nline5\n";
    unsigned char *out = NULL;
    size_t out_len = 0;
    int rc;

    rc = sg_merge_content(BASE, BASE_LEN, ours, sizeof(ours) - 1, BASE, BASE_LEN, "ours", "theirs",
                          &out, &out_len);
    CHECK(rc == 0, "expected clean merge, got %d", rc);
    CHECK(out != NULL && out_len == sizeof(ours) - 1 && memcmp(out, ours, out_len) == 0,
         "result should equal ours exactly");
    free(out);
}

/* (b) only theirs changed: clean merge, result is exactly theirs */
static void test_only_theirs_changed(void)
{
    static const unsigned char theirs[] = "line1\nCHANGED2\nline3\nline4\nline5\n";
    unsigned char *out = NULL;
    size_t out_len = 0;
    int rc;

    rc = sg_merge_content(BASE, BASE_LEN, BASE, BASE_LEN, theirs, sizeof(theirs) - 1, "ours",
                          "theirs", &out, &out_len);
    CHECK(rc == 0, "expected clean merge, got %d", rc);
    CHECK(out != NULL && out_len == sizeof(theirs) - 1 && memcmp(out, theirs, out_len) == 0,
         "result should equal theirs exactly");
    free(out);
}

/* (c) both sides change different regions: clean auto-merge containing both edits */
static void test_both_changed_different_regions(void)
{
    static const unsigned char ours[] = "line1\nCHANGED2\nline3\nline4\nline5\n";
    static const unsigned char theirs[] = "line1\nline2\nline3\nCHANGED4\nline5\n";
    static const unsigned char expected[] = "line1\nCHANGED2\nline3\nCHANGED4\nline5\n";
    unsigned char *out = NULL;
    size_t out_len = 0;
    int rc;

    rc = sg_merge_content(BASE, BASE_LEN, ours, sizeof(ours) - 1, theirs, sizeof(theirs) - 1,
                          "ours", "theirs", &out, &out_len);
    CHECK(rc == 0, "expected clean auto-merge, got %d", rc);
    CHECK(out != NULL && out_len == sizeof(expected) - 1 && memcmp(out, expected, out_len) == 0,
         "result should contain both edits: got '%.*s'", (int)out_len, (const char *)out);
    free(out);
}

/* (d) both sides change the same region differently: conflict with
   exactly-7-char markers */
static void test_same_region_conflict(void)
{
    static const unsigned char ours[] = "line1\nOURS2\nline3\nline4\nline5\n";
    static const unsigned char theirs[] = "line1\nTHEIRS2\nline3\nline4\nline5\n";
    unsigned char *out = NULL;
    size_t out_len = 0;
    int rc;
    const char *s;

    rc = sg_merge_content(BASE, BASE_LEN, ours, sizeof(ours) - 1, theirs, sizeof(theirs) - 1,
                          "ours-branch", "theirs-branch", &out, &out_len);
    CHECK(rc == 1, "expected a conflict, got %d", rc);
    CHECK(out != NULL, "conflict output should not be NULL");
    if (out == NULL)
        return;

    s = (const char *)out;
    CHECK(memmem_str(s, out_len, "<<<<<<< ours-branch\n") != NULL,
         "missing/incorrect ours conflict marker: '%.*s'", (int)out_len, s);
    CHECK(memmem_str(s, out_len, "OURS2\n") != NULL, "missing ours content in conflict");
    CHECK(memmem_str(s, out_len, "=======\n") != NULL, "missing separator marker");
    CHECK(memmem_str(s, out_len, "THEIRS2\n") != NULL, "missing theirs content in conflict");
    CHECK(memmem_str(s, out_len, ">>>>>>> theirs-branch\n") != NULL,
         "missing/incorrect theirs conflict marker");

    /* markers must be exactly 7 characters of their symbol */
    {
        const char *p = memmem_str(s, out_len, "<<<<<<<");
        CHECK(p != NULL && p[7] == ' ', "ours marker must be exactly 7 '<' characters");
        p = memmem_str(s, out_len, "=======");
        CHECK(p != NULL && (p[7] == '\n'), "separator marker must be exactly 7 '=' characters");
        p = memmem_str(s, out_len, ">>>>>>>");
        CHECK(p != NULL && p[7] == ' ', "theirs marker must be exactly 7 '>' characters");
    }

    CHECK(memmem_str(s, out_len, "line1\n") != NULL, "context line1 should still be present");
    CHECK(memmem_str(s, out_len, "line3\n") != NULL, "context line3 should still be present");
    free(out);
}

/* (e) both sides change to exactly the same content: clean, no conflict */
static void test_both_changed_identically(void)
{
    static const unsigned char same[] = "line1\nSAME2\nline3\nline4\nline5\n";
    unsigned char *out = NULL;
    size_t out_len = 0;
    int rc;

    rc = sg_merge_content(BASE, BASE_LEN, same, sizeof(same) - 1, same, sizeof(same) - 1, "ours",
                          "theirs", &out, &out_len);
    CHECK(rc == 0, "identical edits on both sides should not conflict, got %d", rc);
    CHECK(out != NULL && out_len == sizeof(same) - 1 && memcmp(out, same, out_len) == 0,
         "result should equal the common edit");
    free(out);
}

/* (f) binary content (embedded NUL) is always reported as a conflict */
static void test_binary_content_conflicts(void)
{
    unsigned char base_bin[8] = {'a', 'b', 0, 'c', 'd', 0, 'e', 'f'};
    unsigned char ours_bin[8] = {'a', 'b', 0, 'X', 'd', 0, 'e', 'f'};
    unsigned char theirs_bin[8] = {'a', 'b', 0, 'c', 'd', 0, 'Y', 'f'};
    unsigned char *out = NULL;
    size_t out_len = 0;
    int rc;

    rc = sg_merge_content(base_bin, sizeof(base_bin), ours_bin, sizeof(ours_bin), theirs_bin,
                          sizeof(theirs_bin), "ours", "theirs", &out, &out_len);
    CHECK(rc == 1, "binary content must always be reported as a conflict, got %d", rc);
    free(out);
}

/* (g) regression: a sync-point anchor that sits at base's last (newline-less)
   line, but isn't the last line on the ours/theirs side, must still be
   followed by a newline in the output rather than gluing onto what comes
   next. base = "A\nB" (B has no trailing newline); ours appends "C\n" after
   B, theirs appends a different "D\n" after B, so B is a clean sync point
   but the tail conflicts. */
static void test_anchor_newline_not_glued(void)
{
    static const unsigned char base[] = "A\nB";
    static const unsigned char ours[] = "A\nB\nC\n";
    static const unsigned char theirs[] = "A\nB\nD\n";
    unsigned char *out = NULL;
    size_t out_len = 0;
    int rc;

    rc = sg_merge_content(base, sizeof(base) - 1, ours, sizeof(ours) - 1, theirs,
                          sizeof(theirs) - 1, "ours", "theirs", &out, &out_len);
    CHECK(rc == 1, "expected the tail to conflict, got %d", rc);
    CHECK(out != NULL, "conflict output should not be NULL");
    if (out == NULL)
        return;
    CHECK(memmem_str((const char *)out, out_len, "B<<<<<<<") == NULL,
         "anchor line 'B' must not be glued directly onto the conflict marker: '%.*s'",
         (int)out_len, (const char *)out);
    CHECK(memmem_str((const char *)out, out_len, "B\n<<<<<<<") != NULL,
         "anchor line 'B' should be followed by a newline before the conflict marker: '%.*s'",
         (int)out_len, (const char *)out);
    free(out);
}

/* ---- Phase 41: the four rules the merge learned from real git ----

   Every `expected` string below was MEASURED, by running

       git merge-file -L ours -L base -L theirs -p <ours> <base> <theirs>

   on the same three inputs, and is pasted here byte for byte. They are not
   derived from sg's own output, and they are not reasoned out from the
   algorithm -- that is the whole point, since three of the four rules were
   things sg got wrong while looking perfectly sensible. tests/fuzz_merge.py
   runs the same comparison over random inputs; these pin the specific shapes
   so a regression names itself instead of arriving as a rate.

   Note git merge-file's exit code counts conflicts (2 for two conflict
   blocks), while sg_merge_content only ever answers 0 or 1, so the rc
   expectations below are sg's convention, not git's number. */
static void check_merge(const char *name, const unsigned char *base, size_t base_len,
                        const unsigned char *ours, size_t ours_len, const unsigned char *theirs,
                        size_t theirs_len, int want_rc, const char *want, size_t want_len)
{
    unsigned char *out = NULL;
    size_t out_len = 0;
    int rc = sg_merge_content(base, base_len, ours, ours_len, theirs, theirs_len, "ours",
                             "theirs", &out, &out_len);

    CHECK(rc == want_rc, "%s: expected rc %d, got %d", name, want_rc, rc);
    if (out == NULL) {
        CHECK(0, "%s: result should not be NULL", name);
        return;
    }
    CHECK(out_len == want_len && memcmp(out, want, want_len) == 0,
         "%s: result differs from real git's\n  want: '%.*s'\n  got:  '%.*s'", name,
         (int)want_len, want, (int)out_len, (const char *)out);
    free(out);
}

#define CHECK_MERGE(name, base, ours, theirs, rc, want)                                          \
    check_merge(name, (const unsigned char *)(base), sizeof(base) - 1,                            \
               (const unsigned char *)(ours), sizeof(ours) - 1,                                   \
               (const unsigned char *)(theirs), sizeof(theirs) - 1, rc, want, sizeof(want) - 1)

/* Removing the trailing newline IS an edit. Before Phase 41 the line
   comparison ignored has_nl, so ours' edit here read as "ours changed
   nothing", theirs was taken wholesale, and the merge reported SUCCESS while
   silently discarding what the user did -- the worst shape a merge bug can
   take, because nothing tells the user. */
static void test_trailing_newline_removal_is_an_edit(void)
{
    CHECK_MERGE("trailing-newline removal", "x\ny\nz\n", "x\ny\nz", "x\ny\nZZZ\n", 1,
               "x\ny\n<<<<<<< ours\nz\n=======\nZZZ\n>>>>>>> theirs\n");
}

/* A conflict side whose last line has no newline still has to be followed by
   one, or the marker is glued onto it and the output contains a line that
   appears in none of the three inputs ("c=======" here). git does the same,
   via xdl_recs_copy's add_nl. Note the "c" is NOT hoisted out of the conflict
   by refinement: ours' "c" (no newline) and theirs' "c\n" are different
   lines, so the two sides genuinely have nothing in common here. */
static void test_conflict_side_missing_newline(void)
{
    CHECK_MERGE("conflict side without a trailing newline", "a\nb\nc\n", "a\nOURS\nc",
               "a\nTHEIRS\nc\n", 1,
               "a\n<<<<<<< ours\nOURS\nc\n=======\nTHEIRS\nc\n>>>>>>> theirs\n");
}

/* Refinement: the two sides of a conflict are diffed against each other and
   what they agree on is hoisted OUT of the conflict. Without it "B" would be
   printed twice, once inside each side. */
static void test_conflict_refinement_hoists_agreement(void)
{
    CHECK_MERGE("refinement hoists the agreed line", "A\nB", "A\nB\nC\n", "A\nB\nD\n", 1,
               "A\nB\n<<<<<<< ours\nC\n=======\nD\n>>>>>>> theirs\n");
}

/* Simplification, and its threshold. Two conflicts separated by 3 identical
   lines are printed as ONE conflict that swallows the gap; at 4 they stay
   separate. Both measured against git 2.55.0 -- the constant is git's
   (xdl_simplify_non_conflicts' `end - begin > 3`), and the pair of tests is
   what makes it a threshold rather than a number someone typed. */
static void test_close_conflicts_merge_into_one(void)
{
    CHECK_MERGE("gap of 3 merges", "X\ng1\ng2\ng3\nY\n", "ourX\ng1\ng2\ng3\nourY\n",
               "thrX\ng1\ng2\ng3\nthrY\n", 1,
               "<<<<<<< ours\nourX\ng1\ng2\ng3\nourY\n"
               "=======\nthrX\ng1\ng2\ng3\nthrY\n>>>>>>> theirs\n");
}

static void test_distant_conflicts_stay_separate(void)
{
    CHECK_MERGE("gap of 4 stays split", "X\ng1\ng2\ng3\ng4\nY\n",
               "ourX\ng1\ng2\ng3\ng4\nourY\n", "thrX\ng1\ng2\ng3\ng4\nthrY\n", 1,
               "<<<<<<< ours\nourX\n=======\nthrX\n>>>>>>> theirs\n"
               "g1\ng2\ng3\ng4\n"
               "<<<<<<< ours\nourY\n=======\nthrY\n>>>>>>> theirs\n");
}

/* The gap is not measured in lines alone: a one-sided change inside it blocks
   the merge however short it is. Here the gap is 3 lines, exactly the width
   that merges in the test above, but one of them is a line only ours touched,
   and git keeps the two conflicts apart. A purely distance-based rule passes
   the test above and fails this one. */
static void test_one_sided_change_blocks_the_merge(void)
{
    CHECK_MERGE("a resolved change blocks the merge", "X\ng1\nR\ng2\nY\n",
               "ourX\ng1\nourR\ng2\nourY\n", "thrX\ng1\nR\ng2\nthrY\n", 1,
               "<<<<<<< ours\nourX\n=======\nthrX\n>>>>>>> theirs\n"
               "g1\nourR\ng2\n"
               "<<<<<<< ours\nourY\n=======\nthrY\n>>>>>>> theirs\n");
}

/* A merge that changes nothing must not change the bytes either, and a
   conflict earlier in the file must not grow the file a newline at the end.
   Both measured; both were WRONG in the first cut of Phase 41's region list,
   and neither the fuzzer nor any other test here could see it: the sync-point
   pass pushes a zero-length region after the final anchor, and terminating
   the previous line before writing nothing gave a file that legitimately ends
   without a newline one it never had. The clean case is the alarming one --
   sg rewrote a file on a merge that resolved to "unchanged".

   These two need base itself to lack the trailing newline, which is a shape
   tests/fuzz_merge.py's generator could not produce until this round (it
   built base from "base%02d\n" lines and only ever stripped the newline in
   ours/theirs) -- so the fuzzer's 0/200 was, for this one dimension, zero
   evidence. */
static void test_noop_merge_keeps_missing_trailing_newline(void)
{
    CHECK_MERGE("no-op merge of a file with no trailing newline", "a\nb", "a\nb", "a\nb", 0,
               "a\nb");
}

static void test_conflict_above_a_newlineless_last_line(void)
{
    CHECK_MERGE("conflict above a newline-less last line", "a\nb\nc", "a\nX\nc", "a\nY\nc", 1,
               "a\n<<<<<<< ours\nX\n=======\nY\n>>>>>>> theirs\nc");
}

/* The empty-region guard asks whether the side being PRINTED is empty, not
   whether the ours side is -- and for a one-sided pure insertion the two
   answers differ. These two fixtures are what make that distinction
   observable: theirs inserts a line where ours has nothing at all, so the
   region is RESOLVED with an empty ours range and a non-empty theirs range.
   Asking the wrong side there does not merely misplace a newline, it drops
   the inserted line entirely, and the review round measured that all 13
   named tests before these two stayed green under exactly that mutation.
   (Both directions, because the mirror-image mistake is just as easy.) */
static void test_theirs_pure_insertion_survives(void)
{
    CHECK_MERGE("theirs inserts where ours has nothing", "a\nb\n", "a\nb\n", "a\nNEW\nb\n", 0,
               "a\nNEW\nb\n");
}

static void test_ours_pure_insertion_survives(void)
{
    CHECK_MERGE("ours inserts where theirs has nothing", "a\nb\n", "a\nNEW\nb\n", "a\nb\n", 0,
               "a\nNEW\nb\n");
}

int main(void)
{
    test_only_ours_changed();
    test_only_theirs_changed();
    test_both_changed_different_regions();
    test_same_region_conflict();
    test_both_changed_identically();
    test_binary_content_conflicts();
    test_anchor_newline_not_glued();
    test_trailing_newline_removal_is_an_edit();
    test_conflict_side_missing_newline();
    test_conflict_refinement_hoists_agreement();
    test_close_conflicts_merge_into_one();
    test_distant_conflicts_stay_separate();
    test_one_sided_change_blocks_the_merge();
    test_noop_merge_keeps_missing_trailing_newline();
    test_conflict_above_a_newlineless_last_line();
    test_theirs_pure_insertion_survives();
    test_ours_pure_insertion_survives();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all merge_content tests passed\n");
    return 0;
}
