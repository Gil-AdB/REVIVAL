#include "Rev.h"
#include <Base/Object.h>
#include <Base/TriMesh.h>
#include <Base/Scene.h>

#include "CHASE.H"

#define FRONT_TO_BACK_SORTING
static Scene *ChaseSc;

static int32_t CHPartTime = 25.0f*100;

static Object *RflObj;
static Vector RflSurfNorm(0, 1, 0);
static float RflSurfOfs = 0;

static DWord WaterBuf[65536 * 4];

static void Setup_Water_Distort()
{
	TriMesh *T = (TriMesh *)RflObj->Data;
	memcpy(WaterBuf, (DWord *)(T->Faces->Txtr->Txtr->Data), 65536 * 4);//WaterTexture.Data;
}

static void Reflective_Mapper_Setup()
{
	TriMesh *T;
	Vertex *Vtx, *VE;
	Face *F, *FE;
	Vector U, V;

	Object *O;
	int32_t IID = 0;
	for (O = ChaseSc->ObjectHead; O; O = O->Next)
	{
		if (O->Type != Obj_TriMesh) continue;
		//		printf("[%d] Trimesh %s with %d Polygons\n",IID,O->Name,((TriMesh *)O->Data)->FIndex);
		if (!strcmp(O->Name, "water.lwo"))
		{
			RflObj = O;
			T = (TriMesh *)(O->Data);

			F = T->Faces;
			FE = F + T->FIndex;

			// should write a transparent texture/gouraud to enable fog.
//			break;
			for (; F < FE; F++)
			{
				//				F->Filler = IX_Prefiller_TGAcZ;
				//				F->Txtr->Luminosity = 8.0;
								//F->Filler = The_BiMMXTTrue_Fogged;
				//				F->Clipper = &Frust_Set_UVL;
			}
			break;
		}
	}
}


