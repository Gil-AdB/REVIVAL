/*

		 Flood Demo System - LightWave object reader, by Ofir Ehrlich
		 -------------------------------------------------------------

	The reader does NOT support detail polygons and curves, since these are
	not relevant for real-time rendering.


	LightWave-->FLOOD file convertor.

	NOTE!!! Surface==Material
*/

#pragma pack (1)		// No data alligning

#define Version 0.8
#define DebugMode

#include <Mem.H>
#include <StdLib.H>
#include <DOS.H>
#include <StdIO.H>

enum LightWave_Constants
/* These flags are not used in the LWO->FLD convertor, but I have decided to
	 list them here, so one can understand the code better */
{
// Material flags
	Surf_Luminous   		 	  = 1,
	Surf_Outline    		 	  = 2,
	Surf_Smoothing  		 	  = 4,
	Surf_ColorHighlights 	  = 8,
	Surf_ColorFilter		 	  = 16,
	Surf_OpaqueEdge			 	  = 32,
	Surf_TransparentEdge 	  = 64,
	Surf_SharpTerminator 	  = 128,
	Surf_DoubleSided		 	  = 256,
	Surf_Additive				 	  = 512,
	Surf_ShadowAlpha			  = 1024,
// Texture flags
	Texture_XAxis					  = 1,
	Texture_YAxis           = 2,
	Texture_ZAxis           = 4,
	Texture_WorldCoords     = 8,
	Texture_NegativeImage   = 16,
	Texture_PixelBlending   = 32,
	Texture_Antialiasing    = 64,
// Glossiness values
	Gloss_Low							  = 16,
	Gloss_Medium					  = 64,
	Gloss_High						  = 256,
	Gloss_Max							  = 1024,
// Reflection modes
	RefMode_BackdropOnly	  = 0,
	RefMode_RaytraceObjects = 1,
	RefMode_WrapImage				= 2,
	RefMode_WrapImgRayTrace	= 3
};

typedef struct
{
	float X,Y,Z;
} Vector;

typedef struct
{
	unsigned char Red,Green,Blue;
} FldColor;

typedef struct
{
	unsigned short  FaceVerts;
	unsigned short *Vertex;
	signed   short  Surface;
} FldFace;

typedef struct
{
	char 					*Name;
	FldColor			 Color;
	unsigned short Flags; 		   		// See "enum" definition for above.
	float          Luminosity;
	float          Diffuse;
	float          Specular;
	float          Reflection;
	float          Transparency;
	unsigned short Glossiness;   		// See "enum" definition for above.
	unsigned short ReflectionMode;
	char					*ReflectionImage; // See "enum" definition for above.
	float					 ReflectionSeamAngle;
	float					 RefractiveIndex;
	float					 EdgeTransparency;
	float					 MaxSmoothingAngle;
	char					*ColorTexture;
	char					*DiffuseTexture;
	char					*SpecularTexture;
	char					*ReflectionTexture;
	char					*TransparencyTexture;
	char					*BumpTexture;
	char					*TextureImage;
	unsigned short TextureFlags; 		// See "enum" definition for above.
	Vector 				 TextureSize;
	Vector 				 TextureCenter;
	Vector 				 TextureFallOff;
	Vector 				 TextureVelocity;
/*FldColor			 TextureColor;
	float					 TextureAmplitude;*/
	char					*TextureAlpha;
	unsigned short NoiseFrequencies;
	unsigned short TextureWrapOptions;
	float					 AAStrength;
	float					 Opacity;
} FldMat;

typedef struct FldObj
{
	unsigned long  NumOfMat;
	FldMat    	  *Material;    // Note!! Surface count starts with 1
	unsigned long  NumOfVerts;
	Vector        *Vertex;      // Note!! Vertex count starts with 0
	unsigned long  NumOfFaces;
	FldFace       *Face;
	FldObj   			*Next;
} FldObject;

typedef struct
{
	char ID[4];
	unsigned long Length;
} ChunkType;

unsigned int  CurLength;
FILE         *LWO;
FldObject		 *Obj;
FldMat			 *Surf; // Current surface
unsigned char SubChunk;

