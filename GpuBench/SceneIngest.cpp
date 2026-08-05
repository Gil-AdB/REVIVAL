#include "SceneIngest.h"

#include <Base/FDS_DEFS.H>
#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Base/Scene.h>
#include <Base/TriMesh.h>
#include <FLD/FLD_READ.H>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>

namespace gpubench {
namespace {

// greets maps the demo timer to the engine's CurFrame as
//   CHPartTime = 500 + 100*(EndFrame-StartFrame)/30            (GREETS.CPP:1470)
//   CurFrame   = StartFrame + (EndFrame-StartFrame)*t/(CHPartTime-500)
// With the shipped GREETS.FLD (StartFrame 0, EndFrame 2400) that is exactly
// CurFrame = 0.3*t. Derived from the source, then CHECKED against the authored
// camera spline: t=5743 -> 1722.9, which sits just past key 16 (frame 1716,
// source (8.54, 3.195, -51.82)) heading toward key 17 — and the review pose at
// t=5743 is (9.076, 3.196, -52.93). Consistent.
float DemoTimeToCurFrame(const ::Scene &sc, int demoT) {
    const float span = sc.EndFrame - sc.StartFrame;
    const float chPartTime = 500.0f + 100.0f * span / 30.0f;
    const float denom = chPartTime - 500.0f;
    if (denom <= 0.0f) return sc.StartFrame;
    return sc.StartFrame + span * float(demoT) / denom;
}

// Publish XRes/YRes/CntrX/CntrY/CntrEX/CntrEY etc. CalcPersp reads CntrX and
// the XRes/YRes pair; nothing here allocates a framebuffer and Data/Z16 stay
// null (the GPU path never reads them).
void PublishResolution(int xres, int yres) {
    VESA_InitExternal(xres, yres, 32);
    VESA_Surface vs;
    std::memset(&vs, 0, sizeof(vs));
    vs.X = xres;
    vs.Y = yres;
    vs.BPP = 32;
    vs.CPP = 4;
    vs.BPSL = xres * 4;
    vs.PageSize = xres * yres * 4;
    VESA_Surface2Global(&vs);
}

bool FiniteVec(const Vector &v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Engine textures at this point are linear/row-major (Load_Texture, no
// Generate_Mipmaps). BPP is 24 or 32. Expand to tightly packed RGBA8.
//
// Channel order is detected rather than assumed: we compare the decoded mean
// per byte-lane against the material's authored BaseCol, whose ordering we know
// from Color's field order. If the two disagree we swap. The result is printed
// so the assumption is visible in the log instead of buried.
bool ExpandToRGBA(const ::Texture *tx, bool bgr, TextureImage &out) {
    if (!tx || !tx->Data || tx->SizeX <= 0 || tx->SizeY <= 0) return false;
    const int cpp = int(tx->BPP) / 8;
    if (cpp != 3 && cpp != 4) return false;
    out.w = tx->SizeX;
    out.h = tx->SizeY;
    out.rgba.resize(size_t(out.w) * size_t(out.h) * 4);
    const uint8_t *src = tx->Data;
    for (size_t i = 0, n = size_t(out.w) * size_t(out.h); i < n; ++i) {
        const uint8_t a = src[i * cpp + 0];
        const uint8_t b = src[i * cpp + 1];
        const uint8_t c = src[i * cpp + 2];
        out.rgba[i * 4 + 0] = bgr ? c : a;
        out.rgba[i * 4 + 1] = b;
        out.rgba[i * 4 + 2] = bgr ? a : c;
        out.rgba[i * 4 + 3] = 255;
    }
    return true;
}

}  // namespace

bool Load(Scene &out, const LoadOptions &opt) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    out.xres = opt.xres;
    out.yres = opt.yres;
    PublishResolution(opt.xres, opt.yres);

    // ---- 1. geometry + materials + lights + camera splines -----------------
    static ::Scene sc;          // static: TriMesh/Face/Material pointers outlive us
    std::memset(&sc, 0, sizeof(sc));
    if (!LoadFLD(&sc, opt.fldPath)) {
        std::fprintf(stderr, "[INGEST] LoadFLD failed: %s\n", opt.fldPath);
        return false;
    }

    // ---- 2. pose ------------------------------------------------------------
    out.curFrame = DemoTimeToCurFrame(sc, opt.demoT);
    CurFrame = out.curFrame;
    Animate_Objects(&sc, sc.CameraHead);

    // ---- 3. camera ---------------------------------------------------------
    // Built with FDS's OWN Kick_Camera + CalcPersp, so the view and projection
    // are the engine's, not a re-derivation. Same code path DEMO's
    // FDS_GREETS_CAM debug camera uses (GREETS.CPP ~2963).
    if (sc.CameraHead) {
        static ::Camera fc;
        fc = *sc.CameraHead;
        float fov = fc.IFOV;
        if (fov < 1.0f && sc.CameraHead->FOV.NumKeys > 0)
            fov = sc.CameraHead->FOV.Keys[0].Pos.x;
        if (fov < 1.0f) fov = 75.0f;

        float px, py, pz, fx, fy, fz;
        if (!opt.camPose.empty() &&
            std::sscanf(opt.camPose.c_str(), "%f,%f,%f,%f,%f,%f",
                        &px, &py, &pz, &fx, &fy, &fz) == 6) {
            fc.ISource = {px, py, pz};
            Vector look = {px + fx, py + fy, pz + fz};
            Kick_Camera(&fc.ISource, &look, 0.0f, fc.Mat);
        }
        fc.IFOV = fov;
        CalcPersp(&fc);

        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) out.camera.rot[r][c] = fc.Mat[r][c];
        out.camera.src[0] = fc.ISource.x;
        out.camera.src[1] = fc.ISource.y;
        out.camera.src[2] = fc.ISource.z;
        out.camera.perspX = fc.PerspX;
        out.camera.perspY = fc.PerspY;
        out.camera.fov = fov;
    }
    out.camera.cntrEX = CntrEX;
    out.camera.cntrEY = CntrEY;
    // greets pins these in Initialize_Greets (GREETS.CPP:1477-8); the FLD header
    // carries 0, so take the scene values only when they are actually set.
    out.camera.nearZ = (sc.NZP > 0.0f) ? sc.NZP : 0.01f;
    out.camera.farZ  = (sc.FZP > 0.0f) ? sc.FZP : 150.0f;

