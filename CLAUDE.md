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
make test                         # one binary per tests/*.c, any failure fails the whole thing
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
- In the `make test` line's "N/M ran", if N is less than M it means
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
  binary and reports `analyzed N/M`, the same "not measured != measured as
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

### Per-module rules -- READ THE MATCHING FILE BEFORE TOUCHING THE CODE

Everything this file used to spell out inline about one specific module now
lives in a `docs/RULES-*.md` of its own. They are **not** background reading:
each is a list of measured rules and `WARNING:`s, and most of those exist
because something slipped past a fully green board. **Before editing any file
in a row below, read that row's file first.**

**A file can appear in more than one row. Read every row that names it, not
the first one you hit** -- `cli/cmd_merge.c` has rules in three different files
and `cli/cmd_diff.c` in two, so stopping at the first match loses most of
them.

| Touching | Read first |
|---|---|
| `storage/refs.c`, `storage/revparse.c`, `refs.h`, `revparse.h`, `objstore.h`, `object.h`; detached HEAD; `cli/cmd_merge.c`'s `<rev>`/message/fast-forward output; `cli/cmd_tag.c`; `cli/cmd_branch.c`; `cli/ref_delete.c`; `ref_delete.h`; `sg_message_cleanup` | `docs/RULES-refs-revparse.md` |
| `util/date.c`, `date.h`, `cli/cmd_undo.c`'s own formatter; any `--date=` / `%ad` / `%ar` / `%ah` work | `docs/RULES-date.md` |
| `cli/cmd_log.c`, `cli/cmd_show.c`, `cli/cmd_cat_file.c`, `cli/commit_out.c`, `cli/log_graph.c`, `commit_out.h`, `log_graph.h` | `docs/RULES-log-show.md` |
| **any file that joins a path, prints a path to the user, deletes a tracked file, or builds a user-facing string with `snprintf`** -- `sg_path_join`, `sg_quote_path*`, `sg_path_component_is_safe`, `sg_prune_empty_parents`, `sg_strfmt_alloc`; `workdir.h`, `quote.h`, `strfmt.h`; `workdir/apply.c`, `workdir/merge.c`, `object/tree.c`, `storage/refs.c`, `storage/repo.c`, `safety/stash.c`, `cli/pick.c`, `cli/cmd_add.c`, `cli/cmd_restore.c`, `cli/cmd_reset.c`, `cli/cmd_merge.c`, `cli/cmd_rebase.c`, `cli/cmd_branch.c`, `cli/ref_delete.c` | `docs/RULES-paths-strings.md` |
| `workdir/diff.c`, `cli/diff_out.c`, `cli/cmd_diff.c`, `util/diff_lcs.c`, `diff.h`, `diff_out.h`, `tree_build.h` | `docs/RULES-diff.md` |
| `workdir/merge.c`, `cli/cmd_merge.c`, `merge.h` | `docs/RULES-merge.md` |
| `workdir/rename.c`, `util/similarity.c`, `cli/cmd_diff.c`'s `-M`/`-C`/pathspec parsing, `pathspec.h`, `similarity.h` | `docs/RULES-pathspec-rename.md` |
| `cli/cmd_status.c`, `workdir/status.c`, `workdir/tree_build.c`, `status.h` | `docs/RULES-status.md` |
| `net/ssh.c`, `net/transport.c`, `cli/cmd_clone.c`, `ssh.h`, `transport.h` | `docs/RULES-net.md` |
| `safety/stash.c`, `cli/cmd_stash.c`, `stash.h` | `docs/RULES-stash.md` |
| `cli/cmd_push.c` | `docs/RULES-push.md` |
| `cli/pick.c`, `cli/cmd_cherry_pick.c`, `cli/cmd_revert.c`, `cli/cmd_rebase.c`, `cli/cmd_commit.c`, `cli/cmd_switch.c`, `cli/cmd_undo.c`, `safety/sequencer.c`, `safety/rebase.c`, `pick.h`, `sequencer.h` | `docs/RULES-sequencer.md` |
| `cli/cli_args.c`, `cli_args.h`, `workdir/tree_build.c`, `tree_build.h`, `storage/chunk.c`, `storage/reflog.c`, `safety/snapshot.c`; and anything at all before writing a second copy of something | `docs/RULES-duplication.md` |

