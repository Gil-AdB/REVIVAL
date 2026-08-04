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
INCLUDE_FDS="/ow/h:$ROOT/FDS/SOURCE:$ROOT/FDS:$ROOT/FDS/JPEG6"   # Source/ -> FDS/SOURCE
INCLUDE_REV="/ow/h:$REV"                            # Source/ -> REVIVAL/Source (symlinks below)
# Give REVIVAL a Source/ that points back at its own headers so
# "Source/FDS_Vars.H" resolves to the DEMO header (with GridPoint).
mkdir -p "$REV/Source"
for h in FDS_DEFS FDS_VARS FDS_DECS; do ln -sf "../$h.H" "$REV/Source/$h.H"; done

# Original flags: -bt=dos -5r -5 -fp5 -fpi87 -oxl+  (Pentium reg-call, 387 inline, max opt)
# Optional macro defines passed to BOTH wpp386 (C) and wasm (ASM), e.g. FDS_TEX_SWIZZLE=1
# enables the 8-wide texture tiling (build-side reorder in IMGPROC.CPP + tiled fetch in the filler).
DEFS=""
[ -n "${FDS_TEX_SWIZZLE:-}" ] && DEFS="$DEFS -dFDS_TEX_SWIZZLE"
[ -n "${FDS_FUSED_GOUR:-}" ] && DEFS="$DEFS -dFDS_FUSED_GOUR"
[ -n "${FDS_CWOB:-}" ] && DEFS="$DEFS -dFDS_CWOB"
[ -n "${FDS_MMXWOB:-}" ] && DEFS="$DEFS -dFDS_MMXWOB"
[ -n "${FDS_ZBUF:-}" ] && DEFS="$DEFS -dFDS_ZBUF"
[ -n "${FDS_ZLINEAR:-}" ] && DEFS="$DEFS -dFDS_ZLINEAR"
[ -n "${FDS_ZFLIP:-}" ] && DEFS="$DEFS -dFDS_ZFLIP"
[ -n "${FDS_NOLOG:-}" ] && DEFS="$DEFS -dFDS_NOLOG"
[ -n "${FDS_NOFLARE:-}" ] && DEFS="$DEFS -dFDS_NOFLARE"
[ -n "${FDS_PATHVIZ:-}" ] && DEFS="$DEFS -dFDS_PATHVIZ"
[ -n "${FDS_NOSWEEP:-}" ] && DEFS="$DEFS -dFDS_NOSWEEP"
[ -n "${FDS_NOOWN:-}" ] && DEFS="$DEFS -dFDS_NOOWN"
[ -n "${FDS_NULLFILL_DIAG:-}" ] && DEFS="$DEFS -dFDS_NULLFILL_DIAG"
[ -n "${FDS_MAPTEST:-}" ] && DEFS="$DEFS -dFDS_MAPTEST"
[ -n "${FDS_XPARVIZ:-}" ] && DEFS="$DEFS -dFDS_XPARVIZ"
[ -n "${FDS_CLIPCHK:-}" ] && DEFS="$DEFS -dFDS_CLIPCHK"
[ -n "${FDS_GLATBENCH:-}" ] && DEFS="$DEFS -dFDS_GLATBENCH"
# Tiled (8-wide vertical strip) wobbler texture fetch, for BOTH the C layer mapper and the
# plane's MMX ASM. ON by default - proven byte-identical to the row-major mappers (host
# harness over 48 full frames; in-DOS WOBCHK for the ASM). FDS_WOBROW=1 gets the old fetch.
[ -z "${FDS_WOBROW:-}" ] && DEFS="$DEFS -dFDS_WOBTILE"
[ -n "${FDS_WOBCHK:-}" ] && DEFS="$DEFS -dFDS_WOBCHK"    # per-frame byte-compare of the two plane mappers
[ -n "${FDS_SUBTEX:-}" ] && DEFS="$DEFS -dFDS_SUBTEX"    # sub-texel X prestep in the span setup
[ -n "${FDS_NGON:-}" ] && DEFS="$DEFS -dFDS_NGON"        # polygon screen clip + n-gon edge walker
[ -n "${FDS_NGON_NOCLIP:-}" ] && DEFS="$DEFS -dFDS_NGON_NOCLIP"  # bisect: n-gon walker on UNCUT polys only
[ -n "${FDS_NGONLOG:-}" ] && DEFS="$DEFS -dFDS_NGONLOG"    # probe: clip vertices outside the input range
[ -n "${FDS_SNAP:-}" ] && DEFS="$DEFS -dFDS_SNAP"          # write CITY/GLAT/GRT/FNT *.PPM frame dumps (~5MB of disk I/O)
[ -n "${FDS_LOADPROF:-}" ] && DEFS="$DEFS -dFDS_LOADPROF"  # A2: cold/warm re-read of every texture (adds ~2x file I/O)
[ -n "${FDS_TEXDUMP:-}" ] && DEFS="$DEFS -dFDS_TEXDUMP"    # dump first 4 RAW decoded textures (decoder A/B evidence)
[ -n "${FDS_NEARSPHERE:-}" ] && DEFS="$DEFS -dFDS_NEARSPHERE"          # Tri_Ahead vs NEAR plane, not Z=0
[ -n "${FDS_NEARSPHERE_PROBE:-}" ] && DEFS="$DEFS -dFDS_NEARSPHERE_PROBE"  # log meshes whose Tri_Ahead promise is broken
[ -n "${FDS_ARMPROBE:-}" ] && DEFS="$DEFS -dFDS_ARMPROBE"            # per-mesh face fate when the near plane CUTS a mesh
# LOGSYNC = fflush instead of close+reopen. DEFAULT ON: 482 of those fire while CITY.FLD
# loads and each is a FAT dir search + flush + seek-to-end via int21. Measured on 86Box:
# LoadFLD 10.24s -> 1.15s, whole startup 20.47s -> 8.00s. The only thing lost is log
# durability across a HARD HANG -- set FDS_LOGSYNC_SAFE=1 to get the close/reopen back when
# hunting one. (FDS_NOLOG is still the deploy setting: 7.16s, no log at all.)
FDS_FASTLOG="${FDS_FASTLOG:-1}"
[ -n "${FDS_LOGSYNC_SAFE:-}" ] && FDS_FASTLOG=""
[ -n "$FDS_FASTLOG" ] && DEFS="$DEFS -dFDS_FASTLOG"
# Decode JPEGs with IJG jpeg-6b (FDS/JPEG6) instead of the prebuilt 1998 JPEGLIBD.LIB (v5).
# The v5 lib routes decode through its djpeg TARGA WRITER and opens the file itself; ours
# decodes from one in-memory read straight into the destination rows. IDCT: ifast (default,
# v5 had no integer IDCT), or FDS_JPEG6_IDCT=islow|float.
# DEFAULT ON with the islow (accurate integer) IDCT. 86Box, 53 textures: decode 1147.6 ->
# 859.1 Mcy (-25%), texture load 5.51s -> 4.27s. Max delta vs the v5 lib is 4 on the raw
# decoded texture, invisible at x12 amplification; ifast is a further -9% but rings visibly
# along every 8x8 block edge. Set FDS_JPEG_V5=1 to go back to the 1998 lib (explicit A/B leg;
# do NOT rely on "no flag" meaning old behaviour now that the default has flipped).
FDS_JPEG6="${FDS_JPEG6:-1}"
[ -n "${FDS_JPEG_V5:-}" ] && FDS_JPEG6=""
[ -n "$FDS_JPEG6" ] && DEFS="$DEFS -dFDS_JPEG6"
case "${FDS_JPEG6_IDCT:-islow}" in
  islow) DEFS="$DEFS -dFDS_JPEG6_ISLOW" ;;
  float) DEFS="$DEFS -dFDS_JPEG6_FLOAT" ;;
  ifast) ;;   # jpeg-6b's own default; no define needed
