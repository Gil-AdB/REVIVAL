#ifndef REVIVAL_MATERIAL_H
#define REVIVAL_MATERIAL_H

#include "Color.h"
#include "Texture.h"
#include "Vector.h"

// Tier-2 cone-step POM: the maximum cone ratio a ConeMap byte encodes.
// A texel storing 255 means cone ratio kPomConeMax (near-flat → the ray can
// take a near-full step). Shared by the bake (MakeConeMap, DEMO/MeshOps.cpp)
// and the runtime march (FDS/FILLERS/Mekalele.h) — they MUST agree or the
// step size is wrong. Cone ratio = horizontal-UV-distance / height-difference
// to the nearest taller texel; values above ~4 all give near-max steps at the
// ≤~0.7 tangential ray speeds we hit, so 4 is a safe clamp with byte precision
// (~0.016) where it matters most (small ratios, near tall features).
inline constexpr float kPomConeMax = 4.0f;

// S1c HORIZON MAP (--pom_horizon, docs/S1_PIXEL_DISPLACEMENT_PLAN.md §S1c):
// per texel of a height map, the elevation of the relief's own horizon in
// kPomHorizonAzimuths evenly spaced tangent-space azimuths, stored as
// u8 sin(horizon). A light whose tangent-space elevation is below the horizon
// in its azimuth is occluded BY THE RELIEF ITSELF — the light-responsive
// mortar/block self-shadow that NEITHER the tessellation bake nor the shell
// march can produce (the PolyId shadow test is identity-only, one id per
// authored wall plane, so no intra-wall shadow exists in either path).
//
// Layout: 8 bytes per texel, mip chain contiguous, in the SAME block-tile order
// as the source height map — so the swizzled texel index the rasterizer already
// computed for the albedo indexes this too (mipOfs[mip] + swizzledUV, ×8). The
// 8 azimuths of one texel are adjacent, so a lookup that needs two neighbouring
// azimuths touches one cache line, not two.
inline constexpr int kPomHorizonAzimuths = 8;

struct PomHorizonMap {
	unsigned char *data = nullptr;      // kPomHorizonAzimuths bytes per texel
	size_t         mipOfs[16] = {};     // TEXEL offset of each mip (×8 for bytes)
	unsigned       numMipmaps = 0;
	size_t         texels = 0;          // total texels across the chain
	// The bake's "height units per texel of lateral travel" at mip 0 =
	// (UV amplitude the relief is displaced by) × (texels per UV tile). The
	// horizon angle is scale-free in world units — worldPerUV cancels between
	// the height amplitude and the texel pitch — so this one number, plus the
	// height field, fully determines the bake. Part of the disk-cache key.
	float          heightScaleTexels = 0.0f;
	int            radiusTexels = 0;
	// The SOURCE height map's layout. The kernel addresses this map with the
	// ALBEDO's swizzled UV + mip (exactly as it does the normal and AO maps),
	// so these must match the albedo's or the lookup reads the wrong texel —
	// checked once after the textures are unified, never per pixel.
	int            sizeX = 0, sizeY = 0, blockSizeX = 0, blockSizeY = 0;
};

#pragma pack(push, 1)

struct Scene;

