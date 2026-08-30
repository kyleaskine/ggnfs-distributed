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

## Sizing: why wider bands (original estimate; corrected below)

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

**Superseded — see "Per-band overhead: measured" below.** This section's
reasoning is sound but its inputs were AS276's, where a 1000-wide band is
~3.7 s of GPU time. Measured startup is 5.3 s, not the 10-20 s guessed here,
and on the production job a 1000-wide band is 19.4 s rather than 3.7 s. The
answer that falls out is **`--qrange=10000` to `20000`**, not 100000.

Do not size a band from a core-multiplier. Time one band on the card.

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

## Field notes from the first production run

Pointing the GPU at the live coordinator surfaced two things no amount of
local testing would have:

- **The distributed `.job` carried a zero-width space** (U+200B after
  `alambda: 3.6`). lasieve4 had ignored it for weeks across 170 clients;
  cuda-sieve refused the file outright, so the GPU could not sieve a single
  band. Fixed in two places: `init` now rejects such a file before its SHA
  becomes the job's identity, and for campaigns already in that state the
  client sieves from a sanitized copy.
- **The live job is a different shape than the one all the testing used** —
  rational-side special-q (`--sq-side 0`), not algebraic. That path had never
  been exercised end to end. It verified on the first try.

Measured on that job (RTX 5070 vs a 9800X3D core): 4,266 relations in 23.4 s
against ~3,530 in ~735 s — **~38x a core, ~3.2x the whole 12-worker box**, and
21% more relations per band because GGNFS trims its factor-base bound to the
special-q while cuda-sieve does not. That gap narrows as q climbs toward
`alim`.

### Per-band overhead: measured, and how it was first measured wrong

An early reading compared 24.6 s of wall clock per band against the client's
own reported 23.7 s of "sieving" and concluded 96% card utilisation. **That
was wrong.** `sieve_seconds` (`client.c`) brackets the whole `sieve_run_cuda`
call — fork, CUDA context init, factor-base load, sieve, teardown. Startup is
*inside* the number, so the comparison could only ever see the lease/submit
round trip, which `--prefetch` already hides. It was structurally blind to the
cost it was trying to measure.

The honest measurement is an A/B on identical q coverage, prod job at q=93M,
RTX 5070, `--logI 16 --J 32768`, warm `--fb1`:

```
A  10 x 1000-wide : 247.36 s   48397 rels
B   1 x 10000-wide: 199.54 s   48395 rels
```

`A - B = (n-1) * startup` gives **5.31 s of fixed startup per `bench`
invocation**, and 19.42 s of sieving per 1000-wide band. Cross-checked
directly: a band containing a single special-q wall-clocks 5.40 s, of which
0.28 s is sieve. So a 1000-wide band on a GPU is **21.5% overhead, not 4%**.

(The 2-relation difference between A and B is ECM cofactorisation
nondeterminism, not a coverage gap.)

Since startup is a fixed cost, the return on wider bands dies off fast:

| `qrange` | band time | efficiency |
|---|---|---|
| 1000 | 0.41 min | 78.5% |
| 2000 | 0.74 min | 88.0% |
| 5000 | 1.71 min | 94.8% |
| **10000** | **3.33 min** | **97.3%** |
| **20000** | **6.56 min** | **98.7%** |
| 50000 | 16.27 min | 99.5% |
| 100000 | 32.46 min | 99.7% |

**10,000-20,000 is the sweet spot.** 100,000 buys one further point while
making each band 32 minutes — and with no partial-submit concept, a reclaimed
or interrupted band throws all of that away. Recovering the 21.5% is worth
~194 -> ~243 rel/s on a 5070.

Size GPU bands from a measured band time, not from a multiplier: the earlier
`--qrange=100000` figure came from AS276, where a 1000-wide band is ~3.7 s of
GPU time rather than this job's 19.4 s.

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

---

# v2 plan — GPU blocks

**Status: proposed, not built.** v1 (everything above) ships GPU sieving via
`class`-sized workunits. This supersedes the *sizing* half of it and leaves the
engine half untouched.

## Why

v1 makes the GPU band a workunit: one wide row a card leases whole. That
conflates the unit of **work** with the unit of **assignment**, and every
problem v1 has is a symptom of it:

- A wide row is `gpu` class, and `cpu` never falls back to `gpu`, so a band
  left behind when a card disconnects is unreachable by the entire CPU fleet.
- `recarve` can widen rows but only *pristine* ones, because merging destroys
  the per-row `attempt_count` that `--max-attempts` depends on.
- Therefore every lease expiry permanently demotes its q-range to
  un-mergeable. On rented boxes — which are flaky, see the region-mislabelling
  and GFW notes — that ratchet runs one way and never resets.
