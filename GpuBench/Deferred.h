// Deferred path for the GPU benchmark: G-buffer -> cube shadow bake -> PBR
// lighting -> ACES tonemap, with PER-PASS GPU timestamps.
//
// Kept separate from GpuBenchMain.mm's Phase 2 albedo arm so that number stays
// reproducible. See docs/GPU_BENCHMARK_PLAN.md §3.

#pragma once

#import <Metal/Metal.h>

#include "SceneIngest.h"

#include <cstdint>

#include <string>
#include <vector>

namespace gpubench {

struct DeferredOptions {
    int  warmup = 60;
    int  iters = 300;
    bool shadows = true;
    int  staticShadowRes = 512;    // GREETS.CPP greets_omni_shadow_res
    int  movingShadowRes = 128;    // GREETS.CPP greets_moving_omni_shadow_res
    float exposure = 1.0f;
    int  viz = -1;                 // -1 = normal render; else fs_viz mode
    // Re-bake EVERY cube every frame instead of only the moving ones. Off by
    // default because greets caches static omni cubes (Omni_StaticShadow); on,
    // this measures the full cold bake.
    bool rebakeAll = false;
    // How many passes to run: 1 = G-buffer only, 2 = +lighting, 3 = +tonemap.
    // The per-encoder timestamps OVERLAP on Apple GPUs (a pass's vertex stage can
    // start before the previous pass's fragment stage retires), so they are upper
    // bounds and do NOT sum to the frame. Differencing clean whole-frame
    // GPUStartTime/GPUEndTime intervals across stage counts is the trustworthy
    // decomposition -- same discipline as the albedo arm's --no-draw floor.
    int  stages = 3;
    // MEASUREMENT-ONLY. See the shader comment on lightRangeScale.
    float lightRangeScale = 1.0f;
    int   vizLight = -1;          // -1 = all lights; else isolate this index
    // DIAGNOSTIC. Read the baked cube for this light back to the CPU and print
    // the raw stored floats (per face: NaN/Inf count, cleared count, min/max, and
    // the decoded world distance), then write a 3x2 face atlas PPM. This exists
    // because "the tap returns shadowed everywhere" has many possible causes and
    // exactly one of them is "there is nothing valid in the cube" -- which is
    // cheap to settle by LOOKING instead of adjusting conventions.
    // Omni flare sprites. ON: they are what the DEMO reference's bright pools
    // actually are (additive 256^2 procedural sprites, FILLERS.CPP), not omni
    // surface lighting. flareGain is a DIAL, default 1.0 = the CPU's own scale.
    bool  flares = true;
    float flareGain = 1.0f;
    // Parity dial with the CPU's --no-nmap (FeatureFlags no_nmap): skip the
    // normal-map perturbation in the G-buffer so both arms can be compared on
    // geometric normals alone. Diagnostic — the shipped look keeps it true.
    bool  nmap = true;
    // WORKLOAD PARITY (docs/GPU_BENCHMARK_PLAN.md §6.2 "other known gaps").
    // cull: BACKFACE-cull the main G-buffer pass, as Transform_Objects does
    //   (Transform.cpp:2434). The shadow bake deliberately does NOT cull —
    //   shadow_backface_cull defaults 0 because single-sided walls must still
    //   occlude — so matching that is parity, not an omission.
    // shadowCull: per-cube-FACE frustum cull of shadow batches, the analogue
    //   of the CPU's per-pass mesh cull. Both default ON so the comparison
    //   table is not measuring work the CPU never does; --no-cull /
    //   --no-shadow_cull price them.
    bool  cull = true;
    bool  shadowCull = true;
    // Headless CPU-side per-frame profile (animation vs upload), N iters.
    // 0 = off. Exists so the comparison table can carry the CPU-side split
    // WITHOUT a --window run (visible runs are the user's to launch).
    int   cpuProf = 0;
    // Bloom. greets sets bloom ON with bloom_intensity 2.0 (GREETS.CPP:1168-9)
    // and the global bloom_threshold default is 200 in the CPU's linear 0-255
    // radiance scale — divided by 255 on the way into this arm's 0..1 buffer.
    // Bloom / hdr_linear are PER-SCENE RUN FLAGS, not global truths. Both
    // default 0 in FeatureFlags.def (:344, :343) and the only setDefaults in
    // the tree are greets' (GREETS.CPP:1177-1185). fountain sets NEITHER — its
    // shipping recipe is `--deferred --hdr` and nothing else — so applying
    // greets' stack to it renders a different tone curve AND a bloom the CPU
    // never draws. `*Explicit` records whether the user overrode the default on
    // the command line, so the scene-derived value only fills an untouched dial.
    bool  bloom = true;
    bool  bloomExplicit = false;
    float bloomThreshold = 200.0f / 255.0f;
    float bloomIntensity = 2.0f;
    // hdr_linear: albedo^2 * light with an sqrt encode on the way out
    // (DeferredSurfaceKernel.cpp:2618-2631 + Hdr.cpp:847-851) vs the gamma
    // path's albedo^1 * light with no encode (:2587). MEASURED that fountain
    // ships the gamma path: adding --hdr_linear to the pinned reference recipe
    // CHANGES the image (md5 8db68ccb -> ded91a13), so the pin is the gamma one.
    bool  hdrLinear = true;
    bool  hdrLinearExplicit = false;
    // VOLUMETRIC SPOT CONES — the disco beams in the air. PARITY: greets turns
    // them on per-light (GreetsDisco.cpp sets Omni_ForceVolCone on all ten
    // spots, which bypasses the scene-wide draw_cones default of 0) and raises
    // cone_strength from the global 0.05 to 1.2, because 0.05 is city-scale and
    // invisible over greets' ~10-unit beams. Under --hdr the CPU scales the
    // density by hdr_glow_scale (0.25), which this arm reproduces.
    // TRANSPARENT SURFACES. The CPU routes Mat_Transparent / Mat_Additive faces
    // away from the deferred kernel entirely (RenderInner.cpp:294-296, :317-318)
    // and composites them afterwards through the forward transparent kernel +
    // the front/back depth peel. ON is parity; --no-xpar prices the pass and
    // restores the pre-2026-08-08 behaviour of shading them as opaque.
    bool  xpar = true;
    // Peel ENCODER MERGING (--no-xpar_merge prices it). Merging is restricted to
    // clumps whose screen footprints are disjoint, so it cannot change a pixel;
    // off, each clump gets its own encoder pair per (side, pass) — the
    // pre-2026-08-08 scheduling.
    bool  xparMerge = true;
    // PARTICLE REPLAY (--pcl=PATH). fountain's 8,250 water sprays live in
    // Scene::Pcl[], which DEMO fills and FDS never does, so they are not
    // reachable by ANY ingest — see GpuBench/ParticleReplay.h for the proof,
    // the file format and the DEMO-side dump this reads. Empty = no particle
    // pass at all, which is the state fountain has been in.
    std::string pclPath;
    // ---- GPU PARTICLE SIMULATION (--pcl_sim) ------------------------------
    // The replay above is an ORACLE and only an oracle: it needs a recording
    // made for the exact pose being rendered, so a free-fly window — where the
    // pose is whatever the user just flew to — can never have a spray from it.
    // This runs the motion model of DEMO/FOUNTAIN.CPP's Particle_Kinematics as
    // a Metal compute kernel instead (shaders/deferred.metal, cs_pcl_sim), so
    // the window has live, animated spray at any pose and any time.
    //
    // SAY THIS OUT LOUD WHENEVER IT IS ON: with the sim active THE ARM IS NOT AN
    // ORACLE FOR PARTICLES. A GPU integrator does not track the CPU's stateful
    // RAND_15() history, so the individual particles diverge from frame one.
    // The shape, spread, density, lifetime, size and colour distribution are
    // ported term for term and are what a visual comparison can use; a per-pixel
    // one cannot. Every OTHER quantity in the arm keeps its oracle property —
    // the sim writes only the particle instance buffer and nothing else reads
    // it. For a bit-comparable particle pass use --pcl=PATH, which is unchanged
    // and is what the comparison tables keep using.
    //
    // DEFAULT: ON for the interactive window on fountain (so flying it just
    // shows the spray), OFF everywhere else — every offscreen md5 in this
    // document was taken without it and stays valid. `--pcl_sim` forces it on
    // offscreen, `--no_pcl_sim` forces it off anywhere. --pcl wins if both are
    // given: a dump is the more precise instrument.
    int   pclSim = -1;              // -1 auto (window+fountain), 0 off, 1 on
    // Seconds of scene time to spin the sim through before the first rendered
    // frame. Nothing is authored about particle state at a given t — the CPU
    // reaches steady state by having simulated from scene start — so the sim
    // replays the wall of scene time immediately BEFORE the render target time
    // at a fixed step. 5 s covers the longest particle lifetime (outer ~4 s,
    // inner 2.5 s), so the population is at equilibrium and the emitter phase
    // history is the real one.
    float pclSimWarm = 5.0f;
    // Fixed step for the warm-up, in seconds. 1/60 matches the demo's own tick
    // rate closely enough that the ballistics land in the same place, and being
    // FIXED is what makes a --reanimate render reproducible.
    float pclSimStep = 1.0f / 60.0f;
    // ImageSize at the sprite blit. fountain sets it to 10.0 (FOUNTAIN.CPP:2844)
    // and the sprite half-extent is ImageSize*RZ*PerspX*FlareSize*2, so it is
    // load-bearing for sprite SIZE. GpuBench does not link DEMO and cannot read
    // the global, so it is named here and overridable with --pcl_image_size=.
    float pclImageSize = 10.0f;
    // --tex_point: POINT-sample every albedo/material texture with NO mip
    // filtering, instead of this arm's trilinear + 8x anisotropic default.
    // MEASUREMENT ONLY. The CPU rasterizer point-samples and selects its mip by
    // clipper subdivision, so the two arms do not resolve the same detail on a
    // steeply foreshortened surface. This makes that difference priceable
    // instead of arguable. It is NOT a fidelity improvement — point sampling
    // aliases — and it is not the default.
    bool  texPoint = false;
    // Sprite-vs-glass ORDER. The CPU walks ONE back-to-front list holding both
    // sprites and transparent clumps, so a sprite BEHIND glass is attenuated by
    // it and one IN FRONT is not; a single whole-spray pass cannot be both.
    // Drawing after the peel attenuates NO sprite, drawing before attenuates
    // EVERY sprite, and the CPU is bracketed by the two.
    //
    // DEFAULT = after, because it is MEASURED closer at every pose tried
    // (t=1500/2500/3500: whole-frame mean |dY| 15.906/12.048/19.451 before vs
    // 14.320/10.595/17.276 after; on the 3.3-5.3 % of the frame the ordering
    // can touch, 61.7/60.1/61.5 before vs 13.5/20.6/20.6 after, and after is
    // the closer of the two on 98.7/85.9/91.0 % of those pixels). Nothing
    // previously recorded moves with this default: the particle pass only
    // exists when --pcl is given, which is new. --pcl_before_xpar restores the
    // other bracket so the choice stays priceable.
    bool  pclAfterXpar = true;
    // xparPeelPassesEffective() (DeferredSurfaceKernel.cpp:3600): an explicit
    // flag wins, otherwise Scene::XparPeelPasses (greets 1, fountain 4).
    int   xparPeelPasses = 0;       // 0 = use the scene's own value
    // ---- CONDUCTOR PARITY DIALS — MEASUREMENT ONLY, both default OFF -------
    // These switch the GPU's metal shading to the CPU's *HDR-frame* semantics
    // so the two divergences docs/SHADING_CONTRACT.md calls D1 and D2 can be
    // priced in pixels instead of argued from source.
    //   cpuMetalDiffuse: do NOT kill diffuse on conductors. The CPU applies
    //     `fdB *= (1-metal)` at DeferredSurfaceKernel.cpp:2521-2523 to `fdB`,
    //     the LDR combine, while the --hdr_linear frame is built from the RAW
    //     accumulator `lB` (:2625) — so the shipped HDR frame keeps FULL
    //     diffuse on a conductor.
    //   cpuMetalTint:    tint the metal highlight by the GAMMA texel, as the
    //     CPU does at :2541-2546 (`sB *= 1-m + m*texB/255`), instead of by the
    //     squared/linear albedo (deferred.metal:556).
    // Turning either ON makes the GPU LESS physically correct on purpose.
    bool  cpuMetalDiffuse = false;
    bool  cpuMetalTint = false;
    //   envSkipAnimated: keep ANIMATED meshes out of the env-reflection probe
    //     bakes, which is what the CPU does unconditionally
    //     (FDS/RENDER/EnvBake.cpp:311 sets g_envBakeSkipDynamic, and
    //     FDS/RENDER/Transform.cpp:1274 folds it into `inStaticBake`, which at
    //     :1560 `continue`s past every mesh `isDynamicForBake` calls dynamic).
    //     On greets that is the entire mech — Hull.lwo, Hull2.lwo and the four
    //     leg meshes — so the CPU's cockpit probe holds the EMPTY ROOM while
    //     this arm's holds the mech's own hull, barrels and legs. MEASUREMENT
    //     ONLY, default OFF: ON is CPU parity, OFF is what every pinned md5
    //     in this arm was recorded with.
    bool  envSkipAnimated = false;
    bool  cones = true;
    // GREETS.CPP:1187 setDefault(cone_strength, 2.0f). GreetsDisco.cpp:434 also
    // setDefaults it, to 1.2 — but setDefault only skips when the flag was set
    // EXPLICITLY (FeatureFlags.h:148), so the later of the two wins, and
    // GreetsApplyRunDefaults runs after Initialize_Greets' BuildDiscoBall.
    // 1.2 was this arm's value until that ordering was checked.
    float coneStrength = 2.0f;
    float hdrGlowScale = 0.25f;     // FeatureFlags.def:337
    int   volNSamples = 4;          // FeatureFlags.def:256 — the N in "N x mean"
    // INTERACTIVE WINDOW. Opens a real SDL2 window with a CAMetalLayer, animates
    // the scene through FDS's own Animate_Objects, and overlays live per-pass GPU
    // ms. Never enabled unless asked for -- offscreen stays the default so a
    // measurement run never pops a window.
    bool  interactive = false;
    int   winW = 1280, winH = 720;
    // OFFSCREEN regression probe for the window's PER-FRAME refresh path. Runs
    // Reanimate + the three refresh lambdas at two demo-t values and prints the
    // GPU-FACING arrays, so "the CPU list updated but the upload did not" is
    // measurable without a window. See the block in Deferred.mm.
    bool  animProbe = false;
    int   animProbeT[2] = {5743, 5877};
    // Demo-timer rate in centiseconds per second. g_FrameTime is a 100 Hz clock
    // (RENDER.CPP), so 100 is real time.
    float timeScale = 100.0f;
    bool  freeFly = true;              // else follow the authored camera spline
    // Auto-close after N frames. 0 = run until ESC. Exists so the window path is
    // testable without a human at the keyboard, and so a short visible run can be
    // bounded rather than killed.
    int   winFrames = 0;
    const LoadOptions *loadOpt = nullptr;   // needed to Reanimate per frame
    int   dumpCube = -1;
    std::string dumpCubePath;
    // DIAGNOSTIC (--dump_env_cube). Read every baked ENVIRONMENT probe cube
    // back to the CPU, print a per-face radiance census and write a 3x2 face
    // atlas PPM per probe. The counterpart of FDS's FDS_ENVBAKE_DUMP
    // [ENVBAKE-FACE] census, printed on the SAME 0-255 radiance scale (the
    // stored RGBA16Float linear radiance x255), so the two arms' probe CONTENT
    // can be compared face by face instead of inferred from a lit frame.
    // Forces Shared storage on the env cubes, so never a timing run.
    bool  dumpEnvCube = false;
    std::string dumpEnvCubeDir = "/tmp";
    // GROUND-TRUTH PROBE. For one WORLD point, print for every cube light:
    //   (a) the true nearest shadow-casting triangle between light and point,
    //       found by ray-casting the SAME triangles the bake rasterised, with
    //       the mesh + material NAME — no convention arithmetic involved;
    //   (b) a host-side replica of the shader's tap (face pick, uv, stored
    //       depth, decoded distance, ref, bias, pass/fail).
    // If (a) and (b) disagree, the bug is the tap's conventions. If they agree,
    // the cube is telling the truth and the occlusion is geometry. Written
    // because two rounds of this investigation were spent inferring geometry
    // from an 8x8 grid of decoded depths.
    bool  probe = false;
    float probePoint[3] = {0, 0, 0};
    // Same idea keyed on a SCREEN PIXEL: build that pixel's camera ray from the
    // very constants the vertex shader uses, ray-cast ALL geometry, and then
    // replay the lighting pass's own per-light gate on the hit — distance vs
    // range, NoL, attenuation, and the cube tap — printing which test failed.
    // "This pixel is not lit by light N" had no answerable form before this.
    bool  probePx = false;
    int   probePxXY[2] = {0, 0};
    // ---- GPU HARDWARE TESSELLATION (docs/GPU_BENCHMARK_PLAN.md §6.3) -------
    // Displaces the greets stone (`rooms` + `floor`) with REAL geometry, from
    // the same height map the CPU bake and the POM march use. DEFAULT OFF, and
    // byte-null when off: the stone batches take the untessellated pipeline and
    // every md5 recorded in the plan document is unchanged.
    //
    // THE KNOB is `tessTargetPx`: the target triangle EDGE LENGTH IN PIXELS.
    // A patch edge measuring L pixels on screen asks for round(L/target)
    // segments, so the rule is screen-space adaptive by construction and a
    // patch at the far end of the corridor costs a fraction of one at the
    // camera. `--tess_px=1` is the "one triangle per pixel" ask.
    //
    // THE FLOOR IS NOT SET BY THIS KNOB ALONE. Metal caps a pipeline's
    // maxTessellationFactor at 16, so one patch cannot split an edge more than
    // 16 ways however small the target. `tessPresplit` (P) is the second half:
    // instance s of the draw renders sub-triangle s of a uniform PxP
    // barycentric split of the base triangle, each with its own factor record,
    // so the effective per-edge subdivision is P x 16 at zero vertex-buffer
    // cost. Reaching 1 px on the near wall at t=5743 needs P >= 8 — see the
    // cost curve in the plan document.
    bool  tess = false;
    float tessTargetPx = 8.0f;
    int   tessPresplit = 1;
    // World displacement amplitude. The CPU bake's --greets_displace_amp,
    // whose default is 0.3 (FeatureFlags.def:435); the S1d perf table's
    // "tessellation @0.18" arm runs 0.18 to match the shell arm's world amp.
    float tessAmp = 0.3f;
    // Height-map mip the displacement samples, and the mip whose mean is
    // subtracted. CPU parity: --greets_displace_mip defaults to 2
    // (FeatureFlags.def:436) and mipMean is that mip's texel average
    // (MeshOps.cpp:2097-2113).
    int   tessMip = 2;
    // Frustum-cull patches by writing a ZERO tessellation factor (the Metal
    // primitive for discarding a patch), widened by the displacement amplitude
    // so a patch that displacement pushes back on screen is not lost. ON by
    // default; --no-tess_cull prices it.
    bool  tessCull = true;
    // Backface-cull patches the same way. OFF by default and deliberately so:
    // a backfacing base patch can still carry visible relief on a displaced
    // silhouette, which is the whole subject of this arm. The rasterizer's own
    // backface cull still runs either way.
    bool  tessBackCull = false;
    // Which factor slot is which edge. 0 = the OPPOSITE-VERTEX convention
    // (slot i is the edge not touching control point i), 1 = adjacent. Settled
    // by looking for cracks, not by trusting documentation.
    int   tessEdgeMap = 0;
    // Count the generated geometry EXACTLY: one atomic per post-tessellation
    // vertex invocation (a separate shader entry point, never in a timed
    // pipeline) plus the summed boundary segments from the factor kernel.
    // Triangles then follow from Euler for a triangulated disk,
    //   T = 2V - B - 2  per patch,
    // which is exact rather than modelled. Adds an untimed probe frame.
    bool  tessStats = false;
    // Measure the ATTRIBUTE SEAMS of the stone: how many positions shared by
    // 2+ faces disagree about UV or normal, and the world-space displacement
    // gap that disagreement opens at the current amp. This is the mechanism of
    // the thin residual crack that survives a correct factor record — it is a
    // property of the MESH (per-face UVs), not of the tessellator.
    bool  tessSeamAudit = false;

