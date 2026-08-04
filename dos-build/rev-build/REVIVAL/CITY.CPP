
#include "Rev.H"

#include "City.H"
extern "C" long g_TriIn, g_TriDraw;   // fused-mapper triangle accept/reject census
extern "C" long g_ClipFaces, g_ClipTris, g_DirectTris, g_ScreenPolys;
extern "C" unsigned long g_LdTexCy, g_LdSwzCy, g_LdTexN;  // startup load profile
extern "C" unsigned long g_XparTexCy, g_XparLitCy, g_XparN;   // 2-pass xpar split
extern "C" unsigned long g_LdColdCy, g_LdWarmCy, g_LdDetCy, g_LdJpgCy,
                         g_LdSwapCy, g_LdScaleCy, g_LdBytes;   // A2 breakdown  // clipper fan-out census (see FRUSTRUM.CPP)

extern "C" unsigned long RDTSC(void);   // low 32 bits of the cycle counter (per-pass profiling)
#pragma aux RDTSC = 0x0F 0x31 value [eax] modify [eax edx];

// Wrap-correct cycle delta, one conversion site for the whole profiler. See the note at the
// accumulators in Run_City: converting (double)(unsigned long)(t1-t0) inline was unstable.
static double TscDelta(unsigned long t0,unsigned long t1)
{
  unsigned long d = t1 - t0;            // modular: correct across a low-32 wrap
  return (double)d;
}

// Static-lighting bake: City buildings + all city omnis are stationary, so a static mesh's
// per-vertex light is frame-invariant. Bake it into Vtx->LR/LG/LB on the first City_Lighting
// pass, then skip re-lighting those meshes (lighting is view-independent here, so the same
// values serve both the main and the reflection pass). Set to 0 for A/B validation.
#define CITY_LBAKE 1

// Frame-lock Timer to a fixed step/frame so a run is deterministic regardless of render
// speed (the loop is normally wall-clock/Timer gated, so a faster build lands at a different
// CurFrame for the same render-count). Enables byte-exact A/B snapshot validation. 0 = normal.
#define CITY_DETERMINISTIC 0

// In-process bake correctness check (city renders non-deterministically run-to-run — the
// uninitialised-FlareSize reflection coin-flip — so cross-run PPM A/B is invalid). On one late
// frame, recompute each baked static mesh's lighting fresh and compare to the retained baked
// Vtx->LR/LG/LB; any mismatch means the mesh isn't truly frame-invariant. 0 = off.
// PROVEN: mismatch=0/6034 verts, maxDelta=0 at frame 300 (2026-07-11) — bake is byte-exact.
#define CITY_LBAKE_VERIFY 0

// Master lighting model: the DOS city omnis ship ISize=0 / no Range spline, so the legacy
// inverse-square omni term is DEAD (city is ambient-only lit). Wire master's values (per-omni
// Range/ISize by colour) + master's LINEAR range-window falloff  k=(N·L)(1-d/Range), hard
// cutoff at d>=Range. Raw 0-255 colour (DOS L is pre-scaled x56.86 for the dead path). 0=legacy.
#define CITY_MASTER_LIGHT 1
#define LSCALE_INV (1.0f/56.86f)   // recover raw 0-255 colour from the x56.86-scaled Omni L
#define CITY_LIGHT_GAIN 1.7f       // overall brightness of the master omni contribution (tune by eye)

static Scene *CitySc;
static Object *RflObj;
static Vector RflSurfNorm;
static float RflSurfOfs;
static float WaterBaseUV[8][6];   // base planar UVs of the water faces (for the ripple)
static long  WaterFaceN=0;

static long CTPartTime = 40.00 * 100;


signed char *STbl,*SOTbl,*CTbl,*COTbl;

void Calc_STable()
{
	STbl = new signed char [15*XRes];
	SOTbl = STbl + 5*XRes;
	long I;
	for(I=-5*XRes;I<10*XRes;I++)
		SOTbl[I] = (long) (1.5f * sin (I * 0.01f));
}

void Calc_CTable()
{
	CTbl = new signed char [13*XRes];
	COTbl = CTbl + 3*XRes;
	long I;
	for(I=-3*XRes;I<10*XRes;I++)
		COTbl[I] = (long) (3.0f * cos (I * 0.01f));
//	COTbl = SOTbl + 157;
}

DWord *DistBuf;

void Init_Distort(void)
{
	Calc_STable();
	Calc_CTable();
	DistBuf = new DWord[XRes*YRes];
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
	long I,T;

	long X,Y,hX = XRes>>1,hY = YRes>>1;
//	long Frames = 0;
//	Timer = 0;
//	while(!Keyboard[ScESC])
//	{
		memcpy(DistBuf,VPage,PageSize);

		DWord *DPtr = DistBuf;
		DWord *Ptr = (DWord *)VPage;

		I = 0;
		T = (Timer*3>>1)%628;
		// The COTbl/SOTbl indices are linear in X — (T+2X-Y)<<1 and 2T+3X+5Y — so strength-reduce
		// them to per-pixel adds (coIdx += 4, soIdx += 3) instead of recomputing per pixel. Byte-exact.
		{
			long T2 = T<<1;
			for(Y=-hY;Y<hY-1;Y++)
			{
				long coIdx = (T + ((-hX)<<1) - Y) << 1;   // (T + 2X - Y)<<1 at X=-hX
				long soIdx = T2 + 3*(-hX) + 5*Y;          // 2T + 3X + 5Y  at X=-hX
				long yTerm = Y>>3;
				for(X=-hX;X<hX;X++)
				{
					*Ptr++ = DPtr[I + COTbl[coIdx] - (X>>3) + XRes*(SOTbl[soIdx] - yTerm)];
					I++; coIdx += 4; soIdx += 3;
				}
			}
		}
		memcpy(Ptr,DPtr+XRes*YRes_1,VESA_BPSL);
				//(long)(cos(T*0.02+X*0.04-Y*0.02) * 5.0f)
				//(long)(sin(T*0.02+X*0.03+Y*0.05) * 5.0f) - (long)(Y * 0.1)
//		Frames++;
//	}
//	printf("FPS = %1.3f\n",100.0f*(float)Frames/(float)Timer);
}



void Initialize_City()
{
	static long _ict;
#define ICT(tag) {fprintf(LogFile,"IC-TIME %s: %ld.%02lds\n",tag,(long)(Timer-_ict)/100,(long)(Timer-_ict)%100);fflush(LogFile);\
  {FILE *_p=fopen("PERF.TXT","at"); if(_p){fprintf(_p,"IC-TIME %s: %ld.%02lds\n",tag,(long)(Timer-_ict)/100,(long)(Timer-_ict)%100);fclose(_p);}}\
  _ict=Timer;}
	_ict = Timer;
	CitySc = new Scene; memset(CitySc,0,sizeof(Scene));
	LoadFLD(CitySc,"Scenes\\CITY.FLD");
	ICT("LoadFLD(parse+meshes)")
	CTPartTime = 100.0f*(CitySc->EndFrame-CitySc->StartFrame)/25.0f;
	Preprocess_Scene(CitySc);
	ICT("Preprocess(JPEG decode+convert+geometry)")
	CitySc->FZP = 3000.0f;
	CitySc->NZP = 0.1f;     // master DEMO/CITY.CPP:1864 uses 20; we go closer so the robot
	                        // isn't sliced. 0.1 is the closest VERIFIED-safe value here: a
	                        // near-clip vertex gets PX = TPos.X/NZP and city's water quad has
	                        // |TPos.X| ~2e7, so 0.01 pushed PX to ~2e9 and the screen-edge clip
	                        // lost the left ~32 columns of the transparent water (f200 probe).
	CitySc->Flags |= Scn_Fogged;
	Init_Distort();
	ICT("Init_Distort")
#ifdef FDS_TEX_SWIZZLE
	Scene_SwizzleTextures(CitySc); // one-shot: after all init-time texture processing
	ICT("SwizzleTextures")
#endif
}