///////////////////////////////////////////////////////////////////////
// PRIMARY TRANSFORMATION PIPELINE, FOR USE WITH REFLECTIONS
static void Reflected_Transform(Scene *Sc)
{
	TriMesh *T;
	Omni *O;
	Matrix M, IM, ReflMat;
	float M34[3][4];
	Vector AP, S, P, U, V, *W = (Vector *)M, *W2 = (Vector *)IM, *Scl;
	float L1, L2, L3;
	Vertex *Vtx, *VEnd;
	Face *F, *FEnd;
	float PX = FOVX, PY = FOVY, Temp;
	float dz;
	int32_t *pdz = (int32_t *)(&dz);
	int32_t I;
	Face **Ins = FList;
	float *f = (float *)(&M);
	float *fv;

	float fzp = CurScene->FZP;

	TriMesh *TR;

	TR = (TriMesh *)(RflObj->Data);

	//for (T=Sc->TriMeshHead;T;T=T->Next)
	Object *Obj;
	for (Obj = Sc->ObjectHead; Obj; Obj = Obj->Next)
	{

		if (Obj->Type != Obj_TriMesh) continue;
		//if (stricmp(Obj->Name, "Ship1.lwo")) continue;
		T = (TriMesh *)(Obj->Data);

		if (T == TR) { T->Flags |= Tri_Invisible; continue; }

		if (!(T->Flags&HTrack_Visible)) { T->Flags |= Tri_Invisible; continue; }

		Matrix_Copy(ReflMat, T->RotMat);
		//Vector_SelfScale(W2+1,-1.0f);
		ReflMat[0][1] *= -1.0f; ReflMat[1][1] *= -1.0f; ReflMat[2][1] *= -1.0f;
		MatrixXMatrix(View->Mat, ReflMat, M);
		Matrix_Copy(IM, M);
		// Advanced Matrix...(watch this)
		Vector_Scale(W, PX, W);
		Vector_Scale(W + 1, -PY, W + 1);
		Vector_Scale(W + 2, CntrEX, &V);
		Vector_SelfAdd(W, &V);
		Vector_Scale(W + 2, CntrEY, &V);
		Vector_SelfAdd(W + 1, &V);
		// Supermatrix ready.

		// postrioric Offset Vector.
		// mirror T->ipos around first
		L1 = (-2.0f)*Dot_Product(&T->IPos, &RflSurfNorm);
		Vector_Scale(&RflSurfNorm, L1, &P);
		Vector_SelfAdd(&P, &T->IPos);

		Vector_Sub(&P, &View->ISource, &U);
		MatrixXVector(View->Mat, &U, &S);
		V.x = CntrEX * S.z + PX * S.x;
		V.y = CntrEY * S.z - PY * S.y;
		V.z = S.z;
		// make a corrected sphere center vector
		MatrixXVector(IM, &T->BSphereCtr, &AP);
		Vector_SelfAdd(&S, &AP);

		//    Vector_Copy(&V,&S);
		// ready
		// 4x3 AFFINE XFORM
		M34[0][0] = M[0][0]; M34[0][1] = M[0][1]; M34[0][2] = M[0][2]; M34[0][3] = V.x;
		M34[1][0] = M[1][0]; M34[1][1] = M[1][1]; M34[1][2] = M[1][2]; M34[1][3] = V.y;
		M34[2][0] = M[2][0]; M34[2][1] = M[2][1]; M34[2][2] = M[2][2]; M34[2][3] = V.z;
		// ready


		// aprioric Offset Vector.
		MatrixTXVector(ReflMat, &U, &AP);
		Vector *WP = (Vector *)ReflMat;
		Vector_SelfScale(&AP, 1.0 / Vector_SelfDot(WP));
		// ready
		// Bounding Sphere Elimination test Begins.
		W2 = (Vector *)(&T->RotMat);
		L2 = Dot_Product(W2, W2);
		if ((L1 = Dot_Product(W2 + 1, W2 + 1)) > L2) L2 = L1;
		if ((L1 = Dot_Product(W2 + 2, W2 + 2)) > L2) L2 = L1;

		T->Flags &= 0xFFFFFFFF - Tri_Invisible - Tri_Ahead - Tri_Inside;
		T->Flags |= Tri_Inside;

		// Out by depth
		dz = S.z - Sc->NZP;
		if (dz*dz > L2*T->BSphereRad)
		{
			if (dz < 0.0f)
			{
				T->Flags |= Tri_Invisible;
				continue;
			}
			T->Flags |= Tri_Ahead;
		}
		else {
			T->Flags &= ~Tri_Inside;
		}

		dz = S.z - Sc->FZP;
		if (dz*dz > L2*T->BSphereRad)
		{
			if (dz > 0.0f)
			{
				T->Flags |= Tri_Invisible;
				continue;
			}
		}
		else {
			T->Flags &= ~Tri_Inside;
		}
		// Out by left/right
		S.x = fabs(S.x);
		L1 = PX * S.x - CntrEX * S.z;
		if (L1*L1 > L2*T->BSphereRad*(PX*PX + CntrEX * CntrEX))
		{
			if (S.x*PX > S.z*CntrEX)
			{
				T->Flags |= Tri_Invisible;
				continue;
			}
		}
		else {
			if (T->Flags&Tri_Ahead) T->Flags &= ~Tri_Inside;
		}
		// Out by up/down
		S.y = fabs(S.y);
		L1 = PY * S.y - CntrEY * S.z;
		if (L1*L1 > L2*T->BSphereRad*(PY*PY + CntrEY * CntrEY))
		{
			if (S.y*PY > S.z*CntrEY)
			{
				T->Flags |= Tri_Invisible;
				continue;
			}
		}
		else {
			if (T->Flags&Tri_Ahead) T->Flags &= ~Tri_Inside;
		}

		VEnd = T->Verts + T->VIndex;

		/*    FEnd=T->Face+T->NumOfFaces;
		for (F=T->Face;F<FEnd;F++)
		if (!(F->Txtr->Flags&Mat_TwoSided))
		F->Flags = (AP.x*F->N.x + AP.y*F->N.y + AP.z*F->N.z>=F->NormProd);*/


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
			for (Vtx = T->Verts; Vtx < VEnd; Vtx++)
			{
				//        MatrixXVector(M,&Vtx->Pos,&U);
				//        Vector_Add(&U,&V,&Vtx->TPos);
				// 4x3 xform
				Vtx->TPos.x = M34[0][0] * Vtx->Pos.x + M34[0][1] * Vtx->Pos.y + M34[0][2] * Vtx->Pos.z + M34[0][3];
				Vtx->TPos.y = M34[1][0] * Vtx->Pos.x + M34[1][1] * Vtx->Pos.y + M34[1][2] * Vtx->Pos.z + M34[1][3];
				Vtx->TPos.z = M34[2][0] * Vtx->Pos.x + M34[2][1] * Vtx->Pos.y + M34[2][2] * Vtx->Pos.z + M34[2][3];

				Vtx->Flags &= 0xFFFFFFFF - Vtx_Visible;
				Vtx->RZ = 1.0 / Vtx->TPos.z;
				Vtx->PX = Vtx->TPos.x*Vtx->RZ;
				Vtx->PY = Vtx->TPos.y*Vtx->RZ;
				//        Vtx->PX=CntrEX+PX*Vtx->TPos.x*Vtx->RZ;
				//        Vtx->PY=CntrEY-PY*Vtx->TPos.y*Vtx->RZ;
				//        Vtx->RU=Vtx->U*Vtx->RZ;
				//        Vtx->RV=Vtx->V*Vtx->RZ;
				//if (Vtx->TPos.z>Sc->FZP) Vtx->Flags|=Vtx_VisFar;
			}

			goto OUT;
			// This is in case 100% of trimesh AHEAD of camera. this saves some chks
		Ahead://Vertex_Loop1(T->Vertex,VEnd,M,&V);
			for (Vtx = T->Verts; Vtx < VEnd; Vtx++)
			{
				//    if (!Vtx->FRem) continue;
				//        MatrixXVector(M,&Vtx->Pos,&U);
				//        Vector_Add(&U,&V,&Vtx->TPos);
				Vtx->TPos.x = M34[0][0] * Vtx->Pos.x + M34[0][1] * Vtx->Pos.y + M34[0][2] * Vtx->Pos.z + M34[0][3];
				Vtx->TPos.y = M34[1][0] * Vtx->Pos.x + M34[1][1] * Vtx->Pos.y + M34[1][2] * Vtx->Pos.z + M34[1][3];
				Vtx->TPos.z = M34[2][0] * Vtx->Pos.x + M34[2][1] * Vtx->Pos.y + M34[2][2] * Vtx->Pos.z + M34[2][3];

				Vtx->RZ = 1.0 / Vtx->TPos.z;
				Vtx->PX = Vtx->TPos.x*Vtx->RZ;
				Vtx->PY = Vtx->TPos.y*Vtx->RZ;
				//        Vtx->PX=CntrEX+PX*Vtx->TPos.x*Vtx->RZ;
				//        Vtx->PY=CntrEY-PY*Vtx->TPos.y*Vtx->RZ;
				//        Vtx->RU=Vtx->U*Vtx->RZ;
				//        Vtx->RV=Vtx->V*Vtx->RZ;
				Vtx->Flags &= 0xFFFFFFFF - Vtx_Visible;
				if (Vtx->PX < 0) Vtx->Flags |= Vtx_VisLeft;
				if (Vtx->PX >= XRes) Vtx->Flags |= Vtx_VisRight;
				if (Vtx->PY < 0) Vtx->Flags |= Vtx_VisUp;
				if (Vtx->PY >= YRes) Vtx->Flags |= Vtx_VisDown;
				if (Vtx->TPos.z > Sc->FZP) Vtx->Flags |= Vtx_VisFar;
			}
			//    printf("Ahead VGA/Wizard.\n");
			goto OUT;
		Regular:
			for (Vtx = T->Verts; Vtx < VEnd; Vtx++)
			{
				//    if (!Vtx->FRem) continue;
				//        MatrixXVector(M,&Vtx->Pos,&U);
				//        Vector_Add(&U,&V,&Vtx->TPos);
				Vtx->TPos.x = M34[0][0] * Vtx->Pos.x + M34[0][1] * Vtx->Pos.y + M34[0][2] * Vtx->Pos.z + M34[0][3];
				Vtx->TPos.y = M34[1][0] * Vtx->Pos.x + M34[1][1] * Vtx->Pos.y + M34[1][2] * Vtx->Pos.z + M34[1][3];
				Vtx->TPos.z = M34[2][0] * Vtx->Pos.x + M34[2][1] * Vtx->Pos.y + M34[2][2] * Vtx->Pos.z + M34[2][3];

				Vtx->Flags &= 0xFFFFFFFF - Vtx_Visible;
				//      if (*(int32_t *)(&Vtx->TPos.z)>0x3F800000) // 1.0 in floating point rep.
				if (Vtx->TPos.z > Sc->NZP)
				{
					Vtx->RZ = 1.0 / Vtx->TPos.z;
					Vtx->PX = Vtx->TPos.x*Vtx->RZ;
					Vtx->PY = Vtx->TPos.y*Vtx->RZ;
					//          Vtx->PX=CntrEX+PX*Vtx->TPos.x*Vtx->RZ;
					//          Vtx->PY=CntrEY-PY*Vtx->TPos.y*Vtx->RZ;
					//          Vtx->RU=Vtx->U*Vtx->RZ;
					//          Vtx->RV=Vtx->V*Vtx->RZ;
					if (Vtx->PX < 0) Vtx->Flags |= Vtx_VisLeft;
					if (Vtx->PX >= XRes) Vtx->Flags |= Vtx_VisRight;
					if (Vtx->PY < 0) Vtx->Flags |= Vtx_VisUp;
					if (Vtx->PY >= YRes) Vtx->Flags |= Vtx_VisDown;
					if (Vtx->TPos.z > Sc->FZP) Vtx->Flags |= Vtx_VisFar;
				}
				else Vtx->Flags |= Vtx_VisNear;

				//      printf("Regular shit!\n");
			}
		}
		else {
			// instead of all of these complications, I've decided to
			// make the face have void (*Clipper), that will do whatever it needs
			// in one call. the pre-filler will call the asm rasterizers twice
			// if necessary. back to the good old Avatar engine techniques ;)
			// at this section, the code also calculates environment mapping
			// coordinates to (EU,EV) by rotating the v. normals accordingly.
			if (!(T->Flags&Tri_Inside))
			{
				if (!(T->Flags&Tri_Ahead))
					goto ERegular;
				else goto EAhead;
			}
			// Intel inside...this rulez,all object completely inside frustrum.
			for (Vtx = T->Verts; Vtx < VEnd; Vtx++)
			{
				MatrixXVector(M, &Vtx->Pos, &U);
				Vector_Add(&U, &V, &Vtx->TPos);

				Vtx->Flags = 0;
				Vtx->RZ = 1.0 / Vtx->TPos.z;
				Vtx->PX = Vtx->TPos.x*Vtx->RZ;
				Vtx->PY = Vtx->TPos.y*Vtx->RZ;
				//        Vtx->PX=CntrEX+PX*Vtx->TPos.x*Vtx->RZ;
				//        Vtx->PY=CntrEY-PY*Vtx->TPos.y*Vtx->RZ;
				// Environment mapping support removed at 11.04.02
//				Vtx->EU=128.0+95.0*(Vtx->N.x*IM[0][0]+Vtx->N.y*IM[0][1]+Vtx->N.z*IM[0][2]);
//				Vtx->REU=Vtx->EU*Vtx->RZ;
//				Vtx->EV=128.0+95.0*(Vtx->N.x*IM[1][0]+Vtx->N.y*IM[1][1]+Vtx->N.z*IM[1][2]);
//				Vtx->REV=Vtx->EV*Vtx->RZ;
				//        Vtx->RU=Vtx->U*Vtx->RZ;
				//        Vtx->RV=Vtx->V*Vtx->RZ;
			}
			goto OUT;
			// This is in case 100% of trimesh AHEAD of camera. this saves some chks
		EAhead://Vertex_Loop1(T->Vertex,VEnd,M,&V);
			for (Vtx = T->Verts; Vtx < VEnd; Vtx++)
			{
				//    if (!Vtx->FRem) continue;
				MatrixXVector(M, &Vtx->Pos, &U);
				Vector_Add(&U, &V, &Vtx->TPos);

				Vtx->RZ = 1.0 / Vtx->TPos.z;

				// Environment mapping support removed at 11.04.02
//				Vtx->EU=128.0+95.0*(Vtx->N.x*IM[0][0]+Vtx->N.y*IM[0][1]+Vtx->N.z*IM[0][2]);
//				Vtx->REU=Vtx->EU*Vtx->RZ;
//				Vtx->EV=128.0+95.0*(Vtx->N.x*IM[1][0]+Vtx->N.y*IM[1][1]+Vtx->N.z*IM[1][2]);
//				Vtx->REV=Vtx->EV*Vtx->RZ;

				Vtx->PX = Vtx->TPos.x*Vtx->RZ;
				Vtx->PY = Vtx->TPos.y*Vtx->RZ;
				//        Vtx->PX=CntrEX+PX*Vtx->TPos.x*Vtx->RZ;
				//        Vtx->PY=CntrEY-PY*Vtx->TPos.y*Vtx->RZ;
				//        Vtx->RU=Vtx->U*Vtx->RZ;
				//        Vtx->RV=Vtx->V*Vtx->RZ;
				if (Vtx->PX < 0) Vtx->Flags = Vtx_VisLeft; else Vtx->Flags = 0;
				if (Vtx->PX >= XRes) Vtx->Flags += Vtx_VisRight;
				if (Vtx->PY < 0) Vtx->Flags += Vtx_VisUp;
				if (Vtx->PY >= YRes) Vtx->Flags += Vtx_VisDown;
			}
			//    printf("Ahead VGA/Wizard.\n");
			goto OUT;
		ERegular:
			for (Vtx = T->Verts; Vtx < VEnd; Vtx++)
			{
				//    if (!Vtx->FRem) continue;
				MatrixXVector(M, &Vtx->Pos, &U);
				Vector_Add(&U, &V, &Vtx->TPos);

				Vtx->Flags = 0;
				//      if (*(int32_t *)(&Vtx->TPos.z)>0x3F800000) // 1.0 in floating point rep.
				// Environment mapping support removed at 11.04.02
//				Vtx->EU=128.0+127.0*(Vtx->N.x*IM[0][0]+Vtx->N.y*IM[0][1]+Vtx->N.z*IM[0][2]);
//				Vtx->EV=128.0+127.0*(Vtx->N.x*IM[1][0]+Vtx->N.y*IM[1][1]+Vtx->N.z*IM[1][2]);

				if (Vtx->TPos.z > Sc->NZP)
				{
					Vtx->RZ = 1.0 / Vtx->TPos.z;
					Vtx->PX = Vtx->TPos.x*Vtx->RZ;
					Vtx->PY = Vtx->TPos.y*Vtx->RZ;
					//          Vtx->PX=CntrEX+PX*Vtx->TPos.x*Vtx->RZ;
					//          Vtx->PY=CntrEY-PY*Vtx->TPos.y*Vtx->RZ;
					//          Vtx->RU=Vtx->U*Vtx->RZ;
					//          Vtx->RV=Vtx->V*Vtx->RZ;
//					Vtx->REU=Vtx->EU*Vtx->RZ;
//					Vtx->REV=Vtx->EV*Vtx->RZ;
					if (Vtx->PX < 0) Vtx->Flags = Vtx_VisLeft;
					if (Vtx->PX >= XRes) Vtx->Flags += Vtx_VisRight;
					if (Vtx->PY < 0) Vtx->Flags += Vtx_VisUp;
					if (Vtx->PY > YRes_1) Vtx->Flags += Vtx_VisDown;
				}
				else Vtx->Flags = Vtx_VisNear;
				//      printf("Regular shit!\n");
			}

		}
	OUT:FEnd = T->Faces + T->FIndex;
		for (F = T->Faces; F < FEnd; F++)
			if ((!(F->A->Flags&F->B->Flags&F->C->Flags&Vtx_Visible))
				&& ((F->Txtr->Flags&Mat_TwoSided) ||
					//(1)
				(AP.x*F->N.x + AP.y*F->N.y + AP.z*F->N.z < F->NormProd) //backface culling

					))
			{
				*Ins++ = F;
				dz = F->A->TPos.z;
				if (F->B->TPos.z > dz) dz = F->B->TPos.z;
				if (F->C->TPos.z > dz) dz = F->C->TPos.z;
#ifdef FRONT_TO_BACK_SORTING
				F->SortZ.F = dz;
#else
				F->SortZ.F = fzp - dz;
#endif
			}
	}
	CPolys = Ins - FList;
	for (O = Sc->OmniHead; O; O = O->Next)
	{
		Vtx = &O->V;
		Vector ReflectedPos = O->IPos;
		ReflectedPos.y = -ReflectedPos.y;
		Vector_Sub(&ReflectedPos, &View->ISource, &V);
		MatrixXVector(View->Mat, &V, &Vtx->TPos);
		if (Vtx->TPos.z > Sc->NZP&&Vtx->TPos.z < Sc->FZP)
		{
			Vtx->RZ = 1.0 / Vtx->TPos.z;
			Vtx->PX = CntrEX + Vtx->TPos.x*PX*Vtx->RZ;
			Vtx->PY = CntrEY - Vtx->TPos.y*PY*Vtx->RZ;
			// Insert to List
			dz = Vtx->TPos.z;
			//dz *=-16384;
			//dz +=0x7FFFFFFF;
			//RoundToInt(&O->Face.SortZ.DW,dz);
			*Ins++ = &O->F; // no insertion : flares are not reflected for now

			O->F.FlareSize = O->ISize;
#ifdef FRONT_TO_BACK_SORTING
			O->F.SortZ.F = 2 * fzp - dz;
#else
			O->F.SortZ.F = fzp - dz;
#endif			
			*Ins++ = &O->F;
		}
	}
	COmnies = (Ins - FList) - CPolys;
	for (I = 0; I < Sc->NumOfParticles; I++)
	{
		if (Sc->Pcl[I].Flags&Particle_Active)
		{
			if ((dz = Sc->Pcl[I].V.TPos.z) >= Sc->NZP) {
				F = &Sc->Pcl[I].F;
				//dz *=-16384;
				//dz +=0x7FFFFFFF;
				//RoundToInt(&F->SortZ.DW,dz);
#ifdef FRONT_TO_BACK_SORTING
				F->SortZ.F = 2 * fzp - dz;
#else
				F->SortZ.F = fzp - dz;
#endif
				//				*Ins++ = F;
			}
		}
	}
	CAll = Ins - FList;
	CPcls = CAll - COmnies - CPolys;
}


