#include "WorldAabb.h"
#include "EnvCube.h"          // EnvCube_Basis, kEnvCubePad
#include "OffscreenView.h"    // g_offscreenViewDepth
#include "VizLegend.h"        // WireViz_Legend / DisplaceViz_Legend / PomSeamViz_Legend

#include <Base/FDS_VARS.H>    // View, FOVX/FOVY, CntrEX/CntrEY, XRes/YRes, VPage
#include <Base/Scene.h>
#include <Base/Camera.h>
#include <Base/TriMesh.h>
#include <Base/Object.h>
#include <Base/Material.h>
#include <Base/Vector.h>
#include <Base/FeatureFlags.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
// --displace_viz=3/4: per-displaced-vertex full displacement VECTOR
// (final − base, model space), keyed by the FINAL position bits like g_dispMag.
// base is recoverable as final − vec, which is how the overlay reconstructs
// the pre-bake wall plane per triangle.
std::unordered_map<DispPosKey, Vector, DispPosHash> g_dispVec;
// --displace_viz=2: per-displaced-triangle SIGNED height error (truth − carried,
// world units), keyed by the triangle's final centroid position bits.
std::unordered_map<DispPosKey, float, DispPosHash> g_dispErr;
float g_dispErrMax = 0.0f;                                       // max |err| for normalization

// --displace_viz=2 fill rule: error is an absolute fraction of the map's
// relief, and cells under this threshold get NO fill (the wireframe alone reads
// them) so the eye is drawn only to where geometry FAILS the map. At namespace
// scope because DisplaceViz_Legend quotes it — a legend that re-typed "15%"
// would be a second source of truth for the same rule.
constexpr float kDispMatchThresh = 0.15f;

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
    // --viz_arm records even with the viz flag off, so the runtime cycle (X key)
    // can switch INTO --displace_viz mid-flight — the bake that fills this map
    // ran at scene init, long before the key press.
    if (!FeatureFlags::displace_viz() && !FeatureFlags::viz_arm()) return;
    if (M) g_dispMats.insert(M);
    const DispPosKey k = posKey(localPos);
    auto it = g_dispMag.find(k);
    if (it == g_dispMag.end() || dispAbs > it->second) g_dispMag[k] = dispAbs;
    if (dispAbs > g_dispMax) g_dispMax = dispAbs;
}

void DisplaceViz_RecordVec(const Material* M, const Vector& finalLocal, const Vector& dispLocal) {
    // Same arming rule as DisplaceViz_Record: --viz_arm records with the viz
    // flag off so the runtime cycle can switch into mode 3/4 mid-flight.
    if (!FeatureFlags::displace_viz() && !FeatureFlags::viz_arm()) return;
    if (M) g_dispMats.insert(M);
    g_dispVec[posKey(finalLocal)] = dispLocal;
}

