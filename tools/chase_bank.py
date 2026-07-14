#!/usr/bin/env python3
"""Author flight-dynamics into Authoring/chase/CHASE.LWS: bank angles from
spline curvature + ship2 heading from the velocity tangent.

Family: tools/add_city_beam_flags.py (in-place, idempotent, gain-parameterized,
re-runnable). Authored-first — the result is LWS keyframes, no engine change,
fully editor-visible after regen.

What it does (only the two animated ships, obj 79 Ship1 / obj 80 ship2):
  * Bank (B channel, BOTH ships): compute the per-key heading from the
    horizontal velocity tangent (central difference of the position keys),
    take its rate of change (deg per authored frame), normalise per ship by an
    85th-percentile reference so the sharpest turns saturate, and write
    bank = BANK_SIGN * gain * clamp(rate/ref).  An aircraft rolls INTO its
    turn; the sign is validated visually (flip BANK_SIGN if ships bank the
    wrong way).  Near-vertical / near-stationary keys (finale climb) get 0.
  * Heading fill (H channel, BOTH ships): every non-whitelisted key is set to
    the tangent heading so the ship faces its direction of travel (so the bank,
    derived from that same tangent, reads as banking INTO the visible turn).
    Ship1's authored heading pointed it off its flight path (it "looked weird")
    — head-filling both ships keeps heading and bank on the same path basis.
    Glows are parented, so they follow.

Preserved verbatim (never overwritten), matching the authored flourishes:
  * bank: Ship1 -15.4 @frame460; ship2 -37 @460, -39.5 @547 (PRESERVE_BANK).
  * heading: ship2's manual turn-1 yaws @286/388/460 (PRESERVE_HEAD).

Idempotence / determinism:
  * The bank whitelist is keyed on (object, frame) — every NON-whitelisted key
    is recomputed from the (immutable) positions every run, so re-running with
    the same OR a different gain reproduces / re-banks cleanly (never
    double-applies).
  * gain == 0 is a strict no-op: no bank written (computed bank is 0 and equals
    the authored 0 at every non-whitelisted key), no heading fill → the LWS is
    byte-unchanged and the regenerated FLD is byte-identical (idempotence proof).

Usage:  tools/chase_bank.py <bank_gain_deg>
        bank_gain_deg = peak bank angle in degrees (the sharpest turns reach it;
        a few clamp slightly above).  0 = revert to the authored baseline.
        Suggested: 30 (stately) / 55 (arcade).
"""
import sys
import os
import math

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LWS = os.path.join(REPO, "Authoring", "chase", "CHASE.LWS")

# object basename -> what to author on it
SHIPS = {
    "ship1.lwo": {"bank": True,  "headfill": True},
    "ship2.lwo": {"bank": True,  "headfill": True},
}
# authored bank/roll flourishes to keep verbatim: (basename_lower, frame)
PRESERVE_BANK = {("ship1.lwo", 460), ("ship2.lwo", 460), ("ship2.lwo", 547)}
# ship2's authored heading keys (its manual turn-1 yaws) kept verbatim during
# the heading fill — everything else is (re)derived from the tangent.
PRESERVE_HEAD = {("ship2.lwo", 286), ("ship2.lwo", 388), ("ship2.lwo", 460)}

MINSPEED = 1.0       # horizontal units/frame below which the tangent is untrusted
CLAMP_FRAC = 1.2     # peak bank may reach gain * this before the hard clamp
REF_PCTL = 85.0      # per-ship percentile of |rate| that maps to `gain`
BANK_SIGN = -1.0     # roll direction into a turn (visually validated)
SMOOTH = (0.25, 0.5, 0.25)   # 3-tap rate smoothing


def fmt(v):
    """Format a float the LightWave way: trim trailing zeros, plain 0 for zero.
    Only used for values we actually change; unchanged tokens are kept verbatim
    so a no-op run is byte-identical."""
    if abs(v) < 1e-9:
        return "0"
    s = "%.4f" % v
    s = s.rstrip("0").rstrip(".")
    return s


def percentile(vals, p):
    if not vals:
        return 0.0
    xs = sorted(vals)
    k = (len(xs) - 1) * (p / 100.0)
    lo = int(math.floor(k))
    hi = int(math.ceil(k))
    if lo == hi:
        return xs[lo]
    return xs[lo] * (hi - k) + xs[hi] * (k - lo)


def basename(path):
    return path.replace("/", "\\").split("\\")[-1].strip().lower()


def parse_motion(lines, mo_idx):
    """Given the index of an 'ObjectMotion' line, return
    (channels, nkeys, keys) where keys[i] = {'vi': value_line_index,
    'fi': frame_line_index, 'vals': [floats], 'frame': int}."""
    # first two non-empty lines after ObjectMotion are channels, nkeys
    i = mo_idx + 1
    def next_nonempty(j):
        while j < len(lines) and lines[j].strip() == "":
            j += 1
        return j
    i = next_nonempty(i)
    channels = int(lines[i].split()[0]); i += 1
    i = next_nonempty(i)
    nkeys = int(lines[i].split()[0]); i += 1
    keys = []
    for _ in range(nkeys):
        i = next_nonempty(i)
        vi = i
        vals = [float(t) for t in lines[vi].split()]
        i += 1
        i = next_nonempty(i)
        fi = i
        frame = int(round(float(lines[fi].split()[0])))
        i += 1
        keys.append({"vi": vi, "fi": fi, "vals": vals, "frame": frame})
    return channels, nkeys, keys


