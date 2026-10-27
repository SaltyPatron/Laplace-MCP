#!/usr/bin/env python3
"""The code_search bridge takes the words of the most observed sense of a query word, not of its first senses in the
witness's order (#19). The engine is a stub that answers translate as the engine prints it: game's concepts ordered by
cntlist's usage counts (contest 38, single play 26, pastime 14), then the unobserved adjective senses (lame; plucky)."""
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
with tempfile.TemporaryDirectory(prefix='lpm-bridge-senses-') as directory:
    d = Path(directory)
    engine = d / 'engine'
    (d / 'translate.txt').write_text(TRANSLATE)
    # translate -n N WORD en en: game has the concepts above; any other word has none
    engine.write_text('#!/bin/sh\n[ "$1" = translate ] || exit 2\n[ "$4" = game ] && cat "%s" && exit 0\n'
                      'echo "$4: no lexicalization of it is attested in a language written en"\n' % (d / 'translate.txt'))
    engine.chmod(0o700)
    root = d / 'repo'
    root.mkdir()
    (root / 'match.py').write_text('def match_score(match):\n    return match.points\n')
    records = d / 'records.lpm'
    subprocess.run([str(binary), 'index', str(records), str(root)], check=True, capture_output=True)
    token = 't' * 40

    def bridge(extra):
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
                    data=json.dumps({'query': 'score of a game', 'bridge': True}).encode(),
                    headers={'Content-Type': 'application/json', 'Authorization': 'Bearer ' + token})
                with urllib.request.urlopen(request, timeout=30) as response:
                    return json.load(response).get('bridge', {}).get('words', {})
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
    print('bridge: game brings match (observed 38), bout with two senses, and never lame or halt (never observed)')
