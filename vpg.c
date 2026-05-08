#define _GNU_SOURCE

#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "postgres.h"

#include "access/xlog.h"
#include "access/xact.h"
#include "commands/vacuum.h"
#include "executor/spi.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgwriter.h"
#include "postmaster/postmaster.h"
#include "storage/bufmgr.h"
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
static char *vpg_python_last_error = NULL;
static char *vpg_exec_path = NULL;
static pthread_mutex_t vpg_mutex = PTHREAD_MUTEX_INITIALIZER;
static int vpg_connection_count = 0;
static char *vpg_active_data_dir = NULL;
const char *progname = "vpg";

typedef struct VPGConnection
{
	bool		open;
	char	   *data_dir;
	char	   *username;
	char	   *dbname;
} VPGConnection;

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

static bool
vpg_valid_connection(VPGConnection *conn)
{
	return conn != NULL && conn->open;
}

const char *
vpg_get_exec_path(void)
{
	if (vpg_exec_path != NULL)
		return vpg_exec_path;

#ifdef __APPLE__
	uint32_t size = 0;

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
#else
	{
		char path[1024];
		ssize_t len;

		len = readlink("/proc/self/exe", path, sizeof(path) - 1);
		if (len < 0)
			return "vpg";

		path[len] = '\0';
		vpg_exec_path = strdup(path);
		if (vpg_exec_path == NULL)
			return "vpg";

		return vpg_exec_path;
	}
#endif
}

void
vpg_set_exec_path(const char *path)
{
	if (path == NULL || path[0] == '\0')
		return;

	vpg_replace_owned_string(&vpg_exec_path, path);
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

static void
vpg_backend_start_options_unlocked(const char *data_dir,
								   const char *username,
								   const char *dbname,
								   const char *shared_preload_libraries);
static const char *vpg_exec_unlocked(const char *query);
static int vpg_run_vacuum_unlocked(bits32 options);
static void vpg_finish_unlocked(void);

const char *
vpg_last_error_message(void)
{
	return vpg_last_error;
}

void
vpg_set_python_error(const char *message)
{
	vpg_replace_owned_string(&vpg_python_last_error,
							 message != NULL ? message : "unknown vpg python error");
}

const char *
vpg_python_error(void)
{
	if (vpg_python_last_error != NULL)
		return vpg_python_last_error;
	return vpg_last_error;
}

extern int vpg_initdb_run(int argc, char *argv[]);  /* from vpg_initdb.c */

void
vpg_initdb_options(const char *data_dir,
				   const char *username,
				   const char *auth,
				   const char *encoding,
				   const char *locale,
				   bool no_instructions)
{
	char *av[16];
	char  auth_arg[128];
	char  encoding_arg[128];
	char  locale_arg[128];
	int   ac = 0;

	vpg_replace_owned_string(&vpg_last_error, NULL);
	vpg_runtime_init();   /* ensure MemoryContextInit before any palloc */
	pqsignal(SIGUSR1, SIG_IGN);

	snprintf(auth_arg, sizeof(auth_arg), "--auth=%s",
			 auth != NULL && auth[0] != '\0' ? auth : "trust");
	snprintf(encoding_arg, sizeof(encoding_arg), "--encoding=%s",
			 encoding != NULL && encoding[0] != '\0' ? encoding : "UTF8");
	snprintf(locale_arg, sizeof(locale_arg), "--locale=%s",
			 locale != NULL && locale[0] != '\0' ? locale : "C");

	av[ac++] = (char *) vpg_get_exec_path();
	av[ac++] = "-D";
	av[ac++] = (char *) data_dir;
	av[ac++] = "-U";
	av[ac++] = (char *) username;
	av[ac++] = auth_arg;
	av[ac++] = encoding_arg;
	av[ac++] = locale_arg;
	if (no_instructions)
		av[ac++] = "--no-instructions";
	av[ac]   = NULL;

	int rc = vpg_initdb_run(ac, av);
	if (rc != 0)
		vpg_replace_owned_string(&vpg_last_error, "vpg_initdb failed");
	else
	{
		pqsignal(SIGUSR1, procsignal_sigusr1_handler);
		vpg_replace_owned_string(&vpg_active_data_dir, data_dir);
		vpg_initialized = true;
	}
}

void
vpg_backend_start_options(const char *data_dir,
						  const char *username,
						  const char *dbname,
						  const char *shared_preload_libraries)
{
	pthread_mutex_lock(&vpg_mutex);
	vpg_backend_start_options_unlocked(data_dir, username, dbname, shared_preload_libraries);
	pthread_mutex_unlock(&vpg_mutex);
}

static void
vpg_backend_start_options_unlocked(const char *data_dir,
								   const char *username,
								   const char *dbname,
								   const char *shared_preload_libraries)
{
	if (vpg_initialized)
		return;

	vpg_replace_owned_string(&vpg_last_error, NULL);
	vpg_runtime_init();

	PG_TRY();
	{
		Assert(!IsUnderPostmaster);

		progname = vpg_get_exec_path();
		InitStandaloneProcess(progname);
		InitializeGUCOptions();
		SetConfigOption("data_directory", data_dir, PGC_POSTMASTER, PGC_S_ARGV);
		SetConfigOption("shared_preload_libraries",
						shared_preload_libraries != NULL ? shared_preload_libraries : "",
						PGC_POSTMASTER, PGC_S_ARGV);

		if (dbname == NULL || dbname[0] == '\0')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("no database name provided")));

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
		InitPostgres(dbname, InvalidOid, username, InvalidOid, 0, NULL);
		SetProcessingMode(NormalProcessing);

		vpg_replace_owned_string(&vpg_active_data_dir, data_dir);
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
	const char *result;

	pthread_mutex_lock(&vpg_mutex);
	result = vpg_exec_unlocked(query);
	pthread_mutex_unlock(&vpg_mutex);
	return result;
}

