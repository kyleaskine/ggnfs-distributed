/* verify_cli.c — standalone offline verifier.
 *
 * Reuses verify.c's streaming relation parser, q-range check, and GMP norm
 * residue check, and reads the per-workunit q-range plus the polynomial from
 * a (read-only) job.db snapshot pulled alongside the relation files.
 *
 * Usage:
 *   ggnfs-verify --jobdb=<path/to/job.db> [options] <file.dat[.zst]>...
 *
 * Options:
 *   --no-norm         Skip GMP norm check (parse + q-range only).
 *   --no-qrange       Skip per-workunit q-range check (parse [+ norm] only).
 *                     Useful for spot-checking files when you don't have the
 *                     matching job.db.
 *   --quiet           Suppress per-file PASS lines (FAILs still print).
 *
 * Exit status: 0 on success (even if some files fail validation — failures
 * go to stderr but do not change the exit code). Nonzero only on an I/O
 * error opening the DB or a config mistake.
 */
#define _POSIX_C_SOURCE 200809L

#include "verify.h"
#include "db.h"

#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *USAGE =
    "usage: ggnfs-verify --jobdb=<job.db> [--no-norm] [--no-qrange] [--quiet] <file>...\n";

/* Strip leading directory components and any trailing .dat / .dat.zst
 * suffix to recover the workunit id used to look up q_start/q_range/side
 * in job.db. Returns a malloc'd string the caller must free, or NULL on
 * malloc failure. */
static char *derive_workunit_id(const char *path)
{
    /* basename() can mutate its argument on some platforms (glibc's POSIX
     * variant is safe, but we strdup to be portable). */
    char *dup = strdup(path);
    if (!dup) return NULL;
    char *base = basename(dup);
    char *out = strdup(base);
    free(dup);
    if (!out) return NULL;

    size_t n = strlen(out);
    if (n > 4 && strcmp(out + n - 4, ".zst") == 0) {
        out[n - 4] = '\0';
        n -= 4;
    }
    if (n > 4 && strcmp(out + n - 4, ".dat") == 0) {
        out[n - 4] = '\0';
    }
    return out;
}

