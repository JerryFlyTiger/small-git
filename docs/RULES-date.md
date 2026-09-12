# Rules: date and timestamp rendering

Scope: `src/util/date.c`, `include/sg/date.h` -- every `--date=<format>` name, `relative`, `human`, and the date placeholders.

**Read this file before editing anything in that scope.** It is not
background reading: every rule here was measured against real git 2.55.0,
and most of the `WARNING:`s exist because something slipped past a fully
green board. Project-wide rules -- the gates, the code conventions, the
deliberate divergences -- stay in `CLAUDE.md`; the full derivations and
measurement tables stay in `docs/DESIGN.md` (look up a specific section,
do not read the whole thing).

- **Rendering a commit timestamp for a human always goes through
  `sg_date_format_normal`** (`include/sg/date.h`, Phase 54) -- git's
  DATE_NORMAL, the `Date:` line of `git log` and `%ad`. It returns the whole
  field including the offset, so no caller assembles half of it.
  WARNING: **the clock shown is the epoch SHIFTED INTO the stored offset**,
  not UTC and not the machine's local time. `sg log` used to render UTC while
  printing the stored `+0800` beside it, so every date it ever showed was
  wrong by the offset -- eight hours here -- and contradicted itself in its
  own output. Nothing caught it because nothing had ever compared `sg log`
  to `git log`; the pre-existing checks assert exit codes or scrape
  `head -1` for the sha.
  WARNING: **the day of month is NOT zero-padded** (`Jan 1`, not `Jan 01`),
  and the weekday/month names come from git's own hard-coded English tables,
  **not `strftime`'s `%a`/`%b`** -- those follow the locale, and one
  `setlocale` call anywhere in the process would silently translate a format
  whose entire job is to match git byte for byte.
  WARNING: **a stored offset of exactly `-0000` is normalized to `+0000`**
  (Phase 60d, `normalize_tz_for_display` in `date.c`, shared by
  `sg_date_format_normal`/`_rfc2822`/`_iso` so the three can't drift apart
  on it) -- every other value, including ordinary `+0000` and any non-zero
  offset, is echoed unchanged. `--pretty=raw` and `sg cat-file -p` do NOT
  go through this or any rendering function -- they print the object's own
  stored bytes directly, so a stored `-0000` stays `-0000` there, matching
  real git measured the same way. `%aI` needs no such rule: it already
  collapses BOTH `+0000` and `-0000` to a literal `"Z"` before reaching a
  tz-string branch at all.
  WARNING: **`cmd_undo.c` has its own date formatter and must not be
  converged onto this one.** `sg undo` has no real-git counterpart, so it has
  no oracle; it deliberately prints local time in ISO form.
  WARNING: **`sg log`'s oracle is `git log --first-parent`**, because sg's
  walk is first-parent-only by Phase 2 scope -- against a full `git log` the
  two legitimately visit different commit SETS. Interop pins the rendering
  against `--first-parent` AND pins the scope boundary separately (sg's
  output must NOT equal a full walk), so teaching sg to walk every parent
  turns a check red by name instead of quietly changing what the rendering
  checks compare. Four git config knobs were measured to move that oracle
  (`log.decorate`, `core.abbrev`, `log.date`, `format.pretty` /
  `log.abbrevCommit`) and are pinned on the command line, with a
  precondition check proving the pins beat a hostile config.
  WARNING: **an empty commit message prints no message block at all**, not
  even the leading blank line, and entries are separated by one blank line
  with **none after the last** -- so the separator goes BEFORE every entry
  but the first. An empty-message entry in the middle is what distinguishes
  the two models. Abbreviations are hard-coded to 7 (the `Merge:` line and
  `--oneline`) while git's default `core.abbrev=auto` scales with object
  count; interop declares `core.abbrev=7` on git's side rather than
  pretending the two policies agree.
