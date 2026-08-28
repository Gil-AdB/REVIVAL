# CONES — mechanism map and ranked optimization candidates

**Target:** `cones-call` = `Render_VolumetricCones` / `Render_VolumetricCones_Tile`,
`FDS/RENDER/DeferredVolumetric.cpp`. **1.288 Ginstr/f = 30.9 % of `renderFrame`** at
city t=1961 under his arm `--env_live_water --deferred --city-env-pixel`
(`docs/OPTIMIZATION_BACKLOG.md:631`; wall **9.378 ms** of `renderFrame` 40.133 ms ×2,
`docs/PERF_STATE.md:1470`). Largest never-attacked row in the campaign.

**This document is READ-ONLY ANALYSIS.** Nothing was benchmarked or timed for it. Every
number below is either (a) quoted from an existing doc with its citation, (b) read off
the **disassembly of the already-linked binary** at
`/Users/gil-ad/work/revival-fog/build/DEMO/DEMO` (built 19:48 on commit `e017d611`,
committed 19:47:48 — same source), or (c) an explicitly-labelled arithmetic prediction.

---

## 1. WHAT IT DOES

### 1.1 Call graph

| hop | file:line |
|---|---|
| `Run_City` per tick → main view render | `DEMO/CITY.CPP:3977` — `Render()` (`skipVolumetric=false`) |
| water-reflection underlay (NO cones) | `DEMO/CITY.CPP:3881` — `Render(RenderPath::Default, /*skipVolumetric=*/true)` |
| `Render` → pipeline | `FDS/RENDER/RENDER.CPP:1880` → `RenderPipeline::renderFrame` (`RENDER.CPP:452`) |
| volumetric block gate | `FDS/RENDER/RENDER.CPP:1380` — `if (!skipVolumetric)` |
| tri-state dispatch (city takes the *legacy separate-pass* arm; `useUnifiedVolumetric()` at `RENDER.CPP:122` is false — `volumetric_unified` defaults 0 and no scene sets `PreferVolumetricUnified`) | `FDS/RENDER/RENDER.CPP:1381-1385` |
| **the profiled row** | `FDS/RENDER/RENDER.CPP:1385` — `{ TailProf::ScopeTimer _tp("cones-call", 1); Render_VolumetricCones(dctx); }` |
| orchestrator | `FDS/RENDER/DeferredVolumetric.cpp:2812` — `Render_VolumetricCones` |
| ⤷ frame spot prefilter | `:2909` — `if (!lights->isSpot[i] \|\| !(allCones \|\| lights->forceCone[i])) continue;` |
| ⤷ tile grid | `:2923-2936` (`cone_fine_tiles` default 1 → **12×8 = 96 tiles**, `DeferredCommon.h:76-77`) |
| ⤷ per-tile spot binning: screen rect + cone cull | `:2971-3056` (`lightSphereScreenRect`, `DeferredCommon.h:427`; `sphereOutsideCone`, `DeferredCommon.h:489`) |
| ⤷ work-stealing tile dispatch | `:3120-3140` — `dispatchIndexed(numTiles, …)`, `TailProf::Stamp("cones")` |
| **tile kernel** | `FDS/RENDER/DeferredVolumetric.cpp:776` — `Render_VolumetricCones_Tile` |

`cones-call` runs **once** per city frame (the reflection pass is `skipVolumetric=true`).
`renderFrame` runs twice.

### 1.2 What a "cone" is here

A **screen-space additive ray-march of a spotlight's in-scatter**, not geometry. For each
pixel the camera ray `(X, Y, 1)·z` is intersected analytically with the spot's infinite
double cone (a quadratic in `z`), clipped to the range sphere, to the apex plane, to the
G-buffer surface depth and to the fog cutoff, and the in-scatter integral over that chord
is added to the framebuffer.

### 1.3 The city population (all measured/static, agent-verified)

* **46 spots, all FLD/LWS-authored** ("city headlight L/R", 23 vehicles × 2), created once
  at load (`FDS/FLD/FLD_CONV.CPP:604`), never spawned or despawned; only their **aim**
  moves per tick (`FDS/RENDER/Transform.cpp:992-1000`).
* **All three `MakeSpotLight` sites in `DEMO/CITY.CPP` are DEAD in his arm**: `:2407`
  (`city_headlights`=0), `:2452` (`city_headlights_front`=0), `:3367`
  (`city_test_spots`=0). No streetlights, no test spots.
* Per light, identical: **outer half-angle 30°, inner 15°** → `cosOuter = 0.86603`.
  Range ∈ {800 … 2400}. `VolBeamGain = 3.0` → `coneGain`. Colour `L.RGB × ISize` =
  **(306, 282, 222)**. `Omni_ForceVolCone` set (bit 2048) — that is what admits them,
  since `draw_cones` defaults 0.
* **NOT ONE CASTS A SHADOW.** `FDS/FLD/FLD_CONV.CPP:131` never sets `Omni_CastsShadow` for
  FLD lights → `shadowMapIdx == -1` for all 46 → the `sm` blocks at `:1996-2050` and
  `:2189-2233` are dead code in city. (`docs/HW_PROFILING.md:1316` census: `shadowed=0`.)
* **NOT ONE IS NARROW.** `narrowCone = cosOuter > 0.985f` (`:928`); 0.866 < 0.985 → every
  city spot takes the **`!segPath` closed-form analytic branch** (`:1955`). Census
  confirms `seg % = 0.0` (`docs/HW_PROFILING.md:1316`).
* `cone_turbulence` = 0 → `turb.on = false`. `hdr` off → LDR composite.
* `density = cone_strength × 0.001 = 0.05 × 0.001 = 5e-5` (`:2830`).

So **city executes exactly one path**: real spot, wide cone, no shadow, no turbulence,
analytic closed form, LDR composite. Everything else in the 2 559-instruction loop body is
carried but not run.

