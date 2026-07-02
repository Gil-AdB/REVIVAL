# Architecture deep-dive — what's left after the 2026-06/07 perf campaigns

Written 2026-07-01 on `feature/soa-vertex`, after the greets campaign that shipped
deferred-quarter + texture/lighting decouple (C), the bright-pass consolidation, the
edge-AA pass, and three **measured-negative** memory-layout experiments. This doc is
the strategic map: what is closed (with evidence), what is open (with grounded
estimates), and the recommended order.

Reference numbers (greets, user's full config, 1512×848, quarter): frame ≈ 47 ms full
config / ≈ 27 ms with post-FX+SSAO off. City full-rate ≈ 13 ms.

---

## 1. Measured frame anatomy (where the time actually is)

| bucket | ms | evidence |
|---|--:|---|
| Post-FX stack (SSAO 5.8, DoF 4.6, ghosts ~2, anamorphic 1.7, chromatic 1.0, vignette 0.4, AA 0.8) | **~16** | per-pass ablation, this session |
| Shadow bake (DynOmnis ~4.8 + DynMeshes ~2.0, 76 maps, transform-bound) | **~7** | `--shadow_bake_time` |
| Deferred lighting kernel (quarter) | ~2-3 | `_MergedGlobals` ≈ 1 sample in full-config profile |
| G-buffer raster + transforms + mirror RTT + shards | ~10 (worker-overlapped) | `Transform_Objects` 1244 samples but mirror-rtt ablation only −1.8 ms wall |
| Serial tick (Animate, Radix, orchestration) | the 24-28% pool-idle Amdahl fraction | RENDER_DAG_SCOPING.md phase-1 instrumentation |

## 2. Closed doors — do not reopen without new hardware/workload

| door | verdict | where |
|---|---|---|
| Shadow-map tiling/swizzle (all shapes 2x2…8x8) | **negative**, linear won all 15 measurements | SHADOWMAP_TILING_PLAN.md |
| PBR AoS 8-byte interleave (albedo+nmap+rough+AO) | ceiling ≤0.5 ms by ablation bound — can't clear its unpack cost | ablation 2026-07-01 |
| Albedo fetch cost | ≤0.3 ms (`prof_no_tex`) | this session |
| Shadow ∥ G-buffer overlap (Stage A) | **built, net-negative** — both saturate the pool | THREADING_OVERLAP_PLAN.md |
| DAG wave fusion (lighting ∥ cones accumBuf) | **do not build** — waves already at effPar 12-13/12 | RENDER_DAG_SCOPING.md |
| Inner-vec (8-omni) SIMD light loop on arm64 | neutral; default stays scalar (x86 default ON, unmeasured) | this session |
| Quarter-rate on SIMD-friendly scenes (city) | 2.5× SLOWER (sparse pattern breaks 8-wide → scalar libm) | profile 2026-06-30 |
| BRDF cost (Blinn-Phong vs GGX) | free — light loop isn't the bottleneck | `--pbr` A/B |

The through-line: **the engine is not read-latency-bound and its parallel waves are
already balanced.** Remaining wins are (a) bandwidth of the serial post chain,
(b) algorithmic work reduction in the bake, (c) the Amdahl serial fraction,
(d) resolution/rate cuts (quality tradeoffs).

---

## 3. Open doors, ranked

### 3.1 f16 HDR buffer (halve the post-stack's bandwidth) — EST −2…4 ms, medium effort
`g_hdrBuf` is f32×4 = 16 B/px ≈ 20.5 MB at 1512×848. The post stack streams it
~10-14× per frame (12 `hdrDispatchRows` sites in Hdr.cpp alone + SSAO + froxel
composite + tonemap) ≈ 250+ MB/frame of pure streaming. Converting storage to f16×4
(8 B/px; NEON has 1-cycle f16↔f32 converts, `vcvt_f16_f32`) halves that mechanically
across EVERY pass, including future ones. Radiance range fits f16 easily (max 65504;
values here are 0..~10⁴). Alpha/coverage flag can stay a u8 plane or f16.
- **Effort:** broad but mechanical — every `g_hdrBuf.data()+i*4` reader/writer converts
  at the register boundary. ~15 files touch it.
- **Risk:** precision (10-bit mantissa) — bloom thresholds and additive accumulation
  round differently. NOT byte-identical; validate with SSIM + eyeball, and keep a
  compile-time `HdrTexel` typedef so f32 remains one `#define` away.
- **Why it ranks #1:** it's the only lever that cuts the *whole* 16 ms bucket
  proportionally without any per-effect quality decision.

### 3.2 ~~Cube-face shared transform~~ → SUPERSEDED 2026-07-02: adaptive tile grid (SHIPPED, −2.3 ms)
Measurement redirected this one. The per-mesh "Supermatrix" already folds the
projection constants into the same per-vertex FMAs (no separate transform stage
to amortize), and with face-culling each mesh only visits ~1.5-2 faces — so the
shared-transform ceiling was ~0.3-0.5 ms of a 1.5 ms xform phase. The REAL bake
cost was the Phase-B raster: a fixed 4×4 grid (tuned for 512²) made every tile
task re-walk the light's whole FList through the clipper — 16 clip-walks/light
at 256². Shipped: per-light grid = res/128 clamped [1,4] (commit 2168980).
Bake 7.55→4.77 ms, frame −2.3 ms. Also fixed --shadow_prof per-mode accounting.
Residual bake ideas if ever needed: per-omni single mesh-walk (share the 6
face-cull tests), world-bsphere caching per frame.

### (original 3.2 analysis, kept for the record) — cube-face shared transform
The bake is transform-bound (halving raster res only saved ~0.8 of ~5 ms). Today each
of an omni's 6 faces runs a full `Transform_Objects` (world→view 3×3 matmul + project
per vertex) = 76 transform passes/frame. But the cube-face cameras are **world-axis-
aligned** (verified: fixed ±X/±Y/±Z from `O->IPos`), so face view-space coords are a
**signed permutation** of `(world − lightPos)`:
- Per omni: compute `w = worldVert − lightPos` ONCE (subtract only).
- Per face: view = permute/negate w's components (exact float ops — byte-identical
  gate is achievable), then project.
