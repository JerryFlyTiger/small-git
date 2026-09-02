#include "sg/pick.h"

#include "sg/apply.h"
#include "sg/hash.h"
#include "sg/index.h"
#include "sg/loose.h"
#include "sg/merge.h"
#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/quote.h"
#include "sg/rebase.h"
#include "sg/refs.h"
#include "sg/similarity.h"
#include "sg/snapshot.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Ninth byte-for-byte copy of this helper (CLAUDE.md's known-duplication
   list already had eight: reflog.c, chunk.c, safety/stash.c, snapshot.c,
   cmd_rebase.c, cmd_merge.c, cmd_tag.c, cmd_commit.c). Kept local rather
   than shared, same call as those eight -- see the CLAUDE.md edit that
   accompanies this phase. */
static const char *env_or(const char *name, const char *fallback)
{
    const char *v = getenv(name);

    return (v != NULL && v[0] != '\0') ? v : fallback;
}

static const char *op_name(sg_seq_kind kind)
{
    return kind == SG_SEQ_CHERRY_PICK ? "cherry-pick" : "revert";
}

static void short_hex(const unsigned char id[SG_SHA1_RAW_LEN], char out[8])
{
    char hex[SG_SHA1_HEX_LEN + 1];

    sg_sha1_to_hex(id, hex);
    memcpy(out, hex, 7);
    out[7] = '\0';
}

static char *first_line_dup(const char *message)
{
    const char *end = strchr(message, '\n');
    size_t len = end != NULL ? (size_t)(end - message) : strlen(message);
    char *out = malloc(len + 1);

    if (out == NULL)
        return NULL;
    memcpy(out, message, len);
    out[len] = '\0';
    return out;
}

/* "<prefix><subject>" heap allocation, same idiom as cmd_rebase.c's
   reflog_msg_with_subject -- a subject long enough to truncate a fixed
   buffer would silently produce a different reflog line than git's. */
static char *msg_with_subject(const char *prefix, const char *message)
{
    size_t subject_len = strcspn(message, "\n");
    int need = snprintf(NULL, 0, "%s%.*s", prefix, (int)subject_len, message);
    char *out;

    if (need < 0)
        return NULL;
    out = malloc((size_t)need + 1);
    if (out == NULL)
        return NULL;
    snprintf(out, (size_t)need + 1, "%s%.*s", prefix, (int)subject_len, message);
    return out;
}

/* ==================== revert's message (Phase 57 spec section 4) ==================== */

/* Section 4.3's "Reapply" rule: if `subject` begins with the literal 8
   bytes `Revert "`, that prefix is replaced by `Reapply "` and the rest is
   kept verbatim; otherwise the subject is wrapped as `Revert "<subject>"`.
   No closing-quote check, no balance check -- measured over 7 shapes,
   including 4 negatives (see tests/test_pick_engine.c / DESIGN.md). *out is
   malloc'd, caller frees; NULL only on OOM. */
static char *build_revert_subject_line(const char *subject)
{
    static const char reapply_prefix[] = "Revert \"";
    size_t plen = sizeof(reapply_prefix) - 1;
    char *out;

    if (strncmp(subject, reapply_prefix, plen) == 0) {
        const char *rest = subject + plen;
        size_t need = 9 /* "Reapply \"" */ + strlen(rest);

        out = malloc(need + 1);
        if (out == NULL)
            return NULL;
        snprintf(out, need + 1, "Reapply \"%s", rest);
        return out;
    }

    {
        size_t need = 8 /* "Revert \"" */ + strlen(subject) + 1 /* closing quote */;

        out = malloc(need + 1);
        if (out == NULL)
            return NULL;
        snprintf(out, need + 1, "Revert \"%s\"", subject);
        return out;
    }
}

/* Builds the revert commit message (spec 4.1/4.2), then applies
   sg_message_cleanup (this is sg constructing a NEW message, unlike
   cherry-pick's byte-for-byte copy). *out is malloc'd, caller frees.
   parent_hex is only used when mainline_hex != NULL (the -m <n>, merge
   case, spec 4.2). Returns 0 on success, -1 on OOM. */
static int build_revert_message(const unsigned char commit_id[SG_SHA1_RAW_LEN],
                                const char *orig_message,
                                const unsigned char parent_id[SG_SHA1_RAW_LEN], int has_mainline,
                                char **out)
{
    char *subject = first_line_dup(orig_message);
    char *subject_line;
    char commit_hex[SG_SHA1_HEX_LEN + 1];
    char parent_hex[SG_SHA1_HEX_LEN + 1];
    char *raw;
    int rc;

    if (subject == NULL)
        return -1;
    subject_line = build_revert_subject_line(subject);
    free(subject);
    if (subject_line == NULL)
        return -1;

    sg_sha1_to_hex(commit_id, commit_hex);

    if (has_mainline) {
        int need;

        sg_sha1_to_hex(parent_id, parent_hex);
        need = snprintf(NULL, 0, "%s\n\nThis reverts commit %s, reversing\nchanges made to %s.\n",
                        subject_line, commit_hex, parent_hex);
        if (need < 0) {
            free(subject_line);
            return -1;
        }
        raw = malloc((size_t)need + 1);
        if (raw == NULL) {
            free(subject_line);
            return -1;
        }
        snprintf(raw, (size_t)need + 1, "%s\n\nThis reverts commit %s, reversing\nchanges made to %s.\n",
                subject_line, commit_hex, parent_hex);
    } else {
        int need = snprintf(NULL, 0, "%s\n\nThis reverts commit %s.\n", subject_line, commit_hex);

        if (need < 0) {
            free(subject_line);
            return -1;
        }
        raw = malloc((size_t)need + 1);
        if (raw == NULL) {
            free(subject_line);
            return -1;
        }
        snprintf(raw, (size_t)need + 1, "%s\n\nThis reverts commit %s.\n", subject_line, commit_hex);
    }
    free(subject_line);

    rc = sg_message_cleanup(raw, out);
    free(raw);
    return rc;
}

/* ==================== parent selection (spec 3.1) ==================== */

