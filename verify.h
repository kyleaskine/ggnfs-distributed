/* verify.h — relation-file and .job parsers for the verifier.
 *
 * Phase 3 verifier will sit on top of this:
 *   1. parse pass — `verify_parse_file` over every relation, syntactic check.
 *   2. q-range check — at least one prime in the sieved side's list lies in
 *      [q_start, q_start + q_range). (Caller does this; the parser just
 *      exposes the two prime lists separately.)
 *   3. spot-check — recompute rational + algebraic norms on K random
 *      relations using the polynomial loaded from meta.
 *
 * No GMP here yet; norm math lands when we wire up the spot-check.
 */
#ifndef GGNFS_SIEVE_VERIFY_H
#define GGNFS_SIEVE_VERIFY_H

#include <stddef.h>
#include <stdint.h>

/* Forward-decl so we don't drag db.h into every TU that uses the parser. */
typedef struct ggnfs_db_s ggnfs_db_t;

/* ---- relation lines -------------------------------------------------- */

/* Per-side prime cap. A relation has up to mfb/lpb large primes plus the
 * small primes from sieving. In practice we see <10 per side; 32 is plenty
 * with room for pathological factor-base settings. If a line exceeds this
 * we return -1 (parse error) so the caller can flag the submission. */
#define VERIFY_MAX_PRIMES_PER_SIDE 32

typedef struct {
    int64_t  a;                 /* decimal signed in the file */
    uint64_t b;                 /* decimal unsigned, must be > 0 */
    int      n_rprimes;
    int      n_aprimes;
    uint64_t rprimes[VERIFY_MAX_PRIMES_PER_SIDE];  /* rational-side, hex in file */
    uint64_t aprimes[VERIFY_MAX_PRIMES_PER_SIDE];  /* algebraic-side, hex in file */
} verify_relation_t;

/* Parse one relation line. `line` need not be NUL-terminated; `len` is the
 * byte count excluding any trailing '\n'/'\r'. Returns 0 on success and
 * fills *out; returns -1 on malformed input (out content undefined).
 *
 * Rejects b == 0: msieve-style free relations have that shape but raw
 * gnfs-lasieve4* output never does, so a b=0 line in fresh siever output
 * is a sign something is wrong upstream. */
int verify_parse_line(const char *line, size_t len, verify_relation_t *out);

/* Optional per-relation q-range check. The sieved side's prime list
 * (aprimes if side='a', rprimes if side='r') must contain at least one
 * prime in [q_start, q_start + q_range) — that prime IS the special-q the
 * relation came from. Set side=0 to skip the check (parse-pass only). */
typedef struct {
    int64_t q_start;
    int64_t q_range;
    char    side;           /* 'a' or 'r'; 0 = skip q-range check */
} verify_check_t;

/* Reservoir buffer for Algorithm-R sampling of K accepted relations during
 * the parse pass. Caller allocates `buf` (cap entries) and seeds `seed` once.
 * The streamer fills `count` and `sampled_from` as it goes. */
typedef struct {
    verify_relation_t *buf;
    int                cap;
    unsigned int       seed;       /* rand_r state; caller initializes */
    int                count;      /* output: # filled (<= cap) */
    int64_t            sampled_from; /* output: # accepted relations seen */
} verify_reservoir_t;

/* Stream a relation file from disk. Optionally check each parsed relation
 * against `check`, and optionally reservoir-sample fully-accepted relations
 * into `reservoir`. Returns 0 on success (file readable), -1 on I/O error.
 *
 *   out_parsed         parsed lines that ALSO satisfied the q-range check
 *                      (i.e. fully accepted). If check is NULL or side=0,
 *                      this counts every parseable line.
 *   out_failed         lines that failed to parse.
 *   out_q_violations   lines that parsed but had no q in range.
 *                      Always 0 if check is NULL or side=0.
 *   out_first_reason   if non-NULL, buflen >= 1: a one-line description of
 *                      the first failure encountered (parse or q-range);
 *                      empty if no failures. NUL-terminated.
 *   reservoir          if non-NULL with cap > 0: Algorithm R sampling of
 *                      accepted relations. On return, reservoir->count is
 *                      <= cap and contains a uniform random sample.
 *
 * Any out pointer may be NULL (except buflen must be 0 when reason is NULL). */
int verify_parse_file_check(const char *path,
                            const verify_check_t *check,
                            verify_reservoir_t *reservoir,
                            int64_t *out_parsed,
                            int64_t *out_failed,
                            int64_t *out_q_violations,
                            char    *out_first_reason,
                            size_t   reason_buflen);

/* Thin wrapper: parse-pass only, no q-range check. Used by the
 * `verify-parse` diagnostic subcommand. */
int verify_parse_file(const char *path,
                      int64_t *out_parsed,
                      int64_t *out_failed);

