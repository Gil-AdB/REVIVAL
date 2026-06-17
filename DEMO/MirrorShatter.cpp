#include "MirrorShatter.h"
#include "SceneBuilder.h"

#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Base/Scene.h>
#include <Base/Object.h>
#include <Base/TriMesh.h>
#include <Base/Vertex.h>
#include <Base/Face.h>
#include <Base/Material.h>
#include <Base/Spline.h>
#include <Base/Matrix.h>
#include <Base/Camera.h>
#include <Base/FrameState.h>     // g_mainCamera/Faces, g_offAxisFrustumCull
#include <Base/RenderContext.h>  // primaryRenderContext() + RenderContext
#include <Base/CameraContext.h>  // per-worker projection state
#include <Base/FaceListContext.h>// per-worker face list
#include <Base/VertexScratch.h>  // per-worker transformed-vertex clones
#include <RENDER/OffscreenView.h>
#include <FILLERS/Mekalele.h>    // meka::GBuffer + g_gbuffer globals (deferred bake)
#include <RENDER/DeferredCommon.h> // ViewLightsSoA/TileLights/DeferredOverride + Render_DeferredLighting
#include <Base/FeatureFlags.h>
#include <Threads.h>             // ThreadPool — fan shards across cores

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <atomic>
#include <semaphore>

extern void Compute_FaceVertexIndices(TriMesh *T);
extern void Build_YOffs_Table(VESA_Surface *VS);
extern void Scene_RebuildMatTable(Scene *Sc);

