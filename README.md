# small_git (`sg`)

A version control tool implemented in pure C11, **fully compatible with git's object
format**. The same `.git` directory can be used interchangeably with `git` and
`sg` -- commits created by `sg` can be read by `git log` and pass `git fsck --strict`,
and vice versa.

The goal is not to build a teaching toy, but to offer concrete answers to four
common pain points in git. Design decisions and per-phase records are in
[docs/DESIGN.md](docs/DESIGN.md).

## Four pain points and how `sg` actually addresses them

**CLI/UX confusion.** `switch` (change branch) and `restore` (restore files) have
separate responsibilities, instead of being crammed together under `checkout`.
Mistyped commands get a suggestion (`sg stat` -> hints `status`). `status` states
directly what you can do next, instead of dumping a wall of jargon. `.gitignore`
is supported (per-directory rules, negation, `**`, character classes -- full
`gitignore(5)` semantics), `sg add .` recurses the whole directory tree, and
`sg branch` lists/creates/deletes branches.

**Destructive operations with no rescue path.** Operations that can lose data
automatically create a snapshot before running, stored under
`refs/small-git/undo/`, listed with `sg undo` and restored with
`sg undo <number>`. `--force` only skips the confirmation prompt, not the
snapshot -- so even a `--force`-all-the-way run can still be recovered.

The short answer to "will there be a snapshot" is **yes whenever the
operation actually touches your working tree or index** -- an ordinary
`stash push` with changes to stash, a `reset --hard` over real edits, and a
`restore` that overwrites something all leave an entry. The cases worth
spelling out are the ones where that rule is easy to guess wrong (every one
below was measured against this build by running it and then checking
`sg undo`):

- **Nothing to lose, no snapshot.** Starting from a clean tree, `switch`,
  `reset --hard`, `restore`, a `merge` that turns out to be a fast-forward,
  `sg undo` itself, and a `stash push` with nothing to stash all leave no
  entry -- there would be nothing to restore.
- **Could conflict, snapshot regardless.** A real three-way `merge`,
  `merge --abort`, `rebase` (including `--skip` and `--abort`),
  `cherry-pick`/`revert`'s `--skip` and `--abort`, and `reset` with no flag
  (i.e. `--mixed`) all snapshot even when they start from a clean tree,
  because a clean starting tree says nothing about the result.
- **Of the commands that can overwrite your work, the only one that never
  snapshots is `reset --soft`**, which moves `HEAD` and touches neither the
  index nor the working tree, so nothing uncommitted is ever at risk.

Two things are easy to assume the other way round. **The two fast-forwards
land on opposite sides**: a fast-forward `merge` on a clean tree leaves no
entry, a fast-forward `rebase` does. And **`sg undo` protects you from
itself** -- restoring a snapshot over a dirty working tree snapshots that
tree first, so an `sg undo` aimed at the wrong entry is itself undoable.

