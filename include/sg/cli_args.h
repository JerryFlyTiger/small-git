#ifndef SG_CLI_ARGS_H
#define SG_CLI_ARGS_H

#include "sg/pathspec.h"
#include "sg/revparse.h"

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

/* Prints the section-3 ambiguity block (Phase 68b) that every
   sg_rev_parse_commit(_ex)/sg_rev_parse_object caller must print on -4,
   byte for byte against real git:

     error: short object ID <lowercased prefix> is ambiguous
     hint: The candidates are:
     hint:   <7hex> <type>[ <YYYY-MM-DD> - <subject or tag name>]

   `as_typed` is whatever string the caller actually handed to
   sg_rev_parse_commit(_ex)/sg_rev_parse_object -- the ambiguous PREFIX
   itself is re-derived from it (the same base-extraction rule
   sg_rev_parse_commit_ex uses: stop at ':' for the <rev>:<path> form, then
   at the first '~'/'^'/"@{"), so a suffixed or `<rev>:<path>` argument
   still names only the ambiguous hex run, not the whole string -- git's own
   error line does the same. Re-enumerates via sg_object_find_prefix rather
   than being handed the candidate list, since a caller three frames away
   from the actual resolve_ambiguous_prefix call has no list to hand back
   (this project's error-reporting convention: lower layers print nothing,
   return a code, and the CLI layer re-derives whatever it needs to say).
   Candidates are ordered by TYPE first (tag, commit, tree, blob), then by
   hex within a type. A commit row's date is the AUTHOR date rendered in
   the commit's OWN stored offset (sg_date_format_short); a tag row's date
   is the tagger's, and its trailing field is the tag's NAME, not its
   message; the subject shown for a commit is the FOLDED subject
   (sg_commit_out_fold_subject_alloc), not just the first line.

   `disambig` is the CALLER's own top-level disambiguation mode (the same
   value it passed to sg_rev_parse_commit_ex / SG_REV_STRICT for a plain
   sg_rev_parse_commit or sg_rev_parse_object caller) -- NOT necessarily
   the mode that was actually in effect when the ambiguity was found.
   This is load-bearing, measured against real git 2.55.0, and easy to get
   backwards: when a still-ambiguous prefix has at least one commit-ish
   (commit or tag) candidate AND commit-ish disambiguation was actually
   applied, git's own hint list is NARROWED to just the commit-ish
   candidates (tree/blob rows are dropped entirely) -- e.g. `git log -1
   <amb>` on a tag+commit+tree+blob collision lists only the tag and the
   commit. With ZERO commit-ish candidates, or under STRICT, the FULL list
   is shown, matching the un-narrowed section-3 rule. And -- the part a
   caller-only parameter cannot express by itself -- this narrowing fires
   under a STRICT command too whenever the SUFFIX trigger (sg_rev_disambig's
   own trigger 2) escalated that particular resolution to commit-ish, e.g.
   `git cat-file -t <amb>~1` (cat-file is STRICT) still narrows its hint
   list, while the bare `git cat-file -t <amb>` (no suffix) does not. This
   function therefore re-derives the SAME suffix escalation
   sg_rev_parse_commit_ex applies internally -- the effective mode for
   filtering is COMMITTISH whenever `as_typed`'s rev part (before any ':'
   path suffix) carries a trailing "~"/"^"/"@{", regardless of `disambig`.

   The caller is expected to print its own existing diagnostic immediately
   after this (in place of git's trailing "fatal:" line) and exit 1 --
   this function only ever prints the borrowed error:/hint: block, never
   an "sg: " line, and never touches the exit code. Prints nothing at all
   if git_dir/as_typed cannot even be re-enumerated (should not happen for
   an as_typed that legitimately produced -4 moments ago). */
void sg_cli_report_ambiguous_oid(const char *git_dir, const char *as_typed, sg_rev_disambig disambig);

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
   correctly no matter which command shares this function.

   `disambig` (Phase 68b) is threaded straight through to
   sg_rev_parse_commit_ex for the revision check: `sg log` passes
   SG_REV_COMMITTISH, `sg diff` passes SG_REV_STRICT (measured: `git diff
   <amb>` refuses). A -4 result is treated as "this argument WAS a revision
   attempt" -- the ambiguity is reported via sg_cli_report_ambiguous_oid and
   the whole call fails, it is NOT reinterpreted as "maybe a pathspec".
   Real git agrees: `git diff --stat <amb>` prints the error:/hint: block
   rather than treating the string as a path. Getting this backwards is
   silent in the happy direction -- an ambiguous prefix would be reported as
   "no such path", which is both the wrong message and the wrong diagnosis. */
int sg_cli_split_revs_and_paths(const char *git_dir, char **pos, int n_pos, const char *cmd_name,
                                sg_rev_disambig disambig);

#endif