### 1.4 Per-frame / per-tile / per-pixel structure

```
Render_VolumetricCones                          ONCE per frame
  prefilter 46 spots                            :2909
  for each spot: screen rect → tile i/j span    :2971
    for each tile in span: cone-vs-tile cull    :3011-3050  → tileSpotIdx[t][…]
  dispatchIndexed over 96 tiles                 :3120
    Render_VolumetricCones_Tile                 ONCE per tile
      build ConeSpotPre[]                       :916-982    ONCE per (tile × spot)
      for py in tile rows (yStep = 1)           :984        PER ROW
        for pxBase in tile, step 8              :998        PER 8-PX BATCH
          lane setup: X, uV, pxHash, zSurf      :1007-1023  (auto-vectorized)
          noise buffer                          (hoisted here by LLVM — see §3 B6)
          for s in tileSpotCount                :1036       PER (BATCH × SPOT)  ← THE LOOP
            read ConeSpotPre[s]                 :1078-1099
            8-wide cone-interval solve          :1373-1552
            [QUADEARLYOUT — 39.9 % of pairs leave here]      :1424
            per-spot shadow-map setup (dead in city)         :1663-1714
            dz / surface-fade window, 8-wide    :1741-1755
            analytic closed form + midpoint     :1824-2178  PER ALIVE PAIR
            colour accumulate, 8-wide           :2410-2421
          drain accumulators                    :2430
          composite to VPage                    :2436-2444  PER ALIVE PIXEL
```

**Census at city t=1961** (`docs/HW_PROFILING.md:263-265`, `:1316`, `-DFDS_CONE_DIAG=1`):
46 spots → **1 187 tile-entries** over 96 tiles (12.4 spots/tile) →
**3 204 900 (batch × spot) pairs/frame** = 25.6 M (lane × spot).
**dead 39.9 %**, alive lanes/pair 4.78 (59.8 % of 8), **sphdead 7.7 %**, **quaddead 18.8 %**.
An independent `FDS_CONE_ATTR=1` sweep at 1384×768 gives tile-entries 404–1595 across
t=150…2790 (1196 at t=1961) with `real=46 bounce=0 clone=0` at every pose — the population
is invariant, only the binning moves.

### 1.5 Data touched, memory access pattern

| level | data | pattern |
|---|---|---|
| per frame | `ViewLightsSoA` (`DeferredCommon.h:101-160`) — 20-odd `float[256]` planes | 46 strided scalar reads per plane; L1-resident |
| per tile | `ConeSpotPre conePre[64]` (`:755-775`, 112 B each) built on the stack | sequential build, then random-order re-read per (batch × spot) |
| per batch | `zEnc[row + px]` 16 B, `pxHashArr/Xarr/uVarr/zMaxArr` on stack | **sequential, 1 cache line per batch**; z-buffer read is the only streaming input |
| per alive pixel | `out[i]` read-modify-write (LDR) via `VolCompositeAdd` (`:327`) | sequential 4 B RMW |
| dead in city | `sm->packSD/packDyn` shadow gathers, `mmask/mmz` mirror masks | never touched |

**The pass is not memory-bound.** It streams one 16-bit z per pixel and RMWs one dword per
lit pixel. `docs/HW_PROFILING.md:938-952` measured it at **~81 % of the machine's NEON
issue ceiling** — the constraint is the vector-ALU port and the solve's dependency chain.

---

## 2. THE INNER LOOP, QUOTED

### 2.1 The per-(tile × spot) prologue that feeds it (`:755-775`, `:916-982`)

```c++
struct ConeSpotPre {
	float Px, Py_l, Pz;             // apex, view space
	float Dx, Dy, Dz;               // axis, view space
	float cosO, cosI;
	float r2, rr;
	float DP, PP, c2;
	float inv_cosI_minus_cosO;      // THE divide
	float inv_nSamp;
	float sphereC;                  // PP - r2
	float cq;                       // fma(DP, DP, -(c2*PP))
	float c2x2;                     // c2 + c2          (exact)
	float negDPx2;                  // -(DP + DP)       (exact)
	float cqm4;                     // cq * -4          (exact)
	…
};
```

Round 7 (`d8fc4978`) collapsed 10 800 evaluations per tile to one here. **This is done and
must not be re-proposed.**

### 2.2 The 8-wide cone-interval solve — the hot arithmetic (`:1373-1552`)

```c++
__m256 mAlive = _mm256_cmp_ps(sZMax, sZero, _CMP_NLE_UQ);
const float  VPk = std::fma(Y, Py_l, Pz);          // per (row × spot)
const float  DVk = std::fma(Y, Dy,   Dz);
const __m256 sVP = fma_x8(_mm256_set1_ps(Px), sX, _mm256_set1_ps(VPk));
const __m256 sDV = fma_x8(_mm256_set1_ps(Dx), sX, _mm256_set1_ps(DVk));
const __m256 sSphD = fma_x8(sVP, sVP, _mm256_mul_ps(_mm256_set1_ps(-sphereC), sUV));
mAlive = _mm256_and_ps(mAlive, _mm256_cmp_ps(sSphD, sZero, _CMP_NLT_UQ));
const __m256 sA  = fma_x8(sDV, sDV, _mm256_mul_ps(_mm256_set1_ps(-c2), sUV));
const __m256 sB  = fma_x8(_mm256_set1_ps(P_.c2x2), sVP,
                          _mm256_mul_ps(_mm256_set1_ps(P_.negDPx2), sDV));
const __m256 sDisc = fma_x8(sB, sB, _mm256_mul_ps(_mm256_set1_ps(P_.cqm4), sA));
const __m256 sSq    = SQRTV(sDisc);                 // TRUE vsqrt, 2× fsqrt.4s
const __m256 sInv2a = RCP(_mm256_add_ps(sA, sA));   // TRUE vdiv,  2× fdiv.4s
const __m256 sR1 = _mm256_mul_ps(sInv2a, _mm256_sub_ps(NEG(sB), sSq));
const __m256 sR2 = _mm256_mul_ps(sInv2a, _mm256_sub_ps(sSq, sB));
const __m256 rMin = MINT(sR1, sR2);  const __m256 rMax = MAXT(sR1, sR2);
… a-sign / disc / forward-backward blend cascade (≈ 20 __m256 ops) …
#if FDS_CONE_QUADEARLYOUT
    if (_mm256_movemask_ps(mAlive) == 0) { … continue; }      // fires on 39.9 %
#endif
const __m256 sSphSq = SQRTV(sSphD);                 // 2nd true sqrt
const __m256 sInvUV = RCP(sUV);                     // LICM-hoisted to per-batch
zLo = MAXT(zLo, _mm256_mul_ps(sInvUV, _mm256_sub_ps(sVP, sSphSq)));
zHi = MINT(zHi, _mm256_mul_ps(sInvUV, _mm256_add_ps(sVP, sSphSq)));
const __m256 zFwd = _mm256_div_ps(_mm256_set1_ps(DP), sDV);   // 3rd true div
… apex-plane cut, 3 masked stores, movemask #2 …
const int aliveBits = _mm256_movemask_ps(mAlive);
```