(Maintaining this list: it is exactly the call sites of `sg_snapshot_create`
and `sg_safe_apply_tree`. Grep for both when adding a command, and note that
matching on command *name* is not enough in either direction: `merge` has
three separate call sites that land on different bullets above, while
`stash pop`/`stash apply` call neither function at all -- they refuse a
conflicting apply up front via `sg_stash_apply_check_dirty` instead, so a
grep that finds `stash push`'s one call site says nothing about them.)

**Large/binary files.** Built-in content-defined chunking (CDC): files over a
threshold are split into chunks and deduplicated, conceptually similar to Git LFS
but without needing an external server. **Disabled by default**; once enabled,
plain `git` will only see the pointer text, not the file content (see the
limitations section below).

**Performance on huge repos.** Object lookup switched to mmap plus a
process-level pack registry. On a repo with 811 commits and a 48MB pack, `sg log`
went from 2.64s to 0.007s, on par with `git log`'s own 0.005s; more importantly, the cost no
longer scales with pack size (a 448KB repo and a 48MB repo are now equally fast).

## Installation

Requires `zlib`, `openssl`, `libcurl` (detected via pkg-config) and a C11
compiler.

```sh
make release                  # optimized build (-O2), produces build/sg
sudo make install              # install to /usr/local (includes the man page)
```

`PREFIX` and `DESTDIR` can be used to adjust the install location:

```sh
make install PREFIX=$HOME/.local
make install DESTDIR=/tmp/pkg PREFIX=/usr    # for packaging
make uninstall PREFIX=$HOME/.local
```

After installation, `man sg` has the full command reference.

**Set your commit identity before the first commit.** `sg` does not read
`~/.gitconfig` (see the limitations below), so without this every commit is
authored by `small_git <sg@localhost>`:

```sh
export GIT_AUTHOR_NAME="Your Name"
export GIT_AUTHOR_EMAIL="you@example.com"
export GIT_COMMITTER_NAME="$GIT_AUTHOR_NAME"
export GIT_COMMITTER_EMAIL="$GIT_AUTHOR_EMAIL"
```

**Set all four.** The committer pair is resolved independently of the author
pair and does *not* fall back to it -- setting only `GIT_AUTHOR_*` gives you
commits authored by you but committed by `small_git <sg@localhost>`
(measured, not assumed).

Supports **macOS** and **Linux**; Windows is not supported (the code uses POSIX
APIs directly).

## Quick start

```sh
sg init demo && cd demo

echo "hello" > a.txt
sg add a.txt
sg commit -m "first commit"
sg log
sg status

echo "changed" >> a.txt
sg diff                       # see unstaged changes
sg add a.txt && sg commit -m "second"

sg switch -c feature          # create and switch to a new branch
```

Here is what the rescue mechanism actually looks like in practice:

```sh
echo "important work" >> a.txt
sg restore a.txt --force      # oops, overwrote uncommitted content

sg undo                       # 1) 2026-08-04 23:17:52  restore a.txt
sg undo 1                     # content is back
```

The `.git` directory is standard git format from beginning to end, so it can
always be inspected directly with `git log`, `git fsck`.

## Command overview

| Command | Description |
|---|---|
| `init` | Create a new repository |
| `add` | Add a file or an entire directory to the index (`-f` force-adds ignored files) |
| `commit` | Create a commit |
| `log` | Show commit history (`--oneline`, `--graph`, `--pretty=`/`--format=`, `--date=`, `-p`/`--stat`, pathspecs) |
| `show` | Show commits, tags, trees or blobs (`-s`, `-p`, `--stat`, `--name-status`, same format flags as `log`) |
| `status` | Show working directory status |
| `diff` | Show changes -- working dir, `--cached`, or between two revs; six output formats (`--stat`/`--numstat`/`--shortstat`/`--name-only`/`--name-status`/patch), rename and copy detection (`-M`/`-C`), `--histogram` |
| `switch` | Switch branch (`-c` creates a new branch, `--detach` points directly at a commit) |
| `branch` | List, create, or delete branches (`-d`/`-D` deletes, `-f` force-moves) |
| `tag` | List, create (`-a`/`-m` for annotated), or delete (`-d`) tags |
| `restore` | Restore files or unstage them (`--staged`) |
| `reset` | Move a branch and optionally the index/working dir (`--soft`/`--mixed`/`--hard`) |
| `undo` | List or restore automatic snapshots |
| `merge` | Merge another branch (`--abort` aborts it) |
| `merge-base` | Find the nearest common ancestor of two commits |
| `rebase` | Reapply onto another branch (`--continue`/`--skip`/`--abort`) |
| `cherry-pick` | Apply the changes of existing commits (`-n`, `-m <parent>`, `--continue`/`--skip`/`--abort`/`--quit`) |
| `revert` | Revert existing commits (same flags as `cherry-pick`, plus `--no-edit`) |
| `stash` | Stash work-in-progress and return to a clean state (`push`/`list`/`show`/`apply`/`pop`/`drop`/`clear`) |
| `clone` | Clone a repository from a remote (smart HTTP or ssh) |
| `fetch` | Fetch new commits and refs from a remote |
| `push` | Push a local branch or tag to a remote (`--tags` pushes all tags) |
| `repack` | Pack loose objects into a packfile |
| `hash-object` | Compute (and optionally write) the hash of an object |
| `cat-file` | Inspect an object's content/type/size |
| `chunk-info` | Show diagnostic information about chunked storage |
| `reflog` | Show the update history of a ref (`show`/`<ref>`/`-n <count>`) |

`sg --version` shows the version, `sg --help` lists all commands.

## Compatibility with git

- Object format (blob/tree/commit), index v2, packfile, packed-refs are all
  standard formats, bit-for-bit compatible.
- commit, branch, switch, reset, merge, cherry-pick, revert, fetch, push,
  clone and stash all write a reflog entry compatible with real git; `git reflog` can read history produced
  by `sg` directly, and `sg reflog` can likewise read history produced by
  `git`. `<ref>@{N}` can be used in any command that accepts a revision (e.g.
  `sg reset master@{2}`).
  The reflog shape of `sg rebase` also matches real git: it replays entirely on
  a detached HEAD, `logs/HEAD` gets `rebase (start)` / one line per commit /
  `rebase (finish)`, while the branch's own log gains only one line no matter
  how many commits were replayed (Phase 18).
- **detached HEAD is a first-class state**: `sg switch --detach <rev>` enters
  it, `sg switch <branch>` leaves it; while detached, `commit`/`reset`/
  `branch`/`stash`/`log`/`status`/`merge`/`rebase` all work normally, and the
  status description (`HEAD detached at/from <id>`) matches real git verbatim.
  While detached, `merge` only moves `HEAD` and does not touch any branch; a
  `rebase` started from detached likewise never touches a branch and does not
  even write the `rebase (finish)` reflog line -- because there is no branch to
  move back (Phase 19).
- `reset`, `merge`, `rebase` and `stash push` write `ORIG_HEAD` the way git
  does, so `git reset --hard ORIG_HEAD` undoes an `sg` operation and vice
  versa.
- **Every ref write and delete takes git's own `<ref>.lock`**
  (`O_CREAT|O_EXCL`, then an atomic rename). A concurrent `git` process is
  locked out rather than silently overridden, and a stale lock left by a
  crashed process is reported with git's own "cannot lock ref" wording.
- Repos created by `sg` can be operated on directly with `git`, and pass
  `git fsck --strict`; repos created by `git` can likewise be operated on
  directly with `sg`, including a state where `git gc` has folded refs into
  `packed-refs` and packed objects into a pack.
- The network side implements both smart HTTP and ssh, and interoperates with
  a real git server (clone/fetch/push -- both transports cover all three).
  Over HTTPS, credentials come from `~/.netrc` or from `SG_USERNAME` /
  `SG_PASSWORD`; git's credential helpers (and therefore the macOS keychain)
  are not consulted. Over ssh, an `ssh` subprocess is spawned, so an existing
  key and `~/.ssh/config` work as they normally do.
- The test suite `tests/interop.sh` feeds `sg`'s output to a real `git` binary
  (including a local `git http-backend` server) to verify it, rather than
  comparing `sg` against itself. **The check count is deliberately not written
  here** -- it grows every phase, and a number in prose goes stale silently.
  `bash tests/interop.sh` prints its own `interop: N/M passed` line; that line
  is the count.

**The one exception is once chunking is enabled** -- at that point the tree
contains pointer blobs, and `git checkout` will get the pointer text. This is
the same situation as opening a Git LFS repo in an environment without LFS
installed.

## Known limitations

Listed honestly. Most of these are answers this project keeps rather than
work queued up. **The symlink-merge entry is the exception** -- it is a real
gap with a known wrong answer, and it says so.

- **Submodules are not supported.** A `160000` gitlink entry is recognised
  where it has to be (`cat-file` names it `commit`, and `push`'s reachability
  walk skips it instead of erroring), but there is no `.gitmodules` handling,
  no `sg submodule`, and checking out a tree that contains one is untested.
- **Symlinks are supported everywhere except in a three-way merge.** Reading
  them from the working tree, writing them back out (`switch`/`restore`/
  `reset --hard` create real symlinks, not regular files), and rendering them
  in diffs including typechange (`T`) all match git, and `sg add` refuses a
  pathspec that reaches through a symlink the way git does. What is *not*
  aligned is the merge engine: `sg_merge_trees` (`src/workdir/merge.c`) only
  looks at the mode when deciding whether two blobs are *equal*, so once a
  path is a conflict candidate it goes through a line-based text merge no
  matter what type the two sides are. Two symlinks with different targets are
  merged as if their targets were text, and a symlink pitted against a regular
  file does not produce git's `CONFLICT (distinct types)` rename.
  **This affects five commands, not just `sg merge`** -- `merge`,
  `cherry-pick`, `revert`, `rebase` and `stash apply`/`pop` all call that one
  engine.
  **One shape of it is a silent wrong answer, not a formatting difference.**
  Measured against this build, with base `a\nb\nc`, ours `X\nb\nc` and
  theirs `a\nb\nZ` as symlink targets (a newline is a legal byte in a
  target): `sg merge` prints `Merge made by`, `sg cherry-pick` prints
  `Successfully cherry-picked.`, both exit 0, and the link is left pointing at
  `X\nb\nZ` -- a target neither side ever had, committed without a word.
  Real git reports a conflict. **Until this is fixed, do not use any of those
  five commands across branches that change a symlink.** It is the next
  planned phase.
- **`core.excludesFile` / the global ignore file is not read**; only
  per-directory `.gitignore` and `.git/info/exclude` are supported.
- **Cannot walk directory trees exceeding the platform `PATH_MAX`** (sg builds
  absolute paths, while git walks relatively via `openat()`). When this is
  hit, `sg add` reports a clear error and `sg status` prints a warning that
  the listing may be incomplete -- it is never silently skipped.
- **`~/.gitconfig` is not read**; commit identity comes only from the
  `GIT_AUTHOR_NAME`/`GIT_AUTHOR_EMAIL`/`GIT_AUTHOR_DATE` and
  `GIT_COMMITTER_NAME`/`GIT_COMMITTER_EMAIL`/`GIT_COMMITTER_DATE` environment
  variables. The two triples are resolved independently -- neither falls back
  to the other -- and each unset one defaults to `small_git <sg@localhost>`.
- **The write path does not do delta compression**, each object is zlib
  compressed independently; the read path fully supports OFS_DELTA/REF_DELTA.
- **The write path does not produce packs larger than 2GB** (it fails with a
  clear error rather than producing a broken file); the read path supports
  them.
- **Once chunking is enabled, `refs/sg/chunks` cannot be deleted**; every
  chunk stays reachable in the object graph only through it, and deleting it
  lets `git gc` collect the data. `sg` detects this state and fails hard
  instead of silently writing out pointer text.
- **`sg stash` supports** `push`/`list`/`show`/`apply`/`pop`/`drop`/`clear`; `push`
  adds `-u`/`--include-untracked`, `-a`/`--all` (also collects ignored files),
  and `--keep-index` (resets the working directory to the index instead of
  HEAD); `apply`/`pop` add `--index` (after a clean merge, swap the index back
  to exactly what it looked like at push time). `-u`/`-a` store untracked
  files in the stash commit's third parent (a root commit containing only
  untracked files); the tracked half's tree is byte-for-byte identical to the
  case without those flags. `apply`/`pop` no longer require the entire working
  directory to be clean, only blocking dirty changes on paths that this merge
  actually touches -- paths already deleted in the working directory are not
  considered blockers. `push` also takes pathspecs, and `show` is implemented with `-p`/`--stat`/`--numstat`/`--shortstat`/`--name-only`/`--name-status`/`-u`/`--only-untracked`. Two places are
  deliberately different from real git: when `-u` collides with an existing
  file, sg rejects it all-or-nothing (real git applies partially, leaving an
  entry with no clean way out); when a dirty apply collides with an
  **already-staged** change, sg always rejects it (real git's "ours" is the
  index, which can be merged; sg's "ours" is HEAD, and allowing it would
  clobber the staged content). The format itself is fully compatible -- stash
  entries built by `sg` can be read by real `git` and vice versa, and
  `stash@{n}` uses the same byte-for-byte reflog as real git.
