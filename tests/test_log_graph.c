#include "sg/log_graph.h"

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

static void test_empty_input_produces_no_output(void)
{
    sg_log_graph_prefixer pfx;
    char *buf = NULL;
    size_t len = 0;
    FILE *m = open_memstream(&buf, &len);

    sg_log_graph_init(&pfx);
    sg_log_graph_begin_entry(&pfx);
    sg_log_graph_write(&pfx, "", 0, "| ", m);
    fclose(m);

    CHECK(len == 0, "expected no output for an empty write, got %zu bytes: %.*s", len, (int)len, buf);
    free(buf);
}

static void test_single_line_no_trailing_newline(void)
{
    sg_log_graph_prefixer pfx;
    char *buf = NULL;
    size_t len = 0;
    FILE *m = open_memstream(&buf, &len);
    static const char in[] = "hello";

    sg_log_graph_init(&pfx);
    sg_log_graph_begin_entry(&pfx);
    sg_log_graph_write(&pfx, in, sizeof(in) - 1, "| ", m);
    fclose(m);

    CHECK(len == strlen("* hello") && memcmp(buf, "* hello", len) == 0,
          "expected '* hello' with no trailing newline, got %zu bytes: %.*s", len, (int)len, buf);
    free(buf);
}

static void test_two_lines_first_star_second_bar(void)
{
    sg_log_graph_prefixer pfx;
    char *buf = NULL;
    size_t len = 0;
    FILE *m = open_memstream(&buf, &len);
    static const char in[] = "line1\nline2\n";
    static const char expect[] = "* line1\n| line2\n";

    sg_log_graph_init(&pfx);
    sg_log_graph_begin_entry(&pfx);
    sg_log_graph_write(&pfx, in, sizeof(in) - 1, "| ", m);
    fclose(m);

    CHECK(len == sizeof(expect) - 1 && memcmp(buf, expect, len) == 0,
          "expected %s, got %zu bytes: %.*s", expect, len, (int)len, buf);
    free(buf);
}

/* A formerly-blank line becomes "| " -- WITH the trailing space. Rendering
   (a terminal, `sed`) hides this byte; the assertion must check it
   explicitly, not by eye. */
static void test_blank_line_becomes_bar_space_with_trailing_space(void)
{
    sg_log_graph_prefixer pfx;
    char *buf = NULL;
    size_t len = 0;
    FILE *m = open_memstream(&buf, &len);
    static const char in[] = "subject\n\nbody\n";
    static const char expect[] = "* subject\n| \n| body\n";

    sg_log_graph_init(&pfx);
    sg_log_graph_begin_entry(&pfx);
    sg_log_graph_write(&pfx, in, sizeof(in) - 1, "| ", m);
    fclose(m);

    CHECK(len == sizeof(expect) - 1 && memcmp(buf, expect, len) == 0,
          "expected %s (note the trailing space on the blank line), got %zu bytes: %.*s", expect,
          len, (int)len, buf);
    free(buf);
}

static void test_second_entry_first_line_is_star(void)
{
    sg_log_graph_prefixer pfx;
    char *buf = NULL;
    size_t len = 0;
    FILE *m = open_memstream(&buf, &len);
    static const char e1[] = "e1line1\ne1line2\n";
    static const char sep[] = "\n";
    static const char e2[] = "e2line1\ne2line2\n";
    static const char expect[] = "* e1line1\n| e1line2\n| \n* e2line1\n| e2line2\n";

    sg_log_graph_init(&pfx);
    sg_log_graph_begin_entry(&pfx);
    sg_log_graph_write(&pfx, e1, sizeof(e1) - 1, "| ", m);
    sg_log_graph_write(&pfx, sep, sizeof(sep) - 1, "| ", m);
    sg_log_graph_begin_entry(&pfx);
    sg_log_graph_write(&pfx, e2, sizeof(e2) - 1, "| ", m);
    fclose(m);

    CHECK(len == sizeof(expect) - 1 && memcmp(buf, expect, len) == 0,
          "expected %s, got %zu bytes: %.*s", expect, len, (int)len, buf);
    free(buf);
}

/* Phase 63 section 0.3: a FORMAT-kind entry's separator lands MID-LINE
   (the entry itself never terminates with its own '\n'), so it must
   consume no prefix at all -- this is the same prefixer, no special case
   for it, and this test is what proves that. */
static void test_midline_separator_gets_no_prefix(void)
{
    sg_log_graph_prefixer pfx;
    char *buf = NULL;
    size_t len = 0;
    FILE *m = open_memstream(&buf, &len);
    static const char e1[] = "abc123 subject one";
    static const char sep[] = "\n";
    static const char e2[] = "def456 subject two";
    static const char expect[] = "* abc123 subject one\n* def456 subject two";

    sg_log_graph_init(&pfx);
    sg_log_graph_begin_entry(&pfx);
    sg_log_graph_write(&pfx, e1, sizeof(e1) - 1, "| ", m);
    sg_log_graph_write(&pfx, sep, sizeof(sep) - 1, "| ", m);
    sg_log_graph_begin_entry(&pfx);
    sg_log_graph_write(&pfx, e2, sizeof(e2) - 1, "| ", m);
    fclose(m);

    CHECK(len == sizeof(expect) - 1 && memcmp(buf, expect, len) == 0,
          "expected %s, got %zu bytes: %.*s", expect, len, (int)len, buf);
    free(buf);
}

