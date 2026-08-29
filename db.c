#define _POSIX_C_SOURCE 200809L  /* strdup */

#include "db.h"
#include "vendor/sqlite3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ggnfs_db_s {
    sqlite3 *conn;
};

/* Schema with IF NOT EXISTS so opening an existing DB is a no-op. */
static const char SCHEMA_SQL[] =
    "PRAGMA journal_mode = WAL;"
    "PRAGMA synchronous  = NORMAL;"
    "CREATE TABLE IF NOT EXISTS workunits ("
    "  id            TEXT PRIMARY KEY,"
    "  q_start       INTEGER NOT NULL,"
    "  q_range       INTEGER NOT NULL,"
    "  side          TEXT NOT NULL CHECK (side IN ('a','r')),"
    "  class         TEXT NOT NULL DEFAULT 'cpu',"
    "  state         TEXT NOT NULL CHECK (state IN"
    "                  ('available','leased','submitted','verified','failed','poisoned')),"
    "  attempt_count INTEGER NOT NULL DEFAULT 0,"
    "  created_at    INTEGER NOT NULL,"
    "  leased_at     INTEGER,"
    "  leased_to     TEXT,"
    "  lease_expires INTEGER,"
    "  completed_at  INTEGER,"
    "  expected_rels INTEGER"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_wu_lease_expires"
    "    ON workunits(lease_expires) WHERE state = 'leased';"
    "CREATE TABLE IF NOT EXISTS submissions ("
    "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  workunit_id   TEXT NOT NULL REFERENCES workunits(id),"
    "  client_id     TEXT NOT NULL,"
    "  received_at   INTEGER NOT NULL,"
    "  file_path     TEXT NOT NULL,"
    "  sha256        TEXT NOT NULL,"
    "  num_relations INTEGER NOT NULL,"
    "  verify_status TEXT NOT NULL CHECK (verify_status IN"
    "                  ('pending','passed','failed','skipped')),"
    "  verify_reason TEXT,"
    "  sieve_seconds REAL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_sub_wu ON submissions(workunit_id);"
    "CREATE INDEX IF NOT EXISTS idx_sub_pending"
    "    ON submissions(verify_status) WHERE verify_status = 'pending';"
    "CREATE TABLE IF NOT EXISTS clients ("
    "  id              TEXT PRIMARY KEY,"
    "  first_seen      INTEGER NOT NULL,"
    "  last_seen       INTEGER NOT NULL,"
    "  total_relations INTEGER NOT NULL DEFAULT 0,"
    "  total_workunits INTEGER NOT NULL DEFAULT 0,"
    "  total_failures  INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE TABLE IF NOT EXISTS files ("
    "  sha256  TEXT PRIMARY KEY,"
    "  path    TEXT NOT NULL,"
    "  bytes   INTEGER NOT NULL,"
    "  purpose TEXT"
    ");"
    "CREATE TABLE IF NOT EXISTS meta ("
    "  key   TEXT PRIMARY KEY,"
    "  value TEXT"
    ");";

/* Runs after db_migrate has guaranteed every column below exists. Anything
 * naming a migrated column has to live here rather than in SCHEMA_SQL, which
 * executes against the pre-migration shape on a legacy jobdir.
 *
 * idx_wu_lease subsumes the old state-only index: `state` is still the
 * leading column, so the GROUP BY state counting queries use it unchanged,
 * while db_lease's `state = 'available' AND class = ? ORDER BY q_start` is
 * now covered end to end. q_range is the last column purely to make the
 * per-class q-width rollup in db_stats_snapshot covering — without it that
 * GROUP BY does a table lookup per row, and /stats runs on the same thread
 * as /lease and /submit every time the dashboard polls.
 *
 * idx_sub_received turns the "q-width verified in the last hour" query from a
 * full submissions scan into a range scan over one hour, which matters as
 * submissions grow into the hundreds of thousands. */
static const char SCHEMA_SQL_POST[] =
    "DROP INDEX IF EXISTS idx_wu_state;"
    "DROP INDEX IF EXISTS idx_wu_state_class;"
    "CREATE INDEX IF NOT EXISTS idx_wu_lease"
    "    ON workunits(state, class, q_start, q_range);"
    "CREATE INDEX IF NOT EXISTS idx_sub_received"
    "    ON submissions(received_at);";

static int exec_or_log(sqlite3 *conn, const char *sql, const char *what)
{
    char *err = NULL;
    int rc = sqlite3_exec(conn, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db: %s failed: %s\n", what, err ? err : "?");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

/* Does `table` already have a column named `column`? 1 yes, 0 no, -1 error. */
static int column_exists(sqlite3 *conn, const char *table, const char *column)
{
    sqlite3_stmt *st = NULL;
    char sql[128];
    /* PRAGMA won't take a bound parameter for the table name, but `table` is
     * always a literal from this file — never user input. */
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s);", table);
    if (sqlite3_prepare_v2(conn, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "db: table_info(%s): %s\n", table, sqlite3_errmsg(conn));
        return -1;
    }
    int found = 0, rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(st, 1);
        if (name && strcmp((const char *)name, column) == 0) { found = 1; break; }
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) return -1;
    return found;
}

