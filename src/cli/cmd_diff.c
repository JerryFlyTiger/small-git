#include "sg/cli.h"

#include "sg/chunk.h"
#include "sg/diff_lcs.h"
#include "sg/hash.h"
#include "sg/index.h"
#include "sg/objstore.h"
#include "sg/object.h"
#include "sg/quote.h"
#include "sg/repo.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int has_nul(const unsigned char *data, size_t len)
{
    return memchr(data, '\0', len) != NULL;
}

/* O(n*m) LCS-based unified diff; correctness of the +/-/context classification
   matters here, not minimality of the edit script or matching git's hunk
   splitting/context-window conventions exactly. */
/* The unified-diff format separates the filename on a ---/+++ line from an
   optional timestamp with whitespace, so a name containing a space is
   ambiguous. Real git disambiguates by appending a TAB, and only on those
   two lines -- measured against 2.55.0: "diff --git" and "Binary files"
   never get one, and neither does a name whose only oddity is a control
   character, because quoting escapes that away while a space is left as-is.
   Returns "\t" or "". */
static const char *diff_name_terminator(const char *path)
{
    return strchr(path, ' ') != NULL ? "\t" : "";
}

static int print_text_diff(const char *path, const unsigned char *a_data, size_t a_len,
                           const unsigned char *b_data, size_t b_len)
{
    size_t na, nb;
    sg_diff_line *a = sg_diff_split_lines(a_data, a_len, &na);
    sg_diff_line *b = sg_diff_split_lines(b_data, b_len, &nb);
    size_t **dp;
    size_t i, j;

    dp = sg_diff_lcs_table(a, na, b, nb);
    if (dp == NULL) {
        free(a);
        free(b);
        return -1;
    }

    printf("diff --git %s %s\n", sg_quote_path_prefixed("a/", path),
          sg_quote_path_prefixed("b/", path));
    printf("--- %s%s\n", sg_quote_path_prefixed("a/", path), diff_name_terminator(path));
    printf("+++ %s%s\n", sg_quote_path_prefixed("b/", path), diff_name_terminator(path));
    printf("@@ -1,%zu +1,%zu @@\n", na, nb);

    i = 0;
    j = 0;
    while (i < na || j < nb) {
        if (i < na && j < nb && sg_diff_lines_equal(a[i], b[j])) {
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

    sg_diff_lcs_free_table(dp, na);
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
    int had_chunk_error = 0;

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
        char abspath[SG_PATH_MAX];
        unsigned char *a_content = NULL;
        size_t a_len = 0;
        unsigned char *b_content = NULL;
        size_t b_len = 0;

        /* stage 1/2/3 entries (an unresolved conflict) have no single
           "staged blob" to diff against the working tree -- skip them here;
           `sg status`'s "Unmerged paths" section is what surfaces those. */
        if (idx.entries[i].stage != 0)
            continue;

        {
            sg_chunk_missing_info missing;
            int read_rc = sg_chunk_read_blob(git_dir, idx.entries[i].sha1, &a_content, &a_len, &missing);

            if (read_rc == -2) {
                /* A genuine chunk pointer whose data is missing/corrupt --
                   must hard-error, not diff the pointer's own raw text
                   against the working tree as if it were the file's real
                   content (that would produce meaningless garbage hunks). */
                sg_chunk_print_missing_error(idx.entries[i].path, &missing);
                had_chunk_error = 1;
                continue;
            }
            if (read_rc != 0) {
                fprintf(stderr, "sg: warning: cannot read staged blob for '%s'\n", idx.entries[i].path);
                continue;
            }
        }

        /* A truncated path must not be silently skipped: that would leave
           this entry undiffed and `sg diff` would report a changed file as
           unchanged instead. */
        if (sg_path_join(abspath, sizeof(abspath), repo_root, idx.entries[i].path) != 0) {
            fprintf(stderr, "sg: warning: 路徑過長,無法比較 '%s'\n", idx.entries[i].path);
            free(a_content);
            had_chunk_error = 1;
            continue;
        }
        (void)sg_read_file(abspath, &b_content, &b_len); /* missing file => empty b (all removed) */

        if (a_len == b_len && (a_len == 0 || memcmp(a_content, b_content, a_len) == 0)) {
            free(a_content);
            free(b_content);
            continue;
        }

        if (has_nul(a_content, a_len) || (b_content != NULL && has_nul(b_content, b_len))) {
            /* The "diff --git" line goes out for a binary file too. sg used
               to print only the "Binary files ... differ" line, which is not
               a patch any tool can read back: git apply keys off the
               "diff --git" header to know which file a hunk belongs to.
               Measured against git 2.55.0, which prints both. Found while
               adding a binary fixture for the quoting work, not by it. */
            printf("diff --git %s %s\n",
                  sg_quote_path_prefixed("a/", idx.entries[i].path),
                  sg_quote_path_prefixed("b/", idx.entries[i].path));
            printf("Binary files %s and %s differ\n",
                  sg_quote_path_prefixed("a/", idx.entries[i].path),
                  sg_quote_path_prefixed("b/", idx.entries[i].path));
        } else if (print_text_diff(idx.entries[i].path, a_content, a_len, b_content, b_len) != 0) {
            fprintf(stderr, "sg: warning: out of memory diffing '%s'\n", idx.entries[i].path);
        }

        free(a_content);
        free(b_content);
    }

    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
    return had_chunk_error ? 1 : 0;
}
