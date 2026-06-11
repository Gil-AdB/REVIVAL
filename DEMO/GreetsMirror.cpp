#include "GreetsMirror.h"

#include <Base/FDS_DECS.H>  // Matrix_Copy + Mat_ID
#include <Base/FDS_VARS.H>  // MatrixXVector template
#include <Base/Face.h>
#include <Base/FeatureFlags.h>
#include <Base/Material.h>
#include <Base/Matrix.h>
#include <Base/Object.h>
#include <Base/Omni.h>
#include <Base/Scene.h>
#include <Base/TriMesh.h>
#include <Base/Vertex.h>
#include <FILLERS/Mekalele.h>  // g_gbuffer + GBuffer::mirrorId plane
#include <RENDER/OffscreenView.h>  // OffscreenViewScope (RTT world swap)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <Base/FrameState.h>  // fds::g_mainCamera / g_mainFaces (RTT pass)

// Provided by MISC/PREPROC.CPP — stamps F->A_idx/B_idx/C_idx for SoA.
extern void Compute_FaceVertexIndices(TriMesh *T);
// DEMO/CITY.CPP — builds a Material around a Sachletz-tiled 32-bit
// texture (Txtr_Nomip | Txtr_Tiled, numMipmaps=1). Used for the
// second-order mirror RTT slots.
extern Material *Materialize(void *data, int x, int y);
// FDS/VESA — per-surface scanline offset table (the renderer reads the
// global YOffs alias that VESA_Surface2Global points at VS->YTable).
extern void Build_YOffs_Table(VESA_Surface *VS);

// getAlignedBlock / getAlignedType come in via FDS_DECS.H above.

namespace fds {
namespace {

// A previous BuildMirror call leaves its clone mesh wired into
// sc->ObjectHead with Obj->Name starting "__mirrorClone_". Any later
// BuildMirror call must skip those:
// - Cloning them would produce "reflections of reflections" that
//   don't correspond to any physical geometry and end up rendering
//   at unexpected screen positions (the user's "objects appearing
//   out of bounds" complaint).
// - The wall-detection loop would match the cloned mirror panels
//   (same material name, same normal after reflection through a
//   parallel plane) and stamp the mirrorId mask at the CLONED
//   panel's screen position — covering pixels far outside the
//   actual wall's footprint.
inline bool isCloneMesh(Object *Obj) {
    constexpr const char *kPrefix = "__mirrorClone_";
    constexpr size_t kPrefixLen = 14;
    return Obj && Obj->Name
        && std::strncmp(Obj->Name, kPrefix, kPrefixLen) == 0;
}

// Local reflection helpers. Captured by lambdas that need them.
inline Vector reflectPointAcross(const Vector &p, const Vector &N, float d) {
    const float k = 2.0f * (N.x*p.x + N.y*p.y + N.z*p.z + d);
    return { p.x - k*N.x, p.y - k*N.y, p.z - k*N.z };
}
inline Vector reflectDirAcross(const Vector &v, const Vector &N) {
    const float k = 2.0f * (N.x*v.x + N.y*v.y + N.z*v.z);
    return { v.x - k*N.x, v.y - k*N.y, v.z - k*N.z };
}
inline float distToPlane(const Vector &P, const Vector &N, float d) {
    return std::fabs(N.x*P.x + N.y*P.y + N.z*P.z + d);
}

// Generic walker over faces matching a predicate. Visitor receives
// (T, F, wN) where wN is the face's world-space normal. Predicate is
// (const Face&) -> bool; lets us pick wall surfaces by material name,
// texture filename, or any custom rule without changing the walker.
template <class Pred, class Visit>
void walkWallFacesIf(Scene *sc, Pred &&pred, Visit &&visit) {
    for (Object *Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh) continue;
        // Never treat faces inside a prior mirror's clone mesh as wall
        // candidates: the teleporter clone contains mirror-image copies
        // of the P_TEXT screens (same texture), and collecting those
        // spawned phantom mirrors whose planes sit OUTSIDE the room —
        // wall-less (the retarget loop rightly skips clone meshes) and
        // permanently invisible, but each cloning the scene + omnis.
        if (isCloneMesh(Obj)) continue;
        TriMesh *T = (TriMesh*)Obj->Data;
        if (!T || !T->Faces) continue;
        for (DWord fi = 0; fi < T->FIndex; ++fi) {
            Face &F = T->Faces[fi];
            if (!F.A || !F.B || !F.C) continue;
            if (!pred(F)) continue;
            Vector localN = F.N;
            Vector wN;
            MatrixXVector(T->RotMat, &localN, &wN);
            visit(T, F, wN);
        }
    }
}

// Built-in selectors. Used by the by-name and by-texture-filename
// public entry points. Predicates are dirt-cheap lambdas the compiler
// will fully inline through the templated walker.
inline auto matNameSelector(const char *name) {
    return [name](const Face &F) -> bool {
        return F.Txtr && F.Txtr->Name &&
               std::strcmp(F.Txtr->Name, name) == 0;
    };
}
inline auto textureNameSelector(const char *fileName) {
    return [fileName](const Face &F) -> bool {
        return F.Txtr && F.Txtr->Txtr && F.Txtr->Txtr->FileName &&
               std::strstr(F.Txtr->Txtr->FileName, fileName) != nullptr;
    };
}

// Build a 1x1 BGRA Texture from a Material BaseCol so the deferred
// transparent rasterizer (which derefs F->Txtr->Txtr at LSizeX/LSizeY)
// has a valid texture to sample when the source mat is flat-shaded.
Texture *synthesizeFlatTexture(const Color &col) {
    Texture *T = new Texture();
    std::memset(T, 0, sizeof(Texture));
    T->BPP    = 32;
    T->SizeX  = 1; T->SizeY  = 1;
    T->LSizeX = 0; T->LSizeY = 0;
    T->Data   = (byte*)_aligned_malloc(4, 16);
    const dword bgra =
        (dword(uint8_t(std::min(std::max(col.B, 0.0f), 255.0f))) << 0)  |
        (dword(uint8_t(std::min(std::max(col.G, 0.0f), 255.0f))) << 8)  |
        (dword(uint8_t(std::min(std::max(col.R, 0.0f), 255.0f))) << 16) |
        (dword(0xFFu) << 24);
    ((dword*)T->Data)[0] = bgra;
    T->Mipmap[0]    = T->Data;
    T->numMipmaps   = 1;
    T->Flags        = Txtr_Nomip | Txtr_Tiled;
    return T;
}

}  // namespace

namespace {
template <class Pred>
MirrorPlane FindMirrorPlaneImpl(Scene *sc, Pred &&pred, const char *label)
{
    MirrorPlane out{};
    if (!sc) return out;

    // Collect every wall face's unit-length world normal up-front. We
    // then pick the seed face as the one with the LARGEST coplanar
    // neighborhood — averaging all normals fails when the wall is part
    // of a 3D mesh whose other sides cancel out the "real" surface
    // direction (e.g. a screen box has front + back + 4 edges; the
    // average is ~0 so every face looked like an outlier).
    struct Sample {
        TriMesh *T;
        Face    *F;
        Vector   wN;       // unit-length world normal
    };
    std::vector<Sample> samples;
    walkWallFacesIf(sc, pred, [&](TriMesh *T, Face &F, const Vector &wN) {
        Vector u = wN;
        u.normalize();
        samples.push_back({T, &F, u});
    });
    if (samples.empty()) {
        std::fprintf(stderr, "[MIRROR] no '%s' faces found\n", label);
        return out;
    }

    // Pick seed: the face whose normal has the most neighbors within
    // 30° (dot ≥ 0.866). For small N this O(N²) walk is trivially
    // cheap; we never see more than a few hundred faces per material.
    int bestSeed = 0, bestCount = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
        int count = 0;
        for (const auto &s : samples) {
            const float dot = samples[i].wN.x*s.wN.x + samples[i].wN.y*s.wN.y + samples[i].wN.z*s.wN.z;
            if (dot >= 0.866f) ++count;
        }
        if (count > bestCount) { bestCount = count; bestSeed = int(i); }
    }
    const Vector seedN = samples[bestSeed].wN;

    // Average unit normal + plane offset over the seed's coplanar cluster.
    Vector accN = {0.0f, 0.0f, 0.0f};
    float accD = 0.0f;
    Vector accCtr = {0.0f, 0.0f, 0.0f};   // world-space wall centroid
    int keptCount = 0, droppedOutlier = 0;
    for (const auto &s : samples) {
        const float dot = s.wN.x*seedN.x + s.wN.y*seedN.y + s.wN.z*seedN.z;
        if (dot < 0.866f) { ++droppedOutlier; continue; }
        // World-space centroid of this face (A,B,C transformed by the
        // owning mesh's RotMat + IPos), accumulated for the wall's
        // overall centroid. Used by the snapshot debug camera to aim
        // squarely at the mirror; the per-face Pos alone is mesh-local
        // and would point the camera at the wrong world location.
        const Vertex *vtx[3] = { s.F->A, s.F->B, s.F->C };
        Vector wSum = {0,0,0};
        for (int k = 0; k < 3; ++k) {
            Vector lp = vtx[k]->Pos, wp;
            MatrixXVector(s.T->RotMat, &lp, &wp);
            wp += s.T->IPos;
            wSum += wp;
        }
        accCtr.x += wSum.x / 3.0f;
        accCtr.y += wSum.y / 3.0f;
        accCtr.z += wSum.z / 3.0f;
        Vector localA = s.F->A->Pos;
        Vector wA;
        MatrixXVector(s.T->RotMat, &localA, &wA);
        wA += s.T->IPos;
        const float d = -(s.wN.x*wA.x + s.wN.y*wA.y + s.wN.z*wA.z);
        accN += s.wN;
        accD += d;
        ++keptCount;
    }
    if (keptCount == 0) {
        std::fprintf(stderr,
            "[MIRROR '%s'] all %zu wall faces dropped as outliers\n",
            label, samples.size());
        return out;
    }
    out.N = accN;
    out.N.normalize();
    out.d = accD / float(keptCount);
    out.centroid = { accCtr.x / keptCount, accCtr.y / keptCount, accCtr.z / keptCount };
    out.faceCount = keptCount;
    out.valid = true;
    std::fprintf(stderr,
        "[MIRROR '%s'] plane N=(%.3f,%.3f,%.3f) d=%.3f from %d/%zu faces "
        "(%d outliers)\n",
        label, out.N.x, out.N.y, out.N.z, out.d,
        keptCount, samples.size(), droppedOutlier);
    return out;
}

}  // namespace (FindMirrorPlaneImpl template)

// Public wrapper retained for back-compat — selects by material name.
MirrorPlane FindMirrorPlaneByMatName(Scene *sc, const char *wallMatName)
{
    if (!sc || !wallMatName) return MirrorPlane{};
    return FindMirrorPlaneImpl(sc, matNameSelector(wallMatName), wallMatName);
}

// Public entry — pick wall faces by Texture::FileName substring match.
// Lets greets target the real text-display panels (TEXTURES/P_TEXT.JPG)
// rather than guessing material names — multiple distinct Materials
// share the same dynamic-text texture there.
MirrorPlane FindMirrorPlaneByTextureName(Scene *sc, const char *textureFileName)
{
    if (!sc || !textureFileName) return MirrorPlane{};
    return FindMirrorPlaneImpl(sc, textureNameSelector(textureFileName), textureFileName);
}

// ─── --mirror-prof timing ────────────────────────────────────────────
// Wall-time accumulators for the three per-frame mirror passes, printed
// as averages every FDS_MIRROR_PROF_EVERY frames (default 120). The
// frame counter advances in UpdateAllMirrors (called exactly once per
// frame by every scene driver).
namespace {
struct MirrorProfAccum {
    double updMs = 0.0, stampMs = 0.0, rttMs = 0.0;
    int    frames = 0, activeSum = 0, rttJobsSum = 0;
};
MirrorProfAccum g_mirrorProf;

struct ScopedMirrorMs {
    double *acc;
    std::chrono::steady_clock::time_point t0;
    explicit ScopedMirrorMs(double *a)
        : acc(a), t0(std::chrono::steady_clock::now()) {}
    ~ScopedMirrorMs() {
        *acc += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    }
};
}  // namespace

// Monotonic mirror id counter. Each successful BuildMirror call grabs
// the next id (1..255); the value gets written into Face::mirrorMaskTag
// on every face that participates in this mirror (walls + clones), and
// the per-pixel mask check in Mekalele compares against it. Wrap-around
// past 255 is treated as a hard error — at that point the encoding is
// out of u8 space and we'd need to widen the plane.
static uint8_t s_nextMirrorId = 1;

namespace {
template <class Pred>
Mirror BuildMirrorImpl(Scene *sc, Pred &&isWall, const char *label)
{
    Mirror m{};
    if (!sc) return m;
    m.wallMaterialName = label ? label : "";
    m.plane = FindMirrorPlaneImpl(sc, isWall, label);
    if (!m.plane.valid) return m;
    if (s_nextMirrorId == 0) {
        std::fprintf(stderr, "[MIRROR '%s'] all 255 mirror ids in use — skipping\n",
                     label);
        return m;
    }
    m.id = s_nextMirrorId++;

    const Vector &N = m.plane.N;
    const float   d = m.plane.d;
    // A wall face is the actual mirror SURFACE iff it's also coplanar
    // with the mirror plane. For a flat wall every wall face qualifies;
    // for a 3D mesh wall (screen box) only the front face does, and
    // we should clone the back/sides like any other geometry.
    auto isMirrorSurface = [&](const Face &F, TriMesh *T) -> bool {
        if (!isWall(F)) return false;
        Vector localN = F.N;
        Vector wN;
        MatrixXVector(T->RotMat, &localN, &wN);
        wN.normalize();
        const float dot = wN.x*N.x + wN.y*N.y + wN.z*N.z;
        if (dot < 0.866f) return false;
        // Coplanarity by DISTANCE too, not just normal: a parallel
        // same-material face at a different depth is regular geometry
        // to clone, not part of this mirror's surface. Without this it
        // got retargeted + stamped but reflected across the wrong
        // plane (garbage reflection in the offset panel).
        Vector localA = F.A->Pos, wA;
        MatrixXVector(T->RotMat, &localA, &wA);
        wA += T->IPos;
        return distToPlane(wA, N, d) <= 0.6f;
    };

    // Count total verts/faces (excluding mirror-surface faces — we
    // want a hole where the mirror is, not a mirror image of the
    // mirror itself).
    DWord totalVerts = 0, totalFaces = 0;
    for (Object *Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh) continue;
        if (isCloneMesh(Obj)) continue;
        TriMesh *T = (TriMesh*)Obj->Data;
        if (!T || !T->Verts || !T->Faces) continue;
        totalVerts += T->VIndex;
        for (DWord fi = 0; fi < T->FIndex; ++fi) {
            if (isMirrorSurface(T->Faces[fi], T)) continue;
            if (!T->Faces[fi].A) continue;
            ++totalFaces;
        }
    }
    if (totalFaces == 0) {
        std::fprintf(stderr,
            "[MIRROR '%s'] nothing to clone (0 non-wall faces)\n", label);
        return m;
    }

    // Allocate clone mesh + splines (Animate_Objects requires valid
    // single-key Pos/Scale/Rotate splines even for static meshes; see
    // engine-axis-quaternion memory for the Quaternion-init trap).
    TriMesh *MM = getAlignedType<TriMesh>(16);
    std::memset(MM, 0, sizeof(TriMesh));
    MM->Verts = (Vertex*)getAlignedBlock(sizeof(Vertex) * size_t(totalVerts), 16);
    MM->Faces = (Face*)getAlignedBlock(sizeof(Face) * size_t(totalFaces), 16);
    MM->VIndex = totalVerts;
    MM->FIndex = totalFaces;
    Matrix_Copy(MM->RotMat, Mat_ID);
    Matrix_Copy(MM->UnscaledRotMat, Mat_ID);
    MM->IPos   = {0.0f, 0.0f, 0.0f};
    MM->IScale = {1.0f, 1.0f, 1.0f};
    MM->IRot   = {0.0f, 0.0f, 0.0f, 1.0f};
    // Noshading: clone meshes never consume vertex colors — they render
    // deferred-only (per-pixel lighting) and the RTT offscreen pass
    // hides them. Without this, forward Lighting() vertex-lit every
    // ACTIVE mirror's ~27k clone verts against all real omnis each
    // frame (~4 ms/frame of pure waste in greets).
    MM->Flags |= HTrack_Visible | Tri_Noshading;
    auto stampSingleKey = [](Spline &sp, float x, float y, float z, float w) {
        sp.NumKeys = 1; sp.CurKey = 0; sp.Flags = 0;
        sp.Keys = (SplineKey*)std::calloc(1, sizeof(SplineKey));
        sp.Keys[0].Frame = 0.0f;
        sp.Keys[0].Pos.x = x; sp.Keys[0].Pos.y = y; sp.Keys[0].Pos.z = z; sp.Keys[0].Pos.W = w;
        sp.Keys[0].AA.x  = x; sp.Keys[0].AA.y  = y; sp.Keys[0].AA.z  = z; sp.Keys[0].AA.W  = w;
    };
    stampSingleKey(MM->Pos,    0.0f, 0.0f, 0.0f, 0.0f);
    stampSingleKey(MM->Scale,  1.0f, 1.0f, 1.0f, 0.0f);
    stampSingleKey(MM->Rotate, 0.0f, 0.0f, 0.0f, 1.0f);