/* Bring an existing jobdir's schema up to the current shape.
 *
 * SCHEMA_SQL creates tables with `CREATE TABLE IF NOT EXISTS`, so a jobdir
 * initialized by an older build keeps whatever columns it was created with —
 * new ones in SCHEMA_SQL are silently ignored there. Every column added after
 * the initial release therefore needs a matching step here.
 *
 * Each step must be idempotent: db_open runs this on every open, including
 * the verifier thread's second connection. */
static int db_migrate(sqlite3 *conn)
{
    /* workunits.class — 'cpu' (a gnfs-lasieve4-sized q_range) or 'gpu' (a
     * much wider one). Pre-existing rows are all CPU-sized by definition, so
     * the DEFAULT backfills them correctly. */
    int has = column_exists(conn, "workunits", "class");
    if (has < 0) return -1;
    if (!has) {
        if (exec_or_log(conn,
                "ALTER TABLE workunits ADD COLUMN class TEXT NOT NULL"
                "     DEFAULT 'cpu';",
                "migrate: add workunits.class") != 0)
            return -1;
        fprintf(stderr, "db: migrated schema — added workunits.class"
                        " (existing rows default to 'cpu')\n");
    }
    return 0;
}

ggnfs_db_t *db_open(const char *path)
{
    sqlite3 *conn = NULL;
    if (sqlite3_open(path, &conn) != SQLITE_OK) {
        fprintf(stderr, "db: cannot open %s: %s\n", path, sqlite3_errmsg(conn));
        sqlite3_close(conn);
        return NULL;
    }
    if (exec_or_log(conn, SCHEMA_SQL, "schema init") != 0) {
        sqlite3_close(conn);
        return NULL;
    }
    if (db_migrate(conn) != 0) {
        sqlite3_close(conn);
        return NULL;
    }
    if (exec_or_log(conn, SCHEMA_SQL_POST, "schema init (post-migrate)") != 0) {
        sqlite3_close(conn);
        return NULL;
    }
    /* Best-effort; don't fail open if FK enforcement can't be turned on. */
    (void)exec_or_log(conn, "PRAGMA foreign_keys = ON;", "enable foreign_keys");

    /* Two connections (main event loop + verifier thread) will share this file
     * once the verifier lands. WAL mode allows readers + one writer; busy_timeout
     * resolves the brief contention window when both try to commit at once.
     * 5s is long enough to ride out the other side's BEGIN IMMEDIATE / UPDATE /
     * COMMIT but short enough that a real deadlock is still loud. */
    sqlite3_busy_timeout(conn, 5000);

    ggnfs_db_t *db = calloc(1, sizeof(*db));
    if (!db) { sqlite3_close(conn); return NULL; }
    db->conn = conn;
    return db;
}

void db_close(ggnfs_db_t *db)
{
    if (!db) return;
    if (db->conn) sqlite3_close(db->conn);
    free(db);
}

/* ---- meta ------------------------------------------------------------- */

int db_meta_set(ggnfs_db_t *db, const char *key, const char *value)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->conn,
            "INSERT INTO meta(key, value) VALUES (?1, ?2) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value;",
            -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_meta_set: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }
    sqlite3_bind_text(st, 1, key,   -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, value, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

char *db_meta_get(ggnfs_db_t *db, const char *key)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->conn,
            "SELECT value FROM meta WHERE key = ?1;",
            -1, &st, NULL) != SQLITE_OK) {
        return NULL;
    }
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *v = sqlite3_column_text(st, 0);
        if (v) out = strdup((const char *)v);
    }
    sqlite3_finalize(st);
    return out;
}

/* ---- files ------------------------------------------------------------ */

int db_files_insert(ggnfs_db_t *db, const char *sha256_hex,
                    const char *path, int64_t bytes, const char *purpose)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->conn,
            "INSERT INTO files(sha256, path, bytes, purpose) "
            "VALUES (?1, ?2, ?3, ?4) "
            "ON CONFLICT(sha256) DO UPDATE SET "
            "  path = excluded.path, bytes = excluded.bytes, purpose = excluded.purpose;",
            -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_files_insert: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }
    sqlite3_bind_text (st, 1, sha256_hex, -1, SQLITE_STATIC);
    sqlite3_bind_text (st, 2, path,       -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, bytes);
    if (purpose) sqlite3_bind_text(st, 4, purpose, -1, SQLITE_STATIC);
    else         sqlite3_bind_null(st, 4);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

char *db_files_path_for(ggnfs_db_t *db, const char *sha256_hex)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->conn,
            "SELECT path FROM files WHERE sha256 = ?1;",
            -1, &st, NULL) != SQLITE_OK) {
        return NULL;
    }
    sqlite3_bind_text(st, 1, sha256_hex, -1, SQLITE_STATIC);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *v = sqlite3_column_text(st, 0);
        if (v) out = strdup((const char *)v);
    }
    sqlite3_finalize(st);
    return out;
}

/* ---- workunits -------------------------------------------------------- */

/* Fill *out from a lease statement's RETURNING row: id, q_start, q_range,
 * side, class in that column order. */
