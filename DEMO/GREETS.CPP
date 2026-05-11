#include "Rev.h"
#include <Threads.h>
#include "FrameProfiler.h"

// ---- Greets centering diagnostics --------------------------------------
// Two compile-time switches for narrowing where the native-vs-wasm
// shift is coming from. Flip to 1 to engage. Recompile + run.
//
// DEBUG_GREETS_BYPASS_WOBBLER: replaces the GridRendererT pass with a
// straight memcpy(CodeBuf, CodeImage.Data, ...). If text is centered
// with this on, the wobbler is the source. If still shifted, look
// further downstream (TexutreRemap, AlphaBlend, cube-map projection).
//
// DEBUG_GREETS_DUMP_GRID: stderr-dumps a few Code_GP[] values on every
// new greet so native and wasm can be diff'd to byte-exact precision.
// Only fires when the active greet pointer changes — no per-frame spam.
#define DEBUG_GREETS_BYPASS_WOBBLER 0
#define DEBUG_GREETS_DUMP_GRID 0

#include "GREETS.H"
#include "FillerTest.h"
#include "MeshOps.h"
#include "MISC/PREPROC.H"
#include "SceneTick.h"
#include "Scenes.h"
#include "Base/FDS_DECS.H"
#include <memory>
#include <vector>
#include <VESA/Vesa.h>

static float frand() {
	return static_cast <float> (RAND_15()) / static_cast <float> (RAND_15_MAX);
}


static Scene *GreetSc;

// Robot spotlight — created in Initialize_Greets if the robot mesh is
// found, updated each frame in the tick loop after Animate_Objects so
// the spot follows the robot's world position.
static TriMesh *g_RobotMesh = nullptr;
static Omni    *g_RobotSpot = nullptr;

static int32_t CHPartTime = 20.0f*100;

// This function is called to alter parameters within scene objects after loading from FLD.
static void SceneCorrections()
{
	// a crazy hack used to adjust omnilights in code.
	float OmniRangeMult[] = 
	//	ent1  ent2  ctr1  ctr2  crd1  hal1  hal2  rbtl  rbtr  rbth
		{2.0,  2.0,  2.0, 2.0,  2.0,  1.7,  1.7,  2.0,  2.0,  2.0};
//		{0.9,  0.9,  0.3,  0.3,  0.4,  2.0,  2.0,  0.5,  0.5,  0.5};
//		{0.01,  0.01,  0.01,  0.01,  0.01,  1.0,  1.0,  0.01,  0.01,  0.01};
	float OmniSizeMult[] =
	//	ent1  ent2  ctr1  ctr2  crd1  hal1  hal2  rbtl  rbtr  rbth
		{1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  1.5,  1.5,  1.5};
	int OmniDisable[] = 
		{  0,    0,    0,    0,    0,    0,    0,    0,    0,    0};

	int OmniPointFlare[] =
		{  0,    0,    0,    0,    0,    0,    0,    0,    0,    0};

	int OmniStationary[] =
		{  1,	 1,	   1,    1,    1,    1,    1,    0,    0,    0};

	int NumOmnis = sizeof(OmniDisable)/sizeof(int);

	Scene *Sc = GreetSc;
	Omni *O;
	int32_t index;
	for(O=Sc->OmniHead, index=0; O; O=O->Next)
	{
		if (index == NumOmnis) break;

		// disable/enable omnies
		if (OmniDisable[index]) O->Flags &=~Omni_Active;
		if (OmniPointFlare[index]) SetPointFlare(O);
		if (OmniStationary[index]) O->Flags |= Omni_Stationary;

		// adjust size / disable flares
		if (OmniSizeMult[index] == 0)
			O->Flags &=~Omni_Flare;
		Spline_Scale(&O->Size, OmniSizeMult[index]);

		// adjust range
		Spline_Scale(&O->Range, OmniRangeMult[index]);
		index++;
	}

	int ObjStationary[] =
	//    ??? ??? ??? ??? ??? ??? ??? ??? ???
		{	1,	0,	0,	0,	0,	0,	0,	0,	0,};
	int NumObjects = sizeof(ObjStationary)/sizeof(int);

	TriMesh *T;
	for(T=Sc->TriMeshHead, index=0; T; T=T->Next, index++)
	{
		if (ObjStationary[index])
		{
			if (!(T->Flags & Tri_Stationary))
			{
				//T->VIndex
				T->SL = (Color *)getAlignedBlock(sizeof(Color) * T->VIndex, 16);
				T->Flags |= Tri_Stationary;
			}
		}
	}
}

