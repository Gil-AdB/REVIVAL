// Static shadow lightmap bake — Option A: per-face N×N atlas of pre-baked
// shadow factors, one byte per (face, texel, cube-omni) tuple. See
// docs/STATIC_SHADOW_LIGHTMAPS.md.

#include "RENDER/LightmapBake.h"

// Match DeferredLighting.cpp's include order: legacy `.H` headers first
// (they push/pop pack(1) in subtly-unbalanced ways across the chain),
// then StaticShadowLightmap.h so its std::vector members land at the
// same offsets as everywhere else. Mismatched include order produced a
// silent ODR violation: this TU saw a 84-byte struct, DeferredLighting
// saw 88 bytes, the data-vector header was at different offsets, and
// the 14 MB lightmap bake read as empty at runtime.
#include "Base/FDS_DECS.H"
#include "Base/FDS_VARS.H"  // MatrixXVector template
#include "Base/FeatureFlags.h"
#include "Base/Scene.h"
#include "Base/StaticShadowLightmap.h"
#include "Base/TriMesh.h"
#include "Base/Object.h"
#include "Base/Omni.h"
#include "Base/Vector.h"
#include "Base/Matrix.h"
#include "FILLERS/ShadowMap.h"
#include "FILLERS/Mekalele.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <unistd.h>  // isatty for progress-bar carriage-return gating
#include <semaphore>
#include <Threads.h>  // ThreadPool — fan the per-face bake across cores
#include <vector>