static const char *
vpg_exec_unlocked(const char *query)
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

static int
vpg_run_vacuum(bits32 options)
{
	int rc;

	pthread_mutex_lock(&vpg_mutex);
	rc = vpg_run_vacuum_unlocked(options);
	pthread_mutex_unlock(&vpg_mutex);
	return rc;
}

static int
vpg_run_vacuum_unlocked(bits32 options)
{
	int			rc = 0;

	if (!vpg_initialized)
	{
		vpg_replace_owned_string(&vpg_last_error, "backend not initialized");
		return -1;
	}

	vpg_replace_owned_string(&vpg_last_error, NULL);

	PG_TRY();
	{
		VacuumParams params;
		BufferAccessStrategy bstrategy = NULL;
		MemoryContext vac_context;
		MemoryContext old_context;

		memset(&params, 0, sizeof(params));
		params.options = options;
		params.freeze_min_age = -1;
		params.freeze_table_age = -1;
		params.multixact_freeze_min_age = -1;
		params.multixact_freeze_table_age = -1;
		params.log_min_duration = -1;
		params.index_cleanup = VACOPTVALUE_UNSPECIFIED;
		params.truncate = VACOPTVALUE_UNSPECIFIED;
		params.toast_parent = InvalidOid;
		params.max_eager_freeze_failure_rate = vacuum_max_eager_freeze_failure_rate;
		params.nworkers = 0;

		StartTransactionCommand();

		vac_context = AllocSetContextCreate(PortalContext,
											"vpg vacuum",
											ALLOCSET_DEFAULT_SIZES);
		old_context = MemoryContextSwitchTo(vac_context);
		bstrategy = GetAccessStrategyWithSize(BAS_VACUUM, VacuumBufferUsageLimit);
		MemoryContextSwitchTo(old_context);

		vacuum(NIL, &params, bstrategy, vac_context, true);
		MemoryContextDelete(vac_context);

		CommitTransactionCommand();
	}
	PG_CATCH();
	{
		vpg_capture_current_error();
		HOLD_INTERRUPTS();
		AbortCurrentTransaction();
		RESUME_INTERRUPTS();
		rc = -1;
	}
	PG_END_TRY();

	return rc;
}