/* Phase 63 section 0.1a: the caller-chosen continuation string is what
   distinguishes a naturally-ended walk's last entry ("  ", two spaces,
   same width as "| ") from every other entry ("| "). This is the one
   property that --oneline/format:/tformat:/reference cannot exercise at
   all (they have no continuation lines) -- this test uses a multi-line
   body specifically so it can. */
static void test_continuation_string_is_caller_controlled(void)
{
    sg_log_graph_prefixer pfx;
    char *buf = NULL;
    size_t len = 0;
    FILE *m = open_memstream(&buf, &len);
    static const char in[] = "commit abc\nAuthor: x\n\n    subject\n";
    static const char expect[] = "* commit abc\n  Author: x\n  \n      subject\n";

    sg_log_graph_init(&pfx);
    sg_log_graph_begin_entry(&pfx);
    sg_log_graph_write(&pfx, in, sizeof(in) - 1, "  ", m);
    fclose(m);

    CHECK(len == sizeof(expect) - 1 && memcmp(buf, expect, len) == 0,
          "expected %s (continuation lines use the caller's \"  \"), got %zu bytes: %.*s", expect,
          len, (int)len, buf);
    free(buf);
}

/* Phase 63 review round, Bug A: a MIDDLE entry whose captured bytes are
   empty (the real-world trigger is --pretty=format:%b on a body-less
   commit) must still emit its own "* " marker. sg_log_graph_write's byte
   loop is a no-op for len == 0 (by design -- it must not invent a prefix
   for a legitimate zero-length separator write), so it cannot emit that
   marker on its own; sg_log_graph_write_entry is the function that adds
   the len == 0 special case, and this test is what the first version of
   this file was missing -- none of the seven original cases ever chained
   a len == 0 write into a following write, which is exactly the shape
   that exposed the bug (a middle empty entry's marker was silently
   "absorbed" by the following separator, invisible unless that entry
   happened to be the LAST one with nothing left to absorb it). */
/* NOT a witness for Bug A, and the name says so on purpose (measured with a
   directed mutation that disabled the len == 0 marker: this case stayed
   GREEN). In the MIDDLE of a run, the empty entry's own "* " and the "* "
   the following separator would wrongly claim land on the same byte offset,
   so both the correct and the buggy implementation emit "* \n* second" --
   the bytes coincide and no assertion here can tell them apart. What this
   case does pin is that the composition (empty entry, separator, entry)
   produces exactly those bytes at all.
   test_entry_write_emits_marker_for_empty_last_entry is the real witness:
   with nothing following, there is no separator left to borrow a marker
   from, so the buggy version emits zero bytes and it fails by name. */
static void test_empty_middle_entry_composes_but_cannot_witness_bug_a(void)
{
    sg_log_graph_prefixer pfx;
    char *buf = NULL;
    size_t len = 0;
    FILE *m = open_memstream(&buf, &len);
    static const char sep[] = "\n";
    static const char e2[] = "second";
    static const char expect[] = "* \n* second";

    sg_log_graph_init(&pfx);
    sg_log_graph_write_entry(&pfx, "", 0, "| ", m);
    sg_log_graph_write(&pfx, sep, sizeof(sep) - 1, "| ", m);
    sg_log_graph_write_entry(&pfx, e2, sizeof(e2) - 1, "| ", m);
    fclose(m);

    CHECK(len == sizeof(expect) - 1 && memcmp(buf, expect, len) == 0,
          "expected %s (an empty entry followed by a separator and a second entry "
          "composes to exactly these bytes), got %zu bytes: %.*s",
          expect, len, (int)len, buf);
    free(buf);
}

/* The other half of the same bug: an empty entry with NOTHING following it
   at all (the shape a run of empty entries hits on its very last one, since
   there is no following separator left to "borrow" a marker from). */
static void test_entry_write_emits_marker_for_empty_last_entry(void)
{
    sg_log_graph_prefixer pfx;
    char *buf = NULL;
    size_t len = 0;
    FILE *m = open_memstream(&buf, &len);
    static const char expect[] = "* ";

    sg_log_graph_init(&pfx);
    sg_log_graph_write_entry(&pfx, "", 0, "  ", m);
    fclose(m);

    CHECK(len == sizeof(expect) - 1 && memcmp(buf, expect, len) == 0,
          "expected a bare \"* \" marker for an empty entry with nothing after it, got %zu bytes: %.*s",
          len, (int)len, buf);
    free(buf);
}

int main(void)
{
    test_empty_input_produces_no_output();
    test_single_line_no_trailing_newline();
    test_two_lines_first_star_second_bar();
    test_blank_line_becomes_bar_space_with_trailing_space();
    test_second_entry_first_line_is_star();
    test_midline_separator_gets_no_prefix();
    test_empty_middle_entry_composes_but_cannot_witness_bug_a();
    test_entry_write_emits_marker_for_empty_last_entry();
    test_continuation_string_is_caller_controlled();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all log_graph tests passed\n");
    return 0;
}
