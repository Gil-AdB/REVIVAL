# SESSION STATE — DeferredLighting.cpp split

Resume point for the **single-phase split** of FDS/RENDER/DeferredLighting.cpp
(9590 lines at merge 006d9f9). Read memory `deferred-split-plan` for rules.
(Previous content — the fog branch checkpoint — is merged work; git history.)

## Region map (line numbers at 006d9f9)

| lines | content | target file |
|---|---|---|
| 1-110 | includes, externs, renderns, stats statics | stays / DeferredCommon.h |
| 110-290 | ViewLightsSoA, TileLights, DeferredLightingCtx, DEFERRED_* consts | **DeferredCommon.h** |
| 291-794 | computeTileDepthBounds, computeMirrorPresenceGrid, sphereOutsideCone, tileChunkSphere, buildTileLightLists, g_stripLights, buildStripLightLists | **DeferredLightLists.cpp** |
| 795-1011 | sq/fmul/pow_squaring/pow_glossClass, run_vec_spec_loop, vec/checker/quarter enables | **DeferredSurfaceKernel.cpp** |
| 1012-1201 | PixelLightmap, resolvePixelLightmap, CubeAttenFlags, resolveCubeAtten | **DeferredShadowSampling.h** (inline) |
| 1202-2208 | Render_DeferredLighting_Tile (scalar kernel; 2D/PCF + mirrored clone tap) | **DeferredSurfaceKernel.cpp** |
| 2209-2639 | FastFog fwd decls, Render_DeferredTransparentLighting_Tile, deferredUnifiedTbrEnabled, RenderXparClumpInStrip | **DeferredSurfaceKernel.cpp** (xpar) |
| 2640-3568 | Tile_OuterVec, TileFill (checker fill), Render_DeferredLighting() orchestrator | **DeferredSurfaceKernel.cpp** |
| 3931-4040 | Render_DeferredFogPass_Tile, rsqrt_nr_x8 | rsqrt → Common.h; fog tile → Volumetric |
| 4041-5467 | Render_VolumetricCones_Tile (clone gate, bounce clamp, per-segment taps), VolProf, Render_VolumetricCones | **DeferredVolumetric.cpp** |
| 5468-6242 | Render_OmniHalos_Tile / Render_OmniHalos | **DeferredVolumetric.cpp** |
| 6243-6460 | Render_DeferredSkybox, Render_DeferredFogPass | **DeferredVolumetric.cpp** |
| 6461-9590 | fastExp/fogAntideriv, FastFogParams, blobField, volSpotShadow, lightAttenAt/lightRayClip, fogInscatter, fogAtDepth/Pixel, FastFog tiles/refine/composite, froxels, FastFog_SetReflectionZ/BeginFrame | **DeferredFastFog.cpp** (fog half is ~3k lines — own file) |

## Rules (binding — from deferred-split-plan memory)
- Whole-function VERBATIM moves only. No hot-loop edits (two prior perf regressions from reorganizing).
- One region per commit. Gate per commit: build; pixel-diff teleporter pose
  `FDS_GREETS_CAM="-0.069,4.917,-5.214,-0.097,-0.342,0.935"` t=700 full config
  (--greets-mirror --mirror-rtt --shadows); mirrortest 8/8; one city fog pose.
- Demo runs ONLY with `SDL_VIDEODRIVER=dummy` (never pop a window on the user's desktop).
- -flto=thin should preserve cross-TU inlining — verify with USER-run idle bench at the end.
- FDS/CMakeLists.txt source list is explicit — add each new file.
- Order: DeferredCommon.h → LightLists → ShadowSampling.h → Volumetric → FastFog → SurfaceKernel (biggest last).
- Cross-region statics (g_deferredCtx, g_stripLights, VolProf state, FastFog gFr*/gSS* state, g_coneAnalyticHits) need extern decls in DeferredCommon.h — grep each before moving its region.

## Open threads
- TSan sweep in background (task bps278tey → /tmp/tsan_sweep*); expect at least the known BSphereScreenPos race (Transform.cpp ~963) if instrumentation bites.
- Shadow tile flicker (memory: shadow-tile-flicker-hunt): next = pose-aware probe + tile-flip dump; lights 2 & 28 confirmed flapping live.
- Bounce dots through walls (memory: mirror-beam-reflections-design): near-clipped bounce shadow maps + half-space clamp redo (transform convention suspect; first attempt reverted). User relief: --no-mirror-bounce.
- Tune-server precedence assert (setDefault < ParamScript < CLI < console).
