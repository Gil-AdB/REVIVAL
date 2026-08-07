#include "GreetsMirror.h"
#include "SpotlightCones.h"

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
#include <RENDER/DeferredCommon.h> // DeferredOverride + Render_DeferredLighting/VolumetricCones (deferred RTT)
#include <RENDER/Hdr.h>            // HDR-correct reflections: per-RTT-slot begin/tonemap
#include <Base/RenderContext.h>    // fds::RenderContext (deferred RTT bake)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <unordered_map>

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

// ── Mirror clone sub-spheres (see GreetsMirror.h) ────────────────────────
// Keyed on the clone TriMesh* (stable; a MeshRange pointer would dangle
// when the mirrors vector reallocates). Written at scene init (single
// threaded) and refreshed for DYNAMIC ranges only inside UpdateMirror,
// which runs on the tick thread before any pass reads it.
static std::unordered_map<const TriMesh*, std::vector<MirrorCloneSubSphere>>
    s_cloneSubSpheres;

const std::vector<MirrorCloneSubSphere> *MirrorCloneSubSpheres(const TriMesh *T)
{
    if (s_cloneSubSpheres.empty() || !T) return nullptr;
    auto it = s_cloneSubSpheres.find(T);
    return it == s_cloneSubSpheres.end() ? nullptr : &it->second;
}

// Clone mesh -> its mirror's wall faces (the window). Refreshed for ACTIVE
// mirrors each frame by UpdateAllMirrors; an inactive mirror's clone is
// HTrack_Visible-cleared, so a stale entry can never be read.
static std::unordered_map<const TriMesh*, std::vector<MirrorCloneWall>>
    s_cloneWalls;

const std::vector<MirrorCloneWall> *MirrorCloneWalls(const TriMesh *T)
{
    if (s_cloneWalls.empty() || !T) return nullptr;
    auto it = s_cloneWalls.find(T);
    return it == s_cloneWalls.end() ? nullptr : &it->second;
}

