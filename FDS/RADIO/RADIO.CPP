// Radiosity / Light mapper
// Deals with all of the Light-maps involved Raytracing, Texture Caching...

#include <math.h>
#include <string.h>
//#include <conio.h>
#include <stdlib.h>
#include <stdio.h>

#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Base/TriMesh.h"
#include "Base/Omni.h"
#include "Base/Scene.h"

static Scene *RTSc;
static TriMesh *RTTri;
static Face *RTFace;
static Omni **Om_Buf,**OMB,**OMBE;
static Omni **VO_Buf,**VO,**VOE;
static TriMesh **TISect,**TP,**TPE;
static Matrix PolarBase;
static Vector *PBRow = (Vector *)PolarBase;

void Space_Pixel(Vector *V,DWord C)
{
	if (!View) return;
	Vector TP,TP2;
	int32_t PX,PY;

	Vector_Sub(V,&View->ISource,&TP);
	MatrixXVector(View->Mat,&TP,&TP2);
	if (TP2.z>1.0f)
	{
		PX = CntrX + TP2.x*FOVX/TP2.z;
		PY = CntrY - TP2.y*FOVY/TP2.z;
		if (PX>=0&&PX<XRes&&PY>=0&&PY<YRes)
			((DWord *)VPage)[PX+XRes*PY] = C;
	}

}

char * GetTName(Scene *Sc,TriMesh *T)
{
	Object *O;
	for(O=Sc->ObjectHead;O;O=O->Next)
	{
		if (O->Type!=Obj_TriMesh) continue;
		if (O->Data==(void *)T) return O->Name;
	}
	return NULL;
}

// checks if the given Interval intersects the Tri-meshes on scene.
// video killed the radio star! video killed the radio star!
DWord Interval2SceneIntersect(Vector *A,Vector *B)
{
	TriMesh *T;
	Vertex *Vtx,*VE,*V1,*V2,*V3;
	Face *F,*FE;
	float d1,d2,t,t1,t2,t3;
	Vector P;

//	return 1;
	float xc = Dot_Product(PBRow,A);
	float yc = Dot_Product(PBRow+1,A);
	int c = 0;

	// Scan collidable trimesh list
	for(TP=TISect;TP<TPE;TP++)
	{
		T = *TP;
		if (T==RTTri) continue;
		for(Vtx=T->Verts, VE = Vtx + T->VIndex ; Vtx<VE ; Vtx++)
		{
			// (RU,RV) will be used to store ray coordinates
			Vtx->UZ = Dot_Product(PBRow,&Vtx->Pos)-xc;
			Vtx->VZ = Dot_Product(PBRow+1,&Vtx->Pos)-yc;
		}

		for(F = T->Faces, FE = F + T->FIndex ; F<FE ; F++)
		{
			c++;
			//if (F==RTFace) continue; // this causes some problems
			// Backface Culling (yes, faces that are not facing A will be ignored,
			// since for each crossing of such face there's a crossing of a face
			// directed at A)
			if (Dot_Product(&F->N,A)+F->NormProd<0.0f) continue;
			// if d2>0 too, the entire ray is on the same side of the plane
			if (Dot_Product(&F->N,B)+F->NormProd>0.0f) continue;
			//d1=Dot_Product(&F->N,A)+F->NormProd;
			//d2=Dot_Product(&F->N,B)+F->NormProd;
			//if (d1*d2>0.0f) continue;
			V1 = F->A; V2 = F->B; V3 = F->C;
			// an advanced algorithm is used to see if (0,0) is on the
			// (RU,RV) interpolated triangle (as the ray is)
			t1 = (V2->VZ-V1->VZ)*V1->UZ + (V1->UZ-V2->UZ)*V1->VZ;
			t2 = (V3->VZ-V2->VZ)*V2->UZ + (V2->UZ-V3->UZ)*V2->VZ;
			t3 = (V1->VZ-V3->VZ)*V3->UZ + (V3->UZ-V1->UZ)*V3->VZ;
			//printf("OutSider = (%1.3f,%1.3f,%1.3f)",t1,t2,t3);
			if (t1*t2<0.0f) continue;//{printf("\n"); continue;}
			if (t1*t3<0.0f) continue;//{printf("\n"); continue;}
			//printf("..Shadowed!\n");
			return 0;
		}
	}
	return 1;
}

