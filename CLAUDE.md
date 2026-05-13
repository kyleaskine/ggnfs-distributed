# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A distributed coordinator for GGNFS lattice sieving (the special-q sieving phase of the General Number Field Sieve). Two binaries built from one Makefile:

- `ggnfs-sieve-server` — HTTP coordinator: chops a Q-range into workunits, hands them out under lease, receives relation files back, persists state in SQLite, serves a dashboard.
- `ggnfs-sieve-client` — polls the server, leases a workunit, fetches the `.job`, shells out to `gnfs-lasieve4*` via `system()`, posts the relations back.

Output is consumed by `finalize-nfs.sh`, which assembles `nfs.dat` and feeds YAFU's filter / LA / sqrt pipeline.

The verifier (parse pass + q-range check + GMP norm spot-check) runs in a background pthread on the server. New submissions land with `verify_status='pending'` and the verifier transitions them to `passed` or `failed`; workunits go to `verified` on pass and back to `available` (or `poisoned`) on fail. See `FUTURE.md` for ideas not yet built.

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
    --siever-args="-J 16" \
    --jobdir=/tmp/ggnfs-job

# 2. Serve
./ggnfs-sieve-server serve --jobdir=/tmp/ggnfs-job --bind=127.0.0.1 --port=8080

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

The dashboard is at `http://host:8080/?token=<token>` — the HTML itself is unauthenticated, but its JS uses the token to poll `/stats`. `serve` defaults to `--bind=127.0.0.1`; use `--bind=0.0.0.0` only when the coordinator should be reachable from other machines.

## Architecture — the parts that span multiple files

### Threading model (server)
**Two SQLite connections, one per thread, no shared mutex.** The mongoose event-loop thread owns `ctx->db` — every request handler and the periodic lease-expiry sweep timer (`on_sweep_timer` in `server.c`) run on it. The verifier pthread owns its own connection (opened inside `verify_thread_run`). WAL mode + `sqlite3_busy_timeout=5s` (set in `db_open`) keep cross-thread contention to brief stalls rather than `SQLITE_BUSY` failures; inside each thread, no locking is needed. If you add another DB-touching thread, follow the same pattern (own connection, no shared `ggnfs_db_t`).

### Workunit state machine (db.c, schema in `SCHEMA_SQL`)
Happy path: `available → leased → submitted → verified`. Failure loops back: the verifier (or the lease-expiry sweep) sends the workunit to `available` with `attempt_count++`, or to `poisoned` once `attempt_count` hits `--max-attempts`. The atomic `available → leased` transition is one `UPDATE … WHERE id = (SELECT … LIMIT 1) RETURNING …` in `db_lease`. `leased → submitted` (with the submission insert at `verify_status='pending'`) is `BEGIN IMMEDIATE` in `db_submit` and returns `1` if the workunit isn't currently leased (handler responds 409). The verifier's resolve path (`db_verify_pass` / `db_verify_fail`) and the lease-expiry sweep both wrap their transitions in `BEGIN IMMEDIATE` too; the sweep uses a single `UPDATE … RETURNING state` with a `CASE` on the post-increment count to pick `available` vs `poisoned`.

### Verifier (`verify.[ch]`)
Background pthread. Started by `cmd_serve` after `db_open`, signaled by `verify_thread_wake` on every successful `/submit`, stopped via `verify_thread_stop` (currently unreachable — see `FUTURE.md`). Owns its own `ggnfs_db_t`. Each iteration drains every pending submission, then waits on a condvar with a 5s timed-wait as a safety net.

Per submission: one streaming pass over the relation file does (1) parse check on every line (`a` signed-decimal, `b` unsigned-decimal nonzero, two comma-separated hex prime lists; `b=0` is rejected since it's msieve's free-relation shape, not raw lasieve4 output), (2) q-range check (at least one prime on the sieved side falls in `[q_start, q_start+q_range)`), and (3) Algorithm-R reservoir-sampling of K accepted relations. The norm spot-check then runs on the reservoir via GMP: `|N_R| = |a*Y1 + b*Y0|`, `|N_A| = |Σ c_k a^k b^(d-k)|`, abs, divide out listed primes (all multiplicities), trial-divide by primes ≤ 1000, residue must be 1 or probable-prime. Convention matches `msieve/gnfs/relation.c:nfs_read_relation`.