    // Per-source-mesh dynamic test for UpdateMirror's static-skip.
    // Same spline-extent heuristic as Transform.cpp's isDynamicForBake
    // (kept verbatim-local there); walks the parent chain because the
    // robot's leg meshes inherit their motion from the hull.
    auto meshIsDynamic = [](Object *obj) -> bool {
        constexpr float kPosEps = 0.1f;
        constexpr float kRotEps = 0.01f;
        for (Object *o = obj; o; o = o->Parent) {
            if (o->Type != Obj_TriMesh) continue;
            TriMesh *tm = (TriMesh *)o->Data;
            if (!tm) continue;
            if (tm->Pos.NumKeys > 1 && tm->Pos.Keys) {
                const auto &k0 = tm->Pos.Keys[0].Pos;
                for (DWord i = 1; i < tm->Pos.NumKeys; ++i) {
                    const auto &k = tm->Pos.Keys[i].Pos;
                    if (std::fabs(k.x - k0.x) > kPosEps ||
                        std::fabs(k.y - k0.y) > kPosEps ||
                        std::fabs(k.z - k0.z) > kPosEps) return true;
                }
            }
            if (tm->Rotate.NumKeys > 1 && tm->Rotate.Keys) {
                const auto &q0 = tm->Rotate.Keys[0].Pos;
                for (DWord i = 1; i < tm->Rotate.NumKeys; ++i) {
                    const auto &q = tm->Rotate.Keys[i].Pos;
                    if (std::fabs(q.x - q0.x) > kRotEps ||
                        std::fabs(q.y - q0.y) > kRotEps ||
                        std::fabs(q.z - q0.z) > kRotEps ||
                        std::fabs(q.W - q0.W) > kRotEps) return true;
                }
            }
        }
        return false;
    };

    // Fill verts (world-mirrored) and faces (winding swapped, normal +
    // NormProd reflected, UV pairs swapped to match B↔C).
    DWord vOfs = 0, fOfs = 0;
    Vector bbMin = { 1e30f, 1e30f, 1e30f};
    Vector bbMax = {-1e30f,-1e30f,-1e30f};
    for (Object *Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh) continue;
        if (isCloneMesh(Obj)) continue;  // skip prior mirror clones
        TriMesh *T = (TriMesh*)Obj->Data;
        if (!T || !T->Verts || !T->Faces) continue;
        const bool meshDyn = meshIsDynamic(Obj);
        const DWord vStart = vOfs;
        for (DWord vi = 0; vi < T->VIndex; ++vi) {
            MM->Verts[vOfs] = T->Verts[vi];
            Vector localP = T->Verts[vi].Pos;
            Vector worldP;
            MatrixXVector(T->RotMat, &localP, &worldP);
            worldP.x += T->IPos.x; worldP.y += T->IPos.y; worldP.z += T->IPos.z;
            const Vector mirroredP = reflectPointAcross(worldP, N, d);
            MM->Verts[vOfs].Pos = mirroredP;
            // Directions go through the FULL composed RotMat — the
            // engine's own convention (Transform.cpp: IM = ViewMat ×
            // RotMat for N/Tangent; magnitude normalizes per pixel).
            // NOT UnscaledRotMat: only the spline-Animate path stamps
            // it and the parent-hierarchy composition never reaches it,
            // so for the robot's parented legs/hull it's stale — face
            // normals came out wrong-frame and the cull dropped faces.
            // Tangent too: greets is normal-mapped nearly everywhere,
            // and the raw-copied object-space tangent gave mirrored
            // surfaces a wrong TBN ("shading off, faces OK").
            // Known limitation: the kernel rebuilds B = N × T with a
            // fixed sign, and reflection flips cross-product
            // handedness, so the nmap's V-axis detail is inverted in
            // mirrors — subtle next to the gross error this fixes;
            // needs a per-pixel handedness bit to fully resolve.
            Vector localN = T->Verts[vi].N;
            Vector worldN;
            MatrixXVector(T->RotMat, &localN, &worldN);
            MM->Verts[vOfs].N = reflectDirAcross(worldN, N);
            Vector localT = T->Verts[vi].Tangent;
            Vector worldT;
            MatrixXVector(T->RotMat, &localT, &worldT);
            MM->Verts[vOfs].Tangent = reflectDirAcross(worldT, N);
            bbMin.x = std::min(bbMin.x, mirroredP.x);
            bbMin.y = std::min(bbMin.y, mirroredP.y);
            bbMin.z = std::min(bbMin.z, mirroredP.z);
            bbMax.x = std::max(bbMax.x, mirroredP.x);
            bbMax.y = std::max(bbMax.y, mirroredP.y);
            bbMax.z = std::max(bbMax.z, mirroredP.z);
            ++vOfs;
        }
        for (DWord fi = 0; fi < T->FIndex; ++fi) {
            Face &OF = T->Faces[fi];
            if (isMirrorSurface(OF, T)) continue;
            if (!OF.A || !OF.B || !OF.C) continue;
            // Only reflect faces IN FRONT of the mirror plane. A real
            // mirror reflects what's in front of it; faces on the plane
            // (the teleporter's coplanar emissive "screen emiter" glow)
            // or behind it (the room behind the wall) produce a
            // degenerate/wrong reflection that lands on top of the
            // mirror panel. The coplanar yellow emitter clone was the
            // flat yellow wash filling the greets teleporter.
            {
                auto worldPos = [&](const Vertex *v) {
                    Vector lp = v->Pos, wp;
                    MatrixXVector(T->RotMat, &lp, &wp);
                    wp.x += T->IPos.x; wp.y += T->IPos.y; wp.z += T->IPos.z;
                    return wp;
                };
                const Vector a = worldPos(OF.A), b = worldPos(OF.B), c = worldPos(OF.C);
                const float cx = (a.x+b.x+c.x)/3.0f;
                const float cy = (a.y+b.y+c.y)/3.0f;
                const float cz = (a.z+b.z+c.z)/3.0f;
                const float sd = N.x*cx + N.y*cy + N.z*cz + d;
                if (sd <= 0.05f) continue;  // on/behind the plane → no reflection
            }
            Face &CF = MM->Faces[fOfs];
            CF = OF;
            CF.A = MM->Verts + vStart + (OF.A - T->Verts);
            CF.B = MM->Verts + vStart + (OF.C - T->Verts);  // swap
            CF.C = MM->Verts + vStart + (OF.B - T->Verts);  // swap
            std::swap(CF.U2, CF.U3);
            std::swap(CF.V2, CF.V3);
            std::swap(CF.EU2, CF.EU3);
            std::swap(CF.EV2, CF.EV3);
            // Face normal must be the source's WORLD normal (RotMat ×
            // local N) before reflecting — the clone mesh has identity
            // transform, so its face N is consumed as world-space. The
            // robot's meshes carry a real rotation; using OF.N raw gave
            // clone faces a wrong cull normal even at init.
            {
                Vector ln = OF.N, wn;
                MatrixXVector(T->RotMat, &ln, &wn);
                CF.N = reflectDirAcross(wn, N);
            }
            // Engine convention: NormProd = -(N · A).
            CF.NormProd = -(CF.N.x * CF.A->Pos.x +
                            CF.N.y * CF.A->Pos.y +
                            CF.N.z * CF.A->Pos.z);
            m.cloneFaceSrc.push_back({&OF, T, meshDyn});
            // Tag every clone face with the owning mirror's id. Mekalele
            // reads this into ctx.mirrorTag and rejects any pixel whose
            // gb.mirrorId doesn't match — so clones can only paint inside
            // their own mirror's screen-space footprint.
            CF.mirrorMaskTag = m.id;
            // ownerMirrorId tags every pixel this clone face commits
            // (Mekalele writes it into gb.mirrorId past the p_mask
            // commit gate). The deferred lighting kernel filters omnis
            // by this byte, so clone pixels see only their own
            // mirror's cloned omnis — and crucially, foreground
            // geometry occluding the mirror gets ownerMirrorId=0 from
            // its own commit, overriding the z-ignorant 2D stamp.
            CF.ownerMirrorId = m.id;
            ++fOfs;
        }
        m.meshRanges.push_back({T, vStart, T->VIndex, meshDyn});
    }
    // Nothing in FRONT of the plane survived the per-face side test
    // (wall faces away from the room, or sits at its far edge). A
    // wall-retargeted mirror with no reflection content would render
    // as a void portal — bail before linking/retargeting so the wall
    // keeps its original material instead. The pre-allocated clone
    // storage leaks; this runs once at scene init.
    if (fOfs == 0) {
        std::fprintf(stderr,
            "[MIRROR '%s'] nothing in front of plane — skipping mirror\n",
            label);
        // Reclaim the id: nothing kept it (clone mesh is discarded,
        // retarget + omni cloning haven't run yet) and TagFacesBehind-
        // Mirrors only supports ids ≤ 31, so don't burn slots.
        --s_nextMirrorId;
        return Mirror{};
    }
    m.cloneMesh  = MM;
    m.clonedVerts = int(vOfs);
    m.clonedFaces = int(fOfs);
    // The front-side skip can leave fewer faces than pre-counted, so
    // shrink FIndex to what we actually wrote — otherwise the tail
    // slots are uninitialised garbage that the rasterizer would draw.
    MM->FIndex = fOfs;

    // Loose bsphere from the mirrored bbox.
    Vector ctr = { (bbMin.x + bbMax.x) * 0.5f,
                   (bbMin.y + bbMax.y) * 0.5f,
                   (bbMin.z + bbMax.z) * 0.5f };
    const float dx = bbMax.x - bbMin.x;
    const float dy = bbMax.y - bbMin.y;
    const float dz = bbMax.z - bbMin.z;
    const float radSq = 0.25f * (dx*dx + dy*dy + dz*dz);
    MM->BSphereCtr        = ctr;
    MM->BSphereRad        = radSq;
    MM->BSphereRadius     = std::sqrt(radSq);
    MM->BSphereScreenPos  = {0.0f, 0.0f, 0.0f};

    Compute_FaceVertexIndices(MM);

    // Wrap clone in an Object and link into the scene's head lists.
    Object *MObj = getAlignedType<Object>(16);
    std::memset(MObj, 0, sizeof(Object));
    MObj->Type = Obj_TriMesh;
    MObj->Data = MM;
    MObj->Pos  = &MM->IPos;
    MObj->Rot  = &MM->RotMat;
    {
        // Name suffix from the wall material so multiple mirrors get
        // distinct names in diagnostics ("mirror_teleporter" etc.).
        // Prefix is matched by isCloneMesh() to skip prior clones in
        // subsequent BuildMirror calls. Use an underscore + bracket
        // prefix that's unlikely to collide with user-authored names.
        std::string nm = std::string("__mirrorClone_") + (label?label:"x");
        MObj->Name = (char*)std::malloc(nm.size() + 1);
        std::strcpy(MObj->Name, nm.c_str());
    }
    MObj->Next = sc->ObjectHead;
    if (sc->ObjectHead) sc->ObjectHead->Prev = MObj;
    sc->ObjectHead = MObj;
    MM->Next = sc->TriMeshHead;
    if (sc->TriMeshHead) sc->TriMeshHead->Prev = MM;
    sc->TriMeshHead = MM;

    // Retarget mirror-surface wall faces. Two distinct treatments based
    // on whether the source material is already transparent with a
    // real texture:
    //
    // (A) Source is Mat_Transparent + textured (greets's P_TEXT screens):
    //     half-silvered-glass behavior. KEEP the source material as-is
    //     — the existing transparent blend `litRGB*α + dst*(1-α)`
    //     already does the right thing: the screen's text shows where
    //     its alpha is opaque, and the cloned mirror geometry (rendered
    //     into the opaque G-buffer beneath the wall) shows through
    //     where alpha is transparent. No material clone needed; just
    //     tag the face for the mask pre-pass.
    //
    // (B) Source is opaque (teleporter-style flat walls): synthesize
    //     a silvery transparent material clone so the surface visually
    //     reads as a mirror. Cool silver BaseCol, cranked Specular,
    //     low alpha so the mirror clone beneath dominates. Flat-shaded
    //     sources also get a 1×1 silver stub texture for the deferred
    //     transparent rasterizer's LSizeX/LSizeY read.
    // Wall material is a passive gateway to the reflected world, not
    // a lit surface in its own right. Knobs:
    //   - Specular = 0 so warm omnis don't write a spec lobe.
    //   - Diffuse = 0 makes the lit color INDEPENDENT of per-strip
    //     omni coverage. The deferred transparent kernel uses
    //     per-strip light lists, so a wall pixel in a strip without
    //     nearby omnis (because greets's lights are clustered) would
    //     light differently from one in a strip with omnis — visible
    //     as tile-by-tile popping of the silver tint. With Diff=0,
    //     omnis contribute nothing to lit; the wall reads the same
    //     per-pixel no matter which strip processes it.
    //   - Luminosity gives the wall a small fixed self-color so it's
    //     still visibly silvery (lit = Lum*255 = 38; 5% alpha onto
    //     VPage is ~2 units — a faint cool tint over the reflection).
    //   - BaseCol picked cool/neutral; the magnitude doesn't matter
    //     much since we're at 5% alpha + Diff=0.
    // Alpha is high enough that the silver tint is visible over a
    // black/empty backdrop and obviously dims a bright reflection,
    // but still low enough that the underlying reflection dominates
    // and the user sees through. Luminosity > 1 saturates the lit
    // value at the kernel's 250 clamp, giving full BaseCol×0.98 per
    // pixel — stable across strips since Diff=0.
    // No tint. The wall face still rasterises into xpar (for the
    // mask stamp + xparZ bound) but the deferred transparent kernel
    // skips its composition: kMirrorAlpha = -1 is the sentinel for
    // "preserve dst, contribute nothing." Plain XparBlendAlpha = 0
    // would NOT work because the kernel routes alpha==0 to the
    // legacy `litRGB + dst/2` formula and ends up halving the
    // reflected clones behind the wall.
    constexpr float kMirrorAlpha       = -1.0f;
    constexpr float kMirrorSpecular    = 0.0f;
    constexpr float kMirrorDiffuse     = 0.0f;
    constexpr float kMirrorLuminosity  = 0.0f;
    const Color     kMirrorSilver      = { 0.0f, 0.0f, 0.0f, 0.0f };
    // Capture the original wall-face material pointer the first time
    // we see one. After the retarget loop we use this to fix up any
    // EARLIER mirror's clone mesh that already cloned this material
    // before we replaced it — without that fix-up the previous
    // mirror's clones of this mirror's panel still reference the
    // source material with its full Diffuse, so the back-mirror's
    // view of the side-mirror panel was lit per-strip and the silver
    // tint flickered tile-by-tile inside the back mirror's image.
    Material *sourceWallMat = nullptr;
    int halfSilveredWalls = 0;
    for (Object *Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh) continue;
        if (Obj == MObj) continue;
        if (isCloneMesh(Obj)) continue;  // skip prior mirror clones
        TriMesh *T = (TriMesh*)Obj->Data;
        if (!T || !T->Faces) continue;
        for (DWord fi = 0; fi < T->FIndex; ++fi) {
            Face &F = T->Faces[fi];
            if (!isMirrorSurface(F, T)) continue;
            // Two wall treatments:
            //
            // (A) Half-silvered glass — source is already transparent
            //     WITH a real texture (greets's P_TEXT text screens).
            //     Keep the source's LOOK via a clone (texture, flags,
            //     alpha preserved; Diffuse zeroed + Luminosity
            //     saturated for strip-independent lit values — see the
            //     clone-creation comment): the transparent kernel
            //     composites the glowing text over whatever is behind,
            //     and "behind" is the reflected clone world (the real
            //     room behind the panel is gated out per pixel). The
            //     screen reads as a display with a dimmed reflection
            //     in it — the original half-silvered intent.
            //
            // (B) Full portal — opaque source (the teleporter wall).
            //     Retarget to an alpha<0 sentinel material clone so the
            //     wall contributes nothing and the reflection alone
            //     shows.
            const bool halfSilvered =
                (F.Txtr->Flags & Mat_Transparent) && F.Txtr->Txtr;
            if (halfSilvered && !m.wallMatClone) {
                // Half-silvered wall material: keep the source's look
                // (texture, transparency, alpha, Luminosity) but make
                // the lit value PER-STRIP-INDEPENDENT — Diffuse > 0
                // reads the per-strip transparent light lists and the
                // tint pops strip-by-strip (the banding seen across
                // mirrortest's glass wall; greets's Lum=100 screens
                // were immune because their lit value saturates the
                // clamp regardless). With Diff=0 the lit value is
                // purely Luminosity-driven — identical in every strip.
                m.wallMatClone = getAlignedType<Material>(16);
                std::memcpy(m.wallMatClone, F.Txtr, sizeof(Material));
                m.wallMatClone->RelScene   = sc;
                m.wallMatClone->Diffuse    = 0.0f;
                m.wallMatClone->Specular   = 0.0f;
                m.wallMatClone->Next = nullptr;
                if (!MatLib) {
                    m.wallMatClone->Prev = nullptr;
                    MatLib = m.wallMatClone;
                } else {
                    Material *tail = MatLib;
                    while (tail->Next) tail = tail->Next;
                    tail->Next = m.wallMatClone;
                    m.wallMatClone->Prev = tail;
                }
            }
            if (!halfSilvered && !m.wallMatClone) {
                if (!sourceWallMat) sourceWallMat = F.Txtr;
                m.wallMatClone = getAlignedType<Material>(16);
                std::memcpy(m.wallMatClone, F.Txtr, sizeof(Material));
                // RelScene gates Scene_RebuildMatTable inclusion — set
                // explicitly rather than trusting the memcpy'd value.
                m.wallMatClone->RelScene = sc;
                m.wallMatClone->Flags |= Mat_Transparent;
                m.wallMatClone->XparBlendAlpha = kMirrorAlpha;     // 0
                m.wallMatClone->Specular       = kMirrorSpecular;  // 0
                m.wallMatClone->Diffuse        = kMirrorDiffuse;   // 0
                m.wallMatClone->Luminosity     = kMirrorLuminosity;// 0
                m.wallMatClone->BaseCol        = kMirrorSilver;    // all 0
                // Force a tiny synth texture so the rasterizer's
                // F.Txtr->Txtr deref still has a valid pointer; the
                // sampled colour doesn't matter because alpha=0 zeros
                // the whole composition contribution.
                m.wallMatClone->Txtr = synthesizeFlatTexture(kMirrorSilver);
                // Link at MatLib TAIL, not head. Scene_RebuildMatTable
                // (below) assigns matIDs in list order; appending keeps
                // every pre-existing material's ID stable so anything
                // that captured an ID earlier (lightmap bake, gbuffer
                // commits from prior frames) stays valid — same reason
                // SceneBuilder links its materials at the tail.
                m.wallMatClone->Next = nullptr;
                if (!MatLib) {
                    m.wallMatClone->Prev = nullptr;
                    MatLib = m.wallMatClone;
                } else {
                    Material *tail = MatLib;
                    while (tail->Next) tail = tail->Next;
                    tail->Next = m.wallMatClone;
                    m.wallMatClone->Prev = tail;
                }
            }
            F.Txtr = m.wallMatClone;
            if (halfSilvered) ++halfSilveredWalls;
            // Wall face itself is NOT tagged. We used to set
            // F.mirrorMaskTag = m.id here, but Mekalele's per-pixel
            // gb.mirrorId == ctx.mirrorTag test would then apply to
            // the wall face's own rasterization — and StampMirrorMasks
            // fills the mask with its own 2D scanline routine whose
            // sub-pixel coverage doesn't exactly match Mekalele's
            // rasterizer. The mismatch left occasional wall pixels
            // outside the stamped mask, so the check rejected them
            // (whole screens vanishing in greets's P_TEXT mirror was
            // the visible symptom after the transparent gbuffer was
            // added to the mask plane). m.wallFaces still owns these
            // faces so StampMirrorMasks can read them — it doesn't
            // need the tag on the face itself.
            m.wallFaces.push_back(&F);
            m.wallFaceMeshes.push_back(T);
            ++m.wallFacesRetargeted;
        }
    }
    // Fix up faces in previously-built mirrors' clone meshes that
    // still reference the original wall material. Without this, an
    // earlier mirror's reflection of THIS mirror's panel renders the
    // panel through the source material (Diff=1, no XparBlendAlpha)
    // and the wall pixels are lit per-strip, producing tile-by-tile
    // popping inside the earlier mirror's image.
    if (sourceWallMat && m.wallMatClone && sourceWallMat != m.wallMatClone) {
        int patched = 0;
        for (Object *Obj2 = sc->ObjectHead; Obj2; Obj2 = Obj2->Next) {
            if (Obj2->Type != Obj_TriMesh) continue;
            if (!isCloneMesh(Obj2)) continue;
            TriMesh *T2 = (TriMesh*)Obj2->Data;
            if (!T2 || !T2->Faces) continue;
            for (DWord fi = 0; fi < T2->FIndex; ++fi) {
                if (T2->Faces[fi].Txtr == sourceWallMat) {
                    T2->Faces[fi].Txtr = m.wallMatClone;
                    ++patched;
                }
            }
        }
        if (patched > 0) {
            std::fprintf(stderr,
                "[MIRROR '%s'] retargeted %d faces in prior clone meshes "
                "to wallMatClone (cross-mirror panel fix)\n",
                label, patched);
        }
    }
    // Allocate gb.mirrorId plane on first mirror in the scene. Sized
    // to match the engine surface; 1 byte per pixel = 2 MB at 1080p.
    // We allocate on all three engine gbuffers — opaque, transparent
    // front-peel, transparent back-peel — because clones of
    // transparent source faces (e.g. clones of greets's other P_TEXT
    // screens) go through Mekalele's TransparentFront/Back targets
    // and their GBufferSpan reads mirrorId from the matching gbuffer.
    // Leaving the transparent planes empty made the mask short-circuit
    // to "no mask," so cloned transparent geometry was drawing
    // anywhere on screen (= "screens in random places").
    auto ensureMirrorIdSized = [](meka::GBuffer *gb, const char *which) {
        if (!gb) return;
        const size_t needed = gb->normal.size();
        if (gb->mirrorId.size()    < needed) gb->mirrorId.assign   (needed, 0);
        if (gb->mirrorMask.size()  < needed) gb->mirrorMask.assign (needed, 0);
        if (gb->mirrorMaskZ.size() < needed) gb->mirrorMaskZ.assign(needed, 0);
        (void)which;
    };
    if (g_gbuffer) {
        ensureMirrorIdSized(g_gbuffer,                "opaque");
        ensureMirrorIdSized(g_gbufferTransparent,     "xpar-front");
        ensureMirrorIdSized(g_gbufferTransparentBack, "xpar-back");
    } else {
        std::fprintf(stderr,
            "[MIRROR '%s'] WARN: g_gbuffer null at BuildMirror — mask plane not allocated\n",
            label);
    }

    // Clone every omni in the scene. Mirror-side geometry sees the
    // reflected omni; UpdateMirror clamps each omni's range to plane
    // distance for soft compartmentalization.
    int omniCount = 0;
    for (Omni *srcO = sc->OmniHead; srcO; srcO = srcO->Next) {
        // Skip omnis already cloned by ANY mirror (including earlier
        // BuildMirror calls on this scene). Without this, mirror N
        // sees mirror N-1's clones in OmniHead and clones them again,
        // doubling the omni count per added mirror.
        if (srcO->Flags & Omni_MirrorClone) continue;
        Omni *clone = (Omni*)getAlignedBlock(sizeof(Omni), 16);
        std::memcpy(clone, srcO, sizeof(Omni));
        clone->IPos = reflectPointAcross(srcO->IPos, N, d);
        clone->IDir = reflectDirAcross(srcO->IDir, N);
        clone->Flags |= Omni_MirrorClone;
        // The flare Face is held BY VALUE in Omni, but its A/B/C
        // vertex pointers still aim at the SOURCE omni's V after the
        // memcpy — the clone's flare would render at the source's
        // screen position. Repoint at the clone's own V. The tag gates
        // the flare filler on this mirror's stamped footprint — depth
        // alone is NOT enough, because every mirror clones every omni
        // and the other mirrors' clones sit at arbitrary reflected
        // positions that pass plain z tests anywhere in the level.
        clone->F.A = clone->F.B = clone->F.C = &clone->V;
        clone->F.mirrorMaskTag = m.id;
        // Tag this clone with its mirror id. The deferred lighting
        // kernel filters per-pixel against gb.mirrorId, so clones of
        // mirror N's reflected world only receive light from mirror
        // N's omnis (not from originals or from other mirrors'
        // clones). Without this filter the reflected world received
        // 2x the omni population (original + clone), which is the
        // root cause of greets's persistent yellow saturation inside
        // the teleporter mirror.
        clone->mirrorId = m.id;
        clone->Prev = nullptr;
        clone->Next = sc->OmniHead;
        if (sc->OmniHead) sc->OmniHead->Prev = clone;
        sc->OmniHead = clone;
        m.omniClones.push_back({srcO, clone, srcO->IRange, srcO->IRange});
        ++omniCount;
    }

    // Register wallMatClone in the per-scene matID table. Mekalele packs
    // F->Txtr->ID into the committed mat32 and the deferred kernels
    // resolve the Material back through Scene_GetMatTable — the memcpy
    // above copied the SOURCE's ID, so without a rebuild every wall
    // pixel resolved to a different material entirely (untextured
    // 'teleporter' carries the default ID=0 → greets wall pixels lit as
    // MAT 0 'cockpit'; P_TEXT walls resolved to the original Lum=100
    // screens). The alpha<0 sentinel lives on the clone, so the kernels
    // never saw it. Tail-append above keeps pre-existing IDs stable.
    if (m.wallMatClone) Scene_RebuildMatTable(sc);

    std::fprintf(stderr,
        "[MIRROR '%s'] cloned %u verts / %u faces (mirror bbox z=[%.1f..%.1f]); "
        "%d wall faces (%d half-silvered, %d portal); cloned %d omnis\n",
        label, unsigned(vOfs), unsigned(fOfs),
        bbMin.z, bbMax.z, m.wallFacesRetargeted, halfSilveredWalls,
        m.wallFacesRetargeted - halfSilveredWalls, omniCount);
    return m;
}

}  // namespace (BuildMirrorImpl template)