DWord Interval2FaceIntersect(Face *F,Vector *A,Vector *B)
{
	float D1 = Dot_Product(A,&F->N) + F->NormProd;
	float D2 = Dot_Product(B,&F->N) + F->NormProd;
	if (D1*D2>0.0f) return 0;
	float t = D1/(D1-D2);
	Vector Pos,U,V,W,P,N;
	float t1,t2,t3;
	Vector_LERP(A,B,t,&Pos);
	Vector_Sub(&Pos      ,&F->A->Pos,&P);
	Vector_Sub(&F->B->Pos,&F->A->Pos,&U);
	Vector_Sub(&F->C->Pos,&F->A->Pos,&V);
	Cross_Product(&F->N,&U,&N);
	t1 = Dot_Product(&N,&P);
	Cross_Product(&V,&F->N,&N); //VxN == Nx(-V)
	t3 = Dot_Product(&N,&P);
	if (t1*t3<0.0f) return 0;
	Vector_Sub(&V,&U,&W);
	Cross_Product(&F->N,&W,&N);
	t2 = Dot_Product(&N,&P) - Dot_Product(&N,&V);
	return t1*t2>=0.0f;
}

DWord SlowInterval2SceneIntersect(Vector *A,Vector *B)
{
	TriMesh *T;
	Face *F,*FE;
	DWord d;
	// Scan collidable trimesh list
	for(TP=TISect;TP<TPE;TP++)
	{
		T = *TP;
		if (T==RTTri) continue;
		for(F=T->Faces,FE = F + T->FIndex ; F<FE ; F++)
			if (Interval2FaceIntersect(F,A,B)) return 0;
	}
	return 1;
}

void QCSat(QColor *QC1,QColor *QC2)
{
	int32_t A;
	A = QC1->R+QC2->R;
	if (A>255) QC1->R = 255; else QC1->R = A;
	A = QC1->G+QC2->G;
	if (A>255) QC1->G = 255; else QC1->G = A;
	A = QC1->B+QC2->B;
	if (A>255) QC1->B = 255; else QC1->B = A;
}

