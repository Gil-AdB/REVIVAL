
#include "Rev.h"
#include <Base/RenderStats.h>
#include <Threads.h>
#include <Base/FeatureFlags.h>
#include <FILLERS/TheOtherBarry.h>
#include <FILLERS/ShadowMap.h>
#include "CITY.H"
#include "CityPanoramaCache.h"
#include "FrameProfiler.h"
#include "MeshOps.h"
#include "ProceduralWater.h"
#include "SceneTick.h"
#include "Scenes.h"
#include "SpotlightCones.h"
#include <algorithm>
#include <memory>
#include <vector>
#include "FRUSTRUM.H"
#include <Base/VertexFrame.h>  // Reflected_Transform dual-write to T->frame

// PREPROC.CPP — stamp F->A/B/C_idx from F->A/B/C pointer offsets into
// T->Verts. Forward-declared locally; called after any post-init Face
// rebuild (tessellateWaterGrid) so SoA consumers index the right slot.
void Compute_FaceVertexIndices(TriMesh *T);
#include "Gradient.h"
#include <map>

#define FRONT_TO_BACK_SORTING
// Rain particles render through a dedicated mapper (`drawRainDrop`) outside
// the regular tile rasterizer pipeline. Disabling for now while the
// deferred path stabilizes — rain on top of the deferred-lit city was
// distracting and the special-case rasterizer doesn't go through Mekalele.
// #define CITY_RAIN

static Scene *CitySc;
Scene *SkySc;
static Object *RflObj;
static Vector RflSurfNorm(0,1,0);
static float RflSurfOfs = 0;
// World-space extent + plane height of the water grid (set in tessellateWaterGrid).
// Used by the water-bump pass to ray-cast the water plane directly (peel-robust),
// rather than reading the transparent G-buffer (whose front layer doesn't hold
// the water under a multi-pass xpar peel).
static float g_waterMinX = 0, g_waterMaxX = 0, g_waterMinZ = 0, g_waterMaxZ = 0, g_waterY = 0;

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





// Compute the reflective surface's plane (normal + offset) from the first
// face of the water mesh, force-scale the mesh and its UVs to a quarter
// of authored size, and pin Pos.Keys[0] to the origin. Caller is
// Reflective_Surface_Setup; populates RflObj/RflSurfNorm/RflSurfOfs.
static void prepareWaterSurface(Object *waterObj, TriMesh *T)
{
	Vector U, V;
	RflObj = waterObj;
	// patch: modify keys to position zero
	Quaternion &P = T->Pos.Keys[0].Pos;
	P.x = P.y = P.z = 0.0f;

	Face *F = T->Faces;
	Vector_Sub(&F->B->Pos, &F->A->Pos, &U);
	Vector_Sub(&F->C->Pos, &F->A->Pos, &V);
	Cross_Product(&U, &V, &RflSurfNorm);
	Vector_Norm(&RflSurfNorm);

	Vector tmp = T->Pos.Keys[0].Pos.im();
	Vector_Add(&F->A->Pos, &tmp, &U);
	RflSurfOfs = -Dot_Product(&RflSurfNorm, &U);

	// Tint the UVs to a quarter (texture tiles 4x across the surface) and
	// mark the material transparent so the reflected pass blends in.
	Face *FE = T->Faces + T->FIndex;
	for (F = T->Faces; F < FE; ++F) {
		F->U1 *= 0.25; F->V1 *= 0.25;
		F->U2 *= 0.25; F->V2 *= 0.25;
		F->U3 *= 0.25; F->V3 *= 0.25;
		F->Txtr->Flags |= Mat_Transparent;
	}
	// Quarter-scale the vertex positions to match the UV scale.
	Vertex *VE = T->Verts + T->VIndex;
	for (Vertex *Vtx = T->Verts; Vtx < VE; ++Vtx)
		Vector_SelfScale(&Vtx->Pos, 0.25);
}

// Replace the water mesh's authored geometry with a regular NumSections x
// NumSections grid spanning its bounding rectangle in the XZ plane. Helps
// the affine gouraud / xpar shader produce smooth interpolation. Assumes
// RflSurfNorm = (0, 1, 0) — i.e. the water surface is aligned to XZ and
// the UV mapping is a scalar transform onto XZ with no rotation.
// Low-frequency UV domain warp for water de-tiling. Displaces the texture
// mapping by a smooth function of world XZ so the tiled albedo stretches/swirls
// across the surface instead of repeating as a regular grid. Applied to the
// FACE UVs (which the rasterizer consumes); the rasterizer then swizzles the
// warped coords correctly — no manual swizzle math. Per shared vertex position
// → consistent across adjacent faces (no seams). Static (baked at init); the
// per-frame scroll rides on top as a uniform offset, preserving the warp.
static inline float waterDetileU(const Vector& p, float amp, float f) {
	return amp * (std::sin(p.x*f + p.z*f*0.7f) + 0.5f*std::sin(p.x*f*1.9f - p.z*f*1.3f));
}
static inline float waterDetileV(const Vector& p, float amp, float f) {
	return amp * (std::cos(p.z*f + p.x*f*0.6f) + 0.5f*std::cos(p.z*f*1.7f - p.x*f*1.1f));
}

// The procedural water field + caustic texture + screen-space glint pass were
// factored into DEMO/ProceduralWater.{h,cpp} so chase can share them. City owns
// only its water PLANE (g_waterMin/Max/Y, set in tessellateWaterGrid) and feeds
// it to pwater::RenderGlints / pwater::WaveSlope.

static void tessellateWaterGrid(TriMesh *T, dword NumSections)
{
	// Extract coverage (range) over the XZ plane and over texture UV
	// (texture coords come off vertices, not faces).
	float mx = T->Verts[0].Pos.x, Mx = mx;
	float mz = T->Verts[0].Pos.z, Mz = mz;
	float mu = T->Verts[0].U,      Mu = mu;
	float mv = T->Verts[0].V,      Mv = mv;
	const float y = T->Verts[0].Pos.y;
	Material *RMat = T->Faces[0].Txtr;
	for (dword i = 1; i < T->VIndex; ++i) {
		const float x = T->Verts[i].Pos.x;
		const float z = T->Verts[i].Pos.z;
		if (x < mx) mx = x; if (x > Mx) Mx = x;
		if (z < mz) mz = z; if (z > Mz) Mz = z;
		const float u = T->Verts[i].U;
		const float v = T->Verts[i].V;
		if (u < mu) mu = u; if (u > Mu) Mu = u;
		if (v < mv) mv = v; if (v > Mv) Mv = v;
	}
	// Publish the water grid's world XZ extent + plane height for the bump pass.
	g_waterMinX = mx; g_waterMaxX = Mx; g_waterMinZ = mz; g_waterMaxZ = Mz; g_waterY = y;

	delete [] T->Verts;
	delete [] T->Faces;
	T->VIndex = (NumSections + 1) * (NumSections + 1);
	T->FIndex = NumSections * NumSections * 2;
	T->Verts  = new Vertex [T->VIndex];
	T->Faces  = new Face   [T->FIndex];
	memset(T->Verts, 0, sizeof(Vertex) * T->VIndex);
	memset(T->Faces, 0, sizeof(Face)   * T->FIndex);

	const float dx = (Mx - mx) / float(NumSections);
	const float dz = (Mz - mz) / float(NumSections);
	const float du = (Mu - mu) / float(NumSections);
	const float dv = (Mv - mv) / float(NumSections);

	Vertex *Vtx = T->Verts;
	float v = mv, z = mz;
	for (dword j = 0; j <= NumSections; ++j, v += dv, z += dz) {
		float u = mu, x = mx;
		for (dword i = 0; i <= NumSections; ++i, u += du, x += dx) {
			Vtx->Pos.x = x;
			Vtx->Pos.y = y;
			Vtx->Pos.z = z;
			Vtx->U = u / 2.5;
			Vtx->V = v / 2.5;
			++Vtx;
		}
	}

	Face *F = T->Faces;
	Vtx = T->Verts;
	for (dword j = 0; j < NumSections; ++j, Vtx += NumSections + 1) {
		for (dword i = 0; i < NumSections; ++i) {
			F->A = Vtx + i;
			F->B = Vtx + i + 1;
			F->C = Vtx + i + NumSections + 2;
			F->Txtr = RMat;
			F->U1 = F->A->U; F->V1 = F->A->V;
			F->U2 = F->B->U; F->V2 = F->B->V;
			F->U3 = F->C->U; F->V3 = F->C->V;
			++F;
			F->A = Vtx + i;
			F->B = Vtx + i + NumSections + 2;
			F->C = Vtx + i + NumSections + 1;
			F->Txtr = RMat;
			F->U1 = F->A->U; F->V1 = F->A->V;
			F->U2 = F->B->U; F->V2 = F->B->V;
			F->U3 = F->C->U; F->V3 = F->C->V;
			++F;
		}
	}

	// De-tile: warp the face UVs by a smooth low-freq function of vertex world
	// XZ so the tiled albedo stops repeating as a regular grid. Per-vertex-pos
	// → consistent across shared vertices. The rasterizer swizzles the warped
	// coords; the per-frame scroll adds a uniform offset on top (warp preserved).
	if (fds::FeatureFlags::water_detile()) {
		const float amp = fds::FeatureFlags::water_detile_amp();
		const float f   = 0.012f * fds::FeatureFlags::water_detile_scale();
		Face *FW = T->Faces, *FWE = T->Faces + T->FIndex;
		for (; FW < FWE; ++FW) {
			FW->U1 += waterDetileU(FW->A->Pos, amp, f); FW->V1 += waterDetileV(FW->A->Pos, amp, f);
			FW->U2 += waterDetileU(FW->B->Pos, amp, f); FW->V2 += waterDetileV(FW->B->Pos, amp, f);
			FW->U3 += waterDetileU(FW->C->Pos, amp, f); FW->V3 += waterDetileV(FW->C->Pos, amp, f);
		}
	}

	// SoA Phase 6.1: tessellateWaterGrid replaces T->Verts and rewires
	// F->A/B/C — A/B/C_idx default to 0 → every face aliases vertex 0
	// in T->frame. Without this restamp, the Phase 6.1 clipper TPos
	// override reads frame->TPos_x[0] for all water faces and the
	// surface degenerates. Same shape as the MakeFacesIndependent fix
	// in 203c14b.
	Compute_FaceVertexIndices(T);
}

