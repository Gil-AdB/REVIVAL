# Session state — refactor/frame-state-shadow

**Read this first when starting a new session.** Captures what landed,
what's open, what's broken, and which commands reproduce each state
so we can resume cleanly after context compaction.

Last updated: 2026-05-17

---

## Branch + push status

- Branch: `refactor/frame-state-shadow`
- Pushed to: `origin/refactor/frame-state-shadow`
- HEAD: `be13e17` (deferred: replace triangle cone overlay with screen-space ray-march)
- **Not merging to master** — all dev work continues on side branches per user
  direction (2026-05-17).
- ctest: both smoke tests pass (`smoke_city_forward`, `smoke_greets_deferred_shadows`)
- 162+ commits ahead of master.

## What just landed (recent → older)

### Volumetric spotlight cones (this session, in order)

1. **`cc17d65`** — extracted greets's triangle cone overlay into
   `DEMO/SpotlightCones.{h,cpp}` with flags. Built but underwhelming
   visually + slow.
2. **`be13e17`** — replaced triangle overlay with screen-space
   ray-march in `FDS/RENDER/DeferredLighting.cpp::Render_VolumetricCones`.
   Runs after `Render_DeferredLighting`, reuses per-tile spot SoA,
   integrates N=6 samples along ray-cone intersection segment.
   Triangle code deleted; `MakeSpotLight` helper kept.

### Earlier this session

- `cd6a7ca` fix(greets): the `Polys` shadowing bug (local int32_t hid
  the global, broke shadow per-light buffer sizing). Found with ASan
  in 2 minutes after sub-agent's static-read missed it.
- `04b3f07` city: clamp ill-conditioned cv-pull divisor in reflective
  panorama lookup (memory `project_cv_pull_instability`).
- `761aeaa` material: reorder Material fields so hot per-pixel reads
  fit in one cache line (no measurable bench delta but cleaner layout).
- `b353353` deferred + mekalele: replace 14 sites of `1.0f/std::sqrt`
  with `fast_rsqrt`. `fast_rsqrt` hoisted to `FDS/FILLERS/SimdHelpers.h`.
- `cac6411` + `fc77a93` deferred: fold half-vector renormalize into NdotH
  dot — saves 3 muls/pixel × 3 spec paths. **-2.3ms on greets**.
- `753af3f` deferred: replace `std::pow` libm fallback with
  `fastPow2(g*fastLog2(NdotH))`. Greets uses templated path so no bench
  delta; benefits any non-templated gloss.
- `f57bfdd` fix(frustrum): build LogTable/ExpTable once per thread,
  not per Clipper. **-2.7ms on greets** (60k transcendentals/frame ✂).
- `2137c54` made the LUT a single read-only global (vs thread_local).

### Perf cumulative this session

Greets@t=2500 bench: started ~30.9 ms/iter, currently ~26.3 ms/iter.
**−4.6 ms (−15%)** across LUT fix + half-vec fold + various.

### Other session work

- ASan rediscovered as the right tool for memory mysteries.
- LSP findReferences habit established for cross-TU global edits.
- `--no_vsync` flag added; confirmed V_Flip 2.7ms is real Metal upload
  (not vsync), still ~2ms grabbable, deferred.
- Initialize_City coupling resolved (was the Polys shadow bug).
- City spotlights gated behind `FDS_CITY_TEST_SPOTS` (off by default).
- clangd wired (`.clangd` config, self-contained headers).
- Native ctest in CI (.github/workflows/native-test.yml).

---

## What's broken / in progress

### 🐛 Volumetric cone tile-stripe artifact (user-reported, NEXT TO DEBUG)

User reported (2026-05-17): "some tiles have it and some don't". The
volumetric cone pass shows visible per-tile stripes — cones appear in
some tiles, missing from adjacent tiles, producing a 6x4 tile-boundary
checker.

**Likely root cause**: each tile's per-tile spot SoA is built from the
*spot's screen-space bounding sphere* overlapping the tile (existing
deferred-lighting culling). But the cone *volume* (the 3D cone of the
spotlight) can extend through tiles whose sphere-check excluded the
apex. So: cone visible in tiles where the apex's bounding sphere
overlapped → missing from tiles where it didn't, even when the cone's
3D volume crosses both.

**Fix candidates**:
1. Per-tile cone culling that uses the cone's screen-space bounding
   rect (extends across all tiles the cone volume could cover), not
   the apex's bounding circle.
2. Cheapest hack: iterate ALL spots per tile for the volumetric pass,
   skip the per-tile cull entirely. Fine if N < 10.
3. Add cone screen-rect to TileLights as a second pass over the
   light list during build, gated on isSpot.

**Where**: `FDS/RENDER/DeferredLighting.cpp` — `Render_VolumetricCones`
reads from `g_deferredCtx.tileLights[tileIndex]`. The per-tile build
is in `buildTileLightLists` (~line 250); spots are flagged with
`isSpot`. Need to widen what gets into each tile's spotlight subset.

