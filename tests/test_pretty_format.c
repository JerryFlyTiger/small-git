/* Phase 60b: --format placeholder expansion (spec section 5). Three groups:
   the six date renderings against the spec's own fixture timestamp
   (1700000000 +0800, section 5.1), the ten %f rows (section 5.2), and
   sg_pretty_validate_format's accept/reject boundary (section 5.3) plus a
   parent-list check on a merge. The %f/%P checks go through the public
   sg_commit_out_entry entry point with stdout redirected to a temp file --
   sanitize_subject/expand_user_format are file-local statics in
   commit_out.c, and there is no test-only export for them (unlike e.g.
   sg_tree_flatten_test_count), so the public renderer is the only seam. */
#include "sg/commit_out.h"
#include "sg/date.h"
#include "sg/hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

/* Redirects stdout to a temp file for the duration of fn(commit, id, pf),
   then reads the captured bytes into out (NUL-terminated, truncated at
   out_cap - 1). fn is expected to call sg_commit_out_entry itself so this
   helper stays generic across the different fixtures below. */
static void capture(const sg_commit *commit, const unsigned char id[SG_SHA1_RAW_LEN],
                    const sg_pretty_format *pf, char *out, size_t out_cap)
{
    sg_commit_out_opts o;
    FILE *tmp = tmpfile();
    int old_fd;
    size_t n;

    out[0] = '\0';
    if (tmp == NULL)
        return;

    memset(&o, 0, sizeof o);
    o.pretty = pf;

    old_fd = dup(fileno(stdout));
    fflush(stdout);
    dup2(fileno(tmp), fileno(stdout));

    sg_commit_out_entry(NULL, id, commit, &o);

    fflush(stdout);
    dup2(old_fd, fileno(stdout));
    close(old_fd);

    rewind(tmp);
    n = fread(out, 1, out_cap - 1, tmp);
    out[n] = '\0';
    fclose(tmp);
}

/* Same idea as capture(), but for the legacy o.oneline=1 path (kind ==
   SG_PRETTY_LEGACY, o.pretty == NULL) -- capture() always threads a
   non-NULL sg_pretty_format through o.pretty, which cannot express this
   case. */
static void capture_legacy_oneline(const sg_commit *commit, const unsigned char id[SG_SHA1_RAW_LEN],
                                   char *out, size_t out_cap)
{
    sg_commit_out_opts o;
    FILE *tmp = tmpfile();
    int old_fd;
    size_t n;

    out[0] = '\0';
    if (tmp == NULL)
        return;

    memset(&o, 0, sizeof o);
    o.oneline = 1;

    old_fd = dup(fileno(stdout));
    fflush(stdout);
    dup2(fileno(tmp), fileno(stdout));

    sg_commit_out_entry(NULL, id, commit, &o);

    fflush(stdout);
    dup2(old_fd, fileno(stdout));
    close(old_fd);

    rewind(tmp);
    n = fread(out, 1, out_cap - 1, tmp);
    out[n] = '\0';
    fclose(tmp);
}

static void make_fixture_commit(sg_commit *commit)
{
    memset(commit, 0, sizeof(*commit));
    commit->author_name = (char *)"A U Thor";
    commit->author_email = (char *)"author@example.com";
    commit->author_time = 1700000000;
    strcpy(commit->author_tz, "+0800");
    commit->committer_name = (char *)"C O Mitter";
    commit->committer_email = (char *)"committer@example.com";
    commit->committer_time = 1700000100;
    strcpy(commit->committer_tz, "+0900");
    commit->message = (char *)"second subject\n";
    commit->extra_headers = (char *)"";
}

