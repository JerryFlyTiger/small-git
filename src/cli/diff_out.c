#include "sg/diff_out.h"

#include "sg/chunk.h"
#include "sg/diff.h"
#include "sg/diff_lcs.h"
#include "sg/hash.h"
#include "sg/quote.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* ---- --stat=<width>[,<name-width>] argument parsing ------------------ */

int sg_diff_parse_stat_arg(const char *arg, int *width_out, int *name_width_out)
{
    char *end;
    long width;
    long name_width = 0;

    if (*arg == '\0')
        return -1;
    width = strtol(arg, &end, 10);
    if (width <= 0 || width > SG_DIFF_STAT_ARG_MAX)
        return -1;
    if (*end == ',') {
        const char *nw = end + 1;

        if (*nw == '\0')
            return -1;
        name_width = strtol(nw, &end, 10);
        if (name_width <= 0 || name_width > SG_DIFF_STAT_ARG_MAX)
            return -1;
    }
    if (*end != '\0')
        return -1;

    *width_out = (int)width;
    *name_width_out = (int)name_width;
    return 0;
}

/* ---- small numeric/status helpers ---------------------------------- */

static int digits(long long v)
{
    int w = 1;

    if (v < 0)
        v = -v;
    while (v >= 10) {
        v /= 10;
        w++;
    }
    return w;
}

static char entry_status(const sg_diff_entry *e)
{
    if (e->unmerged)
        return 'U';
    if (e->old_side.kind == SG_DIFF_SIDE_ABSENT)
        return 'A';
    if (e->new_side.kind == SG_DIFF_SIDE_ABSENT)
        return 'D';
    return 'M';
}

static int is_binary_data(const unsigned char *data, size_t len)
{
    return data != NULL && memchr(data, '\0', len) != NULL;
}

/* Counts added/deleted *lines* between the two sides by summing an
   sg_diff_script's groups -- --stat/--numstat only need the totals, not the
   alignment choice, so this deliberately skips group compaction and the
   indent heuristic (indent_heuristic=0): the total count of added/deleted
   lines is identical no matter which reachable position a pure group is
   slid to, so paying for the extra work here would only ever change
   nothing. On allocation failure the counts are left at 0, matching this
   file's general "best effort, never crash" policy for the stat machinery
   (a wrong stat count is far less bad than aborting the whole diff). */
static void count_lines(const unsigned char *a_data, size_t a_len, const unsigned char *b_data,
                        size_t b_len, long *added, long *deleted)
{
    size_t na, nb;
    sg_diff_line *a = sg_diff_split_lines(a_data, a_len, &na);
    sg_diff_line *b = sg_diff_split_lines(b_data, b_len, &nb);
    sg_diff_script *script;
    size_t k;
    long add = 0, del = 0;

    *added = 0;
    *deleted = 0;

    script = sg_diff_build_script(a, na, b, nb, 0);
    if (script == NULL) {
        free(a);
        free(b);
        return;
    }

    for (k = 0; k < script->count; k++) {
        add += (long)script->groups[k].b_len;
        del += (long)script->groups[k].a_len;
    }

    sg_diff_script_free(script);
    free(a);
    free(b);
    *added = add;
    *deleted = del;
}

/* The unified-diff format separates the filename on a ---/+++ line from an
   optional timestamp with whitespace, so a name containing a space is
   ambiguous. Real git disambiguates by appending a TAB, and only on those
   two lines -- measured against 2.55.0: "diff --git" and "Binary files"
   never get one, and neither does a name whose only oddity is a control
   character, because quoting escapes that away while a space is left as-is.
   Returns "\t" or "". (Moved here from cmd_diff.c along with the printer it
   served -- rendering now lives in this file, not the CLI layer.) */
static const char *diff_name_terminator(const char *path)
{
    return strchr(path, ' ') != NULL ? "\t" : "";
}

/* ---- display width -------------------------------------------------- */

typedef struct {
    size_t byte_off;
    size_t byte_len;
    int width;
    unsigned int cp;
} cp_info;

/* East Asian Width, measured against real git 2.55.0 (`git -c
   core.quotepath=false diff --cached --stat` on a fixture with one file per
   candidate codepoint, comparing the padding before "|" across rows that
   share a known-ASCII baseline). Only the ranges actually exercised are
   listed here as width 2; every other codepoint -- including Latin-1
   Supplement letters like e-acute/u-diaeresis, Greek, Cyrillic, arrows and
   box-drawing characters, all of which were ALSO measured and confirmed
   width 1 -- falls through to the default of 1 below. Do not extend this
   table from memory; measure first.

   Measured so far (this list IS the coverage, not a subset of it): Hiragana,
   CJK Unified Ideographs, Hangul syllables, Fullwidth Forms (FF01-FF60),
   CJK Extension A, CJK Compatibility Ideographs, and the FFE0-FFE6
   fullwidth signs block below. Codepoints outside all of these -- e.g.
   combining marks (no filesystem can hold one as its own path component to
   test), CJK Extension B and beyond (astral plane, i.e. need a surrogate
   pair / 4-byte UTF-8 sequence -- not attempted), Hangul Jamo, CJK
   Compatibility Forms -- are UNMEASURED and fall through to width 1 by
   default, not because they were checked and found narrow. */
