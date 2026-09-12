# Rules: `sg push` -- refspecs, wildcards, deletion

Scope: `src/cli/cmd_push.c`.

**Read this file before editing anything in that scope.** It is not
background reading: every rule here was measured against real git 2.55.0,
and most of the `WARNING:`s exist because something slipped past a fully
green board. Project-wide rules -- the gates, the code conventions, the
deliberate divergences -- stay in `CLAUDE.md`; the full derivations and
measurement tables stay in `docs/DESIGN.md` (look up a specific section,
do not read the whole thing).

- **`sg push` takes wildcard and push-matching refspecs since Phase 46**,
  the two forms Phase 39 named and deliberately refused. They need their
  input from OPPOSITE sides of the network round trip, and that is the whole
  design: a **wildcard's source set is LOCAL** (measured -- a pattern
  CREATES remote branches that did not exist there, so it is not an
  intersection with the advertisement and there is no prune semantics), so
  it expands BEFORE connecting and Phase 39's whole-batch-abort rule still
  holds; **`:` means "every local branch that already exists on the remote"**,
  so it can only expand AFTER the advertisement and is appended to
  `candidates` there, leaving the candidate->entry loop's rules untouched.
  WARNING: **an expanded wildcard dst is used VERBATIM, with no dwim
  completion** -- the opposite of an explicit dst. Routing it through
  `complete_dst` is the natural-looking choice and is wrong twice over: it
  would turn `refs/heads/*:x*` into pushes to `refs/heads/x<name>` (git sends
  the uncompleted name and lets the remote refuse it), and
  `complete_dst`'s "dst matches more than one" rule would reject every valid
  wildcard, since a wildcard is ambiguous by definition.
  WARNING: **exactly one `*` per side, on BOTH sides** -- a star on one side
  only, two on a side, and a wildcard DELETION are each `fatal: invalid
  refspec` (measured). The deletion case is the one that matters:
  approximating it would delete every matching remote ref.
  WARNING: **the star may sit anywhere and it CROSSES `/`** (measured: a
  pattern rooted at `refs/` matched `refs/remotes/origin/topic/sub`), a
  no-colon wildcard mirrors itself, and a pattern matching nothing is exit 0,
  not an error. An annotated tag matched by a pattern reaches the remote
  unpeeled, the same trap `resolve_refspec_src` documents.
  WARNING: **a wildcard must skip `SG_CHUNK_KEEPALIVE_REF`** -- that ref is
  owned by the chunks-propagation block, which computes its own old/new pair
  on every push. A pattern wide enough to match it queues a SECOND update
  for the same ref, and the remote refuses the whole atomic transaction
  (`multiple updates for ref 'refs/sg/chunks' not allowed`), so **nothing
  lands, not even the branch the user meant**. Narrow fixtures rooted at
  `refs/heads/` cannot reach this; the pin needs a repo whose remote has no
  keepalive ref yet.
  WARNING: **`sg_push_refspec` is defined in `cmd_push.c` AND duplicated in
  `tests/test_refspec.c`** (deliberate convention -- no public header for a
  test-only export). Adding a field to one and not the other is a
  stack-buffer-overflow that `make test` does NOT catch, only ASan does
  (measured in Phase 46: the library's `memset` wrote 40 bytes into the
  test's 32-byte object while `make test` reported only ordinary assertion
  failures).
- **`sg push` gained refspec support (`[+]<src>[:<dst>]`) and
  `--delete <name>...` in Phase 39** (`src/cli/cmd_push.c`, no header
  changes -- see the Phase 39 section of `docs/DESIGN.md` for why). Five
  things are especially easy to get backwards:
  1. **Split on the LAST `:`, not the first** (`strrchr`). Measured:
     `a:b:c:d` reports src `a:b:c`, dst `d`.
  2. **An annotated tag given as `<src>` is never peeled** -- `<src>`
     resolution must not be `sg_rev_parse_commit` (see that function's own
     entry in `docs/RULES-refs-revparse.md` for why, and the exact
     fallback order).
  3. **"`<src>` matches nothing" and "non-fast-forward" are two different
     failure classes, do not conflate them.** A src that resolves to
     nothing aborts the WHOLE push before any network round trip -- not one
     ref lands, not even a connection attempt (measured: `git push origin
     topic:newbr2 nosuch:x` leaves `newbr2` uncreated). A non-fast-forward
     rejection, by contrast, is a PER-REF failure discovered only after the
     advertisement arrives -- a fast-forwardable ref in the SAME invocation
     still lands (measured: `topic -> newbr` succeeds alongside a refused
     `master -> fromhead`). Before Phase 39 a push could only ever carry one
     non-tag ref, so the pre-existing `check_fast_forward` rejection path
     doing a whole-batch `goto done` was unobservable as a bug; Phase 39's
     own multi-refspec fixture exposed it directly (a good ref got
     discarded alongside a bad one in the same push) and it was fixed to
     `had_rejection=1; continue;`, the same per-ref shape the tag-rejection
     and delete-target-missing paths already used right next to it.
  4. **An unqualified `<dst>` that matches MORE THAN ONE advertised ref is
     refused, not guessed at.** Measured: with both `refs/heads/dup` and
     `refs/tags/dup` on the remote, `git push origin topic:dup` prints
     `error: dst refspec dup matches more than one` and changes neither.
     `complete_dst`'s rule-1 loop therefore has to scan **every** guess
     prefix and count matches -- a "stop at the first hit" loop silently
     writes to whichever prefix comes first in `guess_prefixes[]`, i.e. to a
     ref the user never named. Same shape as the pre-existing
     `src refspec '%s' matches more than one` rule one layer up.
  5. **A push where every requested ref was rejected must touch the remote
     nowhere at all**, hence `if (had_rejection && entry_count == 0) goto
     done;` between the candidate loop and the `refs/sg/chunks` propagation
     block. Item 3's `goto done` -> `continue` change is what made this
     necessary: without the gate a fully-refused push falls through into
     chunks propagation and performs a REAL remote write (and prints
     `To <url>`), which pre-Phase-39 single-ref behaviour never did.
     **"Everything was already up to date" also leaves `entry_count` at 0**
     -- that case must still propagate chunks, which is why the gate tests
     `had_rejection` and not just the count.
