# DOS REVIVAL — Performance Plan (Pentium MMX target)

Target HW: Pentium MMX 200–233 (86Box: Presario 4500, P55C, 32 MB). Measure perf in
**86Box** (cycle/chipset/memory-accurate); iterate/debug in DOSBox (fast, but hides cache).

Grounded in three code reads (2026-07-10): master cached-lighting algorithm, DOS ASM
mapper inventory, DOS wobbler + per-frame pipeline baseline.

---

## Baseline: where the time goes (from the pipeline read)

Per-frame passes in `FDS/SOURCE/RENDER/RENDER.CPP`, hottest first:

1. **Per-pixel FILL** — `Render()` → `DoFace->Filler` (RENDER.CPP:704). Fill-rate bound.
   In **City this runs 2× per frame** (reflection pass CITY.CPP:725-751 + main pass :775-780),
   plus `Run_Distort()` (SHIT.CPP:430) does a full `memcpy` + ~2 read/2 write framebuffer
   passes. City is the worst case by far.
2. **Lighting** — `Lighting()` (RENDER.CPP:515). The only stage that walks the vertex array
   **(2 + numLights)** times: ambient init pass, one full vertex pass *per omni* with an
   `RSQRT`+dot (RENDER.CPP:589-608), a saturation pass, +1 fog pass. O(V×L). Dominant
   vertex-side cost when several lights are active (greets: ~10 omnis × thousands of verts).
3. **Transform_Objects** — (RENDER.CPP:176) 1 vertex walk: 4×3 affine + `1/z` divide/vertex
   (:289-294); +6 mul/vertex `EU/EV` env coords for env-mapped meshes, computed *before* the
   owning face's visibility is known (:382-385).
4. **Radix sort + Animate_Objects** — cheap (pointer/object-list walks, no per-vertex inner loop).

The fillers themselves are already well optimized: SMC texture-base patching on the hot paths
(`BITRUE`/`P_Texture_32`, `TEXTGOUR`/`PG_Texture_32`), perspective-correct affine with a real
`1/z` divide every 16 px. There is **no z-buffer and no swizzled/tiled texture anywhere** — both
would be new code, not switch-ons.

---

## MEASURED BASELINE (greets, 2026-07-10)

RDTSC per-pass profiling wired into the greets loop (GREETS.CPP), on-screen readout + logged:
```
frames=1201  render(fill) 53.9%   light 38.8%   xform 5.6%   sort 0.8%   anim 0.1%   flip 0.8%
```
(DOSBox `cycles=fixed 45000` gave fps=15.5 — NOT real HW; the % split is CPU-independent and is
the usable signal. Real fps comes from the 86Box MMX-200 run via the on-screen readout.)

Confirms the pipeline read: **fill + lighting = 93% of the frame.** Lighting's 38.8% is almost
all the static room → Phase 1 is the highest-ROI attack. `RGBGouraudMMX` now wired (Phase 2 done,
pending visual confirm).

## Phase 0 — Measure first (prerequisite, low effort)

Nothing ships without a before/after on the MMX-200.

- `RunScene()` already has a per-pass readout (`Par1..Par5`, RENDER.CPP:861-896) but **City
  hand-rolls its loop and doesn't use it**. Add the same PIT/`rdtsc`-based per-pass timers to
  City's loop (and Fountain/Greets) — Animate / Transform / Lighting / Sort / Render / Distort —
  averaged over N frames, logged to `Runtime.LOG` and/or shown on-screen.
