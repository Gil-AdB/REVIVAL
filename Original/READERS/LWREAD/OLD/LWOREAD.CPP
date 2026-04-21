/*

		 Flood Demo System - LightWave object reader
		 -------------------------------------------

	The reader does NOT support detail polygons and curves, since these are
	not relevant for real-time rendering.


	LightWave-->FLOOD file convertor.

	FLOOD file format:

	char           ID[7]     = "FLOOD3D"
	unsigned short NumOfObjs
//Object data:
		unsigned short NumOfSurfs
		Surface list (AsciiZ)
		unsigned short NumOfVerts
		Vertex list (x,y,z)
		unsigned short NumOfFaces
//	Face list:
			short FaceVerts
			Face related vertex list
			short SurfaceNumber
*/
#pragma pack (1)

#include <Mem.H>
#include <Math.H>
#include <StdLib.H>
#include <DOS.H>
#include <StdIO.H>

#include "E:\Projects\FDS\Source\FDS_Defs.H"
#include "E:\Projects\FDS\Source\FDS_Vars.H"

#define DebugMode

typedef struct
{
	unsigned short  FaceVerts;
	unsigned short *Vertex;
	signed   short  Surface;
} FldFace;

typedef struct
{
	unsigned long  SurfaceLength;
	char          *SurfaceList; // Surface count starts with 1
	unsigned long  NumOfVerts;  // Vertex count starts with 0
	Vector        *Vertex;
	unsigned long  NumOfFaces;
	FldFace       *Face;
	signed short   SurfaceNumber;
} FldObject;

typedef struct
{
	char ID[4];
	unsigned long Length;
} ChunkType;

unsigned int  CurLength;
FILE         *LWO;
FILE         *FLD;
FldObject			Obj;
unsigned char SubChunk;

void ReadChunk(unsigned int ChunkEnd); // Forward declaration

void SwapDW(void *);  // Swap DWORD: 0x12345678 --> 0x78563412
#pragma aux SwapDW=\
"mov eax,[ebx]"\
"Rol ax,8"\
"Rol eax,16"\
"Rol ax,8"\
"mov [ebx],eax"\
parm [ebx]\
modify exact [eax];

void SwapW(void *);  // Swap WORD:   0x1234     --> 0x3412
#pragma aux SwapW=\
"mov ax,[ebx]"\
"Rol ax,8"\
"mov [ebx],ax"\
parm [ebx]\
modify exact [ax];

void ReadAsciiZ(char **s)
{
	int a=0;
	char Temp[256];

	do fread(&Temp[a++],1,1,LWO); while (Temp[a-1]);
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
		printf("Bad object file!\n");
		exit(1);
	}
	ReadChunk(EndPos);
}

