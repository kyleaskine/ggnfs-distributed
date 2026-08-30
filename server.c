/* ggnfs-sieve-server — Phase 1 walking skeleton.
 *
 *   ggnfs-sieve-server init   --job=foo.job --siever=gnfs-lasieve4I14e \
 *                            --qmin=80000000 --qmax=100000000 --qrange=10000 \
 *                            [--side=a] [--jobdir=.]
 *
 *   ggnfs-sieve-server serve  [--bind=127.0.0.1] [--port=8080] [--jobdir=.]
 *                            [--lease-seconds=3600]
 *                            [--sweep-seconds=60] [--max-attempts=5]
 *                            [--spotcheck-k=50]   (0 disables norm spot-check)
 *                            [--block-width-multiple=50]  (GPU block target =
 *                                        this x the job's base q_range)
 *                            [--block-max-members=256] [--block-min-q-width=0]
 *                            [--block-attempt-ceiling=2]
 *
 * The server reuses the bearer token from <jobdir>/token (written at init).
 * One job per server. No verification, no .ranges write-back, no /stats.
 */
#define _POSIX_C_SOURCE 200809L

#include "db.h"
#include "protocol.h"
#include "verify.h"
#include "vendor/cJSON.h"
#include "vendor/mongoose.h"

#include <zstd.h>

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
/* True if `key` appears with no '=' at all (e.g. `--gpu-args` instead of
 * `--gpu-args="..."`). flag() reports that as an empty value, which for a
 * job-wide setting means "clear it" — almost never what was meant. */
static int flag_is_bare(int argc, char **argv, const char *key)
{
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], key) == 0) return 1;
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

/* ===================== init subcommand ================================== */

/* Workunit classes. "cpu" is a gnfs-lasieve4-sized q_range; "gpu" is the much
 * wider band a card can chew through (see GPU-CLIENT.md for the sizing).
 * Validated at init/extend so a typo produces an empty pool at lease time
 * rather than a silently unclaimable band. */
static const char *const WU_CLASSES[] = { "cpu", "gpu", NULL };

static int class_is_valid(const char *c)
{
    for (int i = 0; WU_CLASSES[i]; i++)
        if (strcmp(c, WU_CLASSES[i]) == 0) return 1;
    return 0;
}

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
        "    [--class=cpu]           default (and only) value; 'gpu' is refused —\n"
        "                            GPU sizing is a lease property now (see\n"
        "                            --block-width-multiple on serve), not a row one\n"
        "    [--siever-args=<flags>] extra args appended to the siever command, e.g. \"-J 16\"\n"
        "    [--gpu-args=<flags>]    geometry/tuning flags for cuda-sieve clients, e.g.\n"
        "                            \"--logI 17 --J 16384\". Not a translation of\n"
        "                            --siever-args; the two describe different sieve areas\n"
        "    [--lease-order=asc|desc] default asc; 'desc' hands out highest-q workunits\n"
        "                            first (useful when another sieve is working upward\n"
        "                            from a higher q and you want to close the gap from\n"
        "                            the top)\n"
        "    [--jobdir=<dir>]        default current dir\n");
}

/* Reject a .job carrying bytes cuda-sieve cannot parse, before its SHA becomes
 * the job's identity.
 *
 * A live campaign was found distributing a file with a zero-width space
 * (U+200B) after "alambda: 3.6". gnfs-lasieve4 ignores it; cuda-sieve refuses
 * the file outright, so the GPU could not sieve that job at all. By then the
 * SHA was load-bearing — workunit IDs derive from it and finalize-nfs.sh
 * compares it — so it could only be worked around client-side.
 *
 * Catching it here fixes it once, for every engine and every future job.
 * Tabs and newlines are legitimate separators. */
static int job_bytes_are_clean(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 1;                    /* other code reports open failures */
    int c, line = 1, bad = 0, bad_line = 0;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') { line++; continue; }
        /* CR is fine: .job files routinely have CRLF endings (AS276.job does)
         * and cuda-sieve parses them without complaint. Only bytes it actually
         * chokes on are worth refusing a campaign over. */
        if (c == '\t' || c == '\r' || (c >= 0x20 && c <= 0x7e)) continue;
        if (!bad) bad_line = line;
        bad++;
    }
    fclose(f);
    if (bad) {
        fprintf(stderr,
            "init: %s contains %d unparseable byte(s) (first on line %d).\n"
            "  gnfs-lasieve4 tolerates these; cuda-sieve refuses the file, so a\n"
            "  GPU client could never sieve this job. The .job's SHA becomes the\n"
            "  job's identity here, so this cannot be fixed later without\n"
            "  orphaning the campaign.\n"
            "  Clean it first, e.g.:  perl -i -pe 's/[^\\x20-\\x7e\\t\\r\\n]//g' %s\n",
            path, bad, bad_line, path);
        return 0;
    }
    return 1;
}

