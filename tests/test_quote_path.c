#include "sg/quote.h"

#include <stdio.h>
#include <stdlib.h>
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

/* Helper: quotes a single byte c inside "x<c>y" and checks the expected
   escaped form appears verbatim in the result, still bracketed by the
   surrounding literal bytes -- catches an escaper that mishandles adjacent
   bytes, not just the escaped byte in isolation. */
static void check_single_byte(const char *name, unsigned char c, const char *expected_escape)
{
    char path[4];
    char expected[16];
    const char *got;

    path[0] = 'x';
    path[1] = (char)c;
    path[2] = 'y';
    path[3] = '\0';

    snprintf(expected, sizeof(expected), "\"x%sy\"", expected_escape);
    got = sg_quote_path(path);
    CHECK(strcmp(got, expected) == 0, "%s: got %s, want %s", name, got, expected);
}

/* ---- named escapes: each gets its own CHECK so mutation of any one is
   distinguishable from the others ---- */

static void test_named_escapes(void)
{
    check_single_byte("backslash", '\\', "\\\\");
    check_single_byte("dquote", '"', "\\\"");
    check_single_byte("bel", 0x07, "\\a");
    check_single_byte("bs", 0x08, "\\b");
    check_single_byte("tab", 0x09, "\\t");
    check_single_byte("lf", 0x0a, "\\n");
    check_single_byte("vt", 0x0b, "\\v");
    check_single_byte("ff", 0x0c, "\\f");
    check_single_byte("cr", 0x0d, "\\r");
}

/* ---- octal boundaries ---- */

static void test_octal_boundaries(void)
{
    check_single_byte("octal_0x01", 0x01, "\\001");
    check_single_byte("octal_0x06_below_bel", 0x06, "\\006");
    check_single_byte("octal_0x0e_above_cr", 0x0e, "\\016");
    check_single_byte("esc_0x1b", 0x1b, "\\033");
    check_single_byte("octal_0x1f", 0x1f, "\\037");
    check_single_byte("del_0x7f", 0x7f, "\\177");
}

static void test_esc_is_not_e(void)
{
    const char *got = sg_quote_path("\x1b");

    CHECK(strstr(got, "\\e") == NULL, "ESC must not render as \\e (C has no standard \\e), got %s",
          got);
    CHECK(strcmp(got, "\"\\033\"") == 0, "ESC must render as \\033, got %s", got);
}

/* ---- bytes that must NOT trigger quoting or escaping ---- */

static void test_space_not_escaped(void)
{
    const char *got = sg_quote_path("has space.txt");

    CHECK(strcmp(got, "has space.txt") == 0, "space must not be escaped or trigger quoting, got %s",
          got);
}

static void test_shell_metachars_not_escaped(void)
{
    static const char *metachars[] = {"~", "!", "$", "*", "?", "'", "#", ";",
                                       "|", "&", "(", ")", "<", ">", "[", "]"};
    size_t i;

    for (i = 0; i < sizeof(metachars) / sizeof(metachars[0]); i++) {
        char path[8];
        const char *got;

        snprintf(path, sizeof(path), "x%sy", metachars[i]);
        got = sg_quote_path(path);
        CHECK(strcmp(got, path) == 0,
              "shell metachar %s must not be escaped or quoted (not a git criterion), got %s",
              metachars[i], got);
    }
}

static void test_tilde_0x7e_not_escaped(void)
{
    const char *got = sg_quote_path("x~y");

    CHECK(strcmp(got, "x~y") == 0, "0x7e (~) must not be escaped, got %s", got);
}

static void test_high_bytes_not_escaped(void)
{
    const char *got;

    got = sg_quote_path("x\x80y");
    CHECK(strcmp(got, "x\x80y") == 0, "0x80 must be printed as-is, got %s", got);

    got = sg_quote_path("x\xffy");
    CHECK(strcmp(got, "x\xffy") == 0, "0xff must be printed as-is, got %s", got);

    got = sg_quote_path("\xe4\xb8\xad\xe6\x96\x87.txt"); /* UTF-8 Chinese */
    CHECK(strcmp(got, "\xe4\xb8\xad\xe6\x96\x87.txt") == 0,
          "UTF-8 (Chinese) path must be printed as-is, got %s", got);
}

/* ---- structural cases ---- */

static void test_octal_followed_by_digit(void)
{
    /* 0x01 followed by the ASCII digit '7' must round-trip as "\0017", not
       "\17" (which would re-parse as a different single escape). Nails
       down that octal is zero-padded to a fixed three digits. */
    char path[3];
    const char *got;

    path[0] = 0x01;
    path[1] = '7';
    path[2] = '\0';

    got = sg_quote_path(path);
    CHECK(strcmp(got, "\"\\0017\"") == 0, "octal must be zero-padded 3 digits, got %s", got);
}

static void test_long_control_path(void)
{
    /* 4096 bytes of 0x01: expected length is 4*4096 (each byte -> \001) + 2
       quotes + NUL. This is the unit-level nail for the strbuf_append_path
       truncation the caller-side fix addresses (not exercised here). */
    char path[4097];
    const char *got;
    size_t len;

    memset(path, 0x01, 4096);
    path[4096] = '\0';

    got = sg_quote_path(path);
    len = strlen(got);
    CHECK(len == 4 * 4096 + 2, "expected length %zu, got %zu", (size_t)(4 * 4096 + 2), len);
    CHECK(strcmp(got + len - 3, "01\"") == 0, "expected trailing '01\"', got %s", got + len - 3);
}

