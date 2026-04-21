#include "Rev.h"
#include "IMGGENR/IMGGENR.H"
#include "VESA/Vesa.h"

#include <algorithm>

void Cross_Fade(byte *U1,byte *U2,byte *Target,int32_t Perc)
{
	int32_t I;
	for(I=0;I<PageSize;I++)
		Target[I] = (U1[I]*(255-Perc) + U2[I]*Perc)>>8;
}


	// tables, generated and used as an optimization.
#define TRIG_ACC 4096 //trigonometric table accuracy. must be a power of 2.
#define TRIG_MASK (TRIG_ACC-1)
#define TRIG_FACTOR (float(TRIG_ACC)/PI_M2)

#ifdef _C_WATCOM
#define VSurface Screen
#else
#define VSurface MainSurf
#endif


static float *LenTable;
static float *SinTable;
static float *CosTable;
//static NewGridPoint *Plane_GP;
//static GridPoint *Plane_GP;
static GridPointTG *Plane_GP;
static GridPointT *Code_GP;
static GridPointT *Gfx_GP;
static GridPointT *Sfx_GP;
static VESA_Surface Surf1;
static VESA_Surface Surf2;
static VESA_Surface Surf3;
static VESA_Surface Surf4;
static VESA_Surface FinalSurf;
static int32_t numGridPoints;
static byte *Page1;
static byte *Page2;
static byte *Page3;
static byte *Page4;
static byte* FinalPage;
static int32_t TrigOffset;
static Texture *LogoTexture;
static Image *LogoImage;
static Texture *PlaneTexture;
static Image *PlaneImage;
static Texture *CodeTexture;
static Image *CodeImage;
static Texture *GfxTexture;
static Image *GfxImage;
static Texture *SfxTexture;
static Image *SfxImage;

static int32_t InitScreenXRes, InitScreenYRes;