void Reflective_Surface()
{
	TriMesh *T;
	Face *F,*FE;
	Vector U,V;

	Object *O;
	long IID = 0;
	for(O = CitySc->ObjectHead;O;O=O->Next)
	{
		if (O->Type!=Obj_TriMesh) continue;
//		printf("[%d] Trimesh %s with %d Polygons\n",IID,O->Name,((TriMesh *)O->Data)->FIndex);
		if (!strcmp(O->Name,"water.lwo"))
		{
			RflObj = O;
			T = (TriMesh *)(O->Data);
			F = T->Faces;
			Vector_Sub(&F->B->Pos,&F->A->Pos,&U);
			Vector_Sub(&F->C->Pos,&F->A->Pos,&V);
			Cross_Product(&U,&V,&RflSurfNorm);
			Vector_Norm(&RflSurfNorm);
			RflSurfOfs =-Dot_Product(&RflSurfNorm,&F->A->Pos);
			FE = F+T->FIndex;
			// capture base planar UVs so we can animate a per-vertex sin/cos ripple
			// over them each frame (the shipped water rippled its UVs in the xform).
			WaterFaceN = (T->FIndex<8)?T->FIndex:8;
			{ Face *_f=T->Faces; long _k; for(_k=0;_k<WaterFaceN;_k++,_f++){
				WaterBaseUV[_k][0]=_f->U1; WaterBaseUV[_k][1]=_f->V1;
				WaterBaseUV[_k][2]=_f->U2; WaterBaseUV[_k][3]=_f->V2;
				WaterBaseUV[_k][4]=_f->U3; WaterBaseUV[_k][5]=_f->V3; } }
			for(;F<FE;F++)
				F->Filler = &The_BiGTTrue;   // 2-pass: transparent texture + transparent Gouraud light

			break;
		}
		IID++;
	}
	Omni *Om;
	for(Om=CitySc->OmniHead;Om;Om=Om->Next)
	{
//		printf("City Flare Range was %f\n",Om->Range.Keys[0].Pos.X);
		Om->Range.Keys[0].Pos.X *= 10.0f;
	}
}


