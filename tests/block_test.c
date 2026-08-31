/* block_test.c — unit tests for the GPU block layer in db.c.
 *
 * Links db.o directly: no HTTP, no server, no network. Each test builds a
 * throwaway SQLite jobdir under /tmp and drives the API.
 *
 * Every geometry test runs in BOTH scan directions. That is deliberate and
 * load-bearing: db_block_lease stores its candidate run in SCAN order, so an
 * index naming the low end of a descending run names the HIGH end of an
 * ascending one. Getting it wrong inverts q_start/q_end on every block, which
 * passes a smoke test and then fails verification on every band. Both
 * plausible sign errors were introduced deliberately during development and
 * each produced 71 failures here.
 *
 *   make test        (or: cc -I. -Ivendor tests/block_test.c db.o \
 *                          vendor/sqlite3.o -lpthread -lm -ldl)
 */
#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(cond, ...) do { checks++; if (!(cond)) { \
    fails++; printf("  FAIL %s:%d: ", __func__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *JOB = "testjob0";
#define CEIL 2
#define MAXA 5

/* Every geometry test runs in BOTH scan directions. That is the whole point of
 * this file after the direction change: the run array is stored in scan order,
 * so an index that names the low end going down names the HIGH end going up.
 * A sign error there inverts q_start/q_end on every block — which still passes
 * a smoke test and then fails verification on every single band. */
static int g_desc = 1;
static const char *dirname_(void) { return g_desc ? "desc" : "asc"; }

/* Geometry of a block of `k` members taken from the scan's leading edge of a
 * table holding `n` rows of width `w` starting at `q0`. Descending, the leading
 * edge is the top; ascending, the bottom. */
static void expect_block(int64_t q0, int n, int64_t w, int64_t k,
                         int64_t *eq_start, int64_t *eq_end)
{
    if (g_desc) { *eq_end = q0 + (int64_t)n * w; *eq_start = *eq_end - k * w; }
    else        { *eq_start = q0;                *eq_end   = q0 + k * w; }
}

/* q_start of the row sitting `m` rows in from the scan's leading edge. */
static int64_t row_from_lead(int64_t q0, int n, int64_t w, int m)
{
    return g_desc ? q0 + (int64_t)(n - 1 - m) * w : q0 + (int64_t)m * w;
}

/* Build a fresh jobdir DB with `n` workunits of `w` width from q0. */
static ggnfs_db_t *fresh(const char *path, int64_t q0, int64_t w, int n)
{
    remove(path);
    ggnfs_db_t *db = db_open(path);
    if (!db) { fprintf(stderr, "open %s failed\n", path); exit(1); }
    for (int i = 0; i < n; i++) {
        char id[64];
        snprintf(id, sizeof(id), "wu-%s-%06d", JOB, i);
        if (db_workunit_insert(db, id, q0 + (int64_t)i * w, w, 'r', "cpu", 1000) != 0)
            { fprintf(stderr, "insert failed\n"); exit(1); }
    }
    return db;
}

/* Raw SQL helpers so tests can assert on state the API doesn't expose. */
#include "vendor/sqlite3.h"
static sqlite3 *raw(const char *path)
{
    sqlite3 *c = NULL;
    if (sqlite3_open(path, &c) != SQLITE_OK) exit(1);
    return c;
}
static int64_t scalar(sqlite3 *c, const char *sql)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(c, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "sql: %s -- %s\n", sql, sqlite3_errmsg(c)); exit(1);
    }
    int64_t v = -999;
    if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}
static void run(sqlite3 *c, const char *sql)
{
    char *e = NULL;
    if (sqlite3_exec(c, sql, NULL, NULL, &e) != SQLITE_OK) {
        fprintf(stderr, "exec: %s -- %s\n", sql, e ? e : "?"); exit(1);
    }
}