**Source comments and `docs/DESIGN.md` still say "CLAUDE.md's X entry"** --
173 places in `src/`/`include/`/`tests/` and 83 in `docs/DESIGN.md`, as of Phase
71. They were deliberately NOT rewritten (47 of them are in `tests/interop.sh`,
where a quoting slip silently stops every check below it, for the gain of one
fewer hop while reading a comment), and they are not stale, because the table
above resolves every one of them: **if the named entry is a module rule, it is
now in the `docs/RULES-*.md` on the row matching the file the comment lives in.
If it is not a module rule -- the gates, the exit-code convention, the numbered
deliberate divergences, the module table, the mutation classification, the
testing or delegation conventions -- it is still in this file.** **The file-matching half of that rule only works for a file the table names,
which is no file under `tests/`** (89 of the 173, 47 of them in
`tests/interop.sh`) **and not `docs/DESIGN.md` either. From those, match by
TOPIC, by grepping `docs/RULES-*.md` for a distinctive phrase out of the
comment -- do NOT infer the file from what the commenting file is about.**
Measured counterexample: `tests/fuzz_merge_rename.py`'s reference to
"CLAUDE.md's phase45 note" resolves to `docs/RULES-status.md`, not to either
of the merge or rename files a reader would guess from that script's subject.
A handful name a lesson that was never in this file at all (e.g. "measure git
behaviour, never recall it"); those were already wrong before the split and
are left alone.

**The table itself can go stale silently, so it has a check** -- every sg
source file named by a rule in a `docs/RULES-*.md` must appear in some row, or
the table will never send anyone to its rules:

```
python3 - <<'EOF'
import re, glob, os, collections
real = {fn for r in ("src","include") for _,_,fns in os.walk(r) for fn in fns
        if fn.endswith((".c",".h"))}
table = "".join(l for l in open("CLAUDE.md")
                if re.search(r'\|\s*`docs/RULES-\S+\.md`\s*\|\s*$', l))
m = collections.defaultdict(set)
for f in sorted(glob.glob("docs/RULES-*.md")):
    for n in re.findall(r'[A-Za-z0-9_]+\.[ch]\b', open(f).read()):
        if n in real: m[n].add(f.replace("docs/RULES-", ""))
for n, fs in sorted(m.items()):
    if not re.search(r'[`/]' + re.escape(n) + r'`', table): print(n, sorted(fs))
EOF
```

Three things in it are load-bearing and each was got wrong once. It reads
ONLY the pointer table's own rows -- the ones whose last cell is a
`docs/RULES-*.md` link, matched loosely on purpose (`\S+` and `\s*`, so a new
row whose filename carries a digit or an underscore, or whose spacing differs,
is still scanned) -- because a filter of "every line starting with `| `"
also swallows the Module layout table above and the Core types cheat sheet
below, and the module table's `cli/` row happens to name `` `diff_out.c` ``:
with the looser filter, deleting `cli/diff_out.c` from the row that actually
governs it left the check GREEN (measured). It resolves each name against the
files that actually exist under `src/` and `include/`, which is what keeps
git's own sources (`xdiffi.c`, `xhistogram.c`, `xprepare.c`, `delta.c`) and
prose examples (`other/d.c`) out of the set -- a hand-written allowlist would
excuse a real gap just as readily. And it requires a delimiter around the name
in the table, so a short name cannot be absorbed as a substring of a longer one
(a real `d.c` would otherwise match inside `cmd_add.c`; `d.c` itself is
excluded one step earlier by the real-file filter, so it is NOT the example
this guard handles). The guard is not idle: it changes nothing in the gate's
own answer today, but it decides one row of the per-row count below --
`pathspec-rename.md` names `workdir/diff.c` in prose, and without the delimiter
`diff.c` is found hiding inside that same row's `cli/cmd_diff.c` and wrongly
suppressed, giving 51 pairs instead of 52. Expected
leftovers today: none, so ANY output is a gap. Mutation-verified rather than
assumed: deleting `cli/cmd_tag.c` from its row makes it report `cmd_tag.c`, and
deleting `storage/repo.c` makes it report `repo.c`. (Both mutations first came
back green through a shell heredoc that had silently eaten the string being
removed -- run it from a real script file, not an inline heredoc with
backticks in the argument.) `tests/test_*.c` are excluded by construction --
they are named as evidence FOR a rule, not governed BY one.