///////////////////////////////////////////////////////////////////////
// PRIMARY TRANSFORMATION PIPELINE, FOR USE WITH REFLECTIONS
void Reflected_Transform(Scene *Sc)
{
	TriMesh *T;
	Omni *O;
	Matrix M,IM;
	float M34[3][4];
	Vector AP,S,P,U,V,*W=(Vector *)M,*W2=(Vector *)IM,*Scl;
	float L1,L2,L3;
	Vertex *Vtx,*VEnd;
	Face *F,*FEnd;
	float PX=FOVX,PY=FOVY,Temp;
	float dz;
	long *pdz = (long *)(&dz);
	long I;
	Face **Ins = FList;
	float *f = (float *)(&M);
	float *fv;

	TriMesh *TR;

	TR = (TriMesh *)(RflObj->Data);

	for (T=Sc->TriMeshHead;T;T=T->Next)
	{
		if (T==TR) {T->Flags|=Tri_Invisible; continue;}

		if (!(T->Flags&HTrack_Visible)) {T->Flags|=Tri_Invisible; continue;}
		Matrix_Copy(IM,T->RotMat);
		//Vector_SelfScale(W2+1,-1.0f);
		IM[0][1] *= -1.0f; IM[1][1] *= -1.0f; IM[2][1] *= -1.0f;
		MatrixXMatrix(View->Mat,IM,M);
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
		// mirror T->IPos across the real water plane (file-scope RflSurfNorm/RflSurfOfs
		// from Reflective_Surface). The local statics here shadowed those with a zero
		// normal, so moving objects above the water reflected as in-place duplicates.
		L1 = (-2.0f)*(Dot_Product(&T->IPos,&RflSurfNorm) + RflSurfOfs);
		Vector_Scale(&RflSurfNorm,L1,&P);
		Vector_SelfAdd(&P,&T->IPos);

		Vector_Sub(&P,&View->ISource,&U);
		MatrixXVector(View->Mat,&U,&S);
		V.X = CntrEX*S.Z+PX*S.X;
		V.Y = CntrEY*S.Z-PY*S.Y;
		V.Z = S.Z;
		// make a corrected sphere center vector
		MatrixXVector(IM,&T->BSphereCtr,&AP);
		Vector_SelfAdd(&S,&AP);

//    Vector_Copy(&V,&S);
		// ready
		// 4x3 AFFINE XFORM
		M34[0][0] = M[0][0]; M34[0][1] = M[0][1]; M34[0][2] = M[0][2]; M34[0][3] = V.X;
		M34[1][0] = M[1][0]; M34[1][1] = M[1][1]; M34[1][2] = M[1][2]; M34[1][3] = V.Y;
		M34[2][0] = M[2][0]; M34[2][1] = M[2][1]; M34[2][2] = M[2][2]; M34[2][3] = V.Z;
		// ready


		// aprioric Offset Vector.
		MatrixTXVector(T->RotMat,&U,&AP);
		// ready
		// Bounding Sphere Elimination test Begins.
		W2 = (Vector *)(&T->RotMat);
		L2 = Dot_Product(W2,W2);
		if ((L1 = Dot_Product(W2+1,W2+1))>L2) L2=L1;
		if ((L1 = Dot_Product(W2+2,W2+2))>L2) L2=L1;

		T->Flags&=0xFFFFFFFF-Tri_Invisible-Tri_Ahead-Tri_Inside;
		// Out by depth
		if (S.Z*S.Z>L2*T->BSphereRad)
		{
			if (S.Z<0)
			{
				T->Flags |= Tri_Invisible;
				continue;
			}
			T->Flags |= Tri_Ahead;
		}

		dz = S.Z - Sc->FZP;
		if (dz*dz>L2*T->BSphereRad)
		{
			if (dz>0.0f)
			{
				T->Flags |= Tri_Invisible;
				continue;
			}
		}
		// Out by left/right
		S.X=fabs(S.X);
		L1 = PX*S.X - CntrEX*S.Z;
		if (L1*L1>L2*T->BSphereRad*(PX*PX+CntrEX*CntrEX))
		{
			if (S.X*PX>S.Z*CntrEX)
			{
				T->Flags |= Tri_Invisible;
        continue;
			}
      if (T->Flags&Tri_Ahead) T->Flags |= Tri_Inside;
		}
    // Out by up/down
    S.Y = fabs(S.Y);
    L1 = PY*S.Y - CntrEY*S.Z;
    if (L1*L1>L2*T->BSphereRad*(PY*PY+CntrEY*CntrEY))
    {
			if (S.Y*PY>S.Z*CntrEY)
			{
				T->Flags |= Tri_Invisible;
        continue;
			}
		} else T->Flags&=0xFFFFFFFF-Tri_Inside;
    VEnd=T->Verts+T->VIndex;

/*    FEnd=T->Face+T->NumOfFaces;
		for (F=T->Face;F<FEnd;F++)
			if (!(F->Txtr->Flags&Mat_TwoSided))
        F->Flags = (AP.X*F->N.X + AP.Y*F->N.Y + AP.Z*F->N.Z>=F->NormProd);*/


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
				Vtx->TPos.X = M34[0][0]*Vtx->Pos.X+M34[0][1]*Vtx->Pos.Y+M34[0][2]*Vtx->Pos.Z+M34[0][3];
				Vtx->TPos.Y = M34[1][0]*Vtx->Pos.X+M34[1][1]*Vtx->Pos.Y+M34[1][2]*Vtx->Pos.Z+M34[1][3];
				Vtx->TPos.Z = M34[2][0]*Vtx->Pos.X+M34[2][1]*Vtx->Pos.Y+M34[2][2]*Vtx->Pos.Z+M34[2][3];

				Vtx->Flags&=0xFFFFFFFF-Vtx_Visible;
				Vtx->RZ=1.0/Vtx->TPos.Z;
				Vtx->PX=Vtx->TPos.X*Vtx->RZ;
				Vtx->PY=Vtx->TPos.Y*Vtx->RZ;
//        Vtx->PX=CntrEX+PX*Vtx->TPos.X*Vtx->RZ;
//        Vtx->PY=CntrEY-PY*Vtx->TPos.Y*Vtx->RZ;
//        Vtx->RU=Vtx->U*Vtx->RZ;
//        Vtx->RV=Vtx->V*Vtx->RZ;
				if (Vtx->TPos.Z>Sc->FZP) Vtx->Flags|=Vtx_VisFar;
			}

			goto OUT;
			// This is in case 100% of trimesh AHEAD of camera. this saves some chks
			Ahead://Vertex_Loop1(T->Vertex,VEnd,M,&V);
			for (Vtx=T->Verts;Vtx<VEnd;Vtx++)
			{
	//    if (!Vtx->FRem) continue;
//        MatrixXVector(M,&Vtx->Pos,&U);
//        Vector_Add(&U,&V,&Vtx->TPos);
				Vtx->TPos.X = M34[0][0]*Vtx->Pos.X+M34[0][1]*Vtx->Pos.Y+M34[0][2]*Vtx->Pos.Z+M34[0][3];
				Vtx->TPos.Y = M34[1][0]*Vtx->Pos.X+M34[1][1]*Vtx->Pos.Y+M34[1][2]*Vtx->Pos.Z+M34[1][3];
        Vtx->TPos.Z = M34[2][0]*Vtx->Pos.X+M34[2][1]*Vtx->Pos.Y+M34[2][2]*Vtx->Pos.Z+M34[2][3];

        Vtx->RZ=1.0/Vtx->TPos.Z;
				Vtx->PX=Vtx->TPos.X*Vtx->RZ;
				Vtx->PY=Vtx->TPos.Y*Vtx->RZ;
//        Vtx->PX=CntrEX+PX*Vtx->TPos.X*Vtx->RZ;
//        Vtx->PY=CntrEY-PY*Vtx->TPos.Y*Vtx->RZ;
//        Vtx->RU=Vtx->U*Vtx->RZ;
//        Vtx->RV=Vtx->V*Vtx->RZ;
				Vtx->Flags&=0xFFFFFFFF-Vtx_Visible;
				if (Vtx->PX<0) Vtx->Flags|=Vtx_VisLeft;
				if (Vtx->PX>XRes_1) Vtx->Flags|=Vtx_VisRight;
				if (Vtx->PY<0) Vtx->Flags|=Vtx_VisUp;
				if (Vtx->PY>YRes_1) Vtx->Flags|=Vtx_VisDown;
				if (Vtx->TPos.Z>Sc->FZP) Vtx->Flags|=Vtx_VisFar;
			}
	//    printf("Ahead VGA/Wizard.\n");
      goto OUT;
      Regular:
      for (Vtx=T->Verts;Vtx<VEnd;Vtx++)
      {
  //    if (!Vtx->FRem) continue;
//        MatrixXVector(M,&Vtx->Pos,&U);
//        Vector_Add(&U,&V,&Vtx->TPos);
				Vtx->TPos.X = M34[0][0]*Vtx->Pos.X+M34[0][1]*Vtx->Pos.Y+M34[0][2]*Vtx->Pos.Z+M34[0][3];
        Vtx->TPos.Y = M34[1][0]*Vtx->Pos.X+M34[1][1]*Vtx->Pos.Y+M34[1][2]*Vtx->Pos.Z+M34[1][3];
				Vtx->TPos.Z = M34[2][0]*Vtx->Pos.X+M34[2][1]*Vtx->Pos.Y+M34[2][2]*Vtx->Pos.Z+M34[2][3];

        Vtx->Flags&=0xFFFFFFFF-Vtx_Visible;
  //      if (*(long *)(&Vtx->TPos.Z)>0x3F800000) // 1.0 in floating point rep.
				if (Vtx->TPos.Z>C_NZP)
				{
					Vtx->RZ=1.0/Vtx->TPos.Z;
					Vtx->PX=Vtx->TPos.X*Vtx->RZ;
					Vtx->PY=Vtx->TPos.Y*Vtx->RZ;
//          Vtx->PX=CntrEX+PX*Vtx->TPos.X*Vtx->RZ;
//          Vtx->PY=CntrEY-PY*Vtx->TPos.Y*Vtx->RZ;
//          Vtx->RU=Vtx->U*Vtx->RZ;
//          Vtx->RV=Vtx->V*Vtx->RZ;
					if (Vtx->PX<0) Vtx->Flags|=Vtx_VisLeft;
					if (Vtx->PX>XRes_1) Vtx->Flags|=Vtx_VisRight;
					if (Vtx->PY<0) Vtx->Flags|=Vtx_VisUp;
					if (Vtx->PY>YRes_1) Vtx->Flags|=Vtx_VisDown;
				} else Vtx->Flags|=Vtx_VisNear;
				if (Vtx->TPos.Z>Sc->FZP) Vtx->Flags|=Vtx_VisFar;
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
				Vtx->RZ=1.0/Vtx->TPos.Z;
        Vtx->PX=Vtx->TPos.X*Vtx->RZ;
        Vtx->PY=Vtx->TPos.Y*Vtx->RZ;
//        Vtx->PX=CntrEX+PX*Vtx->TPos.X*Vtx->RZ;
//        Vtx->PY=CntrEY-PY*Vtx->TPos.Y*Vtx->RZ;
        Vtx->EU=128.0+127.0*(Vtx->N.X*IM[0][0]+Vtx->N.Y*IM[0][1]+Vtx->N.Z*IM[0][2]);
				Vtx->REU=Vtx->EU*Vtx->RZ;
        Vtx->EV=128.0+127.0*(Vtx->N.X*IM[1][0]+Vtx->N.Y*IM[1][1]+Vtx->N.Z*IM[1][2]);
				Vtx->REV=Vtx->EV*Vtx->RZ;
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

        Vtx->RZ=1.0/Vtx->TPos.Z;
        Vtx->EU=128.0+127.0*(Vtx->N.X*IM[0][0]+Vtx->N.Y*IM[0][1]+Vtx->N.Z*IM[0][2]);
				Vtx->REU=Vtx->EU*Vtx->RZ;
				Vtx->EV=128.0+127.0*(Vtx->N.X*IM[1][0]+Vtx->N.Y*IM[1][1]+Vtx->N.Z*IM[1][2]);
				Vtx->REV=Vtx->EV*Vtx->RZ;

        Vtx->PX=Vtx->TPos.X*Vtx->RZ;
				Vtx->PY=Vtx->TPos.Y*Vtx->RZ;
//        Vtx->PX=CntrEX+PX*Vtx->TPos.X*Vtx->RZ;
//        Vtx->PY=CntrEY-PY*Vtx->TPos.Y*Vtx->RZ;
//        Vtx->RU=Vtx->U*Vtx->RZ;
//        Vtx->RV=Vtx->V*Vtx->RZ;
        if (Vtx->PX<0) Vtx->Flags=Vtx_VisLeft; else Vtx->Flags=0;
        if (Vtx->PX>XRes_1) Vtx->Flags+=Vtx_VisRight;
				if (Vtx->PY<0) Vtx->Flags+=Vtx_VisUp;
        if (Vtx->PY>YRes_1) Vtx->Flags+=Vtx_VisDown;
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
	//      if (*(long *)(&Vtx->TPos.Z)>0x3F800000) // 1.0 in floating point rep.
				Vtx->EU=128.0+127.0*(Vtx->N.X*IM[0][0]+Vtx->N.Y*IM[0][1]+Vtx->N.Z*IM[0][2]);
        Vtx->EV=128.0+127.0*(Vtx->N.X*IM[1][0]+Vtx->N.Y*IM[1][1]+Vtx->N.Z*IM[1][2]);

        if (Vtx->TPos.Z>C_NZP)
        {
          Vtx->RZ=1.0/Vtx->TPos.Z;
					Vtx->PX=Vtx->TPos.X*Vtx->RZ;
          Vtx->PY=Vtx->TPos.Y*Vtx->RZ;
//          Vtx->PX=CntrEX+PX*Vtx->TPos.X*Vtx->RZ;
//          Vtx->PY=CntrEY-PY*Vtx->TPos.Y*Vtx->RZ;
//          Vtx->RU=Vtx->U*Vtx->RZ;
//          Vtx->RV=Vtx->V*Vtx->RZ;
          Vtx->REU=Vtx->EU*Vtx->RZ;
          Vtx->REV=Vtx->EV*Vtx->RZ;
					if (Vtx->PX<0) Vtx->Flags=Vtx_VisLeft;
					if (Vtx->PX>XRes_1) Vtx->Flags+=Vtx_VisRight;
          if (Vtx->PY<0) Vtx->Flags+=Vtx_VisUp;
					if (Vtx->PY>YRes_1) Vtx->Flags+=Vtx_VisDown;
        } else Vtx->Flags=Vtx_VisNear;
	//      printf("Regular shit!\n");
			}

    }
    OUT:FEnd=T->Faces+T->FIndex;
    for (F=T->Faces;F<FEnd;F++)
      if ((!(F->A->Flags&F->B->Flags&F->C->Flags&Vtx_Visible))
					 &&((F->Txtr->Flags&Mat_TwoSided)||(AP.X*F->N.X + AP.Y*F->N.Y + AP.Z*F->N.Z<F->NormProd)))
      {
        *Ins++ = F;
        dz = F->A->TPos.Z;
//        if (*(long *)(&F->B->TPos.Z)>*pdz) dz=F->B->TPos.Z;
//        if (*(long *)(&F->C->TPos.Z)>*pdz) dz=F->C->TPos.Z;
        if (F->B->TPos.Z>dz) dz=F->B->TPos.Z;
				if (F->C->TPos.Z>dz) dz=F->C->TPos.Z;
#ifdef FDS_ZFLIP
				// same key scheme as Transform_Objects: this pass fed the ONE-WALK renderer
				// painter's keys, so its flare sorted mid-list and later opaques erased it.
				if (F->Txtr->Flags & (Mat_Transparent|Mat_Virtual))
				{ float fzp=Sc->FZP, cz=((float)dz>fzp)?fzp:(float)dz, ff=(fzp-cz)/fzp;
				  F->SortZ.F = (2.0f*fzp - cz) + ff; }
				else
					F->SortZ.F = ((float)dz>Sc->FZP) ? Sc->FZP : (float)dz;
#else
				F->SortZ.F = 100000.0-dz;
#endif
      }
	}
	CPolys = Ins-FList;
  for(O=Sc->OmniHead;O;O=O->Next)
  {
    Vtx=&O->V;
    Vector_Sub(&O->IPos,&View->ISource,&V);
    MatrixXVector(View->Mat,&V,&Vtx->TPos);
		if (Vtx->TPos.Z>1&&Vtx->TPos.Z<Sc->FZP)   // sprite: 1.0 gate
    {
      Vtx->RZ=1.0/Vtx->TPos.Z;
      Vtx->PX=CntrEX+Vtx->TPos.X*PX*Vtx->RZ;
			Vtx->PY=CntrEY-Vtx->TPos.Y*PY*Vtx->RZ;
			// Insert to List
			dz = Vtx->TPos.Z;
      //dz *=-16384;
			//dz +=0x7FFFFFFF;
			//RoundToInt(&O->Face.SortZ.DW,dz);
#ifdef FDS_ZFLIP
      { float fzp=Sc->FZP, cz=((float)dz>fzp)?fzp:(float)dz, ff=(fzp-cz)/fzp;
        O->F.SortZ.F = (2.0f*fzp - cz) + ff; }   /* xpar-style: after all opaques */
#else
      O->F.SortZ.F = 100000.0-dz;
#endif
			// reflection-pass copy of the flare loop: same OOB as the engine's —
			// cull flares whose center is off-screen (unclipped additive filler).
			if (Vtx->PX>=0.0 && Vtx->PX<CntrEX*2.0 && Vtx->PY>=0.0 && Vtx->PY<CntrEY*2.0)
				*Ins++ = &O->F;
    }
	}
	COmnies = (Ins-FList)-CPolys;
	for(I=0;I<Sc->NumOfParticles;I++)
		if (Sc->Pcl[I].Flags&Particle_Active)
			if ((dz=Sc->Pcl[I].V.TPos.Z)>=1.0) {   // sprite: 1.0 gate
				F = &Sc->Pcl[I].F;
			//dz *=-16384;
			//dz +=0x7FFFFFFF;
			//RoundToInt(&F->SortZ.DW,dz);
#ifdef FDS_ZFLIP
				{ float fzp=Sc->FZP, cz=((float)dz>fzp)?fzp:(float)dz, ff=(fzp-cz)/fzp;
				  F->SortZ.F = (2.0f*fzp - cz) + ff; }
#else
				F->SortZ.F = 100000.0-dz;
#endif
				*Ins++ = F;
			}
	CAll = Ins-FList;
	CPcls = CAll-COmnies-CPolys;
}

