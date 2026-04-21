#include "I_defs.h"
#include "FUtils.h"
#include "Base/FDS_VARS.H"
#include "Base/Scene.h"
#include "Base/FDS_DECS.H"
extern FILE * IN_3DS;
// Static World/Scene Reader, Format: 3DS ver 4.0
void New_Object(void)
{

  if (!Host->ObjectHead)
  {
//    printf("<3DS:Obj>: No previous entries detected, Creating List.\n");
    Host->ObjectHead = new Object;
    Cur_Obj = Host->ObjectHead;
    memset(Cur_Obj,0,sizeof(Object));
  }
  else
  {
//    printf("<3DS:Obj>: New object will be attached to '%s'\n",Cur_Obj->Name);
    Cur_Obj->Next = new Object;
    memset(Cur_Obj->Next,0,sizeof(Object));
    Cur_Obj->Next->Prev = Cur_Obj;
    Cur_Obj = Cur_Obj->Next;
  }
  Cur_Obj->Number=0xFFFFFFFF; // unassigned ID
}


void ReadObjectName(unsigned int EndPos)
{
  ReadASCIIZ(&CurObjName);
//  printf("<3DS:Obj>: Reading object, Name = %s\n",CurObjName);
  Read3DSChunk(EndPos,Chk_ID);
}

void ReadTriMesh(unsigned int EndPos)
{
  TriMesh *T;
//  printf("<3DS:Tri>: Adding a Trimesh object...\n");
  New_Object();
  Cur_Obj->Type=Obj_TriMesh;
  Cur_Obj->Name=CurObjName;
  Cur_Obj->Number=Cur_ID++;
  T = (TriMesh *)getAlignedBlock(sizeof(TriMesh), 16);
  Cur_Obj->Data=(void *)T;
  memset(T,0,sizeof(TriMesh));
  Matrix_Copy(T->RotMat,Mat_ID);
  if (Host->TriMeshHead)
  {
    Cur_Tri->Next=T;
    Cur_Tri=Cur_Tri->Next;
  }
  else Cur_Tri = Host->TriMeshHead = T;

  Cur_Obj->Rot=&T->RotMat;
  Cur_Obj->Pos=&T->IPos;
  Read3DSChunk(EndPos,Chk_ID);
}

void ReadVertList(unsigned int EndPos)
{
  unsigned short Verts;
  Vector Pos;
  TriMesh *T = Cur_Tri;
  Vertex *Vtx,*VE;

  Read(&Verts,2);
//  printf("<3DS:Tri>: Trimesh has %d Vertices.\n",Verts);
	T->VIndex=Verts;
	// CACHE ALIGN. dont ask. well...it doesnt work!!!!!! (Arrgh)
//	T->Verts = new Vertex[Verts+1];
//	T->Verts = (Vertex *) ((((DWord)(T->Verts))+0x1F)&0xFFFFFFE0);
	T->Verts = new Vertex[Verts];
  Vtx = T->Verts;
  memset(Vtx,0,sizeof(Vertex)*Verts);

  VE = Vtx+Verts;
  for (;Vtx<VE;Vtx++)
  {
//    Read(&Pos,sizeof(Vector));
	Pos.Read(IN_3DS);
    SwapYZ(&Pos);
    Vector_Copy(&Vtx->Pos,&Pos);
  }
}

void ReadFaceList(unsigned int EndPos)
{
  unsigned short Faces;
  unsigned short A,B,C;
  unsigned short Flag;
  TriMesh *T = Cur_Tri;
  Face *F,*FE;
  Vertex *Vtx = T->Verts;

  Read(&Faces,2);
//  printf("<3DS:Tri>: Object has %d Faces.\n",Faces);
  T->FIndex=Faces;
  T->Faces = new Face[Faces];
  F = T->Faces;
  memset(F,0,sizeof(Face)*Faces);
  FE = F+Faces;
  for (;F<FE;F++)
  {
    Read(&A,2);
    Read(&B,2);
    Read(&C,2);
    Read(&Flag,2);
    if (Flag&0x08) F->Flags |= Face_UWrap;
    if (Flag&0x10) F->Flags |= Face_VWrap;
    F->A=Vtx+A;
    F->B=Vtx+B;
    F->C=Vtx+C;
    F->Txtr=NULL;
  }
  Read3DSChunk(EndPos,Chk_ID);
}

void ReadFaceMaterial(unsigned int EndPos)
{
  unsigned short Faces;
  unsigned short n;
  Material *M;
  char *s;
  Face *F = Cur_Tri->Faces;

  ReadASCIIZ(&s);
//  printf("<3DS:Mat>: Looking for Material %s in Library.\n",s);

  if (MatLib)
  {
    for (M=MatLib;M;M=M->Next)
      if (!strcmp(M->Name,s)) break;
  } else return;


//  if (M) printf("<3DS:Mat>: Material found and will be placed.\n"); else printf("<3DS:Mat>: Material not found. Faces will have NULL material.\n");

  Read(&Faces,2);
  while (Faces--)
  {
    Read(&n,2);
    F[n].Txtr=M;
  }
}

