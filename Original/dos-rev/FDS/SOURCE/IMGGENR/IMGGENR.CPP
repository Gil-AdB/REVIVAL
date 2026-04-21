// Image Generation Routines

#include <StdIO.H>
#include <StdLib.H>
#include <Math.H>
#include <String.H>
#include <DOS.H>



#include "..\FDS_Defs.H"
#include "..\FDS_Vars.H"
#include "..\FDS_Decs.H"

DWord WOBPOINTSHEIGHT;
Material Flare_IR,Flare_IG,Flare_IB;
char NoiseTblRdy = 0;
float *NoiseTbl;      // Range [0,+2PI] acc 0.01

DWord Colors32[256]; // a 32bit 256 color palette. For use with intensity
                     // map effects.

Vector *WPDT = NULL;
float WRFOV;

void New_Image(Image *Img,long X,long Y)
{
  Img->X = X; Img->Y = Y; Img->Data = new DWord[X*Y];
}

// Return Pseudorandom value between -1 and 1. Requires a polynomial
// seed given at x.

float frand1d(long x)
{
  long s = 71 * x; s = s * 8192 ^ s;
  return 1.0 - ((s*(s*s*15731+789221)+1376312589)& 0x7fffffff)/1073741824.0;
}

float Noise1D(float x)
{
  long i;
  float f,n0,n1;

  i = floor(x);
  f = x - i;
  n0 = frand1d(i);
  n1 = frand1d(i+1);

  return n0 + (n1-n0)*f;
}

void Make_Noise()
{
  long I;
  float f;
  if (!NoiseTblRdy)
  {
    NoiseTblRdy = 1;
    NoiseTbl = new float[6284];
  }
  for(I=0;I<=6283;I++)
  {
    f = I/100.0;
    NoiseTbl[I] = Noise1D(f);
  }
}


// Generates a Lens Flare at the target Image, the main color is at [RGB].
// All additional variables are considered constant.
void Generate_Flare_Image_Test(Image *Img,float FR,float FG,float FB,float blah,float XDec,float YDec)
{
  signed long X, Y;
  char  Col;
  float D, R, G, B, F, V ,A ,Bump;
  float Y2;
  float Linear = 0.03;
  float Gauss  = 0.006;
  float Mix    = 0.5;
  float NumLines = 0.1;
  float NoiseFreq = 100.0*blah;
  float NoiseMod = 2;
  float Falloff = 0.01;
  float RMid = 27;
  float RWid = 1.6;
  float Blaze = 0.2;
  float Ring,RMin,RMax,rRW;

  char *Pen = (char *)Img->Data;
  float Rx,Ry;

  if (!NoiseTblRdy) Make_Noise();

  // ring position and width

  Rx = Img->X>>1;
  Ry = -(Img->Y>>1);
  for (Y=-Ry; Y>Ry; Y--)
  {
    Y2=(Y/*-YDec*/)*(Y/*-YDec*/);
    for (X=-Rx; X<Rx; X++)
    {
      // Get distance from center of flare
      D = ((X/*-XDec*/)*(X/*-XDec*/) + Y2);

      // Creates random lines out from the center.
      V = atan2(X-XDec, Y-YDec)+PI; //Angle
      V = (fmod(V/PI*NumLines + 1.0 + NoiseTbl[(int)(V*NoiseFreq)]
      //Noise1D(V*NoiseFreq)
      , 1.0) - 0.5)*NoiseMod;
      V = fabs(V);
//      V *= V*V*V*V;
//      V = pow(fabs(V), 1.5);

      // Add lines and fade out over distance.
      A = Blaze*V / (1.0 + D*Falloff*0.04);

      // Clip to maximum value
      if (A>1) A = 1;
      Col = 255.0 * A;
      *Pen++ = Col;
      *Pen++ = Col;
      *Pen++ = Col;
      Pen++; //Alpha channel
    }
  }
}


