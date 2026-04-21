#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma pack(1)
typedef struct
{
	 char Type;
	 short unsigned int Length;
} ObjChunk;

static char IsEOF=0;
static FILE *ObjFile;
static unsigned int Pointer=0;
char String[80];
unsigned short int Size;
unsigned char CheckSum;
ObjChunk Chunk;
#pragma pack()

void Read(void *P,int size)
{
	fread(P,1,size,ObjFile);
	if (feof(ObjFile)) IsEOF=1;
	Pointer+=size;
	return;
}

void Seek(unsigned int NewOfs)
{
	fseek(ObjFile,NewOfs,SEEK_SET);
	if (feof(ObjFile)) IsEOF=1;
	Pointer=NewOfs;
	return;
}

void ReadTHeader(void)
{
	 int i;
	 char j;
	 unsigned char Size;

	 Read(&Size,1);
	 Read(&String,Size);
	 String[Size]=0;
	 CheckSum=Chunk.Type+(Chunk.Length&0xff)+((Chunk.Length&0xff00)>>8)+Size;
	 printf("Type :%x Name:Translator Header Record Length:%d\n",Chunk.Type,Chunk.Length);
	 printf("Name of the object moudle:%s\n",String);
	 for (i=0;i<Size;i++) CheckSum+=String[i];
	 Read(&j,1);
	 CheckSum+=j;
	 if (((CheckSum%256)==0) || (j==0)) printf("CheckSum OK\n");else printf("CheckSum Wrong!\n");

}

int main(int argc,char *argv[])
{
//	setvbuf(stdout,(char *)(0),_IONBF,0);
	if (argc<2)
	{
		 printf("Parameter needed: Object file name\n");
		 fflush(stdout);
		 return 1;
	}
	if ((ObjFile=fopen(argv[1],"rb"))==NULL)
	{
	 fclose(ObjFile);
	 return 1;
	}
	Seek(0);
	while (!IsEOF)
	{
		 printf("\n");
		 Read(&Chunk,3);
		 switch (Chunk.Type)
		 {
				case 0x80: ReadTHeader();break;
				default : printf("unknown Header: %x Size:%d\n",Chunk.Type,Chunk.Length);Seek(Pointer+Chunk.Length);break;
		 }
	}
	return 0;
}