void ReadChunk(unsigned int ChunkEnd); // Forward declaration
FldMat *FindMat(char *Name); // Yet another forward declaration

void SwapDW(void *);  // Swap DWORD: 0x12345678 --> 0x78563412
#pragma aux SwapDW=\
"mov eax,[ebx]"\
"bswap eax"\
"mov [ebx],eax"\
parm [ebx]\
modify exact [eax];

void SwapW(void *);  // Swap WORD:   0x1234     --> 0x3412
#pragma aux SwapW=\
"Rol word ptr [ebx],8"\
parm [ebx]\
modify exact [];

void ReadAsciiZ(char **s)
{
	int a=0;
	char Temp[256];

	do fread(&Temp[a++],1,1,LWO); while (Temp[a-1]);
	if (ftell(LWO)&0x00000001) fseek(LWO,1,SEEK_CUR);
	*s=strdup(Temp);
}

void ReadSignature(unsigned int EndPos)
{
const char *ID="LWOB";
char Test[5];

	fread(&Test,1,4,LWO);
	Test[4]=0;
	if (strcmp(ID,Test)!=0)
	{
		printf("Error! Bad object file!\n");
		fclose(LWO);
		exit(1);
	}
	ReadChunk(EndPos);
}

void ReadFaceList(unsigned int EndPos)
{
typedef struct _TempFace_
{
	unsigned short  FaceVerts;
	unsigned short *Vertex;
	signed   short  Surface;
	_TempFace_ 		 *Next;
} TempFace;

TempFace *T,*THead=NULL;

unsigned int a,i;
unsigned short FaceVerts,Vert,SurfaceNum;
unsigned int NumOfFaces=0;

#ifdef DebugMode
	printf("Face list:\n");
#endif
	while (ftell(LWO)<EndPos)
	{
		if (!THead)
		{
			THead=new TempFace;
			T=THead;
		}
		else
		{
			T->Next=new TempFace;
			T=T->Next;
		}
		T->Next=NULL;
		fread(&FaceVerts,1,2,LWO);
		SwapW(&FaceVerts);
#ifdef DebugMode
		printf("Face %i has %i vertices:",NumOfFaces,FaceVerts);
#endif
		T->FaceVerts=FaceVerts;
		T->Vertex=new short[FaceVerts];
		for (a=0;a<FaceVerts;a++)
		{
			fread(&Vert,1,2,LWO);
			SwapW(&Vert);
			T->Vertex[a]=Vert;
#ifdef DebugMode
			printf("%i ",Vert);
#endif
		}
		NumOfFaces++;
		fread(&SurfaceNum,1,2,LWO);
		SwapW(&SurfaceNum);
		SurfaceNum--; // We want the count to start from 0 and not 1
		T->Surface=SurfaceNum;
#ifdef DebugMode
		printf("| Surface: %i\n",SurfaceNum);
#endif
		if (SurfaceNum<0)
		{
			printf("Error! Detail polygons are not supported!\n");
			fclose(LWO);
			exit(2);
		}
	}
	Obj->NumOfFaces=NumOfFaces;
	Obj->Face=new FldFace[NumOfFaces];
	i=0;
	for (T=THead;T;T=T->Next,i++)
	{
		Obj->Face[i].FaceVerts=T->FaceVerts;
		Obj->Face[i].Vertex=T->Vertex;
		Obj->Face[i].Surface=T->Surface;
	}
#ifdef DebugMode
	printf("Total faces: %i\n",NumOfFaces);
#endif
}

void ReadVertList(unsigned int EndPos)
{
int a;
Vector V;
unsigned int NumOfVerts=CurLength/12;

#ifdef DebugMode
	printf("Vertex list:\n");
	printf("Number of vertices: %i\n",NumOfVerts);
#endif
	Obj->NumOfVerts=NumOfVerts;
	Obj->Vertex=new Vector[NumOfVerts];
	for (a=0;a<NumOfVerts;a++)
	{
		fread(&V,1,sizeof(Vector),LWO);
		SwapDW(&V.X);
		SwapDW(&V.Y);
		SwapDW(&V.Z);
		Obj->Vertex[a]=V;
#ifdef DebugMode
		printf("Vertex %i: X:%f  Y:%f  Z:%f\n",a,V.X,V.Y,V.Z);
#endif
	}
}

