#include "GpuWeb.h"

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

#include "SceneIngest.h"
#include "GreetsDisco.h"
#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Base/Scene.h>
#include <Base/FeatureFlags.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>

extern bool g_editorPlaying;

namespace rev {

namespace {

static bool s_enabled = false;
static bool s_initialized = false;
static gpubench::Scene s_scene;
static gpubench::LoadOptions s_opt;
static std::string s_loadedSceneName;

#ifdef __EMSCRIPTEN__

// ---------------------------------------------------------------------------
// G-Buffer resources
// ---------------------------------------------------------------------------
static int s_gbufWidth = 0;
static int s_gbufHeight = 0;
static GLuint s_gbufFbo = 0;
static GLuint s_gbufAlbedo = 0;    // RT0: rgb = albedo (gamma), a = AO
static GLuint s_gbufNormal = 0;    // RT1: rg = oct_encode(viewNormal)
static GLuint s_gbufMaterial = 0;  // RT2: x=diffuse, y=spec, z=rough, w=sqrt(lum/128)
static GLuint s_gbufMirror = 0;    // RT3: x=mirrorIdx/255, y=metal, z=envProbe/255, w=F0
static GLuint s_gbufDepth = 0;     // Depth: DEPTH_COMPONENT24

// ---------------------------------------------------------------------------
// Spotlight Shadow Maps
// ---------------------------------------------------------------------------
static const int kMaxShadowSpots = 8;
static const int kShadowDim = 1024;
static GLuint s_shadowFbo = 0;
static GLuint s_shadowSpots[kMaxShadowSpots] = {0};
static int s_numShadowSpots = 0;
static GLuint s_ptextTex = 0;

// ---------------------------------------------------------------------------
// Shaders & Geometry Buffers
// ---------------------------------------------------------------------------
static GLuint s_gbufProg = 0;
static GLuint s_shadowProg = 0;
static GLuint s_resolveProg = 0;
static GLuint s_coneProg = 0;
static GLuint s_xparProg = 0;
static GLuint s_flareProg = 0;

static GLuint s_sceneVao = 0;
static GLuint s_sceneVbo = 0;

static GLuint s_quadVao = 0;
static GLuint s_flareVao = 0;

static GLuint s_whiteTex = 0;
static GLuint s_flatNormalTex = 0;
static GLuint s_envTex = 0;
static std::vector<GLuint> s_textures;

// G-Buffer Shader Uniform Locations
static GLint loc_g_uCamRow0 = -1, loc_g_uCamRow1 = -1, loc_g_uCamRow2 = -1, loc_g_uCamSrc = -1;
static GLint loc_g_uSx = -1, loc_g_uOx = -1, loc_g_uSy = -1, loc_g_uOy = -1, loc_g_uDza = -1, loc_g_uDzb = -1;
static GLint loc_g_uRotRow0 = -1, loc_g_uRotRow1 = -1, loc_g_uRotRow2 = -1, loc_g_uObjPos = -1;
static GLint loc_g_uBaseColor = -1, loc_g_uMatParams = -1, loc_g_uMapFlags = -1, loc_g_uMisc = -1, loc_g_uMisc2 = -1;

// Shadow Shader Uniform Locations
static GLint loc_sh_uRotRow0 = -1, loc_sh_uRotRow1 = -1, loc_sh_uRotRow2 = -1, loc_sh_uObjPos = -1;
static GLint loc_sh_uLightRow0 = -1, loc_sh_uLightRow1 = -1, loc_sh_uLightRow2 = -1, loc_sh_uLightPos = -1;
static GLint loc_sh_uProjData = -1;

// Resolve Shader Uniform Locations
static GLint loc_r_uCamRow0 = -1, loc_r_uCamRow1 = -1, loc_r_uCamRow2 = -1, loc_r_uCamSrc = -1;
static GLint loc_r_uSx = -1, loc_r_uOx = -1, loc_r_uSy = -1, loc_r_uOy = -1, loc_r_uDza = -1, loc_r_uDzb = -1;
static GLint loc_r_uInvSx = -1, loc_r_uInvSy = -1;
static GLint loc_r_uNearZ = -1, loc_r_uFarZ = -1, loc_r_uExposure = -1;
static GLint loc_r_uDiffuseFactor = -1, loc_r_uSpecularFactor = -1, loc_r_uAmbientFactor = -1;
static GLint loc_r_uSkyZenith = -1, loc_r_uSkyNadir = -1, loc_r_uFlatAmbient = -1, loc_r_uHdrMode = -1;
static GLint loc_r_uNumLights = -1, loc_r_uShadowsOn = -1, loc_r_uHasEnvTex = -1;

static const int kMaxLights = 32;
struct LightResolveLoc {
    GLint pos = -1;
    GLint color = -1;
    GLint dir = -1;
    GLint params = -1;
    GLint sRow0 = -1;
    GLint sRow1 = -1;
    GLint sRow2 = -1;
    GLint nearFar = -1;
} s_resolveLightLocs[kMaxLights];

// Cone Shader Uniform Locations
static GLint loc_c_uCamRow0 = -1, loc_c_uCamRow1 = -1, loc_c_uCamRow2 = -1, loc_c_uCamSrc = -1;
static GLint loc_c_uSx = -1, loc_c_uOx = -1, loc_c_uSy = -1, loc_c_uOy = -1, loc_c_uDza = -1, loc_c_uDzb = -1;
static GLint loc_c_uInvSx = -1, loc_c_uInvSy = -1, loc_c_uFarZ = -1;
static GLint loc_c_uNumLights = -1, loc_c_uShadowsOn = -1;
static GLint loc_c_uConeDensity = -1, loc_c_uConeNSamples = -1, loc_c_uConeFadeFloor = -1;
static LightResolveLoc s_coneLightLocs[kMaxLights];

// Xpar Shader Uniform Locations
static GLint loc_x_uCamRow0 = -1, loc_x_uCamRow1 = -1, loc_x_uCamRow2 = -1, loc_x_uCamSrc = -1;
static GLint loc_x_uSx = -1, loc_x_uOx = -1, loc_x_uSy = -1, loc_x_uOy = -1, loc_x_uDza = -1, loc_x_uDzb = -1;
static GLint loc_x_uRotRow0 = -1, loc_x_uRotRow1 = -1, loc_x_uRotRow2 = -1, loc_x_uObjPos = -1;
static GLint loc_x_uBaseColor = -1, loc_x_uMatParams = -1, loc_x_uMisc2 = -1, loc_x_uAmbient = -1;
static GLint loc_x_uNumLights = -1;
static LightResolveLoc s_xparLightLocs[kMaxLights];

// Flare Shader Uniform Locations
static GLint loc_f_uCamRow0 = -1, loc_f_uCamRow1 = -1, loc_f_uCamRow2 = -1, loc_f_uCamSrc = -1;
static GLint loc_f_uSx = -1, loc_f_uOx = -1, loc_f_uSy = -1, loc_f_uOy = -1, loc_f_uDza = -1, loc_f_uDzb = -1;
static GLint loc_f_uFlareCenter = -1, loc_f_uFlareGain = -1;

// ---------------------------------------------------------------------------
// GLSL ES 3.00 Shader Sources
// ---------------------------------------------------------------------------

static const char* kGbufVertexShader = R"(#version 300 es
precision highp float;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aTangent;

uniform vec3 uCamRow0, uCamRow1, uCamRow2, uCamSrc;
uniform float uSx, uOx, uSy, uOy, uDza, uDzb;
uniform vec3 uRotRow0, uRotRow1, uRotRow2, uObjPos;

out vec2 vUV;
out vec3 vViewPos;
out vec3 vWorldPos;
out vec3 vViewNormal;
out vec4 vViewTangent;

void main() {
    vec3 wp = vec3(dot(uRotRow0, aPos), dot(uRotRow1, aPos), dot(uRotRow2, aPos)) + uObjPos;
    vec3 rel = wp - uCamSrc;
    vec3 vp = vec3(dot(uCamRow0, rel), dot(uCamRow1, rel), dot(uCamRow2, rel));

    gl_Position.x = uSx * vp.x + uOx * vp.z;
    gl_Position.y = uSy * vp.y + uOy * vp.z;
    gl_Position.z = (2.0 * uDza - 1.0) * vp.z + (2.0 * uDzb);
    gl_Position.w = vp.z;

    vUV = aUV;
    vViewPos = vp;
    vWorldPos = wp;

    vec3 wn = vec3(dot(uRotRow0, aNormal), dot(uRotRow1, aNormal), dot(uRotRow2, aNormal));
    vViewNormal = vec3(dot(uCamRow0, wn), dot(uCamRow1, wn), dot(uCamRow2, wn));

    vec3 wt = vec3(dot(uRotRow0, aTangent.xyz), dot(uRotRow1, aTangent.xyz), dot(uRotRow2, aTangent.xyz));
    vViewTangent = vec4(vec3(dot(uCamRow0, wt), dot(uCamRow1, wt), dot(uCamRow2, wt)), aTangent.w);
}
)";

static const char* kGbufFragmentShader = R"(#version 300 es
precision highp float;

in vec2 vUV;
in vec3 vViewPos;
in vec3 vWorldPos;
in vec3 vViewNormal;
in vec4 vViewTangent;

uniform vec4 uBaseColor; // rgb = base color, w = hasTexture (1 or 0)
uniform vec4 uMatParams; // x = diffuse, y = spec, z = GGX rough, w = luminosity
uniform vec4 uMapFlags;  // x = hasNormalMap, y = hasRoughMap, z = aoInAlpha, w = parallaxScale
uniform vec4 uMisc;      // x = mirrorIndex, y = hasAoMap, z = hasMetalMap, w = aoStrength*2
uniform vec4 uMisc2;     // x = envProbeID, y = reflection, z = xparAlpha, w = dstWeight

uniform sampler2D uAlbedoTex;
uniform sampler2D uNormalTex;
uniform sampler2D uRoughTex;
uniform sampler2D uAoTex;
uniform sampler2D uMetalTex;

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outMaterial;
layout(location = 3) out vec4 outMirror;

vec2 oct_encode(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    vec2 e = n.xy;
    if (n.z < 0.0) {
        e = (1.0 - abs(n.yx)) * (step(0.0, n.xy) * 2.0 - 1.0);
    }
    return e * 0.5 + 0.5;
}

void main() {
    vec4 alb = vec4(uBaseColor.rgb, 1.0);
    if (uBaseColor.w > 0.5) {
        alb = texture(uAlbedoTex, vUV);
    }

    float aoRaw = 1.0;
    if (uMapFlags.z > 0.5) {
        aoRaw = alb.a;
    } else if (uMisc.y > 0.5) {
        aoRaw = texture(uAoTex, vUV).r;
    }
    float ao = clamp(1.0 - uMisc.w * (1.0 - aoRaw), 0.0, 1.0);
    outAlbedo = vec4(alb.rgb, ao);

    vec3 n = normalize(vViewNormal);
    if (uMapFlags.x > 0.5) {
        vec3 t = vViewTangent.xyz;
        t -= n * dot(n, t);
        float tLen2 = dot(t, t);
        if (tLen2 > 1e-12) {
            t *= inversesqrt(tLen2);
        } else {
            vec3 ref = (abs(n.y) < 0.9) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
            t = normalize(cross(ref, n));
        }
        vec3 bt = cross(n, t) * vViewTangent.w;
        vec3 tn = texture(uNormalTex, vUV).xyz * 2.0 - 1.0;
        n = normalize(tn.x * t + tn.y * bt + tn.z * n);
    }
    outNormal = vec4(oct_encode(n), 0.0, 1.0);

    float spec = uMatParams.y;
    if (uMapFlags.y > 0.5) {
        spec *= max(0.0, 1.0 - texture(uRoughTex, vUV).r);
    }
    float lum = uMatParams.w;
    outMaterial = vec4(uMatParams.x, spec, uMatParams.z, sqrt(clamp(lum * (1.0 / 128.0), 0.0, 1.0)));

    float metal = (uMisc.z > 0.5) ? texture(uMetalTex, vUV).r : 0.0;
    float f0 = max(uMisc2.y * 0.01, 0.04);
    f0 = mix(f0, 0.98, clamp(metal, 0.0, 1.0));
    outMirror = vec4(uMisc.x / 255.0, clamp(metal, 0.0, 1.0), uMisc2.x / 255.0, f0);
}
)";

