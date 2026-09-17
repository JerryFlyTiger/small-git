# Rules: `sg status` -- quoting, untracked folding, pathspec, the long format

Scope: `src/cli/cmd_status.c`, `src/workdir/status.c`, `include/sg/status.h`.

**Read this file before editing anything in that scope.** It is not
background reading: every rule here was measured against real git 2.55.0,
and most of the `WARNING:`s exist because something slipped past a fully
green board. Project-wide rules -- the gates, the code conventions, the
deliberate divergences -- stay in `CLAUDE.md`; the full derivations and
measurement tables stay in `docs/DESIGN.md` (look up a specific section,
do not read the whole thing).

- **The two quoting rules must not be "unified"** (Phase 25):
  `sg status --porcelain`/`-s` uses `sg_quote_path_porcelain` -- **it quotes
  as soon as the path contains a space**, because the `?? ` prefix turns a
  space into a field separator; the long format and the four machine formats
  use `sg_quote_path` -- **spaces are not quoted**. Both quote control
  characters. `tests/interop.sh` has a set of **head-on colliding** checks
  guarding this (the same `has space.txt` must be quoted in porcelain and
  must not be quoted in the long format), because if the two were collapsed
  into one shared wrong rule, all the `cmp` checks would still be green.
- **The folding parameter of `sg_status_list_untracked` is mandatory** (Phase
  25), for exactly the same reason as `sg_workdir_missing` in
  `sg_tree_build_from_workdir`: silently picking one side is precisely the
  bug it exists to eliminate. `safety/stash.c` and `workdir/tree_build.c`
  always pass "do not fold" -- they need **real filenames**, folding would
  make `sg stash -u` store a directory path.
- **`sg status`/`sg stash push` gained pathspec support in Phase 37.**
  `sg_status_list_untracked` also gained a `const sg_pathspec *ps` parameter,
  and this is the **one deliberate, named exception** to the "filter after
  the builder, never inside it" rule the diff/status pipeline otherwise
  follows everywhere else (see `sg_diff_list_filter`'s and
  `sg_status_diff_staged`'s own entries in
  `docs/RULES-pathspec-rename.md`). Reason: how deep a
  wholly-untracked directory folds is itself a function of the pathspec, not
  a fixed shape you can filter after the fact -- measured against git
  2.55.0: `-- wholly/u1.txt` lists that one file instead of folding to
  `wholly/`, while `-- 'wholly/*'` still folds to `wholly/` despite matching
  individual files below it. A folded entry `"wholly/"` cannot be matched
  against a spec naming a file below it after the walk is already done; the
  file would silently vanish instead of being unfolded. `NULL` matches
  everything and reproduces the pre-Phase-37 walk exactly; `safety/stash.c`
  and `workdir/tree_build.c` still pass `NULL` for their own unfiltered
  calls (`sg_tree_build_from_untracked`'s own `-u`/`-a` listing, and
  `sg_tree_build_from_workdir`'s snapshot use), only `sg_stash_push`'s
  partial-pathspec path passes a real one.
  **`sg_status_diff_staged` filters between `sg_diff_tree_index` and
  `sg_diff_detect_renames`**, same ordering rule as `sg_diff_list_filter`
  and for the identical Phase 29 reason (filtering after rename detection
  can turn a real rename into a plain `A` because only half the pair
  survives). The two `cmd_status.c` printers that scan `idx` directly
  (`print_unmerged`, `print_porcelain_tracked`) bypass every `sg_status_list`
  and so each needed its own `sg_pathspec_matches` call -- this is the
  single easiest site to miss when threading a pathspec through `sg
  status`, there is no list to filter, only a raw index scan.
  **`sg status` has no rev/path disambiguation at all** (unlike `sg diff`):
  measured, `git status master` (a real branch name) prints nothing and
  exits 0 -- every positional argument is a pathspec, full stop.
  **`sg stash push`'s partial pathspec push needed a THIRD, orthogonal
  dimension**, not a second `sg_workdir_missing` value: `sg_workdir_missing`
  already decides "how to record a path whose file is gone"
  (`KEEP_INDEX_BLOB` vs `RECORD_DELETION`), but a partial push also needs
  "does this path count this round at all" -- a property of the pathspec,
  independent of that path's own on-disk state. `sg_tree_build_from_workdir`
  therefore gained a separate `const sg_pathspec *ps` parameter: for a path
  `ps` does not match, the working tree is never even looked at (no lstat,
  no read, no hash) -- the index's own blob and mode are copied straight
  through, regardless of which `missing` policy the call was given and
  regardless of whether the file on disk was deleted, unreadable, or simply
  unchanged. This is also why an unmatched path's working-tree **deletion**
  must never leak into the stash's own tree: the working tree for that path
  is never consulted at all, so there is nothing for a deletion to be
  recorded from.
  **`sg_apply_tree_to_workdir` deliberately gained NO pathspec parameter**
  -- it is the shared whole-tree entry point for switch/reset --hard/merge/
  undo/stash, and giving it a filter would put every one of those five call
  sites on the hook for one feature's risk. `sg_stash_push`'s partial
  restore step instead uses a private, narrower per-path reimplementation
  (`restore_matched_paths` in `safety/stash.c`) confined to `sg stash
  push`'s own two call sites (the HEAD reset, and the `--keep-index`
  re-layering on top of it).
  **"Did the pathspec match anything real at all" is a brand-new question
  this codebase never had to answer before Phase 37** -- not even `sg diff`/
  `sg status` ask it (both are silently exit-0 on a pathspec matching
  nothing). `sg stash push -- <pathspec>` answers it and refuses (a new
  return code, 2) when nothing matches, even when the working tree has
  OTHER, unrelated dirty paths the pathspec simply does not name; nothing
  is written when this fires, checked and returned before any tree is even
  built. **Do not "unify" this with `sg status`/`sg diff`'s silent-exit-0
  rule** -- `tests/interop.sh` has a head-on colliding pair
  (`sg status -- nosuch` exit 0 no output vs. `sg stash push -- nosuch` exit
  1) guarding exactly this divergence.
