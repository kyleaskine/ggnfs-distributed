/* ggnfs-sieve-server — Phase 1 walking skeleton.
 *
 *   ggnfs-sieve-server init   --job=foo.job --siever=gnfs-lasieve4I14e \
 *                            --qmin=80000000 --qmax=100000000 --qrange=10000 \
 *                            [--side=a] [--jobdir=.]
 *
 *   ggnfs-sieve-server serve  [--port=8080] [--jobdir=.] [--lease-seconds=3600]
 *                            [--sweep-seconds=60] [--max-attempts=5]
 *
 * The server reuses the bearer token from <jobdir>/token (written at init).
 * One job per server. No verification, no .ranges write-back, no /stats.
 */
#define _POSIX_C_SOURCE 200809L

#include "db.h"
#include "protocol.h"
#include "vendor/cJSON.h"
#include "vendor/mongoose.h"

/* xxd-generated; provides `dashboard_html[]` and `dashboard_html_len`. */
#include "dashboard_html.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ===================== general helpers ================================== */

static int64_t now_unix(void)
{
    return (int64_t)time(NULL);
}

static int mkdir_p(const char *path)
{
    /* Single-level "make it if missing"; we only need `<jobdir>/files` and
     * `<jobdir>/rels` so we don't need full recursive mkdir. */
    if (mkdir(path, 0755) == 0) return 0;
    if (errno == EEXIST) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    }
    fprintf(stderr, "mkdir %s: %s\n", path, strerror(errno));
    return -1;
}

static char *path_join(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    int needs_sep = (la > 0 && a[la - 1] != '/');
    char *p = malloc(la + (size_t)needs_sep + lb + 1);
    if (!p) return NULL;
    memcpy(p, a, la);
    if (needs_sep) p[la] = '/';
    memcpy(p + la + (size_t)needs_sep, b, lb);
    p[la + (size_t)needs_sep + lb] = '\0';
    return p;
}

static int hex_byte(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static int is_sha256_hex(const char *s, size_t len)
{
    if (len != 64) return 0;
    for (size_t i = 0; i < len; i++) if (hex_byte(s[i]) < 0) return 0;
    return 1;
}

static void hex_encode(const unsigned char *in, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2*i]     = H[in[i] >> 4];
        out[2*i + 1] = H[in[i] & 0x0f];
    }
    out[2*n] = '\0';
}

/* SHA-256 of a file's contents into a 64-byte hex string + NUL. */
static int sha256_file(const char *path, char hex_out[65])
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "sha256_file: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    mg_sha256_ctx ctx;
    mg_sha256_init(&ctx);
    unsigned char buf[64 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        mg_sha256_update(&ctx, buf, n);
    }
    int ferr = ferror(f);
    fclose(f);
    if (ferr) {
        fprintf(stderr, "sha256_file: read error on %s\n", path);
        return -1;
    }
    unsigned char dig[32];
    mg_sha256_final(dig, &ctx);
    hex_encode(dig, 32, hex_out);
    return 0;
}

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) { fprintf(stderr, "copy: open %s: %s\n", src, strerror(errno)); return -1; }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fprintf(stderr, "copy: open %s: %s\n", dst, strerror(errno));
        fclose(in);
        return -1;
    }
    unsigned char buf[64 * 1024];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { rc = -1; break; }
    }
    if (ferror(in)) rc = -1;
    fclose(in); fclose(out);
    if (rc != 0) fprintf(stderr, "copy: i/o error %s -> %s\n", src, dst);
    return rc;
}

/* 32 random bytes from /dev/urandom -> 64-char hex token + NUL. */
static int random_token_hex(char out[65])
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) { fprintf(stderr, "open /dev/urandom: %s\n", strerror(errno)); return -1; }
    unsigned char buf[32];
    ssize_t got = 0;
    while (got < (ssize_t)sizeof(buf)) {
        ssize_t r = read(fd, buf + got, sizeof(buf) - (size_t)got);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            close(fd);
            fprintf(stderr, "read /dev/urandom: %s\n", strerror(errno));
            return -1;
        }
        got += r;
    }
    close(fd);
    hex_encode(buf, sizeof(buf), out);
    return 0;
}

static int parse_int64_arg(const char *s, int64_t *out)
{
    char *end = NULL;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return -1;
    *out = (int64_t)v;
    return 0;
}

/* Pull "--key=value" from argv; returns the value or NULL.
 * If key matches but no '=', returns "" so caller can detect bare flag. */
static const char *flag(int argc, char **argv, const char *key)
{
    size_t klen = strlen(key);
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], key, klen) == 0) {
            if (argv[i][klen] == '=') return argv[i] + klen + 1;
            if (argv[i][klen] == '\0') return "";
        }
    }
    return NULL;
}

/* ===================== init subcommand ================================== */

