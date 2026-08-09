#ifndef SG_CLI_H
#define SG_CLI_H

#include "sg/version.h"

/* Each sg_cmd_* receives argv shifted so that argv[0] is the subcommand name
   itself (e.g. "init"), matching conventional getopt-style usage. */
int sg_cmd_init(int argc, char **argv);
int sg_cmd_hash_object(int argc, char **argv);
int sg_cmd_cat_file(int argc, char **argv);
int sg_cmd_add(int argc, char **argv);
int sg_cmd_commit(int argc, char **argv);
int sg_cmd_log(int argc, char **argv);
int sg_cmd_status(int argc, char **argv);
int sg_cmd_diff(int argc, char **argv);
int sg_cmd_branch(int argc, char **argv);
int sg_cmd_tag(int argc, char **argv);
int sg_cmd_switch(int argc, char **argv);
int sg_cmd_restore(int argc, char **argv);
int sg_cmd_reset(int argc, char **argv);
int sg_cmd_undo(int argc, char **argv);
int sg_cmd_repack(int argc, char **argv);
int sg_cmd_merge(int argc, char **argv);
int sg_cmd_merge_base(int argc, char **argv);
int sg_cmd_rebase(int argc, char **argv);
int sg_cmd_clone(int argc, char **argv);
int sg_cmd_fetch(int argc, char **argv);
int sg_cmd_push(int argc, char **argv);
int sg_cmd_chunk_info(int argc, char **argv);

int sg_cli_run(int argc, char **argv);

#endif