This collapses 6 matmul passes to 1 subtract + 6 permutes. Cube omnis are ~72 of the
76 maps. Per-face caster culling stays as-is.
- **Effort:** a dedicated cube-bake transform path in Transform.cpp (or a new slim
  "position-only shadow xform"); the generic path remains for spots.
- **Risk:** medium — Transform.cpp is shared hot code, but the change is compute-path
  (byte-gateable), not threading. Validate: byte-identical shadows + STEADY-count.

### 3.3 Pointwise post-FX fusion — EST −1…1.5 ms, low effort/risk
Tonemap, chromatic, vignette (and grade/grain when used) are each a full-buffer
pointwise stream. Fuse into ONE pass (tonemap already reads hdr + writes VPage; do
chromatic's channel-shifted taps + vignette's radial multiply in the same loop).
Saves 2-3 full-buffer round-trips. DoF/bloom/anamorphic keep their own passes
(neighborhood reads). Byte-identical achievable if the math order is preserved.

### 3.4 Half-res DoF with edge-aware upsample — EST −3 ms, quality tradeoff
DoF's 4.6 ms is a full-res gather blur. SSAO already ships the pattern (downscale=2 +
joint-bilateral upsample). Same recipe: compute CoC + gather at half res, upsample
depth-aware. Standard practice; bokeh edges soften slightly. Flag-gated next to
`--ssao-downscale`.

### 3.5 Temporal SSAO — EST −3 ms, medium risk
5.8 ms is memory-bound sampling; the atan2→poly swap proved ALU isn't the issue. The
remaining lever is doing less sampling per frame: reproject last frame's AO (camera-
only reprojection — the scene is mostly static, demo camera is smooth), depth-reject,
accumulate, quarter the per-frame sample count. Needs an AO history buffer + prev
view matrix. Risk: ghosting on the mech/shards (the depth-reject machinery exists in
the bilateral upsample already). 

### 3.6 Frame pipelining (tick N+1 ∥ render N) — the big Amdahl door, LARGE effort/risk
RENDER_DAG_SCOPING's conclusion stands: the 24-28% pool idle is the tick thread's
serial fraction, not reclaimable by fusing balanced waves. The only structural answer
is overlapping frames: run Animate/Transform/Radix/bake-dispatch of frame N+1 while
frame N's pool-heavy lighting/post runs. This is a state-snapshot campaign
(FrameState/RenderContext migration is ~half done), +1 frame latency (fine for a
demo). It's the only door that changes the frame-time *ceiling* rather than shaving
a bucket. Do it last, or never — the per-bucket levers above total more.

### 3.7 Available now, zero code (config)
- `--greets-moving-omni-shadow-res=128` — measured −1 ms (DynOmnis 5.5→4.8).
- Post-FX trims: ghosts (−2), chromatic+vignette (−1.4), anamorphic (−1.7) if the
  visuals allow. User decision.