struct Material
{
    // ─── HOT: per-pixel-read fields, kept on the first cache line ────
    // Deferred lighting kernel reads these per-lit-pixel. Original
    // layout scattered them across ~3 cache lines (BaseCol at 0,
    // Luminosity/Diffuse/Specular at 40+, NormalMap at ~180+). Reorder
    // packs them into one line; the kernel's `Mat->X` reads stay
    // syntactically identical, just cheaper at the hardware level.
    Color                 BaseCol;                          // 0..16
    Texture             * Txtr                  = nullptr;  // 16..24
    Texture             * NormalMap             = nullptr;  // 24..32
    float                 Luminosity            = 0.0f;     // 32..36
    float                 Diffuse               = 0.0f;     // 36..40
    float                 Specular              = 0.0f;     // 40..44
    unsigned short        Glossiness            = 0;        // 44..46
    unsigned short        ReflectionMode        = 0;        // 46..48
    // Per-material override for the deferred transparent compositor's blend
    // formula. 0 (default) = legacy `litRGB + dst/2` saturated; >0 = linear
    // `litRGB*α + dst*(1-α)` no cap. Initialize_<scene> sets on opt-in
    // materials (e.g. fountain orb glass).
    float                 XparBlendAlpha        = 0.0f;     // 48..52
    float                 Transparency          = 0.0f;     // 52..56
    DWord                 Flags                 = 0;        // 56..60
    DWord                 TFlags                = 0;        // 60..64
    // ─── 64-byte cache line boundary ──────────────────────────────────
    // ─── WARM: scene-tagging, identification ─────────────────────────
    Scene               * RelScene              = nullptr; // This should be nuked from orbit. Just keep a scene id instead of a pointer
    dword                 ID                    = 0;
    // 16-bit shadow-group identity. Cube/spot PolyId paths compare this
    // (not `ID`) so distinct materials can share one shadow group, or
    // one material can be split into many. Set by scene-init code (e.g.
    // greets's hull-merge: hull/hull2 → same ShadowMatID; greets wall
    // split: per-plane-cluster gets a unique ShadowMatID). Default 0 =
    // "unassigned" — ShadowBarry + Mekalele then fall back to
    // `uint16_t(ID + 1)` (the legacy matID-based behavior). 16 bits
    // accommodates ~65 k distinct shadow groups (wall split for greets
    // tops out near 600).
    unsigned short        ShadowMatID           = 0;
    Texture             * EnvTexture            = nullptr;
    // Per-material ambient-occlusion map (PBR). Grayscale; sampled per-pixel
    // in the deferred kernel at the SAME swizzled UV / miplevel as the diffuse
    // (so it must share the diffuse's dimensions + block-tiled layout), and
    // multiplied into the AMBIENT term only — direct light is occluded by
    // shadows, not AO. AO is a scalar (no TBN), so it works on dynamic meshes.
    Texture             * AoMap                 = nullptr;
    // Per-material height/displacement map (PBR parallax). Grayscale height in
    // [0,1] (any channel; sampled byte). Must share the diffuse's dimensions +
    // block-tiled layout + mip structure so the SAME swizzled UV/miplevel the
    // rasterizer computes for the albedo indexes it. Consumed in Mekalele
    // (offset parallax: nudge the UV along the tangent-space view ray before the
    // texel pack), so albedo/normal/AO then all sample the parallax-shifted
    // texel for free. Gated on --parallax; null = flat.
    Texture             * HeightMap              = nullptr;
    // Per-material parallax strength multiplier (× the global --parallax_strength).
    // 1 = full; lower for surfaces where offset-parallax swims (grazing, densely
    // UV-tiled floors). Default 1.
    float                 ParallaxScale          = 1.0f;
    // Tier-2 cone-step map companion to HeightMap (--parallax_pom). 8-bit
    // single-channel, IDENTICAL tiled+mip layout to HeightMap so the SAME
    // swizzled texel address indexes both — each texel stores a conservative
    // cone ratio (quantized over [0,kPomConeMax]) = how far the view ray can
    // advance without hitting the height field. Offline-baked once at material
    // setup (MakeConeMap) only when --parallax_pom is on; null = no cone march.
    Texture             * ConeMap                = nullptr;
    // S1c: horizon map for this material's HeightMap (see PomHorizonMap above).
    // Baked once at scene setup when --pom_horizon is on, disk-cached; null =
    // no relief self-shadow for this material. PATH-AGNOSTIC: it is a shading
    // term, so it serves the tessellation bake and the per-pixel shell equally,
    // and it is baked from the FULL height field in both (under
    // --greets_displace the POM input becomes the RESIDUAL, but the geometry
    // still carries the low band, so the shadow term must model the whole
    // relief — hence the bake runs at height-load time, before the swap).
    PomHorizonMap       * PomHorizon             = nullptr;
    // S1b SHELL POM (--pom_shell, docs/S1_PIXEL_DISPLACEMENT_PLAN.md): the
    // relief slab's amplitude for the full 0..1 height range, in UV UNITS
    // (= the effective parallax strength the shell was BUILT with, i.e.
    // parallax_strength × ParallaxScale at build time). > 0 only after
    // PomShell_Build has pushed this material's geometry out to the lid — each
    // vertex by (amp × that face's world-per-UV-tile)/2 along its normal, with
    // Vertex::ShellH stamped to the height it landed at. UV units (not world)
    // makes it scale- and texel-density-independent: the rasterizer multiplies
    // by the SAME per-triangle world-per-UV solve the depth write uses, so the
    // march, the depth and the lid geometry cannot disagree, and the live
    // --parallax_strength can no longer desync from the built geometry
    // (strength is consumed once, at build time). 0 = not a shell material →
    // the legacy centered march runs.
    float                 PomShellUvAmp          = 0.0f;
    // S1b P0 (--pom_shell_world_amp, DIAGNOSTIC/OPT-IN, default 0 = OFF): the
    // slab amplitude authored in WORLD units instead of UV. With PomShellUvAmp
    // the world depth of the relief is uvAmp x that face's OWN world-per-UV, so
    // two faces of one material displace by different world distances whenever
    // their charts differ in scale (measured on greets: x1.26 across 'rooms'
    // authored planes, and x6.2 between 'rooms' 0.180 and 'floor' 1.113 —
    // docs/S1_DISCREPANCY_INVENTORY.md S8). When this is > 0 the builder offsets
    // every lid vertex by exactly WorldAmp/2 and the rasterizer derives each
    // triangle's UV amplitude as WorldAmp / w, so one authored surface displaces
    // by ONE world distance and pomDepthWorldAmp is constant. 0 keeps the UV
    // semantics byte-for-byte.
    float                 PomShellWorldAmp       = 0.0f;
    // S1b P0 (--pom_shell_world_amp): per-PATCH UV amplitude, one float per
    // patch, indexed by Face::PomShellGroup - 1 — worldAmp / that patch's
    // world-per-UV. A patch is coplanar by construction, so its UV density is a
    // single number and a per-patch table is exact. Null (the default) = every
    // face marches with the material's single PomShellUvAmp.
    float               * PomShellPatchUvAmp     = nullptr;
    // S1b: per-patch UV domain table for this material, 4 floats per patch
    // (uMin, uMax, vMin, vMax), indexed by Face::PomShellGroup - 1. Built by
    // PomShell_Build (union-find over edge-adjacent coplanar target faces);
    // null = no table, faces fall back to their own UV box.
    float               * PomShellDomains        = nullptr;
    unsigned              PomShellDomainCount    = 0;
    // S1b MULTI-BOX domain (--pom_shell_merge_uv): the patches on the SAME
    // PLANE whose UV rects abut/overlap are one physical surface even when no
    // edge joins them (greets' floor is one plane cut into 6 patches by the
    // doorway thresholds). The domain test is then "inside my own box OR inside
    // any of my siblings'" — the UNION OF THE BOXES, never their bounding box,
    // so a genuine opening between two coplanar patches (a doorway in a wall)
    // still discards. CSR: PomShellSibOfs[g]..[g+1] index quads of
    // PomShellSibBoxes (uMin,uMax,vMin,vMax), own box excluded (the kernel
    // tests it first and early-outs). Both null = single-box domains.
    float               * PomShellSibBoxes       = nullptr;
    uint32_t            * PomShellSibOfs         = nullptr;
    // Hot-loop bound on the sibling list (a patch with more coplanar abutting
    // neighbours than this keeps the first kPomShellMaxSibs — dropping siblings
    // can only ADD discards, never holes-by-omission of a real box, and the
    // build prints CLAMPED when it happens so it is never silent).
    static constexpr unsigned kPomShellMaxSibs = 12;
    // Applied albedo tint (per-channel multipliers, 1 = untinted). The tint
    // mutates the shared Texture pixels; these echo the last applied values
    // for the editor UI + sidecar round-trip (see MaterialImport tintR/G/B).
    float                 TintR                  = 1.0f;
    float                 TintG                  = 1.0f;
    float                 TintB                  = 1.0f;
    // Per-material AO-map strength multiplier (× the global --ao_map_strength),
    // same pattern as ParallaxScale. 1 = full effect; lower to tame an AO map
    // that reads too dark on one surface without dialing the whole scene.
    float                 AoStrength             = 1.0f;
    // Per-material specular RESPONSE multiplier (RVSF bit 0x800, editor
    // 'specMul' dial). Scales the FINAL accumulated specular term — analytic
    // highlights AND the env-specular reflection compose — in the deferred
    // kernels, applied AFTER the roughness-map/metal modulation so it never
    // distorts roughness, only the response amplitude. 1 = authored default
    // (multiplying by 1.0f is an exact float identity → byte-null); 0 kills
    // the specular response entirely. For sources whose specular reads wrong
    // on this engine (the Polyhaven sandstone's 'spec' map has no slot here —
    // this dial is the author-side control instead).
    float                 SpecMul                = 1.0f;
    // Per-material roughness map (PBR). Grayscale, 8-bit single-channel, same
    // tiled/mip layout as the albedo (sampled at the same swizzled UV/miplevel
    // — incl. the parallax-shifted UV). White = rough → LESS specular. Cheap
    // tier: modulates specular INTENSITY only (keeps the compile-time gloss
    // exponent). Gated on --roughness_map; null = uniform Mat->Specular.
    Texture             * RoughnessMap           = nullptr;
    // Per-material metalness map (PBR). Grayscale 8-bit, albedo texel layout
    // like the other aux maps. m=1: diffuse dies (metals have no diffuse),
    // specular + env reflection tint by the albedo, env F0 → ~1. Gated on
    // --metal_map; null = dielectric (m=0 everywhere).
    Texture             * MetallicMap            = nullptr;
    Material            * Next                  = nullptr;
    Material            * Prev                  = nullptr;
    char                * Name                  = nullptr;
    // ─── COLD: reflection / refraction (rarely sampled) ─────────────
    float                 Reflection            = 0.0f;
    char                * ReflectionImage       = nullptr;
    float                 ReflectionSeamAngle   = 0.0f;
    float                 RefractiveIndex       = 0.0f;
    // Per-material screen-space glass-refraction IOR override (engine-only,
    // sidecar-persisted 'refractIor' — no LWO/FLD field; distinct from the
    // authored RefractiveIndex above, which FLD ships but nothing consumes).
    // 0 = unset -> the deferred transparent kernel uses the global
    // FeatureFlags glass_refract_ior; >0 = this material Snell-bends (and
    // derives its Schlick F0) with THIS value. Read only inside the kernel's
    // Mat_Refractive glass block, so non-glass materials never pay for it.
    float                 RefractIor            = 0.0f;
    float                 EdgeTransparency      = 0.0f;
    float                 MaxSmoothingAngle     = 0.0f;
    // ─── COLD: texture filename + projection params (init-time) ──────
    char                * ColorTexture          = nullptr;
    char                * DiffuseTexture        = nullptr;
    char                * SpecularTexture       = nullptr;
    char                * ReflectionTexture     = nullptr;
    char                * TransparencyTexture   = nullptr;
    char                * BumpTexture           = nullptr;
    char                * TextureImage          = nullptr;
    unsigned short        TextureFlags          = 0;
    Vector                TextureSize;
    Vector                TextureCenter;
    Vector                TextureFallOff;
    Vector                TextureVelocity;
    char                * TextureAlpha          = nullptr;
    unsigned short        NoiseFrequencies      = 0;
    unsigned short        TextureWrapX          = 0;
    unsigned short        TextureWrapY          = 0;
    float                 AAStrength            = 0.0f;
    float                 Opacity               = 0.0f;
    float                 TFP0                  = 0.0f;
    float                 TFP1                  = 0.0f;
    bool                  ZBufferWrite          = true;
    bool                  ZBufferTest           = true;
    // ─── HDR mirror reflection (Mat_HdrReflection) ───────────────────
    // Float reflection radiance baked by the deferred mirror RTT, row-major
    // BGR (3 floats/texel), hdrReflW×hdrReflH. The transparent kernel samples
    // this emissively into g_hdrBuf so reflected highlights bloom instead of
    // clipping at the 8-bit panel texture. Raw pointer (Material is memcpy'd at
    // mirror-clone time, so no std::vector here); owned/managed by the RTT bake.
    float               * hdrRefl               = nullptr;
    int                   hdrReflW              = 0;
    int                   hdrReflH              = 0;

