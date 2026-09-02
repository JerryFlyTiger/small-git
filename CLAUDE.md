# Small_Git

A simplified git implemented in C11, executable is `sg`. The goal is
**bit-for-bit disk-format compatibility with real git** -- objects, index v2,
packfile, and the pkt-line protocol all have to be directly readable by real
git; this is guarded by `tests/interop.sh` (2185 checks, using real `git` as
the oracle).

On top of that there are two things real git does not have: `src/safety/`
(automatic snapshots before destructive operations) and `src/storage/chunk.c`
(content-defined chunking for large files).

Design decisions are recorded in `docs/DESIGN.md` (**look up a specific
section when needed, do not read the whole thing**).

## Build and verification

```bash
make                              # build/sg, with -g
make test                         # 64 unit test binaries, any failure fails the whole thing
bash tests/interop.sh             # interop test against real git (needs a prior make)
make sanitize                     # clean + rebuild with ASan/UBSan + run unit tests
python3 tests/fuzz_ignore.py      # .gitignore consistency fuzzer (200 rounds by default)
python3 tests/fuzz_diff.py        # patch output consistency fuzzer (200 rounds by default)
python3 tests/fuzz_merge.py       # three-way merge vs real git (200 rounds by default)
python3 tests/fuzz_diff.py --histogram   # same, but both sides use --histogram
```

**After editing `tests/interop.sh`, run `bash -n tests/interop.sh` before
anything else.** A quoting mistake aborts the script at that line, so **every
check below it silently does not run** -- no FAIL, no error in the summary,
only a smaller `M` in the `interop: N/M` line. Measured in Phase 48: a nested
`sh -c "test \"$(...)\" = ..."` killed everything from line 12538 on, and two
mutation rounds were then read as "caught" on the strength of red lines that
all came from checks *above* the break, while the new checks had never
executed. This is the same failure direction as the `M`-shrinking warning
below, just with a different cause.