void Initialize_Glato()
{
	InitScreenXRes = XRes;
	InitScreenYRes = YRes;
	int32_t xres = InitScreenXRes;
	int32_t yres = InitScreenYRes;
	int x,y,i,j;
	int X,Y;
	float XResFactor = xres/320.0;


	LogoTexture = new Texture;
	LogoImage = new Image;
	PlaneTexture = new Texture;
	PlaneImage = new Image;
	CodeTexture = new Texture;
	CodeImage = new Image;
	GfxTexture = new Texture;
	GfxImage = new Image;
	SfxTexture = new Texture;
	SfxImage = new Image;

	Load_Image_JPEG(LogoImage,"Textures/Logo.JPG");
	Scale_Image(LogoImage,xres,yres);

/*	LogoTexture->FileName = strdup("Textures/Logo.JPG");
	Identify_Texture(LogoTexture);
	if (!LogoTexture->BPP)
	{
		printf("Error Loading texture!\n");
		exit(1);
	}
	Load_Texture(LogoTexture);
	BPPConvert_Texture(LogoTexture,32);
	Convert_Texture2Image(LogoTexture,LogoImage);*/
	

	PlaneTexture->FileName = strdup("Textures/SC13.JPG");
	Identify_Texture(PlaneTexture);
	if (!PlaneTexture->BPP)
	{
		printf("Error Loading texture!\n");
		exit(1);
	}
	Load_Texture(PlaneTexture);
	Convert_Texture2Image(PlaneTexture,PlaneImage);
	Sachletz(PlaneImage->Data, PlaneImage->x, PlaneImage->y);
//	memset(PlaneImage->Data, 128, 256 * 256 * 4);
	//PlaneImage->Data[0] = 0x80808080;
//	WOBPOINTSHEIGHT = 30;

	CodeTexture->FileName = strdup("Textures/Code.JPG");
	Identify_Texture(CodeTexture);
	if (!CodeTexture->BPP)
	{
		printf("Error Loading texture!\n");
		exit(1);
	}
	Load_Texture(CodeTexture);
	Convert_Texture2Image(CodeTexture,CodeImage);


	GfxTexture->FileName = strdup("Textures/Gfx.JPG");
	Identify_Texture(GfxTexture);
	if (!GfxTexture->BPP)
	{
		printf("Error Loading texture!\n");
		exit(1);
	}
	Load_Texture(GfxTexture);
	Convert_Texture2Image(GfxTexture,GfxImage);

	SfxTexture->FileName = strdup("Textures/Sfx.JPG");
	Identify_Texture(SfxTexture);
	if (!SfxTexture->BPP)
	{
		printf("Error Loading texture!\n");
		exit(1);
	}
	Load_Texture(SfxTexture);
	Convert_Texture2Image(SfxTexture,SfxImage);


	Page1 = (byte*)_aligned_malloc(PageSize, 32);
	Page2 = (byte*)_aligned_malloc(PageSize, 32);
	Page3 = (byte*)_aligned_malloc(PageSize, 32);
	Page4 = (byte*)_aligned_malloc(PageSize, 32);
	FinalPage = (byte*)_aligned_malloc(PageSize, 32);

	// only last YRes - YRes & (~7) lines should be cleared
	memset(Page1, 0, PageSize);
	memset(Page2, 0, PageSize);
	memset(Page3, 0, PageSize);
	memset(Page4, 0, PageSize);
	memset(FinalPage, 0, PageSize);

	memcpy(&Surf1,VSurface,sizeof(VESA_Surface));
	memcpy(&Surf2,VSurface,sizeof(VESA_Surface));
	memcpy(&Surf3,VSurface,sizeof(VESA_Surface));
	memcpy(&Surf4,VSurface,sizeof(VESA_Surface));
	memcpy(&FinalSurf, VSurface, sizeof(VESA_Surface));
	Surf1.Data = Page1;
	Surf2.Data = Page2;
	Surf3.Data = Page3;
	Surf4.Data = Page4;
	FinalSurf.Data = FinalPage;
	Surf1.Flags = VSurf_Noalloc;
	Surf1.Targ = NULL;
	Surf2.Flags = VSurf_Noalloc;
	Surf2.Targ = NULL;
	Surf3.Flags = VSurf_Noalloc;
	Surf3.Targ = NULL;
	Surf4.Flags = VSurf_Noalloc;
	Surf4.Targ = NULL;
	FinalSurf.Flags = VSurf_Noalloc;
	FinalSurf.Targ = NULL;

	numGridPoints = ((xres>>3)+1)*((yres>>3)+1);
	//Plane_GP = new NewGridPoint[numGridPoints];
	//Plane_GP = new GridPoint[numGridPoints];
	Plane_GP = new GridPointTG[numGridPoints];
	Code_GP = new GridPointT[numGridPoints];
	Gfx_GP = new GridPointT[numGridPoints];
	Sfx_GP = new GridPointT[numGridPoints];
	
	memset(Plane_GP, 0, sizeof(GridPointTG) * numGridPoints);
	LenTable = new float [numGridPoints];
	SinTable = new float [TRIG_ACC];
	CosTable = new float [TRIG_ACC];
	
	j = 0;
	for(i=0; i<TRIG_ACC; i++)
	{
		SinTable[i] = sin(i*PI_M2/TRIG_ACC);
		CosTable[i] = cos(i*PI_M2/TRIG_ACC);
	}

	for (y=0;y<=YRes;y+=8)
	{
		for (x=0;x<=xres;x+=8)
		{
			X = x - xres * 0.5;
			Y = y - yres * 0.5;

			LenTable[j] = sqrt((float)(X*X + Y*Y))/XResFactor;
			j++;
		}
	}
}

static inline float max_magnitude(float a, float b)
{
	if (fabs(a) > fabs(b)) return a; else return b;
}

