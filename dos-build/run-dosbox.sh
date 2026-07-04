#!/bin/sh
# Launch the FLOOD demo (prebuilt REV.EXE) in DOSBox-X, Phase-1 smoke test.
# The demo writes Runtime.LOG into dos-build/runtime/ (mounted as C:), which
# is readable from the repo after the run.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
DOSBOXX="/Applications/dosbox-x.app/Contents/MacOS/dosbox-x"
CONF="$HERE/dosbox-x.conf"

# Fresh log each run.
rm -f "$HERE/runtime/Runtime.LOG" "$HERE/runtime/RUNTIME.LOG" 2>/dev/null || true

exec "$DOSBOXX" -conf "$CONF" "$@"
