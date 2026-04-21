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

#ifdef _C_WATCOM
#include <i86.H>
#include <DOS.H>
#include <Time.H>
#endif

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
#ifdef PORTABLE_CODE
unsigned int OldFPUCW, FPUCW;
#else
unsigned short int OldFPUCW,FPUCW;
#endif

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
#ifdef _C_WATCOM
void DO_NOP();
#pragma aux DO_NOP="NOP";
#endif
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
	for (F=T->Faces;F<FEnd;F++)
		if ((!F->VisibilityFlagsAll())
			&&((F->Txtr->Flags&Mat_TwoSided)
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
				dz = F->A->TPos.z;
				if (F->B->TPos.z>dz) dz=F->B->TPos.z;
				if (F->C->TPos.z>dz) dz=F->C->TPos.z;
				if (dz > fzp)
					F->SortZ.F = fzp;
				else
					F->SortZ.F = 2*fzp - dz;

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



#if defined(__clang__) || defined(PORTABLE_CODE)

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

#else

void Lighting(Scene *Sc)
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

	for(T=Sc->TriMeshHead;T;T=T->Next)
	{
		if (T->Flags&(Tri_Invisible|Tri_Noshading)) continue;

		if (T->FIndex)
		{
			Mat = T->Faces[0].Txtr;
			Lumin = Mat->Luminosity;
		} else Lumin = 0.0;

		
		// Calculate ambient factor
		// LW3D: Luminosity * base color + scene Ambient * diffuse level
		if (Mat->Txtr)
		{
/*			Ambient.B = Lumin * 255.0 + Mat->Diffuse * Sc->Ambient.B;
			Ambient.G = Lumin * 255.0 + Mat->Diffuse * Sc->Ambient.G;
			Ambient.R = Lumin * 255.0 + Mat->Diffuse * Sc->Ambient.R;
			Ambient.A = Lumin * 255.0 + Mat->Diffuse * Sc->Ambient.A;/**/
			__asm
			{
				// xmm0 = resulting Ambient

				mov		edi,		[Sc]
				movaps	xmm0,		[edi]Sc.Ambient
									
				mov		edi,		[Mat]
				movss	xmm1,		[edi]Mat.Diffuse
									
				movss	xmm2,		[Lumin]
				shufps	xmm2,		xmm2,	0
				shufps	xmm1,		xmm1,	0
				movaps	xmm3,		[pf255]
				mulps	xmm0,		xmm1
				mulps	xmm2,		xmm3
				addps	xmm0,		xmm2
				movups  [Ambient],  xmm0
			}/**/
		} else {
			/*Ambient.B = Lumin * Mat->BaseCol.B + Mat->Diffuse * Sc->Ambient.B;
			Ambient.G = Lumin * Mat->BaseCol.G + Mat->Diffuse * Sc->Ambient.G;
			Ambient.R = Lumin * Mat->BaseCol.R + Mat->Diffuse * Sc->Ambient.R;
			Ambient.A = Lumin * Mat->BaseCol.A + Mat->Diffuse * Sc->Ambient.A;/**/
			__asm
			{
				// xmm0 = resulting ambient
				// xmm1 = Lumin

				mov		edi,		[Sc]
				movaps	xmm0,		[edi]Sc.Ambient
									
				mov		edi,		[Mat]
				movss	xmm1,		[edi]Mat.Diffuse
									
				movss	xmm2,		[Lumin]
				shufps	xmm1,		xmm1,	0
				shufps	xmm2,		xmm2,	0
				// remember that xmm4 contains Mat->BaseCol
				movaps	xmm4,		[edi]Mat.BaseCol
				mulps	xmm0,		xmm1
				mulps	xmm2,		xmm4
				addps	xmm0,		xmm2
				movups  [Ambient],  xmm0

			}


		}

		for(O=Sc->OmniHead, L=LA;O;O=O->Next)
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
			__asm
			{
				movss	xmm1,		[rScale]
				rcpss	xmm1,		xmm1
				movss	[rScale],	xmm1
			}
			Vector_SelfScale(&w, rScale*rScale);
			Vector_Copy(&L->Pos, &w);		

			L->Range = O->IRange * rScale;
			L->Range2 = L->Range * L->Range;
			//L->rRange = 1.0/L->Range;
			__asm
			{
				mov		edi,		[L]
				movss	xmm1,		[edi]L.Range
				rcpss	xmm1,		xmm1
				movss	[edi]L.rRange,	xmm1
			}

			// bounding sphere check
			Vector_Sub(&L->Pos, &T->BSphereCtr, &u);
			if (T->BSphereRadius + L->Range < Vector_Length(&u))
				continue;
			
			if (Mat->Txtr)
			{
/*				float intensity = O->ISize * Mat->Diffuse;
				L->Col.B = O->L.B * intensity;
				L->Col.G = O->L.G * intensity;
				L->Col.R = O->L.R * intensity;
				L->Col.A = O->L.A * intensity;*/
				__asm
				{
					// xmm1 = intensity
					mov		edi,	[Mat]
					movss	xmm2,	[edi]Mat.Diffuse
					mov		edi,	[O]
					movss	xmm1,	[edi]O.ISize
					mulss	xmm2,	xmm1
					shufps	xmm2,	xmm2,	0
					movaps	xmm1,	[edi]O.L
					// xmm1 = O->L * intensity
					mulps	xmm1,	xmm2
					mov		edi,	[L]
					movaps	[edi]L.Col,	xmm1
				}
			} else {
/*				float intensity = O->ISize * Mat->Diffuse / 256.0;
				L->Col.B = O->L.B * Mat->BaseCol.B * intensity;
				L->Col.G = O->L.G * Mat->BaseCol.G * intensity;
				L->Col.R = O->L.R * Mat->BaseCol.R * intensity;
				L->Col.A = O->L.A * Mat->BaseCol.A * intensity;*/
				__asm
				{
					// remember that xmm4 contains Mat->BaseCol
					// xmm1 = intensity
					movss	xmm3,	[pfr256]
					mov		edi,	[Mat]
					movss	xmm2,	[edi] Mat.Diffuse
					mov		edi,	[O]
					mulss	xmm2,	xmm3
					movss	xmm1,	[edi]O.ISize
					mulss	xmm2,	xmm1
					shufps	xmm2,	xmm2,	0
					// xmm1 = O->L * intensity(xmm1) * Mat->BaseCol(xmm4)
					movaps	xmm1,	[edi]O.L
					mulps	xmm1,	xmm4
					mulps	xmm1,	xmm2
					mov		edi,	[L]
					movaps	[edi]L.Col,	xmm1
				}

			}
			L++;
		}
		LE = L;

		bool stat = false;
		if (T->Flags & Tri_Stationary)
			stat = true;

		Color *sl = T->SL;
		for(V=T->Verts, VE = V+T->VIndex; V<VE; V++, sl++)
		{
			// this compare isn't so bad as soon as it gets into the BTB.
			if (stat)
			{
				__asm
				{
					// xmm0 is the ambient (na'al baiet)
					// l = xmm1
					mov		edi,	[sl]
					movaps	xmm1,	[edi]
				}
/*				l->B = sl->B;
				l->G = sl->G;
				l->R = sl->R;
				l->A = sl->A;*/
			} else {
/*				l->B = Ambient.B;
				l->G = Ambient.G;
				l->R = Ambient.R;
				l->A = Ambient.A;*/
				__asm
				{
//					movaps	xmm1,	xmm0
					movups  xmm1,  [Ambient]

				}
			}

			__asm
			{
				mov		edi, [pl]
				movaps  [edi], xmm1
			}


			for(L=LA; L<LE; L++)
			{
				Vector_Sub(&L->Pos, &V->Pos, &w);
				float dot = Dot_Product(&w, &V->N);
				if (dot < 0.0) continue;
				float v = Vector_Length(&w);
				float len2 = w.x*w.x+w.y*w.y+w.z*w.z;
				if (len2 > L->Range2) continue;
/*				float len = SQRT(len2);
				float k = dot / len * (1.0-len*L->rRange);

/*				float len = RSQRT(len2);
				float k = dot * len * (1.0-L->rRange/len);/**/
/*				l.B += k*L->Col.B;
				l.G += k*L->Col.G;
				l.R += k*L->Col.R;
				l.A += k*L->Col.A;/**/
				__asm
				{
					// xmm2 = 1.0 / sqrt(len2)
					// xmm3 = sqrt(len2)
					movss	xmm2,	[len2]
					rsqrtss xmm2,	xmm2		// xmm2 = len
					mov		edi,	[L]
					movss	xmm5,	[edi]L.rRange
					rcpss	xmm3,	xmm2		// xmm3	= 1.0 / len
					movss	xmm4,	[pf1]		// xmm4 = 1.0
					mulss	xmm3,	xmm5		// xmm3 = L->rRange/len
					movss	xmm5,	[dot]
					subss	xmm4,	xmm3		// xmm4 = (1.0-L->rRange/len)
					movaps	xmm3,	[edi]L.Col
					mulss	xmm2,	xmm4		// xmm2 = len * (1.0-L->rRange/len)
					mulss	xmm2,	xmm5		// xmm2 = dot * len * (1.0-L->rRange/len)
					shufps	xmm2,	xmm2,	0	// xmm2 = k,k,k,k
					mulps	xmm2,	xmm3		// xmm2 = k*L->Col
					mov		edi,	[pl]
					movaps  xmm1,	[edi]
					addps	xmm1,	xmm2		// l += k * L->Col
					movaps  [edi],	xmm1
				}

			}
/*			__asm
			{
				mov		edi,	[pl]
				movaps  [edi],	xmm1
			}

			// saturation
			if (pl->B > 250.0) pl->B = 250.0;
			if (pl->G > 250.0) pl->G = 250.0;
			if (pl->R > 250.0) pl->R = 250.0;
			if (pl->A > 250.0) pl->A = 250.0;
			
			V->LB = pl->B;
			V->LG = pl->G;
			V->LR = pl->R;
			V->LA = pl->A;*/
			__asm
			{
				movaps		xmm7,	[pfMaxLight]
				mov			edi,	[pl]
				movaps		xmm1,	[edi]
				mov			edi,	[V]
				minps		xmm1,	xmm7
				cvtps2pi	mm0,	xmm1
				movhlps		xmm1,	xmm1
				cvtps2pi	mm1,	xmm1
				packssdw	mm0,	mm1
				packuswb	mm0,	mm0
				movd		[edi],	mm0
				emms
			}
		}
	}
}
#endif

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
			RasterFunc filler;
			clipper.Render(F, Mekalele, false);
			//if (F->Flags & Face_Reflective) {
			//	clipper.Render(F, IX_Prefiller_Reflective, true);
			//}
		}
	}

	std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
	++renderns::tileCounter;
	renderns::condition.notify_one();
}

