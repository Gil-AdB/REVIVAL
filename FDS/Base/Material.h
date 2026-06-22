#ifndef REVIVAL_MATERIAL_H
#define REVIVAL_MATERIAL_H

#include "Color.h"
#include "Texture.h"
#include "Vector.h"

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
    Material            * Next                  = nullptr;
    Material            * Prev                  = nullptr;
    char                * Name                  = nullptr;
    // ─── COLD: reflection / refraction (rarely sampled) ─────────────
    float                 Reflection            = 0.0f;
    char                * ReflectionImage       = nullptr;
    float                 ReflectionSeamAngle   = 0.0f;
    float                 RefractiveIndex       = 0.0f;
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
};

#pragma pack(pop)

#endif //REVIVAL_MATERIAL_H
