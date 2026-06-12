# SESSION STATE — DeferredLighting.cpp split: DONE (2026-06-12)

The single-phase split of FDS/RENDER/DeferredLighting.cpp (9590 lines at
merge 006d9f9) landed as six commits, 1dcfd4e..3cac793, all verbatim
function moves:

| file | lines | contents |
|---|---|---|
| DeferredCommon.h | ~360 | ViewLightsSoA, TileLights, DeferredLightingCtx (+extern g_deferredCtx), DEFERRED_* consts, sphereOutsideCone/tileChunkSphere, rsqrt_nr_x8, VolProf/VolProfScope (+extern g_volProf), light-list decls, FastFog xpar-hook decls |
| DeferredShadowSampling.h | 219 | PixelLightmap, resolvePixelLightmap, CubeAttenFlags, resolveCubeAtten (all static inline) |
| DeferredLightLists.cpp | 478 | computeTileDepthBounds, computeMirrorPresenceGrid, buildTileLightLists, buildStripLightLists, g_stripLights |
| DeferredVolumetric.cpp | 2585 | Render_DeferredFogPass(+Tile), Render_VolumetricCones(+Tile), Render_OmniHalos(+Tile), Render_DeferredSkybox, VolProf_Tick, g_volProf def, cone counters |
| DeferredFastFog.cpp | 3139 | fastExp/fogAntideriv, FastFogParams, blobField, SS+froxel passes, composite, FastFog_SetReflectionZ/BeginFrame, Render_DeferredFastFog, Render_ScreenSpaceRain, Render_DeferredVolumetric (unified) |
| DeferredSurfaceKernel.cpp | 3046 | scalar tile kernel, xpar peel template + Front/Back wrappers, OuterVec, TileFill, gloss squaring dispatch, RenderXparClumpInStrip, Render_DeferredLighting orchestrator, g_deferredCtx def (git mv of the monolith remainder — history preserved) |

Linkage changes (the ONLY non-verbatim edits): light-list builders +
g_stripLights, the four FastFog xpar hooks (XparActive/SampleGrid/
SSActive/SSSample) de-static'd; VolProf hoisted out of its anonymous
namespace into DeferredCommon.h (one shared g_volProf instance, as
before the split).

**Gate (passed on every commit):** build + byte-identical PPMs vs the
pre-split baseline for (a) city fog `--fast_fog --snapshot=city@t=280`,
(b) greets teleporter `FDS_GREETS_CAM="-0.069,4.917,-5.214,-0.097,
-0.342,0.935" --deferred --greets-mirror --mirror-rtt --shadows
--snapshot=greets@t=700`, (c) `--scene-mirrortest --mirrortest-multi-dump`
8/8 poses. Determinism of all three verified run-to-run before trusting
the gate. Gate script: /tmp/split_gate.sh (session-local).

## Remaining
- **Idle-machine bench NOT yet run** — -flto=thin should preserve
  cross-TU inlining but this is unverified. Bench greets full sweep +
  realistic-city vs pre-split (4a64c3c) when the machine is idle.

## Open threads (carried over)
- Shadow tile flicker (memory: shadow-tile-flicker-hunt): next =
  pose-aware probe + tile-flip dump; lights 2 & 28 confirmed flapping live.
- Bounce dots through walls (memory: mirror-beam-reflections-design):
  near-clipped bounce shadow maps + half-space clamp redo. User relief:
  --no-mirror-bounce.
- Tune-server precedence assert (setDefault < ParamScript < CLI < console).
- TSan sweep results in /tmp/tsan_sweep* (task bps278tey) if still around.
