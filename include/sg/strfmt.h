#ifndef SG_STRFMT_H
#define SG_STRFMT_H

/* Phase 65: the one place in this codebase allowed to size a formatted
   string. A fixed-size stack buffer fed by snprintf(buf, sizeof(buf), ...,
   user_string) silently truncates once user_string is long enough -- this
   project has now measured that shape producing a genuinely different git
   object (Phase 65's cmd_merge.c bug: two 250-char branch names made the
   merge commit MESSAGE differ from git's, which is a different commit id),
   not just a cosmetically short line. `sg_strfmt_alloc` replaces the
   pattern "snprintf(NULL, 0, ...) to size, then malloc, then snprintf again"
   that a handful of call sites (cmd_rebase.c's reflog_msg_with_subject,
   pick.c's msg_with_subject/theirs_label, cmd_switch.c's checkout_msg) had
   each hand-rolled independently.

   Returns a malloc'd, NUL-terminated string the caller must free, or NULL
   on a formatting error (a negative vsnprintf return) or an allocation
   failure -- the two are not distinguished, callers that care already treat
   any NULL from an allocator as OOM elsewhere in this codebase. */
char *sg_strfmt_alloc(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

#endif
