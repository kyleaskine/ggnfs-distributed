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

There is no linter config and no CI. There is a small hand-run test suite under `tests/`, built and run by `make test`:

- `tests/block_test.c` — links `db.o` directly (no server, no network) and exercises the GPU block layer: lease/renew/submit/release, contiguity, the attempt ceiling, expiry accounting, id sequencing. **Every geometry case runs in both scan directions**, because `db_block_lease` stores its candidate run in scan order and an index naming one end descending names the other end ascending. Both plausible sign errors were introduced deliberately during development; each produced 71 failures here.
- `tests/dashboard_test.js` — checks the client rollup arithmetic, reading the functions straight out of `dashboard.html` so it cannot drift from what ships. Needs `node`; skipped with a notice if absent. That column has been wrong twice in opposite directions, so it is worth a check.
- `tests/finalize_test.py` — offline pull/finalize integration tests, run by `make test` with Python 3, sqlite3, and zstd. Covers job identity, interrupted snapshots, CPU/GPU assembly, YAFU export limits, output preservation, and metadata preflight using local SSH/rsync stand-ins.

These do not cover the HTTP layer or the verifier end to end; those were tested by hand against real archived relations.

`dashboard_html.h` is generated from `dashboard.html` by `xxd -i` and gets regenerated automatically because `server.o` depends on it.

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
#    GPU block sizing (see "GPU clients"): --block-width-multiple=50
#    --block-max-members=256 --block-attempt-ceiling=2

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

The dashboard is at `http://host:8080/` — the HTML itself is unauthenticated, and the page asks for a credential rather than reading one from the URL (see **Dashboard**). Paste `<jobdir>/view-token` for a read-only view. `serve` defaults to `--bind=127.0.0.1`; use `--bind=0.0.0.0` only when the coordinator should be reachable from other machines.

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
**Two credentials, both 64 lowercase hex, both written by `init` (chmod 600) and stored in `meta`:**

- `<jobdir>/token` — full access. Checked by `check_auth` on `/lease`, `/file`, `/submit`, `/renew`, `/release`.
- `<jobdir>/view-token` — read-only. Accepted by **`/stats` alone**, via `check_auth2(c, hm, ctx->token, ctx->view_token)`. Every other endpoint passes `tok_b = NULL`. The `strlen(tok_b) == 64` guard in `check_auth2` is load-bearing: an unset view token is `""`, and without it an empty or malformed Bearer value would compare equal and authorise the request.

Precedence for each: `--token-file` / `--view-token-file` > `<jobdir>/<name>` > `meta`. An **explicit** `--token-file` that is unreadable or not 64 chars is a hard startup failure, never a silent fallback — falling back to the jobdir's own token would 401 the entire fleet against a server that looks healthy. The implicit `<jobdir>/token` may legitimately be absent and still falls back to `meta`. The startup banner names which source won.

`init --token=<64hex|@path>` adopts an existing key instead of minting one. That is what makes a job switch free: one fleet key across every jobdir means a new factorization needs no client reconfiguration at all. Uppercase is **rejected** rather than folded — `check_auth` compares case-sensitively, so normalising would make the value the operator typed differ from the one clients must send.

A jobdir created before view tokens existed gets one minted and persisted on the next `serve`, so no migration is needed for live campaigns (which cannot be re-`init`ed without orphaning them). A view token equal to the full token is refused/disabled: it would silently turn the status credential into a full-access one.

`/health` and `/` (dashboard) are intentionally unauthenticated; the dashboard HTML carries no credential and asks for one. The server binds to loopback by default; exposing it on a LAN requires an explicit `--bind=0.0.0.0` or specific interface address.

### Files (content-addressed)
Input files (currently just the single `.job`) are stored under `<jobdir>/files/<sha>.job` and tracked in the `files` table by SHA-256. `/file/<sha>` looks up the absolute path in the DB. `init` calls `realpath()` because the server's cwd at `serve` time is unrelated to `init`'s cwd — `mg_http_serve_file` resolves relative paths against the server's cwd and would otherwise miss them.

### Workunit IDs
`wu-<jobhash>-<seq>` where `<jobhash>` is the first 8 hex chars of the `.job` SHA. `init` numbers from 0; `extend` continues the sequence using `db_workunit_extent` so IDs never collide.

### Client worker model
Each `--workers=N` spawns a pthread with its own `mg_mgr`, its own `<workdir>/wN`, its own `client_id` (`<base>-wN`). Workers share shutdown phase (`running → draining → cancelling`) plus a mutex-protected active-lease table. First Ctrl-C enters draining mode: finish active work, retry `/submit` for completed relation files if the server is unavailable, and stop requesting new leases. Under `--prefetch=N` "active work" is the band on the card and nothing else — a prefetched slot is a lease no time has been spent on, so the heartbeat thread `/release`s it within a second rather than letting it add another band's wall clock to the shutdown. Second Ctrl-C enters cancelling mode: the main thread POSTs `/release` for active leases, while workers terminate active siever process groups and exit; any relation file that never got accepted is left in the worker workdir for inspection. On Linux, `--cpu-pin=a,b,c,…` pins each worker and the siever child inherits affinity.

