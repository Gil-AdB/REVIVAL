#include "Rev.h"
#include <Base/FeatureFlags.h>
#include "FrameProfiler.h"
#include "GLAT.H"
#include "IMGGENR/IMGGENR.H"
#include "SceneTick.h"
#include "Scenes.h"
#include "SDL2.h"
#include "VESA/Vesa.h"

#include <algorithm>
#include <memory>

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

#define VSurface MainSurf


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
// Owns FinalSurf's dedicated SDL_Texture. Keeps it alive across engine
// resizes (which destroy MainSurf's texture but not this one) and ensures
// it's destroyed before the renderer at process exit.
static SDLTex s_glatFinalTex;
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
// Glat's current pipeline dims and page size. Updated by Rebuild_Glato_Sized
// at boot and on each engine-resize event so that GlatoScene's per-frame
// reads stay in sync with the size of Page1-4/FinalPage/FinalSurf and the
// grid arrays. Decoupled from the global XRes/YRes/PageSize so a resize
// applied after Glat's per-scene init() can't OOB our buffers.
static int32_t InitScreenPageSize;

GlatoTraceHook g_glatoTraceHook = nullptr;

// Free + reallocate every per-resolution buffer in Glat's pipeline at the
// given dimensions, then rebuild dependent state (LogoImage scale, grid
// point arrays, LenTable, FinalSurf SDL_Texture). Called once from
// Initialize_Glato (with all pointers null) and again from
// GlatoScene::on_resize whenever the engine resizes. The size-independent
// state — SinTable/CosTable, the Texture/Image structs themselves, the
// JPEG-decoded source data of non-Logo textures — stays in place.
static void Rebuild_Glato_Sized(int xres, int yres)
{
	// Glat's grid texture mapper writes 8 pixels per inner-loop iteration but
	// advances scanline pointers by xres dwords. When xres isn't a multiple
	// of 8 the last iteration of each row overwrites the start of the next
	// row — visible as a diagonal stride-mismatch shear. Round both dims
	// down so the rendered area is grid-aligned; SDL_RenderCopy stretches
	// the (slightly smaller) texture to the window so the user sees a tiny
	// 0-7 pixel inset on the right/bottom rather than a corrupted frame.
	xres &= ~7;
	yres &= ~7;
	if (xres < 8) xres = 8;
	if (yres < 8) yres = 8;

	const int32_t pageSize = xres * yres * 4;  // 32bpp
	const int32_t bpsl = xres * 4;

	// LogoImage is the only resolution-scaled input. Scale_Image is
	// destructive (frees Img->Data and replaces with a scaled copy), so to
	// re-target a new size we re-load from disk, then re-scale.
	if (LogoImage->Data) {
		_aligned_free(LogoImage->Data);
		LogoImage->Data = nullptr;
	}
	Load_Image_JPEG(LogoImage, "TEXTURES/LOGO.JPG");
	Scale_Image(LogoImage, xres, yres);

	if (Page1) _aligned_free(Page1);
	if (Page2) _aligned_free(Page2);
	if (Page3) _aligned_free(Page3);
	if (Page4) _aligned_free(Page4);
	if (FinalPage) _aligned_free(FinalPage);
	Page1 = (byte*)_aligned_malloc(pageSize, 32);
	Page2 = (byte*)_aligned_malloc(pageSize, 32);
	Page3 = (byte*)_aligned_malloc(pageSize, 32);
	Page4 = (byte*)_aligned_malloc(pageSize, 32);
	FinalPage = (byte*)_aligned_malloc(pageSize, 32);
	memset(Page1, 0, pageSize);
	memset(Page2, 0, pageSize);
	memset(Page3, 0, pageSize);
	memset(Page4, 0, pageSize);
	memset(FinalPage, 0, pageSize);

	// Surf1-4 and FinalSurf carry renderer/Flip pointers from VSurface
	// (= MainSurf). After an engine resize, MainSurf has a fresh
	// SDL_Texture which we don't want to inherit for these — we override
	// the size + Data + (FinalSurf only) Handle below.
	memcpy(&Surf1, VSurface, sizeof(VESA_Surface));
	memcpy(&Surf2, VSurface, sizeof(VESA_Surface));
	memcpy(&Surf3, VSurface, sizeof(VESA_Surface));
	memcpy(&Surf4, VSurface, sizeof(VESA_Surface));
	Surf1.Data = Page1; Surf2.Data = Page2; Surf3.Data = Page3; Surf4.Data = Page4;
	Surf1.X = Surf2.X = Surf3.X = Surf4.X = xres;
	Surf1.Y = Surf2.Y = Surf3.Y = Surf4.Y = yres;
	Surf1.BPSL = Surf2.BPSL = Surf3.BPSL = Surf4.BPSL = bpsl;
	Surf1.PageSize = Surf2.PageSize = Surf3.PageSize = Surf4.PageSize = pageSize;
	Surf1.Flags = Surf2.Flags = Surf3.Flags = Surf4.Flags = VSurf_Noalloc;
	Surf1.Targ = Surf2.Targ = Surf3.Targ = Surf4.Targ = NULL;

	// Drop the previous SDL_Texture (logged by the deleter) before
	// memcpy stomps the alias in FinalSurf.Handle.
	s_glatFinalTex.reset();
	memcpy(&FinalSurf, VSurface, sizeof(VESA_Surface));
	FinalSurf.Data = FinalPage;
	FinalSurf.X = xres;
	FinalSurf.Y = yres;
	FinalSurf.BPSL = bpsl;
	FinalSurf.PageSize = pageSize;
	FinalSurf.Flags = VSurf_Noalloc;
	FinalSurf.Targ = NULL;
	// Dedicated SDL_Texture sized to current Glat dims. Engine resize
	// destroys MainSurf's texture but leaves this one alone, so Glat keeps
	// flipping into a valid texture across resize events.
	s_glatFinalTex = SDL2_CreateChildTexture(xres, yres, "glat-final");
	FinalSurf.Handle = static_cast<void *>(s_glatFinalTex.get());

	if (Plane_GP) delete[] Plane_GP;
	if (Code_GP) delete[] Code_GP;
	if (Gfx_GP) delete[] Gfx_GP;
	if (Sfx_GP) delete[] Sfx_GP;
	if (LenTable) delete[] LenTable;
	numGridPoints = ((xres>>3)+1)*((yres>>3)+1);
	Plane_GP = new GridPointTG[numGridPoints];
	Code_GP = new GridPointT[numGridPoints];
	Gfx_GP = new GridPointT[numGridPoints];
	Sfx_GP = new GridPointT[numGridPoints];
	memset(Plane_GP, 0, sizeof(GridPointTG) * numGridPoints);
	LenTable = new float[numGridPoints];

	const float XResFactor = xres / 320.0f;
	int j = 0;
	for (int y = 0; y <= yres; y += 8) {
		for (int x = 0; x <= xres; x += 8) {
			float X = x - xres * 0.5f;
			float Y = y - yres * 0.5f;
			LenTable[j++] = sqrtf(X*X + Y*Y) / XResFactor;
		}
	}

	Setup_Grid_Texture_Mapper_MMX(xres, yres);

	InitScreenXRes = xres;
	InitScreenYRes = yres;
	InitScreenPageSize = pageSize;
}


