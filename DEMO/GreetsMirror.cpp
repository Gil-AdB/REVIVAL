#include "GreetsMirror.h"

#include <Base/FDS_VARS.H>  // MatrixXVector template
#include <Base/Face.h>
#include <Base/Material.h>
#include <Base/Matrix.h>
#include <Base/Object.h>
#include <Base/Scene.h>
#include <Base/TriMesh.h>

#include <cmath>
#include <cstdio>
#include <cstring>

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

}  // namespace fds
