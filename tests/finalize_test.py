"""Offline integration tests for archive finalization and pull metadata."""
import hashlib
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
JOB = b'n: 123456789\nc0: 1\nc1: 1\nY0: 1\nY1: 1\n'
SHA = hashlib.sha256(JOB).hexdigest()
CPU = b'1,2:3:3\n'
GPU = b'-4,3:100000001,2:2,100000003\n'


def database(path, rows=()):
    path.parent.mkdir(parents=True, exist_ok=True)
    with sqlite3.connect(path) as db:
        db.executescript('CREATE TABLE meta(key TEXT, value TEXT);'
                         'CREATE TABLE submissions(id INTEGER PRIMARY KEY, file_path TEXT, verify_status TEXT);'
                         'CREATE TABLE workunits(id TEXT, state TEXT);'
                         'CREATE TABLE gpu_blocks(id TEXT, state TEXT);')
        db.execute('INSERT INTO meta VALUES (?, ?)', ('job_sha256', SHA))
        db.executemany('INSERT INTO submissions(file_path, verify_status) VALUES (?, ?)', rows)


@unittest.skipUnless(shutil.which('sqlite3') and shutil.which('zstd'), 'requires sqlite3 and zstd')
class FinalizeTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='finalize test ')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.job = self.base / 'job'
        self.out = self.base / 'yafu output'
        self.out.mkdir()
        (self.job / 'archive').mkdir(parents=True)
        self.snapshot = self.job / 'incoming/2026-09-17T00-00-00Z/job.db'
        database(self.snapshot, [('/server/rels/wu-cpu.dat', 'passed'),
                                 ('/server/rels/blk-gpu.dat.zst', 'passed'),
                                 ('/server/rels/wu-failed.dat', 'failed'),
                                 ('/server/rels/wu-pending.dat', 'pending')])
        files = self.snapshot.parent / 'files'
        files.mkdir()
        (files / f'{SHA}.job').write_bytes(JOB)
        (self.job / 'archive/wu-cpu.dat').write_bytes(CPU)
        (self.job / 'archive/blk-gpu.dat.zst').write_bytes(
            subprocess.run(['zstd', '-cq'], input=GPU, capture_output=True, check=True).stdout)
        (self.job / 'archive/wu-failed.dat').write_bytes(b'FAILED\n')
        (self.job / 'archive/wu-pending.dat').write_bytes(b'PENDING\n')
        (self.snapshot.parent / 'wu-unvalidated.dat').write_bytes(b'INCOMING\n')

    def run_finalize(self, *args, ok=True, umask=0o022):
        result = subprocess.run([str(ROOT / 'finalize-nfs.sh'), f'--jobdir={self.job}',
                                 f'--yafu-dir={self.out}', *args], capture_output=True, text=True, umask=umask)
        self.assertEqual(result.returncode == 0, ok, result.stdout + result.stderr)
        return result

    def test_archive_mixed_formats_and_latest_snapshot(self):
        database(self.job / 'incoming/2026-09-16T00-00-00Z/job.db')
        self.run_finalize('--phase=nc1')
        self.assertEqual((self.out / 'nfs.dat').read_bytes(), b'N 123456789\n' + CPU + GPU)
        self.assertEqual((self.out / 'nfs.job').read_bytes(), JOB)

    def test_check_does_not_write(self):
        self.run_finalize('--check')
        self.assertEqual(list(self.out.iterdir()), [])

    def test_large_b_omitted_without_truncation_or_prime_limits(self):
        keep = b'-1,4294967295:100000001:200000003\n'
        omit = b'2,4294967296:2:3\n3,18446744073709551615:2:3\n'
        source = self.job / 'archive/wu-cpu.dat'
        source.write_bytes(CPU + keep + omit)
        result = self.run_finalize()
        self.assertEqual((self.out / 'nfs.dat').read_bytes(), b'N 123456789\n' + CPU + keep + GPU)
        self.assertIn('omitted 2 relations', result.stderr)
        self.assertEqual(source.read_bytes(), CPU + keep + omit)

    def test_server_layout_and_duplicate_paths(self):
        shutil.move(self.job / 'archive', self.job / 'rels')
        shutil.move(self.snapshot, self.job / 'job.db')
        shutil.move(self.snapshot.parent / 'files', self.job / 'files')
        with sqlite3.connect(self.job / 'job.db') as db:
            db.execute("INSERT INTO submissions(file_path, verify_status) VALUES ('/server/rels/wu-cpu.dat', 'passed')")
        self.run_finalize()
        self.assertEqual((self.out / 'nfs.dat').read_bytes(), b'N 123456789\n' + CPU + GPU)

    def test_archive_preferred_over_rels_copy(self):
        (self.job / 'rels').mkdir()
        (self.job / 'rels/wu-cpu.dat').write_bytes(b'REPLACED\n')
        self.run_finalize()
        self.assertEqual((self.out / 'nfs.dat').read_bytes(), b'N 123456789\n' + CPU + GPU)

    def test_query_failure_never_falls_back(self):
        with sqlite3.connect(self.snapshot) as db:
            db.execute('DROP TABLE submissions')
        result = self.run_finalize(f'--jobdb={self.snapshot}', ok=False)
        self.assertIn('cannot select passed submissions', result.stderr)
        self.assertFalse((self.out / 'nfs.dat').exists())

    def test_no_passed_never_falls_back(self):
        with sqlite3.connect(self.snapshot) as db:
            db.execute("UPDATE submissions SET verify_status='failed'")
        self.run_finalize(ok=False)
        self.assertFalse((self.out / 'nfs.dat').exists())

    def test_wrong_job_file_and_existing_output_job(self):
        wrong = self.base / 'wrong.job'
        wrong.write_bytes(JOB + b'rlim: 100\n')
        self.run_finalize(f'--job-file={wrong}', ok=False)
        (self.out / 'nfs.job').write_bytes(wrong.read_bytes())
        self.run_finalize(ok=False)
        self.assertFalse((self.out / 'nfs.dat').exists())

    def test_corrupt_compression_preserves_previous_output(self):
        (self.out / 'nfs.dat').write_bytes(b'OLD DATA\n')
        (self.job / 'archive/blk-gpu.dat.zst').write_bytes(b'corrupt')
        self.run_finalize(ok=False)
        self.assertEqual((self.out / 'nfs.dat').read_bytes(), b'OLD DATA\n')
        self.assertEqual(list(self.out.glob('.nfs.dat.*')), [])

    def test_resume_preserves_filtered_relation_order(self):
        (self.out / 'nfs.job').write_bytes(JOB)
        old = b'N 123456789\n' + GPU + CPU
        (self.out / 'nfs.dat').write_bytes(old)
        for phase in ('nc2', 'nc3', 'ncr'):
            self.run_finalize(f'--phase={phase}')
            self.assertEqual((self.out / 'nfs.dat').read_bytes(), old)
        self.assertEqual(list(self.out.glob('nfs.dat.prev.*')), [])

    def test_resume_requires_existing_matching_data(self):
        self.run_finalize('--phase=nc2', ok=False)
        (self.out / 'nfs.job').write_bytes(JOB)
        (self.out / 'nfs.dat').write_bytes(b'N 999\n')
        self.run_finalize('--phase=nc3', ok=False)

    def test_success_preserves_old_data_backup(self):
        (self.out / 'nfs.dat').write_bytes(b'OLD\n')
        self.run_finalize()
        backups = list(self.out.glob('nfs.dat.prev.*'))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), b'OLD\n')

    def test_no_database_legacy_glob_includes_gpu(self):
        self.snapshot.unlink()
        (self.job / 'files').mkdir()
        (self.job / f'files/{SHA}.job').write_bytes(JOB)
        (self.job / 'archive/wu-failed.dat').unlink()
        (self.job / 'archive/wu-pending.dat').unlink()
        self.run_finalize()
        self.assertEqual((self.out / 'nfs.dat').read_bytes(), b'N 123456789\n' + CPU + GPU)

    def test_argument_validation(self):
        self.run_finalize('--phase=bad', ok=False)
        self.run_finalize('--threads=0', ok=False)

    def prepare_pull(self):
        remote = self.base / 'remote'
        (remote / 'rels').mkdir(parents=True)
        (remote / 'files').mkdir()
        (remote / f'files/{SHA}.job').write_bytes(JOB)
        database(remote / 'job.db', [('/server/rels/blk-gpu.dat', 'passed')])
        with sqlite3.connect(remote / 'job.db') as db:
            db.execute("INSERT INTO gpu_blocks VALUES ('blk-gpu', 'verified')")
        (remote / 'rels/blk-gpu.dat').write_bytes(GPU)
        mockbin = self.base / 'bin'
        mockbin.mkdir()
        # Run remote shell commands locally, including the real move-rels.sh
        # piped over stdin. No network or coordinator changes in these tests.
        (mockbin / 'ssh').write_text('#!/usr/bin/env python3\nimport os,sys\nos.execv("/bin/bash", ["bash", "-c", sys.argv[-1]])\n')
        (mockbin / 'rsync').write_text('#!/usr/bin/env python3\nimport shutil,sys\nshutil.copytree(sys.argv[-2].split(":", 1)[1], sys.argv[-1], dirs_exist_ok=True)\n')
        for script in mockbin.iterdir():
            script.chmod(0o755)
        return remote, mockbin

    def run_pull(self, remote, mockbin, local, ok=True):
        result = subprocess.run([str(ROOT / 'pull-rels.sh'), '--ssh-host=test',
                                 f'--remote-jobdir={remote}', f'--local-dir={local}', '--no-validate'],
                                env={**os.environ, 'PATH': f'{mockbin}:{os.environ["PATH"]}'},
                                capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, ok, result.stdout + result.stderr)
        return result

    def test_pull_includes_metadata_with_and_without_relations(self):
        remote, mockbin = self.prepare_pull()
        for index in range(2):
            local = self.base / f'pulled-{index}'
            self.run_pull(remote, mockbin, local)
            jobs = list(local.glob('incoming/*/files/*.job' if index == 0 else 'files/*.job'))
            if index == 1:
                self.assertEqual(list((local / 'incoming').iterdir()), [])
            self.assertEqual(len(jobs), 1)
            self.assertEqual(jobs[0].read_bytes(), JOB)
            self.assertEqual(len(list(local.glob('archive/*.dat'))), 1 if index == 0 else 0)
            self.assertFalse(list(self.base.glob('remote-staging/*')))

    def test_output_job_cannot_prove_identity_without_database(self):
        self.snapshot.unlink()
        (self.out / 'nfs.job').write_bytes(JOB)
        result = self.run_finalize(ok=False)
        self.assertIn('no matching .job', result.stderr)
        self.assertFalse((self.out / 'nfs.dat').exists())

    def test_output_job_can_be_authenticated_by_database(self):
        (self.snapshot.parent / f'files/{SHA}.job').unlink()
        (self.out / 'nfs.job').write_bytes(JOB)
        self.run_finalize()
        self.assertEqual((self.out / 'nfs.dat').read_bytes(), b'N 123456789\n' + CPU + GPU)

    def test_unusable_newer_snapshots_fall_back_with_warning(self):
        for stamp, content in [('2026-09-18', b'SQLite format 3\x00truncated'),
                               ('2026-09-19', b'')]:
            path = self.job / f'incoming/{stamp}/job.db'
            path.parent.mkdir()
            path.write_bytes(content)
        result = self.run_finalize()
        self.assertEqual(result.stderr.count('skipping unusable snapshot'), 2)
        self.assertIn(str(self.snapshot), result.stdout)
        self.assertEqual((self.out / 'nfs.dat').read_bytes(), b'N 123456789\n' + CPU + GPU)

    def test_all_snapshots_unusable_never_globs(self):
        self.snapshot.write_bytes(b'truncated')
        result = self.run_finalize(ok=False)
        self.assertIn('no usable database snapshot', result.stderr)
        self.assertFalse((self.out / 'nfs.dat').exists())

    def test_explicit_bad_database_does_not_fall_back(self):
        bad = self.job / 'bad.db'
        bad.write_bytes(b'truncated')
        result = self.run_finalize(f'--jobdb={bad}', ok=False)
        self.assertIn('cannot read job identity', result.stderr)

    def test_unexpected_submission_file_warns_and_skips(self):
        with sqlite3.connect(self.snapshot) as db:
            db.execute("INSERT INTO submissions(file_path, verify_status) VALUES ('/server/rels/notes.txt', 'passed')")
        (self.job / 'archive/notes.txt').write_text('not relations')
        result = self.run_finalize()
        self.assertIn('skipping unsupported relation file', result.stderr)
        self.assertEqual((self.out / 'nfs.dat').read_bytes(), b'N 123456789\n' + CPU + GPU)

    def test_empty_resume_file_has_diagnostic(self):
        (self.out / 'nfs.job').write_bytes(JOB)
        (self.out / 'nfs.dat').touch()
        result = self.run_finalize('--phase=ncr', ok=False)
        self.assertIn('empty or incomplete header', result.stderr)

    def test_check_run_does_not_require_binary_or_write(self):
        self.run_finalize('--check', '--run')
        self.assertEqual(list(self.out.iterdir()), [])

    def test_new_output_permissions_follow_umask(self):
        self.run_finalize(umask=0o027)
        self.assertEqual((self.out / 'nfs.dat').stat().st_mode & 0o777, 0o640)

    def test_replacement_and_backup_preserve_existing_permissions(self):
        old = self.out / 'nfs.dat'
        old.write_bytes(b'OLD\n')
        old.chmod(0o664)
        self.run_finalize(umask=0o077)
        backup, = self.out.glob('nfs.dat.prev.*')
        self.assertEqual(backup.read_bytes(), b'OLD\n')
        self.assertEqual(backup.stat().st_mode & 0o777, 0o664)
        self.assertEqual(old.stat().st_mode & 0o777, 0o664)

    def test_clean_assembly_has_no_stderr(self):
        self.assertEqual(self.run_finalize().stderr, '')

    def test_job_mismatch_reports_hashes_and_remediation(self):
        wrong = JOB + b'rlim: 123\n'
        (self.out / 'nfs.job').write_bytes(wrong)
        result = self.run_finalize(ok=False)
        self.assertIn(SHA, result.stderr)
        self.assertIn(hashlib.sha256(wrong).hexdigest(), result.stderr)
        self.assertIn('separate YAFU directory', result.stderr)

    def test_extensionless_cached_job_can_be_supplied_explicitly(self):
        cached = self.base / SHA
        cached.write_bytes(JOB)
        self.run_finalize(f'--job-file={cached}')
        self.assertEqual((self.out / 'nfs.job').read_bytes(), JOB)

    def test_missing_remote_job_leaves_relations_in_place(self):
        remote, mockbin = self.prepare_pull()
        (remote / f'files/{SHA}.job').unlink()
        self.run_pull(remote, mockbin, self.base / 'local', ok=False)
        self.assertEqual((remote / 'rels/blk-gpu.dat').read_bytes(), GPU)
        self.assertFalse(list(self.base.glob('remote-staging/*/*.dat')))

    def test_pull_remote_path_with_apostrophe(self):
        remote, mockbin = self.prepare_pull()
        quoted = self.base / "remote's job"
        remote.rename(quoted)
        local = self.base / 'local'
        self.run_pull(quoted, mockbin, local)
        self.assertEqual((local / 'archive/blk-gpu.dat').read_bytes(), GPU)

    def test_invalid_remote_job_identity_leaves_relations_in_place(self):
        remote, mockbin = self.prepare_pull()
        with sqlite3.connect(remote / 'job.db') as db:
            db.execute("DELETE FROM meta WHERE key='job_sha256'")
        result = self.run_pull(remote, mockbin, self.base / 'local', ok=False)
        self.assertIn('relations were not moved', result.stderr)
        self.assertEqual((remote / 'rels/blk-gpu.dat').read_bytes(), GPU)

    def test_wrong_remote_job_leaves_relations_in_place(self):
        remote, mockbin = self.prepare_pull()
        (remote / f'files/{SHA}.job').write_bytes(b'wrong job')
        self.run_pull(remote, mockbin, self.base / 'local', ok=False)
        self.assertEqual((remote / 'rels/blk-gpu.dat').read_bytes(), GPU)

    def test_failed_metadata_copy_leaves_relations_in_place(self):
        remote, mockbin = self.prepare_pull()
        mockcp = mockbin / 'cp'
        mockcp.write_text('#!/bin/sh\nexit 23\n')
        mockcp.chmod(0o755)
        self.run_pull(remote, mockbin, self.base / 'local', ok=False)
        self.assertEqual((remote / 'rels/blk-gpu.dat').read_bytes(), GPU)

    def test_empty_pull_with_cached_job_does_not_transfer_or_snapshot(self):
        remote, mockbin = self.prepare_pull()
        (remote / 'rels/blk-gpu.dat').unlink()
        local = self.base / 'local'
        (local / 'files').mkdir(parents=True)
        (local / f'files/{SHA}.job').write_bytes(JOB)
        (mockbin / 'rsync').write_text('#!/bin/sh\nexit 24\n')
        result = self.run_pull(remote, mockbin, local)
        self.assertIn('original .job already available', result.stdout)
        self.assertEqual(list((local / 'incoming').iterdir()), [])
        self.assertFalse(list(local.rglob('job.db')))

    def test_sqlite_invocations_ignore_user_configuration(self):
        remote, mockbin = self.prepare_pull()
        real_sqlite = shutil.which('sqlite3')
        wrapper = mockbin / 'sqlite3'
        wrapper.write_text('#!/usr/bin/env python3\nimport os,sys\n'
                           'assert "-batch" in sys.argv\n'
                           'assert sys.argv[sys.argv.index("-init")+1] == "/dev/null"\n'
                           f'os.execv({real_sqlite!r}, [{real_sqlite!r}, *sys.argv[1:]])\n')
        wrapper.chmod(0o755)
        self.run_pull(remote, mockbin, self.base / 'local')



if __name__ == '__main__':
    unittest.main()
