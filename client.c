/* ggnfs-sieve-client — Phase 2 walking skeleton.
 *
 * Polls a ggnfs-sieve-server in a loop:
 *   POST /lease   -> get a workunit
 *   GET  /file/X  -> fetch any input files (sha-cached)
 *   run gnfs-lasieve4* in an isolated child process group
 *   POST /submit  -> hand the relation file back
 *
 *   ggnfs-sieve-client \
 *     --server-url=http://host:8080 \
 *     --token=<bearer token> \
 *     --siever=/usr/local/bin/gnfs-lasieve4I14e \
 *     [--client-id=<defaults to hostname>] \
 *     [--workdir=/tmp/ggnfs-client] \
 *     [--idle-backoff=30] \
 *     [--workers=1] \
 *     [--cpu-pin=0,1,2,...] \
 *     [--once]
 *
 * With --workers=N, N pthreads each drive an independent lease/sieve/submit
 * loop. Each worker gets its own workdir (<workdir>/wN), its own client-id
 * (<base>-wN), and its own mg_mgr — so no shared mutable state between
 * threads beyond shutdown state and active-lease bookkeeping.
 *
 * --cpu-pin (Linux only) gives each worker an explicit CPU. Worker K pins
 * itself to the K'th CPU in the list, and the siever child process
 * inherits that affinity. Useful on heterogeneous CPUs (Zen 5, Intel
 * P+E-cores) where the OS migrating threads between CCDs/cache domains
 * tanks performance.
 */
#define _GNU_SOURCE             /* sched_setaffinity, cpu_set_t */
#define _POSIX_C_SOURCE 200809L

#include "protocol.h"
#include "sieve_executor.h"
#include "vendor/cJSON.h"
#include "vendor/mongoose.h"

#include <zstd.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sched.h>
#endif

#define CLIENT_MAX_WORKERS 256
#define SUBMIT_ZSTD_LEVEL 1

#define CLIENT_VERSION "0.1.0"

enum {
    SHUTDOWN_RUNNING = 0,
    SHUTDOWN_DRAINING = 1,
    SHUTDOWN_CANCELLING = 2
};

static volatile sig_atomic_t g_shutdown = SHUTDOWN_RUNNING;

typedef struct {
    int  has_lease;
    int  release_in_progress;
    char workunit_id[64];
    char client_id[64];
} active_lease_t;

static pthread_mutex_t g_active_mu = PTHREAD_MUTEX_INITIALIZER;
static active_lease_t *g_active = NULL;
static int g_active_count = 0;

static pthread_mutex_t g_http_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_http_cv = PTHREAD_COND_INITIALIZER;
static int g_http_limit = 16;
static int g_http_in_use = 0;
static int64_t g_http_interval_ms = 50;
static int64_t g_http_next_start_ms = 0;
static int g_http_error_streak = 0;

static pthread_mutex_t g_file_cache_mu = PTHREAD_MUTEX_INITIALIZER;

static int shutdown_phase(void)
{
    return (int)g_shutdown;
}

static void on_signal(int sig)
{
    (void)sig;
    if (g_shutdown == SHUTDOWN_RUNNING) {
        static const char msg[] =
            "\nclient: draining; finishing active work only. Press Ctrl-C again to release leases and exit.\n";
        ssize_t ignored;
        g_shutdown = SHUTDOWN_DRAINING;
        ignored = write(STDERR_FILENO, msg, sizeof(msg) - 1);
        (void)ignored;
    } else if (g_shutdown == SHUTDOWN_DRAINING) {
        static const char msg[] =
            "\nclient: cancelling; releasing active leases and exiting.\n";
        ssize_t ignored;
        g_shutdown = SHUTDOWN_CANCELLING;
        ignored = write(STDERR_FILENO, msg, sizeof(msg) - 1);
        (void)ignored;
    }
}

static int should_cancel_siever(void *ctx)
{
    (void)ctx;
    return shutdown_phase() >= SHUTDOWN_CANCELLING;
}

/* ===================== misc helpers ===================================== */

static int mkdir_p(const char *path)
{
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
    int sep = (la > 0 && a[la - 1] != '/');
    char *p = malloc(la + (size_t)sep + lb + 1);
    if (!p) return NULL;
    memcpy(p, a, la);
    if (sep) p[la] = '/';
    memcpy(p + la + (size_t)sep, b, lb);
    p[la + (size_t)sep + lb] = '\0';
    return p;
}

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int regular_file_size(const char *path, off_t *out_size)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    if (out_size) *out_size = st.st_size;
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

static void hex_encode(const unsigned char *in, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2*i]     = H[in[i] >> 4];
        out[2*i + 1] = H[in[i] & 0x0f];
    }
    out[2*n] = '\0';
}

static int sha256_file(const char *path, char hex_out[65])
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    mg_sha256_ctx ctx;
    mg_sha256_init(&ctx);
    unsigned char buf[64 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        mg_sha256_update(&ctx, buf, n);
    }
    int err = ferror(f);
    fclose(f);
    if (err) return -1;
    unsigned char dig[32];
    mg_sha256_final(dig, &ctx);
    hex_encode(dig, 32, hex_out);
    return 0;
}

static void sha256_buf(const void *buf, size_t len, char hex_out[65])
{
    mg_sha256_ctx ctx;
    mg_sha256_init(&ctx);
    mg_sha256_update(&ctx, (const unsigned char *)buf, len);
    unsigned char dig[32];
    mg_sha256_final(dig, &ctx);
    hex_encode(dig, 32, hex_out);
}

