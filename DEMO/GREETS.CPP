#include "Rev.h"
#include <Base/RenderStats.h>
#include <Threads.h>
#include <Base/RenderStats.h>
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
#include "GreetsMirror.h"
#include "FillerTest.h"
#include "MeshOps.h"
#include "MISC/PREPROC.H"
#include "SceneTick.h"
#include "Scenes.h"
#include "FILLERS/Mekalele.h"
#include <map>
#include "Base/FDS_DECS.H"
#include "FILLERS/ShadowMap.h"
#include "RENDER/LightmapBake.h"
#include <chrono>
#include <memory>
#include <thread>
#include <vector>
#include <VESA/Vesa.h>

static float frand() {
	return static_cast <float> (RAND_15()) / static_cast <float> (RAND_15_MAX);
}

static Scene *GreetSc;

// Mirror plane (teleporter wall). Populated at the tail of
// Initialize_Greets via FindTeleporterMirrorPlane. Phases 2-3 of the
// mirror feature read this; Phase 1 only logs it for verification.
static fds::MirrorPlane g_greetsMirrorPlane;

// Forward-declared at namespace scope (the rest of this TU is in an
// anonymous namespace where `extern` would silently get internal
// linkage and the linker would fail to find the real definition in
// FDS/FILLERS/ShadowMap.cpp).
void ShadowMap_TickStalenessTracker();

