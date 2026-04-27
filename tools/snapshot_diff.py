#!/usr/bin/env python3
"""Compare two snapshot PPMs and emit a diff image + numeric summary.

Usage:
    tools/snapshot_diff.py <a.ppm> <b.ppm> [<diff.png>]

The diff image highlights where pixels differ. A red overlay marks pixels
that disagree at all; brightness scales with the channel-summed absolute
difference. White pixels mean ~equal.
"""
import sys
from PIL import Image, ImageChops, ImageFilter

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    a_path, b_path = sys.argv[1], sys.argv[2]
    out_path = sys.argv[3] if len(sys.argv) > 3 else None

    a = Image.open(a_path).convert("RGB")
    b = Image.open(b_path).convert("RGB")
    if a.size != b.size:
        print(f"size mismatch: {a.size} vs {b.size}", file=sys.stderr)
        sys.exit(1)

    diff = ImageChops.difference(a, b)
    diff_pixels = diff.load()
    w, h = diff.size

    differing = 0
    max_per_channel = 0
    sum_abs = 0
    for y in range(h):
        for x in range(w):
            r, g, b_ = diff_pixels[x, y]
            s = r + g + b_
            if s > 0:
                differing += 1
                sum_abs += s
                if max(r, g, b_) > max_per_channel:
                    max_per_channel = max(r, g, b_)
    total = w * h
    pct = 100.0 * differing / total
    avg = sum_abs / max(differing, 1)
    print(f"{a_path}  vs  {b_path}")
    print(f"  size:           {w}x{h} = {total} px")
    print(f"  differing px:   {differing} ({pct:.3f}%)")
    print(f"  max channel Δ:  {max_per_channel}/255")
    print(f"  mean Δ-sum/diff:{avg:.2f} (out of 765 max)")

    if out_path:
        # Boost diff for visibility: clamp(diff*8) plus a pure-red mask of
        # any-difference pixels overlaid on the dimmed left input.
        boosted = Image.eval(diff, lambda v: min(v * 8, 255))
        base = Image.eval(a, lambda v: v // 3)
        mask = boosted.convert("L").point(lambda v: 255 if v > 0 else 0)
        red = Image.new("RGB", (w, h), (255, 0, 0))
        out = Image.composite(red, base, mask)
        out.save(out_path)
        print(f"  wrote diff:     {out_path}")

if __name__ == "__main__":
    main()
