#include "I_defs.h"
#include "FUtils.h"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Base/FDS_DEFS.H"

// Material Library Reader, Format: 3DS ver 4.0
void ReadMaterialName(unsigned int EndPos)
{
  char *MatName = new char[80];
  Material *M = NULL;

  ReadASCIIZ(&MatName);
//  printf("<3DS:M>: Attempting to insert Material %s to Library.\n",MatName);
//  printf("<3DS:M>: MatLib addr = %xh.\n",(int32_t)MatLib);
  fflush(stdout);
  if (MatLib)
    for(M=MatLib;M;M=M->Next) if (!strcmp(MatName,M->Name)) break;
  if (M)
  {
    printf("Keibinimat! two materials of the same shit! no way ho-zei!\n");
    ReadTextureToo = 0;
    return;
  }

  //Insert material to Library.
  if (!MatLib)
  {
//    printf("<3DS:M>: No previous entries detected, Creating Library.\n");
    fflush(stdout);
    MatLib = getAlignedType<Material>(16);// (Material*)getAlignedBlock(sizeof(Material), 16);
    Cur_Mat = MatLib;
    //memset(MatLib,0,sizeof(Material));
  } else {
//    printf("<3DS:M>: New material will be linked to last entry %s\n",Cur_Mat->Name);
    fflush(stdout);
    Cur_Mat->Next = getAlignedType<Material>(16);// (Material*)getAlignedBlock(sizeof(Material), 16);
//    memset(Cur_Mat->Next,0,sizeof(Material));
    Cur_Mat->Next->Prev = Cur_Mat;
    Cur_Mat=Cur_Mat->Next;
  }
  Cur_Mat->RelScene = Host;
  Cur_Mat->Name = _strdup(MatName);
  delete []MatName;
  ReadTextureToo = 1;
}

void ReadMapFile(unsigned int EndPos)
{
  char *MapFile = new char[40];
  char *NewFile = new char[40];
  Material *M;
  Texture *Tx;
  strcpy(NewFile,"TexturesNew//");

  ReadASCIIZ(&MapFile);
  strcat(NewFile,MapFile);
  for(M=MatLib;M;M=M->Next) if (M->Txtr&&(!strcmp(NewFile,M->Txtr->FileName))) break;
  if (M)
  {
    //Redirect Texture and Leave
//    printf("<3DS:T>: New Texture has been loaded before for Material %s, \n         will be assigned to New Material %s too.\n",M->Name,Cur_Mat->Name);
    Cur_Mat->Txtr = M->Txtr;
    goto OUT;
  }
  if (!ReadTextureToo)
  {
//    printf("<3DS:T>: Last Material was ignored...will not load associated texture.\n");
    goto OUT;
  }

  Cur_Mat->Txtr = new Texture;
  Tx = Cur_Mat->Txtr;
  memset(Tx,0,sizeof(Texture));
  Tx->FileName = _strdup(NewFile);
  Identify_Texture(Tx);

  if (!Tx->BPP) printf("<3DS:T>: Identification Failure for Texture %s at Material %s.\n",NewFile,Cur_Mat->Name); //else printf("<3DS:T>: Texture %s was Identified correctly\n         for Material %s as a %d BPP image file.\n",NewFile,Cur_Mat->Name,Cur_Mat->Txtr->BPP);
  if (!Tx->BPP) {delete Tx; Cur_Mat->Txtr = NULL;}

  Cur_Mat->Flags = 0;
  Cur_Mat->Diffuse = 1.0;
  Cur_Mat->Specular = 1.0; //Rulez
  OUT:
  delete []MapFile;
  delete []NewFile;
}

void SetTwoSides(unsigned int EndPos)
{
  Cur_Mat->Flags|=Mat_TwoSided;
//  printf("<3DS:Mat>: Material %s is double sided.\n",Cur_Mat->Name);
}

void SetTransparent(unsigned int EndPos)
{
  char A[6];

  unsigned short TPerc;
  Read(A,6);
  Read(&TPerc,2);
  if (TPerc>0) Cur_Mat->Flags|=Mat_Transparent;
//  printf("<3DS:Mat>: Material %s is transparent at %i%%.\n",Cur_Mat->Name,TPerc);
}

void ReadShading(unsigned int EndPos)
{
  unsigned short Shading;

  Read(&Shading,2);
  if (Shading>1) Cur_Mat->Flags|=Mat_RGBInterp;
}

