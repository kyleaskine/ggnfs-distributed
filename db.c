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
    /* gpu_blocks: a lease held over N contiguous workunits. The workunits
     * stay canonical at base width and keep their own attempt_count; a block
     * merges nothing and owns no failure state. It is addressed on the wire by
     * `anchor_wu_id` (its lowest-q member), never by `id` — see the "Addressing"
     * section of GPU-CLIENT.md: relation filenames, workunit_id_is_safe_for_job
     * and finalize-nfs.sh's wu-* glob all key on a real workunit id.
     *
     * There is deliberately no workunits.block_id. Membership is derived from
     * [q_start, q_end), which is exact because a block is always a contiguous
     * run, and `state` here is the single source of truth for "is this block
     * live". A column would need clearing on five terminal paths and would
     * silently strand rows if any were missed. */
    "CREATE TABLE IF NOT EXISTS gpu_blocks ("
    "  id            TEXT PRIMARY KEY,"
    "  anchor_wu_id  TEXT NOT NULL REFERENCES workunits(id),"
    "  client_id     TEXT NOT NULL,"
    "  q_start       INTEGER NOT NULL,"
    "  q_end         INTEGER NOT NULL,"          /* exclusive */
    "  member_count  INTEGER NOT NULL,"
    "  side          TEXT NOT NULL CHECK (side IN ('a','r')),"
    "  state         TEXT NOT NULL CHECK (state IN"
    "                  ('leased','submitted','verified','failed','expired','released')),"
    "  leased_at     INTEGER NOT NULL,"
    "  lease_expires INTEGER NOT NULL,"
    "  created_at    INTEGER NOT NULL"
    ");"
    /* The sweep's "live and lapsed" scan, and the anchor lookup that /renew,
     * /release and /submit all do on every request. */
    "CREATE INDEX IF NOT EXISTS idx_blk_expires"
    "    ON gpu_blocks(lease_expires) WHERE state = 'leased';"
    "CREATE INDEX IF NOT EXISTS idx_blk_anchor ON gpu_blocks(anchor_wu_id, state);"
    "CREATE INDEX IF NOT EXISTS idx_blk_client ON gpu_blocks(client_id, state);"
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
 * idx_wu_lease_v2 drops `class` from the key. The block scan cannot constrain
 * class — a block is contiguous q regardless of how its members were carved —
 * and without a class equality idx_wu_lease stops ordering usefully. Measured
 * with EXPLAIN QUERY PLAN on a 390K-row jobdir:
 *
 *   state=? AND class=?   SEARCH workunits USING INDEX idx_wu_lease
 *   state=? only          SEARCH ... idx_wu_lease (state=?)
 *                         USE TEMP B-TREE FOR ORDER BY   <-- sorts all 356K
 *   state=? with v2       SEARCH workunits USING INDEX idx_wu_lease_v2
 *
 * That sort would run on the mongoose event-loop thread, alongside /submit
 * and /stats, on every block lease.
 *
 * idx_sub_received turns the "q-width verified in the last hour" query from a
 * full submissions scan into a range scan over one hour, which matters as
 * submissions grow into the hundreds of thousands. */
static const char SCHEMA_SQL_POST[] =
    "DROP INDEX IF EXISTS idx_wu_state;"
    "DROP INDEX IF EXISTS idx_wu_state_class;"
    "CREATE INDEX IF NOT EXISTS idx_wu_lease"
    "    ON workunits(state, class, q_start, q_range);"
    "CREATE INDEX IF NOT EXISTS idx_wu_lease_v2"
    "    ON workunits(state, q_start, q_range);"
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

    /* Block submissions. Unlike workunits.block_id — which was deliberately
     * not added — submissions.block_id has no lifecycle: it is written once at
     * insert and never mutated. The verifier needs it to keep block
     * submissions off the per-workunit arm of its UNION, and member_count /
     * q_width let /stats and q_passed_1h describe a block without joining
     * gpu_blocks at all.
     *
     * The defaults are correct for every pre-existing row: they are all
     * single-workunit submissions. q_width defaults to 0 rather than a guess;
     * readers coalesce it against the workunit's own q_range. */
    static const struct { const char *col, *ddl; } sub_cols[] = {
        { "block_id",     "ALTER TABLE submissions ADD COLUMN block_id TEXT;" },
        { "member_count", "ALTER TABLE submissions ADD COLUMN member_count"
                          "     INTEGER NOT NULL DEFAULT 1;" },
        { "q_width",      "ALTER TABLE submissions ADD COLUMN q_width"
                          "     INTEGER NOT NULL DEFAULT 0;" },
    };
    for (size_t i = 0; i < sizeof(sub_cols) / sizeof(sub_cols[0]); i++) {
        has = column_exists(conn, "submissions", sub_cols[i].col);
        if (has < 0) MIGRATE_FAIL();
        if (!has && exec_or_log(conn, sub_cols[i].ddl,
                                "migrate: add submissions column") != 0)
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

/* Highest numeric suffix in use among `table`.id values starting with
 * `prefix`, plus one. Shared by workunit and block id generation.
 *
 * `table` is always a literal from this file — never user input — because a
 * table name cannot be a bound parameter. */
static int next_seq_for_prefix(ggnfs_db_t *db, const char *table,
                               const char *prefix, int64_t *out)
{
    if (!db || !table || !prefix || !out) return -1;
    int plen = (int)strlen(prefix);
    if (plen <= 0) return -1;

    /* Half-open key range [prefix, prefix_hi) so this is a B-tree seek on the
     * PRIMARY KEY rather than a scan.
     *
     * MAX(CAST(substr(id,...))) reads every row of the table: measured at
     * 7.3 ms over 50,000 gpu_blocks rows, and it runs INSIDE db_block_lease's
     * write transaction on the event-loop thread, growing linearly with
     * campaign length. Ordering by id instead is O(log n).
     *
     * Taking the lexicographically greatest id is the same as the numerically
     * greatest because suffixes are zero-padded to a fixed minimum width: for
     * equal widths the orders coincide, and a wider suffix necessarily starts
     * with '1'..'9', which sorts above the '0' that pads every narrower one.
     * So this stays correct even if the sequence ever outgrows its padding. */
    char lo[64], hi[64];
    if (snprintf(lo, sizeof(lo), "%s", prefix) >= (int)sizeof(lo)) return -1;
    if (snprintf(hi, sizeof(hi), "%s", prefix) >= (int)sizeof(hi)) return -1;
    hi[plen - 1]++;                     /* prefix always ends in '-' */

    char sql[192];
    snprintf(sql, sizeof(sql),
            "SELECT id FROM %s WHERE id >= ?1 AND id < ?2 "
            "ORDER BY id DESC LIMIT 1;", table);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "next_seq_for_prefix(%s): %s\n", table,
                sqlite3_errmsg(db->conn));
        return -1;
    }
    sqlite3_bind_text(st, 1, lo, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, hi, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const unsigned char *id = sqlite3_column_text(st, 0);
        /* An id whose suffix is not a number yields 0, which cannot raise the
         * maximum — the safe direction, same as the old CAST. */
        *out = id ? strtoll((const char *)id + plen, NULL, 10) + 1 : 0;
        sqlite3_finalize(st);
        return 0;
    }
    sqlite3_finalize(st);
    if (rc == SQLITE_DONE) { *out = 0; return 0; }   /* nothing yet */
    return -1;
}