// Background thread for the LightmapStampOrigBary + LightmapBake_Static
// pair. Spawned from Initialize_Greets as soon as the cube shadow maps
// are populated (these are the bake's inputs). Joined at Run_Greets
// entry. Lets the slow (~1-2 s) atlas bake run during glat in parallel
// with Run_Glato + the rest of the t1 init chain, instead of stalling
// the demo at the glat → city handoff. City's bake is already small
// and synchronous in Initialize_City; this matches its "during glat"
// timing for greets.
static std::thread g_greetsBakeThread;

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
			/*shadowMapRes*/1024);
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

		// Hull.lwo bin-splits per source material (2 = body/upper for
		// cockpit-vs-body self-shadow); other meshes use 1 bin.
		const int kBinsHullDefault = strstr(Obj->Name, "Hull.lwo") ? 2 : 1;
		// Per-source override: the source material named "hull"
		// specifically is collapsed back to 1 bin — its body/upper
		// boundary fires the cube PolyId path's intra-mesh identity
		// mismatch and produces spurious dark patches at the seam.
		// Other sources (cockpit, canons, sing, linght, hull-not-smooth,
		// in-canons) keep their split.
		auto effectiveBins = [&](Material *src) -> int {
			if (kBinsHullDefault <= 1) return 1;
			if (src && src->Name && std::strcmp(src->Name, "hull") == 0) {
				return 1;
			}
			return kBinsHullDefault;
		};

		// Local-Y range of face centroids (mesh-local; animation moves
		// the whole mesh but face centroids' local Y stays constant).
		// Always scan when any per-source override could use >1 bin
		// (some sources keep the split even when "hull" doesn't).
		float ymin =  1e30f, ymax = -1e30f;
		if (kBinsHullDefault > 1) {
			for (Face *F = T->Faces; F < T->Faces + T->FIndex; ++F) {
				const float fy = (F->A->Pos.y + F->B->Pos.y + F->C->Pos.y) / 3.0f;
				if (fy < ymin) ymin = fy;
				if (fy > ymax) ymax = fy;
			}
		}
		const float ySpan = (ymax > ymin) ? (ymax - ymin) : 1.0f;

		// Key: (source material, Y bin). Same source + same bin → reuse
		// clone.
		std::map<std::pair<Material*, int>, Material*> clones;
		for (Face *F = T->Faces; F < T->Faces + T->FIndex; ++F) {
			Material *src = F->Txtr;
			if (!src) continue;
			const int srcBins = effectiveBins(src);
			int bin = 0;
			if (srcBins > 1) {
				const float fy = (F->A->Pos.y + F->B->Pos.y + F->C->Pos.y) / 3.0f;
				bin = int((fy - ymin) * float(srcBins) / ySpan);
				if (bin < 0)         bin = 0;
				if (bin >= srcBins)  bin = srcBins - 1;
			}
			auto key = std::make_pair(src, bin);
			auto it = clones.find(key);
			if (it == clones.end()) {
				Material *clone = (Material*)getAlignedBlock(sizeof(Material), 16);
				std::memcpy(clone, src, sizeof(Material));
				char *nameBuf = new char[96];
				if (srcBins > 1) {
					const char *binName = (bin == 0) ? "body" : "upper";
					std::snprintf(nameBuf, 96, "%s::%s_%s", Obj->Name,
						src->Name ? src->Name : "?", binName);
				} else {
					std::snprintf(nameBuf, 96, "%s::%s", Obj->Name,
						src->Name ? src->Name : "?");
				}
				clone->Name = nameBuf;
				// 16-bit ShadowMatID lifted into a unique-per-clone slot
				// (offset 0x800 to stay clear of the legacy uint16(matID+1)
				// fallback range used by un-overridden materials). Robot
				// sub-mesh clones each get distinct ShadowMatIDs so cube
				// PolyId catches cross-sub-mesh occlusion; the explicit
				// Hull2→Hull merge below overwrites Hull2's faces' Txtr
				// to point at the same clone, sharing the ID.
				clone->ShadowMatID = (unsigned short)(0x800 + totalClones);
				Material *tail = MatLib;
				while (tail->Next) tail = tail->Next;
				clone->Next = nullptr;
				tail->Next = clone;
				it = clones.emplace(key, clone).first;
				++totalClones;
			}
			F->Txtr = it->second;
		}
		std::fprintf(stderr,
			"[ROBOT-MAT] mesh '%s' default-bins=%d cloned %zu materials\n",
			Obj->Name, kBinsHullDefault, clones.size());
	}

	// Hull2 → Hull merge: redirect every Hull2 face to Hull's "hull"
	// source-material clone (no bin suffix now that "hull" runs at 1 bin).
	// Eliminates the cube PolyId intra-mesh identity mismatch across the
	// hull/hull2 seam. Hull keeps its other per-source (cockpit, canons,
	// sing, linght, hull-not-smooth, in-canons) bin-split clones intact;
	// Hull2 loses its variety in exchange. User explicitly scoped this
	// to hull+hull2 + the hull-body/upper merge.
	{
		Material *hullClone = nullptr;
		for (Material *M = MatLib; M; M = M->Next) {
			// Match the exact clone name produced above for srcBins==1.
			if (M->Name && std::strcmp(M->Name, "Hull.lwo::hull") == 0) {
				hullClone = M;
				break;
			}
		}
		if (hullClone) {
			int redirected = 0;
			for (Object *Obj = GreetSc->ObjectHead; Obj; Obj = Obj->Next) {
				if (Obj->Type != Obj_TriMesh || !Obj->Name) continue;
				if (!std::strstr(Obj->Name, "Hull2")) continue;
				TriMesh *T = (TriMesh*)Obj->Data;
				for (Face *F = T->Faces; F < T->Faces + T->FIndex; ++F) {
					F->Txtr = hullClone;
					++redirected;
				}
			}
			std::fprintf(stderr,
				"[ROBOT-MAT] Hull2 → 'Hull.lwo::hull' merge: %d faces redirected\n",
				redirected);
		} else {
			std::fprintf(stderr,
				"[ROBOT-MAT] WARNING: 'Hull.lwo::hull' clone not found — "
				"Hull2 merge skipped, hull/hull2 boundary artifact will persist\n");
		}
	}
	// Piramid wall split: cluster Piramid.lwo faces by (source material,
	// coplanar group). Each cluster gets a unique 16-bit per-face
	// ShadowMatID so distinct walls cross-shadow while same-wall faces
	// self-match. Materials are NOT cloned — the per-face F->ShadowMatID
	// flows through the new `gb.shadowMatID` G-buffer plane to the
	// deferred PolyId cube-shadow path, sidestepping the 8-bit matID cap
	// (256 entries) in the mat32 packing. With no cloning the cluster
	// count is bounded only by the 16-bit ShadowMatID space (~65 k),
	// well above greets's ~2600 distinct clusters.
	{
		// Quantization. `kFooQ` is 1 / grid_size in the `floor(v*Q + 0.5)`
		// formula below. Normal: 16 → ~3.5° tolerance. Distance: 2.0 →
		// 0.5-unit grid (per-room walls separate; only walls within
		// half a world-unit of the same plane collapse).
		constexpr float kNormalQ   = 16.0f;
		constexpr float kDistanceQ = 2.0f;
		auto quantize = [](float v, float q) -> int {
			return int(std::floor(v * q + 0.5f));
		};
		struct PlaneKey {
			int nx, ny, nz, d;
			bool operator<(const PlaneKey &o) const {
				if (nx != o.nx) return nx < o.nx;
				if (ny != o.ny) return ny < o.ny;
				if (nz != o.nz) return nz < o.nz;
				return d < o.d;
			}
		};
		int totalClusters = 0;
		int totalTaggedFaces = 0;
		for (Object *Obj = GreetSc->ObjectHead; Obj; Obj = Obj->Next) {
			if (Obj->Type != Obj_TriMesh || !Obj->Name) continue;
			if (!std::strstr(Obj->Name, "Piramid")) continue;
			TriMesh *T = (TriMesh*)Obj->Data;
			// (source mat, plane key) → assigned ShadowMatID. Same key →
			// same id. Faces with degenerate normals keep ShadowMatID=0
			// and fall back to per-material / matID+1 default.
			std::map<std::pair<Material*, PlaneKey>, uint16_t> clusters;
			int meshTaggedFaces = 0;
			int degenFaceCount  = 0;
			for (Face *F = T->Faces; F < T->Faces + T->FIndex; ++F) {
				Material *src = F->Txtr;
				if (!src) continue;
				// Normalize the face normal so the quantization key is
				// stable regardless of source normal magnitude. The
				// authoring tool sometimes leaves them unnormalized.
				const float len = std::sqrt(F->N.x*F->N.x + F->N.y*F->N.y + F->N.z*F->N.z);
				if (!(len > 1e-4f)) { ++degenFaceCount; continue; }
				const float invLen = 1.0f / len;
				const float nx = F->N.x * invLen;
				const float ny = F->N.y * invLen;
				const float nz = F->N.z * invLen;
				// Plane distance from origin: any point on the face dotted
				// with the unit normal. Use centroid for stability.
				const float cx = (F->A->Pos.x + F->B->Pos.x + F->C->Pos.x) * (1.0f/3.0f);
				const float cy = (F->A->Pos.y + F->B->Pos.y + F->C->Pos.y) * (1.0f/3.0f);
				const float cz = (F->A->Pos.z + F->B->Pos.z + F->C->Pos.z) * (1.0f/3.0f);
				const float d  = nx*cx + ny*cy + nz*cz;
				PlaneKey key{ quantize(nx, kNormalQ),
				              quantize(ny, kNormalQ),
				              quantize(nz, kNormalQ),
				              quantize(d,  kDistanceQ) };
				auto mk = std::make_pair(src, key);
				auto it = clusters.find(mk);
				if (it == clusters.end()) {
					// Assign next ShadowMatID. Offset by 0x1000 so per-face
					// ids can't collide with the legacy `Txtr->ID + 1`
					// fallback range (~256) used by un-overridden faces.
					// 16-bit space has plenty of room (65 k - 0x1000 ≈ 61 k).
					const uint16_t newId = uint16_t(0x1000 + totalClusters);
					it = clusters.emplace(mk, newId).first;
					++totalClusters;
				}
				F->ShadowMatID = it->second;
				++meshTaggedFaces;
				++totalTaggedFaces;
			}
			std::fprintf(stderr,
				"[PIRAMID-SPLIT] mesh '%s' %u faces → %zu wall clusters "
				"(%d tagged, %d degen skipped)\n",
				Obj->Name, unsigned(T->FIndex), clusters.size(),
				meshTaggedFaces, degenFaceCount);
		}
		std::fprintf(stderr,
			"[PIRAMID-SPLIT] total clusters: %d  tagged faces: %d  "
			"(per-face F->ShadowMatID; no Material clones)\n",
			totalClusters, totalTaggedFaces);
	}
	// Piramid spatial chunk split: partition Piramid.lwo's faces into a
	// 3D spatial grid; each non-empty cell becomes its own TriMesh + Object
	// with a tight bsphere. The original Piramid is "retired" (FIndex=0)
	// but kept in the chains so its Material pointers (still referenced
	// by chunk faces via shallow-copy of F->Txtr) remain valid.
	//
	// Why: greets's mesh-level cube-face cull (Transform.cpp:cubeFaceCull)
	// can't reject Piramid as a single mesh — its bsphere has r≈60,
	// containing the mech omnis at distance ~14. With chunking, each
	// cell is ~5-10 units across and the cull rejects most chunks per
	// cube face → ~5× reduction in mech-omni xform cost.
	{
		const int chunkGrid = fds::FeatureFlags::greets_piramid_chunk_grid();
		// Chunking is a deferred-path optimization — splits the Piramid
		// into smaller TriMeshes so the per-cube-face bsphere cull in
		// shadow Transform_Objects can reject most cells per face. The
		// forward path doesn't generate cube shadows and gets nothing
		// from the split, but it DOES hit a per-mesh ambient bug:
		// Lighting() + StaticLighting compute ambient from
		// T->Faces[0].Txtr — with ~150 chunks, each chunk's first-arrived
		// face determines its ambient, and chunks that happen to land
		// on the transparent display material (low Diffuse / zero
		// ambient) render every vertex near-black. Bisected to 205b119,
		// reproduces at grid≥4 on greets fwd (2026-05-30). Until
		// Lighting() is rewritten to use per-face ambient, skip chunking
		// entirely in forward mode.
		if (chunkGrid > 0 && fds::FeatureFlags::deferred()) {
			// Find Piramid.lwo
			Object *piramidObj = nullptr;
			TriMesh *piramid = nullptr;
			for (Object *Obj = GreetSc->ObjectHead; Obj; Obj = Obj->Next) {
				if (Obj->Type != Obj_TriMesh || !Obj->Name) continue;
				if (!std::strstr(Obj->Name, "Piramid")) continue;
				piramidObj = Obj;
				piramid = (TriMesh*)Obj->Data;
				break;
			}
			if (piramid && piramid->FIndex > 0) {
				const DWord origFaces = piramid->FIndex;
				// AABB over all referenced vertices.
				float xmin=FLT_MAX, xmax=-FLT_MAX;
				float ymin=FLT_MAX, ymax=-FLT_MAX;
				float zmin=FLT_MAX, zmax=-FLT_MAX;
				for (DWord i = 0; i < piramid->FIndex; ++i) {
					const Face &f = piramid->Faces[i];
					for (Vertex *v : {f.A, f.B, f.C}) {
						if (!v) continue;
						if (v->Pos.x < xmin) xmin = v->Pos.x;
						if (v->Pos.x > xmax) xmax = v->Pos.x;
						if (v->Pos.y < ymin) ymin = v->Pos.y;
						if (v->Pos.y > ymax) ymax = v->Pos.y;
						if (v->Pos.z < zmin) zmin = v->Pos.z;
						if (v->Pos.z > zmax) zmax = v->Pos.z;
					}
				}
				const float spanX = std::max(0.001f, xmax - xmin);
				const float spanY = std::max(0.001f, ymax - ymin);
				const float spanZ = std::max(0.001f, zmax - zmin);
				auto cellOf = [&](float fx, float fy, float fz) -> int64_t {
					int ix = std::clamp(int((fx - xmin) / spanX * float(chunkGrid)),
					                    0, chunkGrid - 1);
					int iy = std::clamp(int((fy - ymin) / spanY * float(chunkGrid)),
					                    0, chunkGrid - 1);
					int iz = std::clamp(int((fz - zmin) / spanZ * float(chunkGrid)),
					                    0, chunkGrid - 1);
					return (int64_t(iz) * chunkGrid + iy) * chunkGrid + ix;
				};
				// Bucket faces by cell (face centroid).
				std::map<int64_t, std::vector<DWord>> cellFaces;
				DWord degen = 0;
				for (DWord i = 0; i < piramid->FIndex; ++i) {
					const Face &f = piramid->Faces[i];
					if (!f.A || !f.B || !f.C) { ++degen; continue; }
					const float cx = (f.A->Pos.x + f.B->Pos.x + f.C->Pos.x) * (1.0f/3.0f);
					const float cy = (f.A->Pos.y + f.B->Pos.y + f.C->Pos.y) * (1.0f/3.0f);
					const float cz = (f.A->Pos.z + f.B->Pos.z + f.C->Pos.z) * (1.0f/3.0f);
					cellFaces[cellOf(cx, cy, cz)].push_back(i);
				}
				// Build per-cell chunks.
				int chunksCreated = 0;
				DWord totalChunkVerts = 0;
				for (const auto& kv : cellFaces) {
					const auto &faceIndices = kv.second;
					if (faceIndices.empty()) continue;
					// Vertex map for this chunk: old Vertex* → new index.
					std::map<Vertex*, DWord> vertMap;
					for (DWord fi : faceIndices) {
						const Face &f = piramid->Faces[fi];
						for (Vertex *v : {f.A, f.B, f.C}) {
							if (!v) continue;
							if (vertMap.find(v) == vertMap.end()) {
								const DWord idx = DWord(vertMap.size());
								vertMap[v] = idx;
							}
						}
					}
					const DWord chunkVerts = DWord(vertMap.size());
					const DWord chunkFaces = DWord(faceIndices.size());
					TriMesh *chunk = new TriMesh();
					chunk->VIndex = chunkVerts;
					chunk->FIndex = chunkFaces;
					chunk->Verts = new Vertex[chunkVerts];
					chunk->Faces = new Face[chunkFaces];
					// Per-vertex static-lighting buffer — written by Lighting.cpp
					// StaticLighting (gated on Tri_Stationary, which the chunk
					// inherits from Piramid). Crash at runtime if null.
					chunk->SL = new Color[chunkVerts]();
					// Copy vertices.
					for (const auto& vm : vertMap) {
						chunk->Verts[vm.second] = *vm.first;
					}
					// Copy faces; re-point A/B/C; track AABB for bsphere.
					float cxmin=FLT_MAX, cxmax=-FLT_MAX;
					float cymin=FLT_MAX, cymax=-FLT_MAX;
					float czmin=FLT_MAX, czmax=-FLT_MAX;
					for (DWord i = 0; i < chunkFaces; ++i) {
						const Face &srcF = piramid->Faces[faceIndices[i]];
						Face &dstF = chunk->Faces[i];
						dstF = srcF;
						dstF.A = srcF.A ? &chunk->Verts[vertMap[srcF.A]] : nullptr;
						dstF.B = srcF.B ? &chunk->Verts[vertMap[srcF.B]] : nullptr;
						dstF.C = srcF.C ? &chunk->Verts[vertMap[srcF.C]] : nullptr;
						dstF.ParentTri = chunk;
						// SoA Phase 3 indices were inherited from srcF and
						// indexed into the PARENT piramid's Verts (16596
						// entries). The chunk holds a subset, so restamp
						// the indices to point into chunk->Verts; otherwise
						// the per-mesh VertexFrame SoA arrays (sized to
						// chunk->VIndex ≈ 64-some) get out-of-bounds reads
						// during Phase 4+ migrated consumers / Phase 6.
						if (dstF.A) dstF.A_idx = uint32_t(dstF.A - chunk->Verts);
						if (dstF.B) dstF.B_idx = uint32_t(dstF.B - chunk->Verts);
						if (dstF.C) dstF.C_idx = uint32_t(dstF.C - chunk->Verts);
						for (Vertex *v : {dstF.A, dstF.B, dstF.C}) {
							if (v->Pos.x < cxmin) cxmin = v->Pos.x;
							if (v->Pos.x > cxmax) cxmax = v->Pos.x;
							if (v->Pos.y < cymin) cymin = v->Pos.y;
							if (v->Pos.y > cymax) cymax = v->Pos.y;
							if (v->Pos.z < czmin) czmin = v->Pos.z;
							if (v->Pos.z > czmax) czmax = v->Pos.z;
						}
					}
					// Bsphere: AABB center, radius = max vertex distance.
					chunk->BSphereCtr.x = (cxmin + cxmax) * 0.5f;
					chunk->BSphereCtr.y = (cymin + cymax) * 0.5f;
					chunk->BSphereCtr.z = (czmin + czmax) * 0.5f;
					float r2max = 0.0f;
					for (DWord i = 0; i < chunkVerts; ++i) {
						const float dx = chunk->Verts[i].Pos.x - chunk->BSphereCtr.x;
						const float dy = chunk->Verts[i].Pos.y - chunk->BSphereCtr.y;
						const float dz = chunk->Verts[i].Pos.z - chunk->BSphereCtr.z;
						const float r2 = dx*dx + dy*dy + dz*dz;
						if (r2 > r2max) r2max = r2;
					}
					chunk->BSphereRadius = std::sqrt(r2max);
					chunk->BSphereRad = r2max;
					// Inherit transform state from Piramid (static, identity).
					chunk->IPos = piramid->IPos;
					chunk->IScale = piramid->IScale;
					chunk->IRot = piramid->IRot;
					Matrix_Copy(chunk->RotMat, piramid->RotMat);
					Matrix_Copy(chunk->UnscaledRotMat, piramid->UnscaledRotMat);
					// Pre-compute world-space vertex cache (depends on
					// RotMat + IPos set above). Used by the cube-face
					// vertex cull in Transform_Objects: lets the per-vertex
					// pyramid test run on world space (cheap) and the
					// survivor view xform skip the object→world step
					// (already baked in). Safe because chunk is
					// Tri_Possessed (RotMat / IPos won't change per frame).
					chunk->worldVerts = new Vector[chunkVerts];
					for (DWord i = 0; i < chunkVerts; ++i) {
						MatrixXVector(chunk->RotMat, &chunk->Verts[i].Pos,
						              &chunk->worldVerts[i]);
						Vector_SelfAdd(&chunk->worldVerts[i], &chunk->IPos);
					}
					chunk->Flags = piramid->Flags;
					// Tri_Possessed: skip TriMeshHead loop in Animate_Objects.
					// Without it, Spline_Calc_3D on the chunk's empty
					// (NumKeys=0) Pos spline derefs null Keys and hangs.
					chunk->Flags |= Tri_Possessed;
					// Wrap in Object. Allocate per-chunk Pos vector + Rot
					// matrix — Animate_Objects's Object loop writes to
					// Obj->Pos when reconciling parent transforms; sharing
					// Piramid's allocations with N chunks would race / drift.
					Object *chunkObj = new Object();
					chunkObj->Type = Obj_TriMesh;
					chunkObj->Data = chunk;
					chunkObj->Pos = new Vector(*piramidObj->Pos);
					// Matrix is `float[3][3]`; can't `new Matrix()` because
					// that's a `float(*)[3]` not a Matrix*. Allocate a 3x3
					// float buffer via the holder type.
					struct MatrixHolder { Matrix m; };
					MatrixHolder *holder = new MatrixHolder();
					chunkObj->Rot = &holder->m;
					Matrix_Copy(*chunkObj->Rot, *piramidObj->Rot);
					chunkObj->Pivot = piramidObj->Pivot;
					chunkObj->Parent = piramidObj->Parent;
					chunkObj->ParentID = piramidObj->ParentID;
					char nameBuf[80];
					std::snprintf(nameBuf, sizeof(nameBuf),
					    "Piramid.lwo:c%d", chunksCreated);
					chunkObj->Name = strdup(nameBuf);
					// Splice chunkObj just after piramidObj in ObjectHead.
					chunkObj->Next = piramidObj->Next;
					chunkObj->Prev = piramidObj;
					if (piramidObj->Next) piramidObj->Next->Prev = chunkObj;
					piramidObj->Next = chunkObj;
					// Add chunk to TriMeshHead (consumers like Lighting.cpp
					// walk TriMeshHead directly).
					chunk->Next = GreetSc->TriMeshHead;
					chunk->Prev = nullptr;
					if (GreetSc->TriMeshHead) GreetSc->TriMeshHead->Prev = chunk;
					GreetSc->TriMeshHead = chunk;
					totalChunkVerts += chunkVerts;
					++chunksCreated;
				}
				// Retire the original Piramid by zeroing its face count.
				// Don't delete its arrays — chunk faces shallow-copied F->Txtr
				// (Material*) pointers; the Materials themselves live in
				// MatLib so they're fine, but if Piramid's Faces array were
				// freed and the deferred kernel were ever to call back into
				// it, we'd UAF. Cheaper to keep alive than to audit every
				// reader.
				const DWord origVerts = piramid->VIndex;
				piramid->FIndex = 0;
				std::fprintf(stderr,
				    "[PIRAMID-CHUNKS] grid=%d³ → %d chunks  "
				    "(orig %u faces / %u verts → %u total chunk verts; "
				    "%u faces with null vertex skipped)\n",
				    chunkGrid, chunksCreated,
				    unsigned(origFaces), unsigned(origVerts),
				    unsigned(totalChunkVerts), unsigned(degen));
			}
		}
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

	// Shadow-map allocation + static cube bake + lightmap bake spawn.
	// MOVED here from GreetsScene::init() so it runs during glat (via
	// t1's Initialize_Greets) instead of waiting until the user reaches
	// the greets scene (which is after city/chase/fountain/crash playback).
	// The bake itself spawns its own thread and parallelizes with the
	// remaining scene playback.
	{
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

		// Animate_Objects fills in every FLD omni's IPos / IDir from
		// its splines (Transform.cpp:192). The cube-shadow setup below
		// needs the resolved IPos for both CubeShadowMaps_Rebuild's
		// cr.lightISource and Render_DeferredShadowMaps' bake camera.
		// cam=nullptr so Animate_Objects doesn't touch the global View.
		Animate_Objects(GreetSc, /*cam=*/nullptr);

		// Initialize the global `Polys` to greets's per-frame worst-case
		// face count. Render_DeferredShadowMaps's per-light Transform
		// sizes perLightFaces[lightIdx] from this global; without it,
		// the lambda hits a null fList in Transform.cpp:1193. Normally
		// done in GreetsScene::init() right before playback, but the
		// bake setup now runs earlier (during glat) so we redo it here.
		// init() will set Polys again when greets actually plays — that's
		// idempotent.
		Polys = 0;
		for (Object *Obj = GreetSc->ObjectHead; Obj; Obj = Obj->Next) {
			if (Obj->Type != Obj_TriMesh) continue;
			TriMesh *T = (TriMesh *)Obj->Data;
			Polys += T->FIndex;
		}
		for (Omni *O = GreetSc->OmniHead; O; O = O->Next) Polys++;
		fds::g_mainFaces.resize(Polys);

		const int omniCubeRes = std::max(16,
		    fds::FeatureFlags::greets_omni_shadow_res());
		int totalOmnis = 0;
		if (fds::FeatureFlags::greets_omni_shadows()) {
			// Build a map from Omni* → wrapping Object* so we can walk the
			// FLD parent chain. The Omni's own Pos spline can be a single
			// key (static) while world position is computed every frame
			// from `parent_transform * local_offset` in Animate_Objects
			// — meaning the omni IS moving even though O->Pos is static.
			// Without consulting parents we mis-flag mech-attached omnis
			// as static, bake them into the static cube, and never re-bake
			// dynamically.
			std::map<Omni*, Object*> omniOwner;
			for (Object *Obj = GreetSc->ObjectHead; Obj; Obj = Obj->Next) {
				if (Obj->Type == Obj_Omni && Obj->Data) {
					omniOwner[(Omni*)Obj->Data] = Obj;
				}
			}
			auto meshAncestorIsDynamic = [](Object *o) -> bool {
				constexpr float kPosEps = 0.1f, kRotEps = 0.01f;
				for (Object *p = o ? o->Parent : nullptr; p; p = p->Parent) {
					if (p->Type != Obj_TriMesh) continue;
					TriMesh *tm = (TriMesh*)p->Data;
					if (!tm) continue;
					if (tm->Pos.NumKeys > 1 && tm->Pos.Keys) {
						const auto& k0 = tm->Pos.Keys[0].Pos;
						float xmn=k0.x,xmx=k0.x,ymn=k0.y,ymx=k0.y,zmn=k0.z,zmx=k0.z;
						for (DWord i=1;i<tm->Pos.NumKeys;++i) {
							const auto& k = tm->Pos.Keys[i].Pos;
							if (k.x<xmn) xmn=k.x; if (k.x>xmx) xmx=k.x;
							if (k.y<ymn) ymn=k.y; if (k.y>ymx) ymx=k.y;
							if (k.z<zmn) zmn=k.z; if (k.z>zmx) zmx=k.z;
						}
						if ((xmx-xmn)>kPosEps || (ymx-ymn)>kPosEps || (zmx-zmn)>kPosEps)
							return true;
					}
					if (tm->Rotate.NumKeys > 1 && tm->Rotate.Keys) {
						const auto& q0 = tm->Rotate.Keys[0].Pos;
						for (DWord i=1;i<tm->Rotate.NumKeys;++i) {
							const auto& q = tm->Rotate.Keys[i].Pos;
							const float dx=q.x-q0.x, dy=q.y-q0.y, dz=q.z-q0.z, dw=q.W-q0.W;
							if (dx*dx+dy*dy+dz*dz+dw*dw > kRotEps*kRotEps) return true;
						}
					}
				}
				return false;
			};
			auto omniIsStatic = [&](Omni *O) -> bool {
				// First check the omni's own Pos spline — if it animates,
				// it's dynamic regardless of parent.
				if (O->Pos.NumKeys > 1 && O->Pos.Keys) {
					const auto& k0 = O->Pos.Keys[0].Pos;
					float xmin=k0.x, xmax=k0.x, ymin=k0.y, ymax=k0.y, zmin=k0.z, zmax=k0.z;
					for (DWord i = 1; i < O->Pos.NumKeys; ++i) {
						const auto& k = O->Pos.Keys[i].Pos;
						if (k.x < xmin) xmin=k.x; if (k.x > xmax) xmax=k.x;
						if (k.y < ymin) ymin=k.y; if (k.y > ymax) ymax=k.y;
						if (k.z < zmin) zmin=k.z; if (k.z > zmax) zmax=k.z;
					}
					constexpr float kEps = 0.1f;
					if ((xmax-xmin) >= kEps || (ymax-ymin) >= kEps || (zmax-zmin) >= kEps)
						return false;
				}
				// Local pose is static; now check the parent chain via the
				// wrapping Object. If any ancestor mesh animates, the omni
				// moves in world space and must NOT be flagged static.
				auto it = omniOwner.find(O);
				Object *wrap = (it != omniOwner.end()) ? it->second : nullptr;
				return !meshAncestorIsDynamic(wrap);
			};
			const int movingOmniResRaw = fds::FeatureFlags::greets_moving_omni_shadow_res();
			const int movingOmniRes = (movingOmniResRaw > 0)
			    ? std::max(16, movingOmniResRaw) : omniCubeRes;
			int markedStatic = 0, markedMoving = 0;
			for (Omni *O = GreetSc->OmniHead; O; O = O->Next) {
				++totalOmnis;
				if (O->Type != Light_Omni) continue;
				if (O->Flags & Omni_CastsShadow) continue;
				O->Flags |= Omni_CastsShadow;
				if (omniIsStatic(O)) {
					// World position fixed → static cube, baked once,
					// re-used every frame. Cheap.
					O->Flags |= Omni_StaticShadow;
					O->shadowMapRes = dword(omniCubeRes);
					++markedStatic;
				} else {
					// World position changes per frame (e.g. parented to
					// animated mesh). NO static bake — the cube would be
					// projected from the wrong center. Render_DeferredShadow
					// Maps's DynamicOmnisPerFrame mode re-renders all
					// geometry from current IPos into the (static) buffer
					// every frame. Lightmap bake skips these too
					// (LightmapBake.cpp filter on Omni_StaticShadow).
					// Separate res knob: full per-frame raster cost makes
					// these the most expensive omnis in the scene; dropping
					// res here costs less visual quality than dropping the
					// static omnis' res (which only pay once at bake time).
					O->shadowMapRes = dword(movingOmniRes);
					++markedMoving;
				}
			}
			std::fprintf(stderr,
			    "[GREETS] greets_omni_shadows: marked %d static @ %d² + %d moving @ %d² "
			    "/ %d Light_Omni "
			    "(moving = per-frame full re-bake; no static bake or lightmap)\n",
			    markedStatic, omniCubeRes, markedMoving, movingOmniRes, totalOmnis);
		}
		ShadowMaps_Rebuild(GreetSc, 1024);
		CubeShadowMaps_Rebuild(GreetSc, omniCubeRes);
		ShadowMaps_BakeStatic(GreetSc);
		fds::LightmapStampOrigBary(GreetSc);
		std::fprintf(stderr, "[GREETS-BAKE] spawning lightmap bake thread\n");
		const auto bakeSpawnT = std::chrono::steady_clock::now();
		Scene * const greetScForBake = GreetSc;
		g_greetsBakeThread = std::thread([greetScForBake, bakeSpawnT]() {
			fds::LightmapBake_Static(greetScForBake);
			const auto bakeDoneT = std::chrono::steady_clock::now();
			const double ms = std::chrono::duration<double, std::milli>(
				bakeDoneT - bakeSpawnT).count();
			std::fprintf(stderr,
				"[GREETS-BAKE] lightmap bake done in %.1f ms (background)\n",
				ms);
		});
	}

	// Mirror Phase 1: locate the teleporter wall and compute its plane.
	g_greetsMirrorPlane = fds::FindTeleporterMirrorPlane(GreetSc);
	// Mirror Phase 2: build a one-shot world-mirrored clone of every
	// mesh and hide the wall faces. Z-buffer + the surrounding Piramid
	// walls form the visual portal frame so the mirror only shows in
	// the teleporter rectangle. Gated by --greets-mirror (opt-in).
	if (fds::FeatureFlags::greets_mirror()) {
		fds::BuildMirrorMeshAndHideWall(GreetSc, g_greetsMirrorPlane);
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
		Calibrate_FreeCamera_ForScene(GreetSc->FZP, GreetSc->CameraHead);
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
		// Shadow-map allocation + cube bake + lightmap-bake-spawn was
		// here; moved into Initialize_Greets so it runs during glat
		// instead of waiting until greets scene starts (after crash).

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

		Animate_Objects(GreetSc, View);

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

		// Per-frame shadow-map depth pre-pass (Omni_CastsShadow lights).
		// MUST run before gg->Render(): Render() invokes the deferred
		// lighting kernel, which samples shadow maps. If shadow render
		// runs after, the kernel reads PREVIOUS frame's shadow maps
		// (one-frame lag → flicker patches during motion and at startup,
		// frame 0 reads all-zero buffers → no shadows that frame).
		// City does it in this order (CITY.CPP:2230-2235 then Render at
		// :2278). Gated on env FDS_SHADOWS=1; no-op when off.
		Render_DeferredShadowMaps(GreetSc, ShadowBakeMode::DynamicOmnisPerFrame);
		// Per-frame dynamic-mesh bake into the dynamic shadow buffer
		// of each static omni. Lighting kernel samples max(static,
		// dynamic) so moving meshes cast moving shadows on static
		// receivers. No-op when --shadow-dynamic is off.
		if (fds::FeatureFlags::shadow_dynamic()) {
			Render_DeferredShadowMaps(GreetSc, ShadowBakeMode::DynamicMeshesPerFrame);
		}
		gg->Render();
		// reset per-frame zbuffer statistics
		zPass = zReject = 0;
		// Diagnostic: identify shadow maps whose dynamic polyId hash isn't
		// changing across frames (= dynamic bake not reaching that map).
		// No-op unless toggled by T key in REV.CPP.
		::ShadowMap_TickStalenessTracker();

		// One-shot-per-N-frames log of all omnis' IPos so we can verify
		// whether the parented "mech flare" omnis actually track Hull.lwo's
		// world position (as the agent's analysis claims) or stay constant
		// (suggesting the parent-compose hypothesis is wrong).
		{
			static int sFrame = 0;
			if ((sFrame++ % 60) == 0) {
				int omniIdx = 0;
				for (Omni *O = GreetSc->OmniHead; O; O = O->Next) {
					if (O->Type != Light_Omni) continue;
					std::fprintf(stderr,
						"[OMNI-POS] frame=%d  #%d  pos=(%g,%g,%g)  flags=0x%x\n",
						sFrame - 1, omniIdx,
						O->IPos.x, O->IPos.y, O->IPos.z, unsigned(O->Flags));
					++omniIdx;
				}
				// Hull.lwo + parent chain for cross-reference. Name lives
				// on the parent Object, not the TriMesh.
				for (Object *Obj = GreetSc->ObjectHead; Obj; Obj = Obj->Next) {
					if (Obj->Type != Obj_TriMesh) continue;
					if (!Obj->Name) continue;
					if (std::strstr(Obj->Name, "Hull") ||
					    std::strstr(Obj->Name, "mech")) {
						TriMesh *T = (TriMesh*)Obj->Data;
						if (!T) continue;
						std::fprintf(stderr,
							"[MESH-POS] frame=%d  '%s'  pos=(%g,%g,%g)\n",
							sFrame - 1, Obj->Name,
							T->IPos.x, T->IPos.y, T->IPos.z);
					}
				}
			}
		}
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
				if (mat32 == 0xFFFFFFFFu) {
					// Forward-rasterizer sentinel (TheOtherBarry stamps
					// 0xFFFFFFFF to tell the deferred pass "I already
					// shaded this pixel; skip me"). Decodes to matID=255
					// but that's not a real material.
					snprintf(MSGStr, sizeof(MSGStr),
						"[MAT@(%d,%d)] forward-rendered (sentinel)", cx, cy);
				} else {
					const uint32_t matID = (mat32 >> 20) & 0xFF;
					const MatTable mt = Scene_GetMatTable(GreetSc);
					const char *name = "?";
					if (matID < mt.count && mt.data && mt.data[matID] && mt.data[matID]->Name) {
						name = mt.data[matID]->Name;
					}
					snprintf(MSGStr, sizeof(MSGStr),
						"[MAT@(%d,%d)] id=%u %s", cx, cy, (unsigned)matID, name);
				}
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
		// Per-material labels — one label per Material* at the centroid of
		// its closest-to-camera on-screen face. Lets you scrub the scene
		// and read every material's id+name at once, vs single-pixel
		// crosshair readout above. Mirrors the fountain follower-omni
		// label pattern (FOUNTAIN.CPP:2256-2268).
		if (fds::FeatureFlags::greets_mat_labels() && !sShadowMapView) {
			struct Best { Face *face; float z; };
			std::map<Material*, Best> bestByMat;
			for (Object *Obj = GreetSc->ObjectHead; Obj; Obj = Obj->Next) {
				if (Obj->Type != Obj_TriMesh) continue;
				TriMesh *T = (TriMesh*)Obj->Data;
				if (!T || !T->Faces) continue;
				Face *F = T->Faces, *FE = F + T->FIndex;
				for (; F < FE; ++F) {
					if (!F->A || !F->B || !F->C || !F->Txtr) continue;
					const float zc = (F->A->TPos_AOS.z + F->B->TPos_AOS.z + F->C->TPos_AOS.z) * (1.0f/3.0f);
					if (zc < GreetSc->NZP || zc > GreetSc->FZP) continue;
					auto it = bestByMat.find(F->Txtr);
					if (it == bestByMat.end() || zc < it->second.z) {
						bestByMat[F->Txtr] = {F, zc};
					}
				}
			}
			for (auto &kv : bestByMat) {
				Material *M = kv.first;
				Face *F = kv.second.face;
				const float px = (F->A->PX + F->B->PX + F->C->PX) * (1.0f/3.0f);
				const float py = (F->A->PY + F->B->PY + F->C->PY) * (1.0f/3.0f);
				const int ix = int(px), iy = int(py);
				if (ix < 0 || ix >= XRes || iy < 0 || iy >= YRes) continue;
				char label[80];
				std::snprintf(label, sizeof(label), "%u:%s",
					(unsigned)M->ID, M->Name ? M->Name : "?");
				OutTextXY(VPage, ix, iy, label, 255);
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

			fds::RenderStats_Flush();
			double rndrMs = prof.cumulativeMs(PROF_RNDR);
			double mPxPerFrame = FillerPixelcount / (1e6 * numFrames);
			double mPxPerSec = rndrMs > 0 ? FillerPixelcount / 1e3 / rndrMs : 0.0;
			double polysPerFrame = (double)g_renderedPolys / numFrames;
			scroll = prof.drawOverlay(scroll, polysPerFrame, mPxPerFrame, mPxPerSec);
			if (fds_stats::g_clipperEntered) {
				snprintf(MSGStr, sizeof MSGStr,
					"clip: tot=%u Z=%u 2D=%u mip=%u(uniform=%u split=%u)",
					fds_stats::g_clipperEntered,
					fds_stats::g_clipNeedZ,
					fds_stats::g_clipNeed2D,
					fds_stats::g_mipEntered,
					fds_stats::g_mipFastUniform,
					fds_stats::g_mipSplit);
				scroll = OutTextXY(VPage, 0, scroll + 15 * g_fontScale, MSGStr, 255);
				fprintf(stderr, "[GREETS-CLIP] %s\n", MSGStr);
			}
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
				const uint16_t *idRow = sm.polyId.data() + size_t(smY) * size_t(sm.xres);
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
	// Block until the background lightmap bake started in Initialize_Greets
	// has finished writing TriMesh::staticShadowLM. The first rendered
	// frame's deferred kernel samples it; sampling unwritten memory =
	// garbage or full-shadow.
	if (g_greetsBakeThread.joinable()) {
		const auto t0 = std::chrono::steady_clock::now();
		g_greetsBakeThread.join();
		const auto t1 = std::chrono::steady_clock::now();
		const double waitedMs = std::chrono::duration<double, std::milli>(
			t1 - t0).count();
		std::fprintf(stderr,
			"[GREETS-BAKE] Run_Greets waited %.1f ms for bake thread to join\n",
			waitedMs);
	}
	auto scene = createGreetsScene();
	runSceneBlocking(*scene);
}

void Greets_ShutdownBakeThread()
{
	// Called from CodeEntry's demo_exit. Without this the static
	// std::thread is still joinable at process teardown and ~thread()
	// calls std::terminate. We DETACH (not join) so ESC exits immediately
	// without waiting for the bake to finish. The bake reads from the
	// scene + writes to per-mesh atlases; once the process is exiting
	// nobody reads either side, and the OS reclaims everything when
	// main() returns. Safe because we don't run further engine code
	// that depends on the bake's output after this point.
	if (g_greetsBakeThread.joinable()) {
		std::fprintf(stderr,
			"[GREETS-BAKE] shutdown: detaching bake thread (fast exit)\n");
		g_greetsBakeThread.detach();
	}
}