void ReadSurfaceList(unsigned int EndPos)
{
typedef struct _TempMat_
{
	char 			*Name;
	_TempMat_ *Next;
} TempMat;

TempMat *T,*THead;
int 		 i=0;

#ifdef DebuMode
	printf("Surface list:\n");
#endif
	Obj->NumOfMat=1;
	THead=new TempMat;
	T=THead;
	T->Next=NULL;
	ReadAsciiZ(&T->Name);
#ifdef DebugMode
		printf("Surface #%i: %s\n",Obj->NumOfMat,T->Name);
#endif
	while (ftell(LWO)<EndPos)
	{
		Obj->NumOfMat++;
		T->Next=new TempMat;
		T=T->Next;
		T->Next=NULL;
		ReadAsciiZ(&T->Name);
#ifdef DebugMode
		printf("Surface #%i: %s\n",Obj->NumOfMat,T->Name);
#endif
	}
	Obj->Material=new FldMat[Obj->NumOfMat];//(FldMat *)malloc(Obj->NumOfMat);
	memset(Obj->Material,0,sizeof(FldMat)*Obj->NumOfMat);
	for (T=THead;T;T=T->Next,i++)
		Obj->Material[i].Name=T->Name;
}

void ReadSurfaceData(unsigned int EndPos)
{
char *Surface;

	ReadAsciiZ(&Surface);
#ifdef DebugMode
	printf("Detailed data for surface \"%s\":\n",Surface);
#endif
	Surf=FindMat(Surface);
// Read sub-chunks:
	SubChunk=1;
	ReadChunk(EndPos);
	SubChunk=0;
}

void ReadSurfaceColor(unsigned int EndPos)
{
char Dummy;

	fread(&Surf->Color,1,sizeof(FldColor),LWO);
	fread(&Dummy,1,1,LWO);
#ifdef DebugMode
	printf("  Color (Red/Green/Blue): %i/%i/%i\n",
				 Surf->Color.Red,Surf->Color.Green,Surf->Color.Blue);
#endif
}

void ReadSurfaceFlags(unsigned int EndPos)
{
	fread(&Surf->Flags,1,2,LWO);
	SwapW(&Surf->Flags);
#ifdef DebugMode
	printf("  Flags: %Xh\n",Surf->Flags);
#endif
}

void ReadSurfaceLuminosityInt(unsigned int EndPos)
{
char Temp;

	fread(&Temp,1,1,LWO);
	Surf->Luminosity=Temp/2.55;
#ifdef DebugMode
	printf("  Luminosity(int): %3.2f%%\n",Surf->Luminosity);
#endif
}

void ReadSurfaceDiffuseInt(unsigned int EndPos)
{
char Temp;

	fread(&Temp,1,1,LWO);
	Surf->Diffuse=Temp/2.55;
#ifdef DebugMode
	printf("  Diffuse(int): %3.2f%%\n",Surf->Diffuse);
#endif
}

void ReadSurfaceSpecularInt(unsigned int EndPos)
{
char Temp;

	fread(&Temp,1,1,LWO);
	Surf->Specular=Temp/2.55;
#ifdef DebugMode
	printf("  Specular(int): %3.2f%%\n",Surf->Specular);
#endif
}

void ReadSurfaceReflectionInt(unsigned int EndPos)
{
char Temp;

	fread(&Temp,1,1,LWO);
	Surf->Reflection=Temp/2.55;
#ifdef DebugMode
	printf("  Reflection(int): %3.2f%%\n",Surf->Reflection);
#endif
}

void ReadSurfaceTransparencyInt(unsigned int EndPos)
{
char Temp;

	fread(&Temp,1,1,LWO);
	Surf->Transparency=Temp/2.55;
#ifdef DebugMode
	printf("  Transparency(int): %3.2f%%\n",Surf->Transparency);
#endif
}