#define TRIG_ACC 512 //trigonometric table accuracy. must be a power of 2.
#define TRIG_MASK (TRIG_ACC-1)
#define TRIG_FACTOR (512.0/PI_M2)
#define ARRAYSIZE(a) (sizeof((a))/sizeof((*a)))

struct GreetsGenerator
{
#define numGridPoints (((256 >> 3) + 1)*((256 >> 3) + 1))

#define TEXRES 256

	Texture CodeTexture;
	Image CodeImage;
	Image OldImage;
	GridPointT *Code_GP;
	GridPointT* Smear_GP;
	float LenTable[numGridPoints];
	float SinTable[TRIG_ACC];
	float CosTable[TRIG_ACC];
	DWord* ScaledBuf;
	DWord *CodeBuf;
	DWord* OldBuf;
	DWord *OutBuf;
	float scalex = 1.0;
	float scaley = 1.0;

	static constexpr const char* greetingsRound1[] = {
		"MRI",
		"imr",
		"Cycat",
		"coolphat",
		"borzom",
		"civax",
		"yoe",
	};

	static constexpr const char* greetingsRound2[] = {
		"Doomsday",
		"esteem",
		"adept",
		"kombat",
		"rage",
		"magic intros",
		"calius"
	};

	static constexpr const char* greetingsRound3[] = {
		" Revival\nBy  Flood"
	};


	struct GreetsEntry {
		GreetsEntry(const char* text): text(text) {
			// OutTextXYExtents signature is (.., out_height, out_width).
			// The original call passed (x, y) — i.e. width-into-x, but
			// that captured *height* into x and *width* into y. The
			// centering math at line 249 then did
			// (CodeImage.x - greet->x)/2 with x and y swapped, leaving
			// every greet visibly off-center. Fix: pass (y, x) so the
			// fields match their names.
			OutTextXYExtents(12, 0, text, 256, 256, y, x);
		}

		const char* text;
		int32_t x, y;  // x = text width, y = text height
	};

	GreetsEntry ge0 { "Flood\nGreets" };
	std::vector<GreetsEntry> ge1{ std::begin(greetingsRound1) , std::end(greetingsRound1) };
	std::vector<GreetsEntry> ge2{ std::begin(greetingsRound2) , std::end(greetingsRound2) };
	std::vector<GreetsEntry> ge3{ std::begin(greetingsRound3) , std::end(greetingsRound3) };
	//GreetsEntry ge_empty { "" };


	void AttachMatToScene()
	{
		Material *Mat;
		for (Mat = MatLib; Mat->Next; Mat = Mat->Next) {
			if (Mat->RelScene == GreetSc && Mat->Txtr && Mat->Txtr->FileName && !strcmp(Mat->Txtr->FileName, "TEXTURES/P_TEXT.JPG"))
			{
				Mat->Txtr->Mipmap[0] = (byte *)OutBuf;
				Mat->Txtr->Flags = Txtr_Nomip;
				Mat->Txtr->numMipmaps = 0;
			}
		}

	}


	GreetsGenerator() {}

