#include "BlasterBolts.h"
#include "MeshOps.h"         // Scene_MakeTiledTexture (block-tile a hand-built texture)

#include <Base/FDS_DECS.H>   // MatrixXVector, Vector_Sub, getAlignedType
#include <Base/FDS_VARS.H>   // View, FOVX/FOVY, CntrX/CntrY, C_NZP, Image
#include <Base/FDS_DEFS.H>
#include <Base/Vector.h>
#include <Base/Vertex.h>
#include <Base/Face.h>
#include <Base/Material.h>
#include <Base/Texture.h>
#include <Base/FrameState.h>      // MainRenderTargetFromGlobals, g_mainCamera
#include <Base/Omni.h>            // Omni (transient bolt-lights)
#include <Base/Scene.h>           // Scene::OmniHead
#include <Base/Spline.h>          // SplineKey (single-key init for Animate_Objects)
#include <Base/FeatureFlags.h>
#include <FRUSTRUM.H>             // Viewport, viewportInit, viewportCalcFlags
#include <Clipper.h>              // _2DClipper
#include <FILLERS/TheOtherBarry.h>

#include <cmath>
#include <cstring>
#include <cstdint>
#include <vector>

namespace fds {

namespace {

constexpr int   kMaxBolts   = 48;
constexpr float kHalfLen    = 1.2f;   // bolt half-length (world units)
constexpr float kHalfWidth  = 0.22f;  // bolt half-width
constexpr int32_t kLifeTicks = 260;

struct Bolt {
	Vector  pos{}, vel{}, dir{};   // dir stored separately so a slow bolt keeps orientation
	float   r=0, g=0, b=0;
	int32_t life=0;
	bool    active=false;
};

Bolt      s_bolts[kMaxBolts];
Material *s_mat    = nullptr;
bool      s_inited = false;

// ── Transient bolt-lights ─────────────────────────────────────────────────
// Each live bolt drives a non-stationary coloured Omni so the deferred kernel
// lights nearby geometry with a moving pool (and the froxel fog in-scatters it
// for free where fog is on — no fog-specific path needed). Pooled + capped:
// fewer lights than bolts (the per-tile cull bounds spatial cost, but the
// scene-wide omni list shouldn't balloon). Lazily allocated onto CurScene and
// re-attached if the scene changes (the module is scene-agnostic).
constexpr int kMaxBoltLights = 12;
Omni  *s_lights[kMaxBoltLights] = {};
Scene *s_lightScene = nullptr;

// Muzzle/impact flash pool — the fountain bolt_flash pattern (non-stationary
// Omni_FogTransient, exp-decay envelope supplied by the caller). Separate from
// the bolt-body lights so muzzles/impacts flash independently of the streaks.
constexpr int kMaxFlashes = 16;
Omni  *s_flashes[kMaxFlashes] = {};
Scene *s_flashScene = nullptr;
int    s_flashNext = 0;

void NopBoltOmniFiller(Face*, Vertex**, dword, dword,
                       const fds::RenderTarget&, const fds::CameraContext&) {}

// Build one idle transient omni and append it to Sc's OmniHead chain. Single-
// key splines so Animate_Objects (which walks every omni's Pos/Size/Range each
// frame) doesn't OOB; we overwrite IPos per-frame AFTER Animate_Objects anyway.
// F.A/B/C + Filler plumbed or Render() dereferences null (see fountain omnis).
Omni *makeBoltLight(Scene *Sc) {
	Omni *O = (Omni*)getAlignedBlock(sizeof(Omni), 16);
	std::memset(O, 0, sizeof(Omni));
	O->L.R = O->L.G = O->L.B = 0.0f; O->L.A = 1.0f;
	O->ISize = 0.0f; O->IRange = 1.0f; O->rRange = 1.0f;
	O->Flags = 0;                                  // idle: not Active, not Stationary
	auto initKey = [](Spline &sp) {
		sp.NumKeys = 1; sp.CurKey = 0; sp.Flags = 0;
		sp.Keys = new SplineKey; std::memset(sp.Keys, 0, sizeof(SplineKey));
	};
	initKey(O->Pos); initKey(O->Size); initKey(O->Range);
	O->F.A = O->F.B = O->F.C = &O->V;
	O->F.Filler = NopBoltOmniFiller;
	if (!Sc->OmniHead) { Sc->OmniHead = O; }
	else { Omni *t = Sc->OmniHead; while (t->Next) t = t->Next; t->Next = O; O->Prev = t; }
	return O;
}

inline float smooth01(float t) { if (t<0)t=0; if (t>1)t=1; return t*t*(3.0f-2.0f*t); }
inline Vector vnorm(const Vector &a) {
	float l = std::sqrt(a.x*a.x + a.y*a.y + a.z*a.z);
	return l>1e-6f ? Vector{a.x/l,a.y/l,a.z/l} : Vector{0,0,1};
}

// Bake a smooth, TAPERED energy-bolt texture and hand it to Scene_MakeTiledTexture,
// which block-tiles it so the rasterizer's swizzled UV addressing lands on the
// right texels (see MeshOps.h — skipping the tiling step is what made an earlier
// version sample 4x-repeated cells).
//
// Orientation is the STANDARD U→texture-column / V→texture-row mapping. The bolt
// quad uses U across WIDTH, V along LENGTH, so:
//   • columns (X, sampled by U): white-hot core + halo fading to black edges.
//   • rows    (Y, sampled by V): full in the middle, tapering to points at both ends.
// White luminance; per-vertex colour tints the hue, additive blows the core hot.
Material *makeBoltTex() {
	constexpr int W = 256, H = 256;
	std::vector<uint32_t> px(size_t(W) * size_t(H));
	for (int y = 0; y < H; ++y) {
		const float nl = float(y) / float(H-1);              // 0..1 along length (rows / V)
		const float lt = smooth01(nl/0.14f) * smooth01((1.0f-nl)/0.14f);  // end taper
		for (int x = 0; x < W; ++x) {
			const float nw = (float(x) - 0.5f*W) / (0.5f*W); // -1..1 across width (cols / U)
			const float core = std::exp(-(nw*nw) / (2.0f*0.12f*0.12f));
			const float halo = std::exp(-(nw*nw) / (2.0f*0.42f*0.42f)) * 0.45f;
			float I = (core + halo) * lt;
			if (I > 1.0f) I = 1.0f;
			const uint32_t c = uint32_t(I * 255.0f);
			px[size_t(x) + size_t(y)*W] = c | (c<<8) | (c<<16) | (255u<<24);
		}
	}
	Material *M = getAlignedType<Material>(16);
	*M = Material{};   // NSDMI defaults (Tint*/AoStrength = 1), not memset
	M->Flags = Mat_Virtual;
	M->Txtr = Scene_MakeTiledTexture(W, H, px.data(), /*buildMips=*/true);
	return M;
}

}  // namespace

void BlasterBolts_Init() {
	if (s_inited) return;
	s_mat = makeBoltTex();
	s_inited = true;
}

void BlasterBolts_Fire(const Vector &pos, const Vector &dir, float speed,
                       float r, float g, float b) {
	if (!s_inited) return;
	for (int i = 0; i < kMaxBolts; ++i) {
		if (s_bolts[i].active) continue;
		Bolt &B = s_bolts[i];
		B.pos = pos;
		B.dir = vnorm(dir);
		B.vel = { B.dir.x*speed, B.dir.y*speed, B.dir.z*speed };
		B.r=r; B.g=g; B.b=b; B.life=kLifeTicks; B.active=true;
		return;
	}
}

void BlasterBolts_Update(float dtTicks) {
	for (int i = 0; i < kMaxBolts; ++i) {
		Bolt &B = s_bolts[i];
		if (!B.active) continue;
		B.pos.x += B.vel.x*dtTicks; B.pos.y += B.vel.y*dtTicks; B.pos.z += B.vel.z*dtTicks;
		B.life -= int32_t(dtTicks);
		if (B.life <= 0) B.active = false;
	}
}

// ── Pure-t reconstruction (chase) ──────────────────────────────────────────
// Deactivate every bolt so the caller can rebuild the live set from scratch as
// a pure function of t (no velocity integration — Place() sets the world
// position directly). Update() is never called on this path.
void BlasterBolts_ResetActive() {
	for (int i = 0; i < kMaxBolts; ++i) s_bolts[i].active = false;
}

void BlasterBolts_Place(const Vector &pos, const Vector &dir,
                        float r, float g, float b) {
	if (!s_inited) return;
	for (int i = 0; i < kMaxBolts; ++i) {
		if (s_bolts[i].active) continue;
		Bolt &B = s_bolts[i];
		B.pos = pos;
		B.dir = vnorm(dir);
		B.vel = {0, 0, 0};          // unused on the pure-t path
		B.r=r; B.g=g; B.b=b; B.life=kLifeTicks; B.active=true;
		return;
	}
}

// Drive the transient bolt-lights from the live bolts. MUST be called per-frame
// AFTER Animate_Objects (which overwrites every omni's IPos from its spline) and
// BEFORE the deferred lighting builds its view-light list. No-op (and lights
// forced idle) when --no-blaster_light.
void BlasterBolts_EmitLights(Scene *Sc) {
	if (!s_inited || !Sc) return;
	const bool on = fds::FeatureFlags::blaster_light();

	// (Re)allocate the pool onto the rendered scene the first time, or if the
	// scene changed under us (chase vs greets) — old pool stays orphaned on the
	// previous scene's chain, harmless (idle, never re-activated). Sc must be the
	// scene the deferred kernel actually reads (GreetSc, NOT the global CurScene,
	// which can point elsewhere mid-tick).
	if (s_lightScene != Sc) {
		for (int i = 0; i < kMaxBoltLights; ++i) s_lights[i] = makeBoltLight(Sc);
		s_lightScene = Sc;
	}

	// hdr_glow_scale parity with the fountain strike-flash: glowMax's cap is
	// disabled under --hdr, so glow-tuned intensities run hot; scale them back.
	const float hdrMul = fds::FeatureFlags::hdr() ? fds::FeatureFlags::hdr_glow_scale() : 1.0f;
	const float isize = fds::FeatureFlags::blaster_light_intensity() * hdrMul;
	const float range = fds::FeatureFlags::blaster_light_range();
	const float rRange = range > 1e-4f ? 1.0f / range : 1.0f;

	int li = 0;
	if (on && isize > 0.0f) {
		for (int i = 0; i < kMaxBolts && li < kMaxBoltLights; ++i) {
			const Bolt &B = s_bolts[i];
			if (!B.active) continue;
			Omni *O = s_lights[li++];
			O->IPos = B.pos;
			O->L.R = B.r; O->L.G = B.g; O->L.B = B.b; O->L.A = 1.0f;
			O->ISize  = isize;
			O->IRange = range; O->rRange = rRange;
			// Non-stationary (re-lit each frame; stationary omnis get baked once
			// onto static geometry — wrong for a moving light). FogTransient so
			// the froxel fog adds its in-scatter per-frame where fog is enabled.
			O->Flags = Omni_Active | Omni_FogTransient;
		}
	}
	for (; li < kMaxBoltLights; ++li) {            // idle the unused pool lights
		Omni *O = s_lights[li];
		O->Flags &= ~Omni_Active; O->ISize = 0.0f;
	}
}

void BlasterBolts_Draw() {
	if (!s_inited || !s_mat || !View) return;

	Viewport vp; viewportInit(vp, CurScene);
	const auto rt  = fds::MainRenderTargetFromGlobals();
	const auto& cam = fds::g_mainCamera;
	auto *clip = _2DClipper::getInstance();

	for (int i = 0; i < kMaxBolts; ++i) {
		const Bolt &B = s_bolts[i];
		if (!B.active) continue;

		// Project the bolt's two world endpoints to screen (the fountain's
		// drawBoltQuad path), then build a screen-space quad: U across width,
		// V along length, matching the baked texture's orientation.
		const Vector &d = B.dir;
		const Vector A { B.pos.x-d.x*kHalfLen, B.pos.y-d.y*kHalfLen, B.pos.z-d.z*kHalfLen };
		const Vector Bp{ B.pos.x+d.x*kHalfLen, B.pos.y+d.y*kHalfLen, B.pos.z+d.z*kHalfLen };
		Vector va, vb, t;
		Vector_Sub(const_cast<Vector*>(&A),  &View->ISource, &t); MatrixXVector(View->Mat, &t, &va);
		Vector_Sub(const_cast<Vector*>(&Bp), &View->ISource, &t); MatrixXVector(View->Mat, &t, &vb);
		if (va.z < C_NZP || vb.z < C_NZP) continue;
		const float rzA = 1.0f/va.z, rzB = 1.0f/vb.z;
		const float ax = (va.z*CntrX + va.x*FOVX)*rzA, ay = (va.z*CntrY - va.y*FOVY)*rzA;
		const float bx = (vb.z*CntrX + vb.x*FOVX)*rzB, by = (vb.z*CntrY - vb.y*FOVY)*rzB;
		float ddx = bx-ax, ddy = by-ay; const float len = std::sqrt(ddx*ddx+ddy*ddy);
		if (len < 1.0f) continue;
		ddx/=len; ddy/=len;
		const float nx = -ddy, ny = ddx;
		float hwA = kHalfWidth*rzA*FOVX, hwB = kHalfWidth*rzB*FOVX;
		if (hwA < 2.0f) hwA = 2.0f; if (hwA > 300.0f) hwA = 300.0f;
		if (hwB < 2.0f) hwB = 2.0f; if (hwB > 300.0f) hwB = 300.0f;

		auto hot = [](float c){ float v = (0.5f + 0.5f*c)*255.0f; return v>255.0f?255.0f:v; };
		const float cr = hot(B.r), cg = hot(B.g), cb = hot(B.b);

		Vertex q[4];
		q[0].PX=ax-hwA*nx; q[0].PY=ay-hwA*ny; q[0].UZ=0;   q[0].VZ=0;   q[0].RZ=rzA;
		q[1].PX=ax+hwA*nx; q[1].PY=ay+hwA*ny; q[1].UZ=rzA; q[1].VZ=0;   q[1].RZ=rzA;
		q[2].PX=bx+hwB*nx; q[2].PY=by+hwB*ny; q[2].UZ=rzB; q[2].VZ=rzB; q[2].RZ=rzB;
		q[3].PX=bx-hwB*nx; q[3].PY=by-hwB*ny; q[3].UZ=0;   q[3].VZ=rzB; q[3].RZ=rzB;
		for (int k = 0; k < 4; ++k) { q[k].LR=cr; q[k].LG=cg; q[k].LB=cb; q[k].LA=255; viewportCalcFlags(vp, &q[k]); }
		Face f[2];
		f[0].A=&q[0]; f[0].B=&q[1]; f[0].C=&q[2];
		f[0].Filler=(TheOtherBarry<barry::TBlendMode::ADDITIVE, barry::TTextureMode::NORMAL_BILINEAR, false>); f[0].Txtr=s_mat;
		f[1].A=&q[0]; f[1].B=&q[2]; f[1].C=&q[3];
		f[1].Filler=(TheOtherBarry<barry::TBlendMode::ADDITIVE, barry::TTextureMode::NORMAL_BILINEAR, false>); f[1].Txtr=s_mat;
		clip->clip(vp, f[0], rt, cam);
		clip->clip(vp, f[1], rt, cam);
	}
}

// ── Muzzle / impact flashes ────────────────────────────────────────────────
void BlasterBolts_ResetFlashes(Scene *Sc) {
	if (!Sc) return;
	// (Re)allocate the pool onto the rendered scene (chase vs greets) the first
	// time or on scene change — same lazy-attach as the bolt-light pool.
	if (s_flashScene != Sc) {
		for (int i = 0; i < kMaxFlashes; ++i) s_flashes[i] = makeBoltLight(Sc);
		s_flashScene = Sc;
	}
	for (int i = 0; i < kMaxFlashes; ++i) {
		Omni *O = s_flashes[i];
		O->Flags &= ~Omni_Active; O->ISize = 0.0f;
	}
	s_flashNext = 0;
}

void BlasterBolts_AddFlash(const Vector &pos, float intensity,
                           float r, float g, float b) {
	if (!s_flashScene || s_flashNext >= kMaxFlashes) return;
	if (intensity <= 0.02f) return;
	const float peak  = fds::FeatureFlags::blaster_flash_peak();
	const float range = fds::FeatureFlags::blaster_flash_range();
	const float hdrMul = fds::FeatureFlags::hdr() ? fds::FeatureFlags::hdr_glow_scale() : 1.0f;
	Omni *O = s_flashes[s_flashNext++];
	O->IPos = pos;
	O->L.R = r; O->L.G = g; O->L.B = b; O->L.A = 1.0f;
	O->ISize  = peak * intensity * hdrMul;
	O->IRange = range; O->rRange = range > 1e-4f ? 1.0f / range : 1.0f;
	// Non-stationary (moving strike point) + FogTransient (per-frame in-scatter,
	// not baked into fog history) — exactly the fountain strike-flash flags.
	O->Flags = Omni_Active | Omni_FogTransient;
}

// ── Water reflection of the bolt streaks (§8.E) ────────────────────────────
// Mirror each live bolt across y = waterY and draw a dimmed additive billboard
// with the MAIN camera (same reflection idiom as Reflected_Transform's omni
// mirror: y -> 2*waterY - y). Occlusion vs mirrored terrain is the filler's
// built-in Z-test; call between the reflection Render and the main pass so the
// transparent water composites over these.
void BlasterBolts_DrawReflected(float waterY) {
	if (!s_inited || !s_mat || !View) return;
	const float dim = 0.55f;   // reflected streaks lose energy on the water

	Viewport vp; viewportInit(vp, CurScene);
	const auto rt  = fds::MainRenderTargetFromGlobals();
	const auto& cam = fds::g_mainCamera;
	auto *clip = _2DClipper::getInstance();

	for (int i = 0; i < kMaxBolts; ++i) {
		const Bolt &B = s_bolts[i];
		if (!B.active) continue;

		// Mirror position + direction across the water plane.
		const Vector mp{ B.pos.x, 2.0f*waterY - B.pos.y, B.pos.z };
		const Vector d { B.dir.x, -B.dir.y, B.dir.z };
		const Vector A { mp.x-d.x*kHalfLen, mp.y-d.y*kHalfLen, mp.z-d.z*kHalfLen };
		const Vector Bp{ mp.x+d.x*kHalfLen, mp.y+d.y*kHalfLen, mp.z+d.z*kHalfLen };
		Vector va, vb, t;
		Vector_Sub(const_cast<Vector*>(&A),  &View->ISource, &t); MatrixXVector(View->Mat, &t, &va);
		Vector_Sub(const_cast<Vector*>(&Bp), &View->ISource, &t); MatrixXVector(View->Mat, &t, &vb);
		if (va.z < C_NZP || vb.z < C_NZP) continue;
		const float rzA = 1.0f/va.z, rzB = 1.0f/vb.z;
		const float ax = (va.z*CntrX + va.x*FOVX)*rzA, ay = (va.z*CntrY - va.y*FOVY)*rzA;
		const float bx = (vb.z*CntrX + vb.x*FOVX)*rzB, by = (vb.z*CntrY - vb.y*FOVY)*rzB;
		float ddx = bx-ax, ddy = by-ay; const float len = std::sqrt(ddx*ddx+ddy*ddy);
		if (len < 1.0f) continue;
		ddx/=len; ddy/=len;
		const float nx = -ddy, ny = ddx;
		float hwA = kHalfWidth*rzA*FOVX, hwB = kHalfWidth*rzB*FOVX;
		if (hwA < 2.0f) hwA = 2.0f; if (hwA > 300.0f) hwA = 300.0f;
		if (hwB < 2.0f) hwB = 2.0f; if (hwB > 300.0f) hwB = 300.0f;

		auto hot = [dim](float c){ float v = (0.5f + 0.5f*c)*255.0f*dim; return v>255.0f?255.0f:v; };
		const float cr = hot(B.r), cg = hot(B.g), cb = hot(B.b);

		Vertex q[4];
		q[0].PX=ax-hwA*nx; q[0].PY=ay-hwA*ny; q[0].UZ=0;   q[0].VZ=0;   q[0].RZ=rzA;
		q[1].PX=ax+hwA*nx; q[1].PY=ay+hwA*ny; q[1].UZ=rzA; q[1].VZ=0;   q[1].RZ=rzA;
		q[2].PX=bx+hwB*nx; q[2].PY=by+hwB*ny; q[2].UZ=rzB; q[2].VZ=rzB; q[2].RZ=rzB;
		q[3].PX=bx-hwB*nx; q[3].PY=by-hwB*ny; q[3].UZ=0;   q[3].VZ=rzB; q[3].RZ=rzB;
		for (int k = 0; k < 4; ++k) { q[k].LR=cr; q[k].LG=cg; q[k].LB=cb; q[k].LA=255; viewportCalcFlags(vp, &q[k]); }
		Face f[2];
		f[0].A=&q[0]; f[0].B=&q[1]; f[0].C=&q[2];
		f[0].Filler=(TheOtherBarry<barry::TBlendMode::ADDITIVE, barry::TTextureMode::NORMAL_BILINEAR, false>); f[0].Txtr=s_mat;
		f[1].A=&q[0]; f[1].B=&q[2]; f[1].C=&q[3];
		f[1].Filler=(TheOtherBarry<barry::TBlendMode::ADDITIVE, barry::TTextureMode::NORMAL_BILINEAR, false>); f[1].Txtr=s_mat;
		clip->clip(vp, f[0], rt, cam);
		clip->clip(vp, f[1], rt, cam);
	}
}

}  // namespace fds
