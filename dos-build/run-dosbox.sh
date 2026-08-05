#!/bin/sh
# Run the ORIGINAL pristine 1998 REV.EXE (runtime/REV.EXE is byte-identical to
# Original/dos-rev/REVIVAL/REV.EXE) in DOSBox-X with memsize=32 — for comparison.
#   ./run-dosbox.sh   -> ORIGINAL 1998 demo   (runtime/,          dosbox-x.conf)
#   ./run-rebuilt.sh  -> our REBUILT demo     (runtime-rebuilt/,  dosbox-x-rebuilt.conf)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
DOSBOXX="/Applications/dosbox-x.app/Contents/MacOS/dosbox-x"
CONF="$HERE/dosbox-x.conf"

echo ">>> Running the ORIGINAL 1998 REV.EXE (runtime/, memsize=32)."
echo ">>> For the rebuilt demo use ./run-rebuilt.sh instead."

# Fresh log each run.
rm -f "$HERE/runtime/Runtime.LOG" "$HERE/runtime/RUNTIME.LOG" 2>/dev/null || true

exec "$DOSBOXX" -conf "$CONF" "$@"
