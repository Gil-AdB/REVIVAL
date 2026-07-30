#include "WorldAabb.h"
#include "EnvCube.h"          // EnvCube_Basis, kEnvCubePad
#include "OffscreenView.h"    // g_offscreenViewDepth

#include <Base/FDS_VARS.H>    // View, FOVX/FOVY, CntrEX/CntrEY, XRes/YRes, VPage
#include <Base/Scene.h>
#include <Base/Camera.h>
#include <Base/TriMesh.h>
#include <Base/Object.h>
#include <Base/Material.h>
#include <Base/Vector.h>
#include <Base/FeatureFlags.h>

#include <cmath>
#include <cstdint>

namespace fds {

namespace {

// "Is this mesh dynamic for bake purposes?" — byte-for-byte the same
// predicate Transform.cpp (isDynamicForBake) and Shadows.cpp (isMeshDynamic)
// use to partition static vs. moving geometry: own or ancestor Pos spline
// extent > 0.1 world units, or a Rotate spline whose quaternion components
// span > 0.01. Replicated here (small, self-contained) so the AABB module
// doesn't depend on the shadow-bake cache being primed.
bool meshIsDynamic(Object* obj) {
    constexpr float kPosExtentEps = 0.1f;
    constexpr float kRotExtentEps = 0.01f;
    for (Object* o = obj; o; o = o->Parent) {
        if (o->Type != Obj_TriMesh) continue;
        TriMesh* tm = (TriMesh*)o->Data;
        if (!tm) continue;
        if (tm->Pos.NumKeys > 1 && tm->Pos.Keys) {
            const auto& k0 = tm->Pos.Keys[0].Pos;
            float xmn=k0.x,xmx=k0.x,ymn=k0.y,ymx=k0.y,zmn=k0.z,zmx=k0.z;
            for (DWord i = 1; i < tm->Pos.NumKeys; ++i) {
                const auto& k = tm->Pos.Keys[i].Pos;
                if (k.x<xmn) xmn=k.x; if (k.x>xmx) xmx=k.x;
                if (k.y<ymn) ymn=k.y; if (k.y>ymx) ymx=k.y;
                if (k.z<zmn) zmn=k.z; if (k.z>zmx) zmx=k.z;
            }
            if ((xmx-xmn)>kPosExtentEps || (ymx-ymn)>kPosExtentEps ||
                (zmx-zmn)>kPosExtentEps) return true;
        }
        if (tm->Rotate.NumKeys > 1 && tm->Rotate.Keys) {
            const auto& q0 = tm->Rotate.Keys[0].Pos;
            float xmn=q0.x,xmx=q0.x,ymn=q0.y,ymx=q0.y;
            float zmn=q0.z,zmx=q0.z,wmn=q0.W,wmx=q0.W;
            for (DWord i = 1; i < tm->Rotate.NumKeys; ++i) {
                const auto& q = tm->Rotate.Keys[i].Pos;
                if (q.x<xmn) xmn=q.x; if (q.x>xmx) xmx=q.x;
                if (q.y<ymn) ymn=q.y; if (q.y>ymx) ymx=q.y;
                if (q.z<zmn) zmn=q.z; if (q.z>zmx) zmx=q.z;
                if (q.W<wmn) wmn=q.W; if (q.W>wmx) wmx=q.W;
            }
            if ((xmx-xmn)>kRotExtentEps || (ymx-ymn)>kRotExtentEps ||
                (zmx-zmn)>kRotExtentEps || (wmx-wmn)>kRotExtentEps) return true;
        }
    }
    return false;
}

// Model-space (local) AABB from the mesh's vertices — computed once.
void computeLocalAabb(TriMesh* T) {
    float lo[3] = {  1e30f,  1e30f,  1e30f };
    float hi[3] = { -1e30f, -1e30f, -1e30f };
    for (DWord v = 0; v < T->VIndex; ++v) {
        const Vector& p = T->Verts[v].Pos;
        const float c[3] = { p.x, p.y, p.z };
        for (int a = 0; a < 3; ++a) {
            if (c[a] < lo[a]) lo[a] = c[a];
            if (c[a] > hi[a]) hi[a] = c[a];
        }
    }
    T->LocalAabbMin = { lo[0], lo[1], lo[2] };
    T->LocalAabbMax = { hi[0], hi[1], hi[2] };
    T->LocalAabbValid = 1;
}

// World AABB = the tightest axis-aligned box around the eight model-space
// AABB corners transformed by the current pose (RotMat + IPos). Conservative
// under rotation (the world box wraps the rotated local box) — see the
// design note at WorldAabb_UpdateScene.
void computeWorldAabb(TriMesh* T) {
    const Vector& mn = T->LocalAabbMin;
    const Vector& mx = T->LocalAabbMax;
    const float xs[2] = { mn.x, mx.x };
    const float ys[2] = { mn.y, mx.y };
    const float zs[2] = { mn.z, mx.z };
    float lo[3] = {  1e30f,  1e30f,  1e30f };
    float hi[3] = { -1e30f, -1e30f, -1e30f };
    for (int ci = 0; ci < 8; ++ci) {
        Vector local = { xs[ci & 1], ys[(ci >> 1) & 1], zs[(ci >> 2) & 1] };
        Vector w;
        MatrixXVector(T->RotMat, &local, &w);
        w.x += T->IPos.x; w.y += T->IPos.y; w.z += T->IPos.z;
        const float c[3] = { w.x, w.y, w.z };
        for (int a = 0; a < 3; ++a) {
            if (c[a] < lo[a]) lo[a] = c[a];
            if (c[a] > hi[a]) hi[a] = c[a];
        }
    }
    T->WorldAabbMin = { lo[0], lo[1], lo[2] };
    T->WorldAabbMax = { hi[0], hi[1], hi[2] };
    T->WorldAabbValid = 1;
}

// Assemble one world-space plane from a face-frame (view-frame) inward
// normal (nvx,nvy,nvz) and a scalar offset addend `dScalar` such that the
// inside condition is  worldN·W >= worldN·apex + dScalar.
// worldN = nvx*right + nvy*up + nvz*fwd.
inline void addPlane(Frustum& f, const float right[3], const float up[3],
                     const float fwd[3], const float apex[3],
                     float nvx, float nvy, float nvz, float dScalar) {
    float wn[3];
    for (int a = 0; a < 3; ++a)
        wn[a] = nvx * right[a] + nvy * up[a] + nvz * fwd[a];
    const float d = wn[0]*apex[0] + wn[1]*apex[1] + wn[2]*apex[2] + dScalar;
    f.n[f.count][0] = wn[0];
    f.n[f.count][1] = wn[1];
    f.n[f.count][2] = wn[2];
    f.d[f.count] = d;
    ++f.count;
}

}  // namespace

bool WorldAabb_MeshIsDynamic(Object* obj) { return meshIsDynamic(obj); }

void WorldAabb_UpdateScene(Scene* sc) {
    if (!sc) return;
    // Design note: static meshes get their world box ONCE (post first
    // transform — RotMat/IPos are current by the time the tick calls this,
    // after Animate_Objects). Dynamic meshes recompute the world box every
    // call by re-transforming the cached model-space AABB corners through
    // the live pose — the "posed local-AABB corners" option (chosen over
    // spline-extent inflation: it tracks the actual per-frame pose,
    // including rotation, at 8 corner transforms/mesh with no per-vertex
    // re-scan). It is conservative under rotation (the axis-aligned world
    // box wraps the rotated local box) which is exactly what a cull wants.
    for (Object* Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh) continue;
        TriMesh* T = (TriMesh*)Obj->Data;
        if (!T || T->VIndex == 0 || !T->Verts) { if (T) T->WorldAabbValid = 0; continue; }
        if (!T->LocalAabbValid) computeLocalAabb(T);
        const bool dyn = meshIsDynamic(Obj);
        if (T->WorldAabbValid && !dyn) continue;   // static: keep the once-computed box
        computeWorldAabb(T);
    }
}

