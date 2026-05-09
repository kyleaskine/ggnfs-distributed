/* sieve_executor.h — wrapper around one gnfs-lasieve4* invocation.
 *
 * Sieves special-q range [startq, startq+qrange) on `side` ('a' or 'r') for
 * the polynomial in `job_infile`, writing relations to `outfile`. Returns the
 * siever's exit code (mirrors system()).
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
                    char side);

#endif /* GGNFS_SIEVE_EXECUTOR_H */