static int codepoint_width(unsigned int cp)
{
    if (cp < 0x80)
        return 1;
    if (cp >= 0x3040 && cp <= 0x309F) /* Hiragana -- measured via U+3042 (あ) */
        return 2;
    if (cp >= 0x3400 && cp <= 0x4DBF) /* CJK Extension A -- measured via U+3400 (㐀) */
        return 2;
    if (cp >= 0x4E00 && cp <= 0x9FFF) /* CJK Unified Ideographs -- measured via U+4E2D/U+6587 (中/文) */
        return 2;
    if (cp >= 0xAC00 && cp <= 0xD7A3) /* Hangul syllables -- measured via U+AC00 (가) */
        return 2;
    if (cp >= 0xF900 && cp <= 0xFAFF) /* CJK Compatibility Ideographs -- measured via U+F900 (豈) */
        return 2;
    if (cp >= 0xFF01 && cp <= 0xFF60) /* Fullwidth forms -- measured via U+FF21/U+FF01 (Ａ/！) */
        return 2;
    if (cp >= 0xFFE0 && cp <= 0xFFE6) /* fullwidth signs -- measured via U+FFE0/U+FFE5 (￠/￥) */
        return 2;
    return 1;
}

/* Decodes s (assumed valid UTF-8, which is what sg_quote_path produces for
   any byte >= 0x80 since it passes those through unescaped) into an array of
   per-codepoint byte offsets/lengths/display widths. A malformed byte is
   treated as its own one-byte "codepoint" of width 1 rather than rejected --
   this is a rendering helper, not a validator. Returns the codepoint count;
   *out is malloc'd (n+1 capacity so a zero-length s still yields a non-NULL,
   freeable pointer), NULL only on allocation failure (with *count left
   unset by the caller in that case -- callers check for NULL). */
static cp_info *decode_utf8_widths(const char *s, size_t *count_out)
{
    size_t len = strlen(s);
    cp_info *arr = malloc((len + 1) * sizeof(*arr));
    size_t n = 0;
    size_t i = 0;

    if (arr == NULL)
        return NULL;

    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        unsigned int cp;
        size_t clen;

        if (c < 0x80) {
            cp = c;
            clen = 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < len && ((unsigned char)s[i + 1] & 0xC0) == 0x80) {
            cp = ((unsigned int)(c & 0x1F) << 6) | ((unsigned char)s[i + 1] & 0x3F);
            clen = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < len && ((unsigned char)s[i + 1] & 0xC0) == 0x80 &&
                  ((unsigned char)s[i + 2] & 0xC0) == 0x80) {
            cp = ((unsigned int)(c & 0x0F) << 12) | ((unsigned int)((unsigned char)s[i + 1] & 0x3F) << 6) |
                ((unsigned char)s[i + 2] & 0x3F);
            clen = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < len && ((unsigned char)s[i + 1] & 0xC0) == 0x80 &&
                  ((unsigned char)s[i + 2] & 0xC0) == 0x80 && ((unsigned char)s[i + 3] & 0xC0) == 0x80) {
            cp = ((unsigned int)(c & 0x07) << 18) | ((unsigned int)((unsigned char)s[i + 1] & 0x3F) << 12) |
                ((unsigned int)((unsigned char)s[i + 2] & 0x3F) << 6) | ((unsigned char)s[i + 3] & 0x3F);
            clen = 4;
        } else {
            cp = c;
            clen = 1;
        }

        arr[n].byte_off = i;
        arr[n].byte_len = clen;
        arr[n].width = codepoint_width(cp);
        arr[n].cp = cp;
        n++;
        i += clen;
    }

    *count_out = n;
    return arr;
}

static int display_width(const char *s)
{
    size_t n;
    cp_info *cps = decode_utf8_widths(s, &n);
    size_t i;
    int w = 0;

    if (cps == NULL)
        return (int)strlen(s); /* OOM fallback: byte length is still a plausible width */
    for (i = 0; i < n; i++)
        w += cps[i].width;
    free(cps);
    return w;
}

/* --stat's name truncation: "..." (3 columns) + the longest suffix of the
   (already quoted) name that fits in name_width columns, pushed further
   right to start just after the first '/' at or after the cut point.
   name_width <= 3 always yields "..." alone, even though that overflows the
   requested width -- measured, git does the same. */
