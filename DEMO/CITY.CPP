
#include "Rev.h"
#include "CITY.H"
#include <algorithm>
#include <vector>
#include "FRUSTRUM.H"
#include "Gradient.h"
#include <map>

#define FRONT_TO_BACK_SORTING
#define CITY_RAIN

static Scene *CitySc;
Scene *SkySc;
static Object *RflObj;
static Vector RflSurfNorm(0,1,0);
static float RflSurfOfs = 0;

static int32_t CTPartTime;
static Matrix AxisAlignedViews[6] = {
	{
		{ 1,  0,  0},
		{ 0,  1,  0},
		{ 0,  0,  1},
	},
	{
		{ 0,  0,  1},
		{ 0,  1,  0},
		{-1,  0,  0},
	},
	{
		{-1,  0,  0},
		{ 0,  1,  0},
		{ 0,  0, -1},
	},
	{
		{ 0,  0, -1},
		{ 0,  1,  0},
		{ 1,  0,  0},
	},
	{
		{ 1,  0,  0},
		{ 0,  0,  1},
		{ 0,  -1, 0},
	},
	{
		{ 1,  0,  0},
		{ 0,  0, -1},
		{ 0,  1,  0},
	},
};


static signed char *STbl,*SOTbl,*CTbl,*COTbl;

void Calc_STable()
{
	STbl = new signed char [15*XRes];
	SOTbl = STbl + 5*XRes;
	int32_t I;
	for(I=-5*XRes;I<10*XRes;I++)
		SOTbl[I] = (int32_t) (1.5f * YRes/240.0 * sin (I * 0.01f));
}

void Calc_CTable()
{
	CTbl = new signed char [13*XRes];
	COTbl = CTbl + 3*XRes;
	int32_t I;
	for(I=-3*XRes;I<10*XRes;I++)
		COTbl[I] = (int32_t) (4.0f * XRes/320.0 * cos (I * 0.01f));
//	COTbl = SOTbl + 157;
}

DWord *DistBuf,*DistPage;
DWord WaterBuf[65536*4];

void Setup_Water_Distort()
{
	TriMesh *T = (TriMesh *)RflObj->Data;
	memcpy(WaterBuf,(DWord *)(T->Faces->Txtr->Txtr->Data), 65536 * 4);//WaterTexture.Data;
}


void Water_Distort()
{
/*	int32_t I;
	GridPoint *Water_GP;
//	DWord *TempBuf = new DWord[65536 * 4];
	Image WaterImg;
	WaterImg.x = 256;
	WaterImg.y = 256;
	TriMesh *T = (TriMesh *)RflObj->Data;
	DWord *WaterSurf = (DWord *)(T->Faces->Txtr->Txtr->Data);//WaterTexture.Data;
	WaterImg.Data = WaterBuf;
	int x,y,X,Y,i;
	float R1,R3,R4,sinr4,cosr4,u,v;

//	WOBPOINTSHEIGHT = 32;
	Water_GP = new GridPoint[33*33];
	// Run wobbler
	R1=-Timer*0.01;
	i=0;
//	memcpy(WaterSurf,WaterBuf,65536*4);
	for (y=0;y<=256;y+=8)
		for (x=0;x<=256;x+=8,i++)
		{
			X = (x - 128)<<1;
			Y = (y - 128)<<1;
			R3 = sqrt(X * X + Y * Y) / (128.0*1.412);
			if (R3>0.85) R3 = 0.85;
			R4 = cos((R3)*5) + R1 - R3;
			sinr4 = sin(X);
			cosr4 = cos(Y);
			u = x * 256 * 2;
			v = y * 256 * 2;
			Water_GP[i].U=cosr4*(u-32767)+32767;
			Water_GP[i].V=sinr4*(v-32767)+32767;
		}
	Grid_Texture_Mapper_XXX(Water_GP,&WaterImg,WaterSurf);
//	memcpy(WaterSurf, TempBuf, 65536 * 4);

//	for(Y=-128;Y<128;Y++)
//		for(X=-128;X<128;X++)
//			if (X*X+Y*Y>128*128)
//				((DWord *)VorTexture.Data)[128+128*256+X+(Y<<8)] = 0;


//	delete TempBuf;
	delete Water_GP;*/
}



void Init_Distort(void)
{
	Calc_STable();
	Calc_CTable();
	/// Allocate padding for +-16 displacement
	DistBuf = new DWord[(XRes+32)*(YRes+32)];
	memset(DistBuf, 0, (XRes+32)*(YRes+32)*sizeof(dword));
	DistPage = DistBuf + XRes*16 + 16;
}

void Run_Distort(void)
{
//	Image Img;
//	Load_Image_JPEG(&Img,"Textures\\Distort.JPG");

//	Calc_STable();
//	Calc_CTable();
//	Scale_Image(&Img,XRes,YRes);
//	DWord *Ptr;
//	DWord *DPtr = Img.Data;
	int32_t I,T;

	int32_t X,Y,hX = XRes>>1,hY = YRes>>1;
//	int32_t Frames = 0;
//	Timer = 0;
//	while(!Keyboard[ScESC])
//	{
		memcpy(DistPage,VPage,PageSize);

		DWord *DPtr = DistPage;
		DWord *Ptr = (DWord *)VPage;

		I = 0;
		T = (Timer*3>>1)%628;
		for(Y=-hY;Y<hY-1;Y++)
			for(X=-hX;X<hX;X++)
				*Ptr++ = DPtr[(I++) + COTbl[(T+(X<<1)-Y)<<1]+//-(X>>3)+
					XRes*(SOTbl[(T<<1)+3*X+5*Y])]; //- (Y>>3))];
		memcpy(Ptr,DPtr+XRes*YRes_1,VESA_BPSL);
				//(int32_t)(cos(T*0.02+X*0.04-Y*0.02) * 5.0f)
				//(int32_t)(sin(T*0.02+X*0.03+Y*0.05) * 5.0f) - (int32_t)(Y * 0.1)
//		Frames++;
//	}
//	printf("FPS = %1.3f\n",100.0f*(float)Frames/(float)Timer);
}





// sets required properties and modifies the object 'water.lwo' which acts like
// a mirror for the rest of the scene.
static void Reflective_Surface_Setup()
{
	TriMesh *T;
	Vertex *Vtx, *VE;
	Face *F,*FE;
	Vector U,V;

	Object *O;
	int32_t IID = 0;
	for(O = CitySc->ObjectHead;O;O=O->Next)
	{
		if (O->Type!=Obj_TriMesh) continue;
//		printf("[%d] Trimesh %s with %d Polygons\n",IID,O->Name,((TriMesh *)O->Data)->FIndex);
		T = (TriMesh *)(O->Data);

		if (!strcmp(O->Name,"water.lwo"))
		{
			RflObj = O;
			//T->SortPriorityBias = SP_DrawLast;
			F = T->Faces;
			Vector_Sub(&F->B->Pos,&F->A->Pos,&U);
			Vector_Sub(&F->C->Pos,&F->A->Pos,&V);
			Cross_Product(&U,&V,&RflSurfNorm);
			Vector_Norm(&RflSurfNorm);
			
			// patch: modify keys to position zero
			//Vector_Zero(&T->Pos.Keys[0].Pos.im());
			Quaternion &P = T->Pos.Keys[0].Pos;
			P.x = P.y = P.z = 0.0f;

			Vector tmp = T->Pos.Keys[0].Pos.im();
			Vector_Add(&F->A->Pos, &tmp, &U);
			RflSurfOfs =-Dot_Product(&RflSurfNorm,&U);

			F = T->Faces;
			FE = F+T->FIndex;

			// should write a transparent texture/gouraud to enable fog.
			for(;F<FE;F++)
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
			Vtx = T->Verts;
			VE = Vtx+T->VIndex;
			for(;Vtx<VE;Vtx++)
			{
				Vector_SelfScale(&Vtx->Pos, 0.25);
				//int banana=1;
			}

			// Tesselate reflective surface (helps affine gouraud/ shader work nicely)

			// This code section assumes rflSurfNorm = (0, 1, 0)
			// which means the surface is aligned to the XZ plane
			// furthermore it is assumed UV space maps onto XZ with only a scalar transformation
			// (no rotation)

			// Extract coverage (range) over XZ plane and over texture
			// (notice: this reads texture coords. from vertices, not faces).
			float mx, Mx, mz, Mz, y;
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
					Vtx->U = u / 2.5;
					Vtx->V = v / 2.5;
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
			}

//			break;
		} else {
//			T->Flags &=~HTrack_Visible;
		}
		if (strstr(O->Name,"pas"))
		{
			// attach rail tracks to ground
//			T->Pos.Keys[0].Pos.y = 0.0;

		}

		IID++;
	}
}


