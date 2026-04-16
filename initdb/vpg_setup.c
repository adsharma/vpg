/*
 * vpg_setup.c
 *
 * Post-bootstrap SQL initialisation — no fork, no pipe, no signals.
 *
 * Called immediately after vpg_bootstrap() returns, while still in the same
 * process.  At entry shared memory is live, MyProc is registered, and
 * InitPostgres() has already run in bootstrap mode.  All we do is:
 *
 *   1. Switch to NormalProcessing so the executor / SPI work.
 *   2. Run each SQL block that initdb's "--single" pass used to pipe into
 *      a forked backend.
 *   3. Return.  No teardown here — the caller (vpg_initdb_run) handles that.
 *
 * Errors are caught with PG_TRY and returned as -1; call
 * vpg_setup_error() for the message.
 */

#include "postgres.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_collation.h"
#include "executor/spi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/elog.h"
#include "utils/relcache.h"
#include "utils/relmapper.h"
#include "utils/snapmgr.h"

/* ----------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------- */

static char vpg_setup_errbuf[1024];

const char *
vpg_setup_error(void)
{
    return vpg_setup_errbuf;
}

/*
 * Execute a single SQL string inside its own transaction.
 * Errors propagate as PG exceptions — the caller's PG_TRY catches them.
 */
static void
run_sql(const char *sql)
{
    int rc;

    StartTransactionCommand();
    PushActiveSnapshot(GetTransactionSnapshot());

    rc = SPI_connect();
    if (rc != SPI_OK_CONNECT)
        ereport(ERROR,
                (errmsg("SPI_connect failed: %s",
                        SPI_result_code_string(rc))));

    rc = SPI_exec(sql, 0);
    if (rc < 0)
        ereport(ERROR,
                (errmsg("SPI_exec failed (%s) for: %.120s",
                        SPI_result_code_string(rc), sql)));

    SPI_finish();
    PopActiveSnapshot();
    CommitTransactionCommand();
}

/*
 * Read a SQL file and execute it as one block.
 */