// Public wrappers.
Mirror BuildMirror(Scene *sc, const char *wallMatName)
{
    if (!sc || !wallMatName) return Mirror{};
    return BuildMirrorImpl(sc, matNameSelector(wallMatName), wallMatName);
}
Mirror BuildMirrorByTextureName(Scene *sc, const char *textureFileName)
{
    if (!sc || !textureFileName) return Mirror{};
    return BuildMirrorImpl(sc, textureNameSelector(textureFileName), textureFileName);
}

// ─── Render-to-texture shared bits (order 1 + 2) ─────────────────────────────────
namespace {
constexpr int kRttRes         = 256;  // slot texture edge (pow2)
constexpr int kRttPerFrame    = 2;    // most-visible slots re-rendered per frame

// Orthonormal in-plane basis for a mirror plane. Same construction the
// probe uses — keep in sync (the slot UVs and the per-frame projection
// must agree on (u, v)).
inline void planeBasis(const Vector &n, Vector &u, Vector &v) {
    if (std::fabs(n.y) < 0.9f) u = { n.z, 0.0f, -n.x };
    else                       u = { 1.0f, 0.0f, 0.0f };
    u.normalize();
    v = { n.y*u.z - n.z*u.y,
          n.z*u.x - n.x*u.z,
          n.x*u.y - n.y*u.x };
}
}  // namespace

