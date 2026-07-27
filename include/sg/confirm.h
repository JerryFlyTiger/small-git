#ifndef SG_CONFIRM_H
#define SG_CONFIRM_H

/* Gates a destructive operation behind an explicit confirmation. message is
   a caller-composed, complete description of what will happen, ending in a
   newline. When force is true, the check is skipped entirely and this
   returns 1 immediately (a short acknowledgement may still be printed).
   Otherwise: on a non-interactive stdin (no tty), message is printed to
   stderr along with a hint to pass --force, and this returns 0 without ever
   calling fgets (a non-interactive caller must not be blocked waiting for
   input that will never come). On an interactive stdin, message is printed
   followed by a y/N prompt; only an answer starting with 'y'/'Y' returns 1,
   everything else (including a blank line or EOF) returns 0. Returns 1 if
   the dangerous operation may proceed, 0 if it must be aborted with no
   changes made. */
int sg_confirm_dangerous(const char *message, int force);

#endif
