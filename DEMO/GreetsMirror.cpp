#include "GreetsMirror.h"

#include <Base/FDS_DECS.H>  // Matrix_Copy + Mat_ID
#include <Base/FDS_VARS.H>  // MatrixXVector template
#include <Base/Face.h>
#include <Base/Material.h>
#include <Base/Matrix.h>
#include <Base/Object.h>
#include <Base/Scene.h>
#include <Base/TriMesh.h>
#include <Base/Vertex.h>

#include <Base/Omni.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Provided by MISC/PREPROC.CPP — stamps F->A_idx/B_idx/C_idx for SoA.
extern void Compute_FaceVertexIndices(TriMesh *T);

// getAlignedBlock / getAlignedType come in via FDS_DECS.H above.

namespace fds {
namespace {

// Per-frame motion state: remember where each source mesh's verts
// landed inside the mirror clone so we can re-mirror them every frame
// without rebuilding the mesh. Populated by BuildMirrorMeshAndHideWall,
// drained by UpdateMirrorPerFrame.
struct ClonedMeshRange {
    TriMesh *sourceMesh;
    DWord    vStart;       // offset into g_mirrorMesh->Verts
    DWord    vCount;
};
struct ClonedOmniRef {
    Omni *sourceOmni;
    Omni *mirrorOmni;
    // Original IRange values captured at clone time. Per-frame
    // UpdateMirrorPerFrame clamps both omnis' IRange to their
    // distance-to-plane so neither omni's lighting sphere can cross
    // the mirror plane — soft compartmentalization without a per-pixel
    // filter in the deferred kernel. Re-applied each frame so dynamic
    // omnis (Hull-parented robot spot) recompute on every motion.
    float    origSourceRange = 0.0f;
    float    origMirrorRange = 0.0f;
};

static TriMesh *g_mirrorMesh = nullptr;
static std::vector<ClonedMeshRange> g_clonedRanges;
static std::vector<ClonedOmniRef>   g_clonedOmnis;

}  // namespace
}  // namespace fds

