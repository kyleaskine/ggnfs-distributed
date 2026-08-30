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
 *
 * `ggnfs-sieve-client benchmark ...` is a separate subcommand: a fixed-work
 * speed test against the server's real job that screens a rented box (good vs.
 * oversubscribed/contended) in a couple of minutes. See run_benchmark().
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

enum { ENGINE_LASIEVE4 = 0, ENGINE_CUDA = 1 };

static const char *engine_name(int e)
{
    return e == ENGINE_CUDA ? "cuda" : "lasieve4";
}
#define SUBMIT_ZSTD_LEVEL 1

#define CLIENT_VERSION "0.2.0"

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

typedef struct client_cfg_s client_cfg_t;
static int64_t monotonic_ms(void);
static int do_renew(struct mg_mgr *mgr, const client_cfg_t *cfg,
                    const char *workunit_id);

/* Passed to sieve_run_local as the cancellation context. wait_child_cancelable
 * polls that callback about every 100 ms while the siever runs, which is also
 * the only place a worker is awake during a long sieve — so it is where the
 * lease heartbeat belongs. */
typedef struct {
    struct mg_mgr      *mgr;
    const client_cfg_t *cfg;
    const char         *workunit_id;
    int64_t             next_renew_ms;   /* 0 = heartbeat disabled */
    int64_t             interval_ms;     /* cadence, from the lease window */
    int                 lease_lost;      /* set if the server 409'd us */
} sieve_ctx_t;

/* Latched once per process: an older coordinator has no /renew at all, and
 * re-probing it on every workunit would burn a round trip and a warning line
 * per band per worker forever. */
static int g_renew_unsupported = 0;

static int should_cancel_siever(void *ctx)
{
    if (shutdown_phase() >= SHUTDOWN_CANCELLING) return 1;

    sieve_ctx_t *sc = (sieve_ctx_t *)ctx;
    if (!sc) return 0;
    /* Must precede the next_renew_ms test: the lease-lost branch below zeroes
     * next_renew_ms, so checking that first would swallow this and let the
     * siever keep grinding on a workunit we no longer hold. */
    if (sc->lease_lost) return 1;
    if (sc->next_renew_ms == 0 || g_renew_unsupported) return 0;

    /* Draining deliberately keeps heartbeating. Drain is the phase where the
     * client finishes a long band it fully intends to submit, so dropping the
     * heartbeat here is exactly when losing the lease costs the most. The
     * second Ctrl-C stays responsive because do_renew runs on a 5s budget with
     * abort_on_cancel, not because we stop renewing. */

    int64_t now = monotonic_ms();
    if (now < sc->next_renew_ms) return 0;

    int r = do_renew(sc->mgr, sc->cfg, sc->workunit_id);
    if (r == 1) {
        /* Positive knowledge that the workunit was reclaimed and reissued.
         * Every further second of sieving is waste and the submit is
         * guaranteed to 409, so stop now — on a gpu-class band that can save
         * hours. The caller checks lease_lost to skip the submit. */
        fprintf(stderr, "client: %s was reclaimed by the server (lease "
                        "expired); abandoning it\n", sc->workunit_id);
        sc->lease_lost = 1;
        sc->next_renew_ms = 0;
        return 1;
    }
    if (r == 2) {
        /* Server predates /renew. The lease is fine — we just cannot extend
         * it, exactly as before this feature existed. Keep sieving, and latch
         * it process-wide so we stop asking. */
        if (!g_renew_unsupported) {
            fprintf(stderr, "client: server has no /renew; lease heartbeat "
                            "disabled for this run\n");
            g_renew_unsupported = 1;
        }
        sc->next_renew_ms = 0;
        return 0;
    }
    /* On a transient failure retry at the normal cadence rather than
     * hammering a server that is already unhappy. */
    sc->next_renew_ms = now + sc->interval_ms;
    return 0;
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
    while (end && isspace((unsigned char)*end)) end++;
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

typedef struct client_cfg_s {
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
    /* Workunit class to request from /lease. "cpu" (default) takes
     * lasieve4-sized bands; "gpu" prefers the wide bands carved by
     * `extend --class=gpu` and falls back to cpu ones when they run out. */
    char        lease_class[16];
    /* Which siever binary this client drives. The workunit's class sizes the
     * band; the engine decides what runs it. They are independent: a cuda
     * client that falls back to a cpu-class workunit still runs cuda. */
    int         engine;                 /* ENGINE_LASIEVE4 | ENGINE_CUDA */
    char        cuda_bench[256];        /* cuda-sieve `bench` binary */
    char        fb1_path[256];          /* explicit --fb1 cache; "" = none */
    char        fbgen_gpu[256];         /* fbgen_gpu binary to build one with */
    int         cuda_device;            /* -1 = let bench choose */
    /* Lease slots per worker. >1 runs the pipelined worker, which keeps the
     * card sieving across lease round trips and uploads. */
    int         prefetch;

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
        "    --siever=<path>                gnfs-lasieve4* binary (required unless\n"
        "                                   --engine=cuda)\n"
        "    [--client-id=<name>]           label this worker on the dashboard; defaults to hostname\n"
        "    [--workdir=/tmp/ggnfs-client]\n"
        "    [--idle-backoff=30]\n"
        "    [--lease-class=cpu|gpu]        workunit class to request (default: cpu,\n"
        "                                   or gpu under --engine=cuda)\n"
        "    [--engine=lasieve4|cuda]       which siever to drive (default lasieve4).\n"
        "                                   'cuda' runs cuda-sieve's bench on a GPU\n"
        "    [--cuda-bench=<path>]          (required for --engine=cuda) bench binary\n"
        "    [--fb1=<path>]                 pre-generated cuda-sieve factor-base cache\n"
        "    [--fbgen-gpu=<path>]           cuda-sieve fbgen_gpu binary; builds the --fb1\n"
        "                                   cache once per job. Without either, bench\n"
        "                                   rebuilds the factor base every workunit\n"
        "    [--device=N]                   CUDA device index (default: bench chooses)\n"
        "    [--prefetch=N]                 lease slots per worker (default: 2 under\n"
        "                                   --engine=cuda, else 1). >1 overlaps leasing\n"
        "                                   and uploading with sieving\n"
        "    [--workers=1]                  pthread workers (each runs an independent siever)\n"
        "    [--http-concurrency=16]        max simultaneous coordinator HTTP requests\n"
        "    [--http-interval-ms=50]        minimum spacing between coordinator HTTP starts\n"
        "    [--cpu-pin=0,2,4,6]            (Linux) pin worker K to the K'th CPU in the list;\n"
        "                                   list length must equal --workers\n"
        "    [--once]                       single-shot: each worker does one workunit and exits\n"
        "\n"
        "  ggnfs-sieve-client benchmark --server-url=... --token=... --siever=...\n"
        "    ~1-minute-per-phase speed test against the real job (single-core vs all-core\n"
        "    throughput, scaling, CPU steal) to screen a rented box. See `benchmark --help`.\n");
}