/* Read entire file into a malloc'd buffer; *out_len receives byte count. */
static unsigned char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    unsigned char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    int err = ferror(f);
    fclose(f);
    if (err || got != (size_t)sz) { free(buf); return NULL; }
    buf[got] = 0;
    *out_len = got;
    return buf;
}

static int write_file(const char *path, const void *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int rc = (fwrite(buf, 1, len, f) == len) ? 0 : -1;
    fclose(f);
    return rc;
}

static int write_file_atomic(const char *path, const void *buf, size_t len)
{
    size_t n = strlen(path) + 64;
    char *tmp = malloc(n);
    if (!tmp) return -1;
    snprintf(tmp, n, "%s.tmp.%ld", path, (long)getpid());

    int rc = write_file(tmp, buf, len);
    int saved_errno = errno;
    if (rc == 0 && rename(tmp, path) != 0) {
        rc = -1;
        saved_errno = errno;
    }
    if (rc != 0) {
        unlink(tmp);
        errno = saved_errno;
    }
    free(tmp);
    return rc;
}

static double elapsed_seconds(struct timeval start, struct timeval stop)
{
    return (double)(stop.tv_sec - start.tv_sec)
         + (double)(stop.tv_usec - start.tv_usec) / 1e6;
}

static int64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

static int cached_file_has_sha(const char *path, const char *sha_hex)
{
    char have[65];
    return file_exists(path) &&
           sha256_file(path, have) == 0 &&
           strcmp(have, sha_hex) == 0;
}

/* ===================== sync HTTP wrapper ================================ */

static void http_limiter_init(int limit, int64_t interval_ms)
{
    pthread_mutex_lock(&g_http_mu);
    g_http_limit = limit;
    g_http_interval_ms = interval_ms;
    g_http_in_use = 0;
    g_http_next_start_ms = 0;
    g_http_error_streak = 0;
    pthread_mutex_unlock(&g_http_mu);
}

static int http_sleep_ms(int64_t ms, int abort_on_cancel)
{
    while (ms > 0) {
        if (abort_on_cancel && shutdown_phase() >= SHUTDOWN_CANCELLING)
            return -1;
        int64_t chunk = ms > 100 ? 100 : ms;
        usleep((useconds_t)chunk * 1000);
        ms -= chunk;
    }
    return 0;
}

static int http_slot_acquire(int abort_on_cancel)
{
    for (;;) {
        int64_t wait_ms = 0;

        pthread_mutex_lock(&g_http_mu);
        while (g_http_limit > 0 && g_http_in_use >= g_http_limit) {
            if (abort_on_cancel && shutdown_phase() >= SHUTDOWN_CANCELLING) {
                pthread_mutex_unlock(&g_http_mu);
                return -1;
            }
            pthread_cond_wait(&g_http_cv, &g_http_mu);
        }

        int64_t now = monotonic_ms();
        if (g_http_next_start_ms > now) wait_ms = g_http_next_start_ms - now;
        if (wait_ms == 0) {
            g_http_in_use++;
            if (g_http_interval_ms > 0) {
                g_http_next_start_ms = now + g_http_interval_ms;
            }
            pthread_mutex_unlock(&g_http_mu);
            return 0;
        }

        pthread_mutex_unlock(&g_http_mu);
        if (http_sleep_ms(wait_ms, abort_on_cancel) != 0) return -1;
    }
}

static void http_slot_release(int ok)
{
    pthread_mutex_lock(&g_http_mu);
    if (g_http_in_use > 0) g_http_in_use--;
    if (ok) {
        g_http_error_streak = 0;
    } else {
        int64_t pause_ms;
        int64_t until_ms;
        if (g_http_error_streak < 20) g_http_error_streak++;
        pause_ms = 250 * (int64_t)g_http_error_streak;
        if (pause_ms > 5000) pause_ms = 5000;
        until_ms = monotonic_ms() + pause_ms;
        if (g_http_next_start_ms < until_ms) g_http_next_start_ms = until_ms;
    }
    pthread_cond_broadcast(&g_http_cv);
    pthread_mutex_unlock(&g_http_mu);
}

typedef struct {
    /* Request inputs */
    const char    *url;
    const char    *method;
    const char    *extra_headers;  /* trailing CRLF on each header line */
    const void    *body;
    size_t         body_len;

    /* State */
    int            sent;
    int            done;
    int            err;             /* 1 if connect/read failed */
    int            closed;
    int            status;          /* HTTP status code */

    /* Response body */
    unsigned char *resp_body;
    size_t         resp_body_len;
} http_io_t;

static void sync_http_handler(struct mg_connection *c, int ev, void *ev_data)
{
    http_io_t *io = (http_io_t *)c->fn_data;
    if (!io) {
        c->is_closing = 1;
        return;
    }

    if (ev == MG_EV_CONNECT) {
        /* Connection open — send the request line + headers + body. */
        const char    *path = mg_url_uri(io->url);
        struct mg_str  host = mg_url_host(io->url);
        unsigned short port = mg_url_port(io->url);
        if (port == 0) port = 80;

        mg_printf(c, "%s %s HTTP/1.1\r\n", io->method, path);
        mg_printf(c, "Host: %.*s:%u\r\n", (int)host.len, host.buf, port);
        mg_printf(c, "Connection: close\r\n");
        mg_printf(c, "Content-Length: %lu\r\n", (unsigned long)io->body_len);
        if (io->extra_headers) mg_printf(c, "%s", io->extra_headers);
        mg_printf(c, "\r\n");
        if (io->body_len > 0) mg_send(c, io->body, io->body_len);
        io->sent = 1;
    } else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        io->status = mg_http_status(hm);
        io->resp_body_len = hm->body.len;
        io->resp_body = malloc(hm->body.len + 1);
        if (io->resp_body) {
            memcpy(io->resp_body, hm->body.buf, hm->body.len);
            io->resp_body[hm->body.len] = 0;
        } else {
            io->err = 1;
        }
        io->done = 1;
        c->is_draining = 1;
    } else if (ev == MG_EV_ERROR) {
        const char *msg = (const char *)ev_data;
        if (msg && *msg) fprintf(stderr, "http: %s\n", msg);
        io->err = 1;
        io->done = 1;
    } else if (ev == MG_EV_CLOSE) {
        io->closed = 1;
        if (!io->done) {
            io->err = 1;
            io->done = 1;
        }
    }
}