WARNING: **this check asks "is the name anywhere in the table", NOT "is it on
the row of every RULES file that has a rule about it", and the second question
is the one that actually matters.** A file already listed in one row for one
reason is invisible when it is missing from another row it also needs, and the
check stays silent. Measured: `cmd_merge.c` and `cmd_rebase.c` sat in the
refs-revparse/merge and sequencer rows while both were missing from the
paths-strings row, where a WARNING names them for the `snprintf` truncation
bug that gave `sg merge` a silently WRONG COMMIT ID -- the check above was
green for that the whole time, and a cold read is what found it. The per-row
candidate list is `for each docs/RULES-X.md, every real source file it names
that is not on X's own row`; measured at Phase 71 with the same real-file and
delimiter rules as the check above, that is **52 (file, name) pairs, 35 of them
outside the catch-all duplication row** -- quote the algorithm when you quote a
number here: dropping all three of the real-file filter, the delimiter and the
per-file dedup gives exactly 100 instead, and the two readings are not
comparable. That list **needs manual triage and must not be
shipped as a gate**, because most of its entries are a file mentioned in
passing ("`sg_merge_trees` reuses `sg_diff_trees`"), which is not the same as a
rule about that file, and no regex tells the two apart. When you add a rule naming a file, put it on the row
yourself -- the automated check is a floor, not a proof.

**Where a new lesson goes**: a newly measured rule or `WARNING:` goes into the
matching `docs/RULES-*.md`, **never into this file**. This file gains a line
only when a new module appears and needs a new row above. Phase 71 split it
because 70 phases had each appended one more block here, taking it to 3400
lines / 233 KB -- about 60k tokens loaded on every single request, and past the
point where the harness warns that it is too large to keep in context.

## Deliberate divergences from real git

Nine places where sg's answer differs from real git (the numbering still
runs 1-10, with entry 8 retired -- see the parenthetical below for where it
went; do not renumber the rest into a lie). Each was measured
against git 2.55.0 and each is pinned on both sides by an interop check, so
accidentally "fixing" one back into silent agreement with git would itself go
undetected without the pin.

**They are not all the same kind, and the question that separates them is
"is sg's answer one we ACCEPT", not "why does it differ".** Every entry below
is an answer this project keeps: some because matching git would be wrong or
needs a knob sg does not have, others because converging would cost more than
the difference is worth. Do not "fix" one of those without first reading the
WARNING that says why it is there. **No entry currently on this list is a
deferred defect** -- the one that WAS on this list (`sg tag -d` deleting a
tag on a case-aliased argv pair where git refused the whole batch, which
briefly occupied the number 9 below) is fixed; see the parenthetical after
this preamble for where it went. The current entry 9 is a NEW, unrelated
entry, added after that number was freed up -- do not confuse the two by
number alone. **When adding a future entry, still say which of the two
kinds it is** -- a reader who applies "none of these should be fixed" to a
deferred defect will leave it in place on the strength of this list's own
framing, which is exactly what let the old entry 9 sit here labelled a bug
rather than a protected answer.

