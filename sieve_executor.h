#ifndef YAFU_SIEVE_EXECUTOR_H
#define YAFU_SIEVE_EXECUTOR_H

#include <stdint.h>

/* Executor seam for one lattice-sieving invocation:
 * "sieve special-q range [startq, startq+qrange) on `side` for the polynomial
 *  in `job_infile_name`, write relations to `outfilename`."
 *
 * Implementations:
 *   - local: shell out to gnfs-lasieve4* via system(). Phase 0 default.
 *   - distributed (Phase 1+): ship the request to a remote sieve server.
 *
 * The executor owns the output file path: it must ensure outfilename does
 * not contain stale relations on entry, and that on success it contains the
 * relations produced for [startq, startq+qrange). The launcher counts and
 * post-processes the resulting file regardless of which executor ran.
 */

typedef struct {
    const char *sievername;       /* path/name of gnfs-lasieve4* binary  */
    const char *job_infile_name;  /* .job/.poly describing the polynomial */
    const char *outfilename;      /* where the relation file should land  */
    uint32_t    startq;
    uint32_t    qrange;
    int         side_is_algebraic;/* 1 = algebraic, 0 = rational           */
    int         batch_3lp;        /* nonzero => pass -d to the siever      */
    int         tindex;           /* siever's -n value                     */
    int         verbose;          /* mirrors fobj->VFLAG                   */
} sieve_request_t;

typedef struct sieve_executor_s sieve_executor_t;

struct sieve_executor_s {
    /* Mirrors system(): returns the siever's exit code, or -1 on internal
     * executor failure (e.g. could not contact the server). */
    int  (*run)(sieve_executor_t *self, const sieve_request_t *req);
    /* destroy() may be NULL for static singletons (e.g. the local executor). */
    void (*destroy)(sieve_executor_t *self);
    void  *state;
};

/* Factories. */
sieve_executor_t *sieve_executor_local_create(void);

/* Process-wide default executor (returns the local executor in Phase 0).
 * Phase 1+ will add a setter that lets a distributed mode swap this. */
sieve_executor_t *sieve_executor_default(void);

/* Convenience: dispatch through the default executor. */
int sieve_executor_run(const sieve_request_t *req);

void sieve_executor_destroy(sieve_executor_t *e);

#endif /* YAFU_SIEVE_EXECUTOR_H */
