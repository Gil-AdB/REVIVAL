#include "Rev.h"
#include <Threads.h>
#include <Base/FeatureFlags.h>
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
#include "FILLERS/Mekalele.h"
#include <map>
#include "Base/FDS_DECS.H"
#include "FILLERS/ShadowMap.h"
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
// Four colored orbit spots circling the robot at 90° offsets. Same cone
// shape as g_RobotSpot but oriented to point inward/down at the robot.
static constexpr int kOrbitSpotCount = 4;
static Omni    *g_OrbitSpots[kOrbitSpotCount] = { nullptr, nullptr, nullptr, nullptr };

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

// Animate_Objects walks every Omni's Pos/Size/Range splines and crashes
// if NumKeys is 0; the robot spots aren't authored in the FLD, so we
// install a single zero-valued key. The per-frame tick overrides
// IPos/IDir directly after Animate_Objects runs, so the spline values
// get stomped immediately.
static void initSpotlightSingleKeySplines(Omni* o)
{
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
	initSingleKey(o->Pos,   0.0f);
	initSingleKey(o->Size,  o->ISize);
	initSingleKey(o->Range, o->IRange);
}

// Builds a fully-initialised spot omni, prepended to the scene's omni
// chain. Encapsulates all the shared init the robot's main + orbit
// spots need:
//   - colour, intensity, range, position, direction
//   - cone angles (HotSpot/FallOff stored as cosines of inner/outer degrees)
//   - active + shadow-casting flags
//   - flare-pass plumbing (F.A/B/C wired to self.V + no-op filler, otherwise
//     Transform_Objects's flare pass would dereference null Vertex pointers)
//   - single-key splines so Animate_Objects doesn't crash
//   - optional per-light shadow-map resolution override
static Omni* makeSpotLight(Scene* sc,
                            float R, float G, float B,
                            float intensity, float range,
                            const Vector& pos, const Vector& dir,
                            float hotInnerDeg, float fallOuterDeg,
                            uint16_t shadowMapRes = 0)
{
	Omni* o = (Omni*)getAlignedBlock(sizeof(Omni), 16);
	std::memset(o, 0, sizeof(Omni));
	o->L.R = R; o->L.G = G; o->L.B = B; o->L.A = 1.0f;
	o->ISize  = intensity;
	o->IRange = range;
	o->rRange = 1.0f / range;
	o->IPos   = pos;
	o->IDir   = dir;
	o->Type   = Light_SpotLight;
	o->HotSpot = std::cos(hotInnerDeg  * 3.14159f / 180.0f);
	o->FallOff = std::cos(fallOuterDeg * 3.14159f / 180.0f);
	o->Flags   = Omni_Active | Omni_CastsShadow;
	if (shadowMapRes) o->shadowMapRes = shadowMapRes;

	o->F.A = &o->V;
	o->F.B = &o->V;
	o->F.C = &o->V;
	o->F.Filler = [](Face*, Vertex**, dword, dword,
	                 const fds::RenderTarget&,
	                 const fds::CameraContext&) {};

	initSpotlightSingleKeySplines(o);

	// Prepend into the scene's doubly-linked omni chain.
	o->Next = sc->OmniHead;
	if (sc->OmniHead) sc->OmniHead->Prev = o;
	sc->OmniHead = o;
	return o;
}

// Main robot spot + three orbit spots (red/green/blue at 120° phase).
// Per-frame tick overrides positions; this just lands the lights in
// the scene chain so Lighting / shadow passes pick them up.
static void initializeRobotSpotlights()
{
	g_RobotSpot = makeSpotLight(
		GreetSc,
		/*R*/255, /*G*/255, /*B*/255,
		/*intensity*/10.0f, /*range*/30.0f,
		/*pos*/Vector(0.0f, 10.0f, 0.0f),
		/*dir*/Vector(0.0f, -1.0f, 0.0f),
		// ~10° inner (narrow hotspot) / ~55° outer (wide falloff): a wide
		// pool of floor with a small bright core, falloff dominates.
		/*hotDeg*/10.0f, /*fallDeg*/55.0f);

	// Orbit spots: narrower cone for distinct colored patches; short-range
	// shadow maps at 256x256 (16x cheaper raster than the main 1024²).
	struct OrbitInit { float r, g, b; const char* tag; };
	const OrbitInit init[kOrbitSpotCount] = {
		{ 255.0f,  60.0f,  60.0f, "red"    },
		{  60.0f, 255.0f,  60.0f, "green"  },
		{  60.0f,  80.0f, 255.0f, "blue"   },
		{ 255.0f, 220.0f,  80.0f, "yellow" },
	};
	for (int i = 0; i < kOrbitSpotCount; ++i) {
		g_OrbitSpots[i] = makeSpotLight(
			GreetSc,
			init[i].r, init[i].g, init[i].b,
			/*intensity*/8.0f, /*range*/30.0f,
			/*pos*/Vector(0.0f, 0.0f, 0.0f),
			/*dir*/Vector(0.0f, -1.0f, 0.0f),
			/*hotDeg*/15.0f, /*fallDeg*/35.0f,
			/*shadowMapRes*/256);
		std::fprintf(stderr, "[GREETS] orbit spot %d (%s) installed\n",
			i, init[i].tag);
	}
}

