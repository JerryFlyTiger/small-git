#include "sg/commit_out.h"

#include "sg/date.h"
#include "sg/diff.h"
#include "sg/diff_out.h"
#include "sg/objstore.h"
#include "sg/similarity.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ASCII-only case fold, deliberately not strcasecmp -- same reasoning as
   sg_path_component_is_safe's own comment in workdir.h: strcasecmp is
   locale-dependent, and the builtin format names are ASCII identifiers, so
   there is no reason to let a locale's idea of casing anywhere near this. */
static int ascii_ci_equal(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        char ca = *a, cb = *b;

        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static const struct {
    const char *name;
    sg_pretty_kind kind;
} PRETTY_BUILTINS[] = {
    { "oneline", SG_PRETTY_ONELINE },
    { "short", SG_PRETTY_SHORT },
    { "medium", SG_PRETTY_MEDIUM },
    { "full", SG_PRETTY_FULL },
    { "fuller", SG_PRETTY_FULLER },
    { "raw", SG_PRETTY_RAW },
    { "reference", SG_PRETTY_REFERENCE },
};

/* The five ordered grammar rules, CLAUDE.md's `sg log` entry has the full
   measured table. Rule order matters: "format:"/"tformat:" (case-SENSITIVE)
   are checked before the case-INSENSITIVE builtin lookup, which is in turn
   checked before the "contains a %" fallback -- e.g. "oneline%H" must NOT
   match rule 3 (whole-string equality, not a prefix). */
int sg_pretty_parse(const char *arg, sg_pretty_format *out)
{
    size_t i;

    if (arg == NULL || out == NULL)
        return -1;
    out->user_format = NULL;

    if (strncmp(arg, "format:", 7) == 0) {
        out->kind = SG_PRETTY_FORMAT;
        out->user_format = arg + 7;
        return 0;
    }
    if (strncmp(arg, "tformat:", 8) == 0) {
        out->kind = SG_PRETTY_TFORMAT;
        out->user_format = arg + 8;
        return 0;
    }
    for (i = 0; i < sizeof(PRETTY_BUILTINS) / sizeof(PRETTY_BUILTINS[0]); i++) {
        if (ascii_ci_equal(arg, PRETTY_BUILTINS[i].name)) {
            out->kind = PRETTY_BUILTINS[i].kind;
            return 0;
        }
    }
    if (strchr(arg, '%') != NULL) {
        out->kind = SG_PRETTY_TFORMAT;
        out->user_format = arg;
        return 0;
    }
    return -1;
}

/* Phase 60b: the placeholder table, section 5.1 of the spec. One token per
   recognized `%`-sequence; decode_placeholder is the single place that
   knows the grammar, shared by the validator (sg_pretty_validate_format)
   and the renderer (expand_user_format) so the two can never drift apart --
   CLAUDE.md's Phase 60a note about "seven branches is how the eighth format
   gets forgotten" applies here just as much as it did to the tag header
   table. */
typedef enum {
    PH_HASH_FULL, PH_HASH_ABBR, PH_TREE_FULL, PH_TREE_ABBR,
    PH_PARENTS_FULL, PH_PARENTS_ABBR,
    PH_A_NAME, PH_A_EMAIL, PH_A_LOCAL, PH_A_DATE, PH_A_DATE_RFC2822,
    PH_A_DATE_UNIX, PH_A_DATE_ISO, PH_A_DATE_ISO_STRICT, PH_A_DATE_SHORT,
    PH_C_NAME, PH_C_EMAIL, PH_C_LOCAL, PH_C_DATE, PH_C_DATE_RFC2822,
    PH_C_DATE_UNIX, PH_C_DATE_ISO, PH_C_DATE_ISO_STRICT, PH_C_DATE_SHORT,
    PH_SUBJECT, PH_SANITIZED_SUBJECT, PH_BODY, PH_RAW_BODY,
    PH_NEWLINE, PH_PERCENT, PH_HEX_BYTE
} sg_ph_kind;

typedef struct {
    sg_ph_kind kind;
    size_t consumed;      /* total bytes consumed, including the leading '%' */
    unsigned char hexval; /* only meaningful for PH_HEX_BYTE */
} sg_ph_token;

/* %a<c>/%c<c> share the same ten-way suffix table -- author and committer
   differ only in which struct fields feed the renderer, never in which
   suffix letters are legal. */
static const struct {
    char c;
    sg_ph_kind author_kind;
    sg_ph_kind committer_kind;
} PH_DATE_SUFFIX[] = {
    { 'n', PH_A_NAME, PH_C_NAME },
    { 'e', PH_A_EMAIL, PH_C_EMAIL },
    { 'l', PH_A_LOCAL, PH_C_LOCAL },
    { 'd', PH_A_DATE, PH_C_DATE },
    { 'D', PH_A_DATE_RFC2822, PH_C_DATE_RFC2822 },
    { 't', PH_A_DATE_UNIX, PH_C_DATE_UNIX },
    { 'i', PH_A_DATE_ISO, PH_C_DATE_ISO },
    { 'I', PH_A_DATE_ISO_STRICT, PH_C_DATE_ISO_STRICT },
    { 's', PH_A_DATE_SHORT, PH_C_DATE_SHORT },
};

/* Single-char placeholders reached only when the char after `%` is none of
   'a'/'c'/'n'/'x'/'%' -- no ambiguity with %at/%ct etc, those are always
   consumed by the 'a'/'c' branch first. */
static const struct {
    char c;
    sg_ph_kind kind;
} PH_SINGLE[] = {
    { 'H', PH_HASH_FULL }, { 'h', PH_HASH_ABBR },
    { 'T', PH_TREE_FULL }, { 't', PH_TREE_ABBR },
    { 'P', PH_PARENTS_FULL }, { 'p', PH_PARENTS_ABBR },
    { 's', PH_SUBJECT }, { 'f', PH_SANITIZED_SUBJECT },
    { 'b', PH_BODY }, { 'B', PH_RAW_BODY },
};

static int ph_hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* p[0] must be '%'. On success fills *tok and returns 0. On failure returns
   -1 and fills *bad_len with how many bytes (starting at p, '%' included)
   name the offending placeholder for an error message -- e.g. 1 for a
   trailing lone '%', 3 for "%ar" (unrecognized suffix after %a), 2 for an
   unrecognized single char like "%z". */
static int decode_placeholder(const char *p, sg_ph_token *tok, size_t *bad_len)
{
    char c1 = p[1];
    size_t i;

    if (c1 == '\0') {
        *bad_len = 1;
        return -1;
    }
    if (c1 == '%') {
        tok->kind = PH_PERCENT;
        tok->consumed = 2;
        return 0;
    }
    if (c1 == 'n') {
        tok->kind = PH_NEWLINE;
        tok->consumed = 2;
        return 0;
    }
    if (c1 == 'x') {
        int h1, h2;

        h1 = p[2] == '\0' ? -1 : ph_hex_digit(p[2]);
        if (h1 < 0) {
            *bad_len = 2;
            return -1;
        }
        h2 = p[3] == '\0' ? -1 : ph_hex_digit(p[3]);
        if (h2 < 0) {
            *bad_len = 3;
            return -1;
        }
        tok->kind = PH_HEX_BYTE;
        tok->hexval = (unsigned char)(h1 * 16 + h2);
        tok->consumed = 4;
        return 0;
    }
    if (c1 == 'a' || c1 == 'c') {
        char c2 = p[2];

        if (c2 == '\0') {
            *bad_len = 2;
            return -1;
        }
        for (i = 0; i < sizeof(PH_DATE_SUFFIX) / sizeof(PH_DATE_SUFFIX[0]); i++) {
            if (PH_DATE_SUFFIX[i].c == c2) {
                tok->kind = c1 == 'a' ? PH_DATE_SUFFIX[i].author_kind : PH_DATE_SUFFIX[i].committer_kind;
                tok->consumed = 3;
                return 0;
            }
        }
        *bad_len = 3;
        return -1;
    }
    for (i = 0; i < sizeof(PH_SINGLE) / sizeof(PH_SINGLE[0]); i++) {
        if (PH_SINGLE[i].c == c1) {
            tok->kind = PH_SINGLE[i].kind;
            tok->consumed = 2;
            return 0;
        }
    }
    *bad_len = 2;
    return -1;
}

int sg_pretty_validate_format(const char *fmt, const char **bad, size_t *bad_len)
{
    const char *p = fmt;

    if (fmt == NULL)
        return 0;
    while (*p != '\0') {
        if (*p == '%') {
            sg_ph_token tok;
            size_t blen = 0;

            if (decode_placeholder(p, &tok, &blen) != 0) {
                if (bad != NULL)
                    *bad = p;
                if (bad_len != NULL)
                    *bad_len = blen;
                return -1;
            }
            p += tok.consumed;
        } else {
            p++;
        }
    }
    return 0;
}

/* Prints id's 40-hex, or its 7-hex abbreviation (SG_COMMIT_OUT_ABBREV) when
   abbrev is nonzero. Shared by %H/%h, %T/%t, and each entry of %P/%p. */
static void print_id_hex(const unsigned char id[SG_SHA1_RAW_LEN], int abbrev)
{
    char hex[SG_SHA1_HEX_LEN + 1];

    sg_sha1_to_hex(id, hex);
    if (abbrev)
        printf("%.*s", SG_COMMIT_OUT_ABBREV, hex);
    else
        fputs(hex, stdout);
}

/* %P/%p: every parent, space separated -- measured on a 2-parent merge. A
   root commit's parent_count is 0, so this prints nothing at all (not even
   a lone separator), matching the empty %P/%p real git prints for it. */
static void print_parents(const sg_commit *commit, int abbrev)
{
    size_t i;

    for (i = 0; i < commit->parent_count; i++) {
        if (i > 0)
            putchar(' ');
        print_id_hex(commit->parents[i], abbrev);
    }
}

/* %al/%cl: the email's local part, before the first '@'. An email with no
   '@' at all (malformed, should not occur in practice) prints whole. */
static void print_email_local(const char *email)
{
    const char *at = strchr(email, '@');

    if (at == NULL)
        fputs(email, stdout);
    else
        printf("%.*s", (int)(at - email), email);
}

typedef int (*date_fmt_fn)(long long, const char *, char *, size_t);

/* Shared by all ten of the non-%at date placeholders -- SG_DATE_NORMAL_MAX
   (64) is the largest of the four render buffers this project defines, so
   reusing it for all of them is always big enough. On a formatting failure
   this prints nothing, the same empty-string fallback the legacy/builtin
   date fields already use. */
static void print_date_field(long long t, const char *tz, date_fmt_fn fn)
{
    char buf[SG_DATE_NORMAL_MAX];

    if (fn(t, tz, buf, sizeof buf) != 0)
        buf[0] = '\0';
    fputs(buf, stdout);
}

/* Phase 64: the same job as print_date_field, but for one of the FOUR
   reach points --date=<name> affects (CLAUDE.md's --date= entry section
   2): %ad/%cd here, Date:/AuthorDate:/CommitDate:/the legacy Date: line
   and reference's own date field elsewhere in this file, and the
   annotated-tag header's own separate call site in cmd_show.c. A NULL
   date_mode reproduces the pre-Phase-64 fallback byte for byte (calling
   fallback_fn directly); every OTHER date placeholder (%aD/%ai/%aI/%as/
   %at and the committer mirrors) is a fixed format and must keep calling
   print_date_field above, unaffected by --date=. */
static void print_configured_date_field(long long t, const char *tz,
                                        const sg_date_mode *date_mode,
                                        date_fmt_fn fallback_fn)
{
    if (date_mode != NULL) {
        /* mode->kind may be SG_DATE_FORMAT, whose output length is
           unbounded user input -- must go through the alloc'ing renderer,
           not a fixed SG_DATE_MODE_MAX stack buffer (see that constant's
           header comment in date.h for the truncation bug this avoids). */
        char *buf = NULL;

        if (sg_date_format_mode_alloc(date_mode, t, tz, &buf) == 0) {
            fputs(buf, stdout);
            free(buf);
        }
        return;
    }
    {
        char buf[SG_DATE_NORMAL_MAX];

        if (fallback_fn(t, tz, buf, sizeof buf) != 0)
            buf[0] = '\0';
        fputs(buf, stdout);
    }
}

/* Same fallback/override rule as print_configured_date_field, but handing
   the caller a MALLOC'd string (the caller must free() it) instead of
   writing to stdout directly -- used by the medium/fuller/legacy/reference
   call sites in sg_commit_out_entry, which need the
   formatted string before they can decide layout (padding, labels) around
   it. On a rendering failure the string is empty (never NULL, matching the
   old fixed-buffer callers' fallback), but a genuine OOM can still return
   NULL -- callers must check, same as any other malloc-returning call in
   this codebase. */
static char *format_configured_date_field_alloc(long long t, const char *tz,
                                                 const sg_date_mode *date_mode,
                                                 date_fmt_fn fallback_fn)
{
    if (date_mode != NULL) {
        /* See print_configured_date_field above -- SG_DATE_FORMAT's output
           is unbounded, so this must go through the alloc'ing renderer. */
        char *buf = NULL;

        if (sg_date_format_mode_alloc(date_mode, t, tz, &buf) == 0)
            return buf;
        return strdup("");
    }
    {
        char buf[SG_DATE_NORMAL_MAX];

        if (fallback_fn(t, tz, buf, sizeof buf) != 0)
            buf[0] = '\0';
        return strdup(buf);
    }
}

/* A line (given as [p, end), never including its own '\n') is BLANK if it
   is empty or consists entirely of spaces/tabs. This single test is shared
   by fold_subject, first_paragraph_span, and print_message -- all three
   need to answer "is this line blank" and all three were independently
   re-deriving it before this was factored out. */
static int line_is_blank(const char *p, const char *end)
{
    for (; p < end; p++) {
        if (*p != ' ' && *p != '\t')
            return 0;
    }
    return 1;
}

/* git's "subject" extraction -- shared by %s, the oneline/short/reference
   builtins, and legacy --oneline (five sites, one function; measured
   against real git 2.55.0, section: docs/DESIGN.md's Phase 60b entry has
   the full table). NOT the same rule as %f/%b -- see sanitize_subject's
   and print_body's own comments for why those two stay separate.

   Algorithm: skip leading BLANK lines (a line is blank if it is empty or
   consists entirely of spaces/tabs), then join every following line up to
   (not including) the next blank line with a single space -- each line's
   own TRAILING whitespace is stripped before joining, but a continuation
   line's LEADING whitespace survives untouched. A message with no blank
   line anywhere folds its ENTIRE remaining content into one line (measured:
   "l1\nl2\nl3\n" with no blank line gives "l1 l2 l3", and %b for that
   same message is empty -- there is no body left once every line joined
   the subject).

   Writes into `out` (caller-owned, must be at least strlen(msg) bytes --
   each line's own newline is replaced by exactly one joining space and
   trailing-whitespace stripping only shrinks the result, so the output can
   never be longer than the input) and returns the written length, NOT
   NUL-terminated on its own, same convention as sanitize_subject. msg ==
   NULL, or a message consisting only of blank lines, writes nothing and
   returns 0. */
static size_t fold_subject(const char *msg, char *out)
{
    const char *p;
    size_t oi = 0;
    int have_line = 0;

    if (msg == NULL)
        return 0;
    p = msg;

    /* Skip leading blank lines. */
    for (;;) {
        const char *eol = strchr(p, '\n');
        const char *line_end = eol != NULL ? eol : p + strlen(p);

        if (!line_is_blank(p, line_end))
            break;
        if (eol == NULL)
            return 0; /* the whole message is blank lines */
        p = eol + 1;
    }

    /* Collect lines until the next blank line (or the end of the string). */
    for (;;) {
        const char *eol = strchr(p, '\n');
        const char *line_end = eol != NULL ? eol : p + strlen(p);
        const char *trim_end = line_end;

        if (line_is_blank(p, line_end))
            break;

        while (trim_end > p && (trim_end[-1] == ' ' || trim_end[-1] == '\t'))
            trim_end--;

        if (have_line)
            out[oi++] = ' ';
        memcpy(out + oi, p, (size_t)(trim_end - p));
        oi += (size_t)(trim_end - p);
        have_line = 1;

        if (eol == NULL)
            break;
        p = eol + 1;
    }

    return oi;
}

/* Mallocs a buffer sized to fit, folds msg into it via fold_subject, and
   returns the buffer (caller-owned, free() when done) with *out_len set to
   the written length. Returns NULL (and *out_len = 0) for a NULL/empty msg
   or on allocation failure -- callers treat NULL the same as "0-length". */
static char *fold_subject_alloc(const char *msg, size_t *out_len)
{
    char *buf;

    *out_len = 0;
    if (msg == NULL || *msg == '\0')
        return NULL;
    buf = malloc(strlen(msg));
    if (buf == NULL)
        return NULL;
    *out_len = fold_subject(msg, buf);
    return buf;
}

/* %s -- the folded subject, written with no trailing newline. */
static void print_folded_subject(const char *msg)
{
    size_t len;
    char *buf = fold_subject_alloc(msg, &len);

    if (buf != NULL) {
        fwrite(buf, 1, len, stdout);
        free(buf);
    }
}

/* %f -- the message's first line, sanitized the way git's own
   filename-safe subject sanitizer does (measured against real git 2.55.0
   directly, section 5.2 of the Phase 60 spec has the ten pinned rows): a
   byte is TITLE (alnum, '.', or '_' -- '@' is deliberately NOT title,
   measured, despite an older git comment saying otherwise) and copied
   as-is, with a run of consecutive '.' bytes collapsed to a single '.'; a
   run of non-title bytes (including each byte of a multi-byte UTF-8
   character, since none of them test alnum) collapses to a single '-',
   emitted lazily right before the next title byte -- a run at the very end
   of the string, with no following title byte, therefore emits nothing at
   all. Finally, leading '-' bytes are stripped (but NOT a leading '.' --
   measured: ".leading" keeps its dot, "-leading" loses its dash), and
   trailing '-'/'.' bytes are both stripped. Writes into `out`
   (caller-owned, must be at least `len` bytes -- the output can never be
   longer than the input) and returns the written length. */
static size_t sanitize_subject(const char *subject, size_t len, char *out)
{
    size_t oi = 0;
    size_t i;
    int pending_dash = 0;

    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)subject[i];
        int is_title = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '.' || c == '_';

        if (is_title) {
            if (pending_dash) {
                out[oi++] = '-';
                pending_dash = 0;
            }
            out[oi++] = (char)c;
            if (c == '.') {
                while (i + 1 < len && subject[i + 1] == '.')
                    i++;
            }
        } else {
            pending_dash = 1;
        }
    }

    {
        size_t start = 0;

        while (start < oi && out[start] == '-')
            start++;
        if (start > 0) {
            memmove(out, out + start, oi - start);
            oi -= start;
        }
    }
    while (oi > 0 && (out[oi - 1] == '-' || out[oi - 1] == '.'))
        oi--;

    return oi;
}

