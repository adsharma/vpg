#define _GNU_SOURCE

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach-o/dyld.h>

#include "postgres.h"

#include "access/xact.h"
#include "executor/spi.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"

static bool vpg_initialized = false;
static bool vpg_runtime_ready = false;
static char *vpg_last_error = NULL;
static char *vpg_exec_path = NULL;
const char *progname = "vpg";

DispatchOption
parse_dispatch_option(const char *name)
{
	if (strcmp(name, "check") == 0)
		return DISPATCH_CHECK;
	if (strcmp(name, "boot") == 0)
		return DISPATCH_BOOT;
	if (strcmp(name, "forkchild") == 0)
		return DISPATCH_FORKCHILD;
	if (strcmp(name, "describe-config") == 0)
		return DISPATCH_DESCRIBE_CONFIG;
	if (strcmp(name, "single") == 0)
		return DISPATCH_SINGLE;
	return DISPATCH_POSTMASTER;
}

static void
vpg_replace_owned_string(char **slot, const char *value)
{
	if (*slot != NULL)
		free(*slot);

	*slot = value != NULL ? strdup(value) : NULL;
}

static void
vpg_capture_current_error(void)
{
	ErrorData  *edata;
	char	   *message;

	edata = CopyErrorData();
	message = edata->message ? edata->message : "unknown PostgreSQL error";
	vpg_replace_owned_string(&vpg_last_error, message);
	FlushErrorState();
	FreeErrorData(edata);
}

static char *
vpg_strdup_result(const char *value)
{
	return strdup(value != NULL ? value : "");
}

static const char *
vpg_get_exec_path(void)
{
	uint32_t size = 0;

	if (vpg_exec_path != NULL)
		return vpg_exec_path;

	_NSGetExecutablePath(NULL, &size);
	vpg_exec_path = malloc(size);
	if (vpg_exec_path == NULL)
		return "vpg";

	if (_NSGetExecutablePath(vpg_exec_path, &size) != 0)
	{
		free(vpg_exec_path);
		vpg_exec_path = NULL;
		return "vpg";
	}

	return vpg_exec_path;
}

static void
vpg_runtime_init(void)
{
	if (vpg_runtime_ready)
		return;

	MyProcPid = getpid();
	MemoryContextInit();
	(void) set_stack_base();
	set_pglocale_pgservice(vpg_get_exec_path(), PG_TEXTDOMAIN("postgres"));
	vpg_runtime_ready = true;
}

void
vpg_free(void *ptr)
{
	free(ptr);
}

const char *
vpg_last_error_message(void)
{
	return vpg_last_error;
}

void
vpg_init(const char *data_dir, const char *username, const char *dbname)
{
	char	   *av[10];
	int			ac = 0;
	const char *effective_dbname;

	if (vpg_initialized)
		return;

	vpg_replace_owned_string(&vpg_last_error, NULL);
	vpg_runtime_init();

		av[ac++] = (char *) vpg_get_exec_path();
	av[ac++] = "-D";
	av[ac++] = (char *) data_dir;
	av[ac++] = "-c";
	av[ac++] = "shared_preload_libraries=";
	av[ac] = NULL;

	effective_dbname = (dbname != NULL && dbname[0] != '\0') ? dbname : username;

	PG_TRY();
	{
		Assert(!IsUnderPostmaster);

		progname = av[0];
		InitStandaloneProcess(av[0]);
		InitializeGUCOptions();
		process_postgres_switches(ac, av, PGC_POSTMASTER, NULL);

		if (effective_dbname == NULL || effective_dbname[0] == '\0')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("no database name or username provided")));

		if (!SelectConfigFiles(data_dir, progname))
			proc_exit(1);

		checkDataDir();
		ChangeToDataDir();
		CreateDataDirLockFile(false);
		LocalProcessControlFile(false);
		process_shared_preload_libraries();
		InitializeMaxBackends();
		InitPostmasterChildSlots();
		InitializeFastPathLocks();
		process_shmem_requests();
		InitializeShmemGUCs();
		InitializeWalConsistencyChecking();
		CreateSharedMemoryAndSemaphores();
		set_max_safe_fds();
		PgStartTime = GetCurrentTimestamp();
		InitProcess();

		pqsignal(SIGHUP, SIG_IGN);
		pqsignal(SIGINT, StatementCancelHandler);
		pqsignal(SIGTERM, die);
		pqsignal(SIGQUIT, die);
		pqsignal(SIGPIPE, SIG_IGN);
		pqsignal(SIGUSR1, procsignal_sigusr1_handler);
		pqsignal(SIGUSR2, SIG_IGN);
		pqsignal(SIGFPE, FloatExceptionHandler);
		pqsignal(SIGCHLD, SIG_DFL);
		InitializeTimeouts();

		whereToSendOutput = DestNone;
		BaseInit();
		sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);
		InitPostgres(effective_dbname, InvalidOid, username, InvalidOid, 0, NULL);
		SetProcessingMode(NormalProcessing);

		vpg_initialized = true;
	}
	PG_CATCH();
	{
		vpg_capture_current_error();
		vpg_initialized = false;
	}
	PG_END_TRY();
}

