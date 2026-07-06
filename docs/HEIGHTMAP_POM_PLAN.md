# Height-map / POM plan (parked design, 2026-07)

Design notes for upgrading the kernel's parallax from today's single-shift to
real height mapping, in cost tiers. Written down so the constraints and cost
math from the texture-filtering campaign don't get re-derived. All costs are
ESTIMATES unless marked measured — the mandated first step of any tier is the
spike measurement at the bottom.

## Where parallax lives today (tier 0)

- The deferred kernel computes a heightmap-shifted UV per pixel and re-fetches
  the base texel at the shifted address, point-sampled. Ctx heightmap plumbing:
  `FDS/FILLERS/Mekalele.h` (~line 348, `HeightMap mip[miplevel]`, same tiled
  layout as Txtr); the kernel-side branch keys on `Mat->HeightMap`.
- Users: GREETS floor + walls — i.e. **60–90% of frame in an interior**,
  1.2–1.9M px at 1080p. (Do not repeat the mistake of assuming small coverage.)
- Interaction with texture filtering (`--texture-filter`, commit e9e60a3):
  heightmap materials are EXCLUDED from the raster-resolved albedo plane —
  the plane holds the UNSHIFTED sample, useless post-shift. Parallax materials
  keep the kernel suv fetch. Any POM tier must fetch at the shifted UV inside
  the kernel; conveniently the shifted UV is computed in float there, so the
  sub-texel FRACTION is available (unlike the 20-bit integer suv).

## Layout decision (settled — do not revisit)

Heightmaps STAY swizzled/tiled. The march direction is the view ray projected
into UV space — arbitrary, rotating with the surface — and tiled layout has
isotropic locality (the reason GPU texturing tiles memory). For an 8-bit
heightmap an 8×8 tile is exactly one 64-byte cache line: a march step sequence
stays in one line until it crosses a tile, in any direction. Linear layout only
wins for axis-aligned walks. Per-tap address cost is a few ops via the existing
`tile_du`/`tile_dv` helpers.

## Tier 1 — filtered parallax (small, do first)

Bilinear at the SHIFTED UV inside the kernel for heightmap materials (the
fraction is right there in float). Kills the point-sample crawl on greets
walls the same way the albedo plane did for city. Est +1–2 ms at greets
coverage under deferred_quarter; measure, don't trust.

## Tier 2 — constrained POM (the realistic "real height mapping")

Components, all mapping to existing engine machinery:
1. **Relaxed cone-step maps**: offline bake per heightmap (per-texel cone
   ratio) — same bake culture as the env stores / city_envmap_cube.bin.
   March converges in ~3–5 taps instead of 16–32.
2. **Quarter-res OFFSET field**: march once per 2×2 block, bilaterally
   upsample the OFFSETS (smooth except silhouettes), fetch color full-res.
   ÷4 on march cost without quarter-res texture look. Note deferred_quarter
   already runs lighting at 1-of-4 in greets — decide whether the offset
   pass rides that grid or its own.
3. **LOD**: full march near + grazing; fade to tier-1 single-shift with
   distance/angle. Most parallax pixels are far/oblique.

Cost math at greets coverage (user-corrected): full-res naive 5-tap ≈
+45–70 ms — dead. Quarter-res offset field + cone maps + LOD ≈ **+2–5 ms**
— plausible showpiece budget. Self-shadow (second short march toward the
dominant light) roughly doubles the march cost; keep it a separate toggle.

## Tier 3 — the full-price chain (documented so it exists; not planned)

- Full-res march (no offset upsample): ×4 of tier 2's march cost.
- Per-pixel self-shadowing march toward each significant light (×N lights).
- Silhouette correctness: depth-adjust the marched pixel (write corrected Z
  so edges and interpenetration follow the heightfield) and clip marched UVs
  that exit the face border instead of clamping (true silhouettes).
- Trilinear taps during the march (mip chains for heightmaps + cone maps
  per level).
- Realistic total at greets coverage: 15–70 ms/frame class. Offline/stills
  or tiny hero surfaces only, unless the whole march is SIMD'd 8-wide AND
  coverage is cut hard.

## Spike results (2026-07-06, PARTIAL — agent crashed mid-run, NOT re-verified)

An opus agent implemented the spike + Tier 1 (commit below) but hit a
session limit before the visual/doc pass. What exists:
- **`FDS_POM_SPIKE=N`** march landed in Mekalele.h (default 0 = byte-identical).
- **Tier 1 filtered parallax** landed: the `!HeightMap` exclusion was dropped
  in all three albedo-fetch sites so heightmap materials get bilinear-at-
  shifted-UV under `FDS_TEXTURE_FILTER>0`.
- **Per-tap cost (AGENT-MEASURED, DIRTY machine, gbuffer-barrier metric, NOT
  reproduced by me):** single-shift 5.4–6.0 ms / N4 6.7–7.6 / N8 7.9–9.0
  (non-overlapping) ⇒ the agent's ~0.33 ms/tap. Treat as a rough anchor only.
- **Visual verdict: NONE.** I could not exercise parallax in the snapshot
  harness afterward — `greets@t=5780` and `pbrtest` both show ZERO strength
  sensitivity (0.1 vs 0.9 = 0 px changed), i.e. no heightmap geometry in
  those frames. A live `--parallax` run (or a posed cam on the greets floor /
  the momy mesh) is needed to confirm the march and Tier-1 filtering do
  anything, and to judge whether the march reads as deeper relief.
- **KNOWN RISK to verify before trusting the filter path on parallax scenes:**
  the raster gate keys parallax on `F->Txtr->HeightMap`; the kernel's dropped
  exclusion was on `Mat->HeightMap`. If those two ever disagree for a pixel,
  the kernel reads `gb.albedo` where the raster didn't write it → stale/garbage
  albedo. Harmless at `FDS_TEXTURE_FILTER=0` (default, proven byte-identical);
  must be checked before `FDS_TEXTURE_FILTER>0` is used on greets/parallax.

## Mandatory first step (before ANY tier)

Spike measurement to anchor the per-tap cost with real memory behavior:
1. TailProf scope around the kernel's existing parallax branch → measures
   tier-0 cost at true coverage (also tells us what greets pays today).
2. A naive 8-step linear march behind an env flag (no cone maps, no
   upsample) → per-tap ns on real data. Multiply honestly from there.
Bench harness: `--bench=scene@scene=greets,...` + FDS_TAIL_PROF=1; the
whole session's measurement discipline applies (interleaved A/B, dummy
video driver, absolute cmake paths, loop-aware dump captures).
