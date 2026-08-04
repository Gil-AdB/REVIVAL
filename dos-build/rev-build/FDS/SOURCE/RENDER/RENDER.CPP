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

#ifdef FDS_CLIPCHK
long g_MeshCullLog = 0;   // scenes set this for the frames they want MESHCULL lines on
#endif

#include "../FDS_Defs.H"
#include "../FDS_Vars.H"
#include "../FDS_Decs.H"

Scene *CurScene;
Face **FList;
Face **SList;
Face *DoFace; // mar gamish, ta'ase partsuf!
long CPolys,COmnies,CPcls,CAll,Polys;
// When non-NULL, Transform_Objects inserts faces from ONLY this mesh (used by city's
// transparent-water phase: below-water / water / above-water must render in that order,
// so the water is drawn on its own between the reflection pass and the opaque pass).
TriMesh *g_RenderOnlyMesh = 0;
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

    if (T->Flags&Tri_AlignToPath)
    {
      Vector AheadPos,HDir,ZeroV;
      Matrix PathMat,TmpMat;
      Vector_Form(&ZeroV,0.0f,0.0f,0.0f);
      Spline_Calc_3D(&T->Pos,CurFrame+1.0f,&AheadPos);   // sample the path slightly ahead
      Vector_Sub(&AheadPos,&T->IPos,&HDir);              // heading = velocity direction
      if (Vector_Length(&HDir)>0.001f)                   // PathingMinVelocity: keep last heading when ~stationary
        Vector_Copy(&T->Heading,&HDir);
      Kick_Camera(&ZeroV,&T->Heading,0.0f,PathMat);      // build an orientation looking down the path
      Matrix_Transpose(PathMat);
      Spline_Calc_3D(&T->Rotate,CurFrame,&HDir);
      Euler_Angles(T->RotMat,0.0f,0.0f,HDir.Z);          // apply only the Z (bank/roll) from the Rotate track
      MatrixXMatrix(PathMat,T->RotMat,TmpMat);
      Matrix_Copy(T->RotMat,TmpMat);
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
#ifdef FDS_ZFLIP
  float passMinZ = 1e30f;   // nearest inserted-face vertex this pass -> adaptive ZScaleF
#endif
  float dz;
  long *pdz = (long *)(&dz);
  long I;
  Face **Ins = FList;
  float *f = (float *)(&M);
  float *fv;

  for (T=Sc->TriMeshHead;T;T=T->Next)
  {
    if (g_RenderOnlyMesh && T!=g_RenderOnlyMesh) continue;   // water-only phase (city)
    if (!(T->Flags&HTrack_Visible)) {T->Flags|=Tri_Invisible; continue;}
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


    // aprioric Offset Vector = the camera offset expressed in the mesh's LOCAL frame, used
    // by the per-face backface test below (NormProd is built from LOCAL Pos/N in PREPROC).
    // Animate_Objects scales RotMat's ROWS by IScale, so RotMat = diag(IScale)*R and its
    // TRANSPOSE IS NOT ITS INVERSE: MatrixTXVector alone yields R^T*diag(S)*U, i.e. the scale
    // applied instead of un-applied - off by S^2. Pre-divide by IScale^2 so the result is
    // R^T*diag(1/S)*U, the true local offset. Unit-scale meshes are unaffected (this is why
    // city, all IScale=1, looked right while the scaled meshes mis-culled faces:
    // fount.lwo is IScale 1.958, i.e. a 3.83x error in the backface threshold).
    MatrixTXVector(T->RotMat,&U,&AP);
    { // Port of master FDS/RENDER/Transform.cpp:889-892, absent here: divide by the row
      // norm^2. RotMat = k*R (Animate_Objects scales its rows by IScale, and the quaternion
      // path can leave its own k), so MatrixTXVector gives k*R^T*U - the scale APPLIED where
      // it must be UN-applied. Dividing by k^2 yields R^T*U/k, the true local-space camera
      // offset the per-face test needs (NormProd is built from LOCAL Pos/N in PREPROC).
      // Use the ROW NORM, not IScale: the greets mech measured IScale=1.000 with
      // rowlen=0.450 (scale carried in the rotation part), so an IScale-based correction
      // misses it entirely. Unit-scale meshes (all of city) are unaffected.
      Vector *WP = (Vector *)T->RotMat;
      float d = Dot_Product(WP,WP);
      if (d>0.0f) Vector_SelfScale(&AP,1.0f/d); }
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
    // Out by left/right.
    // The side-plane rejection is only MEANINGFUL when the bounding sphere lies entirely in
    // front of the camera (Tri_Ahead). If the sphere straddles Z=0 the side tests are taken
    // on a centre that may be BEHIND the camera, and then `S.X*PX > S.Z*CntrEX` is true for
    // any S.Z<0 (right-hand side negative, left-hand side >=0) - so the whole mesh is culled
    // as "off to the right" exactly when the camera gets close enough to enter its sphere.
    // That is the greets mech vanishing as you approach it. Straddling meshes must fall
    // through to the Regular (fully clipped) path instead.
    S.X=fabs(S.X);
    L1 = PX*S.X - CntrEX*S.Z;
    if (L1*L1>L2*T->BSphereRad*(PX*PX+CntrEX*CntrEX))
    {
      if (T->Flags&Tri_Ahead)
      {
        if (S.X*PX>S.Z*CntrEX)
        {
          T->Flags |= Tri_Invisible;
          continue;
        }
        T->Flags |= Tri_Inside;
      }
    }
    // Out by up/down (same restriction)
    S.Y = fabs(S.Y);
    L1 = PY*S.Y - CntrEY*S.Z;
    if (L1*L1>L2*T->BSphereRad*(PY*PY+CntrEY*CntrEY))
    {
      if ((T->Flags&Tri_Ahead) && S.Y*PY>S.Z*CntrEY)
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
        // "Ahead" only means the whole mesh is in front of Z=0 - it can still cross FZP,
        // and without this flag those faces skip the clipper and never get far-clipped
        // (same inconsistency as the Regular path had).
        if (Vtx->TPos.Z>C_FZP) Vtx->Flags|=Vtx_VisFar;
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
        if (Vtx->TPos.Z>C_NZP)
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
          // Beyond the far plane. Without this, Render_Face's "any vertex outside -> clip"
          // test is FALSE for a face that is fully ON-SCREEN but past FZP, so it skips the
          // clipper and the far clip never runs on it - while a neighbouring face that
          // happens to touch a screen edge DOES get cut. That inconsistency is the
          // camera-angle-dependent far clipping. (CITY.CPP's own transform already set it.)
          if (Vtx->TPos.Z>C_FZP) Vtx->Flags|=Vtx_VisFar;
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

        if (Vtx->TPos.Z>C_NZP)
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
          if (Vtx->TPos.Z>C_FZP) Vtx->Flags+=Vtx_VisFar;   // see note above
        } else Vtx->Flags=Vtx_VisNear;
  //      printf("Regular shit!\n");
      }

    }
    OUT:FEnd=T->Faces+T->FIndex;