static int select_parent(sg_seq_kind kind, const unsigned char commit_id[SG_SHA1_RAW_LEN],
                         const sg_commit *commit, int mainline,
                         unsigned char parent_out[SG_SHA1_RAW_LEN])
{
    char hex[SG_SHA1_HEX_LEN + 1];

    sg_sha1_to_hex(commit_id, hex);

    if (commit->parent_count == 0) {
        fprintf(stderr, "sg: cannot %s a root commit\n",
               kind == SG_SEQ_CHERRY_PICK ? "cherry-pick" : "revert");
        return -1;
    }

    if (mainline > 0) {
        if (commit->parent_count <= 1 || (size_t)mainline > commit->parent_count) {
            fprintf(stderr, "sg: commit %s does not have parent %d\n", hex, mainline);
            return -1;
        }
        memcpy(parent_out, commit->parents[mainline - 1], SG_SHA1_RAW_LEN);
        return 0;
    }

    if (commit->parent_count != 1) {
        fprintf(stderr, "sg: commit %s is a merge but no -m option was given\n", hex);
        return -1;
    }
    memcpy(parent_out, commit->parents[0], SG_SHA1_RAW_LEN);
    return 0;
}

/* ==================== conflict/empty messages ==================== */

static void print_conflict_message(sg_seq_kind kind, const unsigned char commit_id[SG_SHA1_RAW_LEN],
                                   const char *commit_message, char **conflict_paths,
                                   size_t conflict_count)
{
    const char *op = op_name(kind);
    char short_sha[8];
    char *summary = first_line_dup(commit_message);
    size_t i;

    short_hex(commit_id, short_sha);
    fprintf(stderr, "sg: could not apply %s %s\n", short_sha, summary != NULL ? summary : "");
    free(summary);
    fprintf(stderr, "The following files have conflicts:\n");
    for (i = 0; i < conflict_count; i++)
        fprintf(stderr, "    %s\n", sg_quote_path(conflict_paths[i]));
    fprintf(stderr,
           "After resolving conflicts:\n"
           "  sg add <file>...        mark as resolved\n"
           "  sg %s --continue      continue the %s\n"
           "Or:\n"
           "  sg %s --skip          skip this commit\n"
           "  sg %s --abort         give up and return to the original state\n",
           op, op, op, op);
}

static void print_empty_message(sg_seq_kind kind, const unsigned char commit_id[SG_SHA1_RAW_LEN],
                                const char *commit_message)
{
    const char *op = op_name(kind);
    char short_sha[8];
    char *summary = first_line_dup(commit_message);

    short_hex(commit_id, short_sha);
    fprintf(stderr, "sg: %s %s\n", short_sha, summary != NULL ? summary : "");
    fprintf(stderr, "sg: this change is already present; nothing to commit\n");
    fprintf(stderr, "sg: use `sg %s --skip` to skip this commit, or `sg %s --abort` to cancel\n",
           op, op);
    free(summary);
}

/* ==================== one pick (spec 3.1/3.2) ==================== */

typedef enum {
    ATTEMPT_CLEAN,          /* committed (or, if opts->no_commit, staged only) */
    ATTEMPT_CLEAN_NO_COMMIT, /* -n: staged, nothing committed, nothing to continue */
    ATTEMPT_EMPTY,
    ATTEMPT_CONFLICT,
    ATTEMPT_ERROR
} attempt_rc;

typedef struct {
    unsigned char new_commit_id[SG_SHA1_RAW_LEN]; /* valid iff ATTEMPT_CLEAN */
    char *message;                                 /* malloc'd: what the commit's message is/would be */
    char **conflict_paths;                          /* malloc'd array of malloc'd paths, valid iff ATTEMPT_CONFLICT */
    size_t conflict_count;
} attempt_result;

static void attempt_result_free(attempt_result *r)
{
    size_t i;

    free(r->message);
    r->message = NULL;
    for (i = 0; i < r->conflict_count; i++)
        free(r->conflict_paths[i]);
    free(r->conflict_paths);
    r->conflict_paths = NULL;
    r->conflict_count = 0;
}

