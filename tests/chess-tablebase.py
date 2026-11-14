"""A position's tablebase read (native/api_chess.c, /v1/chess_tablebase): the Engine's probe of the Syzygy tables
(chess_tablebase.c in the reader this service links), the same one `laplace tablebase` runs, never a recorded verdict.
The verdict, the moves ranked and the best-play line answer from the tables alone, with no database; each move and ply
carries its [P, M, P'] claim ID, the one the Engine's CLI gives and the claim a game that plays it records.
    tests/chess-tablebase.py path/to/lpm path/to/laplace [TABLES]  (TABLES: Laplace-Engine/tests/chess beside it, KQvK)"""
import contextlib
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request


lpm, engine = (Path(value).resolve() for value in sys.argv[1:3])
tables = Path(sys.argv[3]).resolve() if len(sys.argv) > 3 else Path(__file__).resolve().parents[2] / 'Laplace-Engine' / 'tests' / 'chess'
assert (tables / 'syzygy' / 'KQvK.rtbw').is_file(), tables

FEN = '7k/1Q6/8/8/8/8/8/K7 b - -'
cli = subprocess.run([str(engine), 'tablebase', '-T', str(tables), '--line', FEN], capture_output=True, text=True, encoding='utf-8', timeout=60)
assert cli.returncode == 0, cli.stderr
rows = [line.split('\t') for line in cli.stdout.splitlines()]
cli_line = next(r for r in rows if r[0] == 'line')
cli_claims = [r[3] for r in rows if r[0] == 'claim']

with contextlib.ExitStack() as resources:
    root = Path(resources.enter_context(tempfile.TemporaryDirectory(prefix='mcp-tablebase-', dir=os.environ.get('LAPLACE_WORK'))))
    token = secrets.token_hex(32)
    # no database is reachable: the read answers from the tables
    env = dict(os.environ, LAPLACE_SYZYGY=str(tables), LAPLACE_MCP_TOKEN=token, LAPLACE_CONNINFO='host=127.0.0.1 port=1 dbname=none connect_timeout=1',
               LAPLACE_CODE_STORE=str(root / 'code-store'), LAPLACE_CODE_ROOTS=str(root), LAPLACE_CODE_SEMANTIC='', LAPLACE_WORK=str(root))
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0)); listen = reservation.getsockname()[1]
    log = resources.enter_context((root / 'server.log').open('w+', encoding='utf-8'))
    process = subprocess.Popen([str(lpm), 'serve', '127.0.0.1', str(listen)], env=env, stdout=log, stderr=log)

    def stop():
        if process.poll() is None: process.terminate()
        try: process.wait(timeout=10)
        except subprocess.TimeoutExpired: process.kill(); process.wait(timeout=10)
    resources.callback(stop)

    def request(path, data=None):
        req = urllib.request.Request(f'http://127.0.0.1:{listen}' + path, data=None if data is None else json.dumps(data).encode(),
                                     headers={'Content-Type': 'application/json', 'Authorization': 'Bearer ' + token})
        with urllib.request.urlopen(req, timeout=60) as response: return json.load(response)

    deadline = time.monotonic() + 15
    while True:
        try:
            request('/health'); break
        except (OSError, TimeoutError, urllib.error.HTTPError):
            if process.poll() is not None or time.monotonic() > deadline:
                log.flush(); raise RuntimeError((root / 'server.log').read_text(encoding='utf-8'))
            time.sleep(.1)

    read = request('/v1/chess_tablebase', {'fen': FEN})
    assert read['tables'] and read['covered'] and read['database'] is False and read['claims'] is None, read
    assert (read['position'], read['wdl'], read['dtz']) == ('ba7239ecffb26a06aca87a451b8fea07', 'loss', -12), read
    assert [(m['san'], m['wdl'], m['value']) for m in read['moves']] == [('Kg8', 'loss', -12)], read['moves']
    line = read['line']
    assert (line['id'], line['result'], len(line['plies'])) == (cli_line[1], cli_line[2], int(cli_line[3])) == ('6554c79ccee9acdcb4aa4adfa1b4a6d3', '1-0', 12), (line, cli_line)
    assert ' '.join(p['san'] for p in line['plies']) == cli_line[4], line
    assert [p['claim'] for p in line['plies']] == cli_claims, (line, cli_claims)
    assert read['moves'][0]['claim'] == line['plies'][0]['claim'], read
    assert all(p['recorded'] is None for p in line['plies']), line

    # a stalemate: covered, a draw, no line; a position of more pieces than the tables hold: not covered, its moves listed
    stalemate = request('/v1/chess_tablebase', {'fen': 'k7/8/1Q6/8/8/8/8/7K b - -'})
    assert (stalemate['covered'], stalemate['wdl'], stalemate['dtz'], stalemate['moves'], stalemate['line']) == (True, 'draw', 0, [], None), stalemate
    start = request('/v1/chess_tablebase', {'fen': 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq -'})
    assert (start['covered'], start['wdl'], start['line'], len(start['moves'])) == (False, None, None, 20), start
    assert 'error' in request('/v1/chess_tablebase', {'fen': 'no position'})
print('chess-tablebase: the tables answer the read, with no database; its line and claims are the Engine CLI\'s')
