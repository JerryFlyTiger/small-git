#!/usr/bin/env python3
"""Randomized rename-similarity fuzzer for `sg diff`, with real git as the
oracle. Differential harness for Phase 30 (inexact rename detection).

FAILURE DIRECTION: `git diff --name-status -M<n>` and `sg diff
--name-status -M<n>` are compared per rename-score field, not by a raw
`cmp` of the whole output. A `cmp`-style byte comparison is the wrong tool
here: a similarity score that is off by a single point (`R074` vs `R075`)
makes the *entire line* differ, so every mismatch would look identical to a
mismatch where the two sides do not even agree on which file renamed into
which -- those are wildly different bugs and a boolean pass/fail erases the
distinction. So this script parses both outputs into structured rename
events and buckets every disagreement into one of three kinds (pairing,
score, other), and the thing it reports is a **rate and a distribution**,
never a single true/false.

This also means the tool's own useful state, at the moment this is written,
is "loud": sg has only exact (byte-identical) rename detection so far (see
CLAUDE.md, Phase 29), so any inexact pair -- similarity 1-99% -- is expected
to mismatch on every single iteration. That is not a bug in the tool, it is
the gap the tool exists to measure; the number to watch as Phase 30 lands is
the *pairing*-mismatch rate falling towards the *score*-mismatch rate (sg
finding the same renames git finds, only disagreeing on the percentage), not
the overall rate hitting zero on day one.

Not part of `make test` or tests/interop.sh: verification tool, run by hand
once inexact rename detection is being built or touched.

    make && python3 tests/fuzz_rename.py            # 200 iterations
    python3 tests/fuzz_rename.py 500 --seed 4000     # longer soak, shifted seed
    python3 tests/fuzz_rename.py 200 --max-failures 0   # count only, no detail

Each iteration i uses seed base+i, printed on every mismatch, so a single
iteration reproduces exactly with `--seed <that seed> 1`.
"""
import collections
import os
import random
import shutil
import subprocess
import sys
import tempfile

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SG = os.path.join(PROJECT_ROOT, "build", "sg")

# Same rationale as tests/fuzz_diff.py: the oracle and sg must not disagree
# for reasons that have nothing to do with rename detection (locale, a
# translated line, ambient gitconfig).
ENV = dict(os.environ)
ENV.update({
    "GIT_AUTHOR_NAME": "F", "GIT_AUTHOR_EMAIL": "f@example.com",
    "GIT_COMMITTER_NAME": "F", "GIT_COMMITTER_EMAIL": "f@example.com",
    "GIT_AUTHOR_DATE": "2026-01-01T00:00:00Z",
    "GIT_COMMITTER_DATE": "2026-01-01T00:00:00Z",
    "LC_ALL": "C",
})

# `diff.renames` is unset by default in stock git, but this box's global
# gitconfig may set it -- measured: on this machine, plain `git diff
# --name-status` (no -M at all) already prints an R line. sg's cmd_diff.c
# has no such ambient dependency (it always defaults rename_score = 50
# unless --no-renames), so leaving git's config unpinned would make the
# "no flag given" case compare sg-with-detection against an oracle whose
# answer silently depends on whoever's laptop this runs on. Pin it to match
# sg's actual default.
GIT_DIFF = ["git", "-c", "core.quotepath=false", "-c", "diff.renames=true", "diff"]

# Deliberately small and full of repeats, same reasoning as fuzz_diff.py:
# a shared vocabulary produces content whose similarity is easy to nudge
# across the whole 0-100% range instead of everything being unique noise.
VOCAB = [
    "def handler(request):",
    "class Thing:",
    "_helper = None",
    "    return value",
    "    if flag:",
    "        do_work()",
    "        return None",
    "# comment",
    "}",
    "{",
    "x = 1",
    "y = 2",
    "    pass",
]

# -M values to exercise. Every one of these is accepted by both sides since
# Phase 30 filled in git's grammar; before that, sg rejected the trailing '%'
# form outright and each of these rounds landed in the "other" bucket. Keep
# the '%' spellings here rather than trimming them to the ones that "work":
# the grammar is the counter-intuitive part (-M100 is ten percent, only
# -M100% is exact-renames-only), so it is exactly what wants fuzzing.
FLAG_CHOICES = [None, ["-M"], ["-M50%"], ["-M1%"], ["-M90%"], ["-M100%"]]

PATH_DIRS = ["", "src", "d1", "d1/d2", "lib", "sub"]
PATH_EXT = [".txt", ".c", ".md", ""]


def run(cmd, cwd):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, env=ENV)


def sg(args, cwd, check=True):
    r = run([SG] + args, cwd)
    if check and r.returncode != 0:
        raise RuntimeError("sg %s failed (rc=%d): %s"
                           % (" ".join(args), r.returncode,
                              r.stderr.decode("utf-8", "replace")))
    return r