- **commit-graph and multi-pack-index are not implemented**. This was
  originally planned for Phase 7, but once the object access layer was fixed,
  `sg log` already matched `git log`'s performance, so the marginal benefit is
  low; revisit if there is an actual need.

## Development

```sh
bash tests/gates.sh       # the four gates that make up the completion standard,
                          # with a summary table and a path to each raw log
bash tests/gates.sh --rebuild    # clean first, so the warning count means something
bash tests/gates.sh --sanitize   # also run make sanitize (slow, cleans twice)
bash tests/gates.sh --leaks      # opt-in fifth gate: /usr/bin/leaks (macOS only)
```

The individual gates, if you want them one at a time:

```sh
make                    # normal build (with -g)
make test               # one binary per tests/*.c, any failure fails the whole thing
bash tests/interop.sh   # interop against real git (needs a prior make)
make sanitize           # clean + rebuild with ASan/UBSan, then run the unit tests
```

**Completion standard**: `make` + `make test` + `tests/interop.sh` all green.
Touching memory management, pack or chunk code additionally needs
`make sanitize`; touching ignore handling or directory traversal additionally
needs `fuzz_ignore.py`.

Prefer `tests/gates.sh` over running the four by hand -- not to save typing,
but because it reads every result with the same extraction rules. Three of
them are easy to misread by eye: "0 TUs recompiled" means *not measured*
rather than *zero warnings*; an `N/M ran` where N is smaller than M means the
run aborted partway, not that the rest passed; and a non-zero exit with no
FAIL lines (a crash, a timeout, an ASan abort) is still a failure.