namespace {

// Recompute one sub-range's tight sphere from the clone's CURRENT
// (already re-mirrored) vertex positions. AABB centre + max radius —
// the same construction the Piramid chunk split uses, so the census
// measures what an actual split would deliver, not an optimistic bound.
void recomputeSubSphere(const TriMesh *MM, MirrorCloneSubSphere &s)
{
    if (!MM || !MM->Verts || s.vCount == 0) return;
    float mnx=1e30f, mny=1e30f, mnz=1e30f, mxx=-1e30f, mxy=-1e30f, mxz=-1e30f;
    for (uint32_t i = 0; i < s.vCount; ++i) {
        const Vector &p = MM->Verts[s.vStart + i].Pos;
        mnx = std::min(mnx, p.x); mxx = std::max(mxx, p.x);
        mny = std::min(mny, p.y); mxy = std::max(mxy, p.y);
        mnz = std::min(mnz, p.z); mxz = std::max(mxz, p.z);
    }
    s.ctr = { (mnx+mxx)*0.5f, (mny+mxy)*0.5f, (mnz+mxz)*0.5f };
    float r2 = 0.0f;
    for (uint32_t i = 0; i < s.vCount; ++i) {
        const Vector &p = MM->Verts[s.vStart + i].Pos;
        const float dx = p.x - s.ctr.x, dy = p.y - s.ctr.y, dz = p.z - s.ctr.z;
        r2 = std::max(r2, dx*dx + dy*dy + dz*dz);
    }
    s.radSq = r2;
}

// Build (or rebuild) the whole sub-sphere table for one clone.
//
// Two granularities, because they answer different questions:
//  * PER-SOURCE-MESH (cell <= 0): the ranges UpdateMirror already tracks.
//    This is what the cheapest possible split — emit one clone TriMesh per
//    source mesh — would deliver. Its ceiling is capped by the fact that
//    only the Piramid half of greets is chunked at source; the statues /
//    robot / ceiling arrive as a handful of room-sized ranges.
//  * SPATIAL CELLS (cell > 0): a near-cubic grid of `cell` world units over
//    the WHOLE clone, ignoring source-mesh boundaries — the granularity a
//    real spatial split of the clone would have. vStart is meaningless in
//    this mode (a cell's verts are not contiguous); only vCount + the
//    sphere are, which is all the census reads.
void buildSubSpheres(const TriMesh *MM, const std::vector<ClonedMeshRange> &ranges,
                     float cell)
{
    auto &v = s_cloneSubSpheres[MM];
    v.clear();
    if (cell <= 0.0f) {
        v.reserve(ranges.size());
        for (const auto &r : ranges) {
            MirrorCloneSubSphere s;
            s.vStart = r.vStart; s.vCount = r.vCount; s.dynamic = r.dynamic;
            recomputeSubSphere(MM, s);
            v.push_back(s);
        }
        return;
    }
    if (!MM || !MM->Verts || MM->VIndex == 0) return;
    float mnx=1e30f, mny=1e30f, mnz=1e30f, mxx=-1e30f, mxy=-1e30f, mxz=-1e30f;
    for (DWord i = 0; i < MM->VIndex; ++i) {
        const Vector &p = MM->Verts[i].Pos;
        mnx=std::min(mnx,p.x); mxx=std::max(mxx,p.x);
        mny=std::min(mny,p.y); mxy=std::max(mxy,p.y);
        mnz=std::min(mnz,p.z); mxz=std::max(mxz,p.z);
    }
    const int gx = std::max(1, int(std::ceil((mxx-mnx)/cell)));
    const int gy = std::max(1, int(std::ceil((mxy-mny)/cell)));
    const int gz = std::max(1, int(std::ceil((mxz-mnz)/cell)));
    auto cellOf = [&](const Vector &p) {
        const int ix = std::min(gx-1, std::max(0, int((p.x-mnx)/cell)));
        const int iy = std::min(gy-1, std::max(0, int((p.y-mny)/cell)));
        const int iz = std::min(gz-1, std::max(0, int((p.z-mnz)/cell)));
        return (int64_t(iz)*gy + iy)*gx + ix;
    };
    struct Acc { Vector mn, mx; uint32_t n; };
    std::unordered_map<int64_t, Acc> cells;
    for (DWord i = 0; i < MM->VIndex; ++i) {
        const Vector &p = MM->Verts[i].Pos;
        auto it = cells.find(cellOf(p));
        if (it == cells.end()) { cells.emplace(cellOf(p), Acc{p, p, 1}); continue; }
        Acc &a = it->second;
        a.mn.x=std::min(a.mn.x,p.x); a.mx.x=std::max(a.mx.x,p.x);
        a.mn.y=std::min(a.mn.y,p.y); a.mx.y=std::max(a.mx.y,p.y);
        a.mn.z=std::min(a.mn.z,p.z); a.mx.z=std::max(a.mx.z,p.z);
        ++a.n;
    }
    std::unordered_map<int64_t, size_t> slot;
    v.reserve(cells.size());
    for (const auto &kv : cells) {
        MirrorCloneSubSphere s;
        s.vCount = kv.second.n;
        s.ctr = { (kv.second.mn.x+kv.second.mx.x)*0.5f,
                  (kv.second.mn.y+kv.second.mx.y)*0.5f,
                  (kv.second.mn.z+kv.second.mx.z)*0.5f };
        slot[kv.first] = v.size();
        v.push_back(s);
    }
    // Second pass for the exact max-radius (same construction as the
    // Piramid chunk split: AABB centre, radius = farthest vertex).
    for (DWord i = 0; i < MM->VIndex; ++i) {
        const Vector &p = MM->Verts[i].Pos;
        MirrorCloneSubSphere &s = v[slot[cellOf(p)]];
        const float dx=p.x-s.ctr.x, dy=p.y-s.ctr.y, dz=p.z-s.ctr.z;
        s.radSq = std::max(s.radSq, dx*dx+dy*dy+dz*dz);
    }
}

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

// ── --greets_displace_flat_mirror: reflect the FLAT stone ────────────────────
// A mirror clone is MAIN-VIEW geometry, so Face_MainOnly (which only the
// offscreen passes honour) does not spare it the tessellated wall: measured at
// greets t=5780 the clone costs the displaced arm 11.40 ms/frame against 3.31 ms
// in the flat-POM arm, and pushes 42 870 clone faces while the direct view
// pushes 28 598 displaced ones. With the flag on the clone sources rooms/floor
// from the flat --greets_shadow_proxy mesh instead. Three coupled rules, and
// every clone loop (base count, base fill, compound count, compound fill) must
// apply the SAME three or the vert offsets stop agreeing:
//   1. skip Face_MainOnly faces,
//   2. include the Tri_OffscreenProxy mesh (normally excluded — it would
//      DOUBLE the wall),
//   3. skip a source mesh whose every face is skipped, VERTS AND ALL — else the
//      clone carries orphan vertices of exactly the class §9d removed, and the
//      transform-phase saving (the point of the flag) never materialises.
inline bool mirrorFlatStone() {
    return fds::FeatureFlags::greets_displace_flat_mirror();
}
// Rule 2. Off the flag, an offscreen proxy is never clone material.
inline bool mirrorSkipProxyMesh(const TriMesh *T) {
    if ((T->Flags & Tri_OffscreenProxy) && !mirrorFlatStone()) return true;
    // S1d-4: the prism skirt (flat '::prismside' quads, --pom_prism_flat) is
    // MAIN-VIEW patch geometry. Inside a mirror its clones composite through
    // the FORWARD path, where the late-built mesh has no lighting data
    // (renders black), and at the mirror's grazing angle the mirrored quads
    // occlude the mirrored walls — measured at t=5963: 91 398 black px with
    // the skirt cloned vs 776 without. Excluding it reproduces the mirror
    // look of the marching-quad arm exactly (those quads self-discarded to
    // invisibility inside mirrors anyway).
    if (T->FIndex > 0 && T->Faces && T->Faces[0].Txtr && T->Faces[0].Txtr->Name
        && std::strstr(T->Faces[0].Txtr->Name, "::prismside")) return true;
    return false;
}
// Rule 1.
inline bool mirrorSkipFace(const Face &F) {
    return mirrorFlatStone() && (F.Flags & Face_MainOnly);
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
    // Read once: BuildMirror runs at scene init and the vertex fill below is
    // the only consumer.
    const bool tightBSphere = fds::FeatureFlags::mirror_clone_tight_bsphere();
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
    //
    // FACELESS SOURCE MESHES ARE SKIPPED (`T->FIndex == 0`, here and in the
    // matching fill loop below, and in the compound-mirror pair). The clone's
    // face loop is `for (fi < T->FIndex)`, so a faceless source contributes
    // ZERO clone faces while its `T->VIndex` verts are still copied, mirrored,
    // re-mirrored every frame by UpdateMirror and re-transformed by every pass
    // that sees the clone. The greets Piramid chunk split retires the parent
    // mesh exactly that way (FIndex=0, arrays kept alive, 16 596 verts), so
    // every clone was carrying 16 596 orphan verts — measured 37.7 % of each
    // 44 012-vert greets clone, and the active clone is 53 % of main-view
    // transformed verts. Removing them cannot move a pixel: no face pointed
    // at them.
    //
    // Tri_OffscreenProxy SOURCE MESHES ARE SKIPPED TOO (2026-08-06, same four
    // loops). A proxy exists precisely because the real geometry it stands in
    // for is main-camera-only (Face_MainOnly): greets' flat stone proxy under
    // `--greets_displace --greets_shadow_proxy` substitutes ~226 flat faces for
    // ~87 k displaced wall faces in every OFFSCREEN pass. A mirror clone is not
    // an offscreen pass — it is main-view geometry — and the clone loop copies
    // faces with `CF = OF`, so it was copying BOTH the displaced faces (whose
    // Face_MainOnly is only honoured in offscreen passes) AND the flat proxy
    // standing in for them: two coincident copies of every wall inside the
    // reflection, Z-fighting. Measured at the t=5780 wall pose: main-view
    // fPushed 74 962 -> 75 110 with the proxy on, i.e. 148 duplicate clone
    // faces per frame. The proxy is never wanted here; the displaced original
    // is already in the clone.
    // THE predicate: does this source face become a clone face? Defined ONCE
    // and consumed by both the count loop (to size the vertex array) and the
    // fill loop (which caches it per face). The count/fill lockstep this file
    // keeps warning about cannot drift when there is only one copy of the test.
    auto cloneFaceLive = [&](const Face &OF, TriMesh *T) -> bool {
        if (isMirrorSurface(OF, T)) return false;
        if (!OF.A || !OF.B || !OF.C) return false;
        if (mirrorSkipFace(OF)) return false;   // rule 1: displaced detail, the flat proxy stands in
        // Only reflect faces IN FRONT of the mirror plane. A real mirror
        // reflects what's in front of it; faces on the plane (the
        // teleporter's coplanar emissive "screen emiter" glow) or behind it
        // (the room behind the wall) produce a degenerate/wrong reflection
        // that lands on top of the mirror panel. The coplanar yellow emitter
        // clone was the flat yellow wash filling the greets teleporter.
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
        return sd > 0.05f;   // on/behind the plane → no reflection
    };

    DWord totalVerts = 0, totalFaces = 0;
    for (Object *Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh) continue;
        if (isCloneMesh(Obj)) continue;
        TriMesh *T = (TriMesh*)Obj->Data;
        if (!T || !T->Verts || !T->Faces || T->FIndex == 0) continue;  // faceless: no clone face can reference its verts
        if (mirrorSkipProxyMesh(T)) continue;                          // offscreen stand-in: cloning it DOUBLES the wall (see the count-loop comment)
        // Rule 3 (--greets_displace_flat_mirror): count the faces FIRST and skip
        // the mesh outright if none survive, so a fully-displaced Piramid chunk
        // contributes no verts either. Textually mirrored in the fill loop.
        DWord meshFaces = 0;
        for (DWord fi = 0; fi < T->FIndex; ++fi) {
            if (isMirrorSurface(T->Faces[fi], T)) continue;
            if (!T->Faces[fi].A) continue;
            if (mirrorSkipFace(T->Faces[fi])) continue;
            ++meshFaces;
        }
        if (mirrorFlatStone() && meshFaces == 0) continue;   // flag-gated so the count is bit-identical off the flag
        // FACE count deliberately stays the loose one above (no plane-side
        // test): `totalFaces == 0` is an early-out that returns WITHOUT
        // reclaiming the mirror id, while the `fOfs == 0` early-out below does
        // reclaim it — so tightening this count could move which mirrors get
        // which id, and gb.mirrorId is a rendered value. Over-allocating faces
        // is what HEAD does; FIndex is shrunk to fOfs after the fill.
        totalFaces += meshFaces;
        // VERTEX count is EXACT: the clone carries only vertices a surviving
        // clone face references, so sizing this at T->VIndex would leave the
        // tail allocated and never written — 133 MB across the four greets
        // mirrors under --greets_displace, where 90 % of each pre-compaction
        // clone was orphans. No early-out reads it.
        {
            std::vector<uint8_t> used(size_t(T->VIndex), 0);
            for (DWord fi = 0; fi < T->FIndex; ++fi) {
                const Face &OF = T->Faces[fi];
                if (!cloneFaceLive(OF, T)) continue;
                used[size_t(OF.A - T->Verts)] = 1;
                used[size_t(OF.B - T->Verts)] = 1;
                used[size_t(OF.C - T->Verts)] = 1;
            }
            for (DWord vi = 0; vi < T->VIndex; ++vi) totalVerts += used[vi];
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
        if (!T || !T->Verts || !T->Faces || T->FIndex == 0) continue;  // faceless: no clone face can reference its verts
        if (mirrorSkipProxyMesh(T)) continue;                          // offscreen stand-in: cloning it DOUBLES the wall (see the count-loop comment)
        // Rule 3 — the count loop's mesh skip, textually mirrored. Must stay in
        // lockstep: this loop consumes the capacity that loop reserved.
        {
            DWord meshFaces = 0;
            for (DWord fi = 0; fi < T->FIndex; ++fi) {
                if (isMirrorSurface(T->Faces[fi], T)) continue;
                if (!T->Faces[fi].A) continue;
                if (mirrorSkipFace(T->Faces[fi])) continue;
                ++meshFaces;
            }
            if (mirrorFlatStone() && meshFaces == 0) continue;   // flag-gated so the count is bit-identical off the flag
        }
        const bool meshDyn = meshIsDynamic(Obj);
        // ── ORPHAN-FREE CLONE VERTICES ───────────────────────────────────
        // Decide which faces survive FIRST, then clone only the vertices a
        // surviving clone face actually references.
        //
        // The vertex fill used to copy EVERY vertex of every surviving source
        // mesh, while the face fill drops faces four ways (isMirrorSurface, a
        // null corner, --greets_displace_flat_mirror's Face_MainOnly skip, and
        // the "in FRONT of the plane" side test). Nothing fed those rejections
        // back into the vertex selection, so the clone carried vertices no
        // clone face could ever reference — the exact class
        // docs/VISIBILITY_PLAN.md 9d removed at MESH granularity, one level
        // down at VERTEX granularity, and it dominated the clone: measured at
        // greets t=5743 (--mirror_cull_census build, [MIRROR-ORPHAN]),
        // 245 890 of 272 751 clone verts (90.2 %) under --greets_displace with
        // its default --greets_displace_flat_mirror ON (the flag drops the
        // displaced FACES 90 890 -> 9 198 but left every displaced VERTEX in
        // the clone, so its stated transform-phase saving never materialised),
        // and 93.0 % of mirror 'P_TEXT.JPG#11' in the shipping flat arm, whose
        // 642 surviving faces reference 1 926 of 27 416 cloned vertices.
        // Orphans are transformed by every pass that sees the clone and
        // re-mirrored by UpdateMirror, and produce nothing: a vertex reaches a
        // pixel only through a Face.
        //
        // faceLive[] caches the SHARED cloneFaceLive predicate (defined once
        // above the count loop) so the face loop below need not re-derive it.
        std::vector<uint8_t> faceLive(size_t(T->FIndex), 0);
        for (DWord fi = 0; fi < T->FIndex; ++fi)
            faceLive[fi] = cloneFaceLive(T->Faces[fi], T) ? 1 : 0;
        // cloneOfSrc[srcVert] = clone-local offset, or kUnref for an orphan.
        constexpr uint32_t kUnref = 0xFFFFFFFFu;
        std::vector<uint32_t> cloneOfSrc(size_t(T->VIndex), kUnref);
        for (DWord fi = 0; fi < T->FIndex; ++fi) {
            if (!faceLive[fi]) continue;
            const Face &OF = T->Faces[fi];
            cloneOfSrc[size_t(OF.A - T->Verts)] = 0;
            cloneOfSrc[size_t(OF.B - T->Verts)] = 0;
            cloneOfSrc[size_t(OF.C - T->Verts)] = 0;
        }
        const DWord vStart = vOfs;
        for (DWord vi = 0; vi < T->VIndex; ++vi) {
            Vector localP = T->Verts[vi].Pos;
            Vector worldP;
            MatrixXVector(T->RotMat, &localP, &worldP);
            worldP.x += T->IPos.x; worldP.y += T->IPos.y; worldP.z += T->IPos.z;
            const Vector mirroredP = reflectPointAcross(worldP, N, d);
            // --mirror_clone_tight_bsphere: which vertices the clone's bounding
            // sphere spans.
            //   OFF (default): EVERY source vertex, orphan or not — bit-identical
            //     to the pre-compaction build, so the mesh cull's Tri_Inside /
            //     Tri_Ahead classification (and with it the clipped-vs-unclipped
            //     vertex path, and with it every pixel) cannot move.
            //   ON: only the vertices the clone actually keeps, which is the
            //     CORRECT sphere — the clone cannot draw a vertex it does not
            //     carry. This is what makes a clone cullable at all: mirror
            //     'P_TEXT.JPG#11' keeps 1 926 of 27 416 vertices in one corner of
            //     the room, yet its sphere today spans the whole mirrored room and
            //     no camera can reject it. Correct, not merely conservative — a
            //     clone it culls has no vertex in the frustum — but it CHANGES CULL
            //     OUTCOMES, so it is gated and measured rather than smuggled in.
            if (!tightBSphere || cloneOfSrc[vi] != kUnref) {
                bbMin.x = std::min(bbMin.x, mirroredP.x);
                bbMin.y = std::min(bbMin.y, mirroredP.y);
                bbMin.z = std::min(bbMin.z, mirroredP.z);
                bbMax.x = std::max(bbMax.x, mirroredP.x);
                bbMax.y = std::max(bbMax.y, mirroredP.y);
                bbMax.z = std::max(bbMax.z, mirroredP.z);
            }
            if (cloneOfSrc[vi] == kUnref) continue;   // orphan: no clone face reaches it
            cloneOfSrc[vi] = vOfs - vStart;           // clone-local offset for the face remap
            m.cloneSrcVert.push_back(vi);             // and the reverse map UpdateMirror needs
            MM->Verts[vOfs] = T->Verts[vi];
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
            ++vOfs;
        }
        for (DWord fi = 0; fi < T->FIndex; ++fi) {
            if (!faceLive[fi]) continue;   // the four rejects, decided once above
            Face &OF = T->Faces[fi];
            Face &CF = MM->Faces[fOfs];
            CF = OF;
            CF.A = MM->Verts + vStart + cloneOfSrc[size_t(OF.A - T->Verts)];
            CF.B = MM->Verts + vStart + cloneOfSrc[size_t(OF.C - T->Verts)];  // swap
            CF.C = MM->Verts + vStart + cloneOfSrc[size_t(OF.B - T->Verts)];  // swap
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
        // vCount is now the LIVE (compacted) count, not T->VIndex — the source
        // index of clone vertex vStart+k lives in m.cloneSrcVert[vStart+k].
        // A mesh whose every face was rejected contributes nothing at all.
        if (vOfs > vStart)
            m.meshRanges.push_back({T, vStart, vOfs - vStart, meshDyn});
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
    // Mirror clones are VIRTUAL reflection geometry — they exist only to be
    // drawn into the mirror surface, and their HTrack_Visible tracks whether
    // the mirror is in view (camera-dependent). They must NOT cast real
    // shadows: baking them into the omni shadow cubes made the dynamic-omni
    // shadow maps change with the camera (clone occluders appearing/vanishing
    // as mirrors came into view) → garbled, camera-position-dependent shadows.
    MM->Flags |= Tri_NoShadowCast;
    m.clonedVerts = int(vOfs);
    m.clonedFaces = int(fOfs);
    // The front-side skip can leave fewer faces than pre-counted, so
    // shrink FIndex to what we actually wrote — otherwise the tail
    // slots are uninitialised garbage that the rasterizer would draw.
    MM->FIndex = fOfs;
    // Same for the verts: the count loop reserves the conservative
    // sum-of-T->VIndex (it cannot run the plane-side test, which needs the
    // per-face centroid), and the fill writes only the referenced ones. The
    // tail is over-allocated, never written and never read — every consumer
    // bounds on VIndex.
    MM->VIndex = vOfs;

#if FDS_VIS_CENSUS
    // ORPHAN-CLONE-VERTEX census — the acceptance check for the compaction
    // above. Must now report 0 orphans; anything else means a face survived
    // whose vertices were not marked.
    {
        std::vector<uint8_t> used(size_t(MM->VIndex), 0);
        for (DWord fi = 0; fi < MM->FIndex; ++fi) {
            const Face &CF = MM->Faces[fi];
            if (CF.A) used[size_t(CF.A - MM->Verts)] = 1;
            if (CF.B) used[size_t(CF.B - MM->Verts)] = 1;
            if (CF.C) used[size_t(CF.C - MM->Verts)] = 1;
        }
        DWord live = 0;
        for (DWord i = 0; i < MM->VIndex; ++i) live += used[i];
        std::fprintf(stderr,
            "[MIRROR-ORPHAN '%s'] clone verts %u, referenced by a clone face %u, "
            "ORPHAN %u (%.1f%%)  [faces %u]\n",
            label, unsigned(MM->VIndex), unsigned(live),
            unsigned(MM->VIndex - live),
            100.0 * double(MM->VIndex - live) / double(std::max<DWord>(1, MM->VIndex)),
            unsigned(MM->FIndex));
    }
#endif

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

    // Per-source-mesh tight spheres for --mirror_cull_census. The bsphere
    // just stamped above is the ROOM — it can never be rejected by the
    // mesh-level cull; these are what a split would give the cull instead.
    // Gated: the build walks the clone's verts twice per mirror, and the
    // per-frame dynamic refresh below keys off a NON-empty table, so with
    // the census off nothing is built, nothing is looked up, nothing runs.
    if (fds::FeatureFlags::mirror_cull_census() > 0)
        buildSubSpheres(MM, m.meshRanges, fds::FeatureFlags::mirror_cull_census_cell());

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
        // Mirrored shadow sampling: the kernel shadows this clone via
        // the SOURCE's map at the reflected sample (zero extra bakes).
        clone->mirrorSrcOmni = srcO;
        clone->mirrorPlaneN  = N;
        clone->mirrorPlaneD  = d;
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
constexpr int kRttRes         = 256;  // initial/default surface edge (slots
                                      // are density-sized, see kRttMaxRes)
constexpr int kRttPerFrame    = 2;    // most-visible slots re-rendered per frame
constexpr int kRecurseMaxBakes = 128; // slice-2 tree runaway guard: total
                                      // offscreen bakes per frame (V^depth is
                                      // footprint-pruned but unbounded in
                                      // principle; slice 3 adds a real budget)

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
                              std::vector<MirrorRttSlot> *rttSlots,
                              const std::vector<std::string> *allowedMatNames,
                              bool byMatName)
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
        Vector   ctr;  // world-space face centroid (for spatial clustering)
    };
    std::vector<WallSample> samples;
    auto collect = [&](TriMesh *T, Face &F, const Vector &wN) {
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
        const Vector ctr{ (wA.x+wB.x+wC.x)/3.0f, (wA.y+wB.y+wC.y)/3.0f, (wA.z+wB.z+wC.z)/3.0f };
        samples.push_back({&F, T, u,
                           -(u.x*wA.x + u.y*wA.y + u.z*wA.z), area, ctr});
    };
    if (byMatName) walkWallFacesIf(sc, matNameSelector(textureFileName),   collect);
    else           walkWallFacesIf(sc, textureNameSelector(textureFileName), collect);
    if (samples.empty()) {
        std::fprintf(stderr, "[MIRROR-CLUSTER '%s'] no faces found\n",
                     textureFileName);
        return 0;
    }

    // Greedy seed clustering: same plane = normals within ~18° AND
    // offsets within half a world unit, AND spatially near the seed.
    // The proximity term is essential: coplanarity ALONE merges faces
    // from DIFFERENT fixtures that happen to share a plane (e.g. a
    // screen box's thin +z side cap and a separate screen across the
    // room, both facing +z at the same z). Merged, the cluster looks
    // like neither a clean panel nor a clean strip, so the per-cluster
    // display tests below misjudge it (the cap's mask carved the wall =
    // the black strip; rejecting the merged cluster killed the real
    // screen with it). A single fixture's faces sit within a few units;
    // separate fixtures are far. kFixtureRadius is generous enough to
    // hold one panel together (its tris' centroids are ≈diagonal/3
    // apart) yet far below inter-fixture spacing.
    constexpr float kNormalDot     = 0.95f;
    constexpr float kPlaneDistEps  = 0.5f;
    constexpr float kFixtureRadius = 12.0f;
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
            const float ddx = samples[i].ctr.x - samples[j].ctr.x;
            const float ddy = samples[i].ctr.y - samples[j].ctr.y;
            const float ddz = samples[i].ctr.z - samples[j].ctr.z;
            if (ddx*ddx + ddy*ddy + ddz*ddz > kFixtureRadius*kFixtureRadius) continue;
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
    // Tunable (default 1.5): drops the ~1.0-area central column panels + ~0.30
    // box edge strips — barely-visible half-silvered screens not worth an RTT
    // slot. Only end-screen CLONES (>=2.0) survive as first-order mirrors;
    // second-order/recursive RTT is built separately. Set 0.2 to re-include.
    const float kMinRttArea = fds::FeatureFlags::greets_mirror_rtt_min_area();
    // Recursive mirrors are built on first-order RTT panels (see the
    // verdict demotion below), so --mirror-recurse-depth>0 implies the
    // RTT path even without an explicit --mirror-rtt.
    const bool recurse = fds::FeatureFlags::mirror_recurse_depth() > 0;
    const bool wantRtt =
        rttSlots && (fds::FeatureFlags::mirror_rtt() || recurse);

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
        // ── Display-face discrimination ──────────────────────────────
        // A screen box is a thin slab of half-silvered glass: the FRONT
        // is the display, but the back + 4 side caps share the material
        // and each forms its own coplanar cluster. Those non-display
        // faces should NOT be mirrors — a side cap seen edge-on reflects
        // black and its mask carves the wall behind (the greets black
        // strip); the back is mounted against the wall. Discriminate by:
        //   (1) ASPECT — a side cap is a long thin strip (depth×height,
        //       ~40:1); a display is roughly rectangular (~1:1).
        //   (2) OPEN SPACE on the reflecting (+normal) side — a real
        //       display faces the room; a back cap faces the wall it's
        //       mounted on (opaque geometry within ~2 units along +cN).
        Vector axU, axV; planeBasis(cN, axU, axV);
        float bu0=1e30f,bu1=-1e30f,bv0=1e30f,bv1=-1e30f;
        Vector cCtr{0,0,0}; int cNv=0;
        const char *cMatName = "?";
        for (size_t i = 0; i < samples.size(); ++i) {
            if (clusterOf[i] != c) continue;
            const WallSample &ws = samples[i];
            if (cMatName[0]=='?' && ws.F->Txtr && ws.F->Txtr->Name) cMatName = ws.F->Txtr->Name;
            const Vertex *vs[3] = { ws.F->A, ws.F->B, ws.F->C };
            for (int k = 0; k < 3; ++k) {
                Vector wp = vs[k]->Pos;
                if (ws.T) { Vector lp = wp; MatrixXVector(ws.T->RotMat, &lp, &wp); wp += ws.T->IPos; }
                const float pu = wp.x*axU.x + wp.y*axU.y + wp.z*axU.z;
                const float pv = wp.x*axV.x + wp.y*axV.y + wp.z*axV.z;
                bu0=std::min(bu0,pu); bu1=std::max(bu1,pu);
                bv0=std::min(bv0,pv); bv1=std::max(bv1,pv);
                cCtr.x+=wp.x; cCtr.y+=wp.y; cCtr.z+=wp.z; ++cNv;
            }
        }
        if (cNv) { cCtr.x/=cNv; cCtr.y/=cNv; cCtr.z/=cNv; }
        const float exU = bu1-bu0, exV = bv1-bv0;
        const float exMin = std::min(exU,exV), exMax = std::max(exU,exV);
        const float aspect = (exMin > 1e-3f) ? exMax/exMin : 1e9f;
        // Short ray from just off the surface along +cN (the reflecting
        // side). Hit on opaque, non-clone, non-glass geometry within
        // kFaceWallDist ⇒ the face is mounted against a wall (a back).
        bool facesWall = false;
        {
            const float kFaceWallDist = 2.0f;
            const Vector O{ cCtr.x+cN.x*0.2f, cCtr.y+cN.y*0.2f, cCtr.z+cN.z*0.2f };
            for (Object *O2 = sc->ObjectHead; O2 && !facesWall; O2 = O2->Next) {
                if (O2->Type != Obj_TriMesh || isCloneMesh(O2)) continue;
                TriMesh *T2 = (TriMesh*)O2->Data; if (!T2 || !T2->Faces) continue;
                for (DWord fi = 0; fi < T2->FIndex; ++fi) {
                    Face &F = T2->Faces[fi]; if (!F.A||!F.B||!F.C) continue;
                    if (F.Txtr && (F.Txtr->Flags & Mat_Transparent)) continue; // skip glass
                    auto wp=[&](const Vertex*v){ Vector lp=v->Pos,w; MatrixXVector(T2->RotMat,&lp,&w); w+=T2->IPos; return w; };
                    const Vector A=wp(F.A),B=wp(F.B),C=wp(F.C);
                    const Vector e1{B.x-A.x,B.y-A.y,B.z-A.z}, e2{C.x-A.x,C.y-A.y,C.z-A.z};
                    const Vector pvv{cN.y*e2.z-cN.z*e2.y, cN.z*e2.x-cN.x*e2.z, cN.x*e2.y-cN.y*e2.x};
                    const float det=e1.x*pvv.x+e1.y*pvv.y+e1.z*pvv.z;
                    if (det>-1e-6f && det<1e-6f) continue;
                    const float inv=1.0f/det;
                    const Vector tv{O.x-A.x,O.y-A.y,O.z-A.z};
                    const float uu=(tv.x*pvv.x+tv.y*pvv.y+tv.z*pvv.z)*inv; if (uu<0||uu>1) continue;
                    const Vector qv{tv.y*e1.z-tv.z*e1.y, tv.z*e1.x-tv.x*e1.z, tv.x*e1.y-tv.y*e1.x};
                    const float vv=(cN.x*qv.x+cN.y*qv.y+cN.z*qv.z)*inv; if (vv<0||uu+vv>1) continue;
                    const float dist=(e2.x*qv.x+e2.y*qv.y+e2.z*qv.z)*inv;
                    if (dist>0.05f && dist<kFaceWallDist) { facesWall=true; break; }
                }
            }
        }
        constexpr float kMaxDisplayAspect = 6.0f;
        const char *verdict = "built";
        // Explicit per-surface mirror designation (authored on the LWO surface
        // name). When an allowlist is supplied, a cluster is a mirror candidate
        // ONLY if its surface is named in it — no area heuristic decides IF a
        // screen reflects, only WHICH face of a marked screen's box does.
        bool nameAllowed = true;
        if (allowedMatNames) {
            nameAllowed = false;
            for (const std::string &nm : *allowedMatNames)
                if (nm == cMatName) { nameAllowed = true; break; }
        }
        if (!nameAllowed) {
            verdict = "unmarked";
            ++skippedSlivers;
        } else if (clusterArea < kMinRttArea) {
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
        // Only override a face that would otherwise be a display (built
        // or rtt). 'edge'/'occluded' start with neither 'r' nor 'b', so
        // both the RTT path (verdict[0]=='r') and the clone path
        // (verdict[0]!='b' → continue) skip them.
        // With spatial clustering each cluster is one fixture, so the
        // tests are clean: an isolated thin side cap reads as a high
        // aspect strip; a back reads as facing a wall. (No fill-ratio
        // test — that only mattered when caps merged with real panels.)
        if (verdict[0] == 'b' || verdict[0] == 'r') {
            if (aspect > kMaxDisplayAspect)      verdict = "edge";      // thin box-side cap
            else if (facesWall)                  verdict = "occluded";  // back, mounted on wall
        }
        // Recursive mirrors need first-order RTT panels: the RTT bake
        // hides every CLONE mesh (so a clone can't appear inside another
        // mirror's texture → depth-0 dead end), but RTT panels are real
        // faces retargeted to an RTT material and stay visible. Demote a
        // would-be clone to RTT when --mirror-recurse-depth>0 so mirrors
        // reflect each other across the N-pass bake. Edge/occluded/sliver
        // verdicts are left alone (still not mirrors).
        if (recurse && verdict[0] == 'b') verdict = "rtt";
        std::fprintf(stderr,
            "[CLUSTER %2d] mat='%s' ctr=(%.0f,%.0f,%.0f) N=(%5.2f,%5.2f,%5.2f) d=%8.3f faces=%zu "
            "area=%6.2f ext=%.2fx%.2f aspect=%.1f facesWall=%d -> %s\n",
            c, cMatName, cCtr.x, cCtr.y, cCtr.z, cN.x, cN.y, cN.z, cD, members.size(), clusterArea,
            exU, exV, aspect, (int)facesWall, verdict);
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
            // Texel density proportional to the panel's WORLD size
            // (capped 512x512). The old constant 64K budget spread the
            // same texels over any panel area — big panels rendered
            // visibly blockier than small ones.
            {
                const float density = fds::FeatureFlags::mirror_rtt_density();
                auto pow2of = [](float v, int lo, int hi) {
                    int p = lo;
                    while (p < hi && float(p * 2) <= v * 1.5f) p <<= 1;
                    return p;
                };
                slot.texW = pow2of((slot.u1 - slot.u0) * density, 64, 512);
                slot.texH = pow2of((slot.v1 - slot.v0) * density, 64, 512);
                while (slot.texW * slot.texH > 512 * 512) {
                    if (slot.texW >= slot.texH && slot.texW > 64) slot.texW >>= 1;
                    else if (slot.texH > 64) slot.texH >>= 1;
                    else break;
                }
            }
            std::vector<uint32_t> black(
                size_t(slot.texW) * size_t(slot.texH), 0xFF000000u);
            slot.mat = Materialize(black.data(), slot.texW, slot.texH);
            slot.texWMax = slot.texW;   // buffer is allocated at this size
            slot.texHMax = slot.texH;   // (adaptive sizing only shrinks)
            slot.mat->Luminosity = 1.0f;   // texture holds final colors
            slot.mat->Diffuse    = 0.0f;
            slot.mat->Specular   = 0.0f;
            // Glass reflects on BOTH sides: without this the back view
            // backface-culls into a hole showing the box interior
            // (mirrored text of the far sheet). The per-frame job
            // renders the side the camera is actually on.
            slot.mat->Flags |= Mat_TwoSided;
            if (recurse) {
                // Recursive mirrors must be OPAQUE. The RTT bake fills only
                // the OPAQUE G-buffer (MekaleleFillRegionInline), so a
                // transparent panel is invisible INSIDE another panel's bake
                // — no mirror ever sees another and the tunnel never
                // deepens. A solid opaque mirror renders into every bake, so
                // panel A picks up panel B's texture on the next pass.
            } else {
                // Mat_Transparent: the panel is a SEMI-TRANSPARENT display
                // — it rides the xpar peel and the deferred transparent
                // kernel composites texel + behind/2 (XparBlendAlpha=0 →
                // the legacy glass formula the original screens used), so
                // free-standing column panels show the scene through the
                // glass under the text + reflection.
                slot.mat->Flags |= Mat_Transparent;
                // Blend ratio: panel*a + behind*(1-a) via the kernel's
                // XparBlendAlpha lerp. Runtime knob --mirror-rtt-alpha;
                // the additive texel+behind/2 default read too windowy.
                slot.mat->XparBlendAlpha =
                    fds::FeatureFlags::mirror_rtt_alpha();
            }
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
                // Recursive panels are PURE mirrors: leave textTex unset so
                // the bake's half-silvered composite (texel = baseTex +
                // reflection*gain) never runs. That composite is the washout:
                // it ADDS the panel's base texture at full strength every
                // bounce (v' = base + 0.55*v ⇒ fixed point past white). The
                // greets text-over-reflection look under recursion is the
                // slice-3 "wire the composite through the recursion" item.
                if (!recurse && protoF && protoF->Txtr && protoF->Txtr->Txtr) {
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
                if (!T || !T->Verts || !T->Faces || T->FIndex == 0) continue;  // faceless: no clone face can reference its verts
                if (mirrorSkipProxyMesh(T)) continue;                  // offscreen stand-in: cloning it DOUBLES the wall (see the count-loop comment)
                // Rule 3, compound form (textually mirrored in the fill loop).
                DWord meshFaces = 0;
                for (DWord fi = 0; fi < T->FIndex; ++fi) {
                    if (!T->Faces[fi].A) continue;
                    if (isAnyBaseWallMat(T->Faces[fi].Txtr)) continue;
                    if (mirrorSkipFace(T->Faces[fi])) continue;
                    ++meshFaces;
                }
                if (mirrorFlatStone() && meshFaces == 0) continue;   // flag-gated so the count is bit-identical off the flag
                totalV += T->VIndex;
                totalF += meshFaces;
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
                if (!T || !T->Verts || !T->Faces || T->FIndex == 0) continue;  // faceless: no clone face can reference its verts
                if (mirrorSkipProxyMesh(T)) continue;                  // offscreen stand-in: cloning it DOUBLES the wall (see the count-loop comment)
                // Rule 3 — the compound count loop's mesh skip, in lockstep.
                {
                    DWord meshFaces = 0;
                    for (DWord fi = 0; fi < T->FIndex; ++fi) {
                        if (!T->Faces[fi].A) continue;
                        if (isAnyBaseWallMat(T->Faces[fi].Txtr)) continue;
                        if (mirrorSkipFace(T->Faces[fi])) continue;
                        ++meshFaces;
                    }
                    if (mirrorFlatStone() && meshFaces == 0) continue;   // flag-gated so the count is bit-identical off the flag
                }
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
                    if (mirrorSkipFace(OF)) continue;   // rule 1: displaced detail, the flat proxy stands in
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
        // r.vCount is the COMPACTED clone count; the source index of clone
        // vertex r.vStart+vi is m.cloneSrcVert[r.vStart+vi] (the clone carries
        // only vertices a surviving clone face references). Clamping against
        // T->VIndex as this loop used to do would be wrong now — the counts are
        // unrelated — so bound on the index array instead.
        if (m.cloneSrcVert.size() < size_t(r.vStart) + size_t(r.vCount)) continue;
        const DWord n = r.vCount;
        // Forward-shaded meshes (Tri_Noshading) own their per-vertex
        // colour: the disco ball rewrites LR/LG/LB every tick (the
        // shimmer glint). Re-mirroring only Pos/N/Tangent left the clone
        // ball's sparkle frozen at its build-time shade, so the mirror
        // ball looked dead/out-of-step next to the spinning original.
        // Copy the lit colour through for those meshes too — it's a
        // scalar shade, no reflection needed. Deferred meshes (the
        // robot) get their mirror shade from the kernel, not the vertex
        // colour, so they're skipped to avoid needless writes.
        const bool copyColour = (T->Flags & Tri_Noshading) != 0;
        for (DWord vi = 0; vi < n; ++vi) {
            const uint32_t svi = m.cloneSrcVert[size_t(r.vStart) + vi];
            if (svi >= T->VIndex) continue;
            Vector localP = T->Verts[svi].Pos;
            Vector worldP;
            MatrixXVector(T->RotMat, &localP, &worldP);
            worldP.x += T->IPos.x; worldP.y += T->IPos.y; worldP.z += T->IPos.z;
            m.cloneMesh->Verts[r.vStart + vi].Pos = reflectPointAcross(worldP, N, d);
            // Directions: full composed RotMat then reflect — N and
            // Tangent both, every frame, so the robot's animated TBN
            // stays correct in the mirror. (See the init-fill comment
            // for why NOT UnscaledRotMat, and for the B = N × T
            // handedness caveat.)
            Vector localN = T->Verts[svi].N;
            Vector worldN;
            MatrixXVector(T->RotMat, &localN, &worldN);
            m.cloneMesh->Verts[r.vStart + vi].N = reflectDirAcross(worldN, N);
            Vector localT = T->Verts[svi].Tangent;
            Vector worldT;
            MatrixXVector(T->RotMat, &localT, &worldT);
            m.cloneMesh->Verts[r.vStart + vi].Tangent = reflectDirAcross(worldT, N);
            if (copyColour) {
                Vertex       &cv = m.cloneMesh->Verts[r.vStart + vi];
                const Vertex &sv = T->Verts[svi];
                cv.LR = sv.LR; cv.LG = sv.LG; cv.LB = sv.LB;
            }
        }
    }
    // Sub-spheres of the ranges that just moved (--mirror_cull_census only;
    // the map is empty and this is a single empty-container probe otherwise).
    // Static ranges re-mirror to identical positions, so their spheres from
    // BuildMirror stay exact — same reasoning as the `dynamic` skip above.
    // (Skipped entirely in SPATIAL-CELL census mode: a cell's verts are not
    // a contiguous range, so there is nothing to refresh in place. The cells
    // are built once from the primed clone; only the robot moves and it is a
    // small fraction of the clone, so the census stays representative.)
    if (!s_cloneSubSpheres.empty()
        && fds::FeatureFlags::mirror_cull_census_cell() <= 0.0f) {
        auto it = s_cloneSubSpheres.find(m.cloneMesh);
        if (it != s_cloneSubSpheres.end()) {
            for (auto &s : it->second)
                if (full || s.dynamic) recomputeSubSphere(m.cloneMesh, s);
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

// ── Bounce cones ────────────────────────────────────────────────────
// A real beam striking a mirror window throws a reflected beam back
// into the room. Each active (real spot × mirror) pair whose beam
// axis hits inside the mirror's window AABB activates a pool spot at
// the mirrored position/direction: a plain mirrorId=0 spot, so it
// lights REAL pixels through every existing path (tile lists, cone
// cull, surface kernel → bounced dot pools on the floor; cone pass →
// the visible shaft). Omni_BounceCone makes the cone pass clamp its
// chord to the camera side of the glass (the apex sits behind it).
static constexpr int kBouncePool = 12;
static constexpr float kBounceReflectance = 0.55f;

static void UpdateBounceSpots(Scene *sc, std::vector<Mirror> &mirrors)
{
    static std::vector<Omni*> pool;
    if (!fds::FeatureFlags::mirror_bounce()) {
        for (Omni *o : pool) o->ISize = 0.0f;
        return;
    }
    if (pool.empty()) {
        for (int i = 0; i < kBouncePool; ++i) {
            Omni *o = MakeSpotLight(sc, 255, 255, 255, 0.0f, 1.0f,
                                    {0, -100, 0}, {0, -1, 0},
                                    2.0f, 6.0f, 0, false);
            o->ISize = 0.0f;
            pool.push_back(o);
        }
    }
    int used = 0;
    const bool bprobe = std::getenv("FDS_BOUNCE_PROBE") != nullptr;
    int cand = 0, planeHits = 0, winHits = 0, mirrorsSeen = 0;
    for (Mirror &m : mirrors) {
        // NOT gated on m.active: the bounce shaft lives in the ROOM —
        // it stays visible when its mirror is off-screen behind you.
        // Plane + window are static; nothing here needs the clone
        // machinery the visibility gate exists to throttle.
        if (!m.plane.valid || m.parentMirrorId != 0) continue;
        if (!m.windowValid) {
            // Lazy one-time window AABB from the wall faces (static).
            Vector mn{ 1e30f, 1e30f, 1e30f}, mx{-1e30f,-1e30f,-1e30f};
            for (size_t i = 0; i < m.wallFaces.size(); ++i) {
                const Face *F = m.wallFaces[i];
                TriMesh *T = m.wallFaceMeshes[i];
                if (!F || !T) continue;
                for (const Vertex *v : { F->A, F->B, F->C }) {
                    Vector lp = v->Pos, wp;
                    MatrixXVector(T->RotMat, &lp, &wp);
                    wp += T->IPos;
                    mn.x = std::min(mn.x, wp.x); mx.x = std::max(mx.x, wp.x);
                    mn.y = std::min(mn.y, wp.y); mx.y = std::max(mx.y, wp.y);
                    mn.z = std::min(mn.z, wp.z); mx.z = std::max(mx.z, wp.z);
                }
            }
            if (mx.x >= mn.x) { m.windowMin = mn; m.windowMax = mx; }
            m.windowValid = true;
        }
        if (m.windowMax.x < m.windowMin.x) continue;
        ++mirrorsSeen;
        const Vector &N = m.plane.N;
        const float   d = m.plane.d;
        for (Omni *O = sc->OmniHead; O && used < kBouncePool; O = O->Next) {
            if (O->Type != Light_SpotLight) continue;
            if (!(O->Flags & Omni_Active)) continue;
            if (!(O->Flags & Omni_ForceVolCone)) continue;  // beams only
            if (O->mirrorId != 0) continue;                 // not clones
            if (O->Flags & Omni_BounceCone) continue;       // not pool spots
            if (O->ISize <= 0.0f) continue;
            ++cand;
            // Beam axis vs mirror plane.
            const float ND = N.x*O->IDir.x + N.y*O->IDir.y + N.z*O->IDir.z;
            if (std::fabs(ND) < 1e-6f) continue;
            const float NP = N.x*O->IPos.x + N.y*O->IPos.y + N.z*O->IPos.z + d;
            const float t  = -NP / ND;
            if (t <= 0.0f || t >= O->IRange) continue;
            ++planeHits;
            const Vector hit{ O->IPos.x + O->IDir.x * t,
                              O->IPos.y + O->IDir.y * t,
                              O->IPos.z + O->IDir.z * t };
            // Pad by the beam's radius at the hit distance: the beam
            // is a cone, not an axis — a window clipped by the cone
            // edge still throws a (partial) bounce. Without this the
            // activation window is a few degrees of azimuth and the
            // effect reads as rare random flashes.
            const float sinO = std::sqrt(std::max(0.0f,
                1.0f - O->FallOff * O->FallOff));
            const float kPad = 0.4f + t * sinO;
            const bool inWin =
                !(hit.x < m.windowMin.x - kPad || hit.x > m.windowMax.x + kPad ||
                  hit.y < m.windowMin.y - kPad || hit.y > m.windowMax.y + kPad ||
                  hit.z < m.windowMin.z - kPad || hit.z > m.windowMax.z + kPad);
            if (!inWin) continue;
            ++winHits;
            Omni *b = pool[used++];
            b->IPos   = reflectPointAcross(O->IPos, N, d);
            b->IDir   = reflectDirAcross(O->IDir, N);
            b->L      = O->L;
            b->ISize  = O->ISize * fds::FeatureFlags::mirror_bounce_gain();
            // Range stretched past the source's: at physical range the
            // surviving stub past the glass is (range − distance-to-
            // mirror) ≈ a few units and doesn't read as a shaft.
            b->IRange = O->IRange * fds::FeatureFlags::mirror_bounce_range();
            b->rRange = 1.0f / b->IRange;
            b->HotSpot = O->HotSpot;
            b->FallOff = O->FallOff;
            // Inherit the source beam's authored gain (0 = unset → 1.0 at
            // SoA build) so a dimmed authored beam bounces dimmed too.
            b->VolBeamGain = O->VolBeamGain;
            b->Flags  = Omni_Active | Omni_ForceVolCone | Omni_BounceCone;
            b->mirrorId = 0;
            b->mirrorSrcOmni = O;
            b->mirrorPlaneN  = N;
            b->mirrorPlaneD  = d;
            // Window AABB for the surface kernel's portal test: the
            // bounce only lights room surfaces reachable through this
            // window, not "through" the surrounding wall.
            b->mirrorWinMin  = m.windowMin;
            b->mirrorWinMax  = m.windowMax;
            // Probe: dump each activated bounce spot's pose so a headless
            // camera can be aimed at the lit floor pool. FDS_BOUNCE_PROBE=1.
            if (std::getenv("FDS_BOUNCE_PROBE")) {
                std::fprintf(stderr,
                    "[BOUNCE] t=%d spot mir=%d pos=(%.2f,%.2f,%.2f) dir=(%.2f,%.2f,%.2f) "
                    "range=%.1f size=%.2f planeN=(%.2f,%.2f,%.2f) d=%.2f "
                    "win=[(%.2f,%.2f,%.2f)..(%.2f,%.2f,%.2f)]\n",
                    (int)Timer, (int)m.id, b->IPos.x, b->IPos.y, b->IPos.z,
                    b->IDir.x, b->IDir.y, b->IDir.z, b->IRange, b->ISize,
                    N.x, N.y, N.z, d,
                    m.windowMin.x, m.windowMin.y, m.windowMin.z,
                    m.windowMax.x, m.windowMax.y, m.windowMax.z);
            }
        }
    }
    for (int i = used; i < int(pool.size()); ++i) pool[i]->ISize = 0.0f;
    if (bprobe) {
        std::fprintf(stderr,
            "[BOUNCE] t=%d mirrors=%d cand=%d planeHits=%d winHits=%d used=%d\n",
            (int)Timer, mirrorsSeen, cand, planeHits, winHits, used);
    }
}

void UpdateAllMirrors(Scene *sc, std::vector<Mirror> &mirrors)
{
    int nActive = 0;
    {
    ScopedMirrorMs _t(&g_mirrorProf.updMs);
    for (auto &m : mirrors) {
        // Shattered: permanently closed. Hide the clone mesh + clone
        // flares and skip the re-mirror — the falling shards replace it.
        if (m.broken) {
            m.active = false;
            if (m.cloneMesh) m.cloneMesh->Flags &= ~HTrack_Visible;
            for (auto &c : m.omniClones)
                if (c.mirrorOmni) c.mirrorOmni->ISize = 0.0f;
            continue;
        }
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
        // --mirror_cull_census window ceiling: publish this mirror's wall
        // faces against its clone. Gated on the census flag so the shipping
        // path never touches the map (a few pointer copies for 4 mirrors
        // even when it does).
        if (fds::FeatureFlags::mirror_cull_census() > 0 && m.cloneMesh) {
            auto &w = s_cloneWalls[m.cloneMesh];
            w.clear();
            if (act) {
                w.reserve(m.wallFaces.size());
                for (size_t i = 0; i < m.wallFaces.size(); ++i)
                    w.push_back({ m.wallFaces[i],
                                  i < m.wallFaceMeshes.size() ? m.wallFaceMeshes[i] : nullptr });
            }
        }
    }
    // Bounce cones: after mirror activity + clone updates, before the
    // render consumes light poses.
    UpdateBounceSpots(sc, mirrors);
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
                // Texel density proportional to the window's world
                // size (capped 512x512) — see the order-1 site.
                {
                const float density = fds::FeatureFlags::mirror_rtt_density();
                auto pow2of = [](float v, int lo, int hi) {
                    int p = lo;
                    while (p < hi && float(p * 2) <= v * 1.5f) p <<= 1;
                    return p;
                };
                slot.texW = pow2of((slot.u1 - slot.u0) * density, 64, 512);
                slot.texH = pow2of((slot.v1 - slot.v0) * density, 64, 512);
                while (slot.texW * slot.texH > 512 * 512) {
                    if (slot.texW >= slot.texH && slot.texW > 64) slot.texW >>= 1;
                    else if (slot.texH > 64) slot.texH >>= 1;
                    else break;
                }
            }
                black.assign(size_t(slot.texW) * size_t(slot.texH),
                             0xFF000000u);
                slot.mat = Materialize(black.data(), slot.texW, slot.texH);
                slot.texWMax = slot.texW;   // buffer allocated at this size
                slot.texHMax = slot.texH;   // (adaptive sizing only shrinks)
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

// Number of RTT jobs rendered last frame. The tick's forward
// Lighting() exists (in deferred mode) solely to feed vertex colors
// to this pass — when no slot re-rendered, the next tick can skip it.
int g_rttJobsLastFrame = 0;

Camera MirrorReflectedCamera(const Camera &src, const Vector &N, float d)
{
    Camera out = src;                       // copy FOV, roll, splines, etc.
    out.ISource = reflectPointAcross(src.ISource, N, d);
    // Reflect each basis row (a direction). Reflection has det = -1, so the
    // basis becomes left-handed — the caller renders with inverted cull.
    for (int r = 0; r < 3; ++r) {
        const Vector row{ src.Mat[r][0], src.Mat[r][1], src.Mat[r][2] };
        const Vector rr = reflectDirAcross(row, N);
        out.Mat[r][0] = rr.x; out.Mat[r][1] = rr.y; out.Mat[r][2] = rr.z;
    }
    return out;
}

void RenderSecondOrderMirrors(Scene *sc, std::vector<Mirror> &mirrors,
                              std::vector<MirrorRttSlot> &slots)
{
    ScopedMirrorMs _t(&g_mirrorProf.rttMs);
    g_rttJobsLastFrame = 0;
    if (!sc || slots.empty()) return;
    const Camera *mainCam = ::View ? ::View : sc->CameraHead;
    if (!mainCam) return;
    // Recursive reflection depth (0 = legacy scheduled path). Read once up
    // front — the job selection, the per-frame cap, and the bake-pass count
    // all branch on it.
    const int recurseDepth = fds::FeatureFlags::mirror_recurse_depth();
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
        float          area;   // priority: projected px² × staleness (ranking)
        Vector         camPos; // C_B
        float          dist;   // C_B distance to B's plane
        bool           backSide = false;  // order-1: camera behind plane
        float          sw = 0, sh = 0;    // raw on-screen footprint px (adaptive res)
        bool           offscreen = false; // recurse full-frame fallback (no
                                          // MAIN-screen footprint) — flat-pass
                                          // only; the slice-2 tree skips these
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
        bool offscreen = false;
        if (!anyAhead) {
            if (recurseDepth <= 0) continue;
            offscreen = true;
            // Recurse: bake even when the panel has no MAIN-screen footprint.
            // A mirror is often visible ONLY inside another mirror's
            // reflection (the far wall of an infinity tunnel sits behind the
            // camera), yet its texture must be current for that reflection to
            // resolve. Full-frame footprint → bakes at full resolution.
            bx0 = 0.0f; by0 = 0.0f;
            bx1 = float(::XRes); by1 = float(::YRes);
        }
        bx0 = std::max(bx0, 0.0f); by0 = std::max(by0, 0.0f);
        bx1 = std::min(bx1, float(::XRes)); by1 = std::min(by1, float(::YRes));
        const float area = (bx1 - bx0) * (by1 - by0);
        if (area <= 1.0f && recurseDepth <= 0) continue;
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
        jobs.push_back({ &s, priority, cb, -sd, backSide,
                         bx1 - bx0, by1 - by0, offscreen });
    }
    if (jobs.empty()) return;
    std::sort(jobs.begin(), jobs.end(),
              [](const Job &a, const Job &b) { return a.area > b.area; });
    // Recursive mode bakes EVERY slot each pass (a mirror must be current in
    // every other mirror's view); the per-frame cap + staleness round-robin
    // only apply to the legacy scheduled path.
    if (recurseDepth <= 0 && int(jobs.size()) > kRttPerFrame)
        jobs.resize(kRttPerFrame);
    g_rttJobsLastFrame = int(jobs.size());
    g_mirrorProf.rttJobsSum += int(jobs.size());

    // ── Offscreen surface (allocated once; CITY cube-bake pattern).
    // Buffers sized for the constant 64K-texel budget; X/Y/BPSL (and
    // the YOffs table) are re-stamped per job to the slot's aspect-
    // matched dimensions.
    static VESA_Surface s_rttSurf = {};
    static bool s_rttInit = false;
    constexpr int kRttMaxRes = 512;  // density cap in the slot sizing
    constexpr int kRttTexels = kRttMaxRes * kRttMaxRes;
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

    // --shard-deferred also routes the RTT through the deferred kernel +
    // cone pass (shadows + disco beams in the recursive mirror, matching the
    // main view) instead of the forward filler. Serial pass → reuse one static
    // G-buffer + light/tile scratch sized to the max RTT target.
    const bool rttDeferred = fds::FeatureFlags::shard_deferred();
    const bool rttHdr = fds::FeatureFlags::hdr();  // HDR mirror reflection (b): frame-level, used by the deferred render AND the panel composite below
    static meka::GBuffer            s_rttGB;
    static ViewLightsSoA            s_rttLights;
    static std::vector<TileLights>  s_rttTileLights;
    static bool                     s_rttGBInit = false;
    if (rttDeferred && !s_rttGBInit) {
        s_rttGB.normal.assign(kRttTexels, 0);
        s_rttGB.txtr.assign(kRttTexels, 0xFFFFFFFFu);
        s_rttGB.tangent.assign(kRttTexels, 0);
        s_rttGB.shadowMatID.assign(kRttTexels, 0);
        if (fds::FeatureFlags::shadow_lightmap()) {
            s_rttGB.lightmapMF.assign(kRttTexels, 0);
            s_rttGB.lightmapST.assign(kRttTexels, 0);
        }
        s_rttTileLights.resize(DEFERRED_NUM_TILES);
        s_rttGBInit = true;
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

    // View-dependent resolution: bake each panel at the pow2 size matching
    // its on-screen footprint, clamped to the build-time max (texWMax/Max).
    // A distant/oblique panel that the fixed world-size×density formula
    // would still bake at 512² (saturating the cap) drops to the few-hundred
    // texels it actually occupies — the dominant central-room ANIM cost.
    const bool rttAdaptive = fds::FeatureFlags::mirror_rtt_adaptive();
    const float rttAdScale = fds::FeatureFlags::mirror_rtt_adaptive_scale();
    auto pow2clamp = [](float v, int lo, int hi) {
        int p = lo;
        while (p < hi && float(p * 2) <= v * 1.5f) p <<= 1;
        return p;
    };

    // One bake: render slot s's window view from `camPos` (distance-to-plane
    // D, side-aware backSide) into its texture. swPx/shPx = raw on-screen
    // footprint driving adaptive res, honoured only when `adaptive` — the
    // slice-2 recursive tree bakes at max dims so the sibling texture
    // stash/restore below is a constant-size byte copy. Shared verbatim by
    // the legacy scheduled path, the flat N-pass recursion, and the tree.
    auto bakeJob = [&](MirrorRttSlot &s, const Vector &camPos, const float D,
                       const bool backSide, const float swPx, const float shPx,
                       const bool adaptive) {
        // Shrink this job's bake to its on-screen footprint (never above
        // the allocated texWMax/texHMax). Aspect is preserved by sizing
        // each axis from its own projected extent.
        if (rttAdaptive && adaptive) {
            s.texW = pow2clamp(swPx * rttAdScale, 64, s.texWMax);
            s.texH = pow2clamp(shPx * rttAdScale, 64, s.texHMax);
        } else {
            s.texW = s.texWMax;
            s.texH = s.texHMax;
        }
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
        const Vector eN = backSide
            ? Vector{ -s.bN.x, -s.bN.y, -s.bN.z } : s.bN;
        const Vector eU = backSide
            ? Vector{ -s.axisU.x, -s.axisU.y, -s.axisU.z } : s.axisU;
        const float eU0 = backSide ? -s.u1 : s.u0;
        const float eU1 = backSide ? -s.u0 : s.u1;
        std::memset(&s_rttCam, 0, sizeof(s_rttCam));
        s_rttCam.ISource = camPos;
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
        const float cu = camPos.x*eU.x + camPos.y*eU.y + camPos.z*eU.z;
        const float cv = camPos.x*s.axisV.x + camPos.y*s.axisV.y + camPos.z*s.axisV.z;
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
        static const bool kUv05 = std::getenv("FDS_MIRROR_RTT_UV05") != nullptr;
        for (const MirrorRttSlot::SlotVert &sv : s.verts) {
            const float epu = backSide ? -sv.pu : sv.pu;
            float tu = ( FOVX * (epu - cu) / D + CntrEX) / float(s.texW);
            float tv = (-FOVY * (sv.pv - cv) / D + CntrEY) / float(s.texH);
            // Keep off the wrap seam (Txtr_Tiled wraps).
            tu = std::min(std::max(tu, 0.002f), 0.998f);
            tv = std::min(std::max(tv, 0.002f), 0.998f);
            if (kUv05) { tu = 0.5f; tv = 0.5f; }
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
        // Every rendering RTT slot is an order-2 reflection that shows the
        // room (the disco + its volumetric cones live there), so they ALL
        // want the deferred kernel when --shard-deferred is on — without it
        // the cones are missing from the reflection inside the screens
        // (the m2->m1 "screen reflects the room clone" slot is the obvious
        // case). The forward-clone room mirror (#1) is NOT an RTT slot, so
        // it's untouched by this flag regardless. (An earlier textTex gate
        // tried to keep "geometry" slots forward but only managed to strip
        // the cones from the very reflections that need them.)
        const bool slotDeferred = rttDeferred;
        if (CAll != 0) {
            Radix_Sort(FList, SList, CAll);
            if (slotDeferred) {
                // Deferred RTT bake: render the recursive reflection through the
                // deferred kernel (shadows + matches the main view) + the cone
                // pass (disco beams), instead of the forward filler. Serial pass,
                // so reuse the static per-RTT G-buffer + light/tile scratch and
                // the global FList/g_mainCamera (which carry this slot's off-axis
                // projection). Same inline machinery as the shard bake.
                const size_t np = size_t(s.texW) * size_t(s.texH);
                std::fill_n(s_rttGB.txtr.begin(), np, 0xFFFFFFFFu);
                if (!s_rttGB.lightmapMF.empty())
                    std::fill_n(s_rttGB.lightmapMF.begin(), np, 0u);
                fds::RenderContext rctx;
                rctx.scene       = sc;
                rctx.camera      = fds::g_mainCamera;
                rctx.faces.fList = FList;
                rctx.faces.sList = SList;
                rctx.faces.cAll  = CAll;
                rctx.target.vpage            = (uint32_t*)s_rttSurf.Data;
                rctx.target.bytesPerScanline = s_rttSurf.BPSL;
                rctx.target.zpage16          = (uint16_t*)s_rttSurf.Z16;
                rctx.target.xres             = s.texW;
                rctx.target.yres             = s.texH;
                rctx.target.gbuffer          = &s_rttGB;
                MekaleleFillRegionInline(rctx, 0, 0, float(s.texW), float(s.texH));
                DeferredLightingCtx dctx{};
                DeferredOverride ov;
                ov.gb         = &s_rttGB;
                ov.cam        = &fds::g_mainCamera;
                ov.lights     = &s_rttLights;
                ov.tileLights = s_rttTileLights.data();
                ov.vpage      = s_rttSurf.Data;
                ov.zpage16    = (word*)s_rttSurf.Z16;
                ov.xres       = s.texW;
                ov.yres       = s.texH;
                ov.inlineDispatch = true;
                // HDR-correct reflection: size g_hdrBuf to THIS RTT slot so the
                // deferred kernel + cones accumulate linear radiance. For order-1
                // panels the composite below captures that float radiance into the
                // material's hdrRefl (b); we still tonemap onto s_rttSurf as the
                // 8-bit LDR fallback. g_hdrActive is set manually — the RTT has no
                // froxel composite to set it — so cones add into g_hdrBuf and the
                // tonemap runs. The main pass's Hdr_BeginFrame restores g_hdrBuf.
                if (rttHdr) fds::Hdr_BeginFramePass(s.texW, s.texH);
                Render_DeferredLighting(dctx, &ov);
                // Hdr_ActivateNoFog (not a bare g_hdrActive=true): with
                // --deferred-quarter the kernel shades only wave-1 into g_hdrBuf;
                // the wave-2 FILL pixels land in s_rttSurf (8-bit) but NOT g_hdrBuf,
                // so a bare activate would tonemap them as 0 → a checkerboard
                // garble (HDR only; the 8-bit surface is coherent). Lifting the
                // uncovered (h[3]==0) pixels from s_rttSurf into g_hdrBuf first
                // resolves the full image, THEN activates — so cones + tonemap see
                // a complete buffer.
                if (rttHdr) fds::Hdr_ActivateNoFog();
                Render_VolumetricCones(dctx, /*inlineDispatch=*/true);
                if (rttHdr) {
                    fds::Render_TonemapToVPage();
                    // The slot's radiance is now resolved onto s_rttSurf; clear
                    // the active flag so a later pass (e.g. parallel shards) can't
                    // accumulate into this RTT-sized buffer at its own dims.
                    fds::g_hdrActive = false;
                }
            } else {
                Render(RenderPath::ForceForward);
            }
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
                "[MIRROR-RTT] order=%d %s job: %d/%d color px, %d z px, CAll=%d\n",
                int(s.order), slotDeferred ? "deferred" : "forward",
                nz, s.texW * s.texH, nzz, int(CAll));
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
                                 path, camPos.x, camPos.y, camPos.z,
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
        const float rGain = fds::FeatureFlags::mirror_rtt_gain();
        const uint32_t gainQ = uint32_t(
            std::min(std::max(rGain, 0.0f), 1.0f) * 256.0f);
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
            // HDR reflection (b): build a FLOAT panel radiance = linear(text) +
            // reflection*gain, sampled emissively by the transparent kernel so
            // reflected highlights bloom. Reuses this loop's text affine; the
            // reflection comes from the float g_hdrBuf (the RTT slot's radiance,
            // not yet quantized) at the same pixel index. The 8-bit path below
            // stays as the LDR fallback. hdrRefl owned by the panel material.
            float *hdrR = nullptr;
            if (rttHdr && slotDeferred) {   // float radiance only exists when the deferred render ran this slot
                if (s.mat->hdrReflW != s.texW || s.mat->hdrReflH != s.texH) {
                    std::free(s.mat->hdrRefl);
                    s.mat->hdrRefl = (float*)std::malloc(size_t(s.texW)*s.texH*3*sizeof(float));
                    s.mat->hdrReflW = s.texW; s.mat->hdrReflH = s.texH;
                }
                hdrR = s.mat->hdrRefl;
            }
            auto linf = [](uint32_t c){ const float n = float(c)*(1.0f/255.0f); return n*n*255.0f; };
            for (int y = 0; y < s.texH; ++y) {
                const float pv = s.v1 - (float(y) + 0.5f) * dv;
                for (int x = 0; x < s.texW; ++x) {
                    const float epu = eU0 + (float(x) + 0.5f) * du;
                    const float pu = backSide ? -epu : epu;
                    const float fu = s.tA[0]*pu + s.tA[1]*pv + s.tA[2];
                    const float fv = s.tA[3]*pu + s.tA[4]*pv + s.tA[5];
                    const int iu = int(fu * float(tw)) & (tw - 1);
                    const int iv = int(fv * float(th)) & (th - 1);
                    const int blk = ((iu >> 2) * tBlocksY + (iv >> 2)) << 4;
                    const dword t = td[blk + ((iv & 3) << 2) + (iu & 3)];
                    const size_t pix = size_t(y) * size_t(s.texW) + x;
                    uint32_t &o = px[pix];
                    const uint32_t tb =  t        & 0xFF;
                    const uint32_t tg = (t >> 8)  & 0xFF;
                    const uint32_t tr = (t >> 16) & 0xFF;
                    if (hdrR) {
                        hdrR[pix*3+0] = linf(tb) + fds::g_hdrBuf[pix*4+0]*rGain;
                        hdrR[pix*3+1] = linf(tg) + fds::g_hdrBuf[pix*4+1]*rGain;
                        hdrR[pix*3+2] = linf(tr) + fds::g_hdrBuf[pix*4+2]*rGain;
                    }
                    // Reflection attenuated by the reflectance gain;
                    // the text rides on top at full strength.
                    uint32_t ob = tb + ((( o        & 0xFF) * gainQ) >> 8);
                    uint32_t og = tg + ((((o >> 8)  & 0xFF) * gainQ) >> 8);
                    uint32_t orr = tr + ((((o >> 16) & 0xFF) * gainQ) >> 8);
                    if (ob > 255) ob = 255;
                    if (og > 255) og = 255;
                    if (orr > 255) orr = 255;
                    o = ob | (og << 8) | (orr << 16) | 0xFF000000u;
                }
            }
            if (rttHdr && slotDeferred) s.mat->Flags |=  Mat_HdrReflection;
            else                        s.mat->Flags &= ~Mat_HdrReflection;
        }
        // Recursive passthrough compensation: the deferred kernel displays a
        // Lum=1/Diffuse=0 textured material as texel*250/256 — the 250
        // saturation cap absorbs Luminosity's 255 base PLUS any omni spill,
        // so that ratio is exact regardless of scene lights (the panels are
        // NOT re-lit; the cap eats it). Pre-multiply the stored texture by
        // 256/250 (rounded) so display cancels to texel*1.0: without this
        // every bounce decays ~2.3% and a deep tunnel darkens visibly.
        if (recurseDepth > 0) {
            uint32_t *cpx = (uint32_t*)s_rttSurf.Data;
            const size_t n = size_t(s.texW) * size_t(s.texH);
            for (size_t i = 0; i < n; ++i) {
                const uint32_t p = cpx[i];
                uint32_t b = (( p        & 0xFF) * 256u + 125u) / 250u;
                uint32_t g = (((p >> 8)  & 0xFF) * 256u + 125u) / 250u;
                uint32_t r = (((p >> 16) & 0xFF) * 256u + 125u) / 250u;
                if (b > 255u) b = 255u;
                if (g > 255u) g = 255u;
                if (r > 255u) r = 255u;
                cpx[i] = b | (g << 8) | (r << 16) | 0xFF000000u;
            }
        }
        // Linear RTT pixels → slot texture, re-tiled in place (the
        // same one-way Sachletz the texture loader applies).
        std::memcpy(s.mat->Txtr->Data, s_rttSurf.Data,
                    size_t(s.texW) * size_t(s.texH) * 4);
        Sachletz((dword*)s.mat->Txtr->Data, s.texW, s.texH);
        // Adaptive res may have shrunk texW/texH below the allocated max;
        // re-point the texture's dims (+log2 for the Txtr_Tiled masks) so
        // the sampler reads exactly the filled region. Buffer (texWMax)
        // unchanged. Dims are pow2, so log2 is a shift count.
        auto log2p2 = [](int v) { int l = 0; while (v > 1) { v >>= 1; ++l; } return l; };
        s.mat->Txtr->SizeX = s.texW; s.mat->Txtr->LSizeX = log2p2(s.texW);
        s.mat->Txtr->SizeY = s.texH; s.mat->Txtr->LSizeY = log2p2(s.texH);
        s.staleFrames = 0;
    };

    // FDS_MIRROR_RECURSE_FLAT=1 keeps the slice-1b flat N-pass approximation
    // (every panel baked from its OWN order-1 camera) for A/B comparison.
    static const bool kRecurseFlat =
        std::getenv("FDS_MIRROR_RECURSE_FLAT") != nullptr;
    if (recurseDepth <= 0 || kRecurseFlat) {
        // Flat N-pass recursion (slice 1b): bake the whole slot set N times.
        // Each pass re-renders every panel while the OTHER panels show their
        // latest texture, so a mirror-in-a-mirror gains one bounce per pass.
        // Geometric approximation: every bake uses the panel's own order-1
        // camera, so nested images lack the per-context parallax the tree
        // below provides. Legacy scheduled path = 1 pass.
        const int numPasses = recurseDepth > 0 ? recurseDepth : 1;
        for (int pass = 0; pass < numPasses; ++pass)
            for (const Job &j : jobs)
                bakeJob(*j.slot, j.camPos, j.dist, j.backSide, j.sw, j.sh,
                        /*adaptive=*/true);
    } else {
        // ── Slice 2: per-context recursive bake tree ────────────────────
        // renderView(vcam, depth-1) from the plan: each panel visible in a
        // view is baked deepest-first from THAT view's reflected camera, so
        // nested bounces get exact per-context geometry instead of reusing
        // the panel's own order-1 image. A texture is CONSUMED at the parent
        // bake (memcpy'd pixels), so one texture per slot suffices as long
        // as siblings' subtrees — which re-bake shared panels in their own
        // context — are stashed and restored before the parent renders.
        int bakesLeft = kRecurseMaxBakes;

        // Side-aware eye reflection across slot t's plane (same rule as the
        // top-level job selection). False = degenerate (eye on the plane).
        auto reflectAcross = [](const MirrorRttSlot &t, const Vector &P,
                                Vector &out, bool &backSide) -> bool {
            const float sd = t.bN.x*P.x + t.bN.y*P.y + t.bN.z*P.z + t.bD;
            backSide = (sd < 0.0f);
            out = backSide
                ? reflectPointAcross(P, { -t.bN.x, -t.bN.y, -t.bN.z }, -t.bD)
                : reflectPointAcross(P, t.bN, t.bD);
            float so = t.bN.x*out.x + t.bN.y*out.y + t.bN.z*out.z + t.bD;
            if (backSide) so = -so;
            return so < -0.01f;
        };

        // Panels visible inside slot s's window view from camPos: project
        // each other slot's window corners through s's off-axis projection
        // (the same formulas bakeJob stamps) and keep live footprints in the
        // target rect. A corner behind the near plane makes the footprint
        // conservative (full window) rather than dropping the panel.
        auto childrenOf = [&](const MirrorRttSlot &s, const Vector &camPos,
                              bool backSide) {
            std::vector<MirrorRttSlot*> kids;
            const Vector eN = backSide
                ? Vector{ -s.bN.x, -s.bN.y, -s.bN.z } : s.bN;
            const Vector eU = backSide
                ? Vector{ -s.axisU.x, -s.axisU.y, -s.axisU.z } : s.axisU;
            const float eU0 = backSide ? -s.u1 : s.u0;
            const float eU1 = backSide ? -s.u0 : s.u1;
            float sd = s.bN.x*camPos.x + s.bN.y*camPos.y + s.bN.z*camPos.z + s.bD;
            if (backSide) sd = -sd;
            const float D = -sd;
            if (D <= 0.01f) return kids;
            const float W = float(s.texWMax), H = float(s.texHMax);
            const float fx = W * D / (eU1 - eU0);
            const float fy = H * D / (s.v1 - s.v0);
            const float cu = camPos.x*eU.x + camPos.y*eU.y + camPos.z*eU.z;
            const float cv = camPos.x*s.axisV.x + camPos.y*s.axisV.y
                           + camPos.z*s.axisV.z;
            const float cx = fx * (cu - eU0) / D;
            const float cy = fy * (s.v1 - cv) / D;
            const float nearZ = D * 1.001f + 0.01f;
            for (MirrorRttSlot &t : slots) {
                if (&t == &s || t.order != 1) continue;
                float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
                bool any = false, clippedNear = false;
                for (int ci = 0; ci < 4; ++ci) {
                    const float uu = (ci & 1) ? t.u1 : t.u0;
                    const float vv = (ci & 2) ? t.v1 : t.v0;
                    const Vector wp = {
                        uu*t.axisU.x + vv*t.axisV.x - t.bD*t.bN.x,
                        uu*t.axisU.y + vv*t.axisV.y - t.bD*t.bN.y,
                        uu*t.axisU.z + vv*t.axisV.z - t.bD*t.bN.z };
                    const float dx = wp.x - camPos.x, dy = wp.y - camPos.y,
                                dz = wp.z - camPos.z;
                    const float vz = eN.x*dx + eN.y*dy + eN.z*dz;
                    if (vz <= nearZ) { clippedNear = true; continue; }
                    const float vx = eU.x*dx + eU.y*dy + eU.z*dz;
                    const float vy = s.axisV.x*dx + s.axisV.y*dy + s.axisV.z*dz;
                    const float sx =  fx * vx / vz + cx;
                    const float sy = -fy * vy / vz + cy;
                    bx0 = std::min(bx0, sx); bx1 = std::max(bx1, sx);
                    by0 = std::min(by0, sy); by1 = std::max(by1, sy);
                    any = true;
                }
                if (!any) continue;      // fully behind the mirror plane
                if (clippedNear) { bx0 = 0; by0 = 0; bx1 = W; by1 = H; }
                bx0 = std::max(bx0, 0.0f); by0 = std::max(by0, 0.0f);
                bx1 = std::min(bx1, W);    by1 = std::min(by1, H);
                if ((bx1 - bx0) * (by1 - by0) < 2.0f) continue;
                kids.push_back(&t);
            }
            return kids;
        };

        struct TexStash { MirrorRttSlot *t; std::vector<uint32_t> px; };
        auto stashTex = [](std::vector<TexStash> &out, MirrorRttSlot *t) {
            const uint32_t *d = (const uint32_t*)t->mat->Txtr->Data;
            out.push_back({ t, std::vector<uint32_t>(
                d, d + size_t(t->texW) * size_t(t->texH)) });
        };

        std::function<bool(MirrorRttSlot&, const Vector&, bool, int)> bakeTree =
            [&](MirrorRttSlot &s, const Vector &camPos, bool backSide,
                int bounces) -> bool {
            if (bakesLeft <= 0) return false;
            float sd = s.bN.x*camPos.x + s.bN.y*camPos.y + s.bN.z*camPos.z + s.bD;
            if (backSide) sd = -sd;
            if (sd >= -0.01f) return false;   // eye not behind the panel plane
            if (bounces > 1) {
                std::vector<TexStash> stash;
                for (MirrorRttSlot *t : childrenOf(s, camPos, backSide)) {
                    Vector cpos; bool cback;
                    if (!reflectAcross(*t, camPos, cpos, cback)) continue;
                    if (bakeTree(*t, cpos, cback, bounces - 1))
                        stashTex(stash, t);
                }
                for (const TexStash &st : stash)
                    std::memcpy(st.t->mat->Txtr->Data, st.px.data(),
                                st.px.size() * sizeof(uint32_t));
            }
            --bakesLeft;
            bakeJob(s, camPos, -sd, backSide, 0.0f, 0.0f, /*adaptive=*/false);
            return true;
        };

        // Top level: only panels with a real main-screen footprint. An
        // off-screen mirror (the flat path's full-frame fallback jobs) is
        // baked by the tree as a CHILD of whichever view actually shows it.
        std::vector<TexStash> stash;
        for (const Job &j : jobs) {
            if (j.offscreen) continue;
            if (bakeTree(*j.slot, j.camPos, j.backSide, recurseDepth))
                stashTex(stash, j.slot);
        }
        for (const TexStash &st : stash)
            std::memcpy(st.t->mat->Txtr->Data, st.px.data(),
                        st.px.size() * sizeof(uint32_t));
        g_rttJobsLastFrame = kRecurseMaxBakes - bakesLeft;
        if (bakesLeft <= 0)
            std::fprintf(stderr, "[MIRROR-RTT] recurse bake cap (%d) hit — "
                         "deepest views truncated\n", kRecurseMaxBakes);
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