(This list used to have a fifth entry, `-C -C` / `--find-copies-harder`
being rejected outright -- **implemented as of Phase 51**, see
`docs/RULES-pathspec-rename.md`'s `-C -C` WARNING and the Phase 51 section of
`docs/DESIGN.md`. It also used to have a ninth entry, `sg tag -d <Name>
<name>` deleting a tag where git refused the whole batch -- **fixed as of
Phase 74**, which reproduces git's OWN mechanism: before deleting anything,
it creates `refs/tags/<name>.lock` (`O_CREAT|O_EXCL`) for every name in argv
order, exactly as git does, and treats `EEXIST` as the collision. (Phase
74's round 1 tried an `(st_dev, st_ino)` compare of the two names' REF files
instead and was measured wrong in both directions -- it missed a pair that
is entirely packed-refs, since a packed ref has no loose file to `lstat`,
and it over-refused two ref files deliberately hardlinked to one inode,
since their LOCK paths are unrelated strings that never alias even though
the refs do. Round 2 replaced it with the lock mechanism above. Round 3
found the `EEXIST` branch printed the in-batch aliasing message even when
the colliding lock belonged to nobody in the batch -- a stale lock from a
crashed process said "'refs/tags/foo' and 'refs/tags/foo' are the same
ref", which is false (a ref cannot alias itself); it now checks whether the
colliding lock is one this batch holds and, if not, prints git's own
"cannot lock ref" wording instead, keeping the aliasing message only for
the case it is actually true. Round 3 also filtered `.lock` entries out of
`sg_ref_list_under`'s enumerator, shared by `sg tag`'s and `sg branch`'s
listings, which had been listing a `.lock` file as a phantom ref -- a
pre-existing gap round 2 made newly reachable, since it is the first code
under `src/` to ever create one. Round 3's own filter was itself a
regression: it tested the bare dirent name before checking `S_ISDIR`/
`S_ISREG`, so a DIRECTORY whose name ended in ".lock" took its entire
subtree with it (not just a lock file directly inside it) -- fixed by
moving the check to after `stat()`, gated on `S_ISREG`, so a directory is
always recursed into regardless of its own name. That regression exposed a
pre-existing gap one layer down: `sg_ref_name_valid_for_create`'s ".lock"
check only ever inspected the WHOLE name's last 5 bytes (equivalent to
checking just the last path component), so a MIDDLE component ending in
".lock" was creatable when real git's check-ref-format rejects it at any
component -- round 4 walks every component. Round 4 also fixed the
stale-lock message to match git's singular/plural boundary (one existing
ref in the transaction: "could not delete reference X:"; two or more:
"could not delete references:", keyed on existence, not raw argv count),
and replaced a self-referential test check (comparing sg's output against
a second hardcoded copy of the same literal) with one that derives the
expected wording from git's own output.) See `docs/DESIGN.md`'s Phase 74,
"Phase 74 round 2", "Phase 74 round 3", and "Phase 74 round 4" sections.
Both entries are gone from this list rather than marked "fixed" in place,
to keep the numbering meaning what it says. Entry 8, `sg tag
<name> <unresolvable-rev>` reporting a different sentence than git
(`cannot resolve 'x'` vs git's `Failed to resolve 'x' as a valid ref.`) --
**fixed as of Phase 75**, which resolved the "wider finding" that entry's
own text recorded (sg had four different wordings for "this revision does
not resolve", across eight call sites, none of them consistent with each
other) by aligning every one of those call sites to git's own per-command
wording instead of picking one sg sentence to converge onto. See
`docs/DESIGN.md`'s Phase 75 section. Gone from this list rather than
marked "fixed" in place, same as the other two retired entries above.)

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
6. **`sg log --graph` combined with an EMPTY `--pretty=format:`/`tformat:`
   format string AND a diff (`-p`/`--stat`) does not reproduce git's
   layout** (Phase 63 review round). git renders an entry's header and its
   diff as two INDEPENDENT graph-prefixed blocks: an empty header still
   gets its own `"* "` marker, and the diff that follows gets its OWN
   `"| "` immediately after it, on the SAME physical line
   (`"* | diff --git a/f b/f\n..."`). sg captures a whole entry (header +
   diff together) as a SINGLE block and prefixes it as one unit, so it can
   only ever produce `"* "` directly followed by the diff's own first byte
   -- there is no code path that produces "one empty graph block, then a
   second block" from a single capture. Reproducing git's shape would
   require `sg_commit_out_entry` to expose the header/diff boundary to its
   caller, and that boundary is also `sg show`'s own public contract --
   not worth changing for this degenerate combination of inputs. Pinned on
   both sides in interop's `phase63` group as two LITERAL byte pins (not a
   git-vs-sg `cmp`, since the two are expected to differ): one asserting
   git's exact bytes, one asserting sg's exact bytes, so a future change to
   either renderer fails by name instead of silently drifting apart from
   its own pin.
