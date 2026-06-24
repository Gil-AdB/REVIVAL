#include "BlasterBolts.h"

#include <Base/FDS_DECS.H>   // getAlignedType/Block, MatrixXVector, Mat_ID
#include <Base/FDS_VARS.H>   // View
#include <Base/FDS_DEFS.H>   // Mat_* flags, Tri_* flags
#include <Base/Scene.h>
#include <Base/Object.h>
#include <Base/TriMesh.h>
#include <Base/Face.h>
#include <Base/Vertex.h>
#include <Base/Material.h>
#include <Gradient.h>
#include <FILLERS/TheOtherBarry.h>
#include "MeshOps.h"   // Scene_AddDynamicMesh — the consolidated mesh registrar
#include <vector>

#include <cmath>
#include <cstring>
#include <cstdlib>

// Provided by MISC/PREPROC.CPP — stamps F->A_idx/B_idx/C_idx for the SoA
// transform path (without it every face indexes vertex 0 and collapses).
extern void Compute_FaceVertexIndices(TriMesh *T);

namespace fds {

namespace {

constexpr int   kMaxBolts   = 32;     // pool size
constexpr float kHalfLen    = 0.9f;   // bolt half-length (world units)
constexpr float kHalfWidth  = 0.10f;  // bolt half-width
constexpr float kColorHot   = 2.6f;   // additive core gain (blows past 255 → bloom)
constexpr int32_t kLifeTicks = 220;   // max ticks a bolt lives (~2.2 s)

struct Bolt {
	Vector  pos{};
	Vector  vel{};        // world units / tick
	float   r=0, g=0, b=0;
	int32_t life=0;       // ticks remaining; <=0 = dead
	bool    active=false;
};

Bolt      s_bolts[kMaxBolts];
TriMesh  *s_mesh   = nullptr;
Material *s_mat    = nullptr;
bool      s_inited = false;

// Additive, no-texture, no-Z-write, HDR-accumulating bolt filler — same family
// the fountain bolts use. A bolt is light: it brightens what's behind it and
// blooms; it must not occlude (WriteZ=false) but is itself Z-tested by the
// engine so walls hide it.
RasterFunc boltFiller() {
	// NONE texture mode: vertex-colour-only additive (no per-pixel texture
	// sampling). The material still carries a texture so the deferred per-tile
	// pass's "untextured → skip" gate (RenderInner.cpp) lets the face through;
	// the bolt's soft glow comes from the additive blend + HDR bloom, its hue
	// from the per-vertex colour (Mat_RGBInterp).
	// WriteZ=true: in the deferred pipeline the additive face must write Z so
	// opaque geometry behind it is Z-rejected and the per-pixel mat32 "skip
	// lighting" sentinel the filler stamps survives (else the deferred lighting
	// pass re-shades the pixel and the bolt vanishes — same as the fountain
	// vortex, which Z-tests + writes Z alongside opaque).
	return (TheOtherBarry<barry::TBlendMode::ADDITIVE, barry::TTextureMode::NONE,
	                      /*WriteZ=*/true, /*AlphaTest=*/false, /*HDRAccum=*/false>);
}

inline Vector vsub(const Vector &a, const Vector &b) { return { a.x-b.x, a.y-b.y, a.z-b.z }; }
inline Vector vcross(const Vector &a, const Vector &b) {
	return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}
inline float vlen(const Vector &a) { return std::sqrt(a.x*a.x + a.y*a.y + a.z*a.z); }
inline Vector vnorm(const Vector &a) { float l = vlen(a); return l>1e-6f ? Vector{a.x/l,a.y/l,a.z/l} : Vector{0,0,1}; }

}  // namespace

void BlasterBolts_Init(Scene *sc) {
	if (s_inited || !sc) return;

	// Additive bolt material. It NEEDS a real Texture — the deferred per-tile
	// pass skips any face whose F->Txtr->Txtr is null (RenderInner.cpp). Bake a
	// soft cross-section gradient (black edges → white core across the width);
	// additive blend makes the black edges free soft-AA, the white core blooms,
	// and the per-vertex colour (Mat_RGBInterp) tints the bolt. Generate_Gradient
	// returns a ready Material with a valid mipmapped texture.
	std::vector<GradientEndpoint> eps;
	eps.emplace_back(0.00f, Color{ 0,0,0,0 });
	eps.emplace_back(0.35f, Color{ 0.6f,0.6f,0.6f,0 });
	eps.emplace_back(0.50f, Color{ 1,1,1,0 });   // core
	eps.emplace_back(0.65f, Color{ 0.6f,0.6f,0.6f,0 });
	eps.emplace_back(1.00f, Color{ 0,0,0,0 });
	s_mat = Generate_Gradient(eps, 64, 0.04f, false);
	s_mat->Name    = strdup("__blasterBolt");
	s_mat->Flags  |= Mat_Additive | Mat_TwoSided | Mat_RGBInterp;
	s_mat->RelScene= sc;
	// Generate_Gradient's texture is laid out for the 2-D clipper (the fountain
	// bolt path), NOT the tile rasterizer the scene-mesh path uses — sampling
	// it there reads OOB. Re-tile it through the same Generate_Mipmaps the
	// regular texture loader uses (block layout + full mip chain), so the
	// TileRasterizer addresses it correctly at any depth.
	if (s_mat->Txtr) {
		s_mat->Txtr->Flags |= Txtr_Tiled;
		Generate_Mipmaps(s_mat->Txtr, DEFAULT_BLOCKSIZEX, DEFAULT_BLOCKSIZEY, 1);
	}

	TriMesh *MM = getAlignedType<TriMesh>(16);
	std::memset(MM, 0, sizeof(TriMesh));
	const int maxVerts = kMaxBolts * 4;
	const int maxFaces = kMaxBolts * 2;
	MM->Verts = (Vertex*)getAlignedBlock(sizeof(Vertex) * size_t(maxVerts), 16);
	MM->Faces = (Face*)  getAlignedBlock(sizeof(Face)   * size_t(maxFaces), 16);
	std::memset(MM->Verts, 0, sizeof(Vertex) * size_t(maxVerts));
	std::memset(MM->Faces, 0, sizeof(Face)   * size_t(maxFaces));

	// Pre-wire every face's plumbing once; per frame we only move the verts
	// and set VIndex/FIndex to the live count.
	// Quad UVs: U runs 0→1 across the bolt WIDTH so the cross-section gradient
	// (core bright at U=0.5) maps across it. V is along length (unused by the
	// 1-D gradient but kept sane). Verts 0,1 are the -side edge (U=0); 2,3 the
	// +side edge (U=1).
	const float quadU[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
	const float quadV[4] = { 0.0f, 1.0f, 1.0f, 0.0f };
	for (int q = 0; q < kMaxBolts; ++q) {
		Vertex *v = MM->Verts + q*4;
		for (int k = 0; k < 4; ++k) { v[k].i = -1; v[k].LA = 255; v[k].U = quadU[k]; v[k].V = quadV[k]; }
		// Two triangles of the quad: (0,1,2) and (0,2,3).
		const int tri[2][3] = { {0,1,2}, {0,2,3} };
		for (int t = 0; t < 2; ++t) {
			Face &F = MM->Faces[q*2 + t];
			F.A = v + tri[t][0];
			F.B = v + tri[t][1];
			F.C = v + tri[t][2];
			F.Txtr   = s_mat;
			F.Filler = boltFiller();
			F.Flags  = 0;
			F.N = { 0, 0, 1 };
			F.uvFromVertices();   // stamp per-face U1..V3 (the rasterizer reads these)
		}
	}

	// Reserve the FULL pool in VIndex/FIndex so these faces are counted in the
	// scene's poly budget (setupFaceLists sizes the render lists ONCE at init).
	// Inactive slots are kept degenerate (zero-area → no pixels) per frame.
	MM->VIndex = DWord(maxVerts);
	MM->FIndex = DWord(maxFaces);
	for (int i = 0; i < maxVerts; ++i) MM->Verts[i].Pos = { 0, 0, 0 };  // degenerate

	// Register as a dynamic mesh — the consolidated registrar handles SoA face
	// indices, dynamic-mesh marking, bounding sphere, flags, and scene links.
	Scene_AddDynamicMesh(sc, MM, "__blasterBolts", Vector{ 0, 0, 0 }, 1.0e6f);

	s_mesh = MM;
	s_inited = true;
}

void BlasterBolts_Fire(const Vector &pos, const Vector &dir, float speed,
                       float r, float g, float b) {
	if (!s_inited) return;
	for (int i = 0; i < kMaxBolts; ++i) {
		if (s_bolts[i].active) continue;
		Bolt &B = s_bolts[i];
		B.pos  = pos;
		Vector d = vnorm(dir);
		B.vel  = { d.x*speed, d.y*speed, d.z*speed };
		B.r = r; B.g = g; B.b = b;
		B.life = kLifeTicks;
		B.active = true;
		return;
	}
}

void BlasterBolts_Update(Scene *sc, float dtTicks, const Vector &camPos) {
	if (!s_inited || !s_mesh) return;

	// Advance + expire.
	for (int i = 0; i < kMaxBolts; ++i) {
		Bolt &B = s_bolts[i];
		if (!B.active) continue;
		B.pos.x += B.vel.x * dtTicks;
		B.pos.y += B.vel.y * dtTicks;
		B.pos.z += B.vel.z * dtTicks;
		B.life -= int32_t(dtTicks);
		if (B.life <= 0) B.active = false;
	}

	// Rebuild geometry in place: slot i owns verts[i*4..] / faces[i*2..].
	// Live → a camera-facing, velocity-stretched quad. Dead → collapse all 4
	// verts to one point (zero-area, no pixels; never set A==B which the
	// rasterizer treats as a sprite marker).
	int live = 0;
	for (int i = 0; i < kMaxBolts; ++i) {
		Bolt &B = s_bolts[i];
		if (!B.active) {
			Vertex *v = s_mesh->Verts + i*4;
			for (int k = 0; k < 4; ++k) v[k].Pos = { 0, 0, 0 };
			continue;
		}
		++live;
		const Vector dir  = vnorm(B.vel);
		const Vector toCam= vnorm(vsub(camPos, B.pos));
		Vector side = vcross(dir, toCam);
		float sl = vlen(side);
		if (sl < 1e-4f) {                     // bolt pointing at camera → arbitrary side
			side = vcross(dir, Vector{0,1,0});
			sl = vlen(side);
			if (sl < 1e-4f) side = Vector{1,0,0}, sl = 1.0f;
		}
		side = { side.x/sl*kHalfWidth, side.y/sl*kHalfWidth, side.z/sl*kHalfWidth };
		const Vector ax  = { dir.x*kHalfLen, dir.y*kHalfLen, dir.z*kHalfLen };

		Vertex *v = s_mesh->Verts + i*4;
		// 0: -ax-side  1: +ax-side  2: +ax+side  3: -ax+side
		v[0].Pos = { B.pos.x-ax.x-side.x, B.pos.y-ax.y-side.y, B.pos.z-ax.z-side.z };
		v[1].Pos = { B.pos.x+ax.x-side.x, B.pos.y+ax.y-side.y, B.pos.z+ax.z-side.z };
		v[2].Pos = { B.pos.x+ax.x+side.x, B.pos.y+ax.y+side.y, B.pos.z+ax.z+side.z };
		v[3].Pos = { B.pos.x-ax.x+side.x, B.pos.y-ax.y+side.y, B.pos.z-ax.z+side.z };

		const byte cr = (byte)std::min(255.0f, B.r * 255.0f * kColorHot);
		const byte cg = (byte)std::min(255.0f, B.g * 255.0f * kColorHot);
		const byte cb = (byte)std::min(255.0f, B.b * 255.0f * kColorHot);
		for (int k = 0; k < 4; ++k) {
			v[k].LR = cr; v[k].LG = cg; v[k].LB = cb; v[k].LA = 255;
			v[k].N  = toCam;
		}
		// Refresh the two faces' NormProd (cull plane) against the new verts.
		for (int t = 0; t < 2; ++t) {
			Face &F = s_mesh->Faces[i*2 + t];
			F.N = toCam;
			F.NormProd = -(toCam.x*F.A->Pos.x + toCam.y*F.A->Pos.y + toCam.z*F.A->Pos.z);
		}
	}

	(void)live;
	// VIndex/FIndex stay at full pool size (reserved at init); the mesh is
	// always "visible" — dead slots are degenerate so they cost ~nothing.
}

}  // namespace fds