    // Tangent-space normal-map bitangent handedness: B = TbnHandedness·(N×T).
    // The per-vertex tangent (Lengyel) carries UV winding in its direction,
    // but the kernel reconstructs the bitangent as a fixed-sign cross product,
    // which is wrong on faces with MIRRORED UVs (negative UV determinant) —
    // their normal-map green/V detail inverts, producing a relief seam at the
    // boundary with positive-det faces. Faces are split onto a handedness=-1
    // material clone so the kernel can flip B per pixel. +1 = normal.
    float                 TbnHandedness          = 1.0f;

    // Tri-state override of the env-reflection probe qualification
    // (EnvReflection_FramePrep). 0 = auto: bake when Reflection > 0 or a
    // MetallicMap is present (the historical rule). 1 = force-bake: always
    // bake + publish a probe (the env term's strength still comes from the
    // Reflection scalar / metallic map, so forcing a probe on a 0-reflection
    // dielectric changes nothing visibly until one of those is dialed up).
    // -1 = off: never bake AND never publish a store for this material, even
    // with a metallic map. Engine-only (no LWO/FLD field) — persisted via the
    // scene sidecar as 'envRefl' (see MaterialImport_SetSurfaceProp).
    int8_t                EnvReflMode            = 0;

    // Tri-state override of the procedural-water composite for THIS surface
    // (only meaningful on the scene's water material — the matID registered
    // via SetDeferredWaterMatID). 0 = auto: follow the global
    // --water_procedural flag (city/chase default it on in their factories).
    // 1 = force the procedural fresnel/deep-colour composite; -1 = force the
    // legacy albedo+reflection blend. Engine-only (no LWO/FLD field) —
    // persisted via the scene sidecar as 'waterProcedural'. Same pattern as
    // EnvReflMode above; consumed by the deferred transparent kernel's
    // waterProc hoist and WaterProceduralEffective() (glint/albedo-warp pass).
    int8_t                WaterProcMode          = 0;

