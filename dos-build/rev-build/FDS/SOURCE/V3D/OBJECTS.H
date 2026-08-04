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
  long I;
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

void Insert_Omni2Scene(Scene *Sc,Omni *Om)
{
  long I;
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