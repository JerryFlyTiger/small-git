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

def gen_base(rng, newline_edits=True):
    """A base file whose lines are individually distinguishable.

    BASE ITSELF sometimes lacks a trailing newline, and that is not decoration.
    Until it did, this generator could not produce the one shape where a SYNC
    ANCHOR carries has_nl == 0: an anchor is always a base line, and
    sg_diff_split_lines only clears has_nl on a file's own last line, so with
    every base line newline-terminated the anchor path was unreachable no
    matter how many rounds ran.  The first cut of Phase 41's region list
    invented a trailing newline in exactly that shape -- including on a merge
    that resolved to "unchanged", i.e. rewriting a file it had not merged --
    and this fuzzer reported 0/200 straight through it.  A generator that
    cannot build a shape gives zero evidence about it, however green it is.

    HALF THE ROUNDS ARE INDENTED, for the same reason and found the same way.
    sg's merge asks sg_diff_build_script NOT to apply git's indentation
    heuristic, because git's own ll_merge does not set XDF_INDENT_HEURISTIC
    (only `git diff` turns it on by default).  That argument had no witness:
    every line this generator produced started at column 0, and the heuristic
    only ever moves a slidable group when the candidate positions DIFFER in
    indentation -- so passing 1 instead of 0 was measured at 0/200, exactly
    like passing 0.  A parameter whose two values are indistinguishable is not
    a verified choice, it is an unasked question.  With blocks and blank lines
    in the mix the two values separate.

    Flat rounds are kept, not replaced: they are the shape every earlier
    measurement in this phase was made on, and the blank line / indented body
    mix does not subsume them.
    """
    n = rng.randint(1, 40)
    if rng.random() < 0.5:
        lines = ["base%02d\n" % i for i in range(n)]
    else:
        lines = []
        depth = 0
        for i in range(n):
            roll = rng.random()
            if roll < 0.18:
                lines.append("block%02d:\n" % i)
                depth = 1
            elif roll < 0.28:
                lines.append("\n")
            elif roll < 0.38 and depth < 2:
                lines.append("%sinner%02d:\n" % ("    " * depth, i))
                depth += 1
            else:
                lines.append("%sbase%02d\n" % ("    " * depth, i))
    if newline_edits and rng.random() < 0.15:
        lines[-1] = lines[-1].rstrip("\n")
    return lines


def indent_of(line):
    """The leading whitespace of a line, so an inserted run can be written at
    the depth its neighbour sits at.  An insertion that always starts at
    column 0 cannot make the indentation heuristic prefer one slide position
    over another, which would leave the widening above half-done."""
    return line[:len(line) - len(line.lstrip(" "))]


def mutate(rng, lines, tag, newline_edits=True):
    """Apply a few random edits, the kinds a real diff has to align around."""
    out = list(lines)
    for _ in range(rng.randint(1, 4)):
        if not out:
            out.append("%s-new0\n" % tag)
            continue
        op = rng.choice(("replace", "insert", "delete", "runreplace", "dupblock"))
        i = rng.randrange(len(out))
        if op == "replace":
            out[i] = "%s%s-r%d\n" % (indent_of(out[i]), tag, i)
        elif op == "insert":
            pad = indent_of(out[i])
            for k in range(rng.randint(1, 3)):
                out.insert(i, "%s%s-i%d-%d\n" % (pad, tag, i, k))
        elif op == "delete":
            del out[i:i + rng.randint(1, 3)]
        elif op == "runreplace":
            end = min(len(out), i + rng.randint(2, 4))
            out[i:end] = ["%s%s-run%d\n" % (indent_of(out[i]), tag, i)]
        else:
            # Duplicate a run of EXISTING lines in place. This is the only op
            # that produces a SLIDABLE group: a pure insertion whose first
            # line equals the line following the group can sit at either
            # position without changing what the diff means, and choosing
            # between those positions is the entire job of group compaction
            # and of git's indentation heuristic on top of it.
            #
            # Without it the generator could not tell the two values of
            # sg_diff_build_script's indent_heuristic argument apart, however
            # indented the text was -- every inserted line carried a unique
            # tag, so no group was ever slidable and the heuristic had nothing
            # to choose between. Measured: with indentation but no duplicate
            # blocks, passing 1 instead of 0 was still 0/200.
            end = min(len(out), i + rng.randint(2, 4))
            out[i:i] = list(out[i:end])
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
    base = gen_base(rng, newline_edits)
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

        # Classified from the exit status alone, before anything downstream
        # reads the merged file. By this project's own convention (CLAUDE.md,
        # "Code conventions") sg's exit code is only ever 0 or 1, so anything
        # else -- a signal death, an abort, a failed exec -- is not an answer
        # sg can give. Without this, `sg_conflict = rc != 0` reads a crash as
        # "sg says conflict" and lets it wear the `rc` category's clothes:
        # agreeing silently with a conflicting git on a file sg never wrote,
        # or reporting as an ordinary semantic mismatch against a clean one.
        # The ordering is deliberate -- the exit status is unambiguous
        # evidence, while everything read below is a downstream ARTIFACT of
        # the crash, and the `f.txt missing` check in particular would file
        # the round as "measures nothing" instead of naming the crash. (In
        # this harness f.txt is a fixed filename no round renames or deletes,
        # so that shadowing is theoretical here; in fuzz_merge_rename.py,
        # where paths do move, it is not.)
        #
        # git's side is NOT called a crash: git legitimately exits 128 on its
        # own fatal errors, which says the ORACLE failed to answer, i.e. the
        # round measures nothing -- that is what `setup` already means here.
        if sg_res.returncode not in (0, 1):
            return "crash", ("sg exited outside {0,1}: sg rc=%d, git rc=%d"
                             % (sg_res.returncode, git_res.returncode)), None
        if git_res.returncode not in (0, 1):
            return "setup", ("git exited outside {0,1} (rc=%d): the oracle "
                             "did not answer" % git_res.returncode), None

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