static void print_sanitized_subject(const char *msg)
{
    const char *nl;
    size_t len;
    char *buf;

    if (msg == NULL || *msg == '\0')
        return;
    /* %f skips leading blank lines exactly as %s does -- measured against
       real git on a message starting with a blank line, where %f is
       `subject-here` and not empty.  What %f does NOT share with %s is the
       FOLDING: it takes only the first physical line of the paragraph
       (measured: `first line   \n  continued` gives `first-line`), which is
       why this walks past blank lines by hand instead of calling
       fold_subject. */
    for (;;) {
        const char *eol = strchr(msg, '\n');
        const char *stop = eol == NULL ? msg + strlen(msg) : eol;

        if (!line_is_blank(msg, stop))
            break;
        if (eol == NULL)
            return;
        msg = eol + 1;
    }
    nl = strchr(msg, '\n');
    len = nl == NULL ? strlen(msg) : (size_t)(nl - msg);
    /* len == 0 is unreachable here (not a guard against it): the
       leading-blank-skip loop above only ever stops at a line that failed
       line_is_blank, i.e. one containing at least one non-whitespace byte,
       so [msg, nl) always has at least 1 byte. malloc(0) is therefore
       never requested; no `if (len == 0) return;` needed. */
    buf = malloc(len);
    if (buf == NULL)
        return;
    fwrite(buf, 1, sanitize_subject(msg, len, buf), stdout);
    free(buf);
}

