#include "ShadowMap.h"

#include "Base/Scene.h"
#include "Base/Omni.h"
#include "Base/Vertex.h"
#include "Base/VertexFrame.h"  // SoA Phase 4: F->frame access in shadow-validate dev path
#include "Base/FDS_DEFS.H"
#include "Base/FDS_DECS.H"
#include "Base/FDS_VARS.H"
#include "Base/FeatureFlags.h"
#include "Base/MemCensus.h"
#include "F4Vec.h"
#include "TheOtherBarry.h"
#include "ClipperTileRect.h"

#include <simd/vectorclass.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>

ShadowSwzShape g_shadowSwzShape{3, 3, 7, 7};   // 8x8 until the getter runs

thread_local ShadowMap *g_currentShadowMap = nullptr;
thread_local int g_shadowRasterBox[4] = {0x7FFFFFFF, 0x7FFFFFFF, -1, -1};
std::vector<ShadowMap> g_shadowMaps;
std::vector<CubeShadowRef> g_cubeShadowRefs;

// Shadow-map debug viewer state. See ShadowMap.h for protocol.
int g_shadowViewIdx = -1;
std::atomic<bool> g_shadowFullscreenView{false};
// 0 = static (sm.packSD), 1 = dynamic (sm.packDyn), 2 = combined
// (per-texel closest-by-depth pick of the two — matches what
// CubeShadow_Sample's PolyId path actually reads).
// Cycled by ShadowMap_ViewModeCycle (bound to B in REV.CPP).
std::atomic<int> g_shadowViewMode{0};

// ─── 8x8 PolyId uniformity pyramid ────────────────────────────────────────
// One u32 per 8x8 block = the ShadowMatID every texel of the block's 9x9
// APRON carries, or kShadowUniMixed. See ShadowMap.h for why the apron and
// why the tap that reads it is byte-null.
//
// Structure: a per-texel-ROW pass that collapses each block column's 9-texel
// run to one id, then a per-BLOCK pass that collapses 9 consecutive rows of
// that. The alternative — scanning the 9x9 square per block — re-reads the
// shared column between horizontally adjacent blocks and, worse, walks the
// plane in a 9-row zig-zag per block row. This form touches every texel once,
// strictly front-to-back, so the pass runs at streaming speed over a plane the
// raster has just left in cache. The row scratch is blocksX u32 per row —
// 512^2 face = 128 KB, allocated once per call from a thread_local.
void ShadowMap_BuildUniformity(ShadowMap &sm, bool dynPlane)
{
    const std::vector<uint32_t> &src = dynPlane ? sm.packDyn : sm.packSD;
    std::vector<uint32_t> &dst       = dynPlane ? sm.uniDyn  : sm.uniSD;
    if (sm.xres <= 0 || sm.yres <= 0 || src.empty()) { dst.clear(); return; }
    const int bw = (sm.xres + kShadowUniSize - 1) >> kShadowUniShift;
    const int bh = (sm.yres + kShadowUniSize - 1) >> kShadowUniShift;
    sm.uniW = bw;
    sm.uniH = bh;
    // Phase A cleared this plane, so id 0 — "no occluder" — is the correct
    // entry for every block the bake did not touch. Zero the whole pyramid
    // (16 KB for a 512^2 face) and then rebuild ONLY the dirty box.
    if (dst.size() != size_t(bw) * size_t(bh)) dst.assign(size_t(bw) * size_t(bh), 0u);
    else std::fill(dst.begin(), dst.end(), 0u);
    if (sm.dirtyX1 < sm.dirtyX0 || sm.dirtyY1 < sm.dirtyY0) return;   // wrote nothing

    // Block range to rebuild. A block at (bx, by) reads the 9x9 apron
    // [bx*8, bx*8+8] x [by*8, by*8+8], so a block one column/row BEFORE the
    // dirty box still sees into it — hence the -1 on the low side. Anything
    // further out reads only cleared texels and stays 0.
    int bx0 = (sm.dirtyX0 >> kShadowUniShift) - 1; if (bx0 < 0) bx0 = 0;
    int by0 = (sm.dirtyY0 >> kShadowUniShift) - 1; if (by0 < 0) by0 = 0;
    int bx1 = sm.dirtyX1 >> kShadowUniShift; if (bx1 > bw - 1) bx1 = bw - 1;
    int by1 = sm.dirtyY1 >> kShadowUniShift; if (by1 > bh - 1) by1 = bh - 1;

    // Structure: a per-texel-ROW pass that collapses each block column's
    // 9-texel run to one id, then a per-BLOCK pass that collapses 9 consecutive
    // rows of that. The alternative — scanning the 9x9 square per block —
    // re-reads the column shared by horizontally adjacent blocks and walks the
    // plane in a 9-row zig-zag per block row. This form touches every texel of
    // the dirty box once, front to back.
    const int y0px = by0 << kShadowUniShift;
    int y1px = (by1 << kShadowUniShift) + kShadowUniSize;   // inclusive apron end
    if (y1px > sm.yres - 1) y1px = sm.yres - 1;
    const int rowW = bx1 - bx0 + 1;
    static thread_local std::vector<uint32_t> sRow;
    const size_t need = size_t(rowW) * size_t(y1px - y0px + 1);
    if (sRow.size() < need) sRow.assign(need, 0u);
    uint32_t *rowUni = sRow.data();

    for (int y = y0px; y <= y1px; ++y) {
        const uint32_t *p = src.data() + size_t(y) * size_t(sm.xres);
        uint32_t *r = rowUni + size_t(y - y0px) * size_t(rowW);
        for (int bx = bx0; bx <= bx1; ++bx) {
            const int x0 = bx << kShadowUniShift;
            int x1 = x0 + kShadowUniSize;           // inclusive apron end
            if (x1 > sm.xres - 1) x1 = sm.xres - 1;
            const uint32_t first = p[x0] & 0xFFFF0000u;
            uint32_t diff = 0u;
            for (int x = x0 + 1; x <= x1; ++x) diff |= (p[x] & 0xFFFF0000u) ^ first;
            r[bx - bx0] = diff ? kShadowUniMixed : (first >> 16);
        }
    }
    for (int by = by0; by <= by1; ++by) {
        const int ry0 = (by << kShadowUniShift) - y0px;
        int ry1 = ry0 + kShadowUniSize;             // inclusive apron end
        if (ry1 > y1px - y0px) ry1 = y1px - y0px;
        uint32_t *d = dst.data() + size_t(by) * size_t(bw);
        for (int bx = bx0; bx <= bx1; ++bx) {
            uint32_t c = rowUni[size_t(ry0) * size_t(rowW) + size_t(bx - bx0)];
            for (int y = ry0 + 1; y <= ry1 && c != kShadowUniMixed; ++y) {
                const uint32_t v = rowUni[size_t(y) * size_t(rowW) + size_t(bx - bx0)];
                if (v != c) c = kShadowUniMixed;
            }
            d[bx] = c;
        }
    }
}

#if FDS_SHADOW_TAP_CENSUS
// Per-thread pyramid tap counters + a process-wide registry so the report
// covers every worker that ever took a tap, not just the reporting thread.
// Heap-allocated and deliberately never freed (same reasoning as
// ShadowScratchTLS in Shadows.cpp): the registry outlives the workers.
static std::mutex &pyrCensusMtx() { static std::mutex m; return m; }
static std::vector<ShadowPyrCensus*> &pyrCensusAll()
{
    static std::vector<ShadowPyrCensus*> v; return v;
}
ShadowPyrCensus &ShadowPyrCensusTLS()
{
    static thread_local ShadowPyrCensus *p = [] {
        auto *q = new ShadowPyrCensus();
        std::lock_guard<std::mutex> lk(pyrCensusMtx());
        pyrCensusAll().push_back(q);
        return q;
    }();
    return *p;
}
void ShadowPyrCensusTotals(ShadowPyrCensus &out)
{
    out = ShadowPyrCensus{};
    std::lock_guard<std::mutex> lk(pyrCensusMtx());
    for (const ShadowPyrCensus *c : pyrCensusAll()) {
        out.reached += c->reached; out.noPyr   += c->noPyr;
        out.fastLit += c->fastLit; out.fastOcc += c->fastOcc;
        out.mixed   += c->mixed;   out.dynOnly += c->dynOnly;
        out.spotReached += c->spotReached; out.spotNoPyr   += c->spotNoPyr;
        out.spotFastLit += c->spotFastLit; out.spotFastOcc += c->spotFastOcc;
        out.spotMixed   += c->spotMixed;
    }
}
#endif

