// ReflFaceCull.cpp — the mirror-shard reflection bake's PER-FACE cone cull.
//
// WHY THIS IS ITS OWN TRANSLATION UNIT AND ITS OWN noinline FUNCTION, and not
// forty lines inside Transform_Objects where it logically belongs:
// Transform_Objects is compiled with -ffp-contract=fast, and adding straight-
// line code to it changes how the compiler contracts and schedules the
// floating point in the loops AROUND it — even code that never executes on
// the pass being measured. That is not a theory here, it is measured: with
// this block written inline, all three scene byte-pins moved (greets
// 778fa6ac→7a6370a1, fountain 8db68ccb→eebf68e6, city 3cbe42b1→80583b85) on a
// main-camera snapshot that never runs a shard bake at all. It is the same
// hazard docs/VISIBILITY_PLAN.md §8a records for the --xfrm_pass_prof census.
// Behind one noinline call across a TU boundary the pins are byte-identical.
//
// WHAT THE CULL IS. Each shard of the shattered mirror bakes the room through
// its own narrow off-axis cone (fds::g_reflConeApex / g_reflFaceConeDir /
// g_reflFaceConeTan2, built by MirrorShatter from that shard's actual
// projection). A face whose own world bounding SPHERE lies entirely outside
// that cone cannot paint a pixel of the shard, so it is dropped whole.
//
// WHY PER FACE AND NOT PER VERTEX. The per-VERTEX form this replaces
// (--shard_cone_cull=1, g_reflVertCull, ddb1d15) decided FACE visibility from
// VERTEX positions and stamped every rejected vertex with a fake screen
// position. greets's room is wall quads metres across seen through a ~1° cone,
// so a quad whose INTERIOR covered the entire shard view had all three corners
// rejected and vanished, and a quad with only some corners rejected rasterized
// THROUGH the fakes. Two thirds of the reflection was missing. The per-FACE
// test cannot do either: a face that reaches the cone survives ENTIRE, and a
// face that survives is transformed and rasterized with its vertices
// UNTOUCHED. That is the correctness invariant, and it is why this arm
// measures byte-identical to no cull at all.

#include <cmath>
#include <cstring>
#include <vector>

#include <Base/FDS_DEFS.H>
#include <Base/Compiler.h>   // FDS_NOINLINE
#include <Base/FDS_DECS.H>
#include <Base/FDS_VARS.H>
#include <Base/FeatureFlags.h>
#include <Base/FrameState.h>
#include <Base/TriMesh.h>
#include <Base/Face.h>
#include <Base/Material.h>

// g_inShadowPass lives at global scope (RENDER/Shadows.cpp), not in fds::.
extern thread_local bool g_inShadowPass;