/* Section 5.1's six date renderings, measured from 1700000000 +0800. */
static void test_date_renderings(void)
{
    char buf[SG_DATE_NORMAL_MAX];

    CHECK(sg_date_format_normal(1700000000, "+0800", buf, sizeof buf) == 0 &&
         strcmp(buf, "Wed Nov 15 06:13:20 2023 +0800") == 0,
         "%%ad: got '%s'", buf);
    CHECK(sg_date_format_rfc2822(1700000000, "+0800", buf, sizeof buf) == 0 &&
         strcmp(buf, "Wed, 15 Nov 2023 06:13:20 +0800") == 0,
         "%%aD: got '%s'", buf);
    CHECK(sg_date_format_iso(1700000000, "+0800", buf, sizeof buf) == 0 &&
         strcmp(buf, "2023-11-15 06:13:20 +0800") == 0,
         "%%ai: got '%s'", buf);
    CHECK(sg_date_format_iso_strict(1700000000, "+0800", buf, sizeof buf) == 0 &&
         strcmp(buf, "2023-11-15T06:13:20+08:00") == 0,
         "%%aI: got '%s'", buf);
    CHECK(sg_date_format_short(1700000000, "+0800", buf, sizeof buf) == 0 &&
         strcmp(buf, "2023-11-15") == 0,
         "%%as: got '%s'", buf);
}

/* %at needs no helper -- it is the raw epoch, unshifted -- checked end to
   end below alongside the other placeholders. */
static void test_unix_timestamp_end_to_end(void)
{
    sg_commit commit;
    sg_pretty_format pf;
    unsigned char id[SG_SHA1_RAW_LEN] = { 0 };
    char out[128];

    make_fixture_commit(&commit);
    pf.kind = SG_PRETTY_FORMAT;
    pf.user_format = "%at";
    capture(&commit, id, &pf, out, sizeof out);
    CHECK(strcmp(out, "1700000000") == 0, "%%at: got '%s'", out);
}

/* Phase 67: %ah/%ch end to end. Deliberately uses author/committer times 40
   days apart (matching PHASE67_SPEC.md section 3's own example) rather than
   make_fixture_commit's stock 100-second gap -- a swap of author<->committer
   fields is invisible at 100 seconds (both would render the same relative-
   day shape), but at 40 days the two placeholders land in genuinely
   different output shapes, so a swap bug produces a wrong STRING, not just
   a coincidentally-matching one. Byte values measured against real git
   2.55.0 via python subprocess (argv, no shell) -- see PHASE67_SPEC.md's
   own oracle methodology note. */
static void test_human_placeholder_end_to_end(void)
{
    sg_commit commit;
    sg_pretty_format pf;
    unsigned char id[SG_SHA1_RAW_LEN] = { 0 };
    char out[128];

    make_fixture_commit(&commit);
    commit.author_time = 1700000000;
    strcpy(commit.author_tz, "+0800");
    commit.committer_time = 1700000000 + 40 * 86400;
    strcpy(commit.committer_tz, "-0500");

    /* %ah/%ch compares "now" against the MACHINE'S LOCAL zone (never the
       stored tz) -- pin TZ so this test is deterministic regardless of
       where it runs, same idiom tests/test_date_mode.c's with_tz uses. */
    setenv("TZ", "UTC", 1);
    tzset();
    setenv("GIT_TEST_DATE_NOW", "1703888000" /* 1700000000 + 45*86400 */, 1);

    pf.kind = SG_PRETTY_FORMAT;
    pf.user_format = "%ah";
    capture(&commit, id, &pf, out, sizeof out);
    CHECK(strcmp(out, "Wed Nov 15 06:13") == 0, "%%ah: got '%s'", out);

    pf.user_format = "%ch";
    capture(&commit, id, &pf, out, sizeof out);
    CHECK(strcmp(out, "Sun Dec 24 17:13") == 0, "%%ch: got '%s'", out);

    unsetenv("GIT_TEST_DATE_NOW");
}

