#include <StdIO.H>
#include <StdLIB.H>

#include "..\FDS_Vars.H"
#include "..\FDS_Decs.H"
#include "..\FDS_Defs.H"

float FOVX,FOVY;

// Advanced Frustrum Elite (Nonhomeginous / Triangle / Universal)
void (*Frust_2DI)(Vertex *A,Vertex *B,Vertex *Out,float L);
void (*Frust_3DI)(Vertex *A,Vertex *B,Vertex *Out,float L);
void (*Rasterizer)(Vertex *A,Vertex *B,Vertex *C);

// Avatar is back...
Vertex F_VB[12];  // Vertex Buffer - for clipper generated vertices.
void Vertex_Interpolate3D_Flat(Vertex *A,Vertex *B,Vertex *Out,float L)
{
  float R = 1.0f-L;
  Out->PX = A->TPos.X*R + B->TPos.X*L;
  Out->PY = A->TPos.Y*R + B->TPos.Y*L;
}

void Vertex_Interpolate2D_Flat(Vertex *A,Vertex *B,Vertex *Out,float L)
{
  float R = 1.0f-L;
  Out->PX = A->PX*R + B->PX*L;
  Out->PY = A->PY*R + B->PY*L;
}

void Frust_Set_Flat()
{
  Frust_2DI = &Vertex_Interpolate2D_Flat;
  Frust_3DI = &Vertex_Interpolate3D_Flat;
}

void Vertex_Interpolate3D_L(Vertex *A,Vertex *B,Vertex *Out,float L)
{
  float R = 1.0f-L;
  Out->PX = A->TPos.X*R + B->TPos.X*L;
  Out->PY = A->TPos.Y*R + B->TPos.Y*L;
  Out->LR = A->LR*R + B->LR*L;
  Out->LG = A->LG*R + B->LG*L;
  Out->LB = A->LB*R + B->LB*L;
}

void Vertex_Interpolate2D_L(Vertex *A,Vertex *B,Vertex *Out,float L)
{
  float R = 1.0f-L;
  Out->PX = A->PX*R + B->PX*L;
  Out->PY = A->PY*R + B->PY*L;

  Out->LR = A->LR*R + B->LR*L;
  Out->LG = A->LG*R + B->LG*L;
  Out->LB = A->LB*R + B->LB*L;
}

void Frust_Set_L()
{
  Frust_2DI = &Vertex_Interpolate2D_L;
  Frust_3DI = &Vertex_Interpolate3D_L;
}


void Vertex_Interpolate3D_Env(Vertex *A,Vertex *B,Vertex *Out,float L)
{
  float R = 1.0f-L;
  Out->PX = A->TPos.X*R + B->TPos.X*L;
  Out->PY = A->TPos.Y*R + B->TPos.Y*L;
  Out->RZ = 1.0f;
  Out->REU = A->EU*R + B->EU*L;
  Out->REV = A->EV*R + B->EV*L;
}

void Vertex_Interpolate2D_Env(Vertex *A,Vertex *B,Vertex *Out,float L)
{
  float R = 1.0f-L;
  Out->PX = A->PX*R + B->PX*L;
  Out->PY = A->PY*R + B->PY*L;

  Out->REU = A->REU*R + B->REU*L;
  Out->REV = A->REV*R + B->REV*L;
  Out->RZ = A->RZ*R + B->RZ*L;
}

void Frust_Set_Env()
{
  Frust_2DI = &Vertex_Interpolate2D_Env;
  Frust_3DI = &Vertex_Interpolate3D_Env;
}


void Vertex_Interpolate3D_UV(Vertex *A,Vertex *B,Vertex *Out,float L)
{
  float R = 1.0f-L;
  Out->PX = A->TPos.X*R + B->TPos.X*L;
  Out->PY = A->TPos.Y*R + B->TPos.Y*L;
  Out->RZ = 1.0f;
  Out->RU = A->U*R + B->U*L;
  Out->RV = A->V*R + B->V*L;
}

void Vertex_Interpolate2D_UV(Vertex *A,Vertex *B,Vertex *Out,float L)
{
  float R = 1.0f-L;
  Out->PX = A->PX*R + B->PX*L;
  Out->PY = A->PY*R + B->PY*L;

  Out->RU = A->RU*R + B->RU*L;
  Out->RV = A->RV*R + B->RV*L;
  Out->RZ = A->RZ*R + B->RZ*L;
}

void Frust_Set_UV()
{
  Frust_2DI = &Vertex_Interpolate2D_UV;
  Frust_3DI = &Vertex_Interpolate3D_UV;
}

void Vertex_Interpolate3D_UVL(Vertex *A,Vertex *B,Vertex *Out,float L)
{
  float R = 1.0f-L;
  Out->PX = A->TPos.X*R + B->TPos.X*L;
  Out->PY = A->TPos.Y*R + B->TPos.Y*L;
  Out->RZ = 1.0f;
  Out->RU = A->U*R + B->U*L;
  Out->RV = A->V*R + B->V*L;
  Out->LR = A->LR*R + B->LR*L;
  Out->LG = A->LG*R + B->LG*L;
  Out->LB = A->LB*R + B->LB*L;
}

void Vertex_Interpolate2D_UVL(Vertex *A,Vertex *B,Vertex *Out,float L)
{
  float R = 1.0f-L;
  Out->PX = A->PX*R + B->PX*L;
  Out->PY = A->PY*R + B->PY*L;

  Out->RU = A->RU*R + B->RU*L;
  Out->RV = A->RV*R + B->RV*L;
  Out->RZ = A->RZ*R + B->RZ*L;
  Out->LR = A->LR*R + B->LR*L;
  Out->LG = A->LG*R + B->LG*L;
  Out->LB = A->LB*R + B->LB*L;
}

