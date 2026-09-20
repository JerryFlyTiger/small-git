# Rules: path joining, path quoting, untrusted paths, string building

Scope: `sg_path_join`, `sg_quote_path*`, `sg_path_component_is_safe`, `sg_prune_empty_parents`, `sg_strfmt_alloc` -- `include/sg/workdir.h`, `include/sg/quote.h`, `include/sg/strfmt.h`.

**Read this file before editing anything in that scope.** It is not
background reading: every rule here was measured against real git 2.55.0,
and most of the `WARNING:`s exist because something slipped past a fully
green board. Project-wide rules -- the gates, the code conventions, the
deliberate divergences -- stay in `CLAUDE.md`; the full derivations and
measurement tables stay in `docs/DESIGN.md` (look up a specific section,
do not read the whole thing).

- **Joining `base/rel` always goes through `sg_path_join`**
  (`include/sg/workdir.h`, Phase 21), buffer size uses `SG_PATH_MAX` from the
  same header. **Do not write a raw
  `snprintf(buf, sizeof buf, "%s/%s", ...)`**: a truncated path usually still
  points at some *real but wrong* location in the tree, so a subsequent
  `lstat`/`unlink`/write **succeeds** against the wrong file instead of
  failing outright. What truncation should mean **is decided per category**
  -- write/delete must never skip it; gates lean conservative (mark dirty,
  mark collision, **the failure direction must never be "allow it"**);
  reporting paths return -1 for the CLI to print, and must never silently
  drop a file from `sg status`/`sg diff`. There are exactly two deliberate
  exceptions: `prune_empty_untracked_dirs` keeps its own inline check (its
  convention is to silently skip), and buffers of the `git_dir` + fixed-length
  hex kind (different risk profile, they just use `SG_PATH_MAX`).
- **Printing a path for the user always goes through `sg_quote_path` /
  `_prefixed` / `_delimited`** (`include/sg/quote.h`, Phase 23). If a filename
  containing control characters is not quoted, **real ESC bytes go straight
  into the terminal**, which can clear the screen or rewrite the color of
  subsequent output. The three functions divide work by **layout**, not by
  source: an indented list entry that has a whole line to itself uses
  `sg_quote_path` (no quotes when not needed); diff's `a/`/`b/` use
  `_prefixed` (**the quotes must wrap the prefix**, so the prefix is folded
  into the function); embedded mid-sentence uses `_delimited` (always quotes
  unconditionally, `format` needs to change to a bare `%s`, otherwise it will
  print `'"a\tb"'`).
  WARNING: the return value is a **borrowed pointer**, into one of 4 rotating
  static buffers -- **do not store it across statements, do not free it**.
  WARNING: **bytes >= 0x80 are printed as-is** (equivalent to
  `core.quotepath=false`), so **interop comparisons need `-c
  core.quotepath=false` on the git side**; control-character groups do not
  need it (both sides quote them).
  WARNING: **quoting is explicitly forbidden for**: commit/tag messages and
  author strings (would break the byte-fidelity of `cat-file -p`), ref/branch/
  tag names (real git does not quote them either, **there is no oracle**),
  stdout informational messages like `Cloning into` (real git does not quote
  those either, measured), and **since Phase 34, `* Unmerged path <p>`**
  (measured with `od -c`: real git lets a raw ESC byte in the filename go
  straight to stdout, `core.quotepath` has no effect on this line either).
- **Untrusted paths always go through `sg_path_component_is_safe` /
  `sg_relpath_is_safe`** (`include/sg/workdir.h`, Phase 22). They block
  `""`/`.`/`..`/anything containing `/`, plus any case variant of `.git`,
  forms with trailing `.`/whitespace, and names that equal `.git` after
  folding away HFS+ ignorable code points. **The guards are placed by
  "source", not by "dangerous action"** -- one guard per source, three
  sources total: tree bytes (`sg_tree_flatten`, returns `-2` and fills
  `bad_path`), index entries (`remove()` in `apply.c`, the write in
  `cmd_restore.c`), argv (`cmd_add.c`). Removing any one of them leaves a set
  of inputs only that one could block, so it does not count as redundant
  defense.
  **Do not push the guard down into `sg_write_file_mkdirs`/`sg_path_join`**:
  `storage/refs.c` uses the former precisely to write ref files into
  `.git/refs/`, and blocking `.git` there would outright kill ref writes.
  **Do not pull it up into `sg_tree_parse` either**: real git's object store
  accepts a broken tree as-is, `cat-file -p` can still read it out, and
  pulling the check up would leave `sg cat-file -p` unable to inspect a
  broken object.
  WARNING: **when walking the working directory, "is this the gitdir"
  must not use this predicate**, use `strcmp(name, ".git") == 0` instead:
  real git lists `.git.` as an untracked directory, and using the predicate
  to skip it would make `sg status` **under-report** (measured in Phase 22).