/* Block until the request completes (or times out). Returns 0 on success
 * (HTTP status now filled in) or -1 on connection-level failure. */
static int http_request(struct mg_mgr *mgr, http_io_t *io, int timeout_ms,
                        int abort_on_cancel)
{
    io->sent = io->done = io->err = io->closed = 0;
    io->status = 0;
    io->resp_body = NULL;
    io->resp_body_len = 0;

    if (http_slot_acquire(abort_on_cancel) != 0) return -1;

    struct mg_connection *c = mg_http_connect(mgr, io->url, sync_http_handler, io);
    if (!c) {
        http_slot_release(0);
        return -1;
    }

    int waited_ms = 0;
    while (!io->done && waited_ms < timeout_ms) {
        if (abort_on_cancel && shutdown_phase() >= SHUTDOWN_CANCELLING) break;
        mg_mgr_poll(mgr, 200);
        waited_ms += 200;
    }
    if (!io->done) {
        c->is_closing = 1;
        for (int close_waited_ms = 0;
             !io->done && close_waited_ms < 1000;
             close_waited_ms += 50) {
            mg_mgr_poll(mgr, 50);
        }
    }
    if (!io->closed) c->fn_data = NULL;
    int rc = (!io->done || io->err) ? -1 : 0;
    http_slot_release(rc == 0);
    return rc;
}

static void http_io_free(http_io_t *io)
{
    if (io->resp_body) { free(io->resp_body); io->resp_body = NULL; }
    io->resp_body_len = 0;
}

/* ===================== top-level config ================================= */

typedef struct {
    char        server_url[256];
    char        token[80];
    char        client_id[64];
    char        siever_path[256];
    char        workdir[256];
    char        file_cache_dir[256];
    int64_t     idle_backoff_seconds;
    int64_t     http_interval_ms;
    int         workers;
    int         http_concurrency;
    int         once;

    /* --cpu-pin parsed list. cpu_pin_count == 0 disables pinning. */
    int         cpu_pin_count;
    int         cpu_pin_list[CLIENT_MAX_WORKERS];
} client_cfg_t;

static void usage(void)
{
    fprintf(stderr,
        "usage: ggnfs-sieve-client \\\n"
        "    --server-url=http://host:port  (required)\n"
        "    --token=<bearer token>         (required)\n"
        "    --siever=<path>                (required) gnfs-lasieve4* binary\n"
        "    [--client-id=<name>]           label this worker on the dashboard; defaults to hostname\n"
        "    [--workdir=/tmp/ggnfs-client]\n"
        "    [--idle-backoff=30]\n"
        "    [--workers=1]                  pthread workers (each runs an independent siever)\n"
        "    [--http-concurrency=16]        max simultaneous coordinator HTTP requests\n"
        "    [--http-interval-ms=50]        minimum spacing between coordinator HTTP starts\n"
        "    [--cpu-pin=0,2,4,6]            (Linux) pin worker K to the K'th CPU in the list;\n"
        "                                   list length must equal --workers\n"
        "    [--once]                       single-shot: each worker does one workunit and exits\n");
}

