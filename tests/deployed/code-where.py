"""code_where, code_callers, code_callees, file_outline, sources and ingest_status answer from the deployed database
(Laplace-MCP#9, #27, #28) through the deployed lpm.

The check ingests two small C files and four Python files as its own source, check-code-where-workspace (the recipes of
workspace-code, copied, under the witness "Laplace check code-where workspace"), deletes the files, and reads them back
through `lpm mcp` (standard input and output, no listener): each name's files by their pathname records, the
declarations the code recipes said of it, its lines, its callers and callees, and the files' bytes, from the recorded
content alone. The code tools are not scoped by witness: they walk up from the name's own text. Every name the fixture
declares begins with a prefix nothing else in the database holds (built at run time, so this file's text, which a
workspace ingest records, never holds a whole name), and every file sits in a directory of that prefix, so what the
tools answer of these names and paths is the fixture's alone. sources and ingest_status list every witness; the check
asserts on its own trunk among them. The witness is forgotten before and after.
"""
import os
import sys
from pathlib import Path

import _lpm

check = _lpm.check
P = 'qzcw'                                       # the fixture's names are P + a word; this file holds no whole one
DIR = P + '-proj'
B, R, W = P + 'backoff_delay', P + 'retry_count', P + 'wait_for'
J, S, L, O, RANDOM = P + 'jitter', P + 'spread', P + 'lonely_backoff', P + 'once_jitter', P + 'random'
ABSENT = P + 'exponential_retry_that_is_nowhere'

SRC = {
    'retry.c': '#include <stddef.h>\n/* the delay before an attempt, growing by a factor each time */\n'
               f'static double {B}(int attempt, double first, double factor){{\n    double d = first;\n'
               f'    for (int i = 0; i < attempt; i++) d *= factor;\n    return d;\n}}\n'
               f'int {R}(int limit){{ return limit > 0 ? limit : 1; }}\n',
    'use.c': f'double {B}(int attempt, double first, double factor);\n'
             f'double {W}(int attempt){{\n    return {B}(attempt, 0.1, 2.0);\n}}\n',
    'jitter.py': f'import {RANDOM}\n\n\ndef {J}(delay, fraction):\n    """a delay moved by up to fraction of itself"""\n'
                 f'    return delay * (1 + ({RANDOM}.{RANDOM}() * 2 - 1) * fraction)\n\n\n'
                 f'def {S}(delays):\n    return [{J}(d, 0.1) for d in delays]\n\n\nprint({S}([1.0, 2.0]))\n',
    # one declaration: the file's one claim is its own record (Laplace-Engine#67)
    'call.py': f'from {J} import {J}\n\nprint({J}(1.0, 0.1))\n',
    'alone.py': f'def {L}(attempt):\n    return 2 ** attempt\n',
    'once.py': f'def {O}(delay):\n    return delay\n\n\nprint({O}(1.0))\n',
}


def fail(message):
    raise AssertionError('code-where: ' + message)


def mine(rows, key='path'):
    """The rows of the fixture's files: every file of the fixture is in its directory."""
    return [row for row in rows if f'/{DIR}/' in row[key]]


