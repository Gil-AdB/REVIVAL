#!/bin/zsh
set -u
echo "BEFORE: DEMO procs=$(pgrep -f 'Runtime/DEMO|build.*/DEMO/DEMO' | wc -l | tr -d ' ')  load=$(uptime | sed 's/.*averages: //')"
/Users/gil-ad/work/rev-w1impl/bin/ab3.sh "$@"
echo "AFTER:  DEMO procs=$(pgrep -f 'Runtime/DEMO|build.*/DEMO/DEMO' | wc -l | tr -d ' ')  load=$(uptime | sed 's/.*averages: //')"
