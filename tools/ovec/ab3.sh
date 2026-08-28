#!/bin/zsh
# Interleaved two-BINARY A/B with PER-BINARY flags.
# ab3.sh <binA> "<flagsA>" <binB> "<flagsB>" <scene> <t> <iters> <rounds> "<common>"
set -u
A="$1"; FA="$2"; B="$3"; FB="$4"; SC="$5"; T="$6"; IT="$7"; RND="$8"; CO="$9"
cd /Users/gil-ad/work/rev-w1impl/Runtime
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
aGI=(); aGC=(); aMS=(); aRF=(); aRM=(); bGI=(); bGC=(); bMS=(); bRF=(); bRM=()
for r in $(seq 1 $RND); do
  for who in A B; do
    if [ $who = A ]; then bin=$A; fl=$FA; else bin=$B; fl=$FB; fi
    o=$("$bin" --bench=scene@scene=$SC,t=$T,iters=$IT --deferred_prof=1 --hw_prof ${=CO} ${=fl} 2>&1)
    l=$(echo "$o" | grep -E "^\[DPROF\]     lighting-w1"); f=$(echo "$o" | grep -E "^\[DPROF\] renderFrame")
    [ -z "$l" ] && { echo "NO DPROF for $who"; echo "$o" | tail -3; exit 1; }
    gi=$(echo $l|awk '{print $(NF-2)}'); gc=$(echo $l|awk '{print $(NF-1)}'); ms=$(echo $l|awk '{print $4}')
    rf=$(echo $f|awk '{print $(NF-2)}'); rq=$(echo $f|awk '{print $4}')
    if [ $who = A ]; then aGI+=($gi); aGC+=($gc); aMS+=($ms); aRF+=($rf); aRM+=($rq)
    else bGI+=($gi); bGC+=($gc); bMS+=($ms); bRF+=($rf); bRM+=($rq); fi
  done
done
mn(){ print -l "$@" | sort -n | head -1; }
printf "A  w1 Gi=%s Gcyc=%s ms=%s | rf Gi=%s ms=%s   [w1 ms: %s]\n" "$(mn $aGI)" "$(mn $aGC)" "$(mn $aMS)" "$(mn $aRF)" "$(mn $aRM)" "$aMS"
printf "B  w1 Gi=%s Gcyc=%s ms=%s | rf Gi=%s ms=%s   [w1 ms: %s]\n" "$(mn $bGI)" "$(mn $bGC)" "$(mn $bMS)" "$(mn $bRF)" "$(mn $bRM)" "$bMS"
