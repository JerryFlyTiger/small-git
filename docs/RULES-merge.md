# Rules: three-way merge and rename-aware tree merge

Scope: `src/workdir/merge.c`, `include/sg/merge.h` -- `sg_merge_content`'s region model and `sg_merge_trees`' rename handling.

**Read this file before editing anything in that scope.** It is not
background reading: every rule here was measured against real git 2.55.0,
and most of the `WARNING:`s exist because something slipped past a fully
green board. Project-wide rules -- the gates, the code conventions, the
deliberate divergences -- stay in `CLAUDE.md`; the full derivations and
measurement tables stay in `docs/DESIGN.md` (look up a specific section,
do not read the whole thing).

- **The three-way merge (`sg_merge_content`, `src/workdir/merge.c`) builds a
  region list and post-processes it, it does not append bytes as it
  classifies** (Phase 41). Order is fixed and is git's: sync-point
  classification -> `refine_conflicts` -> `simplify_conflicts` ->
  `emit_regions`. When touching any of it, a green `make test` **does not
  count**: run `python3 tests/fuzz_merge.py 200` AND
  `python3 tests/fuzz_merge.py 200 --no-newline-edits` (real git is the
  oracle) and report both counts.
  Baseline for both is **0**, measured at 200 rounds x 4 seed ranges after
  Phase 42 closed the algorithm gap.
  WARNING: **`git merge` defaults to the HISTOGRAM algorithm while `git diff`
  and `git merge-file` default to Myers** (measured, git 2.55.0; `git merge`
  also honours `diff.algorithm`, which is how the default was established).
  So `src/workdir/merge.c` passes `SG_DIFF_ALGO_HISTOGRAM` at **both** of its
  `sg_diff_build_script` call sites -- `script_matches` AND
  `refine_conflicts` -- while `diff_out.c` passes Myers unless the user wrote
  `--histogram`. Changing only one of merge's two leaves the merge path
  internally inconsistent, with nothing failing to say so.
  **Never use `git merge-file` as the only oracle for `sg merge`**: its
  default is Myers, so it would call a whole class of divergence green.
  `python3 tests/fuzz_merge.py --attribute <keep-dir>` runs both oracles and
  labels each saved case `[algo]` (git's merge algorithm default), `[3way]`
  (sg's sync-point layer) or `[align]` (the aligner itself). All three
  buckets should now be empty; a non-empty one needs attributing before it
  is accepted.
  WARNING: **line comparison here is has_nl-AWARE**
  (`sg_diff_lines_equal_exact`), on both the alignment and the span
  comparison. It was has_nl-blind until Phase 41, and that one choice meant
  "ours' only edit was removing the trailing newline" read as "ours changed
  nothing": theirs was taken, the user's edit was discarded, and the merge
  reported SUCCESS. 29.5% of fuzz rounds mismatched real git because of it.
  WARNING: **a line with `has_nl == 0` is the last line of ITS file, but the
  merged output interleaves three files**, so it can still be followed by
  something. `bytebuf_ensure_nl` terminates it (git does the same in
  `xdl_recs_copy`); without it the output contains a line that appears in
  none of the three inputs, e.g. `base14=======`.
  WARNING: **an EMPTY region must not even terminate the previous line.**
  The sync-point pass pushes a zero-length region after the final anchor as
  a matter of course, so calling `bytebuf_ensure_nl` before writing nothing
  gives a file that legitimately ends without a newline one it never had --
  including on a merge that resolved to "unchanged", i.e. rewriting a file
  it did not merge. `tests/fuzz_merge.py` was blind to this for a whole
  review round: an anchor is always a BASE line and only a file's own last
  line can carry `has_nl == 0`, so while `gen_base` newline-terminated every
  line the shape was unreachable and 0/200 meant nothing about it. The
  generator now strips base's trailing newline 15% of the time; with the
  guard reverted the old generator still reports 0/200 and the new one
  reports 20/200.
  WARNING: **the empty-region guard asks about the side being PRINTED**
  (`from == to` after `take_theirs` is applied), not about the ours side.
  For a one-sided pure insertion the two questions differ, and asking the
  wrong one drops the inserted run entirely rather than misplacing a
  newline. Both directions are pinned by their own fixture; before those
  existed all 13 named tests stayed green under that mutation while
  `fuzz_merge.py` caught it 54/200 -- probabilistic coverage, no witness.
  WARNING: **two conflicts separated by at most 3 identical lines print as
  ONE conflict** (`SG_MERGE_CONFLICT_GAP`, git's
  `xdl_simplify_non_conflicts`; measured: 0-3 merge, 4 splits) -- **but a
  ONE-SIDED change inside the gap blocks the merge however short it is**,
  which is why regions carry a `REGION_RESOLVED` kind distinct from
  `REGION_SAME`. A distance-only rule passes the gap-3 test and fails the
  resolved-change one; both are in `tests/test_merge_content.c` as a pair.
  WARNING: **the emphasis on ONE-SIDED is load-bearing (Phase 50): a span
  BOTH sides changed the SAME way is `REGION_SAME`, not `REGION_RESOLVED`.**
  It does not block, and it contributes its length **in OURS' lines** as
  distance. Measured over 6 gap kinds x 7 widths, and one rule explains all
  38 rows: both-deleted reaches a gap one wider than both-edited precisely
  because a both-sided deletion is 0 ours lines long. This is also why the
  gap is counted in ours' coordinates and not base's -- git's own
  `xdl_simplify_non_conflicts` measures between `xdmerge_t`'s OURS fields.
  `REGION_SAME` therefore means "identical on both sides", **never** "equal
  to base": `refine_conflicts` has always hoisted agreed text out of a
  conflict as `REGION_SAME` while it differs from base, and the classifier
  was the one place that read it the other way (that was `--seed 9058`,
  Phase 49's one known non-zero, and its recorded description called the
  deletion one-sided when it is two-sided).
  WARNING: the one-sided rows agree **trivially** below the threshold -- a
  one-sided change leaves no anchor, so conflict/gap/conflict is already a
  single span between sync points and never reaches the simplify pass. A
  fixture at n <= 2 proves nothing about this rule.
  WARNING: **a conflict's two sides are diffed against EACH OTHER and what
  they agree on is hoisted out of the conflict** (git's
  `xdl_refine_conflict`), and this runs BEFORE simplification, not after --
  refinement splits conflicts, simplification then decides which pieces are
  too close together to keep apart. Reversing them changes the answer.
  WARNING: **do not write down that sg's three-way merge is a port of
  `xdl_merge`.** The sync-point layer underneath these two passes is sg's
  own design. It agrees with git on 800 fuzz rounds; that is a measurement,
  not an equivalence.
- **`sg_merge_trees` detects renames since Phase 49**, and `rename_score` is a
  **mandatory parameter with no default** (`SG_SIMILARITY_MAX` scale; 0 = off
  and reproduces pre-Phase-49 behaviour byte for byte). **All SIX call sites
  pass `SG_SIMILARITY_DEFAULT`** -- `cmd_merge.c`, `cmd_rebase.c`,
  `safety/stash.c`'s two, and (Phase 57) `src/cli/pick.c`'s single
  `attempt_one` call site, shared by both `sg cherry-pick` and (Phase 57b)
  `sg revert` -- because real git is rename-aware in every one of those
  commands. Measured: merge's threshold is exactly `git diff`'s 50%, the
  comparison is `score >= 50%`, and both tools flip at the same input
  (`R050` detected, `R049` missed), so **do not introduce a second constant**.
  **The machinery it reuses is documented in `docs/RULES-pathspec-rename.md`,
  not here** -- the three detection passes and their observable ORDER, the
  `-M<n>` fraction grammar, the 0..60000 score scale, and the `uses`-vs-`used`
  bug: changing any of those changes merge's answer, and changing merge's
  rename handling without reading them (or the reverse) is the one direction
  this split made easy to get wrong.
  Detection itself is not new code: `sg_diff_trees` + `sg_diff_detect_renames`
  are reused as-is, which works only because a tree-vs-tree list is all
  `SG_DIFF_SIDE_BLOB` and `sg_diff_side_read`'s BLOB branch never touches
  `repo_root`.
  WARNING: **the surviving path is the non-base name and ALL THREE stages
  move to it** -- stage 1 is not left at the old name (measured in both
  rename directions). The **only** exception is rename/rename-1to2, where the
  three stages sit at three different paths (`DD`/`AU`/`UA`).
  WARNING: **rename/rename-1to2's stage 2 and stage 3 hold the SAME blob, and
  that blob is the merge RESULT -- conflict markers included.** So this is
  the one conflict shape that must WRITE a blob to the object store.
  `merge_blob_content` deliberately leaves `*out_sha1` untouched on a
  conflict (an ordinary conflict has no resolved blob), so forgetting this
  copies an **uninitialized stack array** into both stages and the index
  names objects that do not exist. All four gates were green for exactly
  that, because interop's phase45 fixture is a PURE rename whose inner merge
  takes the clean branch.
  WARNING: **both of those stages carry the ordinary MERGED mode, not ours'
  own** -- measured in both directions (base 644 / ours 755 / theirs 644, and
  base 644 / ours 644 / theirs 755, both give 100755 at stage 2 AND stage 3).
  "Use ours' mode" passes the first fixture and fails the second, which is
  why `test_rename_rename_1to2_merged_mode` runs the pair.
  WARNING: **a landing entry's `ours_present`/`ours_mode`/`ours_sha1`
  describe the DESTINATION path, not the rename's source.**
  `sg_merge_entry_touches_ours` reads exactly those to decide whether
  `sg_merge_result_apply` may SKIP the write, so filling them from the source
  makes it answer "ours already has this here" for a path ours does not have
  at all -- and the renamed-to file is never created, while the merge
  commit's tree stays correct. Only a working-tree assertion can see this.
  WARNING: **the merge OPERANDS must be swapped by `is_ours` in step with the
  marker labels.** In `emit_standalone_landing` and `compute_kept_landing`,
  `side_e` is the RENAMING side -- which is THEIRS when `is_ours == 0`. Feeding
  it to `merge_blob_content`'s ours slot while the ours label correctly names
  ours puts each side's text under the other's marker. The merge still
  conflicts on the same lines and every index stage is still correct, so only
  the marker BODY shows it, and whoever resolves by hand keeps the wrong half.
  WARNING: **fixtures for this default to OURS doing the renaming, in every
  layer.** That is how the swap above survived a full green board plus a cold
  read: the unit tests, interop's `oneside_rev` (theirs renames, but with no
  edit, so the inner merge is clean and no markers are printed) and the
  fuzzer's `rename_edit` shape were all one-directional. The mirrored
  direction now exists in all three (`rename_edit_rev`, interop's
  `revconflict`, and a unit test asserting each side's text follows its OWN
  label -- not merely that both texts appear somewhere). Any new rename
  fixture needs the same question asked of it.
  WARNING: **a consumed rename source that the other side still has needs its
  own `deleted` entry** (`emit_consumed_source_deletion`), or the old file is
  left behind as an untracked leftover. This is a separate half from the
  previous WARNING -- fixing either alone leaves the other broken, so they
  have separate assertions and separate mutations.
  WARNING: the `:<path>` marker suffix's condition is **the two sides' own
  paths differing from each other**, not "a rename happened" -- both sides
  renaming to the same name gets no suffix. It needs no API change: labels
  reach `sg_merge_content` verbatim, so the caller composes them.
  WARNING: **`marker_size` is now a mandatory parameter of
  `sg_merge_content`**. 7 ordinarily; git widens to **8** for
  rename/rename-1to2 and for a rename whose destination collides, and when
  those nest the OUTER add/add stays 7 while the inner merge is 8.
  WARNING: **a rename destination colliding with an addition degrades to
  add/add with NO stage 1** (so does rename/rename-2to1) -- a different index
  shape from a content conflict, not a variant of it. Handling collisions is
  not optional: without it the rename unit and the ordinary walk both emit an
  entry for the destination, i.e. a duplicate index entry.
  **Deliberately not implemented: directory rename detection.** sg is
  byte-compatible with `merge.directoryRenames=false`; git's default
  (`conflict`) relocates a file added under a directory the other side
  renamed and reports `CONFLICT (file location)`. Also not implemented:
  `-X find-renames=<n>` / `-X no-renames` / `merge.renames` /
  `merge.renameLimit` (sg reads no config, `sg merge` has no `-X`), and git's
  stdout wording -- sg's merge messages were already a different vocabulary
  before this phase.
  WARNING: **`tests/fuzz_merge.py` is STRUCTURALLY blind to renames** -- its
  `build_repo` only ever writes one fixed filename, so no number of rounds
  can produce one; its 0/200 says nothing about this code. When touching
  rename-aware merging, run **`python3 tests/fuzz_merge_rename.py 150
  --max-failures 0`** across a few unused seed ranges and report the actual
  count. Baseline: **0 mismatches**. The git side runs with
  `-c merge.directoryRenames=false` pinned, because that config is exactly
  what sg's deliberate lack of directory rename detection is byte-compatible
  with -- declare the oracle's environment rather than skip rounds after the
  fact, since skipping discards the WHOLE round and would take any real
  rename bug in it along too. One known non-zero: **seed 9058**, a
  PRE-EXISTING content-merge divergence
  (reproduced on master) on a file no rename touches -- two adjacent
  conflicts that git merges across a two-line gap containing a one-sided
  deletion.
  WARNING: **rename/rename-2to1 is a control, not a discriminator.** Two
  different sources renamed onto one name give git an ordinary add/add
  whether or not it noticed the renames, so its 0-out-of-N is the correct
  answer and not a coverage gap. Do not "fix" the generator to make it fail.

## Phase 77: fast-forward's HEAD move can refuse on a foreign lock

`cmd_merge.c`'s fast-forward path's `sg_ref_move_head` call can now return
-1 for a NEW reason (a foreign `refs/heads/<branch>.lock` or `HEAD.lock`),
not just an I/O error -- the failure message uses
`sg_ref_lock_err_report(stderr, "HEAD")` first (git's own fast-forward
failure always names `'HEAD'`, even when the actual collision is on the
branch's own lock, since HEAD's ref transaction is what fast-forward
updates), falling back to the pre-existing generic message otherwise. The
3-way merge path's own `sg_ref_move_head` call is UNCHANGED (still the
generic message) -- best-effort per Phase 77's own spec, not required to
match git's exact multi-line wording.

