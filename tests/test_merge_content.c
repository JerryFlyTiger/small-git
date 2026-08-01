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

int main(void)
{
    test_only_ours_changed();
    test_only_theirs_changed();
    test_both_changed_different_regions();
    test_same_region_conflict();
    test_both_changed_identically();
    test_binary_content_conflicts();
    test_anchor_newline_not_glued();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all merge_content tests passed\n");
    return 0;
}