// This function is called to alter parameters within scene objects after loading from FLD.
static void SceneCorrections()
{
	// a crazy hack used to adjust omnilights in code.
	float OmniRangeMult[] =
		//	ent1  ent2  ctr1  ctr2  crd1  hal1  hal2  rbtl  rbtr  rbth
	{ 0.9,  0.9,  0.3,  0.3,  0.4,  0.5,  0.5,  0.5,  0.5,  0.5 };
	float OmniSizeMult[] =
		//	ent1  ent2  ctr1  ctr2  crd1  hal1  hal2  rbtl  rbtr  rbth
	{ 0.1,  0.1,  0.1,  0.1,  0.1,  0.1,  0.1,  0.1,  0.1,  0.1 };
	int OmniDisable[] =
	{ 0,    0,   0,    0,    0,    0,    0,    0,    0,    0 };
	int NumOmnis = sizeof(OmniDisable) / sizeof(int);

	Scene *Sc = ChaseSc;
	Omni *O;
	int32_t index;
	float redRangeMult = 100000.0;
	for (O = Sc->OmniHead, index = 0; O; O = O->Next)
	{
		//if (index == NumOmnis) break;

		// disable/enable omnies
		//if (OmniDisable[index]) O->Flags &=~Omni_Active;

		// adjust size / disable flares
//		if (OmniSizeMult[index] == 0)
//			O->Flags &=~(Omni_Flare | Omni_PointFlare);
		Spline_Scale(&O->Size, OmniSizeMult[index]);
		O->IRange = 1000.0;
		O->rRange = 1000.0;
		//O->ISize *= 50.0;
		//O->IRange *= 50.0;
		O->FallOff = 5000.0;

		// adjust range
//		Spline_Scale(&O->Range, OmniRangeMult[index]);
		//if (O->L.R == 255.0)
		Spline_Scale(&O->Range, redRangeMult);
		//Spline_Scale(&O->R, redRangeMult);

		//O->Flags |= Omni_Rand;
		index++;
	}
}

