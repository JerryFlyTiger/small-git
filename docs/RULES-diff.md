# Rules: diff -- change lists, output formats, alignment algorithms

Scope: `src/workdir/diff.c`, `src/cli/diff_out.c`, `src/util/diff_lcs.c`, `include/sg/diff.h`, `include/sg/diff_out.h` -- the four builders, the six output formats, combined diff, Myers/histogram.

**Read this file before editing anything in that scope.** It is not
background reading: every rule here was measured against real git 2.55.0,
and most of the `WARNING:`s exist because something slipped past a fully
green board. Project-wide rules -- the gates, the code conventions, the
deliberate divergences -- stay in `CLAUDE.md`; the full derivations and
measurement tables stay in `docs/DESIGN.md` (look up a specific section,
do not read the whole thing).

- **`sg diff` renders a combined diff for an unresolved conflict since Phase
  34** (`-c` / `--cc`, `src/cli/diff_out.c`'s `render_combined_patch` and
  friends -- a from-scratch port of git's `combine-diff.c`, fixed at exactly
  2 parents: ours = index stage 2, theirs = index stage 3, result = the
  working-tree file). Only `sg_diff_index_workdir` (plain `sg diff`, no rev,
  no `--cached`) can produce a "combinable" row -- `sg_diff_entry` carries
  three extra sides (`ours`/`theirs`/`result`) for exactly this, ABSENT
  unless `unmerged` is set; a row is combinable iff both `ours` and `theirs`
  are non-ABSENT. `sg_diff_tree_index` (`--cached`) never fills them, which
  is why `--cached` always prints `* Unmerged path <p>` regardless of
  `-c`/`--cc` -- there is no special-case branch for that, it falls out of
  the data layer.
  WARNING: **PATCH's default IS dense combined, even with no flag at all**
  (measured: `git diff` on a conflict prints `diff --cc` unprompted) --
  `--cc` only makes that explicit, and only `-c` turns density off. The
  other five formats (`--stat`/`--numstat`/`--shortstat`/`--name-only`/
  `--name-status`) do the OPPOSITE: a combinable row renders exactly as it
  did before Phase 34 **unless `-c`/`--cc` is explicit** -- an unflagged
  `sg diff --name-status` on a conflict must keep printing `U`, not silently
  start printing `MM`. `sg_diff_out_opts.combined` (0/1/2) carries both
  "which flag" and "was one given at all" for exactly this asymmetry; see
  its header comment before touching either half.
  WARNING: **`-c`/`--cc` last one wins**, same convention as `-M`/`-C`
  (Phase 33's lesson: untested together, one flag stays silently stuck) --
  `-c --cc` prints `diff --cc`, `--cc -c` prints `diff --combined`.
  **`-c`/`--cc` with a `<rev>` is implemented as of Phase 40** (it was
  rejected outright before that). It is a SECOND producer of
  `ours`/`theirs`/`result`, and it pairs
  **[the index's own entry for the path, the named tree's blob]** against
  the working-tree file -- read Phase 40 of `docs/DESIGN.md` before touching
  it, because Phase 34's recorded reason for the old rejection ("stage 1 vs
  the named tree blob") was measurably wrong in three ways and each one is
  still an inviting way to reimplement it incorrectly:
  it has **nothing to do with conflicts** (a repo with no unmerged entry
  anywhere still prints `diff --combined`); parent 1 is **the index**, not a
  merge base and not HEAD; and parent 1 is the **lowest stage present**, not
  stage 1 (an add/add conflict has no stage 1 and pairs against stage 2).
  WARNING: **it only takes effect for exactly one rev with no `--cached`**.
  `-c --cached <rev>` and `-c <rev1> <rev2>` are unaffected, and neither
  needs a special-case branch -- `sg_diff_trees`/`sg_diff_tree_index` never
  fill `ours`/`theirs`, so it falls out of the data layer, the same way
  `--cached` already did in Phase 34.
  WARNING: **it changes the OUTPUT ORDER of every format**: all combined
  rows print first in path order, then all non-combined rows in path order
  (`sg_diff_reorder_combined_first`, applied only in this mode). An
  interleaved fixture is required to observe this at all.
  WARNING: **rename/copy detection must never see a combined row**
  (`rename.c`'s three predicates skip them). Only a `-C` fixture makes this
  observable -- under plain `-M` a combined row is a modification and would
  never have been paired anyway.
  WARNING: **it also widens which rows EXIST**, not just how they render: a
  row is included when the result differs from **any** parent, so a path
  whose working tree matches the named tree but whose index does not still
  appears (measured: plain `git diff <rev>` prints nothing for it). This is
  why `sg_diff_tree_workdir` takes a `combined` parameter -- a pass running
  after the builder cannot recover a row the builder never emitted.
  WARNING: **`sg_diff_entry_is_combined`'s treatment of `result` is
  deliberately asymmetric**: a real conflict still combines with the
  working-tree file deleted, the Phase 40 rev-mode row does not (it falls
  back to an ordinary `deleted file mode`). Both sides are pinned by
  interop; "unifying" them breaks whichever one you did not measure last.
  WARNING: **the funcname suffix's off-by-one is git's own bug, ported
  faithfully, not fixed**: `comment_end` records the index of the last
  non-blank byte scanned (cap 40 bytes), and the print loop stops BEFORE
  that index, so a trailing `{` silently disappears while the space before
  it survives. `combine-diff.c`'s own minimal heuristic (first byte
  alnum/`_`/`$`) is a SEPARATE implementation from the 2-way patch body's
  `find_function_name` -- do not merge the two.
  WARNING: git's 40-byte funcname scan walks the raw buffer relying on a
  NUL sentinel to stop (`if (!ch) break`); sg's buffers are plain
  `malloc`'d and carry no such sentinel, so the scan is clamped to the
  bytes actually left in the result buffer instead -- a memory-safety fix
  (`make sanitize` catches the heap-overread otherwise), **not a claimed
  output divergence**: git's own stop condition and sg's clamp land on the
  same byte (binary detection already rules out an embedded NUL, and a
  funcname candidate is by construction a line outside every hunk, which
  can never be the file's actual last line), and 216 targeted comparisons
  against real git (9 funcname lengths x 4 leading-context depths x
  with/without a trailing no-newline delete x 3 flag combinations) found 0
  mismatches. **Do not add this to Phase 34's deliberate-divergence list**
  (`* Unmerged path` unquoted -- see `docs/RULES-paths-strings.md`'s quoting
  bullet, pinned on both sides by interop; the rev-argument rejection that
  used to sit beside it is gone, implemented in Phase 40): this one has
  no oracle-side pin, because there is nothing measured to diverge on.
  WARNING: **when touching the combined-diff block of `diff_out.c`
  (`render_combined_patch` and everything it calls), a green `make test`
  does not count, same as the funcname/LCS notes above** -- run
  `python3 tests/fuzz_combined.py 150` (real git is the oracle, nothing is
  borrowed from sg) and report the actual mismatch count. Baseline (Phase
  34, measured): 150 rounds, 104 produced a real conflict, 2 mismatched,
  both attributed to the project's then-pre-existing LCS-vs-Myers 2-way
  alignment residual (the funcname WARNING three above this one has the
  same underlying diff engine), not to the combined layer -- see
  `tests/fuzz_combined.py`'s own docstring and Phase 34 of
  `docs/DESIGN.md` for the attribution method. **That residual is gone as
  of Phase 35** (`diff_lcs.c` now runs git's actual Myers algorithm, not an
  unreduced LCS backtrack) -- re-measured at 200 rounds x 3 seed ranges,
  0 mismatches in all three. A number higher than 0 is a regression.
- **"Which paths changed" always goes through `sg_diff_*`**
  (`include/sg/diff.h`, Phase 25): four builders correspond to
  tree<->tree, tree<->index, index<->working-directory, tree<->working-
  directory, producing a `sg_diff_list` **sorted by path**. **Do not
  hand-roll an index-walking loop again** -- before the rewrite, `sg diff`
  could only ever compare index against the working directory, precisely
  because "find the changes" and "print them" were the same loop. Passing
  `NULL` for `old_tree` means an empty tree, so an unborn HEAD does not need
  to write an empty tree object just for diff.
  WARNING: **a conflicted path gets three different answers under the three
  comparisons** (`--cached` gives a single `U` line; `<rev>` gives an ordinary
  `M`, because index only decides membership and content still comes from the
  working directory; index-vs-working-directory gives `U` plus a stage 2 vs
  working-directory line, two lines total). All three were measured against
  real git 2.55.0, do not unify them by intuition.
  WARNING: **a blob that cannot be read must not fail the whole call**: the
  builder must still put that path into the list as changed, letting the
  rendering layer print an actionable message with the path attached. If the
  whole list dies, nobody even learns which file was broken.
- **When the caller ALREADY has both sides flattened, go through
  `sg_diff_from_flat_lists`, not `sg_diff_trees`** (`include/sg/diff.h`,
  Phase 52). `sg_diff_trees` flattens both of its tree ids itself and frees
  both before returning, so a caller holding live `sg_flat_list`s pays for
  the same walk twice. `sg_merge_trees` was doing exactly that: the main
  union walk flattens base/ours/theirs, then `build_rename_map` called
  `sg_diff_trees` twice, giving **seven** flattens of three trees where three
  suffice, each one re-reading and re-inflating every tree object at every
  directory level (there is no parsed-object cache anywhere).
  `sg_diff_trees` deliberately KEPT its signature -- it has 12 call sites and
  only one of them has a flat list to hand -- and is now a thin wrapper over
  the new function. **The lists are BORROWED**: the new function neither
  frees them nor outlives them, and it cannot report `-2`/`bad_path` because
  the caller did the flattening and already had its chance to. That
  borrowing is only safe because `list_append` makes its own strdup of every
  path, which is the same reason `sg_diff_trees` could always free both flat
  lists before returning its `sg_diff_list`.
  WARNING: **nothing about this refactor is visible in any merge result**, so
  `tests/test_merge_renames.c`'s 14 named shapes and `fuzz_merge_rename.py`
  stay green whether or not it landed. `sg_tree_flatten` therefore counts its
  calls behind a named test hook (`sg_tree_flatten_test_count` /
  `_reset` in `include/sg/tree_build.h`, observability only -- **nothing in
  `src/` may branch on it**, and it is not thread-safe), and
  `tests/test_merge_flatten_count.c` asserts the count is **exactly 3**, for
  `rename_score` default and for 0 separately. The exact number is the whole
  point: `>= 1` or `< 7` would not catch a reintroduced flatten. Measured by
  mutation -- one extra valid flatten reports "got 4", and removing the
  counter increment reports "got 0".

- **Printing a diff always goes through `sg_diff_print`**
  (`include/sg/diff_out.h`, Phase 25), six formats (patch/`--stat`/
  `--numstat`/`--shortstat`/`--name-only`/`--name-status`).
  **`sg_diff_out_opts.summary` (Phase 50) is git's `--summary` block, and is
  NOT a seventh format**: it composes with one, printing after that format's
  own output. Its only caller is `sg merge`'s fast-forward report and only
  `--stat` exercises it. Four measured line shapes (`create mode`,
  `delete mode`, `rename <compressed pair> (NN%)`, `mode change`); the
  rename line reuses `--stat`'s own compressed pairing column, so the two
  cannot drift apart. `sg diff` and
  `sg stash show` share this one, do not write a second formatter.
  The patch body has been **byte-for-byte identical to real git** since Phase
  26 (the `index` line, `new file mode`/`deleted file mode`/`/dev/null`,
  context-3 multi-hunk splitting, function-name suffixes, `\ No newline at end
  of file`), so interop does a full-output `cmp` for **all six formats**.
  **As of Phase 35, the alignment step (`sg_diff_build_script` in
  `src/util/diff_lcs.c`) is a direct port of git's actual Myers algorithm**
  (`xdiff/xdiffi.c`'s `xdl_split`/`xdl_recs_cmp` + `xdiff/xprepare.c`'s
  `xdl_trim_ends`/`xdl_cleanup_records`), not the old unreduced LCS
  backtrack -- the ~2-3% positioning residual Phase 26 measured (of 11
  cases, 6 byte-identical to `git diff --histogram`, because the old
  backtrack's tie-breaking happened to lean that way on ties, not because
  it implemented histogram) is now **0 mismatches**, re-measured at 500
  rounds x 4 seed ranges via `tests/fuzz_diff.py` and 200 rounds x 2 seed
  ranges via `tests/fuzz_combined.py`. Details, including the coordinate-
  mapping trap (Myers runs in a coordinate space `xdl_cleanup_records`
  compacted, and writing `changed[]` back at the wrong index is the easiest
  way to get this wrong) and the perf numbers, are in Phase 35 of
  `docs/DESIGN.md`. **Since Phase 41 `src/workdir/merge.c`'s three-way
  merge uses this same aligner** -- Phase 35 deliberately left it on the old
  LCS backtrack because no fuzzer covered merge's alignment, and Phase 41
  built that net (`tests/fuzz_merge.py`) first, then moved it. That made
  merge the LAST caller of `sg_diff_lcs_table`/`_exact`/`_free_table`, so
  **those three are gone**; do not reintroduce an LCS table, and note the
  reason is not output (holding has_nl constant, Myers and an exact LCS
  backtrack produced identical output on all 200 rounds) but cost: 6000
  lines a side was 602 MB / 0.37s with the table and 10.8 MB / <0.01s with
  Myers, measured. `sg_diff_lines_equal` (has_nl-blind) survives only for
  `diff_out.c`'s combined diff.
  **Since Phase 42 the algorithm is a MANDATORY parameter of
  `sg_diff_build_script`** (`sg_diff_algorithm`, no default, same idiom as
  `sg_workdir_missing`), because git's own two defaults differ: `git diff` is
  Myers, `git merge` is histogram. The histogram port is a reconstruction
  cross-checked against git's `xdiff/xhistogram.c`; it deliberately does NOT
  trim or clean up records first (git guards `xdl_optimize_ctxs` off for
  histogram -- cleanup DISCARDS lines, and occurrence counts are exactly what
  that algorithm decides on). **The divergence from `xhistogram.c` itself is
  and always was zero** -- see the next paragraph for why Phase 42's eight
  rule variants, all searched inside the histogram algorithm, could never
  have found the real cause.
  **Phase 42's ~0.9% divergence (5/500 and 4/500) is fixed as of Phase 52,
  root cause found and NOT in `xhistogram.c` at all.** The gap was in
  `compact_one_side` (`src/util/diff_lcs.c`), the post-processing step that
  runs AFTER either aligner produces its raw script, and it is a HISTOGRAM-
  ONLY step git also has: `xdiffi.c`'s `xdl_change_compact` (git 2.55.0,
  `xdiffi.c:940-958`) reruns Myers on a group's own pair of records whenever
  sliding-compaction moves or resizes that group AND the opposite side's
  matching group is non-empty -- newly-revealed matching lines then fall
  back to unchanged. sg's `compact_one_side` slid groups but never reran
  anything, so a slide that should have re-exposed a shared line instead
  left it inside an oversized changed span. The port adds the same rerun,
  gated on `histogram` (compact_one_side's own new parameter) and on the
  group's start/end actually having moved. One adaptation was required:
  git's rerun `memcpy`s the new changed-bits over the old, but sg's
  `myers_diff` only ever SETS bits to 1 and never clears them, so the port
  must `memset` both sides' changed ranges to 0 immediately before calling
  it, or leftover 1-bits from the pre-rerun script survive the rerun.
  New baseline, measured: `fuzz_diff.py 500 --histogram` is **0** across
  three seed ranges (was 5/500, 4/500), `fuzz_diff.py 500` (Myers, which
  never takes the `histogram` branch) is unaffected at **0**, and all of
  `fuzz_merge.py`/`fuzz_merge_rename.py`/`fuzz_combined.py` stayed at their
  existing 0 baselines. `tests/test_diff_histogram.c` pins the smallest
  known fixture that distinguishes the two behaviours (old `"R\n\nR\n\n"` ->
  new `"R\nR\n\n"`: git deletes only the first blank line; without the
  rerun sg answered with a single two-line replacement instead).
  WARNING: **three of git's own sub-conditions on that rerun have NO witness
  and that is deliberate, measured, not a gap to fill**: the `histogram`
  gate, the "group actually moved" clause, and the "opposite group
  non-empty" clause. Each was removed and fuzzed (1500 Myers rounds for the
  first, 1500 histogram rounds plus merge for the other two): 0 mismatches
  every time. They are kept for faithfulness to git. **Do not go looking for
  their tests, and do not write one on the assumption it must be possible.**
  The companion Myers assertion beside the fixture is NOT such a witness --
  it was added believing it guarded the `histogram` gate, and measurement
  showed it cannot, because git's Myers and histogram answers on that
  fixture are the same one. It has been renamed
  `test_myers_answer_on_the_recompact_fixture` to claim only what it pins;
  a control whose two arms already agree is not a control.
  WARNING: **a single-digit `fuzz_diff.py` mismatch is not automatically an
  algorithmic divergence.** That script counts a non-zero `sg` exit as a
  mismatch, and in `--max-failures 0` mode discards the repo and the output,
  so a subprocess that fails to start under load used to be
  indistinguishable from a real one (measured: 1 phantom in ~2500 rounds,
  which did not survive five reruns of the same seed range). Since Phase 52
  it prints `of which sg exited non-zero: N` separately -- read that line,
  and rerun the same seed range, before calling anything a divergence.
  **Since Phase 53 all five fuzzers make this separation, and the count is
  no longer a lower bound** -- `fuzz_diff.py`'s mode loop runs every mode
  instead of stopping at the first divergence (it still reports and
  reproduces the first one), so a later mode's non-zero exit is counted too.
  The wording differs per harness because the discriminator does. Where sg
  is expected to exit 0 (`fuzz_diff`, `fuzz_combined`, `fuzz_rename`) the
  suspect class is any non-zero exit, printed as `of which sg exited
  non-zero: N`. Where non-zero is a legitimate answer -- `fuzz_merge` and
  `fuzz_merge_rename`, where a conflict IS exit 1 -- the suspect class is an
  exit status **outside {0,1}**, which by this project's own exit-code
  convention is not an answer sg can give, and it is its own `crash`
  category rather than an `rc` divergence. **A crash round still fails the
  run**, it is just never an algorithmic divergence.
  WARNING: **all five count a round only when the exit status is its ONLY
  evidence.** A round that ALSO disagreed on bytes (or, in `fuzz_rename`,
  on the pairing or the score) while exiting 0 is a real divergence that
  happened to crash somewhere too, and offering it under a line that says
  "rerun before calling this a divergence" is strictly worse than not
  counting it: it invites the reader to dismiss a genuine bug. Measured in
  Phase 53 -- master itself laundered six of six rounds this way when the
  crashing mode sorted first, and this phase's own first draft laundered
  the mirrored case as well; `fuzz_rename` went from calling 8 of 8 rounds
  suspect to calling 3, the 5 excluded being exactly those holding a real
  score divergence.
  WARNING: **git's exit code is NOT held to the {0,1} rule** -- that
  convention is sg's own, and real git exits 128 on its own fatal errors.
  Both merge harnesses classify a git exit outside {0,1} as `setup` ("these
  measure nothing -- fix first"), never as a crash, because what it says is
  that the ORACLE did not answer.
  WARNING: **in the two merge harnesses this was not merely mislabelled, it
  was mostly INVISIBLE.** Measured in Phase 53 by forcing `sg merge` to exit
  139 while leaving its output untouched: over 5 rounds the pre-Phase-53
  `fuzz_merge` reported `2 rc mismatches` and scored the other 3 as passes,
  and `fuzz_merge_rename` reported `1 rc` and passed 4 -- a crash agreeing
  with a git that also conflicted looks exactly like agreement. The check
  therefore sits **before** the merged file is read, not after: a crash is
  precisely the case that can leave the file unwritten, which the later
  guard would file as a setup failure ("measures nothing") instead.
  `tests/interop.sh`'s `phase52:` group cmp's full
  `sg diff --histogram` / `sg diff` output against real git on the same
  fixture. Read Phase 52 of `docs/DESIGN.md` for the exact git source
  excerpt and why Phase 42's search never reached this code.
  WARNING: **`--patience`, `--minimal` and `--diff-algorithm=<name>` are
  rejected as unknown flags, not approximated** -- sg has two aligners and
  answering "patience" with one of them is a wrong answer wearing the right
  flag (same reasoning as `--find-copies-harder=<anything>` staying rejected
  rather than quietly treated as plain `--find-copies-harder`, Phase 51).
  git accepts all four and exits 129 on a bad name; sg exits 1. Both sides
  pinned in interop's `phase42` group.
  When touching `diff_out.c` / `diff_lcs.c` / `workdir/diff.c`, a green
  `make test` **does not count**, run `python3 tests/fuzz_diff.py 500
  --max-failures 0` AND `python3 tests/fuzz_diff.py 500 --histogram
  --max-failures 0` (across a few different `--seed` values, not just the
  default) and report both actual mismatch counts -- **both should now stay
  at 0**, a non-zero histogram count is a regression, not an expected
  residual.
  WARNING: **a single-digit `fuzz_diff.py` mismatch count is not, by
  itself, evidence of an algorithm divergence** -- `fuzz_diff.py` used to
  conflate a real output mismatch with `sg` simply exiting non-zero (a
  subprocess launch hiccup under load), and in `--max-failures 0` mode the
  triggering round is discarded the instant the tally increments, leaving
  no way to tell the two apart after the fact (fixed in Phase 52, see its
  `docs/DESIGN.md` section 8 -- measured: one such phantom count vanished
  on five reruns of the identical seed). Before reporting a nonzero count
  as a regression, **rerun the same seed range once** and check the
  `of which sg exited non-zero: N` line the script now prints; if N
  accounts for the whole count, it is a flaky subprocess launch, not a
  diff bug. Phase 52 also measured and recorded (`docs/DESIGN.md` section
  7) that three of `compact_one_side`'s guard sub-conditions -- the
  `histogram` gate itself, "did the group actually move", and "is the
  opposite group non-empty" -- have no mutation witness on any fuzz input
  found so far, and are kept anyway because each is a faithful copy of
  git's own `xdl_change_compact` condition. **Do not go add a test for
  those three speculatively**; they are recorded as measured-inert, not as
  an open coverage gap.