/* ---- 1. happy path: lease / renew / submit --------------------------- */
static void t_basic(void)
{
    const char *P = "/tmp/blk1.db";
    ggnfs_db_t *db = fresh(P, 1000000, 1000, 200);
    db_block_t b;

    /* target 20000 over 1000-wide rows = 20 members, descending from the top */
    int rc = db_block_lease(db, JOB, "gpu-a", 20000, 5000, 64, CEIL, g_desc, 3600, 5000, &b);
    CK(rc == 0, "lease rc=%d", rc);
    CK(b.member_count == 20, "member_count=%lld", (long long)b.member_count);
    CK(b.q_end - b.q_start == 20000, "width=%lld", (long long)(b.q_end - b.q_start));
    CK(b.side == 'r', "side=%c", b.side);
    int64_t eqs, eqe;
    expect_block(1000000, 200, 1000, 20, &eqs, &eqe);
    CK(b.q_start == eqs && b.q_end == eqe, "[%lld,%lld) want [%lld,%lld) (%s)",
       (long long)b.q_start, (long long)b.q_end,
       (long long)eqs, (long long)eqe, dirname_());
    /* The anchor is always the LOWEST-q member, whichever way the scan ran. */
    char want[64];
    snprintf(want, sizeof(want), "wu-%s-%06d", JOB, (int)((eqs - 1000000) / 1000));
    CK(strcmp(b.anchor_wu_id, want) == 0, "anchor=%s want=%s", b.anchor_wu_id, want);

    sqlite3 *c = raw(P);
    CK(scalar(c, "SELECT COUNT(*) FROM workunits WHERE state='leased'") == 20,
       "20 members leased");
    CK(scalar(c, "SELECT COUNT(*) FROM workunits WHERE state='available'") == 180,
       "180 still available");

    /* idempotency: same client asking again gets the SAME block back */
    db_block_t b2;
    rc = db_block_lease(db, JOB, "gpu-a", 20000, 5000, 64, CEIL, g_desc, 3600, 5100, &b2);
    CK(rc == 0, "re-lease rc=%d", rc);
    CK(strcmp(b2.id, b.id) == 0, "same block id");
    CK(scalar(c, "SELECT COUNT(*) FROM workunits WHERE state='leased'") == 20,
       "still only 20 leased after retry");
    CK(scalar(c, "SELECT lease_expires FROM gpu_blocks WHERE id='blk-testjob0-000000'")
       == 5100 + 3600, "block lease pushed out");

    /* a DIFFERENT client gets the next block down, not the same one */
    db_block_t b3;
    rc = db_block_lease(db, JOB, "gpu-b", 20000, 5000, 64, CEIL, g_desc, 3600, 5200, &b3);
    CK(rc == 0, "second client rc=%d", rc);
    CK(g_desc ? (b3.q_end == b.q_start) : (b3.q_start == b.q_end),
       "second block abuts the first: b=[%lld,%lld) b3=[%lld,%lld)",
       (long long)b.q_start, (long long)b.q_end,
       (long long)b3.q_start, (long long)b3.q_end);
    CK(strcmp(b3.id, b.id) != 0, "distinct block id");

    /* renew */
    int64_t n = 0;
    rc = db_block_renew(db, b.anchor_wu_id, "gpu-a", 3600, 6000, &n);
    CK(rc == 0 && n == 20, "renew rc=%d n=%lld", rc, (long long)n);
    /* wrong owner must not renew */
    rc = db_block_renew(db, b.anchor_wu_id, "gpu-zzz", 3600, 6000, &n);
    CK(rc == 1, "renew by wrong owner rc=%d", rc);

    /* submit */
    rc = db_block_submit(db, b.anchor_wu_id, "gpu-a", "/tmp/x.dat", "deadbeef",
                         95600, 378.0, 7000);
    CK(rc == 0, "submit rc=%d", rc);
    CK(scalar(c, "SELECT COUNT(*) FROM workunits WHERE state='submitted'") == 20,
       "20 submitted");
    CK(scalar(c, "SELECT member_count FROM submissions") == 20, "sub member_count");
    CK(scalar(c, "SELECT q_width FROM submissions") == 20000, "sub q_width");
    CK(scalar(c, "SELECT COUNT(*) FROM submissions WHERE block_id IS NOT NULL") == 1,
       "sub block_id set");
    CK(scalar(c, "SELECT COUNT(*) FROM gpu_blocks WHERE state='submitted'") == 1,
       "block submitted");
    /* double submit is refused */
    rc = db_block_submit(db, b.anchor_wu_id, "gpu-a", "/tmp/x.dat", "deadbeef",
                         1, 1.0, 7001);
    CK(rc == 1, "double submit rc=%d", rc);

    /* release the other client's block */
    rc = db_block_release(db, b3.anchor_wu_id, "gpu-b");
    CK(rc == 0, "release rc=%d", rc);
    CK(scalar(c, "SELECT COUNT(*) FROM workunits WHERE state='available'") == 180,
       "released members back to available");
    CK(scalar(c, "SELECT MAX(attempt_count) FROM workunits") == 0,
       "release does NOT increment attempt_count");

    sqlite3_close(c); db_close(db);
}

