# Env-map cube-face campaign plan

Goal: replace the single equirect environment map with **six padded cube
faces** in both env consumers — the deferred per-pixel env-specular path and
the forward per-vertex reflective path (which CITY uses even under
`--shard-deferred`) — with **zero trig in any hot loop** and measured
performance ≥ today. An earlier 6-face attempt was abandoned as too slow;
the design below explains why this one is cheaper than the *current* code,
not just cheaper than that attempt.

## Why equirect is the problem

- **Pole distortion / texel waste**: half the panorama's texels sit near the
  poles where reflections rarely look; the equator (city skylines) is
  under-resolved. Vertical smearing near zenith/nadir.
- **Wrap seam**: the forward path needs the U-wrapping hack
  (`Transform.cpp:1578`, span > 0.8 → +1) and still breaks for triangles
  spanning the seam badly; the deferred fetch wraps per-pixel.
- **Double resample**: `EnvBake.cpp` renders six perfect cube faces, then
  *stitches them into an equirect* (a full bilinear-ish resample), which the
  hot loop then point-samples. Sharpness is thrown away before the first
  frame. CITY does the same dance via `CalcEquirectangularPanoramaTable`.
- **Trig per lookup**: `atan2_approx` + `asin_approx` per pixel
  (DeferredSurfaceKernel.cpp:737-739) and per vertex (Transform.cpp:1570-71).

## Core design decisions

### D1. Cube lookup is trig-free — this is the whole trick

Face select = dominant-axis comparison; UV = one divide (gnomonic
projection). No atans, ever:

```c
// dir (x,y,z) — normalization optional (scale cancels in a/m)
ax=|x| ay=|y| az=|z|
axis  = ax>=ay && ax>=az ? 0 : (ay>=az ? 1 : 2)     // compares+selects
face  = axis*2 + (dir[axis] < 0)                     // 0..5
m     = |dir[axis]|;  (a,b) = the other two components with per-face signs
u = 0.5f + 0.5f * (a/m) * kInvPad                    // ONE divide (or rcp)
v = 0.5f + 0.5f * (b/m) * kInvPad
```

~2 compares + selects + 1 div + 2 fma vs today's two polynomial approx +
wrap. Strictly cheaper in both hot paths. SIMD-able with vectorclass masks
if ever needed (the env compose is scalar per-pixel today; keep it scalar).

### D2. PADDED faces (overscan bake), not gutters

Bake each face with FOV > 90°: `kEnvCubePad = 1.25f` → half-angle
`atan(1.25) ≈ 51.34°` (FOV ≈ 102.7°). The nominal cube [-1,1] range maps to
the central `1/1.25` of the face; the outer ~10% ring holds the *neighbor
faces'* content. One constant solves three problems at once:

1. **Blur-chain seams** (deferred roughness mips): 2×2 box downsamples read
   valid neighbor content at face edges instead of clamp garbage. At 512²
   faces the margin is ~51 texels — covers all 4 mip levels' filter reach.
2. **Per-triangle face spanning** (forward path): a triangle whose vertex
   reflection dirs straddle a face boundary projects entirely onto the
   *centroid-selected* face; UVs beyond ±1 land in the padding (valid
   content) up to ~6.3° of overhang, clamp beyond. No triangle splitting,
   no per-vertex face checks — this is what makes 6-face per-VERTEX env
   mapping viable at all.
3. Future bilinear fetch needs no edge special-casing.

Cost: ~20% linear resolution inside the nominal window. A 512² padded face
still beats a 1024² equirect for angular resolution at the equator
(512/102.7° ≈ 5.0 px/deg vs 1024/360° ≈ 2.8 px/deg).

### D3. One canonical convention, one home

New `FDS/RENDER/EnvCube.h`: face order/orientation, `kEnvCubePad`,
`EnvCube_DirToFaceUV()` (scalar inline, trig-free),
`EnvCube_FaceUVToDir()` (bake/diagnostic inverse), and a
`EnvCube_SelfTest()` round-trip check (dir → face/uv → dir, max angular
error; run under an env var, print + abort on failure). EVERY consumer
(EnvBake, deferred kernel, Transform.cpp, CITY bake, debug painters) uses
these helpers — the equirect era's convention drift (CITY's `ROT=2` quirk,
`ENVFLIP` diagnostics, stitcher orientation) must not repeat. Reconcile
existing conventions by TESTING (painted debug faces + ENVPROBE), not by
reading tea leaves.

### D4. Feature-flagged, byte-identical when off