int
vpg_vacuum(void)
{
	return vpg_run_vacuum(VACOPT_VACUUM |
						  VACOPT_PROCESS_MAIN |
						  VACOPT_PROCESS_TOAST);
}

int
vpg_analyze(void)
{
	return vpg_run_vacuum(VACOPT_ANALYZE |
						  VACOPT_PROCESS_MAIN |
						  VACOPT_PROCESS_TOAST);
}

int
vpg_maintain(void)
{
	int rc = 0;

	pthread_mutex_lock(&vpg_mutex);

	if (vpg_run_vacuum_unlocked(VACOPT_VACUUM |
								VACOPT_ANALYZE |
								VACOPT_PROCESS_MAIN |
								VACOPT_PROCESS_TOAST) != 0)
	{
		pthread_mutex_unlock(&vpg_mutex);
		return -1;
	}

	PG_TRY();
	{
		StartTransactionCommand();
		/* Force dirty buffers and WAL state to disk before returning. */
		RequestCheckpoint(CHECKPOINT_IMMEDIATE |
						  CHECKPOINT_FORCE |
						  CHECKPOINT_WAIT);
		CommitTransactionCommand();
	}
	PG_CATCH();
	{
		vpg_capture_current_error();
		AbortCurrentTransaction();
		rc = -1;
	}
	PG_END_TRY();

	pthread_mutex_unlock(&vpg_mutex);
	return rc;
}

void *
vpg_connect_options(const char *data_dir,
					const char *username,
					const char *dbname,
					const char *shared_preload_libraries)
{
	VPGConnection *conn;

	pthread_mutex_lock(&vpg_mutex);
	vpg_replace_owned_string(&vpg_last_error, NULL);

	if (data_dir == NULL || data_dir[0] == '\0')
	{
		vpg_replace_owned_string(&vpg_last_error, "data directory is required");
		pthread_mutex_unlock(&vpg_mutex);
		return NULL;
	}

	if (vpg_initialized &&
		vpg_active_data_dir != NULL &&
		strcmp(vpg_active_data_dir, data_dir) != 0)
	{
		vpg_replace_owned_string(&vpg_last_error,
								 "only one embedded data directory can be open in a process");
		pthread_mutex_unlock(&vpg_mutex);
		return NULL;
	}

	vpg_backend_start_options_unlocked(data_dir, username, dbname, shared_preload_libraries);
	if (vpg_last_error != NULL)
	{
		pthread_mutex_unlock(&vpg_mutex);
		return NULL;
	}

	conn = calloc(1, sizeof(VPGConnection));
	if (conn == NULL)
	{
		vpg_replace_owned_string(&vpg_last_error, "out of memory allocating connection");
		pthread_mutex_unlock(&vpg_mutex);
		return NULL;
	}

	conn->open = true;
	conn->data_dir = strdup(data_dir);
	conn->username = strdup(username != NULL && username[0] != '\0' ? username : "postgres");
	conn->dbname = strdup(dbname != NULL && dbname[0] != '\0' ? dbname : conn->username);
	if (conn->data_dir == NULL || conn->username == NULL || conn->dbname == NULL)
	{
		free(conn->data_dir);
		free(conn->username);
		free(conn->dbname);
		free(conn);
		vpg_replace_owned_string(&vpg_last_error, "out of memory allocating connection");
		pthread_mutex_unlock(&vpg_mutex);
		return NULL;
	}

	vpg_connection_count++;
	pthread_mutex_unlock(&vpg_mutex);
	return conn;
}

const char *
vpg_conn_exec(void *handle, const char *query)
{
	const char *result;
	VPGConnection *conn = (VPGConnection *) handle;

	pthread_mutex_lock(&vpg_mutex);
	if (!vpg_valid_connection(conn))
	{
		vpg_replace_owned_string(&vpg_last_error, "connection is closed");
		pthread_mutex_unlock(&vpg_mutex);
		return NULL;
	}
	result = vpg_exec_unlocked(query);
	pthread_mutex_unlock(&vpg_mutex);
	return result;
}

