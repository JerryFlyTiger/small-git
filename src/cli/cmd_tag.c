#include "sg/cli.h"

#include "sg/cli_args.h"
#include "sg/hash.h"
#include "sg/ident.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/revparse.h"
#include "sg/workdir.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char USAGE[] =
    "usage: sg tag [-a] [-m <msg>] [-f|--force] [--] <name> [<rev>]\n"
    "       sg tag -d <name>...\n"
    "       sg tag\n";

static int list_tags(const char *git_dir)
{
    char **names;
    size_t count;
    size_t i;

    if (sg_ref_list_under(git_dir, "refs/tags/", &names, &count) != 0) {
        fprintf(stderr, "sg: cannot list tags\n");
        return 1;
    }

    for (i = 0; i < count; i++) {
        printf("%s\n", names[i]);
        free(names[i]);
    }
    free(names);
    return 0;
}

static int create_tag(const char *git_dir, const char *name, const char *rev, int annotated,
                      const char **messages, int message_count, int force)
{
    unsigned char target_id[SG_SHA1_RAW_LEN];
    sg_obj_type target_type;
    char ref_path[SG_PATH_MAX];
    unsigned char old_id[SG_SHA1_RAW_LEN];
    int had_old;

    if (!sg_ref_name_valid_for_create(name)) {
        fprintf(stderr, "sg: '%s' is not a valid tag name.\n", name);
        return 1;
    }

    if (snprintf(ref_path, sizeof(ref_path), "refs/tags/%s", name) >= (int)sizeof(ref_path)) {
        fprintf(stderr, "sg: tag name too long\n");
        return 1;
    }

    had_old = sg_ref_read_path(git_dir, ref_path, old_id) == 0;
    if (!force && had_old) {
        fprintf(stderr, "sg: tag '%s' already exists\n", name);
        return 1;
    }

    {
        /* Phase 73 fix: <rev> must NOT be peeled -- sg_rev_parse_commit
           peels an annotated tag to its underlying commit by contract,
           which is wrong here (measured against real git 2.55.0:
           `tag t atag` makes a lightweight ref to the TAG OBJECT, not to
           the commit it names). sg_rev_parse_object resolves any object
           without peeling and reports its actual type, which also makes
           tagging a tree or a blob (and a "<rev>:<path>" argument) work
           for free -- both are git's real behaviour, not an extension. */
        const char *rev_or_head = rev != NULL ? rev : "HEAD";
        char bad_path[SG_PATH_MAX];
        int prc;

        bad_path[0] = '\0';
        prc = sg_rev_parse_object(git_dir, rev_or_head, target_id, &target_type, bad_path,
                                  sizeof(bad_path));
        /* Phase 75: git's own wordings, measured against real git 2.55.0 --
           see the "tag" row of the table in cli_args.c. class O names the
           REF being created (git's own line does too), so `name` is
           threaded through as the reporter's `detail`; class P is NOT the
           standard "path does not exist" message here -- git answers with
           its class-R wording instead ("Failed to resolve ... as a valid
           ref."), which is why -2 is folded into the same branch as the
           generic failure rather than getting its own. */
        if (prc == -3) {
            sg_cli_report_rev_error("tag", SG_REV_ERR_MISSING_OBJ, rev_or_head, name, 0);
            return 1;
        }
        if (prc != 0) {
            if (prc == -4)
                sg_cli_report_ambiguous_oid(git_dir, rev_or_head, SG_REV_STRICT);
            sg_cli_report_rev_error("tag", SG_REV_ERR_NOT_A_REV, rev_or_head, NULL, 0);
            return 1;
        }
    }

    if (annotated) {
        sg_tag tag;
        unsigned char *serialized;
        size_t serialized_len;
        unsigned char tag_id[SG_SHA1_RAW_LEN];
        sg_ident tagger;
        const char *bad = NULL;
        char *joined_message;
        char *cleaned_message;

        /* An annotated tag's tagger line comes from the COMMITTER identity,
           the same as a commit's committer line -- not the author identity
           (Phase 72's defect C; measured against real git 2.55.0). */
        if (sg_ident_committer(&tagger, &bad) != 0) {
            fprintf(stderr, "sg: invalid date format: %s\n", bad);
            return 1;
        }

        /* Phase 73 addendum: repeated -m is NOT last-one-wins (measured:
           `-m one -m two` -> message "one\n\ntwo", each -m becomes its
           own paragraph). Join first, then clean up exactly as a single
           -m value already was. */
        if (sg_message_join(messages, (size_t)message_count, &joined_message) != 0) {
            fprintf(stderr, "sg: out of memory\n");
            return 1;
        }

        /* Unlike `git commit`, real `git tag -a -m` does NOT refuse an
           empty (or whitespace-only, which normalizes to empty) message --
           it happily creates a tag object whose message segment is empty.
           Verified directly against git 2.55.0. So, unlike cmd_commit.c,
           there is no empty-message rejection here. */
        if (sg_message_cleanup(joined_message, &cleaned_message) != 0) {
            fprintf(stderr, "sg: out of memory\n");
            free(joined_message);
            return 1;
        }
        free(joined_message);

        memset(&tag, 0, sizeof(tag));
        memcpy(tag.object, target_id, SG_SHA1_RAW_LEN);
        /* Phase 73 fix: the target's ACTUAL type, not a hardcoded commit --
           git writes `type tag`/`type tree`/`type blob` when <rev> names
           one of those. */
        tag.object_type = target_type;
        tag.tag_name = (char *)name;
        tag.tagger_name = tagger.name;
        tag.tagger_email = tagger.email;
        tag.tagger_time = tagger.when;
        strcpy(tag.tagger_tz, tagger.tz);
        tag.message = cleaned_message;

        if (sg_tag_serialize(&tag, &serialized, &serialized_len) != 0) {
            fprintf(stderr, "sg: cannot serialize tag object\n");
            free(cleaned_message);
            return 1;
        }
        free(cleaned_message);
        if (sg_loose_write(git_dir, SG_OBJ_TAG, serialized, serialized_len, tag_id) != 0) {
            fprintf(stderr, "sg: cannot write tag object\n");
            free(serialized);
            return 1;
        }
        free(serialized);

        if (sg_ref_write_path(git_dir, ref_path, tag_id) != 0) {
            fprintf(stderr, "sg: cannot create tag '%s'\n", name);
            return 1;
        }

        /* "Updated tag" is about the ref's VALUE moving, not about -f
           overwriting something -- measured: `-f` re-pointing a tag at the
           commit it already names prints nothing at all. Compare against
           the newly written ref target (the tag object id here), not the
           underlying commit. */
        if (had_old && memcmp(old_id, tag_id, SG_SHA1_RAW_LEN) != 0) {
            char old_hex[SG_SHA1_HEX_LEN + 1];

            sg_sha1_to_hex(old_id, old_hex);
            printf("Updated tag '%s' (was %.7s)\n", name, old_hex);
        }
    } else {
        if (sg_ref_write_path(git_dir, ref_path, target_id) != 0) {
            fprintf(stderr, "sg: cannot create tag '%s'\n", name);
            return 1;
        }

        if (had_old && memcmp(old_id, target_id, SG_SHA1_RAW_LEN) != 0) {
            char old_hex[SG_SHA1_HEX_LEN + 1];

            sg_sha1_to_hex(old_id, old_hex);
            printf("Updated tag '%s' (was %.7s)\n", name, old_hex);
        }
    }

    return 0;
}

