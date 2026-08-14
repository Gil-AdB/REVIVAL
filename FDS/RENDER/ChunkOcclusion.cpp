#include "ChunkOcclusion.h"
#include "WorldAabb.h"        // WorldAabb_UpdateScene
#include "OffscreenView.h"    // g_offscreenViewDepth

#include <Base/FDS_VARS.H>    // View, FOVX/FOVY, CntrEX/CntrEY, XRes/YRes, ZPage16, g_zscale
#include <Base/FDS_DEFS.H>
#include <Base/Scene.h>
#include <Base/Camera.h>
#include <Base/TriMesh.h>
#include <Base/Object.h>
#include <Base/Vector.h>
#include <Base/FeatureFlags.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

// Chunk occlusion cull — PREVIOUS-FRAME hi-Z reproject (docs/VISIBILITY_PLAN.md
// §7). TIMING SUBTLETY: the tick CLEARS ZPage16 (PROF_ZCLR) before Transform,
// so last frame's depth cannot be read at BeginFrame — it is captured at
// EndFrame (right after Render, while ZPage16 holds this frame's final opaque
// depth) together with the camera that rendered it. The NEXT frame's
// BeginFrame arms the cull from that capture, and the AABB test projects with
// the CAPTURED (prev) camera — exact for the buffer's content — plus a depth
// margin dilated by the camera's translation since the capture.
//
// Guardrails (the honest ones — byte-exactness is explicitly NOT claimed for
// this prev-frame design):
//   • prev-camera projection: rotation-revealed chunks project off-buffer -> keep.
//   • bias + 2*|camera delta| depth margin: translation parallax bounded.
//   • min-pool coverage: any sky/untouched texel in a block = far plane ->
//     never cull across it; near-plane straddle / off-buffer rect -> keep.
//   • MAIN VIEW ONLY (Transform gates on !_offscreenPass && !offAxis) — the
//     shadow / mirror-RTT / env-probe cameras keep their whole worlds.
//   • first frame of a scene: no capture yet -> cull inert.
//   • snapshot harness sets g_occlSnapshotInert -> cull inert in pin dumps.
//   • FDS_OCCL_VERIFY (--chunk_occl_verify): each culled chunk is re-tested at
//     EndFrame against the FINAL current-frame depth; a chunk that would have
//     contributed a pixel counts as a violation (upper bound — the rect test
//     is loose at silhouettes). A camera-sweep bench must come back clean.