static void usage_init(void)
{
    fprintf(stderr,
        "usage: ggnfs-sieve-server init \\\n"
        "    --job=<file>            (required) ggnfs .job file describing the polynomial\n"
        "    --siever=<name>         (required) gnfs-lasieve4* binary clients should run\n"
        "    --qmin=<int>            (required) start of special-q range\n"
        "    --qmax=<int>            (required) end (exclusive)\n"
        "    --qrange=<int>          (required) per-workunit range size\n"
        "    [--side=a|r]            default a\n"
        "    [--jobdir=<dir>]        default current dir\n");
}

static int cmd_init(int argc, char **argv)
{
    const char *job_path = flag(argc, argv, "--job");
    const char *siever   = flag(argc, argv, "--siever");
    const char *qmin_s   = flag(argc, argv, "--qmin");
    const char *qmax_s   = flag(argc, argv, "--qmax");
    const char *qrange_s = flag(argc, argv, "--qrange");
    const char *side_s   = flag(argc, argv, "--side");
    const char *jobdir   = flag(argc, argv, "--jobdir");

    if (!job_path || !siever || !qmin_s || !qmax_s || !qrange_s) {
        usage_init();
        return 2;
    }
    if (!jobdir || !*jobdir) jobdir = ".";

    int64_t qmin, qmax, qrange;
    if (parse_int64_arg(qmin_s,   &qmin)   != 0 ||
        parse_int64_arg(qmax_s,   &qmax)   != 0 ||
        parse_int64_arg(qrange_s, &qrange) != 0) {
        fprintf(stderr, "init: --qmin/--qmax/--qrange must be integers\n");
        return 2;
    }
    if (qmin < 0 || qmax <= qmin || qrange <= 0) {
        fprintf(stderr, "init: require qmin >= 0, qmax > qmin, qrange > 0\n");
        return 2;
    }
    char side = 'a';
    if (side_s && *side_s) {
        if ((side_s[0] != 'a' && side_s[0] != 'r') || side_s[1] != '\0') {
            fprintf(stderr, "init: --side must be 'a' or 'r'\n");
            return 2;
        }
        side = side_s[0];
    }

    /* Layout. */
    if (mkdir_p(jobdir) != 0) return 1;
    char *files_dir = path_join(jobdir, "files");
    char *rels_dir  = path_join(jobdir, "rels");
    if (!files_dir || !rels_dir) return 1;
    if (mkdir_p(files_dir) != 0 || mkdir_p(rels_dir) != 0) {
        free(files_dir); free(rels_dir); return 1;
    }

    /* Hash + copy the .job file into <jobdir>/files/<sha>.job. */
    char job_sha[65];
    if (sha256_file(job_path, job_sha) != 0) {
        free(files_dir); free(rels_dir); return 1;
    }
    char dst_name[80];
    snprintf(dst_name, sizeof(dst_name), "%s.job", job_sha);
    char *dst_path = path_join(files_dir, dst_name);
    if (!dst_path) { free(files_dir); free(rels_dir); return 1; }
    if (copy_file(job_path, dst_path) != 0) {
        free(files_dir); free(rels_dir); free(dst_path); return 1;
    }
    /* Store an absolute path: the server's cwd at `serve` time is unrelated
     * to the cwd at `init` time, and mg_http_serve_file resolves relative
     * paths against the server's cwd. */
    char *dst_abs = realpath(dst_path, NULL);
    if (!dst_abs) {
        fprintf(stderr, "init: realpath(%s): %s\n", dst_path, strerror(errno));
        free(files_dir); free(rels_dir); free(dst_path); return 1;
    }
    free(dst_path);
    dst_path = dst_abs;
    struct stat dst_st;
    int64_t dst_bytes = (stat(dst_path, &dst_st) == 0) ? (int64_t)dst_st.st_size : 0;

    /* Open db, seed workunits. */
    char *db_path = path_join(jobdir, "job.db");
    if (!db_path) return 1;
    ggnfs_db_t *db = db_open(db_path);
    if (!db) {
        free(files_dir); free(rels_dir); free(dst_path); free(db_path); return 1;
    }

    if (db_files_insert(db, job_sha, dst_path, dst_bytes, "job") != 0) {
        fprintf(stderr, "init: db_files_insert failed\n");
        db_close(db);
        return 1;
    }

    /* job_id = first 8 hex of the .job sha (per the design's wu-<jobhash>-<seq>). */
    char job_id[9];
    memcpy(job_id, job_sha, 8); job_id[8] = '\0';

    int64_t now = now_unix();
    int64_t seq = 0;
    int64_t q;
    for (q = qmin; q < qmax; q += qrange) {
        int64_t this_range = (q + qrange <= qmax) ? qrange : (qmax - q);
        char id[64];
        snprintf(id, sizeof(id), "wu-%s-%06lld", job_id, (long long)seq);
        if (db_workunit_insert(db, id, q, this_range, side, now) != 0) {
            fprintf(stderr, "init: db_workunit_insert failed at seq=%lld\n", (long long)seq);
            db_close(db);
            return 1;
        }
        seq++;
    }

    /* Token. Stash in meta for self-checking + write to <jobdir>/token. */
    char token[65];
    if (random_token_hex(token) != 0) { db_close(db); return 1; }
    char *token_path = path_join(jobdir, "token");
    if (!token_path) { db_close(db); return 1; }
    {
        int fd = open(token_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) {
            fprintf(stderr, "init: cannot write %s: %s\n", token_path, strerror(errno));
            db_close(db);
            return 1;
        }
        ssize_t w1 = write(fd, token, 64);
        ssize_t w2 = write(fd, "\n",  1);
        close(fd);
        if (w1 != 64 || w2 != 1) {
            fprintf(stderr, "init: short write to %s\n", token_path);
            db_close(db);
            return 1;
        }
    }

    db_meta_set(db, "token",     token);
    db_meta_set(db, "job_id",    job_id);
    db_meta_set(db, "siever",    siever);
    {
        char buf[2] = { side, 0 };
        db_meta_set(db, "side",  buf);
    }
    db_meta_set(db, "job_sha256", job_sha);

    db_close(db);

    printf("ggnfs-sieve-server: initialized job %s\n", job_id);
    printf("  jobdir   : %s\n", jobdir);
    printf("  job.db   : %s\n", db_path);
    printf("  job file : %s  (sha=%s, %lld bytes)\n", dst_path, job_sha, (long long)dst_bytes);
    printf("  workunits: %lld   (q_range=%lld, side=%c, siever=%s)\n",
           (long long)seq, (long long)qrange, side, siever);
    printf("  token    : written to %s (chmod 600)\n", token_path);
    printf("\nNext: ggnfs-sieve-server serve --jobdir=%s\n", jobdir);

    free(files_dir); free(rels_dir); free(dst_path); free(db_path); free(token_path);
    return 0;
}