/* ---- 2. contiguity: a hole breaks the run ---------------------------- */
static void t_hole(void)
{
    const char *P = "/tmp/blk2.db";
    ggnfs_db_t *db = fresh(P, 0, 1000, 100);
    sqlite3 *c = raw(P);
    /* Punch out the row 4 in from the scan's leading edge. The 4 rows between
     * it and that edge are only 4000 wide, so with min=5000 that run must be
     * discarded and the block must land on the far side of the hole. */
    int64_t hole = row_from_lead(0, 100, 1000, 4);
    char sql[128];
    snprintf(sql, sizeof(sql), "UPDATE workunits SET state='leased',"
             " leased_to='cpu-x' WHERE q_start = %lld;", (long long)hole);
    run(c, sql);
    db_block_t b;
    int rc = db_block_lease(db, JOB, "gpu-a", 20000, 5000, 64, CEIL, g_desc, 3600, 5000, &b);
    CK(rc == 0, "lease rc=%d", rc);
    int64_t eqs = g_desc ? hole - 20000 : hole + 1000;
    int64_t eqe = g_desc ? hole         : hole + 1000 + 20000;
    CK(b.q_start == eqs && b.q_end == eqe,
       "block sits beyond the hole at %lld: [%lld,%lld) want [%lld,%lld) (%s)",
       (long long)hole, (long long)b.q_start, (long long)b.q_end,
       (long long)eqs, (long long)eqe, dirname_());
    CK(b.member_count == 20, "members=%lld", (long long)b.member_count);
    CK(!(hole >= b.q_start && hole < b.q_end), "block must not span the hole");
    sqlite3_close(c); db_close(db);
}

/* ---- 3. short tail: run above the hole IS taken when min allows ------ */
static void t_short_ok(void)
{
    const char *P = "/tmp/blk3.db";
    ggnfs_db_t *db = fresh(P, 0, 1000, 100);
    sqlite3 *c = raw(P);
    int64_t hole = row_from_lead(0, 100, 1000, 4);
    char sql[128];
    snprintf(sql, sizeof(sql), "UPDATE workunits SET state='leased',"
             " leased_to='cpu-x' WHERE q_start = %lld;", (long long)hole);
    run(c, sql);
    db_block_t b;
    /* min 3000 now accepts the 4-row run at the leading edge */
    int rc = db_block_lease(db, JOB, "gpu-a", 20000, 3000, 64, CEIL, g_desc, 3600, 5000, &b);
    CK(rc == 0, "lease rc=%d", rc);
    int64_t eqs, eqe;
    expect_block(0, 100, 1000, 4, &eqs, &eqe);
    CK(b.q_start == eqs && b.q_end == eqe, "leading run taken: [%lld,%lld) want [%lld,%lld) (%s)",
       (long long)b.q_start, (long long)b.q_end,
       (long long)eqs, (long long)eqe, dirname_());
    CK(b.member_count == 4, "members=%lld", (long long)b.member_count);
    sqlite3_close(c); db_close(db);
}