- **After deleting a tracked file, `sg_prune_empty_parents` must be called**
  (`include/sg/workdir.h`, Phase 21). There were **three** WORKING-TREE call
  sites as of Phase 37: right after a successful `remove()` in
  `workdir/apply.c` and `workdir/merge.c`, and `safety/stash.c`'s
  `restore_matched_paths`, which does the same "delete a matched,
  target-absent path" step for `sg stash push`'s partial-pathspec restore
  -- the same reasoning applies there as at the other two, it is just a
  third call site rather than a reason to route through
  `sg_apply_tree_to_workdir` (which this codebase deliberately does not
  give a pathspec parameter, see Phase 37 in `docs/DESIGN.md`). **Phase 76
  fix round 4 added TWO more, outside the working tree entirely**:
  `cli/ref_delete.c`'s batch-delete engine, once for the ref's own
  directory chain and once (independently) for its reflog's, after every
  successful `sg_ref_delete_under` -- the function's `repo_root` parameter
  is generic (any directory a caller wants treated as the floor that is
  never removed), not worktree-specific, so reusing it for
  `<git_dir>/refs/heads` / `<git_dir>/logs/refs/heads` needed no change to
  the function itself, only to what gets passed in.
  WARNING: it is **deliberately not ignore-aware**, which is **the opposite
  rule** from `prune_empty_untracked_dirs` in `safety/stash.c`: the former
  cleans up a directory that is "empty but ignored" (measured against real
  git 2.55.0), the latter deliberately leaves it alone (the interop check
  that `build/` must survive guards this). **Do not "unify" these two.** It
  also rejects absolute paths and relpaths containing `..` -- **because those
  paths come from tree objects, and `src/object/tree.c` does no validation at
  all when parsing entry names**. WARNING: the same unvalidated paths are
  also used by the adjacent `remove(abspath)` calls (`apply.c`, `merge.c`),
  which is a gap that predates Phase 21 and is **still unfixed**: path
  containment should be enforced at the layer that parses trees / writes the
  index, not patched separately at every consumer.
- **Building any user-facing string that embeds a user-controlled one goes
  through `sg_strfmt_alloc`** (`include/sg/strfmt.h`, Phase 65) -- it sizes
  with `vsnprintf(NULL, 0, ...)`, mallocs exactly that, and returns a string
  the caller frees. **Do not write `char buf[N]; snprintf(buf, sizeof buf,
  "...%s...", <anything a user can lengthen>)`.** That shape had produced
  FOUR measured byte-compatibility bugs by the time it was converged, every
  one of them silent (exit 0, no warning):
  the rebase CONFLICT MARKER (`cmd_rebase.c`, `theirs_label[300]`: a
  289-char subject agreed with git, 290 diverged, and at 404 the marker lost
  its closing `)` -- malformed, not merely short); the MERGE COMMIT MESSAGE
  (`cmd_merge.c`, `message[512]`: two 250-char branch names gave git 523
  bytes and sg 513, and **a different message is a different object id**);
  the `logs/HEAD` line (`cmd_reset.c`, `reflog_msg[512]`: 939 bytes vs 630);
  and `sg_repo_read_remote_url` (`storage/repo.c`), which was the worst
  because it is the only **fail-OPEN** one -- a 2029-byte `url` in
  `.git/config` was truncated and `sg fetch` then made a real network
  request **to a different address than the one configured**, silently. Its
  line reader is now `getline()`, not a bigger `fgets` buffer.
  WARNING: **"a branch name is at most 255 bytes" is a FALSE bound and was
  the reasoning that hid one of these.** A ref name is a PATH: each
  component is capped by `NAME_MAX`, the total is not. The reset fixture
  uses four 200-char components (803 bytes). When arguing that some fixed
  buffer is unreachable, this is the bound that is usually wrong.
  WARNING: **the fixtures need shapes an ordinary test cannot produce** --
  a long commit SUBJECT for the marker, a MULTI-COMPONENT ref for the
  reflog lines, a hand-written `.git/config` for the remote URL. Every
  converted site has its own named check, and each was mutation-verified
  ALONE (one check red per site, 3183/3184): a mutation covering several
  sites at once lets a broken one hide behind a check another site turned
  red.
  WARNING: **`sg undo`'s snapshot labels go through the same helper but
  have NO oracle** (`sg undo` has no real-git counterpart), so their
  conversion is mechanical hygiene, not something a differential check can
  ever witness. Do not go looking for a test.
  WARNING: **one pre-existing leak was found and deliberately NOT fixed**:
  `pick.c`'s `attempt_one` leaks `out->message` on the OOM path where the
  theirs label fails to allocate. It predates this phase (the hand-rolled
  version leaked identically) and is reachable only under OOM; recorded in
  `docs/DESIGN.md` rather than fixed inside a phase about a different bug.

