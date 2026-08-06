/*
		 Flood Demo System - Flood scene file converter
		 -----------------------------------------------
*/

#include <stdio.h>
//#include <dos.h>
#include <stdlib.h>
//#include <conio.h>
#include <string.h>
#include <math.h>

#include <cstdint>
#include <vector>

#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"

#include "LWREAD.H"
#include "FLD_READ.H"
#include "Base/Scene.h"

//#define DebugMode
#define Scene_Scale 1.0

const float Rad2Deg = PI/180.0;

// CurMat/FirstMat globals were removed when AddMaterial gained dedup — the
// canonical Material* of each FldMat is now resolved per-Object into a
// std::vector and passed explicitly to ConvertFace, so the linked-list
// walk that depended on per-Object contiguity is gone.
signed short CurParent;
Scene *SceneGroup;

static float ZoomFactor2FOV(float zf)
{
	return 360.0f/PI * atanf( (float(XRes)/float(YRes))/zf );
}

void AddTriMesh(Scene *Sc,TriMesh *T,char *ObjName,Vector Pivot)
{
	int32_t I;
	Face *F;
	Object *Obj = new Object,*O;
	TriMesh *Tri;

	if (Sc->ObjectHead)
	{
		for(O=Sc->ObjectHead;O->Next;O=O->Next);
		O->Next=Obj;
		Obj->Prev=O;
		Obj->Next=NULL;
	} else {
		Sc->ObjectHead = Obj;
		Obj->Next = Obj->Prev = NULL;
		Obj->Number=0;
	}
	Obj->Data=(void *)T;
	Obj->Type=Obj_TriMesh;
	if (Obj->Prev) Obj->Number=Obj->Prev->Number+1;
	Obj->Pos = &T->IPos;
	Obj->Rot = &T->RotMat;
	Obj->Name=strdup(ObjName);
	Obj->ParentID = CurParent;
	Vector_Copy(&Obj->Pivot,&Pivot);
	Vector_SelfScale(&Obj->Pivot,Scene_Scale);

//	for(I=0;I<T->FIndex;I++)
//		Material2Face(&Default_Mat,T->Faces+I); // For now

	T->Status = new ObjectStatus;
	T->Status->Frame = 0.0;
	T->Status->Stat = HTrack_Visible;
	T->Status->Next = NULL;
	T->Status->Prev = NULL;
	T->CurStat = T->Status;
	T->Flags |= HTrack_Visible;
	Matrix_Form(T->RotMat,1,0,0,0,1,0,0,0,1);
	if (Sc->TriMeshHead)
	{
		for(Tri=Sc->TriMeshHead;Tri->Next;Tri=Tri->Next);
		T->Prev = Tri;
		Tri->Next = T;
	} else {Sc->TriMeshHead = T; T->Prev = NULL;}
	T->Next = NULL;
}

void AddOmni(Scene *Sc,Omni *Om,char *ObjName)
{
	Object *Obj = new Object,*O;
	Omni *Omni;

	memset(Obj,0,sizeof(Object));
	// Attach new Object to list.
	if (Sc->ObjectHead)
	{
		for(O=Sc->ObjectHead;O->Next;O=O->Next);
		O->Next = Obj;
		Obj->Prev = O;
	} else {
		Sc->ObjectHead = Obj;
		Obj->Prev = NULL;
		Obj->Number = 0;
	}
	Obj->Next=NULL;

	// Fill in Object general Data
	Obj->Data = (void *)Om;
	Obj->Type = Obj_Omni;
	if (Obj->Prev) Obj->Number=Obj->Prev->Number+1;
	Obj->Name = ObjName;
	Obj->Pos = &Om->IPos;
	Obj->Rot = &Mat_ID;
	Obj->ParentID=CurParent;
	Vector_Form(&Om->IPos,0,0,0);
	// Lights are active + flare-rendered by default; SceneAuthored marks this
	// as the i-th FLD/LWS light for the editor's enumeration + LWS write-back.
	// Spots get NO flare sprite: a headlight is a beam, not a glow quad (and
	// the code-created city headlights they replace never flared either).
	// The no-op Filler matters: Transform.cpp's flare pass inserts every
	// omni's F face regardless of flags, and PREPROC.CPP skips spots when
	// stamping the sprite blitter — without this lambda the renderer would
	// call a null Filler. Same pattern as fds::MakeSpotLight.
	// Preserve mask: ConvertOmni runs BEFORE AddOmni and may have set the
	// authored volumetric-beam opt-in (FLD bit 512 → Omni_ForceVolCone) —
	// the policy assignment below must not clobber it. (Legacy FLDs never
	// set the bit, so the preserved term is 0 and the assignment is
	// unchanged for them.)
	Om->Flags = Omni_Active | Omni_SceneAuthored
	          | (Om->Type == Light_SpotLight ? 0u : (DWord)Omni_Flare)
	          | (Om->Flags & (DWord)Omni_ForceVolCone);
	if (Om->Type == Light_SpotLight)
		Om->F.Filler = [](Face*, Vertex**, DWord, DWord,
		                  const fds::RenderTarget&,
		                  const fds::CameraContext&) {};
	Om->F.A = Om->F.B = Om->F.C = &Om->V;

	if (Sc->OmniHead)
	{
		for(Omni=Sc->OmniHead;Omni->Next;Omni=Omni->Next);
		Om->Prev = Omni;
		Omni->Next = Om;
	} else {Sc->OmniHead = Om; Om->Prev = NULL;}
	Om->Next = NULL;
}

