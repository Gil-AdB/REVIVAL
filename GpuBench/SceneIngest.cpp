#include "SceneIngest.h"

#include <Base/FDS_DEFS.H>
#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Base/FeatureFlags.h>
#include <Base/Scene.h>
#include <Base/TriMesh.h>
#include <FLD/FLD_READ.H>
#include <FLD/LWREAD.H>   // Surf_Smoothing (Material::TFlags bit)
#include <RENDER/GreetsMirror.h>   // FindMirrorPlaneByMatName (engine's own)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

// A DOCUMENTED GAP, not a workaround. FDS_VARS.H DECLARES `dTime` (the free
// camera's per-frame integration step) but nothing in the FDS static library
// DEFINES it — the definition is DEMO/REV.CPP:567. Dynamic_Camera() therefore
// does not link from an FDS-only target without its owner supplying the
// storage, which is what this line does. It is a GpuBench-side definition; no
// engine file is touched. Anything else that reads `dTime` in this process is
// reading the value FreeCamStep last wrote, which is correct — GpuBench has
// exactly one camera integrator.
float dTime = 0.0f;

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
float DemoTimeToCurFrame(const ::Scene &sc, float demoT) {
    const float span = sc.EndFrame - sc.StartFrame;
    const float chPartTime = 500.0f + 100.0f * span / 30.0f;
    const float denom = chPartTime - 500.0f;
    if (denom <= 0.0f) return sc.StartFrame;
    return sc.StartFrame + span * demoT / denom;
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

// ---------------------------------------------------------------------------
// DEMO-side geometry fixups the shipped greets applies BEFORE its meshes ever
// render, replicated here because they live in DEMO/ (unreachable from a
// target that links FDS only) yet change what the user actually reviews.
// ---------------------------------------------------------------------------

// GreetsRetileFloor (DEMO/GREETS.CPP:1276-1307), verbatim math. Gated on
// stone-tex exactly as DEMO gates it on greets_stone_tex; the scale/warp
// constants come from the same FeatureFlags (defaults 1.5 / 0.06 — ACTIVE in
// the default run, so without this the floor tiles 1.5x too densely and
// carries no de-tile warp).
float DetileU(float wx, float wz, float amp) {
    const float f = 0.017f;
    return amp * (std::sin(wx*f + wz*f*0.7f) + 0.5f*std::sin(wx*f*1.9f - wz*f*1.3f));
}
float DetileV(float wx, float wz, float amp) {
    const float f = 0.012f;
    return amp * (std::cos(wz*f + wx*f*0.6f) + 0.5f*std::cos(wz*f*1.7f - wx*f*1.1f));
}
void ReplicateRetileFloor(::Scene &sc, bool verbose) {
    const float scale = fds::FeatureFlags::greets_floor_uv_scale();
    const float amp   = fds::FeatureFlags::greets_floor_detile();
    if (scale == 1.0f && amp == 0.0f) return;
    const float invScale = scale > 0.0f ? 1.0f / scale : 1.0f;
    int touched = 0;
    for (TriMesh *T = sc.TriMeshHead; T; T = T->Next) {
        if (!T->Faces) continue;
        for (DWord fi = 0; fi < T->FIndex; ++fi) {
            Face &F = T->Faces[fi];
            if (!F.Txtr || !F.Txtr->Name || std::strcmp(F.Txtr->Name, "floor")) continue;
            if (!F.A || !F.B || !F.C) continue;
            ::Vertex *vs[3] = {F.A, F.B, F.C};
            float *uu[3] = {&F.U1, &F.U2, &F.U3};
            float *vv[3] = {&F.V1, &F.V2, &F.V3};
            for (int k = 0; k < 3; ++k) {
                Vector lp = vs[k]->Pos, wp;
                MatrixXVector(T->RotMat, &lp, &wp);
                wp.x += T->IPos.x; wp.y += T->IPos.y; wp.z += T->IPos.z;
                *uu[k] = (*uu[k] - 0.5f) * invScale + 0.5f + DetileU(wp.x, wp.z, amp);
                *vv[k] = (*vv[k] - 0.5f) * invScale + 0.5f + DetileV(wp.x, wp.z, amp);
            }
            ++touched;
        }
    }
    if (verbose)
        std::fprintf(stderr,
            "[INGEST] floor retile: scale=%.2f detile=%.3f (%d faces) -- "
            "parity with GreetsRetileFloor\n", scale, amp, touched);
}

// Per-surface authored smoothing-angle override, the registry
// MeshOps_SeedAuthoredSmoothAngles fills (DEMO/GREETS.CPP:1723-1728): a
// surface flagged Surf_Smoothing whose authored SMAN differs from greets'
// 89.5-degree global default by more than 0.05 degrees smooths at ITS angle,
// restricted to its own faces. Returns true + the angle when the override
// applies.
bool SurfaceSmoothOverride(const Material *M, float &angleDegOut) {
    if (!M || !M->Name) return false;
    if (!(M->TFlags & Surf_Smoothing)) return false;
    const float deg = M->MaxSmoothingAngle * (180.0f / float(M_PI));
    if (std::fabs(deg - 89.5f) <= 0.05f) return false;
    angleDegOut = deg;
    return true;
}

bool MatIsMomy(const Material *M) {
    return M && M->Name &&
           (!std::strcmp(M->Name, "momy-1") || !std::strcmp(M->Name, "momy-2"));
}

// Does DEMO's MakeFacesIndependentByAngle process this mesh at all? It skips
// meshes with no crease at the 30-degree global threshold and no override
// surface (DEMO/MeshOps.cpp:1308-1328) — skipped meshes keep the shared
// smooth Vertex::N / Vertex::Tangent from Scene_Computations.
bool MeshGetsIndependentFaces(TriMesh *T, float cosThr,
                              const std::unordered_map<const ::Vertex *,
                                                       std::vector<const Face *>> &incident) {
    for (const auto &kv : incident) {
        const auto &fs = kv.second;
        for (size_t i = 0; i < fs.size(); ++i)
            for (size_t j = i + 1; j < fs.size(); ++j) {
                const float d = fs[i]->N.x * fs[j]->N.x + fs[i]->N.y * fs[j]->N.y
                              + fs[i]->N.z * fs[j]->N.z;
                if (d < cosThr) return true;
            }
    }
    for (DWord fi = 0; fi < T->FIndex; ++fi) {
        float a;
        if (SurfaceSmoothOverride(T->Faces[fi].Txtr, a)) return true;
    }
    return false;
}

float TriArea(const Vector &a, const Vector &b, const Vector &c) {
    const float e1x = b.x-a.x, e1y = b.y-a.y, e1z = b.z-a.z;
    const float e2x = c.x-a.x, e2y = c.y-a.y, e2z = c.z-a.z;
    const float cx = e1y*e2z - e1z*e2y;
    const float cy = e1z*e2x - e1x*e2z;
    const float cz = e1x*e2y - e1y*e2x;
    return 0.5f * std::sqrt(cx*cx + cy*cy + cz*cz);
}

// The corner-normal rule of DEMO/MeshOps.cpp:171-224 (computeSmoothedNormal),
// term for term: (1) per-surface override — same base surface, angle-gated
// against THIS face's normal; (2) momy — every incident momy face, NO angle
// gate (the true shared normal that keeps the lathe seamless); (3) the global
// 30-degree architectural crease gate. Area-weighted, face-normal fallback.
Vector CornerNormal(const ::Vertex *origVtx, const Face *F, float cosSmoothing,
                    const std::unordered_map<const ::Vertex *,
                                             std::vector<const Face *>> &incident) {
    float ovAngle = 0.0f;
    const bool perSurf = SurfaceSmoothOverride(F->Txtr, ovAngle);
    const float cosPerSurf = perSurf
        ? std::cos(ovAngle * float(M_PI) / 180.0f) : 0.0f;
    const bool momy = !perSurf && MatIsMomy(F->Txtr);
    auto it = incident.find(origVtx);
    if (it == incident.end()) return F->N;
    Vector acc{0, 0, 0};
    for (const Face *adj : it->second) {
        if (perSurf) {
            if (!adj->Txtr || !adj->Txtr->Name || !F->Txtr->Name ||
                std::strcmp(adj->Txtr->Name, F->Txtr->Name)) continue;
            const float d = F->N.x*adj->N.x + F->N.y*adj->N.y + F->N.z*adj->N.z;
            if (d < cosPerSurf) continue;
        } else if (momy) {
            if (!MatIsMomy(adj->Txtr)) continue;
        } else {
            const float d = F->N.x*adj->N.x + F->N.y*adj->N.y + F->N.z*adj->N.z;
            if (d < cosSmoothing) continue;
        }
        const float w = TriArea(adj->A->Pos, adj->B->Pos, adj->C->Pos);
        acc.x += adj->N.x * w; acc.y += adj->N.y * w; acc.z += adj->N.z * w;
    }
    const float len = std::sqrt(acc.x*acc.x + acc.y*acc.y + acc.z*acc.z);
    if (len < 1e-6f) return F->N;
    acc.x /= len; acc.y /= len; acc.z /= len;
    return acc;
}

// The face's own Lengyel tangent from the per-FACE UVs, with
// Compute_Vertex_Tangents' degenerate-UV fallback to the per-vertex UVs
// (FDS/MISC/PREPROC.CPP:396-460). Returns false when both are degenerate.
bool FaceTangent(const Face *F, Vector &out) {
    const Vector &p0 = F->A->Pos, &p1 = F->B->Pos, &p2 = F->C->Pos;
    const float e1x = p1.x-p0.x, e1y = p1.y-p0.y, e1z = p1.z-p0.z;
    const float e2x = p2.x-p0.x, e2y = p2.y-p0.y, e2z = p2.z-p0.z;
    float du1 = F->U2 - F->U1, dv1 = F->V2 - F->V1;
    float du2 = F->U3 - F->U1, dv2 = F->V3 - F->V1;
    if (std::fabs(du1*dv2 - du2*dv1) < 1e-8f) {
        const float vdu1 = F->B->U - F->A->U, vdv1 = F->B->V - F->A->V;
        const float vdu2 = F->C->U - F->A->U, vdv2 = F->C->V - F->A->V;
        if (std::fabs(vdu1*vdv2 - vdu2*vdv1) >= 1e-8f) {
            du1 = vdu1; dv1 = vdv1; du2 = vdu2; dv2 = vdv2;
        }
    }
    const float denom = du1*dv2 - du2*dv1;
    if (std::fabs(denom) < 1e-8f) return false;
    const float r = 1.0f / denom;
    out.x = (e1x*dv2 - e2x*dv1) * r;
    out.y = (e1y*dv2 - e2y*dv1) * r;
    out.z = (e1z*dv2 - e2z*dv1) * r;
    return true;
}

// Gram-Schmidt `t` against unit `n`, with Compute_Vertex_Tangents' fallback
// (N x reference-axis) when degenerate.
Vector OrthonormalTangent(Vector t, const Vector &n, bool haveT) {
    if (haveT) {
        const float d = t.x*n.x + t.y*n.y + t.z*n.z;
        t.x -= d*n.x; t.y -= d*n.y; t.z -= d*n.z;
        const float len = std::sqrt(t.x*t.x + t.y*t.y + t.z*t.z);
        if (len > 1e-6f) { t.x /= len; t.y /= len; t.z /= len; return t; }
    }
    const Vector ref = (std::fabs(n.y) < 0.9f) ? Vector{0, 1, 0} : Vector{1, 0, 0};
    Vector f{n.y*ref.z - n.z*ref.y, n.z*ref.x - n.x*ref.z, n.x*ref.y - n.y*ref.x};
    const float len = std::sqrt(f.x*f.x + f.y*f.y + f.z*f.z);
    if (len > 1e-6f) { f.x /= len; f.y /= len; f.z /= len; }
    return f;
}

// Per-face UV-winding handedness — the predicate of DEMO's
// GreetsFixBitangentHandedness (GREETS.CPP:1318-1351): negative per-face UV
// determinant => the bitangent must flip (B = -(N x T)). The CPU realises
// this as a ::mirUV material clone with TbnHandedness=-1; here it is a
// per-vertex sign.
float FaceHandedness(const Face &F) {
    const float du1 = F.U2 - F.U1, dv1 = F.V2 - F.V1;
    const float du2 = F.U3 - F.U1, dv2 = F.V3 - F.V1;
    return (du1*dv2 - du2*dv1 >= 0.0f) ? 1.0f : -1.0f;
}

// Replicates MaterialImport_ApplyRevMaps (DEMO/MaterialImport.cpp:639-679) —
// the LWO/FLD-authored PBR map SETS. LoadFLD does not apply these: FLD_MAT.CPP
// only RECORDS one RevMapAssignment per Surf_RevMaps material into a
// process-global registry, and DEMO reads it back at scene init. The registry
// accessors (FldRevMapCount / FldRevMapAt, FLD/FLD_READ.H:227-229) are
// FDS-side, so this arm can replay the same assignment without an engine edit.
//
// MEASURED consequence of NOT doing it: the reference applies 32 maps
// ("[MAT-REVMAP] greets: 32 LWO/FLD-authored map(s) applied", read from a real
// run log) and this arm applied ZERO — so every RVSM surface rendered from its
// legacy FLD JPG with no normal/roughness/metallic. The visible symptom at
// t=2000 was the 'amudim' columns reading BRIGHT ORANGE where the reference has
// dark metal: their metallic map kills diffuse on the CPU, and with no map at
// all the GPU showed raw albedo.
//
// Role order is DEMO's (albedo first — it is the slot the others are compared
// against; then alphabetical) and LAST WINS, so a surface carrying both the
// stone-tex override and an RVSM set ends up with the RVSM maps, exactly as in
// DEMO where ApplyRevMaps runs after the stone-tex repoint.
//
// Deliberately NOT replicated: the resample of aux maps to the albedo's
// dimensions. That exists because the CPU kernel addresses every aux map with
// the ALBEDO's swizzled texel index, so a differently-sized map would read
// scrambled. This arm samples each map by UV, so mismatched sizes are correct
// as-is — and skipping the resample keeps the authored texels. Same class of
// meaning-preserving substitution as never calling Generate_Mipmaps.
int ApplyRevMaps(::Scene &sc, bool verbose) {
    static const char *const kRoles[] = {"albedo", "ao", "height",
                                         "metallic", "normal", "roughness"};
    auto exists = [](const std::string &p) {
        if (FILE *f = std::fopen(p.c_str(), "rb")) { std::fclose(f); return true; }
        return false;
    };
    int applied = 0;
    const int n = FldRevMapCount();
    for (int i = 0; i < n; ++i) {
        const RevMapAssignment *e = FldRevMapAt(i);
        if (!e || e->scene != &sc || !e->matName || !e->set || !*e->set) continue;
        const std::string dir = std::string("TEXTURES/PBR/") + e->set;
        // Every material drawing this surface (exact name; this arm has no
        // ::mirUV clones — the handedness split rides the vertex instead).
        std::vector<Material *> mats;
        for (Material *M = MatLib; M; M = M->Next)
            if (M->RelScene == &sc && M->Name && !std::strcmp(M->Name, e->matName))
                mats.push_back(M);
        if (mats.empty()) continue;
        for (const char *role : kRoles) {
            const std::string path = dir + "/" + role + ".png";
            if (!exists(path)) continue;
            ::Texture *t = MakeSidecar(path.c_str());
            for (Material *M : mats) {
                if      (!std::strcmp(role, "albedo"))    { M->Txtr = t; M->Flags &= ~(DWord)Mat_AoInAlpha; }
                else if (!std::strcmp(role, "normal"))    M->NormalMap    = t;
                else if (!std::strcmp(role, "height"))    M->HeightMap    = t;
                else if (!std::strcmp(role, "roughness")) {
                    M->RoughnessMap = t;
                    // Dielectric specular seed, MaterialImport.cpp:266-281:
                    // only when the author left Specular at 0. Glossiness is
                    // seeded from the map's MEAN roughness there; the mean
                    // needs the decoded pixels, which are not loaded yet at
                    // this point, so the gloss half is left to the authored
                    // value and the divergence is stated rather than faked.
                    if (M->Specular <= 0.0f) M->Specular = 0.08f;
                }
                else if (!std::strcmp(role, "ao"))        M->AoMap        = t;
                else if (!std::strcmp(role, "metallic"))  M->MetallicMap  = t;
            }
            ++applied;
            if (verbose)
                std::fprintf(stderr, "[INGEST] revmap '%s' <- %s %s (%zu material(s))\n",
                             e->matName, role, path.c_str(), mats.size());
        }
    }
    if (verbose)
        std::fprintf(stderr, "[INGEST] revmap: %d LWO/FLD-authored map(s) applied "
                     "from %d registry entr(ies) -- parity with "
                     "MaterialImport_ApplyRevMaps\n", applied, n);
    return applied;
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

// The loaded FDS scene, kept alive at file scope so Reanimate() can re-run
// Animate_Objects on it every frame in the interactive window. TriMesh / Face /
// Material pointers inside the flattened output alias into it.
static ::Scene g_scene;
static bool    g_loaded = false;
static std::unordered_map<const ::Texture *, int> texIndex;

// Refresh every per-frame quantity from the FDS scene at the CURRENT CurFrame:
// per-batch model matrices, the scripted camera, and the whole light list
// (including the disco, which rotates). Everything else -- vertices, textures,
// material constants -- is frame-invariant, so it is built once by Load().
//
// VERTICES ARE NOT RE-EXTRACTED, and that is a property of the engine, not a
// shortcut: Animate_Objects fills per-mesh IPos / IScale / RotMat from the
// splines and the parent hierarchy. It does NOT deform vertices -- the mech
// animates as a hierarchy of rigid TriMeshes (Hull, L_leg1, L_leg2, ...), each
// with its own transform. VERIFIED by hashing the de-indexed vertex buffer at
// two different frames (see --verify_static_verts). So the "re-upload only
// meshes whose verts changed" requirement resolves to "none of them do", and the
// per-frame GPU upload is 35 batch uniform blocks, not geometry.
static void RefreshLights(Scene &out, const LoadOptions &opt, ::Scene &sc);
static void RefreshCamera(Scene &out, const LoadOptions &opt, ::Scene &sc);
static void RefreshBatchTransforms(Scene &out, ::Scene &sc);


// Rebuild every per-frame light quantity. Called by Load() and by Reanimate().
static void RefreshLights(Scene &out, const LoadOptions &opt, ::Scene &sc) {
    out.lights.clear();
    auto acquireTexture = [&](::Texture *tx) -> int {
        if (!tx) return -1;
        auto it = texIndex.find(tx);
        if (it != texIndex.end()) return it->second;
        if (!tx->Data && !Load_Texture(tx)) { texIndex[tx] = -1; return -1; }
        TextureImage img;
        img.fileName = tx->FileName ? tx->FileName : "(flare)";
        int idx = -1;
        if (ExpandToRGBA(tx, img)) {
            idx = int(out.textures.size());
            out.textures.push_back(std::move(img));
            ++out.texturesLoaded;
        }
        texIndex[tx] = idx;
        return idx;
    };
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

}

// World-space bounding sphere over a batch's own vertices, under its current
// model matrix. Centroid + max radius (not the minimal sphere — the CPU's
// Compute_BSphere is the same cheap construction, and a slightly loose sphere
// only ever culls LESS, never wrongly).
void ComputeBatchSphere(Scene &out, Batch &b) {
    if (!b.vertexCount) return;
    double cx = 0, cy = 0, cz = 0;
    auto toWorld = [&](const Vertex &V, float w[3]) {
        const float o[3] = {V.px, V.py, V.pz};
        for (int c = 0; c < 3; ++c)
            w[c] = b.rot[c][0]*o[0] + b.rot[c][1]*o[1] + b.rot[c][2]*o[2] + b.pos[c];
    };
    for (uint32_t v = b.firstVertex; v < b.firstVertex + b.vertexCount; ++v) {
        float w[3]; toWorld(out.verts[v], w);
        cx += w[0]; cy += w[1]; cz += w[2];
    }
    const double n = double(b.vertexCount);
    b.bsCtr[0] = float(cx / n); b.bsCtr[1] = float(cy / n); b.bsCtr[2] = float(cz / n);
    float r2 = 0.0f;
    for (uint32_t v = b.firstVertex; v < b.firstVertex + b.vertexCount; ++v) {
        float w[3]; toWorld(out.verts[v], w);
        const float dx = w[0]-b.bsCtr[0], dy = w[1]-b.bsCtr[1], dz = w[2]-b.bsCtr[2];
        r2 = std::max(r2, dx*dx + dy*dy + dz*dz);
    }
    b.bsRad = std::sqrt(r2);
}

// Per-frame refresh of the model matrices. Animate_Objects has already run.
void ComputeBatchSphere(Scene &out, Batch &b);

static void RefreshBatchTransforms(Scene &out, ::Scene &sc) {
    std::unordered_map<std::string, TriMesh *> byName;
    for (Object *obj = sc.ObjectHead; obj; obj = obj->Next)
        if (obj->Type == Obj_TriMesh && obj->Data && obj->Name)
            byName[obj->Name] = (TriMesh *)obj->Data;
    for (auto &b : out.batches) {
        auto it = byName.find(b.meshName);
        if (it == byName.end()) continue;
        TriMesh *T = it->second;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) b.rot[r][c] = T->RotMat[r][c];
        b.pos[0] = T->IPos.x; b.pos[1] = T->IPos.y; b.pos[2] = T->IPos.z;
        ComputeBatchSphere(out, b);   // the sphere rides the model matrix
    }
}


// Rebuild the camera from the FDS scene at the current CurFrame. With an empty
// camPose this follows the AUTHORED spline (Animate_Objects has already moved
// sc.CameraHead); with a pose string it reproduces DEMO's FDS_GREETS_CAM path.
static void RefreshCamera(Scene &out, const LoadOptions &opt, ::Scene &sc) {
    // ---- 3. camera ---------------------------------------------------------
    // Built with FDS's OWN Kick_Camera + CalcPersp, so the view and projection
    // are the engine's, not a re-derivation. Same code path DEMO's
    // FDS_GREETS_CAM debug camera uses (GREETS.CPP ~2963).
    if (sc.CameraHead) {
        static ::Camera fc;
        fc = *sc.CameraHead;
        // FOV convention, matching DEMO exactly (GREETS.CPP:3063-3066 +
        // Snapshot.cpp:508-567): the FDS_GREETS_CAM debug camera copies the
        // scripted camera AT INIT, when IFOV is still 0, so it resolves to the
        // FOV spline's FIRST KEY (75-degree fallback) and Animate_Objects then
        // skips it (View == &FC). The AUTHORED spline camera keeps the animated
        // IFOV. So: posed arm -> Keys[0]; spline arm -> IFOV at CurFrame.
        const bool posed = !opt.camPose.empty();
        float fov = posed ? 0.0f : fc.IFOV;
        if (fov < 1.0f && sc.CameraHead->FOV.NumKeys > 0)
            fov = sc.CameraHead->FOV.Keys[0].Pos.x;
        if (fov < 1.0f) fov = 75.0f;

        float px, py, pz, fx, fy, fz;
        if (posed &&
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

}

bool Load(Scene &out, const LoadOptions &opt) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    out.xres = opt.xres;
    out.yres = opt.yres;
    PublishResolution(opt.xres, opt.yres);

    // ---- 1. geometry + materials + lights + camera splines -----------------
    ::Scene &sc = g_scene;
    std::memset(&sc, 0, sizeof(sc));
    if (!LoadFLD(&sc, opt.fldPath)) {
        std::fprintf(stderr, "[INGEST] LoadFLD failed: %s\n", opt.fldPath);
        return false;
    }

    // ---- 1b. omni range FORCE-OVERRIDE (default off) ------------------------
    // History matters here. Pre-00f7820, Initialize_Greets rewrote every omni
    // whose IRange was 0 (= all ten, since Animate_Objects hadn't run) to a
    // flat 30, and this ingest replicated that patch as parity. Commit 00f7820
    // then authored LightRange 30 into the LWS itself, regenerated the FLD, and
    // DELETED both runtime patches — so parity is now the AUTHORED envelope,
    // evaluated by Animate_Objects like any other spline, with no patch at all.
    // What survives in DEMO is greets_omni_default_range as a default-0 tuning
    // dial that force-rewrites every Range spline key (GREETS.CPP:2692-2711,
    // "rewriting the spline keys, since writing IRange alone is undone by the
    // next Animate_Objects"). --omni_range=F reproduces that dial.
    if (opt.omniDefaultRange > 0.0f) {
        int forced = 0;
        for (::Omni *O = sc.OmniHead; O; O = O->Next) {
            if (O->Type != Light_Omni) continue;
            O->IRange = opt.omniDefaultRange;
            O->rRange = 1.0f / opt.omniDefaultRange;
            for (int k = 0; k < O->Range.NumKeys && O->Range.Keys; ++k)
                O->Range.Keys[k].Pos.x = opt.omniDefaultRange;
            ++forced;
        }
        if (opt.verbose)
            std::fprintf(stderr,
                "[INGEST] --omni_range: forced IRange=%g on %d omnis "
                "(rewriting spline keys, as GREETS.CPP:2692 does)\n",
                opt.omniDefaultRange, forced);
    }

    // ---- 1b1. SceneCorrections' omni SIZE multipliers -----------------------
    // Commit 00f7820 deleted both RANGE patches, but SceneCorrections
    // (DEMO/GREETS.CPP:186-241, called unconditionally at :1531) still scales
    // the omni SIZE splines: OmniSizeMult = {1,1,1,1,1,1,1, 1.5,1.5,1.5} — the
    // three mech omnis run at ISize 0.75, not the authored 0.5. MEASURED as
    // the last big chunk of the unshadowed-direct residual: with everything
    // else matched, the per-pixel linear excess of the CPU's direct term was
    // isolated to the mech omnis at a factor ~1.5-1.6 while the yellow FLD
    // omnis agreed within 8% — exactly this table. Same engine call
    // (Spline_Scale), same order (before Animate_Objects evaluates ISize).
    {
        static const float kOmniSizeMult[10] =
            {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.5f, 1.5f, 1.5f};
        int idx = 0;
        for (::Omni *O = sc.OmniHead; O && idx < 10; O = O->Next, ++idx)
            Spline_Scale(&O->Size, kOmniSizeMult[idx]);
        // (OmniDisable / OmniPointFlare are all zero; Omni_Stationary only
        // affects the CPU's shadow-cache classification, which this arm
        // derives from the FLD parent chain to the same static/moving split.)
    }

    // ---- 1b2. floor UV retile (DEMO-side geometry fixup, default-active) ----
    // DEMO order is Preprocess -> GreetsRetileFloor -> MakeFacesIndependent
    // (which recomputes normals+tangents from the retiled per-face UVs). Here
    // the retile runs BEFORE Scene_Computations so every downstream tangent
    // sees the final UVs; positions are untouched so normals cannot differ.
    if (opt.stoneTex) ReplicateRetileFloor(sc, opt.verbose);

    // ---- 1c. RUN THE ENGINE'S OWN MESH PREPROCESSING ------------------------
    // LoadFLD does NOT compute normals. It fills Vertex::Pos and the Face
    // topology and stops; Face::N, Vertex::N, Vertex::Tangent, Face::NormProd
    // and the bounding spheres are all produced later, by Scene_Computations
    // (FDS/MISC/PREPROC.CPP:632), which DEMO reaches through Preprocess_Scene.
    //
    // WITHOUT THIS CALL EVERY Vertex::N IS ZERO, so the G-buffer stored the
    // oct-decode of (0,0) -- a constant (0,0,1) view normal -- on every surface
    // in the scene. MEASURED before the fix: --viz=normal returned rgb
    // (128,128,255) at BOTH a side wall and the floor, two surfaces at right
    // angles. The consequence was not "flat shading": N·L degenerated into the
    // sign of the light's view-space Z, so a light BEHIND the surface down the
    // corridor lit it and a light three units in FRONT of it did not. The three
    // mech omnis -- which dominate the CPU's direct term at t=5743 -- reached
    // 15,381 of 2,073,600 pixels (0.74 %).
    //
    // Calling the engine's own function rather than re-deriving normals here is
    // the same principle the rest of this ingest follows: the input is the same
    // bytes through the same code. It also fills Vertex::Tangent via
    // Compute_Vertex_Tangents, so the derivative-based TBN in fs_gbuffer could
    // now be replaced by the engine's basis (not done here; separate change).
    Scene_Computations(&sc);

    // ---- 2. pose ------------------------------------------------------------
    out.curFrame = DemoTimeToCurFrame(sc, opt.demoT);
    CurFrame = out.curFrame;
    Animate_Objects(&sc, sc.CameraHead);

    RefreshCamera(out, opt, sc);

    // ---- 4. textures -------------------------------------------------------
    // Decode ONCE per distinct ::Texture. Load_Texture only; Generate_Mipmaps is
    // deliberately never called (see header). File-static so the per-frame
    // Reanimate path resolves an already-decoded flare texture to its existing
    // index instead of decoding it again.
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

    // ---- 4b2. RVSM authored PBR map sets (AFTER stone-tex, as DEMO does) ----
    if (opt.revMaps) ApplyRevMaps(sc, opt.verbose);

    // ---- 4c. mirror panel planes --------------------------------------------
    // The greets first-order mirror set, by the SAME explicit designation
    // Initialize_Greets uses (GREETS.CPP:2880-2897): 'teleporter' by material
    // name plus the two P_TEXT screens authored as mirrors ('screen 3',
    // 'screen 4'; 'screen2' deliberately not). Planes come from the ENGINE's
    // own FindMirrorPlaneByMatName (outlier faces >30 degrees off the majority
    // normal are dropped from the fit). Panel faces — the thin screen box's
    // FRONT face — are selected below in the batching loop: world normal
    // within 30 degrees of the plane normal AND centroid on the plane; the
    // box's back face fails the angle gate (opposite normal), the caps both.
    if (opt.mirrors) {
        static const char *kMirrorMats[] = {"teleporter", "screen 3", "screen 4"};
        for (const char *mname : kMirrorMats) {
            fds::MirrorPlane mp = fds::FindMirrorPlaneByMatName(&sc, mname);
            if (!mp.valid) {
                if (opt.verbose)
                    std::fprintf(stderr, "[INGEST] mirror '%s': no plane (skipped)\n", mname);
                continue;
            }
            MirrorInfo mi;
            mi.n[0] = mp.N.x; mi.n[1] = mp.N.y; mi.n[2] = mp.N.z;
            mi.d = mp.d;
            mi.material = mname;
            out.mirrors.push_back(mi);
            if (opt.verbose)
                std::fprintf(stderr,
                    "[INGEST] mirror %zu '%s': plane N=(%.3f,%.3f,%.3f) d=%.3f "
                    "(%d wall faces in fit, centroid %.2f,%.2f,%.2f)\n",
                    out.mirrors.size(), mname, mp.N.x, mp.N.y, mp.N.z, mp.d,
                    mp.faceCount, mp.centroid.x, mp.centroid.y, mp.centroid.z);
        }
    }
    // Face -> mirror panel membership test, used to key the batch split.
    auto faceMirrorIndex = [&](const TriMesh *T, const Face &F) -> int {
        if (out.mirrors.empty() || !F.Txtr || !F.Txtr->Name || !F.A) return 0;
        for (size_t mi = 0; mi < out.mirrors.size(); ++mi) {
            const MirrorInfo &m = out.mirrors[mi];
            if (std::strcmp(F.Txtr->Name, m.material.c_str())) continue;
            // world face normal (mesh transform is rigid at load)
            Vector n;
            MatrixXVector(const_cast<TriMesh *>(T)->RotMat,
                          const_cast<Vector *>(&F.N), &n);
            const float ndot = n.x*m.n[0] + n.y*m.n[1] + n.z*m.n[2];
            if (ndot < 0.866f) return 0;          // back face / caps
            Vector lp = F.A->Pos, wp;
            MatrixXVector(const_cast<TriMesh *>(T)->RotMat, &lp, &wp);
            wp.x += T->IPos.x; wp.y += T->IPos.y; wp.z += T->IPos.z;
            const float sd = wp.x*m.n[0] + wp.y*m.n[1] + wp.z*m.n[2] + m.d;
            if (std::fabs(sd) > 0.6f) return 0;   // not on the panel plane
            return int(mi) + 1;
        }
        return 0;
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

        // Crease-preserving per-corner normals + tangents, replicating what
        // DEMO's unconditional MakeFacesIndependentByAngle(GreetSc, 30.0f)
        // (GREETS.CPP:1842) does to every mesh that has a >30-degree crease or
        // an authored per-surface smoothing angle. Meshes it would skip keep
        // Scene_Computations' shared smooth Vertex::N / Vertex::Tangent —
        // both paths are reproduced, decided by the same test. Without this
        // every crease (wall<->wall corners, lamp fixtures, the mech's hull)
        // rendered over-smoothed relative to the reference, shifting NoL on
        // exactly the surfaces the direct term is being compared on.
        std::unordered_map<const ::Vertex *, std::vector<const Face *>> incident;
        incident.reserve(size_t(T->FIndex) * 3);
        for (uint32_t f = 0; f < T->FIndex; ++f) {
            const Face &F = T->Faces[f];
            if (F.A) incident[F.A].push_back(&F);
            if (F.B) incident[F.B].push_back(&F);
            if (F.C) incident[F.C].push_back(&F);
        }
        const float cos30 = std::cos(30.0f * float(M_PI) / 180.0f);
        const bool independent = MeshGetsIndependentFaces(T, cos30, incident);

        // Group this mesh's faces by (material, mirror-panel membership) so
        // each batch is one draw and a mirror panel's front faces get their
        // own batch (they composite the reflection).
        std::map<std::pair<Material *, int>, std::vector<uint32_t>> byMat;
        for (uint32_t f = 0; f < T->FIndex; ++f)
            byMat[{T->Faces[f].Txtr, faceMirrorIndex(T, T->Faces[f])}].push_back(f);

        for (auto &kv : byMat) {
            Material *M = kv.first.first;
            const int mirrorIdx = kv.first.second;
            Batch b;
            b.mirrorIndex = mirrorIdx;
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
                b.aoTexIndex     = acquireTexture(M->AoMap);
                b.metalTexIndex  = acquireTexture(M->MetallicMap);
                b.aoStrength     = M->AoStrength;
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
                const ::Vertex *vp[3] = {F.A, F.B, F.C};
                // Per-FACE UVs. NOT F.A->U / F.B->U / F.C->U — see header.
                const float uu[3] = {F.U1, F.U2, F.U3};
                const float vv[3] = {F.V1, F.V2, F.V3};
                const float hand = FaceHandedness(F);
                Vector faceTan{0, 0, 0};
                const bool haveFaceTan = independent && FaceTangent(&F, faceTan);
                for (int k = 0; k < 3; ++k) {
                    Vertex gv;
                    gv.px = vp[k]->Pos.x;
                    gv.py = vp[k]->Pos.y;
                    gv.pz = vp[k]->Pos.z;
                    Vector n, t;
                    if (independent) {
                        // Per-corner crease-preserving normal, then the FACE's
                        // own tangent orthonormalized against it — exactly the
                        // post-MakeFacesIndependent state, where each cloned
                        // vertex has ONE incident face so Compute_Vertex_
                        // Tangents' accumulation collapses to the face tangent.
                        n = CornerNormal(vp[k], &F, cos30, incident);
                        t = OrthonormalTangent(faceTan, n, haveFaceTan);
                    } else {
                        n = vp[k]->N;
                        t = vp[k]->Tangent;   // engine's shared smooth tangent
                    }
                    gv.nx = n.x; gv.ny = n.y; gv.nz = n.z;
                    gv.tx = t.x; gv.ty = t.y; gv.tz = t.z;
                    gv.th = hand;
                    gv.u = uu[k];
                    gv.v = vv[k];
                    out.verts.push_back(gv);
                }
                ++out.faceCount;
            }
            // Mirror panel: parity with the CPU's wallMatClone retarget — the
            // panel neither diffuses nor speculates; its lit value is the
            // emissive text (Luminosity) plus the composited reflection/2.
            if (mirrorIdx > 0) {
                b.diffuse = 0.0f;
                b.specular = 0.0f;
                // The CPU retargets panel faces to a Mat_Transparent clone,
                // which the shadow bake skips — so the panel casts nothing.
                // (The screens were already non-casters via the name filter;
                // this matters for the teleporter, whose base material casts.)
                b.castsShadow = false;
                out.mirrors[size_t(mirrorIdx) - 1].panelFaces +=
                    int((uint32_t(out.verts.size()) - b.firstVertex) / 3);
            }
            b.vertexCount = uint32_t(out.verts.size()) - b.firstVertex;
            if (b.vertexCount) {
                ComputeBatchSphere(out, b);
                out.batches.push_back(std::move(b));
            }
        }
    }

    RefreshLights(out, opt, sc);

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
        for (const auto &b : out.batches)
            if (b.luminosity != 0.0f)
                std::fprintf(stderr,
                    "[INGEST] emissive material '%s' (%s): Luminosity=%.3f Diffuse=%.2f\n",
                    b.materialName.c_str(), b.meshName.c_str(), b.luminosity, b.diffuse);
        std::fprintf(stderr,
            "[INGEST] ambient: Scene::Ambient=(%.1f,%.1f,%.1f)  SkyZenith=(%.1f,%.1f,%.1f)  "
            "SkyNadir=(%.1f,%.1f,%.1f)\n",
            out.ambient[0], out.ambient[1], out.ambient[2],
            out.skyZenith[0], out.skyZenith[1], out.skyZenith[2],
            out.skyNadir[0], out.skyNadir[1], out.skyNadir[2]);
    }
    g_loaded = !out.verts.empty();
    return g_loaded;
}

