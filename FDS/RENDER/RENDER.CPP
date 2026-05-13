#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <memory.h>
#include <string.h>
//#include <conio.h>
#include <Base/Vector.h>
#include <memory>
#include <vector>
#include <algorithm>
#include <map>
#include <limits>
#include <chrono>
#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif
#include "simde/x86/fma.h"

#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "VESA/Vesa.h"
#include "FRUSTRUM.H"

//#define TRACE_OBJECTS

thread_local FrustumClipper clipper;


float frand() {
	return static_cast <float> (RAND_15()) / static_cast <float> (RAND_15_MAX);
}

// Front-to-back sorting used together with ZBuffer.
#define FRONT_TO_BACK_SORTING

//struct SceneContext {
//	std::unique_ptr<Scene>  CurScene;
//	std::vector<Face *>     FList;
//	std::vector<Face *>     SList;
//	Face                    *DoFace;
//	int32_t CPolys,COmnies,CPcls,CAll,Polys;
//	char Cam_HeadLight;
//	float CurFrame;
//	float g_zscale;
//	float g_zscale256;
//	int32_t g_FrameTime;
//	float Ambient_Factor;
//	float Diffusive_Factor;
//	float Specular_Factor;
//	float Range_Factor;
//};


Scene *CurScene;
Face **FList = nullptr;
Face **SList = nullptr;
//Face *DoFace; // mar gamish, ta'ase partsuf!
int32_t CPolys,COmnies,CPcls,CAll,Polys;
//char *TxtrBuf,*TxtrAlign;
//float Exec_FPS;
Material Default_Mat;
unsigned int OldFPUCW, FPUCW;

dword OldMXCSR, CurMXCSR;
char **MsgStr;
int32_t *MsgClock;
DWord *MsgID;
char Cam_HeadLight;
float CurFrame;
float g_zscale;
float g_zscale256;


int32_t g_FrameTime;

////////////////
// LIGHTING
float Ambient_Factor;
float Diffusive_Factor;
float Specular_Factor;
float Range_Factor;

int32_t Frames;
#ifndef __clang__
extern float dTime;
#endif

Texture Phong_Mapping;
Font *Font1;
char G_String[256];

VESA_Surface Layer2;

#include "GENERAL.H"
#include "SORTS.H"
#include <Threads.h>
#include <FILLERS/TheOtherBarry.h>
#include <FILLERS/Mekalele.h>
#include <FILLERS/ShadowMap.h>

// RULEZ
void DO_NOP()
{
}

Vector Transform_Vec(Vector Pos, Vector Source, Matrix Mat) {
	Vector U, V, S;

	// postrioric Offset Vector.
	Vector_Sub(&Pos, &Source, &U);
	MatrixXVector(Mat, &U, &S);
	V.x = CntrEX * S.z + FOVX * S.x;
	V.y = CntrEY * S.z - FOVY * S.y;
	V.z = S.z;

	//MatrixXVector(Mat, &V, &AP);
	//Vector_SelfAdd(&S, &AP);
	V.x /= V.z;
	V.y /= V.z;

	return V;
}

void Draw_Line_32Bit_ForKids(Vertex* V1, Vertex* V2);
void Draw_Line_32Bit(Vertex* V1, Vertex* V2);

void Bivex_Frustrum_Perspective(Vertex* A, Vertex* B, void (*Filler)(Vertex* V1, Vertex* V2));

void DrawSpline(Spline Pos, Vector CamISource, int EndFrame) {
	Vertex Last, Cur;
	for (auto frame = 0; frame < EndFrame; ++frame) {
		Vector IPos;
		Spline_Calc_3D(&Pos, frame, &IPos);
		IPos = Transform_Vec(IPos, CamISource, View->Mat);
		//auto pos = ((Fist(IPos.x * 4) + Fist(IPos.y) * VESA_BPSL));
		//if (pos < PageSize) {
		//	*(dword*)(VPage + pos) = 0xffffffff;
		//}
		Cur.PX = IPos.x;
		Cur.PY = IPos.y;
		if (frame != 0) {
			if (Last.PX < XRes-1 && Last.PY < YRes-1 && Last.PX > 0 && Last.PY > 0 &&
				Cur.PX < XRes-1 && Cur.PY < YRes-1 && Cur.PX > 0 && Cur.PY > 0) {

				Draw_Line_32Bit_ForKids(&Last, &Cur);
			}
		}
		Last.PX = Cur.PX;
		Last.PY = Cur.PY;
	}
}


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

void calcVisibilityFlags(Scene* Sc, Vertex* Vtx) {
	Vtx->Flags &= 0xFFFFFFFF - Vtx_Visible;
	//      if (*(int32_t *)(&Vtx->TPos.z)>0x3F800000) // 1.0 in floating point rep.
	if (Vtx->TPos.z > Sc->NZP) {
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
		if (Vtx->TPos.z > Sc->FZP) Vtx->Flags |= Vtx_VisFar;
	} else Vtx->Flags |= Vtx_VisNear;
}

void addParticleTrail(Scene* Sc, Face**& Ins /* Three star programming */, Particle& p) {
	Vector V;

	Vector VelDir = p.Vel;
	Vector_Norm(&VelDir);

	Vector src = p.V.Pos - p.TrailLength * VelDir;
	Vector targ = p.V.Pos;

	Vector d1 = (src - View->ISource).cross(targ - View->ISource);
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
		V = tmp + d1 * (u - 0.5f) * p.TrailWidth - View->ISource;
		MatrixXVector(View->Mat, &V, &A.TPos);

		A.TPos.x = A.TPos.z * CntrX + A.TPos.x * FOVX;
		A.TPos.y = A.TPos.z * CntrY - A.TPos.y * FOVY;
		A.RZ = 1.0f / A.TPos.z;
		A.PX = A.TPos.x * A.RZ;
		A.PY = A.TPos.y * A.RZ;

		A.LA = A.LR = A.LG = A.LB = 255.0;

		calcVisibilityFlags(Sc, &A);
		++quad;
	}

	for (size_t i = 0; i != 2; ++i) {
#ifdef FRONT_TO_BACK_SORTING
		p.TrailF[i].SortZ.F = 2 * Sc->FZP - p.V.TPos.z;
#else
		p.TrailF[i].SortZ.F = Sc->FZP - p.V.TPos.z;
#endif
	}
	*Ins++ = &p.TrailF[0];
	*Ins++ = &p.TrailF[1];
}

#define DEBUG_PARTICLES 0

// Per-face front-facing test using view-space vertex positions. Computes
// the triangle's face normal via edge cross-product, then sign-tests against
// vertex A's view-space position. Same test the deferred transparent
// classifier uses (see RenderInnerDeferredTransparent) — face-level, not
// per-vertex, so coplanar triangles of the same quad classify identically.
static bool IsFrontFacingInViewSpace(const Face* F)
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

	// Env-var escape hatch for A/B testing the sibling-unification fix.
	static const bool disabled = []{
		const char* s = std::getenv("FDS_NO_QUAD_SORT");
		return s && s[0] == '1';
	}();
	if (disabled) return dz;

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
void Transform_Objects(Scene *Sc, int xresOverride, int yresOverride)
{
	extern thread_local bool g_inShadowPass;
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
	float PX=FOVX,PY=FOVY,Temp;
	float dz;
	int32_t *pdz = (int32_t *)(&dz);
	int32_t I;
	Face **Ins = FList;
	float *f = (float *)(&M);
	float *fv;

	float fzp = Sc->FZP;

#if not(DEBUG_PARTICLES)
	Object *Obj; 
//	for (T=Sc->TriMeshHead;T;T=T->Next)
	for(Obj=Sc->ObjectHead; Obj; Obj=Obj->Next)
	{
		
		if (Obj->Type != Obj_TriMesh) continue;
		//if (stricmp(Obj->Name, "water.lwo")) continue;
		T = (TriMesh *)(Obj->Data);

		if (!(T->Flags&HTrack_Visible)) {T->Flags|=Tri_Invisible; continue;}
		MatrixXMatrix(View->Mat,T->RotMat,M);
		Matrix_Copy(IM,M);
		// Advanced Matrix...(watch this)
		Vector_Scale(W,PX,W);
		Vector_Scale(W+1,-PY,W+1);
		Vector_Scale(W+2,CntrEX,&V);
		Vector_SelfAdd(W,&V);
		Vector_Scale(W+2,CntrEY,&V);
		Vector_SelfAdd(W+1,&V);
		// Supermatrix ready.
		
		// postrioric Offset Vector.
		Vector_Sub(&T->IPos,&View->ISource,&U);
		MatrixXVector(View->Mat,&U,&S);

		V.x = CntrEX*S.z+PX*S.x;
		V.y = CntrEY*S.z-PY*S.y;
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
		dz = S.z - Sc->NZP;
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
		
		dz = S.z - Sc->FZP;
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
		L1 = PX*S.x - CntrEX*S.z;
		if (L1*L1>L2*T->BSphereRad*(PX*PX+CntrEX*CntrEX))
		{
			if (S.x*PX>S.z*CntrEX)
			{
				T->Flags |= Tri_Invisible;
				continue;
			}			
		} else {
			if (T->Flags&Tri_Ahead) T->Flags &=~Tri_Inside;
		}
		// Out by up/down
		S.y = fabs(S.y);
		L1 = PY*S.y - CntrEY*S.z;
		if (L1*L1>L2*T->BSphereRad*(PY*PY+CntrEY*CntrEY))
		{
			if (S.y*PY>S.z*CntrEY)
			{
				T->Flags |= Tri_Invisible;
				continue;
			}
		} else {
			if (T->Flags&Tri_Ahead) T->Flags &=~Tri_Inside;
		}
		VEnd=T->Verts+T->VIndex;
		
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
			for (Vtx=T->Verts;Vtx<VEnd;Vtx++)
			{
				//        MatrixXVector(M,&Vtx->Pos,&U);
				//        Vector_Add(&U,&V,&Vtx->TPos);
				// 4x3 xform
				Vtx->TPos.x = M34[0][0]*Vtx->Pos.x+M34[0][1]*Vtx->Pos.y+M34[0][2]*Vtx->Pos.z+M34[0][3];
				Vtx->TPos.y = M34[1][0]*Vtx->Pos.x+M34[1][1]*Vtx->Pos.y+M34[1][2]*Vtx->Pos.z+M34[1][3];
				Vtx->TPos.z = M34[2][0]*Vtx->Pos.x+M34[2][1]*Vtx->Pos.y+M34[2][2]*Vtx->Pos.z+M34[2][3];
				// Object-space N -> view-space TN. IM is the un-scaled
				// View*RotMat (orthogonal rotation, so inverse-transpose
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
				//        Vtx->PX=CntrEX+PX*Vtx->TPos.x*Vtx->RZ;
				//        Vtx->PY=CntrEY-PY*Vtx->TPos.y*Vtx->RZ;
				Vtx->UZ=Vtx->U*Vtx->RZ;
				Vtx->VZ=Vtx->V*Vtx->RZ;
				//if (Vtx->TPos.z>Sc->FZP) Vtx->Flags|=Vtx_VisFar;
			}
			
			goto AfterXForm;
			// This is in case 100% of trimesh AHEAD of camera. this saves some chks
Ahead://Vertex_Loop1(T->Vertex,VEnd,M,&V);
			for (Vtx=T->Verts;Vtx<VEnd;Vtx++)
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
				if (Vtx->TPos.z > Sc->NZP) {
					Vtx->RZ=1.0/Vtx->TPos.z;
					Vtx->PX=Vtx->TPos.x*Vtx->RZ;
					Vtx->PY=Vtx->TPos.y*Vtx->RZ;
					Vtx->UZ=Vtx->U*Vtx->RZ;
					Vtx->VZ=Vtx->V*Vtx->RZ;
					if (Vtx->PX<0) Vtx->Flags|=Vtx_VisLeft;
					if (Vtx->PX>=xr) Vtx->Flags|=Vtx_VisRight;
					if (Vtx->PY<0) Vtx->Flags|=Vtx_VisUp;
					if (Vtx->PY>=yr) Vtx->Flags|=Vtx_VisDown;
					if (Vtx->TPos.z>Sc->FZP) Vtx->Flags|=Vtx_VisFar;
				} else {
					Vtx->Flags|=Vtx_VisNear;
				}
			}
			//    printf("Ahead VGA/Wizard.\n");
			goto AfterXForm;
Regular:
			for (Vtx=T->Verts;Vtx<VEnd;Vtx++)
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
				if (Vtx->TPos.z>Sc->NZP)
				{
					Vtx->RZ=1.0/Vtx->TPos.z;
					Vtx->PX=Vtx->TPos.x*Vtx->RZ;
					Vtx->PY=Vtx->TPos.y*Vtx->RZ;
					//          Vtx->PX=CntrEX+PX*Vtx->TPos.x*Vtx->RZ;
					//          Vtx->PY=CntrEY-PY*Vtx->TPos.y*Vtx->RZ;
					Vtx->UZ=Vtx->U*Vtx->RZ;
					Vtx->VZ=Vtx->V*Vtx->RZ;
					if (Vtx->PX<0) Vtx->Flags|=Vtx_VisLeft;
					if (Vtx->PX>=xr) Vtx->Flags|=Vtx_VisRight;
					if (Vtx->PY<0) Vtx->Flags|=Vtx_VisUp;
					if (Vtx->PY>=yr) Vtx->Flags|=Vtx_VisDown;
					if (Vtx->TPos.z>Sc->FZP) Vtx->Flags|=Vtx_VisFar;
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
			for (Vtx=T->Verts;Vtx<VEnd;Vtx++)
			{
				MatrixXVector(M,&Vtx->Pos,&U);
				Vector_Add(&U,&V,&Vtx->TPos);
				
				Vtx->Flags=0;
				Vtx->RZ=1.0/Vtx->TPos.z;
				Vtx->PX=Vtx->TPos.x*Vtx->RZ;
				Vtx->PY=Vtx->TPos.y*Vtx->RZ;
				//        Vtx->PX=CntrEX+PX*Vtx->TPos.x*Vtx->RZ;
				//        Vtx->PY=CntrEY-PY*Vtx->TPos.y*Vtx->RZ;

				// Environment mapping support removed at 11.04.02
//				Vtx->EU=128.0+95.0*(Vtx->N.x*IM[0][0]+Vtx->N.y*IM[0][1]+Vtx->N.z*IM[0][2]);
//				Vtx->REU=Vtx->EU*Vtx->RZ;
//				Vtx->EV=128.0+95.0*(Vtx->N.x*IM[1][0]+Vtx->N.y*IM[1][1]+Vtx->N.z*IM[1][2]);
//				Vtx->REV=Vtx->EV*Vtx->RZ;
				Vtx->UZ=Vtx->U*Vtx->RZ;
				Vtx->VZ=Vtx->V*Vtx->RZ;
				//if (Vtx->TPos.z>Sc->FZP) Vtx->Flags|=Vtx_VisFar;
			}
			goto AfterXForm;
			// This is in case 100% of trimesh AHEAD of camera. this saves some chks
EAhead://Vertex_Loop1(T->Vertex,VEnd,M,&V);
			for (Vtx=T->Verts;Vtx<VEnd;Vtx++)
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
				//        Vtx->PX=CntrEX+PX*Vtx->TPos.x*Vtx->RZ;
				//        Vtx->PY=CntrEY-PY*Vtx->TPos.y*Vtx->RZ;
				Vtx->UZ=Vtx->U*Vtx->RZ;
				Vtx->VZ=Vtx->V*Vtx->RZ;
				if (Vtx->PX<0) Vtx->Flags=Vtx_VisLeft; else Vtx->Flags=0;
				if (Vtx->PX>=xr) Vtx->Flags+=Vtx_VisRight;
				if (Vtx->PY<0) Vtx->Flags+=Vtx_VisUp;
				if (Vtx->PY>=yr) Vtx->Flags+=Vtx_VisDown;
				if (Vtx->TPos.z>Sc->FZP) Vtx->Flags|=Vtx_VisFar;
			}
			//    printf("Ahead VGA/Wizard.\n");
			
			goto AfterXForm;
ERegular:
			for (Vtx=T->Verts;Vtx<VEnd;Vtx++)
			{
				//    if (!Vtx->FRem) continue;
				MatrixXVector(M,&Vtx->Pos,&U);
				Vector_Add(&U,&V,&Vtx->TPos);
				
				Vtx->Flags = 0;
				//      if (*(int32_t *)(&Vtx->TPos.z)>0x3F800000) // 1.0 in floating point rep.

				// Environment mapping support removed at 11.04.02
//				Vtx->EU=128.0+95.0*(Vtx->N.x*IM[0][0]+Vtx->N.y*IM[0][1]+Vtx->N.z*IM[0][2]);
//				Vtx->EV=128.0+95.0*(Vtx->N.x*IM[1][0]+Vtx->N.y*IM[1][1]+Vtx->N.z*IM[1][2]);
				
				if (Vtx->TPos.z>Sc->NZP)
				{
					Vtx->RZ=1.0/Vtx->TPos.z;
					Vtx->PX=Vtx->TPos.x*Vtx->RZ;
					Vtx->PY=Vtx->TPos.y*Vtx->RZ;
					//          Vtx->PX=CntrEX+PX*Vtx->TPos.x*Vtx->RZ;
					//          Vtx->PY=CntrEY-PY*Vtx->TPos.y*Vtx->RZ;
					Vtx->UZ=Vtx->U*Vtx->RZ;
					Vtx->VZ=Vtx->V*Vtx->RZ;
//					Vtx->REU=Vtx->EU*Vtx->RZ;
//					Vtx->REV=Vtx->EV*Vtx->RZ;
					if (Vtx->PX<0) Vtx->Flags=Vtx_VisLeft;
					if (Vtx->PX>=xr) Vtx->Flags+=Vtx_VisRight;
					if (Vtx->PY<0) Vtx->Flags+=Vtx_VisUp;
					if (Vtx->PY>=yr) Vtx->Flags+=Vtx_VisDown;
					if (Vtx->TPos.z>Sc->FZP) Vtx->Flags|=Vtx_VisFar;
				} else Vtx->Flags=Vtx_VisNear;
				
				//      printf("Regular shit!\n");
			}
			
		}
AfterXForm:FEnd=T->Faces+T->FIndex;
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
	static const bool forceXparTwoSided = []{
		const char* s = std::getenv("FDS_XPAR_FORCE_TWOSIDED");
		return s && s[0] == '1';
	}();
	// Shadow pass: treat all faces as two-sided. Single-quad walls have
	// no back-side polygon to take over when the light's on their "back",
	// yet they still occlude light. Backface-culling for shadow rendering
	// is geometry-dependent (correct for sealed convex meshes, wrong for
	// single-sided walls/sheets) — easier to just skip it. Toggle this off
	// via FDS_SHADOW_BACKFACE_CULL=1 if you want to A/B.
	extern thread_local bool g_inShadowPass;
	static const bool sShadowBackface = []{
		const char *s = std::getenv("FDS_SHADOW_BACKFACE_CULL");
		return s && s[0] == '1';
	}();
	const bool shadowNoBackface = g_inShadowPass && !sShadowBackface;
	for (F=T->Faces;F<FEnd;F++) {
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

				// auto cv = (T->BSphereCtr - View->ISource) * 0.9 + View->ISource;
				auto cv = View->ISource;
				float optimalDistFromPlane = fabs(T->BSphereCtr * F->N - F->NormProd);
				float viewDistFromPlane = fabs(AP * F->N - F->NormProd);
				if (viewDistFromPlane > optimalDistFromPlane) {
					float hackDistFromPlane = pow(viewDistFromPlane - optimalDistFromPlane, 0.8) + optimalDistFromPlane;
					Vector bsWorldPos;
					MatrixXVector(T->RotMat, &T->BSphereCtr, &bsWorldPos);
					bsWorldPos += T->IPos;

					auto pullDir = bsWorldPos - View->ISource;
					float step = pullDir * F->N;
					cv += (hackDistFromPlane - viewDistFromPlane) / step * pullDir;
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
			*Ins++ = F;

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
				static const bool xparFrontBackDisabled = []{
					const char* s = std::getenv("FDS_NO_XPAR_FRONTBACK");
					return s && s[0] == '1';
				}();
				static const bool xparObjGroupDisabled = []{
					const char* s = std::getenv("FDS_NO_XPAR_OBJGROUP");
					return s && s[0] == '1';
				}();

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

		}
	}  // close per-face loop body opened above
	}
	CPolys = Ins-FList;

	FDW omniFlareSize;
	for(O=Sc->OmniHead;O;O=O->Next)
	{


		Vtx=&O->V;
		Vector_Sub(&O->IPos,&View->ISource,&V);
		//if (O->Flags & Omni_Rand) {
		//	Vector Rand;
		//	Rand.x = (frand() - 0.5) * 2.0f;
		//	Rand.y = (frand() - 0.5) * 2.0f;
		//	Rand.z = (frand() - 0.5) * 2.0f;
		//	Vector_Add(&V, &Rand, &V);
		//}
		MatrixXVector(View->Mat,&V,&Vtx->TPos);
		if (Vtx->TPos.z>Sc->NZP&&Vtx->TPos.z<Sc->FZP)
		{
			Vtx->RZ=1.0/Vtx->TPos.z;
			Vtx->PX=CntrEX+Vtx->TPos.x*PX*Vtx->RZ;
			Vtx->PY=CntrEY-Vtx->TPos.y*PY*Vtx->RZ;
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
			*Ins++ = &O->F;
		}
	}

	COmnies = (Ins-FList)-CPolys;
#endif
	for (I = 0; I < Sc->NumOfParticles; I++) {
		Particle& p = Sc->Pcl[I];

		auto v = p.V.Pos - View->ISource;
		
		p.V.TPos = View->Mat * v;

		if (p.V.TPos.z >= Sc->NZP)
		{
			p.V.RZ = 1.0 / p.V.TPos.z;
			p.V.PX = CntrX + FOVX * p.V.TPos.x * p.V.RZ;
			p.V.PY = CntrY - FOVY * p.V.TPos.y * p.V.RZ;
			p.V.Flags = 0;
		} else {
			p.V.Flags |= Vtx_VisNear;
		}


		if (p.Flags & Particle_Active) {
			if ((dz = p.V.TPos.z) >= Sc->NZP) {
				if (p.TrailLength == 0) {
					F = &Sc->Pcl[I].F;
#ifdef FRONT_TO_BACK_SORTING
					F->SortZ.F = 2 * fzp - dz;
#else
					F->SortZ.F = fzp - dz;
#endif
					*Ins++ = F;
				} else {
					addParticleTrail(Sc, Ins, p);
				}
			}
		}
	}

	CAll = Ins-FList;
	CPcls = CAll-COmnies-CPolys;
}

float Vector_CosAngleFAST(Vector *U,Vector *V)
{
	return (U->x*V->x+U->y*V->y+U->z*V->z)*
		RSQRT((U->x*U->x+U->y*U->y+U->z*U->z)*(V->x*V->x+V->y*V->y+V->z*V->z));
}



// Able to operate before Transformation Module
// based on a 
void LightingOld(Scene *Sc)
{
	TriMesh *T;
	Vertex *Vtx,*VEnd;
	Face *Face,*FEnd;
	Omni *O;
	float R,G,B,Dot,Dot2,Its,Color,Distance;
	const float Ka=Ambient_Factor/*255.0*/,Kd=Diffusive_Factor,Ks=Specular_Factor,n=4.0;
	Vector L,U,V,W,TN,CU,CV;
	Matrix M;
	float rLen,Len,Len2;
	float ir;
	float illumFactor;
	
		
	for(T=Sc->TriMeshHead;T;T=T->Next)
	{
		if (T->Flags&Tri_Invisible) continue;
		if (T->Flags&Tri_Noshading) continue;		

		Material *M;
		float Lumin;

		if (T->FIndex)
		{
			M = T->Faces[0].Txtr;
			Lumin = M->Luminosity;
			if (!(M->Flags & Mat_RGBInterp))
			{
				int banana = 1;
			}
		} else Lumin = 1.0;
		VEnd=T->Verts+T->VIndex;
		FEnd=T->Faces+T->FIndex;
		Vector_Sub(&T->IPos,&View->ISource,&U);
		MatrixTXVector(T->RotMat,&U,&W);
		// Camera Illumination model (Lambertian)
		if (Cam_HeadLight)
		{
			for(Vtx=T->Verts;Vtx<VEnd;Vtx++)
			{
			/*        Its = fabs(Dot_Product(&Vtx->N,(Vector *)(M)+2)*30000000.0);
				Its /= Dot_Product(&Vtx->TPos,&Vtx->TPos);*/
				Vector_Add(&Vtx->Pos,&W,&V);
				Its = 350000.0/Dot_Product(&V,&V);
				if (Its<1.0) Its=0.5;
				if (Its>229.0) Its=229.0;
				Vtx->LR = Its+Sc->Ambient.R;
				Vtx->LG = Its+Sc->Ambient.G;
				Vtx->LB = Its+Sc->Ambient.B;
				//      Vtx->LR=Vtx->LG=Vtx->LB = 0.5;
			}
		} else {
			for(Vtx=T->Verts;Vtx<VEnd;Vtx++)
			{
				Vtx->LR = Sc->Ambient.R * Lumin;
				Vtx->LG = Sc->Ambient.G * Lumin;
				Vtx->LB = Sc->Ambient.B * Lumin;
			}
		}
		// Lightsource based Illumination model
		/*    for(O=Sc->OmniHead;O;O=O->Next)
		if(O->Flags&Flare_LightSource)
		{
		Vector_Sub(&T->IPos,&O->IPos,&U);
		MatrixTXVector(T->RotMat,&U,&W);
		for(Vtx=T->Verts;Vtx<VEnd;Vtx++)
		{
		Vector_Add(&Vtx->Pos,&W,&L);
		Dot=1.0/Dot_Product(&L,&L);
		Vtx->LR += O->L.R * Dot;
		Vtx->LG += O->L.G * Dot;
		Vtx->LB += O->L.B * Dot;
		}
	}*/
	/*
	N = surface normal
	L     = unit vector between point and light
	V = unit vector between point and view
		*/
		//    Ka + Kd * (N dot L) + Ks * (N dot ( L + V / 2))^n
		//int OmniCounter = 0;
		for(O=Sc->OmniHead;O;O=O->Next)
		{
			// HACK: omni disabling code
			//if (OmniCounter < NumOmniDisable && OmniDisable[OmniCounter++]) continue;
			// HACK: move omni to camera
			//Vector_Copy(&O->IPos, &View->ISource);
			if(O->Flags&Omni_Active)
			{
				Vector_Sub(&T->IPos,&O->IPos,&U);
				MatrixTXVector(T->RotMat,&U,&W);
				Vector *WP = (Vector *)(T->RotMat);
				float Scale = Vector_Length(WP);
				float rScale = 1.0/Scale;
				Vector_SelfScale(&W, rScale*rScale);
				
				//				Vector_Sub(&View->ISource,&O->IPos,&CU);
				//				MatrixTXVector(View->Mat,&CU,&CV);
				float effRange = Range_Factor * O->IRange;
				if (!effRange) effRange = O->ISize; // lousy default
				
				// Hack: hardcoded range modifier
				//effRange *= OmniRangeMult[OmniCounter-1]; 
				
				// check the lousy b-sphere
				if (Vector_Length(&U)>sqrt(T->BSphereRad)*Scale+effRange*8.0) continue;
				
				for(Vtx=T->Verts;Vtx<VEnd;Vtx++)
				{
					Vector_Add(&Vtx->Pos,&W,&L);
					//						if (Vector_SelfDot(&L)>ir) continue;
					rLen = RSQRT(L.x*L.x+L.y*L.y+L.z*L.z);					
					Dot=-Dot_Product(&L,&Vtx->N)*rLen;
					
					rLen *=rScale; // Copo-cabana
					
					illumFactor = effRange * rLen;
					if (illumFactor > 1.0/8.0)
					{
						illumFactor *= illumFactor; 
						
						if (Dot>0.0f)
						{
							Color=(Ka+Dot*(Kd+Ks*Dot*Dot))*illumFactor;
						} else Color=Ka*illumFactor;
						Vtx->LR += O->L.R * Color;
						Vtx->LG += O->L.G * Color;
						Vtx->LB += O->L.B * Color;
					}
				}
			}
			
			// Saturation.
			for(Vtx=T->Verts;Vtx<VEnd;Vtx++)
			{
				if (Vtx->LR>245.0) Vtx->LR=245.0;
				if (Vtx->LG>245.0) Vtx->LG=245.0;
				if (Vtx->LB>245.0) Vtx->LB=245.0;
			}
		}
	}
}


