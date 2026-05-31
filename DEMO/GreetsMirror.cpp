#include "GreetsMirror.h"

#include <Base/FDS_DECS.H>  // Matrix_Copy + Mat_ID
#include <Base/FDS_VARS.H>  // MatrixXVector template
#include <Base/Face.h>
#include <Base/Material.h>
#include <Base/Matrix.h>
#include <Base/Object.h>
#include <Base/Omni.h>
#include <Base/Scene.h>
#include <Base/TriMesh.h>
#include <Base/Vertex.h>
#include <FILLERS/Mekalele.h>  // g_gbuffer + GBuffer::mirrorId plane

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Provided by MISC/PREPROC.CPP — stamps F->A_idx/B_idx/C_idx for SoA.
extern void Compute_FaceVertexIndices(TriMesh *T);

// getAlignedBlock / getAlignedType come in via FDS_DECS.H above.

namespace fds {
namespace {

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

// Generic walker over faces whose Material name matches `wallMatName`.
// Visitor receives (T, F, wN) where wN is the face's world-space normal.
template <class Visit>
void walkWallFaces(Scene *sc, const char *wallMatName, Visit &&visit) {
    for (Object *Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh) continue;
        TriMesh *T = (TriMesh*)Obj->Data;
        if (!T || !T->Faces) continue;
        for (DWord fi = 0; fi < T->FIndex; ++fi) {
            Face &F = T->Faces[fi];
            if (!F.A || !F.B || !F.C) continue;
            if (!F.Txtr || !F.Txtr->Name) continue;
            if (std::strcmp(F.Txtr->Name, wallMatName) != 0) continue;
            Vector localN = F.N;
            Vector wN;
            MatrixXVector(T->RotMat, &localN, &wN);
            visit(T, F, wN);
        }
    }
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

MirrorPlane FindMirrorPlaneByMatName(Scene *sc, const char *wallMatName)
{
    MirrorPlane out{};
    if (!sc || !wallMatName) return out;

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
    walkWallFaces(sc, wallMatName, [&](TriMesh *T, Face &F, const Vector &wN) {
        Vector u = wN;
        u.normalize();
        samples.push_back({T, &F, u});
    });
    if (samples.empty()) {
        std::fprintf(stderr, "[MIRROR] no '%s' faces found\n", wallMatName);
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
    int keptCount = 0, droppedOutlier = 0;
    for (const auto &s : samples) {
        const float dot = s.wN.x*seedN.x + s.wN.y*seedN.y + s.wN.z*seedN.z;
        if (dot < 0.866f) { ++droppedOutlier; continue; }
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
            wallMatName, samples.size());
        return out;
    }
    out.N = accN;
    out.N.normalize();
    out.d = accD / float(keptCount);
    out.faceCount = keptCount;
    out.valid = true;
    std::fprintf(stderr,
        "[MIRROR '%s'] plane N=(%.3f,%.3f,%.3f) d=%.3f from %d/%zu faces "
        "(%d outliers)\n",
        wallMatName, out.N.x, out.N.y, out.N.z, out.d,
        keptCount, samples.size(), droppedOutlier);
    return out;
}

// Monotonic mirror id counter. Each successful BuildMirror call grabs
// the next id (1..255); the value gets written into Face::mirrorMaskTag
// on every face that participates in this mirror (walls + clones), and
// the per-pixel mask check in Mekalele compares against it. Wrap-around
// past 255 is treated as a hard error — at that point the encoding is
// out of u8 space and we'd need to widen the plane.
static uint8_t s_nextMirrorId = 1;

Mirror BuildMirror(Scene *sc, const char *wallMatName)
{
    Mirror m{};
    if (!sc || !wallMatName) return m;
    m.wallMaterialName = wallMatName;
    m.plane = FindMirrorPlaneByMatName(sc, wallMatName);
    if (!m.plane.valid) return m;
    if (s_nextMirrorId == 0) {
        std::fprintf(stderr, "[MIRROR '%s'] all 255 mirror ids in use — skipping\n",
                     wallMatName);
        return m;
    }
    m.id = s_nextMirrorId++;

    const Vector &N = m.plane.N;
    const float   d = m.plane.d;
    auto isWall = [&](const Face &F) -> bool {
        return F.Txtr && F.Txtr->Name &&
               std::strcmp(F.Txtr->Name, wallMatName) == 0;
    };
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
        return dot >= 0.866f;
    };

    // Count total verts/faces (excluding mirror-surface faces — we
    // want a hole where the mirror is, not a mirror image of the
    // mirror itself).
    DWord totalVerts = 0, totalFaces = 0;
    for (Object *Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh) continue;
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
            "[MIRROR '%s'] nothing to clone (0 non-wall faces)\n", wallMatName);
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
    MM->Flags |= HTrack_Visible;
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