esac
[ -n "${FDS_JPEG6_FASTUP:-}" ] && DEFS="$DEFS -dFDS_JPEG6_FASTUP"   # replicating chroma upsample (changes the LOOK)
# Fused gouraud sweep chunking: 64px measured best on 86Box (27.91 -> 27.31 Mcy/frame).
# Default ON; set FDS_GOURCHUNK=0 for the old per-scanline sweep.
FDS_GOURCHUNK="${FDS_GOURCHUNK:-64}"
[ "$FDS_GOURCHUNK" != "0" ] && DEFS="$DEFS -dFDS_GOURCHUNK=$FDS_GOURCHUNK"
[ -n "${FDS_SUBTEX_MINH:-}" ] && DEFS="$DEFS -dFDS_SUBTEX_MINH"  # keep the integer coverage decision
[ -n "${FDS_XPARLOG:-}" ] && DEFS="$DEFS -dFDS_XPARLOG"  # log every xpar face before the mapper
[ -n "${FDS_SUBTEX_NEG:-}" ] && DEFS="$DEFS -dFDS_SUBTEX_NEG"  # prestep sign A/B
CFLAGS="-bt=dos -5r -5 -fp5 -fpi87 -oxl+ -zq -fr=/dev/null $DEFS"
AFLAGS="-bt=dos -5r -zq -fr=/dev/null $DEFS"

cpp_ok=0; cpp_fail=0; asm_ok=0; asm_fail=0
fails=""

# Canonical FDS translation units — ONE per module (the ones LINKALL.BAT /
# MAKE*.BAT actually compiled). Other .CPP in a module are #include fragments.
FDS_TUS="DPMI/DPMI ISR/ISR VESA/VESA MDSPLAY/MDSPLAY MATH/MATH V3D/V3D_READ \
3DS/3DS_READ FLD/FLD_READ FLD/FLD_CONV FLD/FLD_MAT IMGCODE/IMGCODE IMGCODE/JPEG6LD IMGPROC/IMGPROC \
IMGGENR/IMGGENR CAMERAS/CAMERAS FRUSTRUM/FRUSTRUM RENDER/RENDER FILLERS/FILLERS \
PCLSYS/PCLSYS MISC/PREPROC MISC/MOUSE MISC/TABLES"

