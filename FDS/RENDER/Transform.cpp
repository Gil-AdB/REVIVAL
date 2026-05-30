// Per-frame geometry pipeline stages — extracted from RENDER.CPP.
//
//   Animate_Objects(Sc, skipCameraAnim)
//     Walks scene splines / keyframes and updates Pos/Rot/Scale on each
//     Object. Builds RotMat per TriMesh. Runs once before transform.
//
//   Vertex_Loop1   (dead code; legacy MMX-era helper kept for reference)
//
//   calcVisibilityFlags(Sc, vtx)
//     Frustum-test classification for a single vertex.
//
//   addParticleTrail(Sc, FListInsertPtr, particle)
//     Emit per-particle Face entries with trail-segment vertices.
//
//   IsFrontFacingInViewSpace(F)
//     View-space facing test; also used by RenderXparClumpInStrip in
//     the deferred-transparent path so it stays non-static.
//
//   QuadAwareMaxViewZ(F, T)
//     Quad-sibling-extended max view-z for back-to-front sort keys.
//
//   Transform_Objects(Sc, xresOverride, yresOverride)
//     The big one. Walks ObjectHead, per-object: skips invisible/culled,
//     transforms vertices into view space, runs particles, builds FList,
//     stamps SortZ/VisibilityFlags/Filler, populates CPolys/COmnies/CPcls.

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <Base/Vector.h>
#include <memory>
#include <algorithm>

// 4-wide SIMD for the per-vertex transform inner loops below. Loads/stores
// are on the fly via vectorclass Vec4f; the per-vertex matrix-vector is
// rewritten as 3 broadcast-FMAs against column-major matrices instead of
// 9 scalar muls + 9 scalar adds. See Transform_Objects for the staging.
#include "simde/x86/sse.h"
#include <simd/vectorclass.h>

// Scalar polynomial atan2/asin approximations for the reflective-face
// equirectangular EU/EV per-vertex stamp (used heavily by city windows).
#include "FILLERS/SimdHelpers.h"

// SoA Vertex refactor — Phase 1: per-mesh SoA companion of the AoS
// transformed-vertex output fields. See docs/SOA_VERTEX_REFACTOR.md.
#include "Base/VertexFrame.h"

// Front-to-back face sort. Closer faces dispatch first so subsequent
// farther faces fail Z and skip the rasterizer's per-pixel work — a
// pure perf optimization. The original RENDER.CPP defined this at
// line 38; the 415fd16 extraction to Transform.cpp accidentally
// dropped it, silently selecting the back-to-front branch. That
// surfaced as a deferred-reflective-window regression: the order
// reversal interacted badly with TheOtherBarry's forward filler
// running inside Mekalele's tile pass — Mekalele wrote wall mat32
// before TheOtherBarry overdrew the window, and the lighting kernel
// then re-shaded the wall over the window's reflection. That
// correctness issue is fixed in the rasterizer (TheOtherBarry now
// stamps the mat32 sentinel for every pixel it writes in deferred
// mode); this define stays in place purely for the perf win.
#define FRONT_TO_BACK_SORTING

#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Base/FeatureFlags.h"
#include "Base/Scene.h"
#include "Base/TriMesh.h"
#include "Base/Vertex.h"
#include "Base/Face.h"
#include "Base/Omni.h"
#include "Base/Camera.h"
#include "Base/Material.h"
#include "Base/Object.h"
#include "Base/Spline.h"
#include "Base/CameraContext.h"
#include "Base/FaceListContext.h"
#include "Base/VertexScratch.h"
#include "FILLERS/ShadowMap.h"

// Defined in Shadows.cpp; the shadow orchestrator sets this to the
// current shadow light before calling Transform_Objects, so the per-mesh
// loop below can read its cone params for the bsphere-vs-cone cull.
extern thread_local Omni* g_currentShadowOmni;

// Return true when the world-space sphere (center C, radius r) lies
// fully outside the spot cone (apex P, normalized axis D, outer cosine
// cosOuter, max range). Conservative — false-negatives (keeps a sphere
// it could safely cull) are fine; false-positives (culls a sphere that
// should contribute) cause missing shadows, so the test is widened by
// the sphere radius along the cone surface (r/cosOuter at any depth)
// and clamped near the apex.
static inline bool sphereOutsideSpotCone(const Vector& C, float r,
                                          const Vector& P, const Vector& D,
                                          float cosOuter, float maxRange)
{
	const float vx = C.x - P.x;
	const float vy = C.y - P.y;
	const float vz = C.z - P.z;
	const float v2 = vx*vx + vy*vy + vz*vz;

	// Range: a sphere centre farther than (range + r) from the apex
	// can't fall inside the cone within `maxRange` of the apex.
	const float rMax = maxRange + r;
	if (v2 > rMax * rMax) return true;

	// Project onto axis. If the sphere is entirely behind the apex by
	// more than its radius, it can't reach the cone.
	const float distAlongAxis = vx*D.x + vy*D.y + vz*D.z;
	if (distAlongAxis < -r) return true;

	// Cone-vs-sphere at the sphere's depth. Defensively skip the cull
	// for very wide cones (cosOuter near 0) — the math goes wonky and
	// the cull buys little there anyway.
	if (cosOuter < 1e-3f) return false;
	const float sinOuter = std::sqrt(std::max(0.0f, 1.0f - cosOuter * cosOuter));
	const float tanOuter = sinOuter / cosOuter;

	const float depth = distAlongAxis > 0.0f ? distAlongAxis : 0.0f;
	const float coneRadiusAtDepth = depth * tanOuter + r / cosOuter;
	const float perpSq = v2 - distAlongAxis * distAlongAxis;
	return perpSq > coneRadiusAtDepth * coneRadiusAtDepth;
}

// Specialized test for the 90°-FOV cube-face pyramid's circumscribed
// cone (cos = 1/√3 = 0.5774). Called per mesh per cube face per frame
// — at greets's chunked Piramid this is ~10k+ ops, so we sqrt+div
// inline once-deduplicate the trig out. Same conservative shape as
// sphereOutsideSpotCone with cosOuter=0.577; just constant-folded.
static inline bool sphereOutsidePyramidCone(const Vector& C, float r,
                                             const Vector& P, const Vector& D,
                                             float maxRange)
{
	constexpr float kTanOuter = 1.4142135623730951f;  // √2
	constexpr float kInvCos   = 1.7320508075688772f;  // √3 = 1 / (1/√3)
	const float vx = C.x - P.x, vy = C.y - P.y, vz = C.z - P.z;
	const float v2 = vx*vx + vy*vy + vz*vz;
	const float rMax = maxRange + r;
	if (v2 > rMax * rMax) return true;
	const float distAlongAxis = vx*D.x + vy*D.y + vz*D.z;
	if (distAlongAxis < -r) return true;
	const float depth = distAlongAxis > 0.0f ? distAlongAxis : 0.0f;
	const float coneRAtDepth = depth * kTanOuter + r * kInvCos;
	const float perpSq = v2 - distAlongAxis * distAlongAxis;
	return perpSq > coneRAtDepth * coneRAtDepth;
}

// Defined in RENDER.CPP. Forward-declare to avoid pulling the rest of
// RENDER.CPP's prelude in here.
float frand();