static char *truncate_name(const char *name, int name_width)
{
    size_t n;
    cp_info *cps = decode_utf8_widths(name, &n);
    size_t i;
    long total_w = 0;
    size_t cut_idx;
    char *result;

    if (cps == NULL)
        return strdup(name);

    for (i = 0; i < n; i++)
        total_w += cps[i].width;

    if (total_w <= name_width) {
        free(cps);
        return strdup(name);
    }
    /* No early return for name_width <= 3. It looks like it needs one, and an
       earlier version had it, but the general path below already produces
       exactly "...": target goes <= 0, the loop drops every character, and an
       empty suffix has no '/' to push past. Keeping the shortcut cost more
       than it saved -- a mutation aimed at the <= 3 boundary came back green
       and read as missing coverage, when the truth was that the branch could
       not change the answer. The defence worth mutating is the loop. */

    {
        long target = name_width - 3;
        long suffix_w = total_w;

        cut_idx = 0;
        while (cut_idx < n && suffix_w > target) {
            suffix_w -= cps[cut_idx].width;
            cut_idx++;
        }

        {
            size_t k;
            size_t found = (size_t)-1;

            for (k = cut_idx; k < n; k++) {
                if (cps[k].cp == '/') {
                    found = k;
                    break;
                }
            }
            /* "Inclusive of that slash" means the slash itself stays in the
               printed suffix (measured against git 2.55.0: a long path
               under one directory renders as three dots then a slash then
               the filename, not three dots directly followed by the
               filename) -- the cut lands AT the slash, not past it. */
            if (found != (size_t)-1)
                cut_idx = found;
        }
    }

    {
        size_t byte_start = (cut_idx < n) ? cps[cut_idx].byte_off : strlen(name);
        size_t tail_len = strlen(name) - byte_start;

        result = malloc(3 + tail_len + 1);
        if (result != NULL) {
            memcpy(result, "...", 3);
            memcpy(result + 3, name + byte_start, tail_len);
            result[3 + tail_len] = '\0';
        }
    }
    free(cps);
    return result;
}

/* ---- terminal width for --stat -------------------------------------- */

/* Measured against git 2.55.0: it honours COLUMNS even when stdout is not a
   tty, and falls back to the controlling terminal's width, and then to 80. */
static int get_columns(void)
{
    const char *env = getenv("COLUMNS");

    if (env != NULL && *env != '\0') {
        char *end;
        long v = strtol(env, &end, 10);

        if (*end == '\0' && v > 0)
            return (int)v;
    }
#ifdef TIOCGWINSZ
    {
        struct winsize ws;

        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
            return ws.ws_col;
    }
#endif
    return 80;
}

/* ---- per-entry content stats, shared by --stat/--numstat/--shortstat/PATCH */

typedef struct {
    int unmerged;
    /* 0 iff both sides are BLOB with the identical id -- the ONLY case this
       flag can identify as a pure mode change without actually reading both
       sides' bytes. It is NOT true in general that content_changed==1 means
       the bytes differ: a tree/index BLOB vs a WORKDIR side (Phase 26) can
       be a bare chmod with byte-identical content, and since a WORKDIR side
       is never BLOB, the check below unconditionally sets content_changed=1
       for it -- the callers below still read both sides' bytes in that case
       and correctly get added==0, deleted==0 from the line-count step
       (measured against git 2.55.0: --stat prints "| 0", --numstat prints
       "0\t0", --shortstat's clauses both read 0). So a text row reaching the
       line-count step with content_changed==1 does NOT always yield
       added>0||deleted>0 -- a mode-only WORKDIR row is the one case where it
       legitimately doesn't. */
    int content_changed;
    int is_binary;
    long added, deleted;
    size_t old_size, new_size;
} entry_stat;

/* Mode-change detection is duplicated here and in print_patch rather than
   carried on entry_stat: print_patch is the only caller that needs it (the
   old/new mode lines), it never calls build_entry_stat (PATCH format reads
   both sides itself, unconditionally, rather than going through the
   content_changed short-circuit below), and a field only one non-caller of
   this function would read is dead weight on every other caller. */
static int build_entry_stat(const char *git_dir, const char *repo_root, const sg_diff_entry *e,
                            entry_stat *st)
{
    memset(st, 0, sizeof(*st));

    if (e->unmerged) {
        st->unmerged = 1;
        return 0;
    }

    st->content_changed = !(e->old_side.kind == SG_DIFF_SIDE_BLOB && e->new_side.kind == SG_DIFF_SIDE_BLOB &&
                            memcmp(e->old_side.id, e->new_side.id, SG_SHA1_RAW_LEN) == 0);

    if (!st->content_changed)
        return 0;

    {
        unsigned char *a_data = NULL, *b_data = NULL;
        size_t a_len = 0, b_len = 0;
        sg_chunk_missing_info missing;
        int rc;

        rc = sg_diff_side_read(git_dir, repo_root, e->path, &e->old_side, &a_data, &a_len, &missing);
        if (rc == -2) {
            sg_chunk_print_missing_error(e->path, &missing);
            return -1;
        }
        if (rc != 0) {
            fprintf(stderr, "sg: warning: cannot read %s\n", sg_quote_path_delimited(e->path));
            return -1;
        }

        rc = sg_diff_side_read(git_dir, repo_root, e->path, &e->new_side, &b_data, &b_len, &missing);
        if (rc == -2) {
            sg_chunk_print_missing_error(e->path, &missing);
            free(a_data);
            return -1;
        }
        if (rc != 0) {
            fprintf(stderr, "sg: warning: cannot read %s\n", sg_quote_path_delimited(e->path));
            free(a_data);
            return -1;
        }

        st->is_binary = is_binary_data(a_data, a_len) || is_binary_data(b_data, b_len);
        if (st->is_binary) {
            st->old_size = a_len;
            st->new_size = b_len;
        } else {
            count_lines(a_data, a_len, b_data, b_len, &st->added, &st->deleted);
        }
        free(a_data);
        free(b_data);
    }
    return 0;
}