int db_workunit_next_seq(ggnfs_db_t *db, const char *job_id, int64_t *out)
{
    if (!db || !job_id || !out) return -1;
    char prefix[32];
    if (snprintf(prefix, sizeof(prefix), "wu-%s-", job_id) >= (int)sizeof(prefix))
        return -1;
    return next_seq_for_prefix(db, "workunits", prefix, out);
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

/* Claim the next available workunit. Returns 0 on claim (out filled), 1 if
 * nothing is available, -1 on error.
 *
 * No class predicate. `class` used to size a workunit — 'gpu' meant a band ~100x
 * a core's — and the lease had to encode an asymmetric fallback (gpu could take
 * cpu work, never the reverse) to stop a single core from timing out on a band
 * it could not finish. Blocks replaced that: sizing is now a property of the
 * LEASE, assembled per request from contiguous base-width rows, so every row in
 * the table is the same size again and anyone can take any of them. Dropping
 * the predicate also un-strands any gpu-class rows an older `extend` left
 * behind, which under the old chain no CPU client could ever reach. */
static int lease_next_available(ggnfs_db_t *db, const char *client_id,
                                int64_t lease_seconds, int64_t now_unix,
                                int lease_desc, db_lease_result_t *out)
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
            "              WHERE state = 'available' "
            "              ORDER BY q_start ASC LIMIT 1) "
            "RETURNING id, q_start, q_range, side, class;";
    static const char *const sql_desc =
            "UPDATE workunits "
            "  SET state = 'leased',"
            "      leased_at = ?1,"
            "      leased_to = ?2,"
            "      lease_expires = ?3 "
            "  WHERE id = (SELECT id FROM workunits "
            "              WHERE state = 'available' "
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

    int result;
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        lease_result_from_row(st, out);
        result = 0;
    } else if (rc == SQLITE_DONE) {
        result = 1; /* nothing available */
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

    /* class_want is recorded on the client row for the dashboard (a GPU box
     * quietly asking for cpu-class work used to be worth seeing) but no longer
     * steers selection — see lease_next_available. */
    (void)class_want;
    return lease_next_available(db, client_id, lease_seconds, now_unix,
                                lease_desc, out);
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
            /* leased_to matters: without it a client whose lease already
             * lapsed can submit against a workunit the sweep has since
             * reissued to somebody else, marking it 'submitted' out from under
             * the current holder. */
            "UPDATE workunits SET state = 'submitted', completed_at = ?1 "
            "  WHERE id = ?2 AND state = 'leased' AND leased_to = ?3;",
            -1, &up, NULL) != SQLITE_OK)
        goto done;
    sqlite3_bind_int64(up, 1, now_unix);
    sqlite3_bind_text (up, 2, workunit_id, -1, SQLITE_STATIC);
    sqlite3_bind_text (up, 3, client_id,   -1, SQLITE_STATIC);
    if (sqlite3_step(up) != SQLITE_DONE) goto done;

    if (sqlite3_changes(db->conn) == 0) {
        result = 1; /* not leased to this client — caller responds 409 */
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

/* ---- gpu blocks -------------------------------------------------------
 *
 * See db.h for the design. The one thing to keep in mind while reading: a
 * block's members are never recorded anywhere. They are exactly the workunits
 * whose q_start falls in [q_start, q_end), which is sound because a block is
 * always built from a contiguous run and no second block can form over rows
 * that are not 'available'. Every statement below that touches members is
 * therefore a range predicate, and every one of them also carries
 * `leased_to = <the block's client>` so a row the sweep already handed to
 * somebody else cannot be caught up in it.
 */

/* Fill *out from a gpu_blocks row selected as:
 * id, anchor_wu_id, q_start, q_end, member_count, side. */
static void block_from_row(sqlite3_stmt *st, db_block_t *out)
{
    const unsigned char *id     = sqlite3_column_text(st, 0);
    const unsigned char *anchor = sqlite3_column_text(st, 1);
    const unsigned char *side   = sqlite3_column_text(st, 5);
    snprintf(out->id,           sizeof(out->id),           "%s", id     ? (const char *)id     : "");
    snprintf(out->anchor_wu_id, sizeof(out->anchor_wu_id), "%s", anchor ? (const char *)anchor : "");
    out->q_start      = sqlite3_column_int64(st, 2);
    out->q_end        = sqlite3_column_int64(st, 3);
    out->member_count = sqlite3_column_int64(st, 4);
    out->side         = side ? (char)side[0] : '?';
}

#define BLOCK_SELECT_COLS \
    "id, anchor_wu_id, q_start, q_end, member_count, side"

/* Push a block row and all its still-owned members out to `until`. Returns
 * the number of member rows renewed, or -1 on error. Caller holds the txn. */
static int64_t block_touch_members(ggnfs_db_t *db, const db_block_t *b,
                                   const char *client_id, int64_t until,
                                   int64_t now_unix)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->conn,
            "UPDATE workunits SET lease_expires = ?1 "
            "  WHERE q_start >= ?2 AND q_start < ?3 "
            "    AND state = 'leased' AND leased_to = ?4 "
            "    AND lease_expires >= ?5;",
            -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "block_touch_members: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }
    sqlite3_bind_int64(st, 1, until);
    sqlite3_bind_int64(st, 2, b->q_start);
    sqlite3_bind_int64(st, 3, b->q_end);
    sqlite3_bind_text (st, 4, client_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 5, now_unix);
    int rc = sqlite3_step(st);
    int64_t changed = sqlite3_changes(db->conn);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return -1;

    if (sqlite3_prepare_v2(db->conn,
            "UPDATE gpu_blocks SET lease_expires = ?1 WHERE id = ?2;",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, until);
    sqlite3_bind_text (st, 2, b->id, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? changed : -1;
}

/* One candidate row from the descending availability scan. */
typedef struct {
    char    id[64];
    int64_t q_start;
    int64_t q_range;
    char    side;
} blk_scan_row_t;

int db_block_lease(ggnfs_db_t *db, const char *job_id, const char *client_id,
                   int64_t target_q_width, int64_t min_q_width,
                   int64_t max_members, int64_t attempt_ceiling,
                   int64_t lease_seconds, int64_t now_unix,
                   db_block_t *out)
{
    if (!db || !job_id || !client_id || !out) return -1;
    memset(out, 0, sizeof(*out));

    if (max_members  < 1) max_members  = 1;
    if (target_q_width < 1) return 1;               /* nothing sensible to ask for */
    if (min_q_width  < 1) min_q_width  = 1;
    if (min_q_width  > target_q_width) min_q_width = target_q_width;
    if (attempt_ceiling < 1) attempt_ceiling = 1;

    if (sqlite3_exec(db->conn, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_block_lease: begin: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }

    int             result = -1;
    sqlite3_stmt   *st     = NULL;
    blk_scan_row_t *run    = NULL;

    /* --- 1. Idempotency: this client may already hold a live block. -----
     * A lost /lease response retried must give the same block back, not a
     * second one. Deliberately ahead of every sizing parameter: what the
     * client already holds is what it gets, whatever it just asked for. */
    if (sqlite3_prepare_v2(db->conn,
            "SELECT " BLOCK_SELECT_COLS " FROM gpu_blocks "
            " WHERE client_id = ?1 AND state = 'leased' AND lease_expires >= ?2 "
            " ORDER BY leased_at DESC, id DESC LIMIT 1;",
            -1, &st, NULL) != SQLITE_OK)
        goto done;
    sqlite3_bind_text (st, 1, client_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, now_unix);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        block_from_row(st, out);
        sqlite3_finalize(st); st = NULL;
        if (block_touch_members(db, out, client_id,
                                now_unix + lease_seconds, now_unix) < 0)
            goto done;
        result = 0;
        goto done;
    }
    if (rc != SQLITE_DONE) goto done;
    sqlite3_finalize(st); st = NULL;

    /* --- 1b. …and it may already hold an ordinary workunit. -------------
     * db_lease has its own "one live lease per client_id" guard, but this
     * function runs BEFORE it, so building a block here would hand a client a
     * second concurrent lease. That happens for real: a slot whose block scan
     * found no run takes a single workunit, the response is lost, it retries,
     * and by then a run has appeared — leaving the single workunit leased,
     * never heartbeated (the client only tracks the block anchor) and
     * eventually expired with an attempt_count++. Defer to db_lease, which
     * returns the workunit the client already has. */
    if (sqlite3_prepare_v2(db->conn,
            "SELECT 1 FROM workunits "
            " WHERE state = 'leased' AND leased_to = ?1 AND lease_expires >= ?2 "
            " LIMIT 1;",
            -1, &st, NULL) != SQLITE_OK)
        goto done;
    sqlite3_bind_text (st, 1, client_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, now_unix);
    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) { result = 1; goto done; }
    if (rc != SQLITE_DONE) goto done;
    sqlite3_finalize(st); st = NULL;

    /* --- 2. Scan descending for a contiguous run. -----------------------
     * Descending because the CPU fleet leases ascending: starting the two at
     * opposite ends of the q-range keeps them out of each other's way and
     * leaves the block scan a pristine, unfragmented top edge.
     *
     * The window is wider than one block so a short run near the top can be
     * skipped past rather than accepted. Bounded so a fragmented job cannot
     * turn one lease into an unbounded scan on the event-loop thread. */
    int64_t scan_limit = max_members * 4;
    if (scan_limit > 8192) scan_limit = 8192;
    if (scan_limit < max_members) scan_limit = max_members;

    run = calloc((size_t)max_members, sizeof(*run));
    if (!run) goto done;

    if (sqlite3_prepare_v2(db->conn,
            "SELECT id, q_start, q_range, side FROM workunits "
            " WHERE state = 'available' AND attempt_count < ?1 "
            " ORDER BY q_start DESC LIMIT ?2;",
            -1, &st, NULL) != SQLITE_OK)
        goto done;
    sqlite3_bind_int64(st, 1, attempt_ceiling);
    sqlite3_bind_int64(st, 2, scan_limit);

    int64_t run_len = 0, run_width = 0;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        blk_scan_row_t r;
        const unsigned char *id   = sqlite3_column_text(st, 0);
        const unsigned char *side = sqlite3_column_text(st, 3);
        snprintf(r.id, sizeof(r.id), "%s", id ? (const char *)id : "");
        r.q_start = sqlite3_column_int64(st, 1);
        r.q_range = sqlite3_column_int64(st, 2);
        r.side    = side ? (char)side[0] : '?';
        if (r.q_range <= 0) continue;               /* defensive; never valid */

        if (run_len > 0) {
            /* Descending, so this row sits directly below the run's low edge
             * exactly when its top touches the run's bottom. Same side, too:
             * a block is sieved by one command with one --sq-side. */
            int contiguous = (r.q_start + r.q_range == run[run_len - 1].q_start)
                          && (r.side == run[0].side);
            if (!contiguous) {
                if (run_width >= min_q_width) break;  /* good enough; take it */
                run_len = 0; run_width = 0;           /* too short; start over */
            }
        }
        if (run_len >= max_members) break;
        run[run_len++] = r;
        run_width     += r.q_range;
        if (run_width >= target_q_width) break;
    }
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) goto done;
    sqlite3_finalize(st); st = NULL;

    if (run_len == 0 || run_width < min_q_width) {
        /* Nothing wide enough. The caller falls back to an ordinary
         * single-workunit lease, which is why this is not an error: a card on
         * a small band is merely inefficient. */
        result = 1;
        goto done;
    }

    /* run[0] is the highest-q member, run[run_len-1] the lowest. The anchor is
     * the lowest — deterministic regardless of scan direction, which matters
     * because it is the id the whole protocol keys on. */
    out->q_start      = run[run_len - 1].q_start;
    out->q_end        = run[0].q_start + run[0].q_range;
    out->member_count = run_len;
    out->side         = run[0].side;
    snprintf(out->anchor_wu_id, sizeof(out->anchor_wu_id), "%s",
             run[run_len - 1].id);

    if (out->q_end - out->q_start != run_width) {
        /* Contiguity is the invariant every member query depends on. If it
         * ever fails, deriving membership from the range would touch rows that
         * are not members, so refuse rather than proceed. */
        fprintf(stderr, "db_block_lease: non-contiguous run "
                        "(width %lld != span %lld) — refusing\n",
                (long long)run_width, (long long)(out->q_end - out->q_start));
        goto done;
    }

    /* --- 3. Allocate the block id and insert the row. ------------------- */
    {
        char prefix[40];
        if (snprintf(prefix, sizeof(prefix), "blk-%s-", job_id) >= (int)sizeof(prefix))
            goto done;
        int64_t seq = 0;
        if (next_seq_for_prefix(db, "gpu_blocks", prefix, &seq) != 0) goto done;
        snprintf(out->id, sizeof(out->id), "%s%06lld", prefix, (long long)seq);
    }

    char side_str[2] = { out->side, 0 };
    if (sqlite3_prepare_v2(db->conn,
            "INSERT INTO gpu_blocks(id, anchor_wu_id, client_id, q_start, q_end,"
            "                       member_count, side, state, leased_at,"
            "                       lease_expires, created_at) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, 'leased', ?8, ?9, ?8);",
            -1, &st, NULL) != SQLITE_OK)
        goto done;
    sqlite3_bind_text (st, 1, out->id,           -1, SQLITE_STATIC);
    sqlite3_bind_text (st, 2, out->anchor_wu_id, -1, SQLITE_STATIC);
    sqlite3_bind_text (st, 3, client_id,         -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, out->q_start);
    sqlite3_bind_int64(st, 5, out->q_end);
    sqlite3_bind_int64(st, 6, out->member_count);
    sqlite3_bind_text (st, 7, side_str, 1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 8, now_unix);
    sqlite3_bind_int64(st, 9, now_unix + lease_seconds);
    if (sqlite3_step(st) != SQLITE_DONE) goto done;
    sqlite3_finalize(st); st = NULL;

    /* --- 4. Flip the members to leased. --------------------------------
     * By range, not by id list: inside this BEGIN IMMEDIATE the run cannot
     * have changed, so every available row in [q_start, q_end) under the
     * ceiling is exactly a member. The count check turns any violation of
     * that into a rollback rather than a half-leased block. */
    if (sqlite3_prepare_v2(db->conn,
            "UPDATE workunits "
            "   SET state = 'leased', leased_at = ?1, leased_to = ?2,"
            "       lease_expires = ?3 "
            " WHERE q_start >= ?4 AND q_start < ?5 "
            "   AND state = 'available' AND attempt_count < ?6;",
            -1, &st, NULL) != SQLITE_OK)
        goto done;
    sqlite3_bind_int64(st, 1, now_unix);
    sqlite3_bind_text (st, 2, client_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, now_unix + lease_seconds);
    sqlite3_bind_int64(st, 4, out->q_start);
    sqlite3_bind_int64(st, 5, out->q_end);
    sqlite3_bind_int64(st, 6, attempt_ceiling);
    if (sqlite3_step(st) != SQLITE_DONE) goto done;
    if (sqlite3_changes(db->conn) != (int)out->member_count) {
        fprintf(stderr, "db_block_lease: expected %lld members in [%lld,%lld), "
                        "updated %d — rolling back\n",
                (long long)out->member_count, (long long)out->q_start,
                (long long)out->q_end, sqlite3_changes(db->conn));
        goto done;
    }
    result = 0;

