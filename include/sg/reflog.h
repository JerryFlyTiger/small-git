#ifndef SG_REFLOG_H
#define SG_REFLOG_H

#include <stddef.h>

#include "sg/hash.h"

typedef struct {
    unsigned char old_id[SG_SHA1_RAW_LEN];
    unsigned char new_id[SG_SHA1_RAW_LEN];
    char *ident;   /* malloc'd, owned. The raw bytes between the new oid and
                      the TAB, e.g. "T U <t@e.com> 1786357285 +0800". Never
                      interpreted -- kept verbatim so that rewriting the file
                      (drop) preserves a real-git-authored entry's author and
                      timestamp instead of restamping it with sg's. */
    char *message; /* malloc'd, owned. Everything after the TAB, without the
                      newline. "" when the line had no TAB at all. */
} sg_reflog_entry;

typedef struct {
    sg_reflog_entry *entries; /* FILE ORDER: entries[0] is the first (oldest) line */
    size_t count;
} sg_reflog;

/* Reads git_dir/logs/<ref_path>. A missing file (ENOENT) is not an error:
   returns 0 with an empty *out (count 0, entries NULL) -- "no stash has ever
   been pushed" is a normal state, not a failure. Any OTHER I/O failure
   (permission denied, a read error mid-file, ...) returns -1, same as a
   malformed line (shorter than 81 bytes, or either oid field not 40 hex
   digits) -- a caller must not treat "cannot read the reflog" as "the stash
   is empty". A missing newline on the final line is accepted; real git
   tolerates it (measured) and a file truncated mid-write should not make the
   surviving entries unreadable. ref_path is validated with the same
   path-safety rule as sg_ref_read_path. */
int sg_reflog_read(const char *git_dir, const char *ref_path, sg_reflog *out);
void sg_reflog_free(sg_reflog *log);

/* Appends one line to git_dir/logs/<ref_path>, creating parent directories.
   The identity is the usual GIT_AUTHOR_NAME/EMAIL pair with the
   small_git/sg@localhost fallback, the timestamp is time(NULL), and the
   timezone is the fixed "+0000" literal every sg-written commit already
   carries -- real git never interprets these fields (measured: it reads a
   hand-forged stash written with exactly these values).

   message is normalized the way real git's copy_reflog_msg does, because one
   entry is one line and a raw newline would forge a second entry: leading
   whitespace is dropped, every run of whitespace (including '\n' and '\t')
   collapses to a single ' ', and trailing spaces are trimmed. Measured
   against `git stash push -m $'line1\nline2\twith tab'`, whose reflog
   subject is "On master: line1 line2 with tab" while the commit message
   keeps the real newline and tab.

   *appended_at_out, when non-NULL, receives the file's size BEFORE the
   append, so a caller whose follow-up ref write fails can truncate the file
   back and leave the reflog consistent with the ref. Returns 0, -1 on I/O
   or allocation failure. */
int sg_reflog_append(const char *git_dir, const char *ref_path,
                     const unsigned char old_id[SG_SHA1_RAW_LEN],
                     const unsigned char new_id[SG_SHA1_RAW_LEN], const char *message,
                     long long *appended_at_out);

/* Replaces git_dir/logs/<ref_path> with exactly these entries, in file
   order, via a temp file + rename. Each entry's ident and message are
   written verbatim (see sg_reflog_entry.ident); only the old_id fields are
   regenerated, so that entry i's old_id is entry i-1's new_id and entry 0's
   is all zeros -- the shape real git leaves behind after `git stash drop`.
   Chaining is cosmetic for readers (measured: git reads a fully zeroed chain
   without complaint) but is reproduced because it is free and because a
   future `sg reflog` would rely on it.
   count == 0 REMOVES the file, matching what git does when the last stash
   entry goes away. Returns 0, -1 on I/O failure. */
int sg_reflog_rewrite(const char *git_dir, const char *ref_path, const sg_reflog_entry *entries,
                      size_t count);

/* Truncates git_dir/logs/<ref_path> back to `offset` bytes -- used to undo a
   reflog append when a follow-up ref write fails, so the tip invariant
   (the ref's value == the last reflog line's new-oid) is never left broken.
   Best effort: a failure here just leaves a slightly-too-long reflog file,
   which is far less harmful than a ref/reflog mismatch. Does not validate
   ref_path with sg_ref_branch_name_is_safe -- callers only ever pass back a
   path they themselves just built for sg_reflog_append. */
void sg_reflog_truncate(const char *git_dir, const char *ref_path, long long offset);

/* Returns entries[count - 1 - n] -- i.e. @{n} reflog notation, where @{0} is
   the most recent entry. The oid this notation refers to is the entry's
   NEW_ID, not old_id (easy to get backwards: @{0} means "the value the ref
   was moved TO by the most recent update", not "the value it had before
   that update"). Returns a borrowed pointer into log->entries (valid until
   log is freed/mutated), or NULL if n >= log->count (including the
   count == 0 case). */
const sg_reflog_entry *sg_reflog_at(const sg_reflog *log, size_t n);

#endif