void Reflective_Mapper_Setup()
{
	TriMesh *T;
	Vertex *Vtx, *VE;
	Face *F,*FE;
	Vector U,V;

	Object *O;
	int32_t IID = 0;
	for(O = CitySc->ObjectHead;O;O=O->Next)
	{
		if (O->Type!=Obj_TriMesh) continue;
//		printf("[%d] Trimesh %s with %d Polygons\n",IID,O->Name,((TriMesh *)O->Data)->FIndex);
		if (!strcmp(O->Name,"water.lwo"))
		{
			RflObj = O;
			T = (TriMesh *)(O->Data);

			F = T->Faces;
			FE = F+T->FIndex;

			// should write a transparent texture/gouraud to enable fog.
//			break;
			for(;F<FE;F++)
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
	Matrix M,IM, ReflMat;
	float M34[3][4];
	Vector AP,S,P,U,V,*W=(Vector *)M,*W2=(Vector *)IM,*Scl;
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

	float fzp = CurScene->FZP;
	
	TriMesh *TR;
	
	TR = (TriMesh *)(RflObj->Data);
	
	//for (T=Sc->TriMeshHead;T;T=T->Next)
	Object *Obj; 
	for(Obj=Sc->ObjectHead; Obj; Obj=Obj->Next)
	{
		
		if (Obj->Type != Obj_TriMesh) continue;
		//if (stricmp(Obj->Name, "Ship1.lwo")) continue;
		T = (TriMesh *)(Obj->Data);

		if (T==TR) {T->Flags|=Tri_Invisible; continue;}
		
		if (!(T->Flags&HTrack_Visible)) {T->Flags|=Tri_Invisible; continue;}
		
		Matrix_Copy(ReflMat,T->RotMat);
		//Vector_SelfScale(W2+1,-1.0f);
		ReflMat[0][1] *= -1.0f; ReflMat[1][1] *= -1.0f; ReflMat[2][1] *= -1.0f;
		MatrixXMatrix(View->Mat,ReflMat,M);
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
		// mirror T->ipos around first
		L1 = (-2.0f)*Dot_Product(&T->IPos,&RflSurfNorm);
		Vector_Scale(&RflSurfNorm,L1,&P);
		Vector_SelfAdd(&P,&T->IPos);
		
		Vector_Sub(&P,&View->ISource,&U);
		MatrixXVector(View->Mat,&U,&S);
		V.x = CntrEX*S.z+PX*S.x;
		V.y = CntrEY*S.z-PY*S.y;
		V.z = S.z;
		// make a corrected sphere center vector
		MatrixXVector(IM,&T->BSphereCtr,&AP);
		Vector_SelfAdd(&S,&AP);
		
		//    Vector_Copy(&V,&S);
		// ready
		// 4x3 AFFINE XFORM
		M34[0][0] = M[0][0]; M34[0][1] = M[0][1]; M34[0][2] = M[0][2]; M34[0][3] = V.x;
		M34[1][0] = M[1][0]; M34[1][1] = M[1][1]; M34[1][2] = M[1][2]; M34[1][3] = V.y;
		M34[2][0] = M[2][0]; M34[2][1] = M[2][1]; M34[2][2] = M[2][2]; M34[2][3] = V.z;
		// ready
		
		
		// aprioric Offset Vector.
		MatrixTXVector(ReflMat,&U,&AP);
		Vector *WP = (Vector *)ReflMat;
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
				//        Vtx->RU=Vtx->U*Vtx->RZ;
				//        Vtx->RV=Vtx->V*Vtx->RZ;
				//if (Vtx->TPos.z>Sc->FZP) Vtx->Flags|=Vtx_VisFar;
			}
			
			goto OUT;
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
				//        Vtx->RU=Vtx->U*Vtx->RZ;
				//        Vtx->RV=Vtx->V*Vtx->RZ;
				Vtx->Flags&=0xFFFFFFFF-Vtx_Visible;
				if (Vtx->PX<0) Vtx->Flags|=Vtx_VisLeft;
				if (Vtx->PX>=XRes) Vtx->Flags|=Vtx_VisRight;
				if (Vtx->PY<0) Vtx->Flags|=Vtx_VisUp;
				if (Vtx->PY>=YRes) Vtx->Flags|=Vtx_VisDown;
				if (Vtx->TPos.z>Sc->FZP) Vtx->Flags|=Vtx_VisFar;
			}
			//    printf("Ahead VGA/Wizard.\n");
			goto OUT;
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
					//          Vtx->RU=Vtx->U*Vtx->RZ;
					//          Vtx->RV=Vtx->V*Vtx->RZ;
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
			// coordinates to (EU,EV) by rotating the v. normals accordingly.
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
				//        Vtx->RU=Vtx->U*Vtx->RZ;
				//        Vtx->RV=Vtx->V*Vtx->RZ;
			}
			goto OUT;
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
				//        Vtx->RU=Vtx->U*Vtx->RZ;
				//        Vtx->RV=Vtx->V*Vtx->RZ;
				if (Vtx->PX<0) Vtx->Flags=Vtx_VisLeft; else Vtx->Flags=0;
				if (Vtx->PX>=XRes) Vtx->Flags+=Vtx_VisRight;
				if (Vtx->PY<0) Vtx->Flags+=Vtx_VisUp;
				if (Vtx->PY>=YRes) Vtx->Flags+=Vtx_VisDown;
			}
			//    printf("Ahead VGA/Wizard.\n");
			goto OUT;
ERegular:
			for (Vtx=T->Verts;Vtx<VEnd;Vtx++)
			{
				//    if (!Vtx->FRem) continue;
				MatrixXVector(M,&Vtx->Pos,&U);
				Vector_Add(&U,&V,&Vtx->TPos);
				
				Vtx->Flags = 0;
				//      if (*(int32_t *)(&Vtx->TPos.z)>0x3F800000) // 1.0 in floating point rep.
				// Environment mapping support removed at 11.04.02
//				Vtx->EU=128.0+127.0*(Vtx->N.x*IM[0][0]+Vtx->N.y*IM[0][1]+Vtx->N.z*IM[0][2]);
//				Vtx->EV=128.0+127.0*(Vtx->N.x*IM[1][0]+Vtx->N.y*IM[1][1]+Vtx->N.z*IM[1][2]);
				
				if (Vtx->TPos.z>Sc->NZP)
				{
					Vtx->RZ=1.0/Vtx->TPos.z;
					Vtx->PX=Vtx->TPos.x*Vtx->RZ;
					Vtx->PY=Vtx->TPos.y*Vtx->RZ;
					//          Vtx->PX=CntrEX+PX*Vtx->TPos.x*Vtx->RZ;
					//          Vtx->PY=CntrEY-PY*Vtx->TPos.y*Vtx->RZ;
 					//          Vtx->RU=Vtx->U*Vtx->RZ;
					//          Vtx->RV=Vtx->V*Vtx->RZ;
//					Vtx->REU=Vtx->EU*Vtx->RZ;
//					Vtx->REV=Vtx->EV*Vtx->RZ;
					if (Vtx->PX<0) Vtx->Flags=Vtx_VisLeft;
					if (Vtx->PX>=XRes) Vtx->Flags+=Vtx_VisRight;
					if (Vtx->PY<0) Vtx->Flags+=Vtx_VisUp;
					if (Vtx->PY>YRes_1) Vtx->Flags+=Vtx_VisDown;
				} else Vtx->Flags=Vtx_VisNear;
				//      printf("Regular shit!\n");
			}
			
		}
OUT:FEnd=T->Faces+T->FIndex;
		for (F = T->Faces; F < FEnd; F++) {
			if (!F->VisibilityFlagsAll()
				&& ((F->Txtr->Flags & Mat_TwoSided) ||
					//(1)
					(AP.x * F->N.x + AP.y * F->N.y + AP.z * F->N.z < F->NormProd) //backface culling

					)) {
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
	}
	CPolys = Ins-FList;
	for(O=Sc->OmniHead;O;O=O->Next)
	{
		Vtx=&O->V;
		Vector ReflectedPos = O->IPos;
		ReflectedPos.y = -ReflectedPos.y;
		Vector_Sub(&ReflectedPos,&View->ISource,&V);
		MatrixXVector(View->Mat,&V,&Vtx->TPos);
		if (Vtx->TPos.z>Sc->NZP&&Vtx->TPos.z<Sc->FZP)
		{
			Vtx->RZ=1.0/Vtx->TPos.z;
			Vtx->PX=CntrEX+Vtx->TPos.x*PX*Vtx->RZ;
			Vtx->PY=CntrEY-Vtx->TPos.y*PY*Vtx->RZ;
			// Insert to List
			//dz = Vtx->TPos.z;
			//dz *=-16384;
			//dz +=0x7FFFFFFF;
			//RoundToInt((int32_t *)&O->F.SortZ.DW,dz);
			dz = Vtx->TPos.z;
			*Ins++ = &O->F; // no insertion : flares are not reflected for now
			O->F.SortZ.F = 2 * fzp - dz;
		}
	}
	COmnies = (Ins-FList)-CPolys;
	for(I=0;I<Sc->NumOfParticles;I++)
	{
		if (Sc->Pcl[I].Flags&Particle_Active)
		{
			if ((dz=Sc->Pcl[I].V.TPos.z)>=Sc->NZP) {
				F = &Sc->Pcl[I].F;
				dz *=-16384;
				dz +=0x7FFFFFFF;
				RoundToInt((int32_t*)&F->SortZ.DW,dz);
#ifdef FRONT_TO_BACK_SORTING
				F->SortZ.F = 2*fzp-dz;
#else
				F->SortZ.F = fzp-dz;
#endif
//				*Ins++ = F;
			}
		}
	}
	CAll = Ins-FList;
	CPcls = CAll-COmnies-CPolys;
}

