#!/bin/sh
# Run the FROM-SOURCE rebuilt REVIVAL/FLOOD demo in DOSBox-X.
#
#   ./run-rebuilt.sh            # visible window (watch the demo)
#   ./run-rebuilt.sh --headless # no window, byte-identical; for log-only runs
#
# It deploys the freshly built REV.EXE into runtime-rebuilt/ (mounted as C:),
# clears the previous log, then launches DOS/4GW + REV.EXE via the rebuilt conf.
# Progress/markers land in runtime-rebuilt/RUNTIME.LOG. DOSBox's own log
# (illegal-opcode / exception dumps under core=normal) goes to /tmp/dosbox_run.out.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
DOSBOXX="/Applications/dosbox-x.app/Contents/MacOS/dosbox-x"
CONF="$HERE/dosbox-x-rebuilt.conf"
BUILT="$HERE/rev-build/_build/REV.EXE"
RT="$HERE/runtime-rebuilt"

# Deploy the latest build, with a staleness guard.
if [ -f "$BUILT" ]; then
  # Warn if any source is newer than the built exe (i.e. you forgot to rebuild).
  newer=$(find "$HERE/rev-build/REVIVAL" "$HERE/rev-build/FDS/SOURCE" \
            \( -name '*.CPP' -o -name '*.H' -o -name '*.ASM' \) -newer "$BUILT" 2>/dev/null | head -1)
  [ -n "$newer" ] && echo ">>> WARNING: source newer than the built REV.EXE — REBUILD FIRST." \
                          "(e.g. $(basename "$newer"))" >&2
  echo ">>> Deploying build: $(stat -f '%z bytes, built %Sm' -t '%b %d %H:%M:%S' "$BUILT")"
  cp "$BUILT" "$RT/REV.EXE"
else
  echo ">>> WARNING: no built REV.EXE at $BUILT — run the build first." >&2
fi

# Fresh log each run.
rm -f "$RT/RUNTIME.LOG" "$RT/Runtime.LOG" 2>/dev/null || true

if [ "$1" = "--headless" ]; then
  SDL_VIDEODRIVER=dummy "$DOSBOXX" -conf "$CONF" >/tmp/dosbox_run.out 2>&1
else
  "$DOSBOXX" -conf "$CONF"
fi
