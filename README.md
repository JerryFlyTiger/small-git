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
(`switch`, `restore`, `merge`, `rebase`, `stash push`) automatically create a
snapshot before running, stored under `refs/small-git/undo/`, listed with
`sg undo` and restored with `sg undo <number>`.
`--force` only skips the confirmation prompt, not the snapshot -- so even a
`--force`-all-the-way run can still be recovered.

**Large/binary files.** Built-in content-defined chunking (CDC): files over a
threshold are split into chunks and deduplicated, conceptually similar to Git LFS
but without needing an external server. **Disabled by default**; once enabled,
plain `git` will only see the pointer text, not the file content (see the
limitations section below).

**Performance on huge repos.** Object lookup switched to mmap plus a
process-level pack registry. On a repo with 811 commits and a 48MB pack, `sg log`
went from 2.64s to 0.008s, on par with `git log`; more importantly, the cost no
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
| `log` | Show commit history |
| `status` | Show working directory status |
| `diff` | Show unstaged changes |
| `switch` | Switch branch (`-c` creates a new branch, `--detach` points directly at a commit) |
| `branch` | List, create, or delete branches (`-d` deletes) |
| `restore` | Restore files or unstage them (`--staged`) |
| `undo` | List or restore automatic snapshots |
| `merge` | Merge another branch (`--abort` aborts it) |
| `merge-base` | Find the nearest common ancestor of two commits |
| `rebase` | Reapply onto another branch (`--continue`/`--skip`/`--abort`) |
| `stash` | Stash work-in-progress and return to a clean state (`push`/`list`/`apply`/`pop`/`drop`/`clear`) |
| `clone` | Clone a repository from a remote (smart HTTP) |
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
- commit, branch, switch, reset, merge, fetch, push, clone, stash all write a
  reflog entry compatible with real git; `git reflog` can read history produced
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
- Repos created by `sg` can be operated on directly with `git`, and pass
  `git fsck --strict`; repos created by `git` can likewise be operated on
  directly with `sg`, including a state where `git gc` has folded refs into
  `packed-refs` and packed objects into a pack.
- The network side implements smart HTTP and can interoperate with a real git
  server (clone/fetch/push).
- The test suite `tests/interop.sh` has 1320 checks, most of which feed `sg`'s
  output to a real `git` binary (including a local `git http-backend` server)
  to verify it, rather than comparing `sg` against itself.

**The one exception is once chunking is enabled** -- at that point the tree
contains pointer blobs, and `git checkout` will get the pointer text. This is
the same situation as opening a Git LFS repo in an environment without LFS
installed.

## Known limitations

Listed honestly, not as a to-do list:

- **Symlinks and submodules are not supported.**
- **`core.excludesFile` / the global ignore file is not read**; only
  per-directory `.gitignore` and `.git/info/exclude` are supported.
- **Cannot walk directory trees exceeding the platform `PATH_MAX`** (sg builds
  absolute paths, while git walks relatively via `openat()`). When this is
  hit, `sg add` reports a clear error and `sg status` prints a warning that
  the listing may be incomplete -- it is never silently skipped.
- **`~/.gitconfig` is not read**; commit identity can only be set via the
  `GIT_AUTHOR_NAME` / `GIT_AUTHOR_EMAIL` environment variables (default
  `small_git <sg@localhost>`).
- **The write path does not do delta compression**, each object is zlib
  compressed independently; the read path fully supports OFS_DELTA/REF_DELTA.
- **The write path does not produce packs larger than 2GB** (it fails with a
  clear error rather than producing a broken file); the read path supports
  them.
- **Once chunking is enabled, `refs/sg/chunks` cannot be deleted**; every
  chunk stays reachable in the object graph only through it, and deleting it
  lets `git gc` collect the data. `sg` detects this state and fails hard
  instead of silently writing out pointer text.
- **`sg stash` supports** `push`/`list`/`apply`/`pop`/`drop`/`clear`; `push`
  adds `-u`/`--include-untracked`, `-a`/`--all` (also collects ignored files),
  and `--keep-index` (resets the working directory to the index instead of
  HEAD); `apply`/`pop` add `--index` (after a clean merge, swap the index back
  to exactly what it looked like at push time). `-u`/`-a` store untracked
  files in the stash commit's third parent (a root commit containing only
  untracked files); the tracked half's tree is byte-for-byte identical to the
  case without those flags. `apply`/`pop` no longer require the entire working
  directory to be clean, only blocking dirty changes on paths that this merge
  actually touches -- paths already deleted in the working directory are not
  considered blockers. **Not supported**: `show` and pathspecs. Two places are
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
make                # normal build (with -g)
make test           # unit tests
bash tests/interop.sh   # interop test against real git (needs a prior make)
make sanitize       # build with ASan/UBSan and run tests

python3 tests/fuzz_ignore.py        # .gitignore consistency fuzzer (real git as oracle)
python3 tests/fuzz_ignore.py 1000   # run longer
```

`tests/fuzz_ignore.py` randomly generates pattern sets and directory trees, and
requires the untracked set from `sg status` to be **exactly equal** to
`git status --porcelain -uall`. It is not in `make test` (needs python3 and
real git, and takes longer), but should be run manually after touching
`src/workdir/ignore.c` or either traversal path.

Run `make clean` before switching build modes (normal / `release` /
`sanitize`) -- object files carry the flags they were compiled with, and make
only relinks when sources are newer, so without a clean build it will reuse
object files from the previous mode.

CI builds and runs the full test suite on Linux (gcc and clang) and on macOS,
and additionally runs a pass of ASan/UBSan.