// Generates a Lens Flare at the target Image, the main color is at [RGB].
// All additional variables are considered constant.
void Generate_Flare_Image(Image *Img,float FR,float FG,float FB)
{
  signed long X, Y;
  float D, R, G, B, F, V ,A ,Bump;
  float Y2;
  float Linear = 0.04;
  float Gauss  = 0.002;
  float Mix    = 0.5;
  float NumLines = 0.2;
  float NoiseFreq = 1000.0;
  float NoiseMod = 2;
  float Falloff = 0.05;
  float RMid = 27;
  float RWid = 1.6;
  float Blaze = 0.35;
  float Ring,RMin,RMax,rRW;

  char *Pen = (char *)Img->Data;
  float Rx,Ry;

  if (!NoiseTblRdy) Make_Noise();

  // ring position and width
  RMax = RMid + RWid;
  RMin = RMid - RWid;
  rRW = 1.0/RWid;

  Rx = Img->X>>1;
  Ry = -(Img->Y>>1);
  for (Y=-Ry; Y>Ry; Y--)
  {
    Y2=Y*Y;
    for (X=-Rx; X<Rx; X++)
    {
      // Get distance from center of flare
      D = sqrt(X*X + Y2)*0.3;
      Bump = exp(-D*Linear)*(1-Mix)-0.1;
      if (Bump<0) Bump=0.0;
      A = -D*D*D*Gauss;
      if (A>-7.0)
      {
        A = 8.0*exp(A)*Mix;
        R = (A+Bump)*FR;
        G = (A+Bump)*FG;
        B = (A+Bump)*FB;
      } else {
        R = Bump*FR;
        G = Bump*FG;
        B = Bump*FB;
      }
      if (R>=1.0&&G>=1.0&&B>=1.0) {*(unsigned long *)Pen = 0x00FFFFFF; Pen+=4; continue;}

      if (D<RMin || D>RMax)
        Ring = 0;
      else
      {
        Ring = fabs(D-RMid)*rRW;
        Ring = 1.0 - Ring*Ring*(3 - 2*Ring); //Polynom shit
        Ring *= 0.10;
      }

      R += Ring*FR;
      G += Ring*FG;
      B += Ring*FB;

      // Creates random lines out from the center.
      V = atan2(X, Y)+PI; //Angle
      V = (fmod(V/PI*NumLines + 1.0 + NoiseTbl[(int)(V*NoiseFreq)]
      //Noise1D(V*NoiseFreq)
      , 1.0) - 0.5)*NoiseMod;
      V *= V;
      //V = pow(fabs(V), 1.5);
      //V = fabs(V);

      // Add lines and fade out over distance.
      A = Blaze*V / (1.0 + D*D*Falloff);
      R+=A;
      G+=A;
      B+=A;

      // Clip to maximum value
      if (R>1) R = 1;
      if (G>1) G = 1;
      if (B>1) B = 1;

      *Pen++ = 255.0 * B;
      *Pen++ = 255.0 * G;
      *Pen++ = 255.0 * R;
      Pen++; //Alpha channel
    }
  }
}

void Generate_RGBFlares()
{
  Image Img;
  Img.X = 256;
  Img.Y = 256;
  Flare_IR.Flags = Mat_Virtual;
  Flare_IR.Txtr = new Texture;
  Flare_IR.Txtr->BPP = BPP;
  Img.Data = new DWord[256*256];
  Generate_Flare_Image(&Img,1.0,0.4,0.2);
  Convert_Image2Texture(&Img,Flare_IR.Txtr);
  Flare_IG.Flags = Mat_Virtual;
  Flare_IG.Txtr = new Texture;
  Flare_IG.Txtr->BPP = BPP;
  Generate_Flare_Image(&Img,0.3,1.0,0.3);
  Convert_Image2Texture(&Img,Flare_IG.Txtr);
  Flare_IB.Flags = Mat_Virtual;
  Flare_IB.Txtr = new Texture;
  Flare_IB.Txtr->BPP = BPP;
  Generate_Flare_Image(&Img,0.2,0.4,1.0);
  Convert_Image2Texture(&Img,Flare_IB.Txtr);
  delete Img.Data;
}

void Fit_FlareImage(Omni *O)
{
  if ((O->L.R>=O->L.G)&&(O->L.R>=O->L.B)) O->F.Txtr = &Flare_IR;
    else if ((O->L.G>=O->L.R)&&(O->L.G>=O->L.B)) O->F.Txtr = &Flare_IG;
      else O->F.Txtr = &Flare_IB;
}

// Fractal Noise (Colour Plasma) generator

char *FrcNoise;
float FrcNoise_Grain;
static long FN_X;

long Linear_Rand(long Base,long Limit)
{
  long L = Base-Limit+(rand()*Limit>>14);
  if (L<0) L=0;
  if (L>255) L=255;
  return L;
}

void Fract_Noise(long X1,long Y1,long X2,long Y2)
{
  long X,Y;
  char C11,C12,C21,C22;

  if (X2-X1<2&&Y2-Y1<2) return;
  X = X1+X2>>1;
  Y = Y1+Y2>>1;
  C11 = FrcNoise[(X1+Y1*FN_X)<<2];
  C12 = FrcNoise[(X2+Y1*FN_X)<<2];
  C21 = FrcNoise[(X1+Y2*FN_X)<<2];
  C22 = FrcNoise[(X2+Y2*FN_X)<<2];
  if (!FrcNoise[(X+Y1*FN_X)<<2]) FrcNoise[(X+Y1*FN_X)<<2] = Linear_Rand(C11+C12>>1,(X2-X1)*FrcNoise_Grain);
  if (!FrcNoise[(X2+Y*FN_X)<<2]) FrcNoise[(X2+Y*FN_X)<<2] = Linear_Rand(C12+C22>>1,(Y2-Y1)*FrcNoise_Grain);
  if (!FrcNoise[(X+Y2*FN_X)<<2]) FrcNoise[(X+Y2*FN_X)<<2] = Linear_Rand(C22+C21>>1,(X2-X1)*FrcNoise_Grain);
  if (!FrcNoise[(X1+Y*FN_X)<<2]) FrcNoise[(X1+Y*FN_X)<<2] = Linear_Rand(C21+C11>>1,(Y2-Y1)*FrcNoise_Grain);
  if (!FrcNoise[(X+Y *FN_X)<<2]) FrcNoise[(X+Y*FN_X)<<2] = C11+C12+C21+C22>>2;//Linear_Rand(C11+C12+C21+C22>>2,(X2-X1)*FrcNoise_Grain*0.5);

  Fract_Noise(X1,Y1,X,Y);
  Fract_Noise(X,Y1,X2,Y);
  Fract_Noise(X,Y,X2,Y2);
  Fract_Noise(X1,Y,X,Y2);
}

