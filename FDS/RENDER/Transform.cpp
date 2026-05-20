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

// Defined in RENDER.CPP. Forward-declare to avoid pulling the rest of
// RENDER.CPP's prelude in here.
float frand();

void Animate_Objects(Scene *Sc, bool SkipCameraAnimation)
{
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
	
	CalcPersp(View);
	FOVX = View->PerspX;
	FOVY = View->PerspY;
	
	
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
			MatrixXMatrix(*Obj->Parent->Rot,*Obj->Rot,M);
			Matrix_Copy(*Obj->Rot,M);
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
static float QuadAwareMaxViewZ(const Face* F, const TriMesh* T)
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

	const Face* prev = (F > T->Faces)                ? F - 1 : nullptr;
	const Face* next = (F + 1 < T->Faces + T->FIndex) ? F + 1 : nullptr;
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
	const bool coneCull = g_inShadowPass
		&& fds::FeatureFlags::shadow_cone_cull()
		&& g_currentShadowOmni
		&& g_currentShadowOmni->Type == Light_SpotLight;
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

		if (!(T->Flags&HTrack_Visible)) {T->Flags|=Tri_Invisible; continue;}

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
				T->Flags |= Tri_Invisible;
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
		
		T->Flags&=0xFFFFFFFF-Tri_Invisible-Tri_Ahead-Tri_Inside;
	
		T->Flags |= Tri_Inside;

		// Out by depth
		dz = S.z - cam.nearZ;
		if (dz*dz>L2*T->BSphereRad)
		{
			if (dz<0.0f)
			{
				T->Flags |= Tri_Invisible;
				continue;
			}
			T->Flags |= Tri_Ahead;
		} else {
			T->Flags &=~Tri_Inside;
		}
		
		dz = S.z - cam.farZ;
		if (dz*dz>L2*T->BSphereRad)
		{
			if (dz>0.0f)
			{
				T->Flags |= Tri_Invisible;
				continue;
			}
		} else {
			T->Flags &=~Tri_Inside;
		}
		// Out by left/right
		S.x=fabs(S.x);
		L1 = PX*S.x - cam.cntrEX*S.z;
		if (L1*L1>L2*T->BSphereRad*(PX*PX+cam.cntrEX*cam.cntrEX))
		{
			if (S.x*PX>S.z*cam.cntrEX)
			{
				T->Flags |= Tri_Invisible;
				continue;
			}			
		} else {
			if (T->Flags&Tri_Ahead) T->Flags &=~Tri_Inside;
		}
		// Out by up/down
		S.y = fabs(S.y);
		L1 = PY*S.y - cam.cntrEY*S.z;
		if (L1*L1>L2*T->BSphereRad*(PY*PY+cam.cntrEY*cam.cntrEY))
		{
			if (S.y*PY>S.z*cam.cntrEY)
			{
				T->Flags |= Tri_Invisible;
				continue;
			}
		} else {
			if (T->Flags&Tri_Ahead) T->Flags &=~Tri_Inside;
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
		//    Main vertex loop,in case no restrictions apply.
		if (!(T->Flags&Tri_Phong))
		{
			if (!(T->Flags&Tri_Inside))
			{
				if (!(T->Flags&Tri_Ahead))
					goto Regular;
				else goto Ahead;
			}
			// Intel inside...this rulez,all object completely inside frustrum.
			for (Vtx=tVerts;Vtx<VEnd;Vtx++)
			{
				//        MatrixXVector(M,&Vtx->Pos,&U);
				//        Vector_Add(&U,&V,&Vtx->TPos);
				// 4x3 xform
				Vtx->TPos.x = M34[0][0]*Vtx->Pos.x+M34[0][1]*Vtx->Pos.y+M34[0][2]*Vtx->Pos.z+M34[0][3];
				Vtx->TPos.y = M34[1][0]*Vtx->Pos.x+M34[1][1]*Vtx->Pos.y+M34[1][2]*Vtx->Pos.z+M34[1][3];
				Vtx->TPos.z = M34[2][0]*Vtx->Pos.x+M34[2][1]*Vtx->Pos.y+M34[2][2]*Vtx->Pos.z+M34[2][3];
				// Object-space N -> view-space TN. IM is the un-scaled
				// cam.view*RotMat (orthogonal rotation, so inverse-transpose
				// = same matrix). Read by the deferred-path clipper +
				// rasterizer; forward-path Lighting() still reads N.
				// Shadow pass doesn't read TN or TTangent — skip the two
				// matrix-vector mults to halve per-vertex cost there.
				if (!g_inShadowPass) {
					MatrixXVector(IM, &Vtx->N, &Vtx->TN);
					MatrixXVector(IM, &Vtx->Tangent, &Vtx->TTangent);
				}

				Vtx->Flags&=0xFFFFFFFF-Vtx_Visible;
				Vtx->RZ=1.0/Vtx->TPos.z;
				Vtx->PX=Vtx->TPos.x*Vtx->RZ;
				Vtx->PY=Vtx->TPos.y*Vtx->RZ;
				//        Vtx->PX=cam.cntrEX+PX*Vtx->TPos.x*Vtx->RZ;
				//        Vtx->PY=cam.cntrEY-PY*Vtx->TPos.y*Vtx->RZ;
				Vtx->UZ=Vtx->U*Vtx->RZ;
				Vtx->VZ=Vtx->V*Vtx->RZ;
				//if (Vtx->TPos.z>cam.farZ) Vtx->Flags|=Vtx_VisFar;
			}
			
			goto AfterXForm;
			// This is in case 100% of trimesh AHEAD of camera. this saves some chks
Ahead://Vertex_Loop1(T->Vertex,VEnd,M,&V);
			for (Vtx=tVerts;Vtx<VEnd;Vtx++)
			{
				//    if (!Vtx->FRem) continue;
				//        MatrixXVector(M,&Vtx->Pos,&U);
				//        Vector_Add(&U,&V,&Vtx->TPos);
				Vtx->TPos.x = M34[0][0]*Vtx->Pos.x+M34[0][1]*Vtx->Pos.y+M34[0][2]*Vtx->Pos.z+M34[0][3];
				Vtx->TPos.y = M34[1][0]*Vtx->Pos.x+M34[1][1]*Vtx->Pos.y+M34[1][2]*Vtx->Pos.z+M34[1][3];
				Vtx->TPos.z = M34[2][0]*Vtx->Pos.x+M34[2][1]*Vtx->Pos.y+M34[2][2]*Vtx->Pos.z+M34[2][3];
				// Shadow pass doesn't read TN or TTangent — skip the two
				// matrix-vector mults to halve per-vertex cost there.
				if (!g_inShadowPass) {
					MatrixXVector(IM, &Vtx->N, &Vtx->TN);
					MatrixXVector(IM, &Vtx->Tangent, &Vtx->TTangent);
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
				//    if (!Vtx->FRem) continue;
				//        MatrixXVector(M,&Vtx->Pos,&U);
				//        Vector_Add(&U,&V,&Vtx->TPos);
				Vtx->TPos.x = M34[0][0]*Vtx->Pos.x+M34[0][1]*Vtx->Pos.y+M34[0][2]*Vtx->Pos.z+M34[0][3];
				Vtx->TPos.y = M34[1][0]*Vtx->Pos.x+M34[1][1]*Vtx->Pos.y+M34[1][2]*Vtx->Pos.z+M34[1][3];
				Vtx->TPos.z = M34[2][0]*Vtx->Pos.x+M34[2][1]*Vtx->Pos.y+M34[2][2]*Vtx->Pos.z+M34[2][3];
				// Shadow pass doesn't read TN or TTangent — skip the two
				// matrix-vector mults to halve per-vertex cost there.
				if (!g_inShadowPass) {
					MatrixXVector(IM, &Vtx->N, &Vtx->TN);
					MatrixXVector(IM, &Vtx->Tangent, &Vtx->TTangent);
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
			if (!(T->Flags&Tri_Inside))
			{
				if (!(T->Flags&Tri_Ahead))
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
					float lat = asin(d.y);
					float lon = atan2(-d.z, -d.x);
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
				dz = QuadAwareMaxViewZ(F, T);
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
				const bool xparFrontBackDisabled = fds::FeatureFlags::no_xpar_frontback();
				const bool xparObjGroupDisabled = fds::FeatureFlags::no_xpar_objgroup();

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