// This lighting module is complient with LightWave 3D, and should be optimized using
// SSE assembly code
//#define LIGHTING_CALC_ALPHA
void StaticLighting(Scene *Sc)
{
	TriMesh *T;
	Vertex *V,*VE;
	Face *F,*FE;
	Omni *O;

	Vector u, v, w;
	dword n;

	Matrix M;
	Material *Mat;
	float Lumin;

	// hopefully, paragraph aligned
	static Color l;
	static Color Ambient;

	// light position vector
	static CurLight LA[128], *L, *LE;

	for(T=Sc->TriMeshHead;T;T=T->Next)
	{
		if (T->Flags&Tri_Noshading) continue;
		if (!(T->Flags & Tri_Stationary)) continue;

		if (T->FIndex)
		{
			Mat = T->Faces[0].Txtr;
			Lumin = Mat->Luminosity;
		} else Lumin = 0.0;
		
		// Calculate ambient factor
		// LW3D: Luminosity * base color + scene Ambient * diffuse level
		if (Mat->Txtr)
		{
			Ambient.B = Lumin * 255.0 + Mat->Diffuse * Sc->Ambient.B;
			Ambient.G = Lumin * 255.0 + Mat->Diffuse * Sc->Ambient.G;
			Ambient.R = Lumin * 255.0 + Mat->Diffuse * Sc->Ambient.R;
#ifdef LIGHTING_CALC_ALPHA
			Ambient.A = Lumin * 255.0 + Mat->Diffuse * Sc->Ambient.A;
#endif
		} else {
			Ambient.B = Lumin * Mat->BaseCol.B + Mat->Diffuse * Sc->Ambient.B;
			Ambient.G = Lumin * Mat->BaseCol.G + Mat->Diffuse * Sc->Ambient.G;
			Ambient.R = Lumin * Mat->BaseCol.R + Mat->Diffuse * Sc->Ambient.R;
#ifdef LIGHTING_CALC_ALPHA
			Ambient.A = Lumin * Mat->BaseCol.A + Mat->Diffuse * Sc->Ambient.A;
#endif
		}

		for(O=Sc->OmniHead, L=LA;O;O=O->Next)
		{
			if (!(O->Flags & Omni_Active))
				continue;
			if (!(O->Flags & Omni_Stationary))
				continue;
			Vector_Sub(&O->IPos, &T->IPos, &u);
			MatrixTXVector(T->RotMat, &u, &w);
			Vector *wp = (Vector *)(T->RotMat);
			float rScale = 1.0/Vector_Length(wp);
			Vector_SelfScale(&w, rScale*rScale);
			Vector_Copy(&L->Pos, &w);

			L->Range = O->IRange * rScale;
			L->Range2 = L->Range * L->Range;
			L->rRange = 1.0/L->Range;

			// Spot light: rotate the cone axis into the mesh's local
			// space so the per-vertex `dot(L->Dir, vertex_to_light)`
			// matches the same coordinate system as the rest of this
			// loop. cosInner/cosOuter come from the Omni unchanged
			// (they're cosines, no scaling needed).
			L->isSpot = (O->Type == Light_SpotLight) ? 1 : 0;
			if (L->isSpot) {
				MatrixTXVector(T->RotMat, (Vector*)&O->IDir, &L->Dir);
				Vector_Norm(&L->Dir);
				L->cosInner = O->HotSpot;
				L->cosOuter = O->FallOff;
			} else {
				L->Dir.x = L->Dir.y = L->Dir.z = 0.0f;
				L->cosInner = -2.0f;  // sentinel: "always inside"
				L->cosOuter = -2.0f;
			}

			if (Mat->Txtr)
			{
				float intensity = O->ISize * Mat->Diffuse;
				L->Col.B = O->L.B * intensity;
				L->Col.G = O->L.G * intensity;
				L->Col.R = O->L.R * intensity;
#ifdef LIGHTING_CALC_ALPHA
				L->Col.A = O->L.A * intensity;
#endif
			} else {
				float intensity = O->ISize * Mat->Diffuse / 256.0;
				L->Col.B = O->L.B * Mat->BaseCol.B * intensity;
				L->Col.G = O->L.G * Mat->BaseCol.G * intensity;
				L->Col.R = O->L.R * Mat->BaseCol.R * intensity;
#ifdef LIGHTING_CALC_ALPHA
				L->Col.A = O->L.A * Mat->BaseCol.A * intensity;
#endif
			}
			L++;
		}
		LE = L;

		mword vi=0;
		for(V=T->Verts, VE = V+T->VIndex; V<VE; V++,vi++)
		{
			l.B = Ambient.B;
			l.G = Ambient.G;
			l.R = Ambient.R;
#ifdef LIGHTING_CALC_ALPHA
			l.A = Ambient.A;
#endif			
			for(L=LA; L<LE; L++)
			{
				Vector_Sub(&L->Pos, &V->Pos, &w);
				float dot = Dot_Product(&w, &V->N);
				if (dot < 0.0) continue;
				float v = Vector_Length(&w);
				float len2 = w.x*w.x+w.y*w.y+w.z*w.z;
				if (len2 > L->Range2) continue;
				float len = SQRT(len2);
				float k = dot / len * (1.0-len*L->rRange);

				// Spot cone attenuation: cosTheta = dot(-Dir, light_to_vertex_unit).
				// w runs vertex→light, so the light's outgoing direction
				// at the vertex is -w/|w| relative to the vertex but +w/|w|
				// relative to the light. We want angle between L->Dir
				// (cone axis) and the vector FROM the light TO the vertex
				// (= -w/|w|). Equivalently: cosTheta = dot(L->Dir, -w/|w|)
				//                                   = -dot(L->Dir, w)/|w|.
				if (L->isSpot) {
					float invLen = 1.0f / len;
					float cosTheta = -(L->Dir.x*w.x + L->Dir.y*w.y + L->Dir.z*w.z) * invLen;
					if (cosTheta <= L->cosOuter) continue;  // outside outer cone
					if (cosTheta < L->cosInner) {
						float t = (cosTheta - L->cosOuter) / (L->cosInner - L->cosOuter);
						k *= t * t * (3.0f - 2.0f * t);  // smoothstep edge falloff
					}
					// (cosTheta >= cosInner: inside inner cone, no attenuation)
				}

				l.B += k*L->Col.B;
				l.G += k*L->Col.G;
				l.R += k*L->Col.R;
#ifdef LIGHTING_CALC_ALPHA
				l.A += k*L->Col.A;
#endif
			}

			// saturation
			if (l.B > 250.0) l.B = 250.0;
			if (l.G > 250.0) l.G = 250.0;
			if (l.R > 250.0) l.R = 250.0;
#ifdef LIGHTING_CALC_ALPHA
			if (l.A > 250.0) l.A = 250.0;
#endif
			
			T->SL[vi].B = l.B;
			T->SL[vi].G = l.G;
			T->SL[vi].R = l.R;
#ifdef LIGHTING_CALC_ALPHA
			T->SL[vi].A = l.A;
#endif
		}
	}
}



// Lighting() now runs unconditionally. The deferred path bypasses
// Mekalele for Face_Reflective faces and dispatches them through the
// forward TheOtherBarry<OVERWRITE, TEXTURETEXTURE> filler — which reads
// Vtx->LR/LG/LB for its colorize step — so the per-vertex pass has to
// populate those even when the rest of the frame is going through the
// G-buffer compositor.
extern bool deferredEnabled();

void Lighting(Scene *Sc)
{

	TriMesh *T;
	Vertex *V, *VE;
	Face *F, *FE;
	Omni *O;

	Vector u, v, w;
	dword n;

	Matrix M;
	Material *Mat;
	float Lumin;

	// hopefully, paragraph aligned
	static Color *pl = (Color *)getAlignedBlock(sizeof(Color), 16);
	static Color Ambient;

	// light position vector
	static CurLight *LA = (CurLight *)getAlignedBlock(sizeof(CurLight) * 128, 16);
	static CurLight *L, *LE;

	if (!(Sc->Flags & Scn_StaticLighting))
	{
		Sc->Flags |= Scn_StaticLighting;
		StaticLighting(Sc);
	}

	for (T = Sc->TriMeshHead; T; T = T->Next)
	{
		if (T->Flags&(Tri_Invisible | Tri_Noshading)) continue;

		if (T->FIndex)
		{
			Mat = T->Faces[0].Txtr;
			Lumin = Mat->Luminosity;
		}
		else Lumin = 0.0;


		// Calculate ambient factor
		// LW3D: Luminosity * base color + scene Ambient * diffuse level
		if (Mat->Txtr)
		{
			Ambient.B = Lumin * 255.0 + Mat->Diffuse * Sc->Ambient.B;
			Ambient.G = Lumin * 255.0 + Mat->Diffuse * Sc->Ambient.G;
			Ambient.R = Lumin * 255.0 + Mat->Diffuse * Sc->Ambient.R;
			Ambient.A = Lumin * 255.0 + Mat->Diffuse * Sc->Ambient.A;
		} else {
			Ambient.B = Lumin * Mat->BaseCol.B + Mat->Diffuse * Sc->Ambient.B;
			Ambient.G = Lumin * Mat->BaseCol.G + Mat->Diffuse * Sc->Ambient.G;
			Ambient.R = Lumin * Mat->BaseCol.R + Mat->Diffuse * Sc->Ambient.R;
			Ambient.A = Lumin * Mat->BaseCol.A + Mat->Diffuse * Sc->Ambient.A;

		}

		for (O = Sc->OmniHead, L = LA; O; O = O->Next)
		{
			if (!(O->Flags & Omni_Active))
				continue;
			if ((T->Flags & Tri_Stationary) && (O->Flags & Omni_Stationary))
				continue;
			Vector_Sub(&O->IPos, &T->IPos, &u);
			MatrixTXVector(T->RotMat, &u, &w);
			Vector *wp = (Vector *)(T->RotMat);
			//			float rScale = 1.0/Vector_Length(wp);
			float rScale = Vector_Length(wp);
			Vector_SelfScale(&w, rScale*rScale);
			Vector_Copy(&L->Pos, &w);

			L->Range = O->IRange * rScale;
			L->Range2 = L->Range * L->Range;
			L->rRange = 1.0/L->Range;

			// bounding sphere check
			Vector_Sub(&L->Pos, &T->BSphereCtr, &u);
			if (T->BSphereRadius + L->Range < Vector_Length(&u))
				continue;

			// Spot params (mirrors the other Lighting() loop above).
			L->isSpot = (O->Type == Light_SpotLight) ? 1 : 0;
			if (L->isSpot) {
				MatrixTXVector(T->RotMat, (Vector*)&O->IDir, &L->Dir);
				Vector_Norm(&L->Dir);
				L->cosInner = O->HotSpot;
				L->cosOuter = O->FallOff;
			} else {
				L->Dir.x = L->Dir.y = L->Dir.z = 0.0f;
				L->cosInner = -2.0f;
				L->cosOuter = -2.0f;
			}

			if (Mat->Txtr)
			{
				float intensity = O->ISize * Mat->Diffuse;
				L->Col.B = O->L.B * intensity;
				L->Col.G = O->L.G * intensity;
				L->Col.R = O->L.R * intensity;
				L->Col.A = O->L.A * intensity;
			}
			else {
				float intensity = O->ISize * Mat->Diffuse / 256.0;
				L->Col.B = O->L.B * Mat->BaseCol.B * intensity;
				L->Col.G = O->L.G * Mat->BaseCol.G * intensity;
				L->Col.R = O->L.R * Mat->BaseCol.R * intensity;
				L->Col.A = O->L.A * Mat->BaseCol.A * intensity;

			}
			L++;
		}
		LE = L;

		bool stat = false;
		if (T->Flags & Tri_Stationary)
			stat = true;

		Color *sl = T->SL;
		for (V = T->Verts, VE = V + T->VIndex; V < VE; V++, sl++)
		{
			// this compare isn't so bad as soon as it gets into the BTB.
			if (stat)
			{
				pl->B = sl->B;
				pl->G = sl->G;
				pl->R = sl->R;
				pl->A = sl->A;
			} else {
				pl->B = Ambient.B;
				pl->G = Ambient.G;
				pl->R = Ambient.R;
				pl->A = Ambient.A;
			}

			for (L = LA; L < LE; L++)
			{
				Vector_Sub(&L->Pos, &V->Pos, &w);
				float dot = Dot_Product(&w, &V->N);
				if (dot < 0.0) continue;
				float v = Vector_Length(&w);
				float len2 = w.x*w.x + w.y*w.y + w.z*w.z;
				if (len2 > L->Range2) continue;
				/*				float len = SQRT(len2);
								float k = dot / len * (1.0-len*L->rRange);*/

				float len = RSQRT(len2);
				float k = dot * len * (1.0-L->rRange/len);

				// Spot cone — same shape as the other Lighting() loop. RSQRT
				// here returns 1/sqrt, so `len` is actually the inverse
				// length. cosTheta = -dot(L->Dir, w) * (1/|w|).
				if (L->isSpot) {
					float cosTheta = -(L->Dir.x*w.x + L->Dir.y*w.y + L->Dir.z*w.z) * len;
					if (cosTheta <= L->cosOuter) continue;
					if (cosTheta < L->cosInner) {
						float t = (cosTheta - L->cosOuter) / (L->cosInner - L->cosOuter);
						k *= t * t * (3.0f - 2.0f * t);
					}
				}

				pl->B += k*L->Col.B;
				pl->G += k*L->Col.G;
				pl->R += k*L->Col.R;
				pl->A += k*L->Col.A;

			}

			// saturation
			if (pl->B > 250.0) pl->B = 250.0;
			if (pl->G > 250.0) pl->G = 250.0;
			if (pl->R > 250.0) pl->R = 250.0;
			if (pl->A > 250.0) pl->A = 250.0;

			V->LB = pl->B;
			V->LG = pl->G;
			V->LR = pl->R;
			V->LA = pl->A;
		}
	}
}

void Restore_Splines(Scene *Sc)
{
	TriMesh *T;
	Camera *Cm;
	Omni *Om;
	for(T=Sc->TriMeshHead;T;T=T->Next)
	{
		T->Pos.CurKey = 0;
		T->Rotate.CurKey = 0;
		T->Scale.CurKey = 0;
		T->CurStat = T->Status;
	}
	for(Om=Sc->OmniHead;Om;Om=Om->Next)
	{
		Om->Pos.CurKey = 0;
		//    Om->Col.CurKey = 0;
	}
	for(Cm=Sc->CameraHead;Cm;Cm=Cm->Next)
	{
		Cm->Source.CurKey = 0;
		Cm->Target.CurKey = 0;
		Cm->Roll.CurKey = 0;
		Cm->FOV.CurKey = 0;
	}
}

namespace renderns {
	std::mutex					tileCounterMutex;
	std::atomic<int>			tileCounter;
	std::condition_variable		condition;
};

void RenderInner(float x1, float y1, float x2, float y2) {
	FrustumClipper clipper;
	clipper.InitViewport(CurScene);
	clipper.SetClippingExtents(x1, y1, x2, y2);

	int32_t I = CAll;
	Face** FLS = FList;//+CAll-1;
	Vertex* A, * B, * C;

	Vertex* V[4];

	while (I--) {
		Face* F = *FLS++;

		// Get Mapping Coordinates from the rendered face.
		A = F->A; B = F->B;
		if (A == B) {
			continue;
		} else {
			// Polygon - can further check the expression
			// (A->Flags|B->Flags|C->Flags)&Vtx_Visible
			// if it is zero, we can use something lighter than
			// the frustrum clipper.
			C = F->C;

			auto flags = A->Flags & B->Flags & C->Flags;
			if (flags & Vtx_Visible) continue;
			RasterFunc filler;
			if (F->Flags & Face_Reflective) {
				clipper.Render(F, TheOtherBarry<barry::TBlendMode::OVERWRITE, barry::TTextureMode::TEXTURETEXTURE>, false);
			} else {
				clipper.Render(F, F->Filler, false);
			}
			//if (F->Flags & Face_Reflective) {
			//	clipper.Render(F, IX_Prefiller_Reflective, true);
			//}
		}
	}

	std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
	++renderns::tileCounter;
	renderns::condition.notify_one();
}


void RenderInnerMekalele(float x1, float y1, float x2, float y2) {
	FrustumClipper clipper;
	clipper.InitViewport(CurScene);
	clipper.SetClippingExtents(x1, y1, x2, y2);

	int32_t I = CAll;
	Face** FLS = FList;//+CAll-1;
	Vertex* A, * B, * C;

	Vertex* V[4];

	while (I--) {
		Face* F = *FLS++;

		// Get Mapping Coordinates from the rendered face.
		A = F->A; B = F->B;
		if (A == B) {
			continue;
		} else {
			// Polygon - can further check the expression
			// (A->Flags|B->Flags|C->Flags)&Vtx_Visible
			// if it is zero, we can use something lighter than
			// the frustrum clipper.
			C = F->C;

			auto flags = A->Flags & B->Flags & C->Flags;
			if (flags & Vtx_Visible) continue;
			// Mekalele dereferences F->Txtr->Txtr->LSizeX in its
			// TileRasterizer constructor — untextured Materials would
			// segfault. Skip them for now; the matID/Material dispatch
			// in the future lighting pass needs a sentinel slot for
			// "no texture; use BaseCol", which is the proper fix.
			if (!F->Txtr || !F->Txtr->Txtr) continue;
			// Reflective faces (City windows, etc.) want forward's
			// per-vertex eu/ev interpolation across the panorama —
			// per-pixel reflection-vector reconstruction in the
			// deferred lighting pass produced wrong-scale, jittery
			// reflections that didn't track the world the way
			// Reflective_Mapper_Setup's per-face cv tweak does.
			// Forward TheOtherBarry<OVERWRITE,TEXTURETEXTURE> writes
			// directly to VPage and ZPage16 (skipping the G-buffer);
			// the mat32 sentinel pre-clear ensures the deferred
			// lighting pass treats those pixels as already-shaded
			// and leaves them alone.
			//
			// Material classification:
			//   Mat_Transparent: deferred — render AFTER lighting pass
			//     so the alpha blend reads finished opaque pixels.
			//     (water, glass, etc. — handled by RenderInnerDeferredTransparent.)
			//   Mat_Additive: render NOW via forward filler (TheOtherBarry
			//     <ADDITIVE>) so it Z-tests + writes Z alongside opaque
			//     in the same tile pass. Most-prominent case is the
			//     fountain vortex, which has SortPriorityBias=DrawFirst
			//     (sorted to FList front) — drawing it first writes its
			//     Z to the empty z-buffer; opaque Mekalele then overwrites
			//     where opaque is closer. Render_DeferredLighting's mat32
			//     sentinel skip preserves vortex's color where no opaque
			//     covered. Particles + transparents drawn later Z-test
			//     against vortex Z and get correctly occluded.
			const dword txtrFlags = F->Txtr->Flags;
			if (txtrFlags & Mat_Transparent) {
				continue;
			}
			if (F->Flags & Face_Reflective) {
				clipper.Render(F, TheOtherBarry<barry::TBlendMode::OVERWRITE, barry::TTextureMode::TEXTURETEXTURE>, false);
			} else if (txtrFlags & Mat_Additive) {
				clipper.Render(F, F->Filler, false);   // TheOtherBarry<ADDITIVE>
			} else {
				clipper.Render(F, Mekalele, false);
			}
		}
	}

	std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
	++renderns::tileCounter;
	renderns::condition.notify_one();
}

// Which face orientation to raster in this pass. Concentric transparent
// meshes need their back-facing tris of OUTER composited BEFORE the
// back-facing tris of INNER, but their front-facing tris in the REVERSE
// order — back-to-front face sequence for two nested spheres is:
//   outer_back, inner_back, inner_front, outer_front
// The caller drives this by issuing two phases: phase 1 raster+composite
// of back faces (outer→inner), phase 2 raster+composite of front faces
// (inner→outer). With XparFaceSel::Both you get the old behaviour where
// each mesh handles both face orientations in one pass (broken for
// concentric, kept for legacy paths).
enum class XparFaceSel { Both, BackOnly, FrontOnly };

void RenderInnerDeferredTransparent(float x1, float y1, float x2, float y2,
                                     int firstIdx, int countIdx,
                                     TriMesh* parentFilter,
                                     XparFaceSel faceSel = XparFaceSel::Both) {
	FrustumClipper clipper;
	clipper.InitViewport(CurScene);
	clipper.SetClippingExtents(x1, y1, x2, y2);

	int32_t I = countIdx;
	Face** FLS = FList + firstIdx;
	Vertex *A, *B, *C;

	while (I--) {
		Face* F = *FLS++;

		// Per-object peeling: when parentFilter is non-null, skip any
		// face that didn't come from that TriMesh. Lets the caller
		// process one transparent object at a time so nested objects
		// (concentric outer+inner) each get their own 2-deep render
		// + composite cycle instead of fighting for the shared back
		// layer.
		if (parentFilter && F->ParentTri != parentFilter) continue;

		A = F->A; B = F->B;
		if (A == B) continue;
		C = F->C;
		auto flags = A->Flags & B->Flags & C->Flags;
		if (flags & Vtx_Visible) continue;
		if (!F->Txtr) continue;
		const dword txtrFlags = F->Txtr->Flags;
		// Mat_Additive (e.g. fountain vortex) renders inside the main
		// Mekalele tile pass via its forward filler, NOT here — that
		// gives it forward-style Z integration with opaque. Only handle
		// Mat_Transparent in this post-lighting pass.
		if (!(txtrFlags & Mat_Transparent)) continue;
		if (F->Flags & Face_Reflective) continue;  // handled in main pass

		// Per-face front/back test: compute the triangle's view-space face
		// normal from its three vertex positions, then sign-test against any
		// point on the face. Using A->TN (a per-vertex normal that the engine
		// smooths across adjacent faces) splits coplanar triangles of the
		// same quad into different code paths when the averaged vertex
		// normals diverge slightly — caused the "panel renders as a frame"
		// bug in xpartest case 3.
		const float ex = B->TPos.x - A->TPos.x;
		const float ey = B->TPos.y - A->TPos.y;
		const float ez = B->TPos.z - A->TPos.z;
		const float fx = C->TPos.x - A->TPos.x;
		const float fy = C->TPos.y - A->TPos.y;
		const float fz = C->TPos.z - A->TPos.z;
		const float nx = ey * fz - ez * fy;
		const float ny = ez * fx - ex * fz;
		const float nz = ex * fy - ey * fx;
		const float vd = nx * A->TPos.x + ny * A->TPos.y + nz * A->TPos.z;
		const bool frontFacing = vd < 0.0f;
		if (frontFacing) {
			if (faceSel == XparFaceSel::BackOnly) continue;
			clipper.Render(F, MekaleleTransparent, false);
		} else {
			if (faceSel == XparFaceSel::FrontOnly) continue;
			// 2-deep transparent G-buffer: back-facing tris of convex
			// transparents (the exit surface along the view ray) go to
			// the back layer. The composite pass lights the back layer
			// onto VPage first, then the front layer on top — so a
			// glass cube shows both its near and far walls.
			clipper.Render(F, MekaleleTransparentBack, false);
		}
	}

	std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
	++renderns::tileCounter;
	renderns::condition.notify_one();
}

// FDS_DEFERRED=1 in env enables the experimental G-buffer path: tiled
// dispatch goes to RenderInnerMekalele instead of RenderInner, then
// Render_DeferredLighting consumes the G-buffer to produce final pixels.
// Lighting() also early-returns when this is set — its per-vertex
// output is unused on the deferred path.
bool deferredEnabled() {
	static bool cached = fdsEnvBool("FDS_DEFERRED", FDS_DEFERRED_DEFAULT_ON);
	return cached;
}

// Per-scene matID for the water material — when this matches the
// G-buffer mat32 at a pixel, the deferred lighting compositor blends
// the lit water texel over the existing VPage value (which holds the
// dispMap-distorted reflection from the pass-1 mirrored-world draw),
// matching forward's TheOtherBarry<TRANSPARENT> filler:
//   pixel = saturate(lit_water + existing/2)
// Set to -1 (default) to disable the water-blend special case.
static int g_deferredWaterMatID = -1;

void SetDeferredWaterMatID(int id) {
	g_deferredWaterMatID = id;
}

// Phase A deferred lighting pass. Replaces the per-vertex Lighting() +
// per-pixel colorize() pipeline with one per-pixel pass: decode mat32 →
// Material*, sample texture, decode oct normal, reconstruct view-space
// position from Z, ambient + Lambertian + linear distance falloff
// per omni, modulate texel by light, write final pixel.
//
// Matches the forward Lighting() model 1:1 — same Lambert × linear
// falloff, same 250-saturation, same Diffuse/Luminosity/BaseCol use.
// Differences vs forward:
//   - Per-pixel material (forward takes Mat[0] per mesh — incidental
//     improvement, not a divergence).
//   - Per-pixel position/normal (forward Gouraud-interpolates per vertex).
//   - View-space lighting (forward transforms each omni into the current
//     mesh's object space; we transform once per frame into view space
//     and reconstruct pixel positions there).
//
// Parallelism: same 6×4 tile-job model the rasterizer uses. Per-frame
// setup (matTable, view-space omni list, screen-projection constants)
// happens once on the demo thread; per-tile work is enqueued onto the
// ThreadPool. Tiles are independent (each writes a disjoint slice of
// VPage). Per-pixel work is scalar; SIMD is a future tighter pass.
// View-space omni storage. SoA layout so the per-pixel inner loop can
// load 8 omnis at a time into Vec8f for the diffuse accumulation.
// Padded entries (index >= numLights, up to MAX_LIGHTS) are zero so
// they fail both the dot >= 0 and len2 <= range2 tests, contributing
// zero — no need to round numLights up to a multiple of 8 explicitly.
constexpr int DEFERRED_MAX_LIGHTS = 128;
constexpr int DEFERRED_NUM_TILES_X = 12;
constexpr int DEFERRED_NUM_TILES_Y = 8;
constexpr int DEFERRED_NUM_TILES   = DEFERRED_NUM_TILES_X * DEFERRED_NUM_TILES_Y;
struct ViewLightsSoA {
	alignas(32) float posX[DEFERRED_MAX_LIGHTS];
	alignas(32) float posY[DEFERRED_MAX_LIGHTS];
	alignas(32) float posZ[DEFERRED_MAX_LIGHTS];
	alignas(32) float colB[DEFERRED_MAX_LIGHTS];
	alignas(32) float colG[DEFERRED_MAX_LIGHTS];
	alignas(32) float colR[DEFERRED_MAX_LIGHTS];
	alignas(32) float range2[DEFERRED_MAX_LIGHTS];
	alignas(32) float rRange[DEFERRED_MAX_LIGHTS];
	// Spot light cone (Light_SpotLight). dirX/Y/Z is the cone axis in
	// view space (unit vector). cosInner / cosOuter are the cone half
	// angles in cosine form. isSpot=0 means omni (cone params ignored).
	alignas(32) float dirX[DEFERRED_MAX_LIGHTS];
	alignas(32) float dirY[DEFERRED_MAX_LIGHTS];
	alignas(32) float dirZ[DEFERRED_MAX_LIGHTS];
	alignas(32) float cosInner[DEFERRED_MAX_LIGHTS];
	alignas(32) float cosOuter[DEFERRED_MAX_LIGHTS];
	alignas(32) uint32_t isSpot[DEFERRED_MAX_LIGHTS];
	// Index into g_shadowMaps for this light's shadow map, or -1 if
	// not a shadow-caster (most omnis). Filled per frame in
	// Render_DeferredLighting alongside the other per-light fields.
	alignas(32) int32_t  shadowMapIdx[DEFERRED_MAX_LIGHTS];
};

// Per-tile light culling. Each tile (a slice of the screen) keeps a
// **compacted** SoA copy of the omnis whose screen-space bounding
// circle overlaps it. Inner loop reads tl.posX[n] etc. directly —
// contiguous, prefetcher-friendly, and — for the vec path — lets a
// single `load_a` pull 8 omnis with one 32-byte aligned read instead
// of building a Vec4f via four `ld1.s {v}[lane]` scalar gathers.
//
// Memory: 24 tiles × 8 arrays × 128 floats × 4 bytes = 96 KiB total.
// Easily fits in L2; per-tile slice stays warm in L1 across pixels.
struct TileLights {
	alignas(32) float posX[DEFERRED_MAX_LIGHTS];
	alignas(32) float posY[DEFERRED_MAX_LIGHTS];
	alignas(32) float posZ[DEFERRED_MAX_LIGHTS];
	alignas(32) float colB[DEFERRED_MAX_LIGHTS];
	alignas(32) float colG[DEFERRED_MAX_LIGHTS];
	alignas(32) float colR[DEFERRED_MAX_LIGHTS];
	alignas(32) float range2[DEFERRED_MAX_LIGHTS];
	alignas(32) float rRange[DEFERRED_MAX_LIGHTS];
	alignas(32) float dirX[DEFERRED_MAX_LIGHTS];
	alignas(32) float dirY[DEFERRED_MAX_LIGHTS];
	alignas(32) float dirZ[DEFERRED_MAX_LIGHTS];
	alignas(32) float cosInner[DEFERRED_MAX_LIGHTS];
	alignas(32) float cosOuter[DEFERRED_MAX_LIGHTS];
	alignas(32) uint32_t isSpot[DEFERRED_MAX_LIGHTS];
	// Shadow-map index in g_shadowMaps for this light's shadow (or -1).
	// Filled by buildTileLightLists when FDS_SHADOWS=1 and the omni
	// has Omni_CastsShadow. Lighting kernel uses it to gate the light's
	// contribution per pixel.
	alignas(32) int32_t shadowMapIdx[DEFERRED_MAX_LIGHTS];
	int             count;          // active entries
	int             paddedCount;    // (count + 7) & ~7, ≤ DEFERRED_MAX_LIGHTS
	float           zMin;           // view-space z of closest pixel in tile
	float           zMax;           // view-space z of farthest pixel in tile
	                                // (+inf / -inf when tile has no geometry)
};

// Per-frame setup shared across all tile jobs. Captured by reference
// in the lambdas; lifetime ends when Render() returns (after the
// tileCounter wait in Render_DeferredLighting).
struct DeferredLightingCtx {
	const meka::GBuffer *gb;
	MatTable             matTable;
	const ViewLightsSoA *lights;
	int                  numLights;
	const TileLights    *tileLights;  // [DEFERRED_NUM_TILES]
	float                invFOVX;
	float                invFOVY;
	float                invZScale;
	Scene               *Sc;
	int                  waterMatID;  // -1 if no water blend
};

// File-scope ctx, populated each frame by Render_DeferredLighting and
// also read by Render_DeferredTransparentLighting_Tile. Both kernels
// share the same per-frame setup (matTable, tileLights, view-space
// projection params, scene); rebuilding it per pass would double the
// per-frame setup cost.
DeferredLightingCtx g_deferredCtx{};

