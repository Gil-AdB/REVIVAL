#include "ProceduralWater.h"

#include "Rev.h"
#include <Threads.h>
#include <Base/FeatureFlags.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace pwater {

// ───────── Procedural water surface field (precomputed tiling normal map) ─────────
// THE shared water field: returns the wave SLOPE (nx,nz) at world XZ. Both the
// reflection displacement and the specular glints derive from this, so they
// ripple to the same waves (coherent water). The field is a small SEAMLESS
// tiling slope map (built once from integer-wavevector sines), sampled as TWO
// scrolling layers at different scale/rotation — the layers break each other's
// tiling into something organic + non-repetitive, and the whole thing is a
// couple of texture taps per pixel instead of ~9 trig calls (the optimization).
static std::vector<float> g_waterNrm;       // WNRM*WNRM*2 floats: slope (nx,nz), tiling
static constexpr int      WNRM = 128;

static void buildWaterNormalMap() {
	g_waterNrm.assign(size_t(WNRM) * size_t(WNRM) * 2, 0.0f);
	struct W { int kx, kz; float a, ph; };
	const W ws[8] = {
		{1, 0, 1.00f, 0.0f}, {0, 1, 0.85f, 1.3f}, {1, 1, 0.70f, 2.1f}, {2,-1, 0.55f, 0.7f},
		{-1,2, 0.45f, 3.4f}, {2, 2, 0.35f, 1.8f}, {3, 1, 0.28f, 0.4f}, {1,-3, 0.22f, 2.7f},
	};
	const float twoPi = 6.2831853f;
	float maxAbs = 1e-6f;
	for (int j = 0; j < WNRM; ++j) for (int i = 0; i < WNRM; ++i) {
		float nx = 0.0f, nz = 0.0f;
		for (const W& w : ws) {
			const float ph = twoPi * (w.kx * float(i) + w.kz * float(j)) / float(WNRM) + w.ph;
			const float c = std::cos(ph);                       // slope ∝ k·cos(phase)
			nx += w.a * (twoPi * float(w.kx) / float(WNRM)) * c;
			nz += w.a * (twoPi * float(w.kz) / float(WNRM)) * c;
		}
		float* p = &g_waterNrm[(size_t(j) * WNRM + i) * 2];
		p[0] = nx; p[1] = nz;
		if (std::fabs(nx) > maxAbs) maxAbs = std::fabs(nx);
		if (std::fabs(nz) > maxAbs) maxAbs = std::fabs(nz);
	}
	const float inv = 1.0f / maxAbs;                            // normalize slope to ~[-1,1]
	for (float& v : g_waterNrm) v *= inv;
}

static inline void sampleWaterNrm(float u, float v, float& nx, float& nz) {
	const int N = WNRM;
	float fu = u - std::floor(u / N) * N;                       // wrap to [0,N)
	float fv = v - std::floor(v / N) * N;
	int i0 = int(fu), j0 = int(fv); if (i0 >= N) i0 = 0; if (j0 >= N) j0 = 0;
	const float du = fu - i0, dv = fv - j0;
	const int i1 = (i0 + 1) % N, j1 = (j0 + 1) % N;
	const float* a = &g_waterNrm[(size_t(j0)*N + i0)*2];
	const float* b = &g_waterNrm[(size_t(j0)*N + i1)*2];
	const float* c = &g_waterNrm[(size_t(j1)*N + i0)*2];
	const float* d = &g_waterNrm[(size_t(j1)*N + i1)*2];
	nx = (a[0]*(1-du)+b[0]*du)*(1-dv) + (c[0]*(1-du)+d[0]*du)*dv;
	nz = (a[1]*(1-du)+b[1]*du)*(1-dv) + (c[1]*(1-du)+d[1]*du)*dv;
}

