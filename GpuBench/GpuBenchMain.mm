// GpuBenchMain — standalone Metal renderer used as a BENCHMARK and ground-truth
// instrument. See docs/GPU_BENCHMARK_PLAN.md.
//
// Phase 2 scope: load greets through FDS, render its geometry with albedo
// textures from one review pose, OFFSCREEN, and report GPU frame timing.
// No scene lighting, no shadows, no HDR.
//
// This is NOT a shipping backend and shares nothing with FDS's rasterizer,
// clipper, or deferred kernel — it only links FDS to load the scene.
//
// Renders offscreen by default and writes a PPM; it never opens a window.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "SceneIngest.h"
#include "Deferred.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct FrameUniforms {
    float camRow0[4];   // float3 in MSL is 16-byte aligned
    float camRow1[4];
    float camRow2[4];
    float camSrc[4];
    float sx, ox, sy, oy;
    float dza, dzb, pad0, pad1;
};

struct BatchUniforms {
    float rotRow0[4];
    float rotRow1[4];
    float rotRow2[4];
    float objPos[4];
    float baseColor[4];
};

void Die(const char *what, NSError *err) {
    std::fprintf(stderr, "[GPUBENCH] FATAL: %s%s%s\n", what,
                 err ? " — " : "",
                 err ? [[err localizedDescription] UTF8String] : "");
    std::exit(1);
}

