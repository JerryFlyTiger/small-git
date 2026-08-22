#include "sg/wildmatch.h"

#include <stdint.h>

/* Extracted verbatim from src/workdir/ignore.c, where it was the per-segment
   matcher; see include/sg/wildmatch.h for why the two callers share it. */

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
int sg_wildmatch(const char *pat, size_t plen, const char *text, size_t tlen)
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
