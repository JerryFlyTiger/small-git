#!/usr/bin/env python3
"""Randomized differential fuzzer for `sg diff`'s combined (`-c` / `--cc`)
output, with real git as the oracle.

Each round builds a real merge conflict with real git, then perturbs the
working-tree file in ways the hand-picked oracle fixtures (PHASE34_ORACLE.md)
do not cover: multi-line replace groups, deletions at EOF, missing trailing
newlines, funcname-candidate lines near the end of the buffer, partially
resolved hunks, and a rename-eligible companion row. `sg diff` is then run
across every flag combination that changes combined rendering (plain,
`-c`/`--cc` alone and stacked both orders, each of the five non-patch
formats with and without `-c`/`--cc`, and `--cached`), and its output is
compared byte for byte against `git -c core.quotepath=false diff`.

Not part of `make test` or tests/interop.sh: it needs python3 and a real git,
takes a while, and is a verification tool rather than a gate. Per CLAUDE.md,
run it by hand after touching the combined-diff block of
src/cli/diff_out.c (render_combined_patch and everything it calls):

    make && python3 tests/fuzz_combined.py            # 150 iterations
    python3 tests/fuzz_combined.py 500                # longer soak
    python3 tests/fuzz_combined.py 150 --seed 4000     # shift the seed base
    python3 tests/fuzz_combined.py 150 --max-failures 0   # count them all

Baseline (Phase 34, measured 2026-08-28): 150 rounds, 104 produced a real
conflict, 2 mismatched -- both attributed to the project's pre-existing
LCS-vs-Myers 2-way alignment residual (docs/DESIGN.md's Phase 26 note; ~2-3%
of hunks), not to the combined layer, by replaying each parent-vs-result pair
as an ordinary 2-way diff and finding it ALREADY disagrees with git there
(the combined layer inherits, does not introduce, that divergence) -- see
docs/DESIGN.md's Phase 34 section for the attribution method and how to tell
"shifted" from "different content" (a diff-of-diffs must not be used for
that, see CLAUDE.md/the project memory note on this exact trap: compare the
multiset of content lines, not the two patches' diff).

Like fuzz_diff.py, each iteration i uses seed base+i, printed on every
mismatch so a failure reproduces exactly with `--seed <that seed> 1`.
"""
import os
import random
import shutil
import subprocess
import sys
import tempfile

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SG = os.path.join(PROJECT_ROOT, "build", "sg")

ENV = dict(os.environ)
ENV.update({
    "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
    "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t",
    "GIT_AUTHOR_DATE": "2026-01-01T00:00:00Z",
    "GIT_COMMITTER_DATE": "2026-01-01T00:00:00Z",
    "LC_ALL": "C",
    "GIT_CONFIG_GLOBAL": "/dev/null",
    "GIT_CONFIG_SYSTEM": "/dev/null",
})

FLAGS = ["", "-c", "--cc", "-c --cc", "--cc -c",
         "--stat", "--numstat", "--shortstat", "--name-only", "--name-status",
         "-c --stat", "--cc --stat", "-c --numstat", "--cc --numstat",
         "-c --name-status", "--cc --name-status", "-c --name-only",
         "--cc --shortstat", "--cached", "--cached --name-status",
         "-c --cached", "--cc --cached"]


def run(cmd, cwd):
    return subprocess.run(cmd, cwd=cwd, env=ENV, capture_output=True, text=True)


def gen_line(rng, i):
    kind = rng.random()
    if kind < 0.25:                       # funcname candidate (alnum/_/$ first)
        return rng.choice(["int f%d(void) {" % i, "_helper%d = %d;" % (i, i),
                           "$var%d {" % i, "func_%d" % i])
    if kind < 0.35:
        return ""                          # blank line
    if kind < 0.45:
        return "  " + "x" * rng.randint(0, 60)
    return "L%d_%s" % (i, "".join(rng.choice("abcdef") for _ in range(rng.randint(1, 6))))


def edit(rng, lines):
    out = list(lines)
    for _ in range(rng.randint(1, 4)):
        if not out:
            break
        op = rng.random()
        i = rng.randrange(len(out))
        if op < 0.45:                                   # replace a run
            n = min(len(out) - i, rng.randint(1, 3))
            out[i:i + n] = [gen_line(rng, 900 + rng.randrange(50))
                            for _ in range(rng.randint(1, 3))]
        elif op < 0.75:
            out.insert(i, gen_line(rng, 800 + rng.randrange(50)))
        else:
            del out[i:i + rng.randint(1, 2)]
    return out