## Phase 79: `sg merge` refuses a merge that would overwrite an untracked file

`cmd_merge.c` calls `sg_untracked_would_be_overwritten` (`include/sg/apply.h`)
on all THREE of its write paths -- 3-way, fast-forward, and the unborn-HEAD
fast-forward -- before anything is written. Every rule below was measured
against git 2.55.0.

- **There are TWO wordings, not one.** An ordinary merge (3-way AND
  fast-forward) gets the plural list form; an UNBORN HEAD gets a genuinely
  different sentence (`Untracked working tree file '<path>' would be
  overwritten by merge.`, singular, quoted path, trailing period, no
  "Please move or remove"/"Aborting" lines). A merge that happens to collide
  on exactly ONE path still gets the PLURAL form -- so the wording is
  selected by the CALL SITE and must never be inferred from the collision
  count.
  WARNING: **the unborn form names only the FIRST colliding path even when
  several collide** (measured: three collisions, git names `f0.txt` alone and
  exits 128). Printing all of them would be sg inventing output git does not
  produce.
- **Paths in the plural list are printed RAW** -- not through `sg_quote_path`
  or `sg_quote_path_delimited`. Measured: git prints a space, a double quote
  and UTF-8 verbatim there. Every OTHER path-printing site in `cmd_merge.c`
  quotes, so this one reads like an oversight and is not;
  `docs/RULES-paths-strings.md` carries the same warning from the other
  direction.
