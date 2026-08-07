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

namespace gpubench {
namespace {

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
    float pad3[3];
};

struct BatchUniforms {
    float rotRow0[4], rotRow1[4], rotRow2[4];
    float objPos[4];
    float baseColor[4];
    float matParams[4];
    float mapFlags[4];
    float misc[4];              // .x = mirror panel index (1-based)
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
    gpd.colorAttachments[3].pixelFormat = MTLPixelFormatR8Uint;   // mirror id
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

    auto makeFsPso = [&](NSString *fn, MTLPixelFormat fmt) -> id<MTLRenderPipelineState> {
        MTLRenderPipelineDescriptor *p = [MTLRenderPipelineDescriptor new];
        p.vertexFunction = [lib newFunctionWithName:@"vs_fullscreen"];
        p.fragmentFunction = [lib newFunctionWithName:fn];
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
        p.vertexFunction = [lib newFunctionWithName:@"vs_flare"];
        p.fragmentFunction = [lib newFunctionWithName:@"fs_flare"];
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
    id<MTLRenderPipelineState> psoBloomBright = makeFsPso(@"fs_bloom_bright", MTLPixelFormatRGBA16Float);
    id<MTLRenderPipelineState> psoBloomBlur   = makeFsPso(@"fs_bloom_blur",   MTLPixelFormatRGBA16Float);
    id<MTLRenderPipelineState> psoBloomAdd;
    {
        MTLRenderPipelineDescriptor *p = [MTLRenderPipelineDescriptor new];
        p.vertexFunction = [lib newFunctionWithName:@"vs_fullscreen"];
        p.fragmentFunction = [lib newFunctionWithName:@"fs_bloom_add"];
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
    id<MTLRenderPipelineState> psoLight   = makeFsPso(@"fs_lighting", MTLPixelFormatRGBA16Float);
    id<MTLRenderPipelineState> psoTonemap = makeFsPso(@"fs_tonemap",  MTLPixelFormatBGRA8Unorm);
    id<MTLRenderPipelineState> psoViz     = makeFsPso(@"fs_viz",      MTLPixelFormatBGRA8Unorm);
    if (!psoLight || !psoTonemap || !psoViz) return false;

    MTLDepthStencilDescriptor *dsd = [MTLDepthStencilDescriptor new];
    dsd.depthCompareFunction = MTLCompareFunctionGreater;   // reversed-Z
    dsd.depthWriteEnabled = YES;
    id<MTLDepthStencilState> dss = [dev newDepthStencilStateWithDescriptor:dsd];

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
    id<MTLTexture> gMirror = mkTarget(MTLPixelFormatR8Uint,      MTLStorageModePrivate);
    id<MTLTexture> gDepth  = mkTarget(MTLPixelFormatDepth32Float, MTLStorageModePrivate);
    id<MTLTexture> hdrTex  = mkTarget(MTLPixelFormatRGBA16Float, MTLStorageModePrivate);
    id<MTLTexture> ldrTex  = mkTarget(MTLPixelFormatBGRA8Unorm,  MTLStorageModePrivate);
    id<MTLTexture> stageTex = mkTarget(MTLPixelFormatBGRA8Unorm, MTLStorageModeShared);

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
        mMirror = mkTarget(MTLPixelFormatR8Uint,       MTLStorageModePrivate);
        mDepth  = mkTarget(MTLPixelFormatDepth32Float, MTLStorageModePrivate);
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
    // GREETS.CPP ~3005, verified by reading.
    fu.ambientFactor = 0.25f;
    fu.diffuseFactor = 1.0f;
    fu.specularFactor = 1.0f;
    fu.lightRangeScale = opt.lightRangeScale;
    fu.vizLight = opt.vizLight;
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
    };

    // Flare list. One draw per flaring omni (<= 10 here) rather than instancing:
    // each flare has its own generated texture, and 10 draws is not a cost worth
    // an argument-buffer for.
    struct FlareInst { float wpos[3]; float worldHalf; int tex; };
    std::vector<FlareInst> flares;
    for (const auto &L : scene.lights) {
        if (L.flareTexIndex < 0 || L.flareSize <= 0.0f) continue;
        if (!std::isfinite(L.pos[0])) continue;
        FlareInst fi{};
        for (int c = 0; c < 3; ++c) fi.wpos[c] = L.pos[c];
        // Half-extent in px = 2 * ImageSize * perspX * flareSize / z; fold the
        // z-independent part here, divide by view z at draw time.
        fi.worldHalf = 2.0f * scene.imageSize * scene.camera.perspX * L.flareSize;
        fi.tex = L.flareTexIndex;
        flares.push_back(fi);
    }
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
    auto drawScene = [&](id<MTLRenderCommandEncoder> enc, bool gbuffer) {
        [enc setVertexBuffer:vb offset:0 atIndex:0];
        for (size_t i = 0; i < scene.batches.size(); ++i) {
            const Batch &b = scene.batches[i];
            // Shadow pass: honour the same caster filter the CPU bake applies.
            if (!gbuffer && !b.castsShadow) continue;
            [enc setVertexBytes:&bus[i] length:sizeof(BatchUniforms) atIndex:2];
            if (gbuffer) {
                [enc setFragmentBytes:&bus[i] length:sizeof(BatchUniforms) atIndex:2];
                id<MTLTexture> a = (b.textureIndex   >= 0) ? texes[size_t(b.textureIndex)]   : texes[0];
                id<MTLTexture> n = (b.normalTexIndex >= 0) ? texes[size_t(b.normalTexIndex)] : texes[0];
                id<MTLTexture> r = (b.roughTexIndex  >= 0) ? texes[size_t(b.roughTexIndex)]  : texes[0];
                [enc setFragmentTexture:a atIndex:0];
                [enc setFragmentTexture:n atIndex:1];
                [enc setFragmentTexture:r atIndex:2];
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
                drawScene(enc, /*gbuffer=*/false);
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
    auto encodeGBuffer = [&](id<MTLCommandBuffer> cb, const FrameUniforms &u,
                             id<MTLTexture> tAlb, id<MTLTexture> tNrm,
                             id<MTLTexture> tPar, id<MTLTexture> tMir,
                             id<MTLTexture> tDep,
                             void (^counterHook)(MTLRenderPassDescriptor *)) {
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
        [enc setCullMode:MTLCullModeNone];
        [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
        [enc setFragmentBytes:&u length:sizeof(u) atIndex:1];
        [enc setFragmentSamplerState:samp atIndex:0];
        drawScene(enc, /*gbuffer=*/true);
        [enc endEncoding];
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
        if (nMirrors > 0 && opt.viz < 0) {
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
            encodeGBuffer(cb, fum, mAlbedo, mNormal, mParams, mMirror, mDepth, hook);
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
                [enc setFragmentSamplerState:shadowSamp atIndex:1];
                [enc setFragmentSamplerState:rawSamp atIndex:2];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [enc endEncoding];
            }
            // flare sprites inside the reflection (the CPU draws clone flares
            // in its mirrors); lights behind the plane are clipped.
            {
                const float clip4[4] = {N[0], N[1], N[2], m.d};
                encodeFlares(cb, fum, reflHdr[size_t(i)], mDepth, clip4);
            }
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
            [enc setFragmentSamplerState:shadowSamp atIndex:1];
            [enc setFragmentSamplerState:rawSamp atIndex:2];
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
            p.vertexFunction = [lib newFunctionWithName:@"vs_fullscreen"];
            p.fragmentFunction = [lib newFunctionWithName:@"fs_hud"];
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

        // Free-fly state, seeded from the ingest camera so the window opens on the
        // same pose the offscreen renders use.
        float eye[3] = {scene.camera.src[0], scene.camera.src[1], scene.camera.src[2]};
        // Derive yaw/pitch from the camera's forward row (Mat row 2 is forward).
        float fwd[3] = {scene.camera.rot[2][0], scene.camera.rot[2][1], scene.camera.rot[2][2]};
        float yaw = std::atan2(fwd[0], fwd[2]);
        float pitch = std::asin(std::max(-1.0f, std::min(1.0f, fwd[1])));
        bool  freeFly = opt.freeFly, paused = false, mouseLook = false, running = true;
        float demoT = float(opt.loadOpt->demoT);
        float speed = 12.0f;                 // world units / second
        LoadOptions lo = *opt.loadOpt;

        std::fprintf(stderr,
            "[WINDOW] open %dx%d. WASD+QE move, mouse-drag look, SHIFT fast, "
            "TAB free-fly/spline, SPACE pause, [ ] time scrub, ESC quit\n",
            opt.winW, opt.winH);

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
                        case SDLK_ESCAPE: running = false; break;
                        case SDLK_TAB:    freeFly = !freeFly; break;
                        case SDLK_SPACE:  paused = !paused; break;
                        case SDLK_LEFTBRACKET:  demoT -= 100.0f; break;
                        case SDLK_RIGHTBRACKET: demoT += 100.0f; break;
                        default: break;
                    }
                } else if (ev.type == SDL_MOUSEBUTTONDOWN) mouseLook = true;
                else if (ev.type == SDL_MOUSEBUTTONUP)   mouseLook = false;
                else if (ev.type == SDL_MOUSEMOTION && mouseLook) {
                    yaw   -= float(ev.motion.xrel) * 0.004f;
                    pitch -= float(ev.motion.yrel) * 0.004f;
                    pitch = std::max(-1.55f, std::min(1.55f, pitch));
                    freeFly = true;
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

            // Camera: free-fly, or the authored spline that Reanimate just moved.
            if (freeFly) {
                const uint8_t *k = SDL_GetKeyboardState(nullptr);
                const float f[3] = {std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                                    std::cos(yaw) * std::cos(pitch)};
                const float r[3] = {f[2], 0.0f, -f[0]};
                const float rl = std::sqrt(r[0]*r[0] + r[2]*r[2]);
                const float rn[3] = {r[0]/std::max(rl,1e-5f), 0.0f, r[2]/std::max(rl,1e-5f)};
                float v = speed * dt * ((k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]) ? 4.0f : 1.0f);
                if (k[SDL_SCANCODE_W]) for (int c=0;c<3;++c) eye[c] += f[c]*v;
                if (k[SDL_SCANCODE_S]) for (int c=0;c<3;++c) eye[c] -= f[c]*v;
                if (k[SDL_SCANCODE_D]) for (int c=0;c<3;++c) eye[c] += rn[c]*v;
                if (k[SDL_SCANCODE_A]) for (int c=0;c<3;++c) eye[c] -= rn[c]*v;
                if (k[SDL_SCANCODE_E]) eye[1] += v;
                if (k[SDL_SCANCODE_Q]) eye[1] -= v;
                // Build the view with the ENGINE's Kick_Camera, same as everywhere else.
                BuildViewMatrix(eye, f, scene.camera.rot);
                for (int c = 0; c < 3; ++c) scene.camera.src[c] = eye[c];
            } else {
                for (int c=0;c<3;++c) eye[c] = scene.camera.src[c];
                yaw = std::atan2(scene.camera.rot[2][0], scene.camera.rot[2][2]);
                pitch = std::asin(std::max(-1.0f, std::min(1.0f, scene.camera.rot[2][1])));
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
            HudText(hudBuf.data(), HUDW, HUDH, 4, ly, 2, line, 190, 190, 190);
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
