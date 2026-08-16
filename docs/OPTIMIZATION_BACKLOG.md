# OPTIMIZATION BACKLOG

Tracked list of deferred optimizations + measured-quality upgrades so they
don't get lost. Rule for this CPU software renderer (measured): it is
**gather-bound**, not FLOP-bound — but "texture reads are expensive" is NOT a
safe assumption (the env-reflection tap measured ~free). So: **measure each
change** (bench `--snapshot=<scene>@t=<T>@iters=<N>` → `mean ms/iter`,
interleave flag off/on ≥6×, take mins vs the noise floor). Everything here is
behind a default-off flag until measured + look-approved.

Status keys: TODO · IN-PROGRESS · DONE · PARKED (measured not-worth / blocked).

## 2026-08-16i — WAVE 1's dead LDR chain: the argument reaches it, the chain is 0.033 Gi/f, and the predicate needs one more term than wave 2's

**Result at greets t=5743 / 2845 / 6097 / 3409 / 5813, 1512x848, his acceptance
arm: `lighting-w1` −1.72 to −1.96 % instructions at EVERY pose (1.570 → 1.542 at
t=5743), `DeferredLighting-call` −1.45 to −1.61 %, `renderFrame` −0.54 to
−0.63 %. `lighting-w2` flat to four decimals at every pose. BYTE-NULL: five
poses under his own arm identical parent-to-child 3/3, nine 2026-08-16f pins at
their recorded values, `render_gate.sh` 4/4, the city underlay control
`56f6aff0` on both binaries.**

Commits `c0ccb40d` (instrument) · `2259c2f2` (`--deferred_shade_ldr_skip`,
default ON) · `aee1e8d7` (the stale `GreetsMirror.cpp` lift comment,
adjudicated).

### FIRST, THE SIZE — because 16h's handoff invites the wrong reading

16h left this as "**THE SAME LDR ARGUMENT APPLIES TO WAVE 1, WHICH IS
1.570 Gi/f** … the single largest untried item the round leaves behind". The
argument does apply, in full. The chain it reaches is **0.033 Gi/f**, not 1.570.

`-DFDS_W1LDR_ABLATE=1` (committed; stage 0 builds a binary `cmp`-identical to
the parent, which is what makes it quotable) removes exactly the chain:

| | `lighting-w1` Gi/f | call | `renderFrame` |
|---|--:|--:|--:|
| t=5743 stage 0 (= parent) | 1.570 | 1.855 | 4.805 |
| t=5743 stage 1 | **1.537 (−2.10 %)** | 1.822 | 4.772 (−0.69 %) |
| t=2845 stage 0 | 1.457 | 1.746 | 4.819 |
| t=2845 stage 1 | **1.425 (−2.20 %)** | 1.713 | 4.787 (−0.66 %) |

**2.1 % of `lighting-w1`, 1.8 % of the call.** Consistent with 16g's pixel
ladder, which priced "compose (after the loop)" at 0.060: 0.033 of that is the
LDR half, the rest is the HDR store, which stays. A stage 2 (tail only,
arithmetic held alive by a sink) read *above* stage 0 — its four sink `fadd`s
cost more than the tail it removed — so the 0.033 does not split and nothing
should be quoted from it. Same method note as 16g/16h; it is now three for
three.

### THE PREDICATE — `ctx.ldrDiscarded` IS NECESSARY BUT NOT SUFFICIENT

Wave 2's flag is gated on `ctx.ldrDiscarded` alone. Wave 1 needs **one more
term, `--hdr_linear`**, and the reason is not about readers at all:

* Under `--hdr_linear` the shipped radiance is `rlB = aB²·dlB + sB`, built from
  `dlB` — the **raw light accumulator** — and never from `fdB`. The whole
  fd → out → `out[i]` chain feeds the VPage alone, and `Render_TonemapToVPage`
  rewrites every byte of it (its tile body has no coverage test anywhere).
* Under plain `--hdr` (B1 gamma) the shipped radiance **is** `hB = fdB + sB`.
  The chain is LIVE. A `ldrDiscarded`-only gate would have blanked every
  gamma-HDR frame. **The fill never had this hazard** — its HDR average is
  built from the neighbours' `hdrBuf` on both arms — which is precisely why
  16h's predicate could not be copied across.

Plus a viz escape: the five debug-viz stomps write `out*` and nothing else, so
any of them forces the chain back on (`constexpr false` unless `FDS_DEV`).

**The reader audit that produced this was run blind and converged on the same
predicate.** What it found, for wave 1's specific outputs (wave 2's audit only
covered the fill's):

| reader | verdict |
|---|---|
| the wave-2 FILL's neighbour gather (`out[nidx[k]]`) | reads wave-1 VPage — but only when `!fillLdrSkip`, i.e. under the same `ctx.ldrDiscarded`. Even with `--no-deferred_fill_ldr_skip` the average it builds is itself overwritten by the tonemap |
| xpar / glass composite (`out[i]`, `sampleBgLdr`) | the xpar span starts at `RENDER.CPP:828`, **after** `hdr-activate` (:793), so `g_hdrActive` is true and the composite takes the `g_hdrBuf` arm; the LDR arm and `sampleBgLdr` are unreachable there |
| the glass-refraction LDR snapshot (`RENDER.CPP:816`, `FILLERS.CPP:2099`) | copied unconditionally, but its ONLY reader is `sampleBgLdr` — dead, not gated |
| `Hdr_ActivateNoFog` (`Hdr.cpp:91`) and its inline twin (:825) | `if (h[x*4+3] != 0.0f) continue` — lifts ONLY uncovered pixels, never one wave 1 wrote |
| SSAO (`DeferredSSAO.cpp:969`) | the VPage arm is the `!useHdr` one; `useHdr = hdr() && Hdr_WritableFor` is implied by our predicate. `--ssao_debug` writes VPage and returns before the tonemap — excluded by `ldrDiscarded`'s third gate, and that gate is doing real work: the viz only writes where `zEnc != 0`, so without it the AO frame would show black sky |
| cones/halos, froxels, rain, sprites, bloom, the LDR post stack | shut off by `g_hdrActive`/`Hdr_WritableFor`, or read only where coverage is 0 — re-verified from source, not from 16h's text |
| all 22 `write_ppm` sites, `V_Flip`, the repro/mirrortest/cloak/displace harnesses | every one is `skipVolumetric=false` → tonemap runs first |
| `EnvBake.cpp:490`, `GreetsMirror.cpp:3821`, `MirrorShatter.cpp:1468` | 8-bit readbacks that ARE the product — all have `ldrDiscarded=false` |
| **`MirrorShatter.cpp:915`** | the serial reflection goes through `renderFrame` itself with `skipVolumetric=true` and reads its surface back with **no tonemap on that path**. Only the `!skipVolumetric` term protects it — an "is this an override bake?" test would have missed it |
| `--env_ssr`'s prologue capture (`RENDER.CPP:579`) | reads the PREVIOUS frame's post-tonemap VPage |

Three things hold today by code placement rather than by predicate and must
stay that way: the skip must never reach `Render_DeferredLighting_Tile_OuterVec`
(a warning comment now sits at its head — see below), the mip-viz store must
stay above the skipped block, and an HDR-redirect of the legacy fog
(`DeferredVolumetric.cpp:701`) or the unified volumetric would turn a currently
dead wave-1 reader live.

### MEASURED — parent / OFF / ON, one worktree, one asset tree

Order-rotated interleaved, min over 11 rounds, one pose per process, 1512x848,
`--profiler=0 --deferred_prof=1 --hw_prof`, 10 iters. Load ran 6.0–8.8, so
`Gi/f` decides; `Gcyc/f` is quoted where it agrees and is visibly load-dirty at
t=6097.

| pose | row | par | off | **ON** |
|---|---|--:|--:|--:|
| **t=5743** | `lighting-w1` Gi/f | 1.570 | 1.580 (+0.64 %) | **1.542 (−1.78 %)** |
| | .. Gcyc/f | 0.448 | 0.458 | **0.442 (−1.34 %)** |
| | .. wall | 13.23 | 13.49 | **12.99 (−1.87 %)** |
| | `lighting-w2` Gi/f | 0.272 | 0.272 | **0.272 (0.00 %)** |
| | `DeferredLighting-call` Gi/f | 1.855 | 1.865 | **1.827 (−1.51 %)** |
| | `renderFrame` Gi/f / wall | 4.804 / 43.55 | 4.815 / 43.51 | **4.777 (−0.56 %) / 43.23** |
| **t=2845** | `lighting-w1` Gi/f | 1.457 | 1.467 (+0.69 %) | **1.430 (−1.85 %)** |
| | `renderFrame` Gi/f | 4.818 | 4.829 | **4.791 (−0.56 %)** |
| **t=6097** | `lighting-w1` Gi/f | 1.326 | 1.337 (+0.83 %) | **1.300 (−1.96 %)** |
| | `renderFrame` Gi/f | 4.257 | 4.269 | **4.230 (−0.63 %)** |
| **t=3409** | `lighting-w1` Gi/f | 1.571 | 1.583 (+0.76 %) | **1.544 (−1.72 %)** |
| | `renderFrame` Gi/f | 4.977 | 4.989 | **4.950 (−0.54 %)** |
| **t=5813** | `lighting-w1` Gi/f | 1.531 | 1.541 (+0.65 %) | **1.503 (−1.83 %)** |
| | `renderFrame` Gi/f | 4.665 | 4.676 | **4.637 (−0.60 %)** |

**The OFF column earns its place a third round running: the restructure costs
+0.64 to +0.83 % of w1 on its own**, so the mechanism is worth −2.5 % against
its own OFF arm and −1.8 % against the parent. `lighting-w2` unmoved to four
decimals at all five poses is the control that says this touched wave 1 and
wave 1 only.

### THE RESTRUCTURE, AND WHY IT IS A FLOAT IDENTITY

`fd` used to be computed BEFORE the metal / roughness / metal-tint / env /
SpecMul blocks and scaled in place by two of them. It now lives after them,
inside the skipped block, with the two scalings applied **in the same order**.
`EnvSpecComposeScalar` takes the texels and `metalM` **by value** and returns
through `sB/sG/sR`; nothing between the old site and the new one touches
`texB/lB/metalM/fresEC`. So it is a reordering of *statements*, not of
*operations*.

Shape matters and was measured: the `--hdr_linear` (B2) store moved **above**
the block so nothing is live across the branch — worth ~0.3 % of w1 over the
first shape, which zero-initialised `out*/h*` outside it — and the gamma (B1)
store moved **inside** it, because `hB` is exactly what that arm ships.

### WHAT THE GATE IS ACTUALLY WORTH — say it out loud

`render_gate.sh` is **4/4 and cannot discriminate this flag**. Three of its four
arms pass no `--hdr` at all and the fourth hashes an RTT surface from an
override bake, so `ldrDiscarded && hdr_linear` is false in every one. The real
coverage is the **greets t=1588 pin** (greets `setDefault`s `hdr_linear`, so the
flag IS live there) and the **five-pose acceptance arm**, both parent-identical
3/3. Quoting 4/4 as evidence for an LDR-skip is quoting a gate that could not
have failed.

### DO NOT PORT THIS TO `Render_DeferredLighting_Tile_OuterVec`

A warning comment now sits at its head. That kernel writes **no** HDR at all —
its 8-bit pack IS the HDR transport, because `Hdr_ActivateNoFog` lifts it (it
leaves `h[3]` at 0). Skipping the pack would tonemap every `PreferOuterVec`
scene (city, crash) from a cleared buffer. The saving looks identical in the
source and is not the same code.

### PRICED AT ZERO, NOT LANDED — the override-bake exclusion IS over-broad

16h excluded every override/offscreen bake from `ldrDiscarded` outright. That is
over-broad by exactly **two** passes, and the correct predicates are:

* **greets mirror RTT** (`GreetsMirror.cpp:3582-3600`): `rttHdr && !rttTrace`.
  `rttHdr` ⟹ `Hdr_BeginFramePass` ran ⟹ `Hdr_ActivateNoFog` (:3629) cannot
  early-return ⟹ `g_hdrActive` ⟹ `Render_TonemapToVPage` (:3632) fires and
  rewrites every pixel. Nothing between :3600 and :3632 reads a covered VPage
  pixel. The `!rttTrace` term is real: :3606/:3616 digest and dump
  `s_rttSurf.Data` **before** the tonemap, so the skip would silently change
  what `--mirror_rtt_trace`'s LIGHTING stage measures.
* **mirror shard bake** (`MirrorShatter.cpp:1415`): `hdrBake`, whose
  `Render_TonemapToVPageInline` (:1447) has no `g_hdrActive` guard at all.
  Academic today — `--shard_hdr` defaults 0, so `hdrWrite` is already false.

**Not landed, because it is worth ~zero on this arm and the rule is measure
each change.** greets' whole `RTT` phase is 0.025 Gi/f of a 4.80 Gi/f frame; 2 %
of the lighting inside it is below the instrument's resolution. Recorded here so
the next round doesn't re-derive it, and so nobody "extends the win" to the RTT
without noticing the trace term.

### WHAT IS LEFT

1. **The gamma-HDR (`--hdr` without `--hdr_linear`) half of the same chain.**
   `out*`, the clamps and the `out[i]` store are dead there too — only `fd` and
   `hB` are live. That is a WEAKER skip needing its own predicate and a second
   branch in the pixel body, for a smaller win on an arm nobody benches. 16h's
   durable finding (a flag-guarded predicate in this kernel costs about what
   these mechanisms save) says price the branch before writing it.
2. **`lighting-w1` is still 1.542 Gi/f = 84 % of the call**, and 16g's split
   still stands: the omni loop is 1.25 of it and **the cube tap is 0.747, 36.6 %
   of the whole call**. Everything else in wave 1 is rounding error against
   that. The next real lever on this arm is the tap or `--ssao_downscale=2`,
   not another compose.
3. **The fountain t=2500 pin is not 3/3-reliable.** One run in 43 on the
   **PARENT** binary produced `b91cb2ba…` instead of `8db68ccb…`; a 24-run gate
   on each binary then gave 24/24 `8db68ccb` on both. Pre-existing
   non-determinism, ~2 %, unrelated to this round — but a battery that calls a
   1-in-43 flip a regression will burn a session, so it is written down here.
4. **Two bugs found by code reading during the audit, NOT verified by running
   anything, handed on rather than sat on.** (a) `Render_DeferredLighting_Tile_OuterVec`
   writes no `ctx.hdrBuf`, so on a `PreferOuterVec` scene (city `CITY.CPP:2537`,
   crash `CRASH.CPP:25`) with `--hdr` **and** `--deferred_checkerboard`/`quarter`,
   the fill would average all-zero neighbour radiance and then stamp `h[3]=1`,
   which BLOCKS the `Hdr_ActivateNoFog` lift — predicted result, a black
   checkerboard. Not reachable in any shipping config found. (b)
   `VolCompositeAdd` (`DeferredVolumetric.cpp:327`) consults the GLOBAL
   `g_hdrActive` and writes the GLOBAL `g_hdrBuf` while the shard bake's cone
   pass runs on N workers against a 64² cell, and `renderReflectionCameras` is
   called from the tick before `Render()`, when `g_hdrActive` still holds the
   previous frame's `true`. Both need a run to confirm or kill.

## 2026-08-16h — `lighting-w2` (the checkerboard fill, 16 % of the call): the edge fallback is 0.12 % of it, and the win was a write nobody reads

**Result at greets t=5743 / 2845 / 6097 / 3409 / 5813, 1512x848, his acceptance
arm: `lighting-w2` −7.0 to −7.4 % instructions at EVERY pose (0.293 → 0.272 at
t=5743), `DeferredLighting-call` −1.1 to −1.3 %, `renderFrame` −0.4 to −0.5 %.
`lighting-w1` flat to three decimals. BYTE-NULL: five poses under his own arm
identical to the parent binary, nine pins 3/3 at their recorded values,
`render_gate.sh` 4/4, plus a city control for the arm the gate excludes.**

Commits `a51534dd` (instrument) · `66444117` (material hoist, no flag) ·
`f11f3e0c` (`--deferred_fill_ldr_skip`, default ON).

### THE MEASUREMENT 16g ASKED FOR, AND IT REFUTES ITS OWN HYPOTHESIS

16g's item 2 was "unmeasured: how much of w2 is the full-shade fallback —
`--deferred_checker_edge_full` is default OFF, so every material/normal/depth
edge is re-shaded by a reduced kernel, light loop and all". Two instruments now
answer it, `-DFDS_W2_CENSUS=ON` (cell census, shares the `--omni_census` runtime
gate) and `-DFDS_W2_ABLATE=n` (a nine-stage `continue` ladder with a cumulative
sink). Both committed, both byte-null at stage 0.

```
greets t=5743, 1512x848, his arm
cells 641 088   AVERAGED 635 581 (99.14 %)   FULL-SHADE FALLBACK 768 (0.12 %)
env-reflective drop 4 472 (0.70 %)   z=0 0.04 %   sentinel 0.00 %   border 0.00 %
haveOwn 99.26 %   compatible neighbours 1.980 / averaged   sharp 1.980 / averaged
```

**768 cells.** The fallback is not a cost centre and no attack on the edge
classification is justified on cost grounds. The corollary matters too: with
1.980 of 2 possible neighbours compatible and nsharp == n to three decimals, the
fill has essentially no rejects — **99 % of neighbour pairs go all the way
through**, so every per-pair instruction is paid ~2× per cell.

### THE LADDER — the two-neighbour loop is 54 % of the fill

| stage | | w2 Gi/f | delta |
|---|---|--:|--:|
| 1 | parity test only (the loop skeleton) | 0.011 | |
| 2 | + z alive test | 0.012 | +0.001 |
| 3 | + mirrorId, mat32, sentinel, matIDc | 0.021 | +0.009 |
| 4 | + envForceFull / `checker_env_full` drop | 0.026 | +0.005 |
| 5 | **+ centre oct normal decode** | 0.052 | **+0.026** |
| 6 | + neighbour index setup | 0.060 | +0.008 |
| 7 | + centre `fetchTexel` | 0.079 | +0.019 |
| 8 | **+ neighbour compat + accumulate loop** | 0.238 | **+0.159** |
| 0 | FULL (write-out + the fallback) | 0.294 | +0.056 |

**ONE oct normal decode is 0.026 Gi/f** and the fill does three per cell (centre
+ two neighbours). Stage 9 (0.308) reads ABOVE stage 0 — the top-of-body cut
forces materialisation the shipping allocator spreads out, exactly 16g's method
note. The fallback's cost is bounded by that artefact, not measured by it; the
census is what settles it.

### WHAT SHIPPED

