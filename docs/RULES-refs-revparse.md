# Rules: refs, HEAD, and revision parsing

Scope: `src/storage/refs.c`, `src/storage/revparse.c`, `include/sg/refs.h`, `include/sg/revparse.h`; also covers detached HEAD as a state, `sg merge <rev>`'s revision handling, and `sg_message_cleanup`.

**Read this file before editing anything in that scope.** It is not
background reading: every rule here was measured against real git 2.55.0,
and most of the `WARNING:`s exist because something slipped past a fully
green board. Project-wide rules -- the gates, the code conventions, the
deliberate divergences -- stay in `CLAUDE.md`; the full derivations and
measurement tables stay in `docs/DESIGN.md` (look up a specific section,
do not read the whole thing).

- **All ref and HEAD writes always go through `sg_ref_update` /
  `sg_ref_set_head` / `sg_ref_set_head_detached`** (`include/sg/refs.h`), do
  not hand-roll `fopen` writes to ref files, and do not duplicate
  `write_ref_file` again. The third function was added in Phase 18, to write
  HEAD as a bare 40-hex (detached). **Do not take the shortcut of using
  `sg_ref_update(git_dir, "HEAD", ...)` instead** -- it produces the same
  file, but it gets old_id via `sg_ref_read_path`, and when HEAD is still a
  symref, hex parsing necessarily fails and is silently recorded as all
  zeros, so "detaching from A to B" gets written as "B created out of
  nowhere".

  WARNING: **"logs/HEAD is always appended, even for a no-op" is a property
  of the MIRRORING path, not of HEAD's file** (measured in Phase 48, and a
  pre-existing bug until then). When HEAD is written **directly** -- i.e.
  while DETACHED -- it obeys the ordinary `old != new` rule like any other
  ref: real git logs nothing for a detached no-op (`reset --hard HEAD`,
  `stash` on a detached HEAD), but still logs the **symbolic-to-detached
  transition** even when the commit does not change. The condition is
  therefore "HEAD was ALREADY detached **and** old == new", it lives in
  `sg_ref_set_head_detached` (the single writer of a detached HEAD), and a
  corrupt HEAD deliberately counts as "not already detached" and still logs.
  Suppressing too much is as wrong as suppressing too little; interop pins
  all three corners in one fixture.

  **An arbitrary SYMBOLIC ref goes through `sg_ref_set_symref`** (Phase 48),
  which is `sg_ref_set_head`'s shape generalized to any ref path -- append
  the log line first, write the file, truncate the log back off if the write
  fails -- and inherits the namespace policy below unchanged. Its one caller
  is `sg clone`, which creates `refs/remotes/<remote>/HEAD`; note its old_id
  reads as all-zeros for a symref that already exists (a `ref: ...` file has
  no id to parse), the same limitation `sg_ref_set_head_detached` documents.

  The two asymmetric reflog rules (a concrete ref's log is only appended when
  `old != new`; `logs/HEAD` is always appended, and is byte-for-byte identical
  to the line for the branch it points at) and the policy gate for "which
  namespaces even get a log" are all contained inside these three functions;
  bypassing them causes silent misses -- no error, the reflog lines just
  quietly do not exist (Phase 17). **Those two rules are not just constraints
  to obey, they are also tools you can use**: Phase 18's rebase finish relies
  on the ordering "update the branch first (HEAD is still detached ->
  not mirrored), then reattach HEAD (old==new but HEAD does not suppress
  no-ops)" to grow real git's reflog shape without writing any special case.

  The combination "move whatever HEAD points at to some commit" (write HEAD
  if detached, otherwise write `refs/heads/<branch>`) has been extracted into
  **`sg_ref_move_head`** (Phase 19), shared by `commit`/`reset`/`merge`. The
  caller passes `branch == NULL` to mean detached, **a corrupt HEAD must be
  blocked by the caller first** -- NULL alone cannot distinguish the two
  cases, and if the function guessed on its own it would wash out the
  distinction Phase 18 worked hard to establish.
- Remote/user strings must pass through a gate function before becoming a
  file path: `sg_ref_name_is_safe` (`include/sg/transport.h:38`),
  `sg_ref_branch_name_is_safe` (`include/sg/refs.h:13`). **Creating** a new
  ref has a separate check-ref-format validator,
  `sg_ref_name_valid_for_create` (`include/sg/refs.h`), shared by branch and
  tag; the three have different rules, the header comment documents the
  division of labor, picking the wrong one leaves a hole.
