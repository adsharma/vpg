/*
 * vpg_setup.c
 *
 * Post-bootstrap SQL initialisation — no fork, no pipe, no signals.
 *
 * After vpg_bootstrap() returns the process is still live with shared
 * memory and a PGPROC entry, but InitPostgres ran in bootstrap mode and
 * skipped shared-catalog relcache setup, the search path, and ACL init.
 * This file:
 *
 *   1. Switches to NormalProcessing.
 *   2. Re-runs RelationCacheInitializePhase2/3 (bootstrap skipped phase2
 *      for shared catalogs such as pg_authid).
 *   3. Initialises the search path and client encoding inside a transaction.
 *   4. Delegates to vpg_post_bootstrap_sql() (in vpg_initdb.c) which calls
 *      the already-correct setup_*() functions via vpg_run_sql/vpg_run_file.
 */

#include "postgres.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "access/session.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_database.h"
#include "executor/spi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "parser/analyze.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/guc.h"
#include "utils/pg_locale.h"
#include "utils/relcache.h"
#include "utils/relmapper.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "vpg_setup.h"

/* ----------------------------------------------------------------
 * Error state
 * ---------------------------------------------------------------- */

static char vpg_setup_errbuf[1024];

const char *
vpg_setup_error(void)
{
    return vpg_setup_errbuf;
}

/* ----------------------------------------------------------------
 * vpg_run_sql / vpg_run_file  (used by setup_* in vpg_initdb.c)
 * ---------------------------------------------------------------- */

static bool
sql_starts_with(const char *sql, const char *kw)
{
    while (*sql == ' ' || *sql == '\n' || *sql == '\r' || *sql == '\t')
        sql++;
    return pg_strncasecmp(sql, kw, strlen(kw)) == 0;
}

static void
vpg_run_utility_toplevel(const char *sql)
{
    List       *raw_parsetree_list;
    ListCell   *lc;

    raw_parsetree_list = pg_parse_query(sql);

    foreach(lc, raw_parsetree_list)
    {
        RawStmt    *parsetree = lfirst_node(RawStmt, lc);
        List       *stmt_list;
        ListCell   *lc2;

        stmt_list = pg_analyze_and_rewrite_fixedparams(parsetree, sql,
                                                       NULL, 0, NULL);
        stmt_list = pg_plan_queries(stmt_list, sql,
                                    CURSOR_OPT_PARALLEL_OK, NULL);

        foreach(lc2, stmt_list)
        {
            PlannedStmt *stmt = lfirst_node(PlannedStmt, lc2);
            QueryCompletion qc;

            if (stmt->utilityStmt == NULL)
                ereport(ERROR,
                        (errmsg("expected utility statement, got DML: %.120s", sql)));

            CommandCounterIncrement();
            PushActiveSnapshot(GetTransactionSnapshot());
            ProcessUtility(stmt,
                           sql,
                           false,
                           PROCESS_UTILITY_TOPLEVEL,
                           NULL,
                           NULL,
                           None_Receiver,
                           &qc);
            PopActiveSnapshot();
        }
    }
}

void
vpg_run_sql(const char *sql)
{
    int rc;

    StartTransactionCommand();
    PushActiveSnapshot(GetTransactionSnapshot());

    if (sql_starts_with(sql, "VACUUM") ||
        sql_starts_with(sql, "CREATE DATABASE"))
    {
        /*
         * Top-level utility commands are rejected in SPI context with
         * "cannot be executed from a function". Route them through
         * ProcessUtility as PROCESS_UTILITY_TOPLEVEL.
         */
        vpg_run_utility_toplevel(sql);
    }
    else
    {
        rc = SPI_connect();
        if (rc != SPI_OK_CONNECT)
            ereport(ERROR,
                    (errmsg("SPI_connect failed: %s",
                            SPI_result_code_string(rc))));

        rc = SPI_exec(sql, 0);
        if (rc < 0)
            ereport(ERROR,
                    (errmsg("SPI_exec failed (%s) for: %.200s",
                            SPI_result_code_string(rc), sql)));

        SPI_finish();
    }

    PopActiveSnapshot();
    CommitTransactionCommand();
}

