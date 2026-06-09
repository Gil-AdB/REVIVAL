# fast_fog froxel volumetrics — design & plan

Replaces the screen-space per-pixel blob ray-march (`blobFieldTau` +
`fogInscatterSegment`) with a **view-frustum-aligned 3D froxel grid**. Motivation
(from the screen-space diagnosis): the per-pixel march paints fog onto the
surface a ray hits (reads as a marble texture on the ground, can't float in
air), speckles at grazing angles (importance shadow taps cluster in the
ill-conditioned near-lamp region), and its cost scales with screen coverage
(city ~20 fps). Froxels fix all three: fog is volumetric by construction, there
is no per-pixel march, and cost is O(froxels) — decoupled from screen res.

Gated behind `--fast_fog_froxel` (default off) so the screen-space path stays
intact for A/B until froxels reach parity+.

## Grid

View-frustum-aligned grid sized `nx × ny × nz`, default ~**160 × 90 × 128**
(tunable via flags). XY can be coarse — fog is low-frequency; the composite
trilinearly upsamples. Froxel `(ix,iy,iz)`:

- `ix,iy` → a view ray direction (screen NDC at the froxel-column center →
  `X=(sx-CntrEX)/FOVX`, `Y=(CntrEY-sy)/FOVY`, like `fogAtPixel`).
- `iz` → a view-space depth `z`. **Exponential slice distribution**
  `z(iz) = near · (fogFar/near)^((iz+0.5)/nz)` so near slices are thin (detail
  where the eye is) — standard for froxel fog. (Start linear if simpler, switch
  to exp once it works.)
- World center: `camWorld + viewToWorld · (X·z, Y·z, z)` — reuse
  `g_deferredCtx.viewToWorld` + `cameraWorld` (same as the height-fog path).

## Passes

1. **Populate** (`Froxel_Populate`, tiled, embarrassingly parallel per froxel):
   per froxel compute
   - **density** = the blob field sampled **once** at the froxel world center
     (reuse `cellHash`/trilinear + the `(val-0.45)*1.8` remap + quintic — but a
     single point sample, **no DDA march**). Or smooth-slab density.
   - **extinction** `σ_t = sigma · density` (× height/distance falloff).
   - **in-scatter** `Lscat` = Σ lights `lightAttenAt(froxelCenter) · vis` × color
     (reuse `lightAttenAt` + `volSpotShadow`, one tap per light per froxel — no
     per-sample loop, no importance sampling → no grazing speckle).
   Store `scatRGB` (= Lscat · density · inscatter) and `ext` (= σ_t) per froxel.

2. **Integrate** (`Froxel_Integrate`, parallel per XY column): front-to-back
   along `iz`, accumulate per froxel
   - `T_i = exp(-Σ_{j≤i} ext_j · Δz_j)` (transmittance camera→slice i),
   - `acc_i = acc_{i-1} + scat_i · T_{i-1} · Δz_i` (Beer-Lambert in-scatter).
   Store `accRGB` and `T` per froxel (in place).

3. **Composite** (`Froxel_Composite`, tiled per screen pixel): pixel's view-z →
   fractional slice `iz` → **trilinear** sample `accRGB`,`T` from the grid →
   `out = out · T + accRGB`. Replaces `fogComposite`. Trilinear in x,y,z gives a
   smooth low-res upsample (fog is low-freq, so the coarse grid is invisible).

## Reuse (all already in DeferredLighting.cpp)

`cellHash` + trilinear noise, the density remap/quintic, `lightAttenAt`,
`volSpotShadow` (PCF), the light SoA `g_deferredCtx`, `viewToWorld`/`cameraWorld`,
the tile-job runner (`runTiles`), the FastFogParams plumbing.

## Cost sketch

160×90×128 ≈ 1.84M froxels × (1 blob sample + 1 tap/light). vs screen-space
0.5M px × ~30 march-cells. ~8× less work at this grid; grid res is the knob.
Memory: ~1.84M × (scatRGB+ext = 16 B) ≈ 30 MB (coarser grid → less). Populate is
the cost; integrate/composite are cheap.

## Flags (FeatureFlags.def, atmos)

- `fast_fog_froxel` (bool, default 0) — enable the froxel path (overrides the
  screen-space blob march).
- `fast_fog_froxel_x` / `_y` / `_z` (int) — grid dims (default 160/90/128).

## Build order (validate each)

1. Grid struct + buffers + the `--fast_fog_froxel` flag + dims. Allocate/size.
2. **Populate** (density + extinction only, no in-scatter) + **integrate** +
   **composite** → verify volumetric fog appears (slab/blobs fill air, no
   marble-on-surface), A/B vs screen-space density.
3. Add **in-scatter** per froxel (lights + shadow tap) → verify glow, no grazing
   speckle (the whole point), A/B vs screen-space inscatter.
4. Exponential depth slices; tune grid res for quality/perf; bench vs 30 ms.
5. Half-res composite / threading polish.

## Known design choices to revisit

- **Depth slice count vs near-field detail** — 128 linear may band near camera;
  exp distribution fixes it.
- **Temporal stability** — coarse XY grid can shimmer under motion at fog/shadow
  edges; froxel fog usually pairs with a small temporal reprojection blend. Defer
  until static quality is right; note it for the demo's moving camera.
- **Trilinear composite across depth discontinuities** — at a surface silhouette
  the froxel behind vs in front differ; may need a depth-aware sample (like the
  current bilateral upsample) to avoid halos. Defer; check at step 3.
