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

## Validation log (living)

### Flag-off legacy-env-path pins + noise floor (established before slice B)

The render gate (mirrortest/conetest/halotest) exercises the SHARED deferred
path but NOT either env consumer (env_refl defaults 0; no gate scene has
`Face_Reflective` faces). So the gate proves shared plumbing didn't drift; the
legacy env path is pinned separately by snapshotting with `--env_refl` ON and
`env_cube` OFF, diffing against a committed baseline under `/tmp/envcube/`.

Noise-floor measurement (same pose rendered ≥3× with the flag off,
`tools`-style byte diff via `/tmp/envcube/ppmdiff.py`):

| pose | determinism | usable as flag-off pin? |
|---|---|---|
| `city@t=150` (forward reflective path) | **byte-exact** across 3 runs | YES — tolerance 0 |
| `chase@t=500`, `chase@t=1000` | **byte-exact** | YES — tolerance 0 |
| `city@t=300` | **nondeterministic** (~64% bytes differ on re-run; moving ships/headlights/timing) | NO — dropped |

Flag-off regression rule per slice: re-render `city@t=150`, `chase@t=500`,
`chase@t=1000` with `env_cube` OFF and `--env_refl`; require **byte-identical**
to `/tmp/envcube/baseline/`. Any nonzero diff = a regression in shared plumbing
(filler t1 masks, EnvPanoLinear/table shape, CityPanoramaCache load) → stop and
fix. (Deferred-consumer flag-on validation is structural + visual A/B only; NEVER
byte-compare cube output against equirect.)

