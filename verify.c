/* verify.c — relation-file / .job parsers + verifier thread; see verify.h.
 *
 * Verifier currently runs parse-pass only (count parseable lines, no GMP norm
 * math). Spot-check on K random relations comes when libgmp is wired in.
 */
#define _POSIX_C_SOURCE 200809L

#include "verify.h"
#include "db.h"

#include <zstd.h>

#include <errno.h>
#include <gmp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

typedef struct {
    FILE          *f;
    int            zstd;
    int            error;
    ZSTD_DStream  *ds;
    unsigned char *inbuf;
    unsigned char *outbuf;
    size_t         in_cap;
    size_t         out_cap;
    ZSTD_inBuffer  in;
    size_t         out_pos;
    size_t         out_len;
    size_t         remaining;
    int            eof;
} rel_reader_t;

static void rel_reader_close(rel_reader_t *r);

static int has_suffix(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

static int rel_reader_open(rel_reader_t *r, const char *path)
{
    memset(r, 0, sizeof(*r));
    r->f = fopen(path, "rb");
    if (!r->f) return -1;
    r->zstd = has_suffix(path, ".zst");
    if (!r->zstd) return 0;

    r->in_cap = ZSTD_DStreamInSize();
    r->out_cap = ZSTD_DStreamOutSize();
    r->inbuf = malloc(r->in_cap);
    r->outbuf = malloc(r->out_cap);
    r->ds = ZSTD_createDStream();
    if (!r->inbuf || !r->outbuf || !r->ds) {
        r->error = 1;
        rel_reader_close(r);
        errno = ENOMEM;
        return -1;
    }
    size_t rc = ZSTD_initDStream(r->ds);
    if (ZSTD_isError(rc)) {
        r->error = 1;
        rel_reader_close(r);
        errno = EIO;
        return -1;
    }
    r->in.src = r->inbuf;
    r->in.size = 0;
    r->in.pos = 0;
    return 0;
}

static int rel_reader_fill_zstd(rel_reader_t *r)
{
    r->out_pos = 0;
    r->out_len = 0;

    for (;;) {
        if (r->in.pos == r->in.size && !r->eof) {
            size_t n = fread(r->inbuf, 1, r->in_cap, r->f);
            if (n == 0) {
                if (ferror(r->f)) { r->error = 1; return -1; }
                r->eof = 1;
            }
            r->in.src = r->inbuf;
            r->in.size = n;
            r->in.pos = 0;
        }
        if (r->in.pos == r->in.size && r->eof) {
            if (r->remaining != 0) {
                r->error = 1;
                errno = EIO;
                return -1;
            }
            return 0;
        }

        ZSTD_outBuffer out = { r->outbuf, r->out_cap, 0 };
        size_t rc = ZSTD_decompressStream(r->ds, &out, &r->in);
        if (ZSTD_isError(rc)) {
            r->error = 1;
            errno = EIO;
            return -1;
        }
        r->remaining = rc;
        r->out_len = out.pos;
        if (r->out_len > 0) return 1;
        if (r->eof && rc != 0) {
            r->error = 1;
            errno = EIO;
            return -1;
        }
    }
}

static ssize_t rel_reader_getline(rel_reader_t *r, char **line, size_t *cap)
{
    if (!r->zstd) return getline(line, cap, r->f);

    size_t len = 0;
    for (;;) {
        if (r->out_pos >= r->out_len) {
            int fill = rel_reader_fill_zstd(r);
            if (fill < 0) return -1;
            if (fill == 0) {
                if (len == 0) return -1;
                if (len >= *cap) {
                    char *new_line = realloc(*line, len + 1);
                    if (!new_line) {
                        r->error = 1;
                        return -1;
                    }
                    *line = new_line;
                    *cap = len + 1;
                }
                (*line)[len] = '\0';
                return (ssize_t)len;
            }
        }

        char ch = (char)r->outbuf[r->out_pos++];
        if (len + 1 >= *cap) {
            size_t new_cap = *cap ? *cap * 2 : 256;
            char *new_line = realloc(*line, new_cap);
            if (!new_line) {
                r->error = 1;
                return -1;
            }
            *line = new_line;
            *cap = new_cap;
        }
        (*line)[len++] = ch;
        if (ch == '\n') {
            (*line)[len] = '\0';
            return (ssize_t)len;
        }
    }
}

static int rel_reader_error(const rel_reader_t *r)
{
    return r->error || (!r->zstd && ferror(r->f));
}

static void rel_reader_close(rel_reader_t *r)
{
    if (r->f) fclose(r->f);
    if (r->ds) ZSTD_freeDStream(r->ds);
    free(r->inbuf);
    free(r->outbuf);
    memset(r, 0, sizeof(*r));
}

/* ===================== primitive integer parsers ======================== */

/* All parsers take [s, end) explicitly — no NUL-termination assumed, since
 * relation lines are sliced out of a larger getline buffer. Returns 0 on
 * success and writes *out; returns -1 on malformed/overflow input. */

static int parse_decimal_int64(const char *s, const char *end, int64_t *out)
{
    if (s >= end) return -1;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; if (s >= end) return -1; }
    uint64_t v = 0;
    while (s < end) {
        if (*s < '0' || *s > '9') return -1;
        uint64_t nv = v * 10u + (uint64_t)(*s - '0');
        if (nv < v) return -1;   /* multiplicative overflow */
        v = nv;
        s++;
    }
    if (neg) {
        /* allow |v| up to 2^63 so INT64_MIN is representable */
        if (v > (uint64_t)INT64_MAX + 1u) return -1;
        /* compute via unsigned wrap so INT64_MIN doesn't trip signed UB */
        *out = (int64_t)(0u - v);
    } else {
        if (v > (uint64_t)INT64_MAX) return -1;
        *out = (int64_t)v;
    }
    return 0;
}

static int parse_decimal_uint64(const char *s, const char *end, uint64_t *out)
{
    if (s >= end) return -1;
    uint64_t v = 0;
    while (s < end) {
        if (*s < '0' || *s > '9') return -1;
        uint64_t nv = v * 10u + (uint64_t)(*s - '0');
        if (nv < v) return -1;
        v = nv;
        s++;
    }
    *out = v;
    return 0;
}

