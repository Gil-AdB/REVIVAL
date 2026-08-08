#include "Deferred.h"

#import <Foundation/Foundation.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL.h>
#include <SDL_metal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <map>
#include <vector>

namespace gpubench {
namespace {

// The interactive keymap, printed on startup and mirrored in the HUD.
//
// The MOVE/LOOK half is not invented here: it is FDS/CAMERAS/CAMERAS.CPP's
// Dynamic_Camera(), the house free-cam that DEMO/DisplaceTest.cpp drives and
// every scene's TAB-camera uses, called directly (SceneIngest FreeCam*). The
// aliases (End/PgDn, gray +/-, the legacy Z-for-back) are its own.
// Mouse-look and the four view keys below it are GpuBench ADDITIONS — the
// house cam reads no mouse and has no notion of a spline toggle.
const char *kKeymap =
    "[KEYS] move   W / S,Z          forward / back        (engine Dynamic_Camera)\n"
    "[KEYS]        A,End / D,PgDn   strafe left / right\n"
    "[KEYS]        Q,gray+ / E,gray-  up / down\n"
    "[KEYS] look   arrows            Left,Right = yaw   Up,Down = pitch\n"
    "[KEYS]        Home / PgUp       roll left / right\n"
    "[KEYS]        mouse-drag        look (GpuBench addition; the house cam has none)\n"
    "[KEYS] speed  , / .             translation dial slower / faster (x1.1 per frame held)\n"
    "[KEYS]        K / L             rotation dial slower / faster\n"
    "[KEYS] pose   G                 dump the pose ([DTEST-POSE] + a --cam= string)\n"
    "[KEYS] view   TAB               free-fly <-> the AUTHORED camera spline\n"
    "[KEYS]        SPACE             pause the demo timer (the camera still moves)\n"
    "[KEYS]        [ / ]             scrub the demo timer -/+ 100\n"
    "[KEYS]        ESC, Backspace    quit\n";

// --- MSL-matching layouts (float3 is 16-byte aligned in MSL) ----------------

struct FrameUniforms {
    float camRow0[4], camRow1[4], camRow2[4];
    float camSrc[4];
    float sx, ox, sy, oy;
    float dza, dzb, invSx, invSy;
    float nearZ, farZ, exposure;
    uint32_t numLights;
    uint32_t shadowsOn;
    float ambientFactor, diffuseFactor, specularFactor;
    float lightRangeScale;
    int32_t vizLight;
    float pad2[2];              // align clipPlane to 16 (MSL float4 rule)
    float clipPlane[4];         // xyz = N, w = d; all-zero N = disabled
    uint32_t mirrorCount;
    float envReflGain;
    float pad3[2];
    float aabbMin[4], aabbMax[4];
    float envProbePos[8][4];
    float metalCompat[4];       // .x = D1 dial, .y = D2 dial (see Deferred.h)
    // FLAT AMBIENT — the CPU's `else` branch, not an approximation of it.
    // `sh_ambient` defaults 0 (FeatureFlags.def:47) and ONLY greets turns it on
    // (GREETS.CPP:1175 setDefault). Every other scene therefore runs the flat
    // branch at DeferredSurfaceKernel.cpp:1761-1768,
    //     lB = Luminosity*255 + Diffuse * Sc->Ambient.B
    // with NO Ambient_Factor (that global is dead in FDS — SHADING_CONTRACT D7).
    // .rgb = Scene::Ambient/255, .w = 1 selects it over the SH evaluation.
    float flatAmbient[4];
};

struct ConeUniforms {
    float density, nSamples, fadeFloor, pad;
};

struct BatchUniforms {
    float rotRow0[4], rotRow1[4], rotRow2[4];
    float objPos[4];
    float baseColor[4];
    float matParams[4];
    float mapFlags[4];
    float misc[4];              // .x = mirror panel index (1-based)
    float misc2[4];             // .x = env probe index (1-based), .y = Reflection,
                                // .z = XparBlendAlpha, .w = transparent dst weight
    float xpar[4];              // .x raw Luminosity, .y Specular, .z Glossiness,
                                // .w additive flag — the FORWARD transparent kernel
};

struct XparUniforms {
    float sceneAmbient[4];      // Scene::Ambient / 255 (float3 + pad)
    float peelReverse, usePeelFloor, pad0, pad1;
};

struct GpuLight {
    float pos[4];
    float color[4];
    float range, invRange;
    int32_t shadowIndex;      // cube slot for omnis, 2D-map slot for spots
    float shadowNear, shadowFar;
    int32_t isSpot;
    float cosInner, cosOuter;
    float dir[4];             // world, normalised (Omni::IDir)
    // Spot shadow projection: view rows from Kick_Camera + tan(halfFov).
    float sRow0[4], sRow1[4], sRow2[4];
};

struct ShadowUniforms {
    float row0[4], row1[4], row2[4];
    float lightPos[4];
    float dza, dzb;
    // 1/tan(halfFov) for a spot's narrow frustum; 1.0 for a 90-degree cube face.
    float projScale, pad1;
};

struct BloomUniforms {
    float srcSize[2];
    float dstSize[2];
    float threshold;
    float intensity;
    float pad[2];
};

struct FlareUniforms {
    float centerPx[4];   // .xy screen px, .z view z, .w half-extent px
    float gain[4];       // .x gain, .zw target resolution
};

constexpr int kMaxShadowCubes = 16;
constexpr int kMaxSpotMaps    = 16;

// Metal cube-face convention, slice order +X,-X,+Y,-Y,+Z,-Z. Rows are
// (right, up, forward) so the shadow vertex shader's rowmul yields
// (view.x, view.y, view.z) with a 90-degree square frustum.
//
// DERIVED, not guessed. Metal (like D3D) maps a direction to a face with
//   ma = max(|x|,|y|,|z|),  u = 0.5*(sc/ma + 1),  v = 0.5*(tc/ma + 1)
// and per face:
//   +X: sc=-z tc=-y   -X: sc=+z tc=-y
//   +Y: sc=+x tc=+z   -Y: sc=+x tc=-z
//   +Z: sc=+x tc=-y   -Z: sc=-x tc=-y
// Our bake writes clip.xy = (dot(right,d), dot(up,d)) and clip.w = dot(fwd,d).
// Metal's viewport has an UPPER-LEFT origin, so texel row 0 <-> ndc.y=+1 <->
// dot(up,d)=+ma, while the sampler puts v=0 (row 0) at tc=-ma. Therefore
//   right = sc_direction   and   up = -tc_direction.
// The `up = -tc` sign is the part that was wrong in the first implementation:
// every face was baked VERTICALLY MIRRORED, so the tap read the wrong texel and
// reported "occluded" almost everywhere once lights were in range. The symptom
// (0% fully-unshadowed pixels) is the signature of a bad face mapping.
struct CubeFace { float right[3], up[3], fwd[3]; };
constexpr CubeFace kCubeFaces[6] = {
    {{ 0,  0, -1}, {0,  1,  0}, { 1,  0,  0}},   // +X  sc=-z tc=-y
    {{ 0,  0,  1}, {0,  1,  0}, {-1,  0,  0}},   // -X  sc=+z tc=-y
    {{ 1,  0,  0}, {0,  0, -1}, { 0,  1,  0}},   // +Y  sc=+x tc=+z
    {{ 1,  0,  0}, {0,  0,  1}, { 0, -1,  0}},   // -Y  sc=+x tc=-z
    {{ 1,  0,  0}, {0,  1,  0}, { 0,  0,  1}},   // +Z  sc=+x tc=-y
    {{-1,  0,  0}, {0,  1,  0}, { 0,  0, -1}},   // -Z  sc=-x tc=-y
};

// ---- tiny 5x7 bitmap font for the HUD ---------------------------------
// A HUD is required (live per-pass GPU ms), and there is no text stack in this
// target. 5 columns x 7 rows, each column a bit mask with bit0 = top row.
struct Glyph { char c; uint8_t col[5]; };
static const Glyph kFont[] = {
 {' ',{0x00,0x00,0x00,0x00,0x00}},{'!',{0x00,0x00,0x5F,0x00,0x00}},
 {'.',{0x00,0x60,0x60,0x00,0x00}},{',',{0x00,0x50,0x30,0x00,0x00}},
 {':',{0x00,0x36,0x36,0x00,0x00}},{'/',{0x20,0x10,0x08,0x04,0x02}},
 {'-',{0x08,0x08,0x08,0x08,0x08}},{'+',{0x08,0x08,0x3E,0x08,0x08}},
 {'=',{0x14,0x14,0x14,0x14,0x14}},{'%',{0x23,0x13,0x08,0x64,0x62}},
 {'(',{0x00,0x1C,0x22,0x41,0x00}},{')',{0x00,0x41,0x22,0x1C,0x00}},
 {'[',{0x00,0x7F,0x41,0x41,0x00}},{']',{0x00,0x41,0x41,0x7F,0x00}},
 {'<',{0x08,0x14,0x22,0x41,0x00}},{'>',{0x41,0x22,0x14,0x08,0x00}},
 {'0',{0x3E,0x51,0x49,0x45,0x3E}},{'1',{0x00,0x42,0x7F,0x40,0x00}},
 {'2',{0x42,0x61,0x51,0x49,0x46}},{'3',{0x21,0x41,0x45,0x4B,0x31}},
 {'4',{0x18,0x14,0x12,0x7F,0x10}},{'5',{0x27,0x45,0x45,0x45,0x39}},
 {'6',{0x3C,0x4A,0x49,0x49,0x30}},{'7',{0x01,0x71,0x09,0x05,0x03}},
 {'8',{0x36,0x49,0x49,0x49,0x36}},{'9',{0x06,0x49,0x49,0x29,0x1E}},
 {'A',{0x7E,0x11,0x11,0x11,0x7E}},{'B',{0x7F,0x49,0x49,0x49,0x36}},
 {'C',{0x3E,0x41,0x41,0x41,0x22}},{'D',{0x7F,0x41,0x41,0x22,0x1C}},
 {'E',{0x7F,0x49,0x49,0x49,0x41}},{'F',{0x7F,0x09,0x09,0x09,0x01}},
 {'G',{0x3E,0x41,0x49,0x49,0x7A}},{'H',{0x7F,0x08,0x08,0x08,0x7F}},
 {'I',{0x00,0x41,0x7F,0x41,0x00}},{'J',{0x20,0x40,0x41,0x3F,0x01}},
 {'K',{0x7F,0x08,0x14,0x22,0x41}},{'L',{0x7F,0x40,0x40,0x40,0x40}},
 {'M',{0x7F,0x02,0x0C,0x02,0x7F}},{'N',{0x7F,0x04,0x08,0x10,0x7F}},
 {'O',{0x3E,0x41,0x41,0x41,0x3E}},{'P',{0x7F,0x09,0x09,0x09,0x06}},
 {'Q',{0x3E,0x41,0x51,0x21,0x5E}},{'R',{0x7F,0x09,0x19,0x29,0x46}},
 {'S',{0x46,0x49,0x49,0x49,0x31}},{'T',{0x01,0x01,0x7F,0x01,0x01}},
 {'U',{0x3F,0x40,0x40,0x40,0x3F}},{'V',{0x1F,0x20,0x40,0x20,0x1F}},
 {'W',{0x3F,0x40,0x38,0x40,0x3F}},{'X',{0x63,0x14,0x08,0x14,0x63}},
 {'Y',{0x07,0x08,0x70,0x08,0x07}},{'Z',{0x61,0x51,0x49,0x45,0x43}},
};
static const Glyph *FindGlyph(char c) {
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    for (const auto &g : kFont) if (g.c == c) return &g;
    return &kFont[0];
}
// Draw `s` into an RGBA8 buffer at (x0,y0), `sc` pixels per font pixel.
static void HudText(uint8_t *buf, int bw, int bh, int x0, int y0, int sc,
                    const char *s, uint8_t r, uint8_t g, uint8_t b) {
    int x = x0;
    for (const char *p = s; *p; ++p) {
        const Glyph *gl = FindGlyph(*p);
        for (int c = 0; c < 5; ++c)
            for (int row = 0; row < 7; ++row) {
                if (!((gl->col[c] >> row) & 1)) continue;
                for (int dy = 0; dy < sc; ++dy)
                    for (int dx = 0; dx < sc; ++dx) {
                        const int px = x + c * sc + dx, py = y0 + row * sc + dy;
                        if (px < 0 || py < 0 || px >= bw || py >= bh) continue;
                        uint8_t *o = buf + (size_t(py) * size_t(bw) + size_t(px)) * 4;
                        o[0] = r; o[1] = g; o[2] = b; o[3] = 255;
                    }
            }
        x += 6 * sc;
    }
}

double Percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double idx = p * double(v.size() - 1);
    const size_t lo = size_t(std::floor(idx)), hi = size_t(std::ceil(idx));
    return v[lo] + (v[hi] - v[lo]) * (idx - double(lo));
}

std::string ReadFile(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string s(size_t(n < 0 ? 0 : n), '\0');
    if (n > 0 && std::fread(s.data(), 1, size_t(n), f) != size_t(n)) s.clear();
    std::fclose(f);
    return s;
}

bool WritePPM(const std::string &path, const uint8_t *bgra, int w, int h, size_t rowBytes) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<uint8_t> row(size_t(w) * 3);
    for (int y = 0; y < h; ++y) {
        const uint8_t *s = bgra + size_t(y) * rowBytes;
        for (int x = 0; x < w; ++x) {
            row[size_t(x) * 3 + 0] = s[size_t(x) * 4 + 2];
            row[size_t(x) * 3 + 1] = s[size_t(x) * 4 + 1];
            row[size_t(x) * 3 + 2] = s[size_t(x) * 4 + 0];
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
}

// Project the FLD's authored vertical zenith->nadir backdrop gradient into 9 L2
// SH coefficients per channel, by numeric integration over the sphere. Real
// authored content, and the shader still pays the full 9-coefficient evaluation
// (which is the point — --sh_ambient is what greets runs, and a flat constant
// would be a different, cheaper shader).
void ProjectSkyGradientToSH(const float zenith[3], const float nadir[3], float sh[9][4]) {
    std::memset(sh, 0, sizeof(float) * 9 * 4);
    const int NT = 64, NP = 128;
    double acc[9][3] = {};
    double wsum = 0.0;
    for (int it = 0; it < NT; ++it) {
        const double theta = (double(it) + 0.5) / NT * M_PI;
        const double st = std::sin(theta), ct = std::cos(theta);
        for (int ip = 0; ip < NP; ++ip) {
            const double phi = (double(ip) + 0.5) / NP * 2.0 * M_PI;
            // world: +Y is up, matching the scene's authored gradient axis
            const double y = ct, x = st * std::cos(phi), z = st * std::sin(phi);
            const double t = (y + 1.0) * 0.5;
            const double dw = st * (M_PI / NT) * (2.0 * M_PI / NP);
            double Y[9];
            Y[0] = 0.282095;
            Y[1] = 0.488603 * y;
            Y[2] = 0.488603 * z;
            Y[3] = 0.488603 * x;
            Y[4] = 1.092548 * x * y;
            Y[5] = 1.092548 * y * z;
            Y[6] = 0.315392 * (3.0 * z * z - 1.0);
            Y[7] = 1.092548 * x * z;
            Y[8] = 0.546274 * (x * x - y * y);
            for (int c = 0; c < 3; ++c) {
                const double L = (nadir[c] + (zenith[c] - nadir[c]) * t) / 255.0;
                for (int k = 0; k < 9; ++k) acc[k][c] += L * Y[k] * dw;
            }
            wsum += dw;
        }
    }
    (void)wsum;
    for (int k = 0; k < 9; ++k)
        for (int c = 0; c < 3; ++c) sh[k][c] = float(acc[k][c]);
}

}  // namespace

bool RunDeferred(Scene &scene, const DeferredOptions &opt,
                 const std::string &shaderPath, DeferredResult &out) {
@autoreleasepool {
    NSError *err = nil;
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) { std::fprintf(stderr, "[DEFERRED] no Metal device\n"); return false; }

    const std::string src = ReadFile(shaderPath);
    if (src.empty()) {
        std::fprintf(stderr, "[DEFERRED] cannot read %s\n", shaderPath.c_str());
        return false;
    }
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(src.c_str())
                                           options:[MTLCompileOptions new]
                                             error:&err];
    if (!lib) {
        std::fprintf(stderr, "[DEFERRED] MSL compile failed: %s\n",
                     [[err localizedDescription] UTF8String]);
        return false;
    }

    const int W = scene.xres, H = scene.yres;

    // ---- vertex descriptor / pipelines -----------------------------------
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat3; vd.attributes[0].offset = 0;  vd.attributes[0].bufferIndex = 0;
    vd.attributes[1].format = MTLVertexFormatFloat3; vd.attributes[1].offset = 12; vd.attributes[1].bufferIndex = 0;
    vd.attributes[2].format = MTLVertexFormatFloat2; vd.attributes[2].offset = 24; vd.attributes[2].bufferIndex = 0;
    // Engine tangent (xyz) + UV-winding handedness sign (w) — see SceneIngest.h.
    vd.attributes[3].format = MTLVertexFormatFloat4; vd.attributes[3].offset = 32; vd.attributes[3].bufferIndex = 0;
    vd.layouts[0].stride = sizeof(Vertex);

    MTLRenderPipelineDescriptor *gpd = [MTLRenderPipelineDescriptor new];
    gpd.vertexFunction = [lib newFunctionWithName:@"vs_gbuffer"];
    gpd.fragmentFunction = [lib newFunctionWithName:@"fs_gbuffer"];
    gpd.vertexDescriptor = vd;
    gpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    gpd.colorAttachments[1].pixelFormat = MTLPixelFormatRG16Snorm;
    gpd.colorAttachments[2].pixelFormat = MTLPixelFormatRGBA8Unorm;
    gpd.colorAttachments[3].pixelFormat = MTLPixelFormatRGBA8Uint;  // mirror id, metalness, env probe, F0
    gpd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    id<MTLRenderPipelineState> psoGBuf = [dev newRenderPipelineStateWithDescriptor:gpd error:&err];
    if (!psoGBuf) { std::fprintf(stderr, "[DEFERRED] gbuffer pso: %s\n", [[err localizedDescription] UTF8String]); return false; }

    MTLRenderPipelineDescriptor *spd = [MTLRenderPipelineDescriptor new];
    spd.vertexFunction = [lib newFunctionWithName:@"vs_shadow"];
    spd.fragmentFunction = nil;                  // depth-only
    spd.vertexDescriptor = vd;
    spd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    id<MTLRenderPipelineState> psoShadow = [dev newRenderPipelineStateWithDescriptor:spd error:&err];
    if (!psoShadow) { std::fprintf(stderr, "[DEFERRED] shadow pso: %s\n", [[err localizedDescription] UTF8String]); return false; }