/* ---- PATCH format ----------------------------------------------------- */

/* index/mode-line plumbing shares this: git's "index <old>..<new>" line and
   an entry's "old/new mode" lines want the id/mode an ordinary tree/index
   entry would carry, resolved off whichever kind of side actually produced
   it. See sg/diff.h's sg_diff_side contract for why BLOB and WORKDIR need
   different treatment here.

   Always fills *out with SOMETHING displayable, falling back to the side's
   raw id when resolution fails, but returns 0 only when that id is actually
   verified -- -1 means "could not verify" (a broken/missing chunk pointer).
   That distinction matters to the caller beyond cosmetics: two "unverified"
   ids that happen to come out byte-equal are NOT proof the content matches,
   and print_patch must not treat them as a mode-only row on that basis --
   doing so would silently skip the sg_diff_side_read call that is the only
   place this failure gets reported to the user (measured: the
   append_index_entry_vs_workdir builder deliberately force-appends a row
   whenever chunk resolution fails, precisely so the renderer gets a chance
   to hit and report the same failure with the path in hand). */
static int side_effective_id(const char *git_dir, const sg_diff_side *side,
                             unsigned char out[SG_SHA1_RAW_LEN])
{
    if (side->kind == SG_DIFF_SIDE_BLOB) {
        if (sg_chunk_effective_id(git_dir, side->id, out) == 0)
            return 0;
        memcpy(out, side->id, SG_SHA1_RAW_LEN);
        return -1;
    }
    if (side->kind == SG_DIFF_SIDE_WORKDIR) {
        memcpy(out, side->id, SG_SHA1_RAW_LEN);
        return 0;
    }
    /* ABSENT: git's "0000000" -- the hex of an all-zero id happens to start
       with 7 zeros, so no special-casing is needed at the print site. */
    memset(out, 0, SG_SHA1_RAW_LEN);
    return 0;
}

/* Unified-diff context width and the merge-adjacent-hunks threshold --
   both measured against real git 2.55.0 (CLAUDE.md's Phase 26 note: 5/6
   equal lines between two changes merge into one hunk, 7/8/9 stay split). */
#define SG_DIFF_CONTEXT 3
#define SG_DIFF_MERGE_GAP (2 * SG_DIFF_CONTEXT)

/* Prints "\ No newline at end of file" right after the line just printed,
   if that line is genuinely the file's last line and lacked a trailing
   newline -- otherwise a no-op. `idx` is the index just printed, `n` the
   line count of the array it came from. */
static void maybe_print_no_newline(const sg_diff_line *arr, size_t idx, size_t n)
{
    if (idx + 1 == n && !arr[idx].has_nl)
        printf("\\ No newline at end of file\n");
}

/* Function-name hunk suffix: scans backward from a[0..before) for the
   nearest line whose first byte is alnum/'_'/'$' (measured against git
   2.55.0 -- see CLAUDE.md's Phase 26 note for the exact character-class
   boundary). Returns that line, or a zero-length line (ptr non-NULL only
   for a non-empty search space) if none is found -- callers check len==0
   to know whether to print the trailing " <name>" at all. */
static sg_diff_line find_function_name(const sg_diff_line *a, size_t before)
{
    sg_diff_line none;
    size_t k;

    none.ptr = "";
    none.len = 0;
    none.has_nl = 1;

    for (k = before; k-- > 0;) {
        if (a[k].len > 0) {
            char c = a[k].ptr[0];

            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '$')
                return a[k];
        }
    }
    return none;
}

/* "-s,c" / "+s,c" half of an @@ line, with git's two omission rules
   (CLAUDE.md's Phase 26 note, both measured against 2.55.0): c == 1 omits
   ",c" entirely; c == 0 prints s as 0 regardless of where it would
   otherwise land. */
static void print_hunk_range(char sign, size_t start, size_t count)
{
    size_t disp_start = (count == 0) ? 0 : start + 1;

    if (count == 1)
        printf("%c%zu", sign, disp_start);
    else
        printf("%c%zu,%zu", sign, disp_start, count);
}

