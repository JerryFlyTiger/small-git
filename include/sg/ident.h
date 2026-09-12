#ifndef SG_IDENT_H
#define SG_IDENT_H

/* Phase 72: the single resolver for "who is writing this object, and at
   what instant" -- replaces eight byte-identical env_or() copies
   (storage/reflog.c, storage/chunk.c, safety/stash.c, safety/snapshot.c,
   cli/cmd_rebase.c, cli/cmd_merge.c, cli/cmd_tag.c, cli/cmd_commit.c), all
   of which read only the NAME and EMAIL vars and none of which read a DATE
   var at all --
   every object sg writes therefore had a different id than the one git
   would write from the same inputs, which is exactly the property this
   project exists to have (see CLAUDE.md's Phase 72 notes / docs/DESIGN.md).

   sg reads no config and never will (this file does not change that): the
   name/email fallback, absent the corresponding env var, stays exactly
   "small_git" / "sg@localhost" -- a pre-existing, deliberate divergence
   from real git (which refuses to commit with no identity at all), out of
   scope here. */

typedef struct {
    char name[256];
    char email[256];
    long long when; /* epoch seconds */
    /* "+HHMM" / "-HHMM", 5 bytes + NUL in every ordinary case -- but NOT
       always: git's `@<epoch> <offset>` form normalizes the offset
       ARITHMETICALLY, so "+9999" (99h99m) becomes "+10039" (100h39m), SIX
       bytes. The buffer is sized for it (a 4-digit token caps the result at
       6 bytes), and callers must not assume 5. WARNING: `parse_tz` in
       src/util/date.c DOES assume 5 and gives up on anything else, so a
       6-byte offset renders with the wrong clock while echoing the right
       offset string -- a PRE-EXISTING renderer bug (a git-written object
       with such an offset misrenders identically), pinned in interop's
       phase72 group, not fixed here. */
    char tz[8];
} sg_ident;

/* Resolves the AUTHOR identity: GIT_AUTHOR_NAME / GIT_AUTHOR_EMAIL (fallback
   "small_git" / "sg@localhost") and GIT_AUTHOR_DATE. Resolves the COMMITTER
   identity the identical way, from GIT_COMMITTER_NAME / GIT_COMMITTER_EMAIL
   / GIT_COMMITTER_DATE -- independently: an unset GIT_COMMITTER_NAME does
   NOT fall back to whatever GIT_AUTHOR_NAME resolved to, it falls back to
   the same hardcoded default the author side would use absent its own env
   var. This matches real git: with no config, git's committer identity is
   not derived from the author identity, both are independently resolved
   from the same underlying default.

   Date resolution (shared by both, see sg_ident_parse_date's own comment
   for the exact grammar):
     - the corresponding *_DATE var absent, or present and empty -> `when`
       is the current time, `tz` the machine's own offset AT THAT INSTANT
       (never a cached "now"'s offset -- this reuses
       sg_date_local_offset_seconds, the same function every
       "--date=*-local" renderer shifts by; see that function's own header
       comment in date.h. It is therefore also subject to CLAUDE.md's
       divergence #7 in a zone whose UTC offset is not a whole number of
       minutes -- unaffected in practice, since the object format itself
       can only ever store a whole-minute "+HHMM" offset, so there is
       nothing for a sub-minute offset to lose here that it would not also
       lose being written into the object).
     - present and non-empty -> parsed strictly by sg_ident_parse_date.

   Returns 0 on success. Returns -1 if the corresponding *_DATE var is
   present, non-empty, and fails to parse; *bad_value_out (may be NULL) is
   then set to a BORROWED pointer (owned by the environment, valid for the
   life of the process, do not free) holding the raw offending value, for
   the caller's own "sg: invalid date format: <value>" message (git prints
   the identical wording and exits 128; this project's own 0-or-1 exit-code
   convention makes sg's exit 1). On failure *out is left zeroed. */
int sg_ident_author(sg_ident *out, const char **bad_value_out);
int sg_ident_committer(sg_ident *out, const char **bad_value_out);

/* The parser behind both of the above's *_DATE handling, exposed directly
   for the unit tests (tests/test_ident.c) to drive with values that never
   touch the environment. `raw` must be non-empty (the empty-string ->
   "now" fallback is the caller's job, not this function's -- an empty
   string has no offending value to report and is not itself a parse
   failure). Accepted grammar, all measured against real git 2.55.0 -- see
   CLAUDE.md's Phase 72 section for the full derivation:

     - "[@]<digits>[ <tz>]"      -- a unix timestamp, optionally prefixed
       with '@', optionally followed by (one or more spaces, then) an
       offset. A missing/malformed/absent offset does NOT fail the parse:
       the timestamp is still honoured and `*tz_out` becomes the machine's
       LOCAL offset at that instant. Whitespace matters: with no space at
       all between the digits and what follows, the tail is honoured only
       if it looks exactly like an attached "[+-]dddd" offset (still not
       applied, for want of the required space -- `*tz_out` is still
       local); anything else immediately attached (no space) is a parse
       FAILURE, not a fallback -- this is what tells "1700000000+0800"
       (accepted, local) apart from "2023-11-15" (rejected) despite both
       being "digits then more stuff with no space". With a space present,
       a single further token that fails to parse as an offset is tolerated
       as junk (still local, still honoured); a SECOND space-separated
       token past that (e.g. "2 hours ago") is a parse FAILURE -- this is
       what tells "@1700000000 x" (accepted, local) apart from git's
       relative-date phrases, which this parser deliberately does NOT
       implement (--date=relative is a RENDERER, Phase 66, a different and
       looser grammar than this strict ident-date parser; git itself
       rejects "yesterday"/"now"/"2 hours ago" here while accepting them
       elsewhere).
     - "YYYY-MM-DDTHH:MM:SS[+-]HH:MM"    -- ISO 8601 strict, colon offset.
     - "YYYY-MM-DD HH:MM:SS [+-]HHMM"    -- ISO-ish, space-separated, no
       colon in the offset.
     - "[Www, ]DD Mon YYYY HH:MM:SS [+-]HHMM" -- RFC 2822 (the weekday-and-
       comma prefix, if present at all, is skipped without being validated
       against the actual day of week).
       All three of the above REQUIRE a syntactically well-formed offset;
       unlike the timestamp form, there is no "tolerate junk, fall back to
       local" here.
     - anything else (a bare date with no time-of-day, a relative phrase,
       garbage, a value strtoll cannot hold) -> parse FAILURE.

   The offset field itself, in every form that carries one: exactly
   sign + 4 digits (optionally with a colon after the first two, ISO-strict
   only), hours <= 23 and minutes <= 59. Anything outside that numeric
   range is NOT a hard failure for the timestamp form -- the offset is
   discarded and the machine's local offset is used instead, the timestamp
   itself is kept. Do not "fix" the range to a real-world +/-14:00 bound;
   the rule is purely numeric (measured: "+1500"/"+2359" are accepted
   verbatim by real git).

   Returns 0 and fills *time_out / tz_out (a caller-supplied buffer of at
   least 8 bytes) on success, -1 on any of the failures above. */
int sg_ident_parse_date(const char *raw, long long *time_out, char tz_out[8]);

#endif /* SG_IDENT_H */