void Initialize_Glato()
{
	int32_t xres = XRes;
	int32_t yres = YRes;


	LogoTexture = new Texture;
	LogoImage = new Image;
	memset(LogoImage, 0, sizeof(Image));  // Data=NULL so Rebuild_Glato_Sized's free guard is a no-op first time.
	PlaneTexture = new Texture;
	PlaneImage = new Image;
	CodeTexture = new Texture;
	CodeImage = new Image;
	GfxTexture = new Texture;
	GfxImage = new Image;
	SfxTexture = new Texture;
	SfxImage = new Image;

/*	LogoTexture->FileName = strdup("TEXTURES/LOGO.JPG");
	Identify_Texture(LogoTexture);
	if (!LogoTexture->BPP)
	{
		printf("Error Loading texture!\n");
		exit(1);
	}
	Load_Texture(LogoTexture);
	BPPConvert_Texture(LogoTexture,32);
	Convert_Texture2Image(LogoTexture,LogoImage);*/
	

	PlaneTexture->FileName = strdup("TEXTURES/SC13.JPG");
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

	CodeTexture->FileName = strdup("TEXTURES/CODE.JPG");
	Identify_Texture(CodeTexture);
	if (!CodeTexture->BPP)
	{
		printf("Error Loading texture!\n");
		exit(1);
	}
	Load_Texture(CodeTexture);
	Convert_Texture2Image(CodeTexture,CodeImage);


	GfxTexture->FileName = strdup("TEXTURES/GFX.JPG");
	Identify_Texture(GfxTexture);
	if (!GfxTexture->BPP)
	{
		printf("Error Loading texture!\n");
		exit(1);
	}
	Load_Texture(GfxTexture);
	Convert_Texture2Image(GfxTexture,GfxImage);

	SfxTexture->FileName = strdup("TEXTURES/SFX.JPG");
	Identify_Texture(SfxTexture);
	if (!SfxTexture->BPP)
	{
		printf("Error Loading texture!\n");
		exit(1);
	}
	Load_Texture(SfxTexture);
	Convert_Texture2Image(SfxTexture,SfxImage);


	// Trig lookup tables: resolution-independent, set up once here.
	SinTable = new float[TRIG_ACC];
	CosTable = new float[TRIG_ACC];
	for (int i = 0; i < TRIG_ACC; i++) {
		SinTable[i] = sin(i*PI_M2/TRIG_ACC);
		CosTable[i] = cos(i*PI_M2/TRIG_ACC);
	}

	// Allocate + populate every per-resolution buffer for the current
	// engine dimensions. Subsequent engine resize events re-call this
	// (via GlatoScene::on_resize) to retarget to the new size.
	Rebuild_Glato_Sized(xres, yres);
}

static inline float max_magnitude(float a, float b)
{
	if (fabs(a) > fabs(b)) return a; else return b;
}

