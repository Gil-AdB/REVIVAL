#include <StdIO.H>
#include <StdLib.H>
#include <ConIO.H>
#include <Math.H>
#include <Memory.H>
#include <String.H>
#include <i86.H>
#include <Time.H>

#include "..\FDS_Defs.H"
#include "..\FDS_Vars.H"
#include "..\FDS_Decs.H"


#pragma aux REPSTDW=\
  "Rep StoSD",\
  parm [EDI] [EAX] [ECX];

FILE *LogFile;

// Computes Re-sorted Indexing that will hopefully produce less CACHE misses.
// ok, maybe not. I'm just keeping this proc for fun. now, all we need is
// a kipper.
void Compute_Optimized_Indices(TriMesh *T)
{
  Face *F,*FEnd;
  DWord *Conversion = new DWord[T->VIndex];
  Vertex *Buffer;
  long I=0;
  long A,B,C;
  REPSTDW(Conversion,-1,T->VIndex);
  F=T->Faces;
  FEnd=F+T->FIndex;
  // Remapping
  for(;F<FEnd;F++)
  {
    A = F->A-T->Verts; B=F->B-T->Verts; C=F->C-T->Verts;
    if (Conversion[A]==-1) Conversion[A]=I++;
    if (Conversion[B]==-1) Conversion[B]=I++;
    if (Conversion[C]==-1) Conversion[C]=I++;
  }
  // Redistribition
  Buffer = new Vertex[T->VIndex];
  memcpy(Buffer,T->Verts,T->VIndex*sizeof(Vertex));
  for(I=0;I<T->VIndex;I++)
    memcpy(T->Verts+Conversion[I],Buffer+I,sizeof(Vertex));
  F=T->Faces;
  for(;F<FEnd;F++)
  {
    A = F->A-T->Verts; B=F->B-T->Verts; C=F->C-T->Verts;
    F->A = T->Verts+Conversion[A];
    F->B = T->Verts+Conversion[B];
    F->C = T->Verts+Conversion[C];
  }
  delete Buffer;
  delete Conversion;
}

// Computes Face/Triangle Plane Normals for Trimesh T.
void Compute_Face_Normals(TriMesh *T)
{
  Face *F = T->Faces, *FEnd = F+T->FIndex;
  Vector U,V;

  for(;F<FEnd;F++)
  {
    Vector_Sub(&F->A->Pos,&F->B->Pos,&U);
    Vector_Sub(&F->C->Pos,&F->A->Pos,&V);
    Cross_Product(&U,&V,&F->N);
    if (Vector_Length(&F->N)<0.000001)
    {
//      printf ("WARNING! 256 - divided by WARNING! (FACE %d)\n",F-T->Face);
//      printf ("U=(%f,%f,%f),V=(%f,%f,%f),Cross=(%f,%f,%f)\n",U.X,U.Y,U.Z,V.X,V.Y,V.Z,F->N.X,F->N.Y,F->N.Z);
    }
      else
        Vector_Norm(&F->N);
  }
}

// Computes Vertex/Pseudo Approximated Surface Normals for Trimesh T.
// Upgraded Version, Works well with meshes having non-aligned Triangularity
void Compute_Vertex_Normals(TriMesh *T)
{
  Vertex *Vtx,*VEnd = T->Verts+T->VIndex;
  Face *F,*FEnd = T->Faces+T->FIndex;
  Vector V;

  // Compute Vertex Normals and Shared faces.
  for(Vtx=T->Verts;Vtx<VEnd;Vtx++)
  {
    Vector_Form(&Vtx->N,0,0,0);
//    Vtx->Faces=0; //Reset Face counter
//    Vtx->FRem=0;  //Reset Face/Remaining counter
  }

  for(F=T->Faces;F<FEnd;F++)
  {
    Vector_Scale(&F->N,Tri_Surface(&F->A->Pos,&F->B->Pos,&F->C->Pos),&V);
//    if (Dot_Product(&V,&F->A->N)>0)
      Vector_SelfAdd(&F->A->N,&V);
//    else
//      Vector_SelfSub(&F->A->N,&V);
//      F->A->Faces++;
//    if (Dot_Product(&V,&F->B->N)>0)
      Vector_SelfAdd(&F->B->N,&V);
//    else
//      Vector_SelfSub(&F->B->N,&V);
//      F->B->Faces++;
//    if (Dot_Product(&V,&F->C->N)>0)
      Vector_SelfAdd(&F->C->N,&V);
//    else
//      Vector_SelfSub(&F->C->N,&V);
//      F->C->Faces++;
  }

  for(Vtx=T->Verts;Vtx<VEnd;Vtx++)
    if (Vector_Length(&Vtx->N)<EPSILON)
    {
//      printf ("WARNING! 256 - divided by WARNING! (VERTEX %d)\n",Vtx-T->Vertex);
    }
    else
      Vector_Norm(&Vtx->N);
}

