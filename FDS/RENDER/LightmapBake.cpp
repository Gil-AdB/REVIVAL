// Static shadow lightmap bake — Option A: per-face N×N atlas of pre-baked
// shadow factors, one byte per (face, texel, cube-omni) tuple. See
// docs/STATIC_SHADOW_LIGHTMAPS.md.

#include "RENDER/LightmapBake.h"

#include "Base/FeatureFlags.h"
#include "Base/Scene.h"
#include "Base/StaticShadowLightmap.h"
#include "Base/TriMesh.h"
#include "Base/Object.h"
#include "Base/Omni.h"
#include "Base/Vector.h"
#include "Base/Matrix.h"
#include "FILLERS/ShadowMap.h"
#include "Base/FDS_DECS.H"
#include "Base/FDS_VARS.H"  // MatrixXVector template

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace fds {
namespace {

// ─── World-space static-cube sampler ─────────────────────────────────────
// Bake-time parallel to CubeShadow_Sample (FDS/FILLERS/ShadowMap.h), but
// takes a world-space sample point instead of view-space. Reads only the
// static-occluder depth buffer (`sm.depth`), since the lightmap caches the
// static-scene contribution. Returns shadow factor in [0, 255] where 255
// is fully lit and 0 is fully shadowed.
uint8_t SampleStaticCubeAtWorld(const CubeShadowRef &cr,
                                 const Vector &worldPos,
                                 int constBias, int slopeBiasInt)
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
    const uint16_t *z0 = sm.depth.data() + rowOfs;
    const uint16_t *z1 = z0 + sm.xres;
    const float fx = smX - float(iX);
    const float fy = smY - float(iY);
    const float w00 = (1.0f - fx) * (1.0f - fy);
    const float w10 =         fx  * (1.0f - fy);
    const float w01 = (1.0f - fx) *         fy;
    const float w11 =         fx  *         fy;

    int pixZenc = 0xFF80 - int(lz * sm.zScale);
    if (pixZenc < 0) pixZenc = 0;
    if (pixZenc > 0xFFFF) pixZenc = 0xFFFF;
    const int biased = pixZenc + constBias + slopeBiasInt;

    float occ = 0.0f;
    if (biased < int(z0[iX  ])) occ += w00;
    if (biased < int(z0[iX+1])) occ += w10;
    if (biased < int(z1[iX  ])) occ += w01;
    if (biased < int(z1[iX+1])) occ += w11;

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

void LightmapBake_Static(Scene *Sc)
{
    if (!fds::FeatureFlags::shadow_lightmap()) return;
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
    size_t meshCount = 0, faceCount = 0;
    size_t texelsBaked = 0, texelsCovered = 0;
    size_t skippedDynamic = 0, considered = 0;

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
        std::fprintf(stderr, "[LM-KEEP] '%s' (%u faces)\n",
                     name, unsigned(T->FIndex));

        // Allocate / reset lightmap on this mesh.
        if (T->staticShadowLM) { T->staticShadowLM->clear(); }
        else                   { T->staticShadowLM = new StaticShadowLightmap(); }
        StaticShadowLightmap &lm = *T->staticShadowLM;
        lm.allocate(int(T->FIndex), numCubeOmnis, lmRes);
        for (int oi = 0; oi < numCubeOmnis; ++oi) lm.omniSceneIdx[oi] = oi;

        ++meshCount;
        const Vector &IP = T->IPos;
        auto toWorld = [&](const Vector &objPos, Vector &out) {
            MatrixXVector(T->RotMat, const_cast<Vector*>(&objPos), &out);
            out.x += IP.x; out.y += IP.y; out.z += IP.z;
        };

        for (DWord fi = 0; fi < T->FIndex; ++fi) {
            const Face &F = T->Faces[fi];
            if (!F.A || !F.B || !F.C) continue;
            ++faceCount;

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

            // Per cube omni: face-level culls then per-texel bake.
            for (int oi = 0; oi < numCubeOmnis; ++oi) {
                const CubeShadowRef &cr = g_cubeShadowRefs[oi];
                Omni *O = cr.omni;
                if (!O) continue;
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

                bool anyCovered = false;

                // Texel grid: (s, t) in [0, 1]^2 with s+t <= 1 = triangle
                // interior; texels with s+t > 1 are mirrored to (1-s, 1-t)
                // so edge dilation doesn't bleed wrong shadow values.
                const float invN1 = 1.0f / float(lmRes - 1);
                for (int ty = 0; ty < lmRes; ++ty) {
                    float t = float(ty) * invN1;
                    for (int tx = 0; tx < lmRes; ++tx) {
                        float s = float(tx) * invN1;
                        float ss = s, tt = t;
                        if (ss + tt > 1.0f) { ss = 1.0f - ss; tt = 1.0f - tt; }
                        const float w1 = 1.0f - ss - tt;  // weight of A
                        const float w2 = ss;              // weight of B
                        const float w3 = tt;              // weight of C
                        const Vector wp = {
                            w1*wA.x + w2*wB.x + w3*wC.x,
                            w1*wA.y + w2*wB.y + w3*wC.y,
                            w1*wA.z + w2*wB.z + w3*wC.z,
                        };

                        // Per-texel range cull.
                        const float dxp = wp.x - OP.x, dyp = wp.y - OP.y, dzp = wp.z - OP.z;
                        if (dxp*dxp + dyp*dyp + dzp*dzp > r2) continue;

                        uint8_t lit = SampleStaticCubeAtWorld(cr, wp, constBias, slopeBiasInt);
                        uint8_t *dst = lm.texel(int(fi), tx, ty) + oi;
                        *dst = lit;
                        ++texelsBaked;
                        if (lit > 0) { anyCovered = true; ++texelsCovered; }
                    }
                }
                if (anyCovered) lm.setCovers(int(fi), oi);
            }
        }
    }

    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::fprintf(stderr,
        "[LM] LightmapBake_Static: %zu/%zu meshes kept (%zu skipped dynamic) / "
        "%zu faces / %d omnis × %d² texels → %zu baked, %zu lit (%.1f%%) in %.1f ms\n",
        meshCount, considered, skippedDynamic,
        faceCount, numCubeOmnis, lmRes,
        texelsBaked, texelsCovered,
        texelsBaked ? 100.0 * double(texelsCovered) / double(texelsBaked) : 0.0,
        ms);
}

}  // namespace fds