int main(int argc, char **argv)
{
    const char *jobdb_path = NULL;
    int  no_norm   = 0;
    int  no_qrange = 0;
    int  quiet     = 0;

    int  positional_start = argc;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--jobdb=", 8) == 0) {
            jobdb_path = a + 8;
        } else if (strcmp(a, "--no-norm") == 0) {
            no_norm = 1;
        } else if (strcmp(a, "--no-qrange") == 0) {
            no_qrange = 1;
        } else if (strcmp(a, "--quiet") == 0) {
            quiet = 1;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            fputs(USAGE, stdout);
            return 0;
        } else if (a[0] == '-' && a[1] == '-') {
            fprintf(stderr, "ggnfs-verify: unknown option: %s\n%s", a, USAGE);
            return 2;
        } else {
            positional_start = i;
            break;
        }
    }

    if (positional_start >= argc) {
        fprintf(stderr, "ggnfs-verify: no input files\n%s", USAGE);
        return 2;
    }
    if (!jobdb_path && (!no_norm || !no_qrange)) {
        fprintf(stderr,
                "ggnfs-verify: --jobdb required unless both --no-norm and --no-qrange given\n");
        return 2;
    }

    /* Open the DB. Even though sqlite3 will open a missing file by creating
     * it, we want to fail loudly here if the path is wrong — db_open will
     * run schema migrations, which is fine against a real job.db and only
     * creates an empty-but-valid DB if the file is missing. We check for
     * presence of the snapshot before opening. */
    ggnfs_db_t *db = NULL;
    verify_poly_t       poly_raw;       verify_poly_init(&poly_raw);
    verify_poly_gmp_t  *poly_gmp = NULL;

    if (jobdb_path) {
        FILE *probe = fopen(jobdb_path, "rb");
        if (!probe) {
            fprintf(stderr, "ggnfs-verify: cannot open jobdb %s: %s\n",
                    jobdb_path, strerror(errno));
            return 1;
        }
        fclose(probe);
        db = db_open(jobdb_path);
        if (!db) {
            fprintf(stderr, "ggnfs-verify: db_open(%s) failed\n", jobdb_path);
            return 1;
        }
        if (!no_norm) {
            if (verify_poly_load_from_meta(db, &poly_raw) != 0) {
                fprintf(stderr,
                        "ggnfs-verify: polynomial not found in jobdb meta; "
                        "either pass --no-norm or use a jobdb that was initialized "
                        "after the polynomial-in-meta feature landed\n");
                db_close(db);
                verify_poly_free(&poly_raw);
                return 1;
            }
            poly_gmp = verify_poly_gmp_new(&poly_raw);
            if (!poly_gmp) {
                fprintf(stderr, "ggnfs-verify: verify_poly_gmp_new failed\n");
                db_close(db);
                verify_poly_free(&poly_raw);
                return 1;
            }
        }
    }

    int total_files = 0, files_with_failures = 0;
    int64_t total_parsed = 0, total_failed = 0, total_qviol = 0, total_norm_fails = 0;

    for (int i = positional_start; i < argc; i++) {
        const char *path = argv[i];
        total_files++;

        verify_check_t  check_buf;
        verify_check_t *check_ptr = NULL;
        char            wuid[64]  = {0};

        if (!no_qrange) {
            char *wid = derive_workunit_id(path);
            if (!wid) {
                fprintf(stderr, "%s: oom deriving workunit id; skipping q-range\n", path);
            } else {
                snprintf(wuid, sizeof(wuid), "%s", wid);
                db_lease_result_t wu;
                int rc = db_workunit_get(db, wid, &wu);
                if (rc == 0) {
                    check_buf.q_start = wu.q_start;
                    check_buf.q_range = wu.q_range;
                    check_buf.side    = wu.side;
                    check_ptr = &check_buf;
                } else if (rc == 1) {
                    /* A GPU block's file is named after the BLOCK, so the id
                     * derived from the filename is not a workunit. Without
                     * this, every block file — the largest in the campaign —
                     * quietly loses its q-range check and still prints PASS. */
                    db_block_t blk;
                    int brc = db_block_get(db, wid, &blk);
                    if (brc == 0) {
                        check_buf.q_start = blk.q_start;
                        check_buf.q_range = blk.q_end - blk.q_start;
                        check_buf.side    = blk.side;
                        check_ptr = &check_buf;
                    } else {
                        fprintf(stderr,
                                "%s: id '%s' is neither a workunit nor a block in "
                                "jobdb; skipping q-range check for this file\n",
                                path, wid);
                    }
                } else {
                    fprintf(stderr,
                            "%s: db error looking up '%s'; skipping q-range check for this file\n",
                            path, wid);
                }
                free(wid);
            }
        }

        int64_t parsed = 0, failed = 0, qviol = 0, normfails = 0;
        char    reason[256] = {0};

        int rc = verify_parse_file_full(path, check_ptr, poly_gmp,
                                        &parsed, &failed, &qviol, &normfails,
                                        reason, sizeof(reason));
        if (rc != 0) {
            fprintf(stderr, "%s: I/O error, skipped\n", path);
            files_with_failures++;
            continue;
        }

        int any_fail = (failed > 0 || qviol > 0 || normfails > 0);
        if (any_fail) files_with_failures++;
        total_parsed     += parsed;
        total_failed     += failed;
        total_qviol      += qviol;
        total_norm_fails += normfails;

        if (any_fail) {
            fprintf(stderr,
                    "%s: FAIL parsed=%lld parse_fail=%lld qviol=%lld norm_fail=%lld (first: %s)\n",
                    path, (long long)parsed, (long long)failed,
                    (long long)qviol, (long long)normfails,
                    reason[0] ? reason : "?");
        } else if (!quiet) {
            printf("%s: PASS parsed=%lld%s%s\n",
                   path, (long long)parsed,
                   check_ptr ? "" : " (no-qrange)",
                   poly_gmp  ? "" : " (no-norm)");
        }
    }

    printf("---\n"
           "summary: %d file(s), %d with failures; "
           "parsed=%lld parse_fail=%lld qviol=%lld norm_fail=%lld\n",
           total_files, files_with_failures,
           (long long)total_parsed, (long long)total_failed,
           (long long)total_qviol, (long long)total_norm_fails);

    if (poly_gmp) verify_poly_gmp_free(poly_gmp);
    verify_poly_free(&poly_raw);
    if (db) db_close(db);
    return 0;
}