7. **`--date=*-local` disagrees with git for a zone whose offset is not a
   whole number of minutes** (Phase 64). Such zones exist AFTER 1970 --
   `Africa/Monrovia` is `-2670` seconds until 1972-01-07 -- so this is
   reachable with an ordinary, positive-timestamp commit (epoch 0) and
   `TZ=Africa/Monrovia`, no `--literally` and no crafted object needed.
   Measured, all six `-local` names: git prints
   `1969-12-31 23:15:30 +0000`, sg prints `1969-12-31 23:16:00 -0044`.
   **Both halves of git's answer come from different code and contradict
   each other**: the wall clock is `localtime_r` at full second precision,
   while the offset LABEL goes through git's own `local_tzoffset`, which
   returns 0 whenever the local time lands before 1970 (its `tm_to_time_t`
   rejects `tm_year < 70`) -- so git labels a `-00:44:30` clock `+0000`.
   sg routes `-local` through a whole-minute `+HHMM` string and is
   internally consistent instead. **Fixing only sg's clock would produce
   agreement in exactly ZERO cases** -- a non-whole-minute offset is the
   only situation that diverges at all, and in every one of those git's
   label is also wrong -- so byte-for-byte agreement would mean
   reproducing git's own inconsistency. Pinned on both sides in interop's
   `phase64` group as twelve LITERAL byte pins (six git, six sg), the same
   shape as divergence #6, with a `skip()` for a machine whose zoneinfo
   lacks the zone.
   WARNING: **this entry replaced a claim in `docs/DESIGN.md` that the
   shape was "measured inert, no oracle in either direction"** -- that
   claim was written from a zoneinfo scan that reported no post-1970
   non-whole-minute offset, and the scan was simply wrong. A negative
   result needs its own path verified before it becomes a documented
   reason not to act; see the Phase 64 section of `docs/DESIGN.md`.
   **`--date=human-local` joins this divergence as of Phase 67** (its
   caller resolves `tz` the identical way every other `-local` name does,
   so the same whole-minute-shift gap applies): measured on a 1970-06-01
   commit under `Africa/Monrovia`, git `Mon 11:15`, sg `Mon 11:16`. Plain
   `human` (non-local) does NOT diverge there (measured:
   `Mon 12:00 +0000` on both). Pinned on both sides in interop's `phase67`
   group, same two-literal-pin shape as the rest of this entry.