    // Fill verts (world-mirrored) and faces (winding swapped, normal +
    // NormProd reflected, UV pairs swapped to match B↔C).
    DWord vOfs = 0, fOfs = 0;
    Vector bbMin = { 1e30f, 1e30f, 1e30f};
    Vector bbMax = {-1e30f,-1e30f,-1e30f};
    for (Object *Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh) continue;
        TriMesh *T = (TriMesh*)Obj->Data;
        if (!T || !T->Verts || !T->Faces) continue;
        const DWord vStart = vOfs;
        for (DWord vi = 0; vi < T->VIndex; ++vi) {
            MM->Verts[vOfs] = T->Verts[vi];
            Vector localP = T->Verts[vi].Pos;
            Vector worldP;
            MatrixXVector(T->RotMat, &localP, &worldP);
            worldP.x += T->IPos.x; worldP.y += T->IPos.y; worldP.z += T->IPos.z;
            const Vector mirroredP = reflectPointAcross(worldP, N, d);
            MM->Verts[vOfs].Pos = mirroredP;
            Vector localN = T->Verts[vi].N;
            Vector worldN;
            MatrixXVector(T->RotMat, &localN, &worldN);
            MM->Verts[vOfs].N = reflectDirAcross(worldN, N);
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
            Face &CF = MM->Faces[fOfs];
            CF = OF;
            CF.A = MM->Verts + vStart + (OF.A - T->Verts);
            CF.B = MM->Verts + vStart + (OF.C - T->Verts);  // swap
            CF.C = MM->Verts + vStart + (OF.B - T->Verts);  // swap
            std::swap(CF.U2, CF.U3);
            std::swap(CF.V2, CF.V3);
            std::swap(CF.EU2, CF.EU3);
            std::swap(CF.EV2, CF.EV3);
            CF.N = reflectDirAcross(OF.N, N);
            // Engine convention: NormProd = -(N · A).
            CF.NormProd = -(CF.N.x * CF.A->Pos.x +
                            CF.N.y * CF.A->Pos.y +
                            CF.N.z * CF.A->Pos.z);
            // Tag every clone face with the owning mirror's id. Mekalele
            // reads this into ctx.mirrorTag and rejects any pixel whose
            // gb.mirrorId doesn't match — so clones can only paint inside
            // their own mirror's screen-space footprint.
            CF.mirrorMaskTag = m.id;
            ++fOfs;
        }
        m.meshRanges.push_back({T, vStart, T->VIndex});
    }
    m.cloneMesh  = MM;
    m.clonedVerts = int(vOfs);
    m.clonedFaces = int(fOfs);

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
        std::string nm = std::string("mirror_") + wallMatName;
        MObj->Name = (char*)std::malloc(nm.size() + 1);
        std::strcpy(MObj->Name, nm.c_str());
    }
    MObj->Next = sc->ObjectHead;
    if (sc->ObjectHead) sc->ObjectHead->Prev = MObj;
    sc->ObjectHead = MObj;
    MM->Next = sc->TriMeshHead;
    if (sc->TriMeshHead) sc->TriMeshHead->Prev = MM;
    sc->TriMeshHead = MM;

    // Retarget mirror-surface wall faces to a transparent silvered mat.
    // The visible appearance: low base alpha (mirror dominates) + a
    // silver stub texture + cranked Specular so the deferred lighting
    // kernel paints highlights along the wall's specular angle —
    // sells "polished metal surface" rather than "tinted glass". For
    // already-textured wall mats we keep the source texture; only
    // BaseCol-only (flat-shaded) walls get the silver stub.
    constexpr float kMirrorAlpha     = 0.15f;   // 15% wall tint, 85% mirror beneath
    constexpr float kMirrorSpecular  = 32.0f;   // pronounced spec lobe
    const Color     kMirrorSilver    = { 180.0f, 180.0f, 200.0f, 255.0f };  // BGRA, cool silver
    for (Object *Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh) continue;
        if (Obj == MObj) continue;
        TriMesh *T = (TriMesh*)Obj->Data;
        if (!T || !T->Faces) continue;
        for (DWord fi = 0; fi < T->FIndex; ++fi) {
            Face &F = T->Faces[fi];
            if (!isMirrorSurface(F, T)) continue;
            if (!m.wallMatClone) {
                m.wallMatClone = getAlignedType<Material>(16);
                std::memcpy(m.wallMatClone, F.Txtr, sizeof(Material));
                m.wallMatClone->Flags |= Mat_Transparent;
                m.wallMatClone->XparBlendAlpha = kMirrorAlpha;
                m.wallMatClone->Specular       = kMirrorSpecular;
                m.wallMatClone->BaseCol        = kMirrorSilver;
                if (!m.wallMatClone->Txtr) {
                    // Flat-shaded source — use silver stub directly.
                    m.wallMatClone->Txtr = synthesizeFlatTexture(kMirrorSilver);
                }
                m.wallMatClone->Prev = nullptr;
                m.wallMatClone->Next = MatLib;
                if (MatLib) MatLib->Prev = m.wallMatClone;
                MatLib = m.wallMatClone;
            }
            F.Txtr = m.wallMatClone;
            // Wall face also tagged with the mirror id. The mask pre-pass
            // (StampMirrorMasks) reads this to know which value to stamp
            // into gb.mirrorId for the face's screen-space pixels.
            F.mirrorMaskTag = m.id;
            m.wallFaces.push_back(&F);
            ++m.wallFacesRetargeted;
        }
    }
    // Allocate gb.mirrorId plane on first mirror in the scene. Sized
    // to match the engine surface; 1 byte per pixel = 2 MB at 1080p.
    if (g_gbuffer) {
        const size_t needed = g_gbuffer->normal.size();
        if (g_gbuffer->mirrorId.size() < needed) {
            g_gbuffer->mirrorId.assign(needed, 0);
        }
    } else {
        std::fprintf(stderr,
            "[MIRROR '%s'] WARN: g_gbuffer null at BuildMirror — mask plane not allocated\n",
            wallMatName);
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
        clone->Prev = nullptr;
        clone->Next = sc->OmniHead;
        if (sc->OmniHead) sc->OmniHead->Prev = clone;
        sc->OmniHead = clone;
        m.omniClones.push_back({srcO, clone, srcO->IRange, srcO->IRange});
        ++omniCount;
    }

    std::fprintf(stderr,
        "[MIRROR '%s'] cloned %u verts / %u faces (mirror bbox z=[%.1f..%.1f]); "
        "retargeted %d wall faces to transparent mat; cloned %d omnis\n",
        wallMatName, unsigned(vOfs), unsigned(fOfs),
        bbMin.z, bbMax.z, m.wallFacesRetargeted, omniCount);
    return m;
}

