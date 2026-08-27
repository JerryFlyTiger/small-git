#include "sg/similarity.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, ...)                                                                         \
    do {                                                                                          \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);                                   \
            fprintf(stderr, __VA_ARGS__);                                                          \
            fprintf(stderr, "\n");                                                                 \
            failures++;                                                                            \
        }                                                                                           \
    } while (0)

/* Every expected number below came from running real git 2.55.0, not from
   reading this implementation back to itself. The scores were reproduced with
   an independent Python port of diffcore-delta.c that was first checked
   against git on 750 random file pairs; the -M grammar was measured directly
   by running `git diff --cached --name-status -M<n>` on a fixture whose score
   was known. An expectation borrowed from the code under test would only ever
   prove the code agrees with itself. */

struct scored {
    const char *src;
    size_t src_len;
    const char *dst;
    size_t dst_len;
    unsigned long copied;
    unsigned long added;
    int score;
};

static void check_pair(const struct scored *c, const char *label)
{
    sg_spanhash *s = sg_spanhash_build((const unsigned char *)c->src, c->src_len);
    sg_spanhash *d = sg_spanhash_build((const unsigned char *)c->dst, c->dst_len);
    unsigned long copied = 12345, added = 12345;
    int score;

    if (s == NULL || d == NULL) {
        fprintf(stderr, "FAIL %s: out of memory building %s\n", __func__, label);
        failures++;
        sg_spanhash_free(s);
        sg_spanhash_free(d);
        return;
    }
    sg_spanhash_count_changes(s, d, &copied, &added);
    score = sg_similarity_score(s, c->src_len, d, c->dst_len);

    CHECK(copied == c->copied, "%s: src_copied is %lu, git says %lu", label, copied, c->copied);
    CHECK(added == c->added, "%s: literal_added is %lu, git says %lu", label, added, c->added);
    CHECK(score == c->score, "%s: score is %d, git says %d", label, score, c->score);

    sg_spanhash_free(s);
    sg_spanhash_free(d);
}

#define LIT(s) (s), (sizeof(s) - 1)

static void test_scores_match_git(void)
{
    /* A whole line survives as a whole line, so identical content is a
       perfect match and the byte counts equal the file size. */
    struct scored identical = {LIT("a\nb\n"), LIT("a\nb\n"), 4, 0, 60000};
    /* Dropping half the lines carries over half the bytes. */
    struct scored halved = {LIT("a\nb\nc\nd\n"), LIT("a\nb\n"), 4, 0, 30000};
    /* Nothing in common: everything the destination has is new. */
    struct scored disjoint = {LIT("aaa\n"), LIT("zzz\n"), 0, 4, 0};
    /* Chunks are counted, never stored, so reordering whole lines is
       invisible to the estimate. git reports these as a 100% match, which is
       a documented consequence of the algorithm and not a bug to fix. */
    struct scored permuted = {LIT("a\nb\n"), LIT("b\na\n"), 4, 0, 60000};
    /* An empty destination is a match of nothing, however large the source. */
    struct scored empty_dst = {LIT("a\nb\n"), LIT(""), 0, 0, 0};
    /* An empty source accounts for none of the destination. */
    struct scored empty_src = {LIT(""), LIT("a\nb\n"), 0, 4, 0};

    check_pair(&identical, "identical");
    check_pair(&halved, "half the lines dropped");
    check_pair(&disjoint, "nothing in common");
    check_pair(&permuted, "lines permuted");
    check_pair(&empty_dst, "empty destination");
    check_pair(&empty_src, "empty source");
}

static void test_a_nul_byte_changes_the_score(void)
{
    /* The pair that pins down why text and binary are told apart at all. The
       CR of a CRLF pair is skipped when hashing text but NOT subtracted from
       the file's size, so a CRLF file matches itself at only 66%. Add a
       single NUL byte and the same bytes are binary, the CRs are hashed like
       any other content, and the very same comparison is a perfect match.

       These two differ by one byte and by 34 points, so a port that gets the
       text/binary decision backwards -- or drops it -- cannot stay green. */
    struct scored crlf_text = {LIT("a\r\nb\r\n"), LIT("a\r\nb\r\n"), 4, 0, 40000};
    struct scored crlf_binary = {LIT("\0a\r\nb\r\n"), LIT("\0a\r\nb\r\n"), 7, 0, 60000};

    check_pair(&crlf_text, "CRLF as text");
    check_pair(&crlf_binary, "the same CRLF bytes, after a NUL");
    CHECK(sg_similarity_percent(40000) == 66, "the text one prints as 66%%");
    CHECK(sg_similarity_percent(60000) == 100, "the binary one prints as 100%%");
}

static void test_size_alone_can_rule_a_pair_out(void)
{
    /* The cheap check that lets git skip reading a blob. At the default 50%
       threshold a file may not have more than half its size in difference. */
    CHECK(!sg_similarity_size_rejects(100, 100, SG_SIMILARITY_DEFAULT),
          "same size is never ruled out on size");
    CHECK(!sg_similarity_size_rejects(100, 50, SG_SIMILARITY_DEFAULT),
          "exactly half is still allowed through");
    CHECK(sg_similarity_size_rejects(100, 49, SG_SIMILARITY_DEFAULT),
          "one byte past half is ruled out");
    /* An empty side is where the guard earns its keep: it disposes of the
       pair before the score's division by size could be reached. */
    CHECK(sg_similarity_size_rejects(100, 0, SG_SIMILARITY_DEFAULT),
          "an empty destination is ruled out on size");
    CHECK(sg_similarity_size_rejects(0, 100, SG_SIMILARITY_DEFAULT),
          "so is an empty source");
    /* The threshold is an input, not a constant: a lax one lets pairs
       through that the default rejects. */
    CHECK(!sg_similarity_size_rejects(100, 49, 600), "-M1%% lets the same pair through");
    /* And the strictest one rejects everything that is not the same size. */
    CHECK(sg_similarity_size_rejects(100, 99, SG_SIMILARITY_MAX),
          "-M100%% rules out any size change at all");
}