- SSAO `--ssao-downscale=3`.

## 4. Recommended sequence

1. **3.3 pointwise fusion** (low risk, quick, byte-identical) →
2. **3.2 cube-face shared transform** (byte-identical, real bake win) →
3. **3.1 f16 HDR** (biggest single lever, needs visual validation) →
4. **3.4 half-res DoF** (flag-gated quality knob) →
5. **3.5 temporal SSAO** (if AO cost still matters after f16) →
6. **3.6 pipelining** only if the demo needs a step-change (e.g. 4K).

Expected total from 1-4: **roughly −7…10 ms on the user's full config** (47→~38 ms)
with only 3.4 carrying a visible quality tradeoff — before any config trims.

## 2026-07-02 — measurement-audit round: 3 instrumentation bugs, corrected anatomy

Audit found a third attribution bug (same class as the --shadow_bake_time /
--shadow_prof shared-accumulator fixes): greets+city entered PROF_RNDR **before**
the shadow bake, folding ~5 ms of bake into the RNDR row everyone reads. Fixed
with a PROF_BAKE section (6680941). One-shot timers (g_ssaoLastMs, g_aaLastMs,
TailProf) audited clean.

Corrected greets anatomy (shadows config, quarter, NO post-FX): frame ~26 ms =
RNDR 19.7 + BAKE 5.1 + LGHT 0.9 + XFRM 0.4. Inside RNDR (TailProf):
lighting-w1 6.7 / lighting-w2 2.6 / cones 2.1 / gbuffer 1.8 / StampMasks 0.3 /
**~5.5 un-instrumented serial** (tonemap, xpar peel, activate, TBR — needs
ScopeTimers before optimizing). lighting-w1 ablation: per-light loop 5.1
(cube taps 2.6 + light math 2.5), base 1.2, nmap 0.65.

New ranked targets from the corrected data:

1. **Lightmap × dynamic-cube composite** — `lmKernelEnabled = !shadow_dynamic()`
   (DeferredSurfaceKernel.cpp:745): --shadow-dynamic globally disables the static
   lightmap fast path, so every static light pays full cube taps. Machinery for
   the fix exists: CubeShadow_Sample(dynamicOnly=true) + per-map hasDynMeshVisible
   (skip the dynamic tap entirely for maps with no moving-mesh content). Est
   −1.5…2 ms of the 2.6 ms tap bucket. The earlier "kernel is negligible" claim
   was a sample-profile misattribution — retracted.
2. **Per-tile light contribution culling** — light math is 2.5 ms across ~41 view
   lights; FDS_CONTRIB_CULL diagnostic already measures the cullable fraction.
3. **Account the ~5.5 ms serial inside Render-3D** (instrument first).
4. Still standing from §3: f16 HDR buffer, pointwise post-FX fusion, half-res
   DoF, temporal SSAO.

## 2026-07-02 (later) — #1 and #3 resolved, #2 closed by prior art

**#1 lightmap × dynamic composite: MEASURED NEUTRAL-to-NEGATIVE — retracted.**
Built + gated (--shadow_lm_dynamic, default OFF, commit 8ad15ad). Two reasons the
2.6ms tap bucket didn't yield: (a) the mech's bsphere keeps dynBaked true on
~20/48 static faces → those pixels pay lightmap + dynamic tap > one combined
tap (the planar sampler's per-pixel projection costs ≈ a tap); (b) a chunk of
the bucket is MOVING-omni taps that can never use lightmaps. Visuals validated
equivalent. Side benefit kept: ShadowMap::dynBaked fixes stale dynamic
silhouettes (only active when the flag is on).

**#2 per-tile light contribution culling: CLOSED (prior art).** Tried before
this campaign — perf only came with hard per-tile light cutoffs, i.e. visible
inter-tile shading discontinuities. Do not reopen without a smooth-falloff
scheme (e.g. per-tile light fade bands), which erases most of the win.

**#3 serial accounting: CLOSED.** New TailProf scopes decompose the former
~5.5ms gap: DeferredLighting serial setup ~1.2 / xpar-peel ~1.0 / tonemap-post
~0.6 / StampMasks 0.3 / hdr-activate 0.23 / ~2.5 clears+sprite-insert+glue.
Long tail — no hidden monster. Confirms the Amdahl picture.

**Standing open doors after this round:** f16 HDR buffer (§3.1, unchanged),
pointwise post-FX fusion (§3.3), half-res DoF (§3.4), temporal SSAO (§3.5),
frame pipelining (§3.6). The lighting waves (w1 7ms + w2 2.8ms) are now the
largest single bucket; levers there are rate/algorithmic (quarter already on)
— or x86, where the vec path may finally pay.

## 2026-07-02 (evening) — #2 bench-verified dead; f16 HDR SHIPPED (−7 ms)

**#2 contribution culling, bench-verified (FDS_CONTRIB_CULL_THR harness,
3e02226):** a-priori per-tile bound culls 23% of tile-lights at the guaranteed-
invisible threshold — frame min UNCHANGED (24.65 vs 24.68). The provably-dim
lights already early-out on the per-pixel range test; the oracle's other half
(in-range but dim via albedo/dot) is unknowable before shading. Confirmed dead
on both perf and quality grounds.

