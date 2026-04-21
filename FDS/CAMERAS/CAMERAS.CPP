#include <Base/Scene.h>
#include <Base/TriMesh.h>
#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"

Camera      * View;

Camera FC;
Vector FV,FT,ColN,OldIS;
Vector Grav(0.0,-0.01,0.0);
static Face *ColFace;
static Matrix NormConv;
static const int32_t Max_Collision_Det = 4;
static int32_t CAttempt;

Vertex **ColVtx_Stack = new Vertex * [30],**CVS;
Edge **ColEdge_Stack = new Edge * [50],**CES;
static Face **ColFace_Stack = new Face * [15],**CFS;
static Vector *ColNorm_Stack = new Vector[50],*CVV;
float AspectRatio = 4.0 / 3.0;


void CalcPersp(Camera *Cm)
{
	const float screenAR = (float)(YRes) / (float)(XRes);
	const float TanFOV = tanf((Cm->IFOV/2.0f)/180.0f*PI);
	Cm->PerspX = CntrX / TanFOV;
	Cm->PerspY = Cm->PerspX * AspectRatio * screenAR;
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
  return 15.0f / lens * 160.0f;
}


/// INFERIOR TO KICK_CAMERA --- WILL BE REMOVED (V0.75A)
void Shatter_Camera(Vector *Source,Vector *Target,float Roll,Matrix CamMat)
{
  Vector CamVec,Temp;
  float VecLen,SinX,CosX,SinY,CosY,SinZ,CosZ;

  Vector_Sub(Target,Source,&CamVec);
  VecLen=sqrtf(CamVec.x*CamVec.x+CamVec.y*CamVec.y+CamVec.z*CamVec.z);
  Temp.x=-atan2f(CamVec.x,CamVec.z);
  Temp.y=asinf(CamVec.y/VecLen);
  Temp.z=-Roll*PI/180.0;
  SinX=sinf(Temp.x); CosX=cosf(Temp.x);
  SinY=sinf(Temp.y); CosY=cosf(Temp.y);
  SinZ=sinf(Temp.z); CosZ=cosf(Temp.z);
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
  float S = sinf(Roll*PI/180.0f),C = cosf(Roll*PI/180.0f);
  float F;

  Vector_Form(&UP,0,1,0);
  Vector_Sub(Target,Source,&V);
  Vector_Norm(&V);
//  UP.x=sin(Roll*PI/180);
//  UP.y=cos(Roll*PI/180);
//  UP.z=0;
//  Cross_Product(&UP,&V,&N);
  N.x = V.z;
  N.y = 0.0f;
  N.z =-V.x;
  F = 1.0f/sqrtf(N.x*N.x+N.z*N.z);
  N.x *= F;
  N.z *= F;
  Cross_Product(&V,&N,&U);

  CamMat[0][0]=N.x*C-U.x*S; CamMat[0][1]=N.y*C-U.y*S; CamMat[0][2]=N.z*C-U.z*S;
  CamMat[1][0]=U.x*C+N.x*S; CamMat[1][1]=U.y*C+N.y*C; CamMat[1][2]=U.z*C+N.z*S;
  CamMat[2][0]=V.x;         CamMat[2][1]=V.y;         CamMat[2][2]=V.z;

//  Matrix_Rotation(M,-Roll*PI/180.0,0,0);
}

#define Camera_Sphere 25.0f
#define Camera_SphereTINY 24.9f
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
  Face **FP;
  Vector U,AP,V,TV,W;
  Vector CV,*VP;
  int32_t Still;
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
      if (AP.x*F->N.x + AP.y*F->N.y + AP.z*F->N.z>F->NormProd) continue;
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
    delete []ColFace_Stack;
    delete []ColNorm_Stack;
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

const float CamRad = 15.0f;
const float CamStepTol = 7.5f;

struct LineSeg
{
	float Nx,Nz;
	float D;
	float t1,t2;
};

LineSeg *LEBuf = NULL;
size_t LEBufSize;

// must be robust (inaccuracy tolerant)
// updates A as necessary in case of overlapping.
DWord MatchLSeg(LineSeg *A,LineSeg *B)
{
	if (A->Nx*B->Nx + A->Nz*B->Nz<0.9999) return 0;
	if (fabs(A->D-B->D)>0.001) return 0;

	if (A->t1 > B->t2) return 0;
	if (A->t2 < B->t1) return 0;

	//overlapping segments: extend paramater range and return 1
	if (B->t1 < A->t1) A->t1 = B->t1;
	if (B->t2 > A->t2) A->t2 = B->t2;
	return 1;
}

