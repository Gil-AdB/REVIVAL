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
/*  Image Img,Img2,Phong;
  long I,J,K,Xl,Yl;
  DWord *VP = (DWord *)VPage;

  memset(VPage,0,PageSize);
  // now I'll try loading a Bitmap and displaying it into screen
  Load_Image_PCX(&Img,"Textures\\F.PCX");
  Scale_Image(&Img,XRes,YRes);
//  New_Image(&Img,256,256);
//  for(I=0;I<65536;I++)
//    Img.Data[I] = RGB32(63,63,127);
  New_Image(&Phong,XRes,YRes);
  Phong_Map(&Phong,1.0,1.0,1.0,4.0);
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
    Bump_Image_2D(&Img2,NULL,&Phong,128+50*cos(K*0.05),128+50*sin(K*0.05));
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
  while (!Keypressed());*/

  // Test the Frustration
  Image Img;
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
  while(!Keypressed());
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


void main(int argc,char *argv[])
 {
  Scene *Sc;
  long I;

	Initialize_System();

	// Run Kernel
//  Initialize_Kernel(320,240,BPP,100,Kernel_Interactive);
//  Kernel(); //go

	// Or: do some other stuff (demo, games, or anything else)

	// I wish
//  Level_Editor(); //yeah

	//das scene reading from disk
//	LoadScenes(argc,argv);
	Sc = new Scene; memset(Sc,0,sizeof(Scene));
	printf("Memory before: %i\n",DPMI_Free_Memory());
	LoadFLD(Sc,"Test.FLD");
	printf("Memory after(1): %i\n",DPMI_Free_Memory());

//  Checkout_Isosurface();

	//das standard 3d scene playing sequence
	// Forces Gouraud on flat or T-G on Texture mapped polygons
/*(	for(Material *M = MatLib; M; M=M->Next)
		M->Flags|=Mat_RGBInterp;*/
//    M->Flags = Mat_Nonconv | Mat_RGBInterp | Mat_TwoSided; //forces gouraud

//  Install_Phong(); // Forces Textured Faked Phong. hmmmmmm ..... yammmmy! // stop drooling buffer
//	for(I=0;I<argc;I++)
//		Preprocess_Scene(CurScene+I);
	Preprocess_Scene(Sc);
	CurScene = Sc;

	printf("Memory after(2): %i\n",DPMI_Free_Memory());
//  Display_Material_Library();

	SetGraphicsMode();

// Start music
#ifdef Play_Music
	Play_Module();
#endif

//	Electricity_Plasma();
//	Electricity_Voltage();

/*	for(TriMesh *T = CurScene->TriMeshHead;T;T=T->Next)
	{
		if (T->Rotate.NumKeys>1)
		{
			printf("Orientation Track Keypoint List follows.\n");
			for(I=0;I<T->Rotate.NumKeys;I++)
				printf("(%f,(%f,%f,%f))\n",T->Rotate.Keys[I].Pos.W,
																	 T->Rotate.Keys[I].Pos.X,
																	 T->Rotate.Keys[I].Pos.Y,
																	 T->Rotate.Keys[I].Pos.Z);
		}
	}*/

	RunScene(CurScene,20.0);/**/

// das new units - testing routine
//  Perform_UnitTests();

// das demo
/*  Build_Scenes(2);
//  Install_Rocket_Runaway(SceneHead);
//  Install_Particle_Dreams(SceneHead->Next);

	for(Sc=SceneHead;Sc;Sc=Sc->Next)
		Preprocess_Scene(Sc);

// Start music
#ifdef Play_Music
	Play_Module();
#endif

//  Run_Rocket_Runaway(SceneHead);
//  Run_Particle_Dreams(SceneHead->Next,4000);/**/
#ifdef Play_Music
	ShutDown();
#endif
	FDS_End();
}