### Sieve executor (`sieve_executor.[ch]`)
`sieve_run_local()` formats the `gnfs-lasieve4*` command line; `sieve_run_cuda()` and `sieve_run_command()` do the same for the other two callers. All three hand the string to `run_child_cancelable()`, which runs it as `/bin/sh -c "exec ..."` in a separate process group and polls a cancellation callback while waiting. The `exec` is load-bearing: dash does not exec a `-c` command in place, so without it the pid being waited on is the shell's, not the siever's. The process-group isolation keeps terminal Ctrl-C from reaching active sievers during drain; cancellation explicitly sends SIGTERM/SIGKILL to that process group. It also `remove()`s any prior file at the output path because the siever opens its `-o` argument in append mode.

### Protocol layer (`protocol.[ch]`)
All JSON encode/decode lives here so `server.c` and `client.c` don't both link cJSON usage directly. Encoders return `malloc`'d strings the caller must `free()`. `proto_decode_lease_response` enforces required-field presence and returns -1 if any are missing — the client treats that as "malformed response, back off". `POST /release` is a voluntary lease return used by client cancellation; it only succeeds for a workunit currently leased to that client and does not increment `attempt_count`.

### Dashboard
`dashboard.html` is embedded at build time via `xxd -i -n dashboard_html` → `dashboard_html.h`. Editing the HTML is enough; Make picks up the dependency.

**The credential is never in the URL.** It used to be (`?token=…`), and that token could lease work and submit relations while sitting in browser history, bookmarks, referrer headers and anyone's view of the address bar. The page now asks for one, keeps it in `sessionStorage` (per-tab, dies with the tab), and sends it as a Bearer header exactly as before. `?token=`/`?view=` are still accepted for links already in circulation but are moved into storage and stripped with `history.replaceState` immediately. A 401 drops the stored value rather than re-polling it on a timer forever. Every `sessionStorage` access is wrapped in try/catch — it throws outright in some privacy modes, where the page must still work for one visit.

Paste the **view token** for a read-only view; `/stats` is the only endpoint the page calls, so that is sufficient for the whole dashboard.

### Siever flags
**The siever binary is a property of the JOB, not of the client.** The server names it in every `/lease` response, and the client resolves `<--siever dir>/<lease.siever>` per lease via `sieve_resolve_siever` (`sieve_executor.c`). Point `--siever` at a directory holding every `gnfs-lasieve4I*` the fleet might be asked for and a worker follows any campaign; point it at a single file and that build is pinned, with a mismatch warning (which is what an operator draining a tail or testing a build wants).

Resolution happens in `stage_acquire` **before** `ensure_afb_cached`, which probes the binary and would unlink a good `.afb.0` if handed a directory. `pipe_slot_t` holds a per-slot copy of the config, and both the serial worker and the pipelined GPU worker funnel through `stage_acquire`, so that is the single resolution point for every engine and worker shape. Only the lease thread writes `slot->cfg.siever_path`; only the sieve loop reads it, with the slot-state handoff between them.

`sieve_resolve_siever` validates the server-supplied name against `gnfs-lasieve4I<N>e` and refuses anything else, because `sieve_run_local` formats the resolved path into a command string that `run_child_cancelable` hands to `/bin/sh -c` — so the name is a shell command word, and a traversal in it would also escape the siever directory.

**`siever_args` and `gpu_args` are not forwarded either — they are parsed and rebuilt.** `sieve_sanitize_args` (`sieve_executor.c`) holds the only table of flags a coordinator can influence (`-J` for lasieve4; `--logI`/`--J` for cuda), parses each value as a bounded integer, and emits a fresh string built from those integers. No byte of the server's string survives into the command line. An unrecognised flag **fails the lease** rather than being dropped, because dropping one silently changes the sieve area — the same class of quiet wrongness as running the wrong siever.

This is what stops **argument** injection, which is a different thing from shell injection and which an argv array would not prevent: `-o /home/you/.ssh/authorized_keys` has no metacharacter in it and is a perfectly well-formed argument. The operator's own `--siever-args` / `--gpu-args` bypass the table — those are local intent, and the escape hatch so a refusal is never a dead end.

What remains is that the command is still *composed as a shell string*. Every field reaching it is now either locally generated or rebuilt from parsed integers, so there is nothing left to inject with; but `run_child_cancelable` still runs `/bin/sh -c`, and the argv/`execvp` repair in `FUTURE.md` is what would make that structural rather than maintained.

`--siever-args="..."` on `init` is stored in `meta.siever_args` and sent to every client in the `/lease` response, where it goes through `sieve_sanitize_args` above before reaching `sieve_run_local` — so it must use flags that table knows, currently just `-J`. Used for tunables every worker should share — e.g. `-J 16` for a larger I-sieve area. To change it on an existing jobdir without reinitializing, edit meta directly: `sqlite3 jobdir/job.db "UPDATE meta SET value='-J 16' WHERE key='siever_args'"` and restart `serve`.