static float C_reFZP;

#if CITY_LBAKE
// Static-light cache: per-static-mesh baked per-vertex light (3 floats/vertex), kept in a
// side table keyed by TriMesh* so the (duplicated) TriMesh struct is left untouched. The
// vertex LR/LG/LB get clobbered by the render pipeline between frames, so each frame we COPY
// the cache back into LR/LG/LB for static meshes (cheap) instead of re-running the per-omni
// lighting loop. Scene runs once; the small leak at scene end is acceptable.
static TriMesh *LC_Ptr[400];
static float   *LC_SL[400];
static long     LC_N = 0;
static float *CityLightCache(TriMesh *T, long verts)
{
	long i; for(i=0;i<LC_N;i++) if(LC_Ptr[i]==T) return LC_SL[i];
	if(LC_N<400){ LC_Ptr[LC_N]=T; LC_SL[LC_N]=new float[3*verts]; return LC_SL[LC_N++]; }
	return 0;
}
#endif

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
#if CITY_LBAKE_VERIFY
	static float omni0[128][3]; static int omniPar[128]; static long nOmni0=0;
	static float omniAux[128][3];            // L.R, ISize, IRange at bake
	static TriMesh *bkPtr[300]; static float bkXf[300][4]; static long nBk=0;  // IPos.XYZ + RotMat-checksum at bake
#endif

	// --- PERF PROBE (one-time): size static-lighting bake for City. City lights with ALL
	// omnis (if(1)) and buildings are static; report how much vtx-light work is bakeable. ---
	{
		static int _cprobed=0;
		if(!_cprobed)
		{
			_cprobed=1;
			Omni *pO; TriMesh *pT; DWord k;
			long nLit=0,nLitS=0,nMesh=0,nMeshS=0,vAll=0,vStat=0;
			// DIAG: dump the first few omnis' actual runtime lighting values (is omni lighting even live?)
			{ FILE *of=fopen("PERF.TXT","at"); long oi=0;
			  for(pO=Sc->OmniHead;pO&&oi<6;pO=pO->Next,oi++){ if(of)
				fprintf(of,"  OMNI#%ld L=(%.0f,%.0f,%.0f) ISize=%.4f IRange=%.2f Range.NumKeys=%lu Range[0]=%.3f Size.NumKeys=%lu Size[0]=%.4f flags=%lx\n",
					oi,(double)pO->L.R,(double)pO->L.G,(double)pO->L.B,(double)pO->ISize,(double)pO->IRange,
					(unsigned long)pO->Range.NumKeys,(double)(pO->Range.NumKeys?pO->Range.Keys[0].Pos.X:-1.0f),
					(unsigned long)pO->Size.NumKeys,(double)(pO->Size.NumKeys?pO->Size.Keys[0].Pos.X:-1.0f),(unsigned long)pO->Flags); }
			  if(of)fclose(of); }
			for(pO=Sc->OmniHead;pO;pO=pO->Next){
				nLit++;
				int c=(pO->Pos.NumKeys<=1);
				if(!c){ c=1; for(k=1;k<pO->Pos.NumKeys;k++){ SplineKey *K=&pO->Pos.Keys[k],*K0=&pO->Pos.Keys[0];
					if(K->Pos.X!=K0->Pos.X||K->Pos.Y!=K0->Pos.Y||K->Pos.Z!=K0->Pos.Z){c=0;break;} } }
				if(c) nLitS++;
#if CITY_LBAKE_VERIFY
				if(nOmni0<128){ omni0[nOmni0][0]=pO->IPos.X; omni0[nOmni0][1]=pO->IPos.Y; omni0[nOmni0][2]=pO->IPos.Z;
					omniAux[nOmni0][0]=pO->L.R; omniAux[nOmni0][1]=pO->ISize; omniAux[nOmni0][2]=pO->IRange;
					omniPar[nOmni0]=((pO->Flags&Flare_Parented)!=0)|(((pO->Flags&Flare_LightSource)!=0)?0:2); nOmni0++; }
#endif
			}
			// Static-light bake is only valid if EVERY omni is stationary (a moving light would
			// change a static mesh's shading frame-to-frame). City: all 26 omnis are static.
			int allLightStatic = (nLit==nLitS);
			FILE *pf=fopen("PERF.TXT","at");
			for(pT=Sc->TriMeshHead;pT;pT=pT->Next){
				if(pT->Flags&(Tri_Invisible|Tri_Noshading)) continue;
				nMesh++; vAll+=pT->VIndex;
				int posV=0,rotV=0,sclV=0;
				Spline *SP[2]; SP[0]=&pT->Pos; SP[1]=&pT->Scale;
				for(int s=0;s<2;s++){ int *vf=s?&sclV:&posV; if(SP[s]->NumKeys>1){ for(k=1;k<SP[s]->NumKeys;k++){
					SplineKey *K=&SP[s]->Keys[k],*K0=&SP[s]->Keys[0];
					if(K->Pos.X!=K0->Pos.X||K->Pos.Y!=K0->Pos.Y||K->Pos.Z!=K0->Pos.Z){*vf=1;break;} } } }
				if(pT->Rotate.NumKeys>1){ for(k=1;k<pT->Rotate.NumKeys;k++){ SplineKey *K=&pT->Rotate.Keys[k],*K0=&pT->Rotate.Keys[0];
					if(K->Pos.W!=K0->Pos.W||K->Pos.X!=K0->Pos.X||K->Pos.Y!=K0->Pos.Y||K->Pos.Z!=K0->Pos.Z||
					   K->AA.W!=K0->AA.W||K->AA.X!=K0->AA.X||K->AA.Y!=K0->AA.Y||K->AA.Z!=K0->AA.Z){rotV=1;break;} } }
				int poss=(pT->Flags&Tri_Possessed)!=0, par=(pT->Flags&Tri_Parented)!=0;
				int stat=!poss&&!par&&!posV&&!rotV&&!sclV;
				if(stat){ nMeshS++; vStat+=pT->VIndex;
#if CITY_LBAKE
					if(allLightStatic) pT->Flags|=Tri_LStatic;   // bake-eligible; light baked on frame 1, skipped after
#endif
				}
			}
			if(pf){ fprintf(pf,"CPROBE: allOmni=%ld(stat %ld) shadedMesh=%ld(stat %ld) verts=%ld(stat %ld) | bakeable=%ld of %ld (%.0f%%) LBAKE=%d\n",
				nLit,nLitS,nMesh,nMeshS,vAll,vStat,(long)nLitS*vStat,(long)nLit*vAll,
				(nLit*vAll)?100.0*((double)nLitS*vStat)/((double)nLit*vAll):0.0, (int)CITY_LBAKE); fclose(pf); }
		}
	}