- Deliverable: per-scene, per-pass ms on 86Box @ MMX-200. Confirms "fill dominates, City 2×".
- Test-run harness: `REVIVAL-DOS-YYYY-MM-DD.iso` (built) → mount as CD, `xcopy D:\ C:\REV\`,
  `dos4gw rev.exe`. (Bump 86Box CPU to 233 for the upper-bound number.)

**Gate:** the numbers pick Phase 3's sub-items. Everything below Phase 2 is measure-driven.

---

## Phase 1 — Incremental / static lighting  ⟵ highest ROI, no ASM, low risk

Port master's static-bake + dynamic-add split (`FDS/RENDER/Lighting.cpp`) into DOS's forward
`Lighting()`. Rationale: most geometry is static under static lights — greets' room (`Piramid`,
3704 verts, the bulk) + its 7 room lights; City's buildings. For a static mesh under static
lights the per-frame light loop should run **zero** times and collapse to a color copy.

Steps (DOS side):
1. Add a per-vertex cache to `TriMesh` (FDS_VARS.H:296) — `float *SLr,*SLg,*SLb` sized `VIndex`
   (or a packed `Color*`), allocated only for stationary meshes.
2. Add two flags (next free bits): `Tri_Stationary = 0x4000` (FDS_DEFS.H), `Omni_Stationary`
   (Flare bit 0x08). Set at load time in the DOS FLD conversion by the simple heuristic
   `NumKeys==1` on Pos/Scale/Rotate (mesh) and Pos/Size/Range (omni) — mirrors native
   FLD_CONV.CPP:272-312 / :444-533 and PREPROC.CPP:618-638.
3. One-time `StaticLighting(Sc)` guarded by a `Scn_StaticLighting` scene flag: run the existing
   DOS omni loop **only** for `Omni_Stationary` lights onto `Tri_Stationary` meshes, plus the
   `Ka` ambient, writing results into `SL[]`. **Use DOS's own falloff term**
   `Color=(Dot*(Kd+Ks*Dot²)*0.5)*rLen²` (RENDER.CPP:602) — NOT native's linear-range formula —
   so the baked look is byte-identical to today.
4. Rework per-frame `Lighting()`: init each vertex from `SL[v]` if `Tri_Stationary` (else `Ka`);
   inside the omni loop `continue` when `(Tri_Stationary && Omni_Stationary)`. Only moving
   lights/meshes hit `RSQRT`+dot.
5. **Fold** the ambient-init and saturation passes into the light loop while here → removes 2 of
   the `(2+numLights)` full vertex walks even for dynamic meshes.

Expected: Lighting pass drops toward ~just-the-mech in greets; big cut in City static geometry.
Risk: low (byte-identical bake by construction; validate with the GCAP capture diff).

Interaction w/ current session's greets work: the mech is dynamic (moving) and its flares are
`Flare_Parented` dynamic → they stay in the per-frame path; room lights + room bake. The
`Flare_Parented` self-exclusion still applies in both bake and dynamic paths.

---

## Phase 2 — Free MMX gouraud fix  ⟵ 1 line, low risk

`RGBGouraudMMX` (GOURMMX.ASM) is fully written but **dead**: `The_MMX_Gouraud`
(FILLERS.CPP:640-661) calls the FPU `RGBGouraud` instead of the MMX one. Point it at
`RGBGouraudMMX_` (FILLERS.CPP:660). Helps opaque **gouraud-lit untextured** faces (pyramid's
flat-color materials, disco/greets light panels). Measure; keep if it wins on MMX-200.

Caveat: `RGBGouraudMMX` reads dynamic `_VESA_BPSL` stride (good) while most fillers hard-code
1280 — fine at 320×240×32, but note if resolution changes.

---

## Phase 3 — Fill-rate reduction (the dominant cost) — measure-driven, pick from:

**3a. City reflection/distort (biggest single lever — City pays fill 2×).**
- The reflection pass renders the *whole* mirrored world full-screen then `Run_Distort` warps it.
  Clip the reflected render + distort to the **water region only** (the reflection is only visible
  on water) instead of full-frame — cuts a large fraction of City's fill + both `Run_Distort`
  framebuffer passes.
- `Run_Distort` (SHIT.CPP:447-457) is pure C: a `memcpy` + per-pixel gather-warp. Fold the warp
  into the blit (drop the `memcpy`), or port the inner loop to the WOBTR-style asm (below).

**3b. Overdraw reduction — coverage/span buffer (new work, MMX-200-gated).**
- Painter's-order today (radix sort) draws every face's pixels even when later occluded. A
  span/coverage buffer skips already-covered pixels front-to-back. Classic DOS win, but per-pixel
  bookkeeping cost — **measure on MMX-200 before committing**; may not pay if overdraw is low.

**3c. WOBTR fast affine mapper as an opaque-fill fast path.**
- `Grid_Texture_MapASM` (IMGGENR/WOBTR.ASM) is a ~4-op/pixel affine gather+store (grid-subsampled,
  no per-pixel perspective divide). For large near-flat opaque textured faces it can beat the
  16-px-perspective `BITRUE`. Caveats: fixed 256×256 tex, 320 stride, color-only (gouraud needs
  the separate 2nd pass the DOSRECOVER wrappers already do). Candidate for ground/wall spans.

---

## Phase 4 — Experimental, only if Phase 0 numbers justify (new work)

- **Z-buffer.** Does not exist; new per-pixel depth read/compare/write. On MMX-200 (small cache,
  in-order) the added memory traffic likely goes **net-negative** vs the current painter's sort —
  opposite of the P3-700 result. Only pursue for genuine interpenetration the sort can't handle,
  and measure both ways.
- **Swizzled/tiled textures.** Reorder texel storage for cache locality. Marginal unless texture
  cache misses are shown to be a real cost (hard to see without 86Box perf counters). Low priority.
- **Transform caching for static meshes.** Camera moves every frame so projection can't be skipped,
  but static meshes rebuild `RotMat` via quat→matrix every frame (Animate) needlessly — a one-key
  fast-path skips it. Small win (Animate is cheap).
- **Lazy env `EU/EV`** — compute after face-visibility cull, not before (RENDER.CPP:382-385).
- **Per-object light culling** — skip lights whose range sphere doesn't touch a mesh.

---

## Assumption corrections (vs the initial framing)

- "Better mapper with **z-buffer** + **swizzled textures**" — neither exists in the DOS tree;
  both are new work. Z-buffer likely net-negative on MMX-200; swizzle marginal. Defer behind Phase 0.
- "ASM mappers with **SMC magic**" — true and **already on** the hot texture paths. Not a new lever.
- "**C barycentric** mappers too advanced for MMX-era" — agreed. DOS already uses hand-tuned
  scanline-affine ASM (the right primitive); native's barycentric design targets 8-wide AVX2. Mine
  master for *algorithms* (lighting), not its rasterizer.
- Master `FDS/RENDER/Lighting.cpp` is the clean reference for Phase 1.

## Recommended order
Phase 0 (measure) → Phase 1 (incremental lighting) → Phase 2 (MMX gouraud) → re-measure →
Phase 3a (City reflection clip) → Phase 3b/c and Phase 4 only if numbers justify.