### Client bootstrap
`setup-client.sh` is the single bootstrap for both CPU and GPU workers, replacing the old `ggnfs-client.sh` / `cuda-client.sh` pair. It detects what the box can do, takes `--cpu` / `--gpu` / `--both`, and writes the four runner scripts (`run-client.sh`, `benchmark.sh`, `run-cuda-client.sh`, `benchmark-gpu.sh`) — those stay separate, since running a siever and setting a box up are different jobs. GPU prerequisites are fatal only when GPU was *explicitly* asked for; under auto-detect a box with no `nvidia-smi`/`nvcc` falls back to CPU with a notice, which is what makes one `curl | bash` URL safe to point at any machine.

**CPU and GPU get independent client ids, not one id with suffixes.** The defaults are derived from the hardware — `hw_tag_cpu` pulls the model number out of `/proc/cpuinfo` and `hw_tag_gpu` out of `nvidia-smi`, giving `KyleAskine-9800X3D` and `KyleAskine-5070` from a base of `KyleAskine`. A dashboard row then says which silicon produced the relations, which `KyleAskine` sitting next to `KyleAskine-gpu0` does not, and on a two-card box the CPU fleet is no longer the one entry with no tag. `--cpu-client-id=` / `--gpu-client-id=` override outright, so an unusual part deriving an ugly tag costs nothing. When two cards would derive the same name the device index is appended — the server keeps one live lease per `client_id`, so identically-named cards would fight over it.

Unlike its predecessors it holds **no server address and no token**, taking them from `--server=`/`--token=`, so it is tracked in git. A deployment that wants defaults baked in wraps it in a two-line script that supplies them; those wrappers stay gitignored and webserver-deployed as before. It installs **all** of `gnfs-lasieve4I{14,15,16}e` from the box's microarch tier into `sievers/` and passes `--siever=<that dir>`, which is the client-side half of following a job change.

## GPU clients (cuda-sieve)

Full design and rationale in `GPU-CLIENT.md`. The parts that span files:

**Sizing is a property of the LEASE, not of the workunit.** Workunits stay
canonical at one base width forever. A GPU client asks for a **block**: one
lease held over N contiguous `available` workunits. Nothing is merged, nothing
is resized, and every member keeps its own row and its own `attempt_count`.

This replaced v1's `class`-sized rows, where `gpu` meant a workunit ~50x wider.
That conflated the unit of *work* with the unit of *assignment* and every
problem it had followed from that: a wide band left behind when a card
disconnected was unreachable by the CPU fleet, and a lease expiry permanently
demoted its q-range to un-mergeable. `recarve` and the gpu→cpu lease fallback
existed to paper over both and are gone; `extend --class=gpu` now refuses.
`workunits.class` survives as an unused column (removing it would be a
migration against a live 430K-row DB) and `clients.last_class` is still what
labels a row on the dashboard.

**Blocks are addressed by their ANCHOR workunit** — the lowest-q member, a real
workunit id. `gpu_blocks.id` (`blk-<jobhash>-NNNNNN`) is internal. This is why
`/lease`, `/submit`, `/renew`, `/release`, `workunit_id_is_safe_for_job` and the
client's whole sieve path needed no change: a block arrives through the ordinary
`workunit_id` / `q_start` / `q_range` fields and just looks like a wider band.

**Membership is derived, not stored.** There is deliberately no
`workunits.block_id`. A block's members are exactly the rows with
`q_start` in `[q_start, q_end)`, which is exact because a block is always a
contiguous run and no second block can form over rows that are not `available`.
A column would have to be cleared on five terminal paths and would silently
strand rows if any were missed — the same one-way ratchet blocks exist to
remove. `gpu_blocks.state` is the single source of truth for "is this block
live".

**Block width is derived from the job, not configured absolutely:**

> `target = --block-width-multiple (default 50) x db_workunit_base_q_range()`

Base q_range is already normalised to wall clock — an operator picks `--qrange`
so one workunit is a sane slice of a *core* — and a GPU is a roughly
job-independent multiple of a core (~42x measured), so a fixed multiple holds
the GPU band near a constant wall-clock target across jobs of any size. A
hardcoded width would be a 3-minute band on one job and hours on the next.
`db_workunit_base_q_range` is a 38 ms full index scan on a 390K-row jobdir, so
`serve` caches it with a 60 s TTL (`base_q_range_cached`) — long enough to be
free, short enough that `extend` under a running server is picked up.

Measured on prod (RTX 5070 vs a 9800X3D core, same 1,000-wide work): 22% of
every GPU band is fixed cuda-sieve startup, and the card sits 88% busy at
`--prefetch=2`. At 50x both collapse — startup to 0.6%, latency hidden behind a
~16 min band — taking the card from **176 to ~255 rel/sec**. 20x already gets
252.9, so the last 30x buys under 1%; the reason to go further is 2.5x fewer
lease/submit transactions on the single event-loop thread as the fleet grows,
and the cost is reclaim granularity, since there is no partial submit.