### Differential fuzzers

These use real `git` as the oracle and are not part of `make test` (they need
python3 and a real git, and take longer):

```sh
python3 tests/fuzz_ignore.py            # .gitignore semantics vs git status --porcelain -uall
python3 tests/fuzz_ignore.py 1000       # each takes a round count
                                        # (default 200, except fuzz_combined.py: 150)
python3 tests/fuzz_diff.py              # patch output
python3 tests/fuzz_diff.py --histogram  # same, with --histogram on both sides
python3 tests/fuzz_merge.py             # three-way merge content
python3 tests/fuzz_rename.py            # rename/copy similarity scores
python3 tests/fuzz_merge_rename.py      # three-way merge when renames are involved
python3 tests/fuzz_combined.py          # combined diff output (-c / --cc)
```

`fuzz_ignore.py` generates random pattern sets and directory trees and
requires the untracked set from `sg status` to be **exactly equal** to
`git status --porcelain -uall`. Run it after touching `src/workdir/ignore.c`
or either traversal path. `tests/test_fuzz_pack.c` and
`tests/test_fuzz_index.c` fuzz the binary parsers instead and *are* in
`make test`; most of their discriminating power comes from the sanitizer
rather than from assertions, so run them under `make sanitize` when touching
the parsing paths.

### Mutation testing