void ConvertKey(FldKeyFrame *KF,SplineKey *Pos,SplineKey *Rot,SplineKey *Scl)
{
	Pos->Frame=Rot->Frame=Scl->Frame=KF->FrameNumber;

// Convert position
	Pos->Pos.x=KF->Position.x;
	Pos->Pos.y=KF->Position.y;
	Pos->Pos.z=KF->Position.z;
	if (KF->LinearValue==1.0f)
		Pos->Tens=1.0f;
	else
		Pos->Tens=KF->Tension;
	Pos->Cont=KF->Continuity;
	Pos->Bias=KF->Bias;
// Convert scale
	Scl->Pos.x=KF->Scale.x;
	Scl->Pos.y=KF->Scale.y;
	Scl->Pos.z=KF->Scale.z;
	if (KF->LinearValue==1.0f)
		Scl->Tens=1.0f;
	else
		Scl->Tens=KF->Tension;
	Scl->Cont=KF->Continuity;
	Scl->Bias=KF->Bias;
// Convert rotation
	Rot->Pos.x=-KF->Rotation.y*Rad2Deg; // X and Y are reversed!
	Rot->Pos.y=-KF->Rotation.x*Rad2Deg;
	Rot->Pos.z=-KF->Rotation.z*Rad2Deg;
	if (KF->LinearValue==1.0f)
		Rot->Tens=1.0f;
	else
		Rot->Tens=KF->Tension;
	Rot->Cont=KF->Continuity;
	Rot->Bias=KF->Bias;
}

void ConvertKey(FldKeyFrame *KF,SplineKey *Pos,float Scl) //OverLoading rules!
{
	Pos->Frame=KF->FrameNumber;
	Pos->Pos.x=KF->Position.x*Scl;
	Pos->Pos.y=KF->Position.y*Scl;
	Pos->Pos.z=KF->Position.z*Scl;
	if (KF->LinearValue==1.0f)
		Pos->Tens=1.0f;
	else
		Pos->Tens=KF->Tension;
	Pos->Cont=KF->Continuity;
	Pos->Bias=KF->Bias;
}

void ConvertKeyRot(FldKeyFrame *KF,SplineKey *Rot,float Scl)
{
	Rot->Frame=KF->FrameNumber;
	Rot->Pos.x=KF->Rotation.x*Scl;
	Rot->Pos.y=KF->Rotation.y*Scl;
	Rot->Pos.z=KF->Rotation.z*Scl;
	if (KF->LinearValue==1.0f)
		Rot->Tens=1.0f;
	else
		Rot->Tens=KF->Tension;
	Rot->Cont=KF->Continuity;
	Rot->Bias=KF->Bias;
}

void ConvertFace(FldFace *Org, Face *Dest, Vertex *V, int *Cur, FldMat *FM,
                 Material* const* matsByIndex, uint32_t numMats)
{
	if ((uint32_t)Org->Surface >= numMats || !matsByIndex[Org->Surface]) {
		printf("Error! No material for current face! (surface=%u, numMats=%u)\n",
		       (unsigned)Org->Surface, numMats);
		exit(200);
	}
	Material* FaceMat = matsByIndex[Org->Surface];
	// Fan triangulator
	for (int i = Org->FaceVerts - 2; i > 0; i--) {
		Dest[*Cur].A = V + Org->Verts[Org->FaceVerts - 1];
		Dest[*Cur].B = V + Org->Verts[i];
		Dest[*Cur].C = V + Org->Verts[i - 1];
		Dest[*Cur].Txtr = FaceMat;
		Get_Mapping(&Dest[*Cur], FM + Org->Surface); // Get mapping for current face using FM
		(*Cur)++;
	}
}