static int print_text_diff_body(const char *path, int old_present, int new_present,
                                const unsigned char *a_data, size_t a_len,
                                const unsigned char *b_data, size_t b_len)
{
    size_t na, nb;
    sg_diff_line *a = sg_diff_split_lines(a_data, a_len, &na);
    sg_diff_line *b = sg_diff_split_lines(b_data, b_len, &nb);
    sg_diff_script *script;
    size_t hi; /* index of the first group of the NEXT hunk, i.e. loop cursor */

    script = sg_diff_build_script(a, na, b, nb, 1 /* diff.indentHeuristic, on by default */);
    if (script == NULL) {
        free(a);
        free(b);
        return -1;
    }

    /* /dev/null for the side that doesn't exist -- measured against git
       2.55.0: no a/ or b/ prefix, no quoting, no TAB terminator even when the
       path has a space. */
    if (old_present)
        printf("--- %s%s\n", sg_quote_path_prefixed("a/", path), diff_name_terminator(path));
    else
        printf("--- /dev/null\n");
    if (new_present)
        printf("+++ %s%s\n", sg_quote_path_prefixed("b/", path), diff_name_terminator(path));
    else
        printf("+++ /dev/null\n");

    hi = 0;
    while (hi < script->count) {
        size_t lo = hi;
        size_t prev_a_end = (lo == 0) ? 0 : script->groups[lo - 1].a_off + script->groups[lo - 1].a_len;
        size_t a_ctx_before, b_ctx_before, a_start, b_start;
        size_t next_a_start, a_ctx_after, a_end, b_end;
        sg_diff_line func;
        size_t pos_a, pos_b;
        size_t gi;

        /* Grow the hunk while the gap to the next group is small enough
           to merge (SG_DIFF_MERGE_GAP, i.e. <= 6 equal lines between). */
        while (hi + 1 < script->count) {
            size_t gap = script->groups[hi + 1].a_off - (script->groups[hi].a_off + script->groups[hi].a_len);

            if (gap > SG_DIFF_MERGE_GAP)
                break;
            hi++;
        }

        {
            size_t avail_before = script->groups[lo].a_off - prev_a_end;

            a_ctx_before = avail_before < SG_DIFF_CONTEXT ? avail_before : SG_DIFF_CONTEXT;
        }
        b_ctx_before = a_ctx_before; /* context lines are identical/equal-length on both sides */
        a_start = script->groups[lo].a_off - a_ctx_before;
        b_start = script->groups[lo].b_off - b_ctx_before;

        next_a_start = (hi + 1 == script->count) ? na : script->groups[hi + 1].a_off;
        {
            size_t last_a_end = script->groups[hi].a_off + script->groups[hi].a_len;
            size_t avail_after = next_a_start - last_a_end;

            a_ctx_after = avail_after < SG_DIFF_CONTEXT ? avail_after : SG_DIFF_CONTEXT;
            a_end = last_a_end + a_ctx_after;
            b_end = script->groups[hi].b_off + script->groups[hi].b_len + a_ctx_after;
        }

        func = find_function_name(a, a_start);

        printf("@@ ");
        print_hunk_range('-', a_start, a_end - a_start);
        printf(" ");
        print_hunk_range('+', b_start, b_end - b_start);
        printf(" @@");
        if (func.len > 0)
            printf(" %.*s", (int)func.len, func.ptr);
        printf("\n");

        pos_a = a_start;
        pos_b = b_start;
        for (gi = lo; gi <= hi; gi++) {
            const sg_diff_group *g = &script->groups[gi];

            while (pos_a < g->a_off) {
                printf(" %.*s\n", (int)a[pos_a].len, a[pos_a].ptr);
                maybe_print_no_newline(a, pos_a, na);
                pos_a++;
                pos_b++;
            }
            while (pos_a < g->a_off + g->a_len) {
                printf("-%.*s\n", (int)a[pos_a].len, a[pos_a].ptr);
                maybe_print_no_newline(a, pos_a, na);
                pos_a++;
            }
            while (pos_b < g->b_off + g->b_len) {
                printf("+%.*s\n", (int)b[pos_b].len, b[pos_b].ptr);
                maybe_print_no_newline(b, pos_b, nb);
                pos_b++;
            }
        }
        while (pos_a < a_end) {
            printf(" %.*s\n", (int)a[pos_a].len, a[pos_a].ptr);
            maybe_print_no_newline(a, pos_a, na);
            pos_a++;
            pos_b++;
        }

        hi++;
    }

    sg_diff_script_free(script);
    free(a);
    free(b);
    return 0;
}