int BuildMirrorsByTextureName(Scene *sc, const char *textureFileName,
                              std::vector<Mirror> &out,
                              std::vector<MirrorRttSlot> *rttSlots)
{
    if (!sc || !textureFileName) return 0;

    // Collect every matching face with its world-space unit normal and
    // plane offset; clustering keys on both.
    struct WallSample {
        Face    *F;
        TriMesh *T;   // owning mesh (world transform for vert positions)
        Vector   wN;  // unit world normal
        float    d;   // plane offset: wN·P + d = 0
        float    area; // world-space triangle area
    };
    std::vector<WallSample> samples;
    walkWallFacesIf(sc, textureNameSelector(textureFileName),
                    [&](TriMesh *T, Face &F, const Vector &wN) {
        Vector u = wN;
        u.normalize();
        auto worldPos = [&](const Vertex *v) {
            Vector lp = v->Pos, wp;
            MatrixXVector(T->RotMat, &lp, &wp);
            wp += T->IPos;
            return wp;
        };
        const Vector wA = worldPos(F.A);
        const Vector wB = worldPos(F.B);
        const Vector wC = worldPos(F.C);
        const Vector e1 = { wB.x-wA.x, wB.y-wA.y, wB.z-wA.z };
        const Vector e2 = { wC.x-wA.x, wC.y-wA.y, wC.z-wA.z };
        const float cxv = e1.y*e2.z - e1.z*e2.y;
        const float cyv = e1.z*e2.x - e1.x*e2.z;
        const float czv = e1.x*e2.y - e1.y*e2.x;
        const float area = 0.5f * std::sqrt(cxv*cxv + cyv*cyv + czv*czv);
        samples.push_back({&F, T, u,
                           -(u.x*wA.x + u.y*wA.y + u.z*wA.z), area});
    });
    if (samples.empty()) {
        std::fprintf(stderr, "[MIRROR-CLUSTER '%s'] no faces found\n",
                     textureFileName);
        return 0;
    }

    // Greedy seed clustering: same plane = normals within ~18° AND
    // offsets within half a world unit. Tighter than the 30° the
    // single-plane finder uses for outlier rejection — here disagreeing
    // faces become their own mirror instead of being dropped.
    constexpr float kNormalDot    = 0.95f;
    constexpr float kPlaneDistEps = 0.5f;
    std::vector<int> clusterOf(samples.size(), -1);
    int numClusters = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
        if (clusterOf[i] >= 0) continue;
        const int c = numClusters++;
        clusterOf[i] = c;
        for (size_t j = i + 1; j < samples.size(); ++j) {
            if (clusterOf[j] >= 0) continue;
            const float dot = samples[i].wN.x*samples[j].wN.x
                            + samples[i].wN.y*samples[j].wN.y
                            + samples[i].wN.z*samples[j].wN.z;
            if (dot < kNormalDot) continue;
            if (std::fabs(samples[i].d - samples[j].d) > kPlaneDistEps) continue;
            clusterOf[j] = c;
        }
    }

    // Minimum cluster area to become a mirror — runtime flag, since it
    // is the cost/coverage tradeoff knob. Measured greets data: box
    // edge strips 0.30-0.32, central column panels 0.98-1.00, end
    // screens 3.2-172. Each mirror clones the whole scene + 15 omnis;
    // the four column-panel mirrors alone visibly hurt frame time, so
    // the default (2.0) excludes them. --greets-mirror-min-area=0.5
    // brings them back.
    const float kMinMirrorArea = fds::FeatureFlags::greets_mirror_min_area();
    // Below the clone threshold but above this: a FIRST-order RTT
    // mirror (the panel re-renders the singly-reflected real scene
    // into its own small texture — no clone mesh, no omni clones, no
    // mask). Greets's pedestal-box panels (≈1.0 area) land here, and
    // so do the MAIN corridor columns' small screens (0.30 area — the
    // earlier 0.5 cutoff misjudged those as box edge strips and they
    // never became mirrors at all). Requires --mirror-rtt and an
    // rttSlots out-param.
    constexpr float kMinRttArea = 0.2f;
    const bool wantRtt = rttSlots && fds::FeatureFlags::mirror_rtt();

    int added = 0, addedRtt = 0, skippedSlivers = 0, skippedHorizontal = 0;
    for (int c = 0; c < numClusters; ++c) {
        // Sorted member list → O(log n) membership predicate. Face
        // pointers are stable (live scene meshes, allocated at load).
        std::vector<const Face*> members;
        float clusterArea = 0.0f;
        for (size_t i = 0; i < samples.size(); ++i)
            if (clusterOf[i] == c) {
                members.push_back(samples[i].F);
                clusterArea += samples[i].area;
            }
        Vector cN = {0,0,0};
        float  cD = 0.0f;
        for (size_t i = 0; i < samples.size(); ++i)
            if (clusterOf[i] == c) { cN = samples[i].wN; cD = samples[i].d; break; }
        const char *verdict = "built";
        if (clusterArea < kMinRttArea) {
            verdict = "sliver";
            ++skippedSlivers;
        } else if (clusterArea < kMinMirrorArea) {
            verdict = wantRtt ? "rtt" : "sliver";
            if (!wantRtt) ++skippedSlivers;
        }
        if (verdict[0] != 's' && std::fabs(cN.y) > 0.8f) {
            // Skip near-horizontal clusters: display panels are
            // vertical; the screen boxes' TOP/BOTTOM caps share the
            // texture but a floor/ceiling-facing mirror there is never
            // the authored intent and each costs a full scene clone +
            // omni set. (Drop this test if a scene ever wants real
            // floor mirrors.)
            verdict = "horizontal";
            ++skippedHorizontal;
        }
        std::fprintf(stderr,
            "[CLUSTER %2d] N=(%5.2f,%5.2f,%5.2f) d=%8.3f faces=%zu "
            "area=%6.2f -> %s\n",
            c, cN.x, cN.y, cN.z, cD, members.size(), clusterArea, verdict);
        if (verdict[0] == 'r') {
            // ── First-order RTT slot ────────────────────────────────
            // Plane fit: average member normals/offsets.
            Vector pN = {0,0,0};
            float  pD = 0.0f;
            int    nM = 0;
            for (size_t i = 0; i < samples.size(); ++i)
                if (clusterOf[i] == c) { pN += samples[i].wN; pD += samples[i].d; ++nM; }
            pN.normalize();
            pD /= float(nM);
            MirrorRttSlot slot{};
            slot.order = 1;
            slot.bN = pN;
            slot.bD = pD;
            // No winding/axis flip needed: the RTT is an ordinary
            // render of the REAL scene from the reflected position
            // with a PROPER camera basis (normal triangle winding),
            // and the mirror inversion is carried entirely by the ray
            // geometry — texel(W) = scene along ray camPos→W, which is
            // exactly what a viewer sees at panel point W. The UV
            // stamp uses the same projection, so display mapping is
            // consistent by construction (same reason order 2 needed
            // nothing special).
            Vector axU, axV;
            planeBasis(pN, axU, axV);
            slot.axisU = axU;
            slot.axisV = axV;
            slot.u0 = slot.v0 = 1e30f; slot.u1 = slot.v1 = -1e30f;
            for (size_t i = 0; i < samples.size(); ++i) {
                if (clusterOf[i] != c) continue;
                const WallSample &ws = samples[i];
                const Vertex *vs[3] = { ws.F->A, ws.F->B, ws.F->C };
                for (int k = 0; k < 3; ++k) {
                    Vector wp = vs[k]->Pos;
                    if (ws.T) {
                        Vector lp = wp;
                        MatrixXVector(ws.T->RotMat, &lp, &wp);
                        wp += ws.T->IPos;
                    }
                    const float pu = wp.x*axU.x + wp.y*axU.y + wp.z*axU.z;
                    const float pv = wp.x*axV.x + wp.y*axV.y + wp.z*axV.z;
                    slot.u0 = std::min(slot.u0, pu); slot.u1 = std::max(slot.u1, pu);
                    slot.v0 = std::min(slot.v0, pv); slot.v1 = std::max(slot.v1, pv);
                }
            }
            if (slot.u1 - slot.u0 < 1e-3f || slot.v1 - slot.v0 < 1e-3f)
                continue;
            // Aspect-match at constant texel budget (same policy as
            // the second-order slots).
            {
                const float winAspect =
                    (slot.u1 - slot.u0) / (slot.v1 - slot.v0);
                while (winAspect / (float(slot.texW) / float(slot.texH)) >= 2.0f
                       && slot.texW < 1024 && slot.texH > 32) {
                    slot.texW <<= 1; slot.texH >>= 1;
                }
                while ((float(slot.texW) / float(slot.texH)) / winAspect >= 2.0f
                       && slot.texH < 1024 && slot.texW > 32) {
                    slot.texW >>= 1; slot.texH <<= 1;
                }
            }
            std::vector<uint32_t> black(
                size_t(slot.texW) * size_t(slot.texH), 0xFF000000u);
            slot.mat = Materialize(black.data(), slot.texW, slot.texH);
            slot.mat->Luminosity = 1.0f;   // texture holds final colors
            slot.mat->Diffuse    = 0.0f;
            slot.mat->Specular   = 0.0f;
            // Glass reflects on BOTH sides: without this the back view
            // backface-culls into a hole showing the box interior
            // (mirrored text of the far sheet). The per-frame job
            // renders the side the camera is actually on.
            // Mat_Transparent: the panel is a SEMI-TRANSPARENT display
            // — it rides the xpar peel and the deferred transparent
            // kernel composites texel + behind/2 (XparBlendAlpha=0 →
            // the legacy glass formula the original screens used), so
            // free-standing column panels show the scene through the
            // glass under the text + reflection.
            slot.mat->Flags     |= Mat_TwoSided | Mat_Transparent;
            slot.mat->RelScene   = sc;
            {
                char nm[64];
                std::snprintf(nm, sizeof(nm), "__mirrorRtt1_%s_%d",
                              textureFileName, c);
                slot.mat->Name = strdup(nm);
            }
            slot.mat->Next = nullptr;
            if (!MatLib) {
                slot.mat->Prev = nullptr;
                MatLib = slot.mat;
            } else {
                Material *tail = MatLib;
                while (tail->Next) tail = tail->Next;
                tail->Next = slot.mat;
                slot.mat->Prev = tail;
            }
            // The panel faces THEMSELVES become the mirror surface
            // (retargeted to the slot material — the path proven by
            // the bare-mirror diagnostic). The half-silvered "text
            // over reflection" look comes from a CPU composite of the
            // panel's ORIGINAL texture (P_TEXT — the dynamic greets
            // text) into the RTT buffer after each re-render, mapped
            // through the panel's authored UVs. An earlier design put
            // the reflection on a hand-built quad behind transparent
            // glass; hand-built meshes fought five separate engine
            // conventions (inward face normals, quaternion key layout,
            // Face_Reflective dispatch, …) and still never rasterized
            // — the composite needs no new geometry at all.
            {
                // Affine map window(pu,pv) → authored text UV, solved
                // from one panel face's three corners (the authored
                // mapping is linear across the panel rect).
                const Face *protoF = nullptr;
                const TriMesh *protoT = nullptr;
                for (size_t i = 0; i < samples.size(); ++i)
                    if (clusterOf[i] == c) {
                        protoF = samples[i].F;
                        protoT = samples[i].T;
                        break;
                    }
                if (protoF && protoF->Txtr && protoF->Txtr->Txtr) {
                    slot.textTex = protoF->Txtr->Txtr;
                    float P[3][2];
                    const Vertex *vs[3] = { protoF->A, protoF->B, protoF->C };
                    for (int k = 0; k < 3; ++k) {
                        Vector wp = vs[k]->Pos;
                        if (protoT) {
                            Vector lp = wp;
                            MatrixXVector(
                                const_cast<TriMesh*>(protoT)->RotMat, &lp, &wp);
                            wp += protoT->IPos;
                        }
                        P[k][0] = wp.x*axU.x + wp.y*axU.y + wp.z*axU.z;
                        P[k][1] = wp.x*axV.x + wp.y*axV.y + wp.z*axV.z;
                    }
                    const float tu[3] = { protoF->U1, protoF->U2, protoF->U3 };
                    const float tv[3] = { protoF->V1, protoF->V2, protoF->V3 };
                    const float det =
                        (P[1][0]-P[0][0])*(P[2][1]-P[0][1]) -
                        (P[2][0]-P[0][0])*(P[1][1]-P[0][1]);
                    if (std::fabs(det) > 1e-9f) {
                        const float inv = 1.0f / det;
                        auto solve = [&](const float *t, float &ga, float &gb, float &gc) {
                            ga = ((t[1]-t[0])*(P[2][1]-P[0][1]) -
                                  (t[2]-t[0])*(P[1][1]-P[0][1])) * inv;
                            gb = ((t[2]-t[0])*(P[1][0]-P[0][0]) -
                                  (t[1]-t[0])*(P[2][0]-P[0][0])) * inv;
                            gc = t[0] - ga*P[0][0] - gb*P[0][1];
                        };
                        solve(tu, slot.tA[0], slot.tA[1], slot.tA[2]);
                        solve(tv, slot.tA[3], slot.tA[4], slot.tA[5]);
                    } else {
                        slot.textTex = nullptr;  // degenerate mapping
                    }
                }
                // Retarget the real panel faces to the slot material
                // and record their verts for the per-render UV stamp.
                for (size_t i = 0; i < samples.size(); ++i) {
                    if (clusterOf[i] != c) continue;
                    const WallSample &ws = samples[i];
                    ws.F->Txtr = slot.mat;
                    slot.faces.push_back(ws.F);
                    Vertex *vsm[3] = { ws.F->A, ws.F->B, ws.F->C };
                    for (int k = 0; k < 3; ++k) {
                        Vector wp = vsm[k]->Pos;
                        if (ws.T) {
                            Vector lp = wp;
                            MatrixXVector(ws.T->RotMat, &lp, &wp);
                            wp += ws.T->IPos;
                        }
                        const float pu = wp.x*axU.x + wp.y*axU.y + wp.z*axU.z;
                        const float pv = wp.x*axV.x + wp.y*axV.y + wp.z*axV.z;
                        bool seen = false;
                        for (const auto &sv : slot.verts)
                            if (sv.v == vsm[k]) { seen = true; break; }
                        if (!seen) slot.verts.push_back({ vsm[k], pu, pv });
                    }
                }
            }
            rttSlots->push_back(std::move(slot));
            ++addedRtt;
            continue;
        }
        if (verdict[0] != 'b') continue;
        std::sort(members.begin(), members.end());
        char label[128];
        std::snprintf(label, sizeof(label), "%s#%d", textureFileName, c);
        Mirror m = BuildMirrorImpl(sc,
            [&members](const Face &F) {
                return std::binary_search(members.begin(), members.end(), &F);
            },
            label);
        if (m.cloneMesh) {
            out.push_back(std::move(m));
            ++added;
        }
    }
    // First-order slot materials entered MatLib — register their
    // matIDs (the wallMatClone lesson: Mekalele packs F->Txtr->ID and
    // the deferred kernels resolve through the per-scene table).
    if (addedRtt > 0) {
        Scene_RebuildMatTable(sc);
        for (const auto &s : *rttSlots) {
            if (s.order != 1) continue;
            std::fprintf(stderr,
                "[MIRROR-RTT1] %dx%d window %.1fx%.1f u=[%.1f..%.1f] "
                "v=[%.1f..%.1f] N=(%.1f,%.1f,%.1f) d=%.2f\n",
                s.texW, s.texH, s.u1 - s.u0, s.v1 - s.v0,
                s.u0, s.u1, s.v0, s.v1,
                s.bN.x, s.bN.y, s.bN.z, s.bD);
        }
    }
    std::fprintf(stderr,
        "[MIRROR-CLUSTER '%s'] %zu faces -> %d clusters -> %d mirrors "
        "+ %d first-order RTT (%d slivers + %d horizontal skipped)\n",
        textureFileName, samples.size(), numClusters, added, addedRtt,
        skippedSlivers, skippedHorizontal);
    return added;
}

int BuildCompoundMirrors(Scene *sc, std::vector<Mirror> &mirrors)
{
    if (!sc) return 0;
    const int baseCount = int(mirrors.size());
    if (baseCount < 2) return 0;  // need ≥2 mirrors for any compound

    // Snapshot the set of base wall materials. Compound clone geometry
    // skips any face whose material is one of these — the wall faces
    // themselves are already part of the BASE mirrors' clone meshes
    // (and the compound's surface IS one of those clones, retagged).
    std::vector<Material*> wallMats;
    wallMats.reserve(baseCount);
    for (int i = 0; i < baseCount; ++i) {
        if (mirrors[i].wallMatClone) wallMats.push_back(mirrors[i].wallMatClone);
    }
    auto isAnyBaseWallMat = [&](Material *M) -> bool {
        for (Material *W : wallMats) if (W && M == W) return true;
        return false;
    };

    int added = 0;
    for (int aIdx = 0; aIdx < baseCount; ++aIdx) {
        Mirror &A = mirrors[aIdx];
        if (!A.cloneMesh || !A.plane.valid) continue;
        for (int bIdx = 0; bIdx < baseCount; ++bIdx) {
            if (aIdx == bIdx) continue;
            Mirror &B = mirrors[bIdx];
            if (!B.wallMatClone || !B.plane.valid) continue;

            // 1) Find A's clone-of-B's-wall faces inside A.cloneMesh.
            std::vector<Face*> compoundWalls;
            for (DWord fi = 0; fi < A.cloneMesh->FIndex; ++fi) {
                Face &F = A.cloneMesh->Faces[fi];
                if (F.Txtr == B.wallMatClone) compoundWalls.push_back(&F);
            }
            if (compoundWalls.empty()) continue;
            if (s_nextMirrorId == 0) {
                std::fprintf(stderr, "[COMPOUND] mirror id space exhausted\n");
                return added;
            }
            const uint8_t compoundId = s_nextMirrorId++;

            // 2) Compound surface faces ride on the parent's stamp.
            //    Mekalele's per-pixel mask check passes anywhere the
            //    parent already covers, then Mekalele's z-test picks
            //    between this face, A's regular clones, and compound
            //    clones (also gated on parent). ownerMirrorId stays
            //    compoundId so post-commit pmid is correct for the
            //    deferred light filter.
            //
            //    Gating on compoundId instead would have blocked A's
            //    clones IN FRONT of the compound wall from rendering
            //    inside the wall's screen footprint — they'd be lost
            //    to the empty backdrop and the wall would visibly
            //    "occlude" geometry it shouldn't.
            for (Face *F : compoundWalls) {
                F->mirrorMaskTag = A.id;
                F->ownerMirrorId = compoundId;
            }

            // 3) Count clone capacity: every non-wall face in every
            //    non-clone scene mesh contributes one compound clone.
            DWord totalV = 0, totalF = 0;
            for (Object *Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
                if (Obj->Type != Obj_TriMesh) continue;
                if (isCloneMesh(Obj)) continue;
                TriMesh *T = (TriMesh*)Obj->Data;
                if (!T || !T->Verts || !T->Faces) continue;
                totalV += T->VIndex;
                for (DWord fi = 0; fi < T->FIndex; ++fi) {
                    if (!T->Faces[fi].A) continue;
                    if (isAnyBaseWallMat(T->Faces[fi].Txtr)) continue;
                    ++totalF;
                }
            }
            if (totalF == 0) continue;

            // 4) Allocate compound clone mesh + splines.
            TriMesh *MM = getAlignedType<TriMesh>(16);
            std::memset(MM, 0, sizeof(TriMesh));
            MM->Verts = (Vertex*)getAlignedBlock(sizeof(Vertex)*size_t(totalV), 16);
            MM->Faces = (Face*)getAlignedBlock(sizeof(Face)*size_t(totalF), 16);
            MM->VIndex = totalV;
            MM->FIndex = totalF;
            Matrix_Copy(MM->RotMat, Mat_ID);
            Matrix_Copy(MM->UnscaledRotMat, Mat_ID);
            MM->IPos   = {0.0f, 0.0f, 0.0f};
            MM->IScale = {1.0f, 1.0f, 1.0f};
            MM->IRot   = {0.0f, 0.0f, 0.0f, 1.0f};
            // Same Noshading rationale as the base-mirror clone mesh.
            MM->Flags |= HTrack_Visible | Tri_Noshading;
            // Field-wise (NOT brace-init): Pos/AA are Quaternion-layout
            // {W,x,y,z} — see the qkey comment in the first-order RTT
            // block for the degenerate-RotMat failure this causes.
            auto stampSingle = [](Spline &sp, float x, float y, float z, float w) {
                sp.NumKeys = 1; sp.CurKey = 0; sp.Flags = 0;
                sp.Keys = (SplineKey*)std::calloc(1, sizeof(SplineKey));
                sp.Keys[0].Frame = 0.0f;
                sp.Keys[0].Pos.x = x; sp.Keys[0].Pos.y = y;
                sp.Keys[0].Pos.z = z; sp.Keys[0].Pos.W = w;
                sp.Keys[0].AA.x  = x; sp.Keys[0].AA.y  = y;
                sp.Keys[0].AA.z  = z; sp.Keys[0].AA.W  = w;
            };
            stampSingle(MM->Pos,    0,0,0,0);
            stampSingle(MM->Scale,  1,1,1,0);
            stampSingle(MM->Rotate, 0,0,0,1);

            // 5) Compose the clone transform: reflect across B then A,
            //    so a world point P maps to reflect_A(reflect_B(P)) —
            //    this is the position where, when viewed back through
            //    mirror A, the user perceives the 2-bounce reflection
            //    at the geometrically-correct reflect_B(P) spot
            //    (matches [R2 m%d>m%d] label math in MirrorTestDriver).
            const Vector &An = A.plane.N; const float Ad = A.plane.d;
            const Vector &Bn = B.plane.N; const float Bd = B.plane.d;
            auto composedPoint = [&](const Vector &P) -> Vector {
                Vector via = reflectPointAcross(P, Bn, Bd);
                return reflectPointAcross(via, An, Ad);
            };
            auto composedDir = [&](const Vector &V) -> Vector {
                Vector via = reflectDirAcross(V, Bn);
                return reflectDirAcross(via, An);
            };

            // 6) Fill compound clone verts + faces. Winding is
            //    UNCHANGED — each single reflection swaps, so two
            //    reflections leave it as-is. Face normals do go
            //    through the composed dir transform.
            Vector bbMin = { 1e30f, 1e30f, 1e30f};
            Vector bbMax = {-1e30f,-1e30f,-1e30f};
            DWord vOfs = 0, fOfs = 0;
            for (Object *Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
                if (Obj->Type != Obj_TriMesh) continue;
                if (isCloneMesh(Obj)) continue;
                TriMesh *T = (TriMesh*)Obj->Data;
                if (!T || !T->Verts || !T->Faces) continue;
                const DWord vStart = vOfs;
                for (DWord vi = 0; vi < T->VIndex; ++vi) {
                    MM->Verts[vOfs] = T->Verts[vi];
                    Vector localP = T->Verts[vi].Pos;
                    Vector worldP;
                    MatrixXVector(T->RotMat, &localP, &worldP);
                    worldP.x += T->IPos.x; worldP.y += T->IPos.y; worldP.z += T->IPos.z;
                    const Vector mP = composedPoint(worldP);
                    MM->Verts[vOfs].Pos = mP;
                    Vector localN = T->Verts[vi].N;
                    Vector worldN;
                    MatrixXVector(T->RotMat, &localN, &worldN);
                    MM->Verts[vOfs].N = composedDir(worldN);
                    bbMin.x = std::min(bbMin.x, mP.x);
                    bbMin.y = std::min(bbMin.y, mP.y);
                    bbMin.z = std::min(bbMin.z, mP.z);
                    bbMax.x = std::max(bbMax.x, mP.x);
                    bbMax.y = std::max(bbMax.y, mP.y);
                    bbMax.z = std::max(bbMax.z, mP.z);
                    ++vOfs;
                }
                for (DWord fi = 0; fi < T->FIndex; ++fi) {
                    Face &OF = T->Faces[fi];
                    if (!OF.A || !OF.B || !OF.C) continue;
                    if (isAnyBaseWallMat(OF.Txtr)) continue;
                    Face &CF = MM->Faces[fOfs];
                    CF = OF;
                    // Winding unchanged: two reflections preserve it.
                    CF.A = MM->Verts + vStart + (OF.A - T->Verts);
                    CF.B = MM->Verts + vStart + (OF.B - T->Verts);
                    CF.C = MM->Verts + vStart + (OF.C - T->Verts);
                    CF.N = composedDir(OF.N);
                    CF.NormProd = -(CF.N.x*CF.A->Pos.x +
                                    CF.N.y*CF.A->Pos.y +
                                    CF.N.z*CF.A->Pos.z);
                    // Same gating rationale as the compound surface
                    // faces above: ride on the parent's stamp so the
                    // z-test decides between this compound clone, A's
                    // regular clones, and the compound wall surface
                    // per pixel. ownerMirrorId keeps compoundId so the
                    // deferred filter sees compound-omni lighting at
                    // pixels this clone wins.
                    CF.mirrorMaskTag = A.id;
                    CF.ownerMirrorId = compoundId;
                    ++fOfs;
                }
            }

            // Loose bsphere from bbox.
            Vector ctr = {(bbMin.x+bbMax.x)*0.5f,
                          (bbMin.y+bbMax.y)*0.5f,
                          (bbMin.z+bbMax.z)*0.5f};
            const float dx = bbMax.x-bbMin.x, dy = bbMax.y-bbMin.y, dz = bbMax.z-bbMin.z;
            const float radSq = 0.25f*(dx*dx + dy*dy + dz*dz);
            MM->BSphereCtr = ctr;
            MM->BSphereRad = radSq;
            MM->BSphereRadius = std::sqrt(radSq);
            MM->BSphereScreenPos = {0,0,0};
            Compute_FaceVertexIndices(MM);

            // 7) Link the compound mesh into the scene with a clone-
            //    prefix name so subsequent BuildMirror calls (if any)
            //    won't recursively clone it.
            Object *MObj = getAlignedType<Object>(16);
            std::memset(MObj, 0, sizeof(Object));
            MObj->Type = Obj_TriMesh;
            MObj->Data = MM;
            MObj->Pos  = &MM->IPos;
            MObj->Rot  = &MM->RotMat;
            {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "__mirrorClone_compound_%u_%u",
                              unsigned(A.id), unsigned(B.id));
                MObj->Name = (char*)std::malloc(std::strlen(buf)+1);
                std::strcpy(MObj->Name, buf);
            }
            MObj->Next = sc->ObjectHead;
            if (sc->ObjectHead) sc->ObjectHead->Prev = MObj;
            sc->ObjectHead = MObj;
            MM->Next = sc->TriMeshHead;
            if (sc->TriMeshHead) sc->TriMeshHead->Prev = MM;
            sc->TriMeshHead = MM;

            // 8) Clone omnis across composed transform; tag with id.
            int omniCount = 0;
            for (Omni *srcO = sc->OmniHead; srcO; srcO = srcO->Next) {
                if (srcO->Flags & Omni_MirrorClone) continue;  // skip prior clones
                Omni *clone = (Omni*)getAlignedBlock(sizeof(Omni), 16);
                std::memcpy(clone, srcO, sizeof(Omni));
                clone->IPos = composedPoint(srcO->IPos);
                clone->IDir = composedDir(srcO->IDir);
                clone->Flags |= Omni_MirrorClone;
                // Same flare-face repoint + footprint tag as the base-
                // mirror clone loop. Compounds gate on the PARENT's
                // stamp (they don't stamp their own id).
                clone->F.A = clone->F.B = clone->F.C = &clone->V;
                clone->F.mirrorMaskTag = A.id;
                clone->mirrorId = compoundId;
                // No intensity attenuation: the per-pixel filter
                // routes only this compound's omnis to compoundId
                // pixels, so the once-physical light gets one full
                // contribution (the actual world light energy doesn't
                // halve at every bounce in a real mirror system — only
                // the visible reflectance does, which is handled by
                // the wall's silver tint blend).
                clone->Prev = nullptr;
                clone->Next = sc->OmniHead;
                if (sc->OmniHead) sc->OmniHead->Prev = clone;
                sc->OmniHead = clone;
                ++omniCount;
            }

            // 9) Build the Mirror struct entry.
            Mirror compound{};
            compound.id = compoundId;
            compound.parentMirrorId = A.id;
            compound.parentPlane = A.plane;
            // The compound's effective inner plane is B's plane, so
            // viewer-side tests downstream that care about the inner
            // reflection (UpdateMirror's omni range clamp, future
            // recursion gates) read this. StampMirrorMasks's viewer
            // gate uses parentPlane so the compound only renders
            // when the camera is on A's front side.
            compound.plane = B.plane;
            compound.cloneMesh = MM;
            compound.wallFaces = std::move(compoundWalls);
            // Compound wall faces all live in A's clone mesh.
            compound.wallFaceMeshes.assign(compound.wallFaces.size(),
                                           A.cloneMesh);
            compound.clonedVerts = int(vOfs);
            compound.clonedFaces = int(fOfs);
            {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "compound_%u_%u",
                              unsigned(A.id), unsigned(B.id));
                compound.wallMaterialName = buf;
            }
            std::fprintf(stderr,
                "[COMPOUND m%u (parent m%u, inner m%u)] %d wall faces, "
                "%u/%u clone verts/faces, %d omnis\n",
                unsigned(compoundId), unsigned(A.id), unsigned(B.id),
                int(compound.wallFaces.size()),
                unsigned(vOfs), unsigned(fOfs), omniCount);
            mirrors.push_back(std::move(compound));
            ++added;
        }
    }
    return added;
}