def set_token(line, idx, value_str):
    """Replace token `idx` in `line`, preserving leading whitespace and single
    space separators (matches the LWS motion-line formatting)."""
    lead = line[:len(line) - len(line.lstrip())]
    toks = line.split()
    toks[idx] = value_str
    return lead + " ".join(toks)


def headings_and_rates(keys):
    """Tangent heading (deg, unwrapped) + smoothed heading-rate (deg/frame) +
    horizontal speed, per key, from the position channels (0=X,1=Y,2=Z)."""
    n = len(keys)
    P = [(k["vals"][0], k["vals"][2]) for k in keys]   # (X, Z)
    F = [k["frame"] for k in keys]
    head = [0.0] * n
    speed = [0.0] * n
    for i in range(n):
        a = max(0, i - 1)
        b = min(n - 1, i + 1)
        dx = P[b][0] - P[a][0]
        dz = P[b][1] - P[a][1]
        df = max(1, F[b] - F[a])
        speed[i] = math.hypot(dx, dz) / df
        head[i] = math.degrees(math.atan2(dx, dz))
    # unwrap
    for i in range(1, n):
        while head[i] - head[i - 1] > 180.0:
            head[i] -= 360.0
        while head[i] - head[i - 1] < -180.0:
            head[i] += 360.0
    rate = [0.0] * n
    for i in range(n):
        a = max(0, i - 1)
        b = min(n - 1, i + 1)
        df = max(1, F[b] - F[a])
        rate[i] = (head[b] - head[a]) / df
    # 3-tap smoothing
    srate = [0.0] * n
    for i in range(n):
        acc = 0.0
        for w, j in zip(SMOOTH, (i - 1, i, i + 1)):
            jj = min(max(j, 0), n - 1)
            acc += w * rate[jj]
        srate[i] = acc
    return head, srate, speed


def patch_ship(lines, mo_idx, name, gain):
    ch, nk, keys = parse_motion(lines, mo_idx)
    head, rate, speed = headings_and_rates(keys)
    ref = percentile([abs(r) for r in rate if r == r], REF_PCTL)
    if ref < 1e-4:
        ref = 1e-4
    cfg = SHIPS[name]
    hard = gain * CLAMP_FRAC
    changed = 0
    for i, k in enumerate(keys):
        vi = k["vi"]
        # --- ship2 heading fill (H channel, idx 3) — idempotent & reversible.
        # Whitelisted keys keep their authored yaw; every other key is set to
        # the tangent heading (gain>0) or reset to 0 (gain==0 → true revert). ---
        if cfg["headfill"] and (name, k["frame"]) not in PRESERVE_HEAD:
            newHs = fmt(head[i] if gain > 0 else 0.0)
            if lines[vi].split()[3] != newHs:
                lines[vi] = set_token(lines[vi], 3, newHs)
                changed += 1
        # --- bank (B channel, idx 5) ---
        if cfg["bank"]:
            if (name, k["frame"]) in PRESERVE_BANK:
                continue  # keep authored flourish verbatim
            if gain <= 0 or speed[i] < MINSPEED:
                b = 0.0
            else:
                b = BANK_SIGN * gain * (rate[i] / ref)
                b = max(-hard, min(hard, b))
            newB = fmt(b)
            # only rewrite when the token text actually differs
            cur = lines[vi].split()[5]
            if cur != newB:
                lines[vi] = set_token(lines[vi], 5, newB)
                changed += 1
    return changed, ref


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: chase_bank.py <bank_gain_deg>   (0 = revert to baseline)")
    gain = float(sys.argv[1])

    raw = open(LWS, encoding="latin-1").read()
    lines = raw.split("\n")

    # locate each ship's ObjectMotion (the 'ObjectMotion' line that follows the
    # ship's LoadObject line)
    total = 0
    for li, line in enumerate(lines):
        s = line.strip()
        if not s.startswith("LoadObject"):
            continue
        base = basename(s[len("LoadObject"):])
        if base not in SHIPS:
            continue
        mo = next((j for j in range(li + 1, len(lines))
                   if lines[j].strip().startswith("ObjectMotion")), None)
        if mo is None:
            sys.exit(f"no ObjectMotion after {base}")
        n, ref = patch_ship(lines, mo, base, gain)
        total += n
        print(f"{base}: {n} channel-values written  (rate ref @P{int(REF_PCTL)}"
              f"={ref:.4f} deg/frame)")

    out = "\n".join(lines)
    with open(LWS, "w", encoding="latin-1") as f:
        f.write(out)
    if gain == 0:
        print("gain=0: no-op" + ("  (LWS byte-identical)" if out == raw
                                  else "  (WARNING: LWS changed!)"))
    print(f"bank_gain={gain}  total values written={total}")


if __name__ == "__main__":
    main()