#if CITY_MASTER_LIGHT
	// One-time per-omni fixup: the FLD ships no Range/Size, so classify each omni by colour and
	// assign master's per-type Range + ISize (blue "bilding flare" 7000/0.5, red "big bilding
	// flare" 5000/0.5, orange "shp1 glow" 5000/0.08, white "Light" rangeless). Runs before the
	// frame-1 bake so the cached static light uses these values.
	{
		static int _mlfixed=0;
		if(!_mlfixed){ _mlfixed=1;
			for(O=Sc->OmniHead;O;O=O->Next){
				float rr=O->L.R*LSCALE_INV, rg=O->L.G*LSCALE_INV, rb=O->L.B*LSCALE_INV;
				if      (rb>200.0f && rr<64.0f)                    { O->IRange=7000.0f; O->ISize=0.5f;  } // blue
				else if (rr>200.0f && rg>200.0f && rb>200.0f)      { O->IRange=0.0f;    O->ISize=0.0f;  } // white (rangeless)
				else if (rr>200.0f && rg>=40.0f && rg<128.0f && rb<40.0f) { O->IRange=5000.0f; O->ISize=0.08f; } // orange glow
				else if (rr>200.0f && rb<40.0f && rg<40.0f)        { O->IRange=5000.0f; O->ISize=0.5f;  } // red
				else                                               { O->IRange=5000.0f; O->ISize=0.5f;  } // fallback
			}
		}
	}
#endif

#if CITY_LBAKE_VERIFY
	#define VSCR_MAX 8192
	static float vscr[VSCR_MAX*3];
	static long vfyMismatch=0, vfyChecked=0; static float vfyMax=0.0f;
	static long _vf=0; int verifyFrame=(++_vf==300);
#endif

	for(T=Sc->TriMeshHead;T;T=T->Next)
	{
		if (T->Flags&Tri_Invisible) continue;
		if (T->Flags&Tri_Noshading) continue;
		int _vfy=0;
#if CITY_LBAKE_VERIFY
		// on the verify frame, save the CACHED baked light then recompute, so we can compare
		// cache-vs-recompute (LR itself is clobbered between frames, so read the cache directly).
		if (verifyFrame && (T->Flags&Tri_LStatic) && (T->Flags&Tri_LBaked) && T->VIndex<=VSCR_MAX)
		{
			float *sl=CityLightCache(T,T->VIndex);
			if(sl){ _vfy=1; long vi; for(vi=0;vi<(long)T->VIndex;vi++){ vscr[vi*3]=sl[vi*3]; vscr[vi*3+1]=sl[vi*3+1]; vscr[vi*3+2]=sl[vi*3+2]; } }
		}
#endif
#if CITY_LBAKE
		// static mesh already baked: refill LR/LG/LB from the cache (render clobbers them each
		// frame) — a cheap copy instead of the full per-omni lighting loop.
		if ((T->Flags&Tri_LStatic)&&(T->Flags&Tri_LBaked)&&!_vfy)
		{
			float *sl=CityLightCache(T,T->VIndex);
			if(sl){ VEnd=T->Verts+T->VIndex; long i=0; for(Vtx=T->Verts;Vtx<VEnd;Vtx++){ Vtx->LR=sl[i*3]; Vtx->LG=sl[i*3+1]; Vtx->LB=sl[i*3+2]; i++; } continue; }
		}
#endif

		VEnd=T->Verts+T->VIndex;
		FEnd=T->Faces+T->FIndex;
		Vector_Sub(&T->IPos,&View->ISource,&U);
		MatrixTXVector(T->RotMat,&U,&W);
		// Camera Illumination model (Lambertian)
		for(Vtx=T->Verts;Vtx<VEnd;Vtx++)
			Vtx->LR=Vtx->LG=Vtx->LB = Ka;
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
#if CITY_MASTER_LIGHT
		// master linear range-window: k = (N·L)·(1 - d/Range), hard cutoff at d>=Range.
		for(O=Sc->OmniHead;O;O=O->Next)
		{
			if (O->IRange<=0.0f) continue;                 // rangeless omni (white) — no contribution
			Vector_Sub(&T->IPos,&O->IPos,&U);
			MatrixTXVector(T->RotMat,&U,&W);
			float Range2=O->IRange*O->IRange, rRange=1.0f/O->IRange;
			// per-mesh cull: skip if the whole bounding sphere is beyond Range (dist-radius > Range)
			// BSphereRad is radius²; Vector_SelfDot(&U) is centre distance². Cheap early-out.
			if (Vector_SelfDot(&U) > Range2 + T->BSphereRad + 2.0f*O->IRange*(float)sqrt((double)T->BSphereRad)) continue;
			float lr=O->L.R*LSCALE_INV*O->ISize*CITY_LIGHT_GAIN, lg=O->L.G*LSCALE_INV*O->ISize*CITY_LIGHT_GAIN, lb=O->L.B*LSCALE_INV*O->ISize*CITY_LIGHT_GAIN;
			for(Vtx=T->Verts;Vtx<VEnd;Vtx++)
			{
				Vector_Add(&Vtx->Pos,&W,&L);
				float len2=L.X*L.X+L.Y*L.Y+L.Z*L.Z;
				if (len2>Range2) continue;                 // beyond range
				rLen=RSQRT(len2);                          // 1/d
				Dot=-Dot_Product(&L,&Vtx->N)*rLen;         // N·Lunit
				if (Dot<=0.0f) continue;
				float k=Dot*(1.0f-rRange/rLen);            // (N·L)·(1 - d/Range)
				Vtx->LR+=lr*k; Vtx->LG+=lg*k; Vtx->LB+=lb*k;
			}
		}
#else
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
						rLen = RSQRT(L.X*L.X+L.Y*L.Y+L.Z*L.Z);
						Dot=-Dot_Product(&L,&Vtx->N)*rLen;

//					Vector_Add(&Vtx->Pos,&CV,&L);
//					rLen = RSQRT(L.X*L.X+L.Y*L.Y+L.Z*L.Z);
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
						rLen = RSQRT(L.X*L.X+L.Y*L.Y+L.Z*L.Z);
						Dot=-Dot_Product(&L,&Vtx->N)*rLen;

//					Vector_Add(&Vtx->Pos,&CV,&L);
//					rLen = RSQRT(L.X*L.X+L.Y*L.Y+L.Z*L.Z);
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

#endif
		// Saturation.
		for(Vtx=T->Verts;Vtx<VEnd;Vtx++)
		{
			if (Vtx->LR>253.0) Vtx->LR=253.0;
			if (Vtx->LG>253.0) Vtx->LG=253.0;
			if (Vtx->LB>253.0) Vtx->LB=253.0;
//      Vtx->LR = 0.25; Vtx->LG = 0.25; Vtx->LB = 0.25; //Or some other jack,like an Ambient constant
		}
#if CITY_LBAKE_VERIFY
		if (_vfy) {
			static int _dumped=0;
			if(!_dumped){ _dumped=1; FILE *df=fopen("PERF.TXT","at"); if(df){
				fprintf(df,"  LDUMP mesh V=%ld: v0 baked(%.2f,%.2f,%.2f) recompute(%.2f,%.2f,%.2f) Ka=%.1f\n",
					(long)T->VIndex,(double)vscr[0],(double)vscr[1],(double)vscr[2],
					(double)T->Verts[0].LR,(double)T->Verts[0].LG,(double)T->Verts[0].LB,(double)Ka); fclose(df); } }
			long vi=0;
			for(Vtx=T->Verts;Vtx<VEnd;Vtx++){
				float dr=Vtx->LR-vscr[vi*3]; if(dr<0)dr=-dr;
				float dg=Vtx->LG-vscr[vi*3+1]; if(dg<0)dg=-dg;
				float db=Vtx->LB-vscr[vi*3+2]; if(db<0)db=-db;
				if(dr>1e-3f||dg>1e-3f||db>1e-3f) vfyMismatch++;
				if(dr>vfyMax)vfyMax=dr; if(dg>vfyMax)vfyMax=dg; if(db>vfyMax)vfyMax=db;
				vfyChecked++; vi++;
			}
		}
#endif
		// first full pass for a static mesh — its light is now baked; skip it on later frames
#if CITY_LBAKE_VERIFY
		if ((T->Flags&Tri_LStatic) && !(T->Flags&Tri_LBaked) && nBk<300) {
			float rs=0.0f; float *rm=(float *)T->RotMat; for(int q=0;q<9;q++) rs+=rm[q];
			bkPtr[nBk]=T; bkXf[nBk][0]=T->IPos.X; bkXf[nBk][1]=T->IPos.Y; bkXf[nBk][2]=T->IPos.Z; bkXf[nBk][3]=rs; nBk++;
		}
#endif
#if CITY_LBAKE
		// first full pass for a static mesh: store its computed light in the cache, then mark
		// it baked so later frames just copy from the cache (render clobbers LR between frames).
		if ((T->Flags&Tri_LStatic) && !(T->Flags&Tri_LBaked))
		{
			float *sl=CityLightCache(T,T->VIndex);
			if(sl){ long i=0; for(Vtx=T->Verts;Vtx<VEnd;Vtx++){ sl[i*3]=Vtx->LR; sl[i*3+1]=Vtx->LG; sl[i*3+2]=Vtx->LB; i++; } }
			T->Flags |= Tri_LBaked;
		}
#else
		if (T->Flags&Tri_LStatic) T->Flags |= Tri_LBaked;
#endif
	}
