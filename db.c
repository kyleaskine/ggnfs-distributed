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
    "  total_failures  INTEGER NOT NULL DEFAULT 0,"
    "  last_class      TEXT"
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
    /* One writer at a time. This is a check-then-act: two processes opening the
     * same jobdir during an upgrade can both observe a column missing, and the
     * loser's ALTER fails with "duplicate column name" — a LOGICAL conflict
     * that sqlite3_busy_timeout does nothing for, since neither statement ever
     * blocks. BEGIN IMMEDIATE serialises the check with the ALTER so the
     * second process re-reads the post-migration shape and finds nothing to
     * do. Harmless when uncontended, and the whole block is idempotent. */
    if (sqlite3_exec(conn, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "db: migrate: cannot begin: %s\n", sqlite3_errmsg(conn));
        return -1;
    }
#define MIGRATE_FAIL() do { \
        sqlite3_exec(conn, "ROLLBACK;", NULL, NULL, NULL); \
        return -1; \
    } while (0)

    /* workunits.class — 'cpu' (a gnfs-lasieve4-sized q_range) or 'gpu' (a
     * much wider one). Pre-existing rows are all CPU-sized by definition, so
     * the DEFAULT backfills them correctly. */
    int has = column_exists(conn, "workunits", "class");
    if (has < 0) MIGRATE_FAIL();
    if (!has) {
        if (exec_or_log(conn,
                "ALTER TABLE workunits ADD COLUMN class TEXT NOT NULL"
                "     DEFAULT 'cpu';",
                "migrate: add workunits.class") != 0)
            MIGRATE_FAIL();
        fprintf(stderr, "db: migrated schema — added workunits.class"
                        " (existing rows default to 'cpu')\n");
    }

    /* clients.last_class — informational only; NULL until the client leases. */
    has = column_exists(conn, "clients", "last_class");
    if (has < 0) MIGRATE_FAIL();
    if (!has) {
        if (exec_or_log(conn,
                "ALTER TABLE clients ADD COLUMN last_class TEXT;",
                "migrate: add clients.last_class") != 0)
            MIGRATE_FAIL();
    }
