#!/bin/zsh
# Count the cone kernel's stack traffic in a given binary. usage: asmcount.sh <bin> <label>
set -u
BIN=$1; LABEL=${2:-x}
S=/private/tmp/claude-501/-Users-gil-ad-work-revival-fog/7ec4ee82-204d-4fb8-a259-a95020066560/scratchpad
A=$(nm -n "$BIN" | grep "VolumetricCones_Tile" | awk '{print $1}')
B=$(nm -n "$BIN" | grep -A1 "VolumetricCones_Tile" | tail -1 | awk '{print $1}')
objdump -d --start-address=0x$A --stop-address=0x$B --no-show-raw-insn "$BIN" > $S/cones_$LABEL.asm
python3 - "$S/cones_$LABEL.asm" "$LABEL" <<'PY'
import re, sys, collections
f, lab = sys.argv[1], sys.argv[2]
ops = collections.Counter(); sp = collections.Counter()
for L in open(f):
    m = re.match(r'\s*([0-9a-f]+):\s+(\S+)\s*(.*)', L)
    if not m: continue
    op, args = m.group(2), m.group(3); ops[op]+=1
    if op.startswith(('ldr','ldur','ldp','str','stur','stp','ld1','st1')):
        stack = bool(re.search(r'\[(sp|x29)[,\]]', args))
        # q-register (128-bit vector) traffic vs scalar
        q = args.lstrip().startswith('q')
        kind = ('ld' if op[0]=='l' else 'st')
        sp[(kind, 'stack' if stack else 'heap', 'q' if q else 's')] += 1
tot = sum(ops.values())
def g(k,s,q): return sp.get((k,s,q),0)
print(f"{lab:12s} total={tot:5d}  "
      f"stackQ ld/st={g('ld','stack','q'):4d}/{g('st','stack','q'):4d}  "
      f"stackS ld/st={g('ld','stack','s'):4d}/{g('st','stack','s'):4d}  "
      f"heap ld/st={g('ld','heap','q')+g('ld','heap','s'):4d}/{g('st','heap','q')+g('st','heap','s'):4d}  "
      f"fmul.4s={ops.get('fmul.4s',0)} fmla.4s={ops.get('fmla.4s',0)} dup.4s={ops.get('dup.4s',0)}")
PY