namespace fds {
namespace {

// ─── World-space static-cube sampler ─────────────────────────────────────
// Bake-time parallel to CubeShadow_Sample (FDS/FILLERS/ShadowMap.h), but
// takes a world-space sample point instead of view-space. Reads only the
// static-occluder buffer (sm.depth or sm.polyId depending on mode), since
// the lightmap caches the static-scene contribution. Returns shadow factor
// in [0, 255] where 255 is fully lit and 0 is fully shadowed.
//
// `surfaceMatId`: -1 = legacy Depth mode (biased z-compare against sm.depth).
//                 0..255 = PolyId mode (identity test against sm.polyId,
//                          same convention as runtime CubeShadow_Sample).
//                          Bias arguments ignored in PolyId mode.
uint8_t SampleStaticCubeAtWorld(const CubeShadowRef &cr,
                                 const Vector &worldPos,
                                 int constBias, int slopeBiasInt,
                                 int surfaceMatId = -1)
{
    const float dwx = worldPos.x - cr.lightISource.x;
    const float dwy = worldPos.y - cr.lightISource.y;
    const float dwz = worldPos.z - cr.lightISource.z;
    const int face = CubeShadow_SelectFace(dwx, dwy, dwz);
    const ShadowMap &sm = g_shadowMaps[cr.faceIdx[face]];

    // Transform sample's *world-space* delta into the cube face's view
    // space. CubeShadowMaps_Rebuild builds `sm.lightViewMat` as a pure
    // world→view rotation centered on lightISource, so the offset cancels.
    const float lx = sm.lightViewMat[0][0] * dwx + sm.lightViewMat[0][1] * dwy + sm.lightViewMat[0][2] * dwz;
    const float ly = sm.lightViewMat[1][0] * dwx + sm.lightViewMat[1][1] * dwy + sm.lightViewMat[1][2] * dwz;
    const float lz = sm.lightViewMat[2][0] * dwx + sm.lightViewMat[2][1] * dwy + sm.lightViewMat[2][2] * dwz;

    if (lz <= 0.05f) return 255;
    constexpr float kFaceFrustumRatio = 1.5f;
    if (lx >  kFaceFrustumRatio * lz || lx < -kFaceFrustumRatio * lz) return 255;
    if (ly >  kFaceFrustumRatio * lz || ly < -kFaceFrustumRatio * lz) return 255;

    const float invLZ = 1.0f / lz;
    const float smX = sm.cntrX + sm.perspX * lx * invLZ;
    const float smY = sm.cntrY - sm.perspY * ly * invLZ;
    const int iX = int(smX);
    const int iY = int(smY);
    if (iX < 0 || iX + 1 >= sm.xres || iY < 0 || iY + 1 >= sm.yres) return 255;

    const size_t rowOfs = size_t(iY) * size_t(sm.xres);
    const float fx = smX - float(iX);
    const float fy = smY - float(iY);
    const float w00 = (1.0f - fx) * (1.0f - fy);
    const float w10 =         fx  * (1.0f - fy);
    const float w01 = (1.0f - fx) *         fy;
    const float w11 =         fx  *         fy;

    float occ = 0.0f;
    if (surfaceMatId >= 0) {
        // PolyId mode: identity test against sm.polyId. Matches the
        // runtime CubeShadow_Sample PolyId branch (ShadowMap.h). 0
        // sentinel = "no occluder wrote here." Receiver's matID+1 means
        // "this texel was written by my own (or a same-matID) face" =
        // not occluded. Anything else nonzero = occluder of a different
        // material = occluded. No bias needed.
        const uint16_t *p0 = sm.polyId.data() + rowOfs;
        const uint16_t *p1 = p0 + sm.xres;
        // 16-bit ShadowMatID direct compare (no +1 offset added here;
        // bake-time caller resolves Material::ShadowMatID upstream).
        const uint16_t receiverId = uint16_t(surfaceMatId);
        auto isOccluded = [&](uint16_t v) -> bool {
            return v != 0 && v != receiverId;
        };
        if (isOccluded(p0[iX  ])) occ += w00;
        if (isOccluded(p0[iX+1])) occ += w10;
        if (isOccluded(p1[iX  ])) occ += w01;
        if (isOccluded(p1[iX+1])) occ += w11;
    } else {
        // Depth mode: biased z-compare. slopeBiasInt computed by caller
        // from this texel's (N · L) so grazing-angle faces don't acne.
        const uint16_t *z0 = sm.depth.data() + rowOfs;
        const uint16_t *z1 = z0 + sm.xres;
        int pixZenc = 0xFF80 - int(lz * sm.zScale);
        if (pixZenc < 0) pixZenc = 0;
        if (pixZenc > 0xFFFF) pixZenc = 0xFFFF;
        const int biased = pixZenc + constBias + slopeBiasInt;
        if (biased < int(z0[iX  ])) occ += w00;
        if (biased < int(z0[iX+1])) occ += w10;
        if (biased < int(z1[iX  ])) occ += w01;
        if (biased < int(z1[iX+1])) occ += w11;
    }

    const float lit = 1.0f - occ;
    int factor = int(lit * 255.0f + 0.5f);
    if (factor < 0)   factor = 0;
    if (factor > 255) factor = 255;
    return uint8_t(factor);
}

// Mirrors Transform.cpp isDynamicForBake — walks the parent Object chain
// and checks each ancestor's Pos/Rotate spline min/max extent. A mesh
// whose own splines are flat but whose parent is animated is *dynamic*
// in world space, so we must NOT bake a static shadow for it. The
// thresholds (0.1 world units for Pos, 0.01 unit-quat delta for Rotate)
// match Transform.cpp exactly so the two passes stay in lockstep.
//
// reasonOut: if non-null, populated with a short string explaining the
// verdict (for the diagnostic dump).
bool isMeshDynamic(Object *obj, char *reasonOut = nullptr, size_t reasonCap = 0)
{
    auto setReason = [&](const char *s, Object *culprit) {
        if (!reasonOut || reasonCap == 0) return;
        std::snprintf(reasonOut, reasonCap, "%s @ '%s'",
                      s, (culprit && culprit->Name) ? culprit->Name : "?");
    };
    constexpr float kPosExtentEps = 0.1f;
    constexpr float kRotExtentEps = 0.01f;
    for (Object *o = obj; o; o = o->Parent) {
        if (o->Type != Obj_TriMesh) continue;
        TriMesh *tm = (TriMesh *)o->Data;
        if (!tm) continue;
        if (tm->Pos.NumKeys > 1 && tm->Pos.Keys) {
            const auto& k0 = tm->Pos.Keys[0].Pos;
            float xmin=k0.x, xmax=k0.x, ymin=k0.y, ymax=k0.y, zmin=k0.z, zmax=k0.z;
            for (DWord i = 1; i < tm->Pos.NumKeys; ++i) {
                const auto& k = tm->Pos.Keys[i].Pos;
                if (k.x < xmin) xmin=k.x; if (k.x > xmax) xmax=k.x;
                if (k.y < ymin) ymin=k.y; if (k.y > ymax) ymax=k.y;
                if (k.z < zmin) zmin=k.z; if (k.z > zmax) zmax=k.z;
            }
            if ((xmax-xmin) > kPosExtentEps ||
                (ymax-ymin) > kPosExtentEps ||
                (zmax-zmin) > kPosExtentEps) {
                setReason("Pos extent", o);
                return true;
            }
        }
        if (tm->Rotate.NumKeys > 1 && tm->Rotate.Keys) {
            const auto& q0 = tm->Rotate.Keys[0].Pos;
            float xmin=q0.x, xmax=q0.x, ymin=q0.y, ymax=q0.y;
            float zmin=q0.z, zmax=q0.z, wmin=q0.W, wmax=q0.W;
            for (DWord i = 1; i < tm->Rotate.NumKeys; ++i) {
                const auto& q = tm->Rotate.Keys[i].Pos;
                if (q.x < xmin) xmin=q.x; if (q.x > xmax) xmax=q.x;
                if (q.y < ymin) ymin=q.y; if (q.y > ymax) ymax=q.y;
                if (q.z < zmin) zmin=q.z; if (q.z > zmax) zmax=q.z;
                if (q.W < wmin) wmin=q.W; if (q.W > wmax) wmax=q.W;
            }
            if ((xmax-xmin) > kRotExtentEps ||
                (ymax-ymin) > kRotExtentEps ||
                (zmax-zmin) > kRotExtentEps ||
                (wmax-wmin) > kRotExtentEps) {
                setReason("Rotate extent", o);
                return true;
            }
        }
    }
    return false;
}

}  // namespace

void LightmapStampOrigBary(Scene *Sc)
{
    if (!fds::FeatureFlags::shadow_lightmap()) return;
    if (!Sc) return;
    // Walk every static mesh, stamp (A, B, C) of each face with its bary
    // coordinate on its own (A, B, C): A is (0, 0), B is (1, 0), C is
    // (0, 1). Same isMeshDynamic predicate as LightmapBake_Static so the
    // stamp set matches the bake set 1:1.
    for (Object *Obj = Sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh) continue;
        TriMesh *T = (TriMesh *)Obj->Data;
        if (!T || T->FIndex == 0) continue;
        if (isMeshDynamic(Obj, nullptr, 0)) continue;
        for (DWord fi = 0; fi < T->FIndex; ++fi) {
            const Face &F = T->Faces[fi];
            if (F.A) { F.A->OrigBaryB = 0.0f; F.A->OrigBaryC = 0.0f; }
            if (F.B) { F.B->OrigBaryB = 1.0f; F.B->OrigBaryC = 0.0f; }
            if (F.C) { F.C->OrigBaryB = 0.0f; F.C->OrigBaryC = 1.0f; }
        }
    }
}

