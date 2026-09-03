#ifndef SG_CLI_ARGS_H
#define SG_CLI_ARGS_H

#include "sg/pathspec.h"

/* CLI-layer helpers shared by every command that takes a pathspec, and (for
   the rev/path split) by every command that also takes a revision. Phase 62
   converges three byte-for-byte-identical copies (cmd_diff.c, cmd_status.c,
   cmd_stash.c) that predate this header -- see CLAUDE.md's "Known
   duplication" entry. Behavior is UNCHANGED by the convergence; only the
   cmd_name plumbed through sg_cli_split_revs_and_paths is new (see its own
   comment). */

/* Prints the "sg: ..." diagnostic for a failed sg_pathspec_add, matching one
   of its three sg_pathspec_error values. There is deliberately no `default:`
   in the switch this wraps -- SG_PATHSPEC_ERR_NONE cannot reach here
   (sg_pathspec_add only fills *err on failure), but naming every enumerator
   keeps the switch exhaustive so -Wswitch complains if a future error code
   is added and this function is not taught to print it; a `default` would
   silently render an unknown code as "outside the repository" instead. */
void sg_cli_report_pathspec_error(sg_pathspec_error err, const char *arg, const char *repo_root);

/* Whether an argument names something that exists in the working tree --
   what decides a bare (no "--") argument's fate in
   sg_cli_split_revs_and_paths. A wildcard argument is accepted without
   asking the filesystem at all: measured against git 2.55.0, `git diff
   '*.zzz'` (matching nothing) exits 0, while the wildcard-free `git diff
   nosuch` is a hard error. Uses lstat, not stat: a dangling symlink is still
   a path the user named. */
int sg_cli_arg_exists_in_worktree(const char *arg);

/* Splits the positional arguments into revisions and pathspecs the way git
   does when no "--" was given, and returns how many leading arguments are
   revisions (-1 after printing an error).

   The rules, measured against git 2.55.0 (and, for `sg log`, re-measured in
   Phase 62 -- identical):
     - an argument that is both a valid revision and an existing file is
       rejected outright rather than guessed at;
     - the first argument that is a path ends the revision list, and from
       there on EVERY remaining argument must exist -- `git diff a.txt HEAD`
       fails naming HEAD, even though HEAD is a perfectly good revision;
     - an argument that is neither is the "ambiguous argument" error, which
       is what `git diff nosuch` prints.
   Each message names the offending argument and points at "--", and names
   `cmd_name` (e.g. "diff", "log") in the "use sg <cmd_name> -- <path>"
   suggestion -- the caller's own command name, so the message reads
   correctly no matter which command shares this function. */
int sg_cli_split_revs_and_paths(const char *git_dir, char **pos, int n_pos, const char *cmd_name);

#endif
