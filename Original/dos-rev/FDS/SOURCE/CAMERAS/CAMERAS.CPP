#include <Math.H>
#include <String.H>
#include <ConIO.H>
#include <StdLib.H>
#include <StdIO.H>

#include "..\FDS_Defs.H"
#include "..\FDS_Vars.H"
#include "..\FDS_Decs.H"

Camera      * View;

Camera FC;
Vector FV,FT,ColN,OldIS;
Vector Grav = {0.0,-0.01,0.0};
static Face *ColFace;
static Matrix NormConv;
static const long Max_Collision_Det = 4;
static long CAttempt;

Vertex **ColVtx_Stack = new Vertex * [30],**CVS;
Edge **ColEdge_Stack = new Edge * [50],**CES;
static Face **ColFace_Stack = new Face * [15],**CFS;
static Vector *ColNorm_Stack = new Vector[50],*CVV;

void CalcPersp(Camera *Cm)
{
  float TanFOV;
  float vx,vy;

  TanFOV = tan((Cm->IFOV/2.0)/180.0*PI);
  vx = CntrX;
  vy = CntrY;// /AspectRatio;
  Cm->PerspX = vx / TanFOV;
  Cm->PerspY = vy / TanFOV;
}

float LensToFOV(float lens)
{
  int i;

  struct {float lens, fov;} lens_table[] =
  {
    {15.0,  115.0}, {20.0, 94.28571}, {24.0, 84.0}, {28.0,  76.36364},
    {35.0,  63.0},  {50.0, 46.0},     {85.0, 28.0}, {135.0, 18.0},
    {200.0, 12.0}
  };

  for (i = 0; i < 9; i++) if (lens == lens_table[i].lens) {
    return lens_table[i].fov;
  }
  return 15.0 / lens * 160;
}


/// INFERIOR TO KICK_CAMERA --- WILL BE REMOVED (V0.75A)
void Shatter_Camera(Vector *Source,Vector *Target,float Roll,Matrix CamMat)
{
  Vector CamVec,Temp;
  float VecLen,SinX,CosX,SinY,CosY,SinZ,CosZ;
  int a;

  Vector_Sub(Target,Source,&CamVec);
  VecLen=sqrt(CamVec.X*CamVec.X+CamVec.Y*CamVec.Y+CamVec.Z*CamVec.Z);
  Temp.X=-atan2(CamVec.X,CamVec.Z);
  Temp.Y=asin(CamVec.Y/VecLen);
  Temp.Z=-Roll*PI/180.0;
  SinX=sin(Temp.X); CosX=cos(Temp.X);
  SinY=sin(Temp.Y); CosY=cos(Temp.Y);
  SinZ=sin(Temp.Z); CosZ=cos(Temp.Z);
  CamMat[0][0]=SinX*SinY*SinZ+CosX*CosZ;
  CamMat[0][1]=CosY*SinZ;
  CamMat[0][2]=SinX*CosZ-CosX*SinY*SinZ;
  CamMat[1][0]=SinX*SinY*CosZ-CosX*SinZ;
  CamMat[1][1]=CosY*CosZ;
  CamMat[1][2]=-CosX*SinY*CosZ-SinX*SinZ;
  CamMat[2][0]=-SinX*CosY;
  CamMat[2][1]=SinY;
  CamMat[2][2]=CosX*CosY;
}

