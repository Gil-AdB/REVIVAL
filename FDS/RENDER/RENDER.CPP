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

void Transform_Objects(Scene *Sc)
{
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
				MatrixXVector(IM, &Vtx->N, &Vtx->TN);

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
				MatrixXVector(IM, &Vtx->N, &Vtx->TN);

				Vtx->RZ=1.0/Vtx->TPos.z;
				Vtx->PX=Vtx->TPos.x*Vtx->RZ;
				Vtx->PY=Vtx->TPos.y*Vtx->RZ;
				//        Vtx->PX=CntrEX+PX*Vtx->TPos.x*Vtx->RZ;
				//        Vtx->PY=CntrEY-PY*Vtx->TPos.y*Vtx->RZ;
				Vtx->UZ=Vtx->U*Vtx->RZ;
				Vtx->VZ=Vtx->V*Vtx->RZ;
				Vtx->Flags&=0xFFFFFFFF-Vtx_Visible;
				if (Vtx->PX<0) Vtx->Flags|=Vtx_VisLeft;
				if (Vtx->PX>=XRes) Vtx->Flags|=Vtx_VisRight;
				if (Vtx->PY<0) Vtx->Flags|=Vtx_VisUp;
				if (Vtx->PY>=YRes) Vtx->Flags|=Vtx_VisDown;
				if (Vtx->TPos.z>Sc->FZP) Vtx->Flags|=Vtx_VisFar;
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
				MatrixXVector(IM, &Vtx->N, &Vtx->TN);

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
					if (Vtx->PX>=XRes) Vtx->Flags|=Vtx_VisRight;
					if (Vtx->PY<0) Vtx->Flags|=Vtx_VisUp;
					if (Vtx->PY>=YRes) Vtx->Flags|=Vtx_VisDown;
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
				if (Vtx->PX>=XRes) Vtx->Flags+=Vtx_VisRight;
				if (Vtx->PY<0) Vtx->Flags+=Vtx_VisUp;
				if (Vtx->PY>=YRes) Vtx->Flags+=Vtx_VisDown;
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
					if (Vtx->PX>=XRes) Vtx->Flags+=Vtx_VisRight;
					if (Vtx->PY<0) Vtx->Flags+=Vtx_VisUp;
					if (Vtx->PY>=YRes) Vtx->Flags+=Vtx_VisDown;
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

// Deferred path's transparent + additive forward pass. Runs after the
// deferred lighting pass has completed so the alpha blend reads finished
// opaque pixels in VPage. Iterates FList in sort order (which puts
// transparent faces back-to-front, after all opaque) and dispatches each
// through F->Filler — preassigned in PREPROC.CPP to TheOtherBarry<TRANSPARENT>
// or TheOtherBarry<ADDITIVE> per material flags.
void RenderInnerDeferredTransparent(float x1, float y1, float x2, float y2) {
	FrustumClipper clipper;
	clipper.InitViewport(CurScene);
	clipper.SetClippingExtents(x1, y1, x2, y2);

	int32_t I = CAll;
	Face** FLS = FList;
	Vertex *A, *B, *C;

	while (I--) {
		Face* F = *FLS++;

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
			clipper.Render(F, MekaleleTransparent, false);
		} else {
			clipper.Render(F, F->Filler, false);
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
	static bool cached = []{
		const char *s = std::getenv("FDS_DEFERRED");
		return s && s[0] == '1';
	}();
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
	int             count;          // active entries
	int             paddedCount;    // (count + 7) & ~7, ≤ DEFERRED_MAX_LIGHTS
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
static void buildTileLightLists(TileLights *tileLights, int numTilesX, int numTilesY,
                                 int tileSizeX, int tileSizeY, int xres, int yres,
                                 const ViewLightsSoA &lights, int numLights)
{
	const int numTiles = numTilesX * numTilesY;
	for (int t = 0; t < numTiles; ++t) {
		tileLights[t].count = 0;
	}

	for (int li = 0; li < numLights; ++li) {
		const float vx = lights.posX[li];
		const float vy = lights.posY[li];
		const float vz = lights.posZ[li];
		const float r2 = lights.range2[li];
		const float r  = std::sqrt(r2);

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

		for (int j = tile_j_lo; j <= tile_j_hi; ++j) {
			for (int i = tile_i_lo; i <= tile_i_hi; ++i) {
				const int idx = j * numTilesX + i;
				TileLights &tl = tileLights[idx];
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
		}
		tl.paddedCount = pad_to;
	}
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
static bool deferredLightingOuterVecEnabled() {
	static bool cached = []{
		const char *s = std::getenv("FDS_DEFERRED_OUTER_VEC");
		return s && s[0] == '1';
	}();
	return cached;
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
			const dword *texData = (const dword *)Mat->Txtr->Mipmap[miplevel];
			if (!texData) continue;
			const dword texel = texData[swizzledUV];
			const float texB = float(texel & 0xFF);
			const float texG = float((texel >> 8) & 0xFF);
			const float texR = float((texel >> 16) & 0xFF);

			// Decode normal (view-space, unit-length).
			float nx, ny, nz;
			meka::oct_decode_u16(gb.normal[i], nx, ny, nz);

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
			const bool wantSpecular = specGlobalOn && (Mat->Specular > 0.0f);
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

			if (!isWater) {
				if (useVec) {
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
					// Vec path doesn't accumulate specular (would need
					// pow on Vec8f, which dominates the loop on simde);
					// fall through to scalar for spec materials.
					if (wantSpecular) {
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
						const float k = dot * lenInv * (1.0f - dist * rRange);
						const float intensity = k * Mat->Diffuse;
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
									const float spec = std::pow(NdotH, gloss);
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

			// Fog: replicate forward TheOtherBarry's per-vertex distance
			// falloff (TheOtherBarry.h:716-734). Forward applies it to
			// LR/LG/LB at vertex shading time; deferred runs per-pixel
			// using the reconstructed view-space z. Scn_Fogged scenes
			// (City, etc.) need this or distant geometry stays vivid.
			if (ctx.Sc->Flags & Scn_Fogged) {
				float fogRate = std::sqrt(std::max(0.0f, 1.0f - z * (1.0f / ctx.Sc->FZP)));
				lB = std::max(lB * fogRate, 10.0f);
				lG = std::max(lG * fogRate, 10.0f);
				lR = std::max(lR * fogRate, 10.0f);
				lB = std::min(lB, 253.0f);
				lG = std::min(lG, 253.0f);
				lR = std::min(lR, 253.0f);
			}

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
static void Render_DeferredTransparentLighting_Tile(const DeferredLightingCtx &ctx,
                                                     int tileIndex,
                                                     int x1, int y1, int x2, int y2)
{
	if (!g_gbufferTransparent || !g_xparZ) return;
	const meka::GBuffer &gbX = *g_gbufferTransparent;
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
			const word zEnc = g_xparZ[i];
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
					const float k = dot * lenInv * (1.0f - dist * vlAll->rRange[n]);
					const float intensity = k * Mat->Diffuse;
					lB += intensity * vlAll->colB[n];
					lG += intensity * vlAll->colG[n];
					lR += intensity * vlAll->colR[n];
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
					const float k = dot * lenInv * (1.0f - dist * tlTile.rRange[n]);
					const float intensity = k * Mat->Diffuse;
					lB += intensity * tlTile.colB[n];
					lG += intensity * tlTile.colG[n];
					lR += intensity * tlTile.colR[n];
				}
			}

			if (lB > 250.0f) lB = 250.0f;
			if (lG > 250.0f) lG = 250.0f;
			if (lR > 250.0f) lR = 250.0f;
			if (lB < 0.0f) lB = 0.0f;
			if (lG < 0.0f) lG = 0.0f;
			if (lR < 0.0f) lR = 0.0f;

			if (ctx.Sc->Flags & Scn_Fogged) {
				float fogRate = std::sqrt(std::max(0.0f, 1.0f - z * (1.0f / ctx.Sc->FZP)));
				lB = std::min(std::max(lB * fogRate, 10.0f), 253.0f);
				lG = std::min(std::max(lG * fogRate, 10.0f), 253.0f);
				lR = std::min(std::max(lR * fogRate, 10.0f), 253.0f);
			}

			int litB = int((texB * lB) * (1.0f / 256.0f));
			int litG = int((texG * lG) * (1.0f / 256.0f));
			int litR = int((texR * lR) * (1.0f / 256.0f));
			if (litB > 255) litB = 255;
			if (litG > 255) litG = 255;
			if (litR > 255) litR = 255;

			// Alpha-blend onto VPage: 50/50 add (matches TheOtherBarry
			// <TRANSPARENT>'s `texel + dst >> 1`, saturated).
			const dword existing = out[i];
			int outB = litB + int((existing      ) & 0xFF) / 2;
			int outG = litG + int((existing >> 8) & 0xFF) / 2;
			int outR = litR + int((existing >> 16) & 0xFF) / 2;
			if (outB > 255) outB = 255;
			if (outG > 255) outG = 255;
			if (outR > 255) outR = 255;
			out[i] = dword(outB) | (dword(outG) << 8) | (dword(outR) << 16) | 0xFF000000u;
		}
	}

	std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
	++renderns::tileCounter;
	renderns::condition.notify_one();
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

		for (; px + 8 <= x2; px += 8) {
			const size_t i = size_t(py) * XRes + px;

			// Load 8 lanes of Z (u16 → s32 zero-extend → float for the
			// cmp; later use the int form too)
			__m128i z16 = _mm_loadu_si128((const __m128i*)(ZPage16 + i));
			__m256i zEncI = _mm256_cvtepu16_epi32(z16);

			// Alive mask: zEnc != 0
			__m256i mask_alive = _mm256_cmpgt_epi32(zEncI, _mm256_setzero_si256());

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

			// Per-omni accumulate. Each omni broadcast, 8 pixels in vec.
			for (int n = 0; n < tl.count; ++n) {
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

			// Per-pixel fog (Scn_Fogged): same formula as scalar Tile.
			if (ctx.Sc->Flags & Scn_Fogged) {
				__m256 vOne   = _mm256_set1_ps(1.0f);
				__m256 vRFZP  = _mm256_set1_ps(1.0f / ctx.Sc->FZP);
				__m256 fogArg = _mm256_max_ps(zero, _mm256_sub_ps(vOne, _mm256_mul_ps(zv, vRFZP)));
				__m256 fogRate = _mm256_sqrt_ps(fogArg);
				__m256 vMin = _mm256_set1_ps(10.0f);
				__m256 vMax = _mm256_set1_ps(253.0f);
				lB = _mm256_min_ps(_mm256_max_ps(_mm256_mul_ps(lB, fogRate), vMin), vMax);
				lG = _mm256_min_ps(_mm256_max_ps(_mm256_mul_ps(lG, fogRate), vMin), vMax);
				lR = _mm256_min_ps(_mm256_max_ps(_mm256_mul_ps(lR, fogRate), vMin), vMax);
			}

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
				const float k = dot * lenInv * (1.0f - dist * tl.rRange[n]);
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
							const float specStrength = spec * Mat->Specular * (1.0f - dist * tl.rRange[n]);
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

			// Per-pixel fog (Scn_Fogged): same formula as Tile.
			if (ctx.Sc->Flags & Scn_Fogged) {
				float fogRate = std::sqrt(std::max(0.0f, 1.0f - z * (1.0f / ctx.Sc->FZP)));
				lB = std::min(std::max(lB * fogRate, 10.0f), 253.0f);
				lG = std::min(std::max(lG * fogRate, 10.0f), 253.0f);
				lR = std::min(std::max(lR * fogRate, 10.0f), 253.0f);
			}

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
		++numLights;
	}

	constexpr int numTilesX = DEFERRED_NUM_TILES_X;
	constexpr int numTilesY = DEFERRED_NUM_TILES_Y;
	const int tileSizeX = (XRes + (numTilesX - 1)) / numTilesX;
	const int tileSizeY = (YRes + (numTilesY - 1)) / numTilesY;

	// Per-tile light culling: project each omni's bounding sphere
	// into screen space, find which tiles it overlaps, populate
	// indices[] per tile.
	static TileLights tileLights[DEFERRED_NUM_TILES];
	buildTileLightLists(tileLights, numTilesX, numTilesY,
	                    tileSizeX, tileSizeY, XRes, YRes,
	                    lights, numLights);
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
	bool deferred = deferredEnabled() && (path != RenderPath::ForceForward);
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

		// Forward pass for Mat_Transparent / Mat_Additive faces. They
		// were skipped in the deferred raster pass; render them now in
		// FList sort order (back-to-front for transparent — sorted by
		// PREPROC's `2*fzp - dz` rule) over the lit opaque framebuffer.
		// Within this pass:
		//   front-facing transparents → MekaleleTransparent (writes
		//     into xpr-G-buffer; closest wins per pixel via xpr-Z)
		//   back-facing transparents  → forward TheOtherBarry (vertex
		//     Gouraud + 50/50 blend onto VPage now)
		renderns::tileCounter = 0;
		for (auto j = 0; j < numTilesY; ++j) {
			auto y1 = tileSizeY * j;
			auto y2 = std::min(y1 + tileSizeY, YRes);
			for (auto i = 0; i < numTilesX; ++i) {
				auto x1 = tileSizeX * i;
				auto x2 = std::min(x1 + tileSizeX, XRes);
				ThreadPool::instance().enqueue([x1, y1, x2, y2]() {
					RenderInnerDeferredTransparent(x1, y1, x2, y2);
				});
			}
		}
		{
			std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
			renderns::condition.wait(lock, []{ return renderns::tileCounter == numTilesX * numTilesY; });
		}

		// Per-pixel deferred lighting for the closest front-facing
		// transparent at each pixel. Reads xpr-G-buffer, computes
		// per-pixel Lambertian, alpha-blends 50/50 onto VPage.
		// Reuses the per-tile light list built by Render_DeferredLighting.
		renderns::tileCounter = 0;
		for (auto j = 0; j < numTilesY; ++j) {
			auto y1 = tileSizeY * j;
			auto y2 = std::min(y1 + tileSizeY, YRes);
			for (auto i = 0; i < numTilesX; ++i) {
				auto x1 = tileSizeX * i;
				auto x2 = std::min(x1 + tileSizeX, XRes);
				const int tileIdx = j * numTilesX + i;
				ThreadPool::instance().enqueue([x1, y1, x2, y2, tileIdx]() {
					Render_DeferredTransparentLighting_Tile(
						g_deferredCtx, tileIdx, x1, y1, x2, y2);
				});
			}
		}
		{
			std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
			renderns::condition.wait(lock, []{ return renderns::tileCounter == numTilesX * numTilesY; });
		}
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