void UpdateMirror(Scene *sc, Mirror &m)
{
    if (!sc || !m.plane.valid || !m.cloneMesh) return;
    const Vector &N = m.plane.N;
    const float   d = m.plane.d;

    // Per-mesh: re-mirror source's CURRENT world verts into the clone.
    // Dynamic meshes (Hull / legs) end up with up-to-date reflections.
    for (const auto &r : m.meshRanges) {
        TriMesh *T = r.sourceMesh;
        if (!T || !T->Verts) continue;
        const DWord n = std::min<DWord>(r.vCount, T->VIndex);
        for (DWord vi = 0; vi < n; ++vi) {
            Vector localP = T->Verts[vi].Pos;
            Vector worldP;
            MatrixXVector(T->RotMat, &localP, &worldP);
            worldP.x += T->IPos.x; worldP.y += T->IPos.y; worldP.z += T->IPos.z;
            m.cloneMesh->Verts[r.vStart + vi].Pos = reflectPointAcross(worldP, N, d);
            Vector localN = T->Verts[vi].N;
            Vector worldN;
            MatrixXVector(T->RotMat, &localN, &worldN);
            m.cloneMesh->Verts[r.vStart + vi].N = reflectDirAcross(worldN, N);
        }
    }
    // Per-omni: re-mirror IPos / IDir + clamp IRange to plane distance
    // so the lighting sphere can't leak to the wrong side.
    //
    // Floor on IRange: the deferred tile binner projects each omni's
    // bsphere to screen and lists per-tile contributors; tiles outside
    // the projected bsphere skip the omni. If we let IRange shrink
    // below ~10% of the source's range, omnis sitting near the plane
    // get a tile bbox so small that adjacent tiles flicker (some
    // include the omni, some don't, near the bsphere edge) and the
    // 1/range falloff math degenerates. Keep a 10% floor so omnis
    // close to the mirror plane still light reasonably even if their
    // contribution leaks slightly across.
    auto clampedRange = [](float orig, float limit) {
        const float floor = orig * 0.10f;
        return std::max(floor, std::min(orig, limit));
    };
    for (auto &c : m.omniClones) {
        if (!c.sourceOmni || !c.mirrorOmni) continue;
        c.mirrorOmni->IPos = reflectPointAcross(c.sourceOmni->IPos, N, d);
        c.mirrorOmni->IDir = reflectDirAcross(c.sourceOmni->IDir, N);
        const float srcLimit = distToPlane(c.sourceOmni->IPos, N, d);
        const float mirLimit = distToPlane(c.mirrorOmni->IPos, N, d);
        c.sourceOmni->IRange = clampedRange(c.origSourceRange, srcLimit);
        c.mirrorOmni->IRange = clampedRange(c.origMirrorRange, mirLimit);
        c.sourceOmni->rRange = c.sourceOmni->IRange > 0.0f ? 1.0f / c.sourceOmni->IRange : 0.0f;
        c.mirrorOmni->rRange = c.mirrorOmni->IRange > 0.0f ? 1.0f / c.mirrorOmni->IRange : 0.0f;
    }
}