/* ---- 4. attempt ceiling excludes rows and breaks runs ---------------- */
static void t_ceiling(void)
{
    const char *P = "/tmp/blk4.db";
    ggnfs_db_t *db = fresh(P, 0, 1000, 100);
    sqlite3 *c = raw(P);
    /* The row 9 in from the scan's leading edge has taken 2 block-scale
     * strikes and is no longer block-eligible, so it breaks the run. */
    int64_t bar = row_from_lead(0, 100, 1000, 9);
    char sql[160], q1[160];
    snprintf(sql, sizeof(sql),
             "UPDATE workunits SET attempt_count=2 WHERE q_start=%lld;", (long long)bar);
    run(c, sql);
    snprintf(q1, sizeof(q1), "SELECT COUNT(*) FROM workunits "
             "WHERE q_start=%lld AND state='available'", (long long)bar);

    db_block_t b;
    /* The 9 rows between it and the leading edge are 9000 wide, which clears
     * min=5000, so that run is taken and the block stops at the barrier. */
    int rc = db_block_lease(db, JOB, "gpu-a", 20000, 5000, 64, CEIL, g_desc, 3600, 5000, &b);
    CK(rc == 0, "lease rc=%d", rc);
    int64_t eqs, eqe;
    expect_block(0, 100, 1000, 9, &eqs, &eqe);
    CK(b.q_start == eqs && b.q_end == eqe,
       "run stops at the ceiling row: [%lld,%lld) want [%lld,%lld) (%s)",
       (long long)b.q_start, (long long)b.q_end,
       (long long)eqs, (long long)eqe, dirname_());
    CK(b.member_count == 9, "members=%lld", (long long)b.member_count);
    CK(scalar(c, q1) == 1, "over-ceiling row left available for individual leasing");

    /* Now the interesting half: with min=12000 that 9000-wide run is too
     * short, so the scan must skip PAST it and the barrier row entirely. */
    db_block_t bs;
    rc = db_block_lease(db, JOB, "gpu-skip", 20000, 12000, 64, CEIL, g_desc, 3600, 5000, &bs);
    CK(rc == 0, "skip-past lease rc=%d", rc);
    int64_t sqs = g_desc ? bar - 20000 : bar + 1000;
    int64_t sqe = g_desc ? bar         : bar + 1000 + 20000;
    CK(bs.q_start == sqs && bs.q_end == sqe,
       "skipped past the short run: [%lld,%lld) want [%lld,%lld) (%s)",
       (long long)bs.q_start, (long long)bs.q_end,
       (long long)sqs, (long long)sqe, dirname_());
    CK(scalar(c, q1) == 1, "ceiling row STILL untouched after the skip");
    /* attempt_count 1 is still fine */
    run(c, "UPDATE workunits SET attempt_count=1 WHERE q_start=50000;");
    db_block_t b2;
    rc = db_block_lease(db, JOB, "gpu-c", 60000, 5000, 64, CEIL, g_desc, 3600, 5000, &b2);
    CK(rc == 0, "lease2 rc=%d", rc);
    CK(b2.q_start <= 50000 && b2.q_end > 50000,
       "attempt_count=1 row still block-eligible: [%lld,%lld)",
       (long long)b2.q_start, (long long)b2.q_end);
    sqlite3_close(c); db_close(db);
}

/* ---- 5. no run wide enough -> rc 1 (caller falls back) --------------- */
static void t_nofit(void)
{
    const char *P = "/tmp/blk5.db";
    ggnfs_db_t *db = fresh(P, 0, 1000, 3);
    db_block_t b;
    int rc = db_block_lease(db, JOB, "gpu-a", 20000, 5000, 64, CEIL, g_desc, 3600, 5000, &b);
    CK(rc == 1, "too little work -> rc=%d (want 1)", rc);
    /* and nothing was leased as a side effect */
    sqlite3 *c = raw(P);
    CK(scalar(c, "SELECT COUNT(*) FROM workunits WHERE state='available'") == 3,
       "no rows consumed on rc=1");
    CK(scalar(c, "SELECT COUNT(*) FROM gpu_blocks") == 0, "no block row created");
    sqlite3_close(c); db_close(db);
}