void BuildViewMatrix(const float eye[3], const float fwd[3], float outRot[3][3]) {
    Vector src{eye[0], eye[1], eye[2]};
    Vector look{eye[0] + fwd[0], eye[1] + fwd[1], eye[2] + fwd[2]};
    Matrix M;
    Kick_Camera(&src, &look, 0.0f, M);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) outRot[r][c] = M[r][c];
}

// ---------------------------------------------------------------------------
// Free camera — a thin bridge onto FDS's own Dynamic_Camera(). See SceneIngest.h.
// ---------------------------------------------------------------------------

static void PublishFreeCam(Scene &s) {
    CalcPersp(&FC);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) s.camera.rot[r][c] = FC.Mat[r][c];
    s.camera.src[0] = FC.ISource.x;
    s.camera.src[1] = FC.ISource.y;
    s.camera.src[2] = FC.ISource.z;
    s.camera.perspX = FC.PerspX;
    s.camera.perspY = FC.PerspY;
    s.camera.fov = FC.IFOV;
}

void FreeCamSyncFromScene(const Scene &s) {
    FC.ISource = {s.camera.src[0], s.camera.src[1], s.camera.src[2]};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) FC.Mat[r][c] = s.camera.rot[r][c];
    if (s.camera.fov > 1.0f) FC.IFOV = s.camera.fov;
    CalcPersp(&FC);
}

