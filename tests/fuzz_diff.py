#!/usr/bin/env python3
"""Randomized `sg diff` patch-output fuzzer, with real git as the oracle.

Generates a random text file, applies a random set of edits to it, and asserts
that `sg diff` emits the patch **byte for byte** identically to `git diff` on
the same repository.

This exists because the patch body is the one place sg has to reproduce a
*choice*, not just a format. Every other diff output (`--numstat`,
`--name-status`, ...) is determined by the set of changed paths, so a
hand-written fixture pins it completely. The patch body is not: when the same
minimal edit script can be written down in several places -- inserting a
duplicate of a block that already appears, editing one of several identical
lines -- git picks one, and which one it picks comes out of xdiff's group
compaction plus the indent heuristic (`diff.indentHeuristic`, on by default
since git 2.14). A fixture suite can only pin the alignments someone thought
to write down, and the alignments nobody thinks to write down are exactly the
ones where an independent implementation drifts.

So the generator is deliberately biased toward ambiguity: repeated blocks,
varied indentation, blank-line separators, and insertions that duplicate text
already present. On unambiguous input any correct differ agrees with git; the
interesting iterations are the ones where more than one answer is minimal.

Not part of `make test` or tests/interop.sh: it needs python3 and a real git,
takes a while, and is a verification tool rather than a gate. Run it by hand
after touching src/cli/diff_out.c, src/util/diff_lcs.c, or src/workdir/diff.c:

    make && python3 tests/fuzz_diff.py            # 200 iterations
    python3 tests/fuzz_diff.py 1000              # longer soak
    python3 tests/fuzz_diff.py 200 --seed 4000   # shift the seed base
    python3 tests/fuzz_diff.py 200 --max-failures 0   # count them all, print none

By default it stops after 3 mismatches, which is what you want when you are
looking at them. It is not what you want when you are converging: a rate is
the whole measurement, and "stopped at 3" is not a rate. --max-failures 0
runs every iteration and prints only the tally.

Each iteration i uses seed base+i, and the seed is printed on every mismatch,
so a failure reproduces exactly with `--seed <that seed> 1`.

Exits non-zero and keeps the offending repository (printing its path, both
outputs, and the two file versions) on the first few mismatches.
"""
import difflib
import os
import random
import shutil
import subprocess
import sys
import tempfile

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SG = os.path.join(PROJECT_ROOT, "build", "sg")

# The oracle and sg must not disagree for reasons that have nothing to do with
# diffing: a translated summary line, a locale-dependent sort, or git quoting a
# high byte that sg passes through (see CLAUDE.md on core.quotepath).
ENV = dict(os.environ)
ENV.update({
    "GIT_AUTHOR_NAME": "F", "GIT_AUTHOR_EMAIL": "f@example.com",
    "GIT_COMMITTER_NAME": "F", "GIT_COMMITTER_EMAIL": "f@example.com",
    "GIT_AUTHOR_DATE": "2026-01-01T00:00:00Z",
    "GIT_COMMITTER_DATE": "2026-01-01T00:00:00Z",
    "LC_ALL": "C",
})

GIT_DIFF = ["git", "-c", "core.quotepath=false", "diff"]

# Lines are drawn from a small vocabulary on purpose. A large one makes every
# line unique, and unique lines make the alignment unambiguous -- which is the
# one case this fuzzer does not need help finding.
VOCAB = [
    "def handler(request):",
    "class Thing:",
    "_helper = None",
    "$var = 1",
    "    return value",
    "    if flag:",
    "        do_work()",
    "        return None",
    "            deeper()",
    "",
    "# comment",
    "}",
    "{",
    "/* block */",
    "  two_space",
    "\ttab_indented",
    "x = 1",
    "y = 2",
]


def rand_block(rng, n):
    return [rng.choice(VOCAB) for _ in range(n)]


def rand_base(rng):
    """A base file: several blocks, with some blocks repeated verbatim."""
    lines = []
    nblocks = rng.randrange(2, 7)
    blocks = [rand_block(rng, rng.randrange(1, 6)) for _ in range(nblocks)]
    for _ in range(rng.randrange(3, 10)):
        blk = rng.choice(blocks)
        lines.extend(blk)
        if rng.randrange(3) == 0:
            lines.append("")
    return lines


def rand_edits(rng, lines):
    """Apply 1-4 edits. Insertions often duplicate text already in the file,
    which is what makes the placement ambiguous and the heuristic observable."""
    out = list(lines)
    for _ in range(rng.randrange(1, 5)):
        if not out:
            out = rand_block(rng, rng.randrange(1, 4))
            continue
        kind = rng.randrange(5)
        pos = rng.randrange(len(out) + 1)
        if kind == 0:                                    # delete a run
            n = min(rng.randrange(1, 4), len(out) - pos)
            if n > 0:
                del out[pos:pos + n]
        elif kind == 1:                                  # insert fresh lines
            out[pos:pos] = rand_block(rng, rng.randrange(1, 4))
        elif kind == 2:                                  # duplicate a run
            if len(out) >= 2:
                s = rng.randrange(len(out) - 1)
                e = min(s + rng.randrange(1, 5), len(out))
                out[pos:pos] = out[s:e]
        elif kind == 3:                                  # replace a run
            n = min(rng.randrange(1, 4), len(out) - pos)
            out[pos:pos + n] = rand_block(rng, rng.randrange(1, 4))
        else:                                            # re-indent a run
            n = min(rng.randrange(1, 4), len(out) - pos)
            for i in range(pos, pos + n):
                out[i] = rng.choice(["    ", "\t", ""]) + out[i].lstrip()
    return out