    // Per-surface env-probe bake FACE resolution override. 0 = unset → the
    // global chain (an EXPLICIT --env-bake-res, else the legacy
    // env_refl_res/2 probe sizing / the city stores' caller-chosen res).
    // Engine-only (no LWO/FLD field) — set via the editor's 'probe res'
    // control / the scene sidecar 'envBakeRes', validated to a power of two
    // in 64..1024 at set time (MaterialImport_SetSurfaceProp). Read at BAKE
    // time only (EnvBake.cpp); consumers read dims from the store itself, so
    // mixed-res stores coexist. Probes are SHARED between materials whose
    // centroids sit within 4 world units — for a shared store the LARGEST
    // per-material wish wins (rebake-on-upgrade). City per-building
    // registered stores size from the FIRST windows clone that registers the
    // building (materials map n:1 onto them) and cap at their 512² source.
    int                   EnvBakeRes             = 0;

    // Authored per-surface flag (ENVDYN Workstream A1, docs/
    // ENVDYN_DISPLACEMENT_PLAN.md): 1 = this material's env-reflection probe
    // opts into the LIVE dynamic-mesh overlay (the mech reflected in it), 0 =
    // static probe (default). Authored in the LWO 'RVSF' SURF sub-chunk (bit
    // 0x400) → FLD Surf_RevExt payload → here; also editor-settable via the
    // 'envDynamic' Material-panel checkbox. Nothing flagged → the whole
    // workstream is inert (byte-null). Consumed by EnvBake.cpp (store
    // retention A2 + the overlay pass A3), never by the static bake.
    int8_t                EnvDynamic             = 0;
};

#pragma pack(pop)

#endif //REVIVAL_MATERIAL_H