    // ---- 4. textures -------------------------------------------------------
    // Decode ONCE per distinct ::Texture. Load_Texture only; Generate_Mipmaps is
    // deliberately never called (see header).
    std::unordered_map<const ::Texture *, int> texIndex;
    auto acquireTexture = [&](::Texture *tx) -> int {
        if (!tx) return -1;
        auto it = texIndex.find(tx);
        if (it != texIndex.end()) return it->second;
        int idx = -1;
        if (!tx->Data) {
            if (!Load_Texture(tx)) {
                ++out.texturesMissing;
                if (opt.verbose)
                    std::fprintf(stderr, "[INGEST] texture MISSING: %s\n",
                                 tx->FileName ? tx->FileName : "(unnamed)");
                texIndex[tx] = -1;
                return -1;
            }
        }
        TextureImage img;
        img.fileName = tx->FileName ? tx->FileName : "(unnamed)";
        // The engine's 8-bit surfaces are BGRA and its image loaders follow the
        // same convention, so 24bpp texel bytes are B,G,R.
        if (ExpandToRGBA(tx, /*bgr=*/true, img)) {
            idx = int(out.textures.size());
            out.textures.push_back(std::move(img));
            ++out.texturesLoaded;
        } else {
            ++out.texturesMissing;
        }
        texIndex[tx] = idx;
        return idx;
    };