static int parse_config(int argc, char **argv, client_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->idle_backoff_seconds = 30;
    cfg->http_interval_ms = 50;
    cfg->workers = 1;
    cfg->http_concurrency = 16;
    snprintf(cfg->lease_class, sizeof(cfg->lease_class), "cpu");
    cfg->engine      = ENGINE_LASIEVE4;
    cfg->cuda_device = -1;
    cfg->prefetch    = 0;      /* resolved per engine once --engine is known */
    snprintf(cfg->workdir, sizeof(cfg->workdir), "%s", "/tmp/ggnfs-client");

    const char *url     = flag(argc, argv, "--server-url");
    const char *token   = flag(argc, argv, "--token");
    const char *siever  = flag(argc, argv, "--siever");
    const char *cid     = flag(argc, argv, "--client-id");
    const char *wdir    = flag(argc, argv, "--workdir");
    const char *ib      = flag(argc, argv, "--idle-backoff");
    const char *engine_s = flag(argc, argv, "--engine");
    if (engine_s && *engine_s) {
        if      (strcmp(engine_s, "lasieve4") == 0) cfg->engine = ENGINE_LASIEVE4;
        else if (strcmp(engine_s, "cuda")     == 0) cfg->engine = ENGINE_CUDA;
        else {
            fprintf(stderr, "client: --engine must be 'lasieve4' or 'cuda'\n");
            return -1;
        }
    }
    /* A cuda client asks for gpu-class bands by default — that is the whole
     * point of having them — but --lease-class still overrides, which is how
     * you point a card at a CPU band to drain a tail. */
    if (cfg->engine == ENGINE_CUDA)
        snprintf(cfg->lease_class, sizeof(cfg->lease_class), "gpu");

    const char *lease_class = flag(argc, argv, "--lease-class");
    if (lease_class && *lease_class) {
        if (strcmp(lease_class, "cpu") != 0 && strcmp(lease_class, "gpu") != 0) {
            fprintf(stderr, "client: --lease-class must be 'cpu' or 'gpu'\n");
            return -1;
        }
        snprintf(cfg->lease_class, sizeof(cfg->lease_class), "%s", lease_class);
    }

    const char *cuda_bench = flag(argc, argv, "--cuda-bench");
    if (cuda_bench && *cuda_bench)
        snprintf(cfg->cuda_bench, sizeof(cfg->cuda_bench), "%s", cuda_bench);

    const char *fb1 = flag(argc, argv, "--fb1");
    if (fb1 && *fb1)
        snprintf(cfg->fb1_path, sizeof(cfg->fb1_path), "%s", fb1);

    const char *fbgen = flag(argc, argv, "--fbgen-gpu");
    if (fbgen && *fbgen)
        snprintf(cfg->fbgen_gpu, sizeof(cfg->fbgen_gpu), "%s", fbgen);

    const char *prefetch_s = flag(argc, argv, "--prefetch");
    if (prefetch_s && *prefetch_s) {
        int64_t n;
        if (parse_int64_arg(prefetch_s, &n) != 0 || n < 1 || n > 8) {
            fprintf(stderr, "client: --prefetch must be an integer 1..8\n");
            return -1;
        }
        cfg->prefetch = (int)n;
    }
    /* A CPU worker stalling through a lease costs one core out of many, so it
     * stays serial by default. A GPU is the whole box: prefetch 2 keeps the
     * card fed across the lease round trip and the upload. */
    if (cfg->prefetch == 0)
        cfg->prefetch = (cfg->engine == ENGINE_CUDA) ? 2 : 1;

    const char *device_s = flag(argc, argv, "--device");
    if (device_s && *device_s) {
        int64_t d;
        if (parse_int64_arg(device_s, &d) != 0 || d < 0 || d > 255) {
            fprintf(stderr, "client: --device must be an integer 0..255\n");
            return -1;
        }
        cfg->cuda_device = (int)d;
    }

    const char *workers = flag(argc, argv, "--workers");
    const char *httpc   = flag(argc, argv, "--http-concurrency");
    const char *httpi   = flag(argc, argv, "--http-interval-ms");
    const char *once    = flag(argc, argv, "--once");

    if (!url || !*url || !token || !*token) {
        usage();
        return -1;
    }
    /* Each engine requires its own binary and ignores the other's. */
    if (cfg->engine == ENGINE_CUDA) {
        if (cfg->cuda_bench[0] == '\0') {
            fprintf(stderr, "client: --engine=cuda requires --cuda-bench=<path to "
                            "cuda-sieve bench>\n");
            return -1;
        }
    } else if (!siever || !*siever) {
        usage();
        return -1;
    }
    snprintf(cfg->server_url,  sizeof(cfg->server_url),  "%s", url);
    snprintf(cfg->token,       sizeof(cfg->token),       "%s", token);
    snprintf(cfg->siever_path, sizeof(cfg->siever_path), "%s", siever ? siever : "");

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

/* A structurally complete .afb cache is 8 + 8*FBsize bytes (u32 entry
 * count, FBsize primes, FBsize roots, trailing u32), optionally followed
 * by the siever's 16-byte provenance trailer (magic, bound, poly
 * fingerprint, checksum). This catches files truncated by a crashed
 * generation; content-level validation is done by the siever itself
 * when it loads the file. */
static int afb_size_ok(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint32_t fbsize = 0;
    if (fread(&fbsize, sizeof(fbsize), 1, f) != 1) { fclose(f); return 0; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    int64_t sz = ftell(f);
    fclose(f);
    int64_t base = 8 + 8 * (int64_t)fbsize;
    return sz == base || sz == base + 20;
}

/* The cache is only safe with sievers that trim and audit it on load:
 * older sievers blindly use the file as-is, which for workunits with
 * q < alim means sieving with factor-base primes >= q. Operators update
 * the client (git pull && make) and the siever binary independently, so
 * sniff the binary for the audit code's message text and treat its
 * absence as "old siever". */
static int siever_supports_afb_audit(const char *siever_path)
{
    static const char marker[] = "Trimmed cached aFB";
    size_t len = 0;
    unsigned char *buf = read_file(siever_path, &len);
    int found = buf && len >= sizeof(marker) - 1 &&
                memmem(buf, len, marker, sizeof(marker) - 1) != NULL;
    free(buf);
    return found;
}

/* Pre-generate the algebraic factor-base cache (<job>.afb.0) beside the
 * hash-named job file. The siever auto-loads the cache and skips its
 * 30-45s factor-base rebuild on every workunit; -c 0 makes it build and
 * write the full factor base without sieving anything (and without the
 * FB_bound lowering that applies when q < alim). Generation runs against
 * a temp copy of the job and the result is renamed into place, so workers
 * (serialized on g_afb_mu) never see a partial file. Because the job file
 * is named by its content hash, a new job can never pick up a stale cache.
 * Failure is non-fatal: without the cache the siever rebuilds the factor
 * base per workunit, exactly as before. */
static pthread_mutex_t g_afb_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_afb_gen_failed = 0;
static int g_afb_siever_ok = -1;   /* -1 unknown, 0 old siever, 1 supported */
static char g_afb_validated_path[300]; /* cache that passed the content probe */

static void ensure_afb_cached(const client_cfg_t *cfg,
                              const proto_lease_response_t *lease,
                              const char *job_local)
{
    char afb[300], genjob[300], genafb[300], genout[300];

    /* Staging names carry the pid: concurrent client processes sharing a
     * workdir (a supported configuration) must not truncate or rename each
     * other's in-progress generation files. Both would generate identical
     * bytes, so whichever rename lands last is still correct. */
    snprintf(afb,    sizeof(afb),    "%s.afb.0",            job_local);
    snprintf(genjob, sizeof(genjob), "%s.fbgen.%d",         job_local, (int)getpid());
    snprintf(genafb, sizeof(genafb), "%s.fbgen.%d.afb.0",   job_local, (int)getpid());
    snprintf(genout, sizeof(genout), "%s.fbgen.%d.out",     job_local, (int)getpid());

    pthread_mutex_lock(&g_afb_mu);

    if (g_afb_siever_ok == -1) {
        g_afb_siever_ok = siever_supports_afb_audit(cfg->siever_path);
        if (!g_afb_siever_ok)
            fprintf(stderr,
                    "client: %s predates factor-base cache support; "
                    "not generating %s (update the siever to enable it)\n",
                    cfg->siever_path, afb);
    }
    if (!g_afb_siever_ok) {
        /* An old siever would still auto-load a leftover cache file (e.g.
         * generated before the operator switched sievers), so make sure
         * none exists for it to find. */
        if (file_exists(afb)) {
            fprintf(stderr, "client: removing %s (current siever cannot "
                    "validate it)\n", afb);
            unlink(afb);
        }
        pthread_mutex_unlock(&g_afb_mu);
        return;
    }

    if (g_afb_gen_failed) {
        pthread_mutex_unlock(&g_afb_mu);
        return;
    }
    if (file_exists(afb)) {
        if (strcmp(g_afb_validated_path, afb) == 0) {
            pthread_mutex_unlock(&g_afb_mu);
            return;
        }
        if (afb_size_ok(afb)) {
            /* The size check only proves the file is structurally whole. A
             * cache that is complete but wrong for this job (hand-seeded
             * from another job, built with a smaller alim, bit rot) would
             * make the siever's audit reject it on every workunit with
             * nothing ever deleting the file. Probe once per process: a
             * "-c 0" run loads and audits the cache against the full bound,
             * sieves nothing, and exits nonzero if the siever refuses it. */
            int prc = sieve_run_local(cfg->siever_path, job_local, genout,
                                      1000, 0, lease->side, "",
                                      should_cancel_siever, NULL);
            unlink(genout);
            if (prc == 0) {
                snprintf(g_afb_validated_path, sizeof(g_afb_validated_path),
                         "%s", afb);
                printf("client: validated existing factor base cache %s\n", afb);
                pthread_mutex_unlock(&g_afb_mu);
                return;
            }
            if (should_cancel_siever(NULL)) {
                /* Probe was killed by shutdown, not by the audit; keep the
                 * cache for the next run. */
                pthread_mutex_unlock(&g_afb_mu);
                return;
            }
            fprintf(stderr, "client: %s failed the siever audit (rc=%d); "
                    "regenerating\n", afb, prc);
        } else {
            fprintf(stderr, "client: %s is truncated; regenerating\n", afb);
        }
        unlink(afb);
    }

    size_t job_len = 0;
    unsigned char *job_buf = read_file(job_local, &job_len);
    int staged = job_buf && write_file_atomic(genjob, job_buf, job_len) == 0;
    free(job_buf);
    if (!staged) {
        fprintf(stderr, "client: cannot stage %s; factor base cache disabled\n",
                genjob);
        g_afb_gen_failed = 1;
        pthread_mutex_unlock(&g_afb_mu);
        return;
    }

    unlink(genafb);
    printf("client: generating factor base cache %s (one-time per job)\n", afb);

    int rc = sieve_run_local(cfg->siever_path, genjob, genout,
                             1000, 0, lease->side, "-k -F",
                             should_cancel_siever, NULL);
    if (rc == 0 && afb_size_ok(genafb) && rename(genafb, afb) == 0) {
        snprintf(g_afb_validated_path, sizeof(g_afb_validated_path), "%s", afb);
        printf("client: factor base cache ready: %s\n", afb);
    } else {
        fprintf(stderr,
                "client: factor base cache generation failed (rc=%d); "
                "sievers will rebuild the factor base per workunit\n", rc);
        g_afb_gen_failed = 1;
        unlink(genafb);
    }
    unlink(genjob);
    unlink(genout);
    pthread_mutex_unlock(&g_afb_mu);
}

/* Returns:
 *    1  - got a workunit, *out filled
 *    0  - 204 No Content (job still running, just no work right now)
 *   -1  - 410 Gone (job complete, caller should exit)
 *   -2  - other error (caller should backoff)
 */
/* ---- cuda-sieve factor-base cache (--fb1) ------------------------------
 *
 * The direct analogue of ensure_afb_cached above. Without a --fb1 file, bench
 * regenerates the complete algebraic factor base on the GPU at every single
 * workunit; with one it loads a prepared file instead. On a job like the C208
 * that file is ~230 MB, so this is the difference between paying a large
 * fixed cost per band and paying it once per client.
 *
 * The cache is keyed on (job sha, logI) in its filename, which is what makes
 * a content probe unnecessary: a file built for a different polynomial or a
 * different sieve width simply cannot be found under this name. Generation
 * stages through a pid-unique path and renames, so concurrent client
 * processes sharing a workdir can never observe a partial file — and neither
 * can a client that was killed mid-generation.
 */
#define GPU_FB_LOGI_DEFAULT 15   /* cuda-sieve bench_main.cu: cfg.logI = 15 */

static pthread_mutex_t g_fb1_mu = PTHREAD_MUTEX_INITIALIZER;
static int  g_fb1_failed = 0;
static char g_fb1_ready[320];    /* "" until this process has a usable cache */

/* Pull "--logI N" out of the server-supplied cuda-sieve arg string. --maxbits
 * must match the width actually sieved at, so guessing wrong here would build
 * a cache bench cannot use. */
static int gpu_args_logI(const char *args)
{
    if (!args) return GPU_FB_LOGI_DEFAULT;
    const char *p = strstr(args, "--logI");
    if (!p) return GPU_FB_LOGI_DEFAULT;
    p += strlen("--logI");
    while (*p == ' ' || *p == '\t' || *p == '=') p++;
    int v = atoi(p);
    return (v >= 8 && v <= 24) ? v : GPU_FB_LOGI_DEFAULT;
}

/* Read "alim: N" from a ggnfs .job. 0 if absent or unparsable. The algebraic
 * bound is the right one even for a rational-side special-q: cuda-sieve's
 * factor base is always the algebraic one. */
static int64_t job_alim(const char *job_path)
{
    FILE *f = fopen(job_path, "r");
    if (!f) return 0;
    char line[512];
    int64_t alim = 0;
    while (fgets(line, sizeof(line), f)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "alim:", 5) != 0) continue;
        p += 5;
        while (*p == ' ' || *p == '\t') p++;
        alim = strtoll(p, NULL, 10);
        break;
    }
    fclose(f);
    return alim > 0 ? alim : 0;
}

