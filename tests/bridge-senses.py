#!/usr/bin/env python3
"""The code_search bridge takes the words of the sense a query word is used in, not of its first senses in the witness's
order (#19): each sense scores its share of the word's observed usages, plus a pull for each of the query's other
words its text or words share (Laplace-Wiki's word sense disambiguation: prior plus pull), and the best sense bridges. The engine is a stub that answers translate as the engine prints it: game's
concepts ordered by cntlist's usage counts (contest 38, single play 26, pastime 14), then the unobserved adjective
senses (lame; plucky); amusement's one concept; any other word has none."""
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
TRANSLATE = '''
  1653 \u00b1 258  a contest with rules to determine a winner   observed 38 times

    en     game, match

  1653 \u00b1 258  a single play of a sport or other contest   observed 26 times

    en     game, bout

  1653 \u00b1 258  an amusement or pastime   observed 14 times

    en     game, pastime

  1653 \u00b1 258  disabled in the feet or legs

    en     game, lame, halt

30 steps, each one set   total 1.0 ms
'''
AMUSEMENT = '''
  1653 \u00b1 258  a feeling of delight at being entertained   observed 5 times

    en     amusement

12 steps, each one set   total 1.0 ms
'''
with tempfile.TemporaryDirectory(prefix='lpm-bridge-senses-') as directory:
    d = Path(directory)
    engine = d / 'engine'
    (d / 'translate.txt').write_text(TRANSLATE)
    (d / 'amusement.txt').write_text(AMUSEMENT)
    # translate -n N WORD en en: game and amusement have the concepts above; any other word has none
    engine.write_text('#!/bin/sh\n[ "$1" = translate ] || exit 2\n[ "$4" = game ] && cat "%s" && exit 0\n'
                      '[ "$4" = amusement ] && cat "%s" && exit 0\n'
                      'echo "$4: no lexicalization of it is attested in a language written en"\n' % (d / 'translate.txt', d / 'amusement.txt'))
    engine.chmod(0o700)
    root = d / 'repo'
    root.mkdir()
    (root / 'match.py').write_text('def match_score(match):\n    return match.points\n')
    records = d / 'records.lpm'
    subprocess.run([str(binary), 'index', str(records), str(root)], check=True, capture_output=True)
    token = 't' * 40

    def bridge(extra, query='score of a game', whole=False):
        environment = dict(os.environ, LAPLACE_ENGINE=str(engine), LAPLACE_MCP_TOKEN=token, LAPLACE_CODE_RECORDS=str(records),
                           LAPLACE_CODE_ROOTS=str(root), LAPLACE_CODE_REFRESH_SECONDS='0', LAPLACE_CODE_BRIDGE='1', **extra)
        environment.pop('LAPLACE_CODE_SEMANTIC', None)
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
        with tempfile.TemporaryFile(mode='w+') as log:
            process = subprocess.Popen([str(binary), 'serve', '127.0.0.1', str(port)], env=environment, cwd=directory,
                                       stdout=log, stderr=log)
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
                request = urllib.request.Request('http://127.0.0.1:' + str(port) + '/v1/code_search',
                    data=json.dumps({'query': query, 'bridge': True}).encode(),
                    headers={'Content-Type': 'application/json', 'Authorization': 'Bearer ' + token})
                with urllib.request.urlopen(request, timeout=30) as response:
                    b = json.load(response).get('bridge', {})
                    return b if whole else b.get('words', {})
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()

    words = bridge({})
    assert words == {'game': ['match']}, f'the most observed sense of game is the contest (match): {words}'
    words = bridge({'LAPLACE_CODE_BRIDGE_CONCEPTS': '2'})
    assert words == {'game': ['match', 'bout']}, f'the two most observed senses: {words}'
    words = bridge({'LAPLACE_CODE_BRIDGE_CONCEPTS': '9'})
    assert 'lame' not in words.get('game', []) and 'halt' not in words.get('game', []), \
        f'a sense no usage was observed of brings nothing: {words}'
    # the query says which sense: amusement is in the text of game's pastime sense (observed 14), so that sense is the
    # one the query is about, before the contest (observed 38)
    b = bridge({}, 'a game played for amusement', whole=True)
    assert b['words'].get('game') == ['pastime'], f'game beside amusement is the pastime: {b}'
    g = b['senses']['game'][0]
    assert g['shared'] == 1 and abs(g['score'] - (14 / 78 + 0.5)) < 1e-3, f'pastime: prior 14 of the 78 usages of the four senses considered, and one shared word: {b}'
    a = b['senses']['amusement'][0]
    assert a['shared'] == 0 and abs(a['score'] - 1.0) < 1e-3, f'amusement shares nothing with game: its prior alone: {b}'
    # with no pull, the prior alone decides: the contest
    b = bridge({'LAPLACE_CODE_BRIDGE_PULL': '0'}, 'a game played for amusement', whole=True)
    assert b['words'].get('game') == ['match'], f'pull 0: the most observed sense: {b}'
    print('bridge: game brings match (observed 38), bout with two senses, and never lame or halt (never observed);'
          ' beside amusement it brings pastime, the sense the query shares a word with')
