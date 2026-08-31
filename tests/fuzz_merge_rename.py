#!/usr/bin/env python3
"""Differential fuzzer for three-way merge WHEN RENAMES ARE INVOLVED, with
real git as the oracle.

Phase 49.  tests/fuzz_merge.py is this project's existing merge-content
fuzzer, and it is STRUCTURALLY BLIND to renames: its generator only ever
writes to a single fixed filename (`f.txt`), and never adds, deletes, or
renames a file.  That is not a low-probability gap, it is a zero-probability
one -- no amount of extra rounds against that generator can ever produce a
rename, an add/add collision, or a rename/rename pairing, because the code
that builds the fixture has no path to write one.  This fuzzer exists to
cover exactly that missing dimension.  At the time this is written, sg has
no rename detection in `sg merge` at all (CLAUDE.md's phase45 note: sg merge
resolves a rename/rename history CLEANLY by keeping both renamed files,
because it never notices the rename), so every round in this file where the
FOCUS shape actually involves a rename is expected to disagree with git, and
that expected redness is what proves this fuzzer has discriminating power.

WHAT IS COMPARED, AND WHAT IS NOT
----------------------------------
Each round builds ONE repository with sg (`sg init`, `sg add`, `sg commit`,
`sg branch`, `sg switch`), so the base/ours/theirs commits exist only once
and both tools operate on byte-identical history.  The repo is then copied,
and the SAME merge (`<tool> merge theirs`) is run once with `sg` on the
original and once with real `git` on the copy.  Three independent things are
then compared, all sourced from git's own oracle (never from sg's own
output, straight down to the object graph, since sg's disk format is
git-compatible and real `git` can read it directly per CLAUDE.md):

  1. exit code           -- did the two tools even agree the merge conflicted?
  2. working directory   -- same set of relative paths, and (after the ONE
                             normalization below) the same bytes in each.
  3. index stage layout  -- `git ls-files -s` on BOTH repos (sg's own
                             on-disk index is read directly by real git, no
                             conversion needed), compared entry by entry as
                             (mode, stage, path); for entries present on both
                             sides, the underlying blob is also fetched with
                             `git cat-file blob <id>` and compared byte for
                             byte after normalization.  Blob ids themselves
                             are deliberately never compared: a resolved
                             conflict's content embeds the branch-name label
                             in its marker line, sg's own label is the
                             CURRENT BRANCH NAME where git's is always
                             `HEAD` (CLAUDE.md's deliberate-divergence #5),
                             so the ids would differ on every single
                             conflicting round for a reason that has nothing
                             to do with renames.

THE ONE NORMALIZATION, AND WHY IT STOPS THERE
----------------------------------------------
Exactly the label text on a `<<<<<<<+ ` marker line is rewritten to `HEAD`,
everywhere it appears (working-tree files and index blobs alike).  Two
things are DELIBERATELY preserved rather than swept away, because they are
themselves part of what this fuzzer exists to catch:

  * the marker's LENGTH (7 or 8 `<` characters -- git widens conflict
    markers in some rename/rename and add/add collision shapes) is passed
    through untouched;
  * a trailing `:<path>` suffix (git appends this when the two sides'
    filenames differ at the conflicted location, e.g.
    `<<<<<<< HEAD:b.txt`) is passed through untouched.

Only the branch-name token between the marker and the optional `:path` is
replaced.  git's own theirs branch is deliberately always named `theirs`
here (the same literal string both tools were told to merge), so the
`>>>>>>> ` line never needs normalizing on either side.

SHAPES THE GENERATOR IS BUILT TO HIT, ON PURPOSE
--------------------------------------------------
A fully free-random per-file mutator (the shape fuzz_merge.py's own
generator uses) would give the narrow shapes below -- especially the two
rename/rename pairings and the add/rename collision -- vanishingly small
odds of ever appearing.  So each round instead draws ONE named focus shape
uniformly at random and builds it directly (plus background "filler" files
that are independently, lightly mutated on each side, or left alone, purely
for variety and never load-bearing for the comparison):

  rename_edit             -- ours renames+edits, theirs edits in place;
                              the rename's edit intensity is drawn from a
                              spread of fractions straddling the ~50%
                              similarity line git's own rename detector
                              uses as its default threshold.
  edit_only                -- no renames anywhere (this is the CONTROL: sg
                              and git should already agree here, since
                              nothing here depends on rename detection).
  delete                   -- ours deletes, theirs edits (also a control:
                              modify/delete has nothing to do with renames).
  rename_add_collision      -- ours renames+edits; theirs, independently,
                              ADDS an unrelated new file at ours' rename
                              destination.
  rename_rename_1to2        -- both sides rename the SAME source file to
                              TWO DIFFERENT destination names.
  rename_rename_same_name   -- both sides rename the SAME source file to
                              the SAME destination name, with different
                              content on each side.
  rename_rename_2to1        -- two DIFFERENT source files are each renamed,
                              one by ours and one by theirs, to the SAME
                              destination name.  MEASURED against real git
                              2.55.0 directly (a minimal, hand-built fixture,
                              not a fuzz round): this shape is a SECOND
                              control, not a discriminator -- git treats the
                              destination path as an ordinary add/add
                              conflict regardless of rename similarity,
                              because the destination has no counterpart in
                              base's tree either way, so a rename-aware and
                              a rename-blind merge land on byte-identical
                              stage 2/3 blobs. Kept in the generator (the
                              task explicitly names this shape), and its
                              distribution is still printed, but a 0/N
                              mismatch rate for it is the CORRECT measured
                              answer, not a coverage gap -- same footing as
                              edit_only/delete below.

A per-shape distribution is always printed at the end (not just on request),
because a generator that draws uniformly from a list still has to be
CHECKED to have actually reached every branch -- see CLAUDE.md's own
"fixture generators create shared blind spots" lesson.

USAGE
    python3 tests/fuzz_merge_rename.py [rounds] [--seed N]
                                        [--max-failures N] [--keep DIR]
                                        [--verbose]

`--max-failures 0` (the default) means "never stop early": stopping at the
first few mismatches would measure nothing but where in seed order the
first bad round happens to land.  A rate is the deliverable.  Round i uses
seed (base + i), printed on every mismatch line, so any single round
reproduces exactly with `--seed <that seed> 1`.
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

# Matches a conflict-marker "ours" line: 7-or-8 `<`, a space, a branch-name
# token, and an OPTIONAL ":<path>" suffix -- both the marker width and the
# suffix are captured so they can be put back unchanged (see the module
# docstring's "ONE normalization" section for why neither is erased).
MARKER_OURS_RE = re.compile(r"^(<{7,8} )([^:\n]+)((?::[^\n]*)?)$", re.M)

SHAPES = [
    "rename_edit",
    "edit_only",
    "delete",
    "rename_add_collision",
    "rename_rename_1to2",
    "rename_rename_same_name",
    "rename_rename_2to1",
]

# Deliberately spans both sides of git's default 50% rename-similarity
# threshold, not just the extremes -- see similarity.c's own oracle notes
# in CLAUDE.md for why "off by one point" is a real, not cosmetic, failure
# mode for rename scoring, and this generator wants to walk right past that
# boundary rather than stand only at 0% and 100%.
FRACS = [0.0, 0.1, 0.2, 0.3, 0.4, 0.45, 0.5, 0.55, 0.6, 0.7, 0.85, 1.0]

# Used for the three "does git even NOTICE a rename here" shapes below
# (rename_add_collision, rename_rename_1to2, rename_rename_same_name,
# rename_rename_2to1): git's own rename detector has to clear its default
# ~50% similarity threshold on EACH side before it will call the destination
# collision a rename/rename or rename/add event at all -- if the edit is too
# heavy, git silently falls back to treating both sides as an ordinary,
# independent add, which is EXACTLY what sg already does with no rename
# detection at all, so the round measures nothing (both tools agree by
# accident, not because sg got anything right). Measured directly: with the
# unbiased FRACS above, rename_rename_2to1 mismatched 0/26 rounds, because
# most of its rolls fell in FRACS' upper half and git downgraded to add/add
# on both sides, same as sg. Biased low (still reaching up to 1.0
# occasionally, for variety) so most rounds actually put git's detector
# in play.
PAIRING_FRACS = [0.0, 0.0, 0.05, 0.1, 0.15, 0.2, 0.25, 0.3, 0.4, 0.5, 0.7, 1.0]

PATH_DIRS = ["", "src/", "lib/sub/", "dir1/"]


def run(cwd, *args):
    return subprocess.run(list(args), cwd=cwd, env=ENV,
                          capture_output=True, text=True)


def run_bytes(cwd, *args):
    return subprocess.run(list(args), cwd=cwd, env=ENV, capture_output=True)


def sg(cwd, *args):
    return run(cwd, SG, *args)


def git(cwd, *args):
    return run(cwd, "git", *args)


# ---------------------------------------------------------------- generation

def gen_lines(rng, tag, n=None):
    n = n if n is not None else rng.randint(10, 28)
    return ["%s-%03d filler words to pad this line out a bit\n" % (tag, i)
            for i in range(n)]


def edit_lines(rng, lines, frac, tag):
    """Replace ~frac of the lines with tagged, unique replacements, plus an
    occasional insert/delete so frac=0.0 is not a hard guarantee of
    byte-identity (a small amount of noise even at the low end)."""
    out = list(lines)
    if out and frac > 0:
        k = max(1, int(round(len(out) * frac)))
        for i in rng.sample(range(len(out)), min(k, len(out))):
            out[i] = "%s-edit-%d-%d\n" % (tag, i, rng.randrange(10 ** 6))
    if rng.random() < 0.3:
        pos = rng.randrange(len(out) + 1)
        out.insert(pos, "%s-extra-%d\n" % (tag, rng.randrange(10 ** 6)))
    if len(out) > 1 and rng.random() < 0.2:
        del out[rng.randrange(len(out))]
    return out


def make_path(rng, stem):
    return rng.choice(PATH_DIRS) + stem


def gen_round_files(rng):
    """Returns (base_files, ours_files, theirs_files, shape).

    All three are full path->lines maps (the actual working-tree state on
    that side), built by starting from an overlay ("what changes on this
    side") and applying it onto a copy of base -- an overlay value of None
    means "delete this path on this side".
    """
    base_files = {}
    ours_overlay = {}
    theirs_overlay = {}

    # Background filler files: independently, lightly mutated (or left
    # alone) on each side. Never load-bearing for the comparison, only here
    # so a round is not JUST the one focus file -- real merges usually touch
    # more than one path.
    for i in range(rng.randint(1, 3)):
        p = make_path(rng, "filler%d.txt" % i)
        content = gen_lines(rng, "fill%d" % i)
        base_files[p] = content
        if rng.random() < 0.5:
            ours_overlay[p] = edit_lines(rng, content, rng.choice([0.0, 0.2, 0.5]),
                                          "ours-fill")
        if rng.random() < 0.5:
            theirs_overlay[p] = edit_lines(rng, content, rng.choice([0.0, 0.2, 0.5]),
                                            "thrs-fill")

    shape = rng.choice(SHAPES)

    if shape == "rename_edit":
        src, dst = make_path(rng, "target.txt"), make_path(rng, "renamed.txt")
        base = gen_lines(rng, "base")
        base_files[src] = base
        ours_overlay[src] = None
        ours_overlay[dst] = edit_lines(rng, base, rng.choice(FRACS), "ours")
        theirs_overlay[src] = edit_lines(rng, base, rng.choice(FRACS), "thrs")

    elif shape == "edit_only":
        src = make_path(rng, "target.txt")
        base = gen_lines(rng, "base")
        base_files[src] = base
        ours_overlay[src] = edit_lines(rng, base, rng.choice(FRACS), "ours")
        theirs_overlay[src] = edit_lines(rng, base, rng.choice(FRACS), "thrs")

    elif shape == "delete":
        src = make_path(rng, "target.txt")
        base = gen_lines(rng, "base")
        base_files[src] = base
        ours_overlay[src] = None
        # Non-trivial edit on theirs so this is a genuine modify/delete, not
        # a clean (unconflicted) delete.
        theirs_overlay[src] = edit_lines(rng, base, rng.choice(FRACS[2:]), "thrs")

    elif shape == "rename_add_collision":
        src, dst = make_path(rng, "target.txt"), make_path(rng, "renamed.txt")
        base = gen_lines(rng, "base")
        base_files[src] = base
        ours_overlay[src] = None
        ours_overlay[dst] = edit_lines(rng, base, rng.choice(PAIRING_FRACS), "ours")
        # theirs never touches src's rename at all; it independently drops
        # an UNRELATED new file at ours' destination path -- a genuine
        # add/add-style collision, not a coincidental rename pairing.
        if rng.random() < 0.5:
            theirs_overlay[src] = edit_lines(rng, base, rng.choice(FRACS), "thrs")
        theirs_overlay[dst] = gen_lines(rng, "unrelated")

    elif shape == "rename_rename_1to2":
        src = make_path(rng, "target.txt")
        dst_a, dst_b = make_path(rng, "renamedA.txt"), make_path(rng, "renamedB.txt")
        base = gen_lines(rng, "base")
        base_files[src] = base
        ours_overlay[src] = None
        ours_overlay[dst_a] = edit_lines(rng, base, rng.choice(PAIRING_FRACS), "ours")
        theirs_overlay[src] = None
        theirs_overlay[dst_b] = edit_lines(rng, base, rng.choice(PAIRING_FRACS), "thrs")

    elif shape == "rename_rename_same_name":
        src = make_path(rng, "target.txt")
        dst = make_path(rng, "sameDest.txt")
        base = gen_lines(rng, "base")
        base_files[src] = base
        ours_overlay[src] = None
        ours_overlay[dst] = edit_lines(rng, base, rng.choice(PAIRING_FRACS), "ours")
        theirs_overlay[src] = None
        theirs_overlay[dst] = edit_lines(rng, base, rng.choice(PAIRING_FRACS), "thrs")

    elif shape == "rename_rename_2to1":
        src1, src2 = make_path(rng, "target1.txt"), make_path(rng, "target2.txt")
        dst = make_path(rng, "mergedDest.txt")
        base1, base2 = gen_lines(rng, "base1"), gen_lines(rng, "base2")
        base_files[src1] = base1
        base_files[src2] = base2
        ours_overlay[src1] = None
        ours_overlay[dst] = edit_lines(rng, base1, rng.choice(PAIRING_FRACS), "ours")
        theirs_overlay[src2] = None
        theirs_overlay[dst] = edit_lines(rng, base2, rng.choice(PAIRING_FRACS), "thrs")

    else:
        raise AssertionError("unreachable shape %r" % shape)

    def finalize(overlay):
        out = dict(base_files)
        for p, v in overlay.items():
            if v is None:
                out.pop(p, None)
            else:
                out[p] = v
        return out

    return base_files, finalize(ours_overlay), finalize(theirs_overlay), shape


# ------------------------------------------------------------------ one round

def write_state(repo, prev, new):
    """Move the working tree from state `prev` to state `new` and stage
    every change with `sg add` (a deletion is staged by `sg add`-ing a path
    whose file no longer exists on disk, matching git's own convention --
    verified against this project's own `sg add` before writing this
    fuzzer). Returns an error string, or None on success."""
    removed = [p for p in prev if p not in new]
    for p in removed:
        try:
            os.remove(os.path.join(repo, p))
        except OSError:
            pass
    changed = [p for p in new if prev.get(p) != new[p]]
    for p in changed:
        fp = os.path.join(repo, p)
        d = os.path.dirname(fp)
        if d:
            os.makedirs(d, exist_ok=True)
        with open(fp, "w") as fh:
            fh.write("".join(new[p]))
    touched = removed + changed
    if touched:
        r = sg(repo, "add", *touched)
        if r.returncode != 0:
            return "sg add failed: %s" % r.stderr.strip()
    return None


def build_repo(parent, base_files, ours_files, theirs_files):
    repo = os.path.join(parent, "repo")
    r = run(parent, SG, "init", "repo")
    if r.returncode != 0:
        return None, "sg init failed: %s" % r.stderr.strip()

    err = write_state(repo, {}, base_files)
    if err:
        return None, err
    r = sg(repo, "commit", "-m", "base")
    if r.returncode != 0:
        return None, "sg commit base failed: %s" % r.stderr.strip()

    r = sg(repo, "branch", "theirs")
    if r.returncode != 0:
        return None, "sg branch failed: %s" % r.stderr.strip()

    err = write_state(repo, base_files, ours_files)
    if err:
        return None, err
    r = sg(repo, "commit", "-m", "ours")
    if r.returncode != 0:
        return None, "sg commit ours failed: %s" % r.stderr.strip()

    r = sg(repo, "switch", "theirs")
    if r.returncode != 0:
        return None, "sg switch theirs failed: %s" % r.stderr.strip()

    err = write_state(repo, base_files, theirs_files)
    if err:
        return None, err
    r = sg(repo, "commit", "-m", "theirs")
    if r.returncode != 0:
        return None, "sg commit theirs failed: %s" % r.stderr.strip()

    r = sg(repo, "switch", "master")
    if r.returncode != 0:
        return None, "sg switch master failed: %s" % r.stderr.strip()

    return repo, None


def collect_workdir(repo):
    out = {}
    for root, dirs, files in os.walk(repo):
        if ".git" in dirs:
            dirs.remove(".git")
        for f in files:
            fp = os.path.join(root, f)
            rel = os.path.relpath(fp, repo).replace(os.sep, "/")
            with open(fp, "rb") as fh:
                out[rel] = fh.read()
    return out


def normalize_bytes(data):
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        return data
    fixed = MARKER_OURS_RE.sub(
        lambda m: m.group(1) + "HEAD" + (m.group(3) or ""), text)
    return fixed.encode("utf-8")


def ls_files_stage(repo):
    r = run_bytes(repo, "git", "ls-files", "-s")
    out = []
    for line in r.stdout.decode("utf-8", "replace").splitlines():
        meta, path = line.split("\t", 1)
        mode, sha, stage = meta.split()
        out.append((mode, int(stage), path, sha))
    return out


def cat_blob(repo, sha):
    r = run_bytes(repo, "git", "cat-file", "blob", sha)
    return r.stdout


def dump_tree(dest, files):
    for p, lines in files.items():
        fp = os.path.join(dest, p)
        d = os.path.dirname(fp)
        if d:
            os.makedirs(d, exist_ok=True)
        with open(fp, "w") as fh:
            fh.write("".join(lines))


def save_case(keep_dir, index, seed, shape, base_files, ours_files, theirs_files,
              sg_res, git_res, sg_wd, git_wd, mismatches):
    dest = os.path.join(keep_dir, "round%05d_%s" % (index, shape))
    os.makedirs(dest, exist_ok=True)
    dump_tree(os.path.join(dest, "base"), base_files)
    dump_tree(os.path.join(dest, "ours"), ours_files)
    dump_tree(os.path.join(dest, "theirs"), theirs_files)
    for name, wd in (("sg_workdir", sg_wd), ("git_workdir", git_wd)):
        for p, data in wd.items():
            fp = os.path.join(dest, name, p)
            d = os.path.dirname(fp)
            if d:
                os.makedirs(d, exist_ok=True)
            with open(fp, "wb") as fh:
                fh.write(data)
    with open(os.path.join(dest, "meta.txt"), "w") as fh:
        fh.write("seed: %d\n" % seed)
        fh.write("shape: %s\n" % shape)
        fh.write("sg rc=%d stdout=%r stderr=%r\n"
                  % (sg_res.returncode, sg_res.stdout, sg_res.stderr))
        fh.write("git rc=%d stdout=%r stderr=%r\n"
                  % (git_res.returncode, git_res.stdout, git_res.stderr))
        fh.write("mismatches:\n")
        for kind, detail in mismatches:
            fh.write("  %s: %s\n" % (kind, detail))
        fh.write("reproduce: python3 tests/fuzz_merge_rename.py 1 --seed %d\n" % seed)
    return dest


def one_round(rng, keep_dir, index, seed, verbose):
    base_files, ours_files, theirs_files, shape = gen_round_files(rng)

    parent = tempfile.mkdtemp(prefix="sg_fuzz_merge_rename_")
    try:
        repo, err = build_repo(parent, base_files, ours_files, theirs_files)
        if repo is None:
            return "setup", err, shape

        gitcopy = repo + ".gitcopy"
        shutil.copytree(repo, gitcopy)

        sg_res = sg(repo, "merge", "theirs")
        git_res = git(gitcopy, "merge", "theirs")

        mismatches = []

        sg_conflict = sg_res.returncode != 0
        git_conflict = git_res.returncode != 0
        if sg_conflict != git_conflict:
            mismatches.append(("rc", "sg conflict=%s (rc=%d) git conflict=%s (rc=%d)"
                                % (sg_conflict, sg_res.returncode,
                                   git_conflict, git_res.returncode)))

        sg_wd = collect_workdir(repo)
        git_wd = collect_workdir(gitcopy)
        if set(sg_wd) != set(git_wd):
            mismatches.append(("workdir_paths", "sg=%r git=%r"
                                % (sorted(sg_wd), sorted(git_wd))))
        else:
            bad_paths = [p for p in sg_wd
                         if normalize_bytes(sg_wd[p]) != normalize_bytes(git_wd[p])]
            if bad_paths:
                mismatches.append(("workdir_content", "paths=%r" % sorted(bad_paths)))

        sg_idx = ls_files_stage(repo)
        git_idx = ls_files_stage(gitcopy)
        sg_keys = {(m, s, p) for m, s, p, _ in sg_idx}
        git_keys = {(m, s, p) for m, s, p, _ in git_idx}
        if sg_keys != git_keys:
            mismatches.append(("index_paths", "sg=%r git=%r"
                                % (sorted(sg_keys), sorted(git_keys))))
        else:
            sg_blob = {(m, s, p): sha for m, s, p, sha in sg_idx}
            git_blob = {(m, s, p): sha for m, s, p, sha in git_idx}
            bad_entries = []
            for key in sg_keys:
                ca = normalize_bytes(cat_blob(repo, sg_blob[key]))
                cb = normalize_bytes(cat_blob(gitcopy, git_blob[key]))
                if ca != cb:
                    bad_entries.append(key)
            if bad_entries:
                mismatches.append(("index_content", "entries=%r" % sorted(bad_entries)))

        if mismatches and keep_dir:
            dest = save_case(keep_dir, index, seed, shape, base_files, ours_files,
                              theirs_files, sg_res, git_res, sg_wd, git_wd, mismatches)
            mismatches = [(k, "%s (case saved to %s)" % (d, dest)) for k, d in mismatches]

        if verbose and not mismatches:
            print("fuzz_merge_rename: round %d (%s) ok" % (index, shape))

        return mismatches, None, shape
    finally:
        shutil.rmtree(parent, ignore_errors=True)


# ------------------------------------------------------------------------ main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rounds", nargs="?", type=int, default=200)
    ap.add_argument("--seed", type=int, default=0,
                    help="seed base; round i uses seed base+i")
    ap.add_argument("--max-failures", type=int, default=0,
                    help="stop after this many mismatching rounds (0 = never "
                         "stop, which is what makes the result a rate)")
    ap.add_argument("--keep", metavar="DIR",
                    help="write each mismatching round's fixture and both "
                         "tools' output here")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(SG):
        print("fuzz_merge_rename: %s not found -- run make first" % SG)
        return 2
    if shutil.which("git") is None:
        print("fuzz_merge_rename: git not found, nothing to compare against")
        return 2
    if args.keep:
        os.makedirs(args.keep, exist_ok=True)

    category_counts = {}
    setup_failures = 0
    shape_total = {}
    shape_mismatch = {}
    mismatching_rounds = 0

    for i in range(args.rounds):
        seed = args.seed + i
        rng = random.Random(seed)
        result = one_round(rng, args.keep, i, seed, args.verbose)
        mismatches, setup_err, shape = result
        shape_total[shape] = shape_total.get(shape, 0) + 1

        if setup_err is not None:
            setup_failures += 1
            print("fuzz_merge_rename: round %d (seed %d, shape %s) setup "
                  "failure: %s" % (i, seed, shape, setup_err))
            continue

        if mismatches:
            mismatching_rounds += 1
            shape_mismatch[shape] = shape_mismatch.get(shape, 0) + 1
            for kind, detail in mismatches:
                category_counts[kind] = category_counts.get(kind, 0) + 1
                print("fuzz_merge_rename: round %d (seed %d, shape %s) %s "
                      "mismatch: %s" % (i, seed, shape, kind, detail))

        if args.max_failures and mismatching_rounds >= args.max_failures:
            print("fuzz_merge_rename: stopping early after %d mismatching "
                  "rounds (--max-failures); this is NOT a rate"
                  % mismatching_rounds)
            break

    print()
    print("fuzz_merge_rename: %d rounds from seed %d" % (args.rounds, args.seed))
    print()
    print("shape distribution:")
    for shape in SHAPES:
        total = shape_total.get(shape, 0)
        bad = shape_mismatch.get(shape, 0)
        print("  %-26s %4d rounds, %4d mismatched" % (shape, total, bad))
    print()
    print("mismatch categories (a round can hit more than one):")
    for kind in ("rc", "workdir_paths", "workdir_content", "index_paths",
                 "index_content"):
        print("  %-16s %4d" % (kind, category_counts.get(kind, 0)))
    if setup_failures:
        print("  setup failures:  %4d  (these measure nothing -- fix first)"
              % setup_failures)
    print()
    print("mismatching rounds: %d / %d" % (mismatching_rounds, args.rounds))
    return 1 if mismatching_rounds else 0


if __name__ == "__main__":
    sys.exit(main())