**Poisoning: failure state lives on the workunit, never on the block.**
`gpu_blocks` has no `attempt_count`. A lease expiry or a verify failure charges
every member one strike, which without a brake would let one flaky host poison a
contiguous 50-workunit region in five lease windows. `--block-attempt-ceiling`
(default 2, forced below `--max-attempts` at startup) breaks the *re-lease loop*
instead: past the ceiling a range is no longer block-eligible and degrades to
individual leases. **The invariant: at most `ceiling` of a workunit's strikes
can come from block-scale evidence, so nothing is ever poisoned without
`max_attempts - ceiling` failures that were specifically about that one
base-width range.**

**Blocks are carved alongside the workunit fleet, not at the opposite end.**
The block scan follows `meta.lease_order` (override with
`--block-lease-order=asc|desc`). Opposite ends was the original design and it is
wrong for how campaigns actually end: they are stopped once there are enough
relations rather than run to exhaustion, so two fleets converging from opposite
ends leave a hole in the middle. Working adjacent costs nothing because there is
no churn zone — `db_lease` consumes strictly in order from whichever end
`meta.lease_order` names, so in-flight leases sit in a solid block just *behind*
the frontier and everything ahead is one unbroken run (measured live under the
default `asc`: highest leased 94,309,000, lowest available 94,310,000, one run
of 355,690 ahead; under `desc` the picture mirrors). It also reclaims failures
sooner, since requeued rows land behind the frontier where the next lease from
either fleet finds them.

**`db_block_lease`'s run array is in SCAN order, so the ends must be picked by
direction, not by index.** `lo = scan_desc ? &run[len-1] : &run[0]`. An index
that names the low end descending names the high end ascending; get it wrong and
every block reports `q_start`/`q_end` inverted, which passes a smoke test and
then fails verification on every band. `tests/block_test.c` runs both directions
for this reason.

**Sweep ordering is load-bearing.** `db_block_expire_sweep` MUST run before
`db_lease_expire_sweep` in `on_sweep_timer`. The block sweep returns expired
members to `available`, and the row sweep's own `state = 'leased'` test then
excludes them. Reverse the two and every member is incremented twice.

**`idx_wu_lease_v2(state, q_start, q_range)` is required.** The block scan
cannot constrain `class`, and without a class equality the older
`idx_wu_lease(state, class, q_start, q_range)` stops ordering usefully —
`EXPLAIN QUERY PLAN` shows `USE TEMP B-TREE FOR ORDER BY` over all 356K
available rows, on the mongoose thread, per lease.

**Block relation files are named after the BLOCK, not the anchor:**
`rels/blk-<jobhash>-NNNNNN.dat.zst`. A block that passes with its lowest-q
member starved sends that member back to `available`; re-leased individually it
would submit to `<anchor>.dat.zst` and overwrite the block's file, silently
discarding the other members' relations. `finalize-nfs.sh` now selects files
from `submissions WHERE verify_status='passed'` rather than globbing (which also
stops failed submissions being assembled), and its no-DB fallback globs **both**
`wu-*` and `blk-*` — dropping `blk-*` there would still find CPU files, so
nothing would look wrong until filtering came up short. `move-rels.sh` reads
verified ids from `workunits` *and* `gpu_blocks`.

**Verification treats a block as one wide band.** `db_verify_next_pending` is a
`UNION ALL`; the per-workunit arm needs `AND s.block_id IS NULL` because a block
submission stores its anchor in `workunit_id` and would otherwise match both
arms and be checked against the anchor's base-width q-range. Use `ORDER BY 1` —
SQLite rejects a qualified column in a compound SELECT's ORDER BY.

**Starved members are found by threshold, not by zero.** Relations are
attributed to a member by the smallest sieved-side prime inside the block range;
the file does not record which prime was the special-q, and a relation
occasionally carries a second in-range prime below its own. Measured on a real
6-member block with one member's output deliberately omitted, that member still
attracted **1** stray against a median of 3383 — so a `== 0` test never fires
and would silently verify a q-range that was never sieved. The rule is
`count * 20 < median`, applied only when the block as a whole passed.
Requeueing exactly the starved members is what keeps partial resolution safe:
the stored file holds no relations for those ranges, so re-sieving cannot
duplicate assembled work.

**Client side is `--blocks=yes|no`** (default yes under `--engine=cuda`, no
otherwise — a block is sized for a card and would blow the lease window on one
core) plus `--block-max-members=N`, which the server clamps. A server that
predates blocks ignores both and returns an ordinary workunit.

**Lease heartbeat.** `POST /renew` on the anchor pushes every member's
`lease_expires` out, under the same guards a single workunit gets
(`state='leased' AND leased_to=? AND lease_expires >= now`). At the default
`--lease-seconds=3600` a 50x block (~16 min) fits one window with 3.8x headroom
and needs no renew at all in the common case; blocks only stop fitting past
~190x.