/* ---- 6. expiry: every member takes exactly ONE strike ---------------- */
static void t_expire(void)
{
    const char *P = "/tmp/blk6.db";
    ggnfs_db_t *db = fresh(P, 0, 1000, 100);
    db_block_t b;
    CK(db_block_lease(db, JOB, "gpu-a", 20000, 5000, 64, CEIL, g_desc, 100, 5000, &b) == 0, "lease");

    int64_t blocks = 0, req = 0, poi = 0;
    /* now well past lease_expires (5000+100) */
    CK(db_block_expire_sweep(db, 9999, MAXA, &blocks, &req, &poi) == 0, "sweep");
    CK(blocks == 1, "blocks=%lld", (long long)blocks);
    CK(req == 20, "requeued=%lld", (long long)req);
    CK(poi == 0, "poisoned=%lld", (long long)poi);

    sqlite3 *c = raw(P);
    CK(scalar(c, "SELECT COUNT(*) FROM workunits WHERE state='available'") == 100,
       "all back to available");
    CK(scalar(c, "SELECT COUNT(*) FROM workunits WHERE attempt_count=1") == 20,
       "exactly the 20 members took a strike");
    CK(scalar(c, "SELECT MAX(attempt_count) FROM workunits") == 1, "one strike, not two");
    CK(scalar(c, "SELECT COUNT(*) FROM gpu_blocks WHERE state='expired'") == 1,
       "block marked expired");

    /* THE ORDERING TEST: the per-workunit sweep must find nothing left to do,
     * so members are not double-incremented. */
    int64_t r2 = 0, p2 = 0;
    CK(db_lease_expire_sweep(db, 9999, MAXA, &r2, &p2) == 0, "row sweep");
    CK(r2 == 0 && p2 == 0, "row sweep after block sweep: requeued=%lld poisoned=%lld",
       (long long)r2, (long long)p2);
    CK(scalar(c, "SELECT MAX(attempt_count) FROM workunits") == 1,
       "still one strike after both sweeps");
    /* sweeping again is a no-op (block is no longer 'leased') */
    CK(db_block_expire_sweep(db, 99999, MAXA, &blocks, &req, &poi) == 0, "sweep2");
    CK(blocks == 0 && req == 0, "second block sweep is a no-op");
    sqlite3_close(c); db_close(db);
}

/* ---- 7. the ceiling caps the poisoning blast radius ------------------ */
static void t_blast_radius(void)
{
    const char *P = "/tmp/blk7.db";
    ggnfs_db_t *db = fresh(P, 0, 1000, 100);
    sqlite3 *c = raw(P);
    db_block_t b;
    int64_t t = 5000;

    /* A host that dies every time. Cycle it until blocks stop forming. */
    int cycles = 0;
    for (; cycles < 10; cycles++) {
        int rc = db_block_lease(db, JOB, "gpu-dead", 20000, 5000, 64, CEIL, g_desc, 100, t, &b);
        if (rc != 0) break;
        t += 1000;
        CK(db_block_expire_sweep(db, t, MAXA, NULL, NULL, NULL) == 0, "sweep");
        CK(db_lease_expire_sweep(db, t, MAXA, NULL, NULL) == 0, "rowsweep");
    }
    /* With CEIL=2, a given range can absorb at most 2 block-scale strikes.
     * The invariant that matters: NOTHING is poisoned by block failures alone. */
    CK(scalar(c, "SELECT COUNT(*) FROM workunits WHERE state='poisoned'") == 0,
       "block-scale failures alone must never poison (got %lld)",
       scalar(c, "SELECT COUNT(*) FROM workunits WHERE state='poisoned'"));
    CK(scalar(c, "SELECT MAX(attempt_count) FROM workunits") <= CEIL,
       "no row exceeds the ceiling from block strikes: max=%lld",
       (long long)scalar(c, "SELECT MAX(attempt_count) FROM workunits"));
    CK(scalar(c, "SELECT COUNT(*) FROM workunits WHERE state='available'") == 100,
       "every row still workable by individual lease");
    printf("  (blast radius: %d block cycles, max attempt_count=%lld, poisoned=%lld)\n",
           cycles, scalar(c, "SELECT MAX(attempt_count) FROM workunits"),
           scalar(c, "SELECT COUNT(*) FROM workunits WHERE state='poisoned'"));
    sqlite3_close(c); db_close(db);
}