- **Phase 75's revision-error reporter is a deliberate exception to this
  file's default quoting rule.** `sg_cli_report_rev_error`
  (`cli/cli_args.c`) embeds arguments RAW inside `'...'`, never through
  `sg_quote_path_delimited` (which always C-quotes and always emits
  `"..."`) -- measured against real git 2.55.0: a tab, a space, a double
  quote, a backslash, and a UTF-8 byte all pass through an argument
  UNMODIFIED in git's own `fatal: ...`/hint lines. The oracle here is
  git's own line, not this project's usual "quote anything embedded in a
  sentence" convention. It DOES still sanitize control bytes (every byte
  0x01-0x08/0x0b-0x1f/0x7f becomes `?`; tab/newline/space/>=0x80 pass
  through raw, matching git's `vreportf`), just not through the C-quoting
  path -- see the reporter's own header comment and `docs/DESIGN.md`'s
  Phase 75 section. Do not "fix" this call site to use
  `sg_quote_path_delimited` for consistency with the rest of this file;
  that would produce a wording git itself does not use.

- **Phase 76 fix round 1: a fixed-size buffer that silently picks the
  WRONG answer once a name gets long enough is worse than one that
  silently truncates a path, because the failure is invisible in the
  common case and only shows up as a wrong branch taken.** Three spots in
  `cli/cmd_branch.c` and `cli/ref_delete.c` used to `continue`/return a
  default the moment a name-derived buffer overflowed a fixed
  `SG_PATH_MAX` array: `check_df_conflict`'s per-ancestor-component
  `prefix` buffer (would have silently skipped checking that ancestor,
  letting a real D/F conflict through uncaught), `is_loose_branch_ref`'s
  path buffer (would have silently answered "not loose", picking the
  WRONG wording branch for the D/F message), and `ref_delete.c`'s pass 2b
  `.lock`-suffixed path (would have silently skipped lock-collision
  detection for that name entirely, letting it reach the delete pass with
  no lock taken at all). None of the three needed to be a fixed-size check
  in the first place: `sg_mkdir_parents` (`workdir.h`) already enforces
  the project's REAL, single, shared length ceiling (`SG_PATH_MAX`, with
  an explicit -1) further down the same call chain, so a second, smaller,
  ad hoc ceiling here bought nothing except a chance to pick the WRONG
  answer silently (CLAUDE.md's own bug #3, an 803-char name, is proof the
  per-name/per-component limit is looser than one of these local buffers,
  not proof there is no limit anywhere -- round 1's own text overstated
  this as "no length limit exists", corrected here in round 2). All three
  now build their strings with `sg_strfmt_alloc` and only fall back on an
  actual allocation failure (not a length-based silent skip), which is a
  different, acceptable category: OOM is failed CLOSED (treated as "a
  conflict/collision could exist and cannot be ruled out"), never as
  "assume the answer that lets the operation through." Phase 76 fix round
  2 additionally found and closed a THIRD failure-direction bug in this
  same family, one call further out: `cmd_branch.c`'s
  `is_loose_branch_ref` returning "not loose" on its OWN allocation
  failure was being read by its one safety-critical caller
  (`branch_aliases_current`, deliberate divergence #10's alias probe) as
  "packed-only, cannot alias," which let a dangerous force-update/delete
  of the checked-out branch through on the strength of an OOM -- a
  cosmetic-wording helper's fail-open leaking into a SAFETY decision one
  level up. Fixed by making the helper genuinely tri-state
  (yes/no/error) so the one caller that needs to can fail closed on
  error, while `check_df_conflict`'s two call sites (a wording choice
  made only AFTER a conflict is already proven to exist) still fold error
  into "not loose," which is fine at that point.

## Phase 77: ref writes across cmd_branch.c/cmd_tag.c/cmd_reset.c/cmd_merge.c/cmd_switch.c can now fail on a lock, not just an I/O error

`sg_ref_update`/`sg_ref_write_path`/`sg_ref_move_head`/`sg_ref_set_head*`
now take a real O_CREAT|O_EXCL lock before every write (see
`docs/RULES-refs-revparse.md`'s Phase 77 entry for the shared mechanism).
`cmd_branch.c`'s create/`-f` path was changed to call
`sg_ref_update_locked` (consuming the lock it already holds) instead of
`sg_ref_update`, to avoid colliding with its own Phase-76 lock -- do not
revert this back to a plain `sg_ref_update` call, that reintroduces a
self-collision (EEXIST against your own lock) on every create/`-f`.
`cmd_tag.c`/`cmd_reset.c`/`cmd_merge.c`'s ff path use
`sg_ref_lock_err_report` to surface git's own wording when the new
failure kind is a lock collision; see each file's own module-table row
(`docs/RULES-merge.md`/`docs/RULES-sequencer.md`) for the specific
`ref_display` chosen at each call site.

## Phase 79 (extended by Phase 79b): one path-printing site in `cmd_merge.c`
## deliberately does NOT quote

`report_untracked_overwrite` (`cli/cmd_merge.c`) prints each colliding path
RAW -- neither `sg_quote_path` nor `sg_quote_path_delimited` -- in BOTH the
FILE bucket's listing ("The following untracked working tree files would be
overwritten by merge:") and the DIRECTORY bucket's listing ("Updating the
following directories would lose untracked files in them:", added Phase
79b). Measured against git 2.55.0: git prints a space, a double quote and
UTF-8 verbatim in both, and this project's goal is byte compatibility with
git's own wording. **It used to be the only function with unquoted
path-printing sites in that file; Phase 79c's `print_untracked_scan_error`
(also `cli/cmd_merge.c`) is a second one**, for the same reason -- it prints
a scan I/O error's path in a message shaped like git's own
`warning:`/`fatal:` opendir wording (see below), and that wording is not
quoted either. Both are deliberate, not an oversight; interop checks pin
the odd-name listing on both sides precisely so that "fixing" either one
goes red by name. See `docs/RULES-merge.md`'s Phase 79 entry for the rest of
that check's rules, including the Phase 79b cold-read correction that
NEITHER bucket is sorted or de-duplicated by this function -- the order and
any repeated names come straight from the caller's candidate list.

Also from Phase 79, in `sg_untracked_would_be_overwritten`
(`workdir/apply.c`): a path too long to join with `repo_root`, and an
`lstat` failure that is neither `ENOENT` nor `ENOTDIR`, both fail CLOSED --
the path is reported as a collision rather than silently cleared. Same
direction as the Phase 76 rule above: a check that cannot verify something
must never answer with the value that lets the destructive operation
through. Phase 79b's own recursive directory scan extends this: any
opendir/readdir/lstat/ignore-engine failure encountered partway through
scanning a candidate directory's contents fails the WHOLE call with -1,
rather than guessing "clear" about the unscanned remainder.

**Phase 79c**: that "-1 rather than guessing" rule now comes with a
distinguishable REASON attached (`sg_untracked_overwrite_error`,
`include/sg/apply.h`) instead of always collapsing to "sg: out of memory" --
an opendir/readdir/lstat failure during the scan carries its own kind, the
repo-relative path involved, and the errno, so `cli/cmd_merge.c`'s
`print_untracked_scan_error` can print git's own truthful wording (measured
against git 2.55.0: a chmod-000 subdirectory is a PERMISSION error, not an
allocation one). The recursive scan itself (`untracked_overwrite_dir_scan`,
`workdir/apply.c`) also stopped keeping a `char[SG_PATH_MAX]` array per
recursion level -- a `untracked_path_accum` (one heap buffer, shared and
truncated/extended in place across the whole recursion) and one shared
`SG_PATH_MAX` scratch buffer for `opendir`/`lstat` absolute paths replace
what used to be three per-frame stack arrays, so a pathological chain of
one-letter untracked directories no longer scales stack use with depth.

