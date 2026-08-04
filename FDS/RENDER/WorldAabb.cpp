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
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

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

// Depth-tested variant for --displace_viz: interpolate view-space z along the
// line and reject pixels the frame's OPAQUE geometry occludes (ZPage16, enc
// zEnc = 0xFF80 - g_zscale*z, larger = nearer, 0 = untouched/sky — never
// occludes). The line's own z is pulled 1% nearer before encoding so a
// wireframe lying exactly ON the surface it annotates wins the compare
// instead of z-fighting; genuinely hidden geometry (a far wall behind a near
// one) stays hidden — the readability fix over the old draw-through overlay.
// Falls back to the depth-free draw when ZPage16 isn't live.
void drawLineZ(int x0, int y0, float z0, int x1, int y1, float z1, uint32_t col) {
    if (!ZPage16) { drawLine(x0, y0, x1, y1, col); return; }
    dword* out = reinterpret_cast<dword*>(VPage);
    const word* zb = ZPage16;
    const float zs = float(g_zscale);
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    const int steps = std::max(dx, -dy);
    const float invSteps = steps > 0 ? 1.0f / float(steps) : 0.0f;
    int err = dx + dy;
    int step = 0;
    for (int guard = 0; guard < 8192; ++guard) {
        if (x0 >= 0 && x0 < XRes && y0 >= 0 && y0 < YRes) {
            const float z = (z0 + (z1 - z0) * (float(step) * invSteps)) * 0.99f;
            int zEnc = 0xFF80 - int(zs * z);
            if (zEnc < 0) zEnc = 0; else if (zEnc > 0xFFFF) zEnc = 0xFFFF;
            const word surf = zb[size_t(y0) * XRes + x0];
            if (surf == 0 || word(zEnc) >= surf)
                out[size_t(y0) * XRes + x0] = col | 0xFF000000u;
        }
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
        ++step;   // one Bresenham iteration == one major-axis step
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

// ── --displace_viz overlay ────────────────────────────────────────────────
namespace {

// Per-vertex displacement magnitude, keyed by EXACT model-space position bits.
// Position bits survive the greets per-cell chunk copy (bitwise) and the
// per-face MakeFacesIndependent split (Pos copied verbatim), so a render-time
// chunk vertex resolves back to what the bake recorded. Collisions between two
// truly-coincident verts are harmless for a debug tint.
struct DispPosKey {
    uint32_t x, y, z;
    bool operator==(const DispPosKey& o) const { return x==o.x && y==o.y && z==o.z; }
};
struct DispPosHash {
    size_t operator()(const DispPosKey& k) const {
        return (size_t(k.x)*73856093u) ^ (size_t(k.y)*19349663u) ^ (size_t(k.z)*83492791u);
    }
};
std::unordered_map<DispPosKey, float, DispPosHash> g_dispMag;   // pos bits -> |offset|
std::unordered_set<const Material*>                g_dispMats;  // displaced materials
float g_dispMax = 0.0f;                                          // for normalization
// --displace_viz=2: per-displaced-triangle SIGNED height error (truth − carried,
// world units), keyed by the triangle's final centroid position bits.
std::unordered_map<DispPosKey, float, DispPosHash> g_dispErr;
float g_dispErrMax = 0.0f;                                       // max |err| for normalization

inline uint32_t f2bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline DispPosKey posKey(const Vector& p) { return { f2bits(p.x), f2bits(p.y), f2bits(p.z) }; }

// Cool→warm ramp (blue=0 → green → red=1). Returns 0x00RRGGBB (drawLine ORs
// the alpha). Matches the flag help's "cool=0, warm=max".
inline uint32_t rampColor(float t) {
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    int r, g, b;
    if (t < 0.5f) { const float u = t * 2.0f;         r = 0;               g = int(255.0f*u);       b = int(255.0f*(1.0f-u)); }
    else          { const float u = (t - 0.5f) * 2.0f; r = int(255.0f*u);  g = int(255.0f*(1.0f-u)); b = 0; }
    return (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
}

// Diverging error ramp for --displace_viz=2, signed error e in [-1,1]:
// GREEN at 0 (geometry matches the map) → RED at +1 (UNDER-carries, missing
// relief) → BLUE at -1 (OVER-carries). Returns 0x00RRGGBB.
inline uint32_t divergeColor(float e) {
    if (e < -1.0f) e = -1.0f; else if (e > 1.0f) e = 1.0f;
    const float m = std::fabs(e);
    int r, g, b;
    if (e >= 0.0f) { r = int(255.0f*m); g = int(255.0f*(1.0f-m)); b = 0; }          // green→red
    else           { b = int(255.0f*m); g = int(255.0f*(1.0f-m)); r = 0; }          // green→blue
    return (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
}

// Depth-tested, alpha-blended flat triangle fill for --displace_viz=2. Screen
// verts + per-vertex view-space z; each covered pixel interpolates z (screen-
// linear barycentric — matches drawLineZ), rejects where the frame's opaque Z
// (ZPage16, enc 0xFF80 − g_zscale*z; the fill z pulled 1% nearer so a triangle
// ON its own surface wins) is nearer, then blends `col` at `alpha` over VPage.
// Falls back to no depth test when ZPage16 isn't live.
void fillTriZ(const int px[3], const int py[3], const float pz[3],
              uint32_t col, float alpha) {
    dword* out = reinterpret_cast<dword*>(VPage);
    const word* zb = ZPage16;
    const float zs = float(g_zscale);
    int xmin = std::min(px[0], std::min(px[1], px[2]));
    int xmax = std::max(px[0], std::max(px[1], px[2]));
    int ymin = std::min(py[0], std::min(py[1], py[2]));
    int ymax = std::max(py[0], std::max(py[1], py[2]));
    if (xmin < 0) xmin = 0; if (ymin < 0) ymin = 0;
    if (xmax >= XRes) xmax = XRes - 1; if (ymax >= YRes) ymax = YRes - 1;
    if (xmin > xmax || ymin > ymax) return;
    const float ax = float(px[0]), ay = float(py[0]);
    const float bx = float(px[1]), by = float(py[1]);
    const float cx = float(px[2]), cy = float(py[2]);
    const float area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
    if (std::fabs(area) < 1e-3f) return;
    const float invArea = 1.0f / area;
    const int cr = int((col >> 16) & 0xFF), cg = int((col >> 8) & 0xFF), cb = int(col & 0xFF);
    for (int y = ymin; y <= ymax; ++y) {
        const float fy = float(y) + 0.5f;
        for (int x = xmin; x <= xmax; ++x) {
            const float fx = float(x) + 0.5f;
            const float w0 = ((bx - fx) * (cy - fy) - (cx - fx) * (by - fy)) * invArea;
            const float w1 = ((cx - fx) * (ay - fy) - (ax - fx) * (cy - fy)) * invArea;
            const float w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;   // outside
            const size_t idx = size_t(y) * XRes + x;
            if (zb) {
                const float z = (w0 * pz[0] + w1 * pz[1] + w2 * pz[2]) * 0.99f;
                int zEnc = 0xFF80 - int(zs * z);
                if (zEnc < 0) zEnc = 0; else if (zEnc > 0xFFFF) zEnc = 0xFFFF;
                const word surf = zb[idx];
                if (surf != 0 && word(zEnc) < surf) continue;    // occluded
            }
            const dword d = out[idx];
            const int dr = int((d >> 16) & 0xFF), dg = int((d >> 8) & 0xFF), db = int(d & 0xFF);
            const int rr = int(cr * alpha + dr * (1.0f - alpha));
            const int rg = int(cg * alpha + dg * (1.0f - alpha));
            const int rb = int(cb * alpha + db * (1.0f - alpha));
            out[idx] = 0xFF000000u | (uint32_t(rr) << 16) | (uint32_t(rg) << 8) | uint32_t(rb);
        }
    }
}

}  // namespace

void DisplaceViz_Record(const Material* M, const Vector& localPos, float dispAbs) {
    if (!FeatureFlags::displace_viz()) return;   // flag-off: record nothing
    if (M) g_dispMats.insert(M);
    const DispPosKey k = posKey(localPos);
    auto it = g_dispMag.find(k);
    if (it == g_dispMag.end() || dispAbs > it->second) g_dispMag[k] = dispAbs;
    if (dispAbs > g_dispMax) g_dispMax = dispAbs;
}

// signedErrFrac is PRE-NORMALIZED by the bake to an absolute fraction where
// ±1 = the map's full peak-to-valley relief missing (see MeshOps viz-2 record).
// We store it as-is and tint by the absolute value — no global-max rescale (the
// old 1/g_dispErrMax wash let a single worst edge cell flatten everything to
// green). g_dispErrMax is kept only for the stderr summary line.
void DisplaceViz_RecordError(const Material* M, const Vector& centroidLocal, float signedErrFrac) {
    if (FeatureFlags::displace_viz() != 2) return;   // mode 0/1: record nothing
    if (M) g_dispMats.insert(M);
    g_dispErr[posKey(centroidLocal)] = signedErrFrac;
    const float a = std::fabs(signedErrFrac);
    if (a > g_dispErrMax) g_dispErrMax = a;
}

void DisplaceViz_DrawOverlay(Scene* sc) {
    if (!sc || !View || !VPage || XRes <= 0 || YRes <= 0) return;
    if (g_offscreenViewDepth > 0) return;
    if (g_dispMats.empty()) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr, "[DISPLACE-VIZ] nothing to show: no displaced "
                "geometry was recorded. --displace_viz needs --greets_displace "
                "(the stone-displacement bake) ON.\n");
        }
        return;
    }
    const int     mode   = FeatureFlags::displace_viz();   // 1 = magnitude, 2 = error
    const Matrix& VM = View->Mat;
    const Vector  P  = View->ISource;
    const float   nearZ  = sc->NZP > 0.01f ? sc->NZP : 0.01f;
    const float   invMax = g_dispMax    > 1e-9f ? (1.0f / g_dispMax)    : 0.0f;
    // --displace_viz=2 fill rule: error is an absolute fraction of the map's
    // relief. Matched cells (|frac| < kMatchThresh) get NO fill — the wireframe
    // alone reads them, so the eye is drawn only to where geometry FAILS the
    // map. Above the threshold the fill fades in (fainter → stronger) and tints
    // RED (under-carries) / BLUE (over).
    const float kMatchThresh = 0.15f;   // <15% of block relief missing = "matched"
    if (mode == 2) {
        static bool once = false;
        if (!once) { once = true;
            std::fprintf(stderr, "[DISPLACE-VIZ] mode 2 (height error): worst |truth-carried| "
                "= %.2f of the map's peak-to-valley relief; RED=under-carries (missing "
                "relief), BLUE=over, unfilled=matched (<%.0f%%).\n",
                (double)g_dispErrMax, (double)(kMatchThresh * 100.0f)); }
    }

    for (Object* Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh) continue;
        TriMesh* T = (TriMesh*)Obj->Data;
        if (!T || T->FIndex == 0 || !T->Faces || !T->Verts) continue;
        for (DWord fi = 0; fi < T->FIndex; ++fi) {
            const Face& F = T->Faces[fi];
            if (!F.A || !F.B || !F.C) continue;
            if (g_dispMats.find(F.Txtr) == g_dispMats.end()) continue;  // not a displaced material
            Vertex* const corner[3] = { F.A, F.B, F.C };

            // Local → world (RotMat * Pos + IPos) then project like the AABB
            // overlay. Back-face + near-plane cull to cut clutter.
            Vector w[3];
            int sx[3], sy[3]; bool ok[3];
            float cog[3] = { 0, 0, 0 };
            for (int k = 0; k < 3; ++k) {
                const Vector& lp = corner[k]->Pos;
                MatrixXVector(T->RotMat, &lp, &w[k]);
                w[k].x += T->IPos.x; w[k].y += T->IPos.y; w[k].z += T->IPos.z;
                cog[0] += w[k].x; cog[1] += w[k].y; cog[2] += w[k].z;
            }
            // World face normal (RotMat * F.N); skip faces pointing away.
            Vector wn; MatrixXVector(T->RotMat, &F.N, &wn);
            const float vx = cog[0]/3.0f - P.x, vy = cog[1]/3.0f - P.y, vz = cog[2]/3.0f - P.z;
            if (wn.x*vx + wn.y*vy + wn.z*vz > 0.0f) continue;   // back-facing

            float mag[3], vzs[3];
            for (int k = 0; k < 3; ++k) {
                Vector d = { w[k].x - P.x, w[k].y - P.y, w[k].z - P.z };
                Vector s; MatrixXVector(VM, &d, &s);
                if (s.z > nearZ) {
                    sx[k] = int(CntrEX + FOVX * s.x / s.z + 0.5f);
                    sy[k] = int(CntrEY - FOVY * s.y / s.z + 0.5f);
                    ok[k] = true;
                } else {
                    ok[k] = false;
                }
                vzs[k] = s.z;
                auto it = g_dispMag.find(posKey(corner[k]->Pos));
                mag[k] = (it != g_dispMag.end()) ? it->second * invMax : 0.0f;
            }
            static const int e3[3][2] = { {0,1}, {1,2}, {2,0} };
            if (mode == 2) {
                // HEIGHT-ERROR field: fill the triangle tinted by its centroid's
                // signed error (recorded at bake), then trace dim edges so the
                // cell grid stays legible over the fill.
                const Vector clocal = { (corner[0]->Pos.x + corner[1]->Pos.x + corner[2]->Pos.x) / 3.0f,
                                        (corner[0]->Pos.y + corner[1]->Pos.y + corner[2]->Pos.y) / 3.0f,
                                        (corner[0]->Pos.z + corner[1]->Pos.z + corner[2]->Pos.z) / 3.0f };
                auto it = g_dispErr.find(posKey(clocal));
                if (it != g_dispErr.end() && ok[0] && ok[1] && ok[2]) {
                    const float frac = it->second;              // signed, absolute scale
                    const float m = std::fabs(frac);
                    if (m >= kMatchThresh) {                     // matched → no fill
                        const float t = (m - kMatchThresh) / (1.0f - kMatchThresh);
                        const float alpha = 0.22f + 0.50f * (t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t);
                        fillTriZ(sx, sy, vzs, divergeColor(frac), alpha);
                    }
                }
                for (auto& e : e3)
                    if (ok[e[0]] && ok[e[1]])
                        drawLineZ(sx[e[0]], sy[e[0]], vzs[e[0]],
                                  sx[e[1]], sy[e[1]], vzs[e[1]], 0x00303030u);
            } else {
                for (auto& e : e3)
                    if (ok[e[0]] && ok[e[1]])
                        drawLineZ(sx[e[0]], sy[e[0]], vzs[e[0]],
                                  sx[e[1]], sy[e[1]], vzs[e[1]],
                                  rampColor(0.5f * (mag[e[0]] + mag[e[1]])));
            }
        }
    }
}

