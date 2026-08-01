#include "sg/diff_lcs.h"

#include <stdlib.h>
#include <string.h>

sg_diff_line *sg_diff_split_lines(const unsigned char *data, size_t len, size_t *count_out)
{
    size_t cap = 16;
    size_t count = 0;
    sg_diff_line *lines = malloc(cap * sizeof(*lines));
    size_t pos = 0;

    if (lines == NULL) {
        *count_out = 0;
        return NULL;
    }

    while (pos < len) {
        const unsigned char *nl = memchr(data + pos, '\n', len - pos);
        size_t linelen = nl != NULL ? (size_t)(nl - (data + pos)) : (len - pos);

        if (count == cap) {
            sg_diff_line *grown;

            cap *= 2;
            grown = realloc(lines, cap * sizeof(*lines));
            if (grown == NULL)
                break;
            lines = grown;
        }
        lines[count].ptr = (const char *)(data + pos);
        lines[count].len = linelen;
        lines[count].has_nl = (nl != NULL);
        count++;

        pos += linelen;
        if (nl != NULL)
            pos++;
    }

    *count_out = count;
    return lines;
}

int sg_diff_lines_equal(sg_diff_line a, sg_diff_line b)
{
    return a.len == b.len && memcmp(a.ptr, b.ptr, a.len) == 0;
}

size_t **sg_diff_lcs_table(const sg_diff_line *a, size_t na, const sg_diff_line *b, size_t nb)
{
    size_t **dp;
    size_t i, j;
    size_t allocated_rows = 0;

    dp = malloc((na + 1) * sizeof(*dp));
    if (dp == NULL)
        return NULL;
    for (i = 0; i <= na; i++) {
        dp[i] = malloc((nb + 1) * sizeof(**dp));
        if (dp[i] == NULL) {
            for (allocated_rows = 0; allocated_rows < i; allocated_rows++)
                free(dp[allocated_rows]);
            free(dp);
            return NULL;
        }
    }

    for (i = na + 1; i-- > 0;) {
        for (j = nb + 1; j-- > 0;) {
            if (i == na || j == nb) {
                dp[i][j] = 0;
            } else if (sg_diff_lines_equal(a[i], b[j])) {
                dp[i][j] = dp[i + 1][j + 1] + 1;
            } else {
                size_t v1 = dp[i + 1][j];
                size_t v2 = dp[i][j + 1];

                dp[i][j] = v1 > v2 ? v1 : v2;
            }
        }
    }

    return dp;
}

void sg_diff_lcs_free_table(size_t **dp, size_t na)
{
    size_t i;

    if (dp == NULL)
        return;
    for (i = 0; i <= na; i++)
        free(dp[i]);
    free(dp);
}