/* ===================== extend subcommand =============================== */

static void usage_extend(void)
{
    fprintf(stderr,
        "usage: ggnfs-sieve-server extend \\\n"
        "    --jobdir=<dir>          (required) existing initialized jobdir\n"
        "    --qmin=<int>            (required) start of new range — must be >= existing q_end\n"
        "    --qmax=<int>            (required) end (exclusive)\n"
        "    --qrange=<int>          (required) per-workunit range size\n"
        "\nAdds workunits to an existing job. Token, .job file, siever, and side\n"
        "are inherited from init. Sequence numbering continues from the last\n"
        "init/extend so workunit IDs stay unique.\n");
}

static int cmd_extend(int argc, char **argv)
{
    const char *jobdir   = flag(argc, argv, "--jobdir");
    const char *qmin_s   = flag(argc, argv, "--qmin");
    const char *qmax_s   = flag(argc, argv, "--qmax");
    const char *qrange_s = flag(argc, argv, "--qrange");

    if (!jobdir || !qmin_s || !qmax_s || !qrange_s) {
        usage_extend();
        return 2;
    }

    int64_t qmin, qmax, qrange;
    if (parse_int64_arg(qmin_s,   &qmin)   != 0 ||
        parse_int64_arg(qmax_s,   &qmax)   != 0 ||
        parse_int64_arg(qrange_s, &qrange) != 0) {
        fprintf(stderr, "extend: --qmin/--qmax/--qrange must be integers\n");
        return 2;
    }
    if (qmin < 0 || qmax <= qmin || qrange <= 0) {
        fprintf(stderr, "extend: require qmin >= 0, qmax > qmin, qrange > 0\n");
        return 2;
    }

    char *db_path = path_join(jobdir, "job.db");
    if (!db_path) return 1;
    ggnfs_db_t *db = db_open(db_path);
    if (!db) {
        fprintf(stderr, "extend: cannot open %s — did you run init?\n", db_path);
        free(db_path);
        return 1;
    }

    /* Inherit side from meta. */
    char *m_side = db_meta_get(db, "side");
    if (!m_side || !*m_side) {
        fprintf(stderr, "extend: meta 'side' missing — db not initialized?\n");
        free(m_side); db_close(db); free(db_path); return 1;
    }
    char side = m_side[0];
    free(m_side);

    /* Inherit job_id from meta (used to form workunit IDs). */
    char *m_jobid = db_meta_get(db, "job_id");
    if (!m_jobid || !*m_jobid) {
        fprintf(stderr, "extend: meta 'job_id' missing\n");
        free(m_jobid); db_close(db); free(db_path); return 1;
    }
    char job_id[16];
    snprintf(job_id, sizeof(job_id), "%s", m_jobid);
    free(m_jobid);

    /* Find next sequence number and existing high-water q. */
    int64_t existing_count = 0, q_end = 0;
    if (db_workunit_extent(db, &existing_count, &q_end) != 0) {
        fprintf(stderr, "extend: cannot read existing workunits\n");
        db_close(db); free(db_path); return 1;
    }
    if (existing_count == 0) {
        fprintf(stderr, "extend: no existing workunits — use 'init' first\n");
        db_close(db); free(db_path); return 1;
    }
    if (qmin < q_end) {
        fprintf(stderr,
            "extend: qmin=%lld would overlap existing range (q_end=%lld). "
            "Pick qmin >= %lld.\n",
            (long long)qmin, (long long)q_end, (long long)q_end);
        db_close(db); free(db_path); return 1;
    }

    /* Insert new workunits, continuing the sequence. */
    int64_t now = now_unix();
    int64_t seq = existing_count;
    int64_t added = 0;
    int64_t q;
    for (q = qmin; q < qmax; q += qrange) {
        int64_t this_range = (q + qrange <= qmax) ? qrange : (qmax - q);
        char id[64];
        snprintf(id, sizeof(id), "wu-%s-%06lld", job_id, (long long)seq);
        if (db_workunit_insert(db, id, q, this_range, side, now) != 0) {
            fprintf(stderr, "extend: db_workunit_insert failed at seq=%lld\n",
                    (long long)seq);
            db_close(db); free(db_path); return 1;
        }
        seq++;
        added++;
    }

    db_close(db);

    printf("ggnfs-sieve-server: extended job %s\n", job_id);
    printf("  new workunits : %lld   (q_range=%lld, side=%c)\n",
           (long long)added, (long long)qrange, side);
    printf("  q range added : [%lld, %lld)\n",
           (long long)qmin, (long long)(qmin + added * qrange));
    printf("  total workunits now: %lld\n", (long long)(existing_count + added));
    free(db_path);
    return 0;
}