def rand_path(rng, used, force_basename=None):
    while True:
        d = rng.choice(PATH_DIRS)
        base = force_basename if force_basename else (
            rng.choice(["a", "b", "c", "file", "mod"]) + str(rng.randrange(1000))
            + rng.choice(PATH_EXT))
        p = (d + "/" + base) if d else base
        if p not in used:
            used.add(p)
            return p


def gen_content(rng):
    """One of four content shapes: plain text, CRLF line endings, binary
    with an embedded NUL, or a line over 64 bytes. All are represented as
    raw bytes split/joined on plain '\\n' so the edit function below works
    uniformly regardless of shape."""
    kind = rng.choice(["text", "crlf", "binary", "longline"])
    lines = [rng.choice(VOCAB) for _ in range(rng.randrange(3, 12))]
    if kind == "longline":
        long_line = "".join(rng.choice(VOCAB) for _ in range(6))
        while len(long_line) <= 64:
            long_line += rng.choice(VOCAB)
        lines[rng.randrange(len(lines))] = long_line
        data = ("\n".join(lines) + "\n").encode()
    elif kind == "crlf":
        data = ("\r\n".join(lines) + "\r\n").encode()
    elif kind == "binary":
        data = ("\n".join(lines)).encode() + b"\x00" + bytes(
            rng.randrange(0, 256) for _ in range(rng.randrange(5, 30)))
    else:
        data = ("\n".join(lines) + "\n").encode()
    return data