static void lease_result_from_row(sqlite3_stmt *st, db_lease_result_t *out)
{
    const unsigned char *id    = sqlite3_column_text (st, 0);
    const unsigned char *side  = sqlite3_column_text (st, 3);
    const unsigned char *class = sqlite3_column_text (st, 4);
    snprintf(out->id,    sizeof(out->id),    "%s", id    ? (const char *)id    : "");
    snprintf(out->class, sizeof(out->class), "%s", class ? (const char *)class : "cpu");
    out->q_start = sqlite3_column_int64(st, 1);
    out->q_range = sqlite3_column_int64(st, 2);
    out->side    = side ? (char)side[0] : '?';
}

int db_workunit_insert(ggnfs_db_t *db, const char *id,
                       int64_t q_start, int64_t q_range, char side,
                       const char *class, int64_t now_unix)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->conn,
            "INSERT INTO workunits(id, q_start, q_range, side, class, state, attempt_count, created_at) "
            "VALUES (?1, ?2, ?3, ?4, ?5, 'available', 0, ?6);",
            -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_workunit_insert: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }
    char side_str[2] = { side, 0 };
    sqlite3_bind_text  (st, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_int64 (st, 2, q_start);
    sqlite3_bind_int64 (st, 3, q_range);
    sqlite3_bind_text  (st, 4, side_str, 1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (st, 5, class && *class ? class : "cpu", -1, SQLITE_STATIC);
    sqlite3_bind_int64 (st, 6, now_unix);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_workunit_extent(ggnfs_db_t *db, int64_t *out_count, int64_t *out_q_end)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->conn,
            "SELECT COUNT(*), COALESCE(MAX(q_start + q_range), 0) "
            "FROM workunits;",
            -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_workunit_extent: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        if (out_count) *out_count = sqlite3_column_int64(st, 0);
        if (out_q_end) *out_q_end = sqlite3_column_int64(st, 1);
        sqlite3_finalize(st);
        return 0;
    }
    sqlite3_finalize(st);
    return -1;
}

int db_workunit_get(ggnfs_db_t *db, const char *id, db_lease_result_t *out)
{
    if (!id || !out) return -1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->conn,
            "SELECT id, q_start, q_range, side, class FROM workunits WHERE id = ?1;",
            -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_workunit_get: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    int result;
    if (rc == SQLITE_ROW) {
        /* Same column order the lease statements return. */
        lease_result_from_row(st, out);
        result = 0;
    } else if (rc == SQLITE_DONE) {
        result = 1;
    } else {
        fprintf(stderr, "db_workunit_get: %s\n", sqlite3_errmsg(db->conn));
        result = -1;
    }
    sqlite3_finalize(st);
    return result;
}

/* [a,b) and [c,d) overlap iff a < d AND c < b. Here [qmin,qmax) is the new
 * range and [q_start, q_start+q_range) is the existing workunit row. */
int db_workunit_overlap(ggnfs_db_t *db, int64_t qmin, int64_t qmax,
                        int64_t *out_q_start, int64_t *out_q_range)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->conn,
            "SELECT q_start, q_range FROM workunits "
            "WHERE q_start < ?1 AND (q_start + q_range) > ?2 "
            "ORDER BY q_start LIMIT 1;",
            -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_workunit_overlap: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }
    sqlite3_bind_int64(st, 1, qmax);
    sqlite3_bind_int64(st, 2, qmin);
    int rc = sqlite3_step(st);
    int result = -1;
    if (rc == SQLITE_ROW) {
        if (out_q_start) *out_q_start = sqlite3_column_int64(st, 0);
        if (out_q_range) *out_q_range = sqlite3_column_int64(st, 1);
        result = 1;
    } else if (rc == SQLITE_DONE) {
        result = 0;
    } else {
        fprintf(stderr, "db_workunit_overlap: %s\n", sqlite3_errmsg(db->conn));
    }
    sqlite3_finalize(st);
    return result;
}

int db_lease_expire_sweep(ggnfs_db_t *db,
                          int64_t now_unix, int64_t max_attempts,
                          int64_t *out_requeued, int64_t *out_poisoned)
{
    if (out_requeued) *out_requeued = 0;
    if (out_poisoned) *out_poisoned = 0;

    /* CASE inside SET decides 'poisoned' vs 'available' based on the
     * post-increment attempt_count. RETURNING tells us which bucket each
     * row landed in so we can log a useful summary. */
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->conn,
            "UPDATE workunits "
            "  SET attempt_count = attempt_count + 1,"
            "      state = CASE WHEN attempt_count + 1 >= ?2 "
            "                   THEN 'poisoned' ELSE 'available' END,"
            "      leased_at = NULL,"
            "      leased_to = NULL,"
            "      lease_expires = NULL "
            "  WHERE state = 'leased' AND lease_expires < ?1 "
            "RETURNING state;",
            -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_lease_expire_sweep: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }
    sqlite3_bind_int64(st, 1, now_unix);
    sqlite3_bind_int64(st, 2, max_attempts);

    int64_t requeued = 0, poisoned = 0;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const unsigned char *s = sqlite3_column_text(st, 0);
        if (s && strcmp((const char *)s, "poisoned") == 0) poisoned++;
        else requeued++;
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return -1;

    if (out_requeued) *out_requeued = requeued;
    if (out_poisoned) *out_poisoned = poisoned;
    return 0;
}

/* Try to claim one available workunit of exactly `class`. Returns 0 on claim
 * (out filled), 1 if that class has nothing available, -1 on error. */