const char *
vpg_exec(const char *query)
{
	StringInfoData buf;
	char	   *result;

	if (!vpg_initialized)
	{
		vpg_replace_owned_string(&vpg_last_error, "backend not initialized");
		return NULL;
	}

	vpg_replace_owned_string(&vpg_last_error, NULL);
	initStringInfo(&buf);
	result = NULL;

	PG_TRY();
	{
		int			spi_rc;
		int			exec_rc;

		StartTransactionCommand();
		PushActiveSnapshot(GetTransactionSnapshot());
		spi_rc = SPI_connect();
		if (spi_rc != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed: %s", SPI_result_code_string(spi_rc));

		exec_rc = SPI_exec(query, 0);
		if (exec_rc < 0)
			elog(ERROR, "SPI_exec failed: %s", SPI_result_code_string(exec_rc));

		if (SPI_tuptable != NULL)
		{
			TupleDesc	tupdesc = SPI_tuptable->tupdesc;
			uint64		row;

			for (int col = 0; col < tupdesc->natts; col++)
			{
				appendStringInfoString(&buf, NameStr(TupleDescAttr(tupdesc, col)->attname));
				if (col + 1 < tupdesc->natts)
					appendStringInfoChar(&buf, ',');
			}
			appendStringInfoChar(&buf, '\n');

			for (row = 0; row < SPI_processed; row++)
			{
				HeapTuple	tuple = SPI_tuptable->vals[row];

				for (int col = 0; col < tupdesc->natts; col++)
				{
					char   *value = SPI_getvalue(tuple, tupdesc, col + 1);

					if (value != NULL)
						appendStringInfoString(&buf, value);
					else
						appendStringInfoString(&buf, "null");

					if (col + 1 < tupdesc->natts)
						appendStringInfoChar(&buf, ',');
				}
				appendStringInfoChar(&buf, '\n');
			}
		}
		else
		{
			appendStringInfo(&buf, "OK %llu\n", (unsigned long long) SPI_processed);
		}

		if (SPI_finish() != SPI_OK_FINISH)
			elog(ERROR, "SPI_finish failed");

		PopActiveSnapshot();
		CommitTransactionCommand();
		result = vpg_strdup_result(buf.data);
	}
	PG_CATCH();
	{
		vpg_capture_current_error();
		HOLD_INTERRUPTS();
		if (ActiveSnapshotSet())
			PopActiveSnapshot();
		AbortCurrentTransaction();
		RESUME_INTERRUPTS();
		result = NULL;
	}
	PG_END_TRY();

	pfree(buf.data);
	return result;
}

void
vpg_finish(void)
{
	vpg_initialized = false;
}
