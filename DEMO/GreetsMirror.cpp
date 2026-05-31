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

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Provided by MISC/PREPROC.CPP — stamps F->A_idx/B_idx/C_idx for SoA.
extern void Compute_FaceVertexIndices(TriMesh *T);

// getAlignedBlock / getAlignedType come in via FDS_DECS.H above.

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

    // Animate_Objects expects a Pos/Rotate spline to evaluate even for
    // a static mesh. Allocate single-key splines parked at zero / identity
    // so Spline_Calc_3D fills IPos=(0,0,0) and IRot=identity every frame.
    auto stampSingleKey = [](Spline &sp, const Quaternion &val) {
        sp.NumKeys = 1;
        sp.CurKey  = 0;
        sp.Flags   = 0;
        sp.Keys    = (SplineKey*)std::calloc(1, sizeof(SplineKey));
        sp.Keys[0].Frame = 0.0f;
        sp.Keys[0].Pos   = val;
        sp.Keys[0].AA    = val;
    };
    stampSingleKey(MM->Pos,    {0.0f, 0.0f, 0.0f, 0.0f});
    stampSingleKey(MM->Scale,  {1.0f, 1.0f, 1.0f, 0.0f});
    stampSingleKey(MM->Rotate, {0.0f, 0.0f, 0.0f, 1.0f});

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
            // Reflect the face normal so deferred/lighting see the right
            // direction. NormProd will be recomputed by Compute_FaceNormals
            // (or stays the original — Mekalele recomputes from F.A->Pos).
            CF.N = reflectDir(OF.N);
            ++fOfs;
        }
    }

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
    MM->BSphereRad        = radSq;     // engine convention: radius squared
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

    // Hide the wall: stamp a no-op Filler on every teleporter face. With
    // the wall pixels never written, the mirror geometry behind shows
    // through; the surrounding Piramid walls (still rendered) provide
    // the visual frame so the mirror only shows in the wall's footprint.
    auto noopFiller = [](Face*, Vertex**, DWord, DWord,
                         const fds::RenderTarget&,
                         const fds::CameraContext&) {};
    int wallFacesHidden = 0;
    for (Object *Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh) continue;
        if (Obj == MObj) continue;  // don't touch the clone
        TriMesh *T = (TriMesh*)Obj->Data;
        if (!T || !T->Faces) continue;
        for (DWord fi = 0; fi < T->FIndex; ++fi) {
            Face &F = T->Faces[fi];
            if (!isTeleporter(F)) continue;
            F.Filler = noopFiller;
            ++wallFacesHidden;
        }
    }

    std::fprintf(stderr,
        "[MIRROR] cloned %u verts / %u faces into 'mirror_clone'; "
        "hid %d teleporter wall faces (bsphere ctr=(%.1f,%.1f,%.1f) rad²=%.1f)\n",
        unsigned(vOfs), unsigned(fOfs), wallFacesHidden,
        ctr.x, ctr.y, ctr.z, radSq);
    return int(fOfs);
}

}  // namespace fds