void Scan_Convert_Lightmap()
{
	// now scan convert [UV]-->[XYZ]
	// with each new position call a Raytracer routine to check for obstacles
	// in light ray path.
/*	float U1 = RTFace->U1*0.0625f,V1 = RTFace->V1*0.0625f;
	float U2 = RTFace->U2*0.0625f,V2 = RTFace->V2*0.0625f;
	float U3 = RTFace->U3*0.0625f,V3 = RTFace->V3*0.0625f;
	float dU12 = U2-U1, dU13 = U3-U1, dU23 = U3-U2;
	float dV12 = V2-V1, dV13 = V3-V1, dV23 = V3-V2;
	float len;
	Vector dPdU,dPdV;
	Vector U,V;
	float t,uMerid;
	Vector lineP,scanP;
	Vector LPos;
	int32_t iU,iV;
	DWord *LMapPtr;
	QColor QC,LC;
	float falloff;
	float rlen;

	// Not enough surface on Texel space.
	if (fabs(dU12*dV13-dU13*dV12)<1.00f) return;

	// Now calc dPdu,dPdv
	if (fabs(dV13)>EPSILON)
	{
		// standard formula is usable:
		t = dV12/dV13;
		uMerid = dU12-t*dU13;
		Vector_Sub(&RTFace->B->Pos,&RTFace->A->Pos,&U); //dP12.
		Vector_Sub(&RTFace->C->Pos,&RTFace->A->Pos,&V); //dP13.
		Vector_Scale(&V,t,&dPdU); //dP13*dV12/dV13.
		Vector_Sub(&U,&dPdU,&dPdU); //dP12-dP13*dV12/dV13.
		Vector_SelfScale(&dPdU,1.0f/uMerid);
	} else {
		// since V1 and V3 are the same, this is easy:
		Vector_Sub(&RTFace->C->Pos,&RTFace->A->Pos,&dPdU); //dP13.
		Vector_SelfScale(&dPdU,1.0f/dU13); //dU13 isnt 0 since dV13 is.
	}
	if (fabs(dU13)>EPSILON)
	{
		// standard formula is usable:
		t = dU12/dU13;
		uMerid = dV12-t*dV13;
		Vector_Sub(&RTFace->B->Pos,&RTFace->A->Pos,&U); //dP12.
		Vector_Sub(&RTFace->C->Pos,&RTFace->A->Pos,&V); //dP13.
		Vector_Scale(&V,t,&dPdV); //dP13*dV12/dV13.
		Vector_Sub(&U,&dPdV,&dPdV); //dP12-dP13*dV12/dV13.
		Vector_SelfScale(&dPdV,1.0f/uMerid);
	} else {
		// since U1 and U3 are the same, this is easy:
		Vector_Sub(&RTFace->C->Pos,&RTFace->A->Pos,&dPdV); //dP13.
		Vector_SelfScale(&dPdV,1.0f/dV13); //dV13 isnt 0 since dU13 is.
	}

	// now, after having dPdU,dPdV, we'll just scan the entire Texel-map...
	// at this point, we'll just draw the positions
	Vector_Copy(&lineP,&RTFace->A->Pos);
	Vector_SAdd(&lineP,-U1,&dPdU);
	Vector_SAdd(&lineP,-V1,&dPdV);

	LMapPtr = RTFace->Surf->LMap;


	VO = VO_Buf;
	for(OMB = Om_Buf ; OMB<OMBE ; OMB++)
	{
		Vector_Sub(&((*OMB)->IPos),&lineP,&LPos);
		if (Dot_Product(&RTFace->N,&LPos)>0.0f)
			*VO++ = *OMB;
	}
	VOE = VO;
	char str[160];
//	sprintf(str,"Radiosity/Omnies = %d",VOE-VO_Buf);
//	MsgToDisplay(COLLISION_DET_ID,0,str);


	for(iV=0;iV<17;iV++)
	{
		Vector_Copy(&scanP,&lineP);
		for(iU=0;iU<17;iU++)
		{

			QC.R = QC.G = QC.B = 0;
			for(VO = VO_Buf ; VO<VOE ; VO++)
			{
				Vector_Sub(&((*VO)->IPos),&scanP,&LPos);
				len = SQRT(LPos.x*LPos.x+LPos.y*LPos.y+LPos.z*LPos.z);
				falloff = (*VO)->IRange-len;
				if (falloff < 0) continue;

				Vector_Copy(PBRow+2,&LPos);
				rlen = RSQRT(PBRow[2].x*PBRow[2].x+PBRow[2].y*PBRow[2].y+PBRow[2].z*PBRow[2].z);
				Vector_SelfScale(PBRow+2,rlen);
				PBRow[0].x = PBRow[2].z;
				PBRow[0].y = 0.0f;
				PBRow[0].z =-PBRow[2].x;
				rlen = PBRow[0].x*PBRow[0].x+PBRow[0].z*PBRow[0].z;
				if (rlen<EPSILON) Matrix_Form(PolarBase,1.0f,0.0f,0.0f,0.0f,0.0f,-1.0f,0.0f,1.0f,0.0f); else
				{
					rlen = RSQRT(rlen);
					Vector_SelfScale(PBRow,rlen);
					Cross_Product(PBRow+2,PBRow,PBRow+1);
				}
//				printf("[*] R-Matrix = [(%1.3f,%1.3f,%1.3f),(%1.3f,%1.3f,%1.3f),(%1.3f,%1.3f,%1.3f)\n",PolarBase[0][0],PolarBase[0][1],PolarBase[0][2],PolarBase[1][0],PolarBase[1][1],PolarBase[1][2],PolarBase[2][0],PolarBase[2][1],PolarBase[2][2]);
//				printf("[*] VSV = (%1.3f,%1.3f,%1.3f)\n",Vector_Length(PBRow),Vector_Length(PBRow+1),Vector_Length(PBRow+2));

				if (Interval2SceneIntersect(&((*VO)->IPos),&scanP))
				{
					falloff *= (*VO)->rRange;
					falloff *= falloff;
					falloff *= Dot_Product(&RTFace->N,&LPos)/len; // this is strictly positive ;)
					falloff *= 1.5f; //this is used so very closely ranged light will override angular falloff
					if (falloff > 1.0f) falloff = 1.0f; //saturation check - only if a >1 m.factor is used.
//					printf("Lighting Intensity %f Added\n",falloff);
					LC.R = (*VO)->L.R * falloff;
					LC.G = (*VO)->L.G * falloff;
					LC.B = (*VO)->L.B * falloff;
					QCSat(&QC,&LC);
				}
			}
//			printf("Writing Quantized Color RGB(%d,%d,%d)\n",QC.R,QC.G,QC.B);
			*LMapPtr++ = (QC.R<<16)+(QC.G<<8)+QC.B;
			Space_Pixel(&scanP,(QC.R<<16)+(QC.G<<8)+QC.B);
			Vector_SelfAdd(&scanP,&dPdU);
		}
		Vector_SelfAdd(&lineP,&dPdV);
	}*/
}