TriMesh *ConvertTriMesh(FldObject *Src)
{
	uint32_t i,TotalFaces;
	int j;
	TriMesh *T;
	bool stationary = true;

	T=(TriMesh *)getAlignedBlock(sizeof(TriMesh), 16);
	memset(T,0,sizeof(TriMesh));
	T->Flags = Tri_Euler;
	if (Src->Flags&Object_AlignToPath) T->Flags|=Tri_AlignToPath;
#ifdef DebugMode
	printf("Loading materials...\n");
#endif
	// Resolve each FldMat in this Object to its canonical (deduplicated)
	// Material*. The vector is indexed by Org->Surface in ConvertFace —
	// no more linked-list walks now that AddMaterial may share entries
	// across Objects.
	std::vector<Material*> objMats(Src->NumOfMat, nullptr);
	for (uint32_t k = 0; k < Src->NumOfMat; ++k)
		objMats[k] = AddMaterial(&Src->Material[k], SceneGroup);

	T->VIndex=Src->NumOfVerts;
	T->Verts=new Vertex[T->VIndex];
	memset(T->Verts,0,sizeof(Vertex)*T->VIndex);
	for (i=0;i<T->VIndex;i++)
	{
		T->Verts[i].Pos=Src->Verts[i];
		Vector_SelfScale(&T->Verts[i].Pos,Scene_Scale);
	}
	TotalFaces=0;
	for (i=0;i<Src->NumOfFaces;i++)
		TotalFaces+=Src->Faces[i].FaceVerts-2;

// WTF!!!??
//  TotalFaces+=2;

	T->FIndex=TotalFaces;
	T->Faces=new Face[TotalFaces];
	memset(T->Faces,0,sizeof(Face)*TotalFaces);
	j=0;

#ifdef DebugMode
	printf("Trimesh name: %s\n",Src->Name);
#endif
	for (i=0;i<Src->NumOfFaces;i++)
		ConvertFace(&Src->Faces[i], T->Faces, T->Verts, &j, Src->Material,
		            objMats.data(), (uint32_t)objMats.size());

	T->Pos.NumKeys=T->Rotate.NumKeys=T->Scale.NumKeys=Src->Keys;
	T->Pos.Keys=new SplineKey[Src->Keys];
	T->Rotate.Keys=new SplineKey[Src->Keys];
	T->Scale.Keys=new SplineKey[Src->Keys];
	memset(T->Pos.Keys,0,sizeof(SplineKey)*Src->Keys);
	memset(T->Rotate.Keys,0,sizeof(SplineKey)*Src->Keys);
	memset(T->Scale.Keys,0,sizeof(SplineKey)*Src->Keys);

	Vector stPos, stRot, stScl;
	for (i=0;i<Src->Keys;i++)
	{
		ConvertKey(&Src->KF[i],&T->Pos.Keys[i],&T->Rotate.Keys[i],&T->Scale.Keys[i]);
		T->Pos.Keys[i].Pos.x*=Scene_Scale;
		T->Pos.Keys[i].Pos.y*=Scene_Scale;
		T->Pos.Keys[i].Pos.z*=Scene_Scale;
		if (i==0)
		{
			stPos = T->Pos.Keys[i].Pos.im();
			stRot = T->Rotate.Keys[i].Pos.im();
			stScl = T->Scale.Keys[i].Pos.im();
		} else {
			if ((T->Pos.Keys[i].Pos.x			!= stPos.x) ||
				(T->Pos.Keys[i].Pos.y			!= stPos.y) ||
				(T->Pos.Keys[i].Pos.z			!= stPos.z) ||
				(T->Rotate.Keys[i].Pos.x		!= stRot.x) ||
				(T->Rotate.Keys[i].Pos.y		!= stRot.y) ||
				(T->Rotate.Keys[i].Pos.z		!= stRot.z) ||
				(T->Scale.Keys[i].Pos.x			!= stScl.x) ||
				(T->Scale.Keys[i].Pos.y			!= stScl.y) ||
				(T->Scale.Keys[i].Pos.z			!= stScl.z))
				stationary = false;
		}
	}
	Spline_Init_3D(&T->Pos);
	Spline_Init_3D(&T->Rotate);
	Spline_Init_3D(&T->Scale);

	if (Src->Flags&EndBehavior_Repeat)
	{
		T->Pos.Flags=TrackREPEAT;
		T->Rotate.Flags=TrackREPEAT;
		T->Scale.Flags=TrackREPEAT;
	}

	if (stationary)
	{
		T->SL = (Color *)getAlignedBlock(sizeof(Color) * T->VIndex, 16);
		T->Flags |= Tri_Stationary;
	}

	CurParent=Src->Parent-1;
	return T;
}