    // ---- SECOND-ORDER MIRRORS (mirror inside a mirror) ---------------------
    // The CPU's --mirror_rtt order-2 slots, done the GPU way. For each ordered
    // pair (A, B) of mirror panels where B is on the reflective side of A's
    // reflected eye, the scene is rendered from the DOUBLY reflected camera
    // reflect_B(reflect_A(eye)) and composited onto B's panel pixels INSIDE A's
    // reflection. Same construction as the CPU's bake camera
    // (FDS/RENDER/GreetsMirror.cpp:2985-2991), and order 2 is the ceiling in
    // both arms. Default ON: the user asked for it, and the CPU only ships it
    // off because of a setDefault ordering defect (docs/SETDEFAULT_AUDIT.md).
    bool  mirror2 = true;
    // Resolution of the order-2 targets as a fraction of the main framebuffer.
    // The order-2 content lands on a PANEL, which is a small part of the frame,
    // so full resolution is wasted; 0.5 is the default and the cost/sharpness
    // knob. Clamped to [0.125, 1].
    float mirror2Scale = 0.5f;
    // Skip an (A,B) pair whose panel B covers fewer than this many pixels in
    // A's reflection — the CPU's `area <= 1.0f` job-selection gate
    // (GreetsMirror.cpp:2961), with a larger default because a 4-pixel panel
    // cannot show a reflection anybody can see.
    float mirror2MinPx = 16.0f;
    // Per-frame census of which order-2 pairs rendered and at what size.
    bool  mirror2Stats = false;
    // CALIBRATION PROBE (0 = off). Force every tessellation factor to this
    // value. Exists to answer two questions no documentation settles for this
    // device: what the hardware's real factor ceiling is, and whether the
    // post-tessellation vertex function is invoked once per VERTEX or once per
    // triangle CORNER — the triangle count depends on both.
    int   tessUniform = 0;
    std::string outPath;
};

// Median/p5/p95 for one pass, in ms.
struct PassTiming {
    std::string name;
    double median = 0, p5 = 0, p95 = 0;
};

struct DeferredResult {
    std::vector<PassTiming> passes;
    PassTiming              frame;
    int                     shadowCubes = 0;
    int                     shadowFaces = 0;
    long                    shadowTexels = 0;
    int                     litLights = 0;
    int                     movingCubes = 0;
    double                  staticBakeMs = 0.0;   // one-time cached bake
    // Tessellation census, from --tess_stats. `tessVerts` is an exact hardware
    // count (one atomic per post-tessellation vertex invocation); `tessTris`
    // follows from Euler per patch and is exact given it.
    long long               tessVerts = 0;
    long long               tessTris = 0;
    long long               tessPatchesLive = 0;
    long long               tessBoundarySegs = 0;
    int                     tessMaxFactor = 0;
    double                  tessFactorMs = 0.0;   // the factor compute kernel alone
};

// Returns false on hard failure. Renders offscreen; never opens a window.
bool RunDeferred(Scene &scene, const DeferredOptions &opt,
                 const std::string &shaderPath, DeferredResult &out);

}  // namespace gpubench
