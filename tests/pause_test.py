"""HTTP integration tests for pausing assignments while existing work drains."""
import http.client
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / 'ggnfs-sieve-server'


class PauseTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='pause test ')
        self.addCleanup(temp.cleanup)
        self.jobdir = Path(temp.name)
        job = self.jobdir / 'input.job'
        job.write_text('c0: 1\nc1: 1\nY0: 1\nY1: 1\n')
        subprocess.run([
            str(SERVER), 'init', f'--job={job}', f'--jobdir={self.jobdir}',
            '--siever=gnfs-lasieve4I14e', '--qmin=100', '--qmax=300',
            '--qrange=10', '--side=a',
        ], check=True, capture_output=True)
        self.token = (self.jobdir / 'token').read_text().strip()
        self.marker = self.jobdir / 'paused'
        self.log = (self.jobdir / 'server.log').open('w+')
        self.addCleanup(self.log.close)
        self.server = None
        self.addCleanup(self.stop_server)
        self.start_server()

    def start_server(self):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            self.port = sock.getsockname()[1]
        self.server = subprocess.Popen([
            str(SERVER), 'serve', f'--jobdir={self.jobdir}',
            f'--port={self.port}', '--bind=127.0.0.1', '--spotcheck-k=0',
            '--block-width-multiple=2',
        ], stdout=self.log, stderr=self.log)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.server.poll() is not None:
                self.log.seek(0)
                self.fail(self.log.read())
            try:
                if self.request('/health', method='GET')[0] == 200:
                    return
            except OSError:
                pass
            time.sleep(0.05)
        self.fail('server did not start')

    def stop_server(self):
        if self.server is not None:
            self.server.terminate()
            try:
                self.server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.server.kill()
                self.server.wait()
            self.server = None

    def request(self, path, payload=None, *, method='POST', headers=None):
        auth = {'Authorization': f'Bearer {self.token}'}
        auth.update(headers or {})
        if isinstance(payload, dict):
            payload = json.dumps(payload).encode()
            auth['Content-Type'] = 'application/json'
        conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=5)
        try:
            conn.request(method, path, body=payload, headers=auth)
            response = conn.getresponse()
            body = response.read()
            return response.status, body
        finally:
            conn.close()

    def lease(self, client, block=False, status=200):
        code, body = self.request('/lease', {
            'client_id': client, 'class': 'gpu' if block else 'cpu', 'block': block,
        })
        self.assertEqual(code, status, body)
        if code == 204:
            self.assertEqual(body, b'')
            return None
        return json.loads(body)

    def test_pause_drains_cpu_and_gpu_then_resumes(self):
        cpu = self.lease('cpu')
        gpu = self.lease('gpu', block=True)
        self.assertEqual(gpu['block_members'], 2)
        released = self.lease('release')
        self.marker.touch()
        self.lease('waiting-cpu', status=204)
        self.lease('waiting-gpu', block=True, status=204)
        self.assertEqual(self.request('/lease', {'client_id': 'unauthorized'},
                                     headers={'Authorization': 'Bearer invalid'})[0], 401)
        self.assertEqual(self.request('/release', {
            'client_id': 'release', 'workunit_id': released['workunit_id'],
        })[0], 200)
        for client, lease in [('cpu', cpu), ('gpu', gpu)]:
            with self.subTest(client=client):
                self.assertEqual(self.request('/renew', {
                    'client_id': client, 'workunit_id': lease['workunit_id'],
                })[0], 200)
                # One prime in each base-width band, so GPU coverage passes too.
                primes = []
                for start in range(lease['q_start'], lease['q_start'] + lease['q_range'], 10):
                    primes.append(next(q for q in range(start, start + 10)
                                       if all(q % d for d in range(2, int(q ** 0.5) + 1))))
                relations = ''.join(f'1,1:2:{q:x}\n' for q in primes).encode()
                code, body = self.request('/submit', relations, headers={
                    'X-Client-Id': client, 'X-Workunit-Id': lease['workunit_id'],
                })
                self.assertEqual(code, 200, body)
                self.assertTrue(json.loads(body)['accepted'])
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            code, body = self.request('/stats', method='GET')
            self.assertEqual(code, 200, body)
            stats = json.loads(body)['workunits']
            if stats['verified'] == 3:
                break
            time.sleep(0.05)
        self.assertEqual(stats['verified'], 3, stats)
        self.assertEqual(stats['leased'], 0, stats)
        self.assertEqual(stats['submitted'], 0, stats)
        self.lease('still-paused', status=204)
        self.marker.unlink()
        self.lease('resumed-cpu')
        self.assertEqual(self.lease('resumed-gpu', block=True)['block_members'], 2)

    def test_marker_survives_restart(self):
        self.marker.touch()
        self.stop_server()
        self.start_server()
        self.lease('cpu', status=204)
        self.lease('gpu', block=True, status=204)
        self.marker.unlink()
        self.lease('cpu')


if __name__ == '__main__':
    unittest.main()