- A failed wide band cannot be split back at all: `submissions.workunit_id`
  is a live `REFERENCES workunits(id)` with `PRAGMA foreign_keys = ON`, so
  deleting it is refused (verified).

A block fixes all four by not merging anything. **Workunits stay canonical at
base width forever; a block is a lease held over N of them.** Nothing is
deleted, nothing is resized, `attempt_count` stays attached to the 1000-wide
range it describes. Class stops being a property of the work — which it never
really was — and becomes a property of the lease.

Deciding factor: the fleet is meant to grow to many GPUs. At that scale the
v1 operational burden (top up gpu bands by hand, unstrand them by hand, per
card, across flaky hosts) does not hold up, and the measured 21.5% per-band
overhead is worth multiple whole cards.

## What v1 code survives

~85%. Discarded: `recarve` (~450 lines), the `class` lease-fallback chain
(~60), and nothing else. `workunits.class` is `NOT NULL DEFAULT 'cpu'` and can
simply go unused rather than be migrated away.

Everything that carries the real value is unaffected and mostly *more* needed:
the `--engine=cuda` client, `sieve_run_cuda` and its inclusive-`--qrange`
off-by-one, the `--fb1` cache, `derive_gpu_args` and the geometry table,
the `.job` sanitizer, `VERIFY_MAX_PRIMES_PER_SIDE 64`, the cuda benchmark
mode, q-width stats, `db_workunit_next_seq`. **`POST /renew`** and
**`--prefetch`** are load-bearing here in a way they only half were in v1.

`spotcheck_k_for()` also stops being at risk of going vestigial: a block
submission carries the block's full width, so sample-density scaling applies
exactly as it does to a v1 wide band.

## The pristine rule is not needed

Worth stating plainly because it is the counter-intuitive part. `recarve`
requires `attempt_count = 0` because **merging** has to decide what the merged
row's count means. A block merges nothing — each member keeps its own count,
untouched, for its own q-range. So block membership needs only:

> `available`, contiguous, same `side`.

Members may have any `attempt_count`. That is what removes the ratchet: an
expiry increments each member individually, they go back on the pile at base
width, and they remain eligible for a future block.

## Schema

Three placements, not one block of SQL — the split is load-bearing (db.c:73-75:
anything naming a migrated column must live in `SCHEMA_SQL_POST`, not
`SCHEMA_SQL`). `db_open` runs the DDL on **every** connection and `cmd_serve`
opens a second one inside the verifier, so a missing `IF NOT EXISTS` fails the
verifier's open and the process runs with verification silently disabled.

`SCHEMA_SQL`:

```sql
CREATE TABLE IF NOT EXISTS gpu_blocks (
  id            TEXT PRIMARY KEY,      -- blk-<jobhash>-<seq>, internal only
  anchor_wu_id  TEXT NOT NULL REFERENCES workunits(id),
  client_id     TEXT    NOT NULL,
  q_start       INTEGER NOT NULL,
  q_end         INTEGER NOT NULL,      -- exclusive
  member_count  INTEGER NOT NULL,
  side          TEXT    NOT NULL,
  state         TEXT    NOT NULL,      -- leased|submitted|verified|failed|expired|released
  leased_at     INTEGER NOT NULL,
  lease_expires INTEGER NOT NULL,
  created_at    INTEGER NOT NULL
);
```

`db_migrate`, each behind a `column_exists` check:

```sql
ALTER TABLE submissions ADD COLUMN block_id     TEXT NULL;
ALTER TABLE submissions ADD COLUMN member_count INTEGER NOT NULL DEFAULT 1;
ALTER TABLE submissions ADD COLUMN q_width      INTEGER NOT NULL DEFAULT 0;
```

`SCHEMA_SQL_POST` (names migrated columns):

```sql
CREATE INDEX IF NOT EXISTS idx_wu_lease_v2 ON workunits(state, q_start, q_range);
```

`idx_wu_lease_v2` is **required in phase 1, not optional**. The existing
`idx_wu_lease(state, class, q_start, q_range)` only orders usefully when
`class` is constrained. Measured with EXPLAIN QUERY PLAN:

```
current  (state=? AND class=?)        SEARCH USING idx_wu_lease
proposed (state=?)                       SEARCH USING idx_wu_lease (state=?)
                                         USE TEMP B-TREE FOR ORDER BY   <-- sorts everything
with idx_wu_lease_v2                  SEARCH USING idx_wu_lease_v2 (state=?)
```

Without it every block lease sorts all ~356,000 available rows on the
mongoose event-loop thread, alongside `/submit` and `/stats`. The same applies
to the ordinary CPU lease the moment the `class` predicate goes away.