// Build per-tile compacted SoA. For each omni: project its view-space
// bounding sphere into screen space, find overlapping tile rects, then
// **append the omni's values** (not its index) into each overlapping
// tile's contiguous SoA arrays. Memory cost is O(omnis × overlapped
// tiles × bytes-per-omni) which for City is ~30 × 4 × 32 ≈ 4 KiB; for
// Greets ~10 × 4 × 32 ≈ 1.3 KiB. Negligible.
//
// The vec inner loop benefits because `tl.posX[slot..slot+8)` is now
// 32-byte aligned and contiguous — one `load_a` instead of four
// `ld1.s {v}[lane]` gathers per Vec4f. Scalar benefits too via
// straight-line prefetchable access.
// Per-tile depth bounds. Scans ZPage16 once per tile (already populated
// by the G-buffer pass), finds the closest and farthest zEnc with
// geometry, and converts back to view-space z. Tiles with no geometry
// get zMin=+inf / zMax=-inf so all lights are culled for them.
//
// Encoding reminder: zEnc = 0xFF80 - g_zscale*z, so larger zEnc means
// closer pixel. zEnc == 0 means the pixel was never touched.
static void computeTileDepthBounds(TileLights *tileLights, int numTilesX, int numTilesY,
                                   int tileSizeX, int tileSizeY, int xres, int yres,
                                   float invZScale)
{
	const uint16_t *zp = reinterpret_cast<const uint16_t*>(ZPage16);
	for (int j = 0; j < numTilesY; ++j) {
		const int y_lo = j * tileSizeY;
		const int y_hi = std::min(y_lo + tileSizeY, yres);
		for (int i = 0; i < numTilesX; ++i) {
			const int x_lo = i * tileSizeX;
			const int x_hi = std::min(x_lo + tileSizeX, xres);
			const int idx = j * numTilesX + i;

			__m128i vMaxZ = _mm_setzero_si128();           // chasing max
			__m128i vMinZ = _mm_set1_epi16(int16_t(0xFFFF)); // chasing min (zeros mapped up)
			uint16_t maxZ = 0;
			uint16_t minZ = 0xFFFF;

			for (int py = y_lo; py < y_hi; ++py) {
				const uint16_t *row = zp + size_t(py) * xres + x_lo;
				const int width = x_hi - x_lo;
				int px = 0;
				for (; px + 8 <= width; px += 8) {
					__m128i v = _mm_loadu_si128((const __m128i*)(row + px));
					// For min-tracking, replace 0 with 0xFFFF so untouched
					// pixels don't pull the minimum down.
					__m128i isZero = _mm_cmpeq_epi16(v, _mm_setzero_si128());
					__m128i vForMin = _mm_or_si128(v, isZero);
					vMaxZ = _mm_max_epu16(vMaxZ, v);
					vMinZ = _mm_min_epu16(vMinZ, vForMin);
				}
				for (; px < width; ++px) {
					uint16_t z = row[px];
					if (z > maxZ) maxZ = z;
					if (z != 0 && z < minZ) minZ = z;
				}
			}
			// Horizontal reduce the SIMD halves.
			alignas(16) uint16_t mx[8], mn[8];
			_mm_store_si128((__m128i*)mx, vMaxZ);
			_mm_store_si128((__m128i*)mn, vMinZ);
			for (int k = 0; k < 8; ++k) {
				if (mx[k] > maxZ) maxZ = mx[k];
				if (mn[k] < minZ) minZ = mn[k];
			}

			if (maxZ == 0) {
				tileLights[idx].zMin = std::numeric_limits<float>::infinity();
				tileLights[idx].zMax = -std::numeric_limits<float>::infinity();
			} else {
				// Larger zEnc → closer, so maxZ-zEnc maps to zMin-view.
				tileLights[idx].zMin = float(0xFF80 - maxZ) * invZScale;
				tileLights[idx].zMax = float(0xFF80 - minZ) * invZScale;
			}
		}
	}
}

static void buildTileLightLists(TileLights *tileLights, int numTilesX, int numTilesY,
                                 int tileSizeX, int tileSizeY, int xres, int yres,
                                 const ViewLightsSoA &lights, int numLights)
{
	const int numTiles = numTilesX * numTilesY;
	for (int t = 0; t < numTiles; ++t) {
		tileLights[t].count = 0;
	}

	// Per-tile depth-bounded light culling. Computed by
	// computeTileDepthBounds (caller fills tileLights[].zMin / zMax);
	// we reject lights whose view-space z extent doesn't overlap the
	// tile's pixel depth range. Default on; set FDS_DEFERRED_ZCULL=0
	// to disable.
	static const bool zCullEnabled = []{
		const char *s = std::getenv("FDS_DEFERRED_ZCULL");
		return !s || s[0] != '0';
	}();

	for (int li = 0; li < numLights; ++li) {
		const float vx = lights.posX[li];
		const float vy = lights.posY[li];
		const float vz = lights.posZ[li];
		const float r2 = lights.range2[li];
		const float r  = std::sqrt(r2);
		const float vz_minus_r = vz - r;
		const float vz_plus_r  = vz + r;

		// Sphere entirely behind camera: skip.
		if (vz + r < 0.0f) continue;

		int sx_min, sx_max, sy_min, sy_max;
		if (vz - r < 1.0f) {
			// Sphere straddles or is in front of near plane — be
			// conservative and tag every tile. (This is rare for
			// City; keeps the math simple.)
			sx_min = 0;        sx_max = xres - 1;
			sy_min = 0;        sy_max = yres - 1;
		} else {
			// Pinhole projection of bounding sphere — small-angle
			// approximation. Center: (CntrEX + vx*FOVX/vz, CntrEY -
			// vy*FOVY/vz). Radius on-screen: r * FOVX/vz (use FOVY for
			// vertical). Slightly over-estimates near the edges of
			// the FOV but that just lights tiles that miss the per-
			// pixel cull, no correctness impact.
			const float invZ = 1.0f / vz;
			const float cx   = CntrEX + vx * FOVX * invZ;
			const float cy   = CntrEY - vy * FOVY * invZ;
			const float rx   = r * FOVX * invZ;
			const float ry   = r * FOVY * invZ;
			sx_min = std::max(0,        int(std::floor(cx - rx)));
			sx_max = std::min(xres - 1, int(std::ceil (cx + rx)));
			sy_min = std::max(0,        int(std::floor(cy - ry)));
			sy_max = std::min(yres - 1, int(std::ceil (cy + ry)));
			if (sx_min > sx_max || sy_min > sy_max) continue;
		}

		const int tile_i_lo = sx_min / tileSizeX;
		const int tile_i_hi = std::min(numTilesX - 1, sx_max / tileSizeX);
		const int tile_j_lo = sy_min / tileSizeY;
		const int tile_j_hi = std::min(numTilesY - 1, sy_max / tileSizeY);

		const float Lpx = lights.posX[li];
		const float Lpy = lights.posY[li];
		const float Lpz = lights.posZ[li];
		const float Lcb = lights.colB[li];
		const float Lcg = lights.colG[li];
		const float Lcr = lights.colR[li];
		const float Lr2 = lights.range2[li];
		const float Lrr = lights.rRange[li];
		const float Ldx = lights.dirX[li];
		const float Ldy = lights.dirY[li];
		const float Ldz = lights.dirZ[li];
		const float Lci = lights.cosInner[li];
		const float Lco = lights.cosOuter[li];
		const uint32_t Lis = lights.isSpot[li];
		const int32_t  Lsi = lights.shadowMapIdx[li];

		for (int j = tile_j_lo; j <= tile_j_hi; ++j) {
			for (int i = tile_i_lo; i <= tile_i_hi; ++i) {
				const int idx = j * numTilesX + i;
				TileLights &tl = tileLights[idx];
				// Depth cull: skip if light's z-extent doesn't overlap
				// the tile's pixel depth range. Empty tiles have
				// zMin=+inf and zMax=-inf so this rejects everything.
				if (zCullEnabled &&
				    (vz_plus_r < tl.zMin || vz_minus_r > tl.zMax)) {
					continue;
				}
				if (tl.count < DEFERRED_MAX_LIGHTS) {
					const int s = tl.count++;
					tl.posX[s]   = Lpx;
					tl.posY[s]   = Lpy;
					tl.posZ[s]   = Lpz;
					tl.colB[s]   = Lcb;
					tl.colG[s]   = Lcg;
					tl.colR[s]   = Lcr;
					tl.range2[s] = Lr2;
					tl.rRange[s] = Lrr;
					tl.dirX[s]     = Ldx;
					tl.dirY[s]     = Ldy;
					tl.dirZ[s]     = Ldz;
					tl.cosInner[s] = Lci;
					tl.cosOuter[s] = Lco;
					tl.isSpot[s]   = Lis;
					tl.shadowMapIdx[s] = Lsi;
				}
			}
		}
	}

	// Zero the padding slots (count..paddedCount) so the vec loop's
	// over-read produces range2=0 entries that fail the per-pixel
	// `len2 <= range2` mask and contribute nothing.
	for (int t = 0; t < numTiles; ++t) {
		TileLights &tl = tileLights[t];
		const int padded = (tl.count + 7) & ~7;
		const int pad_to = std::min(padded, DEFERRED_MAX_LIGHTS);
		for (int p = tl.count; p < pad_to; ++p) {
			tl.posX[p]   = 0.0f;
			tl.posY[p]   = 0.0f;
			tl.posZ[p]   = 0.0f;
			tl.colB[p]   = 0.0f;
			tl.colG[p]   = 0.0f;
			tl.colR[p]   = 0.0f;
			tl.range2[p] = 0.0f;
			tl.rRange[p] = 0.0f;
			tl.dirX[p]     = 0.0f;
			tl.dirY[p]     = 0.0f;
			tl.dirZ[p]     = 0.0f;
			tl.cosInner[p] = -2.0f;
			tl.cosOuter[p] = -2.0f;
			tl.isSpot[p]   = 0u;
			tl.shadowMapIdx[p] = -1;
		}
		tl.paddedCount = pad_to;
	}
}

// Strip-flavored light list builder (1D, Y-only) for the unified TBR
// path's transparent strip rendering. Mirrors buildTileLightLists but
// over 8-row Y-strips matching the TBR tile shape. Each strip's list
// is consumed by RenderXparClumpInStrip via a DeferredLightingCtx
// variant whose `tileLights` points at this array.
//
// MAX strips at any reasonable display: 4096/8 = 512. We size for
// that ceiling so reallocation isn't needed at runtime resize.
constexpr int DEFERRED_MAX_STRIPS = 512;
static TileLights g_stripLights[DEFERRED_MAX_STRIPS];
static int        g_numStripLights = 0;

static void buildStripLightLists(int numStrips, int stripHeight, int yres,
                                  const ViewLightsSoA &lights, int numLights)
{
	if (numStrips > DEFERRED_MAX_STRIPS) numStrips = DEFERRED_MAX_STRIPS;
	for (int s = 0; s < numStrips; ++s) {
		g_stripLights[s].count = 0;
	}

	for (int li = 0; li < numLights; ++li) {
		const float vx = lights.posX[li];
		const float vy = lights.posY[li];
		const float vz = lights.posZ[li];
		const float r  = std::sqrt(lights.range2[li]);

		if (vz + r < 0.0f) continue;

		int sy_min, sy_max;
		if (vz - r < 1.0f) {
			sy_min = 0;        sy_max = yres - 1;
		} else {
			const float invZ = 1.0f / vz;
			const float cy   = CntrEY - vy * FOVY * invZ;
			const float ry   = r * FOVY * invZ;
			sy_min = std::max(0,       int(std::floor(cy - ry)));
			sy_max = std::min(yres - 1, int(std::ceil (cy + ry)));
			if (sy_min > sy_max) continue;
		}

		const int strip_lo = sy_min / stripHeight;
		const int strip_hi = std::min(numStrips - 1, sy_max / stripHeight);

		const float Lpx = lights.posX[li];
		const float Lpy = lights.posY[li];
		const float Lpz = lights.posZ[li];
		const float Lcb = lights.colB[li];
		const float Lcg = lights.colG[li];
		const float Lcr = lights.colR[li];
		const float Lr2 = lights.range2[li];
		const float Lrr = lights.rRange[li];
		const float Ldx = lights.dirX[li];
		const float Ldy = lights.dirY[li];
		const float Ldz = lights.dirZ[li];
		const float Lci = lights.cosInner[li];
		const float Lco = lights.cosOuter[li];
		const uint32_t Lis = lights.isSpot[li];

		for (int s = strip_lo; s <= strip_hi; ++s) {
			TileLights &tl = g_stripLights[s];
			if (tl.count < DEFERRED_MAX_LIGHTS) {
				const int idx = tl.count++;
				tl.posX[idx]   = Lpx;
				tl.posY[idx]   = Lpy;
				tl.posZ[idx]   = Lpz;
				tl.colB[idx]   = Lcb;
				tl.colG[idx]   = Lcg;
				tl.colR[idx]   = Lcr;
				tl.range2[idx] = Lr2;
				tl.rRange[idx] = Lrr;
				tl.dirX[idx]     = Ldx;
				tl.dirY[idx]     = Ldy;
				tl.dirZ[idx]     = Ldz;
				tl.cosInner[idx] = Lci;
				tl.cosOuter[idx] = Lco;
				tl.isSpot[idx]   = Lis;
			}
		}
	}

	// Zero the padding slots so the vec loop's overread is benign
	// (range2=0 lanes fail the per-pixel `len2 <= range2` mask).
	for (int s = 0; s < numStrips; ++s) {
		TileLights &tl = g_stripLights[s];
		const int padded = (tl.count + 7) & ~7;
		const int pad_to = std::min(padded, DEFERRED_MAX_LIGHTS);
		for (int p = tl.count; p < pad_to; ++p) {
			tl.posX[p]   = 0.0f;
			tl.posY[p]   = 0.0f;
			tl.posZ[p]   = 0.0f;
			tl.colB[p]   = 0.0f;
			tl.colG[p]   = 0.0f;
			tl.colR[p]   = 0.0f;
			tl.range2[p] = 0.0f;
			tl.rRange[p] = 0.0f;
			tl.dirX[p]     = 0.0f;
			tl.dirY[p]     = 0.0f;
			tl.dirZ[p]     = 0.0f;
			tl.cosInner[p] = -2.0f;
			tl.cosOuter[p] = -2.0f;
			tl.isSpot[p]   = 0u;
		}
		tl.paddedCount = pad_to;
		// Strip Z bounds: not used by transparent kernel (depth is
		// already in the xpar G-buffer). Set sentinels.
		tl.zMin = -std::numeric_limits<float>::infinity();
		tl.zMax =  std::numeric_limits<float>::infinity();
	}
	g_numStripLights = numStrips;
}

// Fast scalar 1/sqrt(x) via NEON's frsqrte + one Newton-Raphson step.
// arm64 fsqrt+fdiv is ~24 cycles serial; this is ~5. Accuracy is around
// 12 bits — fine for diffuse shading, far below the visual threshold.
// Falls back to portable 1.0f / sqrtf(x) on non-arm64 builds (Clang on
// x86-64 with -msse will emit RSQRTSS + 1 NR for an equivalent cost).
static inline float fast_rsqrt(float x) {
#if defined(__ARM_NEON) || defined(__aarch64__)
	float32x2_t v = vdup_n_f32(x);
	float32x2_t e = vrsqrte_f32(v);
	// One NR step: e' = e * (3 - x*e^2)/2 (vrsqrts implements that)
	e = vmul_f32(vrsqrts_f32(vmul_f32(e, e), v), e);
	return vget_lane_f32(e, 0);
#else
	return 1.0f / std::sqrt(x);
#endif
}

// x^N via binary exponentiation, resolved at compile time. Bit-exact
// vs std::pow for integer N (within float precision; each squaring
// loses ~½ ULP). Optimal op count: 5 squarings + 1 mul for N=48, 6
// squarings for N=64, 7 for N=128, etc. — TMP unrolls the recursion
// into a flat sequence of fmuls. Works for any N ≥ 1; the scalar and
// SIMD overloads of sq()/fmul() let one template body cover both
// float and __m256.
static inline float  sq(float  x) { return x * x; }
static inline __m256 sq(__m256 x) { return _mm256_mul_ps(x, x); }
static inline float  fmul(float  a, float  b) { return a * b; }
static inline __m256 fmul(__m256 a, __m256 b) { return _mm256_mul_ps(a, b); }

template<int N, typename T>
static inline T pow_squaring(T x) {
	static_assert(N >= 1, "pow_squaring: N must be a positive integer");
	if constexpr (N == 1)         return x;
	else if constexpr (N % 2 == 0) return sq(pow_squaring<N/2,   T>(x));
	else                           return fmul(x, pow_squaring<N-1, T>(x));
}

// Runtime dispatch on Glossiness. The TMP template makes adding a new
// case trivial: just list the gloss value here. Falls back to std::pow
// for anything unlisted (FDS_DEFERRED_GLOSS_STATS will flag it).
static inline float pow_glossClass(float x, unsigned gloss) {
	switch (gloss) {
		case 4:   return pow_squaring<4,   float>(x);
		case 8:   return pow_squaring<8,   float>(x);
		case 16:  return pow_squaring<16,  float>(x);
		case 32:  return pow_squaring<32,  float>(x);
		case 48:  return pow_squaring<48,  float>(x);
		case 64:  return pow_squaring<64,  float>(x);
		case 128: return pow_squaring<128, float>(x);
		default:  return std::pow(x, float(gloss));
	}
}

// Vec8f spec accumulator: walks the per-tile light list, computes
// Blinn-Phong half-vector + N·H, raises to the gloss power via
// pow_squaring<Gloss>, accumulates into sB/sG/sR with the same range
// + falloff masks the diffuse loop uses. Called only when wantSpecular
// is true and Mat->Glossiness matches one of our specialized gloss
// constants (the kernel switches on it; default falls back to scalar
// libm pow). Replaces the scalar spec loop the vec path used before.
template<int Gloss>
static inline void run_vec_spec_loop(const TileLights &tl,
                                      float x, float y, float z,
                                      float nx, float ny, float nz,
                                      float vx, float vy, float vz,
                                      float matSpec,
                                      float &sB, float &sG, float &sR) {
	__m256 vx_v   = _mm256_set1_ps(x);
	__m256 vy_v   = _mm256_set1_ps(y);
	__m256 vz_v   = _mm256_set1_ps(z);
	__m256 vnx_v  = _mm256_set1_ps(nx);
	__m256 vny_v  = _mm256_set1_ps(ny);
	__m256 vnz_v  = _mm256_set1_ps(nz);
	__m256 vvx_v  = _mm256_set1_ps(vx);
	__m256 vvy_v  = _mm256_set1_ps(vy);
	__m256 vvz_v  = _mm256_set1_ps(vz);
	__m256 vSpec  = _mm256_set1_ps(matSpec);
	__m256 vZero  = _mm256_setzero_ps();
	__m256 vOne   = _mm256_set1_ps(1.0f);
	__m256 accB   = _mm256_setzero_ps();
	__m256 accG   = _mm256_setzero_ps();
	__m256 accR   = _mm256_setzero_ps();

	for (int slot = 0; slot < tl.paddedCount; slot += 8) {
		__m256 lpx = _mm256_load_ps(tl.posX   + slot);
		__m256 lpy = _mm256_load_ps(tl.posY   + slot);
		__m256 lpz = _mm256_load_ps(tl.posZ   + slot);
		__m256 lcb = _mm256_load_ps(tl.colB   + slot);
		__m256 lcg = _mm256_load_ps(tl.colG   + slot);
		__m256 lcr = _mm256_load_ps(tl.colR   + slot);
		__m256 lr2 = _mm256_load_ps(tl.range2 + slot);
		__m256 lrr = _mm256_load_ps(tl.rRange + slot);

		__m256 wx = _mm256_sub_ps(lpx, vx_v);
		__m256 wy = _mm256_sub_ps(lpy, vy_v);
		__m256 wz = _mm256_sub_ps(lpz, vz_v);
		__m256 dot = _mm256_fmadd_ps(wx, vnx_v,
		              _mm256_fmadd_ps(wy, vny_v,
		               _mm256_mul_ps(wz, vnz_v)));
		__m256 len2 = _mm256_fmadd_ps(wx, wx,
		               _mm256_fmadd_ps(wy, wy,
		                _mm256_mul_ps(wz, wz)));

		__m256 mask_range = _mm256_cmp_ps(len2, lr2,   _CMP_LE_OQ);
		__m256 mask_dot   = _mm256_cmp_ps(dot,  vZero, _CMP_GE_OQ);
		__m256 mask_pos   = _mm256_cmp_ps(len2, vZero, _CMP_GT_OQ);
		__m256 mask       = _mm256_and_ps(mask_range,
		                     _mm256_and_ps(mask_dot, mask_pos));

		__m256 safe_len2 = _mm256_blendv_ps(vOne, len2, mask);
		__m256 lenInv    = _mm256_rsqrt_ps(safe_len2);
		__m256 dist      = _mm256_mul_ps(safe_len2, lenInv);
		__m256 falloff   = _mm256_sub_ps(vOne, _mm256_mul_ps(dist, lrr));

		// Light direction (normalized w). half = L + V, renormalize.
		__m256 ldx = _mm256_mul_ps(wx, lenInv);
		__m256 ldy = _mm256_mul_ps(wy, lenInv);
		__m256 ldz = _mm256_mul_ps(wz, lenInv);
		__m256 hx  = _mm256_add_ps(ldx, vvx_v);
		__m256 hy  = _mm256_add_ps(ldy, vvy_v);
		__m256 hz  = _mm256_add_ps(ldz, vvz_v);
		__m256 hLen2 = _mm256_fmadd_ps(hx, hx,
		                _mm256_fmadd_ps(hy, hy,
		                 _mm256_mul_ps(hz, hz)));
		__m256 mask_hpos = _mm256_cmp_ps(hLen2, vZero, _CMP_GT_OQ);
		__m256 safe_h2   = _mm256_blendv_ps(vOne, hLen2, mask_hpos);
		__m256 hLenInv   = _mm256_rsqrt_ps(safe_h2);
		hx = _mm256_mul_ps(hx, hLenInv);
		hy = _mm256_mul_ps(hy, hLenInv);
		hz = _mm256_mul_ps(hz, hLenInv);
		__m256 NdotH = _mm256_fmadd_ps(hx, vnx_v,
		                _mm256_fmadd_ps(hy, vny_v,
		                 _mm256_mul_ps(hz, vnz_v)));
		__m256 mask_nh = _mm256_cmp_ps(NdotH, vZero, _CMP_GT_OQ);
		// Clamp NdotH to [0,1] before squaring so the chain is stable
		// even for masked-out lanes.
		__m256 safeNH = _mm256_max_ps(NdotH, vZero);
		safeNH        = _mm256_min_ps(safeNH, vOne);
		__m256 spec   = pow_squaring<Gloss, __m256>(safeNH);
		__m256 strength = _mm256_mul_ps(_mm256_mul_ps(spec, vSpec), falloff);
		// Apply combined mask (range + dot ≥ 0 + h positive + NdotH > 0)
		__m256 fullMask = _mm256_and_ps(mask, _mm256_and_ps(mask_hpos, mask_nh));
		__m256 contrib  = _mm256_blendv_ps(vZero, strength, fullMask);

		accB = _mm256_fmadd_ps(contrib, lcb, accB);
		accG = _mm256_fmadd_ps(contrib, lcg, accG);
		accR = _mm256_fmadd_ps(contrib, lcr, accR);
	}
	alignas(32) float bufB[8], bufG[8], bufR[8];
	_mm256_store_ps(bufB, accB);
	_mm256_store_ps(bufG, accG);
	_mm256_store_ps(bufR, accR);
	for (int i = 0; i < 8; ++i) {
		sB += bufB[i];
		sG += bufG[i];
		sR += bufR[i];
	}
}

// FDS_DEFERRED_VEC=1 in env switches the per-pixel inner loop from the
// scalar branch-predicted early-out to a Vec8f SoA accumulation. Stays
// available for A/B benchmarking — on arm64-via-simde it's slower than
// scalar (Vec8f decomposes to two 128-bit NEON ops with full per-lane
// work, no early-out), but the scaffolding is in place to revisit on
// AVX2 / native NEON / wider lanes.
static bool deferredLightingVecEnabled() {
	static bool cached = []{
		const char *s = std::getenv("FDS_DEFERRED_VEC");
		return s && s[0] == '1';
	}();
	return cached;
}

// FDS_DEFERRED_OUTER_VEC=1 selects the outer-SIMD kernel which
// processes 8 pixels per row simultaneously rather than 8 omnis per
// pixel. The setup work (Z-decode, screen→view position reconstruct,
// oct normal decode, ambient compute) amortizes across a vec lane;
// the per-pixel scalar mat-table chase and texel gather stay scalar
// since arm64-via-simde has no usable vgather. The omni loop becomes
// "1 omni × 8 pixels per iter" — each omni's position/color is a
// broadcast, the per-pixel normal/pos/color stay in vec registers.
// Env override: FDS_DEFERRED_OUTER_VEC=1 forces on, =0 forces off,
// unset = follow the per-scene Scene::PreferOuterVec policy. Each
// Initialize_<scene> sets the flag based on whether the scene is
// dominated by matte materials (city, fountain, crash) or by spec/nmap
// shiny ones (greets — outer kernel pays per-lane nmap + scalar
// fallback for most pixels).
static int outerVecEnvSetting() {
	// 0 = follow scene policy, 1 = force on, -1 = force off.
	static int cached = []{
		const char *s = std::getenv("FDS_DEFERRED_OUTER_VEC");
		if (!s) return 0;
		if (s[0] == '0') return -1;
		if (s[0] == '1') return 1;
		return 0;
	}();
	return cached;
}

static bool deferredLightingOuterVecEnabled() {
	const int env = outerVecEnvSetting();
	if (env != 0) return env > 0;
	return CurScene && CurScene->PreferOuterVec != 0;
}

// FDS_DEFERRED_CHECKERBOARD=1 enables half-rate lighting: only pixels
// where (px + py) & 1 == 0 (the "even" cells of a checkerboard) get the
// full per-pixel omni evaluation; the odd cells are filled in a second
// wave by averaging two of their already-shaded neighbors when those
// share matID, falling back to full shade when they don't (e.g. across
// material edges). ~50% of pixels skip the omni loop entirely.
static bool deferredLightingCheckerboardEnabled() {
	static bool cached = []{
		const char *s = std::getenv("FDS_DEFERRED_CHECKERBOARD");
		return s && s[0] == '1';
	}();
	return cached;
}

// FDS_DEFERRED_QUARTER=1 enables quarter-rate lighting: only pixels
// where (px&1)==0 AND (py&1)==0 (one corner of every 2×2 block) get
// the full kernel; the other 3 of each 2×2 are filled in wave 2 by
// direction-specific interpolation:
//   (odd, even) → horizontal: avg of left+right shaded neighbors
//   (even, odd) → vertical:   avg of top+bottom shaded neighbors
//   (odd,  odd) → diagonal:   avg of 4 corners (top-left, top-right,
//                                              bottom-left, bottom-right)
// matID-mismatch detection at each pattern → fall back to a full
// shade for that pixel. ~75% of pixels skip the omni loop. Mutually
// exclusive with checkerboard (this one wins when both are set).
static bool deferredLightingQuarterEnabled() {
	static bool cached = []{
		const char *s = std::getenv("FDS_DEFERRED_QUARTER");
		return s && s[0] == '1';
	}();
	return cached;
}