static int lease_try_class(ggnfs_db_t *db, const char *client_id,
                           int64_t lease_seconds, int64_t now_unix,
                           int lease_desc, const char *class,
                           db_lease_result_t *out)
{
    /* Atomic claim: pick the next available row, flip it to 'leased', return
     * the columns the caller needs. SQLite RETURNING (3.35+) gives us the
     * post-update row in a single statement. `lease_desc` selects the q_start
     * ordering: ascending (default) or descending (useful when neighboring
     * sieve work is going up from a higher q, so we want to close the gap
     * downward and avoid stranding a small leftover band). */
    static const char *const sql_asc =
            "UPDATE workunits "
            "  SET state = 'leased',"
            "      leased_at = ?1,"
            "      leased_to = ?2,"
            "      lease_expires = ?3 "
            "  WHERE id = (SELECT id FROM workunits "
            "              WHERE state = 'available' AND class = ?4 "
            "              ORDER BY q_start ASC LIMIT 1) "
            "RETURNING id, q_start, q_range, side, class;";
    static const char *const sql_desc =
            "UPDATE workunits "
            "  SET state = 'leased',"
            "      leased_at = ?1,"
            "      leased_to = ?2,"
            "      lease_expires = ?3 "
            "  WHERE id = (SELECT id FROM workunits "
            "              WHERE state = 'available' AND class = ?4 "
            "              ORDER BY q_start DESC LIMIT 1) "
            "RETURNING id, q_start, q_range, side, class;";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->conn, lease_desc ? sql_desc : sql_asc,
                           -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_lease: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }
    sqlite3_bind_int64(st, 1, now_unix);
    sqlite3_bind_text (st, 2, client_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, now_unix + lease_seconds);
    sqlite3_bind_text (st, 4, class, -1, SQLITE_STATIC);

    int result;
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        lease_result_from_row(st, out);
        result = 0;
    } else if (rc == SQLITE_DONE) {
        result = 1; /* nothing available in this class */
    } else {
        fprintf(stderr, "db_lease: step: %s\n", sqlite3_errmsg(db->conn));
        result = -1;
    }
    sqlite3_finalize(st);
    return result;
}

int db_lease(ggnfs_db_t *db, const char *client_id,
             int64_t lease_seconds, int64_t now_unix,
             int lease_desc, const char *class_want,
             db_lease_result_t *out)
{
    memset(out, 0, sizeof(*out));

    /* Idempotency guard: a worker client-id is expected to hold at most one
     * active lease. If a prior /lease response was lost and the worker retries,
     * renew and return the same workunit instead of accumulating another
     * leased row. */
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->conn,
            "UPDATE workunits "
            "   SET lease_expires = ?3 "
            " WHERE id = (SELECT id FROM workunits "
            "              WHERE state = 'leased' "
            "                AND leased_to = ?1 "
            "                AND lease_expires >= ?2 "
            "              ORDER BY leased_at DESC, id DESC LIMIT 1) "
            "RETURNING id, q_start, q_range, side, class;",
            -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_lease existing: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }
    sqlite3_bind_text (st, 1, client_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, now_unix);
    sqlite3_bind_int64(st, 3, now_unix + lease_seconds);

    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        /* Deliberately class-agnostic: whatever this client already holds is
         * what it gets back, even if it is now asking for a different class.
         * Handing out a second workunit here is the failure this guard
         * exists to prevent. */
        lease_result_from_row(st, out);
        sqlite3_finalize(st);
        return 0;
    }
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_lease existing step: %s\n", sqlite3_errmsg(db->conn));
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    st = NULL;

    /* Class fallback. A GPU-class workunit is ~100x wider than a CPU-class
     * one, so the two directions are not symmetric:
     *
     *   gpu -> cpu   fine. The card sieves a small band inefficiently —
     *                it just pays lease/startup overhead more often.
     *   cpu -> gpu   never. A single core needs ~43 h for a 100x band; the
     *                lease expires, attempt_count climbs, and the workunit
     *                is eventually poisoned having never been sieved.
     *
     * Any other class is tried alone: a future class must opt into a
     * fallback explicitly rather than silently inheriting cpu's work. */
    static const char *const chain_gpu[] = { "gpu", "cpu", NULL };
    const char *chain_self[2];
    const char *const *chain;

    if (!class_want || !*class_want) class_want = "cpu";
    if (strcmp(class_want, "gpu") == 0) {
        chain = chain_gpu;
    } else {
        chain_self[0] = class_want;
        chain_self[1] = NULL;
        chain = chain_self;
    }

    for (int i = 0; chain[i]; i++) {
        int r = lease_try_class(db, client_id, lease_seconds, now_unix,
                                lease_desc, chain[i], out);
        if (r != 1) return r;   /* claimed (0) or hard error (-1) */
    }
    return 1;   /* nothing available in any class this client accepts */
}