Camera *ConvertCamera(FldCamera *Src,Scene *Sc)
{
	TriMesh *Obj;
	Camera *Cam;
	SplineKey *Pos;
	FldKeyFrame *KF;
	float Temp;
	uint32_t i;
	dword n;

	Cam=new Camera;
	memset(Cam,0,sizeof(Camera));

	Cam->Source.NumKeys=Src->Keys;
	Cam->Source.Keys=new SplineKey[Src->Keys];
	memset(Cam->Source.Keys,0,sizeof(SplineKey)*Src->Keys);
	printf("Camera Source:\n");
	for (i = 0; i < Src->Keys; i++) {
		printf("Key: %d: Frame: %f", i, Src->KF[i].FrameNumber);
		Src->KF[i].Position.print();
		Src->KF[i].Rotation.print();
		Src->KF[i].Scale.print();
		printf("\n");

		ConvertKey(&Src->KF[i], &Cam->Source.Keys[i], 1.0);
	}

	Spline_Init_3D(&Cam->Source);
	if (Src->Flags&EndBehavior_Repeat) Cam->Source.Flags=TrackREPEAT;

	if (!Src->TargetObject)
	{
		Cam->Flags=Cam_Euler;
		Cam->Target.NumKeys=Src->Keys;
		Cam->Target.Keys=new SplineKey[Src->Keys];
		memset(Cam->Target.Keys,0,sizeof(SplineKey)*Src->Keys);
		for (i=0;i<Src->Keys;i++)
		{
			ConvertKeyRot(&Src->KF[i],&Cam->Target.Keys[i],Rad2Deg);
			Temp=Cam->Target.Keys[i].Pos.x;
/*			Cam->Target.Keys[i].Pos.x=Cam->Target.Keys[i].Pos.y;
			Cam->Target.Keys[i].Pos.y=Temp;*/
		}
		Spline_Init_3D(&Cam->Target);
		if (Src->Flags&EndBehavior_Repeat) Cam->Target.Flags=TrackREPEAT;

		//Cam->FOV.NumKeys = 1;
		//Cam->FOV.Keys = new SplineKey;
		//memset(Cam->FOV.Keys,0,sizeof(SplineKey));
		//Cam->FOV.Keys->Pos.x = Src->zoomFactor;
		//Src->zoomFactor->
		n = Src->zoomFactor->Keys;
		Cam->FOV.NumKeys = n;
		Cam->FOV.Keys = new SplineKey [n];
		memset(Cam->FOV.Keys,0,sizeof(SplineKey)*n);
		for(i=0; i<n; i++)
		{
			Cam->FOV.Keys[i].Pos.x = ZoomFactor2FOV(Src->zoomFactor->Key[i].Channel[0]);
			Cam->FOV.Keys[i].Tens = Src->zoomFactor->Key[i].Tension;
			Cam->FOV.Keys[i].Bias = Src->zoomFactor->Key[i].Bias;
			Cam->FOV.Keys[i].Cont = Src->zoomFactor->Key[i].Continuity;
		}

		Spline_Init_3D(&Cam->FOV);
	}
	else
	{
		printf("Camera Target:\n");
		Obj=Sc->TriMeshHead;
		for (i=1;i<Src->TargetObject;i++,Obj=Obj->Next);
		memcpy(&Cam->Target,&Obj->Pos,sizeof(Spline));
		Cam->Target.print();
		Cam->Roll.NumKeys=Src->Keys;
		Cam->Roll.Keys=new SplineKey[Src->Keys];
		memset(Cam->Roll.Keys,0,sizeof(SplineKey)*Src->Keys);
		for (i=0;i<Src->Keys;i++)
		{
			KF = &Src->KF[i];
			Pos = &Cam->Roll.Keys[i];
			Pos->Pos.x = KF->Rotation.z;
			if (KF->LinearValue==1.0f)
				Pos->Tens = 1.0f;
			else
				Pos->Tens = KF->Tension;
			Pos->Cont = KF->Continuity;
			Pos->Bias = KF->Bias;
		}
//		Cam->FOV.NumKeys = 1;
//		Cam->FOV.Keys = new SplineKey;
//		memset(Cam->FOV.Keys,0,sizeof(SplineKey));
//		Cam->FOV.Keys->Pos.x = 90.0;

		n = Src->zoomFactor->Keys;
		Cam->FOV.NumKeys = n;
		Cam->FOV.Keys = new SplineKey [n];
		memset(Cam->FOV.Keys,0,sizeof(SplineKey)*n);
		for(i=0; i<n; i++)
		{
			Cam->FOV.Keys[i].Pos.x = ZoomFactor2FOV(Src->zoomFactor->Key[i].Channel[0]);
			Cam->FOV.Keys[i].Tens = Src->zoomFactor->Key[i].Tension;
			Cam->FOV.Keys[i].Bias = Src->zoomFactor->Key[i].Bias;
			Cam->FOV.Keys[i].Cont = Src->zoomFactor->Key[i].Continuity;
		}

		Spline_Init_3D(&Cam->Roll);
		Spline_Init_3D(&Cam->Target);
		Spline_Init_3D(&Cam->FOV);
		if (Src->Flags&EndBehavior_Repeat)
		{
			Cam->Roll.Flags=TrackREPEAT;
			Cam->Target.Flags=TrackREPEAT;
		}


	}

	return Cam;
}

