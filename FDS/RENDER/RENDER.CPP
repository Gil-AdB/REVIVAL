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



// Deferred path computes lighting per-pixel in Render_DeferredLighting;
// running per-vertex Lighting() too is wasted work whose output (Vtx->LR/
// LG/LB/LA) the deferred path doesn't consume. Skip when the env var is
// set. Forward path keeps running it as before.
extern bool deferredEnabled();

void Lighting(Scene *Sc)
{
	if (deferredEnabled()) return;

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
struct ViewLight {
	Vector  Pos;     // view-space
	Color   Col;     // (O->L * O->ISize) — material multiplier folded in per-pixel below
	float   Range, Range2, rRange;
};

// Per-frame setup shared across all tile jobs. Captured by reference
// in the lambdas; lifetime ends when Render() returns (after the
// tileCounter wait in Render_DeferredLighting).
struct DeferredLightingCtx {
	const meka::GBuffer *gb;
	MatTable             matTable;
	const ViewLight     *lights;
	int                  numLights;
	float                invFOVX;
	float                invFOVY;
	float                invZScale;
	Scene               *Sc;
	int                  waterMatID;  // -1 if no water blend
};

static void Render_DeferredLighting_Tile(const DeferredLightingCtx &ctx,
                                          int x1, int y1, int x2, int y2)
{
	const meka::GBuffer &gb = *ctx.gb;
	dword *out = reinterpret_cast<dword *>(VPage);

	for (int py = y1; py < y2; ++py) {
		for (int px = x1; px < x2; ++px) {
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
			const bool wantSpecular = (Mat->Specular > 0.0f) && (Specular_Factor > 0.0f);
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

			// Per-omni Lambertian + linear distance falloff. Same
			// k = dot * len_inv * (1 - dist*rRange) shape as forward
			// Lighting()'s `k = dot * len * (1 - rRange/len)` (where
			// len is rsqrt(len2) = 1/dist).
			for (int li = 0; !isWater && li < ctx.numLights; ++li) {
				const ViewLight &L = ctx.lights[li];
				const float wx = L.Pos.x - x;
				const float wy = L.Pos.y - y;
				const float wz = L.Pos.z - z;
				const float dot = wx * nx + wy * ny + wz * nz;
				if (dot < 0.0f) continue;
				const float len2 = wx*wx + wy*wy + wz*wz;
				if (len2 > L.Range2) continue;
				const float lenInv = 1.0f / std::sqrt(len2);
				const float dist   = 1.0f / lenInv;
				const float k = dot * lenInv * (1.0f - dist * L.rRange);
				const float intensity = k * Mat->Diffuse;
				lB += intensity * L.Col.B;
				lG += intensity * L.Col.G;
				lR += intensity * L.Col.R;

				// Blinn-Phong specular: H = normalize(L_dir + V_dir),
				// spec = max(0, n·H)^Glossiness * Specular * light * falloff.
				// Same falloff factor (1 - dist*rRange) so the highlight
				// fades with distance like the diffuse term.
				if (wantSpecular) {
					// L_dir = w / dist (already have lenInv = 1/dist)
					const float ldx = wx * lenInv;
					const float ldy = wy * lenInv;
					const float ldz = wz * lenInv;
					float hx = ldx + vx;
					float hy = ldy + vy;
					float hz = ldz + vz;
					const float hLen2 = hx*hx + hy*hy + hz*hz;
					if (hLen2 > 0.0f) {
						const float hLenInv = 1.0f / std::sqrt(hLen2);
						hx *= hLenInv; hy *= hLenInv; hz *= hLenInv;
						const float NdotH = nx*hx + ny*hy + nz*hz;
						if (NdotH > 0.0f) {
							const float spec = std::pow(NdotH, gloss);
							const float specStrength = spec * Mat->Specular *
								(1.0f - dist * L.rRange);
							sB += specStrength * L.Col.B;
							sG += specStrength * L.Col.G;
							sR += specStrength * L.Col.R;
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

			// Env-map sampling. When the material has an EnvTexture
			// (City windows post-clone), reconstruct the world-space
			// reflected ray, lat/lon project, sample the panorama, blend
			// with the diffuse term per Mat->Reflection. Same lat/lon
			// formula as the per-vertex EU/EV setup at RENDER.CPP:761
			// — just done per pixel here against the (oct-decoded) view-
			// space normal and (Z-reconstructed) view-space pixel pos.
			float envB = 0, envG = 0, envR = 0;
			float envBlend = 0.0f;
			if (Mat->EnvTexture && Mat->EnvTexture->Mipmap[0] && Mat->Reflection > 0.0f) {
				// Transform view-space pos and normal back into world space.
				// View->Mat is rotation-only; transpose = inverse-rotation.
				const float wpx = View->Mat[0][0]*x + View->Mat[1][0]*y + View->Mat[2][0]*z;
				const float wpy = View->Mat[0][1]*x + View->Mat[1][1]*y + View->Mat[2][1]*z;
				const float wpz = View->Mat[0][2]*x + View->Mat[1][2]*y + View->Mat[2][2]*z;
				const float wnx = View->Mat[0][0]*nx + View->Mat[1][0]*ny + View->Mat[2][0]*nz;
				const float wny = View->Mat[0][1]*nx + View->Mat[1][1]*ny + View->Mat[2][1]*nz;
				const float wnz = View->Mat[0][2]*nx + View->Mat[1][2]*ny + View->Mat[2][2]*nz;
				// Reflect view direction (camera->pixel = wp) around world normal.
				const float dotDN = wpx*wnx + wpy*wny + wpz*wnz;
				float drx = wpx - 2.0f*dotDN*wnx;
				float dry = wpy - 2.0f*dotDN*wny;
				float drz = wpz - 2.0f*dotDN*wnz;
				const float drLen2 = drx*drx + dry*dry + drz*drz;
				if (drLen2 > 0.0f) {
					const float drInv = 1.0f / std::sqrt(drLen2);
					drx *= drInv; dry *= drInv; drz *= drInv;
					// Lat/lon projection (matches RENDER.CPP:761-765).
					const float lat = std::asin(dry);
					const float lon = std::atan2(-drz, -drx);
					float eu = 0.5f + 0.5f * (lon + float(PI) * 0.5f) / float(PI);
					float ev = 0.5f - 0.5f * lat / (float(PI) * 0.5f);
					if (eu < 0) eu += 1.0f; else if (eu > 1) eu -= 1.0f;
					if (ev < 0) ev = 0; else if (ev > 1) ev = 1;
					Texture *ET = Mat->EnvTexture;
					int tx2 = int(eu * ET->SizeX) & (ET->SizeX - 1);
					int ty2 = int(ev * ET->SizeY) & (ET->SizeY - 1);
					// Sachletz layout: 4×4 blocks, blocks stored column-by-
					// column within rows of blocks (outer X, inner Y in
					// the swizzle loop).
					const int blocksY = ET->SizeY >> 2;
					const int bX = tx2 >> 2;
					const int bY = ty2 >> 2;
					const int blockIdx = bX * blocksY + bY;
					const int inBlock = (ty2 & 3) * 4 + (tx2 & 3);
					const dword *envData = (const dword *)ET->Mipmap[0];
					const dword envT = envData[blockIdx * 16 + inBlock];
					envB = float(envT & 0xFF);
					envG = float((envT >> 8) & 0xFF);
					envR = float((envT >> 16) & 0xFF);
					envBlend = Mat->Reflection;
					if (envBlend > 1.0f) envBlend = 1.0f;
				}
			}

			// Diffuse modulation. Two paths:
			//
			//   non-reflective: pixel = texel * light / 256
			//
			//   reflective:     pixel = saturate(env + tex/2) * light / 256
			//
			// The reflective branch matches the forward path's
			// TheOtherBarry<OVERWRITE, TEXTURETEXTURE> filler (FILLERS/
			// TheOtherBarry.h:377): texture0 = add_saturated(env_texel,
			// base_texel >> 1), then colorize() applies vertex light. The
			// previous LERP-with-Mat->Reflection blend left too much of
			// the lit base texel visible against forward's much darker
			// composite, which read as "City windows are too bright" when
			// the deferred path was sat next to a forward reference.
			float fdB, fdG, fdR;
			if (envBlend > 0.0f) {
				float compositeB = envB + texB * 0.5f;
				float compositeG = envG + texG * 0.5f;
				float compositeR = envR + texR * 0.5f;
				if (compositeB > 255.0f) compositeB = 255.0f;
				if (compositeG > 255.0f) compositeG = 255.0f;
				if (compositeR > 255.0f) compositeR = 255.0f;
				fdB = compositeB * lB * (1.0f / 256.0f);
				fdG = compositeG * lG * (1.0f / 256.0f);
				fdR = compositeR * lR * (1.0f / 256.0f);
			} else {
				fdB = (texB * lB) * (1.0f / 256.0f);
				fdG = (texG * lG) * (1.0f / 256.0f);
				fdR = (texR * lR) * (1.0f / 256.0f);
			}
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
	constexpr int MAX_LIGHTS = 128;
	static ViewLight lights[MAX_LIGHTS];  // function-static; lifetime spans dispatch + wait
	int numLights = 0;
	for (Omni *O = Sc->OmniHead; O && numLights < MAX_LIGHTS; O = O->Next) {
		if (!(O->Flags & Omni_Active)) continue;
		Vector u, w;
		Vector_Sub(&O->IPos, &View->ISource, &u);
		MatrixXVector(View->Mat, &u, &w);
		ViewLight &L = lights[numLights++];
		L.Pos    = w;
		L.Col.B  = O->L.B * O->ISize;
		L.Col.G  = O->L.G * O->ISize;
		L.Col.R  = O->L.R * O->ISize;
		L.Col.A  = O->L.A * O->ISize;
		L.Range  = O->IRange;
		L.Range2 = L.Range * L.Range;
		L.rRange = (L.Range > 0.0f) ? 1.0f / L.Range : 0.0f;
	}

	static DeferredLightingCtx ctx;  // function-static so the lambda captures by reference safely
	ctx.gb        = &gb;
	ctx.matTable  = matTable;
	ctx.lights    = lights;
	ctx.numLights = numLights;
	ctx.invFOVX    = 1.0f / FOVX;
	ctx.invFOVY    = 1.0f / FOVY;
	ctx.invZScale  = 1.0f / float(g_zscale);
	ctx.Sc         = Sc;
	ctx.waterMatID = g_deferredWaterMatID;

	constexpr int numTilesX = 6;
	constexpr int numTilesY = 4;
	const int tileSizeX = (XRes + (numTilesX - 1)) / numTilesX;
	const int tileSizeY = (YRes + (numTilesY - 1)) / numTilesY;

	renderns::tileCounter = 0;
	for (int j = 0; j < numTilesY; ++j) {
		const int y1 = tileSizeY * j;
		const int y2 = std::min(y1 + tileSizeY, YRes);
		for (int i = 0; i < numTilesX; ++i) {
			const int x1 = tileSizeX * i;
			const int x2 = std::min(x1 + tileSizeX, XRes);
			ThreadPool::instance().enqueue([x1, y1, x2, y2]() {
				Render_DeferredLighting_Tile(ctx, x1, y1, x2, y2);
			});
		}
	}
	{
		std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
		renderns::condition.wait(lock, []{
			return renderns::tileCounter == numTilesX * numTilesY;
		});
	}
}

void Render(bool forceForward)
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
	// forceForward bypasses the deferred dispatch — used by RenderSkyCube,
	// whose vertex-stored LR/LG/LB=255 brightness pass-through is what
	// the forward fillers consume; the deferred per-pixel lighting model
	// would re-shade those faces against Mat->Diffuse / Sc->Ambient and
	// (with sky-cube materials authoring neither) come out black.
	bool deferred = deferredEnabled() && !forceForward;
	if (deferred && (!g_gbuffer || g_gbuffer->normal.size() < size_t(XRes) * size_t(YRes))) {
		deferred = false;
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