static int parse_hex_uint64(const char *s, const char *end, uint64_t *out)
{
    if (s >= end) return -1;
    uint64_t v = 0;
    while (s < end) {
        int d;
        char c = *s;
        if      (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
        else return -1;
        if (v > (UINT64_MAX >> 4)) return -1;
        v = (v << 4) | (uint64_t)d;
        s++;
    }
    *out = v;
    return 0;
}

/* Parse a possibly-empty comma-separated list of hex u64s in [start, end).
 * Rejects empty tokens (e.g. "a,,b") and trailing commas. Caps at `cap`
 * entries — returns -1 if exceeded so the caller can flag the submission. */
static int parse_hex_list(const char *start, const char *end,
                          uint64_t *out, int *n_out, int cap)
{
    int n = 0;
    if (start == end) { *n_out = 0; return 0; }
    const char *p = start;
    for (;;) {
        if (n >= cap) return -1;
        const char *comma = memchr(p, ',', (size_t)(end - p));
        const char *tok_end = comma ? comma : end;
        if (tok_end == p) return -1;                    /* empty token */
        if (parse_hex_uint64(p, tok_end, &out[n]) != 0) return -1;
        n++;
        if (!comma) break;
        p = comma + 1;
        if (p == end) return -1;                        /* trailing comma */
    }
    *n_out = n;
    return 0;
}

/* ===================== relation-line parser ============================ */

int verify_parse_line(const char *line, size_t len, verify_relation_t *out)
{
    if (!line || len == 0 || !out) return -1;
    const char *end = line + len;

    const char *comma1 = memchr(line, ',', len);
    if (!comma1) return -1;
    const char *colon1 = memchr(comma1 + 1, ':', (size_t)(end - (comma1 + 1)));
    if (!colon1) return -1;
    const char *colon2 = memchr(colon1 + 1, ':', (size_t)(end - (colon1 + 1)));
    if (!colon2) return -1;

    int64_t  a;
    uint64_t b;
    if (parse_decimal_int64 (line,       comma1, &a) != 0) return -1;
    if (parse_decimal_uint64(comma1 + 1, colon1, &b) != 0) return -1;
    if (b == 0) return -1;        /* free-relation shape; not real siever output */

    int n_r = 0, n_a = 0;
    if (parse_hex_list(colon1 + 1, colon2, out->rprimes, &n_r,
                       VERIFY_MAX_PRIMES_PER_SIDE) != 0) return -1;
    if (parse_hex_list(colon2 + 1, end,    out->aprimes, &n_a,
                       VERIFY_MAX_PRIMES_PER_SIDE) != 0) return -1;

    out->a = a;
    out->b = b;
    out->n_rprimes = n_r;
    out->n_aprimes = n_a;
    return 0;
}

/* ===================== relation-file streamer ========================== */

static size_t rstrip_eol(char *line, size_t len)
{
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        len--;
    return len;
}

/* Is any sieved-side prime in [q_start, q_start+q_range)? When `out_q` is
 * non-NULL the SMALLEST such prime is returned through it, which costs a full
 * scan instead of an early exit — so callers that only need the yes/no (every
 * ordinary submission) pass NULL and keep the fast path. */
static int relation_q_in_range(const verify_relation_t *rel,
                               char side, int64_t q_start, int64_t q_range,
                               uint64_t *out_q)
{
    /* The special-q is *one of* the primes on the sieved side. We don't know
     * which one a priori (it's typically the largest, but lasieve4 doesn't
     * guarantee ordering), so accept the relation if any prime is in range. */
    const uint64_t *primes;
    int n;
    if (side == 'a') { primes = rel->aprimes; n = rel->n_aprimes; }
    else             { primes = rel->rprimes; n = rel->n_rprimes; }

    uint64_t q_lo = (uint64_t)q_start;
    uint64_t q_hi = q_lo + (uint64_t)q_range;   /* exclusive */
    uint64_t best = 0;
    int      found = 0;
    for (int i = 0; i < n; i++) {
        if (primes[i] >= q_lo && primes[i] < q_hi) {
            if (!out_q) return 1;
            if (!found || primes[i] < best) { best = primes[i]; found = 1; }
        }
    }
    if (found && out_q) *out_q = best;
    return found;
}

static int relation_has_q_in_range(const verify_relation_t *rel,
                                   char side, int64_t q_start, int64_t q_range)
{
    return relation_q_in_range(rel, side, q_start, q_range, NULL);
}

/* Index of the member containing q, or -1. bounds[] is ascending with
 * n_members+1 entries, so this is an ordinary upper-bound search. */
static int coverage_bucket(const verify_coverage_t *cov, uint64_t q)
{
    int lo = 0, hi = cov->n_members;      /* answer in [lo, hi) */
    if (cov->n_members <= 0) return -1;
    if ((int64_t)q <  cov->bounds[0])          return -1;
    if ((int64_t)q >= cov->bounds[cov->n_members]) return -1;
    while (hi - lo > 1) {
        int mid = lo + (hi - lo) / 2;
        if ((int64_t)q >= cov->bounds[mid]) lo = mid; else hi = mid;
    }
    return lo;
}

int verify_parse_file_check(const char *path,
                            const verify_check_t *check,
                            verify_reservoir_t *reservoir,
                            verify_coverage_t  *coverage,
                            int64_t *out_parsed,
                            int64_t *out_failed,
                            int64_t *out_q_violations,
                            char    *out_first_reason,
                            size_t   reason_buflen)
{
    rel_reader_t reader;
    if (rel_reader_open(&reader, path) != 0) {
        fprintf(stderr, "verify_parse_file_check: open %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (out_first_reason && reason_buflen > 0) out_first_reason[0] = '\0';
    if (reservoir) { reservoir->count = 0; reservoir->sampled_from = 0; }

    char   *line   = NULL;
    size_t  cap    = 0;
    int64_t parsed = 0, failed = 0, qviol = 0;
    int64_t lineno = 0;
    int     check_q = (check && (check->side == 'a' || check->side == 'r'));
    ssize_t n;

    while ((n = rel_reader_getline(&reader, &line, &cap)) > 0) {
        lineno++;
        size_t len = rstrip_eol(line, (size_t)n);
        if (len == 0) continue;

        verify_relation_t rel;
        if (verify_parse_line(line, len, &rel) != 0) {
            failed++;
            if (out_first_reason && reason_buflen > 0 && out_first_reason[0] == '\0') {
                snprintf(out_first_reason, reason_buflen,
                         "parse error on line %lld", (long long)lineno);
            }
            continue;
        }
        uint64_t qmatch  = 0;
        int      want_q   = (coverage && coverage->n_members > 0);
        int      in_range = check_q
            ? relation_q_in_range(&rel, check->side, check->q_start,
                                  check->q_range, want_q ? &qmatch : NULL)
            : 0;
        if (check_q && !in_range) {
            qviol++;
            if (out_first_reason && reason_buflen > 0 && out_first_reason[0] == '\0') {
                snprintf(out_first_reason, reason_buflen,
                         "line %lld: no prime on side '%c' in [%lld,%lld)",
                         (long long)lineno, check->side,
                         (long long)check->q_start,
                         (long long)(check->q_start + check->q_range));
            }
            continue;
        }
        parsed++;

        if (want_q && in_range) {
            int m = coverage_bucket(coverage, qmatch);
            if (m >= 0) coverage->counts[m]++;
        }

        /* Algorithm R: each accepted relation has cap/sampled_from probability
         * of replacing a uniformly-chosen slot, giving a uniform random sample
         * over the file in a single streaming pass. */
        if (reservoir && reservoir->cap > 0) {
            reservoir->sampled_from++;
            if (reservoir->count < reservoir->cap) {
                reservoir->buf[reservoir->count++] = rel;
            } else {
                int r = (int)((unsigned)rand_r(&reservoir->seed)
                              % (unsigned)reservoir->sampled_from);
                if (r < reservoir->cap) reservoir->buf[r] = rel;
            }
        }
    }
    int read_error = rel_reader_error(&reader);
    free(line);
    rel_reader_close(&reader);
    if (read_error) {
        fprintf(stderr, "verify_parse_file_check: read %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    if (out_parsed)        *out_parsed        = parsed;
    if (out_failed)        *out_failed        = failed;
    if (out_q_violations)  *out_q_violations  = qviol;
    return 0;
}

int verify_parse_file(const char *path, int64_t *out_parsed, int64_t *out_failed)
{
    return verify_parse_file_check(path, NULL, NULL, NULL,
                                   out_parsed, out_failed, NULL, NULL, 0);
}

/* Forward decls — defined further down with the GMP norm code. */
static void compute_rational_norm(mpz_t out, int64_t a, uint64_t b,
                                  const verify_poly_gmp_t *p);
static void compute_algebraic_norm(mpz_t out, int64_t a, uint64_t b,
                                   const verify_poly_gmp_t *p);
static int  residue_ok(mpz_t norm, const uint64_t *primes, int n);

int verify_parse_file_full(const char *path,
                           const verify_check_t *check,
                           const verify_poly_gmp_t *poly,
                           int64_t *out_parsed,
                           int64_t *out_failed,
                           int64_t *out_q_violations,
                           int64_t *out_norm_failures,
                           char    *out_first_reason,
                           size_t   reason_buflen)
{
    rel_reader_t reader;
    if (rel_reader_open(&reader, path) != 0) {
        fprintf(stderr, "verify_parse_file_full: open %s: %s\n",
                path, strerror(errno));
        return -1;
    }
    if (out_first_reason && reason_buflen > 0) out_first_reason[0] = '\0';

    char   *line      = NULL;
    size_t  cap       = 0;
    int64_t parsed    = 0, failed = 0, qviol = 0, normfails = 0;
    int64_t lineno    = 0;
    int     check_q   = (check && (check->side == 'a' || check->side == 'r'));
    int     do_norm   = (poly != NULL);
    ssize_t n;

    /* One scratch mpz reused across all relations — verify_spotcheck has its
     * own; we keep ours local so init/clear cost is paid once per file, not
     * once per relation. */
    mpz_t norm;
    if (do_norm) mpz_init(norm);

    while ((n = rel_reader_getline(&reader, &line, &cap)) > 0) {
        lineno++;
        size_t len = rstrip_eol(line, (size_t)n);
        if (len == 0) continue;

        verify_relation_t rel;
        if (verify_parse_line(line, len, &rel) != 0) {
            failed++;
            if (out_first_reason && reason_buflen > 0 && out_first_reason[0] == '\0') {
                snprintf(out_first_reason, reason_buflen,
                         "parse error on line %lld", (long long)lineno);
            }
            continue;
        }
        if (check_q && !relation_has_q_in_range(&rel, check->side,
                                                check->q_start, check->q_range)) {
            qviol++;
            if (out_first_reason && reason_buflen > 0 && out_first_reason[0] == '\0') {
                snprintf(out_first_reason, reason_buflen,
                         "line %lld: no prime on side '%c' in [%lld,%lld)",
                         (long long)lineno, check->side,
                         (long long)check->q_start,
                         (long long)(check->q_start + check->q_range));
            }
            continue;
        }
        parsed++;

        if (do_norm) {
            int relfail = 0;
            compute_rational_norm(norm, rel.a, rel.b, poly);
            if (!residue_ok(norm, rel.rprimes, rel.n_rprimes)) {
                relfail = 1;
                if (out_first_reason && reason_buflen > 0 && out_first_reason[0] == '\0') {
                    char *rs = mpz_get_str(NULL, 10, norm);
                    snprintf(out_first_reason, reason_buflen,
                             "line %lld (%lld,%llu): rational residue %s is composite",
                             (long long)lineno, (long long)rel.a,
                             (unsigned long long)rel.b, rs ? rs : "?");
                    free(rs);
                }
            } else {
                compute_algebraic_norm(norm, rel.a, rel.b, poly);
                if (!residue_ok(norm, rel.aprimes, rel.n_aprimes)) {
                    relfail = 1;
                    if (out_first_reason && reason_buflen > 0 && out_first_reason[0] == '\0') {
                        char *rs = mpz_get_str(NULL, 10, norm);
                        snprintf(out_first_reason, reason_buflen,
                                 "line %lld (%lld,%llu): algebraic residue %s is composite",
                                 (long long)lineno, (long long)rel.a,
                                 (unsigned long long)rel.b, rs ? rs : "?");
                        free(rs);
                    }
                }
            }
            if (relfail) normfails++;
        }
    }

    if (do_norm) mpz_clear(norm);

    int read_error = rel_reader_error(&reader);
    free(line);
    rel_reader_close(&reader);
    if (read_error) {
        fprintf(stderr, "verify_parse_file_full: read %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    if (out_parsed)        *out_parsed         = parsed;
    if (out_failed)        *out_failed         = failed;
    if (out_q_violations)  *out_q_violations   = qviol;
    if (out_norm_failures) *out_norm_failures  = normfails;
    return 0;
}

/* ===================== polynomial / .job parser ======================== */

void verify_poly_init(verify_poly_t *p)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
}

void verify_poly_free(verify_poly_t *p)
{
    if (!p) return;
    for (int k = 0; k <= VERIFY_MAX_POLY_DEGREE; k++) {
        free(p->c[k]);
        p->c[k] = NULL;
    }
    free(p->Y0); p->Y0 = NULL;
    free(p->Y1); p->Y1 = NULL;
    p->degree = 0;
}

static char *dup_range(const char *s, size_t n)
{
    char *r = malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

/* Validate the parsed strings as decimal integers, drop a zero leading
 * coefficient (it would inflate the degree and put a spurious factor of b in
 * every algebraic norm), and -- when the .job carries an `n:` line -- check the
 * standard NFS invariant that f and g share a root mod n:
 *
 *     sum_k c_k * (-Y0)^k * Y1^(d-k) === 0 (mod n)
 *
 * That congruence is what makes omitting zero coefficients safe. Presence
 * alone cannot tell a coefficient the operator legitimately left out (0) from
 * one a truncated or mangled line dropped; the congruence accepts the first
 * and rejects the second with overwhelming probability. Failing here costs the
 * operator one `init`; failing to check costs a whole sieve job, since a wrong
 * polynomial poisons every workunit one norm spot-check at a time.
 * Returns 0 on success, -1 with a message on stderr otherwise. */
static int poly_finalize_and_check(const char *path, verify_poly_t *p,
                                   const char *n_str)
{
    int rc = -1;
    mpz_t c[VERIFY_MAX_POLY_DEGREE + 1], Y0, Y1, n, acc, term, xp, zp;

    for (int k = 0; k <= VERIFY_MAX_POLY_DEGREE; k++) mpz_init(c[k]);
    mpz_inits(Y0, Y1, n, acc, term, xp, zp, NULL);

    for (int k = 0; k <= p->degree; k++) {
        if (mpz_set_str(c[k], p->c[k], 10) != 0) {
            fprintf(stderr, "verify_parse_job_file: %s: c%d is not a decimal "
                    "integer: \"%s\"\n", path, k, p->c[k]);
            goto done;
        }
    }
    if (mpz_set_str(Y0, p->Y0, 10) != 0 || mpz_set_str(Y1, p->Y1, 10) != 0) {
        fprintf(stderr, "verify_parse_job_file: %s: Y0/Y1 is not a decimal "
                "integer\n", path);
        goto done;
    }
    if (mpz_sgn(Y1) == 0) {
        fprintf(stderr, "verify_parse_job_file: %s: Y1 is zero\n", path);
        goto done;
    }

    while (p->degree >= 1 && mpz_sgn(c[p->degree]) == 0) p->degree--;
    if (p->degree < 1) {
        fprintf(stderr, "verify_parse_job_file: %s: all coefficients above c0 "
                "are zero\n", path);
        goto done;
    }

    if (!n_str) {                      /* pre-existing jobs may omit `n:` */
        rc = 0;
        goto done;
    }
    if (mpz_set_str(n, n_str, 10) != 0 || mpz_cmp_ui(n, 1) <= 0) {
        fprintf(stderr, "verify_parse_job_file: %s: n is not an integer > 1\n",
                path);
        goto done;
    }

    mpz_neg(term, Y0);                 /* term = -Y0, the shared root */
    mpz_set_ui(xp, 1);                 /* xp walks (-Y0)^k             */
    mpz_set_ui(acc, 0);
    for (int k = 0; k <= p->degree; k++) {
        mpz_pow_ui(zp, Y1, (unsigned long)(p->degree - k));
        mpz_mul(zp, zp, xp);
        mpz_addmul(acc, c[k], zp);
        mpz_mul(xp, xp, term);
    }
    mpz_mod(acc, acc, n);
    if (mpz_sgn(acc) != 0) {
        fprintf(stderr, "verify_parse_job_file: %s: polynomial does not have a "
                "root mod n (f and g disagree) -- wrong .job for this number, "
                "or a coefficient line is missing or mangled\n", path);
        goto done;
    }
    rc = 0;

done:
    for (int k = 0; k <= VERIFY_MAX_POLY_DEGREE; k++) mpz_clear(c[k]);
    mpz_clears(Y0, Y1, n, acc, term, xp, zp, NULL);
    return rc;
}

int verify_parse_job_file(const char *path, verify_poly_t *out)
{
    verify_poly_init(out);

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "verify_parse_job_file: open %s: %s\n", path, strerror(errno));
        return -1;
    }

    char   *line = NULL;
    size_t  cap  = 0;
    ssize_t n;
    int     max_c = -1;
    int     ok    = 1;
    char   *n_str = NULL;

    while ((n = getline(&line, &cap, f)) > 0) {
        size_t len = rstrip_eol(line, (size_t)n);
        /* trim trailing spaces/tabs */
        while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) len--;
        if (len == 0) continue;

        /* skip leading whitespace */
        size_t i = 0;
        while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
        if (i >= len) continue;
        if (line[i] == '#') continue;

        /* split on first ':' */
        const char *colon = memchr(line + i, ':', len - i);
        if (!colon) continue;

        size_t key_end = (size_t)(colon - line);
        while (key_end > i && (line[key_end - 1] == ' ' || line[key_end - 1] == '\t'))
            key_end--;
        size_t key_len = key_end - i;

        size_t v_start = (size_t)(colon - line) + 1;
        while (v_start < len && (line[v_start] == ' ' || line[v_start] == '\t')) v_start++;
        if (v_start >= len) continue;
        size_t v_len = len - v_start;

        const char *key = line + i;

        /* c<digit> */
        if (key_len == 2 && key[0] == 'c' && key[1] >= '0' && key[1] <= '9') {
            int k = key[1] - '0';
            if (k > VERIFY_MAX_POLY_DEGREE) {
                fprintf(stderr, "verify_parse_job_file: c%d exceeds max degree %d\n",
                        k, VERIFY_MAX_POLY_DEGREE);
                ok = 0; break;
            }
            free(out->c[k]);
            out->c[k] = dup_range(line + v_start, v_len);
            if (!out->c[k]) { ok = 0; break; }
            if (k > max_c) max_c = k;
        }
        else if (key_len == 2 && key[0] == 'Y' && (key[1] == '0' || key[1] == '1')) {
            char **slot = (key[1] == '0') ? &out->Y0 : &out->Y1;
            free(*slot);
            *slot = dup_range(line + v_start, v_len);
            if (!*slot) { ok = 0; break; }
        }
        else if (key_len == 1 && key[0] == 'n') {
            free(n_str);
            n_str = dup_range(line + v_start, v_len);
            if (!n_str) { ok = 0; break; }
        }
        /* all other keys (skew, rlim, alim, *lpb*, *mfb*, *lambda, lss) ignored */
    }
    free(line);
    if (ok && ferror(f)) {
        fprintf(stderr, "verify_parse_job_file: read %s: %s\n",
                path, strerror(errno));
        ok = 0;
    }
    fclose(f);

    if (!ok) { free(n_str); verify_poly_free(out); return -1; }

    if (max_c < 1) {
        fprintf(stderr, "verify_parse_job_file: %s: no c1 or higher coefficient "
                "found\n", path);
        free(n_str); verify_poly_free(out);
        return -1;
    }
    /* Sparse polynomials are conventionally written with the zero coefficients
     * omitted -- an SNFS sextic given as just c6/c3/c0, say. Every siever and
     * filtering tool reads that shape, so fill the gaps with 0 rather than
     * rejecting the job. */
    for (int k = 0; k <= max_c; k++) {
        if (!out->c[k]) {
            out->c[k] = strdup("0");
            if (!out->c[k]) { free(n_str); verify_poly_free(out); return -1; }
        }
    }
    if (!out->Y0 || !out->Y1) {
        fprintf(stderr, "verify_parse_job_file: %s: missing Y0/Y1\n", path);
        free(n_str); verify_poly_free(out);
        return -1;
    }
    out->degree = max_c;

    /* Everything above is presence-only. This is what checks the values --
     * and, with `n:`, that they are the right values for this number. */
    if (poly_finalize_and_check(path, out, n_str) != 0) {
        free(n_str); verify_poly_free(out);
        return -1;
    }
    free(n_str);
    return 0;
}

/* ===================== meta save/load =================================== */

int verify_poly_save_to_meta(ggnfs_db_t *db, const verify_poly_t *p)
{
    if (!db || !p || p->degree < 1) return -1;

    char buf[32];
    snprintf(buf, sizeof(buf), "%d", p->degree);
    if (db_meta_set(db, "poly_degree", buf) != 0) return -1;

    for (int k = 0; k <= p->degree; k++) {
        if (!p->c[k]) return -1;
        char key[16];
        snprintf(key, sizeof(key), "poly_c%d", k);
        if (db_meta_set(db, key, p->c[k]) != 0) return -1;
    }
    if (!p->Y0 || !p->Y1) return -1;
    if (db_meta_set(db, "poly_Y0", p->Y0) != 0) return -1;
    if (db_meta_set(db, "poly_Y1", p->Y1) != 0) return -1;
    return 0;
}

int verify_poly_load_from_meta(ggnfs_db_t *db, verify_poly_t *out)
{
    verify_poly_init(out);

    char *deg_s = db_meta_get(db, "poly_degree");
    if (!deg_s) return -1;
    int deg = atoi(deg_s);
    free(deg_s);
    if (deg < 1 || deg > VERIFY_MAX_POLY_DEGREE) return -1;

    for (int k = 0; k <= deg; k++) {
        char key[16];
        snprintf(key, sizeof(key), "poly_c%d", k);
        out->c[k] = db_meta_get(db, key);
        if (!out->c[k]) { verify_poly_free(out); return -1; }
    }
    out->Y0 = db_meta_get(db, "poly_Y0");
    out->Y1 = db_meta_get(db, "poly_Y1");
    if (!out->Y0 || !out->Y1) { verify_poly_free(out); return -1; }
    out->degree = deg;
    return 0;
}

/* ===================== GMP poly + norm spot-check ====================== */

struct verify_poly_gmp_s {
    int    degree;
    mpz_t  c[VERIFY_MAX_POLY_DEGREE + 1];
    mpz_t  Y0;
    mpz_t  Y1;
};

verify_poly_gmp_t *verify_poly_gmp_new(const verify_poly_t *src)
{
    if (!src || src->degree < 1) return NULL;
    verify_poly_gmp_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->degree = src->degree;
    /* Init every slot so verify_poly_gmp_free can clear them unconditionally
     * even if a mid-loop parse fails. */
    for (int k = 0; k <= VERIFY_MAX_POLY_DEGREE; k++) mpz_init(p->c[k]);
    mpz_init(p->Y0); mpz_init(p->Y1);

    for (int k = 0; k <= src->degree; k++) {
        if (!src->c[k] || mpz_set_str(p->c[k], src->c[k], 10) != 0) {
            fprintf(stderr, "verify_poly_gmp_new: bad c%d\n", k);
            verify_poly_gmp_free(p); return NULL;
        }
    }
    if (!src->Y0 || !src->Y1 ||
        mpz_set_str(p->Y0, src->Y0, 10) != 0 ||
        mpz_set_str(p->Y1, src->Y1, 10) != 0) {
        fprintf(stderr, "verify_poly_gmp_new: bad Y0/Y1\n");
        verify_poly_gmp_free(p); return NULL;
    }
    return p;
}

void verify_poly_gmp_free(verify_poly_gmp_t *p)
{
    if (!p) return;
    for (int k = 0; k <= VERIFY_MAX_POLY_DEGREE; k++) mpz_clear(p->c[k]);
    mpz_clear(p->Y0); mpz_clear(p->Y1);
    free(p);
}

/* Primes <= 1000 (168 entries). msieve's nfs_read_relation trial-divides each
 * residue by these after dividing by the listed primes — small primes the
 * siever didn't bother to list. Same convention here. */
static const unsigned long SMALL_PRIMES[] = {
      2,   3,   5,   7,  11,  13,  17,  19,  23,  29,
     31,  37,  41,  43,  47,  53,  59,  61,  67,  71,
     73,  79,  83,  89,  97, 101, 103, 107, 109, 113,
    127, 131, 137, 139, 149, 151, 157, 163, 167, 173,
    179, 181, 191, 193, 197, 199, 211, 223, 227, 229,
    233, 239, 241, 251, 257, 263, 269, 271, 277, 281,
    283, 293, 307, 311, 313, 317, 331, 337, 347, 349,
    353, 359, 367, 373, 379, 383, 389, 397, 401, 409,
    419, 421, 431, 433, 439, 443, 449, 457, 461, 463,
    467, 479, 487, 491, 499, 503, 509, 521, 523, 541,
    547, 557, 563, 569, 571, 577, 587, 593, 599, 601,
    607, 613, 617, 619, 631, 641, 643, 647, 653, 659,
    661, 673, 677, 683, 691, 701, 709, 719, 727, 733,
    739, 743, 751, 757, 761, 769, 773, 787, 797, 809,
    811, 821, 823, 827, 829, 839, 853, 857, 859, 863,
    877, 881, 883, 887, 907, 911, 919, 929, 937, 941,
    947, 953, 967, 971, 977, 983, 991, 997
};
#define N_SMALL_PRIMES (sizeof(SMALL_PRIMES) / sizeof(SMALL_PRIMES[0]))

/* N_R = a*Y1 + b*Y0  (b * g(a/b) for rational poly g(x) = Y1*x + Y0). */
static void compute_rational_norm(mpz_t out, int64_t a, uint64_t b,
                                  const verify_poly_gmp_t *p)
{
    mpz_t tmp; mpz_init(tmp);
    mpz_mul_si(out, p->Y1, (long)a);
    mpz_mul_ui(tmp, p->Y0, (unsigned long)b);
    mpz_add(out, out, tmp);
    mpz_clear(tmp);
}

/* N_A = sum c_k * a^k * b^(d-k)  (homogenized eval of algebraic poly). */
static void compute_algebraic_norm(mpz_t out, int64_t a, uint64_t b,
                                   const verify_poly_gmp_t *p)
{
    mpz_t term, apow, bpow;
    mpz_init(term); mpz_init(apow); mpz_init(bpow);

    /* apow = a^0 = 1; bpow = b^d. */
    mpz_set_ui(apow, 1);
    mpz_set_ui(bpow, 1);
    for (int i = 0; i < p->degree; i++)
        mpz_mul_ui(bpow, bpow, (unsigned long)b);

    mpz_set_ui(out, 0);
    for (int k = 0; k <= p->degree; k++) {
        mpz_mul(term, p->c[k], apow);
        mpz_mul(term, term, bpow);
        mpz_add(out, out, term);
        if (k < p->degree) {
            mpz_mul_si    (apow, apow, (long)a);
            mpz_divexact_ui(bpow, bpow, (unsigned long)b);  /* b != 0 enforced by parser */
        }
    }
    mpz_clear(term); mpz_clear(apow); mpz_clear(bpow);
}

/* Reduce |norm| by dividing out each listed prime (all multiplicities), then
 * by every prime <= 1000. Returns 1 if remaining residue is 1 or probable-
 * prime (the relation is acceptable); 0 if it's composite > 1 (broken). */
static int residue_ok(mpz_t norm, const uint64_t *primes, int n)
{
    /* A zero norm is never legitimate -- an irreducible f has no rational
     * root, so it means the relation or the polynomial is broken. Bail before
     * the division loops: GMP reports 0 divisible by every p and divexact
     * leaves it 0, so they would spin forever. */
    if (mpz_sgn(norm) == 0) return 0;
    mpz_abs(norm, norm);
    for (int i = 0; i < n; i++) {
        unsigned long p = (unsigned long)primes[i];
        if (p < 2) continue;
        while (mpz_divisible_ui_p(norm, p))
            mpz_divexact_ui(norm, norm, p);
    }
    for (size_t i = 0; i < N_SMALL_PRIMES; i++) {
        unsigned long p = SMALL_PRIMES[i];
        while (mpz_divisible_ui_p(norm, p))
            mpz_divexact_ui(norm, norm, p);
    }
    if (mpz_cmp_ui(norm, 1) == 0) return 1;
    /* probab_prime returns 2 (definitely prime), 1 (probably), 0 (composite) */
    if (mpz_probab_prime_p(norm, 15) > 0) return 1;
    return 0;
}

int verify_spotcheck(const verify_poly_gmp_t *p,
                     const verify_relation_t *rels, int n,
                     char *out_first_reason, size_t reason_buflen)
{
    if (!p || !rels || n <= 0) return 0;
    if (out_first_reason && reason_buflen > 0) out_first_reason[0] = '\0';

    int fails = 0;
    mpz_t norm; mpz_init(norm);

    for (int i = 0; i < n; i++) {
        const verify_relation_t *r = &rels[i];

        compute_rational_norm(norm, r->a, r->b, p);
        if (!residue_ok(norm, r->rprimes, r->n_rprimes)) {
            fails++;
            if (fails == 1 && out_first_reason && reason_buflen > 0) {
                char *rs = mpz_get_str(NULL, 10, norm);
                snprintf(out_first_reason, reason_buflen,
                         "(%lld,%llu): rational residue %s is composite",
                         (long long)r->a, (unsigned long long)r->b,
                         rs ? rs : "?");
                free(rs);
            }
            continue;
        }

        compute_algebraic_norm(norm, r->a, r->b, p);
        if (!residue_ok(norm, r->aprimes, r->n_aprimes)) {
            fails++;
            if (fails == 1 && out_first_reason && reason_buflen > 0) {
                char *rs = mpz_get_str(NULL, 10, norm);
                snprintf(out_first_reason, reason_buflen,
                         "(%lld,%llu): algebraic residue %s is composite",
                         (long long)r->a, (unsigned long long)r->b,
                         rs ? rs : "?");
                free(rs);
            }
        }
    }

    mpz_clear(norm);
    return fails;
}

/* ===================== verifier thread ================================== */

/* Forward-decl ggnfs_db_t opener via db.h (already included). */

struct verify_thread_s {
    pthread_t       th;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    int             wake_requested;
    int             stop;
    char           *db_path;       /* malloc'd */
    int64_t         max_attempts;
    int             spotcheck_k;   /* 0 disables norm spot-check */
    int64_t         base_q_range;  /* narrowest band in the job; 0 = no scaling */
};

/* A GPU-class workunit covers ~100x the q-width of a CPU-class one and yields
 * proportionally more relations, so a fixed K would sample it ~100x more
 * thinly. Scale K by q_range / base_q_range so sample *density* is what stays
 * constant, and cap the multiplier so a pathological q_range can't ask for an
 * unbounded reservoir.
 *
 * base_q_range is the job's most common band width, read off the workunits table
 * at the top of every drain pass rather than from a config value. That keeps it
 * right on a campaign whose meta predates all of this, and it keeps working
 * when a later extend introduces a narrower band.
 *
 * The cap must stay ABOVE --block-width-multiple (default 50) or it stops being
 * a guard against a pathological q_range and starts clipping normal operation.
 * At 32 it did exactly that: the first production block was 50x base and got
 * k=1600 instead of 2500, sampling 1.16% of its 138,411 relations where a CPU
 * workunit samples 1.81% — 64% of the density, the 32/50 ratio, on precisely
 * the submissions carrying the most work. 64 restores parity at 50x and leaves
 * headroom; the cost is ~900 more GMP norm checks against a ~14 minute band. */
#define SPOTCHECK_MAX_SCALE VERIFY_SPOTCHECK_MAX_SCALE
/* Hard ceiling on the reservoir itself. Bounds both memory and the GMP work
 * per submission regardless of what --spotcheck-k and the scale multiply out
 * to; 1e6 relations is far past any useful sample. */
#define SPOTCHECK_K_MAX 1000000

static int spotcheck_k_for(int base_k, int64_t base_q_range, int64_t q_range)
{
    if (base_k <= 0) return 0;
    if (base_q_range <= 0 || q_range <= base_q_range) return base_k;
    int64_t scale = q_range / base_q_range;
    if (scale > SPOTCHECK_MAX_SCALE) scale = SPOTCHECK_MAX_SCALE;
    /* int64 product, then clamp: (int)(base_k * scale) can overflow for a
     * large --spotcheck-k, and the overflow is silent in the worst possible
     * way — a negative rescap makes both `rescap > resalloc` and `rescap > 0`
     * false, so the reservoir is never allocated, the spot-check is skipped
     * entirely, and every submission passes on parse + q-range alone with
     * nothing in the log saying verification was weakened. */
    int64_t k = (int64_t)base_k * scale;
    if (k > SPOTCHECK_K_MAX) k = SPOTCHECK_K_MAX;
    return (int)k;
}

/* Drain every pending submission; returns when the queue is empty (or on
 * a DB error after logging it). `poly` may be NULL — in that case the norm
 * spot-check is silently skipped (parse + q-range only). */
/* One resolution point for both submission shapes, so every early-exit in the
 * drain loop below stays a one-liner and no failure path can forget that a
 * block has members. Ordinary submissions keep exactly their old behaviour. */
static void resolve_fail(ggnfs_db_t *db, const db_pending_t *p,
                         const char *reason, int64_t max_attempts, int64_t now)
{
    if (p->block_id[0]) {
        int64_t req = 0, poi = 0;
        db_block_verify_fail(db, p->submission_id, reason, max_attempts, now,
                             &req, &poi);
        fprintf(stderr, "verify: %s (block %s, %lld members) FAIL: %s"
                        "  [requeued=%lld poisoned=%lld]\n",
                p->workunit_id, p->block_id, (long long)p->member_count, reason,
                (long long)req, (long long)poi);
    } else {
        db_verify_fail(db, p->submission_id, reason, max_attempts, now, NULL);
        fprintf(stderr, "verify: %s FAIL: %s\n", p->workunit_id, reason);
    }
}

static void drain_pending(verify_thread_t *vt, ggnfs_db_t *db,
                          const verify_poly_gmp_t *poly)
{
    /* Reservoir buffer reused across submissions, grown on demand: a job with
     * only CPU-class workunits never allocates more than K entries, while a
     * GPU-class one grows once and then reuses. `resalloc` is what is
     * allocated; `rescap` is what this submission actually wants. */
    verify_relation_t *resbuf   = NULL;
    int                resalloc = 0;
    const int          spot_on  = (poly && vt->spotcheck_k > 0);

    /* Cheap (one indexed aggregate) and only once per drain pass, not per
     * submission — but it does need re-reading, because `extend` can add a
     * narrower band while the verifier is running. */
    if (spot_on) {
        int64_t base = 0;
        if (db_workunit_base_q_range(db, &base) == 0 && base > 0) {
            if (base != vt->base_q_range) {
                fprintf(stderr, "verify: spot-check baseline q_range=%lld "
                        "(k=%d, scaling to %dx)\n",
                        (long long)base, vt->spotcheck_k, SPOTCHECK_MAX_SCALE);
            }
            vt->base_q_range = base;
        }
    }

    for (;;) {
        db_pending_t p;
        int r = db_verify_next_pending(db, &p);
        if (r == 1) break;              /* no work */
        if (r < 0)  break;              /* error already logged */

        int64_t now    = (int64_t)time(NULL);
        int64_t parsed = 0, failed = 0, qviol = 0;
        char    first_reason[160] = {0};

        if (!p.file_path) {
            /* resolve_fail, not db_verify_fail: for a block the latter requeues
             * only the anchor and leaves every other member stuck in
             * 'submitted' forever, since the sweep only touches 'leased'. */
            resolve_fail(db, &p, "submission has no file_path",
                         vt->max_attempts, now);
            db_pending_free(&p);
            continue;
        }

        verify_check_t check = {
            .q_start = p.q_start,
            .q_range = p.q_range,
            .side    = p.side,
        };
        int rescap = spot_on ? spotcheck_k_for(vt->spotcheck_k,
                                               vt->base_q_range, p.q_range)
                             : 0;
        if (rescap > resalloc) {
            verify_relation_t *nb = realloc(resbuf,
                                            (size_t)rescap * sizeof(*resbuf));
            if (!nb) {
                /* Keep whatever we already have and sample at that size
                 * rather than dropping the spot-check entirely. */
                fprintf(stderr, "verify: reservoir realloc to %d failed; "
                                "sampling %d\n", rescap, resalloc);
                rescap = resalloc;
            } else {
                resbuf   = nb;
                resalloc = rescap;
            }
        }

        verify_reservoir_t reservoir = {
            .buf  = resbuf,
            .cap  = rescap,
            .seed = (unsigned int)(now ^ (uintptr_t)p.file_path),
        };

        /* For a block, count relations per member so a sub-range that produced
         * nothing can be charged on its own instead of dragging the whole
         * block down. Only worth setting up when there is more than one
         * member. */
        verify_coverage_t  cov     = {0};
        verify_coverage_t *cov_arg = NULL;
        int64_t *mstarts = NULL, *mbounds = NULL, *mcounts = NULL;
        int      n_members = 0;
        if (p.block_id[0] && p.member_count > 1 &&
            db_block_members(db, p.q_start, p.q_start + p.q_range,
                             &mstarts, &n_members) == 0 && n_members > 1) {
            mbounds = malloc((size_t)(n_members + 1) * sizeof(*mbounds));
            mcounts = calloc((size_t)n_members, sizeof(*mcounts));
            if (mbounds && mcounts) {
                memcpy(mbounds, mstarts, (size_t)n_members * sizeof(*mbounds));
                mbounds[n_members] = p.q_start + p.q_range;
                cov.bounds    = mbounds;
                cov.n_members = n_members;
                cov.counts    = mcounts;
                cov_arg       = &cov;
            }
        }
#define DRAIN_NEXT() do { free(mstarts); free(mbounds); free(mcounts); \
                          db_pending_free(&p); } while (0)

        int io_err = (verify_parse_file_check(p.file_path, &check,
                                              rescap > 0 ? &reservoir : NULL,
                                              cov_arg,
                                              &parsed, &failed, &qviol,
                                              first_reason, sizeof(first_reason)) != 0);
        if (io_err) {
            char reason[256];
            snprintf(reason, sizeof(reason), "open %s: %s",
                     p.file_path, strerror(errno));
            resolve_fail(db, &p, reason, vt->max_attempts, now);
            DRAIN_NEXT();
            continue;
        }
        if (parsed == 0 && failed == 0 && qviol == 0) {
            resolve_fail(db, &p, "empty relation file", vt->max_attempts, now);
            DRAIN_NEXT();
            continue;
        }
        if (failed > 0 || qviol > 0) {
            char reason[256];
            snprintf(reason, sizeof(reason),
                     "parse_fail=%lld q_out_of_range=%lld accepted=%lld (first: %s)",
                     (long long)failed, (long long)qviol, (long long)parsed,
                     first_reason[0] ? first_reason : "?");
            resolve_fail(db, &p, reason, vt->max_attempts, now);
            DRAIN_NEXT();
            continue;
        }

        /* Parse + q-range clean. Run the norm spot-check if we have a poly. */
        if (rescap > 0 && reservoir.count > 0) {
            char spot_reason[200] = {0};
            int spotfails = verify_spotcheck(poly, reservoir.buf, reservoir.count,
                                             spot_reason, sizeof(spot_reason));
            if (spotfails > 0) {
                char reason[300];
                snprintf(reason, sizeof(reason),
                         "norm spot-check failed: %d/%d (first: %s)",
                         spotfails, reservoir.count,
                         spot_reason[0] ? spot_reason : "?");
                resolve_fail(db, &p, reason, vt->max_attempts, now);
                DRAIN_NEXT();
                continue;
            }
        }

        if (p.block_id[0]) {
            /* Members that contributed nothing are the one thing a passing
             * block can still be wrong about, and they are safe to requeue on
             * their own precisely because they contributed nothing: the stored
             * file has no relations for those ranges, so re-sieving them
             * cannot duplicate anything already assembled. */
            int64_t *empty = NULL;
            int      n_empty = 0;
            if (cov_arg) {
                /* A THRESHOLD, not a zero test. Attribution is by the smallest
                 * sieved-side prime inside the block range, and a relation
                 * occasionally carries a second in-range prime below its own
                 * special-q. Measured on a real 6-member block with one
                 * member's relations deliberately omitted: that member still
                 * attracted 1 stray against a median of 3383, so `== 0` never
                 * fires. A member that was genuinely sieved carries thousands,
                 * so 1/20th of the median separates the two by two orders of
                 * magnitude while staying far away from any real member. */
                /* Compare relation DENSITY, not raw counts. A block is sized
                 * by accumulated q-width and imposes no same-width rule, so a
                 * run can legitimately mix a 1,000-wide row left by an early
                 * `extend --qrange=1000` with 20,000-wide ones. Yield scales
                 * with width, so on raw counts the narrow member sits at ~1/20
                 * of the median and gets requeued despite having sieved
                 * perfectly — duplicating its relations and walking it toward
                 * 'poisoned'. */
                int64_t *dens   = malloc((size_t)n_members * sizeof(*dens));
                int64_t *sorted = malloc((size_t)n_members * sizeof(*sorted));
                if (dens && sorted) {
                    for (int i = 0; i < n_members; i++) {
                        int64_t w = mbounds[i + 1] - mbounds[i];
                        dens[i] = w > 0 ? (mcounts[i] * 1000) / w : 0;
                    }
                    memcpy(sorted, dens, (size_t)n_members * sizeof(*sorted));
                    for (int i = 1; i < n_members; i++) {   /* insertion sort */
                        int64_t v = sorted[i]; int j = i - 1;
                        while (j >= 0 && sorted[j] > v) { sorted[j+1] = sorted[j]; j--; }
                        sorted[j+1] = v;
                    }
                    /* Median normally, but MAX when the median is zero.
                     * A median of 0 means more than half the members produced
                     * nothing — a truncated or killed siever run — and that is
                     * exactly when this check matters most. Guarding on
                     * `median > 0` would skip the requeue and let
                     * db_block_verify_pass mark every one of those unsieved
                     * sub-ranges 'verified', losing them silently for the rest
                     * of the campaign. */
                    int64_t ref = sorted[n_members / 2];
                    if (ref == 0) ref = sorted[n_members - 1];
                    free(sorted); sorted = NULL;

                    if (ref > 0) {
                        empty = malloc((size_t)n_members * sizeof(*empty));
                        if (empty) {
                            for (int i = 0; i < n_members; i++)
                                if (dens[i] * 20 < ref)
                                    empty[n_empty++] = mbounds[i];
                        }
                    } else {
                        /* Nothing bucketed anywhere, yet the file parsed. Do
                         * not guess which members are real — say so instead. */
                        fprintf(stderr, "verify: %s — no relation bucketed to "
                                        "any member; coverage check skipped\n",
                                p.block_id);
                    }
                }
                free(sorted);
                free(dens);
            }
            int64_t req = 0, poi = 0;
            db_block_verify_pass(db, p.submission_id, parsed,
                                 empty, n_empty, vt->max_attempts, now,
                                 &req, &poi);
            fprintf(stderr, "verify: %s (block %s) PASS (%lld relations, "
                            "%d/%d members covered", p.workunit_id, p.block_id,
                    (long long)parsed,
                    cov_arg ? n_members - n_empty : (int)p.member_count,
                    cov_arg ? n_members : (int)p.member_count);
            if (rescap > 0)
                fprintf(stderr, ", spotcheck k=%d/%lld",
                        reservoir.count, (long long)reservoir.sampled_from);
            fprintf(stderr, ")\n");
            for (int i = 0; i < n_empty; i++)
                fprintf(stderr, "verify: %s — member q=%lld produced "
                                "essentially no relations; re-queued\n",
                        p.block_id, (long long)empty[i]);
            if (n_empty > 0)
                fprintf(stderr, "verify: %s — %d/%d members starved; "
                                "requeued=%lld poisoned=%lld\n",
                        p.block_id, n_empty, n_members,
                        (long long)req, (long long)poi);
            free(empty);
        } else {
            db_verify_pass(db, p.submission_id, parsed, now);
            if (rescap > 0) {
                fprintf(stderr, "verify: %s PASS (%lld relations, spotcheck k=%d/%lld)\n",
                        p.workunit_id, (long long)parsed,
                        reservoir.count, (long long)reservoir.sampled_from);
            } else {
                fprintf(stderr, "verify: %s PASS (%lld relations)\n",
                        p.workunit_id, (long long)parsed);
            }
        }
        DRAIN_NEXT();
    }
#undef DRAIN_NEXT

    free(resbuf);
}

static void *verify_thread_run(void *arg)
{
    verify_thread_t *vt = arg;

    ggnfs_db_t *db = db_open(vt->db_path);
    if (!db) {
        fprintf(stderr, "verify: cannot open %s — verifier disabled\n", vt->db_path);
        return NULL;
    }

    /* Load polynomial from meta for the norm spot-check. If meta is missing
     * (e.g. a jobdir initialized before this code landed), spot-check is
     * silently disabled. Parse + q-range still run. */
    verify_poly_gmp_t *poly = NULL;
    if (vt->spotcheck_k > 0) {
        verify_poly_t src;
        if (verify_poly_load_from_meta(db, &src) == 0) {
            poly = verify_poly_gmp_new(&src);
            verify_poly_free(&src);
            if (!poly) {
                fprintf(stderr, "verify: poly_load failed — spot-check disabled\n");
            } else {
                fprintf(stderr, "verify: spot-check enabled (k=%d, poly degree "
                        "%d); sample size scales with band width\n",
                        vt->spotcheck_k, poly->degree);
            }
        } else {
            fprintf(stderr, "verify: meta has no poly — spot-check disabled\n");
        }
    }

    for (;;) {
        drain_pending(vt, db, poly);

        /* Wait for /submit to nudge us, with a timed-wait safety net so we
         * still recover if a signal got lost or pending work was queued
         * before the thread reached this point. */
        pthread_mutex_lock(&vt->lock);
        if (!vt->wake_requested && !vt->stop) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 5;
            pthread_cond_timedwait(&vt->cond, &vt->lock, &ts);
        }
        vt->wake_requested = 0;
        int stop = vt->stop;
        pthread_mutex_unlock(&vt->lock);
        if (stop) break;
    }

    verify_poly_gmp_free(poly);
    db_close(db);
    return NULL;
}

verify_thread_t *verify_thread_start(const char *db_path,
                                     int64_t max_attempts,
                                     int spotcheck_k)
{
    if (!db_path) return NULL;
    verify_thread_t *vt = calloc(1, sizeof(*vt));
    if (!vt) return NULL;
    vt->db_path = strdup(db_path);
    vt->max_attempts = max_attempts;
    vt->spotcheck_k  = spotcheck_k;
    if (!vt->db_path) { free(vt); return NULL; }

    if (pthread_mutex_init(&vt->lock, NULL) != 0) {
        free(vt->db_path); free(vt); return NULL;
    }
    if (pthread_cond_init(&vt->cond, NULL) != 0) {
        pthread_mutex_destroy(&vt->lock);
        free(vt->db_path); free(vt); return NULL;
    }
    if (pthread_create(&vt->th, NULL, verify_thread_run, vt) != 0) {
        pthread_cond_destroy(&vt->cond);
        pthread_mutex_destroy(&vt->lock);
        free(vt->db_path); free(vt);
        fprintf(stderr, "verify: pthread_create failed\n");
        return NULL;
    }
    return vt;
}

void verify_thread_wake(verify_thread_t *vt)
{
    if (!vt) return;
    pthread_mutex_lock(&vt->lock);
    vt->wake_requested = 1;
    pthread_cond_signal(&vt->cond);
    pthread_mutex_unlock(&vt->lock);
}

void verify_thread_stop(verify_thread_t *vt)
{
    if (!vt) return;
    pthread_mutex_lock(&vt->lock);
    vt->stop = 1;
    pthread_cond_signal(&vt->cond);
    pthread_mutex_unlock(&vt->lock);
    pthread_join(vt->th, NULL);
    pthread_cond_destroy(&vt->cond);
    pthread_mutex_destroy(&vt->lock);
    free(vt->db_path);
    free(vt);
}
