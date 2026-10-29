#!/usr/bin/env python3
"""code_search with LAPLACE_CODE_SEMANTIC: an embeddings endpoint and Qdrant collections add candidates, the union is
ordered by weighted reciprocal rank fusion of each collection's ranking and the Laplace rerank's (fusion "rerank": the rerank
alone), and a configured service that fails is an error, never a lexical-only answer."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import socket
from pathlib import Path
import subprocess
import sys
import tempfile
import threading

binary = Path(sys.argv[1]).resolve(strict=True)
seen = []


class Fake(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, status, obj):
        data = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        seen.append((self.path, self.headers.get('Authorization'), body))
        if self.path == '/v1/embeddings':
            if self.headers.get('Authorization') != 'Bearer k3y':
                return self.reply(401, {'error': {'message': 'Invalid API Key'}})
            return self.reply(200, {'data': [{'embedding': [0.25, -0.5, 1e-3]}]})
        if self.path == '/collections/code/points/search':
            return self.reply(200, {'status': 'ok', 'result': [
                {'id': 1, 'score': 0.91, 'payload': {'repo': 'repo', 'path': 'repo/handler.py', 'start': 1, 'end': 3,
                                                     'text': 'def on_retry():\n    return "caf\u00e9"\n'}},
                {'id': 2, 'score': 0.42, 'payload': {'repo': 'repo', 'path': 'repo/notes.md', 'start': 1, 'end': 5,
                                                     'text': '# Journal\nThe ingest_run_journal row\n'}}]})
        if self.path == '/collections/prose/points/search':
            return self.reply(200, {'status': 'ok', 'result': [
                {'id': 3, 'score': 0.8, 'payload': {'repo': 'repo', 'path': 'repo/notes.md', 'start': 1, 'end': 3,
                                                     'text': '# Journal\n\nThe ingest_run_journal row\n'}}]})
        return self.reply(404, {'status': {'error': "Not found: Collection doesn't exist!"}})


def mcp(env, query, **extra):
    lines = [{'jsonrpc': '2.0', 'id': 0, 'method': 'initialize', 'params': {}},
             {'jsonrpc': '2.0', 'id': 1, 'method': 'tools/call',
              'params': {'name': 'code_search', 'arguments': dict(dict(query=query, limit=5), **extra)}}]
    run = subprocess.run([str(binary), 'mcp'], input=''.join(json.dumps(m) + '\n' for m in lines), env=env,
                         capture_output=True, text=True, encoding='utf-8', timeout=120)
    replies = [json.loads(l) for l in run.stdout.splitlines() if l.strip()]
    result = [r for r in replies if r.get('id') == 1][0]['result']
    return result['isError'], json.loads(result['content'][0]['text'])


server = ThreadingHTTPServer(('127.0.0.1', 0), Fake)
threading.Thread(target=server.serve_forever, daemon=True).start()
probe = socket.socket()
probe.bind(('127.0.0.1', 0))
closed = probe.getsockname()[1]
probe.close()
base = f'http://127.0.0.1:{server.server_address[1]}'

with tempfile.TemporaryDirectory(prefix='lpm-code-semantic-') as directory:
    root = Path(directory) / 'repo'
    root.mkdir()
    (root / 'notes.md').write_text('# Journal\n\nThe ingest_run_journal row is written by the run writer.\n')
    (root / 'handler.py').write_text('def on_retry():\n    return "caf\u00e9"\n', encoding='utf-8')
    clean = {k: v for k, v in os.environ.items() if not k.startswith('LAPLACE_CODE_')}
    env = dict(clean, LAPLACE_CODE_RECORDS=str(Path(directory) / 'records.lpm'), LAPLACE_CODE_ROOTS=str(root),
               LAPLACE_CODE_SEMANTIC='1', LAPLACE_CODE_QDRANT=base, LAPLACE_CODE_EMBED_URL=base + '/v1/embeddings',
               LAPLACE_CODE_EMBED_KEY='k3y', LAPLACE_CODE_COLLECTIONS='code')

    err, out = mcp(env, 'ingest_run_journal written')
    assert not err, out
    assert out['semantic'] is True and out['candidates']['semantic'] == 2, out
    embed = [s for s in seen if s[0] == '/v1/embeddings'][-1][2]
    assert embed['input'][0].startswith('Find the most relevant code snippet') and 'ingest_run_journal' in embed['input'][0]
    qd = [s for s in seen if s[0].startswith('/collections/')][-1][2]
    assert all(abs(a - b) < 1e-6 for a, b in zip(qd['vector'], [0.25, -0.5, 0.001])) and len(qd['vector']) == 3 and qd['limit'] == 15 and qd['with_payload'] is True, qd
    by = {Path(h['path']).name: h for h in out['hits']}
    assert by['notes.md']['source'] == 'both' and abs(by['notes.md']['cosine'] - 0.42) < 1e-6, by['notes.md']
    assert by['handler.py']['source'] == 'semantic' and by['handler.py']['collection'] == 'code', by['handler.py']
    assert Path(by['handler.py']['path']) == root / 'handler.py', by['handler.py']['path']
    assert 'caf\u00e9' in by['handler.py']['snippet'], by['handler.py']
    # The fused order: each hit's score is the sum of weight/(60 + rank) over the rankings that hold it, best first;
    # a code collection weighs 1 and the rerank half by default.
    assert out['fusion'] == 'rrf' and out['scope'] == 'code' and out['weights'] == {'rerank': 0.5, 'code': 1}, out
    for h in out['hits']:
        want = sum(out['weights'][k] / (60 + r) for k, r in h['ranks'].items() if k != 'lexical')
        assert abs(h['fused'] - want) < 1e-5 and 'rerank' in h['ranks'], h
    assert by['handler.py']['ranks']['code'] == 1 and by['notes.md']['ranks']['code'] == 2, by
    assert by['notes.md']['ranks']['lexical'] >= 1 and 'lexical' not in by['handler.py']['ranks'], by
    assert [h['fused'] for h in out['hits']] == sorted((h['fused'] for h in out['hits']), reverse=True)
    # The rerank alone, as before: notes.md, which shares the query's words, first.
    err, old = mcp(env, 'ingest_run_journal written', fusion='rerank')
    assert not err and old['fusion'] == 'rerank' and old['hits'][0]['path'].endswith('notes.md'), old['hits']
    assert [h['rerank'] for h in old['hits']] == sorted((h['rerank'] for h in old['hits']), reverse=True)
    # explain: every candidate of the union, with its rankings.
    err, ex = mcp(env, 'ingest_run_journal written', explain=True, limit=1)
    assert not err and len(ex['hits']) == ex['candidates']['union'] >= 2, ex

    # A prose collection: asked under scope all (half weight) and docs, not under code, the default.
    penv = dict(env, LAPLACE_CODE_TEXT_EMBED_URL=base + '/v1/embeddings', LAPLACE_CODE_TEXT_COLLECTIONS='prose')
    def asked(**extra):
        before = len(seen)
        err, o = mcp(penv, 'ingest_run_journal written', **extra)
        assert not err, o
        return o, {s[0].split('/')[2] for s in seen[before:] if s[0].startswith('/collections/')}
    o, colls = asked()
    assert colls == {'code'} and o['scope'] == 'code', (colls, o['scope'])
    o, colls = asked(scope='all')
    assert colls == {'code', 'prose'} and o['weights'] == {'rerank': 0.5, 'code': 1, 'prose': 0.5}, (colls, o['weights'])
    note = [h for h in o['hits'] if h['path'].endswith('notes.md')][0]
    assert note['ranks']['prose'] == 1 and abs(note['fused'] - (0.5 / (60 + note['ranks']['rerank']) + 1 / 62 + 0.5 / 61)) < 1e-5, note
    o, colls = asked(scope='code')
    assert colls == {'code'} and 'prose' not in o['weights'], (colls, o)
    o, colls = asked(scope='docs')
    assert colls == {'prose'} and 'code' not in o['weights'], (colls, o)
    err, o = mcp(penv, 'ingest_run_journal written', scope='nowhere')
    assert err and 'scope' in o['error'], o
    err, o = mcp(dict(penv, LAPLACE_CODE_SCOPE='all'), 'ingest_run_journal written')
    assert not err and o['scope'] == 'all' and 'prose' in o['weights'], o
    err, o = mcp(env, 'ingest_run_journal written', scope='docs')
    assert err and 'LAPLACE_CODE_TEXT_COLLECTIONS' in o['error'], o
    err, o = mcp(dict(env, LAPLACE_CODE_RERANK_WEIGHT='0'), 'ingest_run_journal written')
    assert not err and o['weights']['rerank'] == 0 and all(abs(h['fused'] - sum(1 / (60 + r) for k, r in h['ranks'].items() if k == 'code')) < 1e-5 for h in o['hits']), o
    err, o = mcp(dict(env, LAPLACE_CODE_PROSE_WEIGHT='2'), 'ingest_run_journal written')
    assert err and 'WEIGHT' in o['error'], o

    # Configured and failing: an error naming the cause, not lexical hits.
    for change, cause in ((dict(LAPLACE_CODE_COLLECTIONS='missing'), 'HTTP 404'),
                          (dict(LAPLACE_CODE_EMBED_KEY='wrong'), 'HTTP 401'),
                          (dict(LAPLACE_CODE_QDRANT=f'http://127.0.0.1:{closed}'), 'connection refused'),
                          (dict(LAPLACE_CODE_EMBED_URL=''), 'LAPLACE_CODE_EMBED_URL'),
                          (dict(LAPLACE_CODE_QDRANT=''), 'LAPLACE_CODE_QDRANT')):
        err, out = mcp(dict(env, **change), 'ingest_run_journal written')
        assert err and cause in out['error'] and 'hits' not in out, (change, out)

    # Not configured: lexical only, and the services are not asked.
    before = len(seen)
    err, out = mcp(dict(env, LAPLACE_CODE_SEMANTIC='0'), 'ingest_run_journal written')
    assert not err and out['semantic'] is False and all(h['source'] == 'lexical' for h in out['hits']), out
    assert len(seen) == before

    # A requested path refreshes an indexing root. It must remove vanished
    # files from a saved index even when periodic root refresh is disabled.
    retired = root / 'retired.md'
    retired.write_text('obsoletechecksumterrain\n')
    records = Path(directory) / 'records.lpm'
    subprocess.run([str(binary), 'index', str(records), str(root)], check=True, capture_output=True)
    retired.unlink()
    local = dict(env, LAPLACE_CODE_SEMANTIC='0', LAPLACE_CODE_RECORDS=str(records), LAPLACE_CODE_REFRESH_SECONDS='0')
    err, out = mcp(local, 'obsoletechecksumterrain', path=str(root))
    assert not err and not any(Path(hit['path']).name == 'retired.md' for hit in out['hits']), out
    err, out = mcp(local, 'obsoletechecksumterrain', path=str(root / 'missing'))
    assert err and 'path does not exist' in out['error'] and 'hits' not in out, out

server.shutdown()
print('code semantic ok')
