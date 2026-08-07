#include "sg/ignore.h"

#include "sg/workdir.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* gitignore matching engine. Patterns arrive inside cloned repos and are
   attacker-controlled, so all wildcard matching below is iterative (the
   classic two-pointer backtracking algorithm, O(n*m) worst case) -- no
   recursion anywhere, no fixed-size buffers for pattern or path content. */

typedef struct {
    char *pattern; /* processed pattern text; escapes are kept for the matcher */
    size_t len;
    unsigned negated : 1;  /* leading '!' */
    unsigned dir_only : 1; /* trailing '/' */
    unsigned anchored : 1; /* contains '/': matched against the whole path */
} sg_ignore_rule;

/* One ignore source: the rules of a single file plus the repo-root-relative
   directory they are anchored to ("" for the root .gitignore and for
   info/exclude, which behaves as if it lived at the repo root). */
typedef struct {
    char *base;
    size_t base_len;
    sg_ignore_rule *rules;
    size_t count;
    size_t cap;
} sg_ignore_frame;

struct sg_ignore {
    char *worktree_root;
    sg_ignore_frame exclude; /* .git/info/exclude: lowest precedence */
    sg_ignore_frame *stack;  /* [0] = root .gitignore; deeper entries pushed */
    size_t depth;
    size_t stack_cap;
};

enum {
    SG_IGN_NO_MATCH = 0,
    SG_IGN_IGNORED,
    SG_IGN_KEPT, /* a negation matched: definitively not ignored */
};

/* ---- wildcard matching ------------------------------------------------- */

/* Matches c against the character class starting at pat[0] == '['. On a
   well-formed class sets *consumed to the bytes the class occupies (through
   the closing ']') and returns 1 (member) or 0 (not). Returns -1 for an
   unterminated class which -- exactly like git's wildmatch, verified against
   git 2.55 -- can never match anything at all. A ']' right after the '[' (or
   after the negation) is a literal member; '-' is a range only between two
   members; '\\' escapes the next character. */
static int class_match(const char *pat, size_t plen, char text_ch, size_t *consumed)
{
    size_t i = 1;
    int negated = 0;
    int matched = 0;
    int have_prev = 0;
    int first = 1;
    unsigned char prev = 0;
    unsigned char c = (unsigned char)text_ch;

    if (i < plen && (pat[i] == '!' || pat[i] == '^')) {
        negated = 1;
        i++;
    }
    while (i < plen) {
        unsigned char pc = (unsigned char)pat[i];

        if (pc == ']' && !first) {
            *consumed = i + 1;
            return matched != negated;
        }
        first = 0;
        if (pc == '-' && have_prev && i + 1 < plen && pat[i + 1] != ']') {
            unsigned char hi;

            i++;
            hi = (unsigned char)pat[i];
            if (hi == '\\') {
                i++;
                if (i >= plen)
                    return -1;
                hi = (unsigned char)pat[i];
            }
            if (c >= prev && c <= hi)
                matched = 1;
            prev = hi;
            i++;
            continue;
        }
        if (pc == '\\') {
            i++;
            if (i >= plen)
                return -1;
            pc = (unsigned char)pat[i];
        }
        if (c == pc)
            matched = 1;
        prev = pc;
        have_prev = 1;
        i++;
    }
    return -1;
}

/* Matches one path segment (never contains '/') against one pattern segment.
   Iterative two-pointer backtracking: on a mismatch after a '*', re-extend
   the most recent star by one character and retry -- O(plen*tlen) worst
   case, zero recursion, so a pattern of 10,000 '*'s cannot smash the stack.
   Consecutive stars (an embedded "**") collapse to plain '*' semantics. */
static int seg_match(const char *pat, size_t plen, const char *text, size_t tlen)
{
    size_t p = 0;
    size_t t = 0;
    size_t star_p = SIZE_MAX; /* pattern pos right after the last '*' seen */
    size_t star_t = 0;        /* text pos that star has consumed up to */

    while (t < tlen) {
        int advance = 0;

        if (p < plen) {
            char pc = pat[p];

            if (pc == '*') {
                star_p = ++p;
                star_t = t;
                continue;
            }
            if (pc == '?') {
                p++;
                advance = 1;
            } else if (pc == '[') {
                size_t consumed;

                if (class_match(pat + p, plen - p, text[t], &consumed) > 0) {
                    p += consumed;
                    advance = 1;
                }
                /* 0: not a member; -1: unterminated class, which never
                   matches (git behavior) -- both take the backtrack path. */
            } else if (pc == '\\') {
                if (p + 1 < plen && pat[p + 1] == text[t]) {
                    p += 2;
                    advance = 1;
                }
                /* a lone trailing backslash matches nothing */
            } else if (pc == text[t]) {
                p++;
                advance = 1;
            }
        }
        if (advance) {
            t++;
            continue;
        }
        if (star_p != SIZE_MAX) {
            t = ++star_t;
            p = star_p;
            continue;
        }
        return 0;
    }
    while (p < plen && pat[p] == '*')
        p++;
    return p == plen;
}

