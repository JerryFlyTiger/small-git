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
       with '@', optionally followed by (whitespace, or nothing at all,
       then) an offset. A missing/malformed/out-of-range offset does NOT
       fail the parse: the timestamp is still honoured and `*tz_out`
       becomes the machine's LOCAL offset at that instant.
       WARNING (round 6, SPEC-CORRECTION-2.md): whitespace before the
       offset is NOT required for it to be RECOGNIZED -- "1700000000+0800"
       (no space at all) is PARSED (`*tz_out` = "+0800"), not merely
       tolerated-and-discarded the way an earlier round of this project
       believed, measured on a machine whose own local zone happens to be
       +0800, where "parsed +0800" and "fell back to local +0800" render
       identically and so could not be told apart. Whitespace's only
       remaining role is what happens when the trailing content does NOT
       match the offset grammar (below) at ALL: WITH a space, unrecognized
       content is tolerated as a single token of junk (still local, still
       honoured -- "@1700000000 x"); a SECOND space-separated token past it
       (e.g. "2 hours ago") is a parse FAILURE, not junk -- this is what
       tells that shape apart from git's own relative-date phrases, which
       this parser deliberately does NOT implement (--date=relative is a
       RENDERER, Phase 66, a different and looser grammar than this strict
       ident-date parser; git itself rejects "yesterday"/"now"/
       "2 hours ago" here while accepting them elsewhere). WITHOUT a space,
       unrecognized attached content is a hard parse FAILURE (no
       tolerance) -- this is what tells "2023-11-15" (rejected: "-11-15"
       matches no offset shape) apart from "1700000000+0800" (parsed).
     - "YYYY-MM-DDTHH:MM:SS[+-]HH:MM"    -- ISO 8601 strict, colon offset.
       The offset must be ATTACHED, no space -- this was already true
       before Phase 75a ("no extra internal whitespace before an ISO
       offset", a deliberate rejection) and Phase 75a does NOT carve out
       an exception for the zone-NAME spellings: "...00:00:00 Z" (a space
       before the name) is measured as accepted by git but stays REFUSED
       by this parser, pinned in tests/test_ident.c and interop's
       phase75a group. "...00:00:00Z" (no space) IS accepted.
     - "YYYY-MM-DD HH:MM:SS [+-]HHMM"    -- ISO-ish, space-separated, no
       colon in the offset (though the offset token itself may still use a
       colon -- see the grammar below, shared by every form).
     - "[Www, ]DD Mon YYYY HH:MM:SS [+-]HHMM" -- RFC 2822 (the weekday-and-
       comma prefix, if present at all, is skipped without being validated
       against the actual day of week). The offset is ordinarily its own
       whitespace-separated token, but Phase 75a adds ONE attached
       exception: a zone NAME (only) glued directly onto the seconds with
       no space, e.g. "...06:13:20Z" -- measured, git accepts it. An
       attached DIGIT offset in this form ("...06:13:20+0800") is not
       measured and stays unimplemented.
       All three of the above REQUIRE a syntactically well-formed, IN-RANGE
       offset -- unlike the timestamp form, an out-of-range offset is a
       hard parse FAILURE here (round 5's decision: sg refuses rather than
       reproduce git's "reinterpret as local time" behaviour for a
       calendar form, which is ill-defined during a DST gap/overlap).
     - anything else (a bare date with no time-of-day, a relative phrase,
       garbage, a value strtoll cannot hold) -> parse FAILURE.

   The offset TOKEN grammar itself, shared by every form above (round 6,
   SPEC-CORRECTION-2.md -- re-measured under both TZ=UTC and
   TZ=Asia/Kolkata, since a single +0800 measuring machine cannot tell
   "parsed +0800" apart from "fell back to local +0800"):
     - <sign> then EXACTLY 2 digits    ("+08" -> +0800; the sign is
       REQUIRED for this shape only)
     - [<sign>] then EXACTLY 4 digits  ("+0800", "0800" -- sign optional)
     - [<sign>] <2 digits>:<2 digits>  ("+08:00", "00:00" -- sign optional)
     - "Z" / "UTC" / "GMT", case-insensitive (Phase 75a, measured against
       real git 2.55.0) -- a WHOLE-WORD match worth +0000. The matched
       name must be the entire remaining token: "ZULU"/"ZZ"/"UTC1"/
       "UTC+1"/"GMT0"/"GMT+0" are NOT this token (git's own answer for
       every one of those is either "falls back to local" or "ignores the
       trailing junk", both different from a clean +0000, and this parser
       refuses rather than guess -- see docs/RULES-date.md's phase75a
       table). No other zone name git implements (EST, PST, CET, JST,
       ...) is recognized here, deliberately -- same table.
   Anything else -- 1 digit, 3 digits, 5 or more digits, exactly 2 digits
   with NO sign, or a name not in the list just above -- is NOT a token at
   all, and the value falls back to local the same way a malformed one
   does. The digit-count check is exact (a run of 5 digits is not "the
   first 4 of it"): "+08000" is NOT a token and falls back to local, it is
   not read as "+0800" plus an ignorable trailing digit.
   Range-checking (hours <= 23, minutes <= 59) and the POLICY on a
   recognized-but-out-of-range token are the CALLER's -- the timestamp
   form (bare and calendar alike) discards it for local, the `@` form
   normalizes it ARITHMETICALLY instead (`+9999` -> `+10039`, HH*60+MM
   re-rendered, hours width unbounded) and never range-checks at all. Do
   not "fix" the range to a real-world +/-14:00 bound; the rule is purely
   numeric (measured: "+1500"/"+2359" are accepted verbatim by real git).

   Returns 0 and fills *time_out / tz_out (a caller-supplied buffer of at
   least 8 bytes) on success, -1 on any of the failures above. */
int sg_ident_parse_date(const char *raw, long long *time_out, char tz_out[8]);

#endif /* SG_IDENT_H */