/* %b -- everything after the first blank line, with any further blank
   lines immediately following it also skipped (measured: "subject\n\n\n
   body\n" prints body with no leading blank line at all, not one). A
   message with no blank line anywhere prints nothing. */
static void print_body(const char *msg)
{
    const char *p;

    if (msg == NULL)
        return;
    p = msg;

    /* Find the first BLANK line, using line_is_blank -- the same test
       fold_subject/first_paragraph_span/print_message all use (a line
       consisting only of spaces/tabs counts as blank, not just a
       literally empty one). This function used to search for a literal
       "\n\n" instead, which is a NARROWER test than every sibling
       function in this file settled on -- measured, found during review:
       a separator line containing a single space or tab (not literally
       empty) still counts as the blank line boundary in real git, and the
       old literal-"\n\n" search missed it entirely. */
    for (;;) {
        const char *eol = strchr(p, '\n');
        const char *line_end = eol != NULL ? eol : p + strlen(p);

        if (line_is_blank(p, line_end))
            break;
        if (eol == NULL)
            return; /* no blank line anywhere -- no body */
        p = eol + 1;
    }
    if (strchr(p, '\n') == NULL)
        return; /* the blank line found above is the message's last line */
    p = strchr(p, '\n') + 1;

    /* Skip any further immediately-following blank lines (measured:
       "subject\n\n\nbody\n" prints body with no leading blank line at
       all, not one). */
    for (;;) {
        const char *eol = strchr(p, '\n');
        const char *line_end = eol != NULL ? eol : p + strlen(p);

        if (!line_is_blank(p, line_end))
            break;
        if (eol == NULL)
            return;
        p = eol + 1;
    }

    fputs(p, stdout);
}

