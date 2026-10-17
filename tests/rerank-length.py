#!/usr/bin/env python3
"""Laplace rerank does not reward length: the same evidence in a longer text scores lower, text with none of the
query scores 0, and a span that holds the query outranks a longer one that holds the same words."""
from pathlib import Path
import random
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve(strict=True)
QUERY = 'retry with exponential backoff when the database connection drops'


def rerank(records, *cands):
    out = subprocess.run([str(binary), 'rerank', str(records), QUERY, *map(str, cands)], capture_output=True,
                         text=True, check=True).stdout.splitlines()[1:]
    rows = {}
    for line in out:
        f = line.split()
        rows[' '.join(f[7:])] = dict(score=float(f[0]), lexical=float(f[1]), geometric=float(f[2]), bytes=int(f[6]))

    return rows


rng = random.Random(7)
vocab = ('alpha bravo charlie delta echo foxtrot golf hotel india juliet kilo lima mike november oscar papa quebec '
         'romeo sierra tango uniform victor whiskey xray yankee zulu').split()
with tempfile.TemporaryDirectory(prefix='lpm-rerank-') as directory:
    root = Path(directory) / 'repo'
    root.mkdir()
    # A corpus to give the constituents document frequencies: filler, plus the query's words at ordinary rates.
    for i in range(60):
        words = [rng.choice(vocab) for _ in range(rng.randint(40, 400))]
        if i % 3 == 0:
            words += ['when', 'the', 'with']
        if i % 10 == 0:
            words += ['connection', 'database']
        (root / f'filler{i}.txt').write_text('\n'.join(' '.join(words[k:k + 8]) for k in range(0, len(words), 8)) + '\n')
    policy = ('def delay(attempt):\n    # retry the database connection with exponential backoff when it drops\n'
              '    return base * factor ** attempt\n')
    padding = ''.join(' '.join(rng.choice(vocab) for _ in range(10)) + '\n' for _ in range(120))
    (root / 'policy.py').write_text(policy)
    (root / 'padded.py').write_text(policy + padding)
    (root / 'scattered.txt').write_text(padding[:len(padding) // 2] + 'retry\n' + padding[len(padding) // 2:]
                                        + 'backoff database connection\n' + padding)
    (root / 'unrelated.txt').write_text(padding)
    records = Path(directory) / 'records.lpm'
    subprocess.run([str(binary), 'index', str(records), str(root)], check=True, capture_output=True)

    r = rerank(records, *(root / n for n in ('policy.py', 'padded.py', 'scattered.txt', 'unrelated.txt')))
    p, d, s, u = (r[str(root / n)] for n in ('policy.py', 'padded.py', 'scattered.txt', 'unrelated.txt'))
    print({k: round(v['score'], 4) for k, v in zip(('policy', 'padded', 'scattered', 'unrelated'), (p, d, s, u))})
    assert p['score'] > d['score'], (p, d)              # same evidence, more text: lower
    assert p['lexical'] > d['lexical'], (p, d)
    assert d['score'] > s['score'], (d, s)              # the query together outranks its words scattered apart
    assert u['score'] == 0.0, u                         # no query constituent: no score, at any length
print('rerank length ok')