static void
run_file(const char *path)
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

    run_sql(buf);
    pfree(buf);
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
     * We are still in the same process as vpg_bootstrap().
     * Shared memory is live.  Switch to normal processing so the
     * executor, SPI, and catalog lookups all work correctly.
     */
    SetProcessingMode(NormalProcessing);
    IgnoreSystemIndexes = false;
    allowSystemTableMods = true;   /* initdb needs to modify system catalogs */

    SetProcessingMode(NormalProcessing);
    IgnoreSystemIndexes = false;
    allowSystemTableMods = true;   /* initdb needs to modify system catalogs */

    /*
     * RelationCacheInitializePhase2() skipped shared catalogs during
     * bootstrap (it has an early-return for IsBootstrapProcessingMode).
     * Now that we're in NormalProcessing, run both phases so pg_authid
     * and other shared catalogs are nailed into the relcache properly.
     */
    RelationMapInitializePhase2();
    RelationCacheInitializePhase2();

    PG_TRY();
    {
        char *sql;

        /*
         * Phase3 and search-path setup need an active transaction.
         */
        StartTransactionCommand();
        RelationCacheInitializePhase3();
        InitializeSearchPath();
        InitializeClientEncoding();
        CommitTransactionCommand();

        /* setup_auth: revoke public access to pg_authid */
        run_sql("REVOKE ALL ON pg_authid FROM public;");

        run_file(system_constraints_file);

        run_file(system_functions_file);

        /* setup_depend: stop pinning newly-created objects */
        run_sql("SELECT pg_stop_making_pinned_objects();");

        run_file(system_views_file);

        run_sql(
            "WITH funcdescs AS ("
            "  SELECT p.oid AS p_oid, o.oid AS o_oid, oprname"
            "  FROM pg_proc p JOIN pg_operator o ON oprcode = p.oid)"
            "INSERT INTO pg_description"
            "  SELECT p_oid, 'pg_proc'::regclass, 0,"
            "    'implementation of ' || oprname || ' operator'"
            "  FROM funcdescs"
            "  WHERE NOT EXISTS ("
            "    SELECT 1 FROM pg_description"
            "    WHERE objoid = p_oid AND classoid = 'pg_proc'::regclass)"
            "  AND NOT EXISTS ("
            "    SELECT 1 FROM pg_description"
            "    WHERE objoid = o_oid AND classoid = 'pg_operator'::regclass"
            "    AND description LIKE 'deprecated%');");

        run_sql("UPDATE pg_collation"
                "  SET collversion = pg_collation_actual_version(oid)"
                "  WHERE collname = 'unicode';");
        run_sql("SELECT pg_import_system_collations('pg_catalog');");

        run_file(dictionary_file);

        run_sql(
            "UPDATE pg_class"
            "  SET relacl = (SELECT array_agg(a.acl) FROM"
            "    (SELECT E'=r/\"' || current_user || E'\"' AS acl"
            "    UNION SELECT unnest(relacl)) a)"
            "  WHERE relkind IN ('r','v','m','S','f','p')"
            "  AND relacl IS NOT NULL"
            "  AND pg_catalog.array_position(relacl,"
            "       (E'=r/\"' || current_user || E'\"')::aclitem) IS NULL;");
        run_sql("GRANT USAGE ON SCHEMA pg_catalog, public TO PUBLIC;");
        run_sql("REVOKE ALL ON pg_largeobject FROM PUBLIC;");

        /* pg_init_privs rows for the above grants */
        run_sql(
            "INSERT INTO pg_init_privs"
            "  (objoid, classoid, objsubid, initprivs)"
            "  SELECT oid, 'pg_class'::regclass, 0, relacl"
            "  FROM pg_class"
            "  WHERE relacl IS NOT NULL"
            "  AND relkind IN ('r','v','m','S','f','p');");
        run_sql(
            "INSERT INTO pg_init_privs"
            "  (objoid, classoid, objsubid, initprivs)"
            "  SELECT pg_class.oid, 'pg_class'::regclass, pg_attribute.attnum,"
            "    pg_attribute.attacl"
            "  FROM pg_class JOIN pg_attribute ON pg_class.oid = attrelid"
            "  WHERE pg_attribute.attacl IS NOT NULL"
            "  AND pg_class.relkind IN ('r','v','m','S','f','p');");
        run_sql(
            "INSERT INTO pg_init_privs"
            "  (objoid, classoid, objsubid, initprivs)"
            "  SELECT oid, 'pg_proc'::regclass, 0, proacl"
            "  FROM pg_proc WHERE proacl IS NOT NULL;");
        run_sql(
            "INSERT INTO pg_init_privs"
            "  (objoid, classoid, objsubid, initprivs)"
            "  SELECT oid, 'pg_type'::regclass, 0, typacl"
            "  FROM pg_type WHERE typacl IS NOT NULL;");
        run_sql(
            "INSERT INTO pg_init_privs"
            "  (objoid, classoid, objsubid, initprivs)"
            "  SELECT oid, 'pg_language'::regclass, 0, lanacl"
            "  FROM pg_language WHERE lanacl IS NOT NULL;");
        run_sql(
            "INSERT INTO pg_init_privs"
            "  (objoid, classoid, objsubid, initprivs)"
            "  SELECT oid, 'pg_largeobject_metadata'::regclass, 0, lomacl"
            "  FROM pg_largeobject_metadata WHERE lomacl IS NOT NULL;");
        run_sql(
            "INSERT INTO pg_init_privs"
            "  (objoid, classoid, objsubid, initprivs)"
            "  SELECT oid, 'pg_namespace'::regclass, 0, nspacl"
            "  FROM pg_namespace WHERE nspacl IS NOT NULL;");
        run_sql(
            "INSERT INTO pg_init_privs"
            "  (objoid, classoid, objsubid, initprivs)"
            "  SELECT oid, 'pg_foreign_data_wrapper'::regclass, 0, fdwacl"
            "  FROM pg_foreign_data_wrapper WHERE fdwacl IS NOT NULL;");
        run_sql(
            "INSERT INTO pg_init_privs"
            "  (objoid, classoid, objsubid, initprivs)"
            "  SELECT oid, 'pg_foreign_server'::regclass, 0, srvacl"
            "  FROM pg_foreign_server WHERE srvacl IS NOT NULL;");

        run_file(info_schema_file);

        sql = psprintf(
            "UPDATE information_schema.sql_implementation_info"
            "  SET character_value = '%s'"
            "  WHERE implementation_info_name = 'DBMS VERSION';",
            infoversion);
        run_sql(sql);
        pfree(sql);

        sql = psprintf(
            "COPY information_schema.sql_features"
            "  (feature_id, feature_name, sub_feature_id,"
            "   sub_feature_name, is_supported, comments)"
            "  FROM E'%s';",
            features_file);
        run_sql(sql);
        pfree(sql);

        run_sql("CREATE EXTENSION plpgsql;");

        run_sql("ANALYZE;");
        run_sql("VACUUM FREEZE;");

        run_sql(
            "CREATE DATABASE template0"
            "  IS_TEMPLATE = true ALLOW_CONNECTIONS = false"
            "  OID = 4 STRATEGY = file_copy;");
        run_sql("UPDATE pg_database SET datcollversion = NULL"
                "  WHERE datname = 'template0';");
        run_sql("UPDATE pg_database"
                "  SET datcollversion ="
                "    pg_database_collation_actual_version(oid)"
                "  WHERE datname = 'template1';");
        run_sql("REVOKE CREATE,TEMPORARY ON DATABASE template1 FROM public;");
        run_sql("REVOKE CREATE,TEMPORARY ON DATABASE template0 FROM public;");
        run_sql("COMMENT ON DATABASE template0 IS 'unmodifiable empty database';");
        run_sql("VACUUM pg_database;");

        run_sql(
            "CREATE DATABASE postgres"
            "  OID = 5 STRATEGY = file_copy;");
        run_sql("COMMENT ON DATABASE postgres IS"
                "  'default administrative connection database';");

    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        snprintf(vpg_setup_errbuf, sizeof(vpg_setup_errbuf),
                 "%s", edata->message ? edata->message : "unknown setup error");
        FlushErrorState();
        /* Do NOT call FreeErrorData here — memory context may be gone */
        rc = -1;
    }
    PG_END_TRY();

    return rc;
}