// Computes the 'Normal Product' values for each Face for Trimesh T.
void Compute_Normal_Products(TriMesh *T)
{
  Face *F,*FEnd = T->Faces+T->FIndex;

  for(F=T->Faces;F<FEnd;F++)
		F->NormProd = -Dot_Product(&F->A->Pos,&F->N);
}

// Computes a Bounding Sphere for Trimesh T.
void Compute_Bounding_Sphere(TriMesh *T)
{
  Vertex *Vtx,*VEnd = T->Verts+T->VIndex;
  Face *F,*FEnd = T->Faces+T->FIndex;
  float Rad,F1;
  Vector U,V;
  long I;

  Rad=0;
  for(Vtx=T->Verts;Vtx<VEnd;Vtx++)
    if ((F1=Dot_Product(&Vtx->Pos,&Vtx->Pos))>Rad) Rad=F1;
#ifdef DebugMode
   printf("Sphere Radius = %f,Volume = %f\n",sqrt(Rad),Rad*4*PI);
#endif

  T->BSphereRad=Rad; //this is radius squared.

/*  F1 = 0.0;
  for(I=0;I<T->Scale.NumKeys;I++)
  {
    if ((Rad = T->Scale.Keys[0].Pos.X)>F1) F1 = Rad;
    if ((Rad = T->Scale.Keys[0].Pos.Y)>F1) F1 = Rad;
    if ((Rad = T->Scale.Keys[0].Pos.Z)>F1) F1 = Rad;
  }
  T->BSphereRad*=F1;*/
}

/*void Update_Bounding_Spheres(Scene *Sc)
{

  // Apply scale keys effect. look for maximum scale value possible.
  // actually,this is not 100% correct, because the spline itself may
  // pass that.
  // now THIS is a serious pain in the ass, because HIERARCHY! affects
  // scale too, so I'm supposed to find the max of multiplicand of
  // several splines... besides, the algo supposed to be scene independent
  // i.e. per trimesh
  // no,I'm NOT going to find solutions to deriv(hier_prev*scale splines)=0,
  // look for someone else to do the lousy job. it-would-have-been-possible
  // it was just for a single,non-hierarchial spline.

/*  F1 = 0.0;
  for(I=0;I<T->Scale.NumKeys;I++)
  {
    if ((Rad = T->Scale.Keys[0].Pos.X)>F1) F1 = Rad;
    if ((Rad = T->Scale.Keys[0].Pos.Y)>F1) F1 = Rad;
    if ((Rad = T->Scale.Keys[0].Pos.Z)>F1) F1 = Rad;
  }
  T->BSphereRad*=F1;
}*/

// Compute The neccesity for UV-Wrapping Flags for Trimesh T,and
// removes them if neccesary.
// Only useful for 3DS origined Scenes.
void Compute_Lighting_Flag(TriMesh *T)
{
  Face *F,*FEnd = T->Faces+T->FIndex;
  char bit=0;
  for(F=T->Faces;F<FEnd;F++)
    if (F->Txtr->Flags&Mat_RGBInterp) bit=1;
  if (!bit) T->Flags|=Tri_Noshading;
}

// Determines which of T's Vertices are Spikes.
// Spikes are Vertices whose vectors from neighbours to them
// have a positive dot-prod with their normals.
// essencially spikes are vertices that form an "Arrowhead" from thier
// near surface. they have topological properities making them highly
// important for accurate collision detection.
void Compute_Spikes(TriMesh *T)
{
  Vertex *V = T->Verts,*VE = V + T->VIndex;
  Edge *E = T->Edges,*EE = E + T->EIndex;
  Vector U;
  long Count;
  for(;V<VE;V++)
  {
    for(;E<EE;E++)
    {
      if (V==E->A)
      {
        if (E->Flags&Edge_True) {V->Flags|=Vtx_Spike; goto Spiked;}
        Vector_Sub(&V->Pos,&E->B->Pos,&U);
        if (Dot_Product(&U,&V->N)>EPSILON) {V->Flags|=Vtx_Spike; goto Spiked;}
      }
      if (V==E->B)
      {
        if (E->Flags&Edge_True) {V->Flags|=Vtx_Spike; goto Spiked;}
        Vector_Sub(&V->Pos,&E->A->Pos,&U);
        if (Dot_Product(&U,&V->N)>EPSILON) {V->Flags|=Vtx_Spike; goto Spiked;}
      }
    }
    Spiked:
    E = T->Edges; //reset edge pointer
  }
}

//Edge calculation
struct EdgeNode
{
  Edge *E;
  EdgeNode *L,*R; //Nodes are kept sorted by edge vertices
};

EdgeNode *EdgeIns;