namespace fds {

extern bool g_envBakeSkipDynamic;
bool EnvBake_HasSkipFaces();
bool EnvBake_FaceExcluded(const Face* F, TriMesh* T);

// Walk one mesh's faces in WORLD space, off the static worldVerts cache and
// before a single vertex has been transformed, and return the per-face keep
// mask the caller's face loop reads. Scratch is thread_local and reused, so
// steady state allocates nothing.
//
// The face loop's OWN early rejects are replicated here EXACTLY (same
// expressions, same operands) rather than approximated: this mask is only
// allowed to drop faces that loop would have dropped anyway or that the cone
// proves invisible. Face_MainOnly is the one that matters for cost — under
// --greets_displace the displaced stone detail is main-camera only (the flat
// proxy reflects instead) and it is 16× the face count of everything else.
FDS_NOINLINE
void ReflFaceCull_Mark(const TriMesh *T, const Face *tFaces, const Vertex *tVerts,
                       const Vector &AP, bool offscreenPass,
                       const uint8_t *&keepOut) {
    keepOut = nullptr;
    if (!T->worldVerts || !T->FIndex) return;

    static thread_local std::vector<uint8_t> sKeep;
    if (sKeep.size() < T->FIndex) sKeep.resize(T->FIndex);

    const Vector *wv = T->worldVerts;
    const float apx = g_reflConeApex.x, apy = g_reflConeApex.y, apz = g_reflConeApex.z;
    const float dx  = g_reflFaceConeDir.x, dy = g_reflFaceConeDir.y, dz = g_reflFaceConeDir.z;
    // tan² → the cone's cos²θ and 1/sinθ (the apex back-off per unit sphere
    // radius in the standard sphere-vs-INFINITE-cone test).
    const float k      = g_reflFaceConeTan2;
    const float cos2   = 1.0f / (1.0f + k);
    const float invSin = std::sqrt((1.0f + k) / k);

    const bool bfKeepAll = (g_inShadowPass && !FeatureFlags::shadow_backface_cull())
                         || FeatureFlags::xpar_force_twosided();
    const bool envSkip   = g_envBakeSkipDynamic && EnvBake_HasSkipFaces();

    uint64_t nCull = 0;
    for (DWord fi = 0; fi < T->FIndex; ++fi) {
        const Face &F = tFaces[fi];
        if (offscreenPass && (F.Flags & Face_MainOnly))   { sKeep[fi] = 0; ++nCull; continue; }
        if (envSkip && EnvBake_FaceExcluded(&F, const_cast<TriMesh*>(T)))       { sKeep[fi] = 0; ++nCull; continue; }
        // Backface — byte-for-byte the face loop's own expression, same AP and
        // same Face, so the two can never disagree about what is dropped.
        if (!bfKeepAll && F.Txtr && !(F.Txtr->Flags & Mat_TwoSided)
            && !(AP.x*F.N.x + AP.y*F.N.y + AP.z*F.N.z < F.NormProd)) {
            sKeep[fi] = 0; ++nCull; continue;
        }
        // Face bounding sphere: centroid of the three world verts, radius =
        // furthest corner. Then the sphere-vs-infinite-cone test — shift the
        // apex back by R/sinθ and the sphere misses the cone iff its centre
        // falls outside that widened cone, or the sphere sits entirely behind
        // the apex plane. A sphere that so much as touches the cone is kept.
        const Vector &wa = wv[F.A - tVerts];
        const Vector &wb = wv[F.B - tVerts];
        const Vector &wc = wv[F.C - tVerts];
        const float ccx = (wa.x + wb.x + wc.x) * (1.0f / 3.0f);
        const float ccy = (wa.y + wb.y + wc.y) * (1.0f / 3.0f);
        const float ccz = (wa.z + wb.z + wc.z) * (1.0f / 3.0f);
        float qx = wa.x - ccx, qy = wa.y - ccy, qz = wa.z - ccz;
        float r2 = qx*qx + qy*qy + qz*qz;
        qx = wb.x - ccx; qy = wb.y - ccy; qz = wb.z - ccz;
        float d2 = qx*qx + qy*qy + qz*qz; if (d2 > r2) r2 = d2;
        qx = wc.x - ccx; qy = wc.y - ccy; qz = wc.z - ccz;
        d2 = qx*qx + qy*qy + qz*qz; if (d2 > r2) r2 = d2;
        const float R  = std::sqrt(r2);
        const float vx = ccx - apx, vy = ccy - apy, vz = ccz - apz;
        const float back = R * invSin;
        const float ex = vx + back*dx, ey = vy + back*dy, ez = vz + back*dz;
        const float ad = ex*dx + ey*dy + ez*dz;
        bool keep = false;
        if (ad > 0.0f && ad*ad >= cos2 * (ex*ex + ey*ey + ez*ez))
            keep = (vx*dx + vy*dy + vz*dz) >= -R;
        sKeep[fi] = keep ? 1 : 0;
        if (!keep) ++nCull;
    }
    g_reflFaceTested += T->FIndex;
    g_reflFaceCulled += nCull;
    keepOut = sKeep.data();
}

}  // namespace fds
