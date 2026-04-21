#include <Base/TriMesh.h>
#include <Base/Object.h>
#include <Base/Scene.h>
#include <Base/Omni.h>
#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"


TriMesh *V3D_Object;
FILE *V3D_Mesh;


void Nil_Spline_Init(Spline *S)
{
  S->NumKeys = 1;
  S->Flags = 0;
  S->Keys = new SplineKey;
  memset(S->Keys,0,sizeof(SplineKey));
  S->CurKey = 0;
}

void Material2Face(Material *M,Face *F)
{
  F->Txtr=M;
  if (M->Flags&Mat_TwoSided) F->Flags|=Face_TwoSided; else F->Flags&=0xFFFFFFFF-Face_TwoSided;
  if (M->Flags&Mat_Transparent) F->Flags|=Face_Transparent; else F->Flags&=0xFFFFFFFF-Face_Transparent;
}

// Applies stuff to the basic object read from the V3D file
void Insert_TriMesh2Scene(Scene *Sc,TriMesh *T)
{
  int32_t I;
  Face *F;
  Object *Obj = new Object,*O;
  TriMesh *Tri;

  memset(Obj,0,sizeof(Object));
  // Attach new Object to list.
  if (Sc->ObjectHead)
  {
    for(O=Sc->ObjectHead;O->Next;O=O->Next);
    O->Next = Obj;
    Obj->Prev = O;
    Obj->Next = NULL;
  } else {
    Sc->ObjectHead = Obj;
    Obj->Next = Obj->Prev = NULL;
    Obj->Number = 0;
  }

  // Fill in Object general Data
  Obj->Data = (void *)T;
  Obj->Type = Obj_TriMesh;
  if (Obj->Prev) Obj->Number=Obj->Prev->Number+1;
  Obj->Name = strdup("V3D Class Generated Object");
  Obj->Pos = &T->IPos;
  Obj->Rot = &T->RotMat;
  Obj->Parent = NULL;

  // Initialize Trimesh Info
  T->Flags = 0;
  Nil_Spline_Init(&T->Pos);
  Nil_Spline_Init(&T->Rotate);
  Nil_Spline_Init(&T->Scale);
  T->Status = new ObjectStatus;
  T->Status->Frame = 0.0;
  T->Status->Stat = HTrack_Visible;
  T->Status->Next = NULL;
  T->Status->Prev = NULL;
  T->CurStat = T->Status;

  Quaternion_Form(&T->Scale.Keys->Pos,1,1,1,0);
  Vector_Form(&T->IPos,0,0,0);
  Vector_Form(&T->IScale,1,1,1);
  Quaternion_Form(&T->IRot,0,0,0,1);
  T->Flags |= HTrack_Visible;
  Matrix_Form(T->RotMat,1,0,0,0,1,0,0,0,1);
  for(I=0;I<T->FIndex;I++)
    Material2Face(&Default_Mat,T->Faces+I);
  // Insert to Trimesh List.
  if (Sc->TriMeshHead)
  {
    for(Tri=Sc->TriMeshHead;Tri->Next;Tri=Tri->Next);
    T->Prev = Tri;
    Tri->Next = T;
  } else {Sc->TriMeshHead = T; T->Prev = NULL;}
  T->Next = NULL;
}

void Insert_Omni2Scene(Scene *Sc, Omni *Om)
{
  int32_t I;
  Face *F;
  Object *Obj = new Object,*O;
  Omni *Omni;

  memset(Obj,0,sizeof(Object));
  // Attach new Object to list.
  if (Sc->ObjectHead)
  {
    for(O=Sc->ObjectHead;O->Next;O=O->Next);
    O->Next = Obj;
    Obj->Prev = O;
  } else {
    Sc->ObjectHead = Obj;
    Obj->Prev = NULL;
    Obj->Number = 0;
  }
  Obj->Next=NULL;

  // Fill in Object general Data
  Obj->Data = (void *)Om;
  Obj->Type = Obj_Omni;
  if (Obj->Prev) Obj->Number=Obj->Prev->Number+1;
  Obj->Name = strdup("V3D Class Generated Object");
  Obj->Pos = &Om->IPos;
  Obj->Rot = &Mat_ID;
  Obj->Parent = NULL;

  Nil_Spline_Init(&Om->Pos);
/*  Om->Status = new ObjectStatus;
  Om->Status->Frame = 0;
  Om->Status->Stat = HTrack_Visible;
  Om->Status->Next = NULL;
  Om->Status->Prev = NULL;*/
  Vector_Form(&Om->IPos,0,0,0);
//  Om->Flags |= HTrack_Visible;
  Om->Flags = 0;
  Om->F.A = Om->F.B = Om->F.C = &Om->V;
  if (Sc->OmniHead)
  {
    for(Omni=Sc->OmniHead;Omni->Next;Omni=Omni->Next);
    Om->Prev = Omni;
    Omni->Next = Om;
  } else {Sc->OmniHead = Om; Om->Prev = NULL;}
  Om->Next = NULL;
}

char Load_V3D(Scene *Sc,char *FileName)
{
  int32_t LPts,LPolys;
  int32_t A,B,C,I;
  signed short Ch;

  V3D_Object=NULL;
  if (!(V3D_Mesh=fopen(FileName,"rb")))
  {
    fclose(V3D_Mesh);
    return 0;
  }

  fread(&LPts,4,1,V3D_Mesh);
  fread(&LPolys,4,1,V3D_Mesh);

  #ifdef DebugInfo
    printf("�  (%d Vertices,%d Polygons) \n",LPts,LPolys);
    getchar();
  #endif

  V3D_Object=(TriMesh *)getAlignedBlock(sizeof(TriMesh), 16);
  V3D_Object->VIndex=LPts;
  V3D_Object->FIndex=LPolys;
  V3D_Object->Verts=new Vertex[LPts];
  V3D_Object->Faces=new Face[LPolys];

  for(I=0;I<LPts;I++)
  {
    // Y and Z SWAPPED!!
    fread(&V3D_Object->Verts[I].Pos.x,4,1,V3D_Mesh);
    fread(&V3D_Object->Verts[I].Pos.z,4,1,V3D_Mesh);
    fread(&V3D_Object->Verts[I].Pos.y,4,1,V3D_Mesh);
  }
  for(I=0;I<LPolys;I++)
  {
    fread(&A,4,1,V3D_Mesh);
    fread(&B,4,1,V3D_Mesh);
    fread(&C,4,1,V3D_Mesh);
    V3D_Object->Faces[I].A = (Vertex *)(V3D_Object->Verts+A);
    V3D_Object->Faces[I].B = (Vertex *)(V3D_Object->Verts+B);
    V3D_Object->Faces[I].C = (Vertex *)(V3D_Object->Verts+C);
    V3D_Object->Faces[I].Txtr = &Default_Mat;
  }
  for(I=0;I<LPts;I++)
  {
    fread(&Ch,2,1,V3D_Mesh);
    V3D_Object->Verts[I].U=Ch;

    fread(&Ch,2,1,V3D_Mesh);
    V3D_Object->Verts[I].V=Ch;
  }
  fclose(V3D_Mesh);

  Insert_TriMesh2Scene(Sc,V3D_Object);

//  Scene_Computations(Sc);
  return 1;
}