9. **`sg tag`/`sg branch` LIST a ref reachable only through a
   `.lock`-suffixed path component; real git refuses to even resolve one**
   (Phase 74 round 5). WARNING: **this number was previously something
   else.** Entry 9 used to be `sg tag -d Foo foo` deleting a tag git
   refuses to touch, which Phase 74 round 2 FIXED and removed from this
   list; the number was then reused here. A reference to "divergence 9"
   written before Phase 74 means the deleted one, not this one -- check
   the date before trusting such a pointer. Built by hand (no git porcelain command produces
   this shape, since check-ref-format already rejects creating a name with
   such a component -- see `sg_ref_name_valid_for_create`'s own per-component
   fix, same phase): a loose ref file at `refs/tags/sub.lock/inner`, put
   there directly rather than through any tool's own create path. Measured:
   `git rev-parse --verify refs/tags/sub.lock/inner` fails outright (not
   merely "not listed" -- git's ref resolution refuses the path at every
   layer), and `git tag`/`git for-each-ref` show nothing for it; `sg tag`
   lists `sub.lock/inner` right alongside an ordinary sibling tag. **This is
   the same split this project already applies to a broken tree object**
   (see the module notes above: real git's object store accepts one as-is
   and `cat-file -p` can still read it out, precisely so there is a way to
   inspect and recover from something a stricter reader would simply
   refuse to touch) -- sg is deliberately STRICT at CREATION (the
   validator rejects the name going forward) and deliberately TOLERANT when
   READING whatever already exists on disk, regardless of how it got there
   (an older sg build before this fix, or another tool entirely). `sg
   tag -d` is the recovery tool for exactly this shape, and hiding it from
   the listing would remove the only way a user finds out such a ref
   exists at all.

   **Why this needs a list entry despite needing a hand-built fixture to
   reach**: the question a list entry answers is not "will anyone
   encounter this", it is "could a future edit silently un-fix a real bug
   by chasing agreement with git here". Concretely: `list_loose_branches`
   (`src/storage/refs.c`) used to prune any directory entry ending in
   ".lock" before ever checking whether it was a directory at all, which
   took an entire subtree with it -- a real bug, closed the same round
   this entry was added, where a ref sg could delete by name was invisible
   to its own listing. Restoring that prune (e.g. to "match git" for this
   exact fixture) would silently reintroduce that bug. **The prune must
   not come back**; if this shape's sg-side behavior is ever revisited, it
   should be revisited on its own terms, not because this entry's git-side
   pin looked like something to converge onto. Pinned on both sides in
   interop's `phase74 case2p` group: a precondition asserting git's
   listing does NOT contain the nested ref (while an ordinary sibling tag
   still does), and a second check asserting sg's listing DOES contain it,
   for both `sg tag` and `sg branch` (both share `sg_ref_list_under`'s one
   enumerator).
