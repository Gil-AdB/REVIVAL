#!/bin/zsh
# Interleaved two-binary A/B for the SSAO scopes. min-of-N.
set -u
A="$1"; B="$2"; SC="$3"; T="$4"; IT="$5"; RND="$6"; shift 6
cd /Users/gil-ad/work/rev-w1impl/Runtime
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
typeset -A R
for r in $(seq 1 $RND); do
  for who in A B; do
    bin=$A; [ $who = B ] && bin=$B
    o=$("$bin" --bench=scene@scene=$SC,t=$T,iters=$IT --deferred_prof=1 --hw_prof "$@" 2>&1)
    for row in ssao ssao-march ssao-apply ssao-blur renderFrame; do
      l=$(echo "$o" | grep -E "^\[DPROF\] +${row} " | head -1)
      [ -z "$l" ] && l=$(echo "$o" | grep -E "^\[DPROF\]( +)${row}( |$)" | head -1)
      ms=$(echo $l|awk '{print $4}'); gi=$(echo $l|awk '{print $(NF-2)}'); gc=$(echo $l|awk '{print $(NF-1)}')
      R[$who,$row,ms]="${R[$who,$row,ms]:-} $ms"; R[$who,$row,gi]="${R[$who,$row,gi]:-} $gi"; R[$who,$row,gc]="${R[$who,$row,gc]:-} $gc"
    done
  done
done
mn(){ echo $1 | tr ' ' '\n' | grep -v '^$' | sort -n | head -1; }
printf "%-12s | %-9s %-9s | %-9s %-9s | %-9s %-9s\n" row A_ms B_ms A_Gi B_Gi A_Gcyc B_Gcyc
for row in ssao ssao-march ssao-apply ssao-blur renderFrame; do
  printf "%-12s | %-9s %-9s | %-9s %-9s | %-9s %-9s\n" $row \
   "$(mn "${R[A,$row,ms]}")" "$(mn "${R[B,$row,ms]}")" \
   "$(mn "${R[A,$row,gi]}")" "$(mn "${R[B,$row,gi]}")" \
   "$(mn "${R[A,$row,gc]}")" "$(mn "${R[B,$row,gc]}")"
done
