// deferred.hlsl — Direct3D 11 PBR Deferred Shader matching REVIVAL / FLOOD Engine
// Ported term-for-term from deferred.metal to ensure parity.

// --------------------------------------------------------------------------
// CONSTANT BUFFERS
// --------------------------------------------------------------------------
cbuffer FrameUniforms : register(b0)
{
    float4 camRow0;
    float4 camRow1;
    float4 camRow2;
    float4 camSrc;
    float  sx, ox, sy, oy;
    float  dza, dzb, invSx, invSy;
    float  nearZ, farZ, exposure;
    uint   numLights;
    uint   shadowsOn;
    float  ambientFactor, diffuseFactor, specularFactor;
    float  lightRangeScale;
    int    vizLight;
    float2 pad0;
    float4 clipPlane;
    uint   mirrorCount;
    float  envReflGain;
    float2 reflUv;
    float4 aabbMin;
    float4 aabbMax;
    float4 envProbePos[8];
    float4 metalCompat;
    float4 flatAmbient;
    float4 hdrMode;
};

cbuffer BatchUniforms : register(b1)
{
    float4 rotRow0;
    float4 rotRow1;
    float4 rotRow2;
    float4 objPos;
    float4 baseColor;   // .rgb = base color, .w = hasTexture (1 or 0)
    float4 matParams;   // .x = diffuse, .y = specular, .z = GGX roughness, .w = luminosity
    float4 mapFlags;    // .x = hasNormalMap, .y = hasRoughMap, .z = aoInAlpha, .w = parallaxScale
    float4 misc;        // .x = mirrorIndex, .y = hasAoMap, .z = hasMetalMap, .w = aoStrength*2
    float4 misc2;       // .x = envProbeID, .y = reflection, .z = xparAlpha, .w = dstWeight
    float4 xpar;
};

struct GpuLight {
    float4 pos;       // xyz = pos, w = range
    float4 color;     // xyz = color * intensity, w = invRange
    float4 dir;       // xyz = dir, w = isSpot
    float4 params;    // x = cosInner, y = cosOuter, z = shadowIndex, w = pad
    float4 sRow0;     // .w = 1/tan(halfFov) for spots
    float4 sRow1;
    float4 sRow2;
    float shadowNear;
    float shadowFar;
    float2 pad;
};

cbuffer LightUniforms : register(b2) {
    GpuLight lights[64];
};

// --------------------------------------------------------------------------
// G-BUFFER PASS — vertex + pixel
// --------------------------------------------------------------------------
struct VS_INPUT
{
    float3 pos     : POSITION;
    float3 normal  : NORMAL;
    float2 uv      : TEXCOORD0;
    float4 tangent : TANGENT;
};

struct GBUFFER_VS_OUTPUT
{
    float4 position    : SV_POSITION;
    float2 uv          : TEXCOORD0;
    float3 viewNormal  : TEXCOORD1;
    float4 viewTangent : TEXCOORD2;
    float3 viewPos     : TEXCOORD3;
    float3 worldPos    : TEXCOORD4;
};

struct GBUFFER_PS_OUTPUT
{
    float4 albedo   : SV_TARGET0;   // .rgb = albedo (gamma), .a = AO
    float4 normal   : SV_TARGET1;   // .rg  = oct_encode(viewN)
    float4 material : SV_TARGET2;   // .x = diffuse, .y = spec, .z = rough, .w = sqrt(lum/128)
    float4 mirror   : SV_TARGET3;   // .x = mirrorIdx/255, .y = metal, .z = envProbe/255, .w = F0
};

GBUFFER_VS_OUTPUT vs_gbuffer(VS_INPUT input)
{
    float3 wp = float3(dot(rotRow0.xyz, input.pos), dot(rotRow1.xyz, input.pos), dot(rotRow2.xyz, input.pos)) + objPos.xyz;
    float3 rel = wp - camSrc.xyz;
    float3 vp = float3(dot(camRow0.xyz, rel), dot(camRow1.xyz, rel), dot(camRow2.xyz, rel));

    GBUFFER_VS_OUTPUT output;
    output.position.x = sx * vp.x + ox * vp.z;
    output.position.y = sy * vp.y + oy * vp.z;
    output.position.z = dza * vp.z + dzb;
    output.position.w = vp.z;
    output.uv = input.uv;
    output.viewPos = vp;
    output.worldPos = wp;

    float3 wn = float3(dot(rotRow0.xyz, input.normal), dot(rotRow1.xyz, input.normal), dot(rotRow2.xyz, input.normal));
    output.viewNormal = float3(dot(camRow0.xyz, wn), dot(camRow1.xyz, wn), dot(camRow2.xyz, wn));

    float3 wt = float3(dot(rotRow0.xyz, input.tangent.xyz), dot(rotRow1.xyz, input.tangent.xyz), dot(rotRow2.xyz, input.tangent.xyz));
    output.viewTangent = float4(float3(dot(camRow0.xyz, wt), dot(camRow1.xyz, wt), dot(camRow2.xyz, wt)), input.tangent.w);

    return output;
}

