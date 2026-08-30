/* sieve_executor.h — wrapper around one siever invocation.
 *
 * Two engines write the same relations for the same special-q band:
 *   sieve_run_local  — gnfs-lasieve4* on the CPU
 *   sieve_run_cuda   — cuda-sieve's `bench` on an NVIDIA GPU
 *
 * Both sieve [startq, startq+qrange) on `side` ('a' or 'r') for the
 * polynomial in `job_infile`, write relations to `outfile`, and return the
 * child's exit code (or 128+signal if it died from one). Both run the child
 * in its own process group and poll `should_cancel` while waiting, so a
 * terminal Ctrl-C does not reach an active siever during drain and an
 * explicit cancel can signal the whole group.
 *
 * `extra_args` is appended to the command line verbatim; pass NULL or "" for
 * none. It is engine-specific vocabulary — "-J 16" for lasieve4,
 * "--logI 17 --J 16384" for cuda-sieve — and the two are NOT translations of
 * each other. The server ships both strings in the lease response
 * (siever_args / gpu_args) and the client picks by its --engine.
 */
#ifndef GGNFS_SIEVE_EXECUTOR_H
#define GGNFS_SIEVE_EXECUTOR_H

#include <stdint.h>

typedef int (*sieve_cancel_fn)(void *ctx);

/* Run an arbitrary command through /bin/sh with the same isolation the two
 * sievers get: its own process group (so a terminal Ctrl-C does not reach it)
 * and a `should_cancel` poll that escalates SIGTERM -> SIGKILL to the group.
 * Returns the exit code, or 128+signal. Used for out-of-band helpers such as
 * cuda-sieve's fbgen_gpu. */
int sieve_run_command(const char *syscmd,
                      sieve_cancel_fn should_cancel,
                      void *cancel_ctx);

/* gnfs-lasieve4*. Appends to `outfile`, so any prior file there is removed
 * first. */
int sieve_run_local(const char *siever_path,
                    const char *job_infile,
                    const char *outfile,
                    uint32_t startq,
                    uint32_t qrange,
                    char side,
                    const char *extra_args,
                    sieve_cancel_fn should_cancel,
                    void *cancel_ctx);

/* cuda-sieve `bench`.
 *
 * `fb1_path` is an optional pre-generated factor-base cache (cuda-sieve's
 * --fb1); NULL or "" makes bench generate the base in-process on the GPU,
 * which is correct but costs real time on every workunit. `device` selects a
 * card, or -1 to leave the choice to bench.
 *
 * bench stages relations to "<outfile>.part" and renames to `outfile` only
 * when the band completes, so the caller's "did outfile appear?" check means
 * exactly "band finished" — a cancelled or crashed run leaves no `outfile`.
 * Any prior staging artifacts are discarded (--restart) because this
 * coordinator has no partial-submit concept, so a leftover .part can never be
 * resumed into a valid submission.
 */
int sieve_run_cuda(const char *bench_path,
                   const char *job_infile,
                   const char *outfile,
                   uint32_t startq,
                   uint32_t qrange,
                   char side,
                   const char *extra_args,
                   const char *fb1_path,
                   int device,
                   sieve_cancel_fn should_cancel,
                   void *cancel_ctx);

#endif /* GGNFS_SIEVE_EXECUTOR_H */