- **detached HEAD is a first-class state (Phase 18)**. `sg_ref_resolve_head`'s
  -1 now means **only** unborn HEAD, no longer also detached -- do not write
  code that treats "resolve failed" as "not on a branch". To ask "is it
  detached", use `sg_ref_head_is_detached`, which is tri-state: 1 detached,
  0 symbolic, **-1 corrupt**. Corrupt is deliberately kept separate from
  detached, because the detached answer is exactly what a caller uses to
  decide "is it safe to write a bare sha into HEAD" -- merging the two would
  wash a corrupt HEAD into looking like a normal state. `sg_ref_current_branch`
  returning NULL has these same two causes, and the four commands that used
  to reject on it (merge/reset/rebase/push) have all been split apart.
  Of these, **merge and rebase were changed in Phase 19 to allow detached and
  only reject corrupt**, leaving only push still rejecting unconditionally.
  This entry used to add "its HEAD check comes after the remote ref
  advertisement, unreachable without a live remote, so it cannot be tested".
  **That was wrong, measured in Phase 40**: the refusal fires BEFORE any
  connection attempt, so a remote whose URL simply never connects
  (`http://127.0.0.1:9/`) is enough and no HTTP server is needed -- interop
  now pins it, with two control groups (re-attaching HEAD, and giving an
  explicit refspec) separating the refusal from the dead URL, since a push
  that refused for any reason at all would otherwise look identical.
  Note the explicit-refspec control is not just test scaffolding: since
  Phase 39 a named refspec deliberately bypasses this check entirely
  (`sg push origin HEAD:refs/heads/x` proceeds), matching real git.

  **`current_branch == NULL` now flows through the entire merge/rebase
  path**; when adding or modifying code in those two, any place that feeds it
  into `%s` needs to guard it itself. Phase 19 fixed three spots for this:
  `sg_merge_trees`'s `ours_label` (NULL would segfault while writing conflict
  markers), the rebase description in `cmd_status.c`, and the fast-forward
  shortcut in `cmd_rebase.c` -- **that last one printed
  `Fast-forwarded (null) to master.` while the entire test suite stayed
  green**, because that shortcut returns before any other path and the test
  discarded stdout. On this platform, `%s` fed NULL just prints `(null)`
  without crashing, and even the exit code is 0, so all three CI cells pass
  silently. **When adding a detached-specific message, also add a stdout
  assertion**, verifying only files and reflog misses the whole dimension.

  While detached, both merge and rebase **do not touch any branch ref**;
  rebase goes further and does not even write the `rebase (finish)` reflog
  line (`finish_rebase` returns 0 immediately when `branch == NULL`). This is
  not a special case, it is the natural degeneration of the two-step model
  "move the branch first, then reattach HEAD" when there is no branch --
  measured against real git, this is exactly the shape it takes. The rebase
  sequencer uses the on-disk sentinel `detached HEAD` (the same string as
  real git's `head-name`) to record that it started out detached, while in
  memory it is NULL; **an absent `orig-branch` file still counts as
  corrupt**, it must not be treated as detached.
- **`sg merge <rev>` takes any revision since Phase 43** (tag, `refs/...`
  path, `~N`/`^N`, 40-hex), not just a bare branch name. The merge MESSAGE
  follows git's naming rules, all measured: the name printed is **the
  argument as typed** (`refs/heads/topic` keeps its prefix), a trailing `^`
  run or `~<digits>` is **stripped before classifying and the shortened name
  is printed** (`topic~0` -> `Merge branch 'topic'`), and the form is
  branch/tag/commit accordingly. The ` into <branch>` suffix is omitted on
  exactly `master` and `main` -- **hard-coded, not `init.defaultBranch`**
  (measured: setting that to `trunk` still appends ` into trunk`); a detached
  HEAD gets ` into HEAD`. The conflict marker's theirs label is the argument
  as typed in every form. See Phase 43 of `docs/DESIGN.md` for the tables.
  **The fast-forward output matches git as of Phase 50**: `Updating
  <7hex>..<7hex>`, `Fast-forward`, then exactly
  `git diff --stat --summary <old> <new>` -- rename detection ON (git diff's
  own default), byte-identical to git's, pinned in interop's `phase50` group.
  WARNING: **an UNBORN HEAD prints NOTHING AT ALL**, not even
  `Fast-forward`, while still moving HEAD (measured). That is the whole
  reason `do_fast_forward` takes `ours_commit` as a pointer that is **NULL
  when unborn** -- no separate flag, no zero-id sentinel. Printing a header
  there is the obvious-looking "fix" and is wrong.
  WARNING: **the `mode change` summary line DROPS its path when it follows a
  `rename` line for the same entry** (git already named it one line up), and
  an EMPTY diff prints the two header lines and nothing else -- an empty
  `--stat` is empty, not " 0 files changed". Both measured.
  The rest of `sg merge`'s stdout is still sg's own vocabulary and is still
  not compared against git's; Phase 50 changed this one output, not the
  command's voice.
- **Resolving an OBJECT name (any type, tag NOT peeled) goes through
  `sg_rev_parse_object`** (`include/sg/revparse.h`, Phase 56): a 40-hex id, a
  ref (HEAD/branch/tag), anything `sg_rev_parse_commit`'s grammar accepts, or
  `<rev>:<path>`. `sg show`, `sg cat-file` and `sg log`'s siblings share it --
  those were **exactly the three files** that printed "not a valid object
  id/name", and it was converged before a third copy set in.
  WARNING: **`cat-file` does NOT peel an annotated tag while `merge-base`
  DOES** (both measured: `cat-file -t v1` says `tag` and `-p v1` prints the
  tag object's body; `merge-base v1 topic` answers with a commit). That is
  why one takes `sg_rev_parse_object` and the other `sg_rev_parse_commit`.
  Interop pins both against the same tag as a head-on pair -- a single shared
  rule fails whichever half it was not written for.
  WARNING: **`^{tree}` / `^{commit}` peel syntax is NOT implemented** and is
  refused, never approximated. The rejection is clean by construction, not by
  accident: the base scan stops at the first `~`/`^`/`@{` and the suffix must
  then be all decimal digits, so `{tree}` fails to parse rather than being
  read as `^0`. git accepts it and exits 0; sg exits 1 (the existing
  exit-code divergence). Pinned on both sides.
  WARNING: **-3 is not -1.** A well-formed 40-hex whose object cannot be read
  is MISSING OR CORRUPT, not an invalid name -- the resolver must read an
  object to learn its type, so a failed read looks like a failed resolve
  unless it is distinguished. Sharing the resolver regressed this once and an
  interop check from an earlier phase caught it by pinning the WORDING (a
  packed REF_DELTA whose base is gone must say "not found or corrupt", so the
  reader looks at the pack and not at their own typing). An error message is
  part of the interface.
- **A user-supplied revision string always goes through
  `sg_rev_parse_commit`** (`include/sg/revparse.h`): `HEAD`/tag/branch/full
  40-hex/full `refs/...` path, plus `~N`/`^N`/`@{N}` (Phase 17, reflog index,
  must immediately follow the ref name, digits only, `@{<date>}`/
  `@{upstream}` are not supported), and it peels annotated tags.
  **Bare `@{N}` and bare `@` are supported as of Phase 48**, and the first is
  easy to get backwards: **`@{N}` reads the CURRENT BRANCH's log, not
  HEAD's** -- measurably a different commit once a checkout away and back has
  added lines to `logs/HEAD` and none to the branch's, and git's own
  out-of-range message names the branch. Detached falls back to `logs/HEAD`;
  unborn is rejected; **a corrupt HEAD must be rejected too, which is why the
  predicate is `sg_ref_head_is_detached`'s tri-state and not a NULL test on
  `sg_ref_current_branch`** (Phase 18's rule). Bare `@` is HEAD, suffixes
  included. Both work by rewriting `base` before anything else runs, so
  `@{1}~1` and `@~1` come for free -- but the rewrite must not swallow an
  empty base in general: `~1`, `^` and `@{` alone are still parse errors.
  WARNING: **`@{0}` is the ref's CURRENT VALUE, not the reflog's newest
  `new_id`** (Phase 70b). The two differ whenever a ref moved without a
  matching reflog append: a hand-edited ref file, or -- reachable with no
  hand-editing at all -- a SYMREF whose target later moved, which is
  exactly what `sg clone` leaves behind (`sg_ref_set_symref` writes
  `refs/remotes/<remote>/HEAD` AND, since `ref_path_reflog_allowed` permits
  `refs/remotes/`, a reflog for it) once a later `sg fetch` moves the
  branch it points at. Measured: `b@{0}` is git's c5 (the file) and was
  sg's c2 (the log); `@{N>=1}` agrees with git even on an inconsistent log,
  so **only N == 0 changes**. This was a PRE-EXISTING bug that Phase 70
  merely made reachable through ordinary commands, and two of this
  project's OWN unit tests were asserting the wrong answer, because their
  fixtures happened to embed the inconsistency -- the property they exist
  to demonstrate now sits at `@{1}`.
  WARNING: **the reflog must still EXIST for `@{0}`** -- only the SOURCE of
  the oid changed, never the existence gate. `sg_reflog_at(&log, 0)` is
  still required to return an entry, which is what keeps `<branch>@{0}`
  refusing when the log has been deleted, matching git.
  WARNING: **one measured case is deliberately NOT reproduced** -- with the
  current branch's reflog file deleted by hand, real git lets `@{0}` fall
  back to the branch tip while still rejecting `<branch>@{0}`; sg rejects
  both, rather than inventing an asymmetry between its own two spellings.
  It is not on the deliberate-divergence list (reaching it takes deleting a
  log file by hand); see Phase 48 of `docs/DESIGN.md`.

  WARNING: **this rule governs revision RESOLUTION only, and as of Phase 75
  `sg reflog`'s LISTING deliberately goes the other way** -- it reproduces
  git's asymmetry rather than rejecting uniformly: with the current branch's
  log file deleted, `sg reflog @{0}` exits 0 printing nothing and
  `sg reflog @{1}` says `log for refs/heads/<branch> is empty`, while the
  spelled-out `sg reflog <branch>@{0}` still refuses. So in that one state
  `sg log @{0}` refuses where `sg reflog @{0}` succeeds, and that is
  intentional: here the fallback decides which COMMIT a revision means and
  git's answer drags in branch-tip semantics this project does not want,
  whereas for the listing git's answer is only what to print and costs
  nothing to match. Do not "unify" the two layers on the strength of either
  rule alone -- each is pinned at its own answer (interop's `phase75 round6`
  group for the listing), and Phase 75's own section of `docs/DESIGN.md`
  records the measurement for both.
  **Abbreviated (prefix) object ids are supported as of Phase 68** -- this
  line used to say they deliberately were not; it is gone rather than marked
  "fixed", the same convention this project uses for every closed gap. The
  rules, all measured against git 2.55.0 (Phase 68 of `docs/DESIGN.md` has
  the fixtures):
  - **4..39 hex, case-insensitive; 40 is an exact id, not a prefix.** The
    minimum really is 4, not "whatever is unique" -- a 1/2/3-char prefix
    fails even when it matches exactly one object. Enumeration is
    `sg_object_find_prefix` (`include/sg/objstore.h`), which scans the one
    `objects/<xx>/` the first byte names plus every pack idx, then sorts and
    DEDUPLICATES (an object can be both loose and packed).
  - **`core.abbrev` affects OUTPUT only, never resolution** (measured:
    `-c core.abbrev=40` still resolves a 5-hex prefix). sg reads no config,
    so this costs nothing -- but do not "add" it to the resolver.
  - **The prefix branch sits AFTER the ref lookup, and the two lengths
    resolve in OPPOSITE directions**: a branch literally named with a valid
    6-hex prefix WINS over the prefix reading, while a branch named with a
    full 40-hex LOSES to the object id. Both are pinned head-on; collapsing
    them into one rule breaks whichever was not measured last.
  - **THREE disambiguation modes, not two** (`sg_rev_disambig`): STRICT
    (git's `get_oid`), COMMITTISH (`get_oid_committish`), TREEISH
    (`get_oid_treeish`). On one 4-way collision the hint list comes back at
    three widths -- 4 rows bare, 2 rows for `<amb>~1`, **3 rows for
    `<amb>:f.txt`**. Answering the third with COMMITTISH is a WRONG ANSWER,
    not a missing row: on a commit+tree+blob prefix git's tree-ish count is
    2 and it refuses, while a commit-ish count of 1 resolves.
  - **TWO dwim triggers, and one of them is not the command.** Per command:
    `sg log`, `sg reset`, and (Phase 69) `sg rebase`'s `<upstream>` pass
    COMMITTISH (measured over 15 git commands -- `rev-list` REFUSES while
    `log` resolves, so the rule is not "this command needs a commit"; `git
    rebase` was measured separately in Phase 69 and resolves a prefix with
    exactly one commit-ish candidate the same way `log`/`reset --hard` do,
    while `merge`/`show`/`cherry-pick`/`revert`/`switch --detach` all
    refuse it -- using STRICT for `sg rebase` would be a wrong answer, not
    a smaller one). Independently, **any `~`/`^`/`@{`
    suffix forces COMMITTISH and a `:` forces TREEISH, whatever the caller
    asked for**, with the suffix winning over the colon; the suffix scan is
    bounded at the colon so a PATH containing a `~` is not mistaken for one.
    Both triggers live in `sg_rev_effective_disambig` -- **do not re-derive
    either at a call site**, they had two copies once and it cost a phase.
  - **Membership is decided by the PEELED type, never the raw type.** A tag
    counts toward COMMITTISH only if it peels to a commit (TREEISH: a commit
    or a tree). Decisive fixture: a tag->blob colliding with a real commit --
    counting by raw type gives two commit-ish candidates and refuses, git
    resolves to the commit.
  - **An ambiguous prefix returns -4**, and the CLI prints git's block
    byte-for-byte via `sg_cli_report_ambiguous_oid` (`include/sg/cli_args.h`):
    `error: short object ID <prefix, LOWERCASED> is ambiguous`, then `hint:`
    rows ordered by TYPE (tag, commit, tree, blob) and then by hex. A commit
    row carries the AUTHOR date in the commit's OWN stored offset
    (`sg_date_format_short`) and the FOLDED subject (`fold_subject`, do not
    write "the first line" at a new call site); a tag row carries the tag
    NAME, not its message. The list is narrowed to the mode's matching rows
    ONLY when at least one candidate matches; with none, every row prints.
    git exits 128 here and sg exits 1, the standing 0-or-1 convention --
    **except `sg push`, where both exit 1** (measured; pinned, because
    "unifying" it would otherwise go unnoticed).
  - `sequencer/todo` READS 4..40 hex as of Phase 68c and still WRITES 40
    (wide-in / narrow-out); see `docs/RULES-sequencer.md`'s cherry-pick
    bullet.
  WARNING: **`sg_rev_parse_ref_path` implements git's full six-rule
  gitrevisions lookup table as of Phase 70** -- this used to say a `<base>`
  of the form `heads/<name>` or `tags/<name>` was REFUSED (a real, pinned
  gap found by Phase 69's out-of-repo oracle harness); it is gone from
  this list rather than marked "fixed" in place, the same convention this
  file uses everywhere else for a closed gap. The table, tried in order
  with NO early return (a miss at any rule falls through to the next):
  `"%s"` (any file under `$GIT_DIR` whose first 40 bytes are hex --
  general rule 1, closes `MERGE_HEAD`/`ORIG_HEAD`/`CHERRY_PICK_HEAD`/
  `REVERT_HEAD` spellings for free), `"refs/%s"`, `"refs/tags/%s"`,
  `"refs/heads/%s"`, `"refs/remotes/%s"`, `"refs/remotes/%s/HEAD"`. Rule 2
  sits BEFORE rules 3/4 and changes answers sg used to give with exit 0
  (measured, Phase 70 spec section 2.2): a bare name that collides with a
  same-named ref literally living at `refs/<name>` used to resolve to the
  tag or branch instead. **`sg_ref_path_components_are_safe`**
  (`include/sg/refs.h`, NOT a tightening of `sg_ref_branch_name_is_safe`,
  which has too many other callers to converge blindly) rejects an empty
  name, a leading/trailing `/`, any empty path component, and any component
  that IS `.` or `..` BEFORE any rule is tried. It was file-local to
  `revparse.c` in Phase 70 and promoted to `refs.c` in Phase 70b, because
  `sg_ref_read_path_resolved` needs the identical check on every symref hop
  target it reads off disk. **Those are TWO guards on TWO sources (argv vs.
  disk content) and neither is redundant** -- the hop-target one is
  deliberately NOT applied to `_resolved`'s own incoming `ref_path` (that is
  revparse's job), so a mutation breaking either guard turns a DIFFERENT set
  of checks red; if one mutation reds both, a guard has stopped earning its
  place -- this closes a real pre-existing bug where a
  loose ref's `//`/`/./ ` collapsed at the OS level while a packed ref's
  exact `strcmp` did not, giving two different answers for the same
  spelling depending on whether `git pack-refs` had run.
  Rules 5/6 need `sg_ref_read_path_resolved` (`refs.c`), a NEW,
  symref-following sibling of `sg_ref_read_path` (which stays
  non-following, unchanged, for every existing caller) -- because
  `refs/remotes/<name>/HEAD` is ordinarily a symref, exactly the shape
  `sg clone` itself creates via `sg_ref_set_symref`. Bounded at 5 reads
  total (4 hops), matching real git's own measured dangling-symref cutoff;
  this bound doubles as cycle detection, no separate visited-set. **Both
  `resolve_base` and `sg_rev_parse_object` had to switch their own
  downstream re-read of the resolved ref path from `sg_ref_read_path` to
  `sg_ref_read_path_resolved`** -- found only by writing the implementation,
  not anticipated by the spec: `sg_rev_parse_ref_path` can now hand back a
  ref path that is ITSELF a symref (e.g. `refs/remotes/origin/HEAD`), and
  re-reading it with the non-following function fails to hex-decode the
  `"ref: ..."` line. `sg_rev_parse_object`'s own copy needed the identical
  fix for the identical reason, one call site down; a mutation reverting
  either one alone is caught (the former by `sg_rev_parse_commit("origin")`
  resolving through a symref chain, the latter only by pointing the chain
  at an ANNOTATED TAG OBJECT rather than a commit -- a commit target lets
  the broken direct path silently fall through to `sg_rev_parse_commit`'s
  own already-fixed fallback and land on the same answer by coincidence,
  since that fallback peels tags and `sg_rev_parse_object` must not).
  Pinned on both sides in interop's `phase70` group. To list/delete refs under any
  prefix use `sg_ref_list_under`/`sg_ref_delete_under` (`prefix` must end
  with `/`).
  WARNING: **`sg tag <new> <annotated-tag>` PEELS where git does not, a
  real, pinned, PRE-EXISTING gap unrelated to Phase 70 -- `cmd_tag.c`'s
  own topic, not revparse's.** Measured on the PRE-Phase-70 binary: `git
  tag new atag` (or `refs/tags/atag`) creates `refs/tags/new` as a TAG
  object pointing at the same tag `atag` does; sg creates it pointing
  straight at the underlying COMMIT, for both of those spellings already.
  Phase 70 only adds a THIRD spelling (`tags/atag`) that reaches the
  identical pre-existing bug, previously refused outright. Two other
  dimensions are CORRECT and not part of this gap: `sg cat-file -t
  tags/atag` answers `tag` (unpeeled), and `sg switch --detach tags/atag`
  lands on the same commit as git on both sides. Pinned by name in
  interop (git side `tag`, sg side `commit`) so closing it later turns
  that check red rather than silently changing what it compares -- same
  convention as the `heads/<name>` gap Phase 69 recorded and Phase 70
  closed. Deliberately NOT fixed here: it needs `sg tag`'s full matrix
  (`-a`, `-f`, lightweight vs annotated) measured first, in its own phase.
  **Exception: `sg push`'s explicit-dst refspec `<src>` (Phase 39,
  `src/cli/cmd_push.c`'s `resolve_refspec_src`) must NOT use this
  function** -- `sg_rev_parse_commit` peels annotated tags by definition,
  but measured against real git: `v2:refs/tags/v2copy` leaves the remote's
  `refs/tags/v2copy` as a **tag** object, not the commit it points at.
  `resolve_refspec_src` tries an exact, unpeeled ref lookup first
  (`refs/tags/<src>`/`refs/heads/<src>`/an already-`refs/`-qualified
  `<src>`), then (Phase 70b) the REST of the gitrevisions table the same
  unpeeled way via `sg_rev_parse_ref_path` + `sg_ref_read_path_resolved`,
  and only then falls back to `sg_rev_parse_commit`.
  WARNING: **that middle step exists because Phase 70 silently broke this
  rule and no gate noticed.** Once `tags/<name>` became a resolvable
  spelling, it missed all three literal lookups (none of them tries
  `refs/tags/tags/<name>`) and fell into the peeling fallback, so
  `sg push origin tags/<annotated-tag>:<dst>` pushed the underlying COMMIT
  where git pushes the TAG object -- exit 0 on both sides, visible only in
  the remote ref's object TYPE. Measured over a real `git http-backend`
  push, with the two literal spellings as controls (both correct before and
  after). The three literal lookups must STAY: they carry the
  `src refspec '%s' matches more than one` ambiguity rule, which
  `sg_rev_parse_ref_path` cannot express (it returns the first hit), so
  collapsing them into it would lose that error.
- **A user-supplied commit/tag message always goes through
  `sg_message_cleanup`** first (`include/sg/object.h`), otherwise the
  resulting object id differs from real git's. **There are two exceptions,
  both for the identical reason**: `cmd_rebase.c` and (Phase 57)
  `src/cli/pick.c`'s cherry-pick path forward an EXISTING message (the
  picked commit's own, byte-for-byte) and must preserve it exactly, so both
  deliberately skip cleanup. `pick.c`'s revert path is the opposite case --
  it CONSTRUCTS a brand-new message (the `Revert "..."`/`This reverts
  commit ...` text) and does run it through cleanup, same as any other
  newly-authored message.
- **Phase 76: `sg tag -d`'s Phase 74 batch-delete engine now lives in
  `cli/ref_delete.c` (`sg_ref_delete_batch`), used by both `cmd_tag.c` and
  `cmd_branch.c`.** See `include/sg/ref_delete.h` for the full mechanism
  (precheck/existence/gate merged into one interleaved loop, in-batch
  literal-duplicate refusal, then the O_CREAT|O_EXCL lock-collision
  detector) and the `sg_ref_delete_eexist_style` enum's own comment for a
  measured DIVERGENCE from this phase's own starting assumption: `git tag
  -d` and `git branch -d` are NOT the same C code past the ref-transaction
  layer. The literal-duplicate refusal ("multiple updates for ref '%s' not
  allowed") IS identical between the two; the lock-COLLISION wording is
  NOT -- `git tag -d Foo foo` on a case-insensitive filesystem prints "'X'
  and 'Y' are the same ref" (Phase 74's own finding), while `git branch -d
  Merged merged` for the identical shape prints git's raw lockfile.c
  sentence ("cannot lock ref '...': Unable to create '<path>': File
  exists.\n\nAnother git process seems to be running..."), with no
  in-batch-alias special case at all. `SG_REFDEL_EEXIST_ALIAS` (tag) vs
  `SG_REFDEL_EEXIST_RAW` (branch) exists specifically because this project
  MEASURED the difference rather than assuming Phase 74's tag behavior
  transferred unchanged -- do not merge the two styles back into one
  without re-measuring both commands.
- **`sg branch`'s create path resolves `<start-point>` in STRICT
  disambiguation mode** (`SG_REV_STRICT`, `cmd_branch.c`'s
  `resolve_branch_point`), unlike `sg log`/`sg reset`'s COMMITTISH --
  measured against real git 2.55.0 with a 2-way commit/blob prefix
  collision: `git log` resolves it, `git branch` refuses and lists both
  candidates. A resolved TREE or BLOB (not a nonexistent name) gets a
  message shape `sg_cli_report_rev_error`'s `sg_rev_err_kind` enum cannot
  express at all ("error: object <hex> is a tree, not a commit" followed by
  "not a valid branch point: '<arg>'") -- `cmd_branch.c` prints it directly
  rather than forcing a fifth kind into that table for one caller. The
  REV_ERR_TABLE row named `"branch"` in `cli_args.c` only covers the two
  cells that DO fit the existing R/O/P shape (a name that resolves to
  nothing at all, and a well-formed-but-missing 40-hex).
- **`sg branch <name> <start>` and `sg branch -f <name> <start>` reproduce
  git's D/F (directory/file) conflict detection for the refs/heads/
  namespace** (`cmd_branch.c`'s `check_df_conflict`), in BOTH directions
  (an existing leaf ref blocking a nested name, and an existing nested ref
  blocking a leaf name). The wording differs depending on whether the
  EXISTING, blocking ref is loose or packed-only (measured against real
  git 2.55.0): a loose blocker gets git's "cannot lock ref '<new>': '<old>'
  exists; cannot create '<new>'" wording; a packed-only blocker gets the
  plain "'<old>' exists; cannot create '<new>'" with no "cannot lock ref"
  prefix at all -- git's own ref-transaction code apparently never
  attempts (and so never reports failing) a lock for a conflict it can
  already see is packed-only.
- **`^{tree}`/`^{commit}`/`^{blob}` peel syntax remains unsupported project-
  wide** (see `revparse.h`'s own comment) -- `sg branch new HEAD^{tree}`
  is a known, pre-existing, cross-cutting gap this phase did NOT fix: sg
  reports the generic "not a valid object name: 'HEAD^{tree}'" where git
  reports the tree/blob-specific two-line wording above, because
  `sg_rev_parse_object` never resolves the peel syntax to a tree/blob
  object in the first place. Fixing it would mean teaching peel syntax to
  every caller of `sg_rev_parse_object` project-wide, not just `sg
  branch`'s start-point resolution -- out of scope for this phase, and
  pinned as a known 3-probe gap in the Phase 76 oracle rather than silently
  left unmeasured.
- **Phase 76 fix round 3: `sg_ref_lock_try`/`sg_ref_lock_release`
  (`refs.h`/`refs.c`) are the ONE `O_CREAT|O_EXCL` ref-lock implementation
  in this project.** Shared by `cli/ref_delete.c`'s batch-delete
  transaction, `cli/cmd_branch.c`'s deliberate-divergence-#10 alias probe,
  and `cmd_branch.c`'s create/`-f` write's own lock (added this round to
  close a real hole -- see CLAUDE.md's divergence #10 entry). Do not
  hand-roll a fourth `open(..., O_CREAT|O_EXCL, ...)` on a ref path
  anywhere in this project; extend this one instead. Every non-EEXIST
  failure from either of `sg_ref_lock_try`'s two possible callers-in-one-
  probe (the current branch's own lock, and the typed name's lock) must
  route through `SG_REFLOCK_ERROR`, never silently through the EEXIST arm
  or a bare 0/1 collapse -- round 2 closed this for one of the two open()
  calls in `branch_aliases_current` and missed the other; round 3 audited
  and closed both (see `docs/DESIGN.md`'s Phase 76 round-3 R2-2 section).
  `sg_ref_update` (this same file) still takes NO lock at all -- see
  `docs/DESIGN.md`'s Phase 76 residuals for why that is deliberately out
  of scope here rather than silently inconsistent.
- **Phase 76 fix round 4: `sg_ref_lock_try_query` (`refs.h`/`refs.c`) is
  the PURE-QUERY sibling of `sg_ref_lock_try`** -- same lock file, same
  EEXIST semantics, but never calls `sg_mkdir_parents`, for a caller that
  must not create anything on disk while merely asking "would this name's
  lock collide" (`cmd_branch.c`'s divergence-#10 alias probe is the one
  caller today, since it runs for every batch-delete name regardless of
  whether that name exists). Both share `ref_lock_try_impl`; do not
  duplicate the open()/close() sequence a third time. An `ENOENT`/
  `ENOTDIR` from the query variant means "this spelling resolves to
  nothing on disk" -- classifying that as an error vs. a clean negative
  result is the CALLER's decision, not this function's (see its own
  header comment); `branch_aliases_current` treats it as "not aliased",
  a different answer than the ERROR (fail-closed) treatment for every
  other errno.
- **`cli/ref_delete.c`'s batch-delete engine prunes empty ancestor
  directories after each successful delete** (Phase 76 fix round 4, D1d),
  reusing `sg_prune_empty_parents` (`workdir.h` -- read
  `docs/RULES-paths-strings.md` on it before touching this) with the
  namespace root (`refs/heads`, `refs/tags`, `logs/refs/heads`) passed as
  the `repo_root` argument so it structurally cannot remove that root.
  Applies to `sg tag -d` too (same shared engine) -- do not special-case
  branch here.
- **Phase 76 fix round 5 (P1/R4-1): `sg_ref_delete_batch`'s pass 1
  (`cli/ref_delete.c`) validates `name` with
  `sg_ref_path_components_are_safe` BEFORE its precheck callback, the
  existence check, or any other filesystem call -- state this ordering
  explicitly if you ever touch pass 1, it is load-bearing, not
  incidental.** Two things went wrong before this fix, both worth
  remembering:
  1. **A SHIPPED, pre-existing destructive bug**: `sg branch -d
     ./merged` and `sg tag -d ./lt` DELETED a ref real git says does not
     exist at all, because the OS resolves `refs/heads/./merged` to
     `refs/heads/merged` while git's own lookup treats the two spellings
     as different names. This predates Phase 76 (measured against
     master) but is fixed here because the batch-delete engine is this
     phase's own code.
  2. **Deleting round 3's `sg_ref_path_components_are_safe` guard in
     round 4 was itself a mistake, and the reasoning error is worth
     naming**: round 4 judged the guard "redundant" because removing it
     did not change the FINAL PRINTED MESSAGE for the one case it was
     measured against (`-d merged/` still said "not found" either way).
     But the guard was also the ONLY thing standing between the raw argv
     name and `sg_ref_lock_try_query`'s `open(O_CREAT|O_EXCL)` call --
     without it, `sg branch -d ../../../../x/pwn` builds a path OUTSIDE
     the repository and briefly creates a real file there before
     unlinking it. "Same answer" proved nothing about what the code
     touched on the way to that answer. See CLAUDE.md's own amended
     "redundant guard" bullet for the general rule this incident now
     documents, and this file's own INVARIANT comment (next to
     `sg_ref_lock_try`/`sg_ref_lock_try_query` in `refs.h`) for where the
     restored gate now lives.
- **A `tests/interop.sh` check that scans a git-built fixture for `*.lock`
  must scope to `.git/refs` and to the sg-side directories only, never
  the whole `.git` tree and never a git-side (`*_git`) or "untouched
  reference" fixture** (Phase 76 fix round 8, T1). Measured (main
  conversation, 2026-09-14, git 2.55.0, macOS): in a fresh repo, right
  after `git commit`, a `*.lock` scan of the whole `.git` tree saw
  `.git/objects/maintenance.lock` in 22 of 40 trials (git's own detached
  auto-maintenance), 0 of 40 with `git config maintenance.auto false` and
  `git config gc.auto 0` set in the fixture right after `git init`. sg's
  own ref locks can only ever live under `.git/refs`, so a scan of
  anything wider both risks this exact flake AND fails to discriminate a
  real sg lock leak from git's own background lock. This caused one
  observed flaky `FAIL` in round 7 (`phase76 divergence #10: no .lock
  file anywhere under .git ...`), traced to `.git/objects/maintenance.lock`
  on the git-side/untouched fixtures the scan should never have reached.
  Fix applied on both fronts (scope AND fixture quiescence), not either
  alone -- the six pre-existing phase73/74 `tag -d` lock-scan checks
  (`refs/heads`/`refs/tags`... roots) were already scoped correctly and
  were left untouched. Every lock-leak check must also print the actual
  leftover path(s) on failure, not just fail silently -- a red run should
  name the file, not just say "found something".