**Shape, per (batch × spot) pair, read off the linked binary:**

| property | measured |
|---|---|
| width | 8 lanes, `__m256` via simde → **two independent 128-bit NEON chains per op** |
| transcendentals | **0** in the solve. `atan_approx_x8` (12 ops) once per *alive* pair in the body |
| true divides | 2 in the solve (`1/(2a)`, `DP/DV`) + 1 in dz/fade (`1/fadeW`) = **6 `fdiv.4s`** (`cone.asm` 0x…9698, 0x…9834, 0x…a884) |
| true sqrts | 2 (`sqrt(disc)`, `sqrt(sphereDisc)`) = **4 `fsqrt.4s`** (0x…968c, 0x…97d4) |
| rsqrt / rcp + Newton | `rsqrt_nr_x8` ×1 and `_mm256_rcp_ps`+`rcp_step_x8` ×1 per **alive** pair (0x…a97c, 0x…aba0) |
| gathers | **none** on city's path (shadow taps are the only scalar-per-lane gather and every city spot is unshadowed) |
| branchiness | 103 branches in the 2 559-instruction body; only 2 are on city's hot path (`QUADEARLYOUT`, `!spotAlive`) |
| stack traffic | **449 sp-relative loads + 256 sp-relative stores = 705 of 2 559 (27.6 %)** in the spot loop; whole function 926 `ldr` + 604 `str` of 4 658 (32.8 %) |
| broadcasts | 45 `dup` in the whole 4 658-instruction function, 7 inside the solve region — LLVM has already hoisted most `set1` (confirms `HW_PROFILING.md:706-709`) |
| **floor per pair** | **187 instructions** from the spot-loop head (0x1002194cc) to the `QUADEARLYOUT` `cbz` (0x1002197b0). Every pair pays this, dead or alive |

### 2.3 What is recomputed that is invariant — audited, and mostly ALREADY FIXED

| candidate invariant | verdict |
|---|---|
| `noiseBuf[8]` (`:2165-2174`) — depends only on `pxHashArr` + `noiseStrength`, both per-batch | **already hoisted by LLVM.** The `ushr.4s`/`ucvtf.4s`/`fmla.4s` sequence sits at 0x100219388, *before* the spot-loop head at 0x1002194cc. Matches `HW_PROFILING.md:569-573` |
| `1/uV` (`sInvUV`, `:1509`) — per-batch | **already hoisted.** The solve *loads* it (`ldp q29, q28, [sp, #0x260]` at 0x1002197e4) rather than dividing. Matches `HW_PROFILING.md:320-325`, `:1238-1246` (killed twice) |
| per-spot `_mm256_set1_ps` broadcasts | **already hoisted** (45 `dup` total). `HW_PROFILING.md:706-709` |
| `VPk`/`DVk` = `fma(Y, Py, Pz)` — per (row × spot), computed per (batch × spot) | **genuinely redundant, and priced at ~0.15 % of the pass** by the campaign itself (`HW_PROFILING.md:1621-1625`). Under the noise floor |
| `vVP_v` (`:1836`) recomputes `X·Px + Y·Py + Pz` that `sVP` already holds; midpoint `Wx/Wy/Wz` (`:2082-2090`) rebuild what `sVP`/`sDV` encode | **genuinely redundant**, ~5–7 `__m256` ops per *alive* pair. See candidate **C6** |
| `density × nNorm × coneGain` (`:2415-2418`) — 3 scalars folded into `__m256` one at a time | redundant by 2 ops, but the multiply ORDER is a deliberate byte pin (`:2402-2408`). ~0.4 % |

**The two `_mm256_movemask_ps(m) == 0` tests are NOT free on arm64 and were never
audited.** simde has no `movemask` instruction to lower to; the disassembly at 0x100219750
shows it costs **25 instructions** per site — `ext.16b`, two `adrp` + two constant-table
`ldr q`, `ushl.4s`, `and.16b`, `ext.16b`, `orr.8b`, then **five vector→GPR moves**
(`fmov x8,d12`, `fmov w9,s27`, three `mov.s w,v[i]`) and nine scalar `lsr`/`orr` to pack
the 8 sign bits, then `cbz`. There are exactly **two** such sequences in the kernel
(`ushl.4s` appears twice: 0x10021975c and 0x1002198ec), i.e. one per site, both on the hot
path. `HW_PROFILING.md:1128-1152`'s simde audit (B12) checked `blendv`, unordered
predicates, mask chains, `set1`, cmp-vs-zero, `andnot`, `faddp` — **`movemask` is not in
that list.**

### 2.4 The alive-pair body (`:1824-2178`), city's `!segPath` arm

