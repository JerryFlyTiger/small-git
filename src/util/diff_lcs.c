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

/* Computes the reachable slide range for a pure insert/delete group.

   `pos` is the group's position marker on its own axis (a_off for a pure
   delete group, b_off for a pure insert group -- the axis where the group
   has zero length). `content` is the OTHER array (b for insert, a for
   delete), where the group's actual `len` lines live, starting at
   `content_off` (which is NOT generally equal to `pos` -- the two axes can
   have drifted apart by however much earlier groups added/removed).
   `lower`/`upper` bound the marker axis (from the neighbouring groups, or
   the file ends).

   Sliding down step i (0<=i<down): at marker position pos+i, the context
   line about to be swallowed, pos_file[pos+i], must equal the group's
   current first content line, content[content_off+i] -- i.e. that context
   line becomes the new first content line and the group's old first
   content line becomes the new context line, sliding the window down by
   one. Sliding up step i (0<=i<up): at marker position pos-i, the
   preceding context line, pos_file[pos-i-1], must equal the group's
   current LAST content line, content[content_off-i+len-1] (NOT
   content_off-i-1+len-1 -- an earlier version of this function had that
   off-by-one and it went undetected until tests/fuzz_diff.py caught a
   delete group sliding onto the wrong line; see the Phase 26 note in
   CLAUDE.md's own history of this file for the reproduction). */
static void slide_range(const sg_diff_line *pos_file, size_t pos_n, const sg_diff_line *content,
                        size_t content_n, size_t pos, size_t content_off, size_t len, size_t lower,
                        size_t upper, size_t *low_out, size_t *high_out)
{
    size_t down = 0, up = 0;

    (void)pos_n;
    (void)content_n;
    while (pos + down < upper &&
          sg_diff_lines_equal_exact(pos_file[pos + down], content[content_off + down]))
        down++;
    while (pos > lower + up &&
          sg_diff_lines_equal_exact(pos_file[pos - up - 1], content[content_off - up + len - 1]))
        up++;

    *low_out = pos - up;
    *high_out = pos + down;
}

/* Picks the best position in [low, high] using the indent heuristic,
   ties broken toward `high` (the no-heuristic default). `content` is
   scored (it is the file the group's own lines live in -- see
   slide_range). content_off_at_low is the content-array offset
   corresponding to position `low`. */
static size_t pick_indent_position(const sg_diff_line *content, size_t content_n, size_t low, size_t high,
                                   size_t content_off_at_low, size_t len)
{
    size_t best = low;
    split_score best_score;
    size_t p;
    int best_valid = 0;

    /* Ascending, <=0 (not <0): git's loop walks shift from low to high and
       replaces the best candidate on ties too, so among equally-good splits
       the LAST (highest) one wins -- this is what makes "no heuristic
       preference" default to the bottom of the range. */
    for (p = low; p <= high; p++) {
        size_t content_off = content_off_at_low + (p - low);
        split_score score = score_position(content, content_n, content_off, len);

        if (!best_valid || score_cmp(&score, &best_score) <= 0) {
            best_score = score;
            best = p;
            best_valid = 1;
        }
    }
    return best;
}