int db_submit(ggnfs_db_t *db,
              const char *workunit_id, const char *client_id,
              const char *rel_file_path, const char *body_sha256_hex,
              int64_t num_relations, double sieve_seconds,
              int64_t now_unix)
{
    if (sqlite3_exec(db->conn, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK)
        return -1;

    int result = -1;
    sqlite3_stmt *up = NULL, *ins = NULL;

    if (sqlite3_prepare_v2(db->conn,
            "UPDATE workunits SET state = 'submitted', completed_at = ?1 "
            "  WHERE id = ?2 AND state = 'leased';",
            -1, &up, NULL) != SQLITE_OK)
        goto done;
    sqlite3_bind_int64(up, 1, now_unix);
    sqlite3_bind_text (up, 2, workunit_id, -1, SQLITE_STATIC);
    if (sqlite3_step(up) != SQLITE_DONE) goto done;

    if (sqlite3_changes(db->conn) == 0) {
        result = 1; /* not in 'leased' state — caller should respond 409 */
        goto done;
    }

    if (sqlite3_prepare_v2(db->conn,
            "INSERT INTO submissions("
            "  workunit_id, client_id, received_at, file_path, sha256,"
            "  num_relations, verify_status, sieve_seconds"
            ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, 'pending', ?7);",
            -1, &ins, NULL) != SQLITE_OK)
        goto done;
    sqlite3_bind_text  (ins, 1, workunit_id,     -1, SQLITE_STATIC);
    sqlite3_bind_text  (ins, 2, client_id,       -1, SQLITE_STATIC);
    sqlite3_bind_int64 (ins, 3, now_unix);
    sqlite3_bind_text  (ins, 4, rel_file_path,   -1, SQLITE_STATIC);
    sqlite3_bind_text  (ins, 5, body_sha256_hex, -1, SQLITE_STATIC);
    sqlite3_bind_int64 (ins, 6, num_relations);
    sqlite3_bind_double(ins, 7, sieve_seconds);
    if (sqlite3_step(ins) != SQLITE_DONE) goto done;

    result = 0;

done:
    if (up)  sqlite3_finalize(up);
    if (ins) sqlite3_finalize(ins);
    if (result == 0) {
        sqlite3_exec(db->conn, "COMMIT;",   NULL, NULL, NULL);
    } else {
        if (result == -1)
            fprintf(stderr, "db_submit: %s\n", sqlite3_errmsg(db->conn));
        sqlite3_exec(db->conn, "ROLLBACK;", NULL, NULL, NULL);
    }
    return result;
}

int db_renew_lease(ggnfs_db_t *db, const char *workunit_id,
                   const char *client_id, int64_t lease_seconds,
                   int64_t now_unix)
{
    sqlite3_stmt *st = NULL;
    /* `lease_expires >= now` is deliberate: once a lease has actually lapsed
     * the sweep may already have requeued and reissued the workunit to
     * somebody else, so a late heartbeat must not resurrect this client's
     * claim on it. The client sees 409 and gives up the workunit. */
    if (sqlite3_prepare_v2(db->conn,
            "UPDATE workunits "
            "  SET lease_expires = ?3 "
            "  WHERE id = ?1 AND state = 'leased' AND leased_to = ?2 "
            "    AND lease_expires >= ?4;",
            -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_renew_lease: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }
    sqlite3_bind_text (st, 1, workunit_id, -1, SQLITE_STATIC);
    sqlite3_bind_text (st, 2, client_id,   -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, now_unix + lease_seconds);
    sqlite3_bind_int64(st, 4, now_unix);
    int rc = sqlite3_step(st);
    int changed = sqlite3_changes(db->conn);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_renew_lease: step: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }
    return changed > 0 ? 0 : 1;
}

int db_release_lease(ggnfs_db_t *db, const char *workunit_id,
                     const char *client_id)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->conn,
            "UPDATE workunits "
            "  SET state = 'available',"
            "      leased_at = NULL,"
            "      leased_to = NULL,"
            "      lease_expires = NULL "
            "  WHERE id = ?1 AND state = 'leased' AND leased_to = ?2;",
            -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_release_lease: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }
    sqlite3_bind_text(st, 1, workunit_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, client_id,   -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    int changed = sqlite3_changes(db->conn);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_release_lease: step: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }
    return changed > 0 ? 0 : 1;
}

int db_clients_seen(ggnfs_db_t *db, const char *client_id, int64_t now_unix)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->conn,
            "INSERT INTO clients(id, first_seen, last_seen) VALUES (?1, ?2, ?2) "
            "ON CONFLICT(id) DO UPDATE SET last_seen = ?2;",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text (st, 1, client_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, now_unix);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* ---- verifier --------------------------------------------------------- */

void db_pending_free(db_pending_t *p)
{
    if (!p) return;
    free(p->file_path);
    p->file_path = NULL;
}

int db_verify_next_pending(ggnfs_db_t *db, db_pending_t *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->conn,
            "SELECT s.id, s.workunit_id, s.file_path,"
            "       w.q_start, w.q_range, w.side, w.attempt_count "
            "FROM submissions s JOIN workunits w ON w.id = s.workunit_id "
            "WHERE s.verify_status = 'pending' "
            "ORDER BY s.id LIMIT 1;",
            -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_verify_next_pending: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }

    int result = 1; /* nothing pending */
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        out->submission_id = sqlite3_column_int64(st, 0);
        const unsigned char *wuid = sqlite3_column_text(st, 1);
        snprintf(out->workunit_id, sizeof(out->workunit_id), "%s",
                 wuid ? (const char *)wuid : "");
        const unsigned char *fp = sqlite3_column_text(st, 2);
        out->file_path = fp ? strdup((const char *)fp) : NULL;
        out->q_start = sqlite3_column_int64(st, 3);
        out->q_range = sqlite3_column_int64(st, 4);
        const unsigned char *side = sqlite3_column_text(st, 5);
        out->side = side ? (char)side[0] : '?';
        out->attempt_count = sqlite3_column_int64(st, 6);
        result = 0;
    } else if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_verify_next_pending: step: %s\n", sqlite3_errmsg(db->conn));
        result = -1;
    }
    sqlite3_finalize(st);
    return result;
}