`atanDiff` (stable `atan(u)−atan(v)` identity, one `atan_approx_x8`), a midpoint block
that re-derives `W = z·V − P` from scratch, `softEdge = max(0, 1−rr·d)²`, midpoint
`coneAtten` smoothstep with `rsqrt_nr_x8`, midpoint `surfaceFade`, `1/(zHi−zLo)` via
`rcp+NR`, a midpoint `(1−z·invFogZ)²` fog factor, the hoisted noise multiply, then
`vAccB/G/R = fmadd(w, col, …)`.

**Composition of the pass** (round-2 ablation ladder scaled, `HW_PROFILING.md:541-553`,
re-verified on chase `:1330-1340`): integration body **~38 %**, solve **~34.6 %**, per-lane
dz/fade ~9.4 %, colour accumulate ~10.5 %, per-spot loop + prologue ~5.8 %, per-batch floor
~1.6 %.

---

## 3. PRIOR ART — what is already REFUTED near this code

Seven rounds, `docs/HW_PROFILING.md` §9–§15 (lines 238–1633). **The campaign ends at round
7; there is no round 8.** Landed: 8-wide solve (`--vol_cone_solve_vec`), lane-vec dz/fade +
accumulate (`--vol_cone_lane_vec`), `FDS_CONE_NEONMINMAX`, round-4b algebra folds,
`FDS_CONE_NEONSTEP`, the VP/DV fold, the `!segPath` un-gate + `FDS_CONE_QUADEARLYOUT`,
`ConeSpotPre`, `--cone_fine_tiles`, the clone-spot tile cull. Net **city 4.217 → 2.081
Ginstr/f (−50.6 %)**.

**DO NOT RE-PROPOSE — measured and rejected:**

| # | Tried | Result | Cite |
|---|---|---|---|
| B1 | range-sphere early-out | **+2.0 % instr** on city | `HW_PROFILING.md:393-399` |
| B2 | raw `rcp`/`rsqrt` for the 3 divides + 2 sqrts | +1.6 % instr, no win; NEON estimates are **8-bit**, NR costs more than the divide | `:400-413` |
| B3 | relaxed FP association (`_mm256_min_ps`, `fmsub`) | −0.5 % instr, moves the pin | `:414-417` |
| B4 | **hoist `1/uV`** — tried twice | LICM already does it; disassembly unchanged | `:320-325`, `:1238-1246` |
| B5 | `--cone_fine_tiles` on **city** | 30.866 vs 30.967 ms — wash. "city's cone cost is not a tile-balance problem" | `PERF_STATE.md:2706-2721` |
| B6 | "fix" the per-lane `noiseBuf` loop | already hoisted; +0.015 G | `:569-573` |
| B7 | hoist per-spot broadcasts | already hoisted (37 `dup` in the function) | `:706-709` |
| B8 | **stage fusion** (hand `zLo/zHi/mask` over in registers) | **+2.0 % instr, every run.** "The arrays are not a buffer, they are a phi node"; `ldr q` 248→263 | `:712-809` |
| B9 | `FDS_CONE_W4` (native 128-bit) | rolled: **+7.5 % cyc, +8.5 % wall**. `__m256` on arm64 is already unroll-and-jam by 2 | `:874-930` |
| B10 | collapse the `zLo/zHi` select cascade | +0.6 % cyc, +1.2 % wall | `:1245-1254` |
| B11 | `FDS_CONE_SEG_CLOSEDFORM` (`W²`/`D·W` closed form **in the 8-segment loop**) | +0.1…+0.7 % instr — **because that loop runs on 8.1 % of chase's pairs, ~0.9 % of the pass gross** | `:1586-1617` |
| B12 | the simde 2-for-1 spelling suspects (`blendv`, predicates, `set1`, `andnot`…) | all already folded by LLVM. **`movemask` was not audited** | `:1128-1152` |
| B13 | outlining cold arms (`FDS_CONE_HOTONLY`) | deletes 7.4–10.5 % of instructions for **−2.0 … +0.5 % cycles**. Not built | `:811-847` |
| B15 | `Y·Py`/`Y·Dy` per-row hoist | ~0.15 % of the pass | `:1621-1625` |
| B16 | **per-batch rect cull** | built, **reverted** — "per-batch overhead didn't pay". *(Live finding: `--vol_rect_cull`, `FeatureFlags.def:284`, has **zero consumers** anywhere in `FDS/` or `DEMO/` — a dead flag left by that revert.)* | `DeferredVolumetric.cpp:1038-1042` |
| B17 | true 2-D quarter rate for cones | "X-subsampling wastes lanes and fights the kernel"; ~2 ms on a ~15 ms pass | `:895-901` |
| B19 | DAG fusion (cones ∥ lighting) | **"DO NOT BUILD THIS."** cone wave effPar ≈ 12–13/12 | `RENDER_DAG_SCOPING.md:1-13` |

