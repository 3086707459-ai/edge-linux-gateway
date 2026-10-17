#!/usr/bin/env python3
"""Measure this device's status-request latency and optional Linux VmRSS."""
import argparse
import json
import os
from pathlib import Path
import statistics
import time
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', default='http://127.0.0.1:8080')
    parser.add_argument('--duration', type=float, default=10)
    parser.add_argument('--pid', type=int)
    args = parser.parse_args()
    if not 0 < args.duration <= 3600:
        parser.error('duration must be in (0,3600] seconds')
    samples = []
    errors = 0
    rss = []
    deadline = time.monotonic() + args.duration
    token = os.environ.get('EDGE_API_TOKEN', '')
    while time.monotonic() < deadline:
        request = urllib.request.Request(args.url.rstrip('/') + '/api/v1/status')
        if token:
            request.add_header('Authorization', 'Bearer ' + token)
        start = time.perf_counter()
        try:
            with urllib.request.urlopen(request, timeout=2) as response:
                json.load(response)
            samples.append((time.perf_counter() - start) * 1000)
        except (OSError, ValueError):
            errors += 1
        if args.pid:
            try:
                for line in Path(f'/proc/{args.pid}/status').read_text().splitlines():
                    if line.startswith('VmRSS:'):
                        rss.append(int(line.split()[1]))
            except OSError:
                pass
        time.sleep(0.05)
    ordered = sorted(samples)
    result = dict(requests=len(samples), errors=errors, duration_requested_s=args.duration,
                  latency_ms=None if not samples else dict(min=min(samples), median=statistics.median(samples),
                  p95=ordered[min(len(ordered) - 1, int(len(ordered) * 0.95))], max=max(samples)),
                  rss_kib_max=max(rss) if rss else None)
    print(json.dumps(result, indent=2))
    return 0 if samples and not errors else 1


if __name__ == '__main__':
    raise SystemExit(main())