void LightmapBake_Static(Scene *Sc, bool forceEnable)
{
    if (!forceEnable && !fds::FeatureFlags::shadow_lightmap()) return;
    if (!Sc) return;
    if (g_cubeShadowRefs.empty()) {
        std::fprintf(stderr, "[LM] no cube-shadow casters; skipping bake\n");
        return;
    }

    const int lmRes = std::max(2, fds::FeatureFlags::shadow_lightmap_res());
    const int constBias    = fds::FeatureFlags::shadow_bias();
    const int slopeBiasInt = fds::FeatureFlags::shadow_slope_bias();  // applied as constant; no per-texel slope yet

    const int numCubeOmnis = int(g_cubeShadowRefs.size());

    const auto t0 = std::chrono::steady_clock::now();
    size_t meshCount = 0;
    // Atomic: the per-face bake loop fans across the thread pool (each face is
    // independent — disjoint lm writes), so these stats accumulate concurrently.
    std::atomic<size_t> faceCount{0};
    std::atomic<size_t> texelsBaked{0}, texelsCovered{0};
    size_t skippedDynamic = 0, considered = 0;

    // Pre-walk: count meshes we will actually bake so the progress bar
    // has a denominator. Same predicate the main loop uses; no allocation.
    size_t plannedMeshes = 0;
    for (Object *Obj = Sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh) continue;
        TriMesh *T = (TriMesh *)Obj->Data;
        if (!T || T->FIndex == 0) continue;
        if (isMeshDynamic(Obj, nullptr, 0)) continue;
        ++plannedMeshes;
    }
    auto lastTick = t0;
    const bool toTty = ::isatty(::fileno(stderr));
    auto reportProgress = [&](size_t done, const char *meshName, bool force) {
        const auto now = std::chrono::steady_clock::now();
        const double sinceLast = std::chrono::duration<double, std::milli>(now - lastTick).count();
        if (!force && sinceLast < 200.0) return;
        lastTick = now;
        const double elapsed = std::chrono::duration<double>(now - t0).count();
        const double frac = plannedMeshes ? double(done) / double(plannedMeshes) : 1.0;
        // 30-cell bar
        char bar[33];
        const int filled = int(frac * 30.0 + 0.5);
        for (int i = 0; i < 30; ++i) bar[i] = (i < filled) ? '#' : '.';
        bar[30] = '\0';
        std::fprintf(stderr, "%c[LM-BAKE] [%s] %zu/%zu mesh (%.0f%%)  %.1fs  %-24.24s%s",
                     toTty ? '\r' : ' ',
                     bar, done, plannedMeshes, frac * 100.0, elapsed,
                     meshName ? meshName : "",
                     toTty ? "" : "\n");
        std::fflush(stderr);
    };
    reportProgress(0, "", true);

    // Reset the scene's lightmap mesh table. Index 0 reserved as sentinel
    // (nullptr); kept-mesh indices start at 1.
    if (!Sc->staticLMTable) Sc->staticLMTable = new std::vector<TriMesh*>();
    Sc->staticLMTable->clear();
    Sc->staticLMTable->push_back(nullptr);
    // Clear every mesh's staticLMMeshId so faces from dynamic meshes (or
    // meshes left over from a prior bake) end up with the sentinel 0.
    for (TriMesh *T = Sc->TriMeshHead; T; T = T->Next) {
        T->staticLMMeshId = 0;
    }

    // Iterate Objects (not TriMeshHead directly) so we can walk the parent
    // chain — a mesh whose own splines are flat but whose parent moves is
    // still dynamic in world space and must NOT be baked.
    for (Object *Obj = Sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh) continue;
        TriMesh *T = (TriMesh *)Obj->Data;
        if (!T || T->FIndex == 0) continue;
        ++considered;

        char reason[64] = {0};
        const bool dyn = isMeshDynamic(Obj, reason, sizeof(reason));
        const char *name = Obj->Name ? Obj->Name : "?";
        if (dyn) {
            ++skippedDynamic;
            if (skippedDynamic <= 32) {
                std::fprintf(stderr, "[LM-SKIP] '%s' (%u faces) — %s\n",
                             name, unsigned(T->FIndex), reason);
            }
            continue;
        }

        // Allocate / reset lightmap on this mesh.
        if (T->staticShadowLM) { T->staticShadowLM->clear(); }
        else                   { T->staticShadowLM = new StaticShadowLightmap(); }
        StaticShadowLightmap &lm = *T->staticShadowLM;
        lm.allocate(int(T->FIndex), numCubeOmnis, lmRes);
        for (int oi = 0; oi < numCubeOmnis; ++oi) lm.omniSceneIdx[oi] = oi;

        // Assign this mesh's lightmap-table index (1-based; 0 = none).
        Sc->staticLMTable->push_back(T);
        if (Sc->staticLMTable->size() > 0xFFFF) {
            std::fprintf(stderr,
                "[LM] more than 65535 static meshes — table overflow, "
                "skipping further bakes\n");
            return;
        }
        T->staticLMMeshId = uint16_t(Sc->staticLMTable->size() - 1);

        // Stamp the face's own index so Mekalele can recover it from
        // FList (where Face pointers no longer point into T->Faces).
        for (DWord fi = 0; fi < T->FIndex && fi <= 0xFFFF; ++fi) {
            T->Faces[fi].MeshFaceIdx = uint16_t(fi);
        }

        ++meshCount;
        const Vector &IP = T->IPos;
        auto toWorld = [&](const Vector &objPos, Vector &out) {
            MatrixXVector(T->RotMat, const_cast<Vector*>(&objPos), &out);
            out.x += IP.x; out.y += IP.y; out.z += IP.z;
        };

        const bool planar = fds::FeatureFlags::shadow_lightmap_planar();
        if (planar) {
            lm.planarBases.assign(size_t(T->FIndex), FacePlanarBasis{});
        }

        // Per-face bake (independent: each face writes its own lm texels +
        // planar basis; SampleStaticCubeAtWorld is read-only). Wrapped so the
        // loop can fan across the pool below — this is the bulk of the bake.
        auto bakeOneFace = [&](DWord fi) {
            const Face &F = T->Faces[fi];
            if (!F.A || !F.B || !F.C) return;
            size_t fBaked = 0, fCovered = 0;

            // Vertex world positions for this face.
            Vector wA, wB, wC;
            toWorld(F.A->Pos, wA);
            toWorld(F.B->Pos, wB);
            toWorld(F.C->Pos, wC);
            // World-space face normal (RotMat is orthonormal so direction
            // is preserved without inverse-transpose).
            Vector wN;
            {
                Vector localN = F.N;
                MatrixXVector(T->RotMat, &localN, &wN);
            }

            // Receiver's resolved 16-bit ShadowMatID for the PolyId bake.
            // Direct value (no +1 offset) that matches what ShadowBarry
            // stamped. Priority (high to low) mirrors Mekalele's per-face
            // resolution in MekaleleImpl:
            //   1. F.ShadowMatID  (per-face — greets wall split)
            //   2. F.Txtr->ShadowMatID (per-material — greets hull merge)
            //   3. uint16_t(Txtr->ID + 1) (legacy matID+1 fallback)
            // -1 = depth mode (skip PolyId entirely).
            int bakeMatId = -1;
            if (g_shadowMode.load(std::memory_order_relaxed) == ShadowMode::PolyId
                && F.Txtr) {
                if (F.ShadowMatID != 0) {
                    bakeMatId = int(F.ShadowMatID);
                } else if (F.Txtr->ShadowMatID != 0) {
                    bakeMatId = int(F.Txtr->ShadowMatID);
                } else {
                    bakeMatId = int(F.Txtr->ID + 1);
                }
            }

            // Planar mode: pick dominant cardinal axis from |wN|, compute
            // the face's projected bbox on the orthogonal plane, store
            // basis for runtime sampling. Skip degenerate faces (zero ext
            // along either axis would zero-divide the runtime mapper).
            uint8_t domAxis = 1;
            float uMin = 0, uExt = 0, vMin = 0, vExt = 0;
            if (planar) {
                const float aX = std::fabs(wN.x);
                const float aY = std::fabs(wN.y);
                const float aZ = std::fabs(wN.z);
                if      (aY >= aX && aY >= aZ) domAxis = 1;
                else if (aX >= aZ)             domAxis = 0;
                else                            domAxis = 2;
                auto proj = [&](const Vector &p, float &u, float &v) {
                    if      (domAxis == 0) { u = p.y; v = p.z; }
                    else if (domAxis == 1) { u = p.x; v = p.z; }
                    else                    { u = p.x; v = p.y; }
                };
                float uA, vA, uB, vB, uC, vC;
                proj(wA, uA, vA);
                proj(wB, uB, vB);
                proj(wC, uC, vC);
                uMin = std::min({uA, uB, uC});
                vMin = std::min({vA, vB, vC});
                uExt = std::max({uA, uB, uC}) - uMin;
                vExt = std::max({vA, vB, vC}) - vMin;
                FacePlanarBasis &pb = lm.planarBases[size_t(fi)];
                pb.dominantAxis = domAxis;
                pb.uMin = uMin; pb.uExt = uExt;
                pb.vMin = vMin; pb.vExt = vExt;
            }

            // Per cube omni: face-level culls then per-texel bake.
            for (int oi = 0; oi < numCubeOmnis; ++oi) {
                const CubeShadowRef &cr = g_cubeShadowRefs[oi];
                Omni *O = cr.omni;
                if (!O) continue;
                // Skip moving omnis (Omni_CastsShadow without Omni_StaticShadow):
                // their cube is re-rendered every frame from current IPos by
                // DynamicOmnisPerFrame, so a static lightmap baked at t=0 from
                // a position the omni is no longer at would just be wrong.
                if (!(O->Flags & Omni_StaticShadow)) continue;
                const float range = O->IRange > 0.0f ? O->IRange : 1.0e30f;
                const Vector &OP = cr.lightISource;

                // Face range cull: if all 3 corners are beyond IRange the
                // omni can't light any texel on this face.
                const float dxA = wA.x - OP.x, dyA = wA.y - OP.y, dzA = wA.z - OP.z;
                const float dxB = wB.x - OP.x, dyB = wB.y - OP.y, dzB = wB.z - OP.z;
                const float dxC = wC.x - OP.x, dyC = wC.y - OP.y, dzC = wC.z - OP.z;
                const float r2A = dxA*dxA + dyA*dyA + dzA*dzA;
                const float r2B = dxB*dxB + dyB*dyB + dzB*dzB;
                const float r2C = dxC*dxC + dyC*dyC + dzC*dzC;
                const float r2  = range * range;
                if (r2A > r2 && r2B > r2 && r2C > r2) continue;

                // Face back-face cull: if the omni is on the back side of
                // the face plane, no shading contribution. Centroid·N test.
                const Vector centroid = {
                    (wA.x + wB.x + wC.x) * (1.0f/3.0f),
                    (wA.y + wB.y + wC.y) * (1.0f/3.0f),
                    (wA.z + wB.z + wC.z) * (1.0f/3.0f),
                };
                const float toLight = (OP.x - centroid.x)*wN.x +
                                       (OP.y - centroid.y)*wN.y +
                                       (OP.z - centroid.z)*wN.z;
                if (toLight <= 0.0f) continue;

                // Per-(face, omni) slope-scaled bias: matches the
                // runtime per-pixel calculation (DeferredLighting:1399)
                // but done once per face/omni pair instead of per-
                // pixel. N·L = (toLight / |toOmni|). 0.2 clamp guards
                // grazing angles from exploding invNdotL.
                const float cxOL = OP.x - centroid.x;
                const float cyOL = OP.y - centroid.y;
                const float czOL = OP.z - centroid.z;
                const float ldist2 = cxOL*cxOL + cyOL*cyOL + czOL*czOL;
                const float lenInv = (ldist2 > 0.0f) ? 1.0f / std::sqrt(ldist2) : 0.0f;
                const float nDotL = toLight * lenInv;
                const float invNdotL = 1.0f / (nDotL > 0.2f ? nDotL : 0.2f);
                const int faceSlopeBias = int(float(slopeBiasInt) * (invNdotL - 1.0f));

                bool anyCovered = false;

                // Texel grid. Bary mode: (s, t) in [0, 1]² with s+t ≤ 1 =
                // triangle interior; texels with s+t > 1 are mirrored to
                // (1-s, 1-t) so edge dilation doesn't bleed wrong shadow
                // values. Planar mode: (s, t) maps linearly to the face's
                // pre-computed (uMin + s*uExt, vMin + t*vExt); the third
                // coordinate is solved from the face plane equation
                // (N·P = -NormProd, world-space, using wN computed above).
                const float invN1 = 1.0f / float(lmRes - 1);
                // Pre-solve world-space plane offset once per face: N·P = -d.
                const float planeD = -(wN.x * wA.x + wN.y * wA.y + wN.z * wA.z);
                const float invDomN = planar
                    ? (domAxis == 0 ? (std::fabs(wN.x) > 1.0e-6f ? 1.0f / wN.x : 0.0f)
                       : domAxis == 1 ? (std::fabs(wN.y) > 1.0e-6f ? 1.0f / wN.y : 0.0f)
                                      : (std::fabs(wN.z) > 1.0e-6f ? 1.0f / wN.z : 0.0f))
                    : 0.0f;
                for (int ty = 0; ty < lmRes; ++ty) {
                    float t = float(ty) * invN1;
                    for (int tx = 0; tx < lmRes; ++tx) {
                        float s = float(tx) * invN1;
                        Vector wp;
                        if (planar) {
                            // (s, t) → projected (u, v) → 3D point on face plane.
                            const float wu = uMin + s * uExt;
                            const float wv = vMin + t * vExt;
                            if      (domAxis == 0) {
                                wp.y = wu; wp.z = wv;
                                wp.x = -(wN.y * wu + wN.z * wv + planeD) * invDomN;
                            } else if (domAxis == 1) {
                                wp.x = wu; wp.z = wv;
                                wp.y = -(wN.x * wu + wN.z * wv + planeD) * invDomN;
                            } else {
                                wp.x = wu; wp.y = wv;
                                wp.z = -(wN.x * wu + wN.y * wv + planeD) * invDomN;
                            }
                        } else {
                            float ss = s, tt = t;
                            if (ss + tt > 1.0f) { ss = 1.0f - ss; tt = 1.0f - tt; }
                            const float w1 = 1.0f - ss - tt;
                            const float w2 = ss;
                            const float w3 = tt;
                            wp = {
                                w1*wA.x + w2*wB.x + w3*wC.x,
                                w1*wA.y + w2*wB.y + w3*wC.y,
                                w1*wA.z + w2*wB.z + w3*wC.z,
                            };
                        }

                        // Per-texel range cull.
                        const float dxp = wp.x - OP.x, dyp = wp.y - OP.y, dzp = wp.z - OP.z;
                        if (dxp*dxp + dyp*dyp + dzp*dzp > r2) continue;

                        uint8_t lit = SampleStaticCubeAtWorld(cr, wp, constBias, faceSlopeBias, bakeMatId);
                        uint8_t *dst = lm.texel(int(fi), tx, ty) + oi;
                        *dst = lit;
                        ++fBaked;
                        if (lit > 0) { anyCovered = true; ++fCovered; }
                    }
                }
                if (anyCovered) lm.setCovers(int(fi), oi);
            }
            texelsBaked.fetch_add(fBaked, std::memory_order_relaxed);
            texelsCovered.fetch_add(fCovered, std::memory_order_relaxed);
            faceCount.fetch_add(1, std::memory_order_relaxed);
        };  // bakeOneFace

        // Fan the per-face bake across the pool for big meshes; small meshes
        // run serial. Faces are independent (disjoint lm texel writes), so the
        // atomic cursor just hands them out. LOCAL join semaphore — the bake is
        // on its own background thread, so it must NOT touch renderns::tileDone.
        const uint32_t nf = T->FIndex;
        size_t P = ThreadPool::instance().size();
        // 32-face floor: greets's chunked room mesh is ~225 faces/chunk, so a
        // higher cutoff left the bulk serial. Each face is 11 omnis × lmRes²
        // samples — plenty to amortize the fan-out at 32+.
        if (nf >= 32 && P > 1) {
            std::atomic<uint32_t> cur{0};
            std::counting_semaphore<256> done{0};
            auto worker = [&]() {
                uint32_t fi;
                while ((fi = cur.fetch_add(1, std::memory_order_relaxed)) < nf)
                    bakeOneFace(fi);
                done.release();
            };
            for (size_t t = 1; t < P; ++t)
                ThreadPool::instance().enqueue([&worker]{ worker(); });
            worker();
            for (size_t t = 0; t < P; ++t) done.acquire();
        } else {
            for (DWord fi = 0; fi < nf; ++fi) bakeOneFace(fi);
        }
        reportProgress(meshCount, name, false);
    }
    // Force a final progress tick + newline so the summary line below
    // doesn't land on top of the in-place bar.
    reportProgress(meshCount, "", true);
    if (toTty) { std::fputc('\n', stderr); std::fflush(stderr); }

    // Diagnostic atlas/continuity dumps removed after they localized the
    // clipper-bary root cause; see commit log for the evidence.

    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::fprintf(stderr,
        "[LM] LightmapBake_Static: %zu/%zu meshes kept (%zu skipped dynamic) / "
        "%zu faces / %d omnis × %d² texels → %zu baked, %zu lit (%.1f%%) in %.1f ms\n",
        meshCount, considered, skippedDynamic,
        faceCount.load(), numCubeOmnis, lmRes,
        texelsBaked.load(), texelsCovered.load(),
        texelsBaked.load() ? 100.0 * double(texelsCovered.load()) / double(texelsBaked.load()) : 0.0,
        ms);
}

