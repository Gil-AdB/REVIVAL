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
};

#pragma pack(pop)

#endif //REVIVAL_MATERIAL_H