void Initiate_LEBuf(Scene *Sc)
{
	TriMesh *T;
	Face *F,*FE;

	int32_t Polys = 0;
	int32_t EffLines = 0;
	float t1,t2,t3;

	for(T=Sc->TriMeshHead;T;T=T->Next)
		Polys += T->FIndex;

	LineSeg *TempBuf = new LineSeg[Polys],*LE = TempBuf,*L;

	for(T=Sc->TriMeshHead;T;T=T->Next)
	{
		for(F = T->Faces, FE = F + T->FIndex; F<FE ; F++)
		{
			if (fabs(F->N.y)>0.001) continue;

			LE->Nx = F->N.x;
			LE->Nz = F->N.z;
			LE->D = F->NormProd;

			t1 = LE->Nz * F->A->Pos.x - LE->Nx * F->A->Pos.z;
			t2 = LE->Nz * F->B->Pos.x - LE->Nx * F->B->Pos.z;
			t3 = LE->Nz * F->C->Pos.x - LE->Nx * F->C->Pos.z;

			LE->t1 = LE->t2 = t1;
			if (t2<t3)
			{
				if (t2<t1) LE->t1 = t2;
				if (t3>t1) LE->t2 = t3;
			} else {
				if (t2>t1) LE->t2 = t2;
				if (t3<t1) LE->t1 = t3;
			}

			// match with history
			for(L=TempBuf;L<LE;L++)
				if (MatchLSeg(L,LE)) break;
			if (L==LE) LE++;
		}
	}
	LEBufSize = LE-TempBuf;
	if (LEBufSize)
	{
		LEBuf = new LineSeg[LEBufSize];
		memcpy(LEBuf,TempBuf,LEBufSize*sizeof(LineSeg));
	}
	delete []TempBuf;
}

void LSeg2SphereIntersect2D(LineSeg *LE,Vector *Pos,Vector *Vel)
{
	// Test distance from plane/line.
	float D,t;
	float Px,Pz,Nx,Nz;

	Nx = LE->Nx;
	Nz = LE->Nz;
	D = Pos->x * Nx + Pos->z * Nz + LE->D;
	if (D<0||D>CamRad) return;

	// Project position upon plane. (2D only)
	Px = Pos->x - Nx * D;
	Pz = Pos->z - Nz * D;

	t = Nz * Px - Nx * Pz;

	if (LE->t1<=t&&t<=LE->t2)
	{
		// Collision has occured, update stuff.
		D = CamRad - D;
		Pos->x += Nx * D;
		Pos->z += Nz * D;
		D = Vel->x * Nx + Vel->z * Nz;
		Vel->x -= Nx * D;
		Vel->z -= Nz * D;
	}
}

void Face2SphereIntersect2D(Face *F,Vector *Pos,Vector *Vel)
{
	// Test distance from plane/line.
	float D;
	float Px,Pz,Nx,Nz;

	Nx = F->N.x;
	Nz = F->N.z;
	D = Pos->x * Nx + Pos->z * Nz + F->NormProd;
	if (D<0||D>CamRad) return;

	// Project position upon plane. (2D only)
	Px = Pos->x - Nx * D;
	Pz = Pos->z - Nz * D;

	// This section can be optimized if u keep an array of lines instead
	// of scanning faces...this is the best i can do without this precalc:

	if (fabs(Nz)<0.001)
	{
		// test on Z axis: if Pz resides within face/line bounderies
		if ( ( Pz>F->A->Pos.z && Pz>F->B->Pos.z && Pz>F->C->Pos.z ) ||
				 ( Pz<F->A->Pos.z && Pz<F->B->Pos.z && Pz<F->C->Pos.z ) ) return;
	} else {
		if ( ( Px>F->A->Pos.x && Px>F->B->Pos.x && Px>F->C->Pos.x ) ||
				 ( Px<F->A->Pos.x && Px<F->B->Pos.x && Px<F->C->Pos.x ) ) return;
	}
	// Collision has occured, update stuff.
	D = CamRad - D;
	Pos->x += Nx * D;
	Pos->z += Nz * D;
	D = Vel->x * Nx + Vel->z * Nz;
	Vel->x -= Nx * D;
	Vel->z -= Nz * D;
}


void Vertex2SphereIntersect2D(Vertex *Vtx,Vector *Pos,Vector *Vel)
{
	float dx = Pos->x - Vtx->Pos.x;
	float dz = Pos->z - Vtx->Pos.z;
	float l,nl;
	if ((l = dx*dx+dz*dz)<CamRad*CamRad)
	{
		l = sqrt(l);
		nl = 1.0f/l;
		dx *= nl;
		dz *= nl;

		l = CamRad - l;
		Pos->x += dx * l;
		Pos->z += dz * l;
		l = Vel->x * dx + Vel->z * dz;
		Vel->x -= dx * l;
		Vel->z -= dz * l;
	}
}

void A2DC(Scene *Sc,Vector *Pos,Vector *Vel)
{
	TriMesh *T;
	Vertex *Vtx,*VE;

	LineSeg *L,*LE;
	for(L = LEBuf, LE = L + LEBufSize ; L<LE ; L++)
		LSeg2SphereIntersect2D(L,Pos,Vel);

	for(T=Sc->TriMeshHead;T;T=T->Next)
	{
/*		for(F = T->Faces, FE = F + T->FIndex ; F<FE ; F++)
		{
			// eliminate non-vertical faces
			if (fabs(F->N.y)>0.001) continue;

			Face2SphereIntersect2D(F,Pos,Vel);
		}*/
		for(Vtx = T->Verts, VE = Vtx + T->VIndex ; Vtx<VE ; Vtx++)
			Vertex2SphereIntersect2D(Vtx,Pos,Vel);
	}
}

