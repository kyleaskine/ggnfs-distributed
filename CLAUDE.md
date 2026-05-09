# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A distributed coordinator for GGNFS lattice sieving (the special-q sieving phase of the General Number Field Sieve). Two binaries built from one Makefile:

- `ggnfs-sieve-server` — HTTP coordinator: chops a Q-range into workunits, hands them out under lease, receives relation files back, persists state in SQLite, serves a dashboard.
- `ggnfs-sieve-client` — polls the server, leases a workunit, fetches the `.job`, shells out to `gnfs-lasieve4*` via `system()`, posts the relations back.

Output is consumed by `finalize-nfs.sh`, which assembles `nfs.dat` and feeds YAFU's filter / LA / sqrt pipeline.

The source comments still reference "Phase 1 walking skeleton" / "Phase 2" / "Phase 3 verifier" — the verifier is not yet implemented (every submission is recorded with `verify_status='skipped'`).

## Build

```
make           # both binaries
make server    # just ggnfs-sieve-server
make client    # just ggnfs-sieve-client
make clean
```

There is no test suite, no linter config, and no CI. `dashboard_html.h` is generated from `dashboard.html` by `xxd -i` and gets regenerated automatically because `server.o` depends on it.

Vendored deps under `vendor/` (mongoose, cJSON, sqlite3 amalgamation) are checked in so a clean clone builds offline. **Never edit `vendor/*.c|*.h`.** To bump a version: `make update-mongoose` / `update-cjson` / `update-sqlite`, then update `vendor/VENDOR.md`. These targets are manual and not part of the default build by design.

Vendored sources are compiled with `-w` and a pile of `-DSQLITE_*` / `-DMG_*` flags in the Makefile (see `VENDOR_CFLAGS`). Don't drop those — `MG_MAX_RECV_SIZE=536870912` (512 MiB) is what allows large `/submit` request bodies, and the SQLite flags match how the code expects to use it (WAL, threadsafe, no extension loading).

## Running it end-to-end

```
# 1. Initialize a job (creates job.db, files/, rels/, token in --jobdir)
./ggnfs-sieve-server init \
    --job=input.job --siever=gnfs-lasieve4I14e \
    --qmin=80000000 --qmax=100000000 --qrange=10000 \
    --jobdir=/tmp/ggnfs-job

# 2. Serve
./ggnfs-sieve-server serve --jobdir=/tmp/ggnfs-job --port=8080

# 3. Add more workunits later without restarting state (qmin must be >= existing q_end)
./ggnfs-sieve-server extend --jobdir=/tmp/ggnfs-job --qmin=100000000 --qmax=120000000 --qrange=10000

# 4. On worker machines
./ggnfs-sieve-client \
    --server-url=http://host:8080 --token=<from jobdir/token> \
    --siever=/path/to/gnfs-lasieve4I14e \
    --workers=4 --cpu-pin=0,2,4,6

# 5. Assemble + factor
./finalize-nfs.sh --jobdir=/tmp/ggnfs-job --yafu-dir=/path/to/yafu --threads=8 --run
```

The dashboard is at `http://host:8080/?token=<token>` — the HTML itself is unauthenticated, but its JS uses the token to poll `/stats`.

## Architecture — the parts that span multiple files

### Threading model (server)
**Single SQLite connection, owned by the mongoose event-loop thread.** Every request handler and the periodic lease-expiry sweep timer (`on_sweep_timer` in `server.c`) run on that thread, so there's no locking on `ctx->db`. If anything ever runs DB work off-thread (e.g. a future verifier), it needs its own connection or a mutex — this invariant is the reason the code is lock-free, don't quietly break it.

### Workunit state machine (db.c, schema in `SCHEMA_SQL`)
`available → leased → submitted → verified|failed`, plus `available → leased → available` (requeue on lease timeout) and `→ poisoned` once `attempt_count` hits `--max-attempts`. The atomic `available → leased` transition is one `UPDATE … WHERE id = (SELECT … LIMIT 1) RETURNING …` in `db_lease`. The `available → submitted` transition with submission insert is wrapped in `BEGIN IMMEDIATE` in `db_submit` and returns `1` if the workunit isn't currently leased (handler returns 409). The lease-expiry sweep uses a single `UPDATE … RETURNING state` that decides poisoned-vs-available via a `CASE` on the post-increment attempt count.

### Auth
Bearer token, written by `init` to `<jobdir>/token` (chmod 600) and stored in the `meta` table. `serve` prefers the file on disk so rotating the token is "edit file + restart". `/health` and `/` (dashboard) are intentionally unauthenticated; everything else goes through `check_auth`.

### Files (content-addressed)
Input files (currently just the single `.job`) are stored under `<jobdir>/files/<sha>.job` and tracked in the `files` table by SHA-256. `/file/<sha>` looks up the absolute path in the DB. `init` calls `realpath()` because the server's cwd at `serve` time is unrelated to `init`'s cwd — `mg_http_serve_file` resolves relative paths against the server's cwd and would otherwise miss them.

### Workunit IDs
`wu-<jobhash>-<seq>` where `<jobhash>` is the first 8 hex chars of the `.job` SHA. `init` numbers from 0; `extend` continues the sequence using `db_workunit_extent` so IDs never collide.

### Client worker model
Each `--workers=N` spawns a pthread with its own `mg_mgr`, its own `<workdir>/wN`, its own `client_id` (`<base>-wN`). Workers share only `g_stop` (a `sig_atomic_t` that only goes 0→1). On Linux, `--cpu-pin=a,b,c,…` pins each worker (and the siever it `system()`s, which inherits affinity).

### Sieve executor (`sieve_executor.[ch]`)
One function: `sieve_run_local()` formats the `gnfs-lasieve4*` command line and invokes it via `system()`. It also `remove()`s any prior file at the output path because the siever opens its `-o` argument in append mode.

### Protocol layer (`protocol.[ch]`)
All JSON encode/decode lives here so `server.c` and `client.c` don't both link cJSON usage directly. Encoders return `malloc`'d strings the caller must `free()`. `proto_decode_lease_response` enforces required-field presence and returns -1 if any are missing — the client treats that as "malformed response, back off".

### Dashboard
`dashboard.html` is embedded at build time via `xxd -i -n dashboard_html` → `dashboard_html.h`. Editing the HTML is enough; Make picks up the dependency. The HTML reads its bearer token from `?token=…` in the URL.

## Things that have bitten people (load-bearing detail)

- `mfbr`/`mfba` and `lpbr`/`lpba` in `input.job` must match what you use for filtering later — `finalize-nfs.sh` aborts if `<yafu-dir>/nfs.job` SHA differs from the `.job` the server distributed, because mismatched factor base settings silently corrupt filtering.
- A submission counts relations by counting `\n` bytes in the body (a Phase 1 stand-in). The real verifier is supposed to land in Phase 3 and do parse-level counting.
- `OUTPUT_MAX_BYTES = 500 MiB`. The server's `MG_MAX_RECV_SIZE` is set to allow that — they need to stay in sync if either is bumped.
- The server has no graceful shutdown today (`for (;;) mg_mgr_poll(...)`); the cleanup code below the loop is unreachable. SIGINT just kills it. The DB is in WAL mode so this is fine.
