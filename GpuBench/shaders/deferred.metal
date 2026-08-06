// Phase 3 of the standalone GPU benchmark (docs/GPU_BENCHMARK_PLAN.md):
// deferred G-buffer -> cube shadow maps -> PBR lighting -> ACES tonemap.
//
// Parity target is MEANING, not bytes. The shading model is the one greets
// actually runs (VERIFIED in GreetsApplyRunDefaults, GREETS.CPP:1107-1114):
// Cook-Torrance GGX D + Smith-Schlick G + Schlick F, the Karis split-sum
// analytic env BRDF, Fdez-Aguera multiscatter energy compensation, (1-F) diffuse
// energy weighting, and L2 SH irradiance in place of a flat ambient constant.
// All five are implemented here so the per-pixel cost is real; leaving any of
// them out would measure a cheaper shader than the CPU runs.
//
// Deliberately NOT reproduced (and why, in the plan §3): the packed
// mip|matID|swizzledUV G-buffer word, the PolyId shadow identity test, and the
// static-shadow lightmap. The last two mean the GPU takes every cube tap the CPU
// is allowed to skip, so the GPU here does MORE work per pixel, not less.

#include <metal_stdlib>
using namespace metal;

// ---------------------------------------------------------------------------
// shared types
// ---------------------------------------------------------------------------

struct FrameUniforms {
    float3 camRow0, camRow1, camRow2;   // view matrix rows (FDS Camera::Mat)
    float3 camSrc;                      // eye, world space
    float  sx, ox, sy, oy;              // clip.x = sx*X+ox*Z ; clip.y = sy*Y+oy*Z
    float  dza, dzb;                    // clip.z = dza*Z+dzb  (reversed-Z)
    float  invSx, invSy;                // reconstruction: X = (ndc.x-ox)*Z/sx
    float  nearZ, farZ;
    float  exposure;
    uint   numLights;
    uint   shadowsOn;
    float  ambientFactor, diffuseFactor, specularFactor;
    // MEASUREMENT-ONLY multiplier on every light's range. greets' authored omni
    // ranges are 3-20 units in a room spanning 60+, so at most review poses the
    // hard cutoff culls nearly every light for nearly every pixel. Scaling the
    // range up lets the per-light cost be measured instead of guessed. Changes
    // pixels; never a fidelity setting.
    float  lightRangeScale;
    // --viz_light: isolate ONE light in the shadow viz. Averaging over all
    // in-range lights is not a diagnostic -- lights embedded inside geometry (the
    // three mech-flare omnis sit inside the mech) shadow everything and drag the
    // mean down, which reads as "the shadow tap is broken" when it is not.
    int    vizLight;
};

struct BatchUniforms {
    float3 rotRow0, rotRow1, rotRow2;
    float3 objPos;
    float4 baseColor;      // .rgb authored base colour, .w = has-albedo flag
    float4 matParams;      // diffuse, specular, glossiness(0..1), luminosity
    float4 mapFlags;       // hasNormal, hasRough, aoInAlpha, parallaxScale
};

struct GpuLight {
    float3 pos;            // world
    float3 color;          // linear, already scaled by ISize
    float  range;          // HARD cutoff (Omni::IRange) — not inverse-square
    float  invRange;
    int    shadowIndex;    // -1 = none; cube slot for omnis, 2D slot for spots
    float  shadowNear, shadowFar;
    int    isSpot;         // Light_SpotLight (the 10 GreetsDisco cone spots)
    float  cosInner;       // Omni::HotSpot
    float  cosOuter;       // Omni::FallOff
    float3 dir;            // Omni::IDir, world
    // Spot shadow projection: Kick_Camera's view rows, with sRow0.w carrying
    // 1/tan(halfFov) (the same scale the bake's vertex shader applied).
    float4 sRow0, sRow1, sRow2;
};