static int parse_config(int argc, char **argv, client_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->idle_backoff_seconds = 30;
    cfg->http_interval_ms = 50;
    cfg->workers = 1;
    cfg->http_concurrency = 16;
    snprintf(cfg->workdir, sizeof(cfg->workdir), "%s", "/tmp/ggnfs-client");

    const char *url     = flag(argc, argv, "--server-url");
    const char *token   = flag(argc, argv, "--token");
    const char *siever  = flag(argc, argv, "--siever");
    const char *cid     = flag(argc, argv, "--client-id");
    const char *wdir    = flag(argc, argv, "--workdir");
    const char *ib      = flag(argc, argv, "--idle-backoff");
    const char *workers = flag(argc, argv, "--workers");
    const char *httpc   = flag(argc, argv, "--http-concurrency");
    const char *httpi   = flag(argc, argv, "--http-interval-ms");
    const char *once    = flag(argc, argv, "--once");

    if (!url || !*url || !token || !*token || !siever || !*siever) {
        usage();
        return -1;
    }
    snprintf(cfg->server_url,  sizeof(cfg->server_url),  "%s", url);
    snprintf(cfg->token,       sizeof(cfg->token),       "%s", token);
    snprintf(cfg->siever_path, sizeof(cfg->siever_path), "%s", siever);

    if (cid && *cid) {
        snprintf(cfg->client_id, sizeof(cfg->client_id), "%s", cid);
    } else {
        char host[64];
        if (gethostname(host, sizeof(host)) != 0) snprintf(host, sizeof(host), "unknown");
        host[sizeof(host) - 1] = '\0';
        snprintf(cfg->client_id, sizeof(cfg->client_id), "%s", host);
    }

    if (wdir && *wdir) snprintf(cfg->workdir, sizeof(cfg->workdir), "%s", wdir);
    if (snprintf(cfg->file_cache_dir, sizeof(cfg->file_cache_dir), "%s/files",
                 cfg->workdir) >= (int)sizeof(cfg->file_cache_dir)) {
        fprintf(stderr, "client: --workdir too long\n");
        return -1;
    }

    if (ib && *ib && parse_int64_arg(ib, &cfg->idle_backoff_seconds) != 0) {
        fprintf(stderr, "client: --idle-backoff must be an integer\n");
        return -1;
    }
    if (cfg->idle_backoff_seconds < 1) cfg->idle_backoff_seconds = 1;

    if (workers && *workers) {
        int64_t n;
        if (parse_int64_arg(workers, &n) != 0 || n < 1 || n > CLIENT_MAX_WORKERS) {
            fprintf(stderr, "client: --workers must be an integer in 1..%d\n",
                    CLIENT_MAX_WORKERS);
            return -1;
        }
        cfg->workers = (int)n;
    }

    if (httpc && *httpc) {
        int64_t n;
        if (parse_int64_arg(httpc, &n) != 0 || n < 1 || n > CLIENT_MAX_WORKERS) {
            fprintf(stderr, "client: --http-concurrency must be an integer in 1..%d\n",
                    CLIENT_MAX_WORKERS);
            return -1;
        }
        cfg->http_concurrency = (int)n;
    }

    if (httpi && *httpi) {
        int64_t n;
        if (parse_int64_arg(httpi, &n) != 0 || n < 0 || n > 60000) {
            fprintf(stderr, "client: --http-interval-ms must be an integer in 0..60000\n");
            return -1;
        }
        cfg->http_interval_ms = n;
    }

    const char *cpu_pin = flag(argc, argv, "--cpu-pin");
    if (cpu_pin && *cpu_pin) {
        char tmp[1024];
        snprintf(tmp, sizeof(tmp), "%s", cpu_pin);
        char *p = tmp;
        cfg->cpu_pin_count = 0;
        while (*p) {
            char *end = NULL;
            errno = 0;
            long v = strtol(p, &end, 10);
            if (end == p || errno != 0 || v < 0 || v > 4095) {
                fprintf(stderr, "client: bad --cpu-pin entry near '%s'\n", p);
                return -1;
            }
            if (cfg->cpu_pin_count >= CLIENT_MAX_WORKERS) {
                fprintf(stderr, "client: --cpu-pin has more than %d entries\n",
                        CLIENT_MAX_WORKERS);
                return -1;
            }
            cfg->cpu_pin_list[cfg->cpu_pin_count++] = (int)v;
            p = end;
            while (*p == ',' || *p == ' ') p++;
        }
        if (cfg->cpu_pin_count != cfg->workers) {
            fprintf(stderr,
                "client: --cpu-pin has %d entries but --workers=%d (must match)\n",
                cfg->cpu_pin_count, cfg->workers);
            return -1;
        }
#ifndef __linux__
        fprintf(stderr,
            "client: --cpu-pin ignored on non-Linux builds (sched_setaffinity unavailable)\n");
        cfg->cpu_pin_count = 0;
#endif
    }

    cfg->once = (once != NULL);
    return 0;
}

/* ===================== one iteration ==================================== */

/* Build a header buffer with the bearer token. The trailing CRLF is included.
 * Caller-supplied `extra` can add more headers (each ending with \r\n). */
static void build_auth_headers(char *out, size_t out_n, const char *token,
                               const char *content_type, const char *extra)
{
    if (extra)
        snprintf(out, out_n,
                 "Authorization: Bearer %s\r\nContent-Type: %s\r\n%s",
                 token, content_type, extra);
    else
        snprintf(out, out_n,
                 "Authorization: Bearer %s\r\nContent-Type: %s\r\n",
                 token, content_type);
}

static int join_url(char *out, size_t out_n, const char *base, const char *suffix)
{
    size_t blen = strlen(base);
    int has_slash = (blen > 0 && base[blen - 1] == '/');
    int starts_slash = (suffix[0] == '/');
    const char *sep = (has_slash && starts_slash) ? "" :
                      (!has_slash && !starts_slash) ? "/" : "";
    return snprintf(out, out_n, "%s%s%s", base, sep, suffix) >= (int)out_n ? -1 : 0;
}

static void active_lease_set(int idx, const char *workunit_id, const char *client_id)
{
    if (idx < 0 || idx >= g_active_count || !g_active) return;
    pthread_mutex_lock(&g_active_mu);
    memset(&g_active[idx], 0, sizeof(g_active[idx]));
    g_active[idx].has_lease = 1;
    snprintf(g_active[idx].workunit_id, sizeof(g_active[idx].workunit_id),
             "%s", workunit_id ? workunit_id : "");
    snprintf(g_active[idx].client_id, sizeof(g_active[idx].client_id),
             "%s", client_id ? client_id : "");
    pthread_mutex_unlock(&g_active_mu);
}

static void active_lease_clear(int idx)
{
    if (idx < 0 || idx >= g_active_count || !g_active) return;
    pthread_mutex_lock(&g_active_mu);
    memset(&g_active[idx], 0, sizeof(g_active[idx]));
    pthread_mutex_unlock(&g_active_mu);
}

static int active_lease_claim_release(int idx, active_lease_t *out)
{
    int claimed = 0;
    if (idx < 0 || idx >= g_active_count || !g_active || !out) return 0;

    pthread_mutex_lock(&g_active_mu);
    if (g_active[idx].has_lease && !g_active[idx].release_in_progress) {
        *out = g_active[idx];
        g_active[idx].release_in_progress = 1;
        claimed = 1;
    }
    pthread_mutex_unlock(&g_active_mu);
    return claimed;
}