static const char* kShadowVertexShader = R"(#version 300 es
precision highp float;

layout(location = 0) in vec3 aPos;

uniform vec3 uRotRow0, uRotRow1, uRotRow2, uObjPos;
uniform vec3 uLightRow0, uLightRow1, uLightRow2, uLightPos;
uniform vec3 uProjData; // x = sDza, y = sDzb, z = sScale

void main() {
    vec3 wp = vec3(dot(uRotRow0, aPos), dot(uRotRow1, aPos), dot(uRotRow2, aPos)) + uObjPos;
    vec3 rel = wp - uLightPos;
    vec3 vp = vec3(dot(uLightRow0, rel), dot(uLightRow1, rel), dot(uLightRow2, rel));

    gl_Position.x = vp.x * uProjData.z;
    gl_Position.y = vp.y * uProjData.z;
    gl_Position.z = (2.0 * uProjData.x - 1.0) * vp.z + (2.0 * uProjData.y);
    gl_Position.w = vp.z;
}
)";

static const char* kShadowFragmentShader = R"(#version 300 es
precision highp float;
void main() {}
)";

static const char* kQuadVertexShader = R"(#version 300 es
precision highp float;

out vec2 vUV;

void main() {
    float x = (gl_VertexID == 1) ? 3.0 : -1.0;
    float y = (gl_VertexID == 2) ? 3.0 : -1.0;
    vUV = vec2(x * 0.5 + 0.5, y * 0.5 + 0.5);
    gl_Position = vec4(x, y, 0.0, 1.0);
}
)";

static const char* kResolveFragmentShader = R"(#version 300 es
precision highp float;

in vec2 vUV;
out vec4 fragColor;

uniform sampler2D uGbufAlbedo;
uniform sampler2D uGbufNormal;
uniform sampler2D uGbufMaterial;
uniform sampler2D uGbufMirror;
uniform sampler2D uGbufDepth;
uniform sampler2D uEnvTex;

uniform highp sampler2DShadow uShadowSpots[8];

uniform vec3 uCamRow0, uCamRow1, uCamRow2, uCamSrc;
uniform float uSx, uOx, uSy, uOy, uDza, uDzb;
uniform float uInvSx, uInvSy;
uniform float uNearZ, uFarZ, uExposure;
uniform float uDiffuseFactor, uSpecularFactor, uAmbientFactor;
uniform vec3 uSkyZenith, uSkyNadir;
uniform vec4 uFlatAmbient;
uniform vec4 uHdrMode;
uniform int uNumLights;
uniform int uShadowsOn;
uniform int uHasEnvTex;

uniform vec4 uLightPos[32];
uniform vec4 uLightColor[32];
uniform vec4 uLightDir[32];
uniform vec4 uLightParams[32];
uniform vec4 uLightSRow0[32];
uniform vec4 uLightSRow1[32];
uniform vec4 uLightSRow2[32];
uniform vec2 uLightNearFar[32];

vec3 oct_decode(vec2 e) {
    e = e * 2.0 - 1.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += (n.x >= 0.0) ? -t : t;
    n.y += (n.y >= 0.0) ? -t : t;
    return normalize(n);
}

float D_GGX(float NoH, float a2) {
    float d = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159265 * d * d, 1e-7);
}

float V_SmithSchlick(float NoV, float NoL, float a2) {
    float k = a2 * 0.5;
    float gv = NoV * (1.0 - k) + k;
    float gl = NoL * (1.0 - k) + k;
    return 0.25 / max(gv * gl, 1e-7);
}

float sampleSpotShadow(int sIdx, vec2 uv, float refZ) {
    vec3 coord = vec3(uv, refZ);
    switch (sIdx) {
        case 0: return texture(uShadowSpots[0], coord);
        case 1: return texture(uShadowSpots[1], coord);
        case 2: return texture(uShadowSpots[2], coord);
        case 3: return texture(uShadowSpots[3], coord);
        case 4: return texture(uShadowSpots[4], coord);
        case 5: return texture(uShadowSpots[5], coord);
        case 6: return texture(uShadowSpots[6], coord);
        case 7: return texture(uShadowSpots[7], coord);
        default: return 1.0;
    }
}

float evalShadow(int sIdx, vec3 wpos, vec3 lpos, vec4 sRow0, vec3 sRow1, vec3 sRow2, vec2 nearFar) {
    if (sIdx < 0 || sIdx >= 8) return 1.0;
    vec3 rel = wpos - lpos;
    vec3 vp = vec3(
        dot(sRow0.xyz, rel),
        dot(sRow1, rel),
        dot(sRow2, rel)
    );
    if (vp.z <= nearFar.x) return 1.0;
    
    vec2 ndc = vp.xy * sRow0.w / vp.z;
    vec2 uv = ndc * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;
    
    float sn = nearFar.x, sf = nearFar.y;
    float sDza = sf / (sf - sn);
    float sDzb = (-sf * sn) / (sf - sn);
    float refZ = (sDza * vp.z + sDzb) / vp.z - 0.002;
    return sampleSpotShadow(sIdx, uv, clamp(refZ, 0.0, 1.0));
}

void main() {
    float depth = texture(uGbufDepth, vUV).r;
    if (depth >= 1.0 || depth <= 0.0) {
        fragColor = vec4(0.015, 0.015, 0.025, 1.0);
        return;
    }

    vec4 albAo = texture(uGbufAlbedo, vUV);
    vec2 normEnc = texture(uGbufNormal, vUV).rg;
    vec4 par = texture(uGbufMaterial, vUV);
    vec4 mirEnc = texture(uGbufMirror, vUV);

    float Z = uDzb / min(depth - uDza, -1e-7);

    float ndcx = vUV.x * 2.0 - 1.0;
    float ndcy = vUV.y * 2.0 - 1.0;
    float X = (ndcx - uOx) * Z * uInvSx;
    float Y = (ndcy - uOy) * Z * uInvSy;
    vec3 P = vec3(X, Y, Z);

    vec3 pw = uCamSrc + uCamRow0 * P.x + uCamRow1 * P.y + uCamRow2 * P.z;

    vec3 N = oct_decode(normEnc);
    vec3 V = normalize(-P);
    float NoV = clamp(dot(N, V), 0.0, 1.0);

    vec3 rawAlbedo = albAo.rgb;
    vec3 baseCol = (uHdrMode.x > 0.5) ? (rawAlbedo * rawAlbedo) : rawAlbedo;
    float ao = albAo.a;

    float diffuseK = par.x * uDiffuseFactor;
    float specK = par.y * uSpecularFactor;
    float rough = clamp(par.z, 0.04, 1.0);
    float a = rough * rough;
    float lum = par.w * par.w * 128.0;

    float metal = mirEnc.y;
    float f0 = mirEnc.w;

    // ---- Direct Lighting ----
    vec3 directLight = vec3(0.0);
    for (int i = 0; i < uNumLights && i < 32; ++i) {
        vec3 lpos = uLightPos[i].xyz;
        float range = uLightPos[i].w;
        vec3 lRel = lpos - uCamSrc;
        vec3 lPosView = vec3(dot(uCamRow0, lRel), dot(uCamRow1, lRel), dot(uCamRow2, lRel));
        vec3 toL = lPosView - P;
        float d2 = dot(toL, toL);
        if (range <= 0.0 || d2 >= range * range) continue;

        float d = sqrt(d2);
        vec3 Ldir = toL / max(d, 1e-6);
        float NoL = clamp(dot(N, Ldir), 0.0, 1.0);
        if (NoL <= 0.0) continue;

        float atten = clamp(1.0 - d * uLightColor[i].w, 0.0, 1.0);

        if (uLightDir[i].w > 0.5) {
            vec3 wDir = normalize(pw - lpos);
            float cosTheta = dot(uLightDir[i].xyz, wDir);
            float cosOuter = uLightParams[i].y;
            float cosInner = uLightParams[i].x;
            if (cosTheta <= cosOuter) continue;
            if (cosTheta < cosInner) {
                float tt = (cosTheta - cosOuter) / max(cosInner - cosOuter, 1e-6);
                atten *= tt * tt * (3.0 - 2.0 * tt);
            }
        }

        vec3 H = normalize(Ldir + V);
        float NoH = clamp(dot(N, H), 0.0, 1.0);
        float VoH = clamp(dot(V, H), 0.0, 1.0);

        float om = 1.0 - VoH;
        float om2 = om * om;
        float Fd = 0.04 + 0.96 * (om2 * om2 * om);

        float spec1 = Fd * specK * D_GGX(NoH, a) * V_SmithSchlick(max(NoV, 1e-3), NoL, a);

        float metalKill = 1.0 - metal;
        vec3 tintAlbedo = baseCol;
        vec3 diff = baseCol * diffuseK * metalKill;
        vec3 specTint = mix(vec3(1.0), tintAlbedo, metal);

        float shadow = 1.0;
        if (uShadowsOn != 0) {
            int sIdx = int(uLightParams[i].z);
            if (uLightDir[i].w > 0.5 && sIdx >= 0) {
                shadow = evalShadow(sIdx, pw, lpos, uLightSRow0[i], uLightSRow1[i].xyz, uLightSRow2[i].xyz, uLightNearFar[i]);
            }
        }

        directLight += (diff + spec1 * specTint) * uLightColor[i].rgb * (NoL * atten * shadow);
    }

    // ---- Ambient ----
    vec3 Nw = normalize(uCamRow0 * N.x + uCamRow1 * N.y + uCamRow2 * N.z);
    vec3 irr;
    if (uFlatAmbient.w > 0.5) {
        irr = uFlatAmbient.rgb;
    } else {
        irr = mix(uSkyNadir, uSkyZenith, Nw.y * 0.5 + 0.5) * uAmbientFactor;
    }
    vec3 ambient = baseCol * diffuseK * (1.0 - metal) * irr * ao;

    // ---- Environment Reflection (Disco Ball / Metals) ----
    vec3 refl = vec3(0.0);
    if (uHasEnvTex != 0 && (mirEnc.z > 0.0 || metal > 0.0 || f0 > 0.05)) {
        vec3 Vw = normalize(pw - uCamSrc);
        vec3 Rw = reflect(Vw, Nw);
        vec2 panoUV = vec2(
            atan(-Rw.z, -Rw.x) * (0.5 / 3.14159265) + 0.5,
            asin(clamp(Rw.y, -1.0, 1.0)) * (1.0 / 3.14159265) + 0.5
        );
        vec3 envCol = texture(uEnvTex, panoUV).rgb;
        if (uHdrMode.x > 0.5) envCol = envCol * envCol;
        
        float fres = f0 + (1.0 - f0) * pow(1.0 - NoV, 5.0);
        vec3 tint = mix(vec3(1.0), baseCol, metal);
        refl = envCol * fres * tint * (1.0 - rough * 0.7);
    }

    // ---- Emissive ----
    vec3 emissive = baseCol * lum;

    vec3 radiance = directLight + ambient + refl + emissive;
    radiance *= uExposure;

    // ---- ACES Tonemap ----
    vec3 x = radiance;
    vec3 color = clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);

    // Sqrt gamma re-encode for hdr_linear mode
    if (uHdrMode.x > 0.5) color = sqrt(color);

    fragColor = vec4(color, 1.0);
}
)";