Omni *ConvertOmni(FldLight *Src)
{
	Omni *OL = (Omni *)getAlignedBlock(sizeof(Omni), 16);
	SplineKey *K;
	FldEnvKey *E;
	int i;

	bool stationary = true; 
	float stValue;
	Vector stVec;

	memset(OL,0,sizeof(Omni));

	OL->Pos.NumKeys = Src->Keys;
	OL->Pos.Keys = new SplineKey[Src->Keys];
	memset(OL->Pos.Keys,0,sizeof(SplineKey)*Src->Keys);
	for (i=0;i<Src->Keys;i++)
	{		
		ConvertKey(&Src->KF[i],&OL->Pos.Keys[i],Scene_Scale);
		if (i==0) 
		{
			stVec = OL->Pos.Keys[i].Pos.im();
		} else {
			if ((stVec.x != OL->Pos.Keys[i].Pos.x) ||
				(stVec.y != OL->Pos.Keys[i].Pos.y) ||
				(stVec.z != OL->Pos.Keys[i].Pos.z)) stationary = false;
		}
	}
/*  OL->L.R = Src->Color->Key->Channel[0];
	OL->L.G = Src->Color->Key->Channel[1];
	OL->L.B = Src->Color->Key->Channel[2];  Beep*/
	OL->L.R=Src->Color.Red; OL->L.G=Src->Color.Green; OL->L.B=Src->Color.Blue;

	OL->Size.NumKeys = Src->Intensity->Keys;
	OL->Size.Keys = new SplineKey[Src->Intensity->Keys];
	memset(OL->Size.Keys,0,sizeof(SplineKey)*Src->Intensity->Keys);
	for (i=0;i<Src->Intensity->Keys;i++)
	{
		K=&OL->Size.Keys[i];
		E=&Src->Intensity->Key[i];
		K->Frame=E->FrameNumber;
		K->Pos.x=E->Channel[0];
//		K->Pos.x=1.0;
		if (i==0)
		{
			stValue = K->Pos.x;
		} else {
			if (K->Pos.x != stValue)
				stationary = false;
		}
		K->Tens=E->Tension;
		K->Cont=E->Continuity;
		K->Bias=E->Bias;
	}

	// Range envelope (FLD light flag bit 16 = Light_Range). NOTE for anyone
	// reading Omni::IRange later: this fills the SPLINE only. `OL->IRange`
	// itself stays 0 (the Omni was memset above) until the first
	// Animate_Objects evaluates the spline — Reset_XForm, the only other code
	// that ever set IRange from Keys[0], is dead (see FDS/MISC/PREPROC.CPP).
	// So "IRange == 0 at scene init" means "not animated yet", NOT "the
	// content authored no range".
	//
	// Measured 2026-08-06 — every shipping scene DOES author ranges, and none
	// animates them (all single-key):
	//   greets   10 omnis, Range 3 / 3 / 10 / 10 / 7 / 20 / 20 / 2 / 2 / 2
	//   city     76 lights, Range 800..7000 (one light has no envelope)
	//   chase    41 lights, Range 30000..5e6 (32 have ANIMATED INTENSITY,
	//            which Animate_Objects handles via the Size spline — ranges
	//            themselves are static)
	//   fountain 14 lights, Range 50..100 (one has no envelope)
	//   crash     1 light, no Range envelope
	// Range is NOT multiplied by Scene_Scale here (Scene_Scale is 1.0, so the
	// omission is currently unobservable) whereas Pos keys are — worth knowing
	// if Scene_Scale ever stops being 1.
	//
	// greets caveat: DEMO/GREETS.CPP's greets_omni_default_range patch runs
	// BEFORE the first Animate_Objects, sees IRange == 0 for all 10 omnis, and
	// overwrites BOTH IRange AND Range.Keys[0].Pos.x with the flag value
	// (30 via setDefault). The authored 3/10/7/20/2 above are therefore
	// discarded before anything reads them — verified in a headless run:
	// "[GREETS] patched IRange=30 on 10 FLD omnis (had 0)". That is a
	// deliberate hand-tuned look value, not a fallback; the flag's own
	// description ("the FLD format omits the envelope") is inaccurate — the
	// envelopes are present.
	if (Src->Range)
	{
		OL->Range.NumKeys = Src->Range->Keys;
		OL->Range.Keys = new SplineKey[Src->Range->Keys];
		memset(OL->Range.Keys,0,sizeof(SplineKey)*Src->Range->Keys);
		for (i=0;i<Src->Range->Keys;i++)
		{
			K=&OL->Range.Keys[i];
			E=&Src->Range->Key[i];
			K->Frame=E->FrameNumber;
			K->Pos.x=E->Channel[0];
			if (i==0)
			{
				stValue = K->Pos.x;
			} else {
				if (K->Pos.x != stValue)
					stationary = false;
			}
			K->Tens=//1.0f;
				E->Tension;
			K->Cont=E->Continuity;
			K->Bias=E->Bias;
		}
		Spline_Init_3D(&OL->Range);
	} else
	{
		OL->Range.NumKeys = 1;
		OL->Range.Flags = 0;
		OL->Range.Keys = new SplineKey;
		memset(OL->Range.Keys,0,sizeof(SplineKey));
		OL->Range.Keys[0].Pos.x = 0.0;
		OL->Range.CurKey = 0;
	}

	Spline_Init_3D(&OL->Size);
	Spline_Init_3D(&OL->Pos);
	
	if (Src->Flags&EndBehavior_Repeat)
		OL->Pos.Flags=TrackREPEAT;

	// FLD spot lights (LWS "LightType 2"). Until 2026-07 the converter DROPPED
	// spot records entirely (ConvertFLD only admitted Light_Point), so no
	// shipping FLD contains one — this branch is provably inert for legacy
	// content (verified across every Runtime/SCENES/*.FLD). Convention:
	//   - the authored aim is the light's LOCAL +Z (LightWave's zero-rotation
	//     direction). Authored light-rotation keys remain unsupported, same
	//     as they always were for point lights.
	//   - the FLD ConeAngle envelope's first key is the OUTER cone angle in
	//     degrees; the hotspot (inner) cone is half of it — the same 15/30
	//     ratio the code-created city headlights used (SpotlightCones.h).
	//   - parented spots must never be Omni_Stationary: their single-key Pos
	//     spline holds a LOCAL mount offset and the hierarchy pass composes
	//     the moving parent on top each frame (Transform.cpp), so a static-
	//     lighting bake would freeze them at the wrong place.
	if (Src->Flags & Light_Spot)
	{
		OL->Type = Light_SpotLight;
		float coneDeg = 30.0f;
		if (Src->ConeAngle && Src->ConeAngle->Keys && Src->ConeAngle->Key)
			coneDeg = Src->ConeAngle->Key[0].Channel[0];
		OL->FallOff = cosf(coneDeg * (3.14159265f / 180.0f));
		OL->HotSpot = cosf(0.5f * coneDeg * (3.14159265f / 180.0f));
		// Cone axis = local +Z rotated by the light's AUTHORED first-key rotation
		// (H/P/B), using the same Euler convention as object motion (ConvertKey:
		// engine RX=-rot.y, RY=-rot.x, RZ=-rot.z, radians). The world direction of
		// local +Z is matrix column 2 — identical to the per-frame parented-spot
		// aim (Transform.cpp reads Rot col 2). Parented spots overwrite IDir each
		// frame; NON-parented authored spots (e.g. chase canyon spots) now honor
		// their aim instead of a hardcoded +Z. H=P=B=0 reproduces (0,0,1) exactly,
		// so unrotated spots are byte-identical (this is inert until aim is authored).
		if (Src->Keys > 0) {
			Matrix rm;
			Euler_Angles(rm,
			             -Src->KF[0].Rotation.y * Rad2Deg,
			             -Src->KF[0].Rotation.x * Rad2Deg,
			             -Src->KF[0].Rotation.z * Rad2Deg);
			OL->IDir.x = rm[0][2];
			OL->IDir.y = rm[1][2];
			OL->IDir.z = rm[2][2];
		} else {
			Vector_Form(&OL->IDir, 0.0f, 0.0f, 1.0f);
		}
		stationary = false;
	}

	// Authored volumetric beam (FLD bit 2048, LWS "VolumetricLight 1"):
	// per-light cone-pass opt-in + gain, no scene-wide --draw_cones needed.
	// VolBeamGain 0 = unset → 1.0 at consumption (SoA build). The flag must
	// survive AddOmni's Flags policy assignment — see the preserve mask there.
	if (Src->Flags & Light_VolumetricCone)
	{
		OL->Flags |= Omni_ForceVolCone;
		OL->VolBeamGain = Src->VolBeamGain;
	}

	// Authored per-light flare-sprite size multiplier (FLD bit 4096, LWS
	// "FdsFlareScale <f>"): decouple the lens-flare sprite size from ISize.
	// 0 = unset → 1.0 at consumption (F.FlareSize = ISize × FlareScale, both
	// render passes). Same 0-sentinel convention as VolBeamGain / HaloIntensity.
	if (Src->Flags & Light_FlareScale)
		OL->FlareScale = Src->FlareScale;

	if (stationary)
		OL->Flags |= Omni_Stationary;

	CurParent=Src->Parent-1;
	return OL;
}

