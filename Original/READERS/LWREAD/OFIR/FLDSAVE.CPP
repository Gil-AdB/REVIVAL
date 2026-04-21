/*

		 Flood Demo System - Flood scene file saver
		 ------------------------------------------

*/
float FldVersion=0.1;

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
int a,b;

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

}

void SaveMaterial(FldMat *Mat)
{
#ifdef SaveMaterialNames
	fwrite(Mat->Name,1,strlen(Mat->Name)+1,FLD);
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
	fwrite(Mat->ReflectionImage,1,strlen(Mat->ReflectionImage)+1,FLD);
	fwrite(&Mat->ReflectionSeamAngle,1,4,FLD);
	fwrite(&Mat->RefractiveIndex,1,4,FLD);
	fwrite(&Mat->EdgeTransparency,1,4,FLD);
	fwrite(&Mat->MaxSmoothingAngle,1,4,FLD);
	fwrite(Mat->ColorTexture,1,strlen(Mat->ColorTexture)+1,FLD);
	fwrite(Mat->DiffuseTexture,1,strlen(Mat->DiffuseTexture)+1,FLD);
	fwrite(Mat->SpecularTexture,1,strlen(Mat->SpecularTexture)+1,FLD);
	fwrite(Mat->ReflectionTexture,1,strlen(Mat->ReflectionTexture)+1,FLD);
	fwrite(Mat->TransparencyTexture,1,strlen(Mat->TransparencyTexture)+1,FLD);
	fwrite(Mat->BumpTexture,1,strlen(Mat->BumpTexture)+1,FLD);
	fwrite(Mat->TextureImage,1,strlen(Mat->TextureImage)+1,FLD);
	fwrite(&Mat->TextureFlags,1,2,FLD);
	fwrite(&Mat->TextureSize,1,sizeof(Vector),FLD);
	fwrite(&Mat->TextureCenter,1,sizeof(Vector),FLD);
	fwrite(&Mat->TextureFallOff,1,sizeof(Vector),FLD);
	fwrite(&Mat->TextureVelocity,1,sizeof(Vector),FLD);
	fwrite(Mat->TextureAlpha,1,strlen(Mat->TextureAlpha)+1,FLD);
	fwrite(&Mat->NoiseFrequencies,1,2,FLD);
	fwrite(&Mat->TextureWrapOptions,1,2,FLD);
	fwrite(&Mat->AAStrength,1,4,FLD);
	fwrite(&Mat->Opacity,1,4,FLD);
}

void SaveObject(FldObject *Obj)
{
int i,j;

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
int i,j;

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
	SaveEnvelope(Light->Color);
	SaveEnvelope(Light->Intensity);
	if (Light->Flags&1) fwrite(&Light->Parent,1,4,FLD);
	if (Light->Flags&2) fwrite(&Light->TargetObject,1,4,FLD);
	if (Light->Flags&4) SaveEnvelope(Light->Falloff);
	if (Light->Flags&8) SaveEnvelope(Light->ConeAngle);
}

void SaveCamera(FldCamera *Cam)
{
int i,j;

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