static attempt_rc attempt_one(const char *git_dir, const char *repo_root, sg_seq_kind kind,
                              const unsigned char commit_id[SG_SHA1_RAW_LEN], int mainline,
                              int no_commit, attempt_result *out)
{
    unsigned char head_commit[SG_SHA1_RAW_LEN];
    unsigned char ours_tree[SG_SHA1_RAW_LEN];
    unsigned char parent_id[SG_SHA1_RAW_LEN];
    unsigned char parent_tree[SG_SHA1_RAW_LEN];
    unsigned char base_tree[SG_SHA1_RAW_LEN];
    unsigned char theirs_tree[SG_SHA1_RAW_LEN];
    sg_obj_type type;
    unsigned char *content = NULL;
    size_t content_len;
    sg_commit commit;
    sg_merge_result result;
    sg_index new_idx;
    char **conflict_paths = NULL;
    size_t conflict_count = 0;
    char short_sha[8];
    char theirs_label[300];
    attempt_rc rc = ATTEMPT_ERROR;
    size_t i;

    memset(out, 0, sizeof(*out));

    if (sg_ref_resolve_head(git_dir, head_commit) != 0)
        return ATTEMPT_ERROR;
    if (sg_object_read(git_dir, commit_id, &type, &content, &content_len) != 0 || type != SG_OBJ_COMMIT)
        return ATTEMPT_ERROR;
    if (sg_commit_parse(content, content_len, &commit) != 0) {
        free(content);
        return ATTEMPT_ERROR;
    }
    free(content);

    if (select_parent(kind, commit_id, &commit, mainline, parent_id) != 0) {
        sg_commit_free(&commit);
        return ATTEMPT_ERROR;
    }

    if (sg_commit_tree_of(git_dir, head_commit, ours_tree) != 0 ||
       sg_commit_tree_of(git_dir, parent_id, parent_tree) != 0) {
        sg_commit_free(&commit);
        return ATTEMPT_ERROR;
    }

    if (kind == SG_SEQ_CHERRY_PICK) {
        memcpy(base_tree, parent_tree, SG_SHA1_RAW_LEN);
        memcpy(theirs_tree, commit.tree, SG_SHA1_RAW_LEN);
        out->message = strdup(commit.message);
        if (out->message == NULL) {
            sg_commit_free(&commit);
            return ATTEMPT_ERROR;
        }
    } else {
        memcpy(base_tree, commit.tree, SG_SHA1_RAW_LEN);
        memcpy(theirs_tree, parent_tree, SG_SHA1_RAW_LEN);
        if (build_revert_message(commit_id, commit.message, parent_id, mainline > 0, &out->message) !=
           0) {
            sg_commit_free(&commit);
            return ATTEMPT_ERROR;
        }
    }

    short_hex(commit_id, short_sha);
    {
        char *summary = first_line_dup(commit.message);

        snprintf(theirs_label, sizeof(theirs_label), "%s (%s)", short_sha,
                summary != NULL ? summary : "");
        free(summary);
    }

    if (sg_merge_trees(git_dir, base_tree, ours_tree, theirs_tree, "HEAD", theirs_label,
                       SG_SIMILARITY_DEFAULT, &result) != 0) {
        sg_commit_free(&commit);
        attempt_result_free(out);
        return ATTEMPT_ERROR;
    }

    if (sg_merge_result_apply(git_dir, repo_root, &result, &new_idx, &conflict_paths,
                              &conflict_count) != 0) {
        rc = ATTEMPT_ERROR;
        goto done;
    }

    if (conflict_count > 0) {
        if (sg_index_write(git_dir, &new_idx) != 0) {
            fprintf(stderr, "sg: failed to write index\n");
            rc = ATTEMPT_ERROR;
            goto done;
        }
        out->conflict_paths = conflict_paths;
        out->conflict_count = conflict_count;
        conflict_paths = NULL;
        conflict_count = 0;
        rc = ATTEMPT_CONFLICT;
        goto done;
    }

    {
        unsigned char merged_tree[SG_SHA1_RAW_LEN];

        if (sg_tree_build_from_index(git_dir, &new_idx, merged_tree) != 0) {
            fprintf(stderr, "sg: failed to build tree while applying %s\n", short_sha);
            rc = ATTEMPT_ERROR;
            goto done;
        }

        if (memcmp(merged_tree, ours_tree, SG_SHA1_RAW_LEN) == 0) {
            rc = ATTEMPT_EMPTY;
            goto done;
        }

        if (sg_index_write(git_dir, &new_idx) != 0) {
            fprintf(stderr, "sg: failed to write index\n");
            rc = ATTEMPT_ERROR;
            goto done;
        }

        if (no_commit) {
            rc = ATTEMPT_CLEAN_NO_COMMIT;
            goto done;
        }

        {
            sg_commit new_commit;
            unsigned char *serialized;
            size_t serialized_len;
            const char *committer_name = env_or("GIT_AUTHOR_NAME", "small_git");
            const char *committer_email = env_or("GIT_AUTHOR_EMAIL", "sg@localhost");

            memset(&new_commit, 0, sizeof(new_commit));
            memcpy(new_commit.tree, merged_tree, SG_SHA1_RAW_LEN);
            new_commit.parents = malloc(sizeof(*new_commit.parents));
            if (new_commit.parents == NULL) {
                fprintf(stderr, "sg: out of memory\n");
                rc = ATTEMPT_ERROR;
                goto done;
            }
            memcpy(new_commit.parents[0], head_commit, SG_SHA1_RAW_LEN);
            new_commit.parent_count = 1;

            if (kind == SG_SEQ_CHERRY_PICK) {
                new_commit.author_name = commit.author_name;
                new_commit.author_email = commit.author_email;
                new_commit.author_time = commit.author_time;
                memcpy(new_commit.author_tz, commit.author_tz, sizeof(new_commit.author_tz));
            } else {
                new_commit.author_name = (char *)committer_name;
                new_commit.author_email = (char *)committer_email;
                new_commit.author_time = (long long)time(NULL);
                strcpy(new_commit.author_tz, "+0000");
            }
            new_commit.committer_name = (char *)committer_name;
            new_commit.committer_email = (char *)committer_email;
            new_commit.committer_time = (long long)time(NULL);
            strcpy(new_commit.committer_tz, "+0000");
            new_commit.message = out->message;

            if (sg_commit_serialize(&new_commit, &serialized, &serialized_len) != 0) {
                fprintf(stderr, "sg: failed to serialize commit\n");
                free(new_commit.parents);
                rc = ATTEMPT_ERROR;
                goto done;
            }
            free(new_commit.parents);

            if (sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len,
                               out->new_commit_id) != 0) {
                fprintf(stderr, "sg: failed to write commit object\n");
                free(serialized);
                rc = ATTEMPT_ERROR;
                goto done;
            }
            free(serialized);
            rc = ATTEMPT_CLEAN;
        }
    }

done:
    sg_merge_result_free(&result);
    for (i = 0; i < conflict_count; i++)
        free(conflict_paths[i]);
    free(conflict_paths);
    sg_commit_free(&commit);
    sg_index_free(&new_idx);
    if (rc == ATTEMPT_ERROR)
        attempt_result_free(out);
    return rc;
}

/* Reads MERGE_MSG and strips the "\n# Conflicts:\n..." tail it may carry
   (spec section 2.2's format), recovering the plain message that was used
   to build it -- this is how --continue gets back the exact message
   attempt_one already computed at stop time, without needing to persist
   -m anywhere (sequencer/opts is deliberately never written, see
   sequencer.h). *out is malloc'd, caller frees. Returns 0 on success, -1 on
   I/O failure. */
static int read_message_from_merge_msg(const char *git_dir, char **out)
{
    char path[SG_PATH_MAX];
    unsigned char *data;
    size_t len;
    char *marker;

    if ((size_t)snprintf(path, sizeof(path), "%s/MERGE_MSG", git_dir) >= sizeof(path))
        return -1;
    if (sg_read_file(path, &data, &len) != 0)
        return -1;

    *out = malloc(len + 1);
    if (*out == NULL) {
        free(data);
        return -1;
    }
    memcpy(*out, data, len);
    (*out)[len] = '\0';
    free(data);

    /* marker points at the BLANK SEPARATOR line itself (spec 2.2's format is
       "<message ending in exactly one \n>\n# Conflicts:\n..." -- that extra
       "\n" is the separator, not part of the message). Truncating AT marker,
       not one byte past it, drops the separator along with everything after
       it, leaving exactly the original message with its own single trailing
       newline intact. Truncating one byte later (keeping marker[0]) was the
       bug a review caught: it left the separator attached, so a resumed
       cherry-pick's commit message came out with an extra trailing blank
       line and therefore a different object id than git's. */
    marker = strstr(*out, "\n# Conflicts:\n");
    if (marker != NULL)
        *marker = '\0';
    return 0;
}

