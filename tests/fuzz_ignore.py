#!/usr/bin/env python3
"""Randomized .gitignore conformance fuzzer, with real git as the oracle.

Generates random pattern sets and directory trees from a grammar covering
git's ignore features, then asserts that the set of untracked paths `sg
status` reports is EXACTLY the set `git status --porcelain -uall` reports.
Any divergence is a bug in sg's matcher.

This exists because hand-written cases only cover the combinations someone
thought of. The interop suite's conformance sweep pins the rule families
individually; this pins their *interactions* — a negation inside a nested
.gitignore under a directory excluded by a `**` pattern three levels up, and
so on. It found zero divergences over 600 iterations when the matcher landed,
which is the evidence behind the claim that sg's ignore semantics match git's.

Not part of `make test` or tests/interop.sh: it needs python3 and a real git,
takes a while, and is a verification tool rather than a gate. Run it by hand
after touching src/workdir/ignore.c or either walker:

    make && python3 tests/fuzz_ignore.py          # 200 iterations
    python3 tests/fuzz_ignore.py 1000             # longer soak

Exits non-zero and keeps the offending repository (printing its path and the
.gitignore files involved) on the first few mismatches.
"""
import os
import random
import shutil
import subprocess
import sys
import tempfile

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SG = os.path.join(PROJECT_ROOT, "build", "sg")

NAMES = ["a", "b", "foo", "bar", "x.txt", "y.log", "z.o", "n1", "keep.log",
         "build", "src", "doc", "sub", "deep", "tmp", "a.b.c", "-dash",
         "up.TXT", "q.txt"]
DIRS = ["src", "build", "doc", "sub", "deep", "tmp", "a", "nested"]


def rand_pattern(rng):
    """One .gitignore line, drawn from a grammar covering git's feature set."""
    kind = rng.randrange(16)
    base = rng.choice(NAMES)
    d = rng.choice(DIRS)
    if kind == 0:
        return base                                    # basename, any depth
    if kind == 1:
        return "/" + base                              # anchored to this dir
    if kind == 2:
        return d + "/"                                 # directory-only
    if kind == 3:
        return d + "/" + base                          # slash => anchored
    if kind == 4:
        return "*" + rng.choice([".txt", ".log", ".o"])
    if kind == 5:
        return d + "/*" + rng.choice([".txt", ".log"])  # * must not cross /
    if kind == 6:
        return "**/" + base                            # leading globstar
    if kind == 7:
        return d + "/**"                               # trailing globstar
    if kind == 8:
        return d + "/**/" + base                       # middle, incl. zero depth
    if kind == 9:
        return "!" + base                              # negation
    if kind == 10:
        return "!" + rng.choice(["*.txt", "*.log", d + "/" + base])
    if kind == 11:
        return "[a-c]" + rng.choice([".txt", ""])       # character class
    if kind == 12:
        return "[!a-c]" + rng.choice([".log", ""])      # negated class
    if kind == 13:
        return base[0] + "?" + base[1:] if len(base) > 1 else base + "?"
    if kind == 14:
        return "a**b"                                  # ** not a whole segment
    return rng.choice(["*.o", "tmp", "**/deep/**", "/build/", "!keep.log"])


def build_tree(path, rng):
    """A random nested tree, with .gitignore files scattered at several depths."""
    depth_dirs = [""]
    for _ in range(rng.randrange(2, 6)):
        parent = rng.choice(depth_dirs)
        d = os.path.join(parent, rng.choice(DIRS))
        os.makedirs(os.path.join(path, d), exist_ok=True)
        depth_dirs.append(d)
    for _ in range(rng.randrange(6, 20)):
        parent = rng.choice(depth_dirs)
        full = os.path.join(path, parent, rng.choice(NAMES))
        if os.path.isdir(full):
            continue
        os.makedirs(os.path.dirname(full) or path, exist_ok=True)
        with open(full, "w") as fh:
            fh.write("x\n")
    picks = rng.sample(depth_dirs, min(len(depth_dirs), rng.randrange(1, 4)))
    for parent in set(picks):
        lines = [rand_pattern(rng) for _ in range(rng.randrange(1, 6))]
        with open(os.path.join(path, parent, ".gitignore"), "w") as fh:
            fh.write("\n".join(lines) + "\n")


def git_untracked(repo):
    out = subprocess.run(["git", "status", "--porcelain", "-uall", "-z"],
                         cwd=repo, capture_output=True).stdout
    return {rec[3:].decode("utf-8", "surrogateescape")
            for rec in out.split(b"\0") if rec.startswith(b"?? ")}


def sg_untracked(repo):
    out = subprocess.run([SG, "status"], cwd=repo, capture_output=True)
    if out.returncode != 0:
        raise RuntimeError("sg status failed (rc=%d): %s"
                           % (out.returncode, out.stderr.decode()))
    res, inside = set(), False
    for line in out.stdout.decode("utf-8", "surrogateescape").split("\n"):
        if line.startswith("Untracked files:"):
            inside = True
        elif inside:
            if line.startswith("  (use"):
                continue
            if line.startswith("\t"):
                res.add(line[1:])
            else:
                break
    return res


def dump_ignores(repo):
    for root, dirs, files in os.walk(repo):
        dirs[:] = [d for d in dirs if d != ".git"]
        if ".gitignore" in files:
            rel = os.path.relpath(os.path.join(root, ".gitignore"), repo)
            print("  --- %s:" % rel)
            with open(os.path.join(root, ".gitignore")) as fh:
                for ln in fh:
                    print("      %s" % ln.rstrip())


def main():
    if not os.path.exists(SG):
        print("error: %s not built; run `make` first" % SG, file=sys.stderr)
        return 2
    iterations = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    root = tempfile.mkdtemp(prefix="sg_fuzz_ignore_")
    failures = 0
    i = -1
    for i in range(iterations):
        rng = random.Random(i)
        repo = os.path.join(root, "r%d" % i)
        os.makedirs(repo)
        subprocess.run(["git", "init", "-q", repo], check=True)
        # macOS git defaults core.ignorecase to true; sg is case-sensitive, so
        # pin the oracle to the same semantics rather than comparing apples to
        # oranges on one platform and not the other.
        subprocess.run(["git", "config", "core.ignorecase", "false"],
                       cwd=repo, check=True)
        build_tree(repo, rng)
        g, s = git_untracked(repo), sg_untracked(repo)
        if g == s:
            shutil.rmtree(repo, ignore_errors=True)
            continue
        failures += 1
        print("=== MISMATCH seed %d (repo kept at %s)" % (i, repo))
        print("  git saw, sg did not:", sorted(g - s)[:10])
        print("  sg saw, git did not:", sorted(s - g)[:10])
        dump_ignores(repo)
        if failures >= 3:
            print("stopping after 3 mismatches")
            break
    print("\nfuzz_ignore: %d iterations, %d mismatches" % (i + 1, failures))
    if not failures:
        shutil.rmtree(root, ignore_errors=True)
    return 1 if failures else 0


sys.exit(main())