// note : Also generates noise within alpha channel
void Generate_Fractal_Noise(Image *Img,float Graininess)
{
  long I;
  FrcNoise = (char *)Img->Data;
  FrcNoise_Grain = Graininess;
  FN_X = Img->X;
  I=4;
  while (I--)
  {
    FrcNoise[0] = Linear_Rand(128,64);
    FrcNoise[(FN_X-1)<<2] = Linear_Rand(128,64);
    FrcNoise[((Img->Y-1)*FN_X)<<2] = Linear_Rand(128,64);
    FrcNoise[(Img->Y*FN_X-1)<<2] = Linear_Rand(128,64);
    Fract_Noise(0,0,Img->X-1,Img->Y-1);
    FrcNoise++;
  }
}

static long xseed;

unsigned char cfunc (short x)
{
  if (x&0xff00)
  {
    if (x<0) return 0; else return 255;
  } else return x;
}

short int oldrand (void)
{
  xseed=0x015a4e35*xseed+1;
  return (xseed>>16);
}

int a[55];
int aj;

short int the_rand (void)
{
  aj=(aj+1)%55;
  a[aj]=(a[(aj+1)%55]^a[(aj+2)%55]);
  return (a[aj]);
}

void initrand (void)
{
  aj=0;
  for (int x=0; x<55; x++ )
  {
    a[x]=oldrand();
  }
}

char gb (char *buf, long x)
{
//  cout << hex << x << '\n';
  return buf[x<<2];
}

void Generate_Plasma(Image *Img, short p, long seed)
{
  char *buf = (char *)Img->Data;
  xseed=seed;
  initrand();
  buf[0]=the_rand()&255;

  unsigned long mx2=0x100;
  unsigned long my2=0x10000l;
  unsigned long x,y;

  do {
    unsigned long mx=mx2>>1;
    unsigned long my=my2>>1;
    for (y=0; y<0x10000l; y+=my2)
    for (x=0; x<0x100; x+=mx2)
      {
        short c=2+
             gb(buf,x+y)+
             gb(buf,x+((y+my2)&0xffffl))+
             gb(buf,((x+mx2)&255)+y)+
             gb(buf,((x+mx2)&255)+((y+my2)&0xffffl))+
             ((the_rand()*p*3)>>16);
        buf[(x+mx+y+my)<<2]=cfunc (c>>2);
      }
    for (y=0; y<0x10000l; y+=my2)
    for (x=0; x<0x100; x+=mx2)
      {
        short c=2+
              gb(buf, (((x-mx)&255)+y+my))+
              gb(buf, (x+y))+
              gb(buf, (x+mx+y+my))+
              gb(buf, (x+((y+my2)&0xffffl)));
              ((the_rand()*p)>>16);
        buf[(x+y+my)<<2]=cfunc (c>>2);
      }
    for (y=0; y<0x10000l; y+=my2)
    for (x=0; x<0x100; x+=mx2)
      {
        short c=2+
              gb(buf,x+y)+
              gb(buf,x+mx+((y-my)&0xffffl))+
              gb(buf,((x+mx2)&255)+y)+
              gb(buf,x+mx+y+my);
              ((the_rand()*p)>>16);
        buf[(x+mx+y)<<2]=cfunc(c>>2);
      }
    mx2=mx;
    my2=my;
    p>>=1;
  } while (mx2>=2);
}


// Grid effect generator. Must receive a 256x256 image.

// Interpolation accuracy limit : 8x8 Pixel Granularity.
#define Grid_LOGSubsamp 3
#define Grid_Subsamp 8

long Grid_X,Grid_Y,Grid_XY;
long AGrid_X,AGrid_Y,AGrid_XY;