static float C_reFZP;

// This function is called to alter parameters within scene objects after loading from FLD.
static void SceneCorrections()
{
	// a crazy hack used to adjust omnilights in code.
	float OmniRangeMult[] = 
	//	ent1  ent2  ctr1  ctr2  crd1  hal1  hal2  rbtl  rbtr  rbth
		{0.9,  0.9,  0.3,  0.3,  0.4,  0.5,  0.5,  0.5,  0.5,  0.5};
	float OmniSizeMult[] =
	//	ent1  ent2  ctr1  ctr2  crd1  hal1  hal2  rbtl  rbtr  rbth
		{1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  1.5,  0.75,  1.0};
	int OmniDisable[] = 
		{  0,    0,    0,    0,    0,    0,    0,    0,    0,    0};
	int NumOmnis = sizeof(OmniDisable)/sizeof(int);

	Scene *Sc = CitySc;
	Omni *O;
	int32_t index;
	float redRangeMult = 1.8;
	for(O=Sc->OmniHead, index=0; O; O=O->Next)
	{
		//if (index == NumOmnis) break;

		// disable/enable omnies
		//if (OmniDisable[index]) O->Flags &=~OmniLight_Active;
		O->Flags &= ~Omni_Active;
		// adjust size / disable flares
//		if (OmniSizeMult[index] == 0)
			O->Flags &=~Omni_Flare;
			O->Flags &= ~Omni_PointFlare;
			//		Spline_Scale(&O->Size, OmniSizeMult[index]);

		// adjust range
//		Spline_Scale(&O->Range, OmniRangeMult[index]);
		if (O->L.R == 255.0)
			Spline_Scale(&O->Range, redRangeMult);

		O->Flags = 0;
		index++;
	}
}


void City_Lighting()
{
	Scene *Sc = CitySc;
	TriMesh *T;
	Vertex *Vtx,*VEnd;
	Face *Face,*FEnd;
	Omni *O;
	float R,G,B,Dot,Dot2,Its,Color,Distance;
	const float Ka=Ambient_Factor/*255.0*/,Kd=Diffusive_Factor,Ks=0.0,n=4.0;
	Vector L,U,V,W,TN,CU,CV;
	Matrix M;
	float rLen,Len,Len2;

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
		} else Lumin = 1.0;

		VEnd=T->Verts+T->VIndex;
		FEnd=T->Faces+T->FIndex;
		Vector_Sub(&T->IPos,&View->ISource,&U);
		MatrixTXVector(T->RotMat,&U,&W);
		// Camera Illumination model (Lambertian)
		for(Vtx=T->Verts;Vtx<VEnd;Vtx++)
		{
			Vtx->LR = Lumin*Ka;
			Vtx->LG = Lumin*Ka;
			Vtx->LB = Lumin*Ka;
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
		for(O=Sc->OmniHead;O;O=O->Next)
			if(1)//O->Flags&Flare_LightSource)
			{
				Vector_Sub(&T->IPos,&O->IPos,&U);
				MatrixTXVector(T->RotMat,&U,&W);

//				Vector_Sub(&View->ISource,&O->IPos,&CU);
//				MatrixTXVector(View->Mat,&CU,&CV);

				// check the lousy b-sphere
				if (Vector_SelfDot(&U)-T->BSphereRad<O->IRange) continue;

				if (O->IRange)
				{
					for(Vtx=T->Verts;Vtx<VEnd;Vtx++)
					{
						Vector_Add(&Vtx->Pos,&W,&L);
//						if (Vector_SelfDot(&L)>O->IRange) continue;
						rLen = RSQRT(L.x*L.x+L.y*L.y+L.z*L.z);
						Dot=-Dot_Product(&L,&Vtx->N)*rLen;

//					Vector_Add(&Vtx->Pos,&CV,&L);
//					rLen = RSQRT(L.x*L.x+L.y*L.y+L.z*L.z);
//					Dot2=-Dot_Product(&L,&Vtx->N)*rLen;

						if (Dot>0.0f)
						{
						//Color=(Kd*Dot/*+Ks*(Dot*Dot*Dot+Dot2)*0.5f*/)*rLen*rLen;
							Color=(Dot*(Kd+Ks*Dot*Dot)*0.5f)*rLen*rLen*O->ISize;
							Vtx->LR += O->L.R * Color;
							Vtx->LG += O->L.G * Color;
							Vtx->LB += O->L.B * Color;
						}
					}
				} else {
					for(Vtx=T->Verts;Vtx<VEnd;Vtx++)
					{
						Vector_Add(&Vtx->Pos,&W,&L);
						rLen = RSQRT(L.x*L.x+L.y*L.y+L.z*L.z);
						Dot=-Dot_Product(&L,&Vtx->N)*rLen;

//					Vector_Add(&Vtx->Pos,&CV,&L);
//					rLen = RSQRT(L.x*L.x+L.y*L.y+L.z*L.z);
//					Dot2=-Dot_Product(&L,&Vtx->N)*rLen;

						if (Dot>0.0f)
						{
						//Color=(Kd*Dot/*+Ks*(Dot*Dot*Dot+Dot2)*0.5f*/)*rLen*rLen;
							Color=(Dot*(Kd+Ks*Dot*Dot)*0.5f)*O->IRange*rLen*rLen*O->ISize;
							Vtx->LR += O->L.R * Color;
							Vtx->LG += O->L.G * Color;
							Vtx->LB += O->L.B * Color;
							}
					}
				}
			}

		// Saturation.
		for(Vtx=T->Verts;Vtx<VEnd;Vtx++)
		{
			if (Vtx->LR>253.0) Vtx->LR=253.0;
			if (Vtx->LG>253.0) Vtx->LG=253.0;
			if (Vtx->LB>253.0) Vtx->LB=253.0;
//      Vtx->LR = 0.25; Vtx->LG = 0.25; Vtx->LB = 0.25; //Or some other jack,like an Ambient constant
		}
	}

	// Disable vertex-fog
	if (Sc->Flags&Scn_Fogged)
	{
		for(T=Sc->TriMeshHead;T;T=T->Next)
		{
			if (T->Flags&Tri_Invisible) continue;
			if (T->Flags&Tri_Noshading) continue;

			Vtx=T->Verts;
			VEnd=Vtx+T->VIndex;
			for(;Vtx<VEnd;Vtx++)
			{
				//apply fog
				Color = Vtx->TPos.z*CitySc->FZP;

//				Vtx->LR = Vtx->LR + (128.0f-Vtx->LR) * Color;
//				Vtx->LG = Vtx->LG + (128.0f-Vtx->LG) * Color;
//				Vtx->LB = Vtx->LB + (128.0f-Vtx->LB) * Color;
				Vtx->LR *= Color;
				Vtx->LG *= Color;
				Vtx->LB *= Color;
			}
		}
	}
}

void Reflective_AnimateTexture()
{
	TriMesh *T = (TriMesh *)(RflObj->Data);

	/*Vertex *Vtx,*VE;
	for(Vtx = T->Verts, VE = Vtx + T->VIndex; Vtx<VE; Vtx++)
	{
		Vtx->U += dTime * 0.001;
		Vtx->V += dTime * 0.0005;
	}*/
	Face *F, *FE;
	for(F = T->Faces, FE = F + T->FIndex; F<FE; F++)
	{
		F->U1 += dTime * 0.0007 * 2;
		F->V1 += dTime * 0.0003 * 2;
		F->U2 += dTime * 0.0007 * 2;
		F->V2 += dTime * 0.0003 * 2;
		F->U3 += dTime * 0.0007 * 2;
		F->V3 += dTime * 0.0003 * 2;
	}
}

// this fixes an omnilight to follow the camera and light things up a bit.
static void AddCameraOmni()
{
}

const float rainMaxHeight = 2000.0f;
const float rainMaxDist = 6000.0f;