	void Init()
	{
		OutBuf = (DWord*)_aligned_malloc(256 * 256 * 4, 16);
		CodeBuf = (DWord *)_aligned_malloc(256 * 256 * 4, 16);
		OldBuf = (DWord*)_aligned_malloc(256 * 256 * 4, 16);
		ScaledBuf = (DWord*)_aligned_malloc(256 * 256 * 4, 16);


		CodeTexture.FileName = strdup("TEXTURES/CODE.JPG");
		Identify_Texture(&CodeTexture);
		if (!CodeTexture.BPP)
		{
			printf("Error Loading texture!\n");
			exit(1);
		}
		Load_Texture(&CodeTexture);
		Convert_Texture2Image(&CodeTexture, &CodeImage);
		memset(CodeImage.Data, 0, sizeof(uint32_t) * CodeImage.x * CodeImage.y);
		OldImage.Data = OldBuf;
		OldImage.x = 256;
		OldImage.y = 256;

		memset(OutBuf, 0, 256 * 256 * 4);

		Code_GP = new GridPointT[numGridPoints];
		Smear_GP = new GridPointT[numGridPoints];

		AttachMatToScene();
		ST = 0.0;
	}
	float ST;
	void Render() {
		float Code_R1, Code_RS, Code_R2, CCosR1, CSinR1, CCosR2, CSinR2;
		static float u, v, u1, v1, u2, v2, r, g, b;
		//float ST = (Timer * 2500) / (1000 + Timer);//  sqrt(Timer*1600);

		double greetTime = 0;
		GreetsEntry* greet;
		const int GREET1_START_FRAME = 350;
		const int GREET1_END_FRAME = 730;
		const int GREET2_START_FRAME = 900;
		const int GREET2_END_FRAME = 1200;
		const int GREET3_START_FRAME = 2000;
		const int GREET3_END_FRAME = 2500;
		scalex = fabs(sin(Timer / 60.0)) * 0.2;
		scaley = fabs(cos(Timer / 47.0)) * 0.2;
		if (CurFrame < GREET1_START_FRAME) {
			greet = &ge0;
		}
		else if (CurFrame < GREET1_END_FRAME) {
			greetTime = double(CurFrame - GREET1_START_FRAME) / (GREET1_END_FRAME - GREET1_START_FRAME);
			greet = &ge1[size_t(greetTime * 0.9999 * ge1.size())];
		}
		else if (CurFrame < GREET2_START_FRAME) {
			greet = &ge0;
		}
		else if (CurFrame < GREET2_END_FRAME) {
			greetTime = double(CurFrame - GREET2_START_FRAME) / (GREET2_END_FRAME - GREET2_START_FRAME);
			greet = &ge2[size_t(greetTime * 0.9999 * ge2.size())];
		}
		else if (CurFrame < GREET3_START_FRAME) {
			greet = &ge0;
		}
		else if (CurFrame < GREET3_END_FRAME) {
			greetTime = double(CurFrame - GREET3_START_FRAME) / (GREET3_END_FRAME - GREET3_START_FRAME);
			greet = &ge3[size_t(greetTime * 0.9999 * ge3.size())];
			scalex = 0.0;
			scaley = 0.0;

		}
		else {
			greet = &ge0;
		}

		memset(CodeImage.Data, 0, sizeof(uint32_t) * CodeImage.x * CodeImage.y);
		OutTextXY((byte*)(CodeImage.Data), (CodeImage.x - greet->x) / 2, (CodeImage.y - greet->y) / 2, greet->text, 255, CodeImage.x, CodeImage.y);

		int X, Y;
		int j = 0;
		ST = (Timer * 2500) / (1000 + Timer);//  sqrt(Timer*1600);

		for (int y = 0; y <= TEXRES; y += 8) {
			for (int x = 0; x <= TEXRES; x += 8)
			{
				u = (x / 256.0f - 0.5) * (0.25 +scalex) + 0.5;
				v = (y / 256.0f - 0.5) * (0.25 +scaley) + 0.5;

				Code_GP[j].u = u * 65536.0;
				Code_GP[j].v = v * 65536.0;
				if (Code_GP[j].u > 65535) Code_GP[j].u = 65535;
				if (Code_GP[j].v > 65535) Code_GP[j].v = 65535;
				if (Code_GP[j].u < 0) Code_GP[j].u = 0;
				if (Code_GP[j].v < 0) Code_GP[j].v = 0;

				j++;
			}
		}

#if DEBUG_GREETS_DUMP_GRID
		// One-shot dump per greet change. Diff between native and wasm to
		// see if the wobbler grid points really differ.
		{
			static GreetsEntry* lastDumpedGreet = nullptr;
			if (greet != lastDumpedGreet) {
				lastDumpedGreet = greet;
				fprintf(stderr, "[GREETS-GRID] greet=\"%s\" scalex=%.9f scaley=%.9f\n",
				        greet->text, scalex, scaley);
				const int gx = (TEXRES >> 3) + 1;
				for (int gy = 0; gy < gx; gy += 8) {
					for (int gxi = 0; gxi < gx; gxi += 8) {
						int idx = gy * gx + gxi;
						fprintf(stderr, "[GREETS-GRID]   GP[%d,%d] u=%d v=%d\n",
						        gxi, gy, Code_GP[idx].u, Code_GP[idx].v);
					}
				}
				fflush(stderr);
			}
		}
#endif

#if DEBUG_GREETS_BYPASS_WOBBLER
		// Bypass the GridRendererT pass — straight copy CodeImage to CodeBuf.
		// If centered text now appears centered in the rendered output, the
		// wobbler is the source of the shift. If still shifted, look at
		// TexutreRemap / AlphaBlend / cube-map projection.
		memcpy(CodeBuf, CodeImage.Data,
		       static_cast<size_t>(CodeImage.x) * CodeImage.y * sizeof(uint32_t));
#else
		GridRendererT(Code_GP, &CodeImage, CodeBuf, TEXRES, TEXRES);
#endif

		j = 0;
		for (int y = 0; y <= TEXRES; y += 8) {
			for (int x = 0; x <= TEXRES; x += 8)
			{
				u = (x / 256.0f - 0.5) * 0.98 + 0.5;
				v = (y / 256.0f - 0.5) * 0.95 + 0.5;

				Smear_GP[j].u = u * 65536.0;
				Smear_GP[j].v = v * 65536.0;
				if (Smear_GP[j].u > 65535) Smear_GP[j].u = 65535;
				if (Smear_GP[j].v > 65535) Smear_GP[j].v = 65535;
				if (Smear_GP[j].u < 0) Smear_GP[j].u = 0;
				if (Smear_GP[j].v < 0) Smear_GP[j].v = 0;

				j++;
			}
		}

		GridRendererT(Smear_GP, &OldImage, ScaledBuf, TEXRES, TEXRES);

		auto cola1 = uint32_t(frand() * 100) + 150;
		auto colb1 = uint32_t(frand() * 100) + 150;
		auto colc1 = uint32_t(frand() * 100) + 150;
		auto cold1 = uint32_t(frand() * 100) + 150;

		auto cola2 = uint32_t(frand() * 100) + 150;
		auto colb2 = uint32_t(frand() * 100) + 150;
		auto colc2 = uint32_t(frand() * 100) + 150;
		auto cold2 = uint32_t(frand() * 100) + 150;


		DWord pSrc = (cola1 << 24) + (colb1 << 16) + (colc1 << 8) + cold1;
		DWord pDst = (cola2 << 24) + (colb2 << 16) + (colc2 << 8) + cold2;
		AlphaBlend((byte*)CodeBuf, (byte *)ScaledBuf, pSrc, pDst, 256 * 256 * 4);
		//AlphaBlend((byte*)ScaledBuf, (byte*)OldBuf, pSrc, pDst, 256 * 256 * 4);
		memcpy(OldBuf, ScaledBuf, 256 * 256 * 4);

		TexutreRemap();
	}

