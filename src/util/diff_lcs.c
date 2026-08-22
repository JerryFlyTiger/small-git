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

static int score_split(const sg_diff_line *rec, size_t n, size_t split)
{
    split_measurement m;
    int penalty = 0;
    int post_blank, total_blank, indent, any_blanks;

    measure_split(rec, n, split, &m);

    if (m.pre_indent == -1 && m.pre_blank == 0)
        penalty += SG_INDENT_START_OF_FILE_PENALTY;
    if (m.end_of_file)
        penalty += SG_INDENT_END_OF_FILE_PENALTY;

    post_blank = (m.indent == -1) ? (int)(1 + m.post_blank) : 0;
    total_blank = (int)m.pre_blank + post_blank;

    penalty += SG_INDENT_TOTAL_BLANK_WEIGHT * total_blank;
    penalty += SG_INDENT_POST_BLANK_WEIGHT * post_blank;

    indent = (m.indent != -1) ? m.indent : m.post_indent;
    any_blanks = (total_blank != 0);

    if (indent == -1 || m.pre_indent == -1) {
        /* no adjustment */
    } else if (indent > m.pre_indent) {
        penalty += any_blanks ? SG_INDENT_RELATIVE_INDENT_WITH_BLANK_PENALTY : SG_INDENT_RELATIVE_INDENT_PENALTY;
    } else if (indent == m.pre_indent) {
        /* no adjustment */
    } else if (m.post_indent != -1 && m.post_indent > indent) {
        penalty += any_blanks ? SG_INDENT_RELATIVE_OUTDENT_WITH_BLANK_PENALTY : SG_INDENT_RELATIVE_OUTDENT_PENALTY;
    } else {
        penalty += any_blanks ? SG_INDENT_RELATIVE_DEDENT_WITH_BLANK_PENALTY : SG_INDENT_RELATIVE_DEDENT_PENALTY;
    }

    return penalty;
}

/* Scores candidate position p (an index into the group's OWN axis: a_off
   for a pure delete, b_off for a pure insert) by summing the split penalty
   just above and just below the group's content in that axis's line array.
   Lower is better -- this mirrors score_split's "penalty" convention. */
static int score_position(const sg_diff_line *rec, size_t n, size_t pos, size_t len)
{
    return score_split(rec, n, pos) + score_split(rec, n, pos + len);
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
    size_t best = high;
    int best_score;
    size_t p;

    best_score = score_position(content, content_n, content_off_at_low + (high - low), len);
    for (p = low; p < high; p++) {
        size_t content_off = content_off_at_low + (p - low);
        int score = score_position(content, content_n, content_off, len);

        if (score < best_score) {
            best_score = score;
            best = p;
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

    /* Group compaction: slide every pure insert/delete group to the bottom
       of its reachable range (or, with the heuristic, the best-scoring
       position in that range). "Replace" groups (both a_len and b_len
       non-zero) are left exactly where the backtrack put them -- sliding
       them is ambiguous in a way this reconstruction does not attempt (see
       sg_diff_group's own comment). */
    for (k = 0; k < gb.count; k++) {
        sg_diff_group *g = &gb.groups[k];
        size_t lower_a = (k == 0) ? 0 : gb.groups[k - 1].a_off + gb.groups[k - 1].a_len;
        size_t upper_a = (k + 1 == gb.count) ? na : gb.groups[k + 1].a_off;
        size_t lower_b = (k == 0) ? 0 : gb.groups[k - 1].b_off + gb.groups[k - 1].b_len;
        size_t upper_b = (k + 1 == gb.count) ? nb : gb.groups[k + 1].b_off;

        if (g->a_len == 0 && g->b_len > 0) {
            /* Pure insert: marker lives in a-space (g->a_off), content in b. */
            size_t low, high, chosen;

            slide_range(a, na, b, nb, g->a_off, g->b_off, g->b_len, lower_a, upper_a, &low, &high);
            chosen = indent_heuristic ? pick_indent_position(b, nb, low, high, g->b_off - (g->a_off - low),
                                                             g->b_len)
                                      : high;
            g->b_off = g->b_off + (chosen - g->a_off);
            g->a_off = chosen;
        } else if (g->b_len == 0 && g->a_len > 0) {
            /* Pure delete: marker lives in b-space (g->b_off), content in a. */
            size_t low, high, chosen;

            slide_range(b, nb, a, na, g->b_off, g->a_off, g->a_len, lower_b, upper_b, &low, &high);
            chosen = indent_heuristic ? pick_indent_position(a, na, low, high, g->a_off - (g->b_off - low),
                                                             g->a_len)
                                      : high;
            g->a_off = g->a_off + (chosen - g->b_off);
            g->b_off = chosen;
        }
    }

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