`submissions.q_width` and `.member_count` exist so per-client stats and
`q_passed_1h` need no join to `gpu_blocks` at all — see Stats below.

## Addressing: blocks are anchored, not new ids

**A block is addressed on the wire by its anchor workunit id.** Nothing in the
protocol, the id guard, or the relation filenames changes shape.

This is not cosmetic. `workunit_id_is_safe_for_job` (server.c:983) hard-codes
`"wu-"` + job hash + digits and gates `/submit`, `/renew` *and* `/release`, and
it doubles as the path-safety check for `rels/<id>.dat.zst`. Worse,
`finalize-nfs.sh:74-77` globs `rels/wu-*.dat*` and `move-rels.sh:90-95` strips
the extension to look the id up in `workunits`. A `blk-` filename is invisible
to both: `total` stays > 0 because CPU files exist, so **no error fires** and a
campaign that ran 40% of its q-width on GPUs assembles an `nfs.dat` missing 40%
of its relations — discovered weeks later as an unexplained filtering failure.

`gpu_blocks.id` stays internal. `anchor_wu_id` is the external handle.

## Block membership

> `available`, contiguous, same `side`, `attempt_count < block_attempt_ceiling`.

There is no `block_id` predicate: a member of a live block is `state='leased'`,
so `available` already excludes it. See *Whether `workunits.block_id` should
exist at all* below.

No pristine rule — members keep their own counts, untouched. The ceiling
(default 2, well below `--max-attempts` = 5) is not a ratchet: it degrades a
repeatedly-failing region to *individual* leases, which still get worked, and
caps the blast radius described under Failure semantics.

**Size by accumulated q-width, not member count.** Stop when
`q_end - q_start` reaches the target. Member count assumes uniform base
width, and `extend --qrange=N` with a different width is documented normal
operation (db.c:412-413) — a 10-member block that swallows one 100,000-wide
row is ~35 minutes of card time under a lease window sized for 200 s.

**The target is derived, not a constant:**

> `target_q_width = block_width_multiple * db_workunit_base_q_range(db)`,
> `block_width_multiple` default **50**.

`db_workunit_base_q_range` (db.c:409) already exists — it returns the *mode*
q_range, deliberately not the minimum, and `verify.c:1035` re-reads it once per
drain pass for exactly this class of reason. Reuse it; do not add a second
notion of "the job's band width".

The multiple is the right thing to fix because **base q_range is already
normalised to wall clock.** An operator picks `--qrange` so one workunit is a
sane slice of a *core* — tens of minutes — and picks it larger for a bigger
job because each special-q costs more there. A GPU is a roughly job-independent
multiple of a core (~42x measured here), so `50 x base` holds the GPU band near
a constant wall-clock target across jobs, while a hardcoded 50,000 would
silently become a 7-minute band on a job carved at 100 and an hours-long one on
a job carved at 20,000. On this campaign (base 1,000) it gives 50,000 — 99.4%
efficiency, ~15.6 min of card time.

Consequences to build in:

- **Read it at lease time, not once at startup.** `extend` can change the mode
  under a running `serve`, which is the same reason the verifier re-reads.
  Cache per drain of the lease path if the aggregate ever shows up in a
  profile; it is one indexed aggregate.
- **Clamp and override.** `--block-width-multiple=N` on `serve`, and a hard
  ceiling on the resulting q-width. A job initialised at `--qrange=100000`
  would otherwise derive 5,000,000-wide blocks, which is an unbounded lease.
- **Empty table returns 0** (db.c:429), and a target of 0 must fall back to a
  sane constant rather than producing a zero-width block.
- **`min_members` becomes a width floor too** — express the minimum run as a
  fraction of target (e.g. 1/4) rather than a count, for the same
  non-uniformity reason the target is a width.

**Minimum run.** A contiguous prefix of length 1 means the card pays 5.31 s
startup against 19.42 s of work — 21.5%, the exact number this rewrite exists
to remove. Once CPU leases and partial resolutions punch holes, that becomes
the common case, not the rare one. The lease needs a `min_members` floor and a
search past short runs. **`recarve_plan`'s maximal-contiguous-same-side-run
scanner (db.c:579-586) is exactly this algorithm — extract it before deleting
the rest of `recarve`.**

## Lease

`db_lease_block(db, client_id, target_q_width, min_members, max_members, ...)`,
one `BEGIN IMMEDIATE`: scan, take the run, insert the `gpu_blocks` row, mark
members `leased` with the block's `lease_expires` and `leased_to`.

`db_lease`'s existing "one live lease per client_id" guard must become
block-aware or a reconnecting client gets one member back instead of its block.

**`max_members` needs a hard server-side clamp** (`--max-block-members`) and a
`>= 1` floor. A client-supplied `members` flowing into `LIMIT ?` lets one
request lease the entire remaining campaign; `members: 0` inserts a block row
with `member_count = 0` and undefined `q_start`/`q_end`.

