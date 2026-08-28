#!/bin/zsh
# Interleaved two-BINARY A/B (same worktree, same assets). min-of-N.
set -u
A="$1"; B="$2"; SC="$3"; T="$4"; IT="$5"; RND="$6"; shift 6
cd /Users/gil-ad/work/rev-w1impl/Runtime
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
aGI=(); aGC=(); aMS=(); aRF=(); aRM=(); bGI=(); bGC=(); bMS=(); bRF=(); bRM=()
for r in $(seq 1 $RND); do
  for who in A B; do
    bin=$A; [ $who = B ] && bin=$B
    o=$("$bin" --bench=scene@scene=$SC,t=$T,iters=$IT --deferred_prof=1 --hw_prof "$@" 2>&1)
    l=$(echo "$o" | grep -E "^\[DPROF\]     lighting-w1"); f=$(echo "$o" | grep -E "^\[DPROF\] renderFrame")
    gi=$(echo $l|awk '{print $(NF-2)}'); gc=$(echo $l|awk '{print $(NF-1)}'); ms=$(echo $l|awk '{print $4}')
    rf=$(echo $f|awk '{print $(NF-2)}'); rm=$(echo $f|awk '{print $4}')
    if [ $who = A ]; then aGI+=($gi); aGC+=($gc); aMS+=($ms); aRF+=($rf); aRM+=($rm)
    else bGI+=($gi); bGC+=($gc); bMS+=($ms); bRF+=($rf); bRM+=($rm); fi
  done
done
mn(){ print -l "$@" | sort -n | head -1; }
printf "A(parent) w1 Gi=%s Gcyc=%s ms=%s | rf Gi=%s ms=%s   [w1 ms: %s]\n" "$(mn $aGI)" "$(mn $aGC)" "$(mn $aMS)" "$(mn $aRF)" "$(mn $aRM)" "$aMS"
printf "B(child)  w1 Gi=%s Gcyc=%s ms=%s | rf Gi=%s ms=%s   [w1 ms: %s]\n" "$(mn $bGI)" "$(mn $bGC)" "$(mn $bMS)" "$(mn $bRF)" "$(mn $bRM)" "$bMS"