void Run_Glato(void)
{
//	Setup_Grid_Texture_Mapper_XXX(XRes, YRes);
//	Setup_Grid_Texture_Mapper_MMX(XRes, YRes);
	int32_t xres = InitScreenXRes;
	int32_t yres = InitScreenYRes;
	Setup_Grid_Texture_Mapper_MMX(xres, yres);

	XMMVector CameraPos(0,0,0);
	XMMMatrix CamMat;
	float Radius;
	int x,y,i,j;
	float a = 0.0f,bb = 0.0f,c = 0.0f,d = 0.0f,Delta = 0.0f,X1 = 0.0f,X2 = 0.0f,X3 = 0.0f,z = 0.0f,Rx = 0.0f,Ry = 0.0f,Rz = 0.0f;
	float u,v,u1,v1,u2,v2,r,g,b;
	float Code_R1,Code_RS,Code_R2,CCosR1,CSinR1,CCosR2,CSinR2;
	float Gfx_R1,Gfx_R2,GCosR1,GSinR1,GCosR2,GSinR2,Gfx_RS;
	XMMVector Intersection1,Origin1,Direction1,U;
	
	int X,Y;
	float R1,R3,R4;

	Radius = 1;


	int Gfx = 0,Sfx = 0, Code = 1;
	// Run wobbler
#ifdef _C_WATCOM
#ifdef Play_Music
	Play_Module();
#endif
#endif

	float ST;
	int32_t timerStack[20], timerIndex = 0;
	for(i=0; i<20; i++)
		timerStack[i] = Timer;

	char MSGStr[MAX_GSTRING];

	float XResFactor = xres/320.0;
	float rXResFactor = 320.0/xres;
	float rYResFactor = 240.0/yres;

	dword TTrd = Timer;

	// clear the screen once (only yres % 8 last lines are really needed)
	memset(FinalPage, 0, PageSize);

	while (Timer<3500)
	{
		bool skip = false;
		// fast forward/rewind
		dTime = Timer-TTrd;		
		if (Keyboard[ScF2])
		{
			skip = true;
			Timer += dTime * 8;
		}
		if (Keyboard[ScF1])
		{
			skip = true;
			if (dTime * 8 > Timer)
				Timer = 0;
			else
				Timer -= dTime * 8;
		}
		TTrd = Timer;

		if (Timer <= 100 * 11)
			ST = (Timer*2500)/(1000+Timer);//  sqrt(Timer*1600);
		if (Timer > 100 * 11){ Gfx = 1;Code = 0;
			ST = ((Timer-1100)*2500)/(1000+(Timer-1100));//  sqrt(Timer*1600);
		}
		if (Timer > 100 * 23){ Gfx = 0;Sfx = 1;
			ST = ((Timer-2300)*2500)/(1000+(Timer-2300));//  sqrt(Timer*1600);
		}
//		ST = (Timer*2000)/(1000+Timer);//  sqrt(Timer*1600);
		Euler_Angles(CamMat.Data,Rx,Ry,Rz);
		i=0;
		j=0;
		//code
		Code_R1 = ST * 0.0005;
		// back
		TrigOffset = Code_R1 * 0.1 * TRIG_FACTOR;
		TrigOffset &= TRIG_MASK;
		CSinR1 = SinTable[TrigOffset];
		CCosR1 = CosTable[TrigOffset];
	if (ST < 700)
		Code_RS = Code_R1 * 0.5;

		Gfx_R1 = - (ST) * 0.000001;

		TrigOffset = Gfx_R1 * 0.05 * TRIG_FACTOR;
		TrigOffset &= TRIG_MASK;
		GCosR1 = CosTable[TrigOffset];
		GSinR1 = SinTable[TrigOffset];
		//GSinR1 = sin(Gfx_R1*0.05);
		//GCosR1 = cos(Gfx_R1*0.05);
		if (ST < 400)
			Gfx_RS = (ST) * 0.00001;


		//Origin1.x=CameraPos.x;
		//Origin1.y=CameraPos.y;
		//Origin1.z=CameraPos.z;
	
		Radius = 1;
		Origin1 = CameraPos;
		// Clear page isn't required as wobbler overwrites entire screen / frame
//		memset(VPage, 0, PageSize);
		for (y=0;y<=yres;y+=8)
			for (x=0;x<=xres;x+=8)
			{
				Direction1.x=x-(xres >> 1);
				Direction1.y=y-(yres >> 1);
				Direction1.z=256.0*XResFactor;
				Direction1.w = .0f;
				MatrixXVector(CamMat.Data,&Direction1,&U);
				Direction1=U;
				Direction1.Normalize();
//				Radius = std::max(std::max(fabs(Direction1.y), fabs(Direction1.x)), fabs(Direction1.z));//1.0;//sin(Direction1.x) * cos(Direction1.z);
				a=Radius-Origin1.y;
				c=-Radius-Origin1.y;
				d=Direction1.y;
				
				if (d<=0)
				{
					if (d == 0) {
						X2 = 0;
					} else {
						X2 = c / d;
					}
					Intersection1 = Origin1 + Direction1 * X2;
					//Intersection1.x = Origin1.x + Direction1.x * X2;
					//Intersection1.y = Origin1.y + Direction1.y * X2;
					//Intersection1.z = Origin1.z + Direction1.z * X2;

					TrigOffset = (Intersection1.x + (float)(ST*0.1f) / 28.65f) * TRIG_FACTOR;
					TrigOffset &= TRIG_MASK;
					u = (Intersection1.x + CosTable[TrigOffset] * 0.5f)*0.5f;
					TrigOffset = (Intersection1.z + (float)(ST*0.1f) / 28.65f) * TRIG_FACTOR;
					TrigOffset &= TRIG_MASK;
					v = (Intersection1.z + SinTable[TrigOffset] * 0.5f)*0.5f;


					//u=(Intersection1.x+cos(Intersection1.z+(float)(ST*0.1f)/28.65f)*0.5f)*0.5f;
					//v=(Intersection1.z+sin(Intersection1.x+(float)(ST*0.1f)/28.65f)*0.5f)*0.5f;
					//Intersection1.x-=Origin1.x;
					//Intersection1.y-=Origin1.y;
					//Intersection1.z-=Origin1.z;
					Intersection1 -= Origin1;
					//r=(sqrt(Intersection1.x*Intersection1.x+Intersection1.y*Intersection1.y+Intersection1.z*Intersection1.z)*32);
					r = Intersection1.Length() * 32.f;
					//r*= 1.8;//1.3;
					if (r>253.0f) r=253.0f;
					r=255.0f-r;
					if (r<2.0f) r=2.0f;
					b=r * 0.7f;
					g= r*0.8f;
					//r*= 0.5;
					//g-=Timer /(40 * 4);
					//b-=Timer /(80 * 4);
					//g-=Frames /10;
					//b-=Frames /20;

					if (g>254.0f) g=254.0f;
					if (g<1.0f) g=1.0f;
					if (b>254.0f) b=254.0f;
					if (b<1.0f) b=1.0f;

					u*=256.0f;
					v*=256.0f;
				}
				else
				{
					X1=a/d;
					//Intersection1.x = Origin1.x + Direction1.x * X1;
					//Intersection1.y = Origin1.y + Direction1.y * X1;
					//Intersection1.z = Origin1.z + Direction1.z * X1;
					Intersection1 = Origin1 + Direction1 * X1;

					TrigOffset = (Intersection1.x + (float)(ST*0.1f) / 28.65f) * TRIG_FACTOR;
					TrigOffset &= TRIG_MASK;
					u = (Intersection1.x + CosTable[TrigOffset] * 0.5f)*0.5f;
					TrigOffset = (Intersection1.z + (float)(ST*0.1f) / 28.65f) * TRIG_FACTOR;
					TrigOffset &= TRIG_MASK;
					v = (Intersection1.z + SinTable[TrigOffset] * 0.5f)*0.5f;
					//u=(Intersection1.x+cos(Intersection1.z+(float)(ST*0.1f)/28.65f)*0.5f)*0.5f;
					//v=(Intersection1.z+sin(Intersection1.x+(float)(ST*0.1f)/28.65f)*0.5f)*0.5f;
					//Intersection1.x-=Origin1.x;
					//Intersection1.y-=Origin1.y;
					//Intersection1.z-=Origin1.z;
					Intersection1 -= Origin1;
					r = Intersection1.Length() * 32.f;
					//r=(sqrt(Intersection1.x*Intersection1.x+Intersection1.y*Intersection1.y+Intersection1.z*Intersection1.z) * 32);
//					if (r < 0.0f)
//						r = 0;
//					else
//						r = sqrt(r);
					//r*= 1.8;//1.3;
					r=255.0f-r;
					if (r>253.0f) r=253.0f;
					if (r<2.0f) r=2.0f;
					b=r * 0.7f;
					g= r*0.8f;
					//r*=0.5;
					//g-=Timer /(40 * 4);
					//b-=Timer /(80 * 4);
					//g-=Frames /10;
					//b-=Frames /20;

					if (g>254.0f) g=254.0f;
					if (g<1.0f) g=1.0f;
					if (b>254.0f) b=254.0f;
					if (b<1.0f) b=1.0f;

					u*=256.0f;
					v*=256.0f;
				}
				//r = 0; // green
				//g = 0; // red
				//b = g = r; // blue

				//r*=254.0f;
				//g*=254.0f;
				//b*=254.0f;
				r*=63.0f;
				g*=63.0f;
				b*=63.0f;

				Plane_GP[j].u=u;
				Plane_GP[j].v=v;
				Plane_GP[j].BGRA = Vec8us{ uint16_t(b) , uint16_t(g), uint16_t(r) , 0, uint16_t(b) , uint16_t(g), uint16_t(r) , 0};


				X = x - xres * 0.5;
				Y = y - yres * 0.5;

				if (Code)
				{
//					Code_R2 = sqrt(X*X + Y*Y) / (200.0f*XResFactor)+ST /100.0f;
//					CCosR2 = cos (Code_R2);
//					CSinR2 = sin (Code_R2);
					Code_R2 = LenTable[j] / 200.0f + ST/100.0f;					
					TrigOffset = Code_R2 * TRIG_FACTOR;
					TrigOffset &= TRIG_MASK;
					CCosR2 = CosTable[TrigOffset];
					CSinR2 = SinTable[TrigOffset];


					u = X * (204.8f * rXResFactor) * -(Code_RS * 5);
					v = Y * (327.68f * rYResFactor) * -(Code_RS * 5);

					u1 = (u) * CSinR1  + (v) * CCosR1;
					v1 = (u) * CCosR1  - (v) * CSinR1;

					u2 = (u1) * CSinR2  + (v1) * CCosR2;
					v2 = (u1) * CCosR2  - (v1) * CSinR2;

//					r = g = b = 127.0f;

					r*=254.0f;
					g*=254.0f;
					b*=254.0f;

					Code_GP[j].u=u2+32767;
					Code_GP[j].v=v2+32767;
					if (Code_GP[j].u > 65535) Code_GP[j].u = 65535;
					if (Code_GP[j].v > 65535) Code_GP[j].v = 65535;
					if (Code_GP[j].u < 0) Code_GP[j].u = 0;
					if (Code_GP[j].v < 0) Code_GP[j].v = 0;
//					Code_GP[j].R=r;
//					Code_GP[j].G=g;
//					Code_GP[j].B=b;
				}

				if (Gfx)
				{
//					Gfx_R2 = sqrt(X*X + Y*Y) / (120.0 * XResFactor) - (ST) /(100.0);
//					GCosR2 = cos (Gfx_R2);
//					GSinR2 = sin (Gfx_R2);
					Gfx_R2 = LenTable[j] / 120.0f - ST/100.0f;
					TrigOffset = Gfx_R2 * TRIG_FACTOR;
					TrigOffset &= TRIG_MASK;
					GCosR2 = CosTable[TrigOffset];
					GSinR2 = SinTable[TrigOffset];


					u = X * (204.8*rXResFactor) * (Gfx_RS*20.0);
					v = Y * (327.68*rYResFactor) * (Gfx_RS*20.0);

					u1 = (u) * GSinR1  + (v) * GCosR1;
					v1 = (u) * GCosR1  - (v) * GSinR1;

					u1*=10.0;	v1*=10.0;

					u2 = (u1) * GSinR2  + (v1) * GCosR2;
					v2 = (u1) * GCosR2  - (v1) * GSinR2;

//					r = g = b = 127.0;

					r*=254.0;
					g*=254.0;
					b*=254.0;

					Gfx_GP[j].u=u2+32767;
					Gfx_GP[j].v=v2+32767;
					if (Gfx_GP[j].u > 65535) Gfx_GP[j].u = 65535;
					if (Gfx_GP[j].v > 65535) Gfx_GP[j].v = 65535;
					if (Gfx_GP[j].u < 0) Gfx_GP[j].u = 0;
					if (Gfx_GP[j].v < 0) Gfx_GP[j].v = 0;
//					Gfx_GP[j].R=r;
//					Gfx_GP[j].G=g;
//					Gfx_GP[j].B=b;
				}
		
				if (Sfx)
				{
//					Code_R2 = sqrt(X*X + Y*Y) / (200.0*XResFactor)+ST /100.0;
//					CCosR2 = cos (-Code_R2);
//					CSinR2 = sin (-Code_R2);
					Code_R2 = LenTable[j] / 200.0f + ST/100.0f;
					TrigOffset = Code_R2 * TRIG_FACTOR;
					TrigOffset &= TRIG_MASK;
					CCosR2 = CosTable[TrigOffset];
					CSinR2 =-SinTable[TrigOffset];

					u = X * (204.8*rXResFactor) * -(Code_RS * 5);
					v = Y * (327.68*rYResFactor) * -(Code_RS * 5);

					u1 = (u) * CSinR1  + (v) * CCosR1;
					v1 = (u) * CCosR1  - (v) * CSinR1;

					u2 = (u1) * CSinR2  + (v1) * CCosR2;
					v2 = (u1) * CCosR2  - (v1) * CSinR2;

//					r = g = b = 127.0;

					r*=254.0;
					g*=254.0;
					b*=254.0;

					Sfx_GP[j].u=u2+32767;
					Sfx_GP[j].v=v2+32767;

//					Sfx_GP[j].R=r;
//					Sfx_GP[j].G=g;
//					Sfx_GP[j].B=b;

					if (Sfx_GP[j].u > 65535) Sfx_GP[j].u = 65535;
					if (Sfx_GP[j].v > 65535) Sfx_GP[j].v = 65535;
					if (Sfx_GP[j].u < 0) Sfx_GP[j].u = 0;
					if (Sfx_GP[j].v < 0) Sfx_GP[j].v = 0;
				}
				j++;
			}

//		Grid_Texture_Mapper_XXX(Plane_GP,PlaneImage,(DWord *)Page1);
		Grid_Texture_Mapper_TG(Plane_GP,PlaneImage,(DWord *)Page1, xres, yres);
		//GridRendererTG(Plane_GP,PlaneImage,(DWord *)Page1, XRes, YRes);

		if (Code)
		{
//			Grid_Texture_Mapper_XXX(Code_GP,CodeImage,(DWord *)Page2);
			GridRendererT(Code_GP,CodeImage,(DWord *)Page2, Surf1.X, Surf1.Y);
			//Grid_Texture_Mapper_T(Code_GP, CodeImage, (DWord *)Page2);
			Modulate(&Surf1,&Surf2,0xa0a0a0,0xa0a0a0, Surf1.PageSize);
			Modulate(&Surf2,&FinalSurf,0xa0a0a0, 0xb0b0b0, Surf2.PageSize);
		}
		if (Gfx)
		{
//			Grid_Texture_Mapper_XXX(Gfx_GP,GfxImage,(DWord *)Page3);
			GridRendererT(Gfx_GP,GfxImage,(DWord *)Page3, Surf3.X, Surf3.Y);
			Modulate(&Surf1,&Surf3,0xa0a0a0, 0xd0d0d0, Surf1.PageSize);
			Modulate(&Surf3,&FinalSurf,0xa0a0a0, 0xb0b0b0, Surf3.PageSize);
 //			Modulate(&Surf2,&Surf3,0xa0a0a0,0xa0a0a0);
		}
		if (Sfx)
		{
//			Grid_Texture_Mapper_XXX(Sfx_GP,SfxImage,(DWord *)Page4);
			GridRendererT(Sfx_GP,SfxImage,(DWord *)Page4, Surf4.X, Surf4.Y);
			Modulate(&Surf1,&Surf4,0xa0a0a0,0xa0a0a0, Surf1.PageSize);
			Modulate(&Surf4,&FinalSurf,0xa0a0a0, 0xb0b0b0, Surf4.PageSize);
		}
//		memcpy(VPage, Page1, PageSize);
		if (Timer>3200)
		{
			int32_t cfVal = (Timer-3200)*255/300;
			if (cfVal>255) cfVal = 255;
			DWord SrcPer = ((DWord)cfVal) * 0x01010101;
			DWord DstPer = ((DWord)(255-cfVal)) * 0x01010101;
			AlphaBlend((byte *)LogoImage->Data, FinalPage, SrcPer, DstPer, FinalSurf.PageSize);
		}
		//if (Timer < 750)
		//{
		//	_sleep((750 - Timer) / 5);
		//}
		// FPS printer
		if (g_profilerActive)
		{
			dword tm = Timer;
			timerStack[timerIndex++] = tm;
			{
				if (timerIndex == 20)
				{
					timerIndex = 0;
					sprintf(MSGStr, "%f FPS", 2000.0 / (float)(timerStack[19] - timerStack[timerIndex]));
				}
				else {
					sprintf(MSGStr, "%f FPS", 2000.0 / (float)(timerStack[timerIndex - 1] - timerStack[timerIndex]));
				}
			}

			if (skip) {
				sprintf(MSGStr, "%f FPS", (float)(tm - TTrd));
			}

			OutTextXY(FinalPage,0,0,MSGStr,255, xres, yres);
		}
		Flip(&FinalSurf);
//		Flip(&Surf1);

//		Rx += 0.01;
//		Ry += 0.01;
//		CameraPos.z += 0.01;
		Rx = Timer / 420.0;
		Ry = Timer / 420.0;
		CameraPos.z = Timer / 420.0;
		Frames++;

//      r1,r2
// bg  code      gfx sfx

//		Flip(VSurface);
		if (Keyboard[ScESC])
			Timer = 1000000;
	}

	while (Keyboard[ScESC]) continue;

//	Timer -= 3500;

///	if (Keyboard[ScESC])
//	{
//		#ifdef Play_Music
///		ShutDown();
//		#endif
//		FDS_End();
//		exit(-1);
//	}
	delete [] LenTable;
	delete [] CosTable;
	delete [] SinTable;

	delete Plane_GP;
	delete Code_GP;
	delete Gfx_GP;
	delete Sfx_GP;
	_aligned_free(Page1);
	_aligned_free(Page2);
	_aligned_free(Page3);
	_aligned_free(Page4);
}