- WARNING (cold-read correction to this entry's own earlier text, which
  claimed the opposite of both): **the list is NEITHER sorted NOR
  de-duplicated by `sg_untracked_would_be_overwritten` itself.** Measured
  against git 2.55.0: two candidates blocked by the SAME ancestor (`topic`
  adds `a/b.txt` and `a/c.txt`, local untracked file `a`) print that
  ancestor's name TWICE, not once --
  `\ta\n\ta\n` -- so `sg_untracked_would_be_overwritten`'s own
  de-duplication loop was wrong and has been removed. And a plain
  alphabetical sort of the REPORTED names is also wrong: with `topic` adding
  both `a.txt` and `a/x.txt`, and the working tree holding an untracked
  `a.txt` (self-collision) AND an untracked file `a` (blocks `a/x.txt`),
  git prints `\ta.txt\n\ta\n` -- `a.txt` before `a`, the reverse of what
  `strcmp("a", "a.txt")` would sort them as. The list order is CANDIDATE
  order (the order `sg_tree_flatten` walks `theirs_tree`, i.e. git tree
  order: `.` 0x2E sorts before `/` 0x2F, so `a.txt` sorts before `a/x.txt`
  as a tree entry even though `a` alone -- the blocker's OWN name, not the
  candidate that lost to it -- would sort after `a.txt` as a plain string),
  never the byte order of the reported blocker names. Both of these apply
  identically to the directory bucket added by Phase 79b below.