void ReadMapList(unsigned int EndPos)
{
  unsigned short Verts;
  float u,v;
  TriMesh *T = Cur_Tri;
  Vertex *Vtx = T->Verts,*VE;

  Read(&Verts,2);
/*  if (Verts == T->VIndex) printf("<3DS:Map>: Reading Mapping coordinates for Trimesh %s.\n",Cur_Obj->Name);
    else if (Verts) printf("<3DS:Map>: Reading Partial Mapping coordinate list for Trimesh %s.\n",Cur_Obj->Name);
  if (Verts) printf("<3DS:Map>: No Mapping coordinates are available for Trimesh %s.\n",Cur_Obj->Name);*/

  VE = Vtx + Verts;
  for (;Vtx<VE;Vtx++)
  {
    Read(&u,4);
    Read(&v,4);
    Vtx->U=u*256;
    Vtx->V=v*256;
  }
}

void ReadTranslation(unsigned int EndPos)
{
  Matrix TransMatrix;
  Vector TransPivot;
  Vector U;
  Vertex *Vtx = Cur_Tri->Verts,*VE = Vtx + Cur_Tri->VIndex;

  Read(TransMatrix,sizeof(Matrix));
  //Read(&TransPivot,sizeof(Vector));
  TransPivot.Read(IN_3DS);
  SwapYZ(&TransPivot);
  SwapYZ(TransMatrix);
  Matrix_InvScale(TransMatrix);
  for(;Vtx<VE;Vtx++)
  {
    Vector_Sub(&Vtx->Pos,&TransPivot,&U);
    MatrixXVector(TransMatrix,&U,&Vtx->Pos);
  }
}

void ReadCamera(unsigned int EndPos)
{
  Vector Source,Target;
  Camera *Cm;
//  printf("<3DS:Cam> Reading a Camera object...\n");
  New_Object();
  Cur_Obj->Name=CurObjName;
  Cur_Obj->Number=Cur_ID++;
  Cur_Obj->Type=Obj_Camera;
  Cm = new Camera;
  Cur_Obj->Data=(void *)Cm;
  Cur_Obj->Rot=&Mat_ID;
  Cur_Obj->Pos=&Cm->ISource;
  memset(Cm,0,sizeof(Camera));
  if (Host->CameraHead)
  {
    Cur_Cam->Next=Cm;
    Cur_Cam = Cm;
  }
  else Cur_Cam = Host->CameraHead = Cm;

  // static information that is meaningless in an animated scene
  //Read(&Cm->ISource,sizeof(Vector));
  //Read(&Cm->ITarget,sizeof(Vector));
  Cm->ISource.Read(IN_3DS);
  Cm->ITarget.Read(IN_3DS);
  SwapYZ(&Cm->ISource);
  SwapYZ(&Cm->ITarget);
  Read(&Cm->IRoll,4);
  Read(&Cm->IFOV,4);
  Cm->Flags = 0;
/*  printf("\nCamera object:\n");
  printf("  Source: x: %f  y: %f  z: %f\n",Cm->ISource.X,Cm->ISource.Y,Cm->ISource.Z);
  printf("  Target: x: %f  y: %f  z: %f\n",Cm->ITarget.X,Cm->ITarget.Y,Cm->ITarget.Z);
  printf("  Roll: %f  Field of view: %f\n",Cm->IRoll,Cm->IFOV);*/
}

void ReadLight(unsigned int EndPos)
{
  Vector Pos;
  Omni *O;

//  printf("<3DS:Lgt>: Reading an OmniLight object...\n");
  New_Object();
  Cur_Obj->Name=CurObjName;
  Cur_Obj->Type=Obj_Omni;
  Cur_Obj->Number=Cur_ID++;
  O = (Omni *)getAlignedBlock(sizeof(Omni), 16);
  Cur_Obj->Data=(void *)O;
  Cur_Obj->Rot=&Mat_ID;
  Cur_Obj->Pos=&O->IPos;
  memset(O,0,sizeof(Omni));
  if (Host->OmniHead)
  {
    Cur_Omni->Next=O;
    Cur_Omni=Cur_Omni->Next;
  }
  else Cur_Omni = Host->OmniHead=O;

  Cur_Obj->Pos=&O->IPos;
  //Read(&O->IPos,sizeof(Vector));
  O->IPos.Read(IN_3DS);
  SwapYZ(&O->IPos);
  // Temporary assumptions
  O->Flags=0;//Flare_LightSource;
  //END
  O->F.A=O->F.B=O->F.C=&O->V;
  Read3DSChunk(EndPos,Chk_ID);
}

/*void ReadSpotLight(unsigned int EndPos)
{
  Omni *O;
  Vector Pos;
  float FallOff,HotSpot;

  Object->Name=CurObjName;
  Object->Type=Obj_SpotLight;
  if (!OmniHead->Next) OmniHead=NULL;
  else
  {
    for (O=OmniHead;O->Next;O=O->Next);
    O->Next=NULL;
  }
  delete(Object->Data);
  Object->Data=(void *)(new SpotLightType);
  memset(Object->Data,0,sizeof(SpotLightType));
  if (SpotLight)
  {
    SpotLight->Next=(SpotLightType *)Object->Data;
    SpotLight=SpotLight->Next;
  }
  else
  {
    SpotLightHead=(SpotLightType *)Object->Data;
    SpotLight=SpotLightHead;
  }
//  Read(&Pos,sizeof(Vector));
  Pos.Read(IN_3DS);
  Read(&FallOff,4);
  Read(&HotSpot,4);
  SwapYZ(&Pos);
#ifdef DebugMode
  printf("Spot light target: x: %f  y: %f  z: %f\n",Pos.X,Pos.Y,Pos.Z);
#endif
} */