char Insert_Edge(Vertex *V1,Vertex *V2,Edge *E,EdgeNode *Root)
{
  Vertex *V;
  EdgeNode *Node = Root;
  if (V2<V1) {V=V1; V1=V2; V2=V;} //Order doesn't matter
  if (!Root->E) {Root->E = E; E->A = V1; E->B = V2; E->P = E->Q = NULL; EdgeIns++; return 1;}//First edge will be inserted immidiately
  do
  {
    if (V1<Node->E->A)
    {
Ins_Left:if (Node->L) {Node = Node->L; continue;} else
      {
        Node->L = EdgeIns++;
        Node = Node->L;
        Node->E = E;
        Node->L = Node->R = NULL;
        E->A = V1;
        E->B = V2;
        E->P = E->Q = NULL;
        return 1;
      }
    }
    if (V1==Node->E->A)
    {
      if (V2<Node->E->B) goto Ins_Left;
      if (V2==Node->E->B) return 0;
    }
    if (Node->R) {Node = Node->R; continue;} else
    {
      Node->R = EdgeIns++;
      Node = Node->R;
      Node->E = E;
      Node->L = Node->R = NULL;
      E->A = V1;
      E->B = V2;
      E->P = E->Q = NULL;
      return 1;
    }
  } while(1);
}

Edge * Seek_Edge(Vertex *V1,Vertex *V2,EdgeNode *Root)
{
  EdgeNode *Node = Root;
  Vertex *V;
  if (V2<V1) {V=V1; V1=V2; V2=V;} //Order doesn't matter
  do
  {
    if (V1<Node->E->A)
    {
      GoLeft:
      Node = Node->L;
      continue;
    }
    if (V1==Node->E->A)
    {
      if (V2<Node->E->B) goto GoLeft;
      if (V2==Node->E->B) return Node->E;
    }
    Node = Node->R;
  } while(Node);
  return NULL;
}

char Insert_Poly2Edge(Face *GP,Edge *E)
{
  if (!E->P) E->P = GP;
    else if (!E->Q) E->Q = GP;
      else
      {
        FILE *F;
        F = fopen("Warnings.lst","at");
        fprintf(F,"<Geo:Tri> Edge Insertion Fault. (Tri-mesh is abnormal: it has more than two polygons attached to the same edge.)\n");
        fclose(F);
        return 0;
      }
  return 1;
}

void Delete_EdgeNode(EdgeNode *Cur)
{
  if (Cur->L) Delete_EdgeNode(Cur->L);
  if (Cur->R) Delete_EdgeNode(Cur->R);
  delete Cur;
}

// This routine imports a scene Trimesh to the geometric Trimesh module.
// Winged edge format edges should be calculated at this point
// it worked, now i can try this at work ;)
void Compute_Edges(TriMesh *T)
{
  Edge *Edgebuf,*EP,*EPE;
  EdgeNode *Nodebuf;
  long I,J;
  Face *GP,*GE;

  // Edges will now be calculated
  Edgebuf = new Edge[3*T->FIndex]; //Maximum possible edges
  memset(Edgebuf,0,3*T->FIndex*sizeof(Edge));
  EP = Edgebuf;
  Nodebuf = new EdgeNode[3*T->FIndex];
  memset(Nodebuf,0,sizeof(EdgeNode));
  EdgeIns = Nodebuf;
  for(I=0;I<T->FIndex;I++)
  {
    if (Insert_Edge(T->Faces[I].A,T->Faces[I].B,EP,Nodebuf)) {EP->Length = Distance(&T->Faces[I].A->Pos,&T->Faces[I].B->Pos); EP++;}
    if (Insert_Edge(T->Faces[I].B,T->Faces[I].C,EP,Nodebuf)) {EP->Length = Distance(&T->Faces[I].B->Pos,&T->Faces[I].C->Pos); EP++;}
    if (Insert_Edge(T->Faces[I].C,T->Faces[I].A,EP,Nodebuf)) {EP->Length = Distance(&T->Faces[I].C->Pos,&T->Faces[I].A->Pos); EP++;}
  }
  T->EIndex = EP-Edgebuf;
  T->Edges = (Edge *)realloc(Edgebuf,T->EIndex*sizeof(Edge)); //smaller memblock won't get moved

  // Edge list ready, now we only got to map the faces and grin ;)
  GP = T->Faces;
  GE = GP+T->FIndex;
  for(;GP<GE;GP++)
  {
    EP = Seek_Edge(GP->A,GP->B,Nodebuf);
    if (!Insert_Poly2Edge(GP,EP)) T->Flags|=Tri_EdgeError;
//    else GP->AB = EP;
    EP = Seek_Edge(GP->B,GP->C,Nodebuf);
    if (!Insert_Poly2Edge(GP,EP)) T->Flags|=Tri_EdgeError;
//    else GP->AC = EP;
    EP = Seek_Edge(GP->C,GP->A,Nodebuf);
    if (!Insert_Poly2Edge(GP,EP)) T->Flags|=Tri_EdgeError;
//    else GP->BC = EP;
  }
  // edge insertion complete (we can also build vtx.**edge stuff,
  // maybe some other time...)
  // check whether it is convex or concave edge.
  EP = T->Edges; EPE = EP + T->EIndex;
  for(;EP<EPE;EP++)
  {
    // look for the vertex not within the edge in the face E->P
    if (!(EP->Q)) {EP->Flags |= Edge_Concave|Edge_True; T->Flags |= Tri_Closed; continue;}
    if (EP->P->A!=EP->A&&EP->P->A!=EP->B)
    {
      if (Dot_Product(&EP->P->A->Pos,&EP->Q->N)+EPSILON<Dot_Product(&EP->Q->A->Pos,&EP->Q->N)) EP->Flags |= Edge_Concave;
    } else if (EP->P->B!=EP->A&&EP->P->B!=EP->B)
      {
        if (Dot_Product(&EP->P->B->Pos,&EP->Q->N)+EPSILON<Dot_Product(&EP->Q->A->Pos,&EP->Q->N)) EP->Flags |= Edge_Concave;
      } else {
        if (Dot_Product(&EP->P->C->Pos,&EP->Q->N)+EPSILON<Dot_Product(&EP->Q->A->Pos,&EP->Q->N)) EP->Flags |= Edge_Concave;
      }
  }

  // now we kill the tree - who cares about it
//  Delete_EdgeNode(EdgeIns);
  delete Nodebuf;
}

