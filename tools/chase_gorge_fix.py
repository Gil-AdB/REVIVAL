#!/usr/bin/env python3
"""Lift BOTH chase ships' flight paths through the mm7 gorge so their hulls stop
clipping the canyon walls/floor (the user's "after the loop the leading ship is
clipping the canyon walls").

Family: tools/chase_loop.py / chase_bank.py / chase_camera.py — in-place,
idempotent, re-runnable. Authored-first: the result is edited ship ObjectMotion
values in Authoring/chase/CHASE.LWS, editor-visible after lwsread_legacy regen.

ROOT CAUSE (measured — point-to-triangle 3D distance from each ship's centre to
every mountain mesh, per frame, plus a loop-on/loop-off path diff):
  * The loop (dc5c08e @500-580) is NOT the cause: with the loop removed the
    post-loop path (f>580) is byte-identical (max |Δpos| = 0). The loop-align
    fix (cc8a8f8) re-expresses only PITCH on post-window keys, never position.
  * EXCLUDING mm7, both ships clear every other mountain by a wide margin over
    the whole path (Ship1 >= 304). The clips are:
      Ship1 (hull radius ~175): mm7 gorge f1144-1186, min surface dist 23.
      ship2 (hull radius ~264, the bigger foreground pursuer): mm7 gorge entry
        f1082-1132 (dist 87) + mid f1153-1192 (209), AND m4 at the gorge EXIT
        f1326-1335 (dist 127).
  * mm7.lwo is the single big gorge massif (Y up to 2554); the ships bore
    through its canyon. It is a floor/shelf clip at the worst frames.

FIX: a tapered Y-lift over each ship's gorge keys (both flew flat/low through a
rising canyon). Ship1 peak +450 (23 -> 133; the delta-wing is thin vertically so
133 to the floor below is comfortable belly clearance — verified by render).
ship2 peak +480 over its gorge keys, PLUS a lateral (+X,+Z) nudge on its exit key
f1330 because m4 there is TALL (can't be cleared by lift alone) — opens 127 ->
~965. mm7/m4 are NOT moved (landmark composition preserved). VALUE-ONLY edits —
key COUNTS are unchanged (Ship1 68, ship2 30), so no lwsread motion-key miscount.

Idempotent: sets ABSOLUTE channel values (re-run = no-op); --revert restores the
committed baseline. Only the listed channels of the matched keys are touched.

Usage:
    tools/chase_gorge_fix.py            # apply the lift
    tools/chase_gorge_fix.py --revert   # restore baseline
Then: regen (lwsread_legacy) + install, or run ./chase_preview.sh <variant>.
"""
import os, sys, argparse

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LWS  = os.path.join(REPO, "Authoring", "chase", "CHASE.LWS")

# Per-ship gorge edits. frame -> dict of channel edits, each an ABSOLUTE target
# for apply and the BASELINE for --revert:
#   y = (baselineY, appliedY)   x/z = (baselineX, appliedX) ...
# appliedY = baselineY + peak*taper; x/z applied = baseline + nudge (ship2 exit).
def _y(base, taper, peak):
    return (base, round(base + peak * taper, 4))

SHIP1_PEAK = 450.0
SHIP1 = {
    1028: {"y": _y(530.2166, 0.15, SHIP1_PEAK)},
    1066: {"y": _y(750.0,    0.45, SHIP1_PEAK)},
    1098: {"y": _y(875.0,    0.85, SHIP1_PEAK)},
    1135: {"y": _y(800.0,    1.00, SHIP1_PEAK)},
    1196: {"y": _y(742.9814, 0.80, SHIP1_PEAK)},
    1236: {"y": _y(675.0,    0.40, SHIP1_PEAK)},
    1306: {"y": _y(350.0,    0.10, SHIP1_PEAK)},
}

SHIP2_PEAK = 480.0
SHIP2 = {
    1028: {"y": _y(245.0, 0.15, SHIP2_PEAK)},
    1074: {"y": _y(755.0, 0.70, SHIP2_PEAK)},
    1105: {"y": _y(755.0, 1.00, SHIP2_PEAK)},
    1135: {"y": _y(755.0, 1.00, SHIP2_PEAK)},
    1156: {"y": _y(755.0, 0.95, SHIP2_PEAK)},
    1182: {"y": _y(755.0, 0.75, SHIP2_PEAK)},
    1212: {"y": _y(755.0, 0.50, SHIP2_PEAK)},
    1263: {"y": _y(485.0, 0.40, SHIP2_PEAK)},
    # exit turn: lift + lateral nudge (m4 is tall — lift alone can't clear it)
    1284: {"y": _y(485.0, 0.55, SHIP2_PEAK),
           "x": (-24811.0, -24811.0 + 240.0), "z": (12754.5, 12754.5 + 240.0)},
    1330: {"y": _y(625.0, 0.78, SHIP2_PEAK),
           "x": (-28287.0, -28287.0 + 600.0), "z": (18731.0, 18731.0 + 600.0)},
    1371: {"y": _y(485.0, 0.20, SHIP2_PEAK),
           "x": (-30921.0, -30921.0 + 240.0), "z": (23684.0, 23684.0 + 240.0)},
}

CHAN = {"x": 0, "y": 1, "z": 2}


def fmt(v):
    if abs(v) < 1e-9:
        return "0"
    return ("%.4f" % v).rstrip("0").rstrip(".")


def find_ship_motion(lines, want):
    n = len(lines); i = 0
    while i < n and not (lines[i].strip().startswith("LoadObject")
                         and want in lines[i].lower()):
        i += 1
    if i >= n:
        sys.exit("no LoadObject %s" % want)
    while i < n and not lines[i].strip().startswith("ObjectMotion"):
        i += 1
    def nxt(m):
        while m < n and lines[m].strip() == "": m += 1
        return m
    ci = nxt(i + 1); ki = nxt(ci + 1)
    nk = int(lines[ki].split()[0]); k = nxt(ki + 1)
    return nk, k, nxt


def edit_ship(lines, want, table, revert):
    nk, k, nxt = find_ship_motion(lines, want)
    changed = 0
    for _ in range(nk):
        k = nxt(k)
        f2 = nxt(k + 1)
        fr = int(round(float(lines[f2].split()[0])))
        if fr in table:
            v = lines[k].split()
            for ch, (base, applied) in table[fr].items():
                v[CHAN[ch]] = fmt(base if revert else applied)
            lines[k] = "  " + " ".join(v)
            changed += 1
        k = nxt(f2 + 1)
    return changed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--revert", action="store_true")
    args = ap.parse_args()

    lines = open(LWS, encoding="latin-1").read().split("\n")
    c1 = edit_ship(lines, "ship1", SHIP1, args.revert)
    c2 = edit_ship(lines, "ship2", SHIP2, args.revert)
    open(LWS, "w", encoding="latin-1").write("\n".join(lines))
    print("%s gorge fix: Ship1 %d keys, ship2 %d keys"
          % ("REVERTED" if args.revert else "applied", c1, c2))
    print("NOW: regen (lwsread_legacy) + install")


if __name__ == "__main__":
    main()