void FreeCamInit(const Scene &s) {
    // The engine's per-scene speed calibration: Vel_Speed = sqrt(FZP)/180 and
    // both `, .` / `K L` dials reset. Passing nullptr for the camera because we
    // seed FC from the INGEST camera below (which may be a --cam= review pose,
    // not the scene camera).
    Calibrate_FreeCamera_ForScene(g_scene.FZP > 0.0f ? g_scene.FZP : s.camera.farZ,
                                  nullptr);
    FreeCamSyncFromScene(s);
    // Dynamic_Camera integrates the engine's FV/FT velocity globals. This
    // process never ran another scene through them, so they are still the
    // zero-initialised statics; nothing to reset, and they are not declared in
    // FDS_VARS.H, so reaching for them would mean an extern this arm has no
    // business writing.
}

void FreeCamStep(Scene &s, const FreeCamInput &in, float dtSeconds) {
    // DisplaceTest.cpp:1183 — dTime = (Timer - TTrd) * 0.25, with Timer a 100 Hz
    // tick clock. So one real second is 100 ticks is dTime 25.
    dTime = dtSeconds * 100.0f * 0.25f;
    if (dTime > 2.0f) dTime = 2.0f;     // a hitch must not launch the camera
    // Key state -> the engine's Keyboard[]. Every alias here is CAMERAS.CPP's
    // (Dynamic_Camera:705-757), including the legacy Z-for-backward binding.
    std::memset(const_cast<char *>(Keyboard), 0, sizeof(Keyboard));
    auto set = [](int sc, bool v) { if (v) const_cast<char *>(Keyboard)[sc] = 1; };
    set(ScW,     in.fwd);
    set(ScS,     in.back);       set(ScZ,      in.back);
    set(ScA,     in.left);       set(ScEnd,    in.left);
    set(ScD,     in.right);      set(ScPgDn,   in.right);
    set(ScQ,     in.up);         set(ScGrayPlus,  in.up);
    set(ScE,     in.down);       set(ScGrayMinus, in.down);
    set(ScLeft,  in.yawLeft);    set(ScRight,  in.yawRight);
    set(ScUp,    in.pitchUp);    set(ScDown,   in.pitchDown);
    set(ScHome,  in.rollLeft);   set(ScPgUp,   in.rollRight);
    set(ScComma, in.slower);     set(ScPeriod, in.faster);
    set(ScK,     in.rotSlower);  set(ScL,      in.rotFaster);
    Dynamic_Camera();
    std::memset(const_cast<char *>(Keyboard), 0, sizeof(Keyboard));
    PublishFreeCam(s);
}