// sets required properties and modifies the object 'water.lwo' which acts like
// a mirror for the rest of the scene.
static void Reflective_Surface_Setup()
{
	TriMesh *T;
	Vertex *Vtx, *VE;
	Face *F, *FE;
	Vector U, V;

	Object *O;
	int32_t IID = 0;
	for (O = ChaseSc->ObjectHead; O; O = O->Next)
	{
		if (O->Type != Obj_TriMesh) continue;
		//		printf("[%d] Trimesh %s with %d Polygons\n",IID,O->Name,((TriMesh *)O->Data)->FIndex);
		T = (TriMesh *)(O->Data);

		if (!strcmp(O->Name, "water.lwo"))
		{
			RflObj = O;
			//T->SortPriorityBias = SP_DrawLast;
			F = T->Faces;
			Vector_Sub(&F->B->Pos, &F->A->Pos, &U);
			Vector_Sub(&F->C->Pos, &F->A->Pos, &V);
			Cross_Product(&U, &V, &RflSurfNorm);
			Vector_Norm(&RflSurfNorm);

			// patch: modify keys to position zero
			//Vector_Zero(&T->Pos.Keys[0].Pos.im());
			//Vector_Add(&F->A->Pos, &T->Pos.Keys[0].Pos.im(), &U);
			//RflSurfOfs = -Dot_Product(&RflSurfNorm, &U);

			F = T->Faces;
			FE = F + T->FIndex;

			// should write a transparent texture/gouraud to enable fog.
			for (; F < FE; F++)
			{
				F->U1 *= 0.25;
				F->V1 *= 0.25;
				F->U2 *= 0.25;
				F->V2 *= 0.25;
				F->U3 *= 0.25;
				F->V3 *= 0.25;
				F->Txtr->Flags |= Mat_Transparent;
			}

			// force scale
			//Vtx = T->Verts;
			//VE = Vtx + T->VIndex;
			//for (; Vtx < VE; Vtx++)
			//{
			//	Vector_SelfScale(&Vtx->Pos, 0.25);
			//	//int banana=1;
			//}

			// Tesselate reflective surface (helps affine gouraud/ shader work nicely)

			// This code section assumes rflSurfNorm = (0, 1, 0)
			// which means the surface is aligned to the XZ plane
			// furthermore it is assumed UV space maps onto XZ with only a scalar transformation
			// (no rotation)

			// Extract coverage (range) over XZ plane and over texture
			// (notice: this reads texture coords. from vertices, not faces).
/*			float mx, Mx, mz, Mz, y;
			float mu, Mu, mv, Mv;
			dword i, j;
			mx = Mx = T->Verts[0].Pos.x;
			mz = Mz = T->Verts[0].Pos.z;
			mu = Mu = T->Verts[0].U;
			mv = Mv = T->Verts[0].V;
			y = T->Verts[0].Pos.y;

			Material *RMat = T->Faces[0].Txtr;
			for(i=1; i<T->VIndex; i++)
			{
				float x = T->Verts[i].Pos.x;
				float z = T->Verts[i].Pos.z;
				if (x<mx) mx = x;
				if (x>Mx) Mx = x;
				if (z<mz) mz = z;
				if (z>Mz) Mz = z;

				float u = T->Verts[i].U;
				float v = T->Verts[i].V;
				if (u<mu) mu = u;
				if (u>Mu) Mu = u;
				if (v<mv) mv = v;
				if (v>Mv) Mv = v;

			}

			// replace vertices / polygons with a regular planar mesh
			delete [] T->Verts;
			delete [] T->Faces;
			const dword NumSections = 64;

			T->VIndex = (NumSections+1)*(NumSections+1);
			T->FIndex = NumSections*NumSections*2;

			T->Verts = new Vertex [T->VIndex];
			T->Faces = new Face [T->FIndex];

			memset(T->Verts, 0, sizeof(Vertex) * T->VIndex);
			memset(T->Faces, 0, sizeof(Face) * T->FIndex);

			Vtx = T->Verts;

			float dx = (Mx-mx) / (float)NumSections;
			float dz = (Mz-mz) / (float)NumSections;

			float du = (Mu-mu) / (float)NumSections;
			float dv = (Mv-mv) / (float)NumSections;

			float u, v, x, z;
			for(j=0, v = mv, z = mz; j<=NumSections; j++, v += dv, z += dz)
			{
				for(i=0, u = mu, x = mx; i<=NumSections; i++, u += du, x += dx)
				{
					Vtx->Pos.x = x;
					Vtx->Pos.y = y;
					Vtx->Pos.z = z;
					Vtx->U = u;
					Vtx->V = v;
					Vtx++;
				}
			}

			F = T->Faces;
			Vtx = T->Verts;
			for(j=0; j < NumSections; j++, Vtx += NumSections+1)
			{
				for(i=0; i < NumSections; i++)
				{
					F->A = Vtx+i;
					F->B = Vtx+i+1;
					F->C = Vtx+i+NumSections+2;
					F->Txtr = RMat;
					F->U1 = F->A->U;
					F->V1 = F->A->V;
					F->U2 = F->B->U;
					F->V2 = F->B->V;
					F->U3 = F->C->U;
					F->V3 = F->C->V;
					F++;
					F->A = Vtx+i;
					F->B = Vtx+i+NumSections+2;
					F->C = Vtx+i+NumSections+1;
					F->Txtr = RMat;
					F->U1 = F->A->U;
					F->V1 = F->A->V;
					F->U2 = F->B->U;
					F->V2 = F->B->V;
					F->U3 = F->C->U;
					F->V3 = F->C->V;
					F++;
				}
			}*/

			//			break;
		}
		else {
			//			T->Flags &=~HTrack_Visible;
		}
		if (strstr(O->Name, "pas"))
		{
			// attach rail tracks to ground
//			T->Pos.Keys[0].Pos.y = 0.0;

		}

		IID++;
	}
}