// nOct kept for call-site compatibility; the texture bakes all detail, so both
// the reflection and the glints get the same two-layer sample (coherent).
static inline void waterWaveSlope(float wx, float wz, float t, float scale, int /*nOct*/, float& bnx, float& bnz) {
	if (g_waterNrm.empty()) { bnx = bnz = 0.0f; return; }
	const float base = 0.02f * scale;                          // world→texel frequency
	// Scroll the two layers across the 128-texel map. The terms are in TEXEL
	// units, so they must be big (~tens) to move a meaningful fraction of the
	// map per second — small values (the first cut's ~0.7) crawl ≈1 cycle/90s,
	// i.e. frozen ("no movement"). ~16 → the dominant wave drifts ~0.3 Hz.
	float n1x, n1z; sampleWaterNrm(wx*base + t*16.0f, wz*base - t*12.0f, n1x, n1z);
	const float ca = 0.87f, sa = 0.5f;                         // ~30° rotated 2nd layer
	const float rx = wx*ca - wz*sa, rz = wx*sa + wz*ca;
	float n2x, n2z; sampleWaterNrm(rx*base*1.7f - t*10.0f, rz*base*1.7f + t*14.0f, n2x, n2z);
	bnx = (n1x + n2x*0.6f) * 1.6f;                             // ~match the old trig slope range
	bnz = (n1z + n2z*0.6f) * 1.6f;
}

// Procedural water-detail texture (row-major), sampled by the screen-space water
// pass at a field-WARPED UV per pixel. Generated at init (buildWaterDetail) —
// replaces de-swizzling the original low-res albedo (no swizzle dependence, no
// blockiness).
static std::vector<dword> g_waterTex;
static int g_waterTexW = 0, g_waterTexH = 0;

static void buildWaterDetail() {
	// Procedural, high-res, row-major → no swizzle dependence (no V1/V2 trap) and
	// no blockiness (the original albedo was low-res). A soft tiling Worley cell
	// field tinted blue-cyan; the water pass samples it field-warped for coherence.
	const int N = 256;
	g_waterTex.assign(size_t(N) * size_t(N), 0u);
	g_waterTexW = g_waterTexH = N;
	const int G = 14;                                       // feature-cell grid (tiles seamlessly via %G)
	auto fp = [](int gx, int gy, int ax) -> float {
		uint32_t h = uint32_t(gx)*73856093u ^ uint32_t(gy)*19349663u ^ uint32_t(ax+1)*2654435761u;
		h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
		return float(h & 0xFFFFu) * (1.0f/65535.0f);
	};
	const float cs = float(N) / float(G);
	for (int py = 0; py < N; ++py) for (int px = 0; px < N; ++px) {
		const float fx = (px + 0.5f) / cs, fy = (py + 0.5f) / cs;
		const int cx = int(fx), cy = int(fy);
		float d1 = 1e9f, d2 = 1e9f;                              // nearest two feature dists
		for (int oy = -1; oy <= 1; ++oy) for (int ox = -1; ox <= 1; ++ox) {
			const int gx = ((cx+ox) % G + G) % G, gy = ((cy+oy) % G + G) % G;
			const float dx = (cx+ox + fp(gx,gy,0)) - fx;
			const float dy = (cy+oy + fp(gx,gy,1)) - fy;
			const float d = dx*dx + dy*dy;
			if (d < d1) { d2 = d1; d1 = d; } else if (d < d2) { d2 = d; }
		}
		// Caustic NETWORK: bright thin lines along cell boundaries (where the two
		// nearest features are equidistant → F2-F1 ≈ 0) — the recognizable water look.
		const float edge = std::sqrt(d2) - std::sqrt(d1);
		float c = 1.0f - edge / 0.38f; if (c < 0) c = 0; c = c*c;
		int B = int(40 + c*175), Gn = int(52 + c*190), R = int(30 + c*120);
		if (B>255)B=255; if (Gn>255)Gn=255; if (R>255)R=255;
		g_waterTex[size_t(py)*N + px] = dword(B) | (dword(Gn)<<8) | (dword(R)<<16) | 0xFF000000u;
	}
	if (std::getenv("FDS_WATERTEX_DUMP")) {                  // debug: see the texture itself
		FILE* f = fopen("/tmp/water_tex.ppm", "wb");
		if (f) {
			fprintf(f, "P6\n%d %d\n255\n", N, N);
			for (int i = 0; i < N*N; ++i) {
				const dword c = g_waterTex[i];
				const unsigned char rgb[3] = { (unsigned char)((c>>16)&0xFF), (unsigned char)((c>>8)&0xFF), (unsigned char)(c&0xFF) };
				fwrite(rgb, 1, 3, f);
			}
			fclose(f);
		}
	}
}

