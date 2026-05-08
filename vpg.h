#ifndef VPG_H
#define VPG_H

#include <stdbool.h>

void vpg_initdb(const char *data_dir, const char *username);
void vpg_initdb_options(const char *data_dir,
                        const char *username,
                        const char *auth,
                        const char *encoding,
                        const char *locale,
                        bool no_instructions);
void vpg_init(const char *data_dir, const char *username, const char *dbname);
void vpg_set_exec_path(const char *path);
const char *vpg_exec(const char *query);
const char *vpg_last_error_message(void);
void vpg_finish(void);
void vpg_free(void *ptr);

#endif