done:
    if (st) sqlite3_finalize(st);
    free(run);
    if (result == 0) {
        if (sqlite3_exec(db->conn, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
            fprintf(stderr, "db_block_lease: commit: %s\n", sqlite3_errmsg(db->conn));
            sqlite3_exec(db->conn, "ROLLBACK;", NULL, NULL, NULL);
            return -1;
        }
    } else {
        if (result == -1)
            fprintf(stderr, "db_block_lease: %s\n", sqlite3_errmsg(db->conn));
        sqlite3_exec(db->conn, "ROLLBACK;", NULL, NULL, NULL);
        if (result != 1) memset(out, 0, sizeof(*out));
    }
    return result;
}

int db_block_get(ggnfs_db_t *db, const char *block_id, db_block_t *out)
{
    if (!db || !block_id || !out) return -1;
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->conn,
            "SELECT " BLOCK_SELECT_COLS " FROM gpu_blocks WHERE id = ?1;",
            -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_block_get: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }
    sqlite3_bind_text(st, 1, block_id, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    int result;
    if (rc == SQLITE_ROW)       { block_from_row(st, out); result = 0; }
    else if (rc == SQLITE_DONE) { result = 1; }
    else                        { result = -1; }
    sqlite3_finalize(st);
    return result;
}

int db_block_find_live(ggnfs_db_t *db, const char *anchor_wu_id,
                       db_block_t *out)
{
    if (!db || !anchor_wu_id || !out) return -1;
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *st = NULL;
    /* state='leased' only, with no expiry test: db_submit accepts a workunit
     * whose lease has lapsed but which the sweep has not reclaimed yet, and a
     * block has to behave the same way or a client that finishes a hair late
     * loses a quarter hour of card time. Once the sweep runs, the block is
     * 'expired' and this stops matching. */
    if (sqlite3_prepare_v2(db->conn,
            "SELECT " BLOCK_SELECT_COLS " FROM gpu_blocks "
            " WHERE anchor_wu_id = ?1 AND state = 'leased' "
            " ORDER BY id DESC LIMIT 1;",
            -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_block_find_live: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }
    sqlite3_bind_text(st, 1, anchor_wu_id, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    int result;
    if (rc == SQLITE_ROW)       { block_from_row(st, out); result = 0; }
    else if (rc == SQLITE_DONE) { result = 1; }
    else {
        fprintf(stderr, "db_block_find_live: step: %s\n", sqlite3_errmsg(db->conn));
        result = -1;
    }
    sqlite3_finalize(st);
    return result;
}

/* Load the live block anchored here AND owned by client_id. Shared by renew /
 * release / submit, which all have to prove ownership before acting.
 * 0 found, 1 no match, -1 error. Caller holds the transaction. */
static int block_load_owned(ggnfs_db_t *db, const char *anchor_wu_id,
                            const char *client_id, int require_unexpired,
                            int64_t now_unix, db_block_t *out)
{
    sqlite3_stmt *st = NULL;
    const char *sql = require_unexpired
        ? "SELECT " BLOCK_SELECT_COLS " FROM gpu_blocks "
          " WHERE anchor_wu_id = ?1 AND client_id = ?2 AND state = 'leased' "
          "   AND lease_expires >= ?3 ORDER BY id DESC LIMIT 1;"
        : "SELECT " BLOCK_SELECT_COLS " FROM gpu_blocks "
          " WHERE anchor_wu_id = ?1 AND client_id = ?2 AND state = 'leased' "
          " ORDER BY id DESC LIMIT 1;";
    if (sqlite3_prepare_v2(db->conn, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, anchor_wu_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, client_id,    -1, SQLITE_STATIC);
    if (require_unexpired) sqlite3_bind_int64(st, 3, now_unix);
    int rc = sqlite3_step(st);
    int result;
    if (rc == SQLITE_ROW)       { block_from_row(st, out); result = 0; }
    else if (rc == SQLITE_DONE) { result = 1; }
    else                        { result = -1; }
    sqlite3_finalize(st);
    return result;
}

int db_block_renew(ggnfs_db_t *db, const char *anchor_wu_id,
                   const char *client_id, int64_t lease_seconds,
                   int64_t now_unix, int64_t *out_members)
{
    if (out_members) *out_members = 0;
    if (!db || !anchor_wu_id || !client_id) return -1;

    if (sqlite3_exec(db->conn, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK)
        return -1;

    db_block_t b;
    /* require_unexpired: same reasoning as db_renew_lease. Once the lease has
     * actually lapsed the sweep may have reissued these members, so a late
     * heartbeat must not take them back. The client sees 409 and drops it. */
    int result = block_load_owned(db, anchor_wu_id, client_id, 1, now_unix, &b);
    if (result == 0) {
        int64_t n = block_touch_members(db, &b, client_id,
                                        now_unix + lease_seconds, now_unix);
        if (n < 0) result = -1;
        else if (out_members) *out_members = n;
    }

    if (result == 0) {
        if (sqlite3_exec(db->conn, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
            sqlite3_exec(db->conn, "ROLLBACK;", NULL, NULL, NULL);
            return -1;
        }
    } else {
        if (result == -1)
            fprintf(stderr, "db_block_renew: %s\n", sqlite3_errmsg(db->conn));
        sqlite3_exec(db->conn, "ROLLBACK;", NULL, NULL, NULL);
    }
    return result;
}

int db_block_release(ggnfs_db_t *db, const char *anchor_wu_id,
                     const char *client_id)
{
    if (!db || !anchor_wu_id || !client_id) return -1;
    if (sqlite3_exec(db->conn, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK)
        return -1;

    db_block_t    b;
    sqlite3_stmt *st = NULL;
    int result = block_load_owned(db, anchor_wu_id, client_id, 0, 0, &b);
    if (result != 0) goto done;

    /* No attempt_count++: this is a client shutting down cleanly, not work
     * that failed. Same rule db_release_lease follows for a single workunit. */
    if (sqlite3_prepare_v2(db->conn,
            "UPDATE workunits "
            "   SET state = 'available', leased_at = NULL, leased_to = NULL,"
            "       lease_expires = NULL "
            " WHERE q_start >= ?1 AND q_start < ?2 "
            "   AND state = 'leased' AND leased_to = ?3;",
            -1, &st, NULL) != SQLITE_OK) { result = -1; goto done; }
    sqlite3_bind_int64(st, 1, b.q_start);
    sqlite3_bind_int64(st, 2, b.q_end);
    sqlite3_bind_text (st, 3, client_id, -1, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_DONE) { result = -1; goto done; }
    sqlite3_finalize(st); st = NULL;

    if (sqlite3_prepare_v2(db->conn,
            "UPDATE gpu_blocks SET state = 'released' WHERE id = ?1;",
            -1, &st, NULL) != SQLITE_OK) { result = -1; goto done; }
    sqlite3_bind_text(st, 1, b.id, -1, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_DONE) { result = -1; goto done; }

done:
    if (st) sqlite3_finalize(st);
    if (result == 0) {
        if (sqlite3_exec(db->conn, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
            sqlite3_exec(db->conn, "ROLLBACK;", NULL, NULL, NULL);
            return -1;
        }
    } else {
        if (result == -1)
            fprintf(stderr, "db_block_release: %s\n", sqlite3_errmsg(db->conn));
        sqlite3_exec(db->conn, "ROLLBACK;", NULL, NULL, NULL);
    }
    return result;
}

int db_block_submit(ggnfs_db_t *db, const char *anchor_wu_id,
                    const char *client_id, const char *rel_file_path,
                    const char *body_sha256_hex, int64_t num_relations,
                    double sieve_seconds, int64_t now_unix)
{
    if (!db || !anchor_wu_id || !client_id) return -1;
    if (sqlite3_exec(db->conn, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK)
        return -1;

    db_block_t    b;
    sqlite3_stmt *st = NULL;
    int result = block_load_owned(db, anchor_wu_id, client_id, 0, 0, &b);
    if (result != 0) goto done;

    if (sqlite3_prepare_v2(db->conn,
            "UPDATE workunits SET state = 'submitted', completed_at = ?1 "
            " WHERE q_start >= ?2 AND q_start < ?3 "
            "   AND state = 'leased' AND leased_to = ?4;",
            -1, &st, NULL) != SQLITE_OK) { result = -1; goto done; }
    sqlite3_bind_int64(st, 1, now_unix);
    sqlite3_bind_int64(st, 2, b.q_start);
    sqlite3_bind_int64(st, 3, b.q_end);
    sqlite3_bind_text (st, 4, client_id, -1, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_DONE) { result = -1; goto done; }
    if (sqlite3_changes(db->conn) != (int)b.member_count) {
        /* Some members were reclaimed and possibly reissued while this block
         * was in flight. Accepting a partial submission would leave the
         * submission's q_width describing a range it no longer owns, and the
         * verifier's q-range check is defined against that width. Refuse the
         * whole thing; the client sees 409 and drops the band. The heartbeat
         * is what keeps this rare. */
        fprintf(stderr, "db_block_submit: %s covers %lld members, %d still "
                        "owned by %s — rejecting\n",
                b.id, (long long)b.member_count, sqlite3_changes(db->conn),
                client_id);
        result = 1;
        goto done;
    }
    sqlite3_finalize(st); st = NULL;

    if (sqlite3_prepare_v2(db->conn,
            "UPDATE gpu_blocks SET state = 'submitted' WHERE id = ?1;",
            -1, &st, NULL) != SQLITE_OK) { result = -1; goto done; }
    sqlite3_bind_text(st, 1, b.id, -1, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_DONE) { result = -1; goto done; }
    sqlite3_finalize(st); st = NULL;

    /* workunit_id is the anchor: a real workunit id, so the existing foreign
     * key, the idx_sub_wu index and every wu-keyed reader keep working.
     * block_id / member_count / q_width are what let the verifier and /stats
     * describe the block without joining gpu_blocks. */
    if (sqlite3_prepare_v2(db->conn,
            "INSERT INTO submissions("
            "  workunit_id, client_id, received_at, file_path, sha256,"
            "  num_relations, verify_status, sieve_seconds,"
            "  block_id, member_count, q_width"
            ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, 'pending', ?7, ?8, ?9, ?10);",
            -1, &st, NULL) != SQLITE_OK) { result = -1; goto done; }
    sqlite3_bind_text  (st,  1, anchor_wu_id,    -1, SQLITE_STATIC);
    sqlite3_bind_text  (st,  2, client_id,       -1, SQLITE_STATIC);
    sqlite3_bind_int64 (st,  3, now_unix);
    sqlite3_bind_text  (st,  4, rel_file_path,   -1, SQLITE_STATIC);
    sqlite3_bind_text  (st,  5, body_sha256_hex, -1, SQLITE_STATIC);
    sqlite3_bind_int64 (st,  6, num_relations);
    sqlite3_bind_double(st,  7, sieve_seconds);
    sqlite3_bind_text  (st,  8, b.id,            -1, SQLITE_STATIC);
    sqlite3_bind_int64 (st,  9, b.member_count);
    sqlite3_bind_int64 (st, 10, b.q_end - b.q_start);
    if (sqlite3_step(st) != SQLITE_DONE) { result = -1; goto done; }

done:
    if (st) sqlite3_finalize(st);
    if (result == 0) {
        if (sqlite3_exec(db->conn, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
            sqlite3_exec(db->conn, "ROLLBACK;", NULL, NULL, NULL);
            return -1;
        }
    } else {
        if (result == -1)
            fprintf(stderr, "db_block_submit: %s\n", sqlite3_errmsg(db->conn));
        sqlite3_exec(db->conn, "ROLLBACK;", NULL, NULL, NULL);
    }
    return result;
}

int db_block_expire_sweep(ggnfs_db_t *db, int64_t now_unix,
                          int64_t max_attempts, int64_t *out_blocks,
                          int64_t *out_requeued, int64_t *out_poisoned)
{
    if (out_blocks)   *out_blocks   = 0;
    if (out_requeued) *out_requeued = 0;
    if (out_poisoned) *out_poisoned = 0;
    if (!db) return -1;

    if (sqlite3_exec(db->conn, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK)
        return -1;

    int           result   = -1;
    sqlite3_stmt *st       = NULL;
    db_block_t   *expired  = NULL;
    char        **owners   = NULL;
    size_t        n = 0, cap = 0;
    int64_t       requeued = 0, poisoned = 0;

    /* Collect first, then act: the loop below writes gpu_blocks, and stepping
     * a SELECT over a table being updated in the same transaction is a shape
     * worth not relying on. */
    if (sqlite3_prepare_v2(db->conn,
            "SELECT " BLOCK_SELECT_COLS ", client_id FROM gpu_blocks "
            " WHERE state = 'leased' AND lease_expires < ?1;",
            -1, &st, NULL) != SQLITE_OK)
        goto done;
    sqlite3_bind_int64(st, 1, now_unix);
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 8;
            db_block_t *nb = realloc(expired, ncap * sizeof(*nb));
            char      **no = realloc(owners,  ncap * sizeof(*no));
            if (nb) expired = nb;
            if (no) owners  = no;
            if (!nb || !no) goto done;
            cap = ncap;
        }
        block_from_row(st, &expired[n]);
        const unsigned char *cid = sqlite3_column_text(st, 6);
        owners[n] = strdup(cid ? (const char *)cid : "");
        if (!owners[n]) goto done;
        n++;
    }
    if (rc != SQLITE_DONE) goto done;
    sqlite3_finalize(st); st = NULL;

    for (size_t i = 0; i < n; i++) {
        /* Same CASE-on-post-increment shape as db_lease_expire_sweep, scoped
         * to this block's range and owner. Every member takes one strike: the
         * evidence points at the client, not at any one sub-range. What keeps
         * that from mass-poisoning a contiguous region is the attempt ceiling
         * in db_block_lease, which drops a twice-failed range out of block
         * eligibility long before --max-attempts. */
        if (sqlite3_prepare_v2(db->conn,
                "UPDATE workunits "
                "   SET attempt_count = attempt_count + 1,"
                "       state = CASE WHEN attempt_count + 1 >= ?1 "
                "                    THEN 'poisoned' ELSE 'available' END,"
                "       leased_at = NULL, leased_to = NULL, lease_expires = NULL "
                " WHERE q_start >= ?2 AND q_start < ?3 "
                "   AND state = 'leased' AND leased_to = ?4 "
                "RETURNING state;",
                -1, &st, NULL) != SQLITE_OK)
            goto done;
        sqlite3_bind_int64(st, 1, max_attempts);
        sqlite3_bind_int64(st, 2, expired[i].q_start);
        sqlite3_bind_int64(st, 3, expired[i].q_end);
        sqlite3_bind_text (st, 4, owners[i], -1, SQLITE_STATIC);
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            const unsigned char *s = sqlite3_column_text(st, 0);
            if (s && strcmp((const char *)s, "poisoned") == 0) poisoned++;
            else requeued++;
        }
        if (rc != SQLITE_DONE) goto done;
        sqlite3_finalize(st); st = NULL;

        if (sqlite3_prepare_v2(db->conn,
                "UPDATE gpu_blocks SET state = 'expired' WHERE id = ?1;",
                -1, &st, NULL) != SQLITE_OK)
            goto done;
        sqlite3_bind_text(st, 1, expired[i].id, -1, SQLITE_STATIC);
        if (sqlite3_step(st) != SQLITE_DONE) goto done;
        sqlite3_finalize(st); st = NULL;
    }
    result = 0;

done:
    if (st) sqlite3_finalize(st);
    if (result == 0) {
        if (sqlite3_exec(db->conn, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
            fprintf(stderr, "db_block_expire_sweep: commit: %s\n",
                    sqlite3_errmsg(db->conn));
            sqlite3_exec(db->conn, "ROLLBACK;", NULL, NULL, NULL);
            result = -1;
        } else {
            if (out_blocks)   *out_blocks   = (int64_t)n;
            if (out_requeued) *out_requeued = requeued;
            if (out_poisoned) *out_poisoned = poisoned;
        }
    } else {
        fprintf(stderr, "db_block_expire_sweep: %s\n", sqlite3_errmsg(db->conn));
        sqlite3_exec(db->conn, "ROLLBACK;", NULL, NULL, NULL);
    }
    for (size_t i = 0; i < n; i++) free(owners[i]);
    free(owners);
    free(expired);
    return result;
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

    /* Two arms. The `AND s.block_id IS NULL` on the first is load-bearing:
     * a block submission stores the ANCHOR in workunit_id (so the foreign key
     * and every wu-keyed reader keep working), which means it also satisfies
     * the per-workunit join. Without the guard it matches both arms, UNION ALL
     * preserves arm order on equal s.id, and the block gets verified against
     * its anchor's base-width q-range instead of its own — roughly 1/member
     * of the relations pass the q-range check, db_verify_fail requeues only
     * the anchor, and the other members sit in 'submitted' forever because the
     * sweep only touches 'leased'. That failure has been reproduced.
     *
     * ORDER BY 1, not ORDER BY s.id: SQLite rejects a qualified column in a
     * compound SELECT's ORDER BY. */
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->conn,
            "SELECT s.id, s.workunit_id, s.file_path,"
            "       w.q_start, w.q_range, w.side, w.attempt_count,"
            "       '' AS block_id, 1 AS member_count "
            "FROM submissions s JOIN workunits w ON w.id = s.workunit_id "
            "WHERE s.verify_status = 'pending' AND s.block_id IS NULL "
            "UNION ALL "
            "SELECT s.id, s.workunit_id, s.file_path,"
            "       b.q_start, b.q_end - b.q_start, b.side, 0,"
            "       b.id, b.member_count "
            "FROM submissions s JOIN gpu_blocks b ON b.id = s.block_id "
            "WHERE s.verify_status = 'pending' AND s.block_id IS NOT NULL "
            "ORDER BY 1 LIMIT 1;",
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
        const unsigned char *blk = sqlite3_column_text(st, 7);
        snprintf(out->block_id, sizeof(out->block_id), "%s",
                 blk ? (const char *)blk : "");
        out->member_count = sqlite3_column_int64(st, 8);
        if (out->member_count < 1) out->member_count = 1;
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
                /* s.q_width for a block, the workunit's own q_range
                 * otherwise. Without this a block contributes its ANCHOR's
                 * base width instead of its own — understating GPU throughput
                 * by a factor of member_count in the numerator of the very
                 * metric added to make the two engines comparable. q_width is
                 * 0 on every row written before blocks existed, hence the
                 * NULLIF rather than a bare COALESCE. */
                "SELECT COALESCE(SUM(COALESCE(NULLIF(s.q_width, 0), w.q_range)), 0) "
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
            /* SUM(member_count), not COUNT(*): a block submission is several
             * workunits in one row, and counting rows makes a card look 50x
             * less productive than it is. member_count is 1 on every
             * pre-block row, so this is exactly COUNT(*) for CPU clients. */
            "  COALESCE(SUM(s.member_count), 0), "
            "  COALESCE(SUM(s.num_relations), 0), "
            "  COALESCE(AVG(s.sieve_seconds), 0.0), "
            "  COALESCE(SUM(s.num_relations), 0) * 1.0 "
            "    / NULLIF(SUM(s.sieve_seconds), 0), "
            "  COALESCE(SUM(s.sieve_seconds), 0.0), "
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
                    cc->rel_per_sec         = sqlite3_column_double(st, 7);
                    cc->sieve_seconds_total = sqlite3_column_double(st, 8);
                    const unsigned char *cur = sqlite3_column_text(st, 9);
                    if (cur)
                        snprintf(cc->current_workunit,
                                 sizeof(cc->current_workunit), "%s", cur);
                    const unsigned char *lc = sqlite3_column_text(st, 10);
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

int db_block_members(ggnfs_db_t *db, int64_t q_start, int64_t q_end,
                     int64_t **out_starts, int *out_n)
{
    if (!db || !out_starts || !out_n) return -1;
    *out_starts = NULL;
    *out_n      = 0;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->conn,
            "SELECT q_start FROM workunits "
            " WHERE q_start >= ?1 AND q_start < ?2 ORDER BY q_start;",
            -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_block_members: %s\n", sqlite3_errmsg(db->conn));
        return -1;
    }
    sqlite3_bind_int64(st, 1, q_start);
    sqlite3_bind_int64(st, 2, q_end);

    int64_t *v = NULL;
    int      n = 0, cap = 0, rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (n == cap) {
            int      ncap = cap ? cap * 2 : 32;
            int64_t *nv   = realloc(v, (size_t)ncap * sizeof(*nv));
            if (!nv) { free(v); sqlite3_finalize(st); return -1; }
            v = nv; cap = ncap;
        }
        v[n++] = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { free(v); return -1; }
    *out_starts = v;
    *out_n      = n;
    return 0;
}

/* Shared tail of both block resolutions: flip the submission row and hand back
 * the client id. 0 = resolved (caller continues), 1 = already resolved (a
 * no-op, treat as success), -1 = error. Caller holds the transaction. */
static int block_resolve_submission(ggnfs_db_t *db, int64_t submission_id,
                                    const char *new_status, const char *reason,
                                    int64_t num_relations_actual,
                                    char *client_out, size_t client_n)
{
    sqlite3_stmt *st = NULL;
    /* Guarding on verify_status='pending' makes a duplicate verifier pass a
     * no-op rather than a second round of attempt_count increments. */
    const char *sql = reason
        ? "UPDATE submissions SET verify_status = ?1, verify_reason = ?2 "
          "  WHERE id = ?3 AND verify_status = 'pending' RETURNING client_id;"
        : "UPDATE submissions SET verify_status = ?1, verify_reason = NULL,"
          "                       num_relations = ?2 "
          "  WHERE id = ?3 AND verify_status = 'pending' RETURNING client_id;";
    if (sqlite3_prepare_v2(db->conn, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, new_status, -1, SQLITE_STATIC);
    if (reason) sqlite3_bind_text (st, 2, reason, -1, SQLITE_STATIC);
    else        sqlite3_bind_int64(st, 2, num_relations_actual);
    sqlite3_bind_int64(st, 3, submission_id);

    int rc = sqlite3_step(st);
    int result;
    if (rc == SQLITE_ROW) {
        const unsigned char *cid = sqlite3_column_text(st, 0);
        snprintf(client_out, client_n, "%s", cid ? (const char *)cid : "");
        result = 0;
    } else if (rc == SQLITE_DONE) {
        result = 1;                     /* already resolved */
    } else {
        result = -1;
    }
    sqlite3_finalize(st);
    return result;
}

/* Requeue members: attempt_count++ then available-or-poisoned, scoped either
 * to the whole [q_start,q_end) range (starts == NULL) or to an explicit list
 * of member q_starts. Counts land in *req / *poi. Caller holds the txn. */
static int block_requeue_members(ggnfs_db_t *db, int64_t q_start, int64_t q_end,
                                 const int64_t *starts, int n_starts,
                                 int64_t max_attempts,
                                 int64_t *req, int64_t *poi)
{
    static const char *const sql_range =
        "UPDATE workunits "
        "   SET attempt_count = attempt_count + 1,"
        "       state = CASE WHEN attempt_count + 1 >= ?1"
        "                    THEN 'poisoned' ELSE 'available' END,"
        "       leased_at = NULL, leased_to = NULL, lease_expires = NULL,"
        "       completed_at = NULL "
        " WHERE q_start >= ?2 AND q_start < ?3 AND state = 'submitted' "
        "RETURNING state;";
    static const char *const sql_one =
        "UPDATE workunits "
        "   SET attempt_count = attempt_count + 1,"
        "       state = CASE WHEN attempt_count + 1 >= ?1"
        "                    THEN 'poisoned' ELSE 'available' END,"
        "       leased_at = NULL, leased_to = NULL, lease_expires = NULL,"
        "       completed_at = NULL "
        " WHERE q_start = ?2 AND state = 'submitted' "
        "RETURNING state;";

    int count = starts ? n_starts : 1;
    for (int i = 0; i < count; i++) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db->conn, starts ? sql_one : sql_range,
                               -1, &st, NULL) != SQLITE_OK)
            return -1;
        sqlite3_bind_int64(st, 1, max_attempts);
        if (starts) {
            sqlite3_bind_int64(st, 2, starts[i]);
        } else {
            sqlite3_bind_int64(st, 2, q_start);
            sqlite3_bind_int64(st, 3, q_end);
        }
        int rc;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            const unsigned char *s = sqlite3_column_text(st, 0);
            if (s && strcmp((const char *)s, "poisoned") == 0) { if (poi) (*poi)++; }
            else                                               { if (req) (*req)++; }
        }
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return -1;
    }
    return 0;
}

/* Load a block's range and member_count from a submission's block_id.
 * 0 found, 1 missing, -1 error. Caller holds the txn. */
static int block_range_for_submission(ggnfs_db_t *db, int64_t submission_id,
                                      char *blk_id, size_t blk_id_n,
                                      int64_t *qs, int64_t *qe, int64_t *members)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->conn,
            "SELECT b.id, b.q_start, b.q_end, b.member_count "
            "  FROM submissions s JOIN gpu_blocks b ON b.id = s.block_id "
            " WHERE s.id = ?1;",
            -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(st, 1, submission_id);
    int rc = sqlite3_step(st);
    int result;
    if (rc == SQLITE_ROW) {
        const unsigned char *id = sqlite3_column_text(st, 0);
        snprintf(blk_id, blk_id_n, "%s", id ? (const char *)id : "");
        *qs      = sqlite3_column_int64(st, 1);
        *qe      = sqlite3_column_int64(st, 2);
        *members = sqlite3_column_int64(st, 3);
        result = 0;
    } else {
        result = (rc == SQLITE_DONE) ? 1 : -1;
    }
    sqlite3_finalize(st);
    return result;
}

static int block_set_state(ggnfs_db_t *db, const char *blk_id, const char *state)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->conn,
            "UPDATE gpu_blocks SET state = ?1 WHERE id = ?2;",
            -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, state,   -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, blk_id,  -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_block_verify_pass(ggnfs_db_t *db, int64_t submission_id,
                         int64_t num_relations_actual,
                         const int64_t *empty_q_starts, int n_empty,
                         int64_t max_attempts, int64_t now_unix,
                         int64_t *out_requeued, int64_t *out_poisoned)
{
    (void)now_unix;
    if (out_requeued) *out_requeued = 0;
    if (out_poisoned) *out_poisoned = 0;
    if (!db) return -1;

    if (sqlite3_exec(db->conn, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK)
        return -1;

    int           result = -1;
    sqlite3_stmt *st     = NULL;
    char          client[128] = {0};
    char          blk_id[64]  = {0};
    int64_t       qs = 0, qe = 0, members = 0;
    int64_t       req = 0, poi = 0;

    int r = block_range_for_submission(db, submission_id, blk_id, sizeof(blk_id),
                                       &qs, &qe, &members);
    if (r != 0) { result = (r == 1) ? 0 : -1; goto done; }

    r = block_resolve_submission(db, submission_id, "passed", NULL,
                                 num_relations_actual, client, sizeof(client));
    if (r != 0) { result = (r == 1) ? 0 : -1; goto done; }

    /* Members that produced nothing go back on the pile, charged for their own
     * sub-range. Do this BEFORE the blanket verify below so the two cannot
     * both claim the same row. */
    if (n_empty > 0 && empty_q_starts) {
        if (block_requeue_members(db, qs, qe, empty_q_starts, n_empty,
                                  max_attempts, &req, &poi) != 0)
            goto done;
    }

    if (sqlite3_prepare_v2(db->conn,
            "UPDATE workunits SET state = 'verified' "
            " WHERE q_start >= ?1 AND q_start < ?2 AND state = 'submitted';",
            -1, &st, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_int64(st, 1, qs);
    sqlite3_bind_int64(st, 2, qe);
    if (sqlite3_step(st) != SQLITE_DONE) goto done;
    int64_t verified = sqlite3_changes(db->conn);
    sqlite3_finalize(st); st = NULL;

    if (block_set_state(db, blk_id, "verified") != 0) goto done;

    /* total_workunits advances by members actually verified, not by 1: a
     * block IS its members, and counting it as one submission is what made the
     * dashboard understate a card by a factor of member_count. */
    if (sqlite3_prepare_v2(db->conn,
            "UPDATE clients "
            "   SET total_relations = total_relations + ?1,"
            "       total_workunits = total_workunits + ?2 "
            " WHERE id = ?3;",
            -1, &st, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_int64(st, 1, num_relations_actual);
    sqlite3_bind_int64(st, 2, verified);
    sqlite3_bind_text (st, 3, client, -1, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_DONE) goto done;

    result = 0;

done:
    if (st) sqlite3_finalize(st);
    if (result == 0) {
        if (sqlite3_exec(db->conn, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
            sqlite3_exec(db->conn, "ROLLBACK;", NULL, NULL, NULL);
            return -1;
        }
        if (out_requeued) *out_requeued = req;
        if (out_poisoned) *out_poisoned = poi;
    } else {
        fprintf(stderr, "db_block_verify_pass: %s\n", sqlite3_errmsg(db->conn));
        sqlite3_exec(db->conn, "ROLLBACK;", NULL, NULL, NULL);
    }
    return result;
}

int db_block_verify_fail(ggnfs_db_t *db, int64_t submission_id,
                         const char *reason, int64_t max_attempts,
                         int64_t now_unix,
                         int64_t *out_requeued, int64_t *out_poisoned)
{
    (void)now_unix;
    if (out_requeued) *out_requeued = 0;
    if (out_poisoned) *out_poisoned = 0;
    if (!db) return -1;

    if (sqlite3_exec(db->conn, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK)
        return -1;

    int           result = -1;
    sqlite3_stmt *st     = NULL;
    char          client[128] = {0};
    char          blk_id[64]  = {0};
    int64_t       qs = 0, qe = 0, members = 0;
    int64_t       req = 0, poi = 0;

    int r = block_range_for_submission(db, submission_id, blk_id, sizeof(blk_id),
                                       &qs, &qe, &members);
    if (r != 0) { result = (r == 1) ? 0 : -1; goto done; }

    r = block_resolve_submission(db, submission_id, "failed",
                                 reason ? reason : "unspecified", 0,
                                 client, sizeof(client));
    if (r != 0) { result = (r == 1) ? 0 : -1; goto done; }

    /* Whole block: a parse or norm failure is evidence about the data, and the
     * file spans every member, so there is no way to attribute it more
     * narrowly. Each member takes exactly one strike — what stops that from
     * mass-poisoning a contiguous region is db_block_lease's attempt ceiling,
     * which drops a twice-failed range out of block eligibility well before
     * max_attempts. */
    if (block_requeue_members(db, qs, qe, NULL, 0, max_attempts, &req, &poi) != 0)
        goto done;

    if (block_set_state(db, blk_id, "failed") != 0) goto done;

    if (sqlite3_prepare_v2(db->conn,
            "UPDATE clients SET total_failures = total_failures + 1 WHERE id = ?1;",
            -1, &st, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_text(st, 1, client, -1, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_DONE) goto done;

    result = 0;

done:
    if (st) sqlite3_finalize(st);
    if (result == 0) {
        if (sqlite3_exec(db->conn, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
            sqlite3_exec(db->conn, "ROLLBACK;", NULL, NULL, NULL);
            return -1;
        }
        if (out_requeued) *out_requeued = req;
        if (out_poisoned) *out_poisoned = poi;
    } else {
        fprintf(stderr, "db_block_verify_fail: %s\n", sqlite3_errmsg(db->conn));
        sqlite3_exec(db->conn, "ROLLBACK;", NULL, NULL, NULL);
    }
    return result;
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