// Note! This function uses the 'int32_t dTime' global variable.
// this means that: the camera is passed positioned at its previous
// location. it will attempt to progress by 'dTime * Vel'.
void Advanced2DCollision(Scene *Sc,Vector *Pos,Vector *Vel)
{
	float TDist;
	Vector V,W,uStep,Step;
	float vl = Vector_Length(Vel);
	TDist = vl*dTime;

	if (TDist<CamStepTol)
	{
		Vector_Scale(Vel,dTime,&V);
		Vector_SelfAdd(Pos,&V);
		A2DC(Sc,Pos,Vel);
	} else {
		Vector_Scale(Vel,1.0f/vl,&uStep);
		Vector_Scale(&uStep,CamStepTol,&Step);
		while (TDist>CamStepTol)
		{
			Vector_SelfAdd(Pos,&Step);
			A2DC(Sc,Pos,Vel);
			TDist -= CamStepTol;
		}
		Vector_Scale(&uStep,TDist,&Step);
		Vector_SelfAdd(Pos,&Step);
		A2DC(Sc,Pos,Vel);
	}
	Vector_Scale(Vel,CamRad/vl,&W); //step vector.


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
    FV.x-=FC.Mat[0][0]*Vel_Speed;
    FV.y-=FC.Mat[0][1]*Vel_Speed;
    FV.z-=FC.Mat[0][2]*Vel_Speed;
  }
  if (Keyboard[ScPgDn])
	{
    FV.x+=FC.Mat[0][0]*Vel_Speed;
    FV.y+=FC.Mat[0][1]*Vel_Speed;
    FV.z+=FC.Mat[0][2]*Vel_Speed;
  }
	if (Keyboard[ScGrayPlus])
	{
    FV.x-=FC.Mat[1][0]*Vel_Speed;
		FV.y-=FC.Mat[1][1]*Vel_Speed;
    FV.z-=FC.Mat[1][2]*Vel_Speed;
  }
	if (Keyboard[ScGrayMinus])
  {
    FV.x+=FC.Mat[1][0]*Vel_Speed;
    FV.y+=FC.Mat[1][1]*Vel_Speed;
    FV.z+=FC.Mat[1][2]*Vel_Speed;
  }
  if (Keyboard[ScZ])
  {
    FV.x-=FC.Mat[2][0]*Vel_Speed;
		FV.y-=FC.Mat[2][1]*Vel_Speed;
    FV.z-=FC.Mat[2][2]*Vel_Speed;
  }
  if (Keyboard[ScA])
  {
		FV.x+=FC.Mat[2][0]*Vel_Speed;
		FV.y+=FC.Mat[2][1]*Vel_Speed;
    FV.z+=FC.Mat[2][2]*Vel_Speed;
	}
  if (Keyboard[ScHome]) FT.z+=Rot_Speed;
  if (Keyboard[ScPgUp]) FT.z-=Rot_Speed;
	if (Keyboard[ScLeft]) FT.y-=Rot_Speed;
  if (Keyboard[ScRight]) FT.y+=Rot_Speed;
  if (Keyboard[ScDown]) FT.x-=Rot_Speed;
  if (Keyboard[ScUp]) FT.x+=Rot_Speed;
  if (Keyboard[ScComma]) Vel_Speed/=1.1;
  if (Keyboard[ScPeriod]) Vel_Speed*=1.1;

/*  if (Keyboard[ScCtrl])
    Fire_LASER();*/

	Vector_Scale(&FV,dTime,&V);

	//No collisions - apply velocity
	Vector_SelfAdd(&FC.ISource,&V);

	// yeah well, gotta admit this rulez.
	//if (!LEBuf) Initiate_LEBuf(CurScene);
	//Advanced2DCollision(CurScene,&FC.ISource,&FV);

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
		while (temp_alias>0&&U.x==FC.ISource.x&&U.y==FC.ISource.y&&U.z==FC.ISource.z)
		{
			Vector_SelfAdd(&FC.ISource,&V);
			Vector_Copy(&U,&FC.ISource);
			Advanced_Collision_Detection();
			temp_alias-=1.0;
		}
	}*/

	if (FT.x*FT.x+FT.y*FT.y+FT.z*FT.z>EPSILON)
		Matrix_Rotation(FC.Mat,FT.x*dTime,FT.y*dTime,FT.z*dTime);
	// apply gravitic force
/*  Vector_Scale(&Grav,dTime,&V);
	Vector_SelfAdd(&FV,&V);*/

	// falloff
  Vector_SelfScale(&FV,expf(-Vel_FallOff*dTime));
  Vector_SelfScale(&FT,expf(-Rot_FallOff*dTime));
}