**The material hoist (16g's item 1), NO FLAG.** `fetchTexel` re-derived the
neighbour's matID, bounds-checked it, loaded the table entry and null-tested the
Material and its Texture — for a material `neighborCompatible`'s FIRST term had
already proved equal to the centre's, and which the centre's own fetch had
resolved. `fetchTexelNb` keeps that Texture. Bit-exact by construction.
**−1.0 % of w2 at every pose.**

**`--deferred_fill_ldr_skip` (default ON) — the mover, −6.2 % more.** The fill's
8-bit VPage average is written into a buffer `Render_TonemapToVPage` then
overwrites unconditionally (it never consults the coverage lane) — the "#1 HDR
gotcha" in `docs/GRAPHICS_PIPELINE.md`, never applied here. 16g's
`--deferred_fill_hdr_skip` had already killed the LDR *sharp* accumulators on
the neighbouring argument and left the plain average alone because it IS
written; the write is the part that does not matter. Drops the neighbour colour
load + 3 extracts + 3 adds ×1.980, and the compose + store per cell.

### THE PREDICATE IS THE COMMIT — `hdrWrite` WOULD HAVE BEEN A BUG

Every VPage reader between the lighting call and the tonemap is either shut off
by `g_hdrActive`/`Hdr_WritableFor` (SSAO `DeferredSSAO.cpp:973`, the xpar
composite `DeferredSurfaceKernel.cpp:4568`, cones/halos
`DeferredVolumetric.cpp:339`, rain `DeferredFastFog.cpp:3192`, the sprite
fillers) or reads only where the coverage lane is ZERO (`Hdr_ActivateNoFog`
`Hdr.cpp:91`, the froxel composite `DeferredFastFog.cpp:2220/2364/2592`) — and
the fill sets `h[3] = 1.0f` on every arm that also writes VPage. So the write is
dead **iff the tonemap fires**, and that is strictly stronger than `hdrWrite`:

* **`DEMO/CITY.CPP:3823`'s water-reflection underlay** renders with
  `skipVolumetric=true` into the **MAIN** VPage at the **MAIN** resolution, so
  `hdrBuf` is live — but `renderFrame`'s tonemap sits inside `if
  (!skipVolumetric)`, and `CITY.CPP:3848` reads that VPage straight back to
  displace the reflection. **Its VPage IS the product.**
* `--ssao --ssao_debug` returns before the tonemap with `hdrWrite` true.
* `g_hdrDeferTonemap` (the fountain) re-runs the chain under a different
  predicate.
* Offscreen/override bakes are excluded outright — the shard bake and the greets
  mirror RTT tonemap inline, but `GreetsMirror.cpp:3621` still carries a comment
  claiming wave-2 fill pixels reach `g_hdrBuf` only via the VPage lift, and this
  flag is not the place to adjudicate that. **Open item: that comment looks
  stale against the current fill (all three HDR arms set `h[3]=1`) — somebody
  should confirm and delete it.**

So the kernel reads `ctx.ldrDiscarded`, a promise `renderFrame` sets from the
same three gates the tonemap sits behind. **Control:** city t=1961 with
`--env_live_water --deferred --hdr --city_env_pixel --deferred_checkerboard`
(the underlay pass, checkerboard forced) is byte-identical parent-to-child,
`56f6aff07f5981244463a5a7b23c2539` both.

### REFUTED — and the refutation generalises

**A scanline carry for the repeated neighbour gather. TRIED TWICE, BOTH NET
ZERO.** The cell at px reads px−1 and px+1, the cell at px+2 reads px+1 and
px+3, so every even pixel is gathered TWICE and everything gathered from it (its
G-buffer word, its Z, its decoded normal, its texel) is a pure function of that
pixel. Carrying it one step is bit-exact and should have removed one oct decode
(0.026) and one texel fetch per cell. Measured, min-of-11, three arms:

| form | w2 Gi/f, child OFF | child ON | parent |
|---|--:|--:|--:|
| `NbGather nbg[2]`, runtime `nc` | 0.361 (+23 %) | 0.312 | 0.293 |
| same, trip count 2 + `unroll(full)` | 0.351 (+20 %) | 0.309 | 0.293 |
| two one-slot index-keyed caches | 0.317 (+8.2 %) | 0.293 | 0.293 |

The first two are SROA giving up (an indexed 2-element struct array lands on the
stack); the third is clean code and still nets zero. **The lesson is the OFF
column's, and it is the durable finding of this round: in this kernel a
flag-guarded predicate or eight extra live values in the pixel body cost about
what any of these mechanisms save.** That is also why the material hoist ships
WITHOUT a flag — its first cut carried `--deferred_fill_mat_hoist` and read
par 0.293 / off 0.300 / on 0.292, i.e. the flag ate the win.

**Skipping the neighbour oct decode when its PACKED normal equals the centre's
(dot would be ≥ 0.9999 ≥ `--quarter_normal_cos`, so the branch outcome is
provably unchanged). REFUTED ON HIT RATE: 199 862 of 1 264 816 = 15.80 %.**
Worth ≈0.009 Gi/f gross before the compare costs on the 84 % that miss. Counter
kept in the census.

### WHAT IS LEFT IN w2 (now 0.272 Gi/f = 15 % of the call)

1. **The three oct normal decodes, ~0.078 Gi/f = 29 % of the fill.** The two
   NEIGHBOUR decodes are independent and `fast_rsqrt` is `vrsqrte` + one Newton
   step on a `float32x2_t` (`FDS/FILLERS/SimdHelpers.h:16`) — **lane-independent,
   so a 2-wide `oct_decode_u32_x2` is bit-exact, not a judge call.** Needs the
   two neighbours decoded together, which is the restructure that failed above;
   the difference is that this one removes registers rather than adding them.
2. **The 3-channel scalar arithmetic, est. ~0.03 Gi/f.** Per neighbour: three
   `fmax` + three `fdiv` + three compares for the texel ratio, three fp16 loads,
   three `fmla`, the texel byte unpack; per cell the HDR store. All of it is
   (B,G,R,·) and fits one NEON vector, and 4-wide reduces live registers.
   **Contraction hazard: clang already SLP-vectorises parts of this 2-wide
   (`fmul.2s` ×50, `fmla.2s` ×22 in the shipping function), so a 4-wide rewrite
   must be byte-gated, not assumed.**
3. **`hsB/hsG/hsR` are dead whenever `nsharp > 0`**, which the census says is
   ~always. Three `fadd` per neighbour ≈ 0.004 Gi/f. Needs a 2-bit "which
   neighbours passed" mask to recompute in the rare `nsharp == 0` case.
4. **THE SAME LDR ARGUMENT APPLIES TO WAVE 1, WHICH IS 1.570 Gi/f.** 16g's pixel
   ladder priced "compose (after the loop)" at 0.060 Gi/f and that includes an
   8-bit VPage combine + clamp + store per SHADED pixel, on the same
   `ctx.ldrDiscarded` reasoning and with `ctx.ldrDiscarded` already plumbed.
   **Not attempted here — out of this brief's scope — and it is the single
   largest untried item the round leaves behind.**

Renders (byte-identical to the parent, eyeballed at 1:1 for checker speckle):
`docs/img/w2fill/w2_t{2845,3409,5743,5813,6097}_after.png`.

## 2026-08-16g — 00a ROW 1 (`DeferredLighting-call`, 41 %): the tap's WRAPPER was 7 % of the call, and the floor had never been measured

**Result at greets t=5743 / 2845 / 6097 / 3409 / 5813, 1512x848, his acceptance
arm: `DeferredLighting-call` −6.8 to −7.9 % instructions and −7.2 to −9.1 %
cycles at every pose, `renderFrame` −2.7 to −3.2 % instructions. BYTE-NULL:
nine pins unmoved 3/3, `render_gate.sh` 4/4, and five poses diffed under HIS
OWN ARM (which the pin recipe does not exercise) identical parent-to-child.**

### THE DECOMPOSITION, RE-MEASURED — do not quote the 2026-08-15 split on this arm

Two new instruments, both committed and both byte-null at stage 0:
`-DFDS_PIX_ABLATE=n` (ten staged `continue`s in the PIXEL body, cumulative
sink) alongside the existing `-DFDS_OMNI_ABLATE=n`. Drivers
`scratchpad/dfl_ladder.sh`, `scratchpad/dfl_pixladder.sh`, reports
`scratchpad/dfl_ladder_report.py`, `scratchpad/dfl_pixreport.py`.

`DeferredLighting-call` = **2.040 Gi/f = 41 % of `renderFrame`'s 4.991** at
t=5743. The split, with the omni ladder's stage totals in the loop half and the
pixel ladder's in the floor half:

| item | Gi/f | % of the call |
|---|--:|--:|
| **`lighting-w1`** | **1.720** | **84.3 %** |
| — omni loop | 1.247 | 61.1 % |
|   — **cube tap** | **0.747** | **36.6 %** |
|   — specular lobe (GGX) | 0.161 | 7.9 % |
|   — `computeMapShadowAtten` | 0.079 | 3.9 % |
|   — light vector + N·L reject | 0.078 | 3.8 % |
|   — bounce portal | 0.037 | 1.8 % |
|   — diffuse accumulate | 0.035 | 1.7 % |
|   — len² + range reject | 0.034 | 1.7 % |
|   — rsqrt / dist / attenuation k | 0.032 | 1.6 % |
|   — mirrorId test | 0.026 | 1.3 % |
|   — relief-horizon in-loop test | 0.025 | 1.2 % |
|   — spot cone | ~0.000 | — |
| — **per-pixel floor** | **0.473** | **23.2 %** |
|   — view pos + SH ambient | 0.104 | 5.1 % |
|   — normal decode + TBN | 0.100 | 4.9 % |
|   — compose (after the loop) | 0.060 | 2.9 % |
|   — view dir + PBR per-pixel consts | 0.038 | 1.9 % |
|   — AO fetch | 0.036 | 1.8 % |
|   — POM horizon record | 0.034 | 1.7 % |
|   — mat32 decode + shadowMatID | 0.029 | 1.4 % |
|   — pixel-loop floor (z alive test) | 0.027 | 1.3 % |
|   — albedo fetch + tint | 0.020 | 1.0 % |
|   — sample world position | 0.017 | 0.8 % |
|   — lightmap ADDRESS resolve | 0.008 | 0.4 % |
| **`lighting-w2`** (checkerboard fill) | **0.306** | **15.0 %** |
| setup (light-list, depth-bounds, tile-cull, mirror-grid) | 0.013 | 0.6 % |

t=2845 reproduces the shape: loop 1.108, floor 0.474, w2 0.310, cube tap 0.599.

**Three things this re-measurement contradicts in the older write-ups, all
because greets' flags moved under them:**

1. **`--pbr` is ON at greets** (`GreetsApplyRunDefaults`, `GREETS.CPP:1321`).
   The specular lobe is scalar Cook-Torrance GGX, not `pow_glossClass` — so
   the 2026-08-15 note "no transcendentals to remove: `--pbr` is off at
   greets" is stale.
2. **`--shadow_dynamic` is ON at greets** (`GREETS.CPP:1281`) and
   `--shadow_lm_dynamic` is not, so `lmKernelEnabled` is **false**: the
   static-shadow lightmap is bypassed and every static-omni pixel takes a full
   cube tap. The 2026-08-15 note "`--shadow_dynamic` defaults 0 and greets
   never sets it" is stale, and its refutation (1) was reasoning about a plane
   that is not empty here.
3. **`lighting-w2` exists and is 15 % of the call.** `--deferred_checkerboard`
   is a greets setDefault; no prior round profiled the fill wave at all.

### MEASURED — parent / OFF / ON, one worktree, one asset tree

Interleaved, order-rotated, min over 11 rounds with round 1 dropped, one pose
per process, 1512x848, `--profiler=0`, `--deferred_prof=1 --hw_prof`. Load ran
6-13, so `Ginstr/f` and `Gcyc/f` are the columns that decide; `wall` is quoted
because it agrees. `off` = the CHILD binary with all three flags off — the arm
that prices the restructure by itself.

| pose | row | par | off | **ON** |
|---|---|--:|--:|--:|
| **t=5743** | `DeferredLighting-call` Gi/f | 2.039 | 2.057 (+0.9 %) | **1.877 (−7.9 %)** |
| | .. Gcyc/f | 0.558 | 0.565 (+1.3 %) | **0.516 (−7.5 %)** |
| | .. wall | 17.59 | 17.78 (+1.1 %) | **16.31 (−7.3 %)** |
| | `renderFrame` Gi/f | 4.989 | 5.007 | **4.827 (−3.2 %)** |
| | `renderFrame` wall | 45.02 | 44.34 | **43.71 (−2.9 %)** |
| **t=2845** | `DeferredLighting-call` Gi/f | 1.904 | 1.919 | **1.767 (−7.2 %)** |
| | `renderFrame` Gi/f / wall | 4.977 / 44.13 | 4.993 / 44.73 | **4.840 (−2.8 %) / 43.13 (−2.3 %)** |
| **t=6097** | `DeferredLighting-call` Gi/f | 1.759 | 1.773 | **1.638 (−6.9 %)** |
| | `renderFrame` Gi/f / wall | 4.400 / 39.48 | 4.414 / 40.05 | **4.279 (−2.8 %) / 38.90 (−1.5 %)** |
| **t=3409** | `DeferredLighting-call` Gi/f | 1.972 | 1.984 | **1.877 (−4.8 %)** |
| | `renderFrame` Gi/f / wall | 5.094 / 44.62 | 5.106 / 45.06 | **4.999 (−1.9 %) / 44.08 (−1.2 %)** |
| **t=5813** | `DeferredLighting-call` Gi/f | 1.995 | 2.012 | **1.836 (−8.0 %)** |
| | `renderFrame` Gi/f / wall | 4.846 / 43.03 | 4.863 / 43.42 | **4.687 (−3.3 %) / 42.29 (−1.7 %)** |

**The OFF column earns its place again: the restructure costs +0.2 to +0.9 %
on its own**, so the mechanism is worth −8.7 % against its own OFF arm at
t=5743 and −7.9 % against the parent. t=3409 is the weak pose (−4.8 %) — its
cube-tap share is the lowest of the five.

Per-flag, each alone against the same OFF baseline (t=5743, min-of-4):
`cube_direct` 1.577 (**it is the whole w1 win**), `fill_hdr_skip` w2 0.293
(**the whole w2 win**), `lm_addr_skip` −0.2 %.

### WHAT SHIPPED

**`--deferred_cube_direct` (default ON) — the mover.** `resolveCubeAtten` is
NOT inlined: `-S` on the shipping TU shows a real `bl` to it from the omni
loop, and `sample(1)` gives it 903 of the lighting stage's 15 653 steady-state
samples in its own frame on top of `CubeShadow_Sample`'s 5 377. It takes
twenty arguments and carries four arms — the static-lightmap sample, two
lightmap debug-recompute arms, and Depth mode's slope-bias arithmetic — every
one of which is unreachable when `lmKernelEnabled` is false and `g_shadowMode`
is PolyId, i.e. greets' shipping configuration. Calling `CubeShadow_Sample`
directly in that case passes 8 arguments instead of 20 and drops
wx/wy/wz, `lenInv`, the geometric normal, the `PixelLightmap` and the
`CubeAttenFlags` reference out of the innermost body's live set — which is the
point, in a loop whose disassembly is full of `ldr sN, [sp, #…]` folded
reloads. Predicate hoisted per TILE (the flags) × per PIXEL
(`surfaceShadowId >= 0`); anything it does not cover still takes the wrapper.
**BIT-EXACT by construction: same callee, same arguments, in the arm it
replaces.**

**`--deferred_fill_hdr_skip` (default ON).** Wave 2's `slB/slG/slR` are read
only by its `haveOwn && nsharp > 0 && !hdrWrite` arm, so under `--hdr` they
are three DIVISIONS per compatible neighbour — two neighbours per filled pixel
in checkerboard — computing a value nothing reads. Also hoists
`--quarter_tex_sharp`, which was a `FeatureFlags` read once per FILLED PIXEL.

**`--deferred_lm_addr_skip` (default ON).** Resolve the per-pixel lightmap
ADDRESS only when the lightmap kernel is live. **Measured −0.2 %: real,
reproducible, inside the noise.** Kept because it removes provably dead work,
not because it moved the number.

**The relief-horizon record behind one pointer (`HzFrame`, no flag).** See its
own commit: the ladder priced it at 0.034 Gi/f and it recovers 0.005.

### REFUTED / PRICED, NOT LANDED

* **Hoisting the GGX lobe's per-PIXEL `Gv` and `4·NdotV` out of the per-light
  loop — REFUTED BEFORE IT WAS WRITTEN.** `Gv = NdotV/(NdotV·(1−k)+k)` is a
  DIVISION recomputed per accumulated pair in the source, but `-S` on the
  shipping TU shows exactly ONE `X/(X·omk+k)` fdiv and one reload of a
  precomputed `4·NdotV` stack slot inside the depth-3 loop. **Clang's LICM
  already hoists both.** Read the source, then read the assembly.
* **A finer PolyId uniformity pyramid (`kShadowUniShift` 3 → 2, i.e. 4×4
  blocks with a 5×5 apron) — PRICED, NOT LANDED.** `DeferredLighting-call`
  1.877 → 1.858 (**−1.0 %**), `lighting-w1` 1.570 → 1.551 (−1.2 %), but
  `shadow-bake` 0.216 → 0.224 (**+3.7 %**) and the pyramid's memory goes ×4
  (≈1.9 MB → 7.4 MB across greets' 58 512² faces, both planes).
  **Net `renderFrame` −0.35 %.** Reproducible to 0.001 Gi over three
  interleaved rounds. Not worth 4× the memory on this evidence; recorded so
  nobody re-derives it. The 8×8 shift is still the right default.
* **A finer LIGHT tile grid (`DEFERRED_NUM_TILES_X/Y` beyond 12×8) — REFUTED
  ANALYTICALLY, no build.** 8.32 lights are entered per pixel to shade 2.06,
  which looks like slack, but the tile cull is conservative: a light a finer
  tile would remove is one the per-pixel `len2 > r2` test already rejects, and
  the census puts that at 8.36 % of pairs. Stages 2–4 cost 25.7 instructions
  per pair, so the whole prize is **≈0.012 Gi/f**. The 21.5 % killed by N·L
  and the 10.7 % killed by the cone are genuinely per-pixel and no tiling
  reaches them.

### WHAT IS LEFT, RANKED, ON THE NEW TIP (`DeferredLighting-call` = 1.877)

1. **The cube tap, ~0.60 Gi/f = 32 % of the call.** The interior is still
   closed — three independent measurements say any new predicate in it costs
   more than it removes — and this round only removed the FRAME around it.
   The remaining lever is still "called less". The 8×8 pyramid is in; 4×4 is
   priced above and refused; what has NOT been tried is making the tap
   8-wide over PIXELS for a fixed light (the projection prologue — 3×3 matmul,
   two frustum rejects, `1/lz`, two muls — is the majority of the tap and is
   pure SIMD), which means restructuring the loop from pixel-major to
   light-major. Large, and the per-pixel early-outs fight it.
2. **`lighting-w2`, 0.293 Gi/f = 16 % of the call, and still barely touched.**
   455 instructions per filled pixel to average two neighbours. Untried and
   bit-exact: `fetchTexel` re-resolves the neighbour's Material and Texture
   although `neighborCompatible` has already proved `mID == matIDc`, i.e. the
   neighbour's material IS the centre's — only `Mipmap[mip]` and the swizzled
   UV differ. Unmeasured: how much of the 0.293 is the **full-shade fallback**
   (`--deferred_checker_edge_full` is default OFF, so every material/normal/
   depth edge is re-shaded by wave 2's reduced kernel, light loop and all).
3. **The specular GGX lobe, 0.161 Gi/f (8.6 %).** The invariants are already
   hoisted by the compiler; what is left is D, Gl, the Schlick quintic and two
   divisions that genuinely vary per light.
4. **View pos + SH ambient, 0.104 Gi/f (5.5 %).** `shEvalIrradiance` is 36
   flops plus a 9-mul/6-add world-normal transform per pixel. Rotating the L2
   SH coefficients into VIEW space once per frame would delete the transform
   (~15 flops/px) — but SH rotation is a re-association, so it moves bytes.
   **JUDGE-CALL, not free.**
5. **Normal decode + TBN, 0.100 Gi/f (5.3 %).** Two `fast_rsqrt`, a
   Gram-Schmidt, a cross and a normalise per normal-mapped pixel. No slack
   found; it is already the cheap form.

### METHOD NOTE THAT GENERALISES — read a PIX_ABLATE delta as an upper bound

The pixel ladder priced the OFF relief-horizon record at 0.034 Gi/f; removing
it recovered 0.005. A staged `continue` prices "everything above the cut", so
for a stage whose outputs are consumed only INSIDE a later loop the cut forces
them to be materialised at one point while the shipping build lets the
register allocator spread them across the loop — sometimes rematerialising a
constant instead of keeping it live. Stages whose outputs are consumed
immediately (albedo, mat32 decode, the pixel-loop floor) do not have this
problem. The FIRST build of this ladder had a worse version of the same
disease — per-stage sinks let the compiler dead-strip earlier stages, and
stages 9/10 measured LESS than stage 8. The shipped ladder uses a cumulative
sink; if a future stage delta looks too good, check which of the two it is.

## 2026-08-16e — 00b ROW 4 (`waterWaveSlope`, ~5.4 %): 8-wide, BYTE-NULL, and three contraction traps on the way

**Result at city t=1961 / 2400 / 400, 1512x848, his acceptance arm: frame min
47.51 -> 46.22 (-2.7 %), 30.73 -> 29.59 (-3.7 %), 35.49 -> 33.66 (-5.2 %);
TOTL -1.76 / -1.14 / -1.80 ms. `water-ripple` Ginstr/f 0.219 -> 0.148 (-32.4 %),
wall 1.851 -> 1.142; `water-glints` 0.229 -> 0.203 (-11.4 %) with Gcyc/f
0.082 -> 0.054 (-34 %), wall 2.423 -> 1.590. `renderFrame` instructions FLAT —
both passes run outside it. BYTE-NULL at every pin, `render_gate.sh` 4/4.
Flag `--water_slope_vec8` (default ON). Full map in `docs/PERF_STATE.md` 00b.**

Measured on `e99f5fed` in `/Users/gil-ad/work/rev-wave8`, three arms in one
worktree (parent binary / `--no-water_slope_vec8` / default), min-of-11 over 12
interleaved rounds with r0 dropped and the arm order rotated per round.

### WHAT SHIPPED

**`waterWaveSlope8` + batched consumers.** Both screen passes now collect their
LIVE pixels — the ones that survive the ray-cast (and, for glints, the opaque-Z
reject): 81.4 % and 44.6 % of the scan respectively, per `[WCENSUS]` — and flush
in eights. `pwater::waterWaveSlope` is **132 instructions for one pixel**;
`waterWaveSlope8` is **293 for eight** (36.6/px). The headroom was never 8x:
clang had already SLP-vectorized the scalar **2-wide** (the two scroll layers in
the two lanes of `.2s` vectors), so 4x was the ceiling and 3.6x is taken. The
129-stride halo from 2026-08-16b is what makes the gather cheap — (i0, i0+1) are
contiguous and the components interleaved, so a row's corner pair is ONE 128-bit
load; the tap is 32 quad-loads + 4 transposes for eight lanes.

### THE BYTE STORY IS THE FINDING, NOT THE SPEEDUP

The round expected the fma hazard `waterWaveSlopeVaried`'s comment warns about.
It found three, none of them in the arithmetic being changed:

1. **simde's `_mm256_fnmadd_ps` is not an fma on arm64** — outside
   `SIMDE_X86_FMA_NATIVE` it is a plain `-(a*b)+c` loop body in a header, fused
   only by the build's `-ffp-contract=fast`. And `#pragma clang fp contract` is
   LEXICALLY scoped, so `FP_CONTRACT_OFF` at the call site reaches neither
   simde's fma nor its mul/add. The first cut wrapped the function in
   `FP_CONTRACT_OFF` and the bilinear still came out `fmla.4s`: **84 % of 2.6 M
   taps disagreed with the scalar, some by 6e-4 — a whole neighbouring texel**,
   because a 1-ULP `fu` crosses a texel boundary. Rewritten with clang
   `ext_vector_type` operators in our own file plus `__builtin_elementwise_fma`
   at the sites the scalar fuses: **0 mismatches in 2 612 962 taps.**
2. **LICM hoisted an invariant square out of the specular tail.** `Vy = ey - waterY`
   is loop-invariant; once the slope moved to a batch flush, clang pre-computed
   `Vy*Vy`, which ROUNDS it where the pre-batch code left it unrounded inside an
   fma. Same algebra, swapped rounding, **143 px at 1 LSB**.
3. **The caustic tap's final lerp re-contracted the other way** in the outlined
   flush. Fixed with `sampleWaterCellGlints`, a pinned copy used only by
   `RenderGlints` so the shared sampler stays exactly as chase and the env-bake
   compiled it.

Carry this: a vectorization on this tree is byte-null when it is written where
the pragma can REACH, and the pins move for optimizer decisions **around** the
edit at least as often as for the edit.

### THE `OFF` COLUMN EARNED ITS PLACE

Batching alone (flag off, eight scalar calls per flush) **costs** instructions —
`water-ripple` 0.219 -> 0.243 Ginstr/f, the batch bookkeeping — while **winning**
wall, 1.851 -> 1.577 ms, because eight independent calls overlap in the
out-of-order window where the pre-batch loop interleaved each with its own
branchy tail. The vector form takes 0.243 -> 0.148 on top. A par-vs-default
comparison alone would have credited the restructure's wall win to the vector
code.

### WHAT IS LEFT HERE

* **The `--env_live_water` tilt is still scalar** — the third consumer of the
  field. It reaches it through `fds::g_envLiveWater.slopeFn`, a function POINTER
  called per glass pixel inside the deferred kernel's per-lane loop, so batching
  it means restructuring `Render_DeferredLighting_Tile_OuterVec`'s lane walk.
  That is the 0.041 Ginstr/f `--env_live_water` adds to `lighting-w1` at t=1961;
  `lighting-w1` is unmoved by this round.
* **`water-glints`' residue is the `powf` lobe and the caustic tap**, not the
  slope: its instruction count fell only 11 % while its cycles fell 34 %.
* **The ripple pass's own ray-cast** is ~30 instructions/px over the full
  1 282 176-pixel scan (0.038 Ginstr/f). Vectorizable in principle, but it feeds
  an `int()` truncation into the dispMap index, so it is a byte-risk item with a
  small prize.

### NOTE ON THE RECORDED CITY PINS — CLOSED 2026-08-16f, THERE WAS NO DRIFT

~~`4031ceec` / `925ecd43` no longer reproduce; the drift is city-specific and
pre-existing; whoever owns the city series should re-pin.~~ **No commit moved
them.** Both reproduce byte-exactly at tip `eb5e57d9`, 3/3 each, under the recipe
they were recorded with — i.e. **without `--profiler=0`**. `bd4ffbf8` /
`4cb8d2ca` is the same binary WITH that flag: `RunCitySnapshot` never silenced
the `ProfilerEnable 1` overlay `rev.cfg` seeds, so the older pins carry 3 718 px
of HUD text (0.18 % of the frame, all of it in a 114×221 corner block, zero scene
pixels). Fixed in `DEMO/Snapshot.cpp`; **current city pins `bd4ffbf8` /
`4cb8d2ca`**, now insensitive to the flag. See the 2026-08-16f block at the top
of `docs/SESSION_STATE.md`.

## 2026-08-16c — 00b ROW 3 (`FrustumClipper::Render`, 6.2 %): the tile cull was INERT on the mirror pass

**Result: `gbuffer` at city t=1961 (his arm) `0.658 -> 0.444 Ginstr/f` (-32.5 %),
thrsum `79.3 -> 43.1` core-ms, wall `8.69 -> 6.86`; `renderFrame`
`4.383 -> 4.174 Ginstr/f` (-4.8 %), `1.196 -> 1.115 Gcyc/f`. With the arm order
ROTATED per round (13 rounds, r0 dropped) the wall reads **frame min
`50.61 -> 47.74` (-5.7 %), frame mean `57.71 -> 54.24` (-6.0 %), `renderFrame`
`40.47 -> 37.96` (-6.2 %), `gbuffer` `8.571 -> 6.023` (-29.7 %)**.
**chase t=800 gains
more than city**: `gbuffer` `0.597 -> 0.334 Ginstr/f` (-44.1 %), thrsum
`60.5 -> 26.1` core-ms (-56.9 %), `renderFrame` -6.8 % instructions / -8.8 %
cycles / -6.1 % wall. greets and fountain have no mirror transform and are
NEUTRAL (they get only the binning half: -1.0 % / -5.9 % of `gbuffer`
instructions, frame-level inside the noise). BYTE-NULL — all nine pins unmoved
2/2, `render_gate.sh` 4/4. Two commits: `c26c1c35` + `d9dfa527`.
Full map in `docs/PERF_STATE.md` 00c.**

### THE ROW ITSELF, SAMPLED

`sample <pid> 25` on the running bench, city t=1961, his arm, shares of DEMO
self samples. The parent column reproduces 00b's 6.2 % at 6.46 %:

| symbol | parent | tip |
|---|--:|--:|
| **`FrustumClipper::Render`** | **6.46 %** | **1.21 %** |
| `RenderInnerMekalele` (the per-tile walk) | 2.53 % | 0.16 % |
| `fds::FaceTileBins_Build` + dispatch | — | 0.10 % |
| **together** | **8.99 %** | **1.47 %** |
| `Render_VolumetricCones_Tile` (control) | 35 850 samples | 36 843 |
| `apply_exact<false>` (control) | 8 749 samples | 8 453 |

### THE ROW'S STATED MECHANISM WAS ONLY HALF RIGHT, AND THAT IS THE FINDING

00b row 3 read "every raster tile re-walks the whole face list: 30 tiles x 2
passes x 10 215 faces". The walk is real — it is the `RenderInnerMekalele`
2.53 % row above, and a probe that ran four EXTRA reject-only walks per tile
priced one whole walk at ~4.5 core-ms/frame — but it is a SEPARATE symbol from
the 6.2 % the row was hung on, and it is 39 % of its size.

What was actually happening is that `--tile_bbox_cull` (default ON since the
S2/B5 work) **never fired on the mirror pass at all**. `Transform_Objects`
stamps each pushed face's screen bbox into its `FListEntry`; the two
hand-written mirror transforms that build FList themselves — `Reflected_Transform`
in `DEMO/CITY.CPP` and `DEMO/CHASE.CPP`, for the water-reflection underlay —
pushed `*Ins++ = { F->SortZ.DW, F };`, a two-field aggregate that leaves the
remaining members on their default member initialisers, i.e. the COVER-ALL
sentinel. Every entry, every frame. A new throwaway `[BBOXCENSUS]` probe at
city t=1961 (1512x848, 6x5 tiles):

| pass of the frame | cover-all | avg tiles/face | (face, tile) pairs |
|---|--:|--:|--:|
| main (`Transform_Objects`) | 1.2 % | 1.45 | 29 671 |
| mirror (`Reflected_Transform`) | **100 %** | **30.00** | **621 180** |

Same geometry, **21x the clipper entries**, and city runs that pass every frame.
That is the 6.2 %.

### WHAT SHIPPED

**1. `c26c1c35` — stamp the bbox on the mirror pushes. BYTE-NULL, the whole
prize.** Body extracted verbatim into `FDS/Base/FaceBBox.h`
(`fds::FaceBBox_Stamp`) and called from both `Reflected_Transform`s.
`Transform.cpp` deliberately keeps its own inline copy — its per-mesh face loop
is the one documented there as pin-fragile under `-ffp-contract=fast` + LTO, and
the perf case is entirely on the mirror side. Byte-null follows from the S2
contract, unchanged: the box is a conservative superset of the un-clipped
triangle and the clipper only SHRINKS coverage, so a box that misses a tile
means zero output there; near-plane straddlers keep the sentinel.

**2. `d9dfa527` — `--face_tile_bin` (default ON). BYTE-NULL, the smaller half.**
`renderFrame` walks FList twice (count, then scatter) and hands each tile a
dense, sequential `Face*` run; no tile walks the list. Order inside a tile is
preserved EXACTLY (contiguous chunk split + chunk-major prefix sum), which
matters because the tile kernels are not order-independent — verified
element-by-element by a throwaway `FDS_BINVERIFY` build across city
t=1961/2400/400, greets t=2845/5743/6097 and fountain t=2500, 0 mismatches.
Skipped below 3 000 faces (greets' offscreen RTT passes). New `face-bin`
`--deferred_prof` row prices the build at **0.200 ms/frame** at city t=1961;
`--mem_census` adds two `raster/` rows, **403 KiB arena + 113 KiB scratch**
there. On its own: `gbuffer` wall -8.1 % city / -6.8 % greets, thrsum -11.7 % /
-15.3 %.

### WHAT IS LEFT HERE

* **The finer raster grid is now UNBLOCKED.** §00 row 9 refuted a 12x10 grid at
  +139 % instructions *because each clipper tile re-walked the whole face list* —
  that traversal is gone. Worth re-running on chase (`gbuffer` `effPar` 5.5 of
  12). Caveat: it moves tile boundaries, so it will NOT be byte-null.
* **`FrustumClipper::Render`'s residue is the per-(face, tile) clip itself** —
  three 140-byte `Vertex` copies, the UV/UZ stamp, and `MiplevelClipper`'s
  subdivision, redone for each tile a face straddles (now 1.45 on average
  instead of 30). Cutting it further means cutting the copy, not the traversal.

## 2026-08-16b — CITY UNDER HIS ACCEPTANCE ARM (`--env_live_water --deferred --city-env-pixel`): re-ranked, and the biggest row in it had no instrument

**Result at city t=1961, 1512x848, his exact command: frame min 54.72 -> 49.76 ms
(-4.96, -9.1 %), frame mean 59.62 -> 54.20 ms (-5.42), `renderFrame` instructions
4.409 -> 4.387 Ginstr/f. Four landings, THREE BIT-EXACT; one judge call at 4 px of
2 073 600 at |delta| = 1/255. `render_gate.sh` 4/4 PASS; chase x5, greets t1588,
fountain t2500 unmoved throughout. Full map in `docs/PERF_STATE.md` 00b.**

Measured on `44c8aeed` in `/Users/gil-ad/work/rev-cityarm`, two binaries in one
worktree against one asset tree, `SDL_VIDEODRIVER=dummy`, 12 pool workers,
min-of-10 over 11 interleaved rounds with round 0 dropped, arm order rotating.
Loads 12-39 across the session, so `Ginstr/f` is the column that decides.

### What the arm inverts (both new)

* **`--city_env_pixel` makes city FASTER, not slower.** Clean single runs: frame
  min **77.11 ms** plain `--deferred` vs **59.22 ms** with the flag. It moves the
  glass into the deferred kernel (`lighting-w1` 0.715 -> 0.966 Ginstr/f) and
  deletes a forward per-vertex reflective path that cost more. Any city figure
  quoted against plain `--deferred` is quoting a slower frame than he runs.
* **`--env_live_water` is not free per pixel.** 2026-08-13b's "free at the noise
  floor" was the FORWARD paraboloid path (per vertex). Through the deferred
  compose it is **+0.041 / +0.015 / +0.047 Ginstr/f of `lighting-w1`** at
  t=1961 / 2400 / 400 = +4.4 / +3.8 / +6.3 % of the phase, ~0.74 ms at t=1961,
  and the cost is the wave-slope call, not the mask read.

### WHAT SHIPPED

**1. `LGHT`: the fan's unit was a MESH — BYTE-NULL, -4.9 ms.**
`LGHT` runs outside `renderFrame` so it has no `--deferred_prof` row and no round
had opened it. It reads **5.90 ms p50** at t=1961 for **~9 CORE-ms** of work
(`LightMeshVerts` = 1.67 % of DEMO self time in a frame-dominated profile):
`effPar` ~1.5 of 12, a LOAD-IMBALANCE row, not a compute row. City is 56 visible
meshes of which one is the scene-sized building, and a mesh-granular fan's wall
IS the largest mesh — a work-stealing cursor cannot split a task. `LightMeshVerts`
now takes a vertex RANGE; the unit is a 1 024-vertex chunk, and the per-range
prologue (material, ambient, omni candidate list) is a loop over the scene's
omnis, ~3 k instructions against ~800 per vertex, i.e. under 1 % at that size.

| pose | `LGHT` p50 |
|---|---|
| t=1961 | **5.904 -> 0.974 ms** |
| t=2400 | 0.189 -> 0.164 |
| t=400  | 0.281 -> 0.266 |

t=2400 and t=400 barely move **and that is the check**: those poses do not have
the big mesh flagged visible, so there was no imbalance to fix. Instructions are
unchanged everywhere — this buys wall, not work, which is what a parallelism fix
must look like. Also adds `--prof_no_vertex_light` (default off, byte-null), the
ceiling instrument the row lacked; MEASUREMENT ONLY, it changes pixels, because
the forward `TheOtherBarry` filler reads `Vertex::L{R,G,B}` for transparents,
water, sprites and the forward mirror-pass glass.

**2. The water field's modulo — JUDGE CALL, 4 px at |delta| = 1/255.**
`sampleWaterNrm`'s `(i0 + 1) % WNRM` compiled to add/and/negs/and/csneg — five
instructions, because clang cannot fold a `%` on an `int` it cannot prove
non-negative — **four times per `waterWaveSlope` call** (two axes x two scroll
layers). That function is the 4th-hottest symbol of this frame (6.2 % of DEMO
self time) and is shared by the reflection ripple, the glints AND
`--env_live_water`'s tilt. A 129-stride HALO (last row/column = verbatim copy of
the first, written after the normalize so each halo texel is bit-identical to
its source) removes the modulo and makes the (i0, i0+1) pair contiguous:
**161 -> 132 instructions**.

| pass | t=1961 | t=2400 | t=400 |
|---|---|---|---|
| `water-ripple` Ginstr/f | 0.252 -> 0.221 (-12.3 %) | 0.205 -> 0.181 (-11.7 %) | 0.295 -> 0.259 (-12.2 %) |
| `water-glints` Ginstr/f | 0.248 -> 0.230 (-7.3 %) | 0.196 -> 0.182 (-7.1 %) | 0.258 -> 0.241 (-6.6 %) |

The 4 moved pixels are the `-ffp-contract=fast` + LTO re-contraction hazard
`waterWaveSlopeVaried`'s own comment warns about ("touching that reshapes its
fmadd chain -> city moves"), not an arithmetic change: same fetched bits, same
expression, different fma grouping. Smaller than the caustic-sum-plane judge call
that shipped 2026-08-15e (7 px, also all |delta| = 1). **NEW CITY PINS**:
`--deferred` `3413028bc70b99f4bc3ee9eec9de7c14` -> `4031ceec1a1090372575c4f9c39e2839`;
his arm `1986f2df63a4585c5d05082f36f8722c` -> `925ecd43f45d8f0574acc9c9a5a958a1`.

> **SUPERSEDED 2026-08-16f — these four hashes are all HUD-BEARING.** They were
> taken before `RunCitySnapshot` silenced the `rev.cfg` profiler overlay, so each
> carries the same 3 718 px of corner text. The 4-px A/B above is a *differential*
> between two binaries measured under the same recipe, so the HUD is common-mode
> and the result stands. Current, HUD-free: `--deferred`
> **`bd4ffbf87d1492175a9b6c1111fb3f5f`**, his arm
> **`4cb8d2ca68b72f8a24627f42077eef25`**.

**3. `--env_live_water` projected the same direction twice — BIT-EXACT.**
`EnvLiveWater_Weight` opens with `EnvCube_DirToFaceUV` on the UNPERTURBED
direction (correctly — a perturbed mask read would be circular) and the cube
colour fetch repeats the identical call one line later: two dominant-axis selects
and two divides for one projection, on every glass pixel, in the path his arm
runs. Project once, hand face/u/v to the mask, re-project only when the tilt
fires. `lighting-w1` -0.6 % at t=1961, -1.1 % at t=400. The weight helper is
mirrored into `DeferredSurfaceKernel.cpp` rather than refactored in `EnvBake.h`
because that header's mask machinery is under concurrent change (it tracks
`868ba5d8`'s one-bit `EnvLiveWater_MaskBit` form; keep the two in step).

**4. The froxel composite's per-pixel `logf` — BIT-EXACT.**
`Froxel_CompositeTileVec8` punts a whole 8-lane group to the scalar
`Froxel_CompositePixel` for any group containing a water-reflection lane — most
of city's lower half — and that function recomputed
`1.0f / std::log(gFrFar / gFrNear)` and `1.0f / gFrNear` PER PIXEL. The tile
version hoists them because it is a loop; the per-pixel one cannot, and clang
will not hoist across the caller's loop either (the `dword*` VPage store may
alias a float global). Cached per frame from the same expression:
`fog-composite` -3.8 / -3.2 / -3.5 %, `fastfog` -1.8 / -1.8 / -2.3 %.

### PRICED, NOT LANDED

* **`BACKLOG_PLANS.md` 2e — a salted cache for the live-water bake.**
  `--env_live_water` bypasses `city_envmap_cache` by design (the mask is a
  product of the bake's depth buffer; a colour-only hit leaves the tilt silently
  inert), so his arm cold-bakes on every launch. Whole-process wall, `iters=5`,
  3 runs each, spread < 0.01 s: cache warm **0.98 s**, cold bake **2.18 s**, his
  arm **3.06 s**. So **+2.08 s per launch**, 1.20 s of it the cold bake and
  0.88 s the live-water re-shade. 2d/2e/2f are LOOK items in his own queue —
  priced here, not landed.

### WHAT IS LEFT, RANKED, WITH ITS MECHANISM

1. **`FrustumClipper::Render`, 6.2 % — the largest untouched row.** Every raster
   tile re-walks the whole face list: 30 tiles x 2 `renderFrame` passes x 10 215
   pushed faces a frame. A face->tile binning pass is the shape. §00 row 9's
   refuted "finer grid" measured this same disease from the other side (+139 %
   instructions, because each extra tile re-walked everything).
2. **8-wide `waterWaveSlope`, ~5 % of the frame after (2).** Two bilinear taps
   into a 129^2 map per live pixel in three consumers. Every operation maps 1:1
   to a vector form; the risk is fma contraction — this round's judge call came
   from exactly that for a far smaller edit — so budget a byte battery.
   **DONE 2026-08-16e, and BYTE-NULL: no judge call was needed.** The byte
   battery was the right call — it found THREE contraction traps, none in the
   arithmetic being changed (simde's fma is not an fma on arm64 and the pragma
   cannot reach it; LICM pre-rounding an invariant square; the caustic lerp
   re-contracting). See the 2026-08-16e entry at the top.
3. **`Froxel_GlowTile`'s `atanf`**: one libm call per (coarse column x light x
   slice) for the analytic inscatter integral. 0.3 % of self time, unpriced.
4. **`--env_live_water`'s remaining 0.041 Ginstr/f** IS the wave-slope call —
   but item 2 did **NOT** cover it, and that assumption was wrong. The tilt
   reaches the field through `fds::g_envLiveWater.slopeFn`, a function POINTER
   called per glass pixel inside the deferred kernel's per-lane loop; the 8-wide
   form needs a caller that can batch, which the two screen passes can and this
   lane walk cannot without restructuring `Render_DeferredLighting_Tile_OuterVec`.
   Still open after 2026-08-16e; `lighting-w1` is unmoved by that round.
5. **`gbuffer` `effPar` 8.5-8.7 of 12** under this arm. Better than the 5.5 §00
   recorded on chase, so the "half the pool is idle" framing does not carry to
   city. Not a lever at this size.

## 2026-08-16 — HIS ACCEPTANCE ARM: SSAO was 39 % of the frame and had never been profiled

**The arm:** `--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace`,
greets, 1512x848. **Nothing in the round-1 map covers it** — no prior greets
round carried `--ssao --ssao_gtao`, and `--ssao_downscale` defaults to **1**, so
GTAO runs FULL RES (1.28 M cells) while every tuning note in
`docs/GRAPHICS_PIPELINE.md` 9 was written at quarter-res.

### The ranked map of this arm, measured (min-of-6 over 7 interleaved rounds, r0 dropped)

| item | ms @ t=5743 | Ginstr/f | share of `renderFrame` |
|---|--:|--:|--:|
| **`ssao`** | **19.4** | **2.143** | **39 %** |
| `DeferredLighting-call` (`w1` 14.4 + `w2` 2.1) | 17.6 | 2.040 | 35 % |
| `gbuffer` | 8.5 | 0.986 | 17 % |
| `shadow-bake` (outside `renderFrame`) | 2.5 | 0.207 | — |
| `RTT` | 2.1 | 0.024 | — |
| `bloom-chain` | 1.19 | 0.136 | 2 % |
| `cones` | 0.73 | 0.063 | 1 % |
| `tonemap-post` | 0.45 | 0.037 | 1 % |
| `mirror-grid` | 0.45 | — | 1 % |

`ssao`'s `Ginstr/f` is **2.14–2.16 at all five poses** — it is a fixed full-res
cost, independent of what the camera sees, which no other phase in this engine
is. Ablation at t=5743, same batch: `--no-ssao` frame 58.26 -> **39.33**;
`--no-ssao_gtao` (hemisphere producer) `ssao` 19.4 -> 12.8; dropping
`--texture_filter=2` `gbuffer` 8.50 -> 7.07 (**tf=2 costs 1.4 ms**, all of it in
the raster); `--no-bloom` 1.19 ms; dropping `--greets_displace` frame -5.1 ms
(his approved look, not a target).

### DONE — four rungs, -23 % instructions / -25 % cycles on the pass

Commits `c78d536f` (slice-trig table + 8-wide denoise, BYTE-NULL),
`f5779342` (vector tail, BYTE-NULL), `289e92c7` (sector units, judge call),
`b0905ee1` (apply `vld4_f16`/`vst4_f16`, BYTE-NULL).

Parent `f25bb992` run with the four corner flags EXPLICIT vs the tip run with
the bare umbrella, both binaries built in one worktree against one asset tree,
interleaved min-of-8, r0 dropped, load 23–37:

| pose | `ssao` Ginstr/f | `ssao` Gcyc/f | `ssao` wall ms | frame min ms |
|---|--:|--:|--:|--:|
| t=2845 | 2.150 -> 1.654 | 0.649 -> 0.488 | 19.55 -> 15.06 | 59.53 -> 59.05 |
| t=3409 | 2.141 -> 1.653 | 0.637 -> 0.487 | 22.27 -> 16.28 | 68.45 -> 65.54 |
| t=5743 | 2.143 -> 1.652 | 0.651 -> 0.492 | 23.55 -> 16.36 | 77.02 -> 71.29 |
| t=5813 | 2.149 -> 1.650 | 0.655 -> 0.483 | 20.18 -> 14.26 | 58.30 -> 52.54 |
| t=6097 | 2.155 -> 1.653 | 0.647 -> 0.487 | 19.81 -> 14.14 | 50.17 -> 44.41 |

Whole-frame: `renderFrame` **Ginstr/f -9.0 to -10.2 %, Gcyc/f -9.3 to -10.5 %**.
Image cost of all four rungs together: **343 px of 24 883 200 (0.0014 %) over 12
deterministic greets poses, every one at max |delta| = 1/255** — all of it from
the sector-units rung.

### THE BIG LEVER LEFT IS A DIAL, NOT A REFACTOR — `--ssao_downscale`

Measured on the tip, same protocol, load 15–29. This is a **LOOK change** and is
recorded here as an OPTION, not a default:

| arm | `ssao` ms t=5743 / t=6097 | `ssao` Ginstr/f | frame min t=5743 / t=6097 |
|---|--:|--:|--:|
| `--ssao_downscale=1` (default, what he runs) | 14.33 / 14.61 | 1.651 | 53.92 / 44.93 |
| **`--ssao_downscale=2`** | **4.95 / 4.97** | **0.604** | **44.81 / 35.08** |
| `--ssao_downscale=4` | 2.19 / 2.17 | 0.291 | 42.15 / 32.22 |
| `--ssao_gtao_steps=2` | 10.37 / 10.26 | 1.137 | 50.74 / 40.67 |
| `--ssao_gtao_slices=1` | 8.89 / 8.77 | 1.016 | 48.80 / 39.34 |

**`=2` is -9.4 ms of `ssao` and -9.1 ms of the FRAME** — larger than everything
the four rungs above took together. What it costs, measured against `d=1` over
four poses: 44–63 % of pixels move, but **mean |delta| on the moved pixels is
0.53–0.87 / 255** and the peaks are local to contact creases (max 20 at t=2845,
74 at t=5743; `d=4` reaches 102–109). Side-by-side crops:
`docs/img/ssaoperf/dial_t{5743,6097,2845,6001}_crop_d1_d2_d4.png`, full frames
`dial_t*_full_d1_d2_d4.jpg`. **Needs his eye before it goes near a default.**

### What is left inside SSAO, ranked, after the four rungs

* **The per-lane scalar slice setup** — `gtaoRow8` still runs the cross
  products, `fast_rsqrt` and `atan2_approx` **per lane per slice** (8 x slices
  scalar setups per vector group). Estimated ~20 % of the compute. Vectorising
  it is bit-exact only if `atan2_approx`'s branches become selects over the same
  polynomial; not attempted.
* **The two `_mm256_sqrt_ps` inside `gtaoAcos_x8`** are now the only
  non-pipelined FP ops left in the sample chain. Replacing them with
  `rsqrt`-and-multiply is NOT byte-safe (the Eberly fit's output is quantised to
  1/32 sectors; a 12-bit reciprocal there flips boundaries at ~0.3 % of samples,
  three orders of magnitude more than the sector-units rung moved).
* **The denoise at `down==1` is a 4x4 box over the FULL-RES plane** — 16 taps a
  pixel. Under `--ssao_downscale>1` it shrinks quadratically along with
  everything else, which is another reason the dial is the lever.

## 2026-08-15 — round-1 item #1, the deferred OMNI LOOP: the shadow chain is 74 % of it, not 24.5 %, and 99.5 % of the 2-D-shadow calls compute the constant 1.0f

**Result: greets t=5743 `lighting-w1` 26.00 -> 24.26 ms (-1.75 ms), `renderFrame` 41.87 -> 40.11 ms, frame min 48.33 -> 46.55 ms; instructions -9.0 % of `lighting-w1`, -6.0 % of `renderFrame`. chase t=800 `lighting-w1` -22.5 % instructions / -1.05 ms. BIT-EXACT: all eight pins unmoved 3/3, `render_gate` 4/4 PASS. No flag — there is nothing to A/B, the skipped calls returned 1.0f.**

### The instrument: a committed staged-continue ladder for the omni loop

`-DFDS_OMNI_ABLATE=n` in `DeferredSurfaceKernel.cpp` — the `FDS_CONE_ABLATE`
shape aimed at the other ~100 %-self monolith. Twelve stages, each `continue`ing
at a natural statement group of the per-light body, each sinking what it retains
into a per-tile accumulator that drains to one volatile store per tile call.
Driver `scratchpad/omni_ablate.sh`, report `scratchpad/omni_ladder.py`.
**Two independent build+measure sessions agree to <= 0.10 % on every row at both
poses.** The shipping build (stage 0, census compiled out) measures
`lighting-w1` 3.247 Gi/f against the parent's 3.247 at t=5743 and 1.916 vs 1.917
at t=3122 — the scaffolding costs nothing.

Stage 1 (the loop deleted entirely) is the kernel's non-light remainder and
independently reproduces round 1's `--prof_no_lights` figure: 0.761 Gi/f vs the
recorded 0.801. **The omni loop proper is full minus stage 1 = 2.486 Gi/f at
t=5743**, against the 2.490 the round-1 map carries. The instrument agrees with
the map it was built to subdivide, to 0.16 %.

### The split (Ginstr/f, session A; session B in docs/SESSION_STATE.md)

| stage | t=5743 dGi | % of loop | t=3122 (his) dGi | % of loop |
|---|--:|--:|--:|--:|
| mirrorId test | 0.043 | 1.7 % | 0.039 | 2.7 % |
| w, N.L dot, dot<0 reject | 0.112 | 4.5 % | 0.045 | 3.1 % |
| len2 + range reject | 0.060 | 2.4 % | 0.030 | 2.1 % |
| bounce-window portal | 0.030 | 1.2 % | 0.025 | 1.7 % |
| rsqrt / dist / attenuation k | 0.051 | 2.1 % | 0.027 | 1.9 % |
| spot cone + smoothstep | 0.006 | 0.2 % | 0.002 | 0.1 % |
| **computeMapShadowAtten** | **0.405** | **16.3 %** | **0.706** | **48.9 %** |
| **resolveCubeAtten (cube tap)** | **1.422** | **57.2 %** | **0.272** | **18.8 %** |
| relief horizon | 0.038 | 1.5 % | 0.025 | 1.7 % |
| diffuse accumulate | 0.055 | 2.2 % | 0.044 | 3.0 % |
| **specular lobe** | **0.264** | **10.6 %** | **0.229** | **15.9 %** |
| loop total | 2.486 | 100 % | 1.444 | 100 % |

**The shadow chain is 73.5 % of the omni loop at t=5743 and 67.7 % at his
pose.** The round-1 map's "the tap chain is 24.5 % of the lighting stage" is
superseded: against the same `lighting-w1` denominator the cube tap alone is
43.8 % at t=5743. The old figure came from `--prof_no_cube_tap`, which
short-circuits `resolveCubeAtten` to **1.0f = fully lit** — so every pair whose
tap would have returned 0 stops taking the `continue` and pays diffuse **and**
specular instead. `--omni_census` prices that compensation exactly: 54.6 % of
the 4.74 M taps come back fully shadowed, i.e. 2.59 M pairs a frame that the
no-tap arm shades and the real kernel does not, at ~148 instructions each =
0.385 Gi of the 1.42 Gi gap. An ablation that makes a light BRIGHTER cannot
price the thing it removed.

### The other new instrument: `--omni_census` (per PIXEL x light, `-DFDS_OMNI_CENSUS=ON`)

The tap census (43ac3456) counts per (tile x light) and structurally cannot say
how many lights a PIXEL accumulates. This does. Deterministic frame to frame.

| | t=5743 @1920x1080 | his pose t=3122 @1512x848 |
|---|--:|--:|
| shaded px | 1.044 M | 0.643 M |
| (px x light) pairs entered | 8.687 M | 7.164 M |
| lights/px entered | 8.32 | 11.14 |
| killed: mirrorId | 4.79 % | **44.28 %** |
| killed: N.L < 0 | 21.49 % | 10.32 % |
| killed: range | 8.36 % | 3.58 % |
| killed: spot cone | 10.68 % | 5.32 % |
| killed: 2-D map shadow | 0.12 % | 7.98 % |
| killed: **cube tap** | **29.77 %** | 0.02 % |
| **reach the accumulate** | **24.79 %** | **28.50 %** |
| **live lights per shaded px** | **2.06** | **3.17** |

Live-lights-per-pixel histogram, t=5743: 0 lights 6.71 %, 1 20.47 %, 2 39.50 %,
3 26.63 %, 4 6.55 %, >=5 0.15 %. His pose: 0 1.50 %, 1 5.97 %, 2 21.00 %,
3 17.31 %, **4 53.68 %**, >=5 0.54 %. **A pixel is shaded by two to four lights
and the loop walks eight to eleven to find them.** The two poses fail in
different places — his dies on mirrorId (the mirror clones), t=5743 dies on the
cube tap — so neither pose alone names the lever.

### WHAT SHIPPED: the 2-D-shadow call, skipped when it can only return 1.0f

`computeMapShadowAtten` reads exactly three index planes — the light's own 2-D
spot map, a mirror clone's SOURCE map, a mirror clone's SOURCE cube — and each
of its three bodies is guarded on that index being `>= 0`. With all three
negative it can only return its `1.0f` initialiser. It is nonetheless an
**out-of-line function with a 176-byte frame and ten callee-save `stp`/`ldp`
pairs**, invoked once per (pixel x light) that clears the cone test.

The census says **99.50 % of its 4.749 M calls a frame at t=5743 carry none of
the three** (his pose: 32.43 % of 2.615 M). That whole 0.405 Gi/f stage is a
constant, computed four and three-quarter million times a frame.

The guard is the function's own three `if`s hoisted to the call site, spelled as
one AND: `(smIdx & srcSm & srcCube) >= 0` is false iff every index is negative
(absent = -1, so bit 31 survives the AND only when all three have it). Bit-exact
by construction — it can only skip calls returning 1.0f, and 1.0f fails the
`<= 0.0f` test either way. The vec path needs no equivalent; it already tests the
same planes 8-wide (`anyShadow`) before its lane loop.

Measured (3 arms, one worktree, one asset tree, interleaved min over 7 rounds,
round 0 discarded, load 4.8-7.7):

| | greets t=5743 | greets t=3122 (his) | chase t=800 | city t=1961 | fountain t=1200 |
|---|--:|--:|--:|--:|--:|
| `lighting-w1` Gi/f | 3.246 -> **2.953** (-9.0 %) | 1.916 -> **1.872** (-2.3 %) | 0.738 -> **0.572** (-22.5 %) | 1.146 -> 1.145 | 0.335 -> 0.335 |
| `lighting-w1` ms | 26.00 -> **24.26** | 15.73 -> 15.53 | 5.07 -> **4.01** | 8.84 -> 8.73 | 2.39 -> 2.40 |
| `renderFrame` Gi/f | 4.858 -> **4.566** (-6.0 %) | 4.945 -> **4.901** (-0.9 %) | 4.087 -> **3.921** (-4.1 %) | 6.232 -> 6.230 | 1.580 -> 1.580 |
| `renderFrame` ms | 41.87 -> **40.11** | 37.80 -> **37.41** | 36.73 -> **35.35** | 53.98 -> 54.26 | 13.22 -> 13.13 |
| frame min ms | 48.33 -> **46.55** | 44.37 -> **43.82** | (snapshot) | 76.15 -> 76.59 | 15.42 -> 15.31 |

The frame-saving-equals-pass-saving check passes at t=5743: `lighting-w1` gives
back 1.75 ms, `renderFrame` 1.75 ms, the frame 1.78 ms. city and fountain are
neutral to 0.1 % on instructions (city runs the OuterVec kernel; fountain's whole
omni loop is 0.1 Gi). chase was not expected to move and moves the most in
relative terms — its lights carry cubes, not 2-D maps.

### REFUTED, WITH NUMBERS

**(1) Skip the dynamic shadow plane when it was never baked.** `--shadow_dynamic`
defaults 0 and greets never sets it, so `packDyn` is provably all-zero
(`assign(n, 0)` at rebuild; written only by the `DynamicMeshesPerFrame` bake,
reached only from the two `if (FeatureFlags::shadow_dynamic())` sites). With the
plane zero, PolyId's `closestPacked` collapses **algebraically** to
`ShadowTexId(psB[o])` and Depth's `closest` to `ShadowTexZ(psB[o])` — four loads,
four id extracts and eight compares per tap, deletable bit-exactly. Built it
(one `dynPlaneEmpty` bool through `resolveCubeAtten` into `CubeShadow_Sample`).
**It costs +12.4 % of `lighting-w1`'s instructions at t=5743 and +10.3 % at his
pose — worse than the parent** (3.318 vs 3.246 Gi/f, with lever A's -0.293
already inside it, so the bool alone is +0.365 Gi). Reverted. This is the THIRD
independent measurement of the same mechanism — the cube tap is at its
register-allocation limit and *any* new runtime predicate in its innermost body
costs more than the work it removes (cf. the tap census's +2.0 % hooks,
d9248f6d's `FDS_DEV` abort branch). **Treat "add a cheap test to the tap" as
refuted-by-default; the tap only gets cheaper by being CALLED LESS.**

**(2) Pass the pixel's world position into `computeMapShadowAtten`.** Both
mirror-clone branches recompute `wp = viewToWorld * (x,y,z) + cameraWorld` — 9
muls + 9 adds, per (pixel x light), for a per-PIXEL quantity the caller already
hoists as `sampleWorldX/Y/Z` and hands `resolveCubeAtten` one line later. 67.5 %
of the surviving calls at his pose take one of those branches, so the predicted
saving was ~0.027 Gi. **Measured 0.010 Gi — `lighting-w1` 1.872 -> 1.862 at his
pose (-0.53 %) and 2.954 -> 2.954 at t=5743, cycles flat-to-up.** The premise was
two-thirds wrong: the compiler was already CSE-ing most of it. Not worth a
signature change with a contraction risk against the pins. Reverted.

### WHAT REMAINS, AND ITS REGIME

After the change the omni loop is ~2.19 Gi/f at t=5743 and ~1.40 at his pose.

1. **The cube tap, 1.42 Gi/f = 65 % of what is left at t=5743.** 4.725 M taps at
   ~300 instructions each. `CubeShadow_Sample` is 368 static instructions with a
   144-byte frame and eight callee-save pairs; `resolveCubeAtten` wraps it in 332
   more with a 208-byte frame and eight more. Refutation (1) says the interior is
   closed to cheap edits. **The only lever left is fewer taps**, which is
   byte-moving — the parked 8x8 PolyId id-uniformity pyramid (43ac3456) is the
   specified form, and the census now gives it a denominator: 76.1 % of taps sit
   in an 8x8-uniform block.
2. **`computeMapShadowAtten` at HIS pose, ~0.66 Gi/f.** 1.77 M surviving calls,
   65 % of them the mirror-clone `srcCube` branch: world pos, mirror reflect,
   world->view round trip, then a full cube sample. The round trip exists only
   because `CubeShadow_Sample` wants view space while the reflection happens in
   world space; a world->light matrix would delete ~15 flops per call but is a
   re-association, not a hoist. **JUDGE-CALL, not free.**
   **UPDATE 2026-08-15c — the stage is now fully accounted, and the `srcCube`
   round trip is the ONLY thing left in it.** Of the 1.767 M surviving calls,
   1.703 M (96.4 %) are `srcCube` and already take the cube pyramid's 81.6 %
   skip; 0.063 M (3.6 %) are `srcSm`, a single NON-PCF *depth* compare that an
   *id* pyramid structurally cannot answer; and the light's own 2-D spot map is
   live in **0.02 %** of the stage's calls — about 520 a frame. Wiring the
   pyramid into that spot tap (done, byte-null, free — the spot pyramids were
   already being built) is worth **-0.003 Gi/f at t=1588 and 0.000 at his pose
   and t=5743**. Read the "48.9 % of the omni loop" row above as the STAGE, not
   the spot tap. Same reason `volSpotShadow` (`DeferredFastFog.cpp:344`), the
   volumetric reader of these same 2-D maps, is out of the pyramid's reach: it
   is a depth compare too.
3. **The specular lobe, 0.264 / 0.229 Gi/f (10.6 % / 15.9 %).** No
   transcendentals to remove: `--pbr` is off at greets, so the lobe is
   `pow_glossClass` = a bit-exact squaring chain for gloss in {48, 64}, ~6 fmuls.
   The cost is the half-vector, the `rsqrt`, and that it runs for every
   ACCUMULATED pair. **Nothing cheap here; it is already the cheap form.**
4. **The reject chain itself, ~0.3 Gi/f.** 8.32 lights entered per pixel to
   shade 2.06. Halving the list would save ~0.15 Gi — but the tile-sphere cull
   already took the geometric slack, and the remaining rejects are N.L (21.5 %)
   and cone (10.7 %), both genuinely per-pixel.

## 2026-08-14b — fountain's 77 % frame item, closed: the two-layer transparent kernel was scanning 198 M px a frame to shade 0.97 M, and 3 of its 4 peel passes rendered nothing

**Result: fountain t=1200 27.46 -> 15.47 ms frame min (-11.99 ms, -43.7 %), `TBR-render` 20.17 -> 8.17 ms (-59.5 %), instructions -55.0 % of `renderFrame`. Every gate byte-identical. Two flags, both default ON, both byte-null by construction.**

### What the profile called "two layers" is not two depth layers

`Render_DeferredTransparentLighting_Tile<0>` / `<1>` are `XparLayer::Front` /
`XparLayer::Back` — the front- and back-FACING transparent layers, not a depth
peel. The depth peel is a separate axis: `FOUNTAIN.CPP:1083` sets
`FntSc->XparPeelPasses = 4`, so each (clump, side) rasters and composites FOUR
times. Anyone attacking "one of the two layers" would have been attacking the
wrong structure.

### The census — `--xpar_extent_census`, fountain t=1200, 1920x1080

`RenderXparClumpInStrip` composites a *clump*: a run of consecutive
same-(mesh, frontFacing) transparent faces in one 8-row strip. It flushes on
every mesh/side change **and on every interleaved sprite**, and the fountain
spray is 33 358 sprites a frame. So:

| | value |
|---|--:|
| clump flushes / frame | **3 221** |
| composite invocations (flushes x 4 peel passes) | **12 884** |
| px a full-strip-width scan covers | **197.90 M** |
| px carrying a live transparent fragment | **0.97 M — 0.491 %** |

Each of those 12 884 invocations cleared and re-scanned all 1920x8 px of its
strip for a clump that typically spans a handful of 8-px tile columns. **The
per-strip dispatch bounds Y and only Y** — `Render_DeferredTransparentLighting_
Tile<Layer>(stripCtx, stripIdx, 0, strip_y, XRes, strip_y + strip_h)`. Nothing
in the pipeline bounded X, and nothing skipped an empty clump.

Per side and per peel pass, same pose:

| layer | calls | empty | live px | live / scanned |
|---|--:|--:|--:|--:|
| front peel0 | 1 665 | 12.3 % | 0.517 M | 60.3 % |
| front peel1 | 1 665 | **100.0 %** | 0.000 M | 0 % |
| front peel2 | 1 665 | **100.0 %** | 0.000 M | 0 % |
| front peel3 | 1 665 | **100.0 %** | 0.000 M | 0 % |
| back peel0 | 1 556 | 14.5 % | 0.454 M | 56.2 % |
| back peel1 | 1 556 | **99.9 %** | 0.000 M | 0 % |
| back peel2 | 1 556 | **100.0 %** | 0.000 M | 0 % |
| back peel3 | 1 556 | **100.0 %** | 0.000 M | 0 % |

**9 663 of 12 884 passes a frame produce nothing.** `--xpar-peel-passes` is a
per-SCENE constant, but a clump is a sprite-delimited run of faces, and those
almost never stack in depth. Exactly ONE clump in the frame has a real second
depth layer.

### Lever 1 — `--xpar_strip_extent` (default ON, byte-null by construction)

The rasterizer records the tile-column extent it touched (`meka::g_rasterXExtent`,
updated inside the `g_rasterStripClamp.tileYMax < INT32_MAX` branch that only
the TBR xpar strip path sets, so every opaque raster emits the code it emitted
before). The clump then clears and composites only those columns.

Byte-null argument, not measurement: outside the tracked range a strip's slice
is in its cleared state, so (a) the clear has nothing to clear there and (b) the
composite kernel's first per-pixel test is `mat32 == 0xFFFFFFFF -> continue`.
The extent is the union of per-triangle tile bounding boxes — a superset of what
was written, which is the direction that keeps it safe. Both TBR schedulers call
`XparStripSlices_MarkAllDirty()` before dispatch so the previous frame, the
legacy peel and resolution changes are all covered.

**Scanned px 197.90 M -> 6.66 M (3.36 %). Live fragment count IDENTICAL (0.97 M)
in both arms — the bound loses nothing.**

### Lever 2 — `--xpar_peel_early_out` (default ON, byte-null by construction)

Reverse-peel accept mask (`Mekalele.h:1455`):
`zmask = (z_candidate < z_existing) & (z_candidate > peelFloor)`, with
`z_existing` pre-cleared to `0xFFFF` and `peelFloor` = the previous pass's layer
Z. If pass N-1 committed nothing, its extent is still all `0xFFFF`, so pass N
needs `z < 0xFFFF && z > 0xFFFF` — empty. And pass N then leaves an untouched
layer in turn, so every later pass is empty too. A committed fragment can never
store `0xFFFF` (pass 0's own mask already requires `z_candidate < 0xFFFF`), so
"all 0xFFFF over the clump's columns" is an EXACT "nothing here", not a
heuristic. Passes 12 884 -> 6 014; live count unchanged.

### THREE-ARM COST — parent binary / OFF / ON, one asset tree, interleaved, min-of-6 over 7 rounds (r0 dropped)

Load 15.95 -> 10.90 across the batch, so read `Ginstr/f` where a wall figure
surprises. `parent` = `43ac3456` built in this worktree; `off` = HEAD with both
flags off; `ext` / `peel` = one lever each.

**fountain t=1200, `--deferred`, 1920x1080**

| | parent | off | ext only | peel only | **ON** |
|---|--:|--:|--:|--:|--:|
| frame min | 27.46 | 27.56 | 17.14 | 22.20 | **15.47** |
| `renderFrame` | 25.26 | 25.31 | 14.98 | 19.95 | **13.27** |
| `TBR-render` | 20.17 | 20.17 | 9.93 | 14.75 | **8.17** |
| `renderFrame` Ginstr/f | 3.514 | 3.515 | 1.840 | 2.672 | **1.583** |
| `TBR-render` Ginstr/f | 3.011 | 3.012 | 1.338 | 2.170 | **1.081** |

**The frame-saving-equals-pass-saving check closes to 0.1 %:** `renderFrame`
falls 11.992 ms, `TBR-render` falls 12.003 ms — 100.1 % of the frame saving is
in the phase that was attacked, and `FRAME_MIN` moves the same 11.99 ms.

| pose | parent frame | ON frame | delta | parent TBR | ON TBR | TBR Ginstr/f |
|---|--:|--:|--:|--:|--:|---|
| fountain t=1200 | 27.46 | **15.47** | **-11.99 (-43.7 %)** | 20.17 | 8.17 | 3.011 -> 1.081 |
| fountain t=2500 | 22.73 | **13.88** | **-8.85 (-38.9 %)** | 14.54 | 6.06 | 2.211 -> 0.766 |
| fountain t=600 | 19.96 | **12.28** | **-7.68 (-38.5 %)** | 11.99 | 4.29 | 1.821 -> 0.538 |
| city t=1961 | 76.34 | 76.30 | 0.00 | 7.00 | 6.77 | 0.846 -> 0.841 |
| greets t=3122 (his pose) | 47.04 | 46.96 | -0.08 | 6.13 | 6.06 | 1.178 -> 1.162 |
| chase t=1600 | — | — | — | 4.61 | 4.62 | 0.853 -> 0.853 |

`parent` vs `off` agree on instructions to 3-4 decimals at **every** pose
(3.514/3.515, 2.549/2.551, 2.314/2.315, 6.217/6.220, 5.016/5.016, 1.899/1.899),
so the flags carry no dark cost in their OFF arm. The other three scenes are
NULL, as expected — they do not run thousands of sprite-delimited clumps a
frame. This is a fountain fix.

### GATES — every arm byte-identical, three flag configurations

Differential (same binary, flags off / peel-only / both on), so the claim is
"this moved nothing", not "a hash matched":

| gate | OFF | peel only | **ON** |
|---|---|---|---|
| fountain t=2500 `8db68ccb59416e9a44037e9f387b7bd9` | 3/3 | 3/3 | 3/3 |
| fountain t=1200 glass `40ce5f1e9847ce50add24e68f482fda8` | 3/3 | 3/3 | 3/3 |
| fountain t=1200 plain deferred `3417643da0dfaf57f52be489e1356fce` | 2/2 | 2/2 | 2/2 |
| greets t=1588 `778fa6acd85a69cf241babefcdaf598e` | 2/2 | 2/2 | 2/2 |
| city t=1961 `3f8948232c192a979ffe7f76c4b387ab` | 2/2 | 2/2 | 2/2 |
| chase t=100/400/800/1200/1600 (all five pins) | ✓ | ✓ | ✓ |
| `render_gate.sh` | — | — | ALL FOUR PASS (`4ac809e5` / `826c09e6` / `b41894f9` / `166fa25a`) |

Both TBR schedulers are covered on purpose: the glass rows go through
`TBR_Render_GlassLayered` (barrier-per-band), the plain-deferred row through the
plain `TBR_Render` strip walk.

**Temporal / animated-spray evidence:** `--snapshot=fountain@t=100,200,...,3000`
— **30 animated frames, cat-md5 `14a2ba04f4174453f89eb5701b0028f7` in both
arms, and all 30 per-frame md5s match pairwise.** The additive spray is where a
composite-order or peel-order error would show as flicker; it is byte-identical
frame by frame across the pose sweep, so there is no look call to make here.

### WHAT IS LEFT IN `TBR-render` (8.17 ms at t=1200)

Not the composite any more. The bound leaves 6.66 M px scanned for 0.97 M live,
and the early-out removes 3/4 of the passes; what remains is the per-pass
`FrustumClipper` construct + `InitViewport` + `clipper.Render` over the clump's
faces (6 014 times a frame), the 33 358 `SpriterRT<32>` sprite blits, and the
now-narrow memsets. **Next lever, unpriced:** `peel1` still runs 2 792 times a
frame to find nothing — you cannot know a pass is empty without running it, but
you *could* know it from pass 0's own fragment count if the filler reported
"committed exactly one layer per pixel". That is a real, byte-null-shaped idea
and it is NOT measured.

## 2026-08-14 — the parked per-tile shadow early-out, MEASURED: the byte-null half ships, the byte-affecting half is refuted at the tile and re-specified at 8x8

The shadow-diet round parked *"per-tile light/shadow early-outs — a tile fully
outside a light's range or fully occluded/unoccluded can skip taps"* because it
changes shadow bytes. Both halves are now measured rather than argued, with a
new instrument (`--shadow_tap_census`, and `--shadow_tap_census_block=B` for the
granularity question). One half shipped; the other is refuted **at the tile**
and comes back with a specification.

### First, the denominator — it was wrong by 2x

The parked note carried "shadow sampling is 48.8 % of the lighting stage". The
real number, by ablation (`--prof_no_cube_tap`, one binary, interleaved min-of-4,
greets t=5743 @1920x1080):

| arm | lighting-w1 | renderFrame |
|---|--:|--:|
| shipping | 29.710 ms | 50.054 ms |
| cube tap short-circuited to 1.0 | 22.441 ms | 42.133 ms |
| **the whole cube tap** | **7.269 ms (24.5 %)** | **7.921 ms (15.8 %)** |

Every ceiling below is a fraction of **7.27 ms**, not of ~14.5.

### The census, and the two poses that disagree

`--shadow_tap_census` counts, per (tile x light-slot): pixels of the tile for
which the light survived the per-pixel mirror/dot/range gate, and how each cube
tap resolved. Main-view tiles only.

| | greets t=5743, 1920x1080 | his pose t=3122, 1512x848 |
|---|--:|--:|
| shaded px/frame | 1 043 940 | 643 389 |
| tile-light entries/frame | 837 (8.72 lights/tile) | 1 453 (15.14 lights/tile) |
| **DEAD entries** (zero in-range px) | **80 = 9.6 %** (74 carry a cube) | **763 = 52.5 %** (433 carry a cube) |
| dead loop-prologue px/frame | 0.87 M | **5.17 M** |
| cube taps/frame | 4.725 M | 0.848 M |
| tap result | lit 40.8 %, shadowed 54.7 %, partial 4.5 % | lit 98.8 %, shadowed 0.2 % |
| **tile-uniform taps** | **16.8 %** all-lit, 0.0 % all-shadowed | 91.6 % all-lit |

The two poses are complementary and neither alone would have told the truth:
t=5743 is tap-heavy and light-list-clean; **his pose is the opposite — half of
every tile's light list cannot light one pixel of that tile.**

### DONE, shipped default ON, byte-null: `--deferred_tile_sphere_cull`

The tile light list culled a light against the tile's screen rect **and**, 
separately, against its z-extent. A conjunction of two separable projections of
a sphere is strictly weaker than the sphere test: a light off the **diagonal**
corner of a tile's frustum chunk passes both and reaches no pixel. Fixed by
testing the light's range sphere against the tile's chunk bounding sphere —
the sphere `tileChunkSphere()` already builds for the spot-cone cull.

Byte-null by construction: it can only drop a pair whose every pixel is farther
than the light's cull range, i.e. every pixel that would have failed the
per-pixel `len2 > range2` test anyway, and it reads the **same** `range2` that
test compares against so a `--deferred_max_range` clamp cannot desynchronise
them. **The census proves it structurally, not just by hash:** the cull removes
397 entries per frame at his pose and the cube-tap count does not move by one
(0.848 M both ways).

| | entries/frame | dead entries | dead prologue px | cube taps |
|---|--:|--:|--:|--:|
| t=5743 off / **on** | 837 / **793** | 80 / **36** | 0.87 M / **0.39 M** | 4.725 M / **4.725 M** |
| his pose off / **on** | 1453 / **1056** | 763 / **366** | 5.17 M / **2.50 M** | 0.848 M / **0.848 M** |

**Cost, three-arm interleaved (parent binary / feature OFF / feature ON), one
asset tree, min over rounds.** The box carried load 10-13 from other agents, so
`Ginstr/f` is the column to read — it reproduces to 0.3 % and a descheduled
worker retires no instructions:

| | parent | OFF | **ON** | ON vs OFF |
|---|--:|--:|--:|--:|
| **his pose** lighting-w1 Ginstr/f | 1.998 | 2.001 | **1.935** | **-0.066 (-3.3 %)** |
| his pose renderFrame Ginstr/f | 5.163 | 5.168 | **5.097** | -0.071 (-1.4 %) |
| his pose lighting-w1 wall_min | 19.275 | 19.545 | 18.979 | -0.57 ms |
| **t=5743** lighting-w1 Ginstr/f | 3.296 | 3.296 | **3.278** | **-0.018 (-0.55 %)** |
| t=5743 renderFrame Ginstr/f | 5.114 | 5.114 | **5.091** | -0.023 (-0.45 %) |

**Stated honestly: the wall column does not separate the arms at t=5743** — the
per-round spread there is +/-1 ms against a 0.5 % instruction delta. The
instruction column is monotone in the right direction at both poses and the
mechanism (2.67 M fewer prologue iterations a frame at his pose, ~18
instructions each) predicts the size it measures.

GATES, cull ON *and* OFF on the same binary — differential, so the claim is
"this change moved nothing", not "the hash happens to match":

| pin | ON | OFF |
|---|---|---|
| greets `778fa6acd85a69cf241babefcdaf598e` | 3/3 | 2/2 |
| fountain `8db68ccb59416e9a44037e9f387b7bd9` | 2/2 (run 1 cold-bake discarded) | 3/3 |
| city `3f8948232c192a979ffe7f76c4b387ab` | 2/2 | 2/2 |
| `render_gate.sh` | ALL FOUR PASS (`4ac809e5` / `826c09e6` / `b41894f9` / `166fa25a`) | — |

**Residual, named rather than hidden:** the chunk sphere spans the tile's
*opaque* depth bounds, so a transparent pixel in front of `zMin` is outside it.
That exposure is not new — the shipped `deferred_zcull` already rejects on the
same `zMin` — and it does not fire on anything we ship: the greets pin runs
`--glass-refract=1 --glass-test --xpar-peel-passes=4` and the fountain pin is
glass-heavy, and both are byte-identical either way. `DeferredVolumetric` is
structurally immune: it reads only `tileLights[].zMax` / `.hasSky` and iterates
the frame-global `ViewLightsSoA` for the integration itself, never the per-tile
entries — which the passing `conetest` / `halotest` rows confirm.

### REFUTED at the tile: uniform shadow classification. And the reason is GRANULARITY, not coherence

A perfect oracle — free classification, zero error — replacing every tap in a
tile-uniform (tile x light) entry with one answer would remove **16.8 %** of the
taps at t=5743. Against the measured 7.27 ms that is a **1.22 ms** ceiling, for
a change that moves bytes, whose real corner-sampling classifier captures only
part of it, and whose errors land on **160x135 px** blocks — the most visible
seam size in the frame. Not worth building; not built.

But the parked idea was right that the shadow field is coherent. It was wrong
about where. `--shadow_tap_census_block=B` asks the same question per BxB block
(greets t=5743, uniform share of all 4.725 M taps):

| granularity | uniform taps |
|---|--:|
| tile (160x135) | 16.8 % |
| 32x32 | 35.8 % |
| 16x16 | 50.9 % |
| 8x8 | **76.1 %** |
| 4x4 | 89.5 % |

**SHIPPED 2026-08-15 (docs/SESSION_STATE.md 2026-08-15b), and two of the three
requirements below were wrong.** `ShadowMap::uniSD` / `uniDyn`, one u32 per 8x8
block holding the id of the block's **9x9 apron** (the apron removes the
straddle case instead of handling it). (a) per-8x8 DEPTH BOUNDS is a depth-mode
concept — PolyId compares no depths; (c) the frustum-corner classification was
an artefact of classifying in SCREEN space — the pyramid is indexed in
SHADOW-MAP space by the tap's own iX/iY, which has already selected its cube
face. **80.3 % of taps skip at t=5743**, `lighting-w1` -0.192 Gi/f (-6.5 %),
frame -1.01 ms; his pose -0.125 Gi/f, -0.33 ms. All eight pins unmoved,
`render_gate` 4/4, 9 poses x 4 configs identical. The ceiling below is NOT
"76 % of 7.27 ms": a skipped tap saves ~50 instructions (loads + compares +
addressing); the face select, the 3x3 matmul and the projection are untouched
and are the majority of the tap. Original specification follows.

**TODO, re-specified as a BYTE-NULL design at 8x8.** PolyId mode makes an exact
test available that a corner sample never was: over a block's footprint on the
cube face, if every texel carries the same id `c`, the 2x2 PCF is exactly
`occ = (c != 0 && c != receiverId) ? 1 : 0` — one compare per pixel, no taps, no
error. What it needs: (a) per-8x8 depth bounds (a min/max reduction over
ZPage16, the shape `computeTileDepthBounds` already has), (b) an id-uniformity
pyramid per cube face, built once per bake, (c) a per-(block x light)
classification projecting the block's 8 frustum-cell corners and bailing when
they straddle two faces. Ceiling: 76 % of 7.27 ms minus the classification, and
unlike the tile variant it costs no pixel its exactness.

### Dead hypotheses, with their numbers

* **"Cube taps still run for lights the pixel is out of range of."** FALSE in the
  shipping kernel. The scalar light loop's order is mirror id -> `dot < 0` ->
  `len2 > r2` -> bounce portal -> cone -> map shadows -> cube tap, so no tap can
  belong to an out-of-range pair. The census closes it: the sphere cull deletes
  397 (tile x light) pairs a frame at his pose and the tap count is unchanged.
* **...except in the 8-wide `--deferred_vec` kernel, where it is REAL.** A lane
  failing `mask_range` (or the mirror-id match) gets `safe_len2 = 1`, hence
  `lenInv = 1`, `dist = 1`, `k = dot * (1 - 1/Range)` — which stays **positive**
  for any front-facing lane, so it passes the `kArr[lane] <= 0` guard and calls
  `resolveCubeAtten`, whose result the `mask` blend then throws away. It costs
  this box nothing (`FDS_DEFERRED_VEC_DEFAULT` is 0 on arm64) and is left alone
  deliberately: no pin covers that path, so a "free" fix there would be an
  unverifiable one. On an x86 build, where the default is 1, it is live waste.
* **"The tile light lists do no range culling."** They do — screen rect
  (`lightSphereScreenRect`), z-extent, spot cone, mirror-footprint presence. The
  gap was only that rect AND z-extent approximates a sphere separably.

### The instrument had to be compiled out, and that is its own measurement

The census hooks sit in the two innermost bodies of the hottest loop in the
engine. Never taken, they still cost **+0.040 Ginstr/f (+2.0 %) on lighting-w1**
as first written; moving the block index out of the per-pixel body and into the
tap site took that to +0.018 G (+0.9 %); folding four counter arrays into one
did not help. It is register pressure in the light loop, the same mechanism
`d9248f6d` measured for the cube tap's `FDS_DEV` abort branch. So the hooks are
behind a CMake option (`-DFDS_SHADOW_TAP_CENSUS=ON`), default OFF, and the
shipping kernel measures **+0.003 G (+0.15 %)** against its parent — inside the
0.3 % reproducibility floor. The flag stays registered and prints the rebuild
line when it cannot do anything.

## 2026-08-12 — TODO: the cone INTEGRATION BODY is now the majority of the cone pass

> **UPDATE 2026-08-15 (round 7, docs/HW_PROFILING.md §15).** Two of the things
> this entry points at have since been settled, so read them before starting:
> the ablation split it asks for **was re-run on the vectorised arm** — on
> *chase*, the scene that turned out to own the biggest cone bill (§14.2) — and
> the `!segPath` gate quoted in the first bullet **is gone** (§14.3: it was set
> on a round-1 greets measurement that the kernel has since invalidated; greets
> now improves without it). The prologue this entry dismisses as not-the-lever
> was in fact worth **−9.1 % of chase's pass and −8.1 % of city's**, bit-exact,
> because the spot loop is the innermost of three (§15). What is genuinely left
> of the body: the per-segment shadow tap on greets (`shadowed=51` of 51 spots,
> unpriced), and the `rsqrt_nr_x8` question in this entry's second bullet, still
> open. The 8-segment `W²`/`D·W` closed form is **REFUTED** — built and measured
> at +0.1..+0.7 % instructions, because that loop runs only on ALIVE pairs
> (8.1 % of chase's at t=800); see §15.5 rather than rebuilding it.


The per-lane quadratic solve is **DONE** (`--vol_cone_solve_vec`, default ON,
bit-exact, −9.4 ms/frame on city t=1961; docs/HW_PROFILING.md §9). That moves
the cone pass from 4.09 → 2.87 Ginstr/f, and it is *still* the biggest single
item in the frame (2.87 of 7.07, against DeferredLighting 1.25, fastfog 1.09,
gbuffer 0.89, TBR-render 0.85) — but its composition has flipped. The untouched
SIMD body + shadow taps + accumulate is now **~1.52 of 2.87 G, i.e. the
majority**, so the next lever inside this pass is the integrand, not the
prologue.

**That ~1.52 G is INFERRED**, by holding a16567b's 63.6/36.4 ablation split and
assuming the body is unchanged (its code is). It has not been re-measured on the
new arm. **First step for whoever takes this: re-run the a16567b ablation
against the vectorized arm** (keep the prologue, `continue` before the
integration, sink the result, diff `Ginstr/f`) and get the real split before
believing anything downstream of it.

Two things already known about that body, from this work:

* It is the `useAnalytic` closed form for wide cones (city) and the 8-segment
  hybrid for narrow/turbulent ones (greets). Those are different cost shapes and
  will need separate numbers — the solve port itself won on one and lost on the
  other, which is why it is gated on `!segPath`.
* Its `rsqrt_nr_x8` / `_mm256_rcp_ps`+NR chains are *not* obviously safe to
  cheapen. The approximation family measured a dead loss in the SOLVE (raw
  estimates: +1.6 % instructions, −1.0 % wall — NEON estimates are 8-bit, so
  usable accuracy costs more than the divide), but the body's arithmetic mix is
  different and the question is open there. Measure raw-vs-NR first; it closes
  the family in one build.
## 2026-08-12 — PARKED: the mirror-shard PER-FACE cone cull. It works, it is byte-identical, and it recovers none of the 10.5 ms — the bake is 78 % deferred lighting

`ddb1d15` closed with a recorded next step: *"The right accelerator is a
per-FACE test (face bounding sphere vs cone); nobody has written it."* It is
written now (`FDS/RENDER/ReflFaceCull.cpp`, `--shard_cone_cull=2`, compile-time
gated behind `-DFDS_SHARD_BAKE_LAB=ON`). **The test is correct and the premise
is wrong.**

**IT IS CORRECT — byte-identical, not "close".** At the shatter bracket
(`--repro=greets@t=3122 --repro_from=3112 --repro_settle=0`,
`FDS_GREETS_SHATTER=1`, `FDS_GREETS_CAM="28.8,10.8,-62.85,1,0,0"`), the whole
1024² reflection atlas with the cull on is **0 of 1 048 576 pixels different**
from the cull off, with **84.3 % of face tests rejected** and the face list
going **9 921 → 8 678** entries. The invariant that buys that: a face whose
bounding sphere reaches the cone survives WHOLE, and a face that survives is
transformed and rasterized with its vertices UNTOUCHED — no stamped fake
positions, which is the entire failure mode of the per-vertex form.

**THE PREMISE IS WRONG: THE SHARD BAKE IS NOT GEOMETRY-BOUND.** New
`[SHARD-PHASE]` attribution (on `FDS_SHARD_REFL_PROF`), core-ms summed over 12
workers, min-of-6 interleaved, same bracket:

| phase | cull off | per-face cull | legacy per-vertex (broken) |
|---|--:|--:|--:|
| `Transform_Objects` | 6.5 | 7.5 | 6.4 |
| G-buffer fill (raster) | 27.4 | 29.7 | 7.6 |
| **`Render_DeferredLighting`** | **124.4** | **129.1** | **46.2** |
| volumetric cones | 0.2 | 0.3 | 0.1 |
| **wall ms** | **14.1** | **14.7** | **6.8** |

The geometry front-end is **4 %** of the pass and the deferred shading of the
reflection's own pixels is **78 %**. So the 4.0 → 14.5 ms `ddb1d15` priced is
not culling that was lost, it is **the reflection appearing** — every one of
those pixels is wanted, and no cull, however conservative or however tight, can
take them back. The per-face cull's 12.5 % fewer face-list entries are faces
that rasterize zero pixels; dropping them is free and buys nothing.

**AND IT COSTS ALL THREE SCENE PINS TO CARRY.** Merely having the call inside
`Transform_Objects`' per-mesh body moves greets `778fa6ac→7a6370a1`, fountain
`8db68ccb→eebf68e6` and city `3cbe42b1→80583b85` under `-ffp-contract=fast`, on
snapshots that never shatter a mirror — the same hazard
`docs/VISIBILITY_PLAN.md §8a` records for `--xfrm_pass_prof`. Bisected: the
branch it adds to the face loop is byte-null; the CALL is not, at either call
site tried (before the vertex loops, and after them). Hence the compile-time
gate rather than a re-pin: **0.1 ms is not worth three pins.**

**WHERE THE NEXT ms ACTUALLY IS, then, and it is a big one:**
`Render_DeferredLighting` runs **238 times per shatter frame on a 64² target**
and is 78 % of the pass. Its per-invocation fixed work — light binning into
tiles, the tile-light lists, the shadow-atlas setup — is being paid 238 times
for 4 096 pixels each. STATUS: **DONE 2026-08-12, and the diagnosis above named
the wrong suspects** — see the dated block at the top of `docs/SESSION_STATE.md`.
Measured, the orchestrator prologue this paragraph blames is **9–10 %** of the
pass. The fixed work is per **TILE**, not per invocation: the tile grid was
engine-global at 12×8, so a 64² cell walked 96 tiles (32 of them entirely off
the right edge, shading nothing) = 22 848 tile invocations per shatter frame,
each paying a kernel prologue **and** a `renderns::tileDone` release+acquire on
a semaphore all 12 workers share — **3.4–4.0 µs of core time per pair contended,
against 34.5 ns uncontended**. Two flags, both default-on, both byte-null:
`--deferred_inline_tile_sem` (inline dispatch posts no permit) and
`--deferred_offscreen_tile_px` (offscreen targets size the grid to themselves).
`Render_DeferredLighting` 121.7 → 93.6 core-ms, bake wall 13.6 → 11.6 ms, fixed-
work share 32 % → 3 %, break frame 46.25 → 43.88 ms. Atlas, break+1 frame, all
three pins and `render_gate` byte-identical. **What is left is not overhead:**
97 % of the pass is now the per-pixel shading of 974 848 pixels, and the only
remaining levers are rate/resolution reductions (offscreen checkerboard or
quarter-rate — the wave-2 `TileFill` machinery is already per-target selectable
— or a smaller `texRes_`), every one of which is a LOOK change on a surface the
user gates by eye. Note the per-target-HDR item below still touches this call.

**STATUS of the cull itself: PARKED — built, measured byte-identical, measured
not-worth.** Reproduce with `cmake -S . -B build-lab -G Ninja
-DFDS_SHARD_BAKE_LAB=ON` and `--shard_cone_cull=0|1|2`.

## 2026-08-11 — TODO: a PER-TARGET HDR buffer, so an offscreen bake tonemaps like the frame it feeds

**The defect.** `g_hdrBuf` is a single global sized by `Hdr_BeginFrame()` to the
MAIN view, and every HDR write in the deferred kernel gates on
`Hdr_WritableFor(ctx.xres, ctx.yres)` — the CURRENT pass's dims. So an offscreen
bake at any other resolution silently falls through to the LDR combine
(`texel*light/256 + spec`) while the main frame renders linear radiance through
exposure → ACES → sqrt. The two are different transfer functions, so an
offscreen bake cannot match the frame it is composited into, at any gain.

**Who is affected.** The **mirror-shard bake** (`MirrorShatter.cpp`) — measured
2026-08-11 at the greets shatter screen: with the coverage bug fixed the shard
mosaic reads **86.37** panel-window luma against the **73.86** the main deferred
pass renders from the same reflected eye (**+12.5, brighter**), and under
`--no-hdr` — where the main pass loses ACES+sqrt and falls to 43.28 — the mosaic
barely moves (79.05), which is the signature of a pass ignoring the frame's
transfer function. The **mirror RTT is NOT affected**: it already brackets its
bake with `Hdr_BeginFramePass(texW,texH)` / `Hdr_ActivateNoFog()` /
`Render_TonemapToVPage()` (`GreetsMirror.cpp:3273-3286`), which is exactly the
right shape — and is the reference implementation for this item.

**Why the RTT's fix does not port as-is.** The RTT bake is serial, so it can own
the global for the duration. The shard bake fans N shards across the worker pool
concurrently (`renderShardIntoCell`), so `Hdr_BeginFramePass` cannot be called
per worker without racing. The fix is to thread an HDR target through
`DeferredOverride` (alongside `gb` / `vpage` / `zpage16`) so each worker
accumulates into its own float buffer and tonemaps it, instead of every pass
reaching for one global. That also removes the `Hdr_WritableFor` dims check as
the de-facto "am I the main pass?" test, which is what makes the failure silent.

**Priced before starting:** the shard bake pass is 14.5 ms (min-of-6, load 12.8);
a per-worker `texRes²×4` float buffer is 64 KB at res 64 — the cost is the extra
tonemap sweep, not the memory. STATUS: **BUILT 2026-08-12, MEASURED, AND
OVERTURNED. The diagnosis above is right; the remedy is wrong.**

`--shard_hdr` (default **0**) is that fix, exactly as designed: a per-worker HDR
buffer through `DeferredOverride`, `Hdr_ActivateNoFogInline` +
`Render_TonemapToVPageInline` after the kernel. It makes the residual **four
times worse.** Panel-window luma at the shatter bracket, against the MAIN
deferred pass from the shard's own reflected eye as ground truth:

| | luma | vs reference |
|---|--:|--:|
| reference (main pass, reflected eye) | 74.78 | — |
| shipping, `--no-shard_hdr` | 87.31 | **+12.53** (reproduces `ddb1d15`'s +12.5) |
| `--shard_hdr` | 129.79 | **+55.01** |

**Why: the shard atlas is an ALBEDO TEXTURE, not a finished image.** The shards
are ordinary opaque scene geometry, so the MAIN frame's deferred kernel samples
the atlas as a texel, lights it, writes linear radiance to `g_hdrBuf` and
tonemaps it with everything else. One A/B proves it: with the flag OFF, sweeping
the **frame's** `--hdr_exposure` 1.0 → 2.0 moves the mosaic **87.31 → 130.78**.
The frame's tonemap already owns those pixels; tonemapping the cell as well
applies the transfer function twice — and `--shard_hdr` at exposure 1.0 (129.79)
landing on legacy-at-exposure-2.0 (130.78) is that doubling's signature.

**The mirror RTT is not a counterexample, it is the clue.** It keeps the FLOAT
radiance in the material's `hdrRefl` and hands *that* to the frame
(`GreetsMirror.cpp`); its `Render_TonemapToVPage` onto the 8-bit RTT surface is
only the LDR fallback. So the real remedy for the +12.5 is an **HDR atlas** —
shard cells carrying linear radiance into the frame through an `hdrRefl`-shaped
path — not a tonemapped 8-bit cell. **STATUS of that: TODO, not started.**

**What DID ship from this, unconditionally and byte-null:** the plumbing.
`DeferredLightingCtx::hdrBuf` now carries each pass's own HDR target, replacing
the kernels' `Hdr_WritableFor(ctx.xres, ctx.yres)` test — "the global happens to
be sized like me" standing in for "am I the main pass?", which is what made the
failure silent in the first place. Main frame identical (all three pins,
`render_gate` 3/3); it is the prerequisite the HDR-atlas fix will need.

**AND IT SHIPPED A REGRESSION FOR 15 MINUTES — the plumbing's first form,
`ctx.hdrBuf = ov ? ov->hdr : …`, silently dropped the mirror RTT (the *other*
`DeferredOverride` user, which brings no `ov->hdr` because it borrows the global
through `Hdr_BeginFramePass`) off the HDR path.** Fixed in `283b46ca` by making
the override's buffer an override rather than a mode switch, and verified
2026-08-13 by **byte-identity with the pre-restructure `5adcae12` binary** —
frame `4abe5214…`, RTT slot `5199d3d1…`, against the broken tip's `c7ef96f6…` /
`ab17ac64…`. Cost while broken: 98.9 % of the RTT slot's pixels, −21.0 mean
luma on the slot, −12.3 on the panel as it appears in the frame. Full analysis
in `docs/SESSION_STATE.md` (2026-08-13). **The residual +12.5 above is
untouched by any of this and remains the open item.**

~~**GATE GAP, STILL OPEN AND WORTH ITS OWN LINE:** `render_gate`'s `mirrortest` is
cited throughout this campaign as the thing that "covers the mirror RTT". It does
not. `--scene-mirrortest` never enables `mirror_rtt` (default 0; only
`GREETS.CPP`'s `setDefault` and the editor turn it on), and measurement confirms
it: `mirrortest` is byte-identical on the baseline, the broken and the fixed
binaries — **with `--hdr` as well as without**. The order-2 RTT path has no
standing gate. A cheap one exists and is not wired: greets t=3122
`--hdr --deferred` with `FDS_MIRROR_RTT_DUMP=1`, hashing `/tmp/rtt_*.ppm`.~~
**CLOSED 2026-08-13 — `render_gate`'s fourth row, `rttslot`
`826c09e63217e778cfcef70fe0167279`.** Built on `mirrortest`, not on greets: the
same scene already prepares two order-2 slots the moment `mirror_rtt` is on
(its two mirrors face each other, `MirrorTestDriver.cpp:269` calls
`PrepareSecondOrderMirrorRtt`), and unlike a greets row it does not key on the
user's uncommitted authoring files — which is exactly why `render_gate`'s own
header keeps greets out. Recipe `--scene-mirrortest --mirror_rtt
--shard_deferred --hdr` with `FDS_MIRROR_RTT_DUMP=1`, gated surface the 4
`/tmp/rtt_*.ppm` slot dumps. Proved in **both** directions: PASS 3/3 on
`6656300b`, **FAIL on `00d28a8b`** (`2ecd5e81…`) while the other three rows pass
there unchanged. All three flags shown load-bearing by controls that do not
discriminate the binaries. Details in `docs/SESSION_STATE.md` (2026-08-13b).

## 2026-08-10 — MEMORY-SIZE SWEEP: `--mem_census`, and the per-shadow-map FList (403 MiB at 0.5 % fill)

Asked as *"the lightmap defect and the shadow-plane defect had shapes — sweep
the engine for more of both"*. The two shapes:

1. **Wrong-variable scaling** (`943d644`) — an allocation whose size is a
   product of counts, where one of the counts is the wrong thing to scale with.
   The static lightmap was `numFaces × lmRes² × numOmnis` bytes fully touched at
   init: it scaled with FACE COUNT at a flat 128²/face instead of with SURFACE
   AREA, so tessellation multiplied it ~300× for nothing (19.4 GB displaced).
2. **Scatter** (`af1f8f8`) — one logical datum split across parallel arrays that
   a hot loop always reads together. Four u16 shadow planes 512 KB apart =
   8 cache lines per tap; packing to one u32 plane bought −1.0 ms, byte-null.

**The instrument shipped first, and it is the durable part of this work:
`--mem_census`** (`FDS/Base/MemCensus.h`, default off, byte-null — it reads
sizes and writes stderr). It walks every large allocation and prints resident
bytes, whether every byte is TOUCHED, and **the formula it scales with**. That
last column is the whole point: `0.5 GB` is not actionable, `0.5 GB BECAUSE it
scales with the whole scene's face count, per light-face` is. It also prints the
process `phys_footprint` and the UNCENSUSED RESIDUAL, so a subsystem nobody has
taught it about shows up as a gap rather than as silence. **Adding a subsystem
is one function plus one line** (`FDS_MEMCENSUS_REPORTER`). Cross-checked against
`/usr/bin/time -l`: census-reported footprint 2.17 GiB vs `peak memory footprint`
2 334 362 264 B on the same run.

### Per-scene totals (1920×1080, dummy drivers, `--mem_census`, end of tick 1)

| scene / arm | censused | of which TOUCHED | `phys_footprint` | uncensused residual |
|---|--:|--:|--:|--:|
| greets (pin recipe) | 738.68 MiB | 726.51 | 1.36 GiB | 655 MiB |
| greets `--greets_displace` | **1.46 GiB** | 1.43 GiB | **2.17 GiB** | 734 MiB |
| city (pin recipe) | 760.44 MiB | 744.87 | 1.19 GiB | 459 MiB |
| fountain (pin recipe) | 249.33 MiB | 237.92 | 1.07 GiB | 847 MiB |
| chase t=800 | 125.20 MiB | 110.06 | 197.02 MiB | 72 MiB |

Caveat when reading the `texture/*` rows: `MatLib` is process-global and every
scene is initialised at startup, so those rows are the WHOLE DEMO's texture set,
not the current scene's. The `geometry/*` and `shadow*` rows are `CurScene`-only.
That is also most of why fountain shows an 847 MiB residual on a 249 MiB census.

### THE RANKED TABLE

Sizes are measured (census + `time -l`). Every *win* is labelled measured or
inferred; none of the unlanded items has been built, so none has a measured win.

| # | finding | measured size | formula | should scale with | waste | fix shape | expected win |
|---|---|--:|---|---|--:|---|---|
| **1** | **per-shadow-map FList + radix scratch** `FDS/RENDER/Shadows.cpp` (`perLightFaces[i].resize(Polys)`) | greets **83.27 MiB**, displaced **403.10 MiB** | `numShadowMaps × 2 × Polys × sizeof(FListEntry)=24`; numShadowMaps = 6×cubeOmnis + spots = **76** at greets | the faces ONE LIGHT-FACE actually sees | **97.7 % / 99.5 %** — measured fill mean **856 of 37 173** (flat) and **881 of 182 350** (displaced), max 5 560 / 5 564 | high-water-mark sizing: the `resize` already runs on the tick thread before the Phase-A dispatch, so growth there is safe; keep a per-map high-water + slack, grow-only, with an overflow guard in the fill | **inferred** −81 MiB greets / **−395 MiB** displaced RSS + the first-touch cost at init. No frame-time claim. |
| **2** | **city env paraboloid hemi sheets** `DEMO/CITY.CPP:2724` (`Materialize(sheet, kSheetRes, kSheetRes)`) | **355.00 MiB** (355 sheets) | `6 sheets × kSheetRes(512)² × 4 B × numBuildings` | the building's reflective footprint / screen size | fixed 512² for every building regardless of size | per-building sheet res from bounding radius or authored importance; or 256² globally | **inferred** −266 MiB at 256². **LOOK CHANGE — needs his eye before anything is built.** |
| **3** | **per-shadow-map mesh clones** `FDS/Base/VertexScratch.*` | greets **178.16 MiB** (93.43 Vertex + 36.04 Face + 48.69 VertexFrame), displaced **440.86 MiB** | `Σ over (shadow map × mesh it rasterised) of VIndex×140 + FIndex×162 + ceil8(VIndex)×72`; **2 445 clones** flat, 1 360 displaced | concurrent light passes — but see below | the VertexFrame slab clones all **18** SoA fields when the depth-only shadow pass needs a handful | (a) a shadow-only VertexFrame with the fields the depth pass reads; (b) merge Phase A/B per light so a scratch can be recycled | **inferred** up to −48.69 MiB greets / −119 MiB displaced from (a) alone |
| **4** | **env probe cube stores** `FDS/RENDER/EnvBake.cpp` | city **155.39 MiB** | `stores(78) × 6 faces × res(256)² × 4 B × mipchain(1.328)` | probe count × res², both authored | 78 probes at a uniform res | dedup near-coincident probes; per-probe `storeRes` from footprint | **inferred**, unquantified |
| **5** | **static shadow lightmap atlas** (the `943d644` item, capped but not closed) | greets **88.51 MiB**, displaced **147.38 MiB** | `Σ per mesh of faces × lmRes² × omnis × 1 B`; lmRes 8..128, omnis 11, 115 346 faces displaced | surface area (now partly does, via `--shadow_lightmap_texel_density`) | still linear in FACE COUNT below the density cap | lower the `shadow_lightmap_res` cap, or make the density the only knob | **inferred**, small next to 1–3 |
| **6** | **`ShadowMap::packDyn`** `FDS/FILLERS/ShadowMap.cpp:411` | greets **51.62 MiB** (= packSD exactly) | `res² × 4 B × (6×cubeOmnis + spots)` | the maps a dynamic mesh actually reaches | allocated for EVERY map; `hasDynMeshVisible` typically true for 1–2 of 6 cube faces | allocate on first dynamic hit and keep (the per-frame bit churns, so don't key on it) | **inferred** −30..45 MiB greets |
| **7** | **transparent G-buffer planes, unconditional** `FDS/FILLERS/Mekalele.cpp:125-141` | **45.6 MiB** on EVERY scene (front 16.6 + back 16.6 + z.front/z.back 8.3 + peelFloor 4.15) | `W*H × (4+4+4+4+2+2+2)` | whether the scene has transparent geometry at all | chase and city pay the back layer and the K>1 peel floor for nothing | allocate on first transparent commit | **inferred** −20 MiB on xpar-free scenes. NOT byte-null-trivial: the rasterizer reads the raw pointers. |
| **8** | **`g_stripLights[512]`** `FDS/RENDER/DeferredLightLists.cpp:397` | **8.27 MiB** BSS | `DEFERRED_MAX_STRIPS(512) × sizeof(TileLights)=16 928` | `YRes/8` — **135** strips at 1080p | sized by the CAP; ~74 % never written, so address space rather than RSS | size at resize like every other screen-derived array | low; named because the header claimed "96 KiB" (see below) |
| **9** | **DoF full-res source** `FDS/RENDER/Hdr.cpp` (`g_dofSrcF32`) | **33.2 MiB** when `--dof` runs full-res (0 in the pin arms) | `W*H × 4 × sizeof(float)` — an f16→f32 EXPANSION, i.e. 2× `g_hdrBuf` | the buffer it copies | the copy widens 2 B → 4 B for no stated reason | keep it `hdrf`, or require `--dof_downscale>1` (which skips it entirely) | **inferred** −16.6 MiB |
| **10** | **`WaterBuf[65536*4]`** CITY + CHASE | 1 MiB each, **¾ never touched** | a BYTE count used as an ELEMENT count | 65536 DWords (256×256) | 4× | one-token fix | **LANDED** (see below) |

**Two stale comments were load-bearing** — they are why items 1 and 8 were
invisible, and both are corrected in this changeset:
`DeferredCommon.h` claimed TileLights was *"24 tiles × 8 arrays × 128 floats =
96 KiB total"* (actual: 96 tiles × 33 arrays, `sizeof` 16 928 → 1.55 MiB for
`s_tileLights` and 8.27 MiB for `g_stripLights`), and `FaceListContext.h`
claimed *"contiguous 16-byte slots"* (actual `sizeof(FListEntry)` is 24 —
sortKey(4) + 4 B PADDING + face(8) + bbox(8); reordering does not recover the
padding, only a 32-bit face index would). Two independent audits mis-priced
those lists straight from the stale figures.

### The signature to remember, from item 1

Between the flat and displaced greets arms `Polys` went **37 173 → 182 350**
(4.9×) and the FList capacity went with it, **83 → 403 MiB**. The number of
faces a light-face actually filled went **5 560 → 5 564**. The capacity tracked
tessellation; the demand did not — because the tessellated geometry sits behind
`--greets_shadow_proxy` and never enters a shadow FList at all. That is the
wrong-variable shape in its purest form, and it is exactly what the census's
formula column is for.

### Shape 2 (scatter): the surviving candidates, ranked by base pointers × hotness

`docs/OPTIMIZATION_BACKLOG.md` already inventoried the opaque G-buffer sweep
(§"17 B/px unconditional from 6 separate arrays") and scored it *sequential*.
That judgement priced the BANDWIDTH, not the lines / TLB entries / prefetch
streams per access — which is the axis the `af1f8f8` fix actually moved. Re-open
with that in mind:

* **The wave-1 deferred kernel** (`DeferredSurfaceKernel.cpp:1666`) reads **8
  distinct base pointers for the same pixel index `i`**: `ZPage16`,
  `gb.mirrorId`, `gb.txtr`, `gb.shadowMatID`, `gb.albedo`, `gb.normal`,
  `gb.tangent`, plus `lightmapMF`+`lightmapST`. Planes are 2–8 MB apart, each
  its own `malloc`. Same shape at `:4326` (OuterVec, 6 bases), `:3226`
  (transparent, 6), `:5180` (wave-2 TileFill, 3 bases × 5 pixels).
* **The cleanest pair in the tree**: `lightmapMF` (u32) + `lightmapST` (u16) —
  one logical datum (`meshLMId | faceIdx | s | t` = 6 B that fits one u64), read
  by `resolvePixelLightmap` (`DeferredShadowSampling.h:47`) as two planes 4 MB
  apart, every pixel, whenever `--shadow_lightmap` is on. **This is the closest
  structural analogue to the shadow-plane fix that is still open.**
* **The second-cleanest**: `mirrorMask` (u8) + `mirrorMaskZ` (u16) — allocated
  together (`GreetsMirror.cpp:1171`), written together, read together twice per
  rasterizer row (`Mekalele.h:1456` and `:1510`), and the code already asserts
  they are inseparable (`if (zplane.size() < plane.size()) return; // sized
  together`). 3 B across two allocations 2 MB apart.
* **Half-res fog** (`DeferredFastFog.cpp:902`): `gFogAmt/gFogZ/gFogGR/gFogGG/
  gFogGB` — five parallel `vector<float>`, all five read at the same index, 4
  taps → **20 addresses from 5 bases per pixel** in the bilateral compositor
  (`:1002`). One logical record = 20 B that fits one 32-B struct.
* **SSAO** (`DeferredSSAO.cpp:145`): `aoZ` then `aoRaw` at the same index, 16 box
  taps in the blur and 4 in the apply. 2 bases, always together.
* **EdgeAA** (`DeferredEdgeAA.cpp:99`): `ZPage16` + `gb.normal`, 5-tap stencil
  over 3 rows = ~6 lines from 2 far-apart bases per pixel.
* **`TileLights` scalar per-light loop** (`DeferredSurfaceKernel.cpp:2504`):
  ~22–25 base pointers per light per pixel, consecutive arrays 512 B apart. This
  is the path every normal-mapped pixel takes. **Caveat for judgement:** 16.9 KB
  per tile is L1/L2-resident, so this is a latency/µop item, not a DRAM one.
* **NEGATIVE RESULT, reported not buried:** the froxel accumulation planes
  (`gFrAccR/G/B/T` + `gFrSct`, 5 bases × 8 gathers per pixel) look like a prime
  candidate, but `DeferredFastFog.cpp:2528` records that **an AoS repack of the
  froxel arrays moved nothing**. Read that before spending time there. It may
  have been measured against the uniform-group fast path at `:2486` that already
  collapses the common case — worth confirming, not worth assuming.

### Landed in this changeset (byte-null, gated)

* `--mem_census` + `--mem_census_frame` and reporters for: the three G-buffers
  and their optional planes, the xpar Z/peel planes, cube+2D shadow maps and
  their swizzle copies, the per-shadow-map FList and mesh clones, the main face
  list, the static lightmap atlases, the HDR buffer and post chain, the
  glass-refraction snapshots, froxel + half-res fog, SSAO, the strip light
  lists, scene geometry (with mirror/proxy clones broken out), texture pixel
  data + mip chains by role, env probe stores, and the framebuffer + Z-buffer.
* `WaterBuf[65536*4]` → `[65536]` in CITY and CHASE (item 10). Write-only in
  live code; the `memcpy` of `65536*4` BYTES now exactly fills it.
* The two stale size comments above.

**Gate: differential, on ONE tree snapshot** (other agents held uncommitted work
in the shared tree, which makes an absolute pin meaningless — see the
2026-08-09c hazard note in `docs/SESSION_STATE.md`). Two binaries, `DEMO_memA`
(with this diff) and `DEMO_memB` (my hunks reverted, everyone else's kept),
3 runs each after discarding run 1: **identical, and equal to the recorded pins**
— greets `778fa6acd85a69cf241babefcdaf598e`, fountain
`8db68ccb59416e9a44037e9f387b7bd9`, city `3cbe42b166847e40f7071eedb48d613c`,
chase t100/400/800/1200/1600 `76e7cf68…` `d458e82b…` `c145c7a5…` `31aa5203…`
`1544b0e7…`. 24 runs, one value per cell.

### Not re-litigated

`Vertex` 140→68 and `Face` layout are refuted in
`docs/SOA_VERTEX_REFACTOR.md` (2026-08-09) and §2 below; the census reports
their bytes but this sweep does not reopen them.

## 2026-08-09 — HOT-STRUCT SWEEP: the front end is closed, the SHADOW-TAP planes are the item

Asked as *"trimming hot-struct bloat paid off once — is there more, here or in
other structs?"*. Three structs were audited against their consumer loops and
measured. **The winner is not `Vertex` and not `Face`; it is the four shadow-map
planes, and the mechanism is the same one (`cache LINES touched per access`).**

### 1. `Vertex` 140 → 68 (SoA Phase 5) — **PARKED, re-refuted with fresh numbers**

Full working in docs/SOA_VERTEX_REFACTOR.md (2026-08-09 section). Short version:
`VERT` is the only bucket it can touch and it is **0.345 ms at greets / 0.611 ms
p50 at city / 0.114–0.158 ms at chase**; the best case is ~40 % of that =
**0.24–0.31 % of frame**, for an 11-file refactor across two alternative transform
pipelines. chase is FACE-dominated (FACE 2.9–3.3× VERT), so it is the wrong lever
there by shape as well. Not attempted; do it for the one-writer contract if ever,
not for perf.

### 2. `Face` (162 B, pack(1)) — audited field-by-field; **layout work NOT indicated**

Full consumer audit done (every field × every compiled consumer, `FL.CPP` and
`3DS/WORLD.CPP` excluded as dead). Two structural facts worth keeping:

* **`alignof(Face) == 1` and the stride is 162, so `gcd(162,64) == 2` — a `Face`
  in `TriMesh::Faces[]` has 32 distinct cache-line PHASES.** Faces do not start on
  line boundaries; each spans 3 or 4 lines depending on index. **Any "put the hot
  fields in line 0" plan is defeated by the phase rotation** unless the struct is
  also padded to 128/192 or split into parallel arrays. This is the thing that
  makes `Face` unlike `Vertex`, and it should be checked before anyone proposes a
  regroup.
* **24 bytes of the struct are cold and sit in the middle of it.** `EU1..EV3`
  (offset 72) has exactly one active reader in the built tree —
  `FRUSTRUM.CPP:1065-1070`, *inside* `if (F->Flags & Face_Reflective)` — and
  downstream only `TheOtherBarry<…, TEXTURETEXTURE>` consumes it. `ReflectionTexture`
  (offset 112) is the same reflective-only gate. Together 32 B of a 162 B stride
  that the FList-build loop and Mekalele's per-face prologue both walk over for
  nothing. Evicting them to a side table indexed for reflective faces would take
  `Face` to 130 B.

**But the prize is bounded and was measured at ~zero.** The FACE bucket is only
0.254 ms (greets) / 0.539 ms (city), and §2 of the SoA doc already measured that
reducing lines-per-deref does not move it (the accesses are cache-resident). The
one live cross-core effect found in the audit was tested directly and **also
measured neutral** — see item 4. **Recommendation: do not spend effort on `Face`
layout.** The audit is recorded so the next person does not re-derive it.

### 3. Cube-shadow tap: the packed plane is now the SOURCE OF TRUTH — **DONE, shipped ON, byte-null**

**This was the item, and it landed.** At greets t=5743 the cube tap was **10.28 ms
of a 30.5 ms `lighting-w1`** (`--prof_no_cube_tap`, min-of-3: 30.518 → 20.238;
`--prof_no_lights` → 11.543 for scale). A PolyId tap needs a texel's `polyId` AND
its `depth`, for the static pair and the dynamic pair — and those were **four
separate `std::vector<uint16_t>` that at greets' 512² sit 512 KB apart.** 32 bytes
of useful data, gathered from 4 base pointers over 2 PCF rows = **up to 8 cache
lines (~512 B of line traffic) per tap**; the static-lightmap composite path's
dynamic-only tap still cost 4.

`ShadowMap` now holds **two `std::vector<uint32_t>`** — `packSD` and `packDyn`,
each texel `z | (ShadowMatID << 16)` — and they are the ONLY representation.
A texel's id+z is ONE 32-bit load: **8 lines → 4, and 4 → 2.** Bytes resident are
unchanged (4 × u16 either way); only the grouping changed. `ShadowTexZ` /
`ShadowTexId` / `ShadowTexPack` (FDS/FILLERS/ShadowMap.h) are the accessors.

The probe flag `--shadow_plane_pack` and `CubeShadow_SamplePacked` are **deleted**:
with the packed plane as the source of truth there is nothing left to A/B.

Measured, greets t=5743, 1920×1080, dummy drivers, `--deferred_prof=1`, per-frame
`wall_min`, **min over 11 interleaved rounds** (batch 1 = 6 rounds, load 6.2–8.9;
batch 2 = 5 rounds, load 9.5–12.3, arm order alternated). Matched pair from ONE
tree snapshot at `63bdc85`, base = the four-u16 tree, so the only difference is
this diff. The first round after each rebuild was discarded.

| arm | `lighting-w1` | `renderFrame` | `shadow-bake` |
|---|--:|--:|--:|
| shipping, four u16 planes | 27.620 | 44.730 | 2.474 |
| shipping, **packed** | **26.423** (−1.20) | **43.720** (−1.01) | **2.307** (−0.17) |
| `--no-shadow_lightmap`, four u16 planes | 27.696 | 44.832 | 2.488 |
| `--no-shadow_lightmap`, **packed** | **26.439** (−1.26) | **43.840** (−0.99) | **2.295** (−0.19) |

**Both arms beat the derived-copy probe's numbers, which is the prediction.** The
probe measured −0.42 shipping / −1.05 full-tap on `lighting-w1` while *also* paying
a per-bake rebuild of 144 MB; removing the rebuild and keeping the layout gives
−1.20 / −1.26. The probe's numbers were correctly called a lower bound.

**The `shadow-bake` win is the cleanest signal in the table**: 11 of the 12 packed
bake samples sit below the *minimum* of the 12 base samples (packed 2.295–2.495,
base 2.474–2.671). Mechanism, and it is not the tap: the per-frame dynamic bake
clears ONE plane instead of two arrays over the same bytes, and `ShadowBarry`'s
masked store writes z+id in one 32-bit `select` instead of a `blendv` on the z
array plus a per-lane scalar scatter into the id array.

**A caveat measured and reported, not hidden:** `--no-shadow_lightmap` no longer
doubles the win the way it did for the probe (−1.26 vs −1.20, not 2×). The
lines-touched model predicts a bigger spread; the honest reading is that at
min-of-11 both arms are near the same floor and the extra leverage is being
absorbed elsewhere in `lighting-w1`. The per-round record is what carries the full
tap: with the arm order alternated, the `--no-shadow_lightmap` arm favours packed
**5/5**, and the shipping arm 3/5. In the fixed-order batch the last arm in each
round absorbed the round's load drift — a positional bias worth remembering when
reading any interleaved A/B here.

**Cold readers pay nothing, and that was checked.** The static-lightmap bake and
the fog/volumetric spot taps read only ONE half of a texel, so they now pull a 4 B
word where they used to pull 2 B. That is 2× the bytes but the SAME number of cache
lines per tap, and it measures nil: `[LM] LightmapBake_Static` 53.7 ms base vs
54.0 ms packed (min-of-11), inside the spread.

**Byte-null, certified differentially** — matched base-vs-packed pair from one tree
snapshot, 3 runs each, interleaved so the tree's concurrent authoring edits land on
both arms: greets `778fa6ac…`, city `3cbe42b1…`, fountain `8db68ccb…`, chase
t100 `76e7cf68…` t400 `d458e82b…` t800 `c145c7a5…` t1200 `31aa5203…` t1600
`1544b0e7…`. **48 hashes, 24 matched pairs, zero differences**, and every one also
equals the recorded absolute pin.

**Memory: neutral, which is the point.** The probe form cost **+144 MB** of derived
copy (greets carries 11 cube maps × 6 faces at 512² plus 10 spot maps at 256²;
66 × 512² × 4 B × 2 + 10 × 256² × 4 B × 2 = 143.7 MB). Source-of-truth costs zero:
peak footprint min-of-3 **1420.0 MB base vs 1415.6 MB packed**, i.e. equal within a
±5 MB run-to-run spread. There is no rebuild pass left to time.

Orthogonal to `--shadow_swizzle`, which attacks the ROW-STRADDLE axis and measured
NEGATIVE in all 15 shapes (docs/ARCHITECTURE_NEXT.md). That experiment never
touched the parallel-plane axis; it survives, retargeted at the two packed planes
(`packSDSw` / `packDynSw`, two derived copies instead of four).

### 4. `Face::LastMip` — a dead store from 12 workers into a shared line. **Removed; measured NEUTRAL.**

Found by the `Face` audit: `MiplevelClipper` wrote `F->LastMip` once per face
**per tile** from every tile worker, gated only on `g_mipLastMipWrite` (true), while
**both readers are gated on `mip_hysteresis > 0`, which is DEFAULT 0.** So at
shipping defaults it was a dead store — into byte 136 of a 162-byte `Face`, a line
shared with `ownerMirrorId` / `behindMirrorMask` / `A_idx..C_idx` / `frame`, all
read by other hot paths. Now gated on the same `mipHyst > 0` the readers use
(FRUSTRUM.CPP:784/867). Byte-null by construction.

**Measured NEUTRAL** — matched A/B pair built from ONE tree snapshot (the only
difference is this diff), greets t=5743, min-of-6, quiet box: `gbuffer` 5.286 →
5.201 (−0.085), `renderFrame` 43.630 → 43.440 (−0.19), `shadow-bake` 2.480 → 2.477.
All inside the run-to-run spread.

**That neutral result is itself a finding: it prices Face-tail cross-core line
contention at ~0 and is the direct evidence behind item 2's "do not do `Face`
layout work".** Kept because a provably dead store should not be executed, not
because it bought time.

### Also inventoried, not acted on (ranked by streamed bytes/frame)

| item | traffic/frame | residency | verdict |
|---|--:|---|---|
| cube-shadow taps (2 packed planes × 2×2 PCF + header) | ~0.5 GB of line traffic | 12.6 MB/cube omni ≫ LLC → DRAM | **item 3 above — DONE, halved** |
| `Material` pointer chase | ~796 MB of accesses (6 lines/px: 1 matTable + 5 `Material`) | table ≤ 114 KB → **L2, not DRAM** | `sizeof(Material)` = **467 B** (was 455 before `EnvBakeOfs[3]` landed in 63bdc85), pack(1), no `static_assert`. Per-pixel fields are spread over lines 0/1/3/4/6 and the "hot fields on line 0" comment is **stale** — `TintR/G/B`, `SpecMul`, `AoStrength`, `Roughness/MetallicMap`, `Reflection`, `TbnHandedness` all drifted off it. No `matID` memo anywhere (every kernel re-chases per pixel). A 64-byte hot-fields record per matID (16 KB, L1-resident) collapses 5 lines → 1. **PARKED — now MEASURED, and refuted as a latency item: see §"2026-08-10 — hardware counters" item 2. Bound on the win is ≲0.1 ms/frame.** |
| texture + aux-map point fetches | 130–660 MB | mip chains → DRAM | already measured: the albedo gather is worth only 0.71 ms (PERF_STATE) |
| `g_hdrBuf` | 166–232 MB (10–14 sweeps × 16.59 MB) | > LLC | `hdrf` is already `__fp16` on arm64 (8 B/px, not the 16 B PERF_STATE still quotes) |
| opaque G-buffer sweep | 35–44 MB read | sequential | 17 B/px unconditional from **6 separate arrays** = 9 concurrent streams/worker |
| `Omni` | 60 KB | trivial | `sizeof(Omni)` = **515 B**, not the "[256 Bytes]" its comment claims, and **302 B of it is a `Vertex` + a `Face`** the lighting path never reads. Pure list-walk pollution; harmless at 117 lights. |
| `TileLights` | 1.55 MiB resident | L1 per tile | `sizeof` = 16 928 B × 96 tiles. Its own comment says "96 KiB total" — **stale by 17×** (8 arrays grew to 33). Not a bandwidth item: ~1.3–3.8 KB touched per tile. |
| `FListEntry` | — | — | 24 B, not the 16 B its comment claims (bbox fields were added) — 2.67 per line, not 4. |

## PBR quality series (incremental, one at a time, user reviews each)
Discussion: engine PBR compositing vs canonical (SESSION_STATE / chat 2026-07-13).
The engine is standard in the big decisions (additive diffuse+specular, GGX,
Schlick, metalness→F0, roughness-mip reflection); it simplifies in 3 places —
each is a candidate below.

**SERIES COMPLETE (2026-07-14):** all four increments (#1 analytic split-sum
env-BRDF, #2 SH irradiance ambient, #3 (1−F) diffuse energy, #4 multi-scatter
compensation) have LANDED on fog-wt, each default-OFF and each measured
effectively free (within the frame noise floor). All four await user
look-approval before any default flip. The env-BRDF LUT and diffuse-cubemap
items below stay PARKED.

- **#1 analytic split-sum env-BRDF** — DONE (e0640fe, flag `env_brdf_analytic`,
  default OFF). Measured +0.72 ms on greets = within noise → effectively free.
  Retires the `f90=1-rough` hack. Awaiting user look-approval to default ON.
- **#2 SH irradiance ambient** — DONE (d29302a, flag `sh_ambient`, default
  OFF). Replaces the flat `Sc->Ambient` constant with 9-coeff L2 RGB SH
  irradiance evaluated per-pixel along the (post normal-map) shading normal
  (~9 FMA/channel, no gather; clamped >=0). `SHAmbient_EnsureBaked`
  (EnvBake.cpp) projects a scene-center 32²×6 env cube — rendered through the
  same deferred cube-face path the env-reflection probes use — into 27 floats,
  A_l/π folded in so a uniform env evaluates back to its own colour
  (magnitude-comparable to the flat ambient). Injected at all three opaque
  kernel ambient sites (scalar wave-1 = greets; OuterVec vec-fill = city,
  covers its scalar fallback via `lane_ambB`; TileFill quarter/checker).
  Transparent kernel keeps flat ambient (out of scope). **Measured (arm64,
  1920×1080, whole-frame, 8 interleaved OFF/ON rounds, min-of-each):**
  greets scalar Δ = **+0.84 ms** (min 73.53→74.38; noise floor 6.3 ms) →
  within noise = **effectively free**; city OuterVec Δ = **+3.47 ms** (min
  103.26→106.73; noise floor 9.8 ms; city renders two deferred passes and the
  OuterVec SH is a per-lane *scalar* loop, hence the larger — still
  sub-noise-floor — delta). Flag-OFF byte-identical: city `37e62845…`,
  fountain `51fff7cd…`. A/B: matte pillars/walls gain directional 3D form
  (flat dead silhouette OFF → shaped fill ON); heavily-lit/emissive regions
  barely move (ambient is a small fraction there, HDR-compressed). Awaiting
  user look-approval to default ON. NB the one-shot bake renders the scene →
  nondeterministic on greets, so md5 pins must gate with the flag OFF.
- **#3 (1−F) diffuse energy conservation** — DONE (ccc0229, flag
  `diffuse_energy`, default OFF). Scales the deferred DIFFUSE accumulator by
  `(1-fres)` at the combine, where `fres` is the SAME per-pixel Schlick Fresnel
  the env-specular reflection already computes. Light reflected specularly (F)
  can't also diffuse; the engine scaled diffuse by `(1-metalness)` but skipped
  `(1-F)`, double-counting at grazing (full diffuse AND a strong Fresnel
  reflection). `EnvSpecComposeScalar` + `EnvComposeCityVec8` now expose `fres`
  via an optional out-param; wired at ALL opaque env-compose sites — scalar
  wave-1 (greets/fountain), TileFill, and OuterVec (both scalar-fallback lanes
  multiply the float diffuse; the vec fast-path uses an additive INTEGER
  correction `int(vf*(1-fres)) - int(vf)` so the flag-OFF path is byte-for-byte
  untouched). Transparent kernel carries no env term → out of scope (keeps full
  diffuse), same as #2. Only pixels with a reflection (Reflection>0 / metal)
  pay. **Measured (arm64, 1920×1080, city OuterVec, --snapshot=city@t=1961,
  iters=60, 8 interleaved OFF/ON rounds, min-of-each):** OFF min 100.344 ms →
  ON min 100.333 ms → Δ = **−0.011 ms** (ON marginally faster = noise; OFF
  noise floor min→max = 8.4 ms) → **within noise = effectively free** (it's ~4
  ALU ops per reflective pixel). Flag-OFF byte-identical: city
  `37e62845…`, fountain `51fff7cd…`. A/B (city glass, deterministic): reflective
  facades darken at grazing angles (blue windowed building + red facade go
  noticeably darker ON; 19.3% of pixels change, 100% darkened, mean luma −30.6,
  no pixel brightens), while the matte concrete pillar + emissive window-lights
  (no env term) stay byte-identical. Effect is PRONOUNCED on city because its
  glass has a high authored F0 (`city_env_f0=60`); on true dielectrics
  (F0≈0.04) it's the subtle grazing-only darkening the canonical BRDF intends.
  NB fountain shows zero change at its pin (its glass spheres are TRANSPARENT =
  no env term). Awaiting user look-approval to default ON.
- **#4 multi-scatter compensation** (Fdez-Agüera) — DONE (2718046, flag
  `pbr_multiscatter`, default OFF). The split-sum env-BRDF (#1) is single-scatter
  only — it drops the energy returned by repeated microfacet bounces, so rough
  metals read too dark. Adds it back from the SAME A,B `env_brdf_analytic`
  already computes in `EnvSpecComposeScalar` (a few ALU ops on reflective pixels
  only, NO new gather): `Ess=A+B` (single-scatter energy), `Favg=F0+(1-F0)/21`
  (avg Fresnel), `Fms=Favg*Ess/(1-Favg*(1-Ess))`, then `ek *= 1+Fms*(1-Ess)/Ess`
  (scales the SPECULAR energy only; the single-scatter Fresnel handed to #3 is
  left untouched). **DEPENDS ON `--env_brdf_analytic`** — it needs the A,B terms,
  so it's a NO-OP with that flag off (lives inside the analytic branch; the
  ad-hoc Schlick else-branch computes no A,B). `--env_brdf_analytic` already
  routes city glass off the OuterVec fast path to the scalar compose, so only
  `EnvSpecComposeScalar` needed wiring (4 call sites, 3 flag decls; no
  `EnvComposeCityVec8` touch). **Measured (arm64, 1920×1080, city@t=1961,
  iters=60, 10 interleaved OFF/ON rounds, `--env_brdf_analytic` as the baseline
  in BOTH to isolate #4 from #1, min-of-each):** OFF min 102.569 ms → ON min
  102.271 ms → Δ = **−0.298 ms** (ON marginally faster = noise/thermal, OFF ran
  first each round; noise floor max−min = 14.1 ms) → **within noise = effectively
  free**. Flag-OFF byte-identical: city `37e62845…`, fountain `51fff7cd…`
  (verified default AND `--pbr_multiscatter`-alone with `env_brdf_analytic` off).
  A/B (city glass, deterministic, `--env_brdf_analytic` baseline): reflective
  facades brighten — 19.2% of pixels change, **100% brighten / 0% darken**, mean
  +2.08 luma at the authored `city_env_gloss=24` (rough≈0.28, F0=0.6 → +10%
  specular), rising to +8.66 luma at a rougher `city_env_gloss=6` (the effect
  scales with roughness exactly as the compensation intends — characterized:
  F0=0.04 dielectric barely moves 1.00–1.05×, a true rough metal rough=0.8/F0=1.0
  → 1.79×, rough=1.0 → 2.22×). Non-reflective surfaces (concrete pillar,
  emissive window-lights) unchanged. NB city glass is a moderately-rough high-F0
  DIELECTRIC (metalM=0), not a true rough metal, so its brightening is modest;
  the effect is authored-material-dependent and pronounced only on rough metals.
  Awaiting user look-approval to default ON.
- **env-BRDF LUT (texture) vs analytic** — PARKED. The analytic (#1) is free +
  needs no memory; a real (F0·A+B) LUT is 1 cached tap. Only revisit if the
  analytic precision ever proves insufficient.
- **Diffuse irradiance as a cubemap** — PARKED (avoid). That's a per-pixel
  gather every lit pixel; #2 (SH) gets the same result as pure ALU.

## Micro-optimizations (only if a profile flags them)
- **env-BRDF `exp2`** — TODO-if-needed. `std::exp2(-9.28*ndv)` at
  DeferredSurfaceKernel.cpp ~1239 is scalar libm, and its result is usually
  CLAMPED away inside `std::min(rx*rx, exp2(...))` so full precision is wasted.
  The file already has a fast LUT-based log2/exp2 (used for `pow` ~2065) —
  reuse it (near-free), and it would vectorize into the `__m256` block so the
  city-glass AVX2 fast path (disabled today when the flag is on) could stay on.
  Measured free at greets scale, so LOW priority — do only if a profile shows it.

- **B5 per-face screen-bbox tile-walk pre-reject — DONE (2026-08-02, S2,
  commit 9b6d70d, `--tile_bbox_cull` default ON).** Each face's projected
  screen bbox (int16, floor/ceil+1px, from the face's own A/B/C PX/PY) is
  stamped into its `FListEntry` at FList-build time (Transform.cpp push); the
  tile walk (`RenderInnerMekalele`/`RenderInner`) 4-compare-rejects a face whose
  bbox misses the tile rect BEFORE the Face deref — a rejected face costs only
  the sequential FListEntry read (skips the 3 scattered Vertex flag loads + the
  clipper entry). PURE reject (the clipper already clips to the tile →
  byte-identical); near-plane-straddling faces (any vert behind nearZ) keep a
  cover-all sentinel and are never rejected. **Measured (t=5780, 1080p, threaded,
  40-iter p50, cull OFF→ON):** edge-displaced greets (86.6k faces) frame
  99.3→86.85 (−12.5), RNDR 63.66→51.63 (−12.0); t=2145 108.8→100.0 (−8.8);
  flags-off greets 47.5→47.0 (−0.5); city 95.1→94.1 (−1.0). Byte-null: city
  37e62845 + fountain 51fff7cd exact, render_gate 3/3, displaced-greets
  stable-pixel 1px/2.07M (greets race), wasm links. TRAP found: `frame->PX` is
  NOT populated by `VertexFrame_DumpFromAoS` (only PY), and conetest's giant
  quad has unpopulated `*_idx` — hence the fill reads AoS `F->A->PX` directly.
- **B5 shadow-bake face cull — SUPERSEDED by S1 offscreen proxy** (commit
  376f826, `--greets_shadow_proxy`, opt-in). Instead of per-face rejecting
  displaced faces in the shadow raster, the whole displaced detail is
  main-camera-only (Face_MainOnly) and a FLAT ~226-face proxy casts/reflects in
  every offscreen pass. **Measured: BAKE 27.3→21.5 (−5.8), frame 92.8→88.1
  (−4.7) at t=5780.** The win is BOUNDED — the shadow cube-face cull already
  limited per-frame wall rastering, and mixed chunks (rooms+siling) still pay
  Phase-A transform of ~22k displaced verts (only 151 pure-displaced chunks /
  59k faces are fully Tri_NoShadowCast'd). Default OFF: the flat caster's
  shadows differ from the displaced walls' on the looked-at wall (~1% px,
  maxD 142 — NOT invisible, a look call) and a flat caster carries no relief so
  it does NOT fix the reported light-bleeding.

- **S3 mesh-level AABB-vs-frustum cull — MEASURED, NOT WORTH IT (2026-08-02).**
  XFRM (the per-vertex transform, where a pre-transform AABB reject would save
  work) is 0.70 ms flags-off greets / 2.31 ms city / 7.8 ms edge-displaced
  greets — all tiny vs the RNDR (50-80 ms) + BAKE (20-30 ms) elephants. The
  existing bsphere cull + the greets Piramid chunk split already reject most
  off-screen geometry (XFRM would be an order of magnitude higher if they
  weren't firing — they are). An AABB is a tighter bound but its ceiling is a
  fraction of XFRM (sub-ms). At the heavy displaced pose the walls are ON-screen
  (that's why they're displaced) so an AABB wouldn't cull them either.
  Foundation-F AABBs stay consumed only by the env overlay; skip.

- **S4(a) fan↔edge seam holes — SPEC (not landed; risk vs budget).** The
  cross-patch heal (`MeshOps.cpp:2446-2490`) only REPOSITIONS the finer side's
  verts onto the coarser (anchor) polyline; it never INSERTS a vert on the finer
  side at an anchor kink it lacks. Fan(i/2^L params)↔edge(groove params)
  junctions have no subset relation → the fan chord bypasses the edge's groove
  kink → hairline hole. FIX = union the two sides' param lists at TRIANGULATION
  time (both sides emit `edgeVert` at every union param → welded, heal becomes a
  no-op) — a two-phase change to the tessellator (register-all-params, then
  re-emit), too invasive to retrofit safely late. First step: a diagnostic
  counting sides where neither param list ⊆ the other (the true un-healable
  count) next to the `%d T-junction pins` log.

- **S4(b) stone light-bleeding — SPEC (AO-on-direct; S1 does NOT fix it).**
  Bleeding = DIRECT disco light on mortar that the single-shadow-id collapse
  (acne fix) left un-self-shadowed. A FLAT proxy has no relief so it cannot
  restore per-block mortar self-shadow (the coordinator's S1-fixes-bleeding
  premise is geometrically wrong — verified). The map's grooves ARE the missing
  occlusion. AO already exists (rooms/floor carry it in albedo-alpha,
  `Mat_AoInAlpha`) but is applied to AMBIENT ONLY
  (`DeferredSurfaceKernel.cpp:1856-1874`). FIX = behind a default-off greets flag,
  for `Mat_AoInAlpha` displaced mats, move the AO multiply from ambient-only to
  the FINAL (ambient+direct) color after the light loop → static, acne-free
  groove darkening under direct light. Hot-kernel change (scalar path covers
  greets); flag-gated for byte-null; strength is a user look call. Tuning-only
  fallback: raise `ao_map_strength` (deepens ambient grooves, won't fully stop
  the direct leak).

- **S5 chunk-level LOD (flat+POM near/edge-displaced far) — DESIGN NOTE.**
  Largely SUBSUMED by S1: the offscreen proxy already IS the flat LOD for every
  non-main view. A camera-distance main-pass LOD would bake both meshes per
  chunk and swap by distance via the existing chunk machinery — but the S1
  Face_MainOnly/Tri_OffscreenProxy split + the proxy mesh are exactly the dual
  representation an LOD needs; extending it to swap in the MAIN pass by distance
  is the remaining step. Low priority: RNDR is pixel-bound at these poses (the
  displaced faces add little coverage — B2's ~2-2.8 µs/face is fixed cost, which
  S2 already reclaimed), so a main-pass geometric LOD buys little beyond S2+S1.
  **CEILING MEASURED 2026-08-06 and it confirms "low priority" with numbers**
  (docs/ENVDYN_DISPLACEMENT_PLAN.md §A4). Face-count ladder at t=5780 under
  `--greets_displace`: 6 522 faces (uniform L1) → 87 256 (edge carve) moves RNDR
  46.04 → 53.46 = **92 ns/face threaded ≈ 0.60 µs/face core**, i.e. **3.3–4.7×
  cheaper than B2's 2–2.8 µs serial** — `--tile_bbox_cull` took most of it.
  Halving the face count is worth −6.13 ms with the two new companions OFF, but
  with them ON the 87 k edge carve and the 43 k dome path measure **55.86 vs
  55.64, 0.22 ms apart**: the clone half of that work is already gone. So a
  per-chunk screen-space-error LOD is worth **≈0.2–3 ms** against a bake-time-only
  ladder (the bake is 2–6 s; no per-frame re-tessellation), 25–40 MB resident,
  and DMM's per-edge min-level rule to keep boundaries watertight when neighbours
  choose levels independently (today's border pins only close cracks for a FIXED
  level assignment). NOT built, deliberately; reproducible in three bench runs.

## Geometry front-end (XFRM) — measured 2026-08-05, docs/SOA_VERTEX_REFACTOR.md

> **RE-OPENED 2026-08-06 (b) with a CONTROLLED experiment — the mechanism is now
> quantified, and two items below are REPRICED.** `-DFDS_VERTEX_PAD_BYTES=N`
> adds dead tail padding to `Vertex`: not one instruction in any loop changes,
> only `sizeof`. greets t=5780 `--greets_displace`, 253 280 verts, per-frame min
> over 24, `pad=0` run first and last (drift 0.005 ms):
> **sizeof 140 → VERT 1.118/1.123 ms; 204 → 1.203 (+7.6 %); 268 → 2.315
> (+107 %).** Per-vertex time is a steep, super-linear function of the struct
> stride, with a cliff past ~256 B (stride prefetcher gives up).
> Bandwidth: ~284 B of streamed traffic per vertex at 4.41 ns = **64 GB/s on ONE
> core**, and this doc's own 958 k-vert numbers land at 64.3 GB/s (VERT) and
> 62.3 GB/s (the zero-arithmetic SoA sweep). Three unrelated loops at 62–64 GB/s
> is a single-core streaming ceiling — the one explanation for every wash on
> record (Vec8f, reciprocal estimate, single-precision divide, dropping 2 of 3
> mat-vecs, and the new field reorder).
> **Repricing:**
> - **Phase 5 / the interleaved 64-byte output array is CONFIRMED and worth MORE
>   than the −26 % below** — traffic ~284 → ~132 B/vert, i.e. on the order of
>   **2× VERT** against the measured slope. Blocker unchanged: `Vertex` must
>   split into mesh storage vs the clipper's transient type (every `RasterFunc`
>   takes `Vertex**`).
> - **"Read the per-face `Flags`/`PX`/`PY` from the SoA arrays" is REFUTED.** A
>   field reorder that puts PX/PY/Flags/TPos_AOS.z in 24 contiguous bytes cuts
>   the predicted lines per random 3-vertex deref 2.88 → 1.56 (−46 %) and moved
>   FACE by **0.6 %** (0.583/0.604 → 0.588/0.606). The A/B/C derefs are already
>   cache-resident. The 73 % is the branch chain + the per-face `F->Txtr->Flags`
>   chase + the Face stream, not the vertex reads. No `A/B/C_idx` invariant
>   needed, and no win there to collect.
> - **DONE 2026-08-06 — `--xfrm_par`, DEFAULT ON.** The main-view
>   `Transform_Objects` now runs on the pool: the object list is cut into
>   contiguous mesh-index BLOCKS (2 per worker) that are work-stolen off a shared
>   cursor, each appending into an FList segment **reserved in mesh order** (the
>   prefix sum of per-mesh `FIndex`), then compacted in block order — so
>   execution order is free while output order is pinned, and the result is
>   bit-identical to serial whatever order the workers finish in. Measured,
>   1920x1080, per-frame min over 24, min-of-arm over 3 interleaved reps, load
>   14-23: **displaced t=5780 1.546 -> 0.449 ms (-1.10, 3.4x); shipping t=5743
>   0.423 -> 0.261 (-0.16, 1.6x)**. Machinery overhead measured with 1 block
>   (inline, no dispatch): 10-35 us. Gates: render_gate 3/3 in both arms, city
>   `37e62845` and fountain `51fff7cd` PIN EXACT, chase 5-pose + cinematic and
>   greets t=1588 / t=5780 off==on, and **24 sequential runs of the greets pin
>   recipe with the parallel path on = 1 hash, 0 flips**.
>   **It also corrected the premise it was proposed on.** "The chip's aggregate
>   bandwidth is several times 64 GB/s" is FALSE for this access pattern: the
>   displaced arm streams 53.7 MB in 0.411 ms = **~131 GB/s, ~2x the single-core
>   figure**, and finer blocks do not move it — so that arm is now bandwidth-
>   bound at the SOCKET. The shipping arm is bound by something else entirely:
>   one mirror clone is 55 % of its main-view verts AND 55 % of its faces, so it
>   sits at the per-mesh LPT bound (predicted 0.221, measured 0.246). Full
>   working in docs/SOA_VERTEX_REFACTOR.md 2026-08-06 (c).
>   **Consequence for what is left:** more threads cannot help the displaced arm
>   (bytes/vertex can — Phase 5 / the interleaved 64-byte output array is now the
>   ONLY lever on it), and the shipping arm's lever is shrinking that mirror
>   clone (docs/VISIBILITY_PLAN.md 8e), which would help the serial path too.
> - **LANDED, byte-null, neutral:** the transform's `UZ`/`VZ` stores were dead
>   (the clipper overwrites them at entry) — 10 sites deleted in `fdc7a07`.

> **RE-MEASURED 2026-08-06 — THIS SECTION IS CLOSED AS A PERF TARGET.** With
> tessellation retired and `9d`'s faceless-mesh cull landed, the WHOLE geometry
> front end is **≈1.2–2.6 ms of a ~79 ms greets frame (1.5–3.3 %)**: main-view
> `Transform_Objects` **0.449–0.468 ms** min (SETUP 0.004 / VERT 0.244–0.257 /
> SOA 0.001 / FACE 0.176–0.185) at 49 447 verts, SHADOW phase A 0.74–2.07 ms
> wall across both bake calls, OFFSCREEN ~1.18 core-ms. The 7.92 ms baseline
> quoted below is `--greets_displace`, which is retired geometry.
> **One thing did land from this pass:** the per-light shadow depth/polyId
> `std::fill` ran SERIALLY on the tick thread inside the window `--shadow_prof`
> calls `xform=` — 10.50 MB / 0.19–0.40 core-ms per DynMeshes bake, i.e. MOST of
> that bucket was memset, not transform. Now cleared on the owning phase-A
> worker (byte-null at every gate). Census: `-DFDS_SHADOW_CLEAR_CENSUS`
> → `[SHADOW-CLEAR]`.
> **Phase 5's ceiling is also half what the doc claims** (−26 %, not −45 %, once
> the 13 extra SoA write streams are counted) unless the outputs are laid out as
> ONE interleaved 64-byte-per-vertex array instead of 13 SoA arrays — see the
> 2026-08-06 section of docs/SOA_VERTEX_REFACTOR.md for the corrected design,
> the verified `sizeof(Vertex)`=140 → 68 target, and the full consumer inventory
> (the migration surface is 11 files, incl. `Reflected_Transform` and FOUNTAIN).
> Prize for the whole refactor: ~0.15–0.4 ms (0.2–0.5 % of frame). Do it for
> cleanliness if at all, not for perf.

Instrument: `--xfrm_prof=N` + `--xfrm_ablate=<mask>` (both default OFF, byte-null).
Baseline at greets t=5780 `--greets_displace`, per-frame min: main-view
`Transform_Objects` 7.92 ms = VERT 4.01 + SOA 2.40 + FACE 1.45.

- **DONE — the Phase-1 AoS→SoA dual-write sweep, 2.40 ms.** `--xfrm_soa_inline`
  (now default ON) moves the SoA store into the per-vertex loops. Measured
  7.93 → 5.96 ms (−1.97, −25 %) at t=5780; bit-exact (city/fountain pins exact,
  chase 7 poses + greets t=1588/t=5780 byte-identical, `--soa-verify` clean).
- **The 958 k verts are mostly MIRROR CLONES, not the wall.** The displaced
  Piramid is 261,768 verts; the main view transforms 958,204, because
  `GreetsMirror` clones the whole scene per mirror (534,356 verts each for
  'teleporter' and 'P_TEXT.JPG#6') as ordinary meshes gated only by
  `HTrack_Visible`. **This is now the single biggest front-end lever** — bigger
  than anything left inside `Transform_Objects` — and it lives in
  `FDS/RENDER/GreetsMirror.cpp`: per-clone frustum/visible-panel culling, or a
  decimated clone of the displaced stone (the reflection does not need
  block-level relief). **CEILINGS MEASURED 2026-08-05** — see
  `docs/VISIBILITY_PLAN.md` §8, instruments `--mirror_cull_census[_cell]` +
  `--xfrm_pass_prof` (all default OFF). Exactly ONE clone is active at the wall
  poses = 56 % of main-view transformed verts; mirror RTT passes DO NOT run
  (`--mirror_rtt` defaults 0) and hide clones anyway, so this is purely a
  main-view cost. A per-source-mesh split saturates at ~40 %; a SPATIAL split
  of the clone at ~8 world units (103 sub-meshes) culls **63 % at wall poses /
  89 % at a panel pose** — but only against the MIRROR WINDOW (0.04–3.9 % of
  screen), not the frustum (2–20 %). Estimated ~2 ms off a 5.9 ms main-view
  `Transform_Objects`. NOT built: a spatial split breaks `UpdateMirror`'s
  contiguous-`ClonedMeshRange` invariant and every `m.cloneMesh` consumer
  (RTT/shatter/editor/envbake) — see §8e. **Cheaper first move: 49 % of the
  clone is the displaced Piramid; clone the S1 flat `stone_shadow_proxy`
  instead (~226 faces vs 87,256).** Look call on reflection fidelity.
- **A visibility HIERARCHY (BVH/octree) is REFUTED with numbers (§8c).** ~9,150
  mesh-level tests/frame across ~40 passes, ~7,690 of them rejections, at a
  measured ~55–61 ns per rejected sweep = **0.45 core-ms/frame** total prize,
  ~90 % of it inside 12-way-threaded shadow passes. A hierarchy also cannot
  reduce transformed verts at mesh-sized leaves, and finer leaves measured a net
  LOSS (verts −3.3 %, XFRM +1.08 ms).
- **Phase 5 of the SoA refactor is a PERF item now, not just cleanliness.**
  `Vertex` is pack(1) 140 B and the per-frame loop touches fields spanning
  offsets 4..123 — every cache line. Ablating 2 of the 3 per-vertex mat-vecs
  (34 % of the struct) buys only 8.7 % of VERT: the loop is line-bound, not
  arithmetic-bound. Moving the per-frame-written outputs out of the AoS struct
  shrinks the stride the transform walks; ceiling ~45 % of VERT. (Corollary:
  Phase 2 / Vec8f stays parked — widening lanes cannot help a stride problem.)
- **Per-face visibility test = ~73 % of the FACE bucket** (t=6097: 0.322 of
  0.395 ms). `Face::VisibilityFlagsAll()` is `A->Flags & B->Flags & C->Flags` —
  three chases into 140-byte `Vertex` structs; the tile-bbox stamp chases the
  same three again. Reading `Flags`/`PX`/`PY` from the VertexFrame SoA arrays
  (4-byte stride) is the obvious fix, BUT it requires `F->A/B/C_idx` to be
  trustworthy on every mesh and the tile-bbox comment in Transform.cpp records
  meshes where they are not (the conetest quad) — a wrong bbox DROPS a face
  where a wrong SortZ was harmless. Needs a per-mesh "indices stamped" invariant
  first.
- **Per-chunk normal cones** (bulk-accept / bulk-reject a chunk's faces without
  the per-face dot) — ~~still unmeasured~~ **REFUTED BY MECHANISM 2026-08-06, do
  not build.** A normal cone can only remove the backface term
  `AP·F->N < F->NormProd`. Two facts kill it:
  1. That term is the CHEAP half of the per-face cull. The 73 % figure above is
     `--xfrm_ablate=8`, which runs `VisibilityFlagsAll()` **and** the dot; the
     expense is the three random derefs into 140-byte `Vertex` structs for
     `Flags`, while `F->N`/`F->NormProd` are Face-local and sequential. A cone
     cannot answer the frustum test, so it leaves the expensive part in place.
  2. It cannot help the SHADOW passes **at all** — `shadowNoBackface`
     (`Transform.cpp`, `g_inShadowPass && !shadow_backface_cull()`) sits before
     the dot in the `||` chain, so the dot never runs there. That is 60–80 % of
     the front-end ms.
  Its whole ceiling is the backface dot in MAIN + OFFSCREEN, inside a 0.18 ms
  main-view FACE bucket.

## Perf (measured bottlenecks — from docs/PERF_STATE.md + the 15fps analysis)
The greets frame is ~2.5–3× a "generic deferred" frame; the fat is shadowing,
not shading. Biggest levers, in order:
- **Per-pixel CUBE-SHADOW taps ~32 ms** — the #1 cost (1.44M taps @ ~22 ns).
  `shadow_polyid_no_pcf` (single tap vs 4-tap PCF) saves ~9 ms already; fewer
  shadow-casting omnis; a cheaper cube-face select / better cache layout of the
  4 buffer streams. This is where the real fps is.
- **Dynamic-omni shadow re-bake ~12.5 ms** — re-rasterizing moving geometry to
  depth maps every frame. Cache/skip static portions.
- **Mirror RTT (teleporter, 2nd-order recursive)** — a full second scene
  render. Density/recursion knobs (`mirror_rtt_density`) are the lever.
- **Vectorize the general env-specular compose** (2026-07-31, user-queued).
  `EnvSpecComposeScalar` runs scalar per pixel for everything except the
  city-glass shape (`EnvComposeCityVec8` engages only for: uniform cube
  store across the 8-lane group + noParallax + cv-pull + no rough/metal
  maps + no env diagnostics + none of sphere-parallax/SSR/analytic-BRDF).
  Greets env surfaces are always scalar (per-material probes break the
  uniformity gate; AABB parallax; rough/metal maps). The math chain is the
  same one CityVec8 already vectorizes — per-lane stores become
  mask-selected, parallax correction is a few more vector ops; the
  gathers (face fetch) don't vectorize away regardless (ENV_NOFETCH
  attribution). MEASURE FIRST: only worth it if the env compose is a
  material slice of a greets frame vs the cube-shadow/cone elephants
  above.

## Correctness/determinism (blocks the gate, not just perf)
- **Greets env-bake non-determinism** — TODO, now URGENT-ish. Greets renders
  run-to-run distinct (4/4 by 2026-07-14, up from the old "~1-in-12 flip") —
  amplified by the user's new metallic materials (more env probes; env-bakes
  vary). This BREAKS the greets md5 gate (gate on city/fountain/pbrtest until
  fixed). Root-cause the bake ordering/threading nondeterminism. Related:
  measurement-tool-traps memory.
- **Editor metallic-import OOM** — IN-PROGRESS (targeted per-surface re-bake +
  capped editor bake res). Same env-probe subsystem as the determinism issue.
- **`--greets_displace` at t=6097 is run-to-run NONDETERMINISTIC** (measured
  2026-08-05): 6 sequential runs → 6 distinct color-PPM hashes, with every new
  flag OFF, while t=5780 in the same runs was byte-stable 6/6. So the "greets is
  deterministic again" re-pin (f4e81e9) is scoped to the NON-displaced pin
  recipe; the displaced path has its own defect at that pose. t=6097 cannot be
  used as a byte gate until this is root-caused. Not investigated — found while
  gating the XFRM work.

## Architecture cleanup — retire the ::mirUV material split (QUEUED 2026-08-05)

**What.** Replace the per-MATERIAL UV-handedness clone with a per-FACE
handedness bit, and compute `B = handedness·(N×T)` from it everywhere.

**Why now.** `GreetsFixBitangentHandedness` (DEMO/GREETS.CPP:1254) splits every
face with a negative UV determinant onto a `<name>::mirUV` clone carrying
`TbnHandedness = -1`, because the DEFERRED kernel is per-pixel and the G-buffer
has no channel for handedness — a material clone was the only path available.
Handedness is a property of a TRIANGLE's UV winding, and the determinant is
already computed at the split site; it is simply thrown into a material clone
instead of being stored. The split has caused real damage all session:

- **The parallax march never applied it** — `Mekalele.h:1462` builds
  `B = N×T` unconditionally while `DeferredSurfaceKernel.cpp` applies the sign
  in 5 places and `LightmapBake.cpp:793` applies it. With 41.8% of greets charts
  mirrored (S1d-1 census), the march walked the height field in the OPPOSITE V
  direction on those faces — the user's "texture edge moving half the face"
  swim. Found 2026-08-05 only because the user observed that deeper faces at
  STEEPER grazing angles did NOT swim, which falsified the angle-based theory.
- **`floor` has ZERO faces under its own name post-init** (they all moved to
  `floor::mirUV`), which silently gave the floor no shell at all and cost an
  agent hours.
- **`::mirUV` clones ALIAS their base material's shell tables** — naive frees
  are a use-after-free (hit during the editor rebuild work).
- It inflates material counts through the seam census and amplitude audits.

**Where.** Store the sign in the `Face_*` flag bits (a free bit, not a new
field). Do NOT put it per-vertex: `Vertex` is pack(1) 140 B and the XFRM profile
measured the transform loop CACHE-LINE-BOUND, so widening it costs ms. Stamp at
the determinant computation in `GreetsFixBitangentHandedness`; consumers are
`Mekalele.h` (march), `DeferredSurfaceKernel.cpp` (×5), `LightmapBake.cpp`.
The deferred kernel needs the bit reaching per-pixel — that is the hard part and
the reason the split exists; check whether a G-buffer bit is available or
whether the material lookup can carry it without a clone.

**Sequencing.** AFTER the immediate march fix (thread `Material::TbnHandedness`
into the raster context) — that unblocks the swim and is small. This entry is
the follow-up that fixes the CLASS rather than the instance.

**Expected benefit.** Removes a whole defect class; no measured perf claim.

## Offscreen geometry — what is LEFT after the faceless-mesh skip (2026-08-06)

Context: `docs/VISIBILITY_PLAN.md` §9. The faceless retired-Piramid skip
(`799c808`) took SHADOW 540 706 → 175 594 verts/frame at t=5743 and is DONE.
Ranked by share of what remains, measured with `--xfrm_pass_mesh_prof`:

- **`Hull.lwo` (the robot) — 82 800 verts/frame, 47.2 % of the post-fix shadow
  front end.** TODO. 2 400 faces / 7 200 verts, transformed in ~11.5 of 29
  shadow calls per frame. Levers: a shadow-caster LOD, or tightening the
  per-light mesh cull so fewer of the 29 calls keep it. Both are look-neutral
  inside a shadow map (silhouette-only consumer), so this is a perf call, not a
  look call — unlike the wall proxy below. **Measure before building:** the
  whole SHADOW front end is ~10.7 core-ms/frame post-fix, so the ceiling here is
  ~5 core-ms, i.e. sub-ms of wall clock at the pool's speedup.
- **`Piramid.lwo:cNN` chunks — 80 635 verts/frame, 45.9 %.** PARKED. Already
  chunked; VISIBILITY_PLAN §7b measured finer chunking a net LOSS.
- **robot legs — 11 403 verts/frame, 6.5 %.** TODO-if-#1-pays (same lever).
- **~~wall casters (flat proxy for `rooms`/`floor`/`siling`)~~ — PARKED, MEASURED
  NOT WORTH IT.** `rooms` + `rooms::mirUV` are 2 506 of 540 706 shadow verts
  (0.46 %); with `floor*` and `siling` ~4 000 (0.74 %), = 2.3 % post-fix.
  `--greets_shadow_proxy` was sized against ~81 k **displaced** faces; with
  tessellation retired the shadow bake already rasterises the ~226-face flat
  surface the proxy would substitute, so that win is already banked. Do not
  re-propose without a new measurement.

**Separate LOOK call, not actioned:** mirror clones are hidden inside the
mirror RTT scope but **not** inside the env/SH probe bakes. Re-measured
2026-08-06 after the orphan-clone-vertex compaction (`964bf1d`,
VISIBILITY_PLAN §10): at t=5743 the four clones are **49 390 of 200 464
OFFSCREEN verts/frame (24.6 %)** in the shipping arm (was 68 540 of 219 614 =
31.2 % before the compaction) and **48 804 of 2 407 892 (2.0 %)** under
`--greets_displace`. Whether a probe should see a reflection at all is the
user's call; the machinery already exists — `g_envBakeSkipMirrorClones` /
`EnvBake_IsMirrorCloneObj` at `Transform.cpp:1481` structurally excludes
clones from probe bakes, but it is gated on **`--env_bake_fix`, which defaults
0**, so today's shipping probes DO see the reflections. Turning that on is the
switch; the saving is the 49 390 verts/frame above.

## Mirror clone — what is left after the orphan compaction (2026-08-06)

Context: `docs/VISIBILITY_PLAN.md` §10. `964bf1d` clones only the vertices a
surviving clone face references: displaced-arm MAIN 545 339 → 299 449
verts/frame (−45.1 %), shipping-arm mirror-panel poses 54 272 → 28 782
(−47.0 %, XFRM 0.304 → 0.189 ms), byte-identical at 21 gates in both arms.
Ranked leftovers:

- **~~`--mirror_clone_tight_bsphere` → default ON~~ — PARKED, MEASURED ZERO
  WIN.** Landed default-OFF in `964bf1d`. It is the *correct* sphere (a clone
  cannot draw a vertex it does not carry) and it measured byte-identical at all
  21 gates in both arms — but byte-identical **because it culls nothing**:
  main-view transformed verts are the same with it ON and OFF at all six
  (arm × pose) pairs measured. Even the correct tight sphere over a whole
  compacted clone is 0.0 % frustum-cullable at every pose. Keep the flag for
  whoever revisits a split; do not flip it.
- **Bound the OPAQUE clone raster by the mirror window.** TODO, and the
  remaining half of the user's original question. The TRANSPARENT path already
  does exactly this (`RENDER.CPP` ~936–990: a clone batch's bound is its
  mirror's stamped `gb.mirrorId` window, not the clone geometry's projection);
  the opaque path rasterises clone faces over their full projection and
  rejects them per pixel. RNDR was 7.19 of the clone's 11.40 ms in the
  pre-`1a91ed5` displaced arm. Needs no clone split. NOT measured post-`964bf1d`.
- **~~Spatial split of the clone (VISIBILITY_PLAN §8e)~~ — DE-SCOPED, MEASURED
  NOT WORTH IT.** Post-compaction the clone is ~27 k verts at the wall pose and
  **1 926** at the mirror-panel poses, against a 0.19–0.44 ms main-view pass, so
  §8b's "~2 ms" is void. Re-measured ceiling: at the panel poses a cell = 8
  split culls 98–100 % **of 1 926 verts** (~0.01 ms); at the wall pose the best
  granularity is per-source-mesh at 10 260 verts (~0.05 ms) and the spatial
  cells §8e specced are the *worse* of the two there (25.7 %). The margin cells
  used to hold was the orphan block, twice removed (`799c808`, `964bf1d`). Do
  not re-propose without a new measurement.

## Three architecture candidates, decided on measurement (2026-08-08)

Measured with `--deferred_prof` (`docs/PERF_STATE.md` §0). Read that section for the
tables; this is the disposition.

- **(C) make the filtered-albedo plane (`--texture_filter>0`) the default — KILLED.**
  The premise was that the G-buffer stores a texel ADDRESS, so the kernel pays a
  dependent random gather per pixel that a linear filtered plane would remove.
  Measured at 5 poses × 3 interleaved reps: the kernel is **not faster at any pose**
  (`lighting-w1` 27.32→27.29→27.53 at greets 5743; same flat result at greets 2000 /
  4200, city, fountain), while the raster pass pays a consistent **+1.2–1.4 ms** for
  the extra plane. Frame cost +0.9…+4.0 ms. Proven directly by `--prof_no_tex`:
  deleting the albedo gather outright changes the kernel by +0.9 % — there is no
  gather cost to recover, because the normal / metal / roughness / AO / horizon maps
  are still fetched at the same `Mipmap[mip][swizzledUV]` address. `--texture_filter`
  stays a quality flag. **Do not re-propose as a perf lever without new evidence
  against the `--prof_no_tex` row.**
- **(B) clustered / finer light assignment — NOT SUPPORTED as stated, but the light
  loop IS the elephant.** The omni loop is **20.9 ms = 45 % of the greets frame**
  (`--prof_no_lights`), the single largest slice. But finer binning attacks light
  COUNT, and count is not what sets the cost: the per-tile census (12×8 grid) gives
  greets **6.9 lights/tile → 20.9 ms** and city **26.1 lights/tile → 3.4 ms**. 3.8×
  more lights for 1/6 the cost. The difference is per-light WORK (greets' pixels are
  normal-mapped → scalar kernel path with cube-shadow taps; city's take the 8-wide vec
  path). Corroborating: `--cone_fine_tiles` on city — the same "finer tiles" idea on
  the pass that owns 44 % of that frame — measured **no gain** (30.87 vs 30.97 ms,
  3 reps each). Attack per-light cost, not lights-per-tile.
- **(A) material binning inside the tile — the honest ceiling is ~8.6 ms.** Binning a
  tile's pixels by matID would hoist the `matID → Material*` resolve and the
  has-normal-map / has-AO / has-roughness branches out of the inner loop. That work is
  the NON-light part of the shading wave, measured at **8.61 ms** (`--prof_no_lights`,
  greets 5743) out of a 46.4 ms frame — and binning would recover a fraction of it,
  not all of it. Not refuted, but it is an 18 %-of-frame target where (B)'s territory
  is 45 %, and the (C) result says the coherent-gather half of the argument is worth
  ~0.

**What the numbers actually point at, in order:** the per-light cube-shadow/PBR loop
on greets (20.6 ms, split 10.8 ms shadow sampling / 9.8 ms per-light shading math),
the volumetric cone pass on city (30.7 ms = 44 %, and not a tile-balance problem), and
`TBR_Render` on fountain (14.8 ms = 73 %). None of the three candidates on the table
addresses any of those three directly.

**The one measurement that would settle (B), and does not exist:** a per-PIXEL count
of lights that survive the range/cone test inside the kernel loop. The census counts
per TILE. If most of a tile's 6.9 lights are rejected per pixel, finer binning removes
the rejects and (B) is worth its 20.6 ms target; if most survive, finer binning
removes nothing and the lever is per-light cost (cheaper shadow tap, fewer shadowed
omnis). Cheap to add next to `FDS_TILE_LIGHT_PROF` in `DeferredSurfaceKernel.cpp`.

Also newly visible and previously unattributed (all `docs/PERF_STATE.md` §0):
- **city `cones-call` 30.7 ms = 44 % of the city frame** — the largest single phase
  in any scene measured, and nobody was looking at it. `--cone_fine_tiles` does not
  help (measured). Untouched question: what the cone pass actually costs per spot.
- **fountain `TBR_Render` 14.8 ms = 73 %** — the fountain is a TBR-bound frame, not a
  lighting-bound one (its whole deferred lighting is 1.7 ms).
- **the bloom chain (DoF + bright pass + anamorphic + bloom + lens ghosts) is
  ~1.8 ms/frame on greets** and sat OUTSIDE every existing timer until now.
- **`mirror-grid` 0.68 ms/frame** — a full-res scalar scan of the mirrorMask plane in
  the lighting setup, every frame, on every scene that has a mirror mask.
- **the mirror clone costs ~3.4 ms of G-buffer raster** on greets t=5743 (5.69 ms with
  `--greets_mirror`, 2.30 without).

## Static shadow lightmap: the atlas is sized by FACE COUNT, not area (2026-08-09)

`StaticShadowLightmap::data` is `numFaces * lmRes² * numOmnis` **bytes**, and
`allocate()` fills it with 255 — so every byte is touched, resident and counted.
greets sets `shadow_lightmap_res = 128` (`GREETS.CPP` `GreetsApplyInitDefaults`),
which is calibrated for its authored wall quads and scales with the wrong
quantity the moment anything is tessellated.

**MEASURED** (greets `t=5743`, `/usr/bin/time -l` peak footprint, 64 GB box):

| arm | baked faces | atlas store | peak footprint | bake |
|---|--:|--:|--:|--:|
| flat | 33 396 | 5.61 GB | 6.93 GB | 1.08 s |
| `--greets_displace`, before | 115 346 | 19.36 GB | 22.97 GB | 6.2–11.7 s |
| `--greets_displace`, after | 115 346 | **0.14 GB** | **2.35 GB** | **0.09 s** |

- **DONE for the displaced arm** — `--shadow_lightmap_texel_density` (default 0 =
  OFF = byte-null), defaulted to 14.2 texels/world-unit by `--greets_displace` as
  its third perf companion. Per-mesh res = `clamp(ceil(sqrt(meanFaceArea) *
  density), 8, shadow_lightmap_res)`; capped, so it can only reduce. Look cost in
  the displaced arm: **byte-identical at t=1588 / 2845 / 4871 / 6097, and 3 px at
  1 LSB at t=5743** — the 19.2 GB it removes was buying nothing.
- **DONE for the FLAT (shipping) arm too — 2026-08-09, at the user's
  instruction.** `setDefault(shadow_lightmap_texel_density, 14.2)` moved out of
  the `--greets_displace` branch and into the main `GreetsApplyInitDefaults`
  block, so both arms get it. **347 of the 370 baked meshes** fall under the 128
  cap (mean face edge 1.303 world → res 19); the 23 that keep the cap are the
  big authored quads. MEASURED on the flat arm at `t=5743`:

  | | legacy (`…density=0`) | default 14.2 |
  |---|--:|--:|
  | atlas store | 5.61 GB | **0.09 GB** |
  | peak footprint (`/usr/bin/time -l`) | 7.44 GB | **1.50 GB** |
  | static bake (min-of-9 interleaved, load 11–17) | 1104 ms | **54 ms** |
  | greets-entry join wait (load 31) | 3497 ms | **221 ms** |
  | frame ms `t=5743` / `t=5780` (min-of-6, HEAD `af1f8f8`, load 3.2–7.9) | 49.17 / 48.70 | 49.33 / 48.84 |

  Per-frame is **neutral** (+0.15 ms both poses, inside the spread). An earlier
  batch at load 11–30 read −1.76 ms at `t=5780`; that was noise — the quiet-box
  re-measure above supersedes it. The bake and the memory are the real wins.

  **LOOK: NULL, and measured as such** — byte-identical at all 16 poses of
  `docs/greets_review_poses.txt` and at the pin pose, so the greets pin
  `778fa6ac…` did **not** move (4/4); city `3cbe42b1…` and fountain `8db68ccb…`
  4/4 each; `render_gate` 3/3. Revert flag `--shadow_lightmap_texel_density=0`
  reproduces the pin 4/4.
- **NEW, and bigger than the above: the shipping greets arm BAKES the atlas AND
  NEVER READS IT.** `DeferredSurfaceKernel.cpp:1619`
  `lmKernelEnabled = !shadow_dynamic() || shadow_lm_dynamic()`; greets defaults
  `shadow_dynamic` ON and `shadow_lm_dynamic` is compile-default 0, so every
  pixel takes the cube tap. MEASURED: `--no-shadow_lightmap` renders
  **byte-identical** frames at t=5743 and t=6097, and forcing the atlas live
  with `--shadow_lm_dynamic` is byte-identical to the shipping frame as well.
  So the remaining 0.09 GB + 54 ms bake is *still* pure waste on the default
  path — the whole `LightmapBake_Static` call could be skipped when
  `shadow_dynamic && !shadow_lm_dynamic`. Not done here: it is an FDS/RENDER
  change, and someone may want `--shadow_lm_dynamic` to become the default
  instead (which is the opposite fix, and a look call).
- **CORRECTION, 2026-08-10 — "forcing the atlas live with `--shadow_lm_dynamic`"
  ABOVE DID NOT FORCE ANYTHING LIVE.** That sentence read a null result as
  "the lightmap gives the same answer as the cube tap". It does not: the flag
  is **inert on greets**, because there is a SECOND gate it does not touch.
  `resolvePixelLightmap` (`DeferredShadowSampling.h:52`) needs `gb.lightmapMF`,
  which only `EngineGBuffer_Resize` (`Mekalele.cpp:85`) allocates, under
  `shadow_lightmap()` — and greets sets that flag in `GreetsApplyRunDefaults`
  (`GREETS.CPP:1228`, at `createGreetsScene` `:4393`), i.e. **after** every
  resize call site (`Snapshot.cpp:153`, `SDL2.cpp:433`, `ReproHarness.cpp:130`).
  Planes never exist → `pl.lm` is null on every pixel → `shadow_lm_dynamic`
  cannot matter. Same defect class as `mirror_rtt`, fixed in `7953bab`.
  **Positive control (t=5743, one binary):** `--shadow_lm_dynamic` **0 px**;
  `--shadow_lm_dynamic --shadow_lightmap_texel_density=1` (atlas crippled)
  **0 px** — a crippled atlas changing nothing is the proof it is unread;
  `--shadow_lightmap --shadow_lm_dynamic` **868 274 px (41.87 %)**.
  So the bake-skip above is even more clearly right, and it is now known what
  the opposite fix would cost: with both gates open, **+1.7 ms/frame**
  (min-of-6 interleaved at t=5743/5780/5814, load 9.8–11.6) for a change that
  is **95 % one-LSB** and invisible. Also note `shadow_lightmap` is read by
  nothing after init — it is an allocation-time flag, not the per-pixel sample
  gate `GREETS.CPP:1112-1117` claims it is. Full write-up + evidence:
  `docs/SESSION_STATE.md` 2026-08-10 and `docs/img/fogwt/lmdyn_*`.
- **Bake-resolution question, settled by measurement (2026-08-10):** a richer
  bake does **not** improve the lightmap path. Cap+density swept together to
  256/28.4 (0.32 GB, 191 ms bake) and 512/56.8 (**1.28 GB, 670 ms**): the
  >12/255 population falls only 8–26 % and the max channel delta moves by 0–1
  (138→138, 70→69, 96→96). The residual is not spatial — **inferred**: 8-bit
  quantisation of the shadow factor plus double filtering (4-tap PCF at bake,
  quantised, then bilinear at sample) vs the runtime's PCF at the pixel's own
  world position. Picture: `docs/img/fogwt/lmdyn_bakeres_t5773.png`.
- **DONE 2026-08-10 — the bake is now SKIPPED when the atlas has no reader.**
  `Initialize_Greets` evaluates `FeatureFlags::shadow_lightmap()` at the bake
  spawn point (where only CLI/env can have set it, since `GreetsApplyRunDefaults`
  has not run — so it is exactly the value `EngineGBuffer_Resize` already saw at
  boot) and, when it is off, skips `LightmapBake_Static` **and** the atlas
  allocation entirely. Keyed on the plane-allocation gate alone, deliberately:
  it is the conservative half, so `--shadow_lightmap` still bakes and the
  `--shadow_lightmap --shadow_lm_dynamic` arm is bit-for-bit unchanged (verified,
  matched pair: `fe61ae5c673fb56907eb79d071a7bfb6` both arms).
  **MEASURED:** static bake 55.8–78.3 ms → 0.1 ms (the stamp below); greets-entry
  join wait → gone with the thread; `--mem_census` loses the two lightmap rows,
  **−89.21 MiB** of allocated-and-TOUCHED store (censused total 750.94 → 661.73
  MiB, 41 → 39 buffers); process peak `phys_footprint` −20 to −24 MB (smaller
  than the census because `allocate()`'s uniform 255 fill compresses — *inferred*).
  Frame ms neutral (min-of-6 interleaved, load 7.6–12.9: t=5743 51.89 → 51.47,
  t=5780 50.55 → 50.88, both inside a ~7 ms spread).
  Pins unmoved 3/3 each: greets `778fa6ac…`, fountain `8db68ccb…`, city
  `3cbe42b1…`, `render_gate` 3/3 on both arms.
  **THE SIDE EFFECT THAT HAD TO BE KEPT, and it is not hypothetical:**
  `LightmapBake_Static` also stamps `Face::MeshFaceIdx`, whose *second* consumer
  is `tbrXparOrderLess` (`FILLERS.CPP:1876`) — the camera-independent tie-break
  of the per-strip transparent sort, live every frame and nothing to do with
  lightmaps. Split out as `LightmapStampFaceIndices` and still called on the skip
  path. **Proof it is load-bearing:** a control binary with only that call removed
  moves the greets pin to `76ff35370495cbd67c221fb899a7833b` (stable 2/2) — 1 px,
  max channel Δ 24/255, the exact coplanar-tie signature. Every *other* side
  effect (`staticLMTable`, `staticLMMeshId`, `staticShadowLM`, `omniSceneIdx`,
  `coverageBits`, `planarBases`) is reachable only through `resolvePixelLightmap`
  → `resolveCubeAtten`'s `useLightmap && pl.lm` branch, plus two diagnostics
  (`LightmapViz_Available`, the `--mem_census` row) — all dead on the shipping arm.
- **Cheap unrelated win found while there:** `lm.allocate(faces, numCubeOmnis, res)`
  (`LightmapBake.cpp:373`) sizes K from **all 11** cube omnis, but the bake
  `continue`s on any omni lacking `Omni_StaticShadow` (`:487`) and the kernel's
  `cubeOmniStatic` gate can never read those slots. greets has 8 static + 3
  moving → **3/11 = 27 % of the atlas is allocated, touched at 255, never
  written and never read.** Only worth doing if the lightmap path is ever used.
- Stale comment left behind, worth a one-line fix by whoever next touches the
  file: `FDS/RENDER/LightmapBake.cpp:330-336` still says greets turns the
  density on "only under `--greets_displace`… so the byte-pinned FLAT path never
  takes this branch at all". Both halves are now false.
- Same for city/fountain if they ever raise `shadow_lightmap_res` off its
  default of 16.
- Related: the atlas is sampled per pixel by the deferred kernel
  (`DeferredShadowSampling.h` `resolvePixelLightmap` → `sampleBilinear`), so its
  size is a per-frame cache/TLB question as well as a startup one. On a 64 GB box
  with the array resident the per-frame difference at t=5743 measured **within
  noise** (min-of-6: 55.07 ms before vs 54.82 after); the cost that IS certain is
  the startup bake and the 20 GB of resident pages every other process has to
  live around.

## 2026-08-10 — HARDWARE COUNTERS: THE LIGHTING STAGE IS COMPUTE-BOUND, NOT MEMORY-BOUND

Instrument: `--hw_prof` (adds task-wide retired-instruction / core-cycle / IPC
columns to the `--deferred_prof` table). Method, boundaries and the IPC
calibration ladder for this box: **docs/HW_PROFILING.md**. Cache-miss counters
are unreachable without root on Apple silicon, so IPC is the stall proxy
throughout — sound for before/after on one kernel, not an absolute score across
different code.

Reference anchors measured on this M2 Max: compute-bound (no memory traffic)
IPC 2.42; L1-latency chase 0.96; L2 0.14; DRAM 0.01.

### 1. The packed shadow plane (af1f8f8) — **CONFIRMED, and the mechanism is now measured**

`af1f8f8` claimed −1.0/−1.2 ms/frame from collapsing 4 u16 shadow planes into 2
u32 (8 cache lines per PolyId tap → 4). It shipped on a wall-clock argument. The
counters were re-run against it: matched pair, both arms built from ONE tree with
the same instrumentation applied, run from ONE `Runtime/` (af1f8f8 touches no
asset), greets t=5743, 1920×1080, dummy drivers, iters=20, **8 interleaved ABBA
rounds at load 13.7–18.8**, wall = min over rounds, counters = median.

| phase | wall A→B | Ginstr/f | Gcyc/f | IPC |
|---|--:|--:|--:|--:|
| **lighting-w1** | 27.72 → **26.53 ms (−4.3 %)** | 3.4965 → 3.4360 (**−1.7 %**) | 0.9875 → 0.9125 (**−7.6 %**) | 3.543 → 3.768 (**+6.3 %**) |
| DeferredLighting-call | 32.71 → 31.52 (−3.6 %) | −1.5 % | −7.6 % | +6.5 % |
| renderFrame | 45.35 → 44.19 (−2.6 %) | −1.1 % | −6.5 % | +5.8 % |
| shadow-bake | 2.41 → 2.35 (−2.7 %) | −0.5 % | −1.5 % | +0.7 % |
| gbuffer *(control)* | 6.70 → 6.77 (+1.1 %) | **±0.0 %** | −0.5 % | +0.5 % |
| lighting-w2 / cones / bloom / tonemap / TBR *(controls)* | — | ≤ +0.8 % | ≤ +3.6 % | \|Δ\| ≤ 0.7 % |

The −1.19 ms lands exactly on the claim. **The counters add what wall clock could
not: cycles fell 4.5× harder than instructions did** (−7.6 % vs −1.7 %). The work
did not change; the machine stopped waiting for it. That is a memory-side win by
construction, and it is confined to the two stages that tap the shadow map —
every control phase moved under 1 % in IPC, and `gbuffer` retired a byte-identical
3 significant figures of instructions in both arms. IPC sample sets barely
overlap (A 3.164–3.691, B 3.354–3.806).

### 2. The `Material` hot-record — **PARKED, refuted with numbers (bound ≲0.1 ms)**

The parked item above proposed a 64-byte L1-resident hot record per matID to
collapse the deferred kernel's 5-lines-per-pixel `Material` walk, and flagged
itself "latency not bandwidth, likely less than byte count suggests". Measured at
greets t=5743, lighting-w1 (post-pack arm):

* **1 657 retired instructions per pixel**, **440 core-cycles per pixel**
  (3.436 G instr and 0.9125 G cycles over 1920×1080), **IPC 3.77**.
* An IPC of 3.77 is incompatible with domination by serialised loads — the L2
  chase anchor on this box is IPC 0.14. The ROB is not starved.
* Arithmetic bound: the `Material` table is ≤ 114 KB, so a miss is an L2 hit at
  **30.6 ns ≈ 87 cycles** (measured). Six serialised L2 round-trips per pixel
  would be **~525 cycles/px — more than the entire measured 440-cycle pixel
  budget**. The walk therefore cannot be missing; those lines are being hit and
  overlapped.
* Upside ceiling: the record removes ~5 loads of the 1 657 instructions/px =
  **0.30 % of instructions**, so at unchanged IPC **≲0.1 ms/frame** of the
  26.5 ms wave.

**Verdict: not worth the invasiveness.** Caveat, stated plainly: this is a bound
from aggregate counters plus a latency anchor, *not* an isolation experiment — no
build was made with the walk removed. It rules out the large win the byte count
suggested; it does not measure the small one.

### 3. Where the greets lighting stage actually goes — **compute-bound; the lever is instructions/pixel**

lighting-w1 IPC **3.77**, against a compute anchor of 2.42 and an L1-latency
anchor of 0.96. The stage is issue/throughput-limited, not stalled. `gbuf-clear`
in the same table reads IPC **0.555** on both greets and city — the one phase
that genuinely is bandwidth-bound, which is a useful check that the instrument
discriminates rather than reporting ~3.5 everywhere.

`sample(1)` self-time over the same workload, DEMO symbols only (~50 090 samples
total; ~38 788 land in the lighting stage):

| symbol | samples | share of lighting stage |
|---|--:|--:|
| `Render_DeferredLighting_Tile` | 18 493 | 48 % |
| `CubeShadow_Sample` | 8 695 | **22 %** |
| `Render_DeferredLighting_TileFill` | 3 547 | 9 % |
| `resolveCubeAtten` | 2 898 | 7 % |
| `computeMapShadowAtten` | 2 518 | 6 % |
| `Shadow_MaterialSkipsCasting` | 1 479 | 4 % |

**Shadow sampling is ~40 % of the lighting stage** (15 590 samples across the
four shadow symbols) — the largest single sub-cost, and the same place af1f8f8
already took 7.6 % of cycles out of. That is where the remaining headroom is, and
it should be attacked as *instruction count* (fewer taps, cheaper tap, wider
SIMD), not as cache layout: at IPC 3.77 there are no stalls left to reclaim.

**TODO — new, actionable:** `Shadow_MaterialSkipsCasting` (Shadows.cpp:186) is
called **per pixel** from DeferredSurfaceKernel.cpp:1775 under
`--shadow_noncaster_depth`, and costs 4 % of the lighting stage. Its result
depends only on `Mat` — a material property, constant for every pixel of a given
matID — yet each call hashes the pointer and does a relaxed atomic load on a
shared 256-entry table. Hoist it to a per-tile lookup keyed by matID (the tile
kernel already hoists ~50 FeatureFlags reads out of the pixel loop for exactly
this reason). Expected ~0.5–1 ms/frame at greets t=5743. Not yet attempted.

### 4. city t=1961 — **the frame is volumetric cones, not lighting**

Same instrument, city t=1961, load 21.7 (ms inflated by load; the counter columns
and the *shape* are what to read). `renderFrame` runs **2×/frame** on city.

| phase | wall_min | Ginstr/f | IPC |
|---|--:|--:|--:|
| renderFrame (×2) | 70.47 | 8.344 | 3.72 |
| **cones-call** (×1) | **30.50 (43 % of frame)** | **4.150 (50 % of ALL instructions)** | 3.95 |
| gbuffer | 10.31 | 0.891 | 3.04 |
| DeferredLighting-call | 10.43 | 1.243 | 4.12 |
| fastfog | 9.25 | 1.093 | 3.62 |
| TBR-render | 7.11 | 0.847 | 3.54 |

**Anomaly worth flagging:** city's profile is nothing like greets'. Half of every
instruction the city frame retires is spent in the volumetric cone pass, which
runs once while the rest of the frame runs twice. It is compute-bound (IPC 3.95),
so the lever is again instruction count — cone count, march steps, or early-out —
not memory. Any city optimisation aimed at the lighting kernel is aimed at 15 %
of the frame.

### 5. Allocation audit — two leads, neither yet triaged

`scratchpad/hwprof_alloc.sh` (the CLT stand-in for Instruments' Allocations
template) on greets t=5743, live snapshot 14 s in, load 5.5. Physical footprint
**1.4 GB**; peak RSS 1.51 GB; MALLOC_LARGE 956 MB + MALLOC_SMALL 515 MB.
Biggest attributed live stacks:

| bytes | calls | stack |
|--:|--:|---|
| 117.5 MB | 5 | `Initialize_Greets` → `MaterialImport_ApplyRevMaps` → `loadRoleMapCached` → `Load_Texture` → `LoadPNG` |
| 99.4 MB | 370 | `Initialize_Greets::$_7` thread → `LightmapBake_Static` |

**LEAD, not a claim:** `leaks` reports **9 383 leaks / 387.9 MB** unreferenced at
the snapshot. C++ interior pointers and pointer-tagging (e.g. the packed
`Material*|skip-bit` cache in `Shadow_MaterialSkipsCasting`) are classic
false-positive sources for `leaks`, so this needs triage before it means
anything — but 388 MB against a 1.4 GB footprint is too large to leave
unexamined. Cross-read it with `--mem_census`'s UNCENSUSED RESIDUAL line: if the
census accounts for everything and `leaks` still says 388 MB, it is false
positives; if the residual is the same order, it is real.

### Method note that generalises

During this work the box went from load 13 to load 33 mid-session. Across that,
greets `renderFrame` **wall_min inflated 45.3 → 96.4 ms (+113 %)** while
**Ginstr/f moved 5.251 → 5.257 (+0.1 %)**. Retired-instruction count is very
nearly load-invariant, and IPC nearly so. On a shared machine, prefer them; quote
ms only with the load that produced it.

## How this list is maintained
Add an entry the moment an optimization is deferred (with: what, why deferred,
where in code, expected cost/benefit). Mark DONE with the commit + measured
result. SESSION_STATE "Queued next" points here; the memory
`optimization-backlog` points here too.
