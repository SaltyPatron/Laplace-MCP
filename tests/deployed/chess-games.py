"""Recorded games read back from the deployed database through the deployed lpm: players by the names their tags write,
a player's games and record, a witness's games, and one game replayed into SAN, UCI and FEN by the Engine's rules
(native/api_chess_games.c).

The check states three games as a witness named at run time, as the application records a game it played (laplace
witness --as, class AppDerived), under its own witness "Laplace check chess-games games", never Laplace's. Every
player's name is built at run time from a word nothing else in the database holds, so the players, records and games
the reads return for them are the check's; the games of a witness are read by the check's witness name. lpm serve
listens on a free loopback port with the check's own token. The witness is forgotten before and after.
"""
import sys

import _lpm

check = _lpm.check
LAPLACE = 'Qzlap' + 'vorn'                      # the check's names: this file's text holds none of them whole
CARLSEN, MAGNUS = 'Qzcar' + 'lund', 'Magnus'
NAKAMURA = 'Qznaka' + 'rim, Hikaru'
CARLSEN_NAME = f'{CARLSEN}, {MAGNUS}'

GAMES = f'''[Event "Fixture Open"]
[Date "2026.01.02"]
[White "{CARLSEN_NAME}"]
[Black "{LAPLACE}"]
[Result "1-0"]
[WhiteElo "2830"]
[BlackElo "2700"]

1. e4 {{[%clk 0:03:00]}} e5 {{[%clk 0:02:58]}} 2. Nf3 Nc6 3. Bb5 {{[%clk 0:02:50]}} a6 1-0

[Event "Fixture Open"]
[Date "2026.01.03"]
[White "{LAPLACE}"]
[Black "{CARLSEN_NAME}"]
[Result "1/2-1/2"]
[ECO "C55"]

1. e4 e5 2. Nf3 Nf6 3. Bc4 Bc5 4. O-O O-O 1/2-1/2

[Event "Fixture Open"]
[Date "2026.01.04"]
[White "{NAKAMURA}"]
[Black "{LAPLACE}"]
[Result "0-1"]

1. d4 d5 0-1
'''


def main():
    with check.Fixture('chess-games') as fixture:
        witness = fixture.witness('games')
        fixture.forget()
        games = fixture.data / 'games'
        games.mkdir()
        (games / 'fixture.pgn').write_text(GAMES, encoding='utf-8', newline='\n')
        # A sample of a source's files (ingest -s SOURCE FILE) publishes no witness; a witness named at run time does.
        fixture.run('witness', '-r', fixture.recipes, '-j', '2', '--as', witness, '--class', 'AppDerived', games / 'fixture.pgn')

        with _lpm.Server(fixture, 'lpm-check-chess-games') as server:
            request = server.request
            # players: a name as written, and a word of it
            exact = request('/v1/chess_players', {'search': LAPLACE})
            assert [(p['name'], p['games']) for p in exact['players']] == [(LAPLACE, 3)], exact
            word = request('/v1/chess_players', {'search': CARLSEN})
            assert [(p['name'], p['games']) for p in word['players']] == [(CARLSEN_NAME, 2)], word
            assert 'error' in request('/v1/chess_players', {})

            # a player's record and games, newest first
            carlsen = request('/v1/chess_player', {'name': CARLSEN_NAME})
            record = lambda r: (r['games'], r['wins'], r['draws'], r['losses'], r['unscored'], r['score'])
            assert record(carlsen['overall']) == (2, 1, 1, 0, 0, 1.5), carlsen
            assert record(carlsen['as_white']) == (1, 1, 0, 0, 0, 1), carlsen
            assert record(carlsen['as_black']) == (1, 0, 1, 0, 0, 0.5), carlsen
            rows = carlsen['games']
            assert [(g['date'], g['as_white'], g['opponent'], g['outcome']) for g in rows] == [
                ('2026.01.03', False, LAPLACE, 1), ('2026.01.02', True, LAPLACE, 2)], rows
            assert rows[1]['movetext'] == '1. e4 e5 2. Nf3 Nc6 3. Bb5 a6 1-0', rows
            assert rows[0]['eco'] == 'C55' and rows[0]['event'] == 'Fixture Open', rows
            page = request('/v1/chess_player', {'name': CARLSEN_NAME, 'limit': 1, 'offset': 1})
            assert page['total'] == 2 and [g['id'] for g in page['games']] == [rows[1]['id']], page
            assert request('/v1/chess_player', {'name': 'Qznobody' + 'vex'})['overall']['games'] == 0
            # the ratings the player's own Elo tag wrote, and the players met
            assert carlsen['peak_rating'] == 2830 and carlsen['ratings'] == [{'rating': 2830, 'games': 1}], carlsen
            assert carlsen['opponents_total'] == 1 and carlsen['opponents'] == [
                {'name': LAPLACE, 'games': 2, 'wins': 1, 'draws': 1, 'losses': 0, 'unscored': 0, 'score': 1.5}], carlsen
            laplace = request('/v1/chess_player', {'name': LAPLACE, 'opponents': 1})
            assert laplace['peak_rating'] == 2700 and laplace['opponents_total'] == 2, laplace
            assert [(o['name'], o['games'], o['score']) for o in laplace['opponents']] == [(CARLSEN_NAME, 2, 0.5)], laplace

            # a witness's games: every record under its trunks, newest first
            played = request('/v1/chess_games', {'witness': witness})
            assert played['total'] == 3 and [g['date'] for g in played['games']] == ['2026.01.04', '2026.01.03', '2026.01.02'], played
            assert played['games'][0]['movetext'] == '1. d4 d5 0-1', played
            assert {g['id'] for g in played['games']} >= {g['id'] for g in rows}, played

            # one game, replayed
            game = request('/v1/chess_game', {'id': rows[0]['id']})
            assert game['complete'] and game['white'] == LAPLACE and game['result'] == '1/2-1/2', game
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
    print(f"chess-games ok: players by their written names, a player's record and games, the games of {witness} and a "
          'replayed game read back')
    return 0


if __name__ == '__main__':
    sys.exit(main())
