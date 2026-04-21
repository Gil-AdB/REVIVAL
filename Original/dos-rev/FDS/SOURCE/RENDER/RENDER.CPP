#include <StdIO.H>
#include <String.H>
#include <Math.H>
#include <StdLib.H>
#include <Memory.H>
#include <String.H>
#include <ConIO.H>
#include <i86.H>
#include <DOS.H>
#include <Time.H>

#include "..\FDS_Defs.H"
#include "..\FDS_Vars.H"
#include "..\FDS_Decs.H"

Scene *CurScene;
Face **FList;
Face **SList;
Face *DoFace; // mar gamish, ta'ase partsuf!
long CPolys,COmnies,CPcls,CAll,Polys;
char *TxtrBuf,*TxtrAlign;
float Exec_FPS;
Material Default_Mat;
unsigned short int OldFPUCW,FPUCW;
char **MsgStr;
long *MsgClock;
DWord *MsgID;
char Cam_HeadLight;
float CurFrame;

long Frames;
float dTime;

Texture Phong_Mapping;
Font *Font1;
char G_String[256];

VESA_Surface Layer2;



#include "General.H"
#include "Sorts.H"

// RULEZ
#pragma aux DO_NOP="NOP";

void Animate_Objects(Scene *Sc)
{
  TriMesh *T;
  Omni *Om;
  Object *Obj;
  Vector U,*W;
  Matrix M;
  FILE *F;

//  F = fopen("Matrix.txt","at");
  for (T=Sc->TriMeshHead;T;T=T->Next)
  {
    if (T->Flags&Tri_Possessed) continue;
    Spline_Calc_3D(&T->Pos,CurFrame,&T->IPos);
    Spline_Calc_3D(&T->Scale,CurFrame,&T->IScale);
//    Vector_Form(&T->IScale,1,1,1); // until i get it right

    if (T->Flags&Tri_Euler)
    {
      Spline_Calc_3D(&T->Rotate,CurFrame,&U);
      Euler_Angles(T->RotMat,U.X,U.Y,U.Z);
    } else {
      Spline_Calc_4D_Alt(&T->Rotate,CurFrame,&T->IRot);
      //Spline_Subdivide_Bezier(&T->Rotate,CurFrame,&T->IRot);
  //    Spline_Calc_4D(&T->Rotate,CurFrame,&T->IRot);

      Convert_Quat2Mat(&T->IRot,T->RotMat);
  //    fprintf(F,"%d:((%1.3f,%1.3f,%1.3f),(%1.3f,%1.3f,%1.3f),(%1.3f,%1.3f,%1.3f))\n\n",(long)CurFrame,T->RotMat[0][0],T->RotMat[0][1],T->RotMat[0][2],T->RotMat[1][0],T->RotMat[1][1],T->RotMat[1][2],T->RotMat[2][0],T->RotMat[2][1],T->RotMat[2][2]);
    }

    W = (Vector *)T->RotMat;
    Vector_SelfScale(W,T->IScale.X); W++;
    Vector_SelfScale(W,T->IScale.Y); W++;
    Vector_SelfScale(W,T->IScale.Z); W++;
/*    if (T->Status)
    {
      if (CurFrame>T->CurStat->Frame)
      T->CurStat=T->CurStat->Next;
      T->Flags&=0xFFFFFFFF-HTrack_Visible;
      T->Flags|=T->CurStat->Stat;
    } else*/ T->Flags|=HTrack_Visible;
  }
//  fclose(F);
  for(Om=Sc->OmniHead;Om;Om=Om->Next)
  {
    Spline_Calc_3D(&Om->Pos,CurFrame,&Om->IPos);
    //Spline_Calc_3D(&Om->Pos,CurFrame,&Om->Col);
  }
  if (View!=&FC)
  {
    Spline_Calc_3D(&View->Source,CurFrame,&View->ISource);
    Spline_Calc_3D(&View->Target,CurFrame,&View->ITarget);
    Spline_Calc_1D(&View->Roll,CurFrame,&View->IRoll);
    Spline_Calc_1D(&View->FOV,CurFrame,&View->IFOV);
    if (View->Flags&Cam_Euler)
      Euler_Angles(View->Mat,View->ITarget.Y,View->ITarget.X,-View->ITarget.Z);
    else
      Kick_Camera(&View->ISource,&View->ITarget,View->IRoll,View->Mat);
  }

  CalcPersp(View);

  FOVX=View->PerspX;
  FOVY=View->PerspY;

  // lalala, HARARCHIA , Ver 3, it now rulati
  for (Obj=Sc->ObjectHead;Obj;Obj=Obj->Next)
  {
    MatrixXVector(*Obj->Rot,&Obj->Pivot,&U);
    Vector_SelfSub(Obj->Pos,&U);
    if (Obj->Parent)
    {
      MatrixXVector(*Obj->Parent->Rot,Obj->Pos,&U);
      Vector_Add(Obj->Parent->Pos,&U,Obj->Pos);
      MatrixXMatrix(*Obj->Parent->Rot,*Obj->Rot,M);
      Matrix_Copy(*Obj->Rot,M);
    }
  }
}