void UpdateAllMirrors(Scene *sc, std::vector<Mirror> &mirrors)
{
    for (auto &m : mirrors) UpdateMirror(sc, m);
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
inline void StampTri2D(u8 *plane, int w, int h,
                       float ax, float ay,
                       float bx, float by,
                       float cx, float cy,
                       u8 value)
{
    // Sort vertices by Y so we can split the triangle into a flat-top
    // and flat-bottom half and scan each between two edges per row.
    if (ay > by) { std::swap(ax, bx); std::swap(ay, by); }
    if (by > cy) { std::swap(bx, cx); std::swap(by, cy); }
    if (ay > by) { std::swap(ax, bx); std::swap(ay, by); }
    const int yTop = std::max(0, int(std::ceil(ay)));
    const int yMid = std::clamp(int(std::ceil(by)), 0, h);
    const int yBot = std::min(h, int(std::ceil(cy)));
    if (yTop >= yBot) return;
    // Edge slopes (dx / dy) for the three edges. Guard against 1-pixel
    // tall triangles to avoid divide-by-zero.
    auto slope = [](float x0, float y0, float x1, float y1) -> float {
        const float dy = y1 - y0;
        return dy > 1e-6f ? (x1 - x0) / dy : 0.0f;
    };
    const float dxLong  = slope(ax, ay, cx, cy);
    const float dxUpper = slope(ax, ay, bx, by);
    const float dxLower = slope(bx, by, cx, cy);
    // Upper half: edges (A→C, A→B). Lower half: edges (A→C, B→C).
    for (int y = yTop; y < yBot; ++y) {
        const float yf = float(y);
        const float xLong = ax + dxLong * (yf - ay);
        float xOther;
        if (y < yMid) {
            xOther = ax + dxUpper * (yf - ay);
        } else {
            xOther = bx + dxLower * (yf - by);
        }
        int xL = int(std::ceil(std::min(xLong, xOther)));
        int xR = int(std::ceil(std::max(xLong, xOther)));
        if (xL < 0) xL = 0;
        if (xR > w) xR = w;
        if (xL >= xR) continue;
        std::memset(plane + size_t(y) * size_t(w) + size_t(xL), value, size_t(xR - xL));
    }
}

}  // namespace

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
    if (!sc || !g_gbuffer) return;
    auto &plane = g_gbuffer->mirrorId;
    if (plane.empty()) return;  // no mirror has activated the plane
    // Clear last frame's coverage. Cheap memset — 2 MB at 1080p.
    std::memset(plane.data(), 0, plane.size());
    // Use the engine surface size that matches the gbuffer plane.
    const int w = int(::XRes);
    const int h = int(::YRes);
    if (size_t(w) * size_t(h) > plane.size()) return;
    // Use a Z threshold safely above NZP so the per-vertex divide
    // doesn't blow up screen coords for verts grazing the near plane
    // (greets's NZP is 0.01, but z=0.18 still yields PX values >10×
    // screen width). Sutherland-Hodgman clips each wall triangle in
    // PROJECTION-PRE-DIVIDE space (TPos_AOS.x/.y already have FOVX/
    // CntrX baked in by Transform_Objects's M34; only the /z is
    // pending) against z >= kClipZ, then divides the clipped verts'
    // (x, y) by z to get screen coords.
    const float kClipZ = std::max(0.5f, sc->NZP * 5.0f);
    auto projectPreDivideToScreen = [](const Vector &vp, float &sx, float &sy) {
        const float invZ = 1.0f / vp.z;
        sx = vp.x * invZ;
        sy = vp.y * invZ;
    };
    for (const auto &m : mirrors) {
        if (m.id == 0 || m.wallFaces.empty()) continue;
        for (const Face *F : m.wallFaces) {
            if (!F || !F->A || !F->B || !F->C) continue;
            // Clip the (A,B,C) triangle in view-space against z >= kClipZ.
            // Up to 4 verts after a single-plane clip of a triangle.
            Vector clipped[4];
            int nc = 0;
            const Vector tri[3] = {
                F->A->TPos_AOS, F->B->TPos_AOS, F->C->TPos_AOS
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
            float sx[4], sy[4];
            for (int i = 0; i < nc; ++i) projectPreDivideToScreen(clipped[i], sx[i], sy[i]);
            for (int i = 1; i + 1 < nc; ++i) {
                StampTri2D(plane.data(), w, h,
                           sx[0], sy[0], sx[i], sy[i], sx[i+1], sy[i+1],
                           m.id);
            }
        }
    }
}

}  // namespace fds
