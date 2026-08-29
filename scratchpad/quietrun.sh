#!/bin/bash
# Box-quiet protocol wrapper: wait (bounded) for other agents' builds and
# benches to stop, record load+battery before, run the battery, record after.
# Anything that starts mid-battery shows up in the AFTER line and the caller
# discards the run.
MAXWAIT=${MAXWAIT:-90}          # 90 * 20 s = 30 min
i=0
while [ $i -lt $MAXWAIT ]; do
  # NOTE 2026-08-29: `pgrep -f` matches COMMAND LINES, so these patterns also
  # match a PEER AGENT'S OWN QUIET-WAIT LOOP (whose command string contains the
  # literals "cmake|ninja"). Two waiters then wait on each other and neither
  # ever starts -- this stalled a battery silently. `ps -Ao comm=` matches the
  # executable NAME only and cannot false-match a shell. See quietrun2.sh.
  d=$(ps -Ao comm= | grep -cE '(^|/)DEMO$')
  b=$(ps -Ao comm= | grep -cE '(^|/)(ninja|cc1plus|clang\+\+|cargo)$')
  if [ "$d" -eq 0 ] && [ "$b" -eq 0 ]; then break; fi
  i=$((i+1)); sleep 20
done
echo "BOX-BEFORE waited=${i}x20s demo=$(pgrep -f 'DEMO --bench|DEMO --snapshot'|wc -l|tr -d ' ') build=$(pgrep -f 'ninja|cargo build|cmake --build'|wc -l|tr -d ' ') load=$(uptime|sed 's/.*averages: //')"
pmset -g therm 2>/dev/null | head -6
"$@"
rc=$?
echo "BOX-AFTER  demo=$(pgrep -f 'DEMO --bench|DEMO --snapshot'|wc -l|tr -d ' ') build=$(pgrep -f 'ninja|cargo build|cmake --build'|wc -l|tr -d ' ') load=$(uptime|sed 's/.*averages: //')"
pmset -g therm 2>/dev/null | head -6
exit $rc