/* ---- prefixed / delimited ---- */

static void test_prefixed_quoting_needed(void)
{
    const char *got = sg_quote_path_prefixed("a/", "x\ty");

    CHECK(strcmp(got, "\"a/x\\ty\"") == 0, "prefix must be inside quotes when path needs quoting, got %s",
          got);
}

static void test_prefixed_no_quoting_needed(void)
{
    const char *got = sg_quote_path_prefixed("a/", "x");

    CHECK(strcmp(got, "a/x") == 0, "prefix must be plain, unquoted when path needs no quoting, got %s",
          got);
}

static void test_delimited_always_quotes(void)
{
    const char *got;

    got = sg_quote_path_delimited("plain");
    CHECK(strcmp(got, "\"plain\"") == 0, "delimited must quote even a plain path, got %s", got);

    got = sg_quote_path_delimited("a b");
    CHECK(strcmp(got, "\"a b\"") == 0, "delimited must quote a path with a space, got %s", got);
}

/* ---- ring buffer contract ---- */

static void test_ring_two_consecutive_calls_differ(void)
{
    const char *a = sg_quote_path("alpha");
    const char *b = sg_quote_path("beta");

    CHECK(a != b, "two consecutive calls must return distinct pointers");
    CHECK(strcmp(a, "alpha") == 0, "first call's content must still be intact, got %s", a);
    CHECK(strcmp(b, "beta") == 0, "second call's content must be correct, got %s", b);
}

static void test_ring_recycles_after_slots_exhausted(void)
{
    /* Pin down that the ring *does* recycle after SG_QUOTE_SLOTS + 1 calls
       -- i.e. the first slot's content is overwritten, rather than assuming
       (and silently relying on) unbounded lifetime. */
    const char *first = sg_quote_path("slot0");
    char first_copy[16];
    int i;
    const char *last;

    strncpy(first_copy, first, sizeof(first_copy) - 1);
    first_copy[sizeof(first_copy) - 1] = '\0';

    for (i = 0; i < SG_QUOTE_SLOTS; i++) {
        char path[16];

        snprintf(path, sizeof(path), "slot%d", i + 1);
        last = sg_quote_path(path);
        (void)last;
    }

    /* `first` now points at a slot that has been reused SG_QUOTE_SLOTS + 1
       calls later; its content must no longer equal what it held. */
    CHECK(strcmp(first, first_copy) != 0,
          "slot must have been recycled after SG_QUOTE_SLOTS + 1 calls, still reads %s", first);
}

/* sg_quote_path and sg_quote_path_porcelain must DIVERGE on a plain space:
   the porcelain "XY path" layout needs the space delimited even though it
   needs no C-style escaping, while the long-format listing (one path per
   indented line) must not quote it -- matching git measured 2.55.0. Only
   asserting porcelain's own behavior would miss an implementation that
   quotes spaces unconditionally in both functions; asserting the two
   disagree on the identical input is what actually pins the divergence. */
static void test_porcelain_quotes_space_long_format_does_not(void)
{
    const char *plain = sg_quote_path("has space.txt");
    const char *porc;

    CHECK(strcmp(plain, "has space.txt") == 0,
         "sg_quote_path must not quote a plain space, got %s", plain);

    porc = sg_quote_path_porcelain("has space.txt");
    CHECK(strcmp(porc, "\"has space.txt\"") == 0,
         "sg_quote_path_porcelain must quote a path containing a space, got %s", porc);
}

/* On a control character, both functions must agree: quoted and escaped
   identically. This is the "consistency" half of the porcelain-vs-long
   divergence -- porcelain does not get a DIFFERENT escaping rule, only an
   extra trigger (space) for when to apply the same one. */
static void test_porcelain_and_long_format_agree_on_control_chars(void)
{
    const char *plain = sg_quote_path("ctl\tname.txt");
    const char *porc = sg_quote_path_porcelain("ctl\tname.txt");

    CHECK(strcmp(plain, "\"ctl\\tname.txt\"") == 0, "sg_quote_path control-char case: got %s",
         plain);
    CHECK(strcmp(porc, "\"ctl\\tname.txt\"") == 0,
         "sg_quote_path_porcelain control-char case: got %s", porc);
}

/* A path needing neither escaping nor porcelain's extra space trigger must
   come back completely unquoted from sg_quote_path_porcelain too -- the
   common case must not regress to always-quote. */
static void test_porcelain_no_quoting_needed(void)
{
    const char *got = sg_quote_path_porcelain("plain.txt");

    CHECK(strcmp(got, "plain.txt") == 0, "expected unquoted 'plain.txt', got %s", got);
}

int main(void)
{
    test_named_escapes();
    test_octal_boundaries();
    test_esc_is_not_e();
    test_space_not_escaped();
    test_shell_metachars_not_escaped();
    test_tilde_0x7e_not_escaped();
    test_high_bytes_not_escaped();
    test_octal_followed_by_digit();
    test_long_control_path();
    test_prefixed_quoting_needed();
    test_prefixed_no_quoting_needed();
    test_delimited_always_quotes();
    test_ring_two_consecutive_calls_differ();
    test_ring_recycles_after_slots_exhausted();
    test_porcelain_quotes_space_long_format_does_not();
    test_porcelain_and_long_format_agree_on_control_chars();
    test_porcelain_no_quoting_needed();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all quote_path tests passed\n");
    return 0;
}