/* ---- 8. non-uniform widths: sizing is by q-width, not member count --- */
static void t_mixed_width(void)
{
    const char *P = "/tmp/blk8.db";
    /* One 100000-wide row (as `extend --qrange=100000` would leave) sitting at
     * the scan's leading edge, with ordinary 1000-wide rows behind it. */
    int64_t wide_q = g_desc ? 50000 : 0;
    int64_t narrow_q0 = g_desc ? 0 : 100000;
    remove(P);
    ggnfs_db_t *db = db_open(P);
    CK(db != NULL, "open");
    for (int i = 0; i < 50; i++) {
        char id[64];
        snprintf(id, sizeof(id), "wu-%s-%06d", JOB, i);
        CK(db_workunit_insert(db, id, narrow_q0 + (int64_t)i * 1000, 1000,
                              'r', "cpu", 1000) == 0, "narrow insert");
    }
    CK(db_workunit_insert(db, "wu-testjob0-000050", wide_q, 100000, 'r', "cpu", 1000) == 0,
       "wide insert");
    db_block_t b;
    int rc = db_block_lease(db, JOB, "gpu-a", 20000, 5000, 64, CEIL, g_desc, 3600, 5000, &b);
    CK(rc == 0, "lease rc=%d", rc);
    /* The wide row alone already exceeds target: one member, not 20. A
     * member-count rule would have taken 20 rows = 100000+19000 of work. */
    CK(b.member_count == 1, "members=%lld (want 1)", (long long)b.member_count);
    CK(b.q_start == wide_q && b.q_end == wide_q + 100000, "[%lld,%lld) want [%lld,%lld)",
       (long long)b.q_start, (long long)b.q_end,
       (long long)wide_q, (long long)(wide_q + 100000));
    sqlite3_close(raw(P)); db_close(db);
}

/* ---- 9. max_members clamp ------------------------------------------- */
static void t_clamp(void)
{
    const char *P = "/tmp/blk9.db";
    ggnfs_db_t *db = fresh(P, 0, 1000, 100);
    db_block_t b;
    /* target wants 50 members, clamp allows 8 */
    int rc = db_block_lease(db, JOB, "gpu-a", 50000, 5000, 8, CEIL, g_desc, 3600, 5000, &b);
    CK(rc == 0, "lease rc=%d", rc);
    CK(b.member_count == 8, "clamped to %lld", (long long)b.member_count);
    int64_t eqs, eqe;
    expect_block(0, 100, 1000, 8, &eqs, &eqe);
    CK(b.q_start == eqs && b.q_end == eqe, "[%lld,%lld) want [%lld,%lld) (%s)",
       (long long)b.q_start, (long long)b.q_end,
       (long long)eqs, (long long)eqe, dirname_());
    sqlite3 *c = raw(P);
    CK(scalar(c, "SELECT COUNT(*) FROM workunits WHERE state='leased'") == 8,
       "only 8 rows leased");
    sqlite3_close(c); db_close(db);
}

/* ---- 10. a client holding a plain lease must not also get a block ---- */
static void t_no_double_lease(void)
{
    const char *P = "/tmp/blk10.db";
    ggnfs_db_t *db = fresh(P, 0, 1000, 100);
    db_lease_result_t r;
    /* Slot takes a single workunit (as it would when no run qualified). */
    CK(db_lease(db, "gpu-a", 3600, 5000, 0, "gpu", &r) == 0, "plain lease");

    /* Now a run IS available and the same client asks for a block: it must be
     * refused, so db_lease's one-live-lease guard stays the single authority. */
    db_block_t b;
    int rc = db_block_lease(db, JOB, "gpu-a", 20000, 5000, 64, CEIL, g_desc, 3600, 5001, &b);
    CK(rc == 1, "block refused while holding a plain lease (rc=%d)", rc);

    sqlite3 *c = raw(P);
    CK(scalar(c, "SELECT COUNT(*) FROM gpu_blocks") == 0, "no block row created");
    CK(scalar(c, "SELECT COUNT(*) FROM workunits WHERE state='leased'") == 1,
       "still exactly one leased row");
    /* And db_lease hands the same workunit back, as it always did. */
    db_lease_result_t r2;
    CK(db_lease(db, "gpu-a", 3600, 5002, 0, "gpu", &r2) == 0, "re-lease");
    CK(strcmp(r2.id, r.id) == 0, "same workunit returned");

    /* Once it is submitted the client is free again. */
    CK(db_submit(db, r.id, "gpu-a", "/tmp/x", "sha", 1, 1.0, 5003) == 0, "submit");
    rc = db_block_lease(db, JOB, "gpu-a", 20000, 5000, 64, CEIL, g_desc, 3600, 5004, &b);
    CK(rc == 0, "block granted once the plain lease is resolved (rc=%d)", rc);
    sqlite3_close(c); db_close(db);
}

