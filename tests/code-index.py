#!/usr/bin/env python3
"""The code index answers with the declaration or block that holds the query, not the whole file around it."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve(strict=True)


def search(records, query):
    out = subprocess.run([str(binary), 'search', str(records), query], capture_output=True, text=True, check=True).stdout
    hits = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 7 and parts[1] == 'shared' and parts[4] == 'lines':
            l0, l1 = (int(x) for x in parts[5].split('-'))
            hits.append((Path(' '.join(parts[6:])).name, l0, l1))
    return hits


def filler(word, n):
    return ''.join(f'{word} line {i} of padding text\n' for i in range(n))


with tempfile.TemporaryDirectory(prefix='lpm-code-index-') as directory:
    root = Path(directory) / 'repo'
    root.mkdir()
    # Text with no grammar: blocks at headings, so a section answers, not the 140-line file.
    (root / 'notes.md').write_text(
        '# Alpha\n\n' + filler('alpha', 60) + '\n# Journal\n\nThe ingest_run_journal row is written by the run writer.\n\n'
        + filler('omega', 70))
    # C: with a tree-sitter C grammar, each function is its own document; without one, the text blocks still are.
    c = 'int unrelated(void) {\n' + ''.join(f'    int u{i} = {i};\n' for i in range(60)) + '    return 0;\n}\n\n'
    c += 'int close_observation(int recipe_stream) {\n    int observed = recipe_stream + 1;\n    return observed;\n}\n\n'
    c += 'int trailing(void) {\n' + ''.join(f'    int t{i} = {i};\n' for i in range(60)) + '    return 0;\n}\n'
    (root / 'stream.c').write_text(c)
    records = Path(directory) / 'records.lpm'
    subprocess.run([str(binary), 'index', str(records), str(root)], check=True, capture_output=True)

    hits = search(records, 'ingest_run_journal written')
    assert hits, 'no hit for the journal section'
    name, l0, l1 = hits[0]
    assert name == 'notes.md' and l0 > 1 and l1 - l0 < 48, f'expected the Journal block, got {hits[0]}'
    assert 'Journal' in ''.join((root / 'notes.md').read_text().splitlines(True)[l0 - 1:l1]), hits[0]

    hits = search(records, 'close_observation recipe_stream')
    assert hits and hits[0][0] == 'stream.c', hits
    name, l0, l1 = hits[0]
    assert l1 - l0 < 48, f'expected a function-sized span, got {hits[0]}'
    body = ''.join((root / 'stream.c').read_text().splitlines(True)[l0 - 1:l1])
    assert 'close_observation' in body, hits[0]

    # The same answers from the saved records, and one answer per place.
    paths = [(h[0], h[1], h[2]) for h in search(records, 'padding text')]
    for i, a in enumerate(paths):
        for b in paths[i + 1:]:
            assert not (a[0] == b[0] and a[1] <= b[2] and b[1] <= a[2]), f'overlapping answers {a} {b}'
print('code index ok')