**Scan direction is per lease kind, not global.** `ctx.lease_desc` is read once
from `meta.lease_order` into a single job-wide flag (server.c:1583), so setting
it to `desc` flips the *CPU* fleet too and the "start at opposite ends"
property never materialises. Block leases descend; workunit leases keep
following `meta.lease_order`. Note that under DESC the contiguous prefix runs
downward, so the block's `q_start` is the **last** row scanned — an
implementer following an ascending sketch literally will invert
`q_start`/`q_end`.

## Renew / release / sweep

`/renew` on the anchor id must carry the same guards `db_renew_lease` has today
(db.c:1019-1023) — `state='leased' AND leased_to=? AND lease_expires >= now` —
applied to every member, and report the row count so a partial renew is
detectable. Dropping them lets a late heartbeat stamp `lease_expires` onto rows
the sweep already requeued, or silently extend a *different* client's lease on
a re-issued member.

**Sweep ordering replaces a predicate.** The existing per-workunit sweep
(db.c:781-790) has no block awareness and would otherwise fire on members
alongside the block sweep, double-incrementing `attempt_count`. Rather than
adding an exclusion predicate, **run the block sweep first in the same timer
callback**: it sets expired members to `available`, and the row sweep's
existing `state = 'leased'` predicate then excludes them for free. Both run
sequentially on the mongoose thread, so there is no race, and there is no new
predicate to forget.

### Whether `workunits.block_id` should exist at all

It would do three jobs. **All three dissolve.**

**1. Membership filter (`state='available' AND block_id IS NULL`) — redundant.**
A member of a live block is `state='leased'`, so `state='available'` already
excludes it. The only way to see an `available` row with a non-NULL `block_id`
is to have forgotten to clear it. The filter exists solely to make a bug in its
own lifecycle safe, which is circular.

**2. Finding a block's members — derivable, from data `gpu_blocks` already
stores.** Blocks are contiguous half-open q-ranges, so
`q_start >= b.q_start AND q_start < b.q_end` is exact and rides
`idx_wu_lease_v2` directly. It needs one invariant: *query a block's members
only while the block is non-terminal, and resolve in a single transaction.*
That holds by construction — while a block is `leased` or `submitted` its
members are `leased`/`submitted`, so no second block can form over them
(membership requires `available`). Only after it resolves can a new block
overlap the same q, and by then nothing queries the old one.

**3. Keeping the existing row sweep off block members — better fixed by
ordering.** Run the block sweep first in the same timer callback. It sets
expired members to `available` and increments them; the row sweep's
`state='leased'` predicate then excludes them for free. Both run sequentially
on the mongoose thread, so there is no race. This removes the
`AND block_id IS NULL` requirement rather than satisfying it.

**Performance is a wash — measured, and my earlier "cheap `IS NULL` filter"
claim had the sign backwards.** EQP gives the same plan either way
(`SEARCH workunits USING INDEX idx_wu_lease_v2 (state=?)`), and 200 iterations
of the descending LIMIT-200 scan are below timer resolution in both. `block_id`
is not *in* `idx_wu_lease_v2`, so testing it costs a row fetch per candidate.
Tiny, but a cost, not a benefit.

**The decisive asymmetry is in the failure modes, which are not comparable:**

- *Keep it and miss one clear:* rows become permanently unblockable, with no
  error and no log line. The GPU fleet's eligible pool quietly shrinks. That is
  v1's ratchet relocated — the exact thing v2 exists to remove. Two of the
  fifteen review findings were instances of this, which is evidence about how
  easy the path is to miss.
- *Derive it and get the range logic wrong:* the wrong rows get touched loudly
  and immediately — wrong `attempt_count`, wrong state, visible in the verify
  log on the very first block.

**Decided: derive.** No `workunits.block_id` column, no `idx_wu_block`, and no
"cleared on every terminal transition" requirement. The price is that
`gpu_blocks.state` becomes the single source of truth for "is this block live",
so a late submit from an expired block must 409 on `b.state != 'leased'` rather
than on member state — one guard in one place replacing five clears across five
paths.

**`submissions.block_id` is a different thing and stays.** It is set once at
insert and never mutated: it records which block produced a file, and the
verifier's `UNION ALL` needs `AND s.block_id IS NULL` to keep block submissions
off the per-workunit arm. It has no lifecycle, so none of the above applies to
it. Do not conflate the two while implementing.