- **The git side of Phase 38's comparison must declare three environment
  axes, and they live in one `P38_GIT_FLAGS` variable plus `LC_ALL=C`**:
  the locale (this machine's git is zh_TW-localized), `core.quotepath=false`
  (sg emits `>=0x80` raw), and **`advice.statusHints=true`**. That third one
  is not defensive padding -- Phase 38 was green on all five local gates and
  red on GitHub's **macOS** runner for 21 of 34 cases, with ubuntu green and
  git 2.55.0 on every machine. `advice.statusHints=false` strips the
  `(use "git add" to track)` tail off the **closing summary line**, not just
  the indented hint lines the skeleton already filters, so the failing set
  was exactly "every case carrying a non-indented parenthetical".
  **Do not answer a future instance of this by filtering more lines** -- that
  trades away the coverage the phase exists to provide. Name the knob on the
  command line instead. `phase38 oracle: precondition -- the pinned flags
  keep git's closing-line parenthetical` probes through the same variable so
  a dropped pin names its own cause instead of producing 21 silent `cmp`
  failures.

- **`sg status`'s long format has a skeleton oracle since Phase 38**
  (`tests/interop.sh`'s `phase38:` group, 34 fixture x flag cases plus one
  label-only comparison as of round 2, and named unit assertions in
  `tests/test_status_long_format.c`). The comparison drops exactly two line
  classes and cmp's everything else byte-for-byte against real git, no
  tool-name normalization: lines starting `  (` (hint lines -- both tools
  word these differently on purpose, e.g. `sg add` vs `git add`), and lines
  starting with a tab (path lines). **The hint-line rule is `^  (`, not
  `^  (use "`** -- narrowing it that way was tried and found to silently miss
  a real divergence, because a conflict state's
  `  (fix conflicts and run "git commit")` line does not start with
  `(use "`. **The tab-line rule's coverage claim is narrower than it looks,
  and this was measured, not assumed**: mutating `kind_label`'s
  `"modified:   "` to `"MODIFIED:   "` and rerunning `--interop` did turn
  `phase23: sg status's untracked paths match real git's byte-for-byte` red
  -- but only because that check's preprocessing (`sed -n 's/^\t//p'`) keeps
  the label attached to the path on the same line (see Q3's own comment).
  So the tab-line class is guarded for the untracked case (Phase 23), the
  untracked section (Phase 25), and staged-section ordering (Phase 32) --
  **not** for every one of `unmerged_label`'s seven strings. As of Phase 45
  **all seven have a real-git oracle**. Phase 38 round 2 covered four
  (`both added:`, `both modified:`, `deleted by them:`, `deleted by us:`,
  via `phase38: sg status's unmerged labels match real git byte-for-byte`,
  using Q3's own strip-tab-sort-cmp technique rather than the skeleton
  comparator). The other three (`both deleted:`, `added by us:`,
  `added by them:`) were recorded as unreachable because "an ordinary merge
  cannot produce those stage combinations" -- true of a CONTENT merge, and
  **false in general: a rename/rename produces all three at once**. Base has
  `f.txt`, ours renames it to `a.txt`, theirs to `b.txt`, leaving `f.txt` at
  stage 1 only, `a.txt` at stage 2 only, `b.txt` at stage 3 only (interop's
  `phase45` group, which also pins the three porcelain codes).
  WARNING: **that fixture must be built by GIT, not sg** -- `sg merge` has no
  rename detection and resolves the same history CLEANLY, keeping both
  renamed files. The divergence is pinned in the same group, so teaching sg
  rename-aware merging turns a check red and says so rather than silently
  changing how the fixture builds.
  The closing summary line is deliberately **not** in the dropped set --
  both tools hard-code `git add`/`git commit -a` regardless of the invoking
  binary's own name, so it is supposed to be byte-identical, and this is
  exactly where Phase 38 round 1 found five real bugs (a missing trailing
  blank line after the merge block, another after unborn HEAD's
  `No commits yet`, an unborn-HEAD row misplaced in the five-way closing-line
  priority order, an extra blank line after `-uno`'s
  `Untracked files not listed (...)`, and a resolved in-progress merge that
  must suppress the closing line entirely) and round 2 (a cold read) found a
  sixth: the merge banner used to call `sg_index_has_unmerged` directly,
  unfiltered by pathspec, while the closing-line suppression already used
  the filtered count -- **both must go through the same `count_unmerged`
  call, computed once**, or a pathspec that does not match the conflicted
  path makes the banner and the closing line disagree about whether the
  merge is still unresolved. See Phase 38 of `docs/DESIGN.md` for the full
  measured tables; do not re-derive them from memory.
- **There is exactly one lookup table for the seven unmerged stage
  combinations** (`unmerged_label` in `cmd_status.c`), shared by the long
  format and porcelain. The long format's label column width is **17**, the
  staged/unstaged section is **12**, they differ, do not conflate them.
- **Phase 81a: `SG_STATUS_TYPECHANGE` is `T` in porcelain/short and
  `typechange: ` in the long format** (11 bytes + one space, the same
  12-column width as `modified:   `). Both producers in `workdir/status.c`
  decide it through the one shared predicate `sg_diff_entry_is_typechange`
  (`diff.h`); see `docs/RULES-diff.md`'s Phase 81a entry for its contract.
  WARNING: **`p38_skel` (interop) drops every TAB-indented line, and every
  long-format ENTRY line is TAB-indented** -- so a `p38_cmp`/`p38_cmp_named`
  check compares section headers and closing lines only, never an entry's
  label, padding or path. Measured: removing the trailing space from
  `"typechange: "` stayed green against all three Phase 81a long checks.
  Phase 81a added `p81a_entries`, a byte-for-byte compare of the
  TAB-indented lines, for its own three fixtures only. **This blindness is
  by design, not an accident**: the comment above `p38_skel` delegates path
  lines to separate groups' own strip-tab compares (it names Phase 23,
  Phase 25 and Phase 32), so an entry line is byte-checked only where some
  group built a fixture containing that entry and wrote its own compare --
  the 34 `p38_cmp` cases check none of their own entry lines (recorded, not
  changed). A new long-status check that needs the entry text must add its
  own entry-line compare -- the existing `sed -n 's/^\t//p'` compares in
  those groups and `p81a_entries` are the templates -- and must not rely on
  `p38_cmp_named`.
