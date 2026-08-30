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
    const char *siever_args;      /* extra flags appended to the siever command (may be "") */
    const char *gpu_args;         /* cuda-sieve geometry/tuning flags (may be "") */
    /* Single-file MVP: one .job file shipped per workunit. */
    const char *file_name;
    const char *file_sha256_hex;
    const char *file_url;         /* "/file/<sha>" */
    const char *output_name;
    int64_t     output_max_bytes;
    /* >1 when this lease is a BLOCK: `workunit_id` is then the block's anchor
     * workunit and [q_start, q_start+q_range) spans all of its members. The
     * client sieves it exactly like an ordinary workunit — that is the point
     * of anchored addressing — so this is informational, for logging and for
     * deciding how to pace heartbeats. 0 or absent means an ordinary lease. */
    int64_t     block_members;
} proto_lease_response_args;

char *proto_encode_lease_response(const proto_lease_response_args *a);

/* The lease response carries BOTH engines' argument strings; the client picks
 * by its own --engine rather than the server deciding what it can run.
 * siever_args is gnfs-lasieve4 vocabulary ("-J 16") and gpu_args is
 * cuda-sieve's ("--logI 17 --J 16384"); they are not translations of each
 * other and describe different sieve areas. Either may be "".
 */

/* Decode {"client_id", "client_version", "class", "block", "block_max_members"}.
 * Each output may be NULL to ignore that field. "class" is absent on
 * pre-class clients and comes back empty, which callers read as "cpu".
 *
 * *want_block is 1 only if the client explicitly asked for a block, so every
 * client built before blocks existed keeps getting ordinary workunits with no
 * negotiation. *block_max_members is the client's requested cap and is advice
 * only — the server clamps it, since it flows into a LIMIT.
 * Returns 0 on success, -1 on parse error. */
int proto_decode_lease_request(const char *body, size_t body_len,
                               char *client_id_buf,      size_t client_id_buf_n,
                               char *client_version_buf, size_t client_version_buf_n,
                               char *class_buf,          size_t class_buf_n,
                               int *want_block, int64_t *block_max_members);

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
    char    siever_args[128];
    char    gpu_args[192];
    char    file_name[64];
    char    file_sha256_hex[65];
    char    file_url[160];
    char    output_name[64];
    int64_t output_max_bytes;
    int64_t block_members;   /* >1 = this is a block; see the encoder above */
} proto_lease_response_t;

int proto_decode_lease_response(const char *body, size_t body_len,
                                proto_lease_response_t *out);

/* Build the JSON request body for POST /lease. Caller free()s.
 * `class` is the workunit class this client wants ("cpu" / "gpu"); NULL
 * means "cpu". `want_block` asks for a block lease; `block_max_members` (0 =
 * let the server decide) caps how many workunits it may span. A server that
 * predates blocks ignores both and returns an ordinary lease. */
char *proto_encode_lease_request(const char *client_id, const char *client_version,
                                 const char *class, int want_block,
                                 int64_t block_max_members);

/* ---- /submit ---- */

char *proto_encode_submit_response(int accepted,
                                   const char *verified_status,
                                   int64_t num_relations);

/* ---- /renew ---- */
/* Request body is the same {workunit_id, client_id} shape as /release, so the
 * release encoder/decoder below is reused for it. */
char *proto_encode_renew_response(int accepted, int64_t lease_seconds);

/* ---- /release ---- */

char *proto_encode_release_request(const char *workunit_id, const char *client_id);
int proto_decode_release_request(const char *body, size_t body_len,
                                 char *workunit_id_buf, size_t workunit_id_buf_n,
                                 char *client_id_buf,   size_t client_id_buf_n);

/* ---- /health ---- */

char *proto_encode_health_response(int ok, const char *job_id,
                                   int64_t uptime_seconds);

#endif /* GGNFS_SIEVE_PROTOCOL_H */