/* Expands a FORMAT/TFORMAT user_format's placeholders against one commit.
   Every `%`-sequence has already been validated by
   sg_pretty_validate_format (cmd_log.c's/cmd_show.c's resolve_pretty_arg,
   called once per invocation before any commit is ever rendered), so
   decode_placeholder cannot fail here in practice -- the fallback branch
   below (print the sequence literally) exists only so this function stays
   safe if that precondition is ever violated, it is not a second policy. */
static void expand_user_format(const char *fmt, const char *hex, const sg_commit *commit,
                               const sg_date_mode *date_mode)
{
    const char *p = fmt;

    while (*p != '\0') {
        sg_ph_token tok;
        size_t blen;

        if (*p != '%') {
            putchar(*p);
            p++;
            continue;
        }
        if (decode_placeholder(p, &tok, &blen) != 0) {
            fwrite(p, 1, blen, stdout);
            p += blen;
            continue;
        }
        switch (tok.kind) {
        case PH_HASH_FULL: fputs(hex, stdout); break;
        case PH_HASH_ABBR: printf("%.*s", SG_COMMIT_OUT_ABBREV, hex); break;
        case PH_TREE_FULL: print_id_hex(commit->tree, 0); break;
        case PH_TREE_ABBR: print_id_hex(commit->tree, 1); break;
        case PH_PARENTS_FULL: print_parents(commit, 0); break;
        case PH_PARENTS_ABBR: print_parents(commit, 1); break;
        case PH_A_NAME: fputs(commit->author_name, stdout); break;
        case PH_A_EMAIL: fputs(commit->author_email, stdout); break;
        case PH_A_LOCAL: print_email_local(commit->author_email); break;
        case PH_A_DATE: print_configured_date_field(commit->author_time, commit->author_tz, date_mode, sg_date_format_normal); break;
        case PH_A_DATE_RFC2822: print_date_field(commit->author_time, commit->author_tz, sg_date_format_rfc2822); break;
        case PH_A_DATE_UNIX: printf("%lld", commit->author_time); break;
        case PH_A_DATE_ISO: print_date_field(commit->author_time, commit->author_tz, sg_date_format_iso); break;
        case PH_A_DATE_ISO_STRICT: print_date_field(commit->author_time, commit->author_tz, sg_date_format_iso_strict); break;
        case PH_A_DATE_SHORT: print_date_field(commit->author_time, commit->author_tz, sg_date_format_short); break;
        case PH_C_NAME: fputs(commit->committer_name, stdout); break;
        case PH_C_EMAIL: fputs(commit->committer_email, stdout); break;
        case PH_C_LOCAL: print_email_local(commit->committer_email); break;
        case PH_C_DATE: print_configured_date_field(commit->committer_time, commit->committer_tz, date_mode, sg_date_format_normal); break;
        case PH_C_DATE_RFC2822: print_date_field(commit->committer_time, commit->committer_tz, sg_date_format_rfc2822); break;
        case PH_C_DATE_UNIX: printf("%lld", commit->committer_time); break;
        case PH_C_DATE_ISO: print_date_field(commit->committer_time, commit->committer_tz, sg_date_format_iso); break;
        case PH_C_DATE_ISO_STRICT: print_date_field(commit->committer_time, commit->committer_tz, sg_date_format_iso_strict); break;
        case PH_C_DATE_SHORT: print_date_field(commit->committer_time, commit->committer_tz, sg_date_format_short); break;
        case PH_SUBJECT: print_folded_subject(commit->message); break;
        case PH_SANITIZED_SUBJECT: print_sanitized_subject(commit->message); break;
        case PH_BODY: print_body(commit->message); break;
        case PH_RAW_BODY: if (commit->message != NULL) fputs(commit->message, stdout); break;
        case PH_NEWLINE: putchar('\n'); break;
        case PH_PERCENT: putchar('%'); break;
        case PH_HEX_BYTE: putchar((int)tok.hexval); break;
        default: break;
        }
        p += tok.consumed;
    }
}

/* git EXPANDS TABS in the message body for medium/full/fuller (measured in
   Phase 60 -- CLAUDE.md's earlier "only medium" note undersold this: `git
   log --pretty=full`/`fuller` on a message containing a tab also expand it,
   `--oneline`/`short`/`raw`/`%s` all leave the tab alone). `--expand-tabs=8`
   is the default for that bucket. The column is counted from the start of
   the MESSAGE line, NOT from the indented output column: a line of two tabs
   comes out at column 20, which is 16 expanded columns plus the four-space
   indent, and a line of "1234567\tx" comes out with a single space. Getting
   this from the output column instead would put the second one at a
   different stop.

   Found by `sg show` on a merge whose auto-generated message contains
   "#\tboth.txt"; it was wrong for `sg log` too, and had gone unnoticed
   because no fixture had ever put a tab in a commit message. */
static void print_message_line(const char *line, size_t len, int expand_tabs)
{
    size_t i;
    size_t col = 0;

    printf("    ");
    for (i = 0; i < len; i++) {
        if (expand_tabs && line[i] == '\t') {
            size_t stop = (col / 8 + 1) * 8;

            while (col < stop) {
                putchar(' ');
                col++;
            }
        } else {
            putchar(line[i]);
            col++;
        }
    }
    putchar('\n');
}