// Update Trimesh T to contain The following Information:
// Face plane Normals,Vertex Pseudo Normals,Face Normal products,
// Bounding Sphere,Correct UV Wrapping Flags,Lighting module validity.
void Process_TriMesh(TriMesh *T)
{
//  if (T->Flags&Tri_Processed) continue;

//  Compute_Optimized_Indices(T);
  Compute_Face_Normals(T);
  Compute_Vertex_Normals(T);
  Compute_Normal_Products(T);
  Compute_Bounding_Sphere(T);
  Compute_Lighting_Flag(T);
//  Compute_Edges(T);
//  Compute_Spikes(T);
  T->CurStat=T->Status;
  T->Flags |= Tri_Processed;
}

// Similier to Process_TriMesh,except here all Trimeshes in Scene are
// tested, and update only for the first time the function tests them.
// also calculates scene flags.
void Scene_Computations(Scene *Sc)
{
  TriMesh *T;
  Material *M;

  Sc->Flags = 0;
  for(T=Sc->TriMeshHead;T;T=T->Next)
  {
    if (T->Flags&Tri_Processed) continue;

//    Compute_Optimized_Indices(T);
    Compute_Face_Normals(T);
    Compute_Vertex_Normals(T);
    Compute_Normal_Products(T);
    Compute_Bounding_Sphere(T);
    Compute_Lighting_Flag(T);
//    Compute_Edges(T);
//    Compute_Spikes(T);

    T->Flags |= Tri_Processed;
  }
  if (BPP!=32) {Sc->Flags|=Scn_Nolighting; return;}
  Sc->Flags|=Scn_Nolighting;
  for(M=MatLib;M;M=M->Next)
  {
    if (M->RelScene!=Sc) continue;
    if (M->Flags&Mat_RGBInterp) Sc->Flags&=0xFFFFFFFF-Scn_Nolighting;
  }
}

// Face Drawing List Allocation routine.
void FList_Allocate(Scene *Sc)
{
  Polys = 0; //Counter.
  TriMesh *T;
  Omni *O;
  for(T=Sc->TriMeshHead;T;T=T->Next)
    Polys += T->FIndex;
  for(O=Sc->OmniHead;O;O=O->Next)
    Polys++;
  Polys += Sc->NumOfParticles;
  FList = new Face * [Polys];
  SList = new Face * [Polys];
}

void Init_Default_Material()
{
  Material *M = MatLib;

  if (!M) {MatLib = &Default_Mat; MatLib->Prev = NULL;}
  else {for(;M->Next;M=M->Next); M->Next = &Default_Mat;}

  Default_Mat.Next = NULL;
  Default_Mat.Name = strdup("Zol default material");
  Default_Mat.Txtr = NULL;
  Default_Mat.Flags &= 0xFFFF-Mat_Transparent;
  Default_Mat.Flags |= Mat_TwoSided;
  Default_Mat.RelScene = NULL;
//Default_Mat.Flags |= Mat_Nonconv;

}

