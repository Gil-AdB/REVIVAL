/*

  FDS / Vertigo GL / Pylon engine
  *******************************

  Coded by  Yoni Lavi AKA Silvatar/Flood
            Gil-Ad Ben-Or AKA Diffuse/Flood
            Ofir Ehrlich AKA Griffin_Avenger/Flood
        DFS - Demo Flood System

	LOGO

    FDS - The beurocratic engine!!
*/

#include <StdIO.H>
#include <StdLib.H>
#include <String.H>
#include <Math.H>

#include "Source\FDS_Defs.H"     // Definitions
#include "Source\FDS_Vars.H"     // Variables and structures
#include "Source\FDS_Decs.H"     // Declarations

char *ModuleBuf;


void Perform_UnitTests()
{
	Image Img,Img2,Phong;
	long I,J,K,Xl,Yl;
	DWord *VP = (DWord *)VPage;

	memset(VPage,0,PageSize);
	// now I'll try loading a Bitmap and displaying it into screen
	Load_Image_JPEG(&Img,"Textures\\LZERATUL.JPG");
	Scale_Image(&Img,XRes,YRes);
//  New_Image(&Img,256,256);
//  for(I=0;I<65536;I++)
//    Img.Data[I] = RGB32(63,63,127);
	New_Image(&Phong,XRes,YRes);
	Phong_Map(&Phong,1.0,1.0,1.0,2.0);
	Gamma_Correction(&Phong,1.0);

	Intensity_Alpha(&Img);
//  Image_Ripple(&Img,0.05,0.2,0.00);

	Img2.X = Img.X;
	Img2.Y = Img.Y;
	Img2.Data = new DWord[Img.X*Img.Y];

	K = 0;
	while (!Keypressed())
	{
		memset(VPage,0,PageSize);
		memcpy(Img2.Data,Img.Data,Img.X*Img.Y<<2);
		//Bump_Image_2D(&Img2,NULL,&Phong,160+100*cos(K*0.05),100+100*sin(K*0.05));
		Image_Ripple(&Img2,0.1f,0.005f,K*4.0f);
	//  Scale_Image(&Img,128,128);
	//  New_Image(&Img,256,256);
	//  Generate_Flare_Image(&Img,1.0,0.2,0.2);
		// ok, now put-bitmap
		Xl = Img.X; if (XRes<Xl) Xl = XRes;
		Yl = Img.Y; if (YRes<Yl) Yl = YRes;
		for(J=0;J<Yl;J++)
			memcpy(VP+XRes*J,Img2.Data+Img.X*J,Xl<<2);

		Flip(Screen);
		K++;
	}
	while (!Keypressed());

	// Test the Frustration
	/*Image Img;
	Texture T;
	Material M;
	Vertex A,B,C;
	Face F;

  M.Txtr = &T;

  Load_Image_PCX(&Img,"Textures\\Wals.PCX");
  T.BPP = 32;
  Convert_Image2Texture(&Img,&T);

	FOVX = 2.0;
  FOVY = 2.0;
  DoFace = &F;

	F.A = &A;
  F.B = &B;
  F.C = &C;
  F.Txtr = &M;
	F.Filler = &The_BiTrue;
  F.Clipper = &Frust_Set_UV;

  memset(&A,0,sizeof(Vertex));
  memset(&B,0,sizeof(Vertex));
	memset(&C,0,sizeof(Vertex));
  // Positions.
	Vector_Form(&A.TPos,550,-2.0,2.0);
  Vector_Form(&B.TPos,100,240,2.0);
  Vector_Form(&C.TPos,500,490.2,2.0);
  A.RZ = 1.0/A.TPos.Z;
  B.RZ = 1.0/B.TPos.Z;
	C.RZ = 1.0/C.TPos.Z;

  A.PX = A.TPos.X*A.RZ;
  A.PY = A.TPos.Y*A.RZ;
	B.PX = B.TPos.X*B.RZ;
  B.PY = B.TPos.Y*B.RZ;
  C.PX = C.TPos.X*C.RZ;
  C.PY = C.TPos.Y*C.RZ;
	A.U = 20; A.RU = A.U*A.RZ;
  A.V = 20; A.RV = A.V*A.RZ;
  B.U = 160; B.RU = B.U*B.RZ;
  B.V = 100; B.RV = B.V*B.RZ;
  C.U = 80; C.RU = C.U*C.RZ;
  C.V = 220; C.RV = C.V*C.RZ;

	A.LR = 254; B.LR = 2;   C.LR = 254;
  A.LG = 254; B.LG = 127; C.LG = 2;
  A.LB = 254; B.LB = 254; C.LB = 2;

  A.Flags = Vtx_VisNear;

  memset(VPage,0,PageSize);

  Universal_Frustrum_Elite();
	Flip(Screen);
	while(!Keypressed());*/
}