**This is the reversible direction.** Adding `workunits.block_id` later is a
one-line additive `ALTER TABLE` behind a `column_exists` check — the pattern
already used twice in `db_migrate` (db.c:159, db.c:172) — and the derivation
stays valid while the column backfills, so the two can run side by side and cut
over. Removing it once lease, renew, sweep and resolve all read it would be a
migration against a live 430K-row DB. Start without; add it only if the
derivation measurably hurts.

## Failure semantics

| event | evidence points at | action |
|---|---|---|
| verify failure (parse/norm) | the **data** | all members `available`, `attempt_count + 1` |
| member starved of relations | that **sub-range** | that member only, `attempt_count + 1` |
| lease expiry | the **client** | all members `available`, `attempt_count + 1` |

**Failure state lives on the workunit, never on the block.** `gpu_blocks` has
a `state` for lease bookkeeping but no `attempt_count` and no `poisoned`. Every
strike lands on the base-width row it is actually about, which is the whole
reason members keep their own counts.

Expiry still increments — it is the safety net against work that reliably kills
its client — but the `block_attempt_ceiling` is what stops the blast radius.
Without it, a GPU host that dies mid-band re-leases nearly the same members each
cycle (descending scan returns them to the top of the range) and five cycles
later flips 20 contiguous workunits to `poisoned` in one shot, in about five
lease windows, with nothing actually wrong with that q-range. Under v1 the same
host poisoned one row per cycle, slowly enough to notice.

The ceiling breaks the re-lease loop rather than the increment. With
`block_attempt_ceiling = 2` and `--max-attempts = 5`:

| cycle | member `attempt_count` | block-eligible? |
|---|---|---|
| 1 | 0 -> 1 | yes |
| 2 | 1 -> 2 | yes |
| 3 | 2 | **no** — falls back to individual leases at base width |

**The invariant:** at most `block_attempt_ceiling` of a workunit's strikes can
come from block-scale evidence. The remaining `max_attempts - ceiling` must be
earned one 1,000-wide lease at a time. A row therefore never reaches `poisoned`
without three independent failures that were specifically about *that* range —
so a flaky host can consume budget but can never, by itself, poison anything.
Keeping `ceiling` strictly below `max_attempts` is what makes that hold; they
should be validated against each other at startup.

Residual, stated honestly: `attempt_count` never decreases, so a range that
takes two block-scale failures is permanently out of GPU eligibility. That is a
ratchet — but a bounded, per-row one whose failure mode is "the CPU fleet sieves
it at base width", not v1's ratchet whose failure mode was "unreachable by every
client in the fleet". It only starts to bite the GPU fleet's throughput when
roughly half of all block leases are failing, at which point degrading to
CPU-width assignment is the correct response anyway.

The middle row needs care, and testing changed how it works. Relations are
attributed to a member by the **smallest** sieved-side prime that falls in the
block's range — the file does not record which prime was the special-q — and a
relation occasionally carries a second in-range prime below its own. So the
obvious rule, "charge any member with zero relations", **does not fire**.

Measured, on a real 6-member block of 20,388 lasieve4 relations with one
member's output deliberately omitted:

| member | relations attributed |
|---|---|
| 60001000 | 3287 |
| 60002000 | 3383 |
| **60003000 (omitted)** | **1** |
| 60004000 | 3352 |
| 60005000 | 3684 |
| 60006000 | 3672 |

Two of 17,379 relations carried a second in-range rational prime, and one of
them landed in the empty member. A `== 0` test therefore verified a q-range
that had never been sieved — a silent loss of coverage, the worst failure this
whole design has.

The rule is a **relative threshold**: charge a member whose count is below
1/20th of the median. That separates 1 from 3383 by two orders of magnitude
while staying nowhere near a genuinely sieved member. Only applied when the
block as a whole parsed and passed, so the signal is "this sub-range produced
nothing" and not "the file was broken" — and requeueing exactly the starved
members is what keeps partial resolution safe, since the stored file holds no
relations for those ranges and re-sieving cannot duplicate assembled work.

## Verify

One file per block spanning `[b.q_start, b.q_end)`; parse pass and norm
spot-check unchanged, K scaled by block width. Relations bucket to a member by
the smallest sieved-side prime in the block range — not exact, but a block is
~10,000 wide against primes spread over `[0, lim]`, and it is only ever a
`>= 1` test.

`db_verify_next_pending` becomes a `UNION ALL`, and **the existing arm needs
`AND s.block_id IS NULL`**. Without it the anchor makes every block submission
match both arms; `UNION ALL` preserves arm order on equal `s.id`, so the
per-workunit arm wins and the block is verified against its anchor's 1000-wide
range. ~90% of relations count as `qviol`, `db_verify_fail` requeues only the
anchor, and the remaining members sit in `submitted` forever — the sweep only
touches `state='leased'`. Also: SQLite rejects a qualified `s.id` in a compound
SELECT's ORDER BY; use `ORDER BY 1`.

