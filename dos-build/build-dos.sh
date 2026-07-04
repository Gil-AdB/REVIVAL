#!/bin/bash
# Reconstructed Open Watcom cross-build of the DOS Revival demo.
# Runs INSIDE the linux/amd64 container (see build-dos-container.sh).
# rev-build/ mounted at /work, Open Watcom at /ow.
set -u
ROOT=/work
FDS=$ROOT/FDS/SOURCE
REV=$ROOT/REVIVAL
OUT=$ROOT/_build
mkdir -p "$OUT"

export WATCOM=/ow
export PATH=/ow/binl64:$PATH
# The engine and the demo each have their OWN FDS_{Defs,Vars,Decs}.H, and
# "Source/FDS_Vars.H" means the ENGINE copy inside FDS/ but the DEMO copy inside
# REVIVAL/ (the original built them from separate dirs). GridPoint lives ONLY in
# REVIVAL/FDS_VARS.H, so REVIVAL files must NOT pick up the engine header.
# Two include sets, selected per file group:
INCLUDE_FDS="/ow/h:$ROOT/FDS/SOURCE:$ROOT/FDS"     # Source/ -> FDS/SOURCE
INCLUDE_REV="/ow/h:$REV"                            # Source/ -> REVIVAL/Source (symlinks below)
# Give REVIVAL a Source/ that points back at its own headers so
# "Source/FDS_Vars.H" resolves to the DEMO header (with GridPoint).
mkdir -p "$REV/Source"
for h in FDS_DEFS FDS_VARS FDS_DECS; do ln -sf "../$h.H" "$REV/Source/$h.H"; done

# Original flags: -bt=dos -5r -5 -fp5 -fpi87 -oxl+  (Pentium reg-call, 387 inline, max opt)
CFLAGS="-bt=dos -5r -5 -fp5 -fpi87 -oxl+ -zq -fr=/dev/null"
AFLAGS="-bt=dos -5r -zq -fr=/dev/null"

cpp_ok=0; cpp_fail=0; asm_ok=0; asm_fail=0
fails=""

# Canonical FDS translation units — ONE per module (the ones LINKALL.BAT /
# MAKE*.BAT actually compiled). Other .CPP in a module are #include fragments.
FDS_TUS="DPMI/DPMI ISR/ISR VESA/VESA MDSPLAY/MDSPLAY MATH/MATH V3D/V3D_READ \
3DS/3DS_READ FLD/FLD_READ IMGCODE/IMGCODE IMGPROC/IMGPROC IMGGENR/IMGGENR \
CAMERAS/CAMERAS FRUSTRUM/FRUSTRUM RENDER/RENDER FILLERS/FILLERS PCLSYS/PCLSYS \
MISC/PREPROC"

echo "=== compiling FDS engine (C++) ==="
export INCLUDE="$INCLUDE_FDS"
for m in $FDS_TUS; do
  f="$FDS/$m.CPP"
  [ -f "$f" ] || { echo "MISSING TU: $f"; continue; }
  o="$OUT/$(basename "${f%.CPP}").o"
  if wpp386 $CFLAGS -fo="$o" "$f" >/tmp/cc.log 2>&1; then
    cpp_ok=$((cpp_ok+1))
  else
    cpp_fail=$((cpp_fail+1)); fails="$fails\n[CPP] $f"
    echo "--- FAIL: ${f#$ROOT/}"; grep -iE 'error|Error' /tmp/cc.log | head -6
  fi
done

echo "=== assembling FDS engine (ASM) ==="
while IFS= read -r f; do
  o="$OUT/$(basename "${f%.ASM}").o"
  if wasm $AFLAGS -fo="$o" "$f" >/tmp/as.log 2>&1; then
    asm_ok=$((asm_ok+1))
  else
    asm_fail=$((asm_fail+1)); fails="$fails\n[ASM] $f"
    echo "--- FAIL: ${f#$ROOT/}"; grep -iE 'error|Error' /tmp/as.log | head -4
  fi
done < <(find "$FDS" -name '*.ASM' | sort)

echo "=== compiling demo scenes (C++) ==="
export INCLUDE="$INCLUDE_REV"
for f in "$REV"/REV.CPP "$REV"/SHIT.CPP "$REV"/CITY.CPP "$REV"/FOUNTAIN.CPP "$REV"/GREETS.CPP "$REV"/GLAT.CPP "$REV"/CHASE.CPP "$REV"/CRASH.CPP "$REV"/CREDITS.CPP; do
  [ -f "$f" ] || continue
  o="$OUT/$(basename "${f%.CPP}").o"
  if wpp386 $CFLAGS -fo="$o" "$f" >/tmp/cc.log 2>&1; then
    cpp_ok=$((cpp_ok+1))
  else
    cpp_fail=$((cpp_fail+1)); fails="$fails\n[REV] $f"
    echo "--- FAIL: ${f#$ROOT/}"; grep -iE 'error|Error' /tmp/cc.log | head -4
  fi
done

echo ""
echo "=== SUMMARY: cpp ok=$cpp_ok fail=$cpp_fail | asm ok=$asm_ok fail=$asm_fail ==="
echo "objects produced: $(ls "$OUT"/*.o 2>/dev/null | wc -l)"

if [ "$cpp_fail" = 0 ] && [ "$asm_fail" = 0 ]; then
  echo ""
  echo "=== linking REV.EXE (system dos4g) ==="
  LINKOBJS=""
  for o in "$OUT"/*.o; do
    b=$(basename "$o" .o)
    case "$b" in CHASE|CRASH|CREDITS) continue;; esac   # not in the release sequence
    LINKOBJS="$LINKOBJS,$o"
  done
  LINKOBJS=${LINKOBJS#,}
  wlink system dos4g name "$OUT/REV.EXE" option quiet \
    file "$LINKOBJS" \
    library "$ROOT/FDS/MIDAS.LIB" library "$ROOT/FDS/JPEGLIBD.LIB" library "$ROOT/FDS/UNARJ.LIB" \
    2>&1 | grep -iE 'error|undefined' | sort -u | head -40
  echo "---"
  if [ -f "$OUT/REV.EXE" ]; then echo "LINK OK: $(ls -l "$OUT/REV.EXE" | awk '{print $5}') bytes"; else echo "LINK FAILED"; fi
fi