void Pixel(long X,long Y)
{
  DWord *VP = (DWord *)VGAPtr;
  if (X<0||X>319||Y<0||Y>199) return;
    VP[X+XRes*Y] = 0xFFFFFFFF;
}


void Spline_Test()
{

  Spline S;
//  Spline_Create(&S,5);
  S.NumKeys = 5;
  S.Keys = new SplineKey[5];
  S.CurKey = 0;
  S.Flags = 0;
  Vector V;

  float SPF[5] = {0.0f,0.0f,0.0f,0.0f,0.0f};
  Vector_Form(&V,30.0f,30.0f,0.0f);
  Spline_SetKey_3D(&S,&V,0.0f,SPF);
  Vector_Form(&V,150.0f,30.0f,0.0f);
  Spline_SetKey_3D(&S,&V,4.0f,SPF);
  Vector_Form(&V,200.0f,70.0f,0.0f);
  Spline_SetKey_3D(&S,&V,8.0f,SPF);
  Vector_Form(&V,90.0f,130.0f,0.0f);
	Spline_SetKey_3D(&S,&V,12.0f,SPF);
  Vector_Form(&V,100.0f,70.0f,0.0f);
  Spline_SetKey_3D(&S,&V,16.0f,SPF);
  Spline_Init_3D(&S);

	float t;
  for(t=0.0f;t<16.0f;t+=0.2f)
  {
    Spline_Calc_3D(&S,t,&V);
    Pixel((long)V.X,(long)V.Y);
  }
  while(!Keypressed());
}

void Install_Phong()
{
  // ZE KOL KACH LE'AAAAAAAAAAAAAT, VE KAZE ZOL!!!!
  Image Img;
  Img.X = 256;
  Img.Y = 256;
	Img.Data = new DWord[256*256];
	Phong_Map(&Img,1.0,1.0,1.0,4.0);
  Gamma_Correction(&Img,1.2);
//  Scale_Image(&Img);
  Convert_Image2Texture(&Img,&Phong_Mapping);
//  Phong_Mapping.FileName = strdup("Textures//3.TGA");
//  Load_Texture(&Phong_Mapping);
//  BPPConvert_Texture(&Phong_Mapping,BPP);
  FilterLSB_Texture(&Phong_Mapping);

  for(TriMesh *T = CurScene->TriMeshHead; T; T=T->Next)
    T->Flags|=Tri_Phong;
}

void Initialize_System()
{
  LogFile = fopen("Runtime.LOG","wt");

	if (!FDS_Init(320,240,32))    // Initialize FDS
  {
		printf("Initializing failed! Crashing to OS\n");
    exit(1);
  }

#ifdef Play_Music
	ModuleBuf = LoadARJ("Test.ARJ","Menu.XM");
  Initialize_Module(ModuleBuf);
#endif
	Generate_RGBFlares();
  if (BPP==8) Materials_Remap_Aligned();
//  InitMouse();
  // all Hardwired Initializations complete
}

void Checkout_Isosurface()
{
  Load_V3D(CurScene,"V3D//Metaball.V3D");
  Omni *O;
  O = new Omni;
  memset(O,0,sizeof(Omni));
	O->L.R = 35000.0;
  O->L.G = 0.0;
  O->L.B = 0.0;
  O->FallOff = 1.0;
  O->HotSpot = 1.0;
	Insert_Omni2Scene(CurScene,O);
  Quaternion_Form(&O->Pos.Keys->Pos,15,20,0,0);
  O->Flags = Flare_LightSource;

  O = new Omni;
  memset(O,0,sizeof(Omni));
  O->L.R = 0.0;
  O->L.G = 35000.0;
  O->L.B = 0.0;
  O->FallOff = 1.0;
  O->HotSpot = 1.0;
  Insert_Omni2Scene(CurScene,O);
  Quaternion_Form(&O->Pos.Keys->Pos,-4,15,6,0);
  O->Flags = Flare_LightSource;

	O = new Omni;
  memset(O,0,sizeof(Omni));
  O->L.R = 0.0;
  O->L.G = 0.0;
  O->L.B = 35000.0;
	O->FallOff = 1.0;
  O->HotSpot = 1.0;
  Insert_Omni2Scene(CurScene,O);
  Quaternion_Form(&O->Pos.Keys->Pos,32,30,32,0);
  O->Flags = Flare_LightSource;
}