struct VertexIn {
    float3 pos    [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv     [[attribute(2)]];
};

static inline float3 rowmul(float3 r0, float3 r1, float3 r2, float3 v) {
    return float3(dot(r0, v), dot(r1, v), dot(r2, v));
}

// Octahedral normal packing — same encoding the CPU G-buffer uses
// (FDS/FILLERS/Mekalele.h oct_encode_u16 / oct_decode_u16), so the G-buffer
// carries the same quantity, not a different one.
static inline float2 oct_encode(float3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 e = n.xy;
    if (n.z < 0.0f)
        e = (1.0f - abs(float2(n.y, n.x))) * select(float2(-1.0f), float2(1.0f), n.xy >= 0.0f);
    return e;
}
static inline float3 oct_decode(float2 e) {
    float3 n = float3(e, 1.0f - abs(e.x) - abs(e.y));
    const float t = saturate(-n.z);
    n.x += (n.x >= 0.0f) ? -t : t;
    n.y += (n.y >= 0.0f) ? -t : t;
    return normalize(n);
}

// ---------------------------------------------------------------------------
// G-buffer pass
// ---------------------------------------------------------------------------

struct GBufVertexOut {
    float4 position [[position]];
    float2 uv;
    float3 viewNormal;
    float3 viewPos;
};

struct GBufOut {
    float4 albedo [[color(0)]];   // rgb albedo, a = baked AO (Mat_AoInAlpha) else 1
    float2 normal [[color(1)]];   // oct-packed VIEW-space shading normal
    float4 params [[color(2)]];   // diffuse, specular, glossiness, luminosity/4
};

vertex GBufVertexOut vs_gbuffer(VertexIn in [[stage_in]],
                                constant FrameUniforms &u [[buffer(1)]],
                                constant BatchUniforms &b [[buffer(2)]])
{
    const float3 wp  = rowmul(b.rotRow0, b.rotRow1, b.rotRow2, in.pos) + b.objPos;
    const float3 rel = wp - u.camSrc;
    const float3 vp  = rowmul(u.camRow0, u.camRow1, u.camRow2, rel);

    GBufVertexOut o;
    o.position = float4(u.sx * vp.x + u.ox * vp.z,
                        u.sy * vp.y + u.oy * vp.z,
                        u.dza * vp.z + u.dzb,
                        vp.z);
    o.uv = in.uv;
    const float3 wn = rowmul(b.rotRow0, b.rotRow1, b.rotRow2, in.normal);
    o.viewNormal = rowmul(u.camRow0, u.camRow1, u.camRow2, wn);
    o.viewPos = vp;
    return o;
}

fragment GBufOut fs_gbuffer(GBufVertexOut in [[stage_in]],
                            constant BatchUniforms &b [[buffer(2)]],
                            texture2d<float> albedoTex [[texture(0)]],
                            texture2d<float> normalTex [[texture(1)]],
                            texture2d<float> roughTex  [[texture(2)]],
                            sampler samp [[sampler(0)]])
{
    float4 alb = float4(b.baseColor.rgb, 1.0f);
    if (b.baseColor.w > 0.5f) alb = albedoTex.sample(samp, in.uv);
    // AO lives in the albedo's alpha only when the material says so; otherwise
    // alpha is either cutout or meaningless, so force 1.
    const float ao = (b.mapFlags.z > 0.5f) ? alb.a : 1.0f;

    float3 n = normalize(in.viewNormal);
    if (b.mapFlags.x > 0.5f) {
        // TBN derived from screen-space derivatives. FDS computes per-vertex
        // tangents in DEMO/MISC/PREPROC.CPP (Compute_Vertex_Tangents), which is
        // not reachable from the loader, so we reconstruct instead. Noted in the
        // report: this is a different tangent basis, not the engine's.
        const float3 dpx = dfdx(in.viewPos), dpy = dfdy(in.viewPos);
        const float2 dux = dfdx(in.uv),      duy = dfdy(in.uv);
        const float det = dux.x * duy.y - duy.x * dux.y;
        if (abs(det) > 1e-12f) {
            const float3 t = normalize((duy.y * dpx - dux.y * dpy) / det);
            const float3 bt = normalize(cross(n, t));
            float3 tn = normalTex.sample(samp, in.uv).xyz * 2.0f - 1.0f;
            n = normalize(tn.x * t + tn.y * bt + tn.z * n);
        }
    }

    // Glossiness -> perceptual roughness. The CPU kernel carries Blinn
    // glossiness; a roughness MAP overrides it per pixel when present.
    float gloss = b.matParams.z;
    if (b.mapFlags.y > 0.5f) gloss = 1.0f - roughTex.sample(samp, in.uv).r;

    GBufOut o;
    o.albedo = float4(alb.rgb, ao);
    o.normal = oct_encode(n);
    o.params = float4(b.matParams.x, b.matParams.y, gloss,
                      saturate(b.matParams.w * 0.25f));
    return o;
}

// ---------------------------------------------------------------------------
// shadow pass — depth-only, one 90-degree face of an omni's cube
// ---------------------------------------------------------------------------

struct ShadowUniforms {
    float3 row0, row1, row2;   // face view rows (right, up, forward)
    float3 lightPos;
    float  dza, dzb;           // reversed-Z over [shadowNear, shadowFar]
    // 1/tan(halfFov). A cube face is 90 degrees so this is 1.0 and the xy pass
    // straight through; a disco cone spot is ~15.4 degrees total, so it is ~7.4.
    float  projScale;
};

vertex float4 vs_shadow(VertexIn in [[stage_in]],
                        constant ShadowUniforms &s [[buffer(1)]],
                        constant BatchUniforms  &b [[buffer(2)]])
{
    const float3 wp  = rowmul(b.rotRow0, b.rotRow1, b.rotRow2, in.pos) + b.objPos;
    const float3 rel = wp - s.lightPos;
    const float3 vp  = rowmul(s.row0, s.row1, s.row2, rel);
    return float4(vp.x * s.projScale, vp.y * s.projScale,
                  s.dza * vp.z + s.dzb, vp.z);
}

// ---------------------------------------------------------------------------
// PBR lighting pass
// ---------------------------------------------------------------------------

struct FsQuadOut {
    float4 position [[position]];
    float2 ndc;
};

vertex FsQuadOut vs_fullscreen(uint vid [[vertex_id]]) {
    // full-screen triangle
    const float2 p[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    FsQuadOut o;
    o.position = float4(p[vid], 0.0f, 1.0f);
    o.ndc = p[vid];
    return o;
}

// GGX / Trowbridge-Reitz normal distribution.
static inline float D_GGX(float NoH, float a) {
    const float a2 = a * a;
    const float d = NoH * NoH * (a2 - 1.0f) + 1.0f;
    return a2 / max(3.14159265f * d * d, 1e-7f);
}
// Smith-Schlick height-correlated visibility (G / (4 NoL NoV)).
static inline float V_SmithSchlick(float NoV, float NoL, float a) {
    const float k = a * 0.5f;
    const float gv = NoV * (1.0f - k) + k;
    const float gl = NoL * (1.0f - k) + k;
    return 0.25f / max(gv * gl, 1e-7f);
}
static inline float3 F_Schlick(float3 f0, float VoH) {
    const float f = pow(1.0f - VoH, 5.0f);
    return f0 + (1.0f - f0) * f;
}

// Karis split-sum ANALYTIC env BRDF (--env_brdf_analytic). Returns the (A,B)
// pair the multiscatter term below is built from, so the two ship together
// exactly as they do on the CPU (pbr_multiscatter is a no-op without this).
static inline float2 EnvBRDF_AB(float NoV, float rough) {
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 = float4(1.0f, 0.0425f, 1.04f, -0.04f);
    const float4 r = rough * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28f * NoV)) * r.x + r.y;
    return float2(-1.04f, 1.04f) * a004 + r.zw;
}