- **An ignored file is NOT a collision: git silently overwrites it**
  (measured for a committed `.gitignore`, an uncommitted one, an ignored
  DIRECTORY, and `.git/info/exclude` -- all four merge cleanly). So this
  check cannot be built out of `lstat` alone, which is what
  `safety/stash.c`'s older pre-flight (`sg_stash_apply`) does -- **do not
  "converge" the two, they answer different questions.** The same applies to
  an ignored file BLOCKING a directory the merge must create.
- **A blocking ancestor is reported by ITS OWN name**: an untracked file at
  `dir` where the merge must create `dir/deep.txt` makes git print `dir`, and
  `a` blocking `a/b/c.txt` prints `a`. But a case- or normalization-aliased
  on-disk name is reported by the CALLER's spelling (`new.txt` even though
  `NEW.TXT` is what is on disk) -- which is why the check `lstat`s the
  candidate path and lets the filesystem answer the folding question instead
  of string-matching `sg_status_list_untracked`'s output. An exact compare
  against that list misses the aliased row, and that row is data loss on a
  macOS/APFS working tree.
- **An untracked EMPTY directory exactly at the path is not a collision**
  (git removes it), and an untracked directory holding unrelated files is not
  one either. Both are controls: a rule that treats "a directory is in the
  way" as a collision passes the blocker fixtures and fails these two.