void Vertex_Loop1(Vertex *Vert,Vertex *VEnd,Matrix M,Vector *V)
{
  Vertex *Vtx;
  float *f = (float *)M;
  Vector U;
  for (Vtx=Vert;Vtx<VEnd;Vtx++)
  {
//    if (!Vtx->FRem) continue;
//    MatrixXVector(M,&Vtx->Pos,&U);
//    Vector_Add(&U,V,&Vtx->TPos);
//    Vtx->TPos.X = (*f++)*Vtx->Pos.X+(*f++)*Vtx->Pos.Y+(*f++)*Vtx->Pos.Z+V->X;
//    Vtx->TPos.Y = (*f++)*Vtx->Pos.X+(*f++)*Vtx->Pos.Y+(*f++)*Vtx->Pos.Z+V->Y;
//    Vtx->TPos.Z = (*f++)*Vtx->Pos.X+(*f++)*Vtx->Pos.Y+(*f)*Vtx->Pos.Z+V->Z;
//    f-=8;
    MatrixXVector(M,&Vtx->Pos,&U);
    Vector_Add(&U,V,&Vtx->TPos);


    Vtx->RZ=1.0/Vtx->TPos.Z;
    Vtx->PX=Vtx->TPos.X*Vtx->RZ;
    Vtx->PY=Vtx->TPos.Y*Vtx->RZ;
//    Vtx->RU=Vtx->U*Vtx->RZ;
//    Vtx->RV=Vtx->V*Vtx->RZ;
    Vtx->Flags&=0xFFFFFFFF-Vtx_Visible;
    if (Vtx->PX<0) Vtx->Flags|=Vtx_VisLeft;
    if (Vtx->PX>XRes_1) Vtx->Flags|=Vtx_VisRight;
    if (Vtx->PY<0) Vtx->Flags|=Vtx_VisUp;
    if (Vtx->PY>YRes_1) Vtx->Flags|=Vtx_VisDown;
  }
}