#if CITY_LBAKE_VERIFY
	if (verifyFrame) { FILE *pf=fopen("PERF.TXT","at"); if(pf){
		long omniMoved=0,omniLit=0,parCnt=0,nonLit=0,j=0; float omniMaxDrift=0.0f,omniAuxDrift=0.0f;
		for(O=Sc->OmniHead;O&&j<nOmni0;O=O->Next,j++){
			float dx=O->IPos.X-omni0[j][0],dy=O->IPos.Y-omni0[j][1],dz=O->IPos.Z-omni0[j][2];
			float d=(float)sqrt(dx*dx+dy*dy+dz*dz);
			if(d>0.01f){ omniMoved++; if(d>omniMaxDrift)omniMaxDrift=d; }
			float al=(float)fabs(O->L.R-omniAux[j][0]),as=(float)fabs(O->ISize-omniAux[j][1]),ar=(float)fabs(O->IRange-omniAux[j][2]);
			if(al>omniAuxDrift)omniAuxDrift=al; if(as>omniAuxDrift)omniAuxDrift=as; if(ar>omniAuxDrift)omniAuxDrift=ar;
			if(omniPar[j]&1)parCnt++; if(omniPar[j]&2)nonLit++;
		}
		long meshMoved=0; float meshMaxDrift=0.0f;
		for(T=Sc->TriMeshHead;T;T=T->Next){ long m; for(m=0;m<nBk;m++) if(bkPtr[m]==T){
			float rs=0.0f; float *rm=(float *)T->RotMat; for(int q=0;q<9;q++) rs+=rm[q];
			float dp=(float)(fabs(T->IPos.X-bkXf[m][0])+fabs(T->IPos.Y-bkXf[m][1])+fabs(T->IPos.Z-bkXf[m][2])+fabs(rs-bkXf[m][3]));
			if(dp>0.01f){ meshMoved++; if(dp>meshMaxDrift)meshMaxDrift=dp; } break; } }
		fprintf(pf,"LVERIFY(frame %ld): mismatch=%ld/%ld maxDelta=%.5f | omniMoved=%ld/%ld drift=%.3f auxDrift=%.4f par=%ld nonLit=%ld | meshMoved=%ld/%ld meshDrift=%.4f\n",
			_vf,vfyMismatch,vfyChecked,(double)vfyMax,omniMoved,nOmni0,(double)omniMaxDrift,(double)omniAuxDrift,parCnt,nonLit,meshMoved,nBk,(double)meshMaxDrift); fclose(pf); } }
#endif
	if (Sc->Flags&Scn_Fogged)
	{
		for(T=Sc->TriMeshHead;T;T=T->Next)
		{
			if (T->Flags&Tri_Invisible) continue;
			if (T->Flags&Tri_Noshading) continue;
			// the water is invisible here (reflection pass) so its TPos.Z is the mirrored
			// Z -> the fog would zero it; the water is fogged in its own filler instead.
			if (RflObj && T==(TriMesh *)RflObj->Data) continue;

			Vtx=T->Verts;
			VEnd=Vtx+T->VIndex;
			for(;Vtx<VEnd;Vtx++)
			{
				//apply fog (clamp: past the far plane Color goes negative -> without this
				//the *Color makes lighting negative and the Gouraud filler wraps it bright,
				//which reads as "far geometry lit / z reversed")
				Color = 1.0f-Vtx->TPos.Z*C_reFZP;
				if (Color<0.0f) Color=0.0f;
				if (Color>1.0f) Color=1.0f;

				Vtx->LR *= Color;
				Vtx->LG *= Color;
				Vtx->LB *= Color;
			}
		}
	}
}



#ifdef FDS_XPARVIZ
// Isolation probe for "the transparent mapper doesn't render on the left of the screen":
// dump VPage immediately BEFORE and AFTER the water-only pass, so the transparent mapper's
// own output is visible with nothing else painted over it (the opaque pass runs afterwards).
static void CityVizDump(const char *fn)
{
	FILE *pf=fopen(fn,"wb");
	if (!pf) return;
	unsigned char *px=(unsigned char *)VPage;
	long i,n=(long)XRes*(long)YRes;
	fprintf(pf,"P6\n%d %d\n255\n",(int)XRes,(int)YRes);
	for(i=0;i<n;i++){ fputc(px[i*4+2],pf); fputc(px[i*4+1],pf); fputc(px[i*4+0],pf); }
	fclose(pf);
}
#define XPVFRAME ((_rcf>=1&&_rcf<=3)||_rcf==50||_rcf==100||_rcf==200||_rcf==350)
#endif

