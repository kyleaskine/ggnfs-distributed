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

## Security

- **Stop composing the siever command as a shell string.** `sieve_run_local`,
  `sieve_run_cuda` and `sieve_run_command` `snprintf` their arguments into one
  line and hand it to `run_child_cancelable`, which runs `/bin/sh -c "exec
  ..."`.

  The injection route through that string is closed — `siever` is validated by
  `sieve_resolve_siever` and `siever_args`/`gpu_args` are parsed and rebuilt by
  `sieve_sanitize_args`, so every field is now locally generated. But it is
  closed by *maintenance*, not by construction: the safety depends on every
  future field remembering to do the same. `execvp` on an argv array would make
  it structural.

  The repair is an argv array plus `execvp`, which removes the shell entirely.
  It is not a small change, because the shell is currently doing real work:
  the `exec` prefix is what makes the waited-on pid the siever's rather than
  dash's, and `run_child_cancelable`'s process-group isolation and
  SIGTERM/SIGKILL escalation are built around that (see the `run_child_cancelable`
  entry in CLAUDE.md). `siever_args`/`gpu_args` would also need tokenising,
  which is its own small parser. Worth doing deliberately rather than as a
  rider on something else.

  Mitigating context, not an excuse: the README's trust model already assumes
  trusted clients on a private network with no TLS, and a coordinator that can
  set `meta.siever_args` can usually already reach the workers by other means.

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

  *Sequential* multi-job is done and no longer motivates this: a fleet key
  shared across jobdirs (`init --token=@key`, `serve --token-file=key`) plus
  per-lease siever resolution means repointing `serve` at a new jobdir
  migrates the whole fleet with nothing done on any worker. What remains open
  is genuinely *concurrent* tenancy, which is deeper than it looks — the
  verifier loads one polynomial once at thread start (`verify.c`),
  `db_verify_next_pending` is job-blind, and `db_workunit_base_q_range` is a
  whole-table mode that sizes both GPU blocks and spot-check density. Two jobs
  in one DB would norm-check the second against the first's polynomial and
  poison it.

- **Telling a client *why* its workunit was rejected.** After a repoint the
  server can only answer a stale `/submit`, `/renew` or `/release` with a bare
  400 from `workunit_id_is_safe_for_job`, and the client infers "the
  coordinator moved to a different job" from that. A `job_sha256` in the
  `/lease` response, or a structured body on the 400, would let it say so
  precisely instead of guessing.