void Transform_Objects(Scene *Sc)
{
  TriMesh *T;
  Omni *O;
  Matrix M,IM;
  float M34[3][4];
  Vector AP,S,U,V,*W=(Vector *)(&M),*W2,*Scl;
  float L1,L2,L3;
  Vertex *Vtx,*VEnd;
  Face *F,*FEnd;
  float PX=FOVX,PY=FOVY,Temp;
  float dz;
  long *pdz = (long *)(&dz);
  long I;
  Face **Ins = FList;
  float *f = (float *)(&M);
  float *fv;

  for (T=Sc->TriMeshHead;T;T=T->Next)
  {
    if (!T->Flags&HTrack_Visible) {T->Flags|=Tri_Invisible; continue;}
    MatrixXMatrix(View->Mat,T->RotMat,M);
    Matrix_Copy(IM,M);
    // Advanced Matrix...(watch this)
    Vector_Scale(W,PX,W);
    Vector_Scale(W+1,-PY,W+1);
    Vector_Scale(W+2,CntrEX,&V);
    Vector_SelfAdd(W,&V);
    Vector_Scale(W+2,CntrEY,&V);
    Vector_SelfAdd(W+1,&V);
    // Supermatrix ready.

    // postrioric Offset Vector.
    Vector_Sub(&T->IPos,&View->ISource,&U);
    MatrixXVector(View->Mat,&U,&S);
    V.X = CntrEX*S.Z+PX*S.X;
    V.Y = CntrEY*S.Z-PY*S.Y;
    V.Z = S.Z;
//    Vector_Copy(&V,&S);
    // ready
    // 4x3 AFFINE XFORM
    M34[0][0] = M[0][0]; M34[0][1] = M[0][1]; M34[0][2] = M[0][2]; M34[0][3] = V.X;
    M34[1][0] = M[1][0]; M34[1][1] = M[1][1]; M34[1][2] = M[1][2]; M34[1][3] = V.Y;
    M34[2][0] = M[2][0]; M34[2][1] = M[2][1]; M34[2][2] = M[2][2]; M34[2][3] = V.Z;
    // ready


    // aprioric Offset Vector.
    MatrixTXVector(T->RotMat,&U,&AP);
    // ready
    // Bounding Sphere Elimination test Begins.
    W2 = (Vector *)(&T->RotMat);
    L2 = Dot_Product(W2,W2);
    if ((L1 = Dot_Product(W2+1,W2+1))>L2) L2=L1;
    if ((L1 = Dot_Product(W2+2,W2+2))>L2) L2=L1;

    T->Flags&=0xFFFFFFFF-Tri_Invisible-Tri_Ahead-Tri_Inside;
    // Out by depth
    if (S.Z*S.Z>L2*T->BSphereRad)
    {
      if (S.Z<0)
      {
        T->Flags |= Tri_Invisible;
        continue;
      }
      T->Flags |= Tri_Ahead;
    }
    // Out by left/right
    S.X=fabs(S.X);
    L1 = PX*S.X - CntrEX*S.Z;
    if (L1*L1>L2*T->BSphereRad*(PX*PX+CntrEX*CntrEX))
    {
      if (S.X*PX>S.Z*CntrEX)
      {
        T->Flags |= Tri_Invisible;
        continue;
      }
      if (T->Flags&Tri_Ahead) T->Flags |= Tri_Inside;
    }
    // Out by up/down
    S.Y = fabs(S.Y);
    L1 = PY*S.Y - CntrEY*S.Z;
    if (L1*L1>L2*T->BSphereRad*(PY*PY+CntrEY*CntrEY))
    {
      if (S.Y*PY>S.Z*CntrEY)
      {
        T->Flags |= Tri_Invisible;
        continue;
      }
    } else T->Flags&=0xFFFFFFFF-Tri_Inside;
    VEnd=T->Verts+T->VIndex;

/*    FEnd=T->Face+T->NumOfFaces;
    for (F=T->Face;F<FEnd;F++)
      if (!(F->Txtr->Flags&Mat_TwoSided))
        F->Flags = (AP.X*F->N.X + AP.Y*F->N.Y + AP.Z*F->N.Z>=F->NormProd);*/


//    Main vertex loop,in case no restrictions apply.
    if (!(T->Flags&Tri_Phong))
    {
      if (!(T->Flags&Tri_Inside))
      {
        if (!(T->Flags&Tri_Ahead))
          goto Regular;
        else goto Ahead;
      }
      // Intel inside...this rulez,all object completely inside frustrum.
      for (Vtx=T->Verts;Vtx<VEnd;Vtx++)
      {
//        MatrixXVector(M,&Vtx->Pos,&U);
//        Vector_Add(&U,&V,&Vtx->TPos);
        // 4x3 xform
        Vtx->TPos.X = M34[0][0]*Vtx->Pos.X+M34[0][1]*Vtx->Pos.Y+M34[0][2]*Vtx->Pos.Z+M34[0][3];
        Vtx->TPos.Y = M34[1][0]*Vtx->Pos.X+M34[1][1]*Vtx->Pos.Y+M34[1][2]*Vtx->Pos.Z+M34[1][3];
        Vtx->TPos.Z = M34[2][0]*Vtx->Pos.X+M34[2][1]*Vtx->Pos.Y+M34[2][2]*Vtx->Pos.Z+M34[2][3];

        Vtx->Flags&=0xFFFFFFFF-Vtx_Visible;
        Vtx->RZ=1.0/Vtx->TPos.Z;
        Vtx->PX=Vtx->TPos.X*Vtx->RZ;
        Vtx->PY=Vtx->TPos.Y*Vtx->RZ;
//        Vtx->PX=CntrEX+PX*Vtx->TPos.X*Vtx->RZ;
//        Vtx->PY=CntrEY-PY*Vtx->TPos.Y*Vtx->RZ;
//        Vtx->RU=Vtx->U*Vtx->RZ;
//        Vtx->RV=Vtx->V*Vtx->RZ;
      }
      goto OUT;
      // This is in case 100% of trimesh AHEAD of camera. this saves some chks
      Ahead://Vertex_Loop1(T->Vertex,VEnd,M,&V);
      for (Vtx=T->Verts;Vtx<VEnd;Vtx++)
      {
  //    if (!Vtx->FRem) continue;
//        MatrixXVector(M,&Vtx->Pos,&U);
//        Vector_Add(&U,&V,&Vtx->TPos);
        Vtx->TPos.X = M34[0][0]*Vtx->Pos.X+M34[0][1]*Vtx->Pos.Y+M34[0][2]*Vtx->Pos.Z+M34[0][3];
        Vtx->TPos.Y = M34[1][0]*Vtx->Pos.X+M34[1][1]*Vtx->Pos.Y+M34[1][2]*Vtx->Pos.Z+M34[1][3];
        Vtx->TPos.Z = M34[2][0]*Vtx->Pos.X+M34[2][1]*Vtx->Pos.Y+M34[2][2]*Vtx->Pos.Z+M34[2][3];

        Vtx->RZ=1.0/Vtx->TPos.Z;
        Vtx->PX=Vtx->TPos.X*Vtx->RZ;
        Vtx->PY=Vtx->TPos.Y*Vtx->RZ;
//        Vtx->PX=CntrEX+PX*Vtx->TPos.X*Vtx->RZ;
//        Vtx->PY=CntrEY-PY*Vtx->TPos.Y*Vtx->RZ;
//        Vtx->RU=Vtx->U*Vtx->RZ;
//        Vtx->RV=Vtx->V*Vtx->RZ;
        Vtx->Flags&=0xFFFFFFFF-Vtx_Visible;
        if (Vtx->PX<0) Vtx->Flags|=Vtx_VisLeft;
        if (Vtx->PX>XRes_1) Vtx->Flags|=Vtx_VisRight;
        if (Vtx->PY<0) Vtx->Flags|=Vtx_VisUp;
        if (Vtx->PY>YRes_1) Vtx->Flags|=Vtx_VisDown;
      }
  //    printf("Ahead VGA/Wizard.\n");
      goto OUT;
      Regular:
      for (Vtx=T->Verts;Vtx<VEnd;Vtx++)
      {
  //    if (!Vtx->FRem) continue;
//        MatrixXVector(M,&Vtx->Pos,&U);
//        Vector_Add(&U,&V,&Vtx->TPos);
        Vtx->TPos.X = M34[0][0]*Vtx->Pos.X+M34[0][1]*Vtx->Pos.Y+M34[0][2]*Vtx->Pos.Z+M34[0][3];
        Vtx->TPos.Y = M34[1][0]*Vtx->Pos.X+M34[1][1]*Vtx->Pos.Y+M34[1][2]*Vtx->Pos.Z+M34[1][3];
        Vtx->TPos.Z = M34[2][0]*Vtx->Pos.X+M34[2][1]*Vtx->Pos.Y+M34[2][2]*Vtx->Pos.Z+M34[2][3];

        Vtx->Flags&=0xFFFFFFFF-Vtx_Visible;
  //      if (*(long *)(&Vtx->TPos.Z)>0x3F800000) // 1.0 in floating point rep.
        if (Vtx->TPos.Z>1.0)
        {
          Vtx->RZ=1.0/Vtx->TPos.Z;
          Vtx->PX=Vtx->TPos.X*Vtx->RZ;
          Vtx->PY=Vtx->TPos.Y*Vtx->RZ;
//          Vtx->PX=CntrEX+PX*Vtx->TPos.X*Vtx->RZ;
//          Vtx->PY=CntrEY-PY*Vtx->TPos.Y*Vtx->RZ;
//          Vtx->RU=Vtx->U*Vtx->RZ;
//          Vtx->RV=Vtx->V*Vtx->RZ;
          if (Vtx->PX<0) Vtx->Flags|=Vtx_VisLeft;
          if (Vtx->PX>XRes_1) Vtx->Flags|=Vtx_VisRight;
          if (Vtx->PY<0) Vtx->Flags|=Vtx_VisUp;
          if (Vtx->PY>YRes_1) Vtx->Flags|=Vtx_VisDown;
        } else Vtx->Flags|=Vtx_VisNear;
  //      printf("Regular shit!\n");
      }
    } else {
      // instead of all of these complications, I've decided to
      // make the face have void (*Clipper), that will do whatever it needs
      // in one call. the pre-filler will call the asm rasterizers twice
      // if necessary. back to the good old Avatar engine techniques ;)
      // at this section, the code also calculates environment mapping
      // coordinates to (EU,EV) by rotating the v. normals accordingly.
      if (!(T->Flags&Tri_Inside))
      {
        if (!(T->Flags&Tri_Ahead))
          goto ERegular;
        else goto EAhead;
      }
      // Intel inside...this rulez,all object completely inside frustrum.
      for (Vtx=T->Verts;Vtx<VEnd;Vtx++)
      {
        MatrixXVector(M,&Vtx->Pos,&U);
        Vector_Add(&U,&V,&Vtx->TPos);

        Vtx->Flags=0;
        Vtx->RZ=1.0/Vtx->TPos.Z;
        Vtx->PX=Vtx->TPos.X*Vtx->RZ;
        Vtx->PY=Vtx->TPos.Y*Vtx->RZ;
//        Vtx->PX=CntrEX+PX*Vtx->TPos.X*Vtx->RZ;
//        Vtx->PY=CntrEY-PY*Vtx->TPos.Y*Vtx->RZ;
        Vtx->EU=128.0+127.0*(Vtx->N.X*IM[0][0]+Vtx->N.Y*IM[0][1]+Vtx->N.Z*IM[0][2]);
        Vtx->REU=Vtx->EU*Vtx->RZ;
        Vtx->EV=128.0+127.0*(Vtx->N.X*IM[1][0]+Vtx->N.Y*IM[1][1]+Vtx->N.Z*IM[1][2]);
        Vtx->REV=Vtx->EV*Vtx->RZ;
//        Vtx->RU=Vtx->U*Vtx->RZ;
//        Vtx->RV=Vtx->V*Vtx->RZ;
      }
      goto OUT;
      // This is in case 100% of trimesh AHEAD of camera. this saves some chks
      EAhead://Vertex_Loop1(T->Vertex,VEnd,M,&V);
      for (Vtx=T->Verts;Vtx<VEnd;Vtx++)
      {
  //    if (!Vtx->FRem) continue;
        MatrixXVector(M,&Vtx->Pos,&U);
        Vector_Add(&U,&V,&Vtx->TPos);

        Vtx->RZ=1.0/Vtx->TPos.Z;
        Vtx->EU=128.0+127.0*(Vtx->N.X*IM[0][0]+Vtx->N.Y*IM[0][1]+Vtx->N.Z*IM[0][2]);
        Vtx->REU=Vtx->EU*Vtx->RZ;
        Vtx->EV=128.0+127.0*(Vtx->N.X*IM[1][0]+Vtx->N.Y*IM[1][1]+Vtx->N.Z*IM[1][2]);
        Vtx->REV=Vtx->EV*Vtx->RZ;

        Vtx->PX=Vtx->TPos.X*Vtx->RZ;
        Vtx->PY=Vtx->TPos.Y*Vtx->RZ;
//        Vtx->PX=CntrEX+PX*Vtx->TPos.X*Vtx->RZ;
//        Vtx->PY=CntrEY-PY*Vtx->TPos.Y*Vtx->RZ;
//        Vtx->RU=Vtx->U*Vtx->RZ;
//        Vtx->RV=Vtx->V*Vtx->RZ;
        if (Vtx->PX<0) Vtx->Flags=Vtx_VisLeft; else Vtx->Flags=0;
        if (Vtx->PX>XRes_1) Vtx->Flags+=Vtx_VisRight;
        if (Vtx->PY<0) Vtx->Flags+=Vtx_VisUp;
        if (Vtx->PY>YRes_1) Vtx->Flags+=Vtx_VisDown;
      }
  //    printf("Ahead VGA/Wizard.\n");
      goto OUT;
      ERegular:
      for (Vtx=T->Verts;Vtx<VEnd;Vtx++)
      {
  //    if (!Vtx->FRem) continue;
        MatrixXVector(M,&Vtx->Pos,&U);
        Vector_Add(&U,&V,&Vtx->TPos);

        Vtx->Flags = 0;
  //      if (*(long *)(&Vtx->TPos.Z)>0x3F800000) // 1.0 in floating point rep.
        Vtx->EU=128.0+127.0*(Vtx->N.X*IM[0][0]+Vtx->N.Y*IM[0][1]+Vtx->N.Z*IM[0][2]);
        Vtx->EV=128.0+127.0*(Vtx->N.X*IM[1][0]+Vtx->N.Y*IM[1][1]+Vtx->N.Z*IM[1][2]);

        if (Vtx->TPos.Z>1.0)
        {
          Vtx->RZ=1.0/Vtx->TPos.Z;
          Vtx->PX=Vtx->TPos.X*Vtx->RZ;
          Vtx->PY=Vtx->TPos.Y*Vtx->RZ;
//          Vtx->PX=CntrEX+PX*Vtx->TPos.X*Vtx->RZ;
//          Vtx->PY=CntrEY-PY*Vtx->TPos.Y*Vtx->RZ;
//          Vtx->RU=Vtx->U*Vtx->RZ;
//          Vtx->RV=Vtx->V*Vtx->RZ;
          Vtx->REU=Vtx->EU*Vtx->RZ;
          Vtx->REV=Vtx->EV*Vtx->RZ;
          if (Vtx->PX<0) Vtx->Flags=Vtx_VisLeft;
          if (Vtx->PX>XRes_1) Vtx->Flags+=Vtx_VisRight;
          if (Vtx->PY<0) Vtx->Flags+=Vtx_VisUp;
          if (Vtx->PY>YRes_1) Vtx->Flags+=Vtx_VisDown;
        } else Vtx->Flags=Vtx_VisNear;
  //      printf("Regular shit!\n");
      }

    }
    OUT:FEnd=T->Faces+T->FIndex;
    for (F=T->Faces;F<FEnd;F++)
      if ((!(F->A->Flags&F->B->Flags&F->C->Flags&Vtx_Visible))
           &&((F->Txtr->Flags&Mat_TwoSided)||(AP.X*F->N.X + AP.Y*F->N.Y + AP.Z*F->N.Z<F->NormProd)))
      {
        *Ins++ = F;
        dz = F->A->TPos.Z;
//        if (*(long *)(&F->B->TPos.Z)>*pdz) dz=F->B->TPos.Z;
//        if (*(long *)(&F->C->TPos.Z)>*pdz) dz=F->C->TPos.Z;
        if (F->B->TPos.Z>dz) dz=F->B->TPos.Z;
        if (F->C->TPos.Z>dz) dz=F->C->TPos.Z;
//        dz*=-16384;
//        dz+=0x7FFFFFFF;
//        RoundToInt(&F->SortZ.DW,dz);
        F->SortZ.F = 100000.0-dz;
      }
  }
  CPolys = Ins-FList;
  for(O=Sc->OmniHead;O;O=O->Next)
  {
    Vtx=&O->V;
    Vector_Sub(&O->IPos,&View->ISource,&V);
    MatrixXVector(View->Mat,&V,&Vtx->TPos);
    if (Vtx->TPos.Z>1)
    {
      Vtx->RZ=1.0/Vtx->TPos.Z;
      Vtx->PX=CntrEX+Vtx->TPos.X*PX*Vtx->RZ;
      Vtx->PY=CntrEY-Vtx->TPos.Y*PY*Vtx->RZ;
      // Insert to List
      dz = Vtx->TPos.Z;
      //dz *=-16384;
      //dz +=0x7FFFFFFF;
      //RoundToInt(&O->Face.SortZ.DW,dz);
      O->F.SortZ.F = 100000.0-dz;
      *Ins++ = &O->F;
    }
  }
  COmnies = (Ins-FList)-CPolys;
  for(I=0;I<Sc->NumOfParticles;I++)
    if ((dz=Sc->Pcl[I].V.TPos.Z)>=1.0) {
      F = &Sc->Pcl[I].F;
      //dz *=-16384;
      //dz +=0x7FFFFFFF;
      //RoundToInt(&F->SortZ.DW,dz);
      F->SortZ.F = 100000.0-dz;
      *Ins++ = F;
    }
  CAll = Ins-FList;
  CPcls = CAll-COmnies-CPolys;
}

