Building a single-process PostgreSQL is a fascinating systems project — essentially collapsing Postgres's multi-process architecture into a single address space. Here are the core design problems you'd need to solve:

## Process Model → Thread or Async Model

Postgres forks a backend process per connection. In a single-process design you need to replace this with either:
- **Threads** (with careful locking around all the globals)
- **Coroutines/async** (cooperative scheduling, harder to retrofit)
- **Single-threaded event loop** (simplest, but no parallelism)

The challenge: Postgres has *enormous* amounts of global/static state (`CurrentMemoryContext`, `ActivePortal`, transaction state, etc.) that assumes per-process isolation. You'd need to audit and thread-localize all of it.

## Shared Memory

Postgres uses OS shared memory (`shmget`/`mmap`) for the buffer pool, lock tables, proc arrays, WAL buffers, etc. — because multiple processes need to share it. In a single process, you can replace all of this with **ordinary heap memory**, which is actually a simplification. The hard part is that the shared memory layout is deeply baked in via fixed structs like `PGPROC`, `LockMethodData`, and `BufferDescPadded`.

## Locking and Concurrency

Postgres uses two distinct locking systems:
- **Heavyweight locks** (lock manager, for relations/tuples) — semantics can stay the same
- **Lightweight locks / spinlocks** (`LWLock`, `SpinLock`) — currently use atomic ops and semaphores across processes

In a single-threaded design, many of these locks become **no-ops**, which is a big win. In a multi-threaded design, you'd replace them with pthread mutexes or C11 atomics, but you risk deadlocks that Postgres's deadlock detector wasn't designed to catch in-process.

## Signal Handling

Postgres uses Unix signals heavily for inter-process communication — `SIGTERM`, `SIGUSR1`, `SIGUSR2`, `SIGHUP` to notify backends of cancellation, config reload, recovery state changes, etc. In a single process you'd replace signal-based IPC with **direct function calls or condition variables**.

## Postmaster Supervision Logic

The postmaster monitors child processes, restarts them on crash, and manages startup/shutdown sequencing. In a single process, you need an equivalent **supervisor loop or watchdog** that handles task failure without taking down the whole process. This is particularly hard because Postgres uses `FATAL`/`ERROR` via `longjmp` (`elog`/`ereport`), which assumes process isolation as a crash boundary.

## Memory Management

Postgres's `MemoryContext` system is actually well-suited for single-process use. The big issue is **per-backend contexts** — each connection's query, transaction, and portal memory contexts need to be **fully isolated and cleaned up** when the logical "session" ends. Without process death as a GC mechanism, any leak becomes permanent.

## WAL and Crash Recovery

WAL writer, checkpointer, bgwriter, and walreceiver are all separate processes with their own main loops. You'd need to turn these into **background threads or cooperative tasks** called periodically from your main loop. The trickiest part is ensuring WAL flushing and checkpointing still provide the same durability guarantees without a separate process to enforce them.

## Extension and Plugin Safety

Postgres extensions (written in C) assume process isolation — a buggy extension can only crash one backend. In a single process, an extension crash takes everything down. You'd either need to **sandbox extensions** or simply accept this as a limitation (reasonable for embedded use cases).