/* Prints one line's content with its own TRAILING whitespace stripped
   first (spaces and tabs), then handed to print_message_line for
   tab-expansion/indent -- see print_message's own comment for the measured
   evidence. Tab-expansion and trailing-whitespace-stripping are measured to
   commute (the stripped suffix is always pure whitespace regardless of
   whether it is stripped before or after any earlier tab in the same line
   is expanded to columns, since expansion of one character never touches
   another character's identity), and BOTH happen unconditionally --
   trailing-whitespace stripping does NOT depend on expand_tabs (measured on
   `raw`/`short`, neither of which expands tabs, both still strip trailing
   whitespace). */
static void print_message_line_stripped(const char *p, const char *line_end, int expand_tabs)
{
    const char *trim_end = line_end;

    while (trim_end > p && (trim_end[-1] == ' ' || trim_end[-1] == '\t'))
        trim_end--;
    print_message_line(p, (size_t)(trim_end - p), expand_tabs);
}

/* git indents EVERY message line by four spaces, prints the block only
   when the message has any non-blank content at all (a commit whose
   message is empty, or consists ENTIRELY of blank lines, renders as its
   header lines and nothing else), and applies THREE further rules this
   file used to get wrong for anything but the simplest fixtures (found
   during Phase 60c's review, all measured against real git 2.55.0 via
   `git hash-object -t commit -w --stdin` -- `git commit`'s own message
   cleanup would silently erase every one of these shapes before they ever
   reached the object store, which is exactly why no earlier fixture built
   through porcelain ever exercised them):

   1. Leading blank lines (empty OR all-whitespace, line_is_blank's test)
      are skipped ENTIRELY -- not rendered even as "    \n".
   2. Trailing blank lines are likewise skipped entirely (measured:
      "subj\n\nbody\n\n\n" renders identically to "subj\n\nbody\n").
   3. A BLANK line in the MIDDLE is preserved, and every one of them,
      individually -- measured: two consecutive middle blank lines render
      as TWO separate "    \n" lines, not squeezed into one. This falls out
      naturally below: a middle blank line's content, after being run
      through the SAME per-line trailing-whitespace-strip every other line
      gets, is simply empty, so it prints as "    " + nothing + "\n" without
      needing a special case -- the only lines that need SKIPPING (not
      printing) are the leading and trailing RUNS.

   %B is completely unaffected by any of this -- it is the raw message,
   unconditionally verbatim; this is a RENDERING rule, not a message-content
   rule, so it lives here and nowhere near sg_commit_parse or the %B case of
   expand_user_format. */
static void print_message(const char *msg, int expand_tabs)
{
    const char *p;
    size_t pending_blanks;

    if (msg == NULL || *msg == '\0')
        return;
    p = msg;

    /* Skip leading blank lines entirely. */
    for (;;) {
        const char *eol = strchr(p, '\n');
        const char *line_end = eol != NULL ? eol : p + strlen(p);

        if (!line_is_blank(p, line_end))
            break;
        if (eol == NULL)
            return; /* the whole message is blank lines -- print nothing */
        p = eol + 1;
    }

    /* From here on there is guaranteed to be at least one non-blank line
       ahead, so the message's own leading blank-line separator is always
       printed exactly once. */
    printf("\n");

    /* Blank lines are buffered (never printed immediately) and flushed
       lazily right before the next NON-blank line -- a run that reaches
       the end of the string without ever being followed by a non-blank
       line is a trailing run and is discarded unprinted. */
    pending_blanks = 0;
    for (;;) {
        const char *eol = strchr(p, '\n');
        const char *line_end = eol != NULL ? eol : p + strlen(p);

        if (line_is_blank(p, line_end)) {
            pending_blanks++;
        } else {
            while (pending_blanks > 0) {
                printf("    \n");
                pending_blanks--;
            }
            print_message_line_stripped(p, line_end, expand_tabs);
        }
        if (eol == NULL)
            break;
        p = eol + 1;
    }
}

/* --oneline's second field is the message's FOLDED subject (fold_subject
   above), and the separating space is printed even when there is no
   subject at all: measured, an empty message renders as "<abbrev> " with a
   trailing space. */
static void print_subject(const char *msg)
{
    print_folded_subject(msg);
    putchar('\n');
}

/* Locates the message's "first paragraph", VERBATIM (no folding) -- skip
   leading blank lines (same blank-line test as fold_subject: empty or all
   spaces/tabs), then the paragraph is everything up to (excluding) the
   next blank line or the end of the string. `short` uses this, NOT
   fold_subject: measured against real git 2.55.0, `short` prints SUBJECT
   ONLY (no body -- Phase 60a's own finding), but unlike %s/oneline/
   reference/legacy --oneline (which all fold multi-line subjects into one
   space-joined line, see fold_subject's own comment) it prints each
   physical line of the first paragraph on its OWN line, unchanged --
   confirmed with `git log --pretty=short` on a message whose first
   paragraph spans 3 lines: git prints 3 separate indented lines, not one
   folded line. `short` is therefore the one site of this family that does
   NOT go through fold_subject.
   Sets *out_start (borrowed, into msg) and *out_len to the paragraph's
   span; both are NULL/0 for a NULL msg or a message that is entirely
   blank lines. */
static void first_paragraph_span(const char *msg, const char **out_start, size_t *out_len)
{
    const char *p;
    const char *para_start;
    const char *para_end;

    *out_start = NULL;
    *out_len = 0;
    if (msg == NULL)
        return;
    p = msg;

    /* Skip leading blank lines. */
    for (;;) {
        const char *eol = strchr(p, '\n');
        const char *line_end = eol != NULL ? eol : p + strlen(p);

        if (!line_is_blank(p, line_end))
            break;
        if (eol == NULL)
            return; /* the whole message is blank lines */
        p = eol + 1;
    }
    para_start = p;
    para_end = para_start;

    /* Walk lines until the next blank line (or the end of the string),
       tracking only each confirmed non-blank line's OWN content end --
       never speculatively advancing past a trailing '\n' before knowing
       whether a further line exists, which is what would otherwise fold a
       message with no blank line at all into including a phantom trailing
       empty line. */
    for (;;) {
        const char *eol = strchr(p, '\n');
        const char *line_end = eol != NULL ? eol : p + strlen(p);

        if (line_is_blank(p, line_end))
            break;
        para_end = line_end;
        if (eol == NULL)
            break;
        p = eol + 1;
    }

    *out_start = para_start;
    *out_len = (size_t)(para_end - para_start);
}

