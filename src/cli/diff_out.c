#include "sg/diff_out.h"

#include "sg/chunk.h"
#include "sg/diff.h"
#include "sg/diff_lcs.h"
#include "sg/hash.h"
#include "sg/quote.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* The path a row's OLD side belongs to. Only a rename makes this differ
   from e->path, and today it makes no difference either way: a WORKDIR side
   is the only kind sg_diff_side_read uses the path for, and no builder ever
   puts one on an old_side. Spelled out anyway, because "harmless because of
   an invariant three files away" is how a future builder silently reads a
   renamed row's before-content out of the destination file. */
static const char *old_side_path(const sg_diff_entry *e)
{
    return e->old_path != NULL ? e->old_path : e->path;
}

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

/* "Combinable" -- git's word for "there is content on both sides to
   actually diff, render this as a combined diff instead of a plain 2-way
   one". Thin wrapper: sg_diff_entry_is_combined (sg/diff.h) is the shared
   predicate for BOTH producers of ours/theirs/result (a real conflict via
   sg_diff_index_workdir, and Phase 40's sg_diff_fill_combined_from_index
   over a tree-vs-workdir list), and is false for every row
   sg_diff_tree_index (--cached) or sg_diff_trees produces -- neither fills
   ours/theirs -- which is exactly why --cached and a two-rev diff always
   fall through to ordinary rendering regardless of -c/--cc (Phase 34
   oracle 1, Phase 40 SPEC section 1). */
static int combinable(const sg_diff_entry *e)
{
    return sg_diff_entry_is_combined(e);
}

/* Whether the entry right after index i is the companion row a combinable
   unmerged entry can carry -- shared by every format that needs to fold the
   pair into one row (PHASE34_ORACLE.md #2). Deliberately checks !unmerged
   too, not just the path: a corrupt index can put two genuinely unmerged
   rows for the same path back to back (see CLAUDE.md's status.c/diff.c note
   on index ordering not being validated), and those must NOT be treated as
   an unmerged-row/companion-row pair. */
static int has_companion_row(const sg_diff_list *list, size_t i)
{
    return i + 1 < list->count && !list->entries[i + 1].unmerged &&
          strcmp(list->entries[i + 1].path, list->entries[i].path) == 0;
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
                        size_t b_len, sg_diff_algorithm algo, long *added, long *deleted)
{
    size_t na, nb;
    sg_diff_line *a = sg_diff_split_lines(a_data, a_len, &na);
    sg_diff_line *b = sg_diff_split_lines(b_data, b_len, &nb);
    sg_diff_script *script;
    size_t k;
    long add = 0, del = 0;

    *added = 0;
    *deleted = 0;

    script = sg_diff_build_script(a, na, b, nb, 0, algo);
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
    if (cp >= 0x3040 && cp <= 0x309F) /* Hiragana -- measured via U+3042 */
        return 2;
    if (cp >= 0x3400 && cp <= 0x4DBF) /* CJK Extension A -- measured via U+3400 */
        return 2;
    if (cp >= 0x4E00 && cp <= 0x9FFF) /* CJK Unified Ideographs -- measured via U+4E2D/U+6587 */
        return 2;
    if (cp >= 0xAC00 && cp <= 0xD7A3) /* Hangul syllables -- measured via U+AC00 */
        return 2;
    if (cp >= 0xF900 && cp <= 0xFAFF) /* CJK Compatibility Ideographs -- measured via U+F900 */
        return 2;
    if (cp >= 0xFF01 && cp <= 0xFF60) /* Fullwidth forms -- measured via U+FF21/U+FF01 */
        return 2;
    if (cp >= 0xFFE0 && cp <= 0xFFE6) /* fullwidth signs -- measured via U+FFE0/U+FFE5 */
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
                            sg_diff_algorithm algo, entry_stat *st)
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

        rc = sg_diff_side_read(git_dir, repo_root, old_side_path(e), &e->old_side, &a_data, &a_len, &missing);
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
            count_lines(a_data, a_len, b_data, b_len, algo, &st->added, &st->deleted);
        }
        free(a_data);
        free(b_data);
    }
    return 0;
}

/* ---- PATCH format ----------------------------------------------------- */


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

/* `old_path` and `new_path` are the same for everything except a rename,
   where the a/ side must name where the content came FROM. Nothing caught
   this until Phase 30: an exact rename is byte-identical, so it prints no
   hunks and therefore no ---/+++ lines at all, and those were the only
   renames that existed. */
