"""Recorded games read back: players by the names their tags write, a player's games and record, a witness's games,
and one game replayed into SAN, UCI and FEN by the Engine's rules (native/api_chess_games.c)."""
import contextlib
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
import uuid


lpm, engine = (Path(value).resolve() for value in sys.argv[1:3])
pg_config = os.environ.get('PG_CONFIG')
psql = os.environ.get('PSQL') or (str(Path(pg_config).parent / 'psql') if pg_config else shutil.which('psql') or 'psql')
host, port, user = (os.environ.get(key, default) for key, default in
                    [('PGHOST', '/tmp'), ('PGPORT', '5432'), ('PGUSER', 'laplace')])
name = 'laplace_chess_' + uuid.uuid4().hex[:12]
connection = f'host={host} port={port} user={user} dbname={name}'
created = False

GAMES = '''[Event "Fixture Open"]
[Date "2026.01.02"]
[White "Carlsen, Magnus"]
[Black "Laplace"]
[Result "1-0"]
[WhiteElo "2830"]
[BlackElo "2700"]

1. e4 {[%clk 0:03:00]} e5 {[%clk 0:02:58]} 2. Nf3 Nc6 3. Bb5 {[%clk 0:02:50]} a6 1-0

[Event "Fixture Open"]
[Date "2026.01.03"]
[White "Laplace"]
[Black "Carlsen, Magnus"]
[Result "1/2-1/2"]
[ECO "C55"]

1. e4 e5 2. Nf3 Nf6 3. Bc4 Bc5 4. O-O O-O 1/2-1/2

[Event "Fixture Open"]
[Date "2026.01.04"]
[White "Nakamura, Hikaru"]
[Black "Laplace"]
[Result "0-1"]

1. d4 d5 0-1
'''


def sql(db, query):
    result = subprocess.run([psql, '-XwAtq', '-h', host, '-p', port, '-U', user, '-d', db, '-v', 'ON_ERROR_STOP=1', '-c', query],
                            capture_output=True, text=True, encoding='utf-8', timeout=180)
    if result.returncode: raise RuntimeError(result.stderr)
    return result.stdout.strip()


