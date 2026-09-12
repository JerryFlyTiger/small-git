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
