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
