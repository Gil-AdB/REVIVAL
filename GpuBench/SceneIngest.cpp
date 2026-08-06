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
    // Flare sprite world scale. GREETS.CPP:3060 sets ImageSize = 0.25; the
    // default is 1000.0 (city scale), which would fill the screen here. The
    // blitter's half-extent in pixels is 2 * ImageSize * perspX * flareSize / z
    // (FILLERS.CPP: Size = ImageSize*RZ*PerspX*FlareSize, edgeLen = 2*Size, and
    // Spriter treats its width argument as the HALF-extent).
    ImageSize = 0.25f;
    out.imageSize = ImageSize;
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

    // Spot shadow camera via the ENGINE's Kick_Camera, same call Shadows.cpp makes.
    auto fillSpotShadowCam = [](Light &L) {
        Vector idir{L.dir[0], L.dir[1], L.dir[2]};
        if (std::fabs(idir.x) < 1e-4f && std::fabs(idir.z) < 1e-4f) idir.x = 0.01f;
        Vector src{L.pos[0], L.pos[1], L.pos[2]};
        Vector targ{src.x + idir.x, src.y + idir.y, src.z + idir.z};
        Matrix M;
        Kick_Camera(&src, &targ, 0.0f, M);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) L.shadowRot[r][c] = M[r][c];
        const float cosOuter = std::max(0.01f, L.cosOuter);
        L.shadowTanHalfFov = std::tan(std::acos(cosOuter) * 1.10f);
    };

    for (Omni *O = sc.OmniHead; O; O = O->Next) {
        Light L;
        L.parented = std::find(parentedOmnis.begin(), parentedOmnis.end(),
                               (const void *)O) != parentedOmnis.end();
        L.pos[0] = O->IPos.x; L.pos[1] = O->IPos.y; L.pos[2] = O->IPos.z;
        L.color[0] = O->L.R;  L.color[1] = O->L.G;  L.color[2] = O->L.B;
        L.intensity = O->ISize;
        L.range = O->IRange;
        L.isSpot = (O->Type == Light_SpotLight);
        L.dir[0] = O->IDir.x; L.dir[1] = O->IDir.y; L.dir[2] = O->IDir.z;
        L.cosInner = O->HotSpot;
        L.cosOuter = O->FallOff;
        L.shadowRes = O->shadowMapRes;
        L.castsShadow = true;
        // Flare sprite. The flare MATERIAL is not created by LoadFLD — it comes
        // from Init_Flares (FDS/MISC/PREPROC.CPP:778-809), which runs inside
        // Preprocess_Scene and which GpuBench does not call. So reproduce its
        // body: one procedural 256^2 flare per DISTINCT omni colour, built by the
        // engine's own Generate_RGBFlare, shared between omnis of equal colour.
        // (MatLib insertion is skipped — we own the lifetime and never enumerate
        // MatLib.) Spots are excluded: FLD_CONV gives them a no-op Filler and
        // PREPROC skips them, because "a headlight is a beam, not a glow quad".
        if (!L.isSpot && !O->F.Txtr) {
            Omni *O2 = sc.OmniHead;
            for (; O2 != O; O2 = O2->Next)
                if (O2->L.R == O->L.R && O2->L.G == O->L.G && O2->L.B == O->L.B) break;
            if (O2 == O) {
                O->F.Txtr = Generate_RGBFlare((unsigned char)O->L.R,
                                              (unsigned char)O->L.G,
                                              (unsigned char)O->L.B);
                if (O->F.Txtr) O->F.Txtr->RelScene = &sc;
            } else {
                O->F.Txtr = O2->F.Txtr;
            }
        }
        if (!L.isSpot && O->F.Txtr && O->F.Txtr->Txtr) {
            L.flareTexIndex = acquireTexture(O->F.Txtr->Txtr);
            L.flareSize = O->ISize * (O->FlareScale > 0.0f ? O->FlareScale : 1.0f);
        }
        if (L.isSpot) fillSpotShadowCam(L);
        out.lights.push_back(L);
    }

    // ---- 6b. GreetsDisco.cpp: 10 rotating cone spots + the glow omni clone ---
    // greets_disco defaults to 1, so these ship in the DEFAULT greets run and
    // reproducing them is PARITY. Every constant below is read out of
    // DEMO/GreetsDisco.cpp, not chosen here.
    if (opt.disco) {
        constexpr int   kSpotCount      = 10;
        constexpr float kRadius         = 0.6f;
        constexpr float kSpinRadPerTick = 0.008f;
        constexpr float kBobAmp         = 0.12f;
        constexpr float kBobRadPerTick  = 0.012f;
        constexpr float kPI             = 3.14159265f;
        static const float kTilts[4]    = {-1.05f, -0.65f, -0.40f, -0.18f};

        // Placement, in GreetsDisco's own priority order:
        //   1. an authored `DiscoBall` null's FIRST Pos keyframe
        //   2. else derived above the central `screen2` panel
        // (its option 3 pin and the FDS_DISCO_POS override are not reproduced;
        // the FLD carries a DiscoBall object, so path 1 is what fires.)
        float ballPos[3] = {0, 0, 0};
        const char *how = "none";
        for (Object *Obj = sc.ObjectHead; Obj; Obj = Obj->Next) {
            if (!Obj->Name || std::strcmp(Obj->Name, "DiscoBall") != 0) continue;
            if (Obj->Type == Obj_TriMesh && Obj->Data) {
                TriMesh *T = (TriMesh *)Obj->Data;
                if (T->Pos.NumKeys > 0 && T->Pos.Keys) {
                    ballPos[0] = T->Pos.Keys[0].Pos.x;
                    ballPos[1] = T->Pos.Keys[0].Pos.y;
                    ballPos[2] = T->Pos.Keys[0].Pos.z;
                    how = "authored DiscoBall null";
                }
            }
            break;
        }
        if (!std::strcmp(how, "none")) {
            double sx = 0, sy = 0, sz = 0; float topY = -1e30f; long n = 0;
            for (Object *Obj = sc.ObjectHead; Obj; Obj = Obj->Next) {
                if (Obj->Type != Obj_TriMesh || !Obj->Data) continue;
                TriMesh *T = (TriMesh *)Obj->Data;
                if (!T->Faces) continue;
                for (DWord fi = 0; fi < T->FIndex; ++fi) {
                    const Face &F = T->Faces[fi];
                    if (!F.Txtr || !F.Txtr->Name ||
                        std::strcmp(F.Txtr->Name, "screen2") != 0) continue;
                    const ::Vertex *vtx[3] = {F.A, F.B, F.C};
                    for (int k = 0; k < 3; ++k) {
                        if (!vtx[k]) continue;
                        Vector lp = vtx[k]->Pos, wp;
                        MatrixXVector(T->RotMat, &lp, &wp);
                        wp.x += T->IPos.x; wp.y += T->IPos.y; wp.z += T->IPos.z;
                        sx += wp.x; sy += wp.y; sz += wp.z;
                        if (wp.y > topY) topY = wp.y;
                        ++n;
                    }
                }
            }
            if (n) {
                ballPos[0] = float(sx / double(n));
                ballPos[1] = topY;
                ballPos[2] = float(sz / double(n));
                how = "derived above screen2";
            }
        }

        // UpdateDiscoBall(sc, t) is called with g_FrameTime — the pause-aware
        // scene clock in centiseconds, i.e. the SAME number our --t is.
        const float t = float(opt.demoT);
        const float a = t * kSpinRadPerTick;
        const float cs = std::cos(a), sn = std::sin(a);
        float bp[3] = {ballPos[0], ballPos[1] + kBobAmp * std::sinf(t * kBobRadPerTick),
                       ballPos[2]};

        for (int i = 0; i < kSpotCount; ++i) {
            const float az = 2.0f * kPI * float(i) / float(kSpotCount);
            const float tilt = kTilts[i & 3];
            const float b[3] = {std::cos(tilt) * std::cos(az),
                                std::sin(tilt),
                                std::cos(tilt) * std::sin(az)};
            // Y-axis spin, exactly UpdateDiscoBall's expression.
            const float d[3] = {cs * b[0] + sn * b[2], b[1], -sn * b[0] + cs * b[2]};
            const float off = kRadius + 0.08f;   // origin OUTSIDE the ball surface
            Light L;
            L.pos[0] = bp[0] + d[0] * off;
            L.pos[1] = bp[1] + d[1] * off;
            L.pos[2] = bp[2] + d[2] * off;
            L.color[0] = 215.0f; L.color[1] = 235.0f; L.color[2] = 255.0f;
            L.intensity = 6.0f;
            L.range = 38.0f;
            L.isSpot = true;
            L.dir[0] = d[0]; L.dir[1] = d[1]; L.dir[2] = d[2];
            L.cosInner = std::cos(2.6f * 3.14159f / 180.0f);
            L.cosOuter = std::cos(7.0f * 3.14159f / 180.0f);
            L.shadowRes = 256;
            L.castsShadow = true;
            L.parented = true;              // rotates -> re-baked every frame
            L.origin = "disco-spot";
            fillSpotShadowCam(L);
            out.lights.push_back(L);
        }
        // The glow: a cloned omni at the ball centre. Its flare Filler is a NO-OP
        // in GreetsDisco ("the visible flare burst read as noise on the ball"), so
        // it gets no flareTexIndex here either.
        {
            Light L;
            L.pos[0] = bp[0]; L.pos[1] = bp[1]; L.pos[2] = bp[2];
            L.color[0] = 210.0f; L.color[1] = 225.0f; L.color[2] = 255.0f;
            L.intensity = 0.55f + 0.12f * std::sinf(t * 0.05f);
            L.range = 6.0f;
            L.castsShadow = false;          // Omni_CastsShadow cleared on the clone
            L.origin = "disco-glow";
            out.lights.push_back(L);
        }
        if (opt.verbose)
            std::fprintf(stderr,
                "[INGEST] disco: ball (%.2f,%.2f,%.2f) via %s; %d cone spots "
                "(2.6/7.0 deg, range 38, 256^2 maps, spin %.3f rad) + 1 glow omni\n",
                bp[0], bp[1], bp[2], how, kSpotCount, a);
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