try:
    sql('postgres', f'CREATE DATABASE "{name}"'); created = True
    sql(name, 'CREATE EXTENSION laplace CASCADE')
    with contextlib.ExitStack() as resources:
        root = Path(resources.enter_context(tempfile.TemporaryDirectory(prefix='mcp-chess-', dir=os.environ.get('LAPLACE_WORK'))))
        checkout = Path(__file__).resolve().parents[2] / 'Laplace-Engine'
        recipes = root / 'recipes'; shutil.copytree(checkout / 'recipes', recipes)
        shutil.copytree(checkout / 'firmware', root / 'firmware')
        (root / 'work').mkdir()
        games = root / 'data' / 'Games' / 'Chess' / 'laplace'; games.mkdir(parents=True)
        (games / 'fixture.pgn').write_text(GAMES, encoding='utf-8', newline='\n')
        env = dict(os.environ, LAPLACE_DATA=str(root / 'data'), LAPLACE_WORK=str(root / 'work'), LAPLACE_RECIPES=str(recipes),
                   LAPLACE_ENGINE=str(engine), LAPLACE_INGEST_ONE='1', OMP_NUM_THREADS='2')
        # A game Laplace played is stated by the runtime witness Laplace at AppDerived (Engine: laplace witness), as the
        # application records it when the game ends; a sample of a source's files (ingest -s SOURCE FILE) publishes no witness.
        result = subprocess.run([str(engine), 'witness', '-r', str(recipes), '-d', connection, '-j', '2', '--as', 'Laplace', '--class', 'AppDerived', str(games / 'fixture.pgn')],
                                env=env, capture_output=True, text=True, encoding='utf-8', timeout=300)
        if result.returncode: raise RuntimeError(result.stdout + result.stderr)

        token = secrets.token_hex(32)
        server_env = dict(env, LAPLACE_CONNINFO=connection, LAPLACE_MCP_TOKEN=token, LAPLACE_CODE_STORE=str(root / 'code-store'),
                          LAPLACE_CODE_ROOTS=str(root / 'work'), LAPLACE_CODE_SEMANTIC='')
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0)); listen = reservation.getsockname()[1]
        log = resources.enter_context((root / 'server.log').open('w+', encoding='utf-8'))
        process = subprocess.Popen([str(lpm), 'serve', '127.0.0.1', str(listen)], env=server_env, stdout=log, stderr=log)

        def stop():
            if process.poll() is None: process.terminate()
            try: process.wait(timeout=10)
            except subprocess.TimeoutExpired: process.kill(); process.wait(timeout=10)
        resources.callback(stop)

        def request(path, data=None):
            req = urllib.request.Request(f'http://127.0.0.1:{listen}' + path, data=None if data is None else json.dumps(data).encode(),
                                         headers={'Content-Type': 'application/json', 'Authorization': 'Bearer ' + token})
            with urllib.request.urlopen(req, timeout=90) as response: return json.load(response)

        deadline = time.monotonic() + 15
        while True:
            try:
                assert request('/health') == {'ok': True}; break
            except (OSError, TimeoutError):
                if process.poll() is not None or time.monotonic() > deadline:
                    log.flush(); raise RuntimeError((root / 'server.log').read_text(encoding='utf-8'))
                time.sleep(.1)

        # players: a name as written, and a word of it
        exact = request('/v1/chess_players', {'search': 'Laplace'})
        assert [(p['name'], p['games']) for p in exact['players']] == [('Laplace', 3)], exact
        word = request('/v1/chess_players', {'search': 'Carlsen'})
        assert [(p['name'], p['games']) for p in word['players']] == [('Carlsen, Magnus', 2)], word
        assert 'error' in request('/v1/chess_players', {})

        # a player's record and games, newest first
        carlsen = request('/v1/chess_player', {'name': 'Carlsen, Magnus'})
        record = lambda r: (r['games'], r['wins'], r['draws'], r['losses'], r['unscored'], r['score'])
        assert record(carlsen['overall']) == (2, 1, 1, 0, 0, 1.5), carlsen
        assert record(carlsen['as_white']) == (1, 1, 0, 0, 0, 1), carlsen
        assert record(carlsen['as_black']) == (1, 0, 1, 0, 0, 0.5), carlsen
        rows = carlsen['games']
        assert [(g['date'], g['as_white'], g['opponent'], g['outcome']) for g in rows] == [
            ('2026.01.03', False, 'Laplace', 1), ('2026.01.02', True, 'Laplace', 2)], rows
        assert rows[1]['movetext'] == '1. e4 e5 2. Nf3 Nc6 3. Bb5 a6 1-0', rows
        assert rows[0]['eco'] == 'C55' and rows[0]['event'] == 'Fixture Open', rows
        page = request('/v1/chess_player', {'name': 'Carlsen, Magnus', 'limit': 1, 'offset': 1})
        assert page['total'] == 2 and [g['id'] for g in page['games']] == [rows[1]['id']], page
        assert request('/v1/chess_player', {'name': 'Nobody'})['overall']['games'] == 0
        # the ratings the player's own Elo tag wrote, and the players met
        assert carlsen['peak_rating'] == 2830 and carlsen['ratings'] == [{'rating': 2830, 'games': 1}], carlsen
        assert carlsen['opponents_total'] == 1 and carlsen['opponents'] == [
            {'name': 'Laplace', 'games': 2, 'wins': 1, 'draws': 1, 'losses': 0, 'unscored': 0, 'score': 1.5}], carlsen
        laplace = request('/v1/chess_player', {'name': 'Laplace', 'opponents': 1})
        assert laplace['peak_rating'] == 2700 and laplace['opponents_total'] == 2, laplace
        assert [(o['name'], o['games'], o['score']) for o in laplace['opponents']] == [('Carlsen, Magnus', 2, 0.5)], laplace

        # a witness's games: every record under its trunks, newest first
        played = request('/v1/chess_games', {'witness': 'Laplace'})
        if not played.get('total'): raise AssertionError((played, sql(name, "WITH RECURSIVE t(e, d) AS (SELECT DISTINCT witness, 0 FROM attestation UNION ALL SELECT x.id, t.d + 1 FROM t JOIN physicality p ON p.entity = t.e CROSS JOIN LATERAL unnest(laplace_vertex_ids(p.path)) x(id) WHERE t.d < 4) SELECT t.d, count(*), string_agg(DISTINCT left(p.mask::text, 8), ',') FROM t LEFT JOIN physicality p ON p.entity = t.e GROUP BY 1 ORDER BY 1")))
        assert played['total'] == 3 and [g['date'] for g in played['games']] == ['2026.01.04', '2026.01.03', '2026.01.02'], played
        assert played['games'][0]['movetext'] == '1. d4 d5 0-1', played
        assert {g['id'] for g in played['games']} >= {g['id'] for g in rows}, played

        # one game, replayed
        game = request('/v1/chess_game', {'id': rows[0]['id']})
        assert game['complete'] and game['white'] == 'Laplace' and game['result'] == '1/2-1/2', game
        assert [t['name'] for t in game['tags']] == ['Event', 'Date', 'White', 'Black', 'Result', 'ECO'], game
        assert [p['san'] for p in game['plies']] == ['e4', 'e5', 'Nf3', 'Nf6', 'Bc4', 'Bc5', 'O-O', 'O-O'], game
        assert [p['uci'] for p in game['plies']][-2:] == ['e1g1', 'e8g8'], game
        assert game['start_fen'] == 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1', game
        assert game['plies'][0]['fen'] == 'rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1', game
        assert game['plies'][-1]['fen'].endswith(' w - - 6 5'), game
        assert game['line'] == rows[0]['line'] and all(len(p['position']) == 32 for p in game['plies']), game
        assert all(p['clock'] is None for p in game['plies']), game
        # a game's clocks, as its comments wrote them, each at its ply
        timed = request('/v1/chess_game', {'id': rows[1]['id']})
        assert timed['complete'] and [p['clock'] for p in timed['plies']] == ['0:03:00', '0:02:58', None, None, '0:02:50', None], timed
        assert [t['name'] for t in timed['tags']] == ['Event', 'Date', 'White', 'Black', 'Result', 'WhiteElo', 'BlackElo'], timed
        assert 'error' in request('/v1/chess_game', {'id': '0' * 32})
    print('chess games: players by their written names, a player\'s record and games, a witness\'s games and a replayed game read back')
finally:
    if created: sql('postgres', f'DROP DATABASE IF EXISTS "{name}" WITH (FORCE)')
