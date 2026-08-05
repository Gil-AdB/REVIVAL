#include "SceneIngest.h"

#include <Base/FDS_DEFS.H>
#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Base/Scene.h>
#include <Base/TriMesh.h>
#include <FLD/FLD_READ.H>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

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
// Generate_Mipmaps). Expand to tightly packed RGBA8.
//
// Byte order is NOT guessed. Both engine loaders produce B,G,R(,A):
// LoadPNG (IMGCODE.CPP:1037) takes stb_image's RGBA and swaps lanes 0<->2, and
// the 8/24-bit paths follow the same BGRA convention as VPage. So lane 0 is
// blue and lane 2 is red for every format we touch here.
//
// ALPHA IS PRESERVED for 32-bit sources: the greets stone albedos carry a baked
// AO map in alpha (Mat_AoInAlpha) and the deferred kernel reads occlusion from
// it. Forcing alpha to 255 would silently discard that.
bool ExpandToRGBA(const ::Texture *tx, TextureImage &out) {
    if (!tx || !tx->Data || tx->SizeX <= 0 || tx->SizeY <= 0) return false;
    const int cpp = int(tx->BPP) / 8;
    if (cpp != 1 && cpp != 3 && cpp != 4) return false;
    out.w = tx->SizeX;
    out.h = tx->SizeY;
    out.rgba.resize(size_t(out.w) * size_t(out.h) * 4);
    const uint8_t *src = tx->Data;
    for (size_t i = 0, n = size_t(out.w) * size_t(out.h); i < n; ++i) {
        if (cpp == 1) {   // single-channel (height / roughness) — replicate
            const uint8_t g = src[i];
            out.rgba[i * 4 + 0] = g;
            out.rgba[i * 4 + 1] = g;
            out.rgba[i * 4 + 2] = g;
            out.rgba[i * 4 + 3] = 255;
        } else {
            out.rgba[i * 4 + 0] = src[i * cpp + 2];   // R  (source lane 2)
            out.rgba[i * 4 + 1] = src[i * cpp + 1];   // G
            out.rgba[i * 4 + 2] = src[i * cpp + 0];   // B  (source lane 0)
            out.rgba[i * 4 + 3] = (cpp == 4) ? src[i * cpp + 3] : 255;
        }
    }
    return true;
}

// Replicates DEMO's --greets_stone_tex override (DEMO/GREETS.CPP:1508-1600).
//
// The engine does this as a FILENAME REPOINT performed before Preprocess_Scene
// loads any texture, so the sidecars go through the normal pipeline. We do the
// same, except we never call Generate_Mipmaps (the GPU builds its own mips), so
// the data stays linear and uploads directly.
//
// Deliberately NOT replicated: the Sobel normal-map bake fallback (we require an
// authored greets_*_n.png, which exists), MakeHeight8 / MakeNormal16 packing
// (memory tricks for the CPU kernel), the cone map, and the horizon map. Those
// are CPU-side accelerations, not surface content.
struct StoneOverride {
    const char *matName;
    const char *albedo, *height, *normal, *rough;
    float parallaxScale;
};
constexpr StoneOverride kStoneOverrides[] = {
    {"rooms", "TEXTURES/greets_wall.png",  "TEXTURES/greets_wall_h.png",
              "TEXTURES/greets_wall_n.png", "TEXTURES/greets_wall_r.png", 1.00f},
    // floor: offset-parallax swims on the grazing, densely-tiled floor, so
    // GREETS.CPP dials it down to 0.25. Same number here.
    {"floor", "TEXTURES/greets_floor.png", "TEXTURES/greets_floor_h.png",
              "TEXTURES/greets_floor_n.png", "TEXTURES/greets_floor_r.png", 0.25f},
};

::Texture *MakeSidecar(const char *fileName) {
    auto *tx = new ::Texture();
    tx->FileName = strdup(fileName);
    return tx;
}