void Frust_Set_UVL()
{
  Frust_2DI = &Vertex_Interpolate2D_UVL;
  Frust_3DI = &Vertex_Interpolate3D_UVL;
}

void UFE1(Vertex *A,Vertex *B,Vertex *C);
void UFE2(Vertex *A,Vertex *B,Vertex *C);
void UFE3(Vertex *A,Vertex *B,Vertex *C);
void UFE4(Vertex *A,Vertex *B,Vertex *C);

void Universal_Frustrum_Elite()
{
  DoFace->Clipper(); //Initiate global variables with Face clipper's requirements
  Rasterizer = DoFace->Filler;
  Vertex *A = DoFace->A,*B = DoFace->B,*C = DoFace->C,*T;

  if (A->TPos.Z>B->TPos.Z) {T=A; A=B; B=T;}
  if (A->TPos.Z>C->TPos.Z) {T=A; A=C; C=T;}
  if (B->TPos.Z>C->TPos.Z) {T=B; B=C; C=T;}

  if (A->TPos.Z>=1.0f) {UFE1(A,B,C); return;}
  if (C->TPos.Z<1.0f) return;
  if (B->TPos.Z<1.0f)
  {
    Frust_3DI(B,C,F_VB  ,(B->TPos.Z-1.0f)/(B->TPos.Z-C->TPos.Z));
    Frust_3DI(A,C,F_VB+1,(A->TPos.Z-1.0f)/(A->TPos.Z-C->TPos.Z));
    UFE1(F_VB,F_VB+1,C);
    return;
  }
  Frust_3DI(A,B,F_VB  ,(A->TPos.Z-1.0f)/(A->TPos.Z-B->TPos.Z));
  Frust_3DI(A,C,F_VB+1,(A->TPos.Z-1.0f)/(A->TPos.Z-C->TPos.Z));
  UFE1(F_VB,F_VB+1,B);
  UFE1(F_VB+1,B,C);
}

// 2D clipping Phase one : X=0.
void UFE1(Vertex *A,Vertex *B,Vertex *C)
{
  Vertex *T;
  if (A->PX>B->PX) {T=A; A=B; B=T;}
  if (A->PX>C->PX) {T=A; A=C; C=T;}
  if (B->PX>C->PX) {T=B; B=C; C=T;}

  if (C->PX<0) return;
  if (A->PX>=0) {UFE2(A,B,C); return;}

  if (B->PX<0)
  {
    Frust_2DI(A,C,F_VB+2,A->PX/(A->PX-C->PX));
    Frust_2DI(B,C,F_VB+3,B->PX/(B->PX-C->PX));
    UFE2(F_VB+2,F_VB+3,C);
    return;
  }
  Frust_2DI(A,B,F_VB+2,A->PX/(A->PX-B->PX));
  Frust_2DI(A,C,F_VB+3,A->PX/(A->PX-C->PX));
  UFE2(F_VB+2,F_VB+3,B);
  UFE2(F_VB+3,B,C);
}

void UFE2(Vertex *A,Vertex *B,Vertex *C)
{
  float XClip = XRes_1;

  if (A->PX>XClip) return;
  if (C->PX<=XClip) {UFE3(A,B,C); return;}

  if (B->PX>XClip)
  {
    Frust_2DI(A,B,F_VB+4,(XClip-A->PX)/(B->PX-A->PX));
    Frust_2DI(A,C,F_VB+5,(XClip-A->PX)/(C->PX-A->PX));
    UFE3(A,F_VB+4,F_VB+5);
    return;
  }
  Frust_2DI(A,C,F_VB+4,(XClip-A->PX)/(C->PX-A->PX));
  Frust_2DI(B,C,F_VB+5,(XClip-B->PX)/(C->PX-B->PX));
  UFE3(B,F_VB+4,F_VB+5);
  UFE3(A,B,F_VB+4);
}
void UFE3(Vertex *A,Vertex *B,Vertex *C)
{
  Vertex *T;

  if (A->PY>B->PY) {T=A; A=B; B=T;}
  if (A->PY>C->PY) {T=A; A=C; C=T;}
  if (B->PY>C->PY) {T=B; B=C; C=T;}

  if (C->PY<0) return;
  if (A->PY>=0) {UFE4(A,B,C); return;}
  if (B->PY<0)
  {
    Frust_2DI(A,C,F_VB+6,A->PY/(A->PY-C->PY));
    Frust_2DI(B,C,F_VB+7,B->PY/(B->PY-C->PY));
    UFE4(F_VB+6,F_VB+7,C);
    return;
  }
  Frust_2DI(A,B,F_VB+6,A->PY/(A->PY-B->PY));
  Frust_2DI(A,C,F_VB+7,A->PY/(A->PY-C->PY));
  UFE4(F_VB+6,F_VB+7,B);
  UFE4(F_VB+7,B,C);
}
void UFE4(Vertex *A,Vertex *B,Vertex *C)
{
  float YClip = YRes_1;

  if (C->PY<=YClip) {Rasterizer(A,B,C); return;}
  if (A->PY>YClip) return;
  if (B->PY>YClip)
  {
    Frust_2DI(A,B,F_VB+8,(YClip-A->PY)/(B->PY-A->PY));
    Frust_2DI(A,C,F_VB+9,(YClip-A->PY)/(C->PY-A->PY));
    Rasterizer(A,F_VB+8,F_VB+9);
    return;
  }
  Frust_2DI(A,C,F_VB+8,(YClip-A->PY)/(C->PY-A->PY));
  Frust_2DI(B,C,F_VB+9,(YClip-B->PY)/(C->PY-B->PY));
  Rasterizer(B,F_VB+8,F_VB+9);
  Rasterizer(A,B,F_VB+8);
}