static int print_patch(const char *git_dir, const char *repo_root, const sg_diff_list *list)
{
    size_t i;
    int had_error = 0;

    for (i = 0; i < list->count; i++) {
        const sg_diff_entry *e = &list->entries[i];
        unsigned char *a_data = NULL, *b_data = NULL;
        size_t a_len = 0, b_len = 0;
        sg_chunk_missing_info missing;
        int rc;
        int old_present, new_present;
        unsigned char old_eff[SG_SHA1_RAW_LEN], new_eff[SG_SHA1_RAW_LEN];
        char old_hex[SG_SHA1_HEX_LEN + 1], new_hex[SG_SHA1_HEX_LEN + 1];
        int wrote_mode_line;
        int content_changed;

        /* Deliberate divergence from real git, which prints a "diff --cc"
           combined-diff format for an unresolved conflict -- not
           implemented this round (see CLAUDE.md's PATCH format note). Both
           sides of an unmerged sg_diff_entry are ABSENT (sg/diff.h), so
           there is no single pair of blobs to diff anyway. */
        if (e->unmerged)
            continue;

        old_present = e->old_side.kind != SG_DIFF_SIDE_ABSENT;
        new_present = e->new_side.kind != SG_DIFF_SIDE_ABSENT;

        {
            int old_verified = side_effective_id(git_dir, &e->old_side, old_eff) == 0;
            int new_verified = side_effective_id(git_dir, &e->new_side, new_eff) == 0;

            /* An add or a delete always counts as "content changed" even
               though one side's id is the all-zero ABSENT id and can never
               legitimately equal the other -- spelled out anyway rather than
               relying on that, since a diff.h builder is free to emit a row
               whenever mode OR content changed and every entry here is
               guaranteed to differ in at least one of the two. An
               unverified id on either side also forces this true: see
               side_effective_id's contract for why treating a coincidental
               byte-match as proof of "unchanged" here would swallow a read
               error the code below is about to hit and report. */
            content_changed = !old_present || !new_present || !old_verified || !new_verified ||
                              memcmp(old_eff, new_eff, SG_SHA1_RAW_LEN) != 0;
        }

        printf("diff --git %s %s\n", sg_quote_path_prefixed("a/", e->path),
              sg_quote_path_prefixed("b/", e->path));

        wrote_mode_line = 0;
        if (!old_present && new_present) {
            printf("new file mode %06o\n", e->new_side.mode);
            wrote_mode_line = 1;
        } else if (old_present && !new_present) {
            printf("deleted file mode %06o\n", e->old_side.mode);
            wrote_mode_line = 1;
        } else if (old_present && new_present && e->old_side.mode != 0 && e->new_side.mode != 0 &&
                  e->old_side.mode != e->new_side.mode) {
            printf("old mode %06o\n", e->old_side.mode);
            printf("new mode %06o\n", e->new_side.mode);
            wrote_mode_line = 1;
        }

        /* A pure mode change (content unchanged) prints NO index line at all
           -- measured against git 2.55.0 (oracle rule 3 in sg/diff.h's Phase
           26 note): "diff --git" + "old mode"/"new mode" is the entire
           entry, nothing else. Every other case (add/delete/modify/binary)
           always has content_changed true, since a diff.h builder never
           emits a row unless mode or content differs and add/delete are
           unconditionally "content changed" above. */
        if (!content_changed)
            continue;

        /* "index <old7>..<new7>[ <mode>]" -- the mode suffix appears only
           when no mode line was printed above (measured against git 2.55.0,
           see sg/diff.h's Phase 26 note): new-file/deleted-file/old+new-mode
           already said the mode, so the suffix would be redundant there. */
        sg_sha1_to_hex(old_eff, old_hex);
        sg_sha1_to_hex(new_eff, new_hex);
        old_hex[7] = '\0';
        new_hex[7] = '\0';
        if (wrote_mode_line)
            printf("index %s..%s\n", old_hex, new_hex);
        else
            printf("index %s..%s %06o\n", old_hex, new_hex,
                  new_present ? e->new_side.mode : e->old_side.mode);

        rc = sg_diff_side_read(git_dir, repo_root, e->path, &e->old_side, &a_data, &a_len, &missing);
        if (rc == -2) {
            sg_chunk_print_missing_error(e->path, &missing);
            had_error = 1;
            continue;
        }
        if (rc != 0) {
            fprintf(stderr, "sg: warning: cannot read %s\n", sg_quote_path_delimited(e->path));
            had_error = 1;
            continue;
        }
        rc = sg_diff_side_read(git_dir, repo_root, e->path, &e->new_side, &b_data, &b_len, &missing);
        if (rc == -2) {
            sg_chunk_print_missing_error(e->path, &missing);
            free(a_data);
            had_error = 1;
            continue;
        }
        if (rc != 0) {
            fprintf(stderr, "sg: warning: cannot read %s\n", sg_quote_path_delimited(e->path));
            free(a_data);
            had_error = 1;
            continue;
        }

        if (is_binary_data(a_data, a_len) || is_binary_data(b_data, b_len)) {
            printf("Binary files %s and %s differ\n",
                  old_present ? sg_quote_path_prefixed("a/", e->path) : "/dev/null",
                  new_present ? sg_quote_path_prefixed("b/", e->path) : "/dev/null");
            free(a_data);
            free(b_data);
            continue;
        }

        if (print_text_diff_body(e->path, old_present, new_present, a_data, a_len, b_data, b_len) !=
           0) {
            fprintf(stderr, "sg: warning: out of memory diffing %s\n", sg_quote_path_delimited(e->path));
            had_error = 1;
        }
        free(a_data);
        free(b_data);
    }

    return had_error ? -1 : 0;
}