	void TexutreRemap() {
		// right now, there is no support for non-blocked textures,
		// so we need to rearrange TempBuf by 4x4 blocks
		//memcpy(VorSurf, TempBuf, 65536 * 4);
		int X, Y;
		DWord *writePtr = OutBuf;
		for (X = 0; X < 64; X++)
			for (Y = 0; Y < 64; Y++)
			{
				dword *blockPtr = OldBuf + ((X + (Y << 8)) << 2);
				int x, y;
				for (y = 0; y < 4; y++)
					for (x = 0; x < 4; x++)
						*writePtr++ = blockPtr[x + (y << 8)];
			}
	}

	~GreetsGenerator() {
		delete OutBuf;
		delete CodeTexture.Data;
		delete CodeImage.Data;
	}


};

GreetsGenerator *gg;

// Test-harness hook: compute centroids of the major intermediate buffers
// and write them to `out`. Centered greets should put the centroid near
// (128.0, 128.0) on every stage. Used by --bench=greets-stage to diff
// native vs wasm.
extern "C" void Greets_DumpStageCentroids(FILE *out, const char *tag) {
	if (!gg) {
		fprintf(out, "[GREETS-CENTROID %s] gg=null (Initialize_Greets not run)\n", tag);
		return;
	}
	auto centroid = [](const dword *buf, int w, int h,
	                   double &cx, double &cy, double &mass) {
		cx = 0; cy = 0; mass = 0;
		for (int y = 0; y < h; ++y) {
			for (int x = 0; x < w; ++x) {
				dword px = buf[y * w + x];
				int b = px & 0xFF;
				int g = (px >> 8) & 0xFF;
				int r = (px >> 16) & 0xFF;
				double lum = r + g + b;
				cx += x * lum;
				cy += y * lum;
				mass += lum;
			}
		}
		if (mass > 0) { cx /= mass; cy /= mass; }
	};
	double cx, cy, mass;
	centroid((const dword *)gg->CodeImage.Data, 256, 256, cx, cy, mass);
	fprintf(out, "[GREETS-CENTROID %s] CodeImage  cx=%9.4f cy=%9.4f mass=%.1f\n",
	        tag, cx, cy, mass);
	centroid((const dword *)gg->CodeBuf, 256, 256, cx, cy, mass);
	fprintf(out, "[GREETS-CENTROID %s] CodeBuf    cx=%9.4f cy=%9.4f mass=%.1f\n",
	        tag, cx, cy, mass);
	centroid((const dword *)gg->ScaledBuf, 256, 256, cx, cy, mass);
	fprintf(out, "[GREETS-CENTROID %s] ScaledBuf  cx=%9.4f cy=%9.4f mass=%.1f\n",
	        tag, cx, cy, mass);
	centroid((const dword *)gg->OldBuf, 256, 256, cx, cy, mass);
	fprintf(out, "[GREETS-CENTROID %s] OldBuf     cx=%9.4f cy=%9.4f mass=%.1f\n",
	        tag, cx, cy, mass);
	fflush(out);
}

