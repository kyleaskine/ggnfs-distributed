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

- **`.afb` pre-generation.** Run `gnfs-lasieve4* -k` once (that's the
  write flag; `-F` forces a *recompute*) to generate the algebraic
  factor base, ship the resulting `.afb.0` alongside the `.job`; the
  siever auto-reads it whenever it exists. Measured on the C208: startup
  24s -> 2s with byte-identical relations, i.e. 30-45s saved on every
  workunit. Two requirements: (1) generate with `-f` *above* alim,
  because the siever lowers the FB bound to first_spq-1 when q < alim
  and would write a partial cache; (2) the siever needs the trim-on-read
  patch in yafu's `factor/lasieve5_64/gnfs-lasieve4e.c` (June 2026),
  since the stock read path uses the cached FB unmodified — unsafe for
  q < alim workunits. Don't pass `-k` to fleet clients (concurrent
  first-workunit writers race on the same file); generate server-side
  or once per client at install. Regenerate if the poly or alim ever
  changes — the file carries no consistency check.

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
