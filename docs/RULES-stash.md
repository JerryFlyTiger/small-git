# Rules: `sg stash`

Scope: `src/safety/stash.c`, `src/cli/cmd_stash.c`, `include/sg/stash.h`.

**Read this file before editing anything in that scope.** It is not
background reading: every rule here was measured against real git 2.55.0,
and most of the `WARNING:`s exist because something slipped past a fully
green board. Project-wide rules -- the gates, the code conventions, the
deliberate divergences -- stay in `CLAUDE.md`; the full derivations and
measurement tables stay in `docs/DESIGN.md` (look up a specific section,
do not read the whole thing).

- **`sg stash show` builds on the diff foundation, it does not parse the
  stash commit itself** (Phase 25), and since Phase 31 it also takes
  `-M[<n>]`/`--find-renames[=<n>]`/`--no-renames`, sharing the one CLI-facing
  copy of the grammar, `sg_similarity_parse_score_arg`. **Do not add a second
  copy of that wrapper** -- it was extracted out of `cmd_diff.c` precisely so
  this command would not grow one. The four trees are resolved in one shot
  by `sg_stash_load_trees` (`include/sg/stash.h`): `base_tree` (parents[0],
  i.e. the diff baseline), `theirs_tree` (the stash commit itself),
  `index_tree` (parents[1]), and the optional `untracked_tree` (parents[2]).
  Output goes through `sg_diff_print`.
  WARNING: **the default format is `--stat`, not patch** (measured against
  real git) -- **but any diff option that is not itself a format selector
  switches it to a patch** (Phase 44), exactly as if `-p` had been given.
  `-M[<n>]`, `--find-renames[=<n>]`, `--no-renames` and `--histogram` all do;
  the stash-specific `-u`/`--include-untracked`/`--only-untracked` do NOT
  (and `-u -M` is still a patch, so `-u` neither implies nor suppresses).
  **An explicit format wins regardless of order** -- `-M --stat` and `--stat
  -M` both print a stat -- which is why `cmd_stash.c` tracks `format_given`
  and `diff_opt_given` and resolves them AFTER the parse loop; an in-loop
  "last one wins" passes one of those two and fails the other. Until Phase 44
  sg simply ignored the rule, so `sg stash show -M` printed a stat where git
  prints a patch. WARNING: `-u` and `--only-untracked` **are not two independent
  booleans, they are the same mode selector, and whichever is written last
  wins** (both orderings measured). WARNING: the untracked half of `-u`
  needs to compare an **empty tree** (`NULL`) against `untracked_tree`, **not**
  `base_tree` against `untracked_tree` -- the latter would report a phantom
  deletion for every path that exists only on the tracked side, printing the
  same path twice.