/* Returns the path to pass as --fb1, or NULL to let bench build the base
 * in-process. Never fails the workunit: a cache we cannot build just means
 * slower sieving, not wrong sieving. */
static const char *ensure_gpu_fb_cached(const client_cfg_t *cfg,
                                        const proto_lease_response_t *lease,
                                        const char *job_local,
                                        struct mg_mgr *mgr)
{
    /* An explicit --fb1 is the operator's call and is used as given. */
    if (cfg->fb1_path[0]) return cfg->fb1_path;
    if (cfg->fbgen_gpu[0] == '\0') return NULL;

    /* Build the key BEFORE any early return. The whole point of keying on
     * (job sha, logI) is that a cache built for a different width is not
     * reusable — and gpu_args can change under a running client, since the
     * documented way to change it is `extend --gpu-args=...` plus a `serve`
     * restart, which does not restart the clients. Short-circuiting on
     * g_fb1_ready before computing the key would keep feeding bench an
     * --fb1 built at the old maxbits alongside the new --logI. */
    int logI = gpu_args_logI(lease->gpu_args);

    char cache[320];
    snprintf(cache, sizeof(cache), "%s/%.16s.roots1.m%d",
             cfg->file_cache_dir, lease->file_sha256_hex, logI);

    pthread_mutex_lock(&g_fb1_mu);
    if (g_fb1_failed) { pthread_mutex_unlock(&g_fb1_mu); return NULL; }
    if (strcmp(g_fb1_ready, cache) == 0) {
        pthread_mutex_unlock(&g_fb1_mu);
        return g_fb1_ready;
    }

    if (file_exists(cache)) {
        snprintf(g_fb1_ready, sizeof(g_fb1_ready), "%s", cache);
        printf("client: using factor base cache %s\n", cache);
        pthread_mutex_unlock(&g_fb1_mu);
        return g_fb1_ready;
    }

    int64_t alim = job_alim(job_local);
    if (alim == 0) {
        fprintf(stderr, "client: no alim: in the .job; cannot pre-build the "
                        "factor base (bench will build it per workunit)\n");
        g_fb1_failed = 1;
        pthread_mutex_unlock(&g_fb1_mu);
        return NULL;
    }

    char staged[352];
    snprintf(staged, sizeof(staged), "%s.%d.part", cache, (int)getpid());
    unlink(staged);

    printf("client: building factor base cache %s "
           "(one-time per job; lim=%lld maxbits=%d)\n",
           cache, (long long)alim, logI);

    char syscmd[1280];
    snprintf(syscmd, sizeof(syscmd),
             "%s --poly %s --lim %lld --maxbits %d --out %s",
             cfg->fbgen_gpu, job_local, (long long)alim, logI, staged);

    /* Generation can run for minutes, and the workunit is already leased by
     * the time we get here — so heartbeat through it exactly as we do through
     * a sieve, or the lease lapses before the first band is even started. */
    sieve_ctx_t fbsc = {
        .mgr           = mgr,
        .cfg           = cfg,
        .workunit_id   = lease->workunit_id,
        .next_renew_ms = 0,
        .interval_ms   = 0,
        .lease_lost    = 0,
    };
    if (mgr && lease->lease_seconds > 0) {
        fbsc.interval_ms = (lease->lease_seconds * 1000) / 3;
        if (fbsc.interval_ms < 5000) fbsc.interval_ms = 5000;
        fbsc.next_renew_ms = monotonic_ms() + fbsc.interval_ms;
    }

    int rc = sieve_run_command(syscmd, should_cancel_siever, &fbsc);

    off_t sz = 0;
    if (rc == 0 && regular_file_size(staged, &sz) == 0 && sz > 0 &&
        rename(staged, cache) == 0) {
        snprintf(g_fb1_ready, sizeof(g_fb1_ready), "%s", cache);
        printf("client: factor base cache ready: %s (%lld bytes)\n",
               cache, (long long)sz);
        pthread_mutex_unlock(&g_fb1_mu);
        return g_fb1_ready;
    }

    unlink(staged);
    if (shutdown_phase() >= SHUTDOWN_CANCELLING) {
        /* Killed by shutdown, not by a real failure — don't poison the next
         * run's attempt. */
        pthread_mutex_unlock(&g_fb1_mu);
        return NULL;
    }
    fprintf(stderr, "client: factor base cache generation failed (rc=%d); "
                    "bench will rebuild it per workunit\n", rc);
    g_fb1_failed = 1;
    pthread_mutex_unlock(&g_fb1_mu);
    return NULL;
}