def write_file(path, lines, trailing_nl):
    data = "\n".join(lines)
    if lines and trailing_nl:
        data += "\n"
    with open(path, "w") as fh:
        fh.write(data)


def run(cmd, cwd):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, env=ENV)


def sg(args, cwd, check=True):
    r = run([SG] + args, cwd)
    if check and r.returncode != 0:
        raise RuntimeError("sg %s failed (rc=%d): %s"
                           % (" ".join(args), r.returncode,
                              r.stderr.decode("utf-8", "replace")))
    return r


def build_case(repo, rng):
    """Build a repo whose worktree and index differ in the ways that exercise
    every branch of the patch header, not just the modified-file branch."""
    os.makedirs(repo)
    sg(["init", "."], repo)

    base = rand_base(rng)
    base_nl = rng.randrange(4) != 0
    write_file(os.path.join(repo, "f.txt"), base, base_nl)
    # A second file that only ever gets deleted, and a third that is only ever
    # chmod'ed, so `deleted file mode` and the old/new mode pair are reachable.
    write_file(os.path.join(repo, "gone.txt"), rand_base(rng), True)
    write_file(os.path.join(repo, "perm.sh"), rand_base(rng), True)
    os.chmod(os.path.join(repo, "perm.sh"), 0o755)
    sg(["add", "f.txt", "gone.txt", "perm.sh"], repo)
    sg(["commit", "-m", "base"], repo)

    write_file(os.path.join(repo, "f.txt"), rand_edits(rng, base),
               rng.randrange(4) != 0)
    if rng.randrange(2):
        os.unlink(os.path.join(repo, "gone.txt"))
    if rng.randrange(2):
        os.chmod(os.path.join(repo, "perm.sh"), 0o644)
    if rng.randrange(2):
        write_file(os.path.join(repo, "added.txt"), rand_base(rng),
                   rng.randrange(4) != 0)
        sg(["add", "added.txt"], repo)
    return base, base_nl


def main():
    if not os.path.exists(SG):
        print("error: %s not built; run `make` first" % SG, file=sys.stderr)
        return 2

    argv = sys.argv[1:]
    seed_base, max_failures = 0, 3
    for flag, setter in (("--seed", "seed"), ("--max-failures", "maxf")):
        if flag in argv:
            i = argv.index(flag)
            value = int(argv[i + 1])
            if setter == "seed":
                seed_base = value
            else:
                max_failures = value
            del argv[i:i + 2]
    iterations = int(argv[0]) if argv else 200

    root = tempfile.mkdtemp(prefix="sg_fuzz_diff_")
    failures = 0
    i = -1
    for i in range(iterations):
        seed = seed_base + i
        rng = random.Random(seed)
        repo = os.path.join(root, "r%d" % seed)
        build_case(repo, rng)

        mismatch = None
        for label, flags in (("worktree", []), ("--cached", ["--cached"])):
            want = run(GIT_DIFF + flags, repo)
            got = sg(["diff"] + flags, repo, check=False)
            # A non-zero exit from either side is itself a divergence worth
            # reporting: sg's diff exits 0 unless it could not read a blob.
            if want.stdout != got.stdout or got.returncode != 0:
                mismatch = (label, want.stdout, got.stdout, got.stderr,
                            got.returncode)
                break

        if mismatch is None:
            shutil.rmtree(repo, ignore_errors=True)
            continue

        failures += 1
        label, want, got, err, rc = mismatch
        if max_failures == 0:
            # Counting mode: the tally is the measurement, so keep neither the
            # repo nor the output.
            shutil.rmtree(repo, ignore_errors=True)
            continue
        print("=== MISMATCH seed %d (%s) -- repo kept at %s" % (seed, label, repo))
        print("    reproduce: python3 tests/fuzz_diff.py 1 --seed %d" % seed)
        if rc != 0:
            print("    sg exited %d: %s" % (rc, err.decode("utf-8", "replace")))
        want_s = want.decode("utf-8", "surrogateescape").splitlines(keepends=True)
        got_s = got.decode("utf-8", "surrogateescape").splitlines(keepends=True)
        # -git/+sg: every line here is a byte the two implementations disagree
        # on, not a line of the file under test.
        for ln in list(difflib.unified_diff(want_s, got_s, "git", "sg", n=2))[:60]:
            print("    " + ln.rstrip("\n"))
        if failures >= max_failures:
            print("stopping after %d mismatches" % max_failures)
            break

    print("\nfuzz_diff: %d iterations from seed %d, %d mismatches"
          % (i + 1, seed_base, failures))
    if not failures:
        shutil.rmtree(root, ignore_errors=True)
    return 1 if failures else 0


sys.exit(main())
