/*

	 Object/Library reader, by Ofir Ehrlich
	 ======================================

	 Main file
	 Version: 1e-100000.0
	 TABs: 2

*/

#pragma pack(1)

#include <StdLib.H>
#include <StdIO.H>
#include <DOS.H>

#include "Obj_Read.H"

#define DebugMode

void PrintError(char *ErrMsg)
{
	printf("<OBJ> Error: %s\n",ErrMsg);
	exit(1);
}

void Read(void *Data,unsigned long Size)
{
	long i;

	fread(Data,1,Size,OBJFile);
// CheckSum
	for (i=0;i<Size;i++) CheckSum+=((char *)Data)[i];
}

char *ReadString(void)
{
	char *s;
	unsigned char Size;
	Read(&Size,1);
	if (Size)
	{
		s=new char[Size+1];
		s[Size]=0;
		Read(s,Size);
		return s;
	}
	else return NULL; // Just to be sure
}

void Read_TranslatorHeader(unsigned long EndPos)
{
	Mdl->Type=Module_Object;
	Mdl->Name=ReadString();
#ifdef DebugMode
	printf("Object module name: %s\n",Mdl->Name);
#endif
}

void Read_LibraryHeader(unsigned long EndPos)
{
	Mdl->Type=Module_Library;
	Mdl->Name=ReadString();
#ifdef DebugMode
	printf("Library module name: %s\n",Mdl->Name);
#endif
}

void Read_Comment(unsigned long EndPos)
{
	unsigned char CommentType,CommentClass;
	char *CStr;
	unsigned long CSize;

	if (!Mdl->CommentList)
	{
		Mdl->CommentList=new Comment;
		Cmt=Mdl->CommentList;
	}
	else
	{
		Cmt->Next=new Comment;
		Cmt=Cmt->Next;
	}
	memset(Cmt,0,sizeof(Comment));

	Read(&Cmt->Type,1);
	Read(&Cmt->Class,1);

	CSize=EndPos-ftell(OBJFile)-3; // I think...

	if (CSize)
	{
		Cmt->Data=new char[CSize];
		Read(Cmt->Data,CSize);
	}
}

struct
{
	char Type;
	void (*Reader)(unsigned long EndPos);
} ChunkData[]=
{
	{0x80	,	Read_TranslatorHeader},
	{0x82	, Read_LibraryHeader},
	{0x88	, Read_Comment},					Comment remains commented for now... ;)
/*{0x8A	, Read_ModuleEnd},                            v
	{0x8C	,	Read_ExternalNamesDefinition},              v
	{0x8E	,	Read_TypeDefinition},                       v
	{0x90	,	Read_PublicDefinition},                     v
	{0x94	,	Read_LineNumbers},                          x
	{0x96	,	Read_ListOfNames},                          v
	{0x98	,	Read_SegmentDefinition},                    v
	{0x9A	,	Read_GroupDefinition},                      v
	{0x9C	,	Read_FixUp},                                v
	{0xA0	,	Read_LogicalEnumeratedData},                v
	{0xA2	,	Read_LogicalIteratedData},
	{0xB0	,	Read_CommunalNamesDefinition},
	{0xB2	,	Read_BackPatch},
	{0xB4	, Read_LocalExternalNamesDefinition},
	{0xB6	, Read_LocalPublicNamesDefinition},
	{0xB8	, Read_LocalCommunalNamesDefinition},
	{0xC2	, Read_InitializedCommunalData},
	{0xC4	, Read_SymbolLineNumbers},
	{0xC6	, Read_AliasDefinition},
	{0xC8	, Read_NamedBackPatch},
	{0xCA	, Read_LocalLogicalNamesDefinition},
	{0xF0	,	Read_LibrarySignature}*/
};

signed char FindChunk(unsigned char Type)
{
	long i;

	for (i=0;i<sizeof(ChunkData)/sizeof(ChunkData[0]);i++)
		if (ChunkData[i].Type==Type) return i;
	return -1;
}

void ClearCheckSum(void)
{
	if (CheckSum) PrintError("Checksum error");
	CheckSum=0;
}

void ReadChunk(unsigned long ChunkEnd)
{
	unsigned long Pos;
	short CurChunk;
	ChunkType Chunk;
	unsigned char CheckSumByte;

	while (ftell(OBJFile)<ChunkEnd)
	{
		Read(&Chunk,sizeof(ChunkType));
		Enable32BitValues=Chunk.Type&0x001;
		Chunk.Type&=0xFE; // Remove least significant bit
		Pos=ftell(OBJFile);
		if (Chunk.Length==0) return;
		CurChunk=FindChunk(Chunk.Type);
		if (CurChunk>=0)
		{
			Pos=Pos+Chunk.Length;
			if (ChunkData[CurChunk].Reader)
			{
				ChunkData[CurChunk].Reader(Pos);
				Read(&CheckSumByte,1);
				if (!CheckSumByte) CheckSum=0; else ClearCheckSum();
			}
			else ReadChunk(Pos);
			fseek(OBJFile,Pos,SEEK_SET);
		}
		else fseek(OBJFile,Pos+Chunk.Length,SEEK_SET);
	}
}

void main(long ParmNum,char *ParmList[])
{
	unsigned long FileLength;

	printf("Supermultiatomichyperinternational Object/Library reader!!! :)\n");

	if (ParmNum<2) PrintError("Object/Library file name is required!");
	if (!(OBJFile=fopen(ParmList[1],"rb")))	PrintError("Unable to open file!");

	fseek(OBJFile,0,SEEK_END);
	FileLength=ftell(OBJFile);
	fseek(OBJFile,0,SEEK_SET);

	CheckSum=0;
	Mdl=new Module;
	memset(Mdl,0,sizeof(Module));
	ReadChunk(FileLength);
	fclose(OBJFile)

	printf("Done reading object/library file!\n");
}