`bash tests/mutate.sh <name> <file> <perl-expr> [<test-binary>|--interop]`
copies the tree into a scratch directory, applies the mutation to the copy,
rebuilds from clean and reports which named checks turned red. A new test is
not trusted until it has been shown to go red -- this project has had two
tests that could never fail.

### Build modes

Run `make clean` before switching between normal / `release` / `sanitize`:
object files do not record which flags they were built with, and make only
recompiles sources that changed. The same applies after editing any
`include/sg/*.h` -- there is no header dependency tracking, so a struct whose
definition changed leaves different `.o` files disagreeing about its layout,
and the symptom is a segfault somewhere unrelated.

### CI

Every push to every branch runs: a three-cell build matrix
(ubuntu x {gcc, clang} + macos x clang) with unit tests, interop, a release
build and an install/uninstall check into a staging dir; an ASan/UBSan job
that runs the unit tests, interop *and* the parser fuzzers with
`detect_leaks=1`; a gitignore conformance fuzzer job; and a packfile/index
parser fuzzer job.

**Three things cannot be checked locally on macOS**: gcc, interop under
ASan/UBSan, and leak detection (Apple's ASan has no LeakSanitizer and aborts
on `detect_leaks=1`; `gates.sh --leaks` is a coarse stand-in, not a
substitute). A green local board is not sufficient evidence.