void Display_Material_Library()
{
  Material *M;
  for(M=MatLib;M;M=M->Next)
  {
    //sheat
    printf("Material Entry: %s\n - Texture ",M->Name);
    if (M->Txtr)
		{
      printf("%s with %d BPP\n",M->Txtr->FileName,M->Txtr->BPP);
    } else printf("-NULL-\n");
  }
}

#define EP_Layers 2

void Fade_In(DWord *Target,DWord *Source,char Perc)
{
  DWord *Tg = Target;
  char *Src = (char *)Source,*SE = Src + XRes*YRes*4;
  long r,g,b;
  while (Src<SE)
  {
    b = (((*Src++)*Perc)&0xFF00)>>8;
    g = (((*Src++)*Perc)&0xFF00);
    r = (((*Src++)*Perc)&0xFF00)<<8;
    Src++;
    *Tg++ = r+g+b;
	}
}

void IsoCline_Mark(DWord *Page,DWord *TImg,DWord *Plasma,float Iso)
{
	DWord *PE = Page + XRes*YRes;
//	float illum;
	long qIl,qIso = Iso;
	DWord Il;
	for(;Page<PE;Page++,Plasma++)
	{
		if (!(*TImg++)) continue;
		//calc slow stuff
//    illum = 255.0f*exp(-0.01*fabs(Iso-(float)((*Plasma++))));
//    Il = illum;
//		illum = fabs(Iso-(float)(*Plasma));
		qIl = abs(qIso-(*Plasma));

/////		if (illum>8.0f) continue;
		if (qIl>8) continue;

		//Il = illum;
		//Il = 255-(Il*Il<<1);
		//Il = 255;

//		*Page = (Il<<16)+(Il<<8)+Il;
		*Page = 0xFFFFFF;
	}
}

// the glorious effect, for further details, check out Starcraft by Blizzard
void Electricity_Plasma()
{
	Texture Tx;
	Image Target;
	Tx.FileName = strdup("Textures\\Test.GIF");
	Tx.BPP = 8;
	Load_Texture(&Tx);
	Convert_Texture2Image(&Tx,&Target);
	Scale_Image(&Target,XRes,YRes);

	Image EPI[EP_Layers];
	long I,J;
	for(I=0;I<EP_Layers;I++)
	{
		EPI[I].X = XRes;
		EPI[I].Y = YRes;
		EPI[I].Data = new DWord[XRes*YRes];
		memset(EPI[I].Data,0,XRes*YRes<<2);
		Generate_Fractal_Noise(EPI+I,1.0f);
		Image_LPF(EPI+I);
		Image_LPF(EPI+I);
		for(J=0;J<XRes*YRes;J++) {EPI[I].Data[J] &= 0xFF;}// EPI[I].Data[J] += I<<6;}
	}

	// now start the effect...
	// Fade the Target image to screen

	//500frames
	float t = 0.0f;
	FILE *F;
	Image VideoImage;
	VideoImage.X = XRes; VideoImage.Y = YRes;
	VideoImage.Data = (DWord *)VPage;
	while(!Keypressed())
	{
		Fade_In((DWord *)VPage,Target.Data,t*255.0f);
		for(I=0;I<EP_Layers;I++)
			IsoCline_Mark((DWord *)VPage,Target.Data,EPI[I].Data,t*255.0f);

		Image_LPF(&VideoImage);
		Flip(Screen);
		if (Keyboard[ScX])
		{

			F = fopen("scrshot.raw","wb");
			fwrite(VPage,1,PageSize,F);
			fclose(F);
		}
		t+=0.01;
	}

	// not much of an effect
}