void Animate_Objects(Scene *Sc, Camera *cam)
{
	// `cam` is the explicit camera to animate (replaces the old global
	// `View` read + SkipCameraAnimation bool). nullptr = no camera ops,
	// safe to call from off-render-loop threads (e.g. Initialize_X bake
	// prep) without touching globals.
	const bool SkipCameraAnimation = (cam == nullptr);
	Camera *const View = cam;  // alias so the legacy body still reads `View`
	TriMesh *T;
	Omni *Om;
	Object *Obj;
	Vector U,*W,Z;
	Matrix M, PathMat, tmp;
	FILE *F;
    Vector ZeroVector(0.0f, 0.0f, 0.0f);


    //  F = fopen("Matrix.txt","at");
	for (T=Sc->TriMeshHead;T;T=T->Next)
	{
		if (T->Flags&Tri_Possessed) continue;
		Spline_Calc_3D(&T->Pos,CurFrame,&T->IPos);
		Spline_Calc_3D(&T->Scale,CurFrame,&T->IScale);
		//    Vector_Form(&T->IScale,1,1,1); // until i get it right
		
		if (T->Flags&Tri_Euler)
		{
			Spline_Calc_3D(&T->Rotate,CurFrame,&U);
			Euler_Angles(T->RotMat,U.x,U.y,U.z);
		} else {
			Spline_Calc_4D_Alt(&T->Rotate,CurFrame,&T->IRot);
			//Spline_Subdivide_Bezier(&T->Rotate,CurFrame,&T->IRot);
			//    Spline_Calc_4D(&T->Rotate,CurFrame,&T->IRot);
			
			Convert_Quat2Mat(&T->IRot,T->RotMat);
			//    fprintf(F,"%d:((%1.3f,%1.3f,%1.3f),(%1.3f,%1.3f,%1.3f),(%1.3f,%1.3f,%1.3f))\n\n",(int32_t)CurFrame,T->RotMat[0][0],T->RotMat[0][1],T->RotMat[0][2],T->RotMat[1][0],T->RotMat[1][1],T->RotMat[1][2],T->RotMat[2][0],T->RotMat[2][1],T->RotMat[2][2]);
		}
		
		if (T->Flags&Tri_AlignToPath) 
		{
			const float Aheadfactor = 1.0f;
			Spline_Calc_3D(&T->Pos,CurFrame+Aheadfactor,&Z);
			Vector_Sub(&Z, &T->IPos,&U);
			
			float l = Vector_Length(&U);
			
			// replace constant by 
			if (l > Sc->PathingMinVelocity)
			{
				Vector_Copy(&T->Heading, &U);
			}
			
			Kick_Camera(&ZeroVector, &T->Heading, 0.0, PathMat);
			Matrix_Transpose(PathMat);

			Spline_Calc_3D(&T->Rotate, CurFrame, &U);
			Euler_Angles(T->RotMat, 0, 0, U.z);


			MatrixXMatrix(PathMat, T->RotMat, tmp);
			Matrix_Copy(T->RotMat,tmp);
		}
		
		memcpy(T->UnscaledRotMat, T->RotMat, sizeof(T->RotMat));
		W = (Vector *)T->RotMat;
		Vector_SelfScale(W,T->IScale.x); W++;
		Vector_SelfScale(W,T->IScale.y); W++;
		Vector_SelfScale(W,T->IScale.z); W++;
//AfterScale:;
	/*if (T->Status)
	{
		if (CurFrame>T->CurStat->Frame)
		T->CurStat=T->CurStat->Next;
		T->Flags&=0xFFFFFFFF-HTrack_Visible;
		T->Flags|=T->CurStat->Stat;
	} else*/ 	
		// automatic resetting to VISIBLE removed @ 06.04.02 to enable manual modification
		// of hiding state
		//T->Flags|=HTrack_Visible;
	}
	//  fclose(F);
	for(Om=Sc->OmniHead;Om;Om=Om->Next)
	{
		Spline_Calc_3D(&Om->Pos,CurFrame,&Om->IPos);
		if (Om->Size.NumKeys)
			Spline_Calc_1D(&Om->Size,CurFrame,&Om->ISize);
		else
			Om->ISize = 1.0f;
		Spline_Calc_1D(&Om->Range,CurFrame,&Om->IRange);
		Om->rRange = 1.0f/Om->IRange;
		//		Om->IRange*=Om->IRange;
	}
	if (View!=&FC && !SkipCameraAnimation)
	{
		Spline_Calc_3D(&View->Source,CurFrame,&View->ISource);
		Spline_Calc_3D(&View->Target,CurFrame,&View->ITarget);
		Spline_Calc_1D(&View->Roll,CurFrame,&View->IRoll);
		Spline_Calc_1D(&View->FOV,CurFrame,&View->IFOV);
		if (View->Flags & Cam_Euler) {
			//Euler_Angles(View->Mat,View->ITarget.x,View->ITarget.y,View->ITarget.z);
			Euler_Angles(View->Mat, View->ITarget.y, View->ITarget.x, -View->ITarget.z);
		} else {
			Kick_Camera(&View->ISource, &View->ITarget, View->IRoll, View->Mat);
		}
	}

	// Skip the global-View ops entirely when SkipCameraAnimation is set.
	// Callers like Initialize_Greets's bake-prep run on the t1 init thread
	// before any scene's render loop sets the global `View` — touching it
	// here would either crash on null or stomp on whichever scene happens
	// to have set it last. The bake doesn't need PerspX/PerspY or the
	// FOVX/FOVY globals; those are render-loop concerns.
	if (!SkipCameraAnimation && View) {
		CalcPersp(View);
		FOVX = View->PerspX;
		FOVY = View->PerspY;
	}
	
	
	// lalala, HARARCHIA , Ver 3, it now rulati
	for (Obj=Sc->ObjectHead;Obj;Obj=Obj->Next)
	{
		if (Obj->Type == Obj_Omni) {
			Matrix_Identity(*Obj->Rot);
		}
		//if (Obj->Pivot.is_zero()) {
		MatrixXVector(*Obj->Rot, &Obj->Pivot, &U);
//			U.x = U.y = U.z = 0.0f;
		//} else {
			//MatrixXVector(*Obj->Rot, &Obj->Pivot, &U);
		//}
		//printf("%s U: \n", Obj->Name);
		//U.print();
		//printf("\n");
		Vector_SelfSub(Obj->Pos,&U);
		if (Obj->Parent)
		{
			MatrixXVector(*Obj->Parent->Rot,Obj->Pos,&U);
			Vector_Add(Obj->Parent->Pos,&U,Obj->Pos);
			// Skip the rotation compose for omnis. Omni Objects alias
			// `Obj->Rot` to the SHARED global identity matrix `Mat_ID`
			// (FLD_CONV.CPP:114), so `Matrix_Copy(*Obj->Rot, M)` here
			// would write parent.Rot into Mat_ID — corrupting every
			// other reader of the global identity matrix until the
			// next omni iteration's line-224 reset. Omnis have no
			// meaningful rotation anyway; the position composition
			// above is the only piece they need.
			if (Obj->Type != Obj_Omni) {
				MatrixXMatrix(*Obj->Parent->Rot,*Obj->Rot,M);
				Matrix_Copy(*Obj->Rot,M);
			}
		}
	}
}

void Vertex_Loop1(Vertex *Vert,Vertex *VEnd,Matrix M,Vector *V)
{
	Vertex *Vtx;
	float *f = (float *)M;
	Vector U;
	for (Vtx=Vert;Vtx<VEnd;Vtx++)
	{
		//    if (!Vtx->FRem) continue;
		//    MatrixXVector(M,&Vtx->Pos,&U);
		//    Vector_Add(&U,V,&Vtx->TPos);
		//    Vtx->TPos.x = (*f++)*Vtx->Pos.x+(*f++)*Vtx->Pos.y+(*f++)*Vtx->Pos.z+V->x;
		//    Vtx->TPos.y = (*f++)*Vtx->Pos.x+(*f++)*Vtx->Pos.y+(*f++)*Vtx->Pos.z+V->y;
		//    Vtx->TPos.z = (*f++)*Vtx->Pos.x+(*f++)*Vtx->Pos.y+(*f)*Vtx->Pos.z+V->z;
		//    f-=8;
		MatrixXVector(M,&Vtx->Pos,&Vtx->TPos);
		Vector_SelfAdd(&Vtx->TPos,V);
		
		
		Vtx->RZ=1.0/Vtx->TPos.z;
		Vtx->PX=Vtx->TPos.x*Vtx->RZ;
		Vtx->PY=Vtx->TPos.y*Vtx->RZ;
		Vtx->UZ=Vtx->U*Vtx->RZ;
		Vtx->VZ=Vtx->V*Vtx->RZ;
		Vtx->Flags&=0xFFFFFFFF-Vtx_Visible;
		if (Vtx->PX<0) Vtx->Flags|=Vtx_VisLeft;
		if (Vtx->PX>=XRes) Vtx->Flags|=Vtx_VisRight;
		if (Vtx->PY<0) Vtx->Flags|=Vtx_VisUp;
		if (Vtx->PY>=YRes) Vtx->Flags|=Vtx_VisDown;
	}
}

void calcVisibilityFlags(Scene* Sc, Vertex* Vtx, fds::CameraContext &cam) {
	Vtx->Flags &= 0xFFFFFFFF - Vtx_Visible;
	//      if (*(int32_t *)(&Vtx->TPos.z)>0x3F800000) // 1.0 in floating point rep.
	if (Vtx->TPos.z > cam.nearZ) {
		Vtx->RZ = 1.0 / Vtx->TPos.z;
		Vtx->PX = Vtx->TPos.x * Vtx->RZ;
		Vtx->PY = Vtx->TPos.y * Vtx->RZ;
		//          Vtx->PX=CntrEX+PX*Vtx->TPos.x*Vtx->RZ;
		//          Vtx->PY=CntrEY-PY*Vtx->TPos.y*Vtx->RZ;
		Vtx->UZ = Vtx->U * Vtx->RZ;
		Vtx->VZ = Vtx->V * Vtx->RZ;
		if (Vtx->PX < 0) Vtx->Flags |= Vtx_VisLeft;
		if (Vtx->PX >= XRes) Vtx->Flags |= Vtx_VisRight;
		if (Vtx->PY < 0) Vtx->Flags |= Vtx_VisUp;
		if (Vtx->PY >= YRes) Vtx->Flags |= Vtx_VisDown;
		if (Vtx->TPos.z > cam.farZ) Vtx->Flags |= Vtx_VisFar;
	} else Vtx->Flags |= Vtx_VisNear;
}

