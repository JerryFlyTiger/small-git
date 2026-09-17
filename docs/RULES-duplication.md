# Rules: known duplication, and what has already been converged

Scope: Converge opportunistically when you touch one of these; never add another copy. Also records which batches must not grow back.

**Read this file before editing anything in that scope.** It is not
background reading: every rule here was measured against real git 2.55.0,
and most of the `WARNING:`s exist because something slipped past a fully
green board. Project-wide rules -- the gates, the code conventions, the
deliberate divergences -- stay in `CLAUDE.md`; the full derivations and
measurement tables stay in `docs/DESIGN.md` (look up a specific section,
do not read the whole thing).

- Known duplication (converge opportunistically when you touch it, do not add
  another copy): the two literal copies of `path_join` (`cmd_add.c`,
  `status.c`) were converged into `sg_path_join` in Phase 21, along with 14
  `.c` files' individual `#define SG_PATH_MAX`, `SG_TREE_BUILD_PATH_MAX`,
  `SG_REVPARSE_PATH_MAX`, and 36 bare literal `4096`s -- **this batch must
  not grow back**; small strbufs are still duplicated between
  `src/workdir/apply.c` and `src/cli/cmd_restore.c` (**the two are not
  byte-for-byte identical**: the former takes prefix + path, the latter takes
  only path, so converging them requires deciding on an interface first, it
  is not "opportunistic"); Phase 23 already eliminated their individual
  fixed-length buffers. The six literal copies of `resolve_commit_tree`
  (`cmd_switch.c`, `cmd_merge.c`, `cmd_rebase.c`, `cmd_clone.c`,
  `cmd_reset.c`, `workdir/apply.c`) were converged into `sg_commit_tree_of`
  (`include/sg/objstore.h`) in Phase 15; always call this function to get a
  commit's tree id, do not hand-roll another copy. The two ways of building
  index->tree have also been extracted into
  `sg_tree_build_from_index`/`sg_tree_build_from_workdir`
  (`include/sg/tree_build.h`); the former only consumes the index's stage-0
  entries, the latter re-hashes the working directory; new code should check
  the header comment to pick the right one, not rewrite this logic at the
  call site. **The latter, since Phase 21, additionally requires a mandatory
  `sg_workdir_missing`**, which decides how to handle a path that is "in the
  index but gone from the working directory": `KEEP_INDEX_BLOB`
  (`sg_snapshot_create`, the safety net needs to be able to restore to
  before the deletion) versus `RECORD_DELETION` (`sg_stash_push` building
  `worktree_tree`, needs to be able to represent a deletion). **Having no
  default is deliberate** -- silently picking one side is precisely the bug
  it exists to eliminate. Note that **both are used within a single
  `sg stash push`** (it also calls `sg_snapshot_create` itself). Also, "the
  file exists but cannot be read" is **a hard failure under both policies**,
  so `sg_snapshot_create`'s contract is "resolve it or reject the snapshot",
  not "always resolves"; the classifying `lstat` **must come after
  `sg_read_file` fails**, doing the probe up front would turn a benign race
  into a hard failure (rationale in Phase 21 of `docs/DESIGN.md`). The loop
  shared by merge/rebase/stash that "lands `sg_merge_result` onto the
  working directory + index" has also been extracted into
  `sg_merge_result_apply` (`include/sg/merge.h`). **Since Phase 20 it skips
  entries whose result equals ours (HEAD), not rewriting the working
  directory, but it still adds every result entry to the index**
  (`add_resolved_entry` runs unconditionally, the predicate is
  `sg_merge_entry_touches_ours`, the single definition of it, do not write a
  second one). Both `cmd_merge.c` and `cmd_rebase.c` take the index this
  function builds and use it to build the commit's tree -- if
  `add_resolved_entry` were also skipped to follow suit, merge/rebase commits
  would silently lose files, and `make test` cannot catch this regression,
  only `interop.sh` can (measured in Phase 20: 10 rebase-related interop
  checks turned red while `make test` stayed fully green). When modifying
  this function, a green `make test` does not count. `env_or()` (reads
  `GIT_AUTHOR_NAME`/`EMAIL` with a fallback) is still duplicated **eight**
  times, byte-for-byte: `storage/reflog.c`, `storage/chunk.c`,
  `safety/stash.c`, `safety/snapshot.c`, `cli/cmd_rebase.c`,
  `cli/cmd_merge.c`, `cli/cmd_tag.c`, `cli/cmd_commit.c`. Converge
  opportunistically when you touch it, do not add another copy.
  **Phase 27 already converged this**: `sg_status_diff_unstaged`
  (`src/workdir/status.c`) is now a thin adapter over `sg_diff_index_workdir`,
  no longer a second scanning loop. Before converging, exactly **three
  categories** of divergence were enumerated
  (`tests/test_status_diff_parity.c`); two were fixed, one deliberately kept:
  **an unmerged line and its stage-2 counterpart line do not go into the
  status list** (`cmd_status.c` has its own Unmerged paths section). WARNING:
  the filter predicate must be "**the previous line is unmerged and has the
  same path**", it must not just compare paths -- `sg_index_read` does not
  validate ordering or dedupe, so a corrupt index can put two independent
  lines with the same path next to each other and one gets silently dropped,
  and this list feeds the dirtiness check for `switch`/`reset --hard`.
  WARNING: after converging, **a pure chmod makes the working directory count
  as dirty** (blocked by `switch`/`reset --hard`/`merge`/`rebase`), which
  matches real git, already measured.
  Phase 25 grew **one more pair**: `report_bad_tree_path` (`cli/cmd_diff.c:62`)
  and `report_bad_stash_tree_path` (`cli/cmd_stash.c:337`) are nearly
  identical, byte-for-byte (both turn `sg_tree_flatten`'s `-2` into a single
  error line naming `bad_path`). **Phase 26 added two interop checks for
  this** (using `git mktree` to build a tree containing a `..` entry, going
  through `sg diff <rev> <rev>` and `sg stash show` respectively, asserting
  the error message names the path), so **it is now safe to converge them**.
  **Phase 62 converged the OTHER three-way duplicate `CLAUDE.md` used to list
  separately**: `report_pathspec_error` (`cmd_diff.c`/`cmd_status.c`/
  `cmd_stash.c`, byte-for-byte) and `cmd_diff.c`'s own
  `arg_exists_in_worktree`/`split_revs_and_paths` (pure functions,
  `sg log` needed the exact same disambiguation grammar, measured
  identical rather than assumed) now live in `include/sg/cli_args.h` +
  `src/cli/cli_args.c` as `sg_cli_report_pathspec_error`/
  `sg_cli_arg_exists_in_worktree`/`sg_cli_split_revs_and_paths` (the last
  taking a `cmd_name` parameter so its `use sg <cmd> -- <path>` suggestion
  names the actual caller). Verified as a pure refactor: `interop:
  2720/2720` both immediately before and immediately after this commit.

