#!/usr/bin/env python3
"""lpm serve fails closed: no token, no listener; every route but GET /health needs Authorization: Bearer TOKEN."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

binary = Path(sys.argv[1]).resolve(strict=True)
TOKEN = '0123456789abcdef' * 4


def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def base_env(**extra):
    env = {k: v for k, v in os.environ.items() if k not in ('LAPLACE_MCP_TOKEN', 'LAPLACE_MCP_TOKEN_FILE')}
    env.update(extra)
    return env


def refuses(env, why):
    p = port()
    r = subprocess.run([str(binary), 'serve', '127.0.0.1', str(p)], env=env, capture_output=True, text=True, timeout=10)
    assert r.returncode == 2, (why, r.returncode, r.stderr)
    assert 'refusing to listen unauthenticated' in r.stderr, (why, r.stderr)
    assert TOKEN not in r.stderr + r.stdout


def call(p, path, data=None, token=None, scheme='Bearer'):
    headers = {'Content-Type': 'application/json'}
    if token is not None:
        headers['Authorization'] = scheme + ' ' + token
    req = urllib.request.Request(f'http://127.0.0.1:{p}{path}', headers=headers,
                                 data=None if data is None else json.dumps(data).encode())
    try:
        with urllib.request.urlopen(req, timeout=10) as response:
            return response.status, json.load(response), response.headers
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read() or b'null'), error.headers


def serving(env):
    p = port()
    log = tempfile.TemporaryFile(mode='w+')
    process = subprocess.Popen([str(binary), 'serve', '127.0.0.1', str(p)], env=env, stdout=log, stderr=log)
    for _ in range(200):
        if process.poll() is not None:
            log.seek(0)
            raise RuntimeError('Service exited: ' + log.read())
        try:
            with socket.create_connection(('127.0.0.1', p), timeout=.1):
                return process, p
        except OSError:
            time.sleep(.05)
    process.kill()
    raise RuntimeError('Service did not start')


def stop(process):
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


with tempfile.TemporaryDirectory(prefix='lpm-serve-auth-') as d:
    d = Path(d)
    refuses(base_env(), 'no token')
    refuses(base_env(LAPLACE_MCP_TOKEN='short'), 'short token')
    refuses(base_env(LAPLACE_MCP_TOKEN='a' * 20 + ' ' + 'b' * 20), 'token with whitespace')
    refuses(base_env(LAPLACE_MCP_TOKEN_FILE=str(d / 'absent.env')), 'absent token file')
    (d / 'empty.env').write_text('OTHER=1\n')
    refuses(base_env(LAPLACE_MCP_TOKEN_FILE=str(d / 'empty.env')), 'file without the token')

    ingest = {'jsonrpc': '2.0', 'id': 1, 'method': 'tools/call', 'params': {'name': 'ingest', 'arguments': {'input': 'x'}}}
    (d / 'token.env').write_text('# made once\nLAPLACE_MCP_TOKEN="' + TOKEN + '"\n')
    for env in (base_env(LAPLACE_MCP_TOKEN=TOKEN), base_env(LAPLACE_MCP_TOKEN_FILE=str(d / 'token.env'))):
        process, p = serving(env)
        try:
            status, body, _ = call(p, '/health')
            assert (status, body) == (200, {'ok': True}), (status, body)
            for path, data in (('/v1/surface', None), ('/mcp', ingest), ('/v1/search', {'input': 'x'}), ('/health', {'x': 1})):
                for token, scheme in ((None, 'Bearer'), ('wrong' * 13, 'Bearer'), (TOKEN[:-1], 'Bearer'),
                                      (TOKEN + 'x', 'Bearer'), (TOKEN, 'Basic'), ('', 'Bearer')):
                    status, body, headers = call(p, path, data, token, scheme)
                    assert status == 401 and body == {'error': 'unauthorized'}, (path, token, scheme, status, body)
                    assert headers.get('WWW-Authenticate') == 'Bearer'
            status, body, _ = call(p, '/v1/surface', token=TOKEN)
            assert status == 200 and body.get('service') == 'Laplace-MCP', (status, body)
            status, body, _ = call(p, '/v1/surface', token=TOKEN, scheme='bearer')
            assert status == 200, status
            status, body, _ = call(p, '/mcp', {'jsonrpc': '2.0', 'id': 2, 'method': 'tools/list'}, TOKEN)
            assert status == 200 and any(t['name'] == 'ingest' for t in body['result']['tools']), (status, body)
        finally:
            stop(process)
print('lpm serve refuses to listen without a token and answers 401 to every unauthenticated route but GET /health')