Polynomial coefficients are parsed from the `.job` at `init` time and stored in `meta` (`poly_degree`, `poly_c0..c<d>`, `poly_Y0`, `poly_Y1`); the verifier loads them once at thread start into `verify_poly_gmp_t`. If meta is missing (a jobdir initialized before this code), spot-check is silently disabled and only parse + q-range run. K is set by `--spotcheck-k=N` on `serve`, default 50; K=0 disables.

### Auth
Bearer token, written by `init` to `<jobdir>/token` (chmod 600) and stored in the `meta` table. `serve` prefers the file on disk so rotating the token is "edit file + restart". `/health` and `/` (dashboard) are intentionally unauthenticated; everything else goes through `check_auth`. The server binds to loopback by default; exposing it on a LAN requires an explicit `--bind=0.0.0.0` or specific interface address.

### Files (content-addressed)
Input files (currently just the single `.job`) are stored under `<jobdir>/files/<sha>.job` and tracked in the `files` table by SHA-256. `/file/<sha>` looks up the absolute path in the DB. `init` calls `realpath()` because the server's cwd at `serve` time is unrelated to `init`'s cwd — `mg_http_serve_file` resolves relative paths against the server's cwd and would otherwise miss them.

### Workunit IDs
`wu-<jobhash>-<seq>` where `<jobhash>` is the first 8 hex chars of the `.job` SHA. `init` numbers from 0; `extend` continues the sequence using `db_workunit_extent` so IDs never collide.

### Client worker model
Each `--workers=N` spawns a pthread with its own `mg_mgr`, its own `<workdir>/wN`, its own `client_id` (`<base>-wN`). Workers share shutdown phase (`running → draining → cancelling`) plus a mutex-protected active-lease table. First Ctrl-C enters draining mode: finish active work, submit it, and stop requesting new leases. Second Ctrl-C enters cancelling mode: the main thread POSTs `/release` for active leases and exits. On Linux, `--cpu-pin=a,b,c,…` pins each worker (and the siever it `system()`s, which inherits affinity).

### Sieve executor (`sieve_executor.[ch]`)
One function: `sieve_run_local()` formats the `gnfs-lasieve4*` command line and invokes it via `system()`. It also `remove()`s any prior file at the output path because the siever opens its `-o` argument in append mode.

### Protocol layer (`protocol.[ch]`)
All JSON encode/decode lives here so `server.c` and `client.c` don't both link cJSON usage directly. Encoders return `malloc`'d strings the caller must `free()`. `proto_decode_lease_response` enforces required-field presence and returns -1 if any are missing — the client treats that as "malformed response, back off". `POST /release` is a voluntary lease return used by client cancellation; it only succeeds for a workunit currently leased to that client and does not increment `attempt_count`.

### Dashboard
`dashboard.html` is embedded at build time via `xxd -i -n dashboard_html` → `dashboard_html.h`. Editing the HTML is enough; Make picks up the dependency. The HTML reads its bearer token from `?token=…` in the URL.

### Siever flags
`--siever-args="..."` on `init` is stored in `meta.siever_args`, sent to every client in the `/lease` response, and appended verbatim to the siever command in `sieve_run_local`. Used for tunables every worker should share — e.g. `-J 16` for a larger I-sieve area. To change it on an existing jobdir without reinitializing, edit meta directly: `sqlite3 jobdir/job.db "UPDATE meta SET value='-J 16' WHERE key='siever_args'"` and restart `serve`.

## Things that have bitten people (load-bearing detail)

- `mfbr`/`mfba` and `lpbr`/`lpba` in `input.job` must match what you use for filtering later — `finalize-nfs.sh` aborts if `<yafu-dir>/nfs.job` SHA differs from the `.job` the server distributed, because mismatched factor base settings silently corrupt filtering.
- `/submit` counts `\n` bytes in the body as a fast initial estimate (the JSON response carries that count). The verifier replaces it with the actual parsed line count when the submission passes, so stats / dashboard reflect real counts only after verification.
- `OUTPUT_MAX_BYTES = 500 MiB`. The server's `MG_MAX_RECV_SIZE` is set to allow that — they need to stay in sync if either is bumped.
- `sqlite3_busy_timeout=5s` in `db_open` is what lets the verifier and event-loop threads share a DB file without explicit locking. Dropping it would surface `SQLITE_BUSY` on the main thread under submit load.
- The server has no graceful shutdown today (`for (;;) mg_mgr_poll(...)`); the cleanup code below the loop — including `verify_thread_stop` — is unreachable. SIGINT just kills it. The DB is in WAL mode so this is fine. (Tracked in `FUTURE.md`.)