void UpdateMirror(Scene *sc, Mirror &m)
{
    if (!sc || !m.plane.valid || !m.cloneMesh) return;
    const Vector &N = m.plane.N;
    const float   d = m.plane.d;

    // Per-mesh: re-mirror source's CURRENT world verts into the clone.
    // Dynamic meshes (Hull / legs) end up with up-to-date reflections.
    // After the first full pass (primed — needed because the build-time
    // capture may predate the first Animate_Objects) only DYNAMIC
    // ranges/faces update: static geometry recomputes identical values.
    const bool full = !m.primed;
    m.primed = true;
    for (const auto &r : m.meshRanges) {
        if (!full && !r.dynamic) continue;
        TriMesh *T = r.sourceMesh;
        if (!T || !T->Verts) continue;
        const DWord n = std::min<DWord>(r.vCount, T->VIndex);
        for (DWord vi = 0; vi < n; ++vi) {
            Vector localP = T->Verts[vi].Pos;
            Vector worldP;
            MatrixXVector(T->RotMat, &localP, &worldP);
            worldP.x += T->IPos.x; worldP.y += T->IPos.y; worldP.z += T->IPos.z;
            m.cloneMesh->Verts[r.vStart + vi].Pos = reflectPointAcross(worldP, N, d);
            // Directions: full composed RotMat then reflect — N and
            // Tangent both, every frame, so the robot's animated TBN
            // stays correct in the mirror. (See the init-fill comment
            // for why NOT UnscaledRotMat, and for the B = N × T
            // handedness caveat.)
            Vector localN = T->Verts[vi].N;
            Vector worldN;
            MatrixXVector(T->RotMat, &localN, &worldN);
            m.cloneMesh->Verts[r.vStart + vi].N = reflectDirAcross(worldN, N);
            Vector localT = T->Verts[vi].Tangent;
            Vector worldT;
            MatrixXVector(T->RotMat, &localT, &worldT);
            m.cloneMesh->Verts[r.vStart + vi].Tangent = reflectDirAcross(worldT, N);
        }
    }
    // Per-face: re-derive each clone face's normal from the source's
    // CURRENT world normal (srcMesh->RotMat moves every frame for the
    // robot), reflect it, and recompute NormProd against the already-
    // re-mirrored clone A vertex. Static meshes recompute to the same
    // value; the cost (~9k faces × a 3×3 matmul) is negligible.
    {
        const size_t nf = std::min(m.cloneFaceSrc.size(),
                                   size_t(m.cloneMesh->FIndex));
        for (size_t fi = 0; fi < nf; ++fi) {
            if (!full && !m.cloneFaceSrc[fi].dynamic) continue;
            const Face *src = m.cloneFaceSrc[fi].face;
            TriMesh    *sT  = m.cloneFaceSrc[fi].mesh;
            if (!src || !sT) continue;
            Face &CF = m.cloneMesh->Faces[fi];
            Vector ln = src->N, wn;
            MatrixXVector(sT->RotMat, &ln, &wn);
            CF.N = reflectDirAcross(wn, N);
            CF.NormProd = -(CF.N.x * CF.A->Pos.x +
                            CF.N.y * CF.A->Pos.y +
                            CF.N.z * CF.A->Pos.z);
        }
    }
    // Per-omni: re-mirror IPos / IDir, restore both source and clone
    // to their FULL original ranges every frame.
    //
    // We used to clamp each omni's IRange down to its distance-to-the-
    // mirror-plane ("soft compartmentalization", so a light near the
    // mirror couldn't bleed to the wrong side). That predated the
    // per-pixel mirror filter (Omni::mirrorId vs gb.mirrorId): the
    // filter now guarantees clone omnis light only clone pixels and
    // source omnis light only source pixels, so the range clamp is
    // redundant — and actively harmful, because it clamped the SHARED
    // SOURCE omni. With two mirrors the source omni got clamped to the
    // NEARER plane's distance (~5 units in the test scene), so the real
    // floor beyond that went ambient-only while the reflected floor —
    // lit by the clone omni — stayed bright. That asymmetry read as a
    // phantom "specular highlight that's only in the mirror."
    for (auto &c : m.omniClones) {
        if (!c.sourceOmni || !c.mirrorOmni) continue;
        c.mirrorOmni->IPos = reflectPointAcross(c.sourceOmni->IPos, N, d);
        c.mirrorOmni->IDir = reflectDirAcross(c.sourceOmni->IDir, N);
        c.sourceOmni->IRange = c.origSourceRange;
        c.mirrorOmni->IRange = c.origMirrorRange;
        c.sourceOmni->rRange = c.sourceOmni->IRange > 0.0f ? 1.0f / c.sourceOmni->IRange : 0.0f;
        c.mirrorOmni->rRange = c.mirrorOmni->IRange > 0.0f ? 1.0f / c.mirrorOmni->IRange : 0.0f;
    }
}

namespace {

// Can mirror m contribute any pixel this frame? Two rejects:
//   1. Camera on the back side of every wall face (a mirror shows
//      nothing from behind).
//   2. Every wall vert ahead of the near plane projects outside the
//      margin-expanded viewport (panel off screen), or every vert is
//      behind the near plane (panel behind the camera).
// Runs on the PREVIOUS frame's camera (UpdateAllMirrors precedes
// Transform_Objects, which is what advances ::View) — the 15% viewport
// margin absorbs one frame of camera motion. Conservative everywhere:
// no camera yet → visible; panel straddling the near plane → visible.
bool mirrorPotentiallyVisible(Scene *sc, const Mirror &m)
{
    const Camera *cam = ::View ? ::View : (sc ? sc->CameraHead : nullptr);
    if (!cam) return true;
    bool anyFront = false;
    for (const Face *F : m.wallFaces) {
        if (!F || !F->A) continue;
        const float dot = F->N.x * cam->ISource.x
                        + F->N.y * cam->ISource.y
                        + F->N.z * cam->ISource.z + F->NormProd;
        if (dot > 0.0f) { anyFront = true; break; }
    }
    if (!anyFront) return false;
    const float (*VM)[3] = cam->Mat;
    const Vector C = cam->ISource;
    const float w = float(::XRes), h = float(::YRes);
    const float marginX = w * 0.15f, marginY = h * 0.15f;
    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
    int total = 0, behind = 0;
    for (size_t i = 0; i < m.wallFaces.size(); ++i) {
        const Face *F = m.wallFaces[i];
        TriMesh *WT = i < m.wallFaceMeshes.size() ? m.wallFaceMeshes[i] : nullptr;
        if (!F || !F->A || !F->B || !F->C) continue;
        const Vertex *vs[3] = { F->A, F->B, F->C };
        for (int k = 0; k < 3; ++k) {
            Vector wp = vs[k]->Pos;
            if (WT) {
                Vector lp = wp;
                MatrixXVector(WT->RotMat, &lp, &wp);
                wp += WT->IPos;
            }
            const float dx = wp.x - C.x, dy = wp.y - C.y, dz = wp.z - C.z;
            const float vz = VM[2][0]*dx + VM[2][1]*dy + VM[2][2]*dz;
            ++total;
            if (vz <= 0.05f) { ++behind; continue; }
            const float vx = VM[0][0]*dx + VM[0][1]*dy + VM[0][2]*dz;
            const float vy = VM[1][0]*dx + VM[1][1]*dy + VM[1][2]*dz;
            const float sx =  FOVX * vx / vz + CntrEX;
            const float sy = -FOVY * vy / vz + CntrEY;
            bx0 = std::min(bx0, sx); bx1 = std::max(bx1, sx);
            by0 = std::min(by0, sy); by1 = std::max(by1, sy);
        }
    }
    if (total == 0) return true;          // no usable verts — stay safe
    if (behind == total) return false;    // whole panel behind camera
    if (behind > 0) return true;          // straddles near plane
    return bx1 >= -marginX && bx0 <= w + marginX
        && by1 >= -marginY && by0 <= h + marginY;
}

}  // namespace

void UpdateAllMirrors(Scene *sc, std::vector<Mirror> &mirrors)
{
    int nActive = 0;
    {
    ScopedMirrorMs _t(&g_mirrorProf.updMs);
    for (auto &m : mirrors) {
        bool act;
        if (m.parentMirrorId != 0) {
            // Compounds ride their parent's activity — they render
            // inside the parent's footprint only.
            act = false;
            for (const auto &p : mirrors) {
                if (p.id == m.parentMirrorId) { act = p.active; break; }
            }
        } else {
            act = m.plane.valid && m.cloneMesh
               && mirrorPotentiallyVisible(sc, m);
        }
        m.active = act;
        if (m.cloneMesh) {
            // Hidden clone meshes skip Transform_Objects entirely
            // (mesh-level HTrack_Visible check) — that's the bulk of
            // an off-screen mirror's per-frame cost.
            if (act) m.cloneMesh->Flags |=  HTrack_Visible;
            else     m.cloneMesh->Flags &= ~HTrack_Visible;
        }
        // Clone flares: The_MMX_Scalar early-outs on Size <= 0, and
        // Transform re-stamps F.FlareSize from ISize every frame — so
        // ISize is the kill switch. Restored from the source omni when
        // the mirror reactivates.
        for (auto &c : m.omniClones) {
            if (c.mirrorOmni && c.sourceOmni)
                c.mirrorOmni->ISize = act ? c.sourceOmni->ISize : 0.0f;
        }
        if (act) { UpdateMirror(sc, m); ++nActive; }
    }
    }  // ScopedMirrorMs — accumulate before the print below reads it

    if (fds::FeatureFlags::mirror_prof()) {
        auto &P = g_mirrorProf;
        P.activeSum += nActive;
        ++P.frames;
        static int sEvery = -1;
        if (sEvery < 0) {
            const char *e = std::getenv("FDS_MIRROR_PROF_EVERY");
            sEvery = e ? std::max(1, std::atoi(e)) : 120;
        }
        if (P.frames >= sEvery) {
            std::fprintf(stderr,
                "[MIRROR-PROF] %df avg: update=%.3fms stamp=%.3fms "
                "rtt=%.3fms | active=%.1f rttJobs=%.2f\n",
                P.frames,
                P.updMs / P.frames, P.stampMs / P.frames,
                P.rttMs / P.frames,
                double(P.activeSum) / P.frames,
                double(P.rttJobsSum) / P.frames);
            P = {};
        }
    }
}

void TagFacesBehindMirrors(Scene *sc, const std::vector<Mirror> &mirrors)
{
    if (!sc) return;
    // Epsilon in world units: faces coplanar with a mirror (its own wall,
    // the frame strips) must NOT be tagged as "behind." A small negative
    // threshold means a vertex has to be clearly on the back side.
    constexpr float kBehindEps = 1e-3f;
    for (Object *Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh) continue;
        if (isCloneMesh(Obj)) continue;          // clones gate via mirrorTag
        TriMesh *T = (TriMesh*)Obj->Data;
        if (!T || !T->Verts || !T->Faces) continue;
        for (DWord fi = 0; fi < T->FIndex; ++fi) {
            Face &F = T->Faces[fi];
            if (!F.A || !F.B || !F.C) continue;
            // World-space positions of the three verts.
            const Vertex *vs[3] = { F.A, F.B, F.C };
            Vector wp[3];
            for (int k = 0; k < 3; ++k) {
                Vector lp = vs[k]->Pos;
                MatrixXVector(T->RotMat, &lp, &wp[k]);
                wp[k].x += T->IPos.x; wp[k].y += T->IPos.y; wp[k].z += T->IPos.z;
            }
            uint32_t mask = 0;
            for (const Mirror &m : mirrors) {
                if (m.id == 0 || m.id > 31 || !m.plane.valid) continue;
                // Skip the mirror's own wall faces — they're coplanar and
                // would self-tag under jitter. wallFaces holds pointers
                // into the live mesh, so an identity check is exact.
                bool isOwnWall = false;
                for (const Face *WF : m.wallFaces) {
                    if (WF == &F) { isOwnWall = true; break; }
                }
                if (isOwnWall) continue;
                const Vector &N = m.plane.N;
                const float   d = m.plane.d;
                // Behind = any vertex on the back side (N·P + d < -eps).
                for (int k = 0; k < 3; ++k) {
                    const float sd = N.x*wp[k].x + N.y*wp[k].y + N.z*wp[k].z + d;
                    if (sd < -kBehindEps) { mask |= (1u << m.id); break; }
                }
            }
            F.behindMirrorMask = mask;
        }
    }
}