**Partial resolution creates overlapping relation files by design**, and
`finalize-nfs.sh` cats every `rels/wu-*.dat*` with no dedup and no
verify-status filter. msieve dedups, so it is survivable, but under v2 this is
a routine path rather than an exception. Fix `finalize-nfs.sh` to select files
from `submissions WHERE verify_status='passed'` instead of globbing — which
also fixes the pre-existing case of failed submissions' files being assembled.

## Stats

No `gpu_blocks` join anywhere. Per-client stats stay on
`clients LEFT JOIN submissions` (db.c:1470) using the two new columns:

- workunits = `SUM(s.member_count)` — a real workunit count, not `COUNT(s.id)`
- **rel/sec** = `SUM(num_relations) / SUM(sieve_seconds)` — change the existing
  `AVG` to `SUM`; needs no new data and is the metric that actually compares
  engines
- rel/wu = `SUM(num_relations) / SUM(member_count)` — now comparable across
  engines. Measured on prod: **CPU 3,540, GPU 4,780 over the same 1,000-wide
  range** (+35%, cuda-sieve does not trim `lim` to q). It becomes a geometry
  health check rather than noise — the two engines should stay in that ratio,
  and a drift means one of them is sieving a different rectangle.

Dashboard columns: **relations, rel/sec, rel/wu**. Drop `workunits` (derivable)
and `avg sec` (rel/sec says it better, unit-independent).

`q_passed_1h` currently sums `w.q_range` through `s.workunit_id` (db.c:1398),
so an anchored block would contribute 1,000 instead of 50,000 — understating
GPU throughput ~50x in the numerator of the size-weighted ETA, the one metric
added specifically to make the two engines comparable. Sum `s.q_width` instead.

`db_verify_pass` does `total_workunits + 1` per submission (db.c:1194); make it
`+ member_count`.

There is **no timing column on `workunits`** (db.c:18-33), so phase 4's earlier
"write `t / member_count` to member rows" had nowhere to write. Keep true block
time on `gpu_blocks` and derive any per-workunit allocation at read time.

## Measured on prod (snfs301, 2026-08-30 snapshot)

The 72 GPU-sieved workunits are ordinary 1,000-wide `cpu` rows, so they sit
next to 34,000 CPU submissions on identical work — a clean A/B.

| | CPU (9800X3D, best core) | GPU (RTX 5070, 2 slots) |
|---|---|---|
| rel/sec per worker | 4.8 | 199.7 |
| sec per 1,000-wide unit | 735.7 | 23.94 |
| rel/unit | 3,540 | 4,780 |

**The GPU is ~42x one core.** rel/wu says +35%. This is the case for the
dashboard change on its own: the column that is currently there understates the
card by a factor of 30.

Three numbers that size v2:

- **Startup is 22% of every band.** 23.94 s average against the independently
  measured 5.31 s fixed cost leaves 18.63 s of sieve.
- **The card is 88.1% busy.** `SUM(sieve_seconds) / wall span` = 1724 / 1956 over
  the run, at `--prefetch=2`. The remaining 12% is lease/submit latency the
  prefetch could not hide behind a 24 s band.
- **Both collapse at 50 x base.** A 50,000-wide block is
  `5.31 + 50 x 18.63 = 937 s`, so startup falls 22% -> 0.6% and a ~1.5 s round
  trip is trivially hidden. Card throughput goes **176 -> ~255 rel/sec, +45%**,
  from changing nothing but the unit of assignment.

**Why 50 and not 20**, stated honestly: 20x already reaches 252.9 rel/sec, so
the extra 30x buys **under 1%** of throughput. The real returns are elsewhere —
2.5x fewer lease/submit transactions on the single mongoose thread, which is
what scales with a growing GPU fleet — and the real cost is reclaim
granularity: an expiry throws away 15.6 min of card time instead of 6.3, and
there is no partial-submit concept. Four checks say 50 is still the
conservative end:

| check | at 50x | headroom |
|---|---|---|
| fits one `--lease-seconds=3600` window | 937 s | **3.8x** — no renew needed in the common case |
| submission size vs `OUTPUT_MAX_BYTES` (500 MiB) | ~50 MB raw / ~14 MB zstd | ~10x |
| contiguous available rows | need 50 | **355,690 in a single run** (measured) |
| vs one CPU workunit's wall clock | 937 s | 1.3x of 735.7 s on the fastest core |

The contiguity number is the one that could have gone wrong and did not: prod
is a *single* unbroken run of available rows, because the CPU fleet leases
ascending and consumes from the bottom. Descending block leases therefore carve
from a pristine top edge, and the `min_members` short-run search is insurance
against future fragmentation rather than a day-one necessity.