def write(path, lines, trailing_nl=True):
    body = "\n".join(lines)
    if lines and trailing_nl:
        body += "\n"
    with open(path, "w") as fh:
        fh.write(body)


def one_round(rng, tmp):
    repo = os.path.join(tmp, "r")
    shutil.rmtree(repo, ignore_errors=True)
    os.makedirs(repo)
    run(["git", "init", "-q", "-b", "master", "."], repo)
    run(["git", "config", "user.name", "t"], repo)
    run(["git", "config", "user.email", "t@t"], repo)

    n = rng.randint(3, 25)
    base = [gen_line(rng, i) for i in range(n)]
    write(os.path.join(repo, "f.txt"), base)
    # a second, non-conflicted file so ordering/companion interactions show up
    write(os.path.join(repo, "other.txt"), ["keep", "me"])
    run(["git", "add", "."], repo)
    run(["git", "commit", "-qm", "base"], repo)

    run(["git", "checkout", "-qb", "side"], repo)
    theirs = edit(rng, base)
    write(os.path.join(repo, "f.txt"), theirs, rng.random() < 0.8)
    run(["git", "commit", "-qam", "side"], repo)

    run(["git", "checkout", "-q", "master"], repo)
    ours = edit(rng, base)
    write(os.path.join(repo, "f.txt"), ours, rng.random() < 0.8)
    run(["git", "commit", "-qam", "ours"], repo)

    run(["git", "merge", "side"], repo)
    st = run(["git", "ls-files", "-u", "f.txt"], repo)
    if not st.stdout.strip():
        return None                      # merged cleanly, nothing to compare

    # Perturb the working tree file -- this is what varies the result side.
    p = os.path.join(repo, "f.txt")
    roll = rng.random()
    if roll < 0.15:
        write(p, ours, rng.random() < 0.7)            # resolved to ours
    elif roll < 0.30:
        write(p, theirs, rng.random() < 0.7)          # resolved to theirs
    elif roll < 0.40:
        write(p, [])                                   # emptied
    elif roll < 0.48:
        os.unlink(p)                                   # deleted
    elif roll < 0.62:
        cur = open(p).read().split("\n")
        write(p, cur[:rng.randint(0, len(cur))], rng.random() < 0.5)  # truncated
    elif roll < 0.72:
        cur = open(p).read()
        with open(p, "w") as fh:                       # strip trailing newline
            fh.write(cur.rstrip("\n"))
    elif roll < 0.80:
        # rename-eligible: a new file identical to what the companion row deletes
        write(os.path.join(repo, "moved.txt"), ours)
        run(["git", "add", "moved.txt"], repo)
        os.unlink(p)
    # else: leave the conflict markers as-is

    bad = []
    for fl in FLAGS:
        g = run(["git", "-c", "core.quotepath=false", "diff"] + fl.split(), repo)
        s = run([SG, "diff"] + fl.split(), repo)
        if (g.stdout, g.returncode) != (s.stdout, s.returncode):
            bad.append((fl, g.stdout, s.stdout, g.returncode, s.returncode))
    return bad


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
    rounds = int(argv[0]) if argv else 150

    tmp = tempfile.mkdtemp(prefix="sg_fuzz_cc_")
    conflicts = mismatches = 0
    i = -1
    try:
        for i in range(rounds):
            seed = seed_base + i
            rng = random.Random(seed)
            bad = one_round(rng, tmp)
            if bad is None:
                continue
            conflicts += 1
            if bad:
                mismatches += 1
                print("=== seed %d: %d flag(s) mismatched  (reproduce: "
                      "python3 tests/fuzz_combined.py 1 --seed %d)"
                      % (seed, len(bad), seed))
                if max_failures != 0:
                    for fl, go, so, grc, src in bad[:2]:
                        print("--- flags '%s'  (git rc=%d sg rc=%d)" % (fl, grc, src))
                        print("--- git ---\n%s" % go[:900])
                        print("--- sg  ---\n%s" % so[:900])
                if max_failures and mismatches >= max_failures:
                    print("stopping after %d mismatches" % max_failures)
                    break
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("\nfuzz_combined: %d rounds, %d produced a conflict, %d mismatched"
          % (i + 1, conflicts, mismatches))
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main())
