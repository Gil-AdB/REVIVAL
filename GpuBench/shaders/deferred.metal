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
    // Oblique clip for the mirror REFLECTION pass: fragments with
    // dot(world, clipPlane.xyz) + clipPlane.w < 0.05 are discarded, exactly the
    // sd > 0.05 face gate the CPU's BuildMirror clone applies. Disabled when
    // xyz == 0 (the main pass).
    float4 clipPlane;
    // Number of live reflection textures in this lighting pass. The MAIN pass
    // sets it to the mirror count; a FIRST-ORDER reflection pass sets it too
    // when --mirror2 is on (that is what puts a mirror inside a mirror); a
    // SECOND-ORDER pass always sets 0 — order 2 is the ceiling, exactly as the
    // CPU caps it by hiding every clone mesh inside an RTT bake
    // (docs/MIRROR_RECURSION_PLAN.md:18-22).
    uint   mirrorCount;
    // Environment reflection. envReflGain is FeatureFlags env_refl_gain (1.0);
    // the AABB is the scene bound the parallax correction uses; envProbePos
    // carries each probe's bake point, needed to re-aim the lookup after the
    // parallax hit is found.
    float  envReflGain;
    // 1/width, 1/height OF THIS PASS's own render target, set only when the
    // bound reflTex is a different size (the order-2 targets, which render at
    // --mirror2_scale). position.xy * reflUv is then the [0,1] screen
    // coordinate and the sampler resolves the size difference. Zero means
    // "same size as this pass" and the composite stays an integer same-pixel
    // read(), so the main view is bit-for-bit what it was before order 2
    // existed. Occupies the two floats that used to be explicit padding here,
    // so the struct layout does not move.
    float2 reflUv;
    float4 aabbMin, aabbMax;
    float4 envProbePos[8];
    // CONDUCTOR PARITY DIALS, measurement only (Deferred.h): .x = 1 keeps the
    // diffuse lobe on metal (the CPU's HDR-frame behaviour, contract D1),
    // .y = 1 tints the metal highlight by the GAMMA albedo (contract D2).
    float4 metalCompat;
    // FLAT AMBIENT. `sh_ambient` defaults 0 (FeatureFlags.def:47) and ONLY
    // greets sets it (GREETS.CPP:1175), so greets is the only scene whose CPU
    // reference evaluates L2 SH irradiance. Every other scene runs
    // DeferredSurfaceKernel.cpp:1761-1768's flat branch,
    //     lB = Luminosity*255 + Diffuse * Sc->Ambient.B
    // with NO Ambient_Factor (dead in FDS — SHADING_CONTRACT.md D7).
    // .rgb = Scene::Ambient/255, .w = 1 selects the flat branch.
    float4 flatAmbient;
    // WHICH HDR COMPOSITE this scene's reference is built from. `hdr_linear`
    // defaults 0 (FeatureFlags.def:343) and only greets setDefaults it
    // (GREETS.CPP:1178). .x = 1 -> DeferredSurfaceKernel.cpp:2625's
    //     rl = albedo^2 * l + s,   tonemap ... then sqrt   (Hdr.cpp:847-851)
    // .x = 0 -> :2587's
    //     h  = fd + s = (albedo * l)/256 * (1-metal) + s,  NO sqrt on the way out.
    // The difference is not cosmetic: albedo^2 against albedo^1 darkens every
    // mid-tone, and the missing output encode changes the whole tone curve.
    float4 hdrMode;
};

struct BatchUniforms {
    float3 rotRow0, rotRow1, rotRow2;
    float3 objPos;
    float4 baseColor;      // .rgb authored base colour, .w = has-albedo flag
    float4 matParams;      // diffuse, specular, lobe roughness, luminosity
    float4 mapFlags;       // hasNormal, hasRough, aoInAlpha, parallaxScale
    // .x mirror panel index (1-based; 0 = none), .y hasAoMap, .z hasMetalMap,
    // .w AO strength (--ao_map_strength x Material::AoStrength)
    float4 misc;
    // .x env probe index (1-based; 0 = this material reflects nothing),
    // .y Material::Reflection, .z Material::XparBlendAlpha, .w the destination
    // weight dw of the transparent composite (Transparency*0.01, else 0.5).
    float4 misc2;
    // TRANSPARENT path only (see fs_xpar): .x raw Material::Luminosity
    // (UNCLAMPED — the transparent kernel writes Luminosity*255 with no cap,
    // where the deferred params plane clamps at 4), .y Material::Specular,
    // .z Material::Glossiness, .w 1 = additive (Mat_Additive).
    float4 xpar;
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
    float3 pos     [[attribute(0)]];
    float3 normal  [[attribute(1)]];
    float2 uv      [[attribute(2)]];
    // The ENGINE's per-corner tangent (object space) + the per-face UV-winding
    // handedness sign in w: B = w * (N x T), the deferred kernel's
    // Mat->TbnHandedness convention (DeferredSurfaceKernel.cpp:1711-1719).
    float4 tangent [[attribute(3)]];
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
    float4 viewTangent;   // xyz view-space engine tangent, w handedness sign
    float3 worldPos;      // for the reflection pass's oblique clip
};

struct GBufOut {
    // rgb albedo, a = AO already reduced by the strength dial (see fs_gbuffer)
    float4 albedo [[color(0)]];
    float2 normal [[color(1)]];   // oct-packed VIEW-space shading normal
    float4 params [[color(2)]];   // diffuse, specular, lobe rough, luminosity/4
    // .x mirror panel id (1-based; 0 = none), .y metalness x 255,
    // .z ENV PROBE index (1-based; 0 = none), .w F0 x 255.
    // F0 is resolved HERE rather than in the lighting pass because it needs the
    // per-material Reflection AND the per-texel metalness, and the G-buffer is
    // where the two meet: F0 = max(Reflection*0.01, 0.04) then lerped toward
    // 0.98 by metalness — DeferredSurfaceKernel.cpp:1252-1260.
    uint4  mirror [[color(3)]];
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
    const float3 wt = rowmul(b.rotRow0, b.rotRow1, b.rotRow2, in.tangent.xyz);
    o.viewTangent = float4(rowmul(u.camRow0, u.camRow1, u.camRow2, wt),
                           in.tangent.w);
    o.viewPos = vp;
    o.worldPos = wp;
    return o;
}

