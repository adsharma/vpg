# vPG – Single‑Process Postgres Wrapper (Vlang)

A thin Vlang layer that boots PostgreSQL 18.1 in standalone (single‑user) mode and exposes a simple API to run arbitrary SQL queries and collect data from p

## Overview
- Uses the PostgreSQL source as a git submodule at `./postgresql` (REL_18_1).
- Calls the initialization sequence from `PostgresSingleUserMain` but replaces the interactive stdin/stdout with in‑memory buffers.
- Executes queries via SPI (`SPI_execute`) and returns results as UTF‑8 strings.
- No external postmaster or separate processes – everything runs in the same OS process.

## Build
1. Install V (>=0.4) and a C compiler.
2. Initialize submodules: `git submodule update --init --recursive`.
3. Build PostgreSQL in the submodule (for example `./configure --prefix=$(pwd)/installed && make -j4 install` inside `./postgresql`).
4. Ensure a PostgreSQL data directory exists at `./data` (created by `./postgresql/installed/bin/initdb`).

## Usage
```v
mut pg := vpg.NewPGEmbedded{
	data_dir: './data',
	user:     'embed_user',
	db:       'embed_db',
} or { err => eprintln(err) }

defer pg.Close()

result := pg.Query('SELECT version();') or { err => eprintln(err) }
println(result) // e.g. [{version: 'PostgreSQL 18.1 on ...'}]
```

## Implementation notes
- The wrapper follows the exact startup order from `PostgresSingleUserMain`:
  `InitStandaloneProcess → InitializeGUCOptions → process_postgres_switches → SelectConfigFiles → checkDataDir → ChangeToDataDir → CreateDataDirLockFile → LocalProcessControlFile → process_shared_preload_libraries → InitializeMaxBackends → InitPostmasterChildSlots → InitializeFastPathLocks → process_shmem_requests → InitializeShmemGUCs → InitializeWalConsistencyChecking → CreateSharedMemoryAndSemaphores → set_max_safe_fds → InitProcess → PostgresMain`.
- `whereToSendOutput` is set to `DestNone`; query results are captured via a custom `DestReceiver` that appends tuples to a string buffer.
- Error handling propagates `elog`/`ereport` as V errors via `errno` and `PG_TRY`/`PG_CATCH` blocks wrapped in C functions.
- All memory is allocated in PostgreSQL memory contexts; V only owns the final UTF‑8 strings.

## Safety
- Runs with the privileges of the calling process; ensure the data directory is owned by a non‑root user.
- Signal handlers are installed as in the original backend (SIGINT, SIGTERM, SIGQUIT → die).
- No network listeners are opened; the backend stays in standalone mode.