WorldAabb WorldAabb_ForMaterial(Scene* sc, const Material* M) {
    WorldAabb out;
    if (!sc || !M) return out;
    float lo[3] = {  1e30f,  1e30f,  1e30f };
    float hi[3] = { -1e30f, -1e30f, -1e30f };
    for (TriMesh* T = sc->TriMeshHead; T; T = T->Next) {
        if (!T->WorldAabbValid) continue;
        bool uses = false;
        for (DWord i = 0; i < T->FIndex; ++i)
            if (T->Faces[i].Txtr == M) { uses = true; break; }
        if (!uses) continue;
        const float bmn[3] = { T->WorldAabbMin.x, T->WorldAabbMin.y, T->WorldAabbMin.z };
        const float bmx[3] = { T->WorldAabbMax.x, T->WorldAabbMax.y, T->WorldAabbMax.z };
        for (int a = 0; a < 3; ++a) {
            if (bmn[a] < lo[a]) lo[a] = bmn[a];
            if (bmx[a] > hi[a]) hi[a] = bmx[a];
        }
        out.valid = true;
    }
    if (out.valid)
        for (int a = 0; a < 3; ++a) { out.mn[a] = lo[a]; out.mx[a] = hi[a]; }
    return out;
}

Frustum Frustum_FromMainCamera(Scene* sc) {
    Frustum f;
    if (!sc || !View) return f;
    const float* right = View->Mat[0];
    const float* up    = View->Mat[1];
    const float* fwd   = View->Mat[2];
    const float apex[3] = { View->ISource.x, View->ISource.y, View->ISource.z };
    const float exR = float(XRes) - CntrEX;   // right extent in pixels
    const float eyB = float(YRes) - CntrEY;   // bottom extent in pixels
    const float nearZ = sc->NZP;
    const float farZ  = sc->FZP;
    // Four viewport planes through the camera apex (dScalar = 0), then the
    // near/far depth planes. View-frame inward normals derived from the
    // projection screen_x = CntrEX + FOVX·S.x/S.z, screen_y = CntrEY −
    // FOVY·S.y/S.z (Y flip). See the off-axis test in Transform.cpp.
    addPlane(f, right, up, fwd, apex,  FOVX, 0.0f,  CntrEX, 0.0f);   // left
    addPlane(f, right, up, fwd, apex, -FOVX, 0.0f,  exR,    0.0f);   // right
    addPlane(f, right, up, fwd, apex,  0.0f, -FOVY, CntrEY, 0.0f);   // top
    addPlane(f, right, up, fwd, apex,  0.0f,  FOVY, eyB,    0.0f);   // bottom
    addPlane(f, right, up, fwd, apex,  0.0f, 0.0f,  1.0f,   nearZ);  // near: fwd·(W−P) >= nearZ
    addPlane(f, right, up, fwd, apex,  0.0f, 0.0f, -1.0f,  -farZ);   // far:  fwd·(W−P) <= farZ
    return f;
}