static void active_lease_finish_release(int idx, const active_lease_t *lease,
                                        int release_rc)
{
    if (idx < 0 || idx >= g_active_count || !g_active || !lease) return;

    pthread_mutex_lock(&g_active_mu);
    if (g_active[idx].has_lease &&
        strcmp(g_active[idx].workunit_id, lease->workunit_id) == 0 &&
        strcmp(g_active[idx].client_id, lease->client_id) == 0) {
        if (release_rc == 0)
            memset(&g_active[idx], 0, sizeof(g_active[idx]));
        else
            g_active[idx].release_in_progress = 0;
    }
    pthread_mutex_unlock(&g_active_mu);
}

/* Fetch /file/<sha> into the shared cache. No-op if already present. */
static int ensure_file_cached(struct mg_mgr *mgr, const client_cfg_t *cfg,
                              const proto_lease_response_t *lease,
                              char *out_path, size_t out_path_n)
{
    if (mkdir_p(cfg->file_cache_dir) != 0) return -1;
    char *local = path_join(cfg->file_cache_dir, lease->file_sha256_hex);
    if (!local) return -1;
    snprintf(out_path, out_path_n, "%s", local);

    pthread_mutex_lock(&g_file_cache_mu);

    if (cached_file_has_sha(local, lease->file_sha256_hex)) {
        pthread_mutex_unlock(&g_file_cache_mu);
        free(local);
        return 0;
    }

    if (file_exists(local)) {
        unlink(local);
    }

    char url[512];
    if (join_url(url, sizeof(url), cfg->server_url, lease->file_url) != 0) {
        fprintf(stderr, "client: file url too long\n");
        pthread_mutex_unlock(&g_file_cache_mu);
        free(local);
        return -1;
    }

    char headers[256];
    build_auth_headers(headers, sizeof(headers), cfg->token,
                       "application/octet-stream", NULL);
    http_io_t io = {
        .url = url, .method = "GET",
        .extra_headers = headers,
        .body = NULL, .body_len = 0,
    };
    int rc = http_request(mgr, &io, 60000, 1);
    if (rc != 0) {
        fprintf(stderr, "client: file fetch failed (connection)\n");
        pthread_mutex_unlock(&g_file_cache_mu);
        http_io_free(&io); free(local);
        return -1;
    }
    if (io.status != 200) {
        fprintf(stderr, "client: file fetch returned HTTP %d\n", io.status);
        pthread_mutex_unlock(&g_file_cache_mu);
        http_io_free(&io); free(local);
        return -1;
    }
    if (write_file_atomic(local, io.resp_body, io.resp_body_len) != 0) {
        fprintf(stderr, "client: cannot write %s: %s\n", local, strerror(errno));
        pthread_mutex_unlock(&g_file_cache_mu);
        http_io_free(&io); free(local);
        return -1;
    }
    http_io_free(&io);

    char have[65] = {0};
    if (sha256_file(local, have) != 0 ||
        strcmp(have, lease->file_sha256_hex) != 0) {
        fprintf(stderr, "client: fetched file sha mismatch (got %s, want %s)\n",
                have, lease->file_sha256_hex);
        unlink(local);
        pthread_mutex_unlock(&g_file_cache_mu);
        free(local);
        return -1;
    }
    pthread_mutex_unlock(&g_file_cache_mu);
    free(local);
    return 0;
}

/* Returns:
 *    1  - got a workunit, *out filled
 *    0  - 204 No Content (job still running, just no work right now)
 *   -1  - 410 Gone (job complete, caller should exit)
 *   -2  - other error (caller should backoff)
 */
static int do_lease(struct mg_mgr *mgr, const client_cfg_t *cfg,
                    proto_lease_response_t *out)
{
    char url[512];
    if (join_url(url, sizeof(url), cfg->server_url, "/lease") != 0) return -2;

    char *body = proto_encode_lease_request(cfg->client_id, CLIENT_VERSION);
    if (!body) return -2;
    size_t body_len = strlen(body);

    char headers[256];
    build_auth_headers(headers, sizeof(headers), cfg->token,
                       "application/json", NULL);

    http_io_t io = {
        .url = url, .method = "POST",
        .extra_headers = headers,
        .body = body, .body_len = body_len,
    };
    int rc = http_request(mgr, &io, 30000, 1);
    free(body);

    if (rc != 0) { http_io_free(&io); return -2; }

    int result;
    switch (io.status) {
        case 200:
            if (proto_decode_lease_response((char *)io.resp_body, io.resp_body_len, out) != 0) {
                fprintf(stderr, "client: malformed /lease response\n");
                result = -2;
            } else {
                result = 1;
            }
            break;
        case 204:
            result = 0;
            break;
        case 410:
            result = -1;
            break;
        case 401:
            fprintf(stderr, "client: 401 unauthorized — token wrong?\n");
            result = -2;
            break;
        default:
            fprintf(stderr, "client: /lease returned HTTP %d\n", io.status);
            result = -2;
            break;
    }
    http_io_free(&io);
    return result;
}

/* Returns 0 on success (workunit accepted), 1 on 409 (workunit was reissued),
 * -1 on transient errors worth retrying, -2 on permanent submit rejection. */