#undef MIGRATE_FAIL
    if (sqlite3_exec(conn, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "db: migrate: cannot commit: %s\n", sqlite3_errmsg(conn));
        sqlite3_exec(conn, "ROLLBACK;", NULL, NULL, NULL);
        return -1;
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
    /* Set the busy timeout BEFORE any DDL. db_migrate's ALTER TABLE and
     * SCHEMA_SQL_POST's CREATE INDEX are the most contended statements this
     * process ever runs — a 430K-row index build against a jobdir whose
     * `serve` is mid-write — and without the timeout they take SQLITE_BUSY
     * immediately and fail db_open outright. */
    sqlite3_busy_timeout(conn, 5000);

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

    /* (The busy timeout is set above, before the schema DDL.) Two connections
     * — main event loop and verifier thread — share this file. WAL mode allows
     * readers + one writer; the timeout resolves the brief contention window
     * when both try to commit at once. 5s is long enough to ride out the other
     * side's BEGIN IMMEDIATE / UPDATE / COMMIT but short enough that a real
     * deadlock is still loud. */

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
int db_workunit_base_q_range(ggnfs_db_t *db, int64_t *out)
{
    sqlite3_stmt *st = NULL;
    /* The MODE, not the minimum. A campaign is laid out at one band width and
     * then extended with others, so the most common width is the baseline the
     * job was designed around. MIN would be hostage to a single leftover
     * sliver — a partial extend, a hand-inserted probe row — and would inflate
     * the spot-check sample for every workunit in the job. */
    if (sqlite3_prepare_v2(db->conn,
            "SELECT q_range FROM workunits "
            "GROUP BY q_range ORDER BY COUNT(*) DESC, q_range ASC LIMIT 1;",
            -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_workunit_base_q_range: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }
    int rc = sqlite3_step(st);
    int result = -1;
    if (rc == SQLITE_ROW) {
        if (out) *out = sqlite3_column_int64(st, 0);
        result = 0;
    } else if (rc == SQLITE_DONE) {
        if (out) *out = 0;          /* empty table */
        result = 0;
    }
    sqlite3_finalize(st);
    return result;
}

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

int db_workunit_next_seq(ggnfs_db_t *db, const char *job_id, int64_t *out)
{
    if (!db || !job_id || !out) return -1;

    char prefix[32];
    int plen = snprintf(prefix, sizeof(prefix), "wu-%s-", job_id);
    if (plen <= 0 || plen >= (int)sizeof(prefix)) return -1;

    char like[40];
    snprintf(like, sizeof(like), "%s%%", prefix);

    /* substr() is 1-based, so the suffix starts at plen+1. CAST of a
     * zero-padded suffix ('000123') yields the integer, which is what we
     * want. A row whose ID does not end in digits casts to 0 and simply
     * cannot raise the maximum, which is the safe direction. */
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->conn,
            "SELECT COALESCE(MAX(CAST(substr(id, ?1) AS INTEGER)), -1) + 1 "
            "FROM workunits WHERE id LIKE ?2;",
            -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_workunit_next_seq: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }
    sqlite3_bind_int (st, 1, plen + 1);
    sqlite3_bind_text(st, 2, like, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        *out = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        return 0;
    }
    sqlite3_finalize(st);
    return -1;
}

/* ---- recarve ---- */

typedef struct {
    int64_t q_start;
    int64_t q_range;
    char    side;
} recarve_row_t;

/* One output band, plus how many input rows it consumes. The rows are always
 * a contiguous prefix of what remains, so `consumed` is all we need to delete
 * them by q-range. */
typedef struct {
    int64_t q_start;
    int64_t q_range;
    int64_t consumed;       /* input rows replaced by this band */
    char    side;
} recarve_band_t;

/* Load every eligible row, ordered by q_start. Only rows lying entirely
 * within [qmin, qmax) are returned: a row straddling the window edge is left
 * alone rather than half-recarved. */
static int recarve_load(ggnfs_db_t *db, int64_t qmin, int64_t qmax,
                        recarve_row_t **out_rows, size_t *out_n)
{
    *out_rows = NULL;
    *out_n    = 0;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->conn,
            "SELECT q_start, q_range, side FROM workunits "
            " WHERE state = 'available' AND attempt_count = 0 "
            "   AND q_start >= ?1 AND q_start + q_range <= ?2 "
            " ORDER BY q_start;",
            -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_recarve load: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }
    sqlite3_bind_int64(st, 1, qmin);
    sqlite3_bind_int64(st, 2, qmax);

    size_t cap = 0, n = 0;
    recarve_row_t *rows = NULL;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 4096;
            recarve_row_t *nr = realloc(rows, ncap * sizeof(*nr));
            if (!nr) { free(rows); sqlite3_finalize(st); return -1; }
            rows = nr; cap = ncap;
        }
        rows[n].q_start = sqlite3_column_int64(st, 0);
        rows[n].q_range = sqlite3_column_int64(st, 1);
        const unsigned char *sd = sqlite3_column_text(st, 2);
        rows[n].side = (sd && sd[0]) ? (char)sd[0] : 'a';
        n++;
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { free(rows); return -1; }

    *out_rows = rows;
    *out_n    = n;
    return 0;
}

/* Greedily turn the loaded rows into output bands.
 *
 * Every band starts on an input row's q_start and ends on an input row's
 * q_start+q_range, which is what keeps coverage exact when max_bands cuts the
 * plan short: whatever is not re-tiled is simply left as it was. */
static size_t recarve_plan(const recarve_row_t *rows, size_t n,
                           int64_t target, int64_t max_bands,
                           recarve_band_t *bands, int64_t *out_runs)
{
    size_t nb = 0;
    int64_t runs = 0;
    size_t i = 0;

    while (i < n) {
        /* Extent of the maximal contiguous same-side run starting at i. */
        size_t run_end = i + 1;
        while (run_end < n &&
               rows[run_end].q_start == rows[run_end - 1].q_start + rows[run_end - 1].q_range &&
               rows[run_end].side    == rows[run_end - 1].side)
            run_end++;
        runs++;

        size_t j = i;
        while (j < run_end) {
            if (max_bands > 0 && (int64_t)nb >= max_bands) break;

            if (rows[j].q_range > target) {
                /* Split: one input row becomes several bands. The last one
                 * takes the remainder, so the row stays exactly covered. */
                size_t  nb_row_start = nb;
                int64_t off = 0;
                while (off < rows[j].q_range) {
                    if (max_bands > 0 && (int64_t)nb >= max_bands) break;
                    int64_t w = rows[j].q_range - off;
                    if (w > target) w = target;
                    bands[nb].q_start  = rows[j].q_start + off;
                    bands[nb].q_range  = w;
                    /* Only the first band of the split deletes the input row;
                     * the rest ride along on that same delete. */
                    bands[nb].consumed = (off == 0) ? 1 : 0;
                    bands[nb].side     = rows[j].side;
                    nb++;
                    off += w;
                }
                if (off < rows[j].q_range) {
                    /* Budget ran out mid-row. A partial split would leave the
                     * tail of this row uncovered, so drop the whole thing and
                     * leave the row as it is. Rewinding to the mark keeps that
                     * decision local — it cannot reach into bands emitted for
                     * an earlier row however the budget checks are arranged. */
                    nb = nb_row_start;
                    break;
                }
                j++;
                continue;
            }

            /* Coalesce: accumulate whole rows until the next one would
             * overshoot `target`. */
            int64_t w = 0;
            size_t  k = j;
            while (k < run_end && w + rows[k].q_range <= target) {
                w += rows[k].q_range;
                k++;
            }
            if (k == j) { w = rows[j].q_range; k = j + 1; }  /* exactly-target row */

            bands[nb].q_start  = rows[j].q_start;
            bands[nb].q_range  = w;
            bands[nb].consumed = (int64_t)(k - j);
            bands[nb].side     = rows[j].side;
            nb++;
            j = k;
        }
        if (max_bands > 0 && (int64_t)nb >= max_bands) break;
        i = run_end;
    }

    if (out_runs) *out_runs = runs;
    return nb;
}