XMMVector rainVel;

#define NUM_RAIN_PCLS 3000

static Material dummyRainMat;

static void getRadial(Vector &c, Vector &v)
{
	float a, b;
	do
	{
		a = (RAND_15() - 16384)/16384.0;
		b = (RAND_15() - 16384)/16384.0;
	} while (a*a+b*b > 1.0);

	// project c onto plane y = v.y + rainMaxHeight.
	float y = v.y;
	float t = (y - c.y) / rainVel.y;

	v.x = c.x + t * rainVel.x + a * rainMaxDist;
	v.z = c.z + t * rainVel.z + b * rainMaxDist;
}

static void getRadialRandomHeight(Vector& c, Vector& v)
{
	v.y = RAND_15() * rainMaxHeight / 32768.0;
	getRadial(c, v);
}


static dword numDropsRendered;
static void drawRainDrop(Face* F, Vertex **VA, mword numVerts, dword miplevel)
{
	Vertex *V = VA[0];

	int32_t x = Fist(V->PX);
	int32_t y = Fist(V->PY);
	
	if (x>=0&&x<XRes&&y>=0&&y<YRes)
	{
		dword Z;
		if (V->TPos.z < C_FZP)
			Z = 0xFF80 - Fist(g_zscale * V->TPos.z);
		else
			Z = 1;
		dword offset = x + y*XRes;
		if ( ((word *)(VPage+PageSize))[offset] < Z )
		{
			((dword *)VPage)[offset] = 0x7FAFFF;
			numDropsRendered++;
		}
	}
}

static void rainmapper(float x1, float y1, float x2, float y2)
{
	if (y1<0 || y2 >= YRes || x1<2 || x2<2 || x1 >= XRes-3 || x2 >= XRes-3) return;	

	float rdy = 1.0 / (y2-y1);
	float dxdy = (x2-x1) * rdy;

	int32_t iy1,iy2, ix;
	iy1 = Fist(y1);
	iy2 = Fist(y2);
	float x = x1 + (iy1-y1) * dxdy;

	dword *scanline = (dword *)VPage;
	dword j = XRes;
	scanline += iy1 * j;

	float lt = (iy1-y1) * rdy;
	dword R, G, B, a, A, pixel;

	for(int32_t y=iy1; y<iy2; y++)
	{
		ix = Fist(x);

		if (lt < 0.7)
		{
//			R = lt * 0xCF / 0.8;
//			G = lt * 0xCF / 0.8;
//			B = lt * 0xFF / 0.8;
			A = lt * 0xFF / 0.7;
		} else {
//			R = (1.0-lt) * 0xCF / 0.2;
//			G = (1.0-lt) * 0xCF / 0.2;
//			B = (1.0-lt) * 0xFF / 0.2;
			A = (1.0-lt) * 0xFF / 0.3;
		}

		//dword P2 = ((R>>2)<<16) + ((G>>2)<<8) + (B>>2);
		//dword P1 = ((R>>1)<<16) + ((G>>1)<<8) + (B>>1);
		//dword P0 = ((R   )<<16) + ((G   )<<8) + (B);
		
		// modulate(scanline[ix-2], 0x7F7F7F, A, 255-A);
		
/*		for (int dx=-2; dx<=2; dx++)
		{
			pixel = scanline[ix+dx];

			R = (pixel>>16);
			G = ((pixel & 0xff00)>>8);
			B = (pixel & 0xff);

			a = A >> abs(dx);
			R = (R * (255 - a) + a * 0x7f) >> 8;
			G = (G * (255 - a) + a * 0x7f) >> 8;
			B = (B * (255 - a) + a * 0x7f) >> 8;
			scanline[ix+dx] = ((R   )<<16) + ((G   )<<8) + (B);
		}*/

		pixel = scanline[ix-2];

		R = ((pixel & 0xff0000)>>16);
		G = ((pixel & 0xff00)>>8);
		B = (pixel & 0xff);

		a = A >> 2;
		R = (R * (255 - a) + a * 0x7f) >> 8;
		G = (G * (255 - a) + a * 0x8f) >> 8;
		B = (B * (255 - a) + a * 0xaf) >> 8;
		scanline[ix-2] = ((R   )<<16) + ((G   )<<8) + (B);

		pixel = scanline[ix-1];

		R = ((pixel & 0xff0000)>>16);
		G = ((pixel & 0xff00)>>8);
		B = (pixel & 0xff);

		a = A*3 >> 2;
		R = (R * (255 - a) + a * 0x7f) >> 8;
		G = (G * (255 - a) + a * 0x8f) >> 8;
		B = (B * (255 - a) + a * 0xaf) >> 8;
		scanline[ix-1] = ((R   )<<16) + ((G   )<<8) + (B);

		pixel = scanline[ix];

		R = ((pixel & 0xff0000)>>16);
		G = ((pixel & 0xff00)>>8);
		B = (pixel & 0xff);

		a = A;
		R = (R * (255 - a) + a * 0x7f) >> 8;
		G = (G * (255 - a) + a * 0x8f) >> 8;
		B = (B * (255 - a) + a * 0xaf) >> 8;
		scanline[ix] = ((R   )<<16) + ((G   )<<8) + (B);

		pixel = scanline[ix+1];

		R = ((pixel & 0xff0000)>>16);
		G = ((pixel & 0xff00)>>8);
		B = (pixel & 0xff);

		a = A*3 >> 2;
		R = (R * (255 - a) + a * 0x7f) >> 8;
		G = (G * (255 - a) + a * 0x8f) >> 8;
		B = (B * (255 - a) + a * 0xaf) >> 8;
		scanline[ix+1] = ((R   )<<16) + ((G   )<<8) + (B);

		pixel = scanline[ix+2];

		R = ((pixel & 0xff0000)>>16);
		G = ((pixel & 0xff00)>>8);
		B = (pixel & 0xff);

		a = A >> 2;
		R = (R * (255 - a) + a * 0x7f) >> 8;
		G = (G * (255 - a) + a * 0x8f) >> 8;
		B = (B * (255 - a) + a * 0xaf) >> 8;
		scanline[ix+2] = ((R   )<<16) + ((G   )<<8) + (B);

		//scanline[ix-2] = 0x7f7f7f;
		//scanline[ix-1] = 0x7f7f7f;
		//scanline[ix  ] = 0x7f7f7f;
		//scanline[ix+1] = 0x7f7f7f;
		//scanline[ix+2] = 0x7f7f7f;
		
		lt += rdy;
		x += dxdy;
		scanline += j;
	}
}

static void drawRainDropV2(Vertex **VA, mword numVerts)
{
	Vertex *V = VA[0];

	int32_t x = Fist(V->PX);
	int32_t y = Fist(V->PY);

	float ahead_z = V->TPos.z;

//	float raz = 1.0 / ahead_z;
	float ahead_x = x - 3;
	float ahead_y = y + 15;
	
	if (x>=0&&x<XRes&&y>=0&&y<YRes)
	{
	
		dword Z;
		if (V->TPos.z < C_FZP)
			Z = 0xFF80 - Fist(g_zscale * V->TPos.z);
		else
			Z = 1;
		dword offset = x + y*XRes;
		if ( ((word *)(VPage+PageSize))[offset] < Z )
		{
			rainmapper(x, y, ahead_x, ahead_y);
			numDropsRendered++;
		}
		
	}
}

XMMVector evalRainVel(float curFrame) {
	const float rainVelMult = 200.0;
	XMMVector vel(2.0, -7.0, -1.0);
	vel *= rainVelMult;
	
	vel.x += sin(curFrame * 0.1) * 30.0 + sin(curFrame * 0.15) * 30.0 + sin(curFrame * 0.19) * 20.0;
	vel.z += sin(curFrame * 0.07) * 30.0 + sin(curFrame * 0.17) * 30.0 + sin(curFrame * 0.23) * 20.0;
	return vel;
}

