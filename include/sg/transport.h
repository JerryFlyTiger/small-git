#ifndef SG_TRANSPORT_H
#define SG_TRANSPORT_H

#include <stddef.h>

#include "sg/hash.h"

typedef struct {
    char *name; /* malloc'd, owned */
    unsigned char id[SG_SHA1_RAW_LEN];
} sg_remote_ref;

typedef struct {
    sg_remote_ref *refs; /* malloc'd, owned; only refs that passed
                            sg_ref_name_is_safe are present, in the order the
                            server advertised them */
    size_t count;
    char *capabilities; /* malloc'd capability string from the first
                           advertised ref, or NULL */
    char *head_symref;  /* malloc'd target (e.g. "refs/heads/main") parsed out
                           of a "symref=HEAD:<target>" capability, or NULL if
                           absent */
} sg_ref_adv;

void sg_ref_adv_free(sg_ref_adv *adv);

/* Validates a ref name (remote-controlled input) before it is ever used to
   build a filesystem path: must start with "refs/", and must not contain
   "..", "//", a trailing '/', control characters, or any of
   \ ~ ^ : ? * [ <space> -- the same class of path-traversal/shell-metachar
   hazard as the tree-entry-name check from Phase 2. Non-zero means safe. */
int sg_ref_name_is_safe(const char *name);

/* GET <base_url>/info/refs?service=git-upload-pack and parse the protocol v0
   ref advertisement. An empty remote repository (only the placeholder
   "capabilities^{}" zero-id ref) yields adv_out->count == 0, which is not an
   error. Any advertised ref whose name fails sg_ref_name_is_safe is skipped
   (with a warning on stderr) rather than included. Returns 0 on success, -1
   on a transport error or a response that isn't a smart-HTTP upload-pack
   advertisement (message already printed to stderr). */
int sg_transport_ls_refs(const char *base_url, sg_ref_adv *adv_out);

/* POST <base_url>/git-upload-pack requesting want_ids (want_count > 0) and,
   for a fetch, negotiating with have_ids (have_count may be 0, e.g. for a
   clone). Requests exactly the capabilities side-band-64k, ofs-delta, and
   agent=small-git/0.1 -- deliberately not thin-pack, so the resulting pack is
   always self-contained. side-band-64k progress (band 2) is streamed to
   stderr as it arrives; a band-3 fatal message is printed to stderr and
   fails the whole call. *pack_out is malloc'd (caller frees); it always
   starts with the "PACK" magic on success, even if the pack contains zero
   objects (nothing new to fetch). Returns 0 on success, -1 on failure. */
int sg_transport_fetch_pack(const char *base_url,
                            const unsigned char (*want_ids)[SG_SHA1_RAW_LEN], size_t want_count,
                            const unsigned char (*have_ids)[SG_SHA1_RAW_LEN], size_t have_count,
                            unsigned char **pack_out, size_t *pack_len_out);

#endif