namespace {

using u8 = uint8_t;

// Minimal 2D scanline triangle fill, writes a single u8 value per
// covered pixel. Used for the per-frame mask pre-pass — we don't need
// Z, perspective interpolation, edge subpixel precision, anything
// else; just "where does this triangle cover in screen space."
//
// Coordinates expected in PX/PY pixel-space (what Transform_Objects
// already produced). Clipped to [0, w) × [0, h).
// requireExisting == 0xFF means "unconditional write" (the base-mirror
// path). For compound mirrors we pass requireExisting = parent's id so
// the compound stamp only overrides pixels the parent already claimed
// — without this clip, the compound wall (which is a CLONE face that
// projects to wherever its world-space mirror-of-mirror position lands)
// bleeds outside the parent's actual screen footprint and we end up
// with silver tint over real-world geometry.
inline void StampTri2D(u8 *plane, uint16_t *zplane, int w, int h,
                       float ax, float ay, float arz,
                       float bx, float by, float brz,
                       float cx, float cy, float crz,
                       u8 value, float zscale,
                       u8 requireExisting = 0xFF)
{
    // Sort vertices by Y so we can split the triangle into a flat-top
    // and flat-bottom half and scan each between two edges per row.
    // rz = 1/z rides along — linear interpolation of 1/z in screen
    // space is exact for a planar triangle, so the per-pixel wall
    // depth written to zplane matches what Mekalele would rasterize.
    if (ay > by) { std::swap(ax, bx); std::swap(ay, by); std::swap(arz, brz); }
    if (by > cy) { std::swap(bx, cx); std::swap(by, cy); std::swap(brz, crz); }
    if (ay > by) { std::swap(ax, bx); std::swap(ay, by); std::swap(arz, brz); }
    const int yTop = std::max(0, int(std::ceil(ay)));
    const int yMid = std::clamp(int(std::ceil(by)), 0, h);
    const int yBot = std::min(h, int(std::ceil(cy)));
    if (yTop >= yBot) return;
    // Edge slopes (d? / dy) for the three edges. Guard against 1-pixel
    // tall triangles to avoid divide-by-zero.
    auto slope = [](float v0, float y0, float v1, float y1) -> float {
        const float dy = y1 - y0;
        return dy > 1e-6f ? (v1 - v0) / dy : 0.0f;
    };
    const float dxLong   = slope(ax, ay, cx, cy);
    const float dxUpper  = slope(ax, ay, bx, by);
    const float dxLower  = slope(bx, by, cx, cy);
    const float drzLong  = slope(arz, ay, crz, cy);
    const float drzUpper = slope(arz, ay, brz, by);
    const float drzLower = slope(brz, by, crz, cy);
    // Upper half: edges (A→C, A→B). Lower half: edges (A→C, B→C).
    for (int y = yTop; y < yBot; ++y) {
        const float yf = float(y);
        const float xLong  = ax  + dxLong  * (yf - ay);
        const float rzLong = arz + drzLong * (yf - ay);
        float xOther, rzOther;
        if (y < yMid) {
            xOther  = ax  + dxUpper  * (yf - ay);
            rzOther = arz + drzUpper * (yf - ay);
        } else {
            xOther  = bx  + dxLower  * (yf - by);
            rzOther = brz + drzLower * (yf - by);
        }
        float xLf = xLong, rzLf = rzLong, xRf = xOther, rzRf = rzOther;
        if (xLf > xRf) { std::swap(xLf, xRf); std::swap(rzLf, rzRf); }
        int xL = int(std::ceil(xLf));
        int xR = int(std::ceil(xRf));
        if (xL < 0) xL = 0;
        if (xR > w) xR = w;
        if (xL >= xR) continue;
        const float spanW = xRf - xLf;
        const float drzdx = spanW > 1e-6f ? (rzRf - rzLf) / spanW : 0.0f;
        u8       *row  = plane  + size_t(y) * size_t(w);
        uint16_t *zrow = zplane + size_t(y) * size_t(w);
        for (int x = xL; x < xR; ++x) {
            // Clip pass (requireExisting != 0xFF): only override pixels
            // the parent already stamped. Compound mirrors use this so
            // their wall mask can't bleed past the parent's footprint.
            if (requireExisting != 0xFF && row[x] != requireExisting)
                continue;
            row[x] = value;
            const float rz = rzLf + drzdx * (float(x) - xLf);
            const float z  = rz > 1e-9f ? 1.0f / rz : 1e9f;
            float encF = float(0xFF80) - z * zscale;
            if (encF < 1.0f)       encF = 1.0f;
            if (encF > 65407.0f)   encF = 65407.0f;  // 0xFF7F
            zrow[x] = uint16_t(encF);
        }
    }
}

}  // namespace

int PrepareSecondOrderMirrorRtt(Scene *sc, std::vector<Mirror> &mirrors,
                                std::vector<MirrorRttSlot> &out)
{
    if (!sc || !fds::FeatureFlags::mirror_rtt()) return 0;
    int created = 0;
    std::vector<uint32_t> black(size_t(kRttRes) * size_t(kRttRes), 0xFF000000u);
    for (Mirror &A : mirrors) {
        if (A.id == 0 || A.parentMirrorId != 0 || !A.cloneMesh) continue;
        if (A.cloneFaceSrc.empty()) continue;
        for (const Mirror &B : mirrors) {
            if (&B == &A || B.id == 0 || B.parentMirrorId != 0) continue;
            if (B.wallFaces.empty() || !B.plane.valid) continue;
            Vector axU, axV;
            planeBasis(B.plane.N, axU, axV);
            // Gather A's clone faces of B's wall panels, with each
            // face's SOURCE (u,v) bbox for the component split.
            struct PFace {
                Face       *cf;       // face in A's clone mesh
                const Face *src;      // B wall face it mirrors
                TriMesh    *srcMesh;
                float       u0, u1, v0, v1;
                int         comp = -1;
            };
            std::vector<PFace> pf;
            for (size_t fi = 0; fi < A.cloneFaceSrc.size()
                              && fi < size_t(A.cloneMesh->FIndex); ++fi) {
                const Face *src = A.cloneFaceSrc[fi].face;
                bool isBWall = false;
                size_t wi = 0;
                for (; wi < B.wallFaces.size(); ++wi) {
                    if (B.wallFaces[wi] == src) { isBWall = true; break; }
                }
                if (!isBWall) continue;
                TriMesh *WT = wi < B.wallFaceMeshes.size()
                    ? B.wallFaceMeshes[wi] : nullptr;
                PFace p{};
                p.cf = &A.cloneMesh->Faces[fi];
                p.src = src;
                p.srcMesh = WT;
                p.u0 = p.v0 = 1e30f; p.u1 = p.v1 = -1e30f;
                const Vertex *vs[3] = { src->A, src->B, src->C };
                for (int k = 0; k < 3; ++k) {
                    Vector wp = vs[k]->Pos;
                    if (WT) {
                        Vector lp = wp;
                        MatrixXVector(WT->RotMat, &lp, &wp);
                        wp += WT->IPos;
                    }
                    const float pu = wp.x*axU.x + wp.y*axU.y + wp.z*axU.z;
                    const float pv = wp.x*axV.x + wp.y*axV.y + wp.z*axV.z;
                    p.u0 = std::min(p.u0, pu); p.u1 = std::max(p.u1, pu);
                    p.v0 = std::min(p.v0, pv); p.v1 = std::max(p.v1, pv);
                }
                pf.push_back(p);
            }
            if (pf.empty()) continue;
            // Connected-component split by (u,v)-bbox proximity: one
            // coplanar cluster can span several separate boxes, and an
            // RTT window across all of them would waste most texels.
            constexpr float kJoinEps = 1.0f;
            int numComps = 0;
            for (size_t i = 0; i < pf.size(); ++i) {
                if (pf[i].comp >= 0) continue;
                pf[i].comp = numComps++;
                bool grew = true;
                while (grew) {
                    grew = false;
                    for (size_t j = 0; j < pf.size(); ++j) {
                        if (pf[j].comp >= 0) continue;
                        for (size_t k = 0; k < pf.size(); ++k) {
                            if (pf[k].comp != pf[i].comp) continue;
                            const bool overlap =
                                pf[j].u0 <= pf[k].u1 + kJoinEps &&
                                pf[j].u1 >= pf[k].u0 - kJoinEps &&
                                pf[j].v0 <= pf[k].v1 + kJoinEps &&
                                pf[j].v1 >= pf[k].v0 - kJoinEps;
                            if (overlap) {
                                pf[j].comp = pf[i].comp;
                                grew = true;
                                break;
                            }
                        }
                    }
                }
            }
            for (int c = 0; c < numComps; ++c) {
                MirrorRttSlot slot{};
                slot.aId = A.id;
                slot.bId = B.id;
                slot.bN  = B.plane.N;
                slot.bD  = B.plane.d;
                slot.axisU = axU;
                slot.axisV = axV;
                slot.u0 = slot.v0 = 1e30f; slot.u1 = slot.v1 = -1e30f;
                for (const PFace &p : pf) {
                    if (p.comp != c) continue;
                    slot.u0 = std::min(slot.u0, p.u0);
                    slot.u1 = std::max(slot.u1, p.u1);
                    slot.v0 = std::min(slot.v0, p.v0);
                    slot.v1 = std::max(slot.v1, p.v1);
                }
                if (slot.u1 - slot.u0 < 1e-3f || slot.v1 - slot.v0 < 1e-3f)
                    continue;
                // Aspect-match the texture to the window at constant
                // texel budget: halve one axis / double the other while
                // the aspect mismatch exceeds 2x. Greets's wide end
                // screens (≈3.7:1) land on 512x128 instead of smearing
                // 4x horizontally out of a square.
                {
                    const float winAspect =
                        (slot.u1 - slot.u0) / (slot.v1 - slot.v0);
                    while (winAspect / (float(slot.texW) / float(slot.texH)) >= 2.0f
                           && slot.texW < 1024 && slot.texH > 32) {
                        slot.texW <<= 1; slot.texH >>= 1;
                    }
                    while ((float(slot.texW) / float(slot.texH)) / winAspect >= 2.0f
                           && slot.texH < 1024 && slot.texW > 32) {
                        slot.texW >>= 1; slot.texH <<= 1;
                    }
                }
                slot.mat = Materialize(black.data(), slot.texW, slot.texH);
                // The texture holds FINAL shaded colors — display it
                // unlit: Lum 1.0 saturates the kernel's light factor at
                // ~255 so lit ≈ texel; Diffuse/Specular 0 keep omnis
                // out of it entirely.
                slot.mat->Luminosity = 1.0f;
                slot.mat->Diffuse    = 0.0f;
                slot.mat->Specular   = 0.0f;
                slot.mat->RelScene   = sc;
                {
                    char nm[64];
                    std::snprintf(nm, sizeof(nm), "__mirrorRtt_%u_%u_%d",
                                  unsigned(A.id), unsigned(B.id), c);
                    slot.mat->Name = strdup(nm);
                }
                // MatLib tail-append + (one) table rebuild below — the
                // deferred kernels resolve by table matID (see the
                // wallMatClone lesson).
                slot.mat->Next = nullptr;
                if (!MatLib) {
                    slot.mat->Prev = nullptr;
                    MatLib = slot.mat;
                } else {
                    Material *tail = MatLib;
                    while (tail->Next) tail = tail->Next;
                    tail->Next = slot.mat;
                    slot.mat->Prev = tail;
                }
                for (const PFace &p : pf) {
                    if (p.comp != c) continue;
                    p.cf->Txtr = slot.mat;
                    slot.faces.push_back(p.cf);
                    // Record each clone vert with its (static) source
                    // panel-plane coordinates; the per-frame render
                    // stamps the actual UVs (the window's sub-rect in
                    // the symmetric RTT view moves with the camera).
                    // Clone winding swapped B↔C at build, so clone A↔
                    // src A, clone B↔src C, clone C↔src B.
                    auto record = [&](Vertex *dst, const Vertex *srcV) {
                        for (const auto &sv : slot.verts)
                            if (sv.v == dst) return;  // dedupe shared verts
                        Vector wp = srcV->Pos;
                        if (p.srcMesh) {
                            Vector lp = wp;
                            MatrixXVector(p.srcMesh->RotMat, &lp, &wp);
                            wp += p.srcMesh->IPos;
                        }
                        slot.verts.push_back({ dst,
                            wp.x*axU.x + wp.y*axU.y + wp.z*axU.z,
                            wp.x*axV.x + wp.y*axV.y + wp.z*axV.z });
                    };
                    record(p.cf->A, p.src->A);
                    record(p.cf->B, p.src->C);
                    record(p.cf->C, p.src->B);
                }
                out.push_back(std::move(slot));
                ++created;
            }
        }
    }
    if (created > 0) {
        Scene_RebuildMatTable(sc);
        for (const MirrorRttSlot &s : out) {
            std::fprintf(stderr,
                "[MIRROR-RTT] slot m%u->m%u %dx%d (window %.1fx%.1f "
                "u=[%.1f..%.1f] v=[%.1f..%.1f] N=(%.1f,%.1f,%.1f) d=%.2f)\n",
                unsigned(s.aId), unsigned(s.bId), s.texW, s.texH,
                s.u1 - s.u0, s.v1 - s.v0, s.u0, s.u1, s.v0, s.v1,
                s.bN.x, s.bN.y, s.bN.z, s.bD);
        }
    }
    std::fprintf(stderr, "[MIRROR-RTT] prepared %d slot(s)\n", created);
    return created;
}

