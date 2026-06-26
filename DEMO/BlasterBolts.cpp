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
	std::memset(M, 0, sizeof(Material));
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

}  // namespace fds