/* ==================== stopping: writes CHERRY_PICK_HEAD/REVERT_HEAD, MERGE_MSG, sequencer/ ==================== */

/* Writes just the sequencer STATE (CHERRY_PICK_HEAD/REVERT_HEAD +, iff
   has_sequence, sequencer/{head,abort-safety,todo}) -- not MERGE_MSG. This
   is the half every exit path from the todo loop needs, including an
   internal ERROR (see run_todo's ATTEMPT_ERROR branch): abort_safety is
   always re-read fresh from the CURRENT real HEAD right here, which is
   the whole point -- a review found that leaving a stale on-disk
   abort-safety behind (from an EARLIER stop, now that later todo entries
   have advanced HEAD further) makes --abort wrongly refuse with "HEAD has
   moved since the pick stopped", even though the pick's own machinery is
   what moved it. The invariant this function exists to hold: every path
   out of the todo loop that isn't "finished cleanly" leaves abort_safety
   on disk equal to HEAD's real value at that exact moment. */
static int write_stop_state(const char *git_dir, sg_seq_kind kind,
                            const unsigned char commit_id[SG_SHA1_RAW_LEN],
                            const unsigned char orig_head[SG_SHA1_RAW_LEN], int has_sequence,
                            const unsigned char (*remaining_todo)[SG_SHA1_RAW_LEN],
                            size_t remaining_count)
{
    sg_sequencer_state st;
    unsigned char abort_safety[SG_SHA1_RAW_LEN];

    if (sg_ref_resolve_head(git_dir, abort_safety) != 0)
        return -1;

    memset(&st, 0, sizeof(st));
    st.kind = kind;
    memcpy(st.current, commit_id, SG_SHA1_RAW_LEN);
    st.has_sequence = has_sequence;
    if (has_sequence) {
        memcpy(st.orig_head, orig_head, SG_SHA1_RAW_LEN);
        memcpy(st.abort_safety, abort_safety, SG_SHA1_RAW_LEN);
        st.todo = (unsigned char (*)[SG_SHA1_RAW_LEN])remaining_todo;
        st.todo_count = remaining_count;
    }

    return sg_sequencer_state_write(git_dir, &st);
}

static int write_stop(const char *git_dir, sg_seq_kind kind, const unsigned char commit_id[SG_SHA1_RAW_LEN],
                      const unsigned char orig_head[SG_SHA1_RAW_LEN], int has_sequence,
                      const unsigned char (*remaining_todo)[SG_SHA1_RAW_LEN], size_t remaining_count,
                      const char *message, char **conflict_paths, size_t conflict_count)
{
    if (write_stop_state(git_dir, kind, commit_id, orig_head, has_sequence, remaining_todo,
                         remaining_count) != 0)
        return -1;
    if (sg_sequencer_write_merge_msg(git_dir, message, conflict_paths, conflict_count) != 0)
        return -1;
    return 0;
}

/* ==================== the todo loop (spec 3.4) ==================== */

/* Runs todo[0..todo_count-1] one commit at a time. `resumed` is 1 exactly
   when the FIRST entry finishes via a --continue (spec 3.3's asymmetric
   "commit (cherry-pick)" / "commit" reflog wording); every later entry in
   the same call, and every entry when resumed is 0, uses the ordinary
   "cherry-pick: .../revert: ..." wording. orig_head/has_sequence describe
   the run as a whole (fixed at the very start, never per-step). Returns 0
   if the whole sequence finished, 1 if it stopped (message already
   printed, state already written) or a gate/arg error was already
   reported. */
static int run_todo(const char *git_dir, const char *repo_root, sg_seq_kind kind,
                    const char *current_branch, const unsigned char orig_head[SG_SHA1_RAW_LEN],
                    int has_sequence, unsigned char (*todo)[SG_SHA1_RAW_LEN], size_t todo_count,
                    int mainline, int no_commit, int resumed)
{
    size_t idx;
    const char *op = op_name(kind);

    for (idx = 0; idx < todo_count; idx++) {
        attempt_result out;
        attempt_rc rc = attempt_one(git_dir, repo_root, kind, todo[idx], mainline,
                                    no_commit && idx == 0, &out);

        if (rc == ATTEMPT_ERROR) {
            /* An internal error (I/O, OOM, corrupt object) part way
               through a multi-commit sequence -- HEAD may already have
               advanced past whatever abort_safety was written at the
               ORIGINAL stop (or, resuming via --continue/--skip, at the
               start of this very call), because earlier todo entries in
               THIS run already committed cleanly before this one failed.
               Refresh the on-disk state so abort_safety matches reality;
               without this, --abort's "has HEAD moved" check compares
               against a stale value and wrongly refuses, and --continue
               would then misread the (also stale) recorded commit's tree
               against the NEW HEAD and could misjudge an ordinary pick as
               already-empty. has_sequence is always true whenever this
               branch is reachable with HEAD having moved (a single-commit,
               has_sequence==0 pick's only ATTEMPT_ERROR is at idx==0,
               before anything in THIS call could have moved HEAD), but the
               check is kept explicit rather than assumed. */
            if (has_sequence &&
               write_stop_state(git_dir, kind, todo[idx], orig_head, has_sequence, todo + idx,
                                todo_count - idx) != 0)
                fprintf(stderr, "sg: warning: cannot update %s state after the error\n", op);
            fprintf(stderr, "sg: an error occurred while running %s\n", op);
            free(todo);
            return 1;
        }

        if (rc == ATTEMPT_CONFLICT || rc == ATTEMPT_EMPTY) {
            if (write_stop(git_dir, kind, todo[idx], orig_head, has_sequence, todo + idx,
                           todo_count - idx, out.message, out.conflict_paths,
                           out.conflict_count) != 0)
                fprintf(stderr, "sg: warning: cannot write %s state\n", op);

            if (rc == ATTEMPT_CONFLICT)
                print_conflict_message(kind, todo[idx], out.message, out.conflict_paths,
                                       out.conflict_count);
            else
                print_empty_message(kind, todo[idx], out.message);

            attempt_result_free(&out);
            free(todo);
            return 1;
        }

        if (rc == ATTEMPT_CLEAN_NO_COMMIT) {
            /* Measured: `git cherry-pick -n <clean commit>` leaves
               MERGE_MSG behind (just the message, no "# Conflicts:" block
               -- there was no conflict) while writing NEITHER
               CHERRY_PICK_HEAD nor sequencer/, so it does not count as "in
               progress" (sg_sequencer_kind_in_progress must still answer
               0). An ordinary clean pick with no -n writes no MERGE_MSG at
               all. sg_sequencer_write_merge_msg alone reproduces exactly
               this: it is the MERGE_MSG writer with no accompanying
               sg_sequencer_state_write call. */
            if (sg_sequencer_write_merge_msg(git_dir, out.message, NULL, 0) != 0)
                fprintf(stderr, "sg: warning: cannot write MERGE_MSG\n");
            attempt_result_free(&out);
            free(todo);
            return 0;
        }

        /* ATTEMPT_CLEAN: move the branch (or HEAD, if detached) one commit
           forward and log it. */
        {
            const char *prefix;
            char *reflog_msg;

            if (idx == 0 && resumed)
                prefix = kind == SG_SEQ_CHERRY_PICK ? "commit (cherry-pick): " : "commit: ";
            else
                prefix = kind == SG_SEQ_CHERRY_PICK ? "cherry-pick: " : "revert: ";

            reflog_msg = msg_with_subject(prefix, out.message);
            if (reflog_msg == NULL) {
                fprintf(stderr, "sg: out of memory\n");
                attempt_result_free(&out);
                free(todo);
                return 1;
            }
            if (sg_ref_move_head(git_dir, current_branch, out.new_commit_id, reflog_msg) != 0) {
                fprintf(stderr, "sg: failed to update HEAD\n");
                free(reflog_msg);
                attempt_result_free(&out);
                free(todo);
                return 1;
            }
            free(reflog_msg);
        }
        attempt_result_free(&out);
    }

    free(todo);
    if (has_sequence && sg_sequencer_state_remove(git_dir) != 0)
        fprintf(stderr, "sg: warning: could not remove %s state\n", op);
    else if (!has_sequence)
        sg_sequencer_state_remove(git_dir); /* harmless if nothing is there */

    printf("Successfully %s.\n", kind == SG_SEQ_CHERRY_PICK ? "cherry-picked" : "reverted");
    return 0;
}

