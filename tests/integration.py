#!/usr/bin/env python3
"""Exercise the actual daemon over HTTP, SQLite and real Linux PTY fds."""
import concurrent.futures
import http.client
import importlib.util
import json
import os
from pathlib import Path
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('sensor', ROOT / 'tools/modbus_sensor.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
BINARY = str(Path(sys.argv[1]).resolve())


def wait_for(fn, timeout=6):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            value = fn()
            if value:
                return value
        except (OSError, http.client.HTTPException, json.JSONDecodeError):
            pass
        time.sleep(0.025)
    raise AssertionError('condition timed out')


class Gateway:
    def __init__(self, root, token='', **options):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            self.port = sock.getsockname()[1]
        self.config = root / 'gateway.conf'
        values = dict(source='simulate', interval_ms=60, timeout_ms=80,
                      reconnect_ms=100, bind='127.0.0.1', port=self.port,
                      database=root / 'gateway.db', max_rows=15)
        values.update(options)
        self.config.write_text(''.join(f'{k}={v}\n' for k, v in values.items()))
        self.env = dict(os.environ, EDGE_API_TOKEN=token)
        self.token = token
        self.log = open(root / 'daemon.log', 'ab')
        self.process = subprocess.Popen([BINARY, '--config', str(self.config)],
                                        stderr=self.log, stdout=self.log, env=self.env)
        wait_for(lambda: self.get('/api/v1/status')[0] == 200)

    def get(self, path, auth=True):
        conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=2)
        headers = {'Authorization': 'Bearer ' + self.token} if self.token and auth else {}
        try:
            conn.request('GET', path, headers=headers)
            response = conn.getresponse()
            return response.status, response.read().decode()
        finally:
            conn.close()

    def status(self):
        code, body = self.get('/api/v1/status')
        assert code == 200
        return json.loads(body)

    def stop(self, expected=0):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
        try:
            rc = self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise AssertionError('daemon failed to drain and terminate')
        self.log.close()
        text = self.config.with_name('daemon.log').read_text()
        assert rc == expected, text
        assert 'AddressSanitizer' not in text and 'runtime error:' not in text, text


def raw_request(gateway, data, fragment=False):
    with socket.create_connection(('127.0.0.1', gateway.port), timeout=2) as sock:
        if fragment:
            for part in [data[:7], data[7:22], data[22:]]:
                sock.sendall(part)
                time.sleep(0.005)
        else:
            sock.sendall(data)
        out = b''
        while True:
            part = sock.recv(8192)
            if not part:
                return out
            out += part


def simulation(root):
    gateway = Gateway(root)
    try:
        wait_for(lambda: gateway.status()['persisted'] >= 4)
        assert gateway.get('/healthz')[0] == 200
        assert '<canvas' in gateway.get('/')[1]
        assert 'edge_samples_total' in gateway.get('/metrics')[1]
        for path in ['/api/v1/samples?limit=0', '/api/v1/samples?limit=201',
                     '/api/v1/samples?limit=-1', '/api/v1/alarms?other=1']:
            assert gateway.get(path)[0] == 400
        assert gateway.get('/missing')[0] == 404
        assert b'200 OK' in raw_request(gateway, b'GET /healthz HTTP/1.1\r\nHost: local\r\n\r\n', True)
        assert b'405 Method' in raw_request(gateway, b'POST / HTTP/1.1\r\nHost: local\r\n\r\n')
        assert b'400 Bad' in raw_request(gateway, b'GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n')
        assert b'431 Request' in raw_request(gateway, b'GET / HTTP/1.1\r\nHost: a\r\nX: ' + b'a' * 4100)
        # Several simultaneous clients plus slow headers must not stop acquisition.
        before = gateway.status()['acquired']
        with socket.create_connection(('127.0.0.1', gateway.port), timeout=3) as slow:
            slow.sendall(b'GET / HTTP/1.1\r\n')
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                codes = list(pool.map(lambda _: gateway.get('/api/v1/samples?limit=10')[0], range(40)))
            assert all(code == 200 for code in codes)
            time.sleep(2.1)
            assert slow.recv(1) == b''
        assert gateway.status()['acquired'] > before + 10
        wait_for(lambda: len(json.loads(gateway.get('/api/v1/alarms')[1])) >= 2)
        # A second process must not take ownership of the same database.
        other = subprocess.run([BINARY, '--config', str(gateway.config)], env=gateway.env,
                               capture_output=True, text=True, timeout=3)
        assert other.returncode == 1 and 'database lock' in other.stderr
    finally:
        gateway.stop()
    db = sqlite3.connect(root / 'gateway.db')
    assert db.execute('PRAGMA integrity_check').fetchone()[0] == 'ok'
    assert db.execute('SELECT COUNT(*) FROM samples').fetchone()[0] <= 15
    old_id = db.execute('SELECT MAX(id) FROM samples').fetchone()[0]
    db.close()
    again = Gateway(root)
    try:
        wait_for(lambda: again.status()['persisted'] >= 2)
        rows = json.loads(again.get('/api/v1/samples?limit=1')[1])
        assert rows[0]['id'] > old_id
    finally:
        again.stop()
    print('PASS simulation / HTTP bounds / concurrency / alarms / retention / restart / locking')