float Vector_CosAngleFAST(Vector *U,Vector *V)
{
  return (U->X*V->X+U->Y*V->Y+U->Z*V->Z)*
          RSQRT((U->X*U->X+U->Y*U->Y+U->Z*U->Z)*(V->X*V->X+V->Y*V->Y+V->Z*V->Z));
}



// Able to operate before Transformation Module
void Lighting(Scene *Sc)
{
	TriMesh *T;
	Vertex *Vtx,*VEnd;
	Face *Face,*FEnd;
	Omni *O;
	float R,G,B,Dot,Dot2,Its,Color,Distance;
	const float Ka=31.0,Kd=768.0,Ks=0.0,n=4.0;
	Vector L,U,V,W,TN,CU,CV;
	Matrix M;
	float rLen,Len,Len2;

	for(T=Sc->TriMeshHead;T;T=T->Next)
	{
		if (T->Flags&Tri_Invisible) continue;
		if (T->Flags&Tri_Noshading) continue;

		VEnd=T->Verts+T->VIndex;
    FEnd=T->Faces+T->FIndex;
		Vector_Sub(&T->IPos,&View->ISource,&U);
		MatrixTXVector(T->RotMat,&U,&W);
		// Camera Illumination model (Lambertian)
		if (Cam_HeadLight)
		{
			for(Vtx=T->Verts;Vtx<VEnd;Vtx++)
      {
/*        Its = fabs(Dot_Product(&Vtx->N,(Vector *)(M)+2)*30000000.0);
				Its /= Dot_Product(&Vtx->TPos,&Vtx->TPos);*/
				Vector_Add(&Vtx->Pos,&W,&V);
				Its = 350000.0/Dot_Product(&V,&V);
				if (Its<1.0) Its=0.5;
				if (Its>229.0) Its=229.0;
				Vtx->LR = Its+Ka;
				Vtx->LG = Its+Ka;
				Vtx->LB = Its+Ka;
//      Vtx->LR=Vtx->LG=Vtx->LB = 0.5;
			}
		} else {
			for(Vtx=T->Verts;Vtx<VEnd;Vtx++)
				Vtx->LR=Vtx->LG=Vtx->LB = Ka;
		}
		// Lightsource based Illumination model
/*    for(O=Sc->OmniHead;O;O=O->Next)
			if(O->Flags&Flare_LightSource)
			{
				Vector_Sub(&T->IPos,&O->IPos,&U);
				MatrixTXVector(T->RotMat,&U,&W);
				for(Vtx=T->Verts;Vtx<VEnd;Vtx++)
				{
					Vector_Add(&Vtx->Pos,&W,&L);
					Dot=1.0/Dot_Product(&L,&L);
					Vtx->LR += O->L.R * Dot;
					Vtx->LG += O->L.G * Dot;
					Vtx->LB += O->L.B * Dot;
				}
			}*/
/*
	N = surface normal
	L     = unit vector between point and light
	V = unit vector between point and view
*/
//    Ka + Kd * (N dot L) + Ks * (N dot ( L + V / 2))^n
		for(O=Sc->OmniHead;O;O=O->Next)
			if(O->Flags&Flare_LightSource)
			{
				Vector_Sub(&T->IPos,&O->IPos,&U);
				MatrixTXVector(T->RotMat,&U,&W);

				Vector_Sub(&View->ISource,&O->IPos,&CU);
				MatrixTXVector(View->Mat,&CU,&CV);

				for(Vtx=T->Verts;Vtx<VEnd;Vtx++)
				{
					Vector_Add(&Vtx->Pos,&W,&L);
					rLen = RSQRT(L.X*L.X+L.Y*L.Y+L.Z*L.Z);
					Dot=-Dot_Product(&L,&Vtx->N)*rLen;

//					Vector_Add(&Vtx->Pos,&CV,&L);
//					rLen = RSQRT(L.X*L.X+L.Y*L.Y+L.Z*L.Z);
//					Dot2=-Dot_Product(&L,&Vtx->N)*rLen;

					if (Dot>0.0f)
					{
						//Color=(Kd*Dot/*+Ks*(Dot*Dot*Dot+Dot2)*0.5f*/)*rLen*rLen;
						Color=(Dot*(Kd+Ks*Dot*Dot)*0.5f)*rLen*rLen;
						Vtx->LR += O->L.R * Color;
						Vtx->LG += O->L.G * Color;
						Vtx->LB += O->L.B * Color;
					}
				}
			}

		// Saturation.
		for(Vtx=T->Verts;Vtx<VEnd;Vtx++)
		{
			if (Vtx->LR>253.0) Vtx->LR=253.0;
			if (Vtx->LG>253.0) Vtx->LG=253.0;
			if (Vtx->LB>253.0) Vtx->LB=253.0;
//      Vtx->LR = 0.25; Vtx->LG = 0.25; Vtx->LB = 0.25; //Or some other jack,like an Ambient constant
		}
	}
}