/* short prints the SUBJECT ONLY, never the body -- measured (Phase 60a's
   oracle matrix): `short` looked like it was byte-exact when tested only
   against body-less fixtures, but a real body ("body first") showed up
   verbatim underneath it, which git never does. Reuses print_message's own
   blank-line + four-space-indent-per-line shape (so a `raw`- or
   `medium`-shaped consumer sees the same left margin) by handing it a
   NUL-terminated copy of just the first-paragraph span -- print_message
   itself needs no changes, it already prints each line of whatever it is
   given, and first_paragraph_span is what bounds "whatever it is given" to
   the first paragraph. */
static void print_message_subject_only(const char *msg, int expand_tabs)
{
    const char *start;
    size_t len;
    char *buf;

    first_paragraph_span(msg, &start, &len);
    if (start == NULL)
        return;
    buf = malloc(len + 1);
    if (buf == NULL)
        return;
    memcpy(buf, start, len);
    buf[len] = '\0';
    print_message(buf, expand_tabs);
    free(buf);
}

/* "Merge: <7hex> <7hex> ..." -- shared by short/medium/full/fuller (all
   measured to print it, Phase 60); `oneline`/`raw`/`reference` do not:
   `raw` prints a full "parent <40hex>" line per parent instead, and
   `oneline`/`reference` carry no parent info at all. */
static void print_merge_line(const sg_commit *commit)
{
    size_t pi;

    if (commit->parent_count <= 1)
        return;
    printf("Merge:");
    for (pi = 0; pi < commit->parent_count; pi++) {
        char phex[SG_SHA1_HEX_LEN + 1];

        sg_sha1_to_hex(commit->parents[pi], phex);
        printf(" %.*s", SG_COMMIT_OUT_ABBREV, phex);
    }
    printf("\n");
}

/* The seven builtins, section 3 of the Phase 60 spec -- byte-exact against
   real git 2.55.0, measured with commit-tree-built fixtures (including a
   2-parent merge and a root commit). `hex` is the already-computed 40-hex
   id, `author_date`/`committer_date` the already-rendered DATE_NORMAL
   strings (empty string on a formatting failure, same fallback
   sg_commit_out_entry's pre-Phase-60 code used). */
static void print_pretty_oneline(const char *hex, const sg_commit *commit)
{
    printf("%s ", hex);
    print_subject(commit->message);
}

static void print_pretty_short(const char *hex, const sg_commit *commit)
{
    printf("commit %s\n", hex);
    print_merge_line(commit);
    printf("Author: %s <%s>\n", commit->author_name, commit->author_email);
    /* short does NOT expand tabs (measured) -- only medium/full/fuller do.
       short is also SUBJECT-ONLY (measured, Phase 60a oracle round 2): the
       body, if any, is never printed. */
    print_message_subject_only(commit->message, 0);
}

static void print_pretty_medium(const char *hex, const sg_commit *commit,
                                const char *author_date)
{
    printf("commit %s\n", hex);
    print_merge_line(commit);
    printf("Author: %s <%s>\n", commit->author_name, commit->author_email);
    printf("Date:   %s\n", author_date);
    print_message(commit->message, 1);
}

static void print_pretty_full(const char *hex, const sg_commit *commit)
{
    printf("commit %s\n", hex);
    print_merge_line(commit);
    printf("Author: %s <%s>\n", commit->author_name, commit->author_email);
    printf("Commit: %s <%s>\n", commit->committer_name, commit->committer_email);
    print_message(commit->message, 1);
}

static void print_pretty_fuller(const char *hex, const sg_commit *commit,
                                const char *author_date, const char *committer_date)
{
    printf("commit %s\n", hex);
    print_merge_line(commit);
    /* Each of the four labels is padded to a 12-column field, "%-12s" does
       exactly that: "Author:" (7 chars) + 5 spaces, "AuthorDate:" (11
       chars) + 1 space, measured. */
    printf("%-12s%s <%s>\n", "Author:", commit->author_name, commit->author_email);
    printf("%-12s%s\n", "AuthorDate:", author_date);
    printf("%-12s%s <%s>\n", "Commit:", commit->committer_name, commit->committer_email);
    printf("%-12s%s\n", "CommitDate:", committer_date);
    print_message(commit->message, 1);
}

static void print_pretty_raw(const char *hex, const sg_commit *commit)
{
    char tree_hex[SG_SHA1_HEX_LEN + 1];
    size_t pi;

    sg_sha1_to_hex(commit->tree, tree_hex);
    printf("commit %s\n", hex);
    printf("tree %s\n", tree_hex);
    for (pi = 0; pi < commit->parent_count; pi++) {
        char phex[SG_SHA1_HEX_LEN + 1];

        sg_sha1_to_hex(commit->parents[pi], phex);
        printf("parent %s\n", phex);
    }
    printf("author %s <%s> %lld %s\n", commit->author_name, commit->author_email,
          commit->author_time, commit->author_tz);
    printf("committer %s <%s> %lld %s\n", commit->committer_name, commit->committer_email,
          commit->committer_time, commit->committer_tz);
    /* Phase 61: unknown trailing headers (e.g. "gpgsig") are reprinted here,
       byte for byte, exactly where they sat in the original object --
       measured against real git 2.55.0. extra_headers is "" (never NULL)
       when there were none, so this is a no-op printf in the common case. */
    printf("%s", commit->extra_headers);
    /* raw does NOT expand tabs (measured), unlike medium/full/fuller --
       print_message's own blank-line + four-space-indent shape still
       applies, that part is common to every builtin that shows a message. */
    print_message(commit->message, 0);
}

static void print_pretty_reference(const unsigned char id[SG_SHA1_RAW_LEN],
                                   const sg_commit *commit, const sg_date_mode *date_mode)
{
    char hex7[SG_SHA1_HEX_LEN + 1];
    char *short_date;
    size_t subject_len;
    char *subject_buf;

    sg_sha1_to_hex(id, hex7);
    /* As of Phase 60b, the FOLDED subject (fold_subject above), not just
       the first physical line. */
    subject_buf = fold_subject_alloc(commit->message, &subject_len);
    /* `reference` uses the AUTHOR date (short form, "YYYY-MM-DD" by
       default), NOT the committer's -- measured with a fixture whose two
       dates fall on different days. Phase 64: --date=<name> reaches this
       field too (its own default remains SHORT, distinct from every other
       reach point's DEFAULT) -- measured with a control that does NOT
       coincide with short's own rendering (a fixture using --date=short
       against this field proves nothing, it is what the field already
       renders). */
    short_date = format_configured_date_field_alloc(commit->author_time, commit->author_tz,
                                                     date_mode, sg_date_format_short);
    printf("%.*s (%.*s, %s)\n", SG_COMMIT_OUT_ABBREV, hex7, (int)subject_len,
          subject_buf != NULL ? subject_buf : "", short_date != NULL ? short_date : "");
    free(subject_buf);
    free(short_date);
}

/* Phase 62: see the header comment. Deliberately mirrors print_commit_diff's
   own "first parent's tree, empty tree for a root commit" construction so
   the two can never answer a different question about the same commit. */