/* ==================== sg_pick_start ==================== */

int sg_pick_start(const char *git_dir, const char *repo_root, sg_seq_kind kind,
                  unsigned char (*commits)[SG_SHA1_RAW_LEN], size_t count, const sg_pick_opts *opts)
{
    const char *op = op_name(kind);
    unsigned char orig_head[SG_SHA1_RAW_LEN];
    char *current_branch;
    unsigned char(*todo)[SG_SHA1_RAW_LEN];

    if (sg_rebase_state_exists(git_dir)) {
        fprintf(stderr,
               "sg: a rebase is currently in progress, cannot %s\n"
               "Finish it first (sg rebase --continue) or run sg rebase --abort to give up\n",
               op);
        return 1;
    }
    if (sg_merge_head_exists(git_dir)) {
        fprintf(stderr,
               "sg: an unfinished merge is in progress, cannot %s\n"
               "Finish it first, or run sg merge --abort to give up\n",
               op);
        return 1;
    }
    {
        sg_seq_kind existing = sg_sequencer_kind_in_progress(git_dir);

        if (existing != 0) {
            fprintf(stderr, "sg: a %s is already in progress\n", op_name(existing));
            return 1;
        }
    }
    {
        char what[32];

        snprintf(what, sizeof(what), "sg %s", op);
        if (sg_require_clean_workdir(git_dir, repo_root, what) != 0)
            return 1;
    }

    if (opts->no_commit && count > 1) {
        fprintf(stderr, "sg: -n is only supported with a single commit\n");
        return 1;
    }

    /* A review found this the hard way: -m's value is never persisted
       anywhere (sequencer/opts is deliberately never written, same
       reasoning as -n above), so a merge commit picked with -m that
       conflicts and later resumes via --continue/--skip has no way to
       recover which parent the user meant for any OTHER merge commit
       still left in the todo -- run_todo's continue/skip call sites pass
       mainline=0, and a later merge commit in the SAME multi-commit
       sequence would then be refused with "is a merge but no -m option
       was given" even though the user did give one, just for a different
       commit. Rejecting -m outright whenever more than one commit is
       requested closes this the same way -n's own gap was closed: a
       single-commit -m pick never re-enters run_todo through
       --continue/--skip (state.todo_count is 1, handled without a
       run_todo call at all -- see sg_pick_continue/sg_pick_skip), so the
       hardcoded mainline=0 those two pass is provably unreachable once
       this holds, not merely unlikely. See docs/DESIGN.md's Phase 57
       section for the full writeup of why this is a scope limitation, not
       an approximation. */
    if (opts->mainline > 0 && count > 1) {
        fprintf(stderr, "sg: -m is only supported with a single commit\n");
        return 1;
    }

    if (sg_ref_resolve_head(git_dir, orig_head) != 0) {
        fprintf(stderr, "sg: the current branch has no commits, cannot %s\n", op);
        return 1;
    }
    current_branch = sg_ref_current_branch(git_dir);
    if (current_branch == NULL && sg_ref_head_is_detached(git_dir) != 1) {
        fprintf(stderr, "sg: cannot read HEAD (.git/HEAD is neither a branch nor a commit id)\n");
        return 1;
    }

    todo = malloc(count * sizeof(*todo));
    if (todo == NULL) {
        fprintf(stderr, "sg: out of memory\n");
        free(current_branch);
        return 1;
    }
    memcpy(todo, commits, count * sizeof(*todo));

    {
        int rc = run_todo(git_dir, repo_root, kind, current_branch, orig_head, count > 1, todo,
                          count, opts->mainline, opts->no_commit, 0);

        free(current_branch);
        return rc;
    }
}

/* ==================== --continue/--skip/--abort/--quit ==================== */

static int require_state(const char *git_dir, sg_seq_kind kind, sg_sequencer_state *out)
{
    sg_seq_kind actual = sg_sequencer_kind_in_progress(git_dir);

    if (actual == 0) {
        fprintf(stderr, "sg: no %s in progress\n", op_name(kind));
        return -1;
    }
    if (actual != kind) {
        fprintf(stderr, "sg: no %s in progress (a %s is in progress; run `sg %s --continue` "
                        "instead)\n",
               op_name(kind), op_name(actual), op_name(actual));
        return -1;
    }
    if (sg_sequencer_state_read(git_dir, out) != 0) {
        fprintf(stderr, "sg: %s state is corrupt, run sg %s --abort to clean up\n", op_name(kind),
               op_name(kind));
        return -1;
    }
    return 0;
}