void Init_Flare_Textures(Scene *Sc)
{
  Omni *O,*O2;
  Object *Obj;
  char *S = new char[40];
  char Already;
  char *Scan;
  Material *M;

  if (!Sc->OmniHead) return;
//  printf("<PR:IFT>: Testing Flare names for Filename format.\n");
  //First, we test the scene to check whether the Flare names really
  //represent texture filenames. If they don't,we load 'Flare.GIF' for
  //all of them.
  Already=0;
  for(Obj=Sc->ObjectHead;Obj;Obj=Obj->Next)
  {
    if (Obj->Type!=Obj_Omni) continue;
    Scan = Obj->Name;
    while (*Scan&&*Scan!=46&&*Scan<128) Scan++;
    if (*Scan>127||!(*Scan)) Already=1;
  }

  // Obviously this is not a Flood originated scene,too bad...
  if (Already)
  {
//    printf("<PR:IFT>: Flare names are not ISO filenames,\n        loading Flare.GIF for all of them.\n");
    if (MatLib) {
      for(M=MatLib;M->Next;M=M->Next);
//      printf("<PR:IFT>: New material will be linked to last entry %s",M->Name);
      M->Next = new Material;
      M->Next->Prev = M;
      M=M->Next;
      M->Next = NULL;
    } else {
//      printf("<PR:IFT>: No previous entries detected, Creating Library.\n");
      MatLib = new Material;
      M = MatLib;
      M->Next = NULL;
      M->Prev = NULL;
    }
    M->RelScene = Sc;
//    printf("<PR:IFT>: Assigning new material to Group %d.\n",M->Group);
    M->Name = strdup("Default Flare material");
    M->Txtr = new Texture;
    M->Txtr->FileName = strdup ("Textures//Flare.GIF");
    Identify_Texture(M->Txtr);

//    if (!M->Txtr->BPP) printf("<PR:IFT>: Identification Failure for Texture %s at Material %s.\n",M->Txtr->FileName,M->Name); else printf("<PR:IFT>: Texture %s was Identified correctly\n         for Material %s as a %d BPP image file.\n",M->Txtr->FileName,M->Name,M->Txtr->BPP);
    if (!M->Txtr->BPP) {delete M->Txtr; M->Txtr = NULL;}
//    M->Txtr = NULL;

    M->Flags=Mat_Virtual; // Flare mat.

    // Assign for all flares...same lousy trick will prolly work
    for(O=Sc->OmniHead;O;O=O->Next)
      O->F.Txtr = M;
    return;
  }

  // Get your gun,let's go.
/*  if (Sc->MaterialHead)
    for(M=Sc->MaterialHead;M->Next;M=M->Next); //ready on last mat.
  else
    M = NULL;

  for(O=Sc->OmniHead;O;O=O->Next)
  {
    // read be4?
    Already = 0;
    for(O2=Sc->OmniHead;O2!=O;O2=O2->Next)
      if (!stricmp(O2->Name,O->Name)) {
        Already = 1;
        O->Faces->Txtr = O2->Faces->Txtr; //copy material from be4
      }
    if (Already) continue; // no need for another material

    // okay,new texture with a new material
    if (M)
    {
      M->Next = new Material;
      M->Next->Prev = M;
      M=M->Next;
      M->Next = NULL;
    } else {
      Sc->MaterialHead = new Material;
      M = Sc->MaterialHead;
      M->Next = M->Prev = NULL;
    }
    strcpy(M->Name,O->Name);
    strcpy(S,"Textures//");
    strcat(S,O->Name);
    M->Txtr->FileName = strdup(S);
    Identify_Texture(M->Txtr);
    if (!M->Txtr->BPP) {delete M->Txtr; M->Txtr=NULL;}
    O->Faces->Txtr = M;
  }*/
}

void Init_Flares(Scene *Sc)
{
  for(Omni *O = Sc->OmniHead; O; O=O->Next)
  {
    //For use with Gouraud shading
    if (!(O->Flags&Flare_LightSource))
    {
      O->Flags|=Flare_LightSource;
      O->L.R = 0; O->L.G = 0; O->L.B = 0;
      switch (rand()*3>>15)
      {
				case 0: O->L.R = 14500.0; break;
				case 1: O->L.G = 14500.0; break;
				case 2: O->L.B = 14500.0; break;
			}
    }
    // get an image for the omnilight
    Fit_FlareImage(O);
  }
}