void Initialize_Chase()
{
	ChaseSc = (Scene *)getAlignedBlock(sizeof(Scene), 16); 
	memset(ChaseSc,0,sizeof(Scene));
	LoadFLD(ChaseSc,"SCENES/CHASE.FLD");
	CHPartTime = 25.0*(ChaseSc->EndFrame-ChaseSc->StartFrame)/25.0f;
//	printf("FLD-loaded MEM = %d\n",DPMI_Free_Memory());
	ChaseSc->FZP = 50000.0f;
	ChaseSc->NZP = 2.0f;
	ChaseSc->Flags |= Scn_Fogged | Scn_ZBuffer;

	ChaseSc->Ambient.R = 4 * 2;
	ChaseSc->Ambient.G = 4 * 2;
	ChaseSc->Ambient.B = 64 * 2;

	// also make the appropriate Layer 2 fillers.

	Material *M;
	for (M = MatLib; M; M = M->Next)
	{
		if (M->RelScene != ChaseSc) continue;
		if (!M->Txtr) continue;
		M->Flags |= Mat_RGBInterp;
		M->Txtr->Flags |= Txtr_Tiled;
	}

	Reflective_Surface_Setup();

	SceneCorrections();
	Preprocess_Scene(ChaseSc);
	//	printf("Scene-Proc MEM = %d\n",DPMI_Free_Memory());
}

