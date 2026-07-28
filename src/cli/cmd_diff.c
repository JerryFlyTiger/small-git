#include "sg/cli.h"

#include "sg/hash.h"
#include "sg/index.h"
#include "sg/objstore.h"
#include "sg/object.h"
#include "sg/repo.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *ptr;
    size_t len;
} diff_line;

static diff_line *split_lines(const unsigned char *data, size_t len, size_t *count_out)
{
    size_t cap = 16;
    size_t count = 0;
    diff_line *lines = malloc(cap * sizeof(*lines));
    size_t pos = 0;

    if (lines == NULL) {
        *count_out = 0;
        return NULL;
    }

    while (pos < len) {
        const unsigned char *nl = memchr(data + pos, '\n', len - pos);
        size_t linelen = nl != NULL ? (size_t)(nl - (data + pos)) : (len - pos);

        if (count == cap) {
            diff_line *grown;

            cap *= 2;
            grown = realloc(lines, cap * sizeof(*lines));
            if (grown == NULL)
                break;
            lines = grown;
        }
        lines[count].ptr = (const char *)(data + pos);
        lines[count].len = linelen;
        count++;

        pos += linelen;
        if (nl != NULL)
            pos++;
    }

    *count_out = count;
    return lines;
}

static int lines_equal(diff_line a, diff_line b)
{
    return a.len == b.len && memcmp(a.ptr, b.ptr, a.len) == 0;
}

static int has_nul(const unsigned char *data, size_t len)
{
    return memchr(data, '\0', len) != NULL;
}

/* O(n*m) LCS-based unified diff; correctness of the +/-/context classification
   matters here, not minimality of the edit script or matching git's hunk
   splitting/context-window conventions exactly. */
static int print_text_diff(const char *path, const unsigned char *a_data, size_t a_len,
                           const unsigned char *b_data, size_t b_len)
{
    size_t na, nb;
    diff_line *a = split_lines(a_data, a_len, &na);
    diff_line *b = split_lines(b_data, b_len, &nb);
    size_t **dp;
    size_t i, j;
    size_t allocated_rows = 0;

    dp = malloc((na + 1) * sizeof(*dp));
    if (dp == NULL) {
        free(a);
        free(b);
        return -1;
    }
    for (i = 0; i <= na; i++) {
        dp[i] = malloc((nb + 1) * sizeof(**dp));
        if (dp[i] == NULL) {
            for (allocated_rows = 0; allocated_rows < i; allocated_rows++)
                free(dp[allocated_rows]);
            free(dp);
            free(a);
            free(b);
            return -1;
        }
    }

    for (i = na + 1; i-- > 0;) {
        for (j = nb + 1; j-- > 0;) {
            if (i == na || j == nb) {
                dp[i][j] = 0;
            } else if (lines_equal(a[i], b[j])) {
                dp[i][j] = dp[i + 1][j + 1] + 1;
            } else {
                size_t v1 = dp[i + 1][j];
                size_t v2 = dp[i][j + 1];

                dp[i][j] = v1 > v2 ? v1 : v2;
            }
        }
    }

    printf("diff --git a/%s b/%s\n", path, path);
    printf("--- a/%s\n", path);
    printf("+++ b/%s\n", path);
    printf("@@ -1,%zu +1,%zu @@\n", na, nb);

    i = 0;
    j = 0;
    while (i < na || j < nb) {
        if (i < na && j < nb && lines_equal(a[i], b[j])) {
            printf(" %.*s\n", (int)a[i].len, a[i].ptr);
            i++;
            j++;
        } else if (i < na && (j >= nb || dp[i + 1][j] >= dp[i][j + 1])) {
            printf("-%.*s\n", (int)a[i].len, a[i].ptr);
            i++;
        } else {
            printf("+%.*s\n", (int)b[j].len, b[j].ptr);
            j++;
        }
    }

    for (i = 0; i <= na; i++)
        free(dp[i]);
    free(dp);
    free(a);
    free(b);
    return 0;
}

int sg_cmd_diff(int argc, char **argv)
{
    char *git_dir;
    char *repo_root;
    sg_index idx;
    size_t i;

    (void)argv;
    if (argc != 1) {
        fprintf(stderr, "usage: sg diff\n");
        return 1;
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;
    repo_root = sg_repo_root(git_dir);
    if (repo_root == NULL) {
        fprintf(stderr, "sg: failed to determine repository root\n");
        free(git_dir);
        return 1;
    }
    if (sg_index_read(git_dir, &idx) != 0) {
        fprintf(stderr, "sg: failed to read index (corrupt?)\n");
        free(git_dir);
        free(repo_root);
        return 1;
    }

    for (i = 0; i < idx.count; i++) {
        char abspath[4096];
        sg_obj_type type;
        unsigned char *a_content = NULL;
        size_t a_len = 0;
        unsigned char *b_content = NULL;
        size_t b_len = 0;

        if (sg_object_read(git_dir, idx.entries[i].sha1, &type, &a_content, &a_len) != 0) {
            fprintf(stderr, "sg: warning: cannot read staged blob for '%s'\n", idx.entries[i].path);
            continue;
        }

        snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, idx.entries[i].path);
        (void)sg_read_file(abspath, &b_content, &b_len); /* missing file => empty b (all removed) */

        if (a_len == b_len && (a_len == 0 || memcmp(a_content, b_content, a_len) == 0)) {
            free(a_content);
            free(b_content);
            continue;
        }

        if (has_nul(a_content, a_len) || (b_content != NULL && has_nul(b_content, b_len))) {
            printf("Binary files a/%s and b/%s differ\n", idx.entries[i].path, idx.entries[i].path);
        } else if (print_text_diff(idx.entries[i].path, a_content, a_len, b_content, b_len) != 0) {
            fprintf(stderr, "sg: warning: out of memory diffing '%s'\n", idx.entries[i].path);
        }

        free(a_content);
        free(b_content);
    }

    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
    return 0;
}