JPEG6_OBJS=""
if [ -n "${FDS_JPEG6:-}" ]; then
  echo "=== compiling IJG jpeg-6b (C) ==="
  # IJG's own makefile.wat uses -4r -ort; we match the rest of the tree (-5r -fp5 -oxl+).
  # No -j: Watcom's char is unsigned by default, which is what jconfig.wat assumes.
  JCFLAGS="-bt=dos -5r -5 -fp5 -fpi87 -oxl+ -zq -wx -fr=/dev/null $DEFS"
  jok=0; jfail=0
  # Objects go in a SUBDIR: $OUT/*.o is globbed wholesale at link time, so leaving them
  # there would (a) double-link them and (b) collide with JPEGLIBD.LIB on the next
  # FDS_JPEG6-off build, since nothing ever cleans $OUT.
  JOUT="$OUT/jpeg6"; mkdir -p "$JOUT"
  export INCLUDE="/ow/h:$ROOT/FDS/JPEG6"
  for f in "$ROOT"/FDS/JPEG6/*.c; do
    o="$JOUT/$(basename "${f%.c}").o"
    if wcc386 $JCFLAGS -fo="$o" "$f" >/tmp/jc.log 2>&1; then
      jok=$((jok+1)); JPEG6_OBJS="$JPEG6_OBJS,$o"
    else
      jfail=$((jfail+1)); echo "--- FAIL: ${f#$ROOT/}"; grep -iE 'error' /tmp/jc.log | head -5
    fi
  done
  echo "jpeg-6b: ok=$jok fail=$jfail"
fi

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

# Row-major twin mappers for runtime-generated (dynamic) textures in the swizzle build:
# same sources, -dFDS_TEXLIN renames the PUBLICs (P_TextureLin_32_/PT_TextureLin_32_) and
# leaves FDS_TEX_SWIZZLE undefined so the classic row-major inner loops assemble.
if [ -n "${FDS_TEX_SWIZZLE:-}" ]; then
  for b in BITRUE BITTRUE; do
    if wasm -bt=dos -5r -zq -fr=/dev/null ${FDS_ZBUF:+-dFDS_ZBUF} ${FDS_ZLINEAR:+-dFDS_ZLINEAR} ${FDS_SUBTEX:+-dFDS_SUBTEX} ${FDS_SUBTEX_MINH:+-dFDS_SUBTEX_MINH} ${FDS_NGON:+-dFDS_NGON} -dFDS_TEXLIN -fo="$OUT/${b}LIN.o" "$FDS/FILLERS/$b.ASM" >/tmp/as.log 2>&1; then
      asm_ok=$((asm_ok+1))
    else
      asm_fail=$((asm_fail+1)); fails="$fails\n[ASM-LIN] $b"
      echo "--- FAIL: ${b}LIN"; grep -iE 'error|Error' /tmp/as.log | head -4
    fi
  done
fi

echo "=== compiling demo scenes (C++) ==="
export INCLUDE="$INCLUDE_REV"
for f in "$REV"/REV.CPP "$REV"/DOSRECOVER.CPP "$REV"/SHIT.CPP "$REV"/CITY.CPP "$REV"/FOUNTAIN.CPP "$REV"/GREETS.CPP "$REV"/GLAT.CPP "$REV"/CHASE.CPP "$REV"/CRASH.CPP "$REV"/CREDITS.CPP "$REV"/TESTMAP.CPP; do
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
  # nocaseexact: 1998 libs mangle C++ names lowercase (W?LoadARJ$n(pnapna)pna),
  # OW2 uppercase ($N/PNA) — match case-insensitively.
  # FDS_JPEG6 replaces the v5 lib wholesale -- linking both would duplicate every
  # jpeg_* symbol, and our own LoadJPEG supersedes the one in its DJPEG.C member.
  if [ -n "${FDS_JPEG6:-}" ]; then
    JPEGLIB=""; LINKOBJS="$LINKOBJS${JPEG6_OBJS}"
  else
    JPEGLIB='library '"$ROOT"'/FDS/JPEGLIBD.LIB'
  fi
  wlink system dos4g name "$OUT/REV.EXE" option quiet,nocaseexact,stack=256k,map=$OUT/REV.MAP \
    file "$LINKOBJS" \
    library "$ROOT/FDS/MIDAS.LIB" $JPEGLIB library "$ROOT/FDS/UNARJ.LIB" \
    2>&1 | grep -iE 'error|undefined' | sort -u | head -40
  echo "---"
  if [ -f "$OUT/REV.EXE" ]; then echo "LINK OK: $(ls -l "$OUT/REV.EXE" | awk '{print $5}') bytes"; else echo "LINK FAILED"; fi
fi