static int do_lease(struct mg_mgr *mgr, const client_cfg_t *cfg,
                    proto_lease_response_t *out)
{
    char url[512];
    if (join_url(url, sizeof(url), cfg->server_url, "/lease") != 0) return -2;

    char *body = proto_encode_lease_request(cfg->client_id, CLIENT_VERSION,
                                            cfg->lease_class);
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
/* Push our lease out. Returns:
 *    0  renewed
 *    1  the server says we no longer hold this workunit (409) — it was
 *       reclaimed and reissued, so anything we produce for it is waste
 *    2  the server has no /renew (404) — an older build; the lease is fine,
 *       we simply cannot heartbeat it
 *   -1  transient failure (connection, 5xx) — worth retrying
 */
static int do_renew(struct mg_mgr *mgr, const client_cfg_t *cfg,
                    const char *workunit_id)
{
    char url[512];
    if (join_url(url, sizeof(url), cfg->server_url, "/renew") != 0) return -1;

    /* Same {workunit_id, client_id} body as /release. */
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
    /* Short budget and abort_on_cancel=1: this runs inside the siever's
     * ~100 ms cancellation poll, so a slow or wedged server must not keep the
     * loop out of waitpid() and shutdown_phase() for long. */
    int rc = http_request(mgr, &io, 5000, 1);
    free(body);
    if (rc != 0) {
        http_io_free(&io);
        return -1;
    }
    int status = io.status;
    http_io_free(&io);
    if (status == 200) return 0;
    if (status == 409) return 1;
    if (status == 404) return 2;
    return -1;
}

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
    /* g_active_count is workers*prefetch, not workers: a pipelined worker holds
     * several leases at once and every one of them has to be released here. */
    n = g_active_count;
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
/* ---- pipeline stages ---------------------------------------------------
 *
 * The lease -> sieve -> submit cycle is split into three stages so the serial
 * worker and the pipelined GPU worker share one implementation of each. The
 * serial worker just calls them back to back; the pipelined one runs them on
 * separate threads against a ring of lease slots, so the card keeps sieving
 * while one slot uploads and another is being leased.
 *
 * `active_idx` is the slot's row in the global active-lease table, which the
 * cancellation path walks to release leases. Every slot needs its own row,
 * and its own client_id — the server's "one live lease per client_id" guard
 * would otherwise hand a prefetching client the workunit it already holds
 * instead of a new one.
 */
typedef struct {
    client_cfg_t            cfg;         /* copy, with this slot's client_id */
    proto_lease_response_t  lease;
    char                    job_local[256];
    const char             *fb1;         /* cuda only; owned by the FB cache */
    char                   *outfile;     /* malloc'd */
    double                  sieve_seconds;
    int                     active_idx;
    int                     lease_lost;  /* set by the heartbeat thread on 409 */
} pipe_slot_t;

static void slot_reset(pipe_slot_t *slot)
{
    free(slot->outfile);
    slot->outfile = NULL;
    slot->fb1 = NULL;
    slot->sieve_seconds = 0.0;
    slot->lease_lost = 0;
    memset(&slot->lease, 0, sizeof(slot->lease));
}

/* Stage 1: lease a workunit and fetch everything needed to sieve it.
 *   1  acquired    0  no work right now    -1  job complete / draining
 *  -2  transient failure */
static int stage_acquire(struct mg_mgr *mgr, pipe_slot_t *slot)
{
    const client_cfg_t *cfg = &slot->cfg;

    if (shutdown_phase() >= SHUTDOWN_DRAINING) return -1;

    int lr = do_lease(mgr, cfg, &slot->lease);
    if (lr == 0)  return 0;
    if (lr == -1) return -1;
    if (lr <  0)  return -2;
    if (shutdown_phase() >= SHUTDOWN_DRAINING) {
        do_release(mgr, cfg, slot->lease.workunit_id);
        return -1;
    }

    printf("client: leased %s  q=[%lld,%lld)  width=%lld  side=%c  engine=%s  args=%s\n",
           slot->lease.workunit_id,
           (long long)slot->lease.q_start,
           (long long)(slot->lease.q_start + slot->lease.q_range),
           (long long)slot->lease.q_range,
           slot->lease.side, engine_name(cfg->engine),
           (cfg->engine == ENGINE_CUDA)
               ? (slot->lease.gpu_args[0]    ? slot->lease.gpu_args    : "(none)")
               : (slot->lease.siever_args[0] ? slot->lease.siever_args : "(none)"));
    active_lease_set(slot->active_idx, slot->lease.workunit_id, cfg->client_id);

    /* Fetch input file (the .job) into the local cache. */
    if (ensure_file_cached(mgr, cfg, &slot->lease,
                           slot->job_local, sizeof(slot->job_local)) != 0) {
        if (shutdown_phase() >= SHUTDOWN_DRAINING)
            release_active_lease(mgr, cfg, slot->active_idx);
        return -2;
    }

    /* One-time per job: pre-generate the factor base so the siever does not
     * rebuild it on every workunit. lasieve4 auto-loads its .afb.0; cuda-sieve
     * takes the equivalent as --fb1. */
    if (cfg->engine == ENGINE_CUDA)
        slot->fb1 = ensure_gpu_fb_cached(cfg, &slot->lease, slot->job_local, mgr);
    else
        ensure_afb_cached(cfg, &slot->lease, slot->job_local);

    /* Local outfile path for the siever to write into. Use the workunit id
     * rather than the server's generic output name so concurrent client
     * processes sharing a workdir cannot unlink/replace each other's output. */
    char output_name[sizeof(slot->lease.workunit_id) + 4];
    snprintf(output_name, sizeof(output_name), "%s.dat", slot->lease.workunit_id);
    slot->outfile = path_join(cfg->workdir, output_name);
    if (!slot->outfile) {
        if (shutdown_phase() >= SHUTDOWN_DRAINING)
            release_active_lease(mgr, cfg, slot->active_idx);
        return -2;
    }

    /* The server tells us which siever name to use; we trust the operator
     * to have given --siever pointing at the right binary on disk. We can
     * surface the mismatch as a warning. Meaningless under --engine=cuda:
     * the server names a gnfs-lasieve4 binary because that is what the CPU
     * fleet runs, and the geometry the card should use comes from gpu_args
     * instead. */
    if (cfg->engine != ENGINE_CUDA) {
        const char *siever_basename = strrchr(cfg->siever_path, '/');
        siever_basename = siever_basename ? siever_basename + 1 : cfg->siever_path;
        if (strcmp(siever_basename, slot->lease.siever) != 0) {
            fprintf(stderr, "client: WARNING — server requested '%s' but --siever is '%s'\n",
                    slot->lease.siever, siever_basename);
        }
    }
    return 1;
}

/* Stage 2: run the siever. `mgr` is used only for lease heartbeats.
 *   1  relations ready to submit    -2  nothing to submit */
static int stage_sieve(struct mg_mgr *mgr, pipe_slot_t *slot)
{
    const client_cfg_t *cfg = &slot->cfg;

    /* Heartbeat at a third of the lease window: two consecutive failures
     * still leave a full interval of slack before the server reclaims the
     * workunit. A server that reports no lease_seconds gets no heartbeat. */
    sieve_ctx_t sc = {
        .mgr           = mgr,
        .cfg           = cfg,
        .workunit_id   = slot->lease.workunit_id,
        .next_renew_ms = 0,
        .interval_ms   = 0,
        .lease_lost    = 0,
    };
    if (slot->lease.lease_seconds > 0) {
        sc.interval_ms = (slot->lease.lease_seconds * 1000) / 3;
        if (sc.interval_ms < 5000) sc.interval_ms = 5000;
        sc.next_renew_ms = monotonic_ms() + sc.interval_ms;
    }

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    /* Each engine gets its own vocabulary from the lease: siever_args is
     * lasieve4's, gpu_args is cuda-sieve's. They describe different sieve
     * areas and must never be crossed over. */
    int sieve_rc;
    if (cfg->engine == ENGINE_CUDA) {
        sieve_rc = sieve_run_cuda(cfg->cuda_bench, slot->job_local, slot->outfile,
                                  (uint32_t)slot->lease.q_start,
                                  (uint32_t)slot->lease.q_range,
                                  slot->lease.side,
                                  slot->lease.gpu_args,
                                  slot->fb1,
                                  cfg->cuda_device,
                                  should_cancel_siever,
                                  &sc);
    } else {
        sieve_rc = sieve_run_local(cfg->siever_path, slot->job_local, slot->outfile,
                                   (uint32_t)slot->lease.q_start,
                                   (uint32_t)slot->lease.q_range,
                                   slot->lease.side,
                                   slot->lease.siever_args,
                                   should_cancel_siever,
                                   &sc);
    }
    gettimeofday(&t1, NULL);
    slot->sieve_seconds = elapsed_seconds(t0, t1);

    if (sc.lease_lost) {
        /* The server took this workunit back mid-sieve and has reissued it, so
         * uploading would be a certain 409 and there is no lease to release.
         * The partial output is kept rather than unlinked: the file is left
         * exactly like every other failure path here, so an operator can still
         * salvage it by hand if the band turns out to be expensive to redo. */
        fprintf(stderr, "client: %s lease was reclaimed; not submitting, "
                        "leaving %s for inspection\n",
                slot->lease.workunit_id, slot->outfile);
        active_lease_clear(slot->active_idx);
        return -2;
    }
    if (sieve_rc != 0) {
        fprintf(stderr, "client: siever returned %d (skipping submit)\n", sieve_rc);
        if (shutdown_phase() >= SHUTDOWN_DRAINING)
            release_active_lease(mgr, cfg, slot->active_idx);
        return -2;
    }
    off_t outfile_size = 0;
    if (regular_file_size(slot->outfile, &outfile_size) != 0) {
        fprintf(stderr, "client: siever did not produce %s (skipping submit)\n",
                slot->outfile);
        if (shutdown_phase() >= SHUTDOWN_DRAINING)
            release_active_lease(mgr, cfg, slot->active_idx);
        return -2;
    }
    if (outfile_size == 0) {
        fprintf(stderr, "client: siever produced empty %s (skipping submit)\n",
                slot->outfile);
        if (shutdown_phase() >= SHUTDOWN_DRAINING)
            release_active_lease(mgr, cfg, slot->active_idx);
        unlink(slot->outfile);
        return -2;
    }
    return 1;
}

/* Stage 3: upload and tidy up.  1 accepted, -2 otherwise. */
static int stage_submit(struct mg_mgr *mgr, pipe_slot_t *slot)
{
    const client_cfg_t *cfg = &slot->cfg;

    /* Reading, zstd-compressing and uploading a gpu-class band's relations is
     * minutes of work over a throttled link, and it happens after the last
     * heartbeat. Renew once here so the upload starts with a full lease window
     * ahead of it, instead of racing whatever was left over from the sieve. */
    if (slot->lease.lease_seconds > 0) {
        if (do_renew(mgr, cfg, slot->lease.workunit_id) == 1) {
            /* A completed band. The server reissued the workunit while we were
             * sieving, so /submit would 409 — but these relations are valid and
             * are precisely what the reissued workunit will re-derive, so they
             * are kept on disk rather than deleted. */
            fprintf(stderr, "client: %s lease was reclaimed before submit; "
                            "leaving %s for inspection\n",
                    slot->lease.workunit_id, slot->outfile);
            active_lease_clear(slot->active_idx);
            return -2;
        }
    }

    int sr = submit_with_retries(mgr, cfg, &slot->lease, slot->outfile,
                                 slot->sieve_seconds);
    if (sr == -1) {
        fprintf(stderr, "client: submit cancelled; leaving %s for inspection\n",
                slot->outfile);
        return -2;
    }
    if (sr == -2) {
        fprintf(stderr, "client: submit failed permanently; leaving %s for inspection\n",
                slot->outfile);
        active_lease_clear(slot->active_idx);
        return -2;
    }

    active_lease_clear(slot->active_idx);
    /* Tidy: remove the local rels file once the server accepted it, or once
     * the server says the workunit has already moved on. */
    unlink(slot->outfile);
    return 1;
}

/* The full lease -> sieve -> submit cycle, run serially. Returns:
 *   1  - completed a workunit (caller may continue immediately)
 *   0  - no work right now (caller should backoff)
 *  -1  - job is done (caller should exit cleanly)
 *  -2  - transient failure (caller should backoff)
 */
static int run_one_iteration(struct mg_mgr *mgr, const client_cfg_t *cfg, int worker_idx)
{
    pipe_slot_t slot;
    memset(&slot, 0, sizeof(slot));
    slot.cfg        = *cfg;
    slot.active_idx = worker_idx;

    int r = stage_acquire(mgr, &slot);
    if (r != 1) { slot_reset(&slot); return r; }

    r = stage_sieve(mgr, &slot);
    if (r != 1) { slot_reset(&slot); return -2; }

    r = stage_submit(mgr, &slot);
    slot_reset(&slot);
    return r == 1 ? 1 : -2;
}

/* ---- pipelined worker (GPU) --------------------------------------------
 *
 * A CPU worker idling through a lease round trip costs nothing much: the other
 * 7 or 15 cores keep sieving. A GPU is the whole machine, so every second
 * spent uploading or waiting for a lease is a second the card does nothing.
 *
 * So the GPU worker runs the three stages concurrently over a ring of slots:
 *
 *     lease thread   ->  [ slot ]  ->  sieve loop  ->  [ slot ]  ->  submit thread
 *
 * One slot is being sieved, one is already leased and waiting, and a third may
 * be uploading. The card moves straight from one band to the next.
 *
 * Each slot carries its own client_id (`<base>-sK`) because the server keeps
 * at most one live lease per client_id — a prefetching client that reused one
 * id would get handed the workunit it already holds instead of a new one.
 */
typedef enum {
    SLOT_EMPTY = 0,   /* free; the lease thread may claim it        */
    SLOT_READY,       /* leased and fetched; waiting for the card   */
    SLOT_SIEVING,     /* the card is on it                          */
    SLOT_DONE         /* relations written; waiting for the upload  */
} slot_state_t;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    pipe_slot_t    *slots;
    slot_state_t   *state;
    int             n;
    int             no_more_work;   /* server says the job is complete */
    int             finished;       /* sieve loop is done; drain and exit */
    int             once;           /* --once: stop after one workunit */
    int             completed;      /* workunits fully submitted */
} pipeline_t;