Scene *ConvertFLD(FldScene *FLD)
{
	Scene				*Sc;
	TriMesh			*T;
	FldObject		*SourceObj;
	FldLight    *SourceLgt;
	Omni        *OL;
	Object 			*Obj;

#ifdef DebugMode
	printf("Converting FLD...\n");
#endif

	if (!FLD)
	{
		printf("Error! Scene was not loaded!\n");
		exit(1);
	}
	Sc=(Scene *)getAlignedBlock(sizeof(Scene), 16);
	memset(Sc,0,sizeof(Scene));
	Sc->StartFrame = FLD->FirstFrame;
	Sc->EndFrame   = FLD->LastFrame;
	// Authored scene-wide env-reflection defaults (FLD header extension —
	// see FLD_READ.CPP ReadGeneralSceneData; 0 = unset). LoadFLD memcpy's
	// this Scene over the caller's, so the fields survive into the live
	// scene for EnvReflection_FramePrep.
	Sc->EnvReflSceneMode = FLD->EnvReflSceneMode;
	Sc->EnvBakeResScene  = FLD->EnvBakeResScene;

	// Authored backdrop gradient (FLD Scene_SkyColors payload → Scene). QColor
	// stores BGRA bytes; A left 0 (unused by the paint). HasSkyGradient gates
	// the default-off sky_gradient pass (Render_SkyGradient).
	Sc->HasSkyGradient = FLD->SkyColorsSet;
	if (FLD->SkyColorsSet)
	{
		Sc->SkyZenith.R = FLD->ZenithColor.Red; Sc->SkyZenith.G = FLD->ZenithColor.Green; Sc->SkyZenith.B = FLD->ZenithColor.Blue;
		Sc->SkySky.R    = FLD->SkyColor.Red;    Sc->SkySky.G    = FLD->SkyColor.Green;    Sc->SkySky.B    = FLD->SkyColor.Blue;
		Sc->SkyGround.R = FLD->GroundColor.Red; Sc->SkyGround.G = FLD->GroundColor.Green; Sc->SkyGround.B = FLD->GroundColor.Blue;
		Sc->SkyNadir.R  = FLD->NadirColor.Red;  Sc->SkyNadir.G  = FLD->NadirColor.Green;  Sc->SkyNadir.B  = FLD->NadirColor.Blue;
	}

	// Used to walk MatLib to seek to the tail so AddMaterial could append
	// linearly. AddMaterial now head-inserts and dedupes via a hash map
	// (FLD_MAT.CPP), so the prologue is a no-op.

	for (SourceObj=FLD->Object;SourceObj;SourceObj=SourceObj->Next)
	{
		T=ConvertTriMesh(SourceObj);
		AddTriMesh(Sc,T,SourceObj->Name,SourceObj->Pivot);
	}

	for (SourceLgt=FLD->Light;SourceLgt;SourceLgt=SourceLgt->Next)
	{
		if (SourceLgt->Flags&(Light_Point|Light_Spot))
		{
			 OL=ConvertOmni(SourceLgt);
			 AddOmni(Sc,OL,SourceLgt->Name);
		}
	}

	for (Obj = Sc->ObjectHead; Obj; Obj = Obj->Next)
	{
		printf("\tCurrent Object: %s (%u), Parent: %u\n", Obj->Name, Obj->Number, Obj->ParentID);
		CurParent = (signed short)Obj->ParentID;
		if (CurParent >= 0)
		{
			Object *cur = Sc->ObjectHead;
			for (; cur; cur=cur->Next)
			{
				if (cur->Number == CurParent)
				{
					Obj->Parent = cur;
					printf("\t\tFound Parent Object: %s (%u)\n", cur->Name, cur->Number);
					break;
				}
			}
			if (!cur) {
				printf("Warning! Parent mismatch!\n");
				Obj->Parent = NULL;
			}
		}
		else { Obj->Parent = NULL; }
	}
	Sc->CameraHead=ConvertCamera(FLD->Camera,Sc);


#ifdef DebugMode
	printf("Sorting by hierarchy..\n");
#endif
	Hierarchy_Sort(Sc);
	return Sc;
}


