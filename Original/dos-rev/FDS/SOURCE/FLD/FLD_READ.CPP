/*
		 Flood Demo System - Flood scene file reader
		 -------------------------------------------
*/

#include <STDIO.H>
#include <DOS.H>
#include <STDLIB.H>
#include <CONIO.H>
#include <STRING.H>
#include <MATH.H>

#include "..\FDS_Defs.H"
#include "..\FDS_Vars.H"
#include "..\FDS_Decs.H"

#include "LWRead.H"
#include "FLD_READ.H"


//#define DebugMode

float FldVersion=0.111;

FILE 			*FLD;


//Chunks
#define Chunk_Object					0x1000
#define Chunk_Light						0x2000
#define Chunk_Camera					0x3000

char ReadObjectNames, ReadMaterialNames, ReadLightNames;

void ReadString(char **s)
{
	int a=0;
	char Temp[256];
	do fread(&Temp[a],1,1,FLD); while (Temp[a++]);
	*s=strdup(Temp);
}

FldEnv *ReadEnvelope(void)
{
	int a,b;
	FldEnv *Env;

	Env=new FldEnv;
	fread(&Env->Keys,1,4,FLD);
	fread(&Env->Channels,1,4,FLD);
	fread(&Env->EndBehavior,1,4,FLD);
	Env->Key=new FldEnvKey[Env->Keys];
	for (a=0;a<Env->Keys;a++)
	{
		for (b=0;b<Env->Channels;b++)
		{
			Env->Key[a].Channel=new float[Env->Channels];
			fread(&Env->Key[a].Channel[b],1,4,FLD);
		}
		fread(&Env->Key[a].FrameNumber,1,4,FLD);
		fread(&Env->Key[a].LinearValue,1,4,FLD);
		fread(&Env->Key[a].Tension,1,4,FLD);
		fread(&Env->Key[a].Continuity,1,4,FLD);
		fread(&Env->Key[a].Bias,1,4,FLD);
	}
	return Env;
}

void ReadGeneralSceneData(FldScene *Sc)
{
	char ID[8],Names[3];
	float Ver;
	int i;

	ID[7]=0;
	fread(ID,1,7,FLD);
	if (strcmp(ID,"Flood3D")!=0)
	{
		printf("Error! Incorrect ID! (%s)\n",ID);
		exit(1);
	}
	fread(&Ver,1,4,FLD);
	if (Ver!=FldVersion)
	{
		printf("Error! Saving and loading version of file is incompatible!(%f/%f)\n",Ver,FldVersion);
		exit(1);
	}
	fread(&Sc->FirstFrame,1,4,FLD);
	fread(&Sc->LastFrame,1,4,FLD);
	fread(&Sc->FrameStep,1,4,FLD);
	fread(&Sc->FramesPerSecond,1,4,FLD);
	Sc->AmbientColor=ReadEnvelope();
	Sc->AmbientIntensity=ReadEnvelope();
	fread(Names,1,3,FLD);
	ReadObjectNames=(Names[0]=='O');
	ReadMaterialNames=(Names[1]=='M');
	ReadLightNames=(Names[2]=='L');
}