static size_t seg_end(const char *s, size_t len, size_t pos)
{
    while (pos < len && s[pos] != '/')
        pos++;
    return pos;
}

static int is_dstar_seg(const char *pat, size_t start, size_t end)
{
    return end - start == 2 && pat[start] == '*' && pat[start + 1] == '*';
}

/* Anchored whole-path match. '*', '?' and classes never cross '/'; a full
   "**" segment matches zero or more whole directories. The same two-pointer
   backtracking as seg_match, lifted to segment granularity, so this too is
   fully iterative. One asymmetry, verified against git: a trailing
   slash-plus-"**" needs at least one segment left (the pattern "ab" + "/"
   + "**" matches "ab/f" but not "ab" itself), which falls out naturally
   because a "**" segment the loop never reached still sits in the
   unconsumed pattern remainder. */
static int path_match(const char *pat, size_t plen, const char *path, size_t tlen)
{
    size_t p = 0;
    size_t t = 0;
    size_t star_p = SIZE_MAX; /* pattern pos right after the last "**" segment */
    size_t star_t = 0;        /* path pos that "**" has consumed up to */

    while (t < tlen) {
        size_t pe = seg_end(pat, plen, p);
        size_t te = seg_end(path, tlen, t);

        if (p < plen && is_dstar_seg(pat, p, pe)) {
            star_p = (pe < plen) ? pe + 1 : pe;
            star_t = t;
            p = star_p;
            continue;
        }
        if (p < plen && seg_match(pat + p, pe - p, path + t, te - t)) {
            p = (pe < plen) ? pe + 1 : pe;
            t = (te < tlen) ? te + 1 : te;
            continue;
        }
        if (star_p != SIZE_MAX) {
            size_t se = seg_end(path, tlen, star_t);

            star_t = (se < tlen) ? se + 1 : se;
            t = star_t;
            p = star_p;
            continue;
        }
        return 0;
    }
    return p == plen;
}

/* ---- pattern-list loading ---------------------------------------------- */

/* git's trim_trailing_spaces: unescaped trailing spaces are removed; a
   backslash keeps the following character (so "sp2\ " survives, and the
   matcher later reads the pair as a literal space). A lone backslash at the
   very end suppresses the trim entirely, matching git. Returns the new
   length. */
static size_t trim_trailing_spaces(const char *s, size_t len)
{
    size_t i;
    size_t last_space = SIZE_MAX;

    for (i = 0; i < len; i++) {
        if (s[i] == ' ') {
            if (last_space == SIZE_MAX)
                last_space = i;
        } else if (s[i] == '\\') {
            i++;
            if (i >= len)
                return len;
            last_space = SIZE_MAX;
        } else {
            last_space = SIZE_MAX;
        }
    }
    return last_space == SIZE_MAX ? len : last_space;
}

static int frame_reserve(sg_ignore_frame *f)
{
    size_t new_cap;
    sg_ignore_rule *grown;

    if (f->count < f->cap)
        return 0;
    new_cap = f->cap ? f->cap * 2 : 16;
    grown = realloc(f->rules, new_cap * sizeof(*grown));
    if (grown == NULL)
        return -1;
    f->rules = grown;
    f->cap = new_cap;
    return 0;
}

/* Parses one raw .gitignore line (git's order: strip CR, comment check on
   the raw first byte, trim trailing spaces, then '!' and the trailing '/').
   Lines that boil down to nothing are dropped. Returns 0 on success (which
   includes dropping the line), -1 on allocation failure. */