**Keeping the card busy.** `--prefetch=N` (default 2 under `--engine=cuda`)
gives a worker N lease slots on separate threads. Each slot needs its own
`client_id` (`<base>-w0-s0`) because the server keeps at most one live lease per
`client_id` — and one live *block* per `client_id`, which is what makes a lost
`/lease` response retried return the same block instead of a second one. On a
single Ctrl-C only the slot on the card finishes; the other slots hand their
leases straight back, so shutdown costs one band rather than `prefetch` of them.

**Geometry is derived from the job's own siever, so you usually set nothing.**
`-J n` is lasieve4 vocabulary and `--logI/--J` is cuda-sieve's; they are not the
same words for the same thing, and the axis order differs. cuda-sieve finding 65
measured the equivalence by inverting the q-lattice from emitted relations:

| GGNFS | rectangle | cuda-sieve |
|---|---|---|
| `I14e` | 2^14 x 2^13 | `--logI 14 --J 8192` |
| `I14e -J 14` | 2^15 x 2^13 | `--logI 15 --J 8192` |
| `I16e` | 2^16 x 2^15 | `--logI 16 --J 32768` |
| `I16e -J 16` | 2^17 x 2^15 | `--logI 17 --J 32768` |

One rule covers all of it, with GGNFS's J_bits defaulting to I-1:
**`--logI` = J_bits + 1, `--J` = 2^(I-1)** (`derive_gpu_args` in `client.c`).

Precedence: the client's `--gpu-args`, then `meta.gpu_args`, then the
derivation. A configured value wins — that is operator intent — but the client
warns when it disagrees with what the campaign's siever implies, because that
combination is how a card quietly sieves a different area than the CPU fleet for
a whole campaign. `serve` reads `gpu_args` from `meta` **once at startup**, so
changing it needs a `serve` restart.

**Non-ASCII in the `.job`.** `init` refuses a `.job` containing bytes cuda-sieve
cannot parse (CRLF and tabs are fine). This matters because the `.job`'s SHA *is*
the job's identity — workunit IDs derive from it and `finalize-nfs.sh` compares
it — so it cannot be corrected afterwards without orphaning the campaign. A live
job was found carrying a zero-width space after `alambda: 3.6`: lasieve4 ignored
it for weeks while cuda-sieve refused the file outright. For campaigns already in
that state the cuda client sieves from a sanitized copy and leaves the
distributed file byte-exact.

**Factor base.** `--fbgen-gpu=<path to cuda-sieve fbgen_gpu>` builds the `--fb1`
cache once per `(job sha, logI)` instead of letting `bench` rebuild it every
workunit (~230 MB on the C208). Keying the filename on `(sha, logI)` is why no
content probe is needed. `--fb1=<path>` uses a prepared one directly.

**Progress is q-width, not workunit count.** `/stats` exposes `workunits.q.*`
per state plus per-class rollups, and the dashboard reads those. `q_passed_1h`
sums `COALESCE(NULLIF(s.q_width,0), w.q_range)` so a block contributes its own
width rather than its anchor's. Per-client stats use `SUM(s.member_count)` for
the workunit count and expose **rel/sec**.

**Every relation count in this system is RAW, not unique — and the two engines
differ.** lasieve4 truncates its special-q-side factor base at q and so finds
each relation once; cuda-sieve does not truncate and re-finds the same relation
at several special-q, inflating its raw counts **~1.34x** for the same unique
yield (cuda-sieve `RUNBOOK.md`). The prod +35% rel/wu gap is therefore an
artefact: 4,780 / 1.34 = 3,567 unique against the CPU's 3,616 — slightly
*behind*, which is what the RUNBOOK says happens at matched factor bases.
cuda-sieve's genuine edge, from its looser survivor gate, is **1.6%** measured
against AS276's real corpus (RUNBOOK finding 69). So `rel/wu` is a same-engine
health check only, never an engine comparison; the dashboard labels the rate
columns `(raw)`; and unique yield is knowable only after msieve dedups at
filter time. q-width progress is unaffected, which is one more reason progress
is reported in q-width rather than relations.

**Dashboard rel/sec aggregates at the device, not the worker or the box.**
Workers run in parallel so their `sieve_seconds` overlap in wall clock, but
lease slots of one worker (`-w0-s0`, `-w0-s1`) share a card and serialise on it.
Summing worker rates read 582 rel/s for a card that delivered 176. The rule is
`relations / SUM(seconds)` **within** a device, then summed **across** devices —
correct for a 32-core box, a 2-slot card, and a box holding two GPUs. It is
throughput while sieving; idle gaps between bands are excluded for both
engines.

**Screening a GPU box.** `ggnfs-sieve-client benchmark --engine=cuda
--cuda-bench=... --fb1=...` times one fixed-width band on the card. Like the CPU
benchmark it takes no lease and never submits, so it is safe against a live
coordinator. `cuda-client.sh` bootstraps a GPU worker and writes
`run-cuda-client.sh` and `benchmark-gpu.sh`. Like `ggnfs-client.sh` it bakes in
the live server and token, so all of them are gitignored and deployed by copying
to the webserver rather than by being cloned.

## Things that have bitten people (load-bearing detail)

