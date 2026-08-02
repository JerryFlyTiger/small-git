#ifndef SG_TRANSPORT_H
#define SG_TRANSPORT_H

#include <stddef.h>
#include <stdio.h>

#include "sg/hash.h"

/* Writes remote-authored text to f with control characters (other than
   \n, \r, \t) replaced by '?'. Remote messages land on a terminal, so a
   hostile server must not be able to smuggle ANSI escapes through them. */
void sg_print_remote_text(const unsigned char *text, size_t len, FILE *f);

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

/* Same as sg_transport_ls_refs, but GETs
   <base_url>/info/refs?service=git-receive-pack instead -- the advertisement
   for a push. Wire format is identical (service line, flush, refs with the
   first ref's payload carrying capabilities), just against the other
   service; sg_ref_name_is_safe filtering applies the same way. */
int sg_transport_ls_refs_push(const char *base_url, sg_ref_adv *adv_out);

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

/* ---- push (git-receive-pack) ---- */

typedef struct {
    int ok;         /* 1 = the server accepted this ref update, 0 = rejected */
    char *ref_name; /* malloc'd, owned */
    char *message;  /* malloc'd, owned; the "ng" reason, or NULL when ok */
} sg_push_ref_result;

typedef struct {
    int unpack_ok;      /* 1 if the server reported "unpack ok" */
    char *unpack_error; /* malloc'd, owned; the server's message, or NULL when unpack_ok */
    sg_push_ref_result *refs; /* malloc'd, owned */
    size_t ref_count;
} sg_push_report;

void sg_push_report_free(sg_push_report *report);

/* POST <base_url>/git-receive-pack updating a single ref (old_id -> new_id
   for ref_name) and uploading pack_data/pack_len as the packfile. Requests
   the capabilities "report-status agent=small-git/0.1", plus "side-band-64k"
   iff use_side_band_64k is set -- callers must only pass 1 for that when the
   remote's own advertisement (sg_transport_ls_refs_push) listed it; this
   function does not check. Parses the report-status response (demuxing
   side-band-64k first, if requested) into *report_out. Returns 0 if the HTTP
   round trip itself succeeded -- this says nothing about whether the ref
   update was actually accepted, which is what *report_out is for -- or -1 on
   a transport-level failure (message already printed to stderr). */
int sg_transport_push(const char *base_url, const unsigned char old_id[SG_SHA1_RAW_LEN],
                      const unsigned char new_id[SG_SHA1_RAW_LEN], const char *ref_name,
                      int use_side_band_64k, const unsigned char *pack_data, size_t pack_len,
                      sg_push_report *report_out);

#endif