/* ---- NAME_ONLY / NAME_STATUS (no content reads needed) ---------------- */

static int print_name_only(const sg_diff_list *list)
{
    size_t i;

    for (i = 0; i < list->count; i++)
        printf("%s\n", sg_quote_path(list->entries[i].path));
    return 0;
}

static int print_name_status(const sg_diff_list *list)
{
    size_t i;

    for (i = 0; i < list->count; i++) {
        const sg_diff_entry *e = &list->entries[i];

        printf("%c\t%s\n", entry_status(e), sg_quote_path(e->path));
    }
    return 0;
}

/* ---- NUMSTAT ------------------------------------------------------------ */

static int print_numstat(const char *git_dir, const char *repo_root, const sg_diff_list *list)
{
    size_t i;
    int had_error = 0;

    for (i = 0; i < list->count; i++) {
        const sg_diff_entry *e = &list->entries[i];
        entry_stat st;

        if (build_entry_stat(git_dir, repo_root, e, &st) != 0) {
            had_error = 1;
            continue;
        }

        if (st.unmerged)
            printf("0\t0\t%s\n", sg_quote_path(e->path));
        else if (st.is_binary)
            printf("-\t-\t%s\n", sg_quote_path(e->path));
        else
            printf("%ld\t%ld\t%s\n", st.added, st.deleted, sg_quote_path(e->path));
    }

    return had_error ? -1 : 0;
}

/* ---- STAT / SHORTSTAT --------------------------------------------------- */

typedef struct {
    char *name; /* owned; sg_quote_path's ring can't survive across rows */
    int name_dw;
    int unmerged;
    int is_binary;
    long added, deleted;
    size_t old_size, new_size;
} stat_row;

static int build_stat_rows(const char *git_dir, const char *repo_root, const sg_diff_list *list,
                           stat_row **rows_out, size_t *count_out, int *had_error)
{
    stat_row *rows;
    size_t n = 0, i;

    *rows_out = NULL;
    *count_out = 0;
    if (list->count == 0)
        return 0;

    rows = malloc(list->count * sizeof(*rows));
    if (rows == NULL)
        return -1;

    for (i = 0; i < list->count; i++) {
        const sg_diff_entry *e = &list->entries[i];
        entry_stat st;
        const char *qname;

        if (build_entry_stat(git_dir, repo_root, e, &st) != 0) {
            *had_error = 1;
            continue;
        }

        qname = sg_quote_path(e->path);
        rows[n].name = strdup(qname);
        if (rows[n].name == NULL) {
            *had_error = 1;
            continue;
        }
        rows[n].name_dw = display_width(rows[n].name);
        rows[n].unmerged = st.unmerged;
        rows[n].is_binary = st.is_binary;
        rows[n].added = st.added;
        rows[n].deleted = st.deleted;
        rows[n].old_size = st.old_size;
        rows[n].new_size = st.new_size;
        n++;
    }

    *rows_out = rows;
    *count_out = n;
    return 0;
}

static void free_stat_rows(stat_row *rows, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        free(rows[i].name);
    free(rows);
}

static long scale_linear(long it, int graph_width, long max_change)
{
    if (it == 0)
        return 0;
    return 1 + it * ((long)graph_width - 1) / max_change;
}

static void print_graph(long add, long del, int graph_width, long max_change)
{
    long total;
    long i;

    if (max_change > 0 && graph_width <= max_change) {
        total = scale_linear(add + del, graph_width, max_change);
        if (total < 2 && add != 0 && del != 0)
            total = 2;
        if (add < del) {
            add = scale_linear(add, graph_width, max_change);
            del = total - add;
        } else {
            del = scale_linear(del, graph_width, max_change);
            add = total - del;
        }
    }
    for (i = 0; i < add; i++)
        putchar('+');
    for (i = 0; i < del; i++)
        putchar('-');
}

static void print_stat_row(const stat_row *r, int name_width, int number_width, int graph_width,
                           long max_change)
{
    char *tn = truncate_name(r->name, name_width);
    int dw = tn != NULL ? display_width(tn) : 0;
    int pad = name_width - dw;
    int k;

    printf(" %s", tn != NULL ? tn : r->name);
    for (k = 0; k < pad; k++)
        putchar(' ');
    printf(" |");

    if (r->unmerged) {
        printf(" Unmerged\n");
    } else if (r->is_binary) {
        printf(" %*s %zu -> %zu bytes\n", number_width, "Bin", r->old_size, r->new_size);
    } else {
        long count = r->added + r->deleted;

        printf(" %*ld", number_width, count);
        if (count != 0) {
            putchar(' ');
            print_graph(r->added, r->deleted, graph_width, max_change);
        }
        putchar('\n');
    }
    free(tn);
}

/* Measured against git 2.55.0 with a paired, only-one-variable-different
   comparison: files_changed==0 (an unmerged-only diff) prints JUST " 0
   files changed" -- neither clause -- while files_changed==1 with a pure
   binary/mode change (both counts 0) prints BOTH clauses in the plural
   (" 1 file changed, 0 insertions(+), 0 deletions(-)"). The insertion/
   deletion clauses are therefore gated on files_changed > 0, not just on
   their own counts. */