void Run_City()
{
	long Polys = 0,TTrd;
	TriMesh *T;
	Omni *O;
	long _rcf = 0;
	fprintf(LogFile,"RC0:Run_City entered\n");fflush(LogFile);
	// MODE PROBE: confirm the fillers' hard-coded 1280-byte (320*4) stride matches the real
	// VESA mode. If VESA_BPSL != 1280 or XRes != 320, the fillers write OOB every scanline.
	fprintf(LogFile,"MODEPROBE: XRes=%ld YRes=%ld BPP=%ld VESA_BPSL=%ld PageSize=%ld (fillers assume stride=1280=320*4)\n",
		(long)XRes,(long)YRes,(long)BPP,(long)VESA_BPSL,(long)PageSize);fflush(LogFile);

	CurScene = CitySc;

	// get the reflective water surface object
	Reflective_Surface();
	fprintf(LogFile,"RCrs:RflObj=%p\n",(void*)RflObj);fflush(LogFile);
	// Frame-0 "weird angle" diagnosis: does the camera spline start LATER than StartFrame?
	// (CurFrame at Timer=1 ~= StartFrame; evaluating before the first key = extrapolation.)
	{ Camera *_cam = CitySc->CameraHead;
	  fprintf(LogFile,"RCcam: Start=%.1f End=%.1f | Src keys=%lu f0=%.1f fN=%.1f | Tgt keys=%lu f0=%.1f\n",
	    CitySc->StartFrame,CitySc->EndFrame,
	    (unsigned long)_cam->Source.NumKeys,
	    _cam->Source.NumKeys?_cam->Source.Keys[0].Frame:-1.0f,
	    _cam->Source.NumKeys?_cam->Source.Keys[_cam->Source.NumKeys-1].Frame:-1.0f,
	    (unsigned long)_cam->Target.NumKeys,
	    _cam->Target.NumKeys?_cam->Target.Keys[0].Frame:-1.0f);
	  fflush(LogFile); }

	{ long _nm=0;
	for(T = CitySc->TriMeshHead;T;T=T->Next) {
		Polys+=T->FIndex;
		_nm++;
		if (T->FIndex>50000 || T->VIndex>50000) { fprintf(LogFile,"BADMESH:#%ld FIndex=%ld VIndex=%ld\n",_nm,(long)T->FIndex,(long)T->VIndex);fflush(LogFile); }
	}
	for(O = CitySc->OmniHead;O;O=O->Next)
		Polys++;
	fprintf(LogFile,"RCal:Polys=%ld nmesh=%ld\n",Polys,_nm);fflush(LogFile); }
	FList = new Face * [Polys];
	SList = new Face * [Polys];

	View = CitySc->CameraHead;
//	View = &FC;

	C_FZP = CitySc->FZP;
	C_rFZP = 1.0f/C_FZP;
	C_NZP = CitySc->NZP;
	C_rNZP = 1.0f/C_NZP;
	// Fog must reach black BEFORE the far-clip plane, or geometry gets cut while still
	// visibly lit - that is the "buildings clipped before they turn black" artifact. The old
	// 1.15 put full black at 1.15*FZP, i.e. 15% BEYOND where UFE cuts. 0.95 puts it just
	// inside, so the cut edge is already black. (With FZP 2500->3000 this also keeps the fog
	// curve where it was: black at 2850 now vs 2875 before.)
	C_reFZP = 1.0f/(C_FZP*0.95f);

	fprintf(LogFile,"RC-TIME preloop(bake etc): %ld.%02lds\n",(long)Timer/100,(long)Timer%100);fflush(LogFile);
	TTrd = Timer;

	Ambient_Factor = 96;
	Diffusive_Factor = 80000000;
	ImageSize = 40000;
	// PERF: RDTSC per-pass cycle accumulators (skip early debug-heavy + SNAP frames).
	double cSetup=0,cRefl=0,cMain=0,cFlip=0; long pfC=0; unsigned long tc0,tc1; long g_FlipBack=0;
	// Every bucket used to convert inline as (double)(unsigned long)(tc1-tc0). That form
	// intermittently produced ~2^31 per frame (city read flip=99.8%, 2159 Mcy/frame on BOTH
	// 86Box and DOSBox, then vanished on a rebuild) -- a stale high dword in the temp Watcom
	// FILDs from. Materialising the delta in a named unsigned long first fixes the codegen;
	// route ALL of them through one function so there is a single conversion to verify.
	double cMainX=0,cMainS=0,cMainR=0;   // main-pass split: transform / radix-sort / render(fill)
	while ((!Keyboard[ScESC])&&(!Keyboard[ScBackSpace])&&Timer<CTPartTime)   // BACKSPACE = skip scene, ESC = quit
	{
#if CITY_DETERMINISTIC
		Timer = 1 + (long)((double)_rcf * (double)CTPartTime / 500.0);   // fixed step/frame
#endif
		dTime = Timer-TTrd;
		TTrd = Timer;
		int _prof = (_rcf>=10 && _rcf!=20 && _rcf!=100 && _rcf!=200 && _rcf!=350);
		tc0=RDTSC();


		memset(VPage,0,PageSize);

		if ((Keyboard[ScRight]||Keyboard[ScCloseSq])) Timer += 30;                              // scrub forward
		if ((Keyboard[ScLeft]||Keyboard[ScOpenSq]))  { if (Timer>31) Timer -= 30; else Timer = 1; }   // scrub back (needs the spline rewind guard)

		CurFrame = CitySc->StartFrame + (CitySc->EndFrame-CitySc->StartFrame) * (float)Timer / (float)CTPartTime;
 //							 CitySc->StartFrame + 500 * (float)Timer / (float)CTPartTime;


//		Dynamic_Camera();

#define RCMARK(tag) if(_rcf<4||Timer>=CTPartTime-15){fprintf(LogFile,"f%ld(T%ld):%s\n",_rcf,(long)Timer,tag);fflush(LogFile);}
		RCMARK("RCa:preAnimate");
#ifdef FDS_CLIPCHK
		{ extern void ClipChk_Reset(void); ClipChk_Reset(); }
#endif
		Animate_Objects(CitySc);
		RCMARK("RCb:postAnimate");
		((TriMesh *)(RflObj->Data))->Flags &= 0xFFFFFFFF-HTrack_Visible;

		City_Lighting();
		RCMARK("RCc:postLighting");
		tc1=RDTSC(); if(_prof) cSetup+=TscDelta(tc0,tc1); tc0=tc1;

		// TEMP UNBLOCK: the reflection Render() faults on real hardware (86Box) — mirrored
		// geometry projects to extreme off-screen faces (maxX~1155) that overrun the clipper/
		// filler at the framebuffer edge; DOSBox's looser memory tolerates it, 86Box page-faults.
		// Skip the whole reflection pass until the clipper/filler is made edge-safe. Water then
		// renders without its mirror (base texture only). Set to 0 once the OOB is fixed.
#define CITY_SKIP_REFLECTION 0
#if CITY_SKIP_REFLECTION
		goto norefl;
#endif
		Reflected_Transform(CitySc);
		RCMARK("RCd:postReflXform");
		if(_rcf<4){fprintf(LogFile,"f%ld:CAll_afterRefl=%ld\n",_rcf,(long)CAll);fflush(LogFile);}
		if (!CAll) goto norefl;
		if(_rcf<4){fprintf(LogFile,"RCd2:postRadix1_ok CAll=%ld\n",(long)CAll);fflush(LogFile);}
		Radix_SortingASM(FList,SList,CAll);
		if(_rcf<4){fprintf(LogFile,"RCd3:postRadix1done\n");fflush(LogFile);}
		if(_rcf<1){ long _i; float _mx=0,_my=0; long _nbad=0,_nclip=0,_nunclip=0;
			for(_i=0;_i<CAll;_i++){ Face*_ff=FList[_i];
				if(!_ff){fprintf(LogFile,"DUMP i=%ld ff=NULL\n",_i);fflush(LogFile);continue;}
				Vertex*_a=_ff->A,*_b=_ff->B,*_c=_ff->C;
				if(!_a||!_b||!_c){fprintf(LogFile,"DUMP i=%ld nullvert A=%p B=%p C=%p\n",_i,(void*)_a,(void*)_b,(void*)_c);fflush(LogFile);continue;}
				float ax=_a->PX,ay=_a->PY,bx=_b->PX,by=_b->PY,cx=_c->PX,cy=_c->PY;
				float m=fabs(ax); if(fabs(bx)>m)m=fabs(bx); if(fabs(cx)>m)m=fabs(cx); if(m>_mx)_mx=m;
				float n=fabs(ay); if(fabs(by)>n)n=fabs(by); if(fabs(cy)>n)n=fabs(cy); if(n>_my)_my=n;
				long _vf=(_a->Flags|_b->Flags|_c->Flags)&0x3F;
				int _off=(ax<-8||ax>328||bx<-8||bx>328||cx<-8||cx>328||ay<-8||ay>248||by<-8||by>248||cy<-8||cy>248);
				if(_off){ if(_vf) _nclip++; else { _nunclip++;
					fprintf(LogFile,"UNCLIP i=%ld A(%.0f,%.0f)B(%.0f,%.0f)C(%.0f,%.0f) Fill=%p\n",_i,ax,ay,bx,by,cx,cy,(void*)_ff->Filler);fflush(LogFile); } }
				if(m>2000||n>2000||ax!=ax||ay!=ay){ _nbad++;
					fprintf(LogFile,"DUMP-BAD i=%ld A(%.0f,%.0f) B(%.0f,%.0f) C(%.0f,%.0f)\n",_i,ax,ay,bx,by,cx,cy);fflush(LogFile); } }
			fprintf(LogFile,"DUMP-DONE maxX=%.0f maxY=%.0f nbad=%ld nclip=%ld nunclip=%ld CAll=%ld\n",_mx,_my,_nbad,_nclip,_nunclip,(long)CAll);fflush(LogFile); }
		Render();
		RCMARK("RCe:postRender1refl");

		//Reflective_Distortion();
		Run_Distort();
		RCMARK("RCf1:postDistort");

		norefl:
		// animate the water's UV ripple over the stored base (per-vertex phase = wave),
		// then the standard transform picks up the rippled UVs. Tunable amp/speed.
		if (WaterFaceN)
		{
			Face *_f = ((TriMesh*)RflObj->Data)->Faces;
			float _t = (float)Timer*0.0018f, _amp = 350.0f;   // was 0.004/1400 — still swam too fast; slower phase + far smaller UV swing
			long _k;
			for(_k=0;_k<WaterFaceN;_k++,_f++)
			{
				_f->U1 = WaterBaseUV[_k][0] + (float)cos(_t        )*_amp;
				_f->V1 = WaterBaseUV[_k][1] + (float)sin(_t        )*_amp;
				_f->U2 = WaterBaseUV[_k][2] + (float)cos(_t+2.094f )*_amp;
				_f->V2 = WaterBaseUV[_k][3] + (float)sin(_t+2.094f )*_amp;
				_f->U3 = WaterBaseUV[_k][4] + (float)cos(_t+4.189f )*_amp;
				_f->V3 = WaterBaseUV[_k][5] + (float)sin(_t+4.189f )*_amp;
			}
		}

		tc1=RDTSC(); if(_prof) cRefl+=TscDelta(tc0,tc1); tc0=tc1;

#ifdef FDS_ZFLIP
		// Correct layering: below-water (the reflection pass above) -> transparent water
		// -> above-water opaque geometry. The water used to be a transparent face in the
		// MAIN pass, drawn AFTER the opaque buildings, so its 50/50 blend painted over
		// them. Draw it on its OWN between the two passes instead (g_RenderOnlyMesh limits
		// Transform_Objects to the water mesh). Render() clears Z, so the water (transparent,
		// no z-write) just composites over the reflection already in VPage; the opaque pass
		// then clears Z again and draws the buildings on top.
#ifdef FDS_XPARVIZ
		if (XPVFRAME) { char fn[32]; sprintf(fn,"CTR%ld.PPM",_rcf); CityVizDump(fn); }
#endif
		((TriMesh *)(RflObj->Data))->Flags |= HTrack_Visible;
		g_RenderOnlyMesh = (TriMesh *)(RflObj->Data);
		Transform_Objects(CitySc);
		g_RenderOnlyMesh = 0;
#ifdef FDS_XPARVIZ
		if (XPVFRAME)
		{
			fprintf(LogFile,"XPV:f%ld waterfaces_submitted=%ld (mesh has %ld)\n",
			        _rcf,(long)CAll,(long)WaterFaceN);
			{
				long _i;
				for(_i=0;_i<CAll;_i++)
				{
					Face *_f=FList[_i];
					Vertex *_v[3]; _v[0]=_f->A; _v[1]=_f->B; _v[2]=_f->C;
					int _k;
					fprintf(LogFile,"XPV:f%ld i%ld fill=%p xpar=%d\n",_rcf,_i,(void*)_f->Filler,
					        (int)(_f->Txtr && (_f->Txtr->Flags&(Mat_Transparent|Mat_Virtual))));
					for(_k=0;_k<3;_k++)
						fprintf(LogFile,"XPV:f%ld i%ld  v%d T(%.2f,%.2f,%.2f) P(%.2f,%.2f) RZ=%g fl=%lx\n",
						  _rcf,_i,_k,_v[_k]->TPos.X,_v[_k]->TPos.Y,_v[_k]->TPos.Z,
						  _v[_k]->PX,_v[_k]->PY,(double)_v[_k]->RZ,(long)(_v[_k]->Flags&0x3F));
				}
				fprintf(LogFile,"XPV:f%ld FZP=%.1f rFZP=%g NZP=%g rNZP=%g ZLinScale=%g\n",
				        _rcf,(double)C_FZP,(double)C_rFZP,(double)C_NZP,(double)C_rNZP,(double)ZLinScale);
			}
			fflush(LogFile);
		}
#endif
		if (CAll) { Radix_SortingASM(FList,SList,CAll); Render(); }
#ifdef FDS_XPARVIZ
		if (XPVFRAME) { char fn[32]; sprintf(fn,"CTW%ld.PPM",_rcf); CityVizDump(fn); }
#endif
		RCMARK("RCg0:postWater");
		((TriMesh *)(RflObj->Data))->Flags &= 0xFFFFFFFF-HTrack_Visible;   // exclude from opaque pass
#else
		((TriMesh *)(RflObj->Data))->Flags |= HTrack_Visible;
#endif

		Transform_Objects(CitySc);
		tc1=RDTSC(); if(_prof){ double d=TscDelta(tc0,tc1); cMainX+=d; cMain+=d; } tc0=tc1;
		RCMARK("RCg:postXform");
		if(_rcf<4){fprintf(LogFile,"f%ld:CAll_afterXform=%ld\n",_rcf,(long)CAll);fflush(LogFile);}
		if (!CAll) continue;
		Radix_SortingASM(FList,SList,CAll);
		tc1=RDTSC(); if(_prof){ double d=TscDelta(tc0,tc1); cMainS+=d; cMain+=d; } tc0=tc1;
		Render();
		tc1=RDTSC(); if(_prof){ double d=TscDelta(tc0,tc1); cMainR+=d; cMain+=d; } tc0=tc1;
		RCMARK("RCh:postRender2main");
#ifdef FDS_CLIPCHK
		if ((_rcf>=1&&_rcf<=3)||_rcf==50||_rcf==100||_rcf==200||_rcf==350)
		{ extern void ClipChk_Dump(const char *); char _t[24]; sprintf(_t,"city-f%ld",_rcf); ClipChk_Dump(_t); }
#endif

		// Frame dumps: ~15 colour frames at 230KB plus their z twins = ~4.6MB written to the
		// DOS filesystem mid-demo. Diagnostic only -- gate it, or every release build pays it.
#ifdef FDS_SNAP
		if ((_rcf>=1&&_rcf<=12)||_rcf==100||_rcf==200||_rcf==350)
		{
			char fn[32]; sprintf(fn,"CITY%ld.PPM",_rcf);
			FILE *pf=fopen(fn,"wb");
			if (pf)
			{
				unsigned char *px=(unsigned char *)VPage;
				long i,n=(long)XRes*(long)YRes;
				fprintf(pf,"P6\n%d %d\n255\n",(int)XRes,(int)YRes);
				for(i=0;i<n;i++){ fputc(px[i*4+2],pf); fputc(px[i*4+1],pf); fputc(px[i*4+0],pf); }
				fclose(pf);
			}
			fprintf(LogFile,"SNAP:wrote %s\n",fn);fflush(LogFile);
#ifdef FDS_ZBUF
			// z-buffer viz twin: 16-bit codes -> 8-bit gray, sqrt-ish spread via >>8 plus
			// low-byte fold so far-field (small codes) stays visible. Bright = near.
			{ unsigned short *zb = (unsigned short *)ZBufPtr;
			  sprintf(fn,"CITYZ%ld.PPM",_rcf);
			  FILE *zf=fopen(fn,"wb");
			  if (zf && zb)
			  {
				long i,n=(long)XRes*(long)YRes;
				unsigned long _mx=0,_nz=0;
				fprintf(zf,"P5\n%d %d\n255\n",(int)XRes,(int)YRes);
				for(i=0;i<n;i++){ unsigned v=zb[i]; unsigned g=(v>=4096)?(64+(v>>9)):(v>>6);
					if(g>255)g=255; fputc((int)g,zf);
					if(v){_nz++; if(v>_mx)_mx=v;} }
				fclose(zf);
				fprintf(LogFile,"ZDUMP:f%ld scale=%f nz=%lu max=%lu zb=%p\n",
				        _rcf,(double)ZScaleF,_nz,_mx,(void*)zb); fflush(LogFile);
			  }
			}
#endif
		}
#endif


		Show_FPS();
		Demo_Controls("city",(float)CurFrame,CitySc);
		Flip(Screen);
		tc1=RDTSC(); if(_prof){ unsigned long _fd=(unsigned long)(tc1-tc0);
		  // FLIPDBG: cFlip has been reading ~2100 Mcy/frame on BOTH emulators while every
		  // other bucket stays sane. Log the raw endpoints and count apparent-backwards
		  // deltas (>2^31 = tc1 < tc0) before trusting any number from this bucket.
		  { static long _fn=0, _fback=0; if(_fd>0x80000000UL) _fback++;
		    if(_fn<8||(_fn%200)==0) { fprintf(LogFile,"FLIPDBG f%ld tc0=%lu tc1=%lu d=%lu back=%ld\n",
		      _fn,tc0,tc1,_fd,_fback); fflush(LogFile); }
		    _fn++; g_FlipBack=_fback; }
		  cFlip+=TscDelta(tc0,tc1); pfC++; }

		{ long _step = (_rcf<12)?1:30; if ((_rcf++ % _step)==0) { { fprintf(LogFile,"RCf:%ld Timer=%ld/%ld triIn=%ld triDraw=%ld clipFace=%ld clipTri=%ld direct=%ld scr=%ld\n",_rcf,(long)Timer,(long)CTPartTime,g_TriIn,g_TriDraw,g_ClipFaces,g_ClipTris,g_DirectTris,g_ScreenPolys);fflush(LogFile); g_TriIn=0; g_TriDraw=0; g_ClipFaces=0; g_ClipTris=0; g_DirectTris=0; g_ScreenPolys=0; } } }
	} Timer-=CTPartTime;
	{ double tot=cSetup+cRefl+cMain+cFlip; if(tot<1)tot=1;
	  FILE *pf=fopen("PERF.TXT","at"); if(pf){
	  fprintf(pf,"PERF city: frames=%ld | setup(anim+light)=%.1f%% reflection=%.1f%% main(xform+sort+render)=%.1f%% flip=%.1f%% | Mcy/frame=%.2f | flipback=%ld\n",
	    pfC,cSetup*100/tot,cRefl*100/tot,cMain*100/tot,cFlip*100/tot,(pfC>0?tot/pfC/1e6:0),g_FlipBack);
	  fprintf(pf,"  LOAD: textures=%lu load=%.1f Mcy swizzle=%.1f Mcy (per-texture work, summed)\n",
	    g_LdTexN,g_LdTexCy/1e6,g_LdSwzCy/1e6);
#ifdef FDS_XPARPROF
	  { fprintf(pf,"  XPAR2PASS: calls=%lu texpass=%.1f Mcy lightpass=%.1f Mcy (city water)\n",
	      g_XparN,g_XparTexCy/1e6,g_XparLitCy/1e6); }
#endif
	  fprintf(pf,"  LOAD2: cold=%.1f warm=%.1f det=%.1f jpg=%.1f swap=%.1f scale=%.1f Mcy bytes=%lu\n",
	    g_LdColdCy/1e6,g_LdWarmCy/1e6,g_LdDetCy/1e6,g_LdJpgCy/1e6,g_LdSwapCy/1e6,
	    g_LdScaleCy/1e6,g_LdBytes);
	  fprintf(pf,"  MAIN split: xform=%.1f%% sort=%.1f%% render/fill=%.1f%% (of frame) | of-main: xform=%.0f%% sort=%.0f%% fill=%.0f%%\n",
	    cMainX*100/tot,cMainS*100/tot,cMainR*100/tot,
	    cMain>0?cMainX*100/cMain:0,cMain>0?cMainS*100/cMain:0,cMain>0?cMainR*100/cMain:0);
	  fclose(pf); } }
	Keyboard[ScBackSpace]=0;   // consume the skip key so it doesn't cascade into the next scene
	fprintf(LogFile,"RC_EXITLOOP Timer=%ld\n",(long)Timer);fflush(LogFile);
	if (Keyboard[ScESC])
	{
		#ifdef Play_Music
		ShutDown();
		#endif
		FDS_End();
		exit(-1);
	}
	delete FList;
	delete SList;
	fprintf(LogFile,"RC_preDestroy\n");fflush(LogFile);
	Destroy_Scene(CitySc);
	fprintf(LogFile,"RC_postDestroy\n");fflush(LogFile);
}
