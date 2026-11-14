#!/usr/bin/env bash
# Laplace-MCP's database checks, against the deployed database through the deployed lpm and laplace: every
# tests/deployed/*.py in name order (a file whose name begins with _ is a helper they share, not a check), each to the
# end whatever the one before did. One line a check, its result and seconds; a failed check's output after its line.
# Exit 1 when any failed.
#
# Environment (Laplace-Operations' tests stage, after it deployed this revision):
#   LAPLACE_CONNINFO        the deployed database (laplace.env's)
#   LAPLACE_PG_DIR          the installed PostgreSQL: psql at $LAPLACE_PG_DIR/bin/psql
#   LAPLACE_LPM             the deployed lpm: the release's bin/lpm
#   LAPLACE_PROGRAM         the deployed laplace
#   LAPLACE_ENGINE_SOURCE   the deployed Engine's checkout (tests/deployed/check.py, recipes)
#   TMPDIR                  a scratch directory
set -u
here=$(cd "$(dirname "$0")" && pwd)
for name in LAPLACE_CONNINFO LAPLACE_PG_DIR LAPLACE_LPM LAPLACE_PROGRAM LAPLACE_ENGINE_SOURCE TMPDIR; do
    [ -n "${!name:-}" ] || { echo "run.sh: $name is not set" >&2; exit 2; }
done
export LAPLACE_CONNINFO LAPLACE_PG_DIR LAPLACE_LPM LAPLACE_PROGRAM LAPLACE_ENGINE_SOURCE TMPDIR
python=${PYTHON:-python3}
failed=()
for check in "$here"/*.py; do
    base=$(basename "$check" .py)
    case $base in _*) continue ;; esac
    output=$(mktemp "$TMPDIR/mcp-check-$base.XXXXXX")
    start=$(date +%s.%N)
    if "$python" "$check" >"$output" 2>&1; then result=ok; else result=FAILED; failed+=("$base"); fi
    seconds=$(awk -v a="$start" -v b="$(date +%s.%N)" 'BEGIN { printf "%.1f", b - a }')
    printf '%-8s %-24s %8s s\n' "$result" "$base" "$seconds"
    [ "$result" = ok ] || sed 's/^/    /' "$output"
    rm -f "$output"
done
if [ ${#failed[@]} -gt 0 ]; then
    echo "failed: ${failed[*]}"
    exit 1
fi