// ---------------------------------------------------------------------------
// Volumetric Spot Cones Shader
// ---------------------------------------------------------------------------
static const char* kConeFragmentShader = R"(#version 300 es
precision highp float;

in vec2 vUV;
out vec4 fragColor;

uniform sampler2D uGbufDepth;
uniform highp sampler2DShadow uShadowSpots[8];

uniform vec3 uCamRow0, uCamRow1, uCamRow2, uCamSrc;
uniform float uSx, uOx, uSy, uOy, uDza, uDzb;
uniform float uInvSx, uInvSy;
uniform float uFarZ;
uniform int uNumLights;
uniform int uShadowsOn;
// The CPU's own cone constants (FeatureFlags cone_strength / hdr_glow_scale /
// vol_n_samples), uploaded per frame: density = cone_strength * 1e-3 scaled
// by hdr_glow_scale, nSamples = the "N x mean" calibration, fadeFloor = the
// surface-fade window floor. Same three numbers the Metal bench derives.
uniform float uConeDensity;
uniform float uConeNSamples;
uniform float uConeFadeFloor;

uniform vec4 uLightPos[32];
uniform vec4 uLightColor[32];
uniform vec4 uLightDir[32];
uniform vec4 uLightParams[32];
uniform vec4 uLightSRow0[32];
uniform vec4 uLightSRow1[32];
uniform vec4 uLightSRow2[32];
uniform vec2 uLightNearFar[32];

float sampleSpotShadow(int sIdx, vec2 uv, float refZ) {
    vec3 coord = vec3(uv, refZ);
    switch (sIdx) {
        case 0: return texture(uShadowSpots[0], coord);
        case 1: return texture(uShadowSpots[1], coord);
        case 2: return texture(uShadowSpots[2], coord);
        case 3: return texture(uShadowSpots[3], coord);
        case 4: return texture(uShadowSpots[4], coord);
        case 5: return texture(uShadowSpots[5], coord);
        case 6: return texture(uShadowSpots[6], coord);
        case 7: return texture(uShadowSpots[7], coord);
        default: return 1.0;
    }
}

float evalShadow(int sIdx, vec3 wpos, vec3 lpos, vec4 sRow0, vec3 sRow1, vec3 sRow2, vec2 nearFar) {
    if (sIdx < 0 || sIdx >= 8) return 1.0;
    vec3 rel = wpos - lpos;
    vec3 vp = vec3(
        dot(sRow0.xyz, rel),
        dot(sRow1, rel),
        dot(sRow2, rel)
    );
    if (vp.z <= nearFar.x) return 1.0;
    vec2 ndc = vp.xy * sRow0.w / vp.z;
    vec2 uv = ndc * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;
    float sn = nearFar.x, sf = nearFar.y;
    float sDza = sf / (sf - sn);
    float sDzb = (-sf * sn) / (sf - sn);
    float refZ = (sDza * vp.z + sDzb) / vp.z - 0.002;
    return sampleSpotShadow(sIdx, uv, clamp(refZ, 0.0, 1.0));
}

void main() {
    float depth = texture(uGbufDepth, vUV).r;
    float zMax = (depth > 0.0 && depth < 1.0) ? (uDzb / min(depth - uDza, -1e-7)) : uFarZ;
    float zMin = 0.05;
    if (zMax <= zMin) {
        fragColor = vec4(0.0);
        return;
    }

    vec3 V = vec3(
        (vUV.x * 2.0 - 1.0 - uOx) * uInvSx,
        (vUV.y * 2.0 - 1.0 - uOy) * uInvSy,
        1.0
    );
    float uV = dot(V, V);
    vec3 acc = vec3(0.0);

    for (int i = 0; i < uNumLights && i < 32; ++i) {
        if (uLightDir[i].w < 0.5) continue; // spotlight only
        vec3 lpos = uLightPos[i].xyz;
        float range = uLightPos[i].w;
        if (range <= 0.0) continue;

        vec3 lRel = lpos - uCamSrc;
        vec3 P = vec3(dot(uCamRow0, lRel), dot(uCamRow1, lRel), dot(uCamRow2, lRel));
        vec3 D = vec3(dot(uCamRow0, uLightDir[i].xyz), dot(uCamRow1, uLightDir[i].xyz), dot(uCamRow2, uLightDir[i].xyz));

        float cosO = uLightParams[i].y;
        float cosI = uLightParams[i].x;
        float c2 = cosO * cosO;
        float r2 = range * range;

        float DV = dot(D, V);
        float VP = dot(V, P);
        float PP = dot(P, P);
        float DP = dot(D, P);

        float sphereDisc = VP * VP - uV * (PP - r2);
        if (sphereDisc < 0.0) continue;
        float sphereSq = sqrt(sphereDisc);
        float invUV = 1.0 / uV;
        float zSphLo = (VP - sphereSq) * invUV;
        float zSphHi = (VP + sphereSq) * invUV;

        float a = DV * DV - c2 * uV;
        float b = 2.0 * (c2 * VP - DV * DP);
        float cq = DP * DP - c2 * PP;
        float zLo = zMin, zHi = zMax;

        if (a < -1e-8) {
            float disc = b * b - 4.0 * a * cq;
            if (disc < 0.0) continue;
            float sq = sqrt(disc);
            float inv2a = 1.0 / (2.0 * a);
            float r1 = (-b - sq) * inv2a, r2_ = (-b + sq) * inv2a;
            zLo = min(r1, r2_);
            zHi = max(r1, r2_);
        } else if (a > 1e-8) {
            float disc = b * b - 4.0 * a * cq;
            if (disc < 0.0) { zLo = zMin; zHi = zMax; }
            else {
                float sq = sqrt(disc);
                float inv2a = 1.0 / (2.0 * a);
                float ra = (-b - sq) * inv2a, rb = (-b + sq) * inv2a;
                float r1Q = min(ra, rb), r2Q = max(ra, rb);
                if (DV > 1e-6) { zLo = max(r2Q, zMin); zHi = zMax; }
                else if (DV < -1e-6) { zLo = zMin; zHi = min(r1Q, zMax); }
                else continue;
                if (zHi <= zLo) continue;
            }
        } else continue;

        zLo = max(zLo, zSphLo);
        zHi = min(zHi, zSphHi);
        zLo = max(zLo, zMin);
        zHi = min(zHi, zMax);
        if (zHi <= zLo) continue;

        if (abs(DV) > 1e-6) {
            float zPlane = DP / DV;
            if (DV > 0.0) zLo = max(zLo, zPlane); else zHi = min(zHi, zPlane);
            if (zHi <= zLo) continue;
        }

        // From here on: term for term the Metal bench's fs_cones
        // (GpuBench/shaders/deferred.metal), which is the CPU's analytic cone
        // (DeferredVolumetric.cpp) with one shadow tap per segment.
        // Exact distance-attenuation integral of 1/(alpha z^2 + beta z + gamma)
        // along the chord, cut into 8 segments each weighted by cone x fade x
        // shadow at its midpoint.
        float rr = 1.0 / max(range, 1e-6);
        float rr2 = rr * rr;
        float alpha = rr2 * uV;
        float beta  = -2.0 * rr2 * VP;
        float gamma = rr2 * PP + 0.05;
        float qdisc = 4.0 * alpha * gamma - beta * beta;
        if (qdisc <= 1e-20) continue;          // the CPU's degenerate reject
        float invD = inversesqrt(qdisc);

        const int SEG = 8;
        float segDz = (zHi - zLo) / float(SEG);
        // nSamp = 16 for a narrow cone is what the CPU's fade window derives
        // its step from; the floor stands in for its 12 z16 quanta.
        float fadeW = max((zHi - zLo) * (1.0 / 16.0), uConeFadeFloor);
        float invDz = 1.0 / fadeW;
        float invCIO = 1.0 / max(cosI - cosO, 1e-6);
        int sIdx = int(uLightParams[i].z);

        float sum = 0.0;
        float aPrev = atan((2.0 * alpha * zLo + beta) * invD);
        for (int s = 1; s <= SEG; ++s) {
            float zk = zLo + float(s) * segDz;
            float ak = atan((2.0 * alpha * zk + beta) * invD);
            float zm = zLo + (float(s) - 0.5) * segDz;

            vec3 W = zm * V - P;
            float cosT = dot(D, W) * inversesqrt(max(dot(W, W), 1e-12));
            float cone;
            if (cosT >= cosI) cone = 1.0;
            else {
                float t = clamp((cosT - cosO) * invCIO, 0.0, 1.0);
                cone = t * t * (3.0 - 2.0 * t);
            }
            if (cone > 0.0) {
                float sfade = clamp((zMax - zm) * invDz, 0.0, 1.0);
                float shadow = 1.0;
                if (uShadowsOn != 0 && sIdx >= 0) {
                    vec3 Q = zm * V;
                    vec3 pw = uCamSrc + uCamRow0 * Q.x + uCamRow1 * Q.y + uCamRow2 * Q.z;
                    shadow = evalShadow(sIdx, pw, lpos, uLightSRow0[i], uLightSRow1[i].xyz, uLightSRow2[i].xyz, uLightNearFar[i]);
                }
                sum += (ak - aPrev) * cone * sfade * shadow;
            }
            aPrev = ak;
        }
        float integral = 2.0 * invD * sum;

        // Midpoint near-edge softness (the ray-march's (1 - rr d)^2), once at
        // the chord midpoint, or the halo ends at a hard sphere boundary.
        vec3 Wm = 0.5 * (zLo + zHi) * V - P;
        float soft = max(0.0, 1.0 - rr * length(Wm));
        soft *= soft;

        // "N x mean": the brightness calibration cone_strength is tuned to.
        float vAcc = (integral / max(zHi - zLo, 1e-6)) * uConeNSamples * soft;
        acc += (vAcc * uConeDensity) * uLightColor[i].rgb;
    }

    fragColor = vec4(acc, 1.0);
}
)";