/* ---- polynomial / .job file ----------------------------------------- */

/* Maximum polynomial degree we support. ggnfs jobs are typically degree 4–6;
 * we allow up to 8 to cover any reasonable selection. */
#define VERIFY_MAX_POLY_DEGREE 8

typedef struct {
    int   degree;                                  /* highest k with c[k] set */
    char *c[VERIFY_MAX_POLY_DEGREE + 1];           /* decimal strings, malloc'd */
    char *Y0;                                      /* decimal string, malloc'd */
    char *Y1;                                      /* decimal string, malloc'd */
} verify_poly_t;

/* Zero out a verify_poly_t. Always safe to call before parse/free. */
void verify_poly_init(verify_poly_t *p);

/* Free all owned strings; leaves the struct zeroed. */
void verify_poly_free(verify_poly_t *p);

/* Parse a ggnfs .job file. Recognized keys: c0..c<N>, Y0, Y1. All other
 * lines (n, skew, rlim, alim, *lim, *lpb, *mfb, *lambda, blank, '#...')
 * are ignored. Returns 0 on success, -1 on I/O or parse error. */
int verify_parse_job_file(const char *path, verify_poly_t *out);

/* Persist a parsed polynomial into the `meta` table:
 *   poly_degree, poly_c0..poly_c<degree>, poly_Y0, poly_Y1.
 * Returns 0 on success, -1 on DB error. */
int verify_poly_save_to_meta(ggnfs_db_t *db, const verify_poly_t *p);

/* Load a polynomial from the meta keys written by save_to_meta. The output
 * struct is filled with freshly malloc'd strings; caller frees with
 * verify_poly_free. Returns 0 on success, -1 if any required key is missing
 * or malformed. */
int verify_poly_load_from_meta(ggnfs_db_t *db, verify_poly_t *out);

/* ---- GMP-backed polynomial for norm spot-check -------------------- */

/* Opaque handle; holds mpz_t coefficients built from a verify_poly_t. */
typedef struct verify_poly_gmp_s verify_poly_gmp_t;

verify_poly_gmp_t *verify_poly_gmp_new(const verify_poly_t *src);
void               verify_poly_gmp_free(verify_poly_gmp_t *p);

/* Run msieve-style norm spot-check on `n` relations. For each:
 *   1. Compute |N_R| = |a*Y1 + b*Y0| and |N_A| = |sum c_k a^k b^(d-k)|.
 *   2. Divide each by listed primes (with all multiplicities).
 *   3. Trial-divide remaining by primes <= 1000.
 *   4. Accept if residue is 1 OR probable-prime; otherwise the relation is
 *      considered broken (composite residue means a listed prime didn't
 *      actually divide the norm, or a needed prime is missing).
 * Returns the number of failed relations. If out_first_reason is non-NULL,
 * a one-line description of the first failure is written to it. */
int verify_spotcheck(const verify_poly_gmp_t *p,
                     const verify_relation_t *rels, int n,
                     char *out_first_reason, size_t reason_buflen);

/* ---- verifier thread ------------------------------------------------- */

/* Opaque handle. The thread owns its own ggnfs_db_t (a second connection
 * against the same file); see db.h's threading note. */
typedef struct verify_thread_s verify_thread_t;

/* Start the verifier. Spawns one pthread that:
 *   1. drains every pending submission (parse pass on the relation file),
 *   2. on a clean parse, marks it passed and the workunit verified;
 *   3. on any parse error, marks it failed and requeues / poisons the WU;
 *   4. then waits on a condvar (with a ~5s safety-net timeout) for the
 *      next /submit to nudge it via verify_thread_wake().
 *
 * `db_path` is the SQLite file path (same one cmd_serve opens). `max_attempts`
 * matches the value used by the lease-expiry sweep, so a workunit gets the
 * same retire-after-N treatment regardless of which path failed it.
 *
 * `spotcheck_k` controls the GMP norm spot-check: K random relations per
 * submission have their norms recomputed and trial-divided. K=0 disables
 * spot-check (parse + q-range only). The thread loads the polynomial from
 * meta on startup; if that fails, spot-check is silently disabled and a
 * warning is logged.
 *
 * Returns NULL on failure (logs the cause). */
verify_thread_t *verify_thread_start(const char *db_path,
                                     int64_t max_attempts,
                                     int spotcheck_k);

/* Nudge the verifier to process new work. Cheap; safe from any thread.
 * No-op if vt is NULL so callers can stay simple when verify is disabled. */
void verify_thread_wake(verify_thread_t *vt);

/* Stop the thread (sets a flag, signals the condvar, joins). Frees the
 * handle. No-op if vt is NULL. */
void verify_thread_stop(verify_thread_t *vt);

#endif /* GGNFS_SIEVE_VERIFY_H */
