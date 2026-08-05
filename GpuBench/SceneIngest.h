// SceneIngest — turn an FDS scene (FLD + LWO + textures) into flat buffers a
// GPU can consume, with ZERO involvement of the software rasterizer, the
// clipper, the deferred kernel, or SDL.
//
// This is the load half of the standalone GPU benchmark
// (docs/GPU_BENCHMARK_PLAN.md). It links FDS *purely for loading*, so the
// input is provably the same bytes, through the same code, that DEMO renders.
//
// Two properties of the engine's data model drive the shape of the output and
// are NOT negotiable (see docs/ENGINE.md and GPU_BENCHMARK_PLAN.md §2.4):
//
//   1. UVs must be read from the FACE (Face::U1..V3), never from the vertex.
//      A vertex shared between faces of different projection orientation has
//      its per-vertex U/V clobbered by whichever face was mapped last. So the
//      vertex buffer is DE-INDEXED: 3 unshared vertices per triangle.
//
//   2. Texture pixels must be taken straight after Load_Texture() and BEFORE
//      Generate_Mipmaps(). Load_Texture leaves Data linear/row-major;
//      Generate_Mipmaps is what block-tiles ("Sachletz") it into the swizzled
//      layout the CPU rasterizers require. We never call it — the GPU builds
//      its own mips.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gpubench {

// 32 bytes, interleaved. Positions/normals are OBJECT space; the model matrix
// travels separately per batch so the GPU does the full model->view->clip
// transform (rather than us pre-baking world space on the CPU, which would
// quietly move work off the side under test).
struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};

// One draw per (mesh x material) run of triangles.
struct Batch {
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;   // always a multiple of 3
    int      textureIndex = -1; // -1 = untextured, use baseColor
    int      normalTexIndex = -1;
    int      roughTexIndex = -1;
    int      heightTexIndex = -1;
    float    baseColor[3] = {1, 1, 1};
    float    luminosity = 0.0f, diffuse = 1.0f, specular = 0.0f;
    uint32_t glossiness = 0;
    float    parallaxScale = 1.0f;
    bool     aoInAlpha = false;   // albedo alpha holds baked AO (Mat_AoInAlpha)
    // Does this material act as a solid shadow occluder? Reproduces the CPU
    // bake's caster filter EXACTLY (FDS/RENDER/Shadows.cpp:713-724): skip
    // Mat_Transparent | Mat_Additive | Mat_SkipZ, plus any material whose NAME
    // contains "lamp" or "emi" (the FLD carries no emissive flag, so the engine
    // infers it from the name). This is not an optimisation — every greets omni
    // is authored INSIDE a `lamp light` fixture ~0.3 units across, so without the
    // filter each light bakes the inside of its own housing and the whole room
    // reads as shadowed.
    bool     castsShadow = true;
    // Model matrix: FDS Matrix is float[3][3], ROW-major, applied as row-dot-v
    // (Animate_Objects has already folded IScale into the rows and resolved the
    // parent hierarchy into IPos). worldPos = rot * objPos + pos.
    float    rot[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
    float    pos[3] = {0, 0, 0};
    std::string meshName;
    std::string materialName;
};

struct TextureImage {
    std::string fileName;
    int32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;   // expanded to tightly packed RGBA8
};

struct Light {
    float pos[3] = {0, 0, 0};
    float color[3] = {0, 0, 0};   // 0..255 as authored
    float intensity = 0.0f;       // Omni::ISize
    float range = 0.0f;           // Omni::IRange — HARD cutoff, not inverse-square
    bool  parented = false;       // mech-attached (position was NaN pre-hierarchy)
};

struct Camera {
    float rot[3][3] = {{1,0,0},{0,1,0},{0,0,1}};   // view matrix, row-major row-dot-v
    float src[3] = {0, 0, 0};                      // eye position, world space
    float perspX = 0.0f, perspY = 0.0f;            // == FOVX / FOVY
    float cntrEX = 0.0f, cntrEY = 0.0f;
    float fov = 0.0f;
    float nearZ = 0.01f, farZ = 150.0f;
};

struct Scene {
    std::vector<Vertex>       verts;
    std::vector<Batch>        batches;
    std::vector<TextureImage> textures;
    std::vector<Light>        lights;
    Camera                    camera;
    float                     ambient[3] = {0, 0, 0};    // Scene::Ambient, 0..255
    // Authored backdrop gradient (Scene::SkyZenith / SkyNadir, 0..255). Projected
    // into L2 SH for the --sh_ambient path, so the ambient term is real authored
    // content rather than a flat constant.
    float                     skyZenith[3] = {0, 0, 0};
    float                     skyNadir[3] = {0, 0, 0};

    int      xres = 0, yres = 0;
    float    curFrame = 0.0f;
    // Census, for the report.
    uint32_t meshCount = 0;
    uint32_t faceCount = 0;
    uint32_t srcVertCount = 0;    // pre-de-index, i.e. what the CPU transforms
    uint32_t texturesLoaded = 0;
    uint32_t texturesMissing = 0;
    double   loadMs = 0.0;
};

struct LoadOptions {
    const char *fldPath = "SCENES/GREETS.FLD";
    int         xres = 1920;
    int         yres = 1080;
    // Demo-timer value from docs/greets_review_poses.txt. Mapped to the
    // engine's CurFrame with the greets scene's own formula (see .cpp).
    int         demoT = 5743;
    // "px,py,pz,fx,fy,fz" as in FDS_GREETS_CAM. Empty = use the scripted camera.
    std::string camPose;
    bool        verbose = true;
    // Replicate DEMO's --greets_stone_tex override (default ON there, so ON here).
    // Without it the wall this renders is the AUTHORED FLD wall, not the surface
    // the user actually reviews — see docs/GPU_BENCHMARK_PLAN.md §3.2. No
    // displacement arm may run with this off.
    bool        stoneTex = true;
};

// Returns false on failure. Never opens a window and never touches VPage/ZPage16
// beyond letting VESA_Surface2Global publish the projection globals that
// CalcPersp reads.
bool Load(Scene &out, const LoadOptions &opt);

}  // namespace gpubench