/* Section 5.2's ten %f rows, byte-exact. */
static void test_sanitized_subject_rows(void)
{
    static const struct {
        const char *subject;
        const char *expected;
    } ROWS[] = {
        { "Hello, World!", "Hello-World" },
        { "  leading and trailing  ", "leading-and-trailing" },
        { "a...b", "a.b" },
        { "many   spaces", "many-spaces" },
        { "slash/and:colon", "slash-and-colon" },
        { "ends with a dot.", "ends-with-a-dot" },
        { "-leading-dash", "leading-dash" },
        { "UPPER Case", "UPPER-Case" },
        { "unicode caf\xc3\xa9 test", "unicode-caf-test" },
        /* Review round: this asymmetry (leading '.' survives, leading '-'
           does not) had no witness anywhere in the original ten-row table
           -- none of them start with '.'. Measured against real git. */
        { ".leading", ".leading" },
    };
    size_t i;
    sg_commit commit;
    sg_pretty_format pf;
    unsigned char id[SG_SHA1_RAW_LEN] = { 0 };
    char out[256];
    char ninety_a[91];
    char ninety_msg[93];

    pf.kind = SG_PRETTY_FORMAT;
    pf.user_format = "%f";

    for (i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); i++) {
        char msg[128];

        make_fixture_commit(&commit);
        snprintf(msg, sizeof msg, "%s\n", ROWS[i].subject);
        commit.message = msg;
        capture(&commit, id, &pf, out, sizeof out);
        CHECK(strcmp(out, ROWS[i].expected) == 0, "subject '%s': expected '%s', got '%s'",
             ROWS[i].subject, ROWS[i].expected, out);
    }

    /* Row 10: 90 a's, no length limit, no truncation. */
    memset(ninety_a, 'a', 90);
    ninety_a[90] = '\0';
    snprintf(ninety_msg, sizeof ninety_msg, "%s\n", ninety_a);
    make_fixture_commit(&commit);
    commit.message = ninety_msg;
    capture(&commit, id, &pf, out, sizeof out);
    CHECK(strcmp(out, ninety_a) == 0, "90 a's: expected 90 a's unchanged, got len %zu",
         strlen(out));
}

/* %P/%p on a 2-parent merge -- space separated, full and abbreviated. */
static void test_parent_list_on_merge(void)
{
    sg_commit commit;
    sg_pretty_format pf;
    unsigned char id[SG_SHA1_RAW_LEN] = { 0 };
    unsigned char parents[2][SG_SHA1_RAW_LEN];
    char out[256];

    memset(parents[0], 0x11, SG_SHA1_RAW_LEN);
    memset(parents[1], 0x22, SG_SHA1_RAW_LEN);

    make_fixture_commit(&commit);
    commit.parent_count = 2;
    commit.parents = parents;

    pf.kind = SG_PRETTY_FORMAT;
    pf.user_format = "%P";
    capture(&commit, id, &pf, out, sizeof out);
    CHECK(strcmp(out,
                "1111111111111111111111111111111111111111 2222222222222222222222222222222222222222")
             == 0,
         "%%P: got '%s'", out);

    pf.user_format = "%p";
    capture(&commit, id, &pf, out, sizeof out);
    CHECK(strcmp(out, "1111111 2222222") == 0, "%%p: got '%s'", out);
}

/* Section 5.3: everything outside the table is rejected, naming the
   offending sequence. */
static void test_validate_rejects_outside_table(void)
{
    /* %ar/%cr are implemented as of Phase 66, %ah/%ch as of Phase 67 (see
       test_validate_accepts_every_table_entry below). */
    static const char *const BAD[] = { "%z", "%d", "%C(red)", "100%" };
    size_t i;

    for (i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        const char *bad = NULL;
        size_t bad_len = 0;
        int rc = sg_pretty_validate_format(BAD[i], &bad, &bad_len);

        CHECK(rc == -1, "expected '%s' to be rejected, got rc=%d", BAD[i], rc);
        CHECK(bad != NULL && bad_len > 0, "expected a non-empty offending span for '%s'", BAD[i]);
    }
}

static void test_validate_accepts_every_table_entry(void)
{
    static const char *const GOOD[] = {
        "%H", "%h", "%T", "%t", "%P", "%p",
        "%an", "%ae", "%al", "%ad", "%aD", "%at", "%ai", "%aI", "%as", "%ar", "%ah",
        "%cn", "%ce", "%cl", "%cd", "%cD", "%ct", "%ci", "%cI", "%cs", "%cr", "%ch",
        "%s", "%f", "%b", "%B",
        "%n", "%%", "%x41",
    };
    size_t i;

    for (i = 0; i < sizeof(GOOD) / sizeof(GOOD[0]); i++) {
        int rc = sg_pretty_validate_format(GOOD[i], NULL, NULL);

        CHECK(rc == 0, "expected '%s' to be accepted, got rc=%d", GOOD[i], rc);
    }
}