#define EPAmount 10

Vector *EP;

/*void EvenPotential(DWord *Page,DWord *Img,float Voltage)
{
  DWord *PE = Page + XRes*YRes;
  float illum;
  DWord Il;
  long X,Y;
  for(;Page<PE;Page++,Plasma++)
  {
    if (!(*TImg++)) continue;
    illum = CalcPotential(
		if (illum>10.0f) continue;

		Il = illum;
		Il = 255-(Il*Il<<1);

		*Page = (Il<<16)+(Il<<8)+Il;
	}

}*/

void Electricity_Voltage()
{
//  EP = new Vector[EPAmount];
/// long I;
//  for(I=0;I<EPAmount;I++)
//    Vector_Make(EP+I,(rand()*XRes)>>15,(rand()*YRes)>>15,0);

//  EvenPotential((DWord *)VPage,Target.Data,0.0f);
}

signed char *STbl,*SOTbl,*CTbl,*COTbl;

void Calc_STable()
{
	STbl = new signed char [15*XRes];
	SOTbl = STbl + 5*XRes;
	long I;
	for(I=-5*XRes;I<10*XRes;I++)
		SOTbl[I] = (long) (4.0f * sin (I * 0.01f));
}

void Calc_CTable()
{
	CTbl = new signed char [13*XRes];
	COTbl = CTbl + 3*XRes;
	long I;
	for(I=-3*XRes;I<10*XRes;I++)
		COTbl[I] = (long) (5.0f * cos (I * 0.01f));
//	COTbl = SOTbl + 157;
}

DWord *DistBuf;

void Init_Distort(void)
{
	Calc_STable();
	Calc_CTable();
	DistBuf = new DWord[XRes*YRes];
}

void Run_Distort(void)
{
//	Image Img;
//	Load_Image_JPEG(&Img,"Textures\\Distort.JPG");

//	Calc_STable();
//	Calc_CTable();
//	Scale_Image(&Img,XRes,YRes);
//	DWord *Ptr;
//	DWord *DPtr = Img.Data;
	long I,T;

	long X,Y,hX = XRes>>1,hY = YRes>>1;
//	long Frames = 0;
//	Timer = 0;
//	while(!Keyboard[ScESC])
//	{
		memcpy(DistBuf,VPage,PageSize);

		DWord *DPtr = DistBuf;
		DWord *Ptr = (DWord *)VPage;

		I = 0;
		T = Timer%628;
		for(Y=-hY;Y<hY;Y++)
			for(X=-hX;X<hX;X++)
				*Ptr++ = DPtr[(I++) + COTbl[T+(X<<1)-Y<<1]-(X>>3)+
					XRes*(SOTbl[(T<<1)+3*X+5*Y] - (Y>>3))];

				//(long)(cos(T*0.02+X*0.04-Y*0.02) * 5.0f)
				//(long)(sin(T*0.02+X*0.03+Y*0.05) * 5.0f) - (long)(Y * 0.1)
//		Frames++;
//	}
//	printf("FPS = %1.3f\n",100.0f*(float)Frames/(float)Timer);
}