// ---------------------------------------------------------------------------
// Transparent (XPAR) Forward Shader
// ---------------------------------------------------------------------------
static const char* kXparVertexShader = R"(#version 300 es
precision highp float;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

uniform vec3 uCamRow0, uCamRow1, uCamRow2, uCamSrc;
uniform float uSx, uOx, uSy, uOy, uDza, uDzb;
uniform vec3 uRotRow0, uRotRow1, uRotRow2, uObjPos;

out vec2 vUV;
out vec3 vViewPos;
out vec3 vWorldPos;
out vec3 vViewNormal;

void main() {
    vec3 wp = vec3(dot(uRotRow0, aPos), dot(uRotRow1, aPos), dot(uRotRow2, aPos)) + uObjPos;
    vec3 rel = wp - uCamSrc;
    vec3 vp = vec3(dot(uCamRow0, rel), dot(uCamRow1, rel), dot(uCamRow2, rel));

    gl_Position.x = uSx * vp.x + uOx * vp.z;
    gl_Position.y = uSy * vp.y + uOy * vp.z;
    gl_Position.z = (2.0 * uDza - 1.0) * vp.z + (2.0 * uDzb);
    gl_Position.w = vp.z;

    vUV = aUV;
    vViewPos = vp;
    vWorldPos = wp;

    vec3 wn = vec3(dot(uRotRow0, aNormal), dot(uRotRow1, aNormal), dot(uRotRow2, aNormal));
    vViewNormal = vec3(dot(uCamRow0, wn), dot(uCamRow1, wn), dot(uCamRow2, wn));
}
)";

static const char* kXparFragmentShader = R"(#version 300 es
precision highp float;

in vec2 vUV;
in vec3 vViewPos;
in vec3 vWorldPos;
in vec3 vViewNormal;

uniform vec3 uCamRow0, uCamRow1, uCamRow2, uCamSrc;
uniform vec4 uBaseColor; // rgb = base color, w = hasTexture
uniform vec4 uMatParams; // x = diffuse, y = spec, z = rough, w = luminosity
uniform vec4 uMisc2;     // x = envProbeID, y = reflection, z = xparAlpha, w = isAdditive
uniform vec3 uAmbient;

uniform int uNumLights;
uniform vec4 uLightPos[32];
uniform vec4 uLightColor[32];
uniform vec4 uLightDir[32];
uniform vec4 uLightParams[32];

uniform sampler2D uAlbedoTex;

out vec4 fragColor;

void main() {
    vec4 tex = vec4(uBaseColor.rgb, 1.0);
    if (uBaseColor.w > 0.5) {
        tex = texture(uAlbedoTex, vUV);
    }

    vec3 N = normalize(vViewNormal);
    float diffuseK = uMatParams.x;
    float lum = uMatParams.w;

    vec3 l = vec3(lum) + diffuseK * uAmbient;

    for (int i = 0; i < uNumLights && i < 32; ++i) {
        vec3 lpos = uLightPos[i].xyz;
        float range = uLightPos[i].w;
        vec3 toL = lpos - vWorldPos;
        float d2 = dot(toL, toL);
        if (range <= 0.0 || d2 >= range * range) continue;

        float d = sqrt(d2);
        vec3 Ldir = toL / max(d, 1e-6);
        vec3 toLView = vec3(dot(uCamRow0, toL), dot(uCamRow1, toL), dot(uCamRow2, toL));
        float NoL = clamp(dot(N, normalize(toLView)), 0.0, 1.0);
        if (NoL <= 0.0) continue;

        float atten = clamp(1.0 - d / range, 0.0, 1.0);
        if (uLightDir[i].w > 0.5) {
            float cosTheta = dot(uLightDir[i].xyz, -Ldir);
            float cosOuter = uLightParams[i].y;
            float cosInner = uLightParams[i].x;
            if (cosTheta <= cosOuter) continue;
            if (cosTheta < cosInner) {
                float tt = (cosTheta - cosOuter) / max(cosInner - cosOuter, 1e-6);
                atten *= tt * tt * (3.0 - 2.0 * tt);
            }
        }
        l += (NoL * atten * diffuseK) * uLightColor[i].rgb;
    }

    vec3 texLin = tex.rgb * tex.rgb;
    vec3 lit = texLin * l;

    vec3 x = max(lit, 0.0);
    vec3 col = clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
    col = sqrt(col);

    float alpha = (uMisc2.w > 0.5) ? 1.0 : max(0.01, uMisc2.z);
    fragColor = vec4(col, alpha);
}
)";

static const char* kFlareVertexShader = R"(#version 300 es
precision highp float;

uniform vec3 uCamRow0, uCamRow1, uCamRow2, uCamSrc;
uniform float uSx, uOx, uSy, uOy, uDza, uDzb;
uniform vec4 uFlareCenter; // xyz = world pos, w = world half-extent

out vec2 vUV;

void main() {
    vec2 c[6] = vec2[](
        vec2(-1.0, -1.0), vec2( 1.0, -1.0), vec2(-1.0,  1.0),
        vec2( 1.0, -1.0), vec2( 1.0,  1.0), vec2(-1.0,  1.0)
    );
    vec2 o = c[gl_VertexID];
    vUV = o * 0.5 + 0.5;

    vec3 rel = uFlareCenter.xyz - uCamSrc;
    vec3 vp = vec3(dot(uCamRow0, rel), dot(uCamRow1, rel), dot(uCamRow2, rel));

    vp.xy += o * uFlareCenter.w;

    gl_Position.x = uSx * vp.x + uOx * vp.z;
    gl_Position.y = uSy * vp.y + uOy * vp.z;
    gl_Position.z = (2.0 * uDza - 1.0) * vp.z + (2.0 * uDzb);
    gl_Position.w = vp.z;
}
)";

static const char* kFlareFragmentShader = R"(#version 300 es
precision highp float;

in vec2 vUV;
out vec4 fragColor;

uniform sampler2D uFlareTex;
uniform vec3 uFlareGain; // rgb = color * gain

void main() {
    vec4 tex = texture(uFlareTex, vUV);
    fragColor = vec4(tex.rgb * uFlareGain, tex.a);
}
)";

