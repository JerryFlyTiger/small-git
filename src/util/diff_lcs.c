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

int sg_diff_lines_equal_exact(sg_diff_line a, sg_diff_line b)
{
    return a.has_nl == b.has_nl && sg_diff_lines_equal(a, b);
}

static size_t **lcs_table_ex(const sg_diff_line *a, size_t na, const sg_diff_line *b, size_t nb, int exact)
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
            } else if (exact ? sg_diff_lines_equal_exact(a[i], b[j]) : sg_diff_lines_equal(a[i], b[j])) {
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

size_t **sg_diff_lcs_table(const sg_diff_line *a, size_t na, const sg_diff_line *b, size_t nb)
{
    return lcs_table_ex(a, na, b, nb, 0);
}

size_t **sg_diff_lcs_table_exact(const sg_diff_line *a, size_t na, const sg_diff_line *b, size_t nb)
{
    return lcs_table_ex(a, na, b, nb, 1);
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

/* ---- minimal edit script: backtrack + group compaction + indent heuristic */

typedef struct {
    sg_diff_group *groups;
    size_t count;
    size_t cap;
} group_builder;

static int gb_push(group_builder *gb, size_t a_off, size_t a_len, size_t b_off, size_t b_len)
{
    if (gb->count == gb->cap) {
        size_t new_cap = gb->cap == 0 ? 8 : gb->cap * 2;
        sg_diff_group *grown = realloc(gb->groups, new_cap * sizeof(*grown));

        if (grown == NULL)
            return -1;
        gb->groups = grown;
        gb->cap = new_cap;
    }
    gb->groups[gb->count].a_off = a_off;
    gb->groups[gb->count].a_len = a_len;
    gb->groups[gb->count].b_off = b_off;
    gb->groups[gb->count].b_len = b_len;
    gb->count++;
    return 0;
}

/* Walks the LCS table exactly like the single-step classifier every caller
   of this module used to hand-roll (count_lines in diff_out.c, the old
   print_text_diff_body), but folds consecutive non-equal steps into one
   sg_diff_group instead of acting on each line immediately. Returns -1 only
   on allocation failure. */
static int backtrack_into_groups(const sg_diff_line *a, size_t na, const sg_diff_line *b, size_t nb,
                                 size_t **dp, group_builder *gb)
{
    size_t i = 0, j = 0;
    int in_group = 0;
    size_t gstart_a = 0, gstart_b = 0;

    while (i < na || j < nb) {
        if (i < na && j < nb && sg_diff_lines_equal_exact(a[i], b[j])) {
            if (in_group) {
                if (gb_push(gb, gstart_a, i - gstart_a, gstart_b, j - gstart_b) != 0)
                    return -1;
                in_group = 0;
            }
            i++;
            j++;
            continue;
        }
        if (!in_group) {
            gstart_a = i;
            gstart_b = j;
            in_group = 1;
        }
        if (i < na && (j >= nb || dp[i + 1][j] >= dp[i][j + 1]))
            i++;
        else
            j++;
    }
    if (in_group) {
        if (gb_push(gb, gstart_a, i - gstart_a, gstart_b, j - gstart_b) != 0)
            return -1;
    }
    return 0;
}

/* ---- indentation heuristic (git's diff.indentHeuristic, reconstructed) --

   This is a best-effort reproduction from memory of git's xdiffi.c scoring
   (measure_split / score_add_split), NOT a verified transcription of git's
   source. The weight constants below are marked as such; tests/fuzz_diff.py
   -- which compares sg's actual output against real git's -- is the sole
   judge of whether they are close enough. If the fuzzer disagrees with this
   file, the fuzzer is right and this file is wrong. */

#define SG_INDENT_MAX 200      /* UNVERIFIED: cap on the indent score itself */
#define SG_INDENT_MAX_BLANKS 20 /* UNVERIFIED: how far to look past blank lines */

#define SG_INDENT_START_OF_FILE_PENALTY 1
#define SG_INDENT_END_OF_FILE_PENALTY 21
#define SG_INDENT_TOTAL_BLANK_WEIGHT (-30)
#define SG_INDENT_POST_BLANK_WEIGHT 6
/* Was -4 (a bonus) from memory; flipped to a small penalty after
   tests/fuzz_diff.py disagreed with CLAUDE.md's A2 anchor (a duplicate
   block inserted with no blank-line separator) -- see this file's own
   history / the Phase 26 implementation note for the reproduction. */
#define SG_INDENT_RELATIVE_INDENT_PENALTY 4
#define SG_INDENT_RELATIVE_INDENT_WITH_BLANK_PENALTY 10
#define SG_INDENT_RELATIVE_OUTDENT_PENALTY 24
#define SG_INDENT_RELATIVE_OUTDENT_WITH_BLANK_PENALTY 17
#define SG_INDENT_RELATIVE_DEDENT_PENALTY 23
#define SG_INDENT_RELATIVE_DEDENT_WITH_BLANK_PENALTY 17

/* Verified against git's xdiff/xdiffi.c (score_cmp): the comparison between
   two candidate split positions is NOT the sum of the two penalty scalars
   alone. It is dominated by "effective_indent" -- the sum, over the two
   splits bracketing the group, of each split's indent (or its post_indent
   when the split itself falls on a blank/EOF line) -- weighted by
   SG_INDENT_WEIGHT, with `penalty` only used to break ties on
   effective_indent. Omitting effective_indent entirely (an earlier version
   of this file did) picks the wrong position whenever the two candidates
   differ in indentation, which given INDENT_WEIGHT's size is most of the
   time -- confirmed against git 2.55.0 via tests/fuzz_diff.py. */
#define SG_INDENT_WEIGHT 60

/* -1 for a blank (all-whitespace, including empty) line; otherwise the
   leading-whitespace width (space = 1, tab = round up to next multiple of
   8), capped at SG_INDENT_MAX. */
static int line_indent(sg_diff_line ln)
{
    size_t i;
    int indent = 0;

    for (i = 0; i < ln.len; i++) {
        char c = ln.ptr[i];

        if (c == ' ')
            indent += 1;
        else if (c == '\t')
            indent += 8 - indent % 8;
        else if (c == '\v' || c == '\f' || c == '\r')
            continue;
        else
            return indent;
        if (indent >= SG_INDENT_MAX)
            return SG_INDENT_MAX;
    }
    return -1;
}

typedef struct {
    int end_of_file;
    int indent;
    long pre_blank;
    int pre_indent;
    long post_blank;
    int post_indent;
} split_measurement;

static void measure_split(const sg_diff_line *rec, size_t n, size_t split, split_measurement *m)
{
    size_t i;

    if (split >= n) {
        m->end_of_file = 1;
        m->indent = -1;
    } else {
        m->end_of_file = 0;
        m->indent = line_indent(rec[split]);
    }

    m->pre_blank = 0;
    m->pre_indent = -1;
    for (i = split; i-- > 0;) {
        m->pre_indent = line_indent(rec[i]);
        if (m->pre_indent != -1)
            break;
        m->pre_blank++;
        if (m->pre_blank == SG_INDENT_MAX_BLANKS) {
            m->pre_indent = 0;
            break;
        }
    }

    m->post_blank = 0;
    m->post_indent = -1;
    for (i = split + 1; i < n; i++) {
        m->post_indent = line_indent(rec[i]);
        if (m->post_indent != -1)
            break;
        m->post_blank++;
        if (m->post_blank == SG_INDENT_MAX_BLANKS) {
            m->post_indent = 0;
            break;
        }
    }
}

typedef struct {
    int effective_indent;
    int penalty;
} split_score;

/* Mirrors git's score_add_split(): ACCUMULATES into *s rather than returning
   a fresh value, because a position's score is the sum of the two splits
   that bracket the group (see score_position below), not either split in
   isolation. */
static void score_add_split(const sg_diff_line *rec, size_t n, size_t split, split_score *s)
{
    split_measurement m;
    int post_blank, total_blank, indent, any_blanks;

    measure_split(rec, n, split, &m);

    if (m.pre_indent == -1 && m.pre_blank == 0)
        s->penalty += SG_INDENT_START_OF_FILE_PENALTY;
    if (m.end_of_file)
        s->penalty += SG_INDENT_END_OF_FILE_PENALTY;

    post_blank = (m.indent == -1) ? (int)(1 + m.post_blank) : 0;
    total_blank = (int)m.pre_blank + post_blank;

    s->penalty += SG_INDENT_TOTAL_BLANK_WEIGHT * total_blank;
    s->penalty += SG_INDENT_POST_BLANK_WEIGHT * post_blank;

    indent = (m.indent != -1) ? m.indent : m.post_indent;
    any_blanks = (total_blank != 0);

    s->effective_indent += indent;

    if (indent == -1 || m.pre_indent == -1) {
        /* no adjustment */
    } else if (indent > m.pre_indent) {
        s->penalty += any_blanks ? SG_INDENT_RELATIVE_INDENT_WITH_BLANK_PENALTY : SG_INDENT_RELATIVE_INDENT_PENALTY;
    } else if (indent == m.pre_indent) {
        /* no adjustment */
    } else if (m.post_indent != -1 && m.post_indent > indent) {
        s->penalty += any_blanks ? SG_INDENT_RELATIVE_OUTDENT_WITH_BLANK_PENALTY : SG_INDENT_RELATIVE_OUTDENT_PENALTY;
    } else {
        s->penalty += any_blanks ? SG_INDENT_RELATIVE_DEDENT_WITH_BLANK_PENALTY : SG_INDENT_RELATIVE_DEDENT_PENALTY;
    }
}

/* Mirrors git's score_cmp(): effective_indent (weighted) dominates, penalty
   only breaks ties. Negative means s1 is the better (more favored) split. */
static int score_cmp(const split_score *s1, const split_score *s2)
{
    int cmp_indents = (s1->effective_indent > s2->effective_indent) - (s1->effective_indent < s2->effective_indent);

    return SG_INDENT_WEIGHT * cmp_indents + (s1->penalty - s2->penalty);
}

/* Scores candidate position p (an index into the group's OWN axis: a_off
   for a pure delete, b_off for a pure insert) by summing the two splits
   just above and just below the group's content in that axis's line array. */
static split_score score_position(const sg_diff_line *rec, size_t n, size_t pos, size_t len)
{
    split_score s = {0, 0};

    score_add_split(rec, n, pos + len, &s);
    score_add_split(rec, n, pos, &s);
    return s;
}

/* ---- per-file group sliding (mirrors git's struct xdlgroup / group_*) --

   Unlike the paired sg_diff_group the backtrack produces, sliding operates
   on ONE file's own "changed" bitmap at a time: changed[i] is true iff
   line i of THIS file was touched by some edit (deleted, for the a-side;
   inserted, for the b-side -- a "replace" simply means both sides have a
   changed run at once, with no requirement that the runs line up). A
   group is a maximal run of changed lines, or the (possibly empty) gap
   between two runs -- there is one such gap immediately before every
   changed[i]==false position, including one at index 0 and one at n, so
   group_next/group_previous can always find a next/previous group by
   walking exactly one context line at a time.

   `changed` must point at a buffer of n+2 bytes offset by 1 (index -1 and
   index n both readable and zero) -- see sg_diff_build_script, which owns
   the allocation. start/end are `long`, not size_t, purely so start-1 and
   end-1 can be formed and compared without wraparound; git's own xdlgroup
   uses the same signed-index trick for the same reason. */
typedef struct {
    long start, end;
} slide_group;

static void group_init(const unsigned char *changed, slide_group *g)
{
    g->start = g->end = 0;
    while (changed[g->end])
        g->end++;
}

static int group_next(const unsigned char *changed, long n, slide_group *g)
{
    if (g->end == n)
        return -1;
    g->start = g->end + 1;
    for (g->end = g->start; changed[g->end]; g->end++)
        ;
    return 0;
}

static int group_previous(const unsigned char *changed, slide_group *g)
{
    if (g->start == 0)
        return -1;
    g->end = g->start - 1;
    for (g->start = g->end; changed[g->start - 1]; g->start--)
        ;
    return 0;
}

/* If g can be slid one line toward the end of its own file, do so (merging
   into whatever changed run it bumps into). Only valid within THIS file:
   the line released (rec[g->start]) and the line absorbed (rec[g->end])
   must be equal, because releasing/absorbing swaps which of two
   identical-content lines plays "changed" vs "context" -- it never changes
   what the diff represents. */
static int group_slide_down(const sg_diff_line *rec, long n, unsigned char *changed, slide_group *g)
{
    if (g->end < n && sg_diff_lines_equal_exact(rec[g->start], rec[g->end])) {
        changed[g->start++] = 0;
        changed[g->end++] = 1;
        while (changed[g->end])
            g->end++;
        return 0;
    }
    return -1;
}

static int group_slide_up(const sg_diff_line *rec, unsigned char *changed, slide_group *g)
{
    if (g->start > 0 && sg_diff_lines_equal_exact(rec[g->start - 1], rec[g->end - 1])) {
        changed[--g->start] = 1;
        changed[--g->end] = 0;
        while (changed[g->start - 1])
            g->start--;
        return 0;
    }
    return -1;
}

#define SG_INDENT_HEURISTIC_MAX_SLIDING 100 /* verified: xdiffi.c's own constant, same name */

/* Mirrors git's xdl_change_compact(): slides every maximal changed run in
   `changed` (this file's own bitmap, scored against `rec`/`n`, this file's
   own lines) as far up and down as group_slide_up/down allow, merging same
   -file neighbours it bumps into along the way. `other_changed`/`on` is the
   OTHER file's bitmap -- git's xdf1/xdf2 pairing guarantees the two files
   have the same number of unchanged (context) lines in the same relative
   order, so walking group_next/group_previous on both in lockstep visits
   the "same" gap index in each; that is used ONLY as a tie-breaker: once a
   group has been slid to its full up+down reachable range, prefer whatever
   position (if any) still has a changed run in the other file at the same
   gap (avoiding splitting what was really one edit into a separate
   delete-then-add), and only fall back to the indent heuristic when no
   such alignment exists anywhere in range. */
static void compact_one_side(const sg_diff_line *rec, long n, unsigned char *changed,
                             const unsigned char *other_changed, long on, int indent_heuristic)
{
    slide_group g, go;

    (void)on;
    group_init(changed, &g);
    group_init(other_changed, &go);

    for (;;) {
        long earliest_end, end_matching_other, groupsize;

        if (g.end == g.start)
            goto next;

        do {
            groupsize = g.end - g.start;
            end_matching_other = -1;

            while (group_slide_up(rec, changed, &g) == 0)
                group_previous(other_changed, &go);

            earliest_end = g.end;
            if (go.end > go.start)
                end_matching_other = g.end;

            for (;;) {
                if (group_slide_down(rec, n, changed, &g) != 0)
                    break;
                group_next(other_changed, on, &go);
                if (go.end > go.start)
                    end_matching_other = g.end;
            }
        } while (groupsize != g.end - g.start);

        if (g.end == earliest_end) {
            /* fully unmovable -- nothing to decide */
        } else if (end_matching_other != -1) {
            while (go.end == go.start) {
                group_slide_up(rec, changed, &g);
                group_previous(other_changed, &go);
            }
        } else if (indent_heuristic) {
            long shift, best_shift = -1;
            split_score best_score = {0, 0};

            shift = earliest_end;
            if (g.end - groupsize - 1 > shift)
                shift = g.end - groupsize - 1;
            if (g.end - SG_INDENT_HEURISTIC_MAX_SLIDING > shift)
                shift = g.end - SG_INDENT_HEURISTIC_MAX_SLIDING;
            /* Ascending, <=0 (not <0): among equally-good splits the LAST
               (highest, i.e. furthest down) one wins, matching git and this
               module's own no-heuristic default of sliding to the bottom. */
            for (; shift <= g.end; shift++) {
                split_score score = score_position(rec, (size_t)n, (size_t)(shift - groupsize),
                                                   (size_t)groupsize);

                if (best_shift == -1 || score_cmp(&score, &best_score) <= 0) {
                    best_score = score;
                    best_shift = shift;
                }
            }
            while (g.end > best_shift) {
                group_slide_up(rec, changed, &g);
                group_previous(other_changed, &go);
            }
        }

    next:
        if (group_next(changed, n, &g) != 0)
            break;
        group_next(other_changed, on, &go);
    }
}

/* Rebuilds the final paired sg_diff_group list from the two (now
   compacted) per-file bitmaps. Because achanged/bchanged share the same
   count of unchanged positions in the same order (see compact_one_side's
   comment), walking both index cursors forward together and only
   advancing one past a changed run when THAT file's bitmap says so keeps
   them synchronized: whenever neither is changed at the current pair, the
   two positions are the same matched context line and both advance
   together; whenever either is changed, the full run in each file
   (independently -- possibly zero-length in one of them) becomes one
   sg_diff_group, which is a plain "replace" only when both runs happen to
   be non-empty at the same synchronized position, not a distinguished
   case the caller has to special-case. */
static int build_groups_from_changed(const unsigned char *achanged, size_t na, const unsigned char *bchanged,
                                     size_t nb, group_builder *gb)
{
    size_t i = 0, j = 0;

    while (i < na || j < nb) {
        if ((i < na && achanged[i]) || (j < nb && bchanged[j])) {
            size_t a_off = i, b_off = j;

            while (i < na && achanged[i])
                i++;
            while (j < nb && bchanged[j])
                j++;
            if (gb_push(gb, a_off, i - a_off, b_off, j - b_off) != 0)
                return -1;
        } else {
            i++;
            j++;
        }
    }
    return 0;
}

sg_diff_script *sg_diff_build_script(const sg_diff_line *a, size_t na, const sg_diff_line *b, size_t nb,
                                    int indent_heuristic)
{
    size_t **dp;
    group_builder gb, final_gb;
    sg_diff_script *script;
    unsigned char *achanged_buf, *bchanged_buf, *achanged, *bchanged;
    size_t k;

    dp = sg_diff_lcs_table_exact(a, na, b, nb);
    if (dp == NULL)
        return NULL;

    gb.groups = NULL;
    gb.count = 0;
    gb.cap = 0;
    if (backtrack_into_groups(a, na, b, nb, dp, &gb) != 0) {
        sg_diff_lcs_free_table(dp, na);
        free(gb.groups);
        return NULL;
    }
    sg_diff_lcs_free_table(dp, na);

    /* Two independent per-file "changed" bitmaps, offset by 1 so index -1
       and index n both read as 0 (see compact_one_side/group_*'s own
       comments for why that sentinel matters). Seeded from the backtrack's
       paired groups -- a "replace" group simply marks both bitmaps over
       its own range, with no other special-casing needed from here on. */
    achanged_buf = calloc(na + 2, 1);
    bchanged_buf = calloc(nb + 2, 1);
    if (achanged_buf == NULL || bchanged_buf == NULL) {
        free(achanged_buf);
        free(bchanged_buf);
        free(gb.groups);
        return NULL;
    }
    achanged = achanged_buf + 1;
    bchanged = bchanged_buf + 1;

    for (k = 0; k < gb.count; k++) {
        sg_diff_group *grp = &gb.groups[k];
        size_t t;

        for (t = 0; t < grp->a_len; t++)
            achanged[grp->a_off + t] = 1;
        for (t = 0; t < grp->b_len; t++)
            bchanged[grp->b_off + t] = 1;
    }
    free(gb.groups);

    /* Mirrors git's xdl_build_script call order: xdf1 (a's deletions) is
       compacted first, consulting b's bitmap as it was right after the
       backtrack; THEN xdf2 (b's insertions) is compacted consulting a's
       bitmap as compact_one_side above just left it. Swapping this order
       changes output on any hunk where both sides have room to slide --
       matching git means matching the order, not just the algorithm. */
    compact_one_side(a, (long)na, achanged, bchanged, (long)nb, indent_heuristic);
    compact_one_side(b, (long)nb, bchanged, achanged, (long)na, indent_heuristic);

    final_gb.groups = NULL;
    final_gb.count = 0;
    final_gb.cap = 0;
    if (build_groups_from_changed(achanged, na, bchanged, nb, &final_gb) != 0) {
        free(achanged_buf);
        free(bchanged_buf);
        free(final_gb.groups);
        return NULL;
    }
    free(achanged_buf);
    free(bchanged_buf);

    script = malloc(sizeof(*script));
    if (script == NULL) {
        free(final_gb.groups);
        return NULL;
    }
    script->groups = final_gb.groups;
    script->count = final_gb.count;
    return script;
}

void sg_diff_script_free(sg_diff_script *script)
{
    if (script == NULL)
        return;
    free(script->groups);
    free(script);
}
