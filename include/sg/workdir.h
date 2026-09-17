#ifndef SG_WORKDIR_H
#define SG_WORKDIR_H

#include <stddef.h>
#include <sys/stat.h>

#include "sg/hash.h"

#define SG_PATH_MAX 4096

/* The repo root is git_dir's parent directory (git_dir is ".../.git"). Result
   is malloc'd, caller frees. */
char *sg_repo_root(const char *git_dir);

/* Resolves a CLI-supplied path argument (relative to the current working
   directory, or absolute) into a repo-root-relative, '/'-separated path.
   Result is malloc'd. Returns NULL if the path falls outside repo_root or on
   allocation failure. */
char *sg_resolve_repo_path(const char *repo_root, const char *arg);

/* Like sg_resolve_repo_path, but an argument naming the repository root
   itself (e.g. "." run from the root) resolves to "" instead of being
   rejected, so commands that accept a directory argument can take the whole
   worktree. Result is malloc'd. Returns NULL if the path falls outside
   repo_root or on allocation failure. */
char *sg_resolve_repo_path_allow_root(const char *repo_root, const char *arg);

/* mkdir -p for every directory component leading up to (but not including)
   the file named by path. */
int sg_mkdir_parents(const char *path);

/* Slurps an entire file into a malloc'd buffer. Returns 0 on success, -1 if
   the file can't be opened or read. */
int sg_read_file(const char *path, unsigned char **out, size_t *out_len);

/* Writes data to path, creating any missing parent directories first (via
   sg_mkdir_parents, so it inherits that function's "EEXIST-without-S_ISDIR"
   looseness -- see sg_mkdir_parents's own callers for that), and chmod's the
   result to mode. Returns 0 on success, -1 on I/O failure.

   Phase 80: no longer used by any WORKING-TREE write in src/ -- every one of
   those goes through sg_write_file_worktree below instead, which adds the
   symlink-safety and blocker-replacement rules real git's own checkout
   follows. This function is kept only because it is still the tests'
   general-purpose "write this fixture file, creating its parent directories"
   helper (dozens of call sites across tests, none of them exercising or
   depending on the worktree-write security boundary sg_write_file_worktree
   exists for), and because a `.git`-side write (refs.c, reflog.c) still goes
   through plain sg_mkdir_parents + fopen directly, with no symlink concern
   below the gitdir root the way there is below the repo root. Do not add a
   new worktree call site for this function -- see
   docs/RULES-paths-strings.md. */
int sg_write_file_mkdirs(const char *path, const unsigned char *data, size_t len, int mode);

/* Phase 80 (F1): writes data as a WORKING-TREE file at repo_root/relpath,
   creating missing parent directories, and never writing through a symlink
   (or any other non-directory) sitting below repo_root. This is the single
   worktree-write primitive -- replaces the old sg_write_file_mkdirs at every
   worktree call site in src/ (a write under .git/ still goes through
   sg_mkdir_parents + fopen directly, unaffected by this).

   Every path component strictly BELOW repo_root is lstat'd (never stat'd):
   a real directory is traversed; a missing one is created (mkdir); anything
   else (symlink, regular file, fifo, ...) is NOT traversed, and the whole
   call fails (-1) -- a caller that needs to replace such a blocker clears it
   first (see sg_worktree_clear_write_path in sg/apply.h). Components AT OR
   ABOVE repo_root are never checked, so a repository reached through a
   symlinked ancestor (macOS's /tmp -> /private/tmp, or a symlinked cwd)
   keeps working.

   The final component: if lstat shows a symlink or a regular file, it is
   unlinked first, then created with O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW (so a
   symlink recreated by a racing process between the unlink and this open is
   refused, not followed) and chmod'd to mode. A directory sitting at the
   final component is a failure (-1) unless a caller already removed it.

   Symlink (mode 120000) blob content is unaffected: sg does not check out
   symlinks at all (a separate, future phase), so this function never
   receives one.

   Returns 0 on success, -1 on any failure. */
int sg_write_file_worktree(const char *repo_root, const char *relpath,
                          const unsigned char *data, size_t len, int mode);

/* Phase 80 (fix round, finding 1): removes a WORKING-TREE file or empty
   directory at repo_root/relpath -- the delete-side counterpart to
   sg_write_file_worktree above, and the single worktree-delete primitive:
   every worktree delete in src/ goes through this instead of a bare
   remove()/unlink()/rmdir() on a joined path (a delete under .git/ is
   unaffected, same carve-out as the write side).

   Every path component strictly BELOW repo_root is lstat'd, never stat'd:
   a real directory is traversed; a missing one means there is nothing here
   to remove (returns 0, not a failure); anything else (symlink, regular
   file, fifo, ...) fails the WHOLE call (-1) WITHOUT removing anything --
   this is what stops a symlinked ancestor from letting a plain remove()
   resolve straight through it and delete something OUTSIDE the
   repository, the delete-side twin of F1's write-side rule. Components AT
   OR ABOVE repo_root are never inspected. The final component itself is
   never traversed either way (remove() does not follow a symlink there).

   Returns 0 on success (including "there was nothing to remove"), -1 if a
   non-directory ancestor blocked the walk or the final remove() itself
   failed (e.g. a non-empty directory, or a permission error). */