static GLuint CompileShader(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[1024];
        glGetShaderInfoLog(sh, sizeof(buf), nullptr, buf);
        std::fprintf(stderr, "[GPU-WEB] Shader compilation failed:\n%s\n", buf);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint LinkProgram(const char* vsSrc, const char* fsSrc) {
    GLuint vs = CompileShader(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fsSrc);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        char buf[1024];
        glGetProgramInfoLog(prog, sizeof(buf), nullptr, buf);
        std::fprintf(stderr, "[GPU-WEB] Program linking failed:\n%s\n", buf);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static void ResizeGBuffer(int w, int h) {
    if (s_gbufWidth == w && s_gbufHeight == h && s_gbufFbo != 0) return;
    s_gbufWidth = w;
    s_gbufHeight = h;

    if (s_gbufFbo != 0) {
        glDeleteFramebuffers(1, &s_gbufFbo);
        glDeleteTextures(1, &s_gbufAlbedo);
        glDeleteTextures(1, &s_gbufNormal);
        glDeleteTextures(1, &s_gbufMaterial);
        glDeleteTextures(1, &s_gbufMirror);
        glDeleteTextures(1, &s_gbufDepth);
        s_gbufFbo = 0;
    }

    glGenFramebuffers(1, &s_gbufFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_gbufFbo);

    auto mkTex = [&](GLenum internalFmt, GLenum fmt, GLenum type) -> GLuint {
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, w, h, 0, fmt, type, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return tex;
    };

    s_gbufAlbedo   = mkTex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    s_gbufNormal   = mkTex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    s_gbufMaterial = mkTex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    s_gbufMirror   = mkTex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    s_gbufDepth    = mkTex(GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_gbufAlbedo, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, s_gbufNormal, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, s_gbufMaterial, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, s_gbufMirror, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,  GL_TEXTURE_2D, s_gbufDepth, 0);

    GLenum bufs[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
    glDrawBuffers(4, bufs);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[GPU-WEB] G-Buffer FBO incomplete: 0x%X\n", (unsigned)status);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void InitShadowMaps() {
    if (s_shadowFbo != 0) return;
    glGenFramebuffers(1, &s_shadowFbo);
    for (int i = 0; i < kMaxShadowSpots; ++i) {
        glGenTextures(1, &s_shadowSpots[i]);
        glBindTexture(GL_TEXTURE_2D, s_shadowSpots[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT16, kShadowDim, kShadowDim, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    }
}

#endif // __EMSCRIPTEN__

} // namespace

bool GpuWeb_Init() {
#ifdef __EMSCRIPTEN__
    if (s_initialized) return true;

    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_get_current_context();
    if (ctx <= 0) return false;
    emscripten_webgl_make_context_current(ctx);

    s_gbufProg    = LinkProgram(kGbufVertexShader, kGbufFragmentShader);
    s_shadowProg  = LinkProgram(kShadowVertexShader, kShadowFragmentShader);
    s_resolveProg = LinkProgram(kQuadVertexShader, kResolveFragmentShader);
    s_coneProg    = LinkProgram(kQuadVertexShader, kConeFragmentShader);
    s_xparProg    = LinkProgram(kXparVertexShader, kXparFragmentShader);
    s_flareProg   = LinkProgram(kFlareVertexShader, kFlareFragmentShader);

    if (!s_gbufProg || !s_shadowProg || !s_resolveProg || !s_coneProg || !s_xparProg || !s_flareProg) {
        std::fprintf(stderr, "[GPU-WEB] Fatal: Failed to build deferred shader pipeline\n");
        return false;
    }

    // 1. G-Buffer Uniforms
    glUseProgram(s_gbufProg);
    loc_g_uCamRow0 = glGetUniformLocation(s_gbufProg, "uCamRow0");
    loc_g_uCamRow1 = glGetUniformLocation(s_gbufProg, "uCamRow1");
    loc_g_uCamRow2 = glGetUniformLocation(s_gbufProg, "uCamRow2");
    loc_g_uCamSrc  = glGetUniformLocation(s_gbufProg, "uCamSrc");
    loc_g_uSx      = glGetUniformLocation(s_gbufProg, "uSx");
    loc_g_uOx      = glGetUniformLocation(s_gbufProg, "uOx");
    loc_g_uSy      = glGetUniformLocation(s_gbufProg, "uSy");
    loc_g_uOy      = glGetUniformLocation(s_gbufProg, "uOy");
    loc_g_uDza     = glGetUniformLocation(s_gbufProg, "uDza");
    loc_g_uDzb     = glGetUniformLocation(s_gbufProg, "uDzb");
    loc_g_uRotRow0 = glGetUniformLocation(s_gbufProg, "uRotRow0");
    loc_g_uRotRow1 = glGetUniformLocation(s_gbufProg, "uRotRow1");
    loc_g_uRotRow2 = glGetUniformLocation(s_gbufProg, "uRotRow2");
    loc_g_uObjPos  = glGetUniformLocation(s_gbufProg, "uObjPos");
    loc_g_uBaseColor = glGetUniformLocation(s_gbufProg, "uBaseColor");
    loc_g_uMatParams = glGetUniformLocation(s_gbufProg, "uMatParams");
    loc_g_uMapFlags  = glGetUniformLocation(s_gbufProg, "uMapFlags");
    loc_g_uMisc      = glGetUniformLocation(s_gbufProg, "uMisc");
    loc_g_uMisc2     = glGetUniformLocation(s_gbufProg, "uMisc2");

    glUniform1i(glGetUniformLocation(s_gbufProg, "uAlbedoTex"), 0);
    glUniform1i(glGetUniformLocation(s_gbufProg, "uNormalTex"), 1);
    glUniform1i(glGetUniformLocation(s_gbufProg, "uRoughTex"),  2);
    glUniform1i(glGetUniformLocation(s_gbufProg, "uAoTex"),     3);
    glUniform1i(glGetUniformLocation(s_gbufProg, "uMetalTex"),  4);

    // 2. Shadow Uniforms
    glUseProgram(s_shadowProg);
    loc_sh_uRotRow0   = glGetUniformLocation(s_shadowProg, "uRotRow0");
    loc_sh_uRotRow1   = glGetUniformLocation(s_shadowProg, "uRotRow1");
    loc_sh_uRotRow2   = glGetUniformLocation(s_shadowProg, "uRotRow2");
    loc_sh_uObjPos    = glGetUniformLocation(s_shadowProg, "uObjPos");
    loc_sh_uLightRow0 = glGetUniformLocation(s_shadowProg, "uLightRow0");
    loc_sh_uLightRow1 = glGetUniformLocation(s_shadowProg, "uLightRow1");
    loc_sh_uLightRow2 = glGetUniformLocation(s_shadowProg, "uLightRow2");
    loc_sh_uLightPos  = glGetUniformLocation(s_shadowProg, "uLightPos");
    loc_sh_uProjData  = glGetUniformLocation(s_shadowProg, "uProjData");

    // 3. Resolve Uniforms
    glUseProgram(s_resolveProg);
    loc_r_uCamRow0 = glGetUniformLocation(s_resolveProg, "uCamRow0");
    loc_r_uCamRow1 = glGetUniformLocation(s_resolveProg, "uCamRow1");
    loc_r_uCamRow2 = glGetUniformLocation(s_resolveProg, "uCamRow2");
    loc_r_uCamSrc  = glGetUniformLocation(s_resolveProg, "uCamSrc");
    loc_r_uSx      = glGetUniformLocation(s_resolveProg, "uSx");
    loc_r_uOx      = glGetUniformLocation(s_resolveProg, "uOx");
    loc_r_uSy      = glGetUniformLocation(s_resolveProg, "uSy");
    loc_r_uOy      = glGetUniformLocation(s_resolveProg, "uOy");
    loc_r_uDza     = glGetUniformLocation(s_resolveProg, "uDza");
    loc_r_uDzb     = glGetUniformLocation(s_resolveProg, "uDzb");
    loc_r_uInvSx   = glGetUniformLocation(s_resolveProg, "uInvSx");
    loc_r_uInvSy   = glGetUniformLocation(s_resolveProg, "uInvSy");
    loc_r_uNearZ   = glGetUniformLocation(s_resolveProg, "uNearZ");
    loc_r_uFarZ    = glGetUniformLocation(s_resolveProg, "uFarZ");
    loc_r_uExposure = glGetUniformLocation(s_resolveProg, "uExposure");
    loc_r_uDiffuseFactor  = glGetUniformLocation(s_resolveProg, "uDiffuseFactor");
    loc_r_uSpecularFactor = glGetUniformLocation(s_resolveProg, "uSpecularFactor");
    loc_r_uAmbientFactor  = glGetUniformLocation(s_resolveProg, "uAmbientFactor");
    loc_r_uSkyZenith      = glGetUniformLocation(s_resolveProg, "uSkyZenith");
    loc_r_uSkyNadir       = glGetUniformLocation(s_resolveProg, "uSkyNadir");
    loc_r_uFlatAmbient    = glGetUniformLocation(s_resolveProg, "uFlatAmbient");
    loc_r_uHdrMode        = glGetUniformLocation(s_resolveProg, "uHdrMode");
    loc_r_uNumLights      = glGetUniformLocation(s_resolveProg, "uNumLights");
    loc_r_uShadowsOn      = glGetUniformLocation(s_resolveProg, "uShadowsOn");
    loc_r_uHasEnvTex      = glGetUniformLocation(s_resolveProg, "uHasEnvTex");

    glUniform1i(glGetUniformLocation(s_resolveProg, "uGbufAlbedo"),   0);
    glUniform1i(glGetUniformLocation(s_resolveProg, "uGbufNormal"),   1);
    glUniform1i(glGetUniformLocation(s_resolveProg, "uGbufMaterial"), 2);
    glUniform1i(glGetUniformLocation(s_resolveProg, "uGbufMirror"),   3);
    glUniform1i(glGetUniformLocation(s_resolveProg, "uGbufDepth"),    4);
    glUniform1i(glGetUniformLocation(s_resolveProg, "uEnvTex"),       13);

    for (int i = 0; i < kMaxShadowSpots; ++i) {
        char name[64];
        std::snprintf(name, sizeof(name), "uShadowSpots[%d]", i);
        glUniform1i(glGetUniformLocation(s_resolveProg, name), 5 + i);
    }

    auto bindLightLocs = [](GLuint prog, LightResolveLoc locs[kMaxLights]) {
        for (int i = 0; i < kMaxLights; ++i) {
            char name[64];
            std::snprintf(name, sizeof(name), "uLightPos[%d]", i);
            locs[i].pos = glGetUniformLocation(prog, name);
            std::snprintf(name, sizeof(name), "uLightColor[%d]", i);
            locs[i].color = glGetUniformLocation(prog, name);
            std::snprintf(name, sizeof(name), "uLightDir[%d]", i);
            locs[i].dir = glGetUniformLocation(prog, name);
            std::snprintf(name, sizeof(name), "uLightParams[%d]", i);
            locs[i].params = glGetUniformLocation(prog, name);
            std::snprintf(name, sizeof(name), "uLightSRow0[%d]", i);
            locs[i].sRow0 = glGetUniformLocation(prog, name);
            std::snprintf(name, sizeof(name), "uLightSRow1[%d]", i);
            locs[i].sRow1 = glGetUniformLocation(prog, name);
            std::snprintf(name, sizeof(name), "uLightSRow2[%d]", i);
            locs[i].sRow2 = glGetUniformLocation(prog, name);
            std::snprintf(name, sizeof(name), "uLightNearFar[%d]", i);
            locs[i].nearFar = glGetUniformLocation(prog, name);
        }
    };
    bindLightLocs(s_resolveProg, s_resolveLightLocs);

    // 4. Cone Uniforms
    glUseProgram(s_coneProg);
    loc_c_uCamRow0 = glGetUniformLocation(s_coneProg, "uCamRow0");
    loc_c_uCamRow1 = glGetUniformLocation(s_coneProg, "uCamRow1");
    loc_c_uCamRow2 = glGetUniformLocation(s_coneProg, "uCamRow2");
    loc_c_uCamSrc  = glGetUniformLocation(s_coneProg, "uCamSrc");
    loc_c_uSx      = glGetUniformLocation(s_coneProg, "uSx");
    loc_c_uOx      = glGetUniformLocation(s_coneProg, "uOx");
    loc_c_uSy      = glGetUniformLocation(s_coneProg, "uSy");
    loc_c_uOy      = glGetUniformLocation(s_coneProg, "uOy");
    loc_c_uDza     = glGetUniformLocation(s_coneProg, "uDza");
    loc_c_uDzb     = glGetUniformLocation(s_coneProg, "uDzb");
    loc_c_uInvSx   = glGetUniformLocation(s_coneProg, "uInvSx");
    loc_c_uInvSy   = glGetUniformLocation(s_coneProg, "uInvSy");
    loc_c_uFarZ    = glGetUniformLocation(s_coneProg, "uFarZ");
    loc_c_uNumLights = glGetUniformLocation(s_coneProg, "uNumLights");
    loc_c_uShadowsOn = glGetUniformLocation(s_coneProg, "uShadowsOn");
    loc_c_uConeDensity   = glGetUniformLocation(s_coneProg, "uConeDensity");
    loc_c_uConeNSamples  = glGetUniformLocation(s_coneProg, "uConeNSamples");
    loc_c_uConeFadeFloor = glGetUniformLocation(s_coneProg, "uConeFadeFloor");

    glUniform1i(glGetUniformLocation(s_coneProg, "uGbufDepth"), 4);
    for (int i = 0; i < kMaxShadowSpots; ++i) {
        char name[64];
        std::snprintf(name, sizeof(name), "uShadowSpots[%d]", i);
        glUniform1i(glGetUniformLocation(s_coneProg, name), 5 + i);
    }
    bindLightLocs(s_coneProg, s_coneLightLocs);

    // 5. XPAR Uniforms
    glUseProgram(s_xparProg);
    loc_x_uCamRow0 = glGetUniformLocation(s_xparProg, "uCamRow0");
    loc_x_uCamRow1 = glGetUniformLocation(s_xparProg, "uCamRow1");
    loc_x_uCamRow2 = glGetUniformLocation(s_xparProg, "uCamRow2");
    loc_x_uCamSrc  = glGetUniformLocation(s_xparProg, "uCamSrc");
    loc_x_uSx      = glGetUniformLocation(s_xparProg, "uSx");
    loc_x_uOx      = glGetUniformLocation(s_xparProg, "uOx");
    loc_x_uSy      = glGetUniformLocation(s_xparProg, "uSy");
    loc_x_uOy      = glGetUniformLocation(s_xparProg, "uOy");
    loc_x_uDza     = glGetUniformLocation(s_xparProg, "uDza");
    loc_x_uDzb     = glGetUniformLocation(s_xparProg, "uDzb");
    loc_x_uRotRow0 = glGetUniformLocation(s_xparProg, "uRotRow0");
    loc_x_uRotRow1 = glGetUniformLocation(s_xparProg, "uRotRow1");
    loc_x_uRotRow2 = glGetUniformLocation(s_xparProg, "uRotRow2");
    loc_x_uObjPos  = glGetUniformLocation(s_xparProg, "uObjPos");
    loc_x_uBaseColor = glGetUniformLocation(s_xparProg, "uBaseColor");
    loc_x_uMatParams = glGetUniformLocation(s_xparProg, "uMatParams");
    loc_x_uMisc2     = glGetUniformLocation(s_xparProg, "uMisc2");
    loc_x_uAmbient   = glGetUniformLocation(s_xparProg, "uAmbient");
    loc_x_uNumLights = glGetUniformLocation(s_xparProg, "uNumLights");

    glUniform1i(glGetUniformLocation(s_xparProg, "uAlbedoTex"), 0);
    bindLightLocs(s_xparProg, s_xparLightLocs);

    // 6. Flare Uniforms
    glUseProgram(s_flareProg);
    loc_f_uCamRow0 = glGetUniformLocation(s_flareProg, "uCamRow0");
    loc_f_uCamRow1 = glGetUniformLocation(s_flareProg, "uCamRow1");
    loc_f_uCamRow2 = glGetUniformLocation(s_flareProg, "uCamRow2");
    loc_f_uCamSrc  = glGetUniformLocation(s_flareProg, "uCamSrc");
    loc_f_uSx      = glGetUniformLocation(s_flareProg, "uSx");
    loc_f_uOx      = glGetUniformLocation(s_flareProg, "uOx");
    loc_f_uSy      = glGetUniformLocation(s_flareProg, "uSy");
    loc_f_uOy      = glGetUniformLocation(s_flareProg, "uOy");
    loc_f_uDza     = glGetUniformLocation(s_flareProg, "uDza");
    loc_f_uDzb     = glGetUniformLocation(s_flareProg, "uDzb");
    loc_f_uFlareCenter = glGetUniformLocation(s_flareProg, "uFlareCenter");
    loc_f_uFlareGain   = glGetUniformLocation(s_flareProg, "uFlareGain");
    glUniform1i(glGetUniformLocation(s_flareProg, "uFlareTex"), 0);

    // 7. Buffers
    glGenVertexArrays(1, &s_sceneVao);
    glGenBuffers(1, &s_sceneVbo);

    glGenVertexArrays(1, &s_quadVao);
    glGenVertexArrays(1, &s_flareVao);

    const uint32_t whitePix = 0xFFFFFFFF;
    glGenTextures(1, &s_whiteTex);
    glBindTexture(GL_TEXTURE_2D, s_whiteTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &whitePix);

    const uint32_t flatNormPix = 0xFFFF8080;
    glGenTextures(1, &s_flatNormalTex);
    glBindTexture(GL_TEXTURE_2D, s_flatNormalTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &flatNormPix);

    InitShadowMaps();

    s_initialized = true;
    std::fprintf(stderr, "[GPU-WEB] Deferred PBR Full Hardware Pipeline Initialized Successfully!\n");
    return true;
#else
    return false;
#endif
}

bool GpuWeb_IsEnabled() {
    return s_enabled;
}

void GpuWeb_SetEnabled(bool enabled) {
    s_enabled = enabled;
}

bool GpuWeb_LoadScene(const char* sceneName, float demoT) {
#ifdef __EMSCRIPTEN__
    if (!GpuWeb_Init()) return false;

    if (s_loadedSceneName == sceneName && !s_scene.batches.empty()) {
        return true;
    }

    static std::string s_fldPath;
    s_fldPath = std::string("SCENES/") + sceneName + ".FLD";
    for (char &c : s_fldPath) c = (char)std::toupper(c);

    s_opt.fldPath = s_fldPath.c_str();
    s_opt.demoT = (int)demoT;
    s_opt.verbose = true;

    std::fprintf(stderr, "[GPU-WEB] Ingesting scene '%s' from %s for Deferred PBR...\n", sceneName, s_fldPath.c_str());
    if (!gpubench::Load(s_scene, s_opt)) {
        std::fprintf(stderr, "[GPU-WEB] Failed to ingest scene '%s'\n", sceneName);
        return false;
    }
    s_loadedSceneName = sceneName;

    // Upload Vertex Buffer
    glBindVertexArray(s_sceneVao);
    glBindBuffer(GL_ARRAY_BUFFER, s_sceneVbo);
    glBufferData(GL_ARRAY_BUFFER, s_scene.verts.size() * sizeof(gpubench::Vertex), s_scene.verts.data(), GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(gpubench::Vertex), (void*)offsetof(gpubench::Vertex, px));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(gpubench::Vertex), (void*)offsetof(gpubench::Vertex, nx));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(gpubench::Vertex), (void*)offsetof(gpubench::Vertex, u));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(gpubench::Vertex), (void*)offsetof(gpubench::Vertex, tx));
    glBindVertexArray(0);

    // Upload Textures
    if (!s_textures.empty()) {
        glDeleteTextures((GLsizei)s_textures.size(), s_textures.data());
        s_textures.clear();
    }
    s_textures.resize(s_scene.textures.size(), 0);
    if (!s_scene.textures.empty()) {
        glGenTextures((GLsizei)s_scene.textures.size(), s_textures.data());
        for (size_t t = 0; t < s_scene.textures.size(); ++t) {
            const auto &tex = s_scene.textures[t];
            glBindTexture(GL_TEXTURE_2D, s_textures[t]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex.w, tex.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex.rgba.data());
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            if (tex.fileName.find("P_TEXT") != std::string::npos) {
                s_ptextTex = s_textures[t];
            }
        }
    }

    if (s_scene.envTexIndex >= 0 && size_t(s_scene.envTexIndex) < s_textures.size()) {
        s_envTex = s_textures[s_scene.envTexIndex];
    } else {
        s_envTex = s_whiteTex;
    }

    s_numShadowSpots = 0;
    for (auto &L : s_scene.lights) {
        if (L.isSpot && L.castsShadow && s_numShadowSpots < kMaxShadowSpots) {
            L.shadowSlot = s_numShadowSpots++;
        }
    }

    std::fprintf(stderr, "[GPU-WEB] Scene '%s' ingested: %zu verts, %zu batches, %zu textures, %zu lights, %d shadow spots, envTex=%d\n",
                 sceneName, s_scene.verts.size(), s_scene.batches.size(), s_scene.textures.size(), s_scene.lights.size(), s_numShadowSpots, s_scene.envTexIndex);
    return true;
#else
    return false;
#endif
}

extern "C" void *Greets_GetDynamicScreenLinearBuffer();
extern "C" void Greets_RenderDynamicScreen();

void GpuWeb_RenderFrame(int width, int height, float demoT) {
#ifdef __EMSCRIPTEN__
    if (!s_initialized || s_scene.batches.empty()) return;

    gpubench::Reanimate(s_scene, s_opt, demoT);

    int nextShadowIdx = 0;
    for (auto &L : s_scene.lights) {
        if (L.isSpot && L.castsShadow && nextShadowIdx < kMaxShadowSpots) {
            L.shadowSlot = nextShadowIdx++;
        } else {
            L.shadowSlot = -1;
        }
    }

    if (::g_editorPlaying) {
        Greets_RenderDynamicScreen();
    }
    if (s_ptextTex != 0) {
        void *ptextLinear = Greets_GetDynamicScreenLinearBuffer();
        if (ptextLinear) {
            glBindTexture(GL_TEXTURE_2D, s_ptextTex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, ptextLinear);
            glGenerateMipmap(GL_TEXTURE_2D);
        }
    }

    if (View && !::g_editorPlaying) {
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                s_scene.camera.rot[r][c] = View->Mat[r][c];
            }
        }
        s_scene.camera.src[0] = View->ISource.x;
        s_scene.camera.src[1] = View->ISource.y;
        s_scene.camera.src[2] = View->ISource.z;
        s_scene.camera.perspX = View->PerspX;
        s_scene.camera.perspY = View->PerspY;
        s_scene.camera.cntrEX = CntrEX;
        s_scene.camera.cntrEY = CntrEY;
    }

    ResizeGBuffer(width, height);

    static bool s_envUploaded = false;
    if (!s_envUploaded && fds::GreetsDiscoPanoTexture() && fds::GreetsDiscoPanoTexture()->Data) {
        gpubench::TextureImage envImg;
        if (gpubench::ExpandToRGBA(fds::GreetsDiscoPanoTexture(), envImg)) {
            if (s_envTex == 0 || s_envTex == s_whiteTex) glGenTextures(1, &s_envTex);
            glBindTexture(GL_TEXTURE_2D, s_envTex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, envImg.w, envImg.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, envImg.rgba.data());
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            s_envUploaded = true;
            std::fprintf(stderr, "[GPU-WEB] Uploaded disco panorama reflection map %dx%d\n", envImg.w, envImg.h);
        }
    }

    float refW = (View && !::g_editorPlaying) ? float(XRes > 0 ? XRes : width) : float(s_scene.xres > 0 ? s_scene.xres : width);
    float refH = (View && !::g_editorPlaying) ? float(YRes > 0 ? YRes : height) : float(s_scene.yres > 0 ? s_scene.yres : height);

    float nearZ = std::max(s_scene.camera.nearZ, 0.05f);
    float farZ  = std::max(s_scene.camera.farZ,  150.0f);
    float sx = 2.0f * s_scene.camera.perspX / refW;
    float ox = 2.0f * s_scene.camera.cntrEX / refW - 1.0f;
    float sy = 2.0f * s_scene.camera.perspY / refH;
    float oy = 1.0f - 2.0f * s_scene.camera.cntrEY / refH;
    float dza = farZ / (farZ - nearZ);
    float dzb = (-farZ * nearZ) / (farZ - nearZ);
    float invSx = 1.0f / (sx != 0.0f ? sx : 1e-4f);
    float invSy = 1.0f / (sy != 0.0f ? sy : 1e-4f);

    auto uploadLightUniforms = [](const LightResolveLoc locs[kMaxLights], int &numActive) {
        numActive = 0;
        for (size_t i = 0; i < s_scene.lights.size() && numActive < kMaxLights; ++i) {
            const auto &L = s_scene.lights[i];
            if (!std::isfinite(L.pos[0]) || L.range <= 0.0f) continue;
            int idx = numActive++;

            glUniform4f(locs[idx].pos, L.pos[0], L.pos[1], L.pos[2], L.range);
            float c01[3] = { L.color[0]/255.0f, L.color[1]/255.0f, L.color[2]/255.0f };
            glUniform4f(locs[idx].color, c01[0] * L.intensity, c01[1] * L.intensity, c01[2] * L.intensity, 1.0f / L.range);
            glUniform4f(locs[idx].dir, L.dir[0], L.dir[1], L.dir[2], L.isSpot ? 1.0f : 0.0f);
            glUniform4f(locs[idx].params, L.cosInner, L.cosOuter, float(L.shadowSlot), 0.0f);
            glUniform4f(locs[idx].sRow0, L.shadowRot[0][0], L.shadowRot[0][1], L.shadowRot[0][2], 1.0f / std::max(L.shadowTanHalfFov, 1e-4f));
            glUniform4f(locs[idx].sRow1, L.shadowRot[1][0], L.shadowRot[1][1], L.shadowRot[1][2], 0.0f);
            glUniform4f(locs[idx].sRow2, L.shadowRot[2][0], L.shadowRot[2][1], L.shadowRot[2][2], 0.0f);
            glUniform2f(locs[idx].nearFar, 0.05f, L.range);
        }
    };

    // -----------------------------------------------------------------------
    // PASS 1: SPOTLIGHT SHADOW DEPTH MAPS
    // -----------------------------------------------------------------------
    glBindFramebuffer(GL_FRAMEBUFFER, s_shadowFbo);
    glUseProgram(s_shadowProg);
    glBindVertexArray(s_sceneVao);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);

    for (const auto &L : s_scene.lights) {
        if (!L.isSpot || !L.castsShadow || L.shadowSlot < 0 || L.shadowSlot >= kMaxShadowSpots) continue;
        int sIdx = L.shadowSlot;

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, s_shadowSpots[sIdx], 0);
        glViewport(0, 0, kShadowDim, kShadowDim);
        glClear(GL_DEPTH_BUFFER_BIT);

        float sn = 0.05f;
        float sf = L.range;
        float sDza = sf / (sf - sn);
        float sDzb = (-sf * sn) / (sf - sn);
        float sScale = 1.0f / std::max(L.shadowTanHalfFov, 1e-4f);

        glUniform3f(loc_sh_uLightRow0, L.shadowRot[0][0], L.shadowRot[0][1], L.shadowRot[0][2]);
        glUniform3f(loc_sh_uLightRow1, L.shadowRot[1][0], L.shadowRot[1][1], L.shadowRot[1][2]);
        glUniform3f(loc_sh_uLightRow2, L.shadowRot[2][0], L.shadowRot[2][1], L.shadowRot[2][2]);
        glUniform3f(loc_sh_uLightPos,  L.pos[0], L.pos[1], L.pos[2]);
        glUniform3f(loc_sh_uProjData,  sDza, sDzb, sScale);

        for (const auto &b : s_scene.batches) {
            if (!b.castsShadow || b.transparent || b.additive) continue;

            glUniform3f(loc_sh_uRotRow0, b.rot[0][0], b.rot[0][1], b.rot[0][2]);
            glUniform3f(loc_sh_uRotRow1, b.rot[1][0], b.rot[1][1], b.rot[1][2]);
            glUniform3f(loc_sh_uRotRow2, b.rot[2][0], b.rot[2][1], b.rot[2][2]);
            glUniform3f(loc_sh_uObjPos,  b.pos[0], b.pos[1], b.pos[2]);

            glDrawArrays(GL_TRIANGLES, b.firstVertex, b.vertexCount);
        }
    }

    // -----------------------------------------------------------------------
    // PASS 2: G-BUFFER RASTERIZATION
    // -----------------------------------------------------------------------
    glBindFramebuffer(GL_FRAMEBUFFER, s_gbufFbo);
    GLenum bufs[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
    glDrawBuffers(4, bufs);
    glViewport(0, 0, width, height);

    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(s_gbufProg);
    glBindVertexArray(s_sceneVao);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glDisable(GL_BLEND);

    glUniform3f(loc_g_uCamRow0, s_scene.camera.rot[0][0], s_scene.camera.rot[0][1], s_scene.camera.rot[0][2]);
    glUniform3f(loc_g_uCamRow1, s_scene.camera.rot[1][0], s_scene.camera.rot[1][1], s_scene.camera.rot[1][2]);
    glUniform3f(loc_g_uCamRow2, s_scene.camera.rot[2][0], s_scene.camera.rot[2][1], s_scene.camera.rot[2][2]);
    glUniform3f(loc_g_uCamSrc,  s_scene.camera.src[0], s_scene.camera.src[1], s_scene.camera.src[2]);
    glUniform1f(loc_g_uSx, sx);
    glUniform1f(loc_g_uOx, ox);
    glUniform1f(loc_g_uSy, sy);
    glUniform1f(loc_g_uOy, oy);
    glUniform1f(loc_g_uDza, dza);
    glUniform1f(loc_g_uDzb, dzb);

    for (const auto &b : s_scene.batches) {
        if (b.transparent || b.additive) continue;

        glUniform3f(loc_g_uRotRow0, b.rot[0][0], b.rot[0][1], b.rot[0][2]);
        glUniform3f(loc_g_uRotRow1, b.rot[1][0], b.rot[1][1], b.rot[1][2]);
        glUniform3f(loc_g_uRotRow2, b.rot[2][0], b.rot[2][1], b.rot[2][2]);
        glUniform3f(loc_g_uObjPos,  b.pos[0], b.pos[1], b.pos[2]);

        glUniform4f(loc_g_uBaseColor, b.baseColor[0], b.baseColor[1], b.baseColor[2], b.textureIndex >= 0 ? 1.0f : 0.0f);
        float ggxRough = std::sqrt(2.0f / float(b.glossiness + 2));
        glUniform4f(loc_g_uMatParams, b.diffuse, b.specular, ggxRough, b.luminosity);
        glUniform4f(loc_g_uMapFlags,  b.normalTexIndex >= 0 ? 1.0f : 0.0f,
                                      b.roughTexIndex >= 0 ? 1.0f : 0.0f,
                                      b.aoInAlpha ? 1.0f : 0.0f,
                                      b.parallaxScale);
        glUniform4f(loc_g_uMisc,      float(b.mirrorIndex),
                                      b.aoTexIndex >= 0 ? 1.0f : 0.0f,
                                      b.metalTexIndex >= 0 ? 1.0f : 0.0f,
                                      b.aoStrength * 2.0f);
        glUniform4f(loc_g_uMisc2,     float(b.envProbe), b.reflection, b.xparBlendAlpha, 0.5f);

        if (b.twoSided) glDisable(GL_CULL_FACE); else glEnable(GL_CULL_FACE);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (b.textureIndex >= 0 && size_t(b.textureIndex) < s_textures.size()) ? s_textures[b.textureIndex] : s_whiteTex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, (b.normalTexIndex >= 0 && size_t(b.normalTexIndex) < s_textures.size()) ? s_textures[b.normalTexIndex] : s_flatNormalTex);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, (b.roughTexIndex >= 0 && size_t(b.roughTexIndex) < s_textures.size()) ? s_textures[b.roughTexIndex] : s_whiteTex);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, (b.aoTexIndex >= 0 && size_t(b.aoTexIndex) < s_textures.size()) ? s_textures[b.aoTexIndex] : s_whiteTex);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, (b.metalTexIndex >= 0 && size_t(b.metalTexIndex) < s_textures.size()) ? s_textures[b.metalTexIndex] : s_whiteTex);

        glDrawArrays(GL_TRIANGLES, b.firstVertex, b.vertexCount);
    }

    // -----------------------------------------------------------------------
    // PASS 3: DEFERRED LIGHTING RESOLVE
    // -----------------------------------------------------------------------
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);

    glUseProgram(s_resolveProg);
    glBindVertexArray(s_quadVao);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);

    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, s_gbufAlbedo);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, s_gbufNormal);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, s_gbufMaterial);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, s_gbufMirror);
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, s_gbufDepth);

    for (int i = 0; i < kMaxShadowSpots; ++i) {
        glActiveTexture(GL_TEXTURE5 + i);
        glBindTexture(GL_TEXTURE_2D, s_shadowSpots[i]);
    }

    glActiveTexture(GL_TEXTURE13);
    glBindTexture(GL_TEXTURE_2D, s_envTex != 0 ? s_envTex : s_whiteTex);

    glUniform3f(loc_r_uCamRow0, s_scene.camera.rot[0][0], s_scene.camera.rot[0][1], s_scene.camera.rot[0][2]);
    glUniform3f(loc_r_uCamRow1, s_scene.camera.rot[1][0], s_scene.camera.rot[1][1], s_scene.camera.rot[1][2]);
    glUniform3f(loc_r_uCamRow2, s_scene.camera.rot[2][0], s_scene.camera.rot[2][1], s_scene.camera.rot[2][2]);
    glUniform3f(loc_r_uCamSrc,  s_scene.camera.src[0], s_scene.camera.src[1], s_scene.camera.src[2]);
    glUniform1f(loc_r_uSx, sx);
    glUniform1f(loc_r_uOx, ox);
    glUniform1f(loc_r_uSy, sy);
    glUniform1f(loc_r_uOy, oy);
    glUniform1f(loc_r_uDza, dza);
    glUniform1f(loc_r_uDzb, dzb);
    glUniform1f(loc_r_uInvSx, invSx);
    glUniform1f(loc_r_uInvSy, invSy);
    glUniform1f(loc_r_uNearZ, nearZ);
    glUniform1f(loc_r_uFarZ, farZ);
    glUniform1f(loc_r_uExposure, 1.0f);

    glUniform1f(loc_r_uDiffuseFactor,  1.0f);
    glUniform1f(loc_r_uSpecularFactor, 1.0f);
    glUniform1f(loc_r_uAmbientFactor,  0.25f);
    glUniform3f(loc_r_uSkyZenith, s_scene.skyZenith[0]/255.0f, s_scene.skyZenith[1]/255.0f, s_scene.skyZenith[2]/255.0f);
    glUniform3f(loc_r_uSkyNadir,  s_scene.skyNadir[0]/255.0f,  s_scene.skyNadir[1]/255.0f,  s_scene.skyNadir[2]/255.0f);
    glUniform4f(loc_r_uFlatAmbient, s_scene.ambient[0]/255.0f, s_scene.ambient[1]/255.0f, s_scene.ambient[2]/255.0f, s_scene.shAmbient ? 0.0f : 1.0f);
    glUniform4f(loc_r_uHdrMode, 1.0f, 0.0f, 0.0f, 0.0f);
    glUniform1i(loc_r_uShadowsOn, 1);
    glUniform1i(loc_r_uHasEnvTex, s_envTex != s_whiteTex ? 1 : 0);

    int numActiveLights = 0;
    uploadLightUniforms(s_resolveLightLocs, numActiveLights);
    glUniform1i(loc_r_uNumLights, numActiveLights);

    glDrawArrays(GL_TRIANGLES, 0, 3);

    // -----------------------------------------------------------------------
    // PASS 3b: VOLUMETRIC SPOT CONES (ADDITIVE BEAMS)
    // -----------------------------------------------------------------------
    glUseProgram(s_coneProg);
    glBindVertexArray(s_quadVao);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);

    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, s_gbufDepth);
    for (int i = 0; i < kMaxShadowSpots; ++i) {
        glActiveTexture(GL_TEXTURE5 + i);
        glBindTexture(GL_TEXTURE_2D, s_shadowSpots[i]);
    }

    glUniform3f(loc_c_uCamRow0, s_scene.camera.rot[0][0], s_scene.camera.rot[0][1], s_scene.camera.rot[0][2]);
    glUniform3f(loc_c_uCamRow1, s_scene.camera.rot[1][0], s_scene.camera.rot[1][1], s_scene.camera.rot[1][2]);
    glUniform3f(loc_c_uCamRow2, s_scene.camera.rot[2][0], s_scene.camera.rot[2][1], s_scene.camera.rot[2][2]);
    glUniform3f(loc_c_uCamSrc,  s_scene.camera.src[0], s_scene.camera.src[1], s_scene.camera.src[2]);
    glUniform1f(loc_c_uSx, sx);
    glUniform1f(loc_c_uOx, ox);
    glUniform1f(loc_c_uSy, sy);
    glUniform1f(loc_c_uOy, oy);
    glUniform1f(loc_c_uDza, dza);
    glUniform1f(loc_c_uDzb, dzb);
    glUniform1f(loc_c_uInvSx, invSx);
    glUniform1f(loc_c_uInvSy, invSy);
    glUniform1f(loc_c_uFarZ, farZ);
    glUniform1i(loc_c_uShadowsOn, 1);
    // The CPU's cone constants, live from FeatureFlags (greets sets
    // cone_strength 1.2 in GreetsDisco.cpp): density = cone_strength * 1e-3,
    // scaled by hdr_glow_scale unless the cone soft-knee is on
    // (DeferredVolumetric.cpp, and Deferred.mm:1180 in the Metal bench).
    {
        const float glow = fds::FeatureFlags::hdr_cone_softknee() ? 1.0f : fds::FeatureFlags::hdr_glow_scale();
        glUniform1f(loc_c_uConeDensity, fds::FeatureFlags::cone_strength() * 0.001f * glow);
        glUniform1f(loc_c_uConeNSamples, float(fds::FeatureFlags::vol_n_samples()));
        // The CPU's fade-window floor is 12 z16 quanta; this depth is float,
        // so carry the same world-unit ballpark (Deferred.mm:1186).
        glUniform1f(loc_c_uConeFadeFloor, 12.0f * (farZ * 1.1f) / 65280.0f);
    }

    uploadLightUniforms(s_coneLightLocs, numActiveLights);
    glUniform1i(loc_c_uNumLights, numActiveLights);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);

    // -----------------------------------------------------------------------
    // PASS 3c: TRANSPARENT (XPAR) FORWARD SURFACES
    // -----------------------------------------------------------------------
    glUseProgram(s_xparProg);
    glBindVertexArray(s_sceneVao);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_BLEND);
    glDisable(GL_CULL_FACE);

    glUniform3f(loc_x_uCamRow0, s_scene.camera.rot[0][0], s_scene.camera.rot[0][1], s_scene.camera.rot[0][2]);
    glUniform3f(loc_x_uCamRow1, s_scene.camera.rot[1][0], s_scene.camera.rot[1][1], s_scene.camera.rot[1][2]);
    glUniform3f(loc_x_uCamRow2, s_scene.camera.rot[2][0], s_scene.camera.rot[2][1], s_scene.camera.rot[2][2]);
    glUniform3f(loc_x_uCamSrc,  s_scene.camera.src[0], s_scene.camera.src[1], s_scene.camera.src[2]);
    glUniform1f(loc_x_uSx, sx);
    glUniform1f(loc_x_uOx, ox);
    glUniform1f(loc_x_uSy, sy);
    glUniform1f(loc_x_uOy, oy);
    glUniform1f(loc_x_uDza, dza);
    glUniform1f(loc_x_uDzb, dzb);
    glUniform3f(loc_x_uAmbient, s_scene.ambient[0]/255.0f, s_scene.ambient[1]/255.0f, s_scene.ambient[2]/255.0f);

    uploadLightUniforms(s_xparLightLocs, numActiveLights);
    glUniform1i(loc_x_uNumLights, numActiveLights);

    for (const auto &b : s_scene.batches) {
        if (!b.transparent && !b.additive) continue;

        if (b.additive) {
            glBlendFunc(GL_ONE, GL_ONE);
        } else {
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }

        glUniform3f(loc_x_uRotRow0, b.rot[0][0], b.rot[0][1], b.rot[0][2]);
        glUniform3f(loc_x_uRotRow1, b.rot[1][0], b.rot[1][1], b.rot[1][2]);
        glUniform3f(loc_x_uRotRow2, b.rot[2][0], b.rot[2][1], b.rot[2][2]);
        glUniform3f(loc_x_uObjPos,  b.pos[0], b.pos[1], b.pos[2]);

        glUniform4f(loc_x_uBaseColor, b.baseColor[0], b.baseColor[1], b.baseColor[2], b.textureIndex >= 0 ? 1.0f : 0.0f);
        float ggxRough = std::sqrt(2.0f / float(b.glossiness + 2));
        glUniform4f(loc_x_uMatParams, b.diffuse, b.specular, ggxRough, b.luminosity);
        float alpha = (b.xparBlendAlpha > 0.0f) ? b.xparBlendAlpha : std::max(0.01f, 1.0f - b.transparency * 0.01f);
        glUniform4f(loc_x_uMisc2, float(b.envProbe), b.reflection, alpha, b.additive ? 1.0f : 0.0f);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (b.textureIndex >= 0 && size_t(b.textureIndex) < s_textures.size()) ? s_textures[b.textureIndex] : s_whiteTex);

        glDrawArrays(GL_TRIANGLES, b.firstVertex, b.vertexCount);
    }
    glDisable(GL_BLEND);

    // -----------------------------------------------------------------------
    // PASS 4: OMNI FLARE BILLBOARD SPRITES (ADDITIVE GLOW)
    // -----------------------------------------------------------------------
    glUseProgram(s_flareProg);
    glBindVertexArray(s_flareVao);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDisable(GL_CULL_FACE);

    glUniform3f(loc_f_uCamRow0, s_scene.camera.rot[0][0], s_scene.camera.rot[0][1], s_scene.camera.rot[0][2]);
    glUniform3f(loc_f_uCamRow1, s_scene.camera.rot[1][0], s_scene.camera.rot[1][1], s_scene.camera.rot[1][2]);
    glUniform3f(loc_f_uCamRow2, s_scene.camera.rot[2][0], s_scene.camera.rot[2][1], s_scene.camera.rot[2][2]);
    glUniform3f(loc_f_uCamSrc,  s_scene.camera.src[0], s_scene.camera.src[1], s_scene.camera.src[2]);
    glUniform1f(loc_f_uSx, sx);
    glUniform1f(loc_f_uOx, ox);
    glUniform1f(loc_f_uSy, sy);
    glUniform1f(loc_f_uOy, oy);
    glUniform1f(loc_f_uDza, dza);
    glUniform1f(loc_f_uDzb, dzb);

    for (const auto &L : s_scene.lights) {
        if (L.flareTexIndex < 0 || L.flareSize <= 0.0f || size_t(L.flareTexIndex) >= s_textures.size()) continue;

        float rel[3] = { L.pos[0] - s_scene.camera.src[0], L.pos[1] - s_scene.camera.src[1], L.pos[2] - s_scene.camera.src[2] };
        float vz = s_scene.camera.rot[2][0]*rel[0] + s_scene.camera.rot[2][1]*rel[1] + s_scene.camera.rot[2][2]*rel[2];
        if (vz <= nearZ || vz >= farZ) continue;

        float worldHalf = 2.0f * s_scene.imageSize * L.flareSize;
        glUniform4f(loc_f_uFlareCenter, L.pos[0], L.pos[1], L.pos[2], worldHalf);
        float gain = 1.0f;
        glUniform3f(loc_f_uFlareGain, (L.color[0]/255.0f)*gain, (L.color[1]/255.0f)*gain, (L.color[2]/255.0f)*gain);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_textures[L.flareTexIndex]);

        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    glDisable(GL_BLEND);
    glBindVertexArray(0);