static void initRain(Scene *Sc)
{
	rainVel = evalRainVel(0);
	Sc->NumOfParticles = NUM_RAIN_PCLS;
	Sc->Pcl = new Particle[Sc->NumOfParticles];
	//memset(Sc->Pcl,0,sizeof(Particle)*Sc->NumOfParticles);

//	memset(&dummyRainMat, 0, sizeof(Material));
	// dont use TBR for rain.
//	float typicalParticleSize = 30.0/384.0;
//	dword typicalSpans = Fist(YRes * typicalParticleSize / 4.0);
//	TBR_Init(Sc, Sc->NumOfParticles * typicalSpans);
	Vector InitialCenter = Sc->CameraHead->Source.Keys[0].Pos.im();

	std::vector<GradientEndpoint> endpoints;
	endpoints.emplace_back(0.0, Color{ 0.75, 0.65, 0.55, 0.0 });
	endpoints.emplace_back(1.0, Color{ 0.75, 0.65, 0.55, 0.6 });

	auto M = Generate_Gradient(endpoints, 256, 1, true);
	M->Flags |= Mat_SrcAlpha | Mat_Transparent;

	/*dword i,j;
	for(j=0; j<256; j++)
	{
		for(i=0; i<256; i++)		
		{
			((dword*)M->Txtr->Data)[i + (j << 8)] =
				//0xFFFFFF;				
				//(((i<<3)^(j<<3)) & 0xFF) *0x010101;
				//(i<<16)+(j<<8)+(i^j) * 0x010101;
				(i == 0 && j == 0) ? 0xffffffff : 0;
				//0xff000000 | ((((i ^ j) & 8) != 0) ? 0xffffff : 0);
				//((i>>2)&1)*0xFFFFFF;
				
		}
	}
	Sachletz((dword*)(M->Txtr->Data), 256, 256);*/
	
	for(mword i=0; i<NUM_RAIN_PCLS; i++)
	{
		Particle& p = Sc->Pcl[i];
		p.Grav = 0.0;
		p.Mass = 1.0;
		p.Charge = 0.0;
		p.Radius = 0.0;
		p.Flags = Particle_Active;
		getRadialRandomHeight(InitialCenter, p.V.Pos);

		// Particle Virtual Face -> Particle Vertex		
		p.F.FlareSize = 3.0f; 
		p.F.A = p.F.B = &p.V;
		// manual assignment
		p.F.Filler = drawRainDrop;
		p.F.Txtr = M;

		p.InitTrail(M, IX_Prefiller_TGZTAM, 50.0f, 5.0f);

		// NOTE: consider adding some randomization (per particle and over time)
		p.Vel.x = rainVel.x;
		p.Vel.y = rainVel.y;
		p.Vel.z = rainVel.z;
	}
}

static void riskOfRain(Scene *Sc)
{
	// set rainvel based on some noise function of current frame
	rainVel = evalRainVel(CurFrame);

	// Animate Particles
	float dt = 0.01*dTime;
	Particle *P, *PE;
	Vector u, v;
	mword i;

	Vector centerPred;
	Camera *Cm = Sc->CameraHead;
	mword oldKeySource = Cm->Source.CurKey;
	mword oldKeyTarget = Cm->Source.CurKey;

	//CurFrame+100 for putting rain ahead of camera
	// prepare camera positions
	const mword FramesAhead = 100;
	const mword FrameStep = 5;
	const mword NumFrames = FramesAhead/FrameStep;

	// the amount of time taken for 'FrameStep' Frames to be presented (at 25FPS)
	const float TimeStep = FrameStep / 25.0;
	
	Vector rVelStep;
	Vector_Scale(&rainVel, TimeStep, &rVelStep);
	
	Vector cPos[NumFrames];
	Matrix cMat[NumFrames];
	Spline_Calc_3D(&Cm->Source, CurFrame+50.0, &centerPred);
	// predict if raindrop is seen or not
	for(i=0; i<NumFrames; i++)
	{
		Spline_Calc_3D(&Cm->Source, CurFrame + (i+1) * FrameStep, &cPos[i]);
		Spline_Calc_3D(&Cm->Target, CurFrame + (i+1) * FrameStep, &v);
		Kick_Camera(&cPos[i], &v, 0.0, cMat[i]);
	}
	Cm->Source.CurKey = oldKeySource;
	Cm->Target.CurKey = oldKeyTarget;
	for(P=Sc->Pcl, PE = P + Sc->NumOfParticles; P<PE; P++)
	{
//		if (!P->Flags) continue;
		Vector_Scale(&P->Vel,dt, &v);
		Vector_SelfAdd(&P->V.Pos, &v);
		if (P->V.Pos.y < 0.0f)
		{
			P->Vel.x = rainVel.x;
			P->Vel.y = rainVel.y;
			P->Vel.z = rainVel.z;
			P->V.Pos.y += rainMaxHeight;
			for(i=0; i<100; i++)
			{
				getRadial(centerPred, P->V.Pos);
				Vector rPos;
				Vector_Copy(&rPos, &P->V.Pos);
				for(mword f=0; f<NumFrames; f++)
				{
					Vector_SelfAdd(&rPos, &rVelStep);
					Vector_Sub(&rPos, &cPos[f], &u);
					MatrixXVector(cMat[f], &u, &v);
					if (v.z<C_NZP || v.z>C_FZP*0.8f) continue;
					float rz = 1.0/v.z;
					if (fabs(v.x)*rz*FOVX > CntrX) continue;
					if (fabs(v.y)*rz*FOVY > CntrY) continue;
					goto BailOut;
				}
			}
BailOut:;
		}

		Vector_Sub(&P->V.Pos,&View->ISource,&v);
		MatrixXVector(View->Mat, &v, &P->V.TPos);

		if (P->V.TPos.z>=Sc->NZP)
		{
			P->V.RZ = 1.0/P->V.TPos.z;
			P->V.PX = CntrX+FOVX*P->V.TPos.x*P->V.RZ;
			P->V.PY = CntrY-FOVY*P->V.TPos.y*P->V.RZ;
//      P->PRad = 0;//FOVX*P->Radius*RZ;
			P->V.Flags = 0;
		} else P->V.Flags |= Vtx_VisNear;
	}

}

static dword V_Create(VESA_Surface* VS)
{
	VS->CPP = (VS->BPP + 1) >> 3;
	VS->BPSL = VS->CPP * VS->X;
	VS->PageSize = VS->BPSL * VS->Y;

	dword ZBufferSize = sizeof(word) * VS->X * VS->Y;
	if (!(VS->Data = (byte*)malloc(VS->PageSize + ZBufferSize))) return 1;
	memset(VS->Data, 0, VS->PageSize + ZBufferSize);

	//	VS->Flags = VSurf_Exists;
	
	VS->Handle = 0;

	return 0;
}

struct CubeMapCoord {
	uint32_t textureIdx : 3;
	uint32_t u : 10;
	uint32_t v : 10;

	void set(uint32_t textureIdx, float u, float v) {
		this->textureIdx = textureIdx;
		this->u = uint32_t(u * 1023.999);
		this->v = uint32_t(v * 1023.999);
	}

	uint32_t getTextureIdx() const {
		return textureIdx;
	}
	float getU() const {
		return u / 1023.999;
	}
	float getV() const {
		return v / 1023.999;
	}
};

using std::min;
using std::max;

std::vector<CubeMapCoord> CalcEquirectangularPanoramaTable(int xRes, int yRes) {
	std::vector<CubeMapCoord> res(xRes * yRes);
	auto it = res.begin();
	for (int y = 0; y != yRes; ++y) {
		const float lat = PI * (float(y) / yRes) - PI / 2.0f;
		Vector d;
		d.y = sin(lat);
		const float xzFactor = cos(lat);
		for (int x = 0; x != xRes; ++x, ++it) {
			const float lon = 2.0 * PI * (float(x) / xRes);
			d.x = xzFactor * sin(lon);
			d.z = xzFactor * cos(lon);
			const int ROT = 2;
			if (d.z > max(fabs(d.x), fabs(d.y))) {
				const float u = ((d.x / d.z) + 1.0) / 2.0;
				const float v = ((d.y / d.z) + 1.0) / 2.0;
				it->set(ROT % 4,1-u, v);
			} else if (d.x > max(fabs(d.y), fabs(d.z))) {
				const float u = ((d.z / d.x) + 1.0) / 2.0;
				const float v = ((d.y / d.x) + 1.0) / 2.0;
				it->set((ROT + 1) % 4, u, v);
			} else if (d.z < -max(fabs(d.x), fabs(d.y))) {
				const float u = ((d.x / d.z) + 1.0) / 2.0;
				const float v = ((d.y / d.z) + 1.0) / 2.0;
				it->set((ROT + 2) % 4,1-u,1-v);
			} else if (d.x < -max(fabs(d.y), fabs(d.z))) {
				const float u = ((d.z / d.x) + 1.0) / 2.0;
				const float v = ((d.y / d.x) + 1.0) / 2.0;
				it->set((ROT + 3) % 4,u,1-v);
			} else if (d.y > max(fabs(d.x), fabs(d.z))) {
				const float u = ((d.x / d.y) + 1.0) / 2.0;
				const float v = ((d.z / d.y) + 1.0) / 2.0;
				it->set(4, u, v);
			} else if (d.y < -max(fabs(d.x), fabs(d.z))) {
				const float u = ((d.x / d.y) + 1.0) / 2.0;
				const float v = ((d.z / d.y) + 1.0) / 2.0;
				it->set(5, 1 - u, v);
			}
		}
	}
	return res;
}