void Init_PlanarGrid(GridPoint **GP)
{
  Grid_X = (XRes>>Grid_LOGSubsamp)+1;
  Grid_Y = (YRes>>Grid_LOGSubsamp)+1;
  Grid_XY = Grid_X*Grid_Y;
  if (WPDT) delete WPDT;
  WPDT = new Vector[Grid_XY];
  (*GP) = new GridPoint[Grid_X*Grid_Y];
}

// Maps the given effect from GP and Img as a texture, into the DAC
// (Truecolor 32) Surface directly referred as DAC.
/*void Grid_Texture_Mapper(GridPoint *GP,Image *Img,DWord *DAC)
{
  long I,J,X,Y;
  GridPoint *P11 = GP,*P12,*P21,*P22;
  long dU1,dV1,dU2,dV2;
  long U1,V1,U2,V2;
  long dU,dV,iU,iV;
  DWord *XO,*YO = DAC;
  DWord *Ptr,*Data = Img->Data;
  long J1 = XRes-Grid_Subsamp,J2 = YOffs[Grid_Subsamp];

  for(J=0;J<YRes;J+=Grid_Subsamp)
  {
    XO = YO;
    for(I=0;I<XRes;I+=Grid_Subsamp)
    {
      //P11 holds the currently rendered Grid point
      P12 = P11+1;
      P21 = P11+Grid_X;
      P22 = P21+1;

      //Forward differencing (Linear Interpolation rates)
      U1 = P11->U; V1 = P11->V;
      U2 = P12->U; V2 = P12->V;

      dU1 = (P21->U-U1)>>Grid_LOGSubsamp;
      dV1 = (P21->V-V1)>>Grid_LOGSubsamp;
      dU2 = (P22->U-U2)>>Grid_LOGSubsamp;
      dV2 = (P22->V-V2)>>Grid_LOGSubsamp;

      Ptr = XO;
      for(Y=0;Y<Grid_Subsamp;Y++)
      {
        // Inner linear interpolation rates
        iU = U1;
        iV = V1;
        dU = U2-iU>>Grid_LOGSubsamp;
        dV = V2-iV>>Grid_LOGSubsamp;
        for(X=0;X<Grid_Subsamp;X++)
        {
          // Inner Rasterization loop (24:8 texture)
           *Ptr++ = Data[(iV&0xFF00)+((iU&0xFF00)>>8)];
          iU+=dU; iV+=dV;
        }
        Ptr += J1;
        U1 += dU1;
        V1 += dV1;
        U2 += dU2;
        V2 += dV2;
      }
      XO += Grid_Subsamp;
      P11++;
    }
    P11++;
    YO+=J2;
  }
} */

extern "C" void Grid_Texture_MapASM(char *Texture,GridPoint *Points,char *VirScr);
#pragma aux Grid_Texture_MapASM parm [ecx] [esi] [edi] modify exact [eax ebx ecx edx esi edi];

void Grid_Texture_Mapper(GridPoint *GP,Image *Img,DWord *DAC)
{
  Grid_Texture_MapASM((char *)Img->Data,GP,(char *)DAC);
}


void GridFX_Test()
{
  long I,J;
  GridPoint *GP,*HP;
  Image Img;
  Init_PlanarGrid(&GP);
  HP=GP;
  for(J=0;J<Grid_Y;J++)
    for(I=0;I<Grid_X;I++)
    {
      HP->U = I*I<<6;
      HP->V = J*J<<6;
      HP++;
    }
  Img.X = 256;
  Img.Y = 256;
  Img.Data = new DWord[256*256];
  Generate_Flare_Image(&Img,1.0,0.8,0.2);
  Grid_Texture_Mapper(GP,&Img,(DWord *)VPage);
}

long WW_Spots,WW_CS;
float WW_Grav,WW_Pulse;
AGridPoint *WW_Grid,*WW_GO;
AGridPoint *WW_SP;




// Initialize the internal particle system for the whirlwind effect.
// This occurs in Texel space. WW_SP will then be used to store main
// Gravity generators. All particles Repulse each other.
void Init_Whirlwind(char Spots,float Grav,float Repulse)
{
  WW_Grid = Init_Antiwobbler();
  WW_GO = Init_Antiwobbler();
  WW_Spots = Spots;
  WW_CS = 0;
  WW_SP = new AGridPoint[Spots];
  WW_Grav = Grav;
  WW_Pulse = Repulse;
}

// Initializes the next Gravity center.
void Init_WWGrav(float U,float V)
{
  if (WW_CS==WW_Spots) return;
  WW_SP[WW_CS].X = U;
  WW_SP[WW_CS].Y = V;
  WW_CS++;
}

// Repositions the particles in a Grid shape in the [0,256]x[0,256] limit
void Reset_WWGrid()
{
  AW_Staticmap(WW_Grid);
}

