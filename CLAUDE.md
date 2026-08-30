# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A distributed coordinator for GGNFS lattice sieving (the special-q sieving phase of the General Number Field Sieve). Two binaries built from one Makefile:

- `ggnfs-sieve-server` — HTTP coordinator: chops a Q-range into workunits, hands them out under lease, receives relation files back, persists state in SQLite, serves a dashboard.
- `ggnfs-sieve-client` — polls the server, leases a workunit, fetches the `.job`, runs a siever in an isolated child process group, posts the relations back. Two engines: `--engine=lasieve4` (default, `gnfs-lasieve4*` on a CPU core) and `--engine=cuda` (`../cuda-sieve`'s `bench` on an NVIDIA GPU). See `GPU-CLIENT.md`.

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

# 3. Add more workunits later without restarting state. The new [qmin, qmax) can
#    sit above, below, or in a gap between existing workunits — any non-overlapping
#    placement is accepted.
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

## Screening a worker with `benchmark`

Rented boxes (VastAI etc.) that look identical can differ 50%+ in real sieving speed because of hypervisor CPU steal or a noisy neighbor saturating memory bandwidth. `ggnfs-sieve-client benchmark` screens a box in a couple of minutes instead of hours of sieving:

```
./ggnfs-sieve-client benchmark \
    --server-url=http://host:8080 --token=<token> \
    --siever=/path/to/gnfs-lasieve4I16e \
    [--workers=N] [--qrange=65] [--q=N] [--min-rels-per-sec=X]
```

It reads job params from **`GET /stats`** (`job_sha256`, `siever_args`, `side`, `q_min`) and fetches the `.job` via `GET /file/<sha>` — **it never calls `/lease` or `/submit`, so it is safe against a live coordinator and takes no work out of the pool.** Then it builds the factor-base cache once, sieves a fixed q-range single-core (Phase 1), then has every worker (default: all online CPUs) sieve the *same* q-range at once (Phase 2), and reports single-core rel/s, aggregate rel/s, **multicore scaling %**, slowest-worker time, **CPU steal %**, and load average. `--min-rels-per-sec=X` sets exit code 3 when aggregate throughput is below X, for scripted auto-reject.

The measurement is **fixed work, not fixed time**: it sieves a fixed q-interval width (`--qrange`, the siever's `-c`) from a fixed anchor (`q_min`, or `--q`), so every special-q runs to completion and the relation count comes out identical on every box — time is then a pure hardware signal. Steal >~5% or a scaling collapse are the fingerprints of an oversubscribed / memory-starved VM; rel/s only means something relative to a known-good box, so run it once on a box you trust to set the yardstick. `ggnfs-client.sh` writes a `benchmark.sh` wrapper and offers to run it at bootstrap.

**Requires a server built from this revision** (the `/stats` fields were added here); against an older `serve` it prints "server predates benchmark support" and exits. Just rebuild + restart `serve` — no `re-init` needed, since `job_sha256`/`siever_args` are already in `meta`.

## Architecture — the parts that span multiple files

### Threading model (server)
**Two SQLite connections, one per thread, no shared mutex.** The mongoose event-loop thread owns `ctx->db` — every request handler and the periodic lease-expiry sweep timer (`on_sweep_timer` in `server.c`) run on it. The verifier pthread owns its own connection (opened inside `verify_thread_run`). WAL mode + `sqlite3_busy_timeout=5s` (set in `db_open`) keep cross-thread contention to brief stalls rather than `SQLITE_BUSY` failures; inside each thread, no locking is needed. If you add another DB-touching thread, follow the same pattern (own connection, no shared `ggnfs_db_t`).

### Workunit state machine (db.c, schema in `SCHEMA_SQL`)
Happy path: `available → leased → submitted → verified`. Failure loops back: the verifier (or the lease-expiry sweep) sends the workunit to `available` with `attempt_count++`, or to `poisoned` once `attempt_count` hits `--max-attempts`. The atomic `available → leased` transition is one `UPDATE … WHERE id = (SELECT … LIMIT 1) RETURNING …` in `db_lease`. `leased → submitted` (with the submission insert at `verify_status='pending'`) is `BEGIN IMMEDIATE` in `db_submit` and returns `1` if the workunit isn't currently leased (handler responds 409). The verifier's resolve path (`db_verify_pass` / `db_verify_fail`) and the lease-expiry sweep both wrap their transitions in `BEGIN IMMEDIATE` too; the sweep uses a single `UPDATE … RETURNING state` with a `CASE` on the post-increment count to pick `available` vs `poisoned`.

### Verifier (`verify.[ch]`)
Background pthread. Started by `cmd_serve` after `db_open`, signaled by `verify_thread_wake` on every successful `/submit`, stopped via `verify_thread_stop` (currently unreachable — see `FUTURE.md`). Owns its own `ggnfs_db_t`. Each iteration drains every pending submission, then waits on a condvar with a 5s timed-wait as a safety net.

Per submission: one streaming pass over the relation file does (1) parse check on every line (`a` signed-decimal, `b` unsigned-decimal nonzero, two comma-separated hex prime lists; `b=0` is rejected since it's msieve's free-relation shape, not raw lasieve4 output), (2) q-range check (at least one prime on the sieved side falls in `[q_start, q_start+q_range)`), and (3) Algorithm-R reservoir-sampling of K accepted relations. The norm spot-check then runs on the reservoir via GMP: `|N_R| = |a*Y1 + b*Y0|`, `|N_A| = |Σ c_k a^k b^(d-k)|`, abs, divide out listed primes (all multiplicities), trial-divide by primes ≤ 1000, residue must be 1 or probable-prime. Convention matches `msieve/gnfs/relation.c:nfs_read_relation`.

Polynomial coefficients are parsed from the `.job` at `init` time and stored in `meta` (`poly_degree`, `poly_c0..c<d>`, `poly_Y0`, `poly_Y1`); the verifier loads them once at thread start into `verify_poly_gmp_t`. Degree is the highest `c<k>` line present and omitted lower coefficients are stored as 0, so the sparse form SNFS polys are usually written in (`c6`/`c3`/`c0` and nothing else) is accepted the same as a fully spelled-out one. Because presence is then no longer evidence of correctness, `init` validates the values instead: every coefficient must be a decimal integer, a zero leading coefficient is trimmed rather than inflating the degree, and — when the `.job` has an `n:` line — `sum c_k*(-Y0)^k*Y1^(d-k) ≡ 0 (mod n)` must hold, which is what catches a coefficient line that was dropped or mangled rather than deliberately omitted. A `.job` with no `n:` skips only that last check. If meta is missing (a jobdir initialized before this code), spot-check is silently disabled and only parse + q-range run. K is set by `--spotcheck-k=N` on `serve`, default 50; K=0 disables.

### Auth
Bearer token, written by `init` to `<jobdir>/token` (chmod 600) and stored in the `meta` table. `serve` prefers the file on disk so rotating the token is "edit file + restart". `/health` and `/` (dashboard) are intentionally unauthenticated; everything else goes through `check_auth`. The server binds to loopback by default; exposing it on a LAN requires an explicit `--bind=0.0.0.0` or specific interface address.

### Files (content-addressed)
Input files (currently just the single `.job`) are stored under `<jobdir>/files/<sha>.job` and tracked in the `files` table by SHA-256. `/file/<sha>` looks up the absolute path in the DB. `init` calls `realpath()` because the server's cwd at `serve` time is unrelated to `init`'s cwd — `mg_http_serve_file` resolves relative paths against the server's cwd and would otherwise miss them.

### Workunit IDs
`wu-<jobhash>-<seq>` where `<jobhash>` is the first 8 hex chars of the `.job` SHA. `init` numbers from 0; `extend` continues the sequence using `db_workunit_extent` so IDs never collide.

### Client worker model
Each `--workers=N` spawns a pthread with its own `mg_mgr`, its own `<workdir>/wN`, its own `client_id` (`<base>-wN`). Workers share shutdown phase (`running → draining → cancelling`) plus a mutex-protected active-lease table. First Ctrl-C enters draining mode: finish active work, retry `/submit` for completed relation files if the server is unavailable, and stop requesting new leases. Second Ctrl-C enters cancelling mode: the main thread POSTs `/release` for active leases, while workers terminate active siever process groups and exit; any relation file that never got accepted is left in the worker workdir for inspection. On Linux, `--cpu-pin=a,b,c,…` pins each worker and the siever child inherits affinity.

### Sieve executor (`sieve_executor.[ch]`)
One function: `sieve_run_local()` formats the `gnfs-lasieve4*` command line, runs it through `/bin/sh -c` in a separate process group, and polls a cancellation callback while waiting. The process-group isolation keeps terminal Ctrl-C from reaching active sievers during drain; cancellation explicitly sends SIGTERM/SIGKILL to that process group. It also `remove()`s any prior file at the output path because the siever opens its `-o` argument in append mode.

### Protocol layer (`protocol.[ch]`)
All JSON encode/decode lives here so `server.c` and `client.c` don't both link cJSON usage directly. Encoders return `malloc`'d strings the caller must `free()`. `proto_decode_lease_response` enforces required-field presence and returns -1 if any are missing — the client treats that as "malformed response, back off". `POST /release` is a voluntary lease return used by client cancellation; it only succeeds for a workunit currently leased to that client and does not increment `attempt_count`.

### Dashboard
`dashboard.html` is embedded at build time via `xxd -i -n dashboard_html` → `dashboard_html.h`. Editing the HTML is enough; Make picks up the dependency. The HTML reads its bearer token from `?token=…` in the URL.

### Siever flags
`--siever-args="..."` on `init` is stored in `meta.siever_args`, sent to every client in the `/lease` response, and appended verbatim to the siever command in `sieve_run_local`. Used for tunables every worker should share — e.g. `-J 16` for a larger I-sieve area. To change it on an existing jobdir without reinitializing, edit meta directly: `sqlite3 jobdir/job.db "UPDATE meta SET value='-J 16' WHERE key='siever_args'"` and restart `serve`.

## GPU clients (cuda-sieve)

Full design and rationale in `GPU-CLIENT.md`. The parts that span files:

**Class is a property of the workunit; engine is a property of the client.**
`class` (`cpu`/`gpu`) controls only *sizing*, and therefore lease safety.
`--engine` decides which binary a client runs. They are independent, which is
what lets a GPU client fall back to cpu-class bands when the GPU band runs dry
without either side knowing about the other. `db_lease` encodes the asymmetry:
**`gpu` falls back to `cpu`, `cpu` never takes `gpu`** — a single core needs
~43 h for a 100x band, so it would time out and poison the workunit.

**Sizing.** A GPU is order 400x a single core on this job, so a normal
1000-wide workunit is only a few seconds of GPU time and per-workunit overhead
dominates. Carve GPU bands ~100x wider:

```
./ggnfs-sieve-server extend --jobdir=... --class=gpu \
    --qmin=... --qmax=... --qrange=100000 \
    --gpu-args="--logI 17 --J 16384"
```

**`gpu_args` is not a translation of `siever_args`.** `-J 16` is lasieve4
vocabulary; `--logI 17 --J 16384` is cuda-sieve's, and per cuda-sieve finding
69 they describe *different sieve areas* (`2^17 x 2^15` vs `2^17 x 2^14`). The
lease response ships both and the client picks by its own `--engine`. `serve`
reads `gpu_args` from `meta` **once at startup**, so changing it needs a
`serve` restart — `extend` says so when you set it.

**Lease heartbeat.** `POST /renew` pushes `lease_expires` out; the client
heartbeats at a third of the lease window, through the sieve *and* the upload,
and a pipelined worker heartbeats every held lease, not just the one on the
card. This is what makes one `--lease-seconds` safe across wildly different
band sizes. A 409 means the workunit was reclaimed and reissued, so the client
abandons the band immediately rather than sieving for hours into a certain
rejection.

**Keeping the card busy.** `--prefetch=N` (default 2 under `--engine=cuda`)
gives a worker N lease slots and runs lease / sieve / submit on separate
threads. Each slot needs its own `client_id` (`<base>-w0-s0`) because the
server keeps at most one live lease per `client_id`. Measured against a 1.5 s
latency proxy with 3 s bands: 44.9% card-busy serial, 78.8% at 2, 96.8% at 3.

**Factor base.** `--fbgen-gpu=<path to cuda-sieve fbgen_gpu>` builds the
`--fb1` cache once per `(job sha, logI)` instead of letting `bench` rebuild it
every workunit (~230 MB on the C208). Keying the filename on `(sha, logI)` is
why no content probe is needed. `--fb1=<path>` uses a prepared one directly.

**Progress is q-width, not workunit count.** With a GPU band ~100x a CPU one,
"400 of 404 rows done" can mean 50% of the actual sieving. `/stats` exposes
`workunits.q.*` per state plus per-class rollups, and the dashboard reads
those.

**Screening a GPU box.** `ggnfs-sieve-client benchmark --engine=cuda
--cuda-bench=... --fb1=...` times one fixed-width band on the card. Like the
CPU benchmark it takes no lease and never submits, so it is safe against a
live coordinator. `cuda-client.sh` bootstraps a GPU worker and writes both
`run-cuda-client.sh` and `benchmark-gpu.sh` (all three generated/deployment
scripts are gitignored; `cuda-client.sh` itself is tracked because it prompts
for server and token rather than baking them in, unlike `ggnfs-client.sh`,
which is untracked for exactly that reason).

## Things that have bitten people (load-bearing detail)

- `mfbr`/`mfba` and `lpbr`/`lpba` in `input.job` must match what you use for filtering later — `finalize-nfs.sh` aborts if `<yafu-dir>/nfs.job` SHA differs from the `.job` the server distributed, because mismatched factor base settings silently corrupt filtering.
- Clients submit relation files compressed with zstd level 1 (`X-Compression: zstd`). The server stores those as `<workunit>.dat.zst`; the verifier streams decompression while parsing. Raw uncompressed submissions are still accepted for compatibility.
- `/submit` counts `\n` bytes in the submitted relation stream as a fast initial estimate (the JSON response carries that count). For zstd uploads the server counts while streaming decompression. The verifier replaces it with the actual parsed line count when the submission passes, so stats / dashboard reflect real counts only after verification.
- `OUTPUT_MAX_BYTES = 500 MiB`. The server's `MG_MAX_RECV_SIZE` is set to allow that for compressed request bodies; the zstd submit path also rejects decoded relation streams above `OUTPUT_MAX_BYTES`. These limits need to stay in sync if either is bumped.
- `sqlite3_busy_timeout=5s` in `db_open` is what lets the verifier and event-loop threads share a DB file without explicit locking. Dropping it would surface `SQLITE_BUSY` on the main thread under submit load. It is set **before** the schema DDL on purpose: `db_migrate`'s `ALTER TABLE` and the index build in `SCHEMA_SQL_POST` are the most contended statements the process runs (a 430K-row index build against a live `serve`), and without the timeout already in effect they fail `db_open` outright.
- The server has no graceful shutdown today (`for (;;) mg_mgr_poll(...)`); the cleanup code below the loop — including `verify_thread_stop` — is unreachable. SIGINT just kills it. The DB is in WAL mode so this is fine. (Tracked in `FUTURE.md`.)
- **cuda-sieve's `--qrange MIN:MAX` is INCLUSIVE of MAX**, while a workunit is the half-open `[q_start, q_start+q_range)` and `verify.c` enforces that. `sieve_run_cuda` therefore passes `q_start + q_range - 1`. Get it wrong and every band whose top edge happens to be prime fails verification, requeues, and eventually poisons — while looking like a cuda-sieve bug. Pinned by `sqgen_create` in `cuda-sieve/bench/fbgen.c` and its `inclusive_single_prime` test.
- `VERIFY_MAX_PRIMES_PER_SIDE` counts primes **with multiplicity** — a relation lists a prime once per division, so it bounds factorisation *length*, not distinct primes. It is 64, matching cuda-sieve's `TD_FMAX`. It was 32, and real cuda-sieve output reaches 35 (a 224-bit algebraic norm carrying thirteen factors of 2); gnfs-lasieve4 peaks at 13 on this job, which is why 32 survived so long. One over-length line fails the whole submission, so this presents as every GPU workunit failing.
- The verifier's spot-check sample size scales with `q_range` against the job's **most common** band width, read live from the workunits table (`db_workunit_base_q_range`). It is derived rather than configured because the normal GPU rollout is `extend --class=gpu` onto a campaign whose `meta` predates all of this.
- The siever's `-c` (in `sieve_run_local`, and `benchmark --qrange`) is a special-q interval **width**, not a count of special-q — the same unit as a workunit's `q_range`. The window `[f, f+c)` holds ~`c/ln(q)` prime special-q. A `-c` narrower than ~`ln(q)` contains no prime, so the siever does zero work and yields **0 relations in ~2s** (all `sieve:` counters 0) — a failure that mimics a broken job/siever/cache. This is why `benchmark --qrange` defaults to 65 (~3 special-q at q≈80M, ~1 min single-core), not a small number.