/* Find one slot in `want`; -1 if none. Caller holds the lock. */
static int pipe_find(pipeline_t *p, slot_state_t want)
{
    for (int i = 0; i < p->n; i++)
        if (p->state[i] == want) return i;
    return -1;
}

static void *pipe_lease_thread(void *arg)
{
    pipeline_t *p = (pipeline_t *)arg;
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    for (;;) {
        pthread_mutex_lock(&p->mu);
        int idx;
        while ((idx = pipe_find(p, SLOT_EMPTY)) < 0 &&
               !p->finished && shutdown_phase() < SHUTDOWN_DRAINING) {
            pthread_cond_wait(&p->cv, &p->mu);
        }
        int stop = p->finished || p->no_more_work ||
                   shutdown_phase() >= SHUTDOWN_DRAINING || idx < 0;
        if (!stop) p->state[idx] = SLOT_SIEVING;   /* reserve while we work */
        pthread_mutex_unlock(&p->mu);
        if (stop) break;

        int r = stage_acquire(&mgr, &p->slots[idx]);

        pthread_mutex_lock(&p->mu);
        if (r == 1) {
            p->state[idx] = SLOT_READY;
        } else {
            slot_reset(&p->slots[idx]);
            p->state[idx] = SLOT_EMPTY;
            /* -1 means the job is complete or we are draining: stop asking.
             * 0 (no work now) and -2 (transient) both just back off. */
            if (r == -1) p->no_more_work = 1;
        }
        pthread_cond_broadcast(&p->cv);
        int done = p->no_more_work;
        pthread_mutex_unlock(&p->mu);
        if (done) break;

        if (r == 0 || r == -2) {
            /* Back off, but stay responsive: the sieve loop may finish (or
             * --once may fire) while we are waiting for work that no longer
             * matters, and shutdown should not have to sit out a full
             * idle-backoff before this thread notices. */
            for (int64_t i = 0;
                 i < p->slots[idx].cfg.idle_backoff_seconds &&
                 shutdown_phase() == SHUTDOWN_RUNNING;
                 i++) {
                pthread_mutex_lock(&p->mu);
                int quit = p->finished || p->no_more_work;
                pthread_mutex_unlock(&p->mu);
                if (quit) break;
                sleep(1);
            }
        }
    }

    /* Wake the sieve loop so it does not wait for work that is not coming. */
    pthread_mutex_lock(&p->mu);
    p->no_more_work = 1;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);

    mg_mgr_free(&mgr);
    return NULL;
}

/* A prefetched slot holds a real server lease while it waits its turn on the
 * card, and stage_sieve's heartbeat only covers the slot actually sieving. So
 * an idle slot's lease would quietly lapse, the sweep would requeue it with
 * attempt_count++ and reissue it to somebody else, and we would sieve a band
 * we no longer own — repeat enough times and the workunit is poisoned. This
 * thread renews every held lease that the sieve callback is not already
 * covering. */
static void *pipe_heartbeat_thread(void *arg)
{
    pipeline_t *p = (pipeline_t *)arg;
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    int64_t next_ms = 0;
    for (;;) {
        pthread_mutex_lock(&p->mu);
        int done = p->finished;
        pthread_mutex_unlock(&p->mu);
        if (done || shutdown_phase() >= SHUTDOWN_CANCELLING) break;

        int64_t now = monotonic_ms();
        if (next_ms == 0 || now < next_ms) {
            if (next_ms == 0) next_ms = now;   /* first pass: renew immediately */
            else { sleep(1); continue; }
        }

        /* Snapshot under the lock; do the HTTP outside it. */
        struct { int idx; char wu[64]; } todo[8];
        int n_todo = 0;
        int64_t interval = 0;
        pthread_mutex_lock(&p->mu);
        for (int i = 0; i < p->n && n_todo < (int)(sizeof(todo)/sizeof(todo[0])); i++) {
            /* SIEVING is the sieve callback's job; EMPTY holds no lease. */
            if (p->state[i] != SLOT_READY && p->state[i] != SLOT_DONE) continue;
            if (p->slots[i].lease_lost) continue;
            if (p->slots[i].lease.workunit_id[0] == '\0') continue;
            if (p->slots[i].lease.lease_seconds <= 0) continue;
            interval = (p->slots[i].lease.lease_seconds * 1000) / 3;
            todo[n_todo].idx = i;
            snprintf(todo[n_todo].wu, sizeof(todo[n_todo].wu), "%s",
                     p->slots[i].lease.workunit_id);
            n_todo++;
        }
        pthread_mutex_unlock(&p->mu);

        for (int k = 0; k < n_todo; k++) {
            /* cfg is written once at setup and never mutated, so reading it
             * outside the lock is safe. */
            int r = do_renew(&mgr, &p->slots[todo[k].idx].cfg, todo[k].wu);
            if (r != 1) continue;
            pthread_mutex_lock(&p->mu);
            /* Only mark it if the slot still holds the same workunit. */
            if (strcmp(p->slots[todo[k].idx].lease.workunit_id, todo[k].wu) == 0) {
                fprintf(stderr, "client: queued workunit %s was reclaimed by "
                                "the server; dropping it\n", todo[k].wu);
                p->slots[todo[k].idx].lease_lost = 1;
                pthread_cond_broadcast(&p->cv);
            }
            pthread_mutex_unlock(&p->mu);
        }

        if (interval < 5000) interval = 5000;
        next_ms = monotonic_ms() + interval;
    }

    mg_mgr_free(&mgr);
    return NULL;
}

static void *pipe_submit_thread(void *arg)
{
    pipeline_t *p = (pipeline_t *)arg;
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    for (;;) {
        pthread_mutex_lock(&p->mu);
        int idx;
        while ((idx = pipe_find(p, SLOT_DONE)) < 0 && !p->finished) {
            pthread_cond_wait(&p->cv, &p->mu);
        }
        if (idx < 0) { pthread_mutex_unlock(&p->mu); break; }  /* finished */
        int lost = p->slots[idx].lease_lost;
        pthread_mutex_unlock(&p->mu);

        int r;
        if (lost) {
            /* Reclaimed while queued: /submit would 409. Keep the relations on
             * disk, same as every other give-up path. */
            fprintf(stderr, "client: not submitting %s (lease reclaimed); "
                            "leaving %s for inspection\n",
                    p->slots[idx].lease.workunit_id, p->slots[idx].outfile);
            active_lease_clear(p->slots[idx].active_idx);
            r = -2;
        } else {
            r = stage_submit(&mgr, &p->slots[idx]);
        }

        pthread_mutex_lock(&p->mu);
        if (r == 1) p->completed++;
        slot_reset(&p->slots[idx]);
        p->state[idx] = SLOT_EMPTY;
        pthread_cond_broadcast(&p->cv);
        pthread_mutex_unlock(&p->mu);
    }

    mg_mgr_free(&mgr);
    return NULL;
}

/* One GPU, `prefetch` lease slots. Returns when there is no more work or a
 * shutdown was requested. */
