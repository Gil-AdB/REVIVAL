#include "VertexScratch.h"

#include "Face.h"
#include "TriMesh.h"
#include "Vertex.h"

#include <cstring>

#if FDS_VIS_CENSUS
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#endif

namespace fds {

PerTriMeshClone& VertexScratch::cloneOf(TriMesh* T) {
    // Fast path: same T as the previous call (hits during Transform_
    // Objects' tight per-mesh loop). One pointer compare vs ~30-50
    // cycles for the unordered_map hash + bucket walk.
    if (T == lastT) { lastFresh = false; return *lastClone; }

    auto& c = clones[T];
    lastT     = T;
    lastClone = &c;
    // SIZE-DRIFT INVALIDATION. Transform_Objects walks the clone to the LIVE
    // bound — `VEnd = tVerts + T->VIndex` (Transform.cpp), and the face loop
    // likewise to T->FIndex — while the storage is whatever the FIRST use
    // sized it to. A mesh that grows after being cloned therefore reads AND
    // WRITES past the clone's allocation. Nothing in a shipping run does that
    // (measured: 0 size drifts in 630 622 clone reuses over a 13-pose greets
    // sweep with the shatter live, --clone_stale_census), but two editor paths
    // replace TriMesh::Verts and change VIndex on a LIVE mesh —
    // MeshOps_ResmoothSurface grows it to FIndex*3 (MeshOps.cpp:249) and
    // DisplaceRebuild_Apply re-runs the whole subdivision bake — so the
    // condition is reachable the moment either is driven from the material
    // editor mid-session. Two integer compares on the map-MISS path (the hit
    // path above is untouched) turn an unbounded heap overflow into a rebuild.
    if (c.initialized &&
        c.verts.size() == size_t(T->VIndex) &&
        c.faces.size() == size_t(T->FIndex)) { lastFresh = false; return c; }
    lastFresh = true;
    c.verts.assign(T->Verts, T->Verts + T->VIndex);
    c.faces.assign(T->Faces, T->Faces + T->FIndex);
    // Remap each cloned Face's A/B/C to point into our cloned Verts.
    // Original A/B/C are pointers into T->Verts; the offset from
    // T->Verts is the same in the clone (verts is a contiguous copy
    // sized VIndex), so address arithmetic gives the right slot.
    // A_idx/B_idx/C_idx (SoA Phase 3 indices) survive the assign()
    // unchanged — same indices index correctly into clone.verts AND
    // into c.frame's SoA arrays.
    for (auto& f : c.faces) {
        if (f.A) f.A = &c.verts[f.A - T->Verts];
        if (f.B) f.B = &c.verts[f.B - T->Verts];
        if (f.C) f.C = &c.verts[f.C - T->Verts];
    }
    // SoA Phase 4: size the per-clone SoA arrays to match clone.verts.
    // Reused across frames; ensureSized is a no-op when capacity
    // already matches.
    c.frame.ensureSized(int(T->VIndex));
    c.initialized = true;
    return c;
}

// ── clone INPUT refresh (--clone_refresh_inputs) ─────────────────────────
//
// The clone's Vertex bytes split cleanly at offset 52 (Vertex.h's layout
// contract, static_assert'd there): [0, 52) = PX, PY, Flags, TPos_AOS, RZ,
// TN, TTangent — the projection OUTPUTS Transform_Objects rewrites every
// pass; [52, 140) = Pos, N, Tangent, BGRA, UZ/VZ, EUZ/EVZ, U/V, EU/EV, i,
// OrigBary, ShellH — the INPUTS that were snapshotted at first use and never
// looked at again. Refreshing exactly the second half is what "invalidate the
// clone" means here, and it costs 88 of the 140 bytes rather than a rebuild
// (no reallocation, no Face pointer remap, frame untouched).
static constexpr size_t kVertexInputOfs = offsetof(Vertex, Pos);
static_assert(kVertexInputOfs == 52, "input block starts where the outputs end");

void CloneRefreshInputs(TriMesh *T, PerTriMeshClone &c) {
    if (!T || !T->Verts) return;
    const DWord n = (T->VIndex < DWord(c.verts.size())) ? T->VIndex
                                                        : DWord(c.verts.size());
    for (DWord i = 0; i < n; ++i) {
        std::memcpy(reinterpret_cast<char *>(&c.verts[i]) + kVertexInputOfs,
                    reinterpret_cast<const char *>(&T->Verts[i]) + kVertexInputOfs,
                    sizeof(Vertex) - kVertexInputOfs);
    }
}

void CloneRefreshFaces(TriMesh *T, PerTriMeshClone &c) {
    if (!T || !T->Faces || !T->Verts) return;
    const DWord n = (T->FIndex < DWord(c.faces.size())) ? T->FIndex
                                                        : DWord(c.faces.size());
    for (DWord i = 0; i < n; ++i) {
        Face &d = c.faces[i];
        d = T->Faces[i];
        if (d.A) d.A = &c.verts[d.A - T->Verts];
        if (d.B) d.B = &c.verts[d.B - T->Verts];
        if (d.C) d.C = &c.verts[d.C - T->Verts];
    }
}

#if FDS_VIS_CENSUS
namespace {

struct StaleRow {
    long long checks       = 0;   // reused-clone checks for this mesh
    long long checksStale  = 0;   // ... of which at least one field diverged
    long long vertsChecked = 0;
    long long vPos = 0, vN = 0, vTan = 0, vTail = 0;
    // tail split — BGRA is the per-vertex LIT COLOUR (rewritten every frame by
    // the forward lighting pass), the rest are authored/derived UV + bary data.
    long long vBGRA = 0, vUZVZ = 0, vEUZ = 0, vUV = 0, vEUEV = 0, vMisc = 0;
    long long facesChecked = 0, facesDiff = 0;
    long long fGeo = 0, fUV = 0, fEnvUV = 0, fLw = 0, fPtr = 0, fId = 0;
    long long sizeDrift = 0;     // clone array size != live VIndex/FIndex
    long long vNaN = 0;          // verts whose LIVE Tangent/N is NaN — these
                                 // are why a float-compare census lies
    double    maxPosDelta  = 0.0;
};

std::mutex                       &staleMtx() { static std::mutex m; return m; }
std::map<std::string, StaleRow>  &staleRows() {
    static std::map<std::string, StaleRow> m; return m;
}

// BYTE compare, not value compare. A NaN component (greets' displaced stone
// carries a few degenerate tangents) is != itself, so a float comparison
// reports a frozen-but-identical clone as diverged forever. Staleness is a
// question about BYTES.
inline bool vecDiff(const Vector &a, const Vector &b) {
    return std::memcmp(&a, &b, 3 * sizeof(float)) != 0;
}
inline bool vecNaN(const Vector &a) {
    return !(a.x == a.x) || !(a.y == a.y) || !(a.z == a.z);
}
inline double vecDist(const Vector &a, const Vector &b) {
    const double dx = double(a.x) - b.x, dy = double(a.y) - b.y,
                 dz = double(a.z) - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
enum { FD_GEO = 1, FD_UV = 2, FD_ENVUV = 4, FD_LW = 8, FD_PTR = 16, FD_ID = 32 };
inline unsigned faceInputsDiff(const Face &c, const Face &l) {
    // Everything except the per-pass fields Transform_Objects / the clipper
    // rewrite on the clone (A/B/C — remapped by construction; Flags; SortZ;
    // ParentTri; LastMip).
    unsigned m = 0;
    if (vecDiff(c.N, l.N) || c.NormProd != l.NormProd)                  m |= FD_GEO;
    if (c.U1 != l.U1 || c.V1 != l.V1 || c.U2 != l.U2 || c.V2 != l.V2 ||
        c.U3 != l.U3 || c.V3 != l.V3)                                   m |= FD_UV;
    // EU*/EV* are the ENV-MAP coords the transform's own face loop stamps per
    // pass — an OUTPUT that happens to live in Face. Counted separately so a
    // recomputed field is not read as a stale input.
    if (c.EU1 != l.EU1 || c.EV1 != l.EV1 || c.EU2 != l.EU2 ||
        c.EV2 != l.EV2 || c.EU3 != l.EU3 || c.EV3 != l.EV3)             m |= FD_ENVUV;
    if (c.LwDU != l.LwDU || c.LwDV != l.LwDV)                           m |= FD_LW;
    if (c.Filler != l.Filler || c.Txtr != l.Txtr ||
        c.ReflectionTexture != l.ReflectionTexture)                     m |= FD_PTR;
    if (c.MeshFaceIdx != l.MeshFaceIdx || c.ShadowMatID != l.ShadowMatID ||
        c.PomShellGroup != l.PomShellGroup ||
        c.PomPrismSide != l.PomPrismSide ||
        c.mirrorMaskTag != l.mirrorMaskTag)                             m |= FD_ID;
    return m;
}

} // namespace

void CloneStale_Check(TriMesh *T, const char *name, const PerTriMeshClone &c) {
    if (!T || !T->Verts) return;
    StaleRow r;
    const DWord vn = (T->VIndex < DWord(c.verts.size())) ? T->VIndex
                                                         : DWord(c.verts.size());
    for (DWord i = 0; i < vn; ++i) {
        const Vertex &cv = c.verts[i], &lv = T->Verts[i];
        ++r.vertsChecked;
        if (vecDiff(cv.Pos, lv.Pos)) {
            ++r.vPos;
            const double d = vecDist(cv.Pos, lv.Pos);
            if (d > r.maxPosDelta) r.maxPosDelta = d;
        }
        if (vecDiff(cv.N, lv.N))             ++r.vN;
        if (vecDiff(cv.Tangent, lv.Tangent)) ++r.vTan;
        if (vecNaN(lv.Tangent) || vecNaN(lv.N)) ++r.vNaN;
        // The tail: BGRA (the disco ball's per-tick shade lives here),
        // UZ/VZ, EUZ/EVZ, U/V, EU/EV, i, OrigBary, ShellH.
        if (std::memcmp(reinterpret_cast<const char *>(&cv) + offsetof(Vertex, BGRA),
                        reinterpret_cast<const char *>(&lv) + offsetof(Vertex, BGRA),
                        sizeof(Vertex) - offsetof(Vertex, BGRA)) != 0) {
            ++r.vTail;
            if (cv.BGRA != lv.BGRA)                     ++r.vBGRA;
            if (cv.UZ  != lv.UZ  || cv.VZ  != lv.VZ)    ++r.vUZVZ;
            if (cv.EUZ != lv.EUZ || cv.EVZ != lv.EVZ)   ++r.vEUZ;
            if (cv.U   != lv.U   || cv.V   != lv.V)     ++r.vUV;
            if (cv.EU  != lv.EU  || cv.EV  != lv.EV)    ++r.vEUEV;
            if (cv.i != lv.i || cv.OrigBaryB != lv.OrigBaryB ||
                cv.OrigBaryC != lv.OrigBaryC || cv.ShellH != lv.ShellH)
                ++r.vMisc;
        }
    }
    if (T->Faces) {
        const DWord fn = (T->FIndex < DWord(c.faces.size())) ? T->FIndex
                                                             : DWord(c.faces.size());
        for (DWord i = 0; i < fn; ++i) {
            ++r.facesChecked;
            const unsigned m = faceInputsDiff(c.faces[i], T->Faces[i]);
            if (m) {
                ++r.facesDiff;
                if (m & FD_GEO)   ++r.fGeo;
                if (m & FD_UV)    ++r.fUV;
                if (m & FD_ENVUV) ++r.fEnvUV;
                if (m & FD_LW)    ++r.fLw;
                if (m & FD_PTR)   ++r.fPtr;
                if (m & FD_ID)    ++r.fId;
            }
        }
    }
    // Also record a structural mismatch: the clone was sized to the mesh's
    // VIndex/FIndex at first use, so a mesh that GREW or SHRANK since then is
    // stale in a way no field compare can see.
    const bool sizeDrift = (DWord(c.verts.size()) != T->VIndex) ||
                           (DWord(c.faces.size()) != T->FIndex);
    r.sizeDrift   = sizeDrift ? 1 : 0;
    r.checks      = 1;
    r.checksStale = (r.vPos || r.vN || r.vTan || r.vTail || r.facesDiff ||
                     sizeDrift) ? 1 : 0;

    const std::string key = name ? name : "?";
    std::lock_guard<std::mutex> lk(staleMtx());
    StaleRow &acc = staleRows()[key];
    acc.checks       += r.checks;
    acc.checksStale  += r.checksStale;
    acc.vertsChecked += r.vertsChecked;
    acc.vPos  += r.vPos;  acc.vN    += r.vN;
    acc.vTan  += r.vTan;  acc.vTail += r.vTail;
    acc.vNaN  += r.vNaN;
    acc.vBGRA += r.vBGRA;  acc.vUZVZ += r.vUZVZ;  acc.vEUZ  += r.vEUZ;
    acc.vUV   += r.vUV;    acc.vEUEV += r.vEUEV;  acc.vMisc += r.vMisc;
    acc.fGeo  += r.fGeo;   acc.fUV   += r.fUV;    acc.fEnvUV += r.fEnvUV;
    acc.fLw   += r.fLw;    acc.fPtr  += r.fPtr;   acc.fId    += r.fId;
    acc.sizeDrift += r.sizeDrift;
    acc.facesChecked += r.facesChecked;
    acc.facesDiff    += r.facesDiff;
    if (r.maxPosDelta > acc.maxPosDelta) acc.maxPosDelta = r.maxPosDelta;
}

void CloneStale_Dump() {
    std::lock_guard<std::mutex> lk(staleMtx());
    long long tChecks = 0, tStale = 0, tVerts = 0, tPos = 0, tN = 0, tTan = 0,
              tTail = 0, tFaces = 0, tFDiff = 0, tNaN = 0,
              tBGRA = 0, tUZVZ = 0, tEUZ = 0, tUV = 0, tEUEV = 0, tMisc = 0,
              fGeo = 0, fUV = 0, fEnvUV = 0, fLw = 0, fPtr = 0, fId = 0,
              tDrift = 0;
    int meshes = 0, meshesStale = 0;
    for (const auto &kv : staleRows()) {
        const StaleRow &r = kv.second;
        ++meshes;
        tChecks += r.checks;   tStale  += r.checksStale;
        tVerts  += r.vertsChecked;
        tPos    += r.vPos;     tN      += r.vN;
        tTan    += r.vTan;     tTail   += r.vTail;
        tNaN    += r.vNaN;
        tBGRA += r.vBGRA; tUZVZ += r.vUZVZ; tEUZ += r.vEUZ;
        tUV   += r.vUV;   tEUEV += r.vEUEV; tMisc += r.vMisc;
        fGeo  += r.fGeo;  fUV   += r.fUV;   fEnvUV += r.fEnvUV;
        fLw   += r.fLw;   fPtr  += r.fPtr;  fId    += r.fId;
        tDrift += r.sizeDrift;
        tFaces  += r.facesChecked; tFDiff += r.facesDiff;
        if (r.checksStale) {
            ++meshesStale;
            std::fprintf(stderr,
                "[CLONE-STALE] '%s' %lld/%lld reuses stale | of %lld vert-compares "
                "Pos=%lld (max |d| %.6f) N=%lld Tan=%lld tail=%lld (NaN-live %lld) | faces %lld/%lld\n",
                kv.first.c_str(), r.checksStale, r.checks, r.vertsChecked,
                r.vPos, r.maxPosDelta, r.vN, r.vTan, r.vTail, r.vNaN,
                r.facesDiff, r.facesChecked);
        }
    }
    std::fprintf(stderr,
        "[CLONE-STALE] TOTAL meshes=%d stale=%d | reuses=%lld stale=%lld | "
        "vert-compares=%lld Pos=%lld N=%lld Tan=%lld tail=%lld (NaN-live %lld) | "
        "face-compares=%lld diff=%lld\n"
        "[CLONE-STALE] TOTAL tail split: BGRA=%lld UZ/VZ=%lld EUZ/EVZ=%lld "
        "U/V=%lld EU/EV=%lld i+bary+shellH=%lld\n"
        "[CLONE-STALE] TOTAL face split: N/NormProd=%lld U1..V3=%lld "
        "EU1..EV3=%lld LwDU/DV=%lld Filler/Txtr/Refl=%lld ids=%lld\n"
        "[CLONE-STALE] TOTAL size-drift (clone array vs live VIndex/FIndex) = %lld\n",
        meshes, meshesStale, tChecks, tStale, tVerts, tPos, tN, tTan, tTail,
        tNaN, tFaces, tFDiff,
        tBGRA, tUZVZ, tEUZ, tUV, tEUEV, tMisc,
        fGeo, fUV, fEnvUV, fLw, fPtr, fId, tDrift);
    staleRows().clear();
}
#endif  // FDS_VIS_CENSUS

} // namespace fds
