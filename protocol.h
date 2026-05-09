/* protocol.h — JSON encode/decode for the Phase 1 server endpoints.
 *
 * All encode_* functions return a malloc'd, null-terminated JSON string the
 * caller must free(). NULL on allocation failure.
 */
#ifndef GGNFS_SIEVE_PROTOCOL_H
#define GGNFS_SIEVE_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/* ---- /lease ---- */

typedef struct {
    const char *workunit_id;
    int64_t     q_start;
    int64_t     q_range;
    char        side;             /* 'a' or 'r' */
    int64_t     lease_seconds;
    const char *siever;           /* required gnfs-lasieve4* binary name */
    const char *command_template; /* "{siever} -f {q_start} -c ..." */
    /* Single-file MVP: one .job file shipped per workunit. */
    const char *file_name;
    const char *file_sha256_hex;
    const char *file_url;         /* "/file/<sha>" */
    const char *output_name;
    int64_t     output_max_bytes;
} proto_lease_response_args;

char *proto_encode_lease_response(const proto_lease_response_args *a);

/* Decode {"client_id": "...", "client_version": "..."}.
 * Each output buffer may be NULL to ignore that field.
 * Returns 0 on success, -1 on parse error. */
int proto_decode_lease_request(const char *body, size_t body_len,
                               char *client_id_buf,      size_t client_id_buf_n,
                               char *client_version_buf, size_t client_version_buf_n);

/* Client-side: decode a /lease success response (the JSON the server's
 * encoder above produces). MVP supports exactly one entry in `files`. */
typedef struct {
    char    workunit_id[64];
    int64_t q_start;
    int64_t q_range;
    char    side;
    int64_t lease_seconds;
    char    siever[64];
    char    command_template[256];
    char    file_name[64];
    char    file_sha256_hex[65];
    char    file_url[160];
    char    output_name[64];
    int64_t output_max_bytes;
} proto_lease_response_t;

int proto_decode_lease_response(const char *body, size_t body_len,
                                proto_lease_response_t *out);

/* Build the JSON request body for POST /lease. Caller free()s. */
char *proto_encode_lease_request(const char *client_id, const char *client_version);

/* ---- /submit ---- */

char *proto_encode_submit_response(int accepted,
                                   const char *verified_status,
                                   int64_t num_relations);

/* ---- /health ---- */

char *proto_encode_health_response(int ok, const char *job_id,
                                   int64_t uptime_seconds);

#endif /* GGNFS_SIEVE_PROTOCOL_H */