// Material texture slots for G-buffer pass
Texture2D g_albedoTex : register(t0);
Texture2D g_normalTex : register(t1);
Texture2D g_roughTex  : register(t2);
Texture2D g_aoTex     : register(t3);
Texture2D g_metalTex  : register(t4);
Texture2D<float4> reflTex   : register(t5);
Texture2DArray<float4> mirrorTex : register(t6);

TextureCube<float> shadowCubes[16] : register(t7);
Texture2D<float> shadowSpots[16]   : register(t23);
SamplerComparisonState shadowSamp  : register(s1);

float sampleSpotShadow(int sIdx, float2 uv, float refZ) {
    switch (sIdx) {
        case 0: return shadowSpots[0].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 1: return shadowSpots[1].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 2: return shadowSpots[2].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 3: return shadowSpots[3].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 4: return shadowSpots[4].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 5: return shadowSpots[5].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 6: return shadowSpots[6].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 7: return shadowSpots[7].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 8: return shadowSpots[8].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 9: return shadowSpots[9].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 10: return shadowSpots[10].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 11: return shadowSpots[11].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 12: return shadowSpots[12].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 13: return shadowSpots[13].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 14: return shadowSpots[14].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 15: return shadowSpots[15].SampleCmpLevelZero(shadowSamp, uv, refZ);
    }
    return 1.0f;
}

float evalShadow(GpuLight L, float3 wpos, float dza, float dzb) {
    if (L.params.z < 0) return 1.0f;
    int sIdx = (int)L.params.z;
    if (L.dir.w > 0.5f) { // isSpot
        float3 rel = wpos - L.pos.xyz;
        float3 vp = float3(
            L.sRow0.x * rel.x + L.sRow0.y * rel.y + L.sRow0.z * rel.z,
            L.sRow1.x * rel.x + L.sRow1.y * rel.y + L.sRow1.z * rel.z,
            L.sRow2.x * rel.x + L.sRow2.y * rel.y + L.sRow2.z * rel.z
        );
        float2 uv = vp.xy * L.sRow0.w * (0.5f / vp.z) + 0.5f;
        uv.y = 1.0f - uv.y;
        if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f || vp.z <= 0.0f) return 1.0f;
        
        float refZ = saturate((dza * vp.z + dzb) / vp.z) - 1e-4f;
        return sampleSpotShadow(sIdx, uv, refZ);
    }
    return 1.0f; // omni cube shadows not implemented in shader yet
}


TextureCube<float> shadowCubes[16] : register(t7);
Texture2D<float> shadowSpots[16]   : register(t23);
SamplerComparisonState shadowSamp  : register(s1);

float sampleSpotShadow(int sIdx, float2 uv, float refZ) {
    switch (sIdx) {
        case 0: return shadowSpots[0].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 1: return shadowSpots[1].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 2: return shadowSpots[2].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 3: return shadowSpots[3].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 4: return shadowSpots[4].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 5: return shadowSpots[5].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 6: return shadowSpots[6].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 7: return shadowSpots[7].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 8: return shadowSpots[8].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 9: return shadowSpots[9].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 10: return shadowSpots[10].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 11: return shadowSpots[11].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 12: return shadowSpots[12].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 13: return shadowSpots[13].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 14: return shadowSpots[14].SampleCmpLevelZero(shadowSamp, uv, refZ);
        case 15: return shadowSpots[15].SampleCmpLevelZero(shadowSamp, uv, refZ);
    }
    return 1.0f;
}