void Initialize_Greets()
{	
	GreetSc = (Scene *)getAlignedBlock(sizeof(Scene), 16);
	memset(GreetSc,0,sizeof(Scene));
	
	LoadFLD(GreetSc,"SCENES/GREETS.FLD");

	gg = new GreetsGenerator;

	SceneCorrections();

	// 5 seconds ending time
	CHPartTime = 500.0 + 100.0f*(GreetSc->EndFrame-GreetSc->StartFrame)/30.0f;


	for(Material *M = MatLib;M;M=M->Next)
	{
		if (M->RelScene != GreetSc) continue;
		if (!M->Txtr) continue;
		M->Flags |= Mat_RGBInterp;
		M->Txtr->Flags |= Txtr_Tiled;
	}


//	printf("FLD-loaded MEM = %d\n",DPMI_Free_Memory());
//	printf("Scene-Proc MEM = %d\n",DPMI_Free_Memory());
	GreetSc->NZP = 0.01f;
	GreetSc->FZP = 150.0f;
//	GreetSc->Flags |= Scn_Fogged;
	Preprocess_Scene(GreetSc, "GREETS");
	// also make the appropriate Layer 2 fillers,

	GreetSc->Ambient.R = 32;
	GreetSc->Ambient.G = 32;
	GreetSc->Ambient.B = 32;

	GreetSc->PathingMinVelocity = 0.01;

	// Auto-detect creased meshes (e.g. wall panels) and switch them to
	// per-face vertex normals; smooth meshes (curved letters) untouched.
	MakeFacesIndependentByAngle(GreetSc, 30.0f);

	// Bump Specular on materials that should plausibly have shiny
	// surfaces — marble stairs, marble pillars, stone floor, the mecha
	// hull/cockpit. Most FLD materials author Specular=0; deferred-path
	// Phong needs a non-zero value to draw a highlight, and Glossiness
	// defaults to 32 in Render_DeferredLighting when the FLD didn't set
	// one. Per-name match keeps the change targeted to surfaces where a
	// highlight is sensible. Greets sets Specular_Factor=0 in tick init,
	// which gates the deferred Phong term off — to enable highlights here
	// the per-scene tone bump that follows raises Specular_Factor too.
	for (Material *M = MatLib; M; M = M->Next) {
		if (M->RelScene != GreetSc || !M->Name) continue;
		const char *n = M->Name;
		if (strstr(n, "stairs") || strstr(n, "amudim") ||
		    strstr(n, "floor")  || strstr(n, "marb")   || strstr(n, "MARB") ||
		    strstr(n, "hull")   || strstr(n, "cockpit")) {
			if (M->Specular < 0.4f) M->Specular = 0.4f;
			if (M->Glossiness == 0) M->Glossiness = 48;
		}
	}

	gg->Init();

	// Experimental robot spotlight. A SpotLight follows the mech/Hull
	// mesh — positioned a few units above and pointing down — so the
	// robot drags a pool of light around as it moves through the scene.
	// Updated each frame in the tick loop after Animate_Objects.
	g_RobotMesh = nullptr;
	for (Object *Obj = GreetSc->ObjectHead; Obj; Obj = Obj->Next) {
		if (Obj->Type != Obj_TriMesh) continue;
		if (!Obj->Name) continue;
		if (strstr(Obj->Name, "Hull") || strstr(Obj->Name, "hull") ||
		    strstr(Obj->Name, "mech")) {
			g_RobotMesh = (TriMesh*)Obj->Data;
			break;
		}
	}
	if (g_RobotMesh) {
		g_RobotSpot = (Omni*)getAlignedBlock(sizeof(Omni), 16);
		std::memset(g_RobotSpot, 0, sizeof(Omni));
		g_RobotSpot->L.B = 255.0f; g_RobotSpot->L.G = 255.0f; g_RobotSpot->L.R = 255.0f;
		g_RobotSpot->L.A = 1.0f;
		g_RobotSpot->ISize  = 60.0f;           // intensity
		g_RobotSpot->IRange = 30.0f;
		g_RobotSpot->rRange = 1.0f / 30.0f;
		g_RobotSpot->IPos = Vector(0.0f, 10.0f, 0.0f);
		g_RobotSpot->IDir = Vector(0.0f, -1.0f, 0.0f);
		g_RobotSpot->Type = Light_SpotLight;
		// Cone: ~25° inner, ~40° outer. Cosines.
		g_RobotSpot->HotSpot = std::cos(25.0f * 3.14159f / 180.0f);
		g_RobotSpot->FallOff = std::cos(40.0f * 3.14159f / 180.0f);
		g_RobotSpot->Flags = Omni_Active;
		// Flare-pass plumbing. Transform_Objects unconditionally adds
		// every Omni's F to FList (treats it as a flare sprite). With
		// F.A/B/C left at the memset zeros, the flare pass at
		// RENDER.CPP:3796 dereferences a null Vertex and crashes. Point
		// A/B/C at the omni's own V so the A==B sprite branch sees a
		// valid (degenerate) face, then install a no-op filler so no
		// flare sprite actually gets drawn.
		g_RobotSpot->F.A = &g_RobotSpot->V;
		g_RobotSpot->F.B = &g_RobotSpot->V;
		g_RobotSpot->F.C = &g_RobotSpot->V;
		g_RobotSpot->F.Filler = [](Face*, Vertex**, dword, dword) {};
		// Single-key splines so Animate_Objects doesn't crash when it
		// walks them. The keys evaluate to constant zeros — our per-
		// frame update in the tick loop overrides IPos / IDir after
		// Animate_Objects runs, so the spline-driven values get
		// stomped immediately.
		auto initSingleKey = [](Spline& sp, float val) {
			sp.NumKeys = 1;
			sp.Keys = new SplineKey;
			std::memset(sp.Keys, 0, sizeof(SplineKey));
			sp.Keys[0].Pos.x = val;
			sp.Keys[0].Pos.y = val;
			sp.Keys[0].Pos.z = val;
			sp.Flags = 0;
			sp.CurKey = 0;
		};
		initSingleKey(g_RobotSpot->Pos, 0.0f);
		initSingleKey(g_RobotSpot->Size, g_RobotSpot->ISize);
		initSingleKey(g_RobotSpot->Range, g_RobotSpot->IRange);
		// Link into the scene's omni chain.
		g_RobotSpot->Next = GreetSc->OmniHead;
		if (GreetSc->OmniHead) GreetSc->OmniHead->Prev = g_RobotSpot;
		GreetSc->OmniHead = g_RobotSpot;
	}
}

