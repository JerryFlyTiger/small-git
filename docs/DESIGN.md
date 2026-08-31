# small_git design document

## Project positioning

A solid, usable git alternative (not a purely educational toy). Written in pure C, aiming to
solve the four categories of real-world pain points below while maintaining full compatibility
with git's object format, so that the `.git` directory can be read normally by real
`git`/`libgit2`/GitHub and other existing ecosystem tools.

Pain points to address, in priority order (development order, though they overlap):

1. **Confusing CLI/UX** -- checkout's multiple roles, a staging area that's hard to understand,
   unfriendly error messages
2. **Destructive operations and recovery** -- force push / reset --hard causing data loss, reflog
   being hard to discover
3. **Large/binary file handling** -- no built-in dedup, large files bloating the repo
4. **Performance on huge repos** -- status/checkout/clone slowing down on large monorepos

## Core engineering principles

- **Compatibility over innovation**: the object model (blob/tree/commit/tag), pack format, and
  index format are all git-compatible. New features are added with "compatible mode as default,
  advanced features opt-in", never breaking readability of an existing `.git` directory.
- **Data safety > performance > convenience**: any operation that could cause data loss must have
  a recovery path by default.
- **Base system libraries are allowed**: zlib (compression), OpenSSL EVP (SHA-1/SHA-256). Don't
  reinvent compression/hashing algorithms; spend engineering effort on architecture and UX
  differentiation.
- **Verifiable compatibility**: every milestone needs a round-trip test of "small_git writes, real
  git reads" and vice versa.

## High-level architecture

```
small_git/
  src/
    object/    serialization/parsing of blob / tree / commit / tag
    storage/   loose object read/write, packfile read/write, refs, index
    index/     staging area (.git/index) read/write and comparison
    workdir/   working tree scanning, status, checkout, diff
    cli/       command dispatch, porcelain command implementations
    safety/    automatic snapshots, undo/recovery, destructive-operation guards
    util/       hash (SHA-1 wrapper), zutil (zlib wrapper), path, mmap utilities
  include/sg/  public (cross-module) shared headers
  tests/       unit tests + interop tests (calling system git to verify)
  docs/        design documents, roadmap
```

No over-engineered library/CLI separation; start with a single `sg` executable, modularized
internally, and split off libsg later if needed.

## Design mapping for each pain point

### 1. Confusing CLI/UX
- From day one, split `checkout` into `sg switch` (switch branches) and `sg restore` (restore
  files), avoiding git's early multiple-role design.
- `sg status` prints clearer next-step suggestions by default (e.g. listing concrete commands
  rather than just file names on conflict).
- All commands that could be ambiguous must include "the command you probably meant" in their
  error messages.

### 2. Destructive operations and recovery
- Before destructive operations (reset --hard, branch -D, force-push-equivalent operations), a
  snapshot ref is automatically created at `refs/small-git/undo/<timestamp>`, with no manual
  action required from the user.
- `sg undo` is provided: lists recent automatic snapshots and can restore them with one command,
  more discoverable and easier to use than reflog.
- Force-push-type operations require confirmation by default, and list the commits that would be
  lost when they might overwrite non-fast-forward remote history.

### 3. Large/binary file handling
- Early (compatibility mode): large files are still stored as standard git blobs, correctness
  first.
- Advanced (opt-in): content-defined chunking (rolling hash) splits large blobs into chunks,
  referenced through a small pointer blob, transparently restored on checkout -- conceptually
  like a built-in Git LFS that needs no external server. Can optionally emit a format compatible
  with Git LFS pointers, interoperating with existing LFS repos.

### 4. Performance on huge repos
- Object access uses mmap, avoiding reading large files entirely into memory.
- Supports git's commit-graph file format to speed up history traversal such as log/merge-base.
- Later, support partial clone / sparse checkout (requires a smart protocol v2 client first).

## Phased roadmap

| Phase | Content | Status | Delivery verification |
|---|---|---|---|
| 0 | Project skeleton: build system, directory layout, test framework, CI | Done | `make && make test` runs; CI added in Phase 8 |
| 1 | Object model: SHA-1/zlib wrapper, loose object read/write, `sg init`/`hash-object`/`cat-file` | Done | Blobs written by small_git can be read by system `git cat-file -p`; and vice versa |
| 2 | Index + basic porcelain: `add`/`status`/`commit`/`log`/`diff`, refs, `switch`/`restore` | Done | For a repo created in small_git, `git log`/`git status` work normally |
| 3 | UX differentiation layer: friendly error messages, guided status, confirmation for destructive operations | Done | Manual UX walkthrough |
| 4 | Recovery: automatic snapshot refs, `sg undo`, merge/rebase and conflict UX | Done | 100% recoverable after simulated mistakes |
| 5 | Packfile and network interop: pack reader/writer, smart HTTP client (clone/fetch/push) | Done | Can clone a real repo, can push to it |
| 6 | Large-file chunking (opt-in feature) | Done (disabled by default) | Compare repo size and checkout time for large files |
| 7 | Huge-repo performance | Partially done | See Phase 7a below; **what was actually done differs from the original plan**, explained below |
| 8 | Docs, packaging, cross-platform wrap-up | Done | README, man page, `make release`/`install`; CI set up to build and run the full test suite on Linux (gcc/clang) and macOS |
| 9 | Usability completion: `.gitignore`, recursive `sg add`, `sg branch` | Done | See Phase 9 below; zero divergence over 600 random fuzz runs against real git as oracle |
| 12 | revision parsing, `sg tag`, `sg reset`, message normalization, loose decompression cap | Done | See Phase 12 below; interop grew from 382 to 642 checks, all against real git as oracle |
| 13 | `sg push` pushing tags (`--tags`, name lookup checks both heads/tags) | Done | See Phase 13 below; interop grew from 642 to 680 checks, against real git as oracle |

Phase 7's content differs from the original plan; the reason is recorded here to avoid future
misunderstanding: the original plan listed commit-graph, multi-pack-index, and parallelization.
Measurements found the real bottleneck was elsewhere entirely -- object lookup re-read the whole
pack on every query (see Phase 7a below). Once fixed, `sg log` was already on par with `git log`,
so commit-graph's marginal benefit dropped sharply and it was not implemented, left for evaluation
if there is real demand later. None of the three original items has been done.

Also, Phase 5 originally listed "delta compression", but the writer side still zlib-compresses
each object independently with no delta, to this day; the reader side fully supports
OFS_DELTA/REF_DELTA. The table has removed this description that was never delivered.


## Phase 9: usability completion (`.gitignore`, recursive `sg add`, `sg branch`)

After the first eight phases, `sg` still wasn't usable on real projects: `status` would list
`node_modules`, build artifacts and everything else; `add` only accepted a single file path; and
there was **no command at all to list branches**. Phase 9 fills these three gaps.

### Approach and verification

- **`.gitignore` engine** (`src/workdir/ignore.c`): full `gitignore(5)` semantics -- per-directory
  rule stacking (deeper overrides shallower), `.git/info/exclude` (lower priority), negation,
  anchoring, directory-only matches, the three forms of `**`, character classes, escaping, and
  trailing-whitespace handling. **All 21 semantic rules were confirmed against real git
  measurements before being written into the spec**, not from memory.
- **Recursive `sg add`**: `sg add .` walks the whole tree, skips `.git` (at any depth), prunes
  ignored directories, tracked files are unaffected by ignore rules, explicitly naming an ignored
  file requires `-f`, deletions are staged, and the existing all-or-nothing index write is kept.
- **`sg branch`**: list (merging loose and packed-refs, loose takes priority), create, delete.
  Deletion **must clear both the loose file and the packed-refs line** -- deleting only the loose
  file would let a stale packed entry resurrect the branch at an old commit.

### Verification method

Beyond 377 interop checks and clean ASan/UBSan runs, the key consistency verification is
**random fuzz testing with real git as the oracle**: 600 randomly generated pattern sets and
directory trees, comparing `sg status`'s untracked set against
`git status --porcelain -uall` for exact equality -- **600 runs, zero divergence**. This covers
the matcher's combinatorial edge cases far better than case-by-case hand-written tests.

### Defects found and fixed along the way

**`sg add .` could silently drop files.** The new traversal code treated every `lstat` failure as
"the file was deleted mid-walk" and skipped it, but `lstat` can also fail with `ENAMETOOLONG` (sg
builds absolute paths, and a deep tree can exceed the platform `PATH_MAX`), `EACCES`, or `ELOOP`.
The result was `sg add .` returning 0 with nothing staged, while `git add .` staged normally --
the commit would silently lose content. Now only `ENOENT` is treated as benign; everything else is
reported explicitly, and `sg status` prints a warning that the listing may be incomplete.

Worth recording: **the first version of the regression test was empty**. It built a tree "nested
until mkdir fails", but the failure point landed in `opendir` (which already had correct error
handling) and never reached the problematic `lstat` -- reverting the fix still passed the test.
After switching to `chdir` + relative paths to build a deterministic boundary where "the directory
can be opened, but an entry inside it has an absolute path that's too long", the control group
with the fix reverted actually FAILed. **A test must first prove it can fail before its passing
can be trusted.**

### Known limitations

- `core.excludesFile` and the global ignore file are not supported.
- Cannot traverse directory trees whose absolute path exceeds the platform `PATH_MAX` (git's
  relative traversal via `openat()` is not limited this way). This is a pre-existing limitation,
  not introduced by this phase; it now at least reports an explicit error instead of silently
  skipping.
- Character classes containing `/` (e.g. `a[x/]b`) and POSIX named classes (`[[:alpha:]]`) are not
  implemented.

## Supported platforms

- **macOS**: actually built locally and run through the full test suite (once each for release and
  ASan/UBSan builds).
- **Linux**: CI is set up to build and run the full test suite with gcc and clang. There is no
  Linux development machine, so the Linux build result **is taken on CI's word**, not a locally
  verified conclusion.
- **Windows is not supported.** The code uses POSIX APIs directly (`mmap`, `opendir`, `fcntl` file
  locks, POSIX path separators); supporting Windows would need a compatibility layer, out of scope
  for now.
- The two platforms need different feature-test macros: `-std=c11` is strict ISO C, and glibc
  hides `strdup`, `strtok_r`, `mkstemp`, `getcwd`, and `struct stat`'s `st_mtim` behind
  `_POSIX_C_SOURCE`; Darwin exposes these by default, and conversely **defining**
  `_POSIX_C_SOURCE` hides `st_mtimespec`. So the Makefile defines `-D_POSIX_C_SOURCE=200809L` and
  `-D_DARWIN_C_SOURCE` separately based on `uname -s`; the two are not interchangeable.

## Open technical decisions (confirmed as development proceeds)

- Hash algorithm defaults to SHA-1 (compatible with git's legacy format); SHA-256 repo support is
  deferred past Phase 5, to be evaluated as needed.
- Build system is Makefile (keeping the "small" spirit), no CMake unless a more complex
  cross-platform matrix is needed in the future.
- Test framework: a lightweight homegrown assert-based test runner, avoiding a dependency on a
  large test framework.

## Phase 6: large-file content-defined chunking (CDC) -- known limitations

Chunking is **disabled by default** (`[sg] chunking` in `.git/config`). When not enabled, the
repo's compatibility with git is entirely unchanged.

Trade-offs and known limitations once enabled:

- **Real `git` cannot read the content.** The tree holds a pointer blob, and `git checkout` gets
  the pointer text instead of the file content. This matches Git LFS's behavior in an environment
  without LFS installed, and is a cost accepted by whoever enables the feature. `sg`-to-`sg`
  (clone/fetch/push) works fully normally.
- **`refs/sg/chunks` must not be deleted.** All chunks stay reachable in git's object graph through
  this keep-alive ref; otherwise `git gc` would clean them up. After it's deleted, `sg` hard-fails
  on each chunked file (it does not silently write out pointer text), but the data may already be
  unrecoverable.
- **Concurrency protection is single-machine only.** `keep_alive_add` uses a `.git/sg-chunks.lock`
  file lock to serialize concurrent `sg add` within the same repo. Sharing a filesystem across
  machines is untested.
- **Push atomicity depends on the server.** `sg push` requests the `atomic` capability when the
  remote advertises it, binding the branch ref and `refs/sg/chunks` into a single transaction.
  When the server doesn't support it, a partial application -- "branch update succeeded,
  keep-alive ref rejected" -- is theoretically still possible; `sg push` will then report an
  explicit error (not a silent success), but before the user retries, if the remote happens to run
  `git gc`, the new chunk could still be cleaned up.

## Phase 7a: object access layer -- changes, measurements, and known limitations

### The original problem

`pack_read_depth()` did an `opendir` on `objects/pack/` for **every single object** queried, and
read the entire `.idx` and entire `.pack` into a malloc'd buffer, discarding it after use. The
cost of a single query was therefore proportional to **the entire repo's pack size**, making one
history walk O(number of objects x pack size).

There was also a hard defect: `idx_find()` gave up outright whenever an offset table entry had its
MSB set (idx v2 uses this to mean "this is an index into the large-offset table"), meaning
**packs over 2GB could not be read at all**.

### Changes

1. `.idx` / `.pack` are now `mmap`ed read-only instead of being copied whole into the heap.
2. Added a process-lifetime pack registry (keyed by `git_dir`), so each pack is opened and parsed
   only once.
3. `idx_find()` now uses the fanout table to narrow the binary search range down to `id[0]`'s
   bucket first.
4. The reader now supports idx v2's 64-bit large-offset table (the writer still refuses to produce
   packs >2GB).

### Measurements

Baseline repo: 811 commits, 48MB pack, warm page cache, compared on the same machine.

| | Before | After | System `git` |
|---|---|---|---|
| `sg log` (walking all 811 commits) | 2.64s | 0.007s | 0.005s |
| `sg status` | 0.27s | 0.03s | 0.03s |

`sg log`'s output is **byte-for-byte identical** before and after the change (verified once each
on three baseline repos of different sizes).

### Two defects found and fixed during review and measurement

- **Use-after-free caused by reentrancy.** When `read_entry_at()` resolves a REF_DELTA, it
  recurses back into `pack_read_depth()`; if that nested query misses and the pack directory's
  mtime changed at that moment (e.g. a concurrent `git gc`), it triggers a rescan, and the rescan
  frees the linked list node the outer `pack_read_from_dir()` loop **is currently traversing**.
  Reliably reproduced under ASan (at iteration 127) using a hand-crafted "REF_DELTA whose base is
  missing" object together with a concurrent toucher; the fix defers reclaiming a replaced list
  until the outermost read returns, and no longer reproduces after 1200 runs under the same
  conditions.
- **Staleness window from second-granularity mtime.** Whether a rescan is needed is decided by
  directory mtime, and `st_mtime` only has second-level resolution: if another process writes a
  new pack within **the same second** as our scan, the mtime comparison would be equal and `sg`
  would report an object that actually exists as missing -- a regression relative to the older
  "rescan on every query" implementation. The fix follows git's approach to racily-clean handling:
  if the mtime recorded at scan time is the current second, that scan's result is treated as
  untrustworthy and a rescan always happens on a miss. Verified with a test using a synthesized
  large delta (stretching the read window to about 100ms) alongside publishing the base pack
  within the same second: 5/5 hits after the fix, and 3 of 4 valid runs missed it with the guard
  disabled as the control.

### Known limitations

- **The pack registry has no cap.** A process keeps every pack it has queried, for every
  `git_dir`, mapped until it exits. No impact on short-lived CLI commands; a future long-running
  daemon would need an LRU and eviction.
- **No delta base cache yet.** Every level of a deep delta chain re-decompresses its base, making
  a chain of depth N cost O(N^2). git handles this with `core.deltaBaseCacheLimit` (default
  96MiB). Current measurements show this is not the primary bottleneck, but it will surface doing
  a large number of checkouts on an `--aggressive`-packed repo.
- **commit-graph and multi-pack-index are not implemented.** The original roadmap listed them in
  Phase 7; measurements show that once the object access layer was fixed, `sg log` was already on
  par with `git log`, so commit-graph's marginal benefit dropped sharply -- left for evaluation
  if there is real demand (e.g. `--graph`, heavy merge-base queries).
- **The writer still doesn't produce packs >2GB.** Reading already supports large-offset; writing
  reports an explicit error rather than producing a broken file when it would be needed.

## Phase 10: fuzzing the binary parsers -- attack surface, findings, and known limitations

`sg` parses two classes of **untrusted binary input**: packfiles and `.idx` received over the
network, and index v2 on disk. In the past, hand-crafting a malicious pack for this project always
turned up a real bug (Phase 5a's REF_DELTA cycle causing a stack overflow, Phase 7a's missing-base
REF_DELTA causing a use-after-free), so this was systematized into a standing fuzz harness.

Added `tests/test_fuzz_pack.c` and `tests/test_fuzz_index.c`, following the existing test
convention (no framework, `CHECK` macro, dropped into `tests/` and auto-collected by
`Makefile:48`).

### Why the shape of `tests/fuzz_ignore.py` couldn't be copied

`fuzz_ignore.py` is a **differential test**: it uses real `git` as oracle, comparing semantic
divergence. What a binary parser needs to catch is **memory-safety issues**, and there's no
natural oracle here -- feeding the same corrupted data to real git, git will just say "index
corrupt" and won't tell you whether `sg` went out of bounds. So these two are instead in-process C
harnesses, and their discriminating power comes from the sanitizer, not comparison. CI wiring is
therefore different too: the `fuzz-ignore` job is plain `make`, while where these two actually have
teeth is the `sanitizers` job (see "Which fixes actually have regression protection" below).

### Two design constraints that determine success or failure

1. **The trailer SHA-1 must be recomputed after corrupting bytes.** `sg_index_read` validates the
   whole-file checksum **before** parsing any entry, and pack does the same. Pure random bit-flips
   would get stuck on the checksum 100% of the time and never reach the parsing logic below --
   that would be a test that "looks like fuzzing but tests nothing."
2. **Each round must use a brand-new `mkdtemp` git_dir.** The pack registry is keyed by `git_dir`
   and decides whether to rescan by directory mtime, and `st_mtime` only has second-level
   resolution; swapping pack content repeatedly under the same git_dir within one second would
   read stale cache and give the test a false green.

### Defects found and fixed

| # | Location | Problem |
|---|---|---|
| 1 | Three error paths in `index.c` | The whole index file's `buf` was not freed |
| 2 | The `padded_len` check in `index.c` | `free_entries(entries, i)`'s upper bound excluded the current entry, leaking the just-malloc'd path |
| 3 | `nentries` in `index.c` | An attacker-controlled u32 used directly as a malloc size, with no cap |
| 4 | `pack_inflate` | `strm.avail_out = (uInt)expected_len` truncated a 64-bit size varint to 32-bit |
| 5 | `read_entry_at` and four other sites | Malloc'd the declared decompressed size without checking it against the available compressed byte count |
| 6 | `sg_pack_delta_apply` | The target size is a separate varint inside the **already-decompressed delta stream**, not governed by the check in 5 |

Defects 1 and 2 survived as long as they did because CI's ASan globally disabled leak detection to
tolerate two process-lifetime caches -- effectively a big hole in leak detection. This hole was
closed in Phase 11 (see below).

Defect 4 is the only memory-**content** safety problem (the rest are resource exhaustion): with a
declared size of `2^32+5`, `avail_out` was truncated to 5, and as soon as the compressed stream
happened to decompress exactly 5 bytes, zlib reported success, while the caller used the
untruncated `size` -- returning an object "declared as 4 GB, with only 5 bytes actually valid and
the rest uninitialized memory." The fix feeds zlib in chunks and adds an independent guard that
declares a stall if this round's `total_in`/`total_out` didn't move -- this loop handles untrusted
input, and a hang is a DoS, which should not be outsourced to trust in zlib's implicit contract.

### Measured compression-ratio bound

The fixes for defects 5 and 6 are both plausibility checks of "declared size vs. available byte
count." On the pack side this uses DEFLATE's theoretical maximum compression ratio. The
theoretical hard limit is **1032:1** (258 bytes / 2 bits), but measurements show real zlib reaches
**1026.8:1** on an 8 MB all-zero file (`git verify-pack -v` measures size=8000000 /
packed=7791) -- only **0.45%** short of the limit -- and the **last** object in a pack has
available byte count nearly equal to its own compressed size, which is the tightest case. 1032 is
mathematically safe, but this margin leaves no engineering slack, so the constant was widened to
`1032 * 4`. This doesn't undermine the check's purpose: it's meant to catch values off by orders
of magnitude (e.g. declaring 4 GB with only 33 bytes available).

`sg_pack_delta_apply` can't use the compression ratio, because the target size isn't the result of
decompressing a compressed stream -- it's rebuilt from copy/insert instructions. The bound is
instead derived from the instruction stream: a single copy opcode byte can bring out at most 65536
bytes, so the target size can't exceed "remaining delta bytes x 65536."

### Which fixes actually have regression protection (mutation-verified)

Each fix was broken one at a time to see whether the corresponding test goes red. **This step was
run by the primary conversation, not left to whoever wrote the test**:

| Broken location | Normal build | ASan build |
|---|---|---|
| Removing `free(buf)` on the error path | **red** | -- |
| Reverting `free_entries(i + 1)` to `(i)` | **red** | -- |
| Removing the `nentries` cap | green | green |
| Making `decompressed_size_is_plausible` always true | green | **red** (19 TB allocation request) |
| Removing the delta target bound | green | **red** (545 TB allocation request) |
| Reverting `pack_inflate`'s chunking | **red** | -- |
| Removing both guards in `pack_inflate` at once | **red** (watchdog) | -- |

The last row was originally a **hang** rather than a red test -- `fork()` + `alarm()` only
protects a single call site, and other call sites in the same binary would still hang
indefinitely, while GitHub Actions jobs default to a 6-hour timeout. So both fuzzers' `main()` got
an overall watchdog (`SG_FUZZ_TIMEOUT`, default 600 seconds) that turns a hang at any location into
a non-zero exit with diagnostics.

Warning: **`make clean` every time when doing mutation verification.** The first run through this
table produced the strange result of "test still red after reverting the fix" -- tracked down to
an incremental rebuild not correctly reflecting the revert (object files don't record which
version they were compiled from, see `Makefile:76-80`); after a clean rebuild both directions
behaved correctly. This nearly got misread as test flakiness caused by leftover build state.

Key conclusion: for hardening like "reject absurd sizes", **the return value is identical to the
unhardened version** (both are -1, only the mechanism changes from an explicit rejection to a
malloc failure), so no difference is observable through the public API at all. Their discriminating
power **exists only under ASan** -- and what catches them is a random fuzz round, not a
hand-constructed deterministic test. This is why a long campaign was added to the `sanitizers`
job: it is the only real gatekeeper for this batch of hardening.

### Known limitations

- **No test can distinguish the `nentries` cap.** `0xFFFFFFFF x sizeof(sg_index_entry)` is about
  344 GB, which is within ASan's 1 TB allocation limit and won't trigger
  allocation-size-too-big; under a normal build a malloc failure also just returns -1. This check
  is defense-in-depth, and its corresponding test is a smoke test, not a discriminating one --
  already stated plainly in the test's comment, not pretending it guards anything.
- **macOS has no LeakSanitizer support at all** (passing `detect_leaks=1` aborts immediately, not
  "just disabled by default"). Leak detection therefore relies on `test_index_fail_path_leak`'s
  RSS probe, and ASan's allocator quarantine holds onto freed chunks and inflates RSS causing
  false positives, so the probe disables itself under an ASan build. The result is that leak
  detection and memory-safety detection live in different CI jobs, and you can't look at just one.
  **The more fundamental problem is that CI globally disabled `detect_leaks`, so aside from that
  one index failure path, this project has had no automated leak detection at all until now** --
  that's why defects 1 and 2 survived so long. The original reason for disabling it was "the pack
  registry and chunk keepalive are process-lifetime caches deliberately never freed, they'd be
  falsely flagged," but that premise **is probably false**: both hang off file-scope global
  pointers (`pack.c:498`, `chunk.c:744`), and LeakSanitizer's reachability analysis treats global
  variables as roots, so memory that's still reachable is never classified as a leak. In other
  words, turning on `detect_leaks=1` directly on Linux CI has a good chance of just passing, with
  no suppression file needed at all. This is something a single CI run can verify, and cannot be
  tested locally (macOS).
- **Only the pack/idx/index three parsers are covered.** `sg_decompress`
  (`src/util/zutil.c`, the loose object decompression path) grows unbounded via "double the
  capacity whenever full," with no output-length cap -- the same class of zip-bomb attack surface,
  but out of scope this time.

## Phase 11: turning CI's LeakSanitizer back on

The top-priority follow-up left by Phase 10. The change is semantically one line:
`.github/workflows/ci.yml`'s ASan job changed `ASAN_OPTIONS=detect_leaks=0` to `detect_leaks=1` in
three places.

### The original reason for disabling it was wrong

The original claim was "the mmap pack registry at `pack.c:498` and the chunk keepalive cache at
`chunk.c:744` are process-lifetime caches deliberately never freed, and would be falsely flagged
if enabled." This premise doesn't hold: both are **file-scope global variables**, and
LeakSanitizer's reachability analysis treats globals as roots -- memory that's still-reachable is
never classified as a leak. Measurement confirmed this -- `detect_leaks=1` passed outright on
Linux CI, all three matrix cells plus the ASan job went green, **with no suppression file
needed at all**.

The cost was real: the two `index.c` leaks in defects 1 and 2 survived until Phase 10 only because
`test_fuzz_index` measured peak RSS -- a far blunter tool than LSan.

### A green light isn't evidence: three measured leak plantings

"Still green after turning it on" has two explanations: no leak, or LSan isn't actually running.
Without telling them apart, this gate doesn't really exist. So deliberate leaks were planted and
sent to CI before merging; the results are worth recording:

| Planting shape | CI result | Reading |
|---|---|---|
| 1 block of 1234 B, pointer stored in a local variable of `sg_cli_run` then set to NULL | **green** (382/382 passed) | false negative |
| 100 blocks of 1234 B, same site in a loop | red, `detected memory leaks`, interop 200/382 | LSan is alive |
| 1 block of 1234 B, allocated inside a helper that returns | red, `Direct leak of 1234 byte(s) in 1 object(s)` | a single leak gets caught |

The first row's false negative isn't a flaw in LSan, it's a **planting-location** problem: that
pointer overflowed into `sg_cli_run`'s own stack frame, and that frame is a live ancestor frame for
the whole duration of the command's execution -- the conservative scanner treats it as a root, so
that memory stays "reachable" the whole time. A real leak comes from a **function that returns**,
whose frame is then overwritten by later, deeper calls -- the third row proves that shape gets
caught with just a single block.

Generalized lesson: **when verifying a conservative leak scanner, don't plant in a frame that
survives to process exit**, or what gets proven is that the tool isn't broken, not that the gate
is effective.

### How to pre-verify locally on macOS

Apple's ASan doesn't implement LSan; `detect_leaks=1` aborts immediately, so it can't be tested
locally. The substitute is the system's built-in `leaks(1)`:

```bash
MallocStackLogging=1 leaks --atExit -- build/tests/test_foo
MallocStackLogging=1 leaks --atExit -- build/sg status
```

It prints `N leaks for M total leaked bytes`. It was run across 22 test binaries and 20 kinds of
CLI invocations, all 0 leaks -- this was the confidence source before pushing to CI. The same
discipline applies: first confirm with a small program with a deliberate leak that `leaks` goes
red (it prints a "Process is not debuggable" warning, but detection still works normally), then
trust those zeros.

### Known limitations

- Coverage equals the paths the ASan job actually runs: `make sanitize`'s unit tests +
  `interop.sh` + the two parser fuzzers. Code not exercised by these has no leak detection either.
- The nature of conservative scanning: memory that's still reachable doesn't count as a leak.
  Deliberately retained caches are therefore immune, but so are real leaks where "something that
  should have been freed happens to still be pointed to by some global or live frame."
- This is a Linux-only gate. The macOS CI cell doesn't run ASan, and it can't be reproduced
  locally either.


## Phase 12: `sg tag`, `sg reset`, and two bit-compatibility gaps

Added `sg_rev_parse_commit` (`include/sg/revparse.h`) -- a small rev-syntax subset covering
`HEAD`/branch/tag/40-hex plus `~N`/`^N` suffixes -- and `sg tag`: a lightweight tag is just
`refs/tags/<name>` pointing directly at a commit; `-a`/`-m` annotated tags additionally write a
`SG_OBJ_TAG` object, with the ref pointing at it instead. `-m` without `-a` implies annotated,
matching real git (confirmed by measurement against real git). Shares
`sg_ref_list_under`/`sg_ref_delete_under` (loose+packed merge/cleanup) and
`sg_ref_name_valid_for_create` with `sg branch`.

### Known limitation: `sg push` doesn't push tags (**lifted in Phase 13, see below**)

`sg push` was hardcoded to push only `refs/heads/` -- regardless of whether there were `sg tag` or
real-git-created tags locally, `sg push` would not transmit them to the remote, nor would it error
or warn; a silent scope limitation. Because `sg` is bit-compatible with real git, the user could
work around it with real git's `git push --tags` on the same repo to push the tags separately.
This section is kept because Phase 13's implementation scope was defined exactly by it; the
current state is as of Phase 13.

### `sg reset`: three modes, three different safety gates

`--soft` only moves the branch pointer; `--mixed` (default) additionally rewrites the index;
`--hard` additionally touches the working directory. The target revision goes through
`sg_rev_parse_commit` above.

**Not all three modes are stuffed into `sg_safe_apply_tree`**, even though that's the existing
destructive-operation skeleton. Real git doesn't require confirmation for `--soft`/`--mixed` --
they never overwrite files at all -- routing everything through one path would be more annoying
than the tool it's imitating. The compromise: `--hard` gets full confirmation plus a snapshot;
`--mixed` snapshots automatically but doesn't interrupt; `--soft` gets neither.

`--mixed` needs a path for "rewrite the index, don't touch the working directory", which
`sg_apply_tree_to_workdir` can't express -- it relies on `stat()`ing the file it just wrote to
fill the index entry's stat fields. Without writing a file, that source doesn't exist, and
**reusing the old entry's stat is worse than it looks**: the index would claim the file is
up to date while recording a different blob, and real git trusts stat as a shortcut, so it would
report clean for a file whose content has actually diverged. `sg_index_reset_to_tree` therefore
only carries over the stat fields when the sha is unchanged, and zeroes them out otherwise.

### Two bit-compatibility gaps

**Messages weren't normalized.** `sg commit -m "x"` wrote the string verbatim into the object,
while real git first applies the default `--cleanup=whitespace`. The same logical commit computes
different object ids on the two sides: git's message section ends with a newline, sg's doesn't.
git can read sg's objects fine and `fsck` is clean, so nothing was broken -- but "the same commit
hashes identically under both tools" is exactly this project's claim, and the 382 interop checks
had never compared the message section byte-for-byte.

The rule was determined by measurement, not recall: strip trailing whitespace per line, collapse
consecutive blank lines into one, drop leading and trailing blank lines, end with exactly one
newline, keep leading whitespace on each line. Only the trailing-newline rule was obvious --
fixing just that alone would trade one divergence for four. **`\v` and `\f` don't count as
whitespace** (real git keeps them), `\r` does. The first version stripped those two characters
too; it was only caught during review by comparing against real git.

`sg_message_cleanup` sits next to the serialization functions, but is **deliberately not called
from within them**: `cmd_rebase.c` forwards an existing commit's message, and real git's
`--cleanup=verbatim` can produce a message with no trailing newline -- rebase must preserve it
byte-for-byte. It's applied by whichever caller owns the message -- commit, tag, merge, snapshot,
chunk.

Worth noting a real-git asymmetry, counter-intuitive enough to write down so it doesn't get
"helpfully unified" later: **`git commit -m ""` aborts, but `git tag -a -m ""` accepts it** and
produces a tag with an empty message section.

**Loose object decompression had no cap.** `sg_decompress` doubled the buffer whenever it filled
up, with no ceiling, so a loose object file that decompresses to several GB would be fully
decompressed before anyone got to look at it. Phase 10 recorded this as the remaining zip-bomb
surface.

The cap isn't a magic number: `sg_loose_read` first decompresses only the leading 64 bytes,
parses out the `"<type> <size>\0"` header from that, then uses `header_len + declared_size` as a
hard ceiling for the actual decompression -- git's own shape. zutil stays generic
(`sg_decompress_bounded` takes the ceiling as a parameter and knows nothing about objects); the
two-stage logic lives in `loose.c`, which is entitled to know the format.

The same round fixed a truncation of the same family as Phase 10's defect 4: `avail_in` let a
`size_t` pass through a 32-bit `uInt`, so a loose file over 4 GB would feed zlib the wrong length.
Phase 10 fixed the pack side of this and missed this one.

### Caught during review: a fake merge parent

`sg reset` didn't clear `.git/MERGE_HEAD`. Using bare `sg reset` to abandon a conflicted merge,
stage a fix, and commit normally -- the resulting commit ended up with **two parents**, the second
pointing at the merge target that had just been abandoned, with no warning anywhere along the way.
Only `--hard` cleaned it up, because it goes through `sg_safe_apply_tree`; the soft and mixed
paths never called `sg_merge_head_remove`.

The matrix was measured across three modes x two in-progress states, not guessed: real git
**flatly rejects** `--soft` during a merge or a paused rebase, `--mixed` clears `MERGE_HEAD`, and
**none of the three modes touch rebase state**. sg is now consistent with this, with the sole
exception that `--hard` still clears rebase state via `sg_safe_apply_tree` -- that path is shared
with `switch` and `undo`, and fixing it belongs to a different change, recorded in the BUGS
section of `docs/sg.1`.

None of the 614 checks could see this: **no test had ever called `sg reset` while a merge was in
progress**. The new check asserts the end state a user would notice -- the following commit has
only one parent.

### Three testing lessons from this phase

**Priority ordering was wrong twice at the same spot, in opposite directions.** First, which wins
between tag and branch (gitrevisions says tag wins), then where a full 40-hex sits (it beats any
ref -- a branch that happens to be named the same as another commit's hex gets shadowed by the
literal value). Both times the judgment was made from memory of gitrevisions, and both times it
was wrong. The comment now states "which order was confirmed by measurement."

**A broken mutation can give a prettier-looking red.** When verifying hex priority, the first
attempt broke the ordering of blocks in `resolve_base`, and both tests went red -- one more than
the correct mutation. Redone as a clean mutation, only the target test went red -- that's the
valid evidence.

**Both conservative leak scanning and the stat cache can make "no red" meaningless.** When
proving CI's LeakSanitizer was still alive, a single leak planted in a frame that survives to
process exit wasn't reported (see Phase 11); and `--mixed`'s stat gate is masked by git's
racily-clean rule when the file's mtime and the index share the same second -- every other check
writes the file and the index within the same second, so the whole test suite couldn't see it.
Only pushing the mtime into the past with `touch -t` made it visible.

### Known limitations

- **No abbreviated sha.** `sg_rev_parse_commit` only accepts a full 40-character hex; prefix
  matching would need to scan both loose objects and every pack, a different order of magnitude
  of work -- the header comment states this is deliberate.
- **`sg reset` doesn't accept a pathspec.** `sg restore --staged` is already a synonym for
  `git reset --mixed <path>`; forcing it in would create a third overlapping piece of logic.
- **Detached HEAD is always rejected.** sg never writes a bare sha into HEAD; this primitive
  doesn't exist anywhere in the project. `cmd_push.c` and `cmd_rebase.c` already follow the
  existing convention of "reject on detection."
- **`sg reset --soft --hard` reports a usage error**, while real git accepts it and lets the last
  flag win. Deliberately converged the other way: an ambiguous invocation is rejected rather than
  guessed. Written into the man page so it isn't mistaken for a bug and "fixed" later.
- **The fix for the 4 GB truncation has no test coverage.** Observing the difference requires
  actually allocating over 1 GB, which conflicts with the project's existing constraint that
  "tests don't allocate large amounts of memory"; confidence exists only at the code-review level.
- **Neither `loose.c`'s overflow protection nor its "no progress means stuck" guard has a
  discriminating test.** The former needs a case with `declared_size` near `SIZE_MAX`, the latter
  needs a pathological deflate stream; neither is easy to construct naturally. Recorded honestly,
  not pretending there's coverage.

## Phase 13: `sg push` pushing tags

`sg push` was expanded from recognizing only `refs/heads/` to being able to push tags. Three
modes: no name given keeps the original "push the current branch" behavior; when a name is given,
it **checks both `refs/heads/<name>` and `refs/tags/<name>`**, and if both match it reports
`src refspec '<name>' matches more than one` and rejects it; `--tags` pushes everything under
`refs/tags/`.

### Less to change than expected

The survey's conclusion overturned Phase 12's effort estimate. The note at the time was "needs
to expand both the ref-enumeration scope **and the pack's set of packed objects** (the annotated
tag object itself also needs to go into the pack)" -- the second half was wrong:

- `cmd_push.c`'s `walk_add_object` was already type-agnostic; the `SG_OBJ_TAG` branch already
  existed and already walks the `tag.object` edge. Feeding in an annotated tag object id correctly
  pulls the tag object itself, plus the commit it points at and the whole tree, into the send set.
- `sg_pack_build_buf` has no exclusion or special case for `SG_OBJ_TAG`; it goes through the same
  path as blob/tree/commit.
- The network layer (`sg_transport_push`, pkt-line assembly, report-status parsing) is entirely
  unaware of the ref namespace. `sg_ref_name_is_safe` is only used to filter **remote**
  advertisements, not to block local ref names being sent.

So the entire milestone's production code change landed in a single function in
`src/cli/cmd_push.c`. This is a case where "survey first, then write the spec" directly saved
effort: following Phase 12's estimate would have spent half the effort changing pack and network
layers that didn't need to change at all.

### Tags don't update by fast-forward

Real git doesn't apply an ancestry check to tags at all: if the remote already has a tag with the
same name but a different id, it's rejected outright
(`! [rejected] lw -> lw (already exists)`); only `--force` overwrites it. This was determined by
measurement, not recall.

This directly decided the implementation: tags **don't go through** `check_fast_forward`, and a
tag's id never flows into `sg_merge_base`. The survey found that "feeding an annotated tag object
id into `sg_merge_base` gets judged unrelated, so it becomes non-ff, so `--force` gets required"
-- which happens to **coincidentally** match real git. A path that coincidentally matches can't be
kept: `sg_merge_base` walks the commit parent chain, and nobody has ever guaranteed its behavior
on non-commit input, while the correct answer doesn't need it at all. The tag rule is written as
an independent branch: add if the remote doesn't have it, skip if the id matches, check `--force`
if the id differs.

Two other things were also measured rather than recalled: the remote's `refs/tags/<name>` for an
annotated tag points at **the tag object id, not peeled** (so the local id must be read raw via
`sg_ref_read_path`, not the peeling `sg_rev_parse_commit`); pushing a tag **does not create**
`refs/remotes/<remote>/<tag>` -- that namespace belongs to branches only.

### Partial success

When `--tags` pushes multiple tags and one of them is rejected because it already exists, the
rest are still sent, and the final exit code is 1 -- matching real git. This made the original
`updates[2]` stack array (1 branch + 1 chunks ref) insufficient, so it was switched to dynamic
allocation, and report parsing was generalized from "match a single ref_name" to matching each
sent update one by one. `sg_push_ref_update.ref_name` is a borrowed pointer, so the entry array
holding the strings must stay alive until `sg_transport_push` returns.

### Caught during review: not even touching the remote when there are zero tags

The first version, after collecting the tag list in `--tags` mode, would print
`Everything up-to-date.` and exit 0 directly if there were zero tags. The problem was this
happened **before** `sg_transport_ls_refs_push`, which also skipped the entire
`refs/sg/chunks` keepalive propagation block -- something every other push path goes through.
Observable by measurement: for a repo with zero tags against an unreachable remote,
`sg push origin --tags` returns `Everything up-to-date.` with exit code 0 and zero connection
attempts, while `sg push origin` on the same remote correctly reports a connection failure.

A repo using chunked storage but with no tags at the moment would silently skip chunk
reachability synchronization on `sg push --tags`. The fix removes that early return: zero tags
just keeps the candidate set empty; ls-refs runs as usual, chunks are evaluated as usual, and the
existing unified check `entry_count == 0 && !send_chunks_update` decides at the end whether to
print `Everything up-to-date.`.

The shape of the lesson is worth recording: **"there's nothing to do" and "no need to ask the
other side" are two different things.** The former only holds after asking the remote, and this
project has an invariant tied to remote state that real git doesn't have (chunks keepalive) --
any shortcut that returns early based on purely local state will miss it.

### Test discipline: negative assertions need directed mutations

38 new interop checks were added (642 -> 680), all against real git as oracle. Verification has
two layers, both run by the primary conversation:

**Full revert** -- reverting `cmd_push.c` to before the change while keeping the new tests, **21
of the first batch of 26 checks** went red. The remaining 5 didn't go red, but not all of them
lack discriminating power: 3 of them are **negative assertions** ("no remote-tracking ref was
created", "the remote doesn't have this extra tag"), which naturally hold when nothing was
successfully pushed at all. A full revert is structurally ineffective for this kind of assertion.

**Directed mutation** -- those 3 needed targeted changes to be observable, confirmed by actually
running them: removing the `if (!entries[i].is_tag)` guard -> 667/668, with only "doesn't create
a remote-tracking ref" going red; changing the ambiguity check to `if (0)` -> 665/668, with all
three named checks going red. The three spots in the fix round were each run too: replanting the
early return -> 678/680; changing `rc = had_rejection ? 1 : 0;` to `rc = 0;` -> 679/680; changing
the usage guard's `return 1` to `return 0` -> 679/680. Each one turned only the target check red,
with no collateral damage -- a clean mutation is what makes valid evidence (Phase 12 already
learned the hard way that "a broken mutation gives a prettier-looking red").

The implementer initially reported "5 hollow checks"; the reviewer, reading cold, only counted 2.
Neither was entirely right: the correct distinction is "doesn't go red under a full revert" (5)
versus "no directed mutation can catch it either" (2) -- the two numbers measure different
things.

### Known limitations

- **No refspec syntax.** `src:dst`, the `+force` prefix, and `--delete` are not supported. A name
  is always interpreted as both a branch and a tag candidate; a name collision is rejected rather
  than guessed.
- **`--tags` and an explicit name can't be combined**; that's a usage error. Real git allows both
  together; this was deliberately converged the other way: an ambiguous invocation is rejected
  rather than guessed (the same trade-off as Phase 12's `sg reset --soft --hard`).
- ~~**The "only chunks are behind" guard has no discriminating test.**~~ **Fixed 2026-08-12**
  (interop phase6a case 8, 5 checks, 909 -> 914). The gap itself is recorded here honestly:
  changing `entry_count == 0 && !send_chunks_update` to `if (entry_count == 0)` left all 680
  checks green at the time, because every existing chunks-push check was accompanied by a real
  new commit being sent along with it.

  The discriminating scenario comes from a fact confirmed by measurement, not reasoning:
  `sg add` merges the chunk into the keepalive tree **while writing the blob**
  (`keep_alive_add` in `chunk.c`), not at commit time. So after pushing once, running `sg add` on
  a large file **without committing** leaves the branch where it was while `refs/sg/chunks` has
  already moved forward -- exactly the only shape in which this guard can be observed.

  The same mutation now kills 3 of the 5 checks (914 -> 911), with no collateral damage. The
  other 2 not going red is correct, and each serves a purpose: one is a **fixture
  precondition** (asserting that `sg add` actually moved the keepalive ref and not the branch --
  if `sg add`'s behavior ever changes, the other checks would go green for the wrong reason);
  the other is "the second push exits 0", which **still exits 0** under the mutation, a perfect
  demonstration that looking only at the exit code is false coverage. The truly discriminating
  assertion is **whether the remote's own `refs/sg/chunks` advanced to the local keepalive
  commit**, and whether each chunk of the new file actually landed on the remote.
- **A locally created annotated tag's `object_type` is always commit**
  (`cmd_tag.c` goes through `sg_rev_parse_commit`, which always peels to a commit), so `sg`
  itself cannot create an annotated tag pointing at a tree or blob. The push side doesn't rely on
  this assumption (it pushes the raw id in the ref, not looking at `object_type`), so such a tag
  cloned in from real git pushes normally, but this path has no test coverage -- constructing
  such a tag requires real git's involvement.

## Phase 14: a paused rebase is no longer terminated by `reset --hard` and `switch`

A known divergence left over from Phase 12: `sg reset --hard` cleared an in-progress rebase
sequencer state, while real git doesn't. It was recorded in the BUGS section of `docs/sg.1` at the
time and not fixed, because the root cause -- `sg_safe_apply_tree`, the safety wrapper shared by
four commands -- wasn't within `sg reset`'s scope.

### Measure first, then change

The first thing done was measuring real git 2.55.0's behavior, not recalling it. Six-cell result:

| Scenario | Real git |
|---|---|
| `reset --hard` (with or without a commit argument) during a paused rebase | allowed, **preserves** `.git/rebase-merge`; `--abort`/`--continue` both still work afterward |
| `reset --hard` during a conflicted merge | allowed, **clears** `MERGE_HEAD` |
| `switch` / `switch --force` / `switch --discard-changes` | **rejected**, exit 128, state untouched |
| `switch -c <new>` | rejected the same way, and **the new branch is not created** |
| `checkout -f` during a rebase | allowed to switch, **preserves** rebase state |
| `reset` (mixed) during a paused rebase | preserved (sg already got this right) |

This generalizes into one rule: **`MERGE_HEAD` is cleared by any operation that resets the
working directory; rebase sequencer state can only be ended by rebase's own subcommands.**

This rule directly changed the milestone's scope. The initial assumption was "just don't clear it
in the `reset --hard` line," but once `sg_safe_apply_tree` stops clearing it, if `sg switch` kept
allowing the operation as before, it would leave a leftover rebase state pointing at another
branch -- **worse than before the fix**. So switch had to be changed to reject as well. This
isn't scope creep, it's the other half of the same rule.

### Three call sites, three answers

- `cmd_reset.c` (`--hard`): preserves state. That's what real git does.
- `cmd_switch.c`: rejects outright, `--force` doesn't bypass it (real git's `--force` doesn't
  bypass it either), and the gate must sit before `-c` creates the branch.
- `cmd_undo.c`: **keeps clearing**, but the caller now does it itself. `sg undo` has no real-git
  counterpart to copy an oracle from; rewinding the working directory to a snapshot while leaving
  the sequencer state around would make no sense semantically -- `rebase --continue` would resume
  on a tree that had just been yanked out from under it.
- `cmd_merge.c` (fast-forward): no change needed, `cmd_merge.c:511-518` already blocks an
  in-progress rebase upstream.

So `sg_safe_apply_tree`'s responsibility converges to a sentence that can go in a header comment:
clear `MERGE_HEAD`, don't touch rebase state; callers that need the old behavior handle it
themselves after it returns.

### Mutation-verified: only 1 of 9 checks has discriminating power

9 of the newly added interop checks relate to switch. Disabling `cmd_switch.c`'s gate and rerunning,
**only 1 went red** (`sg switch --force ... is still rejected`).

The reason is an existing mechanism got there first: `sg_safe_apply_tree`'s dirty check
(`apply.c:312`) already includes `sg_rebase_state_exists`, and interop runs in a non-tty context,
so `sg_confirm_dangerous` auto-rejects. So a switch without `--force` **exits 1 whether or not the
gate exists** -- just for entirely different reasons. The matching exit code made 8 checks look
confidently green while actually guarding nothing.

The fix was to additionally assert the **message content** on stderr (the gate's own distinct
string) for these cases, not just the exit code. The shape of the lesson: **when the behavior
under test overlaps with an existing protection mechanism in its observable result, the assertion
must dig down to the "reason" layer, or the coverage is fake.** This is the same class of problem
in a different coat as what Phase 10 recorded: "the return value is identical before and after
hardening, only a sanitizer can tell the difference."

### Caught during review: the confirmation prompt lied to `sg undo`

`sg_safe_apply_tree`'s confirmation prompt is shared by four call sites. The first version said
"but the rebase itself will be preserved; to end it use `sg rebase --abort`" -- true for
`reset --hard`, false for `sg undo`: the user gets promised preservation in the very same prompt,
then it gets deleted after they hit y.

Both `switch` and `merge` get stopped by their own gates and never reach this prompt, so `undo` is
the only call site that would ever see this lie. The fix demotes the shared message to a neutral
truth (only mentioning "will overwrite conflict-resolution content"), with `cmd_undo.c` printing
its own "this will abandon the rebase" notice **before** the call -- ordering it so it appears
before the confirmation prompt, visible when the user decides y/N.

This was originally untestable: every `sg undo` in interop carries `--force`, and
`sg_confirm_dangerous` with `force=1` returns early **without printing the message at all**. Only
the branch for non-tty and no force prints the message to stderr (`confirm.c:18-21`); the added
test needed to go through that branch.

### Continuing after `reset --hard <another commit>`

Previously every case used only bare `reset --hard` (equivalent to staying in place), never the
version with an argument. Measurement shows both sides agree: the remaining commits get replayed
on top of **the reset target**, not the original onto. sg's `rebase --continue` takes the parent
from `sg_ref_resolve_head`, and real git's sequencer does the same, so they coincidentally agree
-- but this time it was written down only after measuring it.

Also noting, in passing, an existing divergence that isn't being handled: during sg's rebase,
**HEAD still points at the branch**, whereas real git detaches it. The final graph in the case
group above is the same on both sides, so this wasn't touched.

### Over-normalized comparisons, and negative assertions

A second cold-read caught the `reset --hard <another commit>` group's graph comparison being
**over-normalized**: after replacing every 40-hex in `%P` with `X`, every line of the linear
history collapsed to `"<subject> X"`, actually only verifying "how many parents does each commit
have" -- and the property this group was actually meant to verify (the replayed commits land on
top of **the reset target**, not the original onto) got flattened away exactly. Cross-repo
comparisons must normalize shas (the commit ids on the two sides are necessarily different), but
the normalization scope must stop exactly at "the part that can't be compared." The fix directly
asserts parent identity: within each repo, verify that `feature^` equals that repo's own base sha,
plus one more check that it's "not a descendant of master," verified on the real-git side too
instead of assuming it's correct there.

The negative assertion ("the prompt must not promise the rebase will be preserved") needed its own
mutation, following Phase 12's lesson: keep the new sentence, and **append** the old promise back
in; the positive assertion stays green while only the negative one goes red. That proves it isn't
redundant.

The final `tests/interop.sh` has 726 checks (680 at the end of Phase 13); each of the eight
directed mutations turned at least one check red.

## Phase 15: `sg stash`

### Architecture decision: why a real reflog was implemented

`stash@{N}` is built entirely on top of the reflog -- this is a conclusion from measurement, not a
design preference. Deleting `.git/logs/refs/stash` while keeping `refs/stash` makes real git's
`stash list` come up empty, and `pop` fails outright with no fallback. Going with a path that
"grows a stash stack out of snapshots without touching the reflog" would produce stashes that
real git can't see at all -- worse than not implementing it, because it would let the user believe
their changes were saved. This path directly violates the project's first principle:
`tests/interop.sh` uses real `git` as oracle, and there is nothing that can test a stash git can't
read.

Decisive evidence: hand-forging an sg-style stash (identity `small_git <sg@localhost>`, fixed
`+0000` timezone, all-zero old-oid, hand-written reflog line), **real git accepted it entirely** --
`list`/`show`/`pop` all worked, producing the correct working directory and index. So the reflog
isn't a subsystem, it's "a one-line-per-entry text file plus one hard invariant" -- details in the
next section.

`sg_reflog_*`'s (`include/sg/reflog.h`) `ref_path` is deliberately a **parameter** rather than a
hardcoded `"refs/stash"` -- the format layer and the caller-side coverage of "write one entry on
every ref update" are two different things; Phase 15 only did the former, and only called it once
(`"refs/stash"`); a future HEAD reflog reuses the same file layer directly, spending its budget
on caller-side coverage.

### Two hard invariants (measured)

- **`refs/stash` must equal the new-oid of the reflog's last line**, or real git reports
  `log for ref refs/stash unexpectedly ended`, and **every** subsequent `stash@{N}` becomes
  invalid too -- not just the one that had the problem.
- **A stash commit must have >= 2 parents.** A hand-forged 1-parent version is still visible to
  `list` (list only reads the reflog, doesn't validate commit structure), but `show`/`apply`/`pop`
  all die with `not a stash-like commit`. So "the index commit (parent 2) is required" is not a
  Phase 16 nice-to-have -- `sg_stash_push` writes it unconditionally.

### The boundary of bit compatibility (the two directions require different things)

Real git reading an sg stash only requires **structural validity**, not identity consistency (the
forgery test above already proved it: `+0000`, `small_git`, all-zero old-oid were all accepted --
git never validates a commit's author/committer identity, only the two invariants above).

sg reading a real-git stash requires **lenient parsing**: arbitrary names (including spaces, `<`,
`>`), arbitrary timezones (`+0800`), 3 parents (`git stash -u`), and a `refs/stash` that lives in
packed-refs after `git gc`. `sg_commit_parse` (`src/object/commit.c`) already grows the parent
array, so 3 parents parse for free; `sg_ref_read_path` already falls back to packed-refs.

Because sg's timezone is the fixed literal `"+0000"`, its timestamp comes from `time(NULL)`, and it
ignores `GIT_AUTHOR_DATE`, **sg's stash commit id can never equal the id real git produces for the
same content** -- this isn't a new concession introduced by Phase 15; every commit sg writes was
already like this, it's just that stash is the first feature where each side reads the other's
output, so this had to be written into the spec for the first time, so nobody later designs an oid
equality check doomed to fail. **So no interop check is allowed to compare commit ids between the
two sides.** The strongest oracle is that `sg stash list` and `git stash list` are byte-for-byte
identical -- this string comes from the reflog message sg itself writes, the one place where the
two sides "should" be identical.

### How the "Dropped" message rule was recorded wrong, then corrected

The first pass sampled two cases and recorded it as "pop carries a `refs/` prefix, drop doesn't."
That was a sampling illusion -- it just happened that that particular pop had no argument while
drop had one. After testing all six combinations (pop/drop x {no argument, bare `0`,
`stash@{0}`}), the real rule turned out to be **shared by both subcommands**: git only echoes the
argument back verbatim when it's already in `stash@{N}` form; both no-argument and bare `0` get
resolved into the full `refs/stash@{N}`.

All six combinations now have interop checks, with each sg assertion paired with a git-oracle
assertion, so that when real git changes behavior, the oracle half goes red first instead of a
check silently freezing an outdated understanding. Two pitfalls unrelated to the rule itself but
worth recording were also hit along the way: the oracle must pin `LC_ALL=C` (git translates this
message; a `zh_TW` environment prints it with full-width Chinese punctuation, while CI defaults to
`C` -- without pinning this it would be "passes on CI, red locally"); pop's output contains double
quotes (the `use "git add"` status block), which get truncated when stuffed into an `sh -c`
string, misjudging half the cases, while drop's single-line output has no such problem -- exactly
half of the six cases were misjudged, looking like a real product bug.

### Dangerous behavior kept consistent with real git

**Running `stash push` during a paused rebase makes the commit currently being rebased
disappear** -- measurement confirms sg and real git 2.55.0 **behave identically**: push exits 0;
`rebase --continue` prints "Successfully rebased" and exits 0; the commit currently being replayed
disappears from the log; the work only remains in the stash. The reason is that push resets the
index and working directory back to HEAD, so `--continue` decides "this change already exists
upstream" and skips it. sg does not change this behavior (changing it would itself be a divergence
from git), but it **prints one extra stderr line to notify** the user, and an interop check that
runs both sides once each pins down "behavioral equivalence" (exit code, whether the commit
disappears from the log, whether the stash retains the work).

### Deliberate deviations (list)

- **pop/apply require a clean working directory**; real git attempts a three-way merge with
  existing dirty changes. Stricter than git, consistent with `sg merge`'s existing precedent
  (`merge_require_clean`) -- a clean working directory lets apply set `sg_merge_trees`'s ours
  directly to the HEAD tree, needing no extra merge layer.
- **pop/apply are rejected during an in-progress rebase** (real git allows it), following the
  existing gate precedent in `cmd_switch.c`/`cmd_merge.c`, rather than inventing a new rule.
- **sg has no exit 128**; real git uses 128 for things like "the reflog entry doesn't exist",
  while sg always uses its own message style + exit 1.
- On success, `sg stash apply`/`pop` print `sg status` content (real git prints an equivalent
  status block, but sg's wording is different and the tips reference `sg` commands rather than
  `git`).

### Explicitly not done (Phase 16+)

| Item | Why this cut point is clean |
|---|---|
| `-u` (stash untracked files) | Would need to promote `collect_untracked` (`cmd_status.c`, currently `static`, returning a raw `char **`) into a public API returning `sg_flat_entry`, add a third parent, and add an overwrite rejection. Purely additive -- the commit builder already accepts a parents array, and the reflog layer needs no change at all; but **a mandatory guard has already been added this round**: Phase 15 unconditionally rejects a 3-parent stash, so it never silently restores only the tracked half and drops the untracked files. |
| `show` | The only subcommand that needs a brand-new output formatter: git defaults to `--stat`, and `sg diff` has no such flag and no tree-vs-tree mode. Nothing else depends on it, and `git stash show` already works normally on stashes built by sg. |
| `--keep-index` / `--index` | `--keep-index` is cheap but would complicate the flag matrix; `--index` isn't cheap -- git does a separate merge for the index tree, with its own failure modes. Both are purely additive, not affecting what's already written. |
| `--patch`, `--staged`, pathspec, `stash branch/create/store` | Each is its own independent interface, not affecting what's already written. |
| apply against a dirty working directory | See "Deliberate deviations" above -- the least comfortable trade-off in this phase, but it's the precondition for letting apply directly reuse `sg_merge_trees` without writing a separate layer. |
| general-purpose reflog (`HEAD@{N}`, `sg reflog`, branch updates writing to the reflog) | The file layer (`src/storage/reflog.c`) landed this time; the expensive half -- wiring in a call at every ref-update site -- is left for its own separate phase. |

### Mutation-verified results

| Mutation | Checks turned red |
|---|---|
| push's unmerged guard | originally **0 red** -> 1 after adding the assertion |
| drop not repointing `refs/stash` | 1 |
| push writes the ref but skips the reflog append | 30 |
| `-u`'s three-parent guard | originally **0 red** -> 1 after adding the assertion |
| swapping the conflict label for a branch name | 2 |
| push switched to `sg_safe_apply_tree` | 9 |
| pop's index fix | 11 |
| clear using unlink instead of `sg_ref_delete_under` | 1 |
| list order reversed | 2 |
| `sg stash apply` also dropping | 5 (**59 checks added earlier all missed this**) |
| reflog message normalization disabled | 6 |
| reflog rewrite re-overwriting ident | 3 |
| reflog count-0 writing an empty file instead of deleting it | 1 |
| reflog rewrite not re-chaining the old-id | 2 (predicted at design time to be "probably untestable"; measurement showed these were the only two that could catch it -- see the correction in the infrastructure subsection below) |
| `read_rc == -1` downgraded to a warning | 3 |
| `print_dropped` always adding the prefix | 2 |

**The two most valuable rows deserve their own paragraph.** push's unmerged guard and the `-u`
three-parent guard were both originally **0 red**, for the same shape of reason recorded in Phase
14: the guard exists at both the CLI and library layers, **disabling either layer alone gets
covered by the other**, giving the same exit code for a different reason. The fix follows the
same pattern too -- the assertion has to dig down to the level of the specific stderr message,
not just the exit code.

While examining the unmerged guard, it was also found that the push path **had no CLI-layer guard
at all**: after the library rejected it, the CLI printed the existing catch-all message "cannot
create a stash (uninitialized HEAD, or unresolved conflicts in the index?)" -- a question mark,
while the user was in the middle of a merge conflict and clearly already knew the answer, yet was
only offered a guess. The fix moves the check to where the answer is already known, giving a
declarative sentence instead of a question, with the interop assertion pinning down that specific
message. This assertion itself hit a pitfall too: the first version grepped for "unresolved
conflict", and the catch-all message happened to contain that exact fragment too, so the check
stayed green even after removing the guard -- converging on comparing the guard's own distinctly
worded message was what made it discriminating.

### Known non-coverage (recorded honestly, following the existing style of Phase 10/12)

- **The reflog's 82-byte minimum line-length guard**: entirely subsumed by the subsequent hex and
  format validation. Two constructions were tried (including exactly 81 bytes, both oids valid
  hex, truncated right after), and **neither goes red** under a normal build or `make sanitize`.
  Same class of gap as `index.c`'s `nentries` cap.
- **`chunk_enabled`'s initial value**: `sg_repo_read_chunk_config`
  (`src/storage/repo.c`) unconditionally sets `*enabled_out = 0` on entry, overwriting whatever
  the caller passed in -- a dead store, unrelated to stash but noticed in passing during the
  survey, recorded here so it isn't lost.
- **The failure branch of `sg_write_file_mkdirs`**: a unit test has now been added (using a
  same-named directory to block the write path, triggering `EISDIR`), so this is no longer a gap,
  but the triggering technique is worth recording: pre-create a directory with the same name as
  the target path, and `open()` returns `EISDIR` on write without needing to actually exhaust disk
  space or permissions.

### Lessons about the test infrastructure itself

While running mutations, **the mutation script itself was wrong three separate times**, each time
fabricating a false conclusion of "the guard has coverage" or "the guard has no coverage":

1. A string-substitution mutation first hit the `entries[i].ident` occurrence in the
   `free(entries[i].ident)` line, and after being replaced with a different expression it ended
   up freeing a string constant and aborting outright -- the script thought this was the effect of
   "the guard disappearing", when it had actually just broken the code.
2. `sg_ref_delete_under` has two call sites in the source; the script hit the drop one instead of
   the `sg_stash_clear` one, so the "clear using unlink instead" mutation initially measured
   drop's behavior instead.
3. `make && interop.sh` were run chained; when compilation failed, interop never actually ran, and
   the old log file wasn't cleared, so the script read the previous round's leftover results and
   misjudged this round as green too.

Conclusion: **the mutation script needs verifying just as much as the code under test** -- at
minimum, confirm compilation succeeded, confirm the changed line actually hit the intended site
(not another occurrence of the same identifier elsewhere), and clear old output before generating
new output every round.

The interop checks themselves also had two problems of the same class, with details written into
the code comments: an unpinned `LC_ALL` made one check "pass on CI, red locally" (see the
"Dropped" message section above); stuffing output containing double quotes into an `sh -c` string
blew up the command line, correctly judging only half the cases.

The final `tests/interop.sh` has 826 checks (726 at the end of Phase 14); three unit tests were
added, `tests/test_reflog.c`, `tests/test_stash.c`, `tests/test_objstore.c`; all 31 `make test`
binaries pass, and `make sanitize` is clean.

## Phase 16: an in-progress merge is no longer terminated by `switch`

The first item on the tracking list left over from Phase 15's handoff: `sg switch --force`
during a conflicted merge would succeed in switching away and clear `MERGE_HEAD`, while real git
rejects it. This is the same shape of bug fixed in Phase 14, just in a different subsystem --
merge instead of rebase.

### Measure first, then change

Following the discipline left by Phase 12, the first thing done was measuring real git 2.55.0,
not recalling it:

| Scenario | Real git |
|---|---|
| `git switch <other>` during a conflicted merge | **rejected**, exit 128 |
| `git switch --force <other>` | **rejected the same way**, `--force` doesn't override it |
| `git switch -c <new>` | rejected the same way, and **the new branch is not created** |
| `git switch <the branch already checked out>` | rejected the same way |
| Conflicts resolved and `git add`ed (index clean, `MERGE_HEAD` still present) | rejected the same way |
| `MERGE_HEAD` content corrupt / empty / a directory | rejected the same way |
| `git checkout -f <other>` | **allowed**, and clears `MERGE_HEAD` |

Summary: **the gate looks at whether `MERGE_HEAD` exists, not whether the index still has
conflicts**, and `switch` and `checkout -f` are split on this. sg has no `checkout`, so the half
to align is `switch`.

### Why this gate "looked" correct before the fix

sg wasn't entirely unguarded before the fix -- `sg switch` without `--force` still exited 1 during
a conflicted merge. But the reason was `sg_safe_apply_tree`'s dirty-working-directory
confirmation, and interop runs in a non-tty context where `sg_confirm_dangerous` auto-rejects.
**`--force` was exactly the mechanism meant to bypass this layer**, so the one path that actually
needed to be blocked was the one path that wasn't blocked at all.

This is exactly the same shape as the lesson recorded in Phase 14, and this time it **showed up
again in mutation testing**: removing the new gate entirely and rerunning, "plain switch is
rejected", "`MERGE_HEAD` is still there after rejection", and "HEAD didn't move" **stayed green**,
and only the checks asserting stderr's specific string went red. Exit-code-level assertions are
false coverage here -- verified a second time.

### `exists` is not `read`

`sg_merge_head_read` collapses "no merge" and "merge state is corrupt" into the same -1; using it
as the gate would let a corrupt merge state through unblocked. So `sg_merge_head_exists`
(`include/sg/merge.h`) was added, symmetric with `sg_rebase_state_exists`, only asking whether
something is present at the path.

It's implemented with `lstat` and **without** an `S_ISREG` filter, because real git uses
`file_exists()` -- measurement confirms even "`MERGE_HEAD` is a directory" is still rejected. The
first version wrote `stat() == 0 && S_ISREG(...)`, and was changed only after measuring the
directory case.

This decision has its own discriminating test, since it would otherwise produce identical results
to the `read` version in every other case: four `phase16 corrupt` interop checks, plus
`tests/test_merge_head.c` directly comparing the two APIs' divergence side by side across four
states -- broken/empty/truncated/directory. Verified with a directed mutation: making `exists`
delegate to `read` turned 4 unit-test checks and 4 interop checks red, everything else stayed
green.

### The order of the two gates: measured to be "unobservable", so record it

`cmd_switch.c` now has two gates (rebase first, merge second). Writing a check to pin down the
order, swapping the two around left **all 875 checks green**.

The reason was measured: a paused rebase **does not** write `MERGE_HEAD` (sg only leaves
`.git/sg-rebase/`; real git leaves `rebase-merge/` plus `REBASE_HEAD`; neither writes
`MERGE_HEAD`). The two conditions cannot both hold along any reachable path -- only hand-forging
both state files gets there, and that state doesn't exist under real git either, so there's no
oracle.

So the ordering is **genuinely unobservable**. Following the project's existing practice, it's
recorded under "known-untestable guards" rather than pretending there's coverage, and the check
was rewritten to pin down the fact that actually supports the conclusion: `MERGE_HEAD` doesn't
exist during a paused rebase (verified once on each of sg and real git).

### Fixture contamination: an over-triggering mutation can go red for the wrong reason

The check group for "the gate must not over-trigger" (switch must return to normal after the
merge is finished) initially used `sg switch` to build the fixture's branch structure. After
making the gate always trigger, that group did go red -- but **the fixture itself failed to
build**, the precondition failed first, and the behavior under test was never reached. This is
another version of "same outcome, different reason," except this time the false result is a red
light instead of a green one.

The fix was to have that group use real git to build the branch structure instead, leaving only
the merge and the final switch for sg. Rerunning the same mutation, the preconditions all stayed
green, and only the four "should return to normal" checks went red.

### What wasn't touched

`reset --hard` still clears `MERGE_HEAD` as before (the rule set in Phase 14 didn't change); the
new gate lives only in `cmd_switch.c`, not pushed down into `sg_safe_apply_tree` -- the latter is
shared by four call sites, and pushing it down would block `reset --hard` too. The `phase16
reset` group exists to pin exactly this.

### Caught by a cold read: the new gate turned an existing inconsistency into a dead end

During the cold read, the reviewer pointed out: `sg_merge_head_exists` is only used at the one
`switch` call site; the other eight places that check "is a merge in progress" still use
`sg_merge_head_read(...) == 0` -- exactly the defect named in the new header comment itself.
Reproduced independently and confirmed, and worse than reported.

With `MERGE_HEAD` corrupted (empty, invalid content, a directory), the measured divergence:

| Command | Real git 2.55.0 | sg before the fix |
|---|---|---|
| `merge --abort` | rc 0, **clears it** | rc 1, **leaves it** |
| `reset --hard` | rc 0, **clears it** | rc 0, **leaves it** |
| `reset --mixed` | rc 0, **clears it** | rc 0, **leaves it** |
| `reset --soft` | **rejects** | **allows it** |
| `stash push` | rc 0, **clears it** | rc 0, **leaves it** |
| `status` | reports "you have unmerged paths" | **doesn't report it** |
| `commit` | **loudly rejects** with "corrupted MERGE_HEAD file" | rc 0, **silently produces a single-parent commit** |

The last row is the most serious: merge semantics silently disappear from the commit graph while
sg reports success. This is exactly the class of divergence this project exists to prevent, and
**it can never be caught by looking at the exit code alone** -- so that test group asserts not
just exit != 0, but also "no new commit was produced at all."

The first six rows together form a dead end: the new gate makes `switch` reject permanently, and
no command can clear that file -- even the two paths `switch`'s own error message suggests
(`sg commit`, `sg merge --abort`) don't work, leaving manually `rm .git/MERGE_HEAD` as the only
way out. **This dead end was caused by the new gate**: before the fix, `switch --force` could at
least still get through (even though the behavior was wrong).

The fix was to converge every "is a merge in progress" check onto `sg_merge_head_exists`. After
converging, `sg_merge_head_read` has only **one** call site left in `src/` -- `cmd_commit.c`, the
only place that actually needs the "value" (the second parent), and it now asks the two questions
separately: first whether it exists, then whether it can be read; if it can't be read, it rejects,
matching real git.

The `cmd_rebase.c` one is the only one with no oracle: measurement shows `git rebase` on a clean
working tree **doesn't look at `MERGE_HEAD` at all** (valid or corrupt makes no difference, it
just runs to completion and clears it); sg rejects deliberately. Given the deliberate rejection,
it must cover the corrupt state too, or rebase and switch would give different answers to "is a
merge in progress." This is written into the comment.

### This round's mutations

- Reverting all eight sites **other than switch** back to `read`: 18 checks went red, covering
  every row; while "a valid merge still produces a two-parent commit" stayed green, proving the
  rejection targets corruption, not merges themselves.
- Reverting only `cmd_commit.c`: exactly 3 red, all in the commit group -- per-site attribution
  holds.
- Adding `S_ISREG` back: the first version only turned a unit test red, with interop entirely
  green (the reviewer pointed out this coverage gap). After adding a "`MERGE_HEAD` is a directory"
  CLI + oracle case, the same mutation now turns 2 interop checks red too.

The shape of the lesson: **when converging a predicate, a call site that isn't converged doesn't
"stay as-is" -- it interacts with the new behavior.** Here the interaction happened to be the
worst kind -- the new guard turned old lenient behavior into a state with no way out. A fix that
looks like it touches only one file actually has a boundary of "every place asking the same
question."

The final `tests/interop.sh` has 909 checks (826 at the end of Phase 15); `tests/test_merge_head.c`
was added as a unit test; all 32 `make test` binaries pass, `make sanitize` is clean. Eight
directed mutations (removing the gate, `exists` -> `read`, always-triggering, swapping the order,
`exists` delegating to `read`, adding `S_ISREG` back, reverting all non-switch call sites,
reverting only `cmd_commit.c`) each had their red-check scope verified individually.

## Phase 17: general-purpose reflog (`HEAD@{N}`, `<ref>@{N}`, `sg reflog`)

## Phase 17: general-purpose reflog (`HEAD@{N}`, `<ref>@{N}`, `sg reflog`)

Phase 15's handoff listed "general-purpose reflog" as an item explicitly deferred to its own
phase: the file layer (`src/storage/reflog.c`) had already landed by then, and `stash@{N}` was
already using it; what was missing was "every ref-update site needs to wire in a call" -- this
phase does exactly that half.

### The opening survey overturned the assumption of "just hang it off refs.c"

The original plan was to call `sg_reflog_append` directly inside `refs.c`'s write function. The
survey first found this assumption didn't hold: **ref writing has no convergence point at all.**
`cmd_push.c`/`cmd_fetch.c`/`cmd_clone.c` each hand-roll their own verbatim copy of
`write_ref_file`; HEAD's symbolic write is hand-rolled separately in `cmd_switch.c`/`cmd_clone.c`/
`repo.c`; and `refs.c` itself has **no** HEAD-write function at all. Hanging the reflog off
`refs.c` would silently miss fetch/push/clone's ref updates and every HEAD move -- not an error,
just those reflog lines silently not existing.

The decision to converge wasn't because "too much duplication feels uncomfortable" -- it's because
this is an **invariant of the ref backend layer**: rules like "how should the old value be read",
"which namespaces should be logged", and "when does HEAD get a mirrored entry" scattered across a
dozen independently implemented call sites means missing one produces no error at all, just a few
missing reflog lines -- this is the same shape as the Phase 16 bug (a rule that should live in a
single gate got scattered across multiple call sites, and the unconverged one was the hole). So
`sg_ref_update`/`sg_ref_set_head` (`include/sg/refs.h`) were added first as the single write
point, and all six hand-rolled ref-write call sites (push/fetch/clone's `write_ref_file`, plus
three HEAD symbolic writes) were switched to use it. The old value is read by the write point
itself; callers no longer have to track it themselves. Converging actually made the later diff for
adding reflog messages smaller: `do_fast_forward` (`cmd_merge.c`) didn't even need its function
signature changed, just one more string argument filled in. Along the way, an existing semantic
bug in `cmd_push.c` also got fixed -- it had recorded a remote-tracking ref's old value as **the
old value from the remote advertisement**, but a local remote-tracking file should record **the
local current value**, and the two diverge once someone else has also pushed to the same remote;
with `sg_ref_update` uniformly reading old from the local file, this divergence disappeared as a
side effect.

### Three rules confirmed by measurement (real git 2.55.0)

The rules weren't recalled, they were measured one by one:

- **Asymmetry**: a concrete ref's log (e.g. `refs/heads/master`) only appends a line when
  `old != new`; `logs/HEAD` always appends regardless of whether `old == new`. Measured:
  `git reset --hard HEAD` (target is itself) -> `logs/HEAD` +1 line, the branch's own log +0
  lines.
- **HEAD mirroring**: when updating the branch HEAD currently points at, `logs/HEAD` gets a line
  with old/new/message **byte-for-byte identical** to it. This happens even using
  `git update-ref` to bypass all porcelain, proving this is an invariant of the ref backend layer,
  not a special behavior of some subcommand. The reverse doesn't hold: updating a branch HEAD
  doesn't point at leaves `logs/HEAD` untouched.
- **Policy**: only `HEAD`, `refs/heads/*`, `refs/remotes/*`, and `refs/stash` get a reflog; tags,
  `refs/sg/chunks`, `refs/small-git/undo/*` don't -- consistent with real git (in a non-bare repo,
  `core.logAllRefUpdates`'s default coverage is exactly these namespaces). `sg_ref_update` returns
  -1 outright for any policy-excluded `ref_path` given a non-NULL message, writing absolutely
  nothing (not even the ref itself), so "which places don't log" is a closed list auditable with a
  single grep, rather than something to remember at every call site about which one should pass
  NULL.

### `<ref>@{N}`

`<ref>@{N}` names that reflog entry's **`new_id`**, not `old_id` -- an easy direction to get
backwards: @{0} means "the value the most recent update moved the ref **to**", not "the value
before it moved"; the oldest entry's `old_id` is all-zero, and picking the wrong direction would
silently return all-zero at the boundary instead of the first commit. `HEAD@{N}` and
`<branch>@{N}` each read a different file, `logs/HEAD` and `logs/refs/heads/<branch>`
respectively.

The line between an index and a date selector isn't syntactic, it's a **numeric magnitude** --
real git has no hardcoded syntax rule for this, it's a heuristic: measurement shows `@{10000000}`
(1e7) is treated as an index, while `@{100000000}` (1e8) real git treats directly as a Unix
timestamp and goes through the `@{<date>}` path, printing a warning that "log only goes back to
<date>". sg doesn't support a date selector, so it deliberately uses a **digits-only whitelist**
to define "this is an index"; anything else (including `@{u}`, `@{now}`, `@{-1}`) is cleanly
rejected, rather than imitating a heuristic bounded at 1e8 that even real git's own documentation
doesn't spell out. The result is that above 1e8, sg rejects while git accepts it (parsing it as a
date) -- a deliberately recorded divergence, not a defect.

Bare `@{N}` (with no ref name in front) is always rejected. Real git's bare `@{N}` refers to
**the current branch**, not HEAD -- measurement shows the two have different values after
operations like `reset` cause HEAD's log and the branch's own log to diverge. Guessing it means
HEAD would be an implementation that **looks like it runs but gives a wrong answer**, more
dangerous than an outright rejection, so rejection was chosen over guessing.

### A real bug caught by a cold read: the `~`/`^` suffix loop never validated `op`

`sg_rev_parse_commit`'s suffix loop had, for years, only checked `if (op == '~') ... else
/* '^' */ ...`, never actually checking whether `op` was `'~'` or `'^'` at all. This was safe as
long as there were only two stop characters (`~`/`^`/`\0`) -- **an invariant guaranteed by the
caller, but never written into the code**: the base scan only stopped upon hitting these three
characters, so the first character entering the suffix loop was necessarily `~` or `^`. `@{N}`
legitimately gave the base scan a fourth stop condition (`@{`), which broke this premise:
measured, `sg tag t 'master@{0}x'` returns 0 and points the tag at `master@{0}`'s parent (`x` gets
swallowed into the "if not `~` then `^`" branch, and since there's no digit after it,
`parse_suffix_number` returns the implicit 1 for an empty string), while real git reports
`ambiguous argument` outright for the same input. `sg_rev_parse_commit` is the sole entry point
for parsing a user's revision string for `sg reset` (a destructive operation) and `sg tag`; a
mistyped character being silently resolved to a nearby but wrong commit isn't "rejected", it's
"resolved somewhere else." The fix adds `if (op != '~' && op != '^') return -1;` inside the loop
itself, rather than patching the tail end of the `@{N}` section -- restoring the invariant as a
local property of the loop itself, no longer depending on an assumption the caller made years ago
still holding.

### Deliberate divergences

1. **A three-way merge writes `merge <arg>: Merge made by the 'sg-3way' strategy.`**, while real
   git writes `'ort'` (or historically `'recursive'`). sg hasn't implemented the ort strategy;
   copying the strategy name verbatim would mislead anyone running `git reflog` on an sg repo into
   thinking "this was merged with ort." The grammatical shell from git is kept (its own strategy
   name has already changed across versions before -- `'recursive'` -> `'ort'` is precedent), just
   swapping in an honest name of its own.
2. ~~**`switch` doesn't write a reflog line for detached HEAD**~~ -- **resolved in Phase 18**, and
   the diagnosis below chose the first of its two options: changing the shared function and
   auditing all 21 call sites one by one. The original text is kept because the reasoning for "why
   this corner shouldn't be patched separately at the time" still holds. Real git, in this
   situation, writes `checkout: moving from <40-hex> to <target>` (measured). Trying to replicate
   this didn't work: `sg_ref_resolve_head` (`include/sg/refs.h`) returns -1 outright for a
   detached HEAD -- it only recognizes a symbolic HEAD, so the fallback logic was never triggered.
   Doing it correctly means either changing this widely depended-on shared function used to
   determine "is it detached" (affecting every command relying on it), or having `cmd_switch.c`
   parse `.git/HEAD`'s raw content itself, bypassing it. And sg's overall footing in the detached
   HEAD state is already shaky to begin with (`sg status` prints `On branch ?`, `No commits yet`,
   with no coherent user experience), so this was left for Phase 18 to handle together, rather
   than patching one unaccompanied corner here.
3. **`sg clone` doesn't create `refs/remotes/<remote>/HEAD`, nor write its log.** Real git has both
   after a clone. During implementation there was briefly a version that wrote only the log
   (without creating the ref itself), which was removed -- `sg reflog origin/HEAD` goes through
   `sg_rev_parse_ref_path` to resolve the ref name, and that function requires the ref itself to
   exist; if the ref doesn't exist, that log file would never be readable, so writing it would be
   dead data, and would amount to claiming a slice of history this repo never actually owned.
4. **`sg clone` doesn't write a log for `refs/remotes/<remote>/<branch>`** -- this one actually
   **matches git** (git also has to wait for the first `fetch` before it writes that log for the
   first time), but it's specifically recorded here because it feels counter-intuitive given the
   instinct that "clone should set everything up", so nobody later mistakes it for a bug and
   "fixes" it.
5. ~~**Rebase's reflog shape was left for Phase 18**~~ -- **completed in Phase 18**, and the
   approach was exactly what's described in the section below: switching to the detached model
   instead of patching in a set of fake messages that don't match the shape. The original text is
   kept because it clearly explains "why adding message strings wouldn't solve it." Real git's
   rebase replays every commit on a detached HEAD throughout, moving the branch over only once at
   the very end, so the branch's own log gets only one line while `logs/HEAD` gets a series of
   `rebase (start)`/`(pick)`/`(finish)` entries. sg's rebase never detaches, moving the branch ref
   once for each commit picked -- structurally unable to reproduce that message sequence
   verbatim. Phase 17's approach was to have every `sg_ref_update_branch` call in `cmd_rebase.c`
   keep passing `NULL` (equivalent to `sg_ref_update`'s `reflog_msg == NULL` branch), i.e. writing
   no reflog at all, rather than writing a set of fake messages whose shape doesn't match real
   git's.
6. **`sg fetch`'s message only embeds the remote name**, while real git embeds the full argv
   (e.g. `fetch -q origin: ...`). sg's flag set differs from git's, so copying argv verbatim would
   be meaningless; embedding only the remote name makes `sg fetch origin` and `git fetch origin`'s
   messages byte-for-byte identical (measured) -- the largest common denominator that can be
   aligned.

### What was learned about test discipline this round

- **`git reflog` is a free oracle**: once the on-disk format is correct, the same sequence of
  operations can be run once on sg and once on real git, comparing `git reflog`/`sg reflog`'s
  message field byte-for-byte -- no need to guess the wording yourself, the answer is right there
  in real git's output.
- **False coverage, only revealed by actually running it**: temporarily relaxing `@{N}`'s
  "digits only" whitelist to "just non-empty", the tests for `@{u}`/`@{now}`/`@{-1}` that
  appeared to be verifying "must be a syntactically valid index" **still went red, but for the
  wrong reason** -- the non-digit characters participated in char arithmetic, producing a huge
  value that was caught by an unrelated bounds check, not by the "must be digits" rule. A
  deliberately constructed `wideranger@{A}` (writing 20 log entries first so that `'A'-'0'==17`
  falls within the valid index range) was needed to get a genuinely discriminating case -- it had
  to hit "this is a legitimate reflog depth" and still get rejected for "isn't a digit", rather
  than being caught by a different guard instead.
- **A false red**: the `master@{1}5` test did go red under the 2-commit fixture, but the reason
  was that the root commit has no parent -- `@{1}5` got misparsed as `@{1}^5`, and looking for 5
  levels of parent would naturally fail -- entirely unrelated to the rule "an invalid suffix must
  be rejected"; it was coincidentally red on the same exit code. Only after extending the fixture
  to 3 commits, separating "parses legitimately but not enough levels" from "invalid character" as
  two distinct failure reasons, was it confirmed the latter was actually in effect. **Proving a
  test can go red isn't enough -- it also has to be confirmed it goes red for the right reason.**
- **Redundant defensive checks hide the real verification point**: `revparse.c` had two blocks of
  checks that entirely duplicated existing code logic; deleting either one alone produced **zero
  red tests**. They weren't an extra layer of safety net -- the defense underneath
  (`parse_suffix_number`, `sg_reflog_at`) was already blocking the same input; the place that
  actually deserves a mutation is the layer below, and mutating these two redundant checks would
  never reveal anything. Both were deleted, rather than left in place creating the illusion that
  "there's a guard here."
- **Negative assertions each need their own directed mutation**: making `sg clone` also pass a
  message for `origin/<branch>` (i.e. secretly starting to write that log) -> exactly 1 check went
  red; removing `sg_ref_delete_under`'s tolerance of `ENOENT` on the log file's `unlink` -> 3 went
  red (one of which was caught incidentally by an existing prefix-collision test). The `sg clone`
  mutation deliberately only passed a message for the remote-tracking ref, not for the tag -- if
  it indiscriminately passed a message for every ref, the tag would trigger the policy gate
  (a policy-excluded ref_path given a non-NULL message returns -1 outright), failing the whole
  clone -- that kind of red isn't "the negative assertion working", it's "clone crashed entirely",
  which doesn't count as evidence.
- **`logs/HEAD`'s line-count assertion**: initially compared the last line's content using
  `tail -1`, but constructing a mutation that "mistakenly writes two identical lines" still passed
  under `tail -1` -- the last line's content really is the same in both cases. Only additionally
  asserting the **line count** (`wc -l`) can tell "should be one line" apart from "an extra line
  got written."

### Third cold read: the model change broke an invariant of `--abort`

Segment two was handed off for a cold read, which caught a **high-severity issue that reproduced
on its own**, directly caused by this change.

`finish_rebase` is **two writes**: first advance the branch, then reattach HEAD. The branch
restoration in `--abort` was removed, on the reasoning that "the branch was never touched
throughout" -- that reasoning only holds if the whole rebase is atomic. If interrupted between
those two writes (crash, SIGKILL, a transient I/O failure in `sg_ref_set_head`), the branch has
already stopped at the post-rebase tip while `.git/sg-rebase/` is still present. At that point
`sg rebase --abort` would:

- restore the working directory to the tree **before the rebase**,
- reattach HEAD to the branch (while the branch is **already** at the post-rebase tip),
- delete the sequencer state, **exit 0**, and print "'topic' is back at <orig>".

The three contradict each other, and it's a **false success**. No commit is permanently lost (the
new tip is still reachable from the branch), but the tool lied. The old version of `--abort`
unconditionally reset the branch back to `orig_head`, robust against any intermediate state; the
new version depends on an invariant that this very path can itself break.

The fix happened to need no trade-off: `--abort` was restored to unconditionally reset the branch,
and **this doesn't affect the reflog shape** -- `sg_ref_update_branch` passes a NULL message, so it
never writes a log line either way; the value doesn't change in the normal case either. The write
order is "branch first (HEAD is still detached, so it doesn't mirror), then reattach HEAD."

This scenario **can't be mutation-verified by reverting a line** (it's missing defense, not
broken existing logic), but the interrupted state can be **constructed** for testing, and that's
how interop was written. Removing the fix turns those two exactly red.

Two more things came out of the same round:

- **`<oid>` normalization was masking `onto`'s correctness.** The branch's finish line is the only
  rebase message that embeds a full 40-hex, and before dual-track comparison, any 40-hex gets
  replaced with `<oid>` -- so swapping `state->onto` for the new tip or `orig_head`, **the entire
  phase18f group stays green.** The cold read specifically asked the primary conversation to
  actually run this mutation to verify its prediction, and running it confirmed the prediction was
  correct. A direct assertion bypassing normalization was added (plus a precondition check
  confirming onto and the resulting tip actually differ, or that assertion itself would have no
  discriminating power).
- **The fast-forward path's interruption window had no marker at all.** The cold read claimed it
  "self-heals on rerun", and measurement showed **this was wrong**: rerunning
  `sg rebase <upstream>` gets rejected because HEAD is detached, and `--abort` says "no rebase in
  progress" -- leaving only a confirmed `sg switch` as the way out. The normal path writes
  sequencer state before touching anything; ff didn't. It now does too (purely to make that window
  recoverable), deleted again at the finish.

### The oracle's environment itself must be declared

phase18f's `(continue)` group was entirely green locally, and **red on every CI runner** (both
Linux and macOS). The diagnostic output added for this gave the answer directly, and it overturned
the original assumption ("git changed the message between 2.54 and 2.55"): git's own track **wrote
zero rebase log entries at all** -- not a different message, the rebase never completed at all.
`git rebase --continue` needs to open an editor, and it fails without one.

It passed locally because this machine's shell exports `GIT_EDITOR=true`.

The lesson here isn't "you need to set GIT_EDITOR" -- it's that **the phrase "use real git as
oracle" only holds when the oracle's execution environment is itself something the test suite
decides.** That measurement was real, but it was obtained through a setting the test never
declared, one that just happened to exist in the developer's shell. `interop.sh` now
`export`s `GIT_EDITOR=true` itself, with the reason spelled out in a comment.

Incidentally, the first attempt to simulate a "clean environment" while verifying this fix
overcorrected -- it turned off the git config too, so git couldn't find a committer identity, and
the entire suite ended at the first step, with empty output. **Empty output reads exactly like a
pass**, and only actually looking at the line count reveals it.

### Follow-up verification at wrap-up: the self-written reset assertion was also false coverage

After merging, running back through the directed mutations for "negative assertions" (the
remembered rule: assertions like "X was not done" can't be verified by a full revert) also found
that **Phase 18's own newly added reset assertion was green with no discriminating power.**

Disabling `move_head_to`'s detached branch (so that it still writes to `refs/heads/%s` even when
detached, with `current_branch` being NULL, producing a branch literally named
`refs/heads/(null)` and never touching HEAD at all), originally only **the four phase14 checks**
went red -- none of the assertions newly written for this fix in phase12 and 18d went red at all.

The reason was fixture degeneration: `p12r_base` tags `v1` on c1, and the case detaches at
`HEAD~1` (= c1 = exactly v1) and then does `reset --mixed v1` -- **that's a no-op.** "HEAD moved
to the target" still holds true on a setup where HEAD never actually moved at all. This is the
third time this milestone has hit the same shape (the previous two were the tag and the scenario
being offset by one, and the "tag moved away" case reverting the id for an unrelated reason).

Changed to detach at c2 and reset to v1 (an actual move), plus two additions: a **precondition
assertion** confirming the target differs from the starting point (or that assertion itself would
have no discriminating power), and a check comparing **the full branch list** rather than just
master -- writing to the wrong place doesn't necessarily hit an existing branch,
`refs/heads/(null)` wouldn't. The same mutation now turns 7 red, with the new guard directly
printing `refs/heads/(null)`.

Also wrote the `tests/mutate.sh` usage pitfalls hit along the way into the script's comments: perl
delimiters colliding with C syntax (`s{}{}` colliding with braces, `s!!!` colliding with `!=`),
and **always adding `/g` when a literal occurs more than once.**

### Not verifiable (recorded honestly)

- Two malloc-failure branches (the OOM paths when `cmd_commit.c` and `cmd_switch.c` build reflog
  message strings) -- this project has no malloc-failure injection mechanism, so confidence is
  only from reading the code to confirm the corresponding `free` runs on the correct path.
- Phase 14/16's switch gates are unobservable **along the reflog dimension**: those tests were
  written before the reflog existed, only reading the exit code and the working directory/index
  state; "a rejected switch wrote no reflog line at all" is currently guaranteed only by the call
  ordering ("the gate runs before any side effect"), with no dedicated assertion.
- The length-truncation branch of the path in `sg_ref_delete_under`'s log-file deletion path --
  needs a branch name whose length lands exactly at the `SG_PATH_MAX` boundary, causing
  `.../logs/...`'s extra 5 bytes over the ref path itself to overflow exactly; expensive to
  construct, not added.
- `sg push`'s suppression branch of rule 1 (no append when `old == new`) in the scenario where
  "the local remote-tracking ref already equals the new value" -- no dedicated case distinguishes
  "not written because it was suppressed" from "simply never called."
- `sg fetch`'s fast-forward check's behavior in the edge case where "an object is missing partway
  through new's ancestor chain": the current implementation conservatively labels it as a
  forced-update (the direction the message lies in is safe -- it never mislabels a non-fast-forward
  update as fast-forward), but no case constructing this scenario has been built.

### One more thing worth noting

`sg_ref_update` has a known limitation, documented in its own header comment: `sg_ref_read_path`
collapses "ref doesn't exist" and "ref file is corrupt" into the same -1, so when an existing but
corrupt ref is updated, its reflog entry records `old_id` as all-zero, making it look like this ref
was just created, rather than "existed but couldn't be read." No third state was introduced for
this scenario, keeping consistent with the project's existing convention of "-1 uniformly means
failure."

The final `tests/interop.sh` has 998 checks (909 at the end of Phase 16; the warm-up survey first
added checks bringing it to 914, then batch A to 919, batch B to 944, batch C to 978, batch D
wrapped up to 998); all 34 `make test` binaries pass (`tests/test_ref_update.c` and
`tests/test_reflog_messages.c` were added; `tests/test_reflog.c` was already a file-layer test
from Phase 15), `make sanitize` is clean, and the subcommand count grew from 23 to 24 (`sg reflog`
added).

## Phase 18: detached HEAD, and rebase's reflog shape

Phase 17 left two items here (see items 2 and 5 under "Deliberate divergences" above). They look
like two topics, but are actually one: real git's rebase replays entirely on a detached HEAD, so
unless detached HEAD first becomes a state sg genuinely understands, rebase's reflog shape can
never structurally line up.

### Root cause: one -1 meaning two different things

`sg_ref_resolve_head` returns -1 outright whenever `sg_ref_current_branch` returns NULL,
collapsing "HEAD is detached" and "HEAD is unborn (repo just created, no commits yet)" into the
same failure. **All 21 call sites read it as the latter** -- the header comment said so too.

This isn't a problem that "would only happen once Phase 18 arrives." Real git can turn any sg repo
into a detached one at any time (`git checkout --detach`), and in that state sg doesn't reject --
it **silently answers wrong**:

- `sg log` prints "fatal: your current branch does not have any commits yet", with a full intact
  history sitting right underneath.
- `sg status` treats HEAD's tree as empty, so a clean working directory gets reported as "every
  tracked file is new."
- `sg_safe_apply_tree` shares this same computation, so before `sg switch` overwrites the working
  directory, it uses an empty tree to decide what it's about to overwrite.
- `sg branch <name>` returns "the current branch has no commits yet" and refuses to create the
  branch.
- `sg stash push`/`pop` fail outright; and `sg_snapshot_create` would record the snapshot as a
  **root commit** -- the safety net itself losing the parent link.

The fix makes a detached HEAD actually resolve to its raw sha, leaving -1 to mean only "unborn."
All 21 call sites were audited one by one; **none of them was using this failure as a detached
guard**, so nobody lost a guard as a result; the three commands that genuinely need to reject
detached (reset, rebase, push) check `sg_ref_current_branch`, unaffected.

`sg_ref_head_is_detached` is deliberately three-state (1/0/-1): **"a broken HEAD" is not
"detached."** This distinction has real consequences -- the "detached" answer is exactly what a
caller uses to decide "I'm allowed to write a raw sha into HEAD"; treating broken as detached
would launder a broken state into something that looks normal.

### Why detach couldn't borrow `sg_ref_update(git_dir, "HEAD", ...)`

When Phase 17 converged ref writing, `sg_ref_update` incidentally gained the ability to write a
raw sha into HEAD (`ref_path_reflog_allowed` already permitted `"HEAD"`, and
`write_ref_path_raw` treats ref_path uniformly). The survey therefore suggested using it directly,
**which was wrong, and quietly wrong**: it reads old_id via `sg_ref_read_path`, and while HEAD is
still `ref: refs/heads/<b>`, that hex parse necessarily fails, falling back to all-zero -- so
"detaching from commit A to B" gets recorded as "B was created out of nowhere." Real git writes
the commit being left, in that field (measured).

So `sg_ref_set_head_detached` was added, with its old_id going through the **now-fixed**
`sg_ref_resolve_head`, resolving both symbolic and raw-sha shapes. A directed mutation switched it
back to `sg_ref_read_path`; only the "detach from a branch" assertion went red, while "detach from
already-detached" stayed green -- because in the latter case HEAD is already a raw sha. The two
tests each belong to a different source shape; missing either would leave this exact blind spot.

### `at` or `from`: the label doesn't live in HEAD, it lives in the reflog

`HEAD detached at <x>` / `from <x>`'s `<x>` is **not** the current HEAD, it's the original point of
detachment, which can only be known by scanning `logs/HEAD`'s last `checkout: moving from ... to
<y>` entry backward. The full rule as measured (git 2.55.0):

- If the token `<y>` **still resolves to the same commit** now, use it; otherwise fall back to an
  abbreviated id. So a tag that was moved, or an expression like `HEAD~1` that was never a ref to
  begin with, both fall back to an id.
- Strips the `refs/tags/` and `refs/remotes/` prefixes, **but not `refs/heads/`** -- git really
  does print `HEAD detached at refs/heads/other`.
- The literal token `"HEAD"` doesn't count (`sg switch --detach HEAD` prints the id). Using HEAD
  to explain where HEAD is would carry no information anyway.
- The line between `at` and `from` is a **value comparison**, not "did something happen":
  `reset --hard` back to the detachment point turns it back into `at`.
- When `logs/HEAD` has no checkout entry at all, git abandons this wording entirely and prints
  `Not currently on any branch.` / `* (no branch)`.

### The order of the finish steps lets two existing rules produce git's shape on their own

Once rebase was switched to the detached model, the order of the two finishing steps became
meaningful, and **required no special case at all**:

1. **First**, update the branch. HEAD is still detached at this point, so `sg_ref_update`'s rule 2
   (`is_current_branch`) is 0, and that line **does not** get mirrored into `logs/HEAD` -- the
   branch ends up with exactly the one line it should have, `rebase (finish): <ref> onto <onto>`.
2. **Then**, reattach HEAD to the branch. old == new (both are the final tip), and `logs/HEAD`
   never applies no-op suppression (rule 1's asymmetry), so the `returning to` line still gets
   written.

Two rules already measured in Phase 17 produce exactly git's output once put in the right order.
Swapping the two steps turns five checks red, including "the finish line has old == new."

`--abort` therefore also became simpler: the branch was never touched throughout, so all that's
needed is reattaching HEAD. Real git's branch log also gains zero lines after an abort, for
exactly the same reason -- the value didn't change, and rule 1 suppresses it.

### Scope was forced to change: `sg reset` had to support detached HEAD

The scope agreed at the start of work was "merge / reset / a rebase started from detached all
keep rejecting." That decision's premise was "detached can only ever be caused by the user or real
git." Once segment two switched rebase to the detached model, that premise disappeared: **a
paused rebase is now itself detached**, and rejecting detached in `reset` would directly remove
"reset is allowed while a rebase is paused" -- a capability Phase 14 established and measured
against real git, so 7 interop checks went red. Real git allows it for exactly the same reason:
its own rebase is detached too.

So `reset` was opened up, while `merge` and "a rebase started from detached" kept rejecting. Two
existing tests that had pinned "reset must reject under detached" were **rewritten the other
way** -- they were now wrong. Along the way, the ref write duplicated verbatim across reset's
three modes was converged into `move_head_to`.

This itself is a lesson: **a scope decision can get overturned by later implementation**, and what
overturned it was an existing test going red, not someone remembering.

### Caught by a cold read: three cases of "no assertion" rather than "wrong assertion"

When segment one was handed off for a cold read, 18a-18d had already verified the HEAD file, both
reflogs, refs, and the wording of `status` and `branch` -- **yet not a single one read `switch`'s
own stdout.** Three divergences from real git lived exactly in that gap:

1. `Previous HEAD position was ...` was tied to "switching to a branch," so detach->detach never
   printed it at all. It actually belongs to "**leaving** detached HEAD." The rule got gotten
   wrong again while fixing it -- real git only prints it when **the commit actually changes**;
   all three same-commit combinations print nothing -- the new test caught it within a minute.
2. `sg switch --detach HEAD` printed `HEAD detached at HEAD`.
3. The description buffer was only 512 bytes; a longer ref name made the entire function fail, and
   `status` rendered that failure as `Not currently on any branch.` -- telling a HEAD that is
   clearly detached that it's "not on any branch" isn't less information, it's wrong information.

All three are **missing assertions**, not wrong ones. This is the first time this project has
clearly hit this category: every dimension the tests covered (file content, exit code, reflog) was
correct, just missing an entire dimension (stdout).

### False coverage: four caught this round

1. **The label and the scenario offset by one**: the display test group used `sg commit` to move
   HEAD, but at the time `sg commit` was still rejected under detached, so the commit never
   happened, and every subsequent case shifted forward by one -- "at should become from" got
   asserted backwards, **and all of it passed.** It was only noticeable because the helper printed
   the compared string into the check's label. Switched to using git to move HEAD from then on:
   this section tests wording, and shouldn't silently become a no-op just because sg's write
   capability regressed.
2. **Passing for an unrelated reason**: the "tag was moved away, so it falls back to an id" case --
   the previous case happened to leave HEAD at the target commit, and git doesn't write to the
   reflog for a "checkout that didn't move" -- so the detachment point was still the earlier
   `HEAD~1` entry, and it fell back to an id because `HEAD~1` was never a ref to begin with. The
   answer was correct, the reason was entirely unrelated, and deleting the guard under test still
   passed.
3. **`cmp` of two empty files succeeds**: the line count is now asserted before the dual-track
   comparison, or a fixture that did nothing at all would report "a perfect match."
4. **Never reaching the line under test**: push's detached/corrupt-message branch sits after the
   remote ref advertisement; without a live remote it's never reached. The first version of the
   assertion passed simply by failing earlier at "remote not configured." Removed and recorded in
   the not-verifiable list below.

Also, **a duplicated string can make a mutation lie**: `"rebase (start): checkout %s"` existed in
two copies; a mutation without `/g` changed only the first copy, so "plain/continue/skip all stay
green, only fast-forward goes red" looked like insufficient coverage, when actually the
verification tool only changed half of it. Converged into one constant.

### Tooling: `tests/mutate.sh`'s name goes into an mktemp template

A mutation name containing `/` makes `mkdtemp` fail ("No such file or directory"). The script
fails loudly (exit 1), but the error message looks like an environment problem rather than "your
name is malformed" -- measured, this wasted two rounds before it was understood. Names are now
filtered to safe characters first.

### Deliberately maintained divergences

- ~~**`sg merge` and "an `sg rebase` started from detached HEAD" still reject**~~ --
  **allowed in Phase 19**, see the next section. The reasoning at the time still held: it was a
  scope decision, not something impossible, so it was pinned with interop rather than left to
  drift. Both the cost and the benefit of pinning it paid off in Phase 19: moving this boundary
  meant deleting those two pinned checks, and that deletion itself broke a third one along the
  way, precisely proving the drift they were originally guarding against was real.
- **`sg status` doesn't have git's `interactive rebase in progress` line.** Measurement found
  that real git prints "interactive" even without `-i` (from 2.26 onward, the merge backend reuses
  the interactive machinery); sg has no `-i`, so copying it verbatim would be misleading.

### Not verifiable (recorded honestly)

- The branch of `sg_ref_detach_description` that falls back to an abbreviated id when the buffer
  can't fit: the label length cap comes from `ref_path[SG_PATH_MAX]` (4096, and
  `sg_rev_parse_ref_path` already returns -1 for a name that doesn't fit), plus
  `"HEAD detached from "`, totaling 4116 bytes; both call sites' buffers are 4160 ->
  **unreachable given the existing call sites.** Two independent rounds of cold reading each did
  this same arithmetic separately. Not structurally dead code (it would trigger if a future third
  call site ever passed a smaller buffer), so the guard is kept, but recorded honestly as
  uncovered.
- `cmd_push.c`'s detached/corrupt HEAD message branch: its HEAD check sits after the remote ref
  advertisement, requiring a live remote to reach it. Changed, but no assertion.
- **The check for "`--abort` doesn't touch the branch's reflog" is guaranteed by the rule, not by
  a code choice guaranteeing it**: abort resets the branch back to the value it already had, and
  rule 1's no-op suppression blocks any write, so changing abort's own code **has no mutation that
  can turn it red** (one was tried). Kept as a regression guard for the shape, but it cannot be
  taken as evidence that "abort chose not to write."
- The interruption window itself, between `finish_rebase`'s two writes, and after detaching on the
  fast-forward path, can't actually be triggered in a test (no fault-injection mechanism). The
  approach was to **construct** the post-interruption disk state and then verify `--abort` can
  recover it -- that covers the recovery logic, not the existence of the window.
- phase18e's branch->detach case is ineffective coverage of the new "did the commit change" rule:
  when HEAD starts from a branch, `have_prev_commit` is always 0, so that `memcmp` is never
  reached at all. It's a regression guard for the refactor, not coverage of new logic -- the label
  is written honestly.

The final `tests/interop.sh` has 1098 checks (998 at the end of Phase 17), all 35 `make test`
binaries pass (`tests/test_head_detach.c` added), `make sanitize` is clean, and the subcommand
count stays at 24 (`sg switch` gained `--detach`).

---

## Phase 19: merge and rebase now also accept detached HEAD

Phase 18 turned detached HEAD into a first-class state, but deliberately stopped short of three
commands: merge, reset, rebase. reset was forced open by its own tests while Phase 18 was still in
progress (a paused rebase became detached, turning 7 existing interop checks red). The remaining
two are covered here.

Real git supports both, so there's no design freedom here at all -- the target behavior is
entirely dictated by measurement.

### Real git as oracle: measure first, then write

The first thing done was measuring the entire behavior of both commands under detached (git
2.55.0), storing it as a comparison table: HEAD file content, what got appended to `logs/HEAD`,
what got appended to the branch's reflog, stdout verbatim, and the shape of the commit object.
Four merge scenarios (true merge / fast-forward / up-to-date / commit after a conflict), five for
rebase (plain / continue after a conflict / abort / up-to-date / fast-forward), each also run once
more as a **branch-started** version to serve as the control.

Having a control group is the key part. Looking at detached's output alone gives no way to tell
"this line was there anyway" from "this line only appears under detached"; laid side by side, the
differences collapse to a small table:

| Aspect | Branch-started | Detached-started |
|---|---|---|
| rebase success message | `...updated refs/heads/topic.` | `...updated detached HEAD.` |
| rebase up-to-date | `Current branch topic is up to date.` | `HEAD is up to date.` |
| `logs/HEAD` finishing line | `rebase (finish): returning to refs/heads/topic` | **none** |
| branch reflog finishing line | `rebase (finish): refs/heads/topic onto <onto>` | **none** |
| `--abort`'s log | `returning to refs/heads/topic` | `returning to <orig-head 40-hex>` |
| state file `head-name` | `refs/heads/topic` | the literal `detached HEAD` |
| `rebase (start)` / `(pick)` / `(continue)` | byte-identical | byte-identical |

**The first pitfall hit had nothing to do with the program**: git prints in the system locale by
default, and what got measured was a translated line, not `Updating ...`. Message strings must
always be measured with `LC_ALL=C` pinned first, or the expected values copied into the tests
will be red on CI without fail. This is the same lesson from Phase 18's "the oracle's environment
must be declared" showing up in a different guise.

### A structural observation: this isn't a special case, it's a degeneration

Reading the table above as "detached needs seven more special cases written" is the wrong
reading. The correct reading is:

Phase 18's `finish_rebase` is two steps -- first update the branch (HEAD is still detached, rule 2
doesn't mirror it), then reattach HEAD (old==new, but `logs/HEAD` never applies no-op
suppression). **When there's no branch, each of these two steps has no target**: there's no branch
to move, and no HEAD to reattach (HEAD was detached the whole time, and has already stopped at
the correct commit). So `finish_rebase` just `return 0`s entirely when `branch == NULL`, writing
not a single byte -- and what real git measured out was exactly "wrote nothing at all."

merge works the same way: the branch path is "update the branch ref, and rule 2 incidentally
mirrors a line into `logs/HEAD`"; the detached path is "write HEAD directly, which records a line
by itself." Both paths end up with exactly one log line, in the right place, with not a single
line written specifically to match git.

This is the second time the two asymmetric rules from Phase 17 were used as a **tool** rather than
a constraint (the first was `finish_rebase`'s ordering in Phase 18). When a rule is well chosen, a
new state doesn't need a new rule.

### A sentinel: the disk needs to say "there's no branch here"

The sequencer state's `orig_branch` used to always be a branch name; `sg_rebase_state_read` judged
it corrupt if it couldn't read one. Starting detached needs a way to express "no branch," and this
expression must be kept separate from "corrupt."

Approach: memory uses `NULL`; disk writes the literal string `detached HEAD` -- the same sentinel
real git uses in `.git/rebase-merge/head-name`. It can't collide with a real branch name, because
both `sg_ref_name_valid_for_create` and real git's check-ref-format reject a ref name containing
whitespace, so a branch with this exact name can never be created at all.

**Deliberately not adopted: "the file doesn't exist = detached."** By the time a rebase has
reached a resumable state, it must have written that file, so a missing file means something
removed it. Reading absence as a valid state would launder data loss into normal operation -- the
same principle behind `sg_ref_head_is_detached` insisting on separating corrupt from detached.
Absence still returns -1, and there's a dedicated unit test pinning this.

### Two bugs, both caught by tests, neither a porting mistake

**A conflicted merge segfaults under detached.** `current_branch` was passed unchanged into
`sg_merge_trees` as `ours_label`, and that label gets formatted into the `<<<<<<< %s` conflict
marker. NULL going in means a crash, and a crash on the path a user is most likely to hit. The
fix computes "NULL -> `HEAD`" as an `ours_label` local variable, shared by the conflict marker, the
merge message body, and the summary line. A side effect is the marker becomes `<<<<<<< HEAD` under
detached -- exactly what real git always uses (sg uses the branch name on a branch, an existing
divergence pinned by phase4b, untouched).

**A corrupt HEAD gets blamed on the working directory.** merge's "the working directory must be
clean" gate runs before the HEAD gate, and comparing the working directory to HEAD naturally
requires reading HEAD first; with a broken HEAD, the comparison result becomes "every file is
new," so the user is told the working directory is dirty -- the one part that's actually fine gets
accused. The HEAD diagnostic was moved to the very front. **This is a pre-existing issue**, not
introduced this time: before Phase 19, merge printed the same "working directory dirty" for a
corrupt HEAD too; it's just that every detached HEAD was rejected earlier, so no test ever reached
this combination.

### The third bug: `(null)` hiding under an entirely green test suite

The cold read found one last unguarded `current_branch` in `cmd_rebase.c`'s fast-forward
shortcut:

```
Fast-forwarded (null) to master.
```

That path **did** have a test reaching it (phase19g's fast-forward case hit it precisely), and the
exit code was 0 with ref and reflog both correct -- because the test discarded stdout to
`/dev/null`. On this platform, `printf("%s", NULL)` just prints `(null)` without crashing, so all
three CI cells pass silently.

What's missing isn't one check, it's **an entire dimension.** 18a-18d once missed three
divergences purely because not a single check read `switch`'s stdout; this exact same shape
appeared again this time, just in rebase instead. Every newly added detached-specific message this
round now has a stdout assertion: the merge summary line, rebase's success line, the
fast-forward line, and the abort line.

One inference worth recording: **"high coverage" and "every dimension covered" are two different
things.** Phase 19's assertions on the HEAD file, reflog, ref, commit shape, and exit code were all
dense -- dense enough to look like there couldn't possibly be a hole -- and the hole was exactly in
the one column nobody was looking at.

### Mutation verification: going red isn't enough, it has to be red for the right reason

Seven directed mutations, six matched the prediction on the first try. The seventh
(`finish_rebase`'s `if (0)`) **was caught, but for the wrong reason**: taking the NULL branch
segfaults directly at `snprintf("refs/heads/%s")`, so rebase never got the chance to write an
extraneous finish line at all. The check specifically meant to verify "no finish line is written"
was therefore **always green** -- the mutation looked successful on the surface but verified
nothing at all.

Switching to a mutation that doesn't crash (having `finish_rebase` write a fake
`rebase (finish)` line into `logs/HEAD` for the NULL branch) actually turned it red for the right
reason, with the failure message `no 'rebase (finish)' line is written when there is no branch
(found 1)`.

The same discipline was applied to the just-fixed `(null)` bug: replanting the bug turns exactly
one check red, with the failure message printing `Fast-forwarded (null) to master.` verbatim.

One more mutation worth recording separately is the sentinel one: after the sentinel converged to
a single constant, changing it makes **both the read and the write change together**, so the
round-trip test stayed green -- only the assertion that directly reads the raw disk content
catches it. This is the flip side of Phase 18's "a duplicated string can make a mutation lie":
when there's only one copy of a constant, catching it requires an assertion that verifies the
format rather than self-consistency, which is exactly why that assertion was added.

### Convergence done in passing

`cmd_reset.c`'s `move_head_to` and `cmd_commit.c`'s inline version were two copies of the same
logic, and merge needed a third. Extracted into `sg_ref_move_head` (`include/sg/refs.h`). The
branch-vs-detached choice is easy to get wrong the same way twice, worth having in only one place.

### Deliberately maintained divergences

- **The conflict marker's ours label is still the branch name on a branch**; real git always uses
  `HEAD`. An existing divergence pinned by phase4b, untouched by Phase 19 -- only under detached,
  where there's no branch to use, does it land on the same `HEAD` as git.
- **Messages like `Fast-forwarded HEAD to <upstream>.` are sg's own wording**, not git's. sg
  already used its own sentences on a branch; detached just reuses the same wording, with no
  reason to copy git verbatim only here.

  This was asked about once during a cold read: of the five detached messages, why does only
  `Successfully rebased and updated detached HEAD onto '%s'.` say "detached," while the other four
  only say `HEAD`? It looks inconsistent, but the actual rule is **matched sentence-by-sentence
  against git**: `HEAD is up to date.` and `...updated detached HEAD` are both copied verbatim from
  git's own sentences (git itself says "detached" in one and not the other); the other three have
  no corresponding output from git at all -- they're sg's own sentences, where `HEAD` alone is
  enough, since the absence of a branch name already conveys "detached." Unifying them into the
  same wording would take the first two out of alignment with the oracle -- a cost bigger than
  surface-level consistency.

### Not verifiable (recorded honestly)

- **Recoverability if a detached-started rebase is interrupted between "state finished writing"
  and "HEAD not yet detached"** has no test coverage. A shell can't precisely kill it at that
  exact point, and Phase 18's substitute technique (constructing the post-interruption disk state
  and verifying `--abort` recovers it) has no counterpart here, because the detached path's
  `finish_rebase` writes nothing at all -- there's no "between two writes" window to construct in
  the first place. Logically self-consistent (traced line by line during a cold read), but backed
  only by manual reasoning.
- **`sg_ref_branch_name_is_safe` doesn't reject whitespace**, so manually editing
  `.git/sg-rebase/orig-branch` to `Detached HEAD` (different capitalization) would be treated as a
  legitimate branch name rather than judged corrupt. Only reachable by directly tampering with the
  disk, unreachable through any `sg` command; that function's purpose was always path safety, not
  full validation. Not a regression from Phase 19, but the sentinel mechanism added a new place
  that depends on it not misjudging -- noted.
- **`sg merge --abort` under detached** now has interop coverage (phase19c), but it never had a
  detached gate to begin with -- previously it was theoretically reachable but no test ever
  exercised it.

The final `tests/interop.sh` has 1165 checks (1098 at the end of Phase 18), all 35 `make test`
binaries pass, `make sanitize` is clean, and the subcommand count stays at 24.

---

## Phase 20: `sg stash`'s `-u`/`-a`/`--keep-index`/`--index`, and relaxing apply/pop's clean gate

Phase 15 shrank `sg stash` down to a core subset; the README at the time listed four unimplemented
items: `-u` (untracked files), `--index`, `--keep-index`, and pathspec. This phase does the first
three; `sg_stash_push` also switched from a run of positional arguments to accepting
`sg_stash_push_opts` (`include/sg/stash.h`). **Deliberately not done**: `stash show`, pathspec, or
any `sg diff` changes -- `show` needs tree-vs-tree diff and `--stat`, which is scope for the next
batch and untouched here.

### `--keep-index`: swap the target tree of the reset, not a whole new code path

Real git's `--keep-index` resets the working directory to **the current index** rather than HEAD,
leaving the index itself untouched. This was measured with a five-state fixture
(unstaged-modify / staged-modify / staged-new / staged-delete / worktree-delete); `staged-delete`
is the one cell that moves in the opposite direction from the other four, so both directions each
got their own assertion.

The implementation is **two chained calls to `sg_apply_tree_to_workdir`** (first reset to HEAD,
then apply the index's tree), not a single call pointed directly at the index tree. A single call
looks equivalent and describes the same final state, but `sg_apply_tree_to_workdir` decides
"should this file on the working directory be deleted" by whether **the index read at the moment
of the call** lists it, and a path staged as a deletion isn't listed by either index (the one at
call time, or the target index tree), so a single call would leave it sitting on disk untouched.
Chaining makes the second call's baseline HEAD's tree (which does list this path), so the first
call cleans it out. This is also how real git itself implements this flag.

### `-u`/`-a`: a third parent, and it's written unconditionally

The enumeration of untracked files was moved from `cmd_status.c`'s `collect_untracked` into
`workdir/status.c`, renamed `sg_status_list_untracked`, with an `include_ignored` switch added
(status and `-u` pass 0, `-a` passes 1), and its return value changed to be sorted --
`sg_tree_build`'s flat-list contract requires sorted input, and the first version fed it
`readdir`'s order directly, which was wrong. `sg_tree_build_from_untracked`, which builds the
corresponding tree, was added to `tree_build.h` alongside the other two tree-building functions.

Rules measured against real git 2.55.0:

- The stash commit grows a third parent: a **root commit with no parent of its own**, whose tree
  holds only untracked files (`-a` includes ignored ones too). **The stash's own tree (the first
  parent) is byte-for-byte identical to when `-u` isn't given** -- this can't be seen from `-u`'s
  output alone; it's a conclusion that only collapses out from diffing the tree id against a
  control run without the flag, side by side.
- As long as there's anything to stash, the third parent is **created unconditionally**, even if
  the untracked list is empty (an empty tree gets written). An optimization of "fall back to two
  parents when the list is empty" would produce objects that don't match real git -- real git
  really does write that empty tree.
- `-m` doesn't affect the second and third parents' subjects, only the first.
- Under `-u`/`-a`, the "nothing to save" determination must also look at the untracked list
  (filtered through the same filtering rules the third parent uses): if the working directory is
  clean except for one untracked file, that counts as nothing-to-save under plain `push`, but
  **doesn't** under `-u`/`-a`.
- After removing the files already gathered into the third parent, `-u` deletes the empty
  directories left behind, but **skips empty directories that are themselves ignored**; `-a`
  deletes both (since `-a` already gathered up ignored files together, there's nothing left in the
  emptied directory that any ignore rule would want to protect). This pruning step's ignore
  engine is opened **before fetching the files, not after**: `.gitignore` itself is usually
  untracked, and `-u` would sweep it up too; if the pruning stage opened the ignore engine one step
  too late, it would see an empty rule set and end up deleting even the ignored empty directories
  that should have been kept. Only a unit fixture that keeps `.gitignore` untracked covers this;
  the interop fixture had already committed it, covering only the guard's existence, not this
  timing blind spot.

### apply/pop: narrowed from "the entire working directory must be clean" to "paths actually
touched can't be dirty"

The old clean gate required the entire working directory and index to match HEAD. The new rule
(`sg_stash_apply_check_dirty`) only looks at **the paths this merge will actually touch** (base =
the stash's first parent's tree, ours = HEAD's tree, theirs = the stash's tree):

- the path's content in the working directory differs from HEAD -> blocked (including the case
  where "the content happens to be identical to what the stash would write" -- the check is
  "does it deviate from HEAD," not "does it deviate from the stash");
- the path in the index already deviates from HEAD -> blocked;
- the path **was deleted** from the working directory -> **not blocked** (nothing would be
  overwritten);
- paths this merge never touches aren't looked at at all, no matter how dirty.

This rule by itself depends on a change to `sg_merge_result_apply` (see the next section) to honor
its promise that "untouched paths stay exactly as they are."

### Two deliberate divergences from real git

1. **`-u`'s pop collision**: when real git hits a collision on the untracked half, the tracked
   half still gets applied while the untracked half fails file by file, leaving the entry on the
   stack -- leaving behind a state with no way out, "the entry is still there, but the file it
   would restore is already fighting with itself on disk" (popping again hits the same collision
   again). sg folds the untracked half's collision check into the existing pre-check (which already
   covers both the tracked half and the `-u`/`-a` untracked half), rejecting the whole apply
   all-or-nothing -- safe, at the cost of not matching real git's partial application.
2. **A dirty apply colliding with staged changes**: real git's `ours` is the index, so it can
   three-way merge the stash into an already-staged change (potentially producing a conflict); sg's
   `ours` is HEAD, and allowing this here would directly overwrite already-staged content, so sg
   rejects it -- the strictly safe side, at the cost of not matching real git's leniency in this
   cell.

### The change to `sg_merge_result_apply`: this phase's single widest-reaching change

It used to unconditionally write every clean entry from `sg_merge_trees` back to the working
directory. As long as "the working directory must be clean" remained a hard precondition, this
unconditional write-back was invisible -- the content written back was byte-for-byte identical to
what was already on disk. The moment a dirty working directory is allowed through, it turns from
harmless into destructive: it would overwrite, in place, dirty content on disk that has nothing
to do with this merge.

The rule now is: skip entries whose result equals ours (HEAD), without re-reading the object or
rewriting the disk. "Whether it touches ours" was factored out into a standalone function,
`sg_merge_entry_touches_ours` (`include/sg/merge.h`), so stash's index-finishing rule (next
paragraph) and merge/rebase's working-directory rule share the same definition -- the direction
two independent implementations would drift is exactly "the gate let a path through, but the
finishing logic overwrites it anyway," and only one shared definition prevents that divergence.

**`add_resolved_entry` still runs unconditionally**, unaffected by this skip rule: both
`cmd_merge.c` and `cmd_rebase.c` build the commit's tree from the in-memory index this function
constructs, so a missing path means a missing file in the commit -- this is the index half;
`sg_merge_entry_touches_ours` governs whether the working-directory half gets written to; the two
aren't the same thing.

Two side effects worth noting: `sg merge`/`sg rebase` no longer rewrite the entire working tree
every time (including reassembling chunked large files), only touching paths that actually
changed; and the case "both sides delete the same path" no longer silently `unlink`s an unrelated
untracked file that happens to share the name (the old version unconditionally called delete for
every "deleted" result, regardless of what that path on disk actually was).

The index half needs **the same rule stated separately**: for a stage-0 entry whose path HEAD
never had and this merge never touched, if the merge result has no corresponding entry to catch
it, the old version would let it silently fall out of the index (becoming unstaged). stash's
apply/pop route around this with their own dedicated second scan (over the pre-apply index) for
"paths that weren't touched," restoring this guarantee, rather than relying on
`sg_merge_result_apply` to handle it itself -- its input is `sg_merge_trees`'s result, and it has
no idea what the caller's prior index looked like at all.

### Lessons from this round

- **The oracle gives an end state, not a mechanism.** The five file states measured for
  `--keep-index` mapped perfectly onto "reset the working directory to the index's tree," so the
  spec was written as "just swap one parameter." Only during implementation, checking
  `sg_apply_tree_to_workdir`'s deletion loop, was it found that it computes **the index read at
  call time** minus the target tree -- staged-delete's path isn't in either index, so the loop
  never considers it at all, and the file stays put -- exactly the opposite of what was measured
  for that cell. The correct fix is the two chained calls described above, also how real git
  itself implements it.
- **A test can be "caught by a mutation while verifying nothing at all."** Making
  `remove_untracked_files` fail unconditionally turned two tests red -- but the reason was
  `-u push failed`, i.e. **the setup itself broke**, not that the semantics of the `-2` return
  value were being verified. The new test was therefore changed to assert "the return value is
  exactly `-2`," not "non-zero."
- **A blind spot needs a fixture that is the guard's sole reason for existing to reveal it.**
  Removing `prune_empty_untracked_dirs`'s ignore guard, both `make test` and interop stayed all
  green -- because neither fixture had a directory that was both **ignored and empty**. Adding one
  also incidentally revealed a real bug: `.gitignore` itself is often untracked, `-u` would sweep
  it up too, and if the pruning stage's ignore engine opened after that, it would see no ignore
  rules at all (details in the `-u`/`-a` section above).
- **`make test` stops at the first failing binary**, so a mutation result can hide a test that
  never even ran -- actually hit this phase: `test_merge_result_apply` failing prevented
  `test_stash` from running at all, which at a glance looked like stash had no regression, when
  really it was never even asked.
- **When changing the landing path shared by merge/rebase/stash, `make test` green doesn't
  count.** Measured: after making `add_resolved_entry` skip along too (mistakenly treating the
  index-half rule as the same as the working-directory-half rule), not a single `make test` check
  went red, but 10 interop checks did (mostly rebase's phase4c / 17 / 18f / 19a) -- this
  project's rule that "only interop catches merge/rebase regressions" held up firmly again.

### Two things deliberately not fixed this phase, left for later

1. **`sg_tree_build_from_workdir` can't represent a deletion in the working directory.** For a
   tracked file that's now missing from the working directory, it falls back to the blob in the
   index (`include/sg/tree_build.h`'s header comment states this is deliberate, to uphold the
   contract that "every entry resolves successfully"). Consequence: `sg stash push` with only one
   tracked file deleted and everything else clean prints "No local changes to save" and creates no
   stash; real git records the deletion itself into the stash's tree, and the file stays deleted
   after popping (this divergence has been measured and confirmed). It has two call sites --
   `stash push` and `snapshot` -- and for snapshot, that fallback is actually correct (a safety net
   needs to be able to restore everything, including "back to before the deletion"). A fix must
   give the two call sites different behavior, which is a design decision, not a patch, left for
   the next batch.
2. **`restore_untracked_flat` builds paths with a raw `snprintf`, with no truncation check.** Its
   paths come from the stash's third parent's tree, which may have been written by real git and
   isn't protected by `sg_status_list_untracked`'s internal `path_join` truncation guard. But the
   existing `sg_apply_tree_to_workdir` uses **the exact same raw `snprintf`** for the tracked half,
   so this is an existing project-wide practice (though unsafe); patching only this one function
   would become "a class of gap only half-fixed." If it's going to be fixed, fix the whole class at
   once, don't open a special case here first.

The final `tests/interop.sh` has 1247 checks (1165 at the end of Phase 19), all 37 `make test`
binaries pass (`tests/test_status_untracked.c` and `tests/test_merge_result_apply.c` added),
`make sanitize` is clean, and the subcommand count stays at 24.

## Phase 21: representing deletions in the working directory, and a batch fix for path truncation

Continuing from Phase 20's closing "two things deliberately not fixed, left for later." That
section is a record of the decision made at the time, unchanged; here records how each of the
two was handled, plus a third thing only discovered while fixing them.

### The symptom was one level more severe than what was recorded at the time

Phase 20 recorded "when only a single tracked file is deleted, it prints `No local changes to
save` and creates no stash." Measurement found that was only the lighter of two symptoms:

| Scenario | Real git | sg before Phase 21 |
|---|---|---|
| Only one tracked file deleted | Creates a stash, the tree omits that file | Prints `No local changes to save`, creates no stash |
| Deletion + another file modified | Creates a stash, the tree omits that file | Creates a stash, but the tree **still has that file**; the file **comes back** after `pop` |

The second row is the real data loss: it's not "something wasn't saved," it's that after one
round of stashing, **the user's deletion gets silently undone**, exit code 0, no warning at all.
Only the first row was recorded at the time because that's what was observable from `stash push`
alone; the second row only becomes visible after running push+pop through a full round.

### The fork between two call sites: a mandatory enum, no default value

`sg_tree_build_from_workdir` now takes an `sg_workdir_missing`:
`SG_WORKDIR_MISSING_KEEP_INDEX_BLOB` (for `sg_snapshot_create`) and
`SG_WORKDIR_MISSING_RECORD_DELETION` (for `sg_stash_push` building `worktree_tree`). **Note that
both semantics are used within a single `sg stash push`** -- it's also a caller of
`sg_snapshot_create` itself, and that safety net needs the former.

An options struct was considered (Phase 20's `sg_stash_push_opts` is the precedent), but that
would necessarily carry a "NULL means default" convention -- **using a shape that institutionalizes
a default value to fix a bug caused by a default value is backwards.** There's only one axis and
two call sites here, so a mandatory enum has each caller state which one it wants. `= 0` maps to
the conservative value, so any accidental zero-initialization lands on the "don't delete anything"
side.

### "Exists but can't be read" becomes a hard failure: `sg_snapshot_create`'s contract changes with it

Previously, a failed `sg_read_file` just fell back to the index blob, not distinguishing "the file
was deleted" from "permission denied / became a directory / I/O error." Under the new semantics
these must be separated: for a genuine deletion, omitting the path is correct; for an I/O error,
omitting it means losing the file from the stash.

Both policies now hard-fail. This applies to `KEEP_INDEX_BLOB` too, because the old behavior was
**silently writing a stale blob and letting the destructive operation proceed anyway** -- a
snapshot that claims to be recoverable but whose content is actually stale is worse than no
snapshot at all. So `sg_snapshot_create`'s contract changed from "every entry resolves
successfully" to "resolves successfully, or the snapshot is rejected"; its 9 upstream call sites
translate this into "the operation is rejected."

The classifier's ordering is the key part: **`lstat` must always come after `sg_read_file` fails,
never a pre-probe**:

- The common race is "deleted after probing, before reading." A pre-probe would classify this as
  "exists but can't be read" -> a harmless race would blow up the entire stash/snapshot. The
  post-hoc classification treats it as "not there," which is true at the moment of
  classification; only the rare opposite direction (the file appears after the read failed) hard-
  fails. Neither eliminates the race, but this ensures the rare direction is the one bearing the
  hard failure.
- Zero extra syscalls, and the success path is entirely untouched.
- **`sg_read_file`'s own errno isn't usable** (`workdir.c`: `free()`/`fclose()` run before it
  fails, and a malloc failure and an EIO share the same `return -1`). The post-hoc `lstat` uses
  its own errno, so `sg_read_file`'s signature doesn't need to change.
- Using `lstat`, not `stat`, matching the exact test `stash.c`'s dirty gate uses for existence --
  if the push side and the gate side define "this file is gone" differently, push could record a
  deletion while the gate still believes it's there.

The `tests/test_snapshot.c` test that verifies the fallback **stayed all green with zero words
changed**, because its `b.txt` was never created (ENOENT) -> classified as "not there" -> falls
back to the index blob. This property is itself a form of verification: if that test needed
changes to pass, the classification would have been written wrong.

### A third thing found only while fixing this: empty parent directories were never cleaned up

`apply.c` and `merge.c` are the **only two** places sg ever deletes a tracked file, and both just
`remove()` the file -- there is no `rmdir` serving this path anywhere in `src/`.

**This gap only became reachable for the first time this phase**: before the fix, `worktree_tree`
necessarily included every path in the index, so a stash apply's `deleted` entry could only ever
come from "base has it, both ours and theirs don't" -> `sg_merge_entry_touches_ours` returns
`ours_present == 0` -> `merge.c` skips `remove()` outright. In other words, **`sg stash pop`
before Phase 21 never actually deleted a single file**; the new semantics were the first time it
ever would, and that was also the first time this gap was ever hit. Another instance of
"relaxing a precondition silently invalidates an old rule" -- the old rule didn't error, it just
no longer covered the newly reachable case.

`sg_prune_empty_parents` (`include/sg/workdir.h`) was added, a purely best-effort `rmdir` walking
up the ancestor chain up to (not including) repo_root. Three boundaries measured (real git
2.55.0):

- A directory that's **covered by an ignore rule**, made empty by the deletion -> **cleaned up**.
- A directory still holding anything at all (including ignored files) -> left alone.
- A nested `a/b/c/t.txt` deleted -> `a`, `b`, `c` all cleaned up in sequence.

Warning: **the first bullet is the opposite of `prune_empty_untracked_dirs`'s rule**: that function
is ignore-aware, and **spares** a directory that's both empty and ignored (the interop `build/`
check guards this). The two prune functions deliberately behave oppositely with respect to
ignore, as stated in the header comment -- **don't "unify" them.**

### The second item: a batch fix, not just patching one function

Phase 20 recorded two places, `restore_untracked_flat` and `sg_apply_tree_to_workdir`, with the
instruction "if it's going to be fixed, fix the whole class at once." The actual survey found
**16 sites**, spread across `merge.c` (the landing function shared by merge/rebase/stash),
`stash.c`, `tree_build.c`, `cmd_add.c`, `cmd_restore.c`, `status.c`, `cmd_diff.c`.

The fix extracted a `sg_path_join` into `include/sg/workdir.h` rather than hand-writing 16
separate checks -- the latter would just create a 17th and 18th copy while fixing duplication.
It also absorbed two verbatim copies of `path_join` in `status.c` and `cmd_add.c`; `SG_PATH_MAX`
was consolidated into the header from 14 separate `.c` files' own `#define`s, along with two
independently named constants of the same value, `SG_TREE_BUILD_PATH_MAX` and
`SG_REVPARSE_PATH_MAX`, and 36 bare `4096` literals.

**The meaning of truncation is decided per category, not uniformly**:

| Category | Handling |
|---|---|
| Write/delete | Never skip (`rc = -1` and continue / report an error and fail that entry / `return -1`) |
| Gate | Fall the conservative way: dirty checks flag dirty, collision pre-checks flag collision. **A gate's failure direction can never be "allow it through"** |
| Report | Return -1 for the CLI to print; must never silently drop a file from `sg status`/`sg diff` |
| Cleanup (best-effort) | Keep silently skipping |

`tree_build.c` is the only site that must fail **before `sg_read_file`**: its loop treats an
unreadable file as "the file isn't there," and if a truncated path just happens not to exist, an
existing file would get recorded as deleted. This site couples two things together, so
truncation has to be handled before the semantic fork.

`would_lose_content` (`cmd_restore.c`) therefore grew a third state: it used to just return
yes/no, and under truncation **both answers are lies** -- returning 0 would let `sg restore`
overwrite content with no warning at all.

Deliberately not changed: the buffers that build paths from `git_dir` + a fixed-length hex
(`loose.c`, `rebase.c`, `cmd_clone.c`, etc.) only had their `#define` swapped, not routed through
the helper -- their risk profile is different, since an overflow would require `git_dir` itself to
be extremely long. `prune_empty_untracked_dirs` kept its own inline check, since its convention
is to silently skip while the helper's callers report; its original comment claimed consistency
with `status.c`'s `path_join`, which was **wrong** (`collect_untracked` prints a warning when it
skips), and has been changed to state the real reason: an uncleaned empty directory is invisible
to both git and sg.

### Verification: six directed mutations, all red for the right reason

`sg_tree_build_from_workdir` had zero test coverage before, and `sg_path_join` had no direct
test either. `tests/test_path_join.c` and `tests/test_tree_build_workdir.c` were added, with
every assertion first proven to go red:

| Mutation | Named checks turned red |
|---|---|
| RECORD_DELETION collapsed into KEEP | "the two policies produce the same tree", "the deletion isn't represented", "the empty tree", "the leftover empty subtree" |
| The reverse collapse | **`test_snapshot`'s existing guardrail**: `b.txt` disappears from the snapshot tree |
| Removing the "can't read" hard failure | Each policy's own hard-failure assertion |
| prune only cleaning one level | "`a/b` should be pruned", "`a` should be pruned", with `a/b/c` staying green |
| Removing the truncation check | Four truncation assertions + `out_size 0` |
| An off-by-one in the truncation check | **Only the boundary one** |

The last row is the most valuable: it turns exactly one assertion red, proving the boundary test
isn't a redundant copy of a coarser one. The reverse-collapse row proves `test_snapshot`'s
guardrail really was guarding the old semantics.

`sg_path_join`'s test uses a **small `out_size`** rather than a real deep directory, and this isn't
laziness: macOS's kernel PATH_MAX is 1024, far below `SG_PATH_MAX`, so a real path long enough to
truncate the 4096 buffer would be blocked by the kernel before it ever got there, and that branch
would never run at all. A test written with real directories would pass locally, but pass for a
reason unrelated to the code under test.

The mutation pattern needs a closing parenthesis: `(size_t)n >= out_size` would also
prefix-match `out_size - pos` elsewhere in the same file, and a mutation that only changed half of
it would read like "already verified."

### interop: putting back the checks that had to be worked around at the time

To avoid this bug, Phase 20 maintained an extra, narrower `p20_index_fixture` (excluding
`wt_del.txt`/`staged_del.txt`). After the fix, the two fixtures were merged, and the `--index`
check group now runs against **the exact case that had to be avoided at the time** -- the
strongest possible form of regression evidence.

After merging, two `--index` checks went red, and **genuinely so**: sg deletes `staged_del.txt`
from disk, while real git keeps it as an untracked file. Measured against a control **binary
built from `1afef8b` (the commit before this phase)**, the behavior was identical -- so this is
**a pre-existing divergence, not caused by this phase**, just previously hidden by the narrower
fixture. The cause is the "sg's ours is HEAD, not the index" divergence already recorded and
deliberately kept in Phase 20: after `git rm --cached`, the file is still on disk, and sg's apply
only looks at HEAD, concluding ours has it and theirs doesn't -> delete it.

The handling was to exclude that path from the byte-for-byte comparison and **pin the divergence
itself with two checks** (real git keeps it / sg deletes it), rather than silently filtering it
out -- if the behavior ever changes, a check would still go red. **The expected value was not
adjusted to make it green** -- that's the worst failure mode for this project.

One of the newly added phase21 checks deserves specific mention: **it asserts stdout is not
`No local changes to save`.** In the "deletion only" scenario, this bug's entire symptom was a
single line of stdout -- files, refs, and reflog showed no difference at all. Phase 19 already
paid the price once for "verifying only files and reflog misses an entire dimension of output."

### A newly reachable divergence, proactively recorded

Stashing an unstaged deletion -> then staging a deletion of the same path -> popping again:
`sg_stash_apply_check_dirty`'s index check rejects it (`idxpos < 0 && hf != NULL`), while real git
doesn't. This is a new combination of the same "ours is HEAD, not the index" divergence, previously
unreachable because stash could never record a deletion at all. An interop check pins sg's own
rejection behavior, deliberately not comparing against real git -- writing this down instead of
leaving it to be discovered as "interop suddenly went red."

The final `tests/interop.sh` has 1276 checks (1247 at the end of Phase 20), all 39 `make test`
binaries pass (`tests/test_path_join.c` and `tests/test_tree_build_workdir.c` added), `make
sanitize` is clean, and the subcommand count stays at 24.

### Caught by the wrap-up cold read: prune could walk outside repo_root

The tail batch (the semantic fork, prune, tests, interop) was produced only after the first
review round handed off, so no one had ever cold-read it -- it was handed off for a **tail-only**
review a second time. It caught a substantive problem: `sg_prune_empty_parents` placed no
constraint on `relpath` at all, while its header comment promises "up to but never including
repo_root."

**Measured (not reasoned about)**:

- `relpath = "../sibling/f.txt"` -> the very first round strips `cur` down to `"../sibling"`,
  `sg_path_join` produces `repo_root/../sibling`, which the OS resolves to the repo's **sibling
  directory**, and `rmdir` really did delete it.
- `relpath = "/f.txt"` -> after stripping, `cur` becomes an empty string, and `sg_path_join`'s
  semantics for an empty `rel` are "just return base," so `absdir` becomes `repo_root`, and
  **`rmdir(repo_root)` gets called.** In a real repo, `.git` makes it non-empty so this would fail,
  but a probe using an empty directory found repo_root really did get deleted when measured --
  what stopped it was `.git` happening to exist, not the code.

The fix added a `relpath_is_confined` function: rejecting absolute paths and paths with a `..`
component. **Why the caller's path can't be assumed clean**: these `relpath`s come from index
entries or a merge result, ultimately from a tree object, and `src/object/tree.c` does **no
validation at all** when parsing entry names. So "will never walk above repo_root" must be
enforced here, and can't rely on upstream.

The guard deliberately only blocks these two classes: a **normal directory name** that happens to
start with a dot, like `...dots`, must still be pruned as usual, and there's a test specifically
pinning this -- when a mutation changed the guard to reject indiscriminately, that test went red
alongside the other four normal-prune checks, which is the evidence that "the guard isn't
over-strict."

**A larger gap, not fixed this phase**: the same unvalidated path is also used by the adjacent
`remove(abspath)` (`apply.c`, `merge.c`), which predates Phase 21. In other words, the fact that
"a tree object's entry name can be an arbitrary string" affects more than just prune. Path
confinement as a batch (rejecting `..`, absolute paths, a `.git` prefix) should be done at the
layer that **parses trees / writes the index**, not patched separately at every consumer. Recorded
here, not addressed this phase.

The length boundary has a separate test using a relpath of **exactly `SG_PATH_MAX`** in length
(one byte shorter would still fit, so that's the actual boundary). A mutation changing `>=` to `>`
was caught, but note **what caught it wasn't the named assertion, it was the binary trapping
directly (exit code 133)** -- `strcpy` overflowing by one byte was caught by the platform's stack
protection. `mutate.sh`'s "a non-zero exit code counts as caught" happens to cover this case, but
what it proves is "the overflow really happened," not "that assertion is guarding it."

## Phase 22: path confinement for tree entry names

Phase 21's wrap-up noted "path confinement should be done at the tree-parsing/index-writing
layer," and this phase did it -- but half the premise going into it was wrong, and the actual
holes numbered two more than expected.

### Correcting the premise: validation isn't zero

`src/object/tree.c` and `src/index/index.c` really do have zero validation on names, but the
actual guard has always lived in `src/workdir/tree_build.c`'s `entry_name_is_safe` (present since
Phase 2), which blocks empty strings, `.`, `..`, and any name containing `/`, checked at
**every level**. So **path traversal has always been blocked.**

The hole was `.git`: no slash, not `.`, not `..` -- it just happened to slip through the gap
between the three rules.

### Four holes confirmed by measurement

| # | Scenario | Before the fix |
|---|---|---|
| 1 | A tree entry named `.git`, `sg reset --hard`/`switch` | **Exit code 0, writes out `.git/hacked.txt`** |
| 2 | `sg add d/.git/evil` | **Actually gets staged into the index** (real git silently ignores it) |
| 3 | Index has `.git/hooks/evil`, `sg restore` it | **Writes PWNED into the real gitdir, exit code 0** |
| 4 | `.g<U+200C>it` (a code point HFS+ folds away) | Accepted (real git rejects it) |

Item 2 makes item 1's fix **mandatory rather than optional**: sg can construct a malicious tree by
itself, and only patching the read side would leave a user who once ran `sg add .git/x` with a
repo that sg itself can never switch back into again.

Item 3 is worse than item 1: item 1 only writes into `.git/`, while item 3 **writes
attacker-chosen content**, and the entry point is a single `sg restore` command the user typed
themselves.

### Where the guard lives: one guard per source, not the same rule scattered three times

**Not in the parsing layer.** Real git's object store accepts a malformed tree as-is, and
`cat-file -p` can read it; the defense lives in `read-tree`. Putting it in the parsing layer would
make `sg cat-file -p <bad tree>` fail, which is exactly the capability most needed during
diagnosis; and `storage/chunk.c` and `cli/cmd_push.c` **only take the sha1, never look at the
name at all**, so blocking there would fail paths that have no attack surface whatsoever.

**Not at the filesystem chokepoint.** This isn't just redundant, it's **wrong**:
`src/storage/refs.c` uses `sg_write_file_mkdirs` exactly to write ref files into
`.git/refs/...`. A guard that rejects a `.git` component installed there would kill ref writing
outright. Whether the rule is correct depends on which namespace the caller is in, and that
information is already lost by the time it reaches `sg_path_join`.

So it's **three sources, three guards**: tree bytes (`sg_tree_flatten`), index entries
(`apply.c`'s `remove()`, `cmd_restore.c`'s writes), and argv (`cmd_add.c`). The test for
redundancy is executable: **deleting any one guard leaves a set of inputs only it could have
blocked.** Phase 21's `relpath_is_confined` is a strict subset of the new predicate, so it was
**converged** away rather than kept in parallel.

### Traversal's skip logic **cannot** use the same predicate (this instruction in the spec was
wrong)

The spec originally asked for "while we're at it, also switch `cmd_add.c`/`status.c`'s `.git`
skip to use the new predicate." Measuring real git overturned this: **git lists `.git.` as an
untracked directory** (`?? .git./`), only rejecting it at `add` time. Unifying the predicate would
make `sg status` **fail to report** a path git does report -- and a status listing silently
dropping a file is one of the hardest classes of bug to notice.

Reverted to `strcmp(ent->d_name, ".git") == 0`. The gitdir sg creates is always literally named
`.git`, so an exact comparison never misses it; other directories with the same name are ordinary
directories, listed as normal, and only rejected when they're **actually about to enter the
index**. The header comment states plainly that this predicate doesn't apply to traversal.

### The rule set (all measured, not recalled)

**Rejected**: `""`, `.`, `..`, anything containing `/`, any case variant of `.git`, forms with a
trailing `.`/whitespace, and names that equal `.git` once HFS+ ignorable code points are folded
away.
**Accepted**: `.gitignore`, `.gitmodules`, `..a`, `a..`, `git~1`, names containing control
characters.

Case folding **is hand-written as ASCII-only, not `strcasecmp`**: it's locale-dependent, and
under a Turkish locale `'I'` doesn't fold to `'i'`, which would let `.GIT` slip through **on
exactly the filesystem this rule exists for.**

**The ignorable code points are 16, confirmed one by one through measurement** (U+200C..U+200F,
U+202A..U+202E, U+206A..U+206F, U+FEFF). **The control group matters here**: U+200B, U+2060,
U+00A0, U+3000 are equally invisible characters, and real git **accepts** them. Writing "all
zero-width characters" from intuition would err in the direction of **rejecting legitimate
filenames**, and that direction has no symptoms at all.

### Three deliberate divergences

1. **`/` inside an entry name**: real git accepts it (expanding it into nested paths), sg rejects
   it. The reason **isn't security, it's correctness**: `sg_tree_flatten`'s output gets treated by
   `flat_find` as an array sorted by path for **binary search** (`include/sg/status.h` explicitly
   documents this invariant), and an entry whose name contains `/` would participate in the
   sorting using its own bytes; once expanded, the path is no longer sorted, and `flat_find` would
   **silently miss lookups.** Relaxing this would require re-proving the sort invariant, in
   exchange for accepting a kind of tree that even `git write-tree` itself can never produce.
2. **`git~1` (an NTFS 8.3 short filename) is not blocked**: sg only supports macOS/Linux, where
   8.3 short filenames don't exist, and `git~1` is a perfectly legitimate ordinary filename;
   blocking it would be a pure false positive.
3. **`sg add` returns exit code 1**; real git silently exits 0. The end state on disk is the
   same. `sg add` already has all-or-nothing semantics, and following that convention is better
   than carving out a special case for this.

### Control characters aren't in scope this phase (Phase 23)

Real git **accepts** control characters into a tree, with the defense living at the **display
side** (C-style escaping), and measurement showed `core.quotePath=false` **can't turn it off** --
`quotePath` only governs bytes >= 0x80, control characters are quoted unconditionally. So
rejecting them in name validation would make sg unable to sign a repo real git considers fully
legitimate -- a new compatibility hole, not a fix. The correct shape is a shared quoting function,
which **cannot** reuse `sg_print_remote_text` (it replaces control characters with `'?'`, lossy
and different from git's output).

Also worth noting, to shrink the threat surface and preempt anyone later thinking git's
`has_dirs_only_path` check was missed: **sg never creates symlinks** (`cmd_add.c` warns and skips
on `S_ISLNK`, `apply.c` writes mode 120000 as a plain file), so git's "a path must not walk
through a symlink" check has no corresponding attack surface in sg.

### The engine's verification is against real git, not against its own tests

Feeding 42 hostile names into both `sg_quote_path` and
`git -c core.quotepath=false ls-files` produced a **byte-for-byte `cmp` match, exactly**. Covering
all control bytes 0x01-0x1F, DEL, backslash, double quote, space, every shell metacharacter,
single quote, a u-diaeresis (U+00FC), CJK text, emoji, and the `0x01` followed by
`'7'` octal-reinterpretation trap.

The method can be reused, but there's one pitfall: **the name list must be NUL-separated, not
newline-separated.** The first time the list was written with newlines, a name containing an LF
got split into two entries, producing a "false divergence" in the comparison -- almost misread as
an sg bug when it was actually the comparison tool that was broken. Approach: python generates
`name\0name\0...`; a small program reads stdin, calling the quoting function entry by entry, then
compares against `git ls-files`'s output with `sort | cmp`.

This is far stronger than running the engine's own unit tests: unit tests verify "I believe git
does this," while this comparison verifies "git actually does this."

### Verification: fifteen mutations, four of them reversed

The four reversed ones are the hardest-won part of this phase -- they verify the rule is **not**
too broad, and over-broad rules normally show no symptoms at all (every test stays green, only
legitimate filenames get rejected):

| Reverse mutation | Result |
|---|---|
| The predicate changed to a prefix match | **14** checks turned red, including a large number of existing phase20 ignore checks |
| The ignorable code points list gained one more, U+200B | **exactly the control-group case** |
| flatten's `-2` changed to `-1` | **only the message assertion goes red, filesystem assertions stay entirely green** |
| Traversal's skip reverted to the overly broad predicate | **only that one false-negative check** |

The third row is especially worth recording: it proves the message assertion does **independent**
work, and the filesystem protection **doesn't depend** on the `-2`/`-1` distinction. Without this
pair, "rejected" and "named the path" would be two ways of saying the same thing.

### Three limitations recorded honestly

1. **`sg_relpath_is_safe`'s length boundary was caught by the platform's stack protection, not by
   an assertion**: changing `>=` to `>`, `memcpy` overflows one byte and aborts outright (exit
   code 134), never even reaching the CHECK statement. `mutate.sh` treating a non-zero exit as
   "caught" is correct, but what it proves is "the overflow really happened," not "that assertion
   is guarding it."
2. **The Unicode batch was only cold-read by the primary conversation**: it didn't exist yet when
   the reviewer read the diff, and the fix produced after that handoff was likewise never
   cold-read by a second party. Following the rule of capping this at one round, recorded
   honestly, not recursed.
3. **`.GIT` can't be tested on macOS**: the filesystem is case-insensitive, so `mkdir .GIT`
   directly becomes the existing `.git`. That cell only counts on CI's Linux.

### Two more instances of "the verification tool itself lied" along the way

- A mutation's perl expression matched nothing at all, and `mutate.sh` correctly judged it "not
  applied" (exit code 3). If it had run silently to completion and reported "no checks turned
  red," the conclusion would have been "this is a blind spot" -- the exact opposite of the truth.
- A newly added interop check stuffed `sg status`'s multi-line output into a shell string and
  grepped it, and even the most stable of those checks went red. **The guard was fine, the check
  was lying.** Without actually running it, the correct code would have been "fixed" backwards.
  The script now carries a comment recording this.

The final `tests/interop.sh` has 1299 checks (1276 at the end of Phase 21), all 40 `make test`
binaries pass (`tests/test_path_safe.c` added), `make sanitize` is clean, and the subcommand
count stays at 24.

### To investigate: `phase6a`'s smart-HTTP check's intermittent failure

Occurred **twice** during Phase 21/22 on 2026-08-21, out of roughly 15+ complete interop runs
total by that day. Five consecutive reruns afterward were all green, **not reproducible on
demand.**

The check that went red:

> `phase6a: that push advances the remote's refs/sg/chunks to the local keep-alive commit`

Its own comment states plainly "exit code alone can't distinguish 'sent it' from 'decided not to
send it,'" so the shape of the failure is: `sg add` moved the local keep-alive ref (the previous
check was green), `sg push` **exited 0**, push **did not** print "Everything up-to-date" (the
previous check was green), but **the remote's `refs/sg/chunks` never advanced.**

In other words, **the push side reported success while the remote state never changed.** This
looks more like a genuine intermittent bug in sg's smart-HTTP push than test-script noise -- these
checks are pure string comparisons, with no timing or race component. The fixture generates
content with `/dev/urandom`, so chunk boundaries differ each round; if the bug relates to a
specific chunk count/size, that would exactly explain the low frequency and difficulty
reproducing it.

**Starting point for next time it occurs**: keep `$WORKDIR/phase6a_http_server.log` and
`$WORKDIR/p6a_chunks_only_push_out.txt` -- both are already written out by the script; compare the
pkt-lines push actually sent against the ref update the server side received. **Do not treat it
as noise and just rerun.**

## Phase 23: display-side quoting for paths

Phase 22 deliberately kept control characters out of name validation, with the reasoning recorded
there: real git **accepts** them into a tree, and the defense lives at the display side. This
phase adds that defense.

### A confirmed problem, and a pitfall in the measurement itself

A file named `evil<ESC>[31m.txt`, after `sg add`/`commit`:

| | Raw bytes (od -c) | Terminal |
|---|---|---|
| sg | `e v i l 033 [ 3 1 m` -- `033` is a **real ESC** | Executes the control sequence |
| Real git | `" e v i l \ 0 3 3 [ 3` -- the backslash and `0`, `3`, `3` are **four separate characters** | Harmless text |

Warning: **`sed -n 'l'` renders a real ESC as `\033`, making it look identical to git's literal
escape.** The first pass read it this way and nearly concluded "both sides escape it." Verify
exclusively with `od -c` or `grep $'\x1b'`. Corroborating counts: sg's output has 1 line
containing a real ESC and 0 containing the literal `\033`; git has 0 lines with a real ESC and 1
with the literal `\033`.

### The rule (measured byte by byte)

Named escapes: `\\` `\"` `\a` `\b` `\t` `\n` `\v` `\f` `\r`; everything else below 0x20 and DEL
goes through **zero-padded three-digit octal** (`\001`, ESC -> `\033`, DEL -> `\177`); **spaces
are neither escaped nor trigger quoting**; quotes are only added when actually needed.

Warning: **ESC is `\033`, not `\e`** -- C has no standard `\e`.
Warning: **shell metacharacters are not git's criterion**: `~ ! $ * ? # ; | & ( ) < > [ ]` and
single quotes all pass through as-is. This is the easiest place to get wrong through "helpful
hardening," and writing it too broad **has no symptoms.**

### API shape: a static ring of borrowed pointers, not malloc

Nearly 70 call sites, most on error paths. A malloc'd version would need an OOM branch at every
site, and the most natural way to write it, `q ? q : path`, is **fail-open** -- printing the raw
bytes exactly when memory is tight. The project had already carefully thought this through for
**a single** call site (`net/http.c`'s `shown_url` falls back to `"(remote)"`, with a comment
stating "fail closed"). Requiring 70 sites to each pick the right direction every time was never
going to succeed. A static ring keeps the OOM decision **existing in exactly one place**, where
callers can't get it wrong.

Three functions rather than one, so that "the wrong shape can't even be expressed": the `a/`,
`b/` prefixes are **folded into** the quoting function (the quotes must wrap the prefix, and
splitting this into two decisions guarantees someone eventually writes `a/"x\ty"`); `_delimited`
unconditionally adds quotes, so leading/trailing whitespace in a path is visible mid-sentence.

**Placed in `util/`, not `workdir/`**: pure byte conversion, zero dependencies; `cat-file` prints
tree entry names, not working-directory paths, and making it depend on workdir would invert the
layering.

### Bytes >= 0x80 printed as-is: a deliberate, permanent divergence

Equivalent to `core.quotepath=false`. Reasoning: sg has no configuration system, so the user
can't turn this off the way git allows; and sg's own messages are UTF-8 Chinese to begin with, so
printing filenames as `\344\270\255` would look inconsistent with the surrounding output. **Every
interop comparison group adds `-c core.quotepath=false`**, except the control-character group
(both sides quote those), plus one **reverse check** -- comparing non-ASCII with git's **default**,
which **must differ.** That's the only thing pinning this divergence itself: without it, silently
switching sg to git's default wouldn't break any check at all.

### Three pre-existing gaps found while building the fixture (unrelated to quoting)

1. **Binary files were missing the `diff --git` line**, printing only `Binary files ... differ`.
   That's not a patch any tool can read back -- `git apply` relies on `diff --git` to know which
   file a hunk belongs to.
2. **`---`/`+++` were missing a trailing TAB.** Real git adds one when a path **contains a
   space** (neither `diff --git` nor `Binary files` add it), used to separate the filename from
   an optional timestamp. The trigger condition was determined by measurement (cross-checked
   across eight cases), not guessed from "probably added whenever there's whitespace."
3. **`strbuf_append_path`'s `char[4200]`**: with quoting applied, this could reach 16 KB, and
   would **silently truncate** -- and exactly the ones getting truncated were the most worth
   looking at (paths that are entirely control characters). Switched to a plain `strbuf_append`,
   eliminating the truncation point and shortening the code.

### Four real blind spots, all from the fixture missing an entire dimension

Site-by-site mutation (the specific instruction from the architect) caught:

| Blind spot | Missing dimension |
|---|---|
| status's staged/unstaged listing | The fixture only had untracked files |
| status's both-modified case | No conflict scenario |
| The `Binary files` line | The fixture was all text files |
| **The whole batch of 56 error-message sites** | interop never compares stderr wording |

**Verified as a whole batch, both `status` and `diff` would appear to "have coverage,"** while
those three printers had never once been touched by any check. The before/after for the last item
is the cleanest: the same `_delimited` degradation mutation, **1317/1317 entirely green
(a real blind spot)** before adding the check, **1319/1320, only that one going red** after.

Warning: that check's design hinges on **using a name that needs no escaping.** A name containing
ESC would still get quoted even after the degradation, giving **zero discriminating power** for
this decision. Written with only hostile names, the check would be green, look like it has
coverage, and actually verify nothing.

### Three false reds, all cases of the verification tool lying while the code was correct

1. `sed -n 'l'` renders a real ESC and git's literal escape identically (see above).
2. **`git status --porcelain` was used as the oracle for the long format.** Measurement:
   porcelain quotes names containing a space (its `?? ` prefix turns a space into a field
   delimiter), while the long format and `ls-files` don't. `sg status` is the long format, and
   printing it bare is **correct.** A check pinning "the two formats have different rules" was
   added, so the next person can't silently swap back in the wrong oracle.
3. A new interop check stuffed `sg status`'s multi-line output into a shell string and grepped
   it, and even the most stable of those checks went red. The guard was fine, the check was
   lying. The script now carries a comment recording this.

All three would have tempted someone into "fixing" code that was actually correct.

### "make: 0 warnings" isn't complete evidence of zero warnings

`sprintf` is deprecated on macOS. **The normal build reports 0 warnings, the sanitize build
reports 1** -- the exact same source, with two different flag sets giving two different warning
sets. Switched to byte-by-byte writing (the output is always a backslash plus three octal digits,
which never actually needed a formatting function). Worth remembering when reading gate
summaries.

### Deliberately not done

- **stdout informational messages like `Cloning into '%s'...` and
  `Initialized empty Git repository in %s` are not quoted** -- measurement confirmed **real git
  doesn't quote them there either** (a raw ESC goes straight to the terminal), and those paths
  come from argv the user typed themselves.
- **ref/branch/tag names are not quoted**: real git's `git branch` doesn't quote them either, and
  **there's no oracle**, and inventing behavior where there's no oracle is something this project
  has repeatedly paid for.
  (`sg_ref_branch_name_is_safe` only blocks path traversal, not control characters, so a
  hand-crafted `refs/heads/a<TAB>b` still prints as raw bytes. Recording this is better than
  silently leaving it be.)
- **Commit/tag messages and author strings are explicitly forbidden from being quoted**: that
  would break `cat-file -p`'s byte-for-byte fidelity, which interop is watching for. Confirmed by
  measurement to have not been mistakenly changed.
- **The two `strbuf`s are not converged**: this phase's diff has the property of being
  "mechanical, greppable, checkable line by line"; adding a public strbuf type would trade that
  for structural risk, and mixing both kinds into a single review would leave the reviewer unable
  to give a meaningful judgment on either. The two also aren't verbatim identical today.

### Payoff

A path with control characters currently produces a patch **`git apply` can't read back in**;
adding quoting fixes this. The scripting side also got strictly better: when a path contains a
newline, `sg status | awk` used to read one file as two lines (a silent bug); it's now a single,
unambiguously parseable line.

The final `tests/interop.sh` has 1320 checks (1299 at the end of Phase 22), all 41 `make test`
binaries pass (`tests/test_quote_path.c` added), `make sanitize` is clean, and the subcommand
count stays at 24.

## Phase 24: root cause of `phase6a`'s intermittent failure

Phase 22's closing section noted a "to investigate": `phase6a`'s
`that push advances the remote's refs/sg/chunks to the local keep-alive commit`
occasionally went red, not reproducible on demand, in the shape of "push reports success, remote
ref doesn't move." It's been tracked down, and it's a real bug.

### Root cause

`keep_alive_add`'s (`src/storage/chunk.c`) early return **only checks `new_count == 0`.** When
every chunk id passed in **already exists**, it still rebuilds a tree with **identical content**,
then builds a new commit with `time(NULL)`.

`sg push` **always** goes through the merge path (`sg_chunk_keepalive_merge_commit`) whenever the
remote and local keep-alive refs differ, and the remote's set is usually a subset of the local
one -- so it ends up "building a new commit despite adding nothing at all."

**Source of the intermittency**: the old and new commits **only differ in timestamp.** Within the
same second, the ids are identical (completely undetectable); across a second boundary, they
differ. When it crosses a second, the local ref advances and a new id gets pushed, while interop
recorded the local value **before** the push, so the comparison fails.

Byte-for-byte evidence (two commits from the same failure):
```
tree bcccb1b01bf96b7b764344472a5b9347877e5f2d
author small_git <sg@localhost> 1787294421 +0000
===
tree bcccb1b01bf96b7b764344472a5b9347877e5f2d
author small_git <sg@localhost> 1787294422 +0000
```

Before/after comparison (same script, same conditions):

| Condition | Failure rate |
|---|---|
| Normal speed | 6/60 (10%) |
| A 1.2-second delay inserted between `sg add` and `push` | **8/8** |
| **After the fix + the same 1.2-second delay** | **0/8** |

### The fix

`entries` now first copies all existing entries, then only adds new, non-duplicate ones, so
`entry_count == existing_count` is exactly equivalent to "nothing new at all." That case returns
directly.

**This wasn't just test flakiness**: every push would leave behind a content-duplicate commit in
the object store, and needlessly move a ref that had actually not changed at all.

### Why the new test deliberately does `sleep(1)`

The one-second delay in `test_keep_alive_merge_of_subset_does_not_move_the_ref` isn't wasted:
without it, the rebuilt commit would land in the same second and get the same id, and **the test
would pass both before and after the fix** -- an assertion that looks like coverage but has zero
discriminating power. The reasoning is written into the test's comment.

### The single most important thing to record about this investigation: the reproducer failed to
reproduce twice in a row, both times disguised as good news

| | Symptom | How it was caught |
|---|---|---|
| First time | Chunking wasn't enabled -> push short-circuited to "Everything up-to-date", all 3 "ok" runs were no-ops | The reproducer's own built-in **scenario self-check** |
| Second time | **`git clone` doesn't fetch `refs/sg/*` by default** -> the remote never had that ref -> all 40 rounds went through the "create" branch, not the "merge" branch being tested | Tightened the self-check: asserting the remote **already has** the ref |

The second time was especially dangerous: **40 rounds all green looks exactly like "no problem
found."** Concluding "chalk it up to environment noise" at that point would have been an entirely
wrong, and quite persuasive, conclusion.

**There was also a self-falsifying trap**: an earlier test of the timestamp hypothesis using
`sleep 1.2` got 0 failures, leading to the conclusion the hypothesis didn't hold. But that run was
on the reproducer **before it was fixed**, which never entered the merge branch at all -- **the
experiment itself was invalid.** After fixing the reproducer, the same experiment gave 8/8
failures. Lesson: **when an experiment gives a negative result, first confirm it actually
exercised the path under test.**

### Mutation verification: four mutations, two added following a cold read

| Mutation | What turned red |
|---|---|
| Reverting the early-return block | `keep-alive subset merge` |
| Making the condition always false | Same as above |
| **The condition's direction reversed** (`==` changed to `!=`) | **8 checks** -- "genuinely has additions" got misjudged as an early return, chunks stopped being protected from then on, and a whole batch of `read_blob`/`missing chunk`/`keepalive-ref-deleted` checks fell with it |
| **The early-return branch returns `-1` instead of `0`** | **Two checks, belonging to two different call sites**: `keep-alive incremental` (via `sg_chunk_store_blob`) and `keep-alive subset merge` (via `sg_chunk_keepalive_merge_commit`) |

The last two came from the cold read's suggested direction, not the original design. **The value
of the fourth mutation is that it proves this early return is guarded by tests from two
independent call sites** -- the first two mutations only verified one of them. Recorded here so
the next person doesn't have to rerun this just to find out "is anyone guarding this line."

### Reproduction method (the reproducer itself is deliberately kept out of the repo)

It needs to spin up an HTTP server, isn't in any gate, and would silently rot if dropped into
`tests/`; the persistent guard is the unit test above. To rebuild it:

1. Enable `sg.chunking true` + `sg.chunkthreshold 1048576` on a source repo, add a 5 MiB random
   file.
2. `git clone --bare` it into a server repo, start `interop.sh`'s embedded `http_server.py`.
3. `sg clone` a dest, **the dest must also have chunking enabled.**
4. **Push once from dest first** (creating the remote's `refs/sg/chunks`) -- this step is the
   second trap; `git clone` doesn't bring along `refs/sg/*`.
5. `sg add` a second 5 MiB random file, `sleep 1.2`, then push.
6. Compare the remote ref against the local ref; **and assert `remote_before` is a valid sha**,
   or it's going through the create branch instead of the merge branch.

## Phase 25: the diff foundation, and `sg status`'s short format

`sg diff` and `sg status` previously **accepted no arguments at all** (`(void)argv` plus
`argc != 1` printed usage). This round adds flags to both, and before doing so, first splits
apart `sg diff` -- because the reason it couldn't accept arguments was structural.

### Why this is a "foundation" rather than just "adding flags"

Before the rewrite, "which paths changed" and "how to print it" were **the same `for` loop**
(the old `cmd_diff.c`'s `for (i = 0; i < idx.count; i++)`). That loop walked index entries
directly and read working-directory files directly, so `sg diff` **could only ever** compare the
index against the working directory -- supporting `--cached`, `<rev>`, `<rev> <rev>` isn't adding
a branch, it requires another source of answers entirely.

Split into two layers:

- `include/sg/diff.h` / `src/workdir/diff.c` -- **only answers "which paths changed, and what's on
  each side."** Four builders correspond to four source pairings (tree<->tree, tree<->index,
  index<->working directory, tree<->working directory), producing a path-sorted `sg_diff_list`.
  Prints nothing.
- `include/sg/diff_out.h` / `src/cli/diff_out.c` -- **only answers "how to print it,"** six
  formats. Shared between `sg diff` and `sg stash show`.

`old_tree` passed as `NULL` means an empty tree, so an unborn HEAD doesn't need an empty tree
object written just for diffing.

### Oracle facts confirmed by measurement (real git 2.55.0, `LC_ALL=C` throughout)

**Conflict paths give three different answers under three different comparisons** -- the
easiest set to get wrong:

| Comparison | Real git | Why |
|---|---|---|
| `diff --cached` | `U` / `\| Unmerged` | There's no single staged blob to compare |
| `diff <rev>` | Ordinary `M` | The index only decides membership; content still comes from the working directory |
| `diff` (index vs working directory) | One `U` row **plus** one row of stage 2 vs working directory | Two rows for the same path |

The third cell's "which stage does the second row use" was identified this way: making the
working-directory content **byte-for-byte identical** to stage 2 makes the second row disappear
entirely; using stage 1 or stage 3's content instead keeps the second row present.

**`N files changed` doesn't count unmerged rows.** With only unmerged rows, it prints
` 0 files changed`, and **neither clause is printed** -- coexisting with the rule that "when both
are 0, both clauses print," with the difference hinging purely on `files_changed`:

| Scenario | Output |
|---|---|
| Binary, `files_changed == 1`, ins/del both 0 | ` 1 file changed, 0 insertions(+), 0 deletions(-)` |
| Entirely unmerged, `files_changed == 0` | ` 0 files changed` |

**`diff <rev>`'s path set is decided by the index, not the working directory**: after
`git rm --cached f`, the file is still on disk, and `git diff HEAD` still reports `D f`;
untracked files never appear at all.

**Folding of untracked directories is recursive and per-directory**: if there's a tracked path
anywhere underneath at any depth, descend into it; otherwise, if there's a non-ignored file
underneath, print `dir/` once; otherwise (everything ignored), don't list it by default, and
print `dir/` once with `--ignored`; an empty directory prints nothing. Warning: the criterion is
"**recursively, is everything underneath ignored**," not "does the directory name itself match a
pattern" -- `*.tmp` never matches the name `d/subignored/`, and git still folds it. A flat fixture
can't tell these two implementations apart.

**Seven unmerged combinations** (based on which stages are present in the index): `{1}` = `DD` /
`both deleted:`, `{1,2}` = `UD` / `deleted by them:`, `{1,2,3}` = `UU` / `both modified:`,
`{1,3}` = `DU` / `deleted by us:`, `{2}` = `AU` / `added by us:`, `{2,3}` = `AA` / `both added:`,
`{3}` = `UA` / `added by them:`. The long format's label column width is **17** (the
staged/unstaged sections use 12). Before this change, `print_unmerged` printed `both modified:`
for all seven, six of which were wrong.

**Two quoting rules**: porcelain **quotes as soon as a name contains a space** (its `?? ` prefix
turns a space into a field delimiter); the long format and the four machine formats
(`--stat`/`--numstat`/`--name-only`/`--name-status`) **don't quote spaces**, but both quote
control characters. Phase 23 produced a false red exactly by picking the wrong oracle; this time
the two were pinned into interop **head to head**: the same `has space.txt` must be quoted under
porcelain and must not be quoted under the long format.

### `--stat`'s layout algorithm

The only part of this round with a genuine algorithm, reverse-engineered by measurement, verified
against 24 boundary cases plus 240 random cross-validation cases. Key points: the layout budget is
`name + number + 6 + graph <= width` (line length is `width - 1` when saturated); `COLUMNS`
applies even without a tty; bar scaling is **truncating integer division, no rounding**, with the
smaller side scaled first and the larger side absorbing the remainder; column width is computed
against the **quoted** string and is **display width, not byte length**.

The reverse-engineering process itself has one lesson worth recording: the first version
concluded "binary rows don't compete for column width," which was a **false negative** derived
from a case where `max_change` overwhelmingly dominated; only after doing a binary-only `COLUMNS`
sweep did `max_graph = max(max_change, bin_width - 4)` get worked out.

### The three most important things to record from this round

**(1) The contract is written in the right place, but only in one place, while the
responsibility spans two layers.**

`diff_out.h`'s comment states "an unreadable blob must not silence the entire diff," and the
rendering layer does honor this. But **the builder itself also reads chunk data** (to answer
"has this changed?"), and it `return -1`s there -- the entire list dies, and the actionable
message disappears too, because the failure happens **before** the rendering layer is even
called, so nothing downstream still knows which file was broken.

Fix: the builder puts that path into the list **as if it changed**, letting the rendering layer
carry the path and hit the same failure a second time.

**This gap could never have been found in any single working tree**: on the builder's side,
`cmd_diff.c` was still the old version, so interop was 1325/1325 all green; on the renderer's
side, there was no fix to test. Each side honestly reported "all green" on its own; the gap only
existed between the two.

**(2) Site-by-site mutation testing conflicts with `mutate.sh`'s own advice.**

The script's comment says "always add `/g` when a literal occurs more than once" -- that answers
"is this rule enforced." Answering "**does each site individually have coverage**" requires
exactly the opposite of `/g`: it blends the results of two sites together, so if either site has
coverage, the overall result goes red.

Example: `sg_chunk_effective_id` has two call sites. Mutated site by site, site A went red while
site B produced **exit code 0, zero FAILs** -- the test only ever exercised index-vs-workdir,
never tree-vs-workdir, and the latter is definitely exercised in real usage (a committed chunked
file has its pointer id stored in the tree). Without normalization, **every unmodified large
file would be misreported as changed.**

Distinguishing sites sharing the same literal doesn't require `/g` -- including the surrounding
context (indentation depth, the preceding line's call) in the pattern is enough.

**(3) There are three reasons a mutation can fail to go red -- don't conflate them.**

| Why it stayed green | Example | What to do about it |
|---|---|---|
| **A real blind spot**: no test covers that dimension | `--stat`'s `W*3/8` squeeze constant changed to `1/2`, every unit test still passed | Add a test, anchored to an external oracle |
| **A redundant guard**: the real defense is a layer below | The `name_width <= 3` early return -- the normal path already produces `"..."` | **Delete the guard**, letting the mutation land on whatever actually decides the behavior |
| **Mathematically unobservable** | `graph_width`'s lower bound of 6 -- a later reclaim branch overwrites it unconditionally | Record the proof, switch to a property that can actually be verified |

The third kind was **proven** to me by the implementer with algebra plus 200,000 random probes,
rather than forcing a falsely-covering test into existence.

### Verification

**78** checks were added to `tests/interop.sh`, of which **22 are `oracle: precondition`** checks
-- they don't verify sg, only that the fixture actually contains that scenario. This batch paid
off immediately: three went red on the very first run, all due to fixture mistakes -- `bin.dat`
and the control-character filename were added in the first commit and never modified again, so
they never appeared in the `--cached` diff or in porcelain either. In other words, the check group
claiming "covers the binary row and quoted names" **covered neither one**, and would have shown
as passing.

`--stat`'s squeeze constant was deliberately verified via interop rather than unit tests: a unit
test's expected string is partly copied from **the tested program's own output**, an assertion
type that has zero discriminating power against "the implementation doesn't match real git," only
proving the program doesn't randomly vary. Comparing side by side against real git is the only
real oracle here.

`tests/mutate.sh` also grew a third rule this round: a mutation left `list_diff_sorted`'s merge
cursor never advancing, and the test was **still running after thirty minutes**, and "never
exits" is neither 0 nor non-zero. There's now `SG_MUTATE_TIMEOUT` (default 300s), with "timeout"
and "crash" **flagged separately** -- both only prove "breaking it causes trouble," neither proves
that named assertion has discriminating power.

### `sg stash show`

The four trees a stash entry needs were already being computed: `load_stash_trees` has been
resolving `stash@{N}` into base / index / worktree / an optional untracked tree since Phase 15,
just that it was `static`, invisible to `cmd_stash.c`. This round promoted it to a public API
rather than having `cmd_stash_show` re-parse the parents itself -- this project has already
learned the lesson from eight verbatim copies of `env_or()`.

Three facts confirmed by measurement, not assumed: the default format is **diffstat, not
patch**; `-u` and `--only-untracked` **aren't two independent booleans, but a single mode
selector, where whichever is written later wins** (only distinguishable by running both orders);
without an untracked parent, `--only-untracked` prints nothing and `-u` falls back to a plain
diff, both exiting 0.

**There was a bug in `-u`'s union, caught by the "ordering" test**: the untracked half used to
compare `base_tree` against `untracked_tree`, and every path that exists only on the tracked side
didn't exist in `untracked_tree` at all, so each got over-reported as a **phantom deletion** --
the same path printed twice. The correct approach is comparing an **empty tree** against
`untracked_tree` (which `--only-untracked` already did correctly).

What revealed it was a deliberate choice in the fixture: the untracked filename `b.txt` was
chosen **to sort between the two tracked filenames `a.txt` and `c.txt`.** If it sorted last,
"concatenating two lists" and "merge-sorting them" would produce identical output, and this test
would have had zero discriminating power.

### Mutation verification: 34 mutations, run by the primary conversation

Recorded so the next person doesn't have to rerun this to know which lines are guarded (the
convention is set out in Phase 24). The list was designed by each round's reviewer and executed
by the primary conversation -- the implementer doesn't verify their own fixes.

| Scope | Count | Properties guarded (notable ones) |
|---|---|---|
| `workdir/diff.c` first round | 5 | blob id comparison, mode comparison, `NULL` tree = empty tree, merge-join's comparison direction |
| `workdir/diff.c` tail | 13 | `index_group_end`'s group advancement, each of the three builders' **own** conflict behavior, the second row taking stage 2 (goes red if switched to stage 1), `-2` must not be collapsed into `-1`, both directions of the content comparison |
| `cmd_status.c` / `status.c` / `quote.c` | 9 | the seven unmerged labels and codes, the three folding rules, porcelain's space-quoting, binary search's `/` boundary, `--ignored`'s negative assertion |
| `cli/diff_out.c` | 11 | graph's `total<2` floor and tie-breaking direction, truncation pushed to `/`, CJK column width, binary counting toward but unmerged not counting toward `files changed`, the `files_changed > 0` gate |
| `cli/cmd_stash.c` | 4+ | the phantom-deletion fix (`NULL` vs `base_tree`), the merge direction, `--only-untracked`'s ternary, "whichever is written later wins" |

**Three results especially worth recording:**

1. **A blind spot only visible site by site**: `sg_chunk_effective_id` has two call sites, site A
   had coverage, site B produced **exit code 0, zero FAILs.** Using `/g` to hit both sites at once
   would have turned the overall result red, with the conclusion "already verified." Site B is
   definitely exercised in real usage (a committed chunked file has its pointer id stored in the
   tree), and without normalization **every unmodified large file would be misreported as
   changed.** A test has been added.
2. **A rule given extensive documentation had no test at all**: `-u` and `--only-untracked`'s
   "whichever is written later wins" had twenty lines of comments, a dedicated commit-message
   section, both orders individually measured -- and no test ever passed both flags at once.
   Gutting the entire rule left all 9 tests passing. A test has been added.
3. **"Removing the guard" usually crashes; only "making the guard return an error code" is
   testable**: verifying `tree_id == NULL`'s semantics, removing the whole section causes a
   segfault (rc 139, zero FAIL lines), never even reaching the named check; keeping the guard but
   making it `return -1` instantly turns `test_null_tree_is_empty_tree` red for the right reason.

`--stat`'s `W*3/8` squeeze constant was **deliberately verified via interop rather than unit
tests**: a unit test's expected string is partly copied from the tested program's own output, an
assertion type with zero discriminating power against "the implementation doesn't match real
git." Compared side by side with real git at COLUMNS 40/60/80/120, the same mutation turned three
of them red (the 120 one doesn't trigger the squeeze, staying green for the right reason).

### Caught by a cold-read review: a real blind spot (the rule was correct, but nothing guarded it)

After merging into master, a round of third-party cold reading was added. **No memory-correctness
issues found** -- the `pos` array's release across six early returns and a `goto done`,
`sg_pathspec_add`'s two realloc failure paths, `sg_diff_list_filter`'s free ordering during
in-place compaction, `sg_quote_path_delimited`'s 4-slot borrowing rule, and `spec_matches`'s
`spec[slen-1]`/`path[slen]` indexing all held up when checked one by one.

But it pointed out something not anticipated: `spec_matches`'s rules 1/2 (literal comparison) run
**unconditionally**, not conditioned on `!has_wildcard(spec)`. So a directory **literally named**
`o[tx]her` would get recursed into by the spec `o[tx]her` via the directory-prefix rule -- which
looked like it contradicted "a spec containing a wildcard has no directory-prefix rule."

Measurement first: real git 2.55.0 for `git diff -- 'o[tx]her'` prints `o[tx]her/f.txt`, and for
`st*ar` prints `st*ar/g.txt`. **sg's behavior matches git exactly, the code was correct** -- this
is exactly git's `match_pathspec_item`'s order (`ps_strncmp` first, `wildmatch` after), and "rule
3 doesn't add rule 2" describes *that one wildcard comparison* not taking the directory prefix,
not "a spec containing a wildcard never takes it at all."

**The real problem was that nothing tested this at all.** A directed mutation confirmed it:
narrowing rules 1/2 to `!has_wildcard(spec) && ...` (exactly the most natural "fix" someone would
make after seeing `test_wildcards`'s negative checks), interop stayed **1500/1500 all passing,
exit code 0** -- not "can't reach it," a **real blind spot.**

Two check groups were added (4 unit + 3 interop cmp + 2 controls), and the same mutation was
rerun to confirm it now goes red: 4 unit checks, 4 interop checks. The reverse mutation was also
rerun, confirming those three "must be empty" checks **still** have discriminating power -- the
new cases didn't accidentally legitimize the over-broad rule in the other direction. **Both
directions need to be pinned at the same time; pinning only one leaves the other to silently
vanish the next time someone "helpfully unifies" it.**

Other findings and rulings:

| Finding | Ruling |
|---|---|
| An overly long pathspec gets misreported as "outside the repository" (`sg_resolve_repo_path_allow_root` returns NULL for both OOM and out-of-bounds) | An existing behavior of an existing shared function, already explicitly acknowledged by `pathspec.h`; Phase 28 is the first call site to feed it **arbitrary user strings**, so it's more noticeable here. **Not changed**, recorded as a known gap |
| The `--cached && rev2` usage check moved to after repo discovery | Benign: consistent with every other `sg_cmd_*` (all call `sg_require_git_dir` first), and real git also reports not-a-repo first outside a repo directory |
| `report_pathspec_error`'s `default:` would silently print "outside the repository" for any future new error code | **Fixed**: switched to enumerating every value, so `-Wswitch` now guards it |

Warning: two mutations' results must be recorded honestly, not counted as "already verified":

- **M10** (moving a `free` into the keep branch to create a double-free) was caught by exit code
  133, but what caught it was the **crash**, not any named assertion. It proves "misusing memory
  causes trouble," it does **not** prove any check verifies the free ordering.
- **M9** (removing the `free(rel)` in `reserve()`'s failure branch) **cannot be verified with
  existing tests**: that path is only reachable if realloc fails, and this project has no fault
  injection. This defense only has a cold read behind it, and no coverage is claimed.

### Deliberately not done this round

- **The patch body doesn't chase real git**: an entire file is still a single hunk
  `@@ -1,N +1,M @@`, with no `index` line, no `/dev/null`, no 3-line-context multi-hunk splitting,
  no `\ No newline at end of file`. As a result, unmerged rows are **skipped outright** in patch
  format (real git prints a `diff --cc` combined format). The four machine-readable formats are
  what this round used for byte-for-byte comparison against git.
- **No pathspec filtering** (added in Phase 28), **no rename detection** (`sg_status_kind`'s data
  structure can't even hold it).
- **Symlinks** aren't listed by any of the three traversal paths -- a pre-existing behavior,
  consistent with this project's deliberately deferred symlink support.
- **When the index has both a stage 0 and stages 1/2/3 at the same time**, the leftover conflict
  entry is silently skipped. sg's own write paths can never produce that state (`cmd_add.c` always
  calls `sg_index_remove_all_stages` before writing stage 0), and the failure direction is "safe
  but incomplete," noted above `index_group_end`.

## Phase 26: byte-for-byte fidelity of the patch

Phase 25 split diff into two layers, "finding changes" and "printing them," but the patch body
deliberately didn't chase real git: an entire file was a single hunk, no `index` line, no
multi-hunk splitting. The cost was that `tests/interop.sh` could only grep header lines and
compare for patch -- **the most significant of the six output formats had no oracle for its body
at all.**

This round brought the patch to byte-for-byte parity with real git, and upgraded interop to a
full-output `cmp`.

### Oracle facts confirmed by measurement (real git 2.55.0, `core.fileMode=true`)

| Case | Output |
|---|---|
| Pure content modification | `index <old7>..<new7> <mode>` |
| Addition | `new file mode 100644` + `index 0000000..<new7>` + `--- /dev/null` |
| Deletion | `deleted file mode 100644` + `index <old7>..0000000` + `+++ /dev/null` |
| Pure chmod | Only `old mode`/`new mode` -- **no index line, no `---`/`+++`, no hunk** |
| chmod + content | `old mode`/`new mode` + `index <old7>..<new7>` (**no** mode suffix) |
| Binary | (mode line, if any) + `index <old7>..<new7>[ <mode>]` + `Binary files a/f and b/f differ` |

- **The index line's mode suffix only appears if no mode line has already been printed for this
  entry.** This is a single rule, not something decided separately for each of the four cases;
  implemented via a single boolean, `wrote_mode_line` -- don't compute it separately in two
  places.
- The abbreviation is a fixed **7 hex digits** (measured across loose object counts of
  10/100/500/1000/2000/4000, always still 7; this project doesn't read `~/.gitconfig`, so there's
  no dynamic abbrev).
- The side that doesn't exist is `0000000`; **an empty file is `e69de29`.** "Exists but empty" and
  "doesn't exist" are two different things.
- **A pure chmod does still get listed in the machine formats**: `--stat` prints ` f | 0`,
  `--numstat` prints `0\t0\tf`, `--shortstat` prints
  ` 1 file changed, 0 insertions(+), 0 deletions(-)`, `--name-status` prints `M`.

The hunk part:

- `@@ -s,c +s,c @@`, with **`c == 1` omitting `,c`**, and **`c == 0` writing `s` as 0**
  (`@@ -1,16 +0,0 @@`).
- context = 3, two changes are **merged if <=6 lines apart, split if >=7** (measured: 5/6 merge,
  7/8/9 split).
- The function-name suffix: scans backward for the first line whose first character is in
  `[A-Za-z_$]`, appending that whole line verbatim right after `@@ `. Measured characters that
  **don't count**: digits, `#`, `/`, `}`, `.`, `@`, whitespace, tab. When nothing is found, **there
  is no trailing extra space at end of line** (confirmed with `od -c`, don't judge by appearance).
- `\ No newline at end of file` immediately follows the line missing a newline, and **doesn't
  count toward the range.** "Identical text but different newline state" counts as **a different
  line**, so line-equality checks must look at `has_nl` -- but `sg_diff_lines_equal` is shared with
  `workdir/merge.c`, whose semantics can't be changed, so a separate strict version was added.

### The data layer previously couldn't print an `index` line at all

The most easily underestimated part of this round: the `index` line wasn't a rendering-layer
problem. `sg_diff_side`'s WORKDIR side had **`mode` always 0, `id` never filled in**, and `sg
diff` with no arguments (the most commonly used invocation) has both sides going through it.
Ironically, `sg_hash_file_blob` had **already computed** that hash, just to discard it after
comparing.

After adding id and mode, `sg diff` could see a pure chmod for the first time. **But `sg status`
still can't** -- it goes through `src/workdir/status.c`'s `sg_status_diff_unstaged`, a
**separate implementation** that goes through none of `sg/diff.h`'s builders and never compares
`.mode` at all. This is a newly discovered duplication this round, and the divergence remains
open.

### Alignment: compaction and the indent heuristic

`diff.indentHeuristic` is **enabled by default**, and genuinely changes hunk placement (the same
input produces `@@ -3,6 +3,10 @@` with it off versus `@@ -4,5 +4,9 @@` with it on). This layer was
**checked against real git's `xdiff/xdiffi.c` line by line**, not from memory: 14 scoring
constants, `measure_split`, `get_indent`'s whitespace handling, and three things governing
`xdl_change_compact`'s driving loop -- the `else if` chain's priority (`end_matching_other`
**overrides** the heuristic), three sliding lower bounds (`earliest_end`,
`g.end - groupsize - 1`, `g.end - MAX_SLIDING`), and `<=0` letting the bottommost position win
ties.

git doesn't tie the deletion side and the addition side together: they each live in their own
file's `changed` bitmap, **sliding independently**, with `end_matching_other` deciding whether to
keep them aligned. sg used to bind them into a pair, so a group containing both a deletion and an
addition **couldn't slide at all** -- the main source of the residual divergence.

### The residual 2-3%, and what it is **not**

Fuzzer measurement (below) leaves about 2-3% of positional divergence. **The cause is not in the
compaction layer**, which has already been checked line by line against the source. It's in the
**underlying alignment algorithm**: sg uses LCS dynamic-programming backtracking, git defaults to
Myers, both are minimal but pair up differently, and compaction only normalizes position, not
pairing.

Evidence: comparing the 11 residual cases against git's other algorithms, **6 were byte-for-byte
identical to `git diff --histogram`** (3 of those also matched `--patience`), 5 corresponded to
none of them.

Warning: **the next person should not go looking for a scoring bug that doesn't exist.** Eating
this 2-3% requires implementing Myers-specific split selection -- a different algorithm, not a
tuning parameter.

### Verification instrumentation: `tests/fuzz_diff.py`

The other five diff formats are fully determined by "which paths changed," so hand-written
fixtures pin them down completely. **The patch body is not**: when the same minimal edit script
can be written at several positions, git picks one, and "which one" comes from compaction and the
heuristic. A fixture can only pin down alignments someone thought of, and **the ones nobody
thought of are exactly where the implementation drifts.**

So the generator was deliberately biased toward ambiguity: repeated blocks, varying indentation,
insertions that duplicate existing text. It compares byte-for-byte against real git as the
oracle. Convergence curve (seeds 0-199):

| Stage | Mismatches |
|---|---|
| Compaction only | 40 |
| Added the indent heuristic | 33 |
| Fixed `score_cmp`'s structure (effective_indent dominates) | 27 |
| Switched to merge-then-score | 16 |
| Each side slides independently | 4 |

Three disjoint seed ranges had consistent rates (2.0% / 3.0% / 2.3%), not an overfit to seeds
0-199.

Warning: **`--max-failures 0` was deliberately added**: the default stops at the first 3
mismatches (that's the debug mode), while convergence needs a **rate** -- "stops at 3" isn't a
rate, and would report the same number on every run, looking like stalled progress.

### Two pitfalls hit along the way

**A parameter tuned to make a test go green can turn out to have fixed nothing at all, even
though the test really did go green.** `RELATIVE_INDENT_PENALTY` was once flipped from `-4` to
`+4`, on the grounds that "this makes anchor A2 pass." In real git's source it's `-4` (a
**bonus**, not a penalty). That `+4` was compensating for a different bug (groups couldn't slide
independently); once the real bug was fixed, the constant was changed back to `-4`, and the
anchor still passed.

**Diffing two diffs against each other renders an "overall shift" as "different content."** At
one point this was used to conclude sg and git had computed different edit scripts, requiring an
algorithm rewrite. After actually comparing the two sides' `+`/`-` lines as **multisets**: at the
time, **all 33** residual cases were shifts, with **0** genuinely different scripts. To
distinguish the two, compare multisets, don't look at a diff-of-diffs -- the rendering layer
itself manufactures that illusion.

### Known limitations

- Conflicted paths' `diff --cc` combined format is still skipped outright
  (`print_patch` starts with a `continue`), unimplemented.
- `sg diff` still doesn't support pathspec (`sg diff -- <path>` just prints usage). **Added in
  Phase 28.**
- Rename detection isn't done, and `sg_diff_side`'s three states still can't hold it.
- When a working-directory file can't be read, sg doesn't abort the entire command the way real
  git does (`fatal: cannot hash`, exit code 128) -- it keeps the row in the list and has the
  rendering layer print a warning naming the file. The **modified/deleted** paths treat it as a
  deletion (consistent with `sg_status_diff_unstaged`), **printing no warning** -- a pre-existing
  behavior, still open. The **addition** path prints a warning, at the cost of the header carrying
  a new id of `0000000`.
- Tests induce a read failure by "turning a tracked path into a directory" (avoiding
  `chmod 000`, which doesn't work when run as root). Measured on macOS: `fopen` succeeds and
  `fread` sets `ferror`/EISDIR; **Linux is only covered by CI.**

## Phase 27: `sg status` and `sg diff` keep only one answer for the same question

While finishing up patch fidelity in Phase 26, it was noticed in passing that `sg diff` could see
a pure chmod while `sg status` couldn't. The cause wasn't a missed line -- it's **two independent
implementations** answering the same question: both `sg_status_diff_unstaged`
(`src/workdir/status.c`) and `sg_diff_index_workdir` (`src/workdir/diff.c`) compute "which paths
changed between the index and the working directory."

### Enumerate first, then converge

Not changed directly at first. `tests/test_status_diff_parity.c` was written first (named cases
plus random scenarios, in the style of `tests/test_fuzz_pack.c`), normalizing both functions'
output and comparing them side by side, **enumerating every divergence.**

Reasoning: the mode one was found **by accident** -- no test would have caught it. Given that,
there's no reason to believe it's the only one. Converging blindly risks either "fixing" an
unknown-until-now behavioral difference that was actually a feature, or leaving an actual bug in
place -- and two of the three call sites are safety gates.

Result: **exactly three categories**, with 5000 random rounds finding no more.

| # | Divergence | Ruling |
|---|---|---|
| Mode | diff compares it, status doesn't look at it at all | A bug on status's side (real git prints ` M` for a pure chmod) |
| Chunk pointer can't be resolved | diff degrades gracefully, status returns `-1`, abandoning the entire scan | A bug on status's side |
| Unmerged | status skips every non-stage-0 entry; diff produces an unmerged row plus a stage-2 comparison row | **A deliberate divergence, kept** |

Reason the third category was kept: `cmd_status.c` has its own Unmerged paths section. Both
functions' header comments already state this.

### Behavioral changes after converging

`sg_status_diff_unstaged` became a thin adapter over `sg_diff_index_workdir`. Three call sites:
`cmd_status.c`, and **the two dirty-working-directory gates** in `apply.c`. So this wasn't just a
refactor:

- `sg status` started reporting pure chmods.
- **`sg switch` / `sg reset --hard` / `sg merge` / `sg rebase` started treating a pure chmod as
  dirty.** Measurement confirms real git blocks it the same way
  (`error: Your local changes to the following files would be overwritten by checkout`), while a
  control run (clean working directory, otherwise identical) switches normally.

Warning: **the direction of a gate's failure has to be confirmed personally, never assumed.**
Before converging, an unresolvable chunk pointer made the function return `-1`, and `apply.c` had
`dirty = !unstaged_ok || ...` -- an inconclusive result was treated as dirty, fail-closed. After
converging, that path no longer returns `-1`, and instead lists that path as changed. The result
is still dirty, but **the mechanism shifted from failure protection to normal reporting.**
Verified item by item that no fail-open was introduced: a truncated path still returns `-1`
(`diff.c:363`), a chunk failure gets listed, and a hash failure becomes ABSENT and gets listed too
-- there's no path that returns 0 while omitting the path silently.

### A new risk introduced by converging, and why it almost went unnoticed

The adapter layer needs to filter out unmerged paths' stage-2 comparison rows. The first
version's criterion was **"skip if the same path as the previous row."**

This is correct under a normal index, but **`sg_index_read` doesn't validate ordering or
deduplicate on load** (`src/index/index.c`, which trusts the file on disk). A hand-edited or
corrupted index can put the same path into two non-contiguous groups; `index_group_end` only
merges a contiguous run, so the two groups each produce one row, and if the paths in between
happen to have no differences, the two rows end up **adjacent** -- the second row gets silently
dropped as if it were the stage-2 comparison row.

The direction matters: the old implementation, in the same scenario, would at worst double-count
(harmless); the new implementation **under-reports**, and this feeds exactly the dirty checks for
`switch`/`reset --hard`. Tightening the criterion to "**the previous row is an unmerged row and
the same path**" fixes it.

Warning: after tightening it, a mutation was run reverting the criterion to the loose version --
**zero tests turned red** -- a real blind spot. The scenario is constructible (a test can
directly manipulate `sg_index`'s array to insert past ordering), just nobody had built it.
`test_corrupt_index_duplicate_path_is_not_dropped` fills exactly this gap. **A fix has effectively
not landed at all if no test can distinguish it from the bug it fixed.**

The invariant "a non-unmerged path never produces a second row" has now been written into
`include/sg/diff.h`'s public contract -- previously it only lived in the adapter layer's own
comment, and if `sg_diff_index_workdir` ever gains a second row for an ordinary path due to a
future feature (e.g. rename detection), that bug would otherwise be silent.

### A known diagnostic downgrade

Before converging, an unresolvable chunk pointer made the gate print "cannot fully determine
working directory status"; after converging, that path gets listed as an ordinary
`SG_STATUS_MODIFIED`, and that warning no longer appears -- the user sees it just like a normal
edited file. **No safety regression** (still counted as dirty), but at the exact moment the user
needs accurate information most (about to overwrite the working directory), one signal is
missing. This is recorded in `include/sg/status.h`'s contract.

### A blind spot in the harness itself

The parity fuzzer initially only counted "was the path reported at all," **not the `kind`.**
Measured: changing the adapter layer's `SG_STATUS_MODIFIED` to `SG_STATUS_DELETED`, not a single
one of 5000 fuzzer rounds went red, only two named tests did. After adding a kind assertion, the
same mutation produced 619 FAIL lines.

Warning: when adding this, the expected value must be **explicitly hardcoded**, not taken from
what the diff side computes -- that would be self-referential, unable to catch both sides being
wrong together, and "would both sides go wrong together" is exactly the reason this harness
exists.

## Phase 28: `sg diff`'s pathspec

`sg diff -- <path>` previously just printed usage. This round added it, and along the way
switched `sg diff`'s argument parsing to real git's "revision or path" disambiguation rules.

### Every rule was measured, and not one of them matches intuition

Before starting, a table was measured against git 2.55.0 (`git diff --name-only -- <spec>`, with
a worktree containing `a.txt`, `sub/b.txt`, `sub/deep/c.txt`, `other/d.c`, `e.c`, all modified):

| spec | git's answer | Why it's easy to guess wrong |
|---|---|---|
| `sub` | `sub/b.txt`, `sub/deep/c.txt` | A directory prefix, matches intuition |
| `*.c` | `e.c`, `other/d.c` | **`*` crosses `/`** -- pathspec uses wildmatch with WM_PATHNAME off |
| `sub*` | `sub/b.txt`, `sub/deep/c.txt` | Matches for the reason above, **not** because it recurses into the directory |
| `o[tx]her` | (empty) | A spec containing a wildcard has **no** directory-prefix rule |
| `su?` / `s*b` / `sub/dee?` | (empty) | Same as above, all three are empty |
| `sub/` | `sub/b.txt`, `sub/deep/c.txt` | A trailing `/` means "what's underneath this name" |
| `a.txt/` | (empty) | The other half of the same rule: there's nothing underneath an ordinary file |
| `lit*st` (a filename that literally contains `*`) | `lit*st` | The literal match runs first; the wildcard characters are treated as literal |
| `nosuch` | (empty, exit 0) | `git diff` doesn't error on a pathspec matching nothing |
| `""` | fatal | An empty string isn't a valid pathspec |

So the comparison is **three ordered rules** (`spec_matches`, `src/workdir/pathspec.c`): exact
literal match -> literal directory prefix -> only a wildcard-containing spec goes through
`sg_wildmatch`. Warning: **the first two rules don't add to the third.** This is exactly the
shape of git's `match_pathspec_item`: `ps_strncmp`'s literal comparison runs first, then
`wildmatch` gets its turn, and the literal branch requires the entire spec to be a prefix of the
path, which `o[tx]her` fails.

### Sharing wildmatch, rather than writing a second glob

`src/workdir/ignore.c`'s `seg_match` was already a pure-byte wildmatch (`*` `?` `[]` `\` all
present, and it **doesn't understand `/`**) -- exactly what pathspec needs. gitignore looks
different only because it layers segment logic on top so `*` stops at `/` and `**` crosses
directories. In real git the two only differ by a single `WM_PATHNAME` flag.

So `seg_match` + `class_match` were moved verbatim into `src/util/wildmatch.c` as `sg_wildmatch`,
with ignore.c keeping the segment layer and calling it instead. After the move,
`python3 tests/fuzz_ignore.py` (using real git as oracle) was run for 200 rounds with 0
mismatches, confirming the move didn't change gitignore's behavior -- the most dangerous failure
mode of this kind of refactor is "behavior changed and nobody noticed," and ignore happens to
have a ready-made differential oracle.

### Filtering happens after the list is already built

`sg_diff_list_filter` (`src/workdir/diff.c`) takes an already-completed `sg_diff_list`. It's
**not** pushed down into the four builders: four independent pathspec checks, each with its own
understanding of "which paths participate in the comparison," is exactly the shape Phase 27 spent
a milestone eliminating. The cost is that filtered-out files still get hashed once -- that's a
speed bill, not a wrong answer, and it's noted in the header.

Unmerged paths occupy **two adjacent rows** under index-vs-working directory (Phase 25); the two
rows share a path, so they're always kept together or dropped together, never split apart.

### Disambiguating bare arguments

Nothing is checked after `--` (that's exactly the point of typing `--`). Without `--`, each
argument is judged individually, again by rules confirmed through measurement:

- Both a valid revision and an existing file -> **rejected outright**, never guessed.
- The first "path-like" argument ends the revision list, and **every argument after it must
  exist** -- `git diff a.txt HEAD` fails naming `HEAD`, even though it's a perfectly valid
  revision.
- Neither one -> "ambiguous argument."
- Warning: **an argument containing a wildcard skips the existence check**: `git diff '*.zzz'`
  still exits 0 even matching nothing, while `git diff nosuch` is a hard error. This is answered
  by `sg_pathspec_looks_like_spec`, whose character set lives in the exact same file as the
  matcher that gives those characters meaning.

Magic (`:(icase)`, `:!`, `:/`) **is rejected rather than treated as a literal path.** Treating it
as literal would give the user either an empty diff matching nothing, or worse -- matching a file
literally named `:!sub`. Both answer a question the user never asked, and a diff silently
printing too few files is the worst failure mode here.

### Verification: forward and reverse mutations

68 checks were added to interop (1432 -> 1500), **almost every one a full-output `cmp` of the
same set of arguments between sg and real git**; asserting against sg's own output would just
freeze "sg's current behavior," and only git can say which one is actually correct. Error cases
can't be compared by exit code (git uses 128, this project only has 0/1), so they were split in
two: sg rejects it and the message names the reason; paired with an oracle check confirming "real
git rejects it too."

Eight directed mutations, all caught:

| Mutation | Went red |
|---|---|
| Removing the directory-prefix rule | 15 interop checks + 6 unit tests |
| Making `sg_diff_list_filter` filter nothing | About 30 interop checks (including the positive control) |
| Removing the wildcard branch (`if (has_wildcard(spec))` -> `if (0)`) | 4 interop checks |
| Not reattaching a trailing `/` | 1 interop check (`a.txt/`) |
| `arg_exists_in_worktree` always true | 6 interop checks |
| Removing the "both a revision and a file" rejection | 2 interop checks |
| `looks_like_spec` doesn't recognize `:` | interop **1 check, and only the message one** |
| **Reverse**: letting a wildcard-containing spec also take the directory-prefix rule | 3 interop checks (`su?`, `s*b`, `sub/dee?`) |

The last one is a **reverse mutation**, and the true focus of this round's verification: a rule
written **too broadly** has no symptoms at all, and every forward check stays entirely green. The
three "must be empty" checks for `su?`/`s*b`/`sub/dee?` exist for no other reason than to turn
this specific mutation red. Warning, recorded honestly: `o[tx]her`'s check did **not** go red --
not because it lacks discriminating power, but because this particular over-broad rule (comparing
spec against the path's first `slen` bytes) just happens not to hit it; a different over-broad
formulation would. **One mutation staying green only proves that mutation didn't reach that spot,
it doesn't prove that dimension lacks coverage.**

The second-to-last row is equally worth recording: with `:` removed from
`sg_pathspec_looks_like_spec`, `sg diff :(icase)a.txt` still exits non-zero (falling through to
"ambiguous argument" instead), so the "magic is rejected" check stayed green -- only the "the
message states the reason" check went red. **Splitting "rejected" and "states why it was
rejected" into two separate assertions is the only source of observability for this mutation.**

There's also one positive control group (`phase28 control`): the unfiltered list really does
contain `a.txt`; after filtering it must be gone, while the path that was actually named must
still be present. Reason: all the `cmp` checks stay entirely green when "both sides print
everything" -- if the filter became a complete no-op, only this check would catch it.

Conflicted paths get a separate group: unmerged occupies two adjacent rows under
index-vs-working-directory, the only structure that could be "split apart" by the filter. Each of
the three comparison types is paired with one pathspec that selects the conflict and one that
excludes it, compared against git, plus a check that directly counts the rows (must be 2, one of
them a `U`).

### Deliberately not done this round

- **Magic pathspecs** are all rejected, not implemented.
- **`core.ignorecase`**: comparisons are always byte-for-byte, even on macOS's case-insensitive
  filesystem (measurement confirms real git is byte-for-byte in the same environment too:
  `git diff -- A.TXT` prints nothing for `a.txt`).
- **Pathspec for `sg stash show` and `sg status`**: both could accept the same `sg_pathspec`, but
  this round only wired it into `sg diff`.
- **Three or more revisions**: real git's `git diff HEAD HEAD HEAD` exits 0 (a combined-diff
  path); sg treats it as a usage error. A known divergence.
- Performance: filtered-out paths still get hashed. See above.

## Phase 29: rename detection (exact)

Renames used to always be printed as delete + add. This round made `sg diff` recognize renames,
with all six formats byte-for-byte identical to real git. **Only exact matches (identical
content) are handled**; git additionally uses a similarity score to find inexact renames, which
isn't done yet -- see the end of this section.

### Two things measurement overturned

1. **Pairing is a global content comparison, not `git mv`'s history.** After completely
   replacing `low.txt`'s content, git paired it with `sub/deep_new.txt` (the one with matching
   content), leaving the original `sub/deep.txt` as `D` and `low_new.txt` as `A`. A rename in
   git's model is "a pairing between the deletion set and the addition set," unrelated to how a
   file actually moved.
2. **The working-directory side has no renames at all.** For an unstaged rename, the new file is
   untracked and doesn't participate in the diff at all -- `git diff` / `git diff HEAD` only see
   `D`, and `git status` shows `D` + `??`. So the index<->working-directory builder can
   structurally never produce a rename row; `sg_diff_index_workdir` needs no special handling.

### The spec table (all measured against git 2.55.0)

| Format | What the rename row looks like |
|---|---|
| `--name-status` | `R100\told\tnew`, the score **zero-padded to three digits** |
| `--name-only` | Prints only the **new** path |
| `--numstat` | `0\t0\t<compacted pairing>` -- the two paths crammed into **one column**, not two |
| `--stat` | The name column holds the compacted pairing |
| `--shortstat` | Unaffected |
| Patch | `diff --git a/old b/new`, `[old/new mode]`, `similarity index N%`, `rename from/to`; at 100%, **no `index` line and no hunk** |

Sorting is by **the new path** (a deletion row uses its own path), exactly the existing sort key
of sg's `sg_diff_list`, so no re-sort is needed after pairing -- only the source row is removed,
while the destination row keeps its original path.

`{a => b}` compaction computes both the prefix and suffix at **`/` boundaries**:

```
a/b/c.txt  -> a/z/c.txt     =>  a/{b => z}/c.txt      both prefix and suffix
h/i/j.txt  -> h2/i/j.txt    =>  {h => h2}/i/j.txt     suffix only
x/y.txt    -> x/y2.txt      =>  x/{y.txt => y2.txt}   prefix only
pre.txt    -> pre.txt.bak   =>  pre.txt => pre.txt.bak   bytes in common, but no shared "component"
```

Warning: the suffix loop must **scan to the very end, updating at every `/`** (taking the longest
component-aligned suffix), not stop at the first `/` -- the latter would print
`{h => h2}/i/j.txt` as `{h/i => h2/i}/j.txt`. That was exactly how the first implementation got
it wrong, caught by interop's `cmp`.
Warning: **a path needing C-quoting shuts off compaction entirely**: git prints
`d/plain.txt => "d/tab\there.txt"`, not `d/{plain.txt => ...}`. Quoting each fragment of the
bracketed form separately would generate quotes in the middle of a path, unparseable by any
consumer.

### Ordering: pathspec first, then rename detection

Measured: `git diff --cached --name-status -- b1.txt` (naming only the new half of the rename)
prints **`A b1.txt`**, not a rename; naming only the old half prints `D`; only when both halves
are given does it become `R100`. So git **filters by pathspec first, then detects renames**;
whichever half gets filtered out has no partner left to pair with.

`cmd_diff.c` is therefore `sg_diff_list_filter` -> `sg_diff_detect_renames`. Getting the order
backwards produces no compile-time or runtime symptom at all -- it would only give `R100` while
git gives `A` when a pathspec names only half of a rename. A directed mutation (moving detection
before filtering) turned 4 checks red.

### Pairing rule

Destinations, in path order, each claim **the first not-yet-claimed** source with matching
content. Measurement support: two identical sources paired with two identical destinations pair
up in order (`a1->b1`, `a2->b2`, no crossing); when one source matches two identical
destinations, the first destination claims it and the second falls back to a plain `A` (git
doesn't do copy detection by default, and measurement confirms even adding `-C` didn't detect it).

Warning: **the comparison uses the effective id.** `sg_diff_side_effective_id` used to be a
`static` inside `diff_out.c`; this round promoted it to a public function shared by both
consumers -- carrying an important contract: **two "unverified" ids being byte-identical is not
evidence of identical content.** The first version of rename.c wrote its own helper that fell
back to the raw id on parse failure, exactly the mistake that sentence warns against, which would
produce false renames. The failure direction now is "don't pair."

### Verification

31 checks were added to interop (1505 -> 1536), with nine directed mutations, eight caught. The
ninth is worth recording:

**Changing `R%03d` to `R%d` turned zero tests red** -- but this isn't a coverage gap, it's
**mathematically unobservable**: at present every rename is exact, with the score always 100, so
both formats print identically. Rather than just noting it, this was **made observable**:
`tests/test_rename.c` gained a new test that directly feeds a score of 93 into
`sg_diff_print --name-status`'s rendering test (that format doesn't read content, so a fabricated
list is sufficient). Rerunning the same mutation now turns it red, with the message showing
`R93`.

### The cost of adding a structural field, caught by ASan

Adding `old_path` and `score` to `sg_diff_entry` hit two things, neither a logic error:

1. **The Makefile has no header dependency tracking** (no `-MMD`/`.d`/`-include`). After changing
   a header, `make` only recompiles the `.c` files that were touched, leaving other TUs using
   `.o`s with the old layout, so `make test` crashed on a `strcmp` in `cmd_stash.c` -- looking
   exactly like a bug in newly written logic. `make clean && make` had all 50/50 passing, with
   not a single line of code changed. This has been written into CLAUDE.md's build section.
2. **Places that manually construct an entry can miss the new field.** `tests/test_diff_out.c`
   has two places that assign fields one by one after `malloc` (with no prior memset), so the new
   field was leftover malloc garbage, and `print_patch` dereferences `old_path`. **The normal
   build runs through fine; only `make sanitize` goes red** -- ASan reports
   `SEGV ... in sg_quote_path_prefixed`, at address `0xbebebebe` (the uninitialized-memory
   pattern).

Point 2 is exactly the value of CLAUDE.md's rule to "run `make sanitize` when touching memory
management": `make test` and interop were **both entirely green**, and only sanitize caught it.
Adding a field to a shared struct should count as "touching memory management."

### Cold-read review: one real bug, two properties with zero coverage

After merging, a round of third-party cold reading was added. The memory-safety side was clean
(the `ids` array indexing, `stolen` pointer transfer, the compaction loop, `sg_quote_path`'s
4-slot rotation, option parsing, and `sg_diff_side_effective_id`'s promotion were all
byte-for-byte identical), but it found three things:

**1. `rename_pair_display`'s overflow fallback silently prints wrong output (a real bug).** It
was written to write into a caller-provided `char[SG_PATH_MAX * 2]` (8192), while two paths of
4095 bytes each plus `" => "` total 8195 -- `snprintf` truncates, falling back, and that fallback
**returns the bare new path**, output that is **indistinguishable** from a plain added file --
the rename disappears entirely. And the comment even claimed the fallback was a "plain
`old => new` form", which was false. The reviewer confirmed this with an ASan probe actually
feeding in a 4095-byte path. Warning: this isn't a hypothetical input -- `src/object/tree.c`
doesn't validate length when parsing entry names, so a tree built with `git mktree` can supply
such a path, reachable by a pure tree<->tree `sg diff`. The fix was to **remove the fixed buffer
entirely**, allocating based on input length (returning an owned string). Adding quotes can
inflate the length by up to fourfold, and no fixed size could ever guarantee enough room.

**2. The safety property "an unverified id never pairs" had zero coverage.** That gate exists
for the reason that "two unverified ids being equal is not evidence of identical content," but
reaching it requires an unresolvable chunk pointer, and the chunking threshold is 64 KiB -- Phase
29's fixture files are only twenty lines each, and not a single interop check combines chunking
with renaming. The reviewer predicted "removing the verification would leave everything green."
Confirmed by running it. The added test needs no fixture: pointing `git_dir` at a nonexistent
directory makes the BLOB side's id fail verification, paired with a control group with **the
same id but going through the WORKDIR side** (whose id is a content hash and does verify),
proving "count == 2" isn't because detection just wasn't pairing at all. Rerunning the mutation
now turns it red.

**3. A rename row's OLD side is read using the** destination **path (latent, currently
unreachable).** Both `print_patch` and `build_entry_stat` feed `e->path` into
`sg_diff_side_read`. Harmless today because that parameter is only used by the WORKDIR side, and
none of the four builders can ever produce a WORKDIR `old_side`. Changed to `old_side_path(e)`.
Warning: after the change, a mutation **still stayed entirely green** -- no builder can produce
input that would distinguish the two. Per the criterion of "first ask whether this value could be
fed in from a lower seam," this is a coverage gap, not something unobservable: the test directly
fabricates an entry with a WORKDIR `old_side`, places two files with different content on disk,
renders the patch, and asserts the removal line comes from the **old** file. Rerunning the
mutation now turns it red.

Everything else: a half-width comma in "out of memory" changed to full-width (the file-wide
convention); the `free(old_path)` inside the compaction loop was confirmed by the reviewer to be
**always NULL** (whatever gets claimed is always the source, and `old_path` is only ever set on
the destination), and running the mutation confirmed it stayed green -- **this is a line of
"proven-inert" code, not a blind spot**, kept in place with the reasoning noted, the same pattern
as `sg_diff_list_filter`.

**`-M`'s syntax is a deliberately kept divergence after measurement**: real git accepts `-M0`,
`-M101`, `-M0.5`, `-M50%` (all rc=0), while sg always returns usage. The reasoning is that sg
currently has no inexact detection, and accepting an option whose semantics it can't deliver
would be worse than rejecting it; the failure direction is "loudly reject" rather than "silently
give a wrong answer." When inexact detection is done, the full grammar needs to be filled in.

### Deliberately not done this round

- **Inexact renames** (similarity scoring). This is the biggest gap, and the risk is already
  visible: to make `R093` and `similarity index 93%` byte-for-byte identical to git, git's
  `diffcore-delta.c` spanhash-counting algorithm would need to be replicated -- off by one point
  and the whole `cmp` goes red. Interop has two checks that **explicitly assert this
  divergence** (git gives `R0..`, sg gives `D`+`A`), plus a `--no-renames` control confirming the
  divergence is genuinely confined to detection and nowhere else -- once inexact detection is
  built, those two checks will go red and must be updated, not silently left wrong.
- **Copy detection** (`-C`). git has it off by default, and measurement confirms it doesn't
  detect it even with `-C` added; not done.
- **`sg status`'s rename row** (`R  old -> new`). `sg_status_diff_staged` is a **second
  implementation** of tree<->index (Phase 27 only converged the unstaged half), and it also
  feeds `apply.c`'s two safety gates -- stuffing renames into it would change the row count they
  see. Doing this correctly requires first enumerating divergences the way Phase 27 did, not
  adding it in passing.
- `-M<n>` only accepts an integer from 1-100, not git's decimals (`-M0.5`) or trailing `%`.

## Phase 30: rename detection (inexact)

`sg diff` now reports a rename plus an edit as a rename, with git's own similarity score:
`R086`, `similarity index 86%`. Phase 29 had only exact detection, and left two interop checks
asserting that gap on purpose so that closing it could not pass unnoticed -- those two failed on
the first build of this milestone, which is exactly what they were for.

### The score is a machine-readable field, so "close" is wrong

This is the Phase 26 situation (LCS vs Myers) with the escape route removed. There, a 2-3%
residue of hunk-placement differences was acceptable, because the disagreement lived in *where*
identical content was shown. Here the disagreement would print as `R086` versus `R085`, in a
field tools parse. One point off and every byte-for-byte `cmp` in interop goes red -- correctly.

So the estimator (`src/util/similarity.c`, `include/sg/similarity.h`) is a deliberate port of
git 2.55.0's `diffcore-delta.c`, not an independent design. The header says so and says why: an
estimate that is better but different is, here, simply wrong.

### How it was verified before a line of C was written

Reading an algorithm and believing you have read it correctly is the failure mode this project
keeps rediscovering. The sequence used instead:

1. The real `diffcore-delta.c` / `diffcore-rename.c` / `diff.c` were fetched at tag `v2.55.0`,
   the version installed on this machine -- not recalled, and not read from `master`.
2. The algorithm was re-implemented **in Python** and run against real git on random file pairs
   spanning text, CRLF, binary (NUL), no-newline-at-all, and byte-identical inputs. First run:
   **200/200**. Widening to five seeds surfaced 9 mismatches, all of one shape -- and they were
   not an algorithm error at all: `mutate()` had occasionally produced content identical to the
   source, which git settles in its **exact** pass and never scores. Modelling that: **750/750**.
3. Only then was the C written, and it was cross-checked against the *Python*, not against git,
   on 1200 more random pairs including empty buffers and a file of nothing but newlines:
   **1200/1200**.

Two independent ports agreeing is the evidence; either one alone would only have proved it
agrees with itself. Step 2's detour is the reusable lesson: **a differential harness that finds
mismatches has not necessarily found bugs**, and the three mismatch shapes here (score wrong /
pairing wrong / not scored at all) had to be told apart before any of them meant anything.

### Three passes, and the order is observable

git runs exact detection (by object id), then a **basename** shortcut, then the full matrix, and
this is not an internal optimization that could be collapsed into "score every pair and keep the
best". The basename pass pairs files sharing a name at a **raised** threshold --
`min + 0.5 * (MAX - min)`, i.e. 75% by default -- *without ever comparing them to anything else*.
git's own comment concedes the result may be sub-optimal and keeps it.

Two fixtures pin this down, and they deliberately disagree about which side wins, so no single
wrong threshold satisfies both (both measured against git 2.55.0):

| fixture | same-name pair | rival | git's answer |
|---|---|---|---|
| `p30_basename`     | `dir1/foo.txt` -> `dir2/foo.txt`, 79% | `other.txt`, 98% | the **name** wins, rival left `A` |
| `p30_basename_low` | `d1/x.txt` -> `d2/x.txt`, 60%         | `zz.txt`, 98%    | the **score** wins, name pair left `A` |

Three further tie-breaks, each reachable only through a built-on-purpose fixture and each
measured:

- **Exact ties go to the file name, not to path order.** Two identical sources `a/g.txt` and
  `b/f.txt` against `c/f.txt`: git takes `b/f.txt`, the one it meets second.
- **Only the best FOUR sources per destination are ranked** (`NUM_CANDIDATE_PER_DST`), and that
  table is sorted **stably**. So a tie is settled by which *slot* a candidate was written into,
  which stops matching the order candidates were considered in as soon as an eviction has moved
  one. Five sources scoring 50/60/89/80/89 make the two come apart: git answers `s5.txt`, and
  ranking by discovery order would answer `s3.txt`. `record_if_better` therefore copies fields
  one at a time rather than assigning the struct, so that a slot's position is structurally
  incapable of being overwritten by a candidate's.
- **Equal scores are separated by whether the source shares the destination's file name.**
  `a/y.txt` and `b/x.txt`, both 59%, against `c/x.txt`: `b/x.txt` wins despite sorting later.
  The 59% is chosen on purpose -- above the 50% an ordinary pair needs, below the 75% the name
  shortcut demands -- so the pair reaches the matrix instead of being settled beforehand.

### `-M<n>` is a fraction, not a percentage

The old parser took a plain integer 1-100 and rejected everything else, which Phase 29 recorded
as a deliberate divergence. Filling it in turned up a rule that is the opposite of the obvious
reading. Measured on a fixture scoring 86%:

| argument | means | rename found? |
|---|---|---|
| `-M`, `-M0`, `-M%` | use the default (50%) | yes |
| `-M5`, `-M50`, `-M0.5`, `-M50%` | 50% | yes |
| `-M05` | 5% | yes |
| `-M0.5%` | 0.5% | yes |
| `-M9`, `-M90` | 90% | no |
| **`-M100`** | **10%** | **yes** |
| `-M100%` | 100%, exact renames only | no |
| `-M86%` / `-M87%` | 86% / 87% -- either side of the fixture | yes / no |

`-M100` meaning ten percent is the trap: it reads like "exact renames only" and is nothing of
the sort. The grammar lives in one place, `sg_similarity_parse_score`, next to the scale it
produces, and `tests/test_similarity.c` writes the whole table out as numbers.

This is also why the threshold is carried on **git's 0..60000 scale** rather than as a
percentage: `-M005` asks for 0.5%, which a percentage cannot hold. Only `sg_diff_entry.score` is
a percentage, converted once at the very end by `sg_similarity_percent` -- which **truncates**,
so 59999 prints as 99%, not 100%. Rounding there would let a near-miss claim the `R100` that
means "byte-identical".

### Text vs binary changes the score of a file against itself

The CR of a CRLF pair is skipped when hashing text, but it is **not** subtracted from the file's
size. So a CRLF file scores about 66% against a byte-identical copy of itself; add a single NUL
byte and the same bytes are binary, the CRs hash like any other content, and the same comparison
is a perfect match. `tests/test_similarity.c` keeps that pair adjacent -- one byte apart, 34
points apart -- because a port that drops the text/binary decision cannot stay green against it.

This is also *why* git settles exact renames by object id before scoring anything: without that
pass, renaming a CRLF file with no edit at all would print `R066`.

### A bug that only became reachable now

`sg diff`'s patch output named the **new** path on the `--- a/` line of a rename, and likewise on
`Binary files a/... and b/... differ`. Nothing had ever caught it because an exact rename is
byte-identical and therefore prints no body at all -- so no rename had ever emitted those lines.
The first inexact rename did. Fixed in `print_text_diff_body`, which now takes both paths.

The general shape is worth keeping: **a code path that is currently unreachable is not a code
path that is correct.** Phase 29's cold read made the same point from the other direction.

### What the mutation round found

Twenty-six mutations, per-site rather than batched, ending with **every one caught**. Getting there
was the useful part:

- **One mutation expressed nothing.** Deleting the line that restored a slot's position looked
  like it inverted the ordering rule, but the candidate's own `seq` was a dummy zero, so the
  mutation was very nearly a no-op. It stayed green not because the property was uncovered but
  because *the mutation did not test it*. Reversing the comparison instead caught it at once.
  A green mutation is a claim about the mutation as much as about the tests -- and this one
  would have been filed as a blind spot by anyone reading only the verdict.

  The fix went further than the test: `record_if_better` now copies fields one at a time instead
  of assigning the struct, so that a slot's position is *structurally* incapable of being taken
  from a candidate. The dummy field that made the bad mutation possible is gone as a hazard.

- **Five genuine blind spots were closed rather than recorded**, each by building a fixture that
  reaches it -- and in every case **real git was measured first**, which is the only reason the
  new assertions are anchored to anything rather than to this implementation's own output:

  | property | fixture needed | git's answer |
  |---|---|---|
  | exact ties prefer the file name | 2 identical sources, dest sharing the later one's name | `b/f.txt` |
  | only four candidates are ranked, stably | 5 sources scoring 50/60/89/80/89 | `s5.txt` |
  | equal scores break on the file name | 2 sources at 59%, one sharing the dest's name | `b/x.txt` |
  | a tie does not displace what is held | 6 sources all scoring 59% | `t1.txt` |
  | the hundred-alternative cap | 101 identical sources, the last sharing the dest's name | `s001.txt` |
  | a repeated name declines the shortcut | 2 sources both called `x.txt`, one at 80%, one at 98% | `b/x.txt` |

  The last one is the one worth keeping: the port assumes git walks those sources in ascending
  order, an assumption about a hashmap traversal inside git that nothing else could check. The
  fixture answers it, and would answer it again against a future git.

  The lesson from Phase 29 held up exactly: **"this cannot be reached right now" is not a reason
  not to test it** -- ask instead what input would reach it. Every one of these five looked
  unreachable until the fixture was designed backwards from the mutation.

- The mutation runner **exited 0 twice while running nothing at all** (a mis-typed test name, and
  a stale working directory), printing its error only in the body of the output. Reading the exit
  code alone would have scored a whole batch as "all caught". The project's standing warning that
  verification tooling fails in the "already verified" direction earned its keep again.

### What the cold read found

A reviewer was given the finished diff with instructions to try to break it, and cross-checked
every arithmetic expression against git's own source. It found no memory-safety defect and no
divergence from git, and three things worth acting on -- two of which were contract problems
invisible from the outside:

- **An allocation failure during scoring was reported as "not a rename".** `load_cand` collapsed
  "this content cannot be read" and "malloc failed while hashing it" into one answer, and the
  first of those is *supposed* to mean no rename. So under memory pressure `sg diff` would have
  quietly found fewer renames and returned success, while the header promises -1 on allocation
  failure. The two are now distinct all the way up: `score_pair` returns -1 for it, and both
  passes propagate. This is the milestone's own rule turned on itself -- the failure direction of
  a scoring failure must never be a silently different answer.
- **`sg_diff_side_read`'s -2 is folded into "cannot be read" here**, which is a documented
  prohibition everywhere else. It is right in this one place, and now says why in full: the
  prohibition exists because diffing a chunk pointer's raw bytes produces meaningless hunks, and
  nothing here diffs anything -- and no diagnostic is lost either way.
- **One coverage gap**, the branch where a file name is repeated and the shortcut must decline
  to guess. Closed, measured against git first, and the mutation confirms it (removing the
  duplicate check answers `a/x.txt` at 80% instead of `b/x.txt` at 98%).

One more thing was fixed on the way, not from the review: the compaction that drops claimed
sources used to recognise them by the NULL path they had been left with. It now records them
explicitly, so an entry that somehow arrived with no path is not silently deleted along with
them.

### Three defences with no test, stated rather than implied

- **The rename limit** (`SG_RENAME_LIMIT`, git's `diff.renameLimit` default of 1000) is not
  covered. Reaching it needs more than a million candidate pairs, and the honest problem is that
  a fixture that big would not *observe* anything: with no real content behind those paths every
  pair scores 0 and the answer is "no renames" either way. Making it observable needs ~2000 real
  files whose contents actually match. Recorded rather than built.
- **The exact pass is O(sources x destinations)** with a `memcmp` per pair, where git uses a hash
  table -- and the rename limit guards only the matrix pass, so a changeset with very many adds
  and deletes pays that quadratic cost before any limit applies. This shape predates the
  milestone (the Phase 29 loop was the same), so it is not a regression, but it is now the only
  unbounded cost left in detection.
- **The allocation-failure path added during the fix round cannot be exercised at all.** Its two
  mutations can only fire inside a real `sg_spanhash_build` failure, and this project has no
  fault injection anywhere -- so the propagation was verified by cold reading and nothing else.
  That is the third of the three reasons a mutation stays green, and it is the one that must be
  written down rather than filed next to a coverage gap: there is no test to go and add.

  What *is* covered is the other half of the same branch, and the distinction matters: mutating
  "unreadable content" into a fatal error reddens three named checks, so the code does prove it
  tells the two apart -- just not that the fatal side reports itself correctly.

One mutation was caught only by a **segfault**, with no FAIL line: indexing `claimed[]` by slot
instead of by list entry leaves a NULL path in the list and the next `strcmp` dies. The runner
counts a non-zero exit as caught, and it is right to -- but a crash only proves that breaking
this causes trouble, not that any named assertion watches the property. Recorded as such.

### Still not done

- **Copy detection (`-C`)**: not implemented, and `cmd_diff.c` has no branch for it at all -- it
  falls into the generic usage error. The estimator and the pass structure would both support it;
  what is missing is the source culling that copy mode changes.
- **`sg status`'s rename row**: unchanged from Phase 29. `sg_status_diff_staged` is still a second
  implementation of tree<->index and still feeds `apply.c`'s two safety gates, so renames cannot
  be added to it without first enumerating divergences the way Phase 27 did.
- **`diff --cc`** for conflicted paths, and the last 2-3% of Myers hunk placement, both unchanged.

## Phase 31: `sg stash show` gains rename detection

Phase 30 gave `sg diff` git's full rename detection, but nothing else was wired to it: within
`src/`, `sg_diff_detect_renames` had exactly one caller. So `sg stash show` still printed a
rename as two rows -- and unlike most gaps of this kind, this one was visible in the **default**
output, since `git stash show` defaults to `--stat`:

| | `stash show` (default `--stat`) | `stash show --name-status` |
|---|---|---|
| git | `src2.txt => dst2.txt \| 20 ---`, one row | `R079 src2.txt dst2.txt` |
| sg (before) | `dst2.txt` + `src2.txt`, two rows | `A dst2.txt` + `D src2.txt` |

The fix is one call, and the whole milestone is about *where* it goes.

### `-u` merges first, then detects once -- and that is observable

Measured against git 2.55.0 with a fixture built to make the two orderings disagree: a tracked
file renamed and edited down to 79%, plus an **untracked** file whose content is byte-identical
to the original tracked file.

```
git stash show --name-status        R079 tracked.txt tracked2.txt

git stash show -u --name-status     A    tracked2.txt
                                    R100 tracked.txt untracked_a.txt
                                    A    untracked_b.txt
```

Adding `-u` does not add a second, separate diff. It merges the tracked and untracked halves into
one list and runs detection **once over the whole thing** -- so the untracked file takes the
source through the exact pass, and the real inexact rename beside it is demoted to a plain `A`.

That needs no special case. It is the Phase 30 pass order (exact before inexact) applied to a
list that happens to span both halves; the only thing the code has to get right is calling
detection *after* the merge rather than inside either half. The mutation that moves the call
before the merge reddens **exactly one** interop check, the `-u` one, and leaves the other seven
green -- which is the shape a well-aimed check should have.

`--only-untracked` compares an empty tree against the untracked tree, so every row is an addition
and there is nothing to pair; it is covered anyway, because "no renames here" is a claim too.

### One CLI-facing copy of the `-M` grammar

`git stash show` accepts `-M`, `-M<n>`, `--find-renames[=<n>]` and `--no-renames`, all measured.
Rather than let `cmd_stash.c` grow a second copy of the wrapper `cmd_diff.c` had around
`sg_similarity_parse_score` (consume the whole argument, reject leftovers, turn a parsed 0 into
the default), the wrapper was promoted to `sg_similarity_parse_score_arg` and both commands call
it. CLAUDE.md's standing rule is that the known-duplication list must not grow back; the cheapest
moment to obey it is the moment a second caller appears.

### Verification

Twenty new interop checks, all byte-for-byte against real git across the six formats plus `-u`,
`--only-untracked`, `-M100%`, `-M87%`, `--no-renames` and a rejected `-Mabc` (message included,
not just the exit code). Each behavioural claim is paired with an **oracle** check asserting that
real git does the thing being compared against -- including the `-u` steal, so that a future git
changing its mind is reported as a changed oracle rather than as an sg regression.

Five mutations, all caught: detection not running at all (8 checks red), detection running
before the merge (1 check red, the right one), the default threshold silently becoming
`--no-renames` (8 red), the shared parser no longer turning a parsed 0 into the default, and the
`-M<n>` value being read from the wrong offset.

The cold read found no defect but did find three coverage gaps, and one of them is worth keeping
as a rule: **`sg stash show`'s bare `-M` and `--find-renames` never reach the shared parser at
all** -- both branches assign the default directly, exactly as `cmd_diff.c` does -- so no amount
of coverage on the `sg diff` side protects them. Sharing a function does not share its tests
when the call sites short-circuit around it. All three gaps were closed: the default spellings
(`-M`, `--find-renames`, `-M0`, `-M%`) now have their own `stash show` checks, and
`sg_similarity_parse_score_arg` has a direct unit test now that promoting it made it reachable
from `tests/` at all.

The stash itself is created with `sg stash -u` and then read back by both sides, the same
discipline as the Phase 25 stash-show block: sg's stash commit is not guaranteed byte-identical
to git's, so building it twice would compare two different stashes and call the difference a bug.

## Phase 32: `sg status` gains a rename row, and the last duplicate walk goes

`sg status` now prints `renamed:    old -> new` and `R  old -> new`, the last place rename
detection had not reached. Getting there meant finishing what Phase 27 started: the staged half
of `sg status` was still a hand-rolled walk of HEAD-tree vs index, a second implementation of
what `sg_diff_tree_index` already did, and rename detection had nowhere to attach to it.

### The convergence, done Phase 27's way

The rule Phase 27 set was: do not converge first and check afterwards. So the differential
harness came first (`tests/test_status_staged_parity.c`, written against the *unchanged* code):
12 named shapes -- clean, content-only, mode-only, deleted, added, all three unmerged
arrangements in both tree-has-the-path and tree-does-not variants, a corrupt duplicate-path
index, an unborn HEAD, and the `a` / `a/b` / `a.txt` / `ab` byte-ordering boundary -- plus a
seeded fuzzer combining up to 16 independently-shaped paths per round.

It found **no divergence at all**: 0 classes at 200, 1000 and 5000 rounds.

That is a claim about the harness as much as about the code, so it was not taken on trust. The
harness self-checks by forcing its own normalisation to swap NEW and DELETED, and -- more to the
point -- four mutations were run against the **product** code:

| mutation | result |
|---|---|
| drop `path_has_unmerged_stage` (the false-deletion guard) | caught, named + fuzz |
| ignore the mode half of the comparison | caught, named + fuzz |
| report a deletion as a modification | caught, named + fuzz |
| stop skipping stage 1/2/3 entries | caught, named + fuzz |

So "equivalent" is a measurement, not an absence of evidence, and the swap was a pure refactor.
`path_has_unmerged_stage` went with the walk it served: `sg_diff_tree_index` reaches the same
answer through its per-path group cursor, which the harness had already confirmed on all three
unmerged shapes.

### Who is allowed to see a rename

The obstacle named in Phase 29 was that this list also feeds `apply.c`'s two safety gates. Reading
them settled it: both use `.count` for the dirty decision and `.path` to tell the user what is
uncommitted, and neither reads `.kind` at all (grepped, 0 hits). So collapsing a delete plus an
add into one row cannot flip either gate -- but it *would* silently stop the message naming the
old path, because that loop prints one path per row.

`rename_score` is therefore a **mandatory parameter with no default**, the same idiom as
`sg_workdir_missing`: the gates pass 0 and keep exactly the list they have always had, and
`cmd_status.c` passes the threshold. Silently picking one side for both is the bug the parameter
exists to prevent.

A rename is carried as `old_path != NULL` rather than a fourth `sg_status_kind`, so every
existing `switch` stays exhaustive, and a renamed row's kind is `SG_STATUS_MODIFIED` -- a
consumer that knows nothing about renames still sees "this path changed", never "nothing here".

### What real git does, measured

| | git 2.55.0 |
|---|---|
| long format | `renamed:    old -> new`, label column 12, same as `modified:` |
| porcelain / `-s` | `R  old -> new` |
| row order | by the **new** path, same as `git diff --name-status` |
| rename + chmod | absorbed into the one row, no separate mode line |
| staged rename, new path then deleted | one line, `RD old -> new` |
| a path with a space | porcelain quotes **both halves**; the long format quotes neither |
| working-tree move, not staged | ` D old` + `?? new` -- the unstaged half does **not** pair |
| `-Mabc` | **exit 0, quietly the default** |

Two of those are worth keeping in mind. The unstaged half never pairs renames, which is what
kept this milestone to one function instead of two. And `git status -Mabc` succeeding is the
exact opposite of `git diff -Mabc`, which exits 129 -- the two commands do not share a rule, so
sg matches them one at a time rather than routing both through the shared parser's
reject-leftovers behaviour.

### A mutation that was not a coverage gap

Removing the loop that carried a group's `old_path` forward left interop fully green, which reads
like a missing test. It was not. Porcelain groups rows by path, a path can carry a staged row and
an unstaged one, only the staged row holds `old_path` -- and `prow_cmp` compared paths alone, so
**`qsort` was free to order those two either way**. Merging `x`/`y` across a group is
order-independent, which is why this never mattered before; `old_path` is not.

The fix was not to write a test for the forwarding loop but to remove the freedom: `prow_cmp`
now breaks ties on append position, making the order total and the staged row always first. The
forwarding loop then became provably dead and was deleted.

Reversing the new tiebreak is caught (6 checks). Deleting it outright is **not**, and cannot be:
it does not produce a wrong answer, it produces an unspecified one that happens to coincide on
this libc. That is the third of the three reasons a mutation stays green, and it is recorded
here rather than filed as a gap, because there is no fixture that would close it.

### One behaviour did change, and it changed for the better

The convergence was meant to be answer-for-answer identical, and for every input the harness
covers it is. One input it does not cover behaves differently: a HEAD tree that cannot be read.

The old code called `sg_tree_flatten` and ignored its return value, so a tree that failed partway
left a *partial* flattened list, and the walk then reported every index entry it no longer had a
counterpart for. For `sg status` that meant silently printing a wrong answer; for `apply.c`'s
gates it was a theoretical fail-open, since differences past the failure point simply vanished.
The adapter turns any failure into -1, which every caller already treats as dirty.

So `sg status` now prints an empty staged section and a warning where it used to print a
confident wrong one, and both gates became strictly more conservative. The warning's wording was
corrected too: it used to blame memory, which is only one of the two reasons it can fire.

### The fixture that tested nothing

Eight of the first interop run's failures were the **oracle** checks -- the ones asserting that
real git does the thing being compared against. The fixture used `sg add -A`, which sg does not
have; nothing was staged, and sg and git agreed perfectly about a state containing no rename at
all. Every byte-for-byte check was green while testing nothing.

This is the second time in three milestones that pairing each behavioural claim with an oracle
check is what caught a fixture problem rather than a code problem. A `cmp` against real git
proves the two agree; only the oracle proves they agree *about the thing you meant*.

## Phase 33: copy detection (`-C`)

`sg diff -C` finds a file that was copied from another, printing `C079 src.txt copy.txt`,
`copy from` / `copy to`, and the same `a => b` column `--stat` already used for renames. The
estimator and the three passes were all in place from Phase 30; what was missing was the part
copy mode changes, which turned out to be more than "let a source be used twice".

### The rule is one line, and it explains four counter-intuitive answers

Measured first, then confirmed against git's source (`diff_resolve_rename_copy`):

```c
else if (--p->one->rename_used > 0)  p->status = DIFF_STATUS_COPIED;
else                                 p->status = DIFF_STATUS_RENAMED;
```

A source is paired some number of times; each paired destination spends one use, **in path
order**; a destination is a copy exactly while uses remain after its own. A source that is not a
deletion is charged one use when it is registered, so anything copied off an edited file is a
copy by construction.

That single rule accounts for every measurement, including the ones that look wrong:

| | fixture | git 2.55.0 |
|---|---|---|
| A | untouched source, exact copy | `A copy.txt` -- **plain `-C` finds nothing** |
| B | edited source, 80% copy | `C079 src.txt copy.txt` **and** `M src.txt` -- the source stays |
| C | deleted source, two exact copies | `C100 -> c1.txt` **and `R100 -> c2.txt`** |
| D | deleted source, a 79% same-name match and a 98% other | `C079 -> dir2/foo.txt`, `R098 -> other.txt` |

C and D are the same surprise: with one source and two destinations, the **first by path** is the
copy and the second is the rename -- **however much better the second matched**. D makes it
sharper still, because it is the exact fixture Phase 30 built to prove the same-file-name
shortcut exists, and `-C` answers it differently: copy mode skips that shortcut entirely.

The model was written down and checked against all four before a line of code was written. That
ordering is the point -- a rule derived from one fixture would have fit C and been wrong about B.

### What `-C` actually changes

Three things, none of them "allow reuse" alone:

1. A path present on **both** sides becomes eligible as a source, which is the only way B can be
   found at all.
2. The same-file-name shortcut is skipped.
3. The ranked matrix is walked **twice**: once refusing already-claimed sources, then again with
   that refusal lifted. One walk that allowed reuse from the start would hand a destination to a
   claimed source before an unclaimed one had its turn.

### `-C -C` is refused, not approximated

`--find-copies-harder` additionally offers every **unchanged** path as a copy source -- which is
why plain `-C` cannot find case A. `sg_diff_list` only ever holds paths that changed, so there is
no source pool to draw from without teaching all four builders to emit unmodified rows.

sg therefore **rejects** it with a named error, and interop pins the divergence from both sides:
sg exits non-zero, and an oracle check asserts real git exits 0. Answering it as plain `-C` would
have been the one failure direction this codebase does not allow -- a quietly different answer to
the question that was asked.

### Two properties that hid behind other machinery

Seven mutations, all caught in the end -- but two only after building fixtures for them, and
both were green for the same reason: something else in the pipeline reached the right answer
anyway, so breaking the rule had no symptom.

- **Copy mode skipping the same-file-name shortcut** is invisible with one source. Copy mode
  lets a source be reused, so claiming it early through the shortcut costs nothing and the
  matrix arrives at the same pairing. It takes TWO sources -- one sharing the destination's name
  at 79%, one not at 98% -- before the shortcut changes the answer. Measured: without `-C` the
  name wins, with `-C` the score does.
- **The exact pass letting a source be claimed again** is invisible because the matrix's second
  walk finds the same pair regardless. The exception is `-C100%`, where the matrix is skipped
  entirely and the exact pass is all that is left.

Neither was a missing rule; both were rules with no witness. The distinction matters, because
"the mutation stayed green" reads as "nobody implemented this" and here it meant "nobody could
see it from where the tests stood".

### The one thing copy detection broke, and how

Every outcome this function had before moved pointers around: a rename hands the source's path
to the destination, the source row goes away, nothing is allocated. A COPY cannot do that -- the
source row stays and keeps owning its path -- so it has to duplicate it, and duplication can
fail. Written as one loop, a failure partway through left rows already rewritten and, worse, a
source row whose path had been taken away, i.e. an entry with a NULL path in a list handed back
to the caller.

The header promises `-1` leaves the list untouched, and it had been true for free. It is now true
on purpose: pass one decides and allocates, touching nothing but local state; pass two writes the
list and cannot fail.

Worth noting how it was found. The comment sitting on that loop still read "the source's path is
handed over rather than copied, so this cannot fail partway and leave the list half-rewritten" --
a sentence that had been accurate for three milestones and was made false by the line added
directly beneath it. **A comment that explains why something is safe is also a test of whether
it still is**, and this one failed loudly on re-reading.

### What the cold read found

Two defects, both in places the tests had no reason to look:

- **`-C` was sticky.** `-M` and `-C` write the same mode field in git, so the last flag wins:
  `git diff -C -M` finds renames only, `-M -C` finds copies. sg's `-M` branches only ever touched
  the score, so `-C -M` stayed in copy mode. Measured against git 2.55.0 after the reviewer
  named it, then fixed. **No check combined the two flags** -- the coverage gap sat exactly where
  the defect was, which is the shape this log keeps recording.
- **`list_append` never set `is_copy`.** It is the one canonical constructor for `sg_diff_entry`,
  it assigns every other field by hand, and its storage comes from `realloc` -- so every entry
  every builder produced carried garbage in the new field. Harmless today only because both
  readers check `old_path != NULL` first, and invisible to every gate this project has: ASan is
  not MSan, and heap storage draws no compiler warning.

  Phase 29's lesson was "when you add a field, audit the sites that do NOT go through the
  constructor". This is the same lesson from the other side: **audit the constructor too.**

Both fixes were then mutated back to the broken version, since a fix a review found has, by
construction, no test standing over it until one is written. Both reddened, on the checks written
for them.

### Verification

Fifty-five new interop checks, every behavioural claim paired with an oracle assertion on real
git -- the discipline Phase 32 earned the hard way, when a fixture using a flag sg does not have
compared two identical rename-free states and passed. Four unit tests assert `is_copy` directly
rather than through rendering, each with a `detect_copies = 0` control so the tests cannot pass
by the feature simply being off.

## Phase 34: combined diff (`sg diff -c` / `--cc`)

Real git's `combine-diff.c`, ported from scratch and fixed at exactly 2
parents (ours = index stage 2, theirs = index stage 3, result = the
working-tree file) -- `sg diff` never needs an N-way merge, only the one
real git produces for an unresolved 2-way conflict.

### Where the expected values came from

A three-document handoff, same shape as Phase 33: an **oracle** (real git
2.55.0 measured, `LC_ALL=C`, both config env vars pointed at `/dev/null`)
covering when combined even applies (only plain `sg diff`, never `--cached`,
never with a `<rev>`), the flag grammar (`--cc` = dense = PATCH's *implicit*
default; `-c` = non-dense; last one wins), the header's exact byte layout,
and eighteen concrete samples pinned as test fixtures; an **algorithm spec**
derived from a cold read of `combine-diff.c` (`struct sline`'s flag bits,
`consume_hunk`/`consume_line`, `coalesce_lines`'s LCS merge of two parents'
deleted-line lists, `make_hunks`'s dense-mode pruning rule, and the funcname
suffix's off-by-one); and an **implementation spec** deciding sg-specific
placement (which struct grows, which file renders, three scope decisions
below). All three were cross-checked against measurement before code was
written, not the other way around.

### Three decisions made in advance, not revisited mid-implementation

1. **`-c`/`--cc` combined with a `<rev>` is rejected outright.** Real git
   switches to a completely different parent pairing there (stage 1 vs the
   named tree blob at that rev) -- approximating it with the stage-2/stage-3
   pairing this renderer implements would be a silently wrong answer, not a
   close one. Same treatment as Phase 33's `-C -C`: both sides of the
   divergence (`git diff -c HEAD` succeeds, `sg diff -c HEAD` is refused)
   are pinned by interop.
2. **`* Unmerged path <p>`'s path is never quoted.** Measured with `od -c`:
   real git lets a raw ESC byte in the filename go straight to stdout,
   `core.quotepath=false` has no effect on this one line. This is the
   fourth documented exception to "printing a path always goes through
   `sg_quote_path`" (CLAUDE.md).
3. **One milestone, not split into a data-layer half and a render-layer
   half.** `sg_diff_entry` already carried the right shape for the
   conflict row (Phase 20's `unmerged` flag, `list_append_unmerged` as the
   sole constructor) -- the only gap was that `print_patch` threw away the
   information needed to render combined instead of skipping the row, so
   splitting would have meant landing an inert data change with no renderer
   to prove it against.

### The two surprises the algorithm spec's translation had to resolve

- **git's `nb`/`nb-1` lost-bucket split (`consume_hunk`) collapses to one
  rule.** Read literally, a pure-deletion hunk hangs its lost lines on
  `sline[nb]` and a mixed add/delete hunk hangs them on `sline[nb-1]` --
  looks like two cases. Working through git's own 1-based "insert after
  line N" convention for hunk headers, both resolve to the SAME index:
  `sg_diff_group`'s own `b_off` (0-based, already what
  `sg_diff_build_script` hands back from the ordinary 2-way LCS engine).
  `combine_process_parent` uses `b_off` unconditionally; no case split
  survives in the port.
- **The 40-byte funcname scan needed a bound `combine-diff.c` does not
  spell out explicitly, but relies on.** Real git's scan loop stops on
  `if (!ch) break` -- a NUL sentinel at the end of its mmap'd/xcalloc'd
  buffer. sg's blob buffers are plain `malloc`'d with no such sentinel, so
  the scan is clamped to the bytes actually left in the result buffer
  instead; without the clamp this is a heap-buffer-overflow read, caught
  immediately by `make sanitize` on the fixture built to exercise it. This
  is **not a claimed output divergence** -- git's NUL-sentinel stop and
  sg's byte-count clamp land on the identical byte: binary detection
  already rules out an embedded NUL reachable by the scan, and a funcname
  candidate is by construction a line outside every hunk (the "not yet
  marked" scan in `combine_dump`), which structurally can never be the
  file's actual last line. Measured, not just argued: 216 targeted
  comparisons against real git (9 funcname lengths x 4 leading-context
  depths x with/without a trailing no-newline delete x 3 flag
  combinations) found 0 mismatches. Do NOT list this alongside the two
  genuine deliberate divergences above (rev-argument rejection, `* Unmerged
  path` unquoted) -- both of those are pinned on both sides by an interop
  check that asserts real git's OTHER behaviour; this one has no oracle-side
  pin because there is nothing to diverge on.

### Not ported: `reuse_combine_diff`

git skips re-running a parent's 2-way diff when two parents' blobs are
byte-identical (`reuse_combine_diff`, `combine-diff.c`), copying the
already-computed `p_lno`/`lost`/`flag` bits across instead. **Deliberately
not ported here** -- it is a pure performance optimization (the output is
provably identical either way: running the same 2-way diff against the same
`(parent, result)` pair twice produces the same `sline` contribution twice),
and sg's fixed 2-parent case pays for the redundant diff at most once per
conflicted path. Recorded explicitly so a future reader does not mistake
the omission for a missed step -- the ALGO spec this phase was built from
flagged it as "suggested to keep", and it was a deliberate call to drop it,
not an oversight.

### Review round: mutation results and the one genuine blind spot found

Seven mutations, run by the main conversation after a cold-read review of
the diff (`bash tests/mutate.sh`, `tests/gates.sh --interop`/named unit
binary as appropriate):

| # | Mutation | Result |
|---|---|---|
| m1 | Swap the ours/theirs (parent 0/1) columns | 9 checks turned red |
| m2 | Invert the dense-mode pruning condition | 3 checks turned red (including the hunk-count assertion, 2 vs 4 `@@@` markers) |
| m3 | Print the funcname suffix in full (undo the off-by-one) | 2 checks turned red |
| m4 | Remove the companion-row skip after a combinable unmerged row | 6 interop checks turned red |
| m5 | Make `-c`/`--cc` sticky (later flag does not override) | 1 check turned red |
| m6 | Quote `* Unmerged path`'s filename | **initially a blind spot** -- every existing interop check used an ASCII-only path, where quoting is a no-op; see the "head-on collision" fixture (control-byte filename `wei rd\ttab.txt`) added afterward, which turns this red on exactly the two checks that name quoting |
| m7 | Reverse `coalesce_lines`'s LCS tie-break | 2 checks turned red |

m7 was the one prediction that came out wrong: it was expected to stay
green (the tie-break only matters when the LCS has more than one optimal
alignment, which seemed unlikely to be pinned by any fixture), but sample
(D)'s empty-result fixture -- both parents' entire content deleted, forcing
`coalesce_lines` to interleave two full sequences -- happens to have exactly
the kind of tie the mutation flips, and it caught it. A useful reminder that
"probably not observable" needs the same measurement discipline as any
other claim in this file.

**`has_companion_row`'s five call sites are not equally covered by
mutation.** Only the `print_patch` site was hit by m4 above; the other four
(`print_name_only`, `print_name_status`, `print_numstat`,
`build_stat_rows`) share the same indentation and structure, so a
per-site mutation (per CLAUDE.md's "per-site vs batch" warning) cannot
distinguish them from each other by context alone. Their correctness is
verified by interop's per-format `p34_cmp` byte compares instead (each of
the five non-patch formats gets its own `-c`/`--cc` comparison against real
git in the Phase 34 A fixture) -- not by a dedicated mutation round. This is
recorded so the next person does not read "5 call sites, 1 mutation" as an
oversight.

### Attributing a fuzz_combined mismatch: diff-of-diffs is the wrong tool

`tests/fuzz_combined.py`'s 150-round baseline (measured 2026-08-28): 104
rounds produced a real conflict, 2 mismatched. Both were run down with a
throwaway attribution script (not checked in -- the method is what needs to
survive, not the script) that answers one question: **is the combined layer
introducing the divergence, or inheriting one that already exists one layer
down?**

The wrong way to answer that is to diff the two patches against each other
(`diff <(git output) <(sg output)`) and read the result as "these lines
changed". CLAUDE.md's `diff-of-diffs-misreads-shifted-groups` note applies
here directly: when sg and git pick different-but-equally-minimal hunk
splits for the SAME underlying content (the documented ~2-3% LCS-vs-Myers
residual, Phase 26), a line-level diff of the two outputs reports a
"content change" at every line whose position shifted, even though not one
byte of actual file content differs. Reading that as "combined diff has a
content bug" would be answering the wrong question.

The right way, used here:

1. **Multiset compare, not diff-of-diffs.** Strip both patches down to their
   content lines (drop `diff --cc`/`index`/`---`/`+++`/`@@@` header lines,
   strip the leading two-column `+`/`-`/` ` prefix from what remains) and
   compare as `collections.Counter` multisets. If they are equal, every line
   git printed and every line sg printed are the same SET of lines in a
   different arrangement -- a positioning disagreement, not a content one.
2. **Replay each parent-vs-result pair as an ordinary 2-way diff.** Take the
   real ours/theirs blobs and the actual working-tree result bytes from the
   failing round, commit each parent alone in a throwaway repo with that
   same result content on disk, and run plain `sg diff` against plain
   `git diff` on that 2-way comparison. If the 2-way diff ALREADY disagrees
   with git there, the combined layer is exonerated: it is feeding a
   correct algorithm (git's own, ported faithfully) a hunk split that
   already diverged before combining ever ran.

Both of the 150-round baseline's 2 mismatches satisfied both checks:
identical content-line multisets, and one of the two parent-vs-result 2-way
diffs already disagreeing with git on its own. 2/104 ~= 1.9%, inside the
documented 2-3% band. **No mismatch was attributable to the combined layer
itself.**

### Verification

Interop grew by 44 checks (1700 -> 1744): 39 from the original combined-diff
matrix, plus 5 from the review round's "head-on collision" fixture (item 1
above) -- all comparing sg's full byte output against
`git -c core.quotepath=false diff` on conflicts `sg merge` itself produced
(same "sg-made conflict, real-git oracle" idiom Phase 4b established for
`sg status`). `tests/test_diff_combined.c` adds ten unit tests built
directly on `sg_diff_entry` (bypassing `sg merge` for precise control over
ours/theirs/result content), covering column order, dense's per-hunk
pruning (not per-file), the LCS-coalesced empty-result sample, the
deleted-result no-hunk-body case, binary, the funcname off-by-one at both
the single-byte and 40-byte-cap edges, and the CLI's `-c`/`--cc`
last-one-wins ordering. `python3 tests/fuzz_diff.py 200 --max-failures 0`:
4 mismatches, unchanged from the Phase 27 baseline (combined diff is a new
code path the fuzzer's existing 2-way generator does not exercise, so this
number was not expected to move). `python3 tests/fuzz_combined.py 150`: 104
conflicts, 2 mismatches, both attributed to the pre-existing LCS-vs-Myers
residual (see above), not the combined layer. `make sanitize`: clean.

## Phase 35: alignment algorithm swap, LCS backtracking -> git's Myers

Replaces the O(na*nb) LCS-table alignment `sg_diff_build_script` used
(`src/util/diff_lcs.c`) with a direct port of git's actual algorithm:
`xdiff/xdiffi.c`'s Myers divide-and-conquer (`xdl_split`/`xdl_recs_cmp`) plus
`xdiff/xprepare.c`'s two pre-passes (`xdl_trim_ends`, `xdl_cleanup_records` /
`xdl_clean_mmatch`), both from git v2.55.0. This closes the last known
divergence in the patch body: the Phase 26 residual (2-3% of hunks disagree
on *positioning*, not content) tracked back to sg using an unreduced LCS
while git defaults to Myers, and the fuzzers below confirm the residual is
gone, not just reduced.

### Why the divergence existed in the first place

Phase 26 measured that of 11 residual cases, 6 were byte-for-byte identical
to `git diff --histogram` and reasoned "sg is accidentally closer to
histogram than to Myers". That was directionally right but incomplete: the
old LCS backtrack, given no size-reduction pass, finds *a* longest common
subsequence, and which one it picks when several tie is an artifact of the
backtrack's own tie-breaking rule -- it has no reason to agree with either
algorithm in general. Myers with git's exact heuristics is the only thing
guaranteed to agree with `git diff`, because it's the same code.

### Scope decisions (made in advance, not revisited mid-implementation)

1. **`xdl_cleanup_records`/`xdl_clean_mmatch` and `xdl_split`'s two
   heuristics were ported despite having no measured witness on this
   project's fixtures for a chunk of their internal logic.** The
   hypothesis going in was that histogram/patience skip the size-reduction
   pass while default Myers always runs it, and that this explained sg's
   histogram-like bias. That hypothesis was **refuted** before writing any
   code: on the 4 saved failing fixtures, `git diff` and `git diff
   --minimal` (which disables both the reduction pass and the split
   heuristics) produce byte-identical output; across 420 additional
   measured cases (360 random files at three sizes, 60 deliberately
   pathological ones built to trigger `xdl_clean_mmatch`'s specific
   discard condition) `git diff` and `git diff --minimal` never disagreed
   once. This part of git's own pipeline was never observed to change its
   own output, so sg cannot be observed disagreeing with it either. The
   code was ported anyway, because the project's completion bar is
   byte-for-byte agreement, not "agrees on everything we happened to
   measure" -- see "No witness, by design" below for the full scope of
   what this covers and the mutation evidence for each piece.
2. **`src/workdir/merge.c`'s three-way merge is untouched.** It calls
   `sg_diff_lcs_table`/`sg_diff_lcs_table_exact` directly and rolls its own
   backtrack (`merge.c:283-313`, `:364-365`), never going through
   `sg_diff_build_script`. Both public LCS-table functions are kept exactly
   as they were. Reason: every measured residual was in the diff path, none
   in merge (`tests/fuzz_diff.py` cannot reach merge's alignment at all),
   so changing merge's line-pairing behaviour would be an unforced change
   with no fuzzer watching it.
3. **`src/cli/diff_out.c`'s `coalesce_lines` (Phase 34's combined-diff LCS,
   a from-scratch port of `combine-diff.c`) is untouched too**, but for a
   different reason than merge: it already has its own separate LCS table
   and never called into `diff_lcs.c`. `fuzz_combined.py`'s pre-existing
   residual (tracked in Phase 34 as "attributed to the LCS-vs-Myers gap")
   is fixed as a side effect, because `combine_process_parent`
   (`diff_out.c:852`) calls `sg_diff_build_script` for the two parent-vs-
   result comparisons that feed `coalesce_lines`, not because
   `coalesce_lines` itself changed.

### The coordinate mapping (the part most likely to be wrong)

`xdl_cleanup_records` builds a `reference_index[]` per file: only lines
classified `KEEP` (or `INVESTIGATE` demoted to `KEEP`) get an entry, so the
Myers core (`myers_recs_cmp`/`myers_split_box` in `diff_lcs.c`, mirroring
git's `xdl_recs_cmp`/`xdl_split`) runs entirely in a *compacted* coordinate
space `[0, nreff)` that skips every line already resolved as
obviously-unmatched. `changed[]`, however, is indexed by the *original*
line number, because that is what `compact_one_side` (Phase 26, untouched)
and the final `sg_diff_group` output need. The two array-of-longs
`aref`/`bref` are exactly this mapping (compacted index -> original index),
and every site that writes into `achanged`/`bchanged` from inside the Myers
core goes through them (`mc->achanged[mc->aref[off1]]`, not
`mc->achanged[off1]`). This is a SEPARATE path from `cleanup_side`'s own
direct write `changed[i + off] = 1` for a line it classified `DISCARD`
before Myers ever runs -- the two write into the same bitmap but from
different call sites, and a mutation review round confirmed they need
separate tests: `tests/test_diff_myers.c` has
`test_myers_aref_mapping_second_dup_deleted`/
`test_myers_bref_mapping_second_dup_inserted` for the Myers/aref path
(a duplicated common line forces the Myers core to decide *which*
occurrence is the edit, expected values checked against `git diff
--no-index` on the same 3/4-line fixtures), and a separate
`test_discard_path_coordinate_mapping` for `cleanup_side`'s own `+ off`
write (two unique-content lines wrapped in a common prefix long enough to
give `dstart != 0`, so a lost `+ off` is observable). All three were
independently confirmed via `tests/mutate.sh` to catch exactly their own
path and not the other: mutating `mc->aref[off1]`/`mc->bref[off2]` down to
bare `off1`/`off2` turns only the two Myers/aref tests red, and mutating
`changed[i + off]` down to `changed[i]` turns only
`test_discard_path_coordinate_mapping` red (plus, incidentally,
`test_trim_only_end_differs`, whose own fixture happens to also have
`dstart != 0` -- see that test's comment for why `test_trim_only_start_differs`
cannot see the same bug).

### No witness, by design: the size-reduction pass and the two split heuristics

Three pieces of the port have no test in this project that fails when they
are individually broken, and this is a deliberate, disclosed gap rather
than an oversight -- see scope decision 1 above for why the code stayed
anyway. All three were checked with `tests/mutate.sh` and, for the two
confirmed blind spots, an independent re-check with `tests/fuzz_diff.py`
directly (copying the mutated working tree to a scratch directory and
running the fuzzer's own `PROJECT_ROOT`-relative binary, the same
mechanism `mutate.sh` uses for `make test`):

- **`cleanup_side`'s `other_is_b ? count_b : count_a` (which file's match
  count decides KEEP/DISCARD/INVESTIGATE) is a blind spot for
  `tests/test_diff_myers.c`** (swapping the two branches: exit 0, no FAIL
  lines) **but IS caught by the differential fuzzer**: 8/500 mismatches at
  `--seed 0`, 7/500 at `--seed 20000`. This one is not actually
  witness-less overall -- it just has no *unit-level* witness, only a
  statistical one. Recorded here so the next person does not "fix" the
  blind spot by deleting fuzzer coverage that is already doing the job.
- **`xdl_split`'s heuristic A forward/backward asymmetry
  (`k == snake_cnt` forward at `:597`, `k == snake_cnt - 1` backward at
  `:620`) is a genuine blind spot for both**: forcing the backward branch
  to match the forward one produces 0/500 mismatches at `--seed 0` and
  0/300 at `--seed 111111`, on top of the pre-existing 0/500 at `--seed
  20000/40000/60000` this phase's baseline already established. Nothing in
  this project's test suite would catch this asymmetry regressing to a
  copy-paste "fix" that makes the two match.
- **`bogosqrt`'s shift amount (`n >>= 2`, used identically for both
  `cleanup_side`'s `mlim` and the Myers core's `mxcost`) is the same kind
  of blind spot**: changing it to `n >>= 1` (a real behavioral change --
  it moves the size-reduction/heuristic trigger thresholds, not a no-op)
  produces 0/500 mismatches at `--seed 0` and 0/300 at `--seed 111111`.

The common thread, and the reason none of this is surprising: **`git diff`
and `git diff --minimal` were never observed to disagree** across the 420
cases scope decision 1 describes. `--minimal` is exactly "skip the
size-reduction pass and both split heuristics" -- so on every input this
project's fuzzers or manual construction have produced, git's own pipeline
never took a path where any of this code changed its answer. sg inherits
that same blind spot by construction, not by a gap in its own test
harness. **The fuzzers here prove "does not misfire on common inputs", not
"is internally correct"** -- CLAUDE.md's third kind of "mutation stayed
green" (mathematically unobservable on this project's fixture
distribution), and this is the disclosure for it, matching the standard
Phase 30 set for `-M`/`-C`'s two-pass ordering and Phase 34 set for
`combine-diff.c`'s NUL-sentinel bound: recorded as a decision, not left for
the next person to rediscover as a mystery.

### No floating point

`xdl_bogosqrt` (git's size-limit heuristic, used both for
`xdl_cleanup_records`'s `mlim` and for the Myers core's `mxcost`) is **not**
an actual square root -- it is `for (i = 1; n > 0; n >>= 2) i <<= 1; return
i;`, a shift-based approximation, ported verbatim as `bogosqrt()` in
`diff_lcs.c`. The whole path (classification, trim, cleanup, Myers) is
integer-only, same as it was before this phase.

### Verification

- `make` + `make sanitize`: clean, no new warnings (checked both compiling
  `src/util/diff_lcs.c` in isolation and the full link).
- `make test`: 54/54 binaries (one new: `tests/test_diff_myers.c`, eleven
  checks covering divide-and-conquer/snake basic shapes, `xdl_trim_ends`'s
  four boundary cases, the three coordinate-mapping tests above (two
  Myers/aref, one cleanup_side/DISCARD), and the `sg_diff_group` contract
  -- ascending, non-overlapping, never both-zero -- on a multi-hunk
  fixture). All 28 pre-existing `tests/test_diff_out.c` checks stayed
  green unmodified, as expected: they anchor real git's output, not the
  old LCS's.
- `bash tests/interop.sh`: 1744/1744 passed, 0 skipped (unchanged from the
  Phase 34 baseline -- interop's full-output `cmp` checks were already
  passing before this phase on every fixture they happened to cover; this
  phase's whole point was the fixtures they *didn't* cover).
- `python3 tests/fuzz_diff.py 500 --seed <s> --max-failures 0` for
  `s in {0, 20000, 40000, 60000}` (the last one previously unused, run as
  an out-of-sample check): **0 mismatches** in all four, down from a
  measured baseline of 14/14/16 (no prior measurement at 60000).
- `python3 tests/fuzz_combined.py 200 --seed <s> --max-failures 0` for
  `s in {0, 20000, 60000}`: **0 mismatches** in all three (141/147/141
  conflicts produced respectively), down from a baseline of 2/0/(unmeasured)
  -- confirming the Phase 34 residual note above (a diff_lcs.c bug, not a
  combine-diff.c bug) was correct.
- The 4 fixtures saved from the baseline measurement
  (`p35_baseline/r48,r85,r114,r192`) were each re-diffed directly (`git
  diff` / `git diff --cached` vs `build/sg diff` / `build/sg diff
  --cached`) and confirmed byte-identical.

### Performance (a second-order benefit, not the goal)

The old LCS table was O(na*nb) time **and space**, with no size cap
(`lcs_table_ex` mallocs `na+1` separate rows of `nb+1` `size_t` each).
Myers is O((N+M)D) where D is the edit distance, using O(N+M) working
memory (the two `kvdf`/`kvdb` vectors). Measured on a synthetic 8000-line
file with 800 scattered single-line edits (10% churn, same content on both
builds, only `src/util/diff_lcs.c` swapped): the old build took 0.51s real
and peaked at 537 MB RSS; the new build finished in under 10ms and peaked
at 11.5 MB RSS. At 5000 lines / 500 edits the gap was already 0.19s/257 MB
vs <5ms/10.6 MB. This is expected from the complexity classes, not a
surprise, but it is a real user-facing improvement for `sg diff` on large
files, which the old implementation had no protection against at all.

## Phase 36: closing the read-side path-containment hole

Every previous path-containment phase (21-23, 28) guarded a *write*: `remove`,
`sg_write_file_mkdirs`, `sg add`'s argv. Measured directly against a crafted
`.git/index` (index v2, entry paths are validated by nobody -- see
`sg_index_read`'s header comment and the module-layout note in CLAUDE.md):
every write-side consumer was already closed (apply.c's `remove` guard,
merge.c's structural dependency on `sg_tree_flatten`, `reset --hard`'s
`--force` ordering). The hole was on the **read** side, and it was worse than
any write-side gap on record: `sg_tree_build_from_workdir` would `stat`/read
a path like `"../secret.txt"`, hash it, and **write the content as a
permanent loose object** -- reachable afterwards via `sg cat-file -p` --
before the separately-guarded delete/apply step downstream ever got a chance
to fail. `sg stash push` against such an index exited non-zero (looking, from
the outside, like the attack had been rejected) while having already
exfiltrated the file into the object store.

Real git's oracle for the exact same crafted index (real git's own
`update-index --add --cacheinfo ...,../secret.txt` refuses this path outright
with "Invalid path", measured -- the fixture can only be built with raw index
bytes): `git status --porcelain` lists the path (`A  ../secret.txt`), `git
stash push` fails (`invalid object 100644 <id> for '../secret.txt'`), and no
blob for the outside content is ever written. Three separate, independently
checkable facts -- not "git also refuses this index", which would have been
too coarse to say anything about *which* half (read vs. write) is guarded.

### The fix: two guards, not one, and neither hard-fails the whole call

**`sg_tree_build_from_workdir`** (`src/workdir/tree_build.c`) gained an
`sg_relpath_is_safe` check on every index path, run *before* `sg_path_join`
even runs (same position and reasoning as the pre-existing truncation check
right below it). Unlike every other guard in the project, the failure
direction here has to be a **hard failure of the whole build** -- this
function is the one index consumer that both reads outside the repository
and turns what it reads into a permanent write, so a caller (`sg stash push`,
`sg_snapshot_create`) must never be handed a tree that silently omits the
bad path (that would be the exact silent-data-loss shape the neighboring
truncation check already guards against) or, worse, a tree that silently
contains content read from outside the repository.

**`sg_diff_index_workdir`** (`src/workdir/diff.c`, three call sites:
`append_index_entry_vs_workdir`, `build_result_side`, and the "in index, not
in tree" branch of `sg_diff_tree_workdir`) needed the **opposite** failure
direction. `sg status` must still be able to list a hostile path -- real git
does too, from the staged half of the same status (`sg_diff_tree_index`,
which never touches the working directory at all and so needed no new
guard). Hard-failing the whole diff/status call here would have made the
fix itself the regression CLAUDE.md's Phase 25 rule was written to prevent:
"one unreadable path must not blind the user to every other path". Instead,
an unsafe path is folded into the **same case the file already has** for
"exists but unreadable" -- `SG_DIFF_SIDE_ABSENT`, never a `WORKDIR` side
carrying real content. This is a pre-existing convention (permission denied,
a race with a delete), not a new case invented for Phase 36; the guard just
adds one more reason to take a branch that was already there.

### `AD` vs real git's answer: the fourth deliberate divergence, not a bug

`sg status --porcelain` prints a fixed `AD ../secret.txt` for this exact
fixture, no matter what is actually sitting at the escaping path. Real git
does not print a fixed answer -- it genuinely reads the outside file to
decide, and gives three different answers depending on what it finds there
(all three measured against git 2.55.0, same crafted index, only the
outside file's on-disk state changed between runs):

| state of the file outside the repo | real git | sg |
|---|---|---|
| content is identical to the blob the index records | `A ` | `AD` |
| content has changed | `AM` | `AD` |
| file has been deleted | `AD` | `AD` |

The earlier draft of this note claimed the divergence was just "git read it,
found no difference, printed nothing extra" -- that explains only the first
row. All three rows share one cause: **byte-for-byte compatibility and
"refuse to read outside the repository" are in direct conflict here, and
this phase chose the second.** sg cannot know which of the three real states
holds without doing the very read Phase 36 exists to prevent, so it cannot
compute git's answer at all -- not "computes it wrong", genuinely does not
have the information.

Given that, printing a fixed code is the only option, and `AD` was chosen
over `A ` deliberately: `AD` is wrong in exactly the same way in all three
rows (it always claims a deletion happened), so it always makes the path
LOOK suspicious and draws the user's attention to it. `A ` would be a
strictly worse wrong answer in two of the three rows -- it claims the
tracked-vs-working-tree state is perfectly clean, which is true in exactly
one of the three rows and false (silently) in the other two. A guard whose
failure mode is "declare everything is fine" defeats the purpose of having
a guard at all. Both sides still name the escaping PATH in every row (the
only property interop pins, and now also the only property that survives
across all three of git's possible answers), sg is simply never able to
say more than that about it.

This is the project's **fourth** recorded deliberate divergence from git,
alongside `-C -C`/`--find-copies-harder` (Phase 33, rejected outright),
`-c`/`--cc` combined with an explicit rev (Phase 34, rejected outright), and
`* Unmerged path` staying unquoted regardless of `core.quotePath` (Phase 34).
Recorded in CLAUDE.md's divergence list alongside the other three.

### What stayed exactly as it was on purpose

`sg_merge_result_apply`'s `remove(abspath)` (`src/workdir/merge.c`) got a
comment, not a guard. Its path safety is a **structural fact**, not an
enforced invariant: every `sg_merge_result` in this codebase is built by
`sg_merge_trees` out of three trees that already went through
`sg_tree_flatten` (which aborts the merge outright, `-2`, on any entry
failing `sg_path_component_is_safe`), so nothing unsafe can reach this loop
today. Adding a guard here anyway would be the same mistake
`add_resolved_entry`'s neighboring comment already documents avoiding: a
redundant defense hides the layer that is actually doing the work, so a
mutation aimed at *this* line would never turn red -- the real guard, one
call away in `sg_tree_flatten`, would silently absorb it, and the next
person reading a green mutation report would misread "no signal" as "no
guard needed" instead of "wrong layer checked". The comment exists so that
the day `sg_merge_result` grows a second producer, its author has to
consciously decide who validates the paths, rather than inheriting safety
that happened to be true only because of who used to be the only caller.

`sg_index_read` gained no validation, again on purpose (this is the fourth
phase to make this exact call): every index consumer downstream needs to
answer "is this hostile" differently -- `sg status` lists it, `sg
stash`/`sg_tree_build_from_workdir` refuse it, the safety gates in apply.c
fold it into "dirty". Centralizing the check in the parser would force one
of those answers on all of them.

### Error messages that discarded `sg_tree_flatten`'s `bad_path`

Two messages predated `sg_tree_flatten`'s `-2`/`bad_path` contract (Phase 25)
and had never been updated to use it, both folding a *named* failure into a
generic guess:

- `cmd_status.c`'s staged-changes warning ("out of memory, or an unreadable
  HEAD tree") -- fixed by giving `sg_status_diff_staged` an optional
  `bad_path` out-parameter that forwards `sg_diff_tree_index`'s own `-2`/
  `bad_path` straight through (same shape `sg_diff_tree_index` and
  `sg_tree_flatten` already use), and printing the path when it's available.
- `sg_require_clean_workdir`'s and `sg_safe_apply_tree`'s "could not fully
  determine the working directory state" (`src/workdir/apply.c`, the message
  `sg merge`/`sg rebase`/`sg switch`/`reset --hard` all share via these two
  functions) -- same fix, threaded through the same new `bad_path` parameter
  on `sg_status_diff_staged`.

`sg_status_diff_unstaged` did **not** get an equivalent parameter: its only
source of `-2` would have been the new Phase 36 guards above, and those were
deliberately built to never produce one (see "opposite failure direction"
above) -- there is nothing for a `bad_path` parameter there to carry.

### Verification

- `make` + `make sanitize`: clean, no new warnings.
- `make test`: 54/54 binaries (three new checks added to the existing
  `tests/test_path_safe.c`, no new binary).
- `bash tests/interop.sh`: 1750/1750 passed, 0 skipped (baseline 1744 + 6
  new Phase 36 checks: status lists the path / stash fails / no blob
  written, on each of sg and real git).
- `python3 tests/fuzz_ignore.py`: 200 iterations, 0 mismatches
  (traversal-adjacent, run per CLAUDE.md's rule for touching `workdir/` path
  handling).
- `make clean && make sanitize`: 54/54 under ASan/UBSan, no new warnings, no
  sanitizer aborts.
- `python3 tests/fuzz_diff.py 500 --max-failures 0`: 0 mismatches (one
  isolated run during measurement reported 1, not reproduced across four
  further runs at the same seed range including a `--max-failures 3` run
  that would have printed the offending seed -- treated as environment
  noise from a concurrent build, not attributed to this phase).
  `python3 tests/fuzz_combined.py 200 --max-failures 0`: 0 mismatches (141
  rounds produced a conflict).
- Directed mutation (`tests/mutate.sh`), one per guard, each turning red on
  exactly its own named assertion and nothing else in the pre-existing 54
  checks (proving all three are net-new coverage, not duplicates of
  something already guarded elsewhere):
  - `sg_tree_build_from_workdir`'s check ->
    `test_tree_build_from_workdir_refuses_escaping_index_entry` (both its
    assertions: refusal, and "blob never written").
  - `append_index_entry_vs_workdir`'s check (used by
    `sg_diff_index_workdir`, what `sg status`/plain `sg diff` use) ->
    `test_diff_index_workdir_refuses_escaping_index_entry`.
  - `sg_diff_tree_workdir`'s own, separate check on the "in index, not in
    tree" branch (what `sg diff <rev>` uses; NOT reached by either guard
    above) -> `test_diff_tree_workdir_refuses_escaping_index_entry`. This
    one is the reason there are three tests, not two: the first attempt at
    this phase's mutation pass found this exact branch was a genuine blind
    spot (exit code 0, no FAIL) until the dedicated third test was added.

## Phase 37: pathspec on `sg status` and `sg stash push`

Two commands gain `-- <pathspec>...`, but they needed genuinely different
treatment, not a single shared "add a pathspec parameter" pass -- Part A
(status) is a filtering problem, Part B (stash push) is a partial-write
problem with no precedent anywhere else in the codebase.

### Part A: `sg status -- <pathspec>...`

**No rev/path disambiguation.** Measured against git 2.55.0:
`git status master` (a real branch name) prints nothing and exits 0 -- every
positional argument is a pathspec, full stop. `cmd_status.c` does not port
`cmd_diff.c`'s `split_revs_and_paths`; that logic is specific to diff having
revision arguments to disambiguate against, `status` has none.

**Five filter points, not three.** staged/unstaged/untracked look like the
obvious three, but two more exist and are easy to miss entirely:

1. **staged** (`sg_status_diff_staged`): must filter **between**
   `sg_diff_tree_index` and `sg_diff_detect_renames`, never after -- the
   same Phase 29 rule `sg_diff_list_filter`'s own callers already follow
   (filtering after rename detection can turn a real rename into a plain
   `A` because only half the pair survives). The function's signature grew
   a `const sg_pathspec *ps` parameter for this; `apply.c`'s two safety
   gates (which enumerate the unfiltered list to tell the user what is
   uncommitted) pass `NULL`.
2. **unstaged** (`sg_status_diff_unstaged`): no rename detection runs on
   this list, so post-hoc filtering is safe. A new `sg_status_list_filter`
   (`workdir/status.c`) is applied by the caller after the fact, rather
   than threading `ps` through the builder -- there is no ordering hazard
   to protect against here, unlike the staged case.
3. **untracked**: see the fold-table discussion below -- this one is the
   real design problem of Part A.
4. **ignored** (`--ignored`): built from two `sg_status_list_untracked`
   calls (a set difference), both inherit pathspec-awareness for free once
   the underlying function does.
5. **unmerged**: `cmd_status.c`'s `print_unmerged` (long format) and
   `print_porcelain_tracked` (porcelain) each scan `idx` **directly**,
   bypassing every `sg_status_list` the other four points funnel through.
   Each needed its own `sg_pathspec_matches` call. This is the single
   easiest site to miss: there is no list to filter, only a raw index scan,
   and it does not show up by grepping for "filter" the way the others do.
   `print_unmerged` specifically has **two separate loops** (a counting
   pass that decides whether the "Unmerged paths:" header even prints, and
   a printing pass) -- a reverse mutation on this phase found that removing
   either loop's filter alone leaves the *other* loop's filter fully
   masking it in an all-matched or all-excluded fixture; catching each
   loop's filter individually needed a spec that matches *some but not all*
   of the fixture's unmerged paths (see Verification below).

**The untracked fold table is the one deliberate exception to "filter after
the builder"** (CLAUDE.md now documents this too). Measured against git
2.55.0, a wholly-untracked `wholly/` directory containing `u1.txt` and
`deep/u2.txt`:

| pathspec | output |
|---|---|
| (none) | `?? wholly/` |
| `-- wholly` | `?? wholly/` |
| `-- wholly/` | `?? wholly/` |
| `-- wholly/u1.txt` | `?? wholly/u1.txt` |
| `-- wholly/deep` | `?? wholly/deep/` |
| `-- wholly/deep/u2.txt` | `?? wholly/deep/u2.txt` |
| `-- 'wholly/*'` | `?? wholly/` (wildcard still folds) |

Git decides fold depth **while walking**, not by filtering an already-folded
result: `sg_status_list_untracked` (`FOLD_DIRS` mode) only ever produces one
line, `"wholly/"`, for the whole subtree, and
`sg_pathspec_matches("wholly/u1.txt", "wholly/")` cannot succeed (the spec is
longer than the path, none of the three matching rules apply) -- filtering
post-hoc would make the file vanish entirely rather than un-fold it.

The fix threads `const sg_pathspec *ps` through `sg_status_list_untracked`
and its whole family of static helpers (`collect_untracked`,
`dir_scan_flags`, `collect_ignored_within`, `collect_untracked_folded`), and
adds one new decision, `spec_forces_recursion(ps, reldir)`: true only when a
**literal** (wildcard-free) spec names something strictly deeper than
`reldir` (a proper descendant, not merely a longer disjoint string). Only
literal specs can force recursion -- a wildcard spec has no directory-prefix
rule of its own (`sg_pathspec_matches`'s header comment already says this),
so whether it matches anything below `reldir` is answered by the ordinary
per-file `sg_pathspec_matches` check that already runs at every leaf,
regardless of fold depth. This is exactly why `-- 'wholly/*'` still folds:
its literal prefix (up to the first wildcard char) is `"wholly"`, which
equals `reldir` at the point the fold decision is made, so nothing forces a
deeper walk, and `dir_scan_flags` (also pathspec-filtered now) confirms at
least one real file matches before emitting the folded line.

When `spec_forces_recursion` is false, `dir_scan_flags` itself decides fold
vs. omit: a `ps`-filtered `has_nonignored`/`has_any` pair (a file that does
not match `ps` counts toward neither flag) is enough to reuse the exact
pre-Phase-37 fold/omit logic unchanged -- "fold if something matches, omit
if nothing does" falls out for free once the flags themselves are filtered.

### Part B: `sg stash push -- <pathspec>...`

**Not a mechanical filter -- a genuine partial write**, and the queue's
original description was wrong on two points (corrected after measurement):
`git stash show` does not accept a pathspec at all (`-- sub` is parsed as a
stash ref and errors "sub is not a valid reference"), so `sg stash show`'s
existing rejection was already correct and needed no change.

**The three trees are asymmetric, not uniformly filtered** (measured, git
2.55.0, fixture: `a.txt` staged+worktree changed but does NOT match `sub`;
`b.txt` worktree-only, does not match; `sub/c.txt` staged+worktree, matches;
`sub/d.txt` worktree-only, matches; `git stash push -u -- sub`):

| tree | content |
|---|---|
| stash's own tree | `a.txt`->`STAGED-a` (**index** content), `b.txt`->base (index==HEAD), `sub/c.txt`->`WORKTREE-c`, `sub/d.txt`->`WORKTREE-d` |
| `stash^2` (index parent) | complete, **unfiltered**: a=`STAGED-a`, b=base, c=`STAGED-c`, d=base |
| `stash^3` (untracked parent) | only the matched untracked file(s) |

Rule: `index_tree` is `sg_tree_build_from_index(idx)`, completely unchanged
-- it was never filtered, there is nothing to touch. The stash's own tree
starts from the **index** tree and, only on a matched path, is replaced
with freshly-rehashed working-tree content (or omitted, recording a
deletion, if that path is gone from disk). An unmatched path's on-disk
state -- edited, deleted, whatever -- is never even consulted.

**This needed a third dimension in `sg_tree_build_from_workdir`, not a
bigger `sg_workdir_missing` enum.** `sg_workdir_missing` already answers
"how to record a path whose file is gone" (`KEEP_INDEX_BLOB` vs.
`RECORD_DELETION`); a partial push additionally needs "does this path count
this round at all", a property of the pathspec, independent of that path's
own on-disk state. Folding the two into one enlarged enum cannot express
"RECORD_DELETION for a matched, deleted path, but ignore the working tree
entirely for every unmatched path" within the same call. The fix is a
**separate** `const sg_pathspec *ps` parameter: for a path `ps` does not
match, the function copies the index's own blob/mode straight through
(`lstat`/read never even run for that path), unconditionally, regardless of
which `missing` policy the call was given. This is also the mechanism that
makes an unmatched path's working-tree **deletion** invisible to the
stash's own tree -- there is nothing to detect a deletion of, because the
working tree for that path was never looked at.

**`sg_apply_tree_to_workdir` gained no pathspec parameter, on purpose** --
it is the one shared whole-tree entry point for switch/reset --hard/merge/
undo/stash, and CLAUDE.md's standing rule is that a filter added there would
put all five call sites on the hook for this one feature's risk. Instead
`safety/stash.c` has a private `restore_matched_paths` (per-path
reimplementation of the same "reset workdir+index to a target tree"
operation, confined to `ps`-matched paths), used only by `sg_stash_push`'s
own two restore calls: the HEAD reset every partial push does, and the
`--keep-index` re-layering of the index tree on top of it. An unmatched
path -- whether or not it appears in the target tree -- is left completely
alone in both the working tree and the index.

**"Did the pathspec match anything at all" is a brand-new question**: not
even `sg diff`/`sg status` ask it (both are silently exit-0 on a pathspec
matching nothing -- see Part A's own header comment on this divergence).
`sg_stash_push` answers it and refuses -- a new return code, **2** --
before any tree is built or any object written, even when the working tree
has OTHER, unrelated dirty paths the pathspec does not name. The check is a
plain existence scan (`idx_has_matching_path`, any stage) over the index,
plus the already-`ps`-filtered untracked-file listing when `-u`/`-a` is
set; it runs before the "nothing to save" (return 1) check and is otherwise
unrelated to it -- a worktree that is clean everywhere the pathspec touches
still returns 1, not 2, when the pathspec itself names a real (but
unchanged) path.

### Two open questions from review, both measured against real git and both "sg is already correct"

- **A pathspec matching only untracked files, without `-u`/`-a`.** Measured:
  `git stash push -- <spec-matching-only-an-untracked-file>` (no `-u`) also
  refuses -- exit 1, no stash created -- exactly like sg's B1 gate. Nothing
  to change; `idx_has_matching_path` correctly ignores untracked files
  entirely when `untracked_flag` is 0, since only the index is a candidate
  match source in that mode.
- **A bare `-` as the pathspec argument.** Measured: git treats it as an
  ordinary (non-matching, in a repo with no file literally named `-`)
  pathspec on both commands -- `git status -- -` is silent and exits 0,
  `git stash push -- -` refuses with "did not match any file(s)". sg
  matches on both counts (`sg_pathspec_looks_like_spec`'s wildcard/magic
  character set does not treat a bare `-` as special, so it is just a
  literal one-character spec that happens not to exist).

### A pre-existing gap this phase made easier to hit: the long format's closing summary line

Independent of pathspec, `cmd_status.c`'s long-format closing-summary logic
had a missing branch since it was first written (not introduced by Phase
37): the non-`-uno` branch's condition required staged/unstaged/untracked/
unmerged ALL to be zero before printing anything, so whenever exactly one of
untracked or unstaged/unmerged was non-zero (with nothing staged), **no
closing line printed at all** -- not the wrong line, no line whatsoever.
Measured against real git 2.55.0 (`LC_ALL=C`, three isolated fixtures, since
this machine's git is zh_TW-localized -- see CLAUDE.md's standing note on
that):

| staged | unstaged/unmerged | untracked | git's closing line |
|---|---|---|---|
| 0 | 0 | 0 | `nothing to commit, working tree clean` |
| 0 | 0 | >0 | `nothing added to commit but untracked files present (use "git add" to track)` |
| 0 | >0 | any | `no changes added to commit (use "git add" and/or "git commit -a")` |
| >0 | any | any | (no closing line -- the sections above already say enough) |

This is exactly the same three-way shape the `-uno` branch already
implements (it already had the second and third lines' logic, just gated on
a different first condition since `-uno` can never learn whether untracked
files exist). The fix mirrors that shape into the default branch, reusing
the identical message strings. Pathspec makes this trivial to reach by
accident: `sg status -- wholly` on a fixture whose only match is an
untracked directory lands exactly on the second row.

**Fixed** (not just recorded) -- the change is confined to the single
`else if (staged.count == 0) { ... }` block in `cmd_status.c`, mirroring
already-established logic one branch up, so it stayed within a
single-location, stop-loss-compatible fix. `tests/test_status_pathspec.c`
gained `test_summary_untracked_only` and `test_summary_unstaged_only`
(the second, sibling branch, not explicitly called out by the report but
covered by the same fix and the same oracle measurement); both reverse-
mutation-verified individually.

### A structural interop blind spot: the long format has no byte-for-byte oracle coverage at all

`tests/interop.sh`'s `cmp`-against-real-git technique (used everywhere else
in this phase, and in Phases 25-36 before it) is fundamentally unusable for
`sg status`'s **long format** hints and closing lines: every hint string is
literally `sg <subcommand> ...` (`"use \"sg restore --staged <file>...\" to
unstage"`, etc.), which can never byte-match `git`'s own `"use \"git
restore...\""` wording. The porcelain/short format and the six machine
formats of `sg diff` have no such problem (their bytes genuinely are
tool-name-independent, or -- for the few that do print a message -- interop
already treats them as sg-only assertions rather than `cmp`s). This is why
1776/1776 passing on this phase's interop run was never going to catch the
missing-branch bug above: **there was no check of that shape running at
all**, not a check that ran and happened to stay green. A "normalize `git`'s
tool name to `sg`'s before `cmp`-ing" scheme was considered and rejected:
half the hint text differs in more than just the tool name (real git has a
`commit -a` shorthand sg's own `commit` command does not implement at all,
`git add -A`-style flags similarly have no sg equivalent, while `restore
--staged` exists verbatim on both sides), so a normalizer would have to
encode a second, hand-maintained copy of every wording difference -- at that
point it is not testing anything a human reading both outputs side-by-side
wasn't already doing, just adding a layer that can silently drift out of
sync with the real divergences. The long format's closing lines and hints
therefore remain provable only by a direct `sg`-only assertion (as
`test_status_pathspec.c` and `test_status_untracked_mode.c` already do) or
by manual side-by-side comparison -- there is no mechanical guard against a
FUTURE missing branch of this same shape, and that is a standing, structural
gap in this project's test coverage, not a one-off bug.

### Verification

- `make` + `make sanitize`: clean, no new warnings, `56/56` binaries under
  ASan/UBSan.
- `make test`: `56/56` binaries (two new test files:
  `tests/test_status_pathspec.c`, `tests/test_stash_pathspec.c`).
- `bash tests/interop.sh`: `1776/1776 passed, 0 skipped` (baseline 1755 + 21
  new Phase 37 checks: Part A's fold-table `cmp`s plus the `master`-as-
  pathspec collision, and Part B's three-tree `ls-tree -r` comparison plus
  the exit-code/no-stash-created checks and the oracle confirmation). The
  new group covers the Part A fold table (full-output `cmp` per row) and
  Part B's three-tree `ls-tree -r` comparison plus the exit-code/
  no-stash-created checks, and the head-on colliding pair (`sg status --
  nosuch` exit 0 vs. `sg stash push -- nosuch` exit 1) guarding against
  unifying the two "did the pathspec match anything" rules. One authoring
  mistake surfaced here and nowhere else: the twin-fixture helper
  (`p37_b2_fixture`) initially ran `sg commit -q -m base` for BOTH
  implementations, but `sg commit` (unlike real git) has no `-q` flag at
  all (`usage: sg commit -m <message>`) -- the sg-side fixture silently
  failed to commit, and every downstream `sg stash push` in that fixture
  failed with "cannot create stash (unborn HEAD...)". None of the unit
  tests could have caught this, since they never shell out to the `sg`
  binary's own CLI argument parser at all.
- `python3 tests/fuzz_ignore.py`: 200 iterations, 0 mismatches (Part A
  touches the untracked directory walk directly).
- Directed mutation (`tests/mutate.sh`), one per filter/skip site, not
  batched:
  - Part A: staged-before-rename, unstaged, the untracked
    `spec_forces_recursion` gate, `dir_scan_flags`'s per-file filter, the
    folded-listing per-file filter, `-uall`'s own (separate) per-file
    filter, and all three `idx`-scanning unmerged sites (porcelain, and
    the long format's counting AND printing loops separately) were each
    individually caught. Three were genuine first-pass blind spots, each
    closed with a new, more targeted fixture rather than a broader one:
    `-uall` + pathspec had zero test coverage at all; the long format's
    counting-loop filter was masked by an all-excluded fixture where the
    header never fires either way; the printing-loop filter was masked by
    an all-matched fixture where the counting loop's own filter already
    produced the right visible answer.
  - Part B: the B1 "matched nothing" gate, the worktree-tree pathspec
    parameter, the untracked-listing pathspec parameter (both the listing
    used for the match check/sweep and the separate one threaded into
    `sg_tree_build_from_untracked`), both `restore_matched_paths` call
    sites (HEAD reset and the `--keep-index` reset), `restore_matched_paths`'s
    own two internal filters (the deletion loop and the write loop), the
    `sg_tree_build_from_workdir` skip-disk branch, and the `partial` flag
    computation itself -- all individually caught. Two were first-pass
    blind spots: the `--keep-index` restore call's pathspec-awareness was
    invisible with a single-file fixture (the whole-tree fallback and the
    per-path restore agree when there is nothing unmatched to disagree
    about) until an unmatched dirty file was added to that fixture; the
    deletion loop's own filter was invisible whenever every unmatched path
    already existed in the target tree (the deletion branch that check
    guards never even triggers) until a fixture added an unmatched,
    newly-staged file absent from HEAD specifically to exercise it.
  - Follow-up round (coordinator review): the two new closing-line branches
    in `cmd_status.c` (untracked-only, unstaged-only) were each caught
    individually by `test_summary_untracked_only`/`test_summary_unstaged_only`.
    The automatic safety snapshot's "must not be pathspec-narrowed" property
    (asserted in `test_b2_three_trees`, snapshot tree must hold a.txt's real
    `WORKTREE-a` content, not the stash tree's `STAGED-a`) has **no**
    mutation to hang a reverse-mutation round on -- `snapshot.c` passes
    `NULL` structurally, there is no pathspec variable at that call site to
    mutate into something else. The assertion exists purely so a FUTURE
    change threading a pathspec into that `NULL` would turn it red; this is
    the one Phase 37 property that is asserted but not mutation-verified,
    recorded here rather than left implicit.
  - Independent re-verification (coordinator, separate from the
    author's own rounds above): Part B's three trees compared **identical**
    byte-for-byte against real git across the full B2 fixture (stash's own
    tree, `stash^2`, `stash^3`, and the post-push working-directory state
    including that an unmatched path's staged status survives as `MM`);
    B1's no-match case agreed on both exit code and refs/stash absence; all
    five gates (`make test` 56/56, interop 1776/1776 0 skipped, `--leaks`
    56/56, `--sanitize` 56/56) were independently green; and a separate
    80-case `sg status` vs `git status` comparison found all 64
    machine-readable cases in agreement (the remaining 16 are the
    long-format hint/closing-line strings the structural gap above already
    explains).

## Phase 38: `sg status` long-format skeleton oracle, five bugs

Phase 37's closing note flagged 16 long-format disagreements as an open gap
without naming them individually. Phase 38 built a proper oracle to find out
exactly which lines those were, and fixed the five distinct bugs behind them.

### The skeleton comparison rule

Compares `sg status` against `LC_ALL=C git -c core.quotepath=false status`
after dropping exactly two line classes, everything else compared
byte-for-byte, no tool-name normalization:

1. Lines starting with `  (` (two spaces then an open paren) -- indented hint
   lines. The two tools word these differently on purpose (`sg add` vs
   `git add`, and git has `commit -a`-style shortcuts sg does not implement),
   so these can never agree byte-for-byte and are not supposed to.
   **The rule is `^  (`, not `^  (use "`** -- narrowing it to `(use "` was
   tried first and found to silently let a real divergence through: in a
   conflict state git also prints `  (fix conflicts and run "git commit")`,
   which does not start with `(use "` and would have slipped past a narrower
   filter uncaught.
2. Lines starting with a tab -- path lines. **Measured, not assumed**: the
   main conversation renamed `kind_label`'s `"modified:   "` to
   `"MODIFIED:   "` and reran `bash tests/interop.sh --interop`
   (1800/1803, 3 red), one of which was `phase23: sg status's untracked
   paths match real git's byte-for-byte`. The reason that check catches a
   *label* mutation despite its name naming only "paths" is worth stating
   precisely, because the name understates its own reach: its preprocessing
   is `sed -n 's/^\t//p'`, which strips only the leading tab -- **the label
   column is compared right alongside the path, on the same line** (Q3's own
   comment already says so: "git's labels are compared alongside the
   paths"). So this tab-line rule is guarded for the untracked case (Phase
   23), the untracked *section* (Phase 25), and staged-section ordering
   (Phase 32) -- but that is not the same claim as "every tab-led line is
   independently guarded". Phase 38 round 2 (item B3) closed part of the
   remaining gap: of the seven `unmerged_label` strings, only
   `both modified:` had a real-git oracle before round 2 -- and it comes from
   **Q6** (`P23_CONF`, `tests/interop.sh`'s `# --- Q6: the unmerged ("both
   modified") line`), a conflict fixture entirely separate from Q3's
   `P23_ST`. Q3 is where the strip-tab-sort-cmp *technique* and the "labels
   are compared alongside the paths" comment live, and Q3 is what proves the
   `modified:`/`new file:`/`deleted:` labels are covered; it builds no
   conflict at all. Attributing the unmerged label's oracle to Q3 (as an
   earlier draft of this section did) sends the next reader looking for a
   Q3 conflict fixture that does not exist. Round 2 added a second oracle
   (see below) covering
   `both added:`, `deleted by them:`, `deleted by us:`. The other three
   (`both deleted:`, `added by us:`, `added by them:`) still have **no**
   real-git oracle -- an ordinary merge cannot produce those stage
   combinations (DD auto-resolves, AU/UA needs a rename or a hand-built
   index) -- this is a pre-existing gap, not something Phase 38 introduced,
   and it is not attempted here.

Everything else -- the branch line, `No commits yet`, section headers,
`Untracked files not listed (...)`, blank lines, and the closing summary
line -- is compared byte-for-byte, **including the closing summary line**.
This does not conflict with Phase 37's rejection of "normalize the tool name
then cmp": that rejection was scoped to the *hint* lines (clause 1 above);
the closing summary lines are a different case, because both tools
hard-code the literal string `git add`/`git commit -a` in their wording
regardless of which binary is actually running (re-measured this phase --
`sg status`'s own closing lines already said `git add`, not `sg add`, before
this phase touched anything), so they are supposed to be byte-identical and
were not the source of any of the five bugs below.

The fixture matrix (18 repos, 27 case x flag combinations) and the two tools
implementing this rule (`mkfixtures.sh`, `cmp_skel.py`) were kept in the
milestone scratchpad during development and their logic was folded into
`tests/interop.sh`'s `phase38:` check group (one `check` per case, not
merged, so a red line names exactly which state broke) and
`tests/test_status_long_format.c` (named assertions for the two bugs with
real branching logic, C and E below).

### The five bugs (13 of 27 fixture cases were red before this phase)

All measured against real git 2.55.0, `LC_ALL=C`.

**Bug A -- merge block missing its trailing blank line.** `cmd_status.c`'s
merge-in-progress block (`You have unmerged paths.` /
`All conflicts fixed but you are still merging.`) printed no blank line
after itself; git always does, unconditionally -- even when nothing else
follows (the `mergebare` fixture: merge in progress, nothing else dirty).
The adjacent rebase block already had this shape (an unconditional
`printf("\n")` after its own lines); Bug A's fix is the same shape, one
`printf("\n")` added right after the merge block.

**Bug B -- unborn HEAD's `No commits yet` missing its trailing blank line.**
The literal was `"\nNo commits yet\n"`; git's is `"\nNo commits yet\n\n"` --
the leading blank line was already baked into the string, only the trailing
one was missing.

**Bug C -- unborn HEAD misplaced in the closing-line priority order.** Real
git's "nothing" family has five ordered rows, measured with six fixtures
pinning all five transitions:

| # | Condition | Literal string |
|---|---|---|
| 1 | `unstaged > 0 \|\| unmerged > 0` | `no changes added to commit (use "git add" and/or "git commit -a")` |
| 2 | `untracked > 0` (and `u_mode != no`) | `nothing added to commit but untracked files present (use "git add" to track)` |
| 3 | unborn HEAD (`has_head == 0`) | `nothing to commit (create/copy files and use "git add" to track)` |
| 4 | `u_mode == no` | `nothing to commit (use -u to show untracked files)` |
| 5 | otherwise | `nothing to commit, working tree clean` |

sg was missing row 3 entirely in both branches (the `u_mode == no` branch
and the default branch), so an unborn HEAD fell through to row 4 or row 5
depending on which branch it was in, instead of stopping at row 3. Fixed by
inserting an `else if (!has_head)` check between the existing row-2 and
row-4/row-5 checks in both branches -- `has_head` was already computed
earlier in the function for other purposes, no new state needed.

**Bug D -- `-uno`'s `Untracked files not listed (...)` had an extra blank
line.** The literal ended `...)\n\n`; git's ends `...)\n`. Dropped the extra
`\n`.

**Bug E -- a resolved in-progress merge suppresses the closing line
entirely.** Five fixtures pin this:

| merge? | unmerged | staged | unstaged | untracked | git's closing line |
|---|---|---|---|---|---|
| yes | >0 | 0 | 0 | 0 | prints row 1 (`conflict`) |
| yes | 0 | >0 | 0 | 0 | **none** (`resolved`) |
| yes | 0 | 0 | 0 | 0 | **none** (`mergebare`) |
| yes | 0 | 0 | 0 | >0 | **none** (`mergeuntr`) |
| yes | 0 | 0 | >0 | any | **none** (`mergeunst`) |

Once `cmd_status.c` has printed `All conflicts fixed but you are still
merging.` (merge in progress, zero unmerged entries left), git prints no
closing summary line at all -- not even in the `-uno` branch. When unmerged
entries remain (`You have unmerged paths.`), the closing line prints
normally (row 1), which sg already had right. Implemented as a single guard
-- `if (merge_in_progress && unmerged_count == 0) { /* no closing line */ }`
-- wrapping the entire existing if/else-if chain, rather than threading the
condition into every individual branch.

### Verification

`bash tests/mutate.sh` against `test_status_long_format.c`, reverting Bug C's
`else if (!has_head)` to `else if (0)` and Bug E's suppression guard to
`if (0)`: both mutations turned the corresponding named assertions red
(`test_priority3_unborn_beats_clean`, `test_priority3_unborn_beats_uno_hedge`
for C; `test_bug_e_resolved_merge_suppresses_summary`,
`test_bug_e_resolved_merge_suppresses_summary_uno` for E), confirming the
new tests have discriminating power rather than being vacuously green.
`tests/interop.sh`'s 27 `phase38:` checks and the full suite were green
after the fix (see the completion-criteria gates in `CLAUDE.md`).

Bugs A/B/D are covered by the interop skeleton comparison directly (they are
pure formatting, no branching logic to unit-test); C and E have both the
skeleton coverage and dedicated unit assertions, since they involve
conditional logic a byte-diff alone would not localize as precisely.

### Round 2: a cold-read fix, three fixture batches, an exit-code check

A cold read of round 1's diff (before merge) found one real bug and three
coverage gaps, all confirmed against real git 2.55.0.

**Bug (new, item A) -- the merge banner and Bug E's suppression guard used
two different scales.** The banner (`You have unmerged paths.` /
`All conflicts fixed but you are still merging.`) called
`sg_index_has_unmerged(&idx)` directly -- **not filtered by pathspec** --
while Bug E's suppression already used `print_unmerged`'s filtered count.
Measured with a fixture carrying one conflicted path (`f.txt`) and one clean
tracked path (`other.txt`):

| pathspec | git's banner | git's closing line | sg's banner (before fix) | verdict |
|---|---|---|---|---|
| `-- f.txt` (matches the conflict) | `You have unmerged paths.` | `no changes added to commit (...)` | same | correct |
| `-- other.txt` (does not match) | `All conflicts fixed but you are still merging.` | **none** | `You have unmerged paths.` | wrong |
| `-- nosuch` (does not match) | `All conflicts fixed but you are still merging.` | **none** | `You have unmerged paths.` | wrong |

Fixed by extracting the counting loop out of `print_unmerged` into its own
function, `count_unmerged(idx, ps)`, called exactly once right after the
index and pathspec are both available (`cmd_status.c`, before the merge
block prints anything), with the result stored in `unmerged_count` and
threaded through to both the banner and `print_unmerged` (which now takes
the count as a parameter instead of recomputing it). One value, two
consumers -- the same shape Bug A itself violated.

**Bug (new, found while building B1's fixtures) -- `-uno`'s
`Untracked files not listed (...)` hedge also needs to fire when a
just-resolved merge left literally nothing else to report.** The existing
condition was `staged.count > 0`; measured against the `mergebare` fixture
(merge resolved, `git add`-ed back to the exact content already in HEAD, so
`sg_status_diff_staged` reports zero staged changes even though `git add`
was run), real git still prints the hedge line -- because Bug E suppresses
the closing summary entirely in that state, and without this line nothing
at all would be printed after the banner, which real git never does.
Confirmed with `mergeuntr` and `mergeunst` too (both also have
`staged.count == 0` after their conflict resolves to HEAD's own content).
Fixed by widening the guard to
`staged.count > 0 || (merge_in_progress && unmerged_count == 0)`. This is
outside PHASE38_ROUND2.md's item A as literally written (which only names
the banner and the closing line), but it is the same underlying interaction
(a resolved-but-still-merging state changing what the closing region of the
long format has to say), found while making the B1 fixtures pass, and fixed
in-scope (`cmd_status.c`'s long-format printer only).

**B1 -- `-uno` on the four merge-resolved fixtures.** `conflict -uno` already
existed but has `unmerged_count > 0`, so it never reaches either of the two
bugs above; `resolved -uno`, `mergebare -uno`, `mergeuntr -uno`,
`mergeunst -uno` were added specifically because they do.

**B2 -- merge + pathspec, item A's regression guard.** The `conflict`
fixture gained a second, untouched tracked file (`other.txt`), and three new
cases: `conflict -- f.txt` (matches), `conflict -- other.txt` (does not),
`conflict -- nosuch` (does not). Adding `other.txt` changes the plain
`conflict` case's own output too (an extra unchanged tracked file shows up),
but the comparison is dynamic (built from both tools' live output at check
time, not a pinned string), so it tracked the fixture change automatically.

**B3 -- a real-git oracle for four more `unmerged_label` strings.** Recipe
(measured, produces byte-for-byte identical output on both sides):

```sh
for f in uu ud du; do echo base > $f.txt; done
git add .; git commit -qm base
git checkout -q -b other
  echo theirs > uu.txt; rm ud.txt; echo theirs > du.txt; echo theirs > aa.txt
  git add -A; git commit -qm theirs
git checkout -q master
  echo ours > uu.txt; echo ours > ud.txt; rm du.txt; echo ours > aa.txt
  git add -A; git commit -qm ours
git merge other > /dev/null 2>&1
```

produces `both added: aa.txt`, `both modified: uu.txt`,
`deleted by them: ud.txt`, `deleted by us: du.txt`. This check is **not**
run through the skeleton comparator (which drops every tab-led line, i.e.
exactly where label text lives) -- it reuses Q3's own technique instead:
strip the leading tab from both sides, sort, `cmp -s`. The remaining three
labels (`both deleted:`, `added by us:`, `added by them:`) are still
without a real-git oracle; see the round-1 skeleton section above for why
(an ordinary merge cannot produce those stage combinations) -- pre-existing,
not attempted this round either.

**Item C -- `p38_cmp` now also asserts sg's own exit code is 0.**
Previously both sides' stderr was discarded and neither exit code was
checked; `test -s` on the git-side skeleton already guarded the worst case
(both outputs empty), but sg printing a stray stderr warning or exiting
non-zero on some fixture was invisible. Real git exits 0 on all 34 fixture
combinations (measured); sg's exit code is now part of the same `check`.

### Round 2 verification

Fixture count: 27 (round 1) + 4 (B1) + 3 (B2) = 34 skeleton cases, all
matching, plus B3's separate label comparison. `bash tests/mutate.sh` against
`test_status_long_format.c`: reverting the banner's `unmerged_count > 0` back
to `sg_index_has_unmerged(&idx)` turned
`test_bug_a_pathspec_missing_conflict_shows_resolved_banner` red; reverting
the widened `-uno` hedge guard back to `staged.count > 0` alone turned
`test_uno_untracked_hedge_prints_on_bare_resolved_merge` red. Full gates
(`bash tests/gates.sh`) green after both fixes.


### Round 3: the oracle was borrowing the developer's environment

Rounds 1 and 2 were fully green locally, on all five gates. CI then went red
on **macOS only**, for 21 of the 34 skeleton cases; ubuntu/gcc, ubuntu/clang,
both fuzzers and the ASan job all stayed green, and no unit test failed. Both
runners and this machine run git **2.55.0**, so the obvious "the runner has an
older git" explanation was checked first and is wrong.

The failing set named the cause once it was listed in full: **every case whose
output carries a non-indented parenthetical failed, and every case without one
passed.** `clean` and `detached` passed because their closing line
(`nothing to commit, working tree clean`) has no parentheses at all;
`conflict -- f.txt` failed while `conflict -- other.txt` and
`conflict -- nosuch` passed, because the merge-suppression rule means only the
first of the three prints a closing line.

The mechanism, measured:

```
$ git -c advice.statusHints=true status      $ git -c advice.statusHints=false status
On branch master                             On branch master
Untracked files:                             Untracked files:
  (use "git add <file>..." to include ...)    	u.txt
	u.txt
                                             nothing added to commit but untracked
nothing added to commit but untracked          files present
  files present (use "git add" to track)
```

`advice.statusHints=false` does not only drop the indented hint lines -- which
this comparison filters anyway, so they were invisible to it. It also strips
the `(use "git add" to track)` tail off the **closing summary line**, which the
comparison very much does compare. GitHub's macOS runner has advice off; this
machine does not.

The fix is not to filter more. It is for the oracle to **declare its
environment** instead of inheriting it, exactly as it already declares
`LC_ALL=C` (this machine's git is zh_TW-localized) and `core.quotepath=false`
(sg emits `>=0x80` raw). `advice.statusHints=true` is simply the third axis,
and the three now live in one `P38_GIT_FLAGS` variable so a future edit cannot
pin one comparison and not another. sg has no advice configuration of its own,
so pinning git to `true` is what makes the two comparable on any machine.

A dropped pin would otherwise come back as 21 silent `cmp` failures naming no
cause, so the group also gained
`phase38 oracle: precondition -- the pinned flags keep git's closing-line
parenthetical`, which probes through the same `$P38_GIT_FLAGS`. Verified by
removing the pin and re-running under an injected
`advice.statusHints=false` (`GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=... `): 22
phase38 checks go red, and the precondition is one of them. With the pin
restored, interop is 1814/1814 **both** in the normal environment and under
the injected one -- the reproducer was verified to actually reproduce before
it was trusted, after a first attempt silently compared against an empty sg
side because `gates.sh` had cleaned `build/`.

**The general rule this is the third instance of**: a local green light is
evidence about this machine, not about the code, whenever an external tool is
the oracle. Every knob that changes the oracle's output has to be named on the
command line.

## Phase 39: refspec support for `sg push`

All measurements below are against real git 2.55.0, `LC_ALL=C`, done by hand
against a scratch fixture (a bare `remote.git` served locally). Before this
phase `sg push` only ever took a bare ref name (or none at all); there was no
refspec parser anywhere in the project. This phase adds `[+]<src>[:<dst>]`
support and `--delete <name>...`, entirely inside `src/cli/cmd_push.c` --
`include/sg/*.h` was deliberately left untouched (see section A below).

### 0. Syntax, `[+]<src>[:<dst>]`

1. **Split on the LAST `:`, not the first** (`strrchr`, not `strchr`).
   Measured: `a:b:c:d` reports src `a:b:c`, dst `d`; `aaa:bbb:ccc` reports
   src `aaa:bbb`, dst `ccc`.
2. No colon at all -> only `<src>`; dst is derived by the SAME literal
   tag/branch lookup this command always used (section 1 below explains why
   this is deliberately not the same code path as an explicit dst's src
   resolution).
3. `:<dst>` (empty src) -> delete `<dst>`.
4. `<src>:` (empty dst) -> error, git prints `fatal: invalid refspec
   '<the whole original argument>'`.
5. A leading `+` forces just THAT ONE refspec. `+:dst` (forced delete) is
   legal and is exactly a delete.
6. `--force`/`-f` forces every refspec in the invocation (measured: a single
   `--force` forced two independent refspecs at once).
7. `--delete <name>...` deletes each name in turn; ANY of them containing a
   `:` rejects the WHOLE command with `fatal: --delete only accepts plain
   target ref names`.

Implemented as `sg_push_refspec_parse` (non-static, but deliberately not
declared in any header -- the same "internal but linkable" convention
`sg_parse_push_report_status` in `src/net/transport.c` already used, so
`tests/test_refspec.c` can `extern` it without a network round trip). Pure
syntax, no I/O at all.

### 1. `<src>` resolution -- done BEFORE connecting, and it is NOT a peeling lookup

- The full rev-parse grammar (`HEAD`, `~N`/`^N`/`@{N}`, a full hex id) is only
  used for an EXPLICIT-dst refspec (`<src>:<dst>`). A no-colon refspec keeps
  the pre-Phase-39 literal name lookup (checks `refs/tags/<name>` then
  `refs/heads/<name>` for an exact match, nothing else) -- confirmed against
  real git: `git push origin HEAD~1` (no colon, not a real ref name) fails
  with the SAME "src refspec ... does not match any" message as a
  completely bogus name, because without an explicit dst git has no way to
  derive one from a rev expression either. Only when a dst is given
  explicitly does the richer grammar make sense (git can name the target
  ref unambiguously either way).
- **An annotated tag given as `<src>` is NOT peeled.** Measured: after
  `v2:refs/tags/v2copy`, the remote's `refs/tags/v2copy` is a **tag**
  object, not the commit it points at. This means `resolve_refspec_src`
  cannot be `sg_rev_parse_commit` (`include/sg/revparse.h`), which peels by
  definition -- it must instead try an EXACT ref lookup first
  (`refs/tags/<src>`, `refs/heads/<src>`, or `<src>` itself if already
  `refs/`-qualified), taking whatever raw id is stored there, and only fall
  back to `sg_rev_parse_commit` when none of those literal lookups match
  (this is also the path that makes `HEAD`, `~`/`^`/`@{N}`, and a bare hex
  id work as an explicit-dst src).
- `HEAD` works even while detached (measured: `HEAD:refs/heads/fromdetached`
  succeeds) -- the pre-Phase-39 "detached HEAD is rejected" gate in
  `sg_cmd_push` only guards the no-name-given path, and must not be
  generalized to the refspec path.
- `<src>` matching both a local tag and a local branch is still rejected
  (`src refspec '%s' matches more than one`), same wording as before Phase
  39, now applied per-refspec inside a loop instead of once for a single
  name.
- **A `<src>` that resolves to nothing aborts the ENTIRE push, before any
  network round trip** -- not a per-ref skip. Measured: `git push origin
  topic:newbr2 nosuch:x` against a real remote leaves `refs/heads/newbr2`
  **not created**, rc=1, and the output is exactly two lines with no `To
  <url>` line at all:
  ```
  error: src refspec nosuch does not match any
  error: failed to push some refs to '<url>'
  ```
  Also measured directly (by hand, unreachable URL, so a stray network
  attempt would show up as a curl error): git's own exit code here is
  already **1**, not 128 -- this case is NOT part of the 128-vs-1 exit-code
  divergence in section 5, only genuine refspec SYNTAX errors are.

### 2. `<dst>` completion -- done AFTER the advertisement is known

Three ordered rules, each measured directly against git's own error text:

1. `<dst>` starts with `refs/` -> used as-is.
2. **Rule 1**: does the remote already have a ref that some
   `refs/<dst>`/`refs/tags/<dst>`/`refs/heads/<dst>`/`refs/remotes/<dst>`
   guess matches? If so, use that guessed path. Measured and pinned: with
   the remote already holding `refs/heads/fromhead`, `v1:fromhead` (src is a
   TAG) updates `refs/heads/fromhead` -- it does **not** also create
   `refs/tags/fromhead`. Rule 1 wins even though src's own namespace would
   suggest rule 2.
3. **Rule 2**: if `<src>` is itself a ref under `refs/heads/` or
   `refs/tags/` (i.e. it came from an EXACT literal match in section 1, not
   the rev-parse fallback), prefix that ref's whole namespace onto the
   entire dst string. Measured: `master:notrefs/x` builds
   `refs/heads/notrefs/x` (the WHOLE dst string gets the prefix, not just
   its last segment). Rule 2 never applies to a `--delete`/`:dst` deletion
   (there is no src to take a namespace from).
4. Neither rule fires -> hard error, git prints `error: The destination you
   provided is not a full refname (i.e., starting with "refs/").` (plus
   more hint lines this project does not reproduce, output text is
   explicitly out of scope, see section 6.3).

Implemented as `complete_dst`, called once per candidate inside the same
loop that used to do only fast-forward checking. **Whether a dst-completion
hard error (rule 4) aborts the whole batch or is a per-ref skip: measured by
the main conversation in the review round.** `git push origin topic:okbranch
<sha>:cannotqualify` against a real bare remote exits 1, prints only "The
destination you provided is not a full refname ..." (no `To <url>` line and
no per-ref report), and the remote afterward has NOT gained
`refs/heads/okbranch` -- i.e. **whole-batch abort**, the same shape as a
syntax error, confirming this implementation's conservative reading of
git's `die()`-style wording was the correct one. No code change was needed.

### 3. `<dst>` format validation -- a brand-new free-input pipeline

Before this phase, every dst was program-constructed
(`"refs/heads/" + name`) and every user-typed name had already gone through
`sg_ref_name_valid_for_create` at ref-CREATION time somewhere upstream. An
explicit `<dst>` breaks both assumptions: it is free-form user text that
gets `snprintf`'d straight into a pkt-line line sent to the remote. Measured
tolerance (`git push origin master:<dst>`):

| dst | git |
|---|---|
| `refs/heads/ok1` | success |
| `notrefs/x` | success (rule 2 -> `refs/heads/notrefs/x`) |
| `refs/heads/../escape` | `fatal: invalid refspec '<original arg>'` |
| `refs/heads/a..b` | same |
| `refs/heads/a b` (space) | same |
| `refs/heads/a~1` / `refs/heads/a^` | same |
| `refs/heads/a\b` | same |
| `refs/heads/.hidden` | same |
| `refs/heads/end.lock` | same |
| `refs/heads/` (trailing slash) | same |
| `refs/heads/a//b` | same |
| dst containing an ESC byte | same |

`sg_ref_name_valid_for_create` (`include/sg/refs.h`) already implements
check-ref-format-level validation and was reused as-is here -- **do not add
a second copy**. It now agrees with every row above. **Review-round fix**:
the first cut of this phase left one gap, reported rather than silently
widened -- `refs/heads/.hidden` (a path component starting with `.`) was
rejected by real git but ACCEPTED by `sg_ref_name_valid_for_create`, because
the function only checked for a literal `".."` substring, never a
leading-`.` path COMPONENT. The main conversation asked for this to be
fixed in the shared validator (not approximated in `cmd_push.c`), reasoning
that shipping a known-wrong validator on a brand-new free-input pipeline was
worse than fixing it, and that the fix also corrects `sg branch`/`sg tag`
(which had the same bug: `sg branch .hidden` and `sg branch x/.hidden` both
used to exit 0 and create the ref, where real git's `git branch .hidden`
exits 128). `sg_ref_name_valid_for_create` gained exactly one added rule:
no `/`-separated path component may start with `.` (the first component is
covered by checking `name[0] == '.'` directly, every later component by
scanning for `/.`). No other rule was touched -- the table above already
showed the other 19 measured names agreeing with git before this fix, and
touching them would have manufactured a new divergence nobody asked for.
`tests/test_ref_name_dot.c` pins the full 21-row table as regression
coverage; `tests/interop.sh` pins `.hidden`/`x/.hidden` against real git for
`sg branch`, `sg tag`, and `sg push origin master:refs/heads/.hidden`
(the last one via the unreachable-URL technique, confirming the rejection
happens before connecting, same as the rest of this section).

**Validation timing**: whenever `<dst>` (or a bare `--delete` name) already
starts with `refs/`, it is validated immediately in
`sg_push_refspec_parse`/the `--delete` argument loop, BEFORE connecting --
confirmed against real git by hand with an unreachable URL: `git push
<unreachable> master:refs/heads/../escape` fails with the invalid-refspec
message and no connection-attempt text at all. Every measured row above is
already `refs/`-prefixed as given, so this covers 100% of the measured
table with a pure string check. Only the rule-1/rule-2 GUESSED path (dst
not already `refs/`-prefixed) defers validation until after
`complete_dst`'s guessing, since the final path isn't known any earlier;
`complete_dst` revalidates that guessed/rule-2-constructed path too, as
defense in depth against a corrupt or hostile advertisement.

### 4. Deletion

- Protocol-wise a deletion is just `new_id` = all-zero; `sg_push_ref_update`
  (`include/sg/transport.h`) already supports this, `src/net/transport.c`
  needed no changes at all.
- Deletion candidates are a THIRD kind, branched off at candidate-
  construction time (`push_entry.is_delete`), not threaded through the
  existing branch/tag `if`s with an `is_zero` check bolted on. Three
  call sites assume `new_id` names a readable object and must never see a
  deletion: `check_fast_forward` (would `sg_object_read` a zero id),
  `walk_add_object` (would abort the whole pack build), and
  `print_lost_commits` (would treat zero as a real ancestor root). All three
  are explicitly skipped for `is_delete` entries.
- Deleting a ref the remote doesn't have is a per-ref failure (not a batch
  abort): `sg: unable to delete '<name>': remote ref does not exist`, rc=1,
  `had_rejection=1`, the loop continues to the next candidate. Matches
  git's own `error: unable to delete '<name>': remote ref does not exist`
  in spirit (message text itself is explicitly out of scope, section 6.3).
- `--delete` accepts both a short name and a full `refs/...` path (measured:
  `--delete refs/tags/baz` works), and combines fine with `--force`
  (measured rc=0; force is simply irrelevant to a deletion's own logic,
  which never fast-forward-checks anything).

### 5. Per-ref failure vs. whole-batch abort -- and a pre-existing bug this phase's fixture exposed

| situation | when | already-successful refs land? | rc |
|---|---|---|---|
| `<src>` resolves to nothing | before connecting | **no, none of them** | 1 |
| refspec syntax error (empty dst, `--delete` with a colon) | before connecting | no | git 128 -> **sg 1** |
| dst completion hard error | after the advertisement | batch-abort (confirmed against real git, see section 2) | 1 |
| non-fast-forward rejection | after the advertisement | **yes** (measured: `topic -> newbr` lands, `master -> fromhead` in the SAME push is refused) | 1 |
| delete of a nonexistent remote ref | after the advertisement | per-ref | 1 |

**Exit code**: git uses 128 for a client-side refspec syntax error, but this
project's convention is "exit codes are only ever 0 or 1" (`CLAUDE.md`). sg
uses 1 for the same case, message text borrowed from git's own wording.
This is a **deliberate, named divergence**, pinned on both sides in
`tests/interop.sh` (git-side 128, sg-side 1, for: an empty dst, `--delete`
with a colon, and an already-`refs/`-prefixed malformed dst) -- see the
project-wide divergence list below.

**The non-fast-forward row above was a real, pre-existing design bug this
phase's own multi-refspec test fixture exposed, not a new requirement
invented for Phase 39.** Before this phase a push could only ever carry ONE
non-tag ref (a single branch, or none), so `check_fast_forward`'s
`SG_PUSH_NON_FF`/`SG_PUSH_UNKNOWN_REMOTE` handling doing `goto done` (a
whole-batch abort) was unobservable as a bug -- there was never a SECOND ref
in the same invocation for the abort to wrongly take down. Once refspecs let
one invocation carry two independent branches, the abort became directly
observable: pushing a fast-forwardable ref alongside a non-fast-forward one
in a single `sg push` command discarded the good one too, contradicting the
measured git behavior above and contradicting the tag path's and the
deletion path's own per-ref-skip convention (`had_rejection=1; continue;`),
which sat right next to it in the same function. Fixed by changing both
`goto done`s to the same `had_rejection=1; continue;` shape. A single-ref
push's own observable behavior (message text, exit code, no ref landing) is
unchanged by this fix -- verified via `bash tests/interop.sh` staying fully
green on every pre-existing phase5c push check.

**A second, purely mechanical bug found by the same fixture**: `complete_dst`
left `old_id_out` uninitialized (stack garbage) whenever the guessed/
completed dst did not match any advertised ref (the ordinary "this is a
brand new ref" case) -- `remote_exists_out` was correctly set to 0, but
`old_id_out` was never memset, and the garbage bytes were then sent to the
remote as the ref update's compare-and-swap `old_id`. Real git's
`git-receive-pack` correctly rejected this (`unable to resolve reference`)
every time, so the bug was 100% reproducible on the very first explicit-dst
push, not a rare race -- caught immediately when running the phase's own
interop fixture, not by any pre-existing test (there was nothing before
Phase 39 that could ever leave old_id uninitialized this way). Fixed by an
unconditional `memset(old_id_out, 0, SG_SHA1_RAW_LEN)` before the
scan-for-a-match loop.

### 6. Deliberately excluded, named rejection (not approximated)

Same treatment as `-C -C`/`--find-copies-harder` (Phase 33) and `-c`/`--cc`
plus an explicit `<rev>` (Phase 34): reject with a specific message, don't
guess.

1. **Wildcard refspecs** (`refs/heads/*:refs/heads/*`) -- rejected outright.
   Measured: real git actually implements this (it is a real feature, not
   invalid syntax) and DOES attempt to connect to the remote for it, so
   there is no matching exit-code divergence to pin here the way there is
   for the syntax-error cases in section 5 -- only sg's own rejection
   behavior is checked in `tests/interop.sh`.
2. **The bare push-"matching" `:`** (and `+:`) -- same treatment, same
   reasoning (real git also tries to connect for this one).
3. **Push output is not required to be byte-for-byte identical to git's.**
   The pre-existing sg push report style (`" * [new %s] ..."`,
   `"   %.7s..%.7s ..."`) is kept and extended with one new line style for
   deletions (`" - [deleted]  <name> -> <remote>/<name>"`); this milestone's
   acceptance criterion is "which refs changed to what, and the exit code",
   not the report text. **This is a known, accepted gap, recorded here
   rather than hidden.**

### A. Why no new header

Per this project's own convention (`tests/test_push_report.c`,
`tests/test_refadv.c`): a function that needs unit tests but has no other
caller outside its own `.c` file is made non-static and declared via
`extern` in the test file, rather than promoted to a public header just for
testability. `sg_push_refspec_parse`/`sg_push_refspec_free` follow this
exactly; `resolve_refspec_src` and `complete_dst` stay `static` (only
covered by `tests/interop.sh`, no direct unit test, since they need real
on-disk refs / an advertisement to exercise meaningfully).

### B. Data model

`push_entry` (previously: `name` + program-derived `ref_path`, nothing
else) gained: `is_delete`, `explicit_dst` (still awaiting `complete_dst`),
`refspec_force` (this candidate's own leading `+`, ORed with the global
`--force` at fast-forward-check time as `cand_force`), `dst_raw` (raw
`<dst>` text pending completion), `raw_arg`/`src_exact_ref_path` (both
owned, used only for messages and rule 2, freed alongside `name`/`ref_path`
in `push_entry_free_all`). No other struct in `include/sg/*.h` was touched.

**Round 3 cold-read fix**: an initial `src_display` field (meant to hold the
user's raw `<src>` text for the report line) was allocated and freed but
never actually read anywhere -- the report line has always printed
`e->name` for both sides (`"%s -> %s/%s"`, `e->name`, remote, `e->name`),
never a distinct src string. Per section 6.3 above ("push output is not
required to be byte-for-byte identical to git's" -- the pre-existing sg
report style is a known, accepted gap), changing the report line to print
git's own raw-src convention would be a bigger, unrequested behavior change
than this milestone's own stated scope. Decision: removed the dead field
rather than wiring it up, and recorded here instead of leaving a
write-only field with a comment claiming a use that doesn't exist.

The candidate/entries split from before Phase 39 is unchanged in shape
(`candidates` built before the advertisement, `entries` built after), just
now the candidates loop distinguishes three shapes instead of two: already-
fully-resolved (legacy `--tags`/no-colon path, `explicit_dst == 0`),
pending explicit-dst (`explicit_dst == 1, is_delete == 0`), and pending
delete (`explicit_dst == 1, is_delete == 1`) -- the last two share
`complete_dst`, differing only in whether rule 2 is allowed to fire.

The remote-tracking-ref-update loop at the very end of `sg_cmd_push` used to
`break` after the FIRST non-tag entry, because a push could only ever carry
one branch. With multiple branches now possible in one push, it updates
`refs/remotes/<remote>/<name>` for EVERY non-tag, non-delete entry instead
(a deletion's local remote-tracking ref is deliberately left alone by this
milestone -- real git does remove it, but that is a separate, un-scoped
piece of behavior, recorded here rather than silently added).

### C. Round 3: a cold read plus main-conversation measurement found five more bugs

1. **Memory leak on every `-1` return of `sg_push_refspec_parse` that
   happens after `out->src`/`out->dst` are already allocated**
   (`src/cli/cmd_push.c`, the two `return -1`s inside the function, plus its
   only caller). The header comment promised "*out is zeroed but not
   otherwise meaningfully filled on -1", the code didn't honor it. Fixed by
   calling `sg_push_refspec_free(out)` + `memset` before each of those two
   returns, and defensively also freeing at the call site. Manually
   confirmed with a 2,000,000-iteration loop harness calling
   `sg_push_refspec_parse("master:refs/heads/../escape", ...)` without
   freeing: pre-fix peak footprint ~98 MB, post-fix ~1.7 MB for the same
   iteration count -- `/usr/bin/leaks --atExit` on a SINGLE invocation
   reported 0 leaks either way (a known blind spot of that conservative
   scanner for a small malloc whose owning stack frame has already
   returned, see CLAUDE.md's leak-detection section), so the loop harness
   was needed to get a signal at all. This is exactly the shape that goes
   unnoticed on macOS (no `detect_leaks` support) but turns red on CI's
   ubuntu ASan job (`detect_leaks=1`), where LeakSanitizer's `atexit` hook
   overwrites the process exit code -- `tests/interop.sh` already has an
   unconditional (non-`HTTP_AVAILABLE`) check that hits this exact path
   (`sg push origin master:refs/heads/../escape`, asserting exit 1).
2. **A multi-segment `<dst>` was truncated to its last path segment**
   (`src/cli/cmd_push.c`, the candidate-completion loop): rule 2 can
   complete `master:notrefs/x` to `refs/heads/notrefs/x`, but `name` was
   taken via `strrchr(completed_path, '/')`, giving `"x"` instead of
   `"notrefs/x"` -- this fed a wrong `refs/remotes/<remote>/<name>` path
   and a factually wrong report line. Fixed by stripping the known
   `refs/heads/`/`refs/tags/` prefix (the whole remainder, not the last
   segment) and only falling back to the last-segment rule for a completed
   path under neither namespace. Pinned by a new HTTP interop check
   asserting both `refs/heads/notrefs/x` on the remote AND
   `refs/remotes/origin/notrefs/x` locally (not `.../x`).
3. **A single-refspec push, once entirely rejected, still ran the
   unconditional `refs/sg/chunks` propagation block**, writing to the
   remote and printing a spurious `To <url>` line -- contradicting
   pre-Phase-39 behavior where a rejected push never touched the remote
   beyond the read-only advertisement. The gate `entry_count == 0 &&
   !send_chunks_update` doesn't distinguish "everything already up to
   date" (also `entry_count == 0`, `had_rejection == 0`) from "everything
   was rejected" (`entry_count == 0`, `had_rejection == 1`). Fixed by
   adding `if (had_rejection && entry_count == 0) goto done;` right after
   the candidate loop, before the chunks block. Pinned by a new HTTP
   interop check (extending the phase 6a chunked-push fixture, since it
   already has real pending `refs/sg/chunks` state to send) asserting the
   remote's ENTIRE `for-each-ref` output is byte-identical before and
   after a rejected single-refspec push.
4. **An unqualified `<dst>` matching more than one advertised ref (rule 1)
   was silently resolved to whichever guessed prefix came first**
   (`refs/`, `refs/tags/`, `refs/heads/`, `refs/remotes/`, in that order)
   instead of being refused -- measured against real git 2.55.0: with the
   remote holding both `refs/heads/dup` and `refs/tags/dup`, `topic:dup`
   is rejected outright (`error: dst refspec dup matches more than one`),
   not resolved to either one. `complete_dst`'s rule-1 loop used to stop
   scanning at the first guess-prefix match; fixed to scan every prefix,
   count matches, and return a new `-4` ("ambiguous", message already
   printed: `sg: dst refspec '%s' matches more than one`) when more than
   one matches. Pinned by a new HTTP interop check (setting up the
   colliding branch/tag pair first via explicit `refs/...`-prefixed
   pushes, which bypass the ambiguity check entirely) plus a `phase39
   oracle:` check confirming real git also rejects it (run under
   `LC_ALL=C`, since this machine's git is zh_TW-localized and translates
   the message).
5. **`resolve_refspec_src`'s `-3` (allocation failure) was handled the
   same way as `-1` ("src matches nothing")** -- setting
   `src_resolve_failed` and `continue`-ing to parse the rest of the batch,
   instead of aborting immediately like every other OOM path in this
   function. Fixed to `fprintf` + `goto done` immediately on `-3`, matching
   the rest of `sg_cmd_push`'s OOM convention.

Item E above (`src_display`) was also found in this same cold read; its
fix and reasoning are recorded inline in section B, next to the field it
removed.

## Phase 40: `-c`/`--cc` with a `<rev>`, and the three ways Phase 34 guessed wrong

Phase 34 rejected `-c`/`--cc` together with a `<rev>` outright and recorded
the reason as "real git switches to a completely different parent pairing
there (stage 1 vs the named tree blob)". Rejecting rather than approximating
was the right call. **The stated reason, however, was wrong in three
separate ways**, and each error would have produced a plausible-looking but
incorrect implementation:

1. It is **not about conflicts at all**. A repository with no unmerged entry
   anywhere still prints `diff --combined` under `-c <rev>`.
2. Parent 1 is **the index**, not a merge base -- and not HEAD either.
3. Parent 1 is the **lowest stage present**, not stage 1; an add/add
   conflict has no stage 1 and pairs against stage 2.

On top of that the feature reorders the entire output and changes which
rows rename detection is even allowed to see -- two rules that no amount of
reasoning about "parent pairing" would have suggested.

All measurements below are against git 2.55.0 on 2026-08-29, driven from
Python with argv passed directly to `subprocess` (never through a shell, so
no argument is silently rewritten before git sees it), with `LC_ALL=C` and
`-c core.quotepath=false`. Six fixture rounds; the ones a rule depends on
are named per rule. **Every rule that needed a control group to be provable
has one, and the control is named** -- a measurement of the target scenario
alone cannot tell which line of output the new flag is responsible for.

### 0. When the rev form activates

| invocation | measured | fixture |
|---|---|---|
| `-c <rev>` / `--cc <rev>` | combined mode, sections 1-5 | B1, B2 |
| `-c --cached <rev>` | ordinary tree-vs-index; `* Unmerged path` for conflicts | C1, L |
| `-c <rev1> <rev2>` | ordinary tree-vs-tree, no combining | C2 |
| `-c` with no rev | unchanged Phase 34 behaviour | A2, A3, K |

So `--cached` and a second rev each beat `-c`. Note that **none of the three
"no effect" rows needs a special-case branch in sg**: `sg_diff_trees` and
`sg_diff_tree_index` never fill `ours`/`theirs`, so the combinable predicate
is false for their rows and the flag degenerates on its own. This is the
same "it falls out of the data layer" property Phase 34 already relied on to
make `--cached` keep printing `* Unmerged path` regardless of `-c`/`--cc`.

`-c`/`--cc` last-one-wins is unchanged (`-c --cc` -> `diff --cc`,
`--cc -c` -> `diff --combined`, both measured in fixture L).

### 1. The three sides

For a row on path `P` from the ordinary `<rev>`-vs-working-tree comparison:

- **parent 1 (left column)** = the **first index entry for `P`**, i.e. the
  lowest stage present.
- **parent 2 (right column)** = the named tree's blob for `P`.
- **result** = the working-tree file.

**Parent 1 is the index, not HEAD.** Rounds 1-2 could not tell the two apart
because the index and HEAD held the same blob in every fixture. Fixture E
was built specifically to separate them, giving `HEAD~1`, `HEAD`, the index
and the working tree four different blobs:

```
$ git diff -c HEAD~1
diff --combined a.txt
index 50beca8,5d33834..0000000        <- 50beca8 is the INDEX entry
@@@ -1,1 -1,1 +1,1 @@@
- V3-staged                           <- the staged blob, not HEAD's V2
 -V1-oldcommit
++V4-worktree
```

**Parent 1 is the lowest stage, not stage 1.** Measured stage sets:

| stages present for the path | parent 1 | fixture |
|---|---|---|
| `{0}` (ordinary path) | stage 0 | B, E, H, J, M, N, O |
| `{1,2,3}` (content conflict) | stage 1 | A, D |
| `{1,2}` (theirs deleted) | stage 1 | F |
| `{1,3}` (ours deleted) | stage 1 | F |
| **`{2,3}` (add/add -- no stage 1 exists)** | **stage 2** | C |

A rule phrased as "stage 1 when the path is unmerged" is right for three of
the five rows and silently picks nothing at all for add/add.

### 2. Which rows render combined

A row renders combined **iff all three sides are non-ABSENT**; otherwise it
renders **byte-for-byte as plain `git diff <rev>` would**.

That "byte-for-byte" is not a guess. Fixture G made the index blob differ
from the named tree's blob for a path deleted from the working tree, so the
two candidate pairings would print different hashes and different content:

```
$ git diff -c named            $ git diff named          (control)
deleted file mode 100644       deleted file mode 100644
index 998e834..0000000         index 998e834..0000000    <- the NAMED TREE's
-GONE-v2-namedtree             -GONE-v2-namedtree           blob, not the
                                                            index's a052637
```

**Control group for the whole section: fixture B is a repository with no
unmerged entry anywhere, and `git diff -c HEAD~1` still prints
`diff --combined`.** Conversely `git diff -c` with no rev on an ordinary
dirty path prints a plain `diff --git` (B-a), so nothing about Phase 34's
existing behaviour changes.

### 2b. `-c <rev>` also widens which rows exist at all

This rule was missing from the phase's own spec and was found during
implementation, by the implementer measuring a case the spec did not cover.
It is recorded here rather than quietly folded into section 2, because the
spec being incomplete is itself the lesson: sections 1-5 were all derived
from fixtures built to answer "how is a row rendered", and none of them
would ever have asked "which rows are there to render".

A row's inclusion test under `-c <rev>` is **"the result differs from ANY
parent"**, not "the result differs from the named tree". The ordinary
`<rev>`-vs-working-tree comparison only ever looks at the tree, so a path
whose working tree happens to match the named tree exactly, but whose index
entry does not (a staged edit reverted in the working tree), is absent from
the plain comparison and present in the combined one:

```
$ git diff named                    $ git diff -c named
(prints nothing at all)             diff --combined p3.txt
                                    index 74bb2c6,8a205e8..0000000
                                    @@@ -1,1 -1,1 +1,1 @@@
                                    - index only
                                    + shared
```

Both halves are the measurement: the empty left-hand side is what makes it a
widening rather than a coincidence. In sg this is why
`sg_diff_tree_workdir` takes a `combined` parameter at all -- a pass running
after the builder cannot recover a row the builder never emitted, so this
one rule genuinely cannot live in `sg_diff_fill_combined_from_index` with
the rest of Phase 40's data work.

### 3. Output order -- the rule that breaks every byte-for-byte check

**All combined rows print first, in path order; then all non-combined rows,
in path order.** In every format, patch included.

Fixture M interleaves five combinable paths with four non-combinable ones,
so path order and group order disagree on every line:

```
$ git diff --name-only -c named     $ git diff --name-only named  (control)
a.txt c.txt e.txt g.txt i.txt       a.txt b.txt c.txt d.txt e.txt
b.txt d.txt f.txt h.txt             f.txt g.txt h.txt i.txt
```

The control is what makes this provable rather than a coincidence of the
fixture: without `-c` the same nine paths come out interleaved.

### 4. Rename and copy detection sees only the non-combined rows

Fixture N: `src.txt` is modified (so it is combinable) and `copy.txt` is a
staged byte-identical copy of `src.txt`'s old content.

```
$ git diff --name-status -C named       (control)   $ git diff --name-status -c -C named
C100    src.txt copy.txt                            MM      src.txt
M       src.txt                                     A       copy.txt
```

So a combined row is removed from the queue **before** detection runs, and
can serve as neither a rename source, a copy source, nor a destination.
Without the `-C` control this is invisible: under plain `-M` a combined row
is a modification and would never have been paired anyway, so the rule only
becomes observable once copy detection makes modified paths eligible as
sources. This is the same shape as Phase 29's "filter before detect" lesson
-- ordering two passes wrongly has no symptom, it just answers differently
in one specific scenario.

The ordinary rename in fixture L still pairs normally, and needs no special
rule: its destination is absent from the named tree, so it was never
combinable in the first place.

### 5. Per-format behaviour

| format | combined row | non-combined row |
|---|---|---|
| patch, `-c` | `diff --combined P`, `index o,t..0000000`, hunks | ordinary |
| patch, `--cc` | `diff --cc P`; dense drops every hunk that differs from only one parent, which can leave **header-only output** | ordinary |
| `--stat` / `--numstat` / `--shortstat` | **contributes nothing at all** | ordinary |
| `--name-only` | the path, once | ordinary |
| `--name-status` | always `MM` | ordinary `A`/`D`/`M`/`R` |

Four details that each cost a fixture:

- **The `--stat` omission is per row, not whole-output.** Round 1 saw
  `--stat -c <rev>` print nothing and could not distinguish "combined rows
  contribute nothing" from "`--stat` gives up entirely", because every row
  in that fixture was combined. Fixture J mixes one combined row with two
  plain ones and prints a stat for exactly the two plain ones, with
  `2 files changed` in the summary line.
- **`--name-status` prints `MM` even when the result is byte-identical to
  one of the parents** (fixture O, rows p2 and p3) and for a mode-only
  change (fixture H). The two letters are not computed from content
  equality -- a combined row is `MM` by construction, because all three
  sides exist by definition of being combinable.
- **The dense header-only shape is new territory**: `diff --cc P`,
  `index o,t..0000000`, `--- a/P`, `+++ b/P`, and then nothing. It cannot
  arise from a conflict, which is why no test before this phase covered it.
  Fixture O builds it twice, once with the result equal to parent 1 (p2)
  and once equal to parent 2 (p3).
- Mode-only change prints the `mode a,b..c` line and no hunks (H); a binary
  row prints `Binary files differ` and **no `---`/`+++` lines at all** (L);
  parent 1 == parent 2 is fine and still renders combined (H, N, O p1).

### 6. Two blind spots the cold read found, and what made them invisible

Neither is a bug this phase introduced; both are properties with no
witness, found by reviewing the diff rather than by any gate. They are
recorded because the *reason* each was invisible generalizes.

**(a) The `ours == ABSENT` half of the combinable predicate.** A conflict
where only one of stage 2 / stage 3 exists -- a delete/modify conflict --
has nothing to combine, and real git prints `* Unmerged path` for it under
no flag, under `-c`, and under `--cc` alike (measured). Deleting that half
of the guard left `make test` and interop at **1915/1915 with zero FAIL
lines**. The cause is a fixture monoculture: every conflict in
`tests/interop.sh` is built by `p34_mkconflict`, which writes non-empty
base/ours/theirs strings and therefore *cannot* produce anything but the
full `{1,2,3}` stage set. A helper that can only build one shape makes
every test that uses it blind to the same dimension, however many of them
there are.

Phase 40 did not create this gap -- it dates from Phase 34 -- but it
promoted the guard from a file-local `combinable()` to a documented public
`sg_diff_entry_is_combined`, and **a contract written in a header reads as
a guarantee whether or not anything checks it**. That is the reason to fix
it here rather than leave it.

**(b) The widening rule reads the index group's LOWEST stage.** Rewriting
that block to read the group's *last* entry instead also left interop fully
green, because every fixture that reaches the widening branch has a
single-entry index group for that path -- where "first" and "last" are the
same row, so both spellings agree. Telling them apart needs a fixture where
they disagree about something observable, which here means a conflicted
path whose stage 1 and stage 3 give **opposite** answers to "does this row
exist at all":

```
stage 1      = BASE   differs from the working tree  -> the row must exist
stage 3      = ZZZ    equals it     -> reading stage 3 yields NO row
named tree   = ZZZ    equals it, so the ordinary test finds nothing alone
working tree = ZZZ
```

The `plain sg diff namedz prints nothing` control is what makes the result
mean anything: without it, a row appearing under `-c` proves only that
something produced it, not that the widening rule did.

Both gaps were confirmed the same way, and it is the only way that counts:
mutate the code, watch every gate stay green, add the fixture, mutate
again, watch the named checks go red.

## Phase 41: a differential net for the three-way merge, and what it caught

**Status: complete.** Sections 0-4 below were written BEFORE the work, so the
measurements would survive an interruption; they are left as they were,
including the plan in section 4, because sections 5-9 are partly a record of
that plan being overtaken. The headline: the fuzzer went from **59/200 to
0/200**, and the change that got it there was only half the one that was
planned.

Phase 35 replaced the alignment step for 2-way patch bodies with a port of
git's Myers algorithm and **deliberately left `src/workdir/merge.c` alone**,
because no fuzzer covers merge's alignment -- there was no net to change its
behaviour under. Phase 41's premise is that this is now the project's only
uncovered alignment path, and that its failure mode is the worst one: a
wrong alignment there writes conflict markers into the user's files, so the
error lands on disk as data rather than as cosmetically odd output.

### 0. Three divergences found while merely proving the oracle mechanism

None of these is about alignment. All three fell out of the first attempt to
compare sg's merge output with git's byte-for-byte, which is precisely the
comparison no test in this project has ever made.

**(a) The conflict marker's "ours" label.** sg writes the current branch
name; **real git always writes `HEAD`**. Measured across five situations, to
rule out the possibility that git merely happened to agree in one of them:

| situation | git's ours label | git's theirs label |
|---|---|---|
| merge on branch `master` | `HEAD` | `topic` |
| merge on branch `weird-branch-name` | `HEAD` | `topic` |
| merging a tag | `HEAD` | `v1` |
| merge from a detached HEAD | `HEAD` | `topic` |
| rebase | `HEAD` | `11f17ed (theirs commit subject)` |

git's ours label is invariant; only the theirs label varies. sg writes
`master` / `weird-branch-name` in the first two rows. Note sg is already
correct when detached, for an accidental reason: `cmd_merge.c:100` reads
`current_branch != NULL ? current_branch : "HEAD"`, and detached makes that
NULL.

**Correction, on checking the source rather than the output:** this
divergence IS deliberate and IS explained, in the comment at
`cmd_merge.c:93-99`. What that comment gets wrong is the next clause -- it
calls the divergence "a pre-existing divergence pinned by phase4b", and
**no such check exists**. All three mentions of `<<<<<<<` in
`tests/interop.sh` are something else: two assert the marker is *absent*
after an abort, and the third pins the *stash* labels (`Updated upstream`),
a different call site. `tests/test_merge_content.c` pins only that
`sg_merge_content` writes whatever label its caller hands it -- not which
label `cmd_merge.c` chooses. So the behaviour is intended, unpinned, and
absent from the "Four places where sg's answer differs from real git on
purpose" list in `CLAUDE.md`, which presents itself as exhaustive.

**A comment asserting a pin that does not exist is worse than no comment**,
because it tells the next reader the property is already guarded and stops
them adding the guard. It is the same failure this project recorded one
phase earlier, in Phase 40's section 6: a contract reads as a guarantee
whether or not anything checks it. The fix here is therefore not to change
the behaviour -- it is deliberate and must stay -- but to add the pin the
comment promised, correct the comment, and grow the `CLAUDE.md` list from
four entries to five.

**(b) `sg merge` accepts only a bare branch name.** Measured: a tag,
`refs/heads/topic`, `refs/tags/v2` and `topic~0` are each rejected with
`sg: invalid reference: <arg>`, while the equivalent `git merge` succeeds.
This contradicts this project's own stated convention that a user-supplied
revision always goes through `sg_rev_parse_commit`. **Deliberately NOT in
Phase 41's scope** -- it is a rev-parsing gap with nothing to do with
alignment, and folding it in would cost this milestone its focus. Recorded
here so it is not rediscovered from scratch.

**(c) The rebase theirs label's formatting.** git writes
`<short-sha> (<subject>)`, sg writes `<short-sha> <subject>` -- the
parentheses are missing (`cmd_rebase.c:138,172`).

The original plan recorded here was to "fix" (a) and (c) so the net could
measure alignment without the label line drowning out the signal. **(a) must
not be fixed** -- see the correction above. That removes the reason to touch
(c) as well, because the oracle no longer needs the labels to agree: `git
merge-file` takes explicit `-L` labels, so comparing at the content level
sidesteps both label questions entirely rather than legislating them away.

What (a) and (c) still need is a witness. (a) gets a pin for its current,
deliberate behaviour. (c) is different in kind -- it has no comment, no
test, and no sign anyone decided it -- so it reads as an oversight against
this project's stated goal of byte-compatibility, and is corrected to match
git rather than pinned as-is.

### 1. What the survey found, and why the swap is not a one-liner

- `merge.c` calls `sg_diff_lcs_table` twice (base x ours, base x theirs,
  `merge.c:364-365`) and backtracks each with its own `lcs_matches`
  (`merge.c:283-313`) into a `match[]` array: for each base line, the line
  it aligns to on the other side, or -1. A sync-point pass
  (`merge.c:374-448`) then keeps only base lines aligned on BOTH sides as
  anchors and classifies each span between anchors.
- `sg_diff_build_script` produces `sg_diff_group`s (a_off/a_len,
  b_off/b_len), **not** a `match[]`. **No function in the codebase converts
  between them.** That adapter is new code, and new code is new risk: it
  needs its own coverage rather than being treated as plumbing.
- **The `has_nl` semantics are opposite.** `merge.c` deliberately compares
  lines *ignoring* `has_nl` and repairs the final-line case afterwards
  (`merge.c:430-447`); `sg_diff_build_script` is has_nl-aware, so "same text,
  one side lacks the trailing newline" becomes two *different* lines and
  never reaches that repair path. That block likely needs redesign, not
  reuse.
- **Scope boundary that must not be misread:** the three-way layer itself
  (the sync-point classification) is sg's own design, **not a port of git's
  `xdl_merge`**. Making the two 2-way alignments agree with git therefore
  does *not* make the three-way result agree with git. "Swapped in Myers"
  must never be written down as "equivalent to git's three-way merge".

### 2. The order of work, and why it is not negotiable

Build the net and **measure the baseline before touching `merge.c`**. The
baseline is what decides whether the swap is even the right change: if the
mismatch rate is dominated by the sg-specific three-way layer rather than by
alignment, replacing the aligner will barely move it, and the honest
conclusion would be that Phase 41 should end at the net. Measuring after the
change can only produce a number with nothing to compare it against.

The oracle must not borrow its expectation from sg: `git merge-file` takes
three files and explicit `-L` labels, which makes it the closest real-git
unit to `sg_merge_content` and removes the label question from the
comparison entirely. Whether it is a faithful stand-in for the full
`git merge` path is itself something to verify, not assume.

### 3. The baseline, and what it says about the plan

`tests/fuzz_merge.py` (new, this phase). Each round builds one repository
with sg, copies it, and merges the SAME two commits with `sg merge` in one
copy and `git merge` in the other -- two independently built repositories
would carry different commit ids, and the rebase-style labels that embed a
short sha could then only be compared by shape. The expectation comes only
from git; nothing is derived from sg's own output.

Exactly one normalization is applied, to both sides: the `<<<<<<< ` label,
which is deliberate divergence #5 and pinned separately. To keep that honest
the harness checks the label *before* erasing it and reports a `label`
mismatch if it is not the expected one -- otherwise the step that hides the
label would also hide a regression in it.

**Baseline, 200 rounds, seed 0, measured before any change to `merge.c`:**

| | rc | label | body | total |
|---|---|---|---|---|
| default | 10 | 0 | 49 | **59 / 200 (29.5%)** |
| `--no-newline-edits` (control) | **0** | 0 | 11 | **11 / 200 (5.5%)** |

The control switches off the generator's one suspicious axis -- removing a
file's trailing newline. **A correlation measured only with the suspected
cause switched on is not an attribution**, which is why the flag exists.
With it off, every `rc` mismatch disappears and body mismatches fall from 49
to 11. (It is not an exact subtraction: the same seeds produce different
content once the mutation is off, so this is the rate with the cause absent,
not the same 200 cases minus some.)

So the mismatch rate is dominated by ONE root cause, and it is not
alignment: **`merge.c` compares lines ignoring `has_nl`**
(`sg_diff_lines_equal`), so "the user removed the trailing newline" reads as
"the user changed nothing". Minimal reproduction, with two controls:

```
base   = "x\ny\nz\n"
ours   = "x\ny\nz"      <- the only edit is removing the trailing newline
theirs = "x\ny\nZZZ\n"

git: conflict (rc=1)
sg : rc=0, result "x\ny\nZZZ\n"   <- ours' edit silently discarded
```

Control B (ours makes a *text* change instead) conflicts in both tools, so
the fixture is not simply one that never conflicts. Control C (only ours
removes the newline, theirs untouched) is clean in both and sg preserves the
removal -- so sg can represent the edit; the loss in A comes from the merge
comparison, not from anywhere else in the pipeline.

**This reads worse than a diff-alignment defect and should be recorded as
such**: sg does not merely lay the conflict out differently, it drops a
user's edit and reports success while doing it.

**What the baseline does to the plan.** The phase was chosen as "build the
net, then swap the aligner to Myers". The net says the swap targets the
**5.5% residual**, not the 29.5% headline -- the headline is one root cause
that no re-alignment is *guaranteed* to address. The swap is still the
coherent single change, because `sg_diff_build_script` is has_nl-aware and
so removes the dominant cause structurally rather than patching the
comparison, and because it is the only change with any chance of moving the
residual. But whether it does either is now a measurement, not a
prediction -- `merge.c:430-447`'s trailing-newline special case exists
*because* the comparison is has_nl-blind, and its behaviour after the swap
is unknown until measured.

### 4. Where the next step starts (anchors, so this is not re-surveyed)

Line numbers are anchors as of this writing and may drift -- go by name.

**The two calls to replace** (`src/workdir/merge.c`):

| what | where |
|---|---|
| `sg_diff_lcs_table` called twice, base x ours and base x theirs | `merge.c:364-365` |
| `lcs_matches`, merge.c's own backtrack, DP table -> `long *match` | `merge.c:283-313` |
| the sync-point pass that turns two `match[]` into merged output | `merge.c:374-448` |
| the trailing-newline special case (`is_final_line`, forcing `has_nl`) | `merge.c:430-447` |
| binary short-circuit, `content_has_nul` on all three sides | `merge.c:343-356` |
| conflict markers written | `merge.c:417-428` |

`match[i]` is "base line i aligns to line N on the other side, or -1".
`sg_diff_build_script` returns `sg_diff_group`s (`a_off/a_len`,
`b_off/b_len`) instead, and **no converter between the two exists** -- the
adapter is new code and needs its own coverage rather than being treated as
plumbing.

**The three call sites that inherit any behaviour change:**

| caller | where | labels it passes |
|---|---|---|
| `sg merge` | `cmd_merge.c:141` | branch name (divergence #5) / `branch_arg` |
| `sg rebase` | `cmd_rebase.c:177` | literal `"HEAD"` / `<short-sha> (<subject>)` |
| `sg stash apply`/`pop` | `stash.c:1117`, `:1255` | `Updated upstream` / `Stashed changes` |

`sg rebase` amplifies the risk: one rebase runs the three-way merge once per
replayed commit, so an alignment change lands N times rather than once.

**Existing tests that can legitimately go red on a re-alignment** (i.e. red
does not immediately mean broken -- attribute before "fixing"):

- `tests/test_merge_content.c`'s `test_both_changed_different_regions`
  (byte-exact `expected` string) and `test_anchor_newline_not_glued`
  (sensitive to where lines break).
- Everything else in that file uses substring checks, and
  `tests/test_merge_result_apply.c` plus interop's `phase4b`/`phase6d`
  groups are existence/exit-code level, so they are insensitive to a legal
  re-alignment. That insensitivity is the gap this phase exists to close;
  `tests/fuzz_merge.py` is the only thing that measures it.

**Acceptance is a measurement, not a green build.** Re-run both modes and
report the actual numbers against the baseline in section 3:

```
python3 tests/fuzz_merge.py 200                     # baseline 59/200
python3 tests/fuzz_merge.py 200 --no-newline-edits  # baseline 11/200
python3 tests/fuzz_merge.py --attribute <keep-dir>  # then re-attribute
```

Either number growing is a regression. The attribution matters as much as
the total: a change that halves the count while moving failures from
`has_nl` into `material-differs` has made things worse, not better.

**Do not write down "sg's three-way merge now matches git".** The sync-point
layer is sg's own design, not a port of `xdl_merge`; making both 2-way
alignments agree with git does not make the merge agree with git. How much
of the 5.5% residual belongs to that layer rather than to alignment is
unmeasured, and is answered by re-attributing after the swap.

### 5. What the Myers swap actually bought, measured one variable at a time

The swap came with a second change that looked like plumbing: `merge.c`'s
`segment_equal` had to stop ignoring `has_nl`, since the new alignment is
has_nl-aware and leaving the span comparison blind would keep the same spans
misclassified after the two sides had stopped being matched. **Two variables
changed at once, so neither can be credited from the total alone.** All four
combinations were built and measured, 200 rounds, seed 0:

| aligner | `segment_equal` | rc | body | total |
|---|---|---|---|---|
| LCS backtrack (before) | has_nl-blind | 10 | 49 | **59 / 200** |
| LCS backtrack | has_nl-aware | 4 | 55 | **59 / 200** |
| Myers | has_nl-blind | 10 | 32 | **42 / 200** |
| Myers | has_nl-aware | 0 | 36 | **36 / 200** |

Neither half alone helps: the exact comparison on its own moves the total by
nothing at all (it trades `rc` mismatches for `body` ones), and Myers on its
own leaves every `rc` mismatch in place. Only together do the `rc` mismatches
-- the class where sg reported a clean merge while dropping a user's edit --
reach zero.

**A third measurement says the algorithm swap is not what did it.** Holding
has_nl-awareness constant and changing ONLY the algorithm (an exact LCS
backtrack via the then-unused `sg_diff_lcs_table_exact`, versus Myers) gives
**the same 12 failures on the same 12 seeds**. On this fuzzer's fixtures,
Myers and the LCS backtrack are indistinguishable in output; what mattered
was has_nl, which the swap brought along rather than caused.

That does not make the swap pointless -- it makes its justification a
different one, and one that has to be measured rather than assumed. Merging a
6000-line file, same result both ways:

| | peak RSS | time |
|---|---|---|
| LCS table (2 x `(n+1)*(m+1)` `size_t`) | 602 MB | 0.37s |
| Myers | 10.8 MB | < 0.01s |

At 2000 lines it is 75 MB vs 9.9 MB, i.e. exactly the quadratic it looks
like. **That, plus having one aligner in the tree instead of two, is the
honest case for the swap; "it matches git better" is not.**

### 6. The three things the exact alignment then required

Making the alignment has_nl-aware immediately broke a case that had been
right before, which is how the remaining work was found. All three fixes are
git's own behaviour, each measured against `git merge-file` before being
written.

**(a) A line that lacks a newline still has to be terminated when output
follows it.** A `has_nl == 0` line is by construction the last line of ITS
file, but the merged output interleaves three files, so it can still be
followed by another side's lines or by a conflict marker. sg printed
`base14=======`, inventing a line present in none of the three inputs; git
prints `base14\n=======` (`xdl_recs_copy`'s `add_nl`). This is
`bytebuf_ensure_nl`, called before every append that follows content and
never after the last one, so a file that legitimately ends without a newline
still does.

**(b) Conflict refinement (git's `xdl_refine_conflict`).** With base `A\nB`
(no trailing newline), ours `A\nB\nC\n` and theirs `A\nB\nD\n`, base's `B` no
longer matches either side's `B\n`, so the whole tail is one conflict -- yet
real git still prints `B` as ordinary context above the marker, because
before printing it diffs the two conflicting sides against EACH OTHER and
hoists what they agree on out of the conflict. Without that step sg printed
`B` twice, once inside each side. This is exactly what
`tests/test_merge_content.c`'s pre-existing `test_anchor_newline_not_glued`
caught -- section 4 listed it as a test that could "legitimately go red on a
re-alignment", and it was right that it would go red and wrong that it would
be legitimate.

**(c) Conflict simplification (git's `xdl_simplify_non_conflicts`), which is
where the entire remaining residual lived.** Two conflicts separated by a
short run of identical lines are printed as ONE conflict that swallows the
gap. Every one of the 10-11 surviving mismatches was this same shape (`git
blocks=1, sg blocks=2`), including all 11 of the `--no-newline-edits`
control's -- which is why the control had sat at exactly 11, byte-identical,
through all three earlier interventions.

The threshold is git's constant, re-measured here rather than recalled:

| identical lines between the two conflicts | git prints |
|---|---|
| 0, 1, 2, 3 | one conflict block |
| 4, 5, 6 | two conflict blocks |

**The gap is not measured in lines alone.** A one-sided change inside the gap
blocks the merge however short it is: with a 3-line gap whose middle line is
an ours-only change, git leaves two conflicts, where a purely distance-based
rule gives one. (In git's terms the rule walks its `changes` list and refuses
when either neighbour has `mode != 0`.) Both halves are pinned as a pair in
`tests/test_merge_content.c`, and a distance-only implementation passes the
first and fails the second.

Expressing (b) and (c) at all meant `sg_merge_content` could no longer append
bytes as it classified: both passes need to see a region's neighbours, and
merging two conflicts across a gap has to reproduce that gap inside each side
of the combined conflict, in that side's own words. Hence the `merge_region`
list, `refine_conflicts`, `simplify_conflicts`, `emit_regions` -- and the
ordering refine-then-simplify, which is git's and is not symmetric: refinement
splits conflicts apart, and simplification then decides which of the pieces
are too close together to be worth separating.

### 7. Final measurement

| | rc | label | body | total |
|---|---|---|---|---|
| before (baseline, section 3) | 10 | 0 | 49 | **59 / 200** |
| after | 0 | 0 | 0 | **0 / 200** |
| after, `--no-newline-edits` control | 0 | 0 | 0 | **0 / 200** (was 11) |

Re-run on seed ranges never used during development -- see section 10 for
the final acceptance runs, which were redone after the generator itself was
found to have a blind spot, so the numbers here are the ones that stand.

Gates: `make test` 60/60, `interop.sh` 1946/1946 (9 new checks), `make
sanitize` 60/60 with 0 sanitizer errors.

### 8. Mutation results, including the one that is green on purpose

Every new rule was mutated individually (`bash tests/mutate.sh <name>
src/workdir/merge.c <expr> test_merge_content`), and each was caught by the
named check written for it, not merely by "something went red":

| mutation | caught by |
|---|---|
| `segment_equal` back to has_nl-blind | trailing-newline removal (rc 1 -> 0, the dropped edit returns) |
| `bytebuf_ensure_nl` neutered | conflict side without a trailing newline |
| refinement skipped | refinement hoists the agreed line, + `test_anchor_newline_not_glued` |
| gap threshold 3 -> 4 | gap of 4 stays split (and ONLY that one) |
| gap threshold 3 -> 2 | gap of 3 merges (and ONLY that one) |
| a resolved region no longer blocks | a resolved change blocks the merge |
| adapter drops a group's `a_len` | three pre-existing merge tests |
| threshold 3 -> 4, run against `--interop` | 2 of the 9 new interop checks, both gap4 |

The threshold pair is the point of that fourth and fifth row: one mutation in
each direction, each caught by exactly one test, is what makes 3 a measured
threshold rather than a number someone typed.

**One mutation is green, and it is not a coverage gap.** Removing the
`bi < g->b_off` half of `script_matches`' lockstep walk changes nothing
observable, because `sg_diff_build_script`'s documented contract ("between two
consecutive groups, a[..]==b[..] line for line") makes `bi` and `g->b_off`
equal at that point by construction. It is the third of the three categories
in the mutation-testing notes (mathematically unobservable, not a blind spot
and not a redundant guard): the clause exists so that a future violation of
that contract degrades into "no sync point here" rather than into a `match[]`
entry past `nb` that the sync-point pass would then use to subscript
`ours_lines`. The failure direction is the whole value, and no test can
observe a failure direction that cannot currently occur.

### 9. What is now true, and what still is not

`sg_diff_lcs_table` / `_exact` / `_free_table` are **gone**: merge was their
last caller, and Phase 35's header comment saying so was about to become
false. `sg_diff_lines_equal` (has_nl-blind) survives only because
`src/cli/diff_out.c`'s combined diff still uses it.

**Still do not write down "sg's three-way merge is a port of `xdl_merge`".**
It is not. The region list reproduces two of git's post-passes and the
sync-point classification underneath them is still sg's own design; it now
AGREES with git on 800 fuzz rounds, which is a measurement, not an
equivalence proof. The known gap in that measurement is the fixture generator
itself: `tests/fuzz_merge.py` builds line-oriented text files with a fixed
vocabulary of mutations, so a shape it cannot generate is a shape nothing
here has tested (`fixture generators create shared blind spots` -- the same
caveat Phase 30's rename fuzzer carries).

Three things this phase deliberately did NOT do, so they are not
re-discovered as bugs: `sg merge` still accepts only a bare branch name
(section 0(b) -- a rev-parsing gap, unrelated to alignment); the ours label is
still the branch name rather than `HEAD` (deliberate divergence #5, now
pinned); and git's `XDL_MERGE_ZEALOUS_ALNUM` variant of the simplification
rule (where the gap must also contain an alphanumeric character) is not
implemented, because plain `git merge` does not use it.

### 10. The review round: a bug the whole net was blind to

The region rewrite was handed to a cold reviewer with the five gates already
green and the fuzzer at 0/200. It found a real defect, and independently of
that the same defect surfaced while probing the region list by hand -- which
is the only reason it is written up here as caught rather than as shipped.

**The defect.** `emit_regions` called `bytebuf_ensure_nl` at the top of every
region, before knowing whether that region would emit anything. The
sync-point pass pushes a zero-length region after the final anchor as a
matter of course, so a file whose last line legitimately has no newline got
one appended. Measured against `git merge-file`:

| inputs | git | sg (first cut) |
|---|---|---|
| base = ours = theirs = `a\nb` | `a\nb`, rc 0 | `a\nb\n`, rc 0 |
| base `a\nb\nc`, ours `a\nX\nc`, theirs `a\nY\nc` (no trailing newline anywhere) | ends `>>>>>>> theirs\nc` | ends `>>>>>>> theirs\nc\n` |

The first row is the alarming one: a merge that resolved to "nothing
changed" rewrote the file anyway. The fix is one guard -- an empty region
must not even terminate the previous line -- and the reverse mutation (back
to an unconditional `ensure_nl`) turns exactly the two new tests red.

**Why nothing caught it, and what that cost.** Five green gates, 800 fuzz
rounds and eight mutations all missed it, for one reason:
`tests/fuzz_merge.py`'s `gen_base` built every base line as `"base%02d\n"`.
An anchor is always a BASE line, and `sg_diff_split_lines` only clears
`has_nl` on a file's own last line, so **an anchor with `has_nl == 0` was
unreachable by construction** -- the generator only ever stripped the newline
from `ours`/`theirs`. The fuzzer was not weakly covering this shape, it could
not build it at all, and 0/200 was therefore zero evidence about it. This is
the `fixture generators create shared blind spots` pattern, and here it hid
the most severe class of defect in the whole phase (silent content change on
a clean merge) behind the most reassuring possible number.

`gen_base` now drops the trailing newline 15% of the time, the same axis
`mutate` already had. Proof that this closed the hole rather than merely
widening the search: with the fix reverted, the OLD generator reports
**0/200** and the new one reports **20/200**. Re-verified after the fix at
200 rounds x 4 unused seed ranges (1000, 5000, 9000, 31337), **0 mismatches**
in each, 129-144 conflicts per 200.

**Two other review findings, recorded rather than acted on.** The tail check
in `refine_conflicts` tests only `ai < r->oe` and not `bi < r->te`; the two
reach their bounds together by the same `sg_diff_build_script` contract
`script_matches` leans on, so the untested half cannot differ -- but if that
contract were ever violated the failure would be silent loss of theirs-side
content rather than an error, which is worth knowing before touching the
aligner. And the empty-side guard in `refine_conflicts` is probably a pure
optimisation (calling `sg_diff_build_script` with an empty side would produce
the same single group), kept because it is also where git's own "no sense
refining a conflict when one side is empty" rule is documented.

### 11. The second review round, on the fix itself

The fix in section 10 was written after the reviewer had already read the
diff, so by this project's own rule it had not been cold-read by anyone. It
went back out on its own, with the tail diff only.

It came back with a coverage finding rather than a defect, and the finding is
worth more than it first looks. The guard asks whether the side being PRINTED
is empty (`from == to`, where `from`/`to` follow `take_theirs`), not whether
the ours side is -- and for a **one-sided pure insertion** those two questions
have different answers: theirs inserts a run where ours has nothing, so the
region is `REGION_RESOLVED` with an empty ours range and a non-empty theirs
range. Asking the wrong side there does not misplace a newline, it **drops
the inserted run entirely**.

Measured, both directions:

| mutation | `test_merge_content` before | after |
|---|---|---|
| `from == to` -> `r->os == r->oe` | green (13 named tests) | red: "theirs inserts where ours has nothing" |
| `from == to` -> `r->ts == r->te` | green | red: "ours inserts where theirs has nothing" |

Every `RESOLVED` fixture in the file was a same-length replace, so nothing
exercised the shape. `tests/fuzz_merge.py` did catch it -- 54/200 under the
first mutation -- so this was probabilistic coverage with no named test
behind it, which is the state this project's mutation table exists to
prevent. Two fixtures, one per direction, close it.

The other two findings were cross-confirmations rather than new: the
asymmetric tail check in `refine_conflicts` (already recorded in section 10,
same conclusion reached independently) and that the `sg_diff_lcs_table`
removal leaves no residue -- verified by search, and separately by the clean
rebuild the gates do anyway, which is the part a search cannot stand in for.

### 12. Witnessing the indentation-heuristic argument, and what that uncovered

`script_matches` asks `sg_diff_build_script` NOT to apply git's indentation
heuristic, because git's own `ll_merge` does not set `XDF_INDENT_HEURISTIC`
(only `git diff` turns it on by default). That argument shipped with **no
witness at all**: passing `1` instead of `0` measured **0/200**, identical to
passing `0`. A parameter whose two values cannot be told apart is not a
verified choice, it is an unasked question.

Two generator dimensions had to be added before the values separated, and the
order matters because the first one alone was not enough:

1. **Indentation.** Every line the generator produced started at column 0,
   and the heuristic scores candidate positions BY indentation. Adding
   blocks, indented bodies and blank lines: still **0/200 either way**.
2. **Slidable groups.** The heuristic only chooses when there is something to
   choose between, i.e. a pure insert/delete bordered by identical lines,
   which can sit at more than one position without changing what the diff
   means. Every inserted line carried a unique tag, so no group was ever
   slidable. Adding a `dupblock` op -- duplicate a run of EXISTING lines in
   place -- finally separated them:

| `indent_heuristic` | mismatches |
|---|---|
| 0 (shipped) | 1 / 200 |
| 1 | 9 / 200 |

`tests/test_merge_content.c`'s `test_merge_does_not_use_the_indent_heuristic`
pins one of those fixtures byte-for-byte against `git merge-file`, so the
argument now has a named witness and not merely a net -- the distinction this
phase learned the hard way in section 11.

**And the widened generator found a real residual: 5 / 1500 rounds (0.33%)
in the default mode, 4 / 1500 in the control.** The first attribution written
here was wrong, and the way it was wrong is worth more than the number.

The harness first asked "are the two 2-way diffs byte-identical to git's?",
answered yes for every case, and concluded the divergence had to be in sg's
own sync-point layer. That reasoning has a hole: it compares sg and git
through their **diff** paths, which proves the aligners agree there, and then
assumes the merge paths use the same aligner. Measured, they do not.

**`git merge` defaults to the HISTOGRAM diff algorithm; `git merge-file`
defaults to Myers.** On the one `rc` case, `git merge-file` reproduces sg's
output byte for byte -- including the "lost" line that looked alarming -- and
`git merge-file --diff-algorithm=histogram` reproduces `git merge`'s. Over all
nine saved cases:

| | count |
|---|---|
| sg == `git merge-file` (Myers), byte for byte | **9 / 9** |
| `git merge` == `git merge-file --diff-algorithm=histogram` | **9 / 9** |
| Myers and histogram agreeing with each other | 0 / 9 |

So the entire residual is **which diff algorithm git's merge uses**, and
nothing else. That is a considerably stronger result about sg than the wrong
attribution suggested: on every case where sg disagrees with `git merge`, it
reproduces git's OWN three-way merge of the same three buffers exactly, so
sg's sync-point layer -- the part that is not a port of `xdl_merge` -- agrees
with `xdl_merge` on all of them. Section 9's caution stands as a caution; the
measurement did not find it.

It also corrects section 2, which chose `git merge-file` as the oracle "the
closest real-git unit to `sg_merge_content`" and noted that whether it is a
faithful stand-in for the full `git merge` path was "itself something to
verify, not assume". It is not faithful, and this is where that assumption
would have been paid for: had the fuzzer used `merge-file`, all nine of these
would have been green and the algorithm difference would never have surfaced.

`--attribute` now runs both oracles and labels each case:

- **`[algo]`** -- sg reproduces `git merge-file` (Myers) exactly and `git
  merge` matches histogram. Not sg's defect.
- **`[3way]`** -- the 2-way diffs agree with git yet sg's merge differs from
  git's own Myers merge: sg's sync-point layer.
- **`[align]`** -- a 2-way diff itself differs from git's. An alignment
  regression, never expected.

Measured today: **9 `[algo]`, 0 `[3way]`, 0 `[align]`.** The `[align]` probe
was checked to have discriminating power rather than assumed to -- desyncing
`sg diff`'s own aligner from git's flips saved cases to `[align]`.

Closing the remaining gap means porting git's histogram algorithm and using it
in the merge path (only there -- `sg diff` matches git with Myers, thousands
of fuzz rounds deep). That is a phase of its own. Until then the acceptance
criterion for `tests/fuzz_merge.py` is **not "0"**: it is "0 `[align]`, 0
`[3way]`, and every `[algo]` case is the known histogram gap". Counting them
together is exactly how a real regression would hide inside a known gap.


## Phase 42: porting histogram, because that is what `git merge` aligns with

Phase 41 ended with a measured 0.33% residual and a named cause: `git merge`
defaults to the **histogram** algorithm while sg's merge ran Myers. This phase
ports histogram and points the merge at it. The headline: **`tests/fuzz_merge.py`
goes to 0/200 in every mode and seed range measured**, and the `[algo]` bucket
that carried the whole Phase 41 residual is empty.

### 1. The two defaults are not the same default, and that is the whole point

Measured, git 2.55.0, on this machine with no diff/merge configuration set at
all (`git config --get diff.algorithm` exits 1):

| | default algorithm |
|---|---|
| `git diff` | Myers |
| `git merge-file` | Myers |
| `git merge` | **histogram** |

`git merge` also honours `diff.algorithm` when it IS set -- `-c
diff.algorithm=myers` changes its answer, which is how the default was
established rather than assumed. sg has no such configuration, so it hard-codes
histogram for merge and Myers for `sg diff`, mirroring git's unconfigured
behaviour.

`sg diff --histogram` exists as the opt-in, and it is not decoration: it is the
only DIRECT oracle the port has. Everything else observes histogram through the
merge, where sg's own three-way layer sits on top and can mask an alignment
difference. `tests/fuzz_diff.py --histogram` points the same fixtures at `git
diff --histogram` on both sides.

### 2. What the port had to get right, and how each was established

The algorithm was written from its definition, then checked against git's own
`xdiff/xhistogram.c`, which was fetched for the purpose (it is not on this
machine). Two structural points were found by MEASUREMENT first and confirmed
in the source afterwards, which is the order that matters here because both
were initially implemented the other way round:

- **No trimming and no record cleanup for histogram.** sg's Myers path starts
  with `trim_ends` (strip the common prefix/suffix) and `cleanup_side` (discard
  lines with no counterpart). Running histogram after `trim_ends` scored
  **9/500** on the histogram fuzzer; skipping it scored **5/500**, and the
  minimal case explains why: with base `[P, R, P]` and ours `[P, P, R]`, git
  matches the two-line run `P, R`, which the prefix trim makes unreachable by
  pairing the first `P` positionally. git skips both steps for histogram (and
  for patience) -- `xdl_optimize_ctxs`, which does trimming AND cleanup, is
  guarded by `XDF_DIFF_ALG(xpp->flags) != XDF_HISTOGRAM_DIFF`. Cleanup would be
  actively wrong here: it DISCARDS lines, and occurrence counts are what this
  algorithm decides on.
- **`scanA` walks its region backwards**, so each line's record points at its
  FIRST occurrence and the chain runs downward; `try_lcs`'s `np` walk depends on
  that direction. Reversing it is observable but only on a fixture where a line
  occurs more than twice and the occurrences are not interchangeable -- see
  section 4, where the first mutation of this found nothing.

One deliberate difference, and it is not observable in the output: git's
`scanA` gives up (falling back to Myers) when 64 DISTINCT lines collide in one
hash bucket, a property of git's own hash function. sg classifies lines into
exact class ids rather than hashing into buckets, so that condition cannot
arise and there is nothing to reproduce. The other 64 -- the occurrence-count
ceiling that decides whether a candidate can be a split point at all -- is
reproduced exactly, and mutating it to 0 turns the histogram alignment test red
(it degrades into the Myers answer, which is what the fallback is).

### 3. The numbers

| measurement | before | after |
|---|---|---|
| `fuzz_merge.py 200` (default) | 1 / 200 | **0 / 200** |
| `fuzz_merge.py 200 --no-newline-edits` | 1 / 200 | **0 / 200** |
| `fuzz_merge.py 200 --seed 1000` / `--seed 5000` | -- | **0 / 200** each |
| `fuzz_diff.py 500` (Myers, regression check) | 0 / 500 | **0 / 500** (seeds 0 and 2000) |
| `fuzz_diff.py 500 --histogram` | -- | **5 / 500** (seed 0), **4 / 500** (seed 2000) |

Gates: `make` 0 warnings over 63 TUs, `make test` 61/61, `interop.sh`
1956/1956, `make sanitize` 61/61 with 0 sanitizer errors.

### 4. The residual, and why it is recorded rather than explained away

**`sg diff --histogram` differs from `git diff --histogram` on about 0.9% of
fuzz cases, and the port is NOT the obvious suspect.** Two independent
transcriptions of git's published `xhistogram.c` -- the C written here and a
separate one in Python, written to cross-check it -- agree with each other and
disagree with this machine's git on exactly those cases. Reduced to three
lines:

```
a = "P\nR\nP\n"   ->   b = "P\nP\nR\n"     both agree
a = "R\n\nR\n\n"  ->   b = "R\nR\n\n"      git: 1 change, both transcriptions: 3
```

For the second, the published algorithm's `try_lcs` cannot take the later,
equal-length, equal-rarity candidate -- `lcs->end1 - lcs->begin1 < ae - as ||
rc < index->cnt` is false for it -- yet git's answer requires exactly that.
Eight variants of the take-condition and the count-update policy were measured
against git over a 400-case corpus; the faithful one scored best (10
disagreements), no variant scored zero, and the two that appeared to were an
artifact of the harness skipping cases that fell back to Myers. A patience-diff
hypothesis was tested and rejected (this git's histogram differs from its own
patience in 124/400 cases).

So: the algorithm this machine's git calls "histogram" is not exactly the one
its published source describes, and rather than tune sg toward an unexplained
target, the divergence is measured, bounded, reproduced and left visible behind
an opt-in flag. **The path that matters is not affected**: `sg merge` agrees
with `git merge` on 800 fuzz rounds. Whether that is because merge's fixtures
do not reach the divergent shapes is unknown, which is why the histogram
fuzzer mode is a standing gate and not a one-off measurement.

### 5. Scope decisions

`--patience`, `--minimal` and `--diff-algorithm=<name>` are **rejected as
unknown flags**, not approximated: sg has two aligners, and answering
"patience" with either one is a wrong answer wearing the right flag (the same
reasoning as `-C -C` in Phase 33). Real git accepts all four spellings and
exits 129 on a bad algorithm name; sg exits 1, which is this project's
convention. Both sides are pinned in `tests/interop.sh`'s `phase42` group.

`sg stash show` did NOT gain the flag. While surveying, a pre-existing and
unrelated divergence turned up there and is recorded here so it is not
rediscovered as fallout from this phase: **`git stash show -M` implies `-p`
and switches the output from `--stat` to a patch, while `sg stash show -M`
stays on `--stat`** (measured). That is a general rule in git -- any pure diff
option implies `-p` for `stash show` -- and adding `--histogram` there without
fixing it would inherit the same hole.

### 6. Coverage, including the mutation that found nothing

Every rule got a named witness, and each was mutated individually:

| mutation | caught by |
|---|---|
| `merge.c`'s `SG_DIFF_ALGO_HISTOGRAM` -> `MYERS` (both call sites) | `test_merge_aligns_with_histogram` |
| `SG_HIST_MAX_CHAIN` 64 -> 0 (always fall back) | `test_histogram_alignment` |
| `hist_scan_a` walking forwards | `test_scan_direction_is_observable` |

The third row is the interesting one. The first attempt at that mutation was
**green**: the alignment fixtures use a line that occurs twice in positions
that make the two scan directions interchangeable. A fixture where a line
occurs three times (`cc, cc, blank, cc` against `cc, aa, cc`) separates them,
and it was found by running both directions of the model over a corpus rather
than by staring at the code -- 249 of 3000 random pairs distinguish them, and
none of the fixtures written by hand did.

`merge.c` has TWO call sites, not one: `script_matches` (the sync-point
alignment) and `refine_conflicts` (the zealous refinement Phase 41 added).
Both take histogram, because git's refinement runs under the same `xpp` flags
as the merge that called it. A survey that named only the first would have
left the merge path internally inconsistent -- one layer on histogram, the
other on Myers -- with nothing failing to say so.

## Phase 43: `sg merge` takes a revision, not just a branch name

`sg merge` called `sg_ref_branch_exists` directly, so a tag, a
`refs/heads/...` path and a `topic~0` were each rejected with `invalid
reference` while the equivalent `git merge` worked. That contradicted this
project's own stated rule -- a user-supplied revision always goes through
`sg_rev_parse_commit` -- and Phase 41 recorded it as out of scope rather than
fixed. It is one line of resolution plus one thing that is not obvious: the
merge MESSAGE depends on which form the user typed.

### 1. What git names a merge, measured

Every row is a `git merge --no-commit` whose `.git/MERGE_MSG` was read back,
git 2.55.0:

| argument | message |
|---|---|
| `topic` | `Merge branch 'topic'` |
| `refs/heads/topic` | `Merge branch 'refs/heads/topic'` |
| `v1` (lightweight tag) | `Merge tag 'v1'` |
| `av1` (annotated tag) | `Merge tag 'av1'` |
| `topic~0` | `Merge branch 'topic'` |
| `<40-hex>` | `Merge commit '<40-hex>'` |

Two things are easy to get backwards, and both are in that table. **The name
printed is the argument AS TYPED, not the ref that was found** -- which is why
`refs/heads/topic` keeps its prefix instead of being shortened. And **a
trailing run of `^`, or a trailing `~<digits>`, is stripped before
classifying, and the SHORTENED name is what gets printed** -- which is why
`topic~0` reads as a branch merge rather than a commit merge.

The conflict marker's theirs label is simpler: it is the argument as typed in
all six forms, which is what sg already did.

### 2. The " into <branch>" suffix, and what it does NOT follow

| current branch | message |
|---|---|
| `master` | `Merge branch 'topic'` |
| `main` | `Merge branch 'topic'` |
| `trunk` | `Merge branch 'topic' into trunk` |
| `trunk`, with `init.defaultBranch=trunk` | `Merge branch 'topic' into trunk` |
| detached HEAD | `Merge branch 'topic' into HEAD` |

So the omission is hard-coded to the two names, **not** to the configured
default branch -- measured, because assuming it followed `init.defaultBranch`
would have been the natural guess. sg previously appended the suffix always,
which was right for every branch except the two that matter most.

All twelve argument x branch combinations were compared against git after the
change and all twelve match byte for byte.

### 3. Pinned, and mutated

`tests/interop.sh`'s `phase43` group builds the history ONCE with sg and
copies it, so both tools merge identical commits; 16 checks. Four mutations,
each caught by exactly the checks written for it:

| mutation | caught by |
|---|---|
| `build_merge_name` always says "branch" | the two tag messages |
| the suffix rule always appends | all five messages (they run on master) |
| the `~N` stripping disabled | only `topic~0`'s message |
| resolution back to `sg_ref_branch_exists` | the tag and `~N` acceptance checks |

### 4. Deliberately unchanged

**`sg merge`'s fast-forward output stays a bare `Fast-forward`** where git
prints `Updating <a>..<b>`, `Fast-forward`, and a diffstat. That divergence
predates this phase, is identical for a plain branch argument, and has
nothing to do with which revision forms are accepted -- fixing it means
implementing the diffstat line, which is its own piece of work. Recorded here
so the next reader does not discover it as fallout from this change.

The error wording now matches git's (`<rev> - not something we can merge`)
with this project's own `sg: ` prefix where git writes `merge: `, and exit 1
where git also uses 1. Both sides are pinned.

## Phase 44: `sg stash show`'s implied `-p`

Found while surveying for Phase 42, not by a failing test: **`git stash show
-M` prints a patch, `sg stash show -M` printed a diffstat.** The divergence
predates every phase that touched stash, was never recorded as deliberate, and
nothing in the suite could see it -- there was no check that gave `sg stash
show` a diff option and looked at which FORMAT came back.

### 1. git's rule, measured

`git stash show` defaults to `--stat`. Beyond that:

| flag class | example | output |
|---|---|---|
| format selector | `--stat`, `--numstat`, `--shortstat`, `--name-only`, `--name-status`, `-p` | that format |
| other diff option | `-M`, `-C`, `--no-renames`, `--histogram`, `--patience`, `-U5` | **patch** |
| stash-specific | `-u`, `--include-untracked`, `--only-untracked` | stat (unchanged) |

Two details decide the implementation, and both are measured:

- **`-u` neither implies nor suppresses.** `-u` alone stays a stat; `-u -M`
  is a patch. So the stash flags are simply not part of the rule, rather than
  being a third state.
- **An explicit format wins regardless of order.** `-M --stat` and `--stat
  -M` both print a stat. That rules out the obvious implementation -- setting
  the format from inside the parse loop, last-one-wins -- which passes one of
  those two and fails the other. `cmd_stash.c` therefore tracks
  `format_given` and `diff_opt_given` and resolves them after the loop.

All 22 flag combinations tried now match git.

### 2. `--histogram` came along, on purpose

`sg diff` gained `--histogram` in Phase 42 and `sg stash show` shares the same
renderer, so it would have been the one command where the algorithm could not
be chosen. It is also a second, independent witness for the implied-`-p` rule:
a fix that special-cased `-M` alone would pass the rename checks and fail the
histogram one.

### 3. Coverage

`tests/interop.sh`'s `phase44` group is 16 checks, each half an oracle
(what git does) and half sg's answer. Three mutations, each caught by exactly
the check written for it:

| mutation | caught by |
|---|---|
| drop the implied `-p` | the three "implies -p" checks + `-u -M` |
| resolve the format inside the parse loop | only `--stat -M`, i.e. the order half |
| make `-u` count as a diff option | only `-u alone stays a stat` |

The second and third rows are the point of writing the rule as two flags: a
single-boolean implementation gets caught by exactly one check each, so the
tests distinguish the three plausible wrong implementations from each other
rather than merely failing together.

## Phase 45: the three unmerged labels that had no oracle

`unmerged_label` maps seven stage combinations to seven long-format strings
and seven porcelain codes. Phase 38 gave four of them a real-git oracle and
recorded the other three -- `both deleted:`, `added by us:`, `added by them:`
-- as unreachable, on the grounds that "an ordinary merge cannot produce those
stage combinations (DD auto-resolves; AU/UA needs a rename or a hand-built
index)". That was true of a CONTENT merge and false in general.

### 1. One fixture produces all three

Measured, eight merge shapes tried:

| shape | stages left unmerged |
|---|---|
| modify/delete | `1,2` (deleted by them) |
| delete/modify | `1,3` (deleted by us) |
| add/add | `2,3` (both added) |
| rename/delete | `1,2` |
| rename/modify | none -- resolves |
| **rename/rename to different names** | **`f.txt:1`, `a.txt:2`, `b.txt:3`** |

The last row is the whole phase: base has `f.txt`, ours renames it to
`a.txt`, theirs renames it to `b.txt`, and git leaves three unmerged paths --
one with only stage 1, one with only stage 2, one with only stage 3 -- which
is exactly the three combinations that had no witness. sg already printed all
three correctly; nothing had ever checked.

### 2. Why the fixture is built by git, and what that pins

`sg merge` has no rename detection, so the same history merges **cleanly**
under sg: it sees `f.txt` deleted on both sides, `a.txt` added by ours only,
`b.txt` added by theirs only, and keeps both files. So the oracle here is
necessarily one-directional -- git builds the index state, sg reads it, the
same bidirectional-interop shape the stash groups already use.

That divergence is pinned in the same group rather than left implicit. If sg
ever grows rename-aware merging, `phase45: sg's own merge resolves
rename/rename cleanly` turns red and says so, instead of the fixture quietly
starting to build itself the other way and the labels losing their oracle
without anyone noticing.

### 3. What went wrong while writing the check, and why it is worth recording

The first run failed with what looked like a real label divergence: sg
printed three labels, git printed none. The cause was neither tool. The group
reused `$P38_GIT_FLAGS` -- the three declared environment axes from Phase 38
-- but sits EARLIER in `interop.sh` than the line that defines it, and
`interop.sh` runs under `set -u`, so the subshell aborted before git ran and
the comparison file came out empty.

**An empty oracle file compares as a difference, not as an error**, which is
the same failure direction this project has recorded before: verification
tooling fails toward "already verified" or toward a confident wrong answer,
never toward "I could not run". The flags are now spelled out locally with a
comment saying why, rather than depending on declaration order.

### 4. Coverage

Five mutations, each caught by exactly the checks written for it:

| mutation | caught by |
|---|---|
| `both deleted:` -> `BOTH DELETED:` | the byte-for-byte cmp + its own named check |
| `added by us:` -> `added by US:` | same, its own |
| `added by them:` -> `added by THEM:` | same, its own |
| the `DD` porcelain code | only the porcelain cmp |
| the `AU` porcelain code | only the porcelain cmp |

The per-label checks exist alongside the sorted byte-for-byte comparison on
purpose: the cmp alone would say "the block differs" without saying which of
the three moved.

## Phase 46: the two push refspec forms Phase 39 refused

Phase 39 implemented `[+]<src>[:<dst>]` and named two real git features it
deliberately would not approximate: **wildcard refspecs** and the bare
**push-matching `:`**. Both are implemented here. The interesting part is not
the code -- it is that the two forms need their input from opposite sides of
the network round trip.

### 1. Wildcards take their source set from the LOCAL repo

Measured against git 2.55.0 with a local bare remote:

| refspec | effect |
|---|---|
| `refs/heads/*:refs/heads/*` | pushes every matching LOCAL ref, **creating** ones the remote lacks |
| `refs/heads/*` (no colon) | mirrors: same as writing the pattern on both sides |
| `refs/heads/*:refs/remotes/up/*` | the captured text is substituted into the dst's star |
| `refs/tags/*:refs/tags/*` | an annotated tag arrives as a **tag object**, unpeeled |
| a pattern matching nothing | exit 0, "Everything up-to-date" |
| `refs/*:refs/*` | matched `refs/remotes/origin/topic/sub` -- **the star crosses `/`** |
| `refs/heads/fe*`, `refs/heads/*/sub` | fine -- the star may sit anywhere, not only at a segment boundary |
| a star on one side only, or two on a side | `fatal: invalid refspec` |
| `:refs/heads/*` (wildcard deletion) | `fatal: invalid refspec` |

Because the source is local, expansion runs **before** the network round
trip, which keeps Phase 39's rule that a src resolving to nothing aborts the
whole batch before anything lands. There is no prune semantics: a pattern
never deletes a remote ref that no longer exists locally.

Matching is a plain prefix/suffix comparison around the single star, which is
git's own rule, and the captured middle is substituted into the dst pattern.
The refs to consider come from `sg_ref_list_under` on the namespace up to the
last `/` before the star -- or `refs/` when the star comes first, which is
also why a pattern not rooted at `refs/` simply matches nothing instead of
erroring (measured: `m*:m*` is "Everything up-to-date").

**One measured difference worth knowing before touching this**: an expanded
dst is used VERBATIM, with no dwim completion -- the opposite of an explicit
dst. git sent the uncompleted name to the remote, which refused it as a
"funny refname"; sg refuses it locally with its own message. Both exit 1.
Routing wildcard destinations through `complete_dst` instead would have been
the natural-looking choice and would have quietly turned `refs/heads/*:x*`
into pushes to `refs/heads/x<name>`.

### 2. Push-matching cannot be expanded until the advertisement arrives

`:` means "every local branch that already exists on the remote", so half its
input is the remote's advertisement. Measured:

- a local-only branch is **not** created;
- a tag that moved locally is **not** updated -- matching is branches only;
- per-ref rules are unchanged: one branch can land while another is rejected
  as non-fast-forward, exit 1, and `+:` forces;
- when nothing matches, exit 0 and "Everything up-to-date" (git only errors
  when the remote has no refs at all, which is a transport-level message).

So the expansion is appended to `candidates` right after
`sg_transport_ls_refs_push` and before the candidate->entry loop, which
leaves every rule that loop already implements -- fast-forward checks,
per-ref rejection, the report, the remote-tracking update -- applying
unchanged.

### 3. Two things the survey caught that the spec had not

A survey of `cmd_push.c` was run before writing any code, and two of its
findings changed the design:

- **`push_entry` assumed one refspec produces at most one candidate.** Both
  new forms are fan-outs. Everything downstream turned out to cope, because
  Phase 39 had already made the entry loop multi-ref, but the candidate
  builders had to grow a second shape.
- **`complete_dst`'s "dst matches more than one" rule would have killed every
  wildcard.** That rule exists to stop an ambiguous unqualified dst; a
  wildcard is ambiguous by definition. Expanded dsts bypass completion
  entirely (see section 1), so the two never meet -- but routing them through
  it was the obvious implementation and would have rejected every valid
  wildcard push.

The survey also asserted that wildcard expansion must happen after the
advertisement. That was wrong, and the measurement in section 1 is why: the
source set is local, so it can and must happen before.

### 4. A struct with two definitions, and the sanitizer that caught it

`sg_push_refspec` lives in `cmd_push.c` with no public header;
`tests/test_refspec.c` declares its own copy via `extern`, a convention this
project uses on purpose and documents in that file: *"The struct definition
below must stay field-for-field identical to cmd_push.c's."*

Adding `is_wildcard` and `is_matching` broke exactly that. The library's
`memset(out, 0, sizeof(*out))` then wrote 40 bytes into the test's 32-byte
stack object. **`make test` did not catch it** -- it reported only the three
expected assertion failures from the behaviour change. ASan caught it:
`stack-buffer-overflow ... WRITE of size 40`. This is the case CLAUDE.md
already describes ("adding a field to a shared struct also counts as touching
memory management") appearing in the one shape that note does not mention: a
struct duplicated across translation units on purpose.

### 5. Coverage, including the check that had to be rewritten

Five mutations, each caught by the checks written for it:

| mutation | caught by |
|---|---|
| matching drops its "already on the remote" filter | "did NOT create the local-only branch" |
| the wildcard's suffix comparison removed | "creates NO other ref under that prefix" |
| the dst substitution drops the capture | four checks, including the substitution one |
| the matched ref read through `sg_rev_parse_commit` (peeling) | only the annotated-tag check |
| (Phase 39's own rejection restored) | the phase46 reversal checks |

The second row took two tries. The first fixture used patterns ending in
`*`, where the suffix is empty and the comparison is dead weight -- the
mutation stayed green. A star in the MIDDLE makes it live, and even then the
first check was the wrong discriminator: dropping the suffix comparison does
not resurrect the excluded name, it invents a TRUNCATED one, because the
capture length is computed from the pattern. Asserting the exact set of refs
under the prefix is what finally turns it red. Both dead ends are recorded
because either one alone reads as "this rule has coverage".

### 6. The review round: one real defect, and two checks that proved nothing

A cold read of the diff found one defect that no gate caught, and its shape is
worth more than the fix.

**`refs/sg/chunks` had two owners.** `wildcard_expand_candidates` enumerates
every local ref under the pattern's namespace, and the keepalive ref is a
local ref like any other -- while the dedicated chunks-propagation block
further down computes its OWN old/new pair for that same ref on every push,
sometimes merging the two sides into a fresh commit. A pattern wide enough to
match it queues both. Reproduced: the remote answers

```
error: multiple updates for ref 'refs/sg/chunks' not allowed
```

and because the push is atomic **nothing lands at all** -- not the chunks
ref, and not the ordinary branch the user actually meant. Every Phase 46
fixture up to that point used narrow patterns rooted at `refs/heads/`, which
structurally cannot reach it. The fix gives the ref exactly one owner: the
expansion skips it.

**Two checks written for that fix proved nothing, in two different ways**,
and both are recorded because each looked correct:

- The first version reused the shared HTTP fixture, whose remote already had
  a keepalive ref from earlier pushes. With both sides equal the propagation
  block sends nothing, so there is no second update to collide with:
  **removing the exclusion left the check green.** It now builds its own
  empty bare repo and its own fresh local repo, and asserts the precondition
  (local has one, remote does not) rather than assuming it.
- The second version grepped the push output for git's collision wording.
  Under the mutation that check **stayed green while the push had in fact
  failed**. The exit code is the property that matters; the message is the
  remote's to word.

Two other findings were acted on. The remote-tracking narrowing (only a
destination under `refs/heads/` gets one) had no fixture at all -- correct by
inspection, unprovable by test -- and now has one that turns red when the
narrowing is removed. And two over-long-name paths silently `continue`d where
the rest of the function aborts; they now report, matching this project's
rule that a reporting path must never quietly drop a ref.

Two are recorded and NOT acted on, so they are not rediscovered as bugs:
`matching_force = matching_force || parsed.force` is untested, because no
fixture passes two `:` arguments in one invocation; and for a destination
outside `refs/heads/`/`refs/tags/` the report line prints the full path on
both sides (`refs/remotes/up/topic/sub -> origin/refs/remotes/up/topic/sub`),
which is ugly but honest, and this command's report has never been required
to match git byte for byte.

## Phase 47: the SSH transport

`sg clone`/`fetch`/`push` spoke smart HTTP and nothing else. This adds
`ssh://[user@]host[:port]/path` and the scp-like `[user@]host:path`.

The surprise is how little of it is protocol. **pkt-line framing, the
want/have negotiation, sideband demultiplexing and the push report are all
transport-independent and are reused byte for byte.** Three things are not:

1. **The `# service=...` envelope is smart-HTTP's, not the protocol's.** It
   exists so a single GET can declare which service it wants. Over ssh the
   service IS the command run on the far side, so the advertisement starts at
   the first ref. `parse_ref_advertisement_for_service` grew an
   `expect_service_line` parameter; requiring it over ssh rejects every valid
   advertisement as malformed.
2. **It is one bidirectional stream**, not a request/response pair.
3. **There is no URL to build**: the path is an argument to the remote
   command and the host is an argument to `ssh`.

### 1. What git actually runs, measured

`GIT_SSH_COMMAND` was pointed at a script that logs its argv:

| url | argv git runs |
|---|---|
| `ssh://host/srv/repo.git` | `ssh -o SendEnv=GIT_PROTOCOL host "git-upload-pack '/srv/repo.git'"` |
| `ssh://user@host:2222/srv/repo.git` | `ssh -o ... -p 2222 user@host "git-upload-pack '/srv/repo.git'"` |
| `host:srv/repo.git` | `ssh host "git-upload-pack 'srv/repo.git'"` |
| `ssh://host/~alice/repo.git` | `ssh host "git-upload-pack '~alice/repo.git'"` |
| a push | ... `git-receive-pack '<path>'` |

Four things that decide the implementation, none of them guessable:

- **The remote command is ONE argument**, with the path single-quoted,
  because the far side runs it through a shell.
- **The two URL forms disagree about the leading slash.** `ssh://` keeps it;
  the scp-like form has none to keep. And `ssh://host/~alice/...` DROPS it,
  so the far side's shell expands the home directory -- that row is not a
  typo.
- **`host:22` is a PATH named 22.** The scp-like syntax has no port at all.
- **`GIT_SSH_COMMAND` is word-split**: `"<prog> -vvv"` reaches ssh as two
  argv entries.

Also measured, and NOT reproduced: git runs `ssh -G` first as a capability
probe, and passes `-o SendEnv=GIT_PROTOCOL` to hand the far side a protocol
version. sg speaks protocol v0 only, so it sends neither -- there is nothing
for the far side to select.

Where the local error lives was measured too, because the tidy-looking answer
is the wrong one: `ssh://host/` asks for the path `/` and `host:` asks for
the empty path, both refused by the far side; only `ssh://host`, with no path
separator at all, fails locally.

### 2. One connection per call, deliberately

Real git holds ONE ssh connection open across the advertisement and the
negotiation. sg's four `sg_transport_*` entry points are each independently
callable, which is the shape smart HTTP gave them, and changing that means
threading a connection object through `clone`/`fetch`/`push`.

So each call opens its own connection, and `sg_ssh_request` **reads and
discards the advertisement it is sent again** before writing its request.
The cost is one extra `ssh` spawn per operation. The benefit is that no
public API changed and the HTTP path is untouched.

### 3. The first subprocess in this codebase

Nothing in `src/` had ever forked or exec'd anything. Each piece of the
plumbing is there for a specific failure:

- **`poll()` writing and reading at once.** The far side starts answering
  before it has consumed the whole request, so write-everything-then-read
  deadlocks as soon as both pipe buffers fill.
- **`SIGPIPE` ignored around the write.** A far side that dies mid-request
  makes the write raise it, and the default action kills sg outright with no
  message -- which reads as a crash rather than as a remote failure.
- **Half-close after the request.** The far side reads to EOF to know the
  request is complete; keeping the pipe open hangs both ends.
- **A flush before closing an advertisement-only connection.** That is a
  client saying "I want nothing"; without it upload-pack reports a hung-up
  connection, an error message for a successful operation.
- **The child's stderr is left alone**, so ssh's own diagnostics (host key
  prompts, permission denied) reach the user. This transport cannot
  authenticate on its own and that message is the only useful thing it has.
- **`waitpid` on every path**, success included, so no zombie is left behind.

A path containing a single quote is refused rather than escaped: it is a
shape this project has no measured behaviour for, and guessing at quoting for
a string that reaches a remote shell is a failure direction that does not get
a second chance.

### 4. A credential leak the new URL form would have opened

`sg_url_redact` returned any string with no `scheme://` unchanged. The
scp-like form has no scheme and real userinfo, so `git@host:path` would have
printed the user name straight into an error message. It now redacts up to
the FIRST colon -- an `@` after the colon is part of the path, not userinfo,
and redacting on the last `@` in the whole string would have eaten the host
name. The pre-existing "no scheme is returned unchanged" test still passes,
but its claim was narrowed: that string survives because it has no colon, not
because schemeless means untouched.

### 5. Testing needs no server

The HTTP group needs a live CGI server and skips itself when one cannot
start. The ssh group needs nothing: `ssh` is replaced by a shim that ignores
its options and the host and runs the remote command locally -- which is what
git itself does with `GIT_SSH_COMMAND`. So **real git is the oracle over the
same transport**: both tools clone the same URL through the same shim and the
two working trees are compared byte for byte.

12 checks, and five mutations each caught by them: never recognising an ssh
URL, dropping the "I want nothing" flush, keeping the advertisement in the
response, requiring the service line over ssh, and dropping the `~` rule
(that last one only the unit test sees, which is why it exists).

### 6. Known, and not fixed here

`sg` warns `ignoring invalid ref name 'HEAD' from remote` on every fetch and
clone, because the advertisement's first ref is `HEAD` and
`sg_ref_name_is_safe` requires a `refs/` prefix. **This is pre-existing and
identical over HTTP** -- measured, not assumed -- so it is not something the
ssh path introduced, and fixing it is a separate change to that predicate's
callers rather than to a transport.

`core.sshCommand` is not read: sg's config reader knows about remotes and
little else, and `GIT_SSH_COMMAND`/`GIT_SSH` cover the same need without
teaching it a new section.

### 7. Which sanitizer sees this code, and which does not

A LOCAL `make sanitize` builds the unit test binaries with ASan and runs
those. The only ssh code a unit test reaches is URL parsing -- the fork, the
poll loop and the pipe handling live behind `interop.sh`, which locally runs
against the ordinary build. So on this machine the newest and least
precedented code in the project is the code its memory gate cannot see.

**CI is not in that position, and checking rather than assuming is the point
of this section.** Its ASan job runs `interop.sh` itself under ASan+UBSan
with `detect_leaks=1` (`.github/workflows/ci.yml`), so the whole ssh group --
clone, push, fetch, the failing path -- is sanitized and leak-checked there,
and this phase's own CI run passed with the group in place. The first draft
of this section said flatly that the sanitizer could not see this code; that
was true locally and wrong about CI, which is exactly the kind of claim this
file exists to keep honest.

Locally the scenario was still driven by hand against an ASan build, because
a green local `make sanitize` proves nothing about that file:

```
make clean && make sanitize          # builds build/sg with ASan too
export GIT_SSH_COMMAND=<shim>        # the same shim tests/interop.sh writes
sg clone ssh://localhost<bare> c && (cd c && sg push origin master && sg fetch origin)
sg clone ssh://localhost<nosuch>     # the failing path matters too
```

Clean on all four for Phase 47. Anyone touching `src/net/ssh.c` should redo
it locally, or push and let CI's ASan job do it properly.

### 8. The review round: a user-visible bug in a file this phase never touched

A cold read of the diff found seven things. The worst of them is not in the
new file at all.

**`sg clone git@host:myproject.git` created a directory named
`git@host:myproject`.** `derive_target_dir` (`src/cli/cmd_clone.c`, untouched
by this phase) scans back from the end of the URL to the last `/`. Every URL
form that existed before had one; the scp-like shorthand with a
single-segment path has none, so the scan ran off the front and took the host
with it. Measured: git clones the same URL into `myproject`. The scan now
also stops at the scp-like colon.

This is the shape worth remembering: **a new input form can break a function
that no one edited**, and the diff-shaped question ("is the new code right?")
would never have reached it. The interop check that would have caught it did
not exist because every scp-like fixture passed an explicit destination
directory -- the convenient thing to write, and the one that skips the guesser
entirely.

Four more were fixed:

- An **empty port** (`ssh://host:/repo.git`) produced `-p ""` on ssh's command
  line. Measured: git passes no `-p` at all. At best that is a confusing ssh
  error, at worst an option that swallows the host argument.
- **`sg_url_redact`'s new branch was wider than the routing rule.** The
  transport routes on "colon before any slash"; the redactor did not, so a
  local relative path like `a/b@c:d` was rewritten to `***@c:d` -- corrupting
  a string that was never a URL. Over-redaction rather than a leak, but it
  contradicted the function's own promise.
- **`sg_ssh_advertise` did not notice a missing advertisement** the way its
  sibling does. A remote command that exits 0 while sending nothing usable
  surfaced downstream as "not a valid git smart HTTP ref advertisement" --
  the wrong transport named for a pure-ssh failure.
- **The child's `dup2` sequence closed what it had just installed** if
  `pipe()` handed back fd 0 or 1, which happens when sg is invoked with those
  descriptors closed. Now the originals are closed only when they are above
  stderr.

One was examined and left alone, with the reason recorded rather than the
finding dropped: `ssh://user@/repo.git` (userinfo, empty host) passes through
as the host argument `user@`. **Measured: that is exactly what git does.**
Rejecting it locally would have been a tidier answer than git's, not the same
one.

And one is a genuine blind spot, named as such: `ssh_pump`'s "no request"
branch is unreachable from either caller today, so no test can turn red for
it either way. It was made explicit rather than deleted -- a zero-length
request must still close the write side, or both ends wait forever -- and its
contract now says plainly that it does NOT send a flush, which is the thing
the next caller would assume.

Three of the fixes have witnesses that fail against the old code: the clone
directory name (interop), the empty port and the over-redaction (unit tests).
The advertisement check and the `dup2` guard do not: reaching them needs a
remote command that exits 0 while sending garbage, and an sg invoked with fd
0 closed. Both are recorded here as unverified rather than counted as covered.


## Phase 48: the reflog gaps Phase 17 left behind

Phase 17 generalized the reflog and, in doing so, wrote down three places it
deliberately did not reach. All three are closed here. None of them is
large; what makes them worth a section is that each one's *measured* shape
disagrees with the shape you would guess.

### 1. `sg stash push` never logged the reset it performs

`git stash` resets the working tree to HEAD, and logs that reset.
Measured on a repo with one commit and one dirty file:

| log | after `git stash` |
|---|---|
| `logs/HEAD` | `commit (initial): one`, then `reset: moving to HEAD` |
| `logs/refs/heads/master` | `commit (initial): one` -- **nothing added** |
| `logs/refs/stash` | `WIP on master: <sha> one` |

The asymmetry is not a special case in stash; it is Phase 17's rule 1
falling out. The reset moves HEAD from a commit to *the same* commit, so
`old == new`: a concrete ref suppresses that as a no-op, while `logs/HEAD`
appends unconditionally. Routing the write through `sg_ref_move_head` --
the same function `commit`/`reset`/`merge` already use -- reproduces both
halves with no branch of its own.

The write is deliberately **not** fatal on failure. By the time it runs the
stash commit exists, `refs/stash` is updated and the working tree is
already reset; returning failure there would report a broken stash for a
stash that is entirely intact. A stderr warning names it instead.

The interop check compares the message column of both log files against
real git's, and pins the branch log's *emptiness* separately -- a
"append to both" implementation matches `logs/HEAD` perfectly.

#### 1a. Two cases where git logs nothing, and one of them was never about stash

Measuring only the fixture above gets this wrong. Two more, both measured,
both of which sg got wrong when the line was first added:

| command | `logs/HEAD` gains |
|---|---|
| `git stash` on a branch | `reset: moving to HEAD` |
| `git stash push -- <path>` | **nothing** |
| `git stash` on a detached HEAD | **nothing** |

The partial push is easy once seen: it resets only the named paths, so HEAD
is never updated at all. sg's partial path (`restore_matched_paths`) likewise
has no HEAD update of its own, so the call is simply skipped there.

The detached case is not a stash fact at all. Checked against `reset --hard`,
which shares nothing with stash but the ref write:

| situation | git logs |
|---|---|
| symbolic -> detached, same commit | yes (`checkout: moving from ...`) |
| detached -> detached, same commit | **no** |
| detached -> detached, other commit | yes |
| on a branch, same commit | yes (mirrored from the branch's suppressed update) |

So **Phase 17's "logs/HEAD always appends, even for a no-op" is a property of
the mirroring path, not of HEAD's file.** When HEAD is written *directly* --
which is exactly the detached case -- it obeys the ordinary `old != new` rule
like any other ref. The one thing that must not be swallowed with it is the
symbolic-to-detached transition, which git logs even when the commit is
unchanged; the condition is therefore "HEAD was *already* detached **and**
old == new", and it lives in `sg_ref_set_head_detached`, the single writer of
a detached HEAD.

This was a pre-existing divergence, not one this phase introduced: `sg reset
--hard HEAD` on a detached HEAD grew a line real git does not write, and had
since Phase 18. Adding the stash line is what made it worth measuring. A
corrupt HEAD (`sg_ref_head_is_detached` returning -1) deliberately counts as
"not already detached" and still logs -- it is about to be overwritten, and a
spurious line is a safer failure direction than silence.

Suppressing too much is as wrong as suppressing too little, so the interop
fixture pins all three corners in one repo: the no-op logs nothing, the move
still logs, and the transition into detachment still logs.

### 2. `sg clone` created no `refs/remotes/<remote>/HEAD`

Measured, after `git clone bare.git dst`:

```
dst/.git/refs/remotes/origin/HEAD        -> "ref: refs/remotes/origin/master"
dst/.git/logs/refs/remotes/origin/HEAD   -> one line, "clone: from <url>"
dst/.git/logs/refs/remotes/origin/master -> does not exist
```

Three separate facts, and only the first is obvious:

- it is a **symbolic** ref, not a copy of the branch's sha;
- its log's old id is all-zeros and its new id is the branch tip;
- the remote-tracking **branch** gets no log at all. Clone writes those refs
  directly; their logs only start at the first fetch. So "give everything
  under `refs/remotes/` a log" passes every check about `origin/HEAD` while
  inventing history for refs git leaves alone, and the interop group pins
  that absence on both sides for exactly that reason.

Phase 17 recorded this as a deliberate divergence, with a reason that was
sound at the time: writing only the *log* was tried and removed, because
`sg reflog origin/HEAD` resolves names through `sg_rev_parse_ref_path`,
which needs the ref -- so a log with no ref claims a history the repository
does not have. Phase 48 creates the ref, which removes the reason rather
than overruling it. The old interop pin ("no ref, and no log either") is
replaced by a pin of the pairing itself: **never one without the other**.

This is the project's first arbitrary symbolic ref. `sg_ref_set_symref`
follows `sg_ref_set_head`'s shape exactly -- append the log line first,
write the file, truncate the log back off if the write then fails -- and
inherits Phase 17's namespace policy unchanged (a message for a ref outside
the four logged namespaces is refused outright, writing nothing).

Its old-id lookup has a known limitation, stated in the header: a symref
that already exists reads as all-zeros, because a `ref: ...` file has no id
to parse. That is the same limitation `sg_ref_set_head_detached` documents,
and it is harmless here because the only caller creates the ref.

### 3. bare `@{N}` and bare `@`

`@{1}` is **not** `HEAD@{1}`. Measured, in a repo where a checkout away and
back has moved `logs/HEAD` but not the branch's log:

| expression | resolves to |
|---|---|
| `@{1}` | `master@{1}` -- the current branch's log |
| `HEAD@{1}` | a different commit |
| `@` | `HEAD` |
| `@~1`, `@{1}~1` | suffixes chain normally |

git's own out-of-range message settles it beyond doubt: `@{99}` reports
`refs/heads/master` has too few entries, naming the branch, not HEAD.
On a **detached** HEAD there is no branch and it falls back to `logs/HEAD`;
on an **unborn** HEAD git rejects the argument outright. A corrupt HEAD must
be rejected too, and only `sg_ref_head_is_detached`'s tri-state separates it
from detached -- Phase 18's rule, and the reason a NULL test on
`sg_ref_current_branch` is the wrong predicate here.

Both spellings are implemented by rewriting `base` before anything else
runs, so the `@{N}` lookup, `resolve_base` and the `~`/`^` loop below are
untouched and `@{1}~1` works for free. The one thing the rewrite must not
do is swallow an empty base generally: `~1`, `^` and `@{` alone are still
parse errors, and a unit test pins that.

One measured case is **deliberately not reproduced**. With the current
branch's reflog file deleted by hand, real git lets `@{0}` fall back to the
branch's tip while still rejecting the spelled-out `master@{0}`:

```
$ git rev-parse @{0}         # -> the tip
$ git rev-parse nolog@{0}    # -> fatal: ambiguous argument
```

sg rejects both. This is not on the deliberate-divergence list, because
reaching it requires deleting a log file by hand -- both tools write one for
every `refs/heads/` update -- and because the divergence sg would be
choosing is *between its own two spellings*. A uniform rejection is a
smaller lie than an asymmetry whose rule this phase could not measure the
boundary of.

### Mutation record

Nine mutations, all against `--interop`. Each turned the check named beside
it red, and the last one is the deliberately over-broad direction:

| mutation | caught by |
|---|---|
| `"ref: %s\n"` -> `"%s\n"` | the symref cmp, the "holds a ref: line" check, and three `git fsck` checks |
| clone's reflog message -> `NULL` | the one-line log check, and phase17d's never-one-without-the-other pairing |
| bare `@{N}` always reads HEAD | `sg resolves a bare @{1} to the same commit git does` |
| bare `@` rejected | three probes, phase17c and phase48 |
| stash's reflog message -> `NULL` | the `logs/HEAD` message cmp |
| clone logs remote-tracking branches too | the negative "no log for origin/`<branch>`" checks |
| never suppress the detached no-op | the detached-stash cmp **and** the `reset --hard` count |
| write the line for a partial push | the partial-stash cmp |
| suppress the symbolic-to-detached transition as well | `and it still logs the transition`, plus two phase18e checks |
| drop `ref_path_reflog_allowed` from `sg_ref_set_symref` | `test_refs` only -- interop has no witness for this guard, deliberately noted rather than claimed |

One defense line is **unverifiable and recorded as such**: the
`sg_reflog_truncate` calls on `sg_ref_set_symref`'s failure branches. No test
forces a mid-write failure (a read-only directory, a full disk), so removing
any one of them turns nothing red. This is the "mathematically unobservable"
category, not a coverage gap a better mutation could reach; closing it would
take a fault-injection fixture the project does not have.

A second review round, over the code the first round's fixes produced, found
that one of those fixes was itself untestable: the first attempt at the
truncation problem added a guard on a `snprintf` into a 4 KB buffer, and
nothing in the suite can drive a 4 KB clone url through that path, so
reverting the guard would have passed all five gates. The second fix deletes
the buffer instead -- the message is `reflog_msg`, already malloc'd to fit
and already the string every other reflog line of the clone uses, so the
failure mode stops existing rather than being guarded against. **Preferring
"remove the branch" over "add a guard you cannot test" is the general rule
here**, and it is the same reasoning as the project's standing note that a
review-found fix with no failing test behind it has not landed.

Two defense lines from that round are recorded as not independently
observable, rather than counted: the if/else fixture builder behaves
identically to the `&&`/`||` spelling it replaced unless the builder actually
fails, and the "refusing `@~1` left HEAD where it was" check cannot be tripped
by any single mutation without also tripping the check beside it, because
`cmd_switch.c` resolves the revision and returns before writing anything.
Both are insurance against a future refactor, not present coverage.

A cold review also asked whether a no-op symref update should be suppressed
the way a concrete ref's own log is. Measured rather than reasoned about:
running `git remote set-head origin master` twice with nothing changing
appends a line each time, so the unconditional append is git's own answer.
It stays unreachable until a second caller exists, and is now written down
as measured instead of inherited.

Two things went wrong while measuring, and both are the kind that fake a
pass:

- the first attempt at the stash mutation matched the phrase inside the
  **comment** above the call, not the call. `mutate.sh` reported a clean
  green and it read as a blind spot. The rule is the one already in
  `CLAUDE.md`: a literal that also appears in prose needs surrounding
  context, not `/g`.
- the detached/partial checks were first written with `$( )` nested inside a
  quoted `sh -c "..."`, and the escaping collapsed into a **shell syntax
  error**. `bash` aborted `interop.sh` at that line, so every check below it
  silently did not run -- and two mutation rounds were then scored as
  "caught" on the strength of red lines that all came from checks *above* the
  break. The values are computed into shell variables first now, and
  `bash -n tests/interop.sh` is the check that would have caught it in one
  second.

### What this closes, and what it does not

The phase does not touch `@{<date>}`, `@{upstream}`, or
`sg reflog expire|delete`; those remain unimplemented and are listed here so
the next reader does not mistake "Phase 17's gaps are closed" for "the
reflog is complete".

## Phase 49: rename detection in the three-way merge

Phase 45 measured, and pinned on both sides, that `sg merge` had no rename
detection: a history real git calls `CONFLICT (rename/rename)` merged cleanly
under sg, keeping both renamed files. That was the top item on the "what
actually blocks real use" list. This phase closes it.

### 1. The oracle, measured before any code was written

Every row below is from git 2.55.0 under `LC_ALL=C` with
`-c advice.statusHints=true -c core.quotepath=false`, one throwaway repo per
scenario, `master` = ours and `theirs` = the merged branch. Nothing here was
recalled; the scenario scripts are reproducible.

**The threshold is exactly `git diff`'s default 50%, and the comparison is
`score >= 50%`.** Measured by bisecting how much of a 100-line file ours
rewrites while renaming it, with theirs editing a line ours left alone:

| lines rewritten | `git diff -M --name-status` | merge rc | outcome |
|---|---|---|---|
| 0 | `R100` | 0 | clean, edit follows the rename |
| 10 / 20 / 30 / 40 / 45 | `R089` … `R053` | 0 | clean |
| **48** | **`R050`** | **0** | clean |
| **49** | **`D` + `A`** | **1** | `CONFLICT (modify/delete)` |
| 50 … 90 | `D` + `A` | 1 | `CONFLICT (modify/delete)` |

Both tools flip at the same K, so sg reuses `SG_SIMILARITY_DEFAULT` (30000 on
git's 0..60000 scale) rather than introducing a second constant.

**The surviving path is always the non-base name, and the stages move with
it.** For a one-sided rename whose content also conflicts, all three stages
sit at the NEW path -- stage 1 is *not* left at the old name:

```
$ git ls-files -s          # ours renamed a.txt -> b.txt, both edited line 5
100644 4083766... 1  b.txt   (base blob)
100644 f4cde8c... 2  b.txt   (ours)
100644 3c00e2d... 3  b.txt   (theirs)
```

Measured identically with the rename on theirs' side instead; only
`git status`'s extra `D  a.txt` row differs, and that is not a fourth index
entry -- `a.txt` is simply absent from the index while HEAD still has it.

**The one exception is rename/rename to two different names**, where the three
stages live at three different paths (this is the shape Phase 45 built with
real git to give `both deleted:` / `added by us:` / `added by them:` an
oracle):

```
$ git status --porcelain=v1     $ git ls-files -s
DD a.txt                        100644 <base> 1  a.txt
AU b.txt                        100644 <M>    2  b.txt
UA c.txt                        100644 <M>    3  c.txt
```

Note `<M>`: **stage 2 and stage 3 hold the same blob**, and it is the result of
a real three-way content merge of (base, ours@b, theirs@c) -- when that merge
conflicts, the stored blob *contains the conflict markers*. Both working-tree
files get those same bytes. The old name has a stage-1 index entry and **no
file in the working tree at all**, which is the one thing
`sg_merge_result_entry` could not express before this phase.

**Both sides renaming to the SAME name is not a rename/rename conflict at
all** -- it collapses to an ordinary content merge at that path, with stage 1
being the base blob relocated to the new name.

**rename/delete leaves no conflict markers**: the renamed side's content sits
at the new path verbatim, and the index carries stages 1+2 (ours renamed,
`UD`) or 1+3 (theirs renamed, `DU`).

**A rename destination that collides with an unrelated add degrades to
add/add with no stage 1 at all** -- so does rename/rename 2to1 (two different
sources renamed onto one name). This is a different index shape from a content
conflict, not a variant of it, and it cannot be assembled by the same path.

**The `:<path>` marker suffix is conditioned on the two sides' own paths
differing from each other, not on "a rename happened."** Both sides renaming
to the same name gets no suffix; when the suffix is present both sides carry
one, each naming its own path (`<<<<<<< HEAD:b.txt` … `>>>>>>> theirs:a.txt`,
measured in both directions).

**The marker run is not always 7.** git widens it to 8 for rename/rename 1to2
and for a rename whose destination collides; when those nest, the outer
add/add stays 7 and the inner rename merge is 8, and it is the *inner*
conflicted text that gets hashed into stage 2.

### 2. What was deliberately left out

- **Directory rename detection.** Measured: git's default is
  `merge.directoryRenames=conflict`, which moves a file added under a
  directory the other side renamed *and* reports
  `CONFLICT (file location)`. `merge.directoryRenames=false` reproduces plain
  path alignment exactly -- so sg's answer is byte-compatible with one
  documented config value, and the divergence is exactly that wide.
- **`-X find-renames=<n>` / `-X no-renames` / `merge.renames` /
  `merge.renameLimit`.** sg reads no config and `sg merge` has no `-X`.
  Measured for the record: `-X find-renames=<n>` and `-X rename-threshold=<n>`
  are aliases and move the threshold in both directions;
  `merge.renames` defaults to `diff.renames`; `renameLimit` gates only the
  inexact pass (a 100% match is settled by the exact-id pass and ignores it);
  and **`git merge --no-renames` does not exist** -- it exits 129, the
  spelling is `-X no-renames`.
- **git's stdout wording.** sg's merge messages were already a different
  vocabulary entirely (`Merge made by '<x>' [<sha>] into '<y>'.`, and a
  conflict list with `sg add` instructions), so `CONFLICT (rename/rename): ...`
  and `Auto-merging <path>` are not reproduced. The dimensions this phase
  holds to real git are exit code, working-tree bytes, and index stage layout.

### 3. What was built

Detection reuses the existing machinery unchanged: `sg_diff_trees` is run
twice (base vs ours, base vs theirs) and each list is handed to
`sg_diff_detect_renames`. That works because a tree-vs-tree list has only
`SG_DIFF_SIDE_BLOB` sides, and `sg_diff_side_read`'s BLOB branch never
touches `repo_root` -- so no working directory or index is needed, which is
what makes the detector usable from inside `sg_merge_trees` at all.

What is new is the layer that consumes those two maps. `sg_merge_trees`'s
union walk is keyed on the path string, so it cannot express "the same file
under two names"; every base path that is a rename source, and every
ours/theirs path that is a rename destination, is therefore lifted out of
the walk (`path_is_rename_consumed`) and resolved by a two-stage pass:
stage one classifies each source's landing on each side, stage two resolves
each destination -- because a collision needs both sides classified before
either can be answered.

Three API changes, all mandatory parameters with no default, the idiom this
codebase already uses for `sg_workdir_missing` and
`sg_status_diff_staged`'s `rename_score`:

- `sg_merge_content` takes a `marker_size` (7, or 8 for the two shapes git
  widens).
- `sg_merge_trees` takes a `rename_score`; 0 reproduces pre-Phase-49
  behaviour byte for byte, and all four call sites -- merge, rebase, and
  stash's two -- pass `SG_SIMILARITY_DEFAULT`, because real git is
  rename-aware in all three commands.
- `sg_merge_result_entry` gains `conflict_no_workdir_file`, the one shape
  the struct could not previously express: rename/rename-1to2's stage-1-only
  entry at the original path, which git leaves with an index stage and no
  file.

The `:<path>` marker suffix needed no signature change -- `sg_merge_content`
takes its labels verbatim, so the caller composes `"<label>:<that side's
path>"`.

### 4. Three bugs every gate was green for

All of `make`, `make test`, `bash tests/interop.sh` and `make sanitize`
passed on code carrying all three of these. Two came out of a cold read plus
the phase's own fuzzer, one out of measuring an answer the spec had left to
the implementer's judgement.

**A clean rename merge did not write the file.** When THEIRS did the
renaming and ours left the source alone -- the commonest real-world shape,
and the one direction none of the unit tests exercised -- the entry at the
destination was filled with ours' blob *at the source path*.
`sg_merge_entry_touches_ours` reads exactly those fields to decide whether
`sg_merge_result_apply` may skip a write on the grounds that ours already
has this content here, so it answered yes for a path ours did not have at
all, and the renamed-to file was never created. The merge COMMIT's tree was
correct throughout, which is why nothing else noticed: only a working-tree
assertion can see it.

**The consumed rename source was never deleted.** Nothing emitted an entry
for a rename source that the other side still had, so no `remove()` ever
ran and the old name stayed behind as an untracked leftover. This is the
same need `conflict_no_workdir_file` was built for in the 1to2 shape,
un-generalized to the other four.

The two halves are independent -- fixing either alone leaves the other
broken -- so they carry separate assertions and separate mutations.

**rename/rename-1to2's stages named objects that did not exist.**
`merge_blob_content` deliberately leaves `*out_sha1` untouched on a
conflict, since an ordinary conflict has no resolved blob and no other
caller wanted one. This shape does: measured, git stores the marker-laden
merge result as a real blob and puts it at BOTH stage 2 and stage 3. The
uninitialized stack array was being copied into both stages instead, i.e. a
corrupt index. Interop's own phase45 fixture cannot see it, because its
renames are pure and the inner merge takes the CLEAN branch, which does fill
the sha1 -- the same "the fixture only reaches the working half" shape as
the mode bug below.

**And the mode.** The spec left "what mode do the two 1to2 stages carry" to
the implementer, who chose ours' own mode and flagged it as the highest-risk
guess in the hand-off. Measured in both directions, it is the ordinary
merged mode: base 644 / ours 755 / theirs 644 gives 100755 at both stages,
and so does base 644 / ours 644 / theirs 755. Taking ours' mode passes the
first fixture and fails the second, so the unit test runs the pair -- a
single-direction fixture would have called the wrong rule green.

### 5. What the fuzzer measured

`tests/fuzz_merge_rename.py` exists because `tests/fuzz_merge.py` is
STRUCTURALLY blind here: its `build_repo` only ever writes one fixed
filename, so no number of rounds can produce a rename. It builds one history,
runs the merge once with sg and once with real git, and compares the exit
code, the working tree, and the index stage layout -- the last read out of
BOTH repositories with real `git ls-files -s`, since sg's index is
format-compatible. Blob ids are never compared (a conflicted blob embeds the
ours label, which diverges by design); the blobs' CONTENT is, after one
narrow normalization of the ours label that deliberately preserves both the
`:path` suffix and the marker width, since those are dimensions under test.

Measured progression on the same 150 rounds, seed 1: **22 mismatches** for
the first implementation, **9** after the two working-tree fixes, **3** after
the stage-blob fix, and **0** once the remaining three were attributed. Three
independent seed ranges (1, 5000, 9000) x 150 rounds now give 0, 0 and 1 --
the single remaining one being the pre-existing case below.

Two attribution results are worth keeping, because both look like coverage
and are not:

- **The directory-rename divergence is removed from the comparison by
  declaring the oracle's environment, not by labelling it afterwards.** sg
  deliberately does not implement directory rename detection and its answer
  is byte-compatible with `merge.directoryRenames=false`, so the git side
  runs with that knob pinned on the command line. The first version instead
  classified such rounds from git's own `CONFLICT (file location)` message
  and skipped them -- which discards the WHOLE round, so a genuine rename bug
  occurring in the same round would have been discarded with it. That
  classifier is kept as a tripwire and should now always report 0.
- **rename/rename-2to1 is a control, not a discriminator.** When two
  different sources are renamed onto one name, the destination has no
  counterpart in base either way, so git resolves it as an ordinary add/add
  whether or not it noticed the renames, landing on stage 2/3 blobs
  byte-identical to what a rename-blind tool produces. Its 0-out-of-N is the
  correct answer, not a gap; the shape is kept precisely so that stays
  written down.

The one remaining mismatch (seed 9058) is **not a rename bug and not new**:
it is `filler1.txt`, a file no rename touches, where git merges two adjacent
conflicts across a two-line gap that contains a one-sided deletion and sg
keeps them separate. Reproduced on `master` with the same seed, so it
predates this phase; `fuzz_merge.py`'s single-file generator cannot build the
shape (it needs a deletion inside a short gap between two conflicts), which
is why 200 x 2 rounds of it stay at 0. Recorded here as the next thing to
attribute properly, in the same spirit as the `--histogram` residual.