def edit_bytes(rng, content):
    """Chunk-level edits (delete/insert/replace/duplicate) on '\\n'-split
    byte chunks. Intensity ranges from "none" (unchanged -> exact rename,
    should score 100) to "heavy" (most chunks touched -> low but usually
    nonzero similarity, since VOCAB entries repeat), spanning the 0-100%
    similarity range the harness needs to exercise."""
    chunks = content.split(b"\n")
    intensity = rng.choice(["none", "light", "medium", "heavy"])
    if intensity == "none":
        return content
    n = {"light": rng.randrange(1, 3),
         "medium": max(1, len(chunks) // 2),
         "heavy": max(1, len(chunks))}[intensity]
    out = list(chunks)
    for _ in range(n):
        if not out:
            out = [bytes(rng.randrange(0, 256) for _ in range(rng.randrange(3, 10)))]
            continue
        pos = rng.randrange(len(out))
        op = rng.randrange(4)
        if op == 0 and len(out) > 1:
            del out[pos]
        elif op == 1:
            out.insert(pos, rng.choice(VOCAB).encode())
        elif op == 2:
            out[pos] = rng.choice(VOCAB).encode()
        else:
            out.insert(pos, out[pos])
    return b"\n".join(out)


def build_case(repo, rng):
    """Commits 1-3 random source files, then in a second commit-worthy state
    deletes them and creates 1-3 destination files whose content is either a
    randomly-edited copy of a source (similarity spanning 0-100%) or wholly
    unrelated fresh content (an unpaired add), sometimes reusing a source's
    basename (git's basename-first-pass shortcut) and sometimes leaving a
    source with no replacement at all (an unpaired delete)."""
    os.makedirs(repo)
    sg(["init", "."], repo)

    used = set()
    sources = []
    for _ in range(rng.randrange(1, 4)):
        p = rand_path(rng, used)
        content = gen_content(rng)
        full = os.path.join(repo, p)
        if os.path.dirname(full):
            os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as fh:
            fh.write(content)
        sources.append((p, content))

    sg(["add"] + [p for p, _ in sources], repo)
    sg(["commit", "-m", "base"], repo)

    for p, _ in sources:
        os.remove(os.path.join(repo, p))

    for _ in range(rng.randrange(1, 4)):
        if sources and rng.random() < 0.7:
            _, src_content = rng.choice(sources)
            content = edit_bytes(rng, src_content)
        else:
            content = gen_content(rng)
        basename = None
        if sources and rng.random() < 0.4:
            basename = os.path.basename(rng.choice(sources)[0])
        p = rand_path(rng, used, force_basename=basename)
        full = os.path.join(repo, p)
        if os.path.dirname(full):
            os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as fh:
            fh.write(content)

    sg(["add", "."], repo)


def build_case_copies_harder(repo, rng):
    """Phase 51: like build_case, but keeps 1-3 of the committed files
    UNTOUCHED (never deleted) alongside the ones that get removed -- exactly
    the shape `--find-copies-harder` exists for, since plain -C only offers
    a deleted or still-present-but-EDITED path as a source, never one that
    never changed at all. Destinations are edited copies of either a
    deleted source or one of the untouched keepers, so the fuzzer actually
    exercises the wider source pool rather than reproducing build_case's
    coverage under a different flag."""
    os.makedirs(repo)
    sg(["init", "."], repo)

    used = set()
    sources = []
    for _ in range(rng.randrange(1, 4)):
        p = rand_path(rng, used)
        content = gen_content(rng)
        full = os.path.join(repo, p)
        if os.path.dirname(full):
            os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as fh:
            fh.write(content)
        sources.append((p, content))

    keepers = []
    for _ in range(rng.randrange(1, 4)):
        p = rand_path(rng, used)
        content = gen_content(rng)
        full = os.path.join(repo, p)
        if os.path.dirname(full):
            os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as fh:
            fh.write(content)
        keepers.append((p, content))

    sg(["add"] + [p for p, _ in sources] + [p for p, _ in keepers], repo)
    sg(["commit", "-m", "base"], repo)

    for p, _ in sources:
        os.remove(os.path.join(repo, p))
    # keepers are deliberately NOT removed and NOT re-staged -- their working
    # copy and the committed blob stay byte-identical, which is exactly what
    # "unchanged" means to sg_diff_trees' include_unchanged parameter.

    candidates = sources + keepers
    for _ in range(rng.randrange(1, 4)):
        if candidates and rng.random() < 0.7:
            _, src_content = rng.choice(candidates)
            content = edit_bytes(rng, src_content)
        else:
            content = gen_content(rng)
        basename = None
        if candidates and rng.random() < 0.4:
            basename = os.path.basename(rng.choice(candidates)[0])
        p = rand_path(rng, used, force_basename=basename)
        full = os.path.join(repo, p)
        if os.path.dirname(full):
            os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as fh:
            fh.write(content)

    sg(["add", "."], repo)


def parse_name_status(output):
    """Returns a list of (status, old_path_or_None, path, score_or_None).
    For R/C rows `path` is the *destination*. For A/D/M rows `path` is the
    single path field and old_path is None."""
    entries = []
    text = output.decode("utf-8", "surrogateescape")
    for line in text.splitlines():
        if not line:
            continue
        parts = line.split("\t")
        code = parts[0]
        if code[0] in ("R", "C") and len(parts) == 3:
            entries.append((code[0], parts[1], parts[2], int(code[1:])))
        elif len(parts) == 2:
            entries.append((code, None, parts[1], None))
        else:
            # Unparseable line (e.g. an unexpected format) -- surface it
            # as an "other" mismatch rather than silently dropping it.
            entries.append(("?", None, line, None))
    return entries


def index_new(entries):
    """path (destination / current path) -> (status, partner_or_None, score)."""
    d = {}
    for status, old, path, score in entries:
        if status == "D":
            continue
        d[path] = (status, old, score)
    return d


def index_old(entries):
    """path (source path that stopped existing) -> (status, partner, score)."""
    d = {}
    for status, old, path, score in entries:
        if status == "D":
            d[path] = ("D", None, None)
        elif status in ("R", "C"):
            d[old] = (status, path, score)
    return d


def compare_domain(git_d, sg_d, score_diffs, other_detail):
    """Returns (pairing, score, other) mismatch counts for one path-key
    domain (either the new-path domain or the old-path domain)."""
    pairing = score = other = 0
    for path in set(git_d) | set(sg_d):
        g = git_d.get(path)
        s = sg_d.get(path)
        if g == s:
            continue
        if g is None or s is None:
            other += 1
            other_detail.append("path %r: git=%r sg=%r" % (path, g, s))
            continue
        gst, gpartner, gscore = g
        sst, spartner, sscore = s
        g_is_r = gst in ("R", "C")
        s_is_r = sst in ("R", "C")
        if g_is_r and s_is_r:
            if gpartner != spartner:
                pairing += 1
            elif gscore != sscore:
                score += 1
                score_diffs[sscore - gscore] += 1
            # else: identical rename, not a mismatch
        elif g_is_r != s_is_r:
            pairing += 1
        else:
            other += 1
            other_detail.append("path %r: git=%r sg=%r" % (path, g, s))
    return pairing, score, other


def compare_outputs(git_out, sg_out, git_rc, sg_rc, score_diffs):
    """Categorizes the divergence between one pair of --name-status outputs.
    Returns (pairing, score, other, detail_lines)."""
    other_detail = []
    if git_rc != 0 or sg_rc != 0:
        other_detail.append("exit codes: git=%d sg=%d" % (git_rc, sg_rc))
    git_entries = parse_name_status(git_out)
    sg_entries = parse_name_status(sg_out)
    p1, s1, o1 = compare_domain(index_new(git_entries), index_new(sg_entries),
                                 score_diffs, other_detail)
    p2, s2, o2 = compare_domain(index_old(git_entries), index_old(sg_entries),
                                 score_diffs, other_detail)
    other = o1 + o2 + (1 if (git_rc != 0 or sg_rc != 0) else 0)
    return p1 + p2, s1 + s2, other, other_detail


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
    # Phase 51: --find-copies-harder mode. A separate case builder (keeps
    # some committed files untouched) and a fixed -C -C flag, rather than
    # FLAG_CHOICES' rotation -- the whole point is exercising the wider
    # source pool, which only fires under -C -C.
    copies_harder = "--copies-harder" in argv
    if copies_harder:
        argv.remove("--copies-harder")
    iterations = int(argv[0]) if argv else 200

    root = tempfile.mkdtemp(prefix="sg_fuzz_rename_")
    rounds_with_pairing = 0
    rounds_with_score = 0
    rounds_with_other = 0
    rounds_with_any = 0
    rounds_with_nonzero = 0
    score_diffs = collections.Counter()
    detail_printed = 0
    i = -1
    for i in range(iterations):
        seed = seed_base + i
        rng = random.Random(seed)
        repo = os.path.join(root, "r%d" % seed)
        if copies_harder:
            build_case_copies_harder(repo, rng)
            extra = ["-C", "-C"]
        else:
            build_case(repo, rng)
            flag = rng.choice(FLAG_CHOICES)
            extra = list(flag) if flag else []
        want = run(GIT_DIFF + ["--cached", "--name-status"] + extra, repo)
        got = run([SG, "diff", "--cached", "--name-status"] + extra, repo)
        pairing, score, other, detail = compare_outputs(
            want.stdout, got.stdout, want.returncode, got.returncode, score_diffs)

        # sg's diff exits 0 unless it could not read a blob (same rationale
        # as fuzz_diff.py's 2026-09-01 note), so a non-zero exit here is the
        # machine-noise class -- but only when it is the round's ONLY
        # evidence.  A round that ALSO disagreed on the pairing or the score
        # is a real divergence that happened to exit non-zero too, and
        # telling the reader to rerun and dismiss it would be worse than not
        # counting at all.
        if got.returncode != 0 and pairing == 0 and score == 0:
            rounds_with_nonzero += 1

        if pairing == 0 and score == 0 and other == 0:
            shutil.rmtree(repo, ignore_errors=True)
            continue

        rounds_with_any += 1
        if pairing:
            rounds_with_pairing += 1
        if score:
            rounds_with_score += 1
        if other:
            rounds_with_other += 1

        if detail_printed >= max_failures:
            shutil.rmtree(repo, ignore_errors=True)
            continue

        detail_printed += 1
        print("=== MISMATCH seed %d -- flags=%r -- repo kept at %s"
              % (seed, extra, repo))
        print("    reproduce: python3 tests/fuzz_rename.py 1 --seed %d" % seed)
        print("    pairing=%d score=%d other=%d" % (pairing, score, other))
        print("    --- git diff --cached --name-status %s ---" % " ".join(extra))
        print("    " + want.stdout.decode("utf-8", "replace").replace("\n", "\n    ").rstrip())
        print("    --- sg diff --cached --name-status %s ---" % " ".join(extra))
        print("    " + got.stdout.decode("utf-8", "replace").replace("\n", "\n    ").rstrip())
        for line in detail[:10]:
            print("    detail: " + line)

    total = i + 1
    print()
    print("fuzz_rename: %d iterations from seed %d" % (total, seed_base))
    if total:
        print("  rounds with ANY mismatch:     %5d / %-5d (%5.1f%%)"
              % (rounds_with_any, total, 100.0 * rounds_with_any / total))
        print("  rounds with pairing mismatch: %5d / %-5d (%5.1f%%)  "
              "(sg and git disagree on WHICH file renamed into which)"
              % (rounds_with_pairing, total, 100.0 * rounds_with_pairing / total))
        print("  rounds with score mismatch:   %5d / %-5d (%5.1f%%)  "
              "(same pairing, disagree on the similarity percentage)"
              % (rounds_with_score, total, 100.0 * rounds_with_score / total))
        print("  rounds with other mismatch:   %5d / %-5d (%5.1f%%)  "
              "(exit code, or a divergence outside pairing/score)"
              % (rounds_with_other, total, 100.0 * rounds_with_other / total))
        if rounds_with_nonzero:
            print("    of which sg exited non-zero: %d -- rerun the same "
                  "seed range before calling any of these an algorithmic "
                  "divergence" % rounds_with_nonzero)
    if score_diffs:
        print("  score diff (sg - git) distribution, top entries:")
        for diff, count in score_diffs.most_common(10):
            print("    %+4d : %d" % (diff, count))
    if rounds_with_any == 0:
        shutil.rmtree(root, ignore_errors=True)
    return 1 if rounds_with_any else 0


sys.exit(main())