// signedErrFrac is PRE-NORMALIZED by the bake to an absolute fraction where
// ±1 = the map's full peak-to-valley relief missing (see MeshOps viz-2 record).
// We store it as-is and tint by the absolute value — no global-max rescale (the
// old 1/g_dispErrMax wash let a single worst edge cell flatten everything to
// green). g_dispErrMax is kept only for the stderr summary line.
void DisplaceViz_RecordError(const Material* M, const Vector& centroidLocal, float signedErrFrac) {
    // --viz_arm is accepted here too, but NOTE it does not actually arm mode 2:
    // the CALLER (MeshOps DisplaceStoneSubdiv) wraps the whole error computation
    // in `if (displace_viz() == 2)`, so with the flag off this function is never
    // reached. Mode 2 therefore still needs --displace_viz=2 at STARTUP, and the
    // runtime cycle drops it (DisplaceViz_HasErrorData) when the data is absent.
    if (FeatureFlags::displace_viz() != 2 && !FeatureFlags::viz_arm()) return;
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
    const int     mode   = FeatureFlags::displace_viz();   // 1=magnitude 2=error 3=dir/height 4=needles
    // mode 4: one needle per unique vertex even though verts are visited once
    // per incident face. Rebuilt every frame (the set is tiny next to the draw).
    std::unordered_set<const void*> seenNeedle;
    // mode 3/4 flush rule: a vertex carrying under 2% of the bake's max push is
    // "flush" — it has NO height, which is a different defect than a wrong
    // direction, so it gets its own colour (solid blue) instead of a hue.
    const float flushEps = 0.02f * g_dispMax;
    const Matrix& VM = View->Mat;
    const Vector  P  = View->ISource;
    const float   nearZ  = sc->NZP > 0.01f ? sc->NZP : 0.01f;
    const float   invMax = g_dispMax    > 1e-9f ? (1.0f / g_dispMax)    : 0.0f;
    // --displace_viz=2 fill rule: error is an absolute fraction of the map's
    // relief. Matched cells (|frac| < kDispMatchThresh) get NO fill — the
    // wireframe alone reads them, so the eye is drawn only to where geometry
    // FAILS the map. Above the threshold the fill fades in (fainter → stronger)
    // and tints RED (under-carries) / BLUE (over). The threshold lives at
    // namespace scope so the on-screen legend quotes this exact number.
    const float kMatchThresh = kDispMatchThresh;
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
            // Modes 3/4 must NOT trust this filter: the greets ::mirUV
            // handedness split (GREETS.CPP GreetsFixBitangentHandedness) runs
            // AFTER the bake and moves displaced faces onto clone Materials the
            // recorder never saw — the pier at the user's t=5965 cams is 92%
            // 'rooms::mirUV' and the pointer filter made the overlay skip
            // exactly the walls under review. For 3/4 the per-corner position
            // lookups are the filter (a face whose corners weren't recorded
            // draws nothing); for 1/2 keep the original pointer gate.
            const bool matKnown = g_dispMats.find(F.Txtr) != g_dispMats.end();
            if (!matKnown && !(mode == 3 || mode == 4)) continue;
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
            } else if (mode == 3 || mode == 4) {
                // DIRECTION/HEIGHT combination. Per corner: the displacement
                // VECTOR the bake actually applied (final − base), judged
                // against the triangle's BASE plane (base = final − vec), i.e.
                // the pre-bake wall plane. Deviation is measured against the
                // plane's AXIS (|dot|), so winding orientation cannot flip it:
                // 0° = rode the wall normal (interiors must read this), ~45° =
                // a legitimate mitre at a square corner, 90° = sliding ALONG
                // the wall. A corner carrying ~no displacement at all is FLUSH
                // (solid blue) — no height, a different failure than direction.
                Vector dv[3]; bool have = true;
                for (int k = 0; k < 3; ++k) {
                    auto it = g_dispVec.find(posKey(corner[k]->Pos));
                    if (it == g_dispVec.end()) { have = false; break; }
                    dv[k] = it->second;
                }
                if (have) {
                    Vector bp[3];
                    for (int k = 0; k < 3; ++k)
                        bp[k] = Vector{ corner[k]->Pos.x - dv[k].x,
                                        corner[k]->Pos.y - dv[k].y,
                                        corner[k]->Pos.z - dv[k].z };
                    const float e1x=bp[1].x-bp[0].x, e1y=bp[1].y-bp[0].y, e1z=bp[1].z-bp[0].z;
                    const float e2x=bp[2].x-bp[0].x, e2y=bp[2].y-bp[0].y, e2z=bp[2].z-bp[0].z;
                    float nbx=e1y*e2z-e1z*e2y, nby=e1z*e2x-e1x*e2z, nbz=e1x*e2y-e1y*e2x;
                    const float nbl = std::sqrt(nbx*nbx+nby*nby+nbz*nbz);
                    if (nbl > 1e-9f) {
                        nbx/=nbl; nby/=nbl; nbz/=nbl;
                        float dev[3], m[3]; bool flush[3];
                        for (int k = 0; k < 3; ++k) {
                            m[k] = std::sqrt(dv[k].x*dv[k].x + dv[k].y*dv[k].y + dv[k].z*dv[k].z);
                            flush[k] = m[k] < flushEps;
                            if (flush[k]) { dev[k] = 0.0f; continue; }
                            float c = std::fabs((dv[k].x*nbx + dv[k].y*nby + dv[k].z*nbz) / m[k]);
                            if (c > 1.0f) c = 1.0f;
                            dev[k] = std::acos(c) * 57.29578f;
                        }
                        if (mode == 3) {
                            if (ok[0] && ok[1] && ok[2]) {
                                if (flush[0] && flush[1] && flush[2]) {
                                    fillTriZ(sx, sy, vzs, 0x002858D0u, 0.45f);   // FLUSH: no height
                                } else {
                                    float maxDev = 0.0f, meanM = 0.0f; int nM = 0;
                                    for (int k = 0; k < 3; ++k) {
                                        if (flush[k]) continue;
                                        if (dev[k] > maxDev) maxDev = dev[k];
                                        meanM += m[k]; ++nM;
                                    }
                                    meanM = nM ? meanM / float(nM) : 0.0f;
                                    const float t = 0.5f + 0.5f * (maxDev > 90.0f ? 1.0f : maxDev / 90.0f);
                                    uint32_t col = rampColor(t);                 // green→red
                                    const float br = 0.35f + 0.65f * (g_dispMax > 1e-9f ? meanM / g_dispMax : 0.0f);
                                    const int r = int(((col>>16)&0xFF) * br), g = int(((col>>8)&0xFF) * br), b = int((col&0xFF) * br);
                                    col = (uint32_t(r)<<16)|(uint32_t(g)<<8)|uint32_t(b);
                                    fillTriZ(sx, sy, vzs, col, 0.55f);
                                }
                            }
                            for (auto& e : e3)
                                if (ok[e[0]] && ok[e[1]])
                                    drawLineZ(sx[e[0]], sy[e[0]], vzs[e[0]],
                                              sx[e[1]], sy[e[1]], vzs[e[1]], 0x00303030u);
                        } else {   // mode 4: needles
                            for (auto& e : e3)
                                if (ok[e[0]] && ok[e[1]])
                                    drawLineZ(sx[e[0]], sy[e[0]], vzs[e[0]],
                                              sx[e[1]], sy[e[1]], vzs[e[1]], 0x00282828u);
                            for (int k = 0; k < 3; ++k) {
                                if (!seenNeedle.insert(corner[k]).second) continue;
                                // world base → world tip (vector ×3 for legibility)
                                Vector tipL = { bp[k].x + dv[k].x*3.0f, bp[k].y + dv[k].y*3.0f, bp[k].z + dv[k].z*3.0f };
                                const Vector* lp[2] = { &bp[k], &tipL };
                                int nsx[2], nsy[2]; float nvz[2]; bool nok[2];
                                for (int q = 0; q < 2; ++q) {
                                    Vector ww; MatrixXVector(T->RotMat, lp[q], &ww);
                                    ww.x += T->IPos.x; ww.y += T->IPos.y; ww.z += T->IPos.z;
                                    Vector dd = { ww.x - P.x, ww.y - P.y, ww.z - P.z };
                                    Vector ss; MatrixXVector(VM, &dd, &ss);
                                    if (ss.z > nearZ) {
                                        nsx[q] = int(CntrEX + FOVX * ss.x / ss.z + 0.5f);
                                        nsy[q] = int(CntrEY - FOVY * ss.y / ss.z + 0.5f);
                                        nok[q] = true;
                                    } else nok[q] = false;
                                    nvz[q] = ss.z;
                                }
                                if (!nok[0] || !nok[1]) continue;
                                uint32_t col;
                                if (flush[k]) col = 0x004080FFu;                 // FLUSH marker
                                else {
                                    const float t = 0.5f + 0.5f * (dev[k] > 90.0f ? 1.0f : dev[k] / 90.0f);
                                    col = rampColor(t);
                                }
                                drawLineZ(nsx[0], nsy[0], nvz[0], nsx[1], nsy[1], nvz[1], col);
                            }
                        }
                    }
                }
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

// ── Arming probes (runtime viz cycle) ─────────────────────────────────────
bool DisplaceViz_HasData()      { return !g_dispMag.empty(); }
bool DisplaceViz_HasErrorData() { return !g_dispErr.empty(); }
bool DisplaceViz_HasVecData()   { return !g_dispVec.empty(); }
bool PomSeamViz_HasData()       { return !g_seamSegs.empty(); }

// ── --wire_viz overlay: whole-scene triangle wireframe ────────────────────
namespace {

// Per-material hue, indexed by a hash of the Material POINTER. Same 12-entry
// palette --poly_viz uses so the two vizzes read alike; the pointer hash (not
// a matID) keeps this module free of the material-table dependency, and it is
// stable for the whole run, which is all "which surface owns this face" needs.
// At namespace scope (was a function-local static) so WireViz_Legend can put
// the real chips on screen instead of restating the palette in prose.
const uint32_t kWireMatHue[12] = {
    0x00E04040u, 0x0040E040u, 0x004060E0u, 0x00E0E040u,
    0x00E040E0u, 0x0040E0E0u, 0x00E09030u, 0x009040E0u,
    0x0040A070u, 0x00B06060u, 0x006090B0u, 0x00C0C0C0u };
// Edge colours the overlay picks; named so the legend cannot drift from them.
constexpr uint32_t kWirePlainCol = 0x00D8D8D8u;
constexpr uint32_t kWireBackCol  = 0x00FF3030u;
constexpr uint32_t kWireFrontCol = 0x0030FF30u;

inline uint32_t wireMatColor(const Material* M, const TriMesh* T) {
    const uint32_t mh = uint32_t(uintptr_t(M) >> 4) * 2654435761u;
    uint32_t c = kWireMatHue[mh % 12u];
    // Per-MESH brightness step so a chunk boundary inside one material is
    // visible (greets splits its stone into per-cell chunk meshes).
    const uint32_t th = uint32_t(uintptr_t(T) >> 4) * 2246822519u;
    const int sc = 88 + int((th >> 25) & 0x27);      // 88..127 of 128
    uint32_t o = 0;
    for (int ch = 0; ch < 3; ++ch) {
        int v = int((c >> (8 * ch)) & 0xFFu) * sc / 128;
        if (v > 255) v = 255;
        o |= uint32_t(v) << (8 * ch);
    }
    return o;
}

// Draw ONE view-space segment: near-plane clip → project → 2D viewport clip →
// perspective-correct subdivision → drawLineZ.
//
// Every step here is load-bearing, and the naive version (project both ends,
// require both in front of the near plane, one drawLineZ) FAILED on exactly the
// geometry this viz exists for. The greets corridor walls are a handful of very
// large quads: viewed from inside the corridor BOTH endpoints of a wall edge are
// behind the eye or far outside the viewport, so the naive draw skipped the wall
// entirely — the first render of this overlay showed a wireframed statue in a
// wall-less corridor. Hence:
//   • near clip, so an edge with one endpoint behind the eye still draws;
//   • Liang-Barsky viewport clip, so drawLineZ's 8192-step Bresenham guard is
//     spent on visible pixels instead of walking in from far off-screen;
//   • subdivision with 1/z (perspective-correct) endpoint depths, because
//     drawLineZ interpolates z LINEARLY in screen space. Over a wall-length edge
//     that overestimates z in the middle (true z is the harmonic interpolation),
//     which reads as "farther" and lets the surface the edge lies on occlude it.
//     ~48 px sub-segments keep the residual well inside the 1% pull-nearer bias.
// Returns the number of sub-segments drawn (0 = fully clipped away).
int drawSegZClipped(Vector a, Vector b, float nearZ, uint32_t col) {
    if (a.z <= nearZ && b.z <= nearZ) return 0;
    if (a.z <= nearZ) {
        const float t = (nearZ - a.z) / (b.z - a.z);
        a.x += (b.x - a.x) * t; a.y += (b.y - a.y) * t; a.z = nearZ;
    } else if (b.z <= nearZ) {
        const float t = (nearZ - b.z) / (a.z - b.z);
        b.x += (a.x - b.x) * t; b.y += (a.y - b.y) * t; b.z = nearZ;
    }
    float x0 = CntrEX + FOVX * a.x / a.z, y0 = CntrEY - FOVY * a.y / a.z;
    float x1 = CntrEX + FOVX * b.x / b.z, y1 = CntrEY - FOVY * b.y / b.z;
    const float iz0 = 1.0f / a.z, iz1 = 1.0f / b.z;

    float t0 = 0.0f, t1 = 1.0f;
    const float dx = x1 - x0, dy = y1 - y0;
    // Liang-Barsky: keep the sub-range of t where p*t <= q holds for all edges.
    auto clip = [&](float p, float q) -> bool {
        if (p == 0.0f) return q >= 0.0f;
        const float r = q / p;
        if (p < 0.0f) { if (r > t1) return false; if (r > t0) t0 = r; }
        else          { if (r < t0) return false; if (r < t1) t1 = r; }
        return true;
    };
    const float xmax = float(XRes) - 1.0f, ymax = float(YRes) - 1.0f;
    if (!clip(-dx, x0))        return 0;   // x >= 0
    if (!clip( dx, xmax - x0)) return 0;   // x <= xmax
    if (!clip(-dy, y0))        return 0;   // y >= 0
    if (!clip( dy, ymax - y0)) return 0;   // y <= ymax

    const float cx0 = x0 + dx * t0, cy0 = y0 + dy * t0;
    const float cx1 = x0 + dx * t1, cy1 = y0 + dy * t1;
    const float len = std::max(std::fabs(cx1 - cx0), std::fabs(cy1 - cy0));
    int nSeg = int(len / 48.0f) + 1;
    if (nSeg > 48) nSeg = 48;
    float px = cx0, py = cy0;
    float pz = 1.0f / (iz0 + (iz1 - iz0) * t0);
    for (int s = 1; s <= nSeg; ++s) {
        const float ts = t0 + (t1 - t0) * (float(s) / float(nSeg));
        const float qx = x0 + dx * ts, qy = y0 + dy * ts;
        const float qz = 1.0f / (iz0 + (iz1 - iz0) * ts);
        drawLineZ(int(px + 0.5f), int(py + 0.5f), pz,
                  int(qx + 0.5f), int(qy + 0.5f), qz, col);
        px = qx; py = qy; pz = qz;
    }
    return nSeg;
}

// Multiply the whole frame down so the wireframe is the brightest thing on
// screen. Integer scale (k/256) — the modes that use it already replace the
// image's role, so exactness doesn't matter; speed does (one pass over VPage).
void wireDimFrame(float keep) {
    if (keep >= 1.0f) return;
    if (keep < 0.0f) keep = 0.0f;
    const uint32_t k = uint32_t(keep * 256.0f + 0.5f);
    dword* out = reinterpret_cast<dword*>(VPage);
    const size_t n = size_t(XRes) * size_t(YRes);
    if (k == 0) { std::memset(out, 0, n * sizeof(dword)); return; }
    for (size_t i = 0; i < n; ++i) {
        const dword d = out[i];
        const uint32_t r = (((d >> 16) & 0xFFu) * k) >> 8;
        const uint32_t g = (((d >>  8) & 0xFFu) * k) >> 8;
        const uint32_t b = (( d        & 0xFFu) * k) >> 8;
        out[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}

}  // namespace

// Mirror-clone object predicate ("__mirrorClone_*"), defined in EnvBake.cpp.
// Declared locally rather than via a header, the same way Transform.cpp does
// it — EnvBake.h does not export it.
bool EnvBake_IsMirrorCloneObj(const Object* O);

void WireViz_DrawOverlay(Scene* sc) {
    const int mode = FeatureFlags::wire_viz();
    if (mode <= 0) return;
    if (!sc || !View || !VPage || XRes <= 0 || YRes <= 0) return;
    if (g_offscreenViewDepth > 0) return;         // main view only

    if (mode >= 2) wireDimFrame(FeatureFlags::wire_viz_dim());

    const Frustum fr = Frustum_FromMainCamera(sc);
    const Matrix& VM = View->Mat;
    const Vector  P  = View->ISource;
    const float   nearZ = sc->NZP > 0.01f ? sc->NZP : 0.01f;
    // Mode 4 draws BACK-facing first and FRONT-facing second, so an edge shared
    // by one of each ends up green: only edges that are back-facing on BOTH
    // sides stay red, which is exactly the inverted-normal / open-shell signal.
    const int passes = (mode == 4) ? 2 : 1;
    long tris = 0, lines = 0, skipClone = 0, skipProxy = 0;
    static bool once = false;
    const std::chrono::steady_clock::time_point t0 =
        once ? std::chrono::steady_clock::time_point{} : std::chrono::steady_clock::now();

    for (int pass = 0; pass < passes; ++pass) {
      const bool wantBack = (mode == 4 && pass == 0);
      for (Object* Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh) continue;
        TriMesh* T = (TriMesh*)Obj->Data;
        if (!T || T->FIndex == 0 || !T->Faces || !T->Verts) continue;
        if (T->Flags & Tri_Invisible) continue;
        // DRAW WHAT THE MAIN PASS COMPOSITES. Two whole-mesh populations sit
        // in the scene object list as full geometry duplicates, and drawing
        // them put a SECOND copy of the scene on screen — measured at greets
        // t=2095: 4 clone meshes / 15.11 MiB (--mem_census), showing up as a
        // phantom second robot floating through the corridor wall.
        //
        //  • Tri_OffscreenProxy — the flat --greets_shadow_proxy stand-in for
        //    the displaced stone. Transform.cpp skips it outright in the main
        //    view (`!_offscreenPass && (T->Flags & Tri_OffscreenProxy)`), so
        //    it contributes no main-view pixel and must contribute no edge.
        //  • "__mirrorClone_*" — GreetsMirror clones the ENTIRE scene per
        //    mirror as ordinary main-view geometry. The main pass does raster
        //    those faces, but commits a pixel only where the per-lane
        //    bit[pixelMirrorId] mask says the mirror surface was rasterised
        //    there (Mekalele.h), i.e. inside a mirror WINDOW that measures
        //    0.04-3.9% of the screen. A wireframe cannot reproduce a per-pixel
        //    mask, so the choice is a phantom scene over the whole frame or no
        //    reflection wireframe inside a few percent of it. The counts go in
        //    the census line below rather than being dropped silently.
        if (T->Flags & Tri_OffscreenProxy)  { ++skipProxy; continue; }
        if (EnvBake_IsMirrorCloneObj(Obj))  { ++skipClone; continue; }
        // Cheap whole-mesh reject (the same conservative AABB test the cull
        // uses). Meshes without a valid box are kept — never silently dropped.
        if (T->WorldAabbValid) {
            const float bmn[3] = { T->WorldAabbMin.x, T->WorldAabbMin.y, T->WorldAabbMin.z };
            const float bmx[3] = { T->WorldAabbMax.x, T->WorldAabbMax.y, T->WorldAabbMax.z };
            if (Frustum_CullsAabb(fr, bmn, bmx)) continue;
        }
        for (DWord fi = 0; fi < T->FIndex; ++fi) {
            const Face& F = T->Faces[fi];
            if (!F.A || !F.B || !F.C) continue;
            Vertex* const corner[3] = { F.A, F.B, F.C };

            Vector w[3];
            float cog[3] = { 0, 0, 0 };
            for (int k = 0; k < 3; ++k) {
                MatrixXVector(T->RotMat, &corner[k]->Pos, &w[k]);
                w[k].x += T->IPos.x; w[k].y += T->IPos.y; w[k].z += T->IPos.z;
                cog[0] += w[k].x; cog[1] += w[k].y; cog[2] += w[k].z;
            }
            Vector wn; MatrixXVector(T->RotMat, &F.N, &wn);
            const float vx = cog[0]/3.0f - P.x, vy = cog[1]/3.0f - P.y, vz = cog[2]/3.0f - P.z;
            const bool back = (wn.x*vx + wn.y*vy + wn.z*vz > 0.0f);
            if (mode == 4) { if (back != wantBack) continue; }
            else if (back) continue;                       // modes 1-3: front only

            // VIEW space (not screen): drawSegZClipped does the near clip, the
            // projection and the viewport clip, so a wall quad whose corners are
            // all behind the eye still draws its visible span.
            Vector vs[3];
            bool anyFront = false;
            for (int k = 0; k < 3; ++k) {
                Vector d = { w[k].x - P.x, w[k].y - P.y, w[k].z - P.z };
                MatrixXVector(VM, &d, &vs[k]);
                if (vs[k].z > nearZ) anyFront = true;
            }
            if (!anyFront) continue;
            const uint32_t col = (mode == 3) ? wireMatColor(F.Txtr, T)
                               : (mode == 4) ? (back ? kWireBackCol : kWireFrontCol)
                                             : kWirePlainCol;
            ++tris;
            static const int e3[3][2] = { {0,1}, {1,2}, {2,0} };
            for (auto& e : e3)
                lines += drawSegZClipped(vs[e[0]], vs[e[1]], nearZ, col);
        }
      }
    }
    // One-shot census so the cost is a number, not a feeling.
    if (!once) { once = true;
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "[WIRE-VIZ] mode %d: %ld triangles / %ld line "
            "sub-segments in %.1f ms this frame (depth-tested, %s)\n",
            mode, tris, lines, ms,
            mode >= 2 ? "over the dimmed frame" : "over the image");
        if (skipClone || skipProxy)
            std::fprintf(stderr, "[WIRE-VIZ] skipped %ld mirror-clone + %ld "
                "offscreen-proxy mesh(es): duplicate populations the main view "
                "does not composite here (clones are masked to the mirror "
                "window, proxies are offscreen-only)\n", skipClone, skipProxy); }
}

// ── On-screen legends for this file's three vizzes (VizLegend.h) ───────────
// Defined HERE rather than in VizLegend.cpp on purpose: every swatch below is
// produced by the SAME call the overlay makes (wireMatColor's palette,
// rampColor, divergeColor, seamColor) and every number is read from the SAME
// constant or the SAME measurement the overlay used (--wire_viz_dim,
// kDispMatchThresh, the bake's g_dispMax / g_dispErrMax). A legend kept
// anywhere else would be a second source of truth for what the picture means,
// and would eventually disagree with it.
namespace {
// Append one row; `sw` may be null for a text-only row. Returns the new count.
int legendRow(VizLegendRow* rows, int n, int maxRows,
              const uint32_t* sw, int nsw, const char* fmt, ...) {
    if (n >= maxRows) return n;
    VizLegendRow& r = rows[n];
    if (nsw < 0) nsw = 0; else if (nsw > 8) nsw = 8;
    r.nsw = nsw;
    for (int i = 0; i < nsw; ++i) r.sw[i] = sw[i] & 0x00FFFFFFu;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(r.text, sizeof r.text, fmt, ap);
    va_end(ap);
    return n + 1;
}
}  // namespace

int WireViz_Legend(VizLegendRow* rows, int maxRows) {
    const int mode = FeatureFlags::wire_viz();
    if (mode <= 0) return 0;
    const double dimPct = 100.0 * double(FeatureFlags::wire_viz_dim());
    int n = 0;
    switch (mode) {
    case 1:
        n = legendRow(rows, n, maxRows, &kWirePlainCol, 1,
            "every visible triangle edge, DEPTH-TESTED: a far wall's edges stay hidden");
        break;
    case 2:
        n = legendRow(rows, n, maxRows, &kWirePlainCol, 1,
            "triangle edges over the scene dimmed to %.0f%% (--wire_viz_dim)", dimPct);
        break;
    case 3:
        // Put the real chips on screen: "same hue = same material" is then
        // checkable against the frame instead of taken on trust.
        n = legendRow(rows, n, maxRows, kWireMatHue, 8,
            "hue = MATERIAL (12-hue palette, first 8 shown)");
        n = legendRow(rows, n, maxRows, nullptr, 0,
            "brightness step = per-MESH chunk: a chunk seam inside one material shows");
        break;
    case 4:
        n = legendRow(rows, n, maxRows, &kWireFrontCol, 1,
            "FRONT-facing. Drawn last, so an edge shared with a back face reads green.");
        n = legendRow(rows, n, maxRows, &kWireBackCol, 1,
            "back-facing on BOTH sides = INVERTED NORMAL / open shell. That is the read.");
        break;
    default: break;
    }
    return n;
}

int DisplaceViz_Legend(VizLegendRow* rows, int maxRows) {
    const int mode = FeatureFlags::displace_viz();
    if (mode <= 0) return 0;
    int n = 0;
    if (mode == 2) {
        const uint32_t ends[2] = { divergeColor(1.0f), divergeColor(-1.0f) };
        n = legendRow(rows, n, maxRows, nullptr, 0,
            "fill = signed height error (truth - carried), as a fraction of the map's relief");
        n = legendRow(rows, n, maxRows, ends, 2,
            "RED = geometry UNDER-carries (raise greets_displace_cpb) / BLUE = over-carries");
        n = legendRow(rows, n, maxRows, nullptr, 0,
            "NO fill = matched, |err| < %.0f%%; opacity grows with |err|; worst here %.2f",
            (double)(kDispMatchThresh * 100.0f), (double)g_dispErrMax);
    } else if (mode == 3 || mode == 4) {
        const uint32_t dir[3] = { rampColor(0.5f), rampColor(0.75f), rampColor(1.0f) };
        n = legendRow(rows, n, maxRows, dir, 3,
            mode == 3
              ? "fill = WORST corner's angle: displacement vs the BASE wall plane normal"
              : "needle = the applied displacement vector x3; colour = angle vs BASE plane");
        n = legendRow(rows, n, maxRows, nullptr, 0,
            "GREEN 0 = rode the wall normal, YELLOW ~45 = mitre, RED 90 = slid ALONG the wall");
        const uint32_t flushC = 0x002858D0u;
        n = legendRow(rows, n, maxRows, &flushC, 1,
            "BLUE = FLUSH: carries <2%% of max push %.3fu - no height, not a direction error",
            (double)g_dispMax);
    } else {
        const uint32_t ramp[3] = { rampColor(0.0f), rampColor(0.5f), rampColor(1.0f) };
        n = legendRow(rows, n, maxRows, ramp, 3,
            "edge tint = per-vertex displacement, 0 (pinned border / flat) -> the bake's max");
        n = legendRow(rows, n, maxRows, nullptr, 0,
            "max push this bake = %.3f world units (measured; it normalises the ramp)",
            (double)g_dispMax);
    }
    return n;
}

int PomSeamViz_Legend(VizLegendRow* rows, int maxRows) {
    if (FeatureFlags::pom_seam_viz() <= 0) return 0;
    static const char* const kText[4] = {
        "COPLANAR continuation - the surface carries on; a hole here is wrong",
        "ANGLED-IN (concave fold) - the ray should enter the neighbour's shell",
        "ANGLED-OUT (convex fold) - exiting here IS a true silhouette",
        "TRUE boundary - nothing continues; this is where side faces belong",
    };
    int n = 0;
    for (int cls = 0; cls < 4; ++cls) {
        const uint32_t c = seamColor(cls);
        n = legendRow(rows, n, maxRows, &c, 1, "%s", kText[cls]);
    }
    return n;
}

}  // namespace fds