#ifdef FDS_CLIPCHK
    // Per-mesh face-cull attribution: WHY does a face never reach the clipper?
    // Gated so it only fires on the frames the scene asks for (one line per mesh per frame).
    if (g_MeshCullLog)
    { long _nAll=0,_nRej=0,_nBack=0,_nIns=0; Face *_f;
      for (_f=T->Faces;_f<FEnd;_f++)
      {
        _nAll++;
        if (_f->A->Flags&_f->B->Flags&_f->C->Flags&Vtx_Visible) { _nRej++; continue; }
        if (!((_f->Txtr->Flags&Mat_TwoSided)||
              (AP.X*_f->N.X+AP.Y*_f->N.Y+AP.Z*_f->N.Z<_f->NormProd))) { _nBack++; continue; }
        _nIns++;
      }
      fprintf(LogFile,"  MESHCULL T=%p faces=%ld trivialreject=%ld backface=%ld inserted=%ld "
                      "flags=%lx(Inside=%d Ahead=%d)\n",
        (void*)T,_nAll,_nRej,_nBack,_nIns,(unsigned long)T->Flags,
        (int)((T->Flags&Tri_Inside)!=0),(int)((T->Flags&Tri_Ahead)!=0));
      fflush(LogFile); }
#endif
#ifdef FDS_ZFLIP
    // One-sort trick (ascending radix, ONE forward walk):
    //   opaque: key = maxZ clamped to fzp     -> front-to-back, all below fzp
    //   xpar:   key = (2*fzp - cz) + ff       -> back-to-front by PURE DEPTH,
    //                                            always after every opaque
    // Pure depth (not master's extent/facing grouping) = the original painter's
    // order within the xpar set: the shipped binary sorted everything 100000-dz.
    // Master's grouping put city's giant water quad (front-facing, +4*fzp) after
    // the omni flares and its 50/50 blend dimmed every light-post in the canal.
    { 
    T->DbgFaces=T->FIndex; T->DbgRej=0; T->DbgBack=0; T->DbgIns=0;
    for (F=T->Faces;F<FEnd;F++)
      if (F->A->Flags&F->B->Flags&F->C->Flags&Vtx_Visible) T->DbgRej++;
      else if (!((F->Txtr->Flags&Mat_TwoSided)||
                 (AP.X*F->N.X + AP.Y*F->N.Y + AP.Z*F->N.Z<F->NormProd))) T->DbgBack++;
      else
      {
        T->DbgIns++;
        *Ins++ = F;
        dz = F->A->TPos.Z;
        if (F->B->TPos.Z>dz) dz=F->B->TPos.Z;
        if (F->C->TPos.Z>dz) dz=F->C->TPos.Z;
        { float mz = F->A->TPos.Z;
          if (F->B->TPos.Z<mz) mz=F->B->TPos.Z;
          if (F->C->TPos.Z<mz) mz=F->C->TPos.Z;
          // near-clipped faces carry vertices BEHIND the camera (z<1, down to -50k);
          // they pinned passMin negative and the adaptive scale never engaged.
          if (mz>=1.0f && mz<passMinZ) passMinZ=mz; }
        // Float keys sort correctly: the C radix (SORTS.H port) reads all 32 bits and
        // positive IEEE floats order by their unsigned bit patterns.
        if (F->Txtr->Flags & (Mat_Transparent|Mat_Virtual))
        { float fzp=Sc->FZP, cz=((float)dz>fzp)?fzp:(float)dz, ff=(fzp-cz)/fzp;
          F->SortZ.F = (2.0f*fzp - cz) + ff; }   // same form as the omni/particle keys
        else
          F->SortZ.F = ((float)dz>Sc->FZP) ? Sc->FZP : (float)dz;
      }
    }