**Run the first four gates in one shot: `bash tests/gates.sh`** (`--sanitize`
also runs the fourth gate, `--rebuild` cleans first). It prints a summary
table, and every line links to the path of the raw log -- a summary you can't
trace back to raw output is just a new place for lies to live. The point is
not fewer keystrokes, it's **reading the result with the same extraction rule
every time**: ad-hoc grepping for the wrong number is misreading the gate, and
that is this project's worst failure mode. When reading the summary, know
these four things (the script's comments have the full WHY):

- The line that prints "0 TUs recompiled" **does not give you a warning
  count**: make compiled nothing this run, so the 0 means "not measured", not
  "measured as zero". To actually measure, use `--rebuild`.
- In the `make test` line's "N/64 ran", if N is less than 64 it means
  **aborted partway through** (the Makefile stops at the first failing
  binary), not "the rest passed".
- A non-zero exit code with zero FAIL lines still counts as FAIL: crashes,
  timeouts, and ASan aborts all look like this.
- If interop can't find the `interop: N/M passed` line it is judged FAIL
  outright, it will not silently skip; and within that line, any `K skipped`
  greater than 0 is flagged `warn` -- interop.sh has 63 `skip()` calls, and
  missing just one `python3` or `git` skips the entire smart-HTTP interop
  group while the script itself still exits 0.
  **`M` itself needs watching too**: measured (back in Phase 17, when the
  total was still 998) that turning off `HTTP_AVAILABLE` made `K` only 32,
  yet `M` dropped from 998 to 886 -- `skip()` only increments `SKIP`, not
  `TOTAL`, so the 80 items not explicitly named by `skip()` leave no trace of
  their count at all. Looking only at `N == M` reads "over a hundred items
  did not run" as a perfect score.

`warn` never affects the exit code (there is no `-Werror`, and treating a
warning as a hard failure would be stricter than the completion criteria
below), but the warnings from all four gates are counted into the summary --
including `tests/*.c`'s own warnings, since those are a single
compile-and-link step and never show up in the `make` gate's log. The
proviso from the first bullet ("not measured != measured as zero") applies to
`make test` too: when it recompiles no test binaries, it says so explicitly
rather than handing you an empty 0.

There is a fifth, opt-in gate that is **not** part of the completion
criteria: `bash tests/gates.sh --leaks` re-runs every unit test binary under
macOS's `/usr/bin/leaks`. It exists because this machine cannot run the real
thing -- Apple's ASan does not implement LeakSanitizer and aborts outright on
`detect_leaks=1` (measured), so a green `make sanitize` here is zero evidence
about leaks; the only precise leak detection the project has is CI's ubuntu
ASan job. Two things about reading its row, both measured:

- **`leaks` is a conservative scanner, so `0 leaks` means "nothing it could
  prove leaked"**, not "no leaks". A single 4 KB `malloc` made in a helper
  that returns goes unreported, because the dead frame still holds the
  pointer and the stack is scanned as a root; 200 x 100 KB with the stack
  scrubbed afterwards is caught. It is a net for accumulating leaks, never a
  substitute for CI.
- **A crashed binary yields exit code 0 and no summary line at all**, so
  reading the exit code alone would score a segfault as green. The gate
  therefore demands the `N leaks for M total leaked bytes` line from every
  binary and reports `analyzed N/64`, the same "not measured != measured as
  zero" shape as the `make` and `make test` rows. Non-macOS is a `skip` row,
  never a silent pass.

`SG_LEAKS_TIMEOUT` (default 120s) bounds each binary, turning a hang into a
named failure. All three of the gate's FAIL branches were proven by planting
a leak, an unanalyzable binary, and a crashing one -- the leak case is the
one that matters, because `make test` and interop both stayed green for it.

`tests/test_fuzz_pack.c` and `tests/test_fuzz_index.c` are fuzzers for the
binary parsers, already included in `make test` (default round count only
takes a few seconds). `SG_FUZZ_ITERS` adjusts the round count,
`SG_FUZZ_SEED_BASE` shifts the seed (round i uses seed base+i, the failure
message prints it so you can reproduce exactly), `SG_FUZZ_TIMEOUT` adjusts the
watchdog (default 600 seconds, turns a hang into a clean failure).
`SG_FUZZ_BIG=1` additionally runs a truncation regression case that allocates
about 4 GB, off by default.

**These two fuzzers derive most of their discriminating power from the
sanitizer, not from assertions** -- for hardening like "reject absurd sizes",
the return value is identical before and after the hardening (both are -1,
only the mechanism changes from an explicit rejection to a malloc failure), so
the difference only shows up under ASan. So when touching the parsing paths of
`src/storage/pack.c` or `src/index/index.c`, a green `make test` **does not
count**, run `make sanitize`. Details and mutation-testing measurements are in
the Phase 10 section of `docs/DESIGN.md`.

**Completion criteria**: `make` + `make test` + `bash tests/interop.sh` all
green. When touching `src/workdir/ignore.c` or any directory-traversal logic,
also run `python3 tests/fuzz_ignore.py` manually (it has also been wired into
the CI `fuzz-ignore` job since 2026-08-07, but running it locally first finds
problems faster). When touching memory management or pack/chunk, additionally
run `make sanitize`.

**"Adding a field to a shared struct" also counts as touching memory
management**, and needs `make sanitize`. Measured 2026-08-23 (Phase 29): after
`sg_diff_entry` gained two extra fields, `tests/test_diff_out.c` had two spots
that assign fields one at a time after `malloc` (no memset first), so the new
fields were malloc garbage, and `print_patch` dereferences them. **Both `make
test` and interop were fully green**, only ASan was red (`SEGV ... in
sg_quote_path_prefixed`, address `0xbebebebe`). When adding a field, also
search for every instance that is **not** built through a construction
function.

**No formatter and no linter** -- no `.clang-format`, no `clang-tidy`, and the
Makefile has no `fmt`/`lint` target. The global rule's `cargo fmt`/`clippy`
have no equivalent here, do not go looking for one. Also, `CFLAGS` only has
`-Wall -Wextra -Wpedantic`, **there is no `-Werror`**, so a green light does
not mean zero warnings -- warnings in the compile output need to be checked
manually (`Makefile:2`).

Always run `make clean` between build modes: object files do not record which
set of flags they were compiled with (`Makefile:76-80` has the full
explanation). `release`/`sanitize` clean automatically; going back to plain
`make` requires a manual clean.

**Also always run `make clean` after changing any `include/sg/*.h`. The
Makefile has no header dependency tracking** -- no `-MMD`, no `.d` files, no
`-include`, so `make` only recompiles the `.c` files you touched, other TUs
keep using their old `.o`. If what changed is a struct definition (e.g. adding
a field to `sg_diff_entry`), different `.o` files end up with different
layouts for the same struct, and the symptom is **segfaults at random
locations** that look exactly like a bug in the new logic you just wrote.
Measured 2026-08-23 (Phase 29): after adding one field, `make test` crashed at
a `strcmp` in `cmd_stash.c`; after `make clean && make`, 49/49 passed, with
not a single line of code changed.

Depends on zlib / openssl / libcurl, all detected via pkg-config
(`Makefile:31-40`). Only macOS and Linux are supported (POSIX APIs used
directly). On macOS, brew's openssl@3 is not on the default path, so
`PKG_CONFIG_PATH` needs to be set.

CI (`.github/workflows/ci.yml`) runs a three-cell matrix of
ubuntu x {gcc,clang} + macos x clang, plus an ASan/UBSan job and a
`fuzz-ignore` job, on every push to every branch.
**What cannot be tested locally (macOS): gcc, interop.sh under ASan/UBSan, and
install/uninstall verification into a staging dir. A local green light is not
sufficient evidence.**

## Module layout

Dependencies flow bottom-up. `src/<mod>/*.c` corresponds to `include/sg/*.h`.

| Directory | Responsibility | Depends on |
|---|---|---|
| `object/` | Object serialization/parsing, pure in-memory, no fs access | hash |
| `index/` | index v2 binary read/write and ordered entry operations, does not read objects | hash |
| `util/` | zlib, SHA-1, levenshtein, LCS table, wildmatch, rename similarity | -- |
| `storage/` | Objects and refs on disk: loose, pack, chunk, refs, reflog, repo, revparse | object, workdir |
| `net/` | smart-HTTP + SSH: libcurl wrapper, ssh subprocess, pkt-line, transport | -- |
| `workdir/` | Working directory: path/file I/O, ignore, status, diff (change list), apply, merge, tree_build | almost everything |
| `safety/` | snapshot (recoverable backup refs), rebase sequencer state, stash | storage, workdir |
| `cli/` | 24 `sg_cmd_*` + dispatcher + six diff output formats (`diff_out.c`), the only assembly point | everything |

- **Reading an object always goes through `sg_object_read`**
  (`include/sg/objstore.h:16`): loose first, then pack. Do not call the
  underlying layer directly except from `loose.c`/`pack.c` themselves.
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
- **`util/` has no path *resolution*** (that lives in `workdir.h`), but since
  Phase 23 there is a pure byte-conversion function `util/quote.c`, and since
  Phase 28 another one, `util/wildmatch.c`. The latter is git's wildmatch
  implemented in "`/` is not special" mode, **gitignore and pathspec share the
  same one**: `src/workdir/ignore.c` layers a segment layer on top of it so
  `*` stops at `/` and `**` crosses directories -- that layer is exactly what
  gitignore has beyond pathspec (in real git too it is only a difference of
  one WM_PATHNAME flag). **Do not write a second glob matcher for pathspec.**
  Path resolution, `mkdir -p`, and file read/write live in
  `include/sg/workdir.h` (`sg_resolve_repo_path`, `sg_mkdir_parents`,
  `sg_read_file`, `sg_write_file_mkdirs`, `sg_hash_file_blob`). Go look for
  path utilities in `workdir.h`, not in `util/`, and do not write another
  copy.
- **Rendering a commit timestamp for a human always goes through
  `sg_date_format_normal`** (`include/sg/date.h`, Phase 54) -- git's
  DATE_NORMAL, the `Date:` line of `git log` and `%ad`. It returns the whole
  field including the offset, so no caller assembles half of it.
  WARNING: **the clock shown is the epoch SHIFTED INTO the stored offset**,
  not UTC and not the machine's local time. `sg log` used to render UTC while
  printing the stored `+0800` beside it, so every date it ever showed was
  wrong by the offset -- eight hours here -- and contradicted itself in its
  own output. Nothing caught it because nothing had ever compared `sg log`
  to `git log`; the pre-existing checks assert exit codes or scrape
  `head -1` for the sha.
  WARNING: **the day of month is NOT zero-padded** (`Jan 1`, not `Jan 01`),
  and the weekday/month names come from git's own hard-coded English tables,
  **not `strftime`'s `%a`/`%b`** -- those follow the locale, and one
  `setlocale` call anywhere in the process would silently translate a format
  whose entire job is to match git byte for byte.
  WARNING: **`cmd_undo.c` has its own date formatter and must not be
  converged onto this one.** `sg undo` has no real-git counterpart, so it has
  no oracle; it deliberately prints local time in ISO form.
  WARNING: **`sg log`'s oracle is `git log --first-parent`**, because sg's
  walk is first-parent-only by Phase 2 scope -- against a full `git log` the
  two legitimately visit different commit SETS. Interop pins the rendering
  against `--first-parent` AND pins the scope boundary separately (sg's
  output must NOT equal a full walk), so teaching sg to walk every parent
  turns a check red by name instead of quietly changing what the rendering
  checks compare. Four git config knobs were measured to move that oracle
  (`log.decorate`, `core.abbrev`, `log.date`, `format.pretty` /
  `log.abbrevCommit`) and are pinned on the command line, with a
  precondition check proving the pins beat a hostile config.
  WARNING: **an empty commit message prints no message block at all**, not
  even the leading blank line, and entries are separated by one blank line
  with **none after the last** -- so the separator goes BEFORE every entry
  but the first. An empty-message entry in the middle is what distinguishes
  the two models. Abbreviations are hard-coded to 7 (the `Merge:` line and
  `--oneline`) while git's default `core.abbrev=auto` scales with object
  count; interop declares `core.abbrev=7` on git's side rather than
  pretending the two policies agree.
- **`sg show` exists as of Phase 55** and shares `sg log`'s entry renderer:
  `git show <non-merge-commit>` is byte-identical to `git log -1 -p` of it in
  every flag combination (measured), so the renderer lives in
  `src/cli/commit_out.c` (`sg_commit_out_entry`) and **neither command owns a
  copy**. Phase 54's `phase54` interop group is what proves a change to it
  did not move `sg log`.
  WARNING: **the separator between several objects is STATEFUL, and a blob
  is the exception**: everything else takes a leading blank line when
  something was already shown or when it is a tag's target, while a blob
  neither takes one nor arms the flag for the next object. Measured over
  every ordered pair. A rule of "a tree never separates" holds only when the
  tree comes first -- that was my spec's error, caught by the implementer.
  WARNING: **only COMMITS are deduplicated across the argument list.**
  `show <c> <c>` prints once, and so does an annotated plus a lightweight tag
  of the same commit; but `show <tree> <tree>` prints twice, and
  `show <tag> <tag>` prints the tag header twice with one commit under it.
  WARNING: **`-s`/`-p`/`--stat` are LAST ONE WINS**: `-s` clears what came
  before, `-p` and `--stat` set their own bits and accumulate, default is a
  patch. `-s -p` prints a patch, `-p -s` prints nothing, `--stat -p` prints
  BOTH (all measured). Giving `-s` a fixed priority passes every single-flag
  case and gets `-s -p` backwards -- same shape as `-M`/`-C` and `-c`/`--cc`.
  WARNING: **a merge is TWO row sets in one command** (Phase 55b). The dense
  patch includes a path iff the result differs from **every** parent, with
  the **MODE** compared as well as the id -- a file whose blob id equals
  theirs' exactly is still included when only its mode differs, and renders
  as a `mode a,b..c` line with no hunks. `--stat` is a completely different
  set: measured byte-identical to `git diff --stat <parent1> <merge>`, so it
  includes paths the dense rule excludes, and it stays the first-parent diff
  at ANY parent count. Computing one set and rendering it both ways is wrong
  for whichever it was not written for; interop names both halves.
  WARNING: **a merge opens its diff section with a blank line even when the
  dense set is EMPTY** (measured on a clean merge and a clean octopus: header,
  blank, nothing else), where an ordinary commit with an empty diff prints no
  blank at all. Returning early on "no rows" gets that wrong AND skips
  `--stat`. The stat->patch blank is printed only when the patch itself will
  produce output, and a merge prints **no `---` line** with `-p --stat` where
  an ordinary commit does.
  WARNING: **`sg_diff_entry.combined_row` exists because the ours/theirs/
  result predicate cannot express these rows** -- a path added by the merge
  has both parents ABSENT, one deleted by it has result ABSENT with nothing
  unmerged. **Do not loosen `sg_diff_entry_is_combined` instead**; its
  asymmetry is deliberate, measured and pinned (see the combined-diff notes
  above). Adding that field required auditing every manual construction site
  per the shared-struct rule.
  WARNING: **an octopus is refused only when it actually needs rendering** --
  `> 2` parents AND a non-empty dense set. A clean octopus prints header plus
  the opening blank line, byte-identical to git, and its `--stat` works like
  any other. **A fixture for the dense path must use a merge that CONFLICTED
  and was resolved**: a clean merge's dense set is empty, and this group's
  precondition caught exactly that mistake.
- **git EXPANDS TABS in a commit message body, and sg must too** (Phase 55b,
  `print_message_line` in `src/cli/commit_out.c`). `--expand-tabs=8` is the
  medium format's default and **only** the medium format's: `--oneline` and
  `%s` leave the tab alone (measured). The column is counted from the start
  of the **message line**, NOT from the indented output column -- a line of
  two tabs lands at output column 20, i.e. 16 expanded columns plus the
  four-space indent.
  WARNING: **this was a pre-existing `sg log` bug that Phase 54 missed while
  pinning `sg log` byte-for-byte**, because no fixture had ever put a tab in
  a commit message. It surfaced only because `git merge`'s own auto-generated
  conflict message contains `#\tboth.txt`. A shape the fixtures cannot
  produce is untested however many checks run over them.
  WARNING: **tree entry names are printed RAW here, and that matches git.**
  Measured on a crafted entry name containing an ESC byte: `git show <tree>`
  prints it raw with `core.quotepath` at its default, while
  `git cat-file -p <tree>` on the same tree C-quotes it. git is inconsistent
  between its own two commands and sg matches git in both -- do not "fix"
  `cmd_show.c` to quote by analogy with `cmd_cat_file.c`.
- **`sg log` takes `-n <count>` / `-<count>` / `--max-count=`, `--oneline`,
  `-p`/`--patch`, `--stat` and a single `<rev>`** (Phase 54). `-p`/`--stat`
  are reuse -- the commit's first-parent tree against its own, through
  `sg_diff_trees` + `sg_diff_print` -- so what needed measuring was where the
  diff sits inside the entry, not the diff.
  WARNING: **the `---` line appears ONLY when `-p` and `--stat` are both
  on.** With `-p` alone git introduces the diff with a blank line, so a rule
  that always printed `---` passes the combined check and fails the plain
  one; interop pins the negative separately. `--oneline` introduces its diff
  with **nothing**, and never prints `---`.
  WARNING: **an EMPTY diff prints no separator at all** -- no blank line, no
  `---`. The separators belong to the diff, not to the entry, and an empty
  commit in a fixture is what makes the difference observable.
  WARNING: **a merge DOES get a diff, against parent 1**, because sg's walk
  is first-parent-only and that is what `git log --first-parent -p` does
  (measured; plain `git log -p` prints nothing for a merge). This is not an
  independent choice, it falls out of the scope boundary above.
  WARNING: **rename detection is ON** at `SG_SIMILARITY_DEFAULT`, because
  git's `diff.renames` has defaulted to true since 2.9 and `git log -p`
  prints `rename from`/`rename to`. The interop fixture carries a
  rename-with-an-edit plus a precondition asserting git itself calls it a
  rename.
  WARNING: **`-n 0` is a legal request for nothing**, not an error and not
  "unlimited": git prints nothing and exits 0.
  **Deliberately not implemented: `-- <pathspec>`** (path-limited history is
  git's history SIMPLIFICATION, not a filter over the same walk -- the same
  reason `--patience` is rejected rather than approximated), `--graph`,
  `--format`/`--pretty`, `--date=`, `--author=`/`--grep=`, `--reverse`,
  `--all`, and `-c`/`--cc`. All are rejected with the usage line and exit 1,
  never silently ignored.
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
  (`* Unmerged path` unquoted -- see the diff module notes below, pinned on
  both sides by interop; the rev-argument rejection that used to sit
  beside it is gone, implemented in Phase 40): this one has
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
- **pathspec always goes through `sg_pathspec_*`** (`include/sg/pathspec.h`,
  Phase 28); the matching rule is **three ordered clauses**: exact literal
  match, literal directory prefix, only fall through to `sg_wildmatch` if the
  spec contains a wildcard.
  WARNING: **the first two clauses and the third do not add together**: a
  spec containing a wildcard has **no** directory-prefix rule. Measured
  against real git 2.55.0: `o[tx]her` against `other/d.c` prints nothing, and
  `su?` and `s*b` against something under `sub/` are also empty; `sub*`
  matches only because **`*` crosses `/`** (pathspec uses wildmatch with
  WM_PATHNAME turned off), not because it recurses into the directory.
  Unifying the two by intuition would make `sg diff` silently print extra or
  missing files.
  WARNING: **a trailing `/` is meaningful, not noise**: `sub/` lists sub's
  contents, `a.txt/` **matches nothing** (it is asking "what's under this
  name"). `sg_resolve_repo_path_allow_root` normalizes it away, so
  `sg_pathspec_add` needs to remember to reattach it -- this pair is the only
  test that can tell "was it reattached or not".
  WARNING: **magic (`:(icase)`, `:!`, `:/`) must always be rejected, never
  treated as a literal path**: silently matching nothing, or matching a file
  actually named `:!sub`, are both answering a question the user never asked.
  Filtering happens **after the list is already built**
  (`sg_diff_list_filter`), not inside the four builders -- each of the four
  having its own pathspec logic was exactly the shape Phase 27 spent a whole
  milestone eliminating. The cost is that filtered-out files still get hashed
  once, which is a speed bill, not a wrong answer.
- **The disambiguation rule for bare arguments (without `--`) came from
  measurement, do not simplify it** (Phase 28): being both a revision and an
  existing file -> reject outright; **every argument after the first path
  must exist** (`sg diff a.txt HEAD` fails naming HEAD, even though it is a
  perfectly valid revision); neither -> "ambiguous argument". WARNING:
  **arguments containing a wildcard skip the existence check** -- `git diff
  '*.zzz'` still exits 0 even though it matches nothing, while `git diff
  nosuch` is a hard error. Use `sg_pathspec_looks_like_spec` to decide "does
  this look like a pathspec", the character set lives in that one place, next
  to the matcher.
- **Rename detection always goes through `sg_diff_detect_renames`**
  (`include/sg/diff.h`, Phase 29); it is a **pass that runs after the list is
  already built**, not inside the four builders (same reason as
  `sg_diff_list_filter`).
  WARNING: **it must run after `sg_diff_list_filter`**. Measured against real
  git: `git diff --cached --name-status -- b1.txt` (naming only the new half
  of a rename) prints `A`, not `R100` -- git filters by pathspec first and
  detects second, so only half the pair remains and no match can form.
  Reversing the order has no visible symptom, it just gives a wrong answer in
  this exact scenario.
  **Since Phase 30 both exact and inexact detection are implemented**, and
  the implementation lives in `src/workdir/rename.c` (**not** `workdir/diff.c`
  -- the module table above will send you to the wrong file). The scoring
  itself is `src/util/similarity.c` + `include/sg/similarity.h`, a deliberate
  port of git's `diffcore-delta.c`.
  WARNING: **the score is a machine-readable field**, printed as `R093` and
  `similarity index 93%`, so being one point off is a wrong answer, not a
  near miss -- `similarity.c` may be reproduced but never "improved". Two
  independent ports agreeing is the only evidence that counts here: the
  algorithm was first re-derived in Python and checked against real git on
  750 random file pairs, and the C was then cross-checked against that.
  When touching `similarity.c` or `rename.c`, a green `make test` **does not
  count**: run `python3 tests/fuzz_rename.py 120 --seed <unused> --max-failures 0`
  and report the actual mismatch count.
  WARNING: **there are THREE passes and their ORDER is observable**, so they
  cannot be collapsed into "score every pair and keep the best": exact (by
  id), then same-basename pairs at a **raised** threshold
  (`min + 0.5 * (MAX - min)`, i.e. 75% by default), then the full matrix.
  Measured against git 2.55.0 with two fixtures that disagree about which
  side wins, so no single wrong threshold satisfies both: a name match at 79%
  beats an unrelated 98% match, while a name match at 60% loses to it.
  WARNING: **`-M<n>`'s grammar is a fraction, not a percentage** (all
  measured): `-M5` is 50%, `-M05` is 5%, and **`-M100` is TEN percent** --
  only `-M100%` limits detection to exact renames. The grammar lives in
  `sg_similarity_parse_score`; do not reimplement it at a call site.
  WARNING: **the score is kept on git's 0..60000 scale, not as a percentage**
  (`SG_SIMILARITY_MAX`), because `-M005` asks for 0.5% and a percentage
  cannot hold that. Only `sg_diff_entry.score` is a percentage, converted
  once at the end by `sg_similarity_percent` (which **truncates**: 59999 is
  99%, not 100%).
  WARNING: **text vs binary changes the score.** The CR of a CRLF pair is
  skipped when hashing text but is still counted in the file's size, so a
  CRLF file scores about 66% against *itself*; one NUL byte makes the same
  bytes binary and the same comparison a perfect match. This is exactly why
  git settles exact renames by id BEFORE scoring anything.
  WARNING: **matching uses `sg_diff_side_effective_id`** (promoted to public
  from `diff_out.c` in Phase 29); it returns -1 to mean "this id was never
  verified" -- **two unverified ids do not count as identical content even if
  they happen to be equal**, that side is never paired. The failure direction
  is "not a rename", never "conjure a rename out of nowhere".
  WARNING: **`sg stash show` detects renames too, and only after `-u`'s
  merge** (Phase 31): the tracked and untracked halves become one list first,
  and detection runs once over the whole thing. Measured against git 2.55.0:
  an untracked file that is byte-identical to a deleted tracked file takes
  the source through the exact pass, demoting the real inexact rename beside
  it to a plain `A`. Detecting per-half, or before the merge, silently gives
  a different answer -- there is no special case for it, only the ordering.
  **`sg diff -C` finds copies since Phase 33**, and a copy is
  `old_path != NULL && is_copy`. The rule for which is which is git's, is one
  line, and looks arbitrary until you know it: a source is paired N times,
  each destination spends one use **in path order**, and a row is a copy while
  uses remain after its own. A source that is not a deletion is charged one
  use up front, so anything copied off an edited file is a copy by
  construction. Consequence, measured: with one source and two destinations
  the FIRST by path is the copy and the second is the rename, **however much
  better the second matched**.
  WARNING: **`-C` changes three things, not one.** A path present on both
  sides becomes eligible as a source (the only way to find a copy from a
  merely edited file); a source may be paired more than once; and **the
  same-file-name shortcut is skipped entirely**. Dropping any one of them
  gives a different answer from git.
  WARNING: **`-C -C` / `--find-copies-harder` is implemented as of Phase 51**
  (previously rejected outright). It offers every *unchanged* path as a copy
  source in addition to plain `-C`'s deleted/edited ones, which needed a
  data-layer change: `sg_diff_list` used to only ever hold paths that
  changed, so the three tree-facing builders (`sg_diff_trees`,
  `sg_diff_tree_index`, `sg_diff_tree_workdir`) gained a mandatory
  `include_unchanged` parameter and `sg_diff_entry` gained an `unchanged`
  field that `sg_diff_detect_renames` strips before returning, on every
  success path. `sg_diff_index_workdir` deliberately did NOT gain the
  parameter -- every path there comes from the index, so there is no
  addition for a copy to land on (measured: `git diff -C -C` with no `--cached`
  and no `<rev>` still prints only `M`, never `C`, for a staged-then-edited
  or an unchanged-plus-modified fixture). The CLI state machine (`detect` in
  {NONE, RENAME, COPY}, a staged `score`, and `harder`, resolved only AFTER
  the argv loop) is a faithful port of git's own; see Phase 51 of
  `docs/DESIGN.md` for the full measured truth table -- three results falsify
  simpler models: `--no-renames` does NOT cancel `--find-copies-harder` and
  does NOT reset the score; a bare `-C` RESETS the score to the default; and
  `-C95 --no-renames -C` finds nothing, because `--no-renames` cleared
  `detect` first so the following `-C` takes the "not yet COPY" branch and
  never sets `harder`.
  WARNING: **a genuinely PRE-EXISTING bug was exposed and fixed during
  Phase 51**: `src/workdir/rename.c`'s `exact_pass` and
  `claim_from_matrix` were reading git's `rename_used` counter (`uses` on
  `rename_cand`) as if it were the plain boolean `used` flag, at two call
  sites. `uses` starts at 0 for a fresh deletion and is PRE-LOADED to 1 for
  a source that exists on both sides (a modification, or -- since Phase 51
  -- an unchanged row) -- **`uses` IS git's `rename_used`, `used` is NOT a
  substitute for it**: real git resolves ordinary renames (deletion
  sources only) in a dedicated FIRST pass, and only THEN considers copies
  (modification/unchanged sources) for whatever destinations are still
  unclaimed, so a modification/unchanged source must never win the
  "real renames only" walk regardless of score. Reading `used` erased that
  distinction, since `used` starts at 0 for every FRESH source no matter
  its kind, letting a higher-scoring modification/unchanged source
  outscore or out-tie-break a genuine deletion for the same destination.
  Reproduced (and pinned) with plain `-C` and a genuine modification
  source -- no `-C -C`, no `unchanged` row needed, i.e. this predates
  Phase 51 entirely. Fixed at three sites, all in `src/workdir/rename.c`:
  `exact_pass`'s skip condition and its tie-break score, and
  `claim_from_matrix`'s rename-only-walk skip -- all three now read
  `uses > 0` instead of `used`. See Phase 51 of `docs/DESIGN.md` for the
  two measured witness shapes (`matrix_pass`, `exact_pass`) and their
  fixtures.
  WARNING: **the `exact_pass` witness has a direction trap, and both
  directions are pinned** (`tests/test_rename.c`,
  `tests/interop.sh`'s `phase51:` group): with the modification's path
  sorting BEFORE the deletion's, the old iteration-order tie-break picked
  the modification and was wrong; with the deletion sorting first, plain
  iteration order already gave the right answer even before the fix, so
  that direction alone is a control, not evidence -- the same
  "fixtures all pointing one way hide the gap" lesson this project has
  hit before. `tests/fuzz_rename.py --copies-harder` (new in Phase 51)
  measured 0.8%-1.7% mismatches per 120-round sample before the fix,
  0/120 across two seed ranges after it.
  WARNING: **`-M` and `-C` write the same mode; the LAST one wins.** Measured:
  `git diff -C -M` finds renames only, `-M -C` finds copies. So every `-M`
  branch in `cmd_diff.c` must clear `detect_copies`, and every `-C` branch
  must set it. sg had `-C` sticky in the first order for exactly as long as
  no test combined the two flags.
  WARNING: **two copy-mode properties are masked by other machinery and need
  deliberately built fixtures** (both cost a mutation round to find): copy
  mode skipping the basename shortcut is invisible with ONE source, because
  reuse makes both routes agree -- it takes two; and the exact pass's reuse is
  invisible unless the matrix is out of the way, i.e. at `-C100%`.
  **`sg status` has a rename row since Phase 32**, and `sg_status_diff_staged`
  is no longer a second implementation of tree<->index -- it is a thin adapter
  over `sg_diff_tree_index`, the way `sg_status_diff_unstaged` has been over
  `sg_diff_index_workdir` since Phase 27. The two walks were proven equivalent
  first (`tests/test_status_staged_parity.c`, 12 named shapes + a fuzzer),
  so the swap changed no answer.
  WARNING: **`sg_status_diff_staged`'s `rename_score` is mandatory and has no
  default**, same idiom as `sg_workdir_missing`. `apply.c`'s two safety gates
  pass **0**: they enumerate the list to tell the user what is uncommitted,
  and a rename row carries TWO paths where that loop prints one, so detection
  there would silently stop naming the old path. `cmd_status.c` passes the
  threshold. Do not give this parameter a default.
  WARNING: **a rename is `old_path != NULL`, not a fourth `sg_status_kind`**
  (same shape as `sg_diff_entry`), so every existing `switch` over kind stays
  exhaustive. A renamed row's kind is `SG_STATUS_MODIFIED`, so a consumer
  that knows nothing about renames still sees "this path changed".
  WARNING: **`git status` and `git diff` disagree about a malformed `-M`**
  (measured): `git status -Mabc` exits 0 and quietly uses the default, while
  `git diff -Mabc` exits 129. sg matches each command separately -- do not
  "unify" them onto the shared parser's reject-leftovers rule.
  WARNING: **the porcelain row sort must stay a TOTAL order**
  (`prow_cmp` breaks ties on append position): a path can carry a staged row
  and an unstaged row, only the staged one holds `old_path`, and `qsort`
  leaves equal elements in an unspecified order. Merging x/y across a group is
  order-independent, so this did not matter before renames; it does now.
- **There are two display formats for renames, do not mix them up** (Phase
  29): `--name-status` prints **two separate fields** (`R100\told\tnew`,
  score zero-padded to three digits); `--stat`/`--numstat` print a **single
  compressed pairing column** (`a/{b => z}/c.txt`). Both the prefix and the
  suffix of the compression are computed at `/` boundaries, and **the suffix
  must scan all the way through and update at every `/`** (take the longest),
  stopping at the first `/` would print `{h/i => h2/i}/j.txt`.
  WARNING: **a path that requires quoting turns off compression entirely**
  (measured), because quoting the bracket form would produce a quote mark in
  the middle of the path.
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
  `sg_status_diff_staged`'s own entries above). Reason: how deep a
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
  (`include/sg/workdir.h`, Phase 21). There are **three** call sites: right
  after a successful `remove()` in `workdir/apply.c` and `workdir/merge.c`,
  and (Phase 37) `safety/stash.c`'s `restore_matched_paths`, which does the
  same "delete a matched, target-absent path" step for `sg stash push`'s
  partial-pathspec restore -- the same reasoning applies there as at the
  other two, it is just a third call site rather than a reason to route
  through `sg_apply_tree_to_workdir` (which this codebase deliberately does
  not give a pathspec parameter, see Phase 37 in `docs/DESIGN.md`).
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
- **`sg clone`/`fetch`/`push` speak SSH as well as smart HTTP since Phase
  47** (`src/net/ssh.c`, `include/sg/ssh.h`): `ssh://[user@]host[:port]/path`
  and the scp-like `[user@]host:path`. **pkt-line framing, want/have
  negotiation, sideband demux and the push report are transport-independent
  and reused byte for byte** -- only three things differ, and each is a trap:
  WARNING: **the `# service=...` packet and the flush after it are
  smart-HTTP's envelope, not the protocol's.** Over ssh the service IS the
  remote command, so the advertisement starts at the first ref;
  `parse_ref_advertisement_for_service` takes `expect_service_line` for
  exactly this, and requiring it over ssh rejects every valid advertisement
  as malformed.
  WARNING: **the two URL forms disagree about the leading slash** (measured
  with a logging stand-in for ssh): `ssh://host/srv/x` asks for `/srv/x`, the
  scp-like `host:srv/x` asks for `srv/x`, and `ssh://host/~alice/x` DROPS the
  slash so the far side's shell expands the home directory. Also **`host:22`
  is a PATH named 22** -- the scp-like syntax has no port.
  WARNING: **each `sg_transport_*` call opens its OWN connection** and
  `sg_ssh_request` therefore reads and discards an advertisement it has
  already seen. Real git holds one connection across both phases; matching
  that means threading a connection object through three commands, which is
  the trade Phase 40's write-up explains. Do not "fix" the discard without
  changing that shape first.
  WARNING: **this is the only subprocess in `src/`.** The poll loop
  (write and read at once), the ignored SIGPIPE, the half-close after the
  request, the flush that ends an advertisement-only connection, and the
  `waitpid` on every path each prevent a specific hang or silent kill --
  see `src/net/ssh.c`'s own comments before simplifying any of them.
  WARNING: **a new URL form can break code no one edited.** Phase 47's worst
  bug was in `derive_target_dir` (`cmd_clone.c`), untouched by the phase: it
  scans back to the last `/`, which every earlier URL form had, so
  `sg clone git@host:myproject.git` created a directory named
  `git@host:myproject`. Every scp-like fixture had passed an explicit
  destination directory -- the convenient thing to write, and the one that
  skips the guesser entirely.
  WARNING: **`sg_url_redact` treats a schemeless string as scp-like** since
  Phase 47, and uses the SAME "colon before any slash" rule the transport
  routes on -- without it a local path like `a/b@c:d` gets rewritten to
  `***@c:d`. It used to return such a string unchanged, which would print the
  user name out of `git@host:path`. An `@` AFTER the colon is path, not
  userinfo.
  WARNING: **a LOCAL `make sanitize` does not cover the ssh spawn path.**
  That gate builds the unit tests with ASan and runs those, and the only ssh
  code a unit test reaches is URL parsing; the fork, poll loop and pipe
  handling live behind `interop.sh`, which locally runs against the ordinary
  build. **CI is different and does cover them**: its ASan job runs
  `interop.sh` under ASan+UBSan with `detect_leaks=1`
  (`.github/workflows/ci.yml`), so the ssh group is sanitized there. Locally,
  when touching `src/net/ssh.c`, drive a clone/push/fetch over the shim by
  hand against a `make sanitize` build -- recipe in Phase 47 of
  `docs/DESIGN.md`. Done once for Phase 47: clean on clone, push, fetch and
  the failing-path case.
  Not read: `core.sshCommand`. `GIT_SSH_COMMAND` (word-split, git-compatible)
  then `GIT_SSH` then `ssh`.
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
  WARNING: **one measured case is deliberately NOT reproduced** -- with the
  current branch's reflog file deleted by hand, real git lets `@{0}` fall
  back to the branch tip while still rejecting `<branch>@{0}`; sg rejects
  both, rather than inventing an asymmetry between its own two spellings.
  It is not on the deliberate-divergence list (reaching it takes deleting a
  log file by hand); see Phase 48 of `docs/DESIGN.md`.
  **Abbreviated sha is not supported** (deliberately). Do not hand-roll a
  "branch name or 40-hex" fragment again. To list/delete refs under any
  prefix use `sg_ref_list_under`/`sg_ref_delete_under` (`prefix` must end
  with `/`).
  **Exception: `sg push`'s explicit-dst refspec `<src>` (Phase 39,
  `src/cli/cmd_push.c`'s `resolve_refspec_src`) must NOT use this
  function** -- `sg_rev_parse_commit` peels annotated tags by definition,
  but measured against real git: `v2:refs/tags/v2copy` leaves the remote's
  `refs/tags/v2copy` as a **tag** object, not the commit it points at.
  `resolve_refspec_src` tries an exact, unpeeled ref lookup first
  (`refs/tags/<src>`/`refs/heads/<src>`/an already-`refs/`-qualified
  `<src>`) and only falls back to `sg_rev_parse_commit` when none of those
  literal lookups match.
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
  (see the WARNING under the ref-writing bullet above). The write is
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
- **`sg push` takes wildcard and push-matching refspecs since Phase 46**,
  the two forms Phase 39 named and deliberately refused. They need their
  input from OPPOSITE sides of the network round trip, and that is the whole
  design: a **wildcard's source set is LOCAL** (measured -- a pattern
  CREATES remote branches that did not exist there, so it is not an
  intersection with the advertisement and there is no prune semantics), so
  it expands BEFORE connecting and Phase 39's whole-batch-abort rule still
  holds; **`:` means "every local branch that already exists on the remote"**,
  so it can only expand AFTER the advertisement and is appended to
  `candidates` there, leaving the candidate->entry loop's rules untouched.
  WARNING: **an expanded wildcard dst is used VERBATIM, with no dwim
  completion** -- the opposite of an explicit dst. Routing it through
  `complete_dst` is the natural-looking choice and is wrong twice over: it
  would turn `refs/heads/*:x*` into pushes to `refs/heads/x<name>` (git sends
  the uncompleted name and lets the remote refuse it), and
  `complete_dst`'s "dst matches more than one" rule would reject every valid
  wildcard, since a wildcard is ambiguous by definition.
  WARNING: **exactly one `*` per side, on BOTH sides** -- a star on one side
  only, two on a side, and a wildcard DELETION are each `fatal: invalid
  refspec` (measured). The deletion case is the one that matters:
  approximating it would delete every matching remote ref.
  WARNING: **the star may sit anywhere and it CROSSES `/`** (measured: a
  pattern rooted at `refs/` matched `refs/remotes/origin/topic/sub`), a
  no-colon wildcard mirrors itself, and a pattern matching nothing is exit 0,
  not an error. An annotated tag matched by a pattern reaches the remote
  unpeeled, the same trap `resolve_refspec_src` documents.
  WARNING: **a wildcard must skip `SG_CHUNK_KEEPALIVE_REF`** -- that ref is
  owned by the chunks-propagation block, which computes its own old/new pair
  on every push. A pattern wide enough to match it queues a SECOND update
  for the same ref, and the remote refuses the whole atomic transaction
  (`multiple updates for ref 'refs/sg/chunks' not allowed`), so **nothing
  lands, not even the branch the user meant**. Narrow fixtures rooted at
  `refs/heads/` cannot reach this; the pin needs a repo whose remote has no
  keepalive ref yet.
  WARNING: **`sg_push_refspec` is defined in `cmd_push.c` AND duplicated in
  `tests/test_refspec.c`** (deliberate convention -- no public header for a
  test-only export). Adding a field to one and not the other is a
  stack-buffer-overflow that `make test` does NOT catch, only ASan does
  (measured in Phase 46: the library's `memset` wrote 40 bytes into the
  test's 32-byte object while `make test` reported only ordinary assertion
  failures).