static inline float sqr(float x) {return x*x;}

// This Procedure calculates all the necessary Light-maps for the scene.
// each face on scene gets loads of memory wasted on the RGB lightmap...
// [16x16-32Bit] is about 1Kb / Face. this will more than quadruple the
// amount of neccessary memory per scene , but n/m.
void Radiosity(Scene *Sc)
{
	TriMesh *T,*T2;
	Face *F,*FE;
	Omni *Om;
	Vector U,V,W;
	int32_t I;
	RTSc = Sc;
	float len;
	float t;
	char *name1,*name2;

	int32_t Num_Om = 0;
	int32_t Num_T = 0;

	setbuf(stdout,NULL);
	printf("<Rad>: Radiosity/Light Maps Calculation starts. Timer = %d.%d\n",Timer/100,Timer%100);
	for(T=Sc->TriMeshHead;T;T=T->Next) Num_T++;
	for(Om=Sc->OmniHead;Om;Om=Om->Next) Num_Om++;
//	printf("<Rad>: Total Number of Trimeshes: %d\n",Num_T);
//	printf("<Rad>: Total Number of Omni-Lights: %d\n",Num_Om);
	Om_Buf = new Omni * [Num_Om];
	VO_Buf = new Omni * [Num_Om];
	TISect = new TriMesh * [Num_T];

	for (T=Sc->TriMeshHead;T;T=T->Next)
	{
		if (!(T->Flags&Tri_Radiosity)) continue;
//		name1 = GetTName(Sc,T);
//		printf("<Rad:Tri>: Creating Light Maps for Trimesh %s with %d Faces\n",name1,T->FIndex);
		RTTri = T;
		// ok, now we have to check whether the omni lights the Face or not.
		// some general tests will enhance speed magnificiently...
		OMBE = Om_Buf;
		for(Om=Sc->OmniHead;Om;Om=Om->Next)
		{
			if (!(Om->Flags&Omni_Radiosity)) continue;
			MatrixXVector(T->RotMat,&T->BSphereCtr,&V);
			Vector_SelfAdd(&V,&T->IPos);
			Vector_SelfSub(&V,&Om->IPos);
			if (SQRT(V.x*V.x+V.y*V.y+V.z*V.z)-SQRT(T->BSphereRad)>Om->IRange)
				continue;
			*OMBE++ = Om;
		}
		if (OMBE==Om_Buf)
		{
//			printf("<Rad:Tri>: No Lights within environment of Trimesh, Aborted.\n");
			continue;
		}


//  build collidable Trimesh list.
//		printf("<Rad:Tri>: Building Partial Trimesh List\n");
		TP = TISect;
		for(T2 = Sc->TriMeshHead;T2;T2=T2->Next)
		{
			if (T2==T) continue; // well...temporary assumption - no selfshadowing
//			name2 = GetTName(Sc,T2);
			// check distance between two spheres
			Vector_Sub(&T2->BSphereCtr,&T->BSphereCtr,&U);
			len = Vector_SelfDot(&U);
			// this awkward approach was used since sphere radius values are squared and
			// a regular vector_length would have sqrt anyway; since v_l<rad1+rad2,
			// v_l^2<rad1^2+rad^2+2*rad1*rad2.
			if (len<T2->BSphereRad+T->BSphereRad+2.0f*SQRT(T2->BSphereRad*T->BSphereRad))
			{
//				printf("<Rad:Tri>: Adding: TriMeshes %s and %s are near each other.\n",name1,name2);
				*TP++ = T2;
				continue;
			}
			// check distance to Lightsources on scene
			for(OMB = Om_Buf ; OMB<OMBE ; OMB++)
			{
				Vector_Sub(&T2->BSphereCtr,&(*OMB)->IPos,&V);
//				printf("<Rad:Tri>: Distance from Light source %3.3f,Radius %3.3f\n",Vector_Length(&V),SQRT(T2->BSphereRad));
				if (Vector_SelfDot(&V)<T2->BSphereRad)
				{
//					printf("<Rad:Tri>: Adding: TriMesh %s is near a Lightsource.\n",name2);
					*TP++ = T2;
					break;
				}
				// now a tough test, check if sphere touchs the cone between the light and the other trimesh sphere
				// ok, we got the T->T2 vector at U
				Vector_Sub(&(*OMB)->IPos,&T->BSphereCtr,&V);
				t = Dot_Product(&U,&V)/Vector_SelfDot(&V);
//				printf("<Rad:Tri>: Intersection ratio %1.3f\n",t);
				if (t<0.0f||t>1.0f) continue;
				Vector_Scale(&V,t,&W);
				t = sqr(1.0f-t);

				// now W contains the midpoint
				Vector_SelfSub(&W,&U);
				if (Vector_SelfDot(&W)<t*T->BSphereRad+T2->BSphereRad+2.0f*SQRT(t*T->BSphereRad*T2->BSphereRad))
				{
//					printf("<Rad:Tri>: Adding: TriMesh %s is near the Projection cone, t = %1.3f.\n",name2,1.0f-SQRT(t));
//					printf("           Measured Offshoot from Cone Axis = %3.3f, Local Radius = %3.3f\n",Vector_Length(&W),SQRT(t*T->BSphereRad+T2->BSphereRad+2.0f*SQRT(t*T->BSphereRad*T2->BSphereRad)));
					*TP++ = T2;
					break;
				}

			}
			if (OMB<OMBE) continue;
		}
		TPE = TP; // collidable trimesh list complete, only these will be scanned
//		printf("<Rad:Tri>: A total of %d TriMeshes were Inserted\n",TPE-TISect);
//		char str[160];
//		sprintf(str,"Collidable Meshes = %d",TPE-TISect);
//		MsgToDisplay(COLLISION_DET_ID,1,str);

//		printf("%d Omni-Lights will be used to Radiate the Tri-Mesh.\n",OMBE-Om_Buf);
		for (F=T->Faces,FE=F+T->FIndex;F<FE;F++)
		{
			RTFace = F;

/*			if (!F->Surf) {F->Surf = new Surface; memset(F->Surf,0,sizeof(Surface));}
			if (!F->Surf->LMap)
				F->Surf->LMap = new DWord[17*17]; //what a shame... 1K of crap...;(
			Scan_Convert_Lightmap();
			// just for fun
//			if (F->Surf->DTex)
//				delete F->Surf->DTex;
//			F->Surf->DTex = new DWord[256*256];
//			TCLightMapModulate(F->Surf);

			// Run a test: Parallel Projected Rays.
			// Vector dU,dV;
			// Vector_Sub(&F->B->Pos,&F->A->Pos,&dU);
			// Vector_SelfScale(&dU,1.0f/16.0f);
			// Vector_Sub(&F->C->Pos,&F->A->Pos,&dV);
			// Vector_SelfScale(&dV,1.0f/16.0f);
			// Vector LP,CP,NP,NO;
			// int32_t tli,tlj;
			// DWord *LMP = F->Surf->LMap,*CMP;
			// Vector_Copy(&LP,&F->A->Pos);
			// Vector_Scale(&F->N,200.0f,&NO);
			// for (tlj = 0 ; tlj <= 16 ; tlj++)
			// {
			// 	Vector_Copy(&CP,&LP);
			// 	CMP = LMP;
			// 	for (tli = 0 ; tli <= 16-tlj ; tli++)
			// 	{
			// 		Vector_Add(&CP,&NO,&NP);
			// 		//if (SlowInterval2SceneIntersect(&LP, &NP)) *LMP++ = 0xFFFFFFFF; else *LMP++ = 0x00000000;
			// 		*CMP++ = SlowInterval2SceneIntersect(&CP, &NP)*0x00010101;
			// 		Space_Pixel(&CP,*(CMP-1)); //hack it!
			// 		Space_Pixel(&NP,*(CMP-1)); //hack it!
			// 		Vector_SelfAdd(&CP,&dU);
			// 	}
			// 	LMP += 17;
			// 	Vector_SelfAdd(&LP,&dV);
			// }*/
		}
	}
//	printf("<Rad>: Radiosity/Light Maps Calculation ends. Timer = %d.%d\n",Timer/100,Timer%100);
	delete TISect;
	delete VO_Buf;
	delete Om_Buf;
}


///////////////////////////////////////
/// Handles Surfaces, S-Cache and stuff

DWord *SCache; // the cache holds modulated textures of 64x64
DWord SCacheSize;

DWord New_SCache(DWord Size)
{
	SCache = new DWord[64*64*Size];
	if (!SCache) return 0;
	SCacheSize = Size;
	return 1;
}