static inline void sampleWaterTex(float u, float v, float& cb, float& cg, float& cr) {
	const int W = g_waterTexW, H = g_waterTexH;
	float fu = u - std::floor(u / W) * W;                       // wrap
	float fv = v - std::floor(v / H) * H;
	int i0 = int(fu), j0 = int(fv); if (i0 >= W) i0 = 0; if (j0 >= H) j0 = 0;
	const float du = fu - i0, dv = fv - j0;
	const int i1 = (i0 + 1) % W, j1 = (j0 + 1) % H;
	const dword a = g_waterTex[size_t(j0)*W + i0], b = g_waterTex[size_t(j0)*W + i1];
	const dword c = g_waterTex[size_t(j1)*W + i0], d = g_waterTex[size_t(j1)*W + i1];
	auto lerp4 = [&](int sh) {
		const float A=float((a>>sh)&0xFF), B=float((b>>sh)&0xFF), C=float((c>>sh)&0xFF), D=float((d>>sh)&0xFF);
		return (A*(1-du)+B*du)*(1-dv) + (C*(1-du)+D*du)*dv;
	};
	cb = lerp4(0); cg = lerp4(8); cr = lerp4(16);
}

// ───────── Public API ─────────

void BuildField() {
	buildWaterNormalMap();
	buildWaterDetail();
}

void WaveSlope(float wx, float wz, float t, float scale, float& bnx, float& bnz) {
	waterWaveSlope(wx, wz, t, scale, /*nOct=*/0, bnx, bnz);
}

// See ProceduralWater.h — RenderGlints' texMix block as a standalone helper
// (same constants, same all-octaves slope warp) for the city env-bake water
// re-shade. Kept OUT of RenderGlints' loop so the screen pass still computes
// its wave slope once per pixel for both the warp and the glint normal.
bool CausticModulation(float wx, float wz, float t, float scale,
                       float texMix, float texScale, float texWarp,
                       float flowU, float flowV,
                       float& mod, float& blueAdd)
{
	if (texMix <= 0.0f || g_waterTexW <= 0 || g_waterNrm.empty()) return false;
	float bnx, bnz;
	waterWaveSlope(wx, wz, t, scale, /*nOct=*/6, bnx, bnz);
	float cb, cg, cr;
	sampleWaterTex(wx*texScale + bnx*texWarp + flowU,
	               wz*texScale + bnz*texWarp + flowV, cb, cg, cr);
	const float cell   = (cb + cg + cr) * (1.0f/765.0f);   // 0..1 cell value
	const float cellHi = cell - 0.40f;                     // + on lines, - in base
	mod     = 1.0f + cellHi * 2.0f * texMix * 0.6f;
	blueAdd = (cellHi > 0.0f ? cellHi : 0.0f) * texMix * 220.0f;
	return true;
}