void Default_Texture(Scene *Sc)
{
  Material *M;
  TriMesh *T;
  Face *F,*FEnd;
  char *AlignOffs;
  char xBPP=0;

  if (MatLib)
    for(M=MatLib;M;M=M->Next) if ((M->RelScene==Sc)&&M->Txtr&&(!(M->Flags&Mat_Virtual))) xBPP|=M->Txtr->BPP;

  if (!xBPP)
  {
//    printf("No textures available...loading default Texture\n");
    if (MatLib)
    {
      for(M=MatLib;M->Next;M=M->Next);
      M->Next = new Material;
      M->Next->Prev = M;
      M = M->Next;
      M->Next = NULL;
    }
    else
    {
      MatLib = new Material;
      M = MatLib;
      M->Next = M->Prev = NULL;
    }
    M->RelScene = Sc;
    M->Name = strdup("Default Textured material");
//    M->Flags = Mat_Nonconv | Mat_RGBInterp | Mat_TwoSided | Mat_Phong;
//    M->Txtr = NULL;
    M->Flags = Mat_RGBInterp;
    M->Txtr = new Texture;
/*    Image Plasma;
    Plasma.X = Plasma.Y = 256;
    Plasma.Data = new DWord[256*256];
    memset(Plasma.Data,0,256*256*4);
    Generate_Fractal_Noise(&Plasma,0.6);
    Gamma_Correction(&Plasma,0.4);
    Convert_Image2Texture(&Plasma,M->Txtr);
    M->Flags = Mat_Nonconv | Mat_RGBInterp;*/

    M->Txtr->FileName = strdup("Textures//Default.GIF");
    M->Txtr->BPP=0;
    Load_Texture(M->Txtr);
    if (!M->Txtr->BPP) {delete M->Txtr; M->Txtr=NULL;}
/*    if (!M->Txtr.BPP)
    {
      M->Txtr.BPP = 8; // Force bits
      M->Txtr.Pal = new Palette; //Force uniform grey palette
//      memset(M->Txtr.Pal,31,sizeof(Palette));
      for(I=0;I<768;I++)
        ((char *)M->Txtr.Pal)[I]=I/3;
//      printf("Warning! Default texture cannot be found,forcing 8BPP unallocated texture\n");
    }*/
    for(T=Sc->TriMeshHead;T;T=T->Next)
    {
      FEnd = T->Faces + T->FIndex;
      // Avoid Modifying special FX to default...
      for(F=T->Faces;F<FEnd;F++)
        if ((!F->Txtr)||((!F->Txtr->Txtr)&&(!(F->Txtr->Flags&Mat_Nonconv))))
          F->Txtr = M;
    }
  }
}

// Remaps all Textures in Material Library into an Aligned Buffer.
// Requires Plenty of memory for Swap space
// Exclusively allocates the DPMI's Aligned Buffer, this sux shit because
// other buffers maybe require aligned shit.
/*void Materials_Remap_Aligned()
{
/*  Material *M;
  TriMesh *T;
  Face *F,*FEnd;
  char *AlignOffs;
  char xBPP=0;
  long I,J;

  // load txtr by default,if no solid materials really available

  // Measure required Aligned-Buffer space. first 64K for Align Slack.
  for(M=Sc->MaterialHead;M;M=M->Next)
    if (M->Txtr&&(!(M->Flags&Mat_Nonconv))) //SizeofBlock+=65536*(M->Txtr.BPP+1>>3);
      SizeofBlock+=65536;

  printf("Allocating %d bytes for Aligned space.\n",SizeofBlock);
//  getchar();
  // Allocating+Aligning Buffer
  if (SizeofBlock==65536) return;
  TxtrBuf = new char[SizeofBlock];
  if ((unsigned int)TxtrBuf&0xFFFF) TxtrAlign=TxtrBuf+65536-((unsigned int)TxtrBuf&0xFFFF);
    else TxtrAlign=TxtrBuf;

  // Remapping Textures.
  AlignOffs = TxtrAlign;

  for(M=Sc->MaterialHead;M;M=M->Next)
  {
    if (M->Txtr&&(!(M->Flags&Mat_Nonconv))) continue;
    memcpy(AlignOffs,M->Txtr->Data,65536);
    delete M->Txtr->Data;
    M->Txtr->Data = AlignOffs;
    printf("Texture space for %s at %xh.\n",M->Txtr->FileName,AlignOffs);
    AlignOffs+=65536;
  }
}*/