/* Phase 73: -d now takes many names, processed in argv order, deleting
   each one that exists and reporting one error line per name that does
   not -- NOT all-or-nothing (measured: `git tag -d atag nosuch lw` deletes
   both atag and lw, prints one error for nosuch, and exits 1). Returns 0
   only if every name was deleted (also 0 for an empty list -- see the
   `-d` with no names note at the call site).

   Phase 73 review round 2: a name appearing MORE THAN ONCE in this same
   invocation is a ref-transaction property in real git, not a per-name
   one -- `git tag -d lw lw` and `git tag -d lw atag lw` both refuse the
   WHOLE batch and delete NOTHING ("could not delete references: multiple
   updates for ref 'refs/tags/lw' not allowed", the identical shape
   cmd_push.c documents for the refs/sg/chunks keepalive). This has to be
   checked BEFORE any deletion runs: `-d lw atag lw` deleting "everything
   except the duplicate" (which is what happens if the check runs
   per-name during the loop, or after it) is a WORSE answer than either
   all-or-nothing extreme, because git's answer is "nothing" and the
   naive answer silently deletes atag too.

   Phase 73 review round 3: the rule above OVER-REFUSES -- it is a
   property of git's ref TRANSACTION, so it only applies to a name that
   actually ENTERS that transaction, i.e. one that resolves to an
   existing tag. `-d nosuch atag nosuch` deletes atag and reports
   "not found" twice on real git (measured); a scan keyed on bare argv
   equality wrongly refused the whole batch here, leaving atag undeleted.
   Existence has to be resolved for every name FIRST (before any
   deletion, so a later name's existence can't have been changed by an
   earlier one in the same call), and only names that both repeat AND
   currently exist can trigger the refusal.

   Also per review round 3: when two DIFFERENT existing names are each
   duplicated (`-d lw lw atag2 atag2`), real git's message names the
   SMALLEST one by strcmp ('atag2'), independent of argv order (measured
   both orderings, including a v1.9/v1.10 fixture where strcmp picks
   v1.10 against intuition, and a fixture where the smallest name sits
   LAST in argv). Error wording is interface in this project, so this is
   matched rather than left as a divergence.

   Phase 73 review round 4: round 3's early return on the refusal
   SWALLOWED the "not found" diagnostics for any missing name in the same
   call -- it decided existence and refused in one pass that returned
   before ever printing anything for a missing name. Measured: `git tag
   -d nosuch lw lw` prints "not found" for nosuch, THEN the refusal for
   lw (both lines, in that order); `-d nosuch nosuch lw lw` prints
   "not found" TWICE (once per occurrence, in argv order) before the
   refusal. So existence resolution, the "not found" reporting, and the
   refusal decision are now three things done in that ORDER, over the
   WHOLE name list, with nothing deleted until all three have run:
   resolve+report first (one line per missing occurrence, as encountered,
   never re-checked against the ORIGINAL argv-equality rule so a
   duplicated MISSING name still doesn't trigger a refusal), then decide
   the refusal from the recorded existence flags, then -- only if no
   refusal fires -- actually delete. */