void RenderSecondOrderMirrors(Scene *sc, std::vector<Mirror> &mirrors,
                              std::vector<MirrorRttSlot> &slots)
{
    ScopedMirrorMs _t(&g_mirrorProf.rttMs);
    if (!sc || slots.empty()) return;
    const Camera *mainCam = ::View ? ::View : sc->CameraHead;
    if (!mainCam) return;
    // Age every slot once per pass — the scheduler trades footprint
    // area against staleness so the 2-jobs/frame cap round-robins
    // across visible slots instead of starving the small ones.
    for (MirrorRttSlot &s : slots)
        if (s.staleFrames < (1 << 20)) ++s.staleFrames;

    // ── Pick the most visible slots ─────────────────────────────────
    const float (*VM)[3] = mainCam->Mat;
    const Vector C = mainCam->ISource;
    struct Job {
        MirrorRttSlot *slot;
        float          area;   // projected px² (visibility ranking)
        Vector         camPos; // C_B
        float          dist;   // C_B distance to B's plane
        bool           backSide = false;  // order-1: camera behind plane
    };
    std::vector<Job> jobs;
    auto projectToScreen = [&](const Vector &wp, float &sx, float &sy) -> bool {
        const float dx = wp.x - C.x, dy = wp.y - C.y, dz = wp.z - C.z;
        const float vz = VM[2][0]*dx + VM[2][1]*dy + VM[2][2]*dz;
        if (vz <= 0.05f) return false;
        const float vx = VM[0][0]*dx + VM[0][1]*dy + VM[0][2]*dz;
        const float vy = VM[1][0]*dx + VM[1][1]*dy + VM[1][2]*dz;
        sx =  FOVX * vx / vz + CntrEX;
        sy = -FOVY * vy / vz + CntrEY;
        return true;
    };
    for (MirrorRttSlot &s : slots) {
        if (s.order == 2) {
            const Mirror *A = nullptr;
            for (const Mirror &m : mirrors) if (m.id == s.aId) { A = &m; break; }
            if (!A || !A->active) continue;
        }
        float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
        bool anyAhead = false;
        if (s.order == 1) {
            // First order: project the panel WINDOW corners — the
            // slot's faces are real mesh faces (mesh-local verts), but
            // the window in plane-basis coordinates reconstructs the
            // same world rectangle: P = u·axisU + v·axisV − d·N.
            for (int ci = 0; ci < 4; ++ci) {
                const float uu = (ci & 1) ? s.u1 : s.u0;
                const float vv = (ci & 2) ? s.v1 : s.v0;
                const Vector wp = {
                    uu*s.axisU.x + vv*s.axisV.x - s.bD*s.bN.x,
                    uu*s.axisU.y + vv*s.axisV.y - s.bD*s.bN.y,
                    uu*s.axisU.z + vv*s.axisV.z - s.bD*s.bN.z };
                float sx, sy;
                if (!projectToScreen(wp, sx, sy)) continue;
                bx0 = std::min(bx0, sx); bx1 = std::max(bx1, sx);
                by0 = std::min(by0, sy); by1 = std::max(by1, sy);
                anyAhead = true;
            }
        } else {
            for (const Face *F : s.faces) {
                if (!F || !F->A || !F->B || !F->C) continue;
                const Vertex *vs[3] = { F->A, F->B, F->C };
                for (int k = 0; k < 3; ++k) {
                    // Clone verts: world-baked positions.
                    float sx, sy;
                    if (!projectToScreen(vs[k]->Pos, sx, sy)) continue;
                    bx0 = std::min(bx0, sx); bx1 = std::max(bx1, sx);
                    by0 = std::min(by0, sy); by1 = std::max(by1, sy);
                    anyAhead = true;
                }
            }
        }
        if (!anyAhead) continue;
        bx0 = std::max(bx0, 0.0f); by0 = std::max(by0, 0.0f);
        bx1 = std::min(bx1, float(::XRes)); by1 = std::min(by1, float(::YRes));
        const float area = (bx1 - bx0) * (by1 - by0);
        if (area <= 1.0f) continue;
        // Virtual camera: order 1 reflects ONCE across the panel's own
        // plane; order 2 reflects through A then B. Either way it must
        // land BEHIND the panel plane (in front = camera on the back
        // side of the panel, which can't show this mirror anyway).
        Vector cb;
        bool backSide = false;
        if (s.order == 1) {
            // Side-aware: the glass reflects on whichever side the
            // camera is on. Back side → reflect across the flipped
            // plane; the render below also negates the U axis so the
            // texture, displayed through the SAME UVs but viewed from
            // behind (left-right mirrored by projection), reads
            // correctly.
            const float sdC = s.bN.x*C.x + s.bN.y*C.y + s.bN.z*C.z + s.bD;
            backSide = (sdC < 0.0f);
            if (backSide)
                cb = reflectPointAcross(C, { -s.bN.x, -s.bN.y, -s.bN.z }, -s.bD);
            else
                cb = reflectPointAcross(C, s.bN, s.bD);
        } else {
            const Mirror *A = nullptr;
            for (const Mirror &m : mirrors) if (m.id == s.aId) { A = &m; break; }
            const Vector ca = reflectPointAcross(C, A->plane.N, A->plane.d);
            cb = reflectPointAcross(ca, s.bN, s.bD);
        }
        float sd = s.bN.x*cb.x + s.bN.y*cb.y + s.bN.z*cb.z + s.bD;
        if (backSide) sd = -sd;      // flipped plane for back-side jobs
        if (sd >= -0.01f) continue;  // not behind the panel plane
        // Priority: projected area boosted by staleness. A slot that
        // hasn't re-rendered in k frames counts as ~(1 + k/30)× its
        // area, so a small panel overtakes a big fresh one within a
        // second; never-rendered slots (staleFrames=2^20) win their
        // first fill immediately.
        const float priority = area * (1.0f + float(s.staleFrames) * (1.0f / 30.0f));
        jobs.push_back({ &s, priority, cb, -sd, backSide });
    }
    if (jobs.empty()) return;
    std::sort(jobs.begin(), jobs.end(),
              [](const Job &a, const Job &b) { return a.area > b.area; });
    if (int(jobs.size()) > kRttPerFrame) jobs.resize(kRttPerFrame);
    g_mirrorProf.rttJobsSum += int(jobs.size());

    // ── Offscreen surface (allocated once; CITY cube-bake pattern).
    // Buffers sized for the constant 64K-texel budget; X/Y/BPSL (and
    // the YOffs table) are re-stamped per job to the slot's aspect-
    // matched dimensions.
    static VESA_Surface s_rttSurf = {};
    static bool s_rttInit = false;
    constexpr int kRttTexels = kRttRes * kRttRes;
    if (!s_rttInit) {
        s_rttSurf.X = kRttRes;
        s_rttSurf.Y = kRttRes;
        s_rttSurf.BPP = 32;
        s_rttSurf.CPP = 4;
        s_rttSurf.BPSL = kRttRes * 4;
        s_rttSurf.PageSize = kRttTexels * 4;
        s_rttSurf.Z16  = (byte*)std::malloc(sizeof(word) * kRttTexels);
        s_rttSurf.Data = (byte*)std::malloc(size_t(kRttTexels) * 4);
        if (!s_rttSurf.Z16 || !s_rttSurf.Data) return;
        s_rttSurf.Flip = MainSurf ? MainSurf->Flip : nullptr;
        Build_YOffs_Table(&s_rttSurf);
        s_rttInit = true;
    }

    // ── Offscreen view scope ────────────────────────────────────────
    // Owns the world-state swap: locks out EngineResize, saves and (in
    // its destructor) restores MainSurf + ::View + FOVX/FOVY + NZP and
    // re-stamps the scene so C_NZP/clipper/zscale match the main pass
    // again. setNearZ() is the only way the near plane moves inside
    // the scope — it re-stamps via SetCurrentScene every time, which
    // is the footgun this object exists to remove (writing Sc->NZP
    // alone never reaches the clipper).
    OffscreenViewScope view(sc, &s_rttSurf);
    // Hide every clone mesh: the RTT view must show the REAL scene
    // only (a reflection of a reflection is exactly what the RTT
    // itself provides; clone geometry would double it).
    std::vector<std::pair<TriMesh*, DWord>> savedMeshFlags;
    for (Mirror &m : mirrors) {
        if (!m.cloneMesh) continue;
        savedMeshFlags.push_back({ m.cloneMesh, m.cloneMesh->Flags });
        m.cloneMesh->Flags &= ~HTrack_Visible;
    }
    // Mute clone-omni FLARES: their footprint gate reads the MAIN
    // screen's mask at main-screen coords — meaningless against the
    // RTT surface. (Clone LIGHTING needs no handling here: forward
    // Lighting skips Omni_MirrorClone globally.)
    std::vector<std::pair<Omni*, float>> savedOmniSize;
    for (Omni *O = sc->OmniHead; O; O = O->Next) {
        if (!(O->Flags & Omni_MirrorClone)) continue;
        savedOmniSize.push_back({ O, O->ISize });
        O->ISize = 0.0f;
    }
    // Forward Gouraud needs lit vertex colors. The tick's own
    // Lighting() provides them every frame (clone-free now) — but it
    // runs AFTER this pass, so the very first frame would render from
    // unlit verts. Prime once; afterwards reuse the previous frame's
    // colors (1-frame-stale robot shading in a 256px second-bounce
    // panel is imperceptible). The per-frame Lighting(sc) that used to
    // sit here was ~4.5 ms — the bulk of the whole RTT pass cost.
    static bool sLitOnce = false;
    if (!sLitOnce) {
        Lighting(sc);
        sLitOnce = true;
    }

    static Camera s_rttCam;
    view.setView(&s_rttCam);     // restored by the view scope

    for (const Job &j : jobs) {
        MirrorRttSlot &s = *j.slot;
        const float D = j.dist;
        // Re-stamp the surface to this slot's aspect-matched dims
        // (same texel count, different shape) and republish globals.
        if (s_rttSurf.X != s.texW || s_rttSurf.Y != s.texH) {
            s_rttSurf.X = s.texW;
            s_rttSurf.Y = s.texH;
            s_rttSurf.BPSL = s.texW * 4;
            s_rttSurf.PageSize = s.texW * s.texH * 4;
            Build_YOffs_Table(&s_rttSurf);
        }
        view.publishSurface();
        // Camera basis: right = axisU, up = axisV, forward = B's
        // normal — looking from behind the plane through the window at
        // the real scene. With the view axis ⟂ the panel, the engine's
        // screen-parallel near plane IS the mirror plane: setting NZP
        // just past D clips everything behind the mirror exactly.
        // Effective plane/basis: back-side jobs flip N and U (V kept)
        // — flipping both keeps the basis right-handed, and the
        // negated U makes the texture display correctly through the
        // same UVs when the face is viewed from behind.
        const Vector eN = j.backSide
            ? Vector{ -s.bN.x, -s.bN.y, -s.bN.z } : s.bN;
        const Vector eU = j.backSide
            ? Vector{ -s.axisU.x, -s.axisU.y, -s.axisU.z } : s.axisU;
        const float eU0 = j.backSide ? -s.u1 : s.u0;
        const float eU1 = j.backSide ? -s.u0 : s.u1;
        std::memset(&s_rttCam, 0, sizeof(s_rttCam));
        s_rttCam.ISource = j.camPos;
        s_rttCam.Mat[0][0] = eU.x;      s_rttCam.Mat[0][1] = eU.y;      s_rttCam.Mat[0][2] = eU.z;
        s_rttCam.Mat[1][0] = s.axisV.x; s_rttCam.Mat[1][1] = s.axisV.y; s_rttCam.Mat[1][2] = s.axisV.z;
        s_rttCam.Mat[2][0] = eN.x;      s_rttCam.Mat[2][1] = eN.y;      s_rttCam.Mat[2][2] = eN.z;
        // TRUE off-axis projection: the panel window maps edge-to-edge
        // onto the kRttRes² target, so the window gets every texel
        // regardless of viewing angle:
        //   screen_x =  FOVX*(pu - cu)/D + CntrEX  with  u0→0, u1→W
        //   screen_y = -FOVY*(pv - cv)/D + CntrEY  with  v1→0, v0→H
        // The projection center generally lies far outside the target;
        // fds::g_offAxisFrustumCull (set below) keeps the engine's
        // symmetric-frustum mesh cull from discarding everything. (The
        // first cut centered a symmetric frustum on the camera's
        // plane-foot instead — at oblique angles the window collapsed
        // to a few dozen texels and smeared, the 'garbled 2nd mirror'.)
        const float cu = j.camPos.x*eU.x + j.camPos.y*eU.y + j.camPos.z*eU.z;
        const float cv = j.camPos.x*s.axisV.x + j.camPos.y*s.axisV.y + j.camPos.z*s.axisV.z;
        FOVX   = float(s.texW) * D / (eU1 - eU0);
        FOVY   = float(s.texH) * D / (s.v1 - s.v0);
        CntrEX = FOVX * (cu - eU0) / D;
        CntrEY = FOVY * (s.v1 - cv) / D;
        CntrX  = int32_t(std::min(std::max(CntrEX, -32000.0f), 32000.0f));
        CntrY  = int32_t(std::min(std::max(CntrEY, -32000.0f), 32000.0f));
        // Oblique mirror-plane clip: near plane just past the wall.
        // setNearZ re-stamps C_NZP + clipper through SetCurrentScene —
        // writing Sc->NZP directly never reaches the clipper, and the
        // un-clipped back-side twin of the panel was the black-mirror
        // bug (z written everywhere, color nowhere).
        view.setNearZ(D * 1.001f + 0.01f);
        // Stamp this slot's UVs for the projection above. With the
        // edge-to-edge mapping these are static in window space, but
        // recomputing through the same formula keeps UV and projection
        // trivially in lockstep.
        for (const MirrorRttSlot::SlotVert &sv : s.verts) {
            const float epu = j.backSide ? -sv.pu : sv.pu;
            float tu = ( FOVX * (epu - cu) / D + CntrEX) / float(s.texW);
            float tv = (-FOVY * (sv.pv - cv) / D + CntrEY) / float(s.texH);
            // Keep off the wrap seam (Txtr_Tiled wraps).
            tu = std::min(std::max(tu, 0.002f), 0.998f);
            tv = std::min(std::max(tv, 0.002f), 0.998f);
            if (std::getenv("FDS_MIRROR_RTT_UV05")) { tu = 0.5f; tv = 0.5f; }
            sv.v->U = tu;
            sv.v->V = tv;
        }
        // The clipper re-stamps vertex UVs from Face::U1..V3
        // (FRUSTRUM.CPP) — without syncing the face fields, the
        // values above get overwritten with the source panel's
        // authored UVs whenever the face clips (which is how the
        // display stayed on the stale mapping no matter what the
        // vertex stamp did).
        for (Face *f : s.faces) f->uvFromVertices();

        std::memset(s_rttSurf.Data, 0, size_t(s_rttSurf.PageSize));
        std::memset(s_rttSurf.Z16, 0, sizeof(word) * size_t(s.texW) * size_t(s.texH));
        fds::g_offAxisFrustumCull = true;
        Transform_Objects(sc, fds::g_mainCamera, fds::g_mainFaces);
        fds::g_offAxisFrustumCull = false;
        if (CAll != 0) {
            Radix_Sort(FList, SList, CAll);
            Render(RenderPath::ForceForward);
        }
        if (std::getenv("FDS_MIRROR_RTT_DUMP")) {
            const uint32_t *px = (const uint32_t*)s_rttSurf.Data;
            const uint16_t *zp = (const uint16_t*)s_rttSurf.Z16;
            int nz = 0, nzz = 0;
            for (int i = 0; i < s.texW * s.texH; ++i) {
                if ((px[i] & 0xFFFFFF) != 0) ++nz;
                if (zp[i] != 0) ++nzz;
            }
            std::fprintf(stderr,
                "[MIRROR-RTT] order=%d job: %d/%d color px, %d z px, CAll=%d\n",
                int(s.order), nz, s.texW * s.texH, nzz, int(CAll));
        }
        // FDS_MIRROR_RTT_MARK=1: paint orientation markers into the
        // linear buffer (top=red, bottom=blue, left=green,
        // right=yellow) so flips/mirror errors in the panel mapping
        // are unambiguous on screen.
        if (std::getenv("FDS_MIRROR_RTT_MARK")) {
            uint32_t *px = (uint32_t*)s_rttSurf.Data;
            for (int y = 0; y < s.texH; ++y) {
                for (int x = 0; x < s.texW; ++x) {
                    uint32_t c = 0;
                    if (y < 16)               c = 0xFFFF0000;  // top: red
                    else if (y >= s.texH-16)  c = 0xFF0000FF;  // bottom: blue
                    else if (x < 16)          c = 0xFF00FF00;  // left: green
                    else if (x >= s.texW-16)  c = 0xFFFFFF00;  // right: yellow
                    if (c) px[size_t(y)*size_t(s.texW) + x] = c;
                }
            }
        }
        // FDS_MIRROR_RTT_DUMP=1: write each slot's first rendered
        // frame to /tmp for inspection (linear, pre-Sachletz).
        if (std::getenv("FDS_MIRROR_RTT_DUMP")) {
            static int sDumped = 0;
            if (sDumped < 8) {
                char path[64];
                std::snprintf(path, sizeof(path),
                              "/tmp/rtt_m%u_m%u_%d.ppm",
                              unsigned(s.aId), unsigned(s.bId), sDumped);
                if (FILE *f = std::fopen(path, "wb")) {
                    std::fprintf(f, "P6\n%d %d\n255\n", s.texW, s.texH);
                    const uint32_t *px = (const uint32_t*)s_rttSurf.Data;
                    for (int i = 0; i < s.texW * s.texH; ++i) {
                        const uint32_t p = px[i];
                        unsigned char rgb[3] = {
                            (unsigned char)((p >> 16) & 0xFF),
                            (unsigned char)((p >>  8) & 0xFF),
                            (unsigned char)( p        & 0xFF) };
                        std::fwrite(rgb, 1, 3, f);
                    }
                    std::fclose(f);
                    std::fprintf(stderr, "[MIRROR-RTT] dumped %s "
                                 "(cam %.1f,%.1f,%.1f D=%.2f win %.1fx%.1f "
                                 "FOVX=%.0f FOVY=%.0f CntrE=%.0f,%.0f "
                                 "NZP=%.2f FZP=%.0f CAll=%d)\n",
                                 path, j.camPos.x, j.camPos.y, j.camPos.z,
                                 D, s.u1 - s.u0, s.v1 - s.v0,
                                 FOVX, FOVY, CntrEX, CntrEY,
                                 sc->NZP, sc->FZP, int(CAll));
                    ++sDumped;
                }
            }
        }
        // Half-silvered composite (first order): overlay the panel's
        // dynamic text texture on the reflection, texel = text +
        // reflection/2 saturated — the formula the transparent kernel
        // uses for glass. Texel→window is the fixed edge-to-edge
        // mapping; window→text UV is the affine captured at build.
        // The text texture is Sachletz-tiled in memory (4x4 blocks,
        // X-outer/Y-inner write order) — read through the inverse.
        if (s.order == 1 && s.textTex && s.textTex->Mipmap[0]) {
            // Mipmap[0], NOT Data: the greets generator repoints
            // Mipmap[0] at its dynamic text buffer (GREETS.CPP ~227);
            // Data keeps the static disk JPG. The rasterizer samples
            // Mipmap[miplevel], so the live content is here.
            const dword *td = (const dword*)s.textTex->Mipmap[0];
            const int tw = s.textTex->SizeX, th = s.textTex->SizeY;
            const int tBlocksY = th >> 2;
            uint32_t *px = (uint32_t*)s_rttSurf.Data;
            const float du = (eU1 - eU0) / float(s.texW);
            const float dv = (s.v1 - s.v0) / float(s.texH);
            for (int y = 0; y < s.texH; ++y) {
                const float pv = s.v1 - (float(y) + 0.5f) * dv;
                for (int x = 0; x < s.texW; ++x) {
                    const float epu = eU0 + (float(x) + 0.5f) * du;
                    const float pu = j.backSide ? -epu : epu;
                    const float fu = s.tA[0]*pu + s.tA[1]*pv + s.tA[2];
                    const float fv = s.tA[3]*pu + s.tA[4]*pv + s.tA[5];
                    const int iu = int(fu * float(tw)) & (tw - 1);
                    const int iv = int(fv * float(th)) & (th - 1);
                    const int blk = ((iu >> 2) * tBlocksY + (iv >> 2)) << 4;
                    const dword t = td[blk + ((iv & 3) << 2) + (iu & 3)];
                    uint32_t &o = px[size_t(y) * size_t(s.texW) + x];
                    const uint32_t tb =  t        & 0xFF;
                    const uint32_t tg = (t >> 8)  & 0xFF;
                    const uint32_t tr = (t >> 16) & 0xFF;
                    uint32_t ob = tb + (( o        & 0xFF) >> 1);
                    uint32_t og = tg + (((o >> 8)  & 0xFF) >> 1);
                    uint32_t orr = tr + (((o >> 16) & 0xFF) >> 1);
                    if (ob > 255) ob = 255;
                    if (og > 255) og = 255;
                    if (orr > 255) orr = 255;
                    o = ob | (og << 8) | (orr << 16) | 0xFF000000u;
                }
            }
        }
        // Linear RTT pixels → slot texture, re-tiled in place (the
        // same one-way Sachletz the texture loader applies).
        std::memcpy(s.mat->Txtr->Data, s_rttSurf.Data,
                    size_t(s.texW) * size_t(s.texH) * 4);
        Sachletz((dword*)s.mat->Txtr->Data, s.texW, s.texH);
        s.staleFrames = 0;
    }

    // ── Restore ─────────────────────────────────────────────────────
    // Camera/surface/near-plane restore happens in the view scope's
    // destructor; only the mirror-specific muting is undone here.
    for (auto &p : savedOmniSize)  p.first->ISize = p.second;
    for (auto &p : savedMeshFlags) p.first->Flags = p.second;
}