// L2 spherical-harmonic irradiance, 9 coefficients per channel. This is the
// --sh_ambient path; a flat constant would be cheaper AND a different shader, so
// the full evaluation runs even though our coefficients come from the FLD's
// authored zenith/nadir backdrop gradient rather than an env-cube bake.
static inline float3 SH_Irradiance(constant float4 *sh, float3 n) {
    const float c1 = 0.429043f, c2 = 0.511664f, c3 = 0.743125f,
                c4 = 0.886227f, c5 = 0.247708f;
    float3 r = c4 * sh[0].rgb
             + 2.0f * c2 * (sh[1].rgb * n.y + sh[2].rgb * n.z + sh[3].rgb * n.x)
             + 2.0f * c1 * (sh[4].rgb * n.x * n.y + sh[5].rgb * n.y * n.z + sh[7].rgb * n.z * n.x)
             + c3 * sh[6].rgb * n.z * n.z - c5 * sh[6].rgb
             + c1 * sh[8].rgb * (n.x * n.x - n.y * n.y);
    return max(r, 0.0f);
}

fragment float4 fs_lighting(FsQuadOut in [[stage_in]],
                            constant FrameUniforms &u   [[buffer(1)]],
                            constant GpuLight      *L   [[buffer(2)]],
                            constant float4        *sh  [[buffer(3)]],
                            texture2d<float>  gAlbedo   [[texture(0)]],
                            texture2d<float>  gNormal   [[texture(1)]],
                            texture2d<float>  gParams   [[texture(2)]],
                            depth2d<float>    gDepth    [[texture(3)]],
                            array<depthcube<float>, 16> shadowCubes [[texture(4)]],
                            array<depth2d<float>,   16> spotMaps    [[texture(20)]],
                            sampler           shadowSamp [[sampler(1)]])
{
    const uint2 px = uint2(in.position.xy);
    const float zEnc = gDepth.read(px);
    if (zEnc <= 0.0f) return float4(0.0f);        // reversed-Z: 0 == untouched (sky)

    // Reconstruct view position. Inverse of the vs projection above.
    const float Z = (u.dzb) / max(zEnc - u.dza, 1e-9f);
    const float X = (in.ndc.x - u.ox * 1.0f) * Z * u.invSx;
    const float Y = (in.ndc.y - u.oy * 1.0f) * Z * u.invSy;
    const float3 P = float3(X, Y, Z);

    const float4 alb = gAlbedo.read(px);
    const float2 ne  = gNormal.read(px).xy;
    const float4 par = gParams.read(px);
    const float3 N = oct_decode(ne);
    const float3 V = normalize(-P);
    const float NoV = saturate(dot(N, V));

    const float3 baseColor = alb.rgb;
    const float  ao        = alb.a;
    const float  diffuseK  = par.x * u.diffuseFactor;
    const float  specK     = par.y * u.specularFactor;
    const float  rough     = clamp(1.0f - par.z, 0.045f, 1.0f);
    const float  a         = rough * rough;
    const float  lum       = par.w * 4.0f;

    // Dielectric F0. The scene has no metallic map bound in this arm, so F0 is
    // the standard 4% and specK scales it — matching how the CPU kernel uses
    // Material::Specular.
    const float3 f0 = float3(0.04f) * max(specK, 0.0f) * 4.0f;

    const float2 ab = EnvBRDF_AB(NoV, rough);
    // Fdez-Aguera energy compensation, built from the SAME A,B terms.
    const float3 FssEss = f0 * ab.x + ab.y;
    const float  Ess = ab.x + ab.y;
    const float  Ems = 1.0f - Ess;
    const float3 Favg = f0 + (1.0f - f0) / 21.0f;
    const float3 Fms = FssEss * Favg / max(1.0f - Ems * Favg, 1e-5f);

    float3 radiance = 0.0f;

    for (uint i = 0; i < u.numLights; ++i) {
        const float3 lPosW = L[i].pos;
        // light position into view space
        const float3 lRel = lPosW - u.camSrc;
        const float3 lPos = rowmul(u.camRow0, u.camRow1, u.camRow2, lRel);
        const float3 toL = lPos - P;
        const float d2 = dot(toL, toL);
        const float range = L[i].range * u.lightRangeScale;
        if (d2 >= range * range) continue;          // HARD cutoff, as the CPU does
        const float d = sqrt(d2);
        const float3 Ldir = toL / max(d, 1e-6f);
        const float NoL = saturate(dot(N, Ldir));
        if (NoL <= 0.0f) continue;

        // Linear falloff to the hard edge — the engine's attenuation shape.
        float atten = saturate(1.0f - d * (L[i].invRange / u.lightRangeScale));

        // World position of the shaded point — needed by both the spot cone and
        // every shadow tap. The view matrix is orthonormal, so its transpose is
        // its inverse; hence the column gather.
        const float3 pw = u.camSrc
                        + float3(u.camRow0.x, u.camRow1.x, u.camRow2.x) * P.x
                        + float3(u.camRow0.y, u.camRow1.y, u.camRow2.y) * P.y
                        + float3(u.camRow0.z, u.camRow1.z, u.camRow2.z) * P.z;

        // SPOT cone. Identical shape to the CPU kernel
        // (DeferredSurfaceKernel.cpp:3247-3254): cosTheta measured between the
        // spot axis and the light->surface direction, HARD cutoff at cosOuter,
        // then a smoothstep t*t*(3-2t) from cosOuter up to cosInner.
        if (L[i].isSpot != 0) {
            const float3 toP = pw - lPosW;
            const float cosTheta = dot(L[i].dir, normalize(toP));
            if (cosTheta <= L[i].cosOuter) continue;
            if (cosTheta < L[i].cosInner) {
                const float tt = (cosTheta - L[i].cosOuter)
                               / max(L[i].cosInner - L[i].cosOuter, 1e-6f);
                atten *= tt * tt * (3.0f - 2.0f * tt);
            }
        }

        float shadow = 1.0f;
        if (u.shadowsOn != 0 && L[i].isSpot != 0 && L[i].shadowIndex >= 0) {
            // Single perspective depth map, as Shadows.cpp bakes for a spot
            // (cubeFace < 0 => one map, not six faces).
            const float3 rel = pw - lPosW;
            const float3 vp = rowmul(L[i].sRow0.xyz, L[i].sRow1.xyz, L[i].sRow2.xyz, rel);
            if (vp.z > L[i].shadowNear) {
                const float sn = L[i].shadowNear, sf = L[i].shadowFar;
                const float dza = -sn / (sf - sn), dzb = sn * sf / (sf - sn);
                const float ref = dza + dzb / vp.z;
                // Same 1/tan(halfFov) the bake's vertex shader applied. NDC -> uv,
                // y flipped for Metal's upper-left texture origin.
                const float ps = L[i].sRow0.w;
                const float2 ndc = vp.xy * ps / vp.z;
                const float2 uv = float2(0.5f + 0.5f * ndc.x, 0.5f - 0.5f * ndc.y);
                const float bias = mix(0.0025f, 0.0004f, NoL);
                shadow = spotMaps[L[i].shadowIndex].sample_compare(shadowSamp, uv,
                                                                  ref + bias);
            }
        } else if (u.shadowsOn != 0 && L[i].shadowIndex >= 0) {
            // World-space direction from light to the shaded point. The GPU takes
            // this tap unconditionally; the CPU is allowed to skip it on static
            // surfaces via the static-shadow lightmap (plan §5.3 item 12).
            const float3 dirW = pw - lPosW;
            const float sn = L[i].shadowNear, sf = L[i].shadowFar;
            // Same reversed-Z encoding the bake wrote, evaluated on the MAJOR
            // axis distance (which is what the 90-degree face's w equals).
            const float major = max(max(abs(dirW.x), abs(dirW.y)), abs(dirW.z));
            const float dza = -sn / (sf - sn), dzb = sn * sf / (sf - sn);
            const float ref = dza + dzb / max(major, 1e-5f);
            // slope-scaled bias in reversed-Z (bigger value == nearer)
            const float bias = mix(0.0025f, 0.0004f, NoL);
            shadow = shadowCubes[L[i].shadowIndex].sample_compare(
                shadowSamp, normalize(dirW), ref + bias);
        }

        const float3 H = normalize(Ldir + V);
        const float NoH = saturate(dot(N, H));
        const float VoH = saturate(dot(V, H));

        const float3 F = F_Schlick(f0, VoH);
        const float  Dv = D_GGX(NoH, a) * V_SmithSchlick(NoV, NoL, a);
        const float3 spec = F * Dv;
        // (1-F) diffuse energy weighting (--diffuse_energy), using the same
        // per-pixel Fresnel the specular term produced.
        //
        // NO Lambert 1/pi. That is deliberate and it is PARITY, not a slip: the
        // CPU kernel's direct diffuse is
        //     intensity = NoL * atten * Material::Diffuse;  lR += intensity*colR
        // (DeferredSurfaceKernel.cpp:3245-3258, and the --pbr path at 2568 only
        // adds the (1-F) factor). Its only 1/pi terms are inside the GGX D
        // (lines 402/442/2431), which is standard. Dividing here made every
        // direct light pi times dimmer than the reference image — measured: the
        // 11 disco lights moved the frame by at most 2/255 even from 5 units away.
        const float3 diff = baseColor * diffuseK * (1.0f - F);

        radiance += (diff + spec) * L[i].color * (NoL * atten * shadow);
    }

    // Ambient: L2 SH irradiance x AO, plus the multiscatter env term. AO
    // multiplies the AMBIENT only — direct light keeps its own shadows.
    //
    // The SH coefficients are projected from the FLD's authored WORLD-space
    // zenith/nadir backdrop gradient, so the normal has to go back to world
    // before evaluation. The view matrix is orthonormal, so its transpose is its
    // inverse — hence the column gather.
    const float3 Nw = normalize(float3(u.camRow0.x, u.camRow1.x, u.camRow2.x) * N.x
                              + float3(u.camRow0.y, u.camRow1.y, u.camRow2.y) * N.y
                              + float3(u.camRow0.z, u.camRow1.z, u.camRow2.z) * N.z);
    const float3 irr = SH_Irradiance(sh, Nw) * u.ambientFactor;
    radiance += baseColor * irr * ao;
    radiance += (FssEss + Fms * Ems) * irr * ao;
    // Emissive
    radiance += baseColor * lum;

    return float4(radiance, 1.0f);
}