int
vpg_conn_vacuum(void *handle)
{
	int rc;
	VPGConnection *conn = (VPGConnection *) handle;

	pthread_mutex_lock(&vpg_mutex);
	if (!vpg_valid_connection(conn))
	{
		vpg_replace_owned_string(&vpg_last_error, "connection is closed");
		pthread_mutex_unlock(&vpg_mutex);
		return -1;
	}
	rc = vpg_run_vacuum_unlocked(VACOPT_VACUUM |
								 VACOPT_PROCESS_MAIN |
								 VACOPT_PROCESS_TOAST);
	pthread_mutex_unlock(&vpg_mutex);
	return rc;
}

int
vpg_conn_analyze(void *handle)
{
	int rc;
	VPGConnection *conn = (VPGConnection *) handle;

	pthread_mutex_lock(&vpg_mutex);
	if (!vpg_valid_connection(conn))
	{
		vpg_replace_owned_string(&vpg_last_error, "connection is closed");
		pthread_mutex_unlock(&vpg_mutex);
		return -1;
	}
	rc = vpg_run_vacuum_unlocked(VACOPT_ANALYZE |
								 VACOPT_PROCESS_MAIN |
								 VACOPT_PROCESS_TOAST);
	pthread_mutex_unlock(&vpg_mutex);
	return rc;
}

int
vpg_conn_maintain(void *handle)
{
	int rc = 0;
	VPGConnection *conn = (VPGConnection *) handle;

	pthread_mutex_lock(&vpg_mutex);
	if (!vpg_valid_connection(conn))
	{
		vpg_replace_owned_string(&vpg_last_error, "connection is closed");
		pthread_mutex_unlock(&vpg_mutex);
		return -1;
	}
	if (vpg_run_vacuum_unlocked(VACOPT_VACUUM |
								VACOPT_ANALYZE |
								VACOPT_PROCESS_MAIN |
								VACOPT_PROCESS_TOAST) != 0)
	{
		pthread_mutex_unlock(&vpg_mutex);
		return -1;
	}

	PG_TRY();
	{
		StartTransactionCommand();
		RequestCheckpoint(CHECKPOINT_IMMEDIATE |
						  CHECKPOINT_FORCE |
						  CHECKPOINT_WAIT);
		CommitTransactionCommand();
	}
	PG_CATCH();
	{
		vpg_capture_current_error();
		AbortCurrentTransaction();
		rc = -1;
	}
	PG_END_TRY();

	pthread_mutex_unlock(&vpg_mutex);
	return rc;
}

void
vpg_conn_close(void *handle)
{
	VPGConnection *conn = (VPGConnection *) handle;

	if (conn == NULL)
		return;

	pthread_mutex_lock(&vpg_mutex);
	if (conn->open)
	{
		conn->open = false;
		if (vpg_connection_count > 0)
			vpg_connection_count--;
		if (vpg_connection_count == 0)
			vpg_finish_unlocked();
	}
	pthread_mutex_unlock(&vpg_mutex);

	free(conn->data_dir);
	free(conn->username);
	free(conn->dbname);
	free(conn);
}

void
vpg_finish(void)
{
	pthread_mutex_lock(&vpg_mutex);
	vpg_finish_unlocked();
	pthread_mutex_unlock(&vpg_mutex);
}

static void
vpg_finish_unlocked(void)
{
	if (!vpg_initialized)
		return;

	PG_TRY();
	{
		AbortOutOfAnyTransaction();
		shmem_exit(0);
	}
	PG_CATCH();
	{
		vpg_capture_current_error();
		FlushErrorState();
	}
	PG_END_TRY();

	vpg_initialized = false;
	vpg_replace_owned_string(&vpg_active_data_dir, NULL);
}
