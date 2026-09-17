#include "sg/diff.h"

#include <stdio.h>
#include <string.h>

/* Phase 81a: sg_diff_entry_is_typechange is the one shared predicate for
   "both sides present, both types known, and the file TYPE differs" --
   entry_status (diff_out.c), the patch splitter (diff_out.c), and
   sg_status_diff_staged/sg_status_diff_unstaged (workdir/status.c) all go
   through it. This is the only place it is unit-tested directly; the
   interop `phase81a` group covers the byte-level rendering that consumes
   it. See CLAUDE.md's "Testing conventions" (copy of tests/test_confirm.c)
   and docs/RULES-diff.md's Phase 81a note. */

static int failures = 0;

#define CHECK(cond, ...)                                                                         \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);                                  \
            fprintf(stderr, __VA_ARGS__);                                                         \
            fprintf(stderr, "\n");                                                                \
            failures++;                                                                           \
        }                                                                                          \
    } while (0)

static sg_diff_entry make_entry(sg_diff_side_kind old_kind, unsigned int old_mode,
                                sg_diff_side_kind new_kind, unsigned int new_mode)
{
    sg_diff_entry e;

    memset(&e, 0, sizeof(e));
    e.old_side.kind = old_kind;
    e.old_side.mode = old_mode;
    e.new_side.kind = new_kind;
    e.new_side.mode = new_mode;
    return e;
}

static void test_regular_to_symlink(void)
{
    sg_diff_entry e = make_entry(SG_DIFF_SIDE_BLOB, 0100644, SG_DIFF_SIDE_BLOB, 0120000);

    CHECK(sg_diff_entry_is_typechange(&e), "100644 -> 120000 must be a typechange");
}

static void test_exec_to_symlink(void)
{
    sg_diff_entry e = make_entry(SG_DIFF_SIDE_BLOB, 0100755, SG_DIFF_SIDE_BLOB, 0120000);

    CHECK(sg_diff_entry_is_typechange(&e), "100755 -> 120000 must be a typechange");
}

static void test_submodule_to_regular(void)
{
    sg_diff_entry e = make_entry(SG_DIFF_SIDE_BLOB, 0160000, SG_DIFF_SIDE_BLOB, 0100644);

    CHECK(sg_diff_entry_is_typechange(&e), "160000 -> 100644 must be a typechange");
}

static void test_regular_to_exec_is_not_typechange(void)
{
    sg_diff_entry e = make_entry(SG_DIFF_SIDE_BLOB, 0100644, SG_DIFF_SIDE_BLOB, 0100755);

    CHECK(!sg_diff_entry_is_typechange(&e),
         "100644 -> 100755 is only an exec-bit change, not a typechange");
}

static void test_old_absent_is_not_typechange(void)
{
    sg_diff_entry e = make_entry(SG_DIFF_SIDE_ABSENT, 0, SG_DIFF_SIDE_BLOB, 0120000);

    CHECK(!sg_diff_entry_is_typechange(&e), "an addition (old side ABSENT) is never a typechange");
}

static void test_new_absent_is_not_typechange(void)
{
    sg_diff_entry e = make_entry(SG_DIFF_SIDE_BLOB, 0120000, SG_DIFF_SIDE_ABSENT, 0);

    CHECK(!sg_diff_entry_is_typechange(&e), "a deletion (new side ABSENT) is never a typechange");
}

static void test_zero_mode_is_not_typechange(void)
{
    sg_diff_entry e1 = make_entry(SG_DIFF_SIDE_BLOB, 0, SG_DIFF_SIDE_BLOB, 0120000);
    sg_diff_entry e2 = make_entry(SG_DIFF_SIDE_BLOB, 0100644, SG_DIFF_SIDE_BLOB, 0);

    CHECK(!sg_diff_entry_is_typechange(&e1), "mode 0 (unknown) on the old side must not be a typechange");
    CHECK(!sg_diff_entry_is_typechange(&e2), "mode 0 (unknown) on the new side must not be a typechange");
}

int main(void)
{
    test_regular_to_symlink();
    test_exec_to_symlink();
    test_submodule_to_regular();
    test_regular_to_exec_is_not_typechange();
    test_old_absent_is_not_typechange();
    test_new_absent_is_not_typechange();
    test_zero_mode_is_not_typechange();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