def authenticated(root):
    gateway = Gateway(root, token='test-token-0123456789', bind='0.0.0.0')
    try:
        assert gateway.get('/api/v1/status', auth=False)[0] == 401
        assert gateway.get('/healthz', auth=False)[0] == 401
        assert gateway.get('/', auth=False)[0] == 200
        assert gateway.get('/api/v1/status')[0] == 200
        assert 'test-token-0123456789' not in gateway.get('/')[1]
    finally:
        gateway.stop()
    print('PASS remote bind / Bearer authentication')


def serial_test(root):
    link = str(root / 'sensor')
    sensor = module.Sensor(link, fragment=True, bad_crc_every=4, drop_every=5, exception_every=7)
    stop = threading.Event()
    failures = []

    def serve(device):
        try:
            while not stop.is_set():
                device.poll()
        except Exception as exc:
            failures.append(exc)

    thread = threading.Thread(target=serve, args=(sensor,))
    thread.start()
    gateway = Gateway(root, source='modbus', serial_device=link, interval_ms=100,
                      timeout_ms=60, reconnect_ms=100, max_rows=100)
    try:
        wait_for(lambda: gateway.status()['acquired'] >= 8)
        status = gateway.status()
        assert status['crc_errors'] >= 1 and status['timeouts'] >= 1
        wait_for(lambda: gateway.status()['modbus_exceptions'] >= 1)
        stop.set(); thread.join(); sensor.close()
        wait_for(lambda: not gateway.status()['connected'])
        count = gateway.status()['acquired']
        # New PTY and same stable device link simulate unplug/replug.
        sensor = module.Sensor(link, temperature=-5.2)
        stop.clear()
        thread = threading.Thread(target=serve, args=(sensor,))
        thread.start()
        wait_for(lambda: gateway.status()['acquired'] > count + 2)
        status = gateway.status()
        assert status['serial_reconnects'] >= 1
        assert status['latest']['temperature_c'] == -5.2
        assert not failures, failures
    finally:
        stop.set(); thread.join(); sensor.close(); gateway.stop()
    print('PASS real PTY / fragmented RTU / CRC / timeout / exceptions / unplug / reconnect / negative temperature')


def storage_failure(root):
    gateway = Gateway(root)
    try:
        wait_for(lambda: gateway.status()['persisted'] >= 2)
        db = sqlite3.connect(root / 'gateway.db', timeout=1)
        db.execute('BEGIN IMMEDIATE')
        wait_for(lambda: gateway.status()['storage_errors'] > 0)
        assert gateway.get('/healthz')[0] == 503
        db.rollback(); db.close()
        wait_for(lambda: gateway.status()['storage_ok'])
        wait_for(lambda: gateway.get('/healthz')[0] == 200)
    finally:
        gateway.stop(expected=2)
    print('PASS writer lock failure / degraded health / recovery / nonzero exit on lost writes')


def crash_recovery(root):
    gateway = Gateway(root)
    wait_for(lambda: gateway.status()['persisted'] >= 4)
    old_id = json.loads(gateway.get('/api/v1/samples?limit=1')[1])[0]['id']
    gateway.process.kill()
    gateway.stop(expected=-signal.SIGKILL)
    with sqlite3.connect(root / 'gateway.db') as db:
        assert db.execute('PRAGMA integrity_check').fetchone()[0] == 'ok'
        assert db.execute('SELECT MAX(id) FROM samples').fetchone()[0] >= old_id
    again = Gateway(root)
    try:
        wait_for(lambda: again.status()['persisted'] >= 2)
        assert json.loads(again.get('/api/v1/samples?limit=1')[1])[0]['id'] > old_id
    finally:
        again.stop()
    print('PASS SIGKILL / WAL recovery / retained commits / restart')


def main():
    with tempfile.TemporaryDirectory(prefix='edge-integration-') as temp:
        root = Path(temp)
        for name, test in [('simulation', simulation), ('auth', authenticated),
                           ('serial', serial_test), ('storage', storage_failure),
                           ('crash', crash_recovery)]:
            folder = root / name
            folder.mkdir()
            try:
                test(folder)
            except Exception:
                for log in folder.glob('*.log'):
                    print(log.read_text(), file=sys.stderr)
                raise


if __name__ == '__main__':
    main()