void ReadSurfaceLuminosityFloat(unsigned int EndPos)
{
float Temp;

	fread(&Temp,1,4,LWO);
	SwapDW(&Temp);
	Surf->Luminosity=Temp*100.0;
#ifdef DebugMode
	printf("  Luminosity(float): %3.2f%%\n",Surf->Luminosity);
#endif
}

void ReadSurfaceDiffuseFloat(unsigned int EndPos)
{
float Temp;

	fread(&Temp,1,4,LWO);
	SwapDW(&Temp);
	Surf->Diffuse=Temp*100.0;
#ifdef DebugMode
	printf("  Diffuse(float): %3.2f%%\n",Surf->Diffuse);
#endif
}

void ReadSurfaceSpecularFloat(unsigned int EndPos)
{
float Temp;

	fread(&Temp,1,4,LWO);
	SwapDW(&Temp);
	Surf->Specular=Temp*100.0;
#ifdef DebugMode
	printf("  Specular(float): %3.2f%%\n",Surf->Specular);
#endif
}

void ReadSurfaceReflectionFloat(unsigned int EndPos)
{
float Temp;

	fread(&Temp,1,4,LWO);
	SwapDW(&Temp);
	Surf->Reflection=Temp*100.0;
#ifdef DebugMode
	printf("  Reflection(float): %3.2f%%\n",Surf->Reflection);
#endif
}

void ReadSurfaceTransparencyFloat(unsigned int EndPos)
{
float Temp;

	fread(&Temp,1,4,LWO);
	SwapDW(&Temp);
	Surf->Transparency=Temp*100.0;
#ifdef DebugMode
	printf("  Transparency(float): %3.2f%%\n",Surf->Transparency);
#endif
}

void ReadSurfaceGlossiness(unsigned int EndPos)
{
	fread(&Surf->Glossiness,1,2,LWO);
	SwapW(&Surf->Glossiness);
#ifdef DebugMode
	printf("	Glossiness: %i\n",Surf->Glossiness);
#endif
}

void ReadSurfaceReflectionMode(unsigned int EndPos)
{
	fread(&Surf->ReflectionMode,1,2,LWO);
	SwapW(&Surf->ReflectionMode);
#ifdef DebugMode
	printf("	Reflection mode: %i\n",Surf->ReflectionMode);
#endif
}

void ReadSurfaceReflectionImage(unsigned int EndPos)
{
	ReadAsciiZ(&Surf->ReflectionImage);
#ifdef DebugMode
	printf("	Reflection image: \"%s\"\n",Surf->ReflectionImage);
#endif
}

void ReadSurfaceReflectionSeamAngle(unsigned int EndPos)
{
	fread(&Surf->ReflectionSeamAngle,1,4,LWO);
	SwapDW(&Surf->ReflectionSeamAngle);
#ifdef DebugMode
	printf("	Reflection seam angle: %3.2f\n",Surf->ReflectionSeamAngle);
#endif
}

void ReadSurfaceRefractiveIndex(unsigned int EndPos)
{
	fread(&Surf->RefractiveIndex,1,4,LWO);
	SwapDW(&Surf->RefractiveIndex);
#ifdef DebugMode
	printf("	Refractive index: %3.2f\n",Surf->ReflectionSeamAngle);
#endif
}

void ReadSurfaceEdgeTransparency(unsigned int EndPos)
{
	fread(&Surf->EdgeTransparency,1,4,LWO);
	SwapDW(&Surf->EdgeTransparency);
#ifdef DebugMode
	printf("	Edge transparency: %3.2f\n",Surf->EdgeTransparency);
#endif
}

void ReadSurfaceMaxSmoothingAngle(unsigned int EndPos)
{
	fread(&Surf->MaxSmoothingAngle,1,4,LWO);
	SwapDW(&Surf->MaxSmoothingAngle);
#ifdef DebugMode
	printf("	Maximum smoothing angle: %3.2f\n",Surf->MaxSmoothingAngle);
#endif
}