/* Mid-review correction (measured directly against real git 2.55.0, not
   in the original Phase 60 spec's tables): %s folds a multi-line subject
   into one space-joined line when no blank line separates it from
   whatever follows -- skip leading blank lines, join every line up to the
   next blank line with a single space, each line's own trailing
   whitespace stripped first but a continuation line's leading whitespace
   preserved. The SAME rule applies to the oneline/reference builtins and
   to legacy --oneline (fold_subject in commit_out.c, one function, five
   call sites) -- but NOT to `short`, which prints the first paragraph
   VERBATIM, one physical line at a time (first_paragraph_span). This is
   the one place these five sites do NOT all behave the same. */
static void test_subject_folding(void)
{
    static const struct {
        const char *msg;
        const char *folded; /* %s / oneline / reference / legacy --oneline */
    } ROWS[] = {
        { "l1\n l2\n l3\n\nbody\n", "l1  l2  l3" },
        { "l1\nl2\n\nbody\n", "l1 l2" },
        { "l1\nl2\nl3\n", "l1 l2 l3" },
        { "l1\n\nl2\nl3\n", "l1" },
        { "\nl1\nl2\n", "l1 l2" },
        { "l1   \nl2\n\nbody\n", "l1 l2" },
        { "l1\n\tl2\n\nbody\n", "l1 \tl2" },
        { "just one\n", "just one" },
    };
    size_t i;
    sg_commit commit;
    sg_pretty_format pf;
    unsigned char id[SG_SHA1_RAW_LEN] = { 0 };
    char out[256];

    for (i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); i++) {
        make_fixture_commit(&commit);
        commit.message = (char *)ROWS[i].msg;

        pf.kind = SG_PRETTY_FORMAT;
        pf.user_format = "%s";
        capture(&commit, id, &pf, out, sizeof out);
        CHECK(strcmp(out, ROWS[i].folded) == 0, "row %zu (%%s): expected '%s', got '%s'",
             i, ROWS[i].folded, out);

        pf.kind = SG_PRETTY_ONELINE;
        pf.user_format = NULL;
        capture(&commit, id, &pf, out, sizeof out);
        CHECK(strstr(out, ROWS[i].folded) != NULL, "row %zu (oneline): expected to contain '%s', got '%s'",
             i, ROWS[i].folded, out);

        pf.kind = SG_PRETTY_REFERENCE;
        capture(&commit, id, &pf, out, sizeof out);
        CHECK(strstr(out, ROWS[i].folded) != NULL, "row %zu (reference): expected to contain '%s', got '%s'",
             i, ROWS[i].folded, out);

        capture_legacy_oneline(&commit, id, out, sizeof out);
        CHECK(strstr(out, ROWS[i].folded) != NULL,
             "row %zu (legacy --oneline): expected to contain '%s', got '%s'", i, ROWS[i].folded, out);
    }
}

/* `short` is the one exception -- verbatim first paragraph, not folded.
   Row 0 (a genuinely multi-line first paragraph) is the discriminator: if
   `short` were wrongly routed through fold_subject, this would match the
   folded string above instead. */
static void test_short_does_not_fold(void)
{
    sg_commit commit;
    sg_pretty_format pf;
    unsigned char id[SG_SHA1_RAW_LEN] = { 0 };
    char out[256];

    make_fixture_commit(&commit);
    commit.message = (char *)"l1\n l2\n l3\n\nbody\n";
    pf.kind = SG_PRETTY_SHORT;
    pf.user_format = NULL;
    capture(&commit, id, &pf, out, sizeof out);

    CHECK(strstr(out, "l1  l2  l3") == NULL, "short must NOT fold, got '%s'", out);
    CHECK(strstr(out, "l1\n") != NULL && strstr(out, " l2\n") != NULL && strstr(out, " l3") != NULL,
         "short must print each physical line of the first paragraph, got '%s'", out);
    CHECK(strstr(out, "body") == NULL, "short must not print the body, got '%s'", out);
}