// ---------------------------------------------------------------------------
// tonemap
// ---------------------------------------------------------------------------

// ACES filmic approximation (Narkowicz), matching the shape of FDS/RENDER/Hdr.cpp.
static inline float3 ACES(float3 x) {
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

fragment float4 fs_tonemap(FsQuadOut in [[stage_in]],
                           constant FrameUniforms &u [[buffer(1)]],
                           texture2d<float> hdr [[texture(0)]])
{
    float3 c = hdr.read(uint2(in.position.xy)).rgb;
    c = ACES(c * u.exposure);
    // hdr_linear: the buffer is linear, so encode on the way out.
    c = sqrt(c);
    return float4(c, 1.0f);
}

// ---------------------------------------------------------------------------
// debug visualisations — a ground-truth instrument needs per-stage verification,
// not just a final image. --viz=<mode> selects.
// ---------------------------------------------------------------------------

fragment float4 fs_viz(FsQuadOut in [[stage_in]],
                       constant FrameUniforms &u [[buffer(1)]],
                       constant GpuLight      *L [[buffer(2)]],
                       texture2d<float> gAlbedo  [[texture(0)]],
                       texture2d<float> gNormal  [[texture(1)]],
                       texture2d<float> gParams  [[texture(2)]],
                       depth2d<float>   gDepth   [[texture(3)]],
                       array<depthcube<float>, 16> shadowCubes [[texture(4)]],
                       array<depth2d<float>,   16> spotMaps    [[texture(20)]],
                       sampler shadowSamp [[sampler(1)]],
                       sampler rawSamp    [[sampler(2)]],
                       constant uint &mode [[buffer(4)]])
{
    const uint2 px = uint2(in.position.xy);
    const float zEnc = gDepth.read(px);
    // NO G-BUFFER here (sky / beyond the far plane). Must NOT be black: mode 5
    // uses black for "lit but fully shadowed", and conflating the two makes an
    // uncovered pixel read as a shadow. Same rule that got white split out
    // earlier -- never encode "no data" as a value the mode already means.
    if (zEnc <= 0.0f) return float4(0.40f, 0.0f, 0.0f, 1);   // dark RED = no geometry

    const float Z = (u.dzb) / max(zEnc - u.dza, 1e-9f);
    const float X = (in.ndc.x - u.ox) * Z * u.invSx;
    const float Y = (in.ndc.y - u.oy) * Z * u.invSy;
    const float3 P = float3(X, Y, Z);
    const float4 alb = gAlbedo.read(px);
    const float3 N = oct_decode(gNormal.read(px).xy);
    const float4 par = gParams.read(px);

    switch (mode) {
        case 0: return float4(sqrt(alb.rgb), 1);                    // albedo
        case 1: return float4(N * 0.5f + 0.5f, 1);                  // view normal
        case 2: return float4(float3(alb.a), 1);                    // baked AO
        case 3: return float4(float3((Z - u.nearZ) / (u.farZ - u.nearZ)), 1);
        case 4: return float4(float3(par.z), 1);                    // glossiness
        case 7: {
            // DIAGNOSTIC: is the baked cube depth in the space the tap assumes?
            // Decode the stored depth back to a WORLD DISTANCE and compare it to
            // the actual distance along the major axis. If the face mapping and
            // depth encoding agree, storedDist <= actualDist everywhere, with
            // equality on surfaces directly visible from the light. Encoded as
            // R = actualDist/40, G = storedDist/40, B = 0. Reading numbers beats
            // reasoning about conventions.
            const int li = max(u.vizLight, 0);
            if (li >= int(u.numLights) || L[li].shadowIndex < 0) return float4(0,0,1,1);
            const float3 pw7 = u.camSrc
                + float3(u.camRow0.x, u.camRow1.x, u.camRow2.x) * P.x
                + float3(u.camRow0.y, u.camRow1.y, u.camRow2.y) * P.y
                + float3(u.camRow0.z, u.camRow1.z, u.camRow2.z) * P.z;
            const float3 dirW = pw7 - L[li].pos;
            const float sn = L[li].shadowNear, sf = L[li].shadowFar;
            const float dza = -sn / (sf - sn), dzb = sn * sf / (sf - sn);
            const float major = max(max(abs(dirW.x), abs(dirW.y)), abs(dirW.z));
            const float stored = shadowCubes[L[li].shadowIndex].sample(rawSamp, normalize(dirW));
            const float storedDist = (stored > dza + 1e-9f) ? dzb / (stored - dza) : 1e9f;
            // R = actualDist/40, G = storedDist/40, B = storedDist/2 (fine scale,
            // to resolve whether the light is EMBEDDED in geometry)
            return float4(saturate(major / 40.0f), saturate(storedDist / 40.0f),
                          saturate(storedDist * 0.5f), 1);
        }
        case 6: {   // lights reaching this pixel, normalised by the light count
            uint hit = 0;
            const float3 pw6 = u.camSrc
                + float3(u.camRow0.x, u.camRow1.x, u.camRow2.x) * P.x
                + float3(u.camRow0.y, u.camRow1.y, u.camRow2.y) * P.y
                + float3(u.camRow0.z, u.camRow1.z, u.camRow2.z) * P.z;
            uint considered = 0;
            for (uint i = 0; i < u.numLights; ++i) {
                if (u.vizLight >= 0 && int(i) != u.vizLight) continue;
                ++considered;
                const float r = L[i].range * u.lightRangeScale;
                const float3 dw = pw6 - L[i].pos;
                if (length(dw) >= r) continue;
                if (L[i].isSpot != 0 &&
                    dot(L[i].dir, normalize(dw)) <= L[i].cosOuter) continue;
                ++hit;
            }
            const float f = considered ? float(hit) / float(considered) : 0.0f;
            // green = some light, red channel scales with fraction, black = none
            return float4(f, f > 0.0f ? 0.35f + 0.65f * f : 0.0f, 0.0f, 1.0f);
        }
        default: break;
    }

    // mode 5: shadow state, THREE distinct colours. The previous encoding
    // returned white both for "lit and unshadowed" and for "no light in range",
    // which made a scene whose lights are mostly out of range look like a broken
    // shadow test. Never conflate "no data" with "the value is 1".
    //   dark blue  = NO shadow-casting light within range of this pixel
    //   white      = lit, fully unshadowed
    //   black      = lit, fully shadowed
    //   grey       = partial (PCF edge / several lights disagreeing)
    float acc = 0.0f;
    uint n = 0;
    const float3 pw = u.camSrc
                    + float3(u.camRow0.x, u.camRow1.x, u.camRow2.x) * P.x
                    + float3(u.camRow0.y, u.camRow1.y, u.camRow2.y) * P.y
                    + float3(u.camRow0.z, u.camRow1.z, u.camRow2.z) * P.z;
    for (uint i = 0; i < u.numLights; ++i) {
        if (u.vizLight >= 0 && int(i) != u.vizLight) continue;
        if (L[i].shadowIndex < 0) continue;
        const float3 dirW = pw - L[i].pos;
        if (length(dirW) >= L[i].range * u.lightRangeScale) continue;
        const float sn = L[i].shadowNear, sf = L[i].shadowFar;
        const float dza = -sn / (sf - sn), dzb = sn * sf / (sf - sn);
        if (L[i].isSpot != 0) {
            // Outside the cone is NOT "in range" for a spot — counting it would
            // report the whole 38-unit sphere as unlit and drown the 7-degree beam.
            if (dot(L[i].dir, normalize(dirW)) <= L[i].cosOuter) continue;
            const float3 vp = rowmul(L[i].sRow0.xyz, L[i].sRow1.xyz, L[i].sRow2.xyz, dirW);
            if (vp.z <= sn) continue;
            const float ref = dza + dzb / vp.z;
            const float2 ndc = vp.xy * L[i].sRow0.w / vp.z;
            const float2 uv = float2(0.5f + 0.5f * ndc.x, 0.5f - 0.5f * ndc.y);
            acc += spotMaps[L[i].shadowIndex].sample_compare(shadowSamp, uv, ref + 0.0015f);
            ++n;
            continue;
        }
        const float major = max(max(abs(dirW.x), abs(dirW.y)), abs(dirW.z));
        const float ref = dza + dzb / max(major, 1e-5f);
        acc += shadowCubes[L[i].shadowIndex].sample_compare(
            shadowSamp, normalize(dirW), ref + 0.0015f);
        ++n;
    }
    if (n == 0) return float4(0.05f, 0.10f, 0.35f, 1);   // no light in range
    return float4(float3(acc / float(n)), 1);
}

// ---------------------------------------------------------------------------
// omni flare sprites
// ---------------------------------------------------------------------------
//
// These are the bright pools in the DEMO reference — NOT omni surface lighting.
// Reproduced from FDS/FILLERS/FILLERS.CPP's The_MMX_Scalar + Spriter:
//
//   Size     = ImageSize * (1/z) * perspX * FlareSize     (FlareSize = ISize*FlareScale)
//   edgeLen  = 2*Size,  and Spriter treats its width/height argument as the
//              HALF-extent (x1 = x - width), so the sprite spans +/- 2*Size px.
//   colour   = the FLARE TEXTURE's rgb, unmodulated (Col = 0xFFFFFF; greets has
//              no fog, so the fog tint never applies).
//   blend    = ADDITIVE, and under --hdr it adds into the float radiance buffer
//              rather than the 8-bit saturating store (Spriter<Res,true,true>).
//   z test   = per pixel, against the sprite's SINGLE centre depth: it draws only
//              where it is NEARER than the stored geometry.
//
// Not reproduced: the mirror-mask footprint test (no mirrors in this arm) and
// Omni_Rand's +/-10% size jitter (greets' omnis are not Omni_Rand).

struct FlareUniforms {
    float4 centerPx;     // .xy screen px, .z view z, .w half-extent in px
    float4 gain;         // .x intensity multiplier, .zw = target resolution
};

struct FlareOut {
    float4 position [[position]];
    float2 uv;
    float  refZ;         // sprite depth in the G-buffer's reversed-Z encoding
};

vertex FlareOut vs_flare(uint vid [[vertex_id]],
                         constant FrameUniforms &u [[buffer(1)]],
                         constant FlareUniforms &f [[buffer(2)]])
{
    // two triangles, CCW, unit quad
    const float2 c[6] = {float2(-1,-1), float2(1,-1), float2(-1,1),
                         float2(1,-1),  float2(1,1),  float2(-1,1)};
    const float2 o = c[vid];
    const float2 px = f.centerPx.xy + o * f.centerPx.w;
    FlareOut out;
    // px -> NDC. Metal's viewport origin is upper-left, so y flips.
    out.position = float4(2.0f * px.x / f.gain.z - 1.0f,
                          1.0f - 2.0f * px.y / f.gain.w,
                          0.0f, 1.0f);
    out.uv = o * 0.5f + 0.5f;
    out.refZ = u.dza + u.dzb / max(f.centerPx.z, 1e-4f);
    return out;
}

fragment float4 fs_flare(FlareOut in [[stage_in]],
                         constant FrameUniforms &u [[buffer(1)]],
                         constant FlareUniforms &f [[buffer(2)]],
                         texture2d<float> flareTex [[texture(0)]],
                         depth2d<float>   gDepth   [[texture(1)]],
                         sampler samp [[sampler(0)]])
{
    const float zEnc = gDepth.read(uint2(in.position.xy));
    // Reversed-Z: bigger == nearer. The sprite draws only where it is in FRONT of
    // the stored geometry, which is Spriter's `if (Z <= ZPage16[..]) skip`.
    if (zEnc > 0.0f && in.refZ <= zEnc) discard_fragment();
    const float3 c = flareTex.sample(samp, in.uv).rgb;
    return float4(c * f.gain.x, 1.0f);
}