- **`sg push` gained refspec support (`[+]<src>[:<dst>]`) and
  `--delete <name>...` in Phase 39** (`src/cli/cmd_push.c`, no header
  changes -- see the Phase 39 section of `docs/DESIGN.md` for why). Five
  things are especially easy to get backwards:
  1. **Split on the LAST `:`, not the first** (`strrchr`). Measured:
     `a:b:c:d` reports src `a:b:c`, dst `d`.
  2. **An annotated tag given as `<src>` is never peeled** -- `<src>`
     resolution must not be `sg_rev_parse_commit` (see that function's own
     entry above for why, and the exact fallback order).
  3. **"`<src>` matches nothing" and "non-fast-forward" are two different
     failure classes, do not conflate them.** A src that resolves to
     nothing aborts the WHOLE push before any network round trip -- not one
     ref lands, not even a connection attempt (measured: `git push origin
     topic:newbr2 nosuch:x` leaves `newbr2` uncreated). A non-fast-forward
     rejection, by contrast, is a PER-REF failure discovered only after the
     advertisement arrives -- a fast-forwardable ref in the SAME invocation
     still lands (measured: `topic -> newbr` succeeds alongside a refused
     `master -> fromhead`). Before Phase 39 a push could only ever carry one
     non-tag ref, so the pre-existing `check_fast_forward` rejection path
     doing a whole-batch `goto done` was unobservable as a bug; Phase 39's
     own multi-refspec fixture exposed it directly (a good ref got
     discarded alongside a bad one in the same push) and it was fixed to
     `had_rejection=1; continue;`, the same per-ref shape the tag-rejection
     and delete-target-missing paths already used right next to it.
  4. **An unqualified `<dst>` that matches MORE THAN ONE advertised ref is
     refused, not guessed at.** Measured: with both `refs/heads/dup` and
     `refs/tags/dup` on the remote, `git push origin topic:dup` prints
     `error: dst refspec dup matches more than one` and changes neither.
     `complete_dst`'s rule-1 loop therefore has to scan **every** guess
     prefix and count matches -- a "stop at the first hit" loop silently
     writes to whichever prefix comes first in `guess_prefixes[]`, i.e. to a
     ref the user never named. Same shape as the pre-existing
     `src refspec '%s' matches more than one` rule one layer up.
  5. **A push where every requested ref was rejected must touch the remote
     nowhere at all**, hence `if (had_rejection && entry_count == 0) goto
     done;` between the candidate loop and the `refs/sg/chunks` propagation
     block. Item 3's `goto done` -> `continue` change is what made this
     necessary: without the gate a fully-refused push falls through into
     chunks propagation and performs a REAL remote write (and prints
     `To <url>`), which pre-Phase-39 single-ref behaviour never did.
     **"Everything was already up to date" also leaves `entry_count` at 0**
     -- that case must still propagate chunks, which is why the gate tests
     `had_rejection` and not just the count.
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
  WARNING: **that interop is ONE-DIRECTIONAL, and "and vice versa" is
  wrong -- do not write it back in.** Measured both directions: sg pauses
  -> a real `git cherry-pick --continue` finishes it (works: log is
  correct, state is cleared). git pauses -> `sg cherry-pick --continue`
  (or `--skip`) CANNOT, because git's own `sequencer/todo` writes an
  abbreviated 7-hex id and sg has no abbreviated-object-name resolution at
  all to read it back with (same limitation the 7-vs-40 divergence two
  bullets up documents). What sg CAN still do on a git-paused sequence,
  after the Phase 57 spec 5b fix described below: `--abort` (restores to
  `sequencer/head`, which -- unlike `sequencer/todo` -- git also writes as
  a plain full 40-hex) and `--quit` (parses nothing at all). See Phase 57
  of `docs/DESIGN.md` for the full reasoning, including the dead-end this
  asymmetry caused before the fix.
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
  an abbreviated 7-hex, and this is deliberate** -- sg has no abbreviated-
  object-name resolution anywhere in this project (a rule that predates this
  phase, see `sg_rev_parse_commit`'s own header comment), so a 7-hex todo
  file would be one sg itself could not read back. Full hex is still
  readable BY GIT (its own parser accepts any length-4-or-more hex prefix),
  so the compatibility direction that matters -- "whatever sg writes,
  something can read back" -- is preserved. Pinned on both sides in
  interop's `phase57` group. **Do not "fix" this to 7 characters.**
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
  mistake), `cmd_commit.c` (the divergence #5 block below), `cmd_stash.c`
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
  lets it finish the pick. See the numbered list below.

## Deliberate divergences from real git

Five places where sg's answer differs from real git **on purpose**, not by
oversight -- each was measured against git 2.55.0, each is pinned on both
sides by an interop check (so accidentally "fixing" one back into silent
agreement with git would itself go undetected without the pin), and none of
them should be "fixed" without first re-reading the WARNING that explains
why the divergence exists.

(This list used to have a fifth entry, `-C -C` / `--find-copies-harder`
being rejected outright -- **implemented as of Phase 51**, see the module
notes' `-C -C` WARNING and the Phase 51 section of `docs/DESIGN.md`. It is
no longer a divergence, so it is gone from this list rather than marked
"fixed" in place, to keep the numbering meaning what it says.)

1. **`* Unmerged path` stays unquoted regardless of `core.quotePath`**
   (Phase 34) -- real git leaves this one line unquoted even when every
   other path is quoted; see PHASE34_ORACLE.md #1.
2. **`sg status --porcelain` prints a fixed `AD` for a path that escapes the
   repository via a crafted index, in all three possible real-world states**
   (Phase 36) -- real git actually reads the file outside the repository to
   decide between `A `/`AM`/`AD`; sg refuses to read it at all (the fix Phase
   36 exists to enforce), so it cannot compute which of the three is true and
   always reports the one that draws the user's attention rather than the one
   that could silently claim "clean". See the Phase 36 section of
   `docs/DESIGN.md` for the full three-row measurement.
3. **`sg push` uses exit code 1 where real git uses 128 for a client-side
   refspec syntax error** (empty dst, `--delete` with a colon, an
   already-`refs/`-prefixed malformed dst -- Phase 39) -- this project's own
   convention is "exit codes are only ever 0 or 1" (see Code conventions
   below), message text is otherwise borrowed from git's own wording. Pinned
   on both sides in `tests/interop.sh` (git-side 128, sg-side 1). **Does
   NOT apply** to a `<src>` that resolves to nothing (`error: src refspec
   ... does not match any`) -- git's own exit code there is already 1, no
   divergence to pin.
4. **`sg merge`'s conflict-marker "ours" label names the current branch
   where real git always writes `HEAD`** (`cmd_merge.c`'s `ours_label`,
   which also feeds the generated merge message and the summary line).
   Measured in Phase 41 across five situations -- a differently named
   branch, merging a tag, a detached HEAD, and rebase -- git's ours label is
   invariant; only the theirs label varies. **This is the oldest entry on
   this list and the last to get a witness**: it predates the list, and
   `cmd_merge.c`'s own comment claimed it was "pinned by phase4b" when no
   such check existed. Pinned on both sides since Phase 41 (interop's
   `phase41` group), including the asymmetry that **a detached HEAD makes
   the divergence disappear** -- `current_branch` is NULL there and sg falls
   back to git's own answer. `sg rebase` and `sg stash` are NOT on this
   list: rebase passes a literal `"HEAD"` and matches git, and stash's
   `Updated upstream`/`Stashed changes` match git's too.
5. **`sg commit` is BLOCKED while a cherry-pick or revert is stopped; real
   git lets `git commit` finish it instead** (Phase 57) -- git reads
   `CHERRY_PICK_HEAD`/`REVERT_HEAD` plus `MERGE_MSG` and even consumes the
   rest of `sequencer/todo`. sg refuses outright and tells the user to run
   `sg cherry-pick --continue` (or `sg revert --continue`). The reason is
   concrete: `cmd_commit.c` decides "is this a merge commit" purely from
   `sg_merge_head_exists`, and has no code path to restore the picked
   commit's author fields or to advance `sequencer/todo` afterward -- a
   `sg commit` that silently produced a wrong-author, un-advanced-sequence
   commit is worse than a refusal. Pinned on both sides in interop's
   `phase57` group (git: exit 0, the pick completes and the state clears;
   sg: exit 1, the state survives the refusal).

## Core types cheat sheet

Line numbers are anchors as of the time of writing and may drift -- go by
name.

| Concept | Type | Location |
|---|---|---|
| Object kind | `sg_obj_type` | `include/sg/object.h:8` |
| Parsed object (content is **borrowed** from the caller's buffer) | `sg_object` | `include/sg/object.h:33` |
| tree / commit / tag | `sg_tree`, `sg_commit`, `sg_tag` | `object.h:46,70,91` |
| index and its entries | `sg_index`, `sg_index_entry` | `include/sg/index.h:23,8` |
| growable byte buffer | `sg_buf` | `include/sg/http.h:6` |
| ref advertisement | `sg_ref_adv`, `sg_remote_ref` | `include/sg/transport.h:14,19` |
| push request/report | `sg_push_ref_update`, `sg_push_report` | `include/sg/transport.h:72,78` |
| chunk pointer | `sg_chunk_pointer` | `include/sg/chunk.h:20` |
| SHA-1 length constants | `SG_SHA1_RAW_LEN` / `_HEX_LEN` | `include/sg/hash.h:6-7` |

The version string has exactly one definition: `SG_VERSION`
(`include/sg/version.h:13`), referenced simultaneously by `sg --version`, the
transport layer's agent string, and the `.TH` line of `docs/sg.1` -- when
bumping the version, keep the man page in sync.

## Code conventions

- External symbols always use the `sg_` prefix + snake_case; typedefs do not
  get a `_t` suffix; file-local static helper functions **do not** get the
  `sg_` prefix. Include guards use `SG_<UPPERCASE-FILENAME>_H`, not
  `#pragma once`.
- **Errors return `int`: 0 for success, -1 for failure. There is no unified
  error type or macro**, semantics are described in header comments. A few
  read paths have a third state, `-2` ("the pointer is valid but the data is
  corrupt", e.g. `sg_chunk_read_blob`, `include/sg/chunk.h:127-131`) -- you
  have to read the header comment to know the signature.
- Error messages are printed by the CLI layer, the lower layers generally
  do not print -- but `pack.c` and `http.c` are pre-existing exceptions,
  they `fprintf(stderr, "sg: ...")` themselves. Do not assume the layering
  is clean.
- User-visible output: errors go to stderr prefixed with `sg: `; usage errors
  print `usage: sg <cmd> ...` (**without** the `sg:` prefix); exit codes are
  only ever 0 or 1, never a third value. There is no `sg_die`/`sg_error`
  helper, each call site does its own `fprintf`.
- Memory: plain malloc/free, no arena. Every compound struct gets a paired
  `_free`. Header comments state whether it's owned or borrowed, and new APIs
  follow the same wording. Two process-lifetime caches are **deliberately**
  never freed: the mmap pack registry in `pack.c:498` and the keepalive
  cache in `chunk.c:744`. These two used to be the reason CI had leak
  detection turned off, but that reason was wrong -- both are attached to
  file-scope global variables, and LSan treats globals as roots, so
  still-reachable does not count as a leak. **CI's ASan job now runs with
  `detect_leaks=1`**, any new process-lifetime cache needs to likewise hang
  off a global, or CI will go red.
- Adding a subcommand touches three places (**no need to touch the
  Makefile**, `src` is globbed in): create `src/cli/cmd_xxx.c`, add the
  declaration in `include/sg/cli.h`, and add both a description to
  `COMMANDS[]` (`src/cli/cli.c:13`) and a `strcmp` in the dispatch chain
  (starting at `:63`).

  A new command that will overwrite the working directory needs to decide
  two separate things, do not treat them as one choice:

  **(1) Gate** -- should a dirty working directory / an in-progress rebase /
  an in-progress merge block it?
  - `switch`/`merge`: reject outright. `switch` has one **explicit** gate
    each for rebase and merge (`cmd_switch.c`, Phase 14 and Phase 16), both
    before any side effect, neither bypassed by `--force`, and `-c` does not
    create the branch either. **Do not rely on `sg_safe_apply_tree`'s dirty
    confirmation as a stand-in** -- `--force` bypasses it exactly, and that
    is how the Phase 16 bug happened.
  - `stash apply`/`stash pop`: **since Phase 20, no longer a blanket
    rejection**, changed to `sg_stash_apply_check_dirty`
    (`include/sg/stash.h`) which only blocks dirty changes on paths this
    particular merge actually touches; paths already deleted in the working
    directory do not block it. An in-progress rebase is still rejected
    outright (consistent with switch/merge), this part is unchanged.
  - `reset --hard`: goes through `sg_safe_apply_tree` (confirmation +
    snapshot).
  - `stash push`: **not blocked** -- "the working directory is dirty" is its
    input, not a danger, so it calls `sg_apply_tree_to_workdir` directly and
    calls `sg_snapshot_create` itself first; using `sg_safe_apply_tree` would
    misfire during a rebase because `apply.c:311-312` counts rebase state as
    dirty, and would demand `--force` when running non-interactively.

  **(2) Finish** -- which in-progress states get ended?
  - `MERGE_HEAD`: cleared by any overwriting operation that **actually goes
    through with it** (measured against real git 2.55.0; `stash push`
    clears it too, without a warning -- sg additionally prints one stderr
    line, but the state ends up exactly the same). `switch` is not on this
    list: it is rejected by the gate above and never reaches the finish step
    (real git's `switch` rejects too; the one that clears it is
    `checkout -f`, and sg has no `checkout`).
  - Rebase sequencer state: **nothing may touch it except rebase's own
    subcommands** (measured in Phase 14). `stash push` is the representative
    case of "does not block it and does not clear it, leaves it untouched".
  - `cmd_undo.c` remains the sole exception (it has no real-git counterpart);
    it clears things itself after returning.

  **"Is a merge in progress" always uses `sg_merge_head_exists`**
  (`include/sg/merge.h`). `sg_merge_head_read` collapses "no merge" and
  "corrupt state" into the same -1, and using it as the predicate would make
  a corrupt `MERGE_HEAD` look like "no merge" -- the result is switch
  rejecting forever with no command able to clear it. Within `src/`,
  `sg_merge_head_read` **has only one caller left, `cmd_commit.c`**, because
  it is the only one that actually needs the value (the second parent); it
  asks `_exists` first, then `_read`, and rejects like real git if the read
  fails, rather than silently producing a single-parent commit. New code that
  asks "does a merge exist" should not introduce a second `_read` call site
  (Phase 16).

## Testing conventions

- 64 independent unit test `.c` files, **no shared header, no test
  framework**. Each file carries its own `static int failures = 0;` and a
  same-named `CHECK(cond, ...)` macro (prints `FAIL %s:%d` and
  `failures++` on failure, **does not abort**), and `main` ends with
  `return 1` if `failures > 0`. To add a test, copy `tests/test_confirm.c`
  (75 lines, the shortest complete example).
- **Anything dropped into `tests/` gets run** -- `Makefile:48` auto-collects
  it via `find tests -name '*.c'`, no registration needed. Tests link
  against `LIB_OBJS` (excluding `main.o`), so they can call internal
  functions directly.
- When you need a temporary repo, copy the existing `make_tmp_repo()` (e.g.
  `tests/test_apply_tree.c:28`): `mkdtemp("/tmp/sg_<name>_test_XXXXXX")` +
  `sg_repo_init()`, using `exit(1)` when setup fails (semantically distinct
  from an assertion failure). **There is no shared fixture helper, do not go
  looking for one.** Most tests do not clean up `/tmp`, leftovers are a
  known phenomenon.
- To run a single test: `make build/tests/test_foo && build/tests/test_foo`.
- **This project has had two incidents of "an empty test that can never
  FAIL". After adding or modifying a test, you must prove it can go red
  before trusting it**. Use `bash tests/mutate.sh <name> <file> <perl-expr>
  [<test-binary>|--interop]`: it copies the working tree into a scratch
  directory, applies the mutation to the copy, does a full rebuild, and
  reports which named checks turned red. **Do not restore with
  `git checkout --`**, that has wiped out an entire file before. This step
  is run by the main conversation, it is not handed to whoever wrote the
  test to verify themselves.

  The script has four hard-won behaviors baked in, know these when reading
  its output: every round does a **full rebuild** from a clean copy (a stale
  `.o`'s mtime would let make skip recompiling, and mutations would silently
  accumulate across rounds); **a non-zero exit code counts as caught**, FAIL
  lines or not (a boundary mutation once made the test binary segfault, and
  grepping only for FAIL would misreport it as a blind spot); a perl
  expression that **matches nothing exits with code 3** immediately, it does
  not pretend to have run (changing nothing obviously never goes red, and
  that is the most common source of a false negative); and
  `SG_MUTATE_TIMEOUT` (default 300 seconds) turns a **hang** into a failure
  labeled "timeout" -- a mutation can leave a merge loop's cursor never
  advancing and never finishing, and "never exits" is neither 0 nor non-zero,
  so the old version of the script would just silently sit there holding the
  terminal (measured in Phase 25, hung for thirty minutes). **Timeout and
  crash are labeled separately**, because both only prove "breaking this
  causes trouble", not that the named assertion has any discriminating power.

  **There are three different reasons a mutation can stay green, do not lump
  them together** (Phase 25): a **genuine blind spot** (that dimension has no
  test, needs one, and it must be anchored to an external oracle); a
  **redundant guard** (the real defense line is one layer down, delete the
  guard so the mutation lands there instead); and **mathematically
  unobservable** (that value gets unconditionally overwritten afterward,
  write down the proof and switch to a property you can actually verify).
  Only the first one is a coverage gap; treating all three as the same thing
  sends the next person hunting for a test that does not exist.

  WARNING: **per-site vs. batch**: the script's comment says "if a literal
  appears more than once, you must add `/g`" -- that answers "is this rule
  enforced at all". When answering "**does each site individually have
  coverage**", `/g` is exactly the wrong tool -- it smears the results of
  every site together, and the whole thing goes red as long as any one site
  is covered. To tell apart sites sharing the same literal, use surrounding
  context (indentation depth, the preceding call) instead, `/g` is not needed
  (measured in Phase 25: of `sg_chunk_effective_id`'s two sites, one had
  coverage and one was a genuine blind spot).

  Going red is not enough by itself, **it has to be red for the right
  reason**: confirm the failure message actually points at the property you
  meant to verify. There was once a test that did go red under a 2-commit
  fixture, but the reason was that the root commit has no parent, unrelated
  to the guard; and there was once a set of assertions that "looked like"
  they were verifying syntax, but were actually being blocked by an unrelated
  bounds check. Also, **a redundant defensive check hides the verification
  point** -- deleting a guard that duplicates existing code sometimes turns
  zero tests red, because the real defense line is one layer down, and the
  mutation has to land there to count (Phase 17).

## Delegation (criteria and standing clauses are in the global
`~/.claude/CLAUDE.md`; this section records only what is specific to this
project)

- This project's cost problem is that **the work of reading files stays in
  the main conversation**: the 2026-08-07 baseline was an average context of
  356K, and a delegation density of 2.0 per 100 turns. Before starting a
  milestone, dispatch `surveyor`s in parallel across non-overlapping scopes
  (this file itself was written that way), do not read `src/` in the main
  conversation as you go.
- When dispatching surveyors, split the scope using the module table above,
  one agent per 3-4 subdirectories works well; `cli/`'s 19 `cmd_*.c` files
  are too fine-grained, name specific commands instead of the whole
  directory.
- **A subagent reporting "all green" has repeatedly been wrong in this
  project** -- the final gates of `make test` / interop.sh are rerun by the
  main conversation itself, do not trust a relayed number.
- Delegation specs must additionally state: the completion criteria are in
  this file's "Build and verification" section (including interop.sh --
  agents often declare done after just `make test`); touching ignore/
  traversal needs a run of fuzz_ignore.py; touching diff output needs a run
  of fuzz_diff.py and **the actual mismatch count reported** (not just
  "did it fail").

## Token throttling

- Open one module at a time, verify with `make test` right after changing
  it, do not re-read the whole batch.
- Delegate lookup-style questions to `Explore`, do not scan files in the
  main conversation.
- Pipe `make test` / interop.sh output through `2>&1 | tail -40`, or write
  it to a file first and grep for FAIL lines, do not leave thousands of
  lines of raw output in the conversation.
- `/clear` at milestone boundaries, continue the new session from this file
  plus the most recent entries of `docs/DESIGN.md`.
