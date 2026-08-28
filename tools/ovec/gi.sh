#!/bin/zsh
# Ginstr/f + Gcyc/f for one phase. $1=binary $2=scene $3=t $4=iters $5..=extra flags
set -u
BIN="$1"; SC="$2"; T="$3"; IT="$4"; shift 4
cd "$(dirname "$BIN")" 2>/dev/null || cd /Users/gil-ad/work/rev-w1impl/Runtime
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$BIN" \
  --bench=scene@scene=$SC,t=$T,iters=$IT --deferred_prof=1 --hw_prof "$@" 2>&1 |
  grep -E "^\[DPROF\] (renderFrame| +(lighting-w1|DeferredLighting-call|cones-call))"
