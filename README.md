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
norms) and `libzstd` (clients compress relation submissions before
upload). On Debian/Ubuntu:

    apt install libgmp-dev libzstd-dev

Everything else (mongoose, cJSON, SQLite) is vendored under `vendor/`.

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

For large machines, set `--workers` to the number of sievers you want to run
and leave `--http-concurrency` at its default of 16 unless the coordinator can
comfortably handle more simultaneous client sockets. The workers share a single
input-file cache under `<workdir>/files`, so high worker counts do not need to
download the job file once per core. The client also spaces new coordinator
connections by `--http-interval-ms=50` by default; increase that to 100 or 200
if a provider or firewall still gets overwhelmed during startup.

If the server is temporarily down when a worker finishes sieving, the
client keeps the completed relation file in its workdir and retries
`/submit` instead of leasing new work. Press Ctrl-C once to drain
without starting more work; press it again to cancel, release active
leases, and leave any unsubmitted local relation file for inspection.

When enough relations have come back, assemble `nfs.dat` and feed it
to YAFU:

    ./finalize-nfs.sh --jobdir=/tmp/myjob --yafu-dir=/path/to/yafu --threads=8 --run

For a local `pull-rels.sh` archive, use the same command with its local
directory, for example `--jobdir=./snfs301`. Finalization reads `archive/`
and `rels/`, using `job.db` or the newest usable `incoming/<timestamp>/job.db`
snapshot to select passed submissions. Automatic discovery checks snapshot
integrity and schema, warning and trying older snapshots if a transfer left
an incomplete database. It includes both CPU `wu-*` and GPU
`blk-*` files, raw or zstd-compressed, and excludes files in `incoming/`.
GPU relations use the same format; YAFU/msieve removes duplicates during
filtering, so raw GPU relation counts can overstate the usable yield.
The target YAFU reader supports 64-bit prime factors but requires `b < 2^32`.
Finalization omits and counts relations with larger `b` coordinates, which
GPU sieving can produce. The source archives remain intact. Without this
step, enough such relations can trigger YAFU's 10,000-relation-error abort.
This is an export constraint for that reader, not a validity requirement:
sieving and verification retain wide-`b` relations for consumers such as
CADO and msieve builds that support them.

Use `--check` to check job identity and report eligible file counts without
reading relation contents or assembling data; `--check --run` still only
checks. Use `--phase=nc1 --run` to run filtering only. Missing local files
recorded as passed in the snapshot produce a warning. Query failures in a
selected database abort; directory globbing is used only when no database
is available. `--jobdb=PATH` selects a particular snapshot and does not fall
back to another database on failure.

Pulls retain the original `files/<sha>.job` beside each database snapshot.
With no new relations, they fetch only the small job file into
`<local-dir>/files/` if needed, without another full database snapshot or
`incoming/<timestamp>/` directory. Remote job metadata is checked and copied
before any relations are moved. For an older archive, run
another pull or supply `--job-file=PATH` with the original job file. The
finalizer also looks in `<jobdir>/files/`, `<jobdir>.job`, and the existing
YAFU `nfs.job`, requiring a match with the database's job hash. The existing
YAFU `nfs.job` is an automatic source only when a database authenticates it.
A local
client caches the original at `<client-workdir>/files/<sha>` (the default
workdir is `/tmp/ggnfs-client`; the cached filename has no `.job` extension).
Pass that extensionless cache path with `--job-file=PATH`, or copy it into
`<jobdir>/files/<sha>.job`; automatic discovery there uses `*.job`.

The `nc2`, `nc3`, and `ncr` phases reuse the existing `nfs.dat` so relation
indices stay consistent with filtering output and LA checkpoints. Start
again at filtering if you want to incorporate additional relations.

Some YAFU builds have a 300-byte `LINE_BUF_SIZE` in
`ms_include/savefile.h`. A long decimal `N` header can then be split into
multiple reads, with its trailing digits reported as an invalid first
relation. The full header in `nfs.dat` is correct; the reader buffer needs
to be enlarged in that YAFU build. Do not change the reader or file during
an existing filtering/LA/sqrt run, whose relation indices must stay stable.

To stage relation files for download before validating or deleting them
from the server, move the contents of `<jobdir>/rels` into a separate
folder:

    ./move-rels.sh --jobdir=/tmp/myjob --dest=/tmp/myjob-relsBackup

Use `--dry-run` first to preview the move, and `--overwrite` only if
replacing same-named files in the destination is intentional.

`finalize-nfs.sh` aborts if `<yafu-dir>/nfs.job` differs from the `.job`
the server distributed, and checks that job against the database's SHA
when available. This prevents mixing jobs or silently changing settings.

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

## More

- **Architecture, threading invariants, load-bearing details:** see
  `CLAUDE.md`.
- **Things we might add later:** see `FUTURE.md`.
- **Vendored library bumps:** see `vendor/VENDOR.md`.