namespace fds {

MirrorPlane FindTeleporterMirrorPlane(Scene *sc)
{
    MirrorPlane out{};
    if (!sc) return out;

    // Pass 1: dominant normal direction. We accumulate the world-space
    // normal of every "teleporter" face, then re-pass to drop faces whose
    // normal disagrees with that majority by > ~30°. Without the filter
    // a single stray decal would tilt the plane.
    Vector dominant = {0.0f, 0.0f, 0.0f};
    int rawCount = 0;
    auto walkTeleporterFaces = [&](auto &&visit) {
        for (Object *Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
            if (Obj->Type != Obj_TriMesh) continue;
            TriMesh *T = (TriMesh*)Obj->Data;
            if (!T || !T->Faces) continue;
            for (DWord fi = 0; fi < T->FIndex; ++fi) {
                Face &F = T->Faces[fi];
                if (!F.A || !F.B || !F.C) continue;
                if (!F.Txtr || !F.Txtr->Name) continue;
                if (std::strcmp(F.Txtr->Name, "teleporter") != 0) continue;
                // World-space normal — RotMat is orthonormal so no
                // inverse-transpose needed for direction transform.
                Vector localN = F.N;
                Vector wN;
                MatrixXVector(T->RotMat, &localN, &wN);
                visit(T, F, wN);
            }
        }
    };
    walkTeleporterFaces([&](TriMesh*, Face &, const Vector &wN) {
        dominant += wN;
        ++rawCount;
    });
    if (rawCount == 0) {
        std::fprintf(stderr, "[MIRROR] no 'teleporter' faces found\n");
        return out;
    }
    dominant.normalize();

    // Pass 2: average normal + plane offset, weighted equally per kept
    // face. Faces with N·dominant < 0.866 (≈30°) dropped as outliers.
    Vector accN = {0.0f, 0.0f, 0.0f};
    float accD = 0.0f;
    int keptCount = 0, droppedOutlier = 0;
    walkTeleporterFaces([&](TriMesh *T, Face &F, const Vector &wN) {
        const float dot = wN.x*dominant.x + wN.y*dominant.y + wN.z*dominant.z;
        if (dot < 0.866f) { ++droppedOutlier; return; }
        // World-space A position: T->RotMat * F.A->Pos + T->IPos
        Vector localA = F.A->Pos;
        Vector wA;
        MatrixXVector(T->RotMat, &localA, &wA);
        wA += T->IPos;
        // Unit-length normal (might drift after the dot-filter accepted
        // raw lengths). Renormalize per-face before accumulating so we
        // average directions, not magnitudes.
        Vector unitN = wN;
        unitN.normalize();
        const float d = -(unitN.x*wA.x + unitN.y*wA.y + unitN.z*wA.z);
        accN += unitN;
        accD += d;
        ++keptCount;
    });
    if (keptCount == 0) {
        std::fprintf(stderr,
            "[MIRROR] all %d teleporter faces dropped as outliers — plane invalid\n",
            rawCount);
        return out;
    }
    out.N = accN;
    out.N.normalize();
    out.d = accD / float(keptCount);
    out.faceCount = keptCount;
    out.valid = true;
    std::fprintf(stderr,
        "[MIRROR] teleporter plane: N=(%.3f, %.3f, %.3f) d=%.3f "
        "from %d/%d faces (%d outliers dropped)\n",
        out.N.x, out.N.y, out.N.z, out.d,
        keptCount, rawCount, droppedOutlier);
    return out;
}

int BuildMirrorMeshAndHideWall(Scene *sc, const MirrorPlane &plane)
{
    if (!sc || !plane.valid) return 0;
    g_clonedRanges.clear();
    g_clonedOmnis.clear();
    g_mirrorMesh = nullptr;
    const Vector &N = plane.N;
    const float d = plane.d;
    auto reflectPt = [&](const Vector &p) -> Vector {
        const float k = 2.0f * (N.x*p.x + N.y*p.y + N.z*p.z + d);
        return { p.x - k*N.x, p.y - k*N.y, p.z - k*N.z };
    };
    auto reflectDir = [&](const Vector &v) -> Vector {
        const float k = 2.0f * (N.x*v.x + N.y*v.y + N.z*v.z);
        return { v.x - k*N.x, v.y - k*N.y, v.z - k*N.z };
    };

    auto isTeleporter = [](const Face &F) -> bool {
        return F.Txtr && F.Txtr->Name &&
               std::strcmp(F.Txtr->Name, "teleporter") == 0;
    };

    // Count total verts/faces across all source meshes (excluding teleporter
    // faces themselves — we want a "hole" where the wall is, not a mirror
    // image of the wall).
    DWord totalVerts = 0, totalFaces = 0;
    for (Object *Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh) continue;
        TriMesh *T = (TriMesh*)Obj->Data;
        if (!T || !T->Verts || !T->Faces) continue;
        totalVerts += T->VIndex;
        for (DWord fi = 0; fi < T->FIndex; ++fi) {
            if (isTeleporter(T->Faces[fi])) continue;
            if (!T->Faces[fi].A) continue;
            ++totalFaces;
        }
    }
    if (totalFaces == 0) {
        std::fprintf(stderr, "[MIRROR] nothing to clone (0 non-teleporter faces)\n");
        return 0;
    }

    // Allocate mirror mesh.
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

    // Animate_Objects evaluates Pos/Scale/Rotate splines every frame.
    // Build single-key splines parked at (0,0,0) / (1,1,1) / identity-quat
    // so per-frame Spline_Calc_3D yields IPos=0, IScale=1, IRot=identity.
    //
    // CRITICAL: Quaternion field order is {W, x, y, z} — W FIRST. An
    // earlier version used aggregate-init `{x,y,z,W}` which yielded
    // IScale=(1,1,0) and RotMat row 2 *= 0 → every clone vert collapsed
    // to a single Z plane (the visible smear bug). Set fields explicitly
    // to avoid the trap.
    auto stampSingleKey = [](Spline &sp, float x, float y, float z, float w) {
        sp.NumKeys = 1;
        sp.CurKey  = 0;
        sp.Flags   = 0;
        sp.Keys    = (SplineKey*)std::calloc(1, sizeof(SplineKey));
        sp.Keys[0].Frame = 0.0f;
        sp.Keys[0].Pos.x = x; sp.Keys[0].Pos.y = y; sp.Keys[0].Pos.z = z; sp.Keys[0].Pos.W = w;
        sp.Keys[0].AA.x  = x; sp.Keys[0].AA.y  = y; sp.Keys[0].AA.z  = z; sp.Keys[0].AA.W  = w;
    };
    // Spline_Calc_3D reads (x,y,z) → Pos / Scale Vector. Spline_Calc_4D_Alt
    // reads full quat → Rotate quat. Pick W=1 for the identity rotation
    // quaternion (zero x/y/z); W doesn't matter for Pos/Scale.
    stampSingleKey(MM->Pos,    /*x*/0.0f, /*y*/0.0f, /*z*/0.0f, /*W*/0.0f);
    stampSingleKey(MM->Scale,  /*x*/1.0f, /*y*/1.0f, /*z*/1.0f, /*W*/0.0f);
    stampSingleKey(MM->Rotate, /*x*/0.0f, /*y*/0.0f, /*z*/0.0f, /*W*/1.0f);