## Phase 80: a working-tree write always goes through `sg_write_file_worktree`,
## never `fopen` a worktree path directly; below the repo root, always `lstat`,
## never `stat`

`sg_write_file_worktree` (`include/sg/workdir.h`, `src/workdir/workdir.c`) is
the ONLY way to write tree/index/merge content into the working tree --
`apply.c`, `merge.c`, `cmd_restore.c`, `stash.c` all go through it, and the
old `sg_write_file_mkdirs` was removed from every one of those call sites
(it still exists, purely as a test-fixture-setup helper with no symlink
concern -- see its own header comment for why it was kept rather than
deleted outright). Every path component strictly BELOW the repo root is
`lstat`'d, never `stat`'d: a real directory is traversed, a missing one is
created, and anything else (symlink, regular file, fifo) stops the write
cold (-1) rather than being silently followed or truncated through --
`stat` would resolve a symlink component and let a write land wherever that
symlink points, which is exactly the security hole this phase closes (a
merge or restore into an ignored symlink used to write straight through it
into whatever it pointed at, including a directory outside the repository
entirely). Components AT OR ABOVE the repo root are never inspected, so a
repository reached through a symlinked ancestor (macOS's `/tmp` ->
`/private/tmp`, or a symlinked cwd) keeps working -- the boundary is
`repo_root` itself, not "any symlink anywhere in the path".