    // A MISSING MSL ENTRY POINT MUST BE FATAL, and this is not defensive
    // programming — it cost a full debugging round. `cmake --build --target
    // GpuBench` does NOT build the separate GpuBenchShaders staging target, so
    // the binary can run against a STALE .metal that lacks a newly added
    // function. `newFunctionWithName:` then returns nil, and Metal accepts a
    // pipeline with a nil fragmentFunction as VALID (it is how depth-only
    // pipelines are built). The result is a perfectly healthy pipeline that
    // writes nothing, and a new pass that measures as "changed 0 pixels" with
    // no error anywhere. Fail here instead.
    auto fn_ = [&](NSString *n) -> id<MTLFunction> {
        id<MTLFunction> f = [lib newFunctionWithName:n];
        if (!f) {
            std::fprintf(stderr,
                "[DEFERRED] FATAL: shader entry point '%s' not found in the compiled\n"
                "[DEFERRED]   library. The staged .metal is almost certainly STALE —\n"
                "[DEFERRED]   build the GpuBenchShaders target too (plain `cmake --build\n"
                "[DEFERRED]   <dir>` does it; `--target GpuBench` does NOT).\n",
                [n UTF8String]);
            std::exit(4);
        }
        return f;
    };
    auto makeFsPso = [&](NSString *fn, MTLPixelFormat fmt) -> id<MTLRenderPipelineState> {
        MTLRenderPipelineDescriptor *p = [MTLRenderPipelineDescriptor new];
        p.vertexFunction = fn_(@"vs_fullscreen");
        p.fragmentFunction = fn_(fn);
        p.colorAttachments[0].pixelFormat = fmt;
        NSError *e = nil;
        id<MTLRenderPipelineState> s = [dev newRenderPipelineStateWithDescriptor:p error:&e];
        if (!s) std::fprintf(stderr, "[DEFERRED] pso %s: %s\n",
                             [fn UTF8String], [[e localizedDescription] UTF8String]);
        return s;
    };
    // Flare sprites: ADDITIVE into the HDR target, matching the CPU's
    // Spriter<Res,true,true> which adds into the float radiance buffer.
    id<MTLRenderPipelineState> psoFlare;
    {
        MTLRenderPipelineDescriptor *p = [MTLRenderPipelineDescriptor new];
        p.vertexFunction = fn_(@"vs_flare");
        p.fragmentFunction = fn_(@"fs_flare");
        p.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        p.colorAttachments[0].blendingEnabled = YES;
        p.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        p.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
        p.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
        p.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
        p.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorZero;
        p.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
        NSError *e = nil;
        psoFlare = [dev newRenderPipelineStateWithDescriptor:p error:&e];
        if (!psoFlare) { std::fprintf(stderr, "[DEFERRED] flare pso: %s\n",
                                      [[e localizedDescription] UTF8String]); return false; }
    }
    // Volumetric cones: ADDITIVE into the HDR target before bloom, which is
    // where the CPU composites them (VolCompositeAdd into g_hdrBuf, unclamped).
    id<MTLRenderPipelineState> psoCones;
    {
        MTLRenderPipelineDescriptor *p = [MTLRenderPipelineDescriptor new];
        p.vertexFunction = fn_(@"vs_fullscreen");
        p.fragmentFunction = fn_(@"fs_cones");
        p.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        p.colorAttachments[0].blendingEnabled = YES;
        p.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        p.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
        p.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
        p.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorZero;
        p.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
        NSError *e = nil;
        psoCones = [dev newRenderPipelineStateWithDescriptor:p error:&e];
        if (!psoCones) { std::fprintf(stderr, "[DEFERRED] cone pso: %s\n",
                                      [[e localizedDescription] UTF8String]); return false; }
    }
    id<MTLRenderPipelineState> psoBloomBright = makeFsPso(@"fs_bloom_bright", MTLPixelFormatRGBA16Float);
    id<MTLRenderPipelineState> psoBloomBlur   = makeFsPso(@"fs_bloom_blur",   MTLPixelFormatRGBA16Float);
    id<MTLRenderPipelineState> psoBloomAdd;
    {
        MTLRenderPipelineDescriptor *p = [MTLRenderPipelineDescriptor new];
        p.vertexFunction = fn_(@"vs_fullscreen");
        p.fragmentFunction = fn_(@"fs_bloom_add");
        p.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        p.colorAttachments[0].blendingEnabled = YES;
        p.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        p.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
        p.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
        p.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorZero;
        p.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
        NSError *e = nil;
        psoBloomAdd = [dev newRenderPipelineStateWithDescriptor:p error:&e];
        if (!psoBloomAdd) { std::fprintf(stderr, "[DEFERRED] bloom-add pso: %s\n",
                                         [[e localizedDescription] UTF8String]); return false; }
    }
    // ---- TRANSPARENT surfaces: depth-resolve + three composite modes -------
    // The depth-resolve pipeline has NO colour attachment: it exists only to
    // pick the one fragment per pixel that this (mesh, side, peel pass) layer
    // keeps, which is what the CPU's single-slot xpar G-buffer does implicitly.
    id<MTLRenderPipelineState> psoXparDepth;
    {
        MTLRenderPipelineDescriptor *p = [MTLRenderPipelineDescriptor new];
        p.vertexFunction = fn_(@"vs_xpar");
        p.fragmentFunction = fn_(@"fs_xpar_depth");
        p.vertexDescriptor = vd;
        p.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        NSError *e = nil;
        psoXparDepth = [dev newRenderPipelineStateWithDescriptor:p error:&e];
        if (!psoXparDepth) { std::fprintf(stderr, "[DEFERRED] xpar-depth pso: %s\n",
                                          [[e localizedDescription] UTF8String]); return false; }
    }
    // Three blend modes, one per CPU composite form. The per-material weight
    // travels as the encoder's BLEND COLOUR, so `dw` and `alpha` are real
    // blend factors rather than shader arithmetic against a stale destination.
    auto makeXparPso = [&](MTLBlendFactor srcF, MTLBlendFactor dstF)
                       -> id<MTLRenderPipelineState> {
        MTLRenderPipelineDescriptor *p = [MTLRenderPipelineDescriptor new];
        p.vertexFunction = fn_(@"vs_xpar");
        p.fragmentFunction = fn_(@"fs_xpar");
        p.vertexDescriptor = vd;
        p.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        p.colorAttachments[0].blendingEnabled = YES;
        p.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        p.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
        p.colorAttachments[0].sourceRGBBlendFactor = srcF;
        p.colorAttachments[0].destinationRGBBlendFactor = dstF;
        p.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorZero;
        p.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
        p.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        NSError *e = nil;
        id<MTLRenderPipelineState> s = [dev newRenderPipelineStateWithDescriptor:p error:&e];
        if (!s) std::fprintf(stderr, "[DEFERRED] xpar pso: %s\n",
                             [[e localizedDescription] UTF8String]);
        return s;
    };
    // out = lit + dst*dw               (Transparency-weighted, the shipped form)
    id<MTLRenderPipelineState> psoXparLegacy =
        makeXparPso(MTLBlendFactorOne, MTLBlendFactorBlendColor);
    // out = lit*a + dst*(1-a)          (XparBlendAlpha > 0: fountain's crystal)
    id<MTLRenderPipelineState> psoXparLerp =
        makeXparPso(MTLBlendFactorBlendColor, MTLBlendFactorOneMinusBlendColor);
    // out = lit + dst                  (Mat_Additive, order-independent)
    id<MTLRenderPipelineState> psoXparAdd =
        makeXparPso(MTLBlendFactorOne, MTLBlendFactorOne);
    if (!psoXparLegacy || !psoXparLerp || !psoXparAdd) return false;

    id<MTLRenderPipelineState> psoLight   = makeFsPso(@"fs_lighting", MTLPixelFormatRGBA16Float);
    id<MTLRenderPipelineState> psoTonemap = makeFsPso(@"fs_tonemap",  MTLPixelFormatBGRA8Unorm);
    id<MTLRenderPipelineState> psoViz     = makeFsPso(@"fs_viz",      MTLPixelFormatBGRA8Unorm);
    if (!psoLight || !psoTonemap || !psoViz) return false;

    MTLDepthStencilDescriptor *dsd = [MTLDepthStencilDescriptor new];
    dsd.depthCompareFunction = MTLCompareFunctionGreater;   // reversed-Z
    dsd.depthWriteEnabled = YES;
    id<MTLDepthStencilState> dss = [dev newDepthStencilStateWithDescriptor:dsd];

    // Transparent depth-peel resolve states. `Near` reproduces the CPU's legacy
    // single-pass peel (side Z cleared to "farthest", the rasterizer keeps the
    // nearer fragment — Mekalele.h:1392-1398); `Far` reproduces the K>1 reverse
    // peel (cleared to "nearest", keep the farther one above the peel floor,
    // ibid. :1399-1420) so the layers composite farthest-first.
    id<MTLDepthStencilState> dssXparNear, dssXparFar, dssXparEqual;
    {
        MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
        d.depthCompareFunction = MTLCompareFunctionGreater;   // reversed-Z: nearer
        d.depthWriteEnabled = YES;
        dssXparNear = [dev newDepthStencilStateWithDescriptor:d];
        d.depthCompareFunction = MTLCompareFunctionLess;      // farther
        dssXparFar = [dev newDepthStencilStateWithDescriptor:d];
        d.depthCompareFunction = MTLCompareFunctionEqual;
        d.depthWriteEnabled = NO;
        dssXparEqual = [dev newDepthStencilStateWithDescriptor:d];
    }

    MTLSamplerDescriptor *sdesc = [MTLSamplerDescriptor new];
    sdesc.minFilter = MTLSamplerMinMagFilterLinear;
    sdesc.magFilter = MTLSamplerMinMagFilterLinear;
    sdesc.mipFilter = MTLSamplerMipFilterLinear;
    sdesc.sAddressMode = MTLSamplerAddressModeRepeat;
    sdesc.tAddressMode = MTLSamplerAddressModeRepeat;
    sdesc.maxAnisotropy = 8;
    id<MTLSamplerState> samp = [dev newSamplerStateWithDescriptor:sdesc];

    MTLSamplerDescriptor *shd = [MTLSamplerDescriptor new];
    shd.minFilter = MTLSamplerMinMagFilterLinear;   // enables hardware PCF
    shd.magFilter = MTLSamplerMinMagFilterLinear;
    shd.compareFunction = MTLCompareFunctionGreater; // reversed-Z
    id<MTLSamplerState> shadowSamp = [dev newSamplerStateWithDescriptor:shd];
    // Plain (non-comparison) sampler so the diagnostic viz can read the RAW baked
    // depth instead of a pass/fail bit.
    MTLSamplerDescriptor *rawd = [MTLSamplerDescriptor new];
    rawd.minFilter = MTLSamplerMinMagFilterNearest;
    rawd.magFilter = MTLSamplerMinMagFilterNearest;
    id<MTLSamplerState> rawSamp = [dev newSamplerStateWithDescriptor:rawd];
    // Env probe cube: trilinear, because roughness selects a MIP and the CPU
    // lerps between levels too (nearest-level select speckled on noisy
    // roughness maps, DeferredSurfaceKernel.cpp:1094-1110).
    MTLSamplerDescriptor *envd = [MTLSamplerDescriptor new];
    envd.minFilter = MTLSamplerMinMagFilterLinear;
    envd.magFilter = MTLSamplerMinMagFilterLinear;
    envd.mipFilter = MTLSamplerMipFilterLinear;
    envd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    envd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> envSamp = [dev newSamplerStateWithDescriptor:envd];
    // Bilinear + clamp-to-edge for the bloom upsample (Hdr.cpp clamps its
    // bilinear fetch to the buffer edge the same way).
    MTLSamplerDescriptor *bsd = [MTLSamplerDescriptor new];
    bsd.minFilter = MTLSamplerMinMagFilterLinear;
    bsd.magFilter = MTLSamplerMinMagFilterLinear;
    bsd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    bsd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> bloomSamp = [dev newSamplerStateWithDescriptor:bsd];

    // ---- buffers / textures ----------------------------------------------
    id<MTLCommandQueue> queue = [dev newCommandQueue];
    id<MTLBuffer> vb = [dev newBufferWithBytes:scene.verts.data()
                                        length:scene.verts.size() * sizeof(Vertex)
                                       options:MTLResourceStorageModeShared];