int sg_commit_out_touches_pathspec(const char *git_dir, const sg_commit *commit,
                                   const sg_pathspec *ps, int *out_touches,
                                   char *bad_path)
{
    sg_diff_list list;
    unsigned char parent_tree[SG_SHA1_RAW_LEN];
    const unsigned char *old_tree = NULL;
    int rc;

    if (ps == NULL || ps->count == 0) {
        *out_touches = 1;
        return 0;
    }

    if (commit->parent_count > 0) {
        if (sg_commit_tree_of(git_dir, commit->parents[0], parent_tree) != 0)
            return -1;
        old_tree = parent_tree;
    }

    rc = sg_diff_trees(git_dir, old_tree, commit->tree, &list, bad_path, 0);
    if (rc != 0)
        return rc == -2 ? -2 : -1;

    /* Run before rename detection on purpose -- see the header comment. */
    sg_diff_list_filter(&list, ps);
    *out_touches = list.count > 0;
    sg_diff_list_free(&list);
    return 0;
}

/* Phase 63 review round (second pass): whether the entry's HEADER is
   DEFINED to render as exactly zero bytes, by the FORMAT/TFORMAT grammar
   itself -- not "did this particular commit's fields happen to expand to
   nothing". The predicate is "is the FORMAT STRING itself empty", the
   SAME one the SG_PRETTY_TFORMAT terminator-suppression case below uses
   (this function exists specifically so the two never drift apart again;
   see its own call sites' comments for what broke the first time this
   rule was written down twice instead of once): `--pretty=format:%b` on a
   body-less commit expands to zero bytes too, but its format STRING is
   "%b", not "", so this returns 0 for it and both the header and this
   function's callers keep behaving as if something had been printed --
   correctly, since `format:%b` non-empty commits DO print something and
   the separator rule cannot special-case per-commit content without
   seeing every commit's rendering first. */
static int pretty_header_is_empty(const sg_commit_out_opts *o)
{
    return o->pretty != NULL &&
           (o->pretty->kind == SG_PRETTY_FORMAT || o->pretty->kind == SG_PRETTY_TFORMAT) &&
           o->pretty->user_format[0] == '\0';
}

/* The commit's own diff: first parent's tree against its own, or the empty
   tree for a root commit. That this renders a diff for a MERGE at all is a
   consequence of sg's first-parent-only walk, and it agrees with git under
   the same restriction: plain `git log -p` prints nothing for a merge, while
   `git log --first-parent -p` -- the oracle this command is compared against
   -- prints the diff against parent 1 (measured, both directions).
   Returns 0, or -1 having printed nothing. */
static int print_commit_diff(const char *git_dir, const sg_commit *commit,
                             const sg_commit_out_opts *o)
{
    sg_diff_list list;
    unsigned char parent_tree[SG_SHA1_RAW_LEN];
    const unsigned char *old_tree = NULL;
    char bad_path[SG_PATH_MAX];
    sg_diff_out_opts opts;
    int rc = 0;

    if (commit->parent_count > 0) {
        if (sg_commit_tree_of(git_dir, commit->parents[0], parent_tree) != 0)
            return -1;
        old_tree = parent_tree;
    }

    bad_path[0] = '\0';
    if (sg_diff_trees(git_dir, old_tree, commit->tree, &list, bad_path, 0) != 0)
        return -1;

    /* Phase 62: restrict the diff to o->pathspec's paths, same as
       sg_commit_out_touches_pathspec restricts which commits are shown --
       must run BEFORE rename detection (CLAUDE.md's Phase 29 ordering
       rule: filtering after detection can turn a real rename into a plain
       delete because only half the pair survives). NULL/empty is a no-op
       (sg_diff_list_filter's own contract), so an unfiltered `sg log -p`
       pays nothing extra here. */
    sg_diff_list_filter(&list, o->pathspec);

    /* git's own default since 2.9: diff.renames is on, so `git log -p` shows
       `rename from`/`rename to` rather than a delete plus an add. */
    if (sg_diff_detect_renames(git_dir, NULL, &list, SG_SIMILARITY_DEFAULT, 0) != 0) {
        sg_diff_list_free(&list);
        return -1;
    }

    /* An EMPTY diff prints nothing at all -- no blank line, no `---`
       separator (measured on an empty commit). The separators below are part
       of the diff, not of the entry. */
    if (list.count == 0) {
        sg_diff_list_free(&list);
        return 0;
    }

    /* Measured, all four combinations: in the default format the diff is
       introduced by a blank line, except when --stat and -p are BOTH on, in
       which case git prints a literal `---` line instead. --oneline
       introduces neither, and never prints `---`.

       WARNING (found by a 159-probe oracle, Phase 59 round 2): the `---`
       rule is NOT just "stat && patch" -- o->stat/o->patch are no longer
       forced to 0 when a name format is active (see sg/commit_out.h's own
       WARNING on that point, and cmd_show.c's resolve_commit_out_opts,
       which stopped zeroing them once the redundant-guard mutation showed
       the renderer was the real enforcement point). So a caller reaching
       here with --name-only/--name-status can still have o->stat and
       o->patch both nonzero underneath (e.g. `-p --name-only --stat`), and
       NAME must win the separator decision the same way it already wins
       which sg_diff_print format gets called below -- an ordinary blank
       line, never `---`.

       Phase 60: builtin `oneline` (opts->pretty->kind ==
       SG_PRETTY_ONELINE) joins --oneline in this exemption -- measured, no
       separator at all, same as legacy --oneline. Every other kind
       (including FORMAT/TFORMAT and every other builtin) falls through to
       the SAME blank-line/`---` print as before: FORMAT's own entry text
       has no trailing newline of its own, so the identical bytes read as
       "no blank line" there (measured -- see CLAUDE.md's `sg log` Phase 60
       entry for the byte-level derivation), while every terminator-bearing
       kind (TFORMAT, and the six other builtins) reads it as an actual
       blank line, exactly as it always has.

       Phase 63 review round (second pass): a FORMAT/TFORMAT entry whose
       format STRING is itself empty prints NEITHER the blank line NOR
       `---` -- measured against real git directly (`format:`/`tformat:`
       with nothing after the colon, `-p`/`--stat`/`-p --stat` all three):
       an empty header is not "a header that happens to render short", it
       is no header at all, and git treats it that way for the separator
       exactly as it does for TFORMAT's own terminator (see
       pretty_header_is_empty's own comment -- this is the SAME predicate,
       shared with SG_PRETTY_TFORMAT's terminator suppression below, on
       purpose: the two used to be written as two separate checks and one
       of them was missing this rule entirely). `format:%b` on a body-less
       commit is the control that proves the predicate is about the
       STRING, not the expansion: its format string ("%b") is non-empty,
       so it still gets its separator/`---` even though that particular
       commit's own header text happens to be empty. --name-only/
       --name-status are unaffected: `format:`/`tformat:` with
       --name-only already has no separator logic to suppress (measured:
       `--pretty=format: --name-only` prints the bare filename with no
       separator either way), so pretty_header_is_empty is checked
       alongside the existing name-format exemption, not instead of it. */
    if (!o->oneline && !(o->pretty != NULL && o->pretty->kind == SG_PRETTY_ONELINE) &&
        !pretty_header_is_empty(o))
        printf((o->stat && o->patch && !o->name_only && !o->name_status) ? "---\n" : "\n");

    memset(&opts, 0, sizeof opts);
    opts.algorithm = SG_DIFF_ALGO_MYERS;
    if (o->name_only || o->name_status) {
        /* Section 3 of the Phase 59 spec: same blank-line rule as patch/
           stat above (already printed), same empty-diff-prints-nothing
           rule (already handled above), just a different sg_diff_print
           format -- reuses the same rename-detected `list`. */
        opts.format = o->name_status ? SG_DIFF_FORMAT_NAME_STATUS : SG_DIFF_FORMAT_NAME_ONLY;
        if (sg_diff_print(git_dir, NULL, &list, &opts) != 0)
            rc = -1;
    } else {
        if (o->stat) {
            opts.format = SG_DIFF_FORMAT_STAT;
            /* repo_root is NULL deliberately: a tree-vs-tree list is entirely
               SG_DIFF_SIDE_BLOB, and that branch of sg_diff_side_read never
               touches it (the same invariant Phase 49's merge rename detection
               relies on). */
            if (sg_diff_print(git_dir, NULL, &list, &opts) != 0)
                rc = -1;
        }
        if (rc == 0 && o->patch) {
            if (o->stat)
                printf("\n");
            opts.format = SG_DIFF_FORMAT_PATCH;
            if (sg_diff_print(git_dir, NULL, &list, &opts) != 0)
                rc = -1;
        }
    }

    sg_diff_list_free(&list);
    return rc;
}