static void print_unmerged_paths(sg_seq_kind kind, const sg_index *idx)
{
    size_t i;

    fprintf(stderr, "sg: unresolved conflicts remain, cannot continue the %s:\n", op_name(kind));
    for (i = 0; i < idx->count; i++) {
        if (idx->entries[i].stage == 0)
            continue;
        if (i > 0 && strcmp(idx->entries[i].path, idx->entries[i - 1].path) == 0)
            continue;
        fprintf(stderr, "\t%s\n", sg_quote_path(idx->entries[i].path));
    }
    fprintf(stderr,
           "Please resolve conflicts and run `sg add <file>...` to mark them resolved, then run "
           "`sg %s --continue` again.\n",
           op_name(kind));
}

int sg_pick_continue(const char *git_dir, const char *repo_root, sg_seq_kind kind)
{
    sg_sequencer_state state;
    sg_index idx;
    unsigned char head_commit[SG_SHA1_RAW_LEN];
    unsigned char ours_tree[SG_SHA1_RAW_LEN];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    sg_obj_type type;
    unsigned char *content = NULL;
    size_t content_len;
    sg_commit orig_commit;
    char *message = NULL;
    int rc;

    if (require_state(git_dir, kind, &state) != 0)
        return 1;

    if (sg_index_read(git_dir, &idx) != 0) {
        fprintf(stderr, "sg: failed to read index (corrupt?)\n");
        sg_sequencer_state_free(&state);
        return 1;
    }
    if (sg_index_has_unmerged(&idx)) {
        print_unmerged_paths(kind, &idx);
        sg_index_free(&idx);
        sg_sequencer_state_free(&state);
        return 1;
    }

    if (sg_ref_resolve_head(git_dir, head_commit) != 0 ||
       sg_commit_tree_of(git_dir, head_commit, ours_tree) != 0) {
        fprintf(stderr, "sg: cannot read the current branch\n");
        sg_index_free(&idx);
        sg_sequencer_state_free(&state);
        return 1;
    }
    if (sg_tree_build_from_index(git_dir, &idx, tree_id) != 0) {
        fprintf(stderr, "sg: failed to build tree from index\n");
        sg_index_free(&idx);
        sg_sequencer_state_free(&state);
        return 1;
    }
    sg_index_free(&idx);

    if (sg_object_read(git_dir, state.current, &type, &content, &content_len) != 0 ||
       type != SG_OBJ_COMMIT || sg_commit_parse(content, content_len, &orig_commit) != 0) {
        free(content);
        fprintf(stderr, "sg: cannot read the original commit\n");
        sg_sequencer_state_free(&state);
        return 1;
    }
    free(content);

    if (memcmp(tree_id, ours_tree, SG_SHA1_RAW_LEN) == 0) {
        print_empty_message(kind, state.current, orig_commit.message);
        fprintf(stderr, "sg: use `sg %s --skip` to skip this commit\n", op_name(kind));
        sg_commit_free(&orig_commit);
        sg_sequencer_state_free(&state);
        return 1;
    }

    if (read_message_from_merge_msg(git_dir, &message) != 0) {
        fprintf(stderr, "sg: cannot read %s/MERGE_MSG\n", git_dir);
        sg_commit_free(&orig_commit);
        sg_sequencer_state_free(&state);
        return 1;
    }

    {
        char *current_branch = sg_ref_current_branch(git_dir);
        sg_commit new_commit;
        unsigned char *serialized;
        size_t serialized_len;
        unsigned char new_commit_id[SG_SHA1_RAW_LEN];
        const char *committer_name = env_or("GIT_AUTHOR_NAME", "small_git");
        const char *committer_email = env_or("GIT_AUTHOR_EMAIL", "sg@localhost");
        char *reflog_msg;

        memset(&new_commit, 0, sizeof(new_commit));
        memcpy(new_commit.tree, tree_id, SG_SHA1_RAW_LEN);
        new_commit.parents = malloc(sizeof(*new_commit.parents));
        if (new_commit.parents == NULL) {
            fprintf(stderr, "sg: out of memory\n");
            free(current_branch);
            free(message);
            sg_commit_free(&orig_commit);
            sg_sequencer_state_free(&state);
            return 1;
        }
        memcpy(new_commit.parents[0], head_commit, SG_SHA1_RAW_LEN);
        new_commit.parent_count = 1;

        if (kind == SG_SEQ_CHERRY_PICK) {
            new_commit.author_name = orig_commit.author_name;
            new_commit.author_email = orig_commit.author_email;
            new_commit.author_time = orig_commit.author_time;
            memcpy(new_commit.author_tz, orig_commit.author_tz, sizeof(new_commit.author_tz));
        } else {
            new_commit.author_name = (char *)committer_name;
            new_commit.author_email = (char *)committer_email;
            new_commit.author_time = (long long)time(NULL);
            strcpy(new_commit.author_tz, "+0000");
        }
        new_commit.committer_name = (char *)committer_name;
        new_commit.committer_email = (char *)committer_email;
        new_commit.committer_time = (long long)time(NULL);
        strcpy(new_commit.committer_tz, "+0000");
        new_commit.message = message;

        if (sg_commit_serialize(&new_commit, &serialized, &serialized_len) != 0) {
            fprintf(stderr, "sg: failed to serialize commit\n");
            free(new_commit.parents);
            free(current_branch);
            free(message);
            sg_commit_free(&orig_commit);
            sg_sequencer_state_free(&state);
            return 1;
        }
        free(new_commit.parents);

        if (sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len, new_commit_id) != 0) {
            fprintf(stderr, "sg: failed to write commit object\n");
            free(serialized);
            free(current_branch);
            free(message);
            sg_commit_free(&orig_commit);
            sg_sequencer_state_free(&state);
            return 1;
        }
        free(serialized);

        reflog_msg = msg_with_subject(kind == SG_SEQ_CHERRY_PICK ? "commit (cherry-pick): " : "commit: ",
                                      message);
        if (reflog_msg == NULL) {
            fprintf(stderr, "sg: out of memory\n");
            free(current_branch);
            free(message);
            sg_commit_free(&orig_commit);
            sg_sequencer_state_free(&state);
            return 1;
        }
        if (sg_ref_move_head(git_dir, current_branch, new_commit_id, reflog_msg) != 0) {
            fprintf(stderr, "sg: failed to update HEAD\n");
            free(reflog_msg);
            free(current_branch);
            free(message);
            sg_commit_free(&orig_commit);
            sg_sequencer_state_free(&state);
            return 1;
        }
        free(reflog_msg);

        sg_commit_free(&orig_commit);
        free(message);

        /* The rest of the sequence (state.todo[1..]) is run WITHOUT the
           "resumed" flag: only the entry that was actually finished by hand
           gets the "commit (cherry-pick)"/"commit" wording, already applied
           above -- every later entry uses the ordinary direct wording. */
        if (state.todo_count > 1) {
            unsigned char(*rest)[SG_SHA1_RAW_LEN] = malloc((state.todo_count - 1) * sizeof(*rest));

            if (rest == NULL) {
                fprintf(stderr, "sg: out of memory\n");
                free(current_branch);
                sg_sequencer_state_free(&state);
                return 1;
            }
            memcpy(rest, state.todo + 1, (state.todo_count - 1) * sizeof(*rest));
            rc = run_todo(git_dir, repo_root, kind, current_branch, state.orig_head, 1, rest,
                         state.todo_count - 1, 0, 0, 0);
        } else {
            if (sg_sequencer_state_remove(git_dir) != 0)
                fprintf(stderr, "sg: warning: could not remove %s state\n", op_name(kind));
            printf("Successfully %s.\n", kind == SG_SEQ_CHERRY_PICK ? "cherry-picked" : "reverted");
            rc = 0;
        }
        free(current_branch);
    }

    sg_sequencer_state_free(&state);
    return rc;
}