int sg_remove_file_worktree(const char *repo_root, const char *relpath);

/* Non-zero if path exists and is a symlink (checked with lstat, so it isn't
   followed). */
int sg_is_symlink(const char *path);

/* Phase 81b: the worktree READ side's shared classification of what lstat
   finds at a repo-relative path, mirroring the tree/index mode space
   (100644/100755/120000) instead of collapsing a symlink into 100644 the
   way the pre-81b sg_diff.c workdir_entry_mode did. */
typedef enum {
    SG_WT_ABSENT = 0,  /* missing, or blocked by a non-directory ancestor */
    SG_WT_REGULAR,     /* S_ISREG -- mode_out is 100644 or 100755 */
    SG_WT_SYMLINK,     /* S_ISLNK -- mode_out is 120000 */
    SG_WT_OTHER        /* directory, fifo, socket, device, ... -- not a blob */
} sg_wt_kind;

/* Non-zero if a proper ancestor component of repo_root/relpath (strictly
   below repo_root) EXISTS but is not a real directory -- a symlink, a
   regular file, or any other non-directory blocker. This is the case a
   plain lstat()/stat() of the full path cannot detect on its own: when the
   blocking ancestor is a symlink to a real directory, ordinary path
   resolution silently follows it and reads through to whatever is really
   there (measured against git 2.55.0: git treats the tracked path as gone
   instead -- ORACLE.md X01-X03). A MISSING ancestor is not reported here
   (returns 0): a plain lstat of the full path already fails with ENOENT in
   that case, with no special handling needed. Shares the walk with
   sg_write_file_worktree/sg_remove_file_worktree (walk_worktree_ancestors in
   workdir.c, per docs/RULES-duplication.md); never creates or removes
   anything. relpath == "" (the repo root itself) is never blocked. */
int sg_worktree_ancestor_blocked(const char *repo_root, const char *relpath);

/* The tree/index mode formula for an already-lstat'd entry: S_ISREG ->
   100644/100755 (by the exec bit), S_ISLNK -> 120000, anything else ->
   SG_WT_OTHER with *mode_out left untouched. Factored out of
   sg_worktree_classify (docs/RULES-duplication.md) so a caller that must
   lstat the path itself first -- tree_build.c's sg_tree_build_from_workdir,
   which needs a single probe to decide BOTH which read to attempt and the
   mode to record, and cannot afford sg_worktree_classify's own internal
   lstat as a second, separate probe -- shares this exact formula instead of
   re-deriving it. */
sg_wt_kind sg_worktree_mode_from_stat(const struct stat *st, unsigned int *mode_out);

/* Classifies repo_root/relpath with lstat (never following a symlink),
   after first checking sg_worktree_ancestor_blocked -- a blocked ancestor
   makes the whole path SG_WT_ABSENT regardless of what lstat itself would
   say. For SG_WT_REGULAR/SG_WT_SYMLINK, *mode_out is set (100644/100755, or
   120000); left untouched otherwise. Does not read any content. */
sg_wt_kind sg_worktree_classify(const char *repo_root, const char *relpath,
                                unsigned int *mode_out);

/* readlink() with a buffer that grows until the target fits (never
   truncated, unlike a fixed PATH_MAX-sized buffer). abspath is an already-
   joined absolute path, never following a symlink itself (this IS the
   readlink). *data is malloc'd on success (caller frees). Returns 0 on
   success, -1 on failure (not a symlink, permission denied, ...). Exposed
   directly (not only via sg_worktree_read_entry below) for callers that
   need to attempt a symlink-specific read WITHOUT a preceding lstat --
   sg_tree_build_from_workdir is the one that needs this, to avoid turning
   an ordinary "file existed when probed, gone by the time of the read" race
   into a hard failure; see its own comment. */
int sg_worktree_readlink(const char *abspath, unsigned char **data, size_t *len_out);

/* Combines sg_worktree_classify with a content read: SG_WT_REGULAR reads the
   file's bytes, SG_WT_SYMLINK readlink()s it with a buffer that grows as
   needed (so a target longer than SG_PATH_MAX is read in full, never
   truncated -- this never routes through storage/chunk.c, a symlink target
   is always tiny). *data is malloc'd on success (caller frees) and *len_out
   is set; both are left as NULL/0 otherwise. "Exists but unreadable"
   (permission denied, a race with a delete, ...) is folded into
   SG_WT_ABSENT, matching sg_hash_file_blob's existing convention. Returns
   the classification actually achieved: SG_WT_REGULAR/SG_WT_SYMLINK only
   when *data was populated. */
sg_wt_kind sg_worktree_read_entry(const char *repo_root, const char *relpath,
                                  unsigned int *mode_out,
                                  unsigned char **data, size_t *len_out);