int32_t iLog2(int32_t x)
{
	union fi {
		uint32_t i;
		float f;
	};

	fi u{.f =(float)x};

	return ((u.i >> 23) - 127);
}

Material* Materialize(void* data, int x, int y) {
	Material* M = getAlignedType<Material>(16); //(Material*)getAlignedBlock(sizeof(Material), 16);
	// whatever that means
	M->Txtr = new Texture;
	memset(M->Txtr, 0, sizeof(Texture));
	M->Txtr->Flags = Txtr_Nomip | Txtr_Tiled;
	M->Txtr->Data = (byte *)_aligned_malloc(x * y * 4, 16);
	memcpy(M->Txtr->Data, data, x * y * 4);

	M->Txtr->BPP = 32;
	M->Txtr->SizeX = x; M->Txtr->LSizeX = iLog2(x);
	M->Txtr->SizeY = y; M->Txtr->LSizeY = iLog2(y);

	Sachletz((dword*)(M->Txtr->Data), x, y);
	M->Txtr->Mipmap[0] = M->Txtr->Data;
	M->Txtr->numMipmaps = 1;

	return M;
}

using std::pair;
using std::vector;
using std::map;

pair <Vertex*, Vertex*> make_vertex_pair(Vertex *A, Vertex *B) {
	return (A < B) ? std::make_pair(A, B) : std::make_pair(B, A);
}

Vertex* get_non_edge_vertex(pair <Vertex*, Vertex*> E, Face* F) {
	if (F->A != E.first && F->A != E.second) {
		return F->A;
	}
	if (F->B != E.first && F->B != E.second) {
		return F->B;
	}
	return F->C;
}

Vector ComputeFaceNormal(Vertex *A, Vertex* B, Vertex *C) {
	Vector U, V, N;
	U = B->Pos - A->Pos;
	V = C->Pos - A->Pos;
	N = V.cross(U);
	N.normalize();

	return N;
}



void Initialize_City()
{
	CitySc = (Scene *)getAlignedBlock(sizeof(Scene), 16);
	memset(CitySc,0,sizeof(Scene));
	LoadFLD(CitySc,"SCENES/CITY.FLD");
//	CitySc->EndFrame = 1046.0f;
	CTPartTime = 100.0f*(CitySc->EndFrame-CitySc->StartFrame)/50.0f;
//	printf("FLD-loaded MEM = %d\n",DPMI_Free_Memory());
	CitySc->NZP = 20.0;
	CitySc->FZP = 7500.0f;
	CitySc->Flags |= Scn_Fogged|Scn_ZBuffer;
	CitySc->Ambient.B = 32*2;
	CitySc->Ambient.G = 48*2;
	CitySc->Ambient.R = 64*2;
	
	Material *M;
	for(M = MatLib;M;M=M->Next)
	{
		if (M->RelScene != CitySc) continue;
		if (!M->Txtr) continue;
		M->Flags |= Mat_RGBInterp;
		M->Txtr->Flags |= Txtr_Tiled;
	}

	Reflective_Surface_Setup();
	AddCameraOmni(); 

	//SceneCorrections();


	Preprocess_Scene(CitySc);

	//for(M = MatLib;M;M=M->Next)
	//{
	//	if (M->RelScene != CitySc) continue;
	//	if (!M->Txtr) continue;
	//}

	const auto PANORAMA_XRES = 1024;
	const auto PANORAMA_YRES = 1024;

	auto cubeMapper = CalcEquirectangularPanoramaTable(PANORAMA_XRES, PANORAMA_YRES);
	
	//	printf("Scene-Proc MEM = %d\n",DPMI_Free_Memory());
	// also make the appropriate Layer 2 fillers,
	// when fog will be implemented
	Init_Distort();

	SkySc = CreateSkyCube(0);

	// TODO: should animate the object to the frame where the camera is nearest to it
	CurFrame = 0;
	View = CitySc->CameraHead;

	const auto CUBE_MAP_XRES = 1024;
	const auto CUBE_MAP_YRES = 1024;

	// render env maps for a few select objects
	FList_Allocate(CitySc);
	auto oldAspectRatio = AspectRatio;
	// HACK: using exactly 1 triggers frustrum clipper bugs
	AspectRatio = 0.999;
	Camera EnvCam = { 0 };
	View = &EnvCam;
	EnvCam.IFOV = 90.0;
	CalcPersp(&EnvCam);
	//EnvCam.PerspX = CUBE_MAP_XRES / 2;
	//EnvCam.PerspY = CUBE_MAP_YRES / 2;
	FOVX = View->PerspX;
	FOVY = View->PerspY;

	Animate_Objects(CitySc, true);
	Lighting(CitySc);

	VESA_Surface* PrevMainSurf = MainSurf;
	VESA_Surface TmpSurf = { 0 };
	MainSurf = &TmpSurf;
	TmpSurf.X = CUBE_MAP_XRES;
	TmpSurf.Y = CUBE_MAP_YRES;
	TmpSurf.BPP = 32;
	TmpSurf.Flip = PrevMainSurf->Flip;
	VESA_BPSL = CUBE_MAP_XRES * 4;
	V_Create(&TmpSurf);
	VESA_VPageExternal(&TmpSurf);

	SetCurrentScene(CitySc);

	const auto OmniFactor = 4.0;

	for (auto O = CitySc->OmniHead; O; O = O->Next) {
		O->ISize /= OmniFactor;
	}

	int building_count = 0;
	bool bailout = false;

	std::vector<uint32_t> cubeMaps[6];
	for (auto& cubeMap : cubeMaps) {
		cubeMap.resize(CUBE_MAP_XRES * CUBE_MAP_YRES);
	}

	std::vector<uint32_t> panorama(PANORAMA_XRES * PANORAMA_YRES);

	for (Object* Obj = CitySc->ObjectHead; Obj != nullptr; Obj = Obj->Next) {
		if (Obj->Type != Obj_TriMesh) { continue; }
		if (Obj->Name == nullptr) { continue; }
		if (!(Obj->Name[0] == 'b' && '0' <= Obj->Name[1] && Obj->Name[1] <= '9' && 0 == strcmp(Obj->Name + 2, ".lwo"))) { continue; }

		building_count++;
		auto T = (TriMesh*)(Obj->Data);
		auto flags = T->Flags;
		T->Flags &= ~HTrack_Visible;

		EnvCam.ISource = T->IPos + T->BSphereCtr;
		for (int i = 0; i != 6; ++i) {
			FastWrite(VPage, 0, (PageSize + XRes * YRes * sizeof(word)) >> 2);
			memcpy(EnvCam.Mat, AxisAlignedViews[i], sizeof(EnvCam.Mat));

			RenderSkyCube(SkySc, View, true);
			Reflected_Transform(CitySc);
			if (0 != CAll) {
				Radix_SortingASM(FList, SList, CAll);
				Render();
			}
			Transform_Objects(CitySc);
			if (0 != CAll) {
				Radix_SortingASM(FList, SList, CAll);
				Render();
			}
			//if (!bailout) {
			//	if (Keyboard[ScESC]) {
			//		bailout = true;
			//	}
			//	Flip(&TmpSurf);
			//	while (!Keyboard[ScSpace]) {
			//	}
			//	while (Keyboard[ScSpace]) {
			//	}
			//	if (Keyboard[ScESC]) {
			//		bailout = true;
			//	}
			//}
			memcpy(cubeMaps[i].data(), TmpSurf.Data, CUBE_MAP_XRES * CUBE_MAP_YRES * 4);
		}
		T->Flags = flags;

		auto outputIt = panorama.begin();
		auto mapperIt = cubeMapper.begin();
		for (int y = 0; y != PANORAMA_YRES; ++y) {
			for (int x = 0; x != PANORAMA_XRES; ++x, ++mapperIt, ++outputIt) {
				const int u = int(mapperIt->getU() * CUBE_MAP_XRES);
				const int v = int(mapperIt->getV() * CUBE_MAP_YRES);
				*outputIt = cubeMaps[mapperIt->getTextureIdx()][u + v * CUBE_MAP_XRES];
			}
		}

		Obj->Reflection = Materialize(panorama.data(), PANORAMA_XRES, PANORAMA_YRES);
		// HACK: works because panorma size = cube
		//memcpy(TmpSurf.Data, panorama.data(), PANORAMA_XRES * PANORAMA_YRES * 4);
		//		
		//if (!bailout) {
		//	if (Keyboard[ScESC]) {
		//		bailout = true;
		//	}
		//	Flip(&TmpSurf);
		//	while (!Keyboard[ScSpace]) {
		//	}
		//	while (Keyboard[ScSpace]) {
		//	}
		//	if (Keyboard[ScESC]) {
		//		bailout = true;
		//	}
		//}
	}

	for (Object* Obj = CitySc->ObjectHead; Obj != nullptr; Obj = Obj->Next) {
		if (Obj->Type != Obj_TriMesh) { continue; }
		if (Obj->Name == nullptr) { continue; }
		if (!(Obj->Name[0] == 'b' && '0' <= Obj->Name[1] && Obj->Name[1] <= '9' && 0 == strcmp(Obj->Name + 2, ".lwo"))) { continue; }

		auto T = (TriMesh*)(Obj->Data);
		for (Face* F = T->Faces, *FE = T->Faces + T->FIndex; F != FE; ++F) {
			if (strstr(F->Txtr->Name, "windows")) {
				F->Flags |= Face_Reflective;
				F->ReflectionTexture = Obj->Reflection->Txtr;
			}
		}
	}
	AspectRatio = oldAspectRatio;

	for (auto O = CitySc->OmniHead; O; O = O->Next) {
		O->ISize *= OmniFactor;
	}

	using edge_list_t = map<pair<Vertex*, Vertex*>, vector<Face*>>;


	/*for (Object* Obj = CitySc->ObjectHead; Obj != nullptr; Obj = Obj->Next) {
		if (Obj->Type != Obj_TriMesh) { continue; }
		if (Obj->Name == nullptr) { continue; }
		if (!(Obj->Name[0] == 'b' && '0' <= Obj->Name[1] && Obj->Name[1] <= '9' && 0 == strcmp(Obj->Name + 2, ".lwo"))) { continue; }

		edge_list_t edge_list;

		auto T = (TriMesh*)(Obj->Data);
		for (Face* F = T->Faces, *FE = T->Faces + T->FIndex; F != FE; ++F) {
			if (!(F->Flags & Face_Reflective)) { continue; }
			edge_list[make_vertex_pair(F->A, F->B)].push_back(F);
			edge_list[make_vertex_pair(F->B, F->C)].push_back(F);
			edge_list[make_vertex_pair(F->C, F->A)].push_back(F);
		}

		Face* NewFaces = new Face[T->FIndex];
		Face* NewFacePtr = NewFaces;


		for (const auto& [edge, faces] : edge_list) {
			if (faces.size() != 2 ||
				faces[0]->A == nullptr || faces[0]->D != nullptr ||
				faces[1]->A == nullptr || faces[1]->D != nullptr) {
				continue;
			}

			if (faces[0]->N * faces[1]->N >= 0.9999) {
				// We have a winner!
				Vertex* v[4] = { get_non_edge_vertex(edge, faces[0]), edge.first, get_non_edge_vertex(edge, faces[1]), edge.second };
				auto N = ComputeFaceNormal(v[0], v[1], v[2]);
				if (N * faces[0]->N < 0.0) {
					std::swap(v[0], v[2]);
				}

				faces[0]->uvToVertices();
				faces[1]->uvToVertices();

				faces[0]->A = v[0];
				faces[0]->B = v[1];
				faces[0]->C = v[2];
				faces[0]->D = v[3];
				faces[0]->uvFromVertices();
				faces[1]->A = nullptr;
			}
		}
		T->FIndex = std::remove_if(T->Faces, T->Faces + T->FIndex, [](const auto &F) {return !F.A; }) - T->Faces;
	}*/

	MainSurf = PrevMainSurf;
	VESA_Surface2Global(MainSurf);

	// initialize rain FX
#ifdef CITY_RAIN
	initRain(CitySc);
#endif
}

