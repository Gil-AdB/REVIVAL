/*
Flood Demo System - Flood scene file reader
-------------------------------------------
*/

#include <stdio.h>
//#include <dos.h>
#include <stdlib.h>
//#include <conio.h>
#include <string.h>
#include <math.h>

#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"

#include "LWREAD.H"
#include "FLD_READ.H"

// Verbose FLD-loader tracing (per-object/material/light tree dump) is
// off by default — it spammed hundreds of lines per scene load. Set
// FDS_FLD_VERBOSE=1 to restore it. Error lines stay on `printf`.
static bool fld_verbose() {
	static int v = -1;
	if (v < 0) {
		const char *e = getenv("FDS_FLD_VERBOSE");
		v = (e && *e && *e != '0') ? 1 : 0;
	}
	return v;
}
#define FLDLOG(...) do { if (fld_verbose()) printf(__VA_ARGS__); } while (0)


#define DebugMode

float FldVersion=0.113;

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
	*s=_strdup(Temp);
}

FldEnv *ReadEnvelope(void)
{
	uint32_t a,b;
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
	// Bit 2048 = Scene_EnvDefaults (authored scene-wide env-reflection
	// defaults, LWS "FdsSceneEnvRefl"/"FdsSceneEnvBakeRes"): two int32s,
	// written conditionally by tools/lwsread FLDSAVE.CPP. Same provably-
	// inert-for-legacy extension idiom as Light_VolumetricCone — legacy
	// writers only ever store EndBehavior values <= 1024 here. The bit is
	// STRIPPED after reading so anything consuming the envelope's
	// EndBehavior sees the legacy value (Sc is memset(0) by the caller,
	// so bit-free files leave both fields 0 = unset).
	if (Sc->AmbientIntensity->EndBehavior & Scene_EnvDefaults)
	{
		fread(&Sc->EnvReflSceneMode,1,4,FLD);
		fread(&Sc->EnvBakeResScene,1,4,FLD);
		Sc->AmbientIntensity->EndBehavior &= ~(uint32_t)Scene_EnvDefaults;
		printf("[FLD] scene env defaults: envRefl=%d envBakeRes=%d\n",
		       (int)Sc->EnvReflSceneMode, (int)Sc->EnvBakeResScene);
	}
	// Bit 4096 = Scene_SkyColors (authored backdrop gradient): four FldColor
	// triples, written after the env-defaults payload by tools/lwsread. Same
	// provably-inert-for-legacy idiom — legacy writers never set >1024 (env-
	// defaults aside) on this envelope's EndBehavior, and Sc is memset(0) by
	// the caller so a bit-free FLD leaves SkyColorsSet=0 (legacy black void).
	// Strip the bit so EndBehavior consumers never see it.
	if (Sc->AmbientIntensity->EndBehavior & Scene_SkyColors)
	{
		fread(&Sc->ZenithColor,1,sizeof(FldColor),FLD);
		fread(&Sc->SkyColor,1,sizeof(FldColor),FLD);
		fread(&Sc->GroundColor,1,sizeof(FldColor),FLD);
		fread(&Sc->NadirColor,1,sizeof(FldColor),FLD);
		Sc->SkyColorsSet = 1;
		Sc->AmbientIntensity->EndBehavior &= ~(uint32_t)Scene_SkyColors;
		printf("[FLD] scene sky gradient: zenith=(%d,%d,%d) nadir=(%d,%d,%d)\n",
		       Sc->ZenithColor.Red, Sc->ZenithColor.Green, Sc->ZenithColor.Blue,
		       Sc->NadirColor.Red, Sc->NadirColor.Green, Sc->NadirColor.Blue);
	}
}

