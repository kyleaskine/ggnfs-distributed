/*
 * nettest.c — minimal plain-HTTP bulk file transfer, the "ggnfs-sieve-client
 * upload" style: ONE TCP connection, NO TLS, Content-Length known up front,
 * body sent as a single bulk stream with no application-layer windowing.
 *
 * Difference from the real client: this streams from/to disk in 256 KB chunks,
 * so the file does NOT have to fit in RAM. Memory use is constant regardless of
 * file size. On the wire it is identical to one big write — TCP segments it the
 * same way — so it is a faithful test of the same transfer path.
 *
 * Standalone: not wired into the Makefile, no deps beyond libc.
 *
 *   Build:  cc -O2 -o nettest nettest.c
 *   Serve:  ./nettest serve --port=8080 --out=received.dat
 *   Send:   ./nettest send  --host=SERVER_IP --port=8080 --file=bigfile.dat
 *
 * Ctrl-C stops the server. It loops, accepting one transfer at a time.
 */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

#define CHUNK (256 * 1024)

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Write all n bytes to fd (socket or file), retrying short writes. 0 / -1. */
static int writen(int fd, const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void report(const char *what, long long bytes, double secs)
{
    double mb  = (double)bytes / 1e6;
    double mbps = secs > 0 ? mb / secs : 0.0;          /* megabytes/sec  */
    double mbit = secs > 0 ? (mb * 8.0) / secs : 0.0;  /* megabits/sec   */
    fprintf(stderr, "%s: %lld bytes in %.3fs  =  %.1f MB/s  (%.1f Mbit/s)\n",
            what, bytes, secs, mbps, mbit);
}

/* ------------------------------------------------------------------ send --- */

static int cmd_send(const char *host, const char *port, const char *file)
{
    struct stat st;
    if (stat(file, &st) != 0) {
        fprintf(stderr, "send: cannot stat %s: %s\n", file, strerror(errno));
        return 1;
    }
    long long total = (long long)st.st_size;

    int ffd = open(file, O_RDONLY);
    if (ffd < 0) {
        fprintf(stderr, "send: cannot open %s: %s\n", file, strerror(errno));
        return 1;
    }

    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int gai = getaddrinfo(host, port, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "send: resolve %s:%s failed: %s\n",
                host, port, gai_strerror(gai));
        close(ffd);
        return 1;
    }

    int sock = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) continue;
        if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    if (sock < 0) {
        fprintf(stderr, "send: connect to %s:%s failed: %s\n",
                host, port, strerror(errno));
        close(ffd);
        return 1;
    }

    double t0 = now_sec();

    /* Request line + headers: Content-Length known up front from stat(). */
    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "POST /upload HTTP/1.1\r\n"
        "Host: %s:%s\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %lld\r\n"
        "Connection: close\r\n"
        "\r\n",
        host, port, total);
    if (writen(sock, hdr, (size_t)hlen) != 0) {
        fprintf(stderr, "send: writing headers failed: %s\n", strerror(errno));
        close(sock); close(ffd); return 1;
    }

    /* Body: stream the file from disk, one bulk direction, one socket. */
    char *buf = malloc(CHUNK);
    if (!buf) { fprintf(stderr, "send: OOM\n"); close(sock); close(ffd); return 1; }
    long long sent = 0;
    for (;;) {
        ssize_t r = read(ffd, buf, CHUNK);
        if (r < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "send: read %s failed: %s\n", file, strerror(errno));
            free(buf); close(sock); close(ffd); return 1;
        }
        if (r == 0) break;
        if (writen(sock, buf, (size_t)r) != 0) {
            fprintf(stderr, "send: socket write failed at %lld/%lld: %s\n",
                    sent, total, strerror(errno));
            free(buf); close(sock); close(ffd); return 1;
        }
        sent += r;
    }
    free(buf);
    close(ffd);

    /* We're done writing; let the server finish and reply. */
    shutdown(sock, SHUT_WR);

    char resp[2048];
    ssize_t rn = recv(sock, resp, sizeof(resp) - 1, 0);
    double t1 = now_sec();
    if (rn > 0) {
        resp[rn] = 0;
        char *eol = strstr(resp, "\r\n");
        if (eol) *eol = 0;
        fprintf(stderr, "send: server said: %s\n", resp);
    } else {
        fprintf(stderr, "send: no response from server\n");
    }
    close(sock);

    report("send", sent, t1 - t0);
    return 0;
}

/* ----------------------------------------------------------------- serve --- */

static long long parse_content_length(const char *hdr, size_t header_len)
{
    /* Lowercase a copy of just the header region, then search. */
    char *lc = malloc(header_len + 1);
    if (!lc) return -1;
    for (size_t i = 0; i < header_len; i++) lc[i] = (char)tolower((unsigned char)hdr[i]);
    lc[header_len] = 0;

    long long cl = -1;
    char *p = strstr(lc, "content-length:");
    if (p) cl = strtoll(p + strlen("content-length:"), NULL, 10);
    free(lc);
    return cl;
}

