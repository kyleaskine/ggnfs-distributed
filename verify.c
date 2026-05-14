/* verify.c — relation-file / .job parsers + verifier thread; see verify.h.
 *
 * Verifier currently runs parse-pass only (count parseable lines, no GMP norm
 * math). Spot-check on K random relations comes when libgmp is wired in.
 */
#define _POSIX_C_SOURCE 200809L

#include "verify.h"
#include "db.h"

#include <errno.h>
#include <gmp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static int relation_has_q_in_range(const verify_relation_t *rel,
                                   char side, int64_t q_start, int64_t q_range)
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
    for (int i = 0; i < n; i++) {
        if (primes[i] >= q_lo && primes[i] < q_hi) return 1;
    }
    return 0;
}

int verify_parse_file_check(const char *path,
                            const verify_check_t *check,
                            verify_reservoir_t *reservoir,
                            int64_t *out_parsed,
                            int64_t *out_failed,
                            int64_t *out_q_violations,
                            char    *out_first_reason,
                            size_t   reason_buflen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
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

    while ((n = getline(&line, &cap, f)) > 0) {
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
    free(line);
    fclose(f);

    if (out_parsed)        *out_parsed        = parsed;
    if (out_failed)        *out_failed        = failed;
    if (out_q_violations)  *out_q_violations  = qviol;
    return 0;
}

int verify_parse_file(const char *path, int64_t *out_parsed, int64_t *out_failed)
{
    return verify_parse_file_check(path, NULL, NULL,
                                   out_parsed, out_failed, NULL, NULL, 0);
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
        /* all other keys (n, skew, rlim, alim, *lpb*, *mfb*, *lambda, lss) ignored */
    }
    free(line);
    fclose(f);

    if (!ok) { verify_poly_free(out); return -1; }

    if (max_c < 1) {
        fprintf(stderr, "verify_parse_job_file: %s: no c0/c1 coefficients found\n", path);
        verify_poly_free(out);
        return -1;
    }
    for (int k = 0; k <= max_c; k++) {
        if (!out->c[k]) {
            fprintf(stderr, "verify_parse_job_file: %s: missing c%d\n", path, k);
            verify_poly_free(out);
            return -1;
        }
    }
    if (!out->Y0 || !out->Y1) {
        fprintf(stderr, "verify_parse_job_file: %s: missing Y0/Y1\n", path);
        verify_poly_free(out);
        return -1;
    }
    out->degree = max_c;
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
};

/* Drain every pending submission; returns when the queue is empty (or on
 * a DB error after logging it). `poly` may be NULL — in that case the norm
 * spot-check is silently skipped (parse + q-range only). */
static void drain_pending(verify_thread_t *vt, ggnfs_db_t *db,
                          const verify_poly_gmp_t *poly)
{
    /* Reservoir buffer reused across submissions; allocated lazily so K=0
     * users pay nothing. */
    verify_relation_t *resbuf = NULL;
    int                rescap = (poly && vt->spotcheck_k > 0) ? vt->spotcheck_k : 0;
    if (rescap > 0) {
        resbuf = malloc((size_t)rescap * sizeof(*resbuf));
        if (!resbuf) {
            fprintf(stderr, "verify: reservoir malloc failed; spot-check disabled\n");
            rescap = 0;
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
            db_verify_fail(db, p.submission_id, "submission has no file_path",
                           vt->max_attempts, now, NULL);
            db_pending_free(&p);
            continue;
        }

        verify_check_t check = {
            .q_start = p.q_start,
            .q_range = p.q_range,
            .side    = p.side,
        };
        verify_reservoir_t reservoir = {
            .buf  = resbuf,
            .cap  = rescap,
            .seed = (unsigned int)(now ^ (uintptr_t)p.file_path),
        };

        int io_err = (verify_parse_file_check(p.file_path, &check,
                                              rescap > 0 ? &reservoir : NULL,
                                              &parsed, &failed, &qviol,
                                              first_reason, sizeof(first_reason)) != 0);
        if (io_err) {
            char reason[256];
            snprintf(reason, sizeof(reason), "open %s: %s",
                     p.file_path, strerror(errno));
            db_verify_fail(db, p.submission_id, reason,
                           vt->max_attempts, now, NULL);
            db_pending_free(&p);
            continue;
        }
        if (parsed == 0 && failed == 0 && qviol == 0) {
            const char *reason = "empty relation file";
            db_verify_fail(db, p.submission_id, reason,
                           vt->max_attempts, now, NULL);
            fprintf(stderr, "verify: %s FAIL: %s\n", p.workunit_id, reason);
            db_pending_free(&p);
            continue;
        }
        if (failed > 0 || qviol > 0) {
            char reason[256];
            snprintf(reason, sizeof(reason),
                     "parse_fail=%lld q_out_of_range=%lld accepted=%lld (first: %s)",
                     (long long)failed, (long long)qviol, (long long)parsed,
                     first_reason[0] ? first_reason : "?");
            db_verify_fail(db, p.submission_id, reason,
                           vt->max_attempts, now, NULL);
            fprintf(stderr, "verify: %s FAIL: %s\n", p.workunit_id, reason);
            db_pending_free(&p);
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
                db_verify_fail(db, p.submission_id, reason,
                               vt->max_attempts, now, NULL);
                fprintf(stderr, "verify: %s FAIL: %s\n", p.workunit_id, reason);
                db_pending_free(&p);
                continue;
            }
        }

        db_verify_pass(db, p.submission_id, parsed, now);
        if (rescap > 0) {
            fprintf(stderr, "verify: %s PASS (%lld relations, spotcheck k=%d/%lld)\n",
                    p.workunit_id, (long long)parsed,
                    reservoir.count, (long long)reservoir.sampled_from);
        } else {
            fprintf(stderr, "verify: %s PASS (%lld relations)\n",
                    p.workunit_id, (long long)parsed);
        }
        db_pending_free(&p);
    }

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
                fprintf(stderr, "verify: spot-check enabled (k=%d, poly degree %d)\n",
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