// ── --pom_seam_viz overlay (S1d-1) ────────────────────────────────────────
namespace {
struct SeamSeg { Vector a, b; int cls; };
std::vector<SeamSeg> g_seamSegs;
// Class colours. Deliberately saturated and far apart in hue — the point of
// this overlay is that the classification can be eyeballed, not trusted.
inline uint32_t seamColor(int cls) {
    switch (cls) {
    case 0:  return 0x0000FF40u;   // COPLANAR      — green
    case 1:  return 0x00FF9000u;   // ANGLED_IN     — orange
    case 2:  return 0x00FF00FFu;   // ANGLED_OUT    — magenta
    default: return 0x00FF2020u;   // TRUE_BOUNDARY — red
    }
}
}  // namespace

void PomSeamViz_Record(const Vector& aLocal, const Vector& bLocal, int cls) {
    if (g_seamSegs.size() > 200000) return;    // debug guard
    g_seamSegs.push_back({ aLocal, bLocal, cls });
}

void PomSeamViz_DrawOverlay(Scene* sc) {
    if (!sc || !View || !VPage || XRes <= 0 || YRes <= 0) return;
    if (g_offscreenViewDepth > 0) return;
    const int mode = FeatureFlags::pom_seam_viz();
    if (mode <= 0) return;
    if (g_seamSegs.empty()) {
        static bool warned = false;
        if (!warned) { warned = true;
            std::fprintf(stderr, "[POM-SEAM-VIZ] nothing recorded: --pom_seam_viz "
                "needs --pom_shell (PomShell_Build is what builds the patches). "
                "Run it with --pom_recess_only so the topology is the authored one.\n"); }
        return;
    }
    // The seams were recorded in the Piramid mesh's MODEL space, before the
    // chunk split; every chunk inherits that object's transform, so one lookup
    // places all of them.
    const TriMesh* ref = nullptr;
    for (Object* Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh || !Obj->Name || !Obj->Data) continue;
        if (std::strstr(Obj->Name, "Piramid")) { ref = (const TriMesh*)Obj->Data; break; }
    }
    if (!ref) return;
    const Matrix& VM = View->Mat;
    const Vector  P  = View->ISource;
    const float   nearZ = sc->NZP > 0.01f ? sc->NZP : 0.01f;
    static bool once = false;
    if (!once) { once = true;
        std::fprintf(stderr, "[POM-SEAM-VIZ] drawing %zu classified patch-boundary "
            "edges (mode %d): GREEN=coplanar continuation, ORANGE=angled-in "
            "(enter the neighbour), MAGENTA=angled-out (true silhouette), "
            "RED=true boundary (side faces belong here)\n", g_seamSegs.size(), mode); }
    for (const SeamSeg& s : g_seamSegs) {
        if (mode == 2 && s.cls != 0) continue;
        if (mode == 3 && !(s.cls == 1 || s.cls == 2)) continue;
        if (mode == 4 && s.cls != 3) continue;
        const Vector* lp[2] = { &s.a, &s.b };
        int sx[2], sy[2]; float vz[2]; bool ok[2];
        for (int k = 0; k < 2; ++k) {
            Vector w; MatrixXVector(ref->RotMat, lp[k], &w);
            w.x += ref->IPos.x; w.y += ref->IPos.y; w.z += ref->IPos.z;
            Vector dv = { w.x - P.x, w.y - P.y, w.z - P.z };
            Vector sv; MatrixXVector(VM, &dv, &sv);
            if (sv.z > nearZ) {
                sx[k] = int(CntrEX + FOVX * sv.x / sv.z + 0.5f);
                sy[k] = int(CntrEY - FOVY * sv.y / sv.z + 0.5f);
                ok[k] = true;
            } else ok[k] = false;
            vz[k] = sv.z;
        }
        if (ok[0] && ok[1])
            drawLineZ(sx[0], sy[0], vz[0], sx[1], sy[1], vz[1], seamColor(s.cls));
    }
}

}  // namespace fds
