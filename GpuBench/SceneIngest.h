// SceneIngest — turn an FDS scene (FLD + LWO + textures) into flat buffers a
// GPU can consume, with ZERO involvement of the software rasterizer, the
// clipper, the deferred kernel, or SDL.
//
// This is the load half of the standalone GPU benchmark
// (docs/GPU_BENCHMARK_PLAN.md). It links FDS *purely for loading*, so the
// input is provably the same bytes, through the same code, that DEMO renders.
//
// Two properties of the engine's data model drive the shape of the output and
// are NOT negotiable (see docs/ENGINE.md and GPU_BENCHMARK_PLAN.md §2.4):
//
//   1. UVs must be read from the FACE (Face::U1..V3), never from the vertex.
//      A vertex shared between faces of different projection orientation has
//      its per-vertex U/V clobbered by whichever face was mapped last. So the
//      vertex buffer is DE-INDEXED: 3 unshared vertices per triangle.
//
//   2. Texture pixels must be taken straight after Load_Texture() and BEFORE
//      Generate_Mipmaps(). Load_Texture leaves Data linear/row-major;
//      Generate_Mipmaps is what block-tiles ("Sachletz") it into the swizzled
//      layout the CPU rasterizers require. We never call it — the GPU builds
//      its own mips.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Texture;

namespace gpubench {

bool ExpandToRGBA(const ::Texture *tx, struct TextureImage &out);

// 48 bytes, interleaved. Positions/normals/tangents are OBJECT space; the
// model matrix travels separately per batch so the GPU does the full
// model->view->clip transform (rather than us pre-baking world space on the
// CPU, which would quietly move work off the side under test).
//
// tx..tz + th: the ENGINE's shading tangent frame, not a screen-space
// reconstruction. Normals and tangents are the per-corner values DEMO's
// MakeFacesIndependentByAngle(30 deg) + Compute_Vertex_Tangents produce
// (crease-preserving smoothing, Lengyel tangent from the per-FACE UVs,
// Gram-Schmidt vs the corner normal), and th is the per-face UV-winding
// handedness sign: B = th * (N x T), exactly the deferred kernel's
// Mat->TbnHandedness convention. The CPU realises the handedness as a
// ::mirUV material clone because its kernel reads per-material state; this
// arm's buffer is de-indexed per face, so the sign rides the vertex instead.
struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    float tx, ty, tz, th;
};

// One planar mirror panel, greets' first-order set: 'teleporter' (opaque
// silver back wall), 'screen 3' (big end display), 'screen 4' — the explicit
// kMirrorMats/kMirrorScreenMats designation of DEMO/GREETS.CPP:2880-2897.
// Plane found by the ENGINE's own FindMirrorPlaneByMatName (world space,
// N·P + d = 0, N face-side outward). The GPU arm renders the reflected scene
// from the plane-reflected camera and composites panel pixels as
// emissive + reflection/2 — radiometrically what the CPU's clone-geometry +
// transparent-wallMatClone machinery produces (the clone/omni-clone/mirrorId
// apparatus is the CPU's way of lighting the reflected world; a reflection
// render of the real world from the mirrored camera is the same integral).
// One baked environment probe: a cube of the LIT scene rendered from `pos`,
// used by EnvSpecComposeScalar's GPU counterpart. `material` is the surface it
// was baked FOR, which the bake self-excludes so a metal does not reflect
// itself (the CPU's g_envBakeSkipMats, face-level, matching on the name with
// any '::mirUV' suffix stripped).
struct EnvProbe {
    float       pos[3] = {0, 0, 0};
    std::string material;
    int         users = 0;      // materials aliasing this probe
};

struct MirrorInfo {
    float       n[3] = {0, 0, 0};
    float       d = 0.0f;
    std::string material;
    int         panelFaces = 0;
    // World AABB of the TAGGED panel triangles (the ones that passed the
    // 30-degree + on-plane test), not of the whole box the panel belongs to.
    // Second-order mirroring needs a screen footprint for this panel from an
    // arbitrary camera — to decide whether an (A,B) pair is worth rendering and
    // how big its target should be — and the plane alone cannot give one. Empty
    // when no face was tagged (bmin > bmax).
    float       bmin[3] = { 1e30f,  1e30f,  1e30f};
    float       bmax[3] = {-1e30f, -1e30f, -1e30f};
    bool hasBounds() const { return bmin[0] <= bmax[0]; }
};

