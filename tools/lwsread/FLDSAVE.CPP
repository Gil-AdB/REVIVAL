/*

		 Flood Demo System - Flood scene file saver
		 ------------------------------------------

*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "LWREAD.H"

float FldVersion=0.113f;

FILE 			*FLD;

#define SaveObjectNames
#define SaveMaterialNames
#define SaveLightNames

//Chunks
#define Chunk_Object					0x1000
#define Chunk_Light						0x2000
#define Chunk_Camera					0x3000

void SetChunk(unsigned short ID)
{
	fwrite(&ID,1,2,FLD);
}

void SaveEnvelope(FldEnv *Env)
{
	unsigned int a,b;

	fwrite(&Env->Keys,1,4,FLD);
	fwrite(&Env->Channels,1,4,FLD);
	fwrite(&Env->EndBehavior,1,4,FLD);
	for (a=0;a<Env->Keys;a++)
	{
		for (b=0;b<Env->Channels;b++)
			fwrite(&Env->Key[a].Channel[b],1,4,FLD);
		fwrite(&Env->Key[a].FrameNumber,1,4,FLD);
		fwrite(&Env->Key[a].LinearValue,1,4,FLD);
		fwrite(&Env->Key[a].Tension,1,4,FLD);
		fwrite(&Env->Key[a].Continuity,1,4,FLD);
		fwrite(&Env->Key[a].Bias,1,4,FLD);
	}
}

void SaveGeneralSceneData(FldScene *Scene)
{
	// Authored scene-wide env-reflection defaults (LWS "FdsSceneEnvRefl" /
	// "FdsSceneEnvBakeRes"): the scene header has no flags word, so the
	// extension bit rides the AmbientIntensity envelope's EndBehavior
	// (legacy values there top out at 1024 — bit 2048 is free, the same
	// idiom as Light_VolumetricCone below). Payload = two int32s appended
	// after the "OML" names triplet, written ONLY when authored, so FLDs
	// without the keywords stay byte-identical to the pre-extension format.
	if (Scene->EnvReflSceneMode || Scene->EnvBakeResScene)
		Scene->AmbientIntensity->EndBehavior |= Scene_EnvDefaults;
	// Authored backdrop gradient (LWS ZenithColor/SkyColor/GroundColor/
	// NadirColor): same scene-header extension idiom — the bit rides the
	// AmbientIntensity envelope's EndBehavior (4096 is free; 2048 is env-
	// defaults). Payload written below after the env-defaults int32s.
	if (Scene->SkyColorsSet)
		Scene->AmbientIntensity->EndBehavior |= Scene_SkyColors;
	fprintf(FLD,"Flood3D");
	fwrite(&FldVersion,1,4,FLD);
	fwrite(&Scene->FirstFrame,1,4,FLD);
	fwrite(&Scene->LastFrame,1,4,FLD);
	fwrite(&Scene->FrameStep,1,4,FLD);
	fwrite(&Scene->FramesPerSecond,1,4,FLD);
	SaveEnvelope(Scene->AmbientColor);
	SaveEnvelope(Scene->AmbientIntensity);
#ifdef SaveObjectNames
	fprintf(FLD,"O");
#else
	fprintf(FLD,"X");
#endif
#ifdef SaveMaterialNames
	fprintf(FLD,"M");
#else
	fprintf(FLD,"X");
#endif
#ifdef SaveLightNames
	fprintf(FLD,"L");
#else
	fprintf(FLD,"X");
#endif
	if (Scene->AmbientIntensity->EndBehavior & Scene_EnvDefaults)
	{
		fwrite(&Scene->EnvReflSceneMode,1,4,FLD);
		fwrite(&Scene->EnvBakeResScene,1,4,FLD);
	}
	// Backdrop gradient payload (4 FldColor triples) — after the env-defaults
	// payload so the read order (FLD_READ.CPP) is unambiguous; a scene can
	// carry neither, either, or both bits. FLDs without the bit are unchanged.
	if (Scene->AmbientIntensity->EndBehavior & Scene_SkyColors)
	{
		fwrite(&Scene->ZenithColor,1,sizeof(FldColor),FLD);
		fwrite(&Scene->SkyColor,1,sizeof(FldColor),FLD);
		fwrite(&Scene->GroundColor,1,sizeof(FldColor),FLD);
		fwrite(&Scene->NadirColor,1,sizeof(FldColor),FLD);
	}
}

void SaveString(char *Str)
{
	char Zero=0;
	if (!Str)
		fwrite(&Zero,1,1,FLD);
	else
		fwrite(Str,1,strlen(Str)+1,FLD);
}

void SaveMaterial(FldMat *Mat)
{
#ifdef SaveMaterialNames
	SaveString(Mat->Name);
#endif
	fwrite(&Mat->Color,1,sizeof(FldColor),FLD);
	fwrite(&Mat->Flags,1,2,FLD);
	fwrite(&Mat->Luminosity,1,4,FLD);
	fwrite(&Mat->Diffuse,1,4,FLD);
	fwrite(&Mat->Specular,1,4,FLD);
	fwrite(&Mat->Reflection,1,4,FLD);
	fwrite(&Mat->Transparency,1,4,FLD);
	fwrite(&Mat->Glossiness,1,2,FLD);
	fwrite(&Mat->ReflectionMode,1,2,FLD);
	SaveString(Mat->ReflectionImage);
	fwrite(&Mat->ReflectionSeamAngle,1,4,FLD);
	fwrite(&Mat->RefractiveIndex,1,4,FLD);
	fwrite(&Mat->EdgeTransparency,1,4,FLD);
	fwrite(&Mat->MaxSmoothingAngle,1,4,FLD);
	SaveString(Mat->ColorTexture);
	SaveString(Mat->DiffuseTexture);
	SaveString(Mat->SpecularTexture);
	SaveString(Mat->ReflectionTexture);
	SaveString(Mat->TransparencyTexture);
	SaveString(Mat->BumpTexture);
	SaveString(Mat->TextureImage);
	fwrite(&Mat->TextureFlags,1,2,FLD);
	fwrite(&Mat->TextureSize,1,sizeof(Vector),FLD);
	fwrite(&Mat->TextureCenter,1,sizeof(Vector),FLD);
	fwrite(&Mat->TextureFallOff,1,sizeof(Vector),FLD);
	fwrite(&Mat->TextureVelocity,1,sizeof(Vector),FLD);
	SaveString(Mat->TextureAlpha);
	fwrite(&Mat->NoiseFrequencies,1,2,FLD);
	fwrite(&Mat->TextureWrapX,1,2,FLD);
	fwrite(&Mat->TextureWrapY,1,2,FLD);
	fwrite(&Mat->AAStrength,1,4,FLD);
	fwrite(&Mat->Opacity,1,4,FLD);
	fwrite(&Mat->TFP0, 1, 4, FLD);
	fwrite(&Mat->TFP1, 1, 4, FLD);
	// Revival per-surface extension (LWS/LWO "RVSF" -> Surf_RevExt bit): a
	// conditional payload written ONLY when the material carries the bit —
	// u16 RevExtMask + only the authored fields, in ascending mask-bit order.
	// Materials without the bit stay byte-identical to the pre-extension format
	// (the read side in FDS/FLD/FLD_READ.CPP walks the same order). LWOREAD sets
	// Surf_RevExt on Flags when it parses an RVSF sub-chunk. See LWREAD.H.
	if (Mat->Flags & Surf_RevExt)
	{
		fwrite(&Mat->RevExtMask,1,2,FLD);
		if (Mat->RevExtMask & 0x001) fwrite(&Mat->AoStrength,1,4,FLD);
		if (Mat->RevExtMask & 0x002) fwrite(&Mat->ParallaxScale,1,4,FLD);
		if (Mat->RevExtMask & 0x004) fwrite(&Mat->TintR,1,4,FLD);
		if (Mat->RevExtMask & 0x008) fwrite(&Mat->TintG,1,4,FLD);
		if (Mat->RevExtMask & 0x010) fwrite(&Mat->TintB,1,4,FLD);
		if (Mat->RevExtMask & 0x020) fwrite(&Mat->RefractIor,1,4,FLD);
		if (Mat->RevExtMask & 0x040) fwrite(&Mat->Refractive,1,1,FLD);
		if (Mat->RevExtMask & 0x080) fwrite(&Mat->EnvReflMode,1,1,FLD);
		if (Mat->RevExtMask & 0x100) fwrite(&Mat->EnvBakeRes,1,4,FLD);
		if (Mat->RevExtMask & 0x200) fwrite(&Mat->WaterProcMode,1,1,FLD);
		if (Mat->RevExtMask & 0x400) fwrite(&Mat->EnvDynamic,1,1,FLD);   // ENVDYN A1
		if (Mat->RevExtMask & 0x800) fwrite(&Mat->SpecMul,1,4,FLD);      // spec response ×
		// 0x1000 = env-probe capture-point OFFSET: ONE bit carrying ONE 12-byte
		// payload of three floats in X/Y/Z order (not a bit per scalar like the
		// tint triple — one semantic vector, and three bits would nearly fill
		// the u16). Native endian, like every other field written here.
		if (Mat->RevExtMask & 0x1000)
		{
			fwrite(&Mat->EnvBakeOfsX,1,4,FLD);
			fwrite(&Mat->EnvBakeOfsY,1,4,FLD);
			fwrite(&Mat->EnvBakeOfsZ,1,4,FLD);
		}
	}
	// Revival per-surface PBR map-SET extension (LWO "RVSM" -> Surf_RevMaps
	// bit): a SECOND conditional payload written ONLY when the material carries
	// the bit, AFTER the RVSF numeric payload — u16 RevMapMask + only the
	// authored fields (set-name string bit 0x01, normalFlip byte bit 0x40) in
	// ascending mask-bit order. Directory-per-set: the engine resolves
	// TEXTURES/PBR/<set>/<role>.png. Materials without the bit stay byte-
	// identical. LWOREAD sets Surf_RevMaps when it parses RVSM. See §1e.
	if (Mat->Flags & Surf_RevMaps)
	{
		fwrite(&Mat->RevMapMask,1,2,FLD);
		if (Mat->RevMapMask & 0x01) SaveString(Mat->MapSet);
		if (Mat->RevMapMask & 0x40) fwrite(&Mat->NormalFlip,1,1,FLD);
	}
}

void SaveObject(FldObject *Obj)
{
unsigned int i,j;

	SetChunk(Chunk_Object);
#ifdef SaveDebugMode
	printf("-|Saving object: \"%s\"...\n",Obj->Name);
#endif
#ifdef SaveObjectNames
	fwrite(Obj->Name,1,strlen(Obj->Name)+1,FLD);
#endif
	fwrite(&Obj->Flags,1,4,FLD);
	fwrite(&Obj->NumOfMat,1,4,FLD);
#ifdef SaveDebugMode
	printf("---|Saving %i materials...\n",Obj->NumOfMat);
#endif
	for (i=0;i<Obj->NumOfMat;i++) SaveMaterial(&Obj->Material[i]);
#ifdef SaveDebugMode
	printf("---|Saving modeller info(faces/vertices)...\n");
#endif
	fwrite(&Obj->NumOfVerts,1,4,FLD);
	for (i=0;i<Obj->NumOfVerts;i++)
		fwrite(&Obj->Vertex[i],1,sizeof(Vector),FLD);
	fwrite(&Obj->NumOfFaces,1,4,FLD);
	for (i=0;i<Obj->NumOfFaces;i++)
	{
		fwrite(&Obj->Face[i].FaceVerts,1,2,FLD);
		for (j=0;j<Obj->Face[i].FaceVerts;j++)
			fwrite(&Obj->Face[i].Vertex[j],1,2,FLD);
		fwrite(&Obj->Face[i].Surface,1,2,FLD);
	}
#ifdef SaveDebugMode
	printf("---|Saving keyframer info...\n");
#endif
	fwrite(&Obj->Keys,1,4,FLD);
	for (i=0;i<Obj->Keys;i++) fwrite(&Obj->KF[i],1,sizeof(FldKeyFrame),FLD);
	if (Obj->Flags&1) fwrite(&Obj->Parent,1,4,FLD);
	if (Obj->Flags&2) fwrite(&Obj->PivotPoint,1,sizeof(Vector),FLD);
	if (Obj->Flags&4) SaveEnvelope(Obj->PolygonSize);
}

void SaveLight(FldLight *Light)
{
 unsigned int i,j;

	SetChunk(Chunk_Light);
#ifdef SaveDebugMode
	printf("-|Saving light: \"%s\"...\n",Light->Name);
#endif
#ifdef SaveLightNames
	fwrite(Light->Name,1,strlen(Light->Name)+1,FLD);
#endif
	fwrite(&Light->Flags,1,4,FLD);
#ifdef SaveDebugMode
	printf("---|Saving keyframer info...\n");
#endif
	fwrite(&Light->Keys,1,4,FLD);
	for (i=0;i<Light->Keys;i++) fwrite(&Light->KF[i],1,sizeof(FldKeyFrame),FLD);

	fwrite(&Light->Color, 1, sizeof(FldColor), FLD);
	//SaveEnvelope(Light->Color);

	SaveEnvelope(Light->Intensity);
	if (Light->Flags&1) fwrite(&Light->Parent,1,4,FLD);
	if (Light->Flags&2) fwrite(&Light->TargetObject,1,4,FLD);
	if (Light->Flags&4) SaveEnvelope(Light->Falloff);
	if (Light->Flags&8) SaveEnvelope(Light->ConeAngle);
	if (Light->Flags&16) SaveEnvelope(Light->Range);
	// Bit 2048 (Light_VolumetricCone, LWS "VolumetricLight 1"): authored
	// volumetric beam. Conditional payload = one float, the per-light beam
	// gain (LWS "VolumetricLightIntensity"; 0 = unset → engine 1.0). Same
	// backward-compatible extension mechanism as the conditional fields
	// above: FLDs whose lights don't carry the bit are byte-identical to
	// the pre-extension format, and old loaders never see the extra bytes
	// unless the bit is set. (512 was NOT free — EndBehavior bits 256/512/
	// 1024 share this Flags word; see Light_VolumetricCone in LWREAD.H.)
	if (Light->Flags&Light_VolumetricCone) fwrite(&Light->VolBeamGain,1,4,FLD);
	// Bit 4096 (Light_FlareScale, LWS "FdsFlareScale <f>"): authored flare-
	// sprite size multiplier. Conditional payload = one float. Written LAST,
	// after the VolBeamGain field, so the byte order is unambiguous and lights
	// without the bit stay byte-identical to the pre-extension format (a light
	// can carry neither, either, or both bits — the reader walks them in the
	// same order). Same 0 → 1.0 sentinel as VolBeamGain / Omni::FlareScale.
	if (Light->Flags&Light_FlareScale) fwrite(&Light->FlareScale,1,4,FLD);
}

void SaveCamera(FldCamera *Cam)
{
	unsigned int i;

	SetChunk(Chunk_Camera);
#ifdef SaveDebugMode
	printf("-|Saving Camera....\n");
#endif
	fwrite(&Cam->Flags,1,4,FLD);
#ifdef SaveDebugMode
	printf("---|Saving keyframer info...\n");
#endif
	fwrite(&Cam->Keys,1,4,FLD);
	for (i=0;i<Cam->Keys;i++) fwrite(&Cam->KF[i],1,sizeof(FldKeyFrame),FLD);
	SaveEnvelope(Cam->ZoomFactor);
	if (Cam->Flags&1) fwrite(&Cam->Parent,1,4,FLD);
	if (Cam->Flags&2) fwrite(&Cam->TargetObject,1,4,FLD);
}

void OpenFLD(char *FileName)
{
	if (!(FLD=fopen(FileName,"wb")))
	{
		fclose(FLD);
		printf("Error! Unable to create Flood file\"%s\"!\n",FileName);
		exit(1);
	}
}

void SaveFLD(FldScene *Scene,char *FileName)
{
FldObject *Obj;
FldLight	*Lgt;
FldCamera	*Cam;

	printf("Saving FLD file: \"%s\" (version %.3f)...\n",FileName,FldVersion);
#ifndef SaveObjectNames
	printf("Object names are not saved!\n");
#endif
#ifndef SaveMaterialNames
	printf("Material names are not saved!\n");
#endif
#ifndef SaveLightNames
	printf("Material names are not saved!\n");
#endif
	OpenFLD(FileName);
#ifdef SaveDebugMode
	printf("Saving general scene data...\n");
#endif
	SaveGeneralSceneData(Scene);
#ifdef SaveDebugMode
	printf("Saving object information:\n");
#endif
	for (Obj=Scene->Object;Obj;Obj=Obj->Next) SaveObject(Obj);
	for (Lgt=Scene->Light;Lgt;Lgt=Lgt->Next)  SaveLight(Lgt);
	SaveCamera(Scene->Camera);
#ifdef SaveDebugMode
	printf("Closing file...\n");
#endif
	fclose(FLD);
#ifdef SaveDebugMode
	printf("Convertion routine completed!\n");
#endif
}