void Update_WWGrid()
{
  long I,J;
//  GridPoint *V,*W;
//  GridPoint F,U;
  float D;
  const float Pulse_Away = 0.001;
  const float Grav_Away = 0.1;

  struct {
    float X,Y;
  } *V,*W,F,U;

  for(I=0;I<AGrid_XY;I++)
  {
    //Calculate Force
    F.X = 0; F.Y = 0;
    // Repulsion
    for(J=0;J<AGrid_XY;J+=4)
    {
      if (J==I) continue;
      U.X = (WW_Grid[J].X - WW_Grid[I].X);
      U.Y = (WW_Grid[J].Y - WW_Grid[I].Y);
      D = WW_Pulse/(U.X*U.X+U.Y*U.Y+Pulse_Away);
      F.X -= D*U.X;
      F.Y -= D*U.Y;
    }
    // Grav centers (cities)
    for(J=0;J<WW_Spots;J++)
    {
      U.X = (WW_SP[J].X - WW_Grid[I].X);
      U.Y = (WW_SP[J].Y - WW_Grid[I].Y);
      D = WW_Grav/(U.X*U.X+U.Y*U.Y+Grav_Away);
      F.X += D*U.X;
      F.Y += D*U.Y;
    }
    D = 1.0/sqrt(F.X*F.X+F.Y*F.Y);
    WW_GO[I].X = WW_Grid[I].X + F.X*D;
    WW_GO[I].Y = WW_Grid[I].Y + F.Y*D;
  }
  memcpy(WW_Grid,WW_GO,AGrid_XY*sizeof(GridPoint));
}

void Display_WWGrid(Image *Img)
{
  AntiGrid_Mapper(WW_Grid,Img,(DWord *)VPage);
}

// Texture Space Grid effect will be Implemented here.
#define Awob_Gran 8
#define Awob_LOGGran 3


AGridPoint * Init_Antiwobbler()
{
  AGridPoint *AGP;
  AGrid_X = (256>>Awob_LOGGran)+1;
  AGrid_Y = (256>>Awob_LOGGran)+1;
  AGrid_XY = AGrid_X*AGrid_Y;
  AGP = new AGridPoint[AGrid_X*AGrid_Y];
  return AGP;
}

extern "C" void P_Texture_32(Bi_Triangle * BT, char * Bitmap,unsigned int u);
#pragma aux P_Texture_32 parm [ebx] [eax] [ecx] modify exact [eax ebx ecx edx esi edi 8087];

void AW_Staticmap(AGridPoint *AGP)
{
  long I,J;
  AGridPoint *GP = AGP;
  float QX=XRes*0.1;
  float QY=YRes*0.1;
  float RX=XRes*0.8;
  float RY=YRes*0.8;
  for(J=0;J<AGrid_Y;J++)
    for(I=0;I<AGrid_X;I++)
    {
      GP->X = QX+I*RX/(float)(AGrid_X-1);
      GP->Y = QY+J*RY/(float)(AGrid_Y-1);
      GP++;
    }
}

void AW_Distort(AGridPoint *AGP,float Bang)
{
  long I,J;
  AGridPoint *GP = AGP;
  float ratio = Bang/16384.0;
  for(J=0;J<AGrid_Y;J++)
    for(I=0;I<AGrid_X;I++)
    {
      GP->X += (rand()-16384)*ratio;
      GP->Y += (rand()-16384)*ratio;
      GP++;
    }
}

//Reads Grid situation from a Particle array.
//Useful to make them move along more complex paths
void AW_FromParticles(AGridPoint *AGP,Particle *P)
{
  long I,J;
  for(J=0;J<AGrid_Y;J++)
    for(I=0;I<AGrid_X;I++)
    {
      AGP->X = P->V.Pos.X;
      AGP->Y = P->V.Pos.Y;
      AGP++;
      P++;
    }
}

// Image must be 256x256
void AntiGrid_Mapper(AGridPoint *AGP,Image *Img,DWord *DAC)
{
  Bi_Triangle BT[3]; // sent to affine mapper
  long U,V;
  AGridPoint *GP=AGP,*P12=GP+1,*P21=GP+AGrid_X,*P22=P12+AGrid_X;
  char *Bitmap = (char *)Img->Data;
  long Target = (long)DAC;

  BT[0].Z=BT[1].Z=BT[2].Z = 1;

  for(V=0;V<256;V+=Awob_Gran)
  {
    for(U=0;U<256;U+=Awob_Gran)
    {
      BT[0].X = GP->X;  BT[0].Y = GP->Y;
      BT[0].U = U;      BT[0].V = V;
      BT[1].X = P12->X; BT[1].Y = P12->Y;
      BT[1].U = U+Awob_Gran; BT[1].V = V;
      BT[2].X = P22->X; BT[2].Y = P22->Y;
      BT[2].U = U+Awob_Gran; BT[2].V = V+Awob_Gran;
      P_Texture_32(BT,Bitmap,Target);

      memcpy(BT+1,BT+2,sizeof(Bi_Triangle));
      BT[2].X = P21->X; BT[2].Y = P21->Y;
      BT[2].U = U;      BT[2].V = V+Awob_Gran;
      P_Texture_32(BT,Bitmap,Target);

      GP++; P12++; P21++; P22++;
    }
    GP++; P12++; P21++; P22++;
  }

}