static void Render_DeferredLighting_Tile(const DeferredLightingCtx &ctx,
                                          int tileIndex,
                                          int x1, int y1, int x2, int y2)
{
	const meka::GBuffer &gb = *ctx.gb;
	dword *out = reinterpret_cast<dword *>(VPage);
	// Hoist mode/global queries once per tile — a `getenv()`-backed
	// cached bool still costs a function call + load + test that the
	// compiler can't see through the std::function/lambda boundary.
	// 2M function calls/frame adds up.
	const bool quarter        = deferredLightingQuarterEnabled();
	const bool checker        = deferredLightingCheckerboardEnabled() && !quarter;
	const bool useVec         = deferredLightingVecEnabled();
	const bool specGlobalOn   = Specular_Factor > 0.0f;

	// Per-stage ablation gates. Read-once env vars; set one to '1' to
	// short-circuit that stage so a bench harness can measure its cost
	// from the frame-time delta (see `--bench=scene@...`).
	static const bool profNoTex = []{
		const char *s = std::getenv("FDS_PROF_NO_TEX");
		return s && s[0] == '1';
	}();
	static const bool profNoLights = []{
		const char *s = std::getenv("FDS_PROF_NO_LIGHTS");
		return s && s[0] == '1';
	}();
	static const bool profNoSpec = []{
		const char *s = std::getenv("FDS_PROF_NO_SPEC");
		return s && s[0] == '1';
	}();
	static const bool profNoFog = []{
		const char *s = std::getenv("FDS_PROF_NO_FOG");
		return s && s[0] == '1';
	}();

	for (int py = y1; py < y2; ++py) {
		for (int px = x1; px < x2; ++px) {
			// Wave-1 of checkerboard: skip odd cells (filled by the
			// fill-pass after all wave-1 tiles complete).
			if (checker && ((px ^ py) & 1)) continue;
			if (quarter && ((px | py) & 1)) continue;  // shade only (even, even)

			const size_t i = size_t(py) * XRes + px;
			const word zEnc = ZPage16[i];
			if (zEnc == 0) continue;  // pixel not touched by Mekalele

			// Decode mat32 → matID, miplevel, swizzledUV.
			const uint32_t mat32 = gb.txtr[i];
			const uint32_t miplevel    = (mat32 >> 28) & 0xF;
			const uint32_t matID       = (mat32 >> 20) & 0xFF;
			const uint32_t swizzledUV  = mat32 & 0xFFFFF;
			if (matID >= ctx.matTable.count) continue;
			Material *Mat = ctx.matTable.data[matID];
			if (!Mat || !Mat->Txtr) continue;

			// Texture sample: Mekalele's apply_exact already wrote a
			// swizzled offset into mat32, so it's a direct lookup into
			// the mip's tile-major data. Mipmap[k] is byte*; texels are
			// dword (B,G,R,A in low→high bytes).
			float texB, texG, texR;
			if (profNoTex) {
				texB = texG = texR = 128.0f;
			} else {
				const dword *texData = (const dword *)Mat->Txtr->Mipmap[miplevel];
				if (!texData) continue;
				const dword texel = texData[swizzledUV];
				texB = float(texel & 0xFF);
				texG = float((texel >> 8) & 0xFF);
				texR = float((texel >> 16) & 0xFF);
			}

			// Decode normal (view-space, unit-length).
			float nx, ny, nz;
			meka::oct_decode_u16(gb.normal[i], nx, ny, nz);
			// Save the geometric (vertex-interpolated, un-perturbed)
			// normal before the normal-map block below mutates nx/ny/nz.
			// Used by the shadow slope-bias so per-pixel bump-induced
			// N·L variance doesn't pollute the slope decision (which is
			// about the underlying surface geometry, not the apparent
			// micro-roughness from the bump map).
			const float nGeoX = nx, nGeoY = ny, nGeoZ = nz;

			// Object-space normal map: if the material ships one, sample
			// it via the same swizzled UV the diffuse uses, decode the
			// (R,G,B) bytes to a unit vector in world space, and replace
			// the geometric N with it. Static meshes only — for dynamic
			// meshes we'd need to transform from mesh-local to world via
			// a per-mesh RotMat, which we don't have at lighting time.
			//
			// FDS_NMAP_FROM_DIFFUSE=1: re-purposes the diffuse texel as
			// the normal map. Visually wrong but lets us measure the
			// per-pixel cost of the normal-map code path on existing
			// scenes without authoring or baking any new textures.
			static const bool nmapFromDiffuse = []{
				const char* s = std::getenv("FDS_NMAP_FROM_DIFFUSE");
				return s && s[0] == '1';
			}();
			// FDS_NO_NMAP=1 forces the kernel to ignore Mat->NormalMap and
			// fall back to the geometric N — useful for A/B comparing
			// the normal-map effect on its own without rebuilding.
			static const bool nmapDisabled = []{
				const char* s = std::getenv("FDS_NO_NMAP");
				return s && s[0] == '1';
			}();
			Texture* nmTex = nmapDisabled ? nullptr : Mat->NormalMap;
			if (!nmTex && nmapFromDiffuse) nmTex = Mat->Txtr;
			if (nmTex) {
				const dword *nmData = (const dword *)nmTex->Mipmap[miplevel];
				if (nmData) {
					const dword nmTexel = nmData[swizzledUV];
					const float nmX = (float((nmTexel >> 16) & 0xFF) * (1.0f/255.0f)) * 2.0f - 1.0f;
					const float nmY = (float((nmTexel >>  8) & 0xFF) * (1.0f/255.0f)) * 2.0f - 1.0f;
					const float nmZ = (float( nmTexel        & 0xFF) * (1.0f/255.0f)) * 2.0f - 1.0f;
					// Tier B tangent-space normal map. Reconstruct view-space
					// N from TBN where T is the per-pixel interpolated tangent
					// from the rasterizer's tangent G-buffer (Gram-Schmidt'd
					// vs N at write time; we re-orthogonalize defensively after
					// nlerp + oct-quant round-trip). When the tangent slot is
					// empty (transparent layers, or vertex tangent collapsed)
					// fall back to a Mikkelsen-style on-the-fly tangent so the
					// effect is still visible — visually wrong rotation but
					// the right per-pixel cost.
					float tx = 0, ty = 0, tz = 0;
					bool tangentValid = false;
					if (!gb.tangent.empty()) {
						const meka::u16 packedT = gb.tangent[i];
						if (packedT != 0) {
							meka::oct_decode_u16(packedT, tx, ty, tz);
							// Re-orthogonalize after the lossy oct round-trip.
							const float tDotN = tx*nx + ty*ny + tz*nz;
							tx -= nx * tDotN;
							ty -= ny * tDotN;
							tz -= nz * tDotN;
							const float tLen2 = tx*tx + ty*ty + tz*tz;
							if (tLen2 > 1e-12f) {
								const float invTLen = 1.0f / std::sqrt(tLen2);
								tx *= invTLen; ty *= invTLen; tz *= invTLen;
								tangentValid = true;
							}
						}
					}
					if (!tangentValid) {
						// Mikkelsen fallback: pick a reference axis not
						// parallel to N, T = normalize(cross(ref, N)).
						float refx, refy, refz;
						if (std::fabs(ny) < 0.9f) { refx = 0; refy = 1; refz = 0; }
						else                       { refx = 1; refy = 0; refz = 0; }
						tx = refy * nz - refz * ny;
						ty = refz * nx - refx * nz;
						tz = refx * ny - refy * nx;
						const float invTLen = 1.0f / std::sqrt(tx*tx + ty*ty + tz*tz);
						tx *= invTLen; ty *= invTLen; tz *= invTLen;
					}
					// B = N × T
					const float bx = ny * tz - nz * ty;
					const float by = nz * tx - nx * tz;
					const float bz = nx * ty - ny * tx;
					// new_N = T * nmX + B * nmY + N * nmZ (view space)
					float vnx = tx * nmX + bx * nmY + nx * nmZ;
					float vny = ty * nmX + by * nmY + ny * nmZ;
					float vnz = tz * nmX + bz * nmY + nz * nmZ;
					float invLen = 1.0f / std::sqrt(vnx*vnx + vny*vny + vnz*vnz);
					nx = vnx * invLen;
					ny = vny * invLen;
					nz = vnz * invLen;
				}
			}

			// Reconstruct view-space position. ZPage16 stores
			// 0xFF80 - round(g_zscale * z), so:
			//   z = (0xFF80 - zEnc) / g_zscale
			const float z = float(0xFF80 - zEnc) * ctx.invZScale;
			const float x = (float(px) - CntrEX) * z * ctx.invFOVX;
			const float y = (CntrEY - float(py)) * z * ctx.invFOVY;

			// Ambient. Same expression as forward Lighting() — except
			// here Mat is per-pixel, not per-mesh-Mat[0].
			float lB, lG, lR;
			if (Mat->Txtr) {
				lB = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.B;
				lG = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.G;
				lR = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.R;
			} else {
				lB = Mat->Luminosity * Mat->BaseCol.B + Mat->Diffuse * ctx.Sc->Ambient.B;
				lG = Mat->Luminosity * Mat->BaseCol.G + Mat->Diffuse * ctx.Sc->Ambient.G;
				lR = Mat->Luminosity * Mat->BaseCol.R + Mat->Diffuse * ctx.Sc->Ambient.R;
			}

			// View direction (pixel -> camera) in view space. Camera is at
			// the view-space origin, so view_dir = -pos / |pos|. Used by
			// the Blinn-Phong specular term below; cheap to skip for matte
			// materials. Glossiness == 0 + Specular > 0 means the FLD
			// authored a specular value but didn't set a Phong exponent;
			// fall back to a sensible mid-sharpness default so we still
			// see a highlight rather than a no-op.
			//
			// Specular_Factor gates the entire highlight pass, mirroring
			// forward's per-scene tone control (CITY/GREETS set it to 0
			// to suppress specular; FOUNTAIN/CRASH leave it nonzero). The
			// deferred path was previously emitting Blinn-Phong even when
			// the scene had explicitly disabled it, which over-brightened
			// City windows + Greets marble against the forward reference.
			// FDS_DEFERRED_NO_SPEC=1: force-disable all deferred specular,
			// regardless of per-material Mat->Specular. Diagnostic for
			// specular firefly / bump-map speckle isolation.
			static const bool sNoSpec = []{
				const char *s = std::getenv("FDS_DEFERRED_NO_SPEC");
				return s && s[0] == '1';
			}();
			const bool wantSpecular = !sNoSpec && specGlobalOn && (Mat->Specular > 0.0f) && !profNoSpec;
			const float gloss = Mat->Glossiness > 0
				? float(Mat->Glossiness)
				: 32.0f;
			float vx = 0, vy = 0, vz = 0;
			if (wantSpecular) {
				const float vlen2 = x*x + y*y + z*z;
				const float vlenInv = 1.0f / std::sqrt(vlen2);
				vx = -x * vlenInv;
				vy = -y * vlenInv;
				vz = -z * vlenInv;
			}

			// Specular accumulator — kept separate from diffuse so it can
			// be added AFTER texture modulation (highlights are independent
			// of base-color tint, which is the standard model).
			float sB = 0, sG = 0, sR = 0;

			// Water gets ambient-only (no omni accumulation, no spec).
			// Forward water mesh's BSphereRadius is tiny (Reflective_
			// Surface_Setup tessellates and scales the verts but never
			// recomputes the bsphere) and forward Lighting() skips any
			// omni whose `BSphereRadius + Range < |omni-bsphere_ctr|` —
			// which is basically all of them — so forward water vertex
			// light is just `Mat->Diffuse * Sc->Ambient`. Per-pixel
			// distance tests in the deferred path don't have that cull,
			// so without this skip every nearby city light slams into
			// every water pixel and saturates lB at 250.
			const bool isWater = (int(matID) == ctx.waterMatID);

			// Per-tile filtered light list — only omnis whose screen-
			// space bounding sphere overlaps this tile. For City with
			// ~100 omnis spread across the city, each tile typically
			// sees 10-20.
			const TileLights &tl = ctx.tileLights[tileIndex];

			// Per-pixel normal-map sampling lives only in the scalar
			// path. Force scalar for any pixel whose material has a
			// normal map (or when FDS_NMAP_FROM_DIFFUSE forces it for
			// every textured material). The geometric N from the
			// G-buffer would otherwise drive vec lighting and the
			// normal-map perturbation would be lost.
			static const bool nmapFromDiffuse_useVec = []{
				const char* s = std::getenv("FDS_NMAP_FROM_DIFFUSE");
				return s && s[0] == '1';
			}();
			const bool hasNormalMap = Mat->NormalMap ||
				(nmapFromDiffuse_useVec && Mat->Txtr);
			const bool useVecHere = useVec && !hasNormalMap;

			if (!isWater && !profNoLights) {
				if (useVecHere) {
					// 8-wide SIMD inner loop — written directly against
					// simde's _mm256_* intrinsics so we bypass
					// vectorclass's Vec8fb byte-packed mask layout.
					// vectorclass needs 6 NEON instructions per compare
					// to coerce the natural uint32x4_t mask into its
					// portable byte form (xtn → uzp1 → zip2 → ushll →
					// shl → cmlt) plus cross-domain GPR roundtrips for
					// `&`. simde's intrinsics produce masks in the
					// native AVX2 layout (all-1s/all-0s lanes, same as
					// what NEON `vcgeq_f32` returns), so `_mm256_and_ps`
					// is a direct AND with no conversion. simde lowers
					// each intrinsic to native NEON on arm64 / native
					// AVX2 on x86, giving us a single cross-platform
					// hot path.
					__m256 vx_v   = _mm256_set1_ps(x);
					__m256 vy_v   = _mm256_set1_ps(y);
					__m256 vz_v   = _mm256_set1_ps(z);
					__m256 vnx_v  = _mm256_set1_ps(nx);
					__m256 vny_v  = _mm256_set1_ps(ny);
					__m256 vnz_v  = _mm256_set1_ps(nz);
					__m256 vDiff  = _mm256_set1_ps(Mat->Diffuse);
					__m256 vZero  = _mm256_setzero_ps();
					__m256 vOne   = _mm256_set1_ps(1.0f);
					__m256 accB   = _mm256_setzero_ps();
					__m256 accG   = _mm256_setzero_ps();
					__m256 accR   = _mm256_setzero_ps();

					for (int slot = 0; slot < tl.paddedCount; slot += 8) {
						__m256 lpx = _mm256_load_ps(tl.posX   + slot);
						__m256 lpy = _mm256_load_ps(tl.posY   + slot);
						__m256 lpz = _mm256_load_ps(tl.posZ   + slot);
						__m256 lcb = _mm256_load_ps(tl.colB   + slot);
						__m256 lcg = _mm256_load_ps(tl.colG   + slot);
						__m256 lcr = _mm256_load_ps(tl.colR   + slot);
						__m256 lr2 = _mm256_load_ps(tl.range2 + slot);
						__m256 lrr = _mm256_load_ps(tl.rRange + slot);

						__m256 wx = _mm256_sub_ps(lpx, vx_v);
						__m256 wy = _mm256_sub_ps(lpy, vy_v);
						__m256 wz = _mm256_sub_ps(lpz, vz_v);
						__m256 dot = _mm256_fmadd_ps(wx, vnx_v,
						              _mm256_fmadd_ps(wy, vny_v,
						               _mm256_mul_ps(wz, vnz_v)));
						__m256 len2 = _mm256_fmadd_ps(wx, wx,
						               _mm256_fmadd_ps(wy, wy,
						                _mm256_mul_ps(wz, wz)));

						__m256 mask_range = _mm256_cmp_ps(len2, lr2,   _CMP_LE_OQ);
						__m256 mask_dot   = _mm256_cmp_ps(dot,  vZero, _CMP_GE_OQ);
						__m256 mask_pos   = _mm256_cmp_ps(len2, vZero, _CMP_GT_OQ);
						__m256 mask = _mm256_and_ps(mask_range,
						               _mm256_and_ps(mask_dot, mask_pos));

						__m256 safe_len2 = _mm256_blendv_ps(vOne, len2, mask);
						__m256 lenInv = _mm256_rsqrt_ps(safe_len2);
						__m256 dist   = _mm256_mul_ps(safe_len2, lenInv);
						__m256 falloff = _mm256_sub_ps(vOne, _mm256_mul_ps(dist, lrr));
						__m256 k = _mm256_mul_ps(_mm256_mul_ps(dot, lenInv), falloff);

						// Spot cone attenuation. For lanes where isSpot==0
						// coneAtten=1 (omni). For spot lanes: smoothstep
						// from cosOuter→cosInner of cosTheta=-dot(Dir,w)/|w|.
						__m256 ldx = _mm256_load_ps(tl.dirX     + slot);
						__m256 ldy = _mm256_load_ps(tl.dirY     + slot);
						__m256 ldz = _mm256_load_ps(tl.dirZ     + slot);
						__m256 lci = _mm256_load_ps(tl.cosInner + slot);
						__m256 lco = _mm256_load_ps(tl.cosOuter + slot);
						__m256i lis = _mm256_load_si256((const __m256i*)(tl.isSpot + slot));
						__m256 dirDotW = _mm256_fmadd_ps(ldx, wx,
						                  _mm256_fmadd_ps(ldy, wy,
						                   _mm256_mul_ps(ldz, wz)));
						__m256 cosTheta = _mm256_mul_ps(
						                    _mm256_sub_ps(vZero, dirDotW),
						                    lenInv);
						__m256 maskInside = _mm256_cmp_ps(cosTheta, lco, _CMP_GT_OQ);
						__m256 rangeRcp = _mm256_div_ps(vOne, _mm256_sub_ps(lci, lco));
						__m256 t = _mm256_mul_ps(_mm256_sub_ps(cosTheta, lco), rangeRcp);
						t = _mm256_min_ps(_mm256_max_ps(t, vZero), vOne);
						// smoothstep(t) = t*t*(3-2t)
						__m256 smooth = _mm256_mul_ps(_mm256_mul_ps(t, t),
						                  _mm256_sub_ps(_mm256_set1_ps(3.0f),
						                                _mm256_mul_ps(_mm256_set1_ps(2.0f), t)));
						// coneAtten = isSpot ? (maskInside ? smooth : 0) : 1
						__m256 spotAtten = _mm256_and_ps(maskInside, smooth);
						__m256 isSpotMask = _mm256_castsi256_ps(
						                     _mm256_cmpgt_epi32(lis, _mm256_setzero_si256()));
						__m256 coneAtten = _mm256_blendv_ps(vOne, spotAtten, isSpotMask);
						k = _mm256_mul_ps(k, coneAtten);

						__m256 intensity = _mm256_blendv_ps(vZero,
						                    _mm256_mul_ps(k, vDiff), mask);

						accB = _mm256_fmadd_ps(intensity, lcb, accB);
						accG = _mm256_fmadd_ps(intensity, lcg, accG);
						accR = _mm256_fmadd_ps(intensity, lcr, accR);
					}
					// Horizontal reduction: AVX2 has no scalar reduce;
					// store + scalar add is what vectorclass does
					// internally too. 24 scalar adds total per pixel
					// — negligible vs the omni loop body.
					alignas(32) float bufB[8], bufG[8], bufR[8];
					_mm256_store_ps(bufB, accB);
					_mm256_store_ps(bufG, accG);
					_mm256_store_ps(bufR, accR);
					for (int i = 0; i < 8; ++i) {
						lB += bufB[i];
						lG += bufG[i];
						lR += bufR[i];
					}
					// Vec-spec path: replaces the previous scalar pow loop.
					// Dispatch on Mat->Glossiness so the inner loop uses a
					// constant squaring sequence (bit-exact pow for integer
					// gloss, ~5-7 fmuls vs ~50-80 cycles for libm pow).
					// FDS_DEFERRED_GLOSS_STATS confirms our FLDs only ship
					// gloss ∈ {48, 64}; the other cases here are defensive
					// for spectest's authored values and future FLDs.
					if (wantSpecular) {
						switch (Mat->Glossiness) {
							case 4:
								run_vec_spec_loop<4>  (tl, x,y,z, nx,ny,nz, vx,vy,vz, Mat->Specular, sB,sG,sR); break;
							case 8:
								run_vec_spec_loop<8>  (tl, x,y,z, nx,ny,nz, vx,vy,vz, Mat->Specular, sB,sG,sR); break;
							case 16:
								run_vec_spec_loop<16> (tl, x,y,z, nx,ny,nz, vx,vy,vz, Mat->Specular, sB,sG,sR); break;
							case 32:
								run_vec_spec_loop<32> (tl, x,y,z, nx,ny,nz, vx,vy,vz, Mat->Specular, sB,sG,sR); break;
							case 48:
								run_vec_spec_loop<48> (tl, x,y,z, nx,ny,nz, vx,vy,vz, Mat->Specular, sB,sG,sR); break;
							case 64:
								run_vec_spec_loop<64> (tl, x,y,z, nx,ny,nz, vx,vy,vz, Mat->Specular, sB,sG,sR); break;
							case 128:
								run_vec_spec_loop<128>(tl, x,y,z, nx,ny,nz, vx,vy,vz, Mat->Specular, sB,sG,sR); break;
							default:
								// Unknown gloss — defensive scalar libm path
								// (matches old behavior). If this fires on a
								// hot scene, add a `case` for the value.
								for (int n = 0; n < tl.count; ++n) {
									const float Lpx = tl.posX[n];
									const float Lpy = tl.posY[n];
									const float Lpz = tl.posZ[n];
									const float wx = Lpx - x;
									const float wy = Lpy - y;
									const float wz = Lpz - z;
									const float dot = wx*nx + wy*ny + wz*nz;
									if (dot < 0.0f) continue;
									const float len2 = wx*wx + wy*wy + wz*wz;
									const float r2 = tl.range2[n];
									if (len2 > r2 || len2 == 0.0f) continue;
									const float lenInv = fast_rsqrt(len2);
									const float dist   = len2 * lenInv;
									const float ldx = wx * lenInv;
									const float ldy = wy * lenInv;
									const float ldz = wz * lenInv;
									float hx = ldx + vx, hy = ldy + vy, hz = ldz + vz;
									const float hLen2 = hx*hx + hy*hy + hz*hz;
									if (hLen2 <= 0.0f) continue;
									const float hLenInv = fast_rsqrt(hLen2);
									hx *= hLenInv; hy *= hLenInv; hz *= hLenInv;
									const float NdotH = nx*hx + ny*hy + nz*hz;
									if (NdotH <= 0.0f) continue;
									const float spec = std::pow(NdotH, gloss);
									const float rRange = tl.rRange[n];
									const float specStrength = spec * Mat->Specular *
										(1.0f - dist * rRange);
									sB += specStrength * tl.colB[n];
									sG += specStrength * tl.colG[n];
									sR += specStrength * tl.colR[n];
								}
								break;
						}
					}
				} else {
					// Scalar path (default). Per-omni early-out on
					// dot/range tests; branch predictor learns the
					// "most filtered omnis still out of range" pattern.
					// `dist = len2 * lenInv` (= sqrt(len2)) avoids a
					// second fdiv vs `1/lenInv` — fdiv on arm64 is
					// ~20 cycles vs fmul's 4. The compiler doesn't do
					// this rewrite under strict FP, so spell it out.
					for (int n = 0; n < tl.count; ++n) {
						const float Lpx = tl.posX[n];
						const float Lpy = tl.posY[n];
						const float Lpz = tl.posZ[n];
						const float wx = Lpx - x;
						const float wy = Lpy - y;
						const float wz = Lpz - z;
						const float dot = wx * nx + wy * ny + wz * nz;
						if (dot < 0.0f) continue;
						const float len2 = wx*wx + wy*wy + wz*wz;
						const float r2 = tl.range2[n];
						if (len2 > r2) continue;
						const float lenInv = fast_rsqrt(len2);
						const float dist   = len2 * lenInv;
						const float rRange = tl.rRange[n];
						float k = dot * lenInv * (1.0f - dist * rRange);
						// Spot cone: matches the vec body's coneAtten so nmap
						// pixels (which all flow through this scalar path)
						// don't render the robot spotlight as an omni.
						if (tl.isSpot[n]) {
							const float cosTheta = -(tl.dirX[n]*wx + tl.dirY[n]*wy + tl.dirZ[n]*wz) * lenInv;
							if (cosTheta <= tl.cosOuter[n]) continue;
							if (cosTheta < tl.cosInner[n]) {
								const float ct = (cosTheta - tl.cosOuter[n]) / (tl.cosInner[n] - tl.cosOuter[n]);
								k *= ct * ct * (3.0f - 2.0f * ct);
							}
						}
						// Shadow test. Project pixel view-space pos to the
						// shadow map's screen+depth; attenuate the light
						// by the occlusion fraction. Cheap branch when
						// smIdx<0 (no shadow).
						//
						// PCF with bilinear-weighted 2×2: sample 4 neighbouring
						// shadow-map texels and blend the per-texel
						// occlusion by fractional (smX, smY) — continuous
						// in light-space position, no integer-snap when
						// the light camera moves a sub-texel between
						// frames. Without bilinear weighting whole
						// polygons that project to a single texel flicker
						// in/out as the texel-grid shifts under them.
						// Cost: 4 loads + 4 compares + 4 muls + 4 adds.
						const int32_t smIdx = tl.shadowMapIdx[n];
						float shadowAtten = 1.0f;
						if (smIdx >= 0 && size_t(smIdx) < g_shadowMaps.size()) {
							const ShadowMap& sm = g_shadowMaps[smIdx];
							const float lx = sm.viewToLight[0][0] * x +
							                 sm.viewToLight[0][1] * y +
							                 sm.viewToLight[0][2] * z +
							                 sm.viewToLightOffset.x;
							const float ly = sm.viewToLight[1][0] * x +
							                 sm.viewToLight[1][1] * y +
							                 sm.viewToLight[1][2] * z +
							                 sm.viewToLightOffset.y;
							const float lz = sm.viewToLight[2][0] * x +
							                 sm.viewToLight[2][1] * y +
							                 sm.viewToLight[2][2] * z +
							                 sm.viewToLightOffset.z;
							if (lz > 0.0f) {
								const float invLZ = 1.0f / lz;
								const float smX = sm.cntrX + sm.perspX * lx * invLZ;
								const float smY = sm.cntrY - sm.perspY * ly * invLZ;
								const int iX = int(smX);
								const int iY = int(smY);
								if (iX >= 0 && iX + 1 < sm.xres &&
								    iY >= 0 && iY + 1 < sm.yres) {
									const uint16_t *zRow0 = sm.depth.data() +
										size_t(iY) * size_t(sm.xres);
									const uint16_t *zRow1 = zRow0 + sm.xres;
									const float fx = smX - float(iX);
									const float fy = smY - float(iY);
									const float w00 = (1.0f - fx) * (1.0f - fy);
									const float w10 =         fx  * (1.0f - fy);
									const float w01 = (1.0f - fx) *         fy;
									const float w11 =         fx  *         fy;
									const ShadowMode mode = g_shadowMode.load(std::memory_order_relaxed);
									float occ = 0.0f;
									if (mode == ShadowMode::PolyId) {
										const uint8_t *idRow0 = sm.polyId.data() +
											size_t(iY) * size_t(sm.xres);
										const uint8_t *idRow1 = idRow0 + sm.xres;
										// Surface matID extracted from gb.txtr's
										// packed (miplevel:4 | matID:8 | swizzledUV:20).
										// Shadow buffer stores matID+1 of the closest
										// occluder; +1 here too so the comparison
										// uses the same offset, and 0 stays as the
										// "no occluder" sentinel.
										const uint8_t surfaceId = uint8_t(matID + 1);
										if (idRow0[iX    ] != surfaceId && idRow0[iX    ] != 0) occ += w00;
										if (idRow0[iX + 1] != surfaceId && idRow0[iX + 1] != 0) occ += w10;
										if (idRow1[iX    ] != surfaceId && idRow1[iX    ] != 0) occ += w01;
										if (idRow1[iX + 1] != surfaceId && idRow1[iX + 1] != 0) occ += w11;
									} else {
										int pixZenc = 0xFF80 - int(lz * sm.zScale);
										if (pixZenc < 0) pixZenc = 0;
										if (pixZenc > 0xFFFF) pixZenc = 0xFFFF;
										// Constant + slope-scale bias. The constant
										// handles front-facing self-shadow acne. The
										// slope term grows as the surface meets the
										// light at shallow angles — a wall almost
										// parallel to the light direction has a steep
										// depth gradient across one shadow texel, so
										// the constant bias alone leaves grazing
										// surfaces with banded "acne" patterns. We
										// scale by 1/(N·L) - 1 so front-facing surfaces
										// get no extra; grazing surfaces get a lot.
										// Tunable via FDS_SHADOW_BIAS / SLOPE_BIAS.
										static const int kShadowBias = []{
											const char *s = std::getenv("FDS_SHADOW_BIAS");
											return s ? std::atoi(s) : 512;
										}();
										static const int kSlopeBias = []{
											const char *s = std::getenv("FDS_SHADOW_SLOPE_BIAS");
											return s ? std::atoi(s) : 1024;
										}();
										// Use the GEOMETRIC normal (saved before normal-
										// map perturbation) for slope calc — the slope
										// is about the underlying surface, not the
										// bumpy micro-detail. Without this, bump-mapped
										// floors show patchy shadow following the bumps,
										// and grazing-angle bumps explode the slope.
										// Clamp at 0.2 so very steep angles plateau
										// rather than blowing past nearby occluders.
										const float dotGeo = wx*nGeoX + wy*nGeoY + wz*nGeoZ;
										const float nDotL = dotGeo * lenInv;
										const float invNdotL = 1.0f / (nDotL > 0.2f ? nDotL : 0.2f);
										const int slopeBias = int(float(kSlopeBias) * (invNdotL - 1.0f));
										const int biased = pixZenc + kShadowBias + slopeBias;
										if (biased < int(zRow0[iX    ])) occ += w00;
										if (biased < int(zRow0[iX + 1])) occ += w10;
										if (biased < int(zRow1[iX    ])) occ += w01;
										if (biased < int(zRow1[iX + 1])) occ += w11;
									}
									if (occ >= 1.0f) continue;       // fully shadowed
									shadowAtten = 1.0f - occ;
								}
							}
						}
						const float intensity = k * Mat->Diffuse * shadowAtten;
						const float Lcb = tl.colB[n];
						const float Lcg = tl.colG[n];
						const float Lcr = tl.colR[n];
						lB += intensity * Lcb;
						lG += intensity * Lcg;
						lR += intensity * Lcr;

						if (wantSpecular) {
							const float ldx = wx * lenInv;
							const float ldy = wy * lenInv;
							const float ldz = wz * lenInv;
							float hx = ldx + vx;
							float hy = ldy + vy;
							float hz = ldz + vz;
							const float hLen2 = hx*hx + hy*hy + hz*hz;
							if (hLen2 > 0.0f) {
								const float hLenInv = fast_rsqrt(hLen2);
								hx *= hLenInv; hy *= hLenInv; hz *= hLenInv;
								const float NdotH = nx*hx + ny*hy + nz*hz;
								if (NdotH > 0.0f) {
									const float spec = pow_glossClass(NdotH, Mat->Glossiness);
									const float specStrength = spec * Mat->Specular *
										(1.0f - dist * rRange);
									sB += specStrength * Lcb;
									sG += specStrength * Lcg;
									sR += specStrength * Lcr;
								}
							}
						}
					}
				}
			}

			// Saturation cap (matches forward at 250).
			if (lB > 250.0f) lB = 250.0f;
			if (lG > 250.0f) lG = 250.0f;
			if (lR > 250.0f) lR = 250.0f;
			if (lB < 0.0f)   lB = 0.0f;
			if (lG < 0.0f)   lG = 0.0f;
			if (lR < 0.0f)   lR = 0.0f;

			// Fog moved to Render_DeferredFogPass — a single post-lighting
			// full-screen pass keeps the lighting kernel SIMD-clean and
			// drops the per-pixel sqrt. Forward TheOtherBarry still
			// applies its own per-vertex fog (TheOtherBarry.h:716-734)
			// for reflective windows / additive fountain vortex; those
			// pixels are skipped by the fog pass via the mat32 sentinel.

			// Diffuse modulation: pixel = texel * light / 256.
			// Reflective faces are dispatched to TheOtherBarry<OVERWRITE,
			// TEXTURETEXTURE> in RenderInnerMekalele and never reach this
			// code path — they're rendered by the forward filler (which
			// does the env+tex/2 composite using forward's per-vertex
			// interpolated eu/ev) and skipped here via the mat32 sentinel.
			float fdB = (texB * lB) * (1.0f / 256.0f);
			float fdG = (texG * lG) * (1.0f / 256.0f);
			float fdR = (texR * lR) * (1.0f / 256.0f);
			int outB = int(fdB) + int(sB);
			int outG = int(fdG) + int(sG);
			int outR = int(fdR) + int(sR);

			// Water-mesh transparent blend. Forward draws the water plane
			// with TheOtherBarry<TRANSPARENT> after a pass-1 mirrored-world
			// draw + dispMap distortion has populated VPage with a wavy
			// reflection of the city. The transparent filler does
			//   pixel = saturate(lit_water_texel + existing_VPage/2)
			// (TheOtherBarry.h:392). We reproduce the same blend here:
			// the existing VPage value at this pixel is the reflection
			// preserved across the inter-pass Z-clear (which lets pass-2
			// deferred shading skip non-water-mesh pixels via zEnc check
			// on the freshly-cleared depth buffer).
			if (isWater) {
				const dword existing = out[i];
				const int rB = int(existing & 0xFF);
				const int rG = int((existing >> 8) & 0xFF);
				const int rR = int((existing >> 16) & 0xFF);
				outB += rB >> 1;
				outG += rG >> 1;
				outR += rR >> 1;
			}

			if (outB > 255) outB = 255;
			if (outG > 255) outG = 255;
			if (outR > 255) outR = 255;
			if (outB < 0)   outB = 0;
			if (outG < 0)   outG = 0;
			if (outR < 0)   outR = 0;

			out[i] = dword(outB) | (dword(outG) << 8) | (dword(outR) << 16) | 0xFF000000u;
		}
	}

	std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
	++renderns::tileCounter;
	renderns::condition.notify_one();
}

