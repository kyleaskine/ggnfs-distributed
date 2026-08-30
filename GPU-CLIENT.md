# GPU-CLIENT.md — running cuda-sieve as a second client engine

Plan of record for letting `../cuda-sieve`'s `bench` binary take workunits
from this coordinator alongside `gnfs-lasieve4*` clients, on one job, at the
same time. Check items off as they land; keep the rationale so we don't
re-litigate.

## Design principle

**Class is a property of the workunit. Engine is a property of the client.**

- *Class* (`cpu` / `gpu`) controls only **sizing**, and therefore lease safety.
- *Engine* (`lasieve4` / `cuda`) is what binary a given client runs.

Keeping them separate is what lets a GPU client safely fall back to CPU-class
workunits when the GPU band runs dry, without either side knowing about the
other. It also means the lease response ships *both* engines' arg strings and
the client picks — rather than the server deciding what a client can run.

## What is already compatible (verified, not assumed)

Ran this repo's verifier over real cuda-sieve output for this exact job:

```
$ ggnfs-verify --jobdb=AS276/job.db --no-qrange \
      ../cuda-sieve/work/as276/as276.rels.txt
as276.rels.txt: PASS parsed=134
summary: 1 file(s), 0 with failures; parse_fail=0 qviol=0 norm_fail=0
```

That is `verify_parse_file_full` — the **full** GMP norm check on every line
(`verify.h:161`), not the K-sample reservoir. So:

- Relation format matches: `a,b:rational:algebraic`, lowercase hex,
  multiplicity by repetition (`cuda-sieve/bench/td.cuh:141`), rational side
  first, special-q present in the list.
- The q-range check will pass. Every line of that sample carries a prime in
  `[30000000, 30001000)` on the algebraic side — 0 misses.
- The verifier's `b=0` rejection is safe: cuda-sieve emits no free relations.
- cuda-sieve reads `rlim`/`alim`/`lpb*`/`mfb*`/poly straight from the `.job`,
  so `/file/<sha>` distribution needs no change at all.

## Sizing: why 100x

| quantity | value | source |
|---|---|---|
| avg CPU sieve time per workunit | 1548 s | `AS276/job.db`, n=7649 |
| special-q per 1000-wide block | 30 | cuda-sieve finding 69 |
| relations per 1000-wide block | ~4564 | cuda-sieve finding 69 |
| GPU time per special-q (5070, auto cofactor) | ~123 ms | cuda-sieve finding 73 |