fragment GBufOut fs_gbuffer(GBufVertexOut in [[stage_in]],
                            constant FrameUniforms &u [[buffer(1)]],
                            constant BatchUniforms &b [[buffer(2)]],
                            texture2d<float> albedoTex [[texture(0)]],
                            texture2d<float> normalTex [[texture(1)]],
                            texture2d<float> roughTex  [[texture(2)]],
                            texture2d<float> aoTex     [[texture(3)]],
                            texture2d<float> metalTex  [[texture(4)]],
                            sampler samp [[sampler(0)]])
{
    // Reflection pass: clip everything at/behind the mirror plane — the same
    // sd > 0.05 gate BuildMirror's clone applies, and what keeps the panel
    // itself (sd == 0) out of its own reflection.
    if (dot(u.clipPlane.xyz, u.clipPlane.xyz) > 0.0f &&
        dot(in.worldPos, u.clipPlane.xyz) + u.clipPlane.w < 0.05f)
        discard_fragment();

    float4 alb = float4(b.baseColor.rgb, 1.0f);
    if (b.baseColor.w > 0.5f) alb = albedoTex.sample(samp, in.uv);
    // AO, in the CPU's priority order (DeferredSurfaceKernel.cpp:1848-1918):
    //   1. Mat_AoInAlpha — baked into the albedo's alpha (free);
    //   2. Material::AoMap — a separate map (the RVSM 'ao' role);
    //   3. neither -> fully open.
    // Then the STRENGTH dial, which the CPU applies as
    //   ao' = 1 - ao_map_strength * Material::AoStrength * (1 - ao)
    // with ao_map_strength defaulting to 2.0 — i.e. occlusion is EXAGGERATED
    // 2x by default. Folding it here (rather than at the point of use) keeps
    // the G-buffer's alpha meaning "the occlusion the frame will apply".
    float aoRaw = 1.0f;
    if      (b.mapFlags.z > 0.5f) aoRaw = alb.a;
    else if (b.misc.y     > 0.5f) aoRaw = aoTex.sample(samp, in.uv).r;
    const float ao = saturate(1.0f - b.misc.w * (1.0f - aoRaw));

    float3 n = normalize(in.viewNormal);
    if (b.mapFlags.x > 0.5f) {
        // The ENGINE's tangent frame, term for term the deferred kernel's
        // normal-map path (DeferredSurfaceKernel.cpp:1680-1727): per-vertex
        // tangent (Compute_Vertex_Tangents, ingested), re-orthogonalized
        // against the interpolated N; Mikkelsen axis fallback when degenerate;
        // B = handedness * (N x T) — the sign is the per-face UV-winding
        // handedness the CPU realises as the ::mirUV material clones
        // (Material::TbnHandedness). The previous screen-space-derivative TBN
        // inverted the V-axis relief detail on every mirrored-UV chart.
        float3 t = in.viewTangent.xyz;
        t -= n * dot(n, t);
        const float tLen2 = dot(t, t);
        if (tLen2 > 1e-12f) {
            t *= rsqrt(tLen2);
        } else {
            const float3 ref = (abs(n.y) < 0.9f) ? float3(0, 1, 0)
                                                 : float3(1, 0, 0);
            t = normalize(cross(ref, n));
        }
        const float3 bt = cross(n, t) * in.viewTangent.w;
        float3 tn = normalTex.sample(samp, in.uv).xyz * 2.0f - 1.0f;
        n = normalize(tn.x * t + tn.y * bt + tn.z * n);
    }

    // Roughness map semantics, the CPU's exactly
    // (DeferredSurfaceKernel.cpp:2525-2539 + FeatureFlags.def:169): the map
    // attenuates SPECULAR MAGNITUDE — specMul = max(0, 1 - strength*texel),
    // strength default 1.0 — it does NOT widen the GGX lobe. The lobe
    // roughness is per-material, host-derived from Glossiness
    // (rough = sqrt(2/(gloss+2))), and rides matParams.z unchanged.
    float spec = b.matParams.y;
    if (b.mapFlags.y > 0.5f)
        spec *= max(0.0f, 1.0f - roughTex.sample(samp, in.uv).r);

    // Metalness (--metal_map, default ON). The CPU kills diffuse on metal
    // pixels and tints the highlight by the albedo
    // (DeferredSurfaceKernel.cpp:2512-2545).
    const float metal = (b.misc.z > 0.5f) ? metalTex.sample(samp, in.uv).r : 0.0f;

    GBufOut o;
    o.albedo = float4(alb.rgb, ao);
    o.normal = oct_encode(n);
    // LUMINOSITY encoding. This plane is RGBA8Unorm, so the emissive has to be
    // squeezed into 8 bits. It used to be saturate(Lum*0.25) with a *4 decode —
    // a hard clamp at Lum = 4, which is fine for greets (hottest 2.25) and
    // catastrophic for FOUNTAIN, whose 'daimond', 'spc1 in engine' and
    // 'in engine' materials author Luminosity = 100 (MEASURED via the ingest's
    // own emissive census): they rendered at 4, i.e. 25x too dim.
    // sqrt(Lum/128) with a squared decode covers 0..128 and puts the precision
    // where the values are: at Lum 2.25 the round trip is 2.274 (1.1 % error),
    // at Lum 100 it is 99.6 (0.4 %). A linear /128 encoding would have made
    // greets' 2.25 land on 2.5.
    o.params = float4(b.matParams.x, spec, b.matParams.z,
                      sqrt(saturate(b.matParams.w * (1.0f / 128.0f))));
    float f0 = max(b.misc2.y * 0.01f, 0.04f);
    f0 = f0 + (0.98f - f0) * saturate(metal);
    o.mirror = uint4(uint(b.misc.x + 0.5f), uint(saturate(metal) * 255.0f + 0.5f),
                     uint(b.misc2.x + 0.5f), uint(saturate(f0) * 255.0f + 0.5f));
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
// (The Karis split-sum env BRDF + Fdez-Aguera multiscatter helpers that used
// to live here are deliberately GONE: on the CPU they run only inside
// EnvSpecComposeScalar, which needs --env_refl AND a material with
// Reflection > 0 / a metallic map — a pano path this arm does not implement.
// Applying them as a whole-frame sky term was over-lighting, measured at
// +24 mean luma once the roughness mapping was corrected.)

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

// ---------------------------------------------------------------------------
// SHARED shading kernels. fs_lighting and fs_viz MUST both call these.
//
// WHY THIS EXISTS AT ALL: --viz=direct (mode 10) used to carry its own copy of
// the light loop, and that copy never applied the shadow factor even though its
// comment claimed "with shadows". It was therefore byte-identical with and
// without --no-shadows, and a whole round of shadow debugging was steered by a
// diagnostic that could not fail its own test. That is the THIRD instrument in
// this arm that reported a value where it had no data (the other two:
// --viz=shadow returning white for both "unshadowed" and "no light in range";
// mode 5 returning black for both "fully shadowed" and "no G-buffer").
//
// The structural fix is not "add the missing line" — it is to make it impossible
// for a viz mode to compute a DIFFERENT quantity from the one the frame uses.
// One decode, one shadow tap, one direct sum, one ambient term, called from both
// entry points.
// ---------------------------------------------------------------------------

struct Surface {
    float3 P;            // view-space position
    float3 pw;           // world-space position
    float3 N;            // view-space shading normal
    float3 V;            // view-space view vector
    float  NoV;
    float3 baseColor;    // LINEAR albedo (gamma texel squared — see below)
    float  ao;
    float  rawAlpha;     // the G-buffer alpha as stored, for the AO viz
    float  diffuseK, specK, rough, a, lum;
    float  metal;        // MetallicMap: kills diffuse, tints the highlight
};

static inline Surface DecodeSurface(constant FrameUniforms &u,
                                    float4 alb, float2 ne, float4 par, float3 P,
                                    float metal = 0.0f)
{
    Surface S;
    S.P = P;
    // VIEW -> WORLD. vs_gbuffer builds view space as
    //     P = Mat * (world - camSrc),   Mat's ROWS being (right, up, forward),
    // so the inverse is world = camSrc + Mat^T * P, and
    //     (Mat^T * P) = P.x*row0 + P.y*row1 + P.z*row2
    // — a combination of the ROWS AS VECTORS.
    //
    // THIS WAS THE DIRECT-LIGHTING BUG (plan §6.2b). The previous code gathered
    // the matrix's COLUMNS, i.e. it computed Mat*P — the FORWARD transform a
    // second time — under a comment asserting it was the transpose. The result
    // is a rotated world position, so every cube-shadow direction, every spot
    // cone test and the SH ambient's world normal were evaluated along the wrong
    // ray. MEASURED at the primary pose: a screen-centre pixel reconstructed
    // 4.8 units from the eye along (0.217, 0.129, 0.973) — the view matrix's
    // third COLUMN — where the camera's forward is (-0.207, -0.141, 0.968).
    // Same magnitude, X and Y mirrored: the signature of the wrong gather.
    S.pw = u.camSrc + u.camRow0 * P.x + u.camRow1 * P.y + u.camRow2 * P.z;
    S.N = oct_decode(ne);
    S.V = normalize(-P);
    S.NoV = saturate(dot(S.N, S.V));

    // ALBEDO IS SQUARED HERE, and that is parity, not a guess. Under
    // --hdr_linear the CPU kernel "square[s] the (normalized) albedo and let[s]
    // light enter at power 1" (DeferredSurfaceKernel.cpp:1374-1381), then
    // re-encodes with sqrt on the way out — which is exactly what fs_tonemap
    // does. The G-buffer stores the raw GAMMA texel (as the CPU's does), so the
    // square belongs at the point of use.
    //
    // WITHOUT --hdr_linear (every scene but greets — see FrameUniforms::hdrMode)
    // the CPU composites `fdB = texB*lB/256` at :2509 and writes `hB = fdB + sB`
    // at :2587: the albedo enters at power ONE, in gamma, and the tonemap does
    // not re-encode. So the square is CONDITIONAL on the same flag the CPU's is.
    S.baseColor = (u.hdrMode.x > 0.5f) ? (alb.rgb * alb.rgb) : alb.rgb;
    S.ao        = alb.a;
    S.rawAlpha  = alb.a;
    S.diffuseK  = par.x * u.diffuseFactor;
    S.specK     = par.y * u.specularFactor;
    // par.z IS the GGX lobe roughness now (host: sqrt(2/(gloss+2)), the CPU's
    // own mapping at DeferredSurfaceKernel.cpp:1806-1809). par.y already
    // carries the roughness map's magnitude attenuation.
    S.rough     = clamp(par.z, 0.04f, 1.0f);
    S.a         = S.rough * S.rough;
    S.lum       = par.w * par.w * 128.0f;   // inverse of fs_gbuffer's sqrt(Lum/128)
    S.metal     = metal;
    // No F0 / env-BRDF / multiscatter precompute: the direct spec uses the
    // CPU's fixed dielectric F = 0.04 + 0.96*(1-VoH)^5 scaled by Specular, and
    // the ambient is a pure irradiance skylight (see AmbientRadiance).
    return S;
}

// The ONE shadow tap. Both the lit frame and --viz=shadow call this, with the
// same bias, so the viz can never report a different occlusion than the frame
// applies. Returns 1 when the light is unoccluded, 0 when fully occluded.
static inline float ShadowFactor(constant GpuLight &Li, float3 pw, float NoL,
                                 array<depthcube<float>, 16> shadowCubes,
                                 array<depth2d<float>,   16> spotMaps,
                                 sampler shadowSamp)
{
    if (Li.shadowIndex < 0) return 1.0f;
    const float sn = Li.shadowNear, sf = Li.shadowFar;
    const float dza = -sn / (sf - sn), dzb = sn * sf / (sf - sn);
    // slope-scaled bias in reversed-Z (bigger value == nearer)
    const float bias = mix(0.0025f, 0.0004f, NoL);
    if (Li.isSpot != 0) {
        // Single perspective depth map, as Shadows.cpp bakes for a spot
        // (cubeFace < 0 => one map, not six faces).
        const float3 rel = pw - Li.pos;
        const float3 vp = rowmul(Li.sRow0.xyz, Li.sRow1.xyz, Li.sRow2.xyz, rel);
        if (vp.z <= sn) return 1.0f;
        const float ref = dza + dzb / vp.z;
        // Same 1/tan(halfFov) the bake's vertex shader applied. NDC -> uv,
        // y flipped for Metal's upper-left texture origin.
        const float2 ndc = vp.xy * Li.sRow0.w / vp.z;
        const float2 uv = float2(0.5f + 0.5f * ndc.x, 0.5f - 0.5f * ndc.y);
        return spotMaps[Li.shadowIndex].sample_compare(shadowSamp, uv, ref + bias);
    }
    // World-space direction from light to the shaded point. The GPU takes this
    // tap unconditionally; the CPU is allowed to skip it on static surfaces via
    // the static-shadow lightmap (plan §5.3 item 12).
    const float3 dirW = pw - Li.pos;
    // Same reversed-Z encoding the bake wrote, evaluated on the MAJOR axis
    // distance (which is what the 90-degree face's w equals).
    const float major = max(max(abs(dirW.x), abs(dirW.y)), abs(dirW.z));
    const float ref = dza + dzb / max(major, 1e-5f);
    return shadowCubes[Li.shadowIndex].sample_compare(shadowSamp,
                                                      normalize(dirW), ref + bias);
}

// Per-light early-out shared by the lighting pass, --viz=direct, --viz=shadow
// and --viz=lights, so all four agree on what "this light reaches this pixel"
// means. Returns false when the light does not light the pixel at all.
// `atten` and `NoL` come back only when it returns true.
static inline bool LightReaches(constant FrameUniforms &u, constant GpuLight &Li,
                                thread const Surface &S, bool needNoL,
                                thread float &NoL, thread float &atten,
                                thread float3 &Ldir)
{
    // light position into view space
    const float3 lRel = Li.pos - u.camSrc;
    const float3 lPos = rowmul(u.camRow0, u.camRow1, u.camRow2, lRel);
    const float3 toL = lPos - S.P;
    const float d2 = dot(toL, toL);
    const float range = Li.range * u.lightRangeScale;
    if (d2 >= range * range) return false;          // HARD cutoff, as the CPU does
    const float d = sqrt(d2);
    Ldir = toL / max(d, 1e-6f);
    NoL = saturate(dot(S.N, Ldir));
    if (needNoL && NoL <= 0.0f) return false;
    // Linear falloff to the hard edge — the engine's attenuation shape.
    atten = saturate(1.0f - d * (Li.invRange / u.lightRangeScale));
    // SPOT cone. Identical shape to the CPU kernel
    // (DeferredSurfaceKernel.cpp:3247-3254): cosTheta measured between the spot
    // axis and the light->surface direction, HARD cutoff at cosOuter, then a
    // smoothstep t*t*(3-2t) from cosOuter up to cosInner.
    if (Li.isSpot != 0) {
        const float cosTheta = dot(Li.dir, normalize(S.pw - Li.pos));
        if (cosTheta <= Li.cosOuter) return false;
        if (cosTheta < Li.cosInner) {
            const float tt = (cosTheta - Li.cosOuter)
                           / max(Li.cosInner - Li.cosOuter, 1e-6f);
            atten *= tt * tt * (3.0f - 2.0f * tt);
        }
    }
    return true;
}

// The direct term. `applyShadow` exists ONLY so the caller can ask for the
// unshadowed term explicitly; it is never defaulted, so a caller cannot forget
// the shadow factor the way mode 10 did. `wantSpec` splits diffuse from
// specular for attribution.
static inline float3 DirectRadiance(constant FrameUniforms &u,
                                    constant GpuLight *L,
                                    thread const Surface &S,
                                    bool applyShadow, bool wantSpec,
                                    int onlyLight,
                                    array<depthcube<float>, 16> shadowCubes,
                                    array<depth2d<float>,   16> spotMaps,
                                    sampler shadowSamp)
{
    float3 radiance = 0.0f;
    for (uint i = 0; i < u.numLights; ++i) {
        // onlyLight >= 0 restricts the sum to ONE light, so --viz=direct can
        // attribute the term per light instead of only in aggregate.
        if (onlyLight >= 0 && int(i) != onlyLight) continue;
        float NoL, atten; float3 Ldir;
        if (!LightReaches(u, L[i], S, /*needNoL=*/true, NoL, atten, Ldir)) continue;

        const float shadow = applyShadow
            ? ShadowFactor(L[i], S.pw, NoL, shadowCubes, spotMaps, shadowSamp)
            : 1.0f;

        const float3 H = normalize(Ldir + S.V);
        const float NoH = saturate(dot(S.N, H));
        const float VoH = saturate(dot(S.V, H));

        // Specular, term for term the CPU's scalar Cook-Torrance
        // (DeferredSurfaceKernel.cpp:2424-2452):
        //   spec = D * Gv * Gl * F / (4*NoV),  F = 0.04 + 0.96*(1-VoH)^5,
        //   accumulated as spec * Material::Specular * falloff * shadow
        // — note the CPU's spec term has NO NoL factor (the NoL is inside Gl's
        // shape) and F0 is the fixed dielectric 0.04, with Material::Specular
        // scaling the WHOLE lobe. D_GGX/V_SmithSchlick here equal the CPU's
        // D*Gv*Gl/(4*NoV) divided by NoL, so spec*NoL below reproduces it.
        const float om  = 1.0f - VoH;
        const float om2 = om * om;
        const float Fd  = 0.04f + 0.96f * (om2 * om2 * om);
        const float spec1 = wantSpec
            ? Fd * S.specK * D_GGX(NoH, S.a)
                * V_SmithSchlick(max(S.NoV, 1e-3f), NoL, S.a)
            : 0.0f;
        // Diffuse: NO Lambert 1/pi and NO (1-F) energy factor — both PARITY,
        // not slips. The CPU's direct diffuse is
        //     intensity = NoL * atten * Material::Diffuse;  lR += intensity*colR
        // (DeferredSurfaceKernel.cpp:2391-2397), its only 1/pi lives inside the
        // GGX D, and the --diffuse_energy (1-F) factor scales only the LDR
        // combine `fdB` (ibid. 2568-2571) — the --hdr_linear HDR write uses the
        // raw accumulator `lB` (ibid. 2618-2631), which (1-F) never touches. An
        // earlier revision applied (1-F) per light here; that made every direct
        // light a few percent dimmer than the reference and, worse, tinted it by
        // the per-light Fresnel the CPU never computes.
        // Metalness (--metal_map, default ON): a conductor has no diffuse, and
        // its highlight is tinted by the albedo instead of staying light-
        // coloured (DeferredSurfaceKernel.cpp:2512-2545). The CPU's remaining
        // metal behaviour — the albedo BECOMING an env reflection — needs the
        // baked equirect pano this arm does not have, so metal here reads
        // darker than the reference. Stated, not hidden.
        // D1 / D2 dials — see FrameUniforms::metalCompat. Default (0,0) is the
        // physically correct form: a conductor has no diffuse lobe, and its F0
        // is the LINEAR albedo because the whole frame is linear.
        const float metalKill = mix(1.0f - S.metal, 1.0f, u.metalCompat.x);
        const float3 tintAlbedo = mix(S.baseColor, sqrt(S.baseColor), u.metalCompat.y);
        const float3 diff = S.baseColor * S.diffuseK * metalKill;
        const float3 specTint = mix(float3(1.0f), tintAlbedo, S.metal);

        radiance += (diff + spec1 * specTint) * L[i].color * (NoL * atten * shadow);
    }
    return radiance;
}

// Ambient: L2 SH irradiance x Material::Diffuse x AO. AO multiplies the
// AMBIENT only — direct light keeps its own shadows. --viz=ambient calls this
// same function, so it cannot drift from the frame's ambient.
//
// NO env-BRDF / multiscatter term here — that is PARITY, and it is measured.
// The CPU's --sh_ambient path is `l = Luminosity*255 + Diffuse*E(n)`
// (DeferredSurfaceKernel.cpp:1741-1760): a pure irradiance skylight. The CPU's
// Karis/Fdez-Aguera machinery lives in EnvSpecComposeScalar, which fires only
// for materials with Reflection > 0 or a metallic map AND --env_refl — an
// equirect pano this arm does not implement. An earlier revision added an
// (FssEss + Fms*Ems)*irr sky term for every pixel; under the old (wrong)
// roughness mapping it happened to evaluate near zero, and the CPU-parity
// roughness change blew it up to +24 mean luma over the whole ablated frame
// (ambient term 34 -> 59 at t=5743). The CPU has no such term, so it is gone.
static inline float3 AmbientRadiance(constant FrameUniforms &u,
                                     constant float4 *sh,
                                     thread const Surface &S)
{
    // The SH coefficients are projected from the FLD's authored WORLD-space
    // zenith/nadir backdrop gradient, so the normal has to go back to world
    // before evaluation. Same view->world inverse as DecodeSurface: a
    // combination of the view matrix's ROWS. This carried the same wrong
    // column gather, so the ambient was being looked up along a rotated normal —
    // it survived unnoticed because the gradient is smooth and the magnitude
    // came out about right.
    const float3 Nw = normalize(u.camRow0 * S.N.x + u.camRow1 * S.N.y + u.camRow2 * S.N.z);
    // FLAT vs SH is not a quality dial — it is WHICH BRANCH OF THE CPU KERNEL
    // this scene runs. See FrameUniforms::flatAmbient. The flat branch carries
    // no ambientFactor because the CPU's `Diffuse * Sc->Ambient` has none.
    const float3 irr = (u.flatAmbient.w > 0.5f)
                     ? u.flatAmbient.rgb
                     : SH_Irradiance(sh, Nw) * u.ambientFactor;
    // (1 - metal): a conductor has no diffuse ambient either. The D1 dial
    // suppresses the kill here too, because the CPU's `lB` — which the HDR
    // frame is built from — carries the AMBIENT as well as the direct term
    // (DeferredSurfaceKernel.cpp:1741-1760 fills lB before the light loop).
    const float metalKill = mix(1.0f - S.metal, 1.0f, u.metalCompat.x);
    return S.baseColor * S.diffuseK * metalKill * irr * S.ao;
}


// ---------------------------------------------------------------------------
// ENVIRONMENT SPECULAR — the port of EnvSpecComposeScalar
// (FDS/RENDER/DeferredSurfaceKernel.cpp:847). This is what gives greets'
// polished metal something to reflect; without it the conductors render matte
// and uniformly dark with the hue merely correct, which is the defect the
// t=2000 pair was showing.
//
// Term for term, in the CPU's own order:
//   1. reflection vector, VIEW space, off the viewer-side normal;
//   2. PARALLAX correction against the scene AABB (the "local cubemap" slab
//      exit-t), so a reflection is looked up toward where the ray actually
//      leaves the room rather than as if the probe were at infinity;
//   3. roughness -> mip, rough = sqrt(2/(gloss+2)) unless a roughness map
//      overrides, TRILINEAR between levels;
//   4. Karis Mobile-SIGGRAPH-2014 split-sum env BRDF (greets runs
//      --env_brdf_analytic) and Fdez-Aguera 2019 multiscatter energy
//      compensation (greets runs --pbr_multiscatter). Both were previously
//      DELETED from this shader with the note that they only run inside
//      EnvSpecComposeScalar "a pano path this arm does not implement" — that
//      is now false, and this is the path.
//   5. metal tint: t = 1 - metal + metal*albedo, so a conductor's reflection
//      takes its colour from the albedo and a dielectric's does not.
//
// The result is ADDED INTO THE SPECULAR ACCUMULATOR, exactly as the CPU adds
// it into sB/sG/sR — it is not a replacement for the direct highlight.
static inline float3 EnvSpecular(constant FrameUniforms &u,
                                 thread const Surface &S,
                                 float f0, uint probe,
                                 float3 aabbMin, float3 aabbMax,
                                 array<texturecube<float>, 8> envCubes,
                                 sampler envSamp,
                                 thread float &fresOut)
{
    fresOut = 0.0f;
    if (probe == 0u || probe > 8u) return 0.0f;

    // Viewer-side normal, then reflect in VIEW space and rotate to world. Same
    // shape as the CPU: d is the incident direction (eye -> surface), so
    // rv = d - 2(d.N)N. S.V is surface -> eye, hence the negation.
    const float3 d = -S.V;
    float3 N = S.N;
    if (dot(d, N) > 0.0f) N = -N;
    const float3 rvV = d - 2.0f * dot(d, N) * N;
    float3 R = normalize(u.camRow0 * rvV.x + u.camRow1 * rvV.y + u.camRow2 * rvV.z);

    // Parallax: slab exit-t of S.pw + t*R against the scene AABB, per-axis far
    // plane, t = min over axes. Then look up toward the hit point from the
    // probe's own position rather than from the shaded point.
    const float3 bake = float3(u.envProbePos[probe - 1u].xyz);
    {
        const float3 inv = 1.0f / float3(abs(R.x) < 1e-6f ? 1e-6f : R.x,
                                         abs(R.y) < 1e-6f ? 1e-6f : R.y,
                                         abs(R.z) < 1e-6f ? 1e-6f : R.z);
        const float3 tA = (aabbMax - S.pw) * inv;
        const float3 tB = (aabbMin - S.pw) * inv;
        const float3 tFar = max(tA, tB);
        const float t = min(min(tFar.x, tFar.y), tFar.z);
        if (t > 0.0f && t < 1e30f) R = normalize(S.pw + t * R - bake);
    }

    // Roughness -> mip, trilinear. par.z is already the CPU's
    // rough = sqrt(2/(gloss+2)) mapping (see DecodeSurface).
    const float rough = clamp(S.rough, 0.0f, 1.0f);
    const float nMips = float(envCubes[probe - 1u].get_num_mip_levels());
    const float lvl = clamp(rough * max(nMips - 1.0f, 0.0f), 0.0f, max(nMips - 1.0f, 0.0f));
    const float3 ec = envCubes[probe - 1u].sample(envSamp, R, level(lvl)).rgb;

    // Karis split-sum analytic env BRDF.
    const float ndv = saturate(dot(S.N, S.V));
    const float rx = -1.0f * rough + 1.0f;
    const float ry = -0.0275f * rough + 0.0425f;
    const float rz = -0.572f * rough + 1.04f;
    const float rw =  0.022f * rough + -0.04f;
    const float a004 = min(rx * rx, exp2(-9.28f * ndv)) * rx + ry;
    const float A = -1.04f * a004 + rz;
    const float B =  1.04f * a004 + rw;
    float envBrdf = f0 * A + B;
    float ek = envBrdf;
    // Fdez-Aguera multiscatter energy compensation.
    const float Ess = A + B;
    const float Favg = f0 + (1.0f - f0) / 21.0f;
    const float Fms = Favg * Ess / max(1.0f - Favg * (1.0f - Ess), 1e-4f);
    ek *= 1.0f + Fms * (1.0f - Ess) / max(Ess, 1e-4f);
    ek *= u.envReflGain;
    fresOut = envBrdf;

    // Metal tint.
    const float3 tint = mix(float3(1.0f), S.baseColor, S.metal);
    return ec * ek * tint;
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
                            texture2d<uint>   gMirror   [[texture(36)]],
                            array<texture2d<float>, 4> reflTex [[texture(37)]],
                            array<texturecube<float>, 8> envCubes [[texture(41)]],
                            sampler           shadowSamp [[sampler(1)]],
                            sampler           envSamp    [[sampler(3)]])
{
    const uint2 px = uint2(in.position.xy);
    const float zEnc = gDepth.read(px);
    if (zEnc <= 0.0f) return float4(0.0f);        // reversed-Z: 0 == untouched (sky)

    // Reconstruct view position. Inverse of the vs projection above.
    const float Z = (u.dzb) / max(zEnc - u.dza, 1e-9f);
    const float X = (in.ndc.x - u.ox * 1.0f) * Z * u.invSx;
    const float Y = (in.ndc.y - u.oy * 1.0f) * Z * u.invSy;

    const uint4 mirEnc = gMirror.read(px);
    const Surface S = DecodeSurface(u, gAlbedo.read(px), gNormal.read(px).xy,
                                    gParams.read(px), float3(X, Y, Z),
                                    float(mirEnc.y) * (1.0f / 255.0f));

    float3 radiance = DirectRadiance(u, L, S, /*applyShadow=*/u.shadowsOn != 0,
                                     /*wantSpec=*/true, /*onlyLight=*/-1,
                                     shadowCubes, spotMaps, shadowSamp);
    // Environment specular, into the SAME accumulator the direct highlight
    // uses. Material::SpecMul scales the whole specular on the CPU after this
    // add; this arm folds SpecMul into par.y at the G-buffer, so the specK
    // factor is already carried by the direct term and the env term takes the
    // material's own reflectance through F0 instead.
    {
        float fres = 0.0f;
        radiance += EnvSpecular(u, S, float(mirEnc.w) * (1.0f / 255.0f), mirEnc.z,
                                u.aabbMin.xyz, u.aabbMax.xyz, envCubes, envSamp, fres);
    }
    radiance += AmbientRadiance(u, sh, S);
    radiance += S.baseColor * S.lum;   // emissive

    // Mirror panel composite: panel radiance = emissive text + reflection/2 —
    // the CPU's half-silvered glass formula (the wallMatClone is transparent
    // with Diffuse 0, and the deferred xpar blend is lit_texel + behind/2;
    // for the teleporter Luminosity is 0 so the panel is the pure half
    // reflection). The panel's OWN batch already carries Diffuse=Specular=0,
    // so the terms above contribute only the emissive. A reflected world
    // point on the plane projects to the SAME pixel in the reflection render
    // (rows reflected, position mirrored, same projection), so the composite
    // is a same-pixel read, no reprojection.
    //
    // SECOND ORDER. Inside a first-order reflection this same branch is what
    // makes a mirror-in-a-mirror show content instead of bare emissive: the
    // host binds, at 37.., the view from the DOUBLY reflected camera
    // reflect_B(reflect_A(eye)) and sets mirrorCount, and `mirEnc.x` is the
    // panel id the reflection view's own G-buffer already wrote. The
    // same-pixel invariant composes: each planar reflection keeps the
    // projection and only moves the basis, so B's panel lands on the same
    // pixel in A's view as its doubly-reflected content does. When those
    // targets are rendered smaller than this pass (--mirror2_scale), reflUv
    // carries their texel size and the fetch becomes a normalised bilinear
    // sample; at reflUv == 0 it stays the exact integer read.
    if (u.mirrorCount > 0u) {
        const uint mid = mirEnc.x;
        if (mid > 0u && mid <= u.mirrorCount) {
            const float3 r = (u.reflUv.x > 0.0f)
                ? reflTex[mid - 1u].sample(envSamp, in.position.xy * u.reflUv).rgb
                : reflTex[mid - 1u].read(px).rgb;
            radiance += r * 0.5f;
        }
    }

    return float4(radiance, 1.0f);
}

// ---------------------------------------------------------------------------
// VOLUMETRIC SPOT CONES — the disco beams in the air.
//
// Port of FDS/RENDER/DeferredVolumetric.cpp's Render_VolumetricCones_Tile, the
// `segPath` branch (narrowCone: cosOuter > 0.985, which the disco's 7-degree
// beams satisfy at cos 7 = 0.9925). It is a SCREEN-SPACE analytic integral
// along the view ray, NOT cone geometry — the CPU draws no cone mesh either.
// Additive into the HDR buffer BEFORE bloom and tonemap, which is where
// RENDER.CPP:1231-1240 puts it (after the opaque/TBR draws, before
// DoF/bright/bloom/tonemap), so the shafts feed the bloom as they do on the CPU.
//
// The distance attenuation is integrated EXACTLY, in closed form: the CPU's
// integrand is the inverse-square kernel 1/(rr^2 d(z)^2 + 0.05) written as
// 1/(alpha z^2 + beta z + gamma), whose antiderivative is
// (2/sqrt(disc)) * atan((2 alpha z + beta)/sqrt(disc)). Each of 8 segments gets
// its own exact sub-integral, weighted by the cone smoothstep, the surface fade
// and ONE SHADOW TAP at the segment midpoint. The per-SEGMENT (not per-chord)
// tap is the CPU's own fix for beams shining through walls, and it is why the
// disco's 256^2 per-spot maps have to exist for this pass and not only for
// surface lighting.
//
// Deliberate deviations from the CPU, all stated:
//  - atan difference taken directly instead of through the
//    atan(u)-atan(v) = atan((u-v)/(1+uv)) identity. That identity exists on the
//    CPU to condition an _mm256_rsqrt_ps + one Newton step; Metal's atan needs
//    no such help.
//  - no tile binning / spot_cone_cull. The GPU evaluates every spot for every
//    pixel and lets the quadratic reject — MORE work than the CPU does, in
//    keeping with the rest of this arm.
//  - turbulence (cone_turbulence) and the stochastic ray-march fallback
//    (--no-vol_cone_analytic) are not ported; both default OFF.
//  - the surface-fade window's "12 z-buffer quanta" floor is a z16 staircase
//    remedy. This arm has a float depth buffer, so the floor is carried as a
//    small absolute constant rather than derived from a quantum that does not
//    exist here.
// ---------------------------------------------------------------------------

struct ConeUniforms {
    // cone_strength * 1e-3, already scaled by hdr_glow_scale (HDR path).
    float  density;
    float  nSamples;      // vol_n_samples — the brightness calibration, N x mean
    float  fadeFloor;     // absolute floor on the surface-fade window
    float  pad;
};

// One spot-map tap at a world point. Same map, same projection and the same
// reversed-Z encoding as ShadowFactor's spot branch; the bias is the CPU's
// constant-ish in-air bias rather than the surface path's slope-scaled one,
// because an in-air sample has no surface normal to scale by.
static inline float ConeShadowAt(constant GpuLight &Li, float3 pw,
                                 array<depth2d<float>, 16> spotMaps,
                                 sampler shadowSamp)
{
    if (Li.shadowIndex < 0) return 1.0f;
    const float sn = Li.shadowNear, sf = Li.shadowFar;
    const float dza = -sn / (sf - sn), dzb = sn * sf / (sf - sn);
    const float3 rel = pw - Li.pos;
    const float3 vp = rowmul(Li.sRow0.xyz, Li.sRow1.xyz, Li.sRow2.xyz, rel);
    if (vp.z <= sn) return 1.0f;
    const float ref = dza + dzb / vp.z;
    const float2 ndc = vp.xy * Li.sRow0.w / vp.z;
    if (any(abs(ndc) > 1.0f)) return 1.0f;   // outside the map = unoccluded
    const float2 uv = float2(0.5f + 0.5f * ndc.x, 0.5f - 0.5f * ndc.y);
    return spotMaps[Li.shadowIndex].sample_compare(shadowSamp, uv, ref + 0.0015f);
}

fragment float4 fs_cones(FsQuadOut in [[stage_in]],
                         constant FrameUniforms &u   [[buffer(1)]],
                         constant GpuLight      *L   [[buffer(2)]],
                         constant ConeUniforms  &cu  [[buffer(4)]],
                         depth2d<float>    gDepth    [[texture(3)]],
                         array<depth2d<float>,   16> spotMaps [[texture(20)]],
                         sampler           shadowSamp [[sampler(1)]])
{
    const uint2 px = uint2(in.position.xy);
    // The view ray at this pixel, unnormalised, with z = 1 — exactly the CPU's
    // V = (X, Y, 1) with X = (px - CntrEX) * invFOVX.
    const float3 V = float3((in.ndc.x - u.ox) * u.invSx,
                            (in.ndc.y - u.oy) * u.invSy, 1.0f);

    // Where the ray stops: the nearest surface, or the far plane for sky
    // pixels (the CPU uses its fog cutoff FZP there — greets' FZP is the far
    // plane, so this is the same number).
    const float zEnc = gDepth.read(px);
    const float zMax = (zEnc > 0.0f) ? (u.dzb / max(zEnc - u.dza, 1e-9f)) : u.farZ;
    const float zMin = 0.05f;
    if (zMax <= zMin) return float4(0.0f);

    const float uV = dot(V, V);
    float3 acc = 0.0f;

    for (uint i = 0; i < u.numLights; ++i) {
        constant GpuLight &Li = L[i];
        if (Li.isSpot == 0) continue;

        // Apex and axis into view space (the axis is a direction: rotate only).
        const float3 P = rowmul(u.camRow0, u.camRow1, u.camRow2, Li.pos - u.camSrc);
        const float3 D = rowmul(u.camRow0, u.camRow1, u.camRow2, Li.dir);

        const float cosO = Li.cosOuter, cosI = Li.cosInner;
        const float c2 = cosO * cosO;
        const float range = Li.range * u.lightRangeScale;
        const float r2 = range * range;

        const float DV = dot(D, V), VP = dot(V, P), PP = dot(P, P), DP = dot(D, P);

        // Range sphere first — it bounds every other interval.
        const float sphereDisc = VP * VP - uV * (PP - r2);
        if (sphereDisc < 0.0f) continue;
        const float sphereSq = sqrt(sphereDisc);
        const float invUV = 1.0f / uV;
        const float zSphLo = (VP - sphereSq) * invUV;
        const float zSphHi = (VP + sphereSq) * invUV;

        // Ray vs infinite cone: (D.(Q-P))^2 >= c^2 |Q-P|^2 with Q = z V gives a
        // quadratic in z. a < 0 => the roots bracket the inside; a > 0 => a
        // half-space selection by sign(D.V); a ~ 0 => degenerate, reject.
        const float a  = DV * DV - c2 * uV;
        const float b  = 2.0f * (c2 * VP - DV * DP);
        const float cq = DP * DP - c2 * PP;
        float zLo, zHi;
        if (a < -1e-8f) {
            const float disc = b * b - 4.0f * a * cq;
            if (disc < 0.0f) continue;
            const float sq = sqrt(disc), inv2a = 1.0f / (2.0f * a);
            const float r1 = (-b - sq) * inv2a, r2_ = (-b + sq) * inv2a;
            zLo = min(r1, r2_); zHi = max(r1, r2_);
        } else if (a > 1e-8f) {
            const float disc = b * b - 4.0f * a * cq;
            if (disc < 0.0f) { zLo = zMin; zHi = zMax; }
            else {
                const float sq = sqrt(disc), inv2a = 1.0f / (2.0f * a);
                const float ra = (-b - sq) * inv2a, rb = (-b + sq) * inv2a;
                const float r1Q = min(ra, rb), r2Q = max(ra, rb);
                if      (DV >  1e-6f) { zLo = max(r2Q, zMin); zHi = zMax; }
                else if (DV < -1e-6f) { zLo = zMin; zHi = min(r1Q, zMax); }
                else continue;
                if (zHi <= zLo) continue;
            }
        } else continue;

        zLo = max(zLo, zSphLo);
        zHi = min(zHi, zSphHi);
        zLo = max(zLo, zMin);
        if (zHi <= zLo) continue;
        if (zLo >= zMax) continue;
        // Clamp AT the surface. Without this the analytic path integrates the
        // chord behind the floor and leans on the midpoint fade to approximate
        // the cut, which is the CPU's 'grazing-angle fur on beams' bug.
        zHi = min(zHi, zMax);
        // Forward half of the cone only (the mirror nappe is not a beam).
        if (abs(DV) > 1e-6f) {
            const float zPlane = DP / DV;
            if (DV > 0.0f) zLo = max(zLo, zPlane); else zHi = min(zHi, zPlane);
            if (zHi <= zLo) continue;
        }

        // Exact distance-attenuation integral: 1/(alpha z^2 + beta z + gamma).
        const float rr = 1.0f / max(range, 1e-6f);
        const float rr2 = rr * rr;
        const float alpha = rr2 * uV;
        const float beta  = -2.0f * rr2 * VP;
        const float gamma = rr2 * PP + 0.05f;
        const float disc  = 4.0f * alpha * gamma - beta * beta;
        if (disc <= 1e-20f) continue;         // the CPU's degenerate reject
        const float invD = rsqrt(disc);

        // 8 segments, each an exact sub-integral weighted by cone x fade x shadow.
        constexpr int SEG = 8;
        const float segDz = (zHi - zLo) / float(SEG);
        // nSamp = 16 for a narrow cone, which is what the CPU's fade window
        // derives its step from.
        const float fadeW = max((zHi - zLo) * (1.0f / 16.0f), cu.fadeFloor);
        const float invDz = 1.0f / fadeW;
        const float invCIO = 1.0f / max(cosI - cosO, 1e-6f);

        float sum = 0.0f;
        float uPrev = (2.0f * alpha * zLo + beta) * invD;
        float aPrev = atan(uPrev);
        for (int seg = 1; seg <= SEG; ++seg) {
            const float zk = zLo + float(seg) * segDz;
            const float ak = atan((2.0f * alpha * zk + beta) * invD);
            const float zm = zLo + (float(seg) - 0.5f) * segDz;

            // cone smoothstep at the segment midpoint
            const float3 W = zm * V - P;
            const float cosT = dot(D, W) * rsqrt(max(dot(W, W), 1e-12f));
            float cone;
            if (cosT >= cosI) cone = 1.0f;
            else {
                const float t = saturate((cosT - cosO) * invCIO);
                cone = t * t * (3.0f - 2.0f * t);
            }
            if (cone > 0.0f) {
                const float sfade = saturate((zMax - zm) * invDz);
                // ONE shadow tap per segment. This is the whole reason the disco
                // beams are occluded by the columns instead of shining through.
                const float3 Q = zm * V;
                const float3 pw = u.camSrc + u.camRow0 * Q.x + u.camRow1 * Q.y
                                           + u.camRow2 * Q.z;
                const float sh = (u.shadowsOn != 0)
                    ? ConeShadowAt(Li, pw, spotMaps, shadowSamp) : 1.0f;
                sum += (ak - aPrev) * cone * sfade * sh;
            }
            aPrev = ak;
        }
        const float integral = 2.0f * invD * sum;

        // Midpoint near-edge softness: the ray-march multiplies the integrand by
        // (1 - rr d)^2, so the analytic path reintroduces it once at the chord
        // midpoint or the halo would end at a hard sphere boundary.
        const float zMid = 0.5f * (zLo + zHi);
        const float3 Wm = zMid * V - P;
        const float distM = length(Wm);
        float soft = max(0.0f, 1.0f - rr * distM);
        soft *= soft;

        // "N x mean" — the same brightness calibration cone_strength is tuned
        // against on the ray-march path. coneAtten/surfaceFade at the midpoint
        // are 1 here: the segmented path already folded them in per segment.
        const float vAcc = (integral / max(zHi - zLo, 1e-6f)) * cu.nSamples * soft;
        acc += (vAcc * cu.density) * Li.color;
    }
    return float4(max(acc, 0.0f), 0.0f);
}

// ---------------------------------------------------------------------------
// TRANSPARENT SURFACES — the FORWARD kernel plus a depth peel.
//
// This is NOT the deferred kernel with a blend bolted on, and that matters:
// the CPU shades transparents in `Render_DeferredTransparentLighting_Tile`
// (DeferredSurfaceKernel.cpp:2751), a DIFFERENT shading model from the opaque
// kernel. Read out of that function, and reproduced here term for term:
//
//   l   = Luminosity*255 + Diffuse * Scene::Ambient            (:2996-2998)
//         — a FLAT scene ambient, NOT the L2 SH irradiance the opaque kernel
//           uses, and NOT scaled by Ambient_Factor.
//   per light in range (:3234-3262):
//         k = NoL * (1 - dist*rRange);  spot cone smoothstep multiplies k
//         l += k * Diffuse * L.colour
//   spec, only if Material::Specular > 0 (:3262-3277):
//         BLINN-PHONG pow(NoH, Glossiness) * Specular * falloff * L.colour
//         — not Cook-Torrance, no Fresnel, no GGX. The cone does NOT reach it
//           (the same penumbra gap as the opaque scalar path, contract D4).
//   NO SHADOW TAP AT ALL — the loop has no shadowAtten term.
//   NO normal / roughness / metal / AO maps: --xpar_pbr defaults OFF
//         (FeatureFlags.def:103), so the shipped transparents are flat-textured.
//   lit = (tex/255)^2 * l   under --hdr_linear                  (:3389-3391)
//   out = lit + spec                                            (:3456-3465)
//
// The composite is the BLEND STATE, not shader arithmetic (:3506-3532):
//   XparBlendAlpha > 0 : out = lit*a + dst*(1-a)
//   else               : out = lit + dst*dw,  dw = Transparency*0.01 else 0.5
//                        — UNCLAMPED in HDR, which is why transparents bloom.
//   Mat_Additive       : out = lit + dst  (TheOtherBarry<ADDITIVE>, :600-604)
//
// Everything is in the GPU's 0..1 scale; the CPU's 0..255 radiance divides out
// because L.colour and Scene::Ambient are both already /255 on this side, and
// (tex01)^2 * l == (texB/255)^2 * lB / 255. Verified algebraically, not assumed.
//
// DELIBERATE OMISSIONS, stated rather than hidden: no screen-space refraction
// (--glass_refract / Mat_Refractive), no froxel fog on the layer
// (--fast_fog_xpar), no water path, no Mat_HdrReflection panel sampling (the
// mirror panels keep this arm's own reflection composite in fs_lighting).
// ---------------------------------------------------------------------------

struct XparUniforms {
    float3 sceneAmbient;   // Scene::Ambient / 255
    // 0 = single-layer peel, keep NEAREST (the CPU's legacy xpar_peel_passes==1
    // path: sideZ cleared to 0 = farthest, rasterizer keeps the greater encoded
    // z = the nearer fragment — Mekalele.h:1392-1398).
    // 1 = reverse peel, keep FARTHEST above the peel floor (Mekalele.h:1399-1420
    // under g_xparPeelReverse).
    float peelReverse;
    float usePeelFloor;    // 0 on pass 0 (nothing peeled yet)
    float pad;
};

struct XparVertexOut {
    float4 position [[position]];
    float2 uv;
    float3 viewNormal;
    float3 viewPos;
    float3 worldPos;
};

vertex XparVertexOut vs_xpar(VertexIn in [[stage_in]],
                             constant FrameUniforms &u [[buffer(1)]],
                             constant BatchUniforms &b [[buffer(2)]])
{
    const float3 wp  = rowmul(b.rotRow0, b.rotRow1, b.rotRow2, in.pos) + b.objPos;
    const float3 vp  = rowmul(u.camRow0, u.camRow1, u.camRow2, wp - u.camSrc);
    XparVertexOut o;
    o.position = float4(u.sx * vp.x + u.ox * vp.z,
                        u.sy * vp.y + u.oy * vp.z,
                        u.dza * vp.z + u.dzb,
                        vp.z);
    o.uv = in.uv;
    const float3 wn = rowmul(b.rotRow0, b.rotRow1, b.rotRow2, in.normal);
    o.viewNormal = rowmul(u.camRow0, u.camRow1, u.camRow2, wn);
    o.viewPos = vp;
    o.worldPos = wp;
    return o;
}

// The gates shared by the depth-resolve pass and the colour pass, so a fragment
// can never be resolved by one and rejected by the other.
//   1. the mirror-reflection oblique clip (main pass: inert);
//   2. OPAQUE-Z OCCLUSION. The CPU applies this in the composite, not the
//      raster, because MekaleleTransparent's z-buffer is the xpar Z and never
//      sees the opaque one: `if (ZPage16[i] > zEnc) continue;`
//      (DeferredSurfaceKernel.cpp:2946-2954). Reversed-Z here, so the test is
//      "the opaque fragment is nearer" == opaqueZ > myZ.
//   3. the PEEL FLOOR: only fragments strictly nearer than the previous pass's
//      peeled depth survive (Mekalele.h:1409-1419's `cand > peelFloor`).
static inline bool XparFragmentPasses(constant FrameUniforms &u,
                                      constant XparUniforms &xu,
                                      float3 worldPos, float myZ,
                                      depth2d<float> opaqueDepth,
                                      depth2d<float> peelFloor,
                                      uint2 px)
{
    if (dot(u.clipPlane.xyz, u.clipPlane.xyz) > 0.0f &&
        dot(worldPos, u.clipPlane.xyz) + u.clipPlane.w < 0.05f)
        return false;
    const float zOpaque = opaqueDepth.read(px);
    if (zOpaque > myZ) return false;
    if (xu.usePeelFloor > 0.5f) {
        const float zFloor = peelFloor.read(px);
        if (myZ <= zFloor) return false;
    }
    return true;
}

// Depth-resolve pass: no colour attachment, depth only. It exists because the
// CPU keeps exactly ONE fragment per pixel per (mesh, side) clump — the xpar
// G-buffer has one slot per pixel per side — while a plain blended draw would
// composite every overlapping triangle of the clump. Reproducing "one fragment,
// depth-resolved" is what makes the layer count match.
fragment void fs_xpar_depth(XparVertexOut in [[stage_in]],
                            constant FrameUniforms &u  [[buffer(1)]],
                            constant XparUniforms  &xu [[buffer(5)]],
                            depth2d<float> opaqueDepth [[texture(5)]],
                            depth2d<float> peelFloor   [[texture(6)]])
{
    if (!XparFragmentPasses(u, xu, in.worldPos, in.position.z,
                            opaqueDepth, peelFloor, uint2(in.position.xy)))
        discard_fragment();
}

fragment float4 fs_xpar(XparVertexOut in [[stage_in]],
                        constant FrameUniforms &u   [[buffer(1)]],
                        constant BatchUniforms &b   [[buffer(2)]],
                        constant GpuLight      *L   [[buffer(3)]],
                        constant XparUniforms  &xu  [[buffer(5)]],
                        texture2d<float> albedoTex  [[texture(0)]],
                        depth2d<float>   opaqueDepth[[texture(5)]],
                        depth2d<float>   peelFloor  [[texture(6)]],
                        sampler samp [[sampler(0)]])
{
    if (!XparFragmentPasses(u, xu, in.worldPos, in.position.z,
                            opaqueDepth, peelFloor, uint2(in.position.xy)))
        discard_fragment();

    float4 tex = float4(b.baseColor.rgb, 1.0f);
    if (b.baseColor.w > 0.5f) tex = albedoTex.sample(samp, in.uv);

    const float3 N = normalize(in.viewNormal);
    const float3 P = in.viewPos;
    const float  lum      = b.xpar.x;      // raw Luminosity, no /4 clamp
    const float  diffuseK = b.matParams.x;
    const float  specK    = b.xpar.y;      // Material::Specular
    const float  gloss    = max(b.xpar.z, 1.0f);

    // l = Luminosity*255 + Diffuse*Scene::Ambient, in 0..1 units.
    float3 l = float3(lum) + diffuseK * xu.sceneAmbient;
    float3 s = 0.0f;
    const bool wantSpec = specK > 0.0f;
    const float3 V = normalize(-P);

    for (uint i = 0; i < u.numLights; ++i) {
        constant GpuLight &Li = L[i];
        const float3 lPos = rowmul(u.camRow0, u.camRow1, u.camRow2, Li.pos - u.camSrc);
        const float3 w = lPos - P;
        const float dotN = dot(w, N);
        if (dotN < 0.0f) continue;
        const float len2 = dot(w, w);
        const float range = Li.range * u.lightRangeScale;
        if (len2 > range * range) continue;
        const float dist = sqrt(len2);
        const float lenInv = 1.0f / max(dist, 1e-6f);
        const float falloff = 1.0f - dist * (Li.invRange / u.lightRangeScale);
        float k = dotN * lenInv * falloff;
        if (Li.isSpot != 0) {
            // The CPU measures the cone in VIEW space here off the same w
            // vector, with dir already rotated into view. This arm keeps the
            // light dir in world, so use the world-space equivalent — same
            // angle, and it avoids carrying a second copy of the direction.
            const float cosTheta = dot(Li.dir, normalize(in.worldPos - Li.pos));
            if (cosTheta <= Li.cosOuter) continue;
            if (cosTheta < Li.cosInner) {
                const float t = (cosTheta - Li.cosOuter)
                              / max(Li.cosInner - Li.cosOuter, 1e-6f);
                k *= t * t * (3.0f - 2.0f * t);
            }
        }
        l += (k * diffuseK) * Li.color;
        if (wantSpec) {
            const float3 H = normalize(w * lenInv + V);
            const float NoH = dot(N, H);
            if (NoH <= 0.0f) continue;
            // NOTE the cone is NOT applied here: `falloff`, not `k`. That is
            // the CPU's expression at :3271-3273, gap and all.
            s += (pow(NoH, gloss) * specK * falloff) * Li.color;
        }
    }

    // hdr_linear: the transparent albedo is squared exactly as the opaque
    // composite squares its own (DeferredSurfaceKernel.cpp:3408-3414) — and the
    // gate is the SAME flag, because the `else` at :3416 is
    // `lit = tex*l/256`, the albedo at power ONE. fountain runs that branch.
    const float3 texLin = (u.hdrMode.x > 0.5f) ? (tex.rgb * tex.rgb) : tex.rgb;
    const float3 lit = texLin * l + s;
    // alpha carries XparBlendAlpha for the lerp pipeline's SourceAlpha factor;
    // the legacy `lit + dst*dw` pipeline takes dw from the blend colour and
    // ignores this.
    return float4(max(lit, 0.0f), b.misc2.z);
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
    // hdr_linear: the buffer is linear, so encode on the way out. Hdr.cpp:847-851
    // gates that sqrt on the SAME flag — the gamma path (`--hdr` without
    // `--hdr_linear`, which is what every scene but greets ships) writes the
    // ACES result straight out. This shader used to encode unconditionally.
    if (u.hdrMode.x > 0.5f) c = sqrt(c);
    return float4(c, 1.0f);
}

// ---------------------------------------------------------------------------
// debug visualisations — a ground-truth instrument needs per-stage verification,
// not just a final image. --viz=<mode> selects.
//
// TWO RULES, both learned the hard way in this arm (plan §6.2 / §6.2a):
//
//  R1. NEVER encode "no data" as a legitimate value. Three separate bugs here
//      were prolonged by exactly that: --viz=shadow returning WHITE for both
//      "unshadowed" and "no light in range"; mode 5 returning BLACK for both
//      "fully shadowed" and "no G-buffer"; mode 7 returning pure BLUE for "this
//      light has no cube" while blue is also one of its data channels.
//      Reserved out-of-band colours below: dark RED = no G-buffer,
//      MAGENTA = the mode was asked for something that does not exist.
//
//  R2. A mode's output MUST depend on the thing it claims to visualise. Mode 10
//      claimed "direct, with shadows" and was byte-identical with and without
//      --no-shadows, because it carried a private copy of the light loop that
//      omitted the shadow factor. Every mode below now calls the SAME shared
//      kernels the lit frame calls (DecodeSurface / LightReaches / ShadowFactor
//      / DirectRadiance / AmbientRadiance), so it cannot silently diverge again.
// ---------------------------------------------------------------------------

// Out-of-band markers. Kept distinct from every mode's data range.
constant float4 kVizNoGeometry = float4(0.40f, 0.0f,  0.0f,  1.0f);  // dark red
constant float4 kVizNoSuchThing = float4(1.0f,  0.0f,  1.0f,  1.0f); // magenta

fragment float4 fs_viz(FsQuadOut in [[stage_in]],
                       constant FrameUniforms &u [[buffer(1)]],
                       constant GpuLight      *L [[buffer(2)]],
                       constant float4        *sh [[buffer(3)]],
                       texture2d<float> gAlbedo  [[texture(0)]],
                       texture2d<float> gNormal  [[texture(1)]],
                       texture2d<float> gParams  [[texture(2)]],
                       depth2d<float>   gDepth   [[texture(3)]],
                       array<depthcube<float>, 16> shadowCubes [[texture(4)]],
                       array<depth2d<float>,   16> spotMaps    [[texture(20)]],
                       sampler shadowSamp [[sampler(1)]],
                       sampler rawSamp    [[sampler(2)]],
                       texture2d<uint>  gMirror  [[texture(36)]],
                       array<texture2d<float>, 4> reflTex [[texture(37)]],
                       constant uint &mode [[buffer(4)]])
{
    // R1: a mode selector the shader does not implement must be VISIBLE, not
    // silently aliased onto whatever falls through. Before this, --viz=<any
    // unmapped number> rendered mode 5 and looked like a real answer.
    if (mode > 13) return kVizNoSuchThing;
    // R1: an out-of-range --viz_light must be visible too. It used to read as
    // "no light in range" (mode 5 dark blue / mode 6 black) — a data value.
    if (u.vizLight >= 0 && uint(u.vizLight) >= u.numLights) return kVizNoSuchThing;

    const uint2 px = uint2(in.position.xy);
    const float zEnc = gDepth.read(px);
    if (zEnc <= 0.0f) return kVizNoGeometry;

    // mode 13: MIRROR PANELS — which pixels carry a panel id, and whether the
    // reflection bound for that panel actually has radiance in it. Written to
    // settle "the rtt mirrors are just not there" into one of: no panel pixels
    // on screen (nothing to composite), panel pixels but a black reflection
    // (the pass did not run or produced nothing), or panel pixels with a live
    // reflection (the composite works and the complaint is about content).
    //   BLACK      = a pixel with no panel id
    //   RED   = 60*id  -> panel id 1,2,3 readable as 60,120,180
    //   GREEN        = the bound reflection's luma at this pixel, x255
    //   BLUE  = 255  -> that reflection texture is non-black here
    if (mode == 13u) {
        const uint mid = gMirror.read(px).x;
        if (mid == 0u) return float4(0.0f, 0.0f, 0.0f, 1.0f);
        float3 r = 0.0f;
        if (mid <= 4u) r = reflTex[mid - 1u].read(px).rgb;
        const float ly = dot(r, float3(0.299f, 0.587f, 0.114f));
        return float4(float(mid) * (60.0f / 255.0f), saturate(ly),
                      ly > 0.0f ? 1.0f : 0.0f, 1.0f);
    }

    const float Z = (u.dzb) / max(zEnc - u.dza, 1e-9f);
    const float X = (in.ndc.x - u.ox) * Z * u.invSx;
    const float Y = (in.ndc.y - u.oy) * Z * u.invSy;
    const float4 alb = gAlbedo.read(px);
    const float4 par = gParams.read(px);
    const Surface S = DecodeSurface(u, alb, gNormal.read(px).xy, par,
                                    float3(X, Y, Z));

    switch (mode) {
        case 0: return float4(alb.rgb, 1);   // albedo — stored GAMMA, show as-is
        case 1: return float4(S.N * 0.5f + 0.5f, 1);                // view normal
        case 2: {
            // BAKED AO. R1: alpha is forced to exactly 1.0 by fs_gbuffer for every
            // material WITHOUT Mat_AoInAlpha, so a plain greyscale image showed
            // "no AO map" and "AO map says fully open" as the same white. Graded
            // AO renders grey; the no-data case renders CYAN.
            if (alb.a >= 1.0f) return float4(0.0f, 0.9f, 0.9f, 1);
            return float4(float3(alb.a), 1);
        }
        case 3: return float4(float3((Z - u.nearZ) / (u.farZ - u.nearZ)), 1);
        case 12:
            // RECONSTRUCTED WORLD POSITION — the quantity every shadow question
            // in this arm turns out to be about, and the one there was no way to
            // read directly. Fixed decode so a pixel can be turned back into a
            // world point and fed to --probe:
            //   x = R/255*80 - 20 ,  y = G/255*25 ,  z = B/255*100 - 80
            // (greets spans X[-13.6..49.4] Y[0..18.5] Z[-75.9..4.9], so the
            // ranges bracket the room with headroom and no clipping.)
            return float4(saturate((S.pw.x + 20.0f) / 80.0f),
                          saturate(S.pw.y / 25.0f),
                          saturate((S.pw.z + 80.0f) / 100.0f), 1);
        case 4: return float4(float3(par.z), 1);                    // glossiness
        case 7: {
            // DIAGNOSTIC: is the baked cube depth in the space the tap assumes?
            // Decode the stored depth back to a WORLD DISTANCE and compare it to
            // the actual distance along the major axis. If the face mapping and
            // depth encoding agree, storedDist <= actualDist everywhere, with
            // equality on surfaces directly visible from the light. Encoded as
            // R = actualDist/40, G = storedDist/40, B = storedDist/2.
            //
            // R1/R2: this used to `max(vizLight,0)` — i.e. silently report light 0
            // when no light was selected, under a name that promised "the" light —
            // and returned pure BLUE for "no cube", which collides with its own B
            // channel. Both fixed: an explicit --viz_light is now REQUIRED, and
            // the two no-data cases are magenta.
            if (u.vizLight < 0) return kVizNoSuchThing;   // needs --viz_light=N
            const int li = u.vizLight;
            if (L[li].shadowIndex < 0 || L[li].isSpot != 0) return kVizNoSuchThing;
            const float3 dirW = S.pw - L[li].pos;
            const float sn = L[li].shadowNear, sf = L[li].shadowFar;
            const float dza = -sn / (sf - sn), dzb = sn * sf / (sf - sn);
            const float major = max(max(abs(dirW.x), abs(dirW.y)), abs(dirW.z));
            const float stored = shadowCubes[L[li].shadowIndex].sample(rawSamp, normalize(dirW));
            const float storedDist = (stored > dza + 1e-9f) ? dzb / (stored - dza) : 1e9f;
            return float4(saturate(major / 40.0f), saturate(storedDist / 40.0f),
                          saturate(storedDist * 0.5f), 1);
        }
        case 8: case 9: case 10: case 11: {
            // PER-TERM decomposition of the lighting equation, tonemapped exactly
            // the way the real output is, so a patch can be compared straight
            // against a DEMO reference patch and the offending term NAMED.
            //   8  = ambient only   (SH irradiance x AO, INCLUDING the
            //        multiscatter env term — it used to omit that half and so
            //        reported a smaller ambient than the frame actually applies)
            //   9  = emissive only  (Material::Luminosity)
            //   10 = direct only, WITH shadows and specular — i.e. exactly the
            //        quantity the CPU's --prof_no_lights ablates, so the two-sided
            //        difference measurement compares like with like. Honours
            //        --no-shadows, which is the test it previously could not fail.
            //   11 = direct only, shadow factor FORCED to 1. Mode 10 minus mode 11
            //        is what the cube tap removes, in one pair of runs.
            float3 c8 = 0.0f;
            if      (mode == 8)  c8 = AmbientRadiance(u, sh, S);
            else if (mode == 9)  c8 = S.baseColor * S.lum;
            else                 c8 = DirectRadiance(u, L, S,
                                        /*applyShadow=*/(mode == 10) && (u.shadowsOn != 0),
                                        /*wantSpec=*/true, /*onlyLight=*/u.vizLight,
                                        shadowCubes, spotMaps, shadowSamp);
            c8 = saturate((c8 * (2.51f * c8 + 0.03f)) / (c8 * (2.43f * c8 + 0.59f) + 0.14f));
            return float4(sqrt(c8), 1);
        }
        case 6: {
            // Lights that actually LIGHT this pixel, as a fraction of those
            // considered. R2: this used its own range/cone test; it now calls
            // LightReaches, the same predicate the lighting pass uses, so
            // "in range" here means what it means there — including the NoL>0
            // gate, which a range-only test wrongly counted as lit.
            uint hit = 0, considered = 0;
            for (uint i = 0; i < u.numLights; ++i) {
                if (u.vizLight >= 0 && int(i) != u.vizLight) continue;
                ++considered;
                float NoL, atten; float3 Ldir;
                if (LightReaches(u, L[i], S, /*needNoL=*/true, NoL, atten, Ldir)) ++hit;
            }
            if (considered == 0) return kVizNoSuchThing;   // R1
            const float f = float(hit) / float(considered);
            // green = some light, red scales with the fraction, BLACK = none in range
            return float4(f, f > 0.0f ? 0.35f + 0.65f * f : 0.0f, 0.0f, 1.0f);
        }
        default: break;
    }

    // mode 5: shadow state, THREE distinct colours.
    //   dark blue  = NO shadow-casting light lights this pixel (out of range,
    //                backfacing, or outside a spot cone) — NOT a shadow
    //   white      = lit, fully unshadowed
    //   black      = lit, fully shadowed
    //   grey       = partial (PCF edge / several lights disagreeing)
    //
    // R2: this used a FIXED 0.0015 bias and no NoL gate, while the lighting pass
    // used mix(0.0025,0.0004,NoL) and skipped NoL<=0 lights. So the picture could
    // show occlusion the frame never applied, and vice versa. It now calls
    // LightReaches + ShadowFactor — literally the frame's own tap.
    float acc = 0.0f;
    uint n = 0;
    for (uint i = 0; i < u.numLights; ++i) {
        if (u.vizLight >= 0 && int(i) != u.vizLight) continue;
        if (L[i].shadowIndex < 0) continue;
        float NoL, atten; float3 Ldir;
        if (!LightReaches(u, L[i], S, /*needNoL=*/true, NoL, atten, Ldir)) continue;
        acc += ShadowFactor(L[i], S.pw, NoL, shadowCubes, spotMaps, shadowSamp);
        ++n;
    }
    if (n == 0) return float4(0.05f, 0.10f, 0.35f, 1);   // no light reaches here
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

// ---------------------------------------------------------------------------
// PARTICLE REPLAY — fountain's water spray, from a CPU dump (ParticleReplay.h)
// ---------------------------------------------------------------------------
//
// The CPU blits these through TBR_Sprite -> Spriter's HDRAccum path, which is
// (FILLERS.CPP:2324-2374 and :1395-1409):
//     halfExtent_px = ImageSize * (1/z) * View->PerspX * F->FlareSize * 2
//     colour        = (V->LR, V->LG, V->LB)          -- already lit, 0..255
//     h[c]         += texel[c] * colour[c] / 255     -- ADDITIVE into the float
//                                                       radiance buffer
//     reversed-Z TEST against ZPage16, NO Z-write
// Additive means order-independent: these need no peel and must not go through
// one. One INSTANCED draw for the whole spray, so 8,250 sprites are one encoder
// and not 8,250 of them.
struct PclInstance {
    float4 posSize;      // .xyz world position, .w  ImageSize*perspX*flareSize*2
    float4 color;        // .rgb 0..1 (the dump's 0..255 / 255), .w unused
};

struct PclOut {
    float4 position [[position]];
    float2 uv;
    float  refZ;
    float3 color;
};

// ---------------------------------------------------------------------------
// GPU PARTICLE SIMULATION (--pcl_sim) — the INTERACTIVE half
// ---------------------------------------------------------------------------
//
// READ THIS BEFORE TRUSTING A PARTICLE COMPARISON. The replay above
// (`--pcl=PATH`) is the ORACLE: identical positions on both arms, so any image
// difference is rendering. This kernel is NOT an oracle. It re-derives the
// motion model of DEMO/FOUNTAIN.CPP's `Particle_Kinematics` so that a free-fly
// window has a live, animated spray at an arbitrary pose — which a recording of
// one pinned pose can never provide. It does NOT track the CPU's RNG history,
// so the individual particles differ from frame one. Shape, spread, density,
// lifetime, colour and size are ported term for term; the samples are not.
//
// Model, from DEMO/FOUNTAIN.H (the Fnt* constants) and DEMO/FOUNTAIN.CPP
// (`Initialize_Particles` :426-624, `Particle_Kinematics` :627-900):
//
//   slots 0..2999      OUTER water spray. Spawns at FntSpring-(0,20,0) with a
//                      +-1 unit xz jitter, speed fpow = (30 + sin(T*0.04)*5 +
//                      rand*23.5)*0.6 on a cone (u in [0,2pi), v in
//                      [0.6pi, 1.6pi)). Phase 1 rises by |vel| PER FRAME (not
//                      per dt -- that is the CPU's own expression) until
//                      y > 83.106, then flips to ballistic with g = 20/s^2.
//                      Killed at y < 0. Colour (63,127,255) * 0.3 * 0.8,
//                      truncated to bytes exactly as QColor does.
//   slots 3000..7499   INNER ring, THREE blocks of 1500, one per track. Emitted
//                      from a rotating point (angular velocity 0.008*0.8, radius
//                      55, height 35 + 55*0.3*cos(T*0.008 + I*2pi/3)), speed
//                      4.5 on a full sphere, lifetime `Charge` = 2.5 s, and each
//                      spawn is rotated 0, 120 or 240 degrees by the xForm pair
//                      -- note the CPU's `case 1:` FALLS THROUGH into `case 2:`,
//                      so case 1 rotates TWICE. Nine streams total.
//                      Brightness fades as Charge*0.7/3.5.
//   slots 7500..8249   SPIRAL. The CPU's block is entirely commented out
//                      (FOUNTAIN.CPP:902-960), so these are allocated and never
//                      spawned. Kept so the slot count is the authored 8,250.
//
// SPAWN ALLOCATION is where this deliberately departs from the CPU. The CPU
// re-scans from slot 0 for the first free slot on every single spawn; that is
// inherently serial. This uses a RING: the host advances a cursor by the frame's
// spawn count and each thread spawns iff its index falls in [cursor, cursor+N).
// No atomics, so it is DETERMINISTIC -- the same (seed, frame sequence) gives
// the same spray, which is what makes a --reanimate run reproducible. The ring
// period exceeds the maximum lifetime in both blocks (outer 3000/600 = 5.0 s vs
// ~4 s max flight; inner 1500/360 = 4.2 s vs 2.5 s), so a live particle is
// essentially never overwritten -- and when the ring does lap, the CPU's own
// behaviour (refuse to spawn) and this one (overwrite the oldest) differ only in
// which of two saturated states you get.
struct PclSimU {
    float dt;            // seconds this step
    float timer;         // scene Timer (centiseconds) at this step
    float sizeScale;     // ImageSize * View->PerspX * 2, the constant half of
                         // FILLERS.CPP:2330's edgeLen
    uint  frameIdx;      // RNG salt; monotonic per sim step
    uint  outerCursor;   // ring write cursor, outer block
    uint  outerSpawn;    // spawns this step, outer block
    uint  innerCursor;   // ring write cursor, EVERY inner block (they share a
                         // rate, so one cursor is enough and it keeps the three
                         // tracks phase-locked exactly as the CPU's shared
                         // Spwn2 does)
    uint  innerSpawn;    // spawns this step, PER inner block (= Spwn2*3)
};

// Persistent per-slot state. 48 B x 8,250 = 396 KB, private to the GPU.
struct PclSimState {
    packed_float3 pos;
    float         charge;    // inner: seconds of life left. outer: unused.
    packed_float3 vel;
    uint          flags;     // 0 dead, 1 active, 3 active+ballistic (outer)
    packed_float3 color;     // 0..255, the CPU's post-truncation QColor bytes
    uint          rng;
};

// PCG-XSH-RR. Any decent hash would do; what matters is that it is a pure
// function of (slot, frame), so the sequence is reproducible.
static inline uint pclRandBits(thread uint &s) {
    s = s * 747796405u + 2891336453u;
    const uint w = ((s >> ((s >> 28) + 4u)) ^ s) * 277803737u;
    return (w >> 22) ^ w;
}
// The CPU's RAND_15()/32768.0 is in [0,1). Match the half-open range.
static inline float pclRand01(thread uint &s) {
    return float(pclRandBits(s) & 0x7FFFFFu) * (1.0f / 8388608.0f);
}

kernel void cs_pcl_sim(uint gid [[thread_position_in_grid]],
                       device PclSimState *S      [[buffer(0)]],
                       device PclInstance *inst   [[buffer(1)]],
                       constant PclSimU   &u      [[buffer(2)]])
{
    const uint kOuter = 3000u;             // FntOuterPcls  = int(2000*1.5)
    const uint kInner = 1500u;             // FntInnerPcls  = int(1000*1.5)
    const uint kTotal = 8250u;             // + FntSpiralPcls = int(500*1.5)
    if (gid >= kTotal) return;

    device PclSimState &p = S[gid];
    const bool isOuter = (gid < kOuter);
    const bool isInner = (gid >= kOuter && gid < kOuter + kInner * 3u);
    const uint innerBlk = isInner ? (gid - kOuter) / kInner : 0u;
    const uint innerIdx = isInner ? (gid - kOuter) % kInner : 0u;

    // ---- 1. KILL, exactly where the CPU's kill scan sits: at the TOP of the
    // frame, reading the PREVIOUS step's state (FOUNTAIN.CPP:645-650, :739-745).
    if (p.flags != 0u) {
        if (isOuter) { if (p.pos.y < 0.0f) p.flags = 0u; }
        else if (isInner) {
            p.charge -= u.dt;
            if (p.charge < 0.0f) p.flags = 0u;
        }
    }

    // ---- 2. SPAWN (ring). `ord` is this slot's ordinal within the frame's
    // spawn batch, which stands in for the CPU's loop counter.
    bool spawned = false;
    uint ord = 0u;
    if (isOuter && u.outerSpawn > 0u) {
        const uint ring = (gid + kOuter - (u.outerCursor % kOuter)) % kOuter;
        if (ring < u.outerSpawn) { spawned = true; ord = ring; }
    } else if (isInner && u.innerSpawn > 0u) {
        const uint ring = (innerIdx + kInner - (u.innerCursor % kInner)) % kInner;
        if (ring < u.innerSpawn) { spawned = true; ord = ring; }
    }

    if (spawned) {
        // Seed is a pure function of (slot, frame): reproducible, and
        // decorrelated between neighbouring slots.
        uint s = (gid * 2654435761u) ^ (u.frameIdx * 1013904223u) ^ 0x9E3779B9u;
        pclRandBits(s); pclRandBits(s);         // discard, decorrelate the seed
        if (isOuter) {
            const float powr = 30.0f + sin(u.timer * 0.04f) * 5.0f;   // FntSpawnOutPow/Tilt/Freq
            const float fpow = (powr + pclRand01(s) * 23.5f) * 0.6f;  // FntSpawnOutPowRand
            float3 pos = float3(-0.261f, 85.106f, -0.159f);           // FntSpring
            pos.x += (pclRand01(s) * 2.0f - 1.0f) * 1.0f;             // FntSpawnOutDisp
            pos.z += (pclRand01(s) * 2.0f - 1.0f) * 1.0f;
            pos.y -= 20.0f;
            const float uu = pclRand01(s) * 6.28318530718f;
            const float vv = pclRand01(s) * 3.14159265359f + 3.14159265359f * 0.6f;
            const float cv = cos(vv);
            p.pos   = pos;
            p.vel   = float3(cv * cos(uu) * fpow, sin(vv) * fpow, cv * sin(uu) * fpow);
            // 63/127/255 * (0.3 * 0.8), stored through a byte QColor -> trunc.
            p.color = float3(float(int(63.0f * 0.24f)), float(int(127.0f * 0.24f)),
                             float(int(255.0f * 0.24f)));   // (15, 30, 61)
            p.charge = 0.0f;
            p.flags  = 1u;
        } else {
            // The CPU's `Spwn` counts DOWN from Spwn2*3-1, and `Spwn%3` picks
            // the rotation. Mirror that so all three arms are populated evenly.
            const uint spwn = (u.innerSpawn - 1u) - ord;
            // Temporal antialiasing: the CPU back-dates the emitter by a random
            // fraction of the step so a 60 Hz spawn burst is not a ring of
            // co-located points (FOUNTAIN.CPP:783).
            const float rtime = u.timer - u.dt * 100.0f * pclRand01(s);
            const float ang   = rtime * 0.008f * 0.8f;      // angularVelocity*0.8
            const float phase = float(innerBlk) * (2.0f * 3.14159265359f / 3.0f);
            float3 pos = float3(cos(ang) * 55.0f,                        // magwav
                                35.0f + cos(rtime * 0.01f * 0.8f + phase) * 0.3f * 55.0f,
                                sin(ang) * 55.0f);
            const float uu = pclRand01(s) * 6.28318530718f;
            const float vv = pclRand01(s) * 3.14159265359f - 1.57079632679f;
            const float cv = cos(vv);
            float3 vel = float3(cv * cos(uu) * 4.5f, sin(vv) * 4.5f, cv * sin(uu) * 4.5f);
            // xForm1 == xForm2 == a 120-degree rotation about y. `case 1:` in
            // the CPU has NO break, so it applies the pair; `case 2:` applies
            // one; `case 0:` none.
            const float ax = -0.5f, az = 0.8660254038f;     // -0.5, sqrt(3)/2
            const uint rots = (spwn % 3u == 0u) ? 0u : ((spwn % 3u == 1u) ? 2u : 1u);
            for (uint r = 0; r < rots; ++r) {
                const float px = pos.x * ax + pos.z * az;
                pos.z = pos.z * ax - pos.x * az; pos.x = px;
                const float vx = vel.x * ax + vel.z * az;
                vel.z = vel.z * ax - vel.x * az; vel.x = vx;
            }
            float3 c = (innerBlk == 0u) ? float3(48.0f, 128.0f, 255.0f)
                     : (innerBlk == 1u) ? float3(80.0f, 255.0f,  80.0f)
                                        : float3(255.0f, 128.0f, 48.0f);
            c = float3(float(int(c.x * 0.8f)), float(int(c.y * 0.8f)), float(int(c.z * 0.8f)));
            p.pos = pos; p.vel = vel; p.color = c;
            p.charge = 2.5f;
            p.flags  = 1u;
        }
        p.rng = s;
    }

    // ---- 3. ANIMATE (FOUNTAIN.CPP:706-736 outer, :877-900 inner) -----------
    float3 lit = float3(0.0f);
    float  flareSize = 0.0f;
    if (p.flags != 0u) {
        if (isOuter) {
            if (p.flags & 2u) {
                float3 v = p.vel;
                v.y -= u.dt * 20.0f;                 // FntSpawnOutGrav
                p.vel = v;
                p.pos = float3(p.pos) + v * u.dt;
            } else {
                if (p.pos.y > 83.106f) p.flags |= 2u;
                float3 q = p.pos; q.y += length(float3(p.vel)); p.pos = q;
            }
            lit = float3(p.color);                   // the "dummy illumination model"
            flareSize = 0.06f * 20.0f / 10.0f;       // 0.12
        } else if (isInner) {
            p.pos = float3(p.pos) + float3(p.vel) * u.dt;
            lit = float3(p.color) * (p.charge * 0.7f / 3.5f);
            flareSize = 0.07f * 20.0f / 10.0f;       // 0.14
        }
    }

    // ---- 4. EMIT the instance the existing vs_pcl/fs_pcl pair consumes ------
    // Byte-truncated on the way out, because the dump path carries LR/LG/LB as
    // uint8 and the two paths must be radiometrically identical.
    device PclInstance &d = inst[gid];
    if (p.flags == 0u || flareSize <= 0.0f) {
        // Dead: w <= 0 is the collapse marker vs_pcl tests. The instance count
        // stays at the authored 8,250 so no readback is needed to draw.
        d.posSize = float4(0.0f);
        d.color   = float4(0.0f);
    } else {
        d.posSize = float4(p.pos.x, p.pos.y, p.pos.z, u.sizeScale * flareSize);
        const float3 b = clamp(floor(lit), 0.0f, 255.0f);
        d.color   = float4(b * (1.0f / 255.0f), 1.0f);
    }
}

vertex PclOut vs_pcl(uint vid [[vertex_id]],
                     uint iid [[instance_id]],
                     constant FrameUniforms &u [[buffer(1)]],
                     constant PclInstance   *P [[buffer(2)]],
                     constant float4        &vp [[buffer(3)]])   // .xy = W,H, .zw = cntrE
{
    const float2 c[6] = {float2(-1,-1), float2(1,-1), float2(-1,1),
                         float2(1,-1),  float2(1,1),  float2(-1,1)};
    constant PclInstance &p = P[iid];
    const float3 rel = p.posSize.xyz - u.camSrc;
    const float3 V = rowmul(u.camRow0, u.camRow1, u.camRow2, rel);
    PclOut out;
    // w <= 0 is the DEAD marker cs_pcl_sim writes. The GPU sim keeps the
    // instance count pinned at the authored 8,250 rather than compacting, so
    // that no readback (and no atomic, and no nondeterminism) sits between the
    // sim and the draw; the dead slots cost one collapsed vertex each. The
    // replay path never writes a non-positive w, so this is inert there.
    if (!(p.posSize.w > 0.0f) || !(V.z > u.nearZ && V.z < u.farZ)) {
        // Behind the eye or past the far plane: collapse the quad so it
        // rasterises nothing. Cheaper and simpler than a CPU-side cull, and it
        // keeps the instance count equal to the dump's particle count so the
        // census is honest.
        out.position = float4(0, 0, -1, 1);
        out.uv = 0; out.refZ = 0; out.color = 0;
        return out;
    }
    // Same projection the frame's own vertex stage uses, expressed in pixels
    // because Spriter's extent is in pixels.
    const float2 ctr = float2(vp.z + V.x * (u.sx * vp.x * 0.5f) / V.z,
                              vp.w - V.y * (u.sy * vp.y * 0.5f) / V.z);
    const float  hx = p.posSize.w / V.z;
    const float2 px = ctr + c[vid] * hx;
    out.position = float4(2.0f * px.x / vp.x - 1.0f, 1.0f - 2.0f * px.y / vp.y, 0.0f, 1.0f);
    out.uv = c[vid] * 0.5f + 0.5f;
    out.refZ = u.dza + u.dzb / max(V.z, 1e-4f);
    out.color = p.color.rgb;
    return out;
}

fragment float4 fs_pcl(PclOut in [[stage_in]],
                       texture2d<float> pclTex [[texture(0)]],
                       depth2d<float>   gDepth [[texture(1)]],
                       sampler samp [[sampler(0)]])
{
    const float zEnc = gDepth.read(uint2(in.position.xy));
    // Reversed-Z: bigger == nearer. Spriter's `if (Z <= ZPage16[..]) skip`.
    if (zEnc > 0.0f && in.refZ <= zEnc) discard_fragment();
    const float3 t = pclTex.sample(samp, in.uv).rgb;
    return float4(t * in.color, 1.0f);
}

// ---------------------------------------------------------------------------
// bloom — the SAME construction as FDS/RENDER/Hdr.cpp's Render_BloomPass
// ---------------------------------------------------------------------------
//
// greets turns this on by default with bloom_intensity 2.0 (GREETS.CPP:1168-9),
// so it is parity. Shape, from Hdr.cpp:
//   1. bright pass + DS=4 box downsample. Per source texel lum = max(R,G,B);
//      contribute only if lum > threshold, weighted by (lum-threshold)/lum (a
//      SOFT knee, not a hard cut), then /16 for the 4x4 box.
//   2. separable 5-tap gaussian [1 4 6 4 1]/16, run TWICE (H,V,H,V) at
//      quarter-res, edge-clamped.
//   3. bilinear upsample, x intensity, ADDED into the HDR buffer BEFORE the
//      tonemap so the glow rolls off through ACES with everything else.
//
// The threshold is in the CPU's linear-radiance 0-255 scale (default 200, i.e.
// 255 = reference white); this arm's HDR buffer is 0..1, so the host divides by
// 255 before handing it over. Not reproduced: HdrClamp on the add-back (f16
// carries the range) and the shared bright-pass cache (anamorphic/lens-ghost are
// off here, so there is nothing to share it with).

struct BloomUniforms {
    float2 srcSize;      // full-res dimensions
    float2 dstSize;      // quarter-res dimensions
    float  threshold;    // already scaled into this buffer's units
    float  intensity;
    float2 pad;
};

fragment float4 fs_bloom_bright(FsQuadOut in [[stage_in]],
                                constant BloomUniforms &b [[buffer(1)]],
                                texture2d<float> src [[texture(0)]])
{
    const uint2 d = uint2(in.position.xy);
    float3 acc = 0.0f;
    for (uint dy = 0; dy < 4; ++dy) {
        const uint sy = d.y * 4 + dy;
        if (sy >= uint(b.srcSize.y)) break;
        for (uint dx = 0; dx < 4; ++dx) {
            const uint sx = d.x * 4 + dx;
            if (sx >= uint(b.srcSize.x)) break;
            const float3 c = src.read(uint2(sx, sy)).rgb;
            const float lum = max(c.r, max(c.g, c.b));
            if (lum > b.threshold) acc += c * ((lum - b.threshold) / lum);
        }
    }
    return float4(acc * (1.0f / 16.0f), 1.0f);
}

// One separable gaussian tap set; `dir` picks horizontal (1,0) or vertical (0,1).
fragment float4 fs_bloom_blur(FsQuadOut in [[stage_in]],
                              constant BloomUniforms &b [[buffer(1)]],
                              constant float2 &dir [[buffer(2)]],
                              texture2d<float> src [[texture(0)]])
{
    const int2 p = int2(in.position.xy);
    const int2 hi = int2(b.dstSize) - 1;
    const int2 d = int2(dir);
    const float gwc = 6.0f/16.0f, gw1 = 4.0f/16.0f, gw2 = 1.0f/16.0f;
    const int2 m2 = clamp(p - 2*d, int2(0), hi);
    const int2 m1 = clamp(p - 1*d, int2(0), hi);
    const int2 p1 = clamp(p + 1*d, int2(0), hi);
    const int2 p2 = clamp(p + 2*d, int2(0), hi);
    const float3 o = src.read(uint2(m2)).rgb * gw2
                   + src.read(uint2(m1)).rgb * gw1
                   + src.read(uint2(p )).rgb * gwc
                   + src.read(uint2(p1)).rgb * gw1
                   + src.read(uint2(p2)).rgb * gw2;
    return float4(o, 1.0f);
}

// Bilinear upsample + intensity, ADDITIVE into the HDR target (blend state does
// the add, matching the CPU's `row[c] += bloom*intensity`).
fragment float4 fs_bloom_add(FsQuadOut in [[stage_in]],
                             constant BloomUniforms &b [[buffer(1)]],
                             texture2d<float> src [[texture(0)]],
                             sampler samp [[sampler(0)]])
{
    // Same sample point the CPU derives: fx = (x+0.5)/DS - 0.5 in DST texels,
    // which is (x+0.5)/srcSize in normalised coords.
    const float2 uv = (in.position.xy) / b.srcSize;
    return float4(src.sample(samp, uv).rgb * b.intensity, 1.0f);
}

// ---------------------------------------------------------------------------
// interactive window: blit + HUD overlay
// ---------------------------------------------------------------------------

// Copy the LDR frame to the drawable (which may be a different size than the
// render target, e.g. on a Retina display), then the HUD is composited on top.
fragment float4 fs_blit(FsQuadOut in [[stage_in]],
                        constant float2 &dstSize [[buffer(1)]],
                        texture2d<float> src [[texture(0)]],
                        sampler samp [[sampler(0)]])
{
    return float4(src.sample(samp, in.position.xy / dstSize).rgb, 1.0f);
}

// HUD: an RGBA8 text bitmap rasterised on the CPU each frame, alpha-composited
// over the drawable. Kept as a separate pass so it is trivially excludable from
// any timing (it is drawn AFTER the timed command buffer's work in any case).
fragment float4 fs_hud(FsQuadOut in [[stage_in]],
                       constant float2 &hudSize [[buffer(1)]],
                       texture2d<float> hud [[texture(0)]])
{
    const uint2 p = uint2(in.position.xy);
    if (p.x >= uint(hudSize.x) || p.y >= uint(hudSize.y)) discard_fragment();
    const float4 t = hud.read(p);
    if (t.a <= 0.0f) discard_fragment();
    return float4(t.rgb, t.a);
}

// ===========================================================================
// GPU HARDWARE TESSELLATION — displaced greets stone (`rooms` / `floor`)
// docs/GPU_BENCHMARK_PLAN.md §6.3
//
// THE KNOB is `--tess_px`: the TARGET TRIANGLE EDGE LENGTH IN PIXELS. A patch
// edge whose projected length is L px asks for round(L / target) segments.
// That is a SCREEN-SPACE ADAPTIVE rule, so a patch at the far end of the
// corridor costs a fraction of one at the camera and the count stays sane.
//
// CRACK-FREEDOM of the FACTORS is structural, not tuned. Every edge factor is
// derived from THE TWO ENDPOINTS OF THAT EDGE ALONE — never from the patch's
// centre, its area, or its other two edges — so two patches sharing an edge
// compute the same number without communicating. The endpoints agree BITWISE
// because the de-indexed vertex buffer copies them from the same source vertex,
// and the sub-patch lattice below derives every interior point as an EXACT
// integer ratio a/P (never 1 - b/P, which is a different float).
//
// THAT ARGUMENT WAS TRUE AND STILL SHIPPED CRACKS, because the factors were
// never delivered. The pipeline asked for MTLTessellationFactorStepFunction-
// PerPatch, under which the record address is a function of the patch index
// ALONE and the instanceStride is required to be ZERO — so all P*P sub-patch
// instances read the record written for SUB-PATCH 0. Whole base triangles
// vanished when sub-patch 0 frustum-culled (MEASURED t=5743 px=8 P=8: 290,756
// background pixels where the flat frame has stone), and the survivors carried
// a neighbour-inconsistent factor. The step function is now PerPatchAndPer-
// Instance, which addresses offset + instanceID*stride + patch*8 — the layout
// cs_tess_factors actually writes. See Deferred.mm's makeTessPso.
//
// THE ONLY CRACK LEFT is a MESH property and is measured by --tess_seam_audit:
// the buffer is de-indexed per face with PER-FACE UVs, so 103 of the 155
// positions two stone faces share disagree about UV (115 about the normal),
// and amp*(h(uv)-mean) along the normal then sends that one point to two
// different places — up to 0.112 world units apart at amp 0.3. No factor rule
// can close that; only a welded parameterisation or a position-based height
// lookup can, and both change the displaced LOOK.
//
// PRE-SUBDIVISION: Metal DEFAULTS maxTessellationFactor to 16, and this M2 Max
// accepts and honours 64 when asked (probed in makeTessPso). Either way one
// patch has a ceiling, so reaching one triangle per pixel on a near wall needs
// PRE-SUBDIVISION, and this arm does it by INSTANCING: instance s of the draw
// renders sub-triangle s of a uniform P x P barycentric split of the base
// triangle, with its own tessellation-factor record. Effective per-edge
// subdivision is P x maxFactor, at NO vertex-buffer cost.
// ===========================================================================

struct TessUniforms {
    // .x target edge length in PIXELS, .y world displacement amplitude,
    // .z height-map mean (the CPU bake's mipMean), .w pre-split P
    float4 k;
    // .x patch count in this batch, .y frustum-cull enable, .z max hw factor,
    // .w height mip level
    float4 k2;
    // .x viewport W, .y viewport H, .z backface-cull enable, .w edge-slot map
    float4 k3;
    // .x UNIFORM FACTOR OVERRIDE (0 = off) — the calibration probe that settles
    // what the hardware's real ceiling is and whether the post-tessellation
    // vertex function is invoked once per VERTEX or once per triangle CORNER.
    // Both questions have to be answered before any triangle count is quoted.
    float4 k4;
};

// One sub-triangle of the P x P barycentric split, in LATTICE coordinates.
// bary(a,b) = ((P-a-b)/P, a/P, b/P) — three exact integer ratios, so a lattice
// point shared by two sub-triangles (or by two BASE triangles across their
// common edge) evaluates to the same float on both sides.
struct SubTri { int a, b, down, pad; };

static inline float3 subBaryCorner(int a, int b, float invP) {
    return float3(float(a) * invP, float(b) * invP, 0.0f);   // filled by caller
}

// Global barycentric (w0,w1,w2) of lattice point (a,b) with base P.
static inline float3 latticeBary(int a, int b, int P) {
    const float invP = 1.0f / float(P);
    return float3(float(P - a - b) * invP, float(a) * invP, float(b) * invP);
}

// The three global-barycentric corners of sub-triangle `st`.
static inline void subTriCorners(SubTri st, int P, thread float3 *B) {
    if (st.down == 0) {
        B[0] = latticeBary(st.a,     st.b,     P);
        B[1] = latticeBary(st.a + 1, st.b,     P);
        B[2] = latticeBary(st.a,     st.b + 1, P);
    } else {
        B[0] = latticeBary(st.a + 1, st.b,     P);
        B[1] = latticeBary(st.a + 1, st.b + 1, P);
        B[2] = latticeBary(st.a,     st.b + 1, P);
    }
}

// ---------------------------------------------------------------------------
// Factor kernel. One thread per (base patch, sub-triangle).
// ---------------------------------------------------------------------------

struct TessVert {          // the ingest's Vertex, read raw (48 B, see SceneIngest.h)
    packed_float3 pos;
    packed_float3 nrm;
    float2        uv;
    packed_float4 tan;
};

// Projected pixel position of a world point, and its view z.
static inline float3 projectPx(constant FrameUniforms &u, constant TessUniforms &tu,
                               float3 wp) {
    const float3 rel = wp - u.camSrc;
    const float3 vp  = rowmul(u.camRow0, u.camRow1, u.camRow2, rel);
    const float  z   = max(vp.z, 1e-4f);
    const float  nx  = (u.sx * vp.x + u.ox * vp.z) / z;
    const float  ny  = (u.sy * vp.y + u.oy * vp.z) / z;
    return float3(nx * 0.5f * tu.k3.x, ny * 0.5f * tu.k3.y, vp.z);
}

// THE EDGE RULE — the SPHERE-PROJECTION metric, and the reason it is not the
// naive screen-space one.
//
// The obvious rule, "project both endpoints and measure the pixel distance",
// is WRONG here and was measured wrong: greets' `rooms` is 196 triangles for
// a whole room, so a wall patch runs from 0.1 world units in front of the eye
// to 14 behind it. Projecting an endpoint near the eye plane divides by ~0,
// which pinned those patches to the maximum factor at EVERY target — the
// triangle count came out essentially independent of --tess_px (352k at a
// 32 px target vs 1.16M at 1 px, when a 32x finer target should be ~1000x
// more), and 99 % of the triangles landed off screen.
//
// Instead: take the edge's WORLD length, divide by the distance from the eye
// to its midpoint, and scale by the projection. That is the pixel length the
// edge would have if it were perpendicular to the view — an upper bound on
// its real footprint, well behaved at and behind the eye plane, and (this is
// the part that matters) still a function of THE TWO ENDPOINTS ALONE. Both
// `length(a-b)` and `0.5*(a+b)` are exactly symmetric in float, so two patches
// sharing an edge still derive the same factor with no communication.
static inline float edgeFactor(float3 wA, float3 wB, float3 eye,
                               float perspX, float nearZ,
                               float targetPx, float maxF) {
    const float wl = length(wA - wB);
    const float d  = max(length(0.5f * (wA + wB) - eye), nearZ);
    const float lenPx = wl * perspX / d;
    return clamp(round(lenPx / max(targetPx, 0.05f)), 1.0f, maxF);
}

kernel void cs_tess_factors(device half *factors            [[buffer(0)]],
                            const device TessVert *verts    [[buffer(1)]],
                            constant FrameUniforms &u       [[buffer(2)]],
                            constant BatchUniforms &b       [[buffer(3)]],
                            constant TessUniforms &tu       [[buffer(4)]],
                            const device SubTri *subs       [[buffer(5)]],
                            device atomic_uint *stats       [[buffer(6)]],
                            uint2 gid [[thread_position_in_grid]])
{
    const uint  nPatch = uint(tu.k2.x);
    const int   P      = int(tu.k.w);
    if (gid.x >= nPatch || gid.y >= uint(P * P)) return;

    const uint  patch = gid.x, sub = gid.y;
    const float maxF  = tu.k2.z;

    // Base control points, world space.
    float3 W[3];
    for (int i = 0; i < 3; ++i) {
        const float3 op = float3(verts[patch * 3u + uint(i)].pos);
        W[i] = rowmul(b.rotRow0, b.rotRow1, b.rotRow2, op) + b.objPos;
    }

    float3 B[3];
    subTriCorners(subs[sub], P, B);

    float3 wsub[3], px[3];
    for (int i = 0; i < 3; ++i) {
        wsub[i] = B[i].x * W[0] + B[i].y * W[1] + B[i].z * W[2];
        px[i] = projectPx(u, tu, wsub[i]);
    }

    // Slot 0 of the factor record. `edgeMap` 0 = the OPPOSITE-VERTEX
    // convention (slot i is the edge NOT touching control point i), which is
    // what D3D/GL/Metal all document; 1 = the adjacent convention. Which one
    // this hardware actually implements is settled by LOOKING for cracks, not
    // by trusting the documentation — see --tess_edge_map. NOW SETTLED, and
    // only settleable once the factor records were actually delivered per
    // instance: t=5743 px=8 P=8 --tess_amp=0 gives ZERO crack pixels at
    // edgeMap 0 and 15 at edgeMap 1. The documented convention is the real one.
    const float amp = abs(tu.k.y);
    // Displacement can push a patch back on screen after it left, so the
    // frustum test is widened by the amplitude expressed in pixels at the
    // nearest corner.
    float3 lo = px[0], hi = px[0];
    float  zmin = px[0].z;
    for (int i = 1; i < 3; ++i) { lo = min(lo, px[i]); hi = max(hi, px[i]); zmin = min(zmin, px[i].z); }
    float zmax = max(max(px[0].z, px[1].z), px[2].z);
    bool cull = false;
    if (tu.k2.y > 0.5f) {
        // BEHIND THE EYE. Not an optimisation — without it a patch entirely
        // behind the camera is never frustum-tested (its projection is
        // meaningless) and, under the old edge rule, was tessellated at the
        // ceiling and then thrown away by the clipper. greets' floor is 30
        // room-spanning triangles, so at presplit 32 that was tens of
        // thousands of full-rate patches rendering nothing.
        if (zmax < u.nearZ - amp) {
            cull = true;
        } else if (zmin > u.nearZ) {
            // Fully in front: the screen bbox is meaningful. Widen it by the
            // displacement amplitude expressed in pixels at the nearest corner,
            // so a patch displacement pushes back on screen is not lost.
            const float margin = amp * u.sx * 0.5f * tu.k3.x / max(zmin, 1e-3f) + 2.0f;
            const float halfW = tu.k3.x * 0.5f + margin, halfH = tu.k3.y * 0.5f + margin;
            if (hi.x < -halfW || lo.x > halfW || hi.y < -halfH || lo.y > halfH) cull = true;
        }
        // Straddling the eye plane: never culled. Conservative on purpose.
    }
    if (!cull && tu.k3.z > 0.5f) {
        // Signed screen area of the sub-patch; the main pass keeps FRONT faces
        // (see Deferred.mm), so a patch of the other sign carries no visible
        // geometry once displacement is small against the patch.
        //
        // THE SIGN IS MEASURED, and it used to be the wrong one. With `> 0`
        // this test discarded the VISIBLE half: --tess_back_cull at t=5743
        // px=8 P=8 --tess_amp=0 left 1,162,395 background pixels where the flat
        // frame has stone. With `< 0` the same arm leaves ZERO — identical to
        // not back-culling at all, which is what a correct backface reject must
        // produce when displacement is off. (The wrong sign was invisible until
        // the factor-record bug above was fixed, because most patches were
        // being dropped for the other reason anyway.) Still OFF by default: a
        // backfacing BASE patch can carry visible relief on a displaced
        // silhouette, so this is a priced choice, not a free one.
        const float2 e1 = px[1].xy - px[0].xy, e2 = px[2].xy - px[0].xy;
        if (e1.x * e2.y - e1.y * e2.x < 0.0f) cull = true;
    }

    const uint base = (sub * nPatch + patch) * 4u;
    if (cull) {
        // A zero factor tells the tessellator to discard the patch entirely.
        for (uint i = 0; i < 4u; ++i) factors[base + i] = half(0.0f);
        return;
    }

    const float perspX = u.sx * 0.5f * tu.k3.x;      // == Camera::PerspX
    const float e12 = edgeFactor(wsub[1], wsub[2], u.camSrc, perspX, u.nearZ, tu.k.x, maxF);
    const float e20 = edgeFactor(wsub[2], wsub[0], u.camSrc, perspX, u.nearZ, tu.k.x, maxF);
    const float e01 = edgeFactor(wsub[0], wsub[1], u.camSrc, perspX, u.nearZ, tu.k.x, maxF);

    float o0, o1, o2;
    if (tu.k3.w < 0.5f) { o0 = e12; o1 = e20; o2 = e01; }
    else                { o0 = e01; o1 = e12; o2 = e20; }

    // The inside factor governs the patch interior only; no neighbour sees it,
    // so it is free to be the max of the three edges (never LESS than an edge,
    // or the stitch ring degenerates).
    const float inner = max(max(o0, o1), o2);

    if (tu.k4.x > 0.5f) { o0 = o1 = o2 = tu.k4.x; }
    const float innerW = (tu.k4.x > 0.5f) ? tu.k4.x : inner;

    factors[base + 0] = half(o0);
    factors[base + 1] = half(o1);
    factors[base + 2] = half(o2);
    factors[base + 3] = half(innerW);

    if (stats) {
        // Boundary segment count and live patch count: with the post-tess
        // VERTEX count these give the EXACT triangle count by Euler
        // (T = 2V - B - 2 per patch, a triangulated disk). See Deferred.mm.
        atomic_fetch_add_explicit(&stats[0], uint(o0 + o1 + o2), memory_order_relaxed);
        atomic_fetch_add_explicit(&stats[1], 1u, memory_order_relaxed);
        // Predictions to check the MEASURED vertex count against:
        //   stats[3] = sum of inner factors
        //   stats[4] = sum of (n+1)(n+2)/2  — vertices if deduplicated
        //   stats[5] = sum of 3*n^2         — vertices if per triangle CORNER
        const uint n = uint(innerW);
        atomic_fetch_add_explicit(&stats[3], n, memory_order_relaxed);
        atomic_fetch_add_explicit(&stats[4], (n + 1u) * (n + 2u) / 2u, memory_order_relaxed);
        atomic_fetch_add_explicit(&stats[5], 3u * n * n, memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
// Post-tessellation vertex function.
// ---------------------------------------------------------------------------

struct TessCP {
    float3 pos     [[attribute(0)]];
    float3 normal  [[attribute(1)]];
    float2 uv      [[attribute(2)]];
    float4 tangent [[attribute(3)]];
};

static inline GBufVertexOut tessShade(patch_control_point<TessCP> cp,
                                      float3 pc, uint iid,
                                      constant FrameUniforms &u,
                                      constant BatchUniforms &b,
                                      constant TessUniforms &tu,
                                      const device SubTri *subs,
                                      texture2d<float> heightTex,
                                      sampler hsamp)
{
    const int P = int(tu.k.w);
    float3 B[3];
    subTriCorners(subs[iid], P, B);
    // Compose: the hardware's barycentric inside the SUB-triangle, mapped back
    // into the BASE triangle's barycentric.
    const float3 g = pc.x * B[0] + pc.y * B[1] + pc.z * B[2];

    float3 op = g.x * cp[0].pos     + g.y * cp[1].pos     + g.z * cp[2].pos;
    float3 on = g.x * cp[0].normal  + g.y * cp[1].normal  + g.z * cp[2].normal;
    float2 uv = g.x * cp[0].uv      + g.y * cp[1].uv      + g.z * cp[2].uv;
    float4 ot = g.x * cp[0].tangent + g.y * cp[1].tangent + g.z * cp[2].tangent;
    ot.w = cp[0].tangent.w;          // handedness is per FACE, not interpolable

    const float3 nrm = normalize(on);
    // THE DISPLACEMENT, term for term the CPU bake's:
    //   offset = amp * (h - mean) along the (interpolated) vertex normal.
    // `mean` is the height map's own average, the analogue of the CPU bake's
    // mipMean, so the surface swings both ways about the authored plane
    // instead of only protruding.
    const float h = heightTex.sample(hsamp, uv, level(tu.k2.w)).r;
    op += nrm * (tu.k.y * (h - tu.k.z));

    const float3 wp  = rowmul(b.rotRow0, b.rotRow1, b.rotRow2, op) + b.objPos;
    const float3 rel = wp - u.camSrc;
    const float3 vp  = rowmul(u.camRow0, u.camRow1, u.camRow2, rel);

    GBufVertexOut o;
    o.position = float4(u.sx * vp.x + u.ox * vp.z,
                        u.sy * vp.y + u.oy * vp.z,
                        u.dza * vp.z + u.dzb,
                        vp.z);
    o.uv = uv;
    const float3 wn = rowmul(b.rotRow0, b.rotRow1, b.rotRow2, nrm);
    o.viewNormal = rowmul(u.camRow0, u.camRow1, u.camRow2, wn);
    const float3 wt = rowmul(b.rotRow0, b.rotRow1, b.rotRow2, ot.xyz);
    o.viewTangent = float4(rowmul(u.camRow0, u.camRow1, u.camRow2, wt), ot.w);
    o.viewPos  = vp;
    o.worldPos = wp;
    return o;
}

[[patch(triangle, 3)]]
vertex GBufVertexOut vs_gbuffer_tess(patch_control_point<TessCP> cp [[stage_in]],
                                     float3 pc [[position_in_patch]],
                                     uint iid  [[instance_id]],
                                     constant FrameUniforms &u  [[buffer(1)]],
                                     constant BatchUniforms &b  [[buffer(2)]],
                                     constant TessUniforms &tu  [[buffer(3)]],
                                     const device SubTri *subs  [[buffer(4)]],
                                     texture2d<float> heightTex [[texture(0)]],
                                     sampler hsamp [[sampler(0)]])
{
    return tessShade(cp, pc, iid, u, b, tu, subs, heightTex, hsamp);
}

// STATS variant — one atomic increment per generated vertex. A separate entry
// point rather than a uniform branch so the timed pipeline carries no trace of
// it. Never used in a timing run.
[[patch(triangle, 3)]]
vertex GBufVertexOut vs_gbuffer_tess_stats(patch_control_point<TessCP> cp [[stage_in]],
                                           float3 pc [[position_in_patch]],
                                           uint iid  [[instance_id]],
                                           constant FrameUniforms &u  [[buffer(1)]],
                                           constant BatchUniforms &b  [[buffer(2)]],
                                           constant TessUniforms &tu  [[buffer(3)]],
                                           const device SubTri *subs  [[buffer(4)]],
                                           device atomic_uint *stats  [[buffer(5)]],
                                           texture2d<float> heightTex [[texture(0)]],
                                           sampler hsamp [[sampler(0)]])
{
    atomic_fetch_add_explicit(&stats[2], 1u, memory_order_relaxed);
    return tessShade(cp, pc, iid, u, b, tu, subs, heightTex, hsamp);
}

// Fragment shader for the --tess_stats probe ONLY. Counts the fragments the
// tessellated stone shades, so "triangles per covered pixel" is a measurement
// rather than an estimate. It writes nothing useful (the stats frame is untimed
// and is followed by a re-warm), and it carries NO discard_fragment(), so the
// early-depth test stays enabled and geometry drawn BEFORE the stone rejects
// its hidden fragments. Geometry drawn AFTER cannot, so the count is an UPPER
// BOUND on visible stone pixels.
fragment GBufOut fs_gbuffer_fragcount(GBufVertexOut in [[stage_in]],
                                      device atomic_uint *stats [[buffer(5)]])
{
    atomic_fetch_add_explicit(&stats[6], 1u, memory_order_relaxed);
    GBufOut o;
    o.albedo = float4(0.0f);
    o.normal = float2(0.0f);
    o.params = float4(0.0f);
    o.mirror = uint4(0u);
    return o;
}