- **`--date=relative` / `relative-local` and the `%ar` / `%cr` placeholders**
  (Phase 66, `sg_date_format_relative` + `sg_date_now` in `src/util/date.c`).
  The algorithm was not derived, it was **verified**: re-implemented
  independently in Python from bisected boundaries and cross-checked against
  real git over 1239 probes, 0 mismatches, and a cold review then repeated
  the exercise with its own 176-probe harness and also got 0 -- the same
  "two independent ports agreeing" bar `similarity.c` is held to.
  WARNING: **there are TWO different month formulas and using one of them
  everywhere is wrong.** Below 365 days the count is `(days + 15) / 30`;
  from 365 days on it is `(days * 12 * 2 + 365) / (365 * 2)`. A draft using
  the second one everywhere matched git on **538 of 539** probes -- the one
  failure was 75 days, where it says 2 months and git says 3. One miss in
  539 was the entire signal, and a smaller probe set would have called that
  draft correct. `tests/test_date_relative.c` carries that delta (6476307)
  as a named witness; it is the ONLY row in the suite that can catch it.
  WARNING: **each threshold is compared against the CONVERTED value, not the
  seconds.** The minutes branch ends when the computed MINUTES reach 90
  (delta 5370), not when the seconds reach 5400. Boundaries, each bisected
  to the exact second and pinned +/-1: 90, 5370, 127770, 1164570, 6002970,
  31490970, and 32873370 (`1 year ago` -> `1 year, 1 month ago`).
  WARNING: **a commit in the FUTURE renders the literal `in the future`** --
  no number, no "ago". This had a unit test pinning sg's own literal but
  **no interop witness at all** until a mutation found it: changing the
  string left `interop` fully green while four unit rows went red. It is now
  compared against git.
  WARNING: **`relative-local` is byte-identical to `relative` in every
  zone** -- the answer is a duration, so the tz has nothing to change. It
  still has to parse. Its dispatch happens BEFORE `resolve_mode_tz`, and
  that ordering is load-bearing: moving it after makes `relative-local`
  return a silently EMPTY field for an out-of-range timestamp instead of a
  duration, which is pinned by its own named check (the first draft of that
  check used `[ -s file ]` and verified nothing, because an empty date field
  still emits a trailing newline).
  **The clock is injectable through `GIT_TEST_DATE_NOW`**, the same name git
  uses, so one interop line sets it for both tools; it reaches `%ar`/`%cr`
  as well as `--date=relative`. sg parses it as a strict decimal integer and
  treats anything malformed as ABSENT. **Do NOT reproduce git's own parsing
  of a bad value** (measured: `abc` and `""` become 0, `1700000000x` takes
  the numeric prefix, `-5` wraps to `584942417301 years ago`) -- that is
  unsigned wraparound in a test hook, not an interface.
  WARNING: **an out-of-range stored timestamp diverges in EVERY date
  format, and this predates Phase 66** -- measured on the `99999999999999999999`
  fixture: git parses it as 0 and renders the epoch (`1970-01-01`,
  `54 years ago`), while sg's `strtoll` saturates to `LLONG_MAX` and renders
  an empty field for most formats, the raw number for `raw`/`unix`, and
  `in the future` for `relative`. That is why the interop checks on that
  fixture assert "does not crash", not byte equality -- a `cmp` there would
  fail permanently for a reason unrelated to whatever is being tested.
  WARNING: **`sg_date_now()` samples the clock afresh at every call site and
  this is UNMEASURED** -- both against real git's own behaviour and against
  this project's gates, since every interop check pins `GIT_TEST_DATE_NOW`
  and so never exercises the real-clock path at all. Recorded honestly in
  `docs/DESIGN.md` rather than defended.