static void pipeline_run(const client_cfg_t *base_cfg, int worker_idx)
{
    int n = base_cfg->prefetch;
    if (n < 1) n = 1;

    pipeline_t p;
    memset(&p, 0, sizeof(p));
    pthread_mutex_init(&p.mu, NULL);
    pthread_cond_init(&p.cv, NULL);
    p.n     = n;
    p.once  = base_cfg->once;
    p.slots = calloc((size_t)n, sizeof(*p.slots));
    p.state = calloc((size_t)n, sizeof(*p.state));
    if (!p.slots || !p.state) {
        fprintf(stderr, "client: pipeline alloc failed\n");
        free(p.slots); free(p.state);
        return;
    }

    for (int i = 0; i < n; i++) {
        p.slots[i].cfg = *base_cfg;
        /* Slot-private client_id and active-lease row. */
        /* --prefetch is capped at 8, so the slot suffix is always one digit;
         * %c keeps that obvious to the compiler as well as the reader. */
        snprintf(p.slots[i].cfg.client_id, sizeof(p.slots[i].cfg.client_id),
                 "%.*s-s%c", (int)(sizeof(p.slots[i].cfg.client_id) - 4),
                 base_cfg->client_id, (char)('0' + i));
        p.slots[i].active_idx = worker_idx * n + i;
        p.state[i] = SLOT_EMPTY;
    }

    fprintf(stderr, "  [w%d] pipelined: %d lease slots (%s .. %s)\n",
            worker_idx, n, p.slots[0].cfg.client_id,
            p.slots[n - 1].cfg.client_id);

    pthread_t lease_tid, submit_tid, hb_tid;
    int have_lease  = pthread_create(&lease_tid,  NULL, pipe_lease_thread,  &p) == 0;
    int have_submit = pthread_create(&submit_tid, NULL, pipe_submit_thread, &p) == 0;
    int have_hb     = pthread_create(&hb_tid,     NULL, pipe_heartbeat_thread, &p) == 0;
    if (!have_lease || !have_submit || !have_hb) {
        /* Joining an uninitialized pthread_t is undefined; bail out cleanly
         * instead, the way main's own worker spawn loop does. */
        fprintf(stderr, "client: [w%d] pipeline thread create failed: %s\n",
                worker_idx, strerror(errno));
        pthread_mutex_lock(&p.mu);
        p.no_more_work = 1;
        p.finished = 1;
        pthread_cond_broadcast(&p.cv);
        pthread_mutex_unlock(&p.mu);
        if (have_lease)  pthread_join(lease_tid, NULL);
        if (have_submit) pthread_join(submit_tid, NULL);
        if (have_hb)     pthread_join(hb_tid, NULL);
        free(p.slots); free(p.state);
        pthread_mutex_destroy(&p.mu);
        pthread_cond_destroy(&p.cv);
        return;
    }

    /* Sieve loop: the card, one band at a time. */
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    int sieved = 0;
    for (;;) {
        pthread_mutex_lock(&p.mu);
        int idx;
        while ((idx = pipe_find(&p, SLOT_READY)) < 0 && !p.no_more_work &&
               shutdown_phase() < SHUTDOWN_CANCELLING) {
            pthread_cond_wait(&p.cv, &p.mu);
        }
        if (idx < 0) { pthread_mutex_unlock(&p.mu); break; }
        int lost = p.slots[idx].lease_lost;
        if (lost) {
            /* Reclaimed while it sat in the queue — never start the card on it. */
            slot_reset(&p.slots[idx]);
            p.state[idx] = SLOT_EMPTY;
            pthread_cond_broadcast(&p.cv);
            pthread_mutex_unlock(&p.mu);
            continue;
        }
        p.state[idx] = SLOT_SIEVING;
        pthread_mutex_unlock(&p.mu);

        int r = stage_sieve(&mgr, &p.slots[idx]);

        pthread_mutex_lock(&p.mu);
        if (r == 1) {
            p.state[idx] = SLOT_DONE;      /* hand off to the submit thread */
        } else {
            slot_reset(&p.slots[idx]);
            p.state[idx] = SLOT_EMPTY;
        }
        pthread_cond_broadcast(&p.cv);
        pthread_mutex_unlock(&p.mu);

        if (r == 1) sieved++;
        if (p.once && sieved >= 1) break;
        if (shutdown_phase() >= SHUTDOWN_CANCELLING) break;

        if (r != 1) {
            /* Back off exactly like the serial worker does. Without this a
             * siever that fails instantly — a wrong --cuda-bench path exits 127
             * in about 150 ms — becomes a lease-and-abandon storm that drains
             * the available pool into 'leased' rows nobody is working on. */
            for (int64_t i = 0;
                 i < base_cfg->idle_backoff_seconds &&
                 shutdown_phase() == SHUTDOWN_RUNNING;
                 i++) {
                pthread_mutex_lock(&p.mu);
                int quit = p.no_more_work;
                pthread_mutex_unlock(&p.mu);
                if (quit) break;
                sleep(1);
            }
        }
    }

    /* Stop leasing, then let the submit thread drain what is already sieved:
     * that work is finished and paid for, and the lease is still ours. */
    pthread_mutex_lock(&p.mu);
    p.no_more_work = 1;
    pthread_cond_broadcast(&p.cv);
    pthread_mutex_unlock(&p.mu);
    pthread_join(lease_tid, NULL);

    pthread_mutex_lock(&p.mu);
    while (pipe_find(&p, SLOT_DONE) >= 0 || pipe_find(&p, SLOT_SIEVING) >= 0) {
        pthread_cond_wait(&p.cv, &p.mu);
    }
    p.finished = 1;
    pthread_cond_broadcast(&p.cv);
    pthread_mutex_unlock(&p.mu);
    pthread_join(submit_tid, NULL);
    pthread_join(hb_tid, NULL);

    /* Any slot still holding a lease we never sieved goes back to the pool
     * rather than waiting out its expiry. */
    for (int i = 0; i < n; i++) {
        if (p.state[i] == SLOT_READY) {
            release_active_lease(&mgr, &p.slots[i].cfg, p.slots[i].active_idx);
            slot_reset(&p.slots[i]);
        }
    }

    printf("[w%d] pipeline finished: %d workunits submitted\n",
           worker_idx, p.completed);

    mg_mgr_free(&mgr);
    free(p.slots);
    free(p.state);
    pthread_mutex_destroy(&p.mu);
    pthread_cond_destroy(&p.cv);
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

    if (cfg.prefetch > 1) {
        /* Pipelined: leasing and uploading overlap with sieving. */
        mg_mgr_free(&mgr);
        pipeline_run(&cfg, idx);
        return NULL;
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

/* ===================== benchmark ======================================== */
/*
 * `ggnfs-sieve-client benchmark ...` runs a fixed-work speed test against the
 * server's *real* job so you can tell a good rented box from a contended one
 * in a couple of minutes instead of hours of sieving.
 *
 * It takes NO lease. It reads the job sha, sieved side, siever args, and the
 * campaign q_min/q_max from /stats (read-only), fetches the .job via
 * /file/<sha>, and sieves a FIXED q-interval WIDTH (--qrange, the siever's -c,
 * exactly like a real workunit's q_range) from a FIXED anchor q (q_min, or --q)
 * so the work is identical run-to-run and box-to-box and never disturbs real
 * work. The window holds ~width/ln(q) prime special-q. After building the
 * factor-base cache (phase 0):
 *   phase 1 — one siever on a single core: time to sieve the q-range
 *   phase 2 — every worker (default: all online cores) sieves the SAME q-range
 *             at once; time each and sum their rel/s
 * We measure fixed WORK (not fixed time), so every special-q runs to completion
 * — no partial-q quantization, and the relation count is ~identical across
 * identical boxes. Reported metrics: single-core rel/s, aggregate rel/s,
 * multicore scaling %, slowest-worker time, CPU steal %, load average. Steal and
 * a scaling collapse are the fingerprints of an oversubscribed / memory-starved
 * VM; compare rel/s against a known-good box.
 */

static long online_cpus(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? n : 1;
}

static long configured_cpus(void)
{
    long n = sysconf(_SC_NPROCESSORS_CONF);
    return n > 0 ? n : 1;
}

/* First "model name" line from /proc/cpuinfo; "unknown" if unavailable. */
static void cpu_model(char *out, size_t n)
{
    if (n == 0) return;
    out[0] = '\0';
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "model name", 10) == 0) {
                char *c = strchr(line, ':');
                if (c) {
                    c++;
                    while (*c == ' ' || *c == '\t') c++;
                    size_t l = strlen(c);
                    while (l > 0 && (c[l - 1] == '\n' || c[l - 1] == '\r')) c[--l] = '\0';
                    snprintf(out, n, "%s", c);
                }
                break;
            }
        }
        fclose(f);
    }
    if (!out[0]) snprintf(out, n, "unknown");
}

/* Count '\n' bytes — the relation count, same convention the server uses. */
static long long count_newlines(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char buf[64 * 1024];
    size_t n;
    long long lines = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < n; i++) if (buf[i] == '\n') lines++;
    }
    fclose(f);
    return lines;
}

/* Cumulative CPU jiffies + steal from /proc/stat's aggregate "cpu" line.
 * Returns 0 on success. steal/total let us compute the steal fraction over
 * a window — the clean signature of a hypervisor handing your vCPU away. */
static int read_cpu_steal(unsigned long long *steal, unsigned long long *total)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    char lbl[16];
    unsigned long long v[8] = {0};
    int got = fscanf(f, "%15s %llu %llu %llu %llu %llu %llu %llu %llu",
                     lbl, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    fclose(f);
    if (got < 9 || strcmp(lbl, "cpu") != 0) return -1;  /* user nice sys idle iowait irq softirq steal */
    unsigned long long t = 0;
    for (int i = 0; i < 8; i++) t += v[i];
    *steal = v[7];
    *total = t;
    return 0;
}

#ifdef __linux__
static void pin_current_thread(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
        fprintf(stderr, "benchmark: pin to cpu %d failed: %s\n", cpu, strerror(errno));
    }
}
#endif

/* Cancel the siever once the wall-clock deadline passes (or shutdown starts). */
typedef struct {
    int64_t deadline_ms;
} bench_cancel_t;

static int bench_should_cancel(void *ctx)
{
    bench_cancel_t *b = (bench_cancel_t *)ctx;
    if (shutdown_phase() >= SHUTDOWN_DRAINING) return 1;
    return monotonic_ms() >= b->deadline_ms;
}

typedef struct {
    const client_cfg_t *cfg;
    const char *job_local;
    const char *siever_args;
    char        side;
    uint32_t    q_start;
    uint32_t    q_count;     /* siever -c: the fixed q-interval width to sieve */
    int64_t     deadline_ms;
    int         cpu;         /* -1 = no pin */
    char        outfile[300];
    /* outputs */
    long long   relations;
    int         rc;
    double      seconds;
} bench_task_t;

/* One timed siever run. Usable inline (phase 1) or as a pthread (phase 2). */
static void *bench_worker_run(void *arg)
{
    bench_task_t *t = (bench_task_t *)arg;
#ifdef __linux__
    if (t->cpu >= 0) pin_current_thread(t->cpu);
#endif
    bench_cancel_t cc = { .deadline_ms = t->deadline_ms };
    struct timeval a, b;
    gettimeofday(&a, NULL);
    t->rc = sieve_run_local(t->cfg->siever_path, t->job_local, t->outfile,
                            t->q_start, t->q_count, t->side, t->siever_args,
                            bench_should_cancel, &cc);
    gettimeofday(&b, NULL);
    t->seconds = elapsed_seconds(a, b);
    t->relations = count_newlines(t->outfile);
    unlink(t->outfile);
    return NULL;
}

static void bench_usage(void)
{
    fprintf(stderr,
        "usage: ggnfs-sieve-client benchmark \\\n"
        "    --server-url=http://host:port  (required)\n"
        "    --token=<bearer token>         (required)\n"
        "    --siever=<path>                (required) gnfs-lasieve4* binary. benchmark\n"
        "                                   has no cuda engine yet; --engine is ignored\n"
        "    [--workers=N]                  sievers in the all-core phase (default: all online CPUs)\n"
        "    [--cpu-pin=0,2,4,...]          (Linux) pin each worker; length must equal --workers\n"
        "    [--qrange=65]                  q-interval WIDTH to sieve (siever -c; like a workunit's q_range);\n"
        "                                   holds ~width/ln(q) special-q; widen for ~1min single-core\n"
        "    [--q=N]                        fixed q anchor (default: campaign q_min from /stats)\n"
        "    [--max-seconds=300]            per-phase safety cap so a stuck box can't hang (0 = no cap)\n"
        "    [--min-rels-per-sec=X]         exit 3 if aggregate throughput is below X (for scripting)\n"
        "    [--workdir=/tmp/ggnfs-client]\n"
        "\n"
        "  Sieves a fixed q-range from a fixed q and times it. Reads job params\n"
        "  from /stats and fetches the .job by sha; never takes a lease.\n");
}

