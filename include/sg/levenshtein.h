#ifndef SG_LEVENSHTEIN_H
#define SG_LEVENSHTEIN_H

#include <stddef.h>

/* Classic edit distance (insertion/deletion/substitution, unit cost) between
   two NUL-terminated strings, via a straightforward O(strlen(a)*strlen(b))
   dynamic-programming table. Command names are short, so no attempt is made
   to optimize beyond that. */
size_t sg_levenshtein(const char *a, const char *b);

#endif