Frustum Frustum_FromProbeFace(const float bake[3], int face, float range) {
    Frustum f;
    if (face < 0 || face >= kEnvCubeFaces) return f;
    const EnvCubeBasisT& B = EnvCube_Basis(face);
    const float* right = B.right;
    const float* up    = B.up;
    const float* fwd   = B.fwd;
    const float pad = kEnvCubePad;
    // Padded cube-face pyramid: the four side planes at the padded half-
    // tangent (kEnvCubePad = tan of the face's half-FOV). Near at the apex
    // (S.z >= 0), far at `range`. This is the tight 4-plane pyramid the
    // Foundation-F frustum machinery gives for free; strictly tighter than
    // a circumscribed cone (see the report's cull derivation).
    addPlane(f, right, up, fwd, bake,  1.0f, 0.0f, pad, 0.0f);      // left
    addPlane(f, right, up, fwd, bake, -1.0f, 0.0f, pad, 0.0f);      // right
    addPlane(f, right, up, fwd, bake,  0.0f, 1.0f, pad, 0.0f);      // bottom
    addPlane(f, right, up, fwd, bake,  0.0f,-1.0f, pad, 0.0f);      // top
    addPlane(f, right, up, fwd, bake,  0.0f, 0.0f, 1.0f, 0.0f);     // near (apex)
    addPlane(f, right, up, fwd, bake,  0.0f, 0.0f,-1.0f, -range);   // far
    return f;
}

bool Frustum_CullsAabb(const Frustum& f, const float mn[3], const float mx[3]) {
    if (f.count == 0) return false;
    for (int i = 0; i < f.count; ++i) {
        const float* n = f.n[i];
        // p-vertex: the box corner maximizing n·W.
        const float pv[3] = {
            n[0] >= 0.0f ? mx[0] : mn[0],
            n[1] >= 0.0f ? mx[1] : mn[1],
            n[2] >= 0.0f ? mx[2] : mn[2],
        };
        if (n[0]*pv[0] + n[1]*pv[1] + n[2]*pv[2] < f.d[i]) return true;  // fully outside plane i
    }
    return false;
}