/* ===================== serve subcommand ================================= */

typedef struct {
    ggnfs_db_t  *db;
    char        token[65];          /* expected bearer token */
    char        job_id[16];
    char        siever[64];         /* required siever binary name */
    char        side;
    char        job_sha256[65];
    char       *jobdir;
    char       *rels_dir;
    int64_t     lease_seconds;
    int64_t     sweep_seconds;
    int64_t     started_at;
    int64_t     max_attempts;       /* mark workunit poisoned after this many lease expiries */
} server_ctx_t;

#define COMMAND_TEMPLATE_DEFAULT \
    "{siever} -f {q_start} -c {q_range} -a {job_file} -o {output_file} -n 0"
#define OUTPUT_NAME_DEFAULT  "rels.out"
#define OUTPUT_MAX_BYTES     (524288000LL)  /* 500 MiB per design */

static void send_text(struct mg_connection *c, int code, const char *body)
{
    /* mg_http_reply takes a printf-style body with %s/%d etc. — we want raw. */
    mg_http_reply(c, code, "Content-Type: text/plain\r\n", "%s", body ? body : "");
}

static void send_json_take(struct mg_connection *c, int code, char *json_owned)
{
    /* Takes ownership of `json_owned` (free()d here). NULL => empty body. */
    if (json_owned) {
        mg_http_reply(c, code, "Content-Type: application/json\r\n", "%s", json_owned);
        free(json_owned);
    } else {
        mg_http_reply(c, code, "Content-Type: application/json\r\n", "{}");
    }
}

/* Returns 1 if the request carries a valid Bearer token; 0 otherwise.
 * On 0, writes a 401 response. */
static int check_auth(struct mg_connection *c, struct mg_http_message *hm,
                      const char *expected_token)
{
    struct mg_str *h = mg_http_get_header(hm, "Authorization");
    if (!h) goto deny;
    const char *prefix = "Bearer ";
    size_t plen = strlen(prefix);
    if (h->len <= plen) goto deny;
    if (mg_strcasecmp(mg_str_n(h->buf, plen), mg_str(prefix)) != 0) goto deny;
    /* Constant-time-ish compare against expected_token. mg_strcmp is fine —
     * we're not defending against timing attacks in the MVP per design. */
    struct mg_str presented = mg_str_n(h->buf + plen, h->len - plen);
    struct mg_str expected  = mg_str(expected_token);
    if (mg_strcmp(presented, expected) != 0) goto deny;
    return 1;
deny:
    mg_http_reply(c, 401, "Content-Type: text/plain\r\n", "unauthorized\n");
    return 0;
}

/* ---- /lease ---- */

static void handle_lease(struct mg_connection *c, struct mg_http_message *hm,
                         server_ctx_t *ctx)
{
    if (!check_auth(c, hm, ctx->token)) return;

    char client_id[64] = {0};
    char client_ver[32] = {0};
    proto_decode_lease_request(hm->body.buf, hm->body.len,
                               client_id, sizeof(client_id),
                               client_ver, sizeof(client_ver));
    if (client_id[0] == '\0') {
        send_text(c, 400, "missing client_id\n");
        return;
    }

    db_clients_seen(ctx->db, client_id, now_unix());

    db_lease_result_t r;
    int rc = db_lease(ctx->db, client_id, ctx->lease_seconds, now_unix(), &r);
    if (rc == 1) {
        /* No work right now. Job is still running. */
        mg_http_reply(c, 204, "", "");
        return;
    }
    if (rc != 0) {
        send_text(c, 500, "internal error\n");
        return;
    }

    char file_url[96];
    snprintf(file_url, sizeof(file_url), "/file/%s", ctx->job_sha256);
    proto_lease_response_args a = {
        .workunit_id      = r.id,
        .q_start          = r.q_start,
        .q_range          = r.q_range,
        .side             = r.side,
        .lease_seconds    = ctx->lease_seconds,
        .siever           = ctx->siever,
        .command_template = COMMAND_TEMPLATE_DEFAULT,
        .file_name        = "job.txt",
        .file_sha256_hex  = ctx->job_sha256,
        .file_url         = file_url,
        .output_name      = OUTPUT_NAME_DEFAULT,
        .output_max_bytes = OUTPUT_MAX_BYTES,
    };
    send_json_take(c, 200, proto_encode_lease_response(&a));
}