namespace {
struct GlatoScene : SceneDriver {
	int32_t xres = 0;
	int32_t yres = 0;
	float XResFactor = 0.0f;
	float rXResFactor = 0.0f;
	float rYResFactor = 0.0f;

	XMMVector CameraPos{0, 0, 0};
	XMMMatrix CamMat;
	float Rx = 0.0f, Ry = 0.0f, Rz = 0.0f;

	// Conditionally written inside tick (only while ST is in range); reads
	// outside that window must see the last computed value, not 0.
	float Code_RS = 0.0f;
	float Gfx_RS = 0.0f;

	int32_t TTrd = 0;          // shared transport state (pause / scrub)
	bool    pause_mode = false;
	int32_t timerStack[20] = {};
	int32_t timerIndex = 0;
	FrameProfiler prof{"glato"};

	char MSGStr[MAX_GSTRING] = {};

	void capture_dims() {
		xres = InitScreenXRes;
		yres = InitScreenYRes;
		XResFactor = float(xres) / 320.0f;
		rXResFactor = 320.0f / float(xres);
		rYResFactor = 240.0f / float(yres);
	}

	void init() override {
		capture_dims();
		Setup_Grid_Texture_Mapper_MMX(xres, yres);

		for(int i = 0; i < 20; i++)
			timerStack[i] = Timer;

		TTrd = Timer;

		// clear the screen once (only yres % 8 last lines are really needed)
		memset(FinalPage, 0, InitScreenPageSize);
	}

	void on_resize(int newX, int newY) override {
		// Rebuild every per-resolution buffer Glat owns at the new size,
		// then re-capture the scene-level scaling factors so the per-frame
		// math (UV strides, code/gfx swirl scales) matches the new dims.
		Rebuild_Glato_Sized(newX, newY);
		capture_dims();
		memset(FinalPage, 0, InitScreenPageSize);
	}

	bool tick() override {
		if (Timer >= 3500) return false;

		prof.beginFrame();
		prof.enter(PROF_ANIM);

		int x, y, i, j;
		float a = 0.0f, bb = 0.0f, c = 0.0f, d = 0.0f;
		float X1 = 0.0f, X2 = 0.0f;
		float u, v, u1, v1, u2, v2, r, g, b;
		float Code_R1 = 0.0f, Code_R2;
		float CCosR1, CSinR1, CCosR2, CSinR2;
		float Gfx_R1, Gfx_R2, GCosR1, GSinR1, GCosR2, GSinR2;
		XMMVector Intersection1, Origin1, Direction1, U;
		int X, Y;
		float Radius = 1;
		int Gfx = 0, Sfx = 0, Code = 1;
		float ST;

		// scrub state for the on-screen readout below; the actual
		// transport (pause P/U, fast-forward/rewind F2/F1, race-free
		// snapshot) is the shared SceneDriver::tickSceneTimer.
		const bool skip = Keyboard[ScF1] != 0 || Keyboard[ScF2] != 0;
		tickSceneTimer(TTrd, pause_mode);

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

		prof.switchTo(PROF_RNDR);

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
		// FPS printer — sourced from FrameProfiler so it agrees with TOTL.
		// Wall-clock text: same gating as the other scenes' FPS printers
		// (profiler_overlay + the screen_text master HUD switch).
		if (g_profilerActive && fds::FeatureFlags::profiler_overlay() &&
		    fds::FeatureFlags::screen_text())
		{
			dword tm = Timer;
			float meanMs = prof.meanFrameMs();
			float fps = meanMs > 0 ? 1000.0f / meanMs : 0.0f;
			snprintf(MSGStr, sizeof(MSGStr), "%f FPS", fps);

			if (skip) {
				snprintf(MSGStr, sizeof(MSGStr), "%f FPS", (float)(tm - TTrd));
			}

			OutTextXY(FinalPage,0,0,MSGStr,255, xres, yres);
		}
		prof.switchTo(PROF_FLIP);
		Flip(&FinalSurf);
		prof.leave(PROF_FLIP);
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
		// ESC = exit demo (handled by runSceneBlocking via g_shouldQuit);
		// Backspace = skip Glato. The original code abused Timer to trigger
		// the scene-end branch; we keep that behaviour for Backspace.
		if (Keyboard[ScBackSpace])
			Timer = 1000000;

		if (g_glatoTraceHook) {
			GlatoTraceSample s = {
				.timer = Timer,
				.st = ST,
				.rx = Rx, .ry = Ry, .rz = Rz,
				.camX = CameraPos.x,
				.camY = CameraPos.y,
				.camZ = CameraPos.z,
			};
			g_glatoTraceHook(s);
		}
		prof.endFrame();
		return true;
	}

	void cleanup() override {
		if (g_profilerActive) prof.dump();
		waitBackspaceRelease();

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
};
} // anonymous namespace

std::unique_ptr<SceneDriver> createGlatoScene()
{
	return std::make_unique<GlatoScene>();
}

void Run_Glato(void)
{
	auto scene = createGlatoScene();
	runSceneBlocking(*scene);
}