void Run_Vortex(void)
{
	Vector CameraPos = {0,0,0};
	Matrix CamMat;
	float Radius;
	static int x,y,i,j;
	static float a,bb,c,d,Delta,X1,X2,X3,z,Rx,Ry,Rz;
	static float u,v,r,g,b;
	static float u1,v1,u2,v2,u3,v3,u4,v4,u6,v6,cosr1,cosr3,sinr1,sinr3,cosr4,sinr4,cosr6,sinr6,R5,R6;
	static Vector Intersection1,Origin1,Direction1,U,Origin2,Direction2,Intersection2;
	static Vector Intersection3,Origin3,Direction3;
	GridPoint *Plane_GP;
	Texture *PlaneTexture = new Texture;
	Image *PlaneImage = new Image;
	int X,Y;
	float R1,R3,R4;

	Matrix_Identity(CamMat);
	Radius = 1;

	PlaneTexture->FileName = strdup("Textures//whirl.JPG");
	Identify_Texture(PlaneTexture);
	if (!PlaneTexture->BPP)
	{
		printf("Error Loading texture!\n");
		exit(1);
	}
	Load_Texture(PlaneTexture);
	Convert_Texture2Image(PlaneTexture,PlaneImage);
//	WOBPOINTSHEIGHT = 30;

	Plane_GP = new GridPoint[XRes*YRes/64];
	// Run wobbler
	while (!Keyboard[ScESC])
	{
		R1 = Timer * 0.01;
		Euler_Angles(CamMat,Rx,Ry,Rz);
		i=0;
		j=0;
		sinr1 = sin(R1);
		cosr1 = cos(R1);
		for (y=0;y<=240;y+=8)
			for (x=0;x<=320;x+=8)
			{

				X = x - 160;
				Y = y - 120;

				u = x * 204.8;
				v = y * 327.68;

				u1 = (u-32768) * sinr1  + (v-32768) * cosr1;
				v1 = (u-32768) * cosr1  - (v-32768) * sinr1;


				r = g = b = 127.0;

				r*=254.0;
				g*=254.0;
				b*=254.0;

				Plane_GP[j].U=u1;
				Plane_GP[j].V=v1;
				Plane_GP[j].R=r;
				Plane_GP[j].G=g;
				Plane_GP[j].B=b;
				j++;
			}
		Grid_Texture_Mapper_MMX(Plane_GP,PlaneImage,(DWord *)VPage);
		Rx += 0.01;
		Ry += 0.01;
		CameraPos.Z += 0.01;
		Frames++;

		Flip(Screen);
	}
}
//	delete VorTexture;
//	delete VorImage;
//}