void ReadMaterial(FldMat *Mat)
{
	if (ReadMaterialNames)
	{
		ReadString(&Mat->Name);
#ifdef DebugMode
		FLDLOG("-----|Material name: \"%s\"\n",Mat->Name);
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
	Mat->TextureSize.Read(FLD);
	Mat->TextureCenter.Read(FLD);
	Mat->TextureFallOff.Read(FLD);
	Mat->TextureVelocity.Read(FLD);
	//fread(&Mat->TextureSize,1,sizeof(Vector),FLD);
	//fread(&Mat->TextureCenter,1,sizeof(Vector),FLD);
	//fread(&Mat->TextureFallOff,1,sizeof(Vector),FLD);
	//fread(&Mat->TextureVelocity,1,sizeof(Vector),FLD);
	ReadString(&Mat->TextureAlpha);
	fread(&Mat->NoiseFrequencies,1,2,FLD);
	fread(&Mat->TextureWrapX,1,2,FLD);
	fread(&Mat->TextureWrapY,1,2,FLD);
	fread(&Mat->AAStrength,1,4,FLD);
	fread(&Mat->Opacity,1,4,FLD);
	fread(&Mat->TFP0,1,4,FLD);
	fread(&Mat->TFP1,1,4,FLD);
	// Revival per-surface extension: conditional payload gated on Surf_RevExt
	// (0x8000) in the material FLAG word — u16 RevExtMask + only the authored
	// fields, ascending mask-bit order (matches tools/lwsread FLDSAVE.CPP). The
	// FldMat is not zero-initialized (new FldMat[] on a POD), so gate on the
	// disk-read Flags bit, not on RevExtMask, and read RevExtMask from disk only
	// when the bit is set. Legacy FLDs never carry the bit (LWOB FLAG tops out at
	// 1024), so this branch is provably inert for shipping content. Mapped to the
	// live Material in FLD_MAT.CPP AddMaterial. See docs/SIDECAR_MIGRATION_PLAN.md.
	if (Mat->Flags & Surf_RevExt)
	{
		fread(&Mat->RevExtMask,1,2,FLD);
		if (Mat->RevExtMask & 0x001) fread(&Mat->AoStrength,1,4,FLD);
		if (Mat->RevExtMask & 0x002) fread(&Mat->ParallaxScale,1,4,FLD);
		if (Mat->RevExtMask & 0x004) fread(&Mat->TintR,1,4,FLD);
		if (Mat->RevExtMask & 0x008) fread(&Mat->TintG,1,4,FLD);
		if (Mat->RevExtMask & 0x010) fread(&Mat->TintB,1,4,FLD);
		if (Mat->RevExtMask & 0x020) fread(&Mat->RefractIor,1,4,FLD);
		if (Mat->RevExtMask & 0x040) fread(&Mat->Refractive,1,1,FLD);
		if (Mat->RevExtMask & 0x080) fread(&Mat->EnvReflMode,1,1,FLD);
		if (Mat->RevExtMask & 0x100) fread(&Mat->EnvBakeRes,1,4,FLD);
		if (Mat->RevExtMask & 0x200) fread(&Mat->WaterProcMode,1,1,FLD);
		if (Mat->RevExtMask & 0x400) fread(&Mat->EnvDynamic,1,1,FLD);
		if (Mat->RevExtMask & 0x800) fread(&Mat->SpecMul,1,4,FLD);
		// One bit, three floats (X,Y,Z) — see the FLD_READ.H note. Read as
		// three separate 4-byte fields, never as one 12-byte struct read: the
		// wire format is positional, not a memory image.
		if (Mat->RevExtMask & 0x1000)
		{
			fread(&Mat->EnvBakeOfsX,1,4,FLD);
			fread(&Mat->EnvBakeOfsY,1,4,FLD);
			fread(&Mat->EnvBakeOfsZ,1,4,FLD);
		}
	}
	// Revival per-surface PBR map-SET extension: a SECOND conditional payload
	// gated on Surf_RevMaps (0x0800), written AFTER the RVSF numeric payload by
	// tools/lwsread FLDSAVE.CPP — u16 RevMapMask + set-name string (0x01) +
	// normalFlip byte (0x40), ascending mask-bit order. Directory-per-set: the
	// engine resolves TEXTURES/PBR/<set>/<role>.png. Independent of Surf_RevExt
	// (a surface can carry maps with no numeric dials, e.g. 'teleporter').
	// Legacy FLDs never carry the bit. Collected in FLD_MAT.CPP + applied at
	// scene init via MaterialImport_ApplyMapFile. See SIDECAR_MIGRATION_PLAN §1e.
	if (Mat->Flags & Surf_RevMaps)
	{
		fread(&Mat->RevMapMask,1,2,FLD);
		if (Mat->RevMapMask & 0x01) ReadString(&Mat->MapSet);
		if (Mat->RevMapMask & 0x40) fread(&Mat->NormalFlip,1,1,FLD);
	}
#ifdef DebugMode
	FLDLOG("---S---|Color texture = %s\n",Mat->ColorTexture);
	FLDLOG("---u---|Diffuse texture = %s\n",Mat->DiffuseTexture);
	FLDLOG("---r---|Specular texture = %s\n",Mat->SpecularTexture);
	FLDLOG("---f---|Reflection texture = %s\n",Mat->ReflectionTexture);
	FLDLOG("---a---|Trans texture = %s\n",Mat->TransparencyTexture);
	FLDLOG("---c---|Bump texture = %s\n",Mat->BumpTexture);
	FLDLOG("---e---|Texture image = %s\n",Mat->TextureImage);
#endif
}

void ReadObject(FldObject *Obj)
{
	uint32_t i,j;
	
#ifdef DebugMode
	FLDLOG("-|Loading object...\n");
#endif
	if (ReadObjectNames)
	{
		ReadString(&Obj->Name);
#ifdef DebugMode
		FLDLOG("---|Object name: \"%s\"\n",Obj->Name);
#endif
	}
	if (strstr(Obj->Name, "pas"))
	{
		int banana = 1;
	}
	fread(&Obj->Flags,1,4,FLD);
	fread(&Obj->NumOfMat,1,4,FLD);
#ifdef DebugMode
	FLDLOG("---|Loading %ul materials...\n",Obj->NumOfMat);
#endif
	if (Obj->NumOfMat) Obj->Material=new FldMat[Obj->NumOfMat];
	for (i=0;i<Obj->NumOfMat;i++) ReadMaterial(&Obj->Material[i]);
#ifdef DebugMode
	FLDLOG("---|Loading modeller info(faces/vertices)...\n");
#endif
	fread(&Obj->NumOfVerts,1,4,FLD);
	if (Obj->NumOfVerts) { Obj->Verts = (Vector *)_aligned_malloc(sizeof(Vector) * Obj->NumOfVerts, 16); }
	for (i=0;i<Obj->NumOfVerts;i++)
	{
		//fread(&Obj->Verts[i],1,sizeof(Vector),FLD);
		Obj->Verts[i].Read(FLD);
	}
	fread(&Obj->NumOfFaces,1,4,FLD);
	if (Obj->NumOfFaces) Obj->Faces=new FldFace[Obj->NumOfFaces];
	for (i=0;i<Obj->NumOfFaces;i++)
	{
		fread(&Obj->Faces[i].FaceVerts,1,2,FLD);
		Obj->Faces[i].Verts=new Word[Obj->Faces[i].FaceVerts];
		for (j=0;j<Obj->Faces[i].FaceVerts;j++)
			fread(&Obj->Faces[i].Verts[j],1,2,FLD);
		fread(&Obj->Faces[i].Surface,1,2,FLD);
	}
#ifdef DebugMode
	FLDLOG("---|Loading keyframer info...\n");
#endif
	fread(&Obj->Keys,1,4,FLD);
	Obj->KF=new FldKeyFrame[Obj->Keys];
#ifdef DebugMode
	FLDLOG("-----|Loading %ul keys...\n",Obj->Keys);
#endif
	for (i = 0; i < Obj->Keys; i++) { Obj->KF[i].Read(FLD); }//fread(&Obj->KF[i],1,sizeof(FldKeyFrame),FLD);
	if (Obj->Flags&1) fread(&Obj->Parent,1,4,FLD);
	if (Obj->Flags & 2) Obj->Pivot.Read(FLD);//fread(&Obj->Pivot,1,sizeof(Vector),FLD);
	if (Obj->Flags&4) Obj->PolygonSize=ReadEnvelope();
}

void ReadLight(FldLight *Light)
{
	uint32_t i;
	
#ifdef DebugMode
	FLDLOG("-|Loading light...\n");
#endif
	if (ReadLightNames)
	{
		ReadString(&Light->Name);
#ifdef DebugMode
		FLDLOG("---|Light name: \"%s\"\n",Light->Name);
#endif
	}
	fread(&Light->Flags,1,4,FLD);
#ifdef DebugMode
	FLDLOG("---|Loading keyframer info...\n");
#endif
	fread(&Light->Keys,1,4,FLD);
	Light->KF=new FldKeyFrame[Light->Keys];
	for (i = 0; i < Light->Keys; i++) {
		Light->KF[i].Read(FLD);
		//fread(&Light->KF[i], 1, sizeof(FldKeyFrame), FLD);
	}
	//	Light->Color=ReadEnvelope(); Beep
	fread(&Light->Color,1,sizeof(FldColor),FLD);
	Light->Intensity=ReadEnvelope();
	if (Light->Flags&1) fread(&Light->Parent,1,4,FLD);
	if (Light->Flags&2) fread(&Light->TargetObject,1,4,FLD);
	if (Light->Flags&4) Light->Falloff=ReadEnvelope();
	if (Light->Flags&8) Light->ConeAngle=ReadEnvelope();
	if (Light->Flags&16) Light->Range=ReadEnvelope(); //v0.114
	// Bit 2048 = Light_VolumetricCone (authored volumetric beam): one float,
	// the per-light beam gain (0 = unset → 1.0). Written conditionally by
	// tools/lwsread FLDSAVE.CPP — legacy FLD writers only ever set bits
	// ≤1024 (EndBehavior tops out the word), so this branch is provably
	// inert for legacy content (Light is memset(0) by the caller).
	if (Light->Flags&Light_VolumetricCone) fread(&Light->VolBeamGain,1,4,FLD);
	// Bit 4096 = Light_FlareScale (authored per-light flare-sprite size, LWS
	// "FdsFlareScale <f>"): one float, the sprite-size multiplier (0 = unset →
	// 1.0). Written conditionally AND LAST by tools/lwsread FLDSAVE.CPP (after
	// the VolBeamGain field), read in the same order here. Provably inert for
	// legacy content — no shipping FLD light carries 4096 (PathAlignment is
	// object-section in every scene; Light is memset(0) by the caller).
	if (Light->Flags&Light_FlareScale) fread(&Light->FlareScale,1,4,FLD);
}

void ReadCamera(FldCamera *Cam)
{
	int i;
	
#ifdef DebugMode
	FLDLOG("-|Loading Camera....\n");
#endif
	fread(&Cam->Flags,1,4,FLD);
#ifdef DebugMode
	FLDLOG("---|Loading keyframer info...\n");
#endif
	fread(&Cam->Keys,1,4,FLD);
	Cam->KF=new FldKeyFrame[Cam->Keys];
	for (i = 0; i < Cam->Keys; i++) { Cam->KF[i].Read(FLD); }//fread(&Cam->KF[i],1,sizeof(FldKeyFrame),FLD);
	Cam->zoomFactor=ReadEnvelope();
	if (Cam->Flags&1) fread(&Cam->Parent,1,4,FLD);
	if (Cam->Flags&2) fread(&Cam->TargetObject,1,4,FLD);
}

void OpenFLD(const char *FileName)
{
	if (!(FLD=fopen(FileName,"rb")))
	{
		//fclose(FLD);
		printf("Error! Unable to open Flood file\"%s\"!\n",FileName);
		exit(1);
	}
}

FldScene *ReadFLD(const char *FileName)
{
	FldObject *Obj;
	FldLight	*Lgt;
	FldScene  *Sc;
	unsigned short Chunk;
	
	FLDLOG("Loading FLD file: \"%s\" (version %.3f)...\n",FileName,FldVersion);
	
	OpenFLD(FileName);
	Sc=new FldScene;
	memset(Sc,0,sizeof(FldScene));
#ifdef DebugMode
	FLDLOG("Loading general scene data...\n");
#endif
	ReadGeneralSceneData(Sc);
#ifdef ReadDebugMode
	FLDLOG("Loading object information:\n");
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
	FLDLOG("Closing file...\n");
#endif
	fclose(FLD);
#ifdef DebugMode
	FLDLOG("FLD Reading complete!\n");
#endif
	
	return Sc;
}
