#pragma once

#include <cstdint>
#include <string>
#include <vector>

// CLI-driven scene snapshot harness. Renders one or more deterministic
// frames of a scene without the SDL window or audio path so we can diff
// native vs wasm output at the pixel level.
//
// Invocation (parsed by ParseSnapshotArgs):
//   DEMO --snapshot=city@t=1000,5000,10000 [--out=PATH]
//
// For each requested Timer value we drive one tick() of the City scene
// driver and dump VPage as PPM (color) and the Z-buffer as PGM.

struct SnapshotConfig {
    std::string scene;
    std::vector<int32_t> timestamps;
    std::string outDir = ".";
};

bool ParseSnapshotArgs(int argc, const char* argv[], SnapshotConfig& cfg);

int RunCitySnapshot(const SnapshotConfig& cfg, int xres, int yres);
int RunFountainSnapshot(const SnapshotConfig& cfg, int xres, int yres);

// Drives the greets scene through a Timer sweep and dumps one PPM per
// timestamp. Mirrors RunFountainSnapshot's structure — initializes City
// first (greets shares textures + SkyCube setup), then Initialize_Greets,
// then ticks the GreetsScene driver at each requested Timer value.
// Stderr trace of which materials get a baked normal map (the
// `[GREETS] baked +Y normal map for 'xxx'` lines) lands here so we can
// confirm what the scene init touched.
//
//   DEMO --snapshot=greets@t=100,500,1000,2000
//
// Default timestamp sweep covers the four greet rounds (intro, round1,
// round2, round3).
int RunGreetsSnapshot(const SnapshotConfig& cfg, int xres, int yres);

// Drives Glat through a deterministic Timer sweep and records per-tick
// state to <outDir>/glat_trace.csv. Useful for cross-platform / cross-
// commit comparison of animation behaviour.
int RunGlatTrace(const SnapshotConfig& cfg, int xres, int yres);

// Renders FillerTest's two-triangle quad through TheOtherBarry per seed,
// dumping VPage/Z to <outDir>/filler_t<seed>_color.ppm + _z.pgm. Used to
// reproduce rasterizer-edge / mask divergence between native and wasm in
// isolation from the city pipeline.
int RunFillerTestSnapshot(const SnapshotConfig& cfg, int xres, int yres);

// Reflection-direction diagnostic. Initialises City (with FDS_DEBUG_PANORAMA
// forced on so every per-building panorama is the synthetic quadrant-coloured
// test image), then positions the camera at six fixed points around one
// chosen building and renders one snapshot per camera pose. Each output PNG
// is named by the camera's offset direction relative to the building's
// centre. By reading off the colours that appear in the building's window
// reflections, we can verify which (eu, ev) the lookup formula picks for a
// given reflected ray, isolated from the (typically large) parallax errors
// that come from sampling a static cube-map at unrelated camera positions.
//
//   DEMO --snapshot=refltest [--out=PATH]
int RunReflectionTest(const SnapshotConfig& cfg, int xres, int yres);

// Cleaner reflection diagnostic with NO city geometry — builds a single
// outward-facing reflective cube programmatically (no FLD load, no bake)
// with the synthetic quadrant-coloured panorama as ReflectionTexture,
// then renders six snapshots from ±x/±y/±z. Each face has a known world-
// space normal; the camera position is known; the reflected ray for the
// face directly facing the camera is straightforwardly computable. The
// colour shown on each face directly identifies the (eu, ev) the lookup
// resolves to, with no other geometry to confuse the picture.
//
//   DEMO --snapshot=cuberefl [--out=PATH]
int RunCubeReflTest(const SnapshotConfig& cfg, int xres, int yres);

// Transparent-rendering test harness. Programmatic scenes (no FLD load)
// for isolating per-pixel deferred transparent issues: lighting drift,
// missing triangles, Z-occlusion failures, multi-layer blend.
//
//   DEMO --snapshot=xpartest@t=1   case 1: single transparent quad
//   DEMO --snapshot=xpartest@t=2   case 2: wall blocks transparent behind
//   DEMO --snapshot=xpartest@t=3   case 3: two layered transparents
//   DEMO --snapshot=xpartest@t=4   case 4: glass cube (TwoSided)
//
// Run with FDS_DEFERRED=0/1 to compare forward vs deferred.
int RunXparTest(const SnapshotConfig& cfg, int xres, int yres);

// Specular-highlight isolation harness. Programmatic scene (no FLD
// load): ground plane + UV sphere + tessellated wall with shiny
// materials (Specular > 0), one off-axis key omni so the highlight
// sits off-centre and slides predictably as the camera orbits. For
// each pose we render BOTH forward and deferred and dump both, so
// any per-pixel vs per-vertex divergence is observable directly in
// the file list. Glossiness sweeps via t=:
//
//   DEMO --snapshot=spectest          all three gloss cases
//   DEMO --snapshot=spectest@t=1      broad lobe   (Glossiness =   4)
//   DEMO --snapshot=spectest@t=2      medium lobe  (Glossiness =  32)
//   DEMO --snapshot=spectest@t=3      sharp lobe   (Glossiness = 128)
//
// Output: <outDir>/spec_g<gloss>_<pose>_<mode>.ppm
int RunSpecTest(const SnapshotConfig& cfg, int xres, int yres);

