#include "sieve_executor.h"

#include <stdio.h>
#include <stdlib.h>

int sieve_run_local(const char *siever_path,
                    const char *job_infile,
                    const char *outfile,
                    uint32_t startq,
                    uint32_t qrange,
                    char side)
{
    /* Discard any prior relation file at this path; the siever appends. */
    remove(outfile);

    char syscmd[1024];
    snprintf(syscmd, sizeof(syscmd),
        "%s -f %u -c %u -o %s -n 0 -%c %s",
        siever_path, startq, qrange, outfile, side, job_infile);

    return system(syscmd);
}