float evalShadow(GpuLight L, float3 wpos, float dza, float dzb) {
    if (L.params.z < 0) return 1.0f;
    int sIdx = (int)L.params.z;
    if (L.dir.w > 0.5f) { // isSpot
        float3 rel = wpos - L.pos.xyz;
        float3 vp = float3(
            L.sRow0.x * rel.x + L.sRow0.y * rel.y + L.sRow0.z * rel.z,
            L.sRow1.x * rel.x + L.sRow1.y * rel.y + L.sRow1.z * rel.z,
            L.sRow2.x * rel.x + L.sRow2.y * rel.y + L.sRow2.z * rel.z
        );
        float2 uv = vp.xy * L.sRow0.w * (0.5f / vp.z) + 0.5f;
        uv.y = 1.0f - uv.y;
        if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f || vp.z <= 0.0f) return 1.0f;
        
        float refZ = saturate((dza * vp.z + dzb) / vp.z) - 1e-4f;
        return sampleSpotShadow(sIdx, uv, refZ);
    }
    return 1.0f; // omni cube shadows not implemented in shader yet
}

SamplerState g_sampler : register(s0);

// Octahedral normal encoding/decoding (matches deferred.metal oct_encode/oct_decode)
float2 oct_encode(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 e = n.xy;
    if (n.z < 0.0f) {
        e = (1.0f - abs(n.yx)) * (n.xy >= 0.0f ? float2(1.0f, 1.0f) : float2(-1.0f, -1.0f));
    }
    return e * 0.5f + 0.5f;
}

float3 oct_decode(float2 e)
{
    e = e * 2.0f - 1.0f;
    float3 n = float3(e, 1.0f - abs(e.x) - abs(e.y));
    float t = saturate(-n.z);
    n.x += (n.x >= 0.0f) ? -t : t;
    n.y += (n.y >= 0.0f) ? -t : t;
    return normalize(n);
}

GBUFFER_PS_OUTPUT ps_gbuffer(GBUFFER_VS_OUTPUT input)
{
    GBUFFER_PS_OUTPUT output;

    // 1. Albedo
    float4 alb = float4(baseColor.rgb, 1.0f);
    if (baseColor.w > 0.5f) {
        alb = g_albedoTex.Sample(g_sampler, input.uv);
    }

    // 2. Ambient Occlusion
    float aoRaw = 1.0f;
    if (mapFlags.z > 0.5f) {
        aoRaw = alb.a;                                        // Mat_AoInAlpha
    } else if (misc.y > 0.5f) {
        aoRaw = g_aoTex.Sample(g_sampler, input.uv).r;        // Material::AoMap
    }
    float ao = saturate(1.0f - misc.w * (1.0f - aoRaw));       // misc.w = 2*aoStrength
    output.albedo = float4(alb.rgb, ao);

    // 3. Normal mapping
    float3 n = normalize(input.viewNormal);
    if (mapFlags.x > 0.5f) {
        float3 t = input.viewTangent.xyz;
        t -= n * dot(n, t);
        float tLen2 = dot(t, t);
        if (tLen2 > 1e-12f) {
            t *= rsqrt(tLen2);
        } else {
            float3 ref = (abs(n.y) < 0.9f) ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
            t = normalize(cross(ref, n));
        }
        float3 bt = cross(n, t) * input.viewTangent.w;
        float3 tn = g_normalTex.Sample(g_sampler, input.uv).xyz * 2.0f - 1.0f;
        n = normalize(tn.x * t + tn.y * bt + tn.z * n);
    }
    output.normal = float4(oct_encode(n), 0.0f, 1.0f);

    // 4. Roughness-attenuated specular
    float spec = matParams.y;
    if (mapFlags.y > 0.5f) {
        spec *= max(0.0f, 1.0f - g_roughTex.Sample(g_sampler, input.uv).r);
    }
    float lum = matParams.w;
    output.material = float4(matParams.x, spec, matParams.z, sqrt(saturate(lum * (1.0f / 128.0f))));

    // 5. Metalness & Fresnel F0
    float metal = (misc.z > 0.5f) ? g_metalTex.Sample(g_sampler, input.uv).r : 0.0f;
    float f0 = max(misc2.y * 0.01f, 0.04f);
    f0 = lerp(f0, 0.98f, saturate(metal));
    // Store as normalized 0..1 values (D3D11 uses UNORM, not UINT like Metal)
    output.mirror = float4(misc.x / 255.0f, saturate(metal), misc2.x / 255.0f, f0);

    return output;
}