/* ---- /file/<sha> ---- */

static void handle_file(struct mg_connection *c, struct mg_http_message *hm,
                        server_ctx_t *ctx)
{
    if (!check_auth(c, hm, ctx->token)) return;

    /* hm->uri starts with "/file/"; the sha is what follows. */
    const char *prefix = "/file/";
    size_t plen = strlen(prefix);
    if (hm->uri.len <= plen) { send_text(c, 404, "not found\n"); return; }
    const char *sha_start = hm->uri.buf + plen;
    size_t sha_len = hm->uri.len - plen;
    if (!is_sha256_hex(sha_start, sha_len)) {
        send_text(c, 404, "not found\n");
        return;
    }
    char sha[65];
    memcpy(sha, sha_start, 64); sha[64] = '\0';

    char *path = db_files_path_for(ctx->db, sha);
    if (!path) { send_text(c, 404, "not found\n"); return; }

    struct mg_http_serve_opts opts = { 0 };
    opts.mime_types = "";  /* default */
    mg_http_serve_file(c, hm, path, &opts);
    free(path);
}

/* ---- /submit ---- */

static int header_get_str(struct mg_http_message *hm, const char *name,
                          char *buf, size_t buf_n)
{
    struct mg_str *h = mg_http_get_header(hm, name);
    if (!h || h->len == 0 || h->len >= buf_n) { buf[0] = '\0'; return -1; }
    memcpy(buf, h->buf, h->len);
    buf[h->len] = '\0';
    return 0;
}

static void handle_submit(struct mg_connection *c, struct mg_http_message *hm,
                          server_ctx_t *ctx)
{
    if (!check_auth(c, hm, ctx->token)) return;

    char workunit_id[64], client_id[64], client_sha[80];
    if (header_get_str(hm, "X-Workunit-Id", workunit_id, sizeof(workunit_id)) != 0 ||
        header_get_str(hm, "X-Client-Id",   client_id,   sizeof(client_id))   != 0) {
        send_text(c, 400, "missing X-Workunit-Id / X-Client-Id\n");
        return;
    }
    /* X-Sha256 and X-Sieve-Seconds are advisory in Phase 1. */
    if (header_get_str(hm, "X-Sha256", client_sha, sizeof(client_sha)) != 0)
        client_sha[0] = '\0';
    char sieve_seconds_buf[32];
    double sieve_seconds = 0.0;
    if (header_get_str(hm, "X-Sieve-Seconds", sieve_seconds_buf,
                       sizeof(sieve_seconds_buf)) == 0) {
        sieve_seconds = strtod(sieve_seconds_buf, NULL);
    }

    /* Compute body sha256 server-side; if X-Sha256 was set and disagrees, 400. */
    mg_sha256_ctx s;
    mg_sha256_init(&s);
    mg_sha256_update(&s, (const unsigned char *)hm->body.buf, hm->body.len);
    unsigned char dig[32]; mg_sha256_final(dig, &s);
    char body_sha[65];
    hex_encode(dig, 32, body_sha);
    if (client_sha[0] != '\0' && strcmp(client_sha, body_sha) != 0) {
        send_text(c, 400, "X-Sha256 mismatch\n");
        return;
    }

    /* Persist body to <jobdir>/rels/<workunit_id>.dat. */
    char rel_name[96];
    snprintf(rel_name, sizeof(rel_name), "%s.dat", workunit_id);
    char *rel_path = path_join(ctx->rels_dir, rel_name);
    if (!rel_path) { send_text(c, 500, "oom\n"); return; }
    {
        FILE *f = fopen(rel_path, "wb");
        if (!f) {
            fprintf(stderr, "submit: open %s: %s\n", rel_path, strerror(errno));
            free(rel_path);
            send_text(c, 500, "cannot write submission\n");
            return;
        }
        if (fwrite(hm->body.buf, 1, hm->body.len, f) != hm->body.len) {
            fclose(f); free(rel_path);
            send_text(c, 500, "short write\n");
            return;
        }
        fclose(f);
    }

    /* Count newlines as a rough proxy for relation count (good enough for
     * Phase 1 — the verifier in Phase 3 will do parse-level counting). */
    int64_t num_relations = 0;
    for (size_t i = 0; i < hm->body.len; i++) {
        if (hm->body.buf[i] == '\n') num_relations++;
    }

    db_clients_seen(ctx->db, client_id, now_unix());
    int rc = db_submit(ctx->db, workunit_id, client_id, rel_path, body_sha,
                       num_relations, sieve_seconds, now_unix());
    if (rc == 1) {
        /* Workunit not currently leased — re-issued or stale. */
        free(rel_path);
        send_text(c, 409, "workunit not leased\n");
        return;
    }
    if (rc != 0) {
        free(rel_path);
        send_text(c, 500, "internal error\n");
        return;
    }
    free(rel_path);

    send_json_take(c, 200, proto_encode_submit_response(1, "skipped", num_relations));
}