#endif // __EMSCRIPTEN__
}

void GpuWeb_Shutdown() {
#ifdef __EMSCRIPTEN__
    if (!s_initialized) return;

    if (s_gbufFbo != 0) {
        glDeleteFramebuffers(1, &s_gbufFbo);
        glDeleteTextures(1, &s_gbufAlbedo);
        glDeleteTextures(1, &s_gbufNormal);
        glDeleteTextures(1, &s_gbufMaterial);
        glDeleteTextures(1, &s_gbufMirror);
        glDeleteTextures(1, &s_gbufDepth);
        s_gbufFbo = 0;
    }
    if (s_shadowFbo != 0) {
        glDeleteFramebuffers(1, &s_shadowFbo);
        glDeleteTextures(kMaxShadowSpots, s_shadowSpots);
        s_shadowFbo = 0;
    }
    if (s_gbufProg)    glDeleteProgram(s_gbufProg);
    if (s_shadowProg)  glDeleteProgram(s_shadowProg);
    if (s_resolveProg) glDeleteProgram(s_resolveProg);
    if (s_coneProg)    glDeleteProgram(s_coneProg);
    if (s_xparProg)    glDeleteProgram(s_xparProg);
    if (s_flareProg)   glDeleteProgram(s_flareProg);

    if (s_sceneVao) glDeleteVertexArrays(1, &s_sceneVao);
    if (s_sceneVbo) glDeleteBuffers(1, &s_sceneVbo);
    if (s_quadVao)  glDeleteVertexArrays(1, &s_quadVao);
    if (s_flareVao) glDeleteVertexArrays(1, &s_flareVao);

    if (s_whiteTex) glDeleteTextures(1, &s_whiteTex);
    if (s_flatNormalTex) glDeleteTextures(1, &s_flatNormalTex);
    if (!s_textures.empty()) glDeleteTextures((GLsizei)s_textures.size(), s_textures.data());

    s_textures.clear();
    s_initialized = false;
#endif
}

} // namespace rev