static void handle_conn(int cfd, const char *outpath)
{
    /* Read the request headers (up to the blank line). Our client's headers
     * are tiny; 16 KB is plenty. We may pull some body bytes along with them. */
    char   hdr[16384];
    size_t hlen = 0;
    char  *body = NULL;
    for (;;) {
        if (hlen == sizeof(hdr)) {
            fprintf(stderr, "serve: request headers too large, dropping\n");
            return;
        }
        ssize_t r = recv(cfd, hdr + hlen, sizeof(hdr) - hlen, 0);
        if (r <= 0) { fprintf(stderr, "serve: connection closed before headers\n"); return; }
        hlen += (size_t)r;
        char *end = memmem(hdr, hlen, "\r\n\r\n", 4);
        if (end) { body = end + 4; break; }
    }

    size_t header_len       = (size_t)(body - hdr);
    size_t body_prefetched  = hlen - header_len;
    long long content_length = parse_content_length(hdr, header_len);
    if (content_length < 0) {
        fprintf(stderr, "serve: no Content-Length, dropping\n");
        return;
    }

    int ofd = open(outpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ofd < 0) {
        fprintf(stderr, "serve: cannot open %s: %s\n", outpath, strerror(errno));
        return;
    }

    double t0 = now_sec();
    long long got = 0;

    /* Body bytes that arrived in the same recv() as the headers. */
    if (body_prefetched > 0) {
        size_t take = body_prefetched;
        if ((long long)take > content_length) take = (size_t)content_length;
        if (writen(ofd, body, take) != 0) {
            fprintf(stderr, "serve: write %s failed: %s\n", outpath, strerror(errno));
            close(ofd); return;
        }
        got += take;
    }

    /* Stream the rest straight to disk — constant memory, any file size. */
    char *buf = malloc(CHUNK);
    if (!buf) { fprintf(stderr, "serve: OOM\n"); close(ofd); return; }
    while (got < content_length) {
        long long want = content_length - got;
        size_t chunk = want < CHUNK ? (size_t)want : CHUNK;
        ssize_t r = recv(cfd, buf, chunk, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "serve: recv failed at %lld/%lld: %s\n",
                    got, content_length, strerror(errno));
            break;
        }
        if (r == 0) {
            fprintf(stderr, "serve: connection closed early at %lld/%lld\n",
                    got, content_length);
            break;
        }
        if (writen(ofd, buf, (size_t)r) != 0) {
            fprintf(stderr, "serve: write %s failed: %s\n", outpath, strerror(errno));
            break;
        }
        got += r;
    }
    free(buf);
    fsync(ofd);
    close(ofd);

    double t1 = now_sec();

    const char *ok =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 3\r\n"
        "Connection: close\r\n"
        "\r\n"
        "OK\n";
    writen(cfd, ok, strlen(ok));

    if (got == content_length)
        report("serve recv", got, t1 - t0);
    else
        fprintf(stderr, "serve: INCOMPLETE %lld/%lld bytes\n", got, content_length);
}

static int cmd_serve(int port, const char *outpath)
{
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { perror("socket"); return 1; }

    int one = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((unsigned short)port);

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind"); close(sfd); return 1;
    }
    if (listen(sfd, 8) != 0) { perror("listen"); close(sfd); return 1; }

    fprintf(stderr, "serve: listening on 0.0.0.0:%d, writing to %s (Ctrl-C to stop)\n",
            port, outpath);

    for (;;) {
        struct sockaddr_in cli;
        socklen_t clilen = sizeof(cli);
        int cfd = accept(sfd, (struct sockaddr *)&cli, &clilen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
        fprintf(stderr, "serve: connection from %s\n", ip);
        handle_conn(cfd, outpath);
        close(cfd);
    }
    close(sfd);
    return 0;
}

/* ------------------------------------------------------------------ main --- */

static const char *opt(int argc, char **argv, const char *key, const char *def)
{
    size_t klen = strlen(key);
    for (int i = 2; i < argc; i++)
        if (strncmp(argv[i], key, klen) == 0 && argv[i][klen] == '=')
            return argv[i] + klen + 1;
    return def;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    if (argc < 2) {
        fprintf(stderr,
            "usage:\n"
            "  %s serve --port=8080 --out=received.dat\n"
            "  %s send  --host=HOST --port=8080 --file=PATH\n",
            argv[0], argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "serve") == 0) {
        int port          = atoi(opt(argc, argv, "--port", "8080"));
        const char *out   = opt(argc, argv, "--out", "received.dat");
        return cmd_serve(port, out);
    }
    if (strcmp(argv[1], "send") == 0) {
        const char *host  = opt(argc, argv, "--host", NULL);
        const char *port  = opt(argc, argv, "--port", "8080");
        const char *file  = opt(argc, argv, "--file", NULL);
        if (!host || !file) {
            fprintf(stderr, "send: --host and --file are required\n");
            return 2;
        }
        return cmd_send(host, port, file);
    }

    fprintf(stderr, "unknown subcommand: %s\n", argv[1]);
    return 2;
}