- **The client used to only WARN when the server asked for a different siever, then run the configured one anyway.** Sieving a job with the wrong `I` still produces *valid* relations — the norms check out, the q-range checks out, the verifier passes it — so nothing anywhere fails. The box just quietly delivers a fraction of the yield for an entire campaign. It is now a hard refusal under `--siever=<dir>`.
- **That refusal RELEASES the lease; it must not merely abandon it.** The nearby error paths in `stage_acquire` release only when `shutdown_phase() >= SHUTDOWN_DRAINING` and otherwise just `return -2`, leaving the workunit to the expiry sweep. Copying that shape here would be actively destructive: a missing siever is *permanent*, so it recurs on every lease, and the sweep charges `attempt_count` each time — roughly 120 abandoned workunits an hour per worker, 50 at a time under `--engine=cuda`, poisoning a contiguous stretch of the q-range while the dashboard shows nothing but a rising `leased` count. The backoff is also floored at `MISCONFIG_BACKOFF_SECONDS` regardless of `--idle-backoff`.
- **A new factorization means a new `.job`.** `job_id` is the first 8 hex of the job SHA, and `workunit_id_is_safe_for_job` is the only guard on `/submit`, `/renew` and `/release`. Two jobdirs built from the *same* `.job` therefore mint colliding `wu-<jobhash>-<seq>` ids, and a submission for one can be accepted against the other's q-range — silently, if both were carved with the same `--qmin`/`--qrange`.
- **Per-job state must be KEYED, never latched.** `g_afb_gen_failed`, `g_fb1_failed` and `effective_gpu_args`'s `warned` were process-lifetime booleans. Once `serve` can be repointed at a new jobdir under a running client, a verdict about job A carries silently into job B forever — one failed factor-base build disables the cache for every later job, and the geometry warning goes quiet exactly when the campaign changes, which is when it exists to fire. All are now keyed on the path/sha/tuple they describe. `g_fb1_ready` always was; `g_afb_validated_path` was keyed on the `.afb.0` alone, which is not enough once the siever can change under a fixed job sha — it now carries the binary that audited it, or the `-c 0` audit is skipped for a siever that has never seen that cache. Keying also avoids a reset hook needing to take `g_afb_mu`, which `ensure_afb_cached` holds across the whole 30-45s generation.
- **`do_renew` must map HTTP 400 to "this job is gone", not to "transient".** After a repoint, `/renew` on an old-job id 400s. Treated as transient it retries at the normal cadence and the siever grinds out the full band — a quarter hour on a GPU block — which `/submit` can then only answer with another 400. Worse under `--prefetch=N`: `pipe_heartbeat_thread` dropped a slot only on `r == 1`, so a *queued* slot survived, was promoted later, and burned a second band on a workunit that could never be submitted.
- **`ensure_gpu_fb_cached` must not hand out a pointer to `g_fb1_ready`.** `stage_acquire` stashed that bare pointer in `slot->fb1` and `stage_sieve` dereferenced it on another thread minutes later, outside `g_fb1_mu`, while another prefetch slot could be re-pointing the global at a different `(sha, logI)` cache. It fills a caller buffer under the lock instead.
- **Nothing off the wire is forwarded into the siever command line — it is all parsed and rebuilt.** `siever` is validated against `gnfs-lasieve4I<N>e`; `siever_args` and `gpu_args` go through `sieve_sanitize_args`, which accepts only the flags in its table and re-emits them from parsed integers. If you add a fourth wire-supplied field, it needs the same treatment: the command is still composed as a `/bin/sh -c` string, so it is safe only because every input to it is now locally generated. An unknown flag fails the lease rather than being dropped — a silently dropped `-J` changes the sieve area, which nothing downstream would notice.
- **Wire fields that become PATHS are validated at decode, not at use.** `proto_decode_lease_response` requires `file_sha256_hex` to be 64 lowercase hex and `workunit_id` / `output_name` / `file_name` to be safe single components. Before that, `ensure_file_cached` took `file_sha256_hex` straight into `path_join` and **`unlink`ed the result before downloading anything** — so a hostile coordinator deleted an arbitrary file the worker could reach, on every lease, without the download even needing to succeed. Validating at the one decode choke point rather than per use site is what makes a new use site inherit the guarantee.
- **`check_auth2`'s second token needs a `strlen(tok_b) == 64` guard.** An unset view token is `""`; without the length test an empty or malformed Bearer value compares equal to it and authorises `/stats`.
- **`dist/ggnfs-portable/*` carry no `Trimmed cached aFB` marker.** The keep-if-current check in the bootstrap fails there on every run and re-copies, and the client's matching "predates factor-base cache support" warning would print once per lease if it were not keyed on the siever path. Both say why rather than looking like loops.