void Run_Vortex1(void)
{
	Vector CameraPos = {0,0,0};
	Matrix CamMat;
	float Radius;
	static int x,y,i,j;
	static float a,bb,c,d,Delta,X1,X2,X3,z,Rx,Ry,Rz;
	static float u,v,u1,v1,u2,v2,r,g,b;
	static float Code_R1,Code_RS,Code_R2,CCosR1,CSinR1,CCosR2,CSinR2;
	static float Gfx_R1,Gfx_R2,GCosR1,GSinR1,GCosR2,GSinR2,Gfx_RS;
	static Vector Intersection1,Origin1,Direction1,U,Origin2,Direction2,Intersection2;
	static Vector Intersection3,Origin3,Direction3;
	int Gfx = 0,Sfx = 0, Code = 1;
	GridPoint *Plane_GP;
	GridPoint *Code_GP;
	GridPoint *Gfx_GP;
	GridPoint *Sfx_GP;
	Texture *PlaneTexture = new Texture;
	Image *PlaneImage = new Image;
	Texture *CodeTexture = new Texture;
	Image *CodeImage = new Image;
	Texture *GfxTexture = new Texture;
	Image *GfxImage = new Image;
	Texture *SfxTexture = new Texture;
	Image *SfxImage = new Image;
	int X,Y;
	float R1,R3,R4;

	Matrix_Identity(CamMat);
	Radius = 1;

	PlaneTexture->FileName = strdup("Textures//Plane.JPG");
	Identify_Texture(PlaneTexture);
	if (!PlaneTexture->BPP)
	{
		printf("Error Loading texture!\n");
		exit(1);
	}
	Load_Texture(PlaneTexture);
	Convert_Texture2Image(PlaneTexture,PlaneImage);
//	WOBPOINTSHEIGHT = 30;

	CodeTexture->FileName = strdup("Textures//Code.JPG");
	Identify_Texture(CodeTexture);
	if (!CodeTexture->BPP)
	{
		printf("Error Loading texture!\n");
		exit(1);
	}
	Load_Texture(CodeTexture);
	Convert_Texture2Image(CodeTexture,CodeImage);


	GfxTexture->FileName = strdup("Textures//Gfx.JPG");
	Identify_Texture(GfxTexture);
	if (!GfxTexture->BPP)
	{
		printf("Error Loading texture!\n");
		exit(1);
	}
	Load_Texture(GfxTexture);
	Convert_Texture2Image(GfxTexture,GfxImage);

	SfxTexture->FileName = strdup("Textures//Sfx.JPG");
	Identify_Texture(SfxTexture);
	if (!SfxTexture->BPP)
	{
		printf("Error Loading texture!\n");
		exit(1);
	}
	Load_Texture(SfxTexture);
	Convert_Texture2Image(SfxTexture,SfxImage);


	char *Page1 = new char[PageSize];
	char *Page2 = new char[PageSize];
	char *Page3 = new char[PageSize];
	char *Page4 = new char[PageSize];

	VESA_Surface Surf1;
	VESA_Surface Surf2;
	VESA_Surface Surf3;
	VESA_Surface Surf4;
	memcpy(&Surf1,Screen,sizeof(VESA_Surface));
	memcpy(&Surf2,Screen,sizeof(VESA_Surface));
	memcpy(&Surf3,Screen,sizeof(VESA_Surface));
	memcpy(&Surf4,Screen,sizeof(VESA_Surface));
	Surf1.Data = Page1;
	Surf2.Data = Page2;
	Surf3.Data = Page3;
	Surf4.Data = Page4;
	Surf1.Flags = VSurf_Noalloc;
	Surf1.Targ = VGAPtr;
	Surf2.Flags = VSurf_Noalloc;
	Surf2.Targ = VGAPtr;
	Surf3.Flags = VSurf_Noalloc;
	Surf3.Targ = VGAPtr;
	Surf4.Flags = VSurf_Noalloc;
	Surf4.Targ = VGAPtr;

	Plane_GP = new GridPoint[XRes*YRes/60];
	Code_GP = new GridPoint[XRes*YRes/60];
	Gfx_GP = new GridPoint[XRes*YRes/60];
	Sfx_GP = new GridPoint[XRes*YRes/60];
	// Run wobbler
	Timer = 0;
	float ST;
	while (!Keyboard[ScESC])
	{

		if (Timer <= 100 * 13)
			ST = (Timer*2000)/(1000+Timer);//  sqrt(Timer*1600);
		if (Timer > 100 * 13){ Gfx = 1;Code = 0;
			ST = ((Timer-1300)*2000)/(1000+(Timer-1300));//  sqrt(Timer*1600);
		}
		if (Timer > 100 * 27){ Gfx = 0;Sfx = 1;
			ST = ((Timer-2700)*2000)/(1000+(Timer-2700));//  sqrt(Timer*1600);
		}
//		ST = (Timer*2000)/(1000+Timer);//  sqrt(Timer*1600);
		Euler_Angles(CamMat,Rx,Ry,Rz);
		i=0;
		j=0;
		//code
		Code_R1 = ST * 0.0005;
		// back
		CSinR1 = sin(Code_R1*0.1);
		CCosR1 = cos(Code_R1*0.1);
	if (ST < 700)
		Code_RS = Code_R1 * 0.5;

		Gfx_R1 = - (ST) * 0.000001;
		GSinR1 = sin(Gfx_R1*0.05);
		GCosR1 = cos(Gfx_R1*0.05);
		if (ST < 400)
			Gfx_RS = (ST) * 0.00001;



		Origin1.X=CameraPos.X;
		Origin1.Y=CameraPos.Y;
		Origin1.Z=CameraPos.Z;
		for (y=0;y<=240;y+=8)
			for (x=0;x<=320;x+=8)
			{
				Direction1.X=x-160.0;
				Direction1.Y=y-100.0;
				Direction1.Z=256.0;
				MatrixXVector(CamMat,&Direction1,&U);
				Direction1=U;
				Vector_Norm(&Direction1);
				Direction2=Direction1;
				Origin2=Origin1;
				Direction3=Direction1;
				Origin3=Origin1;
				a=Radius-Origin1.Y;
				bb=Direction1.Y;

				c=-Radius-Origin1.Y;
				d=Direction1.Y;
				if (bb==0) X1=0;
				else
				{
					X1=a/bb;
					X2=c/d;
				}
				Vector_SelfScale(&Direction1,X1);
				Vector_Add(&Origin1,&Direction1,&Intersection1);
				Vector_SelfScale(&Direction2,X2);
				Vector_Add(&Origin2,&Direction2,&Intersection2);
				if (X2<X1)
				{
					u=(Intersection2.X+cos(Intersection2.Z+(float)(ST*0.1)/28.65)*0.5)*0.5;
					v=(Intersection2.Z+sin(Intersection2.X+(float)(ST*0.1)/28.65)*0.5)*0.5;
					Intersection2.X-=Origin2.X;
					Intersection2.Y-=Origin2.Y;
					Intersection2.Z-=Origin2.Z;
					r=(sqrt(Intersection2.X*Intersection2.X+Intersection2.Y*Intersection2.Y+Intersection2.Z*Intersection2.Z)*32);
					if (r>255.0) r=255.0;
					r=255-r;
					if (r<1.0) r=1;
					b=r * 0.7;
					g= r*0.8;
//					r*= 0.5;
					g-=Frames /10;
					b-=Frames /20;

					if (g>255.0) g=255.0;
					if (g<1.0) g=1;
					if (b>255.0) b=255.0;
					if (b<1.0) b=1;

					u*=65536.0;
					v*=65536.0;
				}
				else
				{
					u=(Intersection1.X+cos(Intersection1.Z+(float)(ST*0.1)/28.65)*0.5)*0.5;
					v=(Intersection1.Z+sin(Intersection1.X+(float)(ST*0.1)/28.65)*0.5)*0.5;
					Intersection1.X-=Origin1.X;
					Intersection1.Y-=Origin1.Y;
					Intersection1.Z-=Origin1.Z;

					u*=65536.0;
					v*=65536.0;

					r=(sqrt(Intersection1.X*Intersection1.X+Intersection1.Y*Intersection1.Y+Intersection1.Z*Intersection1.Z)*32);
					if (r>255.0) r=255.0;
					r=255-r;
					if (r<1.0) r=1;
					b=r * 0.7;
					g= r*0.8;
					g-=Frames /10;
					b-=Frames /20;

					if (g>255.0) g=255.0;
					if (g<1.0) g=1;
					if (b>255.0) b=255.0;
					if (b<1.0) b=1;
				}
				r*=254.0;
				g*=254.0;
				b*=254.0;

				Plane_GP[j].U=u;
				Plane_GP[j].V=v;
				Plane_GP[j].R=r;
				Plane_GP[j].G=g;
				Plane_GP[j].B=b;


				X = x - 160;
				Y = y - 120;

				Code_R2 = sqrt(X*X + Y*Y) / (200.0)+ST /100.0;
				CCosR2 = cos (Code_R2);
				CSinR2 = sin (Code_R2);

				u = X * 204.8 * -(Code_RS * 5);
				v = Y * 327.68 * -(Code_RS * 5);

				u1 = (u) * CSinR1  + (v) * CCosR1;
				v1 = (u) * CCosR1  - (v) * CSinR1;

				u2 = (u1) * CSinR2  + (v1) * CCosR2;
				v2 = (u1) * CCosR2  - (v1) * CSinR2;

				r = g = b = 127.0;

				r*=254.0;
				g*=254.0;
				b*=254.0;

				Code_GP[j].U=u2+32767;
				Code_GP[j].V=v2+32767;
				if (Code_GP[j].U > 65535) Code_GP[j].U = 65535;
				if (Code_GP[j].V > 65535) Code_GP[j].V = 65535;
				if (Code_GP[j].U < 0) Code_GP[j].U = 0;
				if (Code_GP[j].V < 0) Code_GP[j].V = 0;
//				Code_GP[j].R=r;
//				Code_GP[j].G=g;
//				Code_GP[j].B=b;




				Gfx_R2 = sqrt(X*X + Y*Y) / (120.0) - (ST) /(100.0);
				GCosR2 = cos (Gfx_R2);
				GSinR2 = sin (Gfx_R2);

				u = X * 204.8 * (Gfx_RS*20.0);
				v = Y * 327.68 * (Gfx_RS*20.0);

				u1 = (u) * GSinR1  + (v) * GCosR1;
				v1 = (u) * GCosR1  - (v) * GSinR1;

				u1*=10.0;	v1*=10.0;

				u2 = (u1) * GSinR2  + (v1) * GCosR2;
				v2 = (u1) * GCosR2  - (v1) * GSinR2;

				r = g = b = 127.0;

				r*=254.0;
				g*=254.0;
				b*=254.0;

				Gfx_GP[j].U=u2+32767;
				Gfx_GP[j].V=v2+32767;
				if (Gfx_GP[j].U > 65535) Gfx_GP[j].U = 65535;
				if (Gfx_GP[j].V > 65535) Gfx_GP[j].V = 65535;
				if (Gfx_GP[j].U < 0) Gfx_GP[j].U = 0;
				if (Gfx_GP[j].V < 0) Gfx_GP[j].V = 0;
//				Gfx_GP[j].R=r;
//				Gfx_GP[j].G=g;
//				Gfx_GP[j].B=b;


				Code_R2 = sqrt(X*X + Y*Y) / (200.0)+ST /100.0;
				CCosR2 = cos (-Code_R2);
				CSinR2 = sin (-Code_R2);

				u = X * 204.8 * -(Code_RS * 5);
				v = Y * 327.68 * -(Code_RS * 5);

				u1 = (u) * CSinR1  + (v) * CCosR1;
				v1 = (u) * CCosR1  - (v) * CSinR1;

				u2 = (u1) * CSinR2  + (v1) * CCosR2;
				v2 = (u1) * CCosR2  - (v1) * CSinR2;

				r = g = b = 127.0;

				r*=254.0;
				g*=254.0;
				b*=254.0;

				Sfx_GP[j].U=u2+32767;
				Sfx_GP[j].V=v2+32767;
				if (Sfx_GP[j].U > 65535) Sfx_GP[j].U = 65535;
				if (Sfx_GP[j].V > 65535) Sfx_GP[j].V = 65535;
				if (Sfx_GP[j].U < 0) Sfx_GP[j].U = 0;
				if (Sfx_GP[j].V < 0) Sfx_GP[j].V = 0;





				j++;
			}

		Grid_Texture_Mapper_MMX(Plane_GP,PlaneImage,(DWord *)Page1);
		Grid_Texture_Mapper(Code_GP,CodeImage,(DWord *)Page2);
		Grid_Texture_Mapper(Gfx_GP,GfxImage,(DWord *)Page3);
		Grid_Texture_Mapper(Sfx_GP,SfxImage,(DWord *)Page4);

		if (Code)
		{
			Modulate(&Surf1,&Surf2,0xa0a0a0,0xa0a0a0);
			Modulate(&Surf2,Screen,0xa0a0a0,0xa0a0a0);
		}
		if (Gfx)
		{
			Modulate(&Surf1,&Surf3,0xa0a0a0,0xd0d0d0);
			Modulate(&Surf3,Screen,0xa0a0a0,0xa0a0a0);
 //			Modulate(&Surf2,&Surf3,0xa0a0a0,0xa0a0a0);
		}
		if (Sfx)
		{
			Modulate(&Surf1,&Surf4,0xa0a0a0,0xa0a0a0);
			Modulate(&Surf4,Screen,0xa0a0a0,0xa0a0a0);
		}
		Flip(Screen);


		Rx += 0.01;
		Ry += 0.01;
		CameraPos.Z += 0.01;
		Frames++;

//      r1,r2
// bg  code      gfx sfx

//		Flip(Screen);
	}
}
//	delete VorTexture;
//	delete VorImage;
//}

