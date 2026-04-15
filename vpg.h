#ifndef VPG_H
#define VPG_H

void vpg_init(const char *data_dir, const char *username, const char *dbname);
const char *vpg_exec(const char *query);
const char *vpg_last_error_message(void);
void vpg_finish(void);
void vpg_free(void *ptr);

#endif