#else
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
#endif
  }
  CPolys = Ins-FList;
#ifdef FDS_ZFLIP
  // Adaptive z-code scale: spend the 16 bits on [wNear, FZP] instead of [1, FZP].
  // Fixed 49152/z left <50 codes for everything past z~1000 (whole buildings tied on
  // one code). wNear = half the nearest vertex (margin for camera motion between the
  // estimate and the draw); snaps nearer instantly, relaxes farther 25%/frame. The
  // mappers clamp scanline-endpoint codes to 65534, so an underestimate can't wrap.
  { static float zWNear = 1.0f;
    float target = (passMinZ<1e29f) ? passMinZ*0.5f : 1.0f;
    if (target<C_NZP) target=C_NZP;   // near-clip floor: near geom keeps codes
    if (target>256.0f) target=256.0f;
    if (target<zWNear) zWNear=target;
    else { zWNear*=1.25f; if (zWNear>target) zWNear=target; }
    ZScaleF = 49152.0f*zWNear;
  }
  // FDS_ZLINEAR: fixed, far-anchored LINEAR-Z scale (master's IX-filler scheme). Unlike the
  // adaptive 1/w ZScaleF above, this maps view-Z linearly over [0,1.1*FZP] into [0xFF80,~128]
  // with guard bands, so near-clipped geometry (Z=NearZ) can't saturate. Mappers built with
  // the linear-Z encode EXTRN _ZLinScale instead of _ZScaleF.
  ZLinScale = 65280.0f / (Sc->FZP * 1.1f);   // 0xFF00 / (FZP*1.1)