# ------------------------------------------------------------------- attribute

MARK = re.compile(r"^(<<<<<<< |=======$|>>>>>>> )")


def merge_file(case_dir, algo=None):
    """Runs `git merge-file` on a saved case, optionally forcing an algorithm.

    This is git's own three-way merge of the same three buffers, reachable
    without a repository, and it is the second oracle the attribution below
    turns on. Labels are normalized the same way the harness does.
    """
    cmd = ["git", "merge-file"]
    if algo:
        cmd.append("--diff-algorithm=" + algo)
    # "topic" is the branch name the harness's own repos use, so the closing
    # marker matches without a second normalization; the opening one is
    # normalized away for both sides anyway.
    cmd += ["-L", "LABEL", "-L", "base", "-L", "topic", "-p",
            os.path.join(case_dir, "ours.txt"), os.path.join(case_dir, "base.txt"),
            os.path.join(case_dir, "theirs.txt")]
    try:
        return subprocess.run(cmd, capture_output=True, env=ENV, text=True).stdout
    except OSError:
        return None


def two_way_matches_git(case_dir):
    """Does sg align base-vs-ours and base-vs-theirs exactly as git does?

    sg's merge is two layers -- two 2-way alignments (a port of git's Myers
    algorithm since Phase 41) and a three-way classification on top that is
    sg's OWN design -- and a merged-output mismatch could come from either.
    The probe builds a one-file repo with sg, commits base, writes the other
    side into the working tree, and asks BOTH tools for the diff of the same
    repo, which works because sg's on-disk format is git-readable.

    Returns True (alignment agrees), False (it does not), or None if the probe
    itself could not run, which is deliberately NOT folded into either answer.

    Limit, stated rather than papered over: the probe goes through each tool's
    DIFF path, where both sides apply the indentation heuristic, while merge
    asks for it to be off -- so a divergence appearing only with the heuristic
    off would not be caught here (test_merge_does_not_use_the_indent_heuristic
    in tests/test_merge_content.c is that argument's witness instead). It was
    checked to have discriminating power rather than assumed to: desyncing sg
    diff's own aligner from git's flips saved cases to [align], measured.
    """
    try:
        base = open(os.path.join(case_dir, "base.txt"), "rb").read()
    except OSError:
        return None
    for side in ("ours.txt", "theirs.txt"):
        repo = tempfile.mkdtemp(prefix="sg_fuzz_2way_")
        try:
            if sg(repo, "init", ".").returncode != 0:
                return None
            with open(os.path.join(repo, "f.txt"), "wb") as fh:
                fh.write(base)
            sg(repo, "add", "f.txt")
            sg(repo, "commit", "-m", "base")
            with open(os.path.join(repo, "f.txt"), "wb") as fh:
                fh.write(open(os.path.join(case_dir, side), "rb").read())
            if sg(repo, "diff").stdout != git(repo, "-c", "core.quotepath=false",
                                             "diff").stdout:
                return False
        finally:
            shutil.rmtree(repo, ignore_errors=True)
    return True