// Fractal Generators
void Generate_Julia_Fractal(Image *Img,Complex *C,Complex *R1,Complex *R2)
{
  Complex L,Z,Z2;
  float dX,dY;
  long I,J;
  DWord *Ptr = Img->Data;
  long Orb;

  L.Y = R2->Y;
  dX = (R2->X-R1->X)/(float)Img->X;
  dY = (R1->Y-R2->Y)/(float)Img->Y;
  J = Img->Y;
  while(J--)
  {
    I = Img->X;
    L.X = R1->X;
    while (I--)
    {
      Z.X = L.X; Z.Y = L.Y;
      Orb=32;
      Z2.X = Z.X*Z.X;
      Z2.Y = Z.Y*Z.Y;
      do
      {
        Z.Y *= 2.0*Z.X;
        Z.Y += C->Y;
        Z.X = Z2.X-Z2.Y+C->X;
        Z2.X = Z.X*Z.X;
        Z2.Y = Z.Y*Z.Y;
      } while (--Orb&&Z2.X<=4.0&&Z2.Y<=4.0);
      *Ptr++ = Colors32[31-Orb];
      L.X += dX;
    }
    L.Y += dY;
  }
}

void Generate_Mandelbrot_Fractal(Image *Img,Complex *R1,Complex *R2)
{
  Complex L,Z,Z2;
  float dX,dY;
  long I,J;
  DWord *Ptr = Img->Data;
  long Orb;

  L.Y = R2->Y;
  dX = (R2->X-R1->X)/(float)Img->X;
  dY = (R1->Y-R2->Y)/(float)Img->Y;
  J = Img->Y;
  while(J--)
  {
    I = Img->X;
    L.X = R1->X;
    while (I--)
    {
      Z.X = L.X; Z.Y = L.Y;
      Orb=32;
      Z2.X = Z.X*Z.X;
      Z2.Y = Z.Y*Z.Y;
      do
      {
        Z.Y *= 2.0*Z.X;
        Z.Y += L.Y;
        Z.X = Z2.X-Z2.Y+L.X;
        Z2.X = Z.X*Z.X;
        Z2.Y = Z.Y*Z.Y;
      } while (--Orb&&Z2.X<=4.0&&Z2.Y<=4.0);
      *Ptr++ = Colors32[31-Orb];
      L.X += dX;
    }
    L.Y += dY;
  }
}

// Degree 0 Pheonix, Check this out
void Generate_Phoenix_Fractal(Image *Img,float P1,float P2,Complex *R1,Complex *R2)
{
  Complex L,X,Y,Z,Z2;
  float dX,dY;
  long I,J;
  DWord *Ptr = Img->Data;
  long Orb;

  L.Y = R2->Y;
  dX = (R2->X-R1->X)/(float)Img->X;
  dY = (R1->Y-R2->Y)/(float)Img->Y;
  J = Img->Y;
  while(J--)
  {
    I = Img->X;
    L.X = R1->X;
    while (I--)
    {
      Z.X = L.X; Z.Y = L.Y;
      Orb=32;
      Z2.X = Z.X*Z.X;
      Z2.Y = Z.Y*Z.Y;
      Y.X = 0; Y.Y = 0;
      do
      {
        X.X = Z2.X-Z2.Y+P1+P2*Y.X;
        X.Y = 2.0*Z.X*Z.Y+P2*Y.Y;
        Y.X = Z.X;
        Y.Y = Z.Y;
        Z.X = X.X;
        Z.Y = X.Y;
        Z2.X = Z.X*Z.X;
        Z2.Y = Z.Y*Z.Y;
      } while (--Orb&&Z2.X<=4.0&&Z2.Y<=4.0);
      *Ptr++ = Colors32[31-Orb];
      L.X += dX;
    }
    L.Y += dY;
  }
}

// Simple Wobbler-8x8 based effects

// Affine transformer -
//   Ang - Rotation angle
//   Zoom - duh
//   XO,YO - (X,Y) Offset from origin
void Affine_Wobbler(GridPoint *Target_GP,float Ang,float Zoom,float XO,float YO)
{
  float Mat[2][2]; // Rotation and scale matrix
  float *Ptr = (float *)Mat;
  float dX,dY,X,Y;
  long I,J;
  GridPoint *GP = Target_GP;
  Mat[1][1] = Mat[0][0] = cos(Ang)*Zoom;
  Mat[0][1] = sin(Ang)*Zoom;
  Mat[1][0] = -Mat[0][1];
  dX = 65536.0/Grid_X;
  dY = 65536.0/Grid_Y;
  XO *= 256;
  YO *= 256;
  Y = -32768;
  for(J=0;J<Grid_Y;J++)
  {
    X = -32768;
    for(I=0;I<Grid_X;I++)
    {
      GP->U = (*Ptr++)*X+(*Ptr++)*Y+XO;
      GP->V = (*Ptr++)*X+(*Ptr)*Y+YO;
      Ptr -= 3;
      X += dX;
      GP++;
    }
    Y += dY;
  }
}

