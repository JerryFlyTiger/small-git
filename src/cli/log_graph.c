#include "sg/log_graph.h"

void sg_log_graph_init(sg_log_graph_prefixer *pfx)
{
    pfx->at_line_start = 1;
    pfx->entry_first_line = 0;
}

void sg_log_graph_begin_entry(sg_log_graph_prefixer *pfx)
{
    pfx->entry_first_line = 1;
}

void sg_log_graph_write(sg_log_graph_prefixer *pfx, const char *buf, size_t len, const char *cont,
                         FILE *out)
{
    size_t i;

    for (i = 0; i < len; i++) {
        char c = buf[i];

        if (pfx->at_line_start) {
            fputs(pfx->entry_first_line ? "* " : cont, out);
            pfx->entry_first_line = 0;
            pfx->at_line_start = 0;
        }
        fputc(c, out);
        if (c == '\n')
            pfx->at_line_start = 1;
    }
}

void sg_log_graph_write_entry(sg_log_graph_prefixer *pfx, const char *buf, size_t len,
                               const char *cont, FILE *out)
{
    sg_log_graph_begin_entry(pfx);
    if (len == 0) {
        /* sg_log_graph_write's byte loop never runs for len == 0, so it
           has no byte to hang a prefix on -- this entry's marker must be
           emitted explicitly. It is unconditionally "* ", never `cont`:
           this call just set entry_first_line to 1, and an entry with no
           bytes at all has, by construction, nothing but a first line. */
        fputs("* ", out);
        pfx->entry_first_line = 0;
        pfx->at_line_start = 0;
        return;
    }
    sg_log_graph_write(pfx, buf, len, cont, out);
}