static int do_submit(struct mg_mgr *mgr, const client_cfg_t *cfg,
                     const proto_lease_response_t *lease,
                     const char *outfile_path, double sieve_seconds)
{
    size_t raw_len = 0;
    unsigned char *raw = read_file(outfile_path, &raw_len);
    if (!raw) {
        fprintf(stderr, "client: cannot read %s: %s\n", outfile_path, strerror(errno));
        return -1;
    }

    size_t bound = ZSTD_compressBound(raw_len);
    unsigned char *body = malloc(bound);
    if (!body) {
        fprintf(stderr, "client: zstd output allocation failed\n");
        free(raw);
        return -1;
    }
    size_t body_len = ZSTD_compress(body, bound, raw, raw_len, SUBMIT_ZSTD_LEVEL);
    free(raw);
    if (ZSTD_isError(body_len)) {
        fprintf(stderr, "client: zstd compress failed: %s\n", ZSTD_getErrorName(body_len));
        free(body);
        return -1;
    }

    char body_sha[65];
    sha256_buf(body, body_len, body_sha);

    char url[512];
    if (join_url(url, sizeof(url), cfg->server_url, "/submit") != 0) {
        free(body);
        return -1;
    }

    char xtra[512];
    snprintf(xtra, sizeof(xtra),
             "X-Workunit-Id: %s\r\n"
             "X-Client-Id: %s\r\n"
             "X-Sha256: %s\r\n"
             "X-Sieve-Seconds: %.3f\r\n",
             lease->workunit_id, cfg->client_id, body_sha, sieve_seconds);
    snprintf(xtra + strlen(xtra), sizeof(xtra) - strlen(xtra),
             "X-Compression: zstd\r\n"
             "X-Uncompressed-Bytes: %llu\r\n"
             "X-Zstd-Level: %d\r\n",
             (unsigned long long)raw_len, SUBMIT_ZSTD_LEVEL);

    char headers[768];
    build_auth_headers(headers, sizeof(headers), cfg->token,
                       "application/octet-stream", xtra);

    http_io_t io = {
        .url = url, .method = "POST",
        .extra_headers = headers,
        .body = body, .body_len = body_len,
    };
    int rc = http_request(mgr, &io, 60000, 1);
    free(body);
    if (rc != 0) {
        http_io_free(&io);
        fprintf(stderr, "client: /submit connection failure\n");
        return -1;
    }

    int result;
    if (io.status == 200) {
        printf("client: submitted %s (%lld bytes raw -> %lld bytes zstd, %.3fs)\n",
               lease->workunit_id, (long long)raw_len, (long long)body_len,
               sieve_seconds);
        result = 0;
    } else if (io.status == 409) {
        fprintf(stderr, "client: 409 conflict on %s (re-issued?)\n", lease->workunit_id);
        result = 1;
    } else if (io.status >= 500 || io.status == 0) {
        fprintf(stderr, "client: /submit returned HTTP %d\n", io.status);
        result = -1;
    } else {
        fprintf(stderr, "client: /submit returned HTTP %d (not retrying)\n", io.status);
        result = -2;
    }
    http_io_free(&io);
    return result;
}

/* Keep a completed relation file tied to its active lease until the server
 * accepts it or tells us that the workunit has already moved on. */
static int submit_with_retries(struct mg_mgr *mgr, const client_cfg_t *cfg,
                               const proto_lease_response_t *lease,
                               const char *outfile_path, double sieve_seconds)
{
    int attempt = 0;

    for (;;) {
        int sr = do_submit(mgr, cfg, lease, outfile_path, sieve_seconds);
        if (sr == 0 || sr == 1 || sr == -2) return sr;

        attempt++;
        if (shutdown_phase() >= SHUTDOWN_CANCELLING) return -1;

        fprintf(stderr,
                "client: will retry /submit for %s in %llds (retry %d)\n",
                lease->workunit_id, (long long)cfg->idle_backoff_seconds,
                attempt);
        for (int64_t i = 0;
             i < cfg->idle_backoff_seconds &&
             shutdown_phase() < SHUTDOWN_CANCELLING;
             i++) {
            sleep(1);
        }
    }
}

/* Returns 0 if the server released the lease or no longer has that lease for us;
 * -1 on connection/server errors. Treat 409 as non-fatal during shutdown because
 * the lease may already have expired or been submitted. */
static int do_release(struct mg_mgr *mgr, const client_cfg_t *cfg,
                      const char *workunit_id)
{
    char url[512];
    if (join_url(url, sizeof(url), cfg->server_url, "/release") != 0) return -1;

    char *body = proto_encode_release_request(workunit_id, cfg->client_id);
    if (!body) return -1;
    size_t body_len = strlen(body);

    char headers[256];
    build_auth_headers(headers, sizeof(headers), cfg->token,
                       "application/json", NULL);

    http_io_t io = {
        .url = url, .method = "POST",
        .extra_headers = headers,
        .body = body, .body_len = body_len,
    };
    int rc = http_request(mgr, &io, 10000, 0);
    free(body);
    if (rc != 0) {
        http_io_free(&io);
        fprintf(stderr, "client: /release connection failure for %s\n", workunit_id);
        return -1;
    }
    if (io.status == 200) {
        printf("client: released %s\n", workunit_id);
        http_io_free(&io);
        return 0;
    }
    if (io.status == 409) {
        fprintf(stderr, "client: release skipped for %s (not leased to us)\n", workunit_id);
        http_io_free(&io);
        return 0;
    }
    fprintf(stderr, "client: /release returned HTTP %d for %s\n", io.status, workunit_id);
    http_io_free(&io);
    return -1;
}