namespace fds {

namespace {

// ── small vector helpers (Vector's operator* is DOT, so do scalar/cross
//    component-wise to avoid surprises) ──────────────────────────────────
inline Vector vadd(const Vector& a, const Vector& b) { return { a.x+b.x, a.y+b.y, a.z+b.z }; }
inline Vector vsub(const Vector& a, const Vector& b) { return { a.x-b.x, a.y-b.y, a.z-b.z }; }
inline Vector vscale(const Vector& a, float s)       { return { a.x*s, a.y*s, a.z*s }; }
inline float  vdot(const Vector& a, const Vector& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline float  vlen(const Vector& a)                  { return std::sqrt(vdot(a,a)); }
inline Vector vnorm(const Vector& a) { float l = vlen(a); return l > 1e-6f ? vscale(a, 1.0f/l) : Vector{0,0,0}; }
inline Vector vcross(const Vector& a, const Vector& b) {
	return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}

// Single constant keyframe so Animate_Objects has a track to walk
// (it crashes on NumKeys==0). Mirrors SceneBuilder's StampSingleKey,
// which is TU-local there. SplineKey::Pos is a W-first Quaternion.
inline void shardKey(Spline& sp, float x, float y, float z, float w) {
	sp.NumKeys = 1; sp.CurKey = 0; sp.Flags = 0;
	sp.Keys = (SplineKey*)std::calloc(1, sizeof(SplineKey));
	sp.Keys[0].Frame = 0.0f;
	sp.Keys[0].Pos.x = x; sp.Keys[0].Pos.y = y; sp.Keys[0].Pos.z = z; sp.Keys[0].Pos.W = w;
	sp.Keys[0].AA.x  = x; sp.Keys[0].AA.y  = y; sp.Keys[0].AA.z  = z; sp.Keys[0].AA.W  = w;
}

// Tuning (world units, per-frame at the demo's frame pacing).
constexpr float kGravity     = 0.005f;   // downward accel / frame²
constexpr float kPopOut      = 0.08f;    // initial push along +N (toward room)
constexpr float kRadial      = 0.05f;    // initial radial spread from impact
constexpr float kUpKick      = 0.02f;    // initial upward kick (shards arc before falling)
constexpr float kSpin        = 0.035f;   // max initial tumble rate (rad/frame)
constexpr float kRestitution = 0.18f;    // floor bounce energy retained
constexpr float kFriction    = 0.55f;    // horizontal speed retained per bounce
constexpr float kSettleVel   = 0.002f;   // |vel| below which a grounded shard settles

}  // namespace

float MirrorShatter::frand01() {
	rng_ = rng_ * 1664525u + 1013904223u;
	return float(rng_ >> 8) * (1.0f / 16777216.0f);  // [0,1)
}

namespace {
// Half-silvered glaze for the shatter shards (#3). The shards display the
// baked reflection atlas, lit white (textured material => Luminosity*255,
// BaseCol ignored), so the tint has to live in the baked pixels. A real
// half-silvered glass DESATURATES the reflection toward the (cool, dim)
// glass colour — it does NOT brighten it. So: take the per-pixel luma,
// build a cool-silver target (luma tinted blue>green>red, ~0.7 brightness
// so the glass absorbs), and lerp the reflection toward it by `sv`. This
// reads as silver rather than washing to white (the earlier additive-veil
// bug). sv from --greets-shard-silver; 0 = untouched mirror, 1 = full
// cool-silver glass. Re-baked every frame, so the console drives it live.
inline void ApplyShardSilverGlaze(uint32_t* px, int count, float sv) {
	if (sv < 0.0f) sv = 0.0f;
	if (sv > 1.0f) sv = 1.0f;
	// The half-silvered look (#2) is a DIMMED WARM reflection, NOT an added
	// silver colour. So ALWAYS halve the reflection (out = ... + dst/2), and
	// only ADD silver*sv on top — sv=0 leaves the warm reflection at half
	// brightness (matches #2's `litRGB + dst/2` with no extra cast); raise sv
	// for an optional cool cast. (Earlier the silver was added at full strength
	// and the reflection wasn't halved, so the shards read bright/cool/white
	// while #2 reads dim/warm.) FDS_TINT_RED swaps the silver for red to verify.
	static const bool kRed = std::getenv("FDS_TINT_RED") != nullptr;
	const int sr = int(float(kRed ? 255 : 150) * sv);   // optional cast = silver * sv
	const int sg = int(float(kRed ?   0 : 170) * sv);
	const int sb = int(float(kRed ?   0 : 215) * sv);
	for (int i = 0; i < count; ++i) {
		const uint32_t c = px[i];
		int b = int(c & 0xFF), g = int((c >> 8) & 0xFF), r = int((c >> 16) & 0xFF);
		r = sr + (r >> 1); if (r > 255) r = 255;    // silver*sv + reflection/2
		g = sg + (g >> 1); if (g > 255) g = 255;
		b = sb + (b >> 1); if (b > 255) b = 255;
		px[i] = uint32_t(b) | (uint32_t(g) << 8) | (uint32_t(r) << 16) | 0xFF000000u;
	}
}
}  // namespace

// Build one shard TriMesh from 4 world-space corners. Geometry is stored
// LOCAL (relative to the centroid); IPos carries the centroid so the
// rigid-body update only touches IPos + RotMat.
TriMesh* MirrorShatter::makeShardMesh(Scene* sc, const Vector wc[4], const Vector& N,
                                      Material* mat, std::vector<Vector>& localOut) {
	TriMesh* T = (TriMesh*)getAlignedBlock(sizeof(TriMesh), 16);
	std::memset(T, 0, sizeof(TriMesh));
	Matrix_Copy(T->RotMat, Mat_ID);
	Matrix_Copy(T->UnscaledRotMat, Mat_ID);
	Vector ctr = vscale(vadd(vadd(wc[0], wc[1]), vadd(wc[2], wc[3])), 0.25f);
	T->IPos   = ctr;
	T->IScale = {1.0f, 1.0f, 1.0f};
	T->IRot   = {0.0f, 0.0f, 0.0f, 1.0f};
	T->Flags  = 0;  // NOT HTrack_Visible until triggered
	shardKey(T->Pos,    ctr.x, ctr.y, ctr.z, 0.0f);
	shardKey(T->Scale,  1.0f, 1.0f, 1.0f, 0.0f);
	shardKey(T->Rotate, 0.0f, 0.0f, 0.0f, 1.0f);

	localOut.clear();
	T->VIndex = 4;
	T->Verts  = new Vertex[4];
	std::memset(T->Verts, 0, sizeof(Vertex) * 4);
	for (int i = 0; i < 4; ++i) {
		const Vector lp = vsub(wc[i], ctr);
		localOut.push_back(lp);
		T->Verts[i].Pos = lp;
		T->Verts[i].N   = N;
		T->Verts[i].TN  = N;
		T->Verts[i].LR  = T->Verts[i].LG = T->Verts[i].LB = 200;
		T->Verts[i].LA  = 255;
		T->Verts[i].U = (i == 1 || i == 2) ? 1.0f : 0.0f;
		T->Verts[i].V = (i >= 2) ? 1.0f : 0.0f;
	}
	T->FIndex = 2;
	T->Faces  = new Face[2];
	std::memset(T->Faces, 0, sizeof(Face) * 2);
	auto setupTri = [&](Face& F, int ai, int bi, int ci) {
		F.A = T->Verts + ai; F.B = T->Verts + bi; F.C = T->Verts + ci;
		F.Txtr = mat;
		F.U1 = F.A->U; F.V1 = F.A->V;
		F.U2 = F.B->U; F.V2 = F.B->V;
		F.U3 = F.C->U; F.V3 = F.C->V;
		F.N = N;
		F.NormProd = -vdot(F.N, F.A->Pos);
		F.Filler = scene_builder::SceneBuilder::PickFillerForMaterial(mat);
	};
	setupTri(T->Faces[0], 0, 1, 2);
	setupTri(T->Faces[1], 0, 2, 3);

	// Local-space bsphere (centroid is origin in local space).
	float radSq = 0.0f;
	for (int i = 0; i < 4; ++i) { const float r = vdot(localOut[i], localOut[i]); if (r > radSq) radSq = r; }
	T->BSphereCtr = {0,0,0};
	T->BSphereRad = radSq;
	T->BSphereRadius = std::sqrt(radSq);
	Compute_FaceVertexIndices(T);

	// Link into BOTH lists, exactly like SceneBuilder::AddQuad: the
	// engine iterates sc->ObjectHead (the Object wrappers) for transform
	// + render, while TriMeshHead is the parallel mesh chain. A mesh only
	// in TriMeshHead is never drawn.
	Object* Obj = new Object();
	std::memset(Obj, 0, sizeof(Object));
	Obj->Type = Obj_TriMesh;
	Obj->Data = T;
	Obj->Pos  = &T->IPos;
	Obj->Rot  = &T->RotMat;
	Obj->Next = sc->ObjectHead;
	if (sc->ObjectHead) sc->ObjectHead->Prev = Obj;
	sc->ObjectHead = Obj;
	T->Next = sc->TriMeshHead;
	if (sc->TriMeshHead) sc->TriMeshHead->Prev = T;
	sc->TriMeshHead = T;
	return T;
}

void MirrorShatter::build(Scene* sc, const Vector corners[4], const Vector& N,
                          Material* shardMat, int gx, int gy, float floorY) {
	if (!sc || !corners || !shardMat) return;
	shards_.clear();
	built_ = active_ = false;
	floorY_ = floorY;
	origin_ = corners[0];
	uAxis_  = vsub(corners[1], corners[0]);
	vAxis_  = vsub(corners[3], corners[0]);
	normal_ = vnorm(N);
	if (gx < 1) gx = 1;
	if (gy < 1) gy = 1;

	// Shard tessellation. Two independent sources of irregularity, both
	// scaled by greets_shard_randomness (0 = clean uniform grid, 1 =
	// natural cracked glass, >1 = chaotic):
	//   (A) non-uniform grid-line SPACING → shards of varied SIZE
	//   (B) per-INTERSECTION 2D jitter      → irregular SHAPES (the old
	//       code only wobbled whole lines, so every cell in a row shared
	//       an edge and stayed a rectangle)
	const float rnd = std::max(0.0f, fds::FeatureFlags::greets_shard_randomness());
	// (A) cumulative random cell widths, normalised to [0,1].
	auto makeLines = [&](int n) {
		std::vector<float> w(n), g(n + 1);
		float tot = 0.0f;
		for (int k = 0; k < n; ++k) {
			float ww = 1.0f + frandS() * 0.6f * rnd;   // ±60% × randomness
			if (ww < 0.2f) ww = 0.2f;
			w[k] = ww; tot += ww;
		}
		g[0] = 0.0f;
		for (int k = 0; k < n; ++k) g[k + 1] = g[k] + w[k] / tot;
		g[n] = 1.0f;
		return g;
	};
	const std::vector<float> gu = makeLines(gx), gv = makeLines(gy);
	// (B) 2D jitter per interior intersection (capped at ~0.45 of a cell
	// so neighbouring intersections can't cross → no self-intersecting
	// quads). Border intersections move only ALONG their edge, keeping the
	// panel's outer rectangle intact.
	const float amp = std::min(0.45f, 0.35f * rnd);
	const float ju = amp / float(gx), jv = amp / float(gy);
	const int   stride = gx + 1;
	std::vector<float> pu((gx + 1) * (gy + 1)), pv((gx + 1) * (gy + 1));
	for (int j = 0; j <= gy; ++j) {
		for (int i = 0; i <= gx; ++i) {
			float u = gu[i], v = gv[j];
			const bool edgeI = (i == 0 || i == gx);
			const bool edgeJ = (j == 0 || j == gy);
			if (!edgeI) u += frandS() * ju;   // free unless on the left/right border
			if (!edgeJ) v += frandS() * jv;   // free unless on the top/bottom border
			pu[j * stride + i] = u;
			pv[j * stride + i] = v;
		}
	}

	auto panelPt = [&](float u, float v) -> Vector {
		return vadd(origin_, vadd(vscale(uAxis_, u), vscale(vAxis_, v)));
	};

	for (int j = 0; j < gy; ++j) {
		for (int i = 0; i < gx; ++i) {
			const int a = j * stride + i,       b = j * stride + (i + 1);
			const int c = (j + 1) * stride + (i + 1), d = (j + 1) * stride + i;
			const Vector wc[4] = {
				panelPt(pu[a], pv[a]),
				panelPt(pu[b], pv[b]),
				panelPt(pu[c], pv[c]),
				panelPt(pu[d], pv[d]),
			};
			Shard s;
			s.mesh = makeShardMesh(sc, wc, normal_, shardMat, s.local);
			s.pos  = s.mesh->IPos;
			shards_.push_back(std::move(s));
		}
	}

	// Gather the static floor/stage surfaces shards can rest on: opaque,
	// UP-facing faces (world normal y > 0.3) at or below the panel base
	// (centroid.y <= floorY+1). This keeps floors + stages + steps and
	// drops ceilings (down-facing) and walls (side-facing), so a shard's
	// downward ray finds the real surface under it. Sampled once here.
	floorTris_.clear();
	for (Object* Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
		if (Obj->Type != Obj_TriMesh) continue;
		TriMesh* T = (TriMesh*)Obj->Data;
		if (!T || !T->Faces) continue;
		for (DWord fi = 0; fi < T->FIndex; ++fi) {
			Face& F = T->Faces[fi];
			if (!F.A || !F.B || !F.C) continue;
			if (F.Txtr && (F.Txtr->Flags & Mat_Transparent)) continue;  // skip glass/water surfaces
			Vector fn = F.N, wn;
			MatrixXVector(T->RotMat, &fn, &wn);
			if (wn.y < 0.3f) continue;                                  // up-facing only
			auto wp = [&](const Vertex* v) { Vector lp = v->Pos, w; MatrixXVector(T->RotMat, &lp, &w); return vadd(w, T->IPos); };
			const Vector A = wp(F.A), B = wp(F.B), C = wp(F.C);
			if ((A.y + B.y + C.y) * (1.0f/3.0f) > floorY + 1.0f) continue;  // at/below the panel base
			floorTris_.push_back(A);
			floorTris_.push_back(B);
			floorTris_.push_back(C);
		}
	}

	built_ = true;
}

// Highest static floor/stage surface directly under (x,z): a downward ray
// from high above through floorTris_, taking the topmost hit. Returns
// -1e30 if nothing is below (caller falls back to floorY_).
float MirrorShatter::castFloorAt(float x, float z) const {
	constexpr float kBigY = 1.0e4f;
	float best = -1e30f;
	for (size_t i = 0; i + 2 < floorTris_.size(); i += 3) {
		const Vector& A = floorTris_[i]; const Vector& B = floorTris_[i+1]; const Vector& C = floorTris_[i+2];
		const Vector e1 = vsub(B, A), e2 = vsub(C, A);
		const Vector P{ -e2.z, 0.0f, e2.x };          // D × e2, D = (0,-1,0)
		const float det = e1.x*P.x + e1.z*P.z;        // e1 · P (P.y = 0)
		if (det > -1e-6f && det < 1e-6f) continue;
		const float inv = 1.0f / det;
		const Vector Tv{ x - A.x, kBigY - A.y, z - A.z };
		const float u = (Tv.x*P.x + Tv.z*P.z) * inv; if (u < 0.0f || u > 1.0f) continue;
		const Vector Q{ Tv.y*e1.z - Tv.z*e1.y, Tv.z*e1.x - Tv.x*e1.z, Tv.x*e1.y - Tv.y*e1.x };
		const float v = (-Q.y) * inv; if (v < 0.0f || u + v > 1.0f) continue;  // D · Q = -Q.y
		const float t = (e2.x*Q.x + e2.y*Q.y + e2.z*Q.z) * inv;
		const float hy = kBigY - t;
		if (hy > best) best = hy;
	}
	return best;
}

void MirrorShatter::trigger(float impactU, float impactV) {
	if (!built_ || active_) return;
	const Vector impact = vadd(origin_,
		vadd(vscale(uAxis_, impactU), vscale(vAxis_, impactV)));
	for (Shard& s : shards_) {
		// Radial spread in the panel plane (away from the impact point),
		// a pop toward the room along the normal, an upward kick so the
		// shards arc before gravity takes them, plus a little jitter.
		Vector radial = vsub(s.pos, impact);
		radial.y = 0.0f;             // keep the plane spread horizontal-ish
		radial = vnorm(radial);
		s.vel = vadd(vadd(vscale(normal_, kPopOut), vscale(radial, kRadial)),
		             Vector{ frandS()*0.02f, kUpKick + frand01()*0.04f, frandS()*0.02f });
		s.angVel = { frandS()*kSpin, frandS()*kSpin, frandS()*kSpin };
		s.settled = false;
		s.mesh->Flags |= HTrack_Visible;
		// Make each shard a tiny mirror: Face_Reflective + the room
		// panorama. Transform.cpp rebuilds the per-vertex panorama lookup
		// every frame from the face normal, so as the shard tumbles its
		// reflection sweeps across the room. The deferred path forwards
		// these faces to TheOtherBarry<OVERWRITE,TEXTURETEXTURE>.
		if (reflTex_) {
			for (int fi = 0; fi < s.mesh->FIndex; ++fi) {
				s.mesh->Faces[fi].Flags |= Face_Reflective;
				s.mesh->Faces[fi].ReflectionTexture = reflTex_;
			}
		}
	}
	active_ = true;
}

void MirrorShatter::update(float dt) {
	if (!active_) return;
	if (dt <= 0.0f) dt = 1.0f;
	const float fall    = std::max(0.0f, fds::FeatureFlags::greets_shard_fall_speed());
	const bool  layFlat = fds::FeatureFlags::greets_shard_lay_flat();
	for (Shard& s : shards_) {
		if (!s.settled) {
			// Integrate.
			s.vel.y -= kGravity * fall * dt;
			s.pos = vadd(s.pos, vscale(s.vel, dt));
			s.eul = vadd(s.eul, vscale(s.angVel, dt));

			// Per-shard floor: the surface directly under THIS shard (a
			// stage, a step, the floor) — not one global plane. Re-sampled
			// only when the shard drifts > 0.5u horizontally (cheap; most
			// of the fall reuses the cache). Falls back to floorY_ if the
			// ray finds nothing below.
			if (s.cFloor <= -1e29f ||
			    std::fabs(s.pos.x - s.cfX) > 0.5f || std::fabs(s.pos.z - s.cfZ) > 0.5f) {
				const float f = castFloorAt(s.pos.x, s.pos.z);
				s.cFloor = (f > -1e29f) ? f : floorY_;
				s.cfX = s.pos.x; s.cfZ = s.pos.z;
			}
			const float fy = s.cFloor;

			Matrix rot;
			Euler_Angles(rot, s.eul.x, s.eul.y, s.eul.z);

			// Floor collision: lowest world-space vertex y.
			float lowest = 1e30f;
			for (const Vector& lv : s.local) {
				Vector rw;
				MatrixXVector(rot, &lv, &rw);
				const Vector w = vadd(rw, s.pos);
				if (w.y < lowest) lowest = w.y;
			}
			if (lowest < fy) {
				s.pos.y += fy - lowest;                  // lift back to rest on the floor
				s.vel.y = -s.vel.y * kRestitution;       // bounce
				s.vel.x *= kFriction;
				s.vel.z *= kFriction;
				s.angVel = vscale(s.angVel, kFriction);
				if (vlen(s.vel) < kSettleVel) {
					s.vel = {0,0,0};
					s.angVel = {0,0,0};
					// Final rest orientation.
					Matrix fin;
					if (layFlat) {
						// Lay flat: rotate so the shard's face normal (the
						// panel normal, in local axes) maps to +Y, i.e. the
						// quad lies in the XZ floor plane. Row-major MatrixXVector
						// means RotMat·n = +Y iff RotMat's rows are {t1, n, t2}
						// with t1,t2 an in-plane orthonormal basis. A random
						// in-plane spin gives each shard a different yaw.
						const Vector n  = vnorm(normal_);
						const Vector rf = (std::fabs(n.y) < 0.9f) ? Vector{0,1,0} : Vector{1,0,0};
						const Vector t1 = vnorm(vcross(rf, n));
						const Vector t2 = vcross(n, t1);            // unit (n,t1 orthonormal)
						const float  a  = frand01() * 6.2831853f;
						const float  ca = std::cos(a), sa = std::sin(a);
						const Vector r1 = vadd(vscale(t1, ca), vscale(t2, sa));
						const Vector r2 = vadd(vscale(t1, -sa), vscale(t2, ca));
						Matrix_Form(fin, r1.x, r1.y, r1.z,
						                 n.x,  n.y,  n.z,
						                 r2.x, r2.y, r2.z);
					} else {
						// Legacy: freeze at the tumbled orientation.
						Euler_Angles(fin, s.eul.x, s.eul.y, s.eul.z);
					}
					// Re-seat on the floor with the final orientation.
					float lo2 = 1e30f;
					for (const Vector& lv : s.local) {
						Vector rw2; MatrixXVector(fin, &lv, &rw2);
						if (rw2.y + s.pos.y < lo2) lo2 = rw2.y + s.pos.y;
					}
					if (lo2 < fy) s.pos.y += fy - lo2;
					Matrix_Copy(s.settleRot, fin);
					s.settled = true;
				}
			}
		}
		// ALWAYS publish the rigid-body pose — Animate_Objects resets the
		// mesh to its rest pose (the original z=5 panel slot) from the
		// single-key splines every frame, so even settled shards must
		// re-apply their final pose or they snap back and reform the panel.
		if (s.settled) {
			Matrix_Copy(s.mesh->RotMat, s.settleRot);
			Matrix_Copy(s.mesh->UnscaledRotMat, s.settleRot);
		} else {
			Matrix rot;
			Euler_Angles(rot, s.eul.x, s.eul.y, s.eul.z);
			Matrix_Copy(s.mesh->RotMat, rot);
			Matrix_Copy(s.mesh->UnscaledRotMat, rot);
		}
		s.mesh->IPos = s.pos;
	}
}

void MirrorShatter::debugDump() const {
	int settled = 0; float zmin = 1e30f, zmax = -1e30f, ymax = -1e30f;
	for (const Shard& s : shards_) {
		if (s.settled) ++settled;
		if (s.pos.z < zmin) zmin = s.pos.z;
		if (s.pos.z > zmax) zmax = s.pos.z;
		if (s.pos.y > ymax) ymax = s.pos.y;
	}
	std::fprintf(stderr, "[SHATTER-DBG] built=%d active=%d shards=%zu settled=%d "
	             "pos.z[%.2f..%.2f] pos.ymax=%.2f\n",
	             (int)built_, (int)active_, shards_.size(), settled, zmin, zmax, ymax);
	for (size_t i = 0; i < shards_.size() && i < 3; ++i) {
		const Shard& s = shards_[i];
		std::fprintf(stderr, "  shard %zu: pos=(%.2f,%.2f,%.2f) eul=(%.2f,%.2f,%.2f) settled=%d vis=%d\n",
		             i, s.pos.x, s.pos.y, s.pos.z, s.eul.x, s.eul.y, s.eul.z,
		             (int)s.settled, (int)((s.mesh->Flags & HTrack_Visible) != 0));
	}
}

namespace {
inline int32_t iLog2i(int32_t x) {
	union { uint32_t i; float f; } u{}; u.f = float(x);
	return int32_t((u.i >> 23) - 127);
}
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int nextPow2(int x) { int p = 1; while (p < x) p <<= 1; return p; }
// Write a linear srcRes² block into a Sachletz-tiled atlas at cell (cx,cy).
// Tiled layout (matches Sachletz): block (ax>>2,ay>>2) is X-outer/Y-inner,
// 4x4 texels per block stored Y-major within the block.
inline void blitCellTiled(dword* atlas, int atlasW, int atlasH,
                          const dword* src, int srcRes, int cx, int cy) {
	const int blocksY = atlasH >> 2;
	const int ox = cx * srcRes, oy = cy * srcRes;
	for (int y = 0; y < srcRes; ++y) {
		const int ay = oy + y;
		for (int x = 0; x < srcRes; ++x) {
			const int ax = ox + x;
			const int blk = ((ax >> 2) * blocksY + (ay >> 2)) << 4;
			atlas[blk + ((ay & 3) << 2) + (ax & 3)] = src[y * srcRes + x];
		}
	}
}
}  // namespace

// One pool thread's reflection-render scratch. Each holds a complete,
// independent render context — own surface, camera, face list and the
// vertex-clone scratch Transform_Objects writes into — so N shards render
// concurrently with no shared mutable state (the atlas cells are disjoint).
// Kept warm across frames: the surface + clone arrays stay allocated.
struct MirrorShatter::ReflWorker {
	VESA_Surface         surf{};       // owned texRes² color + Z16 target
	Camera               cam{};        // reflection camera (ISource + basis)
	fds::CameraContext   camCtx{};     // off-axis projection for this shard
	fds::FaceListContext faces{};      // own FList/SList — no global race
	fds::VertexScratch   scratch{};    // per-mesh transformed-vertex clones
	bool                 surfInit = false;
	// Deferred-bake scratch (FDS_SHARD_DEFERRED): own G-buffer + view-space
	// light list + tile-light buffer, so N shards shade through the deferred
	// kernel concurrently via Render_DeferredLighting's DeferredOverride. Empty
	// until the first deferred frame (allocated in the pool-sizing loop).
	meka::GBuffer            gb{};
	ViewLightsSoA            lights{};
	std::vector<TileLights>  tileLights;   // sized DEFERRED_NUM_TILES
	bool                     gbInit = false;
};
struct MirrorShatter::ReflPool {
	std::vector<ReflWorker> workers;
};

MirrorShatter::MirrorShatter()  = default;
MirrorShatter::~MirrorShatter() = default;

void MirrorShatter::enableReflectionCameras(Scene* sc, int texRes,
                                            Texture* textTex, const float textWorldAffine[8]) {
	// Back-compat: the original one-shot entry. The greets path now calls the
	// three pieces separately (prepare at load, text+arm at the break).
	prepareReflectionAtlas(sc, texRes);
	setShardText(textTex, textWorldAffine);
	armReflectionCameras();
}

void MirrorShatter::setShardText(Texture* textTex, const float textWorldAffine[8]) {
	if (!built_ || shards_.empty()) return;
	textTex_ = textTex;
	hasText_ = (textTex && textWorldAffine);
	if (!hasText_) return;
	// Each shard's fixed text fragment: its original (rest-pose) corners
	// mapped through the screen's world→text-UV affine. RotMat is identity
	// (rest pose), so the original world corner is local[i] + centroid.
	const float* A = textWorldAffine;
	for (Shard& s : shards_)
		for (int i = 0; i < 4 && i < int(s.local.size()); ++i) {
			const Vector w = vadd(s.local[i], s.pos);
			s.textUV[i][0] = A[0]*w.x + A[1]*w.y + A[2]*w.z + A[3];
			s.textUV[i][1] = A[4]*w.x + A[5]*w.y + A[6]*w.z + A[7];
		}
}

void MirrorShatter::prepareReflectionAtlas(Scene* sc, int texRes) {
	if (!sc || !built_ || shards_.empty()) return;
	if (reflPrepared_) return;   // idempotent — allocate once (at load)
	reflPrepared_ = true;
	texRes_  = texRes > 0 ? texRes : 64;

	// Shared offscreen render target (one 64² surf, reused per shard).
	reflSurf_ = new VESA_Surface();
	std::memset(reflSurf_, 0, sizeof(VESA_Surface));
	reflSurf_->X = texRes_; reflSurf_->Y = texRes_;
	reflSurf_->BPP = 32; reflSurf_->CPP = 4;
	reflSurf_->BPSL = texRes_ * 4;
	reflSurf_->PageSize = texRes_ * texRes_ * 4;
	reflSurf_->Data = (byte*)_aligned_malloc(size_t(texRes_) * texRes_ * 4, 16);
	reflSurf_->Z16  = (byte*)std::malloc(size_t(texRes_) * texRes_ * sizeof(word));
	reflSurf_->Flip = MainSurf ? MainSurf->Flip : nullptr;
	Build_YOffs_Table(reflSurf_);

	// One shared atlas: a grid of texRes² cells, one per shard. A material
	// per shard would overrun the 8-bit deferred matID budget, so every
	// shard shares ONE material/texture and owns a sub-rect.
	const int n = int(shards_.size());
	atlasCols_ = nextPow2(int(std::ceil(std::sqrt(double(n)))));
	atlasRows_ = nextPow2((n + atlasCols_ - 1) / atlasCols_);
	const int aw = atlasCols_ * texRes_, ah = atlasRows_ * texRes_;
	atlasTex_ = getAlignedType<Texture>(16);
	atlasTex_->Flags = Txtr_Nomip | Txtr_Tiled;
	atlasTex_->BPP = 32; atlasTex_->SizeX = aw; atlasTex_->SizeY = ah;
	atlasTex_->LSizeX = iLog2i(aw); atlasTex_->LSizeY = iLog2i(ah);
	atlasTex_->Data = (byte*)_aligned_malloc(size_t(aw) * ah * 4, 16);
	std::memset(atlasTex_->Data, 0, size_t(aw) * ah * 4);
	atlasTex_->Mipmap[0] = atlasTex_->Data; atlasTex_->numMipmaps = 1;

	// Display the atlas texel unlit (Lum saturates the light factor; Diff/Spec
	// 0 keep omnis out) — the same recipe the mirror-RTT slots use.
	atlasMat_ = getAlignedType<Material>(16);
	atlasMat_->Txtr = atlasTex_;
	// Displayed as-is (Lum=1): the half-silvered composite (silver + dst/2)
	// is baked into the atlas pixels by ApplyShardSilverGlaze, so the dimming
	// lives there, not here.
	atlasMat_->Diffuse = 0.0f; atlasMat_->Specular = 0.0f; atlasMat_->Luminosity = 1.0f;
	atlasMat_->BaseCol = Color{255.0f, 255.0f, 255.0f, 255.0f};
	atlasMat_->RelScene = sc;
	atlasMat_->Name = strdup("shard_refl_atlas");
	atlasMat_->Next = nullptr;
	if (!MatLib) { atlasMat_->Prev = nullptr; MatLib = atlasMat_; }
	else { Material* t = MatLib; while (t->Next) t = t->Next; t->Next = atlasMat_; atlasMat_->Prev = t; }

	for (Shard& s : shards_)
		for (int fi = 0; fi < s.mesh->FIndex; ++fi) {
			s.mesh->Faces[fi].Flags &= ~Face_Reflective;
			s.mesh->Faces[fi].Txtr   = atlasMat_;
			s.mesh->Faces[fi].Filler = scene_builder::SceneBuilder::PickFillerForMaterial(atlasMat_);
		}
	Scene_RebuildMatTable(sc);

	// --shard-deferred: bake each shard's reflection through the DEFERRED kernel
	// (shadowed, lit exactly as the main deferred view) instead of the forward
	// filler. The parallel path gives each worker its own G-buffer +
	// DeferredOverride (renderShardIntoCell); the serial fallback
	// (FDS_SHARD_REFL_SERIAL) still swaps the g_gbuffer globals per render.
	deferredBake_ = fds::FeatureFlags::shard_deferred();
	if (deferredBake_) {
		const size_t np = size_t(texRes_) * size_t(texRes_);
		auto mk = [&](bool xpar) {
			auto* g = new meka::GBuffer();
			g->normal.assign(np, 0);
			g->txtr.assign(np, xpar ? 0xFFFFFFFFu : 0u);
			if (!xpar) {
				g->tangent.assign(np, 0);
				g->shadowMatID.assign(np, 0);
				if (fds::FeatureFlags::shadow_lightmap()) {
					g->lightmapMF.assign(np, 0);
					g->lightmapST.assign(np, 0);
				}
			}
			return g;
		};
		reflGB_   = mk(false);
		reflGBxF_ = mk(true);
		reflGBxB_ = mk(true);
		reflXparZ_.assign(np, 0);
		reflXparZBack_.assign(np, 0);
	}
	// Warm the parallel-bake worker pool + per-worker scratch now (the cold
	// allocation was most of the break-frame bake hitch).
	ensureReflWorkers();
	// NOTE: arming (reflCamsOn_) is deliberately NOT done here — that's
	// armReflectionCameras(), called at the break. Prepare only allocates.
}

void MirrorShatter::renderReflectionCamerasSerial(Scene* sc) {
	if (!reflCamsOn_ || !active_ || !reflSurf_ || !atlasTex_) return;
	const Camera* mainCam = View ? View : sc->CameraHead;
	if (!mainCam) return;
	const bool prof = std::getenv("FDS_SHARD_REFL_PROF") != nullptr;
	const auto t0 = std::chrono::steady_clock::now();
	const Vector E = mainCam->ISource;
	const int aw = atlasCols_ * texRes_, ah = atlasRows_ * texRes_;

	// Hide every shard mesh for the whole pass: a mirror shard shouldn't see
	// the other falling fragments (recursive glass) — each reflects the room.
	for (Shard& s : shards_) s.mesh->Flags &= ~HTrack_Visible;

	// FDS_SHARD_REFL_LOOP=N repeats the whole pass N times to extend runtime
	// for a sampling profiler (the snapshot exits too fast otherwise).
	static const int loopN = std::getenv("FDS_SHARD_REFL_LOOP")
	                       ? std::atoi(std::getenv("FDS_SHARD_REFL_LOOP")) : 1;
	for (int rep = 0; rep < loopN; ++rep)
	{
		OffscreenViewScope view(sc, reflSurf_);
		static Camera s_cam;
		view.setView(&s_cam);

		// Render every shard each frame — they're reflective from both sides,
		// so none is skipped. The cost is kept down by frustum-culling the
		// reflected SCENE per shard (the narrow off-axis view rejects most of
		// the room before transform), not by skipping shards.
		for (int si = 0; si < int(shards_.size()); ++si) {
			Shard& s = shards_[si];
			if (s.local.size() < 4) continue;
			const int cx = si % atlasCols_, cy = si / atlasCols_;
			// Current world quad + normal from the shard's live pose.
			Vector wc[4];
			for (int i = 0; i < 4; ++i) {
				Vector rw; MatrixXVector(s.mesh->RotMat, &s.local[i], &rw);
				wc[i] = vadd(rw, s.pos);
			}
			Vector N; MatrixXVector(s.mesh->RotMat, &normal_, &N); N = vnorm(N);
			float d = -vdot(N, wc[0]);
			float side = vdot(N, E) + d;
			Vector axisU = vnorm(vsub(wc[1], wc[0]));
			Vector axisV = vnorm(vsub(wc[3], wc[0]));
			// Render the side the camera is on (flip N + U, keep handedness).
			if (side < 0.0f) { N = vscale(N, -1.0f); d = -d; side = -side; axisU = vscale(axisU, -1.0f); }
			if (side < 1e-3f) continue;                 // edge-on → nothing to show
			const Vector Er = vsub(E, vscale(N, 2.0f * side));
			const float  D  = side;                     // |N·Er + d|

			float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
			float cuv[4][2];
			for (int i = 0; i < 4; ++i) {
				const float pu = vdot(wc[i], axisU), pv = vdot(wc[i], axisV);
				cuv[i][0] = pu; cuv[i][1] = pv;
				u0 = std::min(u0, pu); u1 = std::max(u1, pu);
				v0 = std::min(v0, pv); v1 = std::max(v1, pv);
			}
			if (u1 - u0 < 1e-4f || v1 - v0 < 1e-4f) continue;

			std::memset(&s_cam, 0, sizeof(s_cam));
			s_cam.ISource = Er;
			s_cam.Mat[0][0] = axisU.x; s_cam.Mat[0][1] = axisU.y; s_cam.Mat[0][2] = axisU.z;
			s_cam.Mat[1][0] = axisV.x; s_cam.Mat[1][1] = axisV.y; s_cam.Mat[1][2] = axisV.z;
			s_cam.Mat[2][0] = N.x;     s_cam.Mat[2][1] = N.y;     s_cam.Mat[2][2] = N.z;
			view.publishSurface();

			// Off-axis projection: the shard window maps edge-to-edge onto
			// the texRes² target (same construction as the mirror RTT).
			const float cu = vdot(Er, axisU), cv = vdot(Er, axisV);
			FOVX   = float(texRes_) * D / (u1 - u0);
			FOVY   = float(texRes_) * D / (v1 - v0);
			CntrEX = FOVX * (cu - u0) / D;
			CntrEY = FOVY * (v1 - cv) / D;
			CntrX  = int32_t(clampf(CntrEX, -32000.0f, 32000.0f));
			CntrY  = int32_t(clampf(CntrEY, -32000.0f, 32000.0f));
			view.setNearZ(D * 1.001f + 0.01f);

			// Stamp the shard verts' UVs through the same projection, mapped
			// into this shard's atlas cell ([0,1] within the cell → the cell's
			// sub-rect of the full atlas).
			const float invCols = 1.0f / float(atlasCols_), invRows = 1.0f / float(atlasRows_);
			for (int vi = 0; vi < s.mesh->VIndex; ++vi) {
				Vector rw; MatrixXVector(s.mesh->RotMat, &s.mesh->Verts[vi].Pos, &rw);
				const Vector w = vadd(rw, s.pos);
				const float pu = vdot(w, axisU), pv = vdot(w, axisV);
				float tu = ( FOVX * (pu - cu) / D + CntrEX) / float(texRes_);
				float tv = (-FOVY * (pv - cv) / D + CntrEY) / float(texRes_);
				tu = clampf(tu, 0.004f, 0.996f);
				tv = clampf(tv, 0.004f, 0.996f);
				s.mesh->Verts[vi].U = (float(cx) + tu) * invCols;
				s.mesh->Verts[vi].V = (float(cy) + tv) * invRows;
			}
			for (int fi = 0; fi < s.mesh->FIndex; ++fi) s.mesh->Faces[fi].uvFromVertices();

			std::memset(reflSurf_->Data, 0x10, size_t(texRes_) * texRes_ * 4);
			std::memset(reflSurf_->Z16, 0, size_t(texRes_) * texRes_ * sizeof(word));
			// Reflection cone for the per-vertex cull: circumscribe the shard
			// window's 4 corner directions from Er. Rejects most of the room
			// before the view transform (the bulk of the pass's remaining cost).
			float coneTan2 = 0.0f;
			for (int i = 0; i < 4; ++i) {
				const float rx = wc[i].x - Er.x, ry = wc[i].y - Er.y, rz = wc[i].z - Er.z;
				const float ax = rx*N.x + ry*N.y + rz*N.z;
				if (ax <= 1e-4f) { coneTan2 = 1e9f; break; }   // grazing corner → wide cone
				const float t2 = ((rx*rx+ry*ry+rz*rz) - ax*ax) / (ax*ax);
				if (t2 > coneTan2) coneTan2 = t2;
			}
			g_reflConeApex = Er;
			g_reflConeDir  = N;
			g_reflConeTan2 = coneTan2 * 1.3f + 1e-3f;   // margin for edge pixels
			g_reflVertCull = true;
			g_offAxisFrustumCull = true;
			Transform_Objects(sc, fds::g_mainCamera, fds::g_mainFaces);
			g_offAxisFrustumCull = false;
			g_reflVertCull = false;
			if (CAll != 0) {
				Radix_Sort(FList, SList, CAll);
				if (deferredBake_) {
					// Deferred bake: swap the small G-buffer into the globals,
					// render deferred (shadowed, matches main view), restore.
					// skipVolumetric — a reflection wants no fog/cones/halos.
					meka::GBuffer *sgb = g_gbuffer, *sxf = g_gbufferTransparent, *sxb = g_gbufferTransparentBack;
					uint16_t *sxz = g_xparZ, *sxzb = g_xparZBack; int sxc = g_xparZCount;
					g_gbuffer = reflGB_; g_gbufferTransparent = reflGBxF_; g_gbufferTransparentBack = reflGBxB_;
					g_xparZ = reflXparZ_.data(); g_xparZBack = reflXparZBack_.data();
					g_xparZCount = int(reflXparZ_.size());
					Render(RenderPath::ForceDeferred, /*skipVolumetric=*/true);
					g_gbuffer = sgb; g_gbufferTransparent = sxf; g_gbufferTransparentBack = sxb;
					g_xparZ = sxz; g_xparZBack = sxzb; g_xparZCount = sxc;
				} else {
					// Single-threaded raster: the 64² target's threadpool
					// dispatch + barrier cost (profiled ~half the pass) dwarfs
					// the pixels.
					// Primary context = the OffscreenViewScope-swapped globals
					// (surface/camera) + g_mainFaces. Per-pass-own context lands
					// when the shard pass is parallelized (Slice 6).
					RenderForwardRegionInline(fds::primaryRenderContext(), 0, 0,
					                          float(texRes_), float(texRes_));
				}
			}

			// Reflectance gain: the forward bake is unshadowed, so greets'
			// over-ranged omnis flood it far brighter (and greener) than the
			// shadowed deferred main view. Attenuate to match — same role as
			// the screen RTT's mirror_rtt_gain. Applied before the text
			// composite so the text rides on top at full strength.
			if (reflGain_ < 0.999f) {
				const uint32_t gq = uint32_t(clampf(reflGain_, 0.0f, 1.0f) * 256.0f);
				uint32_t* px = (uint32_t*)reflSurf_->Data;
				for (int i = 0; i < texRes_ * texRes_; ++i) {
					const uint32_t c = px[i];
					const uint32_t b = ((c        & 0xFF) * gq) >> 8;
					const uint32_t g = (((c >> 8)  & 0xFF) * gq) >> 8;
					const uint32_t r = (((c >> 16) & 0xFF) * gq) >> 8;
					px[i] = b | (g << 8) | (r << 16) | 0xFF000000u;
				}
			}

			// Silver half-silvered glaze (#3) — desaturate toward cool silver.
			ApplyShardSilverGlaze((uint32_t*)reflSurf_->Data, texRes_ * texRes_,
			                      fds::FeatureFlags::greets_mirror_tint());

			// Composite the shard's fixed text fragment over its reflection,
			// half-silvered: out = text + reflection*gain (text rides on top).
			// The window→text-UV map is an affine through the shard's CURRENT
			// corner plane-coords + the corners' fixed text UVs — so the text
			// stays painted on the glass as it tumbles.
			if (hasText_ && textTex_ && textTex_->Mipmap[0]) {
				const float d1u = cuv[1][0]-cuv[0][0], d1v = cuv[1][1]-cuv[0][1];
				const float d3u = cuv[3][0]-cuv[0][0], d3v = cuv[3][1]-cuv[0][1];
				const float det = d1u*d3v - d3u*d1v;
				if (std::fabs(det) > 1e-9f) {
					const float inv = 1.0f/det;
					auto solve = [&](float t0, float t1, float t3, float& a0, float& a1, float& a2) {
						a0 = ((t1-t0)*d3v - (t3-t0)*d1v) * inv;
						a1 = ((t3-t0)*d1u - (t1-t0)*d3u) * inv;
						a2 = t0 - a0*cuv[0][0] - a1*cuv[0][1];
					};
					float au0,au1,au2, av0,av1,av2;
					solve(s.textUV[0][0], s.textUV[1][0], s.textUV[3][0], au0,au1,au2);
					solve(s.textUV[0][1], s.textUV[1][1], s.textUV[3][1], av0,av1,av2);
					const dword* td = (const dword*)textTex_->Mipmap[0];
					const int tw = textTex_->SizeX, th = textTex_->SizeY;
					const int tBlocksY = th >> 2;
					// Reflection already carries reflGain_; composite the text
					// on top at full strength (text + reflection, saturated).
					constexpr uint32_t gainQ = 256;
					uint32_t* px = (uint32_t*)reflSurf_->Data;
					const float duf = (u1-u0)/float(texRes_), dvf = (v1-v0)/float(texRes_);
					for (int y = 0; y < texRes_; ++y) {
						const float pv = v1 - (float(y)+0.5f)*dvf;
						for (int x = 0; x < texRes_; ++x) {
							const float pu = u0 + (float(x)+0.5f)*duf;
							float fu = au0*pu + au1*pv + au2;
							float fv = av0*pu + av1*pv + av2;
							const int iu = int(fu*float(tw)) & (tw-1);
							const int iv = int(fv*float(th)) & (th-1);
							const int blk = ((iu>>2)*tBlocksY + (iv>>2)) << 4;
							const dword t = td[blk + ((iv&3)<<2) + (iu&3)];
							uint32_t& o = px[size_t(y)*texRes_ + x];
							uint32_t ob = (t & 0xFF)        + ((( o      & 0xFF)*gainQ)>>8);
							uint32_t og = ((t>>8) & 0xFF)   + ((((o>>8)  & 0xFF)*gainQ)>>8);
							uint32_t orr= ((t>>16)& 0xFF)   + ((((o>>16) & 0xFF)*gainQ)>>8);
							if (ob>255) ob=255; if (og>255) og=255; if (orr>255) orr=255;
							o = ob | (og<<8) | (orr<<16) | 0xFF000000u;
						}
					}
				}
			}

			// Blit the 64² render into this shard's atlas cell (tiled).
			blitCellTiled((dword*)atlasTex_->Data, aw, ah,
			              (const dword*)reflSurf_->Data, texRes_, cx, cy);
		}
	}   // scope exit restores MainSurf/View/FOV/clip

	for (Shard& s : shards_) s.mesh->Flags |= HTrack_Visible;
	if (prof) {
		const auto t1 = std::chrono::steady_clock::now();
		std::fprintf(stderr, "[SHARD-REFL] %d shards in %.1f ms\n",
		             int(shards_.size()),
		             std::chrono::duration<double, std::milli>(t1 - t0).count());
	}
}

// ─── Forward parallel reflection pass (Slice 6) ──────────────────────────────
// Fan the shards across the thread pool: each renders WHOLE + single-threaded
// on a pool thread into its own ReflWorker (surface / camera / face-list /
// vertex-scratch), N of them concurrent. No engine globals are mutated (the
// projection lives in the worker's CameraContext; the cull cone is thread_local;
// the atlas cells are disjoint). The opt-in deferred bake and the
// FDS_SHARD_REFL_SERIAL escape hatch keep the original serial path.
void MirrorShatter::ensureReflWorkers() {
	if (shards_.empty()) return;
	const int N = int(shards_.size());
	// Size the worker pool to the thread count (one whole render per thread —
	// inter-render parallelism; tiling a 64² target is pure dispatch overhead).
	if (!reflPool_) reflPool_ = std::make_unique<ReflPool>();
	size_t P = ThreadPool::instance().size();
	if (P < 1) P = 1;
	if (P > size_t(N)) P = size_t(N);
	if (reflPool_->workers.size() != P) reflPool_->workers.resize(P);
	const int polys = Polys > 0 ? Polys : 1;
	for (ReflWorker& w : reflPool_->workers) {
		if (!w.surfInit) {
			std::memset(&w.surf, 0, sizeof(VESA_Surface));
			w.surf.X = texRes_; w.surf.Y = texRes_;
			w.surf.BPP = 32; w.surf.CPP = 4;
			w.surf.BPSL = texRes_ * 4;
			w.surf.PageSize = texRes_ * texRes_ * 4;
			w.surf.Data = (byte*)_aligned_malloc(size_t(texRes_) * texRes_ * 4, 16);
			w.surf.Z16  = (byte*)std::malloc(size_t(texRes_) * texRes_ * sizeof(word));
			w.surf.Flip = MainSurf ? MainSurf->Flip : nullptr;
			Build_YOffs_Table(&w.surf);
			w.surfInit = true;
		}
		w.faces.resize(size_t(polys));   // no-op when already sized
		// Deferred bake: size this worker's G-buffer + tile-light buffer to the
		// texRes² target (allocated once, reused; cleared per shard below).
		if (deferredBake_ && !w.gbInit) {
			const size_t np = size_t(texRes_) * size_t(texRes_);
			w.gb.normal.assign(np, 0);
			w.gb.txtr.assign(np, 0xFFFFFFFFu);
			w.gb.tangent.assign(np, 0);
			w.gb.shadowMatID.assign(np, 0);
			if (fds::FeatureFlags::shadow_lightmap()) {
				w.gb.lightmapMF.assign(np, 0);
				w.gb.lightmapST.assign(np, 0);
			}
			w.tileLights.resize(DEFERRED_NUM_TILES);
			w.gbInit = true;
		}
	}
}

void MirrorShatter::renderReflectionCameras(Scene* sc) {
	if (!reflCamsOn_ || !active_ || !atlasTex_) return;

	// FDS_SHARD_REFL_SERIAL forces the original serial path (forward or, with
	// FDS_SHARD_DEFERRED, the global-swap deferred bake). Otherwise the shards
	// fan across the pool — forward by default, or per-worker DEFERRED when
	// deferredBake_ (shadowed, hue-correct; each worker owns its G-buffer).
	static const bool forceSerial = std::getenv("FDS_SHARD_REFL_SERIAL") != nullptr;
	if (forceSerial) { renderReflectionCamerasSerial(sc); return; }

	const Camera* mainCam = View ? View : sc->CameraHead;
	if (!mainCam) return;
	const bool prof = std::getenv("FDS_SHARD_REFL_PROF") != nullptr;
	const auto t0 = std::chrono::steady_clock::now();
	const Vector E = mainCam->ISource;
	const int aw = atlasCols_ * texRes_, ah = atlasRows_ * texRes_;
	const int N  = int(shards_.size());
	if (N <= 0) return;

	// Hide every shard for the whole pass — a shard never reflects the other
	// falling fragments (no recursive glass); each reflects the room only.
	for (Shard& s : shards_) s.mesh->Flags &= ~HTrack_Visible;

	// Worker pool + per-worker scratch. Warmed at init (prepareReflectionAtlas
	// → ensureReflWorkers) so the COLD allocation isn't paid on the first
	// post-break bake; here it's a cheap no-op (only grows the face list if
	// Polys grew since init).
	ensureReflWorkers();
	const size_t P = reflPool_->workers.size();

	// Self-balancing fan-out: workers pull shard indices off a shared atomic
	// cursor. The join uses a LOCAL semaphore, NOT renderns::tileDone — each
	// worker's inner RenderForwardRegionInline round-trips tileDone net-zero,
	// so routing the outer join through it could deadlock on the final permit.
	std::atomic<int> cursor{0};
	std::counting_semaphore<256> done{0};
	auto runWorker = [&](ReflWorker& w) {
		int si;
		while ((si = cursor.fetch_add(1, std::memory_order_relaxed)) < N)
			renderShardIntoCell(sc, si, w, E, aw, ah);
		done.release();
	};
	for (size_t t = 1; t < P; ++t) {
		ReflWorker* wp = &reflPool_->workers[t];
		ThreadPool::instance().enqueue([wp, &runWorker]() { runWorker(*wp); });
	}
	runWorker(reflPool_->workers[0]);          // calling thread is worker 0
	for (size_t t = 0; t < P; ++t) done.acquire();

	for (Shard& s : shards_) s.mesh->Flags |= HTrack_Visible;
	if (prof) {
		const auto t1 = std::chrono::steady_clock::now();
		std::fprintf(stderr, "[SHARD-REFL] %d shards / %zu workers in %.1f ms\n",
		             N, P, std::chrono::duration<double, std::milli>(t1 - t0).count());
	}

	// FDS_SHARD_ATLAS_DUMP=<path>: de-tile + write the whole reflection atlas to
	// a PPM at a fixed frame (FDS_SHARD_ATLAS_FRAME, default 30 — shard poses are
	// deterministic, so forward vs deferred is a fair A/B for inspecting whether
	// the deferred shadows actually land in the reflection cells). Diagnostic.
	static const char* sAtlasDump = std::getenv("FDS_SHARD_ATLAS_DUMP");
	if (sAtlasDump) {
		static int sFrame = 0;
		static const int target = std::getenv("FDS_SHARD_ATLAS_FRAME")
		                        ? std::atoi(std::getenv("FDS_SHARD_ATLAS_FRAME")) : 30;
		if (sFrame++ == target) {
			const dword* atl = (const dword*)atlasTex_->Data;
			const int blocksY = ah >> 2;
			std::vector<unsigned char> img(size_t(aw) * size_t(ah) * 3);
			for (int y = 0; y < ah; ++y)
				for (int x = 0; x < aw; ++x) {
					const int blk = ((x >> 2) * blocksY + (y >> 2)) << 4;
					const dword px = atl[blk + ((y & 3) << 2) + (x & 3)];
					const size_t o = (size_t(y) * aw + x) * 3;
					img[o+0] = (px >> 16) & 0xFF; img[o+1] = (px >> 8) & 0xFF; img[o+2] = px & 0xFF;
				}
			if (std::FILE* f = std::fopen(sAtlasDump, "wb")) {
				std::fprintf(f, "P6\n%d %d\n255\n", aw, ah);
				std::fwrite(img.data(), 1, img.size(), f);
				std::fclose(f);
				std::fprintf(stderr, "[SHARD-ATLAS] frame %d -> %s (%dx%d)\n", target, sAtlasDump, aw, ah);
			}
		}
	}
}

// Render one shard's live reflection into its atlas cell using ONLY this
// worker's state. Mirrors the serial body, but the reflection camera + off-axis
// projection land in w.camCtx (not the FOVX/CntrE* globals), the transform
// writes w.faces / w.scratch (not the shared g_mainFaces + per-mesh frame), and
// the raster targets w.surf — so N of these run concurrently. The cull cone
// globals it sets are thread_local; the shard's own mesh UVs it stamps are
// disjoint across shards.
void MirrorShatter::renderShardIntoCell(Scene* sc, int si, ReflWorker& w,
                                        const Vector& E, int aw, int ah) {
	Shard& s = shards_[si];
	if (s.local.size() < 4) return;
	const int cx = si % atlasCols_, cy = si / atlasCols_;

	// Current world quad + normal from the shard's live pose.
	Vector wc[4];
	for (int i = 0; i < 4; ++i) {
		Vector rw; MatrixXVector(s.mesh->RotMat, &s.local[i], &rw);
		wc[i] = vadd(rw, s.pos);
	}
	Vector Nn; MatrixXVector(s.mesh->RotMat, &normal_, &Nn); Nn = vnorm(Nn);
	float d = -vdot(Nn, wc[0]);
	float side = vdot(Nn, E) + d;
	Vector axisU = vnorm(vsub(wc[1], wc[0]));
	Vector axisV = vnorm(vsub(wc[3], wc[0]));
	if (side < 0.0f) { Nn = vscale(Nn, -1.0f); d = -d; side = -side; axisU = vscale(axisU, -1.0f); }
	if (side < 1e-3f) return;                       // edge-on → nothing to show
	const Vector Er = vsub(E, vscale(Nn, 2.0f * side));
	const float  D  = side;

	float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
	float cuv[4][2];
	for (int i = 0; i < 4; ++i) {
		const float pu = vdot(wc[i], axisU), pv = vdot(wc[i], axisV);
		cuv[i][0] = pu; cuv[i][1] = pv;
		u0 = std::min(u0, pu); u1 = std::max(u1, pu);
		v0 = std::min(v0, pv); v1 = std::max(v1, pv);
	}
	if (u1 - u0 < 1e-4f || v1 - v0 < 1e-4f) return;

	// Reflection camera + off-axis projection → this worker's CameraContext.
	std::memset(&w.cam, 0, sizeof(w.cam));
	w.cam.ISource = Er;
	w.cam.Mat[0][0] = axisU.x; w.cam.Mat[0][1] = axisU.y; w.cam.Mat[0][2] = axisU.z;
	w.cam.Mat[1][0] = axisV.x; w.cam.Mat[1][1] = axisV.y; w.cam.Mat[1][2] = axisV.z;
	w.cam.Mat[2][0] = Nn.x;    w.cam.Mat[2][1] = Nn.y;    w.cam.Mat[2][2] = Nn.z;

	const float cu = vdot(Er, axisU), cv = vdot(Er, axisV);
	const float fovX   = float(texRes_) * D / (u1 - u0);
	const float fovY   = float(texRes_) * D / (v1 - v0);
	const float cntrEX = fovX * (cu - u0) / D;
	const float cntrEY = fovY * (v1 - cv) / D;
	w.camCtx.view    = &w.cam;
	w.camCtx.fovX    = fovX;
	w.camCtx.fovY    = fovY;
	w.camCtx.cntrEX  = cntrEX;
	w.camCtx.cntrEY  = cntrEY;
	w.camCtx.cntrX   = int32_t(clampf(cntrEX, -32000.0f, 32000.0f));
	w.camCtx.cntrY   = int32_t(clampf(cntrEY, -32000.0f, 32000.0f));
	const float nearZ = D * 1.001f + 0.01f;
	w.camCtx.nearZ    = nearZ;
	w.camCtx.invNearZ = 1.0f / nearZ;
	w.camCtx.farZ     = sc->FZP;
	w.camCtx.invFarZ  = 1.0f / sc->FZP;
	w.camCtx.zScale    = (float)0xff00 / (sc->FZP * 1.1f);
	w.camCtx.zScale256 = w.camCtx.zScale / 256.0f;

	// Stamp the shard verts' UVs through this projection into its atlas cell.
	const float invCols = 1.0f / float(atlasCols_), invRows = 1.0f / float(atlasRows_);
	for (int vi = 0; vi < s.mesh->VIndex; ++vi) {
		Vector rw; MatrixXVector(s.mesh->RotMat, &s.mesh->Verts[vi].Pos, &rw);
		const Vector wpt = vadd(rw, s.pos);
		const float pu = vdot(wpt, axisU), pv = vdot(wpt, axisV);
		float tu = ( fovX * (pu - cu) / D + cntrEX) / float(texRes_);
		float tv = (-fovY * (pv - cv) / D + cntrEY) / float(texRes_);
		tu = clampf(tu, 0.004f, 0.996f);
		tv = clampf(tv, 0.004f, 0.996f);
		s.mesh->Verts[vi].U = (float(cx) + tu) * invCols;
		s.mesh->Verts[vi].V = (float(cy) + tv) * invRows;
	}
	for (int fi = 0; fi < s.mesh->FIndex; ++fi) s.mesh->Faces[fi].uvFromVertices();

	std::memset(w.surf.Data, 0x10, size_t(texRes_) * texRes_ * 4);
	std::memset(w.surf.Z16,  0,    size_t(texRes_) * texRes_ * sizeof(word));

	// Reflection cone for the per-vertex cull (thread_local — this worker only).
	float coneTan2 = 0.0f;
	for (int i = 0; i < 4; ++i) {
		const float rx = wc[i].x - Er.x, ry = wc[i].y - Er.y, rz = wc[i].z - Er.z;
		const float ax = rx*Nn.x + ry*Nn.y + rz*Nn.z;
		if (ax <= 1e-4f) { coneTan2 = 1e9f; break; }
		const float t2 = ((rx*rx+ry*ry+rz*rz) - ax*ax) / (ax*ax);
		if (t2 > coneTan2) coneTan2 = t2;
	}
	g_reflConeApex = Er;
	g_reflConeDir  = Nn;
	g_reflConeTan2 = coneTan2 * 1.3f + 1e-3f;
	g_reflVertCull = true;
	g_offAxisFrustumCull = true;
	Transform_Objects(sc, w.camCtx, w.faces, texRes_, texRes_, &w.scratch);
	g_offAxisFrustumCull = false;
	g_reflVertCull = false;

	if (w.faces.cAll != 0) {
		Radix_Sort(w.faces.fList, w.faces.sList, w.faces.cAll);
		// RenderInner / MekaleleFill read only ctx.faces.fList / cAll — alias the
		// worker's buffers (no storage copy) rather than deep-copying.
		fds::RenderContext ctx;
		ctx.scene       = sc;
		ctx.camera      = w.camCtx;
		ctx.faces.fList = w.faces.fList;
		ctx.faces.sList = w.faces.sList;
		ctx.faces.cAll  = w.faces.cAll;
		ctx.target.vpage            = (uint32_t*)w.surf.Data;
		ctx.target.bytesPerScanline = w.surf.BPSL;
		ctx.target.zpage16          = (uint16_t*)w.surf.Z16;
		ctx.target.xres             = texRes_;
		ctx.target.yres             = texRes_;
		if (deferredBake_) {
			// Deferred bake: fill this worker's G-buffer, then shade it inline
			// through Render_DeferredLighting (shadowed, hue-correct — the room
			// is lit exactly as the main deferred view, no forward greening).
			// Clear the mat32 sentinel + lightmap plane (Z + colour cleared above
			// to 0x10; unshaded pixels keep that background).
			std::fill(w.gb.txtr.begin(), w.gb.txtr.end(), 0xFFFFFFFFu);
			if (!w.gb.lightmapMF.empty())
				std::fill(w.gb.lightmapMF.begin(), w.gb.lightmapMF.end(), 0u);
			ctx.target.gbuffer = &w.gb;
			MekaleleFillRegionInline(ctx, 0, 0, float(texRes_), float(texRes_));
			DeferredLightingCtx dctx{};
			DeferredOverride ov;
			ov.gb         = &w.gb;
			ov.cam        = &w.camCtx;
			ov.lights     = &w.lights;
			ov.tileLights = w.tileLights.data();
			ov.vpage      = w.surf.Data;
			ov.zpage16    = (word*)w.surf.Z16;
			ov.xres       = texRes_;
			ov.yres       = texRes_;
			ov.inlineDispatch = true;
			Render_DeferredLighting(dctx, &ov);
			// Disco-ball / spotlight volumetric cones in the reflection where
			// applicable: dctx now carries this worker's target + view-space
			// lights, so the cone pass draws the beams (forceCone/draw-cones
			// spots) additively over the shaded reflection, clipped by the
			// reflected room depth. Inline (this worker thread).
			Render_VolumetricCones(dctx, /*inlineDispatch=*/true);
		} else {
			RenderForwardRegionInline(ctx, 0, 0, float(texRes_), float(texRes_));
		}
	}

	// Reflectance gain (forward bake is unshadowed) — applied before text.
	if (reflGain_ < 0.999f) {
		const uint32_t gq = uint32_t(clampf(reflGain_, 0.0f, 1.0f) * 256.0f);
		uint32_t* px = (uint32_t*)w.surf.Data;
		for (int i = 0; i < texRes_ * texRes_; ++i) {
			const uint32_t c = px[i];
			const uint32_t b = ((c        & 0xFF) * gq) >> 8;
			const uint32_t g = (((c >> 8)  & 0xFF) * gq) >> 8;
			const uint32_t r = (((c >> 16) & 0xFF) * gq) >> 8;
			px[i] = b | (g << 8) | (r << 16) | 0xFF000000u;
		}
	}

	// Silver half-silvered glaze (#3) — desaturate toward cool silver.
	ApplyShardSilverGlaze((uint32_t*)w.surf.Data, texRes_ * texRes_,
	                      fds::FeatureFlags::greets_mirror_tint());

	// Half-silvered text composite (text + reflection*gain), same affine the
	// serial path uses (world plane-coords → fixed per-corner text UVs).
	if (hasText_ && textTex_ && textTex_->Mipmap[0]) {
		const float d1u = cuv[1][0]-cuv[0][0], d1v = cuv[1][1]-cuv[0][1];
		const float d3u = cuv[3][0]-cuv[0][0], d3v = cuv[3][1]-cuv[0][1];
		const float det = d1u*d3v - d3u*d1v;
		if (std::fabs(det) > 1e-9f) {
			const float inv = 1.0f/det;
			auto solve = [&](float t0c, float t1c, float t3c, float& a0, float& a1, float& a2) {
				a0 = ((t1c-t0c)*d3v - (t3c-t0c)*d1v) * inv;
				a1 = ((t3c-t0c)*d1u - (t1c-t0c)*d3u) * inv;
				a2 = t0c - a0*cuv[0][0] - a1*cuv[0][1];
			};
			float au0,au1,au2, av0,av1,av2;
			solve(s.textUV[0][0], s.textUV[1][0], s.textUV[3][0], au0,au1,au2);
			solve(s.textUV[0][1], s.textUV[1][1], s.textUV[3][1], av0,av1,av2);
			const dword* td = (const dword*)textTex_->Mipmap[0];
			const int tw = textTex_->SizeX, th = textTex_->SizeY;
			const int tBlocksY = th >> 2;
			constexpr uint32_t gainQ = 256;
			uint32_t* px = (uint32_t*)w.surf.Data;
			const float duf = (u1-u0)/float(texRes_), dvf = (v1-v0)/float(texRes_);
			for (int y = 0; y < texRes_; ++y) {
				const float pv = v1 - (float(y)+0.5f)*dvf;
				for (int x = 0; x < texRes_; ++x) {
					const float pu = u0 + (float(x)+0.5f)*duf;
					float fu = au0*pu + au1*pv + au2;
					float fv = av0*pu + av1*pv + av2;
					const int iu = int(fu*float(tw)) & (tw-1);
					const int iv = int(fv*float(th)) & (th-1);
					const int blk = ((iu>>2)*tBlocksY + (iv>>2)) << 4;
					const dword t = td[blk + ((iv&3)<<2) + (iu&3)];
					uint32_t& o = px[size_t(y)*texRes_ + x];
					uint32_t ob = (t & 0xFF)        + ((( o      & 0xFF)*gainQ)>>8);
					uint32_t og = ((t>>8) & 0xFF)   + ((((o>>8)  & 0xFF)*gainQ)>>8);
					uint32_t orr= ((t>>16)& 0xFF)   + ((((o>>16) & 0xFF)*gainQ)>>8);
					if (ob>255) ob=255; if (og>255) og=255; if (orr>255) orr=255;
					o = ob | (og<<8) | (orr<<16) | 0xFF000000u;
				}
			}
		}
	}

	// Blit the render into this shard's (disjoint) atlas cell (tiled).
	blitCellTiled((dword*)atlasTex_->Data, aw, ah,
	              (const dword*)w.surf.Data, texRes_, cx, cy);
}

}  // namespace fds
