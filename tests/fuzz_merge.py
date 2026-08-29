#!/usr/bin/env python3
"""Differential fuzzer for sg's three-way merge, with real git as the oracle.

Phase 41.  This is the project's LAST alignment path with no differential
coverage: Phase 35 swapped 2-way patch bodies onto a port of git's Myers
algorithm and deliberately left `src/workdir/merge.c` alone, because with no
fuzzer here there was nothing to observe a behaviour change under.  Merge's
failure mode is also the worst one in the project -- a wrong alignment writes
conflict markers into the user's files, so the error lands on disk as data,
not merely as odd-looking output.

WHAT IS COMPARED, AND WHAT IS NOT
---------------------------------
Each round builds one repository with sg, copies it, and then merges the SAME
two commits with `sg merge` in one copy and `git merge` in the other.  Two
independently built repositories would carry different commit ids; sharing
the history keeps every id identical so the comparison can be byte-for-byte.

The expectation comes only from git.  Nothing here is derived from sg's own
output -- a differential harness that borrows its expectation from the thing
under test can only ever prove the tool agrees with itself.

Exactly ONE normalization is applied, to both sides equally: the label on the
`<<<<<<< ` line is replaced with a fixed token.  sg names the current branch
where git always writes HEAD, and that is a deliberate, documented divergence
(CLAUDE.md's deliberate-divergence list, entry 5) pinned independently by
tests/interop.sh's phase41 group.  Normalizing it here is not sweeping it
under the rug: without it every conflicting round would mismatch on that line
and drown out the alignment signal this fuzzer exists to measure.  To keep
the normalization honest the harness also CHECKS the label it erased, and
reports a `label` mismatch if it is not the expected one -- so a regression
in the label cannot hide inside the very step that hides the label.

Mismatches are reported by category, because "N mismatches" alone cannot tell
a real defect from the known limits of this comparison:

  rc      - sg and git disagree about whether the merge conflicted at all
  label   - the erased `<<<<<<< ` label was not the expected one
  body    - the merged bytes differ after normalization

A `body` mismatch is NOT automatically an alignment bug.  sg's three-way
layer (the sync-point classification in merge.c) is its own design, not a
port of git's xdl_merge, so the two can legitimately segment a merge
differently even when both 2-way alignments agree.  Attributing a body
mismatch to alignment versus to that layer takes reading the case, which is
why --keep writes failing cases out instead of only counting them.

USAGE
    python3 tests/fuzz_merge.py [rounds] [--seed N] [--max-failures N]
                                [--keep DIR] [--verbose]

`--max-failures 0` means "never stop early", which is the point: stopping at
the first few failures measures nothing.  A rate is the deliverable, not a
stop.  Round i uses seed (base + i) and the summary prints the base, so any
round can be reproduced exactly.
"""

import argparse
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SG = os.path.join(os.path.dirname(HERE), "build", "sg")

ENV = dict(os.environ)
ENV.update(
    LC_ALL="C",
    LANG="C",
    GIT_AUTHOR_NAME="fuzz",
    GIT_AUTHOR_EMAIL="fuzz@example.invalid",
    GIT_COMMITTER_NAME="fuzz",
    GIT_COMMITTER_EMAIL="fuzz@example.invalid",
)

MARKER_OURS = re.compile(r"^<<<<<<< .*$", re.M)


def run(cwd, *args):
    return subprocess.run(list(args), cwd=cwd, env=ENV,
                          capture_output=True, text=True)


def sg(cwd, *args):
    return run(cwd, SG, *args)


def git(cwd, *args):
    return run(cwd, "git", *args)


# ---------------------------------------------------------------- generation

def gen_base(rng):
    """A base file whose lines are individually distinguishable."""
    n = rng.randint(1, 40)
    return ["base%02d\n" % i for i in range(n)]


def mutate(rng, lines, tag, newline_edits=True):
    """Apply a few random edits, the kinds a real diff has to align around."""
    out = list(lines)
    for _ in range(rng.randint(1, 4)):
        if not out:
            out.append("%s-new0\n" % tag)
            continue
        op = rng.choice(("replace", "insert", "delete", "runreplace"))
        i = rng.randrange(len(out))
        if op == "replace":
            out[i] = "%s-r%d\n" % (tag, i)
        elif op == "insert":
            for k in range(rng.randint(1, 3)):
                out.insert(i, "%s-i%d-%d\n" % (tag, i, k))
        elif op == "delete":
            del out[i:i + rng.randint(1, 3)]
        else:
            end = min(len(out), i + rng.randint(2, 4))
            out[i:end] = ["%s-run%d\n" % (tag, i)]
    # No trailing newline on the last line, sometimes: merge.c has a dedicated
    # special case for it (merge.c:430-447) and Myers treats has_nl as part of
    # line identity, so this axis is exactly where the two can disagree.
    if newline_edits and out and rng.random() < 0.15:
        out[-1] = out[-1].rstrip("\n")
    return out


# ------------------------------------------------------------------ one round

def build_repo(parent, base, ours, theirs):
    """Create <parent>/repo with master/topic diverged from a common base."""
    name = "repo"
    repo = os.path.join(parent, name)
    r = run(parent, SG, "init", name)
    if r.returncode != 0:
        return None, "sg init failed: %s" % r.stderr.strip()

    def write(lines):
        with open(os.path.join(repo, "f.txt"), "w") as fh:
            fh.write("".join(lines))

    write(base)
    sg(repo, "add", "f.txt")
    sg(repo, "commit", "-m", "base")
    sg(repo, "branch", "topic")
    write(ours)
    sg(repo, "add", "f.txt")
    sg(repo, "commit", "-m", "ours")
    sg(repo, "switch", "topic")
    write(theirs)
    sg(repo, "add", "f.txt")
    sg(repo, "commit", "-m", "theirs")
    sg(repo, "switch", "master")
    return repo, None


