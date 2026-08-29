#!/bin/bash
# Box-quiet wrapper using ps -Ao comm= (comm only, no args) so it cannot
# false-match another agent's wait-loop command line -- the trap that stalled
# quietrun.sh: its `pgrep -f 'ninja|cargo build|cmake --build'` matched a peer
# agent's *waiting* shell, whose command string contains those literals.
busy() { ps -Ao comm= | grep -cE '(^|/)(DEMO|ninja|cc1plus|clang\+\+|cargo)$'; }
i=0
while [ $i -lt 60 ]; do
  [ "$(busy)" -eq 0 ] && break
  i=$((i+1)); sleep 20
done
echo "BOX-BEFORE waited=${i}x20s busy=$(busy) load=$(uptime|sed 's/.*averages: //')"
"$@"; rc=$?
echo "BOX-AFTER  busy=$(busy) load=$(uptime|sed 's/.*averages: //')"
exit $rc
