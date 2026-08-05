#ifndef SG_VERSION_H
#define SG_VERSION_H

/* The single definition of sg's version. Used by `sg --version`, by the
   agent string sg announces to remotes in src/net/transport.c, and quoted in
   the .TH line of docs/sg.1. It lives in its own header rather than in
   cli.h so that the transport layer can use it without depending on the
   command-line layer.

   Kept as a bare string so it can be pasted into a format literal by string
   concatenation, which is what keeps the protocol agent strings from
   drifting out of step with the binary's own reported version. */
#define SG_VERSION "0.1"

#endif