namespace fds {

bool          g_chunkOcclActive = false;
bool          g_visStatsActive  = false;
ChunkVisStats g_chunkVisStats;
bool          g_occlSnapshotInert   = false;
long long     g_occlViolationsTotal = 0;

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

// ZPage16 encoding (Mekalele / DeferredSurfaceKernel):
//   zEnc = 0xFF80 - round(g_zscale * viewZ); larger = nearer; 0 = sky/untouched.
constexpr int kZEncNear = 0xFF80;

// A captured main-view depth pyramid + the camera that rendered it.
struct Capture {
    bool  valid = false;
    const Scene* sc = nullptr;
    int   W = 0, H = 0, block = 1;
    std::vector<float> z;          // per-block FARTHEST opaque view-z
                                   // (= decode(min zEnc); sky -> far plane)
    float mat[3][3] = {};
    float apex[3]   = {};
    float sfx = 0, sfy = 0, scx = 0, scy = 0;   // projection into hi-Z texels
    float nearZ = 0.01f;
    double poolMs = 0.0;
};
thread_local Capture cap;

// Armed per-frame cull state (reads cap; cap is only rewritten in EndFrame,
// after the last query).
struct Armed {
    bool  on = false;
    float biasEff = 0.5f;          // base bias + 2*|camera translation delta|
};
thread_local Armed g;

// Culled-chunk record for the EndFrame verify pass.
struct CulledRec { float mn[3], mx[3]; };
thread_local std::vector<CulledRec> gCulled;
thread_local bool gVerify = false;

inline void projectCap(const Capture& c, const float w[3],
                       float& sx, float& sy, float& vz) {
    const float dx = w[0] - c.apex[0];
    const float dy = w[1] - c.apex[1];
    const float dz = w[2] - c.apex[2];
    const float vx = c.mat[0][0]*dx + c.mat[0][1]*dy + c.mat[0][2]*dz;
    const float vy = c.mat[1][0]*dx + c.mat[1][1]*dy + c.mat[1][2]*dz;
    vz             = c.mat[2][0]*dx + c.mat[2][1]*dy + c.mat[2][2]*dz;
    if (vz > 1e-6f) {
        const float rz = 1.0f / vz;
        sx = c.scx + c.sfx * vx * rz;
        sy = c.scy - c.sfy * vy * rz;
    } else {
        sx = sy = 0.0f;
    }
}

// AABB-vs-capture: true iff the whole box projects inside the buffer, in
// front of the near plane, and its nearest corner is at least `bias` BEHIND
// the farthest opaque depth over its (1-texel-expanded) screen rect.
bool hiddenInCapture(const Capture& c, const float mn[3], const float mx[3],
                     float bias) {
    float rx0 = kInf, ry0 = kInf, rx1 = -kInf, ry1 = -kInf, nearestZ = kInf;
    for (int k = 0; k < 8; ++k) {
        const float w[3] = {
            (k & 1) ? mx[0] : mn[0],
            (k & 2) ? mx[1] : mn[1],
            (k & 4) ? mx[2] : mn[2],
        };
        float sx, sy, vz;
        projectCap(c, w, sx, sy, vz);
        if (vz <= c.nearZ) return false;               // straddle -> keep
        rx0 = std::min(rx0, sx); rx1 = std::max(rx1, sx);
        ry0 = std::min(ry0, sy); ry1 = std::max(ry1, sy);
        if (vz < nearestZ) nearestZ = vz;
    }
    const int ix0 = int(std::floor(rx0)) - 1, ix1 = int(std::ceil(rx1)) + 1;
    const int iy0 = int(std::floor(ry0)) - 1, iy1 = int(std::ceil(ry1)) + 1;
    if (ix0 < 0 || iy0 < 0 || ix1 >= c.W || iy1 >= c.H) return false;  // off-buffer

    float hiZ = -kInf;
    for (int y = iy0; y <= iy1; ++y) {
        const float* row = c.z.data() + size_t(y) * c.W;
        for (int x = ix0; x <= ix1; ++x)
            if (row[x] > hiZ) hiZ = row[x];
    }
    return nearestZ >= hiZ + bias;
}

// Min-pool the CURRENT ZPage16 + camera into `out`. Caller guarantees ZPage16
// holds a fully rendered main-view frame (i.e. call only right after Render).
void captureCurrent(Capture& out, const Scene* sc) {
    out.valid = false;
    out.sc = sc;
    const float zscale = g_zscale > 1e-6f ? g_zscale : 0.0f;
    if (!ZPage16 || zscale <= 0.0f || XRes <= 0 || YRes <= 0 || !View) return;

    const auto t0 = std::chrono::steady_clock::now();
    const int reqW  = std::max(16, FeatureFlags::chunk_occl_res());
    const int block = std::max(1, (XRes + reqW - 1) / reqW);
    const int W = (XRes + block - 1) / block;
    const int H = (YRes + block - 1) / block;
    out.W = W; out.H = H; out.block = block;
    out.z.assign(size_t(W) * H, 0.0f);
    const word* zb = ZPage16;
    const float farViewZ = float(kZEncNear) / zscale;
    for (int cy = 0; cy < H; ++cy) {
        const int y0 = cy * block, y1 = std::min<int>(YRes, y0 + block);
        float* orow = out.z.data() + size_t(cy) * W;
        for (int cx = 0; cx < W; ++cx) {
            const int x0 = cx * block, x1 = std::min<int>(XRes, x0 + block);
            int minEnc = 0xFFFF;
            for (int y = y0; y < y1; ++y) {
                const word* zr = zb + size_t(y) * XRes;
                for (int x = x0; x < x1; ++x)
                    if (zr[x] < minEnc) minEnc = zr[x];
            }
            orow[cx] = (minEnc <= 0) ? farViewZ
                                     : float(kZEncNear - minEnc) / zscale;
        }
    }
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out.mat[r][c] = View->Mat[r][c];
    out.apex[0] = View->ISource.x;
    out.apex[1] = View->ISource.y;
    out.apex[2] = View->ISource.z;
    const float kx = float(W) / float(XRes);
    const float ky = float(H) / float(YRes);
    out.sfx = FOVX * kx;  out.sfy = FOVY * ky;
    out.scx = CntrEX * kx; out.scy = CntrEY * ky;
    out.nearZ = (sc && sc->NZP > 0.01f) ? sc->NZP : 0.01f;
    const auto t1 = std::chrono::steady_clock::now();
    out.poolMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    out.valid = true;
}

}  // namespace