void Run_Chase()
{
	int32_t Polys = 0;
	TriMesh *T;

	SetCurrentScene(ChaseSc);

	Reflective_Mapper_Setup();
	Setup_Water_Distort();


	for(T = ChaseSc->TriMeshHead;T;T=T->Next)
		Polys+=T->FIndex;
	FList = new Face * [Polys];
	SList = new Face * [Polys];

	View = ChaseSc->CameraHead;

	C_FZP = ChaseSc->FZP;
	C_rFZP = 1.0f / C_FZP;



	while ((!Keyboard[ScESC])&&Timer<CHPartTime)
	{
		g_FrameTime = Timer;
		//memset(VPage,0,PageSize);
		FastWrite(VPage, 0, (PageSize + XRes * YRes * sizeof(word)) >> 2);

		CurFrame = ChaseSc->StartFrame + (ChaseSc->EndFrame-ChaseSc->StartFrame) * (float)g_FrameTime / (float)CHPartTime;

		if (Keyboard[ScTab])
		{
			if (View == &FC)
				View = ChaseSc->CameraHead;
			else
				View = &FC;
		}

		Dynamic_Camera();
		if (Keyboard[ScC]) { FC.ISource = View->ISource; Matrix_Copy(FC.Mat, View->Mat); FC.IFOV = View->IFOV; }

		Animate_Objects(ChaseSc);

		((TriMesh *)(RflObj->Data))->Flags &= 0xFFFFFFFF - HTrack_Visible;

		Lighting(ChaseSc);

		Reflected_Transform(ChaseSc);

		if (!CAll) goto norefl;
		Radix_SortingASM(FList,SList,CAll);

		Render();

	norefl:

		((TriMesh *)(RflObj->Data))->Flags |= HTrack_Visible;
		Transform_Objects(ChaseSc);
		if (!CAll) continue;
		Radix_SortingASM(FList, SList, CAll);
		Render();

#ifdef _C_WATCOM
		Flip(Screen);
#else
		Flip(MainSurf);
#endif

	} Timer-=CHPartTime;
	/*
	if (Keyboard[ScESC])
	{
		#ifdef Play_Music
		ShutDown();
		#endif
		FDS_End();
		exit(-1);
	}
	*/

	while (Keyboard[ScESC]) continue;
	if (Timer < 0)
		Timer = 0;

	delete FList;
	delete SList;
	Destroy_Scene(ChaseSc);
}