void main(int argc,char *argv[])
 {
	Scene *Sc;
	long I;

	Initialize_System();


//	Sc = new Scene; memset(Sc,0,sizeof(Scene));
//	printf("Memory before: %i\n",DPMI_Free_Memory());
//	LoadFLD(Sc,"Test.FLD");
//	printf("Memory after(1): %i\n",DPMI_Free_Memory());

//	Ambient_Factor = 150.0;
//	Preprocess_Scene(Sc);
//	Sc->FZP = 250000.0f;
//	Sc->Flags |= Scn_Fogged;

//	CurScene = Sc;
//	Diffusive_Factor = 2000.0;

//	printf("Memory after(2): %i\n",DPMI_Free_Memory());
//  Display_Material_Library();

	SetGraphicsMode();

// Start music
#ifdef Play_Music
	Play_Module();
#endif


	Run_Vortex1();
//	Init_Distort();

	long J;

 /*	while(!Keypressed())
	{
		for(J=0;J<YRes;J++)
			for(I=0;I<XRes;I++)
			((DWord *)VPage)[J*XRes+I] = I+(J<<8);

		Run_Distort();
		Flip(Screen);
	} */

//	RunScene(CurScene,(CurScene->EndFrame-CurScene->StartFrame)/45.0);/**/
#ifdef Play_Music
	ShutDown();
#endif
	FDS_End();
}