// Raytracing/Wobbler-8x8 based effects

// Calculates all Ray directions at Pixel density of 1:8, assuming view is
// on the [xy] Plane. The FOV parameter is converted to a perspective
// constant, and is given in Degrees.
void Init_WRaytrace_Table(float FOV)
{
  long I,J;
  float X,Y;
  Vector *V = WPDT;

  if (!WPDT) return;

  WRFOV = FOV;
  float Persp = tan(FOV*(PI/360.0))/CntrX;
  Y = 0;
  for(J=0;J<Grid_Y;J++)
  {
    X = 0;
    for(I=0;I<Grid_X;I++)
    {
      V->X = (X-CntrX)*Persp;
      V->Y = (CntrY-Y)*Persp;
      V->Z = 1.0;
      Vector_Norm(V);
      V++;
      X+=Grid_Subsamp;
    }
    Y+=Grid_Subsamp;
  }
}

// Ray-traces an infinite Cylinder at 8x8 subsampling density.
// the Radius Parameter is given Squared.
void WRaytrace_Cylinder(GridPoint *Target_GP,Vector *Origin,Matrix Orient,float Radius)
{
  GridPoint *GP = Target_GP,*GPE = GP+Grid_XY;
  Vector *PDT = WPDT;
  Vector D;
  const float Z2U_Conv = 32.0;
  float a,b,c=Origin->X*Origin->X+Origin->Y*Origin->Y-Radius,d,t;
  const float V_Conv = 32768.0/PI;
  if (c<0) //camera is inside cylinder
  {
    for(;GP<GPE;GP++,PDT++)
    {
      MatrixXVector(Orient,PDT,&D); // Get Direction vector
      a = D.X*D.X+D.Y*D.Y;
      b = 2*(Origin->X*D.X+Origin->Y*D.Y);
      d = sqrt(b*b-4*a*c);
      t = (d-b) / (2*a+EPSILON);
      D.X *= t; D.Y *= t; D.Z *= t;
      D.X += Origin->X; D.Y += Origin->Y; D.Z += Origin->Z;
      GP->U = D.Z*Z2U_Conv;
      GP->V = fabs(atan2(D.Y,D.X))*V_Conv;
    }
  }
  else
  {
    for(;GP<GPE;GP++,PDT++)
    {
      MatrixXVector(Orient,PDT,&D); // Get Direction vector
      a = D.X*D.X+D.Y*D.Y;
      b = 2*(Origin->X*D.X+Origin->Y*D.Y);
      if ((d=b*b-4*a*c)<0) continue;
      t = (-sqrt(d)-b) / (2*a+EPSILON);
      D.X *= t; D.Y *= t; D.Z *= t;
      D.X += Origin->X; D.Y += Origin->Y; D.Z += Origin->Z;
      GP->U = D.Z*Z2U_Conv;
      GP->V = atan2(D.Y,D.X)*V_Conv;
    }
  }
}

// This is a Tweaked version of the Cylinder, and will only work from the
// inside of the tunnel. This doesn't work 100% properly when the camera is
// not positioned on the axis of the Cylinder.
// The radius table is at 1 degree accuracy.
void WRaytrace_CurvedCylinder(GridPoint *Target_GP,Vector *Origin,Matrix Orient,float *Radius)
{
  GridPoint *GP = Target_GP,*GPE = GP+Grid_XY;
  Vector *PDT = WPDT;
  Vector D;
  const float Z2U_Conv = 32.0;
  const float V_Conv = 32768.0/PI;
  const float Rad2Deg = 180/PI;
  float a,b,c,cc=Origin->X*Origin->X+Origin->Y*Origin->Y,d,t;
  Radius+=179;

  for(;GP<GPE;GP++,PDT++)
  {
    MatrixXVector(Orient,PDT,&D); // Get Direction vector
    a = D.X*D.X+D.Y*D.Y;
    b = 2*(Origin->X*D.X+Origin->Y*D.Y);
    c = cc-Radius[(int)(atan2(D.X,D.Y)*Rad2Deg)];
    d = sqrt(b*b-4*a*c);
    t = (d-b) / (2*a+EPSILON);
    D.X *= t; D.Y *= t; D.Z *= t;
    D.X += Origin->X; D.Y += Origin->Y; D.Z += Origin->Z;
    GP->U = D.Z*Z2U_Conv;
    GP->V = fabs(atan2(D.Y,D.X))*V_Conv;
  }
}


float Plane_Z2U_Conv = 8.0;
float Plane_X2V_Conv = 8.0;