/* Read the job's sha, side, siever_args, and campaign q_min/q_max from the
 * server's /stats (read-only, no lease taken). Fills a synthetic lease struct
 * with just the fields ensure_file_cached / ensure_afb_cached need.
 * Returns 0 on success, -1 on connection/parse error, -2 if the server is too
 * old to expose job_sha256 (operator must rebuild + restart it). */
static int bench_fetch_params(struct mg_mgr *mgr, const client_cfg_t *cfg,
                              proto_lease_response_t *out,
                              int64_t *out_qmin, int64_t *out_qmax)
{
    char url[512];
    if (join_url(url, sizeof(url), cfg->server_url, "/stats") != 0) return -1;

    char headers[256];
    build_auth_headers(headers, sizeof(headers), cfg->token, "application/json", NULL);
    http_io_t io = {
        .url = url, .method = "GET",
        .extra_headers = headers,
        .body = NULL, .body_len = 0,
    };
    int rc = http_request(mgr, &io, 30000, 1);
    if (rc != 0) {
        fprintf(stderr, "benchmark: /stats connection failure\n");
        http_io_free(&io);
        return -1;
    }
    if (io.status == 401) {
        fprintf(stderr, "benchmark: 401 unauthorized — token wrong?\n");
        http_io_free(&io);
        return -1;
    }
    if (io.status != 200) {
        fprintf(stderr, "benchmark: /stats returned HTTP %d\n", io.status);
        http_io_free(&io);
        return -1;
    }

    cJSON *root = cJSON_ParseWithLength((const char *)io.resp_body, io.resp_body_len);
    http_io_free(&io);
    if (!root) {
        fprintf(stderr, "benchmark: could not parse /stats JSON\n");
        return -1;
    }

    memset(out, 0, sizeof(*out));
    cJSON *j_sha  = cJSON_GetObjectItemCaseSensitive(root, "job_sha256");
    cJSON *j_args = cJSON_GetObjectItemCaseSensitive(root, "siever_args");
    cJSON *j_side = cJSON_GetObjectItemCaseSensitive(root, "side");
    cJSON *j_siev = cJSON_GetObjectItemCaseSensitive(root, "siever");
    cJSON *j_wu   = cJSON_GetObjectItemCaseSensitive(root, "workunits");

    if (!(j_sha && cJSON_IsString(j_sha) && j_sha->valuestring &&
          strlen(j_sha->valuestring) == 64)) {
        fprintf(stderr, "benchmark: /stats has no job_sha256 — this server predates "
                "benchmark support.\n           Rebuild and restart the server "
                "(it now publishes job_sha256 + siever_args in /stats).\n");
        cJSON_Delete(root);
        return -2;
    }
    snprintf(out->file_sha256_hex, sizeof(out->file_sha256_hex), "%s", j_sha->valuestring);
    snprintf(out->file_url, sizeof(out->file_url), "/file/%s", out->file_sha256_hex);
    snprintf(out->siever_args, sizeof(out->siever_args), "%s",
             (j_args && cJSON_IsString(j_args) && j_args->valuestring) ? j_args->valuestring : "");
    out->side = (j_side && cJSON_IsString(j_side) && j_side->valuestring &&
                 j_side->valuestring[0]) ? j_side->valuestring[0] : 'a';
    if (j_siev && cJSON_IsString(j_siev) && j_siev->valuestring)
        snprintf(out->siever, sizeof(out->siever), "%s", j_siev->valuestring);

    int64_t qmin = 0, qmax = 0;
    if (j_wu && cJSON_IsObject(j_wu)) {
        cJSON *jq = cJSON_GetObjectItemCaseSensitive(j_wu, "q_min");
        cJSON *jx = cJSON_GetObjectItemCaseSensitive(j_wu, "q_max");
        if (jq && cJSON_IsNumber(jq)) qmin = (int64_t)jq->valuedouble;
        if (jx && cJSON_IsNumber(jx)) qmax = (int64_t)jx->valuedouble;
    }
    *out_qmin = qmin;
    *out_qmax = qmax;
    cJSON_Delete(root);
    return 0;
}

/* Returns 0 (good / no threshold), 1 (setup error), 2 (bad args),
 * 3 (ran, but aggregate throughput below --min-rels-per-sec). */