void addParticleTrail(Scene* Sc, fds::FListEntry*& Ins, Particle& p, fds::CameraContext &cam) {
	Vector V;

	Vector VelDir = p.Vel;
	Vector_Norm(&VelDir);

	Vector src = p.V.Pos - p.TrailLength * VelDir;
	Vector targ = p.V.Pos;

	Vector d1 = (src - cam.view->ISource).cross(targ - cam.view->ISource);
	Vector_Norm(&d1);

	int quad_uvs[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
	Vector* centerPoints[2] = { &src, &targ };
	Vertex* quad = p.TrailV;
	for (const auto& uv : quad_uvs) {
		float u = float(uv[0]);
		int v = uv[1];
		// interpolate along middle of raindrop quad

		const Vector& tmp = *(centerPoints[v]);
		Vertex& A = *quad;
		V = tmp + d1 * (u - 0.5f) * p.TrailWidth - cam.view->ISource;
		MatrixXVector(cam.view->Mat, &V, &A.TPos);

		A.TPos.x = A.TPos.z * cam.cntrX + A.TPos.x * cam.fovX;
		A.TPos.y = A.TPos.z * cam.cntrY - A.TPos.y * cam.fovY;
		A.RZ = 1.0f / A.TPos.z;
		A.PX = A.TPos.x * A.RZ;
		A.PY = A.TPos.y * A.RZ;

		A.LA = A.LR = A.LG = A.LB = 255.0;

		calcVisibilityFlags(Sc, &A, cam);
		++quad;
	}

	for (size_t i = 0; i != 2; ++i) {
#ifdef FRONT_TO_BACK_SORTING
		p.TrailF[i].SortZ.F = 2 * cam.farZ - p.V.TPos.z;
#else
		p.TrailF[i].SortZ.F = cam.farZ - p.V.TPos.z;
#endif
	}
	*Ins++ = { p.TrailF[0].SortZ.DW, &p.TrailF[0] };
	*Ins++ = { p.TrailF[1].SortZ.DW, &p.TrailF[1] };
}

#define DEBUG_PARTICLES 0

// Per-face front-facing test using view-space vertex positions. Computes
// the triangle's face normal via edge cross-product, then sign-tests against
// vertex A's view-space position. Same test the deferred transparent
// classifier uses (see RenderInnerDeferredTransparent) — face-level, not
// per-vertex, so coplanar triangles of the same quad classify identically.
bool IsFrontFacingInViewSpace(const Face* F)
{
	const float ex = F->B->TPos.x - F->A->TPos.x;
	const float ey = F->B->TPos.y - F->A->TPos.y;
	const float ez = F->B->TPos.z - F->A->TPos.z;
	const float fx = F->C->TPos.x - F->A->TPos.x;
	const float fy = F->C->TPos.y - F->A->TPos.y;
	const float fz = F->C->TPos.z - F->A->TPos.z;
	const float nx = ey * fz - ez * fy;
	const float ny = ez * fx - ex * fz;
	const float nz = ex * fy - ey * fx;
	const float vd = nx * F->A->TPos.x + ny * F->A->TPos.y + nz * F->A->TPos.z;
	return vd < 0.0f;
}

// View-space max-Z for a transparent triangle, extended to include its quad
// sibling's unshared vertex when present. Without this, the two coplanar
// triangles of a quad get sort keys based on different vertex subsets,
// splitting them across the back-to-front render order so other faces draw
// between them. On a convex transparent mesh (a glass cube) this caused the
// bottom face's two tris to render at positions 4 and 12 in a 12-triangle
// sort, with front walls drawn over the first half and back walls overdrawing
// the second.
//
// "Sibling" = adjacent Face in the same TriMesh with the same Face normal
// sharing ≥2 vertex pointers. Quads built via appendQuad-style code always
// emit their two tris adjacently, so we only need to check F-1 and F+1.
// `facesBase` is the array F was iterated from — which is the per-pass
// CLONE array (tFaces) when VertexScratch is in use, NOT T->Faces. Using
// T->Faces here was a heap-buffer-overflow waiting for ASan: clone arrays
// are separate allocations with different bounds. ASan caught it within
// the first transparent frame of greets.
static float QuadAwareMaxViewZ(const Face* F, const Face* facesBase, DWord facesCount)
{
	float dz = F->A->TPos.z;
	if (F->B->TPos.z > dz) dz = F->B->TPos.z;
	if (F->C->TPos.z > dz) dz = F->C->TPos.z;

	if (fds::FeatureFlags::no_quad_sort()) return dz;

	auto sameNormal = [](const Face* x, const Face* y) {
		return x->N.x == y->N.x && x->N.y == y->N.y && x->N.z == y->N.z;
	};
	auto sharedCount = [](const Face* x, const Face* y) {
		int n = 0;
		if (y->A == x->A || y->A == x->B || y->A == x->C) ++n;
		if (y->B == x->A || y->B == x->B || y->B == x->C) ++n;
		if (y->C == x->A || y->C == x->B || y->C == x->C) ++n;
		return n;
	};
	auto extendWithSibling = [&](const Face* sib) {
		Vertex* unshared = nullptr;
		if      (sib->A != F->A && sib->A != F->B && sib->A != F->C) unshared = sib->A;
		else if (sib->B != F->A && sib->B != F->B && sib->B != F->C) unshared = sib->B;
		else if (sib->C != F->A && sib->C != F->B && sib->C != F->C) unshared = sib->C;
		if (unshared && unshared->TPos.z > dz) dz = unshared->TPos.z;
	};

	const Face* prev = (F > facesBase)                  ? F - 1 : nullptr;
	const Face* next = (F + 1 < facesBase + facesCount) ? F + 1 : nullptr;
	if      (prev && sameNormal(F, prev) && sharedCount(F, prev) >= 2) extendWithSibling(prev);
	else if (next && sameNormal(F, next) && sharedCount(F, next) >= 2) extendWithSibling(next);

	return dz;
}

// xresOverride / yresOverride: when >= 0, use these instead of the
// global XRes / YRes for vertex visibility flags + face-level
// VisibilityFlagsAll() culling. Lets a caller (e.g. the shadow-map
// orchestrator) project to a different screen rect than the main
// framebuffer without globally mutating XRes/YRes mid-frame. Default
// (-1, -1) preserves legacy behavior.
void Transform_Objects(Scene *Sc, fds::CameraContext &cam, fds::FaceListContext &faces,
                        int xresOverride, int yresOverride,
                        fds::VertexScratch *scratch)
{
	extern thread_local bool g_inShadowPass;
	// Hoist the thread_local load out of the per-vertex hot loops below
	// (Aft / Ahead / Regular paths). macOS TLS access goes through
	// __tls_get_addr or an emutls trampoline — multiple cycles each, and
	// the compiler can't CSE the load across the `MatrixXVector` calls
	// in the body. Caching here turns it into a register read inside the
	// per-vertex `if (!_inShadowPass)` checks at the three sites below.
	const bool _inShadowPass = g_inShadowPass;
	const bool coneCull = g_inShadowPass
		&& fds::FeatureFlags::shadow_cone_cull()
		&& g_currentShadowOmni
		&& g_currentShadowOmni->Type == Light_SpotLight;
	extern thread_local ShadowMap *g_currentShadowMap;
	// Per-cube-face bsphere cull: when xforming geometry into one of the
	// 6 cube faces of an Omni shadow, the face only sees a 90°-FOV pyramid
	// along one cardinal axis. Most meshes are outside that pyramid (an
	// omni's 6 faces partition the world into 6 disjoint hemispheres, so
	// the average mesh is visible to ~1 face out of 6). Skip the per-vertex
	// transform + face submission for meshes whose bsphere is entirely
	// outside this face's pyramid. Reuses sphereOutsideSpotCone with the
	// CIRCUMSCRIBED cone of the 90° pyramid (half-angle ≈ 54.7°, cos ≈
	// 0.577) so we never cull a sphere that could be visible at a corner.
	const bool cubeFaceCull = g_inShadowPass
		&& fds::FeatureFlags::shadow_cube_face_cull()
		&& g_currentShadowOmni
		&& g_currentShadowOmni->Type == Light_Omni
		&& g_currentShadowMap
		&& g_currentShadowMap->cubeFace >= 0;
	Vector cubeFaceDir = { 0.0f, 0.0f, 0.0f };
	Vector cubeFacePos = { 0.0f, 0.0f, 0.0f };
	float  cubeFaceCos = 0.0f;
	float  cubeFaceRange = 0.0f;
	if (cubeFaceCull) {
		// Cube face order matches CubeShadowMaps_Rebuild + the per-face
		// camera setup in Render_DeferredShadowMaps:
		// 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z.
		switch (g_currentShadowMap->cubeFace) {
			case 0: cubeFaceDir.x =  1.0f; break;
			case 1: cubeFaceDir.x = -1.0f; break;
			case 2: cubeFaceDir.y =  1.0f; break;
			case 3: cubeFaceDir.y = -1.0f; break;
			case 4: cubeFaceDir.z =  1.0f; break;
			case 5: cubeFaceDir.z = -1.0f; break;
		}
		cubeFacePos = g_currentShadowOmni->IPos;
		// Circumscribed cone of the 90° square pyramid: half-angle =
		// atan(sqrt(2)) ≈ 54.7°, so cos ≈ 0.577. Conservative (lets
		// through more spheres than a tight pyramid test); never under-
		// culls. Tight 4-plane pyramid test would be ~2× the math for
		// maybe ~30% tighter cull; not worth it at this granularity.
		cubeFaceCos = 0.577f;
		cubeFaceRange = g_currentShadowOmni->IRange;
	}
	extern thread_local bool g_inDynamicShadowBake;
	// Two shadow-bake mesh filters:
	//   inStaticBake : skip dynamic meshes (their t=0 silhouette would
	//                  freeze in the once-baked static map).
	//   inDynamicBake: skip static meshes (the static bake already has
	//                  them; the per-frame dynamic pass only contains
	//                  the moving parts).
	// Same isDynamicForBake() predicate decides both.
	const bool inStaticBake = g_inShadowPass
		&& g_currentShadowOmni
		&& (g_currentShadowOmni->Flags & Omni_StaticShadow)
		&& !g_inDynamicShadowBake
		&& fds::FeatureFlags::shadow_skip_animated();
	const bool inDynamicBake = g_inShadowPass && g_inDynamicShadowBake;
	// Normalize the cone axis once: the shadow lighting kernel does the
	// same for its per-pixel cone test (see StaticLighting), so the
	// authored IDir is not guaranteed unit-length in world space.
	Vector coneDir = { 0.0f, 0.0f, 0.0f };
	Vector conePos = { 0.0f, 0.0f, 0.0f };
	float  coneCosOuter = 0.0f;
	float  coneRange    = 0.0f;
	if (coneCull) {
		coneDir = g_currentShadowOmni->IDir;
		Vector_Norm(&coneDir);
		conePos      = g_currentShadowOmni->IPos;
		coneCosOuter = g_currentShadowOmni->FallOff;
		coneRange    = g_currentShadowOmni->IRange;
	}
	const int32_t xr = (xresOverride >= 0) ? xresOverride : XRes;
	const int32_t yr = (yresOverride >= 0) ? yresOverride : YRes;
	TriMesh *T;
	Omni *O;
	Matrix M,IM;
	float M34[3][4];
	Vector AP,S,U,OS,V,*W=(Vector *)(&M),*W2,*Scl;
	float L1,L2,L3;
	Vertex *Vtx,*VEnd;
	Face *F,*FEnd;
	float PX=cam.fovX,PY=cam.fovY,Temp;
	float dz;
	int32_t *pdz = (int32_t *)(&dz);
	int32_t I;
	fds::FListEntry *Ins = faces.fList;
	float *f = (float *)(&M);
	float *fv;

	float fzp = cam.farZ;

#if not(DEBUG_PARTICLES)
	Object *Obj; 
//	for (T=Sc->TriMeshHead;T;T=T->Next)
	for(Obj=Sc->ObjectHead; Obj; Obj=Obj->Next)
	{
		
		if (Obj->Type != Obj_TriMesh) continue;
		//if (stricmp(Obj->Name, "water.lwo")) continue;
		T = (TriMesh *)(Obj->Data);

		uint32_t frustumFlags = 0;  // Tri_Invisible | Tri_Ahead | Tri_Inside, racy
		                            // when T->Flags is shared across N parallel
		                            // shadow-render tasks. Hold locally per call.

		if (!(T->Flags&HTrack_Visible)) {frustumFlags|=Tri_Invisible; continue;}

		// Static-bake filter: skip meshes whose *position* animates
		// (Pos spline has more than 1 key). Their t=0 silhouette would
		// otherwise be frozen in the never-rebaked shadow as they move.
		// We deliberately don't check Rotate/Scale — many FLD scenes
		// author no-op rotate/scale envelopes (NumKeys=2 with identical
		// values) on otherwise-static meshes, which would over-filter.
		// A rotating-in-place mesh will have a slightly wrong shadow
		// but won't disappear; pure translation is the bigger artifact.
		// Per-mesh "is dynamic" decision:
		//   1. Own Pos spline extent > ε (mesh translates)
		//   2. Own Rotate spline has more than 1 key (mesh rotates;
		//      we don't compute quaternion extent — just trust NumKeys)
		//   3. Any ancestor moves (parent's IPos changes drive ours)
		// Threshold of 0.1 in scene units handles the (~0.005) noise
		// that FLD often authors as 2-key Pos envelopes for static
		// meshes (e.g. Piramid.lwo in greets).
		auto isDynamicForBake = [](Object *obj) -> bool {
			constexpr float kPosExtentEps = 0.1f;
			constexpr float kRotExtentEps = 0.01f;   // unit-quat delta
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
					    (zmax-zmin) > kPosExtentEps) return true;
				}
				// Rotate spline stores a Quaternion in Keys[i].Pos
				// (x,y,z,W). Many FLD scenes author no-op 2-key Rotate
				// envelopes — extent-check on all four components to
				// keep those classified as static.
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
					    (wmax-wmin) > kRotExtentEps) return true;
				}
			}
			return false;
		};
		// Symmetric counterpart for the dynamic per-frame bake: skip
		// meshes whose Pos/Rotate splines are effectively static —
		// those already live in the once-baked static map.
		if (inDynamicBake && !isDynamicForBake(Obj)) {
			static std::atomic<int> sDynSkip{0};
			if (sDynSkip.fetch_add(1) < 32) {
				std::fprintf(stderr,
				    "[DYN-BAKE-SKIP-MESH] '%s' (static)\n",
				    (Obj->Name ? Obj->Name : "?"));
			}
			continue;
		}
		if (inDynamicBake) {
			static std::atomic<int> sDynKeep{0};
			if (sDynKeep.fetch_add(1) < 32) {
				std::fprintf(stderr,
				    "[DYN-BAKE-KEEP-MESH] '%s' Pos.NumKeys=%u Faces=%u\n",
				    (Obj->Name ? Obj->Name : "?"),
				    unsigned(T->Pos.NumKeys), unsigned(T->FIndex));
			}
		}
		if (inStaticBake && isDynamicForBake(Obj)) {
			static std::atomic<int> sSkipLogged{0};
			if (sSkipLogged.fetch_add(1) < 32) {
				std::fprintf(stderr,
				    "[STATIC-BAKE-SKIP-MESH] '%s' Pos.NumKeys=%u Rot.NumKeys=%u parent='%s'\n",
				    (Obj->Name ? Obj->Name : "?"),
				    unsigned(T->Pos.NumKeys), unsigned(T->Rotate.NumKeys),
				    (Obj->Parent && Obj->Parent->Name) ? Obj->Parent->Name : "(none)");
			}
			continue;
		}
		if (inStaticBake) {
			static std::atomic<int> sKeepLogged{0};
			if (sKeepLogged.fetch_add(1) < 32) {
				std::fprintf(stderr, "[STATIC-BAKE-KEEP-MESH] '%s' Pos.NumKeys=%u Rot.NumKeys=%u Faces=%u\n",
					(Obj->Name ? Obj->Name : "?"), unsigned(T->Pos.NumKeys),
					unsigned(T->Rotate.NumKeys), unsigned(T->FIndex));
			}
		}

		// Per-pass clone redirection. With scratch non-null, all
		// reads/writes of this TriMesh's per-vertex projection state
		// (Vertex::PX, PY, RZ, TPos, TN, TTangent, UZ, VZ, Flags…)
		// land in scratch's clone instead of T's own Verts. Face
		// pushes also use the clone's Face* (whose A/B/C point into
		// the clone's Verts), so downstream code sees a coherent
		// per-pass snapshot. Nullptr → in-place writes to tVerts
		// (main pass keeps the no-allocation fast path).
		Vertex *tVerts;
		Face   *tFaces;
		if (scratch) {
			auto& clone = scratch->cloneOf(T);
			tVerts = clone.verts.data();
			tFaces = clone.faces.data();
		} else {
			tVerts = T->Verts;
			tFaces = T->Faces;
		}

		// Mesh-bsphere-vs-cone cull during shadow-pass Transform_Objects.
		// Cheap pre-test that lets us skip the matrix work + vertex
		// transform + face faces.fList submission for any mesh whose bsphere
		// can't contribute to this light's shadow map. Saves both the
		// per-vertex xform and the per-face raster work downstream.
		if (coneCull) {
			Vector wsBsphereCtr;
			MatrixXVector(T->RotMat, &T->BSphereCtr, &wsBsphereCtr);
			Vector_SelfAdd(&wsBsphereCtr, &T->IPos);
			if (sphereOutsideSpotCone(wsBsphereCtr, T->BSphereRadius,
			                          conePos, coneDir, coneCosOuter, coneRange)) {
				frustumFlags |= Tri_Invisible;
				continue;
			}
		}
		// Per-cube-face cull. Same shape of test as the spot-cone cull,
		// but the "cone" is the 90°-FOV pyramid of one of 6 cube faces.
		// No T->Flags write — multiple cube-face xform tasks run in
		// parallel on the same TriMesh; flipping a shared bit would race
		// (each task wants a per-face decision, not a global one).
		if (cubeFaceCull) {
			Vector wsBsphereCtr;
			MatrixXVector(T->RotMat, &T->BSphereCtr, &wsBsphereCtr);
			Vector_SelfAdd(&wsBsphereCtr, &T->IPos);
			// cubeFaceCos = 0.577 by construction; use the constant-
			// folded variant to skip the sqrt + divide per call.
			if (sphereOutsidePyramidCone(wsBsphereCtr, T->BSphereRadius,
			                             cubeFacePos, cubeFaceDir, cubeFaceRange)) {
				continue;
			}
		}

		MatrixXMatrix(cam.view->Mat,T->RotMat,M);
		Matrix_Copy(IM,M);
		// Advanced Matrix...(watch this)
		Vector_Scale(W,PX,W);
		Vector_Scale(W+1,-PY,W+1);
		Vector_Scale(W+2,cam.cntrEX,&V);
		Vector_SelfAdd(W,&V);
		Vector_Scale(W+2,cam.cntrEY,&V);
		Vector_SelfAdd(W+1,&V);
		// Supermatrix ready.
		
		// postrioric Offset Vector.
		Vector_Sub(&T->IPos,&cam.view->ISource,&U);
		MatrixXVector(cam.view->Mat,&U,&S);

		V.x = cam.cntrEX*S.z+PX*S.x;
		V.y = cam.cntrEY*S.z-PY*S.y;
		V.z = S.z;

		// make a corrected sphere center vector
		MatrixXVector(IM,&T->BSphereCtr,&AP);
		Vector_SelfAdd(&S,&AP);
		// Cache the view-space z and radius for the transparent sort's
		// object-level grouping (used in the Mat_Transparent branch below).
		const float objBSphereViewZ = S.z;
		const float objBSphereRadius = T->BSphereRadius;

		Vector_Copy(&OS, &S);


		//    Vector_Copy(&V,&S);
		// ready
		// 4x3 AFFINE XFORM
		M34[0][0] = M[0][0]; M34[0][1] = M[0][1]; M34[0][2] = M[0][2]; M34[0][3] = V.x;
		M34[1][0] = M[1][0]; M34[1][1] = M[1][1]; M34[1][2] = M[1][2]; M34[1][3] = V.y;
		M34[2][0] = M[2][0]; M34[2][1] = M[2][1]; M34[2][2] = M[2][2]; M34[2][3] = V.z;
		// ready

		// Column-major SIMD staging for the per-vertex M34 * (Pos, 1).
		// Each column is loaded once into a Vec4f; per-vertex compute
		// becomes 3 broadcast-FMAs (vfmaq_n_f32-equivalent) instead of
		// the 9 scalar muls + 9 scalar adds the row-major form requires.
		// 4th lane is unused (kept 0); it falls out when storing TPos.
		alignas(16) const float m34_col_x_arr[4] = { M[0][0], M[1][0], M[2][0], 0.0f };
		alignas(16) const float m34_col_y_arr[4] = { M[0][1], M[1][1], M[2][1], 0.0f };
		alignas(16) const float m34_col_z_arr[4] = { M[0][2], M[1][2], M[2][2], 0.0f };
		alignas(16) const float m34_col_w_arr[4] = { V.x,     V.y,     V.z,     0.0f };
		const Vec4f m34_col_x = Vec4f().load_a(m34_col_x_arr);
		const Vec4f m34_col_y = Vec4f().load_a(m34_col_y_arr);
		const Vec4f m34_col_z = Vec4f().load_a(m34_col_z_arr);
		const Vec4f m34_col_w = Vec4f().load_a(m34_col_w_arr);
		// IM is 3x3 (no translation). Used by the !_inShadowPass branch to
		// transform N and Tangent into view space.
		alignas(16) const float im_col_x_arr[4] = { IM[0][0], IM[1][0], IM[2][0], 0.0f };
		alignas(16) const float im_col_y_arr[4] = { IM[0][1], IM[1][1], IM[2][1], 0.0f };
		alignas(16) const float im_col_z_arr[4] = { IM[0][2], IM[1][2], IM[2][2], 0.0f };
		const Vec4f im_col_x = Vec4f().load_a(im_col_x_arr);
		const Vec4f im_col_y = Vec4f().load_a(im_col_y_arr);
		const Vec4f im_col_z = Vec4f().load_a(im_col_z_arr);
		
		
		// aprioric Offset Vector.
		MatrixTXVector(T->RotMat,&U,&AP);
		Vector *WP = (Vector *)T->RotMat;
		Vector_SelfScale(&AP, 1.0/Vector_SelfDot(WP));
		// ready
		// Bounding Sphere Elimination test Begins.
		W2 = (Vector *)(&T->RotMat);
		L2 = Dot_Product(W2,W2);
		if ((L1 = Dot_Product(W2+1,W2+1))>L2) L2=L1;
		if ((L1 = Dot_Product(W2+2,W2+2))>L2) L2=L1;
		
		frustumFlags = 0;

		frustumFlags |= Tri_Inside;

		// Out by depth
		dz = S.z - cam.nearZ;
		if (dz*dz>L2*T->BSphereRad)
		{
			if (dz<0.0f)
			{
				frustumFlags |= Tri_Invisible;
				continue;
			}
			frustumFlags |= Tri_Ahead;
		} else {
			frustumFlags &=~Tri_Inside;
		}

		dz = S.z - cam.farZ;
		if (dz*dz>L2*T->BSphereRad)
		{
			if (dz>0.0f)
			{
				frustumFlags |= Tri_Invisible;
				continue;
			}
		} else {
			frustumFlags &=~Tri_Inside;
		}
		// Out by left/right
		S.x=fabs(S.x);
		L1 = PX*S.x - cam.cntrEX*S.z;
		if (L1*L1>L2*T->BSphereRad*(PX*PX+cam.cntrEX*cam.cntrEX))
		{
			if (S.x*PX>S.z*cam.cntrEX)
			{
				frustumFlags |= Tri_Invisible;
				continue;
			}
		} else {
			if (frustumFlags&Tri_Ahead) frustumFlags &=~Tri_Inside;
		}
		// Out by up/down
		S.y = fabs(S.y);
		L1 = PY*S.y - cam.cntrEY*S.z;
		if (L1*L1>L2*T->BSphereRad*(PY*PY+cam.cntrEY*cam.cntrEY))
		{
			if (S.y*PY>S.z*cam.cntrEY)
			{
				frustumFlags |= Tri_Invisible;
				continue;
			}
		} else {
			if (frustumFlags&Tri_Ahead) frustumFlags &=~Tri_Inside;
		}
		VEnd=tVerts+T->VIndex;
		
		/*    FEnd=T->Face+T->NumOfFaces;
		for (F=T->Face;F<FEnd;F++)
		if (!(F->Txtr->Flags&Mat_TwoSided))
        F->Flags = (AP.x*F->N.x + AP.y*F->N.y + AP.z*F->N.z>=F->NormProd);*/
		
		Vector *BSC = &T->BSphereScreenPos;

		//BSC->x = M34[0][0] * V.x + M34[0][1] * V.y + M34[0][2] * V.z + M34[0][3];
		//BSC->y = M34[1][0] * V.x + M34[1][1] * V.y + M34[1][2] * V.z + M34[1][3];
		//BSC->z = M34[2][0] * V.x + M34[2][1] * V.y + M34[2][2] * V.z + M34[2][3];
		BSC->x = V.x;
		BSC->y = V.y;
		BSC->z = V.z;

		BSC->x /= BSC->z;
		BSC->y /= BSC->z;
		// Alternate vertex loop for cube-face shadow xform: when the mesh
		// has a pre-computed world-space vertex cache, do a per-vertex
		// pyramid test in world space and skip the view matmul for
		// vertices that fall outside the face frustum. The mesh-level
		// cube cull (cubeFaceCull above) already rejected meshes whose
		// whole bsphere is outside; this is for meshes that straddle a
		// face boundary (e.g. greets's split Piramid chunks). Out-of-
		// pyramid vertices get TPos = (out-of-frame, out-of-frame, +1):
		// face submission later AND's the per-vertex Vtx_Visible bits,
		// so a face whose 3 vertices all share an out direction culls
		// cleanly. Faces straddling the boundary still process normally.
		if (cubeFaceCull && T->worldVerts
		    && fds::FeatureFlags::shadow_cube_vert_cull()) {
			// Circumscribed cone (matches mesh-level cull): half-angle
			// 54.7°, tan²(54.7°) = 2. So perp² > 2 × axisDist² → outside.
			constexpr float kTan2 = 2.0f;
			const float ckX = cubeFacePos.x, ckY = cubeFacePos.y, ckZ = cubeFacePos.z;
			const float cdX = cubeFaceDir.x, cdY = cubeFaceDir.y, cdZ = cubeFaceDir.z;
			// View transform reads `cam.view->Mat` directly (no perspective
			// pre-mul; we'll apply PX/PY/cntr scaling here, matching what
			// M34 × Pos produces in the legacy path).
			const float (*VM)[3] = cam.view->Mat;
			for (Vtx = tVerts; Vtx < VEnd; Vtx++) {
				const DWord vi = DWord(Vtx - tVerts);
				const Vector &wp = T->worldVerts[vi];
				const float wdx = wp.x - ckX;
				const float wdy = wp.y - ckY;
				const float wdz = wp.z - ckZ;
				const float axisDist = wdx*cdX + wdy*cdY + wdz*cdZ;
				const float d2 = wdx*wdx + wdy*wdy + wdz*wdz;
				const bool outside = (axisDist < 0.0f)
				    || ((d2 - axisDist*axisDist) > kTan2 * axisDist*axisDist);
				if (outside) {
					// Mark all-out so face cull eats the straddler-edge
					// faces whose 3 verts all agreed on being out.
					Vtx->TPos.x = 0.0f;
					Vtx->TPos.y = 0.0f;
					Vtx->TPos.z = 1.0f;
					Vtx->RZ = 1.0f;
					Vtx->PX = -1.0f;
					Vtx->PY = -1.0f;
					Vtx->UZ = 0.0f;
					Vtx->VZ = 0.0f;
					Vtx->Flags |= Vtx_Visible;  // all 6 frustum-out bits set
					continue;
				}
				// In-pyramid: full view xform + perspective scaling.
				// view_* = view.Mat · (worldPos - cam.ISource). Cube-face
				// camera's ISource == omni IPos == cubeFacePos, so wdX/Y/Z
				// already equal world delta from camera.
				const float vx = VM[0][0]*wdx + VM[0][1]*wdy + VM[0][2]*wdz;
				const float vy = VM[1][0]*wdx + VM[1][1]*wdy + VM[1][2]*wdz;
				const float vz = VM[2][0]*wdx + VM[2][1]*wdy + VM[2][2]*wdz;
				Vtx->TPos.x = PX * vx + cam.cntrEX * vz;
				Vtx->TPos.y = -PY * vy + cam.cntrEY * vz;
				Vtx->TPos.z = vz;
				Vtx->Flags &= ~Vtx_Visible;
				Vtx->RZ = 1.0f / vz;
				Vtx->PX = Vtx->TPos.x * Vtx->RZ;
				Vtx->PY = Vtx->TPos.y * Vtx->RZ;
				Vtx->UZ = Vtx->U * Vtx->RZ;
				Vtx->VZ = Vtx->V * Vtx->RZ;
				if (Vtx->PX < 0.0f) Vtx->Flags |= Vtx_VisLeft;
				if (Vtx->PX >= float(xr)) Vtx->Flags |= Vtx_VisRight;
				if (Vtx->PY < 0.0f) Vtx->Flags |= Vtx_VisUp;
				if (Vtx->PY >= float(yr)) Vtx->Flags |= Vtx_VisDown;
			}
			goto AfterXForm;
		}
		//    Main vertex loop,in case no restrictions apply.
		if (!(T->Flags&Tri_Phong))
		{
			if (!(frustumFlags&Tri_Inside))
			{
				if (!(frustumFlags&Tri_Ahead))
					goto Regular;
				else goto Ahead;
			}
			// Intel inside...this rulez,all object completely inside frustrum.
			// SIMD per-vertex transform via column-major matrices (see
			// staging block above the Inside/Ahead/Regular dispatch).
			// Each broadcast-FMA collapses what was 3 scalar muls + 3
			// scalar adds into one Vec4f op; the 4th lane is unused.
			for (Vtx=tVerts;Vtx<VEnd;Vtx++)
			{
				const float vpx = Vtx->Pos.x, vpy = Vtx->Pos.y, vpz = Vtx->Pos.z;
				// Explicit mul_add chain so the compiler emits FMLA
				// instead of separate vmul+vadd. clang without
				// -ffp-contract=fast won't fuse `a + b*c` written as
				// a normal expression.
				Vec4f tpos = mul_add(m34_col_x, Vec4f(vpx), m34_col_w);
				tpos       = mul_add(m34_col_y, Vec4f(vpy), tpos);
				tpos       = mul_add(m34_col_z, Vec4f(vpz), tpos);
				alignas(16) float tposArr[4];
				tpos.store_a(tposArr);
				Vtx->TPos.x = tposArr[0];
				Vtx->TPos.y = tposArr[1];
				Vtx->TPos.z = tposArr[2];
				if (!_inShadowPass) {
					const float nx = Vtx->N.x, ny = Vtx->N.y, nz = Vtx->N.z;
					Vec4f tn = im_col_x * Vec4f(nx);
					tn       = mul_add(im_col_y, Vec4f(ny), tn);
					tn       = mul_add(im_col_z, Vec4f(nz), tn);
					alignas(16) float tnArr[4];
					tn.store_a(tnArr);
					Vtx->TN.x = tnArr[0]; Vtx->TN.y = tnArr[1]; Vtx->TN.z = tnArr[2];
					const float gx = Vtx->Tangent.x, gy = Vtx->Tangent.y, gz = Vtx->Tangent.z;
					Vec4f tt = im_col_x * Vec4f(gx);
					tt       = mul_add(im_col_y, Vec4f(gy), tt);
					tt       = mul_add(im_col_z, Vec4f(gz), tt);
					alignas(16) float ttArr[4];
					tt.store_a(ttArr);
					Vtx->TTangent.x = ttArr[0]; Vtx->TTangent.y = ttArr[1]; Vtx->TTangent.z = ttArr[2];
				}

				Vtx->Flags &= ~Vtx_Visible;
				const float rz = 1.0f / tposArr[2];
				Vtx->RZ = rz;
				Vtx->PX = tposArr[0] * rz;
				Vtx->PY = tposArr[1] * rz;
				Vtx->UZ = Vtx->U * rz;
				Vtx->VZ = Vtx->V * rz;
			}
			
			goto AfterXForm;
			// This is in case 100% of trimesh AHEAD of camera. this saves some chks
Ahead://Vertex_Loop1(T->Vertex,VEnd,M,&V);
			for (Vtx=tVerts;Vtx<VEnd;Vtx++)
			{
				// SIMD matrix prefix (see Inside path for the column-major
				// staging). Stores TPos via lane-extracts to avoid touching
				// N.x (next field) with the unused 4th lane.
				const float vpx = Vtx->Pos.x, vpy = Vtx->Pos.y, vpz = Vtx->Pos.z;
				Vec4f tpos = mul_add(m34_col_x, Vec4f(vpx), m34_col_w);
				tpos       = mul_add(m34_col_y, Vec4f(vpy), tpos);
				tpos       = mul_add(m34_col_z, Vec4f(vpz), tpos);
				alignas(16) float tposArr[4];
				tpos.store_a(tposArr);
				Vtx->TPos.x = tposArr[0];
				Vtx->TPos.y = tposArr[1];
				Vtx->TPos.z = tposArr[2];
				if (!_inShadowPass) {
					const float nx = Vtx->N.x, ny = Vtx->N.y, nz = Vtx->N.z;
					Vec4f tn = im_col_x * Vec4f(nx);
					tn       = mul_add(im_col_y, Vec4f(ny), tn);
					tn       = mul_add(im_col_z, Vec4f(nz), tn);
					alignas(16) float tnArr[4];
					tn.store_a(tnArr);
					Vtx->TN.x = tnArr[0]; Vtx->TN.y = tnArr[1]; Vtx->TN.z = tnArr[2];
					const float gx = Vtx->Tangent.x, gy = Vtx->Tangent.y, gz = Vtx->Tangent.z;
					Vec4f tt = im_col_x * Vec4f(gx);
					tt       = mul_add(im_col_y, Vec4f(gy), tt);
					tt       = mul_add(im_col_z, Vec4f(gz), tt);
					alignas(16) float ttArr[4];
					tt.store_a(ttArr);
					Vtx->TTangent.x = ttArr[0]; Vtx->TTangent.y = ttArr[1]; Vtx->TTangent.z = ttArr[2];
				}

				Vtx->Flags&=0xFFFFFFFF-Vtx_Visible;
				// Ahead path: BSphere is theoretically fully past NZP, but
				// float precision can let individual verts slip below.
				// Defensively flag those so the clipper's Near() handles
				// them (otherwise RZ would go negative and PX/PY would be
				// flipped, producing ghost polygons at the cone edges).
				if (Vtx->TPos.z > cam.nearZ) {
					Vtx->RZ=1.0/Vtx->TPos.z;
					Vtx->PX=Vtx->TPos.x*Vtx->RZ;
					Vtx->PY=Vtx->TPos.y*Vtx->RZ;
					Vtx->UZ=Vtx->U*Vtx->RZ;
					Vtx->VZ=Vtx->V*Vtx->RZ;
					if (Vtx->PX<0) Vtx->Flags|=Vtx_VisLeft;
					if (Vtx->PX>=xr) Vtx->Flags|=Vtx_VisRight;
					if (Vtx->PY<0) Vtx->Flags|=Vtx_VisUp;
					if (Vtx->PY>=yr) Vtx->Flags|=Vtx_VisDown;
					if (Vtx->TPos.z>cam.farZ) Vtx->Flags|=Vtx_VisFar;
				} else {
					Vtx->Flags|=Vtx_VisNear;
				}
			}
			//    printf("Ahead VGA/Wizard.\n");
			goto AfterXForm;
Regular:
			for (Vtx=tVerts;Vtx<VEnd;Vtx++)
			{
				// SIMD matrix prefix (see Inside path for the staging).
				const float vpx = Vtx->Pos.x, vpy = Vtx->Pos.y, vpz = Vtx->Pos.z;
				Vec4f tpos = mul_add(m34_col_x, Vec4f(vpx), m34_col_w);
				tpos       = mul_add(m34_col_y, Vec4f(vpy), tpos);
				tpos       = mul_add(m34_col_z, Vec4f(vpz), tpos);
				alignas(16) float tposArr[4];
				tpos.store_a(tposArr);
				Vtx->TPos.x = tposArr[0];
				Vtx->TPos.y = tposArr[1];
				Vtx->TPos.z = tposArr[2];
				if (!_inShadowPass) {
					const float nx = Vtx->N.x, ny = Vtx->N.y, nz = Vtx->N.z;
					Vec4f tn = im_col_x * Vec4f(nx);
					tn       = mul_add(im_col_y, Vec4f(ny), tn);
					tn       = mul_add(im_col_z, Vec4f(nz), tn);
					alignas(16) float tnArr[4];
					tn.store_a(tnArr);
					Vtx->TN.x = tnArr[0]; Vtx->TN.y = tnArr[1]; Vtx->TN.z = tnArr[2];
					const float gx = Vtx->Tangent.x, gy = Vtx->Tangent.y, gz = Vtx->Tangent.z;
					Vec4f tt = im_col_x * Vec4f(gx);
					tt       = mul_add(im_col_y, Vec4f(gy), tt);
					tt       = mul_add(im_col_z, Vec4f(gz), tt);
					alignas(16) float ttArr[4];
					tt.store_a(ttArr);
					Vtx->TTangent.x = ttArr[0]; Vtx->TTangent.y = ttArr[1]; Vtx->TTangent.z = ttArr[2];
				}

				Vtx->Flags&=0xFFFFFFFF-Vtx_Visible;
				//      if (*(int32_t *)(&Vtx->TPos.z)>0x3F800000) // 1.0 in floating point rep.
				if (Vtx->TPos.z>cam.nearZ)
				{
					Vtx->RZ=1.0/Vtx->TPos.z;
					Vtx->PX=Vtx->TPos.x*Vtx->RZ;
					Vtx->PY=Vtx->TPos.y*Vtx->RZ;
					//          Vtx->PX=cam.cntrEX+PX*Vtx->TPos.x*Vtx->RZ;
					//          Vtx->PY=cam.cntrEY-PY*Vtx->TPos.y*Vtx->RZ;
					Vtx->UZ=Vtx->U*Vtx->RZ;
					Vtx->VZ=Vtx->V*Vtx->RZ;
					if (Vtx->PX<0) Vtx->Flags|=Vtx_VisLeft;
					if (Vtx->PX>=xr) Vtx->Flags|=Vtx_VisRight;
					if (Vtx->PY<0) Vtx->Flags|=Vtx_VisUp;
					if (Vtx->PY>=yr) Vtx->Flags|=Vtx_VisDown;
					if (Vtx->TPos.z>cam.farZ) Vtx->Flags|=Vtx_VisFar;
				} else Vtx->Flags|=Vtx_VisNear;
				//      printf("Regular shit!\n");
			}
		} else {
			// instead of all of these complications, I've decided to
			// make the face have void (*Clipper), that will do whatever it needs
			// in one call. the pre-filler will call the asm rasterizers twice
			// if necessary. back to the good old Avatar engine techniques ;)
			// at this section, the code also calculates environment mapping
			// coordinates to (EU,EV) by rotating the v. normals accordingly. slow.
			if (!(frustumFlags&Tri_Inside))
			{
				if (!(frustumFlags&Tri_Ahead))
					goto ERegular;
				else goto EAhead;
			}
			// Intel inside...this rulez,all object completely inside frustrum.
			for (Vtx=tVerts;Vtx<VEnd;Vtx++)
			{
				MatrixXVector(M,&Vtx->Pos,&U);
				Vector_Add(&U,&V,&Vtx->TPos);
				
				Vtx->Flags=0;
				Vtx->RZ=1.0/Vtx->TPos.z;
				Vtx->PX=Vtx->TPos.x*Vtx->RZ;
				Vtx->PY=Vtx->TPos.y*Vtx->RZ;
				//        Vtx->PX=cam.cntrEX+PX*Vtx->TPos.x*Vtx->RZ;
				//        Vtx->PY=cam.cntrEY-PY*Vtx->TPos.y*Vtx->RZ;

				// Environment mapping support removed at 11.04.02
//				Vtx->EU=128.0+95.0*(Vtx->N.x*IM[0][0]+Vtx->N.y*IM[0][1]+Vtx->N.z*IM[0][2]);
//				Vtx->REU=Vtx->EU*Vtx->RZ;
//				Vtx->EV=128.0+95.0*(Vtx->N.x*IM[1][0]+Vtx->N.y*IM[1][1]+Vtx->N.z*IM[1][2]);
//				Vtx->REV=Vtx->EV*Vtx->RZ;
				Vtx->UZ=Vtx->U*Vtx->RZ;
				Vtx->VZ=Vtx->V*Vtx->RZ;
				//if (Vtx->TPos.z>cam.farZ) Vtx->Flags|=Vtx_VisFar;
			}
			goto AfterXForm;
			// This is in case 100% of trimesh AHEAD of camera. this saves some chks
EAhead://Vertex_Loop1(T->Vertex,VEnd,M,&V);
			for (Vtx=tVerts;Vtx<VEnd;Vtx++)
			{
				//    if (!Vtx->FRem) continue;
				MatrixXVector(M,&Vtx->Pos,&U);
				Vector_Add(&U,&V,&Vtx->TPos);
				
				Vtx->RZ=1.0/Vtx->TPos.z;

				// Environment mapping support removed at 11.04.02
//				Vtx->EU=128.0+95.0*(Vtx->N.x*IM[0][0]+Vtx->N.y*IM[0][1]+Vtx->N.z*IM[0][2]);
//				Vtx->REU=Vtx->EU*Vtx->RZ;
//				Vtx->EV=128.0+95.0*(Vtx->N.x*IM[1][0]+Vtx->N.y*IM[1][1]+Vtx->N.z*IM[1][2]);
//				Vtx->REV=Vtx->EV*Vtx->RZ;
				
				Vtx->PX=Vtx->TPos.x*Vtx->RZ;
				Vtx->PY=Vtx->TPos.y*Vtx->RZ;
				//        Vtx->PX=cam.cntrEX+PX*Vtx->TPos.x*Vtx->RZ;
				//        Vtx->PY=cam.cntrEY-PY*Vtx->TPos.y*Vtx->RZ;
				Vtx->UZ=Vtx->U*Vtx->RZ;
				Vtx->VZ=Vtx->V*Vtx->RZ;
				if (Vtx->PX<0) Vtx->Flags=Vtx_VisLeft; else Vtx->Flags=0;
				if (Vtx->PX>=xr) Vtx->Flags+=Vtx_VisRight;
				if (Vtx->PY<0) Vtx->Flags+=Vtx_VisUp;
				if (Vtx->PY>=yr) Vtx->Flags+=Vtx_VisDown;
				if (Vtx->TPos.z>cam.farZ) Vtx->Flags|=Vtx_VisFar;
			}
			//    printf("Ahead VGA/Wizard.\n");
			
			goto AfterXForm;
ERegular:
			for (Vtx=tVerts;Vtx<VEnd;Vtx++)
			{
				//    if (!Vtx->FRem) continue;
				MatrixXVector(M,&Vtx->Pos,&U);
				Vector_Add(&U,&V,&Vtx->TPos);
				
				Vtx->Flags = 0;
				//      if (*(int32_t *)(&Vtx->TPos.z)>0x3F800000) // 1.0 in floating point rep.

				// Environment mapping support removed at 11.04.02
//				Vtx->EU=128.0+95.0*(Vtx->N.x*IM[0][0]+Vtx->N.y*IM[0][1]+Vtx->N.z*IM[0][2]);
//				Vtx->EV=128.0+95.0*(Vtx->N.x*IM[1][0]+Vtx->N.y*IM[1][1]+Vtx->N.z*IM[1][2]);
				
				if (Vtx->TPos.z>cam.nearZ)
				{
					Vtx->RZ=1.0/Vtx->TPos.z;
					Vtx->PX=Vtx->TPos.x*Vtx->RZ;
					Vtx->PY=Vtx->TPos.y*Vtx->RZ;
					//          Vtx->PX=cam.cntrEX+PX*Vtx->TPos.x*Vtx->RZ;
					//          Vtx->PY=cam.cntrEY-PY*Vtx->TPos.y*Vtx->RZ;
					Vtx->UZ=Vtx->U*Vtx->RZ;
					Vtx->VZ=Vtx->V*Vtx->RZ;
//					Vtx->REU=Vtx->EU*Vtx->RZ;
//					Vtx->REV=Vtx->EV*Vtx->RZ;
					if (Vtx->PX<0) Vtx->Flags=Vtx_VisLeft;
					if (Vtx->PX>=xr) Vtx->Flags+=Vtx_VisRight;
					if (Vtx->PY<0) Vtx->Flags+=Vtx_VisUp;
					if (Vtx->PY>=yr) Vtx->Flags+=Vtx_VisDown;
					if (Vtx->TPos.z>cam.farZ) Vtx->Flags|=Vtx_VisFar;
				} else Vtx->Flags=Vtx_VisNear;
				
				//      printf("Regular shit!\n");
			}
			
		}
