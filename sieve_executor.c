#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "sieve_executor.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

static int wait_status_to_rc(int status)
{
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

static void signal_child_group(pid_t pid, int sig)
{
    if (kill(-pid, sig) != 0 && errno == ESRCH) {
        (void)kill(pid, sig);
    }
}

/* Grace between SIGTERM and SIGKILL when a run is cancelled. cuda-sieve
 * answers SIGTERM by stopping at the next special-q and draining its cofactor
 * queue rather than dying, so this is how long that clean stop gets. */
#define CANCEL_TERM_GRACE_MS 2000

/* Hard cap on how long we wait for a SIGKILL'd group to disappear, so a member
 * left unreapable (a zombie under a pid 1 that does not reap, in a container)
 * cannot hang shutdown for ever. */
#define CANCEL_GROUP_REAP_CAP_MS 10000

/* Wait out the rest of the child's process GROUP after the leader is reaped.
 *
 * Reaping the leader is not the same as the run being over. `/bin/sh -c CMD`
 * on Debian and Ubuntu is dash, which does not exec CMD in place -- the shell
 * stays as the parent -- so the process we wait on above is the shell and the
 * siever is its child. On cancel the shell takes the default action for
 * SIGTERM and dies at once, while cuda-sieve catches SIGTERM and answers it
 * with a graceful drain (bench_stop_hook_install, cuda-sieve/bench/platform.c)
 * instead of dying. We would then reap the shell, report its 143 as the
 * siever's exit status, and return -- leaving the card sieving and printing
 * its band summary over the shell prompt after the client had exited, and
 * never sending the SIGKILL that was supposed to follow. The `exec` prefix in
 * run_child_cancelable removes the shell from the picture; this covers
 * anything the siever itself spawns. */
static void wait_group_gone(pid_t pgid, int64_t kill_deadline_ms)
{
    int64_t give_up_ms = 0;

    for (;;) {
        int64_t now = monotonic_ms();
        int past_grace = (now >= kill_deadline_ms);

        /* Probe and kill in the SAME syscall rather than testing liveness and
         * then signalling: POSIX keeps the pid reserved while the group has
         * members, so `-pgid` is unambiguous right up to the moment the group
         * empties, but a separate check-then-kill would still leave a window
         * where the group drains between the two calls and the SIGKILL lands
         * on whatever recycled the pid. One call has no such window -- either
         * the group was there and got the signal, or it was already gone.
         * Only ESRCH means gone; EPERM means alive and not ours to signal. */
        if (kill(-pgid, past_grace ? SIGKILL : 0) != 0 && errno == ESRCH)
            return;

        if (past_grace) {
            if (give_up_ms == 0) give_up_ms = now + CANCEL_GROUP_REAP_CAP_MS;
            else if (now >= give_up_ms) return;
        }

        usleep(100000);
    }
}

static int wait_child_cancelable(pid_t pid, sieve_cancel_fn should_cancel,
                                 void *cancel_ctx)
{
    int status = 0;
    int cancelling = 0;
    int sent_kill = 0;
    int64_t term_deadline_ms = 0;

    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            int rc = wait_status_to_rc(status);
            /* Only after a cancel: on the normal path the leader exiting is
             * the run finishing, and a lingering member would be something
             * the siever deliberately backgrounded, not ours to wait on. */
            if (cancelling) wait_group_gone(pid, term_deadline_ms);
            return rc;
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }

        if (!cancelling && should_cancel && should_cancel(cancel_ctx)) {
            cancelling = 1;
            term_deadline_ms = monotonic_ms() + CANCEL_TERM_GRACE_MS;
            signal_child_group(pid, SIGTERM);
        } else if (cancelling && !sent_kill &&
                   monotonic_ms() >= term_deadline_ms) {
            sent_kill = 1;
            signal_child_group(pid, SIGKILL);
        }

        usleep(100000);
    }
}

/* Run `syscmd` through /bin/sh in its own process group, polling
 * `should_cancel` while it runs. Shared by both engines: the process-group
 * isolation and the SIGTERM-then-SIGKILL cancel are engine-independent, and
 * only the command line differs. */
