/*
		 Flood Demo System - Flood scene file converter
		 -----------------------------------------------
*/

#include <STDIO.H>
#include <DOS.H>
#include <STDLIB.H>
#include <CONIO.H>
#include <STRING.H>
#include <MATH.H>

#include "../FDS_Defs.H"
#include "../FDS_Vars.H"
#include "../FDS_Decs.H"

#include "LWRead.H"
#include "FLD_Read.H"

//#define DebugMode
#define Scene_Scale 1.0

const float Rad2Deg = PI/180.0;

Material *CurMat,*FirstMat;
signed short CurParent;
Scene *SceneGroup;

void AddTriMesh(Scene *Sc,TriMesh *T,char *ObjName,Vector Pivot)
{
	long I;
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
	Obj->Parent=(Object *)CurParent;
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
	long I;
	Face *F;
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
	Obj->Parent = (Object *)CurParent;   // was NULL — dropped the FLD parenting so parented lights
	                                     // (robot/alcoves) never followed their parent. Resolve pass
	                                     // below turns the index into a pointer (or NULL if unparented).
	Vector_Form(&Om->IPos,0,0,0);
	// NOTE: do NOT wipe Om->Flags here — ConvertOmni already set Flare_LightSource (+ Flare_Parented
	// for the mech's parented flares). The old code cleared it and relied on Init_Flares to re-set
	// Flare_LightSource (along with the spurious random colour); both are gone now, so clearing here
	// left every omni with Flags=0 -> skipped in Lighting() -> the whole room went dark.
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
	Matrix M;

	Pos->Frame=Rot->Frame=Scl->Frame=KF->FrameNumber;

// Convert position
	Pos->Pos.X=KF->Position.X;
	Pos->Pos.Y=KF->Position.Y;
	Pos->Pos.Z=KF->Position.Z;
	if (KF->LinearValue==1.0f) Pos->Tens=1.0f; else Pos->Tens=KF->Tension;   // master parity: LINEAR keyframe -> tight (linear) interpolation
	Pos->Cont=KF->Continuity;
	Pos->Bias=KF->Bias;
// Convert scale
	Scl->Pos.X=KF->Scale.X;
	Scl->Pos.Y=KF->Scale.Y;
	Scl->Pos.Z=KF->Scale.Z;
	if (KF->LinearValue==1.0f) Scl->Tens=1.0f; else Scl->Tens=KF->Tension;   // master parity
	Scl->Cont=KF->Continuity;
	Scl->Bias=KF->Bias;
// Convert rotation
	Rot->Pos.X=-KF->Rotation.Y*Rad2Deg; // X and Y are reversed!
	Rot->Pos.Y=-KF->Rotation.X*Rad2Deg;
	Rot->Pos.Z=-KF->Rotation.Z*Rad2Deg;
	if (KF->LinearValue==1.0f) Rot->Tens=1.0f; else Rot->Tens=KF->Tension;   // master parity
	Rot->Cont=KF->Continuity;
	Rot->Bias=KF->Bias;
}

void ConvertKey(FldKeyFrame *KF,SplineKey *Pos,float Scl) //OverLoading rules!
{
	Pos->Frame=KF->FrameNumber;
	Pos->Pos.X=KF->Position.X*Scl;
	Pos->Pos.Y=KF->Position.Y*Scl;
	Pos->Pos.Z=KF->Position.Z*Scl;
	if (KF->LinearValue==1.0f) Pos->Tens=1.0f; else Pos->Tens=KF->Tension;   // master parity: LINEAR keyframe -> tight (linear) interpolation
	Pos->Cont=KF->Continuity;
	Pos->Bias=KF->Bias;
}

void ConvertKeyRot(FldKeyFrame *KF,SplineKey *Rot,float Scl)
{
	Rot->Frame=KF->FrameNumber;
	Rot->Pos.X=KF->Rotation.X*Scl;
	Rot->Pos.Y=KF->Rotation.Y*Scl;
	Rot->Pos.Z=KF->Rotation.Z*Scl;
	if (KF->LinearValue==1.0f) Rot->Tens=1.0f; else Rot->Tens=KF->Tension;   // master parity
	Rot->Cont=KF->Continuity;
	Rot->Bias=KF->Bias;
}