- **`sg_stash_apply` and `sg_stash_apply_check_dirty` are two of the four
  `sg_merge_trees` call sites, and until Phase 50 nothing covered their
  rename behaviour at all** -- `fuzz_merge_rename.py` never runs `sg stash`,
  and interop's only stash+rename group (Phase 31) exercises `sg stash
  show`, which goes through `sg_diff_detect_renames`, a different code path.
  `tests/test_stash_rename.c` covers two genuinely different dimensions:
  the STASH'S OWN CONTENT containing a rename (needs detection to come out
  right -- without it the edit and the rename are a modify/delete conflict),
  and a staged rename sitting in the INDEX at apply time that the stash never
  touches (never reaches the merge at all; it exercises the re-stage loop,
  which has no concept of a rename and came out right by construction rather
  than by design).
  WARNING: **`sg_stash_apply_check_dirty`'s `rename_score` is measurably
  unobservable, and that is not a coverage gap.** Setting it to 0 leaves the
  function's full answer byte-identical across clean / dirty-at-destination /
  dirty-at-source (measured with a probe against both builds). It only
  reports which TOUCHED paths are dirty, and rename detection changes how a
  path resolves, not which paths the merge touches. Do not go hunting for the
  test; see Phase 50 of `docs/DESIGN.md` for the printed evidence.
- **`sg stash push` writes `reset: moving to HEAD` to `logs/HEAD`** (Phase
  48), matching real git -- and the branch's own log gets nothing, which is
  Phase 17's rule 1 falling out rather than a special case. WARNING: **two
  measured cases log nothing at all**, and only one of them is a stash rule:
  a **partial** push (`sg stash push -- <path>`) never updates HEAD, so the
  call is skipped explicitly; a **detached** HEAD needs no condition at the
  call site, because `sg_ref_set_head_detached` suppresses the no-op itself
  (see the WARNING under `docs/RULES-refs-revparse.md`'s ref-writing
  bullet). The write is
  deliberately **not fatal** on failure -- the stash commit and the working
  tree reset have both already happened by then.
- **`sg stash` supports `-u`/`--include-untracked`, `-a`/`--all`,
  `--keep-index`, `--index` (Phase 20)**. `sg_stash_push` takes
  `sg_stash_push_opts` (`include/sg/stash.h`), not a run of positional
  arguments. Enumerating untracked files always goes through
  `sg_status_list_untracked` (`include/sg/status.h`, shared by `status`/
  `-u`/`-a`, `include_ignored` toggle), and the corresponding tree is built
  via `sg_tree_build_from_untracked` (`include/sg/tree_build.h`). Two spots
  deliberately diverge from real git: when `-u`/`-a` collides with an
  existing file, all entries are rejected all-or-nothing (real git applies
  partially, leaving an entry with no way out); a dirty apply/pop that
  collides with an **already-staged** change is always rejected (real git's
  ours is the index, which can be merged; sg's ours is HEAD, and allowing it
  would clobber the staged content). Details in Phase 20 of
  `docs/DESIGN.md`.
- **A deletion in the working directory has been stashable since Phase 21**:
  the stash's own tree omits that path, so `pop` re-deletes it instead of
  restoring it; a single deletion is enough on its own to produce a stash
  (no longer "No local changes to save"). The index parent (`stash^2`) still
  lists the file, unless the deletion was already staged. **This is the
  first time `sg stash pop` can actually delete a file from the working
  directory** -- previously `worktree_tree` always contained every index
  path, a `deleted` entry's `ours_present` was always 0, and `merge.c` always
  skipped `remove()`. A newly reachable divergence: stash an unstaged
  deletion, then stage a deletion of that same path, then pop -- sg rejects
  it while real git does not (the same "ours is HEAD, not the index" rule).
  Details in Phase 21 of `docs/DESIGN.md`.

## Phase 77: the whole-tree reset-to-HEAD step can now fail on a lock collision

`sg_stash_push`'s final `sg_ref_move_head(..., "reset: moving to HEAD")`
call (the no-op reset that logs `HEAD@{1}`) used to be unconditionally
non-fatal on ANY failure ("stash succeeded but its reflog line could not
be written", exit 0) -- correct for an ordinary I/O problem (the stash
commit and worktree reset already happened), but measured WRONG for a
lock collision: real git's own `stash push` exits 1 there. The fix checks
`sg_ref_last_lock_err()->kind` right after the failure: `LOCKED`/`_DF`
now returns `-2` (this function's pre-existing "durable stash entry, but
a later step failed" convention, already handled by `cmd_stash.c`); any
other kind keeps the old warn-and-continue behavior. Do not collapse this
back to a single unconditional branch -- the two failure directions
(warn-and-continue vs. -2) are deliberately different per git's own
measured behavior, not an arbitrary choice.

`sg_stash_drop`'s `refs/stash` removal (both the "last entry" branch,
which calls `sg_ref_delete_under`, and any future branch that writes
`refs/stash` directly) is now covered by the SAME lock `sg_ref_update`/
`sg_ref_delete_under` take everywhere else in this project -- before
Phase 77, `sg stash pop`/`sg stash drop` had NO lock of their own at all,
and a foreign `refs/stash.lock` caught nothing. `cmd_stash.c`'s pop/drop
failure messages use `sg_ref_lock_err_report(stderr, NULL)` (see
`docs/RULES-refs-revparse.md`'s Phase 77 entry) to get git's own "cannot
lock ref 'refs/stash': ..." wording when the failure is a lock collision,
falling back to the pre-existing generic message otherwise.

## Phase 81c: which stash path actually writes the working tree

`restore_matched_paths` is reached ONLY by a partial `sg stash push --
<pathspec>`. A plain `stash push` goes through `sg_apply_tree_to_workdir`
(apply.c) and `stash pop`/`apply` go through `sg_merge_result_apply`
(merge.c). Phase 81c changed all three, but every stash fixture in the
project drove the plain push/pop pair, so `restore_matched_paths`'s mode
handling and its `lstat` had **zero coverage** and two mutations against
them stayed green. Interop `phase81c c18` is the fixture that reaches it;
if you touch that function, that is the row that guards you.

**Pre-existing divergence, pinned but NOT fixed by Phase 81c**: after
`stash pop`, real git keeps a real cached size for every entry the stash
never touched and zeroes only the path it rewrote, while sg zeroes the
cached size for ALL of them. sg's answer is safe -- a zero forces the next
reader to compare content, so nothing is ever wrongly called clean -- it
is merely slower. Pinned on both sides in interop's `phase81c c11 index`
rows so a future change to either side fails by name. It predates the
symlink work and was only made visible by adding an index-bytes oracle;
see `docs/RULES-paths-strings.md`'s Phase 81c entry for the size rule
itself.
