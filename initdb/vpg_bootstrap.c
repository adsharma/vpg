/*
 * vpg_bootstrap.c
 *
 * Single-process bootstrap: initialises a fresh PostgreSQL data directory
 * without forking, without pipes, and without signal handlers.
 *
 * This replaces the BootstrapModeMain() call path.  It calls the same
 * internal functions in the same order, but:
 *   - no fork/exec
 *   - no pipe redirection of stdin
 *   - no pqsignal() / sigprocmask() for signal handling
 *   - no proc_exit() — errors are caught with PG_TRY and returned as int
 *
 * Public interface (declared in vpg_bootstrap.h):
 *
 *   int vpg_bootstrap(const char *data_dir,
 *                     const char *bki_file,
 *                     const char *username,
 *                     int         encodingid,
 *                     const char *lc_collate,
 *                     const char *lc_ctype,
 *                     const char *datlocale,
 *                     const char *icu_rules,
 *                     char        locale_provider,
 *                     bool        data_checksums,
 *                     int         wal_segment_size_bytes,
 *                     const char *exec_path);
 *
 * Returns 0 on success, -1 on error (call vpg_bootstrap_error() for message).
 */

#include "postgres.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "access/xlog.h"
#include "access/xact.h"
#include "bootstrap/bootstrap.h"
#include "catalog/pg_collation.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/relmapper.h"

/* flex reentrant API — not in bootstrap.h */
extern int  boot_yyset_in(FILE *f, yyscan_t scanner);

/* BKI token substitution helpers — defined in vpg_initdb_support.c */
extern char **vpg_bki_readfile(const char *path);
extern char **vpg_bki_replace_token(char **lines,
                                    const char *token,
                                    const char *replacement);
extern const char *vpg_bki_encodingid_to_string(int encodingid);
extern const char *vpg_bki_escape_quotes(const char *src);

/* ----------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------- */

static char vpg_bootstrap_errmsg[1024];

const char *
vpg_bootstrap_error(void)
{
    return vpg_bootstrap_errmsg;
}

/* ----------------------------------------------------------------
 * vpg_bootstrap
 * ---------------------------------------------------------------- */

