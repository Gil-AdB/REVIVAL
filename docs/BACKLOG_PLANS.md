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

### 2 — STATUS 2026-08-13: the code landed, the DEFAULT never did

User report: *"for city — the reflections on the buildings are reflecting the
old water texture, not the current water look. we did some work on it a few
weeks ago, but I don't remember we got a conclusion."* That is this item, and
the missing conclusion is a **flag default**, not missing code.

**What the July campaign built.** `5d28db7` (2026-07-11) mapped the wiring and
shipped the sample-time WaveSlope perturb (`--env_live_water`, default OFF);
`0162d3b` the same day shipped the real content fix — `env_live_water_shade`
(default 1, but only *inside* `env_live_water`): hide the water mesh for the
probe bake, then re-shade every unoccluded water-plane texel with the main
view's own formula (`deep*(1-F) + underlay*F` Schlick + `pwater::
CausticModulation`, wave clock frozen at 0). Both env consumers — the forward
paraboloid sheets and the deferred `EnvPanoStore` mips — derive from the same
faces, so both inherit. Nothing after that touched it: `git log` shows the last
`ProceduralWater`/city-water commits are `0162d3b`/`97677a5` (2026-07-11), and
this §2 was never updated. So the whole feature ships **dark**.

**Why he sees it.** `Runtime/PRESETS/city-noir.flags` line 68 sets
`water_procedural` — the main view gets the deep-colour fresnel composite.
It does **not** set `env_live_water`, and `FeatureFlags.def:160` defaults it 0.
Main view = current noir water; every window reflection = the 1998
`TEXTURES/WATER.JPG` caustic tile.

**MEASURED at HEAD (a3a72cc)**, both arms `--no-city_envmap_cache`, i.e. both
COLD-BAKED, `--snapshot=city@t=1961 --deferred --flags-file=PRESETS/city-noir.flags`,
probe faces via `FDS_ENVBAKE_DUMP=all`:

| probe | −Y (straight down) mean RGB, shipping default | with `--env_live_water` | −Y texels changed |
|---|---|---|---|
| b1.lwo | 30.0 / 78.4 / **208.6** | 26.7 / 38.4 / **51.8** | 80.2 % |
| b3.lwo | 38.3 / 50.9 / **161.9** | 27.1 / 31.1 / **45.4** | 91.9 % |
| b5.lwo | 40.6 / 70.0 / **225.7** | 26.7 / 30.3 / **46.6** | 93.7 % |

The +Y (sky) cell is **0.0 % changed** on all three — the clean control that
only the below-horizon region moved. Whole-atlas change 32.8–37.0 %. Main frame
at t=1961: 182 317 px changed (8.79 %), max Δ 92, mean \|Δ\| 7.98 on changed;
every Δ>12 lies in y∈[1,641] — the facades. Evidence:
`docs/img/envmap/citywater_b5_downface_default_vs_livewater.png` (the money
shot: electric-blue tile vs deep water + mirrored towers),
`citywater_b1_downface_…`, `citywater_{b1,b5}_atlas_…`,
`citywater_t1961_facade_…`.

**Dead hypotheses, with numbers.**
- *Stale disk cache.* Killed by the cold-bake arm above: a fresh bake at HEAD
  is equally blue. (The salt hole `6293698` fixed is real but orthogonal — and
  note the salt still does **not** fold the `water_*` look floats or
  `env_live_water*`; harmless only because the default bake reads none of them
  and the live-water bake bypasses the cache. Anyone who caches the live bake
  must fold them — see 2e.)
- *A newer water texture the bake missed.* There is exactly one:
  `Runtime/TEXTURES/WATER.JPG` == `Scenes/CITY/WATER.JPG` (md5
  `61a3c37c79ca5856bb454d8b707510fb`), untouched since 2018 (`394404f2`).
- *Perturb-only is enough.* `--env_live_water --no-env_live_water_shade`
  leaves the store byte-identical to the shipping static bake — it animates the
  wrong content.

**Costs, measured** (each arm ×2–3, same machine, 1920×1080):

| arm | city init (whole `--snapshot` run, wall) |
|---|---|
| today: cached cube | **0.61 / 0.71 / 0.66 s** |
| cold static bake (`--no-city_envmap_cache`) | 2.42 / 3.17 s |
| cold bake + live-water re-shade | **4.82 / 4.28 s** |

Per-frame is **noise**: bench `t=1961, iters=30` gives 92.607 (default) /
92.985 (perturb only) / 92.714 ms (full live water) — the forward per-vertex
path perturbs per vertex. Not measured under `--city_env_pixel` (per pixel).
Cube artifact = 446 694 000 B = 426 MiB.

**DIRECTIONS (user's call — this is a look change).**

2d. **Just default it on for city.** One line: `FeatureFlags.def:160` 0→1, or a
    `setDefault(env_live_water, true)` in the city factory beside the existing
    `water_procedural` one (`DEMO/CITY.CPP:3750`). Fixes content *and* motion.
    Costs **+3.6–4.2 s of city init on every run** (the mode bypasses the cube
    cache by design) and ~0 ms/frame. Still stale afterwards: specular glints,
    froxel-fog inscatter, moving traffic/searchlights; documented caveat —
    baked below-horizon *non*-water content (piers, bridge decks) wobbles a few
    texels because the perturb has no water mask.
2e. **2d + give the live bake its own salted cache.** `DEMO/CITY.CPP:2639`,
    drop `&& !liveWaterShade` from `cacheOn` and fold `liveWaterShade` plus the
    ~9 `water_*` floats the re-shade reads (`water_deep_{r,g,b}`,
    `water_reflectivity`, `water_fresnel_base`, `water_albedo_mix`,
    `water_tex_{scale,warp}`, `water_bump_scale`) into `bakeFlagSalt`. ~10
    lines; the filename already carries the salt so variants coexist. Buys back
    the 0.66 s init. Costs one more 426 MiB file per flag variant, and the
    `water_*` floats stop being live-tunable-per-run unless they're in the salt
    (they must be, or it's the `--mips` trap again).
2f. **2d with `--env_live_water_amp=0`.** Store is **byte-identical** to 2d
    (measured: `06aa55f3…` both), only the lookup wobble is gone — so the
    reflected water reads as a still mirror and the pier/bridge wobble caveat
    disappears. Same init cost. The fallback if the wobble reads wrong.
2g. **Periodic re-bake (old 2b) — REJECTED with numbers.** Measured bake cost
    at HEAD: (4.28−0.66)/71 ≈ **51–59 ms per building** for its 6 faces, i.e.
    ~8–10 ms per *face*. One face/frame is ~10 % of the 92 ms city frame and
    needs 71×6 = 426 frames ≈ 40 s at the measured 10.7 fps for one cycle. And
    it buys *nothing* for this defect: without the re-shade a re-bake
    reproduces the same static tile (that was already 5d28db7's option C
    rejection). Only worth revisiting for moving traffic/searchlights.

Recommendation: **2d**, with 2e queued if the 4 s init annoys, 2f held as the
look fallback. Old 2a is superseded (the re-shade is strictly better than
re-timing the same static composite); old 2c is superseded by the re-shade.

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