void ConvertFace(FldFace *Org,Face *Dest,Vertex *V,int *Cur,FldMat *FM)
{
	Material *FaceMat;
	int i;

	FaceMat = FirstMat;
	for (i=0;i<Org->Surface;i++)
	{
		FaceMat=FaceMat->Next;
		if (!FaceMat)
		{
			printf("Error! No material for current face!\n");
			exit(200);
		}
	}
	// Stupid fan triangulator
	for (i=Org->FaceVerts-2;i>0;i--)
	{
		Dest[*Cur].A=V+Org->Verts[Org->FaceVerts-1];
		Dest[*Cur].B=V+Org->Verts[i];
		Dest[*Cur].C=V+Org->Verts[i-1];
		Dest[*Cur].Txtr=FaceMat;
		Get_Mapping(&Dest[*Cur],FM+Org->Surface); // Get mapping for current face using FM
		(*Cur)++;
	}
}

TriMesh *ConvertTriMesh(FldObject *Src)
{
	int i,j,TotalFaces;
	TriMesh *T;
	void    *Lgt;

	T=new TriMesh;
	memset(T,0,sizeof(TriMesh));
	T->Flags = Tri_Euler;
#ifdef DebugMode
	printf("Loading materials...\n");
#endif
	AddMaterial(Src->Material,SceneGroup);
  FirstMat = CurMat;
  for (i=1;i<Src->NumOfMat;i++)
		AddMaterial(&Src->Material[i],SceneGroup);

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

  // was TotalFaces+=2 (in pristine dos-rev too): allocated 2 EXTRA Face slots the
  // fan triangulator never fills -> they stay memset-0 (A=B=C=NULL, Txtr=NULL,
  // N=0). Render() reads A->TPos.Z off the NULL ptr = heap garbage -> wild Z ->
  // the near-clipper interpolates toward infinity -> radiating vertex streaks on
  // the mech. Heap-layout dependent (original binary happened to survive it).
  // The fan emits EXACTLY sum(FaceVerts-2) real triangles, so drop the +2.

	T->FIndex=TotalFaces;
	T->Faces=new Face[TotalFaces];
	memset(T->Faces,0,sizeof(Face)*TotalFaces);
	j=0;

#ifdef DebugMode
	printf("Trimesh name: %s\n",Src->Name);
#endif
	for (i=0;i<Src->NumOfFaces;i++)
		ConvertFace(&Src->Faces[i],T->Faces,T->Verts,&j,Src->Material);

	T->Pos.NumKeys=T->Rotate.NumKeys=T->Scale.NumKeys=Src->Keys;
	T->Pos.Keys=new SplineKey[Src->Keys];
	T->Rotate.Keys=new SplineKey[Src->Keys];
	T->Scale.Keys=new SplineKey[Src->Keys];
	memset(T->Pos.Keys,0,sizeof(SplineKey)*Src->Keys);
	memset(T->Rotate.Keys,0,sizeof(SplineKey)*Src->Keys);
	memset(T->Scale.Keys,0,sizeof(SplineKey)*Src->Keys);

	for (i=0;i<Src->Keys;i++)
	{
		ConvertKey(&Src->KF[i],&T->Pos.Keys[i],&T->Rotate.Keys[i],&T->Scale.Keys[i]);
		T->Pos.Keys[i].Pos.X*=Scene_Scale;
		T->Pos.Keys[i].Pos.Y*=Scene_Scale;
		T->Pos.Keys[i].Pos.Z*=Scene_Scale;
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
	if (Src->Flags&Object_AlignToPath) T->Flags|=Tri_AlignToPath;
	// Mark parented meshes (mech Hull/legs) so parented "mech flare" omnis skip self-lighting them.
	if (Src->Parent) T->Flags|=Tri_Parented;
	CurParent=Src->Parent-1;
	return T;
}

// LightWave zoom factor -> horizontal FOV in degrees (matches the native engine).
static float ZoomFactor2FOV(float zf)
{
	if (zf<=0.0f) zf = 3.2f;
	return 360.0f/PI * atan( ((float)XRes/(float)YRes)/zf );
}

// Load the camera FOV from the FLD's ZoomFactor envelope (was hardcoded to 90 for
// every camera, ignoring the authored zoom -> greets shot came out far too wide).
static void Load_Camera_FOV(Camera *Cam, FldCamera *Src)
{
	long nz = (Src->ZoomFactor && Src->ZoomFactor->Keys>0) ? (long)Src->ZoomFactor->Keys : 1;
	Cam->FOV.NumKeys = nz;
	Cam->FOV.Keys = new SplineKey[nz];
	memset(Cam->FOV.Keys,0,sizeof(SplineKey)*nz);
	if (Src->ZoomFactor && Src->ZoomFactor->Keys>0)
		for (long zi=0;zi<nz;zi++)
		{
			Cam->FOV.Keys[zi].Pos.X = ZoomFactor2FOV(Src->ZoomFactor->Key[zi].Channel[0]);
			Cam->FOV.Keys[zi].Frame = Src->ZoomFactor->Key[zi].FrameNumber;
			Cam->FOV.Keys[zi].Tens  = Src->ZoomFactor->Key[zi].Tension;
			Cam->FOV.Keys[zi].Cont  = Src->ZoomFactor->Key[zi].Continuity;
			Cam->FOV.Keys[zi].Bias  = Src->ZoomFactor->Key[zi].Bias;
		}
	else Cam->FOV.Keys[0].Pos.X = 90.0f;
	Spline_Init_3D(&Cam->FOV);
}

Camera *ConvertCamera(FldCamera *Src,Scene *Sc)
{
	TriMesh *Obj;
	Camera *Cam;
	SplineKey *Pos;
	FldKeyFrame *KF;
	float Roll,Temp;
	int i;

	Cam=new Camera;
	memset(Cam,0,sizeof(Camera));

	Cam->Source.NumKeys=Src->Keys;
	Cam->Source.Keys=new SplineKey[Src->Keys];
	memset(Cam->Source.Keys,0,sizeof(SplineKey)*Src->Keys);
	for (i=0;i<Src->Keys;i++)
		ConvertKey(&Src->KF[i],&Cam->Source.Keys[i],1.0);
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
			Temp=Cam->Target.Keys[i].Pos.X;
/*			Cam->Target.Keys[i].Pos.X=Cam->Target.Keys[i].Pos.Y;
			Cam->Target.Keys[i].Pos.Y=Temp;*/
		}
		Spline_Init_3D(&Cam->Target);
		if (Src->Flags&EndBehavior_Repeat) Cam->Target.Flags=TrackREPEAT;
		Load_Camera_FOV(Cam,Src);
	}
	else
	{
		Obj=Sc->TriMeshHead;
		for (i=1;i<Src->TargetObject;i++,Obj=Obj->Next);
		memcpy(&Cam->Target,&Obj->Pos,sizeof(Spline));
		Cam->Roll.NumKeys=Src->Keys;
		Cam->Roll.Keys=new SplineKey[Src->Keys];
		memset(Cam->Roll.Keys,0,sizeof(SplineKey)*Src->Keys);
		for (i=0;i<Src->Keys;i++)
		{
			KF = &Src->KF[i];
			Pos = &Cam->Roll.Keys[i];
			Pos->Pos.X = KF->Rotation.Z;
			Pos->Tens = KF->Tension;
			Pos->Cont = KF->Continuity;
			Pos->Bias = KF->Bias;
		}
		Spline_Init_3D(&Cam->Roll);
		Spline_Init_3D(&Cam->Target);
		Load_Camera_FOV(Cam,Src);
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
	Omni *OL = new Omni;
	memset(OL,0,sizeof(Omni));
	int i;

	OL->Pos.NumKeys = Src->Keys;
	OL->Pos.Keys = new SplineKey[Src->Keys];
	memset(OL->Pos.Keys,0,sizeof(SplineKey)*Src->Keys);
	for (i=0;i<Src->Keys;i++)
		ConvertKey(&Src->KF[i],&OL->Pos.Keys[i],Scene_Scale);

	Spline_Init_3D(&OL->Pos);

	if (Src->Flags&EndBehavior_Repeat)
		OL->Pos.Flags=TrackREPEAT;

	// L = FLD Color (0..255) * LScale. The greets room is large (Piramid radius ~92u), so the
	// DOS forward Lighting() (LR += L*cos*384/dist²) needs a big colour magnitude for lights to
	// carry across it — LScale=56.86 maps 255->~14500, the intended DOS light scale. This is
	// what makes the room glow (room lights + the mech's blue "mech flare" omnis, the authored
	// look). The on-mech rainbow blast that this scale used to cause is now prevented by self-
	// exclusion (Flare_Parented below): the mech-flare omnis light the ROOM but not the mech.
	const float LScale=56.86f;
	OL->L.R = (float)Src->Color.Red   * LScale;
	OL->L.G = (float)Src->Color.Green * LScale;
	OL->L.B = (float)Src->Color.Blue  * LScale;
	OL->Flags |= Flare_LightSource;

	// A parented omni (the mech's blue "mech flare" lights, parented to the Hull) lights the
	// ROOM but must NOT illuminate the mech it is embedded in — otherwise point-blank 1/dist²
	// blasts the mech into a rainbow. Mark it so Lighting() skips parented meshes for it.
	if (Src->Parent) OL->Flags |= Flare_Parented;

	CurParent=Src->Parent-1;
	return OL;
}

