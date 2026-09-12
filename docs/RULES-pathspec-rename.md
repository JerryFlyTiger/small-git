# Rules: pathspec matching and rename/copy detection

Scope: `src/workdir/rename.c`, `src/util/similarity.c`, `include/sg/pathspec.h`, `include/sg/similarity.h` -- the three-clause matcher, argument disambiguation, the three detection passes, `-M`/`-C` grammar.

**Read this file before editing anything in that scope.** It is not
background reading: every rule here was measured against real git 2.55.0,
and most of the `WARNING:`s exist because something slipped past a fully
green board. Project-wide rules -- the gates, the code conventions, the
deliberate divergences -- stay in `CLAUDE.md`; the full derivations and
measurement tables stay in `docs/DESIGN.md` (look up a specific section,
do not read the whole thing).

- **pathspec always goes through `sg_pathspec_*`** (`include/sg/pathspec.h`,
  Phase 28); the matching rule is **three ordered clauses**: exact literal
  match, literal directory prefix, only fall through to `sg_wildmatch` if the
  spec contains a wildcard.
  WARNING: **the first two clauses and the third do not add together**: a
  spec containing a wildcard has **no** directory-prefix rule. Measured
  against real git 2.55.0: `o[tx]her` against `other/d.c` prints nothing, and
  `su?` and `s*b` against something under `sub/` are also empty; `sub*`
  matches only because **`*` crosses `/`** (pathspec uses wildmatch with
  WM_PATHNAME turned off), not because it recurses into the directory.
  Unifying the two by intuition would make `sg diff` silently print extra or
  missing files.
  WARNING: **a trailing `/` is meaningful, not noise**: `sub/` lists sub's
  contents, `a.txt/` **matches nothing** (it is asking "what's under this
  name"). `sg_resolve_repo_path_allow_root` normalizes it away, so
  `sg_pathspec_add` needs to remember to reattach it -- this pair is the only
  test that can tell "was it reattached or not".
  WARNING: **magic (`:(icase)`, `:!`, `:/`) must always be rejected, never
  treated as a literal path**: silently matching nothing, or matching a file
  actually named `:!sub`, are both answering a question the user never asked.
  Filtering happens **after the list is already built**
  (`sg_diff_list_filter`), not inside the four builders -- each of the four
  having its own pathspec logic was exactly the shape Phase 27 spent a whole
  milestone eliminating. The cost is that filtered-out files still get hashed
  once, which is a speed bill, not a wrong answer.
- **The disambiguation rule for bare arguments (without `--`) came from
  measurement, do not simplify it** (Phase 28): being both a revision and an
  existing file -> reject outright; **every argument after the first path
  must exist** (`sg diff a.txt HEAD` fails naming HEAD, even though it is a
  perfectly valid revision); neither -> "ambiguous argument". WARNING:
  **arguments containing a wildcard skip the existence check** -- `git diff
  '*.zzz'` still exits 0 even though it matches nothing, while `git diff
  nosuch` is a hard error. Use `sg_pathspec_looks_like_spec` to decide "does
  this look like a pathspec", the character set lives in that one place, next
  to the matcher.
- **`sg_merge_trees` is a consumer of everything below, and its own rules live
  in `docs/RULES-merge.md`** -- it reuses `sg_diff_trees` +
  `sg_diff_detect_renames` unchanged, so a change to the three passes, the
  score scale or the `-M` grammar changes merge's answer too. Read both files
  when touching either.
- **Rename detection always goes through `sg_diff_detect_renames`**
  (`include/sg/diff.h`, Phase 29); it is a **pass that runs after the list is
  already built**, not inside the four builders (same reason as
  `sg_diff_list_filter`).
  WARNING: **it must run after `sg_diff_list_filter`**. Measured against real
  git: `git diff --cached --name-status -- b1.txt` (naming only the new half
  of a rename) prints `A`, not `R100` -- git filters by pathspec first and
  detects second, so only half the pair remains and no match can form.
  Reversing the order has no visible symptom, it just gives a wrong answer in
  this exact scenario.
  **Since Phase 30 both exact and inexact detection are implemented**, and
  the implementation lives in `src/workdir/rename.c` (**not** `workdir/diff.c`
  -- `CLAUDE.md`'s module table will send you to the wrong file). The
  scoring
  itself is `src/util/similarity.c` + `include/sg/similarity.h`, a deliberate
  port of git's `diffcore-delta.c`.
  WARNING: **the score is a machine-readable field**, printed as `R093` and
  `similarity index 93%`, so being one point off is a wrong answer, not a
  near miss -- `similarity.c` may be reproduced but never "improved". Two
  independent ports agreeing is the only evidence that counts here: the
  algorithm was first re-derived in Python and checked against real git on
  750 random file pairs, and the C was then cross-checked against that.
  When touching `similarity.c` or `rename.c`, a green `make test` **does not
  count**: run `python3 tests/fuzz_rename.py 120 --seed <unused> --max-failures 0`
  and report the actual mismatch count.
  WARNING: **there are THREE passes and their ORDER is observable**, so they
  cannot be collapsed into "score every pair and keep the best": exact (by
  id), then same-basename pairs at a **raised** threshold
  (`min + 0.5 * (MAX - min)`, i.e. 75% by default), then the full matrix.
  Measured against git 2.55.0 with two fixtures that disagree about which
  side wins, so no single wrong threshold satisfies both: a name match at 79%
  beats an unrelated 98% match, while a name match at 60% loses to it.
  WARNING: **`-M<n>`'s grammar is a fraction, not a percentage** (all
  measured): `-M5` is 50%, `-M05` is 5%, and **`-M100` is TEN percent** --
  only `-M100%` limits detection to exact renames. The grammar lives in
  `sg_similarity_parse_score`; do not reimplement it at a call site.
  WARNING: **the score is kept on git's 0..60000 scale, not as a percentage**
  (`SG_SIMILARITY_MAX`), because `-M005` asks for 0.5% and a percentage
  cannot hold that. Only `sg_diff_entry.score` is a percentage, converted
  once at the end by `sg_similarity_percent` (which **truncates**: 59999 is
  99%, not 100%).
  WARNING: **text vs binary changes the score.** The CR of a CRLF pair is
  skipped when hashing text but is still counted in the file's size, so a
  CRLF file scores about 66% against *itself*; one NUL byte makes the same
  bytes binary and the same comparison a perfect match. This is exactly why
  git settles exact renames by id BEFORE scoring anything.
  WARNING: **matching uses `sg_diff_side_effective_id`** (promoted to public
  from `diff_out.c` in Phase 29); it returns -1 to mean "this id was never
  verified" -- **two unverified ids do not count as identical content even if
  they happen to be equal**, that side is never paired. The failure direction
  is "not a rename", never "conjure a rename out of nowhere".
  WARNING: **`sg stash show` detects renames too, and only after `-u`'s
  merge** (Phase 31): the tracked and untracked halves become one list first,
  and detection runs once over the whole thing. Measured against git 2.55.0:
  an untracked file that is byte-identical to a deleted tracked file takes
  the source through the exact pass, demoting the real inexact rename beside
  it to a plain `A`. Detecting per-half, or before the merge, silently gives
  a different answer -- there is no special case for it, only the ordering.
  **`sg diff -C` finds copies since Phase 33**, and a copy is
  `old_path != NULL && is_copy`. The rule for which is which is git's, is one
  line, and looks arbitrary until you know it: a source is paired N times,
  each destination spends one use **in path order**, and a row is a copy while
  uses remain after its own. A source that is not a deletion is charged one
  use up front, so anything copied off an edited file is a copy by
  construction. Consequence, measured: with one source and two destinations
  the FIRST by path is the copy and the second is the rename, **however much
  better the second matched**.
  WARNING: **`-C` changes three things, not one.** A path present on both
  sides becomes eligible as a source (the only way to find a copy from a
  merely edited file); a source may be paired more than once; and **the
  same-file-name shortcut is skipped entirely**. Dropping any one of them
  gives a different answer from git.
  WARNING: **`-C -C` / `--find-copies-harder` is implemented as of Phase 51**
  (previously rejected outright). It offers every *unchanged* path as a copy
  source in addition to plain `-C`'s deleted/edited ones, which needed a
  data-layer change: `sg_diff_list` used to only ever hold paths that
  changed, so the three tree-facing builders (`sg_diff_trees`,
  `sg_diff_tree_index`, `sg_diff_tree_workdir`) gained a mandatory
  `include_unchanged` parameter and `sg_diff_entry` gained an `unchanged`
  field that `sg_diff_detect_renames` strips before returning, on every
  success path. `sg_diff_index_workdir` deliberately did NOT gain the
  parameter -- every path there comes from the index, so there is no
  addition for a copy to land on (measured: `git diff -C -C` with no `--cached`
  and no `<rev>` still prints only `M`, never `C`, for a staged-then-edited
  or an unchanged-plus-modified fixture). The CLI state machine (`detect` in
  {NONE, RENAME, COPY}, a staged `score`, and `harder`, resolved only AFTER
  the argv loop) is a faithful port of git's own; see Phase 51 of
  `docs/DESIGN.md` for the full measured truth table -- three results falsify
  simpler models: `--no-renames` does NOT cancel `--find-copies-harder` and
  does NOT reset the score; a bare `-C` RESETS the score to the default; and
  `-C95 --no-renames -C` finds nothing, because `--no-renames` cleared
  `detect` first so the following `-C` takes the "not yet COPY" branch and
  never sets `harder`.
  WARNING: **a genuinely PRE-EXISTING bug was exposed and fixed during
  Phase 51**: `src/workdir/rename.c`'s `exact_pass` and
  `claim_from_matrix` were reading git's `rename_used` counter (`uses` on
  `rename_cand`) as if it were the plain boolean `used` flag, at two call
  sites. `uses` starts at 0 for a fresh deletion and is PRE-LOADED to 1 for
  a source that exists on both sides (a modification, or -- since Phase 51
  -- an unchanged row) -- **`uses` IS git's `rename_used`, `used` is NOT a
  substitute for it**: real git resolves ordinary renames (deletion
  sources only) in a dedicated FIRST pass, and only THEN considers copies
  (modification/unchanged sources) for whatever destinations are still
  unclaimed, so a modification/unchanged source must never win the
  "real renames only" walk regardless of score. Reading `used` erased that
  distinction, since `used` starts at 0 for every FRESH source no matter
  its kind, letting a higher-scoring modification/unchanged source
  outscore or out-tie-break a genuine deletion for the same destination.
  Reproduced (and pinned) with plain `-C` and a genuine modification
  source -- no `-C -C`, no `unchanged` row needed, i.e. this predates
  Phase 51 entirely. Fixed at three sites, all in `src/workdir/rename.c`:
  `exact_pass`'s skip condition and its tie-break score, and
  `claim_from_matrix`'s rename-only-walk skip -- all three now read
  `uses > 0` instead of `used`. See Phase 51 of `docs/DESIGN.md` for the
  two measured witness shapes (`matrix_pass`, `exact_pass`) and their
  fixtures.
  WARNING: **the `exact_pass` witness has a direction trap, and both
  directions are pinned** (`tests/test_rename.c`,
  `tests/interop.sh`'s `phase51:` group): with the modification's path
  sorting BEFORE the deletion's, the old iteration-order tie-break picked
  the modification and was wrong; with the deletion sorting first, plain
  iteration order already gave the right answer even before the fix, so
  that direction alone is a control, not evidence -- the same
  "fixtures all pointing one way hide the gap" lesson this project has
  hit before. `tests/fuzz_rename.py --copies-harder` (new in Phase 51)
  measured 0.8%-1.7% mismatches per 120-round sample before the fix,
  0/120 across two seed ranges after it.
  WARNING: **`-M` and `-C` write the same mode; the LAST one wins.** Measured:
  `git diff -C -M` finds renames only, `-M -C` finds copies. So every `-M`
  branch in `cmd_diff.c` must clear `detect_copies`, and every `-C` branch
  must set it. sg had `-C` sticky in the first order for exactly as long as
  no test combined the two flags.
  WARNING: **two copy-mode properties are masked by other machinery and need
  deliberately built fixtures** (both cost a mutation round to find): copy
  mode skipping the basename shortcut is invisible with ONE source, because
  reuse makes both routes agree -- it takes two; and the exact pass's reuse is
  invisible unless the matrix is out of the way, i.e. at `-C100%`.
  **`sg status` has a rename row since Phase 32**, and `sg_status_diff_staged`
  is no longer a second implementation of tree<->index -- it is a thin adapter
  over `sg_diff_tree_index`, the way `sg_status_diff_unstaged` has been over
  `sg_diff_index_workdir` since Phase 27. The two walks were proven equivalent
  first (`tests/test_status_staged_parity.c`, 12 named shapes + a fuzzer),
  so the swap changed no answer.
  WARNING: **`sg_status_diff_staged`'s `rename_score` is mandatory and has no
  default**, same idiom as `sg_workdir_missing`. `apply.c`'s two safety gates
  pass **0**: they enumerate the list to tell the user what is uncommitted,
  and a rename row carries TWO paths where that loop prints one, so detection
  there would silently stop naming the old path. `cmd_status.c` passes the
  threshold. Do not give this parameter a default.
  WARNING: **a rename is `old_path != NULL`, not a fourth `sg_status_kind`**
  (same shape as `sg_diff_entry`), so every existing `switch` over kind stays
  exhaustive. A renamed row's kind is `SG_STATUS_MODIFIED`, so a consumer
  that knows nothing about renames still sees "this path changed".
  WARNING: **`git status` and `git diff` disagree about a malformed `-M`**
  (measured): `git status -Mabc` exits 0 and quietly uses the default, while
  `git diff -Mabc` exits 129. sg matches each command separately -- do not
  "unify" them onto the shared parser's reject-leftovers rule.
  WARNING: **the porcelain row sort must stay a TOTAL order**
  (`prow_cmp` breaks ties on append position): a path can carry a staged row
  and an unstaged row, only the staged one holds `old_path`, and `qsort`
  leaves equal elements in an unspecified order. Merging x/y across a group is
  order-independent, so this did not matter before renames; it does now.
- **There are two display formats for renames, do not mix them up** (Phase
  29): `--name-status` prints **two separate fields** (`R100\told\tnew`,
  score zero-padded to three digits); `--stat`/`--numstat` print a **single
  compressed pairing column** (`a/{b => z}/c.txt`). Both the prefix and the
  suffix of the compression are computed at `/` boundaries, and **the suffix
  must scan all the way through and update at every `/`** (take the longest),
  stopping at the first `/` would print `{h/i => h2/i}/j.txt`.
  WARNING: **a path that requires quoting turns off compression entirely**
  (measured), because quoting the bracket form would produce a quote mark in
  the middle of the path.
