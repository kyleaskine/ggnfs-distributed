# ggnfs-distributed

A small HTTP coordinator for distributing GGNFS lattice sieving (the
special-q phase of the General Number Field Sieve) across multiple
machines. One server chops a Q-range into workunits and hands them out
under lease; each client runs `gnfs-lasieve4*` locally and returns
relations. Output drops into msieve/YAFU's existing filter → linear
algebra → sqrt pipeline via `finalize-nfs.sh`.

Two binaries from one Makefile: `ggnfs-sieve-server`, `ggnfs-sieve-client`.

## Trust model — read this first

The server speaks plain HTTP, authenticated by a shared bearer token.
**There is no TLS.** Anyone on the same network can read your
submissions and replay the token, and the bug-catching verifier
(parse pass + q-range + GMP norm check) is not an adversary defense:
a hostile client that knows the math could craft passing relations.

This is fine for the use case the project targets — a small group of
trusted operators on a private network. **Don't run it across the
public internet.**

## Build

Depends on `libgmp` (the verifier recomputes algebraic and rational
norms). On Debian/Ubuntu:

    apt install libgmp-dev

Everything else (mongoose, cJSON, SQLite) is vendored under `vendor/`
so a clean clone builds offline.

    make

Produces `ggnfs-sieve-server` and `ggnfs-sieve-client`.

## Run end-to-end

Initialize a jobdir from a `.job` file (the same one YAFU/msieve uses —
polynomial plus factor-base settings):

    ./ggnfs-sieve-server init \
        --job=input.job \
        --siever=gnfs-lasieve4I14e \
        --qmin=80000000 --qmax=100000000 --qrange=10000 \
        --jobdir=/tmp/myjob

`init` writes a random bearer token to `/tmp/myjob/token` (chmod 600).
Hand that file to whoever will run clients.

Serve:

    ./ggnfs-sieve-server serve --jobdir=/tmp/myjob --port=8080

Dashboard: `http://host:8080/?token=<contents of jobdir/token>`. The
HTML is unauthenticated; its embedded JS reads the token from the URL
and polls `/stats`.

On each worker box (every machine must already have a matching
`gnfs-lasieve4*` binary installed):

    ./ggnfs-sieve-client \
        --server-url=http://host:8080 \
        --token=<token> \
        --siever=/path/to/gnfs-lasieve4I14e \
        --workers=4 \
        --cpu-pin=0,2,4,6        # optional, Linux only

When enough relations have come back, assemble `nfs.dat` and feed it
to YAFU:

    ./finalize-nfs.sh --jobdir=/tmp/myjob --yafu-dir=/path/to/yafu --threads=8 --run

`finalize-nfs.sh` aborts if `<yafu-dir>/nfs.job` differs from the `.job`
the server distributed — mismatched factor-base settings silently
corrupt filtering, so the script enforces a SHA match.

## Adding more work to a running job

You can extend the Q-range without restarting `serve`. `qmin` must be
at least the existing `q_end` so workunit IDs don't collide.

    ./ggnfs-sieve-server extend --jobdir=/tmp/myjob \
        --qmin=100000000 --qmax=120000000 --qrange=10000

## Verifier

Every submission is checked three ways before its workunit transitions
to `verified`:

1. **Parse pass** — every line matches `a,b:rprimes:aprimes`. Free-
   relation shape (`b=0`) is rejected.
2. **Q-range check** — at least one prime in the sieved-side list lies
   in `[q_start, q_start + q_range)`. Catches "client returned the
   wrong range" bugs.
3. **Norm spot-check** — for `K` random relations per submission
   (default 50, tune with `--spotcheck-k=N` on `serve`; 0 disables),
   recompute both norms via GMP, divide out the listed primes, trial-
   divide by primes ≤ 1000, confirm the residue is 1 or a probable
   prime. Convention matches msieve's `nfs_read_relation`, so anything
   we accept will also survive msieve's own filter pass.

Any check failing puts the workunit back to `available` with
`attempt_count++`; after `--max-attempts` (default 5) it becomes
`poisoned` and surfaces in `/stats` for operator attention.

The polynomial is parsed from the `.job` file at `init` time and stored
in the `meta` table (`poly_degree`, `poly_c0..c<d>`, `poly_Y0`,
`poly_Y1`) so the verifier doesn't re-read the file at startup.

### Migrating old jobdirs

For a jobdir created before verifier metadata existed, stop `serve` and run:

    ./ggnfs-sieve-server migrate-legacy --jobdir=/tmp/myjob

This backfills the polynomial `meta` rows from the stored `.job` file, then
revalidates legacy `skipped` submissions whose workunits are still
`submitted`. If a jobdir was copied from another machine, the command also
rewrites those legacy relation paths to `<jobdir>/rels/<workunit>.dat` when
that local file exists.

## More

- **Architecture, threading invariants, load-bearing details:** see
  `CLAUDE.md`.
- **Things we might add later:** see `FUTURE.md`.
- **Vendored library bumps:** see `vendor/VENDOR.md`.