void ChunkOcclusion_BeginFrame(Scene* sc) {
    g.on = false;
    g_chunkOcclActive = false;
    g_visStatsActive  = false;
    g_chunkVisStats   = ChunkVisStats{};
    gCulled.clear();
    gVerify = false;

    if (!sc || !View) return;
    if (g_offscreenViewDepth > 0) return;               // main view only

    g_visStatsActive = FeatureFlags::vis_stats();
    if (!FeatureFlags::chunk_occlusion()) return;
    if (g_occlSnapshotInert) return;                    // pin dumps: inert
    if (!cap.valid || cap.sc != sc) return;             // no usable prev frame

    // Refresh world AABBs (chunks carry valid static boxes from the Phase-A
    // split; this fills momy/robot/etc. + re-poses dynamic movers).
    WorldAabb_UpdateScene(sc);

    // Depth margin: base bias + 2x the camera translation since the capture.
    // Rotation needs no dilation — we project with the captured camera, so a
    // rotation-revealed chunk lands off-buffer and is kept.
    const float ddx = View->ISource.x - cap.apex[0];
    const float ddy = View->ISource.y - cap.apex[1];
    const float ddz = View->ISource.z - cap.apex[2];
    const float camDelta = std::sqrt(ddx*ddx + ddy*ddy + ddz*ddz);
    g.biasEff = FeatureFlags::chunk_occl_bias() + 2.0f * camDelta;
    gVerify   = FeatureFlags::chunk_occl_verify();

    g.on = true;
    g_chunkOcclActive = true;
    g_chunkVisStats.prepassMs = cap.poolMs;             // paid last EndFrame
}

bool ChunkOcclusion_CullsAabb(const float mn[3], const float mx[3],
                              int verts, int faces) {
    if (!g.on) return false;
    ++g_chunkVisStats.chunksTested;
    if (!hiddenInCapture(cap, mn, mx, g.biasEff)) return false;
    ++g_chunkVisStats.chunksCulled;
    g_chunkVisStats.vertsCulled += verts;
    g_chunkVisStats.facesCulled += faces;
    if (gVerify) {
        CulledRec r;
        for (int a = 0; a < 3; ++a) { r.mn[a] = mn[a]; r.mx[a] = mx[a]; }
        gCulled.push_back(r);
    }
    return true;
}

void ChunkOcclusion_EndFrame(Scene* sc) {
    const bool wantNext = FeatureFlags::chunk_occlusion()
                       && !g_occlSnapshotInert
                       && g_offscreenViewDepth == 0;

    // Verify pass (--chunk_occl_verify): re-test every culled chunk against
    // the FINAL current-frame depth from the CURRENT camera. A culled chunk
    // that is NOT hidden in the final depth would have contributed a pixel —
    // a real over-cull (upper bound: the max-over-rect is loose at
    // silhouettes). Uses the fresh capture below, so order: capture first.
    Capture fresh;
    if (wantNext || (gVerify && !gCulled.empty()))
        captureCurrent(fresh, sc);

    long long violations = 0;
    if (gVerify && fresh.valid) {
        for (const CulledRec& r : gCulled)
            if (!hiddenInCapture(fresh, r.mn, r.mx, 0.0f))
                ++violations;
        g_occlViolationsTotal += violations;
    }

    if (g_visStatsActive) {
        const ChunkVisStats& s = g_chunkVisStats;
        const long long frustumRej = s.meshesSeen - s.meshesXformed - s.chunksCulled;
        char vbuf[64] = "";
        if (gVerify)
            std::snprintf(vbuf, sizeof vbuf, " | VERIFY viol=%lld%s",
                          violations, violations ? "  <-- OVER-CULL" : " (clean)");
        std::fprintf(stderr,
            "[VIS-STATS] meshes seen=%lld xformed=%lld frustum-rej=%lld | "
            "verts seen=%lld xformed=%lld | occl: tested=%lld culled=%lld "
            "verts=%lld faces=%lld bias=%.2f | prevZ %dx%d(/%d) pool=%.3fms%s\n",
            s.meshesSeen, s.meshesXformed, frustumRej,
            s.vertsSeen, s.vertsXformed, s.chunksTested, s.chunksCulled,
            s.vertsCulled, s.facesCulled, g.on ? g.biasEff : 0.0f,
            cap.valid ? cap.W : 0, cap.valid ? cap.H : 0,
            cap.valid ? cap.block : 0, fresh.valid ? fresh.poolMs : 0.0, vbuf);
    }

    if (wantNext && fresh.valid) cap = std::move(fresh);
    else if (!wantNext)          cap.valid = false;     // flag off: drop stale state

    g.on = false;
    g_chunkOcclActive = false;
    g_visStatsActive  = false;
    gCulled.clear();
    gVerify = false;
}

}  // namespace fds
