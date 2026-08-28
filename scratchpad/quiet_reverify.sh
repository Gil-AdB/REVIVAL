#!/bin/zsh
# Wait for a quiet box, then re-run the headline battery and record the
# before/after box state so a contaminated run can be thrown away.
set -u
WT=/Users/gil-ad/work/rev-perfmap
OUT=$WT/scratchpad/perfmap28_quiet.json
for i in $(seq 1 90); do
  busy=$(pgrep -fc 'build-abl|cmake --build|ninja|Runtime/DEMO' 2>/dev/null || echo 0)
  ld=$(python3 -c 'import os;print("%.2f"%os.getloadavg()[0])')
  if [ "$busy" = "0" ] && [ "$(python3 -c "print(1 if $ld<3.0 else 0)")" = "1" ]; then break; fi
  sleep 20
done
print -r -- "QUIET-WAIT ended after $i polls; load=$(uptime | sed 's/.*averages: //') busy=$busy"
print -r -- "BEFORE: $(uptime | sed 's/.*averages: //')  batt=$(pmset -g batt | tail -1 | tr -s ' ')"
PM_ITERS=24 PM_ROUNDS=12 PM_OUT=$OUT python3 $WT/scratchpad/perfmap28.py 2>&1 | tail -14
print -r -- "AFTER: $(uptime | sed 's/.*averages: //')  busy_now=$(pgrep -fc 'build-abl|cmake --build|ninja' 2>/dev/null || echo 0)"
print -r -- "BATTERY-DONE"