def read_file(repo, name="f.txt"):
    try:
        with open(os.path.join(repo, name)) as fh:
            return fh.read()
    except (FileNotFoundError, IsADirectoryError):
        return None


def normalize(text):
    return MARKER_OURS.sub("<<<<<<< LABEL", text)


def ours_labels(text):
    return [m.group(0) for m in MARKER_OURS.finditer(text)]


def one_round(rng, keep_dir, index, newline_edits=True):
    base = gen_base(rng)
    ours = mutate(rng, base, "ours", newline_edits)
    theirs = mutate(rng, base, "thrs", newline_edits)

    parent = tempfile.mkdtemp(prefix="sg_fuzz_merge_")
    try:
        repo, err = build_repo(parent, base, ours, theirs)
        if repo is None:
            return "setup", err or "repo build failed", None

        gitcopy = repo + ".gitcopy"
        shutil.copytree(repo, gitcopy)

        sg_res = sg(repo, "merge", "topic")
        git_res = git(gitcopy, "merge", "topic")

        sg_text = read_file(repo)
        git_text = read_file(gitcopy)
        if sg_text is None or git_text is None:
            return "setup", "f.txt missing after merge", None

        sg_conflict = sg_res.returncode != 0
        git_conflict = git_res.returncode != 0

        detail = None
        kind = None
        if sg_conflict != git_conflict:
            kind = "rc"
            detail = ("sg conflict=%s (rc=%d), git conflict=%s (rc=%d)"
                      % (sg_conflict, sg_res.returncode,
                         git_conflict, git_res.returncode))
        else:
            # Check the label BEFORE erasing it, so normalization cannot hide
            # a label regression inside the step that hides labels.
            bad = [l for l in ours_labels(sg_text) if l != "<<<<<<< master"]
            gbad = [l for l in ours_labels(git_text) if l != "<<<<<<< HEAD"]
            if bad or gbad:
                kind = "label"
                detail = "sg unexpected=%r git unexpected=%r" % (bad, gbad)
            elif normalize(sg_text) != normalize(git_text):
                kind = "body"
                detail = "merged bytes differ after normalization"

        if kind and keep_dir:
            dest = os.path.join(keep_dir, "round%05d_%s" % (index, kind))
            os.makedirs(dest, exist_ok=True)
            for nm, body in (("base.txt", "".join(base)),
                             ("ours.txt", "".join(ours)),
                             ("theirs.txt", "".join(theirs)),
                             ("sg_result.txt", sg_text),
                             ("git_result.txt", git_text)):
                with open(os.path.join(dest, nm), "w") as fh:
                    fh.write(body)
            detail = "%s (case saved to %s)" % (detail, dest)

        return kind, detail, (sg_conflict or git_conflict)
    finally:
        shutil.rmtree(parent, ignore_errors=True)


# ------------------------------------------------------------------------ main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rounds", nargs="?", type=int, default=200)
    ap.add_argument("--seed", type=int, default=0,
                    help="seed base; round i uses seed base+i")
    ap.add_argument("--max-failures", type=int, default=0,
                    help="stop after this many mismatches (0 = never stop, "
                         "which is what makes the result a rate)")
    ap.add_argument("--keep", metavar="DIR",
                    help="write each failing case's five files here")
    ap.add_argument("--no-newline-edits", action="store_true",
                    help="never remove a file's trailing newline. This is the "
                         "CONTROL for merge.c's has_nl blindness: with the "
                         "suspected cause switched off, whatever mismatch "
                         "remains is attributable to something else. A "
                         "correlation measured only with the cause switched "
                         "ON is not an attribution.")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(SG):
        print("fuzz_merge: %s not found -- run make first" % SG)
        return 2
    if shutil.which("git") is None:
        print("fuzz_merge: git not found, nothing to compare against")
        return 2
    if args.keep:
        os.makedirs(args.keep, exist_ok=True)

    counts = {"rc": 0, "label": 0, "body": 0, "setup": 0}
    conflicted = 0
    for i in range(args.rounds):
        rng = random.Random(args.seed + i)
        kind, detail, had_conflict = one_round(rng, args.keep, i,
                                               not args.no_newline_edits)
        if had_conflict:
            conflicted += 1
        if kind:
            counts[kind] += 1
            print("fuzz_merge: round %d (seed %d) %s mismatch: %s"
                  % (i, args.seed + i, kind, detail))
        elif args.verbose:
            print("fuzz_merge: round %d ok%s"
                  % (i, " (conflict)" if had_conflict else ""))
        total = sum(counts.values())
        if args.max_failures and total >= args.max_failures:
            print("fuzz_merge: stopping early after %d mismatches "
                  "(--max-failures); this is NOT a rate" % total)
            break

    total = sum(counts.values())
    print()
    print("fuzz_merge: %d rounds from seed %d, %d produced a conflict%s"
          % (args.rounds, args.seed, conflicted,
             "  [--no-newline-edits: trailing-newline cases suppressed]"
             if args.no_newline_edits else ""))
    print("  rc mismatches:    %d" % counts["rc"])
    print("  label mismatches: %d" % counts["label"])
    print("  body mismatches:  %d" % counts["body"])
    if counts["setup"]:
        print("  setup failures:   %d  (these measure nothing -- fix first)"
              % counts["setup"])
    print("  total:            %d / %d rounds" % (total, args.rounds))
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