static int frame_add_line(sg_ignore_frame *f, const char *line, size_t len)
{
    int negated = 0;
    int dir_only = 0;
    int anchored = 0;
    sg_ignore_rule *r;
    char *copy;

    if (len > 0 && line[len - 1] == '\r')
        len--;
    if (len == 0 || line[0] == '#')
        return 0;
    len = trim_trailing_spaces(line, len);
    if (len > 0 && line[0] == '!') {
        negated = 1;
        line++;
        len--;
    }
    if (len > 0 && line[len - 1] == '/') {
        dir_only = 1;
        len--;
    }
    if (len > 0 && memchr(line, '/', len) != NULL)
        anchored = 1;
    if (len > 0 && line[0] == '/') {
        line++;
        len--;
    }
    if (len == 0)
        return 0;

    if (frame_reserve(f) != 0)
        return -1;
    copy = malloc(len + 1);
    if (copy == NULL)
        return -1;
    memcpy(copy, line, len);
    copy[len] = '\0';

    r = &f->rules[f->count++];
    r->pattern = copy;
    r->len = len;
    r->negated = negated ? 1 : 0;
    r->dir_only = dir_only ? 1 : 0;
    r->anchored = anchored ? 1 : 0;
    return 0;
}

/* Loads an ignore file into a frame. A missing or unreadable file is normal
   and leaves the frame empty; lines of any length are handled without
   truncation. Returns 0 on success, -1 on allocation failure. */
static int frame_load_file(sg_ignore_frame *f, const char *path)
{
    unsigned char *data;
    size_t len;
    size_t pos = 0;

    if (sg_read_file(path, &data, &len) != 0)
        return 0;

    while (pos < len) {
        size_t eol = pos;

        while (eol < len && data[eol] != '\n')
            eol++;
        if (frame_add_line(f, (const char *)data + pos, eol - pos) != 0) {
            free(data);
            return -1;
        }
        pos = eol + 1;
    }
    free(data);
    return 0;
}

static int frame_init(sg_ignore_frame *f, const char *base)
{
    memset(f, 0, sizeof(*f));
    f->base = strdup(base);
    if (f->base == NULL)
        return -1;
    f->base_len = strlen(base);
    return 0;
}

static void frame_free(sg_ignore_frame *f)
{
    size_t i;

    for (i = 0; i < f->count; i++)
        free(f->rules[i].pattern);
    free(f->rules);
    free(f->base);
    memset(f, 0, sizeof(*f));
}

static char *join_path(const char *a, const char *b)
{
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    char *out = malloc(alen + 1 + blen + 1);

    if (out == NULL)
        return NULL;
    memcpy(out, a, alen);
    out[alen] = '/';
    memcpy(out + alen + 1, b, blen);
    out[alen + 1 + blen] = '\0';
    return out;
}

/* ---- query evaluation -------------------------------------------------- */

/* Verdict of one frame for (path, len): SG_IGN_NO_MATCH if no rule applies,
   otherwise the verdict of the LAST matching rule in the file (scanned
   bottom-up, so the first hit found is the answer). */
static int frame_verdict(const sg_ignore_frame *f, const char *path, size_t len, int is_dir)
{
    const char *rel;
    size_t rel_len;
    const char *bn;
    size_t bn_len;
    size_t i;

    if (f->base_len == 0) {
        rel = path;
        rel_len = len;
    } else {
        /* Rules only apply strictly below the frame's own directory. */
        if (len <= f->base_len + 1 || memcmp(path, f->base, f->base_len) != 0 ||
            path[f->base_len] != '/')
            return SG_IGN_NO_MATCH;
        rel = path + f->base_len + 1;
        rel_len = len - f->base_len - 1;
    }
    if (rel_len == 0)
        return SG_IGN_NO_MATCH;

    bn = rel;
    bn_len = rel_len;
    for (i = rel_len; i-- > 0;) {
        if (rel[i] == '/') {
            bn = rel + i + 1;
            bn_len = rel_len - i - 1;
            break;
        }
    }

    for (i = f->count; i-- > 0;) {
        const sg_ignore_rule *r = &f->rules[i];
        int hit;

        if (r->dir_only && !is_dir)
            continue;
        if (r->anchored)
            hit = path_match(r->pattern, r->len, rel, rel_len);
        else
            hit = seg_match(r->pattern, r->len, bn, bn_len);
        if (hit)
            return r->negated ? SG_IGN_KEPT : SG_IGN_IGNORED;
    }
    return SG_IGN_NO_MATCH;
}

/* Walks sources from highest precedence to lowest (deepest pushed .gitignore
   first, then shallower ones, then info/exclude); the first frame with an
   opinion decides. */