void Kick_Camera(Vector *Source,Vector *Target,float Roll,Matrix CamMat)
{
  Vector UP,N,V,U;
  float S = sin(Roll*PI/180.0),C = cos(Roll*PI/180.0);
  float F;

  Vector_Form(&UP,0,1,0);
  Vector_Sub(Target,Source,&V);
  Vector_Norm(&V);
//  UP.X=sin(Roll*PI/180);
//  UP.Y=cos(Roll*PI/180);
//  UP.Z=0;
//  Cross_Product(&UP,&V,&N);
  N.X = V.Z;
  N.Y = 0.0;
  N.Z =-V.X;
  F = 1.0/sqrt(N.X*N.X+N.Z*N.Z);
  N.X *= F;
  N.Z *= F;
  Cross_Product(&V,&N,&U);

  CamMat[0][0]=N.X*C-U.X*S; CamMat[0][1]=N.Y*C-U.Y*S; CamMat[0][2]=N.Z*C-U.Z*S;
  CamMat[1][0]=U.X*C+N.X*S; CamMat[1][1]=U.Y*C+N.Y*C; CamMat[1][2]=U.Z*C+N.Z*S;
  CamMat[2][0]=V.X;         CamMat[2][1]=V.Y;         CamMat[2][2]=V.Z;

//  Matrix_Rotation(M,-Roll*PI/180.0,0,0);
}

#define Camera_Sphere 25.0
#define Camera_SphereTINY 24.9
#define Camera_Sphere2 Camera_Sphere*Camera_Sphere
#define Camera_Sphere2TINY Camera_SphereTINY*Camera_SphereTINY

// Detects a Collision of the Camera with a Face, and Updates the stack
// if neccesary.
void Sphere2FaceIntersection(Vector *Ray,Face *F)
{
  Vector P,U,V,W,N1,N2,N3;
  float t1,t2,t3;
  char str[80];

  MatrixXVector(NormConv,&F->N,CVV); //rotated face normal. [31 cycles]
                                    //can also be computed with u,v and cross
                                    //prod, unless length=1 is important

  // Tests Plane Intersection.
  float D = -Dot_Product(CVV,&F->A->TPos); //Plane's offset/dist. from Origin
  float D2 = Dot_Product(CVV,Ray)+D;  //Ray's end distance from plane
  if (D2<0||D2>=Camera_SphereTINY) return;

  // Projection
  Vector_Scale(CVV,D2,&V);
  Vector_Sub(Ray,&V,&P);

  Vector_SelfSub(&P,&F->A->TPos);
  Vector_Sub(&F->B->TPos,&F->A->TPos,&U);
  Vector_Sub(&F->C->TPos,&F->A->TPos,&V);

  //Face bound test.
  Cross_Product(&U,CVV,&N1);
  t1 = Dot_Product(&P,&N1);

  Vector_Sub(&V,&U,&W);
  Cross_Product(&W,CVV,&N2);
  Vector_Sub(&P,&U,&W);
  t2 = Dot_Product(&W,&N2);

  if (t1*t2>0)
  {
    Cross_Product(CVV,&V,&N3);
    Vector_Sub(&P,&V,&W);
    t3 = Dot_Product(&W,&N3);
    if (t2*t3>0)
    {
      //Insert face to collision list, record rotated normal vector.
      *CFS++ = F;
      CVV++;
    }
  }
}

void Sphere2EdgeIntersection(Vector *Ray,Edge *E)
{
  Vector U,V,W;
  float Len,dot;

  Vector_Sub(Ray,&E->A->TPos,&U);
  Vector_Sub(&E->B->TPos,&E->A->TPos,&V);
  Cross_Product(&U,&V,&W);
  Len = Vector_SelfDot(&V);
  dot = Dot_Product(&U,&V);
  if ((Vector_SelfDot(&W)<Len*Camera_Sphere2TINY)&&
      (dot>=0&&dot<=Len))
  {
    *CES++ = E;
//    Len = 1.0/E->Length;
//    Vector_SelfScale(&V,Len); //normalize
    Vector_Norm(&V);
//    dot *= Len;
    dot = Dot_Product(&U,&V);
    Vector_SAdd(&U,-dot,&V);
    Vector_Norm(&U);
    Vector_Copy(CVV,&U);
    //SHEET. now we do the bangoff
    //SHEEEET.

    CVV++;
  }
}

// Yeah, piece'o'cake
void Sphere2VertexIntersection(Vector *Ray,Vertex *Vtx)
{
  Vector V;
  Vector_Sub(Ray,&Vtx->TPos,&V);
  if (Vector_SelfDot(&V)<Camera_Sphere2TINY)
  {
    Vector_Norm(&V);
    Vector_Copy(CVV,&V);
    *CVS++ = Vtx;
    CVV++;
  }
}