// Returns the number of materials overridden.
int ApplyStoneTex(Material *M, bool verbose) {
    if (!M || !M->Name || !M->Txtr) return 0;
    for (const auto &o : kStoneOverrides) {
        if (std::strcmp(M->Name, o.matName) != 0) continue;
        // the previous FileName is engine-owned; leaking it matches GREETS.CPP
        M->Txtr->FileName = strdup(o.albedo);
        M->Txtr->BPP = 0;                     // force (re)load
        M->Txtr->Data = nullptr;
        M->Flags |= Mat_AoInAlpha;            // albedo alpha = baked AO
        // Honor an AUTHORED ParallaxScale (persisted by the editor into the LWO
        // 'RVSF' sub-chunk); only stamp the code default when unauthored. Same
        // guard as GREETS.CPP — stomping it unconditionally is the bug that made
        // editor parallax edits silently do nothing.
        if (M->ParallaxScale == 1.0f) M->ParallaxScale = o.parallaxScale;
        if (!M->NormalMap)    M->NormalMap    = MakeSidecar(o.normal);
        if (!M->RoughnessMap) M->RoughnessMap = MakeSidecar(o.rough);
        if (!M->HeightMap)    M->HeightMap    = MakeSidecar(o.height);
        if (verbose)
            std::fprintf(stderr,
                "[INGEST] stone-tex '%s' -> %s (+n/+r/+h), parallaxScale=%.2f, AO-in-alpha\n",
                M->Name, o.albedo, M->ParallaxScale);
        return 1;
    }
    return 0;
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
        if (ExpandToRGBA(tx, img)) {
            idx = int(out.textures.size());
            out.textures.push_back(std::move(img));
            ++out.texturesLoaded;
        } else {
            ++out.texturesMissing;
        }
        texIndex[tx] = idx;
        return idx;
    };

    // ---- 4b. stone-tex override (BEFORE any texture is decoded) ------------
    // docs/GPU_BENCHMARK_PLAN.md §3.2. Must run before acquireTexture touches
    // anything, exactly as GREETS.CPP runs before Preprocess_Scene.
    if (opt.stoneTex) {
        std::vector<Material *> seenMats;
        int overridden = 0;
        for (TriMesh *T = sc.TriMeshHead; T; T = T->Next) {
            if (!T->Faces) continue;
            for (uint32_t f = 0; f < T->FIndex; ++f) {
                Material *M = T->Faces[f].Txtr;
                if (!M) continue;
                if (std::find(seenMats.begin(), seenMats.end(), M) != seenMats.end()) continue;
                seenMats.push_back(M);
                overridden += ApplyStoneTex(M, opt.verbose);
            }
        }
        if (opt.verbose && overridden == 0)
            std::fprintf(stderr,
                "[INGEST] WARNING: --greets_stone_tex requested but NO material named "
                "'rooms' or 'floor' was found. The wall is the AUTHORED FLD wall, not the "
                "reviewed surface — do NOT run a displacement arm on this.\n");
    }

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
                b.parallaxScale = M->ParallaxScale;
                b.aoInAlpha = (M->Flags & Mat_AoInAlpha) != 0;
                b.textureIndex   = acquireTexture(M->Txtr);
                b.normalTexIndex = acquireTexture(M->NormalMap);
                b.roughTexIndex  = acquireTexture(M->RoughnessMap);
                b.heightTexIndex = acquireTexture(M->HeightMap);
                b.materialName = M->Name ? M->Name
                               : (M->Txtr && M->Txtr->FileName ? M->Txtr->FileName : "?");
                // Shadow-caster filter, byte-for-byte the CPU bake's predicate
                // (FDS/RENDER/Shadows.cpp:703-724, `looksEmissive` + `shouldSkip`).
                auto looksEmissive = [](const char *n) -> bool {
                    if (!n) return false;
                    for (const char *p = n; *p; ++p) {
                        if ((p[0]=='l'||p[0]=='L') && (p[1]=='a'||p[1]=='A') &&
                            (p[2]=='m'||p[2]=='M') && (p[3]=='p'||p[3]=='P')) return true;
                        if ((p[0]=='e'||p[0]=='E') && (p[1]=='m'||p[1]=='M') &&
                            (p[2]=='i'||p[2]=='I')) return true;
                    }
                    return false;
                };
                b.castsShadow = !((M->Flags & (Mat_Transparent | Mat_Additive | Mat_SkipZ))
                                  || looksEmissive(M->Name));
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

    // ---- 6. lights + ambient ------------------------------------------------
    out.ambient[0] = sc.Ambient.R;
    out.ambient[1] = sc.Ambient.G;
    out.ambient[2] = sc.Ambient.B;
    out.skyZenith[0] = sc.SkyZenith.R;
    out.skyZenith[1] = sc.SkyZenith.G;
    out.skyZenith[2] = sc.SkyZenith.B;
    out.skyNadir[0] = sc.SkyNadir.R;
    out.skyNadir[1] = sc.SkyNadir.G;
    out.skyNadir[2] = sc.SkyNadir.B;
    // Which omnis are MECH-ATTACHED (greets' "moving" class, re-baked per frame at
    // greets_moving_omni_shadow_res). Detected from the FLD Object hierarchy --
    // NOT from a non-finite IPos, which only happens when CurFrame is outside the
    // authored range and would report zero moving lights at every real pose.
    std::vector<const void *> parentedOmnis;
    for (Object *o = sc.ObjectHead; o; o = o->Next)
        if (o->Type == Obj_Omni && o->Parent && o->Data) parentedOmnis.push_back(o->Data);

    for (Omni *O = sc.OmniHead; O; O = O->Next) {
        Light L;
        L.parented = std::find(parentedOmnis.begin(), parentedOmnis.end(),
                               (const void *)O) != parentedOmnis.end();
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