void ReadSurfaceTextureImage(unsigned int EndPos)
{
	ReadAsciiZ(&Surf->TextureImage);
#ifdef DebugMode
	printf("	Texture image: \"%s\"\n",Surf->TextureImage);
#endif
}

void ReadSurfaceTextureFlags(unsigned int EndPos)
{
	fread(&Surf->TextureFlags,1,2,LWO);
	SwapW(&Surf->TextureFlags);
#ifdef DebugMode
	printf("  TextureFlags: %Xh\n",Surf->TextureFlags);
#endif
}

void ReadSurfaceTextureSize(unsigned int EndPos)
{
	fread(&Surf->TextureSize,1,12,LWO);
	SwapDW(&Surf->TextureSize.X);
	SwapDW(&Surf->TextureSize.Y);
	SwapDW(&Surf->TextureSize.Z);
#ifdef DebugMode
	printf("  Texture size: X:%7.3f Y:%7.3f Z:%7.3f\n",
				 Surf->TextureSize.X,Surf->TextureSize.Y,Surf->TextureSize.Z);
#endif
}

void ReadSurfaceTextureCenter(unsigned int EndPos)
{
	fread(&Surf->TextureCenter,1,12,LWO);
	SwapDW(&Surf->TextureCenter.X);
	SwapDW(&Surf->TextureCenter.Y);
	SwapDW(&Surf->TextureCenter.Z);
#ifdef DebugMode
	printf("  Texture center: X:%7.3f Y:%7.3f Z:%7.3f\n",
				 Surf->TextureCenter.X,Surf->TextureCenter.Y,Surf->TextureCenter.Z);
#endif
}

void ReadSurfaceTextureFallOff(unsigned int EndPos)
{
	fread(&Surf->TextureFallOff,1,12,LWO);
	SwapDW(&Surf->TextureFallOff.X);
	SwapDW(&Surf->TextureFallOff.Y);
	SwapDW(&Surf->TextureFallOff.Z);
#ifdef DebugMode
	printf("  Texture falloff: X:%7.3f Y:%7.3f Z:%7.3f\n",
				 Surf->TextureFallOff.X,Surf->TextureFallOff.Y,Surf->TextureFallOff.Z);
#endif
}

void ReadSurfaceTextureVelocity(unsigned int EndPos)
{
	fread(&Surf->TextureVelocity,1,12,LWO);
	SwapDW(&Surf->TextureVelocity.X);
	SwapDW(&Surf->TextureVelocity.Y);
	SwapDW(&Surf->TextureVelocity.Z);
#ifdef DebugMode
	printf("  Texture velocity: X:%7.3f Y:%7.3f Z:%7.3f\n",
				 Surf->TextureVelocity.X,Surf->TextureVelocity.Y,Surf->TextureVelocity.Z);
#endif
}

void ReadSurfaceNoiseFrequencies(unsigned int EndPos)
{
	fread(&Surf->NoiseFrequencies,1,2,LWO);
	SwapW(&Surf->NoiseFrequencies);
#ifdef DebugMode
	printf("  Noise frequencies: %i\n",Surf->NoiseFrequencies);
#endif
}

void ReadSurfaceTextureAlpha(unsigned int EndPos)
{
	ReadAsciiZ(&Surf->TextureAlpha);
#ifdef DebugMode
	printf("	Alpha image: \"%s\"\n",Surf->TextureAlpha);
#endif
}

void ReadSurfaceImageWrapOptions(unsigned int EndPos)
{
unsigned short Temp;
	fread(&Temp,1,2,LWO);
	SwapW(&Temp);
	Surf->TextureWrapOptions=Temp;
	fread(&Temp,1,2,LWO);
	SwapW(&Temp);
	Surf->TextureWrapOptions+=Temp<<2;
#ifdef DebugMode
	printf("	Wrapping options: %Xh\n",Surf->TextureWrapOptions);
#endif
}