void FreeCamMouseLook(Scene &s, float dYaw, float dPitch) {
    if (dYaw == 0.0f && dPitch == 0.0f) return;
    // Yaw in the WORLD frame, on each row's (x,z) — Dynamic_Camera:809-830.
    if (dYaw != 0.0f) {
        const float c = std::cos(dYaw), sn = std::sin(dYaw);
        for (int r = 0; r < 3; ++r) {
            const float x = FC.Mat[r][0], z = FC.Mat[r][2];
            FC.Mat[r][0] =  c * x + sn * z;
            FC.Mat[r][2] = -sn * x + c * z;
        }
    }
    // Pitch stays camera-LOCAL, same call Dynamic_Camera makes.
    if (dPitch != 0.0f) Matrix_Rotation(FC.Mat, dPitch, 0.0f, 0.0f);
    PublishFreeCam(s);
}

void FreeCamDumpPose(const Scene &s) {
    const float *e = s.camera.src;
    const float fx = s.camera.rot[2][0], fy = s.camera.rot[2][1], fz = s.camera.rot[2][2];
    // DisplaceTest.cpp:1189-1202's own line, so a pose captured here pastes into
    // the same headless Pose{} table.
    std::fprintf(stderr,
        "[DTEST-POSE] { Vector(%.2ff,%.2ff,%.2ff), Vector(%.2ff,%.2ff,%.2ff), "
        "%.1ff, \"probe\" },\n",
        e[0], e[1], e[2], e[0] + fx * 10.0f, e[1] + fy * 10.0f, e[2] + fz * 10.0f,
        s.camera.fov);
    // And the form THIS arm and FDS_GREETS_CAM take.
    std::fprintf(stderr, "[GPUBENCH-POSE] --cam=\"%.8g,%.8g,%.8g,%.8g,%.8g,%.8g\"\n",
                 e[0], e[1], e[2], fx, fy, fz);
}

