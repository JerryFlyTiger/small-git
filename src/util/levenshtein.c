#include "sg/levenshtein.h"

#include <stdlib.h>
#include <string.h>

size_t sg_levenshtein(const char *a, const char *b)
{
    size_t la = strlen(a);
    size_t lb = strlen(b);
    size_t *prev = malloc((lb + 1) * sizeof(*prev));
    size_t *curr = malloc((lb + 1) * sizeof(*curr));
    size_t i, j;
    size_t result;

    if (prev == NULL || curr == NULL) {
        free(prev);
        free(curr);
        return la > lb ? la : lb; /* degrade gracefully on OOM */
    }

    for (j = 0; j <= lb; j++)
        prev[j] = j;

    for (i = 1; i <= la; i++) {
        curr[0] = i;
        for (j = 1; j <= lb; j++) {
            size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            size_t del = prev[j] + 1;
            size_t ins = curr[j - 1] + 1;
            size_t sub = prev[j - 1] + cost;
            size_t best = del < ins ? del : ins;

            curr[j] = best < sub ? best : sub;
        }
        memcpy(prev, curr, (lb + 1) * sizeof(*prev));
    }

    result = prev[lb];
    free(prev);
    free(curr);
    return result;
}