int
vpg_bootstrap(const char *data_dir,
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
              const char *exec_path)
{
    int         rc = 0;
    volatile int saved_stdin_fd = -1;
    volatile int devnull_fd = -1;

    PG_TRY();
    {
        char      **bki_lines;
        char      **line;
        char        buf[64];
        char       *bki_concat;
        size_t      total_len;
        FILE       *bki_stream;
        yyscan_t    scanner;
        uint32      checksum_version;

        /* ---- Step 1: standalone process plumbing ---- */
        InitStandaloneProcess(exec_path);

        /* ---- Step 2: GUC defaults ---- */
        InitializeGUCOptions();

        /* Point GUC at the data directory */
        SetConfigOption("data_directory", data_dir,
                        PGC_POSTMASTER, PGC_S_ARGV);
        SetConfigOption("wal_segment_size",
                        psprintf("%d", wal_segment_size_bytes),
                        PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);

        /* ---- Step 3: read and validate postgresql.conf ---- */
        if (!SelectConfigFiles(data_dir, "vpg"))
            ereport(ERROR,
                    (errmsg("could not read configuration files in \"%s\"",
                            data_dir)));

        /* ---- Step 4: validate and enter the data directory ---- */
        checkDataDir();
        ChangeToDataDir();
        CreateDataDirLockFile(false);

        /* ---- Step 5: bootstrap-mode process flags ---- */
        SetProcessingMode(BootstrapProcessing);
        IgnoreSystemIndexes = true;

        /* ---- Step 6: shared-memory sizing and creation ---- */
        InitializeMaxBackends();
        InitPostmasterChildSlots();
        InitializeFastPathLocks();
        CreateSharedMemoryAndSemaphores();
        set_max_safe_fds();

        /* ---- Step 7: process registration ---- */
        InitProcess();
        BaseInit();

        /* NO signal handlers — we use PG_TRY/PG_CATCH instead */

        /* ---- Step 8: WAL bootstrap ---- */
        checksum_version = data_checksums ? PG_DATA_CHECKSUM_VERSION : 0;
        BootStrapXLOG(checksum_version);

        /* ---- Step 9: catalog bootstrap ---- */
        InitPostgres(NULL, InvalidOid, NULL, InvalidOid, 0, NULL);

        /* ---- Step 10: load and substitute BKI file ---- */
        bki_lines = vpg_bki_readfile(bki_file);
        if (bki_lines == NULL || bki_lines[0] == NULL)
            ereport(ERROR,
                    (errmsg("could not read BKI file \"%s\"", bki_file)));

        /* Verify version header */
        {
            char headerline[256];
            snprintf(headerline, sizeof(headerline),
                     "# PostgreSQL %s\n", PG_MAJORVERSION);
            if (strcmp(headerline, bki_lines[0]) != 0)
                ereport(ERROR,
                        (errmsg("BKI file \"%s\" is for wrong PostgreSQL version",
                                bki_file)));
        }

        /* Token substitutions (same as initdb does) */
        snprintf(buf, sizeof(buf), "%d", NAMEDATALEN);
        bki_lines = vpg_bki_replace_token(bki_lines, "NAMEDATALEN", buf);

        snprintf(buf, sizeof(buf), "%d", (int) sizeof(Pointer));
        bki_lines = vpg_bki_replace_token(bki_lines, "SIZEOF_POINTER", buf);

        bki_lines = vpg_bki_replace_token(bki_lines, "ALIGNOF_POINTER",
                                          (sizeof(Pointer) == 4) ? "i" : "d");
        bki_lines = vpg_bki_replace_token(bki_lines, "FLOAT8PASSBYVAL",
                                          FLOAT8PASSBYVAL ? "true" : "false");

        bki_lines = vpg_bki_replace_token(bki_lines, "POSTGRES",
                                          vpg_bki_escape_quotes(username));
        bki_lines = vpg_bki_replace_token(bki_lines, "ENCODING",
                                          vpg_bki_encodingid_to_string(encodingid));
        bki_lines = vpg_bki_replace_token(bki_lines, "LC_COLLATE",
                                          vpg_bki_escape_quotes(lc_collate));
        bki_lines = vpg_bki_replace_token(bki_lines, "LC_CTYPE",
                                          vpg_bki_escape_quotes(lc_ctype));
        bki_lines = vpg_bki_replace_token(bki_lines, "DATLOCALE",
                                          datlocale ? vpg_bki_escape_quotes(datlocale)
                                                    : "_null_");
        bki_lines = vpg_bki_replace_token(bki_lines, "ICU_RULES",
                                          icu_rules ? vpg_bki_escape_quotes(icu_rules)
                                                    : "_null_");
        snprintf(buf, sizeof(buf), "%c", locale_provider);
        bki_lines = vpg_bki_replace_token(bki_lines, "LOCALE_PROVIDER", buf);

        /* Concatenate lines into a single string for fmemopen */
        total_len = 0;
        for (line = bki_lines; *line != NULL; line++)
            total_len += strlen(*line);

        bki_concat = palloc(total_len + 1);
        bki_concat[0] = '\0';
        for (line = bki_lines; *line != NULL; line++)
        {
            strcat(bki_concat, *line);
            free(*line);
        }
        free(bki_lines);

        /* Open in-memory stream — no temp file */
        bki_stream = fmemopen(bki_concat, total_len, "r");
        if (bki_stream == NULL)
            ereport(ERROR,
                    (errmsg("fmemopen failed: %m")));

        /* ---- Step 11: run the BKI parser ---- */
        if (boot_yylex_init(&scanner) != 0)
            ereport(ERROR, (errmsg("boot_yylex_init failed")));

        boot_yyset_in(bki_stream, scanner);

        /*
         * Keep PostgreSQL source untouched: temporarily make stdin non-tty
         * so bootparse's isatty(0) prompt path stays silent.
         */
        saved_stdin_fd = dup(STDIN_FILENO);
        if (saved_stdin_fd >= 0)
        {
            devnull_fd = open("/dev/null", O_RDONLY);
            if (devnull_fd >= 0)
                (void) dup2(devnull_fd, STDIN_FILENO);
        }

        StartTransactionCommand();
        boot_yyparse(scanner);
        CommitTransactionCommand();

        if (saved_stdin_fd >= 0)
        {
            (void) dup2(saved_stdin_fd, STDIN_FILENO);
            close(saved_stdin_fd);
            saved_stdin_fd = -1;
        }
        if (devnull_fd >= 0)
        {
            close(devnull_fd);
            devnull_fd = -1;
        }

        fclose(bki_stream);
        pfree(bki_concat);

        /* ---- Step 12: finalise relation maps ---- */
        RelationMapFinishBootstrap();
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        if (saved_stdin_fd >= 0)
        {
            (void) dup2(saved_stdin_fd, STDIN_FILENO);
            close(saved_stdin_fd);
            saved_stdin_fd = -1;
        }
        if (devnull_fd >= 0)
        {
            close(devnull_fd);
            devnull_fd = -1;
        }
        snprintf(vpg_bootstrap_errmsg, sizeof(vpg_bootstrap_errmsg),
                 "%s", edata->message ? edata->message : "unknown bootstrap error");
        FlushErrorState();
        FreeErrorData(edata);
        rc = -1;
    }
    PG_END_TRY();

    return rc;
}
