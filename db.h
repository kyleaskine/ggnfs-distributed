/* db.h — SQLite layer for ggnfs-sieve-server.
 *
 * Connection model: each thread that touches SQLite opens its own
 * ggnfs_db_t. The main event loop owns one; the verifier thread owns
 * another. WAL mode + sqlite3_busy_timeout() (set in db_open) keep
 * cross-thread contention to brief stalls rather than failures. There
 * is no shared mutex; correctness comes from sqlite's own locking.
 */
#ifndef GGNFS_SIEVE_DB_H
#define GGNFS_SIEVE_DB_H

#include <stdint.h>

typedef struct ggnfs_db_s ggnfs_db_t;

/* Open or create `path`. Runs schema migrations on a fresh DB.
 * Returns NULL on failure (logs the cause to stderr). */
ggnfs_db_t *db_open(const char *path);
void       db_close(ggnfs_db_t *db);

/* ---- meta: simple string key/value store. ---- */

int   db_meta_set(ggnfs_db_t *db, const char *key, const char *value);
/* Returns malloc'd string the caller must free(); NULL if key absent. */
char *db_meta_get(ggnfs_db_t *db, const char *key);

/* ---- files: content-addressed input files for /file/<sha>. ---- */

int   db_files_insert(ggnfs_db_t *db, const char *sha256_hex,
                      const char *path, int64_t bytes, const char *purpose);
/* Returns malloc'd on-disk path the caller must free(); NULL if not found. */
char *db_files_path_for(ggnfs_db_t *db, const char *sha256_hex);

/* ---- workunits ---- */

int db_workunit_insert(ggnfs_db_t *db, const char *id,
                       int64_t q_start, int64_t q_range, char side,
                       int64_t now_unix);

/* Summarize what's already in the workunits table. `*out_count` receives the
 * total row count (used as the next sequence number for ID generation, since
 * IDs are assigned 0..N-1 by init/extend). `*out_q_end` receives the largest
 * q_start+q_range, i.e. one past the highest-Q workunit (0 if empty). */
int db_workunit_extent(ggnfs_db_t *db, int64_t *out_count, int64_t *out_q_end);

/* Re-queue any leased workunits whose lease has expired. If a workunit's
 * attempt_count would reach `max_attempts`, mark it 'poisoned' instead of
 * available so we stop re-issuing a workunit that keeps timing out.
 * Returns 0 on success; *out_requeued and *out_poisoned receive counts.
 * Either out pointer may be NULL. */
int db_lease_expire_sweep(ggnfs_db_t *db,
                          int64_t now_unix, int64_t max_attempts,
                          int64_t *out_requeued, int64_t *out_poisoned);

/* Atomically claim one available workunit for `client_id`.
 * On success (0) fills *out and transitions state available -> leased.
 * Returns 1 if no workunit is available, -1 on internal error. */
typedef struct {
    char     id[64];
    int64_t  q_start;
    int64_t  q_range;
    char     side;
} db_lease_result_t;

int db_lease(ggnfs_db_t *db, const char *client_id,
             int64_t lease_seconds, int64_t now_unix,
             db_lease_result_t *out);

/* Record a relation file submission and mark the workunit submitted.
 * New submissions land with verify_status='pending' for the verifier to pick
 * up. Returns 0 on success, 1 if the workunit is not currently leased
 * (caller should respond 409), -1 on internal error. */
int db_submit(ggnfs_db_t *db,
              const char *workunit_id, const char *client_id,
              const char *rel_file_path, const char *body_sha256_hex,
              int64_t num_relations, double sieve_seconds,
              int64_t now_unix);

/* Voluntarily release a current lease back to available. Only succeeds if
 * `workunit_id` is currently leased to `client_id`. Does not increment
 * attempt_count, because this is operator-requested shutdown rather than a
 * workunit failure. Returns 0 on release, 1 if no matching active lease exists,
 * -1 on internal error. */
int db_release_lease(ggnfs_db_t *db, const char *workunit_id,
                     const char *client_id);

/* Upsert a client's last_seen timestamp. */
int db_clients_seen(ggnfs_db_t *db, const char *client_id, int64_t now_unix);

/* ---- verifier API (called from the verifier thread) ----------------- */

typedef struct {
    int64_t  submission_id;
    char     workunit_id[64];
    int64_t  q_start;
    int64_t  q_range;
    char     side;
    int64_t  attempt_count;     /* pre-this-attempt; informational */
    char    *file_path;         /* malloc'd; caller frees via db_pending_free */
} db_pending_t;

void db_pending_free(db_pending_t *p);

/* Fetch the oldest pending submission joined with its workunit. Returns 0 on
 * success (out filled), 1 if nothing pending, -1 on internal error.
 * Read-only; doesn't transition state — the verifier resolves with pass/fail
 * after processing the file. */
int db_verify_next_pending(ggnfs_db_t *db, db_pending_t *out);

/* Parse pass: submission → 'passed' (with corrected num_relations), workunit
 * → 'verified', clients.total_relations += num_relations and total_workunits++.
 * One BEGIN IMMEDIATE transaction. Returns 0 on success, -1 on error. */
int db_verify_pass(ggnfs_db_t *db, int64_t submission_id,
                   int64_t num_relations_actual, int64_t now_unix);

/* Parse fail: submission → 'failed' with `reason`; workunit either back to
 * 'available' with attempt_count++ or 'poisoned' if the post-increment count
 * reaches max_attempts. Bumps clients.total_failures.
 * *out_poisoned (may be NULL) set to 1 if the workunit was poisoned.
 * Returns 0 on success, -1 on error. */
int db_verify_fail(ggnfs_db_t *db, int64_t submission_id,
                   const char *reason, int64_t max_attempts,
                   int64_t now_unix, int *out_poisoned);

/* ---- health/status ---- */

typedef struct {
    int64_t total;
    int64_t available;
    int64_t leased;
    int64_t submitted;
    int64_t verified;
    int64_t failed;
    int64_t poisoned;
} db_workunit_counts_t;

int db_workunit_counts(ggnfs_db_t *db, db_workunit_counts_t *out);

/* ---- stats snapshot for /stats and the dashboard --------------------- */

typedef struct {
    char    id[64];
    int64_t first_seen;
    int64_t last_seen;
    int64_t submissions;
    int64_t relations;
    double  avg_sieve_seconds;        /* over this client's submissions    */
    int64_t total_failures;
    char    current_workunit[64];     /* "" if no active lease            */
} db_stats_client_t;

typedef struct {
    /* workunits */
    db_workunit_counts_t  wu;
    int64_t  q_min;                   /* MIN(q_start) — 0 if no rows      */
    int64_t  q_max;                   /* MAX(q_start + q_range) — 0 ditto */

    /* submissions */
    int64_t  sub_total;
    int64_t  sub_relations;           /* SUM(num_relations)               */
    int64_t  sub_last_5m;
    int64_t  sub_last_1h;
    int64_t  sub_last_24h;
    int64_t  last_submit_unix;        /* 0 if no submissions yet          */
    double   avg_sieve_seconds;       /* across all submissions           */

    /* clients */
    int                 client_count;
    db_stats_client_t  *clients;      /* malloc'd; caller frees           */
} db_stats_t;

/* Fill a stats snapshot. Returns 0 on success, -1 on internal error.
 * Caller is responsible for free()ing out->clients via db_stats_free. */
int  db_stats_snapshot(ggnfs_db_t *db, int64_t now_unix, db_stats_t *out);
void db_stats_free(db_stats_t *out);

#endif /* GGNFS_SIEVE_DB_H */