The multiple only stops fitting one lease window past ~190x, so 3600 s is not
the constraint people will assume it is.

## Retiring v1

**Keep `class`.** There is no advantage to removing it: `workunits.class` is
`NOT NULL DEFAULT 'cpu'` and costs a column unused; removing it costs a
migration against a live 430K-row DB. `clients.last_class` is actively
load-bearing — it is what distinguishes a GPU row from a CPU row on the
dashboard, i.e. the comparison this whole exercise is for.

`recarve` goes, **after** extracting its run scanner (see Minimum run). Drop
the `class` fallback chain only in the same change that lands
`idx_wu_lease_v2`, or the CPU lease path loses its index ordering. If the chain
goes, `extend --class=gpu` should reject rather than remain a documented flag
carving rows nothing protects.

`db_workunit_next_seq` hard-codes `"wu-%s-"` and the `workunits` table
(db.c:466); generalise it to take a prefix and table rather than copying it.

A block-capable client against a pre-block server gets a normal lease response
with no `block` object — mirror the benchmark's existing "server predates
benchmark support" precedent.

## Phases

- [x] **1. Schema + lease.** DONE. `gpu_blocks` in `SCHEMA_SQL`, the three
      `submissions` columns in `db_migrate`, `idx_wu_lease_v2` in
      `SCHEMA_SQL_POST`; `db_block_lease` / `_find_live` / `_renew` /
      `_release` / `_submit` / `_expire_sweep`; anchored addressing through
      `/lease`, `/submit`, `/renew`, `/release`; block sweep ordered before the
      row sweep; `--block-width-multiple=50`, `--block-max-members=256`,
      `--block-min-q-width`, `--block-attempt-ceiling=2` (capped below
      `--max-attempts` at startup). `base_q_range` is cached with a 60 s TTL —
      `db_workunit_base_q_range` is a 38 ms full index scan on a 390K-row
      jobdir, measured, which the lease path cannot pay per request.
      Verified: 94 unit checks; migration idempotent and 0.42 s on a 390K-row
      prod copy; both EXPLAIN QUERY PLANs index-ordered; 8 concurrent block
      leases + 40 CPU leases with zero overlap and zero SQLITE_BUSY; a flaky
      host cycled 6 times reaches `attempt_count` 2 and poisons nothing.
- [x] **2. Verify.** DONE. `UNION ALL` with `AND s.block_id IS NULL` on the
      per-workunit arm and `ORDER BY 1`; `db_pending_t` carries `block_id` /
      `member_count`; `db_block_verify_pass` / `_fail` / `db_block_members`;
      `verify_coverage_t` bucketing with the median/20 starvation threshold;
      one `resolve_fail()` so no early exit can forget a block has members.
      Verified against **real archived lasieve4 relations**: a 6-member block
      of 20,388 relations passes parse + q-range + a k=300 GMP norm spot-check
      with 6/6 covered; omitting one member's relations gives 5/6 with only
      that member requeued at `attempt_count=1`; corrupt data and
      wrong-q-range data each requeue all 6; `total_workunits` advances by
      members verified, not by 1; the ordinary single-workunit path is
      byte-for-byte unchanged (k=50, `member_count=1`).
- [x] **3. Client.** DONE, and it is ~40 lines: anchored addressing means a
      block arrives through the ordinary `workunit_id` / `q_start` / `q_range`
      fields, so sieve, heartbeat, submit and release run the *existing* code
      unchanged. New: `--blocks=yes|no` (default yes under `--engine=cuda`, no
      otherwise, with a warning if forced on for lasieve4 — a block is sized
      for a card and would blow the lease window on one core),
      `--block-max-members=N`, and a lease log line that names a block as a
      block. Old-server fallback is free: `block_members` simply stays 0.
      Verified end to end with the real client against a real server: a
      5-workunit block sieved 16,724 archived relations, submitted 1.0 MB
      zstd, verified 5/5 members at k=250, `total_workunits=5`; a
      `--blocks=no` client on the same server took a single 1,000-wide unit
      from the opposite end with `member_count=1` and created no block.
- [x] **4. Stats + dashboard.** DONE. `q_passed_1h` sums
      `COALESCE(NULLIF(s.q_width,0), w.q_range)`; per-client workunits is
      `SUM(s.member_count)`; new `rel_per_sec` and `sieve_seconds_total` on
      `/stats`; `total_workunits` advances by members verified. Dashboard
      columns are now **relations, rel/sec, rel/wu** — `workunits` and
      `avg sec` dropped, and group aggregation sums real seconds instead of
      averaging averages (`avg * submissions` broke the moment `submissions`
      became a workunit count). Verified: a 5-member block and a single
      workunit on one server give `q_passed_1h = 6000` (5000 + 1000, not
      1000 + 1000) and `wus` of 5 and 1.
