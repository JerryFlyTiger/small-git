#ifndef SG_DIFF_LCS_H
#define SG_DIFF_LCS_H

#include <stddef.h>

/* A line viewed into an existing buffer -- ptr/len are borrowed, never
   owned. len excludes the trailing '\n'; has_nl records whether one
   actually followed in the source (false only for a final line that isn't
   newline-terminated). Shared by `sg diff`'s unified-diff printer and the
   three-way merge's line alignment, so the O(n*m) LCS table is built once
   and consumed differently by each caller instead of being reimplemented
   twice. */
typedef struct {
    const char *ptr;
    size_t len;
    int has_nl;
} sg_diff_line;

/* Splits data into lines. *count_out is always set; the returned array is
   NULL only on allocation failure (with *count_out left at 0). Caller frees
   the returned array (it never owns the underlying bytes). */
sg_diff_line *sg_diff_split_lines(const unsigned char *data, size_t len, size_t *count_out);

int sg_diff_lines_equal(sg_diff_line a, sg_diff_line b);

/* Like sg_diff_lines_equal, but ALSO requires has_nl to match. Real git
   treats "same text, different newline-termination" as a different line
   (Phase 26, measured: a final line missing its newline prints as its own
   -/+ row plus a following "\ No newline at end of file", never silently
   merged with an identically-spelled earlier line). Only the PATCH body
   needs this distinction -- sg_diff_lines_equal is left untouched because
   src/workdir/merge.c's three-way merge shares it and must not change
   behaviour here (see that file's own comment on why it ignores has_nl). */
int sg_diff_lines_equal_exact(sg_diff_line a, sg_diff_line b);

/* Builds the (na+1) x (nb+1) LCS length table: dp[i][j] = length of the
   longest common subsequence of a[i..na) and b[j..nb). Returns a malloc'd
   array of na+1 malloc'd rows (each nb+1 size_t entries), or NULL on
   allocation failure. Free with sg_diff_lcs_free_table. */
size_t **sg_diff_lcs_table(const sg_diff_line *a, size_t na, const sg_diff_line *b, size_t nb);
void sg_diff_lcs_free_table(size_t **dp, size_t na);

/* Same table, but classifying lines with sg_diff_lines_equal_exact instead
   of sg_diff_lines_equal -- used by sg_diff_build_script so the has_nl
   distinction actually drives the alignment, not just the final printed
   marker. */
size_t **sg_diff_lcs_table_exact(const sg_diff_line *a, size_t na, const sg_diff_line *b, size_t nb);

/* ---- minimal edit script (patch-body intermediate representation) ---- */

/* One maximal run of change at a given synchronized gap: a[a_off,
   a_off+a_len) is removed, b[b_off, b_off+b_len) is added. Either a_len or
   b_len (but not both) may be 0 for a pure insertion/deletion; both
   non-zero is a "replace", meaning the two files each independently have a
   changed run at this same gap index -- NOT a distinguished case the
   backtrack pins in place. Internally (src/util/diff_lcs.c) each side is
   tracked as its own per-file "changed" bitmap and slid independently
   (mirroring git's xdiff/xdiffi.c: two xdfile_t structures, each compacted
   on its own, synchronized only by an end_matching_other tie-break so an
   edit that really is a single replace doesn't get split into a
   free-floating delete and a free-floating add); this struct is just the
   final, re-paired output of that process. Between two consecutive groups
   (or before the first / after the last), a[..]==b[..] line for line. */
typedef struct {
    size_t a_off, a_len;
    size_t b_off, b_len;
} sg_diff_group;

typedef struct {
    sg_diff_group *groups;
    size_t count;
} sg_diff_script;

/* Builds the minimal edit script between a and b (has_nl-aware), then:
     1. group compaction -- a pure-insert or pure-delete group bordered by
        identical lines on either side can be slid up or down within its
        reachable range without changing what the diff represents (the
        classic "which of these two adjacent identical lines is the
        context line" ambiguity). Default position (indent_heuristic == 0)
        is the BOTTOM of that range -- measured against real git with
        diff.indentHeuristic off.
     2. if indent_heuristic is set, scores every reachable position using a
        reconstruction of git's indentation heuristic (diff.indentHeuristic,
        on by default since git 2.14) and picks the lowest-penalty one,
        breaking ties toward the bottom. The scoring constants
        (INDENT_WEIGHT and friends in diff_lcs.c) are a best-effort
        reconstruction from memory, NOT independently verified against
        git's source -- tests/fuzz_diff.py, which diffs sg's actual output
        against real git's, is the only judge of whether they are right.
   Returns NULL only on allocation failure. */
sg_diff_script *sg_diff_build_script(const sg_diff_line *a, size_t na, const sg_diff_line *b, size_t nb,
                                    int indent_heuristic);
void sg_diff_script_free(sg_diff_script *script);

#endif
