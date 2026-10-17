"""code_where and sources answer from the database (Laplace-MCP#9): a disposable database the engine deploys and
ingests two small C files and a Python file into; the tools find each name's files by their pathname records, the
declarations the code recipes said of it, and its lines, from the recorded content alone. The database is dropped after.
  tests/code-where.py path/to/lpm path/to/laplace   (LAPLACE_RECIPES: the engine's recipes; PGHOST, PGUSER as the runner's)
"""
import json, os, shutil, subprocess, sys, tempfile, uuid
from pathlib import Path

lpm, engine = sys.argv[1], sys.argv[2]
host, user = os.environ.get('PGHOST', '/tmp'), os.environ.get('PGUSER', 'laplace')
name = 'lpm_code_where_' + uuid.uuid4().hex[:12]
conninfo = f'host={host} port={os.environ.get("PGPORT", "5432")} user={user} dbname={name}'
pg_config = os.environ.get('PG_CONFIG')
psql = str(Path(pg_config).parent / 'psql') if pg_config else shutil.which('psql') or 'psql'


def fail(msg):
    print('code-where: ' + msg, file=sys.stderr)
    sys.exit(1)


def call(tool, args):
    msgs = [{'jsonrpc': '2.0', 'id': 0, 'method': 'initialize', 'params': {}},
            {'jsonrpc': '2.0', 'id': 1, 'method': tool and 'tools/call' or 'tools/list',
             'params': {'name': tool, 'arguments': args} if tool else {}}]
    env = dict(os.environ, LAPLACE_CONNINFO=conninfo, LAPLACE_ENGINE=str(Path(engine).resolve()))
    p = subprocess.run([lpm, 'mcp'], input='\n'.join(json.dumps(m) for m in msgs) + '\n', env=env,
                       capture_output=True, text=True, timeout=120)
    lines = [l for l in p.stdout.splitlines() if l.strip()]
    if not lines:
        fail(f'{tool}: no answer: {p.stderr}')
    r = json.loads(lines[-1])['result']
    if not tool:
        return r
    if r.get('isError'):
        fail(f'{tool} {args}: {r["content"][0]["text"]}')
    return json.loads(r['content'][0]['text'])


