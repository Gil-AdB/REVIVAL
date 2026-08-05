#include "Deferred.h"

#import <Foundation/Foundation.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

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
};

struct BatchUniforms {
    float rotRow0[4], rotRow1[4], rotRow2[4];
    float objPos[4];
    float baseColor[4];
    float matParams[4];
    float mapFlags[4];
};

struct GpuLight {
    float pos[4];
    float color[4];
    float range, invRange;
    int32_t shadowIndex;
    float shadowNear, shadowFar, pad0, pad1, pad2;
};

struct ShadowUniforms {
    float row0[4], row1[4], row2[4];
    float lightPos[4];
    float dza, dzb, pad0, pad1;
};

constexpr int kMaxShadowCubes = 16;

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

bool RunDeferred(const Scene &scene, const DeferredOptions &opt,
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
    vd.layouts[0].stride = sizeof(Vertex);

    MTLRenderPipelineDescriptor *gpd = [MTLRenderPipelineDescriptor new];
    gpd.vertexFunction = [lib newFunctionWithName:@"vs_gbuffer"];
    gpd.fragmentFunction = [lib newFunctionWithName:@"fs_gbuffer"];
    gpd.vertexDescriptor = vd;
    gpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    gpd.colorAttachments[1].pixelFormat = MTLPixelFormatRG16Snorm;
    gpd.colorAttachments[2].pixelFormat = MTLPixelFormatRGBA8Unorm;
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
    id<MTLTexture> gDepth  = mkTarget(MTLPixelFormatDepth32Float, MTLStorageModePrivate);
    id<MTLTexture> hdrTex  = mkTarget(MTLPixelFormatRGBA16Float, MTLStorageModePrivate);
    id<MTLTexture> ldrTex  = mkTarget(MTLPixelFormatBGRA8Unorm,  MTLStorageModePrivate);
    id<MTLTexture> stageTex = mkTarget(MTLPixelFormatBGRA8Unorm, MTLStorageModeShared);

    // ---- shadow cubes -----------------------------------------------------
    std::vector<id<MTLTexture>> cubes;
    std::vector<bool> cubeIsMoving;
    std::vector<int> lightCube(scene.lights.size(), -1);
    std::vector<float> cubeNear(scene.lights.size(), 0.05f);
    std::vector<float> cubeFar(scene.lights.size(), 1.0f);
    if (opt.shadows) {
        for (size_t i = 0; i < scene.lights.size(); ++i) {
            const Light &L = scene.lights[i];
            if (!std::isfinite(L.pos[0]) || L.range <= 0.0f) continue;
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
            td.storageMode = (opt.dumpCube >= 0) ? MTLStorageModeShared
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

    // ---- uniforms ---------------------------------------------------------
    FrameUniforms fu{};
    for (int c = 0; c < 3; ++c) {
        fu.camRow0[c] = scene.camera.rot[0][c];
        fu.camRow1[c] = scene.camera.rot[1][c];
        fu.camRow2[c] = scene.camera.rot[2][c];
        fu.camSrc[c] = scene.camera.src[c];
    }
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

    std::vector<GpuLight> lights;
    for (size_t i = 0; i < scene.lights.size(); ++i) {
        const Light &L = scene.lights[i];
        if (!std::isfinite(L.pos[0]) || L.range <= 0.0f) continue;
        GpuLight g{};
        for (int c = 0; c < 3; ++c) {
            g.pos[c] = L.pos[c];
            // Linearise the COLOUR only, then apply ISize as the linear intensity
            // it is. Squaring (colour x ISize) together also squared the
            // intensity, making every ISize=0.5 omni 4x too dim.
            const float c01 = L.color[c] / 255.0f;
            g.color[c] = c01 * c01 * L.intensity;
        }
        g.range = L.range;
        g.invRange = 1.0f / L.range;
        g.shadowIndex = lightCube[i];
        g.shadowNear = cubeNear[i];
        g.shadowFar = cubeFar[i];
        lights.push_back(g);
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
            "[DEFERRED]   [%zu] pos=(%7.2f,%6.2f,%8.2f) rgb=(%.3f,%.3f,%.3f) range=%5.1f "
            "cube=%d %s  nearestGeom=%.3f (%s/%s) vertsWithin1u=%ld\n",
            i, lights[i].pos[0], lights[i].pos[1], lights[i].pos[2],
            lights[i].color[0], lights[i].color[1], lights[i].color[2],
            lights[i].range, lights[i].shadowIndex,
            lights[i].shadowIndex >= 0
                ? (cubeIsMoving[size_t(lights[i].shadowIndex)] ? "(moving 128^2)" : "(static 512^2)")
                : "(no shadow)",
            nearest, nearestMesh, nearestMat, within);
    }
    fu.numLights = uint32_t(lights.size());
    out.litLights = int(lights.size());
    if (lights.empty()) { std::fprintf(stderr, "[DEFERRED] no usable lights\n"); return false; }
    id<MTLBuffer> lightBuf = [dev newBufferWithBytes:lights.data()
                                             length:lights.size() * sizeof(GpuLight)
                                            options:MTLResourceStorageModeShared];

    float sh[9][4];
    ProjectSkyGradientToSH(scene.skyZenith, scene.skyNadir, sh);
    id<MTLBuffer> shBuf = [dev newBufferWithBytes:sh length:sizeof(sh)
                                          options:MTLResourceStorageModeShared];

    std::vector<BatchUniforms> bus(scene.batches.size());
    for (size_t i = 0; i < scene.batches.size(); ++i) {
        const Batch &b = scene.batches[i];
        BatchUniforms &u = bus[i];
        for (int c = 0; c < 3; ++c) {
            u.rotRow0[c] = b.rot[0][c];
            u.rotRow1[c] = b.rot[1][c];
            u.rotRow2[c] = b.rot[2][c];
            u.objPos[c] = b.pos[c];
            // authored base colour is 0..1 sRGB-ish; square to linear
            u.baseColor[c] = b.baseColor[c] * b.baseColor[c];
        }
        u.baseColor[3] = (b.textureIndex >= 0) ? 1.0f : 0.0f;
        u.matParams[0] = b.diffuse;
        u.matParams[1] = b.specular;
        u.matParams[2] = std::min(1.0f, float(b.glossiness) / 128.0f);
        u.matParams[3] = b.luminosity;
        u.mapFlags[0] = (b.normalTexIndex >= 0) ? 1.0f : 0.0f;
        u.mapFlags[1] = (b.roughTexIndex >= 0) ? 1.0f : 0.0f;
        u.mapFlags[2] = b.aoInAlpha ? 1.0f : 0.0f;
        u.mapFlags[3] = b.parallaxScale;
    }

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

    const int kPasses = 4;               // shadow, gbuffer, lighting, tonemap
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
                if (lights[k].shadowIndex == int(ci)) { li = k; break; }
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
                [enc setVertexBytes:&su length:sizeof(su) atIndex:1];
                drawScene(enc, /*gbuffer=*/false);
                [enc endEncoding];
            }
        }
    };

    // One-time STATIC bake, outside the timed loop, as greets caches it.
    {
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        bakeCubes(cb, /*movingOnly=*/false, /*timed=*/false);
        [cb commit];
        [cb waitUntilCompleted];
        out.staticBakeMs = ([cb GPUEndTime] - [cb GPUStartTime]) * 1000.0;
    }

    // ---- --dump_cube: read the baked depth back and LOOK at it -------------
    if (opt.dumpCube >= 0) {
        if (opt.dumpCube >= int(lights.size())) {
            std::fprintf(stderr, "[DUMPCUBE] light %d out of range (%zu lights)\n",
                         opt.dumpCube, lights.size());
        } else if (lights[size_t(opt.dumpCube)].shadowIndex < 0) {
            std::fprintf(stderr, "[DUMPCUBE] light %d has no cube\n", opt.dumpCube);
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

    auto renderFrame = [&]() -> id<MTLCommandBuffer> {
        id<MTLCommandBuffer> cb = [queue commandBuffer];

        // --- pass 0: per-frame DYNAMIC shadow bake (moving omnis only) ---
        if (opt.shadows) bakeCubes(cb, /*movingOnly=*/!opt.rebakeAll, /*timed=*/true);

        // --- pass 1: G-buffer ---
        {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = gAlbedo;
            rp.colorAttachments[1].texture = gNormal;
            rp.colorAttachments[2].texture = gParams;
            for (int i = 0; i < 3; ++i) {
                rp.colorAttachments[i].loadAction = MTLLoadActionClear;
                rp.colorAttachments[i].storeAction = MTLStoreActionStore;
                rp.colorAttachments[i].clearColor = MTLClearColorMake(0, 0, 0, 0);
            }
            rp.depthAttachment.texture = gDepth;
            rp.depthAttachment.loadAction = MTLLoadActionClear;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            rp.depthAttachment.clearDepth = 0.0;
            attachCounters(rp, 1);
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            [enc setRenderPipelineState:psoGBuf];
            [enc setDepthStencilState:dss];
            [enc setCullMode:MTLCullModeNone];
            [enc setVertexBytes:&fu length:sizeof(fu) atIndex:1];
            [enc setFragmentSamplerState:samp atIndex:0];
            drawScene(enc, /*gbuffer=*/true);
            [enc endEncoding];
        }

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
            [enc setFragmentSamplerState:shadowSamp atIndex:1];
            [enc setFragmentSamplerState:rawSamp atIndex:2];
            if (viz) {
                uint32_t m = uint32_t(opt.viz);
                [enc setFragmentBytes:&m length:sizeof(m) atIndex:4];
            }
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [enc endEncoding];
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

    // ---- warmup + measure --------------------------------------------------
    std::fprintf(stderr, "[DEFERRED] warmup %d frames…\n", opt.warmup);
    for (int i = 0; i < opt.warmup; ++i) { id<MTLCommandBuffer> cb = renderFrame(); [cb waitUntilCompleted]; }

    const char *passNames[kPasses] = {"shadow-bake", "gbuffer", "lighting", "tonemap"};
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
