# fast_fog SIMD — parked for an x64/AVX2 pass

These SIMD experiments were written and measured on **arm64/NEON** (an Apple
M-series Mac) where they did **not** pay off, and were reverted from
`DeferredLighting.cpp`. They are parked here because **x64 is the likelier
production target**, and the reason they failed on NEON is largely
NEON-specific — they are expected to behave differently (likely *better*) on
AVX2. **Do this work on an actual x64 machine and measure there**; do not judge
it by arm64 numbers (it regresses NEON, which is why it's not in the tree).

## Why NEON ≠ AVX2 here

| | arm64 NEON | x64 AVX2 |
|---|---|---|
| 8×i32 op (`mul`/`xor`/`shift`) | 2 instrs (`Vec8` = 2×128-bit) — **no** reduction vs scalar | 1 instr (`vpmulld`/`vpxor`/`vpsrld`) — real ~½ instr count |
| gather (PCF taps, pixel-major inscatter) | none → scalar loads into a vector | `VPGATHERDD` exists (only ~OK throughput, but viable) |
| vectorized `atan`/`tan` (VCL/simde) | works, 2×128 | native 256, faster |

The engine builds one intrinsics codebase via **simde** (`simd/` VCL +
`simde/`), so this code compiles to native AVX2 on x64, 2×128 on arm64, and
simd128/4-wide on wasm. Writing it benefits the x64 build automatically.

## Measured arm64 results (the negatives)

- **SIMD 8-corner hash (`cellHash8` below): −1 to −2 ms (regression).** 2×128
  gives no instruction reduction, and storing the 8-lane result back to scalars
  adds a memory round-trip the scalar path (registers) avoids.
- **Corner-caching (carry 4 shared corners across DDA steps): neutral (0 ms).**
  Bit-exact, but no faster — see the key finding below.
- **Pixel-major inscatter SIMD: not attempted** (the profile showed inscatter
  is not the top hot spot; blob hashing is — but see the finding).

### Key finding (why both hash opts failed on arm64)

A `sample`-based instruction profile flagged `cellHash` as the hottest code in
the fog pass. But **both reducing the hash count (caching) and widening it
(SIMD) failed to move wall-clock.** On an out-of-order core the most-*sampled*
PC is not necessarily the *critical path* — the hashes are latency-hidden behind
the DDA/trilinear dependency chain. So the blob field is bound by the dependency
chain / instruction throughput / memory, **not** hash work. On x64 the OOO width
and latencies differ, so AVX2's genuinely-fewer-instructions hash *may* help —
but verify it actually moves wall-clock, don't assume.

## Code: SIMD 8-corner hash

Drop next to the scalar `cellHash` in `DeferredLighting.cpp`. Bit-identical to
8× scalar `cellHash` except the unsigned→float drops the LSB (`>>1` then
`×2/2³²`), which keeps values ≥ 2³¹ non-negative (a signed `to_float` would
not) and is negligible for noise.

```cpp
// Lane order (dx,dy,dz): 0=(0,0,0) 1=(1,0,0) 2=(0,1,0) 3=(1,1,0)
//                        4=(0,0,1) 5=(1,0,1) 6=(0,1,1) 7=(1,1,1)
static inline Vec8f cellHash8(int cx, int cy, int cz) {
    const Vec8i dx(0,1,0,1,0,1,0,1), dy(0,0,1,1,0,0,1,1), dz(0,0,0,0,1,1,1,1);
    Vec8ui h(0x9E3779B9u);
    h ^= Vec8ui(Vec8i(cx) + dx) * 0x8DA6B343u; h = (h << 13) | (h >> 19);
    h ^= Vec8ui(Vec8i(cy) + dy) * 0xD8163841u; h = (h << 13) | (h >> 19);
    h ^= Vec8ui(Vec8i(cz) + dz) * 0xCB1AB31Fu; h = (h << 13) | (h >> 19);
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    h *= 0x297A2D39u; h ^= h >> 15;
    return to_float(Vec8i(h >> 1)) * (2.0f / 4294967296.0f);
}
```

Wiring in `blobFieldTau`, replacing the 8 scalar `h01(...)` calls:

```cpp
float cc[8];
const Vec8f corners = cellHash8(cx, cy, cz);
corners.store(cc);
const float c000 = cc[0], c100 = cc[1], c010 = cc[2], c110 = cc[3];
const float c001 = cc[4], c101 = cc[5], c011 = cc[6], c111 = cc[7];
constexpr float kGap = 0.45f;
const float cmax = horizontal_max(corners);   // replaces the 7-way std::max
```

**x64 idea to avoid the store/reload that hurt NEON:** keep `corners` as a
`Vec8f` and do the trilinear as a dot product — `val = horizontal_add(corners *
weights8)`, where `weights8` are the 8 trilinear weights for a sample point
(`wx·wy·wz` per lane, built from the dx/dy/dz lane masks with the quintic-faded
`u,v,w`). The 2-pt Gauss is then two dot products, no scalar extraction. Not
tried on arm64 (horizontal_add cost), but on AVX2 it keeps everything in YMM.

## Code: corner-caching across DDA steps (bit-exact, neutral on arm64)

Keep the 8 corners as **named register scalars** (not an array — that's what
killed the SIMD version), and on each step carry the 4 corners on the shared
face and hash only the 4 new. `sx/sy/sz` are loop-invariant so the sign branch
is perfectly predicted.

Declare before the DDA loop (initial full hash):

```cpp
float c000 = h01(cx,cy,cz),   c100 = h01(cx+1,cy,cz);
float c010 = h01(cx,cy+1,cz), c110 = h01(cx+1,cy+1,cz);
float c001 = h01(cx,cy,cz+1), c101 = h01(cx+1,cy,cz+1);
float c011 = h01(cx,cy+1,cz+1), c111 = h01(cx+1,cy+1,cz+1);
float t = tA, tau = 0.0f;

auto stepCell = [&]() {
    if (tMaxX <= tMaxY && tMaxX <= tMaxZ) {
        cx += sx; t = tMaxX; tMaxX += tDx;
        if (sx > 0) { c000=c100; c010=c110; c001=c101; c011=c111;
                      c100=h01(cx+1,cy,cz);   c110=h01(cx+1,cy+1,cz);
                      c101=h01(cx+1,cy,cz+1); c111=h01(cx+1,cy+1,cz+1); }
        else        { c100=c000; c110=c010; c101=c001; c111=c011;
                      c000=h01(cx,cy,cz);     c010=h01(cx,cy+1,cz);
                      c001=h01(cx,cy,cz+1);   c011=h01(cx,cy+1,cz+1); }
    } else if (tMaxY <= tMaxZ) {
        cy += sy; t = tMaxY; tMaxY += tDy;
        if (sy > 0) { c000=c010; c100=c110; c001=c011; c101=c111;
                      c010=h01(cx,cy+1,cz);   c110=h01(cx+1,cy+1,cz);
                      c011=h01(cx,cy+1,cz+1); c111=h01(cx+1,cy+1,cz+1); }
        else        { c010=c000; c110=c100; c011=c001; c111=c101;
                      c000=h01(cx,cy,cz);     c100=h01(cx+1,cy,cz);
                      c001=h01(cx,cy,cz+1);   c101=h01(cx+1,cy,cz+1); }
    } else {
        cz += sz; t = tMaxZ; tMaxZ += tDz;
        if (sz > 0) { c000=c001; c100=c101; c010=c011; c110=c111;
                      c001=h01(cx,cy,cz+1);   c101=h01(cx+1,cy,cz+1);
                      c011=h01(cx,cy+1,cz+1); c111=h01(cx+1,cy+1,cz+1); }
        else        { c001=c000; c101=c100; c011=c010; c111=c110;
                      c000=h01(cx,cy,cz);     c100=h01(cx+1,cy,cz);
                      c010=h01(cx,cy+1,cz);   c110=h01(cx+1,cy+1,cz); }
    }
};
```

Then the DDA loop body uses the cached `c000..c111` directly (no per-cell
re-hash), and **both** step sites (the empty-cell-skip `continue` path and the
normal end-of-loop step) call `stepCell();` instead of the inline advance.

## Pixel-major inscatter SIMD (not implemented — the bigger x64 opportunity)

The shadowed in-scatter (`fogInscatterSegment`) per-pixel cost is the analytic
brightness (atan) + early-out probes + the importance loop (tan-recurrence +
`shaping` sqrt/cone + the PCF gather). Process **8 pixels' rays** at once:

- The arithmetic (clip quadratics, `atan`/`tan`, `sqrt` shaping) vectorizes well
  on both arches; AVX2 native 256 is faster than NEON 2×128.
- The **PCF shadow gather** is the blocker on NEON (no gather) but is feasible on
  AVX2 via `VPGATHERDD` — this is the main reason it's an x64 opportunity.
- Divergent control flow (early-out, per-lane cone/range reject) → masks.
- The blob field (`blobFieldTau`) stays scalar/divergent; compute `amt`+`zA`+`zB`
  per pixel scalar, then batch the inscatter for 8.

Engine precedent: the volumetric **cone** pass already does pixel-major SIMD and
won ~21% (sample-major was a wash). See `project_volumetric_simd_no_payoff` /
`project_oct_encode_vec` in the session memory for the patterns.

## How to validate on x64

1. **Correctness, bit-level:** render `--snapshot=conetest` with and without the
   change, compare PPMs — the hash/cache changes are bit-exact (LSB aside);
   mean|Δ| should be 0 (or <0.1 with the LSB drop).
2. **Perf, interleaved A/B** (beats machine-load noise — the arm64 box drifted):
   `--snapshot=conetest@iters=60 ... ` and `git stash` between builds, 2-3 reps.
   Repro cam: `FDS_CONETEST_CAM="0.000,263.647,-332.871,0.000,-0.006,1.000"`.
3. Watch `uptime` load average; throw out numbers taken while busy.