**Adjacent refutations that transfer** (do not re-derive): finer light grid (analytically
refuted, prize 0.012 Gi/f); GGX hoists (LICM already does them); scanline carry (three
forms, all net zero — "eight extra live values in the pixel body cost about what any of
these mechanisms save"); glow `atanf` poly (`fog-glow` −10.9 % but renderFrame −0.24 %);
`slopeFn` restructure (0.10 % of frame); SoA phase 5 (0.56–0.63 % of frame); literal PCF
split (loses on both columns — "count the `bl`s"); 3-D needle cull (premise refuted by
census); cache blocking / swizzled tiling ("linear won all 15 shape-measurements");
threading overlap and tile-size sweeps (all dead even or worse).

**The stated caps — quote these back at any proposal, including mine:**

> "of 3 204 900 (batch × spot) pairs at city t=1961, 39.9 % produce zero alive lanes but
> only **7.7 % lose all eight lanes at the range sphere** — the rest die later, on the
> cone-interval and chord tests, which have no cheap conservative screen-space form.
> **That caps any sphere-based cull … at ~3 % of the pass**" — `HW_PROFILING.md:997-1004`

> "**Stop counting instructions on this pass.** The next win has to come from cycles — the
> dependency structure, the non-pipelined `fdiv`/`fsqrt`, or doing less work (fewer pixels
> × spots)" — `:839-846`

> "the kernel is running at **~81 % of the machine's NEON issue ceiling** … **The metric is
> VECTOR-ALU OP COUNT.** Not instructions, not registers." — `:938-952`

> "**Do not price a cull on this pass without its fire rate first** … the verdict flips
> sign across that range." — `:1477-1479`

> "The flagged arm is **+5.9 % instructions** against the unconditional one … **One extra
> live bool in this kernel costs more than most of the wins the campaign has landed.**"
> — `:1370-1372`

### 3.1 The consequence: geometric culling in city is capped at ~12 % of the pass

Arithmetic, from the census (39.9 % dead) and the composition (all-pairs cost = per-spot
prologue 5.8 % + solve 34.6 %; a dead pair exits at `QUADEARLYOUT` and skips the sphere
clamp, the apex-plane divide and the three stores, so it pays ≈ 70 % of the solve):

```
dead-pair share of the pass ≈ 0.399 × (5.8 + 0.70 × 34.6) = 12.0 %
```

**A PERFECT geometric cull — one that removes every pair that produces no alive lane —
is worth 12 % of the pass, 1.1 ms.** Anything screen-space and conservative gets a
fraction of that. This is the single most important number for ranking: it retires
candidates C4/C8 to the bottom half and puts the money on **fewer PIXELS** and **fewer
SPOTS**, which remove *alive* work too.

---

## 4. RANKED CANDIDATES

Cost anchors used throughout: pass = 9.378 ms / 1.288 Ginstr/f / ~0.545 Gcyc/f at city
t=1961; **1 % of the pass = 0.094 ms**. Pairs = 3.20 M, alive pairs = 1.92 M.

---

### C1 — Sub-LSB range truncation for the cone pass ★ BUILD THIS FIRST

**(a) The change.** Add `FDS_FLAG_FLOAT(cone_range_cull, "FDS_CONE_RANGE_CULL", 1.0f)`.
In the orchestrator (`DeferredVolumetric.cpp:2974`, `r = sqrt(range2[li])`) and in the
per-tile precompute (`:955`, `sphereC = PP − r2`), use `r_eff = k·r`, `r2_eff = k²·r2` —
**and nowhere else**. `rr = 1/range` (`ConeSpotPre::rr`) is left untouched, so the beam's
brightness profile, its `softEdge = max(0, 1−rr·d)²` roll-off and its `α/β/γ` are
bit-identical; only the *range-sphere clamp* and the *screen rect / tile cull* shrink.
Verified: `r2` reaches nothing else on city's analytic path — `vR2_v` (`:1792`) is consumed
only at `:2268`, inside the ray-march `else` branch city never enters.

**(b) Mechanism.** The pass is doing arithmetic whose result is *discarded by the 8-bit
composite*. `VolCompositeAdd` (`:339-347`) does `int((pix>>16)&0xFF) + int(aR)` — a
**truncation**, so any per-spot contribution below 1.0 adds exactly nothing.

**(c) Arithmetic prediction.** With city's measured constants
(`density = 5e-5`, `nNorm = 1`, `coneGain = 3`, `colR = 306`, `N_SAMPLES = 4`):

```
contribution_R = acc × density × nNorm × coneGain × colR = 0.0459 × acc
acc            = N × mean[ 1/(rr²d² + 0.05) ] × coneAtten × surfaceFade × softEdge × fog
```
For a chord whose closest approach to the apex is `d = k·range`:
```
mean ≤ 1/(k² + 0.05),   softEdge = (1−k)²
k = 0.50 → acc ≤ 4 × 3.33 × 0.25 = 3.33 → contribution ≤ 0.153 LSB
k = 0.60 → acc ≤ 4 × 2.44 × 0.16 = 1.56 → contribution ≤ 0.072 LSB
```
So **everything beyond 0.5 · range is worth at most 0.15 of one 8-bit level, per spot.**
Screen rect area scales as `k²`: at `k = 0.5` the per-spot tile footprint drops ~4×
(1 187 tile-entries → an estimated ~300–450), and pairs drop with it — this removes
*alive* pairs and their whole 57.9 %-of-the-pass body, not just dead ones, so it is **not**
bounded by §3.1's 12 % cull cap. **Predicted −35 to −60 % of the pass = 3.3–5.6 ms**
(8–14 % of `renderFrame`). Fire rate is not a risk here: the reduction is in the loop
bounds and the tile list, not in a branch.

**(d) Pixel risk.** Approximate, **but with a hard bound**: ≤ 0.153 LSB per spot at
`k = 0.5`. It only becomes visible where ≥ 7 truncated spots overlap on one pixel. **It may
well be BYTE-NULL** at the pinned poses, in which case it lands with no look review at all —
that is the first thing to check. HDR is a different regime (float composite, no
truncation): gate the default at `k = 1.0` whenever `fds::g_hdrActive`, or scale `k` by
`hdr_glow_scale`.

**(e) Falsifier.** A `k`-sweep (1.0, 0.7, 0.6, 0.5, 0.4) at the city pins with
`render_gate.sh`, comparing **md5 first, then max |Δ| and changed-pixel count**. If `k`
is byte-null down to 0.5, it is free. Second instrument: `FDS_CONE_ATTR=1` tile-entry
counts at the 6 poses before/after — the predicted `k²` scaling of tile-entries is a
direct, cheap check of the mechanism *before* any timing run.

---

### C2 — Half vertical rate for scenes with no narrow cones

**(a) The change.** `yStep` is already there (`:982`), gated on
`vol_cone_half_y || deferred_quarter || deferred_checkerboard`. Replace the standalone
flag's role for city with a **derived** condition computed once per tile: half-rate iff
**every spot in this tile's list is wide** (`cosOuter <= 0.985f`, i.e. `!segPath`) **and**
turbulence is off. City → always true (all 46 spots are 30°). Greets/chase → false wherever
their 2.6°/4.5° disco beams land, so their narrow-beam artifact cannot appear. Ship it as
`FDS_FLAG_BOOL(cone_half_y_wide, …)`, evaluated in `Render_VolumetricCones_Tile` **before**
the row loop so it adds no live value to the inner loop (per `HW_PROFILING.md:1370-1372`).

**(b) Mechanism.** Halves the row count → halves batches → **halves pairs AND alive-pair
body work AND per-batch setup**. The only work that does not halve is the composite, which
runs twice per computed row via `dupRow` (`:2442`).

**(c) Arithmetic prediction.** Composite is inside the per-batch tail, not the spot loop;
everything in §1.4's `PER ROW` → `PER (BATCH × SPOT)` chain halves. Composite ≈ the
"per-batch floor" 1.6 % and stays. **Predicted −47 % of the pass = −4.4 ms**, 11 % of
`renderFrame`. This is the single largest lever available and **it has never been priced**
(`grep half_y docs/*.md` → nothing).

**(d) Pixel risk.** LOOK CHANGE, needs his eyes. The stated veto is explicitly
narrow-beam-specific — *"thins narrow beams — the 1-2px bright core line can fall between
sampled rows"* (`FeatureFlags.def:282`) and *"the narrow disco beams (2.6°/7°) already show
thin one-row edge artifacts at half-Y"* (`:895-901`). **City has zero narrow cones**
(measured: `seg % = 0.0`, all 30°), and its beams are soft gradients at 1–4 LSB peak. The
derived gate means the veto's actual subject is never touched. Expect visible change only
at the beam's sharp *outer edge* against the surface-fade ramp.

**(e) Falsifier.** Cheapest discriminator first: one snapshot pair at each city pin, and
his fly-through. If the outer edge stairsteps, the fallback is `yStep=2` only for spots
whose *screen* footprint exceeds N rows (large, soft, near-camera beams).

---

### C3 — Headlight-pair merge (volumetric LOD on the spot list)

**(a) The change.** City's 46 spots are **23 pairs** of L/R headlights on the same parent
vehicle, with identical direction, angles, range, colour and gain, separated by `0.7 × W`
of vehicle width. In the frame prefilter (`:2909`), when a pair's two apexes project to
within `K` pixels of each other (or the pair's world separation subtends less than a
fraction of the cone's own outer angle), emit **one** spot at the midpoint with
`coneGain × 2` instead of two. Pairing is static and can be tagged once at scene load from
the LWS `ParentObject` index; the per-frame cost is one projection and one compare per
pair, 23 times.

**(b) Mechanism.** Pure "fewer (px × spot) pairs" — the *only* lever the campaign's own
carry-forward names (`PERF_STATE.md:1701`). It removes tile-entries wholesale, and with
them both dead and alive pairs, so it is not bounded by §3.1's 12 % cap. It also stacks
multiplicatively with C1 and C2.

**(c) Arithmetic prediction.** Pairs scale linearly with spot count. If `f` of the 23
vehicles are far enough for a sub-pixel merge, spots go `46 → 46 − 23f` and the pass with
them. At `f = 0.7` (the plausible fraction of a 23-vehicle city scene beyond ~15 m):
**−35 % of tile-entries → −3.3 ms.** At `f = 0.4`: −20 %, −1.9 ms. Measure `f` first with
`FDS_CONE_ATTR`-style counting of merged pairs per pose.

**(d) Pixel risk.** LOOK CHANGE where the merge fires, but bounded *by the merge criterion
itself*: the two apexes are within `K` pixels, so the merged beam's apex is displaced by
`≤ K/2` px. `coneGain × 2` over-counts where the two cones do **not** overlap — a wedge
near the apex whose angular width is the apexes' separation, i.e. sub-pixel at the trigger
distance. Choose `K = 1.0` px for a first landing; the error is then structurally invisible.

**(e) Falsifier.** Byte-diff at the pins with `K` swept 0.25 → 2.0 px. `K` small enough
should be byte-null; the interesting question is what `K` buys what `f`. A merged-pair
counter per pose (stderr, `noinline` reporter) prices `f` before any implementation.

---

### C4 — `_mm256_movemask_ps(m) == 0` → NEON any-lane reduce

**(a) The change.** Add, next to `fmax_x8`/`fmin_x8` (`:226-276`), a compile-time
`FDS_CONE_ANYLANE` helper:
```c++
static inline bool anyLane_x8(const __m256 &m) {          // NEON arm
    simde__m256_private p = simde__m256_to_private(m);
    return vmaxvq_u32(vorrq_u32(p.m128_private[0].neon_u32,
                                p.m128_private[1].neon_u32)) != 0;
}
```
and use it at the two `== 0` sites, `:1424` (`QUADEARLYOUT`) and `:1545`
(`spotAlive = aliveBits != 0`). Keep the real `movemask` under `if constexpr (g_coneDiag)`
for the popcount. **Compile-time constant, not a FeatureFlag** — the dual-arm tax
(`HW_PROFILING.md:1370-1372`) forbids a runtime bool here; precedent is
`FDS_CONE_NEONMINMAX`.

**(b) Mechanism.** simde has nothing to lower `movemask` to. The disassembly at
0x100219750 shows the sign-bit pack costs **25 instructions** — 5 vector-ALU ops, 2 constant
`ldr q` (with 2 `adrp` that are *not* hoisted), **5 vector→GPR moves** and 9 scalar
`lsr`/`orr` — versus 3 for `orr.16b` + `umaxv.4s` + `fmov`. Both sites are on city's hot
path. The vector→GPR moves are ~6–10 cycle latency each and sit directly between the solve
and a branch, so this shortens the chain as well as the op count — exactly the two things
`HW_PROFILING.md:839-846` says the next win must come from.

**(c) Arithmetic prediction.** Site 1 runs on 100 % of pairs, site 2 on the 60.1 % that
survive. **−(25 + 0.601×25) ≈ 40 instructions and ≈ 8 vector-ALU ops per pair** → 8 of the
~461 vec-ALU ops the campaign counts in the hot loop = **1.7 % on the ALU-port metric**,
plus 8 fewer vec→GPR moves on the critical path. Honest range: **1.5–4 % of the pass,
0.14–0.38 ms.** Note `HW_PROFILING.md:1011-1014` warns that "everything that reduces
instructions on the *other* pipes … will keep measuring as zero" — the 5 vector ops and the
latency are the part that can pay; the 9 scalar ops probably will not.

**(d) Pixel risk.** **BIT-EXACT by construction.** Control flow only; identical predicate
(`any lane's sign bit set` ⇔ `movemask != 0`), identical values.

**(e) Falsifier.** Disassembly diff first (`ushl.4s` count 2 → 0). Then cycles, not
instructions, at the city + chase + greets pins. If cycles do not move, B13's lesson
("not all instructions cost the same") has claimed another one — record and revert.

---

### C5 — Cone-hull screen rect instead of the range-sphere rect

**(a) The change.** In the orchestrator (`:2971-2984`), replace the single
`lightSphereScreenRect(P, r, …)` with the **union of two**: the apex as a degenerate sphere
`(P, 0)`, and the cone's base sphere `(P + r·cosθ·D, r·sinθ)`. Union the two rects (or
`full` if either straddles the near plane). Optionally carry the same rect down into
`ConeSpotPre` as clamped `x0/x1/y0/y1` and use it as **loop bounds** (which needs the
`py`/`pxBase` loops swapped to spot-outer within a row so the bound costs *nothing* per
pair — that ordering is bit-exact because each pixel still accumulates its spots in `s`
order).

**(b) Mechanism.** The convex hull of `{apex} ∪ base-sphere` **contains the cone** (each of
the cone's discs, radius `t·r·sinθ` at `P + t·r·cosθ·D`, is inside it), so any pixel whose
ray meets the cone projects inside the hull's screen AABB — the cull is exact-conservative.
For a 30° cone the hull's 3-D extent is `≈ 1.366 r × r × r` against the range sphere's
`2r × 2r × 2r`: **screen-AABB area ratio ≈ 0.25–0.34.** And it fixes the *right* bound:
`HW_PROFILING.md:997-1004` says only 7.7 % of city's pairs die at the **sphere** while
18.8 % die at the **cone quadratic** — the sphere-based cap does not apply to a cone-based
bound.

**(c) Arithmetic prediction.** Hard-capped by §3.1 at **12 % of the pass** (every pair it
removes is by construction a dead pair). Realistically it catches the *quaddead* population
that is geometrically visible in screen space — call it half to two-thirds of the 18.8 % +
7.7 % = 26.5 %: **−7 to −9 % of pairs → 2.4–3.2 % of the pass → 0.22–0.30 ms.** The
tile-list half of it (`i/j` span from a 4×-smaller rect) is nearly free and lands first.

**(d) Pixel risk.** **BIT-EXACT** — a conservative cull of pairs whose contribution is
provably zero. (Caveat to verify in review: the `a > 1e-8, disc < 0` branch at `:1580-1583`
opens the *whole* ray when the ray never leaves the cone; that case only arises when the
ray is inside the cone, whose points are inside the hull, so the bound still holds.)

**(e) Falsifier.** Add a `noinline` counter of culled tile-entries and culled pairs, run the
6 poses, and compare against the census's 39.9 % dead. If the rect culls < 10 % of pairs,
the ceiling arithmetic says stop.

---

### C6 — Closed-form `W²` / `D·W` in the **midpoint** block

**(a) The change.** In the midpoint block (`:2082-2094`), replace
`Wx = z·X − Px; Wy = z·Y − Py; Wz = z − Pz; W2 = …; DW = …` (11 `__m256` ops) with the
quadratics the solve already produced:
`W² = z(z·uV − 2·VP) + PP` (2 fma given `n2VP = −2·VP`) and `D·W = z·DV − DP` (1 fma),
using `vVP_v`/`vUv_v` already in hand and a `vDV` from `sDV`. Same code as
`FDS_CONE_SEG_CLOSEDFORM` (`:1892-1912`), applied at a different site.

**(b) Mechanism.** Removes ~5 `__m256` = 10 NEON vector-ALU ops per **alive** pair. **The
round-7 refutation (B11) explicitly does not transfer**: it was measured on the 8-segment
loop, which runs on 8.1 % of chase's pairs and is "~0.9 % of the pass gross". The midpoint
block runs on **59.8 % of city's pairs** and is on city's only path.

**(c) Arithmetic prediction.** 10 NEON ops out of ~461 vec-ALU ops per pair, on 59.8 % of
pairs: `0.598 × 10 / 461 = 1.3 %` of the ALU-port floor, `≈ 0.12 ms`. B11's own cycle
column showed −1.6…−2.8 % on the *smaller* site, so the cycle win may exceed the op count.
Honest range **1–3 %, 0.09–0.28 ms.** Below the campaign's 2 % bar at the low end — build
it only after C1–C4.

**(d) Pixel risk.** **RE-ASSOCIATION → moves bytes.** B11 measured its sibling at
chase 75/85 px, greets 2 323 px (0.112 %), all ≤ 2/255. Judge call, same family as the
already-accepted VP/DV fold (A6, countersigned "6 seems ok").

**(e) Falsifier.** Cycles at the three cone scenes' pins, plus a changed-pixel/max-|Δ|
report. B11's `+0.1…+0.7 % instructions` on the segment site is the null hypothesis: if the
midpoint site reads the same sign, the block was never the cost.

---

### C7 — Drop the Newton step on `rsqrt`/`rcp` for WIDE cones

**(a) The change.** `rsqrt_nr_x8` (`DeferredCommon.h:680`) and the `rcp_step_x8` refinement
at `:2149` cost 3 extra ops each. Specialize the analytic block on `segPath` (already a
per-spot bool, already branched on at `:2115`/`:2124`) so **wide** cones use raw
`_mm256_rsqrt_ps`/`_mm256_rcp_ps` and narrow cones keep the NR step.

**(b) Mechanism.** The source's own veto is explicitly narrow-cone-specific:
> "for NARROW cones the `1/(cosI−cosO)` gain is ~350 (1.5°/4.5°), which amplifies the raw
> 12-bit rsqrt's quantization staircase … **Wide city cones (gain 2-10) never showed it.**"
> — `FDS/RENDER/DeferredCommon.h:673-679`

City's gain is `1/(cos15° − cos30°) = 1/(0.9659 − 0.8660) = 10.0` — the top of the "never
showed it" band.

**(c) Arithmetic prediction.** 2 refined reciprocals on city's path (`vInvD` at `:1852`,
`vRcpLen` at `:2149`) + 1 `rsqrt_nr_x8` at `:2101` = **≈ 6–9 `__m256` ops = 12–18 NEON ops
per alive pair**, on 59.8 % of pairs: `0.598 × 15 / 461 ≈ 1.9 %` of the ALU floor,
**≈ 0.18 ms.** B2 refuted the *solve's* divides/sqrts; this is a different site (the
already-approximate reciprocals in the body), which `OPTIMIZATION_BACKLOG.md:5209-5215`
names as **still open**: *"Measure raw-vs-NR first; it closes the family in one build."*

