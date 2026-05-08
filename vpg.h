#ifndef VPG_H
#define VPG_H

#include <stdbool.h>

void vpg_initdb_options(const char *data_dir,
                        const char *username,
                        const char *auth,
                        const char *encoding,
                        const char *locale,
                        bool no_instructions);
void vpg_backend_start_options(const char *data_dir,
                               const char *username,
                               const char *dbname,
                               const char *shared_preload_libraries);
void vpg_set_exec_path(const char *path);
void vpg_set_python_error(const char *message);
const char *vpg_python_error(void);
const char *vpg_exec(const char *query);
int vpg_vacuum(void);
int vpg_analyze(void);
int vpg_maintain(void);
const char *vpg_last_error_message(void);
void vpg_finish(void);
void vpg_free(void *ptr);

#endif
