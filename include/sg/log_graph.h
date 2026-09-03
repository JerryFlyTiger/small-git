#ifndef SG_LOG_GRAPH_H
#define SG_LOG_GRAPH_H

#include <stddef.h>
#include <stdio.h>

/* Phase 63: `sg log --graph`, first-parent walk only -- see CLAUDE.md's
   `sg log` entry and docs/DESIGN.md's Phase 63 section for the full
   derivation, including the measured 0.1a exception below. Since a
   first-parent walk can never branch, the graph column is a per-line
   PREFIX, not a real graph layout: "* " on an entry's first line,
   otherwise a caller-chosen two-byte continuation string on every other
   line, including an otherwise-empty separator line between two entries.
   No multi-column characters (`|\`, `|/`, `* |`) are ever produced; there
   is deliberately no code path for them (a first-parent walk cannot
   produce the shape they exist for, and there is no oracle to check them
   against). */
typedef struct {
    /* 1 if the next byte written begins a new line. */
    int at_line_start;
    /* 1 if the line currently being written is the first line of the
       current log entry. Set by sg_log_graph_begin_entry; cleared the
       moment any byte of that line is written. */
    int entry_first_line;
} sg_log_graph_prefixer;

/* Initializes the prefixer to the start of a fresh output stream: the very
   next byte written begins a line. Called once per `sg log --graph`
   invocation, not once per entry. */
void sg_log_graph_init(sg_log_graph_prefixer *pfx);

/* Marks the next line written as the first line of a new log entry (it
   will get the "* " prefix instead of the caller's continuation string).
   Call this once, immediately before feeding that entry's own captured
   bytes to sg_log_graph_write. */
void sg_log_graph_begin_entry(sg_log_graph_prefixer *pfx);

/* Writes len bytes from buf to out, one at a time, prefixing "* " onto an
   entry's first line and `cont` (a caller-supplied two-byte string, e.g.
   "| " or "  ") onto every other line -- including the mid-stream
   separator "\n" a caller feeds between two entries. An empty write
   (len == 0) produces no output at all -- it must never emit a lone
   prefix for nothing written.

   `cont` exists because of a measured exception (Phase 63 section 0.1a):
   under a first-parent walk that reaches the true end of history (the
   root commit, `parent_count == 0`) with no `-n` cutoff and no error, the
   LAST printed entry's continuation lines use "  " (two spaces, same
   width as "| ") instead of "| " -- git's graph renderer knows there is no
   line left to draw underneath it. The decisive fixture is a pathspec that
   filters out everything between the last MATCHING commit and the root:
   the last matching commit still has a parent, but the walk continues
   past it, unprinted, all the way to the root before stopping naturally,
   and that commit's own continuation lines still get "  ". So the
   predicate is "did the walk end naturally", never "does this particular
   commit have a parent" -- a `-n`-truncated walk's last entry has a parent
   too, and still gets "| ". This is exactly why the decision cannot be
   made until it is known whether a following entry exists at all, which
   is why the caller holds back exactly one entry (see cmd_log.c) rather
   than writing each entry through this function as soon as it is
   captured: cont for entry k is only knowable once entry k+1 either
   arrives or definitely doesn't. Single-line formats (--oneline,
   format:/tformat:, reference) have no continuation lines at all, so they
   cannot distinguish "| " from "  " and are not a valid test of this rule
   -- a real regression here needs a multi-line format (medium/fuller/
   full/raw). */
void sg_log_graph_write(sg_log_graph_prefixer *pfx, const char *buf, size_t len, const char *cont,
                         FILE *out);

/* Flushes one whole log entry: calls sg_log_graph_begin_entry (so the
   entry's own first line gets "* ") and then writes its captured bytes
   via sg_log_graph_write -- EXCEPT that when len == 0, it still emits the
   "* " marker for that entry, which sg_log_graph_write's byte loop cannot
   do on its own (its loop body never runs at all for len == 0, by design,
   since it must not invent a prefix for a separator write that legitimately
   writes nothing).

   This one function is the ONLY place that combines begin_entry with
   writing an entry's bytes -- do not call sg_log_graph_begin_entry and
   sg_log_graph_write separately at a new call site; that split is exactly
   how a real bug shipped in this phase's own first round (Phase 63,
   review round): an entry whose expansion is the empty string (e.g.
   `--pretty=format:%b` on a commit with no message body) lost its "* "
   marker entirely, because the marker was written lazily by the byte
   loop and there was no byte to attach it to. A MIDDLE such entry's
   marker was silently absorbed by the FOLLOWING entry's separator "\n"
   (which still went through the byte loop and still triggered the
   at_line_start prefix, just one entry late), so the bug was invisible
   except on the very LAST entry of a run, which has no following
   separator to borrow from and simply lost its marker -- 11 empty entries
   rendered 10 markers, one line short, with an extra trailing "\n" real
   git does not have (see docs/DESIGN.md's Phase 63 review-round section
   for the full byte comparison). */
void sg_log_graph_write_entry(sg_log_graph_prefixer *pfx, const char *buf, size_t len,
                               const char *cont, FILE *out);

#endif