Scene *ConvertFLD(FldScene *FLD)
{
	Scene				*Sc;
	TriMesh			*T;
	Camera			*C;
	FldObject		*SourceObj;
	FldLight    *SourceLgt;
	SpotLight   *SL;
	Omni        *OL;
	Object 			*Obj;
	int					 i;

#ifdef DebugMode
	printf("Converting FLD...\n");
#endif

	if (!FLD)
	{
		printf("Error! Scene was not loaded!\n");
		exit(1);
	}
	Sc=new Scene;
	memset(Sc,0,sizeof(Scene));
	Sc->StartFrame = FLD->FirstFrame;
	Sc->EndFrame   = FLD->LastFrame;

  MatLib = NULL;
	CurMat = MatLib;

	for (SourceObj=FLD->Object;SourceObj;SourceObj=SourceObj->Next)
	{
		fprintf(LogFile,"  CV:src NumFaces=%lu NumVerts=%lu NumMat=%lu name=%s\n",SourceObj->NumOfFaces,SourceObj->NumOfVerts,SourceObj->NumOfMat,SourceObj->Name?SourceObj->Name:"?");LOGSYNC();
		T=ConvertTriMesh(SourceObj);
		fprintf(LogFile,"  CV:->T FIndex=%ld VIndex=%ld\n",(long)T->FIndex,(long)T->VIndex);LOGSYNC();
		AddTriMesh(Sc,T,SourceObj->Name,SourceObj->Pivot);
	}

	for (SourceLgt=FLD->Light;SourceLgt;SourceLgt=SourceLgt->Next)
	{
		fprintf(LogFile,"LGT: name=%s Flags=%lx point=%d spot=%d Col=(%d,%d,%d) IntKeys=%ld\n",
			SourceLgt->Name?SourceLgt->Name:"?",(unsigned long)SourceLgt->Flags,
			(SourceLgt->Flags&Light_Point)?1:0,(SourceLgt->Flags&Light_Spot)?1:0,
			(int)SourceLgt->Color.Red,(int)SourceLgt->Color.Green,(int)SourceLgt->Color.Blue,
			SourceLgt->Intensity?(long)SourceLgt->Intensity->Keys:-1);
		LOGSYNC();
		if (SourceLgt->Flags&Light_Point)
		{
			 OL=ConvertOmni(SourceLgt);
			 AddOmni(Sc,OL,SourceLgt->Name);
		}
	}

	for (Obj=Sc->ObjectHead;Obj;Obj=Obj->Next)
	{
		CurParent=(signed short)Obj->Parent;
		if (CurParent>=0)
		{
			Obj->Parent=Sc->ObjectHead;
			for (i=0;i<CurParent;i++)
			{
				Obj->Parent=Obj->Parent->Next;
				if (!Obj->Parent) printf("Warning! Parent mismatch!\n");
      }
		}
		else Obj->Parent=NULL;
	}
	Sc->CameraHead=ConvertCamera(FLD->Camera,Sc);


#ifdef DebugMode
	printf("Sorting by hierarchy..\n");
#endif
	Hierarchy_Sort(Sc);
	return Sc;
}