#endif
  for(O=Sc->OmniHead;O;O=O->Next)
  {
    Vtx=&O->V;
    Vector_Sub(&O->IPos,&View->ISource,&V);
    MatrixXVector(View->Mat,&V,&Vtx->TPos);
    if (Vtx->TPos.Z>1)   // sprite (flare): no Clipper - keep the 1.0 gate so an off-screen
                          // near-projected sprite never reaches the frustum clipper
    {
      Vtx->RZ=1.0/Vtx->TPos.Z;
      Vtx->PX=CntrEX+Vtx->TPos.X*PX*Vtx->RZ;
      Vtx->PY=CntrEY-Vtx->TPos.Y*PY*Vtx->RZ;
      // Insert to List
      dz = Vtx->TPos.Z;
      //dz *=-16384;
      //dz +=0x7FFFFFFF;
      //RoundToInt(&O->Face.SortZ.DW,dz);
#ifdef FDS_ZFLIP
      { float fzp=Sc->FZP, cz=(dz>fzp)?fzp:(float)dz, ff=(fzp-cz)/fzp;
        O->F.SortZ.F = (2.0f*fzp - cz) + ff; }   // sprite: xpar-style, depth-interleaved
#else
      O->F.SortZ.F = 100000.0-dz;
#endif
      // The flare filler is an unclipped additive sprite blit: an off-screen
      // center writes VPage at a negative/out-of-range offset -> OOB (faults on
      // strict memory, silently tolerated by DOSBox). Real geometry is frustum-
      // flagged + clipped; flares were the one path that skipped it. Cull flares
      // whose center is off-screen.
      if (Vtx->PX>=0.0 && Vtx->PX<CntrEX*2.0 && Vtx->PY>=0.0 && Vtx->PY<CntrEY*2.0)
        *Ins++ = &O->F;
    }
  }
  COmnies = (Ins-FList)-CPolys;
  for(I=0;I<Sc->NumOfParticles;I++)
    if ((dz=Sc->Pcl[I].V.TPos.Z)>=1.0) {   // sprite: keep 1.0 gate (see omni note)
      F = &Sc->Pcl[I].F;
      //dz *=-16384;
      //dz +=0x7FFFFFFF;
      //RoundToInt(&F->SortZ.DW,dz);
#ifdef FDS_ZFLIP
      { float fzp=Sc->FZP, cz=(dz>fzp)?fzp:(float)dz, ff=(fzp-cz)/fzp;
        F->SortZ.F = (2.0f*fzp - cz) + ff; }
#else
      F->SortZ.F = 100000.0-dz;
#endif
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

	// --- PERF PROBE (one-time per scene): size the static-lighting bake opportunity.
	// The per-frame hot loop is O(litOmni x shadedVerts). A pair is bakeable iff BOTH the
	// omni is stationary (Pos spline constant) AND the mesh is stationary (Pos/Rotate/Scale
	// constant, not possessed/parented). Report bakeable vs total vtx-light evaluations. ---
	{
		static Scene *_lprobed = 0;
		if (Sc != _lprobed)
		{
			_lprobed = Sc;
			Omni *pO; TriMesh *pT; DWord k;
			long nLit=0,nLitS=0,nMesh=0,nMeshS=0,vAll=0,vStat=0;
			for(pO=Sc->OmniHead;pO;pO=pO->Next)
			{
				if(!(pO->Flags&Flare_LightSource)) continue;
				nLit++;
				int c=(pO->Pos.NumKeys<=1);
				if(!c){ c=1; for(k=1;k<pO->Pos.NumKeys;k++){
					SplineKey *K=&pO->Pos.Keys[k],*K0=&pO->Pos.Keys[0];
					if(K->Pos.X!=K0->Pos.X||K->Pos.Y!=K0->Pos.Y||K->Pos.Z!=K0->Pos.Z){c=0;break;} } }
				if(c) nLitS++;
			}
			FILE *pf=fopen("PERF.TXT","at");
			for(pT=Sc->TriMeshHead;pT;pT=pT->Next)
			{
				if(pT->Flags&(Tri_Invisible|Tri_Noshading)) continue;
				nMesh++; vAll+=pT->VIndex;
				int posV=0,rotV=0,sclV=0;
				Spline *SP[2]; SP[0]=&pT->Pos; SP[1]=&pT->Scale;
				for(int s=0;s<2;s++){ int *vf=s?&sclV:&posV; if(SP[s]->NumKeys>1){ for(k=1;k<SP[s]->NumKeys;k++){
					SplineKey *K=&SP[s]->Keys[k],*K0=&SP[s]->Keys[0];
					if(K->Pos.X!=K0->Pos.X||K->Pos.Y!=K0->Pos.Y||K->Pos.Z!=K0->Pos.Z){*vf=1;break;} } } }
				if(pT->Rotate.NumKeys>1){ for(k=1;k<pT->Rotate.NumKeys;k++){
					SplineKey *K=&pT->Rotate.Keys[k],*K0=&pT->Rotate.Keys[0];
					if(K->Pos.W!=K0->Pos.W||K->Pos.X!=K0->Pos.X||K->Pos.Y!=K0->Pos.Y||K->Pos.Z!=K0->Pos.Z||
					   K->AA.W!=K0->AA.W||K->AA.X!=K0->AA.X||K->AA.Y!=K0->AA.Y||K->AA.Z!=K0->AA.Z){rotV=1;break;} } }
				int poss=(pT->Flags&Tri_Possessed)!=0, par=(pT->Flags&Tri_Parented)!=0;
				int stat=!poss&&!par&&!posV&&!rotV&&!sclV;
				if(stat){ nMeshS++; vStat+=pT->VIndex; }
				if(pf) fprintf(pf,"  M#%ld V=%4ld %s poss=%d par=%d posV=%d rotV=%d sclV=%d keys(P%lu/R%lu/S%lu)\n",
					nMesh,(long)pT->VIndex,stat?"STATIC":"moving ",poss,par,posV,rotV,sclV,
					(unsigned long)pT->Pos.NumKeys,(unsigned long)pT->Rotate.NumKeys,(unsigned long)pT->Scale.NumKeys);
			}
			if(pf){ fprintf(pf,"LPROBE: litOmni=%ld(stat %ld) shadedMesh=%ld(stat %ld) verts=%ld(stat %ld) | bakeable vtx-light=%ld of total=%ld (%.0f%%)\n",
				nLit,nLitS,nMesh,nMeshS,vAll,vStat,(long)nLitS*vStat,(long)nLit*vAll,
				(nLit*vAll)?100.0*((double)nLitS*vStat)/((double)nLit*vAll):0.0); fclose(pf); }
		}
	}

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
				// Parented "mech flare" omnis light the room but NOT the mech they're embedded
				// in (parented meshes) — avoids the point-blank 1/dist² rainbow blast.
				if ((O->Flags&Flare_Parented)&&(T->Flags&Tri_Parented)) continue;
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
	// distance fog for Scn_Fogged scenes (greets): the shared Lighting() never applied
	// it, so fogged scenes stayed at full brightness ("too bright"). TPos.Z is valid here
	// (Lighting runs AFTER Transform_Objects). Clamp: past FZP the term goes negative and
	// would otherwise wrap bright in the filler.
	if (Sc->Flags&Scn_Fogged)
	{
		TriMesh *FT; Vertex *FV,*FVE;
		for(FT=Sc->TriMeshHead;FT;FT=FT->Next)
		{
			if (FT->Flags&(Tri_Invisible|Tri_Noshading)) continue;
			FV=FT->Verts; FVE=FV+FT->VIndex;
			for(;FV<FVE;FV++)
			{
				float Color = 1.0f - FV->TPos.Z*C_rFZP;
				if (Color<0.0f) Color=0.0f; else if (Color>1.0f) Color=1.0f;
				FV->LR *= Color; FV->LG *= Color; FV->LB *= Color;
			}
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

#ifdef FDS_ZFLIP
// One face through the standard mapping-coords + clip-or-fill dispatch (shared by the
// two-pass FDS_ZBUF walk below; the sorted list is walked twice with a filter).
// The fillers carry u,v as 16.16 fixed point (u*65536 via FISTP), so |u| >= 32768 overflows
// to the x87 integer-indefinite 0x80000000 - NOT a modular wrap - and the whole span then
// samples one bogus texel. Texture addressing is mod 256 on both axes (tiled index is
// ((u>>3)<<11)|(v<<3)|(u&7); the row-major path masks to 8 bits), so subtracting a common
// multiple of 256 from all THREE vertices of a face is exactly equivalent and puts the
// magnitude back in range. Greets ships faces with V ~ 55200 (215 tile repeats baked into
// the coordinate) - measured via FDS_CLIPCHK's uvOVF counter.
// Centre the face's UV range on zero, in whole multiples of 256 - that is the shift that
// MINIMISES max|u| for the face. Do NOT shift by the minimum: city's water quad carries
// mixed-sign UVs spanning ~77000 (a 2-triangle quad with ~150 tile repeats), and pushing its
// minimum up to [0,256) moved every rasterised piece from |u|~2000 to |u|~40000 - i.e. it
// MANUFACTURED the overflow. Centred, a face already straddling zero gets k=0 and is
// untouched, while greets' narrow-span/huge-offset faces (u~1731..1996, v~55173..55442) lose
// the offset and land back in range.
static void NormalizeFaceUV(Vertex *A,Vertex *B,Vertex *C)
{
  float lo,hi,k;
  // The |k|<=0.5 test is the whole fast path: k would round to 0 (= no shift) for any face
  // whose UV midpoint is already within half a texture of the origin, which is almost every
  // face in the demo. Taking it avoids the float->long cast, and on a Pentium each cast is a
  // control-word swap (FSTCW/FLDCW around FISTP) - too expensive to pay per face per frame.
  lo=hi=A->U; if(B->U<lo)lo=B->U; if(B->U>hi)hi=B->U; if(C->U<lo)lo=C->U; if(C->U>hi)hi=C->U;
  k = (lo+hi)*(0.5f/256.0f);
  if (k<-0.5f || k>0.5f)
  { k = 256.0f*(float)(long)(k>=0.0f ? k+0.5f : k-0.5f); A->U-=k; B->U-=k; C->U-=k; }
  lo=hi=A->V; if(B->V<lo)lo=B->V; if(B->V>hi)hi=B->V; if(C->V<lo)lo=C->V; if(C->V>hi)hi=C->V;
  k = (lo+hi)*(0.5f/256.0f);
  if (k<-0.5f || k>0.5f)
  { k = 256.0f*(float)(long)(k>=0.0f ? k+0.5f : k-0.5f); A->V-=k; B->V-=k; C->V-=k; }
}

// Faces that skip the clipper entirely (no vertex flagged Vtx_Visible): one filler call,
// no fan. Total triangles = g_DirectTris + g_ClipTris.
extern "C" long g_DirectTris;

static void Render_Face(void)
{
  Vertex *A,*B,*C,*T;
  A=DoFace->A; B=DoFace->B; C=DoFace->C;
  A->U = DoFace->U1; A->V = DoFace->V1;
  B->U = DoFace->U2; B->V = DoFace->V2;
  C->U = DoFace->U3; C->V = DoFace->V3;
  NormalizeFaceUV(A,B,C);
  A->RU = A->U * A->RZ; A->RV = A->V * A->RZ;
  B->RU = B->U * B->RZ; B->RV = B->V * B->RZ;
  C->RU = C->U * C->RZ; C->RV = C->V * C->RZ;
  if ((A->Flags|B->Flags|C->Flags)&Vtx_Visible)
    Universal_Frustrum_Elite();
  else {
    if (A->PY>B->PY) {T=A; A=B; B=T;}
    if (A->PY>C->PY) {T=A; A=C; C=T;}
    if (B->PY>C->PY) {T=B; B=C; C=T;}
    g_DirectTris++; DoFace->Filler(A,B,C);
  }
}
#define FACE_IS_XPAR(F) ((F)->Txtr && ((F)->Txtr->Flags & (Mat_Transparent|Mat_Virtual)))
#endif

void Render()
{
  long I = CAll; static long _rnc=0;
#ifdef FDS_ZBUF
  // Fresh depth per Render() invocation (city runs two: reflection + main pass).
  memset(ZBuf,0,(long)XRes*(long)YRes*2);
#endif
  Face **FLS = FList;//+CAll-1;
  Vertex *A,*B,*C,*T;
  TriMesh *Tri;
  Edge **EP;
  if (_rnc==0){
    fprintf(LogFile,"FLAGPROBE: Vtx_Visible=0x%lx Vtx_VisNear=0x%lx Vtx_VisFar=0x%lx Vtx_VisLeft=0x%lx Vtx_VisDown=0x%lx  (expect Visible=3f VisFar=2)\n",
      (long)Vtx_Visible,(long)Vtx_VisNear,(long)Vtx_VisFar,(long)Vtx_VisLeft,(long)Vtx_VisDown);
    // scan every inserted face's verts: how many have bit 0x02 set, and is any A/B/C NULL / wild-Z?
    long b1=0,nullv=0,wildz=0,tot=0; Face **fs=FList; long ii=CAll;
    while(ii--){ Face *f=*fs++; Vertex *vv[3]={f->A,f->B,f->C}; for(int k=0;k<3;k++){ tot++;
      if(!vv[k]){nullv++;continue;} if(vv[k]->Flags&0x02)b1++; float z=vv[k]->TPos.Z; if(z< -1e5||z>1e5)wildz++; } }
    fprintf(LogFile,"FLAGPROBE: faces=%ld vtxrefs=%ld bit0x02set=%ld NULLverts=%ld wildZ=%ld\n",(long)CAll,tot,b1,nullv,wildz);
    fflush(LogFile);
  }
#ifdef FDS_ZFLIP
  // Master's one-sort model: the keys (Transform_Objects) already order the whole list
  // as opaque front-to-back, then transparents/sprites back-to-front object-grouped.
  // ONE forward walk, no filtering, no reversal.
  if (_rnc<6) {
    // ORDER VERIFICATION: monotonic key bits (uint order == positive-float order) and a
    // clean opaque->xpar partition judged by the FACE FLAGS, not the key encoding.
    long vio=0, nOp=0, nXp=0, mixed=0; DWord prev=0; char sawX=0;
    Face **fp=FList; long ii;
    for (ii=0; ii<CAll; ii++) {
      DWord k = fp[ii]->SortZ.DW;
      char xp = (fp[ii]->Txtr && (fp[ii]->Txtr->Flags&(Mat_Transparent|Mat_Virtual)));
      if (k<prev) vio++;
      prev=k;
      if (xp) { sawX=1; nXp++; } else { nOp++; if (sawX) mixed++; }
    }
    fprintf(LogFile,"SORTCHK: n=%ld opaque=%ld xpar=%ld violations=%ld opaque-after-xpar=%ld\n",
            CAll,nOp,nXp,vio,mixed);fflush(LogFile);
  }
  while (I--)
  {
    DoFace = *FLS++;
#ifdef FDS_NOFLARE
    if (DoFace->Txtr && (DoFace->Txtr->Flags & Mat_Virtual)) continue;  // DIAG
#endif
    Render_Face();
  }
#else
  while (I--)
  {
    DoFace=*FLS++;

    // Get Mapping Coordinates from the rendered face.
    A=DoFace->A; B=DoFace->B; C=DoFace->C;
    A->U = DoFace->U1; A->V = DoFace->V1;
    B->U = DoFace->U2; B->V = DoFace->V2;
    C->U = DoFace->U3; C->V = DoFace->V3;
    NormalizeFaceUV(A,B,C);
    A->RU = A->U * A->RZ; A->RV = A->V * A->RZ;
    B->RU = B->U * B->RZ; B->RV = B->V * B->RZ;
    C->RU = C->U * C->RZ; C->RV = C->V * C->RZ;

    if(_rnc==0){fprintf(LogFile,"REN %s A(%.0f,%.0f)B(%.0f,%.0f)C(%.0f,%.0f) fl=%lx Fi=%p\n",((A->Flags|B->Flags|C->Flags)&Vtx_Visible)?"CLIP":"FILL",A->PX,A->PY,B->PX,B->PY,C->PX,C->PY,(long)(A->Flags|B->Flags|C->Flags),(void*)DoFace->Filler);fflush(LogFile);}
    if ((A->Flags|B->Flags|C->Flags)&Vtx_Visible)
      Universal_Frustrum_Elite();
    else {
      if (A->PY>B->PY) {T=A; A=B; B=T;}
      if (A->PY>C->PY) {T=A; A=C; C=T;}
      if (B->PY>C->PY) {T=B; B=C; C=T;}
      g_DirectTris++; DoFace->Filler(A,B,C);
    }
    if(_rnc==0){fprintf(LogFile,"RENok\n");fflush(LogFile);}
  }
#endif
  _rnc++;

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
