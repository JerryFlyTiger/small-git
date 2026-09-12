# Rules: `sg cherry-pick`, `sg revert`, `sg rebase`

Scope: `src/cli/pick.c`, `src/cli/cmd_cherry_pick.c`, `src/cli/cmd_revert.c`, `src/cli/cmd_rebase.c`, `src/safety/sequencer.c`, `src/safety/rebase.c`, `include/sg/pick.h`, `include/sg/sequencer.h`.

**Read this file before editing anything in that scope.** It is not
background reading: every rule here was measured against real git 2.55.0,
and most of the `WARNING:`s exist because something slipped past a fully
green board. Project-wide rules -- the gates, the code conventions, the
deliberate divergences -- stay in `CLAUDE.md`; the full derivations and
measurement tables stay in `docs/DESIGN.md` (look up a specific section,
do not read the whole thing).

- **`sg cherry-pick` exists as of Phase 57** (`include/sg/sequencer.h` +
  `src/safety/sequencer.c` for the on-disk state, `include/sg/pick.h` +
  `src/cli/pick.c` for the shared replay engine, `src/cli/cmd_cherry_pick.c`
  as a thin argument-parsing shell). `sg revert` (Phase 57b,
  `src/cli/cmd_revert.c`, byte-for-byte parallel to `cmd_cherry_pick.c`
  except for usage text and which `sg_seq_kind` it passes down) shares this
  same engine via the `sg_seq_kind` parameter -- see `include/sg/pick.h`'s
  own header comment before touching either command. Revert's own message
  rules (section 4.3's 7-row Reapply table below) are unit-tested in
  `tests/test_revert_message.c` as exact byte strings, not `strstr`
  substring checks, driven through the real `sg_pick_start` entry point
  rather than by exposing `pick.c`'s file-local message builders.
  WARNING (found while extending the `docs/sg.1` `FILES` entry for
  `REVERT_HEAD` in Phase 57b): the entry's prose had ALREADY drifted from
  the "one-directional, no 'vice versa'" rule two paragraphs below, despite
  that rule being written down correctly right here in the same commit that
  introduced the drift -- CLAUDE.md being right is not evidence the other
  docs agree with it. Fixed in the same edit; if `docs/sg.1`'s `FILES`
  section is touched again, re-check it against this same paragraph rather
  than assuming it was already correct.
  WARNING: **the state format deliberately MIRRORS git's own** (`CHERRY_
  PICK_HEAD`/`REVERT_HEAD`/`MERGE_MSG`/`sequencer/{head,abort-safety,todo}`,
  directly under `git_dir`) -- the OPPOSITE decision from `sg-rebase/`,
  which is deliberately sg's own incompatible namespace. The two are not in
  tension: `sg-rebase/` protects against a real git binary MISINTERPRETING
  state that means something different to it (rebase there runs permanently
  detached, a mechanism git's own `rebase-merge/` does not expect), while
  cherry-pick's mechanism (`sg_ref_move_head`, moving the branch/HEAD one
  commit per pick) is the SAME one git uses -- there is nothing to protect
  against, and mirroring buys real interop.
  WARNING: **that interop was ONE-DIRECTIONAL until Phase 68c, and is now
  BIDIRECTIONAL -- but the sentence `CLAUDE.md` used to carry ("and vice
  versa is wrong, do not write it back in") was CORRECT when written, and
  the reason it stopped being correct is a code change, not a
  re-measurement.** Phase 57 measured: sg pauses -> a real `git cherry-pick
  --continue` finishes it; git pauses -> `sg cherry-pick --continue` (or
  `--skip`) could NOT, because git's `sequencer/todo` writes an abbreviated
  7-hex id and sg had no abbreviated-object-name resolution to read it back
  with. **Phase 68 gave sg that resolution and Phase 68c widened
  `parse_todo_line`, so both directions now work** -- measured end to end
  against git 2.55.0, and pinned in interop's `phase68c` group in BOTH
  directions. Two Phase 57 facts survive unchanged and must not be
  "simplified" away now that the dead end is gone: `--abort` and `--quit`
  still deliberately parse NO todo at all (they read only
  `sequencer/head`/`abort-safety`, plain 40-hex in either tool), which is
  what keeps them working when the todo is damaged or genuinely ambiguous;
  and an ambiguous abbreviation is REFUSED, never guessed. See Phase 57 and
  Phase 68c of `docs/DESIGN.md`.
  WARNING: **`sequencer/` itself (not just its contents) is conditional on
  commit COUNT, not on any per-call flag** -- a SINGLE-commit conflicting
  `sg cherry-pick <c>` writes `CHERRY_PICK_HEAD`/`MERGE_MSG` and creates NO
  `sequencer/` directory at all (measured against git 2.55.0); only two or
  more commits create it. `sg_sequencer_state.has_sequence` carries this,
  and `--continue`/`--skip`/`--abort` must all branch on it -- a corrupt
  assumption that `sequencer/` always exists would make every one of those
  refuse on a perfectly normal single-commit pause.
  WARNING: **`-n`/`--no-commit` on a CLEAN pick still writes `MERGE_MSG`**
  (just the message, no `# Conflicts:` block -- there was no conflict) --
  measured, and a genuine gap in this phase's original spec (never
  mentioned there at all, found by the same post-green-board oracle run).
  An ordinary clean pick with no `-n` writes NO `MERGE_MSG` at all. Neither
  `CHERRY_PICK_HEAD` nor `sequencer/` is written for `-n`, so it must not
  count as "in progress" (`sg_sequencer_kind_in_progress` stays 0).
  `run_todo`'s `ATTEMPT_CLEAN_NO_COMMIT` branch calls
  `sg_sequencer_write_merge_msg` ALONE, with no accompanying
  `sg_sequencer_state_write` -- that asymmetry (one write, not the other)
  is the whole fix, do not add the state write "for consistency".
  WARNING: **`sequencer/todo`'s id field is a full 40-hex where git writes
  an abbreviated 7-hex, and this is STILL deliberate as of Phase 68c -- but
  for a different reason than the one `CLAUDE.md` used to give.** The old
  reason ("sg cannot read a short one back") expired when Phase 68c widened
  `parse_todo_line`; the standing reason is **wide-in / narrow-out**: sg
  ACCEPTS 4..40 hex (so it can finish a sequence a real git binary paused)
  and EMITS only the unambiguous full 40, which no reader -- git, an older
  sg, or a future one -- can ever resolve to the wrong object. Full hex is
  readable BY GIT either way (its own parser accepts any length-4-or-more
  prefix). Pinned on both sides in interop's `phase57`/`phase68c` groups.
  **Do not "fix" this to 7 characters**, and do not restore the old
  justification.
  WARNING: **the reflog wording has one asymmetry that looks like a typo and
  is not**: a DIRECTLY-applied pick logs `cherry-pick: <subject>` /
  `revert: <subject>`; a pick finished by `--continue` logs `commit
  (cherry-pick): <subject>` for cherry-pick but bare `commit: <subject>`
  (Phase 57b, no `(revert)` suffix at all) for revert -- both measured
  against git 2.55.0. Reproduce the asymmetry; interop's `phase57`/`phase57b`
  groups pin the two as a head-on pair specifically so "unifying" them turns
  a check red by name.
  WARNING: **`--abort` and `--skip` do NOT use cherry-pick-specific reflog
  wording -- both are, underneath, a plain `reset --hard`, and log exactly
  what `git reset --hard` logs: `reset: moving to <40hex>`, nothing else**
  (measured; a genuinely wrong earlier draft of this phase's own spec
  invented `cherry-pick (abort): returning to <hex>` here without checking
  real git first, caught by a one-off oracle harness run after the first
  green board). `--skip`'s target is wherever HEAD already is (a no-op --
  nothing moves), which is exactly the shape the two asymmetric reflog
  rules two bullets up already produce for free through a single
  `sg_ref_move_head` call with the target equal to the CURRENT value -- no
  hand-written reflog line is needed for either subcommand. **This is the
  OPPOSITE convention from `sg rebase --abort`**, which correctly keeps its
  own `rebase (abort): returning to ...` wording (rebase's abort genuinely
  is not a plain reset in real git, it goes through a detached-HEAD replay
  sg-rebase's own sequencer owns) -- do not "unify" the two, interop pins
  both wordings as a head-on pair specifically so a future harmonizing edit
  turns a check red by name.
  WARNING: **`--quit` PARSES NOTHING AND CHECKS NOTHING -- not even the
  in-progress kind** (spec 5b, tightened after a second measurement round
  found the first fix still insufficient -- see the dead-end WARNING
  below). Existence alone (`sg_sequencer_kind_in_progress`) decides whether
  there is anything to do; if so, `sg_sequencer_state_remove` runs
  unconditionally, REGARDLESS of whether the paused kind matches the
  invoked subcommand (`sg cherry-pick --quit` removes a paused REVERT too)
  and regardless of whether `sequencer/todo` or anything else parses. This
  is deliberate: --quit is the ONE escape hatch specified to have literally
  no failing input, so it must not gain a single new precondition, ever.
  With NOTHING in progress it exits 0 and prints nothing at all --
  `--continue`/`--skip`/`--abort` DO refuse there with
  `sg: no cherry-pick in progress` and exit 1; --quit's own job ("remove
  the paused state if there is one") is already satisfied by a clean
  repository, so refusing would report an error for a request that already
  trivially succeeded.
  WARNING: **a second, independent measurement round (after the first
  green board) found the dead end this whole design exists to prevent, and
  it is worth understanding exactly what triggered it.** On a repo where a
  REAL GIT binary paused a two-commit cherry-pick, sg refused ALL FOUR of
  `--continue`/`--skip`/`--quit`/`--abort`, every one with the identical
  `sg: cherry-pick state is corrupt, run sg cherry-pick --abort to clean
  up` -- advice naming one of the four commands that had just failed for
  the same reason. Root cause: `sg_sequencer_state_read`'s contract is
  all-or-nothing (every field, including `sequencer/todo`, must parse or
  the whole read fails), and at the time all four subcommands went through
  it via the shared `require_state` helper. git's `sequencer/todo` holds
  ABBREVIATED 7-hex ids (the divergence two bullets up), which sg's todo
  parser -- fixed-width, expecting 40 -- cannot read at all, so the read
  failed and every subcommand refused. **The trigger is not git-specific**:
  any damaged `sequencer/todo` reaches the identical dead end -- a partial
  write, or a full disk during this phase's own per-step state persistence
   (`sg_sequencer_state_write`, called after every todo entry). `sg switch`/
  `commit`/`merge` were all correctly blocked at the same time (they ask
  `sg_sequencer_kind_in_progress`, existence only, never parseability), so
  the repository had literally no exit short of hand-deleting `.git`
  files -- the exact shape CLAUDE.md already documents for
  `sg_merge_head_read` as a gate predicate, reproduced one layer up because
  the ESCAPE HATCHES themselves, not just the gates, depended on a full
  parse. The fix is `sg_sequencer_abort_target` (reads only
  `sequencer/head`/`sequencer/abort-safety`, both a plain full 40-hex in
  either tool's writing, never `sequencer/todo`) for `--abort`, and the
  now-parses-nothing `--quit` above; `--continue`/`--skip` still need the
  todo for their real job and may still refuse, but ONLY with a message
  naming a command verified to actually work (`--abort`, now that it no
  longer depends on `sequencer/todo` either). **The general rule: an escape
  hatch, or a status line, must never depend on a fully parseable state --
  only on existence, with a graceful degrade for any detail beyond that.**
  WARNING: **`--continue` re-derives its commit message by READING AND
  STRIPPING `MERGE_MSG`, not by recomputing anything** -- `sequencer/opts`
  is deliberately never written (the only option this phase could need to
  persist, `-m`, is rejected outright for more than one commit), so there is
  nothing to recompute FROM. `MERGE_MSG` already holds the exact message
  (cherry-pick: the picked commit's own, byte for byte; revert: built once
  through the Reapply/Revert rules below) computed at the moment the
  sequence stopped; `--continue` reads it back and cuts everything from
  `"\n# Conflicts:\n"` onward if present. Do not add a second message-
  construction call site for the resume path.
  WARNING: **the truncation point for that strip is `marker` itself, not
  `marker + 1`, and getting this one byte wrong is a REAL bug, not a
  cosmetic one.** `MERGE_MSG`'s format (section 2.2 above) is `<message
  ending in exactly one \n>\n# Conflicts:\n...` -- the SEPARATOR `\n`
  belongs to the `# Conflicts:` block, not to the message, so
  `strstr(..., "\n# Conflicts:\n")` returns a pointer AT that separator
  byte, and truncating there drops it along with everything after. An
  earlier version of this code truncated one byte later (kept the
  separator), which meant every `--continue`'d commit's message came out as
  `"<message>\n\n"` instead of `"<message>\n"` -- **a different message
  is a different object id**, so this was not a wording nit, it was the
  wrong commit. Caught by a review, not by either gate: `make sanitize` and
  the ordinary `interop.sh` group both stayed green over it (an extra
  trailing `\n` is well-formed UTF-8/ASCII either way), only a byte-for-
  byte commit-object comparison against real git catches it. Both a unit
  test (`tests/test_pick_engine.c`'s
  `test_continue_message_has_no_extra_blank_line`) and an interop check
  guard this now; both were reverse-mutated (the one-byte fix undone) and
  confirmed red before being trusted.
  WARNING: **revert's `Reapply`/`Revert` subject rule has no closing-quote
  check and no balance check, and three of its seven measured rows are
  controls that exist specifically to kill a looser implementation**: a
  case-insensitive match, a whitespace-tolerant match, and "treat anything
  already looking like a revert as already reapplied" are each falsified by
  one of `Reapply "x y"` -> `Revert "Reapply "x y""`, `revert "lower"` ->
  `Revert "revert "lower""`, and `Revert  "two spaces"` (two spaces) ->
  `Revert "Revert  "two spaces""`. Only the literal 8-byte prefix `Revert "`
  (exact case, exactly one space) triggers the swap to `Reapply "`. See
  Phase 57 spec section 4.3 / `docs/DESIGN.md` for the full table.
  WARNING: **a revert's conflict-marker "theirs" label carries a
  revert-only `"parent of "` prefix that cherry-pick's does not** (found in
  Phase 57b's review round 2, alongside fixing the fixed-300-byte buffer
  that silently truncated it past 300 bytes -- both live in the same
  `attempt_one` `snprintf(NULL, 0, ...)` + `malloc` construction in
  `src/cli/pick.c`, gated on `kind == SG_SEQ_REVERT`). Measured on both a
  single-parent revert and a `-m 1` merge revert: git always prints
  `>>>>>>> parent of <7hex> (<subject>)` for a revert conflict, never the
  bare `<7hex> (<subject>)` cherry-pick uses. This had been wrong since
  Phase 57a wrote the shared `theirs_label` construction with no `kind`
  branch in it at all, and nothing caught it because no check before this
  round ever compared a revert conflict's marker BYTES against real git
  (the pre-existing `REVERT_HEAD`/`MERGE_MSG` checks don't read the
  working-tree file's conflict markers). See Phase 57b's DESIGN.md section
  6 for both the truncation and the prefix bug, and do not "simplify" the
  label construction back to one shared format string.
  WARNING: **the gate convergence list is long and every site matters, and
  `cmd_rebase.c`'s OWN start gate is ON this list, not exempt from it.**
  This project's own Phase 57 spec originally said "every call site of
  `sg_rebase_state_exists` OUTSIDE `cmd_rebase.c`/`safety/rebase.c`" --
  which reads naturally as "rebase doesn't need to check itself", and is
  backwards: `do_rebase_start` needs to ask `sg_sequencer_kind_in_progress`
  same as every other start gate, because a REBASE START is precisely where
  a competing paused state needs to be refused. Measured, reproduced
  directly: before this was fixed, `sg rebase <upstream>` started cleanly
  over a paused cherry-pick, overwrote the user's unresolved conflict
  content with rebase's own conflict markers, and never cleared
  `CHERRY_PICK_HEAD` -- `sg status` then printed two mutually contradictory
  in-progress banners at once. The full list: `src/workdir/apply.c` (the
  dirty-workdir gate, two spots), `cmd_reset.c` (`--soft` refusal),
  `cmd_merge.c` (refuse starting a merge on top of a stopped pick),
  `cmd_switch.c` (refuse switching branches), `cmd_rebase.c`'s own
  `do_rebase_start` (refuse starting a rebase on top of a stopped pick --
  see the WARNING above about the spec wording that carved this one out by
  mistake), `cmd_commit.c` (divergence #5 in `CLAUDE.md`), `cmd_stash.c`
  (both the push-time warning and the apply/pop gate), and `cmd_undo.c`
  (the ONE caller allowed to clear it directly, same sole-exception rule as
  rebase state). Leaving one unconverged is not "unchanged behaviour" --
  CLAUDE.md's own converging-a-predicate lesson applies here unchanged: an
  unconverged site is a new dead end, not a no-op. **This list had ZERO
  automated coverage from the "call the OTHER command and watch it refuse"
  direction until a review added one interop check per site** -- reading
  the code and judging it converged is exactly how the missing
  `cmd_rebase.c` site went unnoticed through a full green board; a check
  per site (not one check covering several sites, or a mutation on one site
  can hide inside a pass caused by another) is what actually verifies
  convergence. Two traps measured while adding that coverage:
  `cmd_stash.c`'s `sg_index_has_unmerged` check runs BEFORE its cherry-pick
  gate, so an UNRESOLVED conflict (which a fresh cherry-pick conflict always
  is) hits that older, shared refusal first and never reaches the gate this
  phase added -- the fixture has to resolve and stage the conflict first,
  which the sequence still correctly counts as "in progress"; and
  `cmd_switch.c`'s own gate, mutated to always-false, stayed GREEN under a
  plain `switch <branch>` probe, because `switch` never performs any side
  effect (including a `-c`-created branch) until AFTER `apply.c`'s own
  dirty-workdir check would ALSO have refused -- the two are only
  distinguishable by `--force`, which bypasses a CONFIRMATION (`apply.c`'s)
  but not an unconditional refusal (`cmd_switch.c`'s own).
  WARNING: **`-m` is rejected outright whenever more than one commit is
  requested** (`sg: -m is only supported with a single commit`, exit 1,
  same shape as `-n`'s own rejection one bullet up) -- a review found the
  alternative (threading `mainline` through a resumed `--continue`/
  `--skip`) has no home to persist to: `sequencer/opts` is deliberately
  never written, and a REAL merge-commit-with-`-m` sequence that stops on
  one merge commit and later needs `mainline` for a DIFFERENT merge commit
  still in the todo has no way to recover it once `run_todo`'s
  continue/skip call sites (which pass `mainline=0`) take over. This closes
  the gap completely: a single-commit `-m` pick's `state.todo_count` is
  exactly 1 after a conflict, so `sg_pick_continue`/`_skip` never call
  `run_todo` for it at all, making the hardcoded `mainline=0` there
  provably unreachable wherever it would matter, not merely unlikely.
  WARNING: **every exit from the todo loop -- conflict, empty result, AND
  an internal error -- must leave `abort_safety` on disk equal to the REAL
  current HEAD, not just the first two.** A review found the `ATTEMPT_ERROR`
  branch was the one exception: it printed a message and returned without
  refreshing state, so a mid-sequence I/O failure (or the mainline bug
  above, before it was closed) left a STALE `abort_safety` behind whenever
  an earlier todo entry in the same call had already committed cleanly --
  and `--abort` then wrongly refused with "HEAD has moved since the pick
  stopped", because the pick's OWN machinery is what moved it. Fixed by
  extracting `write_stop`'s state-only half into `write_stop_state` (no
  `MERGE_MSG` -- an internal error has no user-facing message worth
  writing) and calling it from `ATTEMPT_ERROR` too, gated on `has_sequence`
  (a single-commit pick's only possible `ATTEMPT_ERROR` is at `idx == 0`,
  before this call could have moved HEAD, so there is nothing stale to
  refresh there).
  WARNING: **`sg status`'s cherry-pick/revert banner reuses Phase 38's
  resolved-merge closing-line-suppression condition, it does not add a
  second one** -- find `merge_in_progress && unmerged_count == 0` in
  `cmd_status.c` and extend it to `(merge_in_progress || seq_kind != 0) &&
  unmerged_count == 0`; the filtered `unmerged_count` this reuses is the
  SAME variable the merge banner already shares with `print_unmerged`
  (Phase 38 Bug A) -- do not compute a second, unfiltered count for the new
  banner.
  WARNING: **the banner block prints on `seq_kind != 0` (existence) alone,
  never on a successful full-state read** -- the 7-hex commit name inside
  it comes from `sg_sequencer_current_commit`, a SEPARATE, minimal read of
  just `CHERRY_PICK_HEAD`/`REVERT_HEAD`'s own single hex line, independent
  of `sequencer/`'s existence or parseability; if even that fails (rare),
  the banner degrades to a detail-free `You are currently cherry-picking.`
  rather than disappearing. Measured symptom of gating the whole block on
  a full parse instead: the hint line `(use "sg cherry-pick --abort" to
  cancel...)` printed while the banner line `You are currently
  cherry-picking commit <7hex>.` above it vanished entirely -- status KNEW
  a pick was in progress (it prints hints for it) but would not SAY so,
  the identical banner/other-half disagreement as Phase 38's bug A, one
  layer up.
  WARNING: **the deliberate divergence list gained a fifth entry**: `sg
  commit` is blocked while a cherry-pick/revert is stopped where real git
  lets it finish the pick. See `CLAUDE.md`'s numbered divergence list.
- **`sg rebase <upstream>` takes any revision since Phase 69** (tag,
  `refs/...` path, `~N`/`^N`/`@{N}`, 40-hex, abbreviated 4..39-hex), not
  just a bare branch name -- `do_rebase_start` (`cmd_rebase.c`) now calls
  `sg_rev_parse_commit_ex(..., SG_REV_COMMITTISH, ...)`, the same template
  as `cmd_reset.c`'s `<rev>` resolution, **not** `cmd_merge.c`'s (which is
  STRICT). See `docs/RULES-refs-revparse.md`'s COMMITTISH-callers bullet for
  why: git resolves a
  prefix with exactly one commit-ish candidate for `rebase` the same way it
  does for `log`/`reset --hard`, and refuses it for `merge`/`show`/
  `cherry-pick`/`revert`/`switch --detach`. `sg_cli_report_ambiguous_oid`'s
  disambig argument is COMMITTISH too, so an ambiguous prefix narrows its
  hint list the same way. **None of the sequencer state or reflog wording
  needed to change** -- `.git/sg-rebase/onto` already stored the RESOLVED
  40-hex commit id (never the typed spelling), and git's own
  `rebase (start): checkout <arg>` reflog convention (which sg already
  matched) echoes the argument as typed, unpeeled and unexpanded, so a tag
  or abbreviated hex flows through unchanged. Also gained (independent
  commit, Phase 69b): an unrecognized `-`-prefixed argument is rejected
  with the usage line rather than falling into the `<upstream>` slot (same
  idiom as `cmd_reset.c`'s own guard) -- `sg rebase --help` used to be
  reported as `sg: invalid reference: --help`.
  WARNING: **a bare `-` (git's `@{-1}` shorthand) is refused by BOTH
  spellings sg can reach it through** -- sg's revparse grammar has no
  `@{-1}` at all, so `sg rebase -` was already going to fail before Phase
  69b's flag guard; the guard only changes which error message rejects it,
  a pre-existing grammar gap recorded here rather than "fixed".
- **`sg rebase --quit` exists as of Phase 58** -- the same escape-hatch
  shape as `sg_pick_quit` above, ported to rebase because rebase had the
  exact same dead end: with `.git/sg-rebase/current`, `onto`, or
  `orig-branch` damaged, all three of `--continue`/`--skip`/`--abort`
  refuse (two of the three messages name `--abort`, which had itself just
  failed for the same reason), while `sg switch`/`sg commit` stay blocked
  (both gate on `sg_rebase_state_exists`, existence only), leaving no exit
  but hand-editing files under `.git/sg-rebase/`. Measured against real git
  2.55.0 first: `git rebase --abort`/`--skip`/`--continue` refuse
  IDENTICALLY on the same damaged `rebase-merge/onto`, and `git rebase
  --quit` is real git's own way out. **This is why the fix is a new
  subcommand, not loosened behavior on the existing three** -- loosening
  any of `--abort`/`--skip`/`--continue` would be sg inventing behavior
  real git does not have, the exact mistake Phase 57's own spec made twice
  (see the reflog-wording and exit-code WARNINGs above). `do_rebase_quit`
  (`src/cli/cmd_rebase.c`) is a five-line wrapper, same discipline as
  `sg_pick_quit`: it parses nothing, `sg_rebase_state_exists` (stat-only)
  decides, `sg_rebase_state_remove` (plain `remove()`-by-name) does the
  work, and nothing about it reads `onto`/`current`/`orig-branch`/`todo`.
  WARNING: **`--quit` does not move HEAD and does not touch the index or
  working tree** -- measured on a healthy paused rebase: HEAD stays
  detached exactly where the pause left it (never returned to the original
  branch), and a conflicted `UU` path is still `UU` afterward. Confusing
  this with cherry-pick's `--abort` (which DOES restore things) or with
  rebase's OWN `--abort` (which also restores) is the easy mistake -- among
  rebase's four subcommands, `--quit` is the only one that changes nothing
  but the state directory's existence.
  WARNING: **`sg status`'s rebase banner had the identical "vanishes on a
  damaged state" bug Phase 57 already fixed once for cherry-pick/revert**,
  this being the second time the same bug shape appeared in this codebase.
  `cmd_status.c` used to gate the whole rebase block on
  `sg_rebase_state_exists(...) && sg_rebase_state_read(...) == 0`; measured
  against real git, `git status` on a damaged `rebase-merge/onto` still
  prints its whole block, echoing back the value it could not resolve. The
  fix enters on existence alone and degrades to the detail-free `You are
  currently rebasing.` line (already used elsewhere for a NULL
  `orig_branch`) plus a hint naming `--quit` specifically -- never
  `--continue`/`--skip`/`--abort`, all three known to fail on this exact
  state (Phase 57's rule, needed a second time: an error must never name a
  command that fails on the same input). See Phase 58 in `docs/DESIGN.md`
  for the two fixture traps this uncovered while writing the interop
  group: the SEPARATE "Unmerged paths" section legitimately still names
  `--abort` on a real-conflict fixture (unrelated to this fix, and grepping
  whole-output for `--abort` would have failed for the wrong reason), and
  `sg switch`'s rebase gate is not the same gate as its dirty-worktree
  confirmation (`--force` bypasses only the latter), so proving `--quit`
  actually unblocks `switch` needs `--force` on a fixture whose conflict
  genuinely left the working tree dirty.