// Per-pixel deferred lighting for transparent (front-facing) surfaces.
// Reads the transparent G-buffer (mat32 + normal + xpr-Z, populated by
// MekaleleTransparent in RenderInnerDeferredTransparent for the closest
// front-facing transparent at each pixel). Computes per-pixel ambient
// + Lambertian + fog, modulates the texture sample, alpha-blends 50/50
// onto VPage (matches TheOtherBarry<TRANSPARENT>'s blend rule). Specular
// + checkerboard / quarter-rate / vec paths omitted — transparent
// coverage is small, scalar is fine until profiling says otherwise.
// Templated on which transparent layer to light: Front reads
// g_gbufferTransparent + g_xparZ; Back reads g_gbufferTransparentBack +
// g_xparZBack. Both kernels are identical apart from the buffer pointers
// — kept as one function body so they don't drift.
enum class XparLayer { Front, Back };

template <XparLayer Layer>
static void Render_DeferredTransparentLighting_Tile(const DeferredLightingCtx &ctx,
                                                     int tileIndex,
                                                     int x1, int y1, int x2, int y2)
{
	meka::GBuffer *gbPtr;
	uint16_t      *zPtr;
	if constexpr (Layer == XparLayer::Front) {
		gbPtr = g_gbufferTransparent;
		zPtr  = g_xparZ;
	} else {
		gbPtr = g_gbufferTransparentBack;
		zPtr  = g_xparZBack;
	}
	if (!gbPtr || !zPtr) return;
	const meka::GBuffer &gbX = *gbPtr;
	uint16_t *xparZ = zPtr;
	dword *out = reinterpret_cast<dword *>(VPage);
	// FDS_XPAR_LIGHT_ALL=1: bypass per-tile light culling for transparents;
	// iterate the full scene omni list per pixel. Conservatively-correct
	// (no false omits at tile seams) but slower. Useful for diagnosing
	// whether observed lighting steps come from culling drift.
	static const bool lightAll = []{
		const char *s = std::getenv("FDS_XPAR_LIGHT_ALL");
		return s && s[0] == '1';
	}();
	const TileLights &tlTile = ctx.tileLights[tileIndex];
	const ViewLightsSoA *vlAll = ctx.lights;
	const int allCount = ctx.numLights;

	for (int py = y1; py < y2; ++py) {
		for (int px = x1; px < x2; ++px) {
			const size_t i = size_t(py) * XRes + px;
			const uint32_t mat32 = gbX.txtr[i];
			if (mat32 == 0xFFFFFFFFu) continue;  // no transparent front
			const word zEnc = xparZ[i];
			if (zEnc == 0) continue;
			// Opaque-Z occlusion: if opaque ZPage16 has a larger z_candidate
			// at this pixel, the opaque surface is CLOSER to camera than
			// the transparent we wrote into xpr. Reject the transparent —
			// it's hidden behind opaque. MekaleleTransparent doesn't see
			// opaque Z during raster (its zbuffer points at xpr-Z) so we
			// have to gate here. Without this, transparent surfaces show
			// through opaque walls (Greets banding behind marble).
			const word zOpaque = ZPage16[i];
			if (zOpaque > zEnc) continue;

			const uint32_t miplevel   = (mat32 >> 28) & 0xF;
			const uint32_t matID      = (mat32 >> 20) & 0xFF;
			const uint32_t swizzledUV = mat32 & 0xFFFFF;
			if (matID >= ctx.matTable.count) continue;
			Material *Mat = ctx.matTable.data[matID];
			if (!Mat || !Mat->Txtr) continue;

			const dword *texData = (const dword *)Mat->Txtr->Mipmap[miplevel];
			if (!texData) continue;
			const dword texel = texData[swizzledUV];
			const float texB = float(texel & 0xFF);
			const float texG = float((texel >> 8) & 0xFF);
			const float texR = float((texel >> 16) & 0xFF);

			float nx, ny, nz;
			meka::oct_decode_u16(gbX.normal[i], nx, ny, nz);

			const float z = float(0xFF80 - zEnc) * ctx.invZScale;
			const float x = (float(px) - CntrEX) * z * ctx.invFOVX;
			const float y = (CntrEY - float(py)) * z * ctx.invFOVY;

			float lB = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.B;
			float lG = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.G;
			float lR = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.R;

			// Crystal specular for transparent surfaces (e.g. fountain
			// mizraka glass). Same Blinn-Phong as the opaque kernel; gated
			// on Mat->Specular > 0 so non-spec transparents (water, spire
			// orbs) pay zero. Accumulated into sB/sG/sR alongside diffuse
			// and added on top of the texture-modulated lit color (spec
			// highlights aren't tinted by the base texture — standard
			// model).
			const bool wantSpecular = (Mat->Specular > 0.0f);
			const float gloss = Mat->Glossiness > 0 ? float(Mat->Glossiness) : 32.0f;
			float vx_v = 0, vy_v = 0, vz_v = 0;
			if (wantSpecular) {
				const float vlen2 = x*x + y*y + z*z;
				const float vlenInv = 1.0f / std::sqrt(vlen2);
				vx_v = -x * vlenInv;
				vy_v = -y * vlenInv;
				vz_v = -z * vlenInv;
			}
			float sB = 0, sG = 0, sR = 0;

			// Water gets ambient-only — forward Lighting() culls every omni
			// (water mesh has a tiny BSphereRadius from Reflective_Surface_
			// Setup's tessellation, so range+radius < |omni-bsphere| for all
			// city omnis). Without skipping the omni loop here, every nearby
			// city light slams into every water pixel and saturates the
			// channel → cyan/turquoise water instead of forward's deep blue.
			// Mirrors the same skip in Render_DeferredLighting_Tile.
			const bool isWater = (int(matID) == ctx.waterMatID);

			// Light loop. Default: per-tile compacted list. Diagnostic
			// mode: full scene omni list (FDS_XPAR_LIGHT_ALL=1).
			if (isWater) {
				// Skip omni accumulation — matches the opaque kernel's
				// isWater guard.
			} else if (lightAll) {
				for (int n = 0; n < allCount; ++n) {
					const float wx = vlAll->posX[n] - x;
					const float wy = vlAll->posY[n] - y;
					const float wz = vlAll->posZ[n] - z;
					const float dot = wx * nx + wy * ny + wz * nz;
					if (dot < 0.0f) continue;
					const float len2 = wx*wx + wy*wy + wz*wz;
					if (len2 > vlAll->range2[n]) continue;
					const float lenInv = fast_rsqrt(len2);
					const float dist   = len2 * lenInv;
					const float falloff = (1.0f - dist * vlAll->rRange[n]);
					float k = dot * lenInv * falloff;
					if (vlAll->isSpot[n]) {
						const float cosTheta = -(vlAll->dirX[n]*wx + vlAll->dirY[n]*wy + vlAll->dirZ[n]*wz) * lenInv;
						if (cosTheta <= vlAll->cosOuter[n]) continue;
						if (cosTheta < vlAll->cosInner[n]) {
							float t = (cosTheta - vlAll->cosOuter[n]) / (vlAll->cosInner[n] - vlAll->cosOuter[n]);
							k *= t * t * (3.0f - 2.0f * t);
						}
					}
					const float intensity = k * Mat->Diffuse;
					lB += intensity * vlAll->colB[n];
					lG += intensity * vlAll->colG[n];
					lR += intensity * vlAll->colR[n];

					if (wantSpecular) {
						const float ldx = wx * lenInv;
						const float ldy = wy * lenInv;
						const float ldz = wz * lenInv;
						float hx = ldx + vx_v, hy = ldy + vy_v, hz = ldz + vz_v;
						const float hLen2 = hx*hx + hy*hy + hz*hz;
						if (hLen2 <= 0.0f) continue;
						const float hLenInv = fast_rsqrt(hLen2);
						hx *= hLenInv; hy *= hLenInv; hz *= hLenInv;
						const float NdotH = nx*hx + ny*hy + nz*hz;
						if (NdotH <= 0.0f) continue;
						const float specStrength = std::pow(NdotH, gloss) *
							Mat->Specular * falloff;
						sB += specStrength * vlAll->colB[n];
						sG += specStrength * vlAll->colG[n];
						sR += specStrength * vlAll->colR[n];
					}
				}
			} else {
				for (int n = 0; n < tlTile.count; ++n) {
					const float wx = tlTile.posX[n] - x;
					const float wy = tlTile.posY[n] - y;
					const float wz = tlTile.posZ[n] - z;
					const float dot = wx * nx + wy * ny + wz * nz;
					if (dot < 0.0f) continue;
					const float len2 = wx*wx + wy*wy + wz*wz;
					if (len2 > tlTile.range2[n]) continue;
					const float lenInv = fast_rsqrt(len2);
					const float dist   = len2 * lenInv;
					const float falloff = (1.0f - dist * tlTile.rRange[n]);
					float k = dot * lenInv * falloff;
					if (tlTile.isSpot[n]) {
						const float cosTheta = -(tlTile.dirX[n]*wx + tlTile.dirY[n]*wy + tlTile.dirZ[n]*wz) * lenInv;
						if (cosTheta <= tlTile.cosOuter[n]) continue;
						if (cosTheta < tlTile.cosInner[n]) {
							float t = (cosTheta - tlTile.cosOuter[n]) / (tlTile.cosInner[n] - tlTile.cosOuter[n]);
							k *= t * t * (3.0f - 2.0f * t);
						}
					}
					const float intensity = k * Mat->Diffuse;
					lB += intensity * tlTile.colB[n];
					lG += intensity * tlTile.colG[n];
					lR += intensity * tlTile.colR[n];

					if (wantSpecular) {
						const float ldx = wx * lenInv;
						const float ldy = wy * lenInv;
						const float ldz = wz * lenInv;
						float hx = ldx + vx_v, hy = ldy + vy_v, hz = ldz + vz_v;
						const float hLen2 = hx*hx + hy*hy + hz*hz;
						if (hLen2 <= 0.0f) continue;
						const float hLenInv = fast_rsqrt(hLen2);
						hx *= hLenInv; hy *= hLenInv; hz *= hLenInv;
						const float NdotH = nx*hx + ny*hy + nz*hz;
						if (NdotH <= 0.0f) continue;
						const float specStrength = std::pow(NdotH, gloss) *
							Mat->Specular * falloff;
						sB += specStrength * tlTile.colB[n];
						sG += specStrength * tlTile.colG[n];
						sR += specStrength * tlTile.colR[n];
					}
				}
			}

			if (lB > 250.0f) lB = 250.0f;
			if (lG > 250.0f) lG = 250.0f;
			if (lR > 250.0f) lR = 250.0f;
			if (lB < 0.0f) lB = 0.0f;
			if (lG < 0.0f) lG = 0.0f;
			if (lR < 0.0f) lR = 0.0f;

			if (ctx.Sc->Flags & Scn_Fogged) {
				// sqrt(t) via fast_rsqrt: sqrt(t) = t * rsqrt(t).
				// Guarded against t<=0 (rsqrt undefined at 0).
				const float t = 1.0f - z * (1.0f / ctx.Sc->FZP);
				const float fogRate = t > 0.0f ? t * fast_rsqrt(t) : 0.0f;
				lB = std::min(std::max(lB * fogRate, 10.0f), 253.0f);
				lG = std::min(std::max(lG * fogRate, 10.0f), 253.0f);
				lR = std::min(std::max(lR * fogRate, 10.0f), 253.0f);
			}

			int litB = int((texB * lB) * (1.0f / 256.0f));
			int litG = int((texG * lG) * (1.0f / 256.0f));
			int litR = int((texR * lR) * (1.0f / 256.0f));
			// Specular added on top — independent of base color tint.
			if (wantSpecular) {
				float fogScale = 1.0f;
				if (ctx.Sc->Flags & Scn_Fogged) {
					const float t = 1.0f - z * (1.0f / ctx.Sc->FZP);
					fogScale = t > 0.0f ? t * fast_rsqrt(t) : 0.0f;
				}
				litB += int(sB * fogScale);
				litG += int(sG * fogScale);
				litR += int(sR * fogScale);
			}
			if (litB > 255) litB = 255;
			if (litG > 255) litG = 255;
			if (litR > 255) litR = 255;
			if (litB < 0) litB = 0;
			if (litG < 0) litG = 0;
			if (litR < 0) litR = 0;

			// Alpha-blend onto VPage. Default: `litRGB + dst/2`
			// saturated — matches forward TheOtherBarry<TRANSPARENT>
			// (source-full + dest-halved, NOT 50/50).
			// Opt-in per-material (Mat->XparBlendAlpha > 0): linear
			// interpolate `litRGB * α + dst * (1-α)`, no saturation
			// cap. Fountain orb glass uses α=0.4 for a more
			// see-through look; other transparents in the same scene
			// (e.g. fountain spires) keep the legacy formula.
			const dword existing = out[i];
			const int dB = int((existing      ) & 0xFF);
			const int dG = int((existing >>  8) & 0xFF);
			const int dR = int((existing >> 16) & 0xFF);
			int outB, outG, outR;
			if (Mat->XparBlendAlpha > 0.0f) {
				const float a = Mat->XparBlendAlpha;
				const float ia = 1.0f - a;
				outB = int(float(litB) * a + float(dB) * ia);
				outG = int(float(litG) * a + float(dG) * ia);
				outR = int(float(litR) * a + float(dR) * ia);
			} else {
				outB = litB + dB / 2;
				outG = litG + dG / 2;
				outR = litR + dR / 2;
				if (outB > 255) outB = 255;
				if (outG > 255) outG = 255;
				if (outR > 255) outR = 255;
			}
			out[i] = dword(outB) | (dword(outG) << 8) | (dword(outR) << 16) | 0xFF000000u;
		}
	}

	std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
	++renderns::tileCounter;
	renderns::condition.notify_one();
}

// FDS_DEFERRED_UNIFIED_TBR=1 routes the deferred transparent + sprite
// rendering through the unified per-strip TBR walk (task #9). When off
// (default during validation), the existing per-clump xpar peel runs
// alongside the standalone sprite TBR — the legacy behaviour.
//
// Also requires the current scene to have called TBR_Init (so the per-
// strip linked-list storage exists). Scenes without a TBR — city,
// greets — fall back to the legacy peel; the unified path with no
// SBufferHead would silently drop every transparent face, since
// InsertTransparentToTBR bails when NumTiles==0. Only fountain
// currently calls TBR_Init.
bool deferredUnifiedTbrEnabled() {
	static bool flagCached = []{
		const char *s = std::getenv("FDS_DEFERRED_UNIFIED_TBR");
		return s && s[0] == '1';
	}();
	if (!flagCached) return false;
	if (!CurScene) return false;
	if (!CurScene->SBufferHead || CurScene->NumTiles == 0) return false;
	return true;
}

// Per-strip xpar render helper, called from TBR_Render's per-strip walk
// for each clump of consecutive same-(mesh, frontFacing) transparent
// faces in the sorted item list. The strip covers rows [strip_y,
// strip_y+strip_h) (strip_h is normally TILESIZE=8, but may be smaller
// for the last strip if YRes isn't a multiple of 8).
//
// One xpar G-buffer layer is dirtied per clump (back layer for back-
// facing tris, front layer for front-facing tris). We clear only the
// strip's slice (61 KB per layer at 1920 wide), raster the clump's
// faces with clipper extents = strip rect, then composite the strip
// rows via Render_DeferredTransparentLighting_Tile<Layer>.
void RenderXparClumpInStrip(Face** faces, int count, bool front,
                             int strip_y, int strip_h)
{
	const size_t rowStart  = size_t(strip_y) * size_t(XRes);
	const size_t rowCount  = size_t(strip_h) * size_t(XRes);

	// Clear strip's slice of the relevant xpar layer.
	if (front) {
		if (g_gbufferTransparent) {
			std::memset(g_gbufferTransparent->txtr.data() + rowStart,
			            0xFF, rowCount * sizeof(uint32_t));
		}
		if (g_xparZ) std::memset(g_xparZ + rowStart, 0,
		                          rowCount * sizeof(uint16_t));
	} else {
		if (g_gbufferTransparentBack) {
			std::memset(g_gbufferTransparentBack->txtr.data() + rowStart,
			            0xFF, rowCount * sizeof(uint32_t));
		}
		if (g_xparZBack) std::memset(g_xparZBack + rowStart, 0,
		                              rowCount * sizeof(uint16_t));
	}

	// Raster the clump's faces into this layer. Clipper extents pin the
	// rasterizer to the strip; faces are routed to MekaleleTransparent
	// (front-facing → front layer) or MekaleleTransparentBack (back-
	// facing → back layer). All faces in a clump share orientation by
	// construction.
	FrustumClipper clipper;
	clipper.InitViewport(CurScene);
	clipper.SetClippingExtents(0.0f, float(strip_y),
	                            float(XRes), float(strip_y + strip_h));
	// Strip-clamp the rasterizer: a clipped tri whose max PY snapped to
	// strip_y + strip_h would compute tile_My one tile-row past the strip
	// and write pixels that the neighbouring strip's clear (concurrent)
	// then wipes — visible as 1-row dark stripes every TILE_SIZE rows.
	const int stripTileRow = strip_y >> 3;  // TILELOG=3
	const meka::RasterStripClamp savedClamp = meka::g_rasterStripClamp;
	meka::g_rasterStripClamp = { stripTileRow, stripTileRow };
	for (int i = 0; i < count; ++i) {
		Face* F = faces[i];
		if (!F) continue;
		if (front) clipper.Render(F, MekaleleTransparent,     false);
		else       clipper.Render(F, MekaleleTransparentBack, false);
	}
	meka::g_rasterStripClamp = savedClamp;

	// Composite the strip rows onto VPage via the transparent lighting
	// kernel, with a ctx variant whose `tileLights` points at the
	// per-strip light array (built by buildStripLightLists in
	// Render_DeferredLighting). The kernel reads ctx.tileLights[
	// tileIndex] for its per-pixel light loop; with tileLights ->
	// g_stripLights and tileIndex = strip_y/TILESIZE the strip gets
	// exactly the lights overlapping its 8 rows.
	const int stripIdx = strip_y >> 3;  // TILELOG=3
	DeferredLightingCtx stripCtx = g_deferredCtx;
	stripCtx.tileLights = g_stripLights;
	if (front) {
		Render_DeferredTransparentLighting_Tile<XparLayer::Front>(
			stripCtx, stripIdx,
			0, strip_y, XRes, strip_y + strip_h);
	} else {
		Render_DeferredTransparentLighting_Tile<XparLayer::Back>(
			stripCtx, stripIdx,
			0, strip_y, XRes, strip_y + strip_h);
	}
}

