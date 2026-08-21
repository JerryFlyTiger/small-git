#ifndef SG_QUOTE_H
#define SG_QUOTE_H

/* Display-side quoting of paths, matching git's convention for `status`,
   `diff` headers, and `cat-file -p` tree entries: a path is wrapped in
   double quotes and C-style-escaped if and only if it contains a byte that
   needs escaping. Bytes >= 0x80 are printed as-is (see below) -- everything
   else, including the escape table itself, matches git byte for byte.

   Why these functions return a borrowed, short-lived `const char *` instead
   of a malloc'd string the caller must free: there are close to a hundred
   call sites, most of them on error paths. A malloc'd-string API would need
   an OOM branch at every one of them, and the natural way to write that
   branch -- `q ? q : path` -- is fail-open: it prints the raw, unescaped
   path exactly when memory is tightest, which is the one thing this code
   exists to prevent. A static ring buffer moves the OOM decision to a
   single place (this file) where it cannot be gotten wrong by a call site.

   Why not a caller-supplied buffer instead: escaping can expand a path by
   up to 4x (each byte can become `\ggg`), so a caller-side buffer sized for
   SG_PATH_MAX would need roughly 16 KB on the stack. cmd_add.c's directory
   walk is recursive, so that cost is paid on every stack frame. It also
   just pushes the "what if it's not long enough" question back onto every
   call site, which is the shape of the bug this module exists to fix.

   Why no version that appends into an existing buffer: the two places that
   need to build up a larger string around a quoted path copy the result
   immediately anyway (cmd_diff.c's "a/"/"b/" prefixing is handled by
   sg_quote_path_prefixed so it never needs to pre-assemble "a/<path>";
   strbuf-based confirmation prompts do a plain strbuf_append, which already
   copies). There is no real call site for a buffer-writing variant, so one
   is not provided just for API symmetry.

   Threading: the whole repository has zero pthread usage and libcurl is
   used via its easy (single-threaded, blocking) interface, so the static
   ring below has no reentrancy concerns. If threads are ever introduced,
   this file needs to change first.

   OOM behavior: if the ring cannot grow to fit a path, these functions
   return the fixed string "(unprintable path)" rather than falling back to
   the raw, unescaped bytes -- the one thing they must never do, since an
   unescaped path is exactly the terminal-injection vector this module
   exists to close.

   The backing buffers are file-scope (process-lifetime) globals, like the
   mmap pack registry in storage/pack.c. LSan treats globals as GC roots, so
   they show up as still-reachable rather than as leaks under the ASan job's
   detect_leaks=1. */

#define SG_QUOTE_SLOTS 4

/* Quotes/escapes `path` if needed; otherwise returns it unmodified (still
   via the ring, so the returned pointer's lifetime rules are the same
   either way). Use for paths that occupy their own line in an indented
   listing (git status, cat-file -p tree entries) where the line structure
   itself is the delimiter and an unconditional quote would just make sg's
   output diverge from git's for the common (ASCII, no escaping needed)
   case. */
const char *sg_quote_path(const char *path);

/* Like sg_quote_path, but for a path that is about to be printed with a
   fixed literal prefix ("a/" or "b/" in diff headers). Whether to quote is
   decided from `path` alone; if quoting is needed, the quotes wrap prefix
   and path together (`"a/x\ty"`), because the quotes must enclose the
   prefix too -- deciding "should we quote" and "does the prefix go inside
   the quotes" in two different places is exactly how you end up emitting
   `a/"x\ty"`. If no quoting is needed, the prefix is emitted unquoted and
   unescaped ahead of the plain path, matching git. */
const char *sg_quote_path_prefixed(const char *prefix, const char *path);

/* Always wraps the result in double quotes, whether or not any byte needed
   escaping. Use this -- and only this -- when a path is embedded inside a
   sentence (`sg: cannot stat %s: no such file`) rather than occupying its
   own line: without a delimiter, a leading/trailing space or an embedded
   space becomes invisible in the surrounding text. */
const char *sg_quote_path_delimited(const char *path);

/* Like sg_quote_path, but for `status --short`/`--porcelain` output, where
   the difference is about LAYOUT, not source: a porcelain line is
   "XY<space>path", so a space anywhere in path (even one that needs no
   C-style escaping) would read as an extra field unless the whole thing is
   quoted. Quotes if path needs C-quoting (same rule as sg_quote_path) OR
   contains a space; otherwise returns it unmodified. The long-format
   status output (kind_label(), the "Changes to be committed:"/"Untracked
   files:" sections) must keep using sg_quote_path instead -- git measured
   2.55.0 does NOT quote a plain space there, only real escape-needing
   bytes, so applying this function to that output would diverge from git
   for the common case. */
const char *sg_quote_path_porcelain(const char *path);

#endif