AfterXForm:FEnd=tFaces+T->FIndex;
		// SoA refactor Phase 1: dual-write the transformed-vertex
		// outputs into the per-mesh VertexFrame SoA arrays. Only on
		// the main render pass (scratch == nullptr) — shadow per-light
		// passes use cloned vertices that aren't aliased into T->frame.
		// One sequential sweep over tVerts; the AoS layout we're
		// reading from is cache-friendly (sequential reads at pack(1)
		// stride). Phase 2 will eliminate this sweep by writing SoA
		// directly from the wide-SIMD compute path.
		if (!scratch) {
			if (!T->frame) T->frame = new VertexFrame();
			T->frame->ensureSized(int(T->VIndex));
			if (T->frame->capacity >= int(T->VIndex)) {
				VertexFrame *F_ = T->frame;
				const uint32_t nv = T->VIndex;
				for (uint32_t i = 0; i < nv; ++i) {
					const Vertex *v = &tVerts[i];
					F_->TPos_x[i]     = v->TPos.x;
					F_->TPos_y[i]     = v->TPos.y;
					F_->TPos_z[i]     = v->TPos.z;
					F_->TN_x[i]       = v->TN.x;
					F_->TN_y[i]       = v->TN.y;
					F_->TN_z[i]       = v->TN.z;
					F_->TTangent_x[i] = v->TTangent.x;
					F_->TTangent_y[i] = v->TTangent.y;
					F_->TTangent_z[i] = v->TTangent.z;
					F_->PX[i]         = v->PX;
					F_->PY[i]         = v->PY;
					F_->RZ[i]         = v->RZ;
					F_->UZ[i]         = v->UZ;
					F_->VZ[i]         = v->VZ;
					F_->EUZ[i]        = v->EUZ;
					F_->EVZ[i]        = v->EVZ;
					F_->Flags[i]      = v->Flags;
					F_->BGRA[i]       = v->BGRA;
				}
				// Verification gate — off by default; turn on with
				// --soa-verify during migration to catch any future
				// divergence between AoS and SoA paths bit-for-bit.
				if (fds::FeatureFlags::soa_verify()) {
					for (uint32_t i = 0; i < nv; ++i) {
						const Vertex *v = &tVerts[i];
						const bool ok =
							F_->TPos_x[i] == v->TPos.x && F_->TPos_y[i] == v->TPos.y && F_->TPos_z[i] == v->TPos.z &&
							F_->TN_x[i] == v->TN.x && F_->TN_y[i] == v->TN.y && F_->TN_z[i] == v->TN.z &&
							F_->PX[i] == v->PX && F_->PY[i] == v->PY && F_->RZ[i] == v->RZ &&
							F_->UZ[i] == v->UZ && F_->VZ[i] == v->VZ && F_->Flags[i] == v->Flags;
						if (!ok) {
							std::fprintf(stderr, "[SOA-VERIFY] mismatch vert %u mesh %p\n", i, (void*)T);
							std::abort();
						}
					}
				}
			}
		}
	// Runtime debug: hide specific nested-transparent objects (fountain's
	// f_sphere outer and "f in shpere" inner). Toggled by J / K keys —
	// useful for isolating which face contributes to a rendering bug.
	const bool hideInner = g_HideInnerXpar.load(std::memory_order_relaxed);
	const bool hideOuter = g_HideOuterXpar.load(std::memory_order_relaxed);
	// FDS_XPAR_FORCE_TWOSIDED=1 treats the nested-sphere materials as
	// TwoSided regardless of their actual flag. The fountain meshes ship
	// single-sided, so without this we only see their camera-facing half
	// — which makes "inner is hidden by outer" worse since the inner's
	// far half doesn't even render. Quick experiment to gauge whether
	// the deferred path's missing-inner-objects symptom improves when
	// both halves are present.
	const bool forceXparTwoSided = fds::FeatureFlags::xpar_force_twosided();
	// Per-face xpar sort flags hoisted to per-mesh — the registry read
	// is a memory load + offset and called once per xpar face was ~25
	// leaf samples across greets' xpar walls.
	const bool xparFrontBackDisabled = fds::FeatureFlags::no_xpar_frontback();
	const bool xparObjGroupDisabled  = fds::FeatureFlags::no_xpar_objgroup();
	// Shadow pass: treat all faces as two-sided. Single-quad walls have
	// no back-side polygon to take over when the light's on their "back",
	// yet they still occlude light. Backface-culling for shadow rendering
	// is geometry-dependent (correct for sealed convex meshes, wrong for
	// single-sided walls/sheets) — easier to just skip it.
	extern thread_local bool g_inShadowPass;
	const bool shadowNoBackface = g_inShadowPass && !fds::FeatureFlags::shadow_backface_cull();
	for (F=tFaces;F<FEnd;F++) {
		if ((hideInner || hideOuter) && F->Txtr && F->Txtr->Name) {
			const char* mn = F->Txtr->Name;
			if (hideInner && std::strstr(mn, "in shpere")) continue;
			if (hideOuter && std::strcmp(mn, "f_sphere") == 0) continue;
		}
		const bool forceTS = forceXparTwoSided && F->Txtr && F->Txtr->Name &&
			(std::strstr(F->Txtr->Name, "in shpere") ||
			 std::strcmp(F->Txtr->Name, "f_sphere") == 0);
		if ((!F->VisibilityFlagsAll())
			&&(forceTS
			|| shadowNoBackface
			||(F->Txtr->Flags&Mat_TwoSided)
			||(AP.x*F->N.x + AP.y*F->N.y + AP.z*F->N.z<F->NormProd) // Backface culling
			//||(1) // no backface culling
			))
		{
			if (0 != (F->Flags & Face_Reflective)) {
				// clobber U1, V1, etc. with the equilateral-whatever coordinates matching
				// the direction from camera to the specific vertex, reflected on the face's plane
				float eu[3];
				float ev[3];
				size_t i = 0;
				Vector wsPos[3];

				for (Vertex* v : { F->A, F->B, F->C }) {
					wsPos[i] = T->RotMat * v->Pos + T->IPos;
					++i;
				}

				// auto cv = (T->BSphereCtr - cam.view->ISource) * 0.9 + cam.view->ISource;
				auto cv = cam.view->ISource;
				float optimalDistFromPlane = fabs(T->BSphereCtr * F->N - F->NormProd);
				float viewDistFromPlane = fabs(AP * F->N - F->NormProd);
				if (viewDistFromPlane > optimalDistFromPlane) {
					float hackDistFromPlane = pow(viewDistFromPlane - optimalDistFromPlane, 0.8) + optimalDistFromPlane;
					Vector bsWorldPos;
					MatrixXVector(T->RotMat, &T->BSphereCtr, &bsWorldPos);
					bsWorldPos += T->IPos;

					auto pullDir = bsWorldPos - cam.view->ISource;
					float step = pullDir * F->N;
					// Skip the cv pull when pullDir is nearly parallel to
					// the face plane (step → 0). Dividing by a tiny step
					// otherwise blows cv out to infinity → wildly wrong
					// reflections that swing with small camera motions
					// (see [[project_cv_pull_instability]]).
					// Threshold: 0.1 × |pullDir| corresponds to ~5.7° tilt
					// between pullDir and the plane. Below that the pull
					// is geometrically ill-conditioned anyway — leaving
					// cv = camera ISource gives a slightly-wider FOV than
					// authored, which is a much less jarring artifact than
					// the swing.
					const float pullLen2 = pullDir * pullDir;
					if (step * step > 0.01f * pullLen2) {
						cv += (hackDistFromPlane - viewDistFromPlane) / step * pullDir;
					}
				}
				auto n = (wsPos[0] - wsPos[1]).cross(wsPos[2] - wsPos[1]);
				Vector_Norm(&n);
				i = 0;
				for (Vertex* v : { F->A, F->B, F->C }) {
					auto d = wsPos[i] - cv;
					d -= (d * n) * 2.0f * n;
					Vector_Norm(&d);
					// Equirectangular panorama lookup. The convention
					// matches CalcEquirectangularPanoramaTable's bake:
					//   eu=0    → -z scenery     eu=0.5  → +z scenery
					//   eu=0.25 → +x scenery     eu=0.75 → -x scenery
					//   ev=0    → +y scenery     ev=1    → -y scenery
					// so a reflected ray pointing in world direction d
					// resolves to (eu, ev) where the bake stored that
					// direction's content. Verified with --snapshot=
					// cuberefl (synthetic painter follows the same
					// convention).
					// Polynomial asin / atan2 — libm versions are
					// ~100-200 cycles each. The polynomial pair is
					// ~25 cycles total at ~0.001 rad max error, well
					// below the per-pixel panorama discretization for
					// city windows.
					float lat = asin_approx(d.y);
					float lon = atan2_approx(-d.z, -d.x);
					eu[i] = 0.5 + 0.5 * (lon + PI / 2.0) / PI;
					ev[i] = 0.5 - 0.5 * lat / (PI / 2.0);
					++i;
				}

				// U-wrapping
				if (std::max({ eu[0], eu[1], eu[2] }) - std::min({ eu[0], eu[1], eu[2] }) > 0.8) {
					for (int i = 0; i < 3; ++i) {
						if (eu[i] < 0.5) {
							eu[i] += 1;
						}
					}
				}

				F->EU1 = eu[0];
				F->EV1 = ev[0];
				F->EU2 = eu[1];
				F->EV2 = ev[1];
				F->EU3 = eu[2];
				F->EV3 = ev[2];
			}
			F->ParentTri = T;

#ifdef FRONT_TO_BACK_SORTING
			Material *M = F->Txtr;
//			mword sortid = 0;
//			if (M->Txtr)
//				sortid = M->Txtr->ID;

			// front-to-back sorting, also batches polygons that are all using 
			// the same texture.
			if (M->Flags & Mat_Transparent)
			{
				// Pass the array F was iterated from (tFaces — clone when
				// VertexScratch is in use, else T->Faces) so the prev/next
				// bounds check is correct. The CLONE has its own bounds.
				dz = QuadAwareMaxViewZ(F, tFaces, T->FIndex);
				const bool frontFacing = IsFrontFacingInViewSpace(F);

				// Object-level back-to-front grouping. Without this, nested
				// transparent meshes (fountain's f_sphere outer + "f in
				// shpere" inner, both concentric) interleave by per-face
				// max-z. Using the bsphere extent in view-space depth
				// instead:
				//
				//   BACK partition: extent = bsphere_z + radius (farthest
				//     surface). Larger extent → renders first → outer
				//     before inner.
				//   FRONT partition: extent = bsphere_z - radius (nearest
				//     surface). Larger extent → renders first → inner
				//     before outer (inner-front is farther from camera
				//     than outer-front in a concentric pair).
				//
				// Sort key layout (smaller renders first):
				//   bit-high: front-or-back partition (front gets 4*fzp
				//             offset, larger than any object/face score)
				//   bit-mid:  object score (2*fzp - extent, range 0..4*fzp)
				//   bit-low:  face fine sort (in [0, 1.0], used as fraction
				//             so adjacent faces of the same object still
				//             back-to-front)
				if (xparObjGroupDisabled) {
					// Legacy face-only sort, used for A/B comparison.
					if (dz > fzp) F->SortZ.F = fzp;
					else          F->SortZ.F = 2.0f * fzp - dz;
					if (!xparFrontBackDisabled && frontFacing)
						F->SortZ.F += 2.0f * fzp;
				} else {
					const float extent = frontFacing
						? (objBSphereViewZ - objBSphereRadius)
						: (objBSphereViewZ + objBSphereRadius);
					const float objScore = 2.0f * fzp - extent;
					const float faceFine = (dz > 0.0f && fzp > 0.0f)
						? std::max(0.0f, std::min(1.0f, (fzp - dz) / fzp))
						: 0.0f;
					float key = objScore + faceFine;
					if (!xparFrontBackDisabled && frontFacing)
						key += 4.0f * fzp;
					F->SortZ.F = key;
				}

//				F->SortZ.DW >>= 8;
//				F->SortZ.DW += 255 << 24;
			} else {
				dz = F->A->TPos.z;
				if (F->B->TPos.z>dz) dz=F->B->TPos.z;
				if (F->C->TPos.z>dz) dz=F->C->TPos.z;
				F->SortZ.F = dz;

//				F->SortZ.DW >>= 8;
//				F->SortZ.DW += sortid << 24;

			}

			static int32_t BiasedSortValues[] = {0, 0, (int32_t)0xFFFFFFFF};
			if (T->SortPriorityBias)
				F->SortZ.F = BiasedSortValues[T->SortPriorityBias];
#else
			dz = F->A->TPos.z;
			if (F->B->TPos.z>dz) dz=F->B->TPos.z;
			if (F->C->TPos.z>dz) dz=F->C->TPos.z;
			F->SortZ.F = fzp-dz;
#endif

			// Push AFTER SortZ is computed so FListEntry.sortKey
			// captures the final value (legacy layout could push
			// the Face* first since the radix sort dereffed back
			// through it; the new FListEntry has sortKey inline).
			*Ins++ = { F->SortZ.DW, F };
		}
	}  // close per-face loop body opened above
	}
	faces.cPolys = Ins-faces.fList;

	// Shadow pass (scratch != nullptr) skips omnis: this loop mutates
	// O->V.TPos / RZ / PX / PY in place on the source Omni (no clone
	// equivalent exists for omnis). Running it for each light would
	// leave omni screen positions stuck on the last light's camera,
	// breaking the flare draw on the main pass.
	FDW omniFlareSize;
	if (!scratch) for(O=Sc->OmniHead;O;O=O->Next)
	{


		Vtx=&O->V;
		Vector_Sub(&O->IPos,&cam.view->ISource,&V);
		//if (O->Flags & Omni_Rand) {
		//	Vector Rand;
		//	Rand.x = (frand() - 0.5) * 2.0f;
		//	Rand.y = (frand() - 0.5) * 2.0f;
		//	Rand.z = (frand() - 0.5) * 2.0f;
		//	Vector_Add(&V, &Rand, &V);
		//}
		MatrixXVector(cam.view->Mat,&V,&Vtx->TPos);
		if (Vtx->TPos.z>cam.nearZ&&Vtx->TPos.z<cam.farZ)
		{
			Vtx->RZ=1.0/Vtx->TPos.z;
			Vtx->PX=cam.cntrEX+Vtx->TPos.x*PX*Vtx->RZ;
			Vtx->PY=cam.cntrEY-Vtx->TPos.y*PY*Vtx->RZ;
			// Insert to List
			dz = Vtx->TPos.z;
			//dz *=-16384;
			//dz +=0x7FFFFFFF;
			//RoundToInt(&O->Face.SortZ.DW,dz);

			//omniFlareSize.F = O->ISize;
			//O->F.Flags = omniFlareSize.DW; // overwrite face flags with a pointer to the omnilight object
			O->F.FlareSize = (O->ISize);
			if (O->Flags & Omni_Rand) {
				O->F.FlareSize *= 1.0 + ((frand() - 0.5) * 0.2f);
			}
#ifdef FRONT_TO_BACK_SORTING
			O->F.SortZ.F = 2*fzp-dz;
#else
			O->F.SortZ.F = fzp-dz;
#endif			
			*Ins++ = { O->F.SortZ.DW, &O->F };
		}
	}

	faces.cOmnies = (Ins-faces.fList)-faces.cPolys;