/* ---- /stats ---- */

static char *format_stats_json(server_ctx_t *ctx, const db_stats_t *s,
                               int64_t now)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddStringToObject (root, "job_id",         ctx->job_id);
    cJSON_AddStringToObject (root, "siever",         ctx->siever);
    { char sb[2] = { ctx->side, 0 };
      cJSON_AddStringToObject(root, "side",          sb); }
    cJSON_AddNumberToObject (root, "now_unix",       (double)now);
    cJSON_AddNumberToObject (root, "started_at",     (double)ctx->started_at);
    cJSON_AddNumberToObject (root, "uptime_seconds", (double)(now - ctx->started_at));
    cJSON_AddNumberToObject (root, "lease_seconds",  (double)ctx->lease_seconds);
    cJSON_AddNumberToObject (root, "sweep_seconds",  (double)ctx->sweep_seconds);
    cJSON_AddNumberToObject (root, "max_attempts",   (double)ctx->max_attempts);

    cJSON *wu = cJSON_AddObjectToObject(root, "workunits");
    cJSON_AddNumberToObject(wu, "total",     (double)s->wu.total);
    cJSON_AddNumberToObject(wu, "available", (double)s->wu.available);
    cJSON_AddNumberToObject(wu, "leased",    (double)s->wu.leased);
    cJSON_AddNumberToObject(wu, "submitted", (double)s->wu.submitted);
    cJSON_AddNumberToObject(wu, "verified",  (double)s->wu.verified);
    cJSON_AddNumberToObject(wu, "failed",    (double)s->wu.failed);
    cJSON_AddNumberToObject(wu, "poisoned",  (double)s->wu.poisoned);
    cJSON_AddNumberToObject(wu, "q_min",     (double)s->q_min);
    cJSON_AddNumberToObject(wu, "q_max",     (double)s->q_max);

    cJSON *sub = cJSON_AddObjectToObject(root, "submissions");
    cJSON_AddNumberToObject(sub, "total",              (double)s->sub_total);
    cJSON_AddNumberToObject(sub, "total_relations",    (double)s->sub_relations);
    cJSON_AddNumberToObject(sub, "last_5m",            (double)s->sub_last_5m);
    cJSON_AddNumberToObject(sub, "last_1h",            (double)s->sub_last_1h);
    cJSON_AddNumberToObject(sub, "last_24h",           (double)s->sub_last_24h);
    cJSON_AddNumberToObject(sub, "last_submit_unix",   (double)s->last_submit_unix);
    cJSON_AddNumberToObject(sub, "avg_sieve_seconds",  s->avg_sieve_seconds);

    cJSON *clients = cJSON_AddArrayToObject(root, "clients");
    for (int i = 0; i < s->client_count; i++) {
        const db_stats_client_t *cc = &s->clients[i];
        cJSON *o = cJSON_CreateObject();
        if (!o) continue;
        cJSON_AddStringToObject(o, "id",                 cc->id);
        cJSON_AddNumberToObject(o, "first_seen",         (double)cc->first_seen);
        cJSON_AddNumberToObject(o, "last_seen",          (double)cc->last_seen);
        cJSON_AddNumberToObject(o, "submissions",        (double)cc->submissions);
        cJSON_AddNumberToObject(o, "relations",          (double)cc->relations);
        cJSON_AddNumberToObject(o, "total_failures",     (double)cc->total_failures);
        cJSON_AddNumberToObject(o, "avg_sieve_seconds",  cc->avg_sieve_seconds);
        cJSON_AddStringToObject(o, "current_workunit",   cc->current_workunit);
        cJSON_AddItemToArray(clients, o);
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

static void handle_stats(struct mg_connection *c, struct mg_http_message *hm,
                         server_ctx_t *ctx)
{
    if (!check_auth(c, hm, ctx->token)) return;

    db_stats_t s;
    if (db_stats_snapshot(ctx->db, now_unix(), &s) != 0) {
        send_text(c, 500, "stats query failed\n");
        return;
    }
    char *json = format_stats_json(ctx, &s, now_unix());
    db_stats_free(&s);
    if (!json) { send_text(c, 500, "oom\n"); return; }
    send_json_take(c, 200, json);
}

/* ---- / dashboard (HTML, no auth — JS fetches /stats with bearer token) ---- */

static void handle_dashboard(struct mg_connection *c)
{
    /* The HTML itself is harmless static content; the dashboard's JS reads
     * ?token=<x> from the URL and uses it for the (authenticated) /stats
     * polling. So no token check here. */
    mg_printf(c,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "Content-Length: %u\r\n"
        "\r\n",
        dashboard_html_len);
    mg_send(c, dashboard_html, dashboard_html_len);
}

/* ---- /health ---- */

static void handle_health(struct mg_connection *c, struct mg_http_message *hm,
                          server_ctx_t *ctx)
{
    /* /health is unauthenticated: useful for clients to distinguish a
     * down/restarted server from a transient network blip without burning
     * a token check on every retry. */
    (void)hm;
    int64_t uptime = now_unix() - ctx->started_at;
    send_json_take(c, 200, proto_encode_health_response(1, ctx->job_id, uptime));
}

/* ---- routing ---- */

static int uri_eq(struct mg_http_message *hm, const char *path)
{
    return mg_strcmp(hm->uri, mg_str(path)) == 0;
}

static int uri_starts_with(struct mg_http_message *hm, const char *prefix)
{
    size_t plen = strlen(prefix);
    return hm->uri.len >= plen && memcmp(hm->uri.buf, prefix, plen) == 0;
}

static int method_is(struct mg_http_message *hm, const char *m)
{
    return mg_strcmp(hm->method, mg_str(m)) == 0;
}

static void ev_handler(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev != MG_EV_HTTP_MSG) return;
    struct mg_http_message *hm = (struct mg_http_message *)ev_data;
    server_ctx_t *ctx = (server_ctx_t *)c->fn_data;

    if (uri_eq(hm, "/lease") && method_is(hm, "POST")) {
        handle_lease(c, hm, ctx);
    } else if (uri_starts_with(hm, "/file/") && method_is(hm, "GET")) {
        handle_file(c, hm, ctx);
    } else if (uri_eq(hm, "/submit") && method_is(hm, "POST")) {
        handle_submit(c, hm, ctx);
    } else if (uri_eq(hm, "/health") && method_is(hm, "GET")) {
        handle_health(c, hm, ctx);
    } else if (uri_eq(hm, "/stats") && method_is(hm, "GET")) {
        handle_stats(c, hm, ctx);
    } else if ((uri_eq(hm, "/") || uri_eq(hm, "/dashboard"))
               && method_is(hm, "GET")) {
        handle_dashboard(c);
    } else {
        send_text(c, 404, "not found\n");
    }
}