static void print_summary(int files_changed, long insertions, long deletions)
{
    printf(" %d file%s changed", files_changed, files_changed == 1 ? "" : "s");
    if (files_changed > 0) {
        if (insertions != 0 || deletions == 0)
            printf(", %ld insertion%s(+)", insertions, insertions == 1 ? "" : "s");
        if (deletions != 0 || insertions == 0)
            printf(", %ld deletion%s(-)", deletions, deletions == 1 ? "" : "s");
    }
    printf("\n");
}

static int print_stat(const char *git_dir, const char *repo_root, const sg_diff_list *list,
                      const sg_diff_out_opts *opts, int shortstat_only)
{
    stat_row *rows = NULL;
    size_t n = 0;
    int had_error = 0;
    size_t i;
    long max_len = 0;
    long max_change = 0;
    int bin_width = 0;
    int has_binary = 0;
    int number_width;
    int W, columns;
    long max_graph;
    int graph_width, name_width;
    /* This CLI's flag surface (sg_diff_out_opts) has no --stat-graph-width
       equivalent, unlike real git's separate --stat-graph-width knob. Kept
       as an explicit (always-false) variable rather than deleting the
       branches below so the layout algorithm matches the reverse-engineered
       spec verbatim -- wiring a real flag in later needs no restructuring
       here, just a value. */
    int stat_graph_width = 0;
    int files_changed = 0;
    long insertions = 0, deletions = 0;

    if (build_stat_rows(git_dir, repo_root, list, &rows, &n, &had_error) != 0)
        return -1;

    if (n == 0) {
        free_stat_rows(rows, n);
        return had_error ? -1 : 0;
    }

    for (i = 0; i < n; i++) {
        if (rows[i].name_dw > max_len)
            max_len = rows[i].name_dw;
        if (!rows[i].unmerged)
            files_changed++;

        if (rows[i].unmerged) {
            if (8 > bin_width)
                bin_width = 8;
            continue;
        }
        if (rows[i].is_binary) {
            int w = 14 + digits((long long)rows[i].new_size) + digits((long long)rows[i].old_size);

            has_binary = 1;
            if (w > bin_width)
                bin_width = w;
        } else {
            long change = rows[i].added + rows[i].deleted;

            if (change > max_change)
                max_change = change;
            insertions += rows[i].added;
            deletions += rows[i].deleted;
        }
    }

    number_width = has_binary ? 3 : 0;
    {
        int dw = digits(max_change);

        if (dw > number_width)
            number_width = dw;
    }

    columns = get_columns();
    W = opts->stat_width > 0 ? opts->stat_width : (columns > 0 ? columns : 80);
    if (W < 16 + 6 + number_width)
        W = 16 + 6 + number_width;

    max_graph = max_change;
    if (bin_width && bin_width - 4 > max_graph)
        max_graph = bin_width - 4;

    graph_width = (stat_graph_width && stat_graph_width < max_graph) ? stat_graph_width : (int)max_graph;
    name_width = (opts->stat_name_width > 0 && opts->stat_name_width < max_len) ? opts->stat_name_width
                                                                                : (int)max_len;

    if (name_width + number_width + 6 + graph_width > W) {
        if (graph_width > W * 3 / 8 - number_width - 6) {
            graph_width = W * 3 / 8 - number_width - 6;
            if (graph_width < 6)
                graph_width = 6;
        }
        if (stat_graph_width && stat_graph_width < graph_width)
            graph_width = stat_graph_width;
        if (name_width > W - number_width - 6 - graph_width)
            name_width = W - number_width - 6 - graph_width;
        else
            graph_width = W - number_width - 6 - name_width;
    }

    if (!shortstat_only) {
        for (i = 0; i < n; i++)
            print_stat_row(&rows[i], name_width, number_width, graph_width, max_change);
    }
    print_summary(files_changed, insertions, deletions);

    free_stat_rows(rows, n);
    return had_error ? -1 : 0;
}

/* ---- dispatcher ---------------------------------------------------- */

int sg_diff_print(const char *git_dir, const char *repo_root, const sg_diff_list *list,
                  const sg_diff_out_opts *opts)
{
    switch (opts->format) {
    case SG_DIFF_FORMAT_PATCH:
        return print_patch(git_dir, repo_root, list);
    case SG_DIFF_FORMAT_STAT:
        return print_stat(git_dir, repo_root, list, opts, 0);
    case SG_DIFF_FORMAT_SHORTSTAT:
        return print_stat(git_dir, repo_root, list, opts, 1);
    case SG_DIFF_FORMAT_NUMSTAT:
        return print_numstat(git_dir, repo_root, list);
    case SG_DIFF_FORMAT_NAME_ONLY:
        return print_name_only(list);
    case SG_DIFF_FORMAT_NAME_STATUS:
        return print_name_status(list);
    default:
        return -1;
    }
}