    // Fill verts (world-mirrored) and faces (winding swapped, normal
    // reflected). Walk every source mesh; each gets a contiguous block
    // of verts in MM, faces re-point with the block's base offset.
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
            // World pos through T's transform, then reflect.
            Vector localP = T->Verts[vi].Pos;
            Vector worldP;
            MatrixXVector(T->RotMat, &localP, &worldP);
            worldP.x += T->IPos.x; worldP.y += T->IPos.y; worldP.z += T->IPos.z;
            const Vector mirroredP = reflectPt(worldP);
            MM->Verts[vOfs].Pos = mirroredP;
            // Reflect per-vertex normal too if present (used by Gouraud).
            Vector localN = T->Verts[vi].N;
            Vector worldN;
            MatrixXVector(T->RotMat, &localN, &worldN);
            MM->Verts[vOfs].N = reflectDir(worldN);
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
            if (isTeleporter(OF)) continue;
            if (!OF.A || !OF.B || !OF.C) continue;
            Face &CF = MM->Faces[fOfs];
            CF = OF;  // material, flags, filler, U/V coords
            // Re-point A/B/C to MM's vert block; swap B↔C so the mirror
            // face winds CCW for the real camera (reflection flips chirality).
            CF.A = MM->Verts + vStart + (OF.A - T->Verts);
            CF.B = MM->Verts + vStart + (OF.C - T->Verts);  // swap
            CF.C = MM->Verts + vStart + (OF.B - T->Verts);  // swap
            // The per-face UV slots U2/V2 and EU2/EV2 belong to B; U3/V3
            // and EU3/EV3 belong to C. Since we swapped the vertex
            // pointers we must swap their UVs too — otherwise B samples
            // C's texel and the texture coords come out fragged.
            std::swap(CF.U2, CF.U3);
            std::swap(CF.V2, CF.V3);
            std::swap(CF.EU2, CF.EU3);
            std::swap(CF.EV2, CF.EV3);
            // Reflect the face normal so lighting / culling sees the
            // correct outward direction for the mirrored geometry.
            CF.N = reflectDir(OF.N);
            // Recompute NormProd. Engine convention is `NormProd = -(N·A)`
            // (PREPROC.CPP:113 + all other init sites), and the
            // backface test in Transform.cpp:1340 reads
            // `AP·N < NormProd` against that signed convention. Using
            // the positive form makes every clone face evaluate as
            // back-facing for typical camera positions and the
            // mirror's outward-facing surfaces disappear. Negate.
            CF.NormProd = -(CF.N.x * CF.A->Pos.x +
                            CF.N.y * CF.A->Pos.y +
                            CF.N.z * CF.A->Pos.z);
            ++fOfs;
        }
        // Track this mesh's clone-vert range so UpdateMirrorPerFrame
        // can re-mirror its world-space verts after animation each frame.
        g_clonedRanges.push_back({T, vStart, T->VIndex});
    }
    g_mirrorMesh = MM;

    // Bounding sphere from the mirrored bbox. Loose but safe — better
    // than leaving it at zero (would frustum-cull every frame).
    Vector ctr = { (bbMin.x + bbMax.x) * 0.5f,
                   (bbMin.y + bbMax.y) * 0.5f,
                   (bbMin.z + bbMax.z) * 0.5f };
    const float dx = bbMax.x - bbMin.x;
    const float dy = bbMax.y - bbMin.y;
    const float dz = bbMax.z - bbMin.z;
    const float radSq = 0.25f * (dx*dx + dy*dy + dz*dz);
    MM->BSphereCtr        = ctr;
    MM->BSphereRad        = radSq;          // legacy field: radius squared
    MM->BSphereRadius     = std::sqrt(radSq);  // new field: linear radius
    MM->BSphereScreenPos  = {0.0f, 0.0f, 0.0f};

    // SoA stamping so frame-based vertex consumers see correct A/B/C indices.
    Compute_FaceVertexIndices(MM);

    // Wrap in Object and link at head of both ObjectHead and TriMeshHead.
    Object *MObj = getAlignedType<Object>(16);
    std::memset(MObj, 0, sizeof(Object));
    MObj->Type   = Obj_TriMesh;
    MObj->Data   = MM;
    // Animate_Objects (Transform.cpp:274-306) derefs Obj->Pos and Obj->Rot
    // unconditionally for every Object in the chain — alias them onto the
    // mesh's IPos/RotMat the way the FLD loader does for normal objects.
    // Pivot stays at (0,0,0) (calloc'd) so the pivot-subtract on line 288
    // is a no-op for our static clone.
    MObj->Pos = &MM->IPos;
    MObj->Rot = &MM->RotMat;
    // Strdup a copyable name so the engine's Object_Free won't choke on
    // it (other Objects own char* with similar lifetime expectations).
    const char *nm = "mirror_clone";
    MObj->Name = (char*)std::malloc(std::strlen(nm) + 1);
    std::strcpy(MObj->Name, nm);

    MObj->Next = sc->ObjectHead;
    if (sc->ObjectHead) sc->ObjectHead->Prev = MObj;
    sc->ObjectHead = MObj;

    MM->Next = sc->TriMeshHead;
    if (sc->TriMeshHead) sc->TriMeshHead->Prev = MM;
    sc->TriMeshHead = MM;

    // Turn the wall semi-transparent: clone the teleporter material with
    // Mat_Transparent + XparBlendAlpha so the deferred transparency pass
    // blends the wall's color over the mirror geometry beneath. Without
    // the clone we'd modify the source material and any non-teleporter
    // face that shared it would also go transparent.
    int wallFacesRetargeted = 0;
    Material *teleporterMirrorMat = nullptr;
    for (Object *Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh) continue;
        if (Obj == MObj) continue;  // don't touch the clone
        TriMesh *T = (TriMesh*)Obj->Data;
        if (!T || !T->Faces) continue;
        for (DWord fi = 0; fi < T->FIndex; ++fi) {
            Face &F = T->Faces[fi];
            if (!isTeleporter(F)) continue;
            if (!teleporterMirrorMat) {
                teleporterMirrorMat = getAlignedType<Material>(16);
                std::memcpy(teleporterMirrorMat, F.Txtr, sizeof(Material));
                teleporterMirrorMat->Flags |= Mat_Transparent;
                teleporterMirrorMat->XparBlendAlpha = 0.30f;  // 30% wall, 70% mirror beneath
                teleporterMirrorMat->Prev = nullptr;
                teleporterMirrorMat->Next = MatLib;
                if (MatLib) MatLib->Prev = teleporterMirrorMat;
                MatLib = teleporterMirrorMat;
            }
            F.Txtr = teleporterMirrorMat;
            ++wallFacesRetargeted;
        }
    }

    // Clone every omni in the scene. Mirrored omni sits at the reflected
    // world position with reflected direction (for spots), so the
    // mirror-side geometry gets lit consistently with what the user
    // sees through the wall. Cloned omnis share the source's Pos/Range
    // splines — Animate_Objects will fill their IPos from those each
    // frame and UpdateMirrorPerFrame then overrides with the reflected
    // SOURCE IPos so dynamic / parented omnis track correctly.
    int omniCount = 0;
    for (Omni *srcO = sc->OmniHead; srcO; srcO = srcO->Next) {
        // Don't re-clone clones if BuildMirror is called twice.
        bool isAlreadyClone = false;
        for (const auto &c : g_clonedOmnis) {
            if (c.mirrorOmni == srcO) { isAlreadyClone = true; break; }
        }
        if (isAlreadyClone) continue;
        Omni *clone = (Omni*)getAlignedBlock(sizeof(Omni), 16);
        std::memcpy(clone, srcO, sizeof(Omni));
        clone->IPos = reflectPt(srcO->IPos);
        clone->IDir = reflectDir(srcO->IDir);
        clone->Prev = nullptr;
        clone->Next = sc->OmniHead;
        if (sc->OmniHead) sc->OmniHead->Prev = clone;
        sc->OmniHead = clone;
        g_clonedOmnis.push_back({srcO, clone, srcO->IRange, srcO->IRange});
        ++omniCount;
    }

    std::fprintf(stderr,
        "[MIRROR] cloned %u verts / %u faces into 'mirror_clone'; "
        "retargeted %d teleporter wall faces to 30%% transparent mirror mat; "
        "cloned %d omnis (mirror bbox z=[%.1f..%.1f])\n",
        unsigned(vOfs), unsigned(fOfs), wallFacesRetargeted, omniCount,
        bbMin.z, bbMax.z);
    return int(fOfs);
}