// Bump-mapped specular GLINTS on the water surface — the "bump" proper, vs the
// reflection-displacement ripple. The deferred kernel skips lighting on the
// water, so it has no specular response; this adds it. Per pixel we RAY-CAST
// the view ray to the known water plane (y = waterY) and keep the hit only if
// it isn't occluded by opaque geometry (ZPage16). This is peel-INDEPENDENT —
// unlike reading the transparent G-buffer's front layer, which doesn't hold the
// water under a multi-pass xpar peel. The wave normal is built in WORLD space
// (the hit's XZ) → grounded, perspective-correct, organic; the highlight is
// view-dependent (half-vector). 8-bit VPage; row-parallel.
void RenderGlints(float waterY, float minX, float maxX, float minZ, float /*maxZ*/) {
	const float strength = fds::FeatureFlags::water_bump_strength();
	if (strength <= 0.0f || !View) return;
	if (maxX <= minX) return;                      // water extent not set yet
	const float shin  = std::max(1.0f, fds::FeatureFlags::water_bump_shininess());
	const float scale = fds::FeatureFlags::water_bump_scale();
	// Field-warped albedo texture (only in procedural mode): the SAME field
	// distorts the texture sample → the texture ripples with the waves.
	// WaterProceduralEffective = the water material's per-surface tri-state
	// (sidecar 'waterProcedural') with the global flag as the auto fallback —
	// keeps this pass in lockstep with the deferred kernel's waterProc hoist.
	const float texMix   = WaterProceduralEffective() ? fds::FeatureFlags::water_albedo_mix() : 0.0f;
	const float texScale = fds::FeatureFlags::water_tex_scale();
	const float texWarp  = fds::FeatureFlags::water_tex_warp();
	const float t = (float)Timer * 0.02f * fds::FeatureFlags::water_ripple_speed();
	// Texture FLOW: translate the caustic UV at the wave field's world velocity
	// (the field scrolls g_waterNrm by t*16/-12 texels at frequency `base`; the
	// texture is at frequency `texScale`, so texScale/base matches world speed).
	// water_tex_flow scales it (1 = ride the glints, 0 = warp-only/in-place).
	const float wnBase = 0.02f * scale;                  // mirrors waterWaveSlope()
	const float flowK  = (wnBase > 0.0f) ? (texScale / wnBase) * fds::FeatureFlags::water_tex_flow() : 0.0f;
	const float flowU  =  t * 16.0f * flowK;
	const float flowV  = -t * 12.0f * flowK;
	const float invZScale = (g_zscale != 0.0f) ? 1.0f / g_zscale : 1.0f;
	const float invFX = (FOVX != 0.0f) ? 1.0f / FOVX : 0.0f;
	const float invFY = (FOVY != 0.0f) ? 1.0f / FOVY : 0.0f;
	const float cex = CntrEX, cey = CntrEY;
	// camera world->view rotation (rows = camera axes) + eye → view-to-world.
	const float m00=View->Mat[0][0], m10=View->Mat[1][0], m20=View->Mat[2][0];
	const float m01=View->Mat[0][1], m11=View->Mat[1][1], m21=View->Mat[2][1];
	const float m02=View->Mat[0][2], m12=View->Mat[1][2], m22=View->Mat[2][2];
	const float ex=View->ISource.x, ey=View->ISource.y, ez=View->ISource.z;
	// Key light (world): glint where the wave normal faces the V+L half-vector.
	float Lx=0.35f, Ly=0.82f, Lz=0.42f; { const float l=std::sqrt(Lx*Lx+Ly*Ly+Lz*Lz); Lx/=l;Ly/=l;Lz/=l; }
	// Wave SLOPE comes from waterWaveSlope() — the SAME field the reflection
	// ripple uses, so glints and reflection agree (coherent water).
	const float bumpAmp = 1.7f;
	const float wYplane = waterY;
	// Cut glints off at the far plane and fade them over the last stretch —
	// otherwise the grazing water near the horizon spikes the specular into
	// a bright artifact band.
	const float fzp = (CurScene && CurScene->FZP > 0.0f) ? CurScene->FZP : 1e30f;
	const float fadeStart = fzp * 0.55f;
	const float invFadeRange = 1.0f / std::max(1.0f, fzp - fadeStart);
	dword* const vp = (dword*)VPage;
	const uint16_t* const oz = ZPage16;
	const int xr = XRes;
	auto band = [=](int y0, int y1) {
		for (int y = y0; y < y1; ++y) {
			dword* row = vp + size_t(y) * size_t(xr);
			const uint16_t* orow = oz + size_t(y) * size_t(xr);
			for (int x = 0; x < xr; ++x) {
				// Ray-cast this pixel to the water plane — a peel-independent mask
				// (the transparent G-buffer's front layer doesn't hold the water
				// under a multi-pass xpar peel). View ray dir per unit depth:
				const float xn = (float(x) - cex) * invFX;
				const float yn = (cey - float(y)) * invFY;
				const float D = m01*xn + m11*yn + m21;            // d(world.y)/d(view-z)
				if (D == 0.0f) continue;
				const float sd = (wYplane - ey) / D;              // view-z of the plane hit
				if (sd <= 1.0f || sd >= fzp) continue;            // behind the eye / past the far plane
				const float distFade = sd <= fadeStart ? 1.0f : (fzp - sd) * invFadeRange;
				const float wx = ex + sd * (m00*xn + m10*yn + m20);
				const float wz = ez + sd * (m02*xn + m12*yn + m22);
				// NB: the water mesh's local bounds don't map to its world
				// placement, and the city sits in the water (everything below
				// the horizon that isn't opaque is water), so we gate on the
				// plane hit + opaque occlusion rather than an XZ rect.
				const uint16_t oe = orow[x];                      // opaque-Z occlusion
				if (oe != 0 && float(0xFF80 - int(oe)) * invZScale < sd) continue;   // opaque in front
				float bnx, bnz;
				waterWaveSlope(wx, wz, t, scale, 6, bnx, bnz);   // all octaves (fine detail)
				// Field-warped texture: sample the generated cell texture at a UV
				// distorted by the SAME wave slope → it ripples with the waves
				// (coherent). MODULATE the water brightness by the cell value
				// (rather than lerp a cyan colour over cyan water — invisible) so
				// the cells show as light/dark detail ON TOP of the reflection,
				// not hiding it.
				if (texMix > 0.0f && g_waterTexW > 0) {
					float cb, cg, cr;
					// Warp = ripple-in-place (wave slope); flow = translate with the
					// field at its world velocity (same scroll as the glints) so the
					// caustics ride the waves instead of standing still.
					sampleWaterTex(wx*texScale + bnx*texWarp + flowU,
					               wz*texScale + bnz*texWarp + flowV, cb, cg, cr);
					const float cell = (cb + cg + cr) * (1.0f/765.0f);   // 0..1 cell value
					const float cellHi = cell - 0.40f;                   // + on lines, - in base
					// Uniform (hue-preserving) brightness contrast keeps the base blue
					// instead of warming it...
					const float mod = 1.0f + cellHi * 2.0f * texMix * 0.6f;
					// ...plus an ADDITIVE blue tint on the bright caustic lines: only
					// bluens (never warms), so the caustics read distinctly blue.
					const float blueLine = (cellHi > 0.0f ? cellHi : 0.0f) * texMix * 220.0f;
					const dword p = row[x];
					int nb = int(float(p & 0xFFu)     * mod + blueLine);
					int ng = int(float((p>>8)&0xFFu)  * mod + blueLine * 0.40f);
					int nr = int(float((p>>16)&0xFFu) * mod + blueLine * 0.08f);
					if (nb<0)nb=0; if (ng<0)ng=0; if (nr<0)nr=0;
					if (nb>255)nb=255; if (ng>255)ng=255; if (nr>255)nr=255;
					row[x] = dword(nb) | (dword(ng)<<8) | (dword(nr)<<16) | 0xFF000000u;
				}
				float Nx = bnx * bumpAmp, Ny = 1.0f, Nz = bnz * bumpAmp;
				const float nInv = 1.0f / std::sqrt(Nx*Nx + Ny*Ny + Nz*Nz);
				Nx*=nInv; Ny*=nInv; Nz*=nInv;
				float Vx = ex-wx, Vy = ey-wYplane, Vz = ez-wz;   // view dir (worldPos.y = plane)
				const float vInv = 1.0f / std::sqrt(Vx*Vx + Vy*Vy + Vz*Vz);
				Vx*=vInv; Vy*=vInv; Vz*=vInv;
				float Hx = Vx+Lx, Hy = Vy+Ly, Hz = Vz+Lz;
				const float hInv = 1.0f / std::sqrt(Hx*Hx + Hy*Hy + Hz*Hz);
				const float ndh = (Nx*Hx + Ny*Hy + Nz*Hz) * hInv;
				if (ndh <= 0.0f) continue;
				const float g = std::pow(ndh, shin) * strength * distFade;
				int add = int(g * 255.0f + 0.5f); if (add <= 0) continue; if (add > 255) add = 255;
				const dword p = row[x];
				int B = int(p & 0xFFu) + add, G = int((p>>8)&0xFFu) + add, R = int((p>>16)&0xFFu) + add;
				if (B>255)B=255; if (G>255)G=255; if (R>255)R=255;
				row[x] = dword(B) | (dword(G)<<8) | (dword(R)<<16) | 0xFF000000u;
			}
		}
	};
	auto& tp = ThreadPool::instance();
	const int nT = (int)tp.size();
	if (nT < 2 || YRes < 64) { band(0, YRes); return; }
	const int chunk = (YRes + nT - 1) / nT;
	auto remaining = std::make_shared<std::atomic<int>>(0);
	for (int i = 0; i < nT; ++i) {
		const int y0 = i * chunk; if (y0 >= YRes) break;
		const int y1 = std::min(y0 + chunk, YRes);
		remaining->fetch_add(1, std::memory_order_relaxed);
		tp.enqueue([band, y0, y1, remaining]() { band(y0, y1); remaining->fetch_sub(1, std::memory_order_release); });
	}
	while (remaining->load(std::memory_order_acquire) != 0) std::this_thread::yield();
}

}  // namespace pwater
