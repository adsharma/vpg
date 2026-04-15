# vPG Project Plan

## Goal
Create a single-process version of PostgreSQL by wrapping the existing PostgreSQL code using Vlang as the glue code. The aim is to interface with the existing C code (PostgreSQL 18.1) and provide a simple API to run SQL queries without needing a separate server process.

## Steps Completed

1. **Source Acquisition**
   - Cloned PostgreSQL repository and checked out the REL_18_1 tag (PostgreSQL 18.1).
   - Examined the source structure, focusing on the backend and main entry points.

2. **Understanding the Entry Points**
   - Identified `PostgresSingleUserMain` in `src/backend/tcop/postgres.c` as the entry point for single-user mode.
   - Reviewed the initialization sequence in `PostgresSingleUserMain` and `PostgresMain`.
   - Looked at the Server Programming Interface (SPI) as a potential way to execute queries.

3. **Building PostgreSQL**
   - Installed dependencies (icu4c via Homebrew).
   - Configured and built PostgreSQL 18.1 with `./configure --prefix=$(pwd)/installed` and `make -j4 install`.
   - The built libraries and headers are located in `vpg/postgresql/installed`.

4. **Setting Up the Data Directory**
   - Created a data directory in `vpg/data` using `initdb` from the built PostgreSQL.
   - Created a database and user for embedded use.

5. **Initial Vlang Wrapper**
   - Created a Vlang module (`vpg.v`) that declares external functions to be implemented in C.
   - Wrote a C shim (`vpg.c`) that initializes the PostgreSQL backend in standalone mode and provides a function to execute queries via SPI.
   - The C shim includes:
     - Initialization following the sequence from `PostgresSingleUserMain`.
     - A custom DestReceiver to capture query results (though we switched to using SPI directly for simplicity).
     - Error handling (simplified).
     - A `vpg_exec` function that uses SPI to execute a query and returns the result as a CSV string.
   - Created a simple Vlang API (`vpg.v`) with:
     - `new_pg_embedded` to initialize the embedded Postgres.
     - `query` method to run SQL and return results.
     - `close` method to clean up.

6. **Testing the C Shims**
   - Created a test C program (`test.c`) that calls the vpg_init, vpg_exec, and vpg_finish functions.
   - Added a local `Makefile` that links the shim against the built PostgreSQL backend object tree.
   - Verified that the test harness builds and can initialize the embedded backend, connect to the default `postgres` database, and execute a simple `SELECT` via SPI.

## What Has Been Done
- PostgreSQL 18.1 built and installed locally.
- Data directory initialized.
- Vlang module and C shim created with basic skeleton.
- Initial API designed.
- C shim rewritten to follow the real standalone backend startup path instead of placeholder initialization.
- Runtime error capture now uses `PG_TRY`/`PG_CATCH` with copied PostgreSQL error messages returned to the caller.
- Test harness now proves end-to-end query execution in-process:
  - `current_database,current_user`
  - `postgres,embed_user`

## What's Next

### Immediate Next Steps
1. **Refine the API**
   - Consider returning structured data (e.g., array of maps) instead of CSV strings for easier use in Vlang.
   - Decide how `vpg_finish` should cleanly tear down backend resources without terminating the host process.
   - Add support for parameterized queries.

2. **Tighten Build and Runtime Assumptions**
   - Reduce the current broad backend-object link list to the minimal supported server link set.
   - Decide whether the wrapper should always connect to `postgres` first or create/select an application database automatically.
   - Document the need for an unrestricted runtime when shared memory creation is sandboxed.

3. **Developer Ergonomics**
   - Fold the V test flow into `vpg/Makefile` or a small build script so the `make` and `v` steps are not manual.
   - Clean up the V naming/API surface (`new_pg_embedded`, `query`, `close`) and decide what should be public and stable.
   - Add a README example that matches the now-working C and V harnesses.

### Longer-Term Goals
- **Performance and Robustness**
  - Ensure the embedded Postgres can handle multiple queries and transactions.
  - Test with various SQL commands (DDL, DML, etc.).
  - Ensure that the embedded Postgres cleans up resources properly on close.

- **Feature Completeness**
  - Support for different authentication methods (though in embedded mode we trust the local user).
  - Ability to set configuration parameters.
  - Support for reading from and writing to files (e.g., COPY) if needed.

- **Documentation and Examples**
  - Write comprehensive README with usage examples.
  - Provide examples of common operations (creating tables, inserting data, querying).

- **Testing**
  - Write a test suite in Vlang to verify functionality.
  - Test edge cases and error conditions.

## Notes
- The project is inspired by PGlite but uses Vlang as the extension language instead of JavaScript/TypeScript.
- We are using the single-user mode initialization path but aiming to provide a non-interactive, programmatic interface.
- The use of SPI allows us to avoid dealing with the frontend/backend protocol directly.

## Open Questions
- How to handle multiple simultaneous queries? (Since we are in a single process, we can only have one active session at a time unless we use background worker-like mechanisms, but for simplicity we assume one query at a time.)
- How to manage memory contexts effectively to avoid leaks and ensure proper cleanup?
- What is the best way to capture errors from the PostgreSQL backend and surface them to Vlang?

## Conclusion
We have made initial progress by building PostgreSQL and creating a basic Vlang-C interface. The next steps are to complete the C shim, integrate it with Vlang, and test the API.