void
vpg_run_file(const char *path)
{
    FILE   *f;
    long    len;
    char   *buf;

    f = fopen(path, "r");
    if (!f)
        ereport(ERROR,
                (errmsg("could not open SQL file \"%s\": %m", path)));

    fseek(f, 0, SEEK_END);
    len = ftell(f);
    rewind(f);

    buf = palloc(len + 1);
    if ((long) fread(buf, 1, len, f) != len)
    {
        fclose(f);
        ereport(ERROR,
                (errmsg("could not read SQL file \"%s\": %m", path)));
    }
    buf[len] = '\0';
    fclose(f);

    vpg_run_sql(buf);
    pfree(buf);
}

/*
 * Bootstrap-mode InitPostgres() skips CheckMyDatabase(), which normally sets
 * encoding + locale/collation state used by regex and other locale-sensitive
 * operators.  Recreate the essential parts here.
 */
static void
vpg_setup_database_locale_state(void)
{
    HeapTuple tup;
    Form_pg_database dbform;
    Datum datum;
    char *collate;
    char *ctype;

    tup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
    if (!HeapTupleIsValid(tup))
        elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);

    dbform = (Form_pg_database) GETSTRUCT(tup);

    SetDatabaseEncoding(dbform->encoding);
    SetConfigOption("server_encoding", GetDatabaseEncodingName(),
                    PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);
    SetConfigOption("client_encoding", GetDatabaseEncodingName(),
                    PGC_BACKEND, PGC_S_DYNAMIC_DEFAULT);

    datum = SysCacheGetAttrNotNull(DATABASEOID, tup, Anum_pg_database_datcollate);
    collate = TextDatumGetCString(datum);
    datum = SysCacheGetAttrNotNull(DATABASEOID, tup, Anum_pg_database_datctype);
    ctype = TextDatumGetCString(datum);

    if (pg_perm_setlocale(LC_COLLATE, collate) == NULL)
        ereport(ERROR,
                (errmsg("database locale LC_COLLATE \"%s\" not recognized", collate)));

    if (pg_perm_setlocale(LC_CTYPE, ctype) == NULL)
        ereport(ERROR,
                (errmsg("database locale LC_CTYPE \"%s\" not recognized", ctype)));

    database_ctype_is_c = (strcmp(ctype, "C") == 0 || strcmp(ctype, "POSIX") == 0);
    init_database_collation();

    ReleaseSysCache(tup);
}

/* ----------------------------------------------------------------
 * vpg_setup — public entry point
 * ---------------------------------------------------------------- */

int
vpg_setup(const char *username,
          const char *system_constraints_file,
          const char *system_functions_file,
          const char *system_views_file,
          const char *dictionary_file,
          const char *info_schema_file,
          const char *features_file,
          const char *infoversion)
{
    int rc = 0;

    /*
     * Switch from BootstrapProcessing to NormalProcessing so the executor,
     * SPI, and catalog name lookups all work correctly.
     */
    SetProcessingMode(NormalProcessing);
    IgnoreSystemIndexes = false;
    allowSystemTableMods = true;   /* initdb needs to modify system catalogs */

    /*
     * RelationCacheInitializePhase2() has an early return in bootstrap mode,
     * so shared catalogs (pg_authid, pg_database, ...) were never nailed.
     * Re-run both phases now that we are in NormalProcessing.
     */
    RelationMapInitializePhase2();
    RelationCacheInitializePhase2();

    PG_TRY();
    {
        /*
         * Phase3 and search-path initialisation need an active transaction
         * because they read pg_namespace / pg_database.
         */
        StartTransactionCommand();
        RelationCacheInitializePhase3();
        vpg_setup_database_locale_state();
        InitializeSearchPath();
        InitializeClientEncoding();
        InitializeSession();
        CommitTransactionCommand();

        /* Run all post-bootstrap SQL via the setup_* functions in vpg_initdb.c */
        vpg_post_bootstrap_sql(username,
                               system_constraints_file,
                               system_functions_file,
                               system_views_file,
                               dictionary_file,
                               info_schema_file,
                               features_file,
                               infoversion);
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        snprintf(vpg_setup_errbuf, sizeof(vpg_setup_errbuf),
                 "%s", edata->message ? edata->message : "unknown setup error");
        FlushErrorState();
        rc = -1;
    }
    PG_END_TRY();

    return rc;
}
