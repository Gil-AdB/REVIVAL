/*
		 Flood Demo System - Flood scene material utility
		 -------------------------------------------------
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
//#define DebugMode

void AddMaterial(FldMat *OrgMat, Scene *SceneGroup)
{
	Material *Temp;
	char *Image;
	Texture *Tx;

	if (!OrgMat) return;

	if (!MatLib)
	{
		MatLib = getAlignedType<Material>(16);//(Material *)getAlignedBlock(sizeof(Material), 16);
		CurMat=MatLib;
	}
	else
	{
		CurMat->Next = getAlignedType<Material>(16); //(Material *)getAlignedBlock(sizeof(Material), 16);
		CurMat=CurMat->Next;
	}
//	memset(CurMat,0,sizeof(Material));
	CurMat->Name=OrgMat->Name;
	CurMat->Flags=0;
	CurMat->Diffuse=OrgMat->Diffuse*0.01f;
	CurMat->Specular=OrgMat->Specular*0.01f;
	CurMat->RelScene=SceneGroup;
	if (!(*OrgMat->TextureImage)) return;
	Image=new char[strlen(OrgMat->TextureImage) + 15];
	strcpy(Image,"Textures/");
	strcat(Image,OrgMat->TextureImage);
	for (Temp=MatLib;Temp!=CurMat;Temp=Temp->Next)
	{
		if (Temp->RelScene!=SceneGroup) continue;
		if (Temp->Txtr)
			if (!strcmp(Temp->Txtr->FileName,Image))
			{
				CurMat->Txtr=Temp->Txtr;
				break;
			}
	}
	if (Temp==CurMat)
	{
		Tx=new Texture;
//		memset(Tx,0,sizeof(Texture));
		Tx->FileName=_strdup(Image);
#ifdef DebugMode
		printf("Attempting to identify texture \"%s\"...\n",Image);
#endif
		Identify_Texture(Tx);
		if (!Tx->BPP)
		{
			printf("Error identifying texture \"%s\"!\n",Image);
			delete Tx;
			CurMat->Txtr=NULL;
		} else CurMat->Txtr = Tx;
	}

	// Copy information from material structs
	CurMat->TFlags = OrgMat->Flags;
	CurMat->BaseCol.R = OrgMat->Color.Red;
	CurMat->BaseCol.G = OrgMat->Color.Green;
	CurMat->BaseCol.B = OrgMat->Color.Blue;
	CurMat->Luminosity = OrgMat->Luminosity;
//	CurMat->Luminosity = 1.0f;
	CurMat->Reflection = OrgMat->Reflection;
	CurMat->Transparency = OrgMat->Transparency;
	CurMat->Glossiness = OrgMat->Glossiness;
	CurMat->ReflectionMode = OrgMat->ReflectionMode;
	CurMat->ReflectionImage = OrgMat->ReflectionImage;
	CurMat->ReflectionSeamAngle = OrgMat->ReflectionSeamAngle;
	CurMat->RefractiveIndex = OrgMat->RefractiveIndex;
	CurMat->EdgeTransparency = OrgMat->EdgeTransparency;
	CurMat->MaxSmoothingAngle = OrgMat->MaxSmoothingAngle;
	CurMat->ColorTexture = OrgMat->ColorTexture;
	CurMat->DiffuseTexture = OrgMat->DiffuseTexture;
	CurMat->SpecularTexture = OrgMat->SpecularTexture;
	CurMat->ReflectionTexture = OrgMat->ReflectionTexture;
	CurMat->TransparencyTexture = OrgMat->TransparencyTexture;
	CurMat->BumpTexture = OrgMat->BumpTexture;
	CurMat->TextureFlags = OrgMat->TextureFlags;
	CurMat->TextureSize = OrgMat->TextureSize;
	CurMat->TextureCenter = OrgMat->TextureCenter;
	CurMat->TextureFallOff = OrgMat->TextureFallOff;
	CurMat->TextureVelocity = OrgMat->TextureVelocity;
	CurMat->TextureAlpha = OrgMat->TextureAlpha;
	CurMat->NoiseFrequencies = OrgMat->NoiseFrequencies;
	CurMat->TextureWrapX = OrgMat->TextureWrapX;
	CurMat->TextureWrapY = OrgMat->TextureWrapY;
	CurMat->AAStrength = OrgMat->AAStrength;
	CurMat->Opacity = OrgMat->Opacity;
	if (CurMat->TFlags&Surf_Smoothing) CurMat->Flags |= Mat_RGBInterp;
	if (CurMat->TFlags&Surf_DoubleSided) CurMat->Flags |= Mat_TwoSided;
	if (CurMat->Transparency>0.0) CurMat->Flags |= Mat_Transparent;

	if (CurMat->TextureFlags&Texture_WorldCoords)
		printf("WARNING! World coordinates are currently unsupported!\n (%s)",CurMat->Name);

	delete Image;
}


// Cartesian Coordinates ==> Cylindrical Coordinates
void xyztoh(float x,float y,float z,float *h)
{
	if (x == 0.0f && z == 0.0f) *h = 0.0f;
	else
	{
		if (z == 0.0f)
			*h = (x < 0.0f) ? PI_D2 : -PI_D2;
		else if (z < 0.0f)
			*h = -atanf(x / z) + PI;
		else
			*h = -atanf(x / z);
	}
}

//Cartesian Coordinates ==> Spherical Coordinates
void xyztohp(float x,float y,float z,float *h,float *p)
{
	if (x == 0.0f && z == 0.0f)
	{
		*h = 0.0f;
		if (y != 0.0f)
			*p = (y < 0.0f) ? -PI_D2 : PI_D2;
		else
			*p = 0.0f;
	}
	else
	{
		if (z == 0.0f)
			*h = (x < 0.0f) ? PI_D2 : -PI_D2;
		else if (z < 0.0f)
			*h = -atanf(x / z) + PI;
		else
			*h = -atanf(x / z);
		x = sqrtf(x * x + z * z);
		if (x == 0.0f)
			*p = (y < 0.0f) ? -PI_D2 : PI_D2;
		else
			*p = atanf(y / x);
	}
}

float fract(float f)
{
/*	if (f >= 0.0 && f <= 1.0)
		return f*256.0;
	else
		return (f-floor(f))*256.0;*/
		return f;
}