// Advanced Collision Detection algorithm.
void Advanced_Collision_Detection()
{
  TriMesh *T;
  Face *F,*FEnd;
  Face **FP,**FPEnd;
  Vector U,AP,V,TV,W;
  Vector CV,*VP;
  long Still;
  float f,d;
  Face **Col_Buff,**Prev;
  Vector *Col_NBuff;
  Edge *E,*EE,**EP;
  Vertex *Vtx,*VE,**VtP;

  Vector_Sub(&FC.ISource,&OldIS,&CV);
  MatrixXVector(FC.Mat,&CV,&TV);

  // Empty Collision Buffers
  CVS = ColVtx_Stack;
  CVV = ColNorm_Stack;
  CES = ColEdge_Stack;
  CFS = ColFace_Stack;

  for(T=CurScene->TriMeshHead;T;T=T->Next)
  {
    // Replace this with interval-sphere raytracer, should be better
    if (T->Flags&Tri_Invisible) continue;
    // sphere should be outside the camera's sphere in both states
    Vector_Sub(&T->IPos,&OldIS,&V); //trimesh center rel. position
    // first to added factors should be insignificent.
    if (Vector_Length(&CV)+Camera_Sphere+sqrt(T->BSphereRad)<Vector_Length(&V)) continue;

    F=T->Faces; FEnd=F+T->FIndex;
    MatrixXMatrix(FC.Mat,T->RotMat,NormConv);
    Vector_Sub(&T->IPos,&View->ISource,&U);
    MatrixTXVector(T->RotMat,&U,&AP);
    for(;F<FEnd;F++)
    {
      //B.F.C. (Will be documented better next time ;) aka flags
      if (AP.X*F->N.X + AP.Y*F->N.Y + AP.Z*F->N.Z>F->NormProd) continue;
      Sphere2FaceIntersection(&TV,F);
    }
  }

  Still = 4; // shouldn't get down to 0 anyway.

  // Empty Face collision buffer
  while (Still--)
  {
    //Run through the entire buffer and repel camera off the faces.
    VP = ColNorm_Stack;
    for(FP = ColFace_Stack;FP<CFS;FP++,VP++)
    {
      if (Dot_Product(VP,&TV)>0) continue;
      //Update Position.
      d = Dot_Product(&TV,VP)-Dot_Product(VP,&FP[0]->A->TPos); //distance
      if (d>Camera_Sphere) continue;
      MatrixTXVector(FC.Mat,VP,&V); //camera rel >> absolute normal
      Vector_SAdd(&FC.ISource,Camera_Sphere-d,&V);
      //Update Velocity
      f = Dot_Product(&FV,&V);
      Vector_Scale(&V,f,&U);
      Vector_SelfSub(&FV,&U);
    }

    // Calculate new effective Camera Position
    Vector_Sub(&FC.ISource,&OldIS,&CV);
    MatrixXVector(FC.Mat,&CV,&TV);

    // ok, it's payback time!
    // reconstruct collision list, only checking the following:
    // 1.the original faces
    // 2.the faces from both sides of the collided edges (optional,also means
    // we should calc edge hits before actually taking them into consideration
    Col_Buff = new Face * [15];
    Col_NBuff = new Vector [50];
    VP = ColNorm_Stack;
    Prev = CFS;
    CFS = Col_Buff;
    CVV = Col_NBuff;
    for(FP = ColFace_Stack;FP<Prev;FP++,VP++)
      Sphere2FaceIntersection(&TV,*FP);
    if (CFS == Col_Buff) break; //quit if none were collided (finally)
    // Replicate new list
    delete ColFace_Stack;
    delete ColNorm_Stack;
    ColFace_Stack = Col_Buff;
    ColNorm_Stack = Col_NBuff;
  }

  // Edge-based collision detection (Yeah, piece-o-cake)
  // has a serious m.f. bug
  CVV = ColNorm_Stack;
  for(T=CurScene->TriMeshHead;T;T=T->Next)
  {
    if (T->Flags&Tri_Invisible) continue;

    // sphere should be outside the camera's sphere in both states
    Vector_Sub(&T->IPos,&OldIS,&V); //trimesh center rel. position
    // first to added factors should be insignificent.
    if (Vector_Length(&CV)+Camera_Sphere+sqrt(T->BSphereRad)<Vector_Length(&V)) continue;

    E = T->Edges; EE = E + T->EIndex;

    for(;E<EE;E++)
      if (E->Flags&Edge_Concave)
        Sphere2EdgeIntersection(&TV,E);
  }
  VP = ColNorm_Stack;
  for(EP = ColEdge_Stack;EP<CES;EP++,VP++)
  {
    if (Dot_Product(VP,&TV)>0) continue;
    //Update Position.
    d = Dot_Product(&TV,VP)-Dot_Product(VP,&EP[0]->A->TPos); //distance (gotcha!!!)
    if (d>Camera_SphereTINY) continue;
    MatrixTXVector(FC.Mat,VP,&V); //camera rel >> absolute normal
    Vector_SAdd(&FC.ISource,Camera_Sphere-d,&V);
    //Update Velocity
//    Vector_Form(&FV,0,0,0);
    f = Dot_Product(&FV,&V);
    Vector_Scale(&V,f,&U);
    Vector_SelfSub(&FV,&U);

    // Calculate new effective Camera Position
    Vector_Sub(&FC.ISource,&OldIS,&CV);
    MatrixXVector(FC.Mat,&CV,&TV);
  }


  for(T=CurScene->TriMeshHead;T;T=T->Next)
  {
    if (T->Flags&Tri_Invisible) continue;
    // sphere should be outside the camera's sphere in both states
    Vector_Sub(&T->IPos,&OldIS,&V); //trimesh center rel. position
    // first to added factors should be insignificent.
    if (Vector_Length(&CV)+Camera_Sphere+sqrt(T->BSphereRad)<Vector_Length(&V)) continue;


    Vtx = T->Verts; VE = Vtx + T->VIndex;
    CVV = ColNorm_Stack;
    for(;Vtx<VE;Vtx++)
      if (Vtx->Flags&Vtx_Spike)
        Sphere2VertexIntersection(&TV,Vtx);
  }
  VP = ColNorm_Stack;
  for(VtP = ColVtx_Stack;VtP<CVS;VtP++,VP++)
  {
    if (Dot_Product(VP,&TV)>0) continue;
    d = Dot_Product(&TV,VP)-Dot_Product(VP,&VtP[0]->TPos); //distance
    if (d>Camera_Sphere) continue;
    MatrixTXVector(FC.Mat,VP,&V); //camera rel >> absolute normal
    Vector_SAdd(&FC.ISource,Camera_Sphere-d,&V);
    //Update Velocity
    f = Dot_Product(&FV,&V);
    Vector_Scale(&V,f,&U);
    Vector_SelfSub(&FV,&U);
    // Calculate new effective Camera Position
    Vector_Sub(&FC.ISource,&OldIS,&CV);
    MatrixXVector(FC.Mat,&CV,&TV);
  }
}