static int delete_tags(const char *git_dir, const char **names, int count)
{
    int i;
    int had_failure = 0;
    int *exists;
    int existing_count;
    const char *smallest_dup = NULL;

    /* Upper-bounded by argc via the caller's own positional array. */
    exists = malloc(sizeof(*exists) * (size_t)(count > 0 ? count : 1));
    if (exists == NULL) {
        fprintf(stderr, "sg: out of memory\n");
        return 1;
    }

    /* Pass 1: resolve existence and report every missing name (or one
       whose ref path is too long) EXACTLY ONCE, in argv order, as it is
       encountered. Nothing is deleted here, so this pass and the refusal
       decision below both see the SAME pre-deletion state. */
    for (i = 0; i < count; i++) {
        char ref_path[SG_PATH_MAX];
        unsigned char id[SG_SHA1_RAW_LEN];

        if (snprintf(ref_path, sizeof(ref_path), "refs/tags/%s", names[i]) >= (int)sizeof(ref_path)) {
            fprintf(stderr, "sg: tag name too long: '%s'\n", names[i]);
            exists[i] = 0;
            had_failure = 1;
            continue;
        }
        if (sg_ref_read_path(git_dir, ref_path, id) != 0) {
            fprintf(stderr, "sg: tag '%s' not found.\n", names[i]);
            exists[i] = 0;
            had_failure = 1;
            continue;
        }
        exists[i] = 1;
    }

    /* Phase 74 round 4: how many names actually ENTER git's ref
       transaction -- i.e. how many exist -- decides singular vs plural
       wording in the stray-lock message below (pass 2b). Measured against
       real git 2.55.0, this is keyed on the EXISTING count, not raw argv
       count: `tag -d nosuch foo` (nosuch missing, foo has a stale lock)
       still gets the SINGULAR "could not delete reference refs/tags/foo: "
       form, because only foo ever entered the transaction; `tag -d foo
       bar` (foo locked, bar a normal existing tag) gets the PLURAL
       "could not delete references: " form with two existing names,
       regardless of which one collides or survives. */
    existing_count = 0;
    for (i = 0; i < count; i++) {
        if (exists[i])
            existing_count++;
    }

    /* Pass 2: a repeated name only triggers the transaction refusal when
       it EXISTS (round 3's rule, unchanged here) -- a repeated MISSING
       name already got its "not found" line above, once per occurrence,
       and blocks nothing. */
    for (i = 0; i < count; i++) {
        int j;

        if (!exists[i])
            continue;
        for (j = i + 1; j < count; j++) {
            if (!exists[j] || strcmp(names[i], names[j]) != 0)
                continue;
            if (smallest_dup == NULL || strcmp(names[i], smallest_dup) < 0)
                smallest_dup = names[i];
            break; /* names[i] is flagged; no need to find every pair naming it */
        }
    }
    if (smallest_dup != NULL) {
        fprintf(stderr,
               "sg: could not delete references: multiple updates for ref "
               "'refs/tags/%s' not allowed\n",
               smallest_dup);
        free(exists);
        return 1;
    }

    /* Pass 2b (Phase 74 round 2): reproduce git's OWN mechanism, not a
       heuristic approximation of it. git takes a per-ref LOCK
       (`refs/tags/<name>.lock`, created with O_CREAT|O_EXCL) for every
       name in the batch before deleting anything; two names collide iff
       their LOCK PATHS resolve to the same file. That is a property of
       the lock path string on disk (case-folding merges 'Foo.lock' and
       'foo.lock' into one directory entry), and it is INDEPENDENT of
       whether the underlying ref itself is loose or lives only in
       packed-refs -- git still creates the lock file either way, because
       the lock's job is to serialize the delete, not to touch the ref's
       own storage. An inode comparison of the REF files themselves (this
       function's round-1 approach) got both of these wrong, measured:
       two names that are BOTH packed-only never alias by that test (a
       packed ref has no loose file to `lstat` at all), so sg deleted both
       and exited 0 where git refuses; two ref files deliberately
       hardlinked to the SAME inode DO alias by that test even though
       their `.lock` paths are two unrelated strings, so sg over-refused
       where git deletes both. Locking is what git actually does, so it
       gets both right with one mechanism.

       This is still a different rule from pass 2 above, not a
       generalization of it: git's message names the ref by ARGV ORDER
       here (the first name creates the lock; the first LATER name whose
       lock creation collides is the one reported), where pass 2 names the
       strcmp-SMALLEST string regardless of argv order. Measured against
       real git 2.55.0: `Foo foo` -> names 'foo', `foo Foo` -> names
       'Foo', `FOO foo Foo` -> names 'foo', `zz ZZ` -> names 'ZZ' (all
       names[1]). Measured priority when both rules could apply to the
       same call (`Foo foo lw lw`, `lw lw Foo foo`): pass 2's
       literal-duplicate rule always wins, independent of which pair sits
       first in argv -- so this pass only runs once pass 2 has found
       nothing, unchanged from round 1.

       sg has no ref transaction and no lock file format of its own to
       borrow, so this pass creates ordinary `O_CREAT|O_EXCL` lock files
       purely as a COLLISION DETECTOR, exactly mirroring git's own
       mechanism, and removes every one of them again before returning
       from this pass by ANY exit path (collision, mid-loop error, or
       reaching the end clean) -- a lock file surviving past this pass
       would make the NEXT invocation see a stale lock it cannot explain.
       The message still does not claim "cannot lock ref" (sg's lock is
       an internal detection mechanism only, not a durable feature), and
       still names BOTH colliding ref paths rather than one, matching
       round 1's wording -- attributing the message to the earlier
       colliding name uses an (st_dev, st_ino) compare of the LOCK FILES
       themselves (not the refs), which is safe precisely because lock
       files are always ordinary loose files this function just created,
       whether or not the underlying ref is packed. */
    {
        struct held_lock {
            char *path; /* owned; git_dir + "/" + ref_path + ".lock" */
            const char *name;
        } *locks;
        int lock_count = 0;
        int collision = 0;

        locks = malloc(sizeof(*locks) * (size_t)(count > 0 ? count : 1));
        if (locks == NULL) {
            fprintf(stderr, "sg: out of memory\n");
            free(exists);
            return 1;
        }

        for (i = 0; i < count && !collision; i++) {
            char ref_path[SG_PATH_MAX];
            char *lock_path;
            int fd;

            if (!exists[i])
                continue;
            if (snprintf(ref_path, sizeof(ref_path), "refs/tags/%s.lock", names[i]) >= (int)sizeof(ref_path))
                continue; /* already reported as too-long in pass 1 */
            lock_path = malloc(strlen(git_dir) + 1 + strlen(ref_path) + 1);
            if (lock_path == NULL || sg_path_join(lock_path, strlen(git_dir) + 1 + strlen(ref_path) + 1,
                                                   git_dir, ref_path) != 0) {
                /* Phase 74 round 3: this used to `free(); continue;` with
                   no failure flag, silently dropping this name out of
                   collision detection while the batch proceeded to
                   delete anyway -- fail-OPEN in a function whose entire
                   purpose is fail-closed, and inconsistent with the
                   open() failure below, which already aborts. An OOM or
                   a path-join failure here is at least as serious as a
                   permission error on open(), so it gets the same
                   treatment: abort the whole batch, delete nothing. */
                fprintf(stderr, "sg: out of memory\n");
                free(lock_path);
                collision = 1;
                continue;
            }
            /* A nested tag name (e.g. "release/v1") needs its parent
               directory to exist before the lock file can be created --
               same reason sg_ref_write_path calls this before its own
               fopen. An ordinary top-level tag name is already inside the
               pre-existing refs/tags/ directory, so this is a no-op then. */
            if (sg_mkdir_parents(lock_path) != 0) {
                /* Same fail-closed treatment as the malloc/path-join
                   failure just above, for the same reason. */
                fprintf(stderr, "sg: failed to lock ref 'refs/tags/%s': could not create lock directory\n",
                        names[i]);
                free(lock_path);
                collision = 1;
                continue;
            }

            fd = open(lock_path, O_CREAT | O_EXCL | O_WRONLY, 0666);
            if (fd < 0) {
                if (errno == EEXIST) {
                    struct stat new_st;
                    const char *other_name = NULL;
                    int k;

                    if (lstat(lock_path, &new_st) == 0) {
                        for (k = 0; k < lock_count; k++) {
                            struct stat held_st;

                            if (lstat(locks[k].path, &held_st) == 0 &&
                                held_st.st_dev == new_st.st_dev && held_st.st_ino == new_st.st_ino) {
                                other_name = locks[k].name;
                                break;
                            }
                        }
                    }
                    /* Two different messages for two different causes
                       (Phase 74 round 3): if the colliding lock is one
                       THIS batch already holds, it really is the in-batch
                       aliasing case (case-folding, a hardlink, etc.) and
                       naming both paths is the useful answer -- git only
                       ever names one, but sg can do better since it just
                       created both locks itself. If `other_name` is NULL,
                       nothing in THIS batch owns the colliding lock file:
                       it is a stray `.lock` left by a crashed process or
                       another tool entirely, and "'refs/tags/foo' and
                       'refs/tags/foo' are the same ref" would be a false,
                       self-contradictory statement (a ref cannot alias
                       itself) that misdirects a debugging user toward
                       aliasing when the real cause is unrelated. For that
                       branch, sg genuinely could not take the lock, so
                       git's own "cannot lock ref" wording is now the true
                       description -- round 2's objection to using it (sg
                       had no lock mechanism to describe) no longer holds,
                       since this pass IS that mechanism. The absolute
                       path git includes is left out as an implementation
                       detail sg does not need to expose. */
                    if (other_name != NULL) {
                        /* An in-batch alias always involves at least two
                           EXISTING names by construction (two different
                           strings both had to resolve to collide), so this
                           branch never needs the singular/plural choice
                           below -- it is unconditionally the "two or more"
                           shape, matching git's own plural "references:"
                           wording in every case that can reach here. */
                        fprintf(stderr,
                                "sg: could not delete references: 'refs/tags/%s' and "
                                "'refs/tags/%s' are the same ref\n",
                                other_name, names[i]);
                    } else if (existing_count == 1) {
                        /* Phase 74 round 4: git's singular form when
                           exactly one ref entered the transaction --
                           measured `could not delete reference
                           refs/tags/foo: cannot lock ref 'refs/tags/foo':
                           ...` (no quotes around the outer ref path, and
                           "reference" without an 's'). This is keyed on
                           EXISTENCE, not argv position or count: a batch
                           of `nosuch foo` (nosuch missing, foo the only
                           one that resolves) still gets this singular
                           form, because 'nosuch' never entered the
                           transaction at all. */
                        fprintf(stderr,
                                "sg: could not delete reference refs/tags/%s: cannot lock ref "
                                "'refs/tags/%s': File exists\n",
                                names[i], names[i]);
                    } else {
                        fprintf(stderr,
                                "sg: could not delete references: cannot lock ref 'refs/tags/%s': "
                                "File exists\n",
                                names[i]);
                    }
                    collision = 1;
                } else {
                    fprintf(stderr, "sg: failed to lock ref 'refs/tags/%s': %s\n", names[i], strerror(errno));
                    collision = 1; /* abort the whole batch -- cannot safely proceed unlocked */
                }
                free(lock_path);
                continue;
            }
            close(fd);
            locks[lock_count].path = lock_path;
            locks[lock_count].name = names[i];
            lock_count++;
        }

        /* Every lock this pass created is released here, on every exit
           path -- collision, an open()/mkdir_parents error, or falling
           through clean. None of them are meant to outlive this pass. */
        for (i = 0; i < lock_count; i++) {
            unlink(locks[i].path);
            free(locks[i].path);
        }
        free(locks);

        if (collision) {
            free(exists);
            return 1;
        }
    }

    /* Pass 3: no transaction conflict -- actually delete every name that
       exists. Re-deriving the ref path and re-reading the old id is not a
       check-then-use gap: nothing has been deleted by either earlier pass,
       so the ref's value cannot have changed since pass 1 observed it. */
    for (i = 0; i < count; i++) {
        const char *name = names[i];
        char ref_path[SG_PATH_MAX];
        unsigned char old_id[SG_SHA1_RAW_LEN];
        char old_hex[SG_SHA1_HEX_LEN + 1];
        int rc;

        if (!exists[i])
            continue; /* already reported in pass 1 */

        snprintf(ref_path, sizeof(ref_path), "refs/tags/%s", name);
        if (sg_ref_read_path(git_dir, ref_path, old_id) != 0) {
            fprintf(stderr, "sg: tag '%s' not found.\n", name);
            had_failure = 1;
            continue;
        }
        rc = sg_ref_delete_under(git_dir, "refs/tags/", name);
        if (rc != 0) {
            fprintf(stderr, "sg: failed to delete tag '%s'\n", name);
            had_failure = 1;
            continue;
        }
        sg_sha1_to_hex(old_id, old_hex);
        printf("Deleted tag '%s' (was %.7s)\n", name, old_hex);
    }

    free(exists);
    return had_failure ? 1 : 0;
}