src = {
    'retry.c': '#include <stddef.h>\n/* the delay before an attempt, growing by a factor each time */\n'
               'static double backoff_delay(int attempt, double first, double factor){\n    double d = first;\n'
               '    for (int i = 0; i < attempt; i++) d *= factor;\n    return d;\n}\n'
               'int retry_count(int limit){ return limit > 0 ? limit : 1; }\n',
    'use.c': 'double backoff_delay(int attempt, double first, double factor);\n'
             'double wait_for(int attempt){\n    return backoff_delay(attempt, 0.1, 2.0);\n}\n',
    'jitter.py': 'import random\n\n\ndef jitter(delay, fraction):\n    """a delay moved by up to fraction of itself"""\n'
                 '    return delay * (1 + (random.random() * 2 - 1) * fraction)\n\n\ndef spread(delays):\n    return [jitter(d, 0.1) for d in delays]\n\n\nprint(spread([1.0, 2.0]))\n',
    # one declaration: the file's one claim is its own record (Laplace-Engine#67)
    'call.py': 'from jitter import jitter\n\nprint(jitter(1.0, 0.1))\n',
    'alone.py': 'def lonely_backoff(attempt):\n    return 2 ** attempt\n',
    'once.py': 'def once_jitter(delay):\n    return delay\n\n\nprint(once_jitter(1.0))\n',
}
created = False
with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as tmp:
    root = Path(tmp) / 'proj'
    root.mkdir()
    for f, text in src.items():
        (root / f).write_text(text)
    env = dict(os.environ, LAPLACE_WORK=str(Path(tmp) / 'work'))
    try:
        d = subprocess.run([engine, 'deploy', '-d', conninfo], env=env, capture_output=True, text=True, timeout=300)
        created = True
        if d.returncode:
            fail('deploy: ' + d.stdout[-2000:] + d.stderr[-2000:])
        i = subprocess.run([engine, 'ingest', '-d', conninfo, '-j', '2'] + [str(root / f) for f in src], env=env,
                           capture_output=True, text=True, timeout=300)
        if i.returncode:
            fail('ingest: ' + i.stdout[-3000:] + i.stderr[-2000:])

        tools = {t['name'] for t in call(None, {})['tools']}
        if not {'code_where', 'sources', 'file_outline', 'recipe_plan'} <= tools:
            fail(f'tools/list: {sorted(tools)}')

        r = call('code_where', {'query': 'backoff_delay', 'occurrences': 4, 'context': 40})
        hits = {Path(h['path']).name: h for h in r['hits']}
        if set(hits) != {'retry.c', 'use.c'}:
            fail(f'backoff_delay: files {sorted(hits)}')
        if not hits['retry.c']['path'].endswith('/proj/retry.c'):
            fail(f'backoff_delay: path {hits["retry.c"]["path"]}')
        if 'function_definition' not in hits['retry.c']['defines']:
            fail(f'retry.c: defines {hits["retry.c"]["defines"]}')
        at = hits['retry.c']['at']
        if not at or at[0]['line'] != 3 or at[0]['line0'] != 3 or at[0]['line1'] != 7:
            fail(f'retry.c: at {at}')
        if not at[0]['enclosing'].startswith('static double backoff_delay('):
            fail(f'retry.c: enclosing {at[0]["enclosing"]!r}')
        calls = [a for a in hits['use.c']['at'] if a['line'] == 3]
        if not calls or calls[0]['enclosing'] != 'double wait_for(int attempt){' or (calls[0]['line0'], calls[0]['line1']) != (2, 4):
            fail(f'use.c: at {hits["use.c"]["at"]}')

        for flag in (True, 'true'):      # call.py only calls jitter
            r = call('code_where', {'query': 'jitter', 'defines': flag})
            names = [Path(h['path']).name for h in r['hits']]
            if 'jitter.py' not in names or 'call.py' in names or any(not h['defines'] for h in r['hits']):
                fail(f'defines {flag!r}: {[h["path"] for h in r["hits"]]}')
        for args in ({'defines': False}, {}):
            r = call('code_where', dict(query='jitter', **args))
            if not {'call.py', 'jitter.py'} <= {Path(h['path']).name for h in r['hits']}:
                fail(f'jitter {args}: {[h["path"] for h in r["hits"]]}')
        r = call('code_where', {'query': 'backoff_delay', 'defines': True, 'path': 'retry'})
        if [Path(h['path']).name for h in r['hits']] != ['retry.c']:
            fail(f'defines in retry: {[h["path"] for h in r["hits"]]}')

        r = call('code_where', {'query': 'jitter'})
        py = [h for h in r['hits'] if h['path'].endswith('jitter.py')]
        if not py or 'function_definition' not in py[0]['defines'] or not any((a['line'], a['line0'], a['line1'], a['enclosing']) == (10, 9, 10, 'def spread(delays):') for a in py[0]['at']):
            fail(f'jitter: {r["hits"]}')

        for ident, file in (('lonely_backoff', 'alone.py'), ('once_jitter', 'once.py')):
            r = call('code_where', {'query': ident})
            got = [h for h in r['hits'] if h['path'].endswith('/' + file)]
            if not got or got[0]['defines'] != ['function_definition']:
                fail(f'{ident}: a file of one declaration does not define it: {r["hits"]}')
        r = call('code_where', {'query': 'once_jitter', 'defines': True})
        if [Path(h['path']).name for h in r['hits']] != ['once.py'] or not any(a['line'] == 5 for a in r['hits'][0]['at']):
            fail(f'once_jitter: {r["hits"]}')

        # file_outline: what each file declares, in order, with its lines, by path or by the file's ID
        def outline(**args):
            o = call('file_outline', args)
            if len(o['files']) != 1:
                fail(f'file_outline {args}: {o}')
            return [(d['kind'], d['name'], d.get('line')) for d in o['files'][0]['declarations']], o['files'][0]
        got, f = outline(path='proj/retry.c')
        if got != [('function_definition', 'backoff_delay', 3), ('parameter_declaration', 'attempt', 3), ('parameter_declaration', 'first', 3), ('parameter_declaration', 'factor', 3), ('function_definition', 'retry_count', 8), ('parameter_declaration', 'limit', 8)]:
            fail(f'outline retry.c: {got}')
        if outline(file=f['file'])[0] != got:
            fail('outline by file ID differs from by path')
        got, _ = outline(path='jitter.py')
        got = [d for d in got if d[0] == 'function_definition']
        if got != [('function_definition', 'jitter', 4), ('function_definition', 'spread', 9)]:
            fail(f'outline jitter.py: {got}')
        got, _ = outline(path='alone.py')
        got = [d for d in got if d[0] == 'function_definition']
        if got != [('function_definition', 'lonely_backoff', 1)]:
            fail(f'outline alone.py: {got}')
        if call('file_outline', {'path': 'proj/nowhere.c'})['files']:
            fail('outline of an absent file')

        # recipe_plan: which recipe reads a file, nothing read in
        p = call('recipe_plan', {'input': str(root / 'retry.c')})
        if p['exit'] != 0 or '1 files' not in p['log']:
            fail(f'recipe_plan: {p}')

        r = call('code_where', {'query': 'exponential_retry_that_is_nowhere'})
        if r['files'] or r['hits']:
            fail(f'an absent name: {r}')

        s = call('sources', {})
        if s['database'] != name or not s['sources']:
            fail(f'sources: {s}')
        print('code-where ok: backoff_delay defined at retry.c:3 (3..7), called at use.c:3 in wait_for; jitter defined in jitter.py and called at :10 in spread; a file of one def defines it; outlines and a plan; '
              f'{len(s["sources"])} witnesses')
    finally:
        if created:
            subprocess.run([psql, '-XwAt', '-h', host, '-U', user, '-d', 'postgres', '-c', f'DROP DATABASE IF EXISTS "{name}"'],
                           capture_output=True, text=True, timeout=60)