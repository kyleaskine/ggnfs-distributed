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

static int wait_child_cancelable(pid_t pid, sieve_cancel_fn should_cancel,
                                 void *cancel_ctx)
{
    int status = 0;
    int cancelling = 0;
    int sent_kill = 0;
    int64_t term_deadline_ms = 0;

    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return wait_status_to_rc(status);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }

        if (!cancelling && should_cancel && should_cancel(cancel_ctx)) {
            cancelling = 1;
            term_deadline_ms = monotonic_ms() + 2000;
            signal_child_group(pid, SIGTERM);
        } else if (cancelling && !sent_kill &&
                   monotonic_ms() >= term_deadline_ms) {
            sent_kill = 1;
            signal_child_group(pid, SIGKILL);
        }

        usleep(100000);
    }
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

    sigset_t block;
    sigset_t oldmask;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);

    if (pthread_sigmask(SIG_BLOCK, &block, &oldmask) != 0) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
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

        execl("/bin/sh", "sh", "-c", syscmd, (char *)NULL);
        _exit(127);
    }

    (void)setpgid(pid, pid);
    pthread_sigmask(SIG_SETMASK, &oldmask, NULL);

    return wait_child_cancelable(pid, should_cancel, cancel_ctx);
}