void Restore_Splines(Scene *Sc)
{
	TriMesh *T;
	Camera *Cm;
	Omni *Om;
	for(T=Sc->TriMeshHead;T;T=T->Next)
  {
    T->Pos.CurKey = 0;
    T->Rotate.CurKey = 0;
    T->Scale.CurKey = 0;
    T->CurStat = T->Status;
  }
  for(Om=Sc->OmniHead;Om;Om=Om->Next)
  {
    Om->Pos.CurKey = 0;
//    Om->Col.CurKey = 0;
  }
  for(Cm=Sc->CameraHead;Cm;Cm=Cm->Next)
  {
    Cm->Source.CurKey = 0;
    Cm->Target.CurKey = 0;
    Cm->Roll.CurKey = 0;
    Cm->FOV.CurKey = 0;
  }
}

void Render()
{
  long I = CAll;
  Face **FLS = FList;//+CAll-1;
  Vertex *A,*B,*C,*T;
  TriMesh *Tri;
  Edge **EP;
  while (I--)
  {
    DoFace=*FLS++;

    // Get Mapping Coordinates from the rendered face.
    A=DoFace->A; B=DoFace->B; C=DoFace->C;
    A->U = DoFace->U1; A->V = DoFace->V1;
    B->U = DoFace->U2; B->V = DoFace->V2;
    C->U = DoFace->U3; C->V = DoFace->V3;
    A->RU = A->U * A->RZ; A->RV = A->V * A->RZ;
    B->RU = B->U * B->RZ; B->RV = B->V * B->RZ;
    C->RU = C->U * C->RZ; C->RV = C->V * C->RZ;

    if ((A->Flags|B->Flags|C->Flags)&Vtx_Visible)
      Universal_Frustrum_Elite();
    else {
      if (A->PY>B->PY) {T=A; A=B; B=T;}
      if (A->PY>C->PY) {T=A; A=C; C=T;}
      if (B->PY>C->PY) {T=B; B=C; C=T;}
      DoFace->Filler(A,B,C);
    }
  }

  //test for edges stuff
/*  for(Tri=CurScene->TriMeshHead;Tri;Tri=Tri->Next)
  {
    if (Tri->Flags&Tri_Invisible) continue;
    for(I=0;I<Tri->EIndex;I++)
      if (Tri->Edges[I].Flags&Edge_Concave)
        Bivex_Frustrum_Perspective(Tri->Edges[I].A,Tri->Edges[I].B,&Draw_Line_32Bit_ForKids);
      else
        Bivex_Frustrum_Perspective(Tri->Edges[I].A,Tri->Edges[I].B,&Draw_Line_32Bit);
  }*/

/*  for(EP = ColEdge_Stack;EP<CES;EP++)
    Bivex_Frustrum_Perspective(EP[0]->A,EP[0]->B,&Draw_Line_32Bit_ForKids);*/

  //test for vertex stuff
/*  DWord *VP = (DWord *)VPage;
  for(Tri=CurScene->TriMeshHead;Tri;Tri=Tri->Next)
  {
    if (Tri->Flags&Tri_Invisible) continue;
    for(I=0;I<Tri->VIndex;I++)
      if (Tri->Verts[I].Flags==Vtx_Spike)
        VP[YOffs[(int)Tri->Verts[I].PY]+(int)Tri->Verts[I].PX] = 0xFFFFFFFF;
  }*/

}