void ProbeSecondOrderMirrors(Scene *sc, const std::vector<Mirror> &mirrors)
{
    if (!sc || !fds::FeatureFlags::mirror_rtt_probe()) return;
    const Camera *cam = ::View ? ::View : sc->CameraHead;
    if (!cam) return;
    // Throttle: one report every 30 frames is plenty for reading along
    // while flying (and exactly one per snapshot tick sequence).
    static int sGate = 0;
    if (sGate++ % 30 != 0) return;

    const float (*VM)[3] = cam->Mat;
    const Vector C = cam->ISource;
    auto screenOf = [&](const Vector &wp, float &sx, float &sy) -> bool {
        const float dx = wp.x - C.x, dy = wp.y - C.y, dz = wp.z - C.z;
        const float vz = VM[2][0]*dx + VM[2][1]*dy + VM[2][2]*dz;
        if (vz <= 0.05f) return false;
        const float vx = VM[0][0]*dx + VM[0][1]*dy + VM[0][2]*dz;
        const float vy = VM[1][0]*dx + VM[1][1]*dy + VM[1][2]*dz;
        sx =  FOVX * vx / vz + CntrEX;
        sy = -FOVY * vy / vz + CntrEY;
        return true;
    };

    for (const Mirror &A : mirrors) {
        if (!A.active || A.parentMirrorId != 0 || !A.cloneMesh) continue;
        if (A.cloneFaceSrc.empty()) continue;
        for (const Mirror &B : mirrors) {
            if (&B == &A || B.id == 0 || B.parentMirrorId != 0) continue;
            if (B.wallFaces.empty() || !B.plane.valid) continue;
            // B itself need not be active — its own panel can be off
            // screen while its image inside A is visible.
            float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
            int panelFaces = 0, clippedVerts = 0;
            for (size_t fi = 0; fi < A.cloneFaceSrc.size()
                              && fi < size_t(A.cloneMesh->FIndex); ++fi) {
                const Face *src = A.cloneFaceSrc[fi].face;
                bool isBWall = false;
                for (const Face *WF : B.wallFaces) {
                    if (WF == src) { isBWall = true; break; }
                }
                if (!isBWall) continue;
                ++panelFaces;
                const Face &CF = A.cloneMesh->Faces[fi];
                const Vertex *vs[3] = { CF.A, CF.B, CF.C };
                for (int k = 0; k < 3; ++k) {
                    float sx, sy;
                    // Clone verts are world-baked (identity transform).
                    if (!screenOf(vs[k]->Pos, sx, sy)) { ++clippedVerts; continue; }
                    bx0 = std::min(bx0, sx); bx1 = std::max(bx1, sx);
                    by0 = std::min(by0, sy); by1 = std::max(by1, sy);
                }
            }
            if (panelFaces == 0) continue;
            // Clamp the projected bbox to the viewport — what's outside
            // can never need texels.
            const float vw = float(::XRes), vh = float(::YRes);
            bx0 = std::max(bx0, 0.0f); by0 = std::max(by0, 0.0f);
            bx1 = std::min(bx1, vw);   by1 = std::min(by1, vh);
            const int pxW = int(std::ceil(bx1 - bx0));
            const int pxH = int(std::ceil(by1 - by0));
            if ((pxW <= 0 || pxH <= 0) && clippedVerts == 0) continue;

            // Virtual cameras: the viewer reflected through A sees B's
            // panel; what B shows that viewer is the real scene seen
            // from the doubly-reflected position.
            const Vector CA = reflectPointAcross(C,  A.plane.N, A.plane.d);
            const Vector CB = reflectPointAcross(CA, B.plane.N, B.plane.d);

            // B's panel window in its plane basis — the off-axis
            // frustum rectangle the RTT camera must cover.
            Vector u;  // any unit vector ⟂ B's normal
            {
                const Vector &n = B.plane.N;
                if (std::fabs(n.y) < 0.9f) u = { n.z, 0.0f, -n.x };
                else                       u = { 1.0f, 0.0f, 0.0f };
                u.normalize();
            }
            const Vector &n = B.plane.N;
            const Vector v = { n.y*u.z - n.z*u.y,
                               n.z*u.x - n.x*u.z,
                               n.x*u.y - n.y*u.x };
            float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
            for (size_t i = 0; i < B.wallFaces.size(); ++i) {
                const Face *WF = B.wallFaces[i];
                TriMesh *WT = i < B.wallFaceMeshes.size()
                    ? B.wallFaceMeshes[i] : nullptr;
                if (!WF || !WF->A || !WF->B || !WF->C) continue;
                const Vertex *ws[3] = { WF->A, WF->B, WF->C };
                for (int k = 0; k < 3; ++k) {
                    Vector wp = ws[k]->Pos;
                    if (WT) {
                        Vector lp = wp;
                        MatrixXVector(WT->RotMat, &lp, &wp);
                        wp += WT->IPos;
                    }
                    const float pu = wp.x*u.x + wp.y*u.y + wp.z*u.z;
                    const float pv = wp.x*v.x + wp.y*v.y + wp.z*v.z;
                    u0 = std::min(u0, pu); u1 = std::max(u1, pu);
                    v0 = std::min(v0, pv); v1 = std::max(v1, pv);
                }
            }
            std::fprintf(stderr,
                "[RTT-PROBE] m%u('%s') shows m%u('%s'): footprint %dx%d px"
                "%s | virtCam=(%.2f,%.2f,%.2f) | panel %.2fx%.2f world "
                "(u=[%.2f..%.2f] v=[%.2f..%.2f])\n",
                unsigned(A.id), A.wallMaterialName.c_str(),
                unsigned(B.id), B.wallMaterialName.c_str(),
                pxW, pxH, clippedVerts ? " (near-clipped, partial)" : "",
                CB.x, CB.y, CB.z,
                u1 - u0, v1 - v0, u0, u1, v0, v1);
        }
    }
}

void DebugOverlayMirrorMask(Scene *sc)
{
    (void)sc;
    if (!g_gbuffer || g_gbuffer->mirrorId.empty()) return;
    const u8 *plane = g_gbuffer->mirrorId.data();
    dword *fb = (dword*)::VPage;
    const int w = int(::XRes);
    const int h = int(::YRes);
    if (size_t(w) * size_t(h) > g_gbuffer->mirrorId.size()) return;
    // Distinct color per mirror id. Saturate at full channel so the
    // overlay is visible regardless of underlying pixel.
    auto idToColor = [](u8 id) -> dword {
        if (id == 0) return 0;
        const dword channel = 200u;  // strong but not white-out
        switch (id % 6) {
            case 1: return (channel << 16);                // red
            case 2: return (channel << 8);                 // green
            case 3: return channel;                        // blue
            case 4: return (channel << 16) | (channel << 8);  // yellow
            case 5: return (channel << 8)  | channel;      // cyan
            default: return (channel << 16) | channel;     // magenta
        }
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const u8 id = plane[size_t(y) * size_t(w) + size_t(x)];
            if (id == 0) continue;
            const dword color = idToColor(id);
            const dword existing = fb[size_t(y) * size_t(w) + size_t(x)];
            // Blend ~50/50 so we can still see the underlying scene.
            const dword bExist = (existing      ) & 0xFF;
            const dword gExist = (existing >>  8) & 0xFF;
            const dword rExist = (existing >> 16) & 0xFF;
            const dword bNew = (color      ) & 0xFF;
            const dword gNew = (color >>  8) & 0xFF;
            const dword rNew = (color >> 16) & 0xFF;
            const dword bOut = (bExist + bNew) / 2;
            const dword gOut = (gExist + gNew) / 2;
            const dword rOut = (rExist + rNew) / 2;
            fb[size_t(y) * size_t(w) + size_t(x)] =
                bOut | (gOut << 8) | (rOut << 16) | 0xFF000000u;
        }
    }
}

void StampMirrorMasks(Scene *sc, const std::vector<Mirror> &mirrors)
{
    ScopedMirrorMs _t(&g_mirrorProf.stampMs);
    if (!sc || !g_gbuffer) return;
    // The pre-stamp lives in the gate plane (mirrorMask), NOT the
    // ownership plane (mirrorId). Mekalele's commit mutates mirrorId
    // during raster; mirrorMask stays immutable so the per-pixel gate
    // test reads the same value for every face that targets a pixel.
    auto &plane = g_gbuffer->mirrorMask;
    if (plane.empty()) return;  // no mirror has activated the plane
    auto &zplane = g_gbuffer->mirrorMaskZ;
    if (zplane.size() < plane.size()) return;  // sized together
    // Clear last frame's coverage. Cheap memset — 2+4 MB at 1080p.
    std::memset(plane.data(), 0, plane.size());
    std::memset(zplane.data(), 0, zplane.size() * sizeof(uint16_t));
    // Also clear the ownership plane each frame so foreground commits
    // (which write ownerMirrorId = 0) start from a known baseline; the
    // deferred lighting reads it as pmid per pixel.
    if (!g_gbuffer->mirrorId.empty()) {
        std::memset(g_gbuffer->mirrorId.data(), 0, g_gbuffer->mirrorId.size());
    }
    if (g_gbufferTransparent && !g_gbufferTransparent->mirrorId.empty()) {
        std::memset(g_gbufferTransparent->mirrorId.data(), 0,
                    g_gbufferTransparent->mirrorId.size());
    }
    if (g_gbufferTransparentBack && !g_gbufferTransparentBack->mirrorId.empty()) {
        std::memset(g_gbufferTransparentBack->mirrorId.data(), 0,
                    g_gbufferTransparentBack->mirrorId.size());
    }
    // Use the engine surface size that matches the gbuffer plane.
    const int w = int(::XRes);
    const int h = int(::YRes);
    if (size_t(w) * size_t(h) > plane.size()) return;
    // Use a Z threshold safely above NZP so the per-vertex divide
    // doesn't blow up screen coords for verts grazing the near plane
    // (greets's NZP is 0.01, but z=0.18 still yields PX values >10×
    // screen width). Sutherland-Hodgman clips each wall triangle in
    // PROJECTION-PRE-DIVIDE space against z >= kClipZ, then divides
    // the clipped verts' (x, y) by z to get screen coords.
    //
    // Pre-divide coords are computed HERE from world positions + the
    // live camera — NOT read from Vertex::TPos_AOS. TPos_AOS is only
    // refreshed for meshes that survive Transform_Objects' frustum
    // cull; greets's chunked room mesh gets culled per-chunk all the
    // time, so a culled wall's TPos_AOS holds LAST-VISIBLE-frame
    // values and the stamp landed a stale footprint over arbitrary
    // geometry — which the behind-mirror gate then carved out of real
    // walls ("mirror visible through walls").
    const float kClipZ = std::max(0.5f, sc->NZP * 5.0f);
    const Camera *cam = ::View ? ::View : sc->CameraHead;
    if (!cam) return;
    const float (*VM)[3] = cam->Mat;
    const Vector camSrc = cam->ISource;
    auto preDivideOfWorld = [&](const Vector &wp) -> Vector {
        const float dx = wp.x - camSrc.x;
        const float dy = wp.y - camSrc.y;
        const float dz = wp.z - camSrc.z;
        const float vx = VM[0][0]*dx + VM[0][1]*dy + VM[0][2]*dz;
        const float vy = VM[1][0]*dx + VM[1][1]*dy + VM[1][2]*dz;
        const float vz = VM[2][0]*dx + VM[2][1]*dy + VM[2][2]*dz;
        // Same perspective pre-scale Transform_Objects bakes into
        // TPos_AOS (see the cube-face block in Transform.cpp): FOVX/
        // FOVY are View->PerspX/PerspY, CntrEX/CntrEY the screen
        // center, y negated for screen-down.
        return { FOVX * vx + CntrEX * vz,
                 -FOVY * vy + CntrEY * vz,
                 vz };
    };
    auto projectPreDivideToScreen = [](const Vector &vp, float &sx, float &sy) {
        const float invZ = 1.0f / vp.z;
        sx = vp.x * invZ;
        sy = vp.y * invZ;
    };
    // Camera position used to gate per-mirror rendering: when the
    // viewer crosses to the back side of a mirror plane, the wall
    // faces are back-facing (culled by Transform_Objects) but the
    // clone mesh's faces stay in the FList — some clones happen to
    // be front-facing from the back-side viewpoint and were
    // rendering anyway, since Mekalele's per-pixel mask only rejects
    // pixels outside the wall's screen footprint. The cure is to
    // skip stamping the mask for any mirror whose viewer is on the
    // back side (plane sign N·C + d <= 0); with no mask, Mekalele
    // rejects every clone pixel for that mirror.
    const Vector *camPos = nullptr;
    if (sc->CameraHead) camPos = &sc->CameraHead->ISource;
    if (::View) camPos = &::View->ISource;
    for (const auto &m : mirrors) {
        if (m.id == 0 || m.wallFaces.empty()) continue;
        // Deactivated this frame (UpdateAllMirrors visibility gate) —
        // its clone mesh is hidden, so stamping a footprint would
        // carve behind-gate holes with nothing to fill them.
        if (!m.active) continue;
        // Compound mirrors do not stamp their own pre-mask. Their wall
        // surface and clone faces gate on the PARENT mirror's stamp so
        // Mekalele's z-test resolves "compound wall vs A's clone vs
        // compound clone" naturally per pixel. Stamping a compoundId
        // here would overwrite A.id and gate A's clones out of the
        // sub-area (the bug that hid objects "behind" the in-mirror
        // mirror).
        if (m.parentMirrorId != 0) continue;
        // Per-wall-face viewer-side gate. The old single-plane test
        // used the AVERAGED cluster normal — greets's P_TEXT screens
        // disagree with the average, so it killed reflections on
        // every screen but one. Loop over the actual wall faces
        // instead: if NO face is front-facing from the camera, skip
        // the mirror entirely. For greets's multi-orientation
        // clusters at least one screen is always facing the viewer
        // when any of them is visible; for the test scene's single
        // back-mirror panel, moving the camera behind z=5 turns
        // every wall face back-facing and the mirror falls silent
        // — which is the desired "no leak from the back side".
        bool anyFrontFacing = (camPos == nullptr);
        if (!anyFrontFacing) {
            for (const Face *F : m.wallFaces) {
                if (!F || !F->A) continue;
                // Wall face normal in WORLD space, plus its NormProd
                // (= -N·A_world). Mesh-local→world transform isn't
                // needed: BuildMirror's wallFaces are pointers into
                // live source meshes whose RotMat is identity in
                // practice (mirror walls are static room geometry),
                // so F->N is already world-space. If a scene ever
                // animates a mirror wall, walk T->RotMat * F->N.
                const float dot = F->N.x * camPos->x
                                + F->N.y * camPos->y
                                + F->N.z * camPos->z
                                + F->NormProd;
                if (dot > 0.0f) { anyFrontFacing = true; break; }
            }
        }
        if (!anyFrontFacing) continue;
        for (size_t wf = 0; wf < m.wallFaces.size(); ++wf) {
            const Face *F = m.wallFaces[wf];
            TriMesh *WT = wf < m.wallFaceMeshes.size()
                ? m.wallFaceMeshes[wf] : nullptr;
            if (!F || !F->A || !F->B || !F->C) continue;
            auto worldOf = [&](const Vertex *v) -> Vector {
                Vector lp = v->Pos;
                if (!WT) return lp;  // clone meshes bake world coords
                Vector wp;
                MatrixXVector(WT->RotMat, &lp, &wp);
                wp += WT->IPos;
                return wp;
            };
            // Clip the (A,B,C) triangle in view-space against z >= kClipZ.
            // Up to 4 verts after a single-plane clip of a triangle.
            Vector clipped[4];
            int nc = 0;
            const Vector tri[3] = {
                preDivideOfWorld(worldOf(F->A)),
                preDivideOfWorld(worldOf(F->B)),
                preDivideOfWorld(worldOf(F->C)),
            };
            for (int e = 0; e < 3; ++e) {
                const Vector &v0 = tri[e];
                const Vector &v1 = tri[(e + 1) % 3];
                const bool in0 = v0.z >= kClipZ;
                const bool in1 = v1.z >= kClipZ;
                if (in0) clipped[nc++] = v0;
                if (in0 != in1 && nc < 4) {
                    const float t = (kClipZ - v0.z) / (v1.z - v0.z);
                    clipped[nc++] = {
                        v0.x + t * (v1.x - v0.x),
                        v0.y + t * (v1.y - v0.y),
                        kClipZ
                    };
                }
            }
            if (nc < 3) continue;
            // Project clipped polygon verts to screen + fan-triangulate.
            // rz = 1/z per vert feeds the wall-depth interpolation.
            float sx[4], sy[4], rz[4];
            for (int i = 0; i < nc; ++i) {
                projectPreDivideToScreen(clipped[i], sx[i], sy[i]);
                rz[i] = 1.0f / clipped[i].z;
            }
            // Compound mirrors stamp only into pixels their parent
            // already owns. 0xFF means "unconditional" for base mirrors.
            const u8 requireExisting = (m.parentMirrorId != 0)
                ? m.parentMirrorId : u8(0xFF);
            for (int i = 1; i + 1 < nc; ++i) {
                StampTri2D(plane.data(), zplane.data(), w, h,
                           sx[0], sy[0], rz[0],
                           sx[i], sy[i], rz[i],
                           sx[i+1], sy[i+1], rz[i+1],
                           m.id, g_zscale, requireExisting);
            }
        }
    }
    // Mirror the stamped mask onto the transparent gbuffers so clones
    // of transparent source faces (which Mekalele dispatches via the
    // TransparentFront / TransparentBack targets) gate against the
    // same per-pixel id. Without this, transparent-source clones
    // bypass the mask entirely and render across the whole screen.
    auto copyMaskTo = [&](meka::GBuffer *gb) {
        if (!gb || gb->mirrorMask.size() != plane.size()) return;
        std::memcpy(gb->mirrorMask.data(), plane.data(), plane.size());
        if (gb->mirrorMaskZ.size() == zplane.size()) {
            std::memcpy(gb->mirrorMaskZ.data(), zplane.data(),
                        zplane.size() * sizeof(uint16_t));
        }
    };
    copyMaskTo(g_gbufferTransparent);
    copyMaskTo(g_gbufferTransparentBack);
}

}  // namespace fds