void Render_LightmapViz(Scene *Sc)
{
    const int mode = fds::FeatureFlags::shadow_lightmap_viz();
    if (mode == 0) return;
    if (!Sc || !Sc->staticLMTable) return;
    meka::GBuffer *gb = g_gbuffer;
    if (!gb || gb->lightmapMF.empty() || gb->lightmapST.empty()) return;
    const uint32_t *plMF = gb->lightmapMF.data();
    const uint16_t *plST = gb->lightmapST.data();
    const auto &table = *Sc->staticLMTable;

    auto packRGB = [](int R, int G, int B) -> uint32_t {
        if (R < 0) R = 0; if (R > 255) R = 255;
        if (G < 0) G = 0; if (G > 255) G = 255;
        if (B < 0) B = 0; if (B > 255) B = 255;
        return uint32_t(B) | (uint32_t(G) << 8) | (uint32_t(R) << 16) | 0xFF000000u;
    };

    uint32_t *out = reinterpret_cast<uint32_t*>(VPage);
    for (int y = 0; y < YRes; ++y) {
        const size_t row = size_t(y) * size_t(XRes);
        for (int x = 0; x < XRes; ++x) {
            const size_t i = row + size_t(x);
            const uint32_t mf = plMF[i];
            const uint16_t meshLMId = uint16_t(mf >> 16);
            if (meshLMId == 0) {
                if (mode == 1) {
                    // Mark no-lightmap pixels in dim red so we can see
                    // coverage at a glance.
                    out[i] = packRGB(60, 0, 0);
                }
                continue;
            }
            const uint16_t faceIdx = uint16_t(mf & 0xFFFF);
            const uint16_t st = plST[i];
            const int sB = int(st & 0xFF);
            const int tB = int((st >> 8) & 0xFF);

            switch (mode) {
              case 1: { // mesh ID greyscale
                const int g = std::min(255, 60 + meshLMId * 40);
                out[i] = packRGB(g, g, g);
                break;
              }
              case 2: { // face ID color hash
                const uint32_t h = uint32_t(faceIdx) * 2654435761u;
                out[i] = packRGB(int((h >> 24) & 0xFF),
                                 int((h >> 16) & 0xFF),
                                 int((h >>  8) & 0xFF));
                break;
              }
              case 3: out[i] = packRGB(255 - sB, sB, 0); break;
              case 4: out[i] = packRGB(255 - tB, 0, tB); break;
              case 5: { // lightmap factor for omni 0
                if (size_t(meshLMId) >= table.size()) { out[i] = packRGB(255, 0, 255); break; }
                TriMesh *T = table[meshLMId];
                if (!T || !T->staticShadowLM) { out[i] = packRGB(255, 0, 255); break; }
                StaticShadowLightmap &lm = *T->staticShadowLM;
                if (lm.numOmnis == 0 || faceIdx >= lm.numFaces) { out[i] = packRGB(255, 128, 0); break; }
                // Map s,t (8-bit) → texel coord [0, lmRes). Truncate; the
                // kernel will eventually do bilinear, but this is just viz.
                int tx = (sB * lm.lmRes) >> 8;
                int ty = (tB * lm.lmRes) >> 8;
                if (tx >= lm.lmRes) tx = lm.lmRes - 1;
                if (ty >= lm.lmRes) ty = lm.lmRes - 1;
                const uint8_t *p = lm.texel(faceIdx, tx, ty);
                const int factor = int(p[0]);
                out[i] = packRGB(factor, factor, factor);
                break;
              }
              case 9: { // #66: bary world delta. For each lightmap pixel,
                // compute the world position the runtime bary points to
                // (interp F.A/B/C->Pos by (1-s-t, s, t), then mesh-to-world)
                // and compare to the pixel's actual world (from depth +
                // camera). Color-code |delta| in world units: green (≤0.1),
                // yellow (≤0.5), orange (≤1.0), red (>1.0).
                if (size_t(meshLMId) >= table.size()) { out[i] = packRGB(255, 0, 255); break; }
                TriMesh *T = table[meshLMId];
                if (!T || faceIdx >= T->FIndex) { out[i] = packRGB(255, 0, 255); break; }
                const Face &F = T->Faces[faceIdx];
                if (!F.A || !F.B || !F.C) { out[i] = packRGB(255, 0, 255); break; }
                const float sf = float(sB) * (1.0f / 255.0f);
                const float tf = float(tB) * (1.0f / 255.0f);
                const float wAf = 1.0f - sf - tf;
                Vector op{
                    wAf*F.A->Pos.x + sf*F.B->Pos.x + tf*F.C->Pos.x,
                    wAf*F.A->Pos.y + sf*F.B->Pos.y + tf*F.C->Pos.y,
                    wAf*F.A->Pos.z + sf*F.B->Pos.z + tf*F.C->Pos.z,
                };
                Vector baryWorld;
                MatrixXVector(T->RotMat, &op, &baryWorld);
                baryWorld.x += T->IPos.x;
                baryWorld.y += T->IPos.y;
                baryWorld.z += T->IPos.z;
                const word z16 = ZPage16[i];
                if (z16 == 0 || !View) { out[i] = packRGB(255, 0, 255); break; }
                const float invZS = (g_zscale != 0.0f) ? 1.0f / g_zscale : 0.0f;
                const float zv = float(0xFF80 - z16) * invZS;
                const float invFOVX = (FOVX != 0.0f) ? 1.0f / FOVX : 0.0f;
                const float invFOVY = (FOVY != 0.0f) ? 1.0f / FOVY : 0.0f;
                const float xv = (float(x) - CntrEX) * zv * invFOVX;
                const float yv = (CntrEY - float(y)) * zv * invFOVY;
                Vector pixelWorld;
                pixelWorld.x = View->Mat[0][0]*xv + View->Mat[1][0]*yv + View->Mat[2][0]*zv + View->ISource.x;
                pixelWorld.y = View->Mat[0][1]*xv + View->Mat[1][1]*yv + View->Mat[2][1]*zv + View->ISource.y;
                pixelWorld.z = View->Mat[0][2]*xv + View->Mat[1][2]*yv + View->Mat[2][2]*zv + View->ISource.z;
                const float ddx = baryWorld.x - pixelWorld.x;
                const float ddy = baryWorld.y - pixelWorld.y;
                const float ddz = baryWorld.z - pixelWorld.z;
                const float delta = std::sqrt(ddx*ddx + ddy*ddy + ddz*ddz);
                if      (delta <= 0.1f) out[i] = packRGB(  0, 255,   0);
                else if (delta <= 0.5f) out[i] = packRGB(255, 255,   0);
                else if (delta <= 1.0f) out[i] = packRGB(255, 128,   0);
                else                    out[i] = packRGB(255,   0,   0);
                break;
              }
              default: break;
            }
        }
    }
}