// Ahead VGA/Wizard AT-100 with 256Kb and Trident 8900 with 512Kb RULEZ!!!!!
void Init_FreeCamera()
{
  Vector_Form(&FC.ISource,0,0,-100);
  Vector_Form(&FC.ITarget,0,0,-99);
  FC.IRoll = 0.0;
  FC.IFOV = 80;
  CalcPersp(&FC);
  Matrix_Copy(FC.Mat,Mat_ID);
}

float Vel_Speed   = 0.080;

void Dynamic_Camera()
{

  const float Rot_Speed   = 0.004;
  const float Vel_FallOff = 0.02;
  const float Rot_FallOff = 0.05;
  Vector U,V;
  float dot;
  CAttempt = 1;
  char str[100];
  float temp_alias;

  if (Keyboard[ScEnd])
  {
    FV.X-=FC.Mat[0][0]*Vel_Speed;
    FV.Y-=FC.Mat[0][1]*Vel_Speed;
    FV.Z-=FC.Mat[0][2]*Vel_Speed;
  }
  if (Keyboard[ScPgDn])
  {
    FV.X+=FC.Mat[0][0]*Vel_Speed;
    FV.Y+=FC.Mat[0][1]*Vel_Speed;
    FV.Z+=FC.Mat[0][2]*Vel_Speed;
  }
  if (Keyboard[ScGrayPlus])
  {
    FV.X-=FC.Mat[1][0]*Vel_Speed;
    FV.Y-=FC.Mat[1][1]*Vel_Speed;
    FV.Z-=FC.Mat[1][2]*Vel_Speed;
  }
  if (Keyboard[ScGrayMinus])
  {
    FV.X+=FC.Mat[1][0]*Vel_Speed;
    FV.Y+=FC.Mat[1][1]*Vel_Speed;
    FV.Z+=FC.Mat[1][2]*Vel_Speed;
  }
  if (Keyboard[ScZ])
  {
    FV.X-=FC.Mat[2][0]*Vel_Speed;
    FV.Y-=FC.Mat[2][1]*Vel_Speed;
    FV.Z-=FC.Mat[2][2]*Vel_Speed;
  }
  if (Keyboard[ScA])
  {
    FV.X+=FC.Mat[2][0]*Vel_Speed;
    FV.Y+=FC.Mat[2][1]*Vel_Speed;
    FV.Z+=FC.Mat[2][2]*Vel_Speed;
  }
  if (Keyboard[ScHome]) FT.Z+=Rot_Speed;
  if (Keyboard[ScPgUp]) FT.Z-=Rot_Speed;
  if (Keyboard[ScLeft]) FT.Y-=Rot_Speed;
  if (Keyboard[ScRight]) FT.Y+=Rot_Speed;
  if (Keyboard[ScDown]) FT.X-=Rot_Speed;
  if (Keyboard[ScUp]) FT.X+=Rot_Speed;
  if (Keyboard[ScComma]) Vel_Speed/=1.5;
  if (Keyboard[ScPeriod]) Vel_Speed*=1.5;

/*  if (Keyboard[ScCtrl])
    Fire_LASER();*/

  Vector_Scale(&FV,dTime,&V);

  //No collisions - apply velocity
  Vector_SelfAdd(&FC.ISource,&V);

  //Apply collision detection model

/*  temp_alias = Vector_Length(&V)/Camera_Sphere;
  sprintf(str,"Temporal Aliasing = %f",Vector_Length(&V)/Camera_Sphere);
  MsgToDisplay(COLLISION_DET_ID,0,str);

  if (temp_alias<1.0)
  {
    Vector_Copy(&OldIS,&FC.ISource);
    Vector_SelfAdd(&FC.ISource,&V);

    Advanced_Collision_Detection();
  } else {
    Vector_SelfScale(&V,1.0/ceil(temp_alias));
    Vector_Copy(&OldIS,&FC.ISource);
    Vector_Copy(&U,&FC.ISource); //init
    while (temp_alias>0&&U.X==FC.ISource.X&&U.Y==FC.ISource.Y&&U.Z==FC.ISource.Z)
    {
      Vector_SelfAdd(&FC.ISource,&V);
      Vector_Copy(&U,&FC.ISource);
      Advanced_Collision_Detection();
      temp_alias-=1.0;
    }
  }*/

  if (FT.X*FT.X+FT.Y*FT.Y+FT.Z*FT.Z>EPSILON)
    Matrix_Rotation(FC.Mat,FT.X*dTime,FT.Y*dTime,FT.Z*dTime);
  // apply gravitic force
/*  Vector_Scale(&Grav,dTime,&V);
  Vector_SelfAdd(&FV,&V);*/

  // falloff
  Vector_SelfScale(&FV,exp(-Vel_FallOff*dTime));
  Vector_SelfScale(&FT,exp(-Rot_FallOff*dTime));
}