sg_diff_script *sg_diff_build_script(const sg_diff_line *a, size_t na, const sg_diff_line *b, size_t nb,
                                    int indent_heuristic)
{
    size_t **dp;
    group_builder gb;
    sg_diff_script *script;
    size_t out, k;

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

    /* Group compaction: slide every pure insert/delete group to the bottom
       of its reachable range (or, with the heuristic, the best-scoring
       position in that range).

       Critically -- mirroring git's xdl_change_compact do-while, which
       slides a group fully up and then fully down, merging in any adjacent
       group it bumps into via group_previous/group_next, and repeats until
       a full up+down pass changes nothing -- the merge decision is made
       BEFORE the heuristic runs, purely from whether sliding reaches all
       the way to a same-kind neighbour, and the heuristic then scores
       across the *whole merged cluster's* range. Deciding placement first
       and merging only if the heuristic happened to land on the boundary
       (an earlier version of this file did that) is backwards: a
       backtrack that matched a duplicated line to the "wrong" (but equally
       optimal) occurrence produces two separate pure groups with a single
       matched line between them that CAN slide away entirely, and scoring
       each group only against its own unmerged range never lets the
       heuristic see the option of treating them as one bigger insert --
       confirmed against git 2.55.0 via tests/fuzz_diff.py (a case where the
       unmerged scoring reliably picked the wrong split because it was
       comparing the wrong two candidate splits).

       "Replace" groups (both a_len and b_len non-zero) are left exactly
       where the backtrack put them and are never merged -- sliding them,
       and merging across the insert/delete boundary, would require
       tracking each side on its own axis the way git's xdf1/xdf2 do, which
       this reconstruction does not attempt (see the module comment). */
    out = 0;
    k = 0;
    while (k < gb.count) {
        sg_diff_group cur = gb.groups[k];
        size_t next_k = k + 1;

        if (cur.a_len > 0 && cur.b_len > 0) {
            gb.groups[out] = cur;
            out++;
            k++;
            continue;
        }

        for (;;) {
            size_t lower_a = (out == 0) ? 0 : gb.groups[out - 1].a_off + gb.groups[out - 1].a_len;
            size_t upper_a = (next_k == gb.count) ? na : gb.groups[next_k].a_off;
            size_t lower_b = (out == 0) ? 0 : gb.groups[out - 1].b_off + gb.groups[out - 1].b_len;
            size_t upper_b = (next_k == gb.count) ? nb : gb.groups[next_k].b_off;
            size_t low, high;
            int merged = 0;

            if (cur.a_len == 0 && cur.b_len > 0)
                slide_range(a, na, b, nb, cur.a_off, cur.b_off, cur.b_len, lower_a, upper_a, &low, &high);
            else
                slide_range(b, nb, a, na, cur.b_off, cur.a_off, cur.a_len, lower_b, upper_b, &low, &high);

            if (out > 0) {
                sg_diff_group *prev = &gb.groups[out - 1];

                if (cur.a_len == 0 && cur.b_len > 0 && prev->a_len == 0 && prev->b_len > 0 && low == lower_a) {
                    cur.a_off = prev->a_off;
                    cur.b_off = prev->b_off;
                    cur.b_len = prev->b_len + cur.b_len;
                    out--;
                    merged = 1;
                } else if (cur.b_len == 0 && cur.a_len > 0 && prev->b_len == 0 && prev->a_len > 0 &&
                           low == lower_b) {
                    cur.b_off = prev->b_off;
                    cur.a_off = prev->a_off;
                    cur.a_len = prev->a_len + cur.a_len;
                    out--;
                    merged = 1;
                }
            }
            if (!merged && next_k < gb.count) {
                sg_diff_group *nxt = &gb.groups[next_k];

                if (cur.a_len == 0 && cur.b_len > 0 && nxt->a_len == 0 && nxt->b_len > 0 && high == upper_a) {
                    cur.b_len = cur.b_len + nxt->b_len;
                    next_k++;
                    merged = 1;
                } else if (cur.b_len == 0 && cur.a_len > 0 && nxt->b_len == 0 && nxt->a_len > 0 &&
                           high == upper_b) {
                    cur.a_len = cur.a_len + nxt->a_len;
                    next_k++;
                    merged = 1;
                }
            }
            if (merged)
                continue;

            /* Stable: [low, high] is the group's full reachable range
               (after absorbing every mergeable same-kind neighbour).
               Now, and only now, pick the final position within it. */
            if (cur.a_len == 0 && cur.b_len > 0) {
                size_t chosen = indent_heuristic
                                    ? pick_indent_position(b, nb, low, high, cur.b_off - (cur.a_off - low),
                                                           cur.b_len)
                                    : high;

                cur.b_off = cur.b_off + (chosen - cur.a_off);
                cur.a_off = chosen;
            } else {
                size_t chosen = indent_heuristic
                                    ? pick_indent_position(a, na, low, high, cur.a_off - (cur.b_off - low),
                                                           cur.a_len)
                                    : high;

                cur.a_off = cur.a_off + (chosen - cur.b_off);
                cur.b_off = chosen;
            }
            break;
        }

        gb.groups[out] = cur;
        out++;
        k = next_k;
    }
    gb.count = out;

    script = malloc(sizeof(*script));
    if (script == NULL) {
        free(gb.groups);
        return NULL;
    }
    script->groups = gb.groups;
    script->count = gb.count;
    return script;
}

void sg_diff_script_free(sg_diff_script *script)
{
    if (script == NULL)
        return;
    free(script->groups);
    free(script);
}