static int run_benchmark(int argc, char **argv, client_cfg_t *cfg)
{
    /* Fixed WORK, not fixed time: sieve a fixed q-interval WIDTH (the siever's
     * -c, exactly like a real workunit's q_range) from a fixed anchor q, and
     * measure how long it takes. The window holds ~width/ln(q) prime special-q;
     * each runs to completion, so the work — and the relation count — is
     * identical on every box, making the time a pure hardware signal. Widen it
     * for jobs with small per-special-q cost; aim for ~1 min single-core.
     * (A width too small to contain a prime sieves nothing and yields zero.) */
    int64_t qrange = 65;
    const char *qr = flag(argc, argv, "--qrange");
    if (qr && *qr && (parse_int64_arg(qr, &qrange) != 0 || qrange < 1 || qrange > 10000000)) {
        fprintf(stderr, "benchmark: --qrange must be an integer in 1..10000000\n");
        bench_usage();
        return 2;
    }

    /* Safety cap so a broken/stuck box can't hang forever; a healthy box
     * finishes the --qrange window well within it. 0 disables the cap. */
    int64_t max_seconds = 300;
    const char *ms = flag(argc, argv, "--max-seconds");
    if (ms && *ms && (parse_int64_arg(ms, &max_seconds) != 0 || max_seconds < 0 || max_seconds > 86400)) {
        fprintf(stderr, "benchmark: --max-seconds must be an integer in 0..86400 (0 = no cap)\n");
        return 2;
    }

    double min_rels = 0.0;
    const char *mr = flag(argc, argv, "--min-rels-per-sec");
    if (mr && *mr) {
        char *end = NULL;
        errno = 0;
        min_rels = strtod(mr, &end);
        if (errno != 0 || end == mr || *end != '\0' || min_rels < 0) {
            fprintf(stderr, "benchmark: --min-rels-per-sec must be a non-negative number\n");
            return 2;
        }
    }

    /* Fixed special-q anchor. Default: the campaign's q_min from /stats (same
     * on every box, a real production q). --q pins a constant that survives an
     * `extend`. 0 here means "use q_min once we've fetched /stats". */
    int64_t q_override = 0;
    const char *qflag = flag(argc, argv, "--q");
    if (qflag && *qflag &&
        (parse_int64_arg(qflag, &q_override) != 0 || q_override < 1 || q_override > 0xFFFFFFFF)) {
        fprintf(stderr, "benchmark: --q must be an integer in 1..4294967295\n");
        return 2;
    }

    char model[160];
    cpu_model(model, sizeof(model));
    printf("=== ggnfs box benchmark ===\n");
    printf("cpu        : %s\n", model);
    printf("threads    : %ld online / %ld configured\n", online_cpus(), configured_cpus());
    printf("workers    : %d\n", cfg->workers);
    printf("siever     : %s\n", cfg->siever_path);
    printf("q-range    : %lld wide (fixed work, same as a workunit's q_range)\n",
           (long long)qrange);
    fflush(stdout);

    if (mkdir_p(cfg->workdir) != 0) return 1;
    http_limiter_init(cfg->http_concurrency, cfg->http_interval_ms);

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    /* Read the job sha + side + args + q_min/q_max from /stats. No lease is
     * taken, so the work pool is untouched and the anchor q is deterministic. */
    proto_lease_response_t lease;
    int64_t qmin = 0, qmax = 0;
    int pr = bench_fetch_params(&mgr, cfg, &lease, &qmin, &qmax);
    if (pr != 0) {
        /* bench_fetch_params already explained the failure. */
        mg_mgr_free(&mgr);
        return 1;
    }

    char side = lease.side;
    char siever_args[128];
    snprintf(siever_args, sizeof(siever_args), "%s", lease.siever_args);

    /* Fixed anchor: --q if given, else the campaign's q_min. */
    int64_t anchor = q_override ? q_override : qmin;
    if (anchor < 1) {
        fprintf(stderr, "benchmark: server reports no q range (q_min=%lld); "
                "pass --q=<special-q> to benchmark this job.\n", (long long)qmin);
        mg_mgr_free(&mgr);
        return 1;
    }
    uint32_t base_q = (uint32_t)anchor;
    (void)qmax;
    /* Every phase-2 worker sieves the SAME special-q as the single-core run.
     * The factor base is shared read-only across workers in production too, so
     * identical work stresses memory bandwidth the same way while keeping
     * scaling a clean signal (no per-q-region cost differences to confound it).
     * Each worker is an independent process with its own private sieve arrays. */

    const char *siever_basename = strrchr(cfg->siever_path, '/');
    siever_basename = siever_basename ? siever_basename + 1 : cfg->siever_path;
    if (lease.siever[0] && strcmp(siever_basename, lease.siever) != 0) {
        fprintf(stderr, "benchmark: WARNING — server expects siever '%s' but --siever is '%s'; "
                "results reflect the binary you gave.\n", lease.siever, siever_basename);
    }
    printf("anchor q   : %u%s  side=%c  args=\"%s\"\n",
           base_q, q_override ? " (--q)" : " (q_min)", side, siever_args);
    fflush(stdout);

    char job_local[256];
    if (ensure_file_cached(&mgr, cfg, &lease, job_local, sizeof(job_local)) != 0) {
        fprintf(stderr, "benchmark: could not fetch the job file.\n");
        mg_mgr_free(&mgr);
        return 1;
    }

    /* Phase 0: build the factor-base cache once so phases 1-2 measure sieving,
     * not the 30-45s per-siever FB rebuild. ensure_afb_cached() only aborts on
     * a *second* Ctrl-C (SHUTDOWN_CANCELLING), unlike the sieve phases which
     * bail on the first — intentional: the FB cache is expensive and persists,
     * so a single Ctrl-C lets it finish (and be reused) before the drain check
     * below aborts the run, rather than throwing the work away half-built. */
    printf("\nfactor base: building/validating cache ...\n");
    fflush(stdout);
    struct timeval f0, f1;
    gettimeofday(&f0, NULL);
    ensure_afb_cached(cfg, &lease, job_local);
    gettimeofday(&f1, NULL);
    double fb_seconds = elapsed_seconds(f0, f1);

    mg_mgr_free(&mgr);  /* done talking to the server */

    char afb[300];
    snprintf(afb, sizeof(afb), "%s.afb.0", job_local);
    int have_afb = file_exists(afb);
    printf("factor base: %.1fs (%s)\n", fb_seconds,
           have_afb ? "cache ready" : "no cache");
    if (!have_afb) {
        fprintf(stderr, "benchmark: WARNING — no factor-base cache (siever may predate cache "
                "support); each siever will rebuild the FB, understating sieving throughput.\n");
    }
    if (shutdown_phase() >= SHUTDOWN_DRAINING) return 1;

    uint32_t qwidth = (uint32_t)qrange;
    /* Deadline is only a safety cap (a healthy box finishes the q-range well
     * within it); 0 means no cap. */
    int64_t cap_ms = max_seconds > 0 ? (int64_t)max_seconds * 1000 : (int64_t)1 << 62;

    /* Phase 1: single core. */
    printf("\nphase 1 (1 core ): sieving q-range [%u, %u) ...\n", base_q, base_q + qwidth);
    fflush(stdout);
    bench_task_t one;
    memset(&one, 0, sizeof(one));
    one.cfg = cfg;
    one.job_local = job_local;
    one.siever_args = siever_args;
    one.side = side;
    one.q_start = base_q;
    one.q_count = qwidth;
    one.cpu = cfg->cpu_pin_count > 0 ? cfg->cpu_pin_list[0] : -1;
    snprintf(one.outfile, sizeof(one.outfile), "%s/bench_single.dat", cfg->workdir);
    one.deadline_ms = monotonic_ms() + cap_ms;
    bench_worker_run(&one);
    double T1 = one.seconds;
    int one_capped = (max_seconds > 0 && T1 >= (double)max_seconds - 0.5);
    double single_rps = T1 > 0 ? (double)one.relations / T1 : 0.0;
    printf("phase 1 (1 core ): q-range %u in %.1fs -> %.1f rel/s (%lld relations)\n",
           qwidth, T1, single_rps, one.relations);
    fflush(stdout);
    if (one.relations == 0 || one_capped) {
        if (one_capped)
            fprintf(stderr, "benchmark: single-core phase hit the %llds cap before finishing "
                    "the q-range — this box is extremely slow or stuck.\n",
                    (long long)max_seconds);
        else
            fprintf(stderr, "benchmark: single-core phase produced no relations. The q-range "
                    "[%u,%u) may be too narrow to contain a prime special-q, or q is outside "
                    "this job's yield range — try a larger --qrange or a different --q.\n",
                    base_q, base_q + qwidth);
        return 1;
    }
    if (shutdown_phase() >= SHUTDOWN_DRAINING) return 1;

    /* Phase 2: every worker sieves the same q-range, all at once. */
    int W = cfg->workers;
    printf("\nphase 2 (%d cores): each worker sieves q-range [%u, %u) ...\n",
           W, base_q, base_q + qwidth);
    fflush(stdout);
    bench_task_t *tasks = calloc((size_t)W, sizeof(*tasks));
    pthread_t *tids = calloc((size_t)W, sizeof(*tids));
    if (!tasks || !tids) {
        fprintf(stderr, "benchmark: alloc failed\n");
        free(tasks); free(tids);
        return 1;
    }

    unsigned long long steal0 = 0, total0 = 0, steal1 = 0, total1 = 0;
    int have_steal = (read_cpu_steal(&steal0, &total0) == 0);

    int64_t phase2_deadline = monotonic_ms() + cap_ms;
    int spawned = 0;
    for (int k = 0; k < W; k++) {
        tasks[k].cfg = cfg;
        tasks[k].job_local = job_local;
        tasks[k].siever_args = siever_args;
        tasks[k].side = side;
        tasks[k].q_start = base_q;   /* identical work per worker */
        tasks[k].q_count = qwidth;
        tasks[k].deadline_ms = phase2_deadline;
        tasks[k].cpu = cfg->cpu_pin_count > 0 ? cfg->cpu_pin_list[k] : -1;
        snprintf(tasks[k].outfile, sizeof(tasks[k].outfile), "%s/bench_w%d.dat",
                 cfg->workdir, k);
        if (pthread_create(&tids[k], NULL, bench_worker_run, &tasks[k]) != 0) {
            fprintf(stderr, "benchmark: pthread_create failed for worker %d: %s\n",
                    k, strerror(errno));
            break;
        }
        spawned++;
    }
    for (int k = 0; k < spawned; k++) pthread_join(tids[k], NULL);
    if (have_steal) have_steal = (read_cpu_steal(&steal1, &total1) == 0);

    /* Fixed work per worker: sum each worker's own rel/s so a worker finishing
     * early (or late, under contention) is accounted at its true rate. */
    long long agg_rels = 0;
    double agg_rps = 0.0;
    double tw_min = 0.0, tw_max = 0.0;
    int any_capped = 0;
    for (int k = 0; k < spawned; k++) {
        double tw = tasks[k].seconds;
        agg_rels += tasks[k].relations;
        agg_rps  += tw > 0 ? (double)tasks[k].relations / tw : 0.0;
        if (k == 0 || tw < tw_min) tw_min = tw;
        if (k == 0 || tw > tw_max) tw_max = tw;
        if (max_seconds > 0 && tw >= (double)max_seconds - 0.5) any_capped = 1;
    }
    double scaling = (single_rps > 0 && spawned > 0)
                     ? agg_rps / (single_rps * spawned) : 0.0;
    double steal_pct = (have_steal && total1 > total0)
                       ? 100.0 * (double)(steal1 - steal0) / (double)(total1 - total0)
                       : 0.0;
    double load[3] = {0, 0, 0};
    (void)getloadavg(load, 3);

    printf("phase 2 (%d cores): %lld relations, worker time %.1f-%.1fs -> %.1f rel/s aggregate\n",
           W, agg_rels, tw_min, tw_max, agg_rps);
    if (any_capped)
        fprintf(stderr, "benchmark: WARNING — at least one worker hit the %llds cap; "
                "aggregate is understated (box is very slow/contended).\n",
                (long long)max_seconds);

    free(tasks);
    free(tids);

    /* ---- report ---- */
    printf("\n--- results ---\n");
    printf("single-core throughput : %.1f rel/s  (q-range %u in %.1fs)\n", single_rps, qwidth, T1);
    printf("aggregate throughput   : %.1f rel/s  (%d workers)\n", agg_rps, spawned);
    printf("multicore scaling      : %.0f%%  (aggregate / (single x %d))\n",
           scaling * 100.0, spawned);
    printf("slowest worker         : %.1fs  (fastest %.1fs)\n", tw_max, tw_min);
    if (have_steal)
        printf("cpu steal              : %.1f%%%s\n", steal_pct,
               steal_pct >= 5.0 ? "   <-- RED FLAG: hypervisor is contended" : "");
    else
        printf("cpu steal              : (unavailable)\n");
    printf("load average           : %.1f / %.1f / %.1f\n", load[0], load[1], load[2]);
    printf("\nCompare rel/s against a known-good box. Low scaling with low steal usually\n"
           "means memory-bandwidth contention from a noisy neighbor.\n");

    if (min_rels > 0.0 && agg_rps < min_rels) {
        printf("\nVERDICT: BELOW THRESHOLD (%.1f < %.1f rel/s) — consider re-rolling this box.\n",
               agg_rps, min_rels);
        return 3;
    }
    if (min_rels > 0.0)
        printf("\nVERDICT: OK (%.1f >= %.1f rel/s).\n", agg_rps, min_rels);
    return 0;
}

/* ===================== main ============================================= */

int main(int argc, char **argv)
{
    int is_bench = (argc > 1 && strcmp(argv[1], "benchmark") == 0);

    if (is_bench && flag(argc, argv, "--help") != NULL) {
        bench_usage();
        return 0;
    }

    /* Show the benchmark usage (not the generic client usage parse_config would
     * print) when a benchmark invocation is missing a required flag. */
    if (is_bench) {
        const char *u = flag(argc, argv, "--server-url");
        const char *t = flag(argc, argv, "--token");
        const char *s = flag(argc, argv, "--siever");
        if (!u || !*u || !t || !*t || !s || !*s) {
            bench_usage();
            return 2;
        }
    }

    client_cfg_t cfg;
    if (parse_config(argc, argv, &cfg) != 0) return 2;

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    mg_log_set(MG_LL_ERROR);  /* quiet by default; raise to debug */

    if (is_bench) {
        /* Default the all-core phase to every online CPU when the operator
         * didn't set --workers. (--cpu-pin requires a matching --workers, so a
         * pinned run already has an explicit worker count; the cpu_pin_count
         * guard is just defensive.) */
        if (!flag(argc, argv, "--workers") && cfg.cpu_pin_count == 0) {
            long n = online_cpus();
            if (n > CLIENT_MAX_WORKERS) n = CLIENT_MAX_WORKERS;
            cfg.workers = (int)n;
        }
        return run_benchmark(argc, argv, &cfg);
    }

    if (mkdir_p(cfg.workdir) != 0) return 1;

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
    /* One row per lease slot: a pipelined worker holds several leases at once
     * and the cancellation path must be able to release every one of them. */
    int active_rows = cfg.workers * (cfg.prefetch > 0 ? cfg.prefetch : 1);
    g_active = calloc((size_t)active_rows, sizeof(active_lease_t));
    g_active_count = active_rows;
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
