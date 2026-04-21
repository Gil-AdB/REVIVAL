#include <Math.H>
#include <StdIO.H>
#include <StdLib.H>
#include <String.H>

#include "Source\FDS_Defs.H"     // Definitions
#include "Source\FDS_Vars.H"     // Variables and structures
#include "Source\FDS_Decs.H"     // Declarations

// Explosion related particles, not associated with scene.
Particle *ExpPcls;
long NumExpPcls;

static long SceneFlags = 0;


extern "C" void Seven_UP(int X,int Y,int SizeX,int SizeY,char *Txtr,char *Page);
#pragma aux Seven_UP parm [eax] [ebx] [ecx] [edx] [esi] [edi] modify exact [eax ebx ecx edx esi edi]

void Loxodrome(float K,float t,Vector *Out)
{
  float e,y,a;
  if (t<0)
  {
    Loxodrome(K,-t,Out);
    Out->Y *= -1;
    Out->Z *= -1;
    return;
  }
  e = exp(-t);
  y = sqrt(1-e*e);
  a = K*(e-0.5*log(2.0/(1.0-y)-1.0)-1.0);
  Out->X = e*cos(a);
  Out->Y = y;
  Out->Z = e*sin(a);
}


void Rast_Explosion_Pcl(Vertex *A,Vertex *B,Vertex *C)
{
  char *Pixel;
  long I,J;
  if (A->PX>=1&&A->PX<=XRes-2&&A->PY>=1&&A->PY<=YRes_1-1)
  {
    //addative 32bit 5 putpixels (currently set on 1)
    Pixel = VPage+((int)A->PX+YOffs[(int)A->PY]<<2);
    Pixel -= XRes<<2;
    J = (*Pixel)+A->LB;
    if (J>255) *Pixel++=255; else *Pixel++=J;
    J = (*Pixel)+A->LG;
    if (J>255) *Pixel++=255; else *Pixel++=J;
    J = (*Pixel)+A->LR;
    if (J>255) *Pixel++=255; else *Pixel++=J;
    Pixel += (XRes<<2)-7;
    J = (*Pixel)+A->LB;
    if (J>255) *Pixel++=255; else *Pixel++=J;
    J = (*Pixel)+A->LG;
    if (J>255) *Pixel++=255; else *Pixel++=J;
    J = (*Pixel)+A->LR;
    if (J>255) *Pixel++=255; else *Pixel++=J;
    Pixel++;
    J = (*Pixel)+((int)A->LB<<1);
    if (J>255) *Pixel++=255; else *Pixel++=J;
    J = (*Pixel)+((int)A->LG<<1);
    if (J>255) *Pixel++=255; else *Pixel++=J;
    J = (*Pixel)+((int)A->LR<<1);
    if (J>255) *Pixel++=255; else *Pixel++=J;
    Pixel++;
    J = (*Pixel)+A->LB;
    if (J>255) *Pixel++=255; else *Pixel++=J;
    J = (*Pixel)+A->LG;
    if (J>255) *Pixel++=255; else *Pixel++=J;
    J = (*Pixel)+A->LR;
    if (J>255) *Pixel++=255; else *Pixel++=J;
    Pixel += (XRes<<2)-7;
    J = (*Pixel)+A->LB;
    if (J>255) *Pixel++=255; else *Pixel++=J;
    J = (*Pixel)+A->LG;
    if (J>255) *Pixel++=255; else *Pixel++=J;
    J = (*Pixel)+A->LR;
    if (J>255) *Pixel++=255; else *Pixel++=J;
  }
}

