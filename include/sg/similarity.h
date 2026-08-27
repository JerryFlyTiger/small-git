#ifndef SG_SIMILARITY_H
#define SG_SIMILARITY_H

#include <stddef.h>

/* "How much of this file's content came from that one", the estimator git
   uses to decide that a delete plus an add is really a rename.

   This is a port of git's diffcore-delta.c, and the port has to be exact:
   the answer is printed in a machine-readable field ("R093", "similarity
   index 93%"), so being one point off makes interop's byte-for-byte compare
   fail. Do not "improve" the estimate -- an estimate that is better but
   different is, here, simply wrong.

   The idea (git's own comment says it plainly): cut both buffers into chunks
   ending at a newline or after 64 bytes, whichever comes first, hash each
   chunk, and count bytes per hash value. Where the source holds more bytes
   under a hash value than the destination does, the difference is content
   that was NOT carried over; where the destination holds more, that is
   content it added. Nothing stores the chunks themselves, so two buffers
   that merely permute their lines score as identical -- that is git's
   behaviour, measured, not an artifact of this port.

   Deliberately NOT reused for this: src/util/diff_lcs.c (line alignment for
   patch output) and src/util/levenshtein.c (command-name spelling). Both
   answer a different question, and neither would reproduce git's numbers. */

/* git's MAX_SCORE. Scores live on this scale, not on 0-100, because the
   -M grammar can ask for thresholds finer than one percent (`-M005` is
   0.5%), so a percentage cannot hold a threshold without rounding it. */
#define SG_SIMILARITY_MAX 60000
/* git's DEFAULT_RENAME_SCORE: 50%, the threshold a bare -M or no -M means. */
#define SG_SIMILARITY_DEFAULT 30000

/* The per-hash-value byte counts of one buffer. Building this is the
   expensive half of a comparison, and a rename search compares one
   destination against many sources, so callers build it once per file and
   keep it -- which is also why it is a separate type rather than an
   internal detail of the scoring call. */
typedef struct sg_spanhash sg_spanhash;

/* Builds the table for `len` bytes at `buf` (`buf` may be NULL iff len is 0).
   Returns NULL on allocation failure.

   Whether the buffer counts as text is decided here, from the bytes
   themselves, exactly as git's buffer_is_binary does it: a NUL anywhere in
   the first 8000 bytes makes it binary. The only thing it changes is that a
   CR immediately followed by LF is skipped in text, so a file with CRLF line
   endings hashes the same as the same file with LF endings. It does NOT drop
   those bytes from the file's size, so a CRLF file that is renamed with no
   edit at all would score about 93%, not 100% -- which is exactly why git
   settles exact renames by object id BEFORE scoring anything, and why this
   module is never asked about a pair whose content is byte-identical. */
sg_spanhash *sg_spanhash_build(const unsigned char *buf, size_t len);

void sg_spanhash_free(sg_spanhash *h);

/* git's diffcore_count_changes.
   *src_copied  = how many bytes of the destination were found in the source.
   *literal_added = how many bytes the destination has that the source did not.
   Either out-pointer may be NULL. */
void sg_spanhash_count_changes(const sg_spanhash *src, const sg_spanhash *dst,
                               unsigned long *src_copied,
                               unsigned long *literal_added);

/* The cheap half of git's estimate_similarity: returns 1 when the two sizes
   alone already rule the pair out at this threshold, so the caller can skip
   reading and hashing the content at all.

   Kept separate from the scoring call rather than folded into it because
   that is the whole point of it -- git checks sizes before it loads either
   blob. Folding it in would still give the right answer and would read the
   files anyway. */
int sg_similarity_size_rejects(size_t src_len, size_t dst_len, int min_score);

/* The scoring half: 0..SG_SIMILARITY_MAX, where SG_SIMILARITY_MAX means the
   destination is entirely accounted for by the source. `src_len`/`dst_len`
   are the real file sizes, which are NOT the same as the byte counts in the
   tables (see the CRLF note above), and the score is measured against the
   LARGER of the two -- so adding material costs score just as removing it
   does. A zero-length destination scores 0. */
int sg_similarity_score(const sg_spanhash *src, size_t src_len,
                        const sg_spanhash *dst, size_t dst_len);

/* git's similarity_index(): the 0-100 number actually printed. Truncates,
   never rounds -- 59999 is 99%, not 100%. */
int sg_similarity_percent(int score);

/* git's parse_rename_score, the argument grammar of -M/--find-renames.
   Consumes the number at *cp and advances *cp past it; the caller decides
   what a non-empty remainder means (git rejects it).

   Every rule here was measured against git 2.55.0 and every one of them is
   counter-intuitive, so do not "simplify" it: the digits are read as a
   FRACTION with an implied leading decimal point unless a '%' follows, which
   makes `-M5` mean 50% and `-M05` mean 5%. In particular `-M100` is 10%, NOT
   exact-renames-only; only `-M100%` is. An empty argument yields 0, which
   callers turn into SG_SIMILARITY_DEFAULT. */
int sg_similarity_parse_score(const char **cp);

/* The CLI-facing wrapper around sg_similarity_parse_score: consumes the
   whole of `arg` (not just a prefix), rejects it if anything is left over,
   and turns a parse of 0 (a bare "-M0", "-M%", or "" -- see above) into
   SG_SIMILARITY_DEFAULT, exactly as -M/--find-renames does when its value
   is empty. Shared by `sg diff -M<n>` and `sg stash show -M<n>` so the
   grammar has exactly one CLI-facing copy. Returns 0 and fills *out on
   success, -1 on a malformed argument. */
int sg_similarity_parse_score_arg(const char *arg, int *out);

#endif /* SG_SIMILARITY_H */
