#!/usr/bin/env python3
"""code_search uses Laplace multi-tier AST/prose decomposition, UAX #29 constituent postings, and 4D S^3 Frechet
trajectory reranking directly (model="laplace-frechet4d", source="laplace"), with zero external Qdrant or dense
embedding dependencies."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve(strict=True)


def mcp(env, query, **extra):
    lines = [{'jsonrpc': '2.0', 'id': 0, 'method': 'initialize', 'params': {}},
             {'jsonrpc': '2.0', 'id': 1, 'method': 'tools/call',
              'params': {'name': 'code_search', 'arguments': dict(dict(query=query, limit=5), **extra)}}]
    run = subprocess.run([str(binary), 'mcp'], input=''.join(json.dumps(m) + '\n' for m in lines), env=env,
                         capture_output=True, text=True, encoding='utf-8', timeout=120)
    replies = [json.loads(l) for l in run.stdout.splitlines() if l.strip()]
    result = [r for r in replies if r.get('id') == 1][0]['result']
    return result['isError'], json.loads(result['content'][0]['text'])


with tempfile.TemporaryDirectory(prefix='lpm-code-frechet-') as directory:
    root = Path(directory) / 'repo'
    root.mkdir()
    (root / 'notes.md').write_text('# Journal\n\nThe ingest_run_journal row is written by the run writer.\n', encoding='utf-8')
    (root / 'handler.py').write_text(
        'def on_retry():\n    # written to ingest_run_journal after café retry\n    return "caf\u00e9"\n',
        encoding='utf-8')
    clean = {k: v for k, v in os.environ.items() if not k.startswith('LAPLACE_CODE_')}
    env = dict(clean, LAPLACE_CODE_RECORDS=str(Path(directory) / 'records.lpm'), LAPLACE_CODE_ROOTS=str(root))

    err, out = mcp(env, 'ingest_run_journal written')
    assert not err, out
    assert out['model'] == 'laplace-frechet4d' and out['fusion'] == 'rerank', out
    assert out['candidates']['lexical'] >= 2 and out['candidates']['reranked'] >= 2, out
    by = {Path(h['path']).name: h for h in out['hits']}
    assert 'notes.md' in by and 'handler.py' in by, by
    assert all(h['source'] == 'laplace' for h in out['hits']), out['hits']
    assert 'caf\u00e9' in by['handler.py']['snippet'], by['handler.py']
    assert [h['rerank'] for h in out['hits']] == sorted((h['rerank'] for h in out['hits']), reverse=True), out['hits']

    # RRF fusion between 4D Frechet rerank and lexical constituent rank:
    err, rrf = mcp(env, 'ingest_run_journal written', fusion='rrf')
    assert not err and rrf['fusion'] == 'rrf' and rrf['model'] == 'laplace-frechet4d', rrf
    for h in rrf['hits']:
        want = 1.0 / (60 + h['ranks']['rerank']) + 0.5 / (60 + h['ranks']['lexical'])
        assert abs(h['fused'] - want) < 1e-5, h
    assert [h['fused'] for h in rrf['hits']] == sorted((h['fused'] for h in rrf['hits']), reverse=True)

    # explain: returns every candidate with its rankings
    err, ex = mcp(env, 'ingest_run_journal written', explain=True, limit=1)
    assert not err and len(ex['hits']) == ex['candidates']['reranked'] >= 2, ex

    # Scope filtering: all, code (handler.py only), docs (notes.md only)
    err, o_code = mcp(env, 'ingest_run_journal written', scope='code')
    assert not err and o_code['scope'] == 'code' and [Path(h['path']).name for h in o_code['hits']] == ['handler.py'], o_code
    err, o_docs = mcp(env, 'ingest_run_journal written', scope='docs')
    assert not err and o_docs['scope'] == 'docs' and [Path(h['path']).name for h in o_docs['hits']] == ['notes.md'], o_docs
    err, o = mcp(env, 'ingest_run_journal written', scope='nowhere')
    assert err and 'scope' in o['error'], o

    # A requested path refreshes an indexing root and removes vanished files from a saved index.
    retired = root / 'retired.md'
    retired.write_text('obsoletechecksumterrain\n', encoding='utf-8')
    records = Path(directory) / 'records.lpm'
    subprocess.run([str(binary), 'index', str(records), str(root)], check=True, capture_output=True)
    retired.unlink()
    local = dict(env, LAPLACE_CODE_RECORDS=str(records), LAPLACE_CODE_REFRESH_SECONDS='0')
    err, out = mcp(local, 'obsoletechecksumterrain', path=str(root))
    assert not err and not any(Path(hit['path']).name == 'retired.md' for hit in out['hits']), out
    err, out = mcp(local, 'obsoletechecksumterrain', path=str(root / 'missing'))
    assert err and 'path does not exist' in out['error'] and 'hits' not in out, out

print('code semantic ok')
