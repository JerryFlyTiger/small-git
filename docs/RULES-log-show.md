# Rules: `sg log`, `sg show`, and commit-entry rendering

Scope: `src/cli/cmd_log.c`, `src/cli/cmd_show.c`, `src/cli/commit_out.c`, `src/cli/log_graph.c`, `include/sg/commit_out.h` -- entry layout, `--pretty`/`--format`, `--graph`, message rendering.

**Read this file before editing anything in that scope.** It is not
background reading: every rule here was measured against real git 2.55.0,
and most of the `WARNING:`s exist because something slipped past a fully
green board. Project-wide rules -- the gates, the code conventions, the
deliberate divergences -- stay in `CLAUDE.md`; the full derivations and
measurement tables stay in `docs/DESIGN.md` (look up a specific section,
do not read the whole thing).

- **`sg show` exists as of Phase 55** and shares `sg log`'s entry renderer:
  `git show <non-merge-commit>` is byte-identical to `git log -1 -p` of it in
  every flag combination (measured), so the renderer lives in
  `src/cli/commit_out.c` (`sg_commit_out_entry`) and **neither command owns a
  copy**. Phase 54's `phase54` interop group is what proves a change to it
  did not move `sg log`. **`--pretty`/`--format` (Phase 60a) are also shared
  this way** -- see the dedicated `--pretty=<name>` bullet under `sg log`
  below for the grammar, the seven builtins and the separator model; both
  commands go through the exact same `sg_pretty_parse` + `sg_commit_out_entry`.
  WARNING: **`--oneline` changes an ANNOTATED TAG's header, not just the
  commit's** (Phase 59, measured): it drops the `Tagger:` and `Date:` lines
  entirely, and drops the blank line between the tag message and the commit
  line below it. Without `--oneline` all three are printed, and that half was
  always right -- the suppression is `--oneline`-only, so a fix that removes
  those lines unconditionally passes the new checks and breaks the old ones.
  This was a PRE-EXISTING bug from Phase 55a, which pinned 50 `sg show`
  combinations byte-for-byte and simply never combined `--oneline` with an
  annotated tag; it surfaced in Phase 59 only because a new fixture put a tag
  on an octopus. Interop pins the three `--oneline` forms plus a negative
  control asserting `Tagger:` survives without it.
  WARNING: **the separator between several objects is STATEFUL, and a blob
  is the exception**: everything else takes a leading blank line when
  something was already shown or when it is a tag's target, while a blob
  neither takes one nor arms the flag for the next object. Measured over
  every ordered pair. A rule of "a tree never separates" holds only when the
  tree comes first -- that was my spec's error, caught by the implementer.
  WARNING: **only COMMITS are deduplicated across the argument list.**
  `show <c> <c>` prints once, and so does an annotated plus a lightweight tag
  of the same commit; but `show <tree> <tree>` prints twice, and
  `show <tag> <tag>` prints the tag header twice with one commit under it.
  WARNING: **`-s`/`-p`/`--stat` are LAST ONE WINS**: `-s` clears what came
  before, `-p` and `--stat` set their own bits and accumulate, default is a
  patch. `-s -p` prints a patch, `-p -s` prints nothing, `--stat -p` prints
  BOTH (all measured). Giving `-s` a fixed priority passes every single-flag
  case and gets `-s -p` backwards -- same shape as `-M`/`-C` and `-c`/`--cc`.
  WARNING: **a merge is TWO row sets in one command** (Phase 55b). The dense
  patch includes a path iff the result differs from **every** parent, with
  the **MODE** compared as well as the id -- a file whose blob id equals
  theirs' exactly is still included when only its mode differs, and renders
  as a `mode a,b..c` line with no hunks. `--stat` is a completely different
  set: measured byte-identical to `git diff --stat <parent1> <merge>`, so it
  includes paths the dense rule excludes, and it stays the first-parent diff
  at ANY parent count. Computing one set and rendering it both ways is wrong
  for whichever it was not written for; interop names both halves.
  WARNING: **a merge opens its diff section with a blank line even when the
  dense set is EMPTY** (measured on a clean merge and a clean octopus: header,
  blank, nothing else), where an ordinary commit with an empty diff prints no
  blank at all. Returning early on "no rows" gets that wrong AND skips
  `--stat`. The stat->patch blank is printed only when the patch itself will
  produce output, and a merge prints **no `---` line** with `-p --stat` where
  an ordinary commit does.
  WARNING: **`sg_diff_entry.combined_row` exists because the ours/theirs/
  result predicate cannot express these rows** -- a path added by the merge
  has both parents ABSENT, one deleted by it has result ABSENT with nothing
  unmerged. **Do not loosen `sg_diff_entry_is_combined` instead**; its
  asymmetry is deliberate, measured and pinned (see `docs/RULES-diff.md`'s
  combined-diff notes). Adding that field required auditing every manual
  construction site per the shared-struct rule.
  WARNING: **an octopus is refused only when it actually needs rendering** --
  `> 2` parents, a non-empty dense set, AND a request that actually renders
  the dense PATCH. **A clean octopus prints header plus the opening blank
  line, byte-identical to git, and its `--stat` works like any other -- and
  so does a NON-clean octopus's `--stat` (or `-s`)**, which was a real bug
  from Phase 55b until Phase 59 round 2: the refusal used to fire on ANY
  output request at all for a `> 2`-parent commit with a non-empty dense
  set, not just a request for the dense patch itself, so `sg show --stat
  <non-clean octopus>` and `sg show -s <non-clean octopus>` were both
  wrongly refused even though neither ever touches the dense set (`--stat`
  is a first-parent diff at any parent count, `-s` prints no diff at all --
  this file said so two lines up and got the octopus case wrong right next
  to saying it). The guard is now `commit.parent_count > 2 && o.patch &&
  !(o.name_only || o.name_status)`, in both `render_id`'s direct check and
  `target_is_merge`'s tag-lookahead mirror of it (`cmd_show.c`). This bug
  was unreachable by any fixture an ordinary `sg merge`/`git merge` could
  build -- a real octopus merge tool refuses outright on the same conflict
  such a fixture needs -- so it stayed invisible until a `commit-tree`-built
  fixture (three independent single-parent branches, a hand-picked result
  tree differing from all three) made the non-clean-octopus shape
  constructible at all; see Phase 59's DESIGN.md entry for the fixture.
  **A fixture for the dense path must use a merge that CONFLICTED
  and was resolved**: a clean merge's dense set is empty, and this group's
  precondition caught exactly that mistake.
  WARNING: **`--name-only`/`--name-status` exist as of Phase 59, and the
  five-flag model is NOT "`-s` clears, the other four OR together"** -- that
  prose is one bit too coarse, measured directly against real git 2.55.0
  over 16 combinations: `-p`/`--stat` clear `no_output` when they fire
  (an explicit format request cancels "no output"), `--name-only`/
  `--name-status` do NOT. Only with that detail does one model explain both
  `-s --name-only` (ERROR: `no_output` and `name_only` both survive) and
  `-s -p --name-only` (prints names, no error: `-p` cleared `no_output`
  before `--name-only` ever set its own bit). `--name-only`/`--name-status`
  suppress PATCH/STAT entirely, in either order, when either is active.
  WARNING: **the octopus refusal above does NOT apply to `--name-only`/
  `--name-status`** -- they carry one status letter PER PARENT instead of
  being fixed at two sides (`MMM` for three parents, not `MM`), so a
  non-empty dense set is answered, never refused, at any parent count. The
  2-parent case reuses `sg_diff_combined_from_trees`'s `out` list plus
  `sg_diff_print`; parent counts other than 2 go through a SEPARATE,
  self-contained union-walk (`render_octopus_names`, `cmd_show.c`) that
  duplicates (does not extend) that function's "differs from every parent"
  rule, because `out` is only ever populated at `parent_count == 2` and this
  phase's budget excluded `workdir/diff.c`.
  WARNING: **`diff_out.c`'s `print_name_status` used to hardcode the literal
  `"MM"` for every combinable row, and that was CORRECT until Phase 59** --
  neither pre-existing producer (a live conflict; Phase 40's rev-mode pass)
  can ever have an ABSENT `ours`/`theirs`, so neither letter could be
  anything but `M`. A merge commit's own dense row (`combined_row`) is the
  first producer whose sides genuinely vary, so its letters are now computed
  (`combined_letter`: ABSENT parent -> `A`, present parent but ABSENT
  result -> `D`, else `M`) -- **but only for `combined_row` rows**. Measured
  against real git (both a modify/modify and an add/add unresolved
  conflict, `-c` and `--cc`): git still prints literal `MM` even when the
  result is DELETED, a shape the computed rule would answer `DD` for.
  Applying the computed rule to the two pre-existing producers is a silent
  regression no existing check would have caught; it is pinned by name in
  `tests/interop.sh`'s `phase59:` group specifically because it was
  measured and written down BEFORE the code, not found after.
  WARNING: **suppressing PATCH/STAT when a name format is active belongs in
  the RENDERER, not in `cmd_show.c`'s flag resolver** -- a directed mutation
  (`tests/mutate.sh`) neutralizing a `resolve_commit_out_opts` line that
  zeroed `o->patch`/`o->stat` left `make test` fully green, because both
  render paths (`commit_out.c`'s `print_commit_diff`,
  `cmd_show.c`'s `render_merge_diff`) already check `o->name_only`/
  `o->name_status` FIRST and dispatch the name format regardless of
  `o->patch`/`o->stat`'s value -- a redundant guard one layer above the
  real one (CLAUDE.md's own three-way mutation classification: this was
  neither a blind spot nor unobservable, the defense line was just one
  layer down). The resolver now copies the five flags through
  unconditionally; do not re-add the zeroing.
  WARNING: **that same "patch/stat may be nonzero underneath a name
  format" fact bit a SECOND place, found by an external 159-probe oracle,
  not by this project's own gates.** `commit_out.c`'s `---`-vs-blank-line
  separator rule was still reading `o->stat && o->patch` alone (Phase 54's
  original rule, from before name formats existed) to decide the
  separator, so `sg show -p --name-only --stat` printed a `---` line where
  git prints a blank one -- `make test`, `bash tests/interop.sh` and
  `make sanitize` were all green for this, because no existing fixture
  combined a name format with BOTH `-p` and `--stat` at once. The rule is
  now `o->stat && o->patch && !o->name_only && !o->name_status`: NAME
  wins the separator decision the same way it already wins which
  `sg_diff_print` format gets called two lines below it.
- **git EXPANDS TABS in a commit message body, and sg must too** (Phase 55b,
  `print_message_line` in `src/cli/commit_out.c`). `--expand-tabs=8` is the
  medium format's default and **only** the medium format's: `--oneline` and
  `%s` leave the tab alone (measured). The column is counted from the start
  of the **message line**, NOT from the indented output column -- a line of
  two tabs lands at output column 20, i.e. 16 expanded columns plus the
  four-space indent.
  WARNING: **this was a pre-existing `sg log` bug that Phase 54 missed while
  pinning `sg log` byte-for-byte**, because no fixture had ever put a tab in
  a commit message. It surfaced only because `git merge`'s own auto-generated
  conflict message contains `#\tboth.txt`. A shape the fixtures cannot
  produce is untested however many checks run over them.
  WARNING: **tree entry names are printed RAW here, and that matches git.**
  Measured on a crafted entry name containing an ESC byte: `git show <tree>`
  prints it raw with `core.quotepath` at its default, while
  `git cat-file -p <tree>` on the same tree C-quotes it. git is inconsistent
  between its own two commands and sg matches git in both -- do not "fix"
  `cmd_show.c` to quote by analogy with `cmd_cat_file.c`.
- **`sg log` takes `-n <count>` / `-<count>` / `--max-count=`, `--oneline`,
  `-p`/`--patch`, `--stat`, a single `<rev>`, and (Phase 62) `[--] [<path>...]`**
  (Phase 54). `-p`/`--stat` are reuse -- the commit's first-parent tree
  against its own, through `sg_diff_trees` + `sg_diff_print` -- so what
  needed measuring was where the diff sits inside the entry, not the diff.
  WARNING: **the `---` line appears ONLY when `-p` and `--stat` are both
  on.** With `-p` alone git introduces the diff with a blank line, so a rule
  that always printed `---` passes the combined check and fails the plain
  one; interop pins the negative separately. `--oneline` introduces its diff
  with **nothing**, and never prints `---`.
  WARNING: **an EMPTY diff prints no separator at all** -- no blank line, no
  `---`. The separators belong to the diff, not to the entry, and an empty
  commit in a fixture is what makes the difference observable.
  WARNING: **a merge DOES get a diff, against parent 1**, because sg's walk
  is first-parent-only and that is what `git log --first-parent -p` does
  (measured; plain `git log -p` prints nothing for a merge). This is not an
  independent choice, it falls out of the scope boundary above.
  WARNING: **rename detection is ON** at `SG_SIMILARITY_DEFAULT`, because
  git's `diff.renames` has defaulted to true since 2.9 and `git log -p`
  prints `rename from`/`rename to`. The interop fixture carries a
  rename-with-an-edit plus a precondition asserting git itself calls it a
  rename.
  WARNING: **`-n 0` is a legal request for nothing**, not an error and not
  "unlimited": git prints nothing and exits 0.
  **Deliberately not implemented: `--follow`,
  `--author=`/`--grep=`, `--reverse`, `--all`, `-c`/`--cc`,
  `--full-history`, `--simplify-merges`, and a second `<rev>`.** All are
  rejected with the usage line and exit 1, never silently ignored.
  **`--date=<format>` is implemented as of Phase 64** (see the dedicated
  bullet below) -- gone from this list rather than marked "fixed" in
  place, same convention `--pretty` and `-- <pathspec>` follow here.
  **`--pretty`/`--format` are implemented as of Phase 60a** (see the
  dedicated bullet below) -- this list used to include them, they are gone
  from it now that they exist rather than marked "fixed" in place, same
  convention this file uses everywhere else for a closed gap.
  **`-- <pathspec>` is implemented as of Phase 62.** This used to say
  "path-limited history is git's history SIMPLIFICATION, not a filter over
  the same walk" and reject it for the same reason `--patience` is
  rejected -- that claim is **measured false under sg's own first-parent-
  only walk** (Phase 2 scope): `git log --first-parent -- <path>` IS
  exactly a filter over that restricted walk (print commit C iff C's tree
  differs from its FIRST parent's tree, intersected with the pathspec), so
  it can be implemented as `sg_commit_out_touches_pathspec` deciding
  per-commit whether to print, with no change to the walk itself. **This
  equivalence holds only because the walk is first-parent-only** -- if sg
  ever grows a full, every-parent walk, this needs to be re-measured before
  trusting it again; a full walk's own path-limiting is genuinely git's
  history-simplification machinery (parent-relinking included), not a bare
  filter. `sg_pathspec_add`/`sg_pathspec_matches`/`sg_diff_list_filter` are
  reused as-is, same three-clause matcher as `sg diff` (Phase 28) -- no
  second matcher was written. Two known-since-Phase-29 rules bit this new
  call site exactly as documented there: filtering must run BEFORE rename
  detection (measured: `sg log -p -- a.txt`, at the commit that renamed
  `a.txt` to `renamed.txt`, prints a plain `deleted file mode`, not
  `rename from`/`rename to`, because the ADD half of the pair was already
  filtered out), and a mode-only change (`chmod +x`, no content change)
  counts as touching the path -- `sg_diff_trees` already emits a row for
  it, so the selection judgment is built on top of that existing row rather
  than a from-scratch tree comparison. WARNING: **`shown++` (the `-n`
  counter) had to move to AFTER the selection judgment** -- `-n 2 --
  a.txt` counts the second commit that actually touches `a.txt`, not the
  second commit the walk visits (measured); and the entry-to-entry blank
  line now depends on "was the PREVIOUS commit actually printed", not "was
  this the first commit the walk reached" -- a filtered-out commit must
  leave behind no separator. WARNING: **pathspec magic (`:(icase)`, `:!`,
  `:/`) and `--follow` are both accepted by git and both REFUSED by sg**
  (`--follow` genuinely walks across a rename, which sg's pathspec filter
  does not attempt) -- pinned as real, named divergences in interop, not
  silently approximated.
  WARNING: **`sg_commit_out_touches_pathspec` returns THREE values, and
  collapsing -2 into -1 is a real bug, not a simplification** (found by
  review after a fully green board, fixed in the same phase). Only -2
  fills `bad_path` (`sg_diff_trees`'s own third state, an unsafe tree
  entry name); an ordinary -1 -- a missing or corrupt object -- leaves the
  buffer untouched, so a caller that reports one message for both prints
  `sg: path  is invalid` with an EMPTY path and blames a path for
  something no path caused. That is strictly worse than the pre-Phase-62
  behaviour, where `sg log -p` on the same history printed a generic
  render failure. **And `bad_path` must go through
  `sg_quote_path_delimited`** (`cmd_diff.c`'s `report_bad_tree_path` had
  always done so; the new call site was written from the message text
  rather than from that function, and lost the quoting on the way) -- an
  entry name unsafe enough to reach here is exactly the kind that carries
  a raw ESC byte. Both halves are pinned by their own named interop
  checks, built with `git mktree`/`commit-tree` the same way the phase26
  group builds its `..` fixture; the -1 half additionally deletes a tree
  object, since no porcelain will produce that shape either.
  **`--graph` is implemented as of Phase 63.** Under sg's first-parent-only
  walk the graph column is a per-line prefix, not a real graph layout --
  `"* "` on an entry's first line, otherwise `"| "` (WITH a trailing space,
  not a bare `"|"` -- a terminal or `sed` hides that byte, verify with
  `repr()`/`od -c`, never by eye). No multi-column characters (`|\`, `|/`,
  `* |`) are ever produced and there is no code path for them: a
  first-parent walk cannot branch, so the shape they exist for cannot
  occur, and there is no real-git output to check such code against if it
  existed.
  WARNING: **the LAST printed entry's continuation lines are `"  "` (two
  spaces, same width as `"| "`) instead, and the predicate for this is
  "did the walk end NATURALLY" (reached the true root with no `-n` cutoff
  and no error), never "does this particular commit have a parent"**
  (measured: a first draft of this predicate was `parent_count == 0` on
  the LAST commit, and that is also wrong). The decisive fixture is a
  pathspec that filters out everything between the last matching commit
  and the root: `--graph -- deep` against a fixture where `deep` matches
  exactly one non-root commit with a real parent -- the walk continues
  past it, unprinted, all the way to the root before stopping naturally,
  and that commit still gets `"  "`. Meanwhile `-n 1` (no pathspec) prints
  a commit that also has a parent, but the walk was cut off, and it gets
  `"| "`. `--oneline`/`format:`/`tformat:`/`reference` have no continuation
  lines at all and cannot distinguish the two -- a test using one of them
  verifies nothing about this rule; use a multi-line format (medium/
  fuller/full/raw).
  WARNING: **the `format:`/`medium` inter-entry separator difference (a
  `format:` pair has no empty graph line between them, `medium` does) is
  the SAME rule seen from two angles, not two rules.** A `medium` entry
  terminates itself with `\n`, so the separator `\n` a caller feeds between
  two entries lands at the START of a new line and opens one for the
  prefixer to fill; a `format:` entry does NOT terminate itself, so the
  identical separator byte lands MID-LINE and merely ends the line already
  in progress, consuming no prefix at all. This falls out of the entry's
  own trailing byte and needs no branch keyed on format kind anywhere in
  the prefixer.
  WARNING: **the capture mechanism is `tmpfile()`, not `open_memstream()`
  -- `fileno()` on a memstream `FILE *` is not guaranteed to return a
  valid fd (measured: `-1` on this project's own macOS dev machine), so it
  cannot be `dup2`'d onto fd 1.** A pipe was also considered and rejected:
  nothing reads its write end while the captured call is running, so an
  entry whose output exceeds the pipe buffer (commonly 64 KB, an easy
  bar for a large `-p` diff) would deadlock, intermittently and by data
  size -- the worst kind of failure to debug. See Phase 63 of
  `docs/DESIGN.md` for the full design, including why capturing fd 1
  (rather than threading a `FILE *`/prefix parameter into
  `commit_out.c`/`diff_out.c`) was chosen deliberately: `diff_out.c` alone
  has six output formats plus the combined-diff renderer, and a missed
  `printf` call site while threading a parameter through would be
  invisible without a fixture that happens to combine `--graph` with
  exactly that format.
  WARNING: **`sg show --graph` is refused** -- git also refuses it (exit
  128, "options '--no-walk' and '--graph' cannot be used together"), and
  sg's existing unknown-flag handling in `cmd_show.c` already refuses it
  (usage, exit 1) with zero code changes needed; this is pinned as a
  head-on pair in interop rather than left as an unverified assumption.
  WARNING: **an entry whose EXPANDED bytes are empty must still emit its
  own `"* "` marker** (found by review, fixed in the same phase): the
  per-byte prefixer loop is a no-op for a zero-length write by design (it
  must not invent a prefix for a legitimately empty write), but that also
  meant an entry with genuinely nothing captured -- `--pretty=format:%b`
  on a body-less commit is the real trigger -- never got a marker either,
  since the marker used to be written lazily, attached to the entry's own
  first byte. A MIDDLE such entry's marker was silently absorbed by the
  FOLLOWING entry's separator write one entry late, which is why this was
  invisible except on the very LAST entry of a run (nothing left to
  absorb it into): 11 body-less commits under `--graph
  --pretty=format:%b` printed 10 markers, not 11, plus an extra trailing
  `\n` real git does not have. `$P62`/`$P62M` could not have caught this
  -- every commit in both is a one-line-subject, no-body commit, so `%b`
  is empty for ALL of them, which hides a "middle entry" bug behind the
  "last entry" one; telling the two apart needs a fixture with a genuine
  MIX of body-less and with-body commits. `sg_log_graph_write_entry`
  (`log_graph.h`/`log_graph.c`) is now the ONLY function allowed to
  combine `sg_log_graph_begin_entry` with writing an entry's bytes, for
  exactly this reason -- the bug shipped through a call site that did the
  two separately.
  WARNING: **the SAME review also reproduced a bug in `--pretty=tformat:`
  that PREDATES this phase (Phase 60a) and has NOTHING to do with
  `--graph`**: an EMPTY format string (`--pretty=tformat:`, nothing after
  the colon) must print ZERO bytes total, not even the terminator --
  measured, real git prints nothing at all; sg printed one bare `\n` per
  commit. The predicate is "is the FORMAT STRING itself empty", not "did
  expansion produce zero bytes" -- do not conflate this with the `--graph`
  bug above: `tformat:%b` on a body-less commit expands to zero bytes too
  and STILL correctly gets its terminator, because its format string
  (`"%b"`) is not empty. `format:` (non-`t`) needed no equivalent fix, an
  empty `format:` entry was already correct. Both fixes compose: `--graph
  --pretty=tformat:` on a mixed fixture produces bare markers with no
  separating byte at all (`b'* * * '`), which is itself a named interop
  check rather than an assumption.
- **`--pretty=<name>` / `--format=<...>` select the entry's rendering,
  shared verbatim between `sg log` and `sg show`** (Phase 60a,
  `sg_pretty_parse` in `src/cli/commit_out.c`; `include/sg/commit_out.h`).
  **The grammar is five ordered rules, and the ordering is load-bearing,
  all measured against real git 2.55.0** (given the text after `=`, or the
  literal `"medium"` for a bare `--pretty` with no `=`):
  1. begins with the literal lowercase `format:` -> FORMAT (separator) mode.
  2. begins with the literal lowercase `tformat:` -> TFORMAT (terminator) mode.
  3. **case-insensitively** equals a builtin name -> that builtin.
  4. contains a `%` anywhere -> TFORMAT mode, the whole string is the format.
  5. otherwise -> error, exit 1 (git: 128, `fatal: invalid --pretty format: x`).

  WARNING: **rules 1-2 are case-SENSITIVE while rule 3 is case-INSENSITIVE**
  -- `--pretty=Oneline` resolves the builtin, but `--pretty=FORMAT:%H`
  misses rule 1 entirely and falls through to rule 4, printing the literal
  `FORMAT:` before the hash. The case fold is a hand-rolled ASCII-only
  compare, not `strcasecmp` -- same reasoning as `sg_path_component_is_safe`
  in `workdir.h`: locale-dependent folding has no business anywhere near an
  ASCII identifier table.
  WARNING: **rule 3 requires the WHOLE string to equal a builtin name, not a
  prefix** -- `--pretty=oneline%H` is NOT the oneline builtin, it falls to
  rule 4 (literal `oneline` then the hash).
  WARNING: **a bare `--format=<str>` with neither a builtin name, a
  `format:`/`tformat:` prefix, nor a `%` anywhere is REJECTED, the same as
  `--pretty=<str>` would be** -- `--format=plain` is a rule-5 error on both
  tools, measured; there is no separate "assume format: mode" fallback for
  `--format=`.
  **Placeholder expansion (the `%H`/`%an`/etc. table) is implemented as of
  Phase 60b.** `sg_pretty_validate_format` (`commit_out.c`) walks a
  FORMAT/TFORMAT user_format's `%`-sequences once at the CLI layer, before
  any commit is ever rendered (`cmd_log.c`'s/`cmd_show.c`'s
  `resolve_pretty_arg`), and `expand_user_format` does the actual rendering
  per commit, driven by the same `decode_placeholder` token table so the two
  can never drift apart. The supported table: `%H %h %T %t %P %p` (ids,
  `%h`/`%t`/`%p` abbreviate to 7 hex like the rest of this project, `%P`/`%p`
  are every parent space-separated); `%an %ae %al %ad %aD %at %ai %aI %as`
  and the committer mirror `%cn %ce %cl %cd %cD %ct %ci %cI %cs`; `%s %f %b
  %B`; `%n %% %xNN`.
  WARNING: **six date renderings, not one** -- `%ad`/`%cd` reuse the
  existing `sg_date_format_normal`, `%as`/`%cs` reuse `sg_date_format_short`
  (Phase 60a), and Phase 60b adds three more to `date.h`:
  `sg_date_format_rfc2822` (`%aD`, `"Wed, 15 Nov 2023 06:13:20 +0800"` --
  the day of month is NOT zero-padded, measured: day 4 renders `"Sat, 4 Nov
  2023"`), `sg_date_format_iso` (`%ai`, `"2023-11-15 06:13:20 +0800"`, a
  space between date and time), and `sg_date_format_iso_strict` (`%aI`,
  `"2023-11-15T06:13:20+08:00"`, a literal `T` and the timezone with a
  colon). All six show the epoch SHIFTED INTO the stored offset, same rule
  `sg_date_format_normal`'s own warning states. `%at` is the one exception
  that needs no shifting at all -- it prints the raw epoch integer.
  WARNING: **`%aI`/`%cI`'s ZERO offset is a further exception on top of the
  colon-insertion rule**: `"+0000"` and `"-0000"` both render as a literal
  `"Z"`, never `"+00:00"`/`"-00:00"` -- measured, and easy to miss because
  every other Phase 60 fixture in this file happens to use a non-zero
  offset; interop's own `phase60b` group used `+0000` and caught this.
  WARNING: **`%f`'s algorithm is "collapse a RUN of non-title bytes to ONE
  `-`", not "one `-` per byte"** -- a byte is TITLE (alnum, `.`, or `_`;
  `@` is deliberately NOT title, despite an older git comment claiming
  otherwise) and copied as-is, with consecutive `.` bytes collapsing to a
  single `.`; a run of non-title bytes (including each byte of a multi-byte
  UTF-8 character, since none of them test alnum) collapses to a single
  `-`, emitted lazily right before the next title byte -- a run at the very
  end of the string, with nothing following it, emits nothing at all.
  Leading `-` bytes are then stripped, but a leading `.` is NOT (measured:
  `".leading"` keeps its dot, `"-leading"` loses its dash), and trailing
  `-`/`.` bytes are both stripped. Ten rows measured against real git 2.55.0
  pin this in both `tests/test_pretty_format.c` and interop; the `café` row
  (a UTF-8 character followed by a space) is what distinguishes "collapse a
  run" from "one dash per byte", and the `a...b` -> `a.b` rows are what
  prove `.` is not simply lumped in with ordinary non-title punctuation.
  WARNING: **`%b`/`%B` differ, and `%b`'s blank-line skip is not just ONE
  line** -- `%B` is the raw message verbatim; `%b` is everything after the
  first blank line, with any FURTHER immediately-following blank lines also
  skipped (measured: a message with two consecutive blank lines after the
  subject still starts `%b` with no leading blank at all). A message with
  no blank line anywhere gives `%b` an empty string; measured on a
  body-less commit, `%b` is empty and `%B` is `<subject>\n`.
  WARNING: **`%b`'s notion of "blank line" is `line_is_blank`** (empty OR
  all-whitespace), the SAME test `fold_subject`/`first_paragraph_span`/
  `print_message` use, not a narrower literal-`"\n\n"` search -- Phase
  60d fixed a review-round bug where `print_body` had re-derived the OLD,
  narrower rule while every sibling function in the same diff had already
  converged on `line_is_blank`. Measured: a separator line containing a
  single space or tab (not literally empty) still counts as the blank-line
  boundary in real git.
  WARNING: **`%s` FOLDS a multi-line subject, and this rule now has FIVE
  call sites sharing ONE function, `fold_subject` in `commit_out.c`** --
  `%s`, the `oneline`/`reference` builtins, and legacy `--oneline` all
  fold; `%f` and `short` deliberately do NOT (see their own entries below).
  The algorithm (measured against real git 2.55.0, 8 rows plus 2 further
  probes): skip leading BLANK lines (a line is blank if it is empty or
  consists entirely of spaces/tabs), then join every following line up to
  (not including) the next blank line with a single space -- each line's
  own TRAILING whitespace is stripped before joining, but a continuation
  line's LEADING whitespace survives untouched
  (`"l1\n l2\n l3\n\nbody\n"` -> `"l1  l2  l3"`, two spaces: one join,
  one preserved leading space). A message with NO blank line anywhere folds
  its entire remaining content into one line
  (`"l1\nl2\nl3\n"` -> `"l1 l2 l3"`, and `%b` for that same message is
  empty -- there is no body left once every line joined the subject).
  WARNING: **`short` is the ONE exception, and does NOT go through
  `fold_subject`** -- measured directly: `git log --pretty=short` on a
  message whose first paragraph spans 3 lines prints 3 SEPARATE indented
  lines, not one folded line, despite `short` otherwise being subject-only
  (no body). It uses `first_paragraph_span` instead: same leading-blank-
  skip and same "stop before the next blank line" boundary as
  `fold_subject`, but the lines are handed to `print_message` UNCHANGED
  (verbatim, one physical line per output line) rather than joined.
  WARNING: **`%f` was already correct and stays untouched** -- it sanitizes
  only the literal first physical line, never the folded form (same input
  `"subject\ntrailing no blank\n"` gives `%f` = `"subject"`, not `%s`'s
  folded `"subject trailing no blank"`); `%b`/`%B` were also already
  correct. Do not route either of them through `fold_subject`.
  WARNING: **two further bugs were found while measuring this, and are
  FIXED as of Phase 60c** -- see the dedicated `print_message` bullet below
  for the full rule and why no earlier fixture (built through ordinary
  porcelain commits) ever exercised either shape.
  **Section 5.3's rejection is a deliberate divergence from real git**:
  git prints an unrecognized placeholder literally (`%z` -> `%z`) and
  renders a recognized-but-contextless one as empty (`%C(red)`, `%d`, `%N`,
  `%G?`); sg refuses the WHOLE invocation instead, exit 1, naming the
  offending sequence (e.g. `sg: unsupported --pretty placeholder '%z'`) --
  same standing convention as `--patience`/`--diff-algorithm=` being
  rejected rather than approximated. A lone trailing `%` is rejected the
  same way (git prints it literally: `--format=100%` -> `100%`). Pinned on
  both sides in interop's `phase60b` group (`%z`, `%ar`, `%d`, `%C(red)`,
  `100%` -- git exits 0, sg exits 1).
  **Bare `--pretty` (no `=`) means `medium`; bare `--format` (no `=`) is a
  usage error** -- a measured asymmetry, reproduce it rather than "fixing"
  it into a symmetric pair.
  **The separator model (section 4 of the Phase 60 spec, `docs/DESIGN.md`'s
  Phase 60a section has the full byte-level derivation) reduces to ONE rule
  change, not a kind-by-kind dispatch**: `commit_out.c`'s existing blank-
  line-or-`---`-before-the-diff print (unchanged since Phase 54) is skipped
  only when `!o->oneline` -- Phase 60a's only change is widening that
  exemption to also cover builtin `SG_PRETTY_ONELINE`. `format:`'s entries
  simply never emit their own trailing newline, so the SAME separator bytes
  that read as a blank line after every other kind's self-terminated entry
  read as a single line break (or a `---` stuck directly onto the entry
  text with no line break at all) after `format:`'s bare text -- this is
  not a second rule, it falls out of `format:` entries having no terminator.
  WARNING: **between LOG ENTRIES this is NOT the same rule as
  entry-to-diff**, and needed independent measurement: `tformat:` entries
  self-terminate and get NOTHING extra between them (opposite of its
  entry-diff behavior, where it DOES get the blank line); `format:` entries
  get exactly one `\n` printed before each entry but the first, with none
  after the last (same as every builtin except `oneline`, which like
  `tformat:` gets nothing between entries either).
  WARNING: **`render_merge_diff` (`cmd_show.c`'s merge-specific diff
  renderer) needed NO code changes for any of this** -- it already prints
  its leading blank line unconditionally regardless of `o->oneline` (a
  pre-existing, documented divergence: `git show --oneline <merge>` still
  gets a blank line before `diff --cc`), which is coincidentally already
  format-kind-agnostic; measured against a 2-parent merge with all three of
  `format:`/`tformat:`/builtin `oneline`, zero mismatches with zero changes
  to that function.
  **`reference` uses the AUTHOR date, not the committer's** -- measured
  with a fixture whose author date (Nov 14) and committer date (Nov 17)
  fall three days apart (an ordinary ~100-second author/committer gap can
  land on the same calendar day depending on time zone, and would not have
  distinguished the two). The short `YYYY-MM-DD` form is a new function,
  `sg_date_format_short` (`include/sg/date.h`), sharing `sg_date_format_normal`'s
  shift-into-`tz` step via a factored `shift_tm` rather than a second,
  independently-drifting implementation of the offset rule.
  WARNING: **tab expansion is NOT "medium only"** -- an earlier note here
  (written when only `oneline`/`medium` existed) undersold this: measured
  directly, `full` and `fuller` also expand tabs to columns of 8, `short`
  and `raw` do not, matching git's own "`cmit_fmt >= CMIT_FMT_MEDIUM`"
  grouping. `oneline`'s subject line and `raw`'s message block leave a tab
  raw.
  **`sg_commit_out_opts` gained exactly one field for this**, `const
  sg_pretty_format *pretty` (NULL means the pre-Phase-60 legacy path,
  decided by the existing `oneline` bool) -- per `CLAUDE.md`'s own Phase 29
  shared-struct warning (Phase 59 broke `sg log -p` silently by adding two
  bool fields to this exact struct without auditing every construction
  site). All three construction sites were re-audited: `cmd_log.c` sets
  `o.pretty = NULL;` explicitly; `cmd_show.c`'s `resolve_commit_out_opts`
  derives it from a new `show_flags` field (storage kept ON `show_flags`
  itself, not a bare local, so the borrowed pointer outlives the whole
  render loop); the third site (`cmd_show.c`'s `header_o = o;` merge-header
  copy) needed no change, being a whole-struct copy.
  WARNING: **an ANNOTATED TAG's header follows a DIFFERENT rule per
  builtin, not just `--oneline`** (found by a 212-probe external oracle,
  round 2): `Tagger:`/`TaggerDate:` etc. is table-driven in
  `resolve_tag_header_shape` (`cmd_show.c`) -- a date line (`Date:` or,
  padded, `TaggerDate:`) appears ONLY for medium/fuller, the same two that
  show a date on a COMMIT header; the blank line between the tag message
  and its target is suppressed ONLY for oneline/reference. Phase 59's fix
  branched directly on `flags->oneline`, so `--pretty=oneline` (which does
  not set that bool) never reached it, and the other five builtins had
  never been measured against a tag at all -- **always build a tag fixture
  when adding a new commit-header format**, a commit-only fixture cannot
  see this dimension.
  WARNING: **`short` is SUBJECT-ONLY, same as `reference`** -- it does NOT
  print the message body. Every Phase 60a-round-1 fixture happened to use a
  one-line commit message, so `print_message(commit->message, 0)` (the full
  body) looked byte-exact by coincidence; a body-bearing fixture is what
  makes this observable. Use `print_message_subject_only`.
  WARNING: **`reference` needs a SEPARATE rule for between-LOG-ENTRIES**,
  independent of its entry-diff rule: `git log -N --pretty=reference`
  prints entries back to back with NO blank line, while `git show
  --pretty=reference -p` on one entry still gets the ordinary blank line
  before its diff. These are two different separators measured
  independently -- `cmd_log.c`'s `suppress_join` needed `SG_PRETTY_REFERENCE`
  added to its OR-chain; `commit_out.c`'s `print_commit_diff` (the
  entry-diff separator) is correctly untouched.