#endif
	// Shadow pass skips particles for the same reason as omnis: the
	// projection writes to Sc->Pcl[I].V.* directly, no clone storage.
	if (!scratch) for (I = 0; I < Sc->NumOfParticles; I++) {
		Particle& p = Sc->Pcl[I];

		auto v = p.V.Pos - cam.view->ISource;
		
		p.V.TPos = cam.view->Mat * v;

		if (p.V.TPos.z >= cam.nearZ)
		{
			p.V.RZ = 1.0 / p.V.TPos.z;
			p.V.PX = cam.cntrX + cam.fovX * p.V.TPos.x * p.V.RZ;
			p.V.PY = cam.cntrY - cam.fovY * p.V.TPos.y * p.V.RZ;
			p.V.Flags = 0;
		} else {
			p.V.Flags |= Vtx_VisNear;
		}


		if (p.Flags & Particle_Active) {
			if ((dz = p.V.TPos.z) >= cam.nearZ) {
				if (p.TrailLength == 0) {
					F = &Sc->Pcl[I].F;
#ifdef FRONT_TO_BACK_SORTING
					F->SortZ.F = 2 * fzp - dz;
#else
					F->SortZ.F = fzp - dz;
#endif
					*Ins++ = { F->SortZ.DW, F };
				} else {
					addParticleTrail(Sc, Ins, p, cam);
				}
			}
		}
	}

	faces.cAll = Ins-faces.fList;
	faces.cPcls = faces.cAll-faces.cOmnies-faces.cPolys;
}