`FDS_FLAG_BOOL(env_cube, "FDS_ENV_CUBE", 0, ...)` — flag OFF keeps the
equirect path bit-for-bit (both consumers). All new code branches on it at
the same altitude the equirect code runs today. Default flips to 1 only in
the final slice after A/B validation. `render_gate.sh` scenes don't enable
`--env_refl` (default 0) and city isn't in the gate, so gates must stay
3/3 PASS with the flag both off AND on.

## Where everything lives today (verified anchors)

| What | Where |
|---|---|
| Deferred env compose (hot) | `FDS/RENDER/DeferredSurfaceKernel.cpp:676` `EnvSpecComposeScalar` — equirect math at :737-743, mip fetch `fetchLvl` at :766; called from two sites (:1810, :3667) |
| Deferred pano struct | `FDS/RENDER/EnvBake.h` `EnvPanoLinear` (linear mips, parallax AABB, bake point) |
| Deferred bake | `FDS/RENDER/EnvBake.cpp` — renders 6 cube faces then stitches (:94-193); `EnvReflection_FramePrep` per-material bakes, matID→pano table |
| Forward per-vertex env | `FDS/RENDER/Transform.cpp:1507-1591` — `Face_Reflective` block: cv-pull parallax hack, per-vertex equirect (:1570-73), U-wrap hack (:1578) |
| Env UV plumbing | `Face::EU1..EV3` (`FDS/Base/Face.h:54`), clipper stamps `Vertex::EU/EV/EUZ/EVZ` (`FDS/FRUSTRUM/FRUSTRUM.CPP:866`) |
| Filler second texture | `FDS/FILLERS/TheOtherBarry.h:967` (`TEXTURETEXTURE` → `F->ReflectionTexture->Data`); **UV fixed-point scale and shift are HARDCODED for 1024² at :516-517 (`* 1024.0f`) and :529 (`packed_tile_u(u1, 10, …)`)** — plus `t1_umask_swizzled`/`t1_vmask` init (find it) |
| CITY bake | `DEMO/CITY.CPP` — `CalcEquirectangularPanoramaTable` (:1503), 1024² pano, `Materialize()` (Sachletz-tiled, pow2), installs `Obj->Reflection` (:2053) and stamps every window face's `F->ReflectionTexture` (:2087) |
| CITY disk cache | `DEMO/CityPanoramaCache.{h,cpp}` — raw pre-Sachletz panoramas, FNV key over FLD bytes + dims + names |
| Diagnostics | `ENVPROBE` (sample along normal), `ENVFLIP`, `ENV_NOFETCH` (fetch-vs-math cost attribution), `FDS_ENVBAKE_DUMP`, `--env_refl_viz`, city `paintDebugPanorama` + `savePanoramaPPM` |
| Snapshot harness | `SDL_VIDEODRIVER=dummy ./DEMO --deferred --snapshot=<scene>@t=… --out=DIR` (run from `Runtime/`), see `docs/GRAPHICS_PIPELINE.md:209` |

## Slices

