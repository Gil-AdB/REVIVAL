#include "EnvBake.h"
#include "OffscreenView.h"

#include <Base/FDS_VARS.H>   // MainSurf, View, FList/SList/CAll, CntrX, XRes...
#include <Base/FDS_DECS.H>   // Render, Radix_Sort, Transform_Objects, CalcPersp,
                             // VESA_Surface2Global, Sachletz, getAlignedType
#include <Base/FrameState.h> // g_mainCamera, g_mainFaces
#include <Base/Scene.h>
#include <Base/Camera.h>
#include <Base/Texture.h>
#include <Base/Vector.h>

#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <vector>

// Build_YOffs_Table is commented out in FDS_DECS.H; declared extern wherever
// an offscreen surface is shaped (see DEMO/GreetsMirror.cpp).
extern void Build_YOffs_Table(VESA_Surface *VS);

namespace fds {

namespace {

// One cube face: an orthonormal (right, up, forward) basis. forward is the
// view direction; the engine reads Camera::Mat rows as [right; up; forward].
// Sampling uses the SAME basis it renders with, so the panorama is
// self-consistent regardless of the exact handedness chosen here.
struct CubeFace { Vector right, up, fwd; };
const CubeFace kCubeFaces[6] = {
    { { 0, 0,-1}, {0,1,0}, { 1, 0, 0} },  // +X
    { { 0, 0, 1}, {0,1,0}, {-1, 0, 0} },  // -X
    { { 1, 0, 0}, {0,0,-1},{ 0, 1, 0} },  // +Y
    { { 1, 0, 0}, {0,0,1}, { 0,-1, 0} },  // -Y
    { { 1, 0, 0}, {0,1,0}, { 0, 0, 1} },  // +Z
    { {-1, 0, 0}, {0,1,0}, { 0, 0,-1} },  // -Z
};

inline float dot(const Vector& a, const Vector& b) {
    return a.x*b.x + a.y*b.y + a.z*b.z;
}

inline int32_t iLog2(int32_t x) {
    union { uint32_t i; float f; } u{};
    u.f = float(x);
    return int32_t((u.i >> 23) - 127);
}

// Sample the six rendered cube faces in world direction d. Picks the
// dominant axis (the face whose forward maximises d·fwd) and projects
// through that face's 90°-FOV basis, matching the engine's screen mapping
// (screen y grows downward → the (1-b) flip below).
uint32_t sampleCube(const Vector& d, const std::vector<uint32_t> faces[6],
                    int res, uint32_t voidColor) {
    const float ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
    int f;
    if (ax >= ay && ax >= az)      f = d.x > 0 ? 0 : 1;
    else if (ay >= az)             f = d.y > 0 ? 2 : 3;
    else                           f = d.z > 0 ? 4 : 5;

    const CubeFace& cf = kCubeFaces[f];
    const float fwd = dot(d, cf.fwd);
    if (fwd <= 1e-6f) return voidColor;          // grazing / behind — shouldn't hit
    const float a = dot(d, cf.right) / fwd;      // [-1,1] across the face
    const float b = dot(d, cf.up)    / fwd;      // [-1,1] across the face
    int sx = int((a + 1.0f) * 0.5f * res);
    int sy = int((1.0f - b) * 0.5f * res);
    if (sx < 0) sx = 0; else if (sx >= res) sx = res - 1;
    if (sy < 0) sy = 0; else if (sy >= res) sy = res - 1;
    const uint32_t c = faces[f][size_t(sy) * size_t(res) + size_t(sx)];
    return (c & 0x00FFFFFFu) ? c : voidColor;    // empty (cleared) texel → void
}

}  // namespace

Texture* BakeEquirectPanorama(Scene* sc, const Vector& center,
                              const EnvBakeParams& params) {
    if (!sc) return nullptr;
    const int res = params.cubeRes;
    const int W = params.panoWidth, H = params.panoHeight;

    // Offscreen render target for one cube face. Owns its own Data/Z16; the
    // OffscreenViewScope below points MainSurf here and republishes the
    // engine globals (XRes/VPage/ZPage16/CntrE*) to match.
    VESA_Surface surf = {};
    surf.X = res; surf.Y = res;
    surf.BPP = 32; surf.CPP = 4;
    surf.BPSL = res * 4;
    surf.PageSize = res * res * 4;
    surf.Data = (byte*)std::malloc(size_t(res) * res * 4);
    surf.Z16  = (byte*)std::malloc(size_t(res) * res * sizeof(word));
    surf.Flip = MainSurf ? MainSurf->Flip : nullptr;
    if (!surf.Data || !surf.Z16) { std::free(surf.Data); std::free(surf.Z16); return nullptr; }
    Build_YOffs_Table(&surf);

    std::vector<uint32_t> faces[6];
    for (auto& fc : faces) fc.resize(size_t(res) * res);

    {
        OffscreenViewScope view(sc, &surf);
        view.publishSurface();               // XRes/CntrX/... from surf
        static Camera s_cam;
        view.setView(&s_cam);

        for (int i = 0; i < 6; ++i) {
            std::memset(&s_cam, 0, sizeof(s_cam));
            s_cam.ISource = center;
            s_cam.IFOV = 90.0f;
            const CubeFace& cf = kCubeFaces[i];
            s_cam.Mat[0][0] = cf.right.x; s_cam.Mat[0][1] = cf.right.y; s_cam.Mat[0][2] = cf.right.z;
            s_cam.Mat[1][0] = cf.up.x;    s_cam.Mat[1][1] = cf.up.y;    s_cam.Mat[1][2] = cf.up.z;
            s_cam.Mat[2][0] = cf.fwd.x;   s_cam.Mat[2][1] = cf.fwd.y;   s_cam.Mat[2][2] = cf.fwd.z;
            CalcPersp(&s_cam);

            // Clear to void color (each byte of voidColor) + empty Z.
            std::memset(surf.Data, params.voidColor & 0xFF, size_t(res) * res * 4);
            std::memset(surf.Z16, 0, size_t(res) * res * sizeof(word));

            Transform_Objects(sc, fds::g_mainCamera, fds::g_mainFaces);
            if (CAll != 0) {
                Radix_Sort(FList, SList, CAll);
                Render(RenderPath::ForceForward);
            }
            if (std::getenv("FDS_ENVBAKE_DUMP"))
                std::fprintf(stderr, "[ENVBAKE] face %d: CAll=%d NZP=%.2f FZP=%.2f "
                    "fwd=(%.0f,%.0f,%.0f)\n", i, int(CAll), sc->NZP, sc->FZP,
                    kCubeFaces[i].fwd.x, kCubeFaces[i].fwd.y, kCubeFaces[i].fwd.z);
            std::memcpy(faces[i].data(), surf.Data, size_t(res) * res * 4);
        }
    }   // scope exit restores MainSurf/View/FOV/clip planes

    std::free(surf.Data);
    std::free(surf.Z16);

    // Stitch to equirectangular. The (eu,ev)→direction mapping is the exact
    // inverse of the reflective lookup in RENDER/Transform.cpp, so a face
    // reflecting world-direction d samples the scene content baked for d:
    //   lat = PI*(0.5 - ev),  lon = 2*PI*(eu - 0.5) - PI/2
    //   d = ( -cos(lon)cos(lat), sin(lat), -sin(lon)cos(lat) )
    std::vector<uint32_t> pano(size_t(W) * H);
    for (int py = 0; py < H; ++py) {
        const float ev  = (float(py) + 0.5f) / float(H);
        const float lat = float(M_PI) * (0.5f - ev);
        const float cl  = std::cos(lat), sl = std::sin(lat);
        for (int px = 0; px < W; ++px) {
            const float eu  = (float(px) + 0.5f) / float(W);
            const float lon = 2.0f * float(M_PI) * (eu - 0.5f) - float(M_PI) * 0.5f;
            Vector d;
            d.x = -std::cos(lon) * cl;
            d.y = sl;
            d.z = -std::sin(lon) * cl;
            pano[size_t(py) * W + px] = sampleCube(d, faces, res, params.voidColor);
        }
    }

    // FDS_ENVBAKE_DUMP=1: write the stitched panorama (pre-Sachletz) to
    // /tmp for inspection.
    if (std::getenv("FDS_ENVBAKE_DUMP")) {
        if (FILE *f = std::fopen("/tmp/envbake_pano.ppm", "wb")) {
            std::fprintf(f, "P6\n%d %d\n255\n", W, H);
            for (int i = 0; i < W * H; ++i) {
                const uint32_t p = pano[size_t(i)];
                unsigned char rgb[3] = {
                    (unsigned char)((p >> 16) & 0xFF),
                    (unsigned char)((p >>  8) & 0xFF),
                    (unsigned char)((p      ) & 0xFF) };
                std::fwrite(rgb, 1, 3, f);
            }
            std::fclose(f);
        }
    }

    // Materialize into a Sachletz-tiled Texture the forward env filler reads.
    Texture* T = getAlignedType<Texture>(16);
    T->Flags = Txtr_Nomip | Txtr_Tiled;
    T->Data  = (byte*)_aligned_malloc(size_t(W) * H * 4, 16);
    std::memcpy(T->Data, pano.data(), size_t(W) * H * 4);
    T->BPP = 32;
    T->SizeX = W; T->LSizeX = iLog2(W);
    T->SizeY = H; T->LSizeY = iLog2(H);
    Sachletz((dword*)T->Data, W, H);
    T->Mipmap[0] = T->Data;
    T->numMipmaps = 1;
    return T;
}

}  // namespace fds