static void test_percent_truncates(void)
{
    CHECK(sg_similarity_percent(SG_SIMILARITY_MAX) == 100, "a perfect score is 100%%");
    /* One point short of perfect must not round up to 100: "R100" is how git
       says the content is identical, and this is the whole reason the score
       is kept on git's scale rather than as a percentage. */
    CHECK(sg_similarity_percent(SG_SIMILARITY_MAX - 1) == 99, "one point short is 99%%");
    CHECK(sg_similarity_percent(51600) == 86, "51600 prints as 86%%");
    CHECK(sg_similarity_percent(52199) == 86, "and so does the last point of that band");
    CHECK(sg_similarity_percent(52200) == 87, "the next point over is 87%%");
    CHECK(sg_similarity_percent(0) == 0, "nothing in common is 0%%");
}

static void test_m_grammar_matches_git(void)
{
    /* Measured against git 2.55.0 on a fixture scoring 86%: -M9 and -M90
       find nothing, -M100 finds the rename, -M100% does not. The digits are
       a FRACTION unless a '%' follows -- which is what makes -M100 mean ten
       percent -- and this table is the only place that rule is written down
       as numbers rather than as prose. */
    static const struct {
        const char *arg;
        int want;
        const char *rest;
    } cases[] = {
        {"", 0, ""},              /* nothing given: callers substitute the default */
        {"5", 30000, ""},         /* 0.5 */
        {"05", 3000, ""},         /* 0.05 -- a leading zero is not decoration */
        {"50", 30000, ""},        /* 0.50, the same as -M5 */
        {"9", 54000, ""},         /* 0.9 */
        {"90", 54000, ""},        /* 0.90, the same as -M9 */
        {"100", 6000, ""},        /* 0.100 -- ten percent, NOT exact-only */
        {"100%", 60000, ""},      /* the only spelling of exact-renames-only */
        {"0.5", 30000, ""},       /* the point restarts the scale */
        {"0.5%", 300, ""},        /* half of one percent */
        {"50%", 30000, ""},
        {"86%", 51600, ""},
        {"87%", 52200, ""},
        {"%", 0, ""},             /* no digits: still "use the default" */
        {"1234567", 7407, ""},    /* digits past the fifth are read but ignored */
        {"50x", 30000, "x"},      /* stops at the first thing it cannot use */
        {"abc", 0, "abc"},        /* consumes nothing, so the caller can reject it */
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char *cp = cases[i].arg;
        int got = sg_similarity_parse_score(&cp);

        CHECK(got == cases[i].want, "-M%s is %d, git says %d", cases[i].arg, got,
              cases[i].want);
        CHECK(strcmp(cp, cases[i].rest) == 0, "-M%s leaves \"%s\", expected \"%s\"",
              cases[i].arg, cp, cases[i].rest);
    }
}

static void test_a_chunk_can_be_a_single_byte(void)
{
    /* A chunk ends at a newline OR after 64 bytes, so a file of nothing but
       newlines makes one chunk per byte -- far more than the len/64 a first
       guess at the table size would allow for. This is here because getting
       that bound wrong overruns the table rather than returning a wrong
       number, and a wrong number is the only thing the other tests can see. */
    char buf[500];
    sg_spanhash *h;
    unsigned long copied = 0;

    memset(buf, '\n', sizeof(buf));
    h = sg_spanhash_build((const unsigned char *)buf, sizeof(buf));
    CHECK(h != NULL, "500 newlines hash without running out of room");
    if (h != NULL) {
        sg_spanhash_count_changes(h, h, &copied, NULL);
        CHECK(copied == sizeof(buf), "all %zu bytes are accounted for, got %lu",
              sizeof(buf), copied);
        CHECK(sg_similarity_score(h, sizeof(buf), h, sizeof(buf)) == SG_SIMILARITY_MAX,
              "and the file is a perfect match for itself");
    }
    sg_spanhash_free(h);
}

static void test_a_long_line_is_cut_at_64_bytes(void)
{
    /* The other half of the same rule: no newline at all still produces
       chunks, one per 64 bytes plus a trailing partial one. */
    char buf[200];
    sg_spanhash *h;
    unsigned long copied = 0;

    memset(buf, 'x', sizeof(buf));
    h = sg_spanhash_build((const unsigned char *)buf, sizeof(buf));
    CHECK(h != NULL, "a 200-byte line with no newline hashes");
    if (h != NULL) {
        sg_spanhash_count_changes(h, h, &copied, NULL);
        CHECK(copied == sizeof(buf), "all %zu bytes are accounted for, got %lu",
              sizeof(buf), copied);
    }
    sg_spanhash_free(h);
}

int main(void)
{
    test_scores_match_git();
    test_a_nul_byte_changes_the_score();
    test_size_alone_can_rule_a_pair_out();
    test_percent_truncates();
    test_m_grammar_matches_git();
    test_a_chunk_can_be_a_single_byte();
    test_a_long_line_is_cut_at_64_bytes();

    if (failures > 0) {
        fprintf(stderr, "%d similarity test(s) failed\n", failures);
        return 1;
    }
    printf("all similarity tests passed\n");
    return 0;
}