// Raytraces a Plane, which is always aligned with the [xz] plane. The
// Height parameter measures it's distance
void WRaytrace_Plane(GridPoint *Target_GP,Vector *Origin,Matrix Orient,float Height)
{
  GridPoint *GP = Target_GP,*GPE = GP+Grid_XY;
  Vector *PDT = WPDT;
  Vector D;
  float c = Height - Origin->Y,t;
  if (c<0) //camera is on the upper side of the plane
  {
    for(;GP<GPE;GP++,PDT++)
    {
      MatrixXVector(Orient,PDT,&D); // Get Direction vector
      if (D.Y>0) continue; // Flys away
      t = c/D.Y;
      GP->U = (D.Z*t+Origin->Z)*Plane_Z2U_Conv;
      GP->V = (D.X*t+Origin->X)*Plane_X2V_Conv;
    }
  }
  else
  {
    for(;GP<GPE;GP++,PDT++)
    {
      MatrixXVector(Orient,PDT,&D); // Get Direction vector
      if (D.Y<0) continue;
      t = c/D.Y;
      GP->U = (D.Z*t+Origin->Z)*Plane_Z2U_Conv;
      GP->V = (D.X*t+Origin->X)*Plane_X2V_Conv;
    }
  }
}

// Ray-traces an infinite Cylinder at 8x8 subsampling density.
// the Radius is not squared.
// The pole vector of the Sphere is aligned with the Y axis.
void WRaytrace_Sphere(GridPoint *Target_GP,Vector *Origin,Matrix Orient,float Radius)
{
  GridPoint *GP = Target_GP,*GPE = GP+Grid_XY;
  Vector *PDT = WPDT;
  Vector D;
  const float U_Conv = 32768.0/PI;
  float b,c=Origin->X*Origin->X+Origin->Y*Origin->Y+Origin->Z*Origin->Z-Radius*Radius,d,t;
  const float V_Conv = 32768.0/PI;
  float rd = 1.0/Radius;
  if (c<0) //camera is inside the Sphere
  {
    for(;GP<GPE;GP++,PDT++)
    {
      MatrixXVector(Orient,PDT,&D); // Get Direction vector
      b = Origin->X*D.X+Origin->Y*D.Y+Origin->Z*D.Z;
      d = sqrt(b*b-c);
      t = d-b;
      D.X *= t; D.Y *= t; D.Z *= t;
      D.X += Origin->X; D.Y += Origin->Y; D.Z += Origin->Z;
      GP->U = fabs(asin(D.Y*rd))*U_Conv;
      GP->V = fabs(atan2(D.Z,D.X))*V_Conv;
    }
  }
  else
  {
    for(;GP<GPE;GP++,PDT++)
    {
      MatrixXVector(Orient,PDT,&D); // Get Direction vector
      b = Origin->X*D.X+Origin->Y*D.Y+Origin->Z*D.Z;
      if ((d=b*b-c)<0) continue;
      d = -sqrt(d);
      t = d-b;
      D.X *= t; D.Y *= t; D.Z *= t;
      D.X += Origin->X; D.Y += Origin->Y; D.Z += Origin->Z;
      GP->U = fabs(asin(D.Y*rd))*U_Conv;
      GP->V = fabs(atan2(D.Z,D.X))*V_Conv;
    }
  }
}

// Calculates a Phong/Specular Illumination map, used with
// an Environment mapper to simulate true Phong shading.
// Power should be 0.5 for normal distribution and higher
// for sharper highlights.
void Phong_Map(Image *Img,float R,float G,float B,float Power)
{
  long I,J;
  float X,Y,Y2,Z,dX,dY;
  DWord *Ptr = Img->Data;
  Y = -1.0;
  dX = 2.0/(float)Img->X;
  dY = 2.0/(float)Img->Y;
  for(J=0;J<Img->Y;J++)
  {
    Y2 = 1.0-Y*Y;
    X = -1.0;
    for(I=0;I<Img->X;I++)
    {
      Z = Y2-X*X; //so the Magnitude of (X,Y,Z) will be 1
      if (Z<0.0) {*Ptr++ = 0; X += dX; continue;}
      Z = 255.0*pow(Z,Power);
      *Ptr++ = ((int)(R*Z)<<16)+((int)(G*Z)<<8)+(int)(B*Z);
      X += dX;
    }
    Y += dY;
  }
}

// Hidden Flare effect: Generate a Flare composed of Light rays that are
// blocked by a given Image (the parameter) as if the Lightsource is behind
// it, and hence, only the rays that pass the image are visible. color 0
// is considered a "Block".
// Parameters: Src - the source Image,
// L - Position of the Lightsource, Z value is considered as the depth of the rays.
// (L.Z actually does not matter, because it's only an approximation)
void Hidden_Flare(Image *Src,Vector L,Image *Trg)
{

}