// Bresenham line into the active framebuffer. VPage / XRes / YRes are
// engine-global; they live at the file root (declared in FDS_VARS / DECS)
// so we reference them via the ::-rooted name to dodge the surrounding
// `namespace fds` lookup scope.
static void DrawFramebufferLine(int x0, int y0, int x1, int y1, uint32_t color)
{
    dword *fb = (dword*)::VPage;
    const int W = int(::XRes), H = int(::YRes);
    auto plot = [&](int x, int y) {
        if (x < 0 || x >= W || y < 0 || y >= H) return;
        fb[size_t(y) * size_t(W) + size_t(x)] = color;
    };
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        plot(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void DrawMirrorFrame(Scene *sc)
{
    if (!sc) return;
    const uint32_t kFrameColor = 0xFFFFC080u;  // soft warm white (BGRA)
    auto isTeleporter = [](const Face &F) -> bool {
        // After BuildMirror the wall mat was retargeted to a transparent
        // clone, so match by Filler/UV-pointing into the original
        // material name isn't reliable. Track by the per-face property
        // we know stays stable: ShadowMatID (greets's Piramid wall
        // split stamped a per-cluster ID and teleporter faces never get
        // ShadowMatID assigned). Cheaper match: just compare against
        // the cloned mat name "teleporter" — names survived the
        // material clone via memcpy.
        return F.Txtr && F.Txtr->Name &&
               std::strcmp(F.Txtr->Name, "teleporter") == 0;
    };
    int edgesDrawn = 0;
    for (Object *Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh) continue;
        TriMesh *T = (TriMesh*)Obj->Data;
        if (!T || !T->Faces) continue;
        for (DWord fi = 0; fi < T->FIndex; ++fi) {
            Face &F = T->Faces[fi];
            if (!isTeleporter(F)) continue;
            if (!F.A || !F.B || !F.C) continue;
            // Skip if any vert behind near plane (.RZ <= 0 = z too small).
            if (F.A->TPos_AOS.z <= sc->NZP ||
                F.B->TPos_AOS.z <= sc->NZP ||
                F.C->TPos_AOS.z <= sc->NZP) continue;
            const int ax = int(F.A->PX), ay = int(F.A->PY);
            const int bx = int(F.B->PX), by = int(F.B->PY);
            const int cx = int(F.C->PX), cy = int(F.C->PY);
            DrawFramebufferLine(ax, ay, bx, by, kFrameColor);
            DrawFramebufferLine(bx, by, cx, cy, kFrameColor);
            DrawFramebufferLine(cx, cy, ax, ay, kFrameColor);
            edgesDrawn += 3;
        }
    }
    (void)edgesDrawn;
}

void UpdateMirrorPerFrame(Scene *sc, const MirrorPlane &plane)
{
    if (!sc || !plane.valid || !g_mirrorMesh) return;
    const Vector &N = plane.N;
    const float d = plane.d;
    auto reflectPt = [&](const Vector &p) -> Vector {
        const float k = 2.0f * (N.x*p.x + N.y*p.y + N.z*p.z + d);
        return { p.x - k*N.x, p.y - k*N.y, p.z - k*N.z };
    };
    auto reflectDir = [&](const Vector &v) -> Vector {
        const float k = 2.0f * (N.x*v.x + N.y*v.y + N.z*v.z);
        return { v.x - k*N.x, v.y - k*N.y, v.z - k*N.z };
    };

    // Per-mesh: re-mirror the source's CURRENT world-space verts into
    // the clone's vert range. Source's IPos / RotMat came from this
    // frame's Animate_Objects, so dynamic and parented meshes (Hull,
    // legs, etc.) end up with up-to-date reflections.
    for (const auto &r : g_clonedRanges) {
        TriMesh *T = r.sourceMesh;
        if (!T || !T->Verts) continue;
        const DWord n = std::min(r.vCount, T->VIndex);
        for (DWord vi = 0; vi < n; ++vi) {
            Vector localP = T->Verts[vi].Pos;
            Vector worldP;
            MatrixXVector(T->RotMat, &localP, &worldP);
            worldP.x += T->IPos.x; worldP.y += T->IPos.y; worldP.z += T->IPos.z;
            g_mirrorMesh->Verts[r.vStart + vi].Pos = reflectPt(worldP);
            Vector localN = T->Verts[vi].N;
            Vector worldN;
            MatrixXVector(T->RotMat, &localN, &worldN);
            g_mirrorMesh->Verts[r.vStart + vi].N = reflectDir(worldN);
        }
    }
    // Per-omni: re-mirror source's CURRENT IPos / IDir, and clamp each
    // omni's IRange to its distance-from-plane so the lighting sphere
    // can't cross the mirror plane (soft compartment). This prevents
    // the cloned omnis from double-lighting the real side and the
    // source omnis from leaking into the mirror side.
    auto distToPlane = [&](const Vector &P) -> float {
        return std::fabs(N.x*P.x + N.y*P.y + N.z*P.z + d);
    };
    for (auto &c : g_clonedOmnis) {
        if (!c.sourceOmni || !c.mirrorOmni) continue;
        c.mirrorOmni->IPos = reflectPt(c.sourceOmni->IPos);
        c.mirrorOmni->IDir = reflectDir(c.sourceOmni->IDir);
        const float srcLimit = distToPlane(c.sourceOmni->IPos);
        const float mirLimit = distToPlane(c.mirrorOmni->IPos);
        c.sourceOmni->IRange = std::min(c.origSourceRange, srcLimit);
        c.mirrorOmni->IRange = std::min(c.origMirrorRange, mirLimit);
        c.sourceOmni->rRange = c.sourceOmni->IRange > 0.0f ? 1.0f / c.sourceOmni->IRange : 0.0f;
        c.mirrorOmni->rRange = c.mirrorOmni->IRange > 0.0f ? 1.0f / c.mirrorOmni->IRange : 0.0f;
    }
}

void DumpMirrorState(Scene *sc, const char *tag)
{
    if (!sc) return;
    TriMesh *MM = nullptr;
    // Find via ObjectHead since I named the Object "mirror_clone".
    int objTriMeshCount = 0;
    for (Object *O = sc->ObjectHead; O; O = O->Next) {
        if (O->Type != Obj_TriMesh) continue;
        ++objTriMeshCount;
        if (O->Name && std::strcmp(O->Name, "mirror_clone") == 0) {
            MM = (TriMesh*)O->Data;
            break;
        }
    }
    int triMeshHeadCount = 0;
    for (TriMesh *T = sc->TriMeshHead; T; T = T->Next) ++triMeshHeadCount;
    std::fprintf(stderr, "[MIRROR-DUMP %s] ObjectHead-TriMeshes=%d TriMeshHead-count=%d MM=%p\n",
                 tag, objTriMeshCount, triMeshHeadCount, (void*)MM);
    if (!MM) return;
    std::fprintf(stderr,
        "[MIRROR-DUMP %s] MM->IPos=(%.2f,%.2f,%.2f) RotMat[0]=(%.2f,%.2f,%.2f) RotMat[1]=(%.2f,%.2f,%.2f) RotMat[2]=(%.2f,%.2f,%.2f) VIndex=%u FIndex=%u\n",
        tag, MM->IPos.x, MM->IPos.y, MM->IPos.z,
        MM->RotMat[0][0], MM->RotMat[0][1], MM->RotMat[0][2],
        MM->RotMat[1][0], MM->RotMat[1][1], MM->RotMat[1][2],
        MM->RotMat[2][0], MM->RotMat[2][1], MM->RotMat[2][2],
        MM->VIndex, MM->FIndex);
    for (int i = 0; i < 3 && DWord(i) < MM->VIndex; ++i) {
        Vertex &V = MM->Verts[i];
        std::fprintf(stderr,
            "[MIRROR-DUMP %s]  v%d Pos=(%.2f,%.2f,%.2f) TPos=(%.2f,%.2f,%.2f) PX=%.2f PY=%.2f RZ=%.4f Flags=0x%x\n",
            tag, i, V.Pos.x, V.Pos.y, V.Pos.z,
            V.TPos_AOS.x, V.TPos_AOS.y, V.TPos_AOS.z,
            V.PX, V.PY, V.RZ, V.Flags);
    }
    for (int i = 0; i < 3 && DWord(i) < MM->FIndex; ++i) {
        Face &F = MM->Faces[i];
        std::fprintf(stderr,
            "[MIRROR-DUMP %s]  f%d A_idx=%u B_idx=%u C_idx=%u A=%p B=%p C=%p (MM->Verts=%p, range=[%p,%p))\n",
            tag, i, F.A_idx, F.B_idx, F.C_idx,
            (void*)F.A, (void*)F.B, (void*)F.C,
            (void*)MM->Verts, (void*)MM->Verts, (void*)(MM->Verts + MM->VIndex));
    }
}

}  // namespace fds