// Sets required properties and modifies the object 'water.lwo' which acts
// like a mirror for the rest of the scene.
static void Reflective_Surface_Setup()
{
	for (Object *O = CitySc->ObjectHead; O; O = O->Next) {
		if (O->Type != Obj_TriMesh) continue;
		TriMesh *T = (TriMesh *)O->Data;
		if (std::strcmp(O->Name, "water.lwo") != 0) continue;

		prepareWaterSurface(O, T);
		tessellateWaterGrid(T, /*NumSections=*/64);
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
	fds::FListEntry *Ins = FList;
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
				//        Vector_Add(&U,&V,&Vtx->TPos_AOS);
				// 4x3 xform
				Vtx->TPos_AOS.x = M34[0][0]*Vtx->Pos.x+M34[0][1]*Vtx->Pos.y+M34[0][2]*Vtx->Pos.z+M34[0][3];
				Vtx->TPos_AOS.y = M34[1][0]*Vtx->Pos.x+M34[1][1]*Vtx->Pos.y+M34[1][2]*Vtx->Pos.z+M34[1][3];
				Vtx->TPos_AOS.z = M34[2][0]*Vtx->Pos.x+M34[2][1]*Vtx->Pos.y+M34[2][2]*Vtx->Pos.z+M34[2][3];
				
				Vtx->Flags&=0xFFFFFFFF-Vtx_Visible;
				Vtx->RZ=1.0/Vtx->TPos_AOS.z;
				Vtx->PX=Vtx->TPos_AOS.x*Vtx->RZ;
				Vtx->PY=Vtx->TPos_AOS.y*Vtx->RZ;
				//        Vtx->PX=CntrEX+PX*Vtx->TPos_AOS.x*Vtx->RZ;
				//        Vtx->PY=CntrEY-PY*Vtx->TPos_AOS.y*Vtx->RZ;
				//        Vtx->RU=Vtx->U*Vtx->RZ;
				//        Vtx->RV=Vtx->V*Vtx->RZ;
				//if (Vtx->TPos_AOS.z>Sc->FZP) Vtx->Flags|=Vtx_VisFar;
			}
			
			goto OUT;
			// This is in case 100% of trimesh AHEAD of camera. this saves some chks
Ahead://Vertex_Loop1(T->Vertex,VEnd,M,&V);
			for (Vtx=T->Verts;Vtx<VEnd;Vtx++)
			{
				//    if (!Vtx->FRem) continue;
				//        MatrixXVector(M,&Vtx->Pos,&U);
				//        Vector_Add(&U,&V,&Vtx->TPos_AOS);
				Vtx->TPos_AOS.x = M34[0][0]*Vtx->Pos.x+M34[0][1]*Vtx->Pos.y+M34[0][2]*Vtx->Pos.z+M34[0][3];
				Vtx->TPos_AOS.y = M34[1][0]*Vtx->Pos.x+M34[1][1]*Vtx->Pos.y+M34[1][2]*Vtx->Pos.z+M34[1][3];
				Vtx->TPos_AOS.z = M34[2][0]*Vtx->Pos.x+M34[2][1]*Vtx->Pos.y+M34[2][2]*Vtx->Pos.z+M34[2][3];
				
				Vtx->RZ=1.0/Vtx->TPos_AOS.z;
				Vtx->PX=Vtx->TPos_AOS.x*Vtx->RZ;
				Vtx->PY=Vtx->TPos_AOS.y*Vtx->RZ;
				//        Vtx->PX=CntrEX+PX*Vtx->TPos_AOS.x*Vtx->RZ;
				//        Vtx->PY=CntrEY-PY*Vtx->TPos_AOS.y*Vtx->RZ;
				//        Vtx->RU=Vtx->U*Vtx->RZ;
				//        Vtx->RV=Vtx->V*Vtx->RZ;
				Vtx->Flags&=0xFFFFFFFF-Vtx_Visible;
				if (Vtx->PX<0) Vtx->Flags|=Vtx_VisLeft;
				if (Vtx->PX>=XRes) Vtx->Flags|=Vtx_VisRight;
				if (Vtx->PY<0) Vtx->Flags|=Vtx_VisUp;
				if (Vtx->PY>=YRes) Vtx->Flags|=Vtx_VisDown;
				if (Vtx->TPos_AOS.z>Sc->FZP) Vtx->Flags|=Vtx_VisFar;
			}
			//    printf("Ahead VGA/Wizard.\n");
			goto OUT;
Regular:
			for (Vtx=T->Verts;Vtx<VEnd;Vtx++)
			{
				//    if (!Vtx->FRem) continue;
				//        MatrixXVector(M,&Vtx->Pos,&U);
				//        Vector_Add(&U,&V,&Vtx->TPos_AOS);
				Vtx->TPos_AOS.x = M34[0][0]*Vtx->Pos.x+M34[0][1]*Vtx->Pos.y+M34[0][2]*Vtx->Pos.z+M34[0][3];
				Vtx->TPos_AOS.y = M34[1][0]*Vtx->Pos.x+M34[1][1]*Vtx->Pos.y+M34[1][2]*Vtx->Pos.z+M34[1][3];
				Vtx->TPos_AOS.z = M34[2][0]*Vtx->Pos.x+M34[2][1]*Vtx->Pos.y+M34[2][2]*Vtx->Pos.z+M34[2][3];
				
				Vtx->Flags&=0xFFFFFFFF-Vtx_Visible;
				//      if (*(int32_t *)(&Vtx->TPos_AOS.z)>0x3F800000) // 1.0 in floating point rep.
				if (Vtx->TPos_AOS.z>Sc->NZP)
				{
					Vtx->RZ=1.0/Vtx->TPos_AOS.z;
					Vtx->PX=Vtx->TPos_AOS.x*Vtx->RZ;
					Vtx->PY=Vtx->TPos_AOS.y*Vtx->RZ;
					//          Vtx->PX=CntrEX+PX*Vtx->TPos_AOS.x*Vtx->RZ;
					//          Vtx->PY=CntrEY-PY*Vtx->TPos_AOS.y*Vtx->RZ;
					//          Vtx->RU=Vtx->U*Vtx->RZ;
					//          Vtx->RV=Vtx->V*Vtx->RZ;
					if (Vtx->PX<0) Vtx->Flags|=Vtx_VisLeft;
					if (Vtx->PX>=XRes) Vtx->Flags|=Vtx_VisRight;
					if (Vtx->PY<0) Vtx->Flags|=Vtx_VisUp;
					if (Vtx->PY>=YRes) Vtx->Flags|=Vtx_VisDown;
					if (Vtx->TPos_AOS.z>Sc->FZP) Vtx->Flags|=Vtx_VisFar;
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
				Vector_Add(&U,&V,&Vtx->TPos_AOS);
				
				Vtx->Flags=0;
				Vtx->RZ=1.0/Vtx->TPos_AOS.z;
				Vtx->PX=Vtx->TPos_AOS.x*Vtx->RZ;
				Vtx->PY=Vtx->TPos_AOS.y*Vtx->RZ;
				//        Vtx->PX=CntrEX+PX*Vtx->TPos_AOS.x*Vtx->RZ;
				//        Vtx->PY=CntrEY-PY*Vtx->TPos_AOS.y*Vtx->RZ;
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
				Vector_Add(&U,&V,&Vtx->TPos_AOS);
				
				Vtx->RZ=1.0/Vtx->TPos_AOS.z;

				// Environment mapping support removed at 11.04.02
//				Vtx->EU=128.0+95.0*(Vtx->N.x*IM[0][0]+Vtx->N.y*IM[0][1]+Vtx->N.z*IM[0][2]);
//				Vtx->REU=Vtx->EU*Vtx->RZ;
//				Vtx->EV=128.0+95.0*(Vtx->N.x*IM[1][0]+Vtx->N.y*IM[1][1]+Vtx->N.z*IM[1][2]);
//				Vtx->REV=Vtx->EV*Vtx->RZ;
				
				Vtx->PX=Vtx->TPos_AOS.x*Vtx->RZ;
				Vtx->PY=Vtx->TPos_AOS.y*Vtx->RZ;
				//        Vtx->PX=CntrEX+PX*Vtx->TPos_AOS.x*Vtx->RZ;
				//        Vtx->PY=CntrEY-PY*Vtx->TPos_AOS.y*Vtx->RZ;
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
				Vector_Add(&U,&V,&Vtx->TPos_AOS);
				
				Vtx->Flags = 0;
				//      if (*(int32_t *)(&Vtx->TPos_AOS.z)>0x3F800000) // 1.0 in floating point rep.
				// Environment mapping support removed at 11.04.02
//				Vtx->EU=128.0+127.0*(Vtx->N.x*IM[0][0]+Vtx->N.y*IM[0][1]+Vtx->N.z*IM[0][2]);
//				Vtx->EV=128.0+127.0*(Vtx->N.x*IM[1][0]+Vtx->N.y*IM[1][1]+Vtx->N.z*IM[1][2]);
				
				if (Vtx->TPos_AOS.z>Sc->NZP)
				{
					Vtx->RZ=1.0/Vtx->TPos_AOS.z;
					Vtx->PX=Vtx->TPos_AOS.x*Vtx->RZ;
					Vtx->PY=Vtx->TPos_AOS.y*Vtx->RZ;
					//          Vtx->PX=CntrEX+PX*Vtx->TPos_AOS.x*Vtx->RZ;
					//          Vtx->PY=CntrEY-PY*Vtx->TPos_AOS.y*Vtx->RZ;
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
OUT:
		// SoA Phase 6.1: dual-write T->Verts → T->frame. Without this,
		// the clipper's TPos-from-frame override (FRUSTRUM.CPP:870)
		// reads stale main-camera values during reflection rendering →
		// triangles smeared across screen. Reflected_Transform is the
		// known alternative transform path that writes T->Verts.PX/PY/
		// RZ/TPos in place; see docs/SOA_VERTEX_REFACTOR.md Phase 6.2.
		if (!T->frame) T->frame = new VertexFrame();
		T->frame->ensureSized(int(T->VIndex));
		VertexFrame_DumpFromAoS(T->frame, T->Verts, T->VIndex);

		FEnd=T->Faces+T->FIndex;
		for (F = T->Faces; F < FEnd; F++) {
			if (!F->VisibilityFlagsAll()
				&& ((F->Txtr->Flags & Mat_TwoSided) ||
					//(1)
					(AP.x * F->N.x + AP.y * F->N.y + AP.z * F->N.z < F->NormProd) //backface culling

					)) {
				dz = F->A->TPos_AOS.z;
				if (F->B->TPos_AOS.z > dz) dz = F->B->TPos_AOS.z;
				if (F->C->TPos_AOS.z > dz) dz = F->C->TPos_AOS.z;
#ifdef FRONT_TO_BACK_SORTING
				F->SortZ.F = dz;
#else
				F->SortZ.F = fzp - dz;
#endif
				*Ins++ = { F->SortZ.DW, F };
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
		MatrixXVector(View->Mat,&V,&Vtx->TPos_AOS);
		if (Vtx->TPos_AOS.z>Sc->NZP&&Vtx->TPos_AOS.z<Sc->FZP)
		{
			Vtx->RZ=1.0/Vtx->TPos_AOS.z;
			Vtx->PX=CntrEX+Vtx->TPos_AOS.x*PX*Vtx->RZ;
			Vtx->PY=CntrEY-Vtx->TPos_AOS.y*PY*Vtx->RZ;
			// Insert to List
			//dz = Vtx->TPos_AOS.z;
			//dz *=-16384;
			//dz +=0x7FFFFFFF;
			//RoundToInt((int32_t *)&O->F.SortZ.DW,dz);
			dz = Vtx->TPos_AOS.z;
			// FlareSize parity with Transform_Objects' omni loop. Without
			// this, an omni visible in the MIRROR frustum but never (yet)
			// in the main frustum draws its reflected flare with whatever
			// F.FlareSize holds — uninitialized heap garbage (±4e36) on
			// the scene's first frames. Depending on the per-process
			// garbage bits the flare rect is either degenerate (invisible)
			// or screen-covering: the additive white wash over the whole
			// pass-1 frame that read as "everything below the water is
			// white", nondeterministic per process, and curable by a
			// free-cam 360 (which swept every omni through the MAIN
			// frustum once, letting Transform_Objects initialize it).
			O->F.FlareSize = O->ISize;
			O->F.SortZ.F = 2 * fzp - dz;
			*Ins++ = { O->F.SortZ.DW, &O->F };
		}
	}
	COmnies = (Ins-FList)-CPolys;
	for(I=0;I<Sc->NumOfParticles;I++)
	{
		if (Sc->Pcl[I].Flags&Particle_Active)
		{
			if ((dz=Sc->Pcl[I].V.TPos_AOS.z)>=Sc->NZP) {
				F = &Sc->Pcl[I].F;
				dz *=-16384;
				dz +=float(0x7FFFFFFF); // explicit: INT_MAX bias as float (rounds to 2^31)
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
				Color = Vtx->TPos_AOS.z*CitySc->FZP;

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
	// Scroll the water-surface texture. Drives the rasterizer via the FACE UVs
	// F->U1..U3 (verified empirically — the render path ignores vertex UVs).
	// Original rate restored per user request; water_scroll_speed scales it
	// (1.0 = original).
	const float sp = fds::FeatureFlags::water_scroll_speed();
	const float du = dTime * 0.0007f * 2.0f * sp, dv = dTime * 0.0003f * 2.0f * sp;
	Face *F, *FE;
	for(F = T->Faces, FE = F + T->FIndex; F<FE; F++)
	{
		F->U1 += du; F->V1 += dv;
		F->U2 += du; F->V2 += dv;
		F->U3 += du; F->V3 += dv;
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
static void drawRainDrop(Face* F, Vertex **VA, mword numVerts, dword miplevel,
                         const fds::RenderTarget& /*rt*/,
                         const fds::CameraContext& /*cam*/)
{
	Vertex *V = VA[0];

	int32_t x = Fist(V->PX);
	int32_t y = Fist(V->PY);
	
	if (x>=0&&x<XRes&&y>=0&&y<YRes)
	{
		dword Z;
		if (V->TPos_AOS.z < C_FZP)
			Z = 0xFF80 - Fist(g_zscale * V->TPos_AOS.z);
		else
			Z = 1;
		dword offset = x + y*XRes;
		if ( ZPage16[offset] < Z )
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

	float ahead_z = V->TPos_AOS.z;

//	float raz = 1.0 / ahead_z;
	float ahead_x = x - 3;
	float ahead_y = y + 15;
	
	if (x>=0&&x<XRes&&y>=0&&y<YRes)
	{
	
		dword Z;
		if (V->TPos_AOS.z < C_FZP)
			Z = 0xFF80 - Fist(g_zscale * V->TPos_AOS.z);
		else
			Z = 1;
		dword offset = x + y*XRes;
		if ( ZPage16[offset] < Z )
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
	fprintf(stderr, "[CITY] rain: %d particles\n", NUM_RAIN_PCLS);
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

		// Trail quads write TPos_AOS directly with no frame/A_idx wiring —
		// the SoA rasterizer path reads frame->TPos_x[A_idx] and culls
		// them, so on the city's non-TBR path they render to nothing.
		// Single-pixel drops (drawRainDrop, used when TrailLength==0)
		// bypass the mesh path entirely and still work.
		// p.InitTrail(M, TheOtherBarry<barry::TBlendMode::TRANSPARENT, barry::TTextureMode::NORMAL>, 50.0f, 5.0f);

		// NOTE: consider adding some randomization (per particle and over time)
		p.Vel.x = rainVel.x;
		p.Vel.y = rainVel.y;
		p.Vel.z = rainVel.z;
	}
}

static void riskOfRain(Scene *Sc, float dt)
{
	// set rainvel based on some noise function of current frame
	rainVel = evalRainVel(CurFrame);
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
		MatrixXVector(View->Mat, &v, &P->V.TPos_AOS);

		if (P->V.TPos_AOS.z>=Sc->NZP)
		{
			P->V.RZ = 1.0/P->V.TPos_AOS.z;
			P->V.PX = CntrX+FOVX*P->V.TPos_AOS.x*P->V.RZ;
			P->V.PY = CntrY-FOVY*P->V.TPos_AOS.y*P->V.RZ;
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

	// Color + Z now live in separate allocations (post-LockTexture
	// refactor). The cube-map render uses this local V_Create on a
	// stack-local TmpSurf, so allocate both buffers here too.
	dword ZBufferSize = sizeof(word) * VS->X * VS->Y;
	if (!(VS->Z16 = (byte*)malloc(ZBufferSize))) return 1;
	memset(VS->Z16, 0, ZBufferSize);
	if (!(VS->Data = (byte*)malloc(VS->PageSize))) {
		free(VS->Z16); VS->Z16 = nullptr;
		return 1;
	}
	memset(VS->Data, 0, VS->PageSize);

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
			// >= / <= so directions exactly on a cube-face boundary
			// (two |components| equal) always land in some branch.
			// With strict >, a direction like d=(1,0,1) failed every
			// test and left the output pixel uninitialized — manifested
			// as "missing chunks" at cube-edge directions.
			if (d.z >= max(fabs(d.x), fabs(d.y))) {
				const float u = ((d.x / d.z) + 1.0) / 2.0;
				const float v = ((d.y / d.z) + 1.0) / 2.0;
				it->set(ROT % 4,1-u, v);
			} else if (d.x >= max(fabs(d.y), fabs(d.z))) {
				const float u = ((d.z / d.x) + 1.0) / 2.0;
				const float v = ((d.y / d.x) + 1.0) / 2.0;
				it->set((ROT + 1) % 4, u, v);
			} else if (d.z <= -max(fabs(d.x), fabs(d.y))) {
				const float u = ((d.x / d.z) + 1.0) / 2.0;
				const float v = ((d.y / d.z) + 1.0) / 2.0;
				it->set((ROT + 2) % 4,1-u,1-v);
			} else if (d.x <= -max(fabs(d.y), fabs(d.z))) {
				const float u = ((d.z / d.x) + 1.0) / 2.0;
				const float v = ((d.y / d.x) + 1.0) / 2.0;
				it->set((ROT + 3) % 4,u,1-v);
			} else if (d.y >= max(fabs(d.x), fabs(d.z))) {
				const float u = ((d.x / d.y) + 1.0) / 2.0;
				const float v = ((d.z / d.y) + 1.0) / 2.0;
				it->set(4, u, v);
			} else if (d.y <= -max(fabs(d.x), fabs(d.z))) {
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



constexpr int PANORAMA_XRES = 1024;
constexpr int PANORAMA_YRES = 1024;
constexpr int CUBE_MAP_XRES = 1024;
constexpr int CUBE_MAP_YRES = 1024;

// Synthetic test panorama: paints quadrant labels + lat/lon gridlines.
// Decodes which world direction the equirectangular generator maps to
// which (eu, ev) when overlaid on a real building's reflection.
// CalcEquirectangularPanoramaTable's convention:
//   eu = lon / (2π)   with d = (sin lon · cos lat, sin lat, cos lon · cos lat)
//   ev = (lat + π/2) / π
// Quadrants by eu: 0=YELLOW(+z), 0.25=RED(+x), 0.5=GREEN(-z), 0.75=BLUE(-x).
// Pole bands by ev: 0=WHITE(-y), 1=MAGENTA(+y).
static void paintDebugPanorama(std::vector<uint32_t>& panorama)
{
	for (int y = 0; y < PANORAMA_YRES; ++y) {
		const float ev = float(y) / PANORAMA_YRES;
		uint32_t band_color;
		if (ev < 0.10f) band_color = 0x00FFFFFFu;        // WHITE  (ev=0 band)
		else if (ev > 0.90f) band_color = 0x00FF00FFu;   // MAGENTA(ev=1 band)
		else band_color = 0;                              // mid: pick by eu below
		for (int x = 0; x < PANORAMA_XRES; ++x) {
			const float eu = float(x) / PANORAMA_XRES;
			uint32_t c;
			if (band_color != 0) {
				c = band_color;
			} else {
				if      (eu < 0.125f) c = 0x00FFFF00u;   // YELLOW (eu≈0)
				else if (eu < 0.375f) c = 0x00FF0000u;   // RED    (eu≈0.25)
				else if (eu < 0.625f) c = 0x0000FF00u;   // GREEN  (eu≈0.5)
				else if (eu < 0.875f) c = 0x000000FFu;   // BLUE   (eu≈0.75)
				else                  c = 0x00FFFF00u;   // YELLOW (eu≈1, wraps)
			}
			const float ev_frac = ev * 6.0f;
			if (fabsf(ev_frac - roundf(ev_frac)) < 0.005f) c = 0x00FFFFFFu;
			const float eu_frac = eu * 12.0f;
			if (fabsf(eu_frac - roundf(eu_frac)) < 0.003f) c = 0x00000000u;
			panorama[y * PANORAMA_XRES + x] = c | 0xFF000000u;
		}
	}
}

// Dumps a panorama to /tmp/city_panorama_NN_<objName>.ppm. One-shot per
// process; the static index makes the filename unique across calls.
static void savePanoramaPPM(const std::vector<uint32_t>& panorama, const char* objName)
{
	static int dumpIdx = 0;
	char path[256];
	snprintf(path, sizeof(path), "/tmp/city_panorama_%02d_%s.ppm",
	         dumpIdx++, objName ? objName : "(noname)");
	for (char *p = path; *p; ++p) if (*p == '/' || *p == ' ') *p = '_';
	path[0] = '/'; path[1] = 't'; path[2] = 'm'; path[3] = 'p'; path[4] = '/';
	FILE *f = fopen(path, "wb");
	if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", PANORAMA_XRES, PANORAMA_YRES);
	std::vector<unsigned char> row(PANORAMA_XRES * 3);
	for (int y = 0; y < PANORAMA_YRES; ++y) {
		for (int x = 0; x < PANORAMA_XRES; ++x) {
			const uint32_t px = panorama[y * PANORAMA_XRES + x];
			row[x*3 + 0] = (px >> 16) & 0xFF;
			row[x*3 + 1] = (px >>  8) & 0xFF;
			row[x*3 + 2] = (px      ) & 0xFF;
		}
		fwrite(row.data(), 1, row.size(), f);
	}
	fclose(f);
	fprintf(stderr, "[PANORAMA] dumped %s\n", path);
}

// Render the 6 axis-aligned cube faces for one building, remap to an
// equirectangular panorama, and attach it as Obj->Reflection. Optionally
// overlays the debug panorama or dumps a PPM (per FeatureFlags).
// `cubeMaps` is caller-owned scratch (sized to 6 × CUBE_MAP_XRES * YRES);
// passed in to avoid reallocating per building.
static void bakeBuildingPanorama(Object* Obj,
                                  Camera& envCam,
                                  const VESA_Surface& tmpSurf,
                                  const std::vector<CubeMapCoord>& cubeMapper,
                                  std::vector<uint32_t> cubeMaps[6],
                                  std::vector<uint32_t>& panorama)
{
	TriMesh* T = (TriMesh*)Obj->Data;
	const dword flags = T->Flags;
	T->Flags &= ~HTrack_Visible;

	envCam.ISource = T->IPos + T->BSphereCtr;
	for (int i = 0; i != 6; ++i) {
		parallel_memset(VPage, 0, PageSize);
		parallel_memset(ZPage16, 0, XRes * YRes * sizeof(word));
		memcpy(envCam.Mat, AxisAlignedViews[i], sizeof(envCam.Mat));

		// ForceForward: this is the per-building env-map bake into a
		// temp surface. The deferred-skybox pass won't fire here
		// (cube-map bake uses the forward path explicitly), so the
		// sky has to land via RenderSkyCube or the reflective faces
		// would sample empty backgrounds.
		RenderSkyCube(SkySc, View, true, /*ForceForward=*/true);
		Reflected_Transform(CitySc);
		if (0 != CAll) {
			Radix_Sort(FList, SList, CAll);
			// Cube-map bake doesn't need deferred quality, and the
			// G-buffer is sized for the engine framebuffer not the
			// 1024x1024 TmpSurf — deferred would fall back to forward
			// anyway. Spell it out to skip the size-check + mat32 clear.
			Render(RenderPath::ForceForward);
		}
		Transform_Objects(CitySc, fds::g_mainCamera, fds::g_mainFaces);
		if (0 != CAll) {
			Radix_Sort(FList, SList, CAll);
			Render(RenderPath::ForceForward);
		}
		memcpy(cubeMaps[i].data(), tmpSurf.Data, CUBE_MAP_XRES * CUBE_MAP_YRES * 4);
	}
	T->Flags = flags;

	// Cube faces -> equirectangular panorama via the precomputed table.
	auto outputIt = panorama.begin();
	auto mapperIt = cubeMapper.begin();
	for (int y = 0; y != PANORAMA_YRES; ++y) {
		for (int x = 0; x != PANORAMA_XRES; ++x, ++mapperIt, ++outputIt) {
			const int u = int(mapperIt->getU() * CUBE_MAP_XRES);
			const int v = int(mapperIt->getV() * CUBE_MAP_YRES);
			*outputIt = cubeMaps[mapperIt->getTextureIdx()][u + v * CUBE_MAP_XRES];
		}
	}

#if FDS_DEV
	if (fds::FeatureFlags::debug_panorama()) paintDebugPanorama(panorama);
#endif
	// Caller installs Obj->Reflection via Materialize on the *raw*
	// panorama (the disk cache stores raw too — Materialize one-way
	// Sachletz-tiles in place, so round-tripping through Txtr->Data
	// would tile twice).
#if FDS_DEV
	if (fds::FeatureFlags::dump_panorama())  savePanoramaPPM(panorama, Obj->Name);
#endif
}

// ── Vehicle headlights ("for the lulz") ──────────────────────────────
// Every vehicle-sized mover gets one warm spotlight welded to its nose,
// re-aimed along its motion every frame. "Vehicle" = TriMesh with an
// animated Pos spline (>1 key) and a vehicle-scale bounding sphere —
// the city traffic system: taxis, buses, trams, police, cars, bike,
// ambulance, plus the two AlignToPath ships. The size gate excludes the
// movers that are NOT vehicles (water.lwo r=1061, the b1.lwo building
// r=709, the r=0 camera target). Cones read as volumetric beams under
// --draw_cones and glow in the froxel fog; surface lighting paints
// whatever they sweep. No shadows (~30 moving shadow maps would eat
// the frame budget). The police car strobes red/blue, obviously.
namespace {
struct VehicleHeadlight {
	TriMesh* T;
	Omni*    spot;
	Vector   lastDir;
	Vector   prevPos;
	bool     hasPrev;   // delta heading needs one animated frame first
	bool     police;
};
std::vector<VehicleHeadlight> g_headlights;
}

static void Setup_Vehicle_Headlights()
{
	g_headlights.clear();
	if (!fds::FeatureFlags::city_headlights()) return;
	int omnis = 0;
	for (Omni *o = CitySc->OmniHead; o; o = o->Next) ++omnis;
	for (Object *O = CitySc->ObjectHead; O; O = O->Next) {
		if (O->Type != Obj_TriMesh) continue;
		TriMesh *T = (TriMesh *)O->Data;
		if (T->Pos.NumKeys <= 1) continue;                     // parked scenery
		if (T->BSphereRadius <= 0.0f || T->BSphereRadius > 200.0f)
			continue;                                          // water / moving building / camera target
		const char *name = O->Name ? O->Name : "?";
		const bool police = std::strstr(name, "poliece") != nullptr;
		Omni *spot = fds::MakeSpotLight(
			CitySc,
			/*RGB*/ 255.0f, 235.0f, 185.0f,                  // warm halogen
			fds::FeatureFlags::city_headlight_intensity(),
			/*range*/ 1600.0f,                               // ~16m at 1cm/unit
			T->IPos, Vector(0.0f, 0.0f, 1.0f),
			/*hot=*/6.0f, /*outer=*/16.0f,
			/*shadowMapRes=*/0, /*castsShadow=*/false);
		g_headlights.push_back({T, spot, Vector(0.0f, 0.0f, 0.0f),
		                        Vector(0.0f, 0.0f, 0.0f), false, police});
		fprintf(stderr, "[CITY] headlight on '%s'%s\n", name,
		        police ? " (strobe)" : "");
	}
	fprintf(stderr, "[CITY] %zu vehicle headlights (%d scene omnis before; deferred cap %d)\n",
	        g_headlights.size(), omnis, 128);
}

// Re-aim after Animate_Objects (which re-evaluates each spot's single-key
// Pos spline back to its creation pos — overwrite wins, same pattern as
// the greets robot spots).
static void Update_Vehicle_Headlights()
{
	const float intensity = fds::FeatureFlags::city_headlights()
	    ? fds::FeatureFlags::city_headlight_intensity() : 0.0f;
	for (VehicleHeadlight &h : g_headlights) {
		TriMesh *T = h.T;
		// Heading: AlignToPath meshes (the ships) maintain T->Heading with
		// spline lookahead; everything else moves by rotation-keyed splines,
		// so derive it from the frame-to-frame position delta. Paused /
		// parked frames (zero delta) hold the last good aim; the light
		// stays doused until the first real delta so frame one doesn't
		// flash a default-aimed beam.
		Vector dir = (T->Flags & Tri_AlignToPath) ? T->Heading
		           : Vector(T->IPos.x - h.prevPos.x,
		                    T->IPos.y - h.prevPos.y,
		                    T->IPos.z - h.prevPos.z);
		const bool deltaValid = (T->Flags & Tri_AlignToPath) || h.hasPrev;
		h.prevPos = T->IPos;
		h.hasPrev = true;
		const float l2 = dir.x*dir.x + dir.y*dir.y + dir.z*dir.z;
		bool aimed = true;
		if (deltaValid && l2 > 1e-6f) {
			const float il = 1.0f / std::sqrt(l2);
			dir.x *= il; dir.y *= il; dir.z *= il;
			h.lastDir = dir;
		} else if (h.lastDir.x*h.lastDir.x + h.lastDir.y*h.lastDir.y
		           + h.lastDir.z*h.lastDir.z > 0.5f && h.hasPrev) {
			dir = h.lastDir;       // parked/slow: keep the last good aim
		} else {
			aimed = false;         // no heading yet — keep it dark
		}
		// Nose mount: pivot pushed just outside the bounding sphere
		// (scaled — RotMat rows carry IScale, BSphereRadius doesn't) so
		// the beam doesn't start inside the mesh (the cone march would
		// light its own cabin).
		const float scale = T->IScale.x > 0.0f ? T->IScale.x : 1.0f;
		const float nose = T->BSphereRadius * scale;
		h.spot->IPos.x = T->IPos.x + dir.x * nose;
		h.spot->IPos.y = T->IPos.y + dir.y * nose;
		h.spot->IPos.z = T->IPos.z + dir.z * nose;
		h.spot->IDir  = dir;
		h.spot->ISize = aimed && (T->Flags & HTrack_Visible) ? intensity : 0.0f;
		if (h.police) {
			// 2 Hz red/blue strobe (Timer is ~ms). Lulz mandate.
			const bool red = ((int32_t)Timer / 250) & 1;
			h.spot->L.R = red ? 255.0f : 40.0f;
			h.spot->L.G = 40.0f;
			h.spot->L.B = red ? 40.0f : 255.0f;
		}
	}
}

void Initialize_City()
{
	CitySc = loadSceneAligned("SCENES/CITY.FLD");
	CTPartTime = 100.0f*(CitySc->EndFrame-CitySc->StartFrame)/50.0f;
	CitySc->NZP = 20.0;
	CitySc->FZP = 7500.0f;
	CitySc->Flags |= Scn_Fogged|Scn_ZBuffer;
	// Matte-dominated scene — OuterVec's outer-SIMD wins by ~3 ms.
	CitySc->PreferOuterVec = 1;
	CitySc->Ambient.B = 32*2;
	CitySc->Ambient.G = 48*2;
	CitySc->Ambient.R = 64*2;

	tagSceneMaterials(CitySc, /*restrictToScene=*/true);

	Reflective_Surface_Setup();
	AddCameraOmni(); 

	//SceneCorrections();


	Preprocess_Scene(CitySc, "CITY");

	//for(M = MatLib;M;M=M->Next)
	//{
	//	if (M->RelScene != CitySc) continue;
	//	if (!M->Txtr) continue;
	//}

	auto cubeMapper = CalcEquirectangularPanoramaTable(PANORAMA_XRES, PANORAMA_YRES);
	
	//	printf("Scene-Proc MEM = %d\n",DPMI_Free_Memory());
	// also make the appropriate Layer 2 fillers,
	// when fog will be implemented
	Init_Distort();

	SkySc = CreateSkyCube(0);

	// TODO: should animate the object to the frame where the camera is nearest to it
	CurFrame = 0;
	View = CitySc->CameraHead;

	// render env maps for a few select objects
	FList_Allocate(CitySc);
	auto oldAspectRatio = AspectRatio;
	// HACK: using exactly 1 triggers frustrum clipper bugs (instability /
	// freezes on certain angles). The all-`>=` lookup change in
	// CalcEquirectangularPanoramaTable wasn't enough to make 1.0 safe —
	// the residual cause is in the render side (Reflected_Transform's
	// asymmetric `>=XRes` for VisRight/Down vs the clipper's strict `>`
	// in viewportCalcFlags). Leaving 0.999 until that's fixed.
	AspectRatio = 0.999;
	Camera EnvCam = { 0 };
	View = &EnvCam;
	EnvCam.IFOV = 90.0;
	CalcPersp(&EnvCam);
	//EnvCam.PerspX = CUBE_MAP_XRES / 2;
	//EnvCam.PerspY = CUBE_MAP_YRES / 2;
	FOVX = View->PerspX;
	FOVY = View->PerspY;

	// Auto-detect creased meshes by face-normal angle threshold and
	// duplicate vertices per-face on hits. Replaces the old building-
	// name match (`b0.lwo`..`bN.lwo`) — the angle test is the
	// systematic version and catches non-building creased geometry
	// too. 30° matches common DCC tools' "auto-smooth angle" default.
	// Run before Lighting() so the forward-path Gouraud lighting also
	// sees the faceted normals.
	MakeFacesIndependentByAngle(CitySc, 30.0f);

	Animate_Objects(CitySc, /*cam=*/nullptr);
	Lighting(CitySc);

	// MainSurf swap to a CUBE_MAP_XRES x CUBE_MAP_YRES TmpSurf for the
	// off-screen reflection passes. The demo thread's resize-pending poll
	// can fire EngineResize at any time; that path goes through
	// VESA_VPageExternal which does memcpy(MainSurf, &SDL_MainSurf, ...) —
	// while we have MainSurf pointed at this stack-local TmpSurf, that
	// memcpy would stomp our cube-map state with the SDL screen surface.
	// Hold g_engineSurfaceMutex for the duration of the swap so a
	// concurrent EngineResize blocks until we restore MainSurf below.
	std::lock_guard<std::mutex> mainsurfLock(g_engineSurfaceMutex);

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

	// Skip the cube-map bake when iterating the deferred path. The bake
	// renders 6 cube faces × N buildings × Render() each — slow even on
	// native, more so under ASan. Without it, City windows show their
	// raw "windows" texture (the per-building reflection panorama is
	// never built and Obj->Reflection stays null). Phase C work for
	// deferred env-map will reproduce the reflection sampling per pixel
	// from the panorama anyway, so skipping during dev is fine.
	const bool skipCubeMap = fds::FeatureFlags::skip_cubemap();
	if (skipCubeMap) {
		fprintf(stderr, "[CITY] --skip_cubemap on — skipping per-building cube-map bake\n");
	}

	std::vector<uint32_t> cubeMaps[6];
	for (auto& cubeMap : cubeMaps) {
		cubeMap.resize(CUBE_MAP_XRES * CUBE_MAP_YRES);
	}
	std::vector<uint32_t> panorama(PANORAMA_XRES * PANORAMA_YRES);

	// Collect the buildings up front so the disk cache can key off a
	// stable name list (and skip the bake loop entirely on a cache hit).
	std::vector<fds::BuildingPanoramaEntry> buildings;
	if (!skipCubeMap) {
		for (Object* Obj = CitySc->ObjectHead; Obj != nullptr; Obj = Obj->Next) {
			if (Obj->Type != Obj_TriMesh) { continue; }
			if (Obj->Name == nullptr) { continue; }
			if (!(Obj->Name[0] == 'b' && '0' <= Obj->Name[1] && Obj->Name[1] <= '9'
			      && 0 == strcmp(Obj->Name + 2, ".lwo"))) { continue; }
			buildings.push_back({Obj, std::string(Obj->Name)});
		}
	}

	const bool envCacheEnabled = !skipCubeMap && fds::FeatureFlags::city_envmap_cache();
	uint64_t envCacheKey = 0;
	if (envCacheEnabled) {
		envCacheKey = fds::ComputeCityPanoramaCacheKey(
			"SCENES/CITY.FLD",
			PANORAMA_XRES, PANORAMA_YRES,
			CUBE_MAP_XRES, CUBE_MAP_YRES,
			buildings);
	}
	const bool cacheHit = envCacheEnabled && envCacheKey != 0
		&& fds::TryLoadCityPanoramaCache(envCacheKey,
		                                  PANORAMA_XRES, PANORAMA_YRES,
		                                  buildings);
	const size_t panoramaBytes = size_t(PANORAMA_XRES) * size_t(PANORAMA_YRES) * 4;
	if (cacheHit) {
		building_count = int(buildings.size());
	} else {
		for (auto& b : buildings) {
			// Bake runs on the init worker; ESC during Glato sets g_shouldQuit
			// but is otherwise stuck behind t1.join() until the bake finishes.
			// Bail early so the user gets sub-second quit response.
			if (g_shouldQuit.load(std::memory_order_relaxed)) { break; }
			building_count++;
			bakeBuildingPanorama(b.obj, EnvCam, TmpSurf, cubeMapper, cubeMaps, panorama);
			// Snapshot the raw (pre-Sachletz) panorama so the cache writer
			// can persist it; Materialize one-way-tiles below.
			b.rawPanorama.assign(
				(const uint8_t*)panorama.data(),
				(const uint8_t*)panorama.data() + panoramaBytes);
		}
		if (envCacheEnabled && envCacheKey != 0
		    && !g_shouldQuit.load(std::memory_order_relaxed)
		    && building_count == int(buildings.size())) {
			fds::WriteCityPanoramaCache(envCacheKey,
			                             PANORAMA_XRES, PANORAMA_YRES,
			                             buildings);
		}
	}
	// Materialize each building's raw panorama into Obj->Reflection. Done
	// here (not inside bakeBuildingPanorama) so the cache-hit path can
	// reuse the same call without re-tiling already-tiled bytes.
	for (auto& b : buildings) {
		if (b.rawPanorama.size() != panoramaBytes) continue;
		b.obj->Reflection = Materialize(b.rawPanorama.data(),
		                                 PANORAMA_XRES, PANORAMA_YRES);
		// rawPanorama no longer needed after Materialize copies it into
		// the Texture; release to free ~4 MB per building.
		b.rawPanorama.clear();
		b.rawPanorama.shrink_to_fit();
	}

	// For deferred-path env-map dispatch: the lighting pass has only
	// matID per-pixel (from mat32 in the G-buffer); per-building env
	// textures need to live on the Material the matID resolves to.
	// Clone the windows-base material per (building, base-mat), set
	// EnvTexture = building's panorama, repoint F->Txtr to the clone.
	// The forward path's TheOtherBarry uses F->ReflectionTexture (set
	// alongside) and Face_Reflective so it keeps working unchanged.
	int windowsCloned = 0;
	for (Object* Obj = CitySc->ObjectHead; Obj != nullptr; Obj = Obj->Next) {
		if (Obj->Type != Obj_TriMesh) { continue; }
		if (Obj->Name == nullptr) { continue; }
		if (!(Obj->Name[0] == 'b' && '0' <= Obj->Name[1] && Obj->Name[1] <= '9' && 0 == strcmp(Obj->Name + 2, ".lwo"))) { continue; }
		// Obj->Reflection is set by the cube-map bake — null when SKIP_CUBEMAP=1
		// or any other reason the bake didn't run.
		if (!Obj->Reflection) continue;

		// Per-building dedup: same windows-base-mat shared across multiple
		// faces on this building reuses one clone.
		std::map<Material *, Material *> baseToClone;

		auto T = (TriMesh*)(Obj->Data);
		for (Face* F = T->Faces, *FE = T->Faces + T->FIndex; F != FE; ++F) {
			if (!F->Txtr || !F->Txtr->Name) continue;
			if (!strstr(F->Txtr->Name, "windows")) continue;

			F->Flags |= Face_Reflective;
			F->ReflectionTexture = Obj->Reflection->Txtr;

			Material *base = F->Txtr;
			Material *clone = baseToClone[base];
			if (!clone) {
				clone = getAlignedType<Material>(16);
				std::memcpy(clone, base, sizeof(Material));
				clone->EnvTexture = Obj->Reflection->Txtr;
				clone->Reflection = 0.5f;       // 50% env blend (tunable)
				clone->ID = 0;                  // re-assigned by Scene_RebuildMatTable
				clone->Prev = nullptr;
				clone->Next = MatLib;
				if (MatLib) MatLib->Prev = clone;
				MatLib = clone;
				baseToClone[base] = clone;
				++windowsCloned;
			}
			F->Txtr = clone;
		}
	}
	fprintf(stderr, "[CITY] cloned %d windows-materials with EnvTexture set\n", windowsCloned);

	// Pick up the new clones in matTable so the deferred lighting pass
	// can resolve their matIDs.
	Scene_RebuildMatTable(CitySc);
	AspectRatio = oldAspectRatio;

	// Locate the water material's post-rebuild matID so the deferred
	// lighting pass can recognize water-mesh pixels and apply the
	// transparent-blend with the dispMap-distorted reflection beneath.
	{
		int waterID = -1;
		for (Material *M = MatLib; M; M = M->Next) {
			if (M->RelScene != CitySc || !M->Name) continue;
			if (strstr(M->Name, "water") || strstr(M->Name, "WATER")) {
				waterID = (int)M->ID;
				break;
			}
		}
		SetDeferredWaterMatID(waterID);
		fprintf(stderr, "[CITY] deferred water matID = %d\n", waterID);
	}

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

	// TmpSurf.Data + Z16 were malloc'd by the local V_Create above. The
	// struct itself is stack-local and goes away at scope end, but its
	// allocations would leak. Free them here, after we've restored
	// MainSurf so no further code will read from TmpSurf.
	if (TmpSurf.Data) { free(TmpSurf.Data); TmpSurf.Data = nullptr; }
	if (TmpSurf.Z16) { free(TmpSurf.Z16); TmpSurf.Z16 = nullptr; }

	// initialize rain FX (allocates the particle pool — runs before the
	// driver's setupFaceLists, which sizes the FList for the trails)
	if (fds::FeatureFlags::city_rain())
		initRain(CitySc);

	// Test spotlights for volumetric-cone overlay experimentation.
	// City coordinate scale is roughly 1-unit-per-cm (existing FLD omnis
	// are at y=1314 ≈ 13m, range 7000 ≈ 70m). Cones cast no shadows
	// (Omni_Active only) — we just want the volumetric primitive to
	// read off them. Gated on FDS_CITY_TEST_SPOTS so production runs
	// aren't polluted with the placeholder lights.
	if (fds::FeatureFlags::city_test_spots()) {
		auto streetlight = [&](float x, float y, float z,
		                       float dx, float dy, float dz,
		                       float r, float g, float b,
		                       float range) {
			Omni *O = fds::MakeSpotLight(
				CitySc,
				r, g, b,
				/*intensity*/3.0f, range,
				Vector(x, y, z),
				Vector(dx, dy, dz),
				/*hot=*/12.0f, /*outer=*/35.0f,
				/*shadowMapRes=*/512,
				/*castsShadow=*/true);
			// Streetlights are static — shadow map can be baked once
			// at init and reused every frame (skips per-frame depth
			// pass for this light).
			O->Flags |= Omni_StaticShadow;
		};
		// Lamppost spread, scaled to city's ~1cm/unit. Streetlights at
		// y=2000 (above road). Angled inward-and-down so cones cross
		// the road space in front of the camera — straight-down would
		// be a thin sliver from camera height (~291) and never cross
		// the view. Mostly sodium-orange, one cool-blue pair at depth.
		const float Y = 2000.0f;
		const float R = 4000.0f;  // range
		// dir vectors: inward (-sign(x)) + downward (-1 y). normalize-ish.
		// Right-side cones at +X point in -X, +Y stays -1. Length doesn't
		// matter here — MakeSpotLight calls Vector_Norm at render time.
		streetlight( 1500.0f, Y,  -1500.0f, -0.6f, -1.0f,  0.0f, 255.0f, 180.0f,  60.0f, R);
		streetlight(-1500.0f, Y,  -1500.0f,  0.6f, -1.0f,  0.0f, 255.0f, 180.0f,  60.0f, R);
		streetlight( 1500.0f, Y,    500.0f, -0.6f, -1.0f,  0.0f, 255.0f, 180.0f,  60.0f, R);
		streetlight(-1500.0f, Y,    500.0f,  0.6f, -1.0f,  0.0f, 255.0f, 180.0f,  60.0f, R);
		streetlight( 1500.0f, Y,   2500.0f, -0.6f, -1.0f,  0.0f, 120.0f, 180.0f, 255.0f, R);
		streetlight(-1500.0f, Y,   2500.0f,  0.6f, -1.0f,  0.0f, 120.0f, 180.0f, 255.0f, R);
		fprintf(stderr, "[CITY] installed 6 test streetlight cones\n");

		// One shadow-casting Light_Omni at city ground level between
		// buildings, exercising the cube-shadow path (6 face shadow
		// maps). Buildings cast shadows in all directions from this
		// position so shadowed regions are clearly visible. Bright
		// cool-white so the shadowed regions read against the existing
		// orange streetlight pools.
		{
			Omni *O = (Omni*)getAlignedBlock(sizeof(Omni), 16);
			std::memset(O, 0, sizeof(Omni));
			O->L.R = 220.0f; O->L.G = 240.0f; O->L.B = 255.0f; O->L.A = 1.0f;
			O->ISize  = 2.5f;
			// Range was 2500: too large — it's an omnidirectional sphere,
			// not a directional cone, so range 2500 from a near-camera
			// position covers nearly every screen pixel → the halo pass
			// does per-sample math for every pixel (no sphere-cull early
			// out fires). 800 gives a halo footprint similar to the spot
			// cones; surface lighting falls off more sharply but still
			// reads visually as the "cool blue accent" against the orange
			// streetlight pools.
			O->IRange = 800.0f;
			O->rRange = 1.0f / O->IRange;
			O->IPos   = Vector(0, 600, 500);
			O->Type   = Light_Omni;
			O->Flags  = Omni_Active | Omni_CastsShadow | Omni_StaticShadow;
			O->shadowMapRes = 256;  // 256² per face × 6 = 1.5 MB total
			// Halo controls decoupled from surface lighting: halo is a
			// soft volumetric glow that reads best when it extends past
			// the surface-lit sphere, and at lower density than the
			// brilliant cube interior. Without these, the halo inherits
			// IRange=800 / density=1.0 and overpowers the streetlights.
			O->HaloIntensity = 0.4f;     // 40% density vs default 1.0
			O->HaloRange     = 1800.0f;  // 2.25× wider than IRange
			// Splines so Animate_Objects doesn't crash. Single-key
			// constants for static position/size/range.
			auto initKey = [](Spline& sp, float a, float b, float c) {
				sp.NumKeys = 1; sp.Keys = new SplineKey;
				std::memset(sp.Keys, 0, sizeof(SplineKey));
				sp.Keys[0].Pos.x = a; sp.Keys[0].Pos.y = b; sp.Keys[0].Pos.z = c;
				sp.Flags = 0; sp.CurKey = 0;
			};
			initKey(O->Pos,   O->IPos.x, O->IPos.y, O->IPos.z);
			initKey(O->Size,  O->ISize,  O->ISize,  O->ISize);
			initKey(O->Range, O->IRange, O->IRange, O->IRange);
			// Flare-pass plumbing (filler is no-op for non-flares).
			O->F.A = &O->V; O->F.B = &O->V; O->F.C = &O->V;
			O->F.Filler = [](Face*, Vertex**, dword, dword,
			                 const fds::RenderTarget&,
			                 const fds::CameraContext&) {};
			O->Next = CitySc->OmniHead;
			if (CitySc->OmniHead) CitySc->OmniHead->Prev = O;
			CitySc->OmniHead = O;
			fprintf(stderr, "[CITY] installed 1 shadow-casting cube omni at (0,600,500)\n");
		}

		// Allocate shadow maps for the spots we just installed (they
		// claim castsShadow=true with shadowMapRes=512). The per-frame
		// shadow-render pass in Render_DeferredShadowMaps will populate
		// them. Required so the cone pass can sample shadow per
		// integration sample → cones get occluded by buildings.
		// CubeShadowMaps_Rebuild handles Light_Omni cube faces (must
		// come AFTER ShadowMaps_Rebuild since both push into the same
		// g_shadowMaps vector).
		if (fds::FeatureFlags::shadows()) {
			ShadowMaps_Rebuild(CitySc, 512);
			CubeShadowMaps_Rebuild(CitySc, 256);
			// Static lights baked here, alongside the reflection bake
			// hidden behind Glato — no demo-start delay. Per-frame
			// Render_DeferredShadowMaps skips Omni_StaticShadow.
			ShadowMaps_BakeStatic(CitySc);
		}
	}

	Setup_Vehicle_Headlights();
}

#ifdef TRACE_OBJECTS
struct debug_outtext {
	dword x, y;
	std::string str;
};

std::vector<debug_outtext> DebugStrs;

#endif

namespace {
struct CityScene : SceneDriver {
	char MSGStr[128];
	int32_t TTrd = 0;
	int32_t timerStack[20] = {};
	int32_t timerIndex = 0;
	dword numFrames = 0;
	FrameProfiler prof{"city"};
	bool pause_mode = false;

	std::vector<int> dispMap;
	std::vector<dword> backBuffer;
	std::vector<uint16_t> zRefl;   // displaced pass-1 Z for reflection fog
	// Per-frame animated ripple: 1D sine terms (cheap; XRes/YRes sin calls)
	// summed cross-axis per pixel to drive dispMap. See updateRippleDispMap.
	std::vector<float> rip_gx1, rip_gx2, rip_gy1, rip_gy2;

	~CityScene() override { Destroy_Scene(CitySc); }

	// Rebuild the per-resolution displacement and back buffers. dispMap
	// stores precomputed source-pixel indices for the water-distort effect;
	// the contents bake the current XRes (row stride), so on resize we have
	// to refill, not just resize.
	void rebuildDispBuffers() {
		dispMap.assign(XRes * YRes, 0);
		int idx = 0;
		for (int y = 0; y < YRes; ++y) {
			for (int x = 0; x < XRes; ++x) {
				int tx = x, ty = y;
				tx += sin(x/30.0) * 15;
				ty += cos(y/50.0) * 15;

				if (tx < 0) { tx = -tx; } if (tx > XRes_1) { tx = 2 * XRes_1 - tx; }
				if (ty < 0) { ty = -ty; } if (ty > YRes_1) { ty = 2 * YRes_1 - ty; }
				dispMap[idx++] = ty * XRes + tx;
			}
		}
		backBuffer.assign(XRes * YRes, 0);
		zRefl.assign(XRes * YRes, 0);
	}

	void on_resize(int /*newX*/, int /*newY*/) override {
		rebuildDispBuffers();
	}

	// Animated organic ripple for the water reflection. Replaces the static
	// baked sin(x)/cos(y) warp in rebuildDispBuffers with a time-varying,
	// cross-axis displacement so the mirrored world below the water actually
	// moves. Cost is kept low by summing two SEPARABLE sine waves per axis
	// (XRes+YRes sin calls), then per pixel just lookups + adds; the per-pixel
	// rebuild is row-parallel across the pool (mirrors parallel_memset). t is
	// derived from Timer, which is pinned while paused → the ripple freezes
	// with everything else. No-op when --no-water_ripple (dispMap stays static).
	void updateRippleDispMap(float timeMs) {
		const float speed = fds::FeatureFlags::water_ripple_speed();
		const float amp   = fds::FeatureFlags::water_ripple_amp();
		const float scale = fds::FeatureFlags::water_bump_scale();   // SHARED field scale → coherent with glints
		// Timer is CENTISECONDS (TimerInit(100) → 100 ticks/sec), not ms — so
		// 0.02 puts the dominant wave (×1.3 below) at ~2.5 rad/s ≈ 0.4 Hz, a
		// visible water undulation. (At 0.001 it was ~0.13 rad/s — one cycle per
		// ~48 s, i.e. frozen to the eye: the "can't see the bump" bug.)
		const float t = timeMs * 0.02f * speed;
		if ((int)dispMap.size() < XRes * YRes) dispMap.assign(XRes * YRes, 0);
		if (!View) return;   // need the camera to ray-cast the water plane (keep the static warp)
		// Ray-cast each pixel to the water plane, evaluate THE shared wave slope
		// (waterWaveSlope) there, and offset the reflection source by it → the
		// reflection ripples to the SAME waves as the glints. Coarse octaves (the
		// mirror blurs fine detail + cheaper). Non-water-plane pixels stay identity.
		const float invFX = (FOVX != 0.0f) ? 1.0f / FOVX : 0.0f;
		const float invFY = (FOVY != 0.0f) ? 1.0f / FOVY : 0.0f;
		const float cex = CntrEX, cey = CntrEY;
		const float m00=View->Mat[0][0], m10=View->Mat[1][0], m20=View->Mat[2][0];
		const float m01=View->Mat[0][1], m11=View->Mat[1][1], m21=View->Mat[2][1];
		const float m02=View->Mat[0][2], m12=View->Mat[1][2], m22=View->Mat[2][2];
		const float ex=View->ISource.x, ey=View->ISource.y, ez=View->ISource.z;
		const float wYplane = g_waterY;
		int* const dm = dispMap.data();
		const int xr1 = XRes_1, yr1 = YRes_1, xr = XRes;
		auto band = [=](int y0, int y1) {
			for (int y = y0; y < y1; ++y) {
				int* row = dm + size_t(y) * size_t(xr);
				for (int x = 0; x < xr; ++x) {
					int tx = x, ty = y;
					const float xn = (float(x) - cex) * invFX;
					const float yn = (cey - float(y)) * invFY;
					const float D = m01*xn + m11*yn + m21;
					if (D != 0.0f) {
						const float sd = (wYplane - ey) / D;     // view-z of the plane hit
						if (sd > 1.0f) {
							const float wx = ex + sd * (m00*xn + m10*yn + m20);
							const float wz = ez + sd * (m02*xn + m12*yn + m22);
							float bnx, bnz;
							pwater::WaveSlope(wx, wz, t, scale, bnx, bnz);   // coarse → coherent with glints
							tx = x + int(amp * bnx);
							ty = y + int(amp * bnz);
						}
					}
					if (tx < 0) tx = -tx; if (tx > xr1) tx = 2 * xr1 - tx;
					if (ty < 0) ty = -ty; if (ty > yr1) ty = 2 * yr1 - ty;
					if (tx < 0) tx = 0; else if (tx > xr1) tx = xr1;   // belt + suspenders
					if (ty < 0) ty = 0; else if (ty > yr1) ty = yr1;
					row[x] = ty * xr + tx;
				}
			}
		};
		auto& tp = ThreadPool::instance();
		const int nThreads = (int)tp.size();
		if (nThreads < 2 || YRes < 64) { band(0, YRes); return; }
		const int chunk = (YRes + nThreads - 1) / nThreads;
		auto remaining = std::make_shared<std::atomic<int>>(0);
		int launched = 0;
		for (int i = 0; i < nThreads; ++i) {
			const int y0 = i * chunk; if (y0 >= YRes) break;
			const int y1 = std::min(y0 + chunk, YRes);
			remaining->fetch_add(1, std::memory_order_relaxed);
			++launched;
			tp.enqueue([band, y0, y1, remaining]() {
				band(y0, y1);
				remaining->fetch_sub(1, std::memory_order_release);
			});
		}
		(void)launched;
		while (remaining->load(std::memory_order_acquire) != 0) std::this_thread::yield();
	}

	void init() override {
		SetCurrentScene(CitySc);
		Calibrate_FreeCamera_ForScene(CitySc->FZP, CitySc->CameraHead);

		// get the reflective water surface object
		Reflective_Mapper_Setup();
		Setup_Water_Distort();
		pwater::BuildField();    // procedural water field + caustic texture (shared with chase)

		setupFaceLists(CitySc, /*includeOmnisInCount=*/true);

		for(int32_t i = 0; i < 20; i++)
			timerStack[i] = Timer;

		g_FrameTime = TTrd = Timer;

		Ambient_Factor = 0.25;
		Diffusive_Factor = 1.0;
		// Forward Lighting() ignores Specular_Factor on its active path, so
		// raising it from the legacy 0.0 to 1.0 is a no-op for forward.
		// Deferred uses it as the per-scene specular gate; the City has
		// many materials with authored Mat->Specular and we want them
		// to draw highlights (windows glass, ship hulls, etc.).
		Specular_Factor = 1.0;
		Range_Factor = 1.3;

		ImageSize = 200;

		// SYNC music: set order #7
		Modplayer_SetOrder(g_RevModuleHandle, 7);

		g_renderedPolys = 0;
		numDropsRendered = 0;
		FillerPixelcount = 0;

		rebuildDispBuffers();
	}

	bool tick() override {
		// ESC is now "exit demo" (handled by runSceneBlocking via
		// g_shouldQuit); Backspace is "skip to next scene".
		if (Keyboard[ScBackSpace] || Timer >= CTPartTime) return false;

		numFrames++;

		prof.beginFrame();
		prof.enter(PROF_ZCLR);

		clearFrame();

		prof.switchTo(PROF_ANIM);

		// Shared transport: pause (P/U), fast-forward/rewind (F2/F1), and
		// the race-free scene-time snapshot. dTime (set here) stays the
		// wallclock delta the free camera / water scroll rely on in pause.
		tickSceneTimer(TTrd, pause_mode);
		CurFrame = CitySc->StartFrame + (CitySc->EndFrame - CitySc->StartFrame) * (float)g_FrameTime / (float)CTPartTime;
		tickTabToggle(CitySc, "city");
		Dynamic_Camera();

		// SkyCube uses the main camera; render it AFTER Dynamic_Camera
		// so it tracks the current frame's view (previously was rendered
		// one frame stale because it ran before camera update).
		// (VPage is already cleared by clearFrame() at tick start, so the
		// mirrored-sky pixels pass-1 leaves uncovered are black+skybox —
		// never stale content.)
		prof.switchTo(PROF_SKY);
		RenderSkyCube(SkySc, View, false);
		prof.switchTo(PROF_ANIM);

		// Water UV scroll advances by dTime, which stays NONZERO during
		// pause (dTime is wallclock elapsed, computed before the pause
		// branch pins Timer — the free camera needs it to keep flying).
		// Gate the scroll itself so a paused frame is a fixed state and
		// consecutive captures isolate temporal effects (fog jitter).
		if (!pause_mode)
			Reflective_AnimateTexture();

		// Animate the water-reflection displacement (cross-axis travelling
		// ripples). t = g_FrameTime (the frozen/scrubbed scene time, so the
		// ripple freezes cleanly while paused — the raw atomic would drift).
		// Consumed below at the pass-1 dispMap resample; --no-water_ripple
		// leaves the static warp baked by rebuildDispBuffers untouched.
		if (fds::FeatureFlags::water_ripple())
			updateRippleDispMap((float)g_FrameTime);

		if (Keyboard[ScC]) { FC.ISource = View->ISource; Matrix_Copy(FC.Mat, View->Mat); FC.IFOV = View->IFOV; }

		// 'I' = info: print camera position + forward vector to stderr
		// (rising edge only). Forward = row 2 of View->Mat (row-major,
		// camera-z basis = look direction in world space).
		{
			static bool prevI = false;
			bool curI = Keyboard[ScI] != 0;
			if (curI && !prevI) {
				fprintf(stderr,
					"[CAM] pos=(%.0f, %.0f, %.0f)  fwd=(%.3f, %.3f, %.3f)  IFOV=%.1f\n",
					View->ISource.x, View->ISource.y, View->ISource.z,
					View->Mat[2][0], View->Mat[2][1], View->Mat[2][2],
					View->IFOV);
			}
			prevI = curI;
		}


		Animate_Objects(CitySc, View);
		Update_Vehicle_Headlights();

		((TriMesh*)(RflObj->Data))->Flags &= 0xFFFFFFFF - HTrack_Visible;

		prof.switchTo(PROF_LGHT);

		Lighting(CitySc);

		if (fds::FeatureFlags::city_rain()) {
			// Bill rain to ANIM (it's particle animation, and ANIM is
			// otherwise ~0 in city). dt=0 while paused: drops freeze
			// mid-air like everything else (water-scroll precedent).
			prof.switchTo(PROF_ANIM);
			riskOfRain(CitySc, pause_mode ? 0.0f : 0.01f * dTime);
		}
		prof.switchTo(PROF_XFRM);

		// render reflection of all objects except water
		Reflected_Transform(CitySc);

		prof.leave(PROF_XFRM);

		if (CAll) {
			prof.enter(PROF_SORT);

			Radix_Sort(FList, SList, CAll);

			prof.switchTo(PROF_RNDR);

			// Per-frame shadow-map depth pre-pass for Omni_CastsShadow
			// lights (currently the test streetlights when
			// FDS_CITY_TEST_SPOTS=1). No-op when no spot has the flag.
			// Must run before Render() since it swaps view globals.
			Render_DeferredShadowMaps(CitySc, ShadowBakeMode::DynamicOmnisPerFrame);
			// Re-bake just the moving meshes into the dynamic shadow
			// buffer of each STATIC omni (no-op when --shadow-dynamic
			// is off). Lighting kernel samples min(static, dynamic).
			if (fds::FeatureFlags::shadow_dynamic()) {
				Render_DeferredShadowMaps(CitySc, ShadowBakeMode::DynamicMeshesPerFrame);
			}
			// skipVolumetric=true — the pass-1 output is consumed by the
			// dispMap distortion below, so volumetric work (cones/halos)
			// here is wasted CPU (≈60ms/frame on the cube omni halo).
			Render(RenderPath::Default, /*skipVolumetric=*/true);
			{
				memcpy(backBuffer.data(), VPage, PageSize);
				dword* ptr = (dword*)VPage;
				uint16_t* zbuffer_ptr = ZPage16;
				// Capture the pass-1 Z through the SAME displacement the
				// color takes: the froxel composite fogs the water's
				// reflection underlay at this mirrored path length, giving
				// the mirror the same fog gradient as the world above
				// (|mirrorCam→building| = camera→water→building).
				uint16_t* zr = zRefl.data();
				size_t idx = 0;
				for (auto displacement : dispMap) {
					if (*zbuffer_ptr++ != 0) {
						*ptr = backBuffer[displacement];
						zr[idx] = ZPage16[displacement];
					} else {
						zr[idx] = 0;
					}
					ptr++; ++idx;
				}
				FastFog_SetReflectionZ(zr, -RflSurfOfs);   // plane: N=(0,1,0), y = -ofs
			}

			// Deferred path needs Z cleared between passes so the pass-2
			// deferred-lighting compositor doesn't re-shade pass-1 pixels
			// with their (now stale) G-buffer entries — that would
			// undo the dispMap distortion that just landed in VPage.
			// Forward path keeps the pass-1 Z so its rasterizer can do
			// proper depth occlusion against the mirrored world; we don't
			// have that overlap problem because deferred shading is the
			// only consumer of Z and we want pass-2 to write only on
			// pixels actually covered by normal-world geometry.
			if (deferredEnabled()) {
				parallel_memset(ZPage16, 0, XRes * YRes * sizeof(word));
			}

			prof.leave(PROF_RNDR);
		}

		// render all objects normally
		prof.enter(PROF_XFRM);
		((TriMesh*)(RflObj->Data))->Flags |= HTrack_Visible;
		Transform_Objects(CitySc, fds::g_mainCamera, fds::g_mainFaces);
		prof.leave(PROF_XFRM);
		if (!CAll) return true;
		prof.enter(PROF_SORT);
		Radix_Sort(FList, SList, CAll);
		prof.switchTo(PROF_RNDR);
		Render();
		// Bump-mapped specular glints on the water (post-tonemap, on the final
		// image). zRefl marks the water-reflection pixels from the pass-1 resample.
		if (fds::FeatureFlags::water_bump())
			pwater::RenderGlints(g_waterY, g_waterMinX, g_waterMaxX, g_waterMinZ, g_waterMaxZ);
		// Note: volumetric spotlight cones run inside renderFrame's
		// post-lighting passes (Render_VolumetricCones), so no per-scene
		// hook needed here.

		prof.leave(PROF_RNDR);
		// Deferred sky cost timed inside Render_DeferredSkybox; attribute
		// it back to PROF_SKY (which would otherwise show 0 ms while
		// --deferred-skybox is on, since RenderSkyCube early-returns).
		prof.addExternalNs(PROF_SKY, DeferredSkybox_TakeFrameNs());

		// FPS printer — use the profiler's mean frame total (steady_clock,
		// us-resolution) so the on-screen number agrees with TOTL/mean_fps
		// in the scene-end dump.
		// Always show a tiny frame indicator (top-left, single line) so
		// the user can pinpoint a Timer value to feed --snapshot=city@t=N
		// when reproducing a visual issue. Cheap; small; doesn't need
		// the full profiler overlay.
		{
			snprintf(MSGStr, sizeof(MSGStr), "t=%d  CurFrame=%.1f", (int)Timer, CurFrame);
			OutTextXY(VPage, 0, 0, MSGStr, 255);
		}

		if (g_profilerActive) {
			int scroll = 15 * g_fontScale;  // start below the always-on indicator
			float meanMs = prof.meanFrameMs();
			float fps = meanMs > 0 ? 1000.0f / meanMs : 0.0f;
			snprintf(MSGStr, sizeof(MSGStr), "%f FPS", fps);
			scroll = OutTextXY(VPage, 0, scroll, MSGStr, 255);

			snprintf(MSGStr, sizeof(MSGStr), "%f frame", CurFrame);
			scroll = OutTextXY(VPage, 0, scroll + 15 * g_fontScale, MSGStr, 255);

			fds::RenderStats_Flush();
			prof.noteCounters((double)g_renderedPolys, (double)FillerPixelcount);
			scroll = prof.drawOverlay(scroll);
			// (clipper bucket breakdown removed from the overlay — the
			// counters still exist in fds_stats if an investigation
			// needs them; greets dropped its line for the same reason.)
		}
		// 'G' (grab) — same as greets/conetest: dump THIS completed frame
		// (VPage right before Flip) to /tmp/city_dump_N.ppm and print the
		// pose in the exact CITYSNAP_VIEW format, so a failing view feeds
		// the offline ripple repro verbatim. Edge-triggered; press twice
		// while paused for a consecutive-frame pair. (F9 is a macOS system
		// key, hence 'G'.)
		{
			static bool sGPrev = false;
			static int  sDumpN = 0;
			const bool gNow = Keyboard[ScG] != 0 || Keyboard[ScF9] != 0;
			if (gNow && !sGPrev) {
				char path[64];
				snprintf(path, sizeof(path), "/tmp/city_dump_%d.ppm", sDumpN++);
				std::FILE *f = std::fopen(path, "wb");
				if (f) {
					const int xr = (int)XRes, yr = (int)YRes;
					std::fprintf(f, "P6\n%d %d\n255\n", xr, yr);
					std::vector<unsigned char> row(xr * 3);
					for (int y = 0; y < yr; ++y) {
						const dword *src = (const dword*)((const byte*)VPage
						                  + y * (int)VESA_BPSL);
						for (int x = 0; x < xr; ++x) {
							dword px = src[x];
							row[x*3+0] = (px >> 16) & 0xFF;
							row[x*3+1] = (px >>  8) & 0xFF;
							row[x*3+2] = (px      ) & 0xFF;
						}
						std::fwrite(row.data(), 1, row.size(), f);
					}
					std::fclose(f);
					fprintf(stderr, "[CITY] dumped %s (t=%d)\n", path, (int)Timer);
				}
				fprintf(stderr,
					"[CITY-CAM] CITYSNAP_VIEW=\"%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.1f\"\n",
					View->ISource.x, View->ISource.y, View->ISource.z,
					View->Mat[2][0], View->Mat[2][1], View->Mat[2][2],
					View->IFOV);
				std::fflush(stderr);
			}
			sGPrev = gNow;
		}
		prof.enter(PROF_FLIP);

#ifdef TRACE_OBJECTS
		for (const auto& str : DebugStrs) {
			OutTextXY(VPage, str.x, str.y, str.str.c_str(), 255);
		}
		DebugStrs.clear();
#endif

		Flip(MainSurf);
		prof.leave(PROF_FLIP);
		prof.endFrame();
		return true;
	}

	void cleanup() override {
		if (g_profilerActive) prof.dump();
		Timer -= CTPartTime;
		waitBackspaceRelease();
		if (Timer < 0) Timer = 0;
	}
};
} // anonymous namespace

std::unique_ptr<SceneDriver> createCityScene()
{
	// Bright daylit water mirror → low exposure so the reflection rolls off
	// instead of blowing out.
	// Procedural caustic water on by default (the scene the feature was built
	// for), independent of --cinematic. --no-water_procedural still disables it.
	fds::FeatureFlags::setDefault(fds::FeatureFlags::BoolId::water_procedural, true);
	ApplyCinematicProfile(cine::kCity);          // storm-city look (fog/rain/post)
	return std::make_unique<CityScene>();
}

int32_t getCityCTPartTime()
{
	return CTPartTime;
}

Scene* getCityScene()
{
	return CitySc;
}

void Run_City()
{
	auto scene = createCityScene();
	runSceneBlocking(*scene);
}