void Materials_Remap_Aligned()
{
  Material *M;
  TriMesh *T;
  Face *F,*FEnd;
  long NumOfMats = 0;
  char *NB;

  // load txtr by default,if no solid materials really available

  // Measure required Aligned-Buffer space. first 64K for Align Slack.
  for(M=MatLib;M;M=M->Next)
    if (M->Txtr) NumOfMats++;
  if (!NumOfMats) return;
//  printf("<PR:Aln>: Allocating Aligned buffer for %d Materials.\n",NumOfMats);

  Init_Aligned(NumOfMats);
  for(M=MatLib;M;M=M->Next)
    if (M->Txtr)
    {
      NB = Get_Aligned_Block();
      memcpy(NB,M->Txtr->Data,65536);
      delete M->Txtr->Data;
      M->Txtr->Data = NB;
//      printf("<PR:Aln>: Allocating Aligned block for Material %s.\n",M->Name);
    }

  // Allocating+Aligning Buffer
/*  TxtrBuf = new char[SizeofBlock];
  if ((unsigned int)TxtrBuf&0xFFFF) TxtrAlign=TxtrBuf+65536-((unsigned int)TxtrBuf&0xFFFF);
    else TxtrAlign=TxtrBuf;

  // Remapping Textures.
  AlignOffs = TxtrAlign;

  for(M=Sc->MaterialHead;M;M=M->Next)
  {
    if (M->Txtr&&(!(M->Flags&Mat_Nonconv))) continue;
    memcpy(AlignOffs,M->Txtr->Data,65536);
    delete M->Txtr->Data;
    M->Txtr->Data = AlignOffs;
    printf("Texture space for %s at %xh.\n",M->Txtr->FileName,AlignOffs);
    AlignOffs+=65536;
  }*/
  for(M=MatLib;M;M=M->Next)
  {
    if (!M->Txtr) continue;
    printf("<PR:Aln>: Material %s ===> Address %xH.\n",M->Name,(DWord)M->Txtr->Data);
  }
//  getchar();
}

#define krintf printf

void Materials_Load(Scene *Sc)
{
  Material *M,*FM;
  TriMesh *T;
  Omni *O;
  Face *F,*FEnd;

//  printf("Loading Materials, Please wait\n");
//  printf("Matlib = %xh",(long)MatLib);
  for(M=MatLib;M;M=M->Next)
  {
//    if (M->Group!=Group) printf("ALERT!! GROUP MISMATCH!! (S%d!=M%d) (MATERIAL %s)\n",Group,M->Group,M->Name);
//    if (!(M->Txtr)) printf("NO TEXTURE TO LOAD FOR MATERIAL %s\n",M->Name);
//    if (M->Flags&Mat_Nonconv) printf("NON-CONVENTIONAL MATERIAL %s - ignored\n",M->Name);
    if ((M->Txtr)&&(M->Txtr->Data))
    {
      //krintf("ALREADY LOADED, YOU PIECE OF SHIT (%s)\n",M->Name);
      continue;
    }
    if (M->RelScene==Sc&&M->Txtr&&(!(M->Flags&Mat_Nonconv)))
    {
      if (!Load_Texture(M->Txtr)) printf("Warning! Texture for Material %s could not be read from file %s\n",M->Name,M->Txtr->FileName);
      //else printf("Loaded Texture OK (%s for %s)\n",M->Txtr->FileName,M->Name);
    }
  }

//  printf("All textures loaded...\n");
//  fflush(stdout);
  // Remap faces to Textures (recover from lousy trick state :)
  // Get first valid material.
  for(FM=MatLib;FM&&(FM->RelScene!=Sc||(!FM->Txtr));FM=FM->Next);
//  printf("Crap");
//  fflush(stdout);
//  if ((!FM)||(!FM->Txtr)) FM = &Default_Mat;
  if (!FM) FM = &Default_Mat;

  for(T=Sc->TriMeshHead;T;T=T->Next)
  {
    FEnd = T->Faces+T->FIndex;
    for(F=T->Faces;F<FEnd;F++)
    {
      M = F->Txtr;
      if (M) F->Txtr = M; else F->Txtr = FM;
    }
  }
  for(O=Sc->OmniHead;O;O=O->Next)
  {
    F = &O->F;
    M = F->Txtr;
    if (M&&M->Txtr) F->Txtr = M; else F->Txtr = FM;
  }
}