`sg_worktree_clear_write_path` (`include/sg/apply.h`,
`src/workdir/apply.c`) is the paired pre-write clearing step, called
immediately before every one of the write sites above, with ONE `sg_ignore`
opened per caller-level operation (not per file). It replaces exactly what
real git itself replaces during a checkout-like write -- an IGNORED
ancestor blocker, or a directory sitting at the write target that is
itself ignored or recursively holds nothing but ignored (and, only for the
pre-flight's own use, tracked) content -- and fails closed (-1) on
anything else, leaving it in place. **The tracked-path exemption used by
the recursive scan (`untracked_overwrite_dir_has_nonignored`'s `idx`
parameter, shared with `sg_untracked_would_be_overwritten`) is deliberately
NOT available at write time** -- every real call site passes `idx = NULL`,
because Phase 80's own F4 fix (below) already deletes every tracked path a
merge result removes BEFORE any write runs, so by the time this function
scans a directory a write needs to replace, nothing that would have needed
the exemption is still on disk. Passing a real `idx` here instead of `NULL`
would silently reproduce the exact write-order bug F4 fixes, in reverse.

**Do not add a second worktree-write helper for "just this one case".**
`docs/RULES-duplication.md`'s rule about never coexisting with the old
function applies here just as much as it did to `sg_mkdir_parents`'s own
`.git`-side callers: if a new command needs to write into the working
tree, it calls `sg_write_file_worktree`, preceded by
`sg_worktree_clear_write_path` when the target might be blocked by
something replaceable.

## Phase 80 fix round (cold-read finding 1, CRITICAL): the DELETE side needs
## the exact same symlink guard the write side got, and did not have it

F1/F2 (above) close the write-side symlink hole, but a cold read found the
DELETE side wide open: `sg_apply_tree_to_workdir`'s loop 1 (removing a
tracked path absent from the target tree), `sg_merge_result_apply`'s F4
delete pass, `sg stash`'s two worktree-delete sites, and
`sg_prune_empty_parents` all built an absolute path with `sg_path_join`
and then called `remove()`/`unlink()`/`rmdir()` on it directly -- every one
of those POSIX calls resolves EVERY ancestor component of its argument
through symlinks (only the FINAL component is left unresolved). Measured
directly against git 2.55.0 (`SCRATCH/atk.py`'s construction): a tracked
`a/b/tracked.txt` whose ancestor `a` is replaced on disk by a symlink to a
directory OUTSIDE the repository, with the outside directory's own
`b/tracked.txt` made BYTE-IDENTICAL to the tracked content (so `sg status`
sees no local modification at all, and no second tracked file is needed to
make the fixture reachable -- this is the "clean attack" shape, deliberately
simpler than a fixture with a confounding second tracked change that would
trip the ordinary clean-workdir gate first and never reach the delete code)
-- `sg merge`/`switch`/`cherry-pick` (pre-fix) deleted the OUTSIDE file
through the symlink, exit 0, no warning. git refuses in all three
(`'a/b/tracked.txt' is beyond a symbolic link` for 3-way merge/cherry-pick;
`Your local changes ... would be overwritten` for a fast-forward merge or a
plain switch -- git's own wording differs by code path, sg does not
reproduce either one, see below). **`reset --hard` measured DIFFERENTLY
from what an earlier draft of this fix assumed**: in this exact shape
(topic's target tree has NOTHING at all under `a`, not even a replacement
file), git's `reset --hard` leaves the symlink completely untouched and
exits 0 -- it does not "rebuild the real directory" the way the fix's own
first-draft spec text claimed. That claim was never independently
re-verified before being written down; this section states the MEASURED
answer, and the interop pin (`phase80 D reset ff`/`3way`) asserts exactly
that (exit 0, `a` still a symlink) rather than an unverified assumption
that happened to look plausible.

**Fix**: `sg_remove_file_worktree(repo_root, relpath)`
(`include/sg/workdir.h`, `src/workdir/workdir.c`) is the single
worktree-delete primitive, mirroring `sg_write_file_worktree`'s own
ancestor walk almost exactly (both are now built on one shared static
helper, `walk_worktree_ancestors`, per `docs/RULES-duplication.md` -- one
walk, not two copies of it): every path component strictly below
`repo_root` is `lstat`'d; a missing one (at ANY level, including the final
component itself) means there is nothing to remove, which is SUCCESS, not
a failure -- every caller already tolerated a plain `remove()` failing with
ENOENT this way (a delete racing something else that got there first is
not exceptional), and treating "already gone" as anything other than
success broke an ordinary `sg reset --hard` onto a commit that restores a
path the working tree had already deleted, unstaged (measured: caught by
`tests/interop.sh`'s own `phase12` group during this fix's own gate run,
not by a new test -- a real, pre-existing scenario, not a corner case
invented for this fix). Anything else along the way that is NOT a real
directory (a symlink, a regular file, ...) fails the WHOLE call closed
(-1, `errno` set to `ELOOP` so a caller that already distinguishes
"already gone" from "really failed" via `errno`, like `stash.c`'s
`remove_untracked_files`, gets a reliable answer that is never mistaken
for `ENOENT`) WITHOUT removing anything. Components at or above
`repo_root` are never inspected, same boundary as the write side. Every
worktree delete in `src/` (`apply.c`, `merge.c`, `stash.c` x2) now goes
through this instead of a bare `remove()`/`unlink()` on a joined path.