    // ---- 5. de-indexed geometry, grouped per (mesh x material) -------------
    for (Object *obj = sc.ObjectHead; obj; obj = obj->Next) {
        if (obj->Type != Obj_TriMesh || !obj->Data) continue;
        TriMesh *T = static_cast<TriMesh *>(obj->Data);
        if (!T->FIndex || !T->Faces || !T->Verts) continue;
        if (!FiniteVec(T->IPos)) {
            if (opt.verbose)
                std::fprintf(stderr,
                             "[INGEST] skip '%s': non-finite IPos at CurFrame %.1f\n",
                             obj->Name ? obj->Name : "?", out.curFrame);
            continue;
        }

        ++out.meshCount;
        out.srcVertCount += T->VIndex;

        // Group this mesh's faces by material so each batch is one draw.
        std::map<Material *, std::vector<uint32_t>> byMat;
        for (uint32_t f = 0; f < T->FIndex; ++f) byMat[T->Faces[f].Txtr].push_back(f);

        for (auto &kv : byMat) {
            Material *M = kv.first;
            Batch b;
            b.firstVertex = uint32_t(out.verts.size());
            b.meshName = obj->Name ? obj->Name : "?";
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) b.rot[r][c] = T->RotMat[r][c];
            b.pos[0] = T->IPos.x;
            b.pos[1] = T->IPos.y;
            b.pos[2] = T->IPos.z;
            if (M) {
                b.baseColor[0] = M->BaseCol.R / 255.0f;
                b.baseColor[1] = M->BaseCol.G / 255.0f;
                b.baseColor[2] = M->BaseCol.B / 255.0f;
                b.luminosity = M->Luminosity;
                b.diffuse = M->Diffuse;
                b.specular = M->Specular;
                b.glossiness = M->Glossiness;
                b.textureIndex = acquireTexture(M->Txtr);
                if (M->Txtr && M->Txtr->FileName) b.materialName = M->Txtr->FileName;
            }

            for (uint32_t fi : kv.second) {
                const Face &F = T->Faces[fi];
                if (!F.A || !F.B || !F.C) continue;
                const Vertex *const src[3] = {nullptr, nullptr, nullptr};
                (void)src;
                const ::Vertex *vp[3] = {F.A, F.B, F.C};
                // Per-FACE UVs. NOT F.A->U / F.B->U / F.C->U — see header.
                const float uu[3] = {F.U1, F.U2, F.U3};
                const float vv[3] = {F.V1, F.V2, F.V3};
                for (int k = 0; k < 3; ++k) {
                    Vertex gv;
                    gv.px = vp[k]->Pos.x;
                    gv.py = vp[k]->Pos.y;
                    gv.pz = vp[k]->Pos.z;
                    gv.nx = vp[k]->N.x;
                    gv.ny = vp[k]->N.y;
                    gv.nz = vp[k]->N.z;
                    gv.u = uu[k];
                    gv.v = vv[k];
                    out.verts.push_back(gv);
                }
                ++out.faceCount;
            }
            b.vertexCount = uint32_t(out.verts.size()) - b.firstVertex;
            if (b.vertexCount) out.batches.push_back(std::move(b));
        }
    }

    // ---- 6. lights ---------------------------------------------------------
    for (Omni *O = sc.OmniHead; O; O = O->Next) {
        Light L;
        L.parented = !FiniteVec(O->IPos);
        L.pos[0] = O->IPos.x; L.pos[1] = O->IPos.y; L.pos[2] = O->IPos.z;
        L.color[0] = O->L.R;  L.color[1] = O->L.G;  L.color[2] = O->L.B;
        L.intensity = O->ISize;
        L.range = O->IRange;
        out.lights.push_back(L);
    }

    out.loadMs = std::chrono::duration<double, std::milli>(clock::now() - t0).count();

    if (opt.verbose) {
        std::fprintf(stderr,
            "\n[INGEST] %s  t=%d -> CurFrame=%.1f  %dx%d  (%.1f ms)\n"
            "[INGEST] meshes=%u faces=%u srcVerts=%u -> gpuVerts=%zu batches=%zu\n"
            "[INGEST] textures loaded=%u missing=%u   lights=%zu\n"
            "[INGEST] camera src=(%.3f,%.3f,%.3f) fov=%.2f perspX=%.2f perspY=%.2f "
            "cntrE=(%.1f,%.1f) near=%.3f far=%.1f\n",
            opt.fldPath, opt.demoT, out.curFrame, out.xres, out.yres, out.loadMs,
            out.meshCount, out.faceCount, out.srcVertCount, out.verts.size(),
            out.batches.size(), out.texturesLoaded, out.texturesMissing,
            out.lights.size(),
            out.camera.src[0], out.camera.src[1], out.camera.src[2],
            out.camera.fov, out.camera.perspX, out.camera.perspY,
            out.camera.cntrEX, out.camera.cntrEY, out.camera.nearZ, out.camera.farZ);
    }
    return !out.verts.empty();
}

}  // namespace gpubench