- `mfbr`/`mfba` and `lpbr`/`lpba` in `input.job` must match what you use for filtering later — `finalize-nfs.sh` aborts if `<yafu-dir>/nfs.job` SHA differs from the `.job` the server distributed, because mismatched factor base settings silently corrupt filtering.
- Clients submit relation files compressed with zstd level 1 (`X-Compression: zstd`). The server stores those as `<workunit>.dat.zst`; the verifier streams decompression while parsing. Raw uncompressed submissions are still accepted for compatibility.
- `/submit` counts `\n` bytes in the submitted relation stream as a fast initial estimate (the JSON response carries that count). For zstd uploads the server counts while streaming decompression. The verifier replaces it with the actual parsed line count when the submission passes, so stats / dashboard reflect real counts only after verification.
- `OUTPUT_MAX_BYTES = 500 MiB`. The server's `MG_MAX_RECV_SIZE` is set to allow that for compressed request bodies; the zstd submit path also rejects decoded relation streams above `OUTPUT_MAX_BYTES`. These limits need to stay in sync if either is bumped.
- `sqlite3_busy_timeout=5s` in `db_open` is what lets the verifier and event-loop threads share a DB file without explicit locking. Dropping it would surface `SQLITE_BUSY` on the main thread under submit load. It is set **before** the schema DDL on purpose: `db_migrate`'s `ALTER TABLE` and the index build in `SCHEMA_SQL_POST` are the most contended statements the process runs (a 430K-row index build against a live `serve`), and without the timeout already in effect they fail `db_open` outright.
- The server has no graceful shutdown today (`for (;;) mg_mgr_poll(...)`); the cleanup code below the loop — including `verify_thread_stop` — is unreachable. SIGINT just kills it. The DB is in WAL mode so this is fine. (Tracked in `FUTURE.md`.)
- **cuda-sieve's `--qrange MIN:MAX` is INCLUSIVE of MAX**, while a workunit is the half-open `[q_start, q_start+q_range)` and `verify.c` enforces that. `sieve_run_cuda` therefore passes `q_start + q_range - 1`. Get it wrong and every band whose top edge happens to be prime fails verification, requeues, and eventually poisons — while looking like a cuda-sieve bug. Pinned by `sqgen_create` in `cuda-sieve/bench/fbgen.c` and its `inclusive_single_prime` test.
- `VERIFY_MAX_PRIMES_PER_SIDE` counts primes **with multiplicity** — a relation lists a prime once per division, so it bounds factorisation *length*, not distinct primes. It is 64, matching cuda-sieve's `TD_FMAX`. It was 32, and real cuda-sieve output reaches 35 (a 224-bit algebraic norm carrying thirteen factors of 2); gnfs-lasieve4 peaks at 13 on this job, which is why 32 survived so long. One over-length line fails the whole submission, so this presents as every GPU workunit failing.
- The verifier's spot-check sample size scales with `q_range` against the job's **most common** band width, read live from the workunits table (`db_workunit_base_q_range`). It is derived rather than configured because a GPU block's width is many times the base and its `meta` may predate all of this.
- **`db_block_expire_sweep` must run before `db_lease_expire_sweep`.** That ordering is the *only* thing keeping the per-workunit sweep off block members; reversing it double-increments every member's `attempt_count`.
- **A block's relation file is named after the block, not its anchor.** Anchor naming looks fine until a block passes with its lowest-q member starved: that member is requeued, re-sieved on its own, and its submission overwrites the block's file — silently discarding the other members' relations, with nothing visibly wrong until filtering comes up short.
- **Starved-member detection compares relation DENSITY, and falls back to the max when the median is 0.** Raw counts requeue a legitimately-sieved narrow member when a block mixes band widths (normal after an `extend --qrange=N` at a different N). And a `median > 0` guard turns the check off exactly when it matters most: if over half a block's members produced nothing — a truncated siever run — the median is 0, the check is skipped, and every unsieved sub-range is marked `verified` and lost for the rest of the campaign.
- **A big `/submit` used to cost the whole fleet ~16 seconds of `memcpy`.** mongoose grows `c->recv` by a flat `MG_IO_SIZE` (16 KiB) per read, and `mg_iobuf_resize` deliberately does *not* `realloc`: it callocs the new size, memmoves the old contents in, then bzeroes the old buffer before freeing it. That is three passes over everything received so far for every 16 KiB that arrives — quadratic in body size, on the single event-loop thread that also answers `/lease`, `/renew` and the dashboard. Measured over loopback against this server: 2 MiB 0.13s, 8 MiB 1.1s, 16 MiB 3.7s, 24 MiB 8.7s, **32 MiB 16.6s**. A 50-member GPU block is ~32 MiB compressed, so every block submission froze the coordinator for sixteen seconds and the submitting client timed out on a submission that was in fact accepted. End to end through the full authenticated handler (auth, sha256, newline count, 32 MiB temp-file write, DB transaction), the same 32 MiB POST went **18.54s -> 1.26s**. `ev_handler` now raises `c->recv.align` to `BIG_BODY_RECV_ALIGN` (4 MiB) once `c->recv.len` passes `BIG_BODY_THRESHOLD` (1 MiB); `mg_iobuf_resize` rounds up to `align`, which is the only growth knob mongoose exposes without editing `vendor/`. 32 MiB went 16.6s → 0.7s. It is raised only after the bytes have actually **arrived**, never on the strength of a `Content-Length` a client merely claimed, and setting `align` affects only the next `ioalloc` so it cannot move the buffer mongoose is mid-parse on.