static int query_verdict(const sg_ignore *ig, const char *path, size_t len, int is_dir)
{
    size_t i;

    for (i = ig->depth; i-- > 0;) {
        int v = frame_verdict(&ig->stack[i], path, len, is_dir);

        if (v != SG_IGN_NO_MATCH)
            return v;
    }
    return frame_verdict(&ig->exclude, path, len, is_dir);
}

int sg_ignore_is_ignored(const sg_ignore *ig, const char *relpath, int is_dir)
{
    size_t len;
    size_t i;

    if (ig == NULL || relpath == NULL || relpath[0] == '\0')
        return 0;
    len = strlen(relpath);

    /* An ignored directory buries everything under it: no pattern (not even
       a negation in a deeper .gitignore) can re-include content below it, so
       every leading directory is checked first. */
    for (i = 1; i < len; i++) {
        if (relpath[i] == '/' && query_verdict(ig, relpath, i, 1) == SG_IGN_IGNORED)
            return 1;
    }
    return query_verdict(ig, relpath, len, is_dir) == SG_IGN_IGNORED;
}

/* ---- lifecycle --------------------------------------------------------- */

/* Pushes a frame for dir_relpath ("" = worktree root) and loads its
   .gitignore if there is one. On failure nothing stays pushed. */
static int push_frame(sg_ignore *ig, const char *dir_relpath)
{
    sg_ignore_frame *f;
    char *file = NULL;
    int rc;

    if (ig->depth == ig->stack_cap) {
        size_t new_cap = ig->stack_cap ? ig->stack_cap * 2 : 8;
        sg_ignore_frame *grown = realloc(ig->stack, new_cap * sizeof(*grown));

        if (grown == NULL)
            return -1;
        ig->stack = grown;
        ig->stack_cap = new_cap;
    }
    f = &ig->stack[ig->depth];
    if (frame_init(f, dir_relpath) != 0)
        return -1;

    if (dir_relpath[0] == '\0') {
        file = join_path(ig->worktree_root, ".gitignore");
    } else {
        char *dir = join_path(ig->worktree_root, dir_relpath);

        if (dir != NULL) {
            file = join_path(dir, ".gitignore");
            free(dir);
        }
    }
    if (file == NULL) {
        frame_free(f);
        return -1;
    }
    rc = frame_load_file(f, file);
    free(file);
    if (rc != 0) {
        frame_free(f);
        return -1;
    }
    ig->depth++;
    return 0;
}

int sg_ignore_open(sg_ignore **out, const char *git_dir, const char *worktree_root)
{
    sg_ignore *ig;
    char *path;

    if (out == NULL || git_dir == NULL || worktree_root == NULL)
        return -1;
    *out = NULL;

    ig = calloc(1, sizeof(*ig));
    if (ig == NULL)
        return -1;
    ig->worktree_root = strdup(worktree_root);
    if (ig->worktree_root == NULL)
        goto fail;

    if (frame_init(&ig->exclude, "") != 0)
        goto fail;
    path = join_path(git_dir, "info/exclude");
    if (path == NULL)
        goto fail;
    if (frame_load_file(&ig->exclude, path) != 0) {
        free(path);
        goto fail;
    }
    free(path);

    /* stack[0]: the worktree root's own .gitignore. */
    if (push_frame(ig, "") != 0)
        goto fail;

    *out = ig;
    return 0;

fail:
    sg_ignore_free(ig);
    return -1;
}

int sg_ignore_push_dir(sg_ignore *ig, const char *dir_relpath)
{
    if (ig == NULL || dir_relpath == NULL || dir_relpath[0] == '\0')
        return -1;
    return push_frame(ig, dir_relpath);
}

void sg_ignore_pop_dir(sg_ignore *ig)
{
    /* stack[0] is the root .gitignore loaded by open, never pushed by the
       caller: popping it (or popping past it) is a caller bug -- ignored. */
    if (ig == NULL || ig->depth <= 1)
        return;
    ig->depth--;
    frame_free(&ig->stack[ig->depth]);
}

void sg_ignore_free(sg_ignore *ig)
{
    size_t i;

    if (ig == NULL)
        return;
    for (i = 0; i < ig->depth; i++)
        frame_free(&ig->stack[i]);
    free(ig->stack);
    frame_free(&ig->exclude);
    free(ig->worktree_root);
    free(ig);
}