//void MMXClear(void *Page, dword Len)
//{
//	dword Addr = (dword)Page;
//	__asm 
//	{
//		mov ecx, dword ptr [Len]
//		mov edi, dword ptr [Addr]
//		pxor MM0, MM0
//		xor eax, eax
//
//;		shr ecx, 2
//;		rep stosd
//		add edi, ecx
//		neg ecx
//		Inner:
//			movq [edi+ecx], MM0
//;			movq MM0, [edi+ecx]
//			add ecx, 8
//
//;			mov [edi+ecx], eax
//;			mov eax, [edi+ecx]
//;			add ecx, 4
//		jnz Inner
//		
//		emms
//	}
//}

namespace {
struct GreetsScene : SceneDriver {
	char MSGStr[128];
	std::unique_ptr<Face*[]> fListStorage;
	std::unique_ptr<Face*[]> sListStorage;
	int32_t TTrd = 0;
	int32_t timerStack[20] = {};
	int32_t timerIndex = 0;
	dword numFrames = 0;
	FrameProfiler prof{"greets"};
	bool pause_mode = false;

	~GreetsScene() override { Destroy_Scene(GreetSc); }

	void init() override {
		int32_t Polys = 0;
		TriMesh *T;
		Omni *O;

		for(int32_t i = 0; i < 20; i++)
			timerStack[i] = Timer;

		SetCurrentScene(GreetSc);
		for(Object *Obj = GreetSc->ObjectHead; Obj; Obj = Obj->Next) {
			if (Obj->Type != Obj_TriMesh) continue;
			T = (TriMesh *)Obj->Data;
			Polys += T->FIndex;
		}

		for(O = GreetSc->OmniHead; O; O = O->Next)
			Polys++;

		fListStorage = std::make_unique<Face*[]>(Polys);
		sListStorage = std::make_unique<Face*[]>(Polys);
		FList = fListStorage.get();
		SList = sListStorage.get();

		View = GreetSc->CameraHead;

		Ambient_Factor = 0.25;
		Diffusive_Factor = 1.0;
		// Forward Lighting() ignores Specular_Factor on its active path, so
		// raising it from the legacy 0.0 to 1.0 is a no-op for forward.
		// Deferred uses it as the per-scene specular gate, so this enables
		// the Mat->Specular bump above to draw highlights on marble/hull.
		Specular_Factor = 1.0;
		Range_Factor = 1.0;
		ImageSize = 0.25;

		// this scene messes up at time zero
		Timer++;

		TTrd = Timer;
		g_renderedPolys = 0;
		FillerPixelcount = 0;
	}