bool Frustum_CullsAabb(const Frustum& f, const WorldAabb& b) {
    if (!b.valid) return false;
    return Frustum_CullsAabb(f, b.mn, b.mx);
}

bool Frustum_CullsSphere(const Frustum& f, const float c[3], float r) {
    if (f.count == 0) return false;
    for (int i = 0; i < f.count; ++i) {
        const float* n = f.n[i];
        const float sd = n[0]*c[0] + n[1]*c[1] + n[2]*c[2] - f.d[i];   // scaled signed dist
        if (sd < 0.0f) {
            const float n2 = n[0]*n[0] + n[1]*n[1] + n[2]*n[2];
            if (sd*sd > r*r*n2) return true;                           // fully outside plane i
        }
    }
    return false;
}

// ── Debug overlay ─────────────────────────────────────────────────────────
namespace {

// Clip-free-ish line into VPage (dword ARGB). Endpoints already screen-space;
// clamps per-pixel to the viewport.
void drawLine(int x0, int y0, int x1, int y1, uint32_t col) {
    dword* out = reinterpret_cast<dword*>(VPage);
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int guard = 0; guard < 8192; ++guard) {
        if (x0 >= 0 && x0 < XRes && y0 >= 0 && y0 < YRes)
            out[size_t(y0) * XRes + x0] = col | 0xFF000000u;
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

}  // namespace

void WorldAabb_DrawOverlay(Scene* sc) {
    if (!sc || !View || !VPage || XRes <= 0 || YRes <= 0) return;
    if (g_offscreenViewDepth > 0) return;
    const Frustum fr = Frustum_FromMainCamera(sc);
    const Matrix& Mat = View->Mat;
    const Vector P = View->ISource;
    const float nearZ = sc->NZP > 0.01f ? sc->NZP : 0.01f;

    for (Object* Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh) continue;
        TriMesh* T = (TriMesh*)Obj->Data;
        if (!T || !T->WorldAabbValid) continue;
        const float bmn[3] = { T->WorldAabbMin.x, T->WorldAabbMin.y, T->WorldAabbMin.z };
        const float bmx[3] = { T->WorldAabbMax.x, T->WorldAabbMax.y, T->WorldAabbMax.z };
        const bool culled = Frustum_CullsAabb(fr, bmn, bmx);
        const bool dyn = meshIsDynamic(Obj);
        const uint32_t col = culled ? 0x00600000u              // dim red
                                    : (dyn ? 0x00FFFF00u        // yellow
                                           : 0x0000FF00u);      // green

        // Project the eight corners.
        int sx[8], sy[8]; bool ok[8];
        for (int ci = 0; ci < 8; ++ci) {
            Vector w = { bmn[0] + ((ci & 1) ? (bmx[0]-bmn[0]) : 0.0f),
                         bmn[1] + ((ci & 2) ? (bmx[1]-bmn[1]) : 0.0f),
                         bmn[2] + ((ci & 4) ? (bmx[2]-bmn[2]) : 0.0f) };
            Vector d = { w.x - P.x, w.y - P.y, w.z - P.z };
            Vector s;
            MatrixXVector(Mat, &d, &s);
            if (s.z > nearZ) {
                sx[ci] = int(CntrEX + FOVX * s.x / s.z + 0.5f);
                sy[ci] = int(CntrEY - FOVY * s.y / s.z + 0.5f);
                ok[ci] = true;
            } else {
                ok[ci] = false;
            }
        }
        // 12 edges of the box (corner index bit c: 1=X,2=Y,4=Z).
        static const int edges[12][2] = {
            {0,1},{2,3},{4,5},{6,7},   // X
            {0,2},{1,3},{4,6},{5,7},   // Y
            {0,4},{1,5},{2,6},{3,7},   // Z
        };
        for (auto& e : edges)
            if (ok[e[0]] && ok[e[1]])
                drawLine(sx[e[0]], sy[e[0]], sx[e[1]], sy[e[1]], col);
    }
}

}  // namespace fds