- **`http_request`'s timeout is a NO-PROGRESS budget on the clock, not a total budget counted in polls.** It used to do `waited_ms += 200` once per `mg_mgr_poll(mgr, 200)` — but poll returns as soon as the socket is ready, so polls that took microseconds were each charged 200 ms. Measured: a 64 MiB POST that finished in **307 ms** was declared a "3 second timeout", and `/renew`'s nominal 5s could fire in well under one. Separately, *any* fixed total budget is a bet on the worker's uplink: a 32 MiB GPU-block `/submit` blew the old flat 60s, so the client printed `/submit connection failure` and re-uploaded 32 MiB twice more for relations the coordinator had already stored and verified — the third try drew a 409, reported as "(re-issued?)" when nothing had been reissued. The budget now restarts whenever `c->send.len` falls or `c->recv.len` rises, so body size is irrelevant and a genuinely stalled connection still hangs up on time. `c` is only dereferenced while `io->done` is clear, because `mg_mgr_poll` frees the connection in the same call that delivers `MG_EV_CLOSE`.

- **`/submit` stages to a temp file and renames only after the DB accepts.** Writing straight to the final name lets a client whose lease has lapsed clobber the file of whoever the sweep reissued the workunit to, and the 409 path then `unlink()`s it — destroying a submission that was already recorded. `db_submit` also carries `AND leased_to = ?` for the same reason.
- **A partial `/renew` on a block answers 409, not 200.** `db_block_submit` requires every member, so a block that lost one to the sweep can never be submitted; answering 200 leaves the card sieving a dead band for another quarter hour.
- **`db_block_lease` refuses when the client already holds an ordinary workunit.** It runs before `db_lease`, so without that check a slot whose block scan found no run — then retried after a run appeared — ends up holding two concurrent leases, the single workunit never heartbeated and eventually charged an attempt.
- **`init --class=gpu` is refused as well as `extend --class=gpu`.** Guarding one entry point leaves the hazard reachable from the other.
- **`finalize-nfs.sh`'s fallback glob must match `blk-*` as well as `wu-*`.** Dropping `blk-*` still finds every CPU file, so `total` stays non-zero and no error fires while the whole GPU contribution is missing.
- **The per-workunit arm of `db_verify_next_pending` needs `AND s.block_id IS NULL`.** A block submission stores its anchor in `workunit_id`, so without the guard it matches both `UNION ALL` arms, the per-workunit arm wins on equal `s.id`, and the block is verified against a base-width q-range — most relations count as `qviol`, only the anchor is requeued, and the other members sit in `submitted` forever because the sweep only touches `leased`.
- **`db_block_lease` returns 1, not -1, for its data-integrity refusals.** Both are deterministic functions of table content, so they recur on every retry; `handle_lease` maps -1 to HTTP 500 with no fallback, which would brick `/lease` for the whole GPU fleet rather than letting it take ordinary workunits. -1 is reserved for transient faults.
- **`--block-width-multiple` must stay at or below `VERIFY_SPOTCHECK_MAX_SCALE`** (verify.h). Past it the spot-check clips, and the widest submissions in the job get sampled at a *lower* density than an ordinary workunit — silently, on the work that matters most. `cmd_serve` warns, since the two constants live in different translation units and nothing else can notice.
- **The Makefile has no header dependency tracking.** Only `server.o: dashboard_html.h` is declared, so editing `db.h`, `protocol.h` or `verify.h` does not rebuild dependents. Force a clean rebuild after touching a header or you will link stale objects against a changed struct layout.
- **`run_child_cancelable` runs the siever as `sh -c "exec ..."`, and after a cancel waits for the whole process GROUP, not just the pid it forked.** Dash — `/bin/sh` on Debian and Ubuntu — does *not* exec a `-c` command in place, so the direct child is the shell and the siever is its child. cuda-sieve catches SIGTERM and answers it with a graceful drain (`bench_stop_hook_install`, cuda-sieve `bench/platform.c`) rather than dying, so on a second Ctrl-C the shell took the default action and died instantly, the client reaped it, reported *the shell's* 143 as the siever's exit status, and returned — never sending the SIGKILL that was supposed to follow 2s later. The card went on sieving and printed its whole band summary over the shell prompt after the client had exited. `exec` collapses the shell out of the picture so the status is the siever's; `wait_group_gone` covers anything the siever itself spawns.
- The siever's `-c` (in `sieve_run_local`, and `benchmark --qrange`) is a special-q interval **width**, not a count of special-q — the same unit as a workunit's `q_range`. The window `[f, f+c)` holds ~`c/ln(q)` prime special-q. A `-c` narrower than ~`ln(q)` contains no prime, so the siever does zero work and yields **0 relations in ~2s** (all `sieve:` counters 0) — a failure that mimics a broken job/siever/cache. This is why `benchmark --qrange` defaults to 65 (~3 special-q at q≈80M, ~1 min single-core), not a small number.
