# FUTURE.md — backlog

Ideas that came up while building the MVP but haven't shipped. None of
these block real use; collect the context here so we can revisit later
without re-litigating.

## Lifecycle / hygiene

- **Graceful server shutdown.** `cmd_serve` is `for(;;) mg_mgr_poll(...)`
  and the cleanup code below the loop is unreachable — SIGINT just kills
  the process. WAL mode keeps the DB safe and `verify_thread_stop()`
  already exists, so this is mostly about: install a SIGINT/SIGTERM
  handler that flips a flag, exit the poll loop, stop the verifier,
  free mongoose.

- **Replace client `system()` with child PID management.** Client Ctrl-C
  now has drain/cancel phases and `/release`, but the siever still runs
  through `system("gnfs-lasieve4...")`. Ctrl-C is delivered to the whole
  process group in normal terminal use, so cancellation usually interrupts
  the shell/siever, but precise process control wants `fork`/`exec` or
  `posix_spawn` plus a stored child PID per worker.

- **Legacy submission migration.** Submissions written before the
  verifier landed have `verify_status='skipped'` and their workunits
  are stuck in `'submitted'` because nothing transitions them. A one-
  line `UPDATE` flips them to `'verified'` for any affected jobdir;
  not worth code unless we discover a jobdir we care about.

## Operator convenience

- **`--local-clients=N` on serve.** Auto-spawn N local
  `ggnfs-sieve-client` processes against `localhost`. Single-box use
  becomes one command instead of two terminals. Was Phase 5 of the
  original design.

- **Worker heartbeats / progress.** Lease timeout alone handles failure
  today, but a slow client whose siever is still grinding gets its
  workunit requeued at `lease_seconds`, double-issuing the work. A
  periodic heartbeat that bumps `lease_expires` would let us tighten
  the lease window without false requeues. The original design listed
  this as a non-goal, but if jobs get long it starts mattering.

## Performance / efficiency

- **`.afb` pre-generation.** Server runs `gnfs-lasieve4* -F` once to
  generate the algebraic factor base, ships the resulting `.afb.0`
  alongside the `.job`. Saves factor-base setup time on every workunit
  the client takes. Modest win; only worth it if startup overhead shows
  up in profiling.

- **Client capability advertising.** Clients send the list of
  `gnfs-lasieve4I*` binaries they have installed; server matches each
  workunit to a client that can run it. Lets heterogeneous fleets
  self-target. Not needed if every client has the same siever, which
  is the usual case.

## Big swings (probably never)

- **TLS.** Lets us run over the public internet. Big PKI lift for a
  workgroup tool; the trust model also assumes other things (trusted
  clients, no relay-and-replay defenses on submissions) that TLS alone
  wouldn't fix.
- **Windows clients.** Build changes plus path handling for the siever
  invocation. Niche audience.
- **Multi-job per server.** One factorization at a time today; adding
  multi-tenancy means namespacing workunit IDs, file paths, dashboards,
  and the `meta` table. Probably easier to just run two servers on
  different ports.