static int release_active_lease(struct mg_mgr *mgr, const client_cfg_t *base_cfg,
                                int worker_idx)
{
    active_lease_t lease;
    if (!active_lease_claim_release(worker_idx, &lease))
        return 0;

    client_cfg_t cfg = *base_cfg;
    snprintf(cfg.client_id, sizeof(cfg.client_id), "%s", lease.client_id);
    int rc = do_release(mgr, &cfg, lease.workunit_id);
    active_lease_finish_release(worker_idx, &lease, rc);
    return rc;
}

static void release_active_leases(const client_cfg_t *base_cfg)
{
    int n = 0;

    pthread_mutex_lock(&g_active_mu);
    n = g_active_count;
    if (n > CLIENT_MAX_WORKERS) n = CLIENT_MAX_WORKERS;
    pthread_mutex_unlock(&g_active_mu);

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    for (int i = 0; i < n; i++) {
        release_active_lease(&mgr, base_cfg, i);
    }
    mg_mgr_free(&mgr);
}

/* The full lease -> fetch -> sieve -> submit cycle. Returns:
 *   1  - completed a workunit (caller may continue immediately)
 *   0  - no work right now (caller should backoff)
 *  -1  - job is done (caller should exit cleanly)
 *  -2  - transient failure (caller should backoff)
 */
static int run_one_iteration(struct mg_mgr *mgr, const client_cfg_t *cfg, int worker_idx)
{
    if (shutdown_phase() >= SHUTDOWN_DRAINING) return -1;

    proto_lease_response_t lease;
    int lr = do_lease(mgr, cfg, &lease);
    if (lr == 0)  return 0;
    if (lr == -1) return -1;
    if (lr <  0)  return -2;
    if (shutdown_phase() >= SHUTDOWN_DRAINING) {
        do_release(mgr, cfg, lease.workunit_id);
        return -1;
    }

    printf("client: leased %s  q=[%lld,%lld)  side=%c  siever=%s\n",
           lease.workunit_id,
           (long long)lease.q_start,
           (long long)(lease.q_start + lease.q_range),
           lease.side, lease.siever);
    active_lease_set(worker_idx, lease.workunit_id, cfg->client_id);

    /* Fetch input file (the .job) into the local cache. */
    char job_local[256];
    if (ensure_file_cached(mgr, cfg, &lease, job_local, sizeof(job_local)) != 0) {
        if (shutdown_phase() >= SHUTDOWN_DRAINING)
            release_active_lease(mgr, cfg, worker_idx);
        return -2;
    }

    /* Local outfile path for the siever to write into. Use the workunit id
     * rather than the server's generic output name so concurrent client
     * processes sharing a workdir cannot unlink/replace each other's output. */
    char output_name[sizeof(lease.workunit_id) + 4];
    snprintf(output_name, sizeof(output_name), "%s.dat", lease.workunit_id);
    char *outfile = path_join(cfg->workdir, output_name);
    if (!outfile) {
        if (shutdown_phase() >= SHUTDOWN_DRAINING)
            release_active_lease(mgr, cfg, worker_idx);
        return -2;
    }

    /* The server tells us which siever name to use; we trust the operator
     * to have given --siever pointing at the right binary on disk. We can
     * surface the mismatch as a warning. */
    const char *siever_basename = strrchr(cfg->siever_path, '/');
    siever_basename = siever_basename ? siever_basename + 1 : cfg->siever_path;
    if (strcmp(siever_basename, lease.siever) != 0) {
        fprintf(stderr, "client: WARNING — server requested '%s' but --siever is '%s'\n",
                lease.siever, siever_basename);
    }

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    int sieve_rc = sieve_run_local(cfg->siever_path, job_local, outfile,
                                   (uint32_t)lease.q_start,
                                   (uint32_t)lease.q_range,
                                   lease.side,
                                   lease.siever_args,
                                   should_cancel_siever,
                                   NULL);
    gettimeofday(&t1, NULL);
    double sieve_seconds = elapsed_seconds(t0, t1);

    if (sieve_rc != 0) {
        fprintf(stderr, "client: siever returned %d (skipping submit)\n", sieve_rc);
        if (shutdown_phase() >= SHUTDOWN_DRAINING)
            release_active_lease(mgr, cfg, worker_idx);
        free(outfile);
        return -2;
    }
    off_t outfile_size = 0;
    if (regular_file_size(outfile, &outfile_size) != 0) {
        fprintf(stderr, "client: siever did not produce %s (skipping submit)\n", outfile);
        if (shutdown_phase() >= SHUTDOWN_DRAINING)
            release_active_lease(mgr, cfg, worker_idx);
        free(outfile);
        return -2;
    }
    if (outfile_size == 0) {
        fprintf(stderr, "client: siever produced empty %s (skipping submit)\n", outfile);
        if (shutdown_phase() >= SHUTDOWN_DRAINING)
            release_active_lease(mgr, cfg, worker_idx);
        unlink(outfile);
        free(outfile);
        return -2;
    }

    int sr = submit_with_retries(mgr, cfg, &lease, outfile, sieve_seconds);
    if (sr == -1) {
        fprintf(stderr, "client: submit cancelled; leaving %s for inspection\n", outfile);
        free(outfile);
        return -2;
    }
    if (sr == -2) {
        fprintf(stderr, "client: submit failed permanently; leaving %s for inspection\n",
                outfile);
        active_lease_clear(worker_idx);
        free(outfile);
        return -2;
    }

    active_lease_clear(worker_idx);
    /* Tidy: remove the local rels file once the server accepted it, or once
     * the server says the workunit has already moved on. */
    unlink(outfile);
    free(outfile);
    return 1;
}
/* ===================== worker thread ==================================== */

