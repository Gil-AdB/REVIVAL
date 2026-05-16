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
};

#pragma pack(pop)

#endif //REVIVAL_MATERIAL_H