void CameraTrack(Scene &s, const LoadOptions &opt, float t0, float t1, float step) {
    LoadOptions o = opt;
    // The scripted camera, never a pinned pose — this is the spline being asked
    // to prove it moves.
    o.camPose.clear();
    o.verbose = false;
    if (step <= 0.0f) step = 1.0f;
    std::fprintf(stderr,
        "[CAMTRACK] the AUTHORED spline, evaluated by the engine's own\n"
        "[CAMTRACK]   Animate_Objects -> Spline_Calc_3D(Source/Target) +\n"
        "[CAMTRACK]   Spline_Calc_1D(Roll/FOV) -> Kick_Camera -> CalcPersp.\n"
        "[CAMTRACK]   Format matches DEMO/Snapshot.cpp's [CAM] line.\n");
    for (float t = t0; t <= t1 + 1e-3f; t += step) {
        Reanimate(s, o, t);
        std::fprintf(stderr,
            "[CAM] t=%d pos=(%.0f, %.0f, %.0f)  fwd=(%.3f, %.3f, %.3f)  IFOV=%.1f"
            "  CurFrame=%.2f\n",
            int(t), s.camera.src[0], s.camera.src[1], s.camera.src[2],
            s.camera.rot[2][0], s.camera.rot[2][1], s.camera.rot[2][2],
            s.camera.fov, s.curFrame);
        // FULL precision, in the exact form FDS_GREETS_CAM / --cam= take. This
        // is what makes "does the CPU's own spline put the camera here?"
        // answerable: force the CPU to this pose and diff it against the CPU's
        // unforced frame. A 3-decimal direction is ~1 px of angular error at
        // 1920 wide, which is enough to fail a byte comparison on its own.
        std::fprintf(stderr, "[CAMSTR] t=%d --cam=\"%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\"\n",
                     int(t), s.camera.src[0], s.camera.src[1], s.camera.src[2],
                     s.camera.rot[2][0], s.camera.rot[2][1], s.camera.rot[2][2]);
    }
    // The KEYFRAMES the spline is interpolating BETWEEN, so "it interpolates"
    // can be checked against "it snaps": a snapping camera reproduces only these.
    if (g_scene.CameraHead) {
        const Spline &S = g_scene.CameraHead->Source;
        std::fprintf(stderr, "[CAMTRACK] Source spline: %d keys, frames",
                     int(S.NumKeys));
        for (int i = 0; i < S.NumKeys; ++i)
            std::fprintf(stderr, " %.0f", S.Keys[i].Frame);
        std::fprintf(stderr, "  (FOV spline: %d keys)\n",
                     int(g_scene.CameraHead->FOV.NumKeys));
    }
}

bool Reanimate(Scene &out, const LoadOptions &opt, float demoT) {
    if (!g_loaded) return false;
    ::Scene &sc = g_scene;
    // Exactly how RENDER.CPP derives the engine frame: t = Timer/SceneTime, then
    // CurFrame = lerp(StartFrame, EndFrame, t). DemoTimeToCurFrame is greets' own
    // form of that (GREETS.CPP:3374).
    out.curFrame = DemoTimeToCurFrame(sc, demoT);
    CurFrame = out.curFrame;
    Animate_Objects(&sc, sc.CameraHead);
    RefreshBatchTransforms(out, sc);
    LoadOptions o = opt;
    o.demoT = int(demoT);
    o.verbose = false;
    RefreshLights(out, o, sc);
    RefreshCamera(out, o, sc);
    return true;
}

// Hash the de-indexed vertex buffer so the "do verts change under animation?"
// claim is MEASURED rather than assumed.
uint64_t VertexHash(const Scene &s) {
    uint64_t h = 1469598103934665603ull;
    const uint8_t *p = reinterpret_cast<const uint8_t *>(s.verts.data());
    const size_t n = s.verts.size() * sizeof(Vertex);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

}  // namespace gpubench