int db_recarve(ggnfs_db_t *db, const char *job_id,
               int64_t target_q_range, int64_t qmin, int64_t qmax,
               const char *new_class, int64_t max_bands, int dry_run,
               int64_t now_unix, db_recarve_stats_t *out)
{
    if (!db || !job_id || target_q_range <= 0 || qmax <= qmin) return -1;
    if (out) memset(out, 0, sizeof(*out));

    /* IMMEDIATE even for a dry run: it takes the write lock up front, so the
     * plan we print is the plan that would have been applied rather than one
     * computed against rows a concurrent lease was busy claiming. */
    if (sqlite3_exec(db->conn, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_recarve: begin: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }

    int            result = -1;
    recarve_row_t *rows   = NULL;
    recarve_band_t *bands = NULL;
    size_t          n     = 0;
    sqlite3_stmt   *del   = NULL;

    if (recarve_load(db, qmin, qmax, &rows, &n) != 0) goto done;
    if (n == 0) { result = 0; goto done; }

    /* Upper bound on bands: splitting is the only case that produces more
     * bands than input rows, and it produces at most ceil(q_range/target)
     * each. Sum that bound rather than guessing. */
    size_t band_cap = 0;
    for (size_t i = 0; i < n; i++) {
        int64_t parts = (rows[i].q_range + target_q_range - 1) / target_q_range;
        if (parts < 1) parts = 1;
        band_cap += (size_t)parts;
    }
    bands = calloc(band_cap ? band_cap : 1, sizeof(*bands));
    if (!bands) goto done;

    int64_t runs = 0;
    size_t  nb   = recarve_plan(rows, n, target_q_range, max_bands, bands, &runs);

    int64_t seq = 0;
    if (db_workunit_next_seq(db, job_id, &seq) != 0) goto done;

    if (sqlite3_prepare_v2(db->conn,
            "DELETE FROM workunits "
            " WHERE state = 'available' AND attempt_count = 0 "
            "   AND q_start >= ?1 AND q_start + q_range <= ?2;",
            -1, &del, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_recarve delete: %s\n", sqlite3_errmsg(db->conn));
        goto done;
    }

    int64_t created = 0, deleted = 0, covered = 0;
    for (size_t b = 0; b < nb; b++) {
        if (bands[b].consumed > 0) {
            /* Delete exactly the input rows this band replaces. The band's
             * own extent covers them by construction, except in the split
             * case where the deleted row spans several bands — hence the
             * explicit end below rather than q_start+q_range. */
            int64_t del_end = bands[b].q_start + bands[b].q_range;
            for (size_t t = b + 1; t < nb && bands[t].consumed == 0; t++)
                del_end = bands[t].q_start + bands[t].q_range;

            sqlite3_reset(del);
            sqlite3_bind_int64(del, 1, bands[b].q_start);
            sqlite3_bind_int64(del, 2, del_end);
            if (sqlite3_step(del) != SQLITE_DONE) {
                fprintf(stderr, "db_recarve: delete step: %s\n",
                        sqlite3_errmsg(db->conn));
                goto done;
            }
            int changed = sqlite3_changes(db->conn);
            if (changed != (int)bands[b].consumed) {
                /* Nothing else can write inside BEGIN IMMEDIATE, so this can
                 * only mean the plan and the table disagree. Abort rather
                 * than leave a hole in q coverage. */
                fprintf(stderr,
                        "db_recarve: expected to delete %lld rows in [%lld, %lld), "
                        "deleted %d — aborting\n",
                        (long long)bands[b].consumed, (long long)bands[b].q_start,
                        (long long)del_end, changed);
                goto done;
            }
            deleted += changed;
        }

        char id[64];
        snprintf(id, sizeof(id), "wu-%s-%06lld", job_id, (long long)seq);
        if (db_workunit_insert(db, id, bands[b].q_start, bands[b].q_range,
                               bands[b].side, new_class, now_unix) != 0) {
            fprintf(stderr, "db_recarve: insert failed at seq=%lld\n",
                    (long long)seq);
            goto done;
        }
        seq++;
        created++;
        covered += bands[b].q_range;
    }

    if (out) {
        out->runs          = runs;
        out->bands_created = created;
        out->rows_deleted  = deleted;
        out->q_covered     = covered;
    }
    result = 0;

done:
    if (del) sqlite3_finalize(del);
    free(bands);
    free(rows);
    if (result == 0 && !dry_run) {
        if (sqlite3_exec(db->conn, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
            fprintf(stderr, "db_recarve: commit: %s\n", sqlite3_errmsg(db->conn));
            sqlite3_exec(db->conn, "ROLLBACK;", NULL, NULL, NULL);
            return -1;
        }
    } else {
        sqlite3_exec(db->conn, "ROLLBACK;", NULL, NULL, NULL);
    }
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

int db_clients_seen(ggnfs_db_t *db, const char *client_id, int64_t now_unix,
                    const char *class)
{
    sqlite3_stmt *st = NULL;
    /* One statement, not two: an idle worker polling /lease on --idle-backoff
     * is the highest-frequency request in the system and this runs on the same
     * event-loop thread as /submit and /stats, so a second write transaction
     * per poll is pure overhead. `class` may be NULL (every caller that is not
     * /lease), in which case the stored value is left alone. */
    if (sqlite3_prepare_v2(db->conn,
            "INSERT INTO clients(id, first_seen, last_seen, last_class) "
            "VALUES (?1, ?2, ?2, ?3) "
            "ON CONFLICT(id) DO UPDATE SET last_seen = ?2, "
            "  last_class = COALESCE(?3, last_class);",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text (st, 1, client_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, now_unix);
    if (class) sqlite3_bind_text(st, 3, class, -1, SQLITE_STATIC);
    else       sqlite3_bind_null(st, 3);
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

                out->q.total += qw;
                if      (strcmp(sname, "available") == 0) out->q.available += qw;
                else if (strcmp(sname, "leased")    == 0) out->q.leased    += qw;
                else if (strcmp(sname, "submitted") == 0) out->q.submitted += qw;
                else if (strcmp(sname, "verified")  == 0) out->q.verified  += qw;
                else if (strcmp(sname, "failed")    == 0) out->q.failed    += qw;
                else if (strcmp(sname, "poisoned")  == 0) out->q.poisoned  += qw;

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
                else if (strcmp(sname, "submitted") == 0) {
                    cc->submitted   += n;
                    cc->q_submitted += qw;
                }
                else if (strcmp(sname, "verified")  == 0) {
                    cc->verified   += n;
                    cc->q_verified += qw;
                }
            }
            sqlite3_finalize(st);
        }
    }

    /* Q-width whose sieving finished in the last hour and has since passed
     * verification — the numerator for a size-weighted ETA.
     *
     * Keyed on received_at (when the relations arrived) rather than on when
     * the verifier got to them, because the quantity an ETA wants is sieving
     * throughput, and submission is when the sieving actually finished. There
     * is no verified-at column to key on in any case: `completed_at` is set on
     * the submitted transition, not the verified one. The trade-off is at the
     * window edge — a band submitted 70 minutes ago but verified 2 minutes ago
     * counts zero here while still counting in q_verified — so during a
     * verification backlog this reads low. */
    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db->conn,
                "SELECT COALESCE(SUM(w.q_range), 0) "
                "FROM submissions s JOIN workunits w ON w.id = s.workunit_id "
                "WHERE s.verify_status = 'passed' AND s.received_at >= ?1;",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, now_unix - 3600);
            if (sqlite3_step(st) == SQLITE_ROW)
                out->q_passed_1h = sqlite3_column_int64(st, 0);
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
            "            WHERE state='leased' AND leased_to=c.id LIMIT 1), ''), "
            "  COALESCE(c.last_class, '') "
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
                    const unsigned char *lc = sqlite3_column_text(st, 8);
                    if (lc)
                        snprintf(cc->last_class, sizeof(cc->last_class), "%s", lc);
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
