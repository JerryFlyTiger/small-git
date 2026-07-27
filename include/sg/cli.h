#ifndef SG_CLI_H
#define SG_CLI_H

/* Each sg_cmd_* receives argv shifted so that argv[0] is the subcommand name
   itself (e.g. "init"), matching conventional getopt-style usage. */
int sg_cmd_init(int argc, char **argv);
int sg_cmd_hash_object(int argc, char **argv);
int sg_cmd_cat_file(int argc, char **argv);

int sg_cli_run(int argc, char **argv);

#endif
