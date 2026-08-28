#!/bin/zsh
# Interleaved A,B,A,B,... min-of-N. usage:
#   ab.sh <bin> <scene> <t> <iters> <rounds> ARMS... -- COMMON...
set -u
BIN="$1"; SC="$2"; T="$3"; IT="$4"; RND="$5"; shift 5
ARMS=(); while [ "$1" != "--" ]; do ARMS+=("$1"); shift; done; shift
COMMON=("$@")
cd "$(dirname "$BIN")"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
typeset -A W1GI W1GC W1MS RFGI RFMS
for r in $(seq 1 $RND); do
  for a in "${ARMS[@]}"; do
    o=$("$BIN" --bench=scene@scene=$SC,t=$T,iters=$IT --deferred_prof=1 --hw_prof "${COMMON[@]}" ${=a} 2>&1)
    l=$(echo "$o" | grep -E "^\[DPROF\]     lighting-w1")
    f=$(echo "$o" | grep -E "^\[DPROF\] renderFrame")
    W1GI[$a]="${W1GI[$a]:-} $(echo $l|awk '{print $(NF-2)}')"
    W1GC[$a]="${W1GC[$a]:-} $(echo $l|awk '{print $(NF-1)}')"
    W1MS[$a]="${W1MS[$a]:-} $(echo $l|awk '{print $4}')"
    RFGI[$a]="${RFGI[$a]:-} $(echo $f|awk '{print $(NF-2)}')"
    RFMS[$a]="${RFMS[$a]:-} $(echo $f|awk '{print $4}')"
  done
done
mn() { echo $1 | tr ' ' '\n' | grep -v '^$' | sort -n | head -1; }
printf "%-58s | %-8s %-8s %-9s | %-8s %-9s\n" ARM w1_Gi w1_Gcyc w1_ms_min rf_Gi rf_ms_min
for a in "${ARMS[@]}"; do
  printf "%-58s | %-8s %-8s %-9s | %-8s %-9s   [w1 ms all: %s]\n" "${a:-BASE(all on)}" \
    "$(mn "${W1GI[$a]}")" "$(mn "${W1GC[$a]}")" "$(mn "${W1MS[$a]}")" \
    "$(mn "${RFGI[$a]}")" "$(mn "${RFMS[$a]}")" \
    "$(echo ${W1MS[$a]} | tr ' ' '\n' | grep -v '^$' | sort -n | tr '\n' ' ')"
done