static void TextureBlockTest()
{
	// look for a block tiled texture
	static int numTexture = 1;
	static int delay = 10;
	if (delay>0)
		delay--;
	else {
		delay = 10;
		numTexture++;
	}

	int scanTexture = numTexture;
	Material *M;
	for(M = MatLib; M; M=M->Next)
	{
		if (M->RelScene == CitySc && M->Txtr->Flags & Txtr_Tiled)
		{
			if (--scanTexture == 0)
				break;
		}
	}
	if (!M) return;

	Texture *Tx = M->Txtr;
	dword *mip = ((dword *)Tx->Mipmap[0]);
	for(mword b=0; b<256; b++)
	{
		float
			f_u = 0.0,
			f_v = b;

		dword i_u = (dword)(f_u * 65536.0);
		dword i_v = (dword)(f_v * 65536.0);

		dword u = ((i_u & 0x0000fffe) >> 1) |
				   (i_u & 0x00030000) |
				  ((i_u & 0x000fc000) << 8);

		dword v = ((i_v & 0x0000fffe) >> 1) |
				  ((i_v & 0x00ff0000) << 2);

		float
			f_du = 1.0,
			f_dv = 0.0;

		dword i_du = (dword)(f_du * 65536.0);
		dword i_dv = (dword)(f_dv * 65536.0);

		dword du = ((i_du & 0x0000fffe) >> 1) |
				    (i_du & 0x00030000) |
				   ((i_du & 0x00fc0000) << 8) |
				    0x03fc8000;

		dword dv = ((i_dv & 0x0000fffe) >> 1) |
				   ((i_dv & 0x00ff0000) << 2) |
				    0x00038000;

		for(mword a=0; a<256; a++)
		{
			mword x = a;
			mword y = b;
			mword numBlock = (y>>2) + ((x>>2)<<6);

			u	+=	du;
			v	+=	dv;
			u	&=	0xfc037fff;
			v	&=	0x03fc7fff;

			mword offset = (u+v)>>16;

			((dword *)VPage) [a + b*XRes] = 
//				mip [((x&3)+((y&3)<<2)) + (numBlock<<4)];
				mip [offset];
		}
	}
}
#ifdef TRACE_OBJECTS
struct debug_outtext {
	dword x, y;
	std::string str;
};

std::vector<debug_outtext> DebugStrs;

#endif

