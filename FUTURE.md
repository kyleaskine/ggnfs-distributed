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

## Operator convenience

- **`--local-clients=N` on serve.** Auto-spawn N local
  `ggnfs-sieve-client` processes against `localhost`. Single-box use
  becomes one command instead of two terminals. Was Phase 5 of the
  original design.

- **Worker heartbeats / progress.** *Now scheduled as `POST /renew`, phase
  2.1 of `GPU-CLIENT.md`* — GPU-sized workunits make a fixed lease window
  untenable, so this stops being optional. Original context: lease timeout
  alone handles failure today, but a slow client whose siever is still
  grinding gets its workunit requeued at `lease_seconds`, double-issuing the
  work. The original design listed this as a non-goal; long jobs changed
  that.

## Performance / efficiency

- **Client capability advertising.** Clients send the list of
  `gnfs-lasieve4I*` binaries they have installed; server matches each
  workunit to a client that can run it. Lets heterogeneous fleets
  self-target. Not needed if every client has the same siever, which
  is the usual case. `GPU-CLIENT.md` phase 1.3 adds a narrow special
  case (cpu/gpu workunit classes); the general form is still open.

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
