#!/usr/bin/env python3
"""The real CLI resolves scalar identities through Native's floor index."""
from pathlib import Path
import subprocess
import sys

binary = Path(sys.argv[1]).resolve(strict=True)


def show(text):
    result = subprocess.run([str(binary), "text", text], capture_output=True, text=True, encoding="utf-8")
    assert result.returncode == 0, result.stderr
    return result.stdout.splitlines()[-1]


assert show("Sherlock Holmes") == "[[S,h,e,r,l,o,c,k],' ',[H,o,l,m,e,s]]"
content = "".join(chr(cp) for cp in range(0x400, 0x601))
tree = show(content)
assert "?" not in tree, tree
for character in content:
    assert f"U+{ord(character):04X}" in tree, character
print("text floor: ASCII, 513 distinct Unicode scalars and canonical indexed lookup passed")