**Reproduce**:
```sh
cd Runtime
./DEMO --deferred --draw_cones --city_test_spots
# fly the city with --no_vsync if not pressed for time
# or snapshot: --snapshot=city@t=1500 --out=/tmp
```
Open the output and look for tile-aligned discontinuities.

### Performance impact (user-reported)

Same session: "Really affects performance in City". The ray-march pass
is N pixels × N spots × N=6 samples × scalar math per sample. Cheap
per-sample but high pixel count + iterating all in-tile spots per
pixel. Profile pending. Likely candidates for optimization:
1. Early-out by tile-cone bbox (only iterate spots whose 2D bbox
   overlaps the actual pixel).
2. Vectorize the inner per-sample loop (similar to the deferred
   lighting kernel's per-pixel-vec path).
3. Lower N_SAMPLES for distant spots.
4. Quarter-rate render: only ray-march every 2x2 pixel, dilate.

Worth profiling FIRST with `sample` (see `memory/reference_asan_build`
and earlier session profile output) to find the actual hot spot
before guessing.

### Per-spotlight density / shape tuning

Current City setup: 6 hardcoded streetlights in CITY.CPP gated by
`FDS_CITY_TEST_SPOTS`. Position/angle/color tuning is awkward — would
benefit from runtime knobs or moving the test config out of CITY.CPP.

### Test-snapshot output dir pollution

Local working tree has many stale .ppm/.png debug artifacts under
Runtime/* (City snapshots, seaside sweeps, cube refl, etc.). Should
extend `.gitignore` to cover Runtime/seaside/, Runtime/snap-*,
Runtime/tasks_artifacts/, build-asan/. (See `feedback_git_add_all_forbidden`.)

---

## Key flags + commands

### Cones
- `--draw_cones` or `FDS_DRAW_CONES=1` — enable volumetric cone pass
- `--cone_strength=0.05` (default; range ~0.01-0.5)
- `--city_test_spots` — install 6 city test spotlights

### Other
- `--no_vsync` — for clean V_Flip cost measurement
- `--bench=scene@scene=greets,t=2500,iters=1000` — perf bench harness
- ctest: `ctest --test-dir build`

### Standard test invocations
```sh
# greets snapshot
cd Runtime && FDS_DEFERRED=1 FDS_SHADOWS=1 ./DEMO --snapshot=greets@t=2500 --out=/tmp

# city snapshot with cones
cd Runtime && ./DEMO --deferred --shadows --draw_cones --city_test_spots --snapshot=city@t=1500 --out=/tmp

# interactive city (fly with arrows / Tab for free cam)
cd Runtime && ./DEMO --deferred --draw_cones --city_test_spots
```

---

## Memory entries created this session (read with /memories)

- `feedback_use_feature_flags_for_hot_toggles` — never `getenv` in hot loops
- `feedback_lsp_for_diff_review` — `findReferences` on aliased globals before editing
- `feedback_git_add_all_forbidden` — explicit file names only; Runtime/ has hundreds of MB of debug junk
- (Updated) `reference_asan_build` — reach for ASan BEFORE deferring a memory-mystery as "low ROI"

---

## Roadmap status (see docs/ROADMAP.md)

Top picks ordered by ROI when last revised:

1. ~~Profile first~~ ✓ Done
2. ~~Vec-path shadowAtten~~ ✓ Documented (vec path off by default)
3. ~~Per-spotlight cone primitive for fog~~ ✓ Done as ray-march
4. **City cv-pull stability fix** — done at `04b3f07`, partial fix
   (clamp ill-conditioned divisor). Still some discontinuities visible
   but acceptable per user.
5. **StaticLighting visibility ray-trace** — user-flagged, not started
6. **V_Flip native-Metal investigation** — ~2ms grabable, not started
7. **HDR + bloom** — large project, scoped in roadmap
8. **Per-texel lightmaps** — large project, scoped in roadmap

## Things specifically deferred / noted to revisit

- §9d in roadmap: City first-frame screen-wide jump (intermittent,
  one-time-only, never reproduces on F1 rewind). Likely cube-map bake
  or StaticLighting timing.
- §9b in roadmap: V_Flip ~2.7ms is Metal upload, not vsync. Worth
  investigating; wasm WebGL2 bypass doesn't transfer.

---

## Working-tree state (uncommitted)

The branch is clean source-wise. Working tree has uncommitted asset
swaps + debug snapshots:

- `Runtime/SCENES/GREETS.FLD` — swapped to 1998 small version
- `Runtime/TEXTURES/PBRK34.JPG`, `P_PAVE.JPG` — swapped to 1998 small versions
- `Runtime/rev.cfg` — local tweaks
- many `Runtime/*.ppm`, `Runtime/snap-*.png`, `Runtime/seaside/`, `Runtime/tasks_artifacts/` — debug output, never to be committed
- `build-asan/` — local asan build, never to be committed

These survive across sessions; the user manages them locally. **NEVER
`git add -A` / `git add .`** — explicit file names only.