A current workunit is therefore **~3.7 s of GPU time** — order 400x a single
core (treat as ±2x: the CPU figure is rented boxes of mixed quality, the GPU
figure is one tuned card at half GGNFS's sieve area).

Fixed per-workunit cost on the GPU side is CUDA init + a ~230 MB `--fb1` load
+ lease RTT + submit — call it 10–20 s, more over a throttled link. For that
to be under 5% overhead you want 200–400 s of sieving.

**Start GPU bands at `--qrange=100000`.** ~3000 special-q, ~6 min/WU,
~456K relations, ~29 MB zstd — comfortably under the 500 MiB
`OUTPUT_MAX_BYTES`. Re-tune from measured overhead once Phase 4 lands.

## Phase 1 — Server: workunit classes

Testable with the existing CPU client, before any GPU code exists.

- [x] **1.1 Schema migration.** Add `class TEXT NOT NULL DEFAULT 'cpu'` to
      `workunits` in `SCHEMA_SQL`. `SCHEMA_SQL` uses `CREATE TABLE IF NOT
      EXISTS`, so existing `job.db` files will not pick it up: add
      `db_migrate()` called from `db_open` that checks `PRAGMA
      table_info(workunits)` and `ALTER TABLE … ADD COLUMN` if absent.
      Existing rows default to `cpu`, which is correct. Replace
      `idx_wu_state` with `idx_wu_state_class ON workunits(state, class,
      q_start)` to match the new lease predicate.
- [x] **1.2 `init` / `extend` gain `--class`** (default `cpu`).
      `db_workunit_insert` takes the class. `db_workunit_overlap` stays
      **global** — bands must not overlap regardless of class, or two clients
      sieve the same q. Workunit IDs keep the existing
      `wu-<jobhash>-<seq>` scheme, so `workunit_id_is_safe_for_job` and
      `derive_workunit_id` are untouched. Also write `meta.base_q_range` at
      `init` from `--qrange` (needed by 2.4).
- [x] **1.3 `/lease` class negotiation.** Lease request body gains
      `"class"`; absent means `cpu`, so deployed clients keep working
      unchanged. `db_lease` takes the wanted class. Fallback rule, hardcoded
      and documented: **`gpu` falls back to `cpu`; `cpu` never takes `gpu`.**
      A GPU on a small unit is merely inefficient; a CPU on a 100x unit needs
      ~43 h and gets poisoned. The existing client_id idempotency guard
      (`db.c:356`) is class-agnostic and stays as-is.

**Gate — passed.** Against a copy of `AS276/job.db` (430,001 rows): the
`class` column is added, all rows backfill to `cpu`, `idx_wu_state` is
replaced by `idx_wu_state_class`, and a second open is a silent no-op. On a
scratch job with a 3-workunit cpu band and a 2-workunit gpu band, all of:
gpu client gets gpu work; gpu client falls back to cpu once the gpu band is
dry; explicit cpu client gets cpu work; a client sending no `class` at all
behaves as cpu; an unknown class is 400; an empty pool is 204; the
already-holds-a-lease guard returns the *same* workunit rather than a second
one; and — with only gpu work available — both a cpu client and a legacy
client get 204 and the gpu rows stay untouched.

## Phase 2 — Server: heterogeneous-lease support

- [x] **2.1 `POST /renew`.** Bump `lease_expires = now + lease_seconds` when
      the workunit is leased to that client; 409 otherwise. Chosen over
      scaling `lease_seconds` by `q_range`: less code, and it reclaims work
      from a *dead* client fast instead of waiting out a long scaled lease.
      Also fixes the pre-existing case of a slow CPU box losing a normal
      workunit. (This is FUTURE.md's "worker heartbeats", promoted.)
- [x] **2.2 `gpu_args` in meta.** `init`/`extend` gain
      `--gpu-args="--logI 17 --J 16384"`. The lease response carries **both**
      `siever_args` and `gpu_args`; the client picks by its own `--engine`.
      `siever_args` is lasieve4 vocabulary and does not translate — per
      finding 69, `-J 16` is `2^17 x 2^15` while `--logI 17 --J 16384` is
      `2^17 x 2^14`, a different area, not a renaming. Editable on a live
      jobdir via the documented `sqlite3 UPDATE meta` route, same as
      `siever_args`.
- [x] **2.3 `/stats` q-width fields.** `sum(q_range)` grouped by state, plus
      per-class workunit counts, plus a last-1h q-width rate (join
      `submissions` to `workunits` on `received_at > now-3600`).
- [x] **2.4 Spot-check K scaling** (`verify.c`). A 100x workunit currently
      gets 50 samples over ~456K relations vs 50 over ~4564. Scale
      `K_eff = K * max(1, q_range / meta.base_q_range)`, capped at `32*K`.
      The reservoir needs K fixed before the pass, and the verifier already
      loads `q_range` for the range check, so this is a couple of lines.

**Gate — passed.** With `--lease-seconds=20 --sweep-seconds=5`: a client
heartbeating every 8 s held its workunit for 48 s (2.4 lease windows); once it
stopped, the sweep reclaimed the row within one window; a late heartbeat after
that reclaim got 409 rather than stealing the workunit back; and a heartbeat
from a different client id got 409. `gpu_args` and `siever_args` both arrive in
the lease response, and `extend --gpu-args` updates the job-wide value. On a
job with 3 cpu + 3 gpu workunits, `/stats` reported **67% by workunit count vs
34% by q-width** — the gap 5.1 exists to close. Spot-check scaling verified by
submitting the real cuda-sieve sample twice at `--spotcheck-k=2`: `k=2/134` at
`q_range == base_q_range`, `k=20/134` at 10x. That last run also put cuda-sieve
relations through the live coordinator end to end — lease, submit, parse,
q-range, and norm spot-check all PASS against a gpu-class workunit.

**Pulled forward from 4.2:** the client-side heartbeat. `/renew` with no caller
could not be verified, and the existing lasieve4 client benefits today — a slow
rented box no longer loses a workunit mid-sieve. It hangs off the cancellation
callback `sieve_run_local` already polls every ~100 ms, renewing at a third of
the lease window; a 409 stops the heartbeat but lets the sieve finish, since
the submit will 409 harmlessly if the work really was reassigned.

## Phase 3 — Client: the cuda engine, serial

Correctness first, no pipelining yet.

- [x] **3.1 `sieve_run_cuda()`** in `sieve_executor.c`:

      bench --pipeline --cofactor --poly JOB \
            --qrange QSTART:(QSTART + QRANGE - 1) \
            --sq-side {a->1, r->0} \
            --relations OUT --restart [--fb1 CACHE] [--device N] <gpu_args>

  - **The `-1` is the whole ballgame.** `--qrange MIN:MAX` is *inclusive*
    (`cuda-sieve/bench/fbgen.c:531`, pinned by
    `cuda-sieve/bench/sqgentest.c:140`), while workunits and `verify.c:311`
    are half-open. Get it wrong and any workunit with a prime on its top edge
    fails verification, requeues, and eventually poisons.
  - `--restart` unconditionally: we have no partial-submit concept, so an
    inherited `.part` is never useful.
  - Cancellation reuses the existing `wait_child_cancelable` SIGTERM/SIGKILL
    path unchanged — cuda-sieve stops cleanly at the next special-q, which is
    ~124 ms on this job. No `--stop-file` needed.
  - `--relations` stages to `OUT.part` and renames on band completion, so the
    existing "did `OUT` appear?" check at `client.c:1298` already means
    exactly "band completed" and needs no change.
- [x] **3.2 `--engine=lasieve4|cuda`** (default `lasieve4`), plus
      `--cuda-bench=PATH`, `--device=N`, and `--lease-class` (defaults to
      `gpu` under `--engine=cuda`). Skip the siever-basename mismatch warning
      at `client.c:1274` when engine is cuda.

**Gate — passed.** Tested with two stand-ins that reproduce each engine's real
q-range convention (`bench` inclusive, `lasieve4` half-open) emitting genuine
AS276 relations captured from a cuda-sieve run, so the coordinator's verifier
saw real input throughout.

1. **Off-by-one, with a negative control.** Band `[30000000, 30000037)` —
   the sample's two special-q are 30000023 (inside) and 30000037 (prime, and
   exactly on the excluded upper edge). The client emitted
   `--qrange 30000000:30000036`; 87 relations, verifier PASS, `qviol=0`. Feeding
   the same workunit what the naive `q_start + q_range` would have produced
   (`:30000037`) gives `qviol=47` and FAIL — so the test discriminates, and the
   client is on the right side of it.
2. **Mixed run.** One cuda client and one lasieve4 client against one
   coordinator, on adjacent bands of different classes: cpu got
   `[30000000,30000030)` → 87 relations, gpu got `[30000030,30000060)` → 47.
   Both PASS, no class cross-contamination, and 87+47 = 134 = the whole sample,
   each relation exactly once. Two engines with opposite range conventions tile
   a q-range without gap or overlap.
3. **Mid-band crash.** A `bench` that writes staging and exits 1: client logs
   `siever returned 1 (skipping submit)`, submits nothing, and releases the
   lease. The renamed `outfile` is the only completion marker, so a leftover
   `.part` can never be read as a finished band.

Also verified: the built command line is
`--pipeline --cofactor --poly <fetched job> --sq-side 1 --qrange A:B
--relations <wu>.dat --restart --device N <gpu_args>`, with `gpu_args` (not
`siever_args`) supplied to the cuda engine, and the lasieve4-only `.afb`
pre-generation and siever-name warning both skipped under `--engine=cuda`.

## Review pass after Phase 3

`/code-review high` over the whole diff raised six findings; all six were real
and are fixed. Recorded because most were about the seams between phases, not
the code each phase added.

- **`extend --gpu-args` did not reach a running `serve`.** `ctx.gpu_args` is
  snapshotted at startup, so new gpu-class workunits went live immediately
  while still advertising the old geometry — every cuda client would have
  sieved the new band with bench's defaults. `extend` now prints an explicit
  "restart `serve`" note. Also: `flag()` returns `""` for a bare `--gpu-args`
  (no `=`), which silently *cleared* the job's geometry; that typo is now
  rejected, while `--gpu-args=` still clears deliberately.
- **The lease heartbeat covered only the sieve, not the upload.** A gpu band's
  ~29 MB compressed upload over a throttled link can outlast the lease; the
  sweep requeues, `/submit` 409s, and the client treated that as success —
  unlinking a completed multi-minute band. Now renewed once immediately before
  submit, so the upload starts on a full lease window.
- **`lease_lost` was set but never read**, and conflated 409 with 404.
  `do_renew` now returns distinct codes: 409 means the workunit was reclaimed
  and reissued, so the client aborts the band immediately rather than sieving
  for hours into a guaranteed 409; 404 just means an old server with no
  `/renew`, where the lease is fine and sieving continues.
- **`do_renew` blocked the cancellation poll** for up to 10 s inside
  `wait_child_cancelable`'s ~100 ms loop, delaying a second Ctrl-C. Budget cut
  to 5 s with `abort_on_cancel`, and no renew is started once draining.
- **`benchmark` usage text claimed a cuda mode it does not have** — my own
  bug: a global string replace hit `bench_usage()` as well as `usage()`.
  Reverted; benchmark stays lasieve4-only until 5.2.
- **`/stats` added two full scans on the event-loop thread**, which the
  dashboard polls every 5 s alongside `/lease` and `/submit`. `idx_wu_lease`
  now carries `q_range` so the per-class rollup is a covering index scan
  (0.26 s cold → 0.02 s on 430K rows), and `idx_sub_received` turns the 1-hour
  query from a full submissions scan into an hour-wide range scan. The lease
  plan is unchanged.

Re-verified after the fixes: abort-on-reclaim works end to end (lease yanked
mid-band → client abandons it, submits nothing, then re-leases and completes it
cleanly, `attempt_count` still 0), and the mixed cpu+gpu run still tiles the
sample exactly once (87 + 47 = 134).

**Known, pre-existing, not fixed:** a band containing no prime special-q yields
an empty file, which the client treats as a failure and retries indefinitely
(it releases rather than fails, so nothing is poisoned). This is the `-c`
narrower than `ln(q)` trap already documented in `CLAUDE.md`; real GPU bands at
`--qrange=100000` cannot hit it.

## Phase 4 — Client: kill the idle time

- [x] **4.1 Factor-base cache.** `ensure_gpu_fb_cached()`, the direct
      analogue of `ensure_afb_cached()`: run
      `fbgen_gpu --poly JOB --lim <alim> --maxbits <logI> --out CACHE` once
      per (job sha, logI), stage through `.part` + rename, size-guard like
      `afb_size_ok()`, then pass `--fb1` on every run. New flag
      `--fbgen-gpu=PATH`; omitted means skip the cache and pay in-process FB
      generation per workunit. **This is a bigger lever than the pipelining
      below** — in-process FB generation per workunit dwarfs lease latency.
- [x] **4.2 Lease slots + async submit.** `db_lease`'s idempotency guard
      means one `client_id` = one lease, so a prefetching client that reuses
      its id gets the *same* workunit back. Use **N client_ids as lease
      slots** — `<base>-g0-s0`, `-s1` — with one GPU executor draining them.
      Zero server change; it is the trick `--workers=N` already uses.
  - `g_active` grows from `calloc(workers)` (`client.c:2017`) to
    `workers * prefetch` slots.
  - *Lease thread*: keeps slots full, fetches the `.job`, ensures the FB
    cache, heartbeats `/renew` on all held leases.
  - *Sieve loop*: pops a filled slot, runs `sieve_run_cuda`, hands the result
    to the submit queue, returns the slot.
  - *Submit thread*: drains the queue via the existing
    `submit_with_retries`, clears the active-lease entry, unlinks.
  - Shutdown: *draining* stops refilling, finishes the current sieve, drains
    the submit queue, releases unstarted slots. *Cancelling* kills the siever
    and releases all. Both reuse the existing phase machinery.

**Gate — passed.**

*Factor-base cache.* `fbgen_gpu` is invoked once per (job sha, logI) with
`--lim` taken from the `.job`'s `alim` and `--maxbits` from the server's
`gpu_args` logI (defaulting to cuda-sieve's own 15 when unset — `--maxbits`
must match the width actually sieved at). Built exactly once across three
successive workunits, and exactly once across three concurrent workers.
Staged through a pid-unique path and renamed, so a partial file is never
visible; keying the filename on (sha, logI) is what makes a content probe
unnecessary, since a cache for another poly or width cannot be found under
this name.

*Pipeline.* Measured against a latency-injecting proxy (1.5 s per connection,
standing in for the throttled links in this project's notes) with 3 s bands —
deliberately the worst ratio, where per-workunit overhead rivals the band
itself:

| prefetch | card busy | avg gap between bands |
|---:|---:|---:|
| 1 (serial) | 44.9% | 4.10 s |
| 2 (default) | 78.8% | 0.77 s |
| 3 | 96.8% | 0.10 s |

Correctness alongside it: no workunit was ever issued to two slots, no slot
ever held two leases, and slot client_ids come through the server distinctly
(`<base>-w0-s0`, `-s1`, ...). A clean two-band run gave 134/134 relations,
each exactly once, with the gpu client's spare slot correctly falling back to
the cpu-class band once the gpu band drained. Drain: with three leases held,
the first Ctrl-C finished and submitted the in-flight work, released the
prefetched-but-unstarted lease rather than making it wait out expiry, and
exited with zero leases dangling.

Real GPU bands at `--qrange=100000` run ~6 min, so overhead is proportionally
far smaller than in this test and `--prefetch=2` should be ample; raise it if
per-workunit overhead ever approaches band time.

**Refactor note.** `run_one_iteration` was split into `stage_acquire` /
`stage_sieve` / `stage_submit` so the serial and pipelined workers share one
implementation rather than two copies drifting apart. The serial path calls
them back to back and was regression-tested unchanged before the pipeline was
built on top.

**Pre-existing, unchanged:** the server never sends 410, so a client with no
work idles on `--idle-backoff` forever rather than exiting; both the serial and
pipelined workers behave identically here.

## Review pass after Phase 4 (xhigh) — and the first real-hardware run

Running the real `bench` on an RTX 5070 found what the stand-ins could not:
**`VERIFY_MAX_PRIMES_PER_SIDE` was 32 and real cuda-sieve output reaches 35.**
The cap counts primes with multiplicity — a relation lists a prime once per
division — and a 224-bit algebraic norm carrying a high power of a small prime
blows past it (the first offending line had thirteen factors of 2). bench's own
first-q validation prints "most factors 40 of 64". gnfs-lasieve4 never came
close, the C208 corpus peaking at 13, which is why it sat undiscovered; no
existing relation was ever affected. Raised to 64, matching cuda-sieve's
`TD_FMAX`, which is a hard cap on its side. This was the worst possible failure
mode: one bad line fails the whole file, so every GPU workunit would have
failed, requeued, and poisoned while looking like a cuda-sieve bug.

An `xhigh` review then raised 15 findings, all real, all fixed:

**Lease correctness (the expensive class).**
- Prefetched slots held real leases that nothing renewed — only the slot being
  sieved was heartbeated. A queued slot's lease would lapse, the sweep would
  reissue the workunit, and we would sieve a band we no longer owned. Added a
  pipeline heartbeat thread covering every held lease; slots reclaimed while
  queued are now dropped before the card ever starts them.
- The heartbeat was switched off during drain — precisely the phase where the
  client is finishing a long band it intends to submit. Now it keeps
  heartbeating; the second Ctrl-C stays responsive because `do_renew` runs on a
  5 s budget with `abort_on_cancel`, not because we stop renewing.
- `should_cancel_siever` tested `next_renew_ms == 0` before `lease_lost`, and
  the lease-lost branch zeroes `next_renew_ms` — so the abort could never fire.
  Masked only because `wait_child_cancelable` stops polling once cancelling.
- `stage_submit` **deleted** a fully sieved band when the pre-submit renew
  returned 409. Those relations are valid; every neighbouring path keeps its
  file "for inspection". Now kept.
- `release_active_leases` still clamped to `CLIENT_MAX_WORKERS`, but
  `g_active_count` is now `workers * prefetch` — leases above row 255 were
  never released.

**Spot-check scaling never engaged in the real deployment.** `meta.base_q_range`
was written only by `init`, but the documented GPU rollout is `extend
--class=gpu` onto a campaign whose meta predates all of this. And `init
--class=gpu` would have stored the *GPU* width as the baseline. Replaced with a
value derived from the rows: the **mode** of `q_range`. Verified on a copy of
the real 430,005-row jobdir — baseline 1000, correctly ignoring both the
100000-wide GPU band and stray `q_range=1` rows a minimum would have latched
onto.

**Robustness.**
- No idle backoff after a sieve failure in the pipeline (the serial worker has
  one): a wrong `--cuda-bench` exits 127 in ~150 ms and became a
  lease-and-abandon storm. Measured after the fix: 5 leases in 25 s with the
  available pool intact, against several per second before.
- `pthread_create` returns were ignored, then the possibly-uninitialized ids
  joined.
- `sqlite3_busy_timeout` was applied *after* the migration DDL, so the 430K-row
  `ALTER TABLE` / `CREATE INDEX` had a zero-length retry window against a live
  `serve`.
- The factor-base build held an un-heartbeated lease across a multi-minute
  `fbgen_gpu` run; it now heartbeats like a sieve does.
- The FB cache short-circuited on a process global before computing `logI`, so
  a client running across a `gpu_args` change kept feeding bench an `--fb1`
  built at the old `maxbits`.
- The "server has no /renew" disable was stack-local and re-probed every
  workunit; latched process-wide.
- `q_verified_1h` was keyed on submission time, not verification — renamed
  `q_passed_1h` and documented, since there is no verified-at column to key on
  (`completed_at` is set at submit).
- A per-class cJSON object leaked when the array allocation failed.

## Phase 5 — Ops and polish

- [x] **5.1 Dashboard.** Progress and ETA are `done / w.total` *workunits*
      (`dashboard.html:235`) — meaningless with two sizes. Switch to q-width
      from 2.3, and add an engine/class column to the client table.
- [x] **5.2 `benchmark --engine=cuda`.** Same fixed-work screening idea for
      rented GPU boxes.
- [x] **5.3 GPU bootstrap script** (`cuda-client.sh`), plus a check that
      `pull-rels.sh` / `move-rels.sh` / `finalize-nfs.sh` are indifferent to
      the source. They should be — same `rels/` naming, and relations verify
      identically.
- [x] **5.4 Docs.** Fold the class/engine split, the inclusive-`--qrange`
      trap, and the sizing rationale into `CLAUDE.md`.

**Gate — passed.**

- *Dashboard.* Rendered headless against a job of 400 cpu bands (1000 wide, all
  done) plus 4 gpu bands (100000 wide, none done): **count-based would read
  99.0% "nearly finished"; q-width reads the true 50.0%.** ETA is q-width
  remaining over `q_passed_1h`. Bar segments, per-class breakdown and a client
  `class` column all render, and `clientGroupId` now strips the `-sK` slot
  suffix so a prefetching GPU box is one row rather than N.
- *GPU benchmark.* Real card: 5251 relations in 27.9 s at q=32001000 — exactly
  matching what the live sieving run produced for that same band, which is the
  check that caught `bench_fetch_params` never copying `gpu_args` (it had been
  running at bench's default logI 15 while passing an `--fb1` built for
  maxbits 17, understating the card by ~2x and mismatching the factor base).
- *Downstream.* `finalize-nfs.sh` assembled `nfs.dat` from four GPU-produced
  submissions unchanged — 21,781 relations plus its `N:` header. The scripts
  glob `wu-*.dat*` and carry no engine or band-size assumptions.
- *Bootstrap.* `cuda-client.sh` is shellcheck-clean, and both generated-script
  variants were verified: with and without `fbgen_gpu`, arguments and
  pass-through `"$@"` survive. The first draft spliced an optionally-empty line
  into a backslash-continued command, which silently truncated every argument
  after it on exactly the boxes that lack `fbgen_gpu`; it builds a bash array
  instead.

## Second review pass (xhigh) after Phase 5

15 findings, all real, all fixed. The theme was the GPU benchmark and the new
bootstrap script — the code added last and exercised least.

**The benchmark did not behave like a screening tool.** It passed a NULL cancel
context, so `--max-seconds` was ignored entirely and the first Ctrl-C did
nothing — on a wedged card, the exact case the tool exists for, it hung. It now
uses the same `bench_should_cancel` the CPU phases do. It also printed the CPU
default `--qrange` in its header while sieving the cuda default, printed an
empty `--siever` and a meaningless worker count, and fired the siever-name
mismatch warning on every GPU run. And when the server publishes no `gpu_args`
it measured at bench's own default geometry and said nothing — the same
corruption the Phase 5 gate caught, arriving by a different route; it warns
loudly now.

**The bootstrap script had three ways to produce a broken worker.** It skipped
`make client` when a binary existed, so a box that had run an older bootstrap
kept a client that rejects its own generated command line (the guard is gone;
make is incremental anyway). It cleared `FBGEN` only when make returned
non-zero, so a target that exits 0 without producing the binary left
`--fbgen-gpu` pointing at nothing — now `[ -x ]` decides. And it spliced values
into the generated arrays unquoted, so a pasted token with a stray space or a
path with a space broke the runner; every element is quoted now. Plus `|| true`
on the optional benchmark, since a below-threshold box is a result rather than
a failed bootstrap.

**Migration race.** `db_migrate`'s check-then-ALTER was not transactional: two
processes opening the same jobdir during an upgrade could both see a column
missing, and the loser's ALTER fails `db_open` with "duplicate column name".
`sqlite3_busy_timeout` does nothing for a logical conflict, so the CLAUDE.md
line added in the previous commit was wrong about this. Now wrapped in
`BEGIN IMMEDIATE`, verified with six concurrent migrations of a copy of the
real 430K-row jobdir: zero errors, schema correct.

**Smaller.** `/lease` did two write transactions per poll and the idle poll is
the highest-frequency request in the system — folded into one upsert. The
per-class legend interpolated a server-derived string into `innerHTML` without
escaping, the only one on the page that did. The class breakdown counted
verified-only while the headline counted submitted+verified, so the two
contradicted each other during any verifier backlog. `q_total`/`q_verified`
were scalar duplicates of `q.total`/`q.verified`. The count-based dashboard
fallback was unreachable — the page is embedded in the same binary that serves
`/stats` — and doubled the render logic. And the generated worker scripts were
not gitignored, which would have made every bootstrapped box's
`git pull --ff-only` abort on its next re-run.

**Noted, pre-existing, not fixed:** two concurrent `extend` runs can pick the
same sequence number (`db_workunit_extent` reads it, then inserts) and one
fails on the primary key. Surfaced by the concurrency test above. It fails
loudly and corrupts nothing, and concurrent `extend` is not a normal
operation.

## Expectations to set

cuda-sieve finding 69 measured **99.97% recall and 1.6% genuinely new**
relations against this job's GGNFS corpus. Mixed-source relations are fine for
filtering, but rel/s between engines is not apples-to-apples on the
dashboard — the yield-per-q conventions differ.

## Deferred

- **Partial submits.** cuda-sieve resumes from `.part.ckpt`, so an
  interrupted GPU workunit need not be re-sieved. The server has no
  partial-submit concept, so v1 just re-sieves.
- **Client capability advertising** (see `FUTURE.md`). Class negotiation in
  1.3 is a narrow special case; the general form would let heterogeneous
  fleets self-target.