- **`sg reset --hard` and `sg undo` must NOT get this check.** Measured: real
  git's `reset --hard` overwrites an untracked file without a word (exit 0),
  so sg already agrees with git there. That is why the check is a separate
  opt-in function instead of living inside `sg_safe_apply_tree`, which
  `sg switch`/`sg reset --hard`/`sg undo` all share. `sg switch`, `sg rebase`,
  `sg cherry-pick` and `sg revert` DO diverge (git refuses, sg overwrites) and
  are recorded as residuals, not fixed here.
- Ordering: the check runs AFTER `sg_require_clean_workdir` and AFTER
  `sg_cli_write_orig_head`. Measured, git writes `ORIG_HEAD` even when it then
  refuses -- for both the local-changes refusal and this one -- and a STAGED
  change wins over an untracked collision when both apply. Pinned in
  interop's `phase79d P-staged` row (Phase 79 round 3): a staged change to a
  tracked path the merge does not even touch still makes both git and sg
  refuse on the clean-workdir wording, never the untracked one.
  WARNING: **an UNSTAGED change to that same untouched path diverges, and
  this is PRE-EXISTING, not a new bug.** Measured: git's own dirty check for
  a merge only cares about paths the merge is actually going to write, so an
  unstaged edit to an unrelated tracked path does not trip it, and git falls
  through to report the untracked collision instead (its own wording, naming
  the untracked file). sg's `sg_require_clean_workdir` treats ANY unstaged
  change anywhere in the working directory as disqualifying and refuses
  before the untracked-overwrite check ever runs, so sg reports the
  clean-workdir wording where git reports the untracked one. Pinned as
  `phase79d P-unstaged` (both sides' literal first line, plus the
  precondition that the two wordings differ).

## Phase 79b: the DIRECTORY bucket of the same check

**Git has FOUR wordings here in total, not two.** Phase 79 above only
implemented the two FILE-bucket ones (a blocking FILE, or an ancestor
component that is a FILE). When the merge wants to write a FILE at a path P
where P itself is currently a NON-EMPTY untracked DIRECTORY, git uses a
DIFFERENT sentence, with its own ordinary/unborn pair, and
`sg_untracked_would_be_overwritten` now returns this as a SEPARATE bucket
(`out_dirs`/`out_dirs_count`, alongside `out_files`/`out_files_count`) so
`report_untracked_overwrite` can print the right wording for each.