// Wave-2 of checkerboard: fills the odd (skipped) cells. For each odd
// pixel: if both horizontal neighbors share the same matID, average
// their already-shaded VPage values (~5 cycles per pixel — 2 loads,
// 3 lane averages, 1 store). If matID disagrees (material edge), fall
// back to full deferred shading for that pixel via the same kernel as
// wave-1. The mismatch rate on City is ~5% per the snapshot pixel
// distribution — most of the frame is large continuous surfaces.
//
// Outer-SIMD diffuse kernel: 8 pixels per row in vec registers. The
// scalar gather (matTable → Material* → Mipmap[mip] → texel + mat
// scalars) happens once per lane and stays scalar — arm64-via-simde
// has no usable vgather. Everything after — texel byte-extract, oct
// normal decode, view-space pos reconstruct, per-omni dot/range/k
// accumulate, saturate, modulate — runs 8-wide. Specular and water
// fall back to scalar tail when needed; checkerboard is handled by
// dropping odd lanes via the alive mask.
static void Render_DeferredLighting_Tile_OuterVec(const DeferredLightingCtx &ctx,
                                                   int tileIndex,
                                                   int x1, int y1, int x2, int y2)
{
	const meka::GBuffer &gb = *ctx.gb;
	dword *out = reinterpret_cast<dword *>(VPage);
	const bool   quarter      = deferredLightingQuarterEnabled();
	const bool   checker      = deferredLightingCheckerboardEnabled() && !quarter;
	const bool   specGlobalOn = Specular_Factor > 0.0f;
	const TileLights &tl = ctx.tileLights[tileIndex];
	const float  ambB_sc = float(ctx.Sc->Ambient.B);
	const float  ambG_sc = float(ctx.Sc->Ambient.G);
	const float  ambR_sc = float(ctx.Sc->Ambient.R);

	// Pre-extract per-lane data into 8-aligned scratch buffers so we
	// can do gather work scalar-per-lane and load_a back into vec.
	alignas(32) uint32_t lane_mat32[8];
	alignas(32) uint32_t lane_alive[8];
	alignas(32) float    lane_texB[8], lane_texG[8], lane_texR[8];
	alignas(32) float    lane_ambB[8], lane_ambG[8], lane_ambR[8];
	alignas(32) float    lane_diffuse[8];
	alignas(32) float    lane_specular[8];
	alignas(32) float    lane_gloss[8];
	alignas(32) uint32_t lane_wantSpec[8];
	alignas(32) uint32_t lane_isWater[8];

	for (int py = y1; py < y2; ++py) {
		// vec body: groups of 8 pixels
		int px = x1;
		// Align start to 8-pixel boundary so loads from gb arrays are
		// naturally aligned (gb.txtr/gb.normal are aligned at frame
		// allocation; ZPage16 aligned by parallel_memset).
		const int x_vec_start = (x1 + 7) & ~7;
		// Scalar lead-in for unaligned pixels at tile start.
		while (px < x_vec_start && px < x2) {
			// Tail: just defer to scalar; rare since tile boundaries
			// are 8-aligned for our 1920x1080 / 12x8 grid (160-wide tiles).
			++px;
		}

		for (; px < x2; px += 8) {
			const size_t i = size_t(py) * XRes + px;

			// Load 8 lanes of Z (u16 → s32 zero-extend → float for the
			// cmp; later use the int form too)
			__m128i z16 = _mm_loadu_si128((const __m128i*)(ZPage16 + i));
			__m256i zEncI = _mm256_cvtepu16_epi32(z16);

			// Alive mask: zEnc != 0
			__m256i mask_alive = _mm256_cmpgt_epi32(zEncI, _mm256_setzero_si256());

			// Lane-in-range mask: when this iteration straddles the
			// right tile edge (x2 - px < 8), lanes that fall in the
			// NEXT tile must be masked off so we don't write past x2.
			// For full groups (x2 - px >= 8) every lane is in range
			// and the AND is a no-op. Tile widths at non-multiple-of-8
			// resolutions (e.g., HiDPI 1512/12 = 126) would otherwise
			// leave the trailing 6 columns of each tile-column
			// unrendered — visible as vertical dark bands at every
			// tile-X boundary.
			{
				__m256i lane_idx2 = _mm256_setr_epi32(0,1,2,3,4,5,6,7);
				__m256i in_range  = _mm256_cmpgt_epi32(
					_mm256_set1_epi32(x2 - px), lane_idx2);
				mask_alive = _mm256_and_si256(mask_alive, in_range);
			}

			// Checkerboard: drop odd cells in wave-1.
			if (checker) {
				// (px+lane + py) & 1 == 0 → keep
				__m256i lane_idx = _mm256_setr_epi32(0,1,2,3,4,5,6,7);
				__m256i px_lane  = _mm256_add_epi32(_mm256_set1_epi32(px), lane_idx);
				__m256i parity   = _mm256_and_si256(
					_mm256_xor_si256(px_lane, _mm256_set1_epi32(py)),
					_mm256_set1_epi32(1));
				__m256i keep = _mm256_cmpeq_epi32(parity, _mm256_setzero_si256());
				mask_alive = _mm256_and_si256(mask_alive, keep);
			}
			// Quarter-rate: keep only lanes where both px AND py even.
			if (quarter) {
				if (py & 1) continue;  // entire row skipped in wave-1
				__m256i lane_idx = _mm256_setr_epi32(0,1,2,3,4,5,6,7);
				__m256i px_lane  = _mm256_add_epi32(_mm256_set1_epi32(px), lane_idx);
				__m256i parity_x = _mm256_and_si256(px_lane, _mm256_set1_epi32(1));
				__m256i keep = _mm256_cmpeq_epi32(parity_x, _mm256_setzero_si256());
				mask_alive = _mm256_and_si256(mask_alive, keep);
			}

			// Load mat32
			__m256i mat32v = _mm256_loadu_si256((const __m256i*)(gb.txtr.data() + i));
			__m256i matIDv = _mm256_and_si256(_mm256_srli_epi32(mat32v, 20),
			                                   _mm256_set1_epi32(0xFF));
			// Mask: matID < count
			__m256i mask_id = _mm256_cmpgt_epi32(_mm256_set1_epi32(int(ctx.matTable.count)),
			                                     matIDv);
			mask_alive = _mm256_and_si256(mask_alive, mask_id);

			// Stash for scalar lane work
			_mm256_store_si256((__m256i*)lane_mat32, mat32v);
			_mm256_store_si256((__m256i*)lane_alive, mask_alive);

			// Per-lane scalar gather: resolve Material*, gather texel,
			// fill ambient + spec/diffuse scratch.
			bool any_alive = false;
			for (int k = 0; k < 8; ++k) {
				if (!lane_alive[k]) {
					lane_texB[k] = lane_texG[k] = lane_texR[k] = 0;
					lane_ambB[k] = lane_ambG[k] = lane_ambR[k] = 0;
					lane_diffuse[k] = lane_specular[k] = 0;
					lane_gloss[k] = 32.0f;
					lane_wantSpec[k] = 0;
					lane_isWater[k] = 0;
					continue;
				}
				const uint32_t m = lane_mat32[k];
				const uint32_t matID = (m >> 20) & 0xFF;
				const uint32_t mip   = (m >> 28) & 0xF;
				const uint32_t uv    = m & 0xFFFFF;
				Material *Mat = ctx.matTable.data[matID];
				if (!Mat || !Mat->Txtr) {
					lane_alive[k] = 0;
					lane_texB[k] = lane_texG[k] = lane_texR[k] = 0;
					lane_ambB[k] = lane_ambG[k] = lane_ambR[k] = 0;
					lane_diffuse[k] = lane_specular[k] = 0;
					lane_gloss[k] = 32.0f;
					lane_wantSpec[k] = 0;
					lane_isWater[k] = 0;
					continue;
				}
				const dword *texData = (const dword*)Mat->Txtr->Mipmap[mip];
				if (!texData) {
					lane_alive[k] = 0;
					lane_texB[k] = lane_texG[k] = lane_texR[k] = 0;
					lane_ambB[k] = lane_ambG[k] = lane_ambR[k] = 0;
					lane_diffuse[k] = lane_specular[k] = 0;
					lane_gloss[k] = 32.0f;
					lane_wantSpec[k] = 0;
					lane_isWater[k] = 0;
					continue;
				}
				const dword tx = texData[uv];
				lane_texB[k] = float(tx & 0xFF);
				lane_texG[k] = float((tx >> 8) & 0xFF);
				lane_texR[k] = float((tx >> 16) & 0xFF);
				const float Lumin = Mat->Luminosity;
				const float Diff  = Mat->Diffuse;
				lane_ambB[k]    = Lumin * 255.0f + Diff * ambB_sc;
				lane_ambG[k]    = Lumin * 255.0f + Diff * ambG_sc;
				lane_ambR[k]    = Lumin * 255.0f + Diff * ambR_sc;
				lane_diffuse[k] = Diff;
				lane_specular[k]= Mat->Specular;
				lane_gloss[k]   = Mat->Glossiness > 0 ? float(Mat->Glossiness) : 32.0f;
				lane_wantSpec[k]= (Mat->Specular > 0.0f && specGlobalOn) ? 0xFFFFFFFFu : 0u;
				lane_isWater[k] = (int(matID) == ctx.waterMatID) ? 0xFFFFFFFFu : 0u;
				any_alive = true;
			}
			if (!any_alive) continue;
			// Refresh alive mask from scratch (some lanes may have been
			// killed by mip-data null check above).
			__m256i mask_alive_fresh = _mm256_load_si256((const __m256i*)lane_alive);

			// Decode 8 normals in parallel. oct_decode_u16 form:
			//   qx = sign-extend(low byte), qy = sign-extend(high byte)
			//   ox = qx * (1/127), oy = qy * (1/127)
			//   az = 1 - |ox| - |oy|
			//   if (az < 0) fold ...
			//   then normalize.
			__m128i nrm16 = _mm_loadu_si128((const __m128i*)(gb.normal.data() + i));
			// Split into low-byte and high-byte signed expansion.
			__m128i nrm_qx_8 = _mm_and_si128(nrm16, _mm_set1_epi16(0xFF));
			__m128i nrm_qy_8 = _mm_srli_epi16(nrm16, 8);
			// sign-extend 8-bit to 16
			nrm_qx_8 = _mm_slli_epi16(nrm_qx_8, 8);
			nrm_qx_8 = _mm_srai_epi16(nrm_qx_8, 8);
			nrm_qy_8 = _mm_slli_epi16(nrm_qy_8, 8);
			nrm_qy_8 = _mm_srai_epi16(nrm_qy_8, 8);
			__m256i qx32 = _mm256_cvtepi16_epi32(nrm_qx_8);
			__m256i qy32 = _mm256_cvtepi16_epi32(nrm_qy_8);
			__m256 inv127 = _mm256_set1_ps(1.0f / 127.0f);
			__m256 ox = _mm256_mul_ps(_mm256_cvtepi32_ps(qx32), inv127);
			__m256 oy = _mm256_mul_ps(_mm256_cvtepi32_ps(qy32), inv127);
			__m256 absox = _mm256_andnot_ps(_mm256_set1_ps(-0.0f), ox);
			__m256 absoy = _mm256_andnot_ps(_mm256_set1_ps(-0.0f), oy);
			__m256 az = _mm256_sub_ps(_mm256_sub_ps(_mm256_set1_ps(1.0f), absox), absoy);
			// Branchless fold: t = max(-az, 0); ox -= copysign(t, ox); oy -= copysign(t, oy)
			__m256 t = _mm256_max_ps(_mm256_sub_ps(_mm256_setzero_ps(), az),
			                          _mm256_setzero_ps());
			__m256 sign_ox = _mm256_and_ps(ox, _mm256_set1_ps(-0.0f));
			__m256 sign_oy = _mm256_and_ps(oy, _mm256_set1_ps(-0.0f));
			ox = _mm256_sub_ps(ox, _mm256_or_ps(t, sign_ox));
			oy = _mm256_sub_ps(oy, _mm256_or_ps(t, sign_oy));
			// Normalize via approx_rsqrt
			__m256 lenSq = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(ox, ox),
			                                            _mm256_mul_ps(oy, oy)),
			                              _mm256_mul_ps(az, az));
			__m256 invLenN = _mm256_rsqrt_ps(lenSq);
			__m256 nx = _mm256_mul_ps(ox, invLenN);
			__m256 ny = _mm256_mul_ps(oy, invLenN);
			__m256 nz = _mm256_mul_ps(az, invLenN);

			// Per-lane normal-map sampling. Stores the geometric N back
			// to scratch, then re-runs the wave-1 kernel's Tier-B nmap +
			// TBN reconstruction for each lane whose material has a
			// normal map. Loads the perturbed N back to nx/ny/nz so the
			// vec light loop and the scalar fallback both pick it up.
			// Without this, OuterVec rendered nmap surfaces flat, which
			// looked like horizontal banding on greets's hex floor.
			alignas(32) float nx_lane[8], ny_lane[8], nz_lane[8];
			_mm256_store_ps(nx_lane, nx);
			_mm256_store_ps(ny_lane, ny);
			_mm256_store_ps(nz_lane, nz);
			for (int k = 0; k < 8; ++k) {
				if (!lane_alive[k]) continue;
				const uint32_t m = lane_mat32[k];
				const uint32_t mid  = (m >> 20) & 0xFF;
				const uint32_t mipL = (m >> 28) & 0xF;
				const uint32_t uvL  = m & 0xFFFFF;
				Material *MatN = ctx.matTable.data[mid];
				if (!MatN || !MatN->NormalMap) continue;
				const dword *nmData = (const dword*)MatN->NormalMap->Mipmap[mipL];
				if (!nmData) continue;
				const dword nmTexel = nmData[uvL];
				const float nmX = (float((nmTexel >> 16) & 0xFF) * (1.0f/255.0f)) * 2.0f - 1.0f;
				const float nmY = (float((nmTexel >>  8) & 0xFF) * (1.0f/255.0f)) * 2.0f - 1.0f;
				const float nmZ = (float( nmTexel        & 0xFF) * (1.0f/255.0f)) * 2.0f - 1.0f;
				float lnx = nx_lane[k], lny = ny_lane[k], lnz = nz_lane[k];
				float tx = 0, ty = 0, tz = 0;
				bool tangentValid = false;
				if (!gb.tangent.empty()) {
					const meka::u16 packedT = gb.tangent[i + k];
					if (packedT != 0) {
						meka::oct_decode_u16(packedT, tx, ty, tz);
						const float tDotN = tx*lnx + ty*lny + tz*lnz;
						tx -= lnx * tDotN;
						ty -= lny * tDotN;
						tz -= lnz * tDotN;
						const float tLen2 = tx*tx + ty*ty + tz*tz;
						if (tLen2 > 1e-12f) {
							const float invTLen = 1.0f / std::sqrt(tLen2);
							tx *= invTLen; ty *= invTLen; tz *= invTLen;
							tangentValid = true;
						}
					}
				}
				if (!tangentValid) {
					float refx, refy, refz;
					if (std::fabs(lny) < 0.9f) { refx = 0; refy = 1; refz = 0; }
					else                        { refx = 1; refy = 0; refz = 0; }
					tx = refy * lnz - refz * lny;
					ty = refz * lnx - refx * lnz;
					tz = refx * lny - refy * lnx;
					const float invTLen = 1.0f / std::sqrt(tx*tx + ty*ty + tz*tz);
					tx *= invTLen; ty *= invTLen; tz *= invTLen;
				}
				const float bx = lny * tz - lnz * ty;
				const float by = lnz * tx - lnx * tz;
				const float bz = lnx * ty - lny * tx;
				float vnx = tx * nmX + bx * nmY + lnx * nmZ;
				float vny = ty * nmX + by * nmY + lny * nmZ;
				float vnz = tz * nmX + bz * nmY + lnz * nmZ;
				float invLen = 1.0f / std::sqrt(vnx*vnx + vny*vny + vnz*vnz);
				nx_lane[k] = vnx * invLen;
				ny_lane[k] = vny * invLen;
				nz_lane[k] = vnz * invLen;
			}
			nx = _mm256_load_ps(nx_lane);
			ny = _mm256_load_ps(ny_lane);
			nz = _mm256_load_ps(nz_lane);

			// Reconstruct view-space pos for 8 lanes.
			// z = (0xFF80 - zEnc) * invZScale
			__m256 zEncF = _mm256_cvtepi32_ps(zEncI);
			__m256 zv = _mm256_mul_ps(_mm256_sub_ps(_mm256_set1_ps(float(0xFF80)), zEncF),
			                           _mm256_set1_ps(ctx.invZScale));
			// x = (px+lane - CntrEX) * z * invFOVX
			__m256 px_lane_f = _mm256_add_ps(_mm256_set1_ps(float(px)),
			                                  _mm256_setr_ps(0,1,2,3,4,5,6,7));
			__m256 xv = _mm256_mul_ps(_mm256_mul_ps(_mm256_sub_ps(px_lane_f,
			                                                       _mm256_set1_ps(CntrEX)),
			                                         zv),
			                           _mm256_set1_ps(ctx.invFOVX));
			__m256 yv = _mm256_mul_ps(_mm256_mul_ps(_mm256_sub_ps(_mm256_set1_ps(CntrEY),
			                                                       _mm256_set1_ps(float(py))),
			                                         zv),
			                           _mm256_set1_ps(ctx.invFOVY));

			// Texel + ambient as vec
			__m256 texB = _mm256_load_ps(lane_texB);
			__m256 texG = _mm256_load_ps(lane_texG);
			__m256 texR = _mm256_load_ps(lane_texR);
			__m256 lB   = _mm256_load_ps(lane_ambB);
			__m256 lG   = _mm256_load_ps(lane_ambG);
			__m256 lR   = _mm256_load_ps(lane_ambR);
			__m256 vDiff = _mm256_load_ps(lane_diffuse);

			// Water lanes don't accumulate omnis.
			__m256i waterMask = _mm256_load_si256((const __m256i*)lane_isWater);
			// "alive AND not water" lanes accumulate
			__m256i omniMask  = _mm256_andnot_si256(waterMask, mask_alive_fresh);
			__m256  omniMaskF = _mm256_castsi256_ps(omniMask);

			// Early-out optimization: if every alive lane needs the
			// scalar fallback (wantSpec or isWater), the vec omni loop
			// is wasted — the fallback redoes the lighting with full
			// spec accumulation, and vfB/vfG/vfR get overridden lane-by-
			// lane in the pack loop. Skip the body entirely. For greets
			// the floor + walls are mostly spec/nmap, so this fires
			// often and recovers the regression introduced by adding
			// nmap sampling above.
			__m256i wantSpecMask = _mm256_load_si256((const __m256i*)lane_wantSpec);
			__m256i needScalar   = _mm256_or_si256(wantSpecMask, waterMask);
			__m256i needVec      = _mm256_andnot_si256(needScalar, mask_alive_fresh);
			const bool anyVecLane = !_mm256_testz_si256(needVec, needVec);

			// Per-omni accumulate. Each omni broadcast, 8 pixels in vec.
			// FDS_PROF_NO_LIGHTS gate: skip the loop entirely so the
			// bench harness can isolate non-light kernel cost.
			static const bool profNoLights = []{
				const char *s = std::getenv("FDS_PROF_NO_LIGHTS");
				return s && s[0] == '1';
			}();
			const int omniLoopN = (profNoLights || !anyVecLane) ? 0 : tl.count;
			for (int n = 0; n < omniLoopN; ++n) {
				__m256 wx = _mm256_sub_ps(_mm256_set1_ps(tl.posX[n]), xv);
				__m256 wy = _mm256_sub_ps(_mm256_set1_ps(tl.posY[n]), yv);
				__m256 wz = _mm256_sub_ps(_mm256_set1_ps(tl.posZ[n]), zv);
				__m256 dot = _mm256_fmadd_ps(wx, nx,
				              _mm256_fmadd_ps(wy, ny,
				               _mm256_mul_ps(wz, nz)));
				__m256 len2 = _mm256_fmadd_ps(wx, wx,
				               _mm256_fmadd_ps(wy, wy,
				                _mm256_mul_ps(wz, wz)));
				__m256 mask_dot   = _mm256_cmp_ps(dot,  _mm256_setzero_ps(), _CMP_GE_OQ);
				__m256 mask_range = _mm256_cmp_ps(len2, _mm256_set1_ps(tl.range2[n]), _CMP_LE_OQ);
				__m256 mask_pos   = _mm256_cmp_ps(len2, _mm256_setzero_ps(), _CMP_GT_OQ);
				__m256 omni_lane  = _mm256_and_ps(_mm256_and_ps(mask_dot, mask_range),
				                                   _mm256_and_ps(mask_pos, omniMaskF));
				// safe_len2: 1.0 when masked off
				__m256 safe_len2 = _mm256_blendv_ps(_mm256_set1_ps(1.0f), len2, omni_lane);
				__m256 lenInv = _mm256_rsqrt_ps(safe_len2);
				__m256 dist   = _mm256_mul_ps(safe_len2, lenInv);
				__m256 falloff = _mm256_sub_ps(_mm256_set1_ps(1.0f),
				                                _mm256_mul_ps(dist, _mm256_set1_ps(tl.rRange[n])));
				__m256 k = _mm256_mul_ps(_mm256_mul_ps(dot, lenInv), falloff);

				// Spot cone attenuation (only fires when isSpot[n] is set).
				// Matches the standard kernel exactly so omni pixels under
				// greets's robot spotlight get the same cone falloff.
				if (tl.isSpot[n]) {
					__m256 ldx = _mm256_set1_ps(tl.dirX[n]);
					__m256 ldy = _mm256_set1_ps(tl.dirY[n]);
					__m256 ldz = _mm256_set1_ps(tl.dirZ[n]);
					__m256 lci = _mm256_set1_ps(tl.cosInner[n]);
					__m256 lco = _mm256_set1_ps(tl.cosOuter[n]);
					__m256 dirDotW = _mm256_fmadd_ps(ldx, wx,
					                  _mm256_fmadd_ps(ldy, wy,
					                   _mm256_mul_ps(ldz, wz)));
					__m256 cosTheta = _mm256_mul_ps(_mm256_sub_ps(_mm256_setzero_ps(), dirDotW),
					                                 lenInv);
					__m256 maskInside = _mm256_cmp_ps(cosTheta, lco, _CMP_GT_OQ);
					__m256 rangeRcp = _mm256_div_ps(_mm256_set1_ps(1.0f),
					                                 _mm256_sub_ps(lci, lco));
					__m256 t = _mm256_mul_ps(_mm256_sub_ps(cosTheta, lco), rangeRcp);
					t = _mm256_min_ps(_mm256_max_ps(t, _mm256_setzero_ps()),
					                  _mm256_set1_ps(1.0f));
					__m256 smooth = _mm256_mul_ps(_mm256_mul_ps(t, t),
					                  _mm256_sub_ps(_mm256_set1_ps(3.0f),
					                                _mm256_mul_ps(_mm256_set1_ps(2.0f), t)));
					__m256 coneAtten = _mm256_blendv_ps(_mm256_setzero_ps(), smooth, maskInside);
					k = _mm256_mul_ps(k, coneAtten);
				}

				__m256 intensity = _mm256_blendv_ps(_mm256_setzero_ps(),
				                                     _mm256_mul_ps(k, vDiff),
				                                     omni_lane);
				lB = _mm256_fmadd_ps(intensity, _mm256_set1_ps(tl.colB[n]), lB);
				lG = _mm256_fmadd_ps(intensity, _mm256_set1_ps(tl.colG[n]), lG);
				lR = _mm256_fmadd_ps(intensity, _mm256_set1_ps(tl.colR[n]), lR);
			}

			// Saturate to 250
			__m256 sat = _mm256_set1_ps(250.0f);
			__m256 zero = _mm256_setzero_ps();
			lB = _mm256_max_ps(zero, _mm256_min_ps(lB, sat));
			lG = _mm256_max_ps(zero, _mm256_min_ps(lG, sat));
			lR = _mm256_max_ps(zero, _mm256_min_ps(lR, sat));

			// Fog moved to Render_DeferredFogPass (post-lighting).

			// pixel = texel * lit / 256
			__m256 inv256 = _mm256_set1_ps(1.0f / 256.0f);
			__m256 fdB = _mm256_mul_ps(_mm256_mul_ps(texB, lB), inv256);
			__m256 fdG = _mm256_mul_ps(_mm256_mul_ps(texG, lG), inv256);
			__m256 fdR = _mm256_mul_ps(_mm256_mul_ps(texR, lR), inv256);

			// Spec / water blend / pow are not handled in vec — for any
			// alive lane that needs spec or water-mat, fall back to
			// scalar. Detect:
			//   needsScalar = wantSpec_lane | isWater_lane
			alignas(32) uint32_t lane_needs_scalar[8];
			__m256i needsScalar = _mm256_or_si256(
				_mm256_load_si256((const __m256i*)lane_wantSpec),
				_mm256_load_si256((const __m256i*)lane_isWater));
			_mm256_store_si256((__m256i*)lane_needs_scalar, needsScalar);

			alignas(32) float vfB[8], vfG[8], vfR[8];
			_mm256_store_ps(vfB, fdB);
			_mm256_store_ps(vfG, fdG);
			_mm256_store_ps(vfR, fdR);

			alignas(32) int32_t lane_alive_now[8];
			_mm256_store_si256((__m256i*)lane_alive_now, mask_alive_fresh);

			// Pack lanes into pixel dwords; for spec/water lanes, redo
			// scalar shading (rare on most City pixels).
			alignas(32) float ax[8], ay[8], az_lane[8];
			_mm256_store_ps(ax, xv);
			_mm256_store_ps(ay, yv);
			_mm256_store_ps(az_lane, zv);
			alignas(32) float anx[8], any_l[8], anz[8];
			_mm256_store_ps(anx, nx);
			_mm256_store_ps(any_l, ny);
			_mm256_store_ps(anz, nz);

			for (int k = 0; k < 8; ++k) {
				if (!lane_alive_now[k]) continue;
				int outB, outG, outR;
				if (lane_needs_scalar[k]) {
					// Scalar fallback: redo lighting for this pixel
					// including spec / water blend. Reuses tl + cached
					// texel; ambient already in lane_ambB/G/R.
					float lBs = lane_ambB[k];
					float lGs = lane_ambG[k];
					float lRs = lane_ambR[k];
					float sBs = 0, sGs = 0, sRs = 0;
					float xs = ax[k], ys = ay[k], zs = az_lane[k];
					float nxs = anx[k], nys = any_l[k], nzs = anz[k];
					float vxd = 0, vyd = 0, vzd = 0;
					const bool wantSpec = lane_wantSpec[k] != 0;
					if (wantSpec) {
						float vlen2 = xs*xs + ys*ys + zs*zs;
						float vlenInv = fast_rsqrt(vlen2);
						vxd = -xs * vlenInv;
						vyd = -ys * vlenInv;
						vzd = -zs * vlenInv;
					}
					const bool isWater = lane_isWater[k] != 0;
					if (!isWater) {
						const float matDiff = lane_diffuse[k];
						const float matSpec = lane_specular[k];
						const float gloss   = lane_gloss[k];
						for (int n = 0; n < tl.count; ++n) {
							float wxs = tl.posX[n] - xs;
							float wys = tl.posY[n] - ys;
							float wzs = tl.posZ[n] - zs;
							float dots = wxs*nxs + wys*nys + wzs*nzs;
							if (dots < 0.0f) continue;
							float len2s = wxs*wxs + wys*wys + wzs*wzs;
							if (len2s > tl.range2[n] || len2s == 0.0f) continue;
							float lenInvS = fast_rsqrt(len2s);
							float distS   = len2s * lenInvS;
							float ks = dots * lenInvS * (1.0f - distS * tl.rRange[n]);
							// Spot cone (matches std vec body + std scalar fix).
							if (tl.isSpot[n]) {
								float cosTheta = -(tl.dirX[n]*wxs + tl.dirY[n]*wys + tl.dirZ[n]*wzs) * lenInvS;
								if (cosTheta <= tl.cosOuter[n]) continue;
								if (cosTheta < tl.cosInner[n]) {
									float ct = (cosTheta - tl.cosOuter[n]) / (tl.cosInner[n] - tl.cosOuter[n]);
									ks *= ct * ct * (3.0f - 2.0f * ct);
								}
							}
							float ints = ks * matDiff;
							lBs += ints * tl.colB[n];
							lGs += ints * tl.colG[n];
							lRs += ints * tl.colR[n];
							if (wantSpec) {
								float ldx = wxs * lenInvS, ldy = wys * lenInvS, ldz = wzs * lenInvS;
								float hx = ldx + vxd, hy = ldy + vyd, hz = ldz + vzd;
								float hLen2 = hx*hx + hy*hy + hz*hz;
								if (hLen2 <= 0.0f) continue;
								float hInv = fast_rsqrt(hLen2);
								hx *= hInv; hy *= hInv; hz *= hInv;
								float NdotH = nxs*hx + nys*hy + nzs*hz;
								if (NdotH <= 0.0f) continue;
								float spec = std::pow(NdotH, gloss);
								float ss = spec * matSpec * (1.0f - distS * tl.rRange[n]);
								sBs += ss * tl.colB[n];
								sGs += ss * tl.colG[n];
								sRs += ss * tl.colR[n];
							}
						}
					}
					if (lBs > 250.0f) lBs = 250.0f; if (lBs < 0) lBs = 0;
					if (lGs > 250.0f) lGs = 250.0f; if (lGs < 0) lGs = 0;
					if (lRs > 250.0f) lRs = 250.0f; if (lRs < 0) lRs = 0;
					float fdBs = lane_texB[k] * lBs * (1.0f / 256.0f);
					float fdGs = lane_texG[k] * lGs * (1.0f / 256.0f);
					float fdRs = lane_texR[k] * lRs * (1.0f / 256.0f);
					outB = int(fdBs) + int(sBs);
					outG = int(fdGs) + int(sGs);
					outR = int(fdRs) + int(sRs);
					if (isWater) {
						const dword existing = out[i + k];
						outB += int(existing & 0xFF) >> 1;
						outG += int((existing >> 8) & 0xFF) >> 1;
						outR += int((existing >> 16) & 0xFF) >> 1;
					}
				} else {
					outB = int(vfB[k]);
					outG = int(vfG[k]);
					outR = int(vfR[k]);
				}
				if (outB > 255) outB = 255; if (outB < 0) outB = 0;
				if (outG > 255) outG = 255; if (outG < 0) outG = 0;
				if (outR > 255) outR = 255; if (outR < 0) outR = 0;
				out[i + k] = dword(outB) | (dword(outG) << 8) | (dword(outR) << 16) | 0xFF000000u;
			}
		}

		// Tail: scalar for remaining 1-7 pixels
		for (; px < x2; ++px) {
			// Lazy: just dispatch back to the per-tile inner-loop body.
			// For simplicity we use a no-op since tile widths divide
			// evenly by 8 in our typical 1920×1080 / 12×8 grid.
			// (160-wide tiles → no tail.) If tile widths ever stop
			// being multiples of 8 this needs a real scalar fallback.
			(void)px;
			break;
		}
	}

	std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
	++renderns::tileCounter;
	renderns::condition.notify_one();
}

// Race-free because wave-2 reads only what wave-1 wrote; tile-job
// dispatch waits on tileCounter between waves. Tile-edge odd pixels
// (px==x1 or px==x2-1) read the next/prev tile's shaded result, which
// is safe because that neighbor tile finished its wave-1 already.
static void Render_DeferredLighting_TileFill(const DeferredLightingCtx &ctx,
                                              int tileIndex,
                                              int x1, int y1, int x2, int y2)
{
	const meka::GBuffer &gb = *ctx.gb;
	dword *out = reinterpret_cast<dword *>(VPage);
	const bool specGlobalOn = Specular_Factor > 0.0f;
	const bool quarter      = deferredLightingQuarterEnabled();

	for (int py = y1; py < y2; ++py) {
		for (int px = x1; px < x2; ++px) {
			if (quarter) {
				if (((px | py) & 1) == 0) continue;  // wave-1 shaded
			} else {
				if (((px ^ py) & 1) == 0) continue;
			}

			const size_t i = size_t(py) * XRes + px;
			const word zEnc = ZPage16[i];
			if (zEnc == 0) continue;

			const uint32_t mat32  = gb.txtr[i];
			const uint32_t matIDc = (mat32 >> 20) & 0xFF;

			bool matched = false;
			if (quarter) {
				// One of three patterns by parity:
				//   (odd_x, even_y) → average left + right shaded
				//   (even_x, odd_y) → average top + bottom shaded
				//   (odd_x,  odd_y) → average 4 corner shaded
				const bool odd_x = px & 1;
				const bool odd_y = py & 1;
				if (odd_x && !odd_y) {
					// horizontal: -1 and +1
					const bool haveL = (px > 0);
					const bool haveR = (px < XRes - 1);
					if (haveL && haveR) {
						const uint32_t mIDl = (gb.txtr[i - 1] >> 20) & 0xFF;
						const uint32_t mIDr = (gb.txtr[i + 1] >> 20) & 0xFF;
						if (mIDl == matIDc && mIDr == matIDc) {
							const dword avg = ((out[i - 1] & 0xFEFEFEFEu) >> 1) +
							                   ((out[i + 1] & 0xFEFEFEFEu) >> 1);
							out[i] = avg | 0xFF000000u;
							matched = true;
						}
					}
				} else if (!odd_x && odd_y) {
					// vertical: -XRes and +XRes
					const bool haveT = (py > 0);
					const bool haveB = (py < YRes - 1);
					if (haveT && haveB) {
						const uint32_t mIDt = (gb.txtr[i - XRes] >> 20) & 0xFF;
						const uint32_t mIDb = (gb.txtr[i + XRes] >> 20) & 0xFF;
						if (mIDt == matIDc && mIDb == matIDc) {
							const dword avg = ((out[i - XRes] & 0xFEFEFEFEu) >> 1) +
							                   ((out[i + XRes] & 0xFEFEFEFEu) >> 1);
							out[i] = avg | 0xFF000000u;
							matched = true;
						}
					}
				} else {
					// diagonal: 4 corners
					const bool haveTL = (px > 0)        && (py > 0);
					const bool haveTR = (px < XRes - 1) && (py > 0);
					const bool haveBL = (px > 0)        && (py < YRes - 1);
					const bool haveBR = (px < XRes - 1) && (py < YRes - 1);
					if (haveTL && haveTR && haveBL && haveBR) {
						const uint32_t mTL = (gb.txtr[i - XRes - 1] >> 20) & 0xFF;
						const uint32_t mTR = (gb.txtr[i - XRes + 1] >> 20) & 0xFF;
						const uint32_t mBL = (gb.txtr[i + XRes - 1] >> 20) & 0xFF;
						const uint32_t mBR = (gb.txtr[i + XRes + 1] >> 20) & 0xFF;
						if (mTL == matIDc && mTR == matIDc && mBL == matIDc && mBR == matIDc) {
							// 4-way avg via mask-and-shift-by-2: per-channel
							// (a + b + c + d) / 4 with 2-bit precision loss
							// per channel — well below the visible threshold.
							const dword pTL = out[i - XRes - 1];
							const dword pTR = out[i - XRes + 1];
							const dword pBL = out[i + XRes - 1];
							const dword pBR = out[i + XRes + 1];
							const dword avg =
								((pTL & 0xFCFCFCFCu) >> 2) +
								((pTR & 0xFCFCFCFCu) >> 2) +
								((pBL & 0xFCFCFCFCu) >> 2) +
								((pBR & 0xFCFCFCFCu) >> 2);
							out[i] = avg | 0xFF000000u;
							matched = true;
						}
					}
				}
			} else {
				// Checkerboard L/R interp (existing behaviour).
				const bool haveLeft  = (px > 0);
				const bool haveRight = (px < XRes - 1);
				if (!haveLeft && !haveRight) continue;
				if (haveLeft && haveRight) {
					const uint32_t matIDl = (gb.txtr[i - 1] >> 20) & 0xFF;
					const uint32_t matIDr = (gb.txtr[i + 1] >> 20) & 0xFF;
					if (matIDl == matIDc && matIDr == matIDc) {
						const dword pl = out[i - 1];
						const dword pr = out[i + 1];
						const dword avg = ((pl & 0xFEFEFEFEu) >> 1) +
						                   ((pr & 0xFEFEFEFEu) >> 1);
						out[i] = avg | 0xFF000000u;
						matched = true;
					}
				}
			}
			if (matched) continue;

			// Fallback path — replays the wave-1 kernel for this pixel.
			// We don't expect this branch to be hot (most frame area is
			// continuous surface), so the simpler scalar-only fallback
			// keeps the code small even if we're in vec mode otherwise.
			const uint32_t miplevel    = (mat32 >> 28) & 0xF;
			const uint32_t matID       = matIDc;
			const uint32_t swizzledUV  = mat32 & 0xFFFFF;
			if (matID >= ctx.matTable.count) continue;
			Material *Mat = ctx.matTable.data[matID];
			if (!Mat || !Mat->Txtr) continue;
			const dword *texData = (const dword *)Mat->Txtr->Mipmap[miplevel];
			if (!texData) continue;
			const dword texel = texData[swizzledUV];
			const float texB = float(texel & 0xFF);
			const float texG = float((texel >> 8) & 0xFF);
			const float texR = float((texel >> 16) & 0xFF);

			float nx, ny, nz;
			meka::oct_decode_u16(gb.normal[i], nx, ny, nz);

			// Normal map sampling — mirrors the wave-1 kernel exactly so
			// nmap materials that fall through the surface-similar test
			// produce the same result as if they'd been shaded in wave-1.
			// Without this the fallback's geometric N differed from
			// surrounding interpolated pixels and broke quarter mode
			// silently. Tier B per-vertex tangent + Mikkelsen fallback.
			if (Mat->NormalMap) {
				const dword *nmData = (const dword *)Mat->NormalMap->Mipmap[miplevel];
				if (nmData) {
					const dword nmTexel = nmData[swizzledUV];
					const float nmX = (float((nmTexel >> 16) & 0xFF) * (1.0f/255.0f)) * 2.0f - 1.0f;
					const float nmY = (float((nmTexel >>  8) & 0xFF) * (1.0f/255.0f)) * 2.0f - 1.0f;
					const float nmZ = (float( nmTexel        & 0xFF) * (1.0f/255.0f)) * 2.0f - 1.0f;
					float tx = 0, ty = 0, tz = 0;
					bool tangentValid = false;
					if (!gb.tangent.empty()) {
						const meka::u16 packedT = gb.tangent[i];
						if (packedT != 0) {
							meka::oct_decode_u16(packedT, tx, ty, tz);
							const float tDotN = tx*nx + ty*ny + tz*nz;
							tx -= nx * tDotN; ty -= ny * tDotN; tz -= nz * tDotN;
							const float tLen2 = tx*tx + ty*ty + tz*tz;
							if (tLen2 > 1e-12f) {
								const float invTLen = 1.0f / std::sqrt(tLen2);
								tx *= invTLen; ty *= invTLen; tz *= invTLen;
								tangentValid = true;
							}
						}
					}
					if (!tangentValid) {
						float refx, refy, refz;
						if (std::fabs(ny) < 0.9f) { refx = 0; refy = 1; refz = 0; }
						else                       { refx = 1; refy = 0; refz = 0; }
						tx = refy * nz - refz * ny;
						ty = refz * nx - refx * nz;
						tz = refx * ny - refy * nx;
						const float invTLen = 1.0f / std::sqrt(tx*tx + ty*ty + tz*tz);
						tx *= invTLen; ty *= invTLen; tz *= invTLen;
					}
					const float bx = ny * tz - nz * ty;
					const float by = nz * tx - nx * tz;
					const float bz = nx * ty - ny * tx;
					float vnx = tx * nmX + bx * nmY + nx * nmZ;
					float vny = ty * nmX + by * nmY + ny * nmZ;
					float vnz = tz * nmX + bz * nmY + nz * nmZ;
					float invLen = 1.0f / std::sqrt(vnx*vnx + vny*vny + vnz*vnz);
					nx = vnx * invLen;
					ny = vny * invLen;
					nz = vnz * invLen;
				}
			}

			const float z = float(0xFF80 - zEnc) * ctx.invZScale;
			const float x = (float(px) - CntrEX) * z * ctx.invFOVX;
			const float y = (CntrEY - float(py)) * z * ctx.invFOVY;

			float lB, lG, lR;
			if (Mat->Txtr) {
				lB = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.B;
				lG = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.G;
				lR = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.R;
			} else {
				lB = Mat->Luminosity * Mat->BaseCol.B + Mat->Diffuse * ctx.Sc->Ambient.B;
				lG = Mat->Luminosity * Mat->BaseCol.G + Mat->Diffuse * ctx.Sc->Ambient.G;
				lR = Mat->Luminosity * Mat->BaseCol.R + Mat->Diffuse * ctx.Sc->Ambient.R;
			}

			const bool isWater = (int(matID) == ctx.waterMatID);
			const TileLights &tl = ctx.tileLights[tileIndex];

			float sB = 0, sG = 0, sR = 0;
			float vx = 0, vy = 0, vz = 0;
			const bool wantSpecular = specGlobalOn && (Mat->Specular > 0.0f);
			const float gloss = Mat->Glossiness > 0 ? float(Mat->Glossiness) : 32.0f;
			if (wantSpecular) {
				const float vlen2 = x*x + y*y + z*z;
				const float vlenInv = fast_rsqrt(vlen2);
				vx = -x * vlenInv;
				vy = -y * vlenInv;
				vz = -z * vlenInv;
			}

			for (int n = 0; !isWater && n < tl.count; ++n) {
				const float wx = tl.posX[n] - x;
				const float wy = tl.posY[n] - y;
				const float wz = tl.posZ[n] - z;
				const float dot = wx*nx + wy*ny + wz*nz;
				if (dot < 0.0f) continue;
				const float len2 = wx*wx + wy*wy + wz*wz;
				if (len2 > tl.range2[n]) continue;
				const float lenInv = fast_rsqrt(len2);
				const float dist   = len2 * lenInv;
				float k = dot * lenInv * (1.0f - dist * tl.rRange[n]);
				// Spot cone gate. coneAtten in [0,1] modulates BOTH diffuse
				// and specular contributions so they stay coherent — the
				// spot just darkens the light past the inner cone, not
				// just its diffuse channel.
				float coneAtten = 1.0f;
				if (tl.isSpot[n]) {
					const float cosTheta = -(tl.dirX[n]*wx + tl.dirY[n]*wy + tl.dirZ[n]*wz) * lenInv;
					if (cosTheta <= tl.cosOuter[n]) continue;
					if (cosTheta < tl.cosInner[n]) {
						float t = (cosTheta - tl.cosOuter[n]) / (tl.cosInner[n] - tl.cosOuter[n]);
						coneAtten = t * t * (3.0f - 2.0f * t);
						k *= coneAtten;
					}
				}
				const float intensity = k * Mat->Diffuse;
				lB += intensity * tl.colB[n];
				lG += intensity * tl.colG[n];
				lR += intensity * tl.colR[n];

				if (wantSpecular) {
					const float ldx = wx * lenInv;
					const float ldy = wy * lenInv;
					const float ldz = wz * lenInv;
					float hx = ldx + vx, hy = ldy + vy, hz = ldz + vz;
					const float hLen2 = hx*hx + hy*hy + hz*hz;
					if (hLen2 > 0.0f) {
						const float hLenInv = fast_rsqrt(hLen2);
						hx *= hLenInv; hy *= hLenInv; hz *= hLenInv;
						const float NdotH = nx*hx + ny*hy + nz*hz;
						if (NdotH > 0.0f) {
							const float spec = std::pow(NdotH, gloss);
							const float specStrength = spec * Mat->Specular * (1.0f - dist * tl.rRange[n]) * coneAtten;
							sB += specStrength * tl.colB[n];
							sG += specStrength * tl.colG[n];
							sR += specStrength * tl.colR[n];
						}
					}
				}
			}

			if (lB > 250.0f) lB = 250.0f;
			if (lG > 250.0f) lG = 250.0f;
			if (lR > 250.0f) lR = 250.0f;
			if (lB < 0.0f) lB = 0.0f;
			if (lG < 0.0f) lG = 0.0f;
			if (lR < 0.0f) lR = 0.0f;

			// Fog moved to Render_DeferredFogPass (post-lighting).

			float fdB = (texB * lB) * (1.0f / 256.0f);
			float fdG = (texG * lG) * (1.0f / 256.0f);
			float fdR = (texR * lR) * (1.0f / 256.0f);
			int outB = int(fdB) + int(sB);
			int outG = int(fdG) + int(sG);
			int outR = int(fdR) + int(sR);
			if (isWater) {
				const dword existing = out[i];
				outB += int(existing & 0xFF) >> 1;
				outG += int((existing >> 8) & 0xFF) >> 1;
				outR += int((existing >> 16) & 0xFF) >> 1;
			}
			if (outB > 255) outB = 255;
			if (outG > 255) outG = 255;
			if (outR > 255) outR = 255;
			if (outB < 0) outB = 0;
			if (outG < 0) outG = 0;
			if (outR < 0) outR = 0;
			out[i] = dword(outB) | (dword(outG) << 8) | (dword(outR) << 16) | 0xFF000000u;
		}
	}

	std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
	++renderns::tileCounter;
	renderns::condition.notify_one();
}

