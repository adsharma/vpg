#ifndef VPG_BOOTSTRAP_H
#define VPG_BOOTSTRAP_H

#include <stdbool.h>

/*
 * Run the PostgreSQL bootstrap phase against an already-created data
 * directory (directories exist, postgresql.conf written).
 *
 * Returns 0 on success, -1 on error.  Call vpg_bootstrap_error() to
 * retrieve the error message.
 */
int  vpg_bootstrap(const char *data_dir,
                   const char *bki_file,
                   const char *username,
                   int         encodingid,
                   const char *lc_collate,
                   const char *lc_ctype,
                   const char *datlocale,
                   const char *icu_rules,
                   char        locale_provider,
                   bool        data_checksums,
                   int         wal_segment_size_bytes,
                   const char *exec_path);

const char *vpg_bootstrap_error(void);

#endif /* VPG_BOOTSTRAP_H */