int sg_commit_out_entry(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                        const sg_commit *commit, const sg_commit_out_opts *opts)
{
    char hex[SG_SHA1_HEX_LEN + 1];

    sg_sha1_to_hex(id, hex);

    if (opts->pretty != NULL) {
        switch (opts->pretty->kind) {
        case SG_PRETTY_ONELINE:
            print_pretty_oneline(hex, commit);
            break;
        case SG_PRETTY_SHORT:
            print_pretty_short(hex, commit);
            break;
        case SG_PRETTY_MEDIUM: {
            char *timebuf = format_configured_date_field_alloc(
                commit->author_time, commit->author_tz,
                opts->date_mode, sg_date_format_normal);

            print_pretty_medium(hex, commit, timebuf != NULL ? timebuf : "");
            free(timebuf);
            break;
        }
        case SG_PRETTY_FULL:
            print_pretty_full(hex, commit);
            break;
        case SG_PRETTY_FULLER: {
            char *timebuf = format_configured_date_field_alloc(
                commit->author_time, commit->author_tz,
                opts->date_mode, sg_date_format_normal);
            char *committer_timebuf = format_configured_date_field_alloc(
                commit->committer_time, commit->committer_tz,
                opts->date_mode, sg_date_format_normal);

            print_pretty_fuller(hex, commit, timebuf != NULL ? timebuf : "",
                                committer_timebuf != NULL ? committer_timebuf : "");
            free(timebuf);
            free(committer_timebuf);
            break;
        }
        case SG_PRETTY_RAW:
            print_pretty_raw(hex, commit);
            break;
        case SG_PRETTY_REFERENCE:
            print_pretty_reference(id, commit, opts->date_mode);
            break;
        case SG_PRETTY_FORMAT:
            /* No terminator -- the entry text is exactly opts->pretty->
               user_format with every placeholder expanded, byte for byte.
               The CLI layer has already validated every `%`-sequence in
               this string before this function is ever reached (see this
               function's own header-comment precondition). */
            expand_user_format(opts->pretty->user_format, hex, commit, opts->date_mode);
            break;
        case SG_PRETTY_TFORMAT:
            /* Always terminates with exactly one '\n' -- this is what makes
               tformat: differ byte-for-byte from format: on the exact same
               literal string (measured: "plain" -> "plain" vs "plain\n").

               EXCEPTION, pre-existing since Phase 60a and unrelated to
               --graph (found in Phase 63's review round, by a --graph
               fixture that happened to be the first one ever to combine
               tformat: with an EMPTY format string): when the user_format
               itself is the empty string (`--pretty=tformat:` with
               nothing after the colon), git prints ZERO bytes for the
               whole entry -- not even the terminator. The predicate is
               "is the format STRING empty", not "did expansion produce
               zero bytes": `tformat:%b` on a body-less commit expands to
               zero bytes too, but STILL gets its terminator (measured;
               already covered by existing fixtures, unaffected by this
               fix since user_format there is "%b", not ""). format:
               (non-t) needs no equivalent special case -- an empty
               format: entry is already correct, measured as a
               zero-length string sitting between whatever separators the
               caller prints (`git log -n 2 --pretty=format:` gives
               "\n\n" for two entries, matched before this fix).

               Phase 63 review round (second pass): this predicate is now
               shared with print_commit_diff's separator suppression via
               pretty_header_is_empty, rather than repeated here as a
               second, independent `user_format[0] == '\0'` check -- the
               first version of this fix wrote the same condition twice,
               and only one of the two copies got the rule; see that
               function's own comment. */
            if (pretty_header_is_empty(opts))
                break;
            expand_user_format(opts->pretty->user_format, hex, commit, opts->date_mode);
            putchar('\n');
            break;
        case SG_PRETTY_LEGACY:
        default:
            /* Unreachable: a non-NULL opts->pretty is always one of the
               above. Falls through to the legacy branch below only if this
               ever changes without updating this switch. */
            goto legacy;
        }
    } else {
legacy:
        if (opts->oneline) {
            /* No blank line between --oneline entries, with or without a
               diff attached (measured). */
            printf("%.*s ", SG_COMMIT_OUT_ABBREV, hex);
            print_subject(commit->message);
        } else {
            /* The stored offset is part of the answer, not decoration: the
               wall clock git shows is the timestamp SHIFTED INTO that
               offset. Reading it as UTC while still printing "+0800" beside
               it was wrong by the offset -- eight hours here -- and said so
               in its own output. */
            char *timebuf = format_configured_date_field_alloc(
                commit->author_time, commit->author_tz,
                opts->date_mode, sg_date_format_normal);

            printf("commit %s\n", hex);
            print_merge_line(commit);
            printf("Author: %s <%s>\n", commit->author_name, commit->author_email);
            printf("Date:   %s\n", timebuf != NULL ? timebuf : "");
            free(timebuf);
            print_message(commit->message, 1);
        }
    }

    if ((opts->patch || opts->stat || opts->name_only || opts->name_status) &&
       print_commit_diff(git_dir, commit, opts) != 0)
        return -1;

    return 0;
}