void Get_UV(Face *F,Vertex *Vtx,FldMat *Mat)
{
	Vector W,U,V,N;
	float s,t,lon,lat;

	Vector_Sub(&Vtx->Pos,&Mat->TextureCenter,&V);

	if (!strcmp(Mat->ColorTexture,"Planar Image Map"))
	{
		s = (Mat->TextureFlags&Texture_XAxis) ? V.z / Mat->TextureSize.z + .5f :
			V.x / Mat->TextureSize.x + .5;
		t = (Mat->TextureFlags&Texture_YAxis) ? -V.z / Mat->TextureSize.z + .5f :
			-V.y / Mat->TextureSize.y + .5f;

		Vtx->U = fract(s);
		Vtx->V = fract(t);
#ifdef DebugMode
		printf("Mapping Coordinates (%f,%f) using Planar mapping...\n",Vtx->U,Vtx->V);
#endif
	}
	else if (!strcmp(Mat->ColorTexture,"Cylindrical Image Map"))
	{
		if (Mat->TextureFlags&Texture_XAxis) {
			xyztoh(V.z,V.x,-V.y,&lon);
			t = -V.x / Mat->TextureSize.x + .5f;
		}
		else if (Mat->TextureFlags&Texture_YAxis) {
			xyztoh(-V.x,V.y,V.z,&lon);
			t = -V.y / Mat->TextureSize.y + .5f;
		} else {
			xyztoh(-V.x,V.z,-V.y,&lon);
			t = -V.z / Mat->TextureSize.z + .5f;
		}
		lon = 1.0 - lon / PI_M2;
/*		if (Mat->TFP0 != 1.0)
			lon = fract(lon) * Mat->TFP0;*/
		Vtx->U = fract(lon);//(lon-floor(lon))*256.0;
		Vtx->V = fract(t);//(t-floor(t))*256.0;
#ifdef DebugMode
		printf("Mapping Coordinates (%f,%f) using Cylindrical mapping...\n",Vtx->U,Vtx->V);
#endif
	}
	else if (!strcmp(Mat->ColorTexture,"Spherical Image Map"))
	{
		if (Mat->TextureFlags&Texture_XAxis)
			xyztohp(V.z,V.x,-V.y,&lon,&lat);
		else if (Mat->TextureFlags&Texture_YAxis)
			xyztohp(-V.x,V.y,V.z,&lon,&lat);
		else
			xyztohp(-V.x,V.z,-V.y,&lon,&lat);
		lon = 1.0f - lon / PI_M2;
		lat = 0.5f - lat / PI;

/*		 if (Mat->TFP0 != 1.0)
			 lon = fract(lon) * Mat->TFP0;
		 if (Mat->TFP1 != 1.0)
			 lat = fract(lat) * Mat->TFP1;*/
		Vtx->U = fract(lon);//(lon-floor(lon))*256.0;
		Vtx->V = fract(lat);//(lat-floor(lat))*256.0;
#ifdef DebugMode
		printf("Mapping Coordinates (%f,%f) using Spherical mapping...\n",Vtx->U,Vtx->V);
#endif
	} else if (!strcmp(Mat->ColorTexture,"Cubic Image Map"))
	{
		Vector_Sub(&F->B->Pos,&F->A->Pos,&U);
		Vector_Sub(&F->C->Pos,&F->A->Pos,&W);
		Cross_Product(&U,&W,&N);
		N.x = fabsf(N.x); N.y = fabsf(N.y); N.z = fabsf(N.z);
		char XAxis=0,YAxis=0;

		if (N.x>N.y&&N.x>N.z) XAxis=1;
		else if (N.y>N.x&&N.y>N.z) YAxis=1;

		s = (XAxis) ?  V.z / Mat->TextureSize.z :  V.x / Mat->TextureSize.x;
		t = (YAxis) ? -V.z / Mat->TextureSize.z :	-V.y / Mat->TextureSize.y;

		s += .5f; t += .5f;

		Vtx->U = fract(s);
		Vtx->V = fract(t);

#ifdef DebugMode
		printf("Mapping Coordinates (%f,%f) using Spherical mapping...\n",Vtx->U,Vtx->V);
#endif
	}
/*	else if (Mat->ColorTexture[0]!=0)
		printf("Warning! Color texture \"%s\" is illegal!\n",Mat->ColorTexture);*/
}


void Get_Mapping(Face *F,FldMat *Mat)
{
	Get_UV(F,F->A,Mat);
	Get_UV(F,F->B,Mat);
	Get_UV(F,F->C,Mat);
	F->U1 = F->A->U;
	F->V1 = F->A->V;
	F->U2 = F->B->U;
	F->V2 = F->B->V;
	F->U3 = F->C->U;
	F->V3 = F->C->V;
}
