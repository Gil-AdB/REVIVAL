# Backlog plans (2026-07, post reflection-jump fix cee927d)

Four planned items, each independently landable. Estimates are estimates;
each plan names its measurement gate.

## 1. Position-aware env reflections (bake is centroid-anchored)

Problem: each building's cube store is baked from the material centroid
(mid-height). The compose looks up by reflected DIRECTION only (city stores
are `noParallax` — the whole-city-AABB parallax drew moving exit-face bands,
so it was disabled; the cv-pull modulates magnitude but is not positional).
Consequence: a pixel low on the glass can never see what is actually low in
the world (building bases, waterline) — everything reflects as seen from
mid-height.

Cheap fixes, in cost order (all in EnvSpecComposeScalar + the vec front-end,
flag-gated, city-tunable via params):

1a. **Ground-plane parallax for downward rays** (~6-10 ops/px, the 80% fix):
    for rays with rw.y < 0, intersect with the ground/water plane
    (y = waterY, known in city), then look up the direction from the BAKE
    POINT to that intersection instead of raw rw. Exact for the dominant
    complaint (bases/water in low glass reflect correctly); upward/lateral
    rays stay direction-only. Plane intersect is 1 div + fmadds; blend the
    correction out near rw.y=0 to avoid a seam at the horizon.
1b. **First-order vertical correction** (2-3 ops/px): rw.y' = rw.y +
    (bakeY - pixelWorldY) / R with a tunable scene radius R (sphere-proxy
    approximation). Crude but positional; combine with 1a for lateral rays.
1c. **Stacked probes per building** (2-3 height bands, pick/blend by pixel
    height): runtime cheap (lerp two fetches), but memory ×2-3 on ~150MB of
    stores and bake time ×2-3. Only if 1a+1b look insufficient.
1d. **True local-proxy parallax** (per-bake street-canyon box instead of the
    whole-city AABB that failed): correct but needs proxy authoring or
    heuristic box fitting; revisit only if 1a-1c fail the eye test.

Recommended: implement 1a + 1b behind `--env-ground-parallax` /
`--env-vert-parallax=R`; verify with the micro-dolly harness (no new
discontinuities — the plane blend must be continuous) and by eye at a pose
looking down a facade toward the waterline.

## 2. Caustics/water state in bakes (bake-time snapshot problem)

Problem: bakes render at scene init, before/without the animated water and
searchlight state — reflections show the original static water texture.

Options:
2a. **Bake after first-frame world state** (ordering fix, cheapest): run
    EnvReflection_FramePrep's bakes after procedural water + dispMap init so
    the baked water at least LOOKS like the live water (one frozen phase).
    Static phase is invisible in practice — glass reflections of water are
    small and busy.
2b. **Budgeted re-bake** (flag experiment): re-render one cube face per
    frame round-robin across stores (~71 stores × 6 faces ≈ 7s full cycle at
    60fps; prioritize stores nearest the camera). Live-ish reflections incl.
    searchlight sweeps, at one face-render/frame cost (measure; likely
    1-3 ms). Needs the bake path to be re-entrant mid-scene (it is — the
    FramePrep guard exists) and mip re-chain per face.
2c. Exclude water from bakes and composite analytic sky/fog below the
    horizon line in the store (bake-time shader) — moderate effort, kills
    the artifact class without runtime cost.

Recommended: 2a first (hours), then evaluate whether 2b is worth its budget.

## 3. Forward-path texture filtering (glass/water/additive still point-sampled)

The deferred albedo filtering (e9e60a3) covers Mekalele-rasterized opaque
only. TheOtherBarry keeps point sampling for TEXTURETEXTURE (city glass in
mirror pass, reflective surfaces), ADDITIVE, and transparent modes — those
still texel-crawl. Barry already contains a NORMAL_BILINEAR block (it was
the template for bilinear_sample_x8), so the plan is wiring, not invention:
route `--texture-filter>=1` into Barry's relevant TTextureMode paths, reuse
the swizzle +1 helpers, measure the raster cost delta on the city mirror
pass and fountain (TBR sprites excluded — they're already smooth content).
Gate: default-off byte identity + the flicker metric on a water-reflection
region.

## 4. Smooth scene clock rollout (city-only today)

b36cc83 wired the rate-EMA clock (g_FrameTimeF) into CITY's CurFrame only.
Rollout = replicate the one-line CurFrame change in CHASE/FOUNTAIN/CRASH/
GREETS drivers (each computes CurFrame from g_FrameTime the same way),
keeping the g_fineSceneClock gate (snapshot/bench determinism already
global). Prereq: user confirms the city feel. Risk: none structural (the
per-scene offset is bounded and constant-ish; the failure mode of the
reverted EMA attempt — divergent dual clocks — does not apply, verified in
the city capture).