- **This is a DIFFERENT shape from Phase 79's own row #6/#7/#9/#10**, which
  are all about a directory sitting as an ANCESTOR of the candidate path
  (`dir` blocking `dir/deep.txt`) -- those remain non-blocking for an
  unrelated/empty directory, unchanged by this phase. Phase 79b is about P
  ITSELF, when P is the exact path the merge wants to create as a file, and
  that path currently holds a directory.
- **A non-empty directory is only a collision if it recursively contains at
  least one file that is not ignored** -- measured: a directory whose entire
  contents (at any depth) are ignored merges through cleanly, and git's own
  answer there is not merely "no refusal", it REPLACES the local directory
  (and its ignored contents) entirely with theirs' file. An ignored
  SUBdirectory found during the scan is not descended into (its contents
  cannot surface through an ignored subtree). A symlink is treated as a file
  (never descended into).
- **The two ordinary wordings both end in a BLANK LINE before `Aborting`**,
  present even when there is no file-bucket section afterward:
  ```
  error: Updating the following directories would lose untracked files in them:
  \t<path>
  <blank line>
  Aborting
  ```
  (plus `Merge with strategy ort failed.` for a 3-way, matching the file
  bucket's own rule).
- **The unborn wording has NO trailing period** (the opposite of the file
  bucket's unborn form, which does), and is followed by git's own
  `fatal: read-tree failed` -- a second line sg deliberately does NOT
  reproduce, same treatment the file bucket's unborn case already gets:
  ```
  error: Updating '<path>' would lose untracked files in it
  fatal: read-tree failed
  ```
  The "only the first colliding path" rule for the unborn wording (Phase
  79's own WARNING) holds for this bucket too: first assumed by symmetry,
  then measured in round 2 (U2 in `docs/DESIGN.md`'s Phase 79c oracle
  table: two untracked directories under an unborn HEAD, git names only the
  first). Not pinned in interop: the single-collision unborn rows only
  confirm the wording and cannot tell "first only" from "all of them".
- **When BOTH buckets fire in the same merge**, measured byte-for-byte: the
  directory section prints first (ending in its own blank line), then the
  file section (its own independent `sg: ` line), then exactly ONE
  `Aborting`/strategy-failure pair for the WHOLE refusal, never one per
  bucket.
- **Phase 79c round 2 correction**: for the UNBORN-HEAD case with both
  buckets non-empty, `report_untracked_overwrite` no longer picks "the
  directory wording, always" -- that was an unmeasured assumption by
  symmetry, and it was WRONG. Measured against git 2.55.0 (U1b in the
  Phase 79c oracle, `docs/DESIGN.md`'s Phase 79c section): git reports
  exactly the FIRST collision in CANDIDATE order across BOTH buckets, using
  that collision's own wording -- a topic that adds a file-colliding path
  before a directory-colliding one in candidate order refuses on the FILE
  wording, not the directory one. `sg_untracked_would_be_overwritten`'s
  `out_first_is_dir` out-param (`include/sg/apply.h`) is how the caller
  learns which bucket's `[0]` entry is the actually-first collision.
- **A path can only ever land in one bucket**, since X (the resolved
  blocker) is either a file/blocker-file or a non-empty directory, never
  both at the same time.
- Cold-read correction shared with Phase 79 above: neither bucket is sorted
  or de-duplicated by `sg_untracked_would_be_overwritten` itself -- see that
  section's own WARNING for the measured counterexamples (a blocker printed
  twice, and candidate order beating a byte sort of the reported names).
  Both interop rows for that correction happen to use the FILE bucket, since
  that is where the decisive fixture (`a.txt` vs `a/x.txt`) was found, but
  the underlying function makes no bucket-specific exception -- the
  directory bucket's own multi-entry row (`phase79b B2`) is pinned
  separately to confirm the same non-sorting behavior there.
- **Known residual, NOT this phase's responsibility to fix**: once the
  pre-flight correctly determines "no collision" for a non-empty,
  all-ignored directory, the underlying write (`sg_write_file_mkdirs`'s own
  `fopen()` call) still fails on a path that is currently a directory -- the
  SAME shape as Phase 79's own row #7 (empty directory) and row #10 (ignored
  blocking file) residuals. Pinned in interop as `phase79b B7`, asserting
  only that the pre-flight
  itself got the "no collision" answer right, not that the merge as a whole
  succeeds.

## Phase 79c round 2: further residuals, no behavior change

Same "not this phase's responsibility" class as the B7/row7/row10 residuals
above, measured against git 2.55.0 and pinned on both sides in interop's
`phase79c` group (S-numbering matches the oracle table in `docs/DESIGN.md`'s
Phase 79c section).

- **S3: a candidate directory holding only EMPTY subdirectories (no files at
  all, not even ignored ones)** is the SAME shape as row #7/row #10/B7 --
  the pre-flight correctly says "no collision" (there is nothing, ignored or
  not, anywhere in the recursive scan), but `sg_write_file_mkdirs`'s
  `fopen()` still cannot write a file at a path that is currently a
  directory. git replaces the whole thing cleanly (rc 0); sg prints
  `sg: failed to write "new.txt"`, exits 1, and the directory survives.
  Pinned as `phase79c S3`.
- **S4a/S4d: a candidate directory holding an ignored file plus JUNK that
  merely LOOKS like a `.git` entry (not a valid repository -- either a
  `.git/` directory containing only a `HEAD` file with no object store, or a
  `.git` FILE pointing at a nonexistent gitdir)** -- git recognizes this is
  not an actual repository and replaces the whole directory, junk included
  (rc 0). sg's recursive scan has no "is this actually a git repository"
  check at all: it just walks the directory's real on-disk contents and
  finds the junk `.git/HEAD` (or the `.git` file itself) is not ignored, so
  it reports a directory-bucket collision. This is the SAFE direction
  (over-refusal, not the data-loss direction a false "no collision" would
  be), and is deliberately NOT "fixed" to recognize valid-vs-junk `.git`
  entries -- doing so would only trade this residual for the S3 write
  failure above once the pre-flight agreed "no collision" (the directory
  still can't be written through). Pinned as `phase79c S4a`/`phase79c S4d`.
  Contrast with S4b/S4c (a REAL nested git repository, with or without a
  commit): git refuses there too, matching sg -- the divergence is
  specifically about JUNK that resembles `.git` without being a working
  repository, not about nested repositories in general.
- **S5a/b/c: a SYMLINK sitting exactly at the candidate path** (pointing at
  an empty directory, a non-empty directory, or nowhere at all) -- not a
  residual, a confirmation that the already-documented symlink rule holds:
  `sg_untracked_would_be_overwritten` treats a symlink as a FILE, never
  descending into whatever it points at, so both sides refuse via the FILE
  wording naming the symlink itself, and the symlink (and its target, if
  any) survive untouched. Pinned as `phase79c S5a`/`S5b`/`S5c`.
- **S2: a scan path longer than the OS path limit** (a ~600-level chain,
  measured on macOS where `PATH_MAX` is 1024). Both sides refuse with a
  `cannot lstat '<path>': File name too long` line and change nothing, but
  they are NOT byte-identical, and this is accepted: git first prints a
  cascade of `warning: unable to access ...` / `warning: could not open
  directory ...` lines, which sg does not reproduce; and the two name a
  DIFFERENT path, because git works with paths relative to the work tree
  while sg's scan builds absolute paths, so sg hits the limit sooner (the
  temporary repo's own prefix counts against it -- measured 951 bytes of
  path for sg vs 1025 for git in the same fixture). Not pinned in
  interop (the depth depends on the platform's `PATH_MAX` and on the repo's
  absolute location); `test_deep_recursion_stack_safety` covers only "fails
  closed, never crashes".

## Phase 80: the blocker walk no longer needs `lstat(P)` to fail ENOTDIR, and
## `sg_merge_result_apply` deletes before it creates

**F3(a): a blocking ancestor is now found by walking P's proper ancestors
UNCONDITIONALLY, shortest-first, not only when `lstat(P)` itself fails with
ENOTDIR.** The pre-Phase-80 code only ran the ancestor walk as a fallback
after `lstat(P)` failed ENOTDIR -- correct for an ordinary blocking file,
but wrong for a SYMLINK ancestor: `lstat` resolves every path component
except the FINAL one, so `lstat("a/b/c.txt")` where `a` is a symlink
follows `a` into whatever it points at and answers about THAT location
instead (ENOENT if the target has no `b/c.txt`, or even success if it
does) -- neither of which is ENOTDIR, so the old code silently read this as
"no collision" and let the write proceed straight through the symlink
(see `docs/RULES-paths-strings.md`'s Phase 80 entry for the write-side
half of this same fix). The walk now runs first, always, and its own
`lstat` on each ancestor is likewise never followed for that ancestor
itself, so a symlink ancestor is found regardless of what it points at or
whether the target exists at all. Measured against git 2.55.0: A4 (a
non-ignored symlink `a` blocking `a/b/c.txt`) now correctly refuses,
naming `a`, in the file bucket; A6 (same shape, but `a` points at a REAL
in-repo directory) also refuses naming `a`; A3/A7 (the ignored variants of
each) report no collision, matching git's own silent replacement. All
pre-Phase-80 rows (row #7/#8/#9/#10, B7, S2-S6) stay green under this
change -- the walk's ANSWER for an ordinary file blocker is unchanged, only
WHEN it runs changed.

**F3(b): the recursive directory scan (`untracked_overwrite_dir_has_
nonignored`, `workdir/apply.c`) now takes an optional `idx` parameter.**
When non-NULL, a path tracked at any stage is skipped the same way an
ignored one is (it does not make the scan report "found"), but unlike an
ignored directory it does not stop recursion into siblings -- a directory
can hold both a tracked file the SAME merge is about to delete and a
genuinely untracked one, and only the latter should block. This closes the
T3/T4 shape: a merge that deletes tracked `d/x.txt` and adds a plain file
`d` used to have its own pre-flight refuse, naming `d`, because the scan
found `d/x.txt` still sitting on disk and had no way to know it was about
to be removed by this same operation. `sg_untracked_would_be_overwritten`'s
own pre-flight caller passes its real `idx`; `sg_worktree_clear_write_path`
(see `docs/RULES-paths-strings.md`) always passes `NULL` -- see that
function's own header comment for why the exemption is deliberately
write-time-absent, not a second copy of the same rule with a different
answer.

**F4: `sg_merge_result_apply` (`src/workdir/merge.c`) now does every
deletion its result calls for BEFORE any write, in two full passes over
`result->entries` rather than one interleaved pass in result (path) order**
-- git's own `check_updates` order, and what `sg_apply_tree_to_workdir`
already did. The pre-Phase-80 single pass could write a new file at `d`
before deleting a tracked `d/x.txt` still sitting underneath it in the OLD
tree: the write failed outright (a directory can't be replaced by a file
until it's empty) while the tracked deletion had already gone through --
`sg merge`/`sg cherry-pick` left a HALF-APPLIED state, `d/x.txt` gone AND
`d` not created. Splitting into a deletion-only pass followed by the
existing write pass removes the ordering dependency: by the time any write
runs, everything this SAME merge result deletes is already gone, so a
directory a write needs to replace (via `sg_worktree_clear_write_path`) is
already clear of anything this merge itself removes. **Only the filesystem
side effect moved -- no stdout/stderr byte or index field changed for any
existing case**, since the deletion branch never printed anything and
never touched `index_out`; verify with `python3 tests/fuzz_merge.py 200`
(oracle: real git) after touching this function, same rule this file
already states for `sg_merge_content`. T3 (deleted file does not match any
ignore rule) and T4 (it does, a `.log` file) are the two oracle rows this
closes, both for the FF and 3-way merge paths and for `sg cherry-pick`
(which shares `run_todo`'s call into the same `sg_merge_result_apply`, see
`docs/RULES-sequencer.md`).

**Phase 80 fix round (cold-read finding 2): `conflict_no_workdir_file`'s
own removal (rename/rename-1to2's original-path entry, Phase 49) is ALSO
in the delete-first pass now, not left interleaved in the write pass by
path order.** It is semantically a delete (git leaves no file at the old
name) exactly like the plain `e->deleted` branch above, and leaving it in
the write pass meant F4's own claim ("every deletion runs before every
write") was false for this one entry kind -- a merge whose rename source
happens to sit where a DIFFERENT entry's write needs to land could still
half-apply. Both delete-pass branches now go through the SAME guarded
delete (`sg_remove_file_worktree`, see `docs/RULES-paths-strings.md`'s
delete-side entry) and the same escalation-on-failure discipline: a failed
delete sets `content_missing = 1` and prints `sg: cannot remove "<p>"`,
aborting the whole apply rather than being silently swallowed the way a
bare `remove()` failure used to be here.