void ReadFaceList(unsigned int EndPos)
{
int a;
unsigned short FaceVerts,Vert,SurfaceNum;
unsigned int NumOfFaces=0;

#ifdef DebugMode
	printf("Face list:\n");
#endif
	while (ftell(LWO)<EndPos)
	{
		fread(&FaceVerts,1,2,LWO);
		SwapW(&FaceVerts);
#ifdef DebugMode
		printf("Face %i has %i vertices:",NumOfFaces,FaceVerts);
#endif
		for (a=0;a<FaceVerts;a++)
		{
			fread(&Vert,1,2,LWO);
			SwapW(&Vert);
#ifdef DebugMode
			printf("%i ",Vert);
#endif
		}
		NumOfFaces++;
		fread(&SurfaceNum,1,2,LWO);
		SwapW(&SurfaceNum);
#ifdef DebugMode
		printf("| Surface: %i\n",SurfaceNum);
#endif
		if (SurfaceNum<1)
		{
			printf("\n\n\nError!!! Detail polygon are not supported!\n");
			exit(2);
		}
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
	Obj.NumOfVerts=NumOfVerts;
	Obj.Vertex=new Vector[NumOfVerts];
	for (a=0;a<NumOfVerts;a++)
	{
		fread(&V,1,sizeof(Vector),LWO);
		SwapDW(&V.X);
		SwapDW(&V.Y);
		SwapDW(&V.Z);
		Obj.Vertex[a]=V;
#ifdef DebugMode
		printf("Vertex %i: X:%f  Y:%f  Z:%f\n",a,V.X,V.Y,V.Z);
#endif
	}
}

void ReadSurfaceList(unsigned int EndPos)
{
	Obj.SurfaceList=new char[EndPos-ftell(LWO)];
	Obj.SurfaceLength=EndPos-ftell(LWO);
	fread(Obj.SurfaceList,1,Obj.SurfaceLength,LWO);
}

void ReadSurfaceData(unsigned int EndPos)
{
char *Surface;
unsigned long Pos;

	ReadAsciiZ(&Surface);
	fseek(LWO,ftell((LWO)+1)&0xFFFFFFFE,SEEK_SET);
#ifdef DebugMode
	printf("Detailed data for surface \"%s\":\n");
#endif
// Read sub-chunks
	SubChunk=1;
	ReadChunk(EndPos);
	SubChunk=0;
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
//	{"CRVS" , ReadCurves} Unsupported - irellevant for realtime rendering
	{"SURF" , ReadSurfaceData},
/*	{"COLR" , ReadSurfaceColor},
	{"FLAG" , ReadSurfaceFlag},
	{"LUMI" , ReadSurfaceLuminosity},
	{"DIFF" , ReadSurfaceDiffuse},
	{"SPEC" , ReadSurfaceSpecular},
	{"REFL" , ReadSurfaceReflection},
	{"TRAN" , ReadSurfaceTransparency},
	{"GLOS" , ReadSurfaceGlossiness},
	{"RIMG" , ReadSurfaceReflectionImage},
	{"RSAN" , ReadSurfaceReflectionSeamAngle},
	{"RIND" , ReadSurfaceRefractiveIndex},               /
	{"EDGE" , ReadSurfaceEdgeTransparency},              /
	{"SMAN" , ReadSurfaceMaximumSmoothingAngle},         /
	{"CTEX" , ReadSurfaceColorTexture},                  /
	{"DTEX" , ReadSurfaceDiffuseTexture},                /
	{"STEX" , ReadSurfaceSpecularTexture},               /
	{"RTEX" , ReadSurfaceReflectionTexture},             /
	{"TTEX" , ReadSurfaceTransparencyTexture},           /
	{"BTEX" , ReadSurfaceBumpTexture},                   /
	{"TIMG" , ReadSurfaceTextureImage},
	{"TFLG" , ReadSurfaceTextureFlag},
	{"TSIZ" , ReadSurfaceTextureSize},
	{"TCTR" , ReadSurfaceTextureCenter},
	{"TFAL" , ReadSurfaceTextureFallOff},
	{"TVEL" , ReadSurfaceTextureVelocity},
	{"TCLR" , ReadSurfaceTextureColor},
	{"TVAL" , ReadSurfaceTextureValue},
	{"TAMP" , ReadSurfaceTextureAmplitude},
	{"TFRQ" , ReadSurfaceNoiseFrequencies},
	{"TSP0" , ReadSurfaceTypeSpecificParameter0},        /
	{"TSP1" , ReadSurfaceTypeSpecificParameter1},        /
	{"TSP2" , ReadSurfaceTypeSpecificParameter2}         /*/
};

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
//		memset(&Chunk,sizeof(ChunkType),0);
		if (SubChunk)
		{
			fread(&Chunk,1,sizeof(ChunkType)-2,LWO);
			SwapW(&Chunk);
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

void WriteData(void)
{
	fprintf(FLD,"FLOOD3D");
	fwrite(&Obj.SurfaceLength,1,4,FLD);
	fwrite(&Obj.SurfaceList[0],1,Obj.SurfaceLength,FLD);
	fwrite(&Obj.NumOfVerts,1,4,FLD);
	fwrite(&Obj.Vertex[0],sizeof(Vector),Obj.NumOfVerts,FLD);
}

void ReadLWObject(char *InFile,char *OutFile)
{
unsigned int FileSize;

	if (!(LWO=fopen(InFile,"rb")))
	{
		fclose(LWO);
		printf("Unable to open file \"%s\"!\n",InFile);
		exit(1);
	}
	if (!(FLD=fopen(OutFile,"wb")))
	{
		fclose(FLD);
		printf("Unable to open file for writing!\n");
		exit(1);
	}
	memset(&Obj,0,sizeof(FldObject));
	fseek(LWO,0,SEEK_END);
	FileSize=ftell(LWO);
	fseek(LWO,0,SEEK_SET);
	ReadChunk(FileSize);
	WriteData();
	fclose(LWO);
	fclose(FLD);
}

void main(int argc,char *argv[])
{
	SubChunk=0;
	if (argc>1) ReadLWObject(argv[1],argv[2]);
	else printf("Syntax error! Type LWORead <Object file> <Flood file>\n");
}