void ShadowMap_ViewModeCycle()
{
    int cur = g_shadowViewMode.load(std::memory_order_relaxed);
    cur = (cur + 1) % 3;
    g_shadowViewMode.store(cur, std::memory_order_relaxed);
    const char *name = (cur == 0) ? "static"
                     : (cur == 1) ? "dynamic"
                                  : "combined (closest-by-depth)";
    std::fprintf(stderr, "[SHADOW-VIEW] mode = %s\n", name);
}

// Per-frame staleness tracker for the dynamic cube buffers. When on,
// computes an FNV-1a hash of every shadow map's dynamic polyIds each
// frame and reports either:
//   - "alive" indices whose hash changed since last frame, or
//   - "stale" indices whose hash didn't change (suggests dynamic bake
//     isn't reaching this map — the bug we're hunting).
// Verbose; throttled to once every 60 frames. Toggled by T key.
std::atomic<bool> g_shadowStalenessTrack{false};
void ShadowMap_StalenessToggle()
{
    const bool was = g_shadowStalenessTrack.exchange(
        !g_shadowStalenessTrack.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    std::fprintf(stderr, "[SHADOW-STALE] tracking %s\n", was ? "OFF" : "ON");
}

void ShadowMap_TickStalenessTracker()
{
    if (!g_shadowStalenessTrack.load(std::memory_order_relaxed)) return;
    static std::vector<uint64_t> sLastHash;
    static int sFrame = 0;
    ++sFrame;
    if (sLastHash.size() != g_shadowMaps.size()) {
        sLastHash.assign(g_shadowMaps.size(), 0);
    }
    bool report = (sFrame % 60 == 0);
    std::vector<int> stale;
    stale.reserve(g_shadowMaps.size());
    for (size_t i = 0; i < g_shadowMaps.size(); ++i) {
        const ShadowMap &sm = g_shadowMaps[i];
        uint64_t h = 0xcbf29ce484222325ull;
        for (uint32_t t : sm.packDyn) {
            h ^= uint8_t(ShadowTexId(t)); h *= 0x100000001b3ull;
        }
        if (report && h == sLastHash[i]) {
            stale.push_back(int(i));
        }
        sLastHash[i] = h;
    }
    if (report) {
        std::fprintf(stderr,
            "[SHADOW-STALE] frame=%d  %zu/%zu shadow maps have UNCHANGED "
            "dynamic polyId hash since last 60-frame check:\n",
            sFrame, stale.size(), g_shadowMaps.size());
        for (int i : stale) {
            const ShadowMap &sm = g_shadowMaps[i];
            const Omni *O = sm.omni;
            std::fprintf(stderr,
                "  [%d] cubeFace=%d  pos=(%g,%g,%g)  flags=0x%x\n",
                i, int(sm.cubeFace),
                O ? O->IPos.x : 0.0f, O ? O->IPos.y : 0.0f,
                O ? O->IPos.z : 0.0f, O ? unsigned(O->Flags) : 0u);
        }
    }
}

void ShadowMap_ViewCycleBack()
{
    // Mirror of ShadowMap_ViewCycle that steps backward through the
    // registered shadow maps. Bound to Shift+V in REV.CPP. Same
    // fullscreen-vs-thumbnail wrap semantics.
    const int n = int(g_shadowMaps.size());
    if (n == 0) {
        std::fprintf(stderr, "[SHADOW-VIEW] no shadow maps registered\n");
        g_shadowViewIdx = -1;
        return;
    }
    const bool fullscreen = g_shadowFullscreenView.load(std::memory_order_relaxed);
    if (fullscreen) {
        g_shadowViewIdx = (g_shadowViewIdx <= 0) ? (n - 1) : (g_shadowViewIdx - 1);
    } else {
        g_shadowViewIdx = (g_shadowViewIdx <= 0) ? (n - 1)
                                                 : (g_shadowViewIdx - 1);
    }
    // Forward to the existing logger so the SHADOW-VIEW line still fires
    // (decremented once already; the forward will print without further
    // change because the cycle index logic above already advanced).
    // Simplest: replicate the print stanza from ShadowMap_ViewCycle.
    if (g_shadowViewIdx >= 0 && g_shadowViewIdx < n) {
        const ShadowMap &sm = g_shadowMaps[g_shadowViewIdx];
        std::fprintf(stderr,
            "[SHADOW-VIEW] (back) %d / %d  cubeFace=%d\n",
            g_shadowViewIdx, n, int(sm.cubeFace));
    }
}

void ShadowMap_ViewCycle()
{
    // -1 -> 0 -> 1 -> ... -> N-1 -> -1.
    const int n = int(g_shadowMaps.size());
    if (n == 0) {
        std::fprintf(stderr, "[SHADOW-VIEW] no shadow maps registered\n");
        g_shadowViewIdx = -1;
        return;
    }
    // In full-screen mode (greets M), -1 (= "off") would clear the
    // viz; instead wrap 0..N-1. Outside full-screen mode keep the
    // -1 sentinel so V can hide the thumbnail.
    const bool fullscreen = g_shadowFullscreenView.load(std::memory_order_relaxed);
    if (fullscreen) {
        g_shadowViewIdx = (g_shadowViewIdx + 1) % n;
    } else {
        g_shadowViewIdx = (g_shadowViewIdx >= n - 1) ? -1 : g_shadowViewIdx + 1;
    }
    if (g_shadowViewIdx < 0) {
        std::fprintf(stderr, "[SHADOW-VIEW] off (cycled past last of %d)\n", n);
    } else {
        const ShadowMap &sm = g_shadowMaps[g_shadowViewIdx];
        const char *omniName = (sm.omni && sm.omni->Type == Light_Omni) ? "omni" :
                               (sm.omni && sm.omni->Type == Light_SpotLight) ? "spot" : "?";
        // Stats so we can see whether the actual data differs across maps
        // (vs an overlay rendering bug). Sample some unique polyIds + the
        // depth extent and non-empty pixel count. Reported for both
        // STATIC and DYNAMIC buffers so we can spot per-omni issues like
        // "dynamic buffer stuck at t=0" (one omni's dynamic count stays
        // constant while others change across frames).
        auto polyStats = [](const std::vector<uint32_t> &arr,
                            size_t &nonZeroOut, int &uniqOut,
                            uint64_t &hashOut) {
            // Switched to 16-bit polyId (Material::ShadowMatID widen).
            // Unique-ID estimate uses bottom 12 bits modulo 4096; close
            // enough for the diag.
            nonZeroOut = 0; uniqOut = 0; hashOut = 0xcbf29ce484222325ull;
            bool seen[4096] = {};
            for (uint32_t t : arr) {
                const uint16_t p = ShadowTexId(t);
                if (p) ++nonZeroOut;
                const int idx = p & 0xFFF;
                if (!seen[idx]) { seen[idx] = true; ++uniqOut; }
                hashOut ^= p; hashOut *= 0x100000001b3ull;
            }
        };
        auto depthStats = [](const std::vector<uint32_t> &arr,
                             size_t &nonZeroOut, uint16_t &dminOut,
                             uint16_t &dmaxOut, uint64_t &hashOut) {
            nonZeroOut = 0; dminOut = 0xFFFF; dmaxOut = 0;
            hashOut = 0xcbf29ce484222325ull;
            for (uint32_t t : arr) {
                const uint16_t d = ShadowTexZ(t);
                if (d) ++nonZeroOut;
                if (d < dminOut) dminOut = d;
                if (d > dmaxOut) dmaxOut = d;
                hashOut ^= d; hashOut *= 0x100000001b3ull;
            }
        };
        size_t nzPs = 0, nzPd = 0, nzZs = 0, nzZd = 0;
        int uPs = 0, uPd = 0;
        uint16_t dmin_s = 0, dmax_s = 0, dmin_d = 0, dmax_d = 0;
        uint64_t pHs = 0, pHd = 0, dHs = 0, dHd = 0;
        polyStats (sm.packSD,  nzPs, uPs, pHs);
        polyStats (sm.packDyn, nzPd, uPd, pHd);
        depthStats(sm.packSD,  nzZs, dmin_s, dmax_s, dHs);
        depthStats(sm.packDyn, nzZd, dmin_d, dmax_d, dHd);
        const size_t total = sm.packSD.size();
        std::fprintf(stderr,
            "[SHADOW-VIEW] %d / %d  %s  %dx%d  cubeFace=%d\n"
            "  STATIC : polyId %zu/%zu nz (%d uniq) h=%016llx | depth %zu nz [%u..%u] h=%016llx\n"
            "  DYNAMIC: polyId %zu/%zu nz (%d uniq) h=%016llx | depth %zu nz [%u..%u] h=%016llx\n",
            g_shadowViewIdx, n, omniName, sm.xres, sm.yres, int(sm.cubeFace),
            nzPs, total, uPs, (unsigned long long)pHs,
            nzZs, unsigned(dmin_s), unsigned(dmax_s), (unsigned long long)dHs,
            nzPd, total, uPd, (unsigned long long)pHd,
            nzZd, unsigned(dmin_d), unsigned(dmax_d), (unsigned long long)dHd);
        if (sm.omni) {
            std::fprintf(stderr,
                "              pos=(%g,%g,%g) IRange=%g  flags=0x%x\n",
                sm.omni->IPos.x, sm.omni->IPos.y, sm.omni->IPos.z,
                sm.omni->IRange, unsigned(sm.omni->Flags));
        }
    }
}

void ShadowMap_Overlay(byte *vpage, int xres, int yres, int pitchBytes)
{
    if (g_shadowViewIdx < 0) return;
    if (g_shadowViewIdx >= int(g_shadowMaps.size())) return;
    if (!vpage || xres <= 0 || yres <= 0 || pitchBytes <= 0) return;

    const ShadowMap &sm = g_shadowMaps[g_shadowViewIdx];
    if (sm.xres <= 0 || sm.yres <= 0 || sm.packSD.empty()) return;

    // Fullscreen mode (greets M-key sets g_shadowFullscreenView): paint
    // the shadow map across the whole framebuffer instead of a corner
    // thumbnail. Same content + mode (B-key) as the thumbnail, just
    // sized up. Letterbox the larger axis so the cube face stays square.
    const bool fullscreen = g_shadowFullscreenView.load(std::memory_order_relaxed);
    int dstW, dstH, ox, oy;
    if (fullscreen) {
        const int side = std::min(xres, yres);
        dstW = side;
        dstH = side;
        ox = (xres - side) / 2;
        oy = (yres - side) / 2;
    } else {
        int thumbSize = std::min(xres, yres) / 4;
        if (thumbSize < 64)   thumbSize = 64;
        if (thumbSize > sm.xres) thumbSize = sm.xres;
        if (thumbSize > 512)  thumbSize = 512;
        dstW = thumbSize;
        dstH = thumbSize;
        if (dstW + 4 >= xres || dstH + 4 >= yres) return;
        ox = 4;
        oy = 4;
    }

    // Row stride in dwords. SDL locked textures may pad scanlines past
    // xres*4 bytes — use the surface's BPSL (passed as pitchBytes).
    const ptrdiff_t pitchD = ptrdiff_t(pitchBytes) / 4;
    dword *out = reinterpret_cast<dword*>(vpage);

    // PolyId visualization: each unique polyId hashes to a unique
    // color. 0 = empty (no surface) → black so you can see the cube
    // face coverage instead of just a uniform splat.
    auto hashPolyColor = [](uint8_t pid) -> dword {
        if (pid == 0) return 0xFF000000u;
        uint32_t h = uint32_t(pid) * 0x9E3779B9u;
        h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16;
        const uint8_t r = uint8_t((h >> 16) & 0xFF);
        const uint8_t g = uint8_t((h >>  8) & 0xFF);
        const uint8_t b = uint8_t( h        & 0xFF);
        return 0xFF000000u | (dword(r) << 16) | (dword(g) << 8) | dword(b);
    };
    const int mode = g_shadowViewMode.load(std::memory_order_relaxed);
    for (int dy = 0; dy < dstH; ++dy) {
        const int sy = (dy * sm.yres) / dstH;
        const uint32_t *pS = &sm.packSD[size_t(sy) * size_t(sm.xres)];
        const uint32_t *pD = sm.packDyn.empty() ? nullptr
                            : &sm.packDyn[size_t(sy) * size_t(sm.xres)];
        dword *dstRow = out + ptrdiff_t(oy + dy) * pitchD + ptrdiff_t(ox);
        for (int dx = 0; dx < dstW; ++dx) {
            const int sx = (dx * sm.xres) / dstW;
            uint16_t pick;
            if (mode == 1) {
                pick = pD ? ShadowTexId(pD[sx]) : uint16_t(0);
            } else if (mode == 2) {
                const uint16_t s = ShadowTexId(pS[sx]);
                const uint16_t d = pD ? ShadowTexId(pD[sx]) : uint16_t(0);
                if (s == 0) pick = d;
                else if (d == 0) pick = s;
                else {
                    // Closest-by-depth (larger zEnc wins; matches the
                    // sampler's logic in ShadowMap.h CubeShadow_Sample).
                    const uint16_t zs = ShadowTexZ(pS[sx]);
                    const uint16_t zd = pD ? ShadowTexZ(pD[sx]) : uint16_t(0);
                    pick = (zd > zs) ? d : s;
                }
            } else {
                pick = ShadowTexId(pS[sx]);
            }
            // Hash the full 16-bit polyId into RGB for the viz.
            dstRow[dx] = hashPolyColor(uint8_t(pick ^ (pick >> 8)));
        }
    }
    // Mode-colored border, 3 px thick so the mode is obvious at a glance.
    // static=cyan, dynamic=magenta, combined=yellow. Wide enough to read
    // when the overlay is fullscreen letterboxed and the label below the
    // viz might fall off the bottom of the framebuffer.
    const dword borderC = (mode == 0) ? 0xFF00FFFFu   // cyan = static
                       : (mode == 1) ? 0xFFFF00FFu   // magenta = dynamic
                                     : 0xFFFFFF00u;  // yellow = combined
    constexpr int kBorderT = 3;
    for (int t = 1; t <= kBorderT; ++t) {
        for (int dx = -t; dx <= dstW + t - 1; ++dx) {
            const int xx = ox + dx;
            if (xx < 0 || xx >= xres) continue;
            if (oy - t >= 0)
                out[ptrdiff_t(oy - t) * pitchD + ptrdiff_t(xx)] = borderC;
            if (oy + dstH + t - 1 < yres)
                out[ptrdiff_t(oy + dstH + t - 1) * pitchD + ptrdiff_t(xx)] = borderC;
        }
        for (int dy = -t; dy <= dstH + t - 1; ++dy) {
            const int yy = oy + dy;
            if (yy < 0 || yy >= yres) continue;
            if (ox - t >= 0)
                out[ptrdiff_t(yy) * pitchD + ptrdiff_t(ox - t)] = borderC;
            if (ox + dstW + t - 1 < xres)
                out[ptrdiff_t(yy) * pitchD + ptrdiff_t(ox + dstW + t - 1)] = borderC;
        }
    }
    char buf[96];
    const char *modeName = (mode == 0) ? "STATIC"
                         : (mode == 1) ? "DYNAMIC"
                                       : "COMBINED";
    std::snprintf(buf, sizeof(buf), "[%s]  SM %d/%zu  %dx%d  face=%d",
                  modeName, g_shadowViewIdx, g_shadowMaps.size(),
                  sm.xres, sm.yres, int(sm.cubeFace));
    // Label both above (always on-screen if overlay fits) and below
    // (works for the small-thumbnail case where there's vertical room).
    const int labelYAbove = std::max(0, oy - kBorderT - 14);
    OutTextXY(vpage, ox + 4, labelYAbove, buf, 255, xres, yres);
    if (oy + dstH + kBorderT + 4 + 14 < yres) {
        OutTextXY(vpage, ox + 4, oy + dstH + kBorderT + 4, buf, 255, xres, yres);
    }
}

void ShadowMaps_Rebuild(Scene *Sc, int res)
{
	g_shadowMaps.clear();
	if (!Sc) return;
	// Shadow camera's far plane is a multiple of the light's IRange. The
	// light only LIGHTS within IRange, but geometry between IRange and
	// FZP must still be in the depth buffer because it can occlude lit
	// receivers near the cone edge. Multiplier is tunable via env.
	const float sFzpMult = fds::FeatureFlags::shadow_fzp_mult();
	for (Omni *O = Sc->OmniHead; O; O = O->Next) {
		if (!(O->Flags & Omni_CastsShadow)) continue;
		// Light_Omni shadow casters use cube shadow maps via
		// CubeShadowMaps_Rebuild — not a single 2D entry here.
		if (O->Type != Light_SpotLight) continue;
		// `{}` = VALUE-initialization. ShadowMap's two `Matrix` members are
		// raw float[3][3] with no NSDMI, so a bare `ShadowMap sm;` hands the
		// vector this stack frame in place of a light transform — see the
		// comment on ShadowMap::lightViewMat for what that cost. The header
		// now initialises them too; this is the belt to that brace, and it
		// also covers any member added later without an initializer.
		ShadowMap sm{};
		// Per-light resolution: Omni.shadowMapRes overrides the global
		// default. Lets short-range orbit lights use 256² (16× less
		// raster cost than 1024²) while keeping the main spot at 1024².
		const int lightRes = (O->shadowMapRes > 0) ? int(O->shadowMapRes) : res;
		sm.xres = lightRes;
		sm.yres = lightRes;
		const size_t n = size_t(lightRes) * size_t(lightRes);
		sm.packSD.assign(n, 0u);
		sm.packDyn.assign(n, 0u);
		// Uniformity pyramid, sized with the planes. All-zero is the CORRECT
		// content here, not a placeholder: both planes are all-zero, so every
		// block's apron is uniformly id 0 and a tap taken before the first bake
		// reads "no occluder" — exactly what the plane itself would have said.
		sm.uniW = (sm.xres + kShadowUniSize - 1) >> kShadowUniShift;
		sm.uniH = (sm.yres + kShadowUniSize - 1) >> kShadowUniShift;
		sm.uniSD.assign(size_t(sm.uniW) * size_t(sm.uniH), 0u);
		sm.uniDyn.assign(size_t(sm.uniW) * size_t(sm.uniH), 0u);
		sm.omni = O;
		// Camera basis + FOV / z-scale are computed each frame in
		// Render_DeferredShadowMaps from the omni's pose. zScale here
		// is initialized from the omni's range as a sane default but
		// recomputed per frame.
		sm.fzp    = O->IRange * sFzpMult;
		sm.rFZP   = 1.0f / sm.fzp;
		sm.zScale = float(0xFF00) / (sm.fzp * 1.1f);
		g_shadowMaps.push_back(std::move(sm));
	}
	std::fprintf(stderr, "[SHADOW] ShadowMaps_Rebuild: %zu shadow maps "
		"(R/G/B intensities from O->L):\n", g_shadowMaps.size());
	for (const auto& s : g_shadowMaps) {
		std::fprintf(stderr, "  res=%dx%d  R=%.0f G=%.0f B=%.0f  IRange=%.1f\n",
			s.xres, s.yres,
			s.omni ? s.omni->L.R : 0.0f,
			s.omni ? s.omni->L.G : 0.0f,
			s.omni ? s.omni->L.B : 0.0f,
			s.omni ? s.omni->IRange : 0.0f);
	}
	std::fflush(stderr);
}

// Cube shadow map setup for omnis. Each omni gets 6 ShadowMap entries
// appended to g_shadowMaps (treated as 90°-FOV "spotlights" for the
// existing render pass) plus a CubeShadowRef that groups them.
void CubeShadowMaps_Rebuild(Scene *Sc, int res)
{
	g_cubeShadowRefs.clear();
	if (!Sc) return;
	const float sFzpMult = fds::FeatureFlags::shadow_fzp_mult();
	for (Omni *O = Sc->OmniHead; O; O = O->Next) {
		if (!(O->Flags & Omni_CastsShadow)) continue;
		if (O->Type != Light_Omni) continue;  // spots handled by ShadowMaps_Rebuild

		CubeShadowRef ref;
		ref.omni = O;
		ref.lightISource = O->IPos;
		const int faceRes = (O->shadowMapRes > 0) ? int(O->shadowMapRes) : res;

		for (int f = 0; f < 6; ++f) {
			ShadowMap sm{};   // value-init: see ShadowMaps_Rebuild above
			sm.xres = faceRes;
			sm.yres = faceRes;
			const size_t n = size_t(faceRes) * size_t(faceRes);
			sm.packSD.assign(n, 0u);
			sm.packDyn.assign(n, 0u);
			sm.uniW = (sm.xres + kShadowUniSize - 1) >> kShadowUniShift;
			sm.uniH = (sm.yres + kShadowUniSize - 1) >> kShadowUniShift;
			sm.uniSD.assign(size_t(sm.uniW) * size_t(sm.uniH), 0u);
			sm.uniDyn.assign(size_t(sm.uniW) * size_t(sm.uniH), 0u);
			sm.omni = O;  // shared across all 6 faces
			sm.cubeFace = int8_t(f);  // tells render pass which axis to face
			sm.fzp    = O->IRange * sFzpMult;
			sm.rFZP   = 1.0f / sm.fzp;
			sm.zScale = float(0xFF00) / (sm.fzp * 1.1f);
			ref.faceIdx[f] = int32_t(g_shadowMaps.size());
			g_shadowMaps.push_back(std::move(sm));
		}
		g_cubeShadowRefs.push_back(std::move(ref));
	}
	if (!g_cubeShadowRefs.empty()) {
		std::fprintf(stderr, "[SHADOW] CubeShadowMaps_Rebuild: %zu cube maps "
			"(6 faces each):\n", g_cubeShadowRefs.size());
		for (const auto& cr : g_cubeShadowRefs) {
			std::fprintf(stderr, "  res=%dx%d  R=%.0f G=%.0f B=%.0f  IRange=%.1f\n",
				g_shadowMaps[cr.faceIdx[0]].xres,
				g_shadowMaps[cr.faceIdx[0]].yres,
				cr.omni->L.R, cr.omni->L.G, cr.omni->L.B,
				cr.omni->IRange);
		}
		std::fflush(stderr);
	}
}

// Forward decl — defined in RENDER/Shadows.cpp.
void Render_DeferredShadowMaps(Scene *Sc, ShadowBakeMode mode, bool forceEnable);

void ShadowMaps_BakeStatic(Scene *Sc, bool forceEnable)
{
	// One-shot: render shadow maps for Omni_StaticShadow lights. After
	// this returns, Render_DeferredShadowMaps's per-frame skip filter
	// avoids re-rendering them. Intended to be called from scene init,
	// hiding inside the existing init bake window (city's Glato cube
	// bake, etc.) so the demo start time is unaffected.
	//
	// forceEnable bypasses the global FeatureFlags::shadows() gate for this
	// one static bake, so a scene that only turns --shadows on at RUN time
	// (greets) still fills its static occluder maps here at INIT — the
	// force-enabled static-shadow lightmap bake that runs right after reads
	// them. Without this the maps stay empty and the lightmap bakes 100% lit.
	Render_DeferredShadowMaps(Sc, ShadowBakeMode::StaticOnce, forceEnable);
	int n = 0;
	for (Omni *O = Sc ? Sc->OmniHead : nullptr; O; O = O->Next) {
		if ((O->Flags & Omni_CastsShadow) && (O->Flags & Omni_StaticShadow)) ++n;
	}
	std::fprintf(stderr, "[SHADOW] ShadowMaps_BakeStatic: %d static light(s) baked\n", n);
}

// ShadowBarry: tile-based AVX2 depth+polyId rasterizer modeled on
// TheOtherBarry. Walks 8×8 tiles via super-tile (4 tiles per side =
// 32px) hierarchical coverage culling. Per-tile fast path skips edge
// mask construction when the tile is fully inside the triangle.
//
// Output is one PACKED uint32 per texel (z | ShadowMatID<<16) — none of
// TheOtherBarry's UV / texture / color / specular machinery. Constructor
// is no-Txtr.
//
// Encoded Z: `enc = 0xFF80 - round(z * zScale)`. Higher enc = closer
// to light. The id half is matID+1 of the writing face; because it lives
// in the SAME word it is written by the same masked store as the z, so the
// closest-occluder id wins with no second array and no second store.
struct ShadowBarry {
	ShadowMap *sm;
	uint32_t *pArr;   // sm->packSD or sm->packDyn
	// Static-plane cull source: non-null ONLY when we're writing the
	// dynamic plane (so apply_exact can mask out lanes already occluded by
	// closer static geometry). The runtime cube tap's closestPacked() picks
	// the plane with larger zEnc anyway, so any dynamic write where the
	// static plane is already closer would be ignored at sample time —
	// we skip the write here to save the RMW.
	const uint32_t *pStaticArr;
	float drzdx, drzdy;
	uint16_t idByte;  // legacy name; now a 16-bit ShadowMatID
	bool g_useFullStore;  // cached once per ShadowBarry, read per row inside apply_exact.

	ShadowBarry(ShadowMap *smIn, uint16_t idIn, bool useDynamic)
		: sm(smIn),
		  pArr(useDynamic ? smIn->packDyn.data() : smIn->packSD.data()),
		  pStaticArr(useDynamic ? smIn->packSD.data() : nullptr),
		  drzdx(0), drzdy(0), idByte(idIn),
		  g_useFullStore(fds::FeatureFlags::rast_full_store()) {}

	template <barry::TCoverage Coverage = barry::TCoverage::PARTIAL>
	void apply_exact(const barry::Tile& tile) {
		const int xres = sm->xres;
		uint32_t * const pRowBase = pArr
			+ size_t(tile.y) * barry::TILE_SIZE * size_t(xres)
			+ size_t(tile.x) * barry::TILE_SIZE;

		barry::TScreenCoord a0 = tile.a0, b0 = tile.b0, c0 = tile.c0;

		// Edge function lanes only needed for PARTIAL.
		Vec8i p_a, p_b, p_c;
		if constexpr (Coverage == barry::TCoverage::PARTIAL) {
			p_a = v8_from_arith_seq(a0, tile.dadx);
			p_b = v8_from_arith_seq(b0, tile.dbdx);
			p_c = v8_from_arith_seq(c0, tile.dcdx);
		}

		Vec8f p_rz = v8_from_arith_seq(tile.rz0, drzdx);
		const float zScale = sm->zScale;
		const Vec8f vZScale(zScale);

		uint32_t *pRow = pRowBase;
		const uint32_t * const pStaticRowBase = pStaticArr
		    ? (pStaticArr + (pRowBase - pArr)) : nullptr;
		const uint32_t *pStaticRow = pStaticRowBase;
		for (int row = 0; row < barry::TILE_SIZE; ++row,
				pRow += xres,
				pStaticRow = pStaticRow ? (pStaticRow + xres) : nullptr) {
			Vec8ib p_mask;
			bool row_has_pixels;
			if constexpr (Coverage == barry::TCoverage::FULL) {
				p_mask = Vec8i(0) == Vec8i(0);
				row_has_pixels = true;
			} else {
				p_mask = (p_a | p_b | p_c) >= 0;
				row_has_pixels = barry::any_lane_set(p_mask);
			}
			if (row_has_pixels) {
				const Vec8f p_z = approx_recipr(p_rz);
				Vec8i enc = Vec8i(0xFF80) - roundi(p_z * vZScale);
				enc = max(enc, Vec8i(0));
				enc = min(enc, Vec8i(0xFFFF));

				// ONE 32-bit load per texel yields the existing z AND the
				// existing id; the z half is the low 16 bits.
				Vec8ui p_existing;
				p_existing.load(pRow);
				const Vec8i z_existing = Vec8i(p_existing & Vec8ui(0xFFFFu));
				p_mask &= Vec8ib(enc > z_existing);

				// Static-z cull: when writing the dynamic plane, mask
				// off lanes where the STATIC plane already has a closer
				// occluder (zs > enc). closestPacked() at sample time would
				// pick the static plane anyway, so writing them here is
				// a wasted RMW. pStaticArr non-null only on the dynamic
				// write path.
				if (pStaticArr) {
					Vec8ui ps_existing;
					ps_existing.load(pStaticRow);
					const Vec8i zs_existing = Vec8i(ps_existing & Vec8ui(0xFFFFu));
					p_mask &= Vec8ib(enc > zs_existing);
				}

				if (barry::any_lane_set(p_mask)) {
					// The new word: the freshly-encoded z, plus this face's
					// ShadowMatID in the high half. A face with no material
					// (idByte == 0) writes z only and PRESERVES the id half —
					// the historic behaviour when they were two arrays and
					// the polyId store was gated on `if (idByte)`.
					const Vec8ui p_new = idByte
						? (Vec8ui(enc) | Vec8ui(uint32_t(idByte) << 16))
						: (Vec8ui(enc) | (p_existing & Vec8ui(0xFFFF0000u)));
					// FULL row: when all 8 lanes survived edge+Z+static-Z,
					// the masked select is wasted (it overwrites every lane
					// anyway). Mirrors Mekalele's FULL store optimization at
					// TileRasterizer::apply_exact. Gated by FDS_RAST_FULL_STORE
					// since the optimization spans rasterizers.
					if (g_useFullStore && barry::all_lanes_set(p_mask)) {
						p_new.store(pRow);
					} else {
						select(Vec8ib(p_mask), p_new, p_existing).store(pRow);
					}
				}
			}
			if constexpr (Coverage == barry::TCoverage::PARTIAL) {
				p_a += tile.dady;
				p_b += tile.dbdy;
				p_c += tile.dcdy;
			}
			p_rz += Vec8f(drzdy);
		}
	}

	void rasterize_triangle(const Vertex& v1, const Vertex& v2, const Vertex& v3) {
		// AABB in tile coords, clamped to shadow-map dimensions.
		using barry::TILE_SIZE;
		using barry::SUBPIXEL_BITS;
		using barry::SUBPIXEL_MULT;
		using barry::TScreenCoord;
		using barry::orient2d;

		const int xres = sm->xres;
		const int yres = sm->yres;
		auto clampX = [xres](int v) { return std::min(std::max(v, 0), xres - 1); };
		auto clampY = [yres](int v) { return std::min(std::max(v, 0), yres - 1); };

		// Clamp to the OWNING clipper tile's range — see ClipperTileRect.h.
		// Without this, two adjacent clipper workers can both rasterize the
		// same 8x8 SIMD tile when a clipped vertex lands exactly on the
		// shared tile boundary -> blendv RMW race in apply_exact.
		const fds::ClipperTileRect& _ctr = fds::g_clipperTileRect;
		const int tile_mx = std::max(_ctr.tile_mx_lo,
			clampX(int(std::min({v1.PX, v2.PX, v3.PX}))) / TILE_SIZE);
		const int tile_Mx = std::min(_ctr.tile_mx_hi,
			clampX(int(std::max({v1.PX, v2.PX, v3.PX}))) / TILE_SIZE);
		const int tile_my = std::max(_ctr.tile_my_lo,
			clampY(int(std::min({v1.PY, v2.PY, v3.PY}))) / TILE_SIZE);
		const int tile_My = std::min(_ctr.tile_my_hi,
			clampY(int(std::max({v1.PY, v2.PY, v3.PY}))) / TILE_SIZE);
		if (tile_mx > tile_Mx || tile_my > tile_My) return;

		// Subpixel-precise vertex coords.
		const TScreenCoord v1x = TScreenCoord(std::lroundf(v1.PX * SUBPIXEL_MULT));
		const TScreenCoord v1y = TScreenCoord(std::lroundf(v1.PY * SUBPIXEL_MULT));
		const TScreenCoord v2x = TScreenCoord(std::lroundf(v2.PX * SUBPIXEL_MULT));
		const TScreenCoord v2y = TScreenCoord(std::lroundf(v2.PY * SUBPIXEL_MULT));
		const TScreenCoord v3x = TScreenCoord(std::lroundf(v3.PX * SUBPIXEL_MULT));
		const TScreenCoord v3y = TScreenCoord(std::lroundf(v3.PY * SUBPIXEL_MULT));

		const TScreenCoord x0 = tile_mx * TILE_SIZE << SUBPIXEL_BITS;
		const TScreenCoord y0 = tile_my * TILE_SIZE << SUBPIXEL_BITS;
		TScreenCoord _a0 = orient2d(v2x, v2y, v1x, v1y, x0, y0);
		TScreenCoord _b0 = orient2d(v3x, v3y, v2x, v2y, x0, y0);
		TScreenCoord _c0 = orient2d(v1x, v1y, v3x, v3y, x0, y0);

		const TScreenCoord dadx = (v2y - v1y);
		const TScreenCoord dady = (v1x - v2x);
		const TScreenCoord dbdx = (v3y - v2y);
		const TScreenCoord dbdy = (v2x - v3x);
		const TScreenCoord dcdx = (v1y - v3y);
		const TScreenCoord dcdy = (v3x - v1x);

		// Hierarchical traversal gating — small triangles take the direct
		// per-tile loop; big triangles use the 4×4-tile super grid.
		constexpr int SUPER = 4;
		constexpr int SUPER_PIXELS = SUPER * TILE_SIZE;
		const int super_mx = tile_mx / SUPER;
		const int super_Mx = tile_Mx / SUPER;
		const int super_my = tile_my / SUPER;
		const int super_My = tile_My / SUPER;
		const bool spans_multi_super = (super_mx != super_Mx) || (super_my != super_My);

		auto build_tile = [&](int x, int y, TScreenCoord a, TScreenCoord b, TScreenCoord c) {
			barry::Tile tile = {};
			tile.x = x;
			tile.y = y;
			tile.a0 = a;
			tile.dadx = dadx; tile.dady = dady;
			tile.b0 = b;
			tile.dbdx = dbdx; tile.dbdy = dbdy;
			tile.c0 = c;
			tile.dcdx = dcdx; tile.dcdy = dcdy;
			tile.rz0 = v1.RZ + (x * TILE_SIZE - v1.PX) * drzdx
			                  + (y * TILE_SIZE - v1.PY) * drzdy;
			return tile;
		};

		if (!spans_multi_super) {
			for (int y = tile_my; y <= tile_My; ++y,
					_a0 += TILE_SIZE * dady, _b0 += TILE_SIZE * dbdy, _c0 += TILE_SIZE * dcdy) {
				TScreenCoord a0 = _a0, b0 = _b0, c0 = _c0;
				for (int x = tile_mx; x <= tile_Mx; ++x,
						a0 += TILE_SIZE * dadx, b0 += TILE_SIZE * dbdx, c0 += TILE_SIZE * dcdx) {
					const TScreenCoord max_a = a0 + ((dadx > 0) ? dadx * TILE_SIZE : 0) + ((dady > 0) ? dady * TILE_SIZE : 0);
					const TScreenCoord max_b = b0 + ((dbdx > 0) ? dbdx * TILE_SIZE : 0) + ((dbdy > 0) ? dbdy * TILE_SIZE : 0);
					const TScreenCoord max_c = c0 + ((dcdx > 0) ? dcdx * TILE_SIZE : 0) + ((dcdy > 0) ? dcdy * TILE_SIZE : 0);
					if ((max_a | max_b | max_c) < 0) continue;

					const TScreenCoord min_a = a0 + ((dadx < 0) ? dadx * TILE_SIZE : 0) + ((dady < 0) ? dady * TILE_SIZE : 0);
					const TScreenCoord min_b = b0 + ((dbdx < 0) ? dbdx * TILE_SIZE : 0) + ((dbdy < 0) ? dbdy * TILE_SIZE : 0);
					const TScreenCoord min_c = c0 + ((dcdx < 0) ? dcdx * TILE_SIZE : 0) + ((dcdy < 0) ? dcdy * TILE_SIZE : 0);
					const bool full_cover = (min_a >= 0) && (min_b >= 0) && (min_c >= 0);

					auto tile = build_tile(x, y, a0, b0, c0);
					if (full_cover) {
						apply_exact<barry::TCoverage::FULL>(tile);
					} else {
						apply_exact<barry::TCoverage::PARTIAL>(tile);
					}
				}
			}
			return;
		}

		// Super-tile path for large triangles.
		const TScreenCoord ssx0 = super_mx * SUPER_PIXELS << SUBPIXEL_BITS;
		const TScreenCoord ssy0 = super_my * SUPER_PIXELS << SUBPIXEL_BITS;
		TScreenCoord _sa0 = orient2d(v2x, v2y, v1x, v1y, ssx0, ssy0);
		TScreenCoord _sb0 = orient2d(v3x, v3y, v2x, v2y, ssx0, ssy0);
		TScreenCoord _sc0 = orient2d(v1x, v1y, v3x, v3y, ssx0, ssy0);

		for (int sy = super_my; sy <= super_My; ++sy,
				_sa0 += SUPER_PIXELS * dady, _sb0 += SUPER_PIXELS * dbdy, _sc0 += SUPER_PIXELS * dcdy) {
			TScreenCoord sa0 = _sa0, sb0 = _sb0, sc0 = _sc0;
			for (int sx = super_mx; sx <= super_Mx; ++sx,
					sa0 += SUPER_PIXELS * dadx, sb0 += SUPER_PIXELS * dbdx, sc0 += SUPER_PIXELS * dcdx) {
				const TScreenCoord smax_a = sa0 + ((dadx > 0) ? dadx * SUPER_PIXELS : 0) + ((dady > 0) ? dady * SUPER_PIXELS : 0);
				const TScreenCoord smax_b = sb0 + ((dbdx > 0) ? dbdx * SUPER_PIXELS : 0) + ((dbdy > 0) ? dbdy * SUPER_PIXELS : 0);
				const TScreenCoord smax_c = sc0 + ((dcdx > 0) ? dcdx * SUPER_PIXELS : 0) + ((dcdy > 0) ? dcdy * SUPER_PIXELS : 0);
				if ((smax_a | smax_b | smax_c) < 0) continue;

				const TScreenCoord smin_a = sa0 + ((dadx < 0) ? dadx * SUPER_PIXELS : 0) + ((dady < 0) ? dady * SUPER_PIXELS : 0);
				const TScreenCoord smin_b = sb0 + ((dbdx < 0) ? dbdx * SUPER_PIXELS : 0) + ((dbdy < 0) ? dbdy * SUPER_PIXELS : 0);
				const TScreenCoord smin_c = sc0 + ((dcdx < 0) ? dcdx * SUPER_PIXELS : 0) + ((dcdy < 0) ? dcdy * SUPER_PIXELS : 0);
				const bool super_full = (smin_a >= 0) && (smin_b >= 0) && (smin_c >= 0);

				const int ty_start = std::max(sy * SUPER, tile_my);
				const int ty_end   = std::min(sy * SUPER + SUPER - 1, tile_My);
				const int tx_start = std::max(sx * SUPER, tile_mx);
				const int tx_end   = std::min(sx * SUPER + SUPER - 1, tile_Mx);

				if (super_full) {
					// Every tile in this super-tile is FULL — skip per-tile
					// edge tests, dispatch apply_exact<FULL> directly.
					for (int y = ty_start; y <= ty_end; ++y) {
						for (int x = tx_start; x <= tx_end; ++x) {
							auto tile = build_tile(x, y, 0, 0, 0);
							apply_exact<barry::TCoverage::FULL>(tile);
						}
					}
					continue;
				}

				TScreenCoord _ta0 = sa0 + (tx_start - sx * SUPER) * TILE_SIZE * dadx
				                       + (ty_start - sy * SUPER) * TILE_SIZE * dady;
				TScreenCoord _tb0 = sb0 + (tx_start - sx * SUPER) * TILE_SIZE * dbdx
				                       + (ty_start - sy * SUPER) * TILE_SIZE * dbdy;
				TScreenCoord _tc0 = sc0 + (tx_start - sx * SUPER) * TILE_SIZE * dcdx
				                       + (ty_start - sy * SUPER) * TILE_SIZE * dcdy;

				for (int y = ty_start; y <= ty_end; ++y,
						_ta0 += TILE_SIZE * dady, _tb0 += TILE_SIZE * dbdy, _tc0 += TILE_SIZE * dcdy) {
					TScreenCoord a0 = _ta0, b0 = _tb0, c0 = _tc0;
					for (int x = tx_start; x <= tx_end; ++x,
							a0 += TILE_SIZE * dadx, b0 += TILE_SIZE * dbdx, c0 += TILE_SIZE * dcdx) {
						const TScreenCoord max_a = a0 + ((dadx > 0) ? dadx * TILE_SIZE : 0) + ((dady > 0) ? dady * TILE_SIZE : 0);
						const TScreenCoord max_b = b0 + ((dbdx > 0) ? dbdx * TILE_SIZE : 0) + ((dbdy > 0) ? dbdy * TILE_SIZE : 0);
						const TScreenCoord max_c = c0 + ((dcdx > 0) ? dcdx * TILE_SIZE : 0) + ((dcdy > 0) ? dcdy * TILE_SIZE : 0);
						if ((max_a | max_b | max_c) < 0) continue;

						const TScreenCoord min_a = a0 + ((dadx < 0) ? dadx * TILE_SIZE : 0) + ((dady < 0) ? dady * TILE_SIZE : 0);
						const TScreenCoord min_b = b0 + ((dbdx < 0) ? dbdx * TILE_SIZE : 0) + ((dbdy < 0) ? dbdy * TILE_SIZE : 0);
						const TScreenCoord min_c = c0 + ((dcdx < 0) ? dcdx * TILE_SIZE : 0) + ((dcdy < 0) ? dcdy * TILE_SIZE : 0);
						const bool full_cover = (min_a >= 0) && (min_b >= 0) && (min_c >= 0);

						auto tile = build_tile(x, y, a0, b0, c0);
						if (full_cover) {
							apply_exact<barry::TCoverage::FULL>(tile);
						} else {
							apply_exact<barry::TCoverage::PARTIAL>(tile);
						}
					}
				}
			}
		}
	}
};

static void rasterize_depth_tri(const Vertex& v0, const Vertex& v1, const Vertex& v2,
                                 ShadowMap& sm,
                                 uint16_t idOverride = 0,
                                 bool useDynamic = false)
{
	const float x0 = v0.PX, y0 = v0.PY;
	const float x1 = v1.PX, y1 = v1.PY;
	const float x2 = v2.PX, y2 = v2.PY;

	const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
	if (std::fabs(area) < 1e-4f) return;
	const float invArea = 1.0f / area;

	float xmin = std::min(std::min(x0, x1), x2);
	float xmax = std::max(std::max(x0, x1), x2);
	float ymin = std::min(std::min(y0, y1), y2);
	float ymax = std::max(std::max(y0, y1), y2);
	if (xmin < 0.0f) xmin = 0.0f;
	if (ymin < 0.0f) ymin = 0.0f;
	if (xmax > float(sm.xres - 1)) xmax = float(sm.xres - 1);
	if (ymax > float(sm.yres - 1)) ymax = float(sm.yres - 1);

	const int ixmin = int(std::floor(xmin));
	const int ixmax = int(std::ceil (xmax));
	const int iymin = int(std::floor(ymin));
	const int iymax = int(std::ceil (ymax));
	if (ixmin > ixmax || iymin > iymax) return;

	const float rz0 = v0.RZ;
	const float rz1 = v1.RZ;
	const float rz2 = v2.RZ;
	const float zScale = sm.zScale;

	// Bary partial derivatives wrt screen X (constant across the tri).
	// Expanding w0 = ((x1-px)(y2-py) - (x2-px)(y1-py)) * invArea gives
	//   ∂w0/∂px = (y1 - y2) * invArea
	//   ∂w1/∂px = (y2 - y0) * invArea
	//   ∂w2/∂px = -∂w0/∂px - ∂w1/∂px  (since w0+w1+w2 ≡ 1)
	const float dw0dx = (y1 - y2) * invArea;
	const float dw1dx = (y2 - y0) * invArea;
	const float drzdx = dw0dx * rz0 + dw1dx * rz1 + (-dw0dx - dw1dx) * rz2;

	// 8-lane offsets [0,1,2,…,7]; used to broadcast (base + i*dx) per row.
	const Vec8f laneOffsets(0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f);
	const Vec8f vDw0dx(dw0dx), vDw1dx(dw1dx), vDrzdx(drzdx);
	const Vec8f vDw0dx8 = vDw0dx * 8.0f;
	const Vec8f vDw1dx8 = vDw1dx * 8.0f;
	const Vec8f vDrzdx8 = vDrzdx * 8.0f;
	const Vec8f vZScale(zScale);
	const uint16_t idByte = idOverride;  // 16-bit ShadowMatID now

	uint32_t * const pBase = useDynamic ? sm.packDyn.data() : sm.packSD.data();
	for (int y = iymin; y <= iymax; ++y) {
		uint32_t *pRow = pBase + size_t(y) * size_t(sm.xres);
		const float py = float(y) + 0.5f;
		const float px0 = float(ixmin) + 0.5f;
		const float w0Row = ((x1 - px0) * (y2 - py) - (x2 - px0) * (y1 - py)) * invArea;
		const float w1Row = ((x2 - px0) * (y0 - py) - (x0 - px0) * (y2 - py)) * invArea;
		const float rzRow = w0Row * rz0 + w1Row * rz1 + (1.0f - w0Row - w1Row) * rz2;

		Vec8f vW0 = Vec8f(w0Row) + laneOffsets * vDw0dx;
		Vec8f vW1 = Vec8f(w1Row) + laneOffsets * vDw1dx;
		Vec8f vRz = Vec8f(rzRow) + laneOffsets * vDrzdx;

		int x = ixmin;
		// SIMD body: process [x, x+7] in chunks of 8. Stop early so the
		// 8-uint16 load doesn't read past row end into the next row's
		// memory (write-back through blendv would corrupt it).
		const int xSimdEnd = ixmax - 7;
		for (; x <= xSimdEnd; x += 8) {
			const Vec8f vW2 = Vec8f(1.0f) - vW0 - vW1;
			Vec8fb bary = (vW0 >= 0.0f) & (vW1 >= 0.0f) & (vW2 >= 0.0f);
			if (horizontal_or(bary)) {
				const Vec8f vZ = 1.0f / vRz;
				Vec8i enc = Vec8i(0xFF80) - roundi(vZ * vZScale);
				// Clamp to [0, 0xFFFF].
				enc = max(enc, Vec8i(0));
				enc = min(enc, Vec8i(0xFFFF));

				// Load 8 existing packed words; the z half is the low 16.
				Vec8ui p_existing;
				p_existing.load(pRow + x);
				const Vec8i existing = Vec8i(p_existing & Vec8ui(0xFFFFu));
				Vec8ib pass = enc > existing;
				pass &= Vec8ib(bary);
				if (horizontal_or(pass)) {
					// One masked 32-bit select writes z AND id together.
					// idByte == 0 (material-less face) preserves the id half.
					const Vec8ui p_new = idByte
						? (Vec8ui(enc) | Vec8ui(uint32_t(idByte) << 16))
						: (Vec8ui(enc) | (p_existing & Vec8ui(0xFFFF0000u)));
					select(pass, p_new, p_existing).store(pRow + x);
				}
			}
			vW0 += vDw0dx8;
			vW1 += vDw1dx8;
			vRz += vDrzdx8;
		}
		// Scalar tail for the last (ixmax - x + 1) pixels.
		float w0Tail = w0Row + float(x - ixmin) * dw0dx;
		float w1Tail = w1Row + float(x - ixmin) * dw1dx;
		float rzTail = rzRow + float(x - ixmin) * drzdx;
		for (; x <= ixmax; ++x) {
			const float w2 = 1.0f - w0Tail - w1Tail;
			if (w0Tail >= 0.0f && w1Tail >= 0.0f && w2 >= 0.0f) {
				if (rzTail > 0.0f) {
					int enc = 0xFF80 - int((1.0f / rzTail) * zScale);
					if (enc < 0) enc = 0;
					if (enc > 0xFFFF) enc = 0xFFFF;
					const uint16_t cand = uint16_t(enc);
					if (cand > ShadowTexZ(pRow[x])) {
						pRow[x] = idByte
							? ShadowTexPack(cand, idByte)
							: ShadowTexPack(cand, ShadowTexId(pRow[x]));
					}
				}
			}
			w0Tail += dw0dx;
			w1Tail += dw1dx;
			rzTail += drzdx;
		}
	}
}

void MekaleleShadowDepth(Face *F, Vertex** V, dword numVerts, dword /*miplevel*/,
                          const fds::RenderTarget& /*rt*/,
                          const fds::CameraContext& /*cam*/)
{
	ShadowMap *sm = g_currentShadowMap;
	if (!sm) return;
	if (numVerts < 3) return;

	// Capture clipper outputs that aren't within the input triangle's
	// convex hull, to find clipper edge cases the near-skip in the
	// orchestrator missed. Capped to first 8 events.
	const bool sValidate = fds::FeatureFlags::shadow_validate();
	if (sValidate && F) {
		static std::atomic<int> sLogged{0};
		// SoA Phase 4: read via F->frame (dev-only validation path).
		const VertexFrame *ff = F->frame;
		const uint32_t ai = F->A_idx, bi = F->B_idx, ci = F->C_idx;
		const float Ax = ff->PX[ai], Ay = ff->PY[ai];
		const float Bx = ff->PX[bi], By = ff->PY[bi];
		const float Cx = ff->PX[ci], Cy = ff->PY[ci];
		const float denom = (Ax - Cx) * (By - Cy) - (Bx - Cx) * (Ay - Cy);
		// Skip degenerate inputs: |denom| < 10 means triangle area < 5
		// pixels² in 2D — bary math gives wild values from float noise.
		if (std::fabs(denom) >= 10.0f) {
			const float invDen = 1.0f / denom;
			const float slack = 0.05f;
			bool bad = false;
			for (dword i = 0; i < numVerts && !bad; ++i) {
				const float Ox = V[i]->PX, Oy = V[i]->PY;
				if (!std::isfinite(Ox) || !std::isfinite(Oy)) { bad = true; break; }
				const float a = ((Ox - Cx) * (By - Cy) - (Bx - Cx) * (Oy - Cy)) * invDen;
				const float b = ((Ax - Cx) * (Oy - Cy) - (Ox - Cx) * (Ay - Cy)) * invDen;
				const float c = 1.0f - a - b;
				if (a < -slack || b < -slack || c < -slack ||
				    a > 1.0f + slack || b > 1.0f + slack || c > 1.0f + slack) {
					bad = true;
				}
			}
			if (bad && sLogged.fetch_add(1) < 8) {
				std::fprintf(stderr,
					"[SHADOW-CLIP] t=%d frame=%.2f n=%u Face=%p  "
					"Az=%.3g Bz=%.3g Cz=%.3g  "
					"in: A(%.1f,%.1f)F%x B(%.1f,%.1f)F%x C(%.1f,%.1f)F%x  out:",
					int(Timer.load()), CurFrame, numVerts, (void*)F,
					ff->TPos_z[ai], ff->TPos_z[bi], ff->TPos_z[ci],
					Ax, Ay, (unsigned)ff->Flags[ai],
					Bx, By, (unsigned)ff->Flags[bi],
					Cx, Cy, (unsigned)ff->Flags[ci]);
				for (dword i = 0; i < numVerts; ++i) {
					std::fprintf(stderr, " (%.1f,%.1f)",
						V[i]->PX, V[i]->PY);
				}
				std::fprintf(stderr, "\n");
			}
		}
	}

	// Always write the face's material ID (+1 so the 0-sentinel
	// "unassigned" stays distinct from matID=0) into the shadow
	// texel's id half, regardless of render mode. The
	// lighting kernel decides whether to USE it (PolyId mode) or
	// ignore it (Depth mode) via g_shadowMode. Unconditional write
	// lets the M-key viz read polyId even while rendering in Depth.
	// Resolve the 16-bit ShadowMatID stamp this face writes into
	// the packed word's id half. Priority (high to low):
	//   1. Material::ShadowMatID — scene-init group override (e.g.
	//      greets's per-wall split assigns a unique ShadowMatID per
	//      coplanar cluster; hull-merge assigns one shared ShadowMatID
	//      to all hull/hull2 faces).
	//   2. Per-face F->ShadowMatID — 16-bit per-face override (used by
	//      greets wall split to give each coplanar cluster its own ID
	//      without inflating matTable past the 8-bit matID cap).
	//   3. Fallback: uint16_t(Txtr->ID + 1) — matches the historic
	//      matID-based polyId, with +1 so 0 stays as the unassigned
	//      sentinel.
	uint16_t idOverride;
	if (F && F->ShadowMatID != 0) {
		idOverride = F->ShadowMatID;
	} else if (F && F->Txtr && F->Txtr->ShadowMatID != 0) {
		idOverride = F->Txtr->ShadowMatID;
	} else {
		idOverride = (F && F->Txtr) ? uint16_t(F->Txtr->ID + 1) : 0;
	}

	// Triangulate the clipped n-gon as a fan from V[0] — same shape as
	// Mekalele's tri loop. Per-triangle: compute the RZ screen-space
	// gradient via the affine inverse of the (v2-v1, v3-v1) screen
	// matrix, then hand off to ShadowBarry which does tile-hierarchical
	// AVX2 rasterization.
	extern thread_local bool g_inDynamicShadowBake;
	const bool useDynamic = g_inDynamicShadowBake;
	// Dirty-box stamp for the uniformity pyramid. ONE bbox per clipped n-gon,
	// taken before the fan is triangulated — the fan triangles cover exactly
	// this polygon, so it is the same bound the per-triangle rasteriser would
	// have produced, for a fraction of the bookkeeping. Clamped to the plane
	// because that is where the rasteriser clamps its own span loop.
	{
		float bx0 = V[0]->PX, bx1 = V[0]->PX;
		float by0 = V[0]->PY, by1 = V[0]->PY;
		for (dword i = 1; i < numVerts; ++i) {
			const float px = V[i]->PX, py = V[i]->PY;
			if (px < bx0) bx0 = px; else if (px > bx1) bx1 = px;
			if (py < by0) by0 = py; else if (py > by1) by1 = py;
		}
		int ix0 = int(std::floor(bx0)); if (ix0 < 0) ix0 = 0;
		int iy0 = int(std::floor(by0)); if (iy0 < 0) iy0 = 0;
		int ix1 = int(std::ceil (bx1)); if (ix1 > sm->xres - 1) ix1 = sm->xres - 1;
		int iy1 = int(std::ceil (by1)); if (iy1 > sm->yres - 1) iy1 = sm->yres - 1;
		if (ix0 <= ix1 && iy0 <= iy1) {
			if (ix0 < g_shadowRasterBox[0]) g_shadowRasterBox[0] = ix0;
			if (iy0 < g_shadowRasterBox[1]) g_shadowRasterBox[1] = iy0;
			if (ix1 > g_shadowRasterBox[2]) g_shadowRasterBox[2] = ix1;
			if (iy1 > g_shadowRasterBox[3]) g_shadowRasterBox[3] = iy1;
		}
	}
	ShadowBarry r(sm, idOverride, useDynamic);
	for (dword i = 2; i < numVerts; ++i) {
		const Vertex& v1 = *V[0];
		const Vertex& v2 = *V[i - 1];
		const Vertex& v3 = *V[i];
		const float mxx = v2.PX - v1.PX, mxy = v2.PY - v1.PY;
		const float myx = v3.PX - v1.PX, myy = v3.PY - v1.PY;
		const float det = mxx * myy - mxy * myx;
		if (std::fabs(det) <= 0.01f) continue;  // degenerate
		const float invDet = 1.0f / det;
		const float imxx =  myy * invDet, imxy = -mxy * invDet;
		const float imyx = -myx * invDet, imyy =  mxx * invDet;
		r.drzdx = imxx * (v2.RZ - v1.RZ) + imxy * (v3.RZ - v1.RZ);
		r.drzdy = imyx * (v2.RZ - v1.RZ) + imyy * (v3.RZ - v1.RZ);
		r.rasterize_triangle(v1, v2, v3);
	}
}

// Extract the matID byte the lighting kernel reads from gb.txtr packs.
// Mekalele's packed format: miplevel(4) | matID(8) | swizzled_uv(20).
// Defined here so the kernel can compare against shadow buffer matIDs.
// matID maps to the +1-shifted value we wrote into the texel's id half.

// ── --mem_census: the shadow-map planes ────────────────────────────────────
// The formula is `res² × 4 B × 2 planes × (6 per cube omni + 1 per spot)`,
// and it scales with LIGHT COUNT — greets carries 21 shadow-casting omnis.
// Both planes are `assign`-ed, so every byte is touched at rebuild; packDyn
// is additionally re-filled per frame for any map a dynamic mesh reaches.
// --shadow_swizzle keeps two further derived copies, doubling the total.
static void MemCensus_ShadowMaps() {
    if (g_shadowMaps.empty()) return;
    size_t sd = 0, dyn = 0, sw = 0, texels = 0;
    size_t nCubeFaces = 0, nSpot = 0;
    int    minRes = 1 << 30, maxRes = 0;
    for (const ShadowMap &sm : g_shadowMaps) {
        sd     += sm.packSD.capacity()  * sizeof(uint32_t);
        dyn    += sm.packDyn.capacity() * sizeof(uint32_t);
        sw     += (sm.packSDSw.capacity() + sm.packDynSw.capacity()) * sizeof(uint32_t);
        texels += size_t(sm.xres) * size_t(sm.yres);
        if (sm.cubeFace >= 0) ++nCubeFaces; else ++nSpot;
        minRes = std::min(minRes, sm.xres);
        maxRes = std::max(maxRes, sm.xres);
    }
    const size_t nCubes = g_cubeShadowRefs.size();
    fds::MemCensus::add("shadow", "packSD (static z|id)", sd, true,
        "%zu maps = %zu cubes x 6 + %zu spots; res %d..%d; %zu texels x u32(4)",
        g_shadowMaps.size(), nCubes, nSpot, minRes, maxRes, texels);
    fds::MemCensus::add("shadow", "packDyn (dynamic z|id)", dyn, true,
        "same shape as packSD — allocated for EVERY map whether or not a "
        "dynamic mesh ever reaches it; %zu texels x u32(4)", texels);
    fds::MemCensus::add("shadow", "packSDSw+packDynSw (--shadow_swizzle)", sw, sw != 0,
        "derived 8x8-tiled COPIES of both planes; 0 unless --shadow_swizzle");
}
FDS_MEMCENSUS_REPORTER(MemCensus_ShadowMaps);