void Render()
{
	constexpr auto		numTilesX = 6;
	constexpr auto		numTilesY = 4;
	const auto			tileSizeX = (XRes + (numTilesX - 1)) / numTilesX;
	const auto			tileSizeY = (YRes + (numTilesY - 1)) / numTilesY;

	renderns::tileCounter = 0;

	for (auto j = 0; j < numTilesY; ++j) {
		auto y1 = tileSizeY * j;
		auto y2 = std::min(y1 + tileSizeY, YRes);
		for (auto i = 0; i < numTilesX; ++i) {
			auto x1 = tileSizeX * i;
			auto x2 = std::min(x1 + tileSizeX, XRes);

			ThreadPool::instance().enqueue([x1, y1, x2, y2]() { RenderInner(x1, y1, x2, y2); });
			// RenderInner(x1, y1, x2, y2);
		}
	}

	{
		std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
		renderns::condition.wait(lock, [] {return renderns::tileCounter == numTilesX * numTilesY; });
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
	// Force LINEAR palette
#ifdef _C_WATCOM
	if (BPP==8)
	{
		char *Palbuf = new char[768],*Pal = Palbuf;
		for(I=0;I<256;I++)
		{
			//			printf("color %d (%d,%d,%d)\n",I,Sc->Pal->C[I].R,Sc->Pal->C[I].G,Sc->Pal->C[I].B);
			*Pal++ = Sc->Pal->C[I].R;
			*Pal++ = Sc->Pal->C[I].G;
			*Pal++ = Sc->Pal->C[I].B;
		}
		Set_Palette(Palbuf);
		delete Palbuf;
	}
#endif
	
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
#ifdef _C_WATCOM
		Radix_Asm(FList,SList,CAll);
#else
		Radix_Sorting();
#endif
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
			 Y = OutTextXY(VPage,0,Y+15,MsgStr[I],64);
			 if (Timer>MsgClock[I]+150)
			 {
				 delete MsgStr[I];
				 MsgStr[I]=NULL;
			 }
		 }
#ifdef RUNTIME_INFO_L1
		 sprintf(Str,"Time = %d , Frame = %5.1f",Timer.load(), CurFrame);
		 Y = OutTextXY(VPage,0,Y+15,Str,64);
		 sprintf(Str,"Inst. FPS = %3.2f (%3.2f)",FPS,100.0*(float)Frames/(float)Timer);
		 Y = OutTextXY(VPage,0,Y+15,Str,64);
#endif
#ifdef RUNTIME_INFO_L2
		 //    sprintf(Str,"Active Camera = %s",View->Name);
		 //    Y = OutTextXY(VPage,0,Y+15,Str,64);
		 //    if (Tot)
		 //		{
		 //			sprintf(Str,"Perf. Distribution: xForm=%d%%,P.Knmtx=%d%%,FList=%d%%,Light=%d%%,\nRender=%d%%",(Par1-TTrd)*100/Tot,(Par2-Par1)*100/Tot,(Par3-Par2)*100/Tot,(Par4-Par3)*100/Tot,(Par5-Par4)*100/Tot);
		 //			Y = OutTextXY(VPage,0,Y+15,Str,64);
		 //		}
		 //    sprintf(Str,"Dyn. Kinematics: p(%4.2f,%4.2f,%4.2f),v(%1.4f,%1.4f,%1.4f) r(%1.4f,%1.4f,%1.4f)",FC.ISource.x,FC.ISource.y,FC.ISource.z,FV.x,FV.y,FV.z,FT.x,FT.y,FT.z);
		 //    Y = OutTextXY(VPage,0,Y+15,Str,64);
		 sprintf(Str,"Average Polys/Frame: %5.1f (%3.1f%%)",(float)RenderedPolys/(float)Frames,100.0*(float)CPolys/(float)Polys);
		 Y = OutTextXY(VPage,0,Y+15,Str,64);
		 int32_t GTP = 0,GTPP = 0;
		 for(TriMesh *Tri = Sc->TriMeshHead;Tri;Tri = Tri->Next)
			 if (!(Tri->Flags&Tri_Invisible)) {GTP++; GTPP += Tri->FIndex;}
			 sprintf(Str,"%d Triangle Meshes totalling in %d Faces",GTP,GTPP);
			 Y = OutTextXY(VPage,0,Y+15,Str,64);
#endif
#ifdef RUNTIME_INFO_L3
			 sprintf(Str,"Rendered %d Polygons,%d Omnilights and %d particles.\n",CPolys,COmnies,CPcls);
			 Y = OutTextXY(VPage,0,Y+15,Str,64);
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
	/*  sprintf(Str,"Inst. FPS = %3.2f (%3.2f)",FPGS,100.0*(float)Frames/(float)Par4);
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