// One draw per (mesh x material) run of triangles.
struct Batch {
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;   // always a multiple of 3
    int      textureIndex = -1; // -1 = untextured, use baseColor
    // 1-based index into Scene::mirrors when this batch IS a mirror panel's
    // front faces (they composite the reflection; Diffuse/Specular forced 0,
    // parity with the CPU's wallMatClone). 0 = ordinary geometry.
    int      mirrorIndex = 0;
    int      normalTexIndex = -1;
    int      roughTexIndex = -1;
    int      heightTexIndex = -1;
    int      aoTexIndex = -1;      // Material::AoMap (RVSM 'ao' role)
    int      metalTexIndex = -1;   // Material::MetallicMap (RVSM 'metallic')
    // ENVIRONMENT REFLECTION. 1-based index into Scene::envProbes, 0 = this
    // material reflects nothing. Assignment replicates
    // EnvReflection_FramePrep's rule (EnvBake.cpp:1050): a material qualifies
    // when Reflection > 0 OR it has a MetallicMap, the probe sits at the
    // world CENTROID of that material's faces, and a material whose centroid
    // is within 4 world units of an existing probe ALIASES it instead of
    // getting its own.
    int      envProbe = 0;
    // Material::Reflection. F0 = max(Reflection*0.01, 0.04), then lerped
    // toward 0.98 by metalness — DeferredSurfaceKernel.cpp:1252-1260.
    float    reflection = 0.0f;
    // Material::AoStrength (editor 'aoStrength'), multiplied by the global
    // --ao_map_strength (default 2.0) at the point of use, as the CPU does.
    float    aoStrength = 1.0f;
    float    baseColor[3] = {1, 1, 1};
    float    luminosity = 0.0f, diffuse = 1.0f, specular = 0.0f;
    uint32_t glossiness = 0;
    float    parallaxScale = 1.0f;
    bool     aoInAlpha = false;   // albedo alpha holds baked AO (Mat_AoInAlpha)
    // ---- TRANSPARENCY (FDS_DEFS.H:129-141 material flag bits) --------------
    // Carried per batch because the CPU routes transparent faces through a
    // completely different renderer: the FORWARD transparent kernel + the
    // per-clump depth peel (DeferredSurfaceKernel.cpp:3654 RenderXparClumpInStrip),
    // not the deferred G-buffer. A transparent batch must therefore be REMOVED
    // from the opaque G-buffer draw list, not merely shaded differently.
    uint32_t matFlags = 0;        // Material::Flags, verbatim
    bool     transparent = false; // Mat_Transparent (0x0020)
    bool     additive = false;    // Mat_Additive    (0x0040)
    bool     skipZ = false;       // Mat_SkipZ       (0x0100)
    bool     twoSided = false;    // Mat_TwoSided    (0x0010)
    bool     refractive = false;  // Mat_Refractive  (0x1000) — --glass_refract opt-in
    // Blend weights, read out of the transparent kernel's composite
    // (DeferredSurfaceKernel.cpp:3506-3532, the g_hdrActive branch greets runs):
    //   XparBlendAlpha > 0 : out = lit*a + dst*(1-a)          (lerp)
    //   else               : out = lit + dst*dw, UNCLAMPED in HDR,
    //                        dw = Transparency*0.01 if > 0 else 0.5
    float    xparBlendAlpha = 0.0f;   // Material::XparBlendAlpha
    float    transparency = 0.0f;     // Material::Transparency (LWO TRAN, 0-100)
    // Does this material act as a solid shadow occluder? Reproduces the CPU
    // bake's caster filter EXACTLY (FDS/RENDER/Shadows.cpp:713-724): skip
    // Mat_Transparent | Mat_Additive | Mat_SkipZ, plus any material whose NAME
    // contains "lamp" or "emi" (the FLD carries no emissive flag, so the engine
    // infers it from the name). This is not an optimisation — every greets omni
    // is authored INSIDE a `lamp light` fixture ~0.3 units across, so without the
    // filter each light bakes the inside of its own housing and the whole room
    // reads as shadowed.
    bool     castsShadow = true;
    // Is the OBJECT this batch belongs to ANIMATED, by the CPU's own
    // static-bake predicate (`isDynamicForBake`, FDS/RENDER/Transform.cpp:1466)?
    // Pos-spline extent > 0.1 world units, or a Rotate spline whose quaternion
    // extent > 0.01, on this object OR ANY ANCESTOR.
    //
    // WHY IT IS HERE, and it is not an optimisation: the CPU EXCLUDES every
    // animated mesh from every env-reflection probe bake
    // (Transform.cpp:1274 `inStaticBake |= g_envBakeSkipDynamic`, applied at
    // :1560), on the stated grounds that "the panorama is a STATIC capture, so
    // moving meshes (the walking mech) must not be frozen into it". This arm
    // has no such rule, so its cockpit probe contains the mech's own hull,
    // barrels and legs while the CPU's contains the empty room. That is the
    // largest single CONTENT difference between the two arms' probes on greets
    // and `--env_bake_skip_animated` is what prices it.
    bool     animForBake = false;
    // Model matrix: FDS Matrix is float[3][3], ROW-major, applied as row-dot-v
    // (Animate_Objects has already folded IScale into the rows and resolved the
    // parent hierarchy into IPos). worldPos = rot * objPos + pos.
    float    rot[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
    float    pos[3] = {0, 0, 0};
    // WORLD-space bounding sphere of this batch's triangles, refreshed with the
    // model matrix. Used only by the shadow bake's per-cube-face frustum cull —
    // the analogue of the CPU's per-pass mesh cull, which is what makes the
    // shadow workloads comparable.
    float    bsCtr[3] = {0, 0, 0};
    float    bsRad = 0.0f;
    std::string meshName;
    std::string materialName;
    // Running index of the OBJECT this batch came from. The CPU's transparent
    // peel clumps by (ParentTri, side) — the mesh POINTER, not its name — and
    // fountain has six distinct Objects all called "pilon.lwo", so grouping by
    // name would fuse six separate glass shells into one peel clump.
    int      meshId = -1;
    // The SOURCE TriMesh this batch was de-indexed from, as an opaque pointer
    // (this header stays FDS-free; SceneIngest.cpp casts it back).
    //
    // This is the object's IDENTITY, and the per-frame transform refresh keys
    // on it. It used to key on `meshName`, which is a BUG with a picture: a
    // name->TriMesh* map keeps only the LAST object of each name, so on the
    // first Reanimate all six of fountain's "pilon.lwo" batches were handed the
    // SIXTH pilon's model matrix and the six spires stacked onto one. Offscreen
    // renders never call Reanimate, so it only ever showed in the --window
    // build — the "there is only one spire present" report. Reproduce with
    // `--reanimate`; see docs/GPU_BENCHMARK_PLAN.md.
    const void *srcMesh = nullptr;
};

struct TextureImage {
    std::string fileName;
    int32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;   // expanded to tightly packed RGBA8
};

struct Light {
    float pos[3] = {0, 0, 0};
    float color[3] = {0, 0, 0};   // 0..255 as authored
    float intensity = 0.0f;       // Omni::ISize
    float range = 0.0f;           // Omni::IRange — HARD cutoff, not inverse-square
    bool  parented = false;       // mech-attached (position was NaN pre-hierarchy)
    // Light_Omni vs Light_SpotLight. Spots come from GreetsDisco.cpp, which is
    // ON by default in greets (greets_disco defaults 1) — so they are PARITY, not
    // an addition. A spot bakes ONE perspective depth map, not a 6-face cube
    // (FDS/RENDER/Shadows.cpp treats Light_SpotLight as a single-map entry).
    bool  isSpot = false;
    float dir[3] = {0, 0, -1};    // Omni::IDir, world, normalised
    float cosInner = 1.0f;        // Omni::HotSpot = cos(hotInnerDeg)
    float cosOuter = 1.0f;        // Omni::FallOff = cos(fallOuterDeg)
    int   shadowRes = 0;          // Omni::shadowMapRes (0 = use the class default)
    // DEMO/GpuWeb.cpp only: which of its 8 spot shadow maps this light owns
    // this frame (-1 = none). The Metal bench keys its maps by light index and
    // never reads it.
    int   shadowSlot = -1;
    bool  castsShadow = true;
    // SPOT shadow camera, built by FDS's own Kick_Camera exactly as
    // FDS/RENDER/Shadows.cpp:470-491 does it — look from IPos toward IPos+IDir,
    // world-up Y, with the same tiny X nudge for a perfectly vertical IDir, and
    // fovHalf = acos(max(0.01, cosOuter)) * 1.10 (the engine's 10% pad so
    // silhouettes near the cone edge get a few pixels of context).
    float shadowRot[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
    float shadowTanHalfFov = 1.0f;
    // Flare sprite (Omni::F). Under --hdr the flare adds into the float radiance
    // buffer (FILLERS.CPP Spriter<Res,true,true>), additively, with the colour
    // taken from the FLARE TEXTURE — not from the light colour. Half-size in
    // pixels = ImageSize * flareSize * perspX / viewZ, and greets sets
    // ImageSize = 0.25 (GREETS.CPP:3060).
    int   flareTexIndex = -1;
    float flareSize = 0.0f;       // Omni::ISize * (FlareScale ?: 1)
    const char *origin = "fld";   // fld | disco-spot | disco-glow — for the report
};

struct Camera {
    float rot[3][3] = {{1,0,0},{0,1,0},{0,0,1}};   // view matrix, row-major row-dot-v
    float src[3] = {0, 0, 0};                      // eye position, world space
    float perspX = 0.0f, perspY = 0.0f;            // == FOVX / FOVY
    float cntrEX = 0.0f, cntrEY = 0.0f;
    float fov = 0.0f;
    float nearZ = 0.01f, farZ = 150.0f;
};

struct Scene {
    std::vector<Vertex>       verts;
    std::vector<Batch>        batches;
    std::vector<TextureImage> textures;
    std::vector<Light>        lights;
    std::vector<MirrorInfo>   mirrors;
    std::vector<EnvProbe>     envProbes;
    int                       envTexIndex = -1;
    // Scene AABB, world. The parallax proxy the env lookup corrects against —
    // the CPU's EnvPanoLinear::boxMin/boxMax slab test.
    float                     aabbMin[3] = {0, 0, 0};
    float                     aabbMax[3] = {0, 0, 0};
    Camera                    camera;
    float                     ambient[3] = {0, 0, 0};    // Scene::Ambient, 0..255
    // Authored backdrop gradient (Scene::SkyZenith / SkyNadir, 0..255). Projected
    // into L2 SH for the --sh_ambient path, so the ambient term is real authored
    // content rather than a flat constant.
    float                     skyZenith[3] = {0, 0, 0};
    float                     skyNadir[3] = {0, 0, 0};
    // Which ambient BRANCH of the CPU kernel this scene runs. `sh_ambient`
    // defaults 0 (FeatureFlags.def:47) and the ONLY setDefault in the tree is
    // greets' (GREETS.CPP:1175) — so greets takes the SH branch
    // (DeferredSurfaceKernel.cpp:1741-1760) and every other scene takes the FLAT
    // one (:1761-1768), `Diffuse * Sc->Ambient`. Getting this wrong is not a
    // shade of grey: fountain authors SkyZenith = SkyNadir = (0,0,0), so an
    // unconditional SH path gives it EXACTLY ZERO ambient.
    bool                      shAmbient = false;