def attribute(root):
    """Bucket saved failures by cause.

    A diff-of-diffs would read "the same lines arranged differently" as
    "different content", and that is exactly the distinction this phase turns
    on, so the first split is by MULTISET of non-marker lines: the same
    multiset means the two tools chose different boundaries for the same
    material, a different multiset means content was lost or invented, which
    no re-alignment excuses.

    Every case is ALSO asked the one question that separates the two layers
    sg's merge is made of: are the two 2-way diffs (base vs ours, base vs
    theirs) byte-identical to real git's?  If they are, the alignment agreed
    and the divergence is in sg's own sync-point three-way classification --
    which is not a port of git's xdl_merge and is not claimed to be.  That
    split is what keeps a real alignment regression from hiding inside the
    known model gap; the two must never be counted together.

    Run this again after any change to merge.c: the counts on their own
    cannot say whether a change moved the right bucket.
    """
    import collections

    def read(path):
        with open(path) as fh:
            return fh.read().splitlines(True)

    buckets = collections.Counter()
    total = 0
    three_way = 0
    algo_only = 0
    for name in sorted(os.listdir(root)):
        d = os.path.join(root, name)
        if not os.path.isdir(d):
            continue
        total += 1
        kind = name.rsplit("_", 1)[1]
        sgl, gil = read(os.path.join(d, "sg_result.txt")), read(os.path.join(d, "git_result.txt"))
        sides = [read(os.path.join(d, n)) for n in ("base.txt", "ours.txt", "theirs.txt")]
        no_nl = any(side and not side[-1].endswith("\n") for side in sides)
        nonmark = lambda ls: [l for l in ls if not MARK.match(l)]
        same_material = collections.Counter(nonmark(sgl)) == collections.Counter(nonmark(gil))
        sg_marks = sum(1 for l in sgl if MARK.match(l))
        gi_marks = sum(1 for l in gil if MARK.match(l))
        sg_raw = normalize("".join(sgl))
        gi_raw = normalize("".join(gil))
        myers = merge_file(d)
        hist = merge_file(d, "histogram")
        if myers is not None and sg_raw == normalize(myers):
            # sg reproduces git's OWN three-way merge of the same buffers, so
            # neither sg's alignment nor its three-way layer is responsible.
            if hist is not None and gi_raw == normalize(hist):
                layer = "algo"
            else:
                layer = "algo?"
            algo_only += 1
        elif two_way_matches_git(d):
            layer = "3way"
            three_way += 1
        else:
            layer = "align"
        if kind == "rc":
            b = "rc/has_nl" if no_nl else "rc/other"
        elif same_material and sg_marks == gi_marks:
            b = "body/same-material-same-marker-count"
        elif same_material:
            b = "body/same-material-different-marker-count"
        else:
            b = "body/material-differs (has_nl)" if no_nl else "body/material-differs"
        buckets["%s  [%s]" % (b, layer)] += 1

    print("=== attribution over %d saved cases ===" % total)
    for b, n in buckets.most_common():
        print("  %-42s %3d" % (b, n))
    nl = sum(n for b, n in buckets.items() if "has_nl" in b)
    print()
    print("  involving a missing trailing newline: %d / %d" % (nl, total))
    print("  [algo]  = sg reproduces `git merge-file` (Myers) EXACTLY, and `git")
    print("            merge` matches --diff-algorithm=histogram: the divergence")
    print("            is git's merge defaulting to histogram, not sg:  %d / %d"
          % (algo_only, total))
    print("  [3way]  = both 2-way diffs match git yet the merge differs from git's")
    print("            own Myers merge -- sg's sync-point layer:  %d / %d" % (three_way, total))
    print("  [align] = a 2-way diff differs from git's -- an ALIGNMENT regression,")
    print("            which is never expected and never part of the known residual")
    return 0


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
    ap.add_argument("--attribute", metavar="DIR",
                    help="do not fuzz; bucket the failures already saved in "
                         "DIR by cause and exit (see attribute()'s docstring "
                         "for why the split is by multiset, not by diff)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if args.attribute:
        return attribute(args.attribute)

    if not os.path.exists(SG):
        print("fuzz_merge: %s not found -- run make first" % SG)
        return 2
    if shutil.which("git") is None:
        print("fuzz_merge: git not found, nothing to compare against")
        return 2
    if args.keep:
        os.makedirs(args.keep, exist_ok=True)

    counts = {"rc": 0, "label": 0, "body": 0, "setup": 0, "crash": 0}
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
    # A crash is exit status outside {0,1} on either side -- not an answer sg
    # (or git) can give by this project's own convention, so it is not an
    # algorithmic divergence and must not be read as one; it still fails the
    # run (counted into `total` below) because a crash is not acceptable.
    print("  crash rounds:     %d  (exit status outside {0,1} -- not an "
          "answer sg can give; rerun the same seed range)" % counts["crash"])
    if counts["setup"]:
        print("  setup failures:   %d  (these measure nothing -- fix first)"
              % counts["setup"])
    print("  total:            %d / %d rounds" % (total, args.rounds))
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
