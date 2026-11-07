#!/usr/bin/env python3
"""The code_search retrieval eval: ten questions an agent asks of the Laplace repositories, each with
the file that answers it. Each is asked through lpm mcp using Laplace's 4D S^3 Frechet trajectory reranker
(rerank and rrf), and hit@1, @5, @10, mean reciprocal rank at 10, and median latency are reported.
  tests/code-search-eval.py path/to/lpm [--min-mrr X] [--fusion rerank,rrf] [--explain FILE]
"""
import argparse, json, os, statistics, subprocess, sys, time

QUERIES = [
    ("retry with exponential backoff when the database connection drops", "Laplace/app/Laplace.Substrate/Ingestion/TransientErrorRetryPolicy.cs"),
    ("where are source keys mapped to their ingest command", "Laplace/app/Laplace.Cli/IngestDispatchTable.cs"),
    ("walk up the container index from claims to the trunks that hold them", "Laplace-Engine/src/walk.c"),
    ("Glicko rating update of a claim's standing from a witness's series", "Laplace-Native/src/consensus.c"),
    ("reranker score that corrects lexical overlap for document length", "Laplace-MCP/native/lpm_rerank.c"),
    ("turn a tree-sitter syntax tree into the file tree the recipe lays out", "Laplace-Engine/src/structure.c"),
    ("reconnect the lichess game stream after an empty response with capped backoff", "Laplace/app/Laplace.Chess/Service/LichessGameStream.cs"),
    ("diagnose who holds the measurement lane advisory lock", "Laplace/app/Laplace.Substrate/Crud/Npgsql/AdvisoryTxLock.cs"),
    ("decide whether a deployment drops and reseeds the database", "Laplace-Operations/seed-stamp.sh"),
    ("discrete Frechet trajectory distance over 4D S3 coordinates for code search reranking", "Laplace-MCP/native/lpm_rerank.c"),
]


def answers(path, target):
    """A hit is the target when its path ends in the target's path in its repository, under that repository."""
    p = path.replace('\\', '/')
    repo, rel = target.split('/', 1)
    return p.endswith('/' + target) or p == target or (p.endswith('/' + rel) and ('/' + repo + '/' in '/' + p))


class McpSession:
    def __init__(self, lpm):
        self.proc = subprocess.Popen(
            [lpm, 'mcp'],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding='utf-8',
            bufsize=1,
        )
        self.req_id = 0
        self._send({'jsonrpc': '2.0', 'id': 0, 'method': 'initialize', 'params': {}})
        self._recv()

    def _send(self, msg):
        self.proc.stdin.write(json.dumps(msg) + '\n')
        self.proc.stdin.flush()

    def _recv(self):
        while True:
            line = self.proc.stdout.readline()
            if not line:
                err = self.proc.stderr.read() if self.proc.stderr else ''
                raise SystemExit('code-search-eval: no answer: ' + err[-2000:])
            line = line.strip()
            if line:
                return json.loads(line)

    def ask(self, query, fusion, explain=False, scope=None):
        self.req_id += 1
        args = {'query': query, 'limit': 10, 'fusion': fusion}
        if scope:
            args['scope'] = scope
        if explain:
            args['explain'] = True
        t = time.time()
        self._send({'jsonrpc': '2.0', 'id': self.req_id, 'method': 'tools/call',
                    'params': {'name': 'code_search', 'arguments': args}})
        resp = self._recv()
        ms = (time.time() - t) * 1000
        r = resp['result']
        if r.get('isError'):
            raise SystemExit('code-search-eval: ' + r['content'][0]['text'])
        out = json.loads(r['content'][0]['text'])
        return out, out.get('ms', ms)

    def close(self):
        if self.proc.poll() is None:
            try:
                self.proc.stdin.close()
                self.proc.wait(timeout=5)
            except Exception:
                self.proc.kill()


def file_rank(hits, target):
    files = []
    for h in hits:
        if h['path'] not in files:
            files.append(h['path'])
        if len(files) > 10:
            return None
        if answers(h['path'], target):
            return len(files)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('lpm')
    ap.add_argument('--min-mrr', type=float, default=None, help='fail when the last fusion scores below this')
    ap.add_argument('--fusion', default='rerank,rrf', help='orders to compare')
    ap.add_argument('--explain', help='write every candidate with its rankings here (the last fusion)')
    a = ap.parse_args()
    if not os.environ.get('LAPLACE_CODE_ROOTS') and not os.environ.get('LAPLACE_CODE_RECORDS'):
        print('code-search-eval: skipped: no LAPLACE_CODE_ROOTS or LAPLACE_CODE_RECORDS configured here')
        return 0
    fusions = [f for f in a.fusion.split(',') if f]
    table, summary, explained = [], {}, {}
    sess = McpSession(a.lpm)
    try:
        for f in fusions:
            ranks, lat = [], []
            for q, target in QUERIES:
                fu, _, sc = f.partition('@')
                o, ms = sess.ask(q, fu, explain=bool(a.explain) and f == fusions[-1], scope=sc or None)
                if a.explain and f == fusions[-1]:
                    explained[q] = o
                    hits = sorted(o['hits'], key=lambda h: h['rank'])
                else:
                    hits = o['hits']
                ranks.append(file_rank(hits, target))
                lat.append(ms)
            summary[f] = {'hit@1': sum(r == 1 for r in ranks), 'hit@5': sum(1 for r in ranks if r and r <= 5),
                          'hit@10': sum(1 for r in ranks if r), 'mrr': round(sum(1 / r for r in ranks if r) / len(ranks), 3),
                          'median_ms': round(statistics.median(lat))}
            table.append(ranks)
    finally:
        sess.close()
    print('code-search-eval   ' + '  '.join(f'{f:>8s}' for f in fusions) + '   target')
    for i, (q, target) in enumerate(QUERIES):
        print(f'  {q[:60]:60s} ' + '  '.join(f'{(str(t[i]) if t[i] else "-"):>8s}' for t in table) + '   ' + target)
    for f in fusions:
        print(f'  {f:9s} ' + json.dumps(summary[f]))
    if a.explain:
        with open(a.explain, 'w', encoding='utf-8') as s:
            json.dump({'queries': QUERIES, 'answers': explained}, s)
    if a.min_mrr is not None and summary[fusions[-1]]['mrr'] < a.min_mrr:
        print(f'code-search-eval: {fusions[-1]} mean reciprocal rank {summary[fusions[-1]]["mrr"]} is below {a.min_mrr}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