- **Phase 75 added a second shared reporter to `cli_args.c`**:
  `sg_cli_report_rev_error` + `sg_cli_classify_rev_error`, the single place
  every command's "this revision does not resolve" diagnostic goes through
  (see `docs/DESIGN.md`'s Phase 75 section for the full measured table).
  Ten call sites (`cmd_tag.c`, `cmd_show.c`, `cmd_cat_file.c`, `cmd_log.c`,
  `cmd_diff.c`, `cmd_reset.c`, `cmd_reflog.c`, `cmd_merge_base.c`,
  `cmd_cherry_pick.c`, `cmd_revert.c`) were converged onto it; `cmd_rebase.c`
  was NOT, on purpose -- its `invalid upstream 'X'` wording is identical
  across every input class (measured), so there is no per-class table row
  for it to share, just a plain wording fix at its one call site. **Do not
  add a second per-class rev-error table** -- a new command that needs this
  kind of diagnostic gets a new row in `REV_ERR_TABLE`, not a new switch
  statement at its own call site.

## Phase 77: reflog.c shares refs.c's empty-directory removal instead of copying it

`sg_reflog_append` (`storage/reflog.c`) needed the SAME D1a fix
`storage/refs.c`'s ref-write path got -- a stale/foreign empty directory
sitting exactly at the reflog path (`logs/refs/heads/<name>`) must not
block the append, matching real git. Rather than writing a second
depth-first empty-directory walk, `sg_ref_remove_empty_dir_tree`
(`storage/refs.c`) was made non-`static` and declared in `refs.h`
specifically so `reflog.c` could call the existing one. If a THIRD file
ever needs this same removal, it goes through this same function -- do
not write a third copy.

## Phase 80: the worktree ancestor walk is one function, shared by write and delete

F1's `sg_write_file_worktree` and the fix round's `sg_remove_file_worktree`
(both `workdir/workdir.c`) need the IDENTICAL "lstat each ancestor component
strictly below the repo root; a non-directory blocks the whole chain"
walk -- the write side to refuse writing THROUGH a symlink, the delete side
to refuse deleting through one. It is a single static helper,
`walk_worktree_ancestors(abs, root_len, create_missing)`: `create_missing=1`
is the write path (mkdir a missing component), `0` is the delete/prune path
(a missing component means "nothing here", stop). `sg_prune_empty_parents`
calls the same helper (with `0`) before its `rmdir`. Do NOT write a second
ancestor-lstat walk for any future worktree mutation -- route it through this
one. The rule it enforces (never traverse a symlink below the root; never
inspect a component at or above the root, so a repo reached through a
symlinked `/tmp` still works) lives in `docs/RULES-paths-strings.md`.