### Slice A — EnvCube.h foundation + flag  (DONE)
- `FDS/RENDER/EnvCube.h`: convention (basis = EnvBake's kCubeFaces),
  `kEnvCubePad=1.25`, trig-free `EnvCube_DirToFaceUV`, inverse
  `EnvCube_FaceUVToDir`, `EnvCube_SelfTest`, `EnvCube_PaintDebugFace`.
- Flag `env_cube` (default 0) added after `env_refl_viz`.
- Self-test hook in `REV.CPP` after parseArgs, gated by `FDS_ENV_CUBE_SELFTEST`.
- **Self-test: PASS** — `dir→uv→dir maxAngErr=5.98e-4 rad` (that magnitude is
  `acos`-near-1 ill-conditioning; true dir error ~1e-7), `central uv→dir→uv
  maxErr=5.96e-8`, `faceMiss=0`.
- **Gate: 3/3 PASS** flag OFF and flag ON (nothing wired yet).
- Flag-off pins: N/A (no render wiring yet; header + flag + startup hook only).

### Slice B — deferred path  (DONE)
- `EnvPanoLinear` gained `bool isCube`; in cube mode each `mip[k]` is a
  FACE-MAJOR block of six `(W>>k)²` faces (`W==H==faceRes`, face `f` at
  `f*(W>>k)²`).
- `EnvBake.cpp`: extracted `renderSixFaces(...,fovDeg,...)` (equirect passes
  90°, cube passes `EnvCube_FaceFovDegrees()≈102.68°`); new
  `renderCubeFacesMajor` packs padded faces face-major (no stitch);
  `boxDownsampleCube` downsamples each face independently (padding absorbs the
  edge box); `bakeStore` branches on `env_cube`. `FDS_ENVBAKE_DUMP` writes a
  3×2 grid `/tmp/envbake_cube.ppm`; `FDS_ENVCUBE_PAINT` overrides the render
  with painted debug faces.
- `EnvSpecComposeScalar`: ONE branch on `envP->isCube` at the direction-lookup
  altitude — cube → `EnvCube_DirToFaceUV` + face-major fetch; equirect
  unchanged. Parallax, roughness→mip trilinear, Fresnel, metal tint untouched.
  `ENVPROBE`/`ENVFLIP`/`ENV_NOFETCH` apply to `rwx/rwy/rwz` before the branch →
  work in cube mode.
- `EnvReflection_DrawViz` (`--env_refl_viz`) lays the six faces out 3×2 in cube.
- **Validation:**
  - Painted debug-face grid dump: all 6 faces correct (tint, dot-count, green
    (0,0) corner, red +u / blue +v bars, yellow padding rectangle).
  - ENVPROBE + painted faces on chase cockpit: cube vs equirect probe renders
    essentially identical (same environment orientation) and BOTH seamless —
    confirms bake-camera orientation matches the convention, no face seams.
    PNGs: `/tmp/envcube/probe_{cube,equi}_painted.png`.
  - A/B chase (off vs on, real bake): differs by design (cube reflections are a
    tiny screen contribution in chase — 125 bytes @ t=500). PPMs:
    `/tmp/envcube/{off,on}/chase_t000500_color.ppm`.
  - **Flag-OFF pins: BYTE-IDENTICAL** — `city@t=150`, `chase@t=500`,
    `chase@t=1000` all 0-diff vs baseline. Gate 3/3 PASS flag OFF and ON.
  - Bakes `256×256` faces at `env_refl_res=512` (faceRes = res/2), as planned.
- **Perf:** coarse chase-snapshot wall-time (startup-dominated) OFF 0.10-0.11s
  vs ON 0.10s — no regression; cube marginally faster (bake skips the equirect
  stitch resample). Per-pixel math analytically cheaper (2-3 cmp + 1 div + 2
  fma vs `atan2_approx`+`asin_approx`+wrap). A clean per-pass number needs a
  reflection-dominated scene; chase's env area is too small to isolate — the
  real signal comes with CITY (Slice C).

### Slice C — forward per-vertex path + CITY  (DONE)
- `TriMesh::EnvCubeFaces[6]` (null by default) carries the six Sachletz-tiled
  face textures; Transform's `Face_Reflective` block gained an `env_cube &&
  EnvCubeFaces[0]` branch: face selected ONCE per triangle from the summed
  (centroid) reflected dir via `EnvCube_SelectFace`, all three vertex dirs
  projected onto THAT face via `EnvCube_DirToUVOnFace` (overhang → padded
  ring), `F->ReflectionTexture` swapped per frame. cv-pull hack kept verbatim;
  the equirect branch (incl. U-wrap hack) preserved byte-for-byte in the else.
- `TheOtherBarry` TEXTURETEXTURE: second-texture fixed-point scale + swizzle
  masks now derive from `F->ReflectionTexture->LSizeX/LSizeY` (t0-pattern
  `Log1Width/Log1Height`, default 10 = the old 1024² hardcode). Per-triangle
  constants — zero per-pixel cost.
- CITY: `bakeBuildingCubeFaces` renders the six padded faces on the existing
  1024² surface and 2×2-box-downsamples to 512² (free supersampling), packs
  face-major, `Materialize`s each face into `T->EnvCubeFaces`;
  `Obj->Reflection` = face-0 material (non-null marker for the windows-stamp
  loop; `Material::EnvTexture` verified inert in the deferred kernel).
  Separate cache file `cache/city_envmap_cube.bin` + format salt in the key.
- **Validation:**
  - **Definitive flag-off exoneration**: fresh-bake (cacheless) `city@t=150`
    rendered byte-identical WITH slice C vs WITHOUT it (stash/rebuild A/B) —
    the earlier 13.3% drift vs the plan-start baseline was the repo's STALE
    pre-branch `city_envmap.bin`, since reseeded.
  - Cache round-trip byte-identical (fresh-bake render == cache-hit render);
    cube/equirect caches coexist (flag-off after a cube run == fresh-bake
    render); chase@500/1000 flag-off pins byte-identical; gate 3/3 PASS flag
    OFF and ON.
  - Flag-on `city@t=150`: no face-boundary seams, no orientation artifacts,
    window reflections slightly sharper (PNGs: `/tmp/envcube/city_{off,on}_150.png`,
    face grid `/tmp/envcube/city_cube_faces.png`).

### Slice D — default flip

All gates clean (B+C above), so `env_cube` defaults to **1**; `--no-env_cube`
keeps the full equirect path for A/B.

**Perf table (M-series, city, Release):**

| metric | equirect (off) | cube (on) |
|---|---|---|
| steady-state `--bench=scene@scene=city,t=1961,iters=100` (3 runs) | 75.5 / 76.8 / 76.7 ms/iter | 74.6 / 75.8 / 76.2 ms/iter |
| city init, cacheless bake (71 buildings, wall) | ~2.75 s | ~2.88 s |
| cache size | 284 MiB | 426 MiB |
| chase snapshot wall (slice B, deferred path) | 0.10–0.11 s | 0.10 s |

Cube ≤ equirect on frame cost in all three bench pairs (~1% win — the
per-vertex lookup drops two polynomial trig approximations for compares + one
divide; the filler is unchanged). Init is a wash: the cube bake skips the
equirect table reprojection but renders with a wider FOV and writes 1.5× the
cache bytes.

### Post-slice-D fix — close-up smear (user repro, CITYSNAP_VIEW t=124)

Wide-span triangles broke the per-triangle gnomonic projection: close to a
tower the reflected eye sits near the glass, vertex reflection dirs span tens
of degrees, and two defects compounded — (1) dirs >~103° off the chosen
face's axis hit the `m<=0` guard in `EnvCube_DirToUVOnFace` and collapsed to
the FACE CENTER (0.5,0.5), smearing whole facades toward one texel; (2) dirs
past the padded window hard-clamped, flat-lining interpolation. Baked faces
verified clean (grid dump) — lookup-side only.

**Fix v1 (hybrid fallback — superseded):** cube face when all 3 vertex dirs
fit the padded window, else per-triangle fallback to an equirect pano. Fixed
the smear but still POPPED: the cube/fallback decision and the face pick are
camera-dependent, so triangles swapped charts discretely while moving, and
neighboring quads could disagree ("still jumping").

**Fix v2 (landed): static paraboloid hemisphere sheets.** The chart choice
must not depend on the camera. Per building, six PARABOLOID sheets
(`uv=(a,b)/(1+m)`, one divide, no trig — EnvCube_DirToParaboloidUV), each
covering the full hemisphere about one axis, synthesized at init from the
padded cube faces via a precomputed building-independent gather table (no
extra render, no extra cache). Transform binds each reflective triangle to
the sheet of its PANEL NORMAL's dominant axis (viewer-side-corrected) —
static for static geometry. Every physically possible reflected ray off a
panel lies in that normal's hemisphere, so one chart always suffices: no
fallback, no clamp threshold, no U-wrap, and coplanar quads share exact UVs.
The gnomonic per-triangle path (`EnvCube_DirToUVOnFace`) and
`TriMesh::EnvCubeFaces`/`EnvFallbackPano` were replaced by
`TriMesh::EnvHemiSheets[6]`.

Trade: sheet sharpness ~2.8 px/deg (≈ a CORRECT 1024 equirect; the legacy
bake's actual output was zoom-bugged below that) vs the gnomonic faces'
5.0 — temporal stability bought with some sharpness. The exact endgame, if
ever wanted: per-PIXEL env for city windows through the deferred kernel's
cube path (slice B) — no interpolation at all + parallax correction.

Validated: user pose t=124 + ±60/±150 dolly — coherent, smooth tracking;
2-unit micro-dolly frame delta 30.3k px (mean 3.47) vs legacy equirect's
36.2k px (mean 3.77) at the same pose — temporally MORE stable than legacy;
flag-off pin byte-identical; gates 3/3 both states; bench 73.8 ms/iter
(best measurement yet, still ≤ equirect's ~76).

### Per-pixel city windows experiment (`--city_env_pixel`, default OFF)

The "exact endgame" was tested: windows skip `Face_Reflective`, rasterize
through Mekalele with their per-building cloned matID, and the deferred
kernel's env compose reflects per pixel from the stores
`EnvReflection_FramePrep` auto-bakes (Reflection>0 → per-clone cube store,
centroid-deduped ≈ per building). Clone gets Reflection=40 (F0 0.4) +
Glossiness=96 under the flag.

**Findings:**
- **Latent bug discovered: the OuterVec kernel has NO env compose** — on
  scenes with `PreferOuterVec` (city), `--env_refl` has always been silently
  inert. Env only works via the wave-1 scalar/vec kernel (chase) or the
  wave-2 fill fallback. The experiment therefore requires
  `FDS_DEFERRED_OUTER_VEC=0`.
- **Look: excellent.** Smooth per-pixel reflection with Fresnel grazing
  response, roughness mips, parallax correction; zero charts → nothing can
  pop, by construction. More physically restrained than the stylized legacy
  half-add (tunable via clone F0 / --env_refl_gain).
- **Perf: confirms the historical "too slow" verdict, now quantified**
  (city t=1961 bench): paraboloid+OuterVec 77.5 ms/iter; scalar kernel
  alone 104.9 (+27 — the OuterVec switch, unrelated to env); + per-pixel
  env 120.8 (+16 for the compose; ENV_NOFETCH ≈ same → math/plumbing-bound,
  not fetch-bound). Net +56%. Adding a per-lane env compose INSIDE OuterVec
  would drop the kernel-switch cost and land ≈ +16-20% — the follow-up if
  per-pixel city glass is ever wanted for real.

**Status: kept as an experiment flag** (`FDS_CITY_ENV_PIXEL=1` +
`FDS_DEFERRED_OUTER_VEC=0 --env_refl`). Default OFF; the static paraboloid
sheets remain the production path (temporally stable at ~zero cost).

**v2/v3 (after live testing read "garbled, incorrect, jumpy"):**
1. **Garbled bands** = the AABB parallax proxy: the whole-city box is a
   hopeless fit for a street canyon; the reflected ray's box-exit-face
   switch locus drew dark diagonal bands that crawled with the camera.
   `EnvPanoLinear.noParallax` (per-store) now disables the correction —
   verified clean via ENVNOPARA A/B.
2. **Incorrect content** = FramePrep's auto-stores (256² bakes probed at
   the material's LARGEST window cluster, possibly the building's far
   side). Replaced: `EnvReflection_RegisterCubeFaces/AliasMaterial` feed
   the kernel THE SAME per-building 512² padded faces the forward path
   bakes (disk-cached, linear, building-centered), downsampled to 256²
   stores, noParallax, one store per building aliased across its windows
   clones. The 71 redundant auto-bakes are gone (7 legit ones remain:
   cockpit, ambulance glass, ...).
3. **Jumpy shimmer** = nearest-neighbor store sampling under continuously
   sweeping per-pixel reflection dirs. The cube fetch is now BILINEAR
   (in-face clamped 4-tap — the D2 padding makes it seam-free); the
   equirect fetch is untouched (legacy byte-stability). Chase cube
   reflections change by 55 px (mean 0.001) — strictly smoother.

Residual 2-unit-dolly frame delta (50k px vs paraboloid's 30k) is
LEGITIMATE reflection parallax on high-contrast per-pixel content (mean
|Δ| near-equal, 3.72 vs 3.47) — physics, not popping. Perf unchanged
(124.7 ms/iter; the compose is math-bound, so 8 taps ≈ free). The look:
glass identity restored, city visible in the panes, zero discrete
artifacts. Still gated by the OuterVec cost — the per-lane OuterVec env
compose remains the ticket to production.

**Live verdict v2 — the scripted-cam jumping was DIAGNOSED and FIXED, not
inherent:** consecutive-live-frame grids (multi-t snapshot = live ticking)
showed a white bloom sweeping the big facades every frame in pixel mode.
Two real defects, neither the reflection math:
(a) windows became ordinary G-buffer surfaces, so the deferred kernel
applied PER-PIXEL dynamic lighting — the sweeping searchlights painted
animated pools across facades that the authored vertex-Gouraud glass never
showed (pools vanish between building-scale verts). Fix: glass clones take
Diffuse=0 + Luminosity≈0.45 (authored texel×ambient) — emissive texture +
Fresnel reflection, i.e. the authored composite per pixel.
(b) the city face bake captured omni FLARE sprites — giant white blobs in
the stores that grazing Fresnel reflected near full strength. Fix: flares
muted during bakeBuildingCubeFaces (mirror-RTT trick), cube cache salt
bumped (benefits the paraboloid path's content too).
Tuning knobs exposed: --city_env_f0 (default 40), --city_env_lum (0.45).
After the fixes the live consecutive-frame stability matches the
paraboloid path. Remaining gap to production is unchanged: perf (the
OuterVec per-lane compose).

**Earlier live verdict (superseded, kept for the record):** free cam reads WELL (v2 rendering confirmed
correct); the scripted demo cam does not — (a) ~120 ms/frame ≈ 8 fps
turns the spline cam's fast moves/cuts into violent stutter (the same
perf gap, made visceral), and (b) the demo's city shots were COMPOSED
against the flat stylized `tex + refl/2` glass — per-pixel Fresnel
redistributes brightness (dim head-on, bright grazing) and breaks the
authored balance in a way no gain/F0 tweak restores. Experiment closed:
technically validated, wrong for the demo. Flag stays default-off;
paraboloid sheets (which keep the authored composite formula at full
speed) are production. Revisit only if a NEW scene is authored around
physical glass — then the OuterVec per-lane compose is the first task.

## Deviations

- **Separate cache FILE, not just a salted key** (slice C): the cache is a
  single-record file, so cube and equirect bakes sharing one path would
  overwrite each other on every flag flip. `cache/city_envmap_cube.bin` +
  format salt gives true coexistence; legacy `city_envmap.bin` loads
  unchanged when the flag is off.
- **CITY faces are supersampled, not direct-rendered** (slice C): faces render
  at the existing 1024² TmpSurf then box-downsample to 512² — reuses the
  legacy surface plumbing and gets 2× supersampling for free, at the cost of
  the same render resolution as before (the init-time "cheaper bake" claim
  from the plan mostly cancels out; measured a wash).
- **Deferred faceRes stays `env_refl_res/2`** (slice B, per plan suggestion):
  256² faces at the default 512 — memory comparable to the old 512² equirect.