int db_verify_pass(ggnfs_db_t *db, int64_t submission_id,
                   int64_t num_relations_actual, int64_t now_unix)
{
    (void)now_unix;  /* completed_at on workunit was set at submit time */

    if (sqlite3_exec(db->conn, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_verify_pass: begin: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }

    int result = -1;
    sqlite3_stmt *st = NULL;

    /* Update the submission row. RETURNING gives us workunit_id and client_id
     * so we can roll the rest of the transition off the same statement.
     * Guard with the current state so a duplicate verifier run is a no-op. */
    if (sqlite3_prepare_v2(db->conn,
            "UPDATE submissions "
            "  SET verify_status = 'passed',"
            "      verify_reason = NULL,"
            "      num_relations = ?1 "
            "  WHERE id = ?2 AND verify_status = 'pending' "
            "RETURNING workunit_id, client_id;",
            -1, &st, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_int64(st, 1, num_relations_actual);
    sqlite3_bind_int64(st, 2, submission_id);

    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        /* No row matched — already resolved or bad id. Treat as success so the
         * caller stops retrying; nothing further to do. */
        result = (rc == SQLITE_DONE) ? 0 : -1;
        goto done;
    }
    const unsigned char *wuid = sqlite3_column_text(st, 0);
    const unsigned char *cid  = sqlite3_column_text(st, 1);
    char wu_buf[64]; char client_buf[128];
    snprintf(wu_buf,     sizeof(wu_buf),     "%s", wuid ? (const char *)wuid : "");
    snprintf(client_buf, sizeof(client_buf), "%s", cid  ? (const char *)cid  : "");
    sqlite3_finalize(st); st = NULL;

    /* Workunit → verified. */
    if (sqlite3_prepare_v2(db->conn,
            "UPDATE workunits SET state = 'verified' WHERE id = ?1;",
            -1, &st, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_text(st, 1, wu_buf, -1, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_DONE) goto done;
    sqlite3_finalize(st); st = NULL;

    /* clients.total_relations += N; total_workunits += 1. The submitting
     * client is whoever the submission row recorded; we trust that here. */
    if (sqlite3_prepare_v2(db->conn,
            "UPDATE clients "
            "  SET total_relations = total_relations + ?1,"
            "      total_workunits = total_workunits + 1 "
            "  WHERE id = ?2;",
            -1, &st, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_int64(st, 1, num_relations_actual);
    sqlite3_bind_text (st, 2, client_buf, -1, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_DONE) goto done;

    result = 0;

done:
    if (st) sqlite3_finalize(st);
    if (result == 0) {
        sqlite3_exec(db->conn, "COMMIT;",   NULL, NULL, NULL);
    } else {
        fprintf(stderr, "db_verify_pass: %s\n", sqlite3_errmsg(db->conn));
        sqlite3_exec(db->conn, "ROLLBACK;", NULL, NULL, NULL);
    }
    return result;
}

int db_verify_fail(ggnfs_db_t *db, int64_t submission_id,
                   const char *reason, int64_t max_attempts,
                   int64_t now_unix, int *out_poisoned)
{
    (void)now_unix;
    if (out_poisoned) *out_poisoned = 0;

    if (sqlite3_exec(db->conn, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_verify_fail: begin: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }

    int result = -1;
    sqlite3_stmt *st = NULL;

    /* Flip submission to failed, capture workunit + client for the rest. */
    if (sqlite3_prepare_v2(db->conn,
            "UPDATE submissions "
            "  SET verify_status = 'failed',"
            "      verify_reason = ?1 "
            "  WHERE id = ?2 AND verify_status = 'pending' "
            "RETURNING workunit_id, client_id;",
            -1, &st, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_text (st, 1, reason ? reason : "unspecified", -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, submission_id);

    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        result = (rc == SQLITE_DONE) ? 0 : -1;
        goto done;
    }
    const unsigned char *wuid = sqlite3_column_text(st, 0);
    const unsigned char *cid  = sqlite3_column_text(st, 1);
    char wu_buf[64]; char client_buf[128];
    snprintf(wu_buf,     sizeof(wu_buf),     "%s", wuid ? (const char *)wuid : "");
    snprintf(client_buf, sizeof(client_buf), "%s", cid  ? (const char *)cid  : "");
    sqlite3_finalize(st); st = NULL;

    /* Workunit: attempt_count++ then either available (retry) or poisoned.
     * Mirrors the CASE pattern in db_lease_expire_sweep so the two paths
     * to retire a workunit stay consistent. */
    if (sqlite3_prepare_v2(db->conn,
            "UPDATE workunits "
            "  SET attempt_count = attempt_count + 1,"
            "      state = CASE WHEN attempt_count + 1 >= ?1"
            "                   THEN 'poisoned' ELSE 'available' END,"
            "      leased_at = NULL,"
            "      leased_to = NULL,"
            "      lease_expires = NULL "
            "  WHERE id = ?2 "
            "RETURNING state;",
            -1, &st, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_int64(st, 1, max_attempts);
    sqlite3_bind_text (st, 2, wu_buf, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) goto done;
    {
        const unsigned char *new_state = sqlite3_column_text(st, 0);
        if (out_poisoned && new_state && strcmp((const char *)new_state, "poisoned") == 0)
            *out_poisoned = 1;
    }
    sqlite3_finalize(st); st = NULL;

    /* clients.total_failures++ */
    if (sqlite3_prepare_v2(db->conn,
            "UPDATE clients SET total_failures = total_failures + 1 WHERE id = ?1;",
            -1, &st, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_text(st, 1, client_buf, -1, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_DONE) goto done;

    result = 0;

done:
    if (st) sqlite3_finalize(st);
    if (result == 0) {
        sqlite3_exec(db->conn, "COMMIT;",   NULL, NULL, NULL);
    } else {
        fprintf(stderr, "db_verify_fail: %s\n", sqlite3_errmsg(db->conn));
        sqlite3_exec(db->conn, "ROLLBACK;", NULL, NULL, NULL);
    }
    return result;
}

/* ---- stats snapshot --------------------------------------------------- */

int db_stats_snapshot(ggnfs_db_t *db, int64_t now_unix, db_stats_t *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    /* Workunit counts (reuses existing helper). */
    if (db_workunit_counts(db, &out->wu) != 0) return -1;

    /* Q-range covered. */
    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db->conn,
                "SELECT COALESCE(MIN(q_start), 0), "
                "       COALESCE(MAX(q_start + q_range), 0) "
                "FROM workunits;",
                -1, &st, NULL) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) {
                out->q_min = sqlite3_column_int64(st, 0);
                out->q_max = sqlite3_column_int64(st, 1);
            }
            sqlite3_finalize(st);
        }
    }

    /* Q-width progress, per class and in total. One GROUP BY class,state scan
     * feeds both: workunit counts answer "how many units", q_range sums answer
     * "how much of the campaign", and only the latter is comparable across a
     * cpu band and a gpu band 100x wider. */
    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db->conn,
                "SELECT class, state, COUNT(*), COALESCE(SUM(q_range), 0) "
                "FROM workunits GROUP BY class, state;",
                -1, &st, NULL) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                const unsigned char *cls = sqlite3_column_text(st, 0);
                const unsigned char *stt = sqlite3_column_text(st, 1);
                int64_t n  = sqlite3_column_int64(st, 2);
                int64_t qw = sqlite3_column_int64(st, 3);
                const char *cname = cls ? (const char *)cls : "cpu";
                const char *sname = stt ? (const char *)stt : "";

                out->q_total += qw;
                if (strcmp(sname, "verified") == 0) out->q_verified += qw;

                /* Find or append this class's row. A jobdir should only ever
                 * hold the classes init/extend accept, so overflowing
                 * DB_STATS_MAX_CLASSES means someone hand-edited the DB;
                 * fold the stragglers into the totals and skip the row
                 * rather than scribbling past the array. */
                db_stats_class_t *cc = NULL;
                for (int i = 0; i < out->class_count; i++) {
                    if (strcmp(out->classes[i].name, cname) == 0) {
                        cc = &out->classes[i];
                        break;
                    }
                }
                if (!cc) {
                    if (out->class_count >= DB_STATS_MAX_CLASSES) continue;
                    cc = &out->classes[out->class_count++];
                    memset(cc, 0, sizeof(*cc));
                    snprintf(cc->name, sizeof(cc->name), "%s", cname);
                }
                cc->total   += n;
                cc->q_total += qw;
                if      (strcmp(sname, "available") == 0) cc->available += n;
                else if (strcmp(sname, "leased")    == 0) cc->leased    += n;
                else if (strcmp(sname, "verified")  == 0) {
                    cc->verified   += n;
                    cc->q_verified += qw;
                }
            }
            sqlite3_finalize(st);
        }
    }

    /* Q-width verified in the last hour — the numerator for a size-weighted
     * ETA. Counts each workunit once via its passing submission, so a
     * re-sieved workunit does not inflate the rate. */
    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db->conn,
                "SELECT COALESCE(SUM(w.q_range), 0) "
                "FROM submissions s JOIN workunits w ON w.id = s.workunit_id "
                "WHERE s.verify_status = 'passed' AND s.received_at >= ?1;",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, now_unix - 3600);
            if (sqlite3_step(st) == SQLITE_ROW)
                out->q_verified_1h = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }
    }

    /* Submission rollups. CASE inside COUNT() gives us per-window throughput
     * in a single scan. */
    {
        sqlite3_stmt *st = NULL;
        const char *sql =
            "SELECT "
            "  COUNT(*), "
            "  COALESCE(SUM(num_relations), 0), "
            "  COALESCE(AVG(sieve_seconds), 0.0), "
            "  COALESCE(MAX(received_at), 0), "
            "  COUNT(CASE WHEN received_at >= ?1 THEN 1 END), "
            "  COUNT(CASE WHEN received_at >= ?2 THEN 1 END), "
            "  COUNT(CASE WHEN received_at >= ?3 THEN 1 END) "
            "FROM submissions;";
        if (sqlite3_prepare_v2(db->conn, sql, -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, now_unix - 300);    /* 5m  */
            sqlite3_bind_int64(st, 2, now_unix - 3600);   /* 1h  */
            sqlite3_bind_int64(st, 3, now_unix - 86400);  /* 24h */
            if (sqlite3_step(st) == SQLITE_ROW) {
                out->sub_total          = sqlite3_column_int64 (st, 0);
                out->sub_relations      = sqlite3_column_int64 (st, 1);
                out->avg_sieve_seconds  = sqlite3_column_double(st, 2);
                out->last_submit_unix   = sqlite3_column_int64 (st, 3);
                out->sub_last_5m        = sqlite3_column_int64 (st, 4);
                out->sub_last_1h        = sqlite3_column_int64 (st, 5);
                out->sub_last_24h       = sqlite3_column_int64 (st, 6);
            }
            sqlite3_finalize(st);
        }
    }

    /* Per-client. Include clients that have either contributed relations or
     * currently hold a lease; hide idle zero-relation clients from stale test
     * runs. The dashboard groups worker IDs client-side, so returning all
     * productive/active workers is more useful than a recency cap. */
    {
        sqlite3_stmt *st = NULL;
        const char *where_sql =
            "WHERE c.total_relations > 0 "
            "   OR EXISTS (SELECT 1 FROM workunits "
            "              WHERE state='leased' AND leased_to=c.id) ";
        int n_clients = 0;

        char count_sql[512];
        snprintf(count_sql, sizeof(count_sql),
            "SELECT COUNT(*) FROM clients c %s;", where_sql);
        if (sqlite3_prepare_v2(db->conn, count_sql, -1, &st, NULL) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW)
                n_clients = sqlite3_column_int(st, 0);
            sqlite3_finalize(st);
            st = NULL;
        }

        if (n_clients <= 0) {
            out->clients = NULL;
            out->client_count = 0;
            return 0;
        }

        const char *sql =
            "SELECT "
            "  c.id, c.first_seen, c.last_seen, c.total_failures, "
            "  COUNT(s.id), "
            "  COALESCE(SUM(s.num_relations), 0), "
            "  COALESCE(AVG(s.sieve_seconds), 0.0), "
            "  COALESCE((SELECT id FROM workunits "
            "            WHERE state='leased' AND leased_to=c.id LIMIT 1), '') "
            "FROM clients c "
            "LEFT JOIN submissions s ON s.client_id = c.id "
            "WHERE c.total_relations > 0 "
            "   OR EXISTS (SELECT 1 FROM workunits "
            "              WHERE state='leased' AND leased_to=c.id) "
            "GROUP BY c.id "
            "ORDER BY c.last_seen DESC;";
        if (sqlite3_prepare_v2(db->conn, sql, -1, &st, NULL) == SQLITE_OK) {
            db_stats_client_t *arr = calloc((size_t)n_clients, sizeof(db_stats_client_t));
            int n = 0;
            if (arr) {
                while (sqlite3_step(st) == SQLITE_ROW && n < n_clients) {
                    db_stats_client_t *cc = &arr[n++];
                    const unsigned char *id = sqlite3_column_text(st, 0);
                    if (id) snprintf(cc->id, sizeof(cc->id), "%s", id);
                    cc->first_seen        = sqlite3_column_int64 (st, 1);
                    cc->last_seen         = sqlite3_column_int64 (st, 2);
                    cc->total_failures    = sqlite3_column_int64 (st, 3);
                    cc->submissions       = sqlite3_column_int64 (st, 4);
                    cc->relations         = sqlite3_column_int64 (st, 5);
                    cc->avg_sieve_seconds = sqlite3_column_double(st, 6);
                    const unsigned char *cur = sqlite3_column_text(st, 7);
                    if (cur)
                        snprintf(cc->current_workunit,
                                 sizeof(cc->current_workunit), "%s", cur);
                }
                out->clients      = arr;
                out->client_count = n;
            }
            sqlite3_finalize(st);
        }
    }

    return 0;
}

void db_stats_free(db_stats_t *out)
{
    if (!out) return;
    free(out->clients);
    out->clients      = NULL;
    out->client_count = 0;
}

int db_workunit_counts(ggnfs_db_t *db, db_workunit_counts_t *out)
{
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->conn,
            "SELECT state, COUNT(*) FROM workunits GROUP BY state;",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *state = sqlite3_column_text (st, 0);
        int64_t              n     = sqlite3_column_int64(st, 1);
        if (!state) continue;
        out->total += n;
        if      (strcmp((const char *)state, "available") == 0) out->available = n;
        else if (strcmp((const char *)state, "leased")    == 0) out->leased    = n;
        else if (strcmp((const char *)state, "submitted") == 0) out->submitted = n;
        else if (strcmp((const char *)state, "verified")  == 0) out->verified  = n;
        else if (strcmp((const char *)state, "failed")    == 0) out->failed    = n;
        else if (strcmp((const char *)state, "poisoned")  == 0) out->poisoned  = n;
    }
    sqlite3_finalize(st);
    return 0;
}