- **`print_message` (the WHOLE-message-body renderer shared by
  `medium`/`full`/`fuller`/`raw`, `commit_out.c`) has THREE rules beyond
  "indent every line 4 spaces", fixed in Phase 60c** (measured against real
  git 2.55.0 via `git hash-object -t commit -w --stdin` -- **`git commit`'s
  own message cleanup silently erases every one of these shapes before the
  object ever reaches the store**, which is exactly why no fixture built
  through ordinary porcelain commits ever exercised any of them, and why
  interop's fixtures for this bullet use the plumbing layer instead):
  1. Leading blank lines (empty OR all-whitespace -- `line_is_blank`, the
     SAME test `fold_subject`/`first_paragraph_span` use, factored into one
     shared function) are skipped ENTIRELY, not even rendered as `    \n`.
  2. Trailing blank lines are likewise skipped entirely (measured:
     `"subj\n\nbody\n\n\n"` renders identically to
     `"subj\n\nbody\n"`).
  3. Every line's own TRAILING whitespace (spaces and tabs) is stripped
     before indenting -- this applies to EVERY line, not just the subject,
     including body lines, and applies REGARDLESS of `expand_tabs` (measured
     on `raw`/`short`, neither of which expands tabs, both still strip
     trailing whitespace).
  WARNING: **a BLANK line in the MIDDLE is preserved, and EACH one
  individually -- there is no squeezing.** Measured: two consecutive middle
  blank lines render as TWO separate `    \n` lines, not one. This needs no
  special case: a middle blank/whitespace-only line's content, after the
  SAME trailing-whitespace-strip every other line gets, is simply empty, so
  it naturally prints as `    ` + nothing + `\n` -- the only lines that
  need SKIPPING (not printing) are the leading and trailing RUNS, which
  `print_message` implements by buffering each blank-line run and flushing
  it lazily right before the next non-blank line (a run that reaches the
  end of the string unflushed is a trailing run, discarded unprinted).
  WARNING: **`%B` is completely unaffected -- this is a RENDERING rule, not
  a message-content rule.** `%B` returns the raw message verbatim
  regardless of any of the above (measured: `%B` on
  `"subj\n\nbody\n\n\n"` still returns all five lines, blank ones
  included). Do not go looking for this logic near `sg_commit_parse` or
  `expand_user_format`'s `PH_RAW_BODY` case -- it belongs only in
  `print_message`, which only `medium`/`full`/`fuller`/`raw` call.
  WARNING: **trailing-whitespace-strip and tab-expansion are measured to
  commute** -- both orders were tested and give byte-identical output,
  because the trailing suffix being stripped is always pure whitespace,
  unaffected by an earlier tab's own column expansion. The implementation
  strips first (`print_message_line_stripped`), then hands the trimmed
  range to the unchanged `print_message_line` for tab expansion.
