# Rules: transport -- smart HTTP and SSH

Scope: `src/net/ssh.c`, `src/net/transport.c`, `include/sg/ssh.h`, `include/sg/transport.h`.

**Read this file before editing anything in that scope.** It is not
background reading: every rule here was measured against real git 2.55.0,
and most of the `WARNING:`s exist because something slipped past a fully
green board. Project-wide rules -- the gates, the code conventions, the
deliberate divergences -- stay in `CLAUDE.md`; the full derivations and
measurement tables stay in `docs/DESIGN.md` (look up a specific section,
do not read the whole thing).

- **`sg clone`/`fetch`/`push` speak SSH as well as smart HTTP since Phase
  47** (`src/net/ssh.c`, `include/sg/ssh.h`): `ssh://[user@]host[:port]/path`
  and the scp-like `[user@]host:path`. **pkt-line framing, want/have
  negotiation, sideband demux and the push report are transport-independent
  and reused byte for byte** -- only three things differ, and each is a trap:
  WARNING: **the `# service=...` packet and the flush after it are
  smart-HTTP's envelope, not the protocol's.** Over ssh the service IS the
  remote command, so the advertisement starts at the first ref;
  `parse_ref_advertisement_for_service` takes `expect_service_line` for
  exactly this, and requiring it over ssh rejects every valid advertisement
  as malformed.
  WARNING: **the two URL forms disagree about the leading slash** (measured
  with a logging stand-in for ssh): `ssh://host/srv/x` asks for `/srv/x`, the
  scp-like `host:srv/x` asks for `srv/x`, and `ssh://host/~alice/x` DROPS the
  slash so the far side's shell expands the home directory. Also **`host:22`
  is a PATH named 22** -- the scp-like syntax has no port.
  WARNING: **each `sg_transport_*` call opens its OWN connection** and
  `sg_ssh_request` therefore reads and discards an advertisement it has
  already seen. Real git holds one connection across both phases; matching
  that means threading a connection object through three commands, which is
  the trade Phase 40's write-up explains. Do not "fix" the discard without
  changing that shape first.
  WARNING: **this is the only subprocess in `src/`.** The poll loop
  (write and read at once), the ignored SIGPIPE, the half-close after the
  request, the flush that ends an advertisement-only connection, and the
  `waitpid` on every path each prevent a specific hang or silent kill --
  see `src/net/ssh.c`'s own comments before simplifying any of them.
  WARNING: **a new URL form can break code no one edited.** Phase 47's worst
  bug was in `derive_target_dir` (`cmd_clone.c`), untouched by the phase: it
  scans back to the last `/`, which every earlier URL form had, so
  `sg clone git@host:myproject.git` created a directory named
  `git@host:myproject`. Every scp-like fixture had passed an explicit
  destination directory -- the convenient thing to write, and the one that
  skips the guesser entirely.
  WARNING: **`sg_url_redact` treats a schemeless string as scp-like** since
  Phase 47, and uses the SAME "colon before any slash" rule the transport
  routes on -- without it a local path like `a/b@c:d` gets rewritten to
  `***@c:d`. It used to return such a string unchanged, which would print the
  user name out of `git@host:path`. An `@` AFTER the colon is path, not
  userinfo.
  WARNING: **a LOCAL `make sanitize` does not cover the ssh spawn path.**
  That gate builds the unit tests with ASan and runs those, and the only ssh
  code a unit test reaches is URL parsing; the fork, poll loop and pipe
  handling live behind `interop.sh`, which locally runs against the ordinary
  build. **CI is different and does cover them**: its ASan job runs
  `interop.sh` under ASan+UBSan with `detect_leaks=1`
  (`.github/workflows/ci.yml`), so the ssh group is sanitized there. Locally,
  when touching `src/net/ssh.c`, drive a clone/push/fetch over the shim by
  hand against a `make sanitize` build -- recipe in Phase 47 of
  `docs/DESIGN.md`. Done once for Phase 47: clean on clone, push, fetch and
  the failing-path case.
  Not read: `core.sshCommand`. `GIT_SSH_COMMAND` (word-split, git-compatible)
  then `GIT_SSH` then `ssh`.
