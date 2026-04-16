# vPG – Single-Process Embedded PostgreSQL Plan

## Goal

A SQLite3-like embedded database engine: one C library + a thin Vlang API.
No server process, no network, no fork/exec, no signals, no pipes.
Single connection (like SQLite in WAL mode with one writer).

The inspiration is [PGlite](https://pglite.dev) — PostgreSQL compiled to WASM and
stripped of its process model — but done natively with a C + Vlang approach.

---

## What the current code does wrong

The current `vpg_initdb.c` still forks children and uses pipes to communicate
with them (via `vpg_popen_w`).  It calls `BootstrapModeMain` and
`PostgresSingleUserMain` which are `__attribute__((noreturn))` — they always
end with `proc_exit()`.  Replacing those with `setjmp`/`longjmp` is fragile
and hard to reason about.

The right fix is to **not call those entry points at all**.  They are
convenience wrappers.  The real work is done by the internal functions they
call, and those functions return normally.

---

## Architecture

```
┌────────────────────────────────────┐
│  Vlang API  (vpg.v)                │  ← user-facing: initdb / open / exec / close
└───────────────┬────────────────────┘
                │ C FFI
┌───────────────▼────────────────────┐
│  C control layer  (vpg.c)          │  ← thin glue; no process management
│  vpg_initdb()                      │
│  vpg_init()                        │
│  vpg_exec()                        │
│  vpg_finish()                      │
└───────────────┬────────────────────┘
                │ direct C calls
┌───────────────▼────────────────────┐
│  PostgreSQL internals (libvpg.a)   │  ← storage, catalog, SPI, parser, planner,
│                                    │    executor, WAL, GUC, memory contexts
└────────────────────────────────────┘
```

---

## What stays as PostgreSQL C (untouched)

Everything below the "entry point" layer is a clean library:

| Subsystem | Key files |
|-----------|-----------|
| Memory contexts | `utils/mmgr/` |
| GUC (config) | `utils/misc/guc*.c` |
| Storage / buffer manager | `storage/` |
| Catalog | `catalog/` |
| Parser | `parser/` |
| Planner | `optimizer/` |
| Executor | `executor/` |
| SPI | `executor/spi.c` |
| WAL | `access/transam/xlog.c` |
| Transactions | `access/transam/xact.c` |
| Bootstrap parser | `bootstrap/bootparse.y`, `bootstrap/bootscanner.l` |

We compile all of this into `libvpg.a` exactly as we do today.

---

## What gets rewritten (the process-model layer)

PostgreSQL's process-model scaffolding lives in three entry points:

| Entry point | What it does | Our replacement |
|-------------|-------------|-----------------|
| `BootstrapModeMain()` | signals, option parsing, calls bootstrap internals, `proc_exit(0)` | `vpg_bootstrap()` in `vpg.c` |
| `PostgresSingleUserMain()` | signals, option parsing, calls `PostgresMain()`, `proc_exit()` | `vpg_open()` in `vpg.c` |
| `PostgresMain()` | signal handlers, `sigjmp_buf` REPL loop, `proc_exit(0)` | `vpg_exec()` + `vpg_finish()` in `vpg.c` |

We copy the *sequence of internal calls* from each entry point and drop:
- All `pqsignal()` / `sigprocmask()` calls
- The `sigjmp_buf` error-recovery REPL loop
- All `proc_exit()` / `on_proc_exit()` calls
- `find_my_exec()` / `find_other_exec()` (no external binary)

Errors are caught at the C/V boundary with `PG_TRY` / `PG_CATCH` and returned
as strings.  There is no global error-recovery loop.

---

## Phase 1 — `vpg_initdb()`: one-time data directory setup

Replaces `vpg_initdb.c` (currently ~3600 lines with fork/pipe machinery).

### Step 1a: directory + config setup

These functions from `initdb.c` are pure file I/O and need no modification:

```c
setup_pgdata();           // set pg_data, PGDATA env var
create_data_directory();  // mkdir
create_xlog_or_symlink(); // pg_wal subdir
write_version_file(NULL); // PG_VERSION
set_null_conf();          // empty postgresql.conf
setup_config();           // write postgresql.conf, pg_hba.conf, pg_ident.conf
```

We call `get_share_path()` with the current executable path to locate the
BKI file and SQL scripts.  No `find_other_exec()` needed.

### Step 1b: `vpg_run_bootstrap()` — replaces `BootstrapModeMain`

```c
// Sequence copied from BootstrapModeMain, proc_exit and signals removed:
MemoryContextInit();
InitializeGUCOptions();
// set data_dir, encoding, checksums via SetConfigOption()
SelectConfigFiles(data_dir, "vpg");
checkDataDir();
ChangeToDataDir();
CreateDataDirLockFile(false);
SetProcessingMode(BootstrapProcessing);
IgnoreSystemIndexes = true;
InitializeMaxBackends();
InitPostmasterChildSlots();
InitializeFastPathLocks();
CreateSharedMemoryAndSemaphores();
set_max_safe_fds();
InitProcess();
BaseInit();
// NO bootstrap_signals()
BootStrapXLOG(checksum_version);
InitPostgres(NULL, InvalidOid, NULL, InvalidOid, 0, NULL);

// Feed BKI file content directly — no stdin, no pipe, no fork
FILE *bki = fopen(bki_file_path, "r");
yyscan_t scanner;
boot_yylex_init(&scanner);
boot_yyset_in(bki, scanner);         // point scanner at the file
StartTransactionCommand();
boot_yyparse(scanner);               // runs all bootstrap commands
CommitTransactionCommand();
fclose(bki);
RelationMapFinishBootstrap();
// NO proc_exit — just return
```

`boot_yyset_in()` is the flex API to redirect the scanner's input source.
No stdin redirection, no pipe, no fork.

### Step 1c: `vpg_run_setup_sql()` — replaces popen("--single") calls

After bootstrap, the database is in `BootstrapProcessing` mode.  We switch
to `NormalProcessing` and run the setup SQL files directly via SPI:

```c
SetProcessingMode(NormalProcessing);
// Re-use the already-initialised shared memory and process state.
// No re-init needed — we are still the same process.

// Run each setup SQL script:
vpg_exec_file(system_constraints_file);
vpg_exec_file(system_functions_file);
vpg_exec_file(system_views_file);
vpg_exec_file(info_schema_file);
vpg_exec_file(dictionary_file);
// Inline SQL (auth, privileges, collations, etc.) via SPI_execute():
vpg_exec_sql("UPDATE pg_authid SET rolpassword = NULL ...");
// etc.
```

```c
static void
vpg_exec_file(const char *path)
{
    // read file into string, call SPI_execute in a transaction
    char *sql = read_file(path);
    SPI_connect();
    StartTransactionCommand();
    SPI_execute(sql, false, 0);
    CommitTransactionCommand();
    SPI_finish();
    pfree(sql);
}
```

No `PostgresSingleUserMain`, no fork, no pipe.

### Step 1d: teardown

```c
// Flush WAL, sync, release shared memory
RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_FORCE | CHECKPOINT_WAIT);
ShutdownXLOG(0, 0);
// Release process slot
ProcKill(0, 0);
// DO NOT call proc_exit — just return to the caller
```

---

## Phase 2 — `vpg_init()`: open an existing data directory

Sequence copied from `PostgresSingleUserMain` + `PostgresMain`, with signals
and the REPL loop removed:

```c
void vpg_init(const char *data_dir, const char *user, const char *db)
{
    MemoryContextInit();
    InitializeGUCOptions();
    SelectConfigFiles(data_dir, "vpg");
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
    BaseInit();
    // NO signal handlers
    // NO sigjmp_buf loop
    InitPostgres(db, InvalidOid, user, InvalidOid, 0, NULL);
    SetProcessingMode(NormalProcessing);
    BeginReportingGUCOptions();
    // ready to execute queries
}
```

---

## Phase 3 — `vpg_exec()`: run a query

No REPL loop.  One query, one PG_TRY block, result returned as string.

```c
const char *vpg_exec(const char *sql)
{
    PG_TRY();
    {
        StartTransactionCommand();
        SPI_connect();
        SPI_execute(sql, false, 0);
        // format result into a malloc'd CSV/JSON string
        result = format_spi_result();
        SPI_finish();
        CommitTransactionCommand();
    }
    PG_CATCH();
    {
        SPI_finish();
        AbortCurrentTransaction();
        vpg_last_error = CopyErrorData()->message;
        FlushErrorState();
        result = NULL;
    }
    PG_END_TRY();
    return result;
}
```

---

## Phase 4 — `vpg_finish()`: clean shutdown

```c
void vpg_finish(void)
{
    RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_FORCE | CHECKPOINT_WAIT);
    ShutdownXLOG(0, 0);
    ProcKill(0, 0);
    // shared memory is process-private anyway; just let it go
}
```

---

## Vlang API (vpg.v)

No changes to the public API; just the internals become correct:

```v
pub fn initdb(data_dir string, user string) !      // one-time setup
pub fn new_pg_embedded(data_dir string, user string, db string) !PGEmbedded
pub fn (mut pg PGEmbedded) query(sql string) !string
pub fn (mut pg PGEmbedded) close()
```

---

## Files to change

| File | Action |
|------|--------|
| `vpg_initdb.c` | **Delete** (replace with new `vpg_initdb.c` ~300 lines, no fork/pipe) |
| `vpg_findtimezone.c` | Keep (pure timezone detection, no process model) |
| `vpg_localtime.c` | Keep (timezone tables) |
| `vpg_fe_shim.c` | Keep (fe/be shim, needed for initdb SQL helpers) |
| `vpg.c` | Rewrite `vpg_initdb()`, `vpg_init()`, `vpg_exec()`, `vpg_finish()` |
| `vpg.v` | Unchanged |
| `Makefile` | Remove objects that are no longer needed |

---

## Hard constraints — never violate these

- **No `fork()`** — absolutely forbidden anywhere in the codebase.
  `fork()` is incompatible with single-process embedded use: the child
  inherits shared-memory state (LatchWaitSet, PGPROC, shmem segments)
  that it cannot reinitialise safely, producing crashes or corruption.
- **No `popen()` / `pclose()`** — they fork internally.
- **No `exec()`** — no external postgres binary.
- **No signal-based query cancellation** — `pqsignal()` calls are removed
  everywhere; errors are caught with `PG_TRY`/`PG_CATCH` instead.
- **No `proc_exit()`** — control must always return to the caller.

Any code that calls `fork`, `popen`, `pclose`, `exec`, `system`, or
`proc_exit` is a bug and must be replaced.

---

## What we do NOT support (intentional)

- Multiple concurrent connections (single connection like SQLite)
- Postmaster / network listener
- Signal-based query cancellation
- External authentication (trust only)
- Extensions that fork worker processes
- Parallel query (requires worker processes)

These constraints may be relaxed later without breaking the API, because the
design does not bake in any assumptions that would prevent it — it just does
not implement it yet.

---

## Build

The build process stays the same:

1. Compile PostgreSQL backend objects into `libvpg.a` (already done).
2. Compile `vpg.c`, `vpg_initdb.c`, `vpg_findtimezone.c`, `vpg_localtime.c`,
   `vpg_fe_shim.c` and link against `libvpg.a`.
3. `v run cmd/vpg_test.v` compiles and runs the V test.