**(d) Pixel risk.** LOOK CHANGE, and it is the beam-fur/fan-stripe moire family — the exact
artifact the NR step was added to kill. The claim is only that city's 10.0 gain is safe.
Marginal at ~2 %; propose it last, and only with his eyes on a city fly-through.

**(e) Falsifier.** One build closes the whole family (B2's own method). Cycles + a
changed-pixel report at city, then **narrow-beam scenes must be unchanged** — if the
`segPath` specialization leaks into greets/chase the moire returns and the build is void.

---

### C8 — Depth-sliced tile-vs-cone cull

**(a) The change.** `spot_cone_cull` (`:3011-3050`) builds ONE `tileChunkSphere` spanning
`[0.05, zMax]` per (tile × spot) and tests `sphereOutsideCone`. Because that chunk's radius
is dominated by the depth extent, the test **rejects only 14 %**
(`HW_PROFILING.md:314-319`). Split the chunk into 3–4 geometric depth slabs and reject the
tile-entry only if the cone misses **all** slabs.

**(b) Mechanism.** Same test, a much tighter bounding volume; a narrow beam crossing a tile
diagonally misses most slabs.

**(c) Arithmetic prediction.** Cost: 4 × ~25 flops × 1 187 entries ≈ 0.12 Mflop/frame —
free. Benefit is capped by §3.1 at 12 % of the pass and overlaps almost entirely with C5
(both remove dead pairs); marginal value over C5 alone is **≤ 2 %, ≤ 0.19 ms**. It also
carries B5's warning: `--cone_fine_tiles` proved city's cone cost is **not** a tile-binning
problem.

**(d) Pixel risk.** **BIT-EXACT** (conservative cull), with one trap: the `hasSky` far-bound
logic (`:3018-3030`) must be replicated per slab or the rectangular seam bug
`eb36c1fd` fixed comes back.

**(e) Falsifier.** `FDS_CONE_ATTR=1` tile-entry counts at the 6 poses, before any build:
if 4 slabs do not take 1 187 entries below ~800, drop it.

---

## 5. THE ONE TO BUILD FIRST

**C1 — sub-LSB range truncation.**

Three reasons:

1. **It is the only candidate whose prize is arithmetically derived from the scene's own
   constants rather than estimated.** `density = 5e-5`, `coneGain = 3`, `col = 306`,
   `N = 4` and an 8-bit *truncating* composite together say that beyond `0.5 × range` a
   city headlight beam is worth **≤ 0.153 of one display level**. The pass is spending an
   estimated third to a half of its time on radiance that the framebuffer discards.
2. **It might be free of a look call entirely.** If the `k`-sweep is byte-null at the pins,
   it lands like `ConeSpotPre` did — no judge call, no countersign. That is a possibility
   none of the other big levers (C2's half-Y, C3's merge) has.
3. **It is cheap to falsify before it is built.** `FDS_CONE_ATTR=1` already prints
   tile-entries per pose. Scale `r` by `k` in the *orchestrator only*, rebuild, and read the
   tile-entry count: the prediction is that it falls like `k²`. If it does not, the
   mechanism is wrong and nothing further was spent.

**Order after that:** C4 (cheap, bit-exact, unaudited simde spelling — good second landing
while C1's look question is with him), then C2 (largest single lever, but gated on his eyes),
then C3, then C5. C6/C7/C8 are 1–3 % items; hold them until the big three have landed and
the composition has been re-measured.

**Two process notes carried from the campaign, both binding on the implementation round:**

* **Re-run the `-DFDS_CONE_DIAG=1` census after every landing.** All the ratios above
  (39.9 % dead, 59.8 % alive lanes, 12.4 spots/tile) come from one pose of one build and
  every candidate here moves them.
* **`Ginstr/f` is the wrong instrument now** (`HW_PROFILING.md:839-846`). Price these in
  **cycles and wall**, min-of-N interleaved, two binaries in one worktree — and never price
  a commit with a one-binary flag A/B (`:602-628`).