def main():
    with check.Fixture('code-where') as fixture:
        name, root = _lpm.source(fixture, 'workspace', copy_from='workspace-code')
        witness = fixture.witness_of(name)
        fixture.forget()
        project = root / DIR
        project.mkdir(parents=True)
        for filename, text in SRC.items():
            (project / filename).write_bytes(text.encode('utf-8'))
        _lpm.ingest(fixture, name)
        trunks = fixture.trunks(name)
        if len(trunks) != 1:
            fail(f'{name}: witness trunks {trunks}')
        call = lambda tool, arguments=None, **more: _lpm.mcp(fixture, tool, arguments, **more)

        searched = call('search', {'input': B})
        if not searched.get('physicality') or not searched.get('constituents'):
            fail(f'search through the installed namespaces: {searched}')
        call('forward', {'input': B})

        # lpm runs the release's engine with the release's recipes: a C file outside every source root is one file
        # of the generic C recipe there, and one of the copied workspace C recipe under the check's recipes.
        plan = call('recipe_plan', {'input': str(project / 'retry.c')})
        if plan['exit'] != 0 or '1 files' not in plan['log']:
            fail(f'recipe_plan: {plan}')
        # What follows is read from PostgreSQL alone: the fixture's bytes are gone.
        for filename in SRC:
            (project / filename).unlink()

        tools = {t['name'] for t in call(None)['tools']}
        if not {'code_where', 'code_callers', 'code_callees', 'sources', 'ingest_status', 'file_outline', 'recipe_plan'} <= tools:
            fail(f'tools/list: {sorted(tools)}')

        # The bytes come from persisted content, without concatenating overlapping declaration, name and parameter
        # captures into the source.
        for filename, expected in SRC.items():
            o = call('file_outline', {'path': f'{DIR}/{filename}', 'include_text': True})
            if len(o['files']) != 1 or o['files'][0].get('text') != expected:
                fail(f'{filename}: persisted source differs: {o}')
            if o['files'][0]['bytes'] != len(expected.encode('utf-8')):
                fail(f'{filename}: persisted byte count differs: {o}')

        r = call('code_where', {'query': B, 'occurrences': 4, 'context': 40})
        hits = {Path(h['path']).name: h for h in mine(r['hits'])}
        if set(hits) != {'retry.c', 'use.c'}:
            fail(f'{B}: files {sorted(hits)}')
        if not hits['retry.c']['path'].endswith(f'/{DIR}/retry.c'):
            fail(f'{B}: path {hits["retry.c"]["path"]}')
        if 'function_definition' not in hits['retry.c']['defines']:
            fail(f'retry.c: defines {hits["retry.c"]["defines"]}')
        at = hits['retry.c']['at']
        if not at or at[0]['line'] != 3 or at[0]['line0'] != 3 or at[0]['line1'] != 7:
            fail(f'retry.c: at {at}')
        if not at[0]['enclosing'].startswith(f'static double {B}('):
            fail(f'retry.c: enclosing {at[0]["enclosing"]!r}')
        calls = [a for a in hits['use.c']['at'] if a['line'] == 3]
        if not calls or calls[0]['enclosing'] != f'double {W}(int attempt){{' or (calls[0]['line0'], calls[0]['line1']) != (2, 4):
            fail(f'use.c: at {hits["use.c"]["at"]}')

        for flag in (True, 'true'):      # call.py only calls the name
            r = call('code_where', {'query': J, 'defines': flag})
            ours = mine(r['hits'])
            names = [Path(h['path']).name for h in ours]
            if 'jitter.py' not in names or 'call.py' in names or any(not h['defines'] for h in r['hits']):
                fail(f'defines {flag!r}: {[h["path"] for h in r["hits"]]}')
        for arguments in ({'defines': False}, {}):
            r = call('code_where', dict(query=J, **arguments))
            if not {'call.py', 'jitter.py'} <= {Path(h['path']).name for h in mine(r['hits'])}:
                fail(f'{J} {arguments}: {[h["path"] for h in r["hits"]]}')
        r = call('code_where', {'query': B, 'defines': True, 'path': f'{DIR}/retry'})
        if [Path(h['path']).name for h in r['hits']] != ['retry.c']:
            fail(f'defines in retry: {[h["path"] for h in r["hits"]]}')

        r = call('code_where', {'query': J})
        py = [h for h in mine(r['hits']) if h['path'].endswith('/jitter.py')]
        if not py or 'function_definition' not in py[0]['defines'] or not any(
                (a['line'], a['line0'], a['line1'], a['enclosing']) == (10, 9, 10, f'def {S}(delays):') for a in py[0]['at']):
            fail(f'{J}: {r["hits"]}')

        for ident, file in ((L, 'alone.py'), (O, 'once.py')):
            r = call('code_where', {'query': ident})
            got = [h for h in mine(r['hits']) if h['path'].endswith('/' + file)]
            if not got or got[0]['defines'] != ['function_definition']:
                fail(f'{ident}: a file of one declaration does not define it: {r["hits"]}')
        r = call('code_where', {'query': O, 'defines': True})
        ours = mine(r['hits'])
        if [Path(h['path']).name for h in ours] != ['once.py'] or not any(a['line'] == 5 for a in ours[0]['at']):
            fail(f'{O}: {r["hits"]}')

        # file_outline: what each file declares, in order, with its lines, by path or by the file's ID
        def outline(**arguments):
            o = call('file_outline', arguments)
            if len(o['files']) != 1:
                fail(f'file_outline {arguments}: {o}')
            return [(d['kind'], d['name'], d.get('line')) for d in o['files'][0]['declarations']], o['files'][0]
        got, f = outline(path=f'{DIR}/retry.c')
        if got != [('function_definition', B, 3), ('parameter_declaration', 'attempt', 3), ('parameter_declaration', 'first', 3),
                   ('parameter_declaration', 'factor', 3), ('function_definition', R, 8), ('parameter_declaration', 'limit', 8)]:
            fail(f'outline retry.c: {got}')
        if outline(file=f['file'])[0] != got:
            fail('outline by file ID differs from by path')
        got = [d for d in outline(path=f'{DIR}/jitter.py')[0] if d[0] == 'function_definition']
        if got != [('function_definition', J, 4), ('function_definition', S, 9)]:
            fail(f'outline jitter.py: {got}')
        got = [d for d in outline(path=f'{DIR}/alone.py')[0] if d[0] == 'function_definition']
        if got != [('function_definition', L, 1)]:
            fail(f'outline alone.py: {got}')
        if call('file_outline', {'path': f'{DIR}/nowhere.c'})['files']:
            fail('outline of an absent file')

        # code_callers: the declarations whose code uses a name; a use outside every declaration is the top level, and a
        # declaration's own name is not a use (Laplace-MCP#27)
        cr = call('code_callers', {'query': B})
        callers = mine(cr['callers'])
        if {(Path(x['path']).name, x['name'], x['kind']) for x in callers} != {('use.c', W, 'function_definition')} \
                or [x['at'] for x in callers] != [[3]] or callers[0]['line'] != 2:
            fail(f'callers of {B}: {cr}')
        cr = call('code_callers', {'query': J})
        tops = {Path(x['path']).name: x['at'] for x in mine(cr['top_level'])}
        if [(Path(x['path']).name, x['name'], x['at']) for x in mine(cr['callers'])] != [('jitter.py', S, [10])] \
                or 3 not in tops.get('call.py', []):
            fail(f'callers of {J}: {cr}')
        if call('code_callers', {'query': J, 'path': f'{DIR}/call.py'})['callers']:
            fail(f'callers of {J} in call.py')
        cr = call('code_callers', {'query': S})
        if mine(cr['callers']) or [x['at'] for x in mine(cr['top_level']) if x['path'].endswith('/jitter.py')] != [[13]]:
            fail(f'callers of {S}: {cr}')

        # code_callees: what a declaration calls that a code recipe declared, and where; the rest is outside
        ce = call('code_callees', {'query': S, 'path': DIR})
        defs = ce['definitions']
        if len(defs) != 1 or (defs[0]['line0'], defs[0]['line1']) != (9, 10) or defs[0]['kind'] != 'function_definition' \
                or [(x['name'], x['kind'], [Path(f['path']).name for f in x['files']]) for x in defs[0]['callees']] != [(J, 'function_definition', ['jitter.py'])]:
            fail(f'callees of {S}: {ce}')
        ce = call('code_callees', {'query': W, 'path': DIR})
        got = {x['name']: {Path(f['path']).name for f in x['files']} for x in ce['definitions'][0]['callees']} if ce['definitions'] else {}
        if 'retry.c' not in got.get(B, set()) or (ce['definitions'][0]['line0'], ce['definitions'][0]['line1']) != (2, 4):
            fail(f'callees of {W}: {ce}')
        ce = call('code_callees', {'query': J, 'path': DIR})
        if len(ce['definitions']) != 1 or ce['definitions'][0]['callees'] or RANDOM not in ce['definitions'][0]['outside']:
            fail(f'callees of {J}: {ce}')
        ce = call('code_callees', {'query': B, 'path': DIR})
        if not ce['definitions'] or ce['definitions'][0]['callees']:
            fail(f'{B} calls nothing declared: {ce}')

        r = call('code_where', {'query': ABSENT})
        if r['files'] or r['hits']:
            fail(f'an absent name: {r}')

        database = check.sql('SELECT current_database()')
        s = call('sources', {})
        named = {w['id'] for w in s['sources'] if w['name'] == witness}
        if s['database'] != database or named != set(trunks):
            fail(f'sources: {database}: {witness}: {sorted(named)} against the trunks {trunks}')

        # ingest_status: each witness with its parts and readings, the busy sessions, the engine's ingest logs (here
        # the check's own log directory)
        logs = fixture.scratch / 'ingest-logs'
        logs.mkdir()
        demo, older = 'check-code-where-demo', 'check-code-where-older'
        (logs / f'{older}.log').write_text('reading\n')
        os.utime(logs / f'{older}.log', (1, 1))
        (logs / f'{demo}.log').write_text('reading demo\ndemo: 6 files, done\n\n')
        st = call('ingest_status', {}, LAPLACE_INGEST_LOGS=logs)
        own = [w for w in st['sources'] if w['id'] in trunks]
        if st['database'] != database or len(own) != 1 or own[0]['name'] != witness \
                or own[0]['readings'] != 1 or own[0]['parts'] != 1 or not isinstance(st['activity'], list):
            fail(f'ingest_status: {own} {st["database"]} activity {type(st["activity"]).__name__}')
        if [(x['source'], x['last']) for x in st['logs']['runs']] != [(demo, 'demo: 6 files, done'), (older, 'reading')]:
            fail(f'ingest_status logs: {st["logs"]}')
        only = call('ingest_status', {'source': witness}, LAPLACE_INGEST_LOGS=logs)
        if [w['id'] for w in only['sources']] != trunks or only['logs']['runs']:
            fail(f'ingest_status of {witness}: {only}')
        just = call('ingest_status', {'source': demo}, LAPLACE_INGEST_LOGS=logs)
        if just['sources'] or [x['source'] for x in just['logs']['runs']] != [demo]:
            fail(f'ingest_status of {demo}: {just}')
        # the newest Laplace-Operations ingest run (ingest.sh: <UTC>/<step>.log and .exit); absent directories say so
        runs = fixture.scratch / 'ingest-runs'
        (runs / '20260101T000000Z').mkdir(parents=True)
        (runs / '20260101T000000Z' / f'{older}.log').write_text('old run\n')
        newest = runs / '20260102T000000Z'
        newest.mkdir()
        (newest / f'{older}.log').write_text('reading\nwordnet: done\n')
        (newest / f'{older}.exit').write_text('0\n')
        (newest / f'{demo}.log').write_text('reading demo\n')
        st = call('ingest_status', {}, LAPLACE_INGEST_LOGS=logs, LAPLACE_INGEST_RUNS=runs)
        steps = {x['source']: (x['last'], x['exit']) for x in st['ingest_run'].get('steps', [])}
        if not st['ingest_run']['dir'].endswith('20260102T000000Z') \
                or steps != {older: ('wordnet: done', 0), demo: ('reading demo', None)}:
            fail(f'ingest_status ingest_run: {st["ingest_run"]}')
        st = call('ingest_status', {}, LAPLACE_INGEST_LOGS=logs / 'absent', LAPLACE_INGEST_RUNS=runs / 'absent')
        if 'no ingest log directory' not in st['logs'].get('error', '') \
                or 'no ingest run directory' not in st['ingest_run'].get('error', ''):
            fail(f'ingest_status of absent log directories: {st["logs"]} {st["ingest_run"]}')
    print(f'code-where ok: {B} defined at retry.c:3 (3..7), called at use.c:3 in {W}; {J} defined in jitter.py and called '
          f'at :10 in {S}; a file of one def defines it; callers and callees; outlines and a plan; ingest status of {witness}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