// Volumetric-cone isolation harness. Programmatic scene (no FLD load):
// one downward-pointing spotlight + a ground plane + a wall behind, so
// cones land on real geometry. Renders the same scene from a fixed set
// of camera poses around / inside the cone — side, above-apex, inside-
// looking-along, inside-looking-back, etc. — and dumps one PPM per pose
// so the ray-march math can be inspected from every angle the user has
// reported artifacts in (the dark elliptical cutoff inside the cone, the
// brightness drop at the surface boundary).
//
// REQUIRES: `--deferred --draw_cones` on the CLI (the harness uses the
// production Render() path; without those the cone pass is a no-op).
//
//   DEMO --deferred --draw_cones --snapshot=conetest [--out=PATH]
//
// Output: <outDir>/conetest_<pose>.ppm
int RunConeTest(const SnapshotConfig& cfg, int xres, int yres);

// Omni-halo test harness — single omnidirectional light with several
// camera poses (inside the range sphere, outside-close, outside-far,
// side-offset). Lets the halo math be inspected at the cases that
// matter: full screen coverage (camera inside sphere, where sphereDisc
// cull doesn't fire), small projected sphere (camera far, halo as a
// tight ball), edge-grazing rays.
//
// REQUIRES: `--deferred --omni_halo_strength=0.5` (or any non-zero
// value). Add `--no-vol_halo_analytic` to compare the ray-march
// fallback against the analytic atan integral.
//
//   DEMO --deferred --omni_halo_strength=0.5 --snapshot=halotest [--out=PATH]
//
// Output: <outDir>/halotest_<pose>.ppm
int RunHaloTest(const SnapshotConfig& cfg, int xres, int yres);

// Reproduce the user-reported wrong-direction reflection: stand outside
// the city over open water at four extremes, look back at the nearest
// reflective building. The water-facing side reflects what's behind the
// camera (sky/sea), so seeing buildings in the reflection is a bug.
//
//   DEMO --snapshot=seaside [--out=PATH]
int RunCitySeasideTest(const SnapshotConfig& cfg, int xres, int yres);

// Synthetic rasterizer benchmark. Repeatedly renders the FillerTest fixture
// and reports ms/iter + Mpx/s. Same fixture across native and wasm so the
// numbers compare apples-to-apples.
//
// Invocation:
//   DEMO --bench=raster                 -> default 200 iters, seed 0
//   DEMO --bench=raster@iters=1000      -> 1000 iters
//   DEMO --bench=raster@iters=500,seed=2-> custom iters + seed
//
// Under wasm: run via `node --cpu-prof DEMO_snapshot.js --bench=raster`
// and V8 dumps a .cpuprofile that opens in Chrome DevTools Performance.
struct BenchConfig {
    std::string kind;
    int iters = 200;
    int seed = 0;
    // Used by --bench=scene: which scene driver + Timer value to drive.
    std::string scene;
    int32_t ts = 0;
    // For --bench=scene: when tend > 0, the bench advances Timer from
    // ts to tend (inclusive) over `iters` iterations — useful for
    // measuring averaged frame cost across a stretch of scene playback
    // (e.g. ts=500, tend=2500, iters=200 → ~10ms per timer step across
    // ~2 sec of city). When tend == 0 (default), behaves like before
    // (repeats the same frame `iters` times).
    int32_t tend = 0;
    // Optional override of resolution (otherwise rev.cfg's value is used).
    // Used by --bench=flip to vary canvas size without editing rev.cfg.
    int xres = 0;
    int yres = 0;
};
bool ParseBenchArgs(int argc, const char* argv[], BenchConfig& cfg);
int RunRasterBench(const BenchConfig& cfg, int xres, int yres);

// Drives a real scene's tick() repeatedly at a fixed Timer value and reports
// total/mean ms. Used to attribute the wasm rasterizer cost to scene-shaped
// triangle workloads (small overdrawn triangles, varied UVs/lighting) versus
// the synthetic full-screen quad of --bench=raster.
//
// Invocation:
//   DEMO --bench=scene@scene=city,t=1961,iters=200
//   DEMO --bench=scene@scene=greets,t=600,iters=200
int RunSceneBench(const BenchConfig& cfg, int xres, int yres);

// FLIP pipeline microbenchmark — isolates the per-frame SDL stages we
// run inside V_Flip (unlock, bar fills, RenderCopy, RenderPresent, lock).
// Forces SDL_RENDERER_SOFTWARE so native numbers track the wasm path's
// cost model. Useful for iterating on the present path without rerunning
// the full demo.
//
// Invocation:
//   DEMO --bench=flip                       -> 3024x1696, 200 iters
//   DEMO --bench=flip@iters=1000            -> 1000 iters
//   DEMO --bench=flip@iters=500,xres=1920,yres=1080
int RunFlipBench(const BenchConfig& cfg, int xres, int yres);

// Greets dynamic-text-texture pipeline harness. Self-contained: doesn't
// load SCENES/GREETS.FLD or any 3D state, just fills a 256x256 CodeImage
// with a centered synthetic glyph, runs the wobbler (Code_GP) + smear
// (Smear_GP) + AlphaBlend chain N times with fixed scalex/scaley, and
// prints centroids of each intermediate buffer. Diff the output between
// native and wasm — centered input + symmetric pipeline should give
// centroids near (128.0, 128.0). Any deviation pinpoints which step
// breaks symmetry.
//
// Invocation:
//   DEMO --bench=greets-pipe              -> 100 iters, scalex=scaley=0
//   DEMO --bench=greets-pipe@iters=200
int RunGreetsPipeBench(const BenchConfig& cfg, int xres, int yres);