void ReadSurfaceAntialiasingStrength(unsigned int EndPos)
{
	fread(&Surf->AAStrength,1,4,LWO);
	SwapDW(&Surf->AAStrength);
	Surf->AAStrength*=100.0;
#ifdef DebugMode
	printf("	Antialiasing strength: %3.2f%%\n",Surf->AAStrength);
#endif
}

void ReadSurfaceTextureOpacity(unsigned int EndPos)
{
	fread(&Surf->Opacity,1,4,LWO);
	SwapDW(&Surf->Opacity);
	Surf->Opacity*=100.0;
	#ifdef DebugMode
	printf("	Texture opacity: %3.2f%%\n",Surf->Opacity);
#endif
}

void ReadSurfaceColorTexture(unsigned int EndPos)
{
	ReadAsciiZ(&Surf->ColorTexture);
#ifdef DebugMode
	printf("	Color texture: \"%s\"\n",Surf->ColorTexture);
#endif
}

void ReadSurfaceDiffuseTexture(unsigned int EndPos)
{
	ReadAsciiZ(&Surf->DiffuseTexture);
#ifdef DebugMode
	printf("	Diffuse texture: \"%s\"\n",Surf->DiffuseTexture);
#endif
}

void ReadSurfaceSpecularTexture(unsigned int EndPos)
{
	ReadAsciiZ(&Surf->SpecularTexture);
#ifdef DebugMode
	printf("	Specular texture: \"%s\"\n",Surf->SpecularTexture);
#endif
}

void ReadSurfaceTransparencyTexture(unsigned int EndPos)
{
	ReadAsciiZ(&Surf->TransparencyTexture);
#ifdef DebugMode
	printf("	Transparency texture: \"%s\"\n",Surf->TransparencyTexture);
#endif
}

void ReadSurfaceReflectionTexture(unsigned int EndPos)
{
	ReadAsciiZ(&Surf->ReflectionTexture);
#ifdef DebugMode
	printf("	Reflection texture: \"%s\"\n",Surf->ReflectionTexture);
#endif
}

void ReadSurfaceBumpTexture(unsigned int EndPos)
{
	ReadAsciiZ(&Surf->BumpTexture);
#ifdef DebugMode
	printf("	Bump texture: \"%s\"\n",Surf->BumpTexture);
#endif
}

struct
{
	const char *ID;
	void (*Reader)(unsigned int EndPos);
} ChunkData[]=
{
	{"FORM" , ReadSignature},
	{"PNTS"	, ReadVertList},
	{"POLS" , ReadFaceList},
	{"SRFS" , ReadSurfaceList},
/*{"CRVS" , ReadCurveList},						 			// Unsupported
	{"PCHS"	,	ReadPatchList},*/ 							// Unsupported
	{"SURF" , ReadSurfaceData},
	{"COLR" , ReadSurfaceColor},
	{"FLAG" , ReadSurfaceFlags},
	{"LUMI" , ReadSurfaceLuminosityInt},
	{"DIFF" , ReadSurfaceDiffuseInt},
	{"SPEC" , ReadSurfaceSpecularInt},
	{"REFL" , ReadSurfaceReflectionInt},
	{"TRAN" , ReadSurfaceTransparencyInt},
	{"VLUM"	,	ReadSurfaceLuminosityFloat},
	{"VDIF" , ReadSurfaceDiffuseFloat},
	{"VSPC" , ReadSurfaceSpecularFloat},
	{"VRFL" , ReadSurfaceReflectionFloat},
	{"VTRN" , ReadSurfaceTransparencyFloat},
	{"GLOS" , ReadSurfaceGlossiness},
	{"RFLT" , ReadSurfaceReflectionMode},
	{"RIMG" , ReadSurfaceReflectionImage},
	{"RSAN" , ReadSurfaceReflectionSeamAngle},
	{"RIND" , ReadSurfaceRefractiveIndex},
	{"EDGE" , ReadSurfaceEdgeTransparency},
	{"SMAN" , ReadSurfaceMaxSmoothingAngle},

	{"CTEX" , ReadSurfaceColorTexture},
	{"DTEX" , ReadSurfaceDiffuseTexture},
	{"STEX" , ReadSurfaceSpecularTexture},
	{"RTEX" , ReadSurfaceReflectionTexture},
	{"TTEX" , ReadSurfaceTransparencyTexture},
	{"BTEX" , ReadSurfaceBumpTexture},

	{"TIMG" , ReadSurfaceTextureImage},
	{"TFLG" , ReadSurfaceTextureFlags},
	{"TSIZ" , ReadSurfaceTextureSize},
	{"TCTR" , ReadSurfaceTextureCenter},
	{"TFAL" , ReadSurfaceTextureFallOff},
	{"TVEL" , ReadSurfaceTextureVelocity},
/*{"TCLR" , ReadSurfaceTextureColor}, 	      // Unsupported
	{"TVAL" , ReadSurfaceTextureValue},	       	// Unsupported
	{"TAMP" , ReadSurfaceTextureAmplitude},*/   // Unsupported
	{"TFRQ" , ReadSurfaceNoiseFrequencies},
	{"TALP"	,	ReadSurfaceTextureAlpha},
	{"TWRP"	,	ReadSurfaceImageWrapOptions},
	{"TAAS"	,	ReadSurfaceAntialiasingStrength},
	{"TOPC"	,	ReadSurfaceTextureOpacity}
};