// Per-frame setup + dispatch tile jobs across the ThreadPool. Same 6×4
// split the rasterizer uses; tiles are independent (each writes a
// disjoint slice of VPage). Reuses the renderns::tileCounter +
// condition variable that Render() already uses for the rasterizer
// pass — fine because we wait synchronously between Render's tile
// dispatch and our own.
static void Render_DeferredLighting() {
	if (!g_gbuffer || !ZPage16 || !VPage) return;
	const meka::GBuffer &gb = *g_gbuffer;
	const size_t numPixels = size_t(XRes) * size_t(YRes);
	if (gb.normal.size() < numPixels || gb.txtr.size() < numPixels) return;

	Scene *Sc = CurScene;
	if (!Sc) return;

	MatTable matTable = Scene_GetMatTable(Sc);
	if (!matTable.data || matTable.count == 0) return;

	// FDS_DEFERRED_GLOSS_STATS=1: dump distinct Mat->Glossiness values
	// (and material count per value) for materials with Specular > 0 in
	// the current scene. Run once per scene change; lets us confirm the
	// squaring-dispatch switch in the lighting kernel covers everything
	// in use, and warn if an FLD ships a gloss value we don't specialize.
	if (std::getenv("FDS_DEFERRED_GLOSS_STATS")) {
		static Scene* lastScene = nullptr;
		if (Sc != lastScene) {
			lastScene = Sc;
			std::map<unsigned short, int> glossHisto;
			int specMats = 0;
			for (size_t i = 0; i < matTable.count; ++i) {
				Material *M = matTable.data[i];
				if (!M || M->Specular <= 0.0f) continue;
				++specMats;
				++glossHisto[M->Glossiness];
			}
			std::fprintf(stderr, "[GLOSS-STATS] scene=%p specMats=%d distinct={",
				(void*)Sc, specMats);
			bool first = true;
			for (const auto &kv : glossHisto) {
				std::fprintf(stderr, "%s%u:%d", first ? "" : ",", kv.first, kv.second);
				first = false;
			}
			std::fprintf(stderr, "}\n");
		}
	}

	// Build view-space omni list once per frame.
	static ViewLightsSoA lights;  // function-static; lifetime spans dispatch + wait
	// Optional Range clamp — if FDS_DEFERRED_MAX_RANGE is set, any
	// per-omni Range above that ceiling is clamped *for culling
	// purposes only* (the falloff math still uses the real Range, so
	// pixel brightness is barely affected — at the clamp boundary
	// contribution is `1 - clamp/Range_real` ≈ 1, dropping to 0 at the
	// clamp edge). Used to estimate the perf upside of dialing in
	// effective ranges scene-wide.
	float maxRange = 0.0f;
	if (const char *s = std::getenv("FDS_DEFERRED_MAX_RANGE")) {
		maxRange = float(std::atof(s));
	}

	std::memset(&lights, 0, sizeof(lights));
	int numLights = 0;
	for (Omni *O = Sc->OmniHead; O && numLights < DEFERRED_MAX_LIGHTS; O = O->Next) {
		if (!(O->Flags & Omni_Active)) continue;
		Vector u, w;
		Vector_Sub(&O->IPos, &View->ISource, &u);
		MatrixXVector(View->Mat, &u, &w);
		float Range = O->IRange;
		if (maxRange > 0.0f && Range > maxRange) Range = maxRange;
		lights.posX[numLights]   = w.x;
		lights.posY[numLights]   = w.y;
		lights.posZ[numLights]   = w.z;
		lights.colB[numLights]   = O->L.B * O->ISize;
		lights.colG[numLights]   = O->L.G * O->ISize;
		lights.colR[numLights]   = O->L.R * O->ISize;
		lights.range2[numLights] = Range * Range;
		// Note: rRange uses the *real* range so falloff math is
		// preserved. Only the cull radius shrinks.
		const float realRange = O->IRange;
		lights.rRange[numLights] = (realRange > 0.0f) ? 1.0f / realRange : 0.0f;
		// Spot light: rotate the cone axis (world space) into view space
		// using the same View->Mat that placed posX/Y/Z. Pure rotation,
		// no translation — IDir is a direction not a point.
		if (O->Type == Light_SpotLight) {
			Vector dirView;
			MatrixXVector(View->Mat, (Vector*)&O->IDir, &dirView);
			Vector_Norm(&dirView);
			lights.dirX[numLights] = dirView.x;
			lights.dirY[numLights] = dirView.y;
			lights.dirZ[numLights] = dirView.z;
			lights.cosInner[numLights] = O->HotSpot;
			lights.cosOuter[numLights] = O->FallOff;
			lights.isSpot[numLights] = 1u;
		} else {
			lights.dirX[numLights] = 0.0f;
			lights.dirY[numLights] = 0.0f;
			lights.dirZ[numLights] = 0.0f;
			lights.cosInner[numLights] = -2.0f;
			lights.cosOuter[numLights] = -2.0f;
			lights.isSpot[numLights] = 0u;
		}
		// Map this light to its shadow map (or -1). Only set when
		// shadows are enabled — otherwise the kernel must skip the
		// shadow test (and the shadow maps are stale-empty anyway).
		int32_t smIdx = -1;
		static const bool sShadowsOn = fdsEnvBool("FDS_SHADOWS", FDS_SHADOWS_DEFAULT_ON);
		if (sShadowsOn && (O->Flags & Omni_CastsShadow)) {
			for (size_t i = 0; i < g_shadowMaps.size(); ++i) {
				if (g_shadowMaps[i].omni == O) { smIdx = int32_t(i); break; }
			}
		}
		lights.shadowMapIdx[numLights] = smIdx;
		++numLights;
	}

	constexpr int numTilesX = DEFERRED_NUM_TILES_X;
	constexpr int numTilesY = DEFERRED_NUM_TILES_Y;
	// Round tileSizeX up to the next multiple of 8 so the OuterVec
	// kernel's 8-wide vec loop sees aligned tiles for all-but-the-last
	// tile column. The last tile width can still be unaligned at non-
	// multiple-of-8 XRes (rare on real displays — most are 8-aligned)
	// — handled by the lane-in-range mask inside OuterVec.
	// tileSizeY doesn't need this because the kernels iterate rows one
	// at a time, not in 8-tall groups.
	const int rawTileX  = (XRes + (numTilesX - 1)) / numTilesX;
	const int tileSizeX = (rawTileX + 7) & ~7;
	const int tileSizeY = (YRes + (numTilesY - 1)) / numTilesY;

	// Per-tile light culling: project each omni's bounding sphere
	// into screen space, find which tiles it overlaps, populate
	// indices[] per tile.
	static TileLights tileLights[DEFERRED_NUM_TILES];
	const float invZScale = 1.0f / float(g_zscale);
	computeTileDepthBounds(tileLights, numTilesX, numTilesY,
	                       tileSizeX, tileSizeY, XRes, YRes,
	                       invZScale);
	buildTileLightLists(tileLights, numTilesX, numTilesY,
	                    tileSizeX, tileSizeY, XRes, YRes,
	                    lights, numLights);

	// Per-strip light lists for the unified-TBR transparent path's
	// RenderXparClumpInStrip. 1D Y-strips of TILESIZE rows; built only
	// when the unified path is active to avoid the per-frame cost on
	// the legacy path. (Strip count = ceil(YRes / TILESIZE), capped at
	// DEFERRED_MAX_STRIPS=512.)
	if (deferredUnifiedTbrEnabled()) {
		constexpr int STRIP_H = 1 << 3;  // TILESIZE from FILLERS.CPP
		const int numStrips = (YRes + STRIP_H - 1) >> 3;
		buildStripLightLists(numStrips, STRIP_H, YRes, lights, numLights);
	}
	if (std::getenv("FDS_DEFERRED_TILE_STATS")) {
		int total = 0, tmin = INT_MAX, tmax = 0;
		for (int t = 0; t < DEFERRED_NUM_TILES; ++t) {
			total += tileLights[t].count;
			tmin = std::min(tmin, tileLights[t].count);
			tmax = std::max(tmax, tileLights[t].count);
		}
		fprintf(stderr, "[TILE-LIGHTS] numLights=%d tiles avg=%.1f min=%d max=%d\n",
			numLights, total / float(DEFERRED_NUM_TILES), tmin, tmax);
	}

	// File-scope static: also read by the transparent-lighting kernel
	// (Render_DeferredTransparentLighting_Tile) — same per-frame setup,
	// no point rebuilding it. The kernels never run concurrently with
	// each other, so the single instance is safe.
	extern DeferredLightingCtx g_deferredCtx;
	DeferredLightingCtx &ctx = g_deferredCtx;
	ctx.gb         = &gb;
	ctx.matTable   = matTable;
	ctx.lights     = &lights;
	ctx.numLights  = numLights;
	ctx.tileLights = tileLights;
	ctx.invFOVX    = 1.0f / FOVX;
	ctx.invFOVY    = 1.0f / FOVY;
	ctx.invZScale  = 1.0f / float(g_zscale);
	ctx.Sc         = Sc;
	ctx.waterMatID = g_deferredWaterMatID;

	// Wave 1: shade even cells (full deferred kernel). When checkerboard
	// is off, this is the entire pass and odd-cell skip is a no-op.
	const bool useOuterVec = deferredLightingOuterVecEnabled();
	renderns::tileCounter = 0;
	for (int j = 0; j < numTilesY; ++j) {
		const int y1 = tileSizeY * j;
		const int y2 = std::min(y1 + tileSizeY, YRes);
		for (int i = 0; i < numTilesX; ++i) {
			const int x1 = tileSizeX * i;
			const int x2 = std::min(x1 + tileSizeX, XRes);
			const int tileIndex = j * numTilesX + i;
			if (useOuterVec) {
				ThreadPool::instance().enqueue([tileIndex, x1, y1, x2, y2]() {
					Render_DeferredLighting_Tile_OuterVec(ctx, tileIndex, x1, y1, x2, y2);
				});
			} else {
				ThreadPool::instance().enqueue([tileIndex, x1, y1, x2, y2]() {
					Render_DeferredLighting_Tile(ctx, tileIndex, x1, y1, x2, y2);
				});
			}
		}
	}
	{
		std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
		renderns::condition.wait(lock, []{
			return renderns::tileCounter == numTilesX * numTilesY;
		});
	}

	// Wave 2: fill odd cells via 2-tap interpolation (with full-shade
	// fallback at material edges). Skip entirely when checkerboard is
	// off — wave 1 already covered everything.
	if (deferredLightingCheckerboardEnabled() || deferredLightingQuarterEnabled()) {
		renderns::tileCounter = 0;
		for (int j = 0; j < numTilesY; ++j) {
			const int y1 = tileSizeY * j;
			const int y2 = std::min(y1 + tileSizeY, YRes);
			for (int i = 0; i < numTilesX; ++i) {
				const int x1 = tileSizeX * i;
				const int x2 = std::min(x1 + tileSizeX, XRes);
				const int tileIndex = j * numTilesX + i;
				ThreadPool::instance().enqueue([tileIndex, x1, y1, x2, y2]() {
					Render_DeferredLighting_TileFill(ctx, tileIndex, x1, y1, x2, y2);
				});
			}
		}
		std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
		renderns::condition.wait(lock, []{
			return renderns::tileCounter == numTilesX * numTilesY;
		});
	}
}

// Full-screen distance fog over opaque pixels. Runs after
// Render_DeferredLighting writes finished colors to VPage; before the
// transparent peel composites. 1998 used `sqrt(1 - z/FZP)` per VERTEX;
// we keep the sqrt curve (the linear curve makes mid-range too thick)
// but compute it per-pixel via approx rsqrt: `sqrt(t) = t * rsqrt(t)`.
// On arm64 this maps to vrsqrteq_f32 (4-5 cycles) and on x86 to
// _mm256_rsqrt_ps. With the simde arm64 m256 rsqrt patch, both lanes
// hit native NEON / AVX2 directly.
//
// Skipped pixels:
//   mat32 == 0xFFFFFFFF — forward filler wrote here (reflective windows,
//     additive fountain vortex, sky-cube). Those have their own
//     per-vertex fog from TheOtherBarry and would double-fog otherwise.
//   zEnc == 0 — no opaque pixel (sky-cube background, or empty Z).
//     Sky already represents infinite distance; don't fog it.
static void Render_DeferredFogPass_Tile(int x1, int y1, int x2, int y2,
                                         float invFZP)
{
	dword *out = reinterpret_cast<dword*>(VPage);
	const uint32_t *mat = g_gbuffer->txtr.data();
	const uint16_t *zEnc = ZPage16;
	const float invZScale = 1.0f / float(g_zscale);
	const Vec8f vInvZScale(invZScale);
	const Vec8f vInvFZP(invFZP);
	const Vec8f vOne(1.0f);
	const Vec8f vZero(0.0f);
	const Vec8i vSentinel(int(0xFFFFFFFF));
	const Vec8i vZEncZero(0);
	const Vec8f vZBase(float(0xFF80));
	const Vec8i vFFu(0xFF);
	const Vec8i vTen(10);
	const Vec8i vAlpha(int(0xFF000000));

	for (int py = y1; py < y2; ++py) {
		int px = x1;
		const size_t row = size_t(py) * XRes;
		// 8-pixel SIMD body.
		for (; px + 8 <= x2; px += 8) {
			const size_t i = row + px;

			Vec8i v8;
			v8.load(out + i);
			Vec8i m8;
			m8.load(mat + i);
			// 8x u16 → 8x i32 (no sign-extension needed: zEnc is unsigned).
			alignas(16) uint16_t z16buf[8];
			std::memcpy(z16buf, zEnc + i, 16);
			alignas(32) int32_t z32buf[8];
			for (int k = 0; k < 8; ++k) z32buf[k] = int32_t(z16buf[k]);
			Vec8i z8;
			z8.load_a(z32buf);

			// Pixel-valid mask: mat != sentinel AND zEnc != 0.
			Vec8ib maskValid = (m8 != vSentinel) & (z8 != vZEncZero);

			// Decode z, fog rate, sqrt-via-rsqrt.
			Vec8f zf  = (vZBase - to_float(z8)) * vInvZScale;
			Vec8f t   = max(vZero, vOne - zf * vInvFZP);
			Vec8f rs  = approx_rsqrt(t);
			Vec8f fog = t * rs;          // sqrt(t)
			// At t==0, rsqrt yields garbage; mask the result.
			Vec8fb tPositive = t > vZero;
			fog = select(tPositive, fog, vZero);
			fog = min(fog, vOne);

			// Channel split, fog-multiply, floor-at-10, re-pack.
			Vec8i bB = v8        & vFFu;
			Vec8i bG = (v8 >>  8) & vFFu;
			Vec8i bR = (v8 >> 16) & vFFu;
			Vec8i nB = max(vTen, truncatei(to_float(bB) * fog));
			Vec8i nG = max(vTen, truncatei(to_float(bG) * fog));
			Vec8i nR = max(vTen, truncatei(to_float(bR) * fog));
			Vec8i packed = nB | (nG << 8) | (nR << 16) | vAlpha;
			Vec8i result = select(Vec8ib(maskValid), packed, v8);
			result.store(out + i);
		}
		// Scalar tail (≤7 pixels).
		for (; px < x2; ++px) {
			const size_t i = row + px;
			if (mat[i] == 0xFFFFFFFFu) continue;
			const word z16 = zEnc[i];
			if (z16 == 0) continue;
			const float z = float(0xFF80 - z16) * invZScale;
			const float t = 1.0f - z * invFZP;
			const float fog = t > 0.0f ? t * fast_rsqrt(t) : 0.0f;
			const dword v = out[i];
			int B = std::max(10, int(((v      ) & 0xFF) * fog));
			int G = std::max(10, int(((v >>  8) & 0xFF) * fog));
			int R = std::max(10, int(((v >> 16) & 0xFF) * fog));
			out[i] = dword(B) | (dword(G) << 8) | (dword(R) << 16) | 0xFF000000u;
		}
	}
}

static void Render_DeferredFogPass() {
	if (!CurScene || !(CurScene->Flags & Scn_Fogged)) return;
	if (!g_gbuffer || !ZPage16 || !VPage) return;
	const float invFZP = 1.0f / CurScene->FZP;
	constexpr auto numTilesX = 6;
	constexpr auto numTilesY = 4;
	const auto tileSizeX = (XRes + (numTilesX - 1)) / numTilesX;
	const auto tileSizeY = (YRes + (numTilesY - 1)) / numTilesY;
	renderns::tileCounter = 0;
	for (int j = 0; j < numTilesY; ++j) {
		const int y1 = tileSizeY * j;
		const int y2 = std::min(y1 + tileSizeY, YRes);
		for (int i = 0; i < numTilesX; ++i) {
			const int x1 = tileSizeX * i;
			const int x2 = std::min(x1 + tileSizeX, XRes);
			ThreadPool::instance().enqueue([x1, y1, x2, y2, invFZP]() {
				Render_DeferredFogPass_Tile(x1, y1, x2, y2, invFZP);
				std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
				++renderns::tileCounter;
				renderns::condition.notify_one();
			});
		}
	}
	std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
	renderns::condition.wait(lock, []{
		return renderns::tileCounter == numTilesX * numTilesY;
	});
}

// Per-frame depth pre-pass over every Omni_CastsShadow light. For each
// such light we build a temporary Camera (look down IDir, FOV = spot
// outer cone), swap the engine's view globals to it, re-run
// Transform_Objects to project every visible face into the light's
// view+screen space, then walk FList and rasterize depth-only into the
// light's ShadowMap. Globals are saved/restored around the whole
// thing — when this returns the main view + perspective + clipper are
// untouched.
//
// Re-uses the main FList buffer. Transform_Objects overwrites it
// with this light's CAll faces; the caller (Render()) re-runs
// Transform_Objects with the main camera after this returns to
// repopulate FList for the main pass. (One extra Transform_Objects per
// shadow light per frame — acceptable until we profile.)
//
// Gate: env `FDS_SHADOWS=1`. Off by default while wiring.
static bool shadowsEnabled() {
	static const bool cached = fdsEnvBool("FDS_SHADOWS", FDS_SHADOWS_DEFAULT_ON);
	return cached;
}

thread_local bool g_inShadowPass = false;

// PolyId is the production default. FDS_SHADOW_POLYID=0 forces back to
// Depth at startup (F3 still toggles at runtime). The compile-time
// FDS_SHADOW_POLYID_DEFAULT_ON applies when the env var is unset —
// keeps the WASM build on PolyId without needing browser env vars.
std::atomic<ShadowMode> g_shadowMode{
	fdsEnvBool("FDS_SHADOW_POLYID", FDS_SHADOW_POLYID_DEFAULT_ON)
		? ShadowMode::PolyId : ShadowMode::Depth};


void Render_DeferredShadowMaps(Scene *Sc)
{
	if (!shadowsEnabled()) return;
	if (!Sc || g_shadowMaps.empty()) return;

	// FDS_SHADOW_PROF=1: per-shadow-pass timing breakdown of Transform vs
	// Rasterize cost, averaged over a rolling window and printed every N
	// frames. Tells us where to focus optimization — if Transform >> Raster,
	// cross-light parallelism is moot and Transform itself needs work.
	static const bool sProfShadow = []{
		const char *s = std::getenv("FDS_SHADOW_PROF");
		return s && s[0] == '1';
	}();
	using clk = std::chrono::high_resolution_clock;
	static thread_local int sFrameIdx = 0;
	static thread_local double sXformAcc = 0.0;
	static thread_local double sRasterAcc = 0.0;
	static thread_local int sLightCount = 0;

	// Save engine state.
	Camera *savedView   = View;
	const float savedFOVX  = FOVX;
	const float savedFOVY  = FOVY;
	const float savedCntrEX = CntrEX;
	const float savedCntrEY = CntrEY;

	Camera lightCam = {};

	for (ShadowMap& sm : g_shadowMaps) {
		Omni *O = sm.omni;
		if (!O) continue;
		if (!(O->Flags & Omni_Active)) continue;
		if (O->Type != Light_SpotLight) continue;  // omni cube maps deferred

		// Light camera. Look from O->IPos toward IPos+IDir, world-up=Y.
		// Roll=0. FOV = 2 * acos(cosOuter) so the outer cone fits in
		// the shadow map. Pad a bit so silhouettes near the cone edge
		// have a few pixels of context (otherwise floating-point in
		// the lighting kernel's cone test lands pixels just past the
		// shadow map edge that read as "lit").
		// Kick_Camera has a singularity at IDir=(0,±1,0) — it builds the
		// horizontal basis as N=(V.z,0,-V.x) and normalizes, which goes
		// 0/0 when the look direction is exactly world-up. Nudge by a
		// tiny X bias if the caller handed us a perfectly-vertical IDir.
		Vector idir = O->IDir;
		if (std::fabs(idir.x) < 1e-4f && std::fabs(idir.z) < 1e-4f) {
			idir.x = 0.01f;
		}
		Vector targ = O->IPos;
		targ.x += idir.x;
		targ.y += idir.y;
		targ.z += idir.z;
		Kick_Camera(&O->IPos, &targ, 0.0f, lightCam.Mat);
		lightCam.ISource = O->IPos;
		const float cosOuter = std::max(0.01f, O->FallOff);
		const float fovHalfRad = std::acos(cosOuter) * 1.10f;  // 10% pad
		const float perspXY = (float(sm.xres) * 0.5f) / std::tan(fovHalfRad);
		lightCam.PerspX = perspXY;
		lightCam.PerspY = perspXY;
		lightCam.IFOV   = (fovHalfRad * 2.0f) * (180.0f / PI);

		// Stash the light camera's pose onto the ShadowMap so the
		// lighting kernel can sample later without rebuilding it.
		Matrix_Copy(sm.lightViewMat, lightCam.Mat);
		sm.lightISource = O->IPos;
		sm.perspX = perspXY;
		sm.perspY = perspXY;
		sm.cntrX  = float(sm.xres) * 0.5f - 0.5f;
		sm.cntrY  = float(sm.yres) * 0.5f - 0.5f;

		// Swap globals to the shadow camera. XRes/YRes are passed via
		// Transform_Objects(Sc, xres, yres) override instead of mutating
		// the globals — Transform_Objects's visibility-flag math (incl.
		// the F->VisibilityFlagsAll face filter) needs to use the shadow
		// rect, not the main screen, otherwise hex tiles near the cone
		// edge get all-3-verts-VisDown and the face filter drops them.
		View   = &lightCam;
		FOVX   = perspXY;
		FOVY   = perspXY;
		CntrEX = sm.cntrX;
		CntrEY = sm.cntrY;

		// Patch the scene's FZP for the duration so Transform_Objects
		// clips against the light's range, not the camera's. We restore
		// it inside the loop because the next shadow light may have a
		// different range. Final restore happens via SetCurrentScene at
		// the bottom.
		const float savedSceneFZP = Sc->FZP;
		Sc->FZP = sm.fzp;

		// Refresh sm.zScale to match what the rasterizer expects.
		sm.zScale = float(0xFF00) / (sm.fzp * 1.1f);

		// Clear shadow map.
		std::fill(sm.depth.begin(), sm.depth.end(), uint16_t(0));
		std::fill(sm.polyId.begin(), sm.polyId.end(), uint8_t(0));

		// Transform from the light's POV. Overwrites FList + per-vertex
		// PX/PY/RZ — main pass re-transforms after we return.
		// Skip Animate_Objects: the scene driver already animated this
		// frame's object poses before calling us. We just re-project
		// those poses through the light camera; calling Animate_Objects
		// again would re-tick per-frame state (object splines,
		// particles, anything its TriMesh-walk touches) and diverge
		// the main pass.
		const auto tXformStart = clk::now();
		g_inShadowPass = true;
		Transform_Objects(Sc, sm.xres, sm.yres);
		g_inShadowPass = false;
		const auto tXformEnd = clk::now();
		if (sProfShadow) {
			sXformAcc += std::chrono::duration<double, std::milli>(tXformEnd - tXformStart).count();
		}

		const auto tRasterStart = clk::now();
		// Depth-only raster every visible face into sm.depth + sm.polyId.
		// Tile-parallel: split the shadow map into 6×4 tiles, dispatch
		// one job per tile to the engine threadpool. Each worker runs
		// its own FrustumClipper bounded to its tile rect — the clipper
		// trims each face down to the tile, and tiles never overlap, so
		// there's no contention on sm.depth/polyId between workers.
		// Same dispatch pattern as RenderInnerMekalele for the main pass.
		{
			std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
			renderns::tileCounter = 0;
		}
		const int numTilesX = 6;
		const int numTilesY = 4;
		const int totalTiles = numTilesX * numTilesY;
		const int tileSizeX = (sm.xres + numTilesX - 1) / numTilesX;
		const int tileSizeY = (sm.yres + numTilesY - 1) / numTilesY;
		ShadowMap * const smPtr = &sm;
		for (int ty = 0; ty < numTilesY; ++ty) {
			const float y1f = float(ty * tileSizeY);
			const float y2f = float(std::min((ty + 1) * tileSizeY, sm.yres));
			for (int tx = 0; tx < numTilesX; ++tx) {
				const float x1f = float(tx * tileSizeX);
				const float x2f = float(std::min((tx + 1) * tileSizeX, sm.xres));
				ThreadPool::instance().enqueue(
					[Sc, smPtr, x1f, y1f, x2f, y2f]() {
						g_currentShadowMap = smPtr;
						FrustumClipper clipper;
						clipper.InitViewport(Sc);
						clipper.SetClippingExtents(x1f, y1f, x2f, y2f);
						for (int i = 0; i < CAll; ++i) {
							Face *F = FList[i];
							if (!F) continue;
							if (!F->Txtr) continue;
							if (F->A == F->B) continue;
							if (F->A->TPos.z <= 0.0f &&
							    F->B->TPos.z <= 0.0f &&
							    F->C->TPos.z <= 0.0f) {
								continue;
							}
							clipper.Render(F, MekaleleShadowDepth, false,
							               /*skipMipLevel=*/true);
						}
						g_currentShadowMap = nullptr;
						std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
						++renderns::tileCounter;
						renderns::condition.notify_one();
					});
			}
		}
		{
			std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
			renderns::condition.wait(lock,
				[totalTiles]{ return renderns::tileCounter >= totalTiles; });
		}
		const auto tRasterEnd = clk::now();
		if (sProfShadow) {
			sRasterAcc += std::chrono::duration<double, std::milli>(tRasterEnd - tRasterStart).count();
			++sLightCount;
		}

		Sc->FZP = savedSceneFZP;
	}

	if (sProfShadow) {
		++sFrameIdx;
		// Print every 60 frames (~2s at 30 fps).
		if (sFrameIdx % 60 == 0) {
			const double xformAvg  = sXformAcc  / 60.0;
			const double rasterAvg = sRasterAcc / 60.0;
			const double perLightX = sLightCount ? sXformAcc / sLightCount : 0.0;
			const double perLightR = sLightCount ? sRasterAcc / sLightCount : 0.0;
			std::fprintf(stderr,
				"[SHADOW-PROF] last 60 frames: total/frame: xform=%.2fms raster=%.2fms "
				"sum=%.2fms  per-light: xform=%.2fms raster=%.2fms  "
				"(N=%d lights/frame)\n",
				xformAvg, rasterAvg, xformAvg + rasterAvg,
				perLightX, perLightR,
				int(sLightCount / 60));
			std::fflush(stderr);
			sXformAcc = 0.0;
			sRasterAcc = 0.0;
			sLightCount = 0;
		}
	}

	// Restore engine state. SetCurrentScene refreshes C_FZP / g_zscale
	// from the un-patched scene FZP so the main pass uses the right
	// depth-encoding constants again.
	View   = savedView;
	FOVX   = savedFOVX;
	FOVY   = savedFOVY;
	CntrEX = savedCntrEX;
	CntrEY = savedCntrEY;
	SetCurrentScene(Sc);

	// Each shadow pass overwrote per-vertex PX/PY/RZ + FList/CAll with
	// the light-camera projection. Re-run Transform_Objects with the
	// main camera so the subsequent Render() sees a consistent scene
	// state. Cost: one extra Transform_Objects per frame on scenes
	// that have any shadow-casting lights. Acceptable until we
	// rewrite Transform_Objects to write into a thread-local Vertex
	// scratch. Animate_Objects not re-run (see comment inside the
	// per-light loop above).
	Transform_Objects(Sc);

	// Precompute the per-shadow-map "view-space → light-view-space"
	// affine, with the main camera now restored on View. Derivation:
	//     pixelWorld     = mainView->Mat^T * pixelView + mainView->ISource
	//     pixelLightView = sm.lightViewMat * (pixelWorld - sm.lightISource)
	//   ⇒ pixelLightView = (sm.lightViewMat * mainView->Mat^T) * pixelView
	//                      + sm.lightViewMat * (mainView->ISource - sm.lightISource)
	// The kernel reads `viewToLight` (3×3) and `viewToLightOffset` (vec)
	// per pixel — no per-pixel world-space round-trip.
	for (ShadowMap& sm : g_shadowMaps) {
		if (!sm.omni) continue;
		// Build sm.lightViewMat * mainView->Mat^T. MatrixXMatrix(A, B, R)
		// gives R = A * B. Need the transpose of mainView->Mat first.
		Matrix mvT;
		for (int r = 0; r < 3; ++r)
			for (int c = 0; c < 3; ++c)
				mvT[r][c] = View->Mat[c][r];
		MatrixXMatrix(sm.lightViewMat, mvT, sm.viewToLight);

		// Offset: sm.lightViewMat * (mainView->ISource - sm.lightISource).
		Vector d;
		Vector_Sub(&View->ISource, &sm.lightISource, &d);
		MatrixXVector(sm.lightViewMat, &d, &sm.viewToLightOffset);
	}

	// FDS_DUMP_SHADOWMAP=1: write each shadow map as a .pgm under
	// /tmp/shadowmap_<N>.pgm. One-shot per process — only dumps on
	// the first call.
	static bool dumpedShadowMap = false;
	if (!dumpedShadowMap && std::getenv("FDS_DUMP_SHADOWMAP")) {
		dumpedShadowMap = true;
		for (size_t i = 0; i < g_shadowMaps.size(); ++i) {
			const ShadowMap& sm = g_shadowMaps[i];
			char path[256];
			std::snprintf(path, sizeof(path), "/tmp/shadowmap_%zu.pgm", i);
			FILE *f = std::fopen(path, "wb");
			if (!f) continue;
			std::fprintf(f, "P5\n%d %d\n255\n", sm.xres, sm.yres);
			for (uint16_t e : sm.depth) {
				// e in [0..0xFFFF]; 0 = empty, 0xFFFF = closest. Scale
				// to 0..255 for the PGM viewer. Empty/background pixels
				// render as black.
				uint8_t b = uint8_t(e >> 8);
				std::fwrite(&b, 1, 1, f);
			}
			std::fclose(f);
			std::fprintf(stderr, "[SHADOWMAP] dumped %s\n", path);
		}
	}
}