**Callers now treat a blocked delete as a hard failure, not a silently
ignored one.** Before this fix, `sg_apply_tree_to_workdir`'s loop 1 and
`sg_merge_result_apply`'s delete pass had NO `else` branch at all on a
failed `remove()` -- the file stayed on disk, untracked, and the caller
carried on as if nothing had happened. Now a failure prints `sg: cannot
remove "<p>"` and sets the caller's own failure flag (`rc = -1` in
`apply.c`/`stash.c`, `content_missing = 1` in `merge.c`, matching the
existing convention each function already uses for a failed write),
aborting the whole operation. This is a broader change than "just the
symlink case" -- any REAL removal failure (e.g. a permission error on an
existing, non-empty directory) now also aborts loudly where it used to be
silent -- but ENOENT (the ordinary "already gone" case) is excluded from
this by `sg_remove_file_worktree`'s own success-on-ENOENT rule above, so
the common case is unaffected.

**`sg_prune_empty_parents` needed the identical guard, not just the delete
primitive** -- `rmdir()` has the exact same "resolves every ancestor
component except the last" behavior `remove()`/`unlink()` do, and this
function's own loop climbs from the deepest now-possibly-empty ancestor
back up toward `repo_root`, one `rmdir()` per level. A single
`walk_worktree_ancestors` check on `dirname(relpath)`'s full ancestor
chain, run ONCE before the loop's first `rmdir`, covers every candidate the
loop will ever pass to `rmdir` (each is a shrinking PREFIX of that same
chain) -- a symlinked ancestor found there blocks the WHOLE chain, not
just the deepest candidate, so the check cannot be threaded through the
loop itself. Its existing "deliberately NOT ignore-aware" contract (this
file's own rule above) is unchanged: this fix only closes the symlink hole,
it does not change what counts as empty or ignorable.

**Behavior contract**: a delete refused by the guard aborts the whole
operation via the caller's own ordinary failure path (`-1` up the stack),
exit 1. The **outside** directory is left byte-for-byte untouched -- that is
the whole point of the guard and the invariant the `phase80 D*` interop rows
pin. The IN-repo working tree is NOT guaranteed pristine, and must not be
described as such: `sg_apply_tree_to_workdir`'s loop 2 and
`sg_merge_result_apply`'s second pass both run unconditionally after the
delete pass, so an unrelated new file (`o.txt` in the `phase80 D` fixtures)
can already sit on disk when the abort fires -- exactly the "the working
tree may already be partially updated" caveat that `include/sg/apply.h` and
`include/sg/merge.h` have carried since before this phase. This guard adds
the outside-repo guarantee on top of that pre-existing (weaker) in-repo one;
it does not upgrade the in-repo one to atomic. This
is FAIL-CLOSED and is the ACCEPTED answer, not a new numbered divergence --
git also refuses in merge/switch/cherry-pick for this shape; only
`reset --hard` differs (see the measurement above), and sg failing closed
there instead of git's own "leave the symlink untouched" answer is
consistent with divergence #11's philosophy already documented in
`CLAUDE.md`: sg does not delete or otherwise disturb something it cannot
prove is safe. The message is `sg: cannot remove "<p>"`, in the project's
existing `sg: <verb> "<p>"` vocabulary (matching `sg: failed to write
"<p>"`'s own shape) -- NOT git's `is beyond a symbolic link` wording, which
sg has no equivalent concept for.

**A genuine, unrelated fourth delete site was found the same way** (a
`grep` for every other `remove`/`unlink`/`rmdir` on a worktree-joined
path): `safety/stash.c`'s `remove_untracked_files` (used by `sg stash push
-u`/`-a` to delete the untracked files it just captured into the stash's
third parent) had the identical bare-`unlink()` hole -- reachable with NO
tracked content at all, purely through an untracked file sitting under a
symlinked directory. Fixed the same way, keeping its own pre-existing
`ENOENT`-tolerant contract (a delete racing something else is not an
error) intact via `sg_remove_file_worktree`'s own success-on-ENOENT rule.

**Finding 3 (uninitialized read)**: `mkdir_parents_worktree`'s (and now
`walk_worktree_ancestors`'s) loop starts at `i = root_len + 1`,
deliberately skipping `abs[root_len]` (the terminator, when `abs` equals
`repo_root` exactly). For an EMPTY `relpath`, `sg_path_join` copies
`repo_root` alone (its own "rel is empty" rule), so `abs` has length
exactly `root_len` and the loop's first read is `abs[root_len + 1]` --
one byte past the string's own end, uninitialized stack content rather
than the terminator. No real caller passes an empty relpath (every one is
a repo-relative FILE path), but `sg_write_file_worktree` and
`sg_remove_file_worktree` both reject an empty `relpath` outright (`-1`)
before ever joining or walking, closing this rather than relying on every
future caller never doing it.

**Recorded, not fixed (same cold-read pass)**:
- A double `/` in the joined path when `repo_root` itself ends in a
  trailing slash -- no caller passes one (`sg_repo_root` never produces
  one), so this is cosmetic-only and left alone.
- `sg_write_file_mkdirs` (this file's own entry above) remains exported in
  the public header purely as a test-fixture-setup helper. Moving it out
  of `include/sg/workdir.h` into a test-only location is a future cleanup
  that touches roughly 30 test files for no behavior change; not done
  here.
- **Phase 81b: every read of a working-tree path as a blob goes through
  `workdir.h`'s symlink-aware helpers**, never `sg_hash_file_blob`/
  `sg_read_file` directly: `sg_worktree_classify` (one `lstat`, type and
  mode via `sg_worktree_mode_from_stat`), `sg_worktree_read_entry`/
  `sg_worktree_hash_entry` (a symlink's blob is its `readlink` bytes,
  read by `sg_worktree_readlink` with a growing buffer, never truncated),
  and `sg_worktree_ancestor_blocked` (a tracked path under a symlinked or
  non-directory ancestor is ABSENT, measured: git reports ` D dir/a.txt`
  plus `?? dir`). A dangling symlink EXISTS. The repo root itself may still
  be reached through a symlink (Phase 80 rule): only components below it
  are checked.
  WARNING: **`sg_worktree_read_entry`/`_hash_entry` fold "exists but
  unreadable" into ABSENT**, which is right for status/diff but wrong for a
  gate that must fail CLOSED. Round 1 of Phase 81b shipped exactly that
  regression in `sg_stash_apply_check_dirty` (an unreadable tracked file
  stopped counting as dirty, so `stash apply`/`pop` would overwrite it);
  such a gate must classify first and treat any non-ABSENT path it cannot
  hash as dirty.
  WARNING: **decide how to read a path from its ON-DISK type, never from
  the index's mode.** Round 2 keyed `sg_tree_build_from_workdir`'s
  readlink-vs-fopen choice on the index mode and broke both directions,
  with all gates green: index 120000 + worktree regular file made every
  automatic snapshot fail (`sg reset --hard --force` refused); index 100644
  + worktree symlink made `sg stash push` store the link TARGET's content
  as 100644 (git stores 120000 + the link text). The recorded mode is the
  observed one too, exec bit included (measured against `git stash push`).
- **Phase 81b: `sg add` refuses a pathspec that is "beyond a symbolic
  link"** (a symlink as a proper ancestor component, or a symlink named
  with a trailing slash) with git's own wording, exit 1, in a PRE-PASS
  over every argument before any object is written: `sg add f.txt
  ld/a.txt` writes nothing, not even f.txt's blob (git writes nothing
  either). The escape case matters: without it `sg add eo/x` (eo ->
  ../out) copied a file from OUTSIDE the repository into the index.
  Staging a non-directory at `P` evicts every index entry under `P/`
  (`sg_index_remove_under`, `P/` boundary -- `dir2/x`, `dir.txt` and
  `dir-x/y` survive `sg add dir`).

## Phase 81c: the worktree write path creates real symlinks

`sg_write_file_worktree` now dispatches on the caller's mode TYPE bits
(`mode & 0170000`), not on permission bits alone: `0120000` calls
`symlink()` with the blob bytes as the target, anything else takes the
old `open(O_EXCL|O_NOFOLLOW)` + `fwrite` + `chmod` path. Three rules come
with it, all measured:

- **Callers must pass the FULL mode.** All six writers used to mask with
  `& 0777` (apply.c, merge.c x2, cmd_restore.c, stash.c x2), which
  collapsed 120000 to 0 before the function ever saw it. While that mask
  was there, changing `sg_write_file_worktree` alone was a no-op -- if a
  seventh writer appears, it inherits this requirement.
- **WARNING: never `chmod()` a path you just created as a symlink.**
  `chmod()` FOLLOWS a symlink, so it would silently change the TARGET
  file's permissions -- a file that may be anywhere, including outside
  the repository. The symlink branch returns before the chmod block for
  exactly this reason. `tests/test_worktree_write.c`'s
  `test_write_symlink_never_chmods_target` plants a 0400 file outside the
  repo and asserts its bits survive; mutation c11 (adding a chmod back)
  reds it.
- The regular-file branch masks explicitly: `chmod(abs, mode & 07777)`.
  POSIX leaves chmod's behaviour undefined for bits outside 07777; macOS
  and Linux both ignore them (measured), so this removes a platform
  freedom rather than documenting it. It is deliberately NOT observable
  on either CI platform -- do not go looking for the test that proves it.

Everything Phase 80 guaranteed still holds: the ancestor `lstat` walk and
the final-component `unlink` run BEFORE the dispatch, so a symlink is
never created through a symlinked ancestor, and `symlink()` is itself
exclusive (it fails if the path exists).

**The index's cached size follows git's rule, which is neither "what we
wrote" nor "what lstat says"** (apply.c, stash.c, and merge.c's
`add_resolved_entry`). Measured, git 2.55.0, hand-built 120000 blobs: a
target that landed intact records its true lstat size (5 for `f.txt`); a
target truncated at an embedded NUL records **0**. The 0 is deliberate --
size, mtime, ino and mode would otherwise ALL match the link git just
wrote, so a stat-only reader would call a link whose content does not
match its blob CLEAN. sg follows: a mismatch between the length asked for
and the length that landed zeroes the cached size.
WARNING: **`st.st_size` is the wrong answer here and it looks like the
right one.** A cold read recommended it; measuring git showed it is the
one value git avoids. The reverse mutation (c20) is pinned by
`phase81c c19 index`, the only check that can tell the two apart.
Truncation at a NUL is git's own behaviour (`symlink()` takes a C
string): do not pre-scan the target, do not reject it, do not translate
it.