    std::vector<id<MTLTexture>> texes;
    {
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        for (const auto &img : scene.textures) {
            MTLTextureDescriptor *td =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                  width:NSUInteger(img.w)
                                                                 height:NSUInteger(img.h)
                                                              mipmapped:YES];
            td.usage = MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModeShared;
            id<MTLTexture> t = [dev newTextureWithDescriptor:td];
            [t replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(img.w), NSUInteger(img.h))
                 mipmapLevel:0 withBytes:img.rgba.data() bytesPerRow:NSUInteger(img.w) * 4];
            [blit generateMipmapsForTexture:t];
            texes.push_back(t);
        }
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
    }

    // Render targets are PRIVATE. StorageModeShared forces write-through to
    // system memory and disables lossless compression on Apple silicon; using it
    // here inflated the gbuffer / lighting / tonemap buckets several-fold in the
    // first run. The final image is copied to a shared staging texture ONCE,
    // after the timed loop, so readback never taxes the measurement.
    auto mkTarget = [&](MTLPixelFormat fmt, MTLStorageMode mode) -> id<MTLTexture> {
        MTLTextureDescriptor *td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                                              width:NSUInteger(W)
                                                             height:NSUInteger(H)
                                                          mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = mode;
        return [dev newTextureWithDescriptor:td];
    };
    id<MTLTexture> gAlbedo = mkTarget(MTLPixelFormatRGBA8Unorm,  MTLStorageModePrivate);
    id<MTLTexture> gNormal = mkTarget(MTLPixelFormatRG16Snorm,   MTLStorageModePrivate);
    id<MTLTexture> gParams = mkTarget(MTLPixelFormatRGBA8Unorm,  MTLStorageModePrivate);
    // RGBA8Uint, not RG8Uint: .x mirror panel id, .y metalness*255, and since
    // the env-reflection work .z the 1-based ENV PROBE index and .w F0*255.
    id<MTLTexture> gMirror = mkTarget(MTLPixelFormatRGBA8Uint,   MTLStorageModePrivate);
    id<MTLTexture> gDepth  = mkTarget(MTLPixelFormatDepth32Float, MTLStorageModePrivate);
    id<MTLTexture> hdrTex  = mkTarget(MTLPixelFormatRGBA16Float, MTLStorageModePrivate);
    id<MTLTexture> ldrTex  = mkTarget(MTLPixelFormatBGRA8Unorm,  MTLStorageModePrivate);
    id<MTLTexture> stageTex = mkTarget(MTLPixelFormatBGRA8Unorm, MTLStorageModeShared);
    // Transparent peel scratch: the per-layer resolved depth, and the previous
    // layer's resolved depth (the CPU's g_xparPeelFloor). Both are depth
    // textures so the resolve can render into them AND the shader can read them.
    auto mkDepthRW = [&](void) -> id<MTLTexture> {
        MTLTextureDescriptor *td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                               width:NSUInteger(W)
                                                              height:NSUInteger(H)
                                                           mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        return [dev newTextureWithDescriptor:td];
    };
    id<MTLTexture> xparDepth = mkDepthRW();
    id<MTLTexture> xparFloor = mkDepthRW();

    // ---- mirror reflection targets -----------------------------------------
    // One full-res lit HDR reflection per active mirror panel (greets has 3),
    // rendered through the SAME deferred pipeline from the plane-reflected
    // camera, plus one shared reflection G-buffer set reused sequentially
    // (Metal's hazard tracking orders the encoders). Full resolution because
    // the composite is a same-pixel read — a reflected point on the mirror
    // plane projects to the SAME pixel in both views — and because the CPU
    // renders its first-order clones at main resolution too.
    constexpr int kMaxMirrors = 4;
    const int nMirrors = std::min<int>(int(scene.mirrors.size()), kMaxMirrors);
    std::vector<id<MTLTexture>> reflHdr;
    id<MTLTexture> mAlbedo = nil, mNormal = nil, mParams = nil, mMirror = nil,
                   mDepth = nil;
    if (nMirrors > 0) {
        for (int i = 0; i < nMirrors; ++i)
            reflHdr.push_back(mkTarget(MTLPixelFormatRGBA16Float, MTLStorageModePrivate));
        mAlbedo = mkTarget(MTLPixelFormatRGBA8Unorm,   MTLStorageModePrivate);
        mNormal = mkTarget(MTLPixelFormatRG16Snorm,    MTLStorageModePrivate);
        mParams = mkTarget(MTLPixelFormatRGBA8Unorm,   MTLStorageModePrivate);
        mMirror = mkTarget(MTLPixelFormatRGBA8Uint,    MTLStorageModePrivate);
        mDepth  = mkTarget(MTLPixelFormatDepth32Float, MTLStorageModePrivate);
    }
    // ---- ENVIRONMENT PROBE CUBES -------------------------------------------
    // One mipmapped HDR texturecube per probe, baked ONCE from the lit scene
    // through this arm's own G-buffer + lighting pipeline. See the DECISION
    // comment in SceneIngest.cpp: the probes are GPU-baked rather than pulled
    // from the CPU's EnvReflection_Table, because the CPU bakes its probes with
    // the software deferred rasterizer and importing that would put the CPU
    // renderer inside the GPU cost being measured.
    constexpr int kMaxEnvProbes = 8;
    const int envRes = (opt.loadOpt && opt.loadOpt->envRes > 0) ? opt.loadOpt->envRes : 128;
    const int nProbes = std::min<int>(int(scene.envProbes.size()), kMaxEnvProbes);
    std::vector<id<MTLTexture>> envCubes;
    id<MTLTexture> eAlbedo = nil, eNormal = nil, eParams = nil, eMirror = nil,
                   eDepth = nil, eHdr = nil;
    auto mkSquare = [&](MTLPixelFormat fmt, int res) -> id<MTLTexture> {
        MTLTextureDescriptor *td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                                              width:NSUInteger(res)
                                                             height:NSUInteger(res)
                                                          mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        return [dev newTextureWithDescriptor:td];
    };
    if (nProbes > 0) {
        for (int i = 0; i < nProbes; ++i) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor
                textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                size:NSUInteger(envRes)
                                           mipmapped:YES];
            td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
            td.storageMode = MTLStorageModePrivate;
            envCubes.push_back([dev newTextureWithDescriptor:td]);
        }
        eAlbedo = mkSquare(MTLPixelFormatRGBA8Unorm,   envRes);
        eNormal = mkSquare(MTLPixelFormatRG16Snorm,    envRes);
        eParams = mkSquare(MTLPixelFormatRGBA8Unorm,   envRes);
        eMirror = mkSquare(MTLPixelFormatRGBA8Uint,    envRes);
        eDepth  = mkSquare(MTLPixelFormatDepth32Float, envRes);
        eHdr    = mkSquare(MTLPixelFormatRGBA16Float,  envRes);
    }
    id<MTLTexture> dummyEnvCube;
    {
        MTLTextureDescriptor *td = [MTLTextureDescriptor
            textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                            size:1 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        dummyEnvCube = [dev newTextureWithDescriptor:td];
    }

    id<MTLTexture> dummyRefl;
    {
        MTLTextureDescriptor *td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                              width:1 height:1 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        dummyRefl = [dev newTextureWithDescriptor:td];
    }
    // Quarter-res bloom ping-pong (DS=4, exactly Hdr.cpp's constant).
    const int BW = (W + 3) / 4, BH = (H + 3) / 4;
    auto mkBloom = [&]() -> id<MTLTexture> {
        MTLTextureDescriptor *td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                              width:NSUInteger(BW)
                                                             height:NSUInteger(BH)
                                                          mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        return [dev newTextureWithDescriptor:td];
    };
    id<MTLTexture> bloomA = mkBloom(), bloomB = mkBloom();

    // ---- shadow cubes (omnis) + single maps (spots) -----------------------
    // A Light_SpotLight bakes ONE perspective depth map, not six cube faces —
    // FDS/RENDER/Shadows.cpp treats `cubeFace < 0` as the spot path. Matching
    // that matters for cost as well as correctness: 10 disco spots as cubes would
    // be 60 extra faces per frame instead of 10.
    std::vector<id<MTLTexture>> cubes;
    std::vector<bool> cubeIsMoving;
    std::vector<id<MTLTexture>> spots;
    std::vector<int> lightCube(scene.lights.size(), -1);
    std::vector<float> cubeNear(scene.lights.size(), 0.05f);
    std::vector<float> cubeFar(scene.lights.size(), 1.0f);
    if (opt.shadows) {
        for (size_t i = 0; i < scene.lights.size(); ++i) {
            const Light &L = scene.lights[i];
            if (!std::isfinite(L.pos[0]) || L.range <= 0.0f) continue;
            if (!L.castsShadow) continue;
            if (L.isSpot) {
                if (int(spots.size()) >= kMaxSpotMaps) continue;
                const int res = L.shadowRes > 0 ? L.shadowRes : 256;
                MTLTextureDescriptor *td =
                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                      width:NSUInteger(res)
                                                                     height:NSUInteger(res)
                                                                  mipmapped:NO];
                td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                td.storageMode = MTLStorageModePrivate;
                lightCube[i] = int(spots.size());
                cubeNear[i] = 0.05f;
                cubeFar[i] = L.range * opt.lightRangeScale;
                spots.push_back([dev newTextureWithDescriptor:td]);
                out.shadowFaces += 1;
                out.shadowTexels += long(res) * res;
                continue;
            }
            if (int(cubes.size()) >= kMaxShadowCubes) break;
            // greets flags every FLD omni as a caster; static ones bake at
            // greets_omni_shadow_res (512), mech-parented "moving" ones at
            // greets_moving_omni_shadow_res (128).
            const int res = L.parented ? opt.movingShadowRes : opt.staticShadowRes;
            MTLTextureDescriptor *td = [MTLTextureDescriptor new];
            td.textureType = MTLTextureTypeCube;
            td.pixelFormat = MTLPixelFormatDepth32Float;
            td.width = td.height = NSUInteger(res);
            td.mipmapLevelCount = 1;
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            // Private for measurement (Shared disables lossless compression); the
            // --dump_cube diagnostic needs CPU-visible storage, and that run is
            // never a timing run.
            td.storageMode = (opt.dumpCube >= 0 || opt.probe) ? MTLStorageModeShared
                                                              : MTLStorageModePrivate;
            id<MTLTexture> c = [dev newTextureWithDescriptor:td];
            lightCube[i] = int(cubes.size());
            cubeNear[i] = 0.05f;
            // The cube's far plane MUST track lightRangeScale. Scaling a light's
            // reach without re-scaling the frustum it was baked in puts every
            // surface beyond the original range outside the baked depth, which
            // reads as "occluded" -- it looked like a broken shadow tap when it
            // was a broken test.
            cubeFar[i] = L.range * opt.lightRangeScale;
            cubes.push_back(c);
            cubeIsMoving.push_back(L.parented);
            if (L.parented) ++out.movingCubes;
            out.shadowFaces += 6;
            out.shadowTexels += 6L * res * res;
        }
    }
    out.shadowCubes = int(cubes.size());
    // Every slot of the shader's texture array must be bound.
    id<MTLTexture> dummyCube;
    {
        MTLTextureDescriptor *td = [MTLTextureDescriptor new];
        td.textureType = MTLTextureTypeCube;
        td.pixelFormat = MTLPixelFormatDepth32Float;
        td.width = td.height = 1;
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        dummyCube = [dev newTextureWithDescriptor:td];
    }
    id<MTLTexture> dummy2D;
    {
        MTLTextureDescriptor *td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                              width:1 height:1 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        dummy2D = [dev newTextureWithDescriptor:td];
    }

    // ---- uniforms ---------------------------------------------------------
    FrameUniforms fu{};
    // Everything view-dependent lives in this lambda so the interactive window can
    // re-run it every frame without duplicating the derivation.
    auto refreshFrameUniforms = [&]() {
        for (int c = 0; c < 3; ++c) {
            fu.camRow0[c] = scene.camera.rot[0][c];
            fu.camRow1[c] = scene.camera.rot[1][c];
            fu.camRow2[c] = scene.camera.rot[2][c];
            fu.camSrc[c] = scene.camera.src[c];
        }
    };
    refreshFrameUniforms();
    fu.sx = 2.0f * scene.camera.perspX / float(W);
    fu.ox = 2.0f * scene.camera.cntrEX / float(W) - 1.0f;
    fu.sy = 2.0f * scene.camera.perspY / float(H);
    fu.oy = 1.0f - 2.0f * scene.camera.cntrEY / float(H);
    fu.invSx = 1.0f / fu.sx;
    fu.invSy = 1.0f / fu.sy;
    fu.nearZ = scene.camera.nearZ;
    fu.farZ = scene.camera.farZ;
    fu.dza = -fu.nearZ / (fu.farZ - fu.nearZ);
    fu.dzb = fu.nearZ * fu.farZ / (fu.farZ - fu.nearZ);
    fu.exposure = opt.exposure;
    fu.shadowsOn = opt.shadows ? 1u : 0u;
    // GREETS.CPP ~3005, verified by reading. Applies to the SH branch only.
    fu.ambientFactor = 0.25f;
    // Which ambient branch of the CPU kernel this scene runs — see
    // Scene::shAmbient. greets: SH (sh_ambient setDefault true). Everything
    // else: the flat Sc->Ambient constant. fountain's authored sky gradient is
    // (0,0,0), so the SH branch gave it zero ambient and the frame went black.
    fu.flatAmbient[0] = scene.ambient[0] * (1.0f / 255.0f);
    fu.flatAmbient[1] = scene.ambient[1] * (1.0f / 255.0f);
    fu.flatAmbient[2] = scene.ambient[2] * (1.0f / 255.0f);
    fu.flatAmbient[3] = scene.shAmbient ? 0.0f : 1.0f;
    fu.diffuseFactor = 1.0f;
    fu.specularFactor = 1.0f;
    fu.lightRangeScale = opt.lightRangeScale;
    fu.vizLight = opt.vizLight;
    // env_refl_gain, FeatureFlags.def:142, default 1.0.
    fu.envReflGain = 1.0f;
    fu.metalCompat[0] = opt.cpuMetalDiffuse ? 1.0f : 0.0f;
    fu.metalCompat[1] = opt.cpuMetalTint ? 1.0f : 0.0f;
    for (int c = 0; c < 3; ++c) {
        fu.aabbMin[c] = scene.aabbMin[c];
        fu.aabbMax[c] = scene.aabbMax[c];
    }
    for (size_t i = 0; i < scene.envProbes.size() && i < 8; ++i)
        for (int c = 0; c < 3; ++c) fu.envProbePos[i][c] = scene.envProbes[i].pos[c];

    // ---- volumetric cone constants (DeferredVolumetric.cpp:1797-1802) -------
    //   density = cone_strength * 1e-3, and under --hdr (which greets runs)
    //   with --no-hdr_cone_softknee (the default) it is scaled by
    //   hdr_glow_scale. greets' cone_strength is 1.2 from GreetsDisco.cpp,
    //   NOT the global 0.05 default.
    // The light colour needs no conversion: the CPU's cone adds
    // w * (O->L.rgb * ISize) into a 0..255 HDR buffer, this arm's GpuLight
    // colour is (O->L.rgb/255 * ISize) into a 0..1 HDR buffer, so the same
    // `density` lands the same relative brightness.
    ConeUniforms coneU{};
    coneU.density   = opt.coneStrength * 0.001f * opt.hdrGlowScale;
    coneU.nSamples  = float(opt.volNSamples);
    // The CPU's fade window floor is "12 z-buffer quanta" — a z16 staircase
    // remedy. This arm's depth is float, so carry a small absolute floor in
    // the same world-unit ballpark (12 * FZP*1.1/0xFF00) instead of pretending
    // to a quantum that does not exist here.
    coneU.fadeFloor = 12.0f * (scene.camera.farZ * 1.1f) / 65280.0f;
    int nSpotLights = 0;
    for (const auto &l : scene.lights) if (l.isSpot) ++nSpotLights;
    // clipPlane stays all-zero (disabled) for the main view; the reflection
    // passes carry their own FrameUniforms copy with the mirror plane set.
    fu.mirrorCount = uint32_t(nMirrors);

    std::vector<GpuLight> lights;
    std::vector<const char *> lightOrigin;
    for (size_t i = 0; i < scene.lights.size(); ++i) {
        const Light &L = scene.lights[i];
        if (!std::isfinite(L.pos[0]) || L.range <= 0.0f) continue;
        GpuLight g{};
        for (int c = 0; c < 3; ++c) {
            g.pos[c] = L.pos[c];
            // NOT squared. The CPU treats the authored 0..255 light colour as
            // LINEAR radiance at power 1: the view-light list is
            // `colR = O->L.R * O->ISize` (DeferredSurfaceKernel.cpp:5551-5553)
            // and the --hdr_linear composite is `rl = (albedo/255)^2 * l + s`
            // (ibid. 2618-2631) — the albedo is linearised, the light is not.
            // Squaring here halved every 128-valued channel (the mech omnis'
            // G) against the CPU. Independent confirmation of the convention:
            // the SH ambient projects the UNSQUARED sky gradient and matched
            // the CPU base within 1.4%.
            const float c01 = L.color[c] / 255.0f;
            g.color[c] = c01 * L.intensity;
        }
        g.range = L.range;
        g.invRange = 1.0f / L.range;
        g.shadowIndex = lightCube[i];
        g.shadowNear = cubeNear[i];
        g.shadowFar = cubeFar[i];
        g.isSpot = L.isSpot ? 1 : 0;
        g.cosInner = L.cosInner;
        g.cosOuter = L.cosOuter;
        for (int c = 0; c < 3; ++c) g.dir[c] = L.dir[c];
        for (int c = 0; c < 3; ++c) {
            g.sRow0[c] = L.shadowRot[0][c];
            g.sRow1[c] = L.shadowRot[1][c];
            g.sRow2[c] = L.shadowRot[2][c];
        }
        // sRow0.w carries 1/tan(halfFov) — the same projection scale the bake's
        // vertex shader applies, so the tap cannot drift from the bake.
        g.sRow0[3] = 1.0f / std::max(L.shadowTanHalfFov, 1e-4f);
        lights.push_back(g);
        lightOrigin.push_back(L.origin ? L.origin : "?");
    }
    std::fprintf(stderr, "[DEFERRED] LIGHT INVENTORY (%zu usable of %zu in scene):\n",
                 lights.size(), scene.lights.size());
    for (size_t i = 0; i < lights.size(); ++i) {
        // Nearest-geometry probe. A shadow cube whose every face reads ~0.25 units
        // means the omni is INSIDE something; that is a scene fact, not a bake bug,
        // and it is far cheaper to establish here than from the baked depth.
        float nearest = 1e30f;
        const char *nearestMesh = "-", *nearestMat = "-";
        long within = 0;
        for (const auto &b : scene.batches) {
            for (uint32_t v = b.firstVertex; v < b.firstVertex + b.vertexCount; ++v) {
                const Vertex &V = scene.verts[v];
                const float o[3] = {V.px, V.py, V.pz};
                float w[3];
                for (int c = 0; c < 3; ++c)
                    w[c] = b.rot[c][0]*o[0] + b.rot[c][1]*o[1] + b.rot[c][2]*o[2] + b.pos[c];
                const float dx = w[0]-lights[i].pos[0], dy = w[1]-lights[i].pos[1],
                            dz = w[2]-lights[i].pos[2];
                const float d = std::sqrt(dx*dx + dy*dy + dz*dz);
                if (d < nearest) { nearest = d; nearestMesh = b.meshName.c_str();
                                   nearestMat = b.materialName.c_str(); }
                if (d < 1.0f) ++within;
            }
        }
        std::fprintf(stderr,
            "[DEFERRED]   [%zu] %-10s pos=(%7.2f,%6.2f,%8.2f) rgb=(%.3f,%.3f,%.3f) "
            "range=%5.1f slot=%d %s  nearestGeom=%.3f (%s/%s) vertsWithin1u=%ld\n",
            i, lightOrigin[i], lights[i].pos[0], lights[i].pos[1], lights[i].pos[2],
            lights[i].color[0], lights[i].color[1], lights[i].color[2],
            lights[i].range, lights[i].shadowIndex,
            lights[i].shadowIndex < 0 ? "(no shadow)"
              : (lights[i].isSpot ? "(spot 2D map)"
                 : (cubeIsMoving[size_t(lights[i].shadowIndex)] ? "(moving 128^2)"
                                                                : "(static 512^2)")),
            nearest, nearestMesh, nearestMat, within);
    }
    fu.numLights = uint32_t(lights.size());
    out.litLights = int(lights.size());
    if (lights.empty()) { std::fprintf(stderr, "[DEFERRED] no usable lights\n"); return false; }
    id<MTLBuffer> lightBuf = [dev newBufferWithBytes:lights.data()
                                             length:lights.size() * sizeof(GpuLight)
                                            options:MTLResourceStorageModeShared];
    // Flare list. One draw per flaring omni (<= 10 here) rather than instancing:
    // each flare has its own generated texture, and 10 draws is not a cost worth
    // an argument-buffer for.
    struct FlareInst { float wpos[3]; float worldHalf; int tex; };
    std::vector<FlareInst> flares;
    // THE FROZEN-MECH-OMNI BUG (reported 2026-08-08: "mech omnis are staying in
    // place"). This list used to be built ONCE here, outside the frame loop, and
    // nothing rewrote it — so the flare SPRITES stayed at their load-time world
    // positions forever while everything else animated. That is exactly what a
    // viewer sees as "the mech omnis don't move": §6.2b established that the
    // reference's bright blue-white pools ARE the flare sprites, not omni
    // surface lighting, so a frozen sprite list freezes the visible light even
    // though the GpuLight buffer underneath was being refreshed correctly every
    // frame. Rebuilding it per frame is now part of refreshLightBuffer's
    // contract rather than a separate step someone can forget again.
    auto rebuildFlares = [&]() {
        flares.clear();
        for (const auto &L : scene.lights) {
            if (L.flareTexIndex < 0 || L.flareSize <= 0.0f) continue;
            if (!std::isfinite(L.pos[0])) continue;
            FlareInst fi{};
            for (int c = 0; c < 3; ++c) fi.wpos[c] = L.pos[c];
            // Half-extent in px = 2 * ImageSize * perspX * flareSize / z; fold
            // the z-independent part here, divide by view z at draw time.
            // perspX is re-read each rebuild because the authored FOV spline can
            // change it (greets' has one key, but a posed arm's does not have to).
            fi.worldHalf = 2.0f * scene.imageSize * scene.camera.perspX * L.flareSize;
            fi.tex = L.flareTexIndex;
            flares.push_back(fi);
        }
    };
    rebuildFlares();

    // Per-frame light refresh for the interactive window: the mech omnis ride the
    // hierarchy and the 10 disco spots rotate, so position/direction/intensity and
    // the spot shadow basis all change every tick. Cube/map SLOT assignment does
    // not -- Reanimate rebuilds scene.lights in the same order and length, so
    // lightCube[] stays valid.
    auto refreshLightBuffer = [&]() {
        size_t k = 0;
        for (size_t i = 0; i < scene.lights.size() && k < lights.size(); ++i) {
            const Light &L = scene.lights[i];
            if (!std::isfinite(L.pos[0]) || L.range <= 0.0f) continue;
            GpuLight &g = lights[k++];
            for (int c = 0; c < 3; ++c) {
                g.pos[c] = L.pos[c];
                // Not squared — see the comment on the initial fill above.
                const float c01 = L.color[c] / 255.0f;
                g.color[c] = c01 * L.intensity;
                g.dir[c] = L.dir[c];
                g.sRow0[c] = L.shadowRot[0][c];
                g.sRow1[c] = L.shadowRot[1][c];
                g.sRow2[c] = L.shadowRot[2][c];
            }
            g.range = L.range;
            g.invRange = 1.0f / L.range;
            g.cosInner = L.cosInner;
            g.cosOuter = L.cosOuter;
            g.sRow0[3] = 1.0f / std::max(L.shadowTanHalfFov, 1e-4f);
        }
        // The flare sprite list rides the SAME per-frame refresh, so a light
        // that moves cannot leave its visible burst behind. See rebuildFlares.
        rebuildFlares();
    };
    std::fprintf(stderr, "[DEFERRED] flare sprites: %zu (ImageSize=%.3f, additive into HDR)\n",
                 flares.size(), scene.imageSize);


    float sh[9][4];
    ProjectSkyGradientToSH(scene.skyZenith, scene.skyNadir, sh);
    id<MTLBuffer> shBuf = [dev newBufferWithBytes:sh length:sizeof(sh)
                                          options:MTLResourceStorageModeShared];

    std::vector<BatchUniforms> bus(scene.batches.size());
    auto refreshBatchUniforms = [&]() {
    for (size_t i = 0; i < scene.batches.size(); ++i) {
        const Batch &b = scene.batches[i];
        BatchUniforms &u = bus[i];
        for (int c = 0; c < 3; ++c) {
            u.rotRow0[c] = b.rot[0][c];
            u.rotRow1[c] = b.rot[1][c];
            u.rotRow2[c] = b.rot[2][c];
            u.objPos[c] = b.pos[c];
            // NOT squared here. The G-buffer carries a GAMMA value for both the
            // textured and untextured cases, and fs_lighting does the single
            // --hdr_linear squaring at the point of use. Squaring here as well
            // made untextured surfaces a different colour space from textured
            // ones.
            u.baseColor[c] = b.baseColor[c];
        }
        u.baseColor[3] = (b.textureIndex >= 0) ? 1.0f : 0.0f;
        u.matParams[0] = b.diffuse;
        u.matParams[1] = b.specular;
        // GGX lobe roughness, the CPU's exact mapping
        // (DeferredSurfaceKernel.cpp:1790-1809): gloss 0 means "authored
        // specular without a Phong exponent" and falls back to 32; then
        // rough = sqrt(2/(gloss+2)), clamped [0.04, 1]. The previous
        // 1 - gloss/128 mapping gave Glossiness=48 a rough of 0.625 where
        // the CPU runs 0.2 — a much wider, dimmer lobe.
        {
            const float gloss = b.glossiness > 0 ? float(b.glossiness) : 32.0f;
            float rough = std::sqrt(2.0f / (gloss + 2.0f));
            if (rough < 0.04f) rough = 0.04f;
            if (rough > 1.0f)  rough = 1.0f;
            u.matParams[2] = rough;
        }
        u.matParams[3] = b.luminosity;
        u.mapFlags[0] = (opt.nmap && b.normalTexIndex >= 0) ? 1.0f : 0.0f;
        u.mapFlags[1] = (b.roughTexIndex >= 0) ? 1.0f : 0.0f;
        u.mapFlags[2] = b.aoInAlpha ? 1.0f : 0.0f;
        u.mapFlags[3] = b.parallaxScale;
        u.misc[0] = float(b.mirrorIndex);
        u.misc[1] = (b.aoTexIndex >= 0) ? 1.0f : 0.0f;
        u.misc[2] = (b.metalTexIndex >= 0) ? 1.0f : 0.0f;
        // AO strength, the CPU's product: --ao_map_strength (2.0) x
        // Material::AoStrength, applied as ao' = 1 - k*(1-ao) on the AMBIENT
        // only (DeferredSurfaceKernel.cpp:1871-1878 + FeatureFlags.def:137).
        u.misc[3] = 2.0f * b.aoStrength;
        u.misc2[0] = float(b.envProbe);
        u.misc2[1] = b.reflection;
        // Transparent composite weights, verbatim from the CPU's blend
        // (DeferredSurfaceKernel.cpp:3514-3532): XparBlendAlpha > 0 selects the
        // lerp, otherwise dw = Transparency*0.01 with a 0.5 fallback for the
        // hand-built xpar materials that author Transparency = 0.
        u.misc2[2] = b.xparBlendAlpha;
        u.misc2[3] = (b.transparency > 0.0f) ? b.transparency * 0.01f : 0.5f;
        // The FORWARD transparent kernel's own material inputs. Luminosity is
        // passed RAW: the transparent composite is `Luminosity*255 + ...` with
        // no cap (ibid. :2996), while matParams[3] rides an RGBA8 plane that
        // saturates at 4 — fountain's 'f in shpere' authors 100.
        u.xpar[0] = b.luminosity;
        u.xpar[1] = b.specular;
        u.xpar[2] = float(b.glossiness > 0 ? b.glossiness : 32u);
        u.xpar[3] = b.additive ? 1.0f : 0.0f;
    }
    };
    refreshBatchUniforms();

    {
        int nCast = 0; long triCast = 0, triAll = 0;
        std::string skipped;
        for (const auto &b : scene.batches) {
            triAll += b.vertexCount / 3;
            if (b.castsShadow) { ++nCast; triCast += b.vertexCount / 3; }
            else { if (!skipped.empty()) skipped += ", "; skipped += b.materialName; }
        }
        std::fprintf(stderr,
            "[DEFERRED] shadow casters: %d of %zu batches, %ld of %ld tris "
            "(CPU parity: Shadows.cpp skips Transparent/Additive/SkipZ + name "
            "lamp|emi). NON-casters: %s\n",
            nCast, scene.batches.size(), triCast, triAll,
            skipped.empty() ? "(none)" : skipped.c_str());
    }

    // ---- per-pass GPU timestamps -----------------------------------------
    // MEASURED on this device: counterSets == ["timestamp"], and
    // supportsCounterSampling(AtStageBoundary) == YES while AtDrawBoundary ==
    // NO. So per-ENCODER is the finest granularity available -- which is exactly
    // "per pass", the granularity the comparison wants.
    id<MTLCounterSet> tsSet = nil;
    for (id<MTLCounterSet> cs in [dev counterSets])
        if ([[cs name] isEqualToString:MTLCommonCounterSetTimestamp]) tsSet = cs;
    const bool haveStageCounters =
        tsSet && [dev supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary];

    const int kPasses = 5;               // shadow, gbuffer, lighting, tonemap, mirror
    id<MTLCounterSampleBuffer> sampleBuf = nil;
    if (haveStageCounters) {
        MTLCounterSampleBufferDescriptor *d = [MTLCounterSampleBufferDescriptor new];
        d.counterSet = tsSet;
        d.sampleCount = NSUInteger(kPasses * 2);
        d.storageMode = MTLStorageModeShared;
        sampleBuf = [dev newCounterSampleBufferWithDescriptor:d error:&err];
        if (!sampleBuf)
            std::fprintf(stderr, "[DEFERRED] counter sample buffer failed: %s\n",
                         [[err localizedDescription] UTF8String]);
    }

    auto attachCounters = [&](MTLRenderPassDescriptor *rp, int passIdx) {
        if (!sampleBuf) return;
        rp.sampleBufferAttachments[0].sampleBuffer = sampleBuf;
        rp.sampleBufferAttachments[0].startOfVertexSampleIndex = NSUInteger(passIdx * 2);
        rp.sampleBufferAttachments[0].endOfFragmentSampleIndex = NSUInteger(passIdx * 2 + 1);
        rp.sampleBufferAttachments[0].endOfVertexSampleIndex = MTLCounterDontSample;
        rp.sampleBufferAttachments[0].startOfFragmentSampleIndex = MTLCounterDontSample;
    };

    // ---- one frame --------------------------------------------------------
    // Per-cube-FACE frustum cull for the shadow bake. `fc` is the face basis
    // (right, up, forward) and `lp` the light position; a batch survives if its
    // world bounding sphere intersects the 90-degree square frustum out to
    // `far`. This is the analogue of the CPU's per-pass mesh cull — without it
    // the GPU rasterises all 27 casting batches into all 6 faces of every cube
    // while the CPU culls per face, and the shadow rows of the comparison table
    // would be measuring different workloads.
    auto sphereInCubeFace = [](const Batch &b, const float lp[3],
                               const CubeFace &fc, float farD) -> bool {
        const float d[3] = {b.bsCtr[0]-lp[0], b.bsCtr[1]-lp[1], b.bsCtr[2]-lp[2]};
        const float z = d[0]*fc.fwd[0] + d[1]*fc.fwd[1] + d[2]*fc.fwd[2];
        if (z < -b.bsRad || z > farD + b.bsRad) return false;
        // 90-degree frustum: the side planes are (fwd +/- right)/sqrt2 and
        // (fwd +/- up)/sqrt2, all through the light.
        const float x = d[0]*fc.right[0] + d[1]*fc.right[1] + d[2]*fc.right[2];
        const float y = d[0]*fc.up[0]    + d[1]*fc.up[1]    + d[2]*fc.up[2];
        const float k = 0.70710678f, r = b.bsRad;
        if ((z - x) * k < -r) return false;
        if ((z + x) * k < -r) return false;
        if ((z - y) * k < -r) return false;
        if ((z + y) * k < -r) return false;
        return true;
    };
    // Shadow-pass cull state, set per face by bakeCubes before drawScene.
    const CubeFace *curFace = nullptr;
    const float *curLightPos = nullptr;
    float curFar = 0.0f;
    long shadowBatchesDrawn = 0, shadowBatchesCulled = 0;

    // ENV PROBE SELF-EXCLUSION. While baking probe P, every batch whose
    // material IS the surface P was baked for is skipped — the CPU's
    // g_envBakeSkipMats, which is FACE-level and matches on the material name
    // with any '::mirUV' suffix stripped (EnvBake.cpp:243-252). Without it a
    // metal bakes the inside of itself and reflects a black shell.
    // Empty = no exclusion (every pass other than a probe bake).
    std::string envSkipMat;
    auto baseMatName = [](const std::string &n) {
        const size_t k = n.rfind("::mirUV");
        return (k != std::string::npos) ? n.substr(0, k) : n;
    };
    // `baseCull` is the pass's cull mode; drawScene overrides it PER BATCH for
    // Mat_TwoSided materials and restores it after. THE BUG THIS FIXES
    // (reported 2026-08-08: "has backface culling for transparent materials"):
    // culling landed in 69bf0f0 as one setCullMode for the whole pass, but the
    // engine's own test has a per-material bypass —
    //   FDS/RENDER/Transform.cpp:2429-2434
    //     if ((!F->VisibilityFlagsAll()) && (forceTS || shadowNoBackface
    //         || (F->Txtr->Flags & Mat_TwoSided)
    //         || (AP·F->N < F->NormProd)))            // backface culling
    // so a Mat_TwoSided face is NEVER culled on the CPU. greets' 'screen2' —
    // the display box between the amudim columns, flags 0x0034 — is exactly
    // such a material, and this arm was dropping half of it. The bypass is
    // keyed on TwoSided, NOT on transparency: 'lamp light' / 'screen 3' /
    // 'screen 4' (0x0024) are single-sided on both arms and culling them is
    // correct parity.
    MTLCullMode curCull = MTLCullModeNone;
    auto drawScene = [&](id<MTLRenderCommandEncoder> enc, bool gbuffer,
                         MTLCullMode baseCull = MTLCullModeNone) {
        [enc setVertexBuffer:vb offset:0 atIndex:0];
        curCull = baseCull;
        for (size_t i = 0; i < scene.batches.size(); ++i) {
            const Batch &b = scene.batches[i];
            if (!envSkipMat.empty() && baseMatName(b.materialName) == envSkipMat) continue;
            // TRANSPARENT + ADDITIVE surfaces are NOT in the opaque G-buffer.
            // That is the CPU's own routing, not a simplification:
            // RenderInner.cpp:294-296 `if (Mat_Transparent) continue;` in
            // RenderInnerMekalele, and :317-318 sends Mat_Additive to the
            // forward TheOtherBarry<ADDITIVE> instead of Mekalele. Both are
            // composited later, by encodeXpar. Mirror-tagged panels stay here:
            // this arm composites their reflection in fs_lighting rather than
            // through the CPU's transparent wallMatClone (a stated deviation).
            if (gbuffer && b.mirrorIndex == 0 && (b.transparent || b.additive)) continue;
            // Shadow pass: honour the same caster filter the CPU bake applies.
            if (!gbuffer && !b.castsShadow) continue;
            if (gbuffer) {
                const MTLCullMode want = b.twoSided ? MTLCullModeNone : baseCull;
                if (want != curCull) { [enc setCullMode:want]; curCull = want; }
            }
            if (!gbuffer && opt.shadowCull && curFace && curLightPos) {
                if (!sphereInCubeFace(b, curLightPos, *curFace, curFar)) {
                    ++shadowBatchesCulled;
                    continue;
                }
                ++shadowBatchesDrawn;
            }
            [enc setVertexBytes:&bus[i] length:sizeof(BatchUniforms) atIndex:2];
            if (gbuffer) {
                [enc setFragmentBytes:&bus[i] length:sizeof(BatchUniforms) atIndex:2];
                id<MTLTexture> a = (b.textureIndex   >= 0) ? texes[size_t(b.textureIndex)]   : texes[0];
                id<MTLTexture> n = (b.normalTexIndex >= 0) ? texes[size_t(b.normalTexIndex)] : texes[0];
                id<MTLTexture> r = (b.roughTexIndex  >= 0) ? texes[size_t(b.roughTexIndex)]  : texes[0];
                id<MTLTexture> ao = (b.aoTexIndex    >= 0) ? texes[size_t(b.aoTexIndex)]     : texes[0];
                id<MTLTexture> mt = (b.metalTexIndex >= 0) ? texes[size_t(b.metalTexIndex)]  : texes[0];
                [enc setFragmentTexture:a atIndex:0];
                [enc setFragmentTexture:n atIndex:1];
                [enc setFragmentTexture:r atIndex:2];
                [enc setFragmentTexture:ao atIndex:3];
                [enc setFragmentTexture:mt atIndex:4];
            }
            [enc drawPrimitives:MTLPrimitiveTypeTriangle
                    vertexStart:NSUInteger(b.firstVertex)
                    vertexCount:NSUInteger(b.vertexCount)];
        }
    };

    // Bake policy MATCHES greets. GreetsApplyInitDefaults sets
    // greets_omni_shadows, which marks every FLD omni Omni_CastsShadow |
    // Omni_StaticShadow -- so the STATIC omnis' cubes are baked ONCE and cached,
    // and only the mech-parented "moving" omnis pay a per-frame re-bake
    // (PERF_STATE.md stage 7, DynamicOmnisPerFrame). An earlier revision here
    // re-baked all 60 faces every frame: wrong for parity, and it starved the GPU
    // on CPU encode, which inflated every later pass's timestamp span.
    auto bakeCubes = [&](id<MTLCommandBuffer> cb, bool movingOnly, bool timed) {
        size_t firstIdx = SIZE_MAX, lastIdx = 0;
        if (timed) {
            for (size_t ci = 0; ci < cubes.size(); ++ci) {
                if (movingOnly && !cubeIsMoving[ci]) continue;
                if (firstIdx == SIZE_MAX) firstIdx = ci;
                lastIdx = ci;
            }
        }
        for (size_t ci = 0; ci < cubes.size(); ++ci) {
            if (movingOnly && !cubeIsMoving[ci]) continue;
            // find the light owning this cube
            size_t li = 0;
            for (size_t k = 0; k < lights.size(); ++k)
                if (!lights[k].isSpot && lights[k].shadowIndex == int(ci)) { li = k; break; }
            const float sn = lights[li].shadowNear, sf = lights[li].shadowFar;
            for (int f = 0; f < 6; ++f) {
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.depthAttachment.texture = cubes[ci];
                rp.depthAttachment.slice = NSUInteger(f);
                rp.depthAttachment.loadAction = MTLLoadActionClear;
                rp.depthAttachment.storeAction = MTLStoreActionStore;
                rp.depthAttachment.clearDepth = 0.0;
                // Time the WHOLE bake wave: start on the first face's encoder,
                // end on the last. Sampling only the first face measured 1 of 60.
                const bool firstFace = timed && (ci == firstIdx && f == 0);
                const bool lastFace  = timed && (ci == lastIdx && f == 5);
                if (sampleBuf && (firstFace || lastFace)) {
                    rp.sampleBufferAttachments[0].sampleBuffer = sampleBuf;
                    rp.sampleBufferAttachments[0].startOfVertexSampleIndex =
                        firstFace ? 0 : MTLCounterDontSample;
                    rp.sampleBufferAttachments[0].endOfFragmentSampleIndex =
                        lastFace ? 1 : MTLCounterDontSample;
                    rp.sampleBufferAttachments[0].endOfVertexSampleIndex = MTLCounterDontSample;
                    rp.sampleBufferAttachments[0].startOfFragmentSampleIndex = MTLCounterDontSample;
                }
                id<MTLRenderCommandEncoder> enc =
                    [cb renderCommandEncoderWithDescriptor:rp];
                [enc setRenderPipelineState:psoShadow];
                [enc setDepthStencilState:dss];
                [enc setCullMode:MTLCullModeNone];
                ShadowUniforms su{};
                for (int c = 0; c < 3; ++c) {
                    su.row0[c] = kCubeFaces[f].right[c];
                    su.row1[c] = kCubeFaces[f].up[c];
                    su.row2[c] = kCubeFaces[f].fwd[c];
                    su.lightPos[c] = lights[li].pos[c];
                }
                su.dza = -sn / (sf - sn);
                su.dzb = sn * sf / (sf - sn);
                su.projScale = 1.0f;          // 90-degree cube face
                [enc setVertexBytes:&su length:sizeof(su) atIndex:1];
                curFace = &kCubeFaces[f];
                curLightPos = lights[li].pos;
                curFar = sf;
                drawScene(enc, /*gbuffer=*/false);
                curFace = nullptr; curLightPos = nullptr;
                [enc endEncoding];
            }
        }
    };

    // Spot maps: ONE perspective depth map each, re-baked EVERY FRAME because the
    // disco spots rotate (GreetsDisco's UpdateDiscoBall rewrites IPos/IDir per
    // tick, and Shadows.cpp re-bakes them on the DynamicOmnisPerFrame path). The
    // 10% FOV pad and the Kick_Camera basis both come from the ingest, which built
    // them with the engine's own call — so bake and tap cannot drift apart.
    auto bakeSpotMaps = [&](id<MTLCommandBuffer> cb) {
        for (size_t si = 0; si < spots.size(); ++si) {
            size_t li = SIZE_MAX;
            for (size_t k = 0; k < lights.size(); ++k)
                if (lights[k].isSpot && lights[k].shadowIndex == int(si)) { li = k; break; }
            if (li == SIZE_MAX) continue;
            const float sn = lights[li].shadowNear, sf = lights[li].shadowFar;
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.depthAttachment.texture = spots[si];
            rp.depthAttachment.loadAction = MTLLoadActionClear;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            rp.depthAttachment.clearDepth = 0.0;
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            [enc setRenderPipelineState:psoShadow];
            [enc setDepthStencilState:dss];
            [enc setCullMode:MTLCullModeNone];
            ShadowUniforms su{};
            for (int c = 0; c < 3; ++c) {
                su.row0[c] = lights[li].sRow0[c];
                su.row1[c] = lights[li].sRow1[c];
                su.row2[c] = lights[li].sRow2[c];
                su.lightPos[c] = lights[li].pos[c];
            }
            su.dza = -sn / (sf - sn);
            su.dzb = sn * sf / (sf - sn);
            su.projScale = lights[li].sRow0[3];
            [enc setVertexBytes:&su length:sizeof(su) atIndex:1];
            drawScene(enc, /*gbuffer=*/false);
            [enc endEncoding];
        }
    };

    // One-time STATIC bake, outside the timed loop, as greets caches it.
    {
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        bakeCubes(cb, /*movingOnly=*/false, /*timed=*/false);
        bakeSpotMaps(cb);
        // Clear the 1x1 dummy reflection to black: it is bound for INACTIVE
        // mirrors (camera behind the plane), whose panels then composite +0
        // instead of stale or undefined texels.
        {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = dummyRefl;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
            [e endEncoding];
        }
        [cb commit];
        [cb waitUntilCompleted];
        out.staticBakeMs = ([cb GPUEndTime] - [cb GPUStartTime]) * 1000.0;
    }

    // ---- --dump_cube: read the baked depth back and LOOK at it -------------
    if (opt.dumpCube >= 0) {
        if (opt.dumpCube >= int(lights.size())) {
            std::fprintf(stderr, "[DUMPCUBE] light %d out of range (%zu lights)\n",
                         opt.dumpCube, lights.size());
        } else if (lights[size_t(opt.dumpCube)].shadowIndex < 0
                   || lights[size_t(opt.dumpCube)].isSpot) {
            std::fprintf(stderr, "[DUMPCUBE] light %d has no CUBE (spot=%d, slot=%d)\n",
                         opt.dumpCube, lights[size_t(opt.dumpCube)].isSpot,
                         lights[size_t(opt.dumpCube)].shadowIndex);
        } else {
            const size_t ci = size_t(lights[size_t(opt.dumpCube)].shadowIndex);
            id<MTLTexture> cube = cubes[ci];
            const int res = int([cube width]);
            const float sn = lights[size_t(opt.dumpCube)].shadowNear;
            const float sf = lights[size_t(opt.dumpCube)].shadowFar;
            const float dza = -sn / (sf - sn), dzb = sn * sf / (sf - sn);
            static const char *fnames[6] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
            std::vector<float> face(size_t(res) * size_t(res));
            // 3x2 atlas: row 0 = +X -X +Y, row 1 = -Y +Z -Z
            std::vector<uint8_t> atlas(size_t(res) * 3 * size_t(res) * 2 * 4, 0);
            const size_t aw = size_t(res) * 3;
            std::fprintf(stderr,
                "[DUMPCUBE] light %d cube %zu res=%d near=%.4f far=%.4f "
                "(dza=%.6f dzb=%.6f)\n",
                opt.dumpCube, ci, res, sn, sf, dza, dzb);
            for (int f = 0; f < 6; ++f) {
                [cube getBytes:face.data()
                   bytesPerRow:NSUInteger(res) * sizeof(float)
                 bytesPerImage:face.size() * sizeof(float)
                    fromRegion:MTLRegionMake2D(0, 0, NSUInteger(res), NSUInteger(res))
                   mipmapLevel:0
                         slice:NSUInteger(f)];
                long nNan = 0, nCleared = 0, nValid = 0;
                float mn = 1e30f, mx = -1e30f, dmn = 1e30f, dmx = -1e30f;
                double dsum = 0.0;
                for (float v : face) {
                    if (!std::isfinite(v)) { ++nNan; continue; }
                    if (v <= 0.0f) { ++nCleared; continue; }
                    ++nValid;
                    mn = std::min(mn, v); mx = std::max(mx, v);
                    const float dist = dzb / (v - dza);
                    dmn = std::min(dmn, dist); dmx = std::max(dmx, dist);
                    dsum += dist;
                }
                std::fprintf(stderr,
                    "[DUMPCUBE]   %s  valid=%7ld (%5.1f%%)  cleared=%7ld  nonfinite=%ld"
                    "  storedZ[%.6f..%.6f]  dist[%.3f..%.3f] mean=%.3f\n",
                    fnames[f], nValid, 100.0 * double(nValid) / double(face.size()),
                    nCleared, nNan,
                    nValid ? mn : 0.0f, nValid ? mx : 0.0f,
                    nValid ? dmn : 0.0f, nValid ? dmx : 0.0f,
                    nValid ? dsum / double(nValid) : 0.0);
                // 8x8 grid of DECODED WORLD DISTANCE across the face. min/max
                // alone hid the thing that mattered here: a face can be 100%
                // valid, span 0.5..30 units, and still be a near wall over most
                // of its solid angle. The grid shows WHERE the occluder is.
                // Row 0 is texel row 0 = the +up edge (Metal's origin is
                // upper-left), column 0 the -right edge.
                for (int gy = 0; gy < 8; ++gy) {
                    std::string line;
                    char cell[16];
                    for (int gx = 0; gx < 8; ++gx) {
                        const int sx = (2 * gx + 1) * res / 16;
                        const int sy = (2 * gy + 1) * res / 16;
                        const float v = face[size_t(sy) * size_t(res) + size_t(sx)];
                        if (!std::isfinite(v))   std::snprintf(cell, sizeof cell, "  nan ");
                        else if (v <= 0.0f)      std::snprintf(cell, sizeof cell, "  --- ");
                        else std::snprintf(cell, sizeof cell, "%6.2f", dzb / (v - dza));
                        line += cell;
                    }
                    std::fprintf(stderr, "[DUMPCUBE]     %s|%s\n",
                                 gy == 0 ? fnames[f] : "  ", line.c_str());
                }
                // atlas tile: distance ramped over [near, far]; magenta = nonfinite,
                // dark blue = cleared (nothing rendered into this texel)
                const size_t tx = size_t(f % 3) * size_t(res);
                const size_t ty = size_t(f / 3) * size_t(res);
                for (int y = 0; y < res; ++y)
                    for (int x = 0; x < res; ++x) {
                        const float v = face[size_t(y) * size_t(res) + size_t(x)];
                        uint8_t r, g, b;
                        if (!std::isfinite(v)) { r = 255; g = 0; b = 255; }
                        else if (v <= 0.0f)    { r = 10;  g = 15; b = 60; }
                        else {
                            const float dist = dzb / (v - dza);
                            const float t = std::min(1.0f, std::max(0.0f,
                                                (dist - sn) / (sf - sn)));
                            const uint8_t g8 = uint8_t(255.0f * (1.0f - t));
                            r = g8; g = g8; b = g8;
                        }
                        uint8_t *p = &atlas[((ty + size_t(y)) * aw + tx + size_t(x)) * 4];
                        p[0] = b; p[1] = g; p[2] = r; p[3] = 255;   // BGRA, WritePPM order
                    }
            }
            const std::string dp = opt.dumpCubePath.empty()
                                 ? std::string("gpubench_cube.ppm") : opt.dumpCubePath;
            if (WritePPM(dp, atlas.data(), int(aw), res * 2, aw * 4))
                std::fprintf(stderr, "[DUMPCUBE] wrote %s (3x2 face atlas, "
                                     "white=near black=far, magenta=nonfinite, "
                                     "navy=cleared)\n", dp.c_str());
        }
    }

    // ---- --probe_px: why is THIS pixel not lit by light N ------------------
    // Builds the pixel's camera ray from the same fu.sx/ox/sy/oy the vertex
    // shader projects with, ray-casts ALL geometry for the visible surface and
    // its geometric normal, then replays the lighting pass's own per-light gate
    // and names the test that failed.
    if (opt.probePx) {
        const int PX = opt.probePxXY[0], PY = opt.probePxXY[1];
        const float ndcx = 2.0f * (float(PX) + 0.5f) / float(W) - 1.0f;
        const float ndcy = 1.0f - 2.0f * (float(PY) + 0.5f) / float(H);
        const float vd[3] = {(ndcx - fu.ox) * fu.invSx, (ndcy - fu.oy) * fu.invSy, 1.0f};
        float rd[3];
        for (int c = 0; c < 3; ++c)
            rd[c] = scene.camera.rot[0][c] * vd[0] + scene.camera.rot[1][c] * vd[1]
                  + scene.camera.rot[2][c] * vd[2];
        const float rl = std::sqrt(rd[0]*rd[0] + rd[1]*rd[1] + rd[2]*rd[2]);
        for (int c = 0; c < 3; ++c) rd[c] /= rl;
        const float ro[3] = {scene.camera.src[0], scene.camera.src[1], scene.camera.src[2]};
        float bestT = 1e30f, bestN[3] = {0,0,0};
        std::string bm = "-", bmat = "-";
        for (const auto &b : scene.batches) {
            for (uint32_t v = b.firstVertex; v + 2 < b.firstVertex + b.vertexCount; v += 3) {
                float w[3][3];
                for (int k = 0; k < 3; ++k) {
                    const Vertex &V = scene.verts[v + uint32_t(k)];
                    const float ob[3] = {V.px, V.py, V.pz};
                    for (int c = 0; c < 3; ++c)
                        w[k][c] = b.rot[c][0]*ob[0] + b.rot[c][1]*ob[1]
                                + b.rot[c][2]*ob[2] + b.pos[c];
                }
                float e1[3], e2[3], pv[3], tv[3], qv[3];
                for (int c = 0; c < 3; ++c) {
                    e1[c] = w[1][c]-w[0][c]; e2[c] = w[2][c]-w[0][c]; tv[c] = ro[c]-w[0][c];
                }
                pv[0]=rd[1]*e2[2]-rd[2]*e2[1]; pv[1]=rd[2]*e2[0]-rd[0]*e2[2]; pv[2]=rd[0]*e2[1]-rd[1]*e2[0];
                const float det = e1[0]*pv[0]+e1[1]*pv[1]+e1[2]*pv[2];
                if (std::fabs(det) < 1e-12f) continue;
                const float inv = 1.0f/det;
                const float uu = (tv[0]*pv[0]+tv[1]*pv[1]+tv[2]*pv[2])*inv;
                if (uu < 0.0f || uu > 1.0f) continue;
                qv[0]=tv[1]*e1[2]-tv[2]*e1[1]; qv[1]=tv[2]*e1[0]-tv[0]*e1[2]; qv[2]=tv[0]*e1[1]-tv[1]*e1[0];
                const float vv = (rd[0]*qv[0]+rd[1]*qv[1]+rd[2]*qv[2])*inv;
                if (vv < 0.0f || uu+vv > 1.0f) continue;
                const float t = (e2[0]*qv[0]+e2[1]*qv[1]+e2[2]*qv[2])*inv;
                if (t > 1e-4f && t < bestT) {
                    bestT = t; bm = b.meshName; bmat = b.materialName;
                    // FDS winding: Compute_Face_Normals (PREPROC.CPP:29) does
                    // Cross_Product(V, U) with U=B-A, V=C-A, i.e. N = e2 x e1 —
                    // the NEGATION of the usual e1 x e2. Getting this backwards
                    // made every room surface look outward-facing and sent this
                    // investigation down a blind alley for a round.
                    bestN[0]=e2[1]*e1[2]-e2[2]*e1[1];
                    bestN[1]=e2[2]*e1[0]-e2[0]*e1[2];
                    bestN[2]=e2[0]*e1[1]-e2[1]*e1[0];
                    const float nl = std::sqrt(bestN[0]*bestN[0]+bestN[1]*bestN[1]+bestN[2]*bestN[2]);
                    if (nl > 0.0f) for (int c=0;c<3;++c) bestN[c]/=nl;
                }
            }
        }
        if (bestT > 1e29f) {
            std::fprintf(stderr, "[PROBEPX] px(%d,%d): camera ray hits nothing\n", PX, PY);
        } else {
            const float hp[3] = {ro[0]+rd[0]*bestT, ro[1]+rd[1]*bestT, ro[2]+rd[2]*bestT};
            std::fprintf(stderr,
                "[PROBEPX] px(%d,%d) -> %s/%s at (%.3f,%.3f,%.3f) dist=%.3f "
                "geoNormal=(%.3f,%.3f,%.3f)\n",
                PX, PY, bm.c_str(), bmat.c_str(), hp[0], hp[1], hp[2], bestT,
                bestN[0], bestN[1], bestN[2]);
            for (size_t li = 0; li < lights.size(); ++li) {
                float toL[3] = {lights[li].pos[0]-hp[0], lights[li].pos[1]-hp[1],
                                lights[li].pos[2]-hp[2]};
                const float d = std::sqrt(toL[0]*toL[0]+toL[1]*toL[1]+toL[2]*toL[2]);
                if (d <= 1e-6f) continue;
                const float rr = lights[li].range * opt.lightRangeScale;
                // Two-sided NoL: the sign only tells which face of the polygon
                // the light is on, and the loader does not carry a consistent
                // winding for every authored surface.
                const float NoLs = (toL[0]*bestN[0]+toL[1]*bestN[1]+toL[2]*bestN[2])/d;
                const float atten = std::max(0.0f, 1.0f - d * (lights[li].invRange
                                                               / opt.lightRangeScale));
                const char *verdict = (d >= rr) ? "OUT OF RANGE"
                                    : (NoLs <= 0.0f ? "BACKFACING (NoL<=0)" : "reaches");
                std::fprintf(stderr,
                    "[PROBEPX]   light %2zu %-11s d=%7.3f range=%5.1f NoL=%+.3f atten=%.3f "
                    "col=(%.2f,%.2f,%.2f)\n",
                    li, verdict, d, rr, NoLs, atten,
                    lights[li].color[0], lights[li].color[1], lights[li].color[2]);
            }
        }
    }

    // ---- --probe: ground truth vs the tap, for ONE world point -------------
    // (a) ray-cast the SAME casting triangles the bake rasterised, name the
    //     nearest hit's mesh/material; (b) replicate the shader's cube tap on
    //     the host from the read-back cube. Agreement means the cube is honest
    //     and any occlusion is geometry; disagreement localises the bug to the
    //     tap's conventions. Requires --dump_cube (Shared storage on the cubes).
    if (opt.probe) {
        const float P[3] = {opt.probePoint[0], opt.probePoint[1], opt.probePoint[2]};
        std::fprintf(stderr, "[PROBE] world point (%.3f, %.3f, %.3f)\n", P[0], P[1], P[2]);
        // Moller-Trumbore against every CASTING triangle, in world space.
        auto castRay = [&](const float o[3], const float d[3], float maxT,
                           float &hitT, std::string &mesh, std::string &mat) {
            hitT = maxT; mesh = "-"; mat = "-";
            for (const auto &b : scene.batches) {
                if (!b.castsShadow) continue;
                for (uint32_t v = b.firstVertex; v + 2 < b.firstVertex + b.vertexCount; v += 3) {
                    float w[3][3];
                    for (int k = 0; k < 3; ++k) {
                        const Vertex &V = scene.verts[v + uint32_t(k)];
                        const float ob[3] = {V.px, V.py, V.pz};
                        for (int c = 0; c < 3; ++c)
                            w[k][c] = b.rot[c][0]*ob[0] + b.rot[c][1]*ob[1]
                                    + b.rot[c][2]*ob[2] + b.pos[c];
                    }
                    float e1[3], e2[3], pv[3], tv[3], qv[3];
                    for (int c = 0; c < 3; ++c) {
                        e1[c] = w[1][c] - w[0][c];
                        e2[c] = w[2][c] - w[0][c];
                        tv[c] = o[c] - w[0][c];
                    }
                    pv[0] = d[1]*e2[2] - d[2]*e2[1];
                    pv[1] = d[2]*e2[0] - d[0]*e2[2];
                    pv[2] = d[0]*e2[1] - d[1]*e2[0];
                    const float det = e1[0]*pv[0] + e1[1]*pv[1] + e1[2]*pv[2];
                    if (std::fabs(det) < 1e-12f) continue;   // two-sided, as the bake is
                    const float inv = 1.0f / det;
                    const float u = (tv[0]*pv[0] + tv[1]*pv[1] + tv[2]*pv[2]) * inv;
                    if (u < 0.0f || u > 1.0f) continue;
                    qv[0] = tv[1]*e1[2] - tv[2]*e1[1];
                    qv[1] = tv[2]*e1[0] - tv[0]*e1[2];
                    qv[2] = tv[0]*e1[1] - tv[1]*e1[0];
                    const float vv = (d[0]*qv[0] + d[1]*qv[1] + d[2]*qv[2]) * inv;
                    if (vv < 0.0f || u + vv > 1.0f) continue;
                    const float t = (e2[0]*qv[0] + e2[1]*qv[1] + e2[2]*qv[2]) * inv;
                    if (t > 1e-4f && t < hitT) {
                        hitT = t; mesh = b.meshName; mat = b.materialName;
                    }
                }
            }
        };
        std::vector<float> faceBuf;
        for (size_t li = 0; li < lights.size(); ++li) {
            if (lights[li].isSpot || lights[li].shadowIndex < 0) continue;
            const float L0[3] = {lights[li].pos[0], lights[li].pos[1], lights[li].pos[2]};
            float dw[3] = {P[0]-L0[0], P[1]-L0[1], P[2]-L0[2]};
            const float dist = std::sqrt(dw[0]*dw[0] + dw[1]*dw[1] + dw[2]*dw[2]);
            if (dist <= 1e-5f) continue;
            const float dir[3] = {dw[0]/dist, dw[1]/dist, dw[2]/dist};
            float hitT; std::string hm, hmat;
            castRay(L0, dir, dist * 0.999f, hitT, hm, hmat);
            const bool geomBlocked = hitT < dist * 0.999f;
            // Host replica of the shader tap.
            const float ax = std::fabs(dw[0]), ay = std::fabs(dw[1]), az = std::fabs(dw[2]);
            const float major = std::max(ax, std::max(ay, az));
            int face; float sc, tc;
            if (ax >= ay && ax >= az) { face = dw[0] > 0 ? 0 : 1;
                                        sc = dw[0] > 0 ? -dw[2] : dw[2]; tc = -dw[1]; }
            else if (ay >= az)        { face = dw[1] > 0 ? 2 : 3;
                                        sc = dw[0]; tc = dw[1] > 0 ? dw[2] : -dw[2]; }
            else                      { face = dw[2] > 0 ? 4 : 5;
                                        sc = dw[2] > 0 ? dw[0] : -dw[0]; tc = -dw[1]; }
            const float sn = lights[li].shadowNear, sf = lights[li].shadowFar;
            const float dza = -sn / (sf - sn), dzb = sn * sf / (sf - sn);
            const size_t ci = size_t(lights[li].shadowIndex);
            id<MTLTexture> cube = cubes[ci];
            const int res = int([cube width]);
            const char *storageNote = "";
            float stored = 0.0f, storedDist = -1.0f;
            int tx = -1, ty = -1;
            if ([cube storageMode] == MTLStorageModeShared) {
                faceBuf.assign(size_t(res) * size_t(res), 0.0f);
                [cube getBytes:faceBuf.data()
                   bytesPerRow:NSUInteger(res) * sizeof(float)
                 bytesPerImage:faceBuf.size() * sizeof(float)
                    fromRegion:MTLRegionMake2D(0, 0, NSUInteger(res), NSUInteger(res))
                   mipmapLevel:0 slice:NSUInteger(face)];
                const float u = 0.5f * (sc / major + 1.0f);
                const float v = 0.5f * (tc / major + 1.0f);
                tx = std::min(res - 1, std::max(0, int(u * float(res))));
                ty = std::min(res - 1, std::max(0, int(v * float(res))));
                stored = faceBuf[size_t(ty) * size_t(res) + size_t(tx)];
                storedDist = (stored > dza + 1e-9f) ? dzb / (stored - dza) : 1e9f;
            } else {
                storageNote = " (cube is Private — pass --dump_cube=N for the readback)";
            }
            const float ref = dza + dzb / std::max(major, 1e-5f);
            std::fprintf(stderr,
                "[PROBE]  light %2zu  dist=%7.3f major=%7.3f  RAYCAST: %s"
                "  t=%7.3f (%s/%s)\n",
                li, dist, major, geomBlocked ? "BLOCKED" : "clear  ",
                hitT, hm.c_str(), hmat.c_str());
            std::fprintf(stderr,
                "[PROBE]            CUBE face=%d texel=(%d,%d) stored=%.8f"
                "  storedDist=%8.3f  ref=%.8f  tap says %s%s\n",
                face, tx, ty, stored, storedDist, ref,
                (storedDist < 0.0f) ? "?"
                    : ((ref + 0.0004f > stored) ? "LIT" : "SHADOWED"), storageNote);
        }
    }

    // One deferred G-buffer fill into an arbitrary target set with an
    // arbitrary camera (FrameUniforms). Shared by the main view and the
    // per-mirror reflection views.
    // `mirroredBasis` = this pass's view matrix has determinant -1 (a reflection).
    // See the cull-mode comment below: it MUST invert the cull sense.
    auto encodeGBuffer = [&](id<MTLCommandBuffer> cb, const FrameUniforms &u,
                             id<MTLTexture> tAlb, id<MTLTexture> tNrm,
                             id<MTLTexture> tPar, id<MTLTexture> tMir,
                             id<MTLTexture> tDep,
                             void (^counterHook)(MTLRenderPassDescriptor *),
                             bool mirroredBasis = false) {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = tAlb;
        rp.colorAttachments[1].texture = tNrm;
        rp.colorAttachments[2].texture = tPar;
        rp.colorAttachments[3].texture = tMir;
        for (int i = 0; i < 4; ++i) {
            rp.colorAttachments[i].loadAction = MTLLoadActionClear;
            rp.colorAttachments[i].storeAction = MTLStoreActionStore;
            rp.colorAttachments[i].clearColor = MTLClearColorMake(0, 0, 0, 0);
        }
        rp.depthAttachment.texture = tDep;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        rp.depthAttachment.clearDepth = 0.0;
        if (counterHook) counterHook(rp);
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:psoGBuf];
        [enc setDepthStencilState:dss];
        // BACKFACE CULL, matching Transform_Objects' own test
        // (Transform.cpp:2434). WHICH mode is correct was MEASURED, not
        // reasoned: FDS's Compute_Face_Normals builds the plane normal as
        // Cross_Product(V,U) with U=B-A, V=C-A — i.e. e2 x e1, the NEGATION of
        // the usual convention — so the engine's visible faces are the ones
        // Metal calls FRONT-facing under its default clockwise winding.
        // Rendering both: CullBack drops the whole room (whole-frame luma
        // 115.68 -> 63.19), CullFront is pixel-identical to CullNone.
        //
        // THE MIRROR BUG (reported 2026-08-08: "the gpu main mirror is missing
        // most of the reflected geometry"). A reflection view matrix has
        // determinant -1, so every triangle's SCREEN-SPACE WINDING reverses.
        // CullFront — measured correct for the main camera — therefore rejects
        // in a mirror exactly the geometry it keeps in the main view. This pass
        // predates culling: the reflection code still carries the comment
        // "det -1, so a left-handed basis; harmless while raster culling is
        // off", and culling was turned ON in 69bf0f0 without revisiting it. A
        // stale invariant, not a new mistake — and the reason the mirror lost
        // most of its content while still rendering something (the panels'
        // own emissive and whatever happened to face the other way).
        const MTLCullMode baseCull = opt.cull ? (mirroredBasis ? MTLCullModeBack
                                                               : MTLCullModeFront)
                                              : MTLCullModeNone;
        [enc setCullMode:baseCull];
        [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
        [enc setFragmentBytes:&u length:sizeof(u) atIndex:1];
        [enc setFragmentSamplerState:samp atIndex:0];
        drawScene(enc, /*gbuffer=*/true, baseCull);
        [enc endEncoding];
    };

    // ---- TRANSPARENT SURFACES: the depth peel ------------------------------
    // The CPU's mechanism, in its own two halves (the user's framing): the PEEL
    // sets the ORDER, the tiled TBR only makes it fast. This arm reproduces the
    // order exactly and picks its own scheduling, because a GPU's natural
    // scheduling is not a per-strip linked list.
    //
    // ORDER, read out of FDS/RENDER/Transform.cpp:2586-2633 (the sort key) and
    // RENDER.CPP:895-1140 (the batcher):
    //   * transparents are clumped by (ParentTri, side) — the MESH pointer, not
    //     the material, so greets' four transparent materials all living in
    //     Piramid.lwo form ONE clump per side;
    //   * the key is objScore + faceFine, with +4*fzp for front-facing, so
    //     EVERY back-facing clump composites before EVERY front-facing one, and
    //     within a side the clumps run far-to-near. The documented result for a
    //     nested pair is outer.back, inner.back, inner.front, outer.front;
    //   * within a clump, K = xparPeelPassesEffective() passes PER SIDE
    //     (DeferredSurfaceKernel.cpp:3600): K == 1 keeps the single nearest
    //     fragment (the legacy 2-deep front/back peel greets runs); K > 1 peels
    //     farthest-first against a per-pixel peel floor (fountain authors 4).
    //
    // SCHEDULING, chosen freely and stated: two encoders per (clump, side,
    // pass) — a depth resolve then a blended colour pass at depth-Equal — plus
    // one depth blit per extra pass to advance the floor. No tile binning.
    struct XparGroup { std::vector<size_t> batches; float ctr[3]; float rad; };
    std::vector<XparGroup> xparGroups;
    {
        std::map<int, size_t> byMesh;
        for (size_t i = 0; i < scene.batches.size(); ++i) {
            const Batch &b = scene.batches[i];
            if (b.mirrorIndex != 0) continue;
            if (!b.transparent && !b.additive) continue;
            auto it = byMesh.find(b.meshId);
            if (it == byMesh.end()) {
                byMesh[b.meshId] = xparGroups.size();
                XparGroup g;
                g.batches.push_back(i);
                for (int c = 0; c < 3; ++c) g.ctr[c] = b.bsCtr[c];
                g.rad = b.bsRad;
                xparGroups.push_back(std::move(g));
            } else {
                XparGroup &g = xparGroups[it->second];
                g.batches.push_back(i);
                // Union of bounding spheres, coarse but only used for ordering.
                g.rad = std::max(g.rad, b.bsRad);
            }
        }
    }
    const int xparPeelPasses = std::max(1, opt.xparPeelPasses > 0 ? opt.xparPeelPasses
                                                                  : scene.xparPeelPasses);
    if (!xparGroups.empty())
        std::fprintf(stderr,
            "[XPAR] %zu peel clump(s) (grouped by mesh, as the CPU clumps by "
            "ParentTri), %d peel pass(es) per side -> %d layers per pixel per "
            "clump; %zu encoder(s)/frame\n",
            xparGroups.size(), xparPeelPasses, 2 * xparPeelPasses,
            xparGroups.size() * size_t(xparPeelPasses) * 2 * 2);

    // dst = the HDR target this composite lands in (the main frame's, or a
    // mirror reflection's). `srcDepth` is the OPAQUE depth of that same view.
    auto encodeXpar = [&](id<MTLCommandBuffer> cb, const FrameUniforms &u,
                          id<MTLTexture> dst, id<MTLTexture> srcDepth,
                          bool mirroredBasis) {
        if (xparGroups.empty() || !opt.xpar) return;
        // Order groups within a side. `extent` is the CPU's own: the object's
        // bounding-sphere view depth pushed to its FAR edge for back faces and
        // its NEAR edge for front faces (Transform.cpp:2621-2626). Larger
        // extent composites first.
        auto viewZ = [&](const XparGroup &g) {
            float z = 0.0f;
            for (int c = 0; c < 3; ++c) z += u.camRow2[c] * (g.ctr[c] - u.camSrc[c]);
            return z;
        };
        for (int side = 0; side < 2; ++side) {          // 0 = BACK faces first
            const bool front = (side == 1);
            std::vector<size_t> order(xparGroups.size());
            for (size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                const float ea = viewZ(xparGroups[a]) + (front ? -xparGroups[a].rad
                                                               :  xparGroups[a].rad);
                const float eb = viewZ(xparGroups[b]) + (front ? -xparGroups[b].rad
                                                               :  xparGroups[b].rad);
                return ea > eb;                          // far to near
            });
            // Engine-front-facing == Metal BACK-facing here (measured, see the
            // G-buffer cull comment), so drawing only the engine's front faces
            // means CullModeFront. A reflection basis has determinant -1 and
            // reverses screen winding, so both senses flip.
            MTLCullMode cull = front ? MTLCullModeFront : MTLCullModeBack;
            if (mirroredBasis)
                cull = (cull == MTLCullModeFront) ? MTLCullModeBack : MTLCullModeFront;

            for (size_t oi = 0; oi < order.size(); ++oi) {
                const XparGroup &g = xparGroups[order[oi]];
                for (int pass = 0; pass < xparPeelPasses; ++pass) {
                    XparUniforms xu{};
                    for (int c = 0; c < 3; ++c)
                        xu.sceneAmbient[c] = scene.ambient[c] * (1.0f / 255.0f);
                    xu.peelReverse   = (xparPeelPasses > 1) ? 1.0f : 0.0f;
                    xu.usePeelFloor  = (pass > 0) ? 1.0f : 0.0f;

                    // (a) depth resolve — one fragment per pixel for this layer
                    {
                        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                        rp.depthAttachment.texture = xparDepth;
                        rp.depthAttachment.loadAction = MTLLoadActionClear;
                        rp.depthAttachment.storeAction = MTLStoreActionStore;
                        // reversed-Z: 0 = far (keep-nearest init), 1 = near
                        // (keep-farthest init), matching the CPU's 0 / 0xFFFF.
                        rp.depthAttachment.clearDepth = (xparPeelPasses > 1) ? 1.0 : 0.0;
                        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
                        [e setRenderPipelineState:psoXparDepth];
                        [e setDepthStencilState:(xparPeelPasses > 1) ? dssXparFar : dssXparNear];
                        [e setCullMode:cull];
                        [e setVertexBuffer:vb offset:0 atIndex:0];
                        [e setVertexBytes:&u length:sizeof(u) atIndex:1];
                        [e setFragmentBytes:&u length:sizeof(u) atIndex:1];
                        [e setFragmentBytes:&xu length:sizeof(xu) atIndex:5];
                        [e setFragmentTexture:srcDepth atIndex:5];
                        [e setFragmentTexture:xparFloor atIndex:6];
                        for (size_t bi : g.batches) {
                            const Batch &b = scene.batches[bi];
                            if (!front && !b.twoSided) continue;   // see below
                            [e setVertexBytes:&bus[bi] length:sizeof(BatchUniforms) atIndex:2];
                            [e drawPrimitives:MTLPrimitiveTypeTriangle
                                  vertexStart:NSUInteger(b.firstVertex)
                                  vertexCount:NSUInteger(b.vertexCount)];
                        }
                        [e endEncoding];
                    }
                    // (b) composite the resolved layer
                    {
                        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                        rp.colorAttachments[0].texture = dst;
                        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
                        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                        rp.depthAttachment.texture = xparDepth;
                        rp.depthAttachment.loadAction = MTLLoadActionLoad;
                        rp.depthAttachment.storeAction = MTLStoreActionStore;
                        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
                        [e setDepthStencilState:dssXparEqual];
                        [e setCullMode:cull];
                        [e setVertexBuffer:vb offset:0 atIndex:0];
                        [e setVertexBytes:&u length:sizeof(u) atIndex:1];
                        [e setFragmentBytes:&u length:sizeof(u) atIndex:1];
                        [e setFragmentBuffer:lightBuf offset:0 atIndex:3];
                        [e setFragmentBytes:&xu length:sizeof(xu) atIndex:5];
                        [e setFragmentSamplerState:samp atIndex:0];
                        [e setFragmentTexture:srcDepth atIndex:5];
                        [e setFragmentTexture:xparFloor atIndex:6];
                        for (size_t bi : g.batches) {
                            const Batch &b = scene.batches[bi];
                            // THE BACK LAYER ONLY EXISTS FOR Mat_TwoSided. The
                            // peel splits the faces that SURVIVED
                            // Transform_Objects' backface cull, and that cull's
                            // only bypass is Mat_TwoSided
                            // (Transform.cpp:2429-2434) — so a single-sided
                            // transparent contributes nothing to the back
                            // layer. This is the reason FOUNTAIN.CPP:854-864
                            // force-sets TwoSided on the orb shells: without it
                            // "those back-facing tris get culled before reaching
                            // the deferred dispatch, so the back layer stays
                            // empty and the orbs render as a thin shell."
                            // Drawing both sides unconditionally double-lit
                            // greets' 560-tri 'lamp light' set: MEASURED at
                            // t=2000 as 223,615 px moved with |err| against the
                            // reference rising 40.81 -> 46.44 on them.
                            if (!front && !b.twoSided) continue;
                            if (b.additive) {
                                [e setRenderPipelineState:psoXparAdd];
                            } else if (b.xparBlendAlpha > 0.0f) {
                                [e setRenderPipelineState:psoXparLerp];
                                [e setBlendColorRed:b.xparBlendAlpha green:b.xparBlendAlpha
                                               blue:b.xparBlendAlpha alpha:1.0f];
                            } else {
                                const float dw = (b.transparency > 0.0f)
                                               ? b.transparency * 0.01f : 0.5f;
                                [e setRenderPipelineState:psoXparLegacy];
                                [e setBlendColorRed:dw green:dw blue:dw alpha:1.0f];
                            }
                            [e setVertexBytes:&bus[bi] length:sizeof(BatchUniforms) atIndex:2];
                            [e setFragmentBytes:&bus[bi] length:sizeof(BatchUniforms) atIndex:2];
                            [e setFragmentTexture:(b.textureIndex >= 0
                                                       ? texes[size_t(b.textureIndex)]
                                                       : texes[0]) atIndex:0];
                            [e drawPrimitives:MTLPrimitiveTypeTriangle
                                  vertexStart:NSUInteger(b.firstVertex)
                                  vertexCount:NSUInteger(b.vertexCount)];
                        }
                        [e endEncoding];
                    }
                    // (c) advance the peel floor to this layer's depth
                    if (xparPeelPasses > 1 && pass + 1 < xparPeelPasses) {
                        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                        [bl copyFromTexture:xparDepth sourceSlice:0 sourceLevel:0
                               sourceOrigin:MTLOriginMake(0, 0, 0)
                                 sourceSize:MTLSizeMake(NSUInteger(W), NSUInteger(H), 1)
                                  toTexture:xparFloor destinationSlice:0 destinationLevel:0
                          destinationOrigin:MTLOriginMake(0, 0, 0)];
                        [bl endEncoding];
                    }
                }
            }
        }
    };

    // ---- ENVIRONMENT PROBE BAKE --------------------------------------------
    // Six 90-degree faces per probe, each rendered through the SAME G-buffer +
    // lighting pipeline the frame uses — so what a metal reflects is the lit
    // world, with this arm's shadows and lights in it, not a constant and not
    // a sky gradient. Baked ONCE (greets' probes are static; the CPU caches
    // them the same way and re-bakes only on invalidation), and deliberately
    // NOT included in the per-frame timings for the same reason the static
    // shadow cubes are not.
    //
    // Face bases are kCubeFaces, the same table the shadow cubes use and the
    // same one Metal's texturecube sampler expects, so a direction sampled in
    // the lighting pass lands on the texel this bake wrote. That equivalence
    // was already established (and its 'up = -tc' sign already paid for) by
    // the shadow work; reusing it is why this needs no new convention.
    auto bakeEnvProbes = [&]() {
        if (nProbes <= 0) return 0.0;
        const auto t0 = std::chrono::steady_clock::now();
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        for (int p = 0; p < nProbes; ++p) {
            // Self-exclusion: the surface this probe belongs to is not in it.
            envSkipMat = baseMatName(scene.envProbes[size_t(p)].material);
            for (int f = 0; f < 6; ++f) {
                FrameUniforms fe = fu;
                for (int c = 0; c < 3; ++c) {
                    fe.camRow0[c] = kCubeFaces[f].right[c];
                    fe.camRow1[c] = kCubeFaces[f].up[c];
                    fe.camRow2[c] = kCubeFaces[f].fwd[c];
                    fe.camSrc[c]  = scene.envProbes[size_t(p)].pos[c];
                }
                // A 90-degree square face: perspX = perspY = res/2 and the
                // principal point at the centre, so sx = sy = 1 and ox = oy = 0.
                fe.sx = 1.0f; fe.ox = 0.0f;
                fe.sy = 1.0f; fe.oy = 0.0f;
                fe.invSx = 1.0f; fe.invSy = 1.0f;
                fe.mirrorCount = 0;                 // a probe carries no mirror composite
                for (int c = 0; c < 4; ++c) fe.clipPlane[c] = 0.0f;
                encodeGBuffer(cb, fe, eAlbedo, eNormal, eParams, eMirror, eDepth, nil);
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = eHdr;
                rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
                id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
                [enc setRenderPipelineState:psoLight];
                [enc setFragmentBytes:&fe length:sizeof(fe) atIndex:1];
                [enc setFragmentBuffer:lightBuf offset:0 atIndex:2];
                [enc setFragmentBuffer:shBuf offset:0 atIndex:3];
                [enc setFragmentTexture:eAlbedo atIndex:0];
                [enc setFragmentTexture:eNormal atIndex:1];
                [enc setFragmentTexture:eParams atIndex:2];
                [enc setFragmentTexture:eDepth  atIndex:3];
                for (int s = 0; s < kMaxShadowCubes; ++s)
                    [enc setFragmentTexture:(s < int(cubes.size()) ? cubes[size_t(s)] : dummyCube)
                                    atIndex:NSUInteger(4 + s)];
                for (int s = 0; s < kMaxSpotMaps; ++s)
                    [enc setFragmentTexture:(s < int(spots.size()) ? spots[size_t(s)] : dummy2D)
                                    atIndex:NSUInteger(20 + s)];
                [enc setFragmentTexture:eMirror atIndex:36];
                for (int s = 0; s < 4; ++s)
                    [enc setFragmentTexture:dummyRefl atIndex:NSUInteger(37 + s)];
                // NO ENV CUBES inside a probe bake: one bounce only. Feeding the
                // probes back in would make the bake order-dependent and let a
                // pair of facing metals amplify each other, which the CPU's
                // single-pass FramePrep also does not do.
                for (int s = 0; s < kMaxEnvProbes; ++s)
                    [enc setFragmentTexture:dummyEnvCube atIndex:NSUInteger(41 + s)];
                [enc setFragmentSamplerState:shadowSamp atIndex:1];
                [enc setFragmentSamplerState:rawSamp atIndex:2];
                [enc setFragmentSamplerState:envSamp atIndex:3];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [enc endEncoding];
                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                [bl copyFromTexture:eHdr sourceSlice:0 sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(NSUInteger(envRes), NSUInteger(envRes), 1)
                           toTexture:envCubes[size_t(p)] destinationSlice:NSUInteger(f)
                    destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
                [bl endEncoding];
            }
        }
        envSkipMat.clear();
        {   // roughness -> mip needs the chain
            id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
            for (int p = 0; p < nProbes; ++p) [bl generateMipmapsForTexture:envCubes[size_t(p)]];
            [bl endEncoding];
        }
        [cb commit];
        [cb waitUntilCompleted];
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - t0).count();
    };

    // Flare sprites, additive into `dst`, projected with camera `camRot/camSrc`
    // taken from a FrameUniforms. `clip` skips lights at/behind a mirror plane
    // (their reflections would otherwise hang in front of it).
    auto encodeFlares = [&](id<MTLCommandBuffer> cb, const FrameUniforms &u,
                            id<MTLTexture> dst, id<MTLTexture> depthTex,
                            const float *clip4) {
        if (!opt.flares || flares.empty()) return;
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = dst;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:psoFlare];
        [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
        [enc setFragmentBytes:&u length:sizeof(u) atIndex:1];
        [enc setFragmentSamplerState:samp atIndex:0];
        [enc setFragmentTexture:depthTex atIndex:1];
        for (const auto &fi : flares) {
            if (clip4) {
                const float sd = fi.wpos[0]*clip4[0] + fi.wpos[1]*clip4[1]
                               + fi.wpos[2]*clip4[2] + clip4[3];
                if (sd < 0.05f) continue;
            }
            float rel[3];
            for (int c = 0; c < 3; ++c) rel[c] = fi.wpos[c] - u.camSrc[c];
            float vx = 0, vy = 0, vz = 0;
            for (int c = 0; c < 3; ++c) {
                vx += u.camRow0[c] * rel[c];
                vy += u.camRow1[c] * rel[c];
                vz += u.camRow2[c] * rel[c];
            }
            if (!(vz > scene.camera.nearZ && vz < scene.camera.farZ)) continue;
            FlareUniforms fun{};
            fun.centerPx[0] = scene.camera.cntrEX + vx * scene.camera.perspX / vz;
            fun.centerPx[1] = scene.camera.cntrEY - vy * scene.camera.perspY / vz;
            fun.centerPx[2] = vz;
            fun.centerPx[3] = fi.worldHalf / vz;
            fun.gain[0] = opt.flareGain;
            fun.gain[2] = float(W);
            fun.gain[3] = float(H);
            [enc setVertexBytes:&fun length:sizeof(fun) atIndex:2];
            [enc setFragmentBytes:&fun length:sizeof(fun) atIndex:2];
            [enc setFragmentTexture:texes[size_t(fi.tex)] atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
        }
        [enc endEncoding];
    };

    // VOLUMETRIC SPOT CONES into an arbitrary HDR target with an arbitrary
    // camera. Factored out of renderFrame so the MIRROR REFLECTION views can
    // run it too — reported 2026-08-08 as "no spotlight cones in mirrors".
    //
    // The CPU shows beams inside its mirrors, and it is worth being precise
    // about how, because the two arms get there differently. The CPU has ONE
    // screen-space cone pass (RENDER.CPP:1237) and admits the MIRROR-CLONE
    // spots into it, gated per pixel on the mirror's stamped footprint with the
    // chord clamped to start at the glass depth (DeferredVolumetric.cpp:761-772
    // and the comment at :1858-1883, "Mirror-clone spots ARE admitted (beams
    // show in mirrors)"). This arm has no clone geometry and no footprint mask:
    // its mirror is a real reflection render, so the faithful equivalent is to
    // run the same integral from the reflected camera against the reflection's
    // own depth buffer. Same beams, same shadow taps, different bookkeeping.
    auto encodeCones = [&](id<MTLCommandBuffer> cb, const FrameUniforms &u,
                           id<MTLTexture> dst, id<MTLTexture> depthTex) {
        if (!opt.cones || coneU.density <= 0.0f || nSpotLights <= 0) return;
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = dst;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:psoCones];
        [enc setFragmentBytes:&u length:sizeof(u) atIndex:1];
        [enc setFragmentBuffer:lightBuf offset:0 atIndex:2];
        [enc setFragmentBytes:&coneU length:sizeof(coneU) atIndex:4];
        [enc setFragmentTexture:depthTex atIndex:3];
        for (int i = 0; i < kMaxSpotMaps; ++i)
            [enc setFragmentTexture:(i < int(spots.size()) ? spots[size_t(i)] : dummy2D)
                            atIndex:NSUInteger(20 + i)];
        [enc setFragmentSamplerState:shadowSamp atIndex:1];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
    };

    auto renderFrame = [&]() -> id<MTLCommandBuffer> {
        id<MTLCommandBuffer> cb = [queue commandBuffer];

        // --- pass 0: per-frame DYNAMIC shadow bake (moving omnis only) ---
        if (opt.shadows) {
            bakeCubes(cb, /*movingOnly=*/!opt.rebakeAll, /*timed=*/true);
            bakeSpotMaps(cb);
        }

        // --- pass 0b: mirror REFLECTION views (pass index 4 in the counters) ---
        // Per visible mirror: reflect the camera across the panel plane
        // (position mirrored, each view row reflected — det -1, so a
        // left-handed basis; harmless while raster culling is off), render
        // the real scene through the same G-buffer + lighting pipeline with
        // an oblique clip at the plane, and light it with the same lights and
        // the same world-space shadow cubes. This is radiometrically the
        // CPU's clone+cloned-omni machinery: what you see through the panel
        // is the lit world reflected. No second bounce: another mirror's
        // panel inside a reflection shows its emissive text, not a further
        // reflection (the CPU's --mirror_rtt order-2 slots are out of scope,
        // stated in the plan).
        bool mirrorActive[kMaxMirrors] = {};
        int firstActive = -1, lastActive = -1;
        // --viz=mirror (13) NEEDS the reflection passes to run; every other
        // viz mode deliberately skips them.
        if (nMirrors > 0 && (opt.viz < 0 || opt.viz == 13)) {
            for (int i = 0; i < nMirrors; ++i) {
                const auto &m = scene.mirrors[size_t(i)];
                const float sd = m.n[0]*scene.camera.src[0] + m.n[1]*scene.camera.src[1]
                               + m.n[2]*scene.camera.src[2] + m.d;
                mirrorActive[i] = sd > 0.01f;
                if (mirrorActive[i]) { if (firstActive < 0) firstActive = i; lastActive = i; }
            }
        }
        for (int i = 0; i < nMirrors; ++i) {
            if (!mirrorActive[i]) continue;
            const auto &m = scene.mirrors[size_t(i)];
            FrameUniforms fum = fu;
            const float N[3] = {m.n[0], m.n[1], m.n[2]};
            const float sdC = N[0]*fu.camSrc[0] + N[1]*fu.camSrc[1] + N[2]*fu.camSrc[2] + m.d;
            for (int c = 0; c < 3; ++c) fum.camSrc[c] = fu.camSrc[c] - 2.0f * sdC * N[c];
            float *rows[3] = {fum.camRow0, fum.camRow1, fum.camRow2};
            const float *src[3] = {fu.camRow0, fu.camRow1, fu.camRow2};
            for (int r = 0; r < 3; ++r) {
                const float d = src[r][0]*N[0] + src[r][1]*N[1] + src[r][2]*N[2];
                for (int c = 0; c < 3; ++c) rows[r][c] = src[r][c] - 2.0f * d * N[c];
            }
            for (int c = 0; c < 3; ++c) fum.clipPlane[c] = N[c];
            fum.clipPlane[3] = m.d;
            fum.mirrorCount = 0;   // no recursion
            const bool first = (i == firstActive), last = (i == lastActive);
            void (^hook)(MTLRenderPassDescriptor *) = nil;
            if (sampleBuf && first) {
                hook = ^(MTLRenderPassDescriptor *rp) {
                    rp.sampleBufferAttachments[0].sampleBuffer = sampleBuf;
                    rp.sampleBufferAttachments[0].startOfVertexSampleIndex = 8;
                    rp.sampleBufferAttachments[0].endOfFragmentSampleIndex = MTLCounterDontSample;
                    rp.sampleBufferAttachments[0].endOfVertexSampleIndex = MTLCounterDontSample;
                    rp.sampleBufferAttachments[0].startOfFragmentSampleIndex = MTLCounterDontSample;
                };
            }
            encodeGBuffer(cb, fum, mAlbedo, mNormal, mParams, mMirror, mDepth, hook,
                          /*mirroredBasis=*/true);
            {   // reflection lighting into reflHdr[i]
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = reflHdr[size_t(i)];
                rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
                id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
                [enc setRenderPipelineState:psoLight];
                [enc setFragmentBytes:&fum length:sizeof(fum) atIndex:1];
                [enc setFragmentBuffer:lightBuf offset:0 atIndex:2];
                [enc setFragmentBuffer:shBuf offset:0 atIndex:3];
                [enc setFragmentTexture:mAlbedo atIndex:0];
                [enc setFragmentTexture:mNormal atIndex:1];
                [enc setFragmentTexture:mParams atIndex:2];
                [enc setFragmentTexture:mDepth atIndex:3];
                for (int s = 0; s < kMaxShadowCubes; ++s)
                    [enc setFragmentTexture:(s < int(cubes.size()) ? cubes[size_t(s)] : dummyCube)
                                    atIndex:NSUInteger(4 + s)];
                for (int s = 0; s < kMaxSpotMaps; ++s)
                    [enc setFragmentTexture:(s < int(spots.size()) ? spots[size_t(s)] : dummy2D)
                                    atIndex:NSUInteger(20 + s)];
                [enc setFragmentTexture:mMirror atIndex:36];
                for (int s = 0; s < 4; ++s)
                    [enc setFragmentTexture:dummyRefl atIndex:NSUInteger(37 + s)];
                for (int s = 0; s < kMaxEnvProbes; ++s)
                    [enc setFragmentTexture:(s < nProbes ? envCubes[size_t(s)] : dummyEnvCube)
                                    atIndex:NSUInteger(41 + s)];
                [enc setFragmentSamplerState:shadowSamp atIndex:1];
                [enc setFragmentSamplerState:rawSamp atIndex:2];
                [enc setFragmentSamplerState:envSamp atIndex:3];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [enc endEncoding];
            }
            // flare sprites inside the reflection (the CPU draws clone flares
            // in its mirrors); lights behind the plane are clipped.
            {
                const float clip4[4] = {N[0], N[1], N[2], m.d};
                encodeFlares(cb, fum, reflHdr[size_t(i)], mDepth, clip4);
            }
            // Transparent surfaces and volumetric beams INSIDE the reflection.
            // The CPU gets both: its clone mesh carries the transparent faces
            // (they composite through the same peel, bounded to the mirror
            // window, RENDER.CPP:936-985) and its clone spots are admitted to
            // the cone pass behind the glass. Order matches the main view:
            // peel, then cones.
            encodeXpar(cb, fum, reflHdr[size_t(i)], mDepth, /*mirroredBasis=*/true);
            encodeCones(cb, fum, reflHdr[size_t(i)], mDepth);
            if (sampleBuf && last) {
                // close the mirror-pass counter interval with an empty encoder
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = reflHdr[size_t(i)];
                rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                rp.sampleBufferAttachments[0].sampleBuffer = sampleBuf;
                rp.sampleBufferAttachments[0].startOfVertexSampleIndex = MTLCounterDontSample;
                rp.sampleBufferAttachments[0].endOfFragmentSampleIndex = 9;
                rp.sampleBufferAttachments[0].endOfVertexSampleIndex = MTLCounterDontSample;
                rp.sampleBufferAttachments[0].startOfFragmentSampleIndex = MTLCounterDontSample;
                id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
                [e endEncoding];
            }
        }

        // --- pass 1: G-buffer ---
        encodeGBuffer(cb, fu, gAlbedo, gNormal, gParams, gMirror, gDepth,
                      sampleBuf ? ^(MTLRenderPassDescriptor *rp) {
                          rp.sampleBufferAttachments[0].sampleBuffer = sampleBuf;
                          rp.sampleBufferAttachments[0].startOfVertexSampleIndex = 2;
                          rp.sampleBufferAttachments[0].endOfFragmentSampleIndex = 3;
                          rp.sampleBufferAttachments[0].endOfVertexSampleIndex = MTLCounterDontSample;
                          rp.sampleBufferAttachments[0].startOfFragmentSampleIndex = MTLCounterDontSample;
                      } : (void (^)(MTLRenderPassDescriptor *))nil);

        // --- pass 2: PBR lighting (or a debug viz) ---
        const bool viz = opt.viz >= 0;
        if (opt.stages >= 2) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = viz ? ldrTex : hdrTex;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            attachCounters(rp, 2);
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            [enc setRenderPipelineState:viz ? psoViz : psoLight];
            [enc setFragmentBytes:&fu length:sizeof(fu) atIndex:1];
            [enc setFragmentBuffer:lightBuf offset:0 atIndex:2];
            [enc setFragmentBuffer:shBuf offset:0 atIndex:3];
            [enc setFragmentTexture:gAlbedo atIndex:0];
            [enc setFragmentTexture:gNormal atIndex:1];
            [enc setFragmentTexture:gParams atIndex:2];
            [enc setFragmentTexture:gDepth atIndex:3];
            for (int i = 0; i < kMaxShadowCubes; ++i)
                [enc setFragmentTexture:(i < int(cubes.size()) ? cubes[size_t(i)] : dummyCube)
                                atIndex:NSUInteger(4 + i)];
            for (int i = 0; i < kMaxSpotMaps; ++i)
                [enc setFragmentTexture:(i < int(spots.size()) ? spots[size_t(i)] : dummy2D)
                                atIndex:NSUInteger(20 + i)];
            [enc setFragmentTexture:gMirror atIndex:36];
            // Reflection slots: the live render for ACTIVE mirrors, cleared
            // black for inactive ones (their panels then show emissive only).
            for (int i = 0; i < 4; ++i)
                [enc setFragmentTexture:((i < nMirrors && mirrorActive[i])
                                             ? reflHdr[size_t(i)] : dummyRefl)
                                atIndex:NSUInteger(37 + i)];
            if (opt.viz == 13) {
                std::fprintf(stderr, "[MIRRORPROBE] panels=%d\n", nMirrors);
                for (int i = 0; i < nMirrors; ++i) {
                    const auto &m = scene.mirrors[size_t(i)];
                    const float sd = m.n[0]*scene.camera.src[0] + m.n[1]*scene.camera.src[1]
                                   + m.n[2]*scene.camera.src[2] + m.d;
                    std::fprintf(stderr,
                        "[MIRRORPROBE]   %d '%s' N=(%.2f,%.2f,%.2f) d=%.2f  camSignedDist=%+.2f  %s\n",
                        i + 1, m.material.c_str(), m.n[0], m.n[1], m.n[2], m.d, sd,
                        mirrorActive[i] ? "ACTIVE (reflection rendered + bound)"
                                        : "INACTIVE (camera behind the plane; black bound)");
                }
            }
            for (int i = 0; i < kMaxEnvProbes; ++i)
                [enc setFragmentTexture:(i < nProbes ? envCubes[size_t(i)] : dummyEnvCube)
                                atIndex:NSUInteger(41 + i)];
            [enc setFragmentSamplerState:shadowSamp atIndex:1];
            [enc setFragmentSamplerState:rawSamp atIndex:2];
            [enc setFragmentSamplerState:envSamp atIndex:3];
            if (viz) {
                uint32_t m = uint32_t(opt.viz);
                [enc setFragmentBytes:&m length:sizeof(m) atIndex:4];
            }
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [enc endEncoding];
        }

        // --- pass 2b: flare sprites, additive into the HDR target ---
        if (!viz && opt.stages >= 2)
            encodeFlares(cb, fu, hdrTex, gDepth, nullptr);

        // --- pass 2b1: TRANSPARENT SURFACES (depth peel) ---
        // RENDER.CPP order: the peel runs after the deferred lighting resolve
        // and the sprite loop, and BEFORE the volumetric cones and the bloom
        // bright-pass (:741-1161 peel, :1192-1206 sprites, :1233-1239 cones,
        // :1266 bloom). Transparents therefore bloom, which is why the CPU
        // deliberately leaves their composite unclamped under HDR.
        if (!viz && opt.stages >= 2)
            encodeXpar(cb, fu, hdrTex, gDepth, /*mirroredBasis=*/false);

        // --- pass 2b2: VOLUMETRIC SPOT CONES, additive into HDR ---
        // RENDER.CPP:1231-1240 order: after the opaque/TBR draws, BEFORE
        // DoF/bright/bloom/tonemap. So the shafts feed the bloom, as they do on
        // the CPU — putting this after bloom would render visibly harder beams.
        if (!viz && opt.stages >= 2)
            encodeCones(cb, fu, hdrTex, gDepth);

        // --- pass 2c: bloom (bright-pass -> 4 blur taps -> additive upsample) ---
        if (!viz && opt.stages >= 3 && opt.bloom && opt.bloomIntensity > 0.0f) {
            BloomUniforms bu{};
            bu.srcSize[0] = float(W); bu.srcSize[1] = float(H);
            bu.dstSize[0] = float(BW); bu.dstSize[1] = float(BH);
            bu.threshold = opt.bloomThreshold;
            bu.intensity = opt.bloomIntensity;
            auto fsPass = [&](id<MTLRenderPipelineState> pso, id<MTLTexture> dst,
                              id<MTLTexture> srcTex, const float dir[2], bool additive) {
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = dst;
                rp.colorAttachments[0].loadAction =
                    additive ? MTLLoadActionLoad : MTLLoadActionDontCare;
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
                [e setRenderPipelineState:pso];
                [e setFragmentBytes:&bu length:sizeof(bu) atIndex:1];
                if (dir) [e setFragmentBytes:dir length:sizeof(float) * 2 atIndex:2];
                [e setFragmentTexture:srcTex atIndex:0];
                [e setFragmentSamplerState:bloomSamp atIndex:0];
                [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [e endEncoding];
            };
            static const float kH[2] = {1.0f, 0.0f}, kV[2] = {0.0f, 1.0f};
            fsPass(psoBloomBright, bloomA, hdrTex, nullptr, false);
            // TWO full separable passes, as Hdr.cpp does (`for pass < 2`).
            for (int pass = 0; pass < 2; ++pass) {
                fsPass(psoBloomBlur, bloomB, bloomA, kH, false);
                fsPass(psoBloomBlur, bloomA, bloomB, kV, false);
            }
            fsPass(psoBloomAdd, hdrTex, bloomA, nullptr, true);
        }

        // --- pass 3: tonemap ---
        if (!viz && opt.stages >= 3) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = ldrTex;
            rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            attachCounters(rp, 3);
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            [enc setRenderPipelineState:psoTonemap];
            [enc setFragmentBytes:&fu length:sizeof(fu) atIndex:1];
            [enc setFragmentTexture:hdrTex atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [enc endEncoding];
        }

        [cb commit];
        return cb;
    };

    // ---- one-time ENV PROBE BAKE -------------------------------------------
    // After the static shadow bake (so the probes contain shadowed light) and
    // before anything timed. Reported, not folded into the frame cost.
    if (nProbes > 0) {
        const double ms = bakeEnvProbes();
        std::fprintf(stderr,
            "[ENVREFL] baked %d probe cube(s), %d faces at %d^2 + mips, ONCE: %.2f ms\n",
            nProbes, nProbes * 6, envRes, ms);
        for (int i = 0; i < nProbes; ++i)
            std::fprintf(stderr, "[ENVREFL]   probe %d '%s' at (%.1f %.1f %.1f), %d material(s)\n",
                         i + 1, scene.envProbes[size_t(i)].material.c_str(),
                         scene.envProbes[size_t(i)].pos[0], scene.envProbes[size_t(i)].pos[1],
                         scene.envProbes[size_t(i)].pos[2], scene.envProbes[size_t(i)].users);
    }

    // ---- --anim_probe: the window's per-frame refresh, OFFSCREEN ------------
    // Written because "mech omnis are staying in place" was reported from a
    // window run and could not be reproduced offscreen: setting the pose AT LOAD
    // moves everything, so the load path was innocent and the defect lived in
    // the per-frame path — which nothing headless exercised.
    //
    // This runs the EXACT sequence the window loop runs (Reanimate, then
    // refreshFrameUniforms / refreshBatchUniforms / refreshLightBuffer) and
    // prints the GPU-FACING structures afterwards, not scene.* — the whole bug
    // class here is "the CPU-side list updated but the thing the shader reads
    // did not", and reading scene.* would have reported success.
    if (opt.animProbe) {
        LoadOptions lo = opt.loadOpt ? *opt.loadOpt : LoadOptions{};
        lo.camPose.clear();          // same as the window: follow the spline
        lo.verbose = false;
        std::fprintf(stderr,
            "[ANIMPROBE] the window's per-frame refresh, run offscreen. Values are read\n"
            "[ANIMPROBE]   from the GPU-facing arrays (GpuLight / BatchUniforms / flare\n"
            "[ANIMPROBE]   instances) AFTER the refresh, so a stale upload shows up here.\n");
        for (float t : {float(opt.animProbeT[0]), float(opt.animProbeT[1])}) {
            Reanimate(scene, lo, t);
            refreshFrameUniforms();
            refreshBatchUniforms();
            refreshLightBuffer();
            std::fprintf(stderr, "[ANIMPROBE] t=%.0f  cam=(%.2f,%.2f,%.2f)\n",
                         t, fu.camSrc[0], fu.camSrc[1], fu.camSrc[2]);
            for (size_t i = 0; i < lights.size(); ++i) {
                if (lightOrigin[i] == std::string("fld") && i < 7) continue;
                std::fprintf(stderr, "[ANIMPROBE]   light[%zu] %-10s pos=(%7.2f,%6.2f,%8.2f)\n",
                             i, lightOrigin[i], lights[i].pos[0], lights[i].pos[1],
                             lights[i].pos[2]);
                if (i >= 12 && i < 20) { i = 19; }   // the ten spots are enough at three
            }
            for (size_t i = 0; i < flares.size(); ++i)
                std::fprintf(stderr, "[ANIMPROBE]   flare[%zu] wpos=(%7.2f,%6.2f,%8.2f) half=%.3f\n",
                             i, flares[i].wpos[0], flares[i].wpos[1], flares[i].wpos[2],
                             flares[i].worldHalf);
            // A mech batch: the geometry half of the same question.
            for (size_t i = 0; i < scene.batches.size(); ++i) {
                if (scene.batches[i].meshName.find("mech") == std::string::npos &&
                    scene.batches[i].meshName.find("Mech") == std::string::npos) continue;
                std::fprintf(stderr, "[ANIMPROBE]   batch[%zu] '%s' objPos=(%7.2f,%6.2f,%8.2f)\n",
                             i, scene.batches[i].meshName.c_str(),
                             bus[i].objPos[0], bus[i].objPos[1], bus[i].objPos[2]);
                break;
            }
        }
        return true;
    }

    // ---- INTERACTIVE WINDOW ------------------------------------------------
    if (opt.interactive) {
        if (!opt.loadOpt) { std::fprintf(stderr, "[WINDOW] no LoadOptions\n"); return false; }
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            std::fprintf(stderr, "[WINDOW] SDL_Init: %s\n", SDL_GetError()); return false;
        }
        SDL_Window *win = SDL_CreateWindow("GpuBench — greets on the GPU",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, opt.winW, opt.winH,
            SDL_WINDOW_METAL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
        if (!win) { std::fprintf(stderr, "[WINDOW] CreateWindow: %s\n", SDL_GetError()); return false; }
        SDL_MetalView view = SDL_Metal_CreateView(win);
        CAMetalLayer *layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(view);
        layer.device = dev;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;

        id<MTLRenderPipelineState> psoBlit = makeFsPso(@"fs_blit", MTLPixelFormatBGRA8Unorm);
        id<MTLRenderPipelineState> psoHud;
        {
            MTLRenderPipelineDescriptor *p = [MTLRenderPipelineDescriptor new];
            p.vertexFunction = fn_(@"vs_fullscreen");
            p.fragmentFunction = fn_(@"fs_hud");
            p.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            p.colorAttachments[0].blendingEnabled = YES;
            p.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
            p.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            p.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            NSError *e = nil;
            psoHud = [dev newRenderPipelineStateWithDescriptor:p error:&e];
            if (!psoHud) { std::fprintf(stderr, "[WINDOW] hud pso: %s\n",
                                        [[e localizedDescription] UTF8String]); return false; }
        }
        const int HUDW = 520, HUDH = 190;
        MTLTextureDescriptor *htd =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                              width:HUDW height:HUDH mipmapped:NO];
        htd.usage = MTLTextureUsageShaderRead;
        htd.storageMode = MTLStorageModeShared;
        id<MTLTexture> hudTex = [dev newTextureWithDescriptor:htd];
        std::vector<uint8_t> hudBuf(size_t(HUDW) * HUDH * 4, 0);

        // Free-fly is FDS's OWN Dynamic_Camera (see SceneIngest.h) — seeded from
        // the ingest camera so the window opens on the same pose the offscreen
        // renders use, and calibrated for this scene's depth range.
        FreeCamInit(scene);
        bool  freeFly = opt.freeFly, paused = false, mouseLook = false, running = true;
        float demoT = float(opt.loadOpt->demoT);
        LoadOptions lo = *opt.loadOpt;
        // THE CAMERA-INTERPOLATION FIX. `lo` drives Reanimate's RefreshCamera
        // every frame, and with a non-empty camPose RefreshCamera PINS the
        // camera to that review pose and takes FOV from FOV.Keys[0]. The
        // interactive arm cleared camPose only when --spline was passed at
        // STARTUP, so in a default `--window` run TAB into "SPLINE" showed a
        // frozen viewpoint — Animate_Objects was evaluating the authored spline
        // into sc.CameraHead correctly, and RefreshCamera was then overwriting
        // it. Clearing it here means the scripted camera is ALWAYS the engine's
        // per-frame spline evaluation (Spline_Calc_3D Source/Target +
        // Spline_Calc_1D Roll/FOV -> Kick_Camera -> CalcPersp), while free-fly
        // still starts from the review pose because FC was seeded above.
        lo.camPose.clear();

        std::fprintf(stderr,
            "[WINDOW] open %dx%d — camera is FDS's own Dynamic_Camera (the\n"
            "[WINDOW]   DisplaceTest / TAB-camera control set), spline is the\n"
            "[WINDOW]   engine's per-frame Spline_Calc evaluation.\n"
            "%s", opt.winW, opt.winH, kKeymap);

        uint64_t prevTick = SDL_GetPerformanceCounter();
        const double freq = double(SDL_GetPerformanceFrequency());
        double cpuAnimMs = 0, cpuUploadMs = 0, gpuFrameMs = 0;
        double emaFps = 0;
        int frameNo = 0;
        uint64_t vhash0 = VertexHash(scene);
        bool vertsEverChanged = false;

        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) running = false;
                else if (ev.type == SDL_KEYDOWN) {
                    switch (ev.key.keysym.sym) {
                        // ESC and Backspace both quit, as DisplaceTest's loop does.
                        case SDLK_ESCAPE: case SDLK_BACKSPACE: running = false; break;
                        case SDLK_TAB:
                            // Leaving spline mode must not teleport: FC is kept in
                            // sync with the scripted camera below, so free-fly
                            // resumes exactly where the spline had the eye.
                            freeFly = !freeFly; break;
                        case SDLK_SPACE:  paused = !paused; break;
                        case SDLK_LEFTBRACKET:  demoT -= 100.0f; break;
                        case SDLK_RIGHTBRACKET: demoT += 100.0f; break;
                        // G, rising edge only — DisplaceTest's pose dump.
                        case SDLK_g: FreeCamDumpPose(scene); break;
                        case SDLK_F1: std::fputs(kKeymap, stderr); break;
                        default: break;
                    }
                } else if (ev.type == SDL_MOUSEBUTTONDOWN) mouseLook = true;
                else if (ev.type == SDL_MOUSEBUTTONUP)   mouseLook = false;
                else if (ev.type == SDL_MOUSEMOTION && mouseLook) {
                    // A GpuBench addition, but through the same world-yaw /
                    // camera-local-pitch decomposition Dynamic_Camera uses.
                    if (!freeFly) { FreeCamSyncFromScene(scene); freeFly = true; }
                    FreeCamMouseLook(scene, -float(ev.motion.xrel) * 0.004f,
                                            -float(ev.motion.yrel) * 0.004f);
                }
            }
            const uint64_t now = SDL_GetPerformanceCounter();
            const float dt = float(double(now - prevTick) / freq);
            prevTick = now;
            emaFps = emaFps ? emaFps * 0.9 + (1.0 / std::max(dt, 1e-4f)) * 0.1
                            : 1.0 / std::max(dt, 1e-4f);

            if (!paused) demoT += opt.timeScale * dt;

            // --- CPU-side per-frame work, timed separately from GPU pass time ---
            const auto tAnim0 = std::chrono::steady_clock::now();
            Reanimate(scene, lo, demoT);
            const auto tAnim1 = std::chrono::steady_clock::now();
            cpuAnimMs = std::chrono::duration<double, std::milli>(tAnim1 - tAnim0).count();

            if (frameNo == 3 && VertexHash(scene) != vhash0) vertsEverChanged = true;

            // Camera. SPLINE = whatever Reanimate's Animate_Objects just
            // evaluated (the engine's Kochanek-Bartels Source/Target/Roll/FOV
            // splines through Kick_Camera + CalcPersp). FREE-FLY = the engine's
            // Dynamic_Camera, driven by the house key set.
            if (freeFly) {
                const uint8_t *k = SDL_GetKeyboardState(nullptr);
                FreeCamInput in;
                in.fwd       = k[SDL_SCANCODE_W];
                in.back      = k[SDL_SCANCODE_S] || k[SDL_SCANCODE_Z];
                in.left      = k[SDL_SCANCODE_A] || k[SDL_SCANCODE_END];
                in.right     = k[SDL_SCANCODE_D] || k[SDL_SCANCODE_PAGEDOWN];
                in.up        = k[SDL_SCANCODE_Q] || k[SDL_SCANCODE_KP_PLUS];
                in.down      = k[SDL_SCANCODE_E] || k[SDL_SCANCODE_KP_MINUS];
                in.yawLeft   = k[SDL_SCANCODE_LEFT];
                in.yawRight  = k[SDL_SCANCODE_RIGHT];
                in.pitchUp   = k[SDL_SCANCODE_UP];
                in.pitchDown = k[SDL_SCANCODE_DOWN];
                in.rollLeft  = k[SDL_SCANCODE_HOME];
                in.rollRight = k[SDL_SCANCODE_PAGEUP];
                in.slower    = k[SDL_SCANCODE_COMMA];
                in.faster    = k[SDL_SCANCODE_PERIOD];
                in.rotSlower = k[SDL_SCANCODE_K];
                in.rotFaster = k[SDL_SCANCODE_L];
                FreeCamStep(scene, in, dt);
            } else {
                // Keep FC under the scripted camera so TAB does not teleport.
                FreeCamSyncFromScene(scene);
            }

            // --- refresh the GPU-visible per-frame state ---
            const auto tUp0 = std::chrono::steady_clock::now();
            refreshFrameUniforms();
            refreshBatchUniforms();
            refreshLightBuffer();
            std::memcpy([lightBuf contents], lights.data(), lights.size() * sizeof(GpuLight));
            const auto tUp1 = std::chrono::steady_clock::now();
            cpuUploadMs = std::chrono::duration<double, std::milli>(tUp1 - tUp0).count();

            id<MTLCommandBuffer> cb = renderFrame();
            [cb waitUntilCompleted];
            gpuFrameMs = ([cb GPUEndTime] - [cb GPUStartTime]) * 1000.0;
            std::vector<double> perPass(kPasses, 0.0);
            if (sampleBuf) {
                NSData *d = [sampleBuf resolveCounterRange:NSMakeRange(0, NSUInteger(kPasses*2))];
                if (d && [d length] >= sizeof(MTLCounterResultTimestamp) * kPasses * 2) {
                    const auto *ts = static_cast<const MTLCounterResultTimestamp *>([d bytes]);
                    for (int p = 0; p < kPasses; ++p) {
                        const uint64_t a = ts[p*2].timestamp, b2 = ts[p*2+1].timestamp;
                        if (a != MTLCounterErrorValue && b2 != MTLCounterErrorValue && b2 > a)
                            perPass[p] = double(b2 - a) / 1e6;
                    }
                }
            }

            // --- HUD ---
            std::fill(hudBuf.begin(), hudBuf.end(), uint8_t(0));
            char line[160];
            int ly = 4;
            std::snprintf(line, sizeof line, "GPU FRAME %6.3F MS   %5.0F FPS", gpuFrameMs, emaFps);
            HudText(hudBuf.data(), HUDW, HUDH, 4, ly, 2, line, 255, 255, 255); ly += 18;
            static const char *pn[5] = {"SHADOW BAKE", "GBUFFER", "LIGHTING", "TONEMAP", "MIRROR"};
            for (int p = 0; p < kPasses; ++p) {
                std::snprintf(line, sizeof line, "  %-12s %6.3F MS", pn[p], perPass[p]);
                HudText(hudBuf.data(), HUDW, HUDH, 4, ly, 2, line, 170, 210, 255); ly += 15;
            }
            std::snprintf(line, sizeof line, "CPU ANIM %5.3F  UPLOAD %5.3F MS", cpuAnimMs, cpuUploadMs);
            HudText(hudBuf.data(), HUDW, HUDH, 4, ly, 2, line, 255, 220, 140); ly += 18;
            std::snprintf(line, sizeof line, "T=%6.0F FRAME %6.1F  %s%s",
                          demoT, scene.curFrame, freeFly ? "FREE-FLY" : "SPLINE",
                          paused ? " (PAUSED)" : "");
            HudText(hudBuf.data(), HUDW, HUDH, 4, ly, 2, line, 200, 255, 200); ly += 15;
            std::snprintf(line, sizeof line, "%d LIGHTS  %zu DRAWS  VERTS %s",
                          out.litLights, scene.batches.size(),
                          vertsEverChanged ? "RE-UPLOADED" : "STATIC (NO RE-UPLOAD)");
            HudText(hudBuf.data(), HUDW, HUDH, 4, ly, 2, line, 190, 190, 190); ly += 18;
            // The keymap belongs on screen, not only in the startup log — the
            // report that "I don't get the complete set of keys" was made at the
            // window, where the log had already scrolled.
            HudText(hudBuf.data(), HUDW, HUDH, 4, ly, 1,
                    "W/S,Z FWD/BACK  A,END/D,PGDN STRAFE  Q/E UP/DOWN", 150, 150, 150); ly += 11;
            HudText(hudBuf.data(), HUDW, HUDH, 4, ly, 1,
                    "ARROWS LOOK  HOME/PGUP ROLL  DRAG LOOK  , . SPEED  K L TURN", 150, 150, 150); ly += 11;
            HudText(hudBuf.data(), HUDW, HUDH, 4, ly, 1,
                    "TAB SPLINE  SPACE PAUSE  [ ] TIME  G POSE  F1 KEYS  ESC QUIT", 150, 150, 150);
            [hudTex replaceRegion:MTLRegionMake2D(0, 0, HUDW, HUDH) mipmapLevel:0
                        withBytes:hudBuf.data() bytesPerRow:HUDW * 4];

            // --- present ---
            @autoreleasepool {
                id<CAMetalDrawable> drawable = [layer nextDrawable];
                if (drawable) {
                    id<MTLCommandBuffer> pcb = [queue commandBuffer];
                    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                    rp.colorAttachments[0].texture = drawable.texture;
                    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                    id<MTLRenderCommandEncoder> e = [pcb renderCommandEncoderWithDescriptor:rp];
                    float ds[2] = {float(drawable.texture.width), float(drawable.texture.height)};
                    [e setRenderPipelineState:psoBlit];
                    [e setFragmentBytes:ds length:sizeof(ds) atIndex:1];
                    [e setFragmentTexture:ldrTex atIndex:0];
                    [e setFragmentSamplerState:bloomSamp atIndex:0];
                    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                    float hs[2] = {float(HUDW), float(HUDH)};
                    [e setRenderPipelineState:psoHud];
                    [e setFragmentBytes:hs length:sizeof(hs) atIndex:1];
                    [e setFragmentTexture:hudTex atIndex:0];
                    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                    [e endEncoding];
                    [pcb presentDrawable:drawable];
                    [pcb commit];
                }
            }
            // Periodic telemetry so the window's animation and per-pass costs are
            // verifiable from a log, not only from the on-screen HUD.
            if ((frameNo % 30) == 0)
                std::fprintf(stderr,
                    "[WINDOW] f=%4d t=%7.0f CurFrame=%7.1f cam=(%6.2f,%5.2f,%7.2f) %s | "
                    "gpu %6.3f ms (bake %.3f gbuf %.3f light %.3f tone %.3f) | "
                    "cpu anim %.3f upload %.3f | %.0f fps\n",
                    frameNo, demoT, scene.curFrame,
                    scene.camera.src[0], scene.camera.src[1], scene.camera.src[2],
                    freeFly ? "free-fly" : "spline",
                    gpuFrameMs, perPass[0], perPass[1], perPass[2], perPass[3],
                    cpuAnimMs, cpuUploadMs, emaFps);
            ++frameNo;
            if (opt.winFrames > 0 && frameNo >= opt.winFrames) running = false;
        }
        std::fprintf(stderr, "[WINDOW] closed after %d frames. verts re-uploaded: %s\n",
                     frameNo, vertsEverChanged ? "YES" : "NO (rigid-hierarchy animation)");
        SDL_Metal_DestroyView(view);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return true;
    }

    // ---- CPU-side per-frame cost, HEADLESS ---------------------------------
    // The comparison table needs the CPU work a real-time frame would pay
    // alongside the GPU passes, split the way the interactive path splits it:
    // ANIMATION (Reanimate = the engine's own Animate_Objects + the light /
    // camera / transform refresh) vs UPLOAD (rebuilding the batch uniform
    // blocks and the light buffer, then the memcpy). Measured here rather than
    // quoted from a --window run, because a visible window is the user's to
    // launch. The offscreen benchmark itself renders a PINNED pose and pays
    // NEITHER — so these are reported separately and never folded into the
    // GPU frame time.
    if (opt.cpuProf > 0 && opt.loadOpt) {
        using clk = std::chrono::steady_clock;
        LoadOptions lo = *opt.loadOpt;
        lo.verbose = false;
        double animMs = 0, upMs = 0;
        float t = float(opt.loadOpt->demoT);
        for (int i = 0; i < opt.cpuProf; ++i) {
            t += 1.0f;                       // a real tick, so splines re-evaluate
            const auto a0 = clk::now();
            Reanimate(scene, lo, t);
            const auto a1 = clk::now();
            refreshFrameUniforms();
            refreshBatchUniforms();
            refreshLightBuffer();
            std::memcpy([lightBuf contents], lights.data(),
                        lights.size() * sizeof(GpuLight));
            const auto a2 = clk::now();
            animMs += std::chrono::duration<double, std::milli>(a1 - a0).count();
            upMs   += std::chrono::duration<double, std::milli>(a2 - a1).count();
        }
        std::fprintf(stderr,
            "[DEFERRED] CPU per-frame (headless, %d iters): animation %.4f ms "
            "(Animate_Objects + light/camera/transform refresh), upload %.4f ms "
            "(%zu batch uniform blocks + %zu lights). NOT included in the GPU "
            "frame time -- the timed loop renders a pinned pose.\n",
            opt.cpuProf, animMs / opt.cpuProf, upMs / opt.cpuProf,
            scene.batches.size(), lights.size());
    }

    // ---- warmup + measure --------------------------------------------------
    std::fprintf(stderr, "[DEFERRED] warmup %d frames…\n", opt.warmup);
    for (int i = 0; i < opt.warmup; ++i) { id<MTLCommandBuffer> cb = renderFrame(); [cb waitUntilCompleted]; }

    const char *passNames[kPasses] = {"shadow-bake", "gbuffer", "lighting", "tonemap", "mirror"};
    std::vector<double> frameMs;
    std::vector<std::vector<double>> passMs(kPasses);

    for (int i = 0; i < opt.iters; ++i) {
        id<MTLCommandBuffer> cb = renderFrame();
        [cb waitUntilCompleted];
        const double ms = ([cb GPUEndTime] - [cb GPUStartTime]) * 1000.0;
        if (ms > 0.0) frameMs.push_back(ms);
        if (sampleBuf) {
            NSData *d = [sampleBuf resolveCounterRange:NSMakeRange(0, NSUInteger(kPasses * 2))];
            if (d && [d length] >= sizeof(MTLCounterResultTimestamp) * kPasses * 2) {
                const auto *ts = static_cast<const MTLCounterResultTimestamp *>([d bytes]);
                for (int p = 0; p < kPasses; ++p) {
                    const uint64_t a = ts[p * 2].timestamp, b = ts[p * 2 + 1].timestamp;
                    if (a == MTLCounterErrorValue || b == MTLCounterErrorValue || b <= a) continue;
                    passMs[p].push_back(double(b - a) / 1e6);   // GPU ticks are ns here
                }
            }
        }
    }

    if (frameMs.empty()) { std::fprintf(stderr, "[DEFERRED] no GPU timestamps\n"); return false; }
    // Workload census: what the culls actually removed, per frame, so the
    // comparison table's shadow row can state the batch count it measured
    // rather than the batch count that exists.
    if (opt.iters > 0) {
        const double perFrame = double(opt.iters);
        std::fprintf(stderr,
            "[DEFERRED] culling: main-view backface %s; shadow per-cube-face frustum %s "
            "-- shadow batch draws %.1f/frame, culled %.1f/frame (%.1f%% rejected)\n",
            opt.cull ? "ON" : "OFF", opt.shadowCull ? "ON" : "OFF",
            double(shadowBatchesDrawn) / perFrame,
            double(shadowBatchesCulled) / perFrame,
            (shadowBatchesDrawn + shadowBatchesCulled)
                ? 100.0 * double(shadowBatchesCulled)
                        / double(shadowBatchesDrawn + shadowBatchesCulled)
                : 0.0);
    }
    out.frame = {"frame", Percentile(frameMs, 0.5), Percentile(frameMs, 0.05), Percentile(frameMs, 0.95)};
    for (int p = 0; p < kPasses; ++p) {
        if (passMs[p].empty()) continue;
        out.passes.push_back({passNames[p],
                              Percentile(passMs[p], 0.5),
                              Percentile(passMs[p], 0.05),
                              Percentile(passMs[p], 0.95)});
    }

    if (!opt.outPath.empty()) {
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:ldrTex sourceSlice:0 sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(NSUInteger(W), NSUInteger(H), 1)
                    toTexture:stageTex destinationSlice:0 destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        const size_t rowBytes = size_t(W) * 4;
        std::vector<uint8_t> pix(rowBytes * size_t(H));
        [stageTex getBytes:pix.data() bytesPerRow:rowBytes
               fromRegion:MTLRegionMake2D(0, 0, NSUInteger(W), NSUInteger(H)) mipmapLevel:0];
        if (WritePPM(opt.outPath, pix.data(), W, H, rowBytes))
            std::fprintf(stderr, "[DEFERRED] wrote %s\n", opt.outPath.c_str());
    }
    return true;
}
}

}  // namespace gpubench
