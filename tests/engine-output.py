#!/usr/bin/env python3
"""Exercise the real HTTP/child-process path with exact, short output and no database."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request


binary = Path(sys.argv[1]).resolve(strict=True)
real_engine = Path(sys.argv[2]).resolve(strict=True) if len(sys.argv) > 2 else None
with tempfile.TemporaryDirectory(prefix='laplace-engine-output-') as directory:
    engine = Path(directory) / 'engine'
    engine.write_text('#!/bin/sh\nprintf "%s" "$2"\n')
    engine.chmod(0o700)
    token = 't' * 40
    environment = dict(os.environ, LAPLACE_ENGINE=str(engine), MALLOC_PERTURB_='85', LAPLACE_MCP_TOKEN=token)
    if real_engine:
        environment['LAPLACE_CATALOG_ENGINE'] = str(real_engine)
        environment['LAPLACE_RECIPES'] = str(Path(__file__).resolve().parents[2] / 'Laplace-Engine' / 'recipes')
        environment['LAPLACE_CONNINFO'] = 'host=invalid'
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    with tempfile.TemporaryFile(mode='w+') as log:
        process = subprocess.Popen([str(binary), 'serve', '127.0.0.1', str(port)],
                                   env=environment, cwd=directory, stdout=log, stderr=log)
        try:
            for _ in range(100):
                if process.poll() is not None:
                    log.seek(0)
                    raise RuntimeError('Service exited: ' + log.read())
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=.1):
                        break
                except OSError:
                    time.sleep(.05)
            else:
                raise RuntimeError('Service did not start')
            for expected in ('Longer output from the first child process', 'short', 'x'):
                request = urllib.request.Request('http://127.0.0.1:' + str(port) + '/v1/text',
                    data=json.dumps({'input': expected}).encode(),
                    headers={'Content-Type': 'application/json', 'Authorization': 'Bearer ' + token})
                with urllib.request.urlopen(request, timeout=10) as response:
                    result = json.load(response)
                if result != {'exit': 0, 'log': expected}:
                    raise AssertionError('Child output contains bytes outside the captured output: ' +
                                         repr(result)[:200])
            print('HTTP child output is exact across three calls; no stale heap bytes returned')
            if real_engine:
                # Continue through the same real HTTP/child boundary, now with
                # the candidate Engine and the complete recipe estate.
                engine.write_text('#!/bin/sh\nexec "$LAPLACE_CATALOG_ENGINE" "$@"\n')
                request = urllib.request.Request('http://127.0.0.1:' + str(port) + '/v1/recipe_catalog',
                    data=b'{}', headers={'Content-Type': 'application/json', 'Authorization': 'Bearer ' + token})
                with urllib.request.urlopen(request, timeout=30) as response:
                    result = json.load(response)
                assert result['exit'] == 0, result
                catalog = json.loads(result['log'])
                declarations = Path(environment['LAPLACE_RECIPES'])
                assert {s['name'] for s in catalog['sources']} == {p.parent.name for p in declarations.glob('*/source')}
                assert len(catalog['formats']) == len(list(declarations.glob('*.recipe')))
                unicode = next(s for s in catalog['sources'] if s['name'] == 'unicode')
                assert 'uca' in unicode['aliases']
                print(f"HTTP recipe catalog: {len(catalog['sources'])} sources and {len(catalog['formats'])} formats; no database")
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
