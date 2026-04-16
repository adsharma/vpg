/*
 * vpg_fe_shim.c
 *
 * Single-process embedded PostgreSQL has no frontend/backend distinction.
 * This file provides the handful of "frontend-style" helper functions that
 * vpg_initdb.c (a modified copy of initdb.c) calls, implemented as thin
 * wrappers over their backend equivalents or simple no-ops.
 *
 * Compiled with the same backend flags as everything else.
 */

#include "postgres.h"

#include "common/file_utils.h"
#include "common/logging.h"
#include "libpq/pqsignal.h"
#include "port.h"

#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

/* ----------------------------------------------------------------
 * Memory — pg_malloc/pg_free use plain malloc/free.
 * initdb allocates and frees with these as matched pairs; mixing
 * palloc (MemoryContext-based) with free() would corrupt memory.
 * ---------------------------------------------------------------- */

void *
pg_malloc(Size size)
{
	void *p = malloc(size > 0 ? size : 1);
	if (!p) { fprintf(stderr, "out of memory\n"); abort(); }
	return p;
}

void *
pg_malloc0(Size size)
{
	void *p = calloc(1, size > 0 ? size : 1);
	if (!p) { fprintf(stderr, "out of memory\n"); abort(); }
	return p;
}

void *
pg_malloc_extended(Size size, int flags)
{
	void *p = malloc(size > 0 ? size : 1);
	if (!p) { fprintf(stderr, "out of memory\n"); abort(); }
	return p;
}

void *
pg_realloc(void *ptr, Size size)
{
	void *p = realloc(ptr, size > 0 ? size : 1);
	if (!p) { fprintf(stderr, "out of memory\n"); abort(); }
	return p;
}

char *
pg_strdup(const char *str)
{
	char *p = strdup(str);
	if (!p) { fprintf(stderr, "out of memory\n"); abort(); }
	return p;
}

char *
pg_strdup_noerr(const char *str)
{
	return strdup(str);
}

void
pg_free(void *ptr)
{
	free(ptr);
}

/* ----------------------------------------------------------------
 * Logging — write to stderr; pg_log_generic is the single call-site.
 * ---------------------------------------------------------------- */

void
pg_logging_init(const char *progname)
{
	/* no-op: backend elog handles error reporting */
}

void
pg_logging_set_level(enum pg_log_level new_level)
{
	/* no-op */
}

void
pg_logging_increase_verbosity(void)
{
	/* no-op */
}

void
pg_logging_set_pre_callback(void (*cb) (void))
{
	/* no-op */
}

void
pg_logging_set_locus_callback(void (*cb) (const char **filename, uint64 *lineno))
{
	/* no-op */
}

void
pg_log_generic(enum pg_log_level level, enum pg_log_part part,
			   const char *pg_restrict fmt, ...)
{
	va_list		ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

void
pg_log_generic_v(enum pg_log_level level, enum pg_log_part part,
				 const char *pg_restrict fmt, va_list ap)
{
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	fflush(stderr);
}

/* ----------------------------------------------------------------
 * Signals
 * ---------------------------------------------------------------- */

/* pqsignal_fe = pqsignal in FRONTEND builds; pqsignal returns void */
void
pqsignal_fe(int signo, pqsigfunc func)
{
	pqsignal_be(signo, func);
}

/* ----------------------------------------------------------------
 * Auth prompt — not needed in embedded/trust mode.
 * ---------------------------------------------------------------- */

char *
simple_prompt(const char *prompt, bool echo)
{
	return pstrdup("");
}

char *
simple_prompt_extended(const char *prompt, bool echo, bool *canceled)
{
	if (canceled)
		*canceled = false;
	return pstrdup("");
}

/* ----------------------------------------------------------------
 * Restricted token — Unix no-op.
 * ---------------------------------------------------------------- */

void
get_restricted_token(void)
{
	/* no-op on Unix */
}

/* ----------------------------------------------------------------
 * sync_pgdata — fsync the freshly-created data directory.
 * We delegate to the backend's fsync_fname / durable_rename helpers
 * which are already compiled into libvpg.a.
 * ---------------------------------------------------------------- */

/*
 * Simplified sync: just call sync() and let the OS handle it.
 * A full implementation would walk the directory tree, but for an
 * embedded use-case (typically on local storage) this is fine.
 */
int
sync_pgdata(const char *pg_data, int serverVersion, DataDirSyncMethod method,
			bool sync_data_files)
{
	sync();
	return 0;
}

int
sync_dir_recurse(const char *dir, DataDirSyncMethod method)
{
	sync();
	return 0;
}

/* ----------------------------------------------------------------
 * appendShellString / appendShellStringNoError
 * Copied from fe_utils/string_utils.c; that object conflicts with
 * ruleutils.o over quote_all_identifiers so we inline it here.
 * ---------------------------------------------------------------- */
#include "pqexpbuffer.h"

bool
appendShellStringNoError(PQExpBuffer buf, const char *str)
{
	bool		ok = true;
	const char *p;

	if (*str != '\0' &&
		strspn(str, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_./:") == strlen(str))
	{
		appendPQExpBufferStr(buf, str);
		return ok;
	}

	appendPQExpBufferChar(buf, '\'');
	for (p = str; *p; p++)
	{
		if (*p == '\n' || *p == '\r')
		{
			ok = false;
			continue;
		}
		if (*p == '\'')
			appendPQExpBufferStr(buf, "'\"'\"'");
		else
			appendPQExpBufferChar(buf, *p);
	}
	appendPQExpBufferChar(buf, '\'');
	return ok;
}

void
appendShellString(PQExpBuffer buf, const char *str)
{
	if (!appendShellStringNoError(buf, str))
	{
		fprintf(stderr, "shell command argument contains a newline or carriage return: \"%s\"\n", str);
		exit(EXIT_FAILURE);
	}
}
