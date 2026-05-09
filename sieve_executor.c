#include "sieve_executor.h"

#include <stdio.h>
#include <stdlib.h>

/* Same buffer size YAFU uses for syscmd everywhere else. */
#ifndef SIEVE_EXEC_CMD_MAX
#define SIEVE_EXEC_CMD_MAX 1024
#endif

/* ---------- local executor: shell out to gnfs-lasieve4* via system() ----- */

static int local_run(sieve_executor_t *self, const sieve_request_t *req)
{
    char syscmd[SIEVE_EXEC_CMD_MAX];
    const char *batch3lp = req->batch_3lp ? "-d" : "";
    char side = req->side_is_algebraic ? 'a' : 'r';

    (void)self;

    /* Discard any prior relation file at this path; the siever appends. */
    remove(req->outfilename);

    snprintf(syscmd, SIEVE_EXEC_CMD_MAX,
        "%s%s -f %u -c %u -o %s -n %d %s -%c %s ",
        req->sievername,
        req->verbose > 1 ? " -v" : "",
        req->startq,
        req->qrange,
        req->outfilename,
        req->tindex,
        batch3lp,
        side,
        req->job_infile_name);

    if (req->verbose > 1) {
        printf("syscmd: %s\n", syscmd);
        fflush(stdout);
    }

    return system(syscmd);
}

static sieve_executor_t s_local_executor = {
    /* .run     = */ local_run,
    /* .destroy = */ NULL,   /* static singleton; nothing to free */
    /* .state   = */ NULL,
};

sieve_executor_t *sieve_executor_local_create(void)
{
    return &s_local_executor;
}

/* ---------- default executor singleton ---------------------------------- */

sieve_executor_t *sieve_executor_default(void)
{
    return &s_local_executor;
}

int sieve_executor_run(const sieve_request_t *req)
{
    sieve_executor_t *e = sieve_executor_default();
    if (e == NULL || e->run == NULL) {
        return -1;
    }
    return e->run(e, req);
}

void sieve_executor_destroy(sieve_executor_t *e)
{
    if (e == NULL) return;
    if (e->destroy != NULL) e->destroy(e);
}
