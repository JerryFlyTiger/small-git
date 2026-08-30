#ifndef SG_SSH_H
#define SG_SSH_H

#include "sg/http.h" /* sg_buf */

#include <stddef.h>

/* The SSH transport (Phase 47). git's smart protocol over ssh is the same
   pkt-line conversation as over HTTP, with three differences that this
   header exists to name:

     1. There is no "# service=..." packet and no flush before the refs --
        that envelope is smart-HTTP's own, invented so a single GET can say
        which service it wants. Over ssh the service IS the command run on
        the far side ("git-upload-pack '<path>'"), so the advertisement
        starts at the first ref.
     2. It is ONE bidirectional stream, not a request/response pair.
     3. There is no URL to build: the path is an argument to the remote
        command, quoted, and the host is an argument to ssh.

   Everything else -- pkt-line framing, the want/have negotiation, sideband
   demultiplexing, the push report -- is protocol, not transport, and is
   shared with the HTTP path unchanged. */

/* Is this a URL this transport handles? True for an explicit "ssh://..."
   and for the scp-like "[user@]host:path" shorthand.

   The shorthand is the delicate one: it is "a colon before any slash", so
   "host:path" and "user@host:repo.git" match while "http://x/y" (the colon
   comes after "//"... which contains no slash before it -- see the
   implementation for why the "://" case is excluded first) and a plain local
   path "/tmp/x" do not. git's own rule is the same shape. */
int sg_ssh_is_ssh_url(const char *url);

/* Splits an ssh URL into the pieces the ssh command line needs. All three
   outputs are malloc'd (caller frees); *port_out is NULL when the URL names
   no port. Returns 0 on success, -1 on a malformed URL (message printed).

   Measured against git 2.55.0 with a logging stand-in for ssh, because the
   two URL forms do NOT agree about the leading slash:
     ssh://host/srv/repo.git   -> path "/srv/repo.git"   (slash KEPT)
     host:srv/repo.git         -> path "srv/repo.git"    (no slash to keep)
     ssh://host/~alice/repo    -> path "~alice/repo"     (slash DROPPED)
   That last row is not a typo: git strips the leading slash when what
   follows is a "~", so a home-relative path survives. */
int sg_ssh_parse_url(const char *url, char **user_host_out, char **port_out, char **path_out);

/* Runs <service> on the far side and reads the ref advertisement it sends
   immediately, leaving nothing else in *out. Sends a flush before closing,
   which is how a client says "I want nothing" -- without it the far side
   reports a hung-up connection. Returns 0 on success, -1 on failure
   (message printed). */
int sg_ssh_advertise(const char *url, const char *service, sg_buf *out);

/* Runs <service>, DISCARDS the advertisement it sends, then writes request
   and reads everything the far side sends back into *out.

   Discarding an advertisement sg has already seen is the price of keeping
   the four sg_transport_* entry points independent, each opening its own
   connection: real git holds one connection open across both phases. See
   Phase 47 of docs/DESIGN.md for why that trade was taken.

   Returns 0 on success, -1 on failure (message printed). */
int sg_ssh_request(const char *url, const char *service, const void *request, size_t request_len,
                   sg_buf *out);

#endif