void Run_City()
{
	int32_t Polys = 0,TTrd;
	TriMesh *T;
	Omni *O;

	SetCurrentScene(CitySc);

	// get the reflective water surface object
	Reflective_Mapper_Setup();
	Setup_Water_Distort();
//	Setup_Grid_Texture_Mapper_XXX(256, 256);

	// scatter point lights.
	/*for(O=CitySc->OmniHead;O;O=O->Next)
	{
		Quaternion_Form(&O->Pos.Keys[0].Pos, 
			((RAND_15()-16384)*5000.0)/16384.0,
			(RAND_15()*2000.0)/32768.0,
			((RAND_15()-16384)*5000.0)/16384.0,
			0);
	}*/


	for(T = CitySc->TriMeshHead;T;T=T->Next)
		Polys+=T->FIndex;
	for(O = CitySc->OmniHead;O;O=O->Next)
		Polys++;

	//MusicModuleInfo mmi;
	//mmi.ModuleHandle = g_RevModuleHandle;

	FList = new Face * [Polys];
	SList = new Face * [Polys];

	View = CitySc->CameraHead;
//	View = &FC;

	int32_t i, timerStack[20], timerIndex = 0;
	for(i=0; i<20; i++)
		timerStack[i] = Timer;

	C_FZP = CitySc->FZP;
	C_rFZP = 1.0f/C_FZP;
	//C_reFZP = 1.0f/(C_FZP*1.15f);

	g_FrameTime = TTrd = Timer;

//  old lighting model
//	Ambient_Factor = 32;
//	Diffusive_Factor = 10000000;//80 000 000;

	Ambient_Factor = 0.25;
	Diffusive_Factor = 1.0;
	Specular_Factor = 0.0;
	Range_Factor = 1.3;


	ImageSize = 200;
	char MSGStr[128];

	// SYNC music: set order #7
	//mmi.Order = 7;
	//FModSetModuleInfo(mmi);

	Modplayer_SetOrder(g_RevModuleHandle, 7);

	dword numFrames = 0;

	//dword avgPolys = 0;
	g_renderedPolys = 0;
	numDropsRendered = 0;

	FillerPixelcount = 0;

	dword Profiler[PROF_NUM];
	float ProfSample[PROF_NUM];
	float ProfPerc[PROF_NUM];
	const char *ProfNames[] = {"ZCLR","ANIM","XFRM", "LGHT", "SORT", "RNDR", "FLIP"};
	dword ProfSum = 0;

	std::vector<int> dispMap;
	dispMap.resize(XRes * YRes);

	//float waves[10][2];
	//for (int w = 0; w != 10; ++w) {
	//	waves[w][0] = (1.0 / 50.0) * (2 * (RAND_15() / 32768.0f) - 1);
	//	waves[w][1] = (1.0 / 50.0) * (2 * (RAND_15() / 32768.0f) - 1);
	//	waves[w][2] = (1.0 / 50.0) * (2 * (RAND_15() / 32768.0f) - 1);
	//	waves[w][3] = (1.0 / 50.0) * (2 * (RAND_15() / 32768.0f) - 1);
	//}
	int idx = 0;
	for (int y = 0; y < YRes; ++y) {
		for (int x = 0; x < XRes; ++x) {
			int tx = x, ty = y;

			//for (int w = 0; w != 10; ++w) {
			//	tx += sin(x * waves[w][0] + y * waves[w][1]) * 3.5;
			//	ty += cos(x * waves[w][0] + y * waves[w][1]) * 3.5;
			//}
			tx += sin(x/30.0) * 15;
			ty += cos(y/50.0) * 15;

			if (tx < 0) { tx = -tx; } if (tx > XRes_1) { tx = 2 * XRes_1 - tx; }
			if (ty < 0) { ty = -ty; } if (ty > YRes_1) { ty = 2 * YRes_1 - ty; }
			dispMap[idx++] = ty * XRes + tx;
		}
	}
	std::vector<dword> backBuffer;
	backBuffer.resize(XRes * YRes);

	for(i=0; i<PROF_NUM; i++)
		Profiler[i] = 0;	

	while ((!Keyboard[ScESC]) && Timer < CTPartTime)
	{
		numFrames++;

		Profiler[PROF_ZCLR] -= Timer;

		dTime = Timer - TTrd;
		if (dTime > 300) dTime = 300;
		// fast forward/rewind
		if (Keyboard[ScF2])
		{
			Timer += dTime * 8;
		}
		if (Keyboard[ScF1])
		{
			if (dTime * 8 > Timer)
				Timer = 0;
			else
				Timer -= dTime * 8;
		}
		static bool pause_mode = false;
		if (Keyboard[ScP])
			pause_mode = true;
		if (Keyboard[ScU])
			pause_mode = false;
		if (pause_mode)
		{
			Timer = TTrd;
		}
		g_FrameTime = TTrd = Timer;
		CurFrame = CitySc->StartFrame + (CitySc->EndFrame - CitySc->StartFrame) * (float)g_FrameTime / (float)CTPartTime;
		if (Keyboard[ScTab]) {
			if (View == &FC)
				View = CitySc->CameraHead;
			else
				View = &FC;
		}
		Dynamic_Camera();

		// Clear framebuffer and Z-buffer
		//memset(VPage,0,PageSize + XRes*YRes*sizeof(word));
		FastWrite(VPage, 0, (PageSize + XRes * YRes * sizeof(word)) >> 2);
		RenderSkyCube(SkySc, View, false);
//		FastWrite(VPage + PageSize, 0, (XRes * YRes * sizeof(word)) >> 2);

		Profiler[PROF_ZCLR] += Timer;
		Profiler[PROF_ANIM] -= Timer;

		Reflective_AnimateTexture();
		//		Water_Distort();

		//							 CitySc->StartFrame + 500 * (float)Timer / (float)CTPartTime;

		if (Keyboard[ScC]) { FC.ISource = View->ISource; Matrix_Copy(FC.Mat, View->Mat); FC.IFOV = View->IFOV; }

		Animate_Objects(CitySc);

		((TriMesh*)(RflObj->Data))->Flags &= 0xFFFFFFFF - HTrack_Visible;

		Profiler[PROF_ANIM] += Timer;
		Profiler[PROF_LGHT] -= Timer;

		Lighting(CitySc);
		//City_Lighting();

		// NOTE: Profiler ignores rain f/x
#ifdef CITY_RAIN
		riskOfRain(CitySc);
#endif
		Profiler[PROF_LGHT] += Timer;
		Profiler[PROF_XFRM] -= Timer;

		// render reflection of all objects except water
		Reflected_Transform(CitySc);


		Profiler[PROF_XFRM] += Timer;

		if (!CAll) goto norefl;
		Profiler[PROF_SORT] -= Timer;

		Radix_SortingASM(FList, SList, CAll);

		Profiler[PROF_SORT] += Timer;
		Profiler[PROF_RNDR] -= Timer;

		Render();
		{
			memcpy(backBuffer.data(), VPage, PageSize);
			int ii = 0;
			dword* ptr = (dword*)VPage;
			uint16_t* zbuffer_ptr = (uint16_t*)(VPage + PageSize);
			for (auto displacement : dispMap) {
				if (*zbuffer_ptr++ != 0) {
					*ptr = backBuffer[displacement];
				}
				ptr++;
			}
		}


		Profiler[PROF_RNDR] += Timer;

		//Reflective_Distortion();
		//Run_Distort();
		// Clear framebuffer and Z-buffer
		//memset(VPage,0,PageSize + XRes*YRes*sizeof(word));		
//		FastWrite(VPage+PageSize, 0, (XRes*YRes*sizeof(word)) >> 2);

	norefl:

		// render all objects normally
		Profiler[PROF_XFRM] -= Timer;
		((TriMesh*)(RflObj->Data))->Flags |= HTrack_Visible;
		Transform_Objects(CitySc);
		Profiler[PROF_XFRM] += Timer;
		if (!CAll) continue;
		Profiler[PROF_SORT] -= Timer;
		Radix_SortingASM(FList, SList, CAll);
		Profiler[PROF_SORT] += Timer;
		Profiler[PROF_RNDR] -= Timer;
		Render();

		Profiler[PROF_RNDR] += Timer;

		// FPS printer
		if (g_profilerActive)
		{
			mword scroll = 0;

			timerStack[timerIndex++] = Timer;
			if (timerIndex == 20)
			{
				timerIndex = 0;
				sprintf(MSGStr, "%f FPS", 2000.0 / (float)(timerStack[19] - timerStack[timerIndex]));
			}
			else {
				sprintf(MSGStr, "%f FPS", 2000.0 / (float)(timerStack[timerIndex - 1] - timerStack[timerIndex]));
			}
			scroll = OutTextXY(VPage, 0, scroll, MSGStr, 255);

			sprintf(MSGStr, "%f frame", CurFrame);
			scroll = OutTextXY(VPage, 0, scroll + 15, MSGStr, 255);
			sprintf(MSGStr, "%.3lfM pixels/frame", (FillerPixelcount / (1000000.0 * numFrames)));
			scroll = OutTextXY(VPage, 0, scroll + 15, MSGStr, 255);
			sprintf(MSGStr, "%.3lfM pixels/second", (FillerPixelcount / 1000000.0 * 100.0 / Profiler[PROF_RNDR]));
			scroll = OutTextXY(VPage, 0, scroll + 15, MSGStr, 255);
			sprintf(MSGStr, "%d polys/frame", (int32_t)(g_renderedPolys / numFrames));
			scroll = OutTextXY(VPage, 0, scroll + 15, MSGStr, 255);
			//		numDropsRendered=0;
			//		sprintf(MSGStr, "%d raindrops" , (int32_t)(numDropsRendered) );
			//		OutTextXY(VPage,0,60,MSGStr,255);

			//		FModGetModuleInfo(mmi);
			//		sprintf(MSGStr, "Order: %d, Row: %d", mmi.Order, mmi.Row);
			//		OutTextXY(VPage,0,15,MSGStr,255);
			//		sprintf(MSGStr, "%dK pixels/second" , (int32_t)(FillerPixelcount/1000.0 * 100.0 / Timer));
			//		OutTextXY(VPage,0,30,MSGStr,255);
			//		PROFILER: display stats		
			for (i = 0; i < PROF_NUM; i++)
			{
				sprintf(MSGStr, "%s %3.1fms (%3.1f%%)", ProfNames[i], ProfSample[i], ProfPerc[i]);
				scroll = OutTextXY(VPage, 0, scroll + 15, MSGStr, 255);
			}

			sprintf(MSGStr, "TOTL %3.1fms", ProfSum * 10.0 / numFrames);
			scroll = OutTextXY(VPage, 0, scroll + 15, MSGStr, 255);

		}
		Profiler[PROF_FLIP] -= Timer;

#ifdef TRACE_OBJECTS
		for (const auto& str : DebugStrs) {
			OutTextXY(VPage, str.x, str.y, str.str.c_str(), 255);
		}
		DebugStrs.clear();
#endif

		Flip(MainSurf);
		Profiler[PROF_FLIP] += Timer;

		//		PROFILER: Generate sample
		ProfSum = 0;
		for (i = 0; i < PROF_NUM; i++)
			ProfSum += Profiler[i];
		for (i = 0; i < PROF_NUM; i++)
		{
			ProfSample[i] =
				Profiler[i] * 10.0 / numFrames;
			ProfPerc[i] =
				Profiler[i] * 100.0 / ProfSum;
		}

		//if (dTime<5) SysSleep(10*(5-dTime));
	} Timer -= CTPartTime;
/*	if (Keyboard[ScESC])
	{
		#ifdef Play_Music
		ShutDown();
		#endif
		FDS_End();
		exit(-1);
	}*/
	while (Keyboard[ScESC]) continue;
	if (Timer < 0)
		Timer = 0;
	delete FList;
	delete SList;
	Destroy_Scene(CitySc);
}