void Render(RenderPath path)
{
	constexpr auto		numTilesX = 6;
	constexpr auto		numTilesY = 4;
	const auto			tileSizeX = (XRes + (numTilesX - 1)) / numTilesX;
	const auto			tileSizeY = (YRes + (numTilesY - 1)) / numTilesY;

	renderns::tileCounter = 0;
	// Deferred only if env var set AND the G-buffer is sized for the
	// current framebuffer. The cube-map bake in Initialize_City swaps
	// MainSurf to a 1024x1024 TmpSurf — XRes/YRes change without our
	// engine-sized G-buffer being resized — so we'd write past the
	// vector. Safer to fall back to forward in that case (and any
	// other ctx where storage hasn't been provisioned).
	//
	// RenderPath::ForceForward bypasses the deferred dispatch — used by
	// RenderSkyCube and the City cube-map bake, whose lighting models
	// are unsuitable for the deferred per-pixel pass.
	// RenderPath::ForceDeferred takes the deferred path regardless of
	// the FDS_DEFERRED env var (which is read-once-and-cached) so
	// snapshot harnesses can render both modes in a single process.
	bool deferred;
	if (path == RenderPath::ForceForward)       deferred = false;
	else if (path == RenderPath::ForceDeferred) deferred = true;
	else                                        deferred = deferredEnabled();
	if (deferred && (!g_gbuffer || g_gbuffer->normal.size() < size_t(XRes) * size_t(YRes))) {
		deferred = false;
	}

	// Clear G-buffer mat32 to a sentinel pattern before each deferred
	// raster pass. Mekalele writes mat32 only for the pixels it covers;
	// any pixel rendered by a forward filler (sky cube, City reflective
	// windows) leaves mat32 untouched. The deferred lighting pass then
	// skips matID >= count via the existing range check, leaving those
	// forward-rendered pixels alone.
	if (deferred && g_gbuffer) {
		const size_t numPixels = size_t(XRes) * size_t(YRes);
		parallel_memset(g_gbuffer->txtr.data(), 0xFF, numPixels * sizeof(uint32_t));
		// Transparent G-buffer: mat32=0xFFFFFFFF sentinel ("no transparent
		// front face here"); xpr-Z=0 ("empty, anything passes Z-test").
		if (g_gbufferTransparent) {
			parallel_memset(g_gbufferTransparent->txtr.data(), 0xFF,
			                numPixels * sizeof(uint32_t));
		}
		if (g_xparZ) {
			parallel_memset(g_xparZ, 0, numPixels * sizeof(uint16_t));
		}
		// Back layer (depth-peel slot for back-facing transparents).
		if (g_gbufferTransparentBack) {
			parallel_memset(g_gbufferTransparentBack->txtr.data(), 0xFF,
			                numPixels * sizeof(uint32_t));
		}
		if (g_xparZBack) {
			parallel_memset(g_xparZBack, 0, numPixels * sizeof(uint16_t));
		}
	}

	for (auto j = 0; j < numTilesY; ++j) {
		auto y1 = tileSizeY * j;
		auto y2 = std::min(y1 + tileSizeY, YRes);
		for (auto i = 0; i < numTilesX; ++i) {
			auto x1 = tileSizeX * i;
			auto x2 = std::min(x1 + tileSizeX, XRes);

			if (deferred) {
				ThreadPool::instance().enqueue([x1, y1, x2, y2]() { RenderInnerMekalele(x1, y1, x2, y2); });
			} else {
				ThreadPool::instance().enqueue([x1, y1, x2, y2]() { RenderInner(x1, y1, x2, y2); });
			}
			// RenderInner(x1, y1, x2, y2);
		}
	}

	{
		std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
		renderns::condition.wait(lock, [] {return renderns::tileCounter == numTilesX * numTilesY; });
	}

	if (deferred) {
		// Phase A deferred lighting pass: per-pixel ambient + Lambertian
		// + linear falloff against the scene's omnis, modulated by the
		// G-buffer's per-pixel texture sample. Replaces the per-vertex
		// Lighting() (skipped on the deferred path) plus the rasterizer's
		// colorize step in one go.
		Render_DeferredLighting();

		// Full-screen linear distance fog over deferred-lit opaque pixels.
		// Runs BEFORE the transparent peel so xpar surfaces composite onto
		// already-fogged opaque dst. Skipped on non-Scn_Fogged scenes.
		Render_DeferredFogPass();

		// Per-object peeled transparent rendering. We process each
		// transparent TriMesh independently — clear the xpar G-buffers,
		// render that mesh's transparent faces (back-facing → back
		// layer, front-facing → front layer), light the back layer
		// onto VPage, light the front layer onto VPage — before moving
		// on to the next mesh. Object order is each mesh's FIRST
		// appearance in the sorted FList, which is back-to-front by
		// the object-grouping sort key, so outer/farther meshes
		// composite before inner/closer ones.
		//
		// This is what makes nested transparent meshes work (fountain
		// outer + inner spire spheres). With a single shared pass the
		// inner mesh's back-facing tris would Z-compete with the
		// outer mesh's back-facing tris in the back layer and one of
		// them would lose. Per-object peeling gives each mesh a fresh
		// xpar G-buffer.

		const bool unifiedTbr = deferredUnifiedTbrEnabled();

		// Unified-TBR path (task #9): insert every transparent face into
		// the per-strip TBR linked lists. Sprites are still inserted in
		// the standalone post-loop further below — TBR_Render at the
		// end of Render() flushes everything together per-strip in
		// back-to-front render-Z order. The 2-deep xpar G-buffer is
		// kept (used in strip-scoped form by RenderXparClumpInStrip per
		// clump of consecutive same-(mesh, frontFacing) entries).
		if (unifiedTbr) {
			for (int32_t k = 0; k < CAll; ++k) {
				Face* F = FList[k];
				if (!F || !F->Txtr) continue;
				if (F->A == F->B) continue;  // sprite, handled by post-loop
				if (!(F->Txtr->Flags & Mat_Transparent)) continue;
				if (F->Flags & Face_Reflective) continue;  // forward handles
				if (!F->ParentTri) continue;
				InsertTransparentToTBR(F);
			}
			// Skip the legacy per-mesh xpar peel below.
			goto xpar_done;
		}

		{
		auto runTilePass = [&](auto workFn) {
			renderns::tileCounter = 0;
			for (auto j = 0; j < numTilesY; ++j) {
				auto y1 = tileSizeY * j;
				auto y2 = std::min(y1 + tileSizeY, YRes);
				for (auto i = 0; i < numTilesX; ++i) {
					auto x1 = tileSizeX * i;
					auto x2 = std::min(x1 + tileSizeX, XRes);
					const int tileIdx = j * numTilesX + i;
					ThreadPool::instance().enqueue([x1, y1, x2, y2, tileIdx, workFn]() {
						workFn(x1, y1, x2, y2, tileIdx);
					});
				}
			}
			std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
			renderns::condition.wait(lock, []{ return renderns::tileCounter == numTilesX * numTilesY; });
		};

		const size_t numPixels = size_t(XRes) * size_t(YRes);
		auto clearBack = [&]() {
			if (g_gbufferTransparentBack) {
				parallel_memset(g_gbufferTransparentBack->txtr.data(), 0xFF,
				                numPixels * sizeof(uint32_t));
			}
			if (g_xparZBack) parallel_memset(g_xparZBack, 0, numPixels * sizeof(uint16_t));
		};
		auto clearFront = [&]() {
			if (g_gbufferTransparent) {
				parallel_memset(g_gbufferTransparent->txtr.data(), 0xFF,
				                numPixels * sizeof(uint32_t));
			}
			if (g_xparZ) parallel_memset(g_xparZ, 0, numPixels * sizeof(uint16_t));
		};

		// One walk through FList in sort order. The xpar sort key (built
		// in Transform_Objects) already encodes the correct back-to-front
		// order for concentric meshes via a per-object extent bias —
		// outer.back, inner.back, inner.front, outer.front for a nested
		// pair. We just need to render each face in that order without
		// regrouping by mesh.
		//
		// To keep the 2-deep G-buffer's pixel budget (one back-layer
		// slot + one front-layer slot per screen pixel), batch
		// consecutive faces that share BOTH (mesh, side). When the
		// bucket changes, flush: clear the side's layer, raster the
		// batch with parentFilter + faceSel, composite the side.
		// Consecutive same-(mesh,side) faces naturally co-cluster in
		// the sorted list because the sort key has a high-bit partition
		// per side and a per-mesh score within.
		TriMesh* batchMesh    = nullptr;
		bool     batchFront   = false;
		bool     batchOpen    = false;
		int      batchFirst   = 0;
		int      batchLastEnd = 0;
		auto flushBatch = [&]() {
			if (!batchOpen) return;
			TriMesh*    T   = batchMesh;
			XparFaceSel sel = batchFront ? XparFaceSel::FrontOnly
			                              : XparFaceSel::BackOnly;
			const int first = batchFirst;
			const int count = batchLastEnd - batchFirst;
			if (batchFront) clearFront();
			else            clearBack();
			runTilePass([first, count, T, sel](int x1, int y1, int x2, int y2, int /*tileIdx*/) {
				RenderInnerDeferredTransparent(float(x1), float(y1),
				                                float(x2), float(y2),
				                                first, count, T, sel);
			});
			if (batchFront) {
				runTilePass([](int x1, int y1, int x2, int y2, int tileIdx) {
					Render_DeferredTransparentLighting_Tile<XparLayer::Front>(
						g_deferredCtx, tileIdx, x1, y1, x2, y2);
				});
			} else {
				runTilePass([](int x1, int y1, int x2, int y2, int tileIdx) {
					Render_DeferredTransparentLighting_Tile<XparLayer::Back>(
						g_deferredCtx, tileIdx, x1, y1, x2, y2);
				});
			}
			batchOpen = false;
		};

		for (int32_t k = 0; k < CAll; ++k) {
			Face* F = FList[k];
			if (!F || !F->Txtr) continue;
			if (!(F->Txtr->Flags & Mat_Transparent)) continue;
			if (F->Flags & Face_Reflective) continue;  // forward handles
			TriMesh* T = F->ParentTri;
			if (!T) continue;

			const bool front = IsFrontFacingInViewSpace(F);
			if (!batchOpen || T != batchMesh || front != batchFront) {
				flushBatch();
				batchMesh    = T;
				batchFront   = front;
				batchFirst   = k;
				batchOpen    = true;
			}
			batchLastEnd = k + 1;
		}
		flushBatch();
		}  // close unifiedTbr=false legacy block
xpar_done: ;
	}

	int32_t I = CAll;
	Face **FLS = FList;//+CAll-1;
	Vertex *A,*B;
	
	while (I--)
	{
		Face *F = *FLS++;
		
		// Get Mapping Coordinates from the rendered face.
		A=F->A; B=F->B; 
				
		// TODO: change this into a flag check
		if (A == B)
		{
			// Particle/Sprite - no far-Z clipping
			if (A->TPos.z < C_NZP) continue;
			F->Filler(F, &A, 1, 0);
		}
	}

	if (CurScene->Flags & Scn_SpriteTBR)
	{
		TBR_Render(CurScene);
	}
	

#ifdef TRACE_OBJECTS
	//printf("frame: %f\n\t", CurFrame);
	Camera* Cam = CurScene->CameraHead;

	for (Object* O = CurScene->ObjectHead; O; O = O->Next)
	{
		if (O->Type != Obj_TriMesh) continue;
		TriMesh* T = (TriMesh*)O->Data;
		if (!(T->Flags & Tri_Invisible) && T->Flags & Tri_Ahead) {
			if (T->BSphereScreenPos.x < XRes && T->BSphereScreenPos.y < YRes && T->BSphereScreenPos.x > 0 && T->BSphereScreenPos.y > 0) {
				char text[255] = { 0 };
				snprintf(text, 254, "%s (%d)", O->Name, O->Number);
				OutTextXY(VPage, T->BSphereScreenPos.x, T->BSphereScreenPos.y, text, 255);
			}

			DrawSpline(T->Pos, View->ISource, Fist(CurScene->EndFrame));
		}
	}
#endif

	//test for edges stuff
/*	for(Tri=CurScene->TriMeshHead;Tri;Tri=Tri->Next)
	{
		if (Tri->Flags&Tri_Invisible) continue;
		for(I=0;I<Tri->EIndex;I++)
		if (Tri->Edges[I].Flags&Edge_Concave)
		Bivex_Frustrum_Perspective(Tri->Edges[I].A,Tri->Edges[I].B,&Draw_Line_32Bit_ForKids);
		else
		Bivex_Frustrum_Perspective(Tri->Edges[I].A,Tri->Edges[I].B,&Draw_Line_32Bit);
    }
	*/
/*  for(EP = ColEdge_Stack;EP<CES;EP++)
    Bivex_Frustrum_Perspective(EP[0]->A,EP[0]->B,&Draw_Line_32Bit_ForKids);*/
	
	//test for vertex stuff
	/*  DWord *VP = (DWord *)VPage;
	for(Tri=CurScene->TriMeshHead;Tri;Tri=Tri->Next)
	{
    if (Tri->Flags&Tri_Invisible) continue;
    for(I=0;I<Tri->VIndex;I++)
	if (Tri->Verts[I].Flags==Vtx_Spike)
	VP[YOffs[(int)Tri->Verts[I].PY]+(int)Tri->Verts[I].PX] = 0xFFFFFFFF;
}*/

	// FDS_DEBUG_XPAR_IDS=1 overlays each transparent face's render-order
	// index and SortZ at its screen centroid. Lets you read the actual sort
	// values that drove rendering — useful for diagnosing back-to-front
	// ordering on real scenes.
	static const bool debugXparIds = []{
		const char* s = std::getenv("FDS_DEBUG_XPAR_IDS");
		return s && s[0] == '1';
	}();
	if (debugXparIds) {
		// Min screen area threshold (px²) — skips tiny particles. Override
		// with FDS_DEBUG_XPAR_MIN_AREA. Also dumps the full list to stderr
		// regardless of the visual filter.
		float minArea = 500.0f;
		if (const char* s = std::getenv("FDS_DEBUG_XPAR_MIN_AREA")) {
			minArea = float(std::atof(s));
		}
		std::fprintf(stderr, "[XPAR-IDS] (all transparent faces in render order)\n");
		for (int32_t i = 0; i < CAll; ++i) {
			Face* F = FList[i];
			if (!F || !F->Txtr) continue;
			if (!(F->Txtr->Flags & Mat_Transparent)) continue;
			const float ax = F->A->PX, ay = F->A->PY;
			const float bx = F->B->PX, by = F->B->PY;
			const float cxv = F->C->PX, cyv = F->C->PY;
			const float area = 0.5f * std::fabs(ax*(by-cyv) + bx*(cyv-ay) + cxv*(ay-by));
			const float cx = (ax + bx + cxv) * (1.0f/3.0f);
			const float cy = (ay + by + cyv) * (1.0f/3.0f);
			const float azv = F->A->TPos.z, bzv = F->B->TPos.z, czv = F->C->TPos.z;
			std::fprintf(stderr,
				"  [%4d] sortZ=%8.2f  tri-zs=(%.2f,%.2f,%.2f)  area=%6.0f  "
				"centroid=(%.0f,%.0f)  mat=%s\n",
				int(i), F->SortZ.F, azv, bzv, czv, area, cx, cy,
				(F->Txtr->Name ? F->Txtr->Name : "?"));
			if (area < minArea) continue;
			if (cx < 0 || cy < 0 || cx >= XRes || cy >= YRes) continue;
			char text[64];
			std::snprintf(text, sizeof(text), "%d z=%.1f", int(i), F->SortZ.F);
			OutTextXY(VPage, int(cx), int(cy), text, 255);
		}
	}
}

// yeah
char Check_Texture_Memory_Range(Scene *Sc)
{
	TriMesh *T;
	Face *F,*FE;
	Material *M;
	Material *Ptr;
	for(T=Sc->TriMeshHead;T;T=T->Next)
	{
		F = T->Faces;
		FE = F + T->FIndex;
		for(;F<FE;F++)
		{
			Ptr = F->Txtr;
			if (!Ptr) continue;
			for(M=MatLib;M;M=M->Next)
				if (M==Ptr)
					break;
				if (!M)
				{
					printf("Address generation Interlock!!\n"); // some ppl say Asyncrontus Gateway Interlock (AGI) delay...one of them is me -Silvatar.
					printf("Ptr = %ph\n", Ptr);
					printf("Material Name = %s\n",Ptr->Name);
					return 0;
				}
		}
	}
	//  for(M=MatLib;M;M=M->Next)
	//    printf("Mat-lib = %xh ==> txtr = %xh\n",(int32_t)M,(int32_t)(M->Txtr));
	return 1;
}

void SetCurrentScene(Scene *Sc)
{
	clipper.InitViewport(Sc);
	CurScene = Sc;
	g_zscale = (float)0xff00 / (CurScene->FZP * 1.1);
	g_zscale256 = g_zscale / 256.0f;

	C_FZP = Sc->FZP;
	C_rFZP = 1.0f/C_FZP;
	C_NZP = Sc->NZP;
	C_rNZP = 1.0f/C_NZP;

}

void RunScene(Scene *Sc,float Seconds)
{
	float t;
	int32_t TTrd=0;
	int32_t Par1,Par2,Par3,Par4,Par5,Par6,Tot;
	int32_t RenderedPolys=0;
	int32_t Y;
	int32_t SceneTime = Seconds * Timer_Freq;
	char MotionBlur_Enabled = 0;
	
	int32_t I,J;
	float FPS;
	char *Str = new char[80];
	char AfterFirstFrame = 0,Cur = 0;
	byte *BPage = new byte[PageSize];
	memset(BPage,0,PageSize);
	VESA_Surface Blurred;
	memcpy(&Blurred,Screen,sizeof(VESA_Surface));
	Blurred.Data = BPage;
	Blurred.Flags = VSurf_Noalloc;
	Blurred.Targ = VGAPtr;
	
	Layer2.Data = new byte [PageSize];
	Layer2.BPP = 32;
	memset(Layer2.Data,0,sizeof(PageSize));
	Layer2.Flags = VSurf_Noalloc;
	Layer2.Targ = VGAPtr;
	
	Material *M;
	Object *Obj;
	SetCurrentScene(Sc);
	CurFrame=Sc->StartFrame;
	View=&FC;
	View->Next=Sc->CameraHead;
	if (Sc->CameraHead)
	{
		Sc->CameraHead->Prev=View;
		View=View->Next;
	}
	
	FList_Allocate(Sc);

	if (!Check_Texture_Memory_Range(Sc)) return;
	
	/*  for(Obj = Sc->ObjectHead;Obj;Obj=Obj->Next)
	printf("Object (%s)'s Pivot is (%f,%f,%f)\n",Obj->Name,Obj->Pivot.x,Obj->Pivot.y,Obj->Pivot.z);*/
	
	Timer=0; Frames=0;
	CurFrame=0;
	
	while (!Keyboard[ScESC])
	{
		if (Timer>SceneTime)
		{
			Frames=0; RenderedPolys=0;
			Timer-=SceneTime;
			TTrd-=SceneTime;
			Restore_Splines(Sc);
			// clear message buffer
			for(I=0;I<10;I++)
			{
				delete MsgStr[I];
				MsgStr[I]=NULL;
			}
		}
		t=float(Timer)/(float)SceneTime;
		
		CurFrame=float((1.0-t)*Sc->StartFrame)+float(t*Sc->EndFrame);
		if (!Keyboard[ScD])
		{
			memset(VPage,0,PageSize);
			//      memset(Layer2.Data,0,PageSize);
		}
		if (AfterFirstFrame) //so lame
		{
			dTime = Timer-TTrd;
			TTrd = Timer;
			if (View==&FC) Dynamic_Camera();
			dTime *= 0.25;
		} else {AfterFirstFrame=1; TTrd = Timer; dTime = 0;}
		Animate_Objects(Sc);
		Transform_Objects(Sc); Par1 = Timer;
		if (!(Sc->Flags&Scn_Nolighting)) Lighting(Sc); Par4 = Timer;
		
		//if (Sc->PclExec) Sc->PclExec(); Par2 = Timer;
		if (!CAll) goto Away;
		Radix_Sorting();
		//    Quick_Sorting(0,CAll-1);
		if (!Keyboard[ScE]) Render();     Par5 = Timer;
Away:Tot = Timer-TTrd;
	 FPS = 100.0/((float)(Tot));
	 Frames++;
	 RenderedPolys+=CPolys;
	 
	 Y = -15;
	 for(I=0;I<10;I++)
		 if (MsgStr[I])
		 {
			 Y = OutTextXY(VPage,0,Y+15 * g_fontScale,MsgStr[I],64);
			 if (Timer>MsgClock[I]+150)
			 {
				 delete MsgStr[I];
				 MsgStr[I]=NULL;
			 }
		 }
#ifdef RUNTIME_INFO_L1
		 snprintf(Str, 80,"Time = %d , Frame = %5.1f",Timer.load(), CurFrame);
		 Y = OutTextXY(VPage,0,Y+15 * g_fontScale,Str,64);
		 snprintf(Str, 80,"Inst. FPS = %3.2f (%3.2f)",FPS,100.0*(float)Frames/(float)Timer);
		 Y = OutTextXY(VPage,0,Y+15 * g_fontScale,Str,64);
#endif
#ifdef RUNTIME_INFO_L2
		 //    snprintf(Str, 80,"Active Camera = %s",View->Name);
		 //    Y = OutTextXY(VPage,0,Y+15 * g_fontScale,Str,64);
		 //    if (Tot)
		 //		{
		 //			snprintf(Str, 80,"Perf. Distribution: xForm=%d%%,P.Knmtx=%d%%,FList=%d%%,Light=%d%%,\nRender=%d%%",(Par1-TTrd)*100/Tot,(Par2-Par1)*100/Tot,(Par3-Par2)*100/Tot,(Par4-Par3)*100/Tot,(Par5-Par4)*100/Tot);
		 //			Y = OutTextXY(VPage,0,Y+15 * g_fontScale,Str,64);
		 //		}
		 //    snprintf(Str, 80,"Dyn. Kinematics: p(%4.2f,%4.2f,%4.2f),v(%1.4f,%1.4f,%1.4f) r(%1.4f,%1.4f,%1.4f)",FC.ISource.x,FC.ISource.y,FC.ISource.z,FV.x,FV.y,FV.z,FT.x,FT.y,FT.z);
		 //    Y = OutTextXY(VPage,0,Y+15 * g_fontScale,Str,64);
		 snprintf(Str, 80,"Average Polys/Frame: %5.1f (%3.1f%%)",(float)RenderedPolys/(float)Frames,100.0*(float)CPolys/(float)Polys);
		 Y = OutTextXY(VPage,0,Y+15 * g_fontScale,Str,64);
		 int32_t GTP = 0,GTPP = 0;
		 for(TriMesh *Tri = Sc->TriMeshHead;Tri;Tri = Tri->Next)
			 if (!(Tri->Flags&Tri_Invisible)) {GTP++; GTPP += Tri->FIndex;}
			 snprintf(Str, 80,"%d Triangle Meshes totalling in %d Faces",GTP,GTPP);
			 Y = OutTextXY(VPage,0,Y+15 * g_fontScale,Str,64);
#endif
#ifdef RUNTIME_INFO_L3
			 snprintf(Str, 80,"Rendered %d Polygons,%d Omnilights and %d particles.\n",CPolys,COmnies,CPcls);
			 Y = OutTextXY(VPage,0,Y+15 * g_fontScale,Str,64);
#endif
			 //    DisplayMouse();
			 //		Radiosity(Sc);
			 
			 if (!Keyboard[ScD])
			 {
				 //hope it works ;)
				 if (MotionBlur_Enabled)
				 {
					 if (MMXState)
						 Modulate(Screen,&Blurred,0xb0b0b0,0xb0b0b0, PageSize);
					 else
						 Transparence(Screen,&Blurred);
					 Flip(&Blurred);
				 }
				 //else memcpy(BPage,VPage,PageSize); //v.fast flip emulation
				 else Flip(Screen);
				 
				 /*      if (MMXState)
				 MulModulate(&Layer2,Screen);
				 else*/
				 //    Transparence(&Layer2,Screen);
			 }
			 
			 if (Keyboard[ScP]) { I = Timer; while (Keypressed()) {}; Timer = I; } //Timefreeze
			 if (Keyboard[ScTab]) { if (View->Next) { View = View->Next; } else { View = &FC; } }
			 /*		if (Keyboard[ScR]) {
			 Restore_9();
			 Deinitialize_VESA();
			 printf("Debug me, PLEASE!\n");
			 getchar();
			 getchar();
			 getchar();
			 exit(-1);
		}*/
			 
			 // Free camera ASSUME IDENTITY command.
			 if (Keyboard[ScC]) {FC.ISource = View->ISource; Matrix_Copy(FC.Mat,View->Mat); FC.IFOV=View->IFOV;}
			 if (Keyboard[ScH]) Cam_HeadLight^=1;
			 if (Keyboard[ScM]) MotionBlur_Enabled^=1;
			 if (Keyboard[ScOpenSq]) Sc->FZP+=100;
			 if (Keyboard[ScCloseSq]) Sc->FZP-=100;
			 if (Keyboard[ScY])
			 {
				 FILE *ff = fopen("bearing.","at");
				 fprintf(ff,"POS = (%5.3f,%5.3f,%5.3f), ORIENT = (%5.3f,%5.3f,%5.3f)\n",FC.ISource.x,FC.ISource.y,FC.ISource.z,FC.Mat[2][0],FC.Mat[2][1],FC.Mat[2][2]);
				 fclose(ff);
			 }
	}
	//	printf("FPS == %f\n",100.0*(float)Frames/(float)Timer);
	while (Keyboard[ScESC]) DO_NOP();
	//Exec_FPS = 100.0*(float)Frames/(float)Par5;
	/*  snprintf(Str, 80,"Inst. FPS = %3.2f (%3.2f)",FPGS,100.0*(float)Frames/(float)Par4);
	OutTextXY(VGAPtr,0,0,Str,63);
	while (!Keyboard[ScESC]) DO_NOP();
	while (Keyboard[ScESC]) DO_NOP();*/
	Restore_Splines(Sc);
	delete FList;
	delete SList;
	delete [] Str;
	delete [] BPage;
	delete Layer2.Data;
}
