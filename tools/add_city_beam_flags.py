#!/usr/bin/env python3
"""Stamp authored volumetric-beam flags onto the 46 'city headlight L/R'
spot blocks in Authoring/city/CITY1.LWS.

Inserts (or updates, if already present — idempotent, so gain-tuning reruns
just rewrite the value):

    VolumetricLight 1
    VolumetricLightIntensity <gain>

after each headlight block's LightRange line. Engine-glow / bilding-flare
lights are untouched (matched strictly by LightName). See tools/lwsread
LWSREAD.CPP for the keyword semantics (flag -> FLD light bit 2048 ->
Omni_ForceVolCone; intensity -> per-light cone-density gain).

Usage: add_city_beam_flags.py <gain>   (run from anywhere; paths are repo-relative)
"""
import re
import sys
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LWS = os.path.join(REPO, "Authoring", "city", "CITY1.LWS")

HEADLIGHT_NAMES = {"city headlight L", "city headlight R"}


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: add_city_beam_flags.py <gain>")
    gain = float(sys.argv[1])

    lines = open(LWS, encoding="latin-1").read().split("\n")
    starts = [i for i, l in enumerate(lines) if l.strip() == "AddLight"]
    out = []
    patched = 0
    i = 0
    for bi, lo in enumerate(starts):
        hi = starts[bi + 1] if bi + 1 < len(starts) else len(lines)
        out.extend(lines[i:lo])
        block = lines[lo:hi]
        name = next((l.strip()[len("LightName "):] for l in block
                     if l.strip().startswith("LightName ")), None)
        if name in HEADLIGHT_NAMES:
            # Drop any existing volumetric lines (idempotence), then insert
            # fresh ones after LightRange.
            block = [l for l in block
                     if not l.strip().startswith("VolumetricLight")]
            ri = next(k for k, l in enumerate(block)
                      if l.strip().startswith("LightRange "))
            block[ri + 1:ri + 1] = [
                "VolumetricLight 1",
                f"VolumetricLightIntensity {gain:.6f}",
            ]
            patched += 1
        out.extend(block)
        i = hi
    out.extend(lines[i:])

    with open(LWS, "w", encoding="latin-1") as f:
        f.write("\n".join(out))
    print(f"patched {patched} headlight blocks with gain {gain}")
    if patched != 46:
        sys.exit(f"expected 46 headlight blocks, patched {patched}")


if __name__ == "__main__":
    main()