/* ---- 11. submit must prove ownership ---- */
static void t_submit_ownership(void)
{
    const char *P = "/tmp/blk11.db";
    ggnfs_db_t *db = fresh(P, 0, 1000, 10);
    db_lease_result_t r;
    CK(db_lease(db, "owner", 3600, 5000, 0, "cpu", &r) == 0, "lease");
    /* A stale client whose lease was reissued must not submit against it. */
    CK(db_submit(db, r.id, "impostor", "/tmp/bad", "sha", 9, 1.0, 5001) == 1,
       "submit by non-owner must be refused");
    sqlite3 *c = raw(P);
    CK(scalar(c, "SELECT COUNT(*) FROM submissions") == 0, "no submission row");
    CK(scalar(c, "SELECT COUNT(*) FROM workunits WHERE state='leased'") == 1,
       "workunit still leased to the owner");
    CK(db_submit(db, r.id, "owner", "/tmp/good", "sha", 9, 1.0, 5002) == 0,
       "owner submit succeeds");
    sqlite3_close(c); db_close(db);
}

/* ---- 12. block ids never collide, however many are issued ---- */
static void t_block_ids(void)
{
    const char *P = "/tmp/blk12.db";
    ggnfs_db_t *db = fresh(P, 0, 1000, 600);   /* 20 blocks x 20 + headroom */
    db_block_t b;
    for (int i = 0; i < 20; i++) {
        char cid[32]; snprintf(cid, sizeof(cid), "gpu-%d", i);
        CK(db_block_lease(db, JOB, cid, 20000, 5000, 64, CEIL, g_desc, 3600, 5000, &b) == 0,
           "lease %d", i);
    }
    sqlite3 *c = raw(P);
    CK(scalar(c, "SELECT COUNT(*) FROM gpu_blocks") == 20, "20 blocks");
    CK(scalar(c, "SELECT COUNT(DISTINCT id) FROM gpu_blocks") == 20, "ids distinct");
    /* Deleting a middle row must not let the next id be recycled. */
    run(c, "DELETE FROM gpu_blocks WHERE id='blk-testjob0-000005';");
    db_block_t b2;
    CK(db_block_lease(db, JOB, "gpu-new", 20000, 5000, 64, CEIL, g_desc, 3600, 5000, &b2) == 0,
       "lease after delete");
    CK(strcmp(b2.id, "blk-testjob0-000020") == 0,
       "next id continues past the highest in use, got %s", b2.id);
    sqlite3_close(c); db_close(db);
}

int main(void)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "basic lease/renew/submit/release", t_basic },
        { "contiguity: hole breaks run",      t_hole },
        { "short top run accepted when min allows", t_short_ok },
        { "attempt ceiling",                  t_ceiling },
        { "no run wide enough -> fallback",   t_nofit },
        { "expiry: one strike per member",    t_expire },
        { "ceiling caps poison blast radius", t_blast_radius },
        { "mixed widths sized by q-width",    t_mixed_width },
        { "max_members clamp",                t_clamp },
        { "no block while holding a plain lease", t_no_double_lease },
        { "submit proves ownership",          t_submit_ownership },
        { "block ids never recycle",          t_block_ids },
    };
    /* Both directions, same suite. The geometry helpers above turn "the top"
     * into "the scan's leading edge", so every fixture and expectation mirrors
     * with the scan instead of being hardcoded to descending. */
    int dirs[2] = { 1, 0 };
    for (int d = 0; d < 2; d++) {
        g_desc = dirs[d];
        printf("\n######## scan direction: %s ########\n", dirname_());
        for (size_t i = 0; i < sizeof(tests)/sizeof(tests[0]); i++) {
            int before = fails;
            printf("== %s\n", tests[i].name);
            tests[i].fn();
            if (fails == before) printf("  ok\n");
        }
    }
    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