//renders all of the explosion based particles on screen
//this is a full rendering pipe, ignores the main pipe,
//should go right after it
void Explosion_Particles()
{
  long I,J;
  char *Pixel;
  Vector dV,Pos,V,N;

  Particle *P;
  // explosion decay
  for(I=0;I<NumExpPcls;I++)
  {
    P = ExpPcls+I;
    if (P->Charge<=0.0) continue;
    Vector_Scale(&P->Vel,dTime,&dV);
    Vector_Add(&P->V.Pos,&dV,&P->V.Pos);

    Vector_Sub(&P->V.Pos,&View->ISource,&N);
    MatrixXVector(View->Mat,&N,&P->V.TPos);

    if (P->V.TPos.Z>=1.0)
    {
      P->V.RZ = 1.0/P->V.TPos.Z;
      P->V.PX = CntrX+FOVX*P->V.TPos.X*P->V.RZ;
      P->V.PY = CntrY-FOVY*P->V.TPos.Y*P->V.RZ;
//      P->PRad = 0;//FOVX*P->Radius*RZ;
      P->V.Flags = 0;
    } else P->V.Flags |= Vtx_VisNear;
/*      if (P->V.PX>=1&&P->V.PX<=XRes-2&&P->V.PY>=1&&P->V.PY<=YRes_1-1)
      {
        //addative 32bit 5 putpixels (currently set on 1)

        Pixel = VPage+((int)P->V.PX+YOffs[(int)P->V.PY]<<2);
        Pixel -= XRes<<2;
        J = (*Pixel)+((int)(P->Color.B*P->Charge)>>5);
        if (J>255) *Pixel++=255; else *Pixel++=J;
        J = (*Pixel)+((int)(P->Color.G*P->Charge)>>5);
        if (J>255) *Pixel++=255; else *Pixel++=J;
        J = (*Pixel)+((int)(P->Color.R*P->Charge)>>5);
        if (J>255) *Pixel++=255; else *Pixel++=J;
        Pixel += (XRes<<2)-7;
        J = (*Pixel)+((int)(P->Color.B*P->Charge)>>5);
        if (J>255) *Pixel++=255; else *Pixel++=J;
        J = (*Pixel)+((int)(P->Color.G*P->Charge)>>5);
        if (J>255) *Pixel++=255; else *Pixel++=J;
        J = (*Pixel)+((int)(P->Color.R*P->Charge)>>5);
        if (J>255) *Pixel++=255; else *Pixel++=J;
        Pixel++;
        J = (*Pixel)+((int)(P->Color.B*P->Charge)>>4);
        if (J>255) *Pixel++=255; else *Pixel++=J;
        J = (*Pixel)+((int)(P->Color.G*P->Charge)>>4);
        if (J>255) *Pixel++=255; else *Pixel++=J;
        J = (*Pixel)+((int)(P->Color.R*P->Charge)>>4);
        if (J>255) *Pixel++=255; else *Pixel++=J;
        Pixel++;
        J = (*Pixel)+((int)(P->Color.B*P->Charge)>>5);
        if (J>255) *Pixel++=255; else *Pixel++=J;
        J = (*Pixel)+((int)(P->Color.G*P->Charge)>>5);
        if (J>255) *Pixel++=255; else *Pixel++=J;
        J = (*Pixel)+((int)(P->Color.R*P->Charge)>>5);
        if (J>255) *Pixel++=255; else *Pixel++=J;
        Pixel += (XRes<<2)-7;
        J = (*Pixel)+((int)(P->Color.B*P->Charge)>>5);
        if (J>255) *Pixel++=255; else *Pixel++=J;
        J = (*Pixel)+((int)(P->Color.G*P->Charge)>>5);
        if (J>255) *Pixel++=255; else *Pixel++=J;
        J = (*Pixel)+((int)(P->Color.R*P->Charge)>>5);
        if (J>255) *Pixel++=255; else *Pixel++=J;
      }
    }*/

    // dummy illumination model
    P->V.LB = P->Color.B*P->Charge*0.03;
    P->V.LG = P->Color.G*P->Charge*0.03;
    P->V.LR = P->Color.R*P->Charge*0.03;

    P->Charge -= dTime*0.5;
  }
}