void Assign_Fillers(Scene *Sc)
{
  TriMesh *T;
  Omni *O;
  Face *F,*FEnd;

  for(T=Sc->TriMeshHead;T;T=T->Next)
  {
    // this should check on ALL materials associated with the trimesh.
    if (BPP!=32&&T->Faces->Txtr->Txtr)
    {
      T->Flags|=Tri_Noshading;
      T->Faces->Txtr->Flags&=0xFFFFFFFF-Mat_RGBInterp;
    } //get more gouraud mappers,kid.
    FEnd = T->Faces+T->FIndex;
    for(F=T->Faces;F<FEnd;F++)
#ifdef SpanBuffer
      F->Filler = &DrawTextureTriangle;
#else
      switch(BPP)
      {
        case 8:
          if (F->Txtr->Txtr)
          {
            F->Filler = &The_Bilinear;
            F->Clipper = &Frust_Set_UV;
          }
          else
          {
            F->Filler = &Flat_Poly_8Bit;
            F->Clipper = &Frust_Set_Flat;
          }
          break;
        case 15:
          if (F->Txtr->Txtr)
          {
            F->Filler = &The_BiHi;
            F->Clipper = &Frust_Set_UV;
          }
          else
          {
            F->Filler = &Flat_Poly_ManyBits;
						F->Clipper = &Frust_Set_Flat;
					}
					break;
				case 16:
					if (F->Txtr->Txtr)
					{
						F->Filler = &The_BiHi;
						F->Clipper = &Frust_Set_UV;
          }
          else
          {
            F->Filler = &Flat_Poly_ManyBits;
            F->Clipper = &Frust_Set_Flat;
          }
        break;
        case 32:
          if (F->Txtr->Txtr)
          {
            if (T->Flags&Tri_Phong)
            {
              F->Clipper = &Frust_Set_Env;
              if (F->Txtr->Flags&Mat_Transparent)
                F->Filler = &The_BiPTTrue;
              else
                F->Filler = &The_BiPTrue;
            } else {
              F->Clipper = &Frust_Set_UV;
              if (F->Txtr->Flags&Mat_Transparent)
                F->Filler = &The_BiTTrue;
              else
              {
                if (F->Txtr->Flags&Mat_RGBInterp)
                {
                  if (MMXState)
                    F->Filler = &The_BiGMMXTrue;
                  else
                    F->Filler = &The_BiGTrue;

                  F->Clipper = &Frust_Set_UVL;
                }
                else
                  F->Filler = &The_BiTrue;
              }
            }
          } else {
            if (F->Txtr->Flags&Mat_RGBInterp)
            {
              if (F->Txtr->Flags&Mat_Transparent)
              {
                F->Filler = &The_Specular_TGouraud;
                F->Clipper = &Frust_Set_L;
              }
              else
              {
                if (MMXState)
                  F->Filler = &The_MMX_Gouraud;
                else
                  F->Filler = &The_Specular_Gouraud;

/*                if (T->Flags&Tri_Phong)
                  F->Clipper = &Trivex_Frustrum_NTEPerspective;
                else*/
                  F->Clipper = &Frust_Set_L;
              }
            } else {
              F->Filler = &Flat_Poly_ManyBits;
              F->Clipper = &Frust_Set_Flat;
            }
          }
      }
#endif
  }

  for(O=Sc->OmniHead;O;O=O->Next)
  {
    switch (BPP)
    {
      case 8:  O->F.Filler = &Dark_Sprite; break; // Non-Addative
      case 15: O->F.Filler = &Spirit_16; break;   // Unsupported
      case 16: O->F.Filler = &Spirit_16; break;   // Unsupported
      case 32: if (MMXState) O->F.Filler = &The_MMX_Scalar; else O->F.Filler = &The_Seven_UP; break;
    }
  }
}

void Force_Default_Mat(Scene *Sc)
{
  TriMesh *T;
  long I;
  for(T=Sc->TriMeshHead;T;T=T->Next)
    for(I=0;I<T->FIndex;I++)
 /*     if (!T->Faces[I].Txtr) */T->Faces[I].Txtr = &Default_Mat;
}

void Preprocess_Scene(Scene *Sc)
{
//  printf("<PR> Preprocessing Scene #%d\n",EnumScene(Sc));
  Init_Flares(Sc);
  Materials_Load(Sc); //load ALL textures sequently.
//  Force_Default_Mat(Sc);
  Default_Texture(Sc);
  Unify_Textures(Sc);
  Scene_Computations(Sc);
//  if (BPP==8) Materials_Remap_Aligned(Sc);
  Assign_Fillers(Sc);
  Restore_Splines(Sc);
}


static Object **ObjSort,**ObjPtr;
static Scene *HTSc;
static long Objs;

void Hierarchy_TreeSort(Object *Node)
{
  *ObjPtr++ = Node; //Insert to List
  Object *Obj;
  for(Obj = HTSc->ObjectHead; Obj ; Obj=Obj->Next)
    if (Obj->Parent == Node)
      Hierarchy_TreeSort(Obj);
}

void Hierarchy_Sort(Scene *Sc)
{
  long I;

  HTSc = Sc;
  // Count Objects
  Objs = 0;
  Object *Obj;
  for(Obj = Sc->ObjectHead; Obj ; Obj=Obj->Next)
    Objs++;

  // Allocate Pointer Array
  ObjSort = new Object * [Objs];
  ObjPtr = ObjSort;

  for(Obj = Sc->ObjectHead; Obj ; Obj=Obj->Next)
    if (!Obj->Parent) Hierarchy_TreeSort(Obj);

  Sc->ObjectHead = ObjSort[0];

  ObjSort[0]->Prev = NULL; ObjSort[0]->Next = ObjSort[1];
  for(I=1;I<Objs-1;I++)
  {
    ObjSort[I]->Prev = ObjSort[I-1];
    ObjSort[I]->Next = ObjSort[I+1];
  }
  ObjSort[I]->Prev = ObjSort[I-1]; ObjSort[I]->Next = NULL;

  delete ObjSort;
}