static int print_text_diff_body(const char *old_path, const char *new_path,
                                int old_present, int new_present,
                                const unsigned char *a_data, size_t a_len,
                                const unsigned char *b_data, size_t b_len,
                                sg_diff_algorithm algo)
{
    size_t na, nb;
    sg_diff_line *a = sg_diff_split_lines(a_data, a_len, &na);
    sg_diff_line *b = sg_diff_split_lines(b_data, b_len, &nb);
    sg_diff_script *script;
    size_t hi; /* index of the first group of the NEXT hunk, i.e. loop cursor */

    script = sg_diff_build_script(a, na, b, nb, 1 /* diff.indentHeuristic, on by default */, algo);
    if (script == NULL) {
        free(a);
        free(b);
        return -1;
    }

    /* /dev/null for the side that doesn't exist -- measured against git
       2.55.0: no a/ or b/ prefix, no quoting, no TAB terminator even when the
       path has a space. */
    if (old_present)
        printf("--- %s%s\n", sg_quote_path_prefixed("a/", old_path),
              diff_name_terminator(old_path));
    else
        printf("--- /dev/null\n");
    if (new_present)
        printf("+++ %s%s\n", sg_quote_path_prefixed("b/", new_path),
              diff_name_terminator(new_path));
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

/* ---- combined diff (Phase 34: `sg diff -c` / `--cc`) ------------------ */

/* This is a from-scratch port of git v2.55.0's combine-diff.c, fixed at
   exactly 2 parents (ours = index stage 2, theirs = index stage 3) --
   sg never needs an N-way merge here, only the one real git produces for an
   unresolved 2-way conflict. See PHASE34_ALGO.md (written while porting)
   for the line numbers this was derived from; the two surprises worth
   flagging inline as they come up are noted below. */

#define SG_COMBINE_MARK 0x4u          /* bit 2: this position is in a hunk */
#define SG_COMBINE_NO_PRE_DELETE 0x8u /* bit 3: don't show its lost lines */
#define SG_COMBINE_ALL_MASK 0x3u      /* bits 0,1: "new relative to parent n" */
#define SG_COMBINE_CONTEXT 3

/* One deleted line attached to a result position, tagged with which
   parent(s) it was deleted from (bit n = parent n). `text` is borrowed from
   that parent's own sg_diff_split_lines array. */
typedef struct {
    sg_diff_line text;
    unsigned int parent_map;
} combine_lost;

typedef struct {
    sg_diff_line line; /* the result line; meaningless past index cnt-1 */
    unsigned int flag;
    size_t p_lno[2];
    combine_lost *lost;
    size_t lost_count;
    /* Per-parent holding pen, filled while combine_process_parent walks
       that one parent's diff against the result, flushed into `lost` (via
       coalesce_lines) before the next parent is processed. */
    combine_lost *plost;
    size_t plost_count, plost_cap;
} combine_sline;

static int combine_plost_append(combine_sline *sl, sg_diff_line text, unsigned int pbit)
{
    if (sl->plost_count == sl->plost_cap) {
        size_t new_cap = sl->plost_cap == 0 ? 4 : sl->plost_cap * 2;
        combine_lost *grown = realloc(sl->plost, new_cap * sizeof(*grown));

        if (grown == NULL)
            return -1;
        sl->plost = grown;
        sl->plost_cap = new_cap;
    }
    sl->plost[sl->plost_count].text = text;
    sl->plost[sl->plost_count].parent_map = pbit;
    sl->plost_count++;
    return 0;
}

/* git's coalesce_lines: merges `newarr` (nnew items, all newly deleted by
   parent `pbit`, already in that parent's own order) into the persistent
   `*base`/`*base_count` list built up from earlier parents, via the LCS of
   the two content sequences -- identical content collapses into one row
   with the union of parent bits, differing content is interleaved in LCS
   order. Always consumes (frees or reuses) `*base`'s old storage; `newarr`
   is only read. Returns 0, or -1 on allocation failure (base unchanged). */
static int coalesce_lines(combine_lost **base, size_t *base_count, const combine_lost *newarr,
                          size_t nnew, unsigned int pbit)
{
    size_t m = *base_count;
    size_t i, j;
    size_t **lcs;
    int **dir; /* 0 = BASE (keep base line), 1 = NEW, 2 = MATCH */
    combine_lost *result;
    size_t rc;

    if (nnew == 0)
        return 0;
    if (m == 0) {
        combine_lost *copy = malloc(nnew * sizeof(*copy));

        if (copy == NULL)
            return -1;
        memcpy(copy, newarr, nnew * sizeof(*copy));
        free(*base);
        *base = copy;
        *base_count = nnew;
        return 0;
    }

    lcs = malloc((m + 1) * sizeof(*lcs));
    dir = malloc((m + 1) * sizeof(*dir));
    if (lcs == NULL || dir == NULL) {
        free(lcs);
        free(dir);
        return -1;
    }
    for (i = 0; i <= m; i++) {
        lcs[i] = calloc(nnew + 1, sizeof(size_t));
        dir[i] = malloc((nnew + 1) * sizeof(int));
        if (lcs[i] == NULL || dir[i] == NULL) {
            size_t k;

            for (k = 0; k <= i; k++) {
                free(lcs[k]);
                free(dir[k]);
            }
            free(lcs);
            free(dir);
            return -1;
        }
        dir[i][0] = 0;
    }
    for (j = 1; j <= nnew; j++)
        dir[0][j] = 1;

    for (i = 1; i <= m; i++) {
        for (j = 1; j <= nnew; j++) {
            if (sg_diff_lines_equal((*base)[i - 1].text, newarr[j - 1].text)) {
                lcs[i][j] = lcs[i - 1][j - 1] + 1;
                dir[i][j] = 2;
            } else if (lcs[i][j - 1] >= lcs[i - 1][j]) {
                lcs[i][j] = lcs[i][j - 1];
                dir[i][j] = 1;
            } else {
                lcs[i][j] = lcs[i - 1][j];
                dir[i][j] = 0;
            }
        }
    }

    result = malloc((m + nnew) * sizeof(*result));
    if (result == NULL) {
        for (i = 0; i <= m; i++) {
            free(lcs[i]);
            free(dir[i]);
        }
        free(lcs);
        free(dir);
        return -1;
    }

    rc = 0;
    i = m;
    j = nnew;
    while (i != 0 || j != 0) {
        if (dir[i][j] == 2) {
            result[rc] = (*base)[i - 1];
            result[rc].parent_map |= pbit;
            rc++;
            i--;
            j--;
        } else if (dir[i][j] == 1) {
            result[rc++] = newarr[j - 1];
            j--;
        } else {
            result[rc++] = (*base)[i - 1];
            i--;
        }
    }
    for (i = 0; i < rc / 2; i++) {
        combine_lost tmp = result[i];

        result[i] = result[rc - 1 - i];
        result[rc - 1 - i] = tmp;
    }

    for (i = 0; i <= m; i++) {
        free(lcs[i]);
        free(dir[i]);
    }
    free(lcs);
    free(dir);
    free(*base);
    *base = result;
    *base_count = rc;
    return 0;
}

/* Runs parent n's 2-way diff against the result and folds it into `sline`:
   consume_hunk+consume_line's job (append deleted lines to the lost bucket,
   flag added lines) followed by the "assign p_lno, coalesce plost into
   lost" full-file pass, both from combine_diff() in combine-diff.c.

   The lost-bucket index is the SAME for both a pure-deletion group and a
   mixed add/delete group: git's nb/nb-1 split (comment_end note in
   PHASE34_ALGO.md) resolves to sg_diff_group's own b_off either way once
   you work through git's 1-based "insert after line N" convention -- see
   the design note in DESIGN.md's Phase 34 section. */
static int combine_process_parent(combine_sline *sline, size_t cnt, const sg_diff_line *parent_lines,
                                  size_t parent_cnt, const sg_diff_line *result_lines, int n,
                                  sg_diff_algorithm algo)
{
    sg_diff_script *script;
    size_t k;
    unsigned int pbit = 1u << n;
    size_t lno;
    size_t p_lno;

    (void)result_lines;
    script = sg_diff_build_script(parent_lines, parent_cnt, result_lines, cnt, 1, algo);
    if (script == NULL)
        return -1;

    for (k = 0; k < script->count; k++) {
        const sg_diff_group *g = &script->groups[k];
        size_t t;

        for (t = 0; t < g->a_len; t++) {
            if (combine_plost_append(&sline[g->b_off], parent_lines[g->a_off + t], pbit) != 0) {
                sg_diff_script_free(script);
                return -1;
            }
        }
        for (t = 0; t < g->b_len; t++)
            sline[g->b_off + t].flag |= pbit;
    }
    sg_diff_script_free(script);

    p_lno = 1;
    for (lno = 0; lno <= cnt; lno++) {
        combine_sline *sl = &sline[lno];
        size_t li;

        sl->p_lno[n] = p_lno;
        if (sl->plost_count > 0) {
            if (coalesce_lines(&sl->lost, &sl->lost_count, sl->plost, sl->plost_count, pbit) != 0)
                return -1;
            free(sl->plost);
            sl->plost = NULL;
            sl->plost_count = 0;
            sl->plost_cap = 0;
        }
        for (li = 0; li < sl->lost_count; li++)
            if (sl->lost[li].parent_map & pbit)
                p_lno++;
        if (lno < cnt && !(sl->flag & pbit))
            p_lno++;
    }
    sline[cnt + 1].p_lno[n] = p_lno;
    return 0;
}

static int combine_interesting(const combine_sline *sl)
{
    return (sl->flag & SG_COMBINE_ALL_MASK) != 0 || sl->lost_count != 0;
}

static size_t combine_adjust_hunk_tail(const combine_sline *sline, size_t hunk_begin, size_t i)
{
    if (hunk_begin + 1 <= i && !(sline[i - 1].flag & SG_COMBINE_ALL_MASK))
        i--;
    return i;
}

static size_t combine_find_next(const combine_sline *sline, size_t i, size_t cnt, int look_for_uninteresting)
{
    while (i <= cnt) {
        int marked = (sline[i].flag & SG_COMBINE_MARK) != 0;

        if (look_for_uninteresting ? !marked : marked)
            return i;
        i++;
    }
    return i;
}

/* git's give_context(): grows each already-marked run by SG_COMBINE_CONTEXT
   lines on either side, merging two runs whose gap is smaller than that.
   Returns 1 if anything was marked, 0 if the whole file is uninteresting. */
static int combine_give_context(combine_sline *sline, size_t cnt)
{
    size_t i;

    i = combine_find_next(sline, 0, cnt, 0);
    if (cnt < i)
        return 0;

    while (i <= cnt) {
        size_t j = (SG_COMBINE_CONTEXT < i) ? (i - SG_COMBINE_CONTEXT) : 0;
        size_t k;

        while (j < i) {
            if (!(sline[j].flag & SG_COMBINE_MARK))
                sline[j].flag |= SG_COMBINE_NO_PRE_DELETE;
            sline[j++].flag |= SG_COMBINE_MARK;
        }

    again:
        j = combine_find_next(sline, i, cnt, 1);
        if (cnt < j)
            break;

        k = combine_find_next(sline, j, cnt, 0);
        j = combine_adjust_hunk_tail(sline, i, j);

        if (k < j + SG_COMBINE_CONTEXT) {
            while (j < k)
                sline[j++].flag |= SG_COMBINE_MARK;
            i = k;
            goto again;
        }

        i = k;
        k = (j + SG_COMBINE_CONTEXT < cnt + 1) ? j + SG_COMBINE_CONTEXT : cnt + 1;
        while (j < k)
            sline[j++].flag |= SG_COMBINE_MARK;
    }
    return 1;
}

/* git's make_hunks(): marks every "interesting" position, then, for dense
   (--cc) mode only, prunes a whole candidate hunk when every one of its
   differing lines points at exactly one parent -- i.e. the result equals
   that other parent verbatim there. -c (dense == 0) skips the pruning
   entirely and returns give_context's raw marking. */
static int combine_make_hunks(combine_sline *sline, size_t cnt, int dense)
{
    size_t i;

    for (i = 0; i <= cnt; i++) {
        if (combine_interesting(&sline[i]))
            sline[i].flag |= SG_COMBINE_MARK;
        else
            sline[i].flag &= ~SG_COMBINE_MARK;
    }
    if (!dense)
        return combine_give_context(sline, cnt);

    i = 0;
    while (i <= cnt) {
        size_t j, hunk_begin, hunk_end;
        unsigned int same_diff;
        int has_interesting;

        while (i <= cnt && !(sline[i].flag & SG_COMBINE_MARK))
            i++;
        if (cnt < i)
            break;
        hunk_begin = i;
        for (j = i + 1; j <= cnt; j++) {
            if (!(sline[j].flag & SG_COMBINE_MARK)) {
                size_t la;
                int contin = 0;

                la = combine_adjust_hunk_tail(sline, hunk_begin, j);
                la = (la + SG_COMBINE_CONTEXT < cnt + 1) ? la + SG_COMBINE_CONTEXT : cnt + 1;
                while (la > 0) {
                    la--;
                    if (la < j)
                        break;
                    if (sline[la].flag & SG_COMBINE_MARK) {
                        contin = 1;
                        break;
                    }
                }
                if (!contin)
                    break;
                j = la;
            }
        }
        hunk_end = j;

        same_diff = 0;
        has_interesting = 0;
        for (j = i; j < hunk_end && !has_interesting; j++) {
            unsigned int this_diff = sline[j].flag & SG_COMBINE_ALL_MASK;
            size_t li;

            if (this_diff) {
                if (!same_diff)
                    same_diff = this_diff;
                else if (same_diff != this_diff) {
                    has_interesting = 1;
                    break;
                }
            }
            for (li = 0; li < sline[j].lost_count && !has_interesting; li++) {
                this_diff = sline[j].lost[li].parent_map;
                if (!same_diff)
                    same_diff = this_diff;
                else if (same_diff != this_diff)
                    has_interesting = 1;
            }
        }

        if (!has_interesting && same_diff != SG_COMBINE_ALL_MASK) {
            for (j = hunk_begin; j < hunk_end; j++)
                sline[j].flag &= ~SG_COMBINE_MARK;
        }
        i = hunk_end;
    }

    return combine_give_context(sline, cnt);
}

static int combine_is_space(int ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\v' || ch == '\f';
}

/* Function-name candidate: combine-diff.c's OWN minimal heuristic
   (hunk_comment_line), unrelated to find_function_name's xdiff-style rule
   used by the 2-way patch body -- do not merge the two. */
static int combine_comment_line(const sg_diff_line *line)
{
    int ch;

    if (line->len == 0)
        return 0;
    ch = (unsigned char)line->ptr[0];
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_' || ch == '$';
}

/* dump_sline's "@@@ -a,b -c,d +e,f @@@[ <funcname>]" header, including the
   off-by-one funcname suffix (PHASE34_ORACLE.md #4: comment_end records the
   INDEX of the last non-blank byte, and the print loop stops BEFORE it, so
   a trailing "{" silently disappears -- this is git's own behaviour, not a
   bug to fix here).

   `comment_limit` bounds the 40-byte scan to the remaining bytes in the
   result buffer: real git scans a NUL/newline-terminated mmap and can walk
   past the funcname candidate's own line into whatever follows in memory.
   sg's buffers are plain malloc'd and NOT padded, so scanning unconditionally
   up to 40 bytes risks a heap-buffer-overflow read past the allocation on a
   file whose last line is a short, newline-less funcname candidate --
   clamping to what is actually left in the buffer is the deliberate,
   memory-safe adaptation; it only differs from git's output in that exact
   edge case (a funcname line that is simultaneously both the hunk's
   trailing context AND the last, newline-less bytes of the file). */
static void combine_print_hunk_header(const combine_sline *sline, size_t lno, size_t hunk_end, size_t cnt,
                                      const sg_diff_line *hunk_comment, size_t comment_limit)
{
    size_t rlines = hunk_end - lno;
    int n;

    if (hunk_end > cnt)
        rlines--;

    printf("@@@");
    for (n = 0; n < 2; n++) {
        size_t l0 = sline[lno].p_lno[n];
        size_t l1 = sline[hunk_end].p_lno[n];

        printf(" -%zu,%zu", l0, l1 - l0);
    }
    printf(" +%zu,%zu ", lno + 1, rlines);
    printf("@@@");

    if (hunk_comment != NULL) {
        size_t max_i = 40;
        int comment_end = 0;
        size_t i;

        if (comment_limit < max_i)
            max_i = comment_limit;
        for (i = 0; i < max_i; i++) {
            int ch = (unsigned char)hunk_comment->ptr[i];

            if (ch == 0 || ch == '\n')
                break;
            if (!combine_is_space(ch))
                comment_end = (int)i;
        }
        if (comment_end != 0)
            putchar(' ');
        for (i = 0; i < (size_t)comment_end; i++)
            putchar(hunk_comment->ptr[i]);
    }
    putchar('\n');
}

/* dump_sline's body: walk hunks in order, and within each hunk, for every
   result position print its attached (already-coalesced) lost lines first,
   then its own line -- unless SG_COMBINE_NO_PRE_DELETE says this position's
   lost lines were already shown as part of an earlier hunk's trailing
   context (give_context's job). */
static void combine_dump(const combine_sline *sline, size_t cnt, const unsigned char *result_data,
                         size_t result_len)
{
    size_t lno = 0;
    /* `result_data + result_len` is UNDEFINED when result_data is NULL, even
       with result_len 0 -- and a merge's combined row CAN have no result
       buffer at all (a path the merge deleted, whose hunk body a
       tree-sourced row still prints). glibc's UBSan calls that "applying
       zero offset to null pointer" and the CI job halts on it; macOS's
       stayed silent, so this only ever showed up on Linux. */
    const unsigned char *result_end = result_data != NULL ? result_data + result_len : NULL;

    for (;;) {
        size_t hunk_end;
        const sg_diff_line *hunk_comment = NULL;

        while (lno <= cnt && !(sline[lno].flag & SG_COMBINE_MARK)) {
            if (combine_comment_line(&sline[lno].line))
                hunk_comment = &sline[lno].line;
            lno++;
        }
        if (cnt < lno)
            break;
        for (hunk_end = lno + 1; hunk_end <= cnt; hunk_end++)
            if (!(sline[hunk_end].flag & SG_COMBINE_MARK))
                break;

        {
            size_t comment_limit = 0;

            if (hunk_comment != NULL)
                comment_limit = (size_t)(result_end - (const unsigned char *)hunk_comment->ptr);
            combine_print_hunk_header(sline, lno, hunk_end, cnt, hunk_comment, comment_limit);
        }

        while (lno < hunk_end) {
            const combine_sline *sl = &sline[lno++];
            const combine_lost *ll = (sl->flag & SG_COMBINE_NO_PRE_DELETE) ? NULL : sl->lost;
            int j;

            if (ll != NULL) {
                size_t li;

                for (li = 0; li < sl->lost_count; li++) {
                    for (j = 0; j < 2; j++)
                        putchar((sl->lost[li].parent_map & (1u << j)) ? '-' : ' ');
                    printf("%.*s\n", (int)sl->lost[li].text.len, sl->lost[li].text.ptr);
                }
            }
            if (cnt < lno)
                break;
            for (j = 0; j < 2; j++)
                putchar((sl->flag & (1u << j)) ? '+' : ' ');
            printf("%.*s\n", (int)sl->line.len, sl->line.ptr);
        }
    }
}

/* Header + hunk body for one combinable unmerged row. `dense` is
   sg_diff_out_opts.combined != 2 -- see that header comment for the "0 means
   PATCH's implicit dense default" rule. Reads ours/theirs/result itself
   (print_patch never reads e->old_side/e->new_side for an unmerged row --
   both stay ABSENT by contract, see sg/diff.h). Returns 0, or -1 after
   printing a message (same convention as build_entry_stat/print_patch). */
static int render_combined_patch(const char *git_dir, const char *repo_root, const sg_diff_entry *e, int dense,
                                 sg_diff_algorithm algo)
{
    unsigned char *ours_data = NULL, *theirs_data = NULL, *result_data = NULL;
    size_t ours_len = 0, theirs_len = 0, result_len = 0;
    sg_chunk_missing_info missing;
    int rc;
    int deleted;
    int is_binary;
    int mode_differs;
    unsigned char ours_eff[SG_SHA1_RAW_LEN], theirs_eff[SG_SHA1_RAW_LEN];
    char ours_hex[SG_SHA1_HEX_LEN + 1], theirs_hex[SG_SHA1_HEX_LEN + 1];
    unsigned int result_mode;
    int added; /* Phase 55b: added by the merge itself -- both parents ABSENT */
    char result_hex[SG_SHA1_HEX_LEN + 1];

    rc = sg_diff_side_read(git_dir, repo_root, e->path, &e->ours, &ours_data, &ours_len, &missing);
    if (rc == -2) {
        sg_chunk_print_missing_error(e->path, &missing);
        return -1;
    }
    if (rc != 0) {
        fprintf(stderr, "sg: warning: cannot read %s\n", sg_quote_path_delimited(e->path));
        return -1;
    }
    rc = sg_diff_side_read(git_dir, repo_root, e->path, &e->theirs, &theirs_data, &theirs_len, &missing);
    if (rc == -2) {
        sg_chunk_print_missing_error(e->path, &missing);
        free(ours_data);
        return -1;
    }
    if (rc != 0) {
        fprintf(stderr, "sg: warning: cannot read %s\n", sg_quote_path_delimited(e->path));
        free(ours_data);
        return -1;
    }
    rc = sg_diff_side_read(git_dir, repo_root, e->path, &e->result, &result_data, &result_len, &missing);
    if (rc == -2) {
        sg_chunk_print_missing_error(e->path, &missing);
        free(ours_data);
        free(theirs_data);
        return -1;
    }
    if (rc != 0) {
        fprintf(stderr, "sg: warning: cannot read %s\n", sg_quote_path_delimited(e->path));
        free(ours_data);
        free(theirs_data);
        return -1;
    }

    deleted = e->result.kind == SG_DIFF_SIDE_ABSENT;
    /* Phase 55b: a merge's own combined row (sg_diff_combined_from_trees)
       can have BOTH parent sides ABSENT -- a path present in neither parent
       that the merge itself added. Neither of the existing two producers
       can reach this (sg_diff_entry_is_combined requires ours/theirs both
       non-ABSENT for them), so this is unambiguous: it only ever fires for
       a combined_row entry. */
    added = e->ours.kind == SG_DIFF_SIDE_ABSENT && e->theirs.kind == SG_DIFF_SIDE_ABSENT;
    result_mode = deleted ? 0 : e->result.mode;
    /* git: mode_differs iff ANY parent's mode != the result's (0 for
       deleted) -- NOT "the two parents differ from each other". */
    mode_differs = (e->ours.mode != result_mode) || (e->theirs.mode != result_mode);

    is_binary = is_binary_data(ours_data, ours_len) || is_binary_data(theirs_data, theirs_len) ||
               is_binary_data(result_data, result_len);

    sg_diff_side_effective_id(git_dir, &e->ours, ours_eff);
    sg_diff_side_effective_id(git_dir, &e->theirs, theirs_eff);
    sg_sha1_to_hex(ours_eff, ours_hex);
    sg_sha1_to_hex(theirs_eff, theirs_hex);
    ours_hex[7] = '\0';
    theirs_hex[7] = '\0';

    printf("diff %s %s\n", dense ? "--cc" : "--combined", sg_quote_path(e->path));
    /* The destination side is a real object only for a merge commit's own
       combined row (Phase 55b, result.kind == BLOB, straight out of the
       result tree) -- every other producer's result is the working tree
       copy, which is never in the object store, so it stays "0000000"
       (PHASE34_ORACLE.md #3). */
    if (e->result.kind == SG_DIFF_SIDE_BLOB) {
        unsigned char result_eff[SG_SHA1_RAW_LEN];

        sg_diff_side_effective_id(git_dir, &e->result, result_eff);
        sg_sha1_to_hex(result_eff, result_hex);
        result_hex[7] = '\0';
    } else {
        strcpy(result_hex, "0000000");
    }
    printf("index %s,%s..%s\n", ours_hex, theirs_hex, result_hex);
    if (added) {
        printf("new file mode %06o\n", result_mode);
    } else if (mode_differs) {
        if (deleted)
            printf("deleted file mode %06o,%06o\n", e->ours.mode, e->theirs.mode);
        else
            printf("mode %06o,%06o..%06o\n", e->ours.mode, e->theirs.mode, result_mode);
    }

    if (is_binary) {
        printf("Binary files differ\n");
        free(ours_data);
        free(theirs_data);
        free(result_data);
        return 0;
    }

    printf("--- %s\n", added ? "/dev/null" : sg_quote_path_prefixed("a/", e->path));
    printf("+++ %s\n", deleted ? "/dev/null" : sg_quote_path_prefixed("b/", e->path));

    if (deleted && !e->combined_row) {
        /* No hunk body at all for a deleted result -- PHASE34_ORACLE.md
           sample (E) / PHASE34_ALGO.md #7 (result_deleted skips
           combine_diff and dump_sline both). Measured only for producer 1's
           shape (an unresolved conflict resolved by deleting the working
           tree file): `git diff --cc` there prints "--- a/x" / "+++
           /dev/null" and nothing else. Phase 55b's merge-commit combined
           rows (combined_row) are a DIFFERENT producer and were measured to
           differ here -- `git show <merge>` on a path both parents edited
           and the merge deleted DOES print a hunk showing what each parent
           removed, same shape as an ordinary deletion's hunk body. */
        free(ours_data);
        free(theirs_data);
        free(result_data);
        return 0;
    }

    {
        size_t na, nb, ncnt;
        sg_diff_line *a_lines = sg_diff_split_lines(ours_data, ours_len, &na);
        sg_diff_line *b_lines = sg_diff_split_lines(theirs_data, theirs_len, &nb);
        sg_diff_line *r_lines = sg_diff_split_lines(result_data, result_len, &ncnt);
        combine_sline *sline;
        size_t i;
        int had_error = 0;

        sline = calloc(ncnt + 2, sizeof(*sline));
        if (sline == NULL || (na != 0 && a_lines == NULL) || (nb != 0 && b_lines == NULL) ||
           (ncnt != 0 && r_lines == NULL)) {
            free(sline);
            free(a_lines);
            free(b_lines);
            free(r_lines);
            free(ours_data);
            free(theirs_data);
            free(result_data);
            fprintf(stderr, "sg: warning: out of memory diffing %s\n", sg_quote_path_delimited(e->path));
            return -1;
        }
        for (i = 0; i < ncnt; i++)
            sline[i].line = r_lines[i];

        if (combine_process_parent(sline, ncnt, a_lines, na, r_lines, 0, algo) != 0 ||
           combine_process_parent(sline, ncnt, b_lines, nb, r_lines, 1, algo) != 0)
            had_error = 1;

        if (!had_error) {
            combine_make_hunks(sline, ncnt, dense);
            combine_dump(sline, ncnt, result_data, result_len);
        }

        for (i = 0; i < ncnt + 2; i++) {
            free(sline[i].lost);
            free(sline[i].plost);
        }
        free(sline);
        free(a_lines);
        free(b_lines);
        free(r_lines);
        free(ours_data);
        free(theirs_data);
        free(result_data);

        if (had_error) {
            fprintf(stderr, "sg: warning: out of memory diffing %s\n", sg_quote_path_delimited(e->path));
            return -1;
        }
    }
    return 0;
}

static int print_patch(const char *git_dir, const char *repo_root, const sg_diff_list *list, int combined,
                       sg_diff_algorithm algo)
{
    size_t i;
    int had_error = 0;
    /* PATCH's default IS dense combined, even with no -c/--cc at all
       (PHASE34_ORACLE.md #2) -- only an explicit -c (combined == 2) turns
       off density. */
    int dense = combined != 2;
    int skip_next = 0;

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

        if (skip_next) {
            skip_next = 0;
            continue;
        }

        if (e->unmerged) {
            /* The companion row (stage2-vs-workdir) that
               sg_diff_index_workdir appends right after a combinable
               unmerged row is a plain, non-unmerged entry sharing the same
               path -- render the combined diff in its place and swallow it,
               same as real git (PHASE34_ORACLE.md #1: "no longer prints the
               companion's own 2-way patch"). Deliberately NOT gated on
               `combined != 0` -- PATCH's default IS dense combined for a
               real conflict, even with no -c/--cc at all (PHASE34_ORACLE.md
               #2). */
            if (combinable(e)) {
                if (render_combined_patch(git_dir, repo_root, e, dense, algo) != 0)
                    had_error = 1;
                if (has_companion_row(list, i))
                    skip_next = 1;
                continue;
            }
            /* Not combinable (only one of stage2/stage3 present) --
               "* Unmerged path" plus, when it exists, the ordinary 2-way
               companion row right after it (PHASE34_ORACLE.md #1). Path is
               deliberately unquoted -- see the "printing a path" rule's
               fourth exception in CLAUDE.md. */
            printf("* Unmerged path %s\n", e->path);
            continue;
        }

        /* Phase 40: a combined row from sg_diff_fill_combined_from_index
           (`sg diff -c/--cc <rev>`) is the OPPOSITE of the case above -- it
           only renders combined when the flag was actually given (SPEC
           section 1: "-c/--cc with no rev given has no effect" degenerates
           because ordinary sg_diff_tree_workdir rows never set unmerged,
           but a row THIS pass filled must still check the flag itself). */
        if (combined != 0 && combinable(e)) {
            if (render_combined_patch(git_dir, repo_root, e, dense, algo) != 0)
                had_error = 1;
            continue;
        }

        old_present = e->old_side.kind != SG_DIFF_SIDE_ABSENT;
        new_present = e->new_side.kind != SG_DIFF_SIDE_ABSENT;

        {
            int old_verified = sg_diff_side_effective_id(git_dir, &e->old_side, old_eff) == 0;
            int new_verified = sg_diff_side_effective_id(git_dir, &e->new_side, new_eff) == 0;

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

        /* The a/ side of a rename is where the content came from -- measured
           against git 2.55.0: "diff --git a/exact.txt b/exact_new.txt". */
        printf("diff --git %s %s\n",
              sg_quote_path_prefixed("a/", e->old_path != NULL ? e->old_path : e->path),
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

        /* A rename's own three lines come AFTER any mode lines and BEFORE
           the index line -- measured against git 2.55.0, which prints
           "old mode"/"new mode", then "similarity index 100%", then
           "rename from"/"rename to". The two paths are quoted with
           sg_quote_path (no a//b/ prefix on these two lines: measured
           `rename to "tab\there.txt"`). */
        if (e->old_path != NULL) {
            /* "copy from"/"copy to" when the source is still there --
               measured against git 2.55.0; the similarity line above is
               worded identically either way. */
            printf("similarity index %d%%\n", e->score);
            printf("%s from %s\n", e->is_copy ? "copy" : "rename",
                  sg_quote_path(e->old_path));
            printf("%s to %s\n", e->is_copy ? "copy" : "rename",
                  sg_quote_path(e->path));
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

        rc = sg_diff_side_read(git_dir, repo_root, old_side_path(e), &e->old_side, &a_data, &a_len, &missing);
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
            /* The a/ side names the OLD path, which differs from the new
               one for a rename -- measured against git 2.55.0, which prints
               "Binary files a/b.bin and b/c.bin differ". */
            printf("Binary files %s and %s differ\n",
                  old_present ? sg_quote_path_prefixed("a/", old_side_path(e)) : "/dev/null",
                  new_present ? sg_quote_path_prefixed("b/", e->path) : "/dev/null");
            free(a_data);
            free(b_data);
            continue;
        }

        if (print_text_diff_body(old_side_path(e), e->path, old_present, new_present,
                                 a_data, a_len, b_data, b_len, algo) != 0) {
            fprintf(stderr, "sg: warning: out of memory diffing %s\n", sg_quote_path_delimited(e->path));
            had_error = 1;
        }
        free(a_data);
        free(b_data);
    }

    return had_error ? -1 : 0;
}

/* ---- NAME_ONLY / NAME_STATUS (no content reads needed) ---------------- */

static int print_name_only(const sg_diff_list *list, int combined)
{
    size_t i;
    int skip_next = 0;

    for (i = 0; i < list->count; i++) {
        const sg_diff_entry *e = &list->entries[i];

        if (skip_next) {
            skip_next = 0;
            continue;
        }
        printf("%s\n", sg_quote_path(e->path));
        if (combined != 0 && combinable(e) && has_companion_row(list, i))
            skip_next = 1;
    }
    return 0;
}

static int print_name_status(const sg_diff_list *list, int combined)
{
    size_t i;
    int skip_next = 0;

    for (i = 0; i < list->count; i++) {
        const sg_diff_entry *e = &list->entries[i];

        if (skip_next) {
            skip_next = 0;
            continue;
        }

        /* A rename is the one row with two paths and a score. Measured
           against git 2.55.0: "R100\told\tnew", the score three digits and
           zero-padded. Both paths get sg_quote_path, same as every other
           row in this format. Two calls in one printf is within the four
           rotating buffers sg_quote_path hands out (see sg/quote.h). */
        if (e->old_path != NULL) {
            printf("%c%03d\t%s\t%s\n", e->is_copy ? 'C' : 'R', e->score,
                  sg_quote_path(e->old_path),
                  sg_quote_path(e->path));
            continue;
        }
        /* Combinable + -c/--cc: one row, two status letters (one per
           parent) -- measured "MM" in every shape tested
           (PHASE34_ORACLE.md #2), never any other two-letter combination. */
        if (combined != 0 && combinable(e)) {
            printf("MM\t%s\n", sg_quote_path(e->path));
            if (has_companion_row(list, i))
                skip_next = 1;
            continue;
        }
        printf("%c\t%s\n", entry_status(e), sg_quote_path(e->path));
    }
    return 0;
}

/* ---- NUMSTAT ------------------------------------------------------------ */

/* git's pprint_rename: the display name for a rename row, shared by --stat
   and --numstat (--name-status prints the two paths as separate fields
   instead, and the patch header has its own shape).

   The common prefix and the common suffix are measured at '/' boundaries
   only, and what is left in the middle goes inside braces. Measured against
   git 2.55.0:
     a/b/c.txt   -> a/z/c.txt      =>  a/{b => z}/c.txt
     deep/p/q/r  -> deep/p/q2/r    =>  deep/p/{q => q2}/r.txt
     h/i/j.txt   -> h2/i/j.txt     =>  {h => h2}/i/j.txt
     x/y.txt     -> x/y2.txt       =>  x/{y.txt => y2.txt}
     pre.txt     -> pre.txt.bak    =>  pre.txt => pre.txt.bak
   The last one is why the boundaries matter: "pre.txt" is a common prefix by
   bytes but not up to a '/', so there is no compression at all rather than
   "pre.txt{ => .bak}".

   WARNING: a path that needs C-quoting turns compression OFF entirely -- measured,
   git prints `d/plain.txt => "d/tab\there.txt"`, not `d/{plain.txt => ...}`.
   Quoting the pieces of a braced form would produce quotes in the middle of
   a path, which no consumer could unquote.

   Returns a malloc'd string the caller owns, or NULL if allocation fails.
   Sized from the inputs rather than written into a fixed buffer on purpose:
   a first version took a caller-supplied char[SG_PATH_MAX * 2], which two
   4095-byte paths overflow (4095 + 4 + 4095 + 1 > 8192), and its overflow
   path returned the destination path alone -- output indistinguishable from
   an ordinary addition, with the rename silently gone. Quoting makes the
   bound worse still, since one byte can expand to four. Tree entry names are
   not length-validated anywhere in this codebase (src/object/tree.c), so
   those lengths are reachable from a crafted tree, not just hypothetical. */
static char *rename_pair_display(const char *old_path, const char *new_path)
{
    const char *q_old = sg_quote_path(old_path);
    const char *q_new = sg_quote_path(new_path);
    size_t len_old = strlen(old_path);
    size_t len_new = strlen(new_path);
    size_t pfx = 0;
    size_t sfx = 0;
    size_t i;
    size_t need;
    char *out;

    /* sg_quote_path only adds quotes when it had to escape something, so a
       leading '"' is exactly "this path needed quoting" -- a path whose own
       first byte is '"' needs quoting for that very reason. Both quoted
       forms are read here while still live: two of the ring's four slots
       (sg/quote.h), and no further sg_quote_path call happens before they
       are copied out. */
    if (q_old[0] == '"' || q_new[0] == '"') {
        need = strlen(q_old) + 4 + strlen(q_new) + 1;
        out = malloc(need);
        if (out == NULL)
            return NULL;
        snprintf(out, need, "%s => %s", q_old, q_new);
        return out;
    }

    for (i = 0; i < len_old && i < len_new && old_path[i] == new_path[i]; i++) {
        if (old_path[i] == '/')
            pfx = i + 1;
    }
    /* Symmetric with the prefix loop above: keep scanning the whole common
       tail and record the position each time it lands on a '/', so the
       LONGEST component-aligned suffix wins. Stopping at the first '/' found
       going backwards instead gives "{h/i => h2/i}/j.txt" where git says
       "{h => h2}/i/j.txt". Only a '/' ever sets sfx, which is why "pre.txt"
       vs "pre.txt.bak" -- bytes in common but no component in common --
       comes out uncompressed. */
    for (i = 0; i < len_old - pfx && i < len_new - pfx &&
                old_path[len_old - i - 1] == new_path[len_new - i - 1]; ) {
        i++;
        if (old_path[len_old - i] == '/')
            sfx = i;
    }

    if (pfx == 0 && sfx == 0) {
        need = len_old + 4 + len_new + 1;
        out = malloc(need);
        if (out == NULL)
            return NULL;
        snprintf(out, need, "%s => %s", old_path, new_path);
        return out;
    }

    /* pfx + mid_old + " => " + mid_new + sfx + "{}" + NUL, bounded above by
       both whole paths plus the fixed punctuation. */
    need = len_old + len_new + 8;
    out = malloc(need);
    if (out == NULL)
        return NULL;
    snprintf(out, need, "%.*s{%.*s => %.*s}%.*s",
            (int)pfx, old_path,
            (int)(len_old - pfx - sfx), old_path + pfx,
            (int)(len_new - pfx - sfx), new_path + pfx,
            (int)sfx, old_path + len_old - sfx);
    return out;
}

/* The display name for any row: the rename pair when there is one, the
   quoted path otherwise. Always malloc'd so the caller frees exactly one
   thing either way; NULL on allocation failure. */
static char *entry_display_name(const sg_diff_entry *e)
{
    if (e->old_path != NULL)
        return rename_pair_display(e->old_path, e->path);
    return strdup(sg_quote_path(e->path));
}

static int print_numstat(const char *git_dir, const char *repo_root, const sg_diff_list *list, int combined,
                         sg_diff_algorithm algo)
{
    size_t i;
    int had_error = 0;
    int skip_next = 0;

    for (i = 0; i < list->count; i++) {
        const sg_diff_entry *e = &list->entries[i];
        entry_stat st;

        if (skip_next) {
            skip_next = 0;
            continue;
        }
        /* Combinable + -c/--cc: the row disappears entirely, not even
           "0\t0" -- and so does its companion (PHASE34_ORACLE.md #2). */
        if (combined != 0 && combinable(e)) {
            if (has_companion_row(list, i))
                skip_next = 1;
            continue;
        }

        if (build_entry_stat(git_dir, repo_root, e, algo, &st) != 0) {
            had_error = 1;
            continue;
        }

        {
            char *name = entry_display_name(e);

            if (name == NULL) {
                had_error = 1;
                continue;
            }
            if (st.unmerged)
                printf("0\t0\t%s\n", name);
            else if (st.is_binary)
                printf("-\t-\t%s\n", name);
            else
                printf("%ld\t%ld\t%s\n", st.added, st.deleted, name);
            free(name);
        }
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
                           int combined, sg_diff_algorithm algo, stat_row **rows_out,
                           size_t *count_out, int *had_error)
{
    stat_row *rows;
    size_t n = 0, i;
    int skip_next = 0;

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

        if (skip_next) {
            skip_next = 0;
            continue;
        }
        /* Combinable + -c/--cc: the row (and its companion) vanish entirely
           -- not even " Unmerged" (PHASE34_ORACLE.md #2, --stat's own row
           disappears too, unlike --numstat's still-present-but-zero shape
           for other formats). */
        if (combined != 0 && combinable(e)) {
            if (has_companion_row(list, i))
                skip_next = 1;
            continue;
        }

        if (build_entry_stat(git_dir, repo_root, e, algo, &st) != 0) {
            *had_error = 1;
            continue;
        }

        /* Same display name --numstat uses, so the two formats can never
           disagree about how a rename is spelled. Already owned, so there is
           nothing left to strdup. */
        rows[n].name = entry_display_name(e);
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

    if (build_stat_rows(git_dir, repo_root, list, opts->combined, opts->algorithm, &rows, &n,
                       &had_error) != 0)
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

/* git's --summary block. Printed after the format's own output, never
   instead of it -- see sg_diff_out_opts.summary for the measured line
   shapes. Entries are walked in list order, which is path order, which is
   the order the stat above it used. An unmerged row is skipped: --summary
   describes tree-to-tree structure changes and the only caller diffs two
   trees, where an unmerged row cannot arise. */
static int print_extended_summary(const sg_diff_list *list)
{
    size_t i;

    for (i = 0; i < list->count; i++) {
        const sg_diff_entry *e = &list->entries[i];
        int named_by_rename = 0;

        if (e->unmerged)
            continue;

        if (e->old_path != NULL) {
            char *disp = entry_display_name(e);

            if (disp == NULL)
                return -1;
            printf(" %s %s (%d%%)\n", e->is_copy ? "copy" : "rename", disp, e->score);
            free(disp);
            named_by_rename = 1;
        } else if (e->old_side.kind == SG_DIFF_SIDE_ABSENT) {
            printf(" create mode %06o %s\n", e->new_side.mode, sg_quote_path(e->path));
            continue;
        } else if (e->new_side.kind == SG_DIFF_SIDE_ABSENT) {
            printf(" delete mode %06o %s\n", e->old_side.mode, sg_quote_path(e->path));
            continue;
        }

        /* A mode change on a surviving path. Both modes have to be known:
           0 means "unknown" for a side that never carried one. */
        if (e->old_side.kind != SG_DIFF_SIDE_ABSENT && e->new_side.kind != SG_DIFF_SIDE_ABSENT &&
           e->old_side.mode != 0 && e->new_side.mode != 0 &&
           e->old_side.mode != e->new_side.mode) {
            if (named_by_rename)
                printf(" mode change %06o => %06o\n", e->old_side.mode, e->new_side.mode);
            else
                printf(" mode change %06o => %06o %s\n", e->old_side.mode, e->new_side.mode,
                       sg_quote_path(e->path));
        }
    }
    return 0;
}

int sg_diff_print(const char *git_dir, const char *repo_root, const sg_diff_list *list,
                  const sg_diff_out_opts *opts)
{
    int rc;

    switch (opts->format) {
    case SG_DIFF_FORMAT_PATCH:
        rc = print_patch(git_dir, repo_root, list, opts->combined, opts->algorithm);
        break;
    case SG_DIFF_FORMAT_STAT:
        rc = print_stat(git_dir, repo_root, list, opts, 0);
        break;
    case SG_DIFF_FORMAT_SHORTSTAT:
        rc = print_stat(git_dir, repo_root, list, opts, 1);
        break;
    case SG_DIFF_FORMAT_NUMSTAT:
        rc = print_numstat(git_dir, repo_root, list, opts->combined, opts->algorithm);
        break;
    case SG_DIFF_FORMAT_NAME_ONLY:
        rc = print_name_only(list, opts->combined);
        break;
    case SG_DIFF_FORMAT_NAME_STATUS:
        rc = print_name_status(list, opts->combined);
        break;
    default:
        return -1;
    }
    if (rc != 0 || !opts->summary)
        return rc;
    return print_extended_summary(list);
}
