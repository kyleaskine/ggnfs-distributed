/* sieve_executor.h — wrapper around one gnfs-lasieve4* invocation.
 *
 * Sieves special-q range [startq, startq+qrange) on `side` ('a' or 'r') for
 * the polynomial in `job_infile`, writing relations to `outfile`. Returns the
 * siever's exit code (mirrors system()).
 *
 * `extra_args` is appended to the command line verbatim. Pass NULL or "" for
 * none. Used for tunables the operator wants on every worker — typically
 * shipped by the server in the lease response so the whole job uses one
 * setting (e.g. "-J 16" for a larger I-sieve area).
 *
 * The siever appends to `outfile`, so this function removes any prior file at
 * that path before invoking it.
 */
#ifndef GGNFS_SIEVE_EXECUTOR_H
#define GGNFS_SIEVE_EXECUTOR_H

#include <stdint.h>

int sieve_run_local(const char *siever_path,
                    const char *job_infile,
                    const char *outfile,
                    uint32_t startq,
                    uint32_t qrange,
                    char side,
                    const char *extra_args);

#endif /* GGNFS_SIEVE_EXECUTOR_H */