typedef struct {
    int                 idx;
    const client_cfg_t *base_cfg;
} worker_args_t;

/* One worker = one mg_mgr + one workdir + one client_id. */
static void *worker_main(void *arg)
{
    worker_args_t      *wa  = (worker_args_t *)arg;
    int                 idx = wa->idx;
    client_cfg_t        cfg = *wa->base_cfg;  /* per-thread copy */

    /* Per-worker workdir: <base>/wN. Truncate base to leave room for "/wNNN". */
    snprintf(cfg.workdir, sizeof(cfg.workdir), "%.*s/w%d",
             (int)(sizeof(cfg.workdir) - 8), wa->base_cfg->workdir, idx);

    /* Per-worker client-id: <base>-wN. */
    snprintf(cfg.client_id, sizeof(cfg.client_id), "%.*s-w%d",
             (int)(sizeof(cfg.client_id) - 8), wa->base_cfg->client_id, idx);

    if (mkdir_p(cfg.workdir) != 0) return NULL;

#ifdef __linux__
    /* Pin this worker and its siever children, which inherit affinity from the
     * calling thread, to the assigned CPU. */
    if (cfg.cpu_pin_count > 0) {
        int cpu = cfg.cpu_pin_list[idx];
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
            fprintf(stderr, "  [w%d] pthread_setaffinity_np(%d) failed: %s\n",
                    idx, cpu, strerror(errno));
        }
    }
#endif

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    if (cfg.cpu_pin_count > 0) {
        fprintf(stderr, "  [w%d] workdir=%s  client_id=%s  cpu=%d\n",
                idx, cfg.workdir, cfg.client_id, cfg.cpu_pin_list[idx]);
    } else {
        fprintf(stderr, "  [w%d] workdir=%s  client_id=%s\n",
                idx, cfg.workdir, cfg.client_id);
    }

    while (shutdown_phase() < SHUTDOWN_CANCELLING) {
        int r = run_one_iteration(&mgr, &cfg, idx);
        if (r == 1) {
            if (cfg.once) break;
            continue;
        }
        if (r == -1) {
            if (shutdown_phase() >= SHUTDOWN_DRAINING)
                printf("[w%d] drain requested — exiting\n", idx);
            else
                printf("[w%d] server reports job complete — exiting\n", idx);
            break;
        }
        if (shutdown_phase() >= SHUTDOWN_DRAINING) break;
        /* r == 0 (no work) or r == -2 (transient failure) — backoff. */
        for (int64_t i = 0;
             i < cfg.idle_backoff_seconds && shutdown_phase() == SHUTDOWN_RUNNING;
             i++) {
            sleep(1);
        }
    }

    mg_mgr_free(&mgr);
    return NULL;
}

/* ===================== main ============================================= */

int main(int argc, char **argv)
{
    client_cfg_t cfg;
    if (parse_config(argc, argv, &cfg) != 0) return 2;

    if (mkdir_p(cfg.workdir) != 0) return 1;

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    mg_log_set(MG_LL_ERROR);  /* quiet by default; raise to debug */

    fprintf(stderr,
        "ggnfs-sieve-client: %s\n"
        "  server   : %s\n"
        "  client_id: %s\n"
        "  siever   : %s\n"
        "  workdir  : %s\n"
        "  workers  : %d\n"
        "  http max : %d\n"
        "  http gap : %lldms\n"
        "  backoff  : %llds   once=%d\n",
        CLIENT_VERSION, cfg.server_url, cfg.client_id, cfg.siever_path,
        cfg.workdir, cfg.workers, cfg.http_concurrency,
        (long long)cfg.http_interval_ms,
        (long long)cfg.idle_backoff_seconds, cfg.once);

    http_limiter_init(cfg.http_concurrency, cfg.http_interval_ms);

    pthread_t     *tids = calloc((size_t)cfg.workers, sizeof(pthread_t));
    worker_args_t *args = calloc((size_t)cfg.workers, sizeof(worker_args_t));
    int           *joined = calloc((size_t)cfg.workers, sizeof(int));
    g_active = calloc((size_t)cfg.workers, sizeof(active_lease_t));
    g_active_count = cfg.workers;
    if (!tids || !args || !joined || !g_active) {
        fprintf(stderr, "client: alloc failed\n");
        free(tids); free(args); free(joined); free(g_active);
        g_active = NULL; g_active_count = 0;
        return 1;
    }

    int spawned = 0;
    for (int i = 0; i < cfg.workers; i++) {
        args[i].idx      = i;
        args[i].base_cfg = &cfg;
        if (pthread_create(&tids[i], NULL, worker_main, &args[i]) != 0) {
            fprintf(stderr, "client: pthread_create failed for worker %d: %s\n",
                    i, strerror(errno));
            g_shutdown = SHUTDOWN_CANCELLING;
            break;
        }
        spawned++;
    }

    int done = 0;
    int released_on_cancel = 0;
    while (done < spawned) {
        for (int i = 0; i < spawned; i++) {
            if (joined[i]) continue;
            if (pthread_tryjoin_np(tids[i], NULL) == 0) {
                joined[i] = 1;
                done++;
            }
        }
        if (shutdown_phase() >= SHUTDOWN_CANCELLING && !released_on_cancel) {
            release_active_leases(&cfg);
            released_on_cancel = 1;
        }
        if (done < spawned) usleep(100000);
    }
    if (shutdown_phase() >= SHUTDOWN_DRAINING && !released_on_cancel)
        release_active_leases(&cfg);

    free(tids);
    free(args);
    free(joined);
    free(g_active);
    g_active = NULL;
    g_active_count = 0;
    return 0;
}