// Debug entry — reaches into the anonymous-namespace SampleStaticCubeAtWorld
// via a sibling re-open. Reaches the runtime per-pixel deferred path via
// LightmapBake.h. Used only when --shadow-lightmap-recompute-bake is set,
// to verify whether the bake function itself agrees with CubeShadow_Sample
// at the same world point. If output looks correct with this flag, the
// bake function is fine and the bug is downstream in atlas / bary; if it
// matches the existing broken lightmap output, the bake function is wrong.
namespace { uint8_t SampleStaticCubeAtWorld(const CubeShadowRef &, const Vector &, int, int, int); }
uint8_t LightmapBake_DebugSampleAtWorld(int cubeIdx, float wx, float wy, float wz,
                                          int constBias, int slopeBiasInt)
{
    if (cubeIdx < 0 || size_t(cubeIdx) >= g_cubeShadowRefs.size()) return 255;
    const CubeShadowRef &cr = g_cubeShadowRefs[cubeIdx];
    Vector wp{wx, wy, wz};
    // Debug entry: no face context here, so always use Depth mode for
    // a clean apples-to-apples against the runtime CubeShadow_Sample
    // depth path. (Runtime --shadow-lightmap-recompute-bake comparison
    // is only meaningful in Depth mode anyway.)
    return SampleStaticCubeAtWorld(cr, wp, constBias, slopeBiasInt, -1);
}

}  // namespace fds
