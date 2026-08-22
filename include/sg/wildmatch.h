#ifndef SG_WILDMATCH_H
#define SG_WILDMATCH_H

#include <stddef.h>

/* git's wildmatch, in its "'/' is not special" mode -- the flavour git uses
   for pathspecs (WM_PATHNAME unset), where '*' happily crosses directory
   boundaries. Measured against git 2.55.0: `git diff -- '*.c'` reports
   `other/d.c`, and a spec of sub-slash-star reports `sub/deep/c.txt`.

   Supports '*', '?', bracket classes (including ranges, '!'/'^' negation and
   a leading ']' as a literal member) and '\\' escapes. An unterminated class
   never matches anything at all, exactly like git's wildmatch.

   The matcher is iterative two-pointer backtracking, O(plen*tlen) worst case
   and zero recursion: patterns reach it from cloned .gitignore files and from
   argv, so a pattern of 10,000 '*'s must not be able to smash the stack.

   gitignore's own matching adds a segment layer on top of this (where '*'
   stops at '/' and a whole "**" segment spans directories) -- that layer
   lives in src/workdir/ignore.c and calls this function once per segment.
   Both callers share this one implementation on purpose: they are the same
   matcher in git too, differing only by the WM_PATHNAME flag.

   Takes explicit lengths because ignore.c matches substrings of a longer
   pattern without copying them out. Returns 1 on a match, 0 otherwise. */
int sg_wildmatch(const char *pat, size_t plen, const char *text, size_t tlen);

#endif /* SG_WILDMATCH_H */