    int      xres = 0, yres = 0;
    // Scene::XparPeelPasses — transparent depth-peel passes PER SIDE, the CPU's
    // own per-scene value (greets leaves it 0 -> 1; fountain sets 4). The
    // effective value follows the CPU's xparPeelPassesEffective()
    // (DeferredSurfaceKernel.cpp:3600): an explicit flag wins, else this.
    int      xparPeelPasses = 1;
    float    curFrame = 0.0f;
    // The demo-t that actually produced `curFrame`. Differs from
    // LoadOptions::demoT only when Load() substituted a mid-scene default for
    // the greets-specific built-in. The pose block and the window HUD report
    // THIS one, so the paste-ready repro lines name the t that was rendered.
    float    resolvedDemoT = -1.0f;
    // Objects the ingest REFUSED, and why. Non-zero means the batch list on
    // screen is smaller than the scene, permanently — the per-frame refresh
    // only updates batches that already exist, so scrubbing back into the
    // authored range does NOT bring them back. Surfaced in the window HUD
    // because a screenshot that says "48 DRAWS" and nothing else cannot be
    // told apart from a scene that only has 48.
    int         droppedMeshes = 0;
    std::string droppedNames;
    // FDS global ImageSize at ingest time (greets sets 0.25) — the flare
    // sprite's world scale. Carried so the GPU flare pass uses the scene's
    // number rather than a constant of its own.
    float    imageSize = 1.0f;
    // Census, for the report.
    uint32_t meshCount = 0;
    uint32_t faceCount = 0;
    uint32_t srcVertCount = 0;    // pre-de-index, i.e. what the CPU transforms
    uint32_t texturesLoaded = 0;
    uint32_t texturesMissing = 0;
    double   loadMs = 0.0;
};

struct LoadOptions {
    const char *fldPath = "SCENES/GREETS.FLD";
    int         xres = 1920;
    int         yres = 1080;
    // Demo-timer value from docs/greets_review_poses.txt. Mapped to the
    // engine's CurFrame with the greets scene's own formula (see .cpp).
    int         demoT = 5743;
    // Was --t actually given on the command line? The default above is GREETS'
    // and lands outside every other scene's authored frame range; Load()
    // substitutes a mid-scene t when this is false. An explicit --t is always
    // honoured, warning if out of range. See Load().
    bool        demoTExplicit = false;
    // "px,py,pz,fx,fy,fz" as in FDS_GREETS_CAM. Empty = use the scripted camera.
    std::string camPose;
    bool        verbose = true;
    // --dump_meshes: per-OBJECT name + triangle count + per-material route,
    // for a straight diff against the CPU's `DUMP_MESHES=1` listing. The
    // camera-independent answer to "is geometry missing on this arm".
    bool        dumpMeshes = false;
    // Replicate GreetsDisco.cpp's 10 rotating cone spotlights + glow omni clone.
    // greets_disco defaults to 1 in FeatureFlags.def, so these are part of the
    // DEFAULT greets run — reproducing them is parity. ON here for the same
    // reason stoneTex is.
    bool        disco = true;
    // Apply the LWO/FLD-authored PBR map SETS (Surf_RevMaps / RVSM). DEMO does
    // this at scene init via MaterialImport_ApplyRevMaps; the registry it reads
    // is FDS-side, so this arm replays it. ON = parity (the reference applies
    // 32 maps). --no-revmaps renders every surface from its legacy FLD JPG.
    bool        revMaps = true;
    // Identify the greets mirror panels (teleporter / screen 3 / screen 4) so
    // the deferred arm can render + composite first-order reflections.
    // greets_mirror defaults ON in DEMO, so ON here is parity; --no-mirror
    // renders the panels as plain emissive geometry (the pre-mirror look).
    bool        mirrors = true;
    // Disambiguate WHICH face of a mirror material is the mirror. greets'
    // 'screen 3'/'screen 4' are closed boxes, and the engine's plane fitter
    // returns one of the six faces with no facing rule — for 'screen 4' it
    // returned the face pointing OUT of the corridor, which left the panel
    // permanently inactive. Pick the face whose normal points toward the
    // scene's centre of mass instead. --no-mirror_face restores the raw
    // engine plane, so the change is priceable.
    bool        mirrorFacing = true;
    // Bake environment probes for reflective/metallic materials. greets gets
    // --env_refl for free on the CPU (the metallic import setDefaults it,
    // MaterialImport.cpp:418), so ON is PARITY.
    bool        envRefl = true;
    int         envRes = 128;   // cube FACE resolution
    // Replicate DEMO's --greets_stone_tex override (default ON there, so ON here).
    // Without it the wall this renders is the AUTHORED FLD wall, not the surface
    // the user actually reviews — see docs/GPU_BENCHMARK_PLAN.md §3.2. No
    // displacement arm may run with this off.
    bool        stoneTex = true;
    // Omni range FORCE-OVERRIDE, matching what survives of the greets range
    // patch after commit 00f7820 ("author omni light ranges in the LWS, delete
    // both runtime patches"): the LWS/FLD now authors LightRange 30 on all ten
    // omnis and DEMO's greets_omni_default_range is a default-0 (inert) tuning
    // dial that, when set, rewrites every omni's Range spline keys. Same here:
    // 0 (default) = the authored envelope flows through Animate_Objects
    // untouched; > 0 = rewrite every Range key to this value.
    // (An earlier revision replicated the pre-00f7820 IRange==0 patch with a
    // default of 30 — correct then, but it would now stomp future authored
    // per-light ranges with a flat 30.)
    float       omniDefaultRange = 0.0f;    // 0 = authored ranges (parity)
};

// Returns false on failure. Never opens a window and never touches VPage/ZPage16
// beyond letting VESA_Surface2Global publish the projection globals that
// CalcPersp reads.
bool Load(Scene &out, const LoadOptions &opt);

// Re-run the engine's animation at a new demo-timer value and refresh everything
// that is per-frame: object model matrices, the scripted camera, and the whole
// light list (the disco spots rotate). Vertices, textures and material constants
// are frame-invariant and are NOT rebuilt -- Animate_Objects fills per-mesh
// IPos/IScale/RotMat and does not deform vertices, so the mech animates as a
// hierarchy of rigid TriMeshes. VertexHash() exists to keep that claim measured.
bool Reanimate(Scene &out, const LoadOptions &opt, float demoT);
uint64_t VertexHash(const Scene &s);

// Build a view matrix with the ENGINE's Kick_Camera from an eye + forward
// direction. Exposed so the interactive window's free-fly camera goes through
// the same construction as every other camera in this arm, without pulling the
// FDS headers (whose ::Vertex / ::Scene / ::Texture collide with ours) into the
// Objective-C++ renderer.
void BuildViewMatrix(const float eye[3], const float fwd[3], float outRot[3][3]);

// ---------------------------------------------------------------------------
// The interactive FREE CAMERA is the ENGINE's own.
//
// FDS/CAMERAS/CAMERAS.CPP's Dynamic_Camera() is the house free-cam — the one
// DEMO/DisplaceTest.cpp drives and the one every scene's TAB-camera uses. It is
// in FDS, so this arm CALLS it rather than reimplementing a WASD scheme: same
// velocity/angular-velocity integration, same exponential damping and hard stop,
// same world-yaw / camera-local-pitch decomposition (a plain Euler compose
// reverses yaw past 90 degrees of pitch — the reason the split exists), same
// `, . K L` speed dials, same per-scene speed calibration sqrt(FZP)/180.
//
// FreeCamInput is a pass-through for KEY STATE only. The mapping of these fields
// onto FDS scancodes lives next to the call, and the mapping of scancodes onto
// motion is CAMERAS.CPP's — not re-derived here.
struct FreeCamInput {
    bool fwd = false, back = false;          // W        | S, Z
    bool left = false, right = false;        // A, End   | D, PgDn
    bool up = false, down = false;           // Q, KP+   | E, KP-
    bool yawLeft = false, yawRight = false;  // Left     | Right
    bool pitchUp = false, pitchDown = false; // Up       | Down
    bool rollLeft = false, rollRight = false;// Home     | PgUp
    bool slower = false, faster = false;     // ,        | .      (translation dial)
    bool rotSlower = false, rotFaster = false;// K       | L      (rotation dial)
};

// Calibrate FDS's free camera for this scene (Calibrate_FreeCamera_ForScene,
// which sets Vel_Speed = sqrt(FZP)/180 and resets both dials) and seed it from
// the camera currently in `s`, so the window opens where the offscreen renders do.
void FreeCamInit(const Scene &s);
// One step of Dynamic_Camera(). `dtSeconds` is real time; it becomes the
// engine's `dTime` through DisplaceTest's own formula (Timer is a 100 Hz tick
// clock and dTime = 0.25 * elapsed ticks), so the feel matches the demo's.
// Writes the result into s.camera through the engine's CalcPersp.
void FreeCamStep(Scene &s, const FreeCamInput &in, float dtSeconds);
// Mouse-look. NOT a house control — Dynamic_Camera reads no mouse at all — so
// this is a stated GpuBench ADDITION, implemented with the same world-yaw /
// local-pitch decomposition rather than an Euler compose.
void FreeCamMouseLook(Scene &s, float dYaw, float dPitch);
// Copy the scene camera into FC, so leaving spline mode does not teleport.
void FreeCamSyncFromScene(const Scene &s);
// The `G` key: print the pose under the camera, in DisplaceTest's own
// [DTEST-POSE] form AND as this arm's --cam= string.
void FreeCamDumpPose(const Scene &s);

// ---- THE POSE BLOCK -----------------------------------------------------
// Where the camera came from. Printed as part of the block because "the GPU
// shows X and the CPU shows Y" is unanswerable until both arms agree they were
// looking at the same thing, and a pinned pose and a spline pose at the same
// `t` are NOT the same thing (the spline moves; the pin does not).
enum class PoseOrigin {
    Spline,       // camPose empty: the FLD's own authored camera track at CurFrame
    ExplicitCam,  // --cam="px,py,pz,fx,fy,fz" on the command line
    // GREETS ONLY, and it is the DEFAULT there: GpuBenchMain seeds camPose with
    // the greets primary review pose and clears it for every other scene. So a
    // bare `--t=N` on greets PINS the camera and animates only the scene — the
    // pose is byte-identical at t=1588 and t=5743. This is a distinct origin and
    // not "the spline", which is why it has its own name: reporting it as the
    // spline is how you end up comparing two arms that were never on the same
    // camera at all.
    DefaultReviewPose,
    FreeFly,      // the interactive window's Dynamic_Camera free camera
};

// Print, ONCE, the block that names exactly where this camera is and how to
// reproduce it in the OTHER arm. Every GpuBench run prints it — offscreen from
// Load(), interactive from the window loop and again on `G`.
//
// Nine significant digits, for DEMO's own stated reason (GREETS.CPP:3932): a
// grazing face's front/back test (N.A ~ 0) flips inside 3-decimal truncation
// error, so a pose quoted at 3 decimals reproduces the OTHER side of the flip
// from the run being reported. The GPU line and the CPU line are emitted as
// complete, copy-paste-runnable commands, not as fragments to be assembled.
void PrintPoseBlock(const Scene &s, const LoadOptions &opt, PoseOrigin origin,
                    float demoT);

// The two repro COMMANDS on their own, so the window's periodic telemetry and
// the block share one source of truth instead of two drifting snprintf sites.
// Both are complete, quoted, runnable command lines at 9 significant digits.
// The CPU one is keyed to the scene's OWN snapshot driver: fountain reads
// FNTSNAP_POS/FWD/FOV, greets reads FDS_GREETS_CAM, city reads CITYSNAP_VIEW —
// they are not interchangeable, and a line that prints the wrong one (or a
// literal "<scene>" placeholder, which zsh reads as a redirection and refuses)
// is not paste-ready.
void PoseGpuCommand(const Scene &s, const LoadOptions &opt, float demoT,
                    char *out, size_t n);
void PoseCpuCommand(const Scene &s, const LoadOptions &opt, float demoT,
                    char *out, size_t n);

// OFFSCREEN evidence for "the scripted camera interpolates". Re-animates the
// scene at each demo-t in [t0, t1] step `step` and prints the resulting camera
// in the SAME format DEMO's snapshot [CAM] line uses, so the two can be diffed
// without opening a window. Prints nothing else and renders nothing.
void CameraTrack(Scene &s, const LoadOptions &opt, float t0, float t1, float step);

}  // namespace gpubench