void ReadMaterial(FldMat *Mat)
{
	if (ReadMaterialNames)
	{
		ReadString(&Mat->Name);
#ifdef DebugMode
		printf("-----|Material name: \"%s\"\n",Mat->Name);
#endif
	}
	fread(&Mat->Color,1,sizeof(FldColor),FLD);
	fread(&Mat->Flags,1,2,FLD);
	fread(&Mat->Luminosity,1,4,FLD);
	fread(&Mat->Diffuse,1,4,FLD);
	fread(&Mat->Specular,1,4,FLD);
	fread(&Mat->Reflection,1,4,FLD);
	fread(&Mat->Transparency,1,4,FLD);
	fread(&Mat->Glossiness,1,2,FLD);
	fread(&Mat->ReflectionMode,1,2,FLD);
	ReadString(&Mat->ReflectionImage);
	fread(&Mat->ReflectionSeamAngle,1,4,FLD);
	fread(&Mat->RefractiveIndex,1,4,FLD);
	fread(&Mat->EdgeTransparency,1,4,FLD);
	fread(&Mat->MaxSmoothingAngle,1,4,FLD);
	ReadString(&Mat->ColorTexture);
	ReadString(&Mat->DiffuseTexture);
	ReadString(&Mat->SpecularTexture);
	ReadString(&Mat->ReflectionTexture);
	ReadString(&Mat->TransparencyTexture);
	ReadString(&Mat->BumpTexture);
	ReadString(&Mat->TextureImage);
	fread(&Mat->TextureFlags,1,2,FLD);
	fread(&Mat->TextureSize,1,sizeof(Vector),FLD);
	fread(&Mat->TextureCenter,1,sizeof(Vector),FLD);
	fread(&Mat->TextureFallOff,1,sizeof(Vector),FLD);
	fread(&Mat->TextureVelocity,1,sizeof(Vector),FLD);
	ReadString(&Mat->TextureAlpha);
	fread(&Mat->NoiseFrequencies,1,2,FLD);
	fread(&Mat->TextureWrapX,1,2,FLD);
	fread(&Mat->TextureWrapY,1,2,FLD);
	fread(&Mat->AAStrength,1,4,FLD);
	fread(&Mat->Opacity,1,4,FLD);

#ifdef DebugMode
	printf("---S---|Color texture = %s\n",Mat->ColorTexture);
	printf("---u---|Diffuse texture = %s\n",Mat->DiffuseTexture);
	printf("---r---|Specular texture = %s\n",Mat->SpecularTexture);
	printf("---f---|Reflection texture = %s\n",Mat->ReflectionTexture);
	printf("---a---|Trans texture = %s\n",Mat->TransparencyTexture);
	printf("---c---|Bump texture = %s\n",Mat->BumpTexture);
	printf("---e---|Texture image = %s\n",Mat->TextureImage);
#endif
}

void ReadObject(FldObject *Obj)
{
	int i,j;

#ifdef DebugMode
	printf("-|Loading object...\n");
#endif
	if (ReadObjectNames)
	{
		ReadString(&Obj->Name);
#ifdef DebugMode
		printf("---|Object name: \"%s\"\n",Obj->Name);
#endif
	}
	fread(&Obj->Flags,1,4,FLD);
	fread(&Obj->NumOfMat,1,4,FLD);
#ifdef DebugMode
	printf("---|Loading %i materials...\n",Obj->NumOfMat);
#endif
	if (Obj->NumOfMat) Obj->Material=new FldMat[Obj->NumOfMat];
	for (i=0;i<Obj->NumOfMat;i++) ReadMaterial(&Obj->Material[i]);
#ifdef DebugMode
	printf("---|Loading modeller info(faces/vertices)...\n");
#endif
	fread(&Obj->NumOfVerts,1,4,FLD);
  if (Obj->NumOfVerts) Obj->Verts=new Vector[Obj->NumOfVerts];
	for (i=0;i<Obj->NumOfVerts;i++)
		fread(&Obj->Verts[i],1,sizeof(Vector),FLD);
	fread(&Obj->NumOfFaces,1,4,FLD);
	if (Obj->NumOfFaces) Obj->Faces=new FldFace[Obj->NumOfFaces];
	for (i=0;i<Obj->NumOfFaces;i++)
	{
		fread(&Obj->Faces[i].FaceVerts,1,2,FLD);
		Obj->Faces[i].Verts=new short[Obj->Faces[i].FaceVerts];
		for (j=0;j<Obj->Faces[i].FaceVerts;j++)
			fread(&Obj->Faces[i].Verts[j],1,2,FLD);
		fread(&Obj->Faces[i].Surface,1,2,FLD);
	}
#ifdef DebugMode
	printf("---|Loading keyframer info...\n");
#endif
	fread(&Obj->Keys,1,4,FLD);
	Obj->KF=new FldKeyFrame[Obj->Keys];
#ifdef DebugMode
	printf("-----|Loading %i keys...\n",Obj->Keys);
#endif
	for (i=0;i<Obj->Keys;i++) fread(&Obj->KF[i],1,sizeof(FldKeyFrame),FLD);
	if (Obj->Flags&1) fread(&Obj->Parent,1,4,FLD);
	if (Obj->Flags&2) fread(&Obj->Pivot,1,sizeof(Vector),FLD);
	if (Obj->Flags&4) Obj->PolygonSize=ReadEnvelope();
}