/* ---- serve mode entry ---- */

static int read_file_string(const char *path, char *out, size_t out_n)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(out, 1, out_n - 1, f);
    fclose(f);
    out[n] = '\0';
    /* trim trailing \n / \r / spaces */
    while (n > 0 && (out[n-1] == '\n' || out[n-1] == '\r' ||
                     out[n-1] == ' '  || out[n-1] == '\t')) {
        out[--n] = '\0';
    }
    return 0;
}

/* mongoose timer fires this on the event-loop thread, so it shares the DB
 * connection with the request handlers — no locking needed. */
static void on_sweep_timer(void *arg)
{
    server_ctx_t *ctx = (server_ctx_t *)arg;
    int64_t requeued = 0, poisoned = 0;
    if (db_lease_expire_sweep(ctx->db, now_unix(), ctx->max_attempts,
                              &requeued, &poisoned) != 0) {
        fprintf(stderr, "sweep: db_lease_expire_sweep failed\n");
        return;
    }
    if (requeued > 0 || poisoned > 0) {
        fprintf(stderr, "sweep: requeued=%lld  poisoned=%lld\n",
                (long long)requeued, (long long)poisoned);
    }
}

static int cmd_serve(int argc, char **argv)
{
    const char *port_s    = flag(argc, argv, "--port");
    const char *jobdir    = flag(argc, argv, "--jobdir");
    const char *lease_s   = flag(argc, argv, "--lease-seconds");
    const char *sweep_s   = flag(argc, argv, "--sweep-seconds");
    const char *attempt_s = flag(argc, argv, "--max-attempts");

    int64_t port = 8080;
    if (port_s && *port_s && parse_int64_arg(port_s, &port) != 0) {
        fprintf(stderr, "serve: bad --port\n");
        return 2;
    }
    if (!jobdir || !*jobdir) jobdir = ".";

    int64_t lease_seconds = 3600;
    if (lease_s && *lease_s && parse_int64_arg(lease_s, &lease_seconds) != 0) {
        fprintf(stderr, "serve: bad --lease-seconds\n");
        return 2;
    }

    int64_t sweep_seconds = 60;
    if (sweep_s && *sweep_s && parse_int64_arg(sweep_s, &sweep_seconds) != 0) {
        fprintf(stderr, "serve: bad --sweep-seconds\n");
        return 2;
    }
    if (sweep_seconds < 1) sweep_seconds = 1;

    int64_t max_attempts = 5;
    if (attempt_s && *attempt_s && parse_int64_arg(attempt_s, &max_attempts) != 0) {
        fprintf(stderr, "serve: bad --max-attempts\n");
        return 2;
    }
    if (max_attempts < 1) max_attempts = 1;

    char *db_path = path_join(jobdir, "job.db");
    if (!db_path) return 1;

    server_ctx_t ctx = {0};
    ctx.db = db_open(db_path);
    free(db_path);
    if (!ctx.db) return 1;

    /* Pull job metadata. token is also re-loaded from disk for the operator's
     * convenience (rotating the token = edit <jobdir>/token + restart). */
    char *m_token  = db_meta_get(ctx.db, "token");
    char *m_jobid  = db_meta_get(ctx.db, "job_id");
    char *m_siever = db_meta_get(ctx.db, "siever");
    char *m_side   = db_meta_get(ctx.db, "side");
    char *m_jobsha = db_meta_get(ctx.db, "job_sha256");
    if (!m_token || !m_jobid || !m_siever || !m_side || !m_jobsha) {
        fprintf(stderr, "serve: db missing meta — was 'init' run on this jobdir?\n");
        free(m_token); free(m_jobid); free(m_siever); free(m_side); free(m_jobsha);
        db_close(ctx.db);
        return 1;
    }

    /* Prefer the token file on disk if it exists. */
    {
        char *tok_path = path_join(jobdir, "token");
        char file_tok[80];
        if (tok_path && read_file_string(tok_path, file_tok, sizeof(file_tok)) == 0
                && strlen(file_tok) == 64) {
            snprintf(ctx.token, sizeof(ctx.token), "%s", file_tok);
        } else {
            snprintf(ctx.token, sizeof(ctx.token), "%s", m_token);
        }
        free(tok_path);
    }
    snprintf(ctx.job_id,     sizeof(ctx.job_id),     "%s", m_jobid);
    snprintf(ctx.siever,     sizeof(ctx.siever),     "%s", m_siever);
    snprintf(ctx.job_sha256, sizeof(ctx.job_sha256), "%s", m_jobsha);
    ctx.side = m_side[0];
    free(m_token); free(m_jobid); free(m_siever); free(m_side); free(m_jobsha);

    ctx.jobdir        = strdup(jobdir);
    ctx.rels_dir      = path_join(jobdir, "rels");
    ctx.lease_seconds = lease_seconds;
    ctx.sweep_seconds = sweep_seconds;
    ctx.max_attempts  = max_attempts;
    ctx.started_at    = now_unix();
    if (!ctx.jobdir || !ctx.rels_dir) { db_close(ctx.db); return 1; }

    char listen_url[64];
    snprintf(listen_url, sizeof(listen_url), "http://0.0.0.0:%lld",
             (long long)port);

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    mg_log_set(MG_LL_INFO);

    if (mg_http_listen(&mgr, listen_url, ev_handler, &ctx) == NULL) {
        fprintf(stderr, "serve: cannot listen on %s\n", listen_url);
        mg_mgr_free(&mgr);
        db_close(ctx.db);
        return 1;
    }

    /* Lease-expiry sweep: every sweep_seconds, requeue leased workunits
     * whose lease passed (sets state back to 'available'), or mark them
     * 'poisoned' once they've timed out max_attempts times. */
    mg_timer_add(&mgr, (uint64_t)sweep_seconds * 1000,
                 MG_TIMER_REPEAT, on_sweep_timer, &ctx);

    fprintf(stderr,
        "ggnfs-sieve-server: serving job %s on %s\n"
        "  jobdir       : %s\n"
        "  siever       : %s   side=%c   lease=%llds\n"
        "  sweep        : every %llds   max_attempts=%lld\n"
        "  job .job sha : %s\n"
        "  token        : %.8s... (read from <jobdir>/token)\n"
        "  dashboard    : %s/?token=%s\n",
        ctx.job_id, listen_url, ctx.jobdir, ctx.siever, ctx.side,
        (long long)ctx.lease_seconds,
        (long long)sweep_seconds, (long long)ctx.max_attempts,
        ctx.job_sha256, ctx.token,
        listen_url, ctx.token);

    for (;;) mg_mgr_poll(&mgr, 1000);

    /* Unreachable today (no graceful shutdown), but tidy: */
    mg_mgr_free(&mgr);
    free(ctx.jobdir); free(ctx.rels_dir);
    db_close(ctx.db);
    return 0;
}

/* ===================== main ============================================= */

static void usage_top(void)
{
    fprintf(stderr,
        "ggnfs-sieve-server — Phase 1 walking skeleton\n"
        "usage:\n"
        "  ggnfs-sieve-server init   [args...]   create a new job\n"
        "  ggnfs-sieve-server extend [args...]   add workunits to an existing job\n"
        "  ggnfs-sieve-server serve  [args...]   run the HTTP server\n"
        "Run a subcommand without args to see its flags.\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage_top(); return 2; }
    if (strcmp(argv[1], "init")   == 0) return cmd_init  (argc - 1, argv + 1);
    if (strcmp(argv[1], "extend") == 0) return cmd_extend(argc - 1, argv + 1);
    if (strcmp(argv[1], "serve")  == 0) return cmd_serve (argc - 1, argv + 1);
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage_top(); return 0;
    }
    usage_top();
    return 2;
}
