#ifndef VPG_SETUP_H
#define VPG_SETUP_H

/*
 * Execute a single SQL string in its own transaction (no fork, no pipe).
 * Errors propagate as PG exceptions — the caller's PG_TRY catches them.
 */
void        vpg_run_sql(const char *sql);

/* Read a file and execute its contents as a single SPI_exec block. */
void        vpg_run_file(const char *path);

/*
 * Run all post-bootstrap SQL (system_constraints, system_functions,
 * system_views, information_schema, plpgsql, VACUUM, template0/postgres).
 * Called by vpg_setup() after the relcache and search-path are ready.
 * Implemented in vpg_initdb.c so it can call the static setup_* functions.
 */
void        vpg_post_bootstrap_sql(const char *username,
                                   const char *system_constraints_file,
                                   const char *system_functions_file,
                                   const char *system_views_file,
                                   const char *dictionary_file,
                                   const char *info_schema_file,
                                   const char *features_file,
                                   const char *infoversion);

/*
 * Top-level entry point: switch to NormalProcessing, reinit the relcache
 * for shared catalogs, set up the search path, then call
 * vpg_post_bootstrap_sql().  Returns 0 on success, -1 on error.
 */
int         vpg_setup(const char *username,
                      const char *system_constraints_file,
                      const char *system_functions_file,
                      const char *system_views_file,
                      const char *dictionary_file,
                      const char *info_schema_file,
                      const char *features_file,
                      const char *infoversion);

const char *vpg_setup_error(void);

#endif