- [x] **5. `finalize-nfs.sh` from the DB.** DONE, and it exposed a real bug
      first: a block file named after its anchor is **overwritten** when that
      anchor is starved, requeued and re-sieved individually — silently losing
      the other members' relations. Block files are now named
      `blk-<jobhash>-NNNNNN.dat[.zst]`, which no workunit submission can
      collide with. `finalize-nfs.sh` selects `verify_status='passed'` file
      paths from the DB (re-rooted under the local `rels/`, since stored paths
      are the server's), and its no-DB fallback globs **both** `wu-*` and
      `blk-*`. `move-rels.sh` reads verified ids from `workunits` *and*
      `gpu_blocks`. Verified: 5-member block + 1 workunit + 1 failed
      submission on disk assembles to exactly 20,011 lines
      (16,724 + 3,286 + header) with the failed file excluded.

- [x] **Retiring v1.** `recarve` deleted (269 lines from `db.c`, 139 from
      `server.c`, plus the `db.h` API); the `class` lease-fallback chain
      replaced by `lease_next_available`, which has no class predicate at all
      and so also un-strands any gpu-class rows an older `extend` left behind;
      `extend --class=gpu` now refuses and points at blocks. `workunits.class`
      and `clients.last_class` are kept — the column costs nothing and
      `last_class` is what labels a dashboard row.

## Open decisions

1. ~~Target block q-width~~ — **decided: `50 x db_workunit_base_q_range()`**,
   overridable, clamped. See Block membership.
2. ~~`block_attempt_ceiling`~~ — **decided: 2.** See Failure semantics.
3. ~~`workunits.block_id`~~ — **decided: derive**, no column. Reversible: it
   can be added later as an additive migration if the derivation disappoints.

## Post-implementation review

A full review pass after phases 1-5 raised 15 findings. Twelve were real and are
fixed; two were refuted by measurement and one was downgraded.

**The two that would have lost relations silently:**

- `median > 0` disabled starved-member detection exactly when it mattered. If
  more than half a block's members produce nothing — a killed or truncated
  siever — the median is 0, the requeue is skipped, and `db_block_verify_pass`
  marks every unsieved sub-range `verified`. Fixed by falling back to the max;
  verified live, 4 of 6 starved members now requeue instead of being verified.
- The starved threshold compared raw counts, so a block mixing a 1,000-wide row
  with 20,000-wide ones (normal after an `extend` at a different `--qrange`)
  requeued the narrow member for having 1/20th the relations of its neighbours,
  duplicating its output and walking it toward `poisoned`. Now compares density
  (`count * 1000 / width`); verified live on a real 1,000 + 20,000 block, 2/2
  covered.

**Refuted, with the measurement:**

- *"`idx_wu_lease` is dead now that the lease has no class predicate."* It is
  not: it is the covering index for `/stats`'s `GROUP BY class, state` rollup,
  which the dashboard polls. 50 rollups take 2.10 s with it and 10.48 s without
  (`SCAN workunits` + `USE TEMP B-TREE FOR GROUP BY`). Kept.
- *"`db_block_lease` holds the write lock across an 8192-row scan."* The scan
  limit is `max_members * 4`, so at the default `--block-max-members=256` it is
  1024 rows — 0.67 ms, measured on a 390K-row jobdir, and only on the endgame
  path where no run qualifies. 8192 needs `--block-max-members=2048`.

**Worse than the finding claimed:** `next_seq_for_prefix` full-scanned its table
(`MAX(CAST(substr(id,...)))` cannot use an index) — 7.3 ms over 50,000
`gpu_blocks` rows, inside `db_block_lease`'s write transaction, growing linearly
with campaign length. Now a half-open key-range seek on the primary key,
`O(log n)`. Correct because zero-padded suffixes sort lexicographically in
numeric order, and a suffix that outgrows its padding starts with `1`-`9`, which
still sorts above the `0` padding every narrower one.

Also fixed: the `!file_path` verifier exit bypassed block resolution and
stranded members in `submitted`; a block-sweep failure returned early and
skipped ordinary lease expiry for the whole fleet; `ggnfs-verify` silently
skipped the q-range check on every `blk-*` file; the dashboard's box-level
rel/sec was a weighted mean of worker rates, so a 16-core box read the same as
one core; and `usage_extend` still advertised the flag it now refuses.

## Migration reality

Nothing to migrate. Prod (`/stats`, checked) carries **zero gpu-class rows**:
390,000 rows, all `cpu`. The 72 workunits sieved on the GPU during testing
went through the class-fallback path as ordinary 1000-wide `cpu` rows and
verified normally — they are indistinguishable from CPU work.