void Delete_Envelope(FldEnv *E)
{
	if (!E) return;

/*	for (i=0;i<E->Keys;i++) delete (E->Key[i].Channel);*/

	delete E;
}

void Delete_Scene(FldScene *Sc)
{
	FldLight *L;
	FldObject *O;

	Delete_Envelope(Sc->AmbientColor);
	Delete_Envelope(Sc->AmbientIntensity);
// Delete camera
	delete Sc->Camera->KF;
	Delete_Envelope(Sc->Camera->zoomFactor);
	delete Sc->Camera;
// Delete lights
	FldLight *TempLight = NULL;
	for (L=Sc->Light;L;L=L->Next)
	{
		if (TempLight) delete TempLight;
//		delete L->Name;
		delete L->KF;
		Delete_Envelope(L->Intensity);
		Delete_Envelope(L->Falloff);
		Delete_Envelope(L->ConeAngle);
		TempLight = L;
	}
	if (TempLight) delete TempLight;
// Delete objects
	FldObject *TempObj = NULL;
	for (O=Sc->Object;O;O=O->Next)
	{
		if (TempObj) delete TempObj;
//		delete O->Name;
// Delete materials
/*		for (i=0,M=&O->Material[i];i<O->NumOfMat;i++)
		{
			delete M->Name;
			delete M->ReflectionImage;
			delete M->ColorTexture;
			delete M->DiffuseTexture;
			delete M->SpecularTexture;
			delete M->ReflectionTexture;
			delete M->TransparencyTexture;
			delete M->BumpTexture;
			delete M->TextureImage;
			delete M->TextureAlpha;
		}*/
		delete O->Material;

		delete O->Verts;
		delete O->Faces;
		delete O->KF;
		Delete_Envelope(O->PolygonSize);
		TempObj = O;
	}
	if (TempObj) delete TempObj;
	// Test for memory bugs:
	int Size=DPMI_Free_Memory()-500000;
//	char *d=new char[Size];
//bc	printf("%i\n",d);
//	memset(d,0,Size);
//	delete d;
	delete Sc;
	Size=DPMI_Free_Memory();
}

char LoadFLD(Scene *Sc, const char *FileName)
{
	//SceneGroup = EnumScene(Sc);
	SceneGroup = Sc;
	FldScene *FS = ReadFLD(FileName);
	if (!FS) return 0;
	Scene *OUT = ConvertFLD(FS);
	printf("Deleting scene...\n");
	dword MFree=DPMI_Free_Memory();
//	Delete_Scene(FS);
	printf("%u bytes were freed\n",DPMI_Free_Memory()-MFree);
	if (!OUT) return 0;
	memcpy(Sc,OUT,sizeof(Scene));
	freeAlignedBlock(OUT);
	return 1;
}