10. **`sg branch -d`/`-D`/`-f` refuses a case- or Unicode-normalization-
    aliased spelling of the checked-out branch; real git does not, and
    leaves `HEAD` dangling** (Phase 76 fix round 2) -- an ACCEPTED answer,
    not a deferred defect: git's own answer is unsafe, sg's is the safer
    one. Measured on a case-insensitive filesystem (macOS default): with
    `master` checked out, `git branch -d Master` (or `-D Master`, or
    `-f Master <rev>`) exits 0, prints `Deleted branch Master (was
    <hex>).`, and actually deletes/moves `refs/heads/master` (the SAME
    loose file, since the filesystem folds the two spellings to one path)
    while `HEAD` still symrefs to `refs/heads/master` -- the repo is left
    looking like "No commits yet" (delete) or with `HEAD`/the working tree
    disagreeing (`-f`). **The identical shape reproduces via Unicode
    NORMALIZATION instead of case** (measured, `NOTES-nfd.md`, not in this
    repo): with `core.precomposeUnicode` false or unset, an NFD-spelled
    argv name (`cafe\xcc\x81`) aliases an NFC-named checked-out branch
    (`caf\xc3\xa9`) on APFS the same way a differently-cased spelling
    does -- case-folding and normalization-folding are independent
    filesystem properties that happen to both be true on APFS, probed
    separately in interop (`P73_FS_CASE_INSENSITIVE` and a second,
    NFC/NFD-specific probe). git's own checked-out-branch safety check is
    a plain string compare against the current branch's name, which
    neither alias spelling textually equals, so it never fires; the
    actual delete/force-update then goes through the OS, which resolves
    both alias forms to the same file. sg detects this with a lock-path
    probe (`cmd_branch.c`'s `branch_aliases_current`) -- NOT `strcasecmp`
    (wrong on a case-SENSITIVE filesystem, and blind to normalization
    aliasing entirely) and NOT an inode compare of the ref files (Phase 74
    round 1 measured that wrong in both directions for an unrelated
    collision) -- and refuses with git's own "used by worktree" wording,
    exit 1, nothing changed. A packed-only current branch cannot alias
    this way (packed-refs lookup is an exact string match, unaffected by
    filesystem case- or normalization-folding) and keeps its ordinary
    "not found" answer. A resource failure (allocation/mkdir) inside the
    probe fails CLOSED (an "out of memory" refusal), never silently as
    "no alias" -- round 1 shipped this collapsed into a fail-OPEN 0/1
    return, closed in round 2 (see `docs/RULES-paths-strings.md`'s own
    Phase 76 bullet).
    **What is actually pinned, precisely** (round 1 claimed a pin here and
    had none at all -- confirmed empirically, the main conversation's
    mutation battery found M3/drop-the-probe-at-create and
    M4/drop-the-probe-at-delete both stayed fully green against round 1's
    tree): for `-d Master`/`-D Master`/`-f Master HEAD~1`, from three cwd
    contexts each (repo root, a subdirectory, cwd reached through a
    symlink to the repo) -- a git-side precondition (exit 0, the ref
    actually gone or moved) and a SEPARATE sg-side literal-string
    assertion of the full refusal line INCLUDING the worktree path (not a
    git-vs-sg comparison: real git prints no message here at all in the
    cases this entry is about), plus `refs/heads/master` and its reflog
    byte-identical to an untouched copy and no `.lock` file left anywhere
    under `.git`; one more pin for the identical shape via NFD/NFC
    normalization; and one pin that a FOREIGN stale
    `refs/heads/master.lock` does not make `-d Master` misfire (measured:
    it does not, but via the SHARED `ref_delete.c` transaction's own
    lock-collision detector independently catching the case-folded
    collision, not via `branch_aliases_current` itself, which cannot
    safely probe when the current branch's own lock path is already held
    by something else and documented-falls-through as "not aliased" in
    that one case).
    **Phase 76 fix round 3 (L1): the CREATE/`-f` side of this same
    scenario was a REAL, unrelated hole, not part of this divergence --
    `sg branch -f Master HEAD~1` with a foreign `refs/heads/master.lock`
    used to move the checked-out `master` anyway, because the create path
    took no lock of its own and the alias probe's documented fall-through
    (above) let the name through unchecked. Real git refuses this cell
    too (`fatal: cannot lock ref ...`), so closing it needed no new
    divergence, only a lock `sg branch`'s create/`-f` write had simply
    never taken -- now closed the same way `sg tag -d`/`sg branch -d`
    always were, via the shared `sg_ref_lock_try` (`refs.h`). Pinned
    separately (`phase76 L1 (*)` in interop, a `lockcreate` oracle
    fixture) from the alias-probe pins above; a mutation disabling the
    create lock and a mutation disabling the alias probe are each
    expected to red a DIFFERENT named set. This is git PARITY, not part
    of the divergence: the divergence itself is still exactly "no foreign
    lock present," where the alias probe (not any lock) is what refuses.
    **`sg_ref_update` itself still takes no lock at all, project-wide,
    for every OTHER ref-writing command** -- `sg tag -d`/`sg branch -d`/
    the create/`-f` write above are the only three O_EXCL-guarded call
    sites in this project; `switch`, `reset`, plain `tag` (create), and
    `commit` all still silently override a concurrent git process's lock.
    Recorded as the recommended NEXT phase in `docs/DESIGN.md`'s Phase 76
    residuals, deliberately NOT fixed here.
    **Phase 76 fix round 4: the alias probe's own lock-taking (round 3's
    L1 fix) leaked EMPTY directories** (`refs/heads/<name>/`) for any
    NONEXISTENT nested delete target, since it ran for every batch name
    regardless of existence -- fixed by making the probe a pure query
    (`sg_ref_lock_try_query`, no `mkdir`), which also fixed a
    misdiagnosed "sg: out of memory" for an ordinary not-found sibling
    shape (`-d heads/x/y`) and made round 3's own path-safety guard for
    `-d merged/` redundant (deleted). Round 4 also closed a real
    regression of SHIPPED `sg tag -d`: round 1's extraction held every
    acquired lock's fd open for the whole batch, exhausting the platform
    default `RLIMIT_NOFILE` (256 on macOS) past ~253 names -- fixed by
    closing the fd immediately after `O_CREAT|O_EXCL` succeeds (the lock
    IS the file's existence, not the descriptor). See `docs/DESIGN.md`'s
    Phase 76 round-4 section for the full measurements, the D1a residual
    (an externally-planted empty directory still defeats
    `sg_ref_update`, recorded alongside L2 above rather than fixed here),
    and the two smaller D1e disk-state/wording fixes for the NEW `-f`
    feature (a no-op force on a packed branch no longer materializes a
    loose file; a packed, case-folded D/F conflict now names the real
    stored ref and matches git's wording byte-for-byte).

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

- One independent unit test `.c` file per area (79 as of Phase 67), **no shared header, no test
  framework**. **Do not hardcode that count anywhere in this file again**: the
  Makefile globs `tests/*.c`, so it grows every phase that adds a test, and a
  stale number in the gate-reading instructions above is exactly the
  "misreading the gate" failure this file calls its worst. It said 64 while
  the real total was 72 (fixed in Phase 62), then 73 while it was 74 (Phase
  63), and 74 while it was 75 (Phase 64) -- three phases in a row. Read the `N/M` the gate itself prints and compare N against M. Each file carries its own `static int failures = 0;` and a
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
  **"Redundant guard" requires checking TWO things, not one** (Phase 76
  round 5, R4-1): the FINAL ANSWER staying the same after deleting the
  guard is necessary but not sufficient -- a guard can ALSO be the only
  thing standing between untrusted input and something the code touches
  ON THE WAY to that answer (a filesystem call, a lock, a subprocess, a
  network request), and "same final wording" says nothing about that.
  Measured: deleting a path-safety guard in `sg branch`'s alias probe
  left the printed message unchanged for `sg branch -d ../../evil` (still
  "not found"), which looked like exactly the "real defense line is one
  layer down" shape -- but the guard was ALSO the only check between the
  raw argv name and an `open(O_CREAT|O_EXCL)` call, and without it the
  probe built a path OUTSIDE the repository and briefly created a real
  file there before unlinking it. The fixture that catches this is NOT
  "plant something at the escape target" (an already-existing path of
  ANY kind returns `EEXIST` before any of this matters, so the answer is
  "not found" either way and the check cannot discriminate) -- it is
  making the escape target's PARENT directory read-only, so the escaped
  `open()` fails with `EACCES` instead of the ordinary "doesn't exist"
  errno, which a real gate running first would never reach at all. Before
  deleting a guard for being "redundant," measure with a fixture where
  the side effect itself would fail in a way that changes the OUTPUT --
  same-answer-on-the-happy-path is not proof of nothing left to prove.

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
- **Every delegation spec must name the `docs/RULES-*.md` for the scope it
  hands over** (the table under "Module layout" says which). An agent does not
  read this file's pointer table on its own, and the rules it needs are no
  longer inline here -- since Phase 71 an unnamed rules file is an unread one.
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