void ReadLight(FldLight *Light)
{
	int i,j;

#ifdef DebugMode
	printf("-|Loading light...\n");
#endif
	if (ReadLightNames)
	{
		ReadString(&Light->Name);
#ifdef DebugMode
		printf("---|Light name: \"%s\"\n",Light->Name);
#endif
	}
	fread(&Light->Flags,1,4,FLD);
#ifdef DebugMode
	printf("---|Loading keyframer info...\n");
#endif
	fread(&Light->Keys,1,4,FLD);
	Light->KF=new FldKeyFrame[Light->Keys];
	for (i=0;i<Light->Keys;i++) fread(&Light->KF[i],1,sizeof(FldKeyFrame),FLD);
	Light->Color=ReadEnvelope();
	Light->Intensity=ReadEnvelope();
	if (Light->Flags&1) fread(&Light->Parent,1,4,FLD);
	if (Light->Flags&2) fread(&Light->TargetObject,1,4,FLD);
	if (Light->Flags&4) Light->Falloff=ReadEnvelope();
	if (Light->Flags&8) Light->ConeAngle=ReadEnvelope();
}

void ReadCamera(FldCamera *Cam)
{
	int i,j;

#ifdef DebugMode
	printf("-|Loading Camera....\n");
#endif
	fread(&Cam->Flags,1,4,FLD);
#ifdef DebugMode
	printf("---|Loading keyframer info...\n");
#endif
	fread(&Cam->Keys,1,4,FLD);
	Cam->KF=new FldKeyFrame[Cam->Keys];
	for (i=0;i<Cam->Keys;i++) fread(&Cam->KF[i],1,sizeof(FldKeyFrame),FLD);
	Cam->ZoomFactor=ReadEnvelope();
	if (Cam->Flags&1) fread(&Cam->Parent,1,4,FLD);
	if (Cam->Flags&2) fread(&Cam->TargetObject,1,4,FLD);
}

void OpenFLD(char *FileName)
{
	if (!(FLD=fopen(FileName,"rb")))
	{
		fclose(FLD);
		printf("Error! Unable to open Flood file\"%s\"!\n",FileName);
		exit(1);
	}
}

FldScene *ReadFLD(char *FileName)
{
	FldObject *Obj;
	FldLight	*Lgt;
	FldCamera	*Cam;
	FldScene  *Sc;
	unsigned short Chunk;

	printf("Loading FLD file: \"%s\" (version %.3f)...\n",FileName,FldVersion);

	OpenFLD(FileName);
	Sc=new FldScene;
	memset(Sc,0,sizeof(FldScene));
#ifdef DebugMode
	printf("Loading general scene data...\n");
#endif
	ReadGeneralSceneData(Sc);
#ifdef ReadDebugMode
	printf("Loading object information:\n");
#endif
	while (!feof(FLD))
	{
		fread(&Chunk,1,2,FLD);
		switch (Chunk)
		{
		case Chunk_Object:
			if (!Sc->Object)
			{
				Sc->Object=new FldObject;
				Obj=Sc->Object;
			}
			else
			{
				Obj->Next=new FldObject;
				Obj=Obj->Next;
			}
			memset(Obj,0,sizeof(FldObject));
			ReadObject(Obj);
			break;
		case Chunk_Light:
			if (!Sc->Light)
			{
				Sc->Light=new FldLight;
				Lgt=Sc->Light;
			}
			else
			{
				Lgt->Next=new FldLight;
				Lgt=Lgt->Next;
			}
			memset(Lgt,0,sizeof(FldLight));
			ReadLight(Lgt);
			break;
		case Chunk_Camera:
			Sc->Camera=new FldCamera;
			memset(Sc->Camera,0,sizeof(FldCamera));
			ReadCamera(Sc->Camera);
			goto CloseFile;
			break;
		}
	}
CloseFile:
#ifdef DebugMode
	printf("Closing file...\n");
#endif
	fclose(FLD);
#ifdef DebugMode
	printf("FLD Reading complete!\n");
#endif

	return Sc;
}