FldMat *FindMat(char *Name)
{
int i;
FldMat *Temp;
	for(i=0;i<Obj->NumOfMat;i++)
		if (strcmp(Obj->Material[i].Name,Name)==0)
			return &Obj->Material[i]; // There might be an error here!

	printf("Warning! Material not found (object file structure error!)\n");
	printf("Using temporary material instead...\n");
	Temp=new FldMat;
	memset(Temp,0,sizeof(FldMat));
	Temp->Name=Name;
	return Temp;

}

signed char FindChunk(char *ID)
{
char Temp[5];

	strcpy(Temp,ID);
	Temp[4]=0;
	for (int a=0;a<sizeof(ChunkData)/sizeof(ChunkData[0]);a++)
		if (strcmp(ChunkData[a].ID,Temp)==0)
			return a;
	return -1;
}

void ReadChunk(unsigned int ChunkEnd)
{
unsigned int Pos;
short CurChunk;
ChunkType Chunk;

	while (ftell(LWO)<ChunkEnd)
	{
		if (SubChunk)
		{
			Chunk.Length=0;
			fread(&Chunk,1,sizeof(ChunkType)-2,LWO);
			SwapW(&Chunk.Length);
		}
		else
		{
			fread(&Chunk,1,sizeof(ChunkType),LWO);
			SwapDW(&Chunk.Length);
		}
		CurLength=Chunk.Length;
		Pos=ftell(LWO);
		if (Chunk.Length==0) return;
		CurChunk=FindChunk(Chunk.ID);
		if (CurChunk>=0)
		{
			Pos=Pos+Chunk.Length;
			if (ChunkData[CurChunk].Reader)
					 ChunkData[CurChunk].Reader(Pos);
			else ReadChunk(Pos);
			fseek(LWO,(Pos+1)&0xFFFFFFFE,SEEK_SET);
		}
		else fseek(LWO,Pos+((Chunk.Length+1)&0xFFFFFFFE),SEEK_SET);
	}
}

void ReadLWObject(char *FileName,FldObject *O)
{
unsigned int FileSize;

	if (!(LWO=fopen(FileName,"rb")))
	{
		fclose(LWO);
		printf("Error! Unable to open file \"%s\"!\n",FileName);
		exit(1);
	}
	SubChunk=0;
	Obj=O;
	memset(Obj,0,sizeof(FldObject));
	fseek(LWO,0,SEEK_END);
	FileSize=ftell(LWO);
	fseek(LWO,0,SEEK_SET);
	ReadChunk(FileSize);
	fclose(LWO);
}

void main(int argc,char *argv[])
{
FldObject *O;

	O=new FldObject;
	if (argc>1) ReadLWObject(argv[1],O);
	else printf("Syntax error! Type LWORead <Object file>\n");
}