static int run_child_cancelable(const char *syscmd,
                                sieve_cancel_fn should_cancel,
                                void *cancel_ctx)
{
    sigset_t block;
    sigset_t oldmask;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);

    if (pthread_sigmask(SIG_BLOCK, &block, &oldmask) != 0) {
        return -1;
    }

    /* Prefix `exec` here, before the fork, so a long command line is a clean
     * allocation failure rather than a silent truncation that would degrade
     * back to running under a shell -- the exact bug this exists to prevent.
     * (malloc after fork() in a threaded process is not safe; before it is.) */
    size_t execlen = strlen(syscmd) + sizeof("exec ");
    char *execcmd = malloc(execlen);
    if (!execcmd) {
        pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
        errno = ENOMEM;
        return -1;
    }
    snprintf(execcmd, execlen, "exec %s", syscmd);

    pid_t pid = fork();
    if (pid < 0) {
        free(execcmd);
        int saved_errno = errno;
        pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
        errno = saved_errno;
        return -1;
    }

    if (pid == 0) {
        struct sigaction ign;
        struct sigaction dfl;

        (void)setpgid(0, 0);

        memset(&ign, 0, sizeof(ign));
        sigemptyset(&ign.sa_mask);
        ign.sa_handler = SIG_IGN;
        sigaction(SIGINT, &ign, NULL);
        sigaction(SIGTERM, &ign, NULL);

        sigprocmask(SIG_SETMASK, &oldmask, NULL);

        memset(&dfl, 0, sizeof(dfl));
        sigemptyset(&dfl.sa_mask);
        dfl.sa_handler = SIG_DFL;
        sigaction(SIGINT, &dfl, NULL);
        sigaction(SIGTERM, &dfl, NULL);

        /* `exec` so the siever replaces the shell instead of running as its
         * child: dash does not do this on its own for `-c`. Without it the
         * pid we wait on is the shell's, so the status we report is the
         * shell's rather than the siever's, and on cancel the shell dies of
         * the SIGTERM while the siever -- which may catch it -- carries on
         * unattended.
         *
         * This does narrow what a command line may be: `exec` takes a command
         * word, so an assignment prefix (`VAR=v prog`) or a second command
         * after `&&` no longer works. Every caller here formats a plain
         * `program args...`, which is unaffected. */
        execl("/bin/sh", "sh", "-c", execcmd, (char *)NULL);
        _exit(127);
    }

    (void)setpgid(pid, pid);
    pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
    free(execcmd);

    return wait_child_cancelable(pid, should_cancel, cancel_ctx);
}

int sieve_run_command(const char *syscmd,
                      sieve_cancel_fn should_cancel,
                      void *cancel_ctx)
{
    return run_child_cancelable(syscmd, should_cancel, cancel_ctx);
}

int sieve_run_local(const char *siever_path,
                    const char *job_infile,
                    const char *outfile,
                    uint32_t startq,
                    uint32_t qrange,
                    char side,
                    const char *extra_args,
                    sieve_cancel_fn should_cancel,
                    void *cancel_ctx)
{
    /* Discard any prior relation file at this path; the siever appends. */
    remove(outfile);

    char syscmd[1280];
    snprintf(syscmd, sizeof(syscmd),
        "%s -f %u -c %u -o %s -n 0 %s -%c %s",
        siever_path, startq, qrange, outfile,
        extra_args ? extra_args : "",
        side, job_infile);

    return run_child_cancelable(syscmd, should_cancel, cancel_ctx);
}

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
                   void *cancel_ctx)
{
    if (qrange == 0) return -1;

    /* Clear the finished-band marker and both staging artifacts. --restart
     * tells bench to discard them too, but doing it here as well means a
     * failed run can never leave a stale `outfile` that the caller would read
     * as "band completed". */
    remove(outfile);
    {
        char part[1152];
        snprintf(part, sizeof(part), "%s.part", outfile);
        remove(part);
        snprintf(part, sizeof(part), "%s.part.ckpt", outfile);
        remove(part);
    }

    /* THE off-by-one. A workunit is the half-open [startq, startq+qrange);
     * cuda-sieve's --qrange MIN:MAX is INCLUSIVE of MAX (sqgen_create in
     * bench/fbgen.c, pinned by the inclusive_single_prime case in
     * bench/sqgentest.c). Passing startq+qrange would sieve one q past the
     * band whenever that value is prime, and the server's verifier rejects
     * the whole file for it (verify.c relation_has_q_in_range) — which
     * requeues the workunit and eventually poisons it. qrange == 0 is
     * rejected above so this cannot underflow. */
    unsigned long qmax_inclusive = (unsigned long)startq + qrange - 1;

    /* cuda-sieve numbers sides: 1 = algebraic, 0 = rational. */
    int sq_side = (side == 'a') ? 1 : 0;

    char fb1[600] = "";
    if (fb1_path && *fb1_path)
        snprintf(fb1, sizeof(fb1), " --fb1 %s", fb1_path);

    char dev[32] = "";
    if (device >= 0)
        snprintf(dev, sizeof(dev), " --device %d", device);

    char syscmd[2048];
    snprintf(syscmd, sizeof(syscmd),
        "%s --pipeline --cofactor --poly %s --sq-side %d "
        "--qrange %lu:%lu --relations %s --restart%s%s %s",
        bench_path, job_infile, sq_side,
        (unsigned long)startq, qmax_inclusive, outfile,
        fb1, dev,
        extra_args ? extra_args : "");

    return run_child_cancelable(syscmd, should_cancel, cancel_ctx);
}