/* Phase 60c: print_message's whole-message-body rendering
   (medium/full/fuller/raw), measured directly against real git 2.55.0.
   git's own porcelain commit path strips every one of these shapes before
   the object ever reaches the store, which is exactly why this went
   unnoticed until fixtures were built directly at the object level
   (interop's `phase60c:` group uses `git hash-object -t commit -w --stdin`
   for the same reason). Uses --pretty=medium as the representative
   format -- interop's group additionally sweeps full/fuller/raw to prove
   the fix (one shared function, print_message) covers all four. */
static void test_message_block_rendering(void)
{
    sg_commit commit;
    sg_pretty_format pf;
    unsigned char id[SG_SHA1_RAW_LEN] = { 0 };
    char out[512];

    pf.kind = SG_PRETTY_MEDIUM;
    pf.user_format = NULL;

    /* Leading blank line(s) are skipped entirely -- not rendered even as
       "    \n". */
    make_fixture_commit(&commit);
    commit.message = (char *)"\nsubject here\n\nbody\n";
    capture(&commit, id, &pf, out, sizeof out);
    CHECK(strstr(out, "\n    subject here\n") != NULL,
         "leading blank: expected the subject right after the header blank line, got '%s'", out);
    CHECK(strstr(out, "\n    \n    subject here") == NULL,
         "leading blank: the leading blank line must not be rendered, got '%s'", out);

    /* Trailing blank line(s) are skipped entirely. */
    make_fixture_commit(&commit);
    commit.message = (char *)"subj\n\nbody\n\n\n";
    capture(&commit, id, &pf, out, sizeof out);
    CHECK(strstr(out, "    subj\n    \n    body\n") != NULL,
         "trailing blank: expected the message to end right after 'body', got '%s'", out);
    CHECK(strstr(out, "    body\n    \n") == NULL,
         "trailing blank: no trailing blank line may follow 'body', got '%s'", out);

    /* Trailing whitespace on a BODY line is stripped. */
    make_fixture_commit(&commit);
    commit.message = (char *)"subj\n\nbody trailing   \nsecond   \n";
    capture(&commit, id, &pf, out, sizeof out);
    CHECK(strstr(out, "    body trailing\n    second\n") != NULL,
         "body trailing ws: expected both body lines stripped, got '%s'", out);
    CHECK(strstr(out, "trailing   \n") == NULL && strstr(out, "second   \n") == NULL,
         "body trailing ws: no trailing whitespace may survive, got '%s'", out);

    /* Trailing whitespace on the SUBJECT line is stripped too. */
    make_fixture_commit(&commit);
    commit.message = (char *)"subj   \n\nbody\n";
    capture(&commit, id, &pf, out, sizeof out);
    CHECK(strstr(out, "    subj\n    \n    body\n") != NULL,
         "subject trailing ws: expected the subject line stripped, got '%s'", out);
    CHECK(strstr(out, "subj   \n") == NULL,
         "subject trailing ws: no trailing whitespace may survive, got '%s'", out);

    /* A blank line in the MIDDLE is preserved, and EACH one individually
       (not squeezed into a single blank line). */
    make_fixture_commit(&commit);
    commit.message = (char *)"subj\n\n\nbody\n";
    capture(&commit, id, &pf, out, sizeof out);
    CHECK(strstr(out, "    subj\n    \n    \n    body\n") != NULL,
         "middle blank: expected TWO separate blank lines preserved, got '%s'", out);

    /* Reverse control: %B is the raw message, completely unaffected. */
    make_fixture_commit(&commit);
    commit.message = (char *)"subj\n\nbody\n\n\n";
    pf.kind = SG_PRETTY_FORMAT;
    pf.user_format = "%B";
    capture(&commit, id, &pf, out, sizeof out);
    CHECK(strcmp(out, "subj\n\nbody\n\n\n") == 0,
         "%%B must return the raw message verbatim, got '%s'", out);
}
int main(void)
{
    test_date_renderings();
    test_unix_timestamp_end_to_end();
    test_human_placeholder_end_to_end();
    test_sanitized_subject_rows();
    test_parent_list_on_merge();
    test_validate_rejects_outside_table();
    test_validate_accepts_every_table_entry();
    test_subject_folding();
    test_short_does_not_fold();
    test_message_block_rendering();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all pretty-format tests passed\n");
    return 0;
}