// --------------------------------------------------------------------------
// RESOLVE PASS — fullscreen triangle
// --------------------------------------------------------------------------
struct QUAD_VS_OUTPUT
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

QUAD_VS_OUTPUT vs_quad(uint id : SV_VertexID)
{
    QUAD_VS_OUTPUT output;
    output.uv = float2((id == 1) ? 2.0 : 0.0, (id == 2) ? 2.0 : 0.0);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

// G-Buffer SRVs for resolve pass
Texture2D g_gbufAlbedo   : register(t0);
Texture2D g_gbufNormal   : register(t1);
Texture2D g_gbufMaterial : register(t2);
Texture2D g_gbufMirror   : register(t3);
Texture2D g_gbufDepth    : register(t4);

// --------------------------------------------------------------------------
// BRDF functions — term-for-term match of deferred.metal
// --------------------------------------------------------------------------
float D_GGX(float NoH, float a2)
{
    float d = NoH * NoH * (a2 - 1.0f) + 1.0f;
    return a2 / max(3.14159265f * d * d, 1e-7f);
}

float V_SmithSchlick(float NoV, float NoL, float a2)
{
    float k = a2 * 0.5f;
    float gv = NoV * (1.0f - k) + k;
    float gl = NoL * (1.0f - k) + k;
    return 0.25f / max(gv * gl, 1e-7f);
}

float4 ps_resolve(QUAD_VS_OUTPUT input) : SV_TARGET
{
    int3 px = int3(input.position.xy, 0);

    float depth = g_gbufDepth.Load(px).r;
    if (depth <= 0.0f) {
        // Sky / background
        return float4(0.02f, 0.02f, 0.04f, 1.0f);
    }

    // ---- Decode G-Buffer ----
    float4 albAo  = g_gbufAlbedo.Load(px);
    float2 normEnc= g_gbufNormal.Load(px).rg;
    float4 par    = g_gbufMaterial.Load(px);
    float4 mirEnc = g_gbufMirror.Load(px);

    // Reconstruct view position (matches Metal's DecodeSurface)
    float Z = dzb / max(depth - dza, 1e-9f);
    float ndcx = input.uv.x * 2.0f - 1.0f;
    float ndcy = 1.0f - input.uv.y * 2.0f;
    float X = (ndcx - ox) * Z * invSx;
    float Y = (ndcy - oy) * Z * invSy;
    float3 P = float3(X, Y, Z);

    // World position reconstruction
    float3 pw = camSrc.xyz + camRow0.xyz * P.x + camRow1.xyz * P.y + camRow2.xyz * P.z;

    // Surface decoding
    float3 N = oct_decode(normEnc);
    float3 V = normalize(-P);
    float  NoV = saturate(dot(N, V));

    // Albedo: square for hdr_linear mode (matching Metal's DecodeSurface)
    float3 rawAlbedo = albAo.rgb;
    float3 baseCol = (hdrMode.x > 0.5f) ? (rawAlbedo * rawAlbedo) : rawAlbedo;
    float  ao = albAo.a;

    // Material params (diffuse, spec scaled by diffuse/specular factors)
    float  diffuseK = par.x * diffuseFactor;
    float  specK    = par.y * specularFactor;
    float  rough    = clamp(par.z, 0.04f, 1.0f);
    float  a        = rough * rough;
    float  lum      = par.w * par.w * 128.0f;   // Decode sqrt(lum/128)

    // Metalness (D3D11 UNORM: already 0..1)
    float  metal = mirEnc.y;

    // ---- Direct Lighting ----
    // Matches Metal DirectRadiance term-for-term.
    // NO Lambert 1/pi on diffuse, NO (1-F) energy factor — CPU PARITY.
    // See deferred.metal:594-618 for the rationale.
    float3 directLight = 0.0f;
    for (uint i = 0; i < numLights && i < 64; ++i) {
        GpuLight Li = lights[i];

        // Light position in view space
        float3 lRel = Li.pos.xyz - camSrc.xyz;
        float3 lPos = float3(dot(camRow0.xyz, lRel), dot(camRow1.xyz, lRel), dot(camRow2.xyz, lRel));
        float3 toL = lPos - P;
        float d2 = dot(toL, toL);
        float range = Li.pos.w * lightRangeScale;
        if (range <= 0.0f || d2 >= range * range) continue;

        float d = sqrt(d2);
        float3 Ldir = toL / max(d, 1e-6f);
        float NoL = saturate(dot(N, Ldir));
        if (NoL <= 0.0f) continue;

        // Linear falloff to the hard edge — the engine's attenuation shape
        float atten = saturate(1.0f - d * (Li.color.w / lightRangeScale));

        // Spot cone
        if (Li.dir.w > 0.5f) {
            float3 wDir = normalize(pw - Li.pos.xyz);
            float cosTheta = dot(Li.dir.xyz, wDir);
            if (cosTheta <= Li.params.y) continue;
            if (cosTheta < Li.params.x) {
                float tt = (cosTheta - Li.params.y) / max(Li.params.x - Li.params.y, 1e-6f);
                atten *= tt * tt * (3.0f - 2.0f * tt);
            }
        }

        // BRDF
        float3 H = normalize(Ldir + V);
        float NoH = saturate(dot(N, H));
        float VoH = saturate(dot(V, H));

        // Fresnel: fixed F0=0.04, exactly as the CPU does
        float om = 1.0f - VoH;
        float om2 = om * om;
        float Fd = 0.04f + 0.96f * (om2 * om2 * om);

        float spec1 = Fd * specK * D_GGX(NoH, a) * V_SmithSchlick(max(NoV, 1e-3f), NoL, a);

        // Conductor diffuse kill + specular tint (metalCompat dials)
        float metalKill = lerp(1.0f - metal, 1.0f, metalCompat.x);
        float3 tintAlbedo = lerp(baseCol, sqrt(baseCol), metalCompat.y);
        float3 diff = baseCol * diffuseK * metalKill;
        float3 specTint = lerp(float3(1.0f, 1.0f, 1.0f), tintAlbedo, metal);

        float shadow = shadowsOn ? evalShadow(Li, pw, dza, dzb) : 1.0f;
        directLight += (diff + spec1 * specTint) * Li.color.rgb * (NoL * atten * shadow);
    }

    // ---- Ambient ----
    // Matches Metal AmbientRadiance: world normal for sky gradient lookup
    float3 Nw = normalize(camRow0.xyz * N.x + camRow1.xyz * N.y + camRow2.xyz * N.z);
    float3 irr;
    if (flatAmbient.w > 0.5f) {
        irr = flatAmbient.rgb;
    } else {
        // L2 SH sky gradient from authored zenith/nadir (approximation: simple
        // Y-based lerp matching the CPU's SH evaluation of a smooth vertical gradient)
        float3 skyZen = float3(0.0f, 40.0f, 80.0f) / 255.0f;
        float3 skyNad = float3(100.0f, 80.0f, 60.0f) / 255.0f;
        irr = lerp(skyNad, skyZen, Nw.y * 0.5f + 0.5f) * ambientFactor;
    }
    float metalKillAmb = lerp(1.0f - metal, 1.0f, metalCompat.x);
    float3 ambient = baseCol * diffuseK * metalKillAmb * irr * ao;

    // ---- Emissive ----
    float3 emissive = baseCol * lum;

    // ---- Composite ----
    float3 radiance = directLight + ambient + emissive;
    radiance *= exposure;

    // ---- Tonemap (ACES Narkowicz) ----
    float3 x = radiance;
    float3 color = saturate((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f));

    // Sqrt gamma encoding for hdr_linear mode (matching Metal's fs_tonemap)
    if (hdrMode.x > 0.5f) color = sqrt(color);


    if (mirrorCount > 0) {
        uint mid = (uint)(g_gbufMirror.Load(int3(px, 0)).x * 255.0f + 0.5f);
        if (mid > 0 && mid <= mirrorCount) {
            float3 r = mirrorTex.Load(int4(px.x, px.y, mid - 1, 0)).rgb;
            color += r * 0.5f;
        }
    }


    if (mirrorCount > 0) {
        uint mid = (uint)(g_gbufMirror.Load(int3(px, 0)).x * 255.0f + 0.5f);
        if (mid > 0 && mid <= mirrorCount) {
            float3 r = mirrorTex.Load(int4(px.x, px.y, mid - 1, 0)).rgb;
            color += r * 0.5f;
        }
    }

    return float4(color, 1.0f);
}