static int cmd_init(int argc, char **argv)
{
    const char *job_path    = flag(argc, argv, "--job");
    const char *siever      = flag(argc, argv, "--siever");
    const char *qmin_s      = flag(argc, argv, "--qmin");
    const char *qmax_s      = flag(argc, argv, "--qmax");
    const char *qrange_s    = flag(argc, argv, "--qrange");
    const char *side_s      = flag(argc, argv, "--side");
    const char *class_s     = flag(argc, argv, "--class");
    const char *siever_args = flag(argc, argv, "--siever-args");
    const char *gpu_args    = flag(argc, argv, "--gpu-args");
    if (flag_is_bare(argc, argv, "--gpu-args")) {
        fprintf(stderr, "init: --gpu-args needs a value, e.g. "
                        "--gpu-args=\"--logI 17 --J 16384\"\n"
                        "  (to deliberately clear it, pass --gpu-args=)\n");
        return 2;
    }
    const char *lease_order = flag(argc, argv, "--lease-order");
    const char *jobdir      = flag(argc, argv, "--jobdir");

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
    const char *class_norm = (class_s && *class_s) ? class_s : "cpu";
    if (!class_is_valid(class_norm)) {
        fprintf(stderr, "init: --class must be 'cpu' or 'gpu'\n");
        return 2;
    }
    /* Same refusal as cmd_extend, and for the same reason — guarding only
     * `extend` would leave the whole hazard reachable from the other end.
     * Nothing sizes work around class any more, so a gpu-class campaign is
     * just a campaign of enormous workunits that a single core will time out
     * on and poison. */
    if (strcmp(class_norm, "gpu") == 0) {
        fprintf(stderr,
            "init: --class=gpu is no longer supported.\n"
            "  GPU sizing is a property of the LEASE now, not of the row: a\n"
            "  client asking for a block gets one lease over N contiguous\n"
            "  base-width workunits, sized by --block-width-multiple on serve.\n"
            "  Initialise at your normal --qrange and let blocks do the widening.\n");
        return 2;
    }
    const char *lease_order_norm = "asc";
    if (lease_order && *lease_order) {
        if (strcmp(lease_order, "asc") == 0 || strcmp(lease_order, "desc") == 0) {
            lease_order_norm = lease_order;
        } else {
            fprintf(stderr, "init: --lease-order must be 'asc' or 'desc'\n");
            return 2;
        }
    }

    /* Layout. */
    if (mkdir_p(jobdir) != 0) return 1;
    char *files_dir = path_join(jobdir, "files");
    char *rels_dir  = path_join(jobdir, "rels");
    if (!files_dir || !rels_dir) { free(files_dir); free(rels_dir); return 1; }
    if (mkdir_p(files_dir) != 0 || mkdir_p(rels_dir) != 0) {
        free(files_dir); free(rels_dir); return 1;
    }

    /* Check the bytes BEFORE hashing: once the SHA is taken it is the job's
     * identity and the file can no longer be corrected. */
    if (!job_bytes_are_clean(job_path)) {
        free(files_dir); free(rels_dir); return 2;
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
    if (!db_path) { free(files_dir); free(rels_dir); free(dst_path); return 1; }
    ggnfs_db_t *db = db_open(db_path);
    if (!db) {
        free(files_dir); free(rels_dir); free(dst_path); free(db_path); return 1;
    }

    if (db_files_insert(db, job_sha, dst_path, dst_bytes, "job") != 0) {
        fprintf(stderr, "init: db_files_insert failed\n");
        db_close(db);
        free(files_dir); free(rels_dir); free(dst_path); free(db_path);
        return 1;
    }

    /* Parse the .job for polynomial coefficients and stash them in meta so
     * the verifier (Phase 3) doesn't have to re-parse the file at startup. */
    {
        verify_poly_t poly;
        if (verify_parse_job_file(dst_path, &poly) != 0) {
            fprintf(stderr, "init: failed to parse polynomial from %s\n", dst_path);
            db_close(db);
            free(files_dir); free(rels_dir); free(dst_path); free(db_path);
            return 1;
        }
        if (verify_poly_save_to_meta(db, &poly) != 0) {
            fprintf(stderr, "init: failed to store polynomial in meta\n");
            verify_poly_free(&poly);
            db_close(db);
            free(files_dir); free(rels_dir); free(dst_path); free(db_path);
            return 1;
        }
        verify_poly_free(&poly);
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
        if (db_workunit_insert(db, id, q, this_range, side, class_norm, now) != 0) {
            fprintf(stderr, "init: db_workunit_insert failed at seq=%lld\n", (long long)seq);
            db_close(db);
            free(files_dir); free(rels_dir); free(dst_path); free(db_path);
            return 1;
        }
        seq++;
    }

    /* Token. Stash in meta for self-checking + write to <jobdir>/token. */
    char token[65];
    if (random_token_hex(token) != 0) {
        db_close(db);
        free(files_dir); free(rels_dir); free(dst_path); free(db_path);
        return 1;
    }
    char *token_path = path_join(jobdir, "token");
    if (!token_path) {
        db_close(db);
        free(files_dir); free(rels_dir); free(dst_path); free(db_path);
        return 1;
    }
    {
        int fd = open(token_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) {
            fprintf(stderr, "init: cannot write %s: %s\n", token_path, strerror(errno));
            db_close(db);
            free(files_dir); free(rels_dir); free(dst_path); free(db_path); free(token_path);
            return 1;
        }
        ssize_t w1 = write(fd, token, 64);
        ssize_t w2 = write(fd, "\n",  1);
        close(fd);
        if (w1 != 64 || w2 != 1) {
            fprintf(stderr, "init: short write to %s\n", token_path);
            db_close(db);
            free(files_dir); free(rels_dir); free(dst_path); free(db_path); free(token_path);
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
    db_meta_set(db, "job_sha256",  job_sha);
    db_meta_set(db, "siever_args", siever_args ? siever_args : "");
    db_meta_set(db, "gpu_args",    gpu_args    ? gpu_args    : "");
    db_meta_set(db, "lease_order", lease_order_norm);

    db_close(db);

    printf("ggnfs-sieve-server: initialized job %s\n", job_id);
    printf("  jobdir   : %s\n", jobdir);
    printf("  job.db   : %s\n", db_path);
    printf("  job file : %s  (sha=%s, %lld bytes)\n", dst_path, job_sha, (long long)dst_bytes);
    printf("  workunits: %lld   (q_range=%lld, class=%s, side=%c, siever=%s, lease_order=%s)\n",
           (long long)seq, (long long)qrange, class_norm, side, siever, lease_order_norm);
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
        "    --qmin=<int>            (required) start of new range\n"
        "    --qmax=<int>            (required) end (exclusive); [qmin, qmax) must not\n"
        "                            overlap any existing workunit\n"
        "    --qrange=<int>          (required) per-workunit range size\n"
        "    [--class=cpu]           default (and only) value; 'gpu' is refused —\n"
        "                            GPU sizing is a lease property now (see\n"
        "                            --block-width-multiple on serve), not a row one\n"
        "    [--gpu-args=<flags>]    set/replace the job-wide cuda-sieve flags, e.g.\n"
        "                            \"--logI 17 --J 16384\". Omit to leave as-is\n"
        "\nAdds workunits to an existing job. The new range can sit above, below, or\n"
        "in a gap between existing workunits — any non-overlapping placement is OK.\n"
        "Token, .job file, siever, and side are inherited from init. Sequence\n"
        "numbering continues from the last init/extend so workunit IDs stay unique\n"
        "(they no longer track q-order once you extend below or into a gap).\n");
}

static int cmd_extend(int argc, char **argv)
{
    const char *jobdir   = flag(argc, argv, "--jobdir");
    const char *qmin_s   = flag(argc, argv, "--qmin");
    const char *qmax_s   = flag(argc, argv, "--qmax");
    const char *qrange_s = flag(argc, argv, "--qrange");
    const char *class_s  = flag(argc, argv, "--class");
    const char *gpu_args = flag(argc, argv, "--gpu-args");
    if (flag_is_bare(argc, argv, "--gpu-args")) {
        fprintf(stderr, "extend: --gpu-args needs a value, e.g. "
                        "--gpu-args=\"--logI 17 --J 16384\"\n"
                        "  (to deliberately clear it, pass --gpu-args=)\n");
        return 2;
    }

    if (!jobdir || !qmin_s || !qmax_s || !qrange_s) {
        usage_extend();
        return 2;
    }
    const char *class_norm = (class_s && *class_s) ? class_s : "cpu";
    if (!class_is_valid(class_norm)) {
        fprintf(stderr, "extend: --class must be 'cpu' or 'gpu'\n");
        return 2;
    }
    /* Carving wide gpu-class rows is how v1 fed a card, and it is exactly what
     * blocks replaced. Accepting it now would create rows that nothing sizes
     * work around and that no longer get any special treatment at lease time —
     * a card would simply be handed one enormous workunit. Refuse rather than
     * leave a documented flag that quietly does the wrong thing. */
    if (strcmp(class_norm, "gpu") == 0) {
        fprintf(stderr,
            "extend: --class=gpu is no longer supported.\n"
            "  GPU sizing is a property of the LEASE now, not of the row: a\n"
            "  client asking for a block gets one lease over N contiguous\n"
            "  base-width workunits, sized by --block-width-multiple on serve.\n"
            "  Extend at your normal --qrange and let blocks do the widening.\n");
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

    /* Find next sequence number. Sequence IDs are just unique handles; they
     * don't need to track q-order, so extending below or into a gap is fine.
     * It comes from the highest suffix in use rather than from the row count,
     * so that an id is never reused even if rows are ever removed. */
    int64_t existing_count = 0, next_seq = 0;
    if (db_workunit_extent(db, &existing_count, NULL) != 0 ||
        db_workunit_next_seq(db, job_id, &next_seq) != 0) {
        fprintf(stderr, "extend: cannot read existing workunits\n");
        db_close(db); free(db_path); return 1;
    }
    if (existing_count == 0) {
        fprintf(stderr, "extend: no existing workunits — use 'init' first\n");
        db_close(db); free(db_path); return 1;
    }

    /* Reject only on actual overlap with an already-sieved range. */
    int64_t over_q_start = 0, over_q_range = 0;
    int ov = db_workunit_overlap(db, qmin, qmax, &over_q_start, &over_q_range);
    if (ov < 0) {
        fprintf(stderr, "extend: cannot check for overlap\n");
        db_close(db); free(db_path); return 1;
    }
    if (ov == 1) {
        fprintf(stderr,
            "extend: [%lld, %lld) overlaps existing workunit [%lld, %lld).\n",
            (long long)qmin, (long long)qmax,
            (long long)over_q_start,
            (long long)(over_q_start + over_q_range));
        db_close(db); free(db_path); return 1;
    }

    /* Insert new workunits, continuing the sequence. */
    int64_t now = now_unix();
    int64_t seq = next_seq;
    int64_t added = 0;
    int64_t q;
    for (q = qmin; q < qmax; q += qrange) {
        int64_t this_range = (q + qrange <= qmax) ? qrange : (qmax - q);
        char id[64];
        snprintf(id, sizeof(id), "wu-%s-%06lld", job_id, (long long)seq);
        if (db_workunit_insert(db, id, q, this_range, side, class_norm, now) != 0) {
            fprintf(stderr, "extend: db_workunit_insert failed at seq=%lld\n",
                    (long long)seq);
            db_close(db); free(db_path); return 1;
        }
        seq++;
        added++;
    }

    /* Job-wide, like siever_args — not per-band. Setting it here is the
     * ergonomic path: you learn the right geometry when you add the GPU band,
     * not at init time. Must happen before db_close. */
    if (gpu_args) db_meta_set(db, "gpu_args", gpu_args);

    db_close(db);

    printf("ggnfs-sieve-server: extended job %s\n", job_id);
    if (gpu_args) {
        printf("  gpu_args      : %s\n", *gpu_args ? gpu_args : "(cleared)");
        /* serve reads meta once at startup, so a running coordinator keeps
         * handing out the OLD gpu_args while happily leasing the new band —
         * every cuda client would then sieve it with the wrong geometry.
         * New workunits go live immediately; this value does not. */
        printf("\n  NOTE: restart `serve` for the new gpu_args to reach clients.\n"
               "        Until then the new workunits are leasable but carry the\n"
               "        previously loaded gpu_args.\n");
    }
    printf("  new workunits : %lld   (q_range=%lld, class=%s, side=%c)\n",
           (long long)added, (long long)qrange, class_norm, side);
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
    char        siever_args[128];   /* extra flags shipped to clients (may be "") */
    char        gpu_args[192];      /* cuda-sieve flags shipped to clients (may be "") */
    char        side;
    char        job_sha256[65];
    char       *jobdir;
    char       *rels_dir;
    int64_t     lease_seconds;
    int64_t     sweep_seconds;
    int64_t     started_at;
    int64_t     max_attempts;       /* mark workunit poisoned after this many lease expiries */
    int         lease_desc;         /* 0 = hand out lowest-q_start first, 1 = highest first */
    verify_thread_t *verifier;      /* background parse-pass verifier; may be NULL */

    /* ---- gpu blocks ----
     * A block's target width is DERIVED, not configured: it is
     * block_width_multiple times the job's own base q_range. Base q_range is
     * already normalised to wall clock — an operator picks --qrange so one
     * workunit is a sane slice of a CPU core — and a GPU is a roughly
     * job-independent multiple of a core, so a fixed multiple holds the GPU
     * band near a constant wall-clock target across jobs of any size. A
     * hardcoded width would be a 3-minute band on one job and an hours-long
     * one on the next. */
    int64_t     block_width_multiple;
    int64_t     block_max_members;    /* hard clamp; a client's ask is advice */
    int64_t     block_min_q_width;    /* 0 = target/4 */
    int64_t     block_attempt_ceiling;/* must stay < max_attempts */

    /* db_workunit_base_q_range is a full covering-index scan plus two temp
     * b-trees — 38 ms on a 390K-row jobdir, measured. The verifier can afford
     * that once per drain pass; the lease path, on the mongoose thread next to
     * /submit and /stats, cannot afford it per request. Cache it, and re-read
     * on a timer so `extend` adding a differently-sized band under a running
     * serve is still picked up. */
    int64_t     base_q_range;
    int64_t     base_q_range_at;
} server_ctx_t;

#define BASE_Q_RANGE_TTL_SECONDS 60

#define COMMAND_TEMPLATE_DEFAULT \
    "{siever} -f {q_start} -c {q_range} -a {job_file} -o {output_file} -n 0"
#define OUTPUT_NAME_DEFAULT  "rels.out"
#define OUTPUT_MAX_BYTES     (524288000LL)  /* 500 MiB per design */
#define SERVER_POLL_MS       50

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

static void send_json_take_close(struct mg_connection *c, int code, char *json_owned)
{
    /* /stats is browser-polled and often interrupted by reloads. Do not leave
     * those fetch connections in the browser's keep-alive pool. */
    const char *headers =
        "Content-Type: application/json\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n";
    if (json_owned) {
        mg_http_reply(c, code, headers, "%s", json_owned);
        free(json_owned);
    } else {
        mg_http_reply(c, code, headers, "{}");
    }
    c->is_draining = 1;
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

/* The job's base band width, re-read at most once per TTL. Returns 0 if the
 * workunits table is empty, which disables blocks rather than guessing. */
static int64_t base_q_range_cached(server_ctx_t *ctx, int64_t now)
{
    if (ctx->base_q_range <= 0 ||
        now - ctx->base_q_range_at >= BASE_Q_RANGE_TTL_SECONDS) {
        int64_t base = 0;
        if (db_workunit_base_q_range(ctx->db, &base) == 0 && base > 0) {
            if (base != ctx->base_q_range) {
                fprintf(stderr, "blocks: base q_range = %lld "
                                "(target %lld x %lld = %lld)\n",
                        (long long)base, (long long)ctx->block_width_multiple,
                        (long long)base,
                        (long long)(base * ctx->block_width_multiple));
            }
            ctx->base_q_range = base;
        }
        ctx->base_q_range_at = now;
    }
    return ctx->base_q_range;
}

/* ---- /lease ---- */

static void handle_lease(struct mg_connection *c, struct mg_http_message *hm,
                         server_ctx_t *ctx)
{
    if (!check_auth(c, hm, ctx->token)) return;

    char client_id[64] = {0};
    char client_ver[32] = {0};
    char class_want[16] = {0};
    int     want_block = 0;
    int64_t want_members = 0;
    proto_decode_lease_request(hm->body.buf, hm->body.len,
                               client_id, sizeof(client_id),
                               client_ver, sizeof(client_ver),
                               class_want, sizeof(class_want),
                               &want_block, &want_members);
    if (client_id[0] == '\0') {
        send_text(c, 400, "missing client_id\n");
        return;
    }
    /* Pre-class clients send no "class" at all, and they are all CPU. */
    if (class_want[0] == '\0') snprintf(class_want, sizeof(class_want), "cpu");
    if (!class_is_valid(class_want)) {
        send_text(c, 400, "unknown class\n");
        return;
    }

    db_clients_seen(ctx->db, client_id, now_unix(), class_want);

    db_lease_result_t r;
    int64_t block_members = 0;
    int     rc            = 1;

    /* Block path first, when the client asked for one. A block is returned
     * through the ordinary lease fields — workunit_id is its anchor and
     * [q_start, q_start+q_range) spans every member — so the client sieves it
     * with the same code path as a single workunit. That is the whole point of
     * anchored addressing: /submit, /renew, /release, the relation filename
     * and finalize-nfs.sh's wu-* glob all keep working untouched. */
    if (want_block) {
        int64_t base   = base_q_range_cached(ctx, now_unix());
        int64_t target = base > 0 ? base * ctx->block_width_multiple : 0;
        int64_t maxm   = ctx->block_max_members;
        /* The client's ask is advice: it flows into a LIMIT, so an unclamped
         * value would let one request lease the rest of the campaign. */
        if (want_members > 0 && want_members < maxm) maxm = want_members;

        if (target > 0) {
            int64_t minw = ctx->block_min_q_width > 0
                         ? ctx->block_min_q_width : target / 4;
            db_block_t b;
            int brc = db_block_lease(ctx->db, ctx->job_id, client_id,
                                     target, minw, maxm,
                                     ctx->block_attempt_ceiling,
                                     ctx->lease_seconds, now_unix(), &b);
            if (brc == 0) {
                snprintf(r.id, sizeof(r.id), "%s", b.anchor_wu_id);
                r.q_start = b.q_start;
                r.q_range = b.q_end - b.q_start;
                r.side    = b.side;
                snprintf(r.class, sizeof(r.class), "gpu");
                block_members = b.member_count;
                rc = 0;
            } else if (brc < 0) {
                send_text(c, 500, "internal error\n");
                return;
            }
            /* brc == 1: no run wide enough. Fall through to an ordinary
             * single-workunit lease — a card on a small band is merely
             * inefficient, and this is what keeps a GPU working through the
             * endgame when contiguous runs have run out. */
        }
    }

    if (rc != 0)
        rc = db_lease(ctx->db, client_id, ctx->lease_seconds, now_unix(),
                      ctx->lease_desc, class_want, &r);
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
        .siever_args      = ctx->siever_args,
        .gpu_args         = ctx->gpu_args,
        .file_name        = "job.txt",
        .file_sha256_hex  = ctx->job_sha256,
        .file_url         = file_url,
        .output_name      = OUTPUT_NAME_DEFAULT,
        .output_max_bytes = OUTPUT_MAX_BYTES,
        .block_members    = block_members,
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

static int workunit_id_is_safe_for_job(const char *id, const char *job_id)
{
    size_t job_len = strlen(job_id);
    size_t prefix_len = 3 + job_len + 1;  /* "wu-" + job_id + "-" */

    if (job_len == 0) return 0;
    if (strncmp(id, "wu-", 3) != 0) return 0;
    if (strncmp(id + 3, job_id, job_len) != 0) return 0;
    if (id[3 + job_len] != '-') return 0;
    if (id[prefix_len] == '\0') return 0;

    for (const char *p = id + prefix_len; *p; p++) {
        if (!isdigit((unsigned char)*p)) return 0;
    }
    return 1;
}

static int count_zstd_newlines(const void *src, size_t src_len,
                               int64_t max_decoded_bytes,
                               int64_t *out_newlines)
{
    ZSTD_DStream *ds = ZSTD_createDStream();
    if (!ds) return -1;
    size_t init = ZSTD_initDStream(ds);
    if (ZSTD_isError(init)) {
        ZSTD_freeDStream(ds);
        return -1;
    }

    unsigned char outbuf[64 * 1024];
    ZSTD_inBuffer in = { src, src_len, 0 };
    int64_t nlines = 0;
    int64_t decoded = 0;
    size_t ret = 1;
    while (in.pos < in.size || ret != 0) {
        ZSTD_outBuffer out = { outbuf, sizeof(outbuf), 0 };
        ret = ZSTD_decompressStream(ds, &out, &in);
        if (ZSTD_isError(ret)) {
            ZSTD_freeDStream(ds);
            return -1;
        }
        decoded += (int64_t)out.pos;
        if (decoded > max_decoded_bytes) {
            ZSTD_freeDStream(ds);
            errno = EFBIG;
            return -1;
        }
        for (size_t i = 0; i < out.pos; i++) {
            if (outbuf[i] == '\n') nlines++;
        }
        if (out.pos == 0 && in.pos == in.size && ret != 0) {
            ZSTD_freeDStream(ds);
            return -1;
        }
    }
    ZSTD_freeDStream(ds);
    if (out_newlines) *out_newlines = nlines;
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
    if (!workunit_id_is_safe_for_job(workunit_id, ctx->job_id)) {
        send_text(c, 400, "invalid X-Workunit-Id\n");
        return;
    }
    /* X-Sha256 and X-Sieve-Seconds are advisory in Phase 1. */
    if (header_get_str(hm, "X-Sha256", client_sha, sizeof(client_sha)) != 0)
        client_sha[0] = '\0';
    char compression[32];
    int is_zstd = 0;
    if (header_get_str(hm, "X-Compression", compression, sizeof(compression)) == 0) {
        if (strcmp(compression, "zstd") == 0) {
            is_zstd = 1;
        } else if (strcmp(compression, "none") != 0) {
            send_text(c, 400, "unsupported X-Compression\n");
            return;
        }
    }
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

    /* Count newlines as a rough proxy for relation count. For compressed
     * submissions this also rejects malformed zstd frames before storing. */
    int64_t num_relations = 0;
    if (is_zstd) {
        if (count_zstd_newlines(hm->body.buf, hm->body.len,
                                OUTPUT_MAX_BYTES, &num_relations) != 0) {
            if (errno == EFBIG) {
                send_text(c, 413, "decompressed submission too large\n");
            } else {
                send_text(c, 400, "invalid zstd submission\n");
            }
            return;
        }
    } else {
        for (size_t i = 0; i < hm->body.len; i++) {
            if (hm->body.buf[i] == '\n') num_relations++;
        }
    }

    /* Resolve the block BEFORE choosing a filename: a block's file must not be
     * named after its anchor.
     *
     * The anchor is a real workunit, and a block that passes with its lowest-q
     * member starved sends exactly that member back to 'available'. Re-leased
     * on its own it would submit to <anchor>.dat.zst and overwrite the block's
     * file — silently discarding the other members' relations, which are not
     * recoverable and would not show up until filtering came out short. Naming
     * a block's file after the block makes the collision impossible: block ids
     * are server-generated and never reused. */
    db_block_t blk;
    int found = db_block_find_live(ctx->db, workunit_id, &blk);
    if (found < 0) { send_text(c, 500, "internal error\n"); return; }

    /* Persist body to <jobdir>/rels/<workunit_id|block_id>.dat[.zst]. Both are
     * server-generated ids containing no path separators — workunit_id has
     * already been through workunit_id_is_safe_for_job. */
    char rel_name[96];
    snprintf(rel_name, sizeof(rel_name), "%s.dat%s",
             found == 0 ? blk.id : workunit_id, is_zstd ? ".zst" : "");
    char *rel_path = path_join(ctx->rels_dir, rel_name);
    if (!rel_path) { send_text(c, 500, "oom\n"); return; }

    /* Stage to a per-request temp file and rename only once the DB has
     * ACCEPTED the submission.
     *
     * Writing straight to rel_path lets a client whose lease already lapsed
     * clobber the file of whoever the sweep reissued the workunit to — and
     * then the 409 path below would unlink() it, destroying a submission that
     * was already recorded. rename(2) is atomic within a directory, so the
     * final name only ever appears with content the DB agreed to. */
    char tmp_name[128];
    snprintf(tmp_name, sizeof(tmp_name), "%s.tmp.%ld.%p",
             rel_name, (long)getpid(), (void *)c);
    char *tmp_path = path_join(ctx->rels_dir, tmp_name);
    if (!tmp_path) { free(rel_path); send_text(c, 500, "oom\n"); return; }
    {
        FILE *f = fopen(tmp_path, "wb");
        if (!f) {
            fprintf(stderr, "submit: open %s: %s\n", tmp_path, strerror(errno));
            free(tmp_path); free(rel_path);
            send_text(c, 500, "cannot write submission\n");
            return;
        }
        if (fwrite(hm->body.buf, 1, hm->body.len, f) != hm->body.len) {
            fclose(f);
            unlink(tmp_path);
            free(tmp_path); free(rel_path);
            send_text(c, 500, "short write\n");
            return;
        }
        fclose(f);
    }

    db_clients_seen(ctx->db, client_id, now_unix(), NULL);

    /* Same anchor routing as /renew (the lookup happened above, before the
     * filename was chosen). db_block_submit additionally proves ownership, so
     * a client that is not the lease holder gets 409 rather than marking the
     * anchor submitted underneath the block that owns it. */
    int rc;
    if (found == 0) {
        rc = db_block_submit(ctx->db, workunit_id, client_id, rel_path,
                             body_sha, num_relations, sieve_seconds, now_unix());
    } else if (found == 1) {
        rc = db_submit(ctx->db, workunit_id, client_id, rel_path, body_sha,
                       num_relations, sieve_seconds, now_unix());
    } else {
        rc = -1;
    }
    if (rc == 1) {
        /* Not leased to this client — re-issued, stale, or a late block whose
         * members have moved on. Only the staging file is removed; whatever is
         * at rel_path belongs to the current holder. */
        unlink(tmp_path);
        free(tmp_path); free(rel_path);
        send_text(c, 409, "workunit not leased to client\n");
        return;
    }
    if (rc != 0) {
        unlink(tmp_path);
        free(tmp_path); free(rel_path);
        send_text(c, 500, "internal error\n");
        return;
    }
    /* Accepted: publish the staged file under the name the submission row
     * records. A failure here leaves the row pointing at a file that does not
     * exist, which the verifier reports as an open error rather than silently
     * passing, so it is loud either way. */
    if (rename(tmp_path, rel_path) != 0) {
        fprintf(stderr, "submit: rename %s -> %s: %s\n",
                tmp_path, rel_path, strerror(errno));
        unlink(tmp_path);
    }
    free(tmp_path);
    free(rel_path);

    /* Nudge the verifier so it processes this submission promptly instead of
     * waiting for its 5-second timed-wait safety net. */
    verify_thread_wake(ctx->verifier);

    send_json_take(c, 200, proto_encode_submit_response(1, "pending", num_relations));
}

/* ---- /renew ---- */
/*
 * Lease heartbeat. A client sieving a wide GPU-class band would otherwise
 * have to be covered by a --lease-seconds long enough for the slowest
 * workunit on the slowest box, which is also how long a *dead* client's work
 * sits unreclaimed. Heartbeating decouples the two: hold the band as long as
 * you keep talking, lose it within lease_seconds if you stop.
 *
 * Shares the {workunit_id, client_id} request shape with /release.
 */
static void handle_renew(struct mg_connection *c, struct mg_http_message *hm,
                         server_ctx_t *ctx)
{
    if (!check_auth(c, hm, ctx->token)) return;

    char workunit_id[64], client_id[64];
    if (proto_decode_release_request(hm->body.buf, hm->body.len,
                                     workunit_id, sizeof(workunit_id),
                                     client_id,   sizeof(client_id)) != 0 ||
        workunit_id[0] == '\0' || client_id[0] == '\0') {
        send_text(c, 400, "missing workunit_id / client_id\n");
        return;
    }
    if (!workunit_id_is_safe_for_job(workunit_id, ctx->job_id)) {
        send_text(c, 400, "invalid workunit_id\n");
        return;
    }

    db_clients_seen(ctx->db, client_id, now_unix(), NULL);

    /* An id that anchors a live block renews the whole block; anything else is
     * an ordinary workunit. Routing on the anchor rather than on a client hint
     * means a confused client cannot renew half a block. */
    db_block_t blk;
    int found = db_block_find_live(ctx->db, workunit_id, &blk);
    int rc;
    if (found == 0) {
        int64_t renewed = 0;
        rc = db_block_renew(ctx->db, workunit_id, client_id,
                            ctx->lease_seconds, now_unix(), &renewed);
        if (rc == 0 && renewed != blk.member_count) {
            /* Part of the block is gone: the sweep reclaimed a member whose
             * lease lapsed just before this heartbeat landed. db_block_submit
             * requires every member, so this band can no longer be submitted —
             * it is already dead, and answering 200 would leave the card
             * sieving it for another quarter hour before finding out. 409 is
             * the documented "abandon immediately" signal. */
            fprintf(stderr, "renew: block %s renewed %lld of %lld members — "
                            "telling the client to abandon it\n",
                    blk.id, (long long)renewed, (long long)blk.member_count);
            rc = 1;
        }
    } else if (found == 1) {
        rc = db_renew_lease(ctx->db, workunit_id, client_id,
                            ctx->lease_seconds, now_unix());
    } else {
        rc = -1;
    }
    if (rc == 0) {
        send_json_take(c, 200,
                       proto_encode_renew_response(1, ctx->lease_seconds));
    } else if (rc == 1) {
        /* Either never leased to this client, or the lease already lapsed and
         * the sweep took it back. Both mean: stop working on it. */
        send_text(c, 409, "no live lease for client\n");
    } else {
        send_text(c, 500, "internal error\n");
    }
}

/* ---- /release ---- */

static void handle_release(struct mg_connection *c, struct mg_http_message *hm,
                           server_ctx_t *ctx)
{
    if (!check_auth(c, hm, ctx->token)) return;

    char workunit_id[64], client_id[64];
    if (proto_decode_release_request(hm->body.buf, hm->body.len,
                                     workunit_id, sizeof(workunit_id),
                                     client_id,   sizeof(client_id)) != 0 ||
        workunit_id[0] == '\0' || client_id[0] == '\0') {
        send_text(c, 400, "missing workunit_id / client_id\n");
        return;
    }
    if (!workunit_id_is_safe_for_job(workunit_id, ctx->job_id)) {
        send_text(c, 400, "invalid workunit_id\n");
        return;
    }

    db_clients_seen(ctx->db, client_id, now_unix(), NULL);
    db_block_t blk;
    int found = db_block_find_live(ctx->db, workunit_id, &blk);
    int rc;
    if (found == 0)      rc = db_block_release(ctx->db, workunit_id, client_id);
    else if (found == 1) rc = db_release_lease(ctx->db, workunit_id, client_id);
    else                 rc = -1;
    if (rc == 0) {
        send_json_take(c, 200, proto_encode_submit_response(1, "released", 0));
    } else if (rc == 1) {
        send_text(c, 409, "workunit not leased to client\n");
    } else {
        send_text(c, 500, "internal error\n");
    }
}

/* ---- /stats ---- */

static char *format_stats_json(server_ctx_t *ctx, const db_stats_t *s,
                               int64_t now)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddStringToObject (root, "job_id",         ctx->job_id);
    cJSON_AddStringToObject (root, "siever",         ctx->siever);
    cJSON_AddStringToObject (root, "job_sha256",     ctx->job_sha256);
    cJSON_AddStringToObject (root, "siever_args",    ctx->siever_args);
    cJSON_AddStringToObject (root, "gpu_args",       ctx->gpu_args);
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
    /* Size-weighted progress. With a GPU band ~100x wider than a CPU one,
     * "workunits done / total" is not progress; these are. */
    /* Per-state q-width: the same five buckets as the counts above, weighted
     * by band width. The dashboard's bar and percentage read from these. */
    cJSON *qw = cJSON_AddObjectToObject(wu, "q");
    if (qw) {
        cJSON_AddNumberToObject(qw, "total",     (double)s->q.total);
        cJSON_AddNumberToObject(qw, "available", (double)s->q.available);
        cJSON_AddNumberToObject(qw, "leased",    (double)s->q.leased);
        cJSON_AddNumberToObject(qw, "submitted", (double)s->q.submitted);
        cJSON_AddNumberToObject(qw, "verified",  (double)s->q.verified);
        cJSON_AddNumberToObject(qw, "failed",    (double)s->q.failed);
        cJSON_AddNumberToObject(qw, "poisoned",  (double)s->q.poisoned);
    }
    cJSON_AddNumberToObject(wu, "q_passed_1h",   (double)s->q_passed_1h);

    cJSON *cls = cJSON_AddArrayToObject(root, "classes");
    /* cJSON_AddItemToArray does not take ownership when the array is NULL, so
     * without this guard every object built below would leak on the
     * allocation-failure path — once per dashboard poll. */
    for (int i = 0; cls && i < s->class_count; i++) {
        const db_stats_class_t *cc = &s->classes[i];
        cJSON *o = cJSON_CreateObject();
        if (!o) continue;
        cJSON_AddStringToObject(o, "name",       cc->name);
        cJSON_AddNumberToObject(o, "total",      (double)cc->total);
        cJSON_AddNumberToObject(o, "available",  (double)cc->available);
        cJSON_AddNumberToObject(o, "leased",     (double)cc->leased);
        cJSON_AddNumberToObject(o, "submitted",  (double)cc->submitted);
        cJSON_AddNumberToObject(o, "verified",   (double)cc->verified);
        cJSON_AddNumberToObject(o, "q_total",    (double)cc->q_total);
        cJSON_AddNumberToObject(o, "q_submitted", (double)cc->q_submitted);
        cJSON_AddNumberToObject(o, "q_verified", (double)cc->q_verified);
        cJSON_AddItemToArray(cls, o);
    }

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
        cJSON_AddNumberToObject(o, "rel_per_sec",        cc->rel_per_sec);
        cJSON_AddNumberToObject(o, "sieve_seconds_total", cc->sieve_seconds_total);
        cJSON_AddStringToObject(o, "current_workunit",   cc->current_workunit);
        cJSON_AddStringToObject(o, "class",              cc->last_class);
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
    send_json_take_close(c, 200, json);
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
    /* Protocol-mismatch shield: plain HTTP/1.1 only. MG_MAX_RECV_SIZE is
     * deliberately huge (512 MiB, to fit /submit bodies), so misdirected
     * clients — TLS handshakes (0x16 …), HTTP/2 prior-knowledge ("PRI …"),
     * SOCKS probes (0x04/0x05 …), random scanner bytes — would otherwise
     * sit on the connection waiting for an HTTP terminator that will never
     * arrive. Since every valid HTTP request line starts with an ASCII
     * uppercase method letter (GET, POST, …), reject anything else on the
     * very first byte. */
    if (ev == MG_EV_READ && c->recv.len >= 1) {
        unsigned char b = c->recv.buf[0];
        if (b < 'A' || b > 'Z') {
            c->is_closing = 1;
            return;
        }
    }

    if (ev != MG_EV_HTTP_MSG) return;
    struct mg_http_message *hm = (struct mg_http_message *)ev_data;
    server_ctx_t *ctx = (server_ctx_t *)c->fn_data;

    if (uri_eq(hm, "/lease") && method_is(hm, "POST")) {
        handle_lease(c, hm, ctx);
    } else if (uri_starts_with(hm, "/file/") && method_is(hm, "GET")) {
        handle_file(c, hm, ctx);
    } else if (uri_eq(hm, "/submit") && method_is(hm, "POST")) {
        handle_submit(c, hm, ctx);
    } else if (uri_eq(hm, "/renew") && method_is(hm, "POST")) {
        handle_renew(c, hm, ctx);
    } else if (uri_eq(hm, "/release") && method_is(hm, "POST")) {
        handle_release(c, hm, ctx);
    } else if (uri_eq(hm, "/health") && method_is(hm, "GET")) {
        handle_health(c, hm, ctx);
    } else if ((uri_eq(hm, "/stats") || uri_eq(hm, "/stats/")) &&
               method_is(hm, "GET")) {
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
    int64_t bblocks = 0, brequeued = 0, bpoisoned = 0;

    /* BLOCKS FIRST. This ordering is what keeps the per-workunit sweep off
     * block members without a predicate: the block sweep sets them back to
     * 'available', and db_lease_expire_sweep's own state = 'leased' test then
     * excludes them. Reverse the two and every member is incremented twice —
     * once as a stray leased row, once as a block member. */
    /* Log and carry on rather than returning: the row sweep is what reclaims
     * every ordinary CPU lease, and letting a transient block-sweep error (a
     * SQLITE_BUSY past the busy_timeout while the verifier drains) stop it
     * would stall the whole fleet's lease expiry behind a one-line message.
     * The ordering guarantee still holds — any block member this tick failed
     * to requeue is still 'leased' with a lapsed expiry, so the row sweep
     * charges it once, exactly as it would a stray workunit. */
    if (db_block_expire_sweep(ctx->db, now_unix(), ctx->max_attempts,
                              &bblocks, &brequeued, &bpoisoned) != 0)
        fprintf(stderr, "sweep: db_block_expire_sweep failed; "
                        "continuing with the per-workunit sweep\n");
    if (bblocks > 0) {
        fprintf(stderr, "sweep: blocks=%lld  members requeued=%lld  poisoned=%lld\n",
                (long long)bblocks, (long long)brequeued, (long long)bpoisoned);
    }

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
    const char *bind      = flag(argc, argv, "--bind");
    const char *port_s    = flag(argc, argv, "--port");
    const char *jobdir    = flag(argc, argv, "--jobdir");
    const char *lease_s   = flag(argc, argv, "--lease-seconds");
    const char *sweep_s   = flag(argc, argv, "--sweep-seconds");
    const char *attempt_s = flag(argc, argv, "--max-attempts");
    const char *spot_s    = flag(argc, argv, "--spotcheck-k");
    const char *bmult_s   = flag(argc, argv, "--block-width-multiple");
    const char *bmaxm_s   = flag(argc, argv, "--block-max-members");
    const char *bminw_s   = flag(argc, argv, "--block-min-q-width");
    const char *bceil_s   = flag(argc, argv, "--block-attempt-ceiling");

    int64_t port = 8080;
    if (port_s && *port_s && parse_int64_arg(port_s, &port) != 0) {
        fprintf(stderr, "serve: bad --port\n");
        return 2;
    }
    if (port < 1 || port > 65535) {
        fprintf(stderr, "serve: --port must be in 1..65535\n");
        return 2;
    }
    if (!bind || !*bind) bind = "127.0.0.1";
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

    int64_t spotcheck_k = 50;
    if (spot_s && *spot_s && parse_int64_arg(spot_s, &spotcheck_k) != 0) {
        fprintf(stderr, "serve: bad --spotcheck-k\n");
        return 2;
    }
    if (spotcheck_k < 0) spotcheck_k = 0;

    /* 50x base q_range. See the server_ctx_t comment for why the multiple is
     * what gets configured rather than an absolute width. Measured on a 5070
     * against 1,000-wide bands: 22% of every band is fixed cuda-sieve startup
     * at 1x, 1.4% at 20x, 0.6% at 50x. Past ~20x the throughput curve is flat
     * (+0.9% from 20 to 50); the reason to go further is fewer lease/submit
     * transactions on the single event-loop thread as the GPU fleet grows,
     * and the cost is reclaim granularity, since there is no partial submit. */
    int64_t block_width_multiple = 50;
    if (bmult_s && *bmult_s && parse_int64_arg(bmult_s, &block_width_multiple) != 0) {
        fprintf(stderr, "serve: bad --block-width-multiple\n");
        return 2;
    }
    if (block_width_multiple < 1) block_width_multiple = 1;

    int64_t block_max_members = 256;
    if (bmaxm_s && *bmaxm_s && parse_int64_arg(bmaxm_s, &block_max_members) != 0) {
        fprintf(stderr, "serve: bad --block-max-members\n");
        return 2;
    }
    if (block_max_members < 1) block_max_members = 1;

    int64_t block_min_q_width = 0;   /* 0 = target/4 */
    if (bminw_s && *bminw_s && parse_int64_arg(bminw_s, &block_min_q_width) != 0) {
        fprintf(stderr, "serve: bad --block-min-q-width\n");
        return 2;
    }
    if (block_min_q_width < 0) block_min_q_width = 0;

    int64_t block_attempt_ceiling = 2;
    if (bceil_s && *bceil_s && parse_int64_arg(bceil_s, &block_attempt_ceiling) != 0) {
        fprintf(stderr, "serve: bad --block-attempt-ceiling\n");
        return 2;
    }
    if (block_attempt_ceiling < 1) block_attempt_ceiling = 1;
    /* The ceiling is what guarantees a workunit can never be poisoned by
     * block-scale evidence alone: past it a range drops out of block
     * eligibility and can only take further strikes one individual lease at a
     * time. At or above --max-attempts that guarantee is gone and one flaky
     * host can poison a contiguous region in a handful of lease windows. */
    if (block_attempt_ceiling >= max_attempts) {
        int64_t capped = max_attempts > 1 ? max_attempts - 1 : 1;
        fprintf(stderr, "serve: --block-attempt-ceiling %lld >= --max-attempts "
                        "%lld; capping to %lld so block failures alone cannot "
                        "poison a workunit\n",
                (long long)block_attempt_ceiling, (long long)max_attempts,
                (long long)capped);
        block_attempt_ceiling = capped;
    }

    char *db_path = path_join(jobdir, "job.db");
    if (!db_path) return 1;

    server_ctx_t ctx = {0};
    ctx.block_width_multiple  = block_width_multiple;
    ctx.block_max_members     = block_max_members;
    ctx.block_min_q_width     = block_min_q_width;
    ctx.block_attempt_ceiling = block_attempt_ceiling;
    ctx.db = db_open(db_path);
    if (!ctx.db) { free(db_path); return 1; }

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
        free(db_path);
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

    /* siever_args is optional — jobs initialized before this flag existed
     * won't have the key, in which case clients get an empty string. */
    {
        char *m_args = db_meta_get(ctx.db, "siever_args");
        snprintf(ctx.siever_args, sizeof(ctx.siever_args), "%s",
                 m_args ? m_args : "");
        free(m_args);
    }
    /* gpu_args likewise — absent on any jobdir initialized before the cuda
     * engine, where "" is the right answer since it has no GPU band either. */
    {
        char *m_gargs = db_meta_get(ctx.db, "gpu_args");
        snprintf(ctx.gpu_args, sizeof(ctx.gpu_args), "%s",
                 m_gargs ? m_gargs : "");
        free(m_gargs);
    }

    /* lease_order is optional; absence (or unrecognized value) means ascending,
     * which preserves the historical behavior for jobs initialized before this
     * flag existed. */
    ctx.lease_desc = 0;
    {
        char *m_order = db_meta_get(ctx.db, "lease_order");
        if (m_order && strcmp(m_order, "desc") == 0) ctx.lease_desc = 1;
        free(m_order);
    }

    ctx.jobdir        = strdup(jobdir);
    ctx.rels_dir      = path_join(jobdir, "rels");
    ctx.lease_seconds = lease_seconds;
    ctx.sweep_seconds = sweep_seconds;
    ctx.max_attempts  = max_attempts;
    ctx.started_at    = now_unix();
    if (!ctx.jobdir || !ctx.rels_dir) {
        free(ctx.jobdir);
        free(ctx.rels_dir);
        free(db_path);
        db_close(ctx.db);
        return 1;
    }

    char listen_url[64];
    if (snprintf(listen_url, sizeof(listen_url), "http://%s:%lld",
                 bind, (long long)port) >= (int)sizeof(listen_url)) {
        fprintf(stderr, "serve: bind address too long\n");
        free(ctx.jobdir);
        free(ctx.rels_dir);
        free(db_path);
        db_close(ctx.db);
        return 2;
    }

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    mg_log_set(MG_LL_ERROR);  /* keep noise down; bump to MG_LL_INFO when debugging */

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

    /* Start the verifier. It opens its own SQLite connection against the same
     * file; correctness comes from WAL mode + busy_timeout (see db_open). On
     * startup it will drain any pending submissions left over from a prior
     * run before going to sleep on its condvar. */
    ctx.verifier = verify_thread_start(db_path, ctx.max_attempts, (int)spotcheck_k);
    if (!ctx.verifier) {
        fprintf(stderr, "serve: verifier failed to start — submissions will queue as 'pending'\n");
    }

    fprintf(stderr,
        "ggnfs-sieve-server: serving job %s on %s\n"
        "  jobdir       : %s\n"
        "  siever       : %s   side=%c   lease=%llds\n"
        "  siever_args  : %s\n"
        "  gpu_args     : %s\n"
        "  lease_order  : %s\n"
        "  sweep        : every %llds   max_attempts=%lld\n"
        "  job .job sha : %s\n"
        "  token        : %.8s... (read from <jobdir>/token)\n"
        "  dashboard    : %s/?token=%s\n",
        ctx.job_id, listen_url, ctx.jobdir, ctx.siever, ctx.side,
        (long long)ctx.lease_seconds,
        ctx.siever_args[0] ? ctx.siever_args : "(none)",
        ctx.gpu_args[0]    ? ctx.gpu_args    : "(none)",
        ctx.lease_desc ? "desc (highest q first)" : "asc (lowest q first)",
        (long long)sweep_seconds, (long long)ctx.max_attempts,
        ctx.job_sha256, ctx.token,
        listen_url, ctx.token);

    for (;;) mg_mgr_poll(&mgr, SERVER_POLL_MS);

    /* Unreachable today (no graceful shutdown), but tidy: */
    verify_thread_stop(ctx.verifier);
    mg_mgr_free(&mgr);
    free(ctx.jobdir); free(ctx.rels_dir);
    free(db_path);
    db_close(ctx.db);
    return 0;
}

/* ===================== verify-parse subcommand ========================= */

/* Diagnostic: stream a relation file through the parser and report counts.
 * Useful for smoke-testing the parser against real siever output without
 * standing up a full job. */
static int cmd_verify_parse(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: ggnfs-sieve-server verify-parse <relation-file>\n");
        return 2;
    }
    int64_t parsed = 0, failed = 0;
    if (verify_parse_file(argv[1], &parsed, &failed) != 0) return 1;
    printf("parsed=%lld failed=%lld\n",
           (long long)parsed, (long long)failed);
    return failed > 0 ? 1 : 0;
}

/* ===================== main ============================================= */

static void usage_top(void)
{
    fprintf(stderr,
        "ggnfs-sieve-server — Phase 1 walking skeleton\n"
        "usage:\n"
        "  ggnfs-sieve-server init         [args...]   create a new job\n"
        "  ggnfs-sieve-server extend       [args...]   add workunits to an existing job\n"
        "  ggnfs-sieve-server serve        [--bind=127.0.0.1] [--port=8080] [args...]\n"
        "  ggnfs-sieve-server verify-parse <file>      diagnostic: parse a relation file\n"
        "Run a subcommand without args to see its flags.\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage_top(); return 2; }
    if (strcmp(argv[1], "init")         == 0) return cmd_init        (argc - 1, argv + 1);
    if (strcmp(argv[1], "extend")       == 0) return cmd_extend      (argc - 1, argv + 1);
    if (strcmp(argv[1], "serve")        == 0) return cmd_serve       (argc - 1, argv + 1);
    if (strcmp(argv[1], "verify-parse") == 0) return cmd_verify_parse(argc - 1, argv + 1);
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage_top(); return 0;
    }
    usage_top();
    return 2;
}