int sg_pick_skip(const char *git_dir, const char *repo_root, sg_seq_kind kind)
{
    sg_sequencer_state state;
    unsigned char head_commit[SG_SHA1_RAW_LEN];
    unsigned char head_tree[SG_SHA1_RAW_LEN];
    char *current_branch;
    int rc;

    if (require_state(git_dir, kind, &state) != 0)
        return 1;

    if (sg_ref_resolve_head(git_dir, head_commit) != 0 ||
       sg_commit_tree_of(git_dir, head_commit, head_tree) != 0) {
        fprintf(stderr, "sg: cannot read the current branch\n");
        sg_sequencer_state_free(&state);
        return 1;
    }

    {
        sg_index idx;

        if (sg_index_read(git_dir, &idx) != 0) {
            fprintf(stderr, "sg: failed to read index (corrupt?)\n");
            sg_sequencer_state_free(&state);
            return 1;
        }
        {
            char snap_bad_path[SG_PATH_MAX];

            snap_bad_path[0] = '\0';
            if (sg_snapshot_create(git_dir, repo_root, &idx, "cherry-pick/revert --skip", NULL,
                                   snap_bad_path) != 0) {
                fprintf(stderr, "sg: automatic snapshot failed, aborting the skip for safety (no "
                               "changes made)\n");
                sg_index_free(&idx);
                sg_sequencer_state_free(&state);
                return 1;
            }
        }
        sg_index_free(&idx);
    }

    if (sg_apply_tree_to_workdir(git_dir, repo_root, head_tree) != 0) {
        fprintf(stderr, "sg: failed to restore the working directory\n");
        sg_sequencer_state_free(&state);
        return 1;
    }

    current_branch = sg_ref_current_branch(git_dir);

    /* git's own --skip is, underneath, a no-op `reset --hard` to the
       current commit -- measured: it logs "reset: moving to <40hex>" to
       logs/HEAD even though nothing moves, and does NOT use any
       cherry-pick-specific wording (that would be the natural-looking
       "fix" and is wrong; a review caught this). sg_ref_move_head with
       target == the already-current commit reproduces exactly this: rule 1
       (CLAUDE.md) suppresses the branch's OWN log for a no-op, but rule 2
       still mirrors the line into logs/HEAD unconditionally. */
    {
        char reset_msg[SG_SHA1_HEX_LEN + 32];
        char hex[SG_SHA1_HEX_LEN + 1];

        sg_sha1_to_hex(head_commit, hex);
        snprintf(reset_msg, sizeof(reset_msg), "reset: moving to %s", hex);
        if (sg_ref_move_head(git_dir, current_branch, head_commit, reset_msg) != 0)
            fprintf(stderr, "sg: warning: failed to record the skip in the reflog\n");
    }

    if (state.todo_count <= 1) {
        if (sg_sequencer_state_remove(git_dir) != 0)
            fprintf(stderr, "sg: warning: could not remove %s state\n", op_name(kind));
        printf("Skipped %s.\n", op_name(kind));
        rc = 0;
    } else {
        unsigned char(*rest)[SG_SHA1_RAW_LEN] = malloc((state.todo_count - 1) * sizeof(*rest));

        if (rest == NULL) {
            fprintf(stderr, "sg: out of memory\n");
            free(current_branch);
            sg_sequencer_state_free(&state);
            return 1;
        }
        memcpy(rest, state.todo + 1, (state.todo_count - 1) * sizeof(*rest));
        rc = run_todo(git_dir, repo_root, kind, current_branch, state.orig_head, 1, rest,
                     state.todo_count - 1, 0, 0, 0);
    }

    free(current_branch);
    sg_sequencer_state_free(&state);
    return rc;
}

