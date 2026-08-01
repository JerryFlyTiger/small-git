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

/* Builds the (na+1) x (nb+1) LCS length table: dp[i][j] = length of the
   longest common subsequence of a[i..na) and b[j..nb). Returns a malloc'd
   array of na+1 malloc'd rows (each nb+1 size_t entries), or NULL on
   allocation failure. Free with sg_diff_lcs_free_table. */
size_t **sg_diff_lcs_table(const sg_diff_line *a, size_t na, const sg_diff_line *b, size_t nb);
void sg_diff_lcs_free_table(size_t **dp, size_t na);

#endif