// yeah
char Check_Texture_Memory_Range(Scene *Sc)
{
  TriMesh *T;
  Face *F,*FE;
  Material *M;
  Material *Ptr;
  for(T=Sc->TriMeshHead;T;T=T->Next)
  {
    F = T->Faces;
    FE = F + T->FIndex;
    for(;F<FE;F++)
    {
      Ptr = F->Txtr;
      if (!Ptr) continue;
      for(M=MatLib;M;M=M->Next)
        if (M==Ptr)
          break;
      if (!M)
      {
        printf("Address generation Interlock!!\n"); // some ppl say Asyncrontus Gateway Interlock (AGI) delay...one of them is me -Silvatar.
        printf("Ptr = %xh\n",(long)Ptr);
        printf("Material Name = %s\n",Ptr->Name);
        return 0;
      }
    }
  }
//  for(M=MatLib;M;M=M->Next)
//    printf("Mat-lib = %xh ==> txtr = %xh\n",(long)M,(long)(M->Txtr));
  return 1;
}

void RunScene(Scene *Sc,float Seconds)
{
  float t;
  long TTrd=0;
  long Par1,Par2,Par3,Par4,Par5,Par6,Tot;
  long RenderedPolys=0;
  long Y;
  long SceneTime = Seconds * Timer_Freq;

  long I,J;
  float FPS;
  char *Str = new char[80];
  char AfterFirstFrame = 0,Cur = 0;
  char *BPage = new char[PageSize];
  memset(BPage,0,PageSize);
  VESA_Surface Blurred;
  memcpy(&Blurred,Screen,sizeof(VESA_Surface));
  Blurred.Data = BPage;
  Blurred.Flags = VSurf_Noalloc;
  Blurred.Targ = VGAPtr;

  Layer2.Data = new char [PageSize];
  Layer2.BPP = 32;
  memset(Layer2.Data,0,sizeof(PageSize));
  Layer2.Flags = VSurf_Noalloc;
  Layer2.Targ = VGAPtr;

  Material *M;
  Object *Obj;
  CurScene=Sc;
  CurFrame=Sc->StartFrame;
	View=&FC;
	View->Next=Sc->CameraHead;
	if (Sc->CameraHead)
	{
		Sc->CameraHead->Prev=View;
//    View=View->Next;
	}
	View=View->Next;
  FList_Allocate(Sc);
  // Force LINEAR palette
  if (BPP==8)
  {
    char *Pal = Screen->Pal;
    for(I=0;I<256;I++)
    {
      *Pal++ = Sc->Pal->C[I].R;
      *Pal++ = Sc->Pal->C[I].G;
      *Pal++ = Sc->Pal->C[I].B;
    }
    Set_Palette(Screen->Pal);
  }

  if (!Check_Texture_Memory_Range(Sc)) return;

/*  for(Obj = Sc->ObjectHead;Obj;Obj=Obj->Next)
    printf("Object (%s)'s Pivot is (%f,%f,%f)\n",Obj->Name,Obj->Pivot.X,Obj->Pivot.Y,Obj->Pivot.Z);*/

  Timer=0; Frames=0;
  CurFrame=0;

  while (!Keyboard[ScESC])
  {
    if (Timer>SceneTime)
    {
      Frames=0; RenderedPolys=0;
      Timer-=SceneTime;
      TTrd-=SceneTime;
      Restore_Splines(Sc);
      // clear message buffer
      for(I=0;I<10;I++)
      {
        delete MsgStr[I];
        MsgStr[I]=NULL;
      }

    }
    t=float(Timer)/(float)SceneTime;

    CurFrame=float((1.0-t)*Sc->StartFrame)+float(t*Sc->EndFrame);
    if (!Keyboard[ScD])
    {
      memset(VPage,0,PageSize);
//      memset(Layer2.Data,0,PageSize);
    }
    if (AfterFirstFrame) //so lame
    {
      dTime = Timer-TTrd;
      TTrd = Timer;
      if (View==&FC) Dynamic_Camera();
      dTime *= 0.25;
    } else {AfterFirstFrame=1; TTrd = Timer; dTime = 0;}
    Animate_Objects(Sc);

    if (!(Sc->Flags&Scn_Nolighting)) Lighting(Sc); Par4 = Timer;
    Transform_Objects(Sc); Par1 = Timer;
    if (Sc->PclExec) Sc->PclExec(); Par2 = Timer;
    if (!CAll) goto Away;
    Radix_Asm(FList,SList,CAll);
//    Radix_Sorting();
//    Quick_Sorting(0,CAll-1);
    if (!Keyboard[ScE]) Render();     Par5 = Timer;
    Away:Tot = Timer-TTrd;
    FPS = 100.0/((float)(Tot));
    Frames++;
    RenderedPolys+=CPolys;

    Y = -15;
    for(I=0;I<10;I++)
      if (MsgStr[I])
      {
        Y = OutTextXY(VPage,0,Y+15,MsgStr[I],64);
        if (Timer>MsgClock[I]+150)
        {
          delete MsgStr[I];
          MsgStr[I]=NULL;
        }
      }
#ifdef RUNTIME_INFO_L1
    sprintf(Str,"Time = %d , Frame = %5.1f",Timer,CurFrame);
    Y = OutTextXY(VPage,0,Y+15,Str,64);
    sprintf(Str,"Inst. FPS = %3.2f (%3.2f)",FPS,100.0*(float)Frames/(float)Timer);
    Y = OutTextXY(VPage,0,Y+15,Str,64);
#endif
#ifdef RUNTIME_INFO_L2
//    sprintf(Str,"Active Camera = %s",View->Name);
//    Y = OutTextXY(VPage,0,Y+15,Str,64);
    if (Tot)
    {
      sprintf(Str,"Perf. Distribution: xForm=%d%%,P.Knmtx=%d%%,FList=%d%%,Light=%d%%,\nRender=%d%%",(Par1-TTrd)*100/Tot,(Par2-Par1)*100/Tot,(Par3-Par2)*100/Tot,(Par4-Par3)*100/Tot,(Par5-Par4)*100/Tot);
      Y = OutTextXY(VPage,0,Y+15,Str,64);
    }
//    sprintf(Str,"Dyn. Kinematics: p(%4.2f,%4.2f,%4.2f),v(%1.4f,%1.4f,%1.4f) r(%1.4f,%1.4f,%1.4f)",FC.ISource.X,FC.ISource.Y,FC.ISource.Z,FV.X,FV.Y,FV.Z,FT.X,FT.Y,FT.Z);
//    Y = OutTextXY(VPage,0,Y+15,Str,64);
    sprintf(Str,"Average Polys/Frame: %5.1f (%3.1f%%)",(float)RenderedPolys/(float)Frames,100.0*(float)CPolys/(float)Polys);
    Y = OutTextXY(VPage,0,Y+15,Str,64);
#endif
#ifdef RUNTIME_INFO_L3
    sprintf(Str,"Rendered %d Polygons,%d Omnilights and %d particles.\n",CPolys,COmnies,CPcls);
    Y = OutTextXY(VPage,0,Y+15,Str,64);
#endif
//    DisplayMouse();
    if (!Keyboard[ScD])
    {
      //hope it works ;)
/*      if (MMXState)
        Modulate(Screen,&Blurred,0xb0b0b0,0xb0b0b0);
      else
        Transparence(Screen,&Blurred);*/
//      Flip(&Blurred);
      Flip(Screen);

/*      if (MMXState)
        MulModulate(&Layer2,Screen);
      else*/
//    Transparence(&Layer2,Screen);
//      Flip(Screen);
    }

    if (Keyboard[ScP]) {I=Timer; while (Keypressed()); Timer=I;} //Timefreeze
    if (Keyboard[ScTab]) if (View->Next) View=View->Next; else View=&FC;
    if (Keyboard[ScR]) {
      Restore_9();
      Deinitialize_VESA();
      printf("Debug me, PLEASE!\n");
      getchar();
      getchar();
      getchar();
      exit(-1);
    }

    // Free camera ASSUME IDENTITY command.
    if (Keyboard[ScC]) {FC.ISource = View->ISource; Matrix_Copy(FC.Mat,View->Mat); FC.IFOV=View->IFOV;}
    if (Keyboard[ScH]) Cam_HeadLight^=1;
  }

  while (Keyboard[ScESC]) DO_NOP();
  Exec_FPS = 100.0*(float)Frames/(float)Par5;
/*  sprintf(Str,"Inst. FPS = %3.2f (%3.2f)",FPS,100.0*(float)Frames/(float)Par4);
  OutTextXY(VGAPtr,0,0,Str,63);
  while (!Keyboard[ScESC]) DO_NOP();
  while (Keyboard[ScESC]) DO_NOP();*/
  Restore_Splines(Sc);
  delete FList;
  delete SList;
  delete Str;
  delete BPage;
  delete Layer2.Data;
}