int sg_pick_abort(const char *git_dir, const char *repo_root, sg_seq_kind kind)
{
    sg_seq_kind actual;
    int has_sequence;
    unsigned char orig_head[SG_SHA1_RAW_LEN];
    unsigned char abort_safety[SG_SHA1_RAW_LEN];
    unsigned char target[SG_SHA1_RAW_LEN];
    unsigned char target_tree[SG_SHA1_RAW_LEN];
    char *current_branch;
    char abort_msg[SG_SHA1_HEX_LEN + 64];
    char target_hex[SG_SHA1_HEX_LEN + 1];

    actual = sg_sequencer_kind_in_progress(git_dir);
    if (actual == 0) {
        fprintf(stderr, "sg: no %s in progress\n", op_name(kind));
        return 1;
    }
    if (actual != kind) {
        fprintf(stderr, "sg: no %s in progress (a %s is in progress; run `sg %s --abort` "
                        "instead)\n",
               op_name(kind), op_name(actual), op_name(actual));
        return 1;
    }

    /* Deliberately does NOT go through sg_sequencer_state_read (and so
       never touches sequencer/todo) -- see sg_sequencer_abort_target's own
       header comment. This is the whole fix for the dead end where a
       damaged sequencer/todo (a partial write, a full disk, or -- most
       reachably -- a sequence a REAL GIT binary paused, whose todo ids are
       7-hex and unreadable by sg's parser) made every one of
       --continue/--skip/--abort/--quit refuse, with --abort's own error
       message pointing at itself. */
    if (sg_sequencer_abort_target(git_dir, &actual, &has_sequence, orig_head, abort_safety) != 0) {
        /* Only reachable if sequencer/head or sequencer/abort-safety
           THEMSELVES are malformed -- as simple a format as
           CHERRY_PICK_HEAD's own, so this really is corruption, not the
           todo-specific damage --abort exists to route around. --quit is
           still available: it parses nothing at all (see sg_pick_quit). */
        fprintf(stderr, "sg: %s state is corrupt, run sg %s --quit to give up (this discards the "
                        "conflict resolution work in progress, but leaves the working directory "
                        "as-is)\n",
               op_name(kind), op_name(kind));
        return 1;
    }

    if (has_sequence) {
        unsigned char now[SG_SHA1_RAW_LEN];

        if (sg_ref_resolve_head(git_dir, now) != 0) {
            fprintf(stderr, "sg: cannot read HEAD\n");
            return 1;
        }
        if (memcmp(now, abort_safety, SG_SHA1_RAW_LEN) != 0) {
            fprintf(stderr, "sg: HEAD has moved since the %s stopped; refusing to abort\n",
                   op_name(kind));
            return 1;
        }
        memcpy(target, orig_head, SG_SHA1_RAW_LEN);
    } else {
        if (sg_ref_resolve_head(git_dir, target) != 0) {
            fprintf(stderr, "sg: cannot read HEAD\n");
            return 1;
        }
    }

    {
        sg_index idx;

        if (sg_index_read(git_dir, &idx) != 0) {
            fprintf(stderr, "sg: failed to read index (corrupt?)\n");
            return 1;
        }
        {
            char snap_bad_path[SG_PATH_MAX];

            snap_bad_path[0] = '\0';
            if (sg_snapshot_create(git_dir, repo_root, &idx, "cherry-pick/revert --abort", NULL,
                                   snap_bad_path) != 0) {
                fprintf(stderr, "sg: automatic snapshot failed, aborting the abort for safety (no "
                               "changes made)\n");
                sg_index_free(&idx);
                return 1;
            }
        }
        sg_index_free(&idx);
    }

    if (sg_commit_tree_of(git_dir, target, target_tree) != 0) {
        fprintf(stderr, "sg: cannot read the pre-%s commit\n", op_name(kind));
        return 1;
    }
    if (sg_apply_tree_to_workdir(git_dir, repo_root, target_tree) != 0) {
        fprintf(stderr, "sg: failed to restore the working directory\n");
        return 1;
    }

    /* git's own --abort is, underneath, a plain `reset --hard <target>` --
       measured: it logs "reset: moving to <40hex>", never a cherry-pick- or
       revert-specific wording (the spec's earlier draft invented
       "cherry-pick (abort): returning to ..." here; real git does not say
       that, and this was corrected after measuring). This is the OPPOSITE
       convention from `sg rebase --abort`, which does use its own
       "rebase (abort): returning to ..." wording -- do not "unify" the two,
       they are deliberately different because rebase's abort is not a
       plain reset in real git either. */
    current_branch = sg_ref_current_branch(git_dir);
    sg_sha1_to_hex(target, target_hex);
    snprintf(abort_msg, sizeof(abort_msg), "reset: moving to %s", target_hex);
    if (sg_ref_move_head(git_dir, current_branch, target, abort_msg) != 0) {
        fprintf(stderr, "sg: cannot point HEAD back at %s\n", target_hex);
        free(current_branch);
        return 1;
    }
    free(current_branch);

    if (sg_sequencer_state_remove(git_dir) != 0) {
        fprintf(stderr, "sg: warning: could not fully remove %s state\n", op_name(kind));
        return 1;
    }

    printf("%s aborted; back at %s.\n", kind == SG_SEQ_CHERRY_PICK ? "Cherry-pick" : "Revert",
          target_hex);
    return 0;
}

int sg_pick_quit(const char *git_dir, const char *repo_root, sg_seq_kind kind)
{
    (void)repo_root;
    (void)kind;

    /* Spec section 5b: --quit PARSES NOTHING. Existence is the only
       question it asks (sg_sequencer_kind_in_progress, the same
       lstat-only check every gate in this project uses), and removal
       (sg_sequencer_state_remove) is plain remove()-by-name -- neither
       step ever reads sequencer/todo or anything else that could be
       damaged. This is deliberate and load-bearing: --quit is the one
       escape hatch that must work UNCONDITIONALLY, because --continue and
       --skip need a readable todo to do their real job, and --abort (see
       sg_pick_abort) reads only two other, equally simple 40-hex files.
       If those two ever also failed to parse, --quit is what is left.

       Measured: `git cherry-pick --quit` with NOTHING in progress exits 0
       and prints nothing at all -- unlike --continue/--skip/--abort, which
       all refuse with "no cherry-pick in progress" and exit 1. --quit's own
       job is "remove the state if there is any", so an already-clean
       repository is already in the state --quit asks for; a refusal here
       would be reporting an error for a request that already succeeded.

       Also deliberate: this does NOT check that the in-progress kind
       matches the invoked one (`sg cherry-pick --quit` removes a paused
       REVERT too, if that is what is actually there) -- the whole point is
       that no input, including "the wrong subcommand name", can make this
       fail. `kind` is accepted only to keep the same signature as the
       other three entry points sg_pick_*'s callers share. */
    if (sg_sequencer_kind_in_progress(git_dir) == 0)
        return 0;

    if (sg_sequencer_state_remove(git_dir) != 0) {
        fprintf(stderr, "sg: warning: could not fully remove the paused cherry-pick/revert "
                        "state\n");
        return 1;
    }
    return 0;
}