void Initialize_Greets()
{
	GreetSc = loadSceneAligned("SCENES/GREETS.FLD");

	gg = new GreetsGenerator;

	SceneCorrections();

	// 5 seconds ending time
	CHPartTime = 500.0 + 100.0f*(GreetSc->EndFrame-GreetSc->StartFrame)/30.0f;

	tagSceneMaterials(GreetSc, /*restrictToScene=*/true);


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
		const bool isShiny = strstr(n, "stairs") || strstr(n, "amudim") ||
		                     strstr(n, "floor")  || strstr(n, "marb")   ||
		                     strstr(n, "MARB")   || strstr(n, "hull")   ||
		                     strstr(n, "cockpit");
		// Wall / ceiling surfaces — the FLD names them 'rooms' (the
		// big brick wall texture in the back of the greets scene) and
		// 'siling' (the ceiling that uses the same brick atlas). Not
		// "shiny" — we don't bump Specular here — but they show enough
		// surface detail in the diffuse to benefit from a Sobel-baked
		// tangent-space map.
		const bool isWall = !strcmp(n, "rooms") || !strcmp(n, "siling");
		if (isShiny) {
			if (M->Specular < 0.4f) M->Specular = 0.4f;
			if (M->Glossiness == 0) M->Glossiness = 48;
		}
		// Tangent-space normal maps work on any face orientation —
		// the perturbation lives in the surface's own tangent plane
		// and the lighting kernel multiplies by per-pixel TBN to bring
		// it into view space. Apply to every shiny material + the
		// wall/ceiling materials.
		if ((isShiny || isWall) && M->Txtr && !M->NormalMap) {
			M->NormalMap = BakeNormalMapFromDiffuse(M->Txtr, 4.0f);
			if (M->NormalMap) {
				std::fprintf(stderr, "[GREETS] baked tangent-space normal map for '%s'\n", n);
			}
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
		    strstr(Obj->Name, "mech") || strstr(Obj->Name, "Mech") ||
		    strstr(Obj->Name, "MECH")) {
			g_RobotMesh = (TriMesh*)Obj->Data;
			std::fprintf(stderr, "[GREETS] robot spotlight target = '%s'\n", Obj->Name);
			break;
		}
	}

	// Per-sub-mesh material cloning for the robot. Each robot TriMesh
	// (Hull, Hull2, L_leg1/2, R_leg1/2) gets its own private clones of
	// every material it uses, so each sub-mesh ends up with distinct
	// matIDs. Hull.lwo additionally splits each material into K bins
	// along the local Y axis (upper / body for K=2) so the cockpit area
	// can self-shadow the body within the same TriMesh — PolyId needs
	// distinct matIDs for the comparison to register a shadow.
	auto isRobotSubMesh = [](const char *n) {
		return n && (strstr(n, "Hull") || strstr(n, "hull") ||
		             strstr(n, "leg")  || strstr(n, "Leg") ||
		             strstr(n, "LEG"));
	};
	int totalClones = 0;
	for (Object *Obj = GreetSc->ObjectHead; Obj; Obj = Obj->Next) {
		if (Obj->Type != Obj_TriMesh || !Obj->Name) continue;
		if (!isRobotSubMesh(Obj->Name)) continue;
		TriMesh *T = (TriMesh*)Obj->Data;

		// Hull.lwo splits into 2 Y bins; smaller meshes (Hull2, legs)
		// use 1 bin (just per-mesh isolation).
		const int kBins = strstr(Obj->Name, "Hull.lwo") ? 2 : 1;

		// Local-Y range of face centroids (mesh-local; animation moves
		// the whole mesh but face centroids' local Y stays constant).
		float ymin =  1e30f, ymax = -1e30f;
		if (kBins > 1) {
			for (Face *F = T->Faces; F < T->Faces + T->FIndex; ++F) {
				const float fy = (F->A->Pos.y + F->B->Pos.y + F->C->Pos.y) / 3.0f;
				if (fy < ymin) ymin = fy;
				if (fy > ymax) ymax = fy;
			}
		}
		const float invSpan = (ymax > ymin) ? float(kBins) / (ymax - ymin) : 0.0f;

		// Key: (source material, Y bin). Same source + same bin → reuse
		// clone.
		std::map<std::pair<Material*, int>, Material*> clones;
		for (Face *F = T->Faces; F < T->Faces + T->FIndex; ++F) {
			Material *src = F->Txtr;
			if (!src) continue;
			int bin = 0;
			if (kBins > 1) {
				const float fy = (F->A->Pos.y + F->B->Pos.y + F->C->Pos.y) / 3.0f;
				bin = int((fy - ymin) * invSpan);
				if (bin < 0)        bin = 0;
				if (bin >= kBins)   bin = kBins - 1;
			}
			auto key = std::make_pair(src, bin);
			auto it = clones.find(key);
			if (it == clones.end()) {
				Material *clone = (Material*)getAlignedBlock(sizeof(Material), 16);
				std::memcpy(clone, src, sizeof(Material));
				char *nameBuf = new char[96];
				if (kBins > 1) {
					const char *binName = (bin == 0) ? "body" : "upper";
					std::snprintf(nameBuf, 96, "%s::%s_%s", Obj->Name,
						src->Name ? src->Name : "?", binName);
				} else {
					std::snprintf(nameBuf, 96, "%s::%s", Obj->Name,
						src->Name ? src->Name : "?");
				}
				clone->Name = nameBuf;
				Material *tail = MatLib;
				while (tail->Next) tail = tail->Next;
				clone->Next = nullptr;
				tail->Next = clone;
				it = clones.emplace(key, clone).first;
				++totalClones;
			}
			F->Txtr = it->second;
		}
		std::fprintf(stderr, "[ROBOT-MAT] mesh '%s' bins=%d cloned %zu materials\n",
			Obj->Name, kBins, clones.size());
	}
	Scene_RebuildMatTable(GreetSc);
	std::fprintf(stderr, "[ROBOT-MAT] total clones: %d\n", totalClones);
	std::fflush(stderr);
	if (!g_RobotMesh) {
		std::fprintf(stderr, "[GREETS] robot spotlight target NOT FOUND. ObjectHead names:\n");
		for (Object *Obj = GreetSc->ObjectHead; Obj; Obj = Obj->Next) {
			if (Obj->Name) std::fprintf(stderr, "  %s\n", Obj->Name);
		}
	}
	if (g_RobotMesh && !fds::FeatureFlags::no_greets_spots()) {
		initializeRobotSpotlights();
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
	int32_t TTrd = 0;
	int32_t timerStack[20] = {};
	int32_t timerIndex = 0;
	dword numFrames = 0;
	FrameProfiler prof{"greets"};
	bool pause_mode = false;

	~GreetsScene() override { Destroy_Scene(GreetSc); }

	void init() override {
		// Use the global ::Polys (alias for fds::g_mainFaces.polys) — was
		// previously a local that shadowed the global, leaving ::Polys at
		// its previous value. Read downstream by Shadows.cpp when sizing
		// per-light face buffers — when greets runs without a prior scene
		// having populated the global, ::Polys stays 0 → shadow per-light
		// resize(0) → null fList → SEGV at Transform.cpp:1041.
		Polys = 0;
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

		fds::g_mainFaces.resize(Polys);

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

		// Shadow-map storage for every Omni_CastsShadow light in this
		// scene. Spotlights (robot + orbits) are flagged at install in
		// makeSpotLight. With --greets-omni-shadows, also flag every
		// FLD-loaded Light_Omni so CubeShadowMaps_Rebuild bakes a 6-face
		// cube per omni.
		// FLD-loaded omnis in greets all ship with IRange=0 (the FLD
		// Range envelope is optional; FLD_CONV.CPP defaults it to a
		// 1-key spline at 0). That kills surface lighting (range2=0 →
		// rejects every pixel), halo bounds, and shadow FZP. Patch to
		// the flag-controlled default so they actually contribute.
		const float omniDefRange = fds::FeatureFlags::greets_omni_default_range();
		if (omniDefRange > 0.0f) {
			int patched = 0;
			for (Omni *O = GreetSc->OmniHead; O; O = O->Next) {
				if (O->Type != Light_Omni) continue;
				if (O->IRange > 0.0f) continue;
				O->IRange = omniDefRange;
				O->rRange = 1.0f / omniDefRange;
				if (O->Range.NumKeys >= 1 && O->Range.Keys) {
					O->Range.Keys[0].Pos.x = omniDefRange;
				}
				++patched;
			}
			if (patched > 0) {
				std::fprintf(stderr,
				    "[GREETS] patched IRange=%g on %d FLD omnis (had 0)\n",
				    omniDefRange, patched);
			}
		}

		const int omniCubeRes = std::max(16,
		    fds::FeatureFlags::greets_omni_shadow_res());
		int totalOmnis = 0;
		if (fds::FeatureFlags::greets_omni_shadows()) {
			int marked = 0, skippedDynamic = 0;
			for (Omni *O = GreetSc->OmniHead; O; O = O->Next) {
				++totalOmnis;
				if (O->Type != Light_Omni) continue;
				if (O->Flags & Omni_CastsShadow) continue;
				// Only mark *static* omnis — the bake fires once per
				// scene, so an animated Pos spline would freeze the
				// shadow at the t=0 position and look wrong as the
				// light moves. NumKeys > 1 = animated.
				if (O->Pos.NumKeys > 1) { ++skippedDynamic; continue; }
				O->Flags |= (Omni_CastsShadow | Omni_StaticShadow);
				O->shadowMapRes = dword(omniCubeRes);
				++marked;
			}
			std::fprintf(stderr,
			    "[GREETS] greets_omni_shadows: marked %d / %d Light_Omni "
			    "(skipped %d animated) for cube shadow bake at %dx%d per face\n",
			    marked, totalOmnis, skippedDynamic, omniCubeRes, omniCubeRes);
		}
		ShadowMaps_Rebuild(GreetSc, 1024);
		CubeShadowMaps_Rebuild(GreetSc, omniCubeRes);
		ShadowMaps_BakeStatic(GreetSc);

		// this scene messes up at time zero
		Timer++;

		TTrd = Timer;
		g_renderedPolys = 0;
		FillerPixelcount = 0;
	}

	static void draw_cones(bool sShadowMapView) {
		// Spotlight cone volumetric glow. For each shadow-casting spot,
		// build a 16-segment fan cone with apex at the light and rim at
		// the cone's outer edge. Each fan triangle is 3D-clipped against
		// the near plane (z=NZP) so that when the camera is inside the
		// cone — apex behind camera — the triangle becomes a clipped
		// polygon with new verts on the near plane instead of being
		// discarded. Then project + rasterize each (sub-)triangle with
		// additive blend and vertex-alpha falloff (apex bright → rim
		// transparent). Z-tests against ZPage16 so walls correctly
		// occlude the glow.
		//
		// Two-sided by construction: bary test fills the screen-space
		// triangle regardless of winding (signs of area and w_i match),
		// so the cone surface is visible from both inside and outside.
		if (!sShadowMapView) {
			struct ConeVtx { float vx, vy, vz; float alpha; };
			const float kNearZ = std::max(0.01f, GreetSc->NZP);
			auto worldToView = [&](const Vector &world) -> Vector {
				const Vector d = {
					world.x - View->ISource.x,
					world.y - View->ISource.y,
					world.z - View->ISource.z };
				Vector v;
				MatrixXVector(View->Mat, &d, &v);
				return v;
			};
			// Project to float screen coords. Near-plane-clipped verts
			// have vz = kNearZ (~0.01) so projected x/y can reach ~1e6.
			// That's fine in float — the bbox is clamped to the viewport
			// below, and the bary math stays in float throughout. Do NOT
			// clamp the projected coords themselves: clamping creates
			// fake degenerate triangles (collapsing multiple off-screen
			// verts to the same corner, or producing a screen-corner
			// vertex that yields a viewport-sized "phantom" triangle).
			auto projectViewVtx = [&](float vx, float vy, float vz,
			                          float &outX, float &outY) {
				const float invZ = 1.0f / vz;
				outX = CntrX + FOVX * vx * invZ;
				outY = CntrY - FOVY * vy * invZ;
			};
			// Sutherland-Hodgman near-plane clip of a triangle. Up to 4
			// output verts (when one of the input verts is behind, the
			// triangle becomes a quad).
			auto clipTriNear = [&](const ConeVtx in[3], ConeVtx out[4]) -> int {
				int outCount = 0;
				ConeVtx prev = in[2];
				bool prevIn = prev.vz > kNearZ;
				for (int i = 0; i < 3; ++i) {
					const ConeVtx &cur = in[i];
					const bool curIn = cur.vz > kNearZ;
					if (prevIn != curIn) {
						const float t = (kNearZ - prev.vz) / (cur.vz - prev.vz);
						out[outCount].vx    = prev.vx    + t * (cur.vx    - prev.vx);
						out[outCount].vy    = prev.vy    + t * (cur.vy    - prev.vy);
						out[outCount].vz    = kNearZ;
						out[outCount].alpha = prev.alpha + t * (cur.alpha - prev.alpha);
						++outCount;
					}
					if (curIn) out[outCount++] = cur;
					prev = cur;
					prevIn = curIn;
				}
				return outCount;
			};
			// Additive fan triangle with perspective-correct Z-test +
			// vertex-alpha falloff. View-z is interpolated as 1/z (which
			// IS linear in screen-space under perspective), then
			// inverted at the pixel to recover view-z and encode it for
			// the ZPage16 compare. Linear-in-z bary lerp would be wrong
			// for varying-z triangles and produces flickering occlusion
			// as the camera moves.
			auto addTri = [&](float x0, float y0, float z0, float a0,
			                  float x1, float y1, float z1, float a1,
			                  float x2, float y2, float z2, float a2,
			                  uint8_t cr, uint8_t cg, uint8_t cb) {
				if (z0 <= 0.0f || z1 <= 0.0f || z2 <= 0.0f) return;
				const float area = (x1-x0)*(y2-y0) - (x2-x0)*(y1-y0);
				if (std::fabs(area) < 1.0f) return;
				const float invArea = 1.0f / area;
				const float rz0 = 1.0f / z0;
				const float rz1 = 1.0f / z1;
				const float rz2 = 1.0f / z2;
				float fxmin = std::min({x0,x1,x2});
				float fxmax = std::max({x0,x1,x2});
				float fymin = std::min({y0,y1,y2});
				float fymax = std::max({y0,y1,y2});
				if (fxmin < 0.0f)         fxmin = 0.0f;
				if (fymin < 0.0f)         fymin = 0.0f;
				if (fxmax > float(XRes-1)) fxmax = float(XRes-1);
				if (fymax > float(YRes-1)) fymax = float(YRes-1);
				const int xmin = int(fxmin);
				const int xmax = int(fxmax);
				const int ymin = int(fymin);
				const int ymax = int(fymax);
				if (xmin > xmax || ymin > ymax) return;
				dword *fb = (dword*)VPage;
				const word *zb = ZPage16;
				for (int y = ymin; y <= ymax; ++y) {
					const float fy = float(y);
					for (int x = xmin; x <= xmax; ++x) {
						const float fx = float(x);
						const float w0 = ((x1-fx)*(y2-fy) - (x2-fx)*(y1-fy)) * invArea;
						const float w1 = ((x2-fx)*(y0-fy) - (x0-fx)*(y2-fy)) * invArea;
						const float w2 = 1.0f - w0 - w1;
						// Two-sided: when winding flips, area and W_i flip
						// sign together so w_i = W_i/area stays positive
						// when the point is inside the triangle.
						if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
						// Perspective-correct view-z: lerp 1/z linearly,
						// then invert. Z-test against scene depth so
						// walls occlude.
						const float prz = w0*rz0 + w1*rz1 + w2*rz2;
						if (prz <= 0.0f) continue;
						const float pz = 1.0f / prz;
						int penc = 0xFF80 - int(pz * g_zscale);
						if (penc < 0) penc = 0;
						if (penc > 0xFFFF) penc = 0xFFFF;
						const size_t i = size_t(y) * size_t(XRes) + size_t(x);
						const int zexist = int(zb[i]);
						if (penc < zexist) continue;  // cone is behind opaque
						// Vertex-alpha falloff. apex (a=1) → rim (a=0).
						float alpha = w0*a0 + w1*a1 + w2*a2;
						if (alpha <= 0.0f) continue;
						// Soft falloff curve so the rim is much darker
						// than the apex (linear bary alpha looks flat).
						alpha = alpha * alpha;
						const int addR = int(float(cr) * alpha);
						const int addG = int(float(cg) * alpha);
						const int addB = int(float(cb) * alpha);
						const dword pix = fb[i];
						int oldR = int((pix >> 16) & 0xFF);
						int oldG = int((pix >>  8) & 0xFF);
						int oldB = int( pix        & 0xFF);
						int newR = oldR + addR; if (newR > 255) newR = 255;
						int newG = oldG + addG; if (newG > 255) newG = 255;
						int newB = oldB + addB; if (newB > 255) newB = 255;
						fb[i] = (dword(newR) << 16) | (dword(newG) << 8)
						        |  dword(newB)        | 0xFF000000u;
					}
				}
			};
			constexpr int N_SEGMENTS = 16;
			for (Omni *o = GreetSc->OmniHead; o; o = o->Next) {
				if (o->Type != Light_SpotLight) continue;
				if (!(o->Flags & Omni_Active)) continue;
				const float cosOuter = std::max(0.01f, o->FallOff);
				const float outerAng = std::acos(cosOuter);
				Vector axis = o->IDir;
				Vector_Norm(&axis);
				Vector ref = (std::fabs(axis.y) < 0.9f)
					             ? Vector(0.0f, 1.0f, 0.0f)
					             : Vector(1.0f, 0.0f, 0.0f);
				Vector right;
				Cross_Product(&axis, &ref, &right);
				Vector_Norm(&right);
				Vector up;
				Cross_Product(&right, &axis, &up);
				Vector_Norm(&up);
				const float L = o->IRange;
				const float rimAxis = L * std::cos(outerAng);
				const float rimRad  = L * std::sin(outerAng);

				// Transform apex + rim verts to view space ONCE, keep
				// the original (vx, vy, vz) so we can 3D-clip each fan
				// triangle against the near plane before projecting.
				const Vector apexV = worldToView(o->IPos);
				ConeVtx rimV[N_SEGMENTS + 1];
				for (int s = 0; s <= N_SEGMENTS; ++s) {
					const float theta = float(s) * (2.0f * float(PI) / float(N_SEGMENTS));
					const float cs = std::cos(theta), sn = std::sin(theta);
					const Vector rim = {
						o->IPos.x + axis.x * rimAxis + (right.x * cs + up.x * sn) * rimRad,
						o->IPos.y + axis.y * rimAxis + (right.y * cs + up.y * sn) * rimRad,
						o->IPos.z + axis.z * rimAxis + (right.z * cs + up.z * sn) * rimRad,
					};
					const Vector rimVw = worldToView(rim);
					rimV[s].vx = rimVw.x;
					rimV[s].vy = rimVw.y;
					rimV[s].vz = rimVw.z;
					rimV[s].alpha = 0.0f;
				}

				// Scale the per-spot glow intensity. Additive blending
				// blows out white-ish cones much faster than saturated
				// colors (R+G+B all sum into the framebuffer), so we
				// dim achromatic cones by their chroma — pure white
				// drops to ~30% of color-channel intensity, fully
				// saturated colors stay at full strength.
				const float maxC = std::max({o->L.R, o->L.G, o->L.B});
				const float minC = std::min({o->L.R, o->L.G, o->L.B});
				const float chroma = (maxC > 1.0f) ? (maxC - minC) / maxC : 0.0f;
				const float whitenessScale = 0.3f + 0.7f * chroma;
				const float intensity = std::min(1.0f, o->ISize * 0.1f) * whitenessScale;
				const uint8_t cr = uint8_t(std::min(255.0f, o->L.R * intensity));
				const uint8_t cg = uint8_t(std::min(255.0f, o->L.G * intensity));
				const uint8_t cb = uint8_t(std::min(255.0f, o->L.B * intensity));

				const ConeVtx apexCV = { apexV.x, apexV.y, apexV.z, 1.0f };
				for (int s = 0; s < N_SEGMENTS; ++s) {
					const ConeVtx tri[3] = { apexCV, rimV[s], rimV[s + 1] };
					ConeVtx clipped[4];
					const int n = clipTriNear(tri, clipped);
					if (n < 3) continue;
					float sx[4], sy[4];
					for (int k = 0; k < n; ++k) {
						projectViewVtx(clipped[k].vx, clipped[k].vy, clipped[k].vz,
						               sx[k], sy[k]);
					}
					// Fan triangulate the clipped polygon (3 or 4 verts).
					for (int k = 2; k < n; ++k) {
						addTri(sx[0],   sy[0],   clipped[0].vz,   clipped[0].alpha,
						       sx[k-1], sy[k-1], clipped[k-1].vz, clipped[k-1].alpha,
						       sx[k],   sy[k],   clipped[k].vz,   clipped[k].alpha,
						       cr, cg, cb);
					}
				}
			}
		}
	}

	bool tick() override {
		// ESC = exit demo (handled by runSceneBlocking); Backspace = skip scene.
		if (Keyboard[ScBackSpace] || Timer >= CHPartTime) return false;

		numFrames++;

		prof.beginFrame();
		// PROFILER: ZClear phase. also includes control keys handling
		prof.enter(PROF_ZCLR);

		// Shadow-map debug view toggle (M). Hoisted up so the
		// spotlight-snap and C-key handlers below can gate on it.
		// Also publishes the state to g_shadowFullscreenView so the
		// SDL2 V_Flip thumbnail suppresses itself while M-view is on.
		static bool sShadowMapView = false;
		{
			static bool sMPrev = false;
			const bool mNow = Keyboard[ScM] != 0;
			if (mNow && !sMPrev) {
				sShadowMapView = !sShadowMapView;
				g_shadowFullscreenView.store(sShadowMapView,
				                             std::memory_order_relaxed);
			}
			sMPrev = mNow;
		}

		// F3: toggle shadow technique (depth ↔ poly-ID). Runtime A/B.
		{
			static bool sF3Prev = false;
			const bool f3Now = Keyboard[ScF3] != 0;
			if (f3Now && !sF3Prev) {
				const ShadowMode cur = g_shadowMode.load();
				const ShadowMode next = (cur == ShadowMode::Depth)
				                        ? ShadowMode::PolyId
				                        : ShadowMode::Depth;
				g_shadowMode.store(next);
				std::fprintf(stderr, "[SHADOW-MODE] %s\n",
					next == ShadowMode::Depth ? "Depth" : "PolyId");
				std::fflush(stderr);
			}
			sF3Prev = f3Now;
		}

		tickTabToggle(GreetSc, "greets");

		dTime = Timer - TTrd;
		if (Keyboard[ScP]) pause_mode = true;
		if (Keyboard[ScU]) pause_mode = false;
		if (pause_mode) {
			Timer = TTrd;
		}
		// Fast forward / rewind. In normal play, scrub at 8× the
		// frame's dTime. In pause, dTime is 0 (Timer pinned above) so
		// we'd never advance — fall back to a small per-tick step so
		// F1/F2 still scrubs while paused (10 ms ≈ 0.3 frames at 30 fps,
		// fine-grained enough to step single anim frames).
		const int32_t scrubStep = pause_mode ? 10 : (dTime * 8);
		if (Keyboard[ScF2]) {
			Timer += scrubStep;
		}
		if (Keyboard[ScF1]) {
			if (scrubStep > Timer) Timer = 0;
			else                   Timer -= scrubStep;
		}
		g_FrameTime = TTrd = Timer;

		clearFrame();

		// PROFILER: ANIM phase. also includes Dynamic Camera manager
		prof.switchTo(PROF_ANIM);

		if (Timer < CHPartTime-500.0)
			CurFrame = GreetSc->StartFrame + (GreetSc->EndFrame-GreetSc->StartFrame) * (float)g_FrameTime / (float)(CHPartTime-500.0);
		else
			CurFrame = GreetSc->EndFrame;

		Dynamic_Camera();
		// C-key: snap FC to a useful starting pose. In normal view, copy
		// current View. In shadow view, copy the spot light's pose so the
		// user can Tab into FC, see from the light's perspective, then fly
		// around (with the spot following FC) for shadow-map inspection.
		// C-key edge-detect. Held-fire would re-snap FC every frame the
		// key is down, overriding Dynamic_Camera's per-frame rotation —
		// which would look exactly like the camera being "stuck".
		static bool sCPrev = false;
		const bool cNow = Keyboard[ScC] != 0;
		const bool cFire = cNow && !sCPrev;
		sCPrev = cNow;
		if (cFire) {
			if (sShadowMapView && g_RobotSpot) {
				// Snap FC to the spot's pose. Spot is an Omni (IPos+IDir
				// only, no Mat), so we reconstruct an orientation matrix
				// via Kick_Camera. Tiny nudge if IDir is exactly along
				// world-up to avoid the basis singularity (N = (V.z, 0,
				// -V.x) normalizes to 0/0 when V is parallel to world-up);
				// world-Y yaw in Dynamic_Camera handles flying from any
				// orientation, including near-vertical, so no pre-tilt
				// needed.
				FC.ISource = g_RobotSpot->IPos;
				Vector d = g_RobotSpot->IDir;
				if (std::fabs(d.x) < 1e-3f && std::fabs(d.z) < 1e-3f) {
					d.x = 0.01f;
				}
				Vector targ = { FC.ISource.x + d.x,
				                FC.ISource.y + d.y,
				                FC.ISource.z + d.z };
				Kick_Camera(&FC.ISource, &targ, 0.0f, FC.Mat);
				FC.IFOV = View->IFOV;
			} else {
				FC.ISource = View->ISource;
				Matrix_Copy(FC.Mat, View->Mat);
				FC.IFOV = View->IFOV;
			}
		}

		Animate_Objects(GreetSc);

		// Free-camera shadow override: when in shadow-view mode AND FC is
		// active, the spotlight tracks FC's pose. Lets you fly with Tab
		// and inspect the shadow map from arbitrary vantage points.
		// Restricted to shadow-view so normal lit playback isn't hijacked
		// when the user toggles FC for unrelated reasons.
		if (sShadowMapView && g_RobotSpot && View == &FC) {
			g_RobotSpot->IPos = FC.ISource;
			// Camera "forward" = row 2 of Mat (Kick_Camera convention).
			g_RobotSpot->IDir.x = FC.Mat[2][0];
			g_RobotSpot->IDir.y = FC.Mat[2][1];
			g_RobotSpot->IDir.z = FC.Mat[2][2];
			Vector_Norm(&g_RobotSpot->IDir);
		}
		// Robot spotlight follow. Position the spot ~10 units above the
		// robot, pointing nearly straight down through it with a slow
		// Lissajous sweep so the cone footprint drifts ±~12° from
		// vertical. The drift makes the shadow visibly move under the
		// robot. CurFrame ticks with scene playback so the rate stays
		// stable across timer stalls.
		//
		// IDir is never exactly (0,±1,0): Kick_Camera's basis math
		// (N=(V.z,0,-V.x) then normalize) divides by sqrt(0) at
		// world-up. Our sweep amplitude keeps |IDir.x| ≥ ~0.2 most of
		// the time and ≥ 0.005 even at the sweep zero-crossing.
		else if (g_RobotSpot && g_RobotMesh) {
			g_RobotSpot->IPos.x = g_RobotMesh->IPos.x;
			// Spot height above the robot. Above ~+10 sits above the
			// ceiling (cone fires into the ceiling top, corridors stay
			// dark); too low sits on the robot itself (no shadow lever
			// arm). +5 is a reasonable middle.
			const float envSpotHeight = fds::FeatureFlags::greets_spot_height();
			const float kSpotHeight = envSpotHeight > 0.0f ? envSpotHeight : 4.0f;
			g_RobotSpot->IPos.y = g_RobotMesh->IPos.y + kSpotHeight;
			g_RobotSpot->IPos.z = g_RobotMesh->IPos.z;
			const float t = float(CurFrame) * 0.02f;
			const float maxOff = std::sin(12.0f * float(PI) / 180.0f);  // ~0.21
			const float dx = std::sin(t)       * maxOff + 0.005f;       // never 0
			const float dz = std::cos(t * 0.7f) * maxOff;
			const float dy = -std::sqrt(std::max(0.0f, 1.0f - dx*dx - dz*dz));
			g_RobotSpot->IDir.x = dx;
			g_RobotSpot->IDir.y = dy;
			g_RobotSpot->IDir.z = dz;
		}

		// Orbit spots — three colored lights moving around the robot at
		// 120° phase offsets. Position on a horizontal circle at radius
		// kOrbitRadius around the robot, height kOrbitHeight above its
		// feet, pointing inward/down toward the robot. Each light's
		// shadow map is rendered into its own ShadowMap by the per-
		// frame orchestrator (Render_DeferredShadowMaps walks
		// g_shadowMaps), so colored shadows from each light overlap
		// independently on the floor and walls.
		if (g_RobotMesh) {
			constexpr float kOrbitRadius = 8.0f;
			// Capped at +4 above the robot so the spots stay below the
			// ceiling (light pointing straight down from beyond the
			// ceiling would shadow nothing useful + fire the spot cone
			// into the wrong volume). Matches the main g_RobotSpot
			// height (kSpotHeight=4 above the robot, also tuned to fit
			// under the ceiling).
			constexpr float kOrbitHeight = 4.0f;
			constexpr float kOrbitSpeed  = 0.015f;
			const float baseAngle = float(CurFrame) * kOrbitSpeed;
			for (int i = 0; i < kOrbitSpotCount; ++i) {
				Omni *o = g_OrbitSpots[i];
				if (!o) continue;
				const float phase = baseAngle
					+ float(i) * (2.0f * float(PI) / float(kOrbitSpotCount));
				const float cs = std::cos(phase);
				const float sn = std::sin(phase);
				const Vector target = g_RobotMesh->IPos;
				o->IPos.x = target.x + cs * kOrbitRadius;
				o->IPos.y = target.y + kOrbitHeight;
				o->IPos.z = target.z + sn * kOrbitRadius;
				// Aim at the robot from the light's position; keep the
				// IDir off the world-up axis (Kick_Camera would
				// singularize) by mixing horizontal direction with the
				// downward component.
				const float dxw = target.x - o->IPos.x;
				const float dyw = (target.y + 1.0f) - o->IPos.y;  // aim slightly above feet
				const float dzw = target.z - o->IPos.z;
				const float invLen = 1.0f / std::sqrt(dxw*dxw + dyw*dyw + dzw*dzw);
				o->IDir.x = dxw * invLen;
				o->IDir.y = dyw * invLen;
				o->IDir.z = dzw * invLen;
			}
		}

		// PROFILER: XFRM phase.
		prof.switchTo(PROF_XFRM);

		Transform_Objects(GreetSc, fds::g_mainCamera, fds::g_mainFaces);

		// PROFILER: LGHT phase.
		prof.switchTo(PROF_LGHT);

		Lighting(GreetSc);

		// PROFILER: SORT phase.
		prof.leave(PROF_LGHT);
		if (!CAll) return true;
		prof.enter(PROF_SORT);

		Radix_Sort(FList, SList, CAll);

		// PROFILER: RNDR phase.
		prof.switchTo(PROF_RNDR);

		gg->Render();
		// reset per-frame zbuffer statistics
		zPass = zReject = 0;
		// Per-frame shadow-map depth pre-pass (Omni_CastsShadow lights).
		// Gated on env FDS_SHADOWS=1. No-op when off. Must run BEFORE
		// Render() because it swaps view globals and re-runs
		// Transform_Objects internally — Render()'s post-restore
		// transform leaves the main view in shape.
		Render_DeferredShadowMaps(GreetSc);
		Render();

		// PROFILER: FLIP phase. also contains display of runtime stats
		prof.leave(PROF_RNDR);

		// Persistent render-mode indicator (top-right). Always visible
		// outside shadow-viz mode so the active F3 mode is obvious.
		if (!sShadowMapView) {
			const bool inPolyId = (g_shadowMode.load() == ShadowMode::PolyId);
			snprintf(MSGStr, sizeof(MSGStr), "Shadow: %s [F3]",
				inPolyId ? "PolyId" : "Depth");
			// Right-justify against the right edge — variable string
			// length (PolyId vs Depth) shouldn't shift the indicator.
			// One line below the resolution overlay drawn in V_Flip
			// (SDL2.cpp:48) at y≈4*scale with ~16*scale glyph height.
			const int kCharPx = 10;  // approx per-glyph width at 1x, scales with g_fontScale
			const int textPx = int(strlen(MSGStr)) * kCharPx * g_fontScale;
			int xJustified = XRes - textPx - 8 * g_fontScale;
			if (xJustified < 0) xJustified = 0;
			OutTextXY(VPage, xJustified, 5 + 20 * g_fontScale, MSGStr, 255);
		}

		//draw_cones(sShadowMapView);

		// Live matID readout at screen center. Reads the G-buffer's
		// packed mat32 at the center pixel, extracts the matID byte,
		// looks up Material* via the scene's matTable, prints id + name.
		// Updates every frame so the value follows whatever's under the
		// crosshair as the camera moves. Suppressed in shadow-viz mode
		// where M overwrites the framebuffer.
		if (g_gbuffer && !sShadowMapView) {
			const int cx = XRes / 2;
			const int cy = YRes / 2;
			const size_t ci = size_t(cy) * size_t(XRes) + size_t(cx);
			if (ci < g_gbuffer->txtr.size()) {
				const uint32_t mat32 = g_gbuffer->txtr[ci];
				const uint32_t matID = (mat32 >> 20) & 0xFF;
				const MatTable mt = Scene_GetMatTable(GreetSc);
				const char *name = "?";
				if (matID < mt.count && mt.data && mt.data[matID] && mt.data[matID]->Name) {
					name = mt.data[matID]->Name;
				}
				snprintf(MSGStr, sizeof(MSGStr),
					"[MAT@(%d,%d)] id=%u %s", cx, cy, (unsigned)matID, name);
				OutTextXY(VPage, 0, YRes - 30, MSGStr, 255);
			}
			// Crosshair so user can see what pixel the readout reflects.
			dword *fb = (dword*)VPage;
			const dword crossColor = 0xFFFFFFFFu;
			for (int dx = -6; dx <= 6; ++dx) {
				int x = cx + dx;
				if (x >= 0 && x < XRes) fb[size_t(cy) * size_t(XRes) + size_t(x)] = crossColor;
			}
			for (int dy = -6; dy <= 6; ++dy) {
				int y = cy + dy;
				if (y >= 0 && y < YRes) fb[size_t(y) * size_t(XRes) + size_t(cx)] = crossColor;
			}
		}
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

		// Shadow-map debug view. sShadowMapView is toggled at the top of
		// tick() (M key, edge-detect). Grayscale: higher = closer to
		// light. The actual scene rendered this frame is discarded when
		// the toggle is on — pure debug view. g_shadowViewIdx (V key)
		// picks which map; clamp to [0, N-1] since in full-screen mode
		// the cycle skips the -1 hide state.
		if (sShadowMapView && !g_shadowMaps.empty()) {
			int idx = g_shadowViewIdx;
			if (idx < 0) idx = 0;
			if (idx >= int(g_shadowMaps.size())) idx = int(g_shadowMaps.size()) - 1;
			const ShadowMap& sm = g_shadowMaps[idx];
			// Viz follows the active render mode so what you see is what
			// the kernel is using. F3 toggles both at once.
			const bool sVizPolyId = (g_shadowMode.load() == ShadowMode::PolyId);
			// For depth-mode viz, encoded z lives in a narrow upper band
			// (typically 0xB000-0xFF80 — light pulls everything close in
			// encoded space). Raw `>>8` collapses to a tight gray band
			// that's nearly indistinguishable. Auto-stretch min..max of
			// the actual data each frame.
			uint16_t zMin = 0xFFFF, zMax = 0;
			if (!sVizPolyId) {
				for (uint16_t e : sm.depth) {
					if (e == 0) continue;  // skip unwritten
					if (e < zMin) zMin = e;
					if (e > zMax) zMax = e;
				}
				if (zMin >= zMax) { zMin = 0; zMax = 0xFFFF; }
			}
			const float invRange = sVizPolyId ? 0.0f : 255.0f / float(zMax - zMin);
			dword *out = (dword*)VPage;
			for (int py = 0; py < YRes; ++py) {
				const int smY = (py * sm.yres) / YRes;
				const uint16_t *zRow = sm.depth.data() + size_t(smY) * size_t(sm.xres);
				const uint8_t *idRow = sm.polyId.data() + size_t(smY) * size_t(sm.xres);
				for (int px = 0; px < XRes; ++px) {
					const int smX = (px * sm.xres) / XRes;
					uint32_t pixel;
					if (sVizPolyId) {
						const uint32_t v = idRow[smX];
						if (v == 0) {
							pixel = 0xFF000000u;
						} else {
							uint32_t h = v * 2654435761u;
							uint8_t r = uint8_t(h);
							uint8_t g = uint8_t(h >> 8);
							uint8_t b = uint8_t(h >> 16);
							pixel = uint32_t(b) | (uint32_t(g) << 8)
							      | (uint32_t(r) << 16) | 0xFF000000u;
						}
					} else {
						const uint16_t z = zRow[smX];
						uint8_t g;
						if (z == 0) {
							g = 0;
						} else {
							float t = float(z - zMin) * invRange;
							if (t < 0.0f) t = 0.0f;
							if (t > 255.0f) t = 255.0f;
							g = uint8_t(t);
						}
						pixel = dword(g) | (dword(g) << 8) | (dword(g) << 16) | 0xFF000000u;
					}
					out[size_t(py) * size_t(XRes) + size_t(px)] = pixel;
				}
			}
			OutTextXY(VPage, 0, 0,
				sVizPolyId ? "SHADOWMAP[0] (PolyId)" : "SHADOWMAP[0] (Depth)",
				255);
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
		waitBackspaceRelease();
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