### Slice A — `EnvCube.h` foundation
- Face convention, pad constant, `DirToFaceUV` / `FaceUVToDir`, self-test.
- Debug face painter (per-face label + gridlines — the cube analog of
  city's `paintDebugPanorama`) for use by both bakes.
- Feature flag `env_cube` (default 0) in `FeatureFlags.def`.
- Gate: builds; self-test passes; `render_gate.sh` 3/3 (nothing wired yet).

### Slice B — deferred path
- `EnvBake.cpp`: when `env_cube`, render the six faces with the padded FOV
  and keep them — **no stitch** — as a new `EnvCubeLinear` (or extend
  `EnvPanoLinear` with a mode discriminator; keep one matID table): 6 faces
  × `kMaxMips` linear mips, per-face 2×2 box downsample. Layout: one
  contiguous allocation per mip, face-major (`face*R*R` offset). Suggested
  face res: `env_refl_res/2` (matches the existing "cube faces render at
  half this" convention) — memory comparable to today.
- `EnvSpecComposeScalar`: branch on `env_cube` → `EnvCube_DirToFaceUV` +
  face-major fetch. Parallax correction, roughness→mip trilinear, Fresnel,
  metal tint all UNCHANGED (they operate before/after the lookup).
  `ENVPROBE`/`ENVFLIP`/`ENV_NOFETCH` must work in cube mode.
- `--env_refl_viz` should display something sane for cube bakes (a 3×2
  face grid blit is fine).
- Validation:
  - ENVPROBE continuity: probe render must show NO jumps at face
    boundaries (the classic sign of orientation/winding mistakes).
  - Painted-debug-face bake: verify each face lands where the label says.
  - A/B snapshots (flag off/on): chase + editor pbr scene (whichever
    scenes actually enable `--env_refl`; check how the material editor
    turns it on). Expect sharper reflections, no seams; capture PPMs.
  - Perf: time the env compose (ENV_NOFETCH on/off delta, flag off vs on)
    on the same pose — cube math must be ≤ equirect math.
- Gate: `render_gate.sh` 3/3 both flag states.

### Slice C — forward per-vertex path + CITY
- Storage: 6 face `Texture*`s reachable from the Face's owner. Suggested:
  `Texture* EnvCubeFaces[6]` next to `Obj->Reflection` (Object) mirrored
  onto the TriMesh or stamped per-face the way `F->ReflectionTexture`
  already is (Transform sees only `F` and `T` — pick what's reachable in
  the `Face_Reflective` block and document it).
- `Transform.cpp` `Face_Reflective` block, `env_cube` branch:
  - Keep the cv-pull parallax hack verbatim (it adjusts the reflection
    origin, orthogonal to parametrization).
  - Compute the 3 reflected dirs as today; face select ONCE per face from
    `d0+d1+d2` (centroid dir); all 3 vertices project gnomonically onto
    that face (pad scale, clamp to [0,1)); stamp `EU/EV` + swap
    `F->ReflectionTexture = faces[k]`. DELETE the U-wrap hack in this
    branch. Zero trig.
- Filler: fix the hardcoded 1024 assumption. Face textures are pow2
  (512² suggested); either derive scale/shift/masks from
  `F->ReflectionTexture` dims (check how `t1_umask_swizzled` is built —
  extend that) or hard-require 512 and change the constants under a
  static assert. Do NOT add per-pixel work: dims are per-triangle
  constants.
- CITY: when `env_cube`, capture the six padded faces directly (the cube
  render already exists; drop the pano reprojection), `Materialize` each
  face (Sachletz, pow2 — 512² suggested), install as the building's face
  set, stamp window faces. Extend `CityPanoramaCache` to store 6 faces
  raw (pre-Sachletz); include a format tag + pad + face res in the key so
  old caches miss cleanly; keep the legacy pano record for flag-off (both
  variants can coexist keyed separately).
- Validation:
  - City A/B snapshots (`--snapshot=city@t=150,300`) flag off/on — look
    specifically at large glass facades for face-boundary artifacts
    (padding should absorb; clamp artifacts mean a facade quad spans >
    ~6° — if seen, note it and consider pad 1.4 for city only).
  - City init time (bake path is CHEAPER: no 1024² table reprojection)
    and steady-state frame ms (vertex math cheaper, filler unchanged).
  - Chase scene if it uses the forward reflective path.
- Gate: `render_gate.sh` 3/3 both flag states.

### Slice D — flip + docs
- Flip `env_cube` default to 1 if B+C validation is clean; keep equirect
  path for `--no-env_cube` A/B (do not delete).
- Update `docs/GRAPHICS_PIPELINE.md` env section + this doc's status.
- Final full pass: render_gate, cloaktest depth-0 sanity, snapshots
  archived, perf table in this doc.

## Hard-won facts to respect

- Offscreen bakes: `OffscreenViewScope` owns the surface/camera/NZP swap;
  `g_engineSurfaceMutex` is non-reentrant (`g_offscreenViewDepth` gates).
  EnvBake + CITY bake already do this — don't re-derive.
- `Sachletz` is one-way; caches must store raw (pre-tile) pixels
  (CityPanoramaCache already documents this).
- The deferred `EnvPanoLinear` mips are LINEAR (unswizzled); the forward
  `ReflectionTexture` is Sachletz-tiled pow2. Two different artifacts of
  the same bake — keep it that way.
- `Materialize()` requires pow2 dims (`iLog2`).
- Face::EU* are consumed by the clipper per frame (FRUSTRUM.CPP:866); they
  are stamped fresh each frame by Transform — no `uvFromVertices`-style
  re-sync issue applies to EU/EV.
- Perf attribution: use `ENV_NOFETCH` to separate math from gather cost
  before concluding anything about the lookup change.

## Risks

- **Orientation bugs** (v-flips, face swaps): burn them down with the
  painted debug faces + ENVPROBE FIRST, before looking at real scenes.
- **Large facade quads** spanning > pad margin: clamp smear. Measure on
  city; escalate pad or note as a known limit.
- **Filler mask plumbing**: the 1024 hardcode may hide more than two
  constants — audit every `t1_*` use.
- **Cache stampede**: key must change (format tag) or stale pano caches
  will be misread as face data.
- Memory: 6×512²×4×1.33 ≈ 8.4 MB per reflective material (deferred) /
  per building (city, no mips ≈ 6.3 MB). Comparable to today's 1024²
  panoramas; `env_refl_res` remains the knob.