char LoadFLD(Scene *Sc,char *FileName)
{
	//SceneGroup = EnumScene(Sc);
  SceneGroup = Sc;
	fprintf(LogFile,"LF0:preReadFLD %s\n",FileName);fflush(LogFile);
	FldScene *FS = ReadFLD(FileName);
	fprintf(LogFile,"LF1:postReadFLD FS=%p\n",(void*)FS);fflush(LogFile);
	if (!FS) return 0;
	Scene *OUT = ConvertFLD(FS);
	fprintf(LogFile,"LF2:postConvertFLD OUT=%p szScene(engine)=%d\n",(void*)OUT,(int)sizeof(Scene));LOGSYNC();
	if (!OUT) return 0;
	{ TriMesh *_t; long _n=0; for(_t=OUT->TriMeshHead;_t&&_n<8;_t=_t->Next){ fprintf(LogFile,"  M@OUT #%ld FI=%ld VI=%ld\n",++_n,(long)_t->FIndex,(long)_t->VIndex);LOGSYNC(); } }
	memcpy(Sc,OUT,sizeof(Scene));
	delete OUT;
	{ TriMesh *_t; long _n=0; for(_t=Sc->TriMeshHead;_t&&_n<8;_t=_t->Next){ fprintf(LogFile,"  M@postcpy #%ld FI=%ld VI=%ld\n",++_n,(long)_t->FIndex,(long)_t->VIndex);LOGSYNC(); } }
	fprintf(LogFile,"LF3:done\n");fflush(LogFile);
	return 1;
}