std::string ReadFile(const char *path) {
    FILE *f = std::fopen(path, "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string s(size_t(n < 0 ? 0 : n), '\0');
    if (n > 0 && std::fread(s.data(), 1, size_t(n), f) != size_t(n)) s.clear();
    std::fclose(f);
    return s;
}

bool WritePPM(const char *path, const uint8_t *bgra, int w, int h, size_t rowBytes) {
    FILE *f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<uint8_t> row(size_t(w) * 3);
    for (int y = 0; y < h; ++y) {
        const uint8_t *s = bgra + size_t(y) * rowBytes;
        for (int x = 0; x < w; ++x) {
            row[size_t(x) * 3 + 0] = s[size_t(x) * 4 + 2];  // R
            row[size_t(x) * 3 + 1] = s[size_t(x) * 4 + 1];  // G
            row[size_t(x) * 3 + 2] = s[size_t(x) * 4 + 0];  // B
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
}

double Percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double idx = p * double(v.size() - 1);
    const size_t lo = size_t(std::floor(idx)), hi = size_t(std::ceil(idx));
    return v[lo] + (v[hi] - v[lo]) * (idx - double(lo));
}

const char *kUsage =
    "GpuBench — standalone Metal deferred-renderer benchmark (Phase 2: geometry + albedo)\n"
    "\n"
    "Run from Runtime/ (asset paths are CWD-relative).\n"
    "\n"
    "  --fld=PATH        scene file            (default SCENES/GREETS.FLD)\n"
    "  --t=N             demo-timer pose       (default 5743, see docs/greets_review_poses.txt)\n"
    "  --cam=\"px,py,pz,fx,fy,fz\"   review pose; same string as FDS_GREETS_CAM\n"
    "  --xres=N --yres=N resolution            (default 1920x1080)\n"
    "  --warmup=N        untimed frames        (default 60 — Apple GPUs clock up)\n"
    "  --iters=N         timed frames          (default 300)\n"
    "  --out=PATH        write a PPM of the last frame (default gpubench.ppm; '' = none)\n"
    "  --shaders=DIR     shader source dir     (default alongside the binary, then ./shaders)\n"
    "  --no-draw         issue zero draws — measures the render-pass FLOOR (clear + store\n"
    "                    of the target) so the scene number can be reported net of it\n"
    "  --pass=albedo|deferred    albedo = Phase 2 arm (geometry + albedo, no lighting).\n"
    "                    deferred = Phase 3: G-buffer -> cube shadows -> PBR lighting ->\n"
    "                    ACES tonemap, with PER-PASS GPU timestamps. Default albedo.\n"
    "  --no-shadows      deferred only: skip the cube bake and the per-pixel tap\n"
    "  --rebake_all      deferred only: re-bake ALL cubes every frame instead of only the\n"
    "                    moving ones. greets caches static cubes, so this is NOT its policy.\n"
    "  --stages=1|2|3    deferred only: 1 = G-buffer only, 2 = +lighting, 3 = +tonemap.\n"
    "                    Per-encoder timestamps OVERLAP and do not sum to the frame; take\n"
    "                    pass costs from DIFFERENCES of whole-frame totals across stages.\n"
    "  --shadow_res=N    deferred only: static-omni cube face res (default 512, as greets)\n"
    "  --exposure=F      deferred only: tonemap exposure (default 1.0)\n"
    "  --viz=MODE        deferred only: albedo|normal|ao|depth|gloss|shadow|lights —\n"
    "                    per-stage verification output instead of the lit frame\n"
    "  --light_range_scale=F   deferred only, MEASUREMENT ONLY (changes pixels). greets'\n"
    "                    authored omni ranges are 3-20 units in a 60+ unit room, so the hard\n"
    "                    cutoff culls nearly every light for nearly every pixel. Scale up to\n"
    "                    measure the real per-light cost.\n"
    "  --dump_cube=N     deferred only, DIAGNOSTIC: read light N's baked shadow cube back\n"
    "                    to the CPU, print per-face stored-depth statistics (nonfinite /\n"
    "                    cleared / min-max / decoded world distance) and write a 3x2 face\n"
    "                    atlas PPM. Forces Shared storage on the cubes, so never a timing run.\n"
    "  --dump_cube_out=PATH  where the atlas goes (default gpubench_cube.ppm)\n"
    "  --no-bloom / --bloom_intensity=F / --bloom_threshold=F   greets defaults are ON,\n"
    "                    intensity 2.0, threshold 200 (the CPU's linear 0-255 radiance scale)\n"
    "  --no-flares / --flare_gain=F   omni flare sprites (the DEMO reference's bright pools)\n"
    "  --no-disco        do NOT synthesise GreetsDisco.cpp's 10 cone spotlights + glow\n"
    "                    omni. greets_disco defaults ON in DEMO, so these are PARITY --\n"
    "                    turning them off makes the arm dimmer than the shipped scene.\n"
    "  --no-stone_tex    do NOT apply DEMO's greets_stone_tex wall/floor override. The\n"
    "                    render is then the AUTHORED FLD wall, not the reviewed surface.\n"
    "  --help\n";

}  // namespace

int main(int argc, const char *argv[]) {
@autoreleasepool {
    gpubench::LoadOptions opt;
    // The primary review pose (docs/greets_review_poses.txt, t=5743 "primary hole repro").
    opt.camPose = "9.07557869,3.19592357,-52.9277191,-0.20672597,-0.140846997,0.968207836";
    int warmup = 60, iters = 300;
    std::string outPath = "gpubench.ppm";
    std::string shaderDir;
    bool noDraw = false;
    std::string passMode = "albedo";
    gpubench::DeferredOptions dopt;

    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        auto val = [&](const char *k) -> const char * {
            size_t n = std::strlen(k);
            // >= so an explicitly EMPTY value (e.g. --out= to suppress output)
            // still matches instead of falling through to "unknown arg".
            return (a.size() >= n && a.compare(0, n, k) == 0) ? a.c_str() + n : nullptr;
        };
        if (a == "--help" || a == "-h") { std::fputs(kUsage, stdout); return 0; }
        else if (const char *v = val("--fld="))     { static std::string s; s = v; opt.fldPath = s.c_str(); }
        else if (const char *v = val("--t="))       opt.demoT = std::atoi(v);
        else if (const char *v = val("--cam="))     opt.camPose = v;
        else if (const char *v = val("--xres="))    opt.xres = std::atoi(v);
        else if (const char *v = val("--yres="))    opt.yres = std::atoi(v);
        else if (const char *v = val("--warmup="))  warmup = std::atoi(v);
        else if (const char *v = val("--iters="))   iters = std::atoi(v);
        else if (const char *v = val("--out="))     outPath = v;
        else if (const char *v = val("--shaders=")) shaderDir = v;
        else if (a == "--no-draw")                  noDraw = true;
        else if (const char *v = val("--pass="))    passMode = v;
        else if (a == "--no-shadows")               dopt.shadows = false;
        else if (a == "--rebake_all")               dopt.rebakeAll = true;
        else if (const char *v = val("--stages="))   dopt.stages = std::atoi(v);
        else if (const char *v = val("--light_range_scale=")) dopt.lightRangeScale = float(std::atof(v));
        else if (const char *v = val("--viz_light="))  dopt.vizLight = std::atoi(v);
        else if (const char *v = val("--shadow_res=")) dopt.staticShadowRes = std::atoi(v);
        else if (const char *v = val("--exposure=")) dopt.exposure = float(std::atof(v));
        else if (const char *v = val("--dump_cube=")) dopt.dumpCube = std::atoi(v);
        else if (const char *v = val("--dump_cube_out=")) dopt.dumpCubePath = v;
        else if (a == "--no-stone_tex")             opt.stoneTex = false;
        else if (a == "--no-disco")                 opt.disco = false;
        else if (a == "--no-bloom")                 dopt.bloom = false;
        else if (const char *v = val("--bloom_intensity=")) dopt.bloomIntensity = float(std::atof(v));
        else if (const char *v = val("--bloom_threshold=")) dopt.bloomThreshold = float(std::atof(v)) / 255.0f;
        else if (a == "--no-flares")                dopt.flares = false;
        else if (const char *v = val("--flare_gain=")) dopt.flareGain = float(std::atof(v));
        else if (const char *v = val("--viz=")) {
            const std::string m(v);
            if      (m == "albedo") dopt.viz = 0;
            else if (m == "normal") dopt.viz = 1;
            else if (m == "ao")     dopt.viz = 2;
            else if (m == "depth")  dopt.viz = 3;
            else if (m == "gloss")  dopt.viz = 4;
            else if (m == "shadow") dopt.viz = 5;
            else if (m == "lights") dopt.viz = 6;
            else if (m == "shadowraw") dopt.viz = 7;
            else if (m == "ambient")  dopt.viz = 8;
            else if (m == "emissive") dopt.viz = 9;
            else if (m == "direct")   dopt.viz = 10;
            else { std::fprintf(stderr, "unknown --viz mode: %s\n", v); return 2; }
        }
        else { std::fprintf(stderr, "unknown arg: %s\n\n%s", argv[i], kUsage); return 2; }
    }

    // ---- scene ------------------------------------------------------------
    gpubench::Scene scene;
    if (!gpubench::Load(scene, opt)) Die("scene ingest failed", nil);

    // ---- device -----------------------------------------------------------
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) Die("no Metal device", nil);
    std::fprintf(stderr, "[GPUBENCH] device: %s (unified=%d)\n",
                 [[dev name] UTF8String], (int)[dev hasUnifiedMemory]);

    // ---- Phase 3: deferred arm --------------------------------------------
    if (passMode == "deferred") {
        std::string sp;
        std::vector<std::string> t;
        if (!shaderDir.empty()) t.push_back(shaderDir + "/deferred.metal");
        if (const char *exe = argv[0]) {
            std::string e(exe);
            size_t slash = e.find_last_of('/');
            if (slash != std::string::npos) t.push_back(e.substr(0, slash) + "/shaders/deferred.metal");
        }
        t.push_back("shaders/deferred.metal");
        t.push_back("../GpuBench/shaders/deferred.metal");
        for (const auto &p : t) if (!ReadFile(p.c_str()).empty()) { sp = p; break; }
        if (sp.empty()) Die("could not find deferred.metal (use --shaders=DIR)", nil);
        std::fprintf(stderr, "[GPUBENCH] shaders: %s\n", sp.c_str());

        dopt.warmup = warmup;
        dopt.iters = iters;
        dopt.outPath = outPath;
        gpubench::DeferredResult res;
        if (!gpubench::RunDeferred(scene, dopt, sp, res)) return 3;

        std::fprintf(stderr,
            "\n[GPUBENCH] ===== DEFERRED RESULT =====\n"
            "[GPUBENCH] scene=%s pose t=%d (CurFrame %.1f) %dx%d MSAA=1x\n"
            "[GPUBENCH] draws=%zu tris=%u textures=%u lights=%d\n"
            "[GPUBENCH] shadows: %s — %d cubes / %d faces / %.2f Mtexels (static %d^2, moving %d^2)\n"
            "[GPUBENCH]   static cubes baked ONCE (Omni_StaticShadow, as greets caches them): %.3f ms\n"
            "[GPUBENCH]   per-frame re-bake: %d moving cube(s) = %d faces%s\n"
            "[GPUBENCH] PBR: GGX + Smith-Schlick + Schlick F, Karis split-sum env BRDF,\n"
            "[GPUBENCH]      Fdez-Aguera multiscatter, (1-F) diffuse energy, L2 SH ambient\n"
            "[GPUBENCH] GPU ms, median (p5/p95), %d frames after %d warmup:\n",
            opt.fldPath, opt.demoT, scene.curFrame, scene.xres, scene.yres,
            scene.batches.size(), scene.faceCount, scene.texturesLoaded, res.litLights,
            dopt.shadows ? "ON" : "OFF", res.shadowCubes, res.shadowFaces,
            double(res.shadowTexels) / 1e6, dopt.staticShadowRes, dopt.movingShadowRes,
            res.staticBakeMs,
            dopt.rebakeAll ? res.shadowCubes : res.movingCubes,
            6 * (dopt.rebakeAll ? res.shadowCubes : res.movingCubes),
            dopt.rebakeAll ? "  [--rebake_all: FULL cold bake, not greets' policy]" : "",
            iters, warmup);
        for (const auto &p : res.passes)
            std::fprintf(stderr, "[GPUBENCH]   %-14s %8.4f  (%.4f / %.4f)\n",
                         p.name.c_str(), p.median, p.p5, p.p95);
        std::fprintf(stderr,
            "[GPUBENCH]   %-14s %8.4f  (%.4f / %.4f)\n"
            "[GPUBENCH]   => %.1f FPS equivalent\n",
            "FRAME TOTAL", res.frame.median, res.frame.p5, res.frame.p95,
            res.frame.median > 0.0 ? 1000.0 / res.frame.median : 0.0);
        if (dopt.viz >= 0)
            std::fprintf(stderr, "[GPUBENCH] NOTE: --viz active, the lighting pass was "
                                 "replaced by a debug output — timings are not the lit path.\n");
        return 0;
    }

    // ---- shaders (runtime MSL compile; no offline `metal` on this machine) --
    std::string src;
    std::vector<std::string> tries;
    if (!shaderDir.empty()) tries.push_back(shaderDir + "/albedo.metal");
    if (const char *exe = argv[0]) {
        std::string e(exe);
        size_t slash = e.find_last_of('/');
        if (slash != std::string::npos)
            tries.push_back(e.substr(0, slash) + "/shaders/albedo.metal");
    }
    tries.push_back("shaders/albedo.metal");
    tries.push_back("../GpuBench/shaders/albedo.metal");
    std::string usedPath;
    for (const auto &p : tries) { src = ReadFile(p.c_str()); if (!src.empty()) { usedPath = p; break; } }
    if (src.empty()) Die("could not find albedo.metal (use --shaders=DIR)", nil);
    std::fprintf(stderr, "[GPUBENCH] shaders: %s\n", usedPath.c_str());

    NSError *err = nil;
    MTLCompileOptions *copts = [MTLCompileOptions new];
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(src.c_str()) options:copts error:&err];
    if (!lib) Die("MSL compile failed", err);

    id<MTLFunction> vs = [lib newFunctionWithName:@"vs_albedo"];
    id<MTLFunction> fs = [lib newFunctionWithName:@"fs_albedo"];
    if (!vs || !fs) Die("shader entry points missing", nil);

    // ---- pipeline ---------------------------------------------------------
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat3;  vd.attributes[0].offset = 0;   vd.attributes[0].bufferIndex = 0;
    vd.attributes[1].format = MTLVertexFormatFloat3;  vd.attributes[1].offset = 12;  vd.attributes[1].bufferIndex = 0;
    vd.attributes[2].format = MTLVertexFormatFloat2;  vd.attributes[2].offset = 24;  vd.attributes[2].bufferIndex = 0;
    vd.layouts[0].stride = sizeof(gpubench::Vertex);
    vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vs;
    pd.fragmentFunction = fs;
    pd.vertexDescriptor = vd;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!pso) Die("pipeline creation failed", err);

    // Reversed-Z: ndc 1 at near, 0 at far. Clear to 0, keep the greater value.
    MTLDepthStencilDescriptor *dsd = [MTLDepthStencilDescriptor new];
    dsd.depthCompareFunction = MTLCompareFunctionGreater;
    dsd.depthWriteEnabled = YES;
    id<MTLDepthStencilState> dss = [dev newDepthStencilStateWithDescriptor:dsd];

    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    // Piramid UVs span roughly U[-561..38] V[-40..562] (measured) — wrap is mandatory.
    sd.sAddressMode = MTLSamplerAddressModeRepeat;
    sd.tAddressMode = MTLSamplerAddressModeRepeat;
    sd.mipFilter = MTLSamplerMipFilterLinear;
    sd.maxAnisotropy = 1;
    id<MTLSamplerState> samp = [dev newSamplerStateWithDescriptor:sd];

    // ---- buffers ----------------------------------------------------------
    const size_t vbBytes = scene.verts.size() * sizeof(gpubench::Vertex);
    id<MTLBuffer> vb = [dev newBufferWithBytes:scene.verts.data()
                                        length:vbBytes
                                       options:MTLResourceStorageModeShared];

    // ---- textures: upload linear data, let the GPU build the mip chain -----
    std::vector<id<MTLTexture>> texes;
    texes.reserve(scene.textures.size());
    id<MTLCommandQueue> queue = [dev newCommandQueue];
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
                 mipmapLevel:0
                   withBytes:img.rgba.data()
                 bytesPerRow:NSUInteger(img.w) * 4];
            [blit generateMipmapsForTexture:t];
            texes.push_back(t);
        }
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
    }

    // ---- render targets (offscreen) ---------------------------------------
    MTLTextureDescriptor *ctd =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                          width:NSUInteger(scene.xres)
                                                         height:NSUInteger(scene.yres)
                                                      mipmapped:NO];
    ctd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    ctd.storageMode = MTLStorageModeShared;
    id<MTLTexture> colorTex = [dev newTextureWithDescriptor:ctd];

    MTLTextureDescriptor *dtd =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                          width:NSUInteger(scene.xres)
                                                         height:NSUInteger(scene.yres)
                                                      mipmapped:NO];
    dtd.usage = MTLTextureUsageRenderTarget;
    dtd.storageMode = MTLStorageModePrivate;
    id<MTLTexture> depthTex = [dev newTextureWithDescriptor:dtd];

    // ---- uniforms ---------------------------------------------------------
    // Engine pixel mapping (docs/GRAPHICS_PIPELINE.md §5):
    //   px = cntrEX + (X/Z)*FOVX ,  py = cntrEY - (Y/Z)*FOVY
    // Metal viewport (upper-left origin):
    //   px = ( ndc.x*0.5 + 0.5)*W ,  py = (-ndc.y*0.5 + 0.5)*H
    // Solving for ndc and writing clip = ndc*w with w = Z:
    const float W = float(scene.xres), H = float(scene.yres);
    FrameUniforms fu{};
    for (int c = 0; c < 3; ++c) {
        fu.camRow0[c] = scene.camera.rot[0][c];
        fu.camRow1[c] = scene.camera.rot[1][c];
        fu.camRow2[c] = scene.camera.rot[2][c];
    }
    for (int c = 0; c < 3; ++c) fu.camSrc[c] = scene.camera.src[c];
    fu.sx = 2.0f * scene.camera.perspX / W;
    fu.ox = 2.0f * scene.camera.cntrEX / W - 1.0f;
    fu.sy = 2.0f * scene.camera.perspY / H;
    fu.oy = 1.0f - 2.0f * scene.camera.cntrEY / H;
    {   // reversed-Z: ndc 1 at near, 0 at far
        const float n = scene.camera.nearZ, f = scene.camera.farZ;
        fu.dza = -n / (f - n);
        fu.dzb = n * f / (f - n);
    }

    std::vector<BatchUniforms> bus(scene.batches.size());
    for (size_t i = 0; i < scene.batches.size(); ++i) {
        const auto &b = scene.batches[i];
        BatchUniforms &u = bus[i];
        for (int c = 0; c < 3; ++c) {
            u.rotRow0[c] = b.rot[0][c];
            u.rotRow1[c] = b.rot[1][c];
            u.rotRow2[c] = b.rot[2][c];
            u.objPos[c] = b.pos[c];
            u.baseColor[c] = b.baseColor[c];
        }
        u.baseColor[3] = (b.textureIndex >= 0) ? 1.0f : 0.0f;
    }

    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = colorTex;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0.02, 0.02, 0.04, 1.0);
    rp.depthAttachment.texture = depthTex;
    rp.depthAttachment.loadAction = MTLLoadActionClear;
    rp.depthAttachment.storeAction = MTLStoreActionDontCare;
    rp.depthAttachment.clearDepth = 0.0;   // reversed-Z

    auto renderOne = [&](void) -> id<MTLCommandBuffer> {
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:pso];
        [enc setDepthStencilState:dss];
        // Winding: the engine's own backface test lives in Transform_Objects and
        // is not reproduced here, so cull nothing. Correct with a depth buffer,
        // just more fragment work — stated in the report rather than hidden.
        [enc setCullMode:MTLCullModeNone];
        [enc setVertexBuffer:vb offset:0 atIndex:0];
        [enc setVertexBytes:&fu length:sizeof(fu) atIndex:1];
        [enc setFragmentSamplerState:samp atIndex:0];
        for (size_t i = 0; noDraw ? false : i < scene.batches.size(); ++i) {
            const auto &b = scene.batches[i];
            [enc setVertexBytes:&bus[i] length:sizeof(BatchUniforms) atIndex:2];
            [enc setFragmentBytes:&bus[i] length:sizeof(BatchUniforms) atIndex:2];
            if (b.textureIndex >= 0 && b.textureIndex < int(texes.size()))
                [enc setFragmentTexture:texes[size_t(b.textureIndex)] atIndex:0];
            else if (!texes.empty())
                [enc setFragmentTexture:texes[0] atIndex:0];   // bound but unused
            [enc drawPrimitives:MTLPrimitiveTypeTriangle
                    vertexStart:NSUInteger(b.firstVertex)
                    vertexCount:NSUInteger(b.vertexCount)];
        }
        [enc endEncoding];
        [cb commit];
        return cb;
    };

    // ---- warmup + measure --------------------------------------------------
    std::fprintf(stderr, "[GPUBENCH] warmup %d frames…\n", warmup);
    for (int i = 0; i < warmup; ++i) { id<MTLCommandBuffer> cb = renderOne(); [cb waitUntilCompleted]; }

    std::vector<double> gpuMs;
    gpuMs.reserve(size_t(iters));
    for (int i = 0; i < iters; ++i) {
        id<MTLCommandBuffer> cb = renderOne();
        [cb waitUntilCompleted];
        const double ms = ([cb GPUEndTime] - [cb GPUStartTime]) * 1000.0;
        if (ms > 0.0) gpuMs.push_back(ms);
    }

    if (gpuMs.empty()) Die("no GPU timestamps captured", nil);

    const double med = Percentile(gpuMs, 0.50);
    std::fprintf(stderr,
        "\n[GPUBENCH] ===== RESULT =====\n"
        "[GPUBENCH] scene=%s pose t=%d (CurFrame %.1f) %dx%d MSAA=1x%s\n"
        "[GPUBENCH] draws=%zu tris=%u gpuVerts=%zu textures=%u\n"
        "[GPUBENCH] GPU frame ms over %zu frames (after %d warmup):\n"
        "[GPUBENCH]   median %.4f   p5 %.4f   p95 %.4f   min %.4f   max %.4f\n"
        "[GPUBENCH]   => %.1f FPS equivalent\n",
        opt.fldPath, opt.demoT, scene.curFrame, scene.xres, scene.yres,
        noDraw ? "  [--no-draw: RENDER-PASS FLOOR, clear+store only]" : "",
        noDraw ? size_t(0) : scene.batches.size(),
        noDraw ? 0u : scene.faceCount, scene.verts.size(), scene.texturesLoaded,
        gpuMs.size(), warmup,
        med, Percentile(gpuMs, 0.05), Percentile(gpuMs, 0.95),
        *std::min_element(gpuMs.begin(), gpuMs.end()),
        *std::max_element(gpuMs.begin(), gpuMs.end()),
        med > 0.0 ? 1000.0 / med : 0.0);

    // ---- readback ---------------------------------------------------------
    if (!outPath.empty()) {
        const size_t rowBytes = size_t(scene.xres) * 4;
        std::vector<uint8_t> pixels(rowBytes * size_t(scene.yres));
        [colorTex getBytes:pixels.data()
              bytesPerRow:rowBytes
               fromRegion:MTLRegionMake2D(0, 0, NSUInteger(scene.xres), NSUInteger(scene.yres))
              mipmapLevel:0];
        if (WritePPM(outPath.c_str(), pixels.data(), scene.xres, scene.yres, rowBytes))
            std::fprintf(stderr, "[GPUBENCH] wrote %s\n", outPath.c_str());
        else
            std::fprintf(stderr, "[GPUBENCH] WARNING: could not write %s\n", outPath.c_str());
    }
    return 0;
}
}
