/* Deterministic submission race regression: inspect the verifier's separate
 * SQLite connection at rename, before the file is published. No sleeps needed.
 * Also exercise failed publication and stale owners for CPU and GPU leases. */
#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks, failures, rename_calls;
static ggnfs_db_t *reader;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); failures++; \
} } while (0)

int __real_rename(const char *oldpath, const char *newpath);

static void expect_no_pending(void)
{
    db_pending_t pending;
    int rc = db_verify_next_pending(reader, &pending);
    CHECK(rc == 1);
    if (rc == 0) db_pending_free(&pending);
}

int __wrap_rename(const char *oldpath, const char *newpath)
{
    rename_calls++;
    /* This is exactly where a running verifier used to observe the pending
     * row and fail opening newpath. The transaction must hide it here. */
    expect_no_pending();
    return __real_rename(oldpath, newpath);
}

static void write_file(const char *path, const char *body)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    CHECK(fputs(body, f) >= 0);
    CHECK(fclose(f) == 0);
}

static void expect_file(const char *path, const char *expected)
{
    char buf[128] = {0};
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    if (!f) return;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    CHECK(n == strlen(expected));
    CHECK(strcmp(buf, expected) == 0);
    fclose(f);
}

static int submit(ggnfs_db_t *db, int block, const char *id,
                  const char *client, const char *stage, const char *final)
{
    return block ? db_block_submit(db, id, client, stage, final, "sha", 1, 1, 1001)
                 : db_submit(db, id, client, stage, final, "sha", 1, 1, 1001);
}

static void run_case(int block)
{
    char dir[] = "/tmp/submit-publish-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); exit(1); }
    char dbpath[256], stage[256], final[256], id[64];
    snprintf(dbpath, sizeof(dbpath), "%s/job.db", dir);
    snprintf(stage, sizeof(stage), "%s/result.tmp", dir);
    snprintf(final, sizeof(final), "%s/result.dat.zst", dir);
    ggnfs_db_t *db = db_open(dbpath);
    if (!db) exit(1);
    for (int i = 0; i < 2; i++) {
        snprintf(id, sizeof(id), "wu-testjob0-%06d", i);
        CHECK(db_workunit_insert(db, id, 100 + i * 10, 10, 'a', "cpu", 1000) == 0);
    }
    if (block) {
        db_block_t lease;
        CHECK(db_block_lease(db, "testjob0", "owner", 20, 20, 2, 2,
                             0, 3600, 1000, &lease) == 0);
        snprintf(id, sizeof(id), "%s", lease.anchor_wu_id);
    } else {
        db_lease_result_t lease;
        CHECK(db_lease(db, "owner", 3600, 1000, 0, "cpu", &lease) == 0);
        snprintf(id, sizeof(id), "%s", lease.id);
    }
    reader = db_open(dbpath);
    if (!reader) exit(1);
    rename_calls = 0;

    /* An unpublishable file must leave every lease intact for retry. */
    CHECK(submit(db, block, id, "owner", stage, final) == -1);
    CHECK(rename_calls == 1);
    expect_no_pending();
    db_workunit_counts_t counts;
    CHECK(db_workunit_counts(reader, &counts) == 0);
    CHECK(counts.leased == (block ? 2 : 1));
    CHECK(counts.submitted == 0);
    if (block) {
        int64_t members = 0;
        CHECK(db_block_renew(db, id, "owner", 3600, 1002, &members) == 0);
        CHECK(members == 2);
    } else {
        CHECK(db_renew_lease(db, id, "owner", 3600, 1002) == 0);
    }

    /* A rejected owner cannot overwrite an existing file or consume staging. */
    write_file(final, "existing");
    write_file(stage, "new result");
    CHECK(submit(db, block, id, "stale", stage, final) == 1);
    CHECK(rename_calls == 1);
    expect_file(final, "existing");
    expect_file(stage, "new result");
    expect_no_pending();
    CHECK(unlink(final) == 0);

    CHECK(submit(db, block, id, "owner", stage, final) == 0);
    CHECK(rename_calls == 2);
    CHECK(access(stage, F_OK) == -1);
    db_pending_t pending;
    int rc = db_verify_next_pending(reader, &pending);
    CHECK(rc == 0);
    if (rc == 0) {
        CHECK(strcmp(pending.file_path, final) == 0);
        expect_file(pending.file_path, "new result");
        db_pending_free(&pending);
    }
    /* A duplicate cannot overwrite the file now being verified. */
    write_file(stage, "duplicate");
    CHECK(submit(db, block, id, "owner", stage, final) == 1);
    CHECK(rename_calls == 2);
    expect_file(final, "new result");
    expect_file(stage, "duplicate");

    db_close(reader);
    reader = NULL;
    db_close(db);
    unlink(stage);
    unlink(final);
    unlink(dbpath);
    CHECK(rmdir(dir) == 0);
}

int main(void)
{
    run_case(0);
    run_case(1);
    printf("submission publication: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