- **`--date=human` / `human-local` and the `%ah` / `%ch` placeholders**
  (Phase 67, `sg_date_format_human` in `src/util/date.c`). Unlike
  `relative`, this is a CALENDAR comparison, not a duration: it renders the
  commit in its own render offset (`tz`, or the local offset at the
  commit's instant for `human-local`, same as every other `-local` mode)
  and compares its calendar date against "now" in the MACHINE'S LOCAL
  zone -- never the stored tz, never UTC. Re-implemented independently in
  Python and cross-checked against real git 2.55.0 over 5780 probes (0
  mismatches; 520+520 boundary probes plus 1580 x 3 seeded random probes),
  the same "two independent ports agreeing" bar `similarity.c` and Phase
  66's own relative algorithm are held to. There are exactly four output
  shapes: same local calendar day -> the existing `--date=relative` string
  verbatim (including `in the future`); same year+month with
  `mday < now.mday < mday+5` -> `Www HH:MM` (`+HHMM` appended when
  `!local_mode` and the render offset differs from "now"'s local offset);
  same year, anything else (including a future commit past that window,
  in EITHER direction) -> `Www Mmm D HH:MM`, no year, no offset ever;
  different year (either direction) -> `Mmm D YYYY`, no weekday, no time,
  no offset ever.
  WARNING: **the "today" test is the CALENDAR DAY, not a small delta**:
  a commit at 00:01 viewed at 23:59 the SAME day (23h58m apart) renders
  `24 hours ago`; a commit at 23:59 viewed 2 minutes later, past midnight,
  renders `Tue 23:59`. A "< 24h means relative" rule gets this backwards.
  WARNING: **the 5-day window is `mday` arithmetic INSIDE ONE MONTH, and
  the comparison is `>`, not `>=`**: commit Oct 30 viewed Oct 31 renders
  `Mon 12:00`; the very next calendar day, Nov 1, renders
  `Mon Oct 30 12:00` -- purely because the MONTH rolled over, not because
  the day-count grew. A "days apart < 5" rule gets both of these backwards.
  WARNING: **a future commit is NOT `in the future` unless it is the SAME
  calendar day** -- one day ahead renders the full `Www Mmm D HH:MM`, a
  future year renders `Mmm D YYYY`. There is no "window" shape for a
  future commit; that branch only fires when the commit's day-of-month is
  LESS than now's.
  WARNING: **the offset suffix appears ONLY on the `Www HH:MM` shape, and
  `human-local` NEVER prints it at all** -- not even across a DST change
  inside the 5-day window, which is the one case where "render offset
  differs from now's local offset" would otherwise be true (measured:
  1080 probes across 8 zones, zero offsets). `human-local`'s own "-local"
  rule (the commit rendered in the LOCAL offset at ITS OWN instant, same
  as every other `-local` mode) is unaffected; only the suffix decision is
  suppressed.
  WARNING: **the offset suffix ECHOES the stored `+HHMM` string, and the
  decision to print it compares that string against the local one -- not
  either offset's length in seconds.** Both halves were wrong in Phase
  67's first implementation and both were found by a cold review after all
  four gates, the 390-probe oracle harness and a 9108-probe sg-vs-git
  sweep were green, because every one of those inputs used a CANONICAL
  offset, where the two rules agree by construction. A commit object may
  legally store a minute field >= 60: measured, a stored `+0165` renders
  as `+0165` in git and as `+0205` in a version that re-derives it from
  seconds -- and sg's OWN `--date=iso` already echoed `+0165` for the same
  object, so that version disagreed with sg as well as with git. The
  comparison is the same story: a stored `+0060` against a local `+0100`
  is the same 3600 seconds, and git prints the suffix anyway. The `-0000`
  -> `+0000` normalization applies here exactly as it does in every other
  renderer (`normalize_tz_for_display`), which is why the stored string is
  taken through it rather than used raw.
  WARNING: **"now" must be shifted by the EXACT local offset in SECONDS,
  never through a `+HHMM` string.** Every other renderer in `date.c` may
  round-trip an offset through that string because it only affects which
  digits are printed; human's does not, because the offset feeds a
  CALENDAR-DAY comparison, so truncating the seconds of a zone whose
  offset is not a whole number of minutes flips the whole output SHAPE.
  Measured in the 30 seconds before local midnight at `-0044:30`: git
  `12 hours ago`, sg (before the fix) `Mon 12:44 +0000`. This is why
  `shift_tm` was split into `shift_tm_offset` -- human is the one caller
  that must not go through the string form.
  WARNING: **a non-whole-minute local offset needs NO zoneinfo database**:
  the POSIX TZ string `TZ=XXX0:44:30` is a -2670s offset on any POSIX
  system (and `TZ=XXX-1` is `+0100`), verified in both tools. The Phase 67
  review checks use those rather than `Africa/Monrovia`, so unlike
  divergence #7's own group they can never degrade into a silent `skip`.
  WARNING: **`%ah`/`%ch` are FIXED formats, unaffected by `--date=`**, same
  as `%ar`/`%cr` -- measured with `--date=raw`/`--date=iso`/
  `--date=human-local`, `%ah` stays the non-local human form in all three.
  They render through their own `print_human_date_field` in
  `commit_out.c`, not through `print_configured_date_field` (the
  `--date=`-driven path %ad/%cd use).
  WARNING: **`human-local` inherits deliberate divergence #7** (see
  `CLAUDE.md`'s deliberate-divergence list):
  in a zone whose UTC offset is not a whole number of minutes, sg's
  whole-minute `-local` shift renders a different clock from git's
  `localtime_r` -- measured on a hand-crafted 1970-06-01 commit (an
  ordinary `git commit` cannot store a pre-1973 author date at all; git's
  own date parser rejects a bare epoch that small) under
  `TZ=Africa/Monrovia` (offset `-2670`s, not a whole minute, until
  1972-01-07): git `Mon 11:15`, sg `Mon 11:16`. Plain (non-local) `human`
  does NOT diverge there (measured: `Mon 12:00 +0000` on both) -- and the
  REASON matters, because that sentence was true of the measured point
  while being false in general until Phase 67's review round: the clock
  `human` prints comes from the STORED offset, so the local zone reaches
  it only through the calendar-day comparison, and that comparison uses
  the exact offset in seconds. Truncate it and plain `human` diverges too,
  in the 30 seconds before local midnight, by a whole SHAPE.
- **`--date=<format>` selects how a timestamp is RENDERED, and is shared
  verbatim by `sg log` and `sg show`** (Phase 64, `sg_date_parse_mode` /
  `sg_date_format_mode` / `sg_date_format_mode_alloc` in
  `include/sg/date.h`). Both `--date=<v>` and the separate-argument
  `--date <v>` are accepted, last one wins, and the names are
  **case-sensitive** (`Short`, `ISO`, `DEFAULT` are all errors; git exits
  128, sg exits 1 per this project's 0-or-1 convention, pinned on both
  sides). Supported: `default`, `local`/`default-local`, `iso`/`iso8601`,
  `iso-strict`/`iso8601-strict`, `rfc`/`rfc2822`, `short`, `raw`, `unix`,
  `format:<strftime>`, `format-local:<strftime>`, plus a `-local` suffix on
  any of those base names.
  **`relative`/`relative-local` are implemented as of Phase 66, and
  `human`/`human-local` as of Phase 67** (see the dedicated bullets below),
  gone from this list rather than marked "fixed" in place. **Still
  deliberately not implemented: `auto:<name>`** -- rejected, never
  approximated, because its meaning depends on `isatty(1)` (measured:
  piped, `auto:relative` renders as `default`), so it has no oracle
  without a pty.
  WARNING: **`--date=` reaches exactly FOUR places, and the fourth is the
  one a careless probe misses**: medium's `Date:` and fuller's
  `AuthorDate:`/`CommitDate:`; the **annotated tag header's own `Date:`**
  (a SEPARATE call site in `cmd_show.c`, not reached through anything in
  `commit_out.c`); `%ad`/`%cd`; and **`--pretty=reference`'s date field**.
  That last one was recorded as UNAFFECTED in this phase's own spec,
  because the probe that established it used `--date=short` -- which is
  exactly what `reference` already renders. A control whose two arms agree
  by construction proves nothing; re-measured with `unix`/`raw`/`format:`,
  it moves. Genuine negatives, each re-measured with a non-coinciding
  name: `%ai`/`%aI`/`%aD`/`%as`/`%at` and the committer mirrors are fixed
  formats, `--pretty=raw` prints the object's stored bytes, and
  `full`/`short`/`oneline` have no date line at all.
  WARNING: **`-local` takes the machine's offset AT THE COMMIT'S OWN
  INSTANT**, never a cached one and never "now" -- measured across a DST
  boundary (`TZ=America/New_York`: a January commit renders `-0500`, a July
  one `-0400`). The stored tz is ignored entirely by every `-local` name.
  `local`/`default-local` additionally print **no offset field at all**,
  the one shape none of the five original renderers can produce.
  WARNING: **`format:`/`format-local:` do NOT hand the string to
  `strftime` unchanged.** `%s`, `%z` and `%Z` are substituted first (a
  hand-built `struct tm` cannot carry them portably), and `%Z` is **empty**
  for the non-local form. `%%` is its own two-byte token, NOT a shield for
  a following `%z`: `%%z` -> `%z`, but `%z%%z` -> `+0000%z`.
  WARNING: **the output buffer must GROW; a fixed one is the wrong tool
  and this was got wrong TWICE in one phase.** The format string is
  unbounded user input. Round 1 used a 1024-byte stack buffer at all five
  render call sites and turned "does not fit" into an **empty date field
  with exit 0** (measured: 1010 bytes agreed with git, 1200 rendered
  nothing). Round 2 replaced it with a growing buffer but left **two
  independent caps** -- an outer `1<<24` and `strftime_grow`'s own
  `1<<20` -- and since the inner one is the real bound, the identical
  silent-empty bug reappeared at exactly 1048584 bytes of output, reachable
  with an 87 KB argv. There is now ONE cap, `1<<28`, and it is justified by
  measurement rather than chosen: the largest `strftime` expansion under
  `LC_ALL=C` is 14x (`%+`, 2 bytes in / 28 out; `%c` is next at 24), and
  `ARG_MAX` here is 1 MiB (Linux caps a single argv string at 128 KiB), so
  the largest output reachable through the CLI is about 14 MiB, roughly
  19x below the cap. **Do not "simplify" this back toward a constant-size
  buffer, and do not add a second cap.**
