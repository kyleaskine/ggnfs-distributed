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

#include <stddef.h>
#include <stdint.h>

typedef int (*sieve_cancel_fn)(void *ctx);

/* ---- which binary do we invoke? ----------------------------------------
 *
 * The siever is a property of the JOB, not of the client: the coordinator
 * names it in every /lease response. A client whose --siever points at a
 * DIRECTORY resolves <dir>/<server_name> per lease, so pointing `serve` at a
 * new jobdir switches the whole fleet's siever with nothing to reconfigure.
 *
 *   configured  -- the --siever value: "" (cuda, no lasieve4 at all),
 *                  a file (used verbatim, today's behaviour), or a directory
 *   server_name -- lease.siever / stats.siever. Arrives OVER THE WIRE.
 *
 * server_name is validated, never trusted: sieve_run_local formats the
 * resolved path into a command string that run_child_cancelable hands to
 * /bin/sh -c, so a name carrying a shell metacharacter would run as a command.
 * Only a plain gnfs-lasieve4-shaped basename is accepted.
 *
 * Be clear about what this does and does not buy. It is NOT the case that
 * nothing from the wire reached that command string before: `siever_args` and
 * `gpu_args` come from the same /lease response and are still interpolated
 * verbatim (see sieve_run_local / sieve_run_cuda below), so a coordinator that
 * can set meta.siever_args already has this reach. Validating the name closes
 * one of three wire-supplied inputs and keeps a path traversal out of the
 * resolution step, which is worth doing on its own; it is not a general fix.
 * The general fix is to stop composing a shell string at all and execvp an
 * argv array -- see FUTURE.md.
 *
 * On success returns SIEVE_RESOLVE_OK and fills `out`. The failure codes are
 * distinct because the operator fix differs for each: a bad name means the
 * coordinator's meta is wrong, a missing binary means this box's siever
 * directory is incomplete.
 */
#define SIEVE_RESOLVE_OK          0   /* `out` holds the binary to run      */
#define SIEVE_RESOLVE_NONE        1   /* nothing configured; `out` is ""    */
#define SIEVE_RESOLVE_BAD_NAME   -1   /* server_name failed validation      */
#define SIEVE_RESOLVE_NOT_FOUND  -2   /* no such entry in the directory     */
#define SIEVE_RESOLVE_NOT_EXEC   -3   /* present but not an executable file */
#define SIEVE_RESOLVE_TOO_LONG   -4   /* <dir>/<name> would not fit in out  */

int sieve_resolve_siever(const char *configured, const char *server_name,
                         char *out, size_t out_n);

/* Human-readable form of the codes above, for error messages. */
const char *sieve_resolve_strerror(int rc);

/* ---- server-supplied tuning flags --------------------------------------
 *
 * `siever_args` and `gpu_args` arrive in the /lease response and end up in a
 * command line. Passing them through verbatim means the coordinator decides
 * what the worker executes -- not just the shell metacharacter case, but the
 * quieter one where every token is shell-safe and the flags themselves are
 * hostile (`-o /home/you/.ssh/authorized_keys` needs no metacharacters at all,
 * and an argv array would not help).
 *
 * So the client does not forward these. It PARSES them into typed, bounded
 * values and rebuilds the string itself from the parsed integers. What the
 * coordinator sends is data to be interpreted, never a fragment to be run, and
 * the only flags that can ever reach a siever are the ones in the table in
 * sieve_executor.c.
 *
 * An unrecognised flag fails rather than being dropped: dropping one silently
 * changes the sieve area, which is the same class of quiet wrongness as
 * running the wrong siever. The operator's own --siever-args / --gpu-args are
 * NOT put through this -- those are local intent and the escape hatch when a
 * campaign needs a flag this table does not know.
 *
 * `bad` receives the offending token for the error message; pass NULL to skip.
 */
#define SIEVE_ARGS_LASIEVE4  0
#define SIEVE_ARGS_CUDA      1

#define SIEVE_ARGS_OK        0
#define SIEVE_ARGS_UNKNOWN  -1   /* token is not in the table            */
#define SIEVE_ARGS_RANGE    -2   /* known flag, value outside its bounds */
#define SIEVE_ARGS_NOVALUE  -3   /* flag present with no value after it  */
#define SIEVE_ARGS_TOO_LONG -4   /* rebuilt string does not fit in `out` */

int sieve_sanitize_args(int vocab, const char *in,
                        char *out, size_t out_n,
                        char *bad, size_t bad_n);

const char *sieve_args_strerror(int rc);

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