/* Like sg_hash_file_blob, but symlink-aware and ancestor-blocked-aware: the
   single worktree hashing primitive that replaces the
   workdir_entry_mode()+sg_hash_file_blob() pair at every worktree diff/
   status/restore/stash call site (docs/RULES-duplication.md -- one helper,
   not N copies of the stat+hash pair). Returns 0 and sets *mode_out/
   sha1_out on a regular file or symlink, -1 (leaving both untouched)
   otherwise -- absent, blocked, another type, or unreadable. */
int sg_worktree_hash_entry(const char *repo_root, const char *relpath,
                           unsigned int *mode_out,
                           unsigned char sha1_out[SG_SHA1_RAW_LEN]);

/* Like sg_read_file, but symlink-aware and ancestor-blocked-aware: used by
   sg_diff_side_read for an SG_DIFF_SIDE_WORKDIR side, so a patch against a
   worktree symlink diffs its target text instead of failing to open() it.
   Returns 0 and sets *data and *len_out on a regular file or symlink, -1
   otherwise (same collapsing convention as sg_worktree_hash_entry). */
int sg_worktree_read_blob(const char *repo_root, const char *relpath,
                          unsigned char **data, size_t *len_out);

/* Joins base and rel as "base/rel" into out, or copies whichever side is
   non-empty when the other is NULL/"". Returns 0, or -1 if the result does
   not fit: a truncated path usually still names a real entry higher up the
   tree, so every caller must treat -1 as "this path does not exist to me"
   and never act on the buffer. Replaces the per-file path_join copies in
   status.c/cmd_add.c. prune_empty_untracked_dirs in safety/stash.c keeps its
   own inline bounds check on purpose: it skips silently where this helper's
   callers report, an empty directory left behind being invisible to sg and
   to real git alike. */
int sg_path_join(char *out, size_t out_size, const char *base, const char *rel);

/* SHA-1 object id of path's content as a blob, without writing any object. */
int sg_hash_file_blob(const char *path, unsigned char sha1_out[SG_SHA1_RAW_LEN]);

/* After a tracked file has been removed, removes each ancestor directory
   that is now empty, up to but never including repo_root -- rmdir only, so
   a directory still holding anything at all (tracked or not) is left alone.
   relpath must stay under repo_root: an absolute path, or one with a ".."
   component, is refused outright rather than resolved, since these paths
   come from tree objects whose entry names sg does not validate.
   Best effort: every failure is ignored, because a leftover empty directory
   is invisible to sg status and to real git alike.

   Deliberately NOT ignore-aware, unlike safety/stash.c's
   prune_empty_untracked_dirs: real git (2.55.0, verified) removes a
   now-empty directory here even if that directory is itself covered by
   .gitignore, whereas the untracked-file sweep leaves an empty-but-ignored
   directory (e.g. "build/") alone. The two functions' rules are opposite by
   design -- do not "unify" them. */
void sg_prune_empty_parents(const char *repo_root, const char *relpath);

/* Whether a single path component -- a tree entry name, or one
   slash-separated piece of a repo-relative path -- may be turned into a
   working-tree path. Returns 1 if safe, 0 if not.

   Rejects: "", ".", "..", anything containing '/', and any case variant of
   ".git" including the trailing-'.'-or-space forms (".GIT", ".git.",
   ".git ") -- on a case-insensitive filesystem (the macOS default) ".GIT"
   names the very same directory, and real git refuses ".git." / ".git " on
   every platform, so accepting them would only ever produce a repository
   real git cannot check out.

   The case fold is ASCII-only and deliberately not strcasecmp: strcasecmp
   is locale-dependent and does not fold 'I' to 'i' under a Turkish locale,
   which would let ".GIT" through on exactly the filesystem this rule exists
   for.

   Also rejects a name that folds to ".git" once the code points HFS+
   ignores when comparing are dropped -- ".g<U+200C>it", "<U+FEFF>.git" and
   so on. The set is the sixteen git itself uses (U+200C..U+200F,
   U+202A..U+202E, U+206A..U+206F, U+FEFF), measured rather than guessed:
   U+200B, U+2060, U+00A0 and U+3000 are just as invisible and git ACCEPTS
   them, so this is a specific list and not "anything zero-width".

   Accepts ".gitignore", ".gitmodules", "..a", "a.." -- the comparison is on
   the whole component, never a prefix or substring match. Control characters
   are accepted: real git accepts them in tree entries and defends at the
   display layer instead (measured; core.quotePath does NOT turn that off).

   This is the wrong predicate for deciding whether a directory encountered
   while WALKING the working tree is the gitdir: git lists ".git." as an
   untracked directory, so skipping everything this rejects would
   under-report. Compare against ".git" exactly for that.

   NOT applied when sg parses a tree object: like git, sg can read and print
   a hostile tree (`sg cat-file -p`), it just refuses to turn one into
   files. */
int sg_path_component_is_safe(const char *name);

/* Whether an already-assembled repo-relative path may be turned into a
   working-tree path: non-empty, not absolute, no empty component ("a//b",
   "a/"), and every component passes sg_path_component_is_safe.
   For the two sources that arrive assembled rather than one component at a
   time -- .git/index entries and command-line arguments. */
int sg_relpath_is_safe(const char *relpath);

#endif