	bool tick() override {
		// ESC = exit demo (handled by runSceneBlocking); Backspace = skip scene.
		if (Keyboard[ScBackSpace] || Timer >= CHPartTime) return false;

		numFrames++;

		prof.beginFrame();
		// PROFILER: ZClear phase. also includes control keys handling
		prof.enter(PROF_ZCLR);

		if (Keyboard[ScTab]) {
			if (View == &FC)
				View = GreetSc->CameraHead;
			else
				View = &FC;
		}

		dTime = Timer - TTrd;
		// fast forward/rewind
		if (Keyboard[ScF2]) {
			Timer += dTime * 8;
		}
		if (Keyboard[ScF1]) {
			if (dTime * 8 > Timer)
				Timer = 0;
			else
				Timer -= dTime * 8;
		}
		if (Keyboard[ScP])
			pause_mode = true;
		if (Keyboard[ScU])
			pause_mode = false;
		if (pause_mode) {
			Timer = TTrd;
		}
		g_FrameTime = TTrd = Timer;

		// Clear Framebuffer and ZBuffer
		parallel_memset(VPage, 0, PageSize); parallel_memset(ZPage16, 0, XRes * YRes * sizeof(word));

		// PROFILER: ANIM phase. also includes Dynamic Camera manager
		prof.switchTo(PROF_ANIM);

		if (Timer < CHPartTime-500.0)
			CurFrame = GreetSc->StartFrame + (GreetSc->EndFrame-GreetSc->StartFrame) * (float)g_FrameTime / (float)(CHPartTime-500.0);
		else
			CurFrame = GreetSc->EndFrame;

		Dynamic_Camera();
		if (Keyboard[ScC]) {FC.ISource = View->ISource; Matrix_Copy(FC.Mat,View->Mat); FC.IFOV=View->IFOV;}

		Animate_Objects(GreetSc);

		// Robot spotlight follow. Position the spot ~10 units above the
		// robot, then point it 45° forward — so the cone hits the floor
		// AHEAD of the robot rather than straight underneath. Forward
		// direction comes from the robot's RotMat (column 2 = local +Z
		// transformed into world), with the light cone axis tilted at
		// 45° between -Y and that forward vector:
		//   dir = sin(45°) * worldFwd + cos(45°) * (-Y)
		if (g_RobotSpot && g_RobotMesh) {
			g_RobotSpot->IPos.x = g_RobotMesh->IPos.x;
			g_RobotSpot->IPos.y = g_RobotMesh->IPos.y + 10.0f;
			g_RobotSpot->IPos.z = g_RobotMesh->IPos.z;
			Vector localFwd(0.0f, 0.0f, 1.0f);
			Vector worldFwd;
			MatrixXVector(g_RobotMesh->RotMat, &localFwd, &worldFwd);
			worldFwd.y = 0.0f;  // project onto XZ plane so the 45° tilt math is well-defined
			Vector_Norm(&worldFwd);
			// Side-to-side sweep — rotate worldFwd around the world-up
			// axis by a sine-modulated angle. Period and amplitude are
			// hand-tuned for a "scanning" feel; Timer is in millisecond
			// ticks so 0.003 rad/tick = ~3 s full cycle, ±35° sweep.
			const float sweepRad = std::sin(float(Timer) * 0.003f) * (35.0f * float(PI) / 180.0f);
			const float cs = std::cos(sweepRad);
			const float sn = std::sin(sweepRad);
			const float fwdX = cs * worldFwd.x + sn * worldFwd.z;
			const float fwdZ = -sn * worldFwd.x + cs * worldFwd.z;
			worldFwd.x = fwdX;
			worldFwd.z = fwdZ;
			constexpr float k = 0.70710678f;  // sin(45°) = cos(45°)
			g_RobotSpot->IDir.x = k * worldFwd.x;
			g_RobotSpot->IDir.y = -k;
			g_RobotSpot->IDir.z = k * worldFwd.z;
			Vector_Norm(&g_RobotSpot->IDir);
		}

		// PROFILER: XFRM phase.
		prof.switchTo(PROF_XFRM);

		Transform_Objects(GreetSc);

		// PROFILER: LGHT phase.
		prof.switchTo(PROF_LGHT);

		Lighting(GreetSc);

		// PROFILER: SORT phase.
		prof.leave(PROF_LGHT);
		if (!CAll) return true;
		prof.enter(PROF_SORT);

		Radix_SortingASM(FList, SList, CAll);

		// PROFILER: RNDR phase.
		prof.switchTo(PROF_RNDR);

		gg->Render();
		// reset per-frame zbuffer statistics
		zPass = zReject = 0;
		Render();

		// PROFILER: FLIP phase. also contains display of runtime stats
		prof.leave(PROF_RNDR);
		if (g_profilerActive) {
			// FPS printer — sourced from FrameProfiler so it agrees with TOTL.
			float meanMs = prof.meanFrameMs();
			float fps = meanMs > 0 ? 1000.0f / meanMs : 0.0f;
			snprintf(MSGStr, sizeof(MSGStr), "%f FPS", fps);
			int scroll = 0;
			scroll = OutTextXY(VPage, 0, 0, MSGStr, 255);
			snprintf(MSGStr, sizeof(MSGStr), "Frame %f", CurFrame);
			scroll = OutTextXY(VPage, 0, scroll + 15 * g_fontScale, MSGStr, 255);

			double rndrMs = prof.cumulativeMs(PROF_RNDR);
			double mPxPerFrame = FillerPixelcount / (1e6 * numFrames);
			double mPxPerSec = rndrMs > 0 ? FillerPixelcount / 1e3 / rndrMs : 0.0;
			double polysPerFrame = (double)g_renderedPolys / numFrames;
			scroll = prof.drawOverlay(scroll, polysPerFrame, mPxPerFrame, mPxPerSec);
		}

		prof.enter(PROF_FLIP);

		Flip(MainSurf);
		// PROFILER: frame finished
		prof.leave(PROF_FLIP);
		prof.endFrame();
		return true;
	}

	void cleanup() override {
		if (g_profilerActive) prof.dump();
		Timer -= CHPartTime;
		while (Keyboard[ScBackSpace] && !g_shouldQuit.load()) continue;
		if (Timer < 0) Timer = 0;
	}
};
} // anonymous namespace

std::unique_ptr<SceneDriver> createGreetsScene()
{
	return std::make_unique<GreetsScene>();
}

void Run_Greets()
{
	auto scene = createGreetsScene();
	runSceneBlocking(*scene);
}