int sg_cmd_tag(int argc, char **argv)
{
    int del = 0;
    int force = 0;
    int annotated = 0;
    int opts_done = 0;
    const char **messages;
    int message_count = 0;
    const char **positional;
    int npositional = 0;
    const char *name = NULL;
    const char *rev = NULL;
    char *git_dir;
    int rc;
    int i;

    /* Both arrays are upper-bounded by argc: every positional argument
       and every -m value comes straight from argv, so there can never be
       more of either than argv itself. */
    positional = malloc(sizeof(*positional) * (size_t)(argc > 0 ? argc : 1));
    messages = malloc(sizeof(*messages) * (size_t)(argc > 0 ? argc : 1));
    if (positional == NULL || messages == NULL) {
        fprintf(stderr, "sg: out of memory\n");
        free(positional);
        free(messages);
        return 1;
    }

    for (i = 1; i < argc; i++) {
        if (!opts_done && strcmp(argv[i], "--") == 0) {
            opts_done = 1;
        } else if (!opts_done && (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--delete") == 0)) {
            del = 1;
        } else if (!opts_done && (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0)) {
            force = 1;
        } else if (!opts_done && strcmp(argv[i], "-a") == 0) {
            annotated = 1;
        } else if (!opts_done && strcmp(argv[i], "-m") == 0) {
            if (i + 1 >= argc) {
                fputs(USAGE, stderr);
                free(positional);
                free(messages);
                return 1;
            }
            /* Phase 73 addendum: repeated -m joins into one message
               (see sg_message_join's header comment) -- it does NOT
               overwrite the previous value the way this used to read. */
            messages[message_count++] = argv[++i];
        } else if (!opts_done && argv[i][0] == '-') {
            fputs(USAGE, stderr);
            free(positional);
            free(messages);
            return 1;
        } else {
            positional[npositional++] = argv[i];
        }
    }

    if (del) {
        /* -d combined with any tag-creation option (-a/-m/-f) is a usage
           error in real git, not a silent partial action (measured:
           `git tag -d -a -m x name` and `git tag -d -f name` both print
           usage and delete nothing, exit 129). Multiple names are no
           longer special-cased here -- see delete_tags for the per-name
           partial-failure semantics Phase 73 adds.

           Phase 73 addendum: `-d` with NO names at all is NOT a usage
           error -- measured twice against real git 2.55.0, `git tag -d`
           exits 0 and prints nothing. This is the opposite direction from
           a create-only flag with no name (which IS a usage error, see
           below); the two are deliberately not unified. */
        if (annotated || message_count != 0 || force) {
            fputs(USAGE, stderr);
            free(positional);
            free(messages);
            return 1;
        }
    } else {
        if (npositional > 2) {
            fputs(USAGE, stderr);
            free(positional);
            free(messages);
            return 1;
        }
        if (npositional >= 1)
            name = positional[0];
        if (npositional >= 2)
            rev = positional[1];

        if (annotated && message_count == 0) {
            fputs(USAGE, stderr);
            free(positional);
            free(messages);
            return 1;
        }
        /* -m without -a implicitly means an annotated tag, matching real
           git. */
        if (message_count != 0)
            annotated = 1;

        /* Phase 73 fix: a create-only flag (-a/-m/-f) with no tag name is
           a usage error in real git (exit 129: `git tag -m hi`, `git tag
           -f`, `git tag -a -m hi` all refuse) -- sg used to fall through
           to list_tags, silently swallowing the flags. Bare `sg tag` with
           no arguments at all must keep listing, which this leaves
           untouched (annotated and force are both still 0 there). */
        if (name == NULL && (annotated || force)) {
            fputs(USAGE, stderr);
            free(positional);
            free(messages);
            return 1;
        }
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL) {
        free(positional);
        free(messages);
        return 1;
    }

    if (del)
        rc = delete_tags(git_dir, positional, npositional);
    else if (name != NULL)
        rc = create_tag(git_dir, name, rev, annotated, messages, message_count, force);
    else
        rc = list_tags(git_dir);

    free(positional);
    free(messages);
    free(git_dir);
    return rc;
}