**#3.3 pointwise fusion: dead for the production config** — chromatic is a
GATHER over the tonemapped image, so Render_LensPostPass must exist whenever
--chromatic is on, and vignette already rides in it. Only pays with chromatic
off (~0.3 ms); not built.

**#3.1 f16 HDR buffer: SHIPPED (44cd690).** hdrf = __fp16 storage on arm64.
Full-config frame min 50.7→43.6 ms (**−7 ms, −14%** — bigger than the −2…4
estimate; effective streaming bandwidth under pool contention is well below
peak, so halving bytes pays double). Visual: maxΔ 3 vs f32 on the full post-FX
snapshot; disco beams + fountain bolts banding-free. FDS_HDR_F32 escape hatch.

Standing doors: half-res DoF, temporal SSAO, frame pipelining, x86 measurements.

## 2026-07-02 (night) — two HDR regressions found by the user, fixed (5f5dfdf)

1. **f16 black tiles**: spec hotspots legitimately reach ~63000 radiance —
   past __fp16 max (65504) → inf → NaN through bright-pass/DoF → black blotches.
   Fix: fds::HdrClamp(60000) at every g_hdrBuf store. New FDS_HDR_SCAN=1
   diagnostic (inf/NaN scan at phase boundaries) pinpointed the source in
   minutes — keep using it for any future HDR anomaly.
2. **Bright-pass sharing (cae9ebf) visibly changed the lens-ghost halo**
   (smooth post-bloom rainbow ring → pre-bloom speckles). The sequential
   passes deliberately compound; the one-frame byte-gate that "validated" the
   consolidation had no halo-visible content. Sharing is now opt-in
   (FDS_SHARED_BP), default per-pass. LESSON for the ledger: a byte-gate on ONE
   frame does not validate an ORDER change — sweep frames with each effect
   visibly exercised, or don't reorder.

Final full-config: 44.5-45.5 ms min (pre-campaign f32 baseline 50.7-51.4).

## 2026-07-02 — third HDR regression: C-fill blowout on high-contrast textures

User report: mech cockpit LED panel "way too bright". Bisected headless (same
FDS_GREETS_CAM pose across commits, panel-crop mean) to 1f8315e (C step 2, HDR).
The divide-out model R_i = R_n·(texel_i/texel_n)^exp explodes when the
neighbour's texel is near-black: max(texel,1) floors the denominator at 1, so
an LED dot (255) over the panel's black background computes ratio 255 —
squared to ×65025 under hdr_linear — amplifying the black texel's residual
spec/ambient radiance. Panel crop: full-rate truth 30.2, C-fill 52.1 (+ cyan
smear). Fix: trust region — a neighbour with any channel ratio > 4 is excluded
from the sharp reconstruction (falls back to the plain radiance average, the
pre-C behaviour). Floor-detail win intact: |quarter−full| 3.09 → 0.31 on the
same frame. The commit's own "byte-identical to full" gate passed because the
verification frame (stone floor) has locally smooth texture ratios — same
lesson as #2 above: gate frames must exercise the failure mode (here, a
high-contrast texture), not just the success mode.

## 2026-07-02 — quarter/checker vs full is FRAME-dependent; reference frame now ts=491

Prior "quarter is neutral on greets" was measured at ts=1700 (shadow-bound).
User designated ts=491 (disco, kernel-bound: RNDR 81% of frame) as the perf
reference. Full config, 1512x848, min over 120 iters, interleaved x2:
quarter 46.5-48.0 ms / checkerboard 47.4-47.5 / full-rate 67.0-68.2. The
whole 20 ms delta is RNDR (58.5 -> 39.1 min); BAKE/ANIM identical. So on
kernel-bound frames quarter is a 30% frame win and checker buys nothing over
it; on shadow-bound frames it's neutral; on city it's still 2.5x slower.
Quote both ts=491 and ts=1700 for anything shifting kernel-vs-bake balance.
