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
#include "GreetsDisco.h"
#include "BlasterBolts.h"
#include <RENDER/GreetsMirror.h>
#include <RENDER/MirrorShatter.h>
#include "RENDER/EnvBake.h"
#include "RENDER/WorldAabb.h"
#include "RENDER/ChunkOcclusion.h"
#include "FillerTest.h"

// FDS_XPAR_TRACE dev diagnostic (defined in DeferredSurfaceKernel.cpp).
void Xtrace_WriteFile(const char*);
#include "MeshOps.h"
#include "DisplaceRebuild.h"   // live shell rebuild (editor displacement panel)
#include "MaterialImport.h"
#include "MISC/PREPROC.H"
#include "SceneTick.h"
#include "Scenes.h"
#include "FILLERS/Mekalele.h"
#include <map>
#include <set>
#include <algorithm>
#include "Base/FDS_DECS.H"
#include "FILLERS/ShadowMap.h"
#include "RENDER/TailProf.h"   // phase-1 serial-glue breakdown (FDS_TAIL_PROF)
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

// Mirrors active in greets. One entry per `BuildMirror(wallMatName)`
// call at scene init; per-frame `UpdateAllMirrors` re-mirrors every
// dynamic source mesh + cloned omni for each.
static std::vector<fds::Mirror> g_greetsMirrors;
// Handed to the live displacement rebuild (DEMO/DisplaceRebuild.cpp), which
// has to exclude the clone meshes from the shell build and re-stamp each clone
// face's patch id from its source afterwards. Null when no mirror was built.
std::vector<fds::Mirror> *Greets_MirrorList()
{
	return g_greetsMirrors.empty() ? nullptr : &g_greetsMirrors;
}
// Second-order RTT slots (one per outer-mirror × inner-panel pair),
// prepared at init when --mirror-rtt is on; textures re-rendered per
// frame by RenderSecondOrderMirrors before the main transform.
static std::vector<fds::MirrorRttSlot> g_greetsRttSlots;

// Shatter effect for the largest wall screen — breaks on the Y key,
// reflecting the room from a panorama baked at the moment of the break.
// Built in Initialize_Greets (so GreetsScene::init's Polys count covers
// the shard faces); driven in the tick. Index of the targeted mirror in
// g_greetsMirrors (-1 = none), so the tick can close + hide it on break.
static fds::MirrorShatter g_greetsShatter;
static int                g_greetsShatterMirror = -1;
// Invisible material the broken screen's wall faces are retargeted to, so
// the panel surface (reflection AND half-silvered text) vanishes when it
// shatters. Opaque Flags=0 keeps Transform happy; Txtr=null makes the
// rasterizer skip every face (RenderInner: `!F->Txtr->Txtr` → continue).
static Material          *g_greetsScreenHideMat = nullptr;
// The screen is a thin BOX, not a flat quad: besides the front mirror
// faces (m.wallFaces) it has side rims + a back face, with their OWN
// material — not in wallFaces, so the front-face hide alone left them
// behind as a dark strip on the wall after the break. Collected at
// BuildGreetsShatter (faces whose verts all fall inside the panel's
// (u,v) footprint AND sit behind the front plane) and retargeted to
// the hide material alongside m.wallFaces on trigger.
static std::vector<Face*> g_greetsScreenBoxFaces;

// 1x1 BGRA texture from an (R,G,B) triple — the shard material's base so
// the rasterizer's F->Txtr->Txtr deref is valid (GreetsMirror has an
// identical helper, but it's TU-local).
static Texture *makeFlatTexture(byte r, byte g, byte b) {
	Texture *T = new Texture();
	std::memset(T, 0, sizeof(Texture));
	// MUST be at least one full 4x4 tile: Txtr_Tiled stores texels in 4x4
	// blocks, so a 1x1 tiled texture is malformed — the deferred kernel's
	// swizzled-UV sample reads outside the 4-byte alloc and returns BLACK
	// (this lit the teleporter portal's silver black → it darkened the
	// reflection instead of tinting it). 8x8 = 2x2 blocks; every texel is
	// the flat colour so the tiled layout is irrelevant.
	const int S = 8;
	T->BPP = 32; T->SizeX = S; T->SizeY = S; T->LSizeX = 3; T->LSizeY = 3;
	T->Data = (byte*)_aligned_malloc(size_t(S) * S * 4, 16);
	const dword px = (dword(b) << 0) | (dword(g) << 8)
	               | (dword(r) << 16) | (dword(0xFFu) << 24);
	for (int i = 0; i < S * S; ++i) ((dword*)T->Data)[i] = px;
	T->Mipmap[0] = T->Data; T->numMipmaps = 1;
	T->Flags = Txtr_Nomip | Txtr_Tiled;
	return T;
}

// Per-frame live tint for the teleporter portal mirror (#1). The deferred
// kernel lights a TEXTURED material as Luminosity*255 (pure white) and
// IGNORES BaseCol (DeferredSurfaceKernel: `if (Mat->Txtr) lit = Lum*255`),
// so the tint must ride on the TEXTURE, not BaseCol. The portal wall was
// retargeted to a transparent clone with a flat synth texture + an alpha
// sentinel of -1 ("contribute nothing"). Swap that texture to a cool
// silver and give it a positive blend alpha: the kernel then composites
// silver*alpha + reflection*(1-alpha). pt=0 restores the clear-portal
// sentinel. Re-applied every frame so the tuning console / greets.params
// can drive it.
static void GreetsApplyLiveTints()
{
	static Texture *sPortalSilver = nullptr;
	if (!sPortalSilver) sPortalSilver = std::getenv("FDS_TINT_RED")
		? makeFlatTexture(255, 0, 0)         // verify: unmistakable bright red
		: makeFlatTexture(150, 170, 215);    // cool silver
	const float pt = fds::FeatureFlags::greets_mirror_tint();
	for (fds::Mirror &m : g_greetsMirrors) {
		if (m.wallMaterialName.find("teleporter") == std::string::npos) continue;
		if (!m.wallMatClone) continue;
		// EXACT same composite as the half-silvered screens (#2) and the
		// shards: out = litRGB + dst/2, where dst is the (warm) reflection and
		// litRGB = silver*tint is an OPTIONAL cool cast. XparBlendAlpha=0
		// selects the kernel's `litRGB + dst/2` path; Lum=pt scales the silver
		// (litRGB = Lum*255 × silverTex = silver*pt). pt=0 → litRGB=0 → out =
		// dst/2: a dimmed warm reflection, identical to #2 with no cast and to
		// the shard glaze at sv=0. Raise pt for a cool silver cast on all three.
		// (Two real bugs had to be fixed for any silver to show: the texture was
		// a malformed 1x1 Txtr_Tiled — now 8x8 — and the inherited Lum=0.)
		m.wallMatClone->Txtr           = sPortalSilver;
		m.wallMatClone->Luminosity     = pt;
		m.wallMatClone->Diffuse        = 0.0f;
		m.wallMatClone->XparBlendAlpha = 0.0f;
	}
}

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
	//
	// Light RANGE is no longer adjusted here — it is AUTHORED, in
	// Authoring/greets/JENINPYR-new-2.LWS ("LightRange <f>" per AddLight
	// block) → tools/lwsread → the FLD Range envelope → Omni::Range →
	// Omni::IRange, evaluated per frame by Animate_Objects
	// (FDS/RENDER/Transform.cpp). Two code-side patches used to sit on top
	// of that and both are gone:
	//   * this function's `OmniRangeMult[] = {2,2,2,2,2,1.7,1.7,2,2,2}`
	//     (Spline_Scale on O->Range), and
	//   * Initialize_Greets' `greets_omni_default_range` patch, which ran
	//     before the first Animate_Objects, saw IRange == 0 (which means
	//     "spline not evaluated yet", NOT "no authored range" — see
	//     FDS/FLD/FLD_CONV.CPP) and overwrote BOTH IRange and Range.Keys[0]
	//     with a flat 30, silently discarding the multipliers above AND the
	//     authored envelope.
	// The LWS now carries the final rendered value (30 on all ten), so the
	// authoring file says what actually renders and per-light tuning is an
	// authoring act. Verified byte-identical at the greets pin.
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

		// (range: authored in the LWS — see the note at the top of this
		// function. Nothing to do here.)
		index++;
	}

	int ObjStationary[] =
	//    ??? ??? ??? ??? ??? ??? ??? ??? ???
		{	1,	0,	0,	0,	0,	0,	0,	0,	0,};
	int NumObjects = sizeof(ObjStationary)/sizeof(int);

	TriMesh *T;
	for(T=Sc->TriMeshHead, index=0; T; T=T->Next, index++)
	{
		// Bounds guard (ASan find, 2026-07-03): the scene has far more meshes
		// than the 9-entry table; the unguarded read walked stack garbage and
		// randomly flagged meshes stationary depending on stack contents.
		if (index < NumObjects && ObjStationary[index])
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
		// ── DETERMINISM (2026-08-05) ────────────────────────────────────────
		// OldBuf is the SMEAR FEEDBACK source. Render() does
		//   GridRendererT(Smear_GP, &OldImage /* == OldBuf */, ScaledBuf, …)
		//   AlphaBlend(CodeBuf, ScaledBuf, …);  memcpy(OldBuf, ScaledBuf, …)
		//   TexutreRemap()          // OldBuf → OutBuf == Txtr->Mipmap[0]
		// so the FIRST frame resampled whatever _aligned_malloc handed back and
		// fed it forward into the greets screen texture from then on. Left
		// uninitialized, the code screens rendered raw heap garbage: different
		// every run, and different on every render of the SAME timestamp.
		// PROVEN, not inferred: at greets@t=1588 every other kernel input hashes
		// bit-identical across divergent runs (geometry, Z, all G-buffer planes,
		// omnis, static shadow maps, static-shadow lightmaps + coverage bits,
		// vertex colours, and every source texture at its true BPP extent) —
		// only the sampled screen texel differed (`tex=9bd0204f` vs `5ecf175c`
		// at px 1113,376, material `screen2`). Zeroing here makes that texel
		// stable and is also the correct start for a decaying smear trail.
		// ScaledBuf/CodeBuf are GridRendererT destinations; zero them too so a
		// partial write can never resurrect the same bug class.
		// Init-time only — no per-frame cost. See docs/SESSION_STATE.md.
		memset(OldBuf,    0, 256 * 256 * 4);
		memset(ScaledBuf, 0, 256 * 256 * 4);
		memset(CodeBuf,   0, 256 * 256 * 4);


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
		// scale=1: CodeImage is a fixed-size off-screen texture buffer.
		// Letting g_fontScale (HiDPI auto-double) leak in here doubles
		// glyphs in a 256x256 buffer, overflowing/wrapping the text.
		OutTextXY((byte*)(CodeImage.Data), (CodeImage.x - greet->x) / 2, (CodeImage.y - greet->y) / 2, greet->text, 255, CodeImage.x, CodeImage.y, 1);

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
			// 512²: the orbit shadows are soft colored pools — 1024²
			// was 4 of the 5 big per-frame depth rasters (~4 of the
			// bake pass's 5 ms) for no visible gain.
			/*shadowMapRes*/512);
		std::fprintf(stderr, "[GREETS] orbit spot %d (%s) installed\n",
			i, init[i].tag);
	}
}

// Build the shatter shards for the largest wall SCREEN (the two giant
// P_TEXT side-wall panels; the teleporter is excluded — it's the plain
// back-wall mirror). Runs at the tail of Initialize_Greets, before
// GreetsScene::init counts Polys, so the shard faces are sized into FList.
// Press Y at runtime to break it (see the tick).
static void BuildGreetsShatter()
{
	if (!fds::FeatureFlags::greets_mirror() || g_greetsMirrors.empty()) return;
	auto crossv = [](const Vector &a, const Vector &b) {
		return Vector{ a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
	};
	auto normv = [](Vector v) {
		float l = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
		if (l > 1e-6f) { v.x/=l; v.y/=l; v.z/=l; }
		return v;
	};
	auto worldPos = [](const Vertex *vtx, TriMesh *T) {
		Vector lp = vtx->Pos, w;
		if (T) { MatrixXVector(T->RotMat, &lp, &w); w += T->IPos; } else w = lp;
		return w;
	};
	// Pick the largest non-teleporter wall mirror by total wall-face area.
	int best = -1; float bestArea = 0.0f;
	for (size_t i = 0; i < g_greetsMirrors.size(); ++i) {
		const fds::Mirror &m = g_greetsMirrors[i];
		if (m.wallMaterialName.find("teleporter") != std::string::npos) continue;
		if (m.wallFaces.empty() || !m.plane.valid) continue;
		float area = 0.0f;
		for (size_t k = 0; k < m.wallFaces.size(); ++k) {
			const Face *F = m.wallFaces[k];
			TriMesh *T = k < m.wallFaceMeshes.size() ? m.wallFaceMeshes[k] : nullptr;
			const Vector a = worldPos(F->A, T), b = worldPos(F->B, T), c = worldPos(F->C, T);
			const Vector e1{ b.x-a.x, b.y-a.y, b.z-a.z }, e2{ c.x-a.x, c.y-a.y, c.z-a.z };
			const Vector cr = crossv(e1, e2);
			area += 0.5f * std::sqrt(cr.x*cr.x + cr.y*cr.y + cr.z*cr.z);
		}
		if (area > bestArea) { bestArea = area; best = int(i); }
	}
	if (best < 0) return;
	fds::Mirror &m = g_greetsMirrors[best];

	// Plane basis: U/V span the screen, N is the room-facing normal.
	const Vector N = normv(m.plane.N);
	const Vector ref = (std::fabs(N.y) > 0.9f) ? Vector{1,0,0} : Vector{0,1,0};
	const Vector U = normv(crossv(ref, N));
	const Vector V = normv(crossv(N, U));
	float u0=1e30f, u1=-1e30f, v0=1e30f, v1=-1e30f, ymin=1e30f;
	for (size_t k = 0; k < m.wallFaces.size(); ++k) {
		const Face *F = m.wallFaces[k];
		TriMesh *T = k < m.wallFaceMeshes.size() ? m.wallFaceMeshes[k] : nullptr;
		const Vertex *vs[3] = { F->A, F->B, F->C };
		for (int j = 0; j < 3; ++j) {
			const Vector w = worldPos(vs[j], T);
			const float pu = w.x*U.x + w.y*U.y + w.z*U.z;
			const float pv = w.x*V.x + w.y*V.y + w.z*V.z;
			u0 = std::min(u0, pu); u1 = std::max(u1, pu);
			v0 = std::min(v0, pv); v1 = std::max(v1, pv);
			ymin = std::min(ymin, w.y);
		}
	}
	const float d = m.plane.d;
	auto planePt = [&](float u, float v) {
		return Vector{ u*U.x + v*V.x - d*N.x,
		               u*U.y + v*V.y - d*N.y,
		               u*U.z + v*V.z - d*N.z };
	};
	const Vector corners[4] = {
		planePt(u0, v0), planePt(u1, v0), planePt(u1, v1), planePt(u0, v1),
	};

	// Mirror shard material: the forward env blend is reflection + base/2,
	// so a bright base washes the reflection into flat gray. Keep the base
	// near-black so the room reflection dominates (a real mirror is ~all
	// reflection), and drive it Luminosity-bright so per-vertex greets
	// lighting doesn't dim it — matching the intact screen's reflective look
	// rather than popping to silver on the break.
	Material *shardMat = getAlignedType<Material>(16);
	shardMat->Txtr       = makeFlatTexture(20, 20, 24);
	shardMat->BaseCol    = Color{24.0f, 20.0f, 20.0f, 255.0f};
	shardMat->Diffuse    = 0.0f;
	shardMat->Specular   = 0.0f;
	shardMat->Glossiness = 48;
	shardMat->Luminosity = 1.0f;
	shardMat->RelScene   = GreetSc;
	shardMat->Name       = strdup("greets_shard");
	shardMat->Next = nullptr;
	if (!MatLib) { shardMat->Prev = nullptr; MatLib = shardMat; }
	else { Material *t = MatLib; while (t->Next) t = t->Next; t->Next = shardMat; shardMat->Prev = t; }
	// Invisible material for the broken screen's wall faces (see decl).
	g_greetsScreenHideMat = getAlignedType<Material>(16);
	g_greetsScreenHideMat->Txtr     = nullptr;
	g_greetsScreenHideMat->Flags    = 0;
	g_greetsScreenHideMat->RelScene = GreetSc;
	g_greetsScreenHideMat->Name     = strdup("greets_screen_hidden");
	g_greetsScreenHideMat->Next = nullptr;
	{ Material *t = MatLib; while (t->Next) t = t->Next; t->Next = g_greetsScreenHideMat; g_greetsScreenHideMat->Prev = t; }
	Scene_RebuildMatTable(GreetSc);

	// Collect every screen face the break must hide besides the front
	// mirror panel (m.wallFaces). The screen is a box with a raised
	// emissive frame, so two families survive otherwise:
	//   (A) box body — side rims + back face: verts all inside the glass
	//       (u,v) footprint AND extending BEHIND the front plane (N·P=-d
	//       is the front; room-facing N puts the body at N·P < -d). The
	//       all-inside test rejects the surrounding wall; behind-front
	//       rejects the coplanar front faces; depth is slabbed so we
	//       don't reach far geometry.
	//       *** The "all-inside test rejects the surrounding wall" claim is
	//       true only of FLAT geometry, and that was the bug the user hit as
	//       "when the mirror breaks it takes the wall behind it with it —
	//       it's caused by the displace flag". (A) had NO material filter, so
	//       its only scoping was the footprint + a depth slab reaching
	//       0.5*min(span) = 5.93u BEHIND the glass — and the stone wall
	//       'rooms' sits inside that slab. Flat, 'rooms' is 2 room-spanning
	//       quads and no face fits the 14.55x11.86 footprint (measured: (A)
	//       contributes ZERO faces (B) didn't already have). Under
	//       --greets_displace, DisplaceStoneSubdiv splits 'rooms' into ~68.5k
	//       faces and 2436 of them fit entirely inside it → the break hides
	//       the WALL, leaving a hole into the void once the shards fall.
	//       --greets_shatter_screen_mat_only (default ON) scopes (A) by the
	//       same material test (B)/(C) use, so the mirror can only take its
	//       own geometry with it. Measured hide-face counts:
	//       450 flat / 2886 displaced before → 450 / 450 after.
	//   (B) the emissive frame ("screen emiter*") — it PROTRUDES in front
	//       of the glass (positive depth) and sits just OUTSIDE the glass
	//       rect, so (A) never catches it; that's the black border on the
	//       wall after the break. Match it by material name within an
	//       expanded footprint + depth slab, which scopes it to THIS
	//       screen (other screens' frames are on other walls, and nearby
	//       room geometry like the stairs has a different material).
	g_greetsScreenBoxFaces.clear();
	{
		const float uMargin = 0.02f * std::max(1.0f, u1 - u0);
		const float vMargin = 0.02f * std::max(1.0f, v1 - v0);
		const float maxDepth = 0.5f * std::min(u1 - u0, v1 - v0);
		const float frontEps = 0.02f * maxDepth;  // ignore coplanar front faces
		// 1.0x footprint: the half-silvered glass ("screen 3") edge faces and
		// the box frame spill well past the glass rect; the material filter
		// ("screen") keeps this scoped to THIS screen (walls/stairs/rooms and
		// neighbouring screens on other planes don't match here).
		const float uExp = 1.00f * (u1 - u0), vExp = 1.00f * (v1 - v0);
		const bool  dump = std::getenv("FDS_GREETS_SHATTER_BOXDUMP") != nullptr;
		float dmin = 1e30f, dmax = -1e30f; int nFrame = 0, nMatRejected = 0;
		for (Object *Obj = GreetSc->ObjectHead; Obj; Obj = Obj->Next) {
			if (Obj->Type != Obj_TriMesh) continue;
			TriMesh *T = (TriMesh*)Obj->Data;
			if (!T || !T->Faces) continue;
			for (DWord fi = 0; fi < T->FIndex; ++fi) {
				Face &F = T->Faces[fi];
				// Any "screen*" material is this screen's own geometry (frame,
				// emitter rims, panels — census: screen emiter / fance / green
				// / screen 3); stairs/rooms in the volume don't match, so they
				// stay. The footprint scopes it to THIS screen (others are on
				// other walls).
				const bool isFrameMat = F.Txtr && F.Txtr->Name &&
				                        strstr(F.Txtr->Name, "screen") != nullptr;
				const float uPan = 3.0f * (u1 - u0), vPan = 3.0f * (v1 - v0);
				const Vertex *vs[3] = { F.A, F.B, F.C };
				bool inGlass = true, behindFront = false, inExp = true, coplanar = true, inPanel = true;
				for (int j = 0; j < 3; ++j) {
					const Vector w  = worldPos(vs[j], T);
					const float  pu = w.x*U.x + w.y*U.y + w.z*U.z;
					const float  pv = w.x*V.x + w.y*V.y + w.z*V.z;
					const float  depth = (w.x*N.x + w.y*N.y + w.z*N.z) + d; // 0 = front
					if (depth < -frontEps) behindFront = true;
					if (pu < u0 - uMargin || pu > u1 + uMargin ||
					    pv < v0 - vMargin || pv > v1 + vMargin ||
					    depth > frontEps   || depth < -maxDepth) inGlass = false;
					if (pu < u0 - uExp || pu > u1 + uExp || pv < v0 - vExp ||
					    pv > v1 + vExp || depth < -maxDepth || depth > maxDepth) inExp = false;
					// Coplanar = on the broken screen's plane (front-glass depth).
					if (depth < -0.6f || depth > 0.6f) coplanar = false;
					// Generous panel footprint (3x) — captures the rest of the
					// physical glass panel while excluding far-Z coplanar
					// neighbours that share the plane.
					if (pu < u0 - uPan || pu > u1 + uPan || pv < v0 - vPan || pv > v1 + vPan) inPanel = false;
				}
				// (A) box body. The material test is the scoping the geometric
				// footprint cannot provide once the room shell is tessellated —
				// see the block comment above. --no-greets_shatter_screen_mat_only
				// restores the unscoped collector (the wall-eating arm).
				const bool boxMatOK = isFrameMat ||
				    !fds::FeatureFlags::greets_shatter_screen_mat_only();
				const bool isBox   = inGlass && behindFront && boxMatOK;  // (A) box body
				const bool isFrame = inExp   && isFrameMat;           // (B) frame in footprint
				// (C) The mirror cluster is only a SUB-RECTANGLE of the physical
				// glass panel; the rest of the coplanar 'screen*' glass survives
				// the break as a strip. Hide the whole coplanar panel, bounded
				// by a generous (3x) footprint so it can't reach a different
				// screen that merely shares this X-plane far away in Z.
				const bool isPanelFace = coplanar && isFrameMat && inPanel; // (C)
				// Census: faces rule (A) WOULD have taken without the material
				// scope. Under --greets_displace this is the displaced wall.
				if (inGlass && behindFront && !isFrameMat) ++nMatRejected;
				if (isBox || isFrame || isPanelFace) {
					g_greetsScreenBoxFaces.push_back(&F);
					if (isFrame) ++nFrame;
					if (dump) {
						const Vector w = worldPos(F.A, T);
						const float depth = (w.x*N.x + w.y*N.y + w.z*N.z) + d;
						dmin = std::min(dmin, depth); dmax = std::max(dmax, depth);
					}
				}
			}
		}
		if (dump)
			std::fprintf(stderr, "[GREETS-SHATTER] hide faces=%zu (frame=%d) depth=[%.3f..%.3f] "
			             "(maxDepth=%.3f span u=%.2f v=%.2f) non-screen-mat in rule(A) "
			             "footprint=%d (%s)\n",
			             g_greetsScreenBoxFaces.size(), nFrame, dmin, dmax, maxDepth,
			             u1-u0, v1-v0, nMatRejected,
			             fds::FeatureFlags::greets_shatter_screen_mat_only()
			                 ? "REJECTED by --greets_shatter_screen_mat_only"
			                 : "COLLECTED — the wall goes with the mirror");
		// One-shot per-material census over a GENEROUS region around the
		// screen (0.6x footprint margin, 1.5x depth slab), tallying total
		// vs collected per material so we can see what survives the break
		// and why (a material we don't target, or faces just outside the
		// narrower collection bounds).
		if (std::getenv("FDS_GREETS_SHATTER_SCAN")) {
			const float ue = 1.00f * (u1 - u0), ve = 1.00f * (v1 - v0);
			const float dWide = 2.5f * maxDepth;
			struct Tally { std::string name; int total=0, collected=0, reflective=0, transparent=0; float dmin=1e30f, dmax=-1e30f; };
			std::vector<Tally> tally;
			for (Object *Obj = GreetSc->ObjectHead; Obj; Obj = Obj->Next) {
				if (Obj->Type != Obj_TriMesh) continue;
				TriMesh *T = (TriMesh*)Obj->Data;
				if (!T || !T->Faces) continue;
				for (DWord fi = 0; fi < T->FIndex; ++fi) {
					Face &F = T->Faces[fi];
					const bool isFrameMat = F.Txtr && F.Txtr->Name &&
					                        strstr(F.Txtr->Name, "screen") != nullptr;
					const Vertex *vs[3] = { F.A, F.B, F.C };
					float cd = 0; bool inWide=true, inGlass=true, behindFront=false, inExp=true;
					for (int j = 0; j < 3; ++j) {
						const Vector w = worldPos(vs[j], T);
						const float pu=w.x*U.x+w.y*U.y+w.z*U.z, pv=w.x*V.x+w.y*V.y+w.z*V.z;
						const float dp=(w.x*N.x+w.y*N.y+w.z*N.z)+d; cd+=dp/3;
						if (pu<u0-ue||pu>u1+ue||pv<v0-ve||pv>v1+ve||dp<-dWide||dp>dWide) inWide=false;
						if (dp < -frontEps) behindFront = true;
						if (pu<u0-uMargin||pu>u1+uMargin||pv<v0-vMargin||pv>v1+vMargin||dp>frontEps||dp<-maxDepth) inGlass=false;
						if (pu<u0-uExp||pu>u1+uExp||pv<v0-vExp||pv>v1+vExp||dp<-maxDepth||dp>maxDepth) inExp=false;
					}
					if (!inWide) continue;
					const bool collected = (inGlass && behindFront) || (inExp && isFrameMat);
					const char *nm = (F.Txtr && F.Txtr->Name) ? F.Txtr->Name : "(null)";
					Tally *t = nullptr;
					for (Tally &x : tally) if (x.name == nm) { t = &x; break; }
					if (!t) { tally.push_back({nm,0,0,0,0,1e30f,-1e30f}); t = &tally.back(); }
					t->total++; if (collected) t->collected++;
					if (F.Flags & Face_Reflective) t->reflective++;
					if (F.Txtr && (F.Txtr->Flags & Mat_Transparent)) t->transparent++;
					t->dmin = std::min(t->dmin, cd); t->dmax = std::max(t->dmax, cd);
				}
			}
			for (const Tally &t : tally)
				std::fprintf(stderr, "[SHATTER-CENSUS] mat=%-26s total=%-4d collected=%-4d refl=%-4d xpar=%-4d %s depth=[%+.2f..%+.2f]\n",
				             t.name.c_str(), t.total, t.collected, t.reflective, t.transparent,
				             t.collected < t.total ? "<-- SURVIVES" : "", t.dmin, t.dmax);
			// Scene-wide reflective-face dump (robot-reflecting strip hunt):
			// one line per Face_Reflective material with count + world bbox +
			// reflection texture, so we can spot the strip wherever it lives.
			struct RTally { std::string name; std::string refTex; int n=0;
			                float x0=1e30f,y0=1e30f,z0=1e30f,x1=-1e30f,y1=-1e30f,z1=-1e30f; };
			std::vector<RTally> rt;
			for (Object *Obj = GreetSc->ObjectHead; Obj; Obj = Obj->Next) {
				if (Obj->Type != Obj_TriMesh) continue;
				TriMesh *T = (TriMesh*)Obj->Data;
				if (!T || !T->Faces) continue;
				for (DWord fi = 0; fi < T->FIndex; ++fi) {
					Face &F = T->Faces[fi];
					if (!(F.Flags & Face_Reflective)) continue;
					const char *nm = (F.Txtr && F.Txtr->Name) ? F.Txtr->Name : "(null)";
					const char *rx = (F.ReflectionTexture && F.ReflectionTexture->FileName)
					               ? F.ReflectionTexture->FileName : "(env)";
					RTally *e = nullptr;
					for (RTally &x : rt) if (x.name == nm && x.refTex == rx) { e = &x; break; }
					if (!e) { rt.push_back({nm,rx,0,1e30f,1e30f,1e30f,-1e30f,-1e30f,-1e30f}); e = &rt.back(); }
					e->n++;
					const Vector w = worldPos(F.A, T);
					e->x0=std::min(e->x0,w.x); e->x1=std::max(e->x1,w.x);
					e->y0=std::min(e->y0,w.y); e->y1=std::max(e->y1,w.y);
					e->z0=std::min(e->z0,w.z); e->z1=std::max(e->z1,w.z);
				}
			}
			for (const RTally &e : rt)
				std::fprintf(stderr, "[REFL-FACE] mat=%-22s refTex=%-18s n=%-4d world x[%.0f..%.0f] y[%.0f..%.0f] z[%.0f..%.0f]\n",
				             e.name.c_str(), e.refTex.c_str(), e.n, e.x0,e.x1, e.y0,e.y1, e.z0,e.z1);
		}
	}

	// ~1.2 shards per world unit, clamped.
	auto clampi = [](int x, int lo, int hi){ return x < lo ? lo : (x > hi ? hi : x); };
	const int gx = clampi(int((u1-u0)*1.2f), 6, 40);
	const int gy = clampi(int((v1-v0)*1.2f), 5, 40);
	g_greetsShatter.build(GreetSc, corners, N, shardMat, gx, gy, ymin);
	g_greetsShatterMirror = best;
	// Pre-bake the shard reflection atlas NOW (at load) rather than on the
	// break frame: the offscreen surface + atlas texture + per-shard material
	// swap + Scene_RebuildMatTable + deferred G-buffers were a ~40ms one-frame
	// hitch right when the screen shatters. trigger() only flips the per-frame
	// bake on (armReflectionCameras) + stamps the text fragments.
	// Per-shard reflection res (--greets-shard-res), rounded up to pow2 so
	// the Txtr_Tiled atlas (res² × pow2 shard grid) stays power-of-two.
	int shardRes = fds::FeatureFlags::greets_shard_res();
	{ int p = 16; while (p < shardRes && p < 1024) p <<= 1; shardRes = p; }
	g_greetsShatter.prepareReflectionAtlas(GreetSc, shardRes);
	std::fprintf(stderr,
		"[GREETS-SHATTER] target mirror %d area=%.1f grid=%dx%d floorY=%.1f "
		"N=(%.2f,%.2f,%.2f)\n", best, bestArea, gx, gy, ymin, N.x, N.y, N.z);
	// Camera pose looking straight at the broken screen, for headless
	// inspection via CITYSNAP_POS / CITYSNAP_FWD (works in --snapshot=greets).
	if (std::getenv("FDS_GREETS_SHATTER_SCAN")) {
		const Vector ctr = planePt(0.5f*(u0+u1), 0.5f*(v0+v1));
		const float dist = 0.9f * std::max(u1-u0, v1-v0);
		std::fprintf(stderr,
			"[GREETS-SHATTER] CITYSNAP_POS='%.1f %.1f %.1f' CITYSNAP_FWD='%.3f %.3f %.3f' (screen ctr %.1f,%.1f,%.1f)\n",
			ctr.x + N.x*dist, ctr.y + N.y*dist, ctr.z + N.z*dist,
			-N.x, -N.y, -N.z, ctr.x, ctr.y, ctr.z);
	}

}

// Greets scene defaults — the tuned look/perf the scene ships with. Applied
// via FeatureFlags::setDefault, so ANY explicit --flag / FDS_* on the CLI/env
// still wins (setDefault only writes when the flag wasn't user-set).
//
// SCOPING: the demo inits ALL scenes (Initialize_City/.../Greets) before
// running any, so a GLOBAL render flag (hdr, bloom, deferred_quarter, …) set
// during Initialize_Greets would leak into every scene's render (e.g. break
// the mirrortest gate, which runs Initialize_Greets then Run_MirrorTest).
// So the defaults are split by WHERE they're read:
//   - GreetsApplyInitDefaults  → Initialize_Greets: only flags the greets
//     BUILD/BAKE reads (mirror clone, omni cube + lightmap bake, shard atlas).
//     These are greets-specific; no other scene's render reads them.
//   - GreetsApplyRunDefaults   → createGreetsScene: the global render +
//     per-frame flags. createGreetsScene is greets-exclusive (Run_Greets AND
//     the bench call it; --scene-mirrortest / other single scenes don't) and
//     runs AFTER the shared init phase, so nothing leaks: greets is the last
//     scene to run, and mirrortest never creates the greets scene.
// True when a DIFFERENT single scene is the target. The demo inits every scene
// before running any, so Initialize_Greets runs even for --scene-mirrortest /
// --scene-conetest; applying greets's defaults there would leak into that
// scene's render (and the byte gate). Greets still gets them via --scene-greets,
// the greets bench, or the full sequence (where greets runs last).
static bool GreetsScenePreempted()
{
	return fds::FeatureFlags::scene_mirrortest() ||
	       fds::FeatureFlags::scene_conetest();
}

static void GreetsApplyInitDefaults()
{
	if (GreetsScenePreempted()) return;
	using FF = fds::FeatureFlags;
	FF::setDefault(FF::BoolId::greets_mirror,           true);
	FF::setDefault(FF::BoolId::no_greets_spots,         true);
	FF::setDefault(FF::BoolId::greets_omni_shadows,     true);
	FF::setDefault(FF::IntId::greets_omni_shadow_res,   512);
	// Moving omnis (mech-attached) pay a full cube re-bake every frame; 128
	// is visually adequate for lights that near-always sit close to their
	// caster and saves ~1 ms of bake (measured 2026-07-01).
	FF::setDefault(FF::IntId::greets_moving_omni_shadow_res, 128);
	// greets_omni_default_range stays at its .def default of 0 (inert).
	// Omni ranges are authored in Authoring/greets/JENINPYR-new-2.LWS; the
	// flag is a hard override for tuning only. It used to default to 30
	// here and unconditionally flattened all ten authored ranges.
	FF::setDefault(FF::BoolId::shard_deferred,          true);
	FF::setDefault(FF::IntId::greets_shard_res,         64);
	// Mirror RTT slot build. MUST be here, not in GreetsApplyRunDefaults:
	// GreetsMirror.cpp's `wantRtt = rttSlots && (mirror_rtt() || recurse)`
	// (:1401) is evaluated during Initialize_Greets, so a run-phase default
	// arrives after the decision and is INERT — measured, 0 RTT slots ever
	// built on the default path. Moving it here is what makes the reflections
	// (and second-order mirrors) actually default ON, at the cost of building
	// the slots: +3.67 ms under --greets_displace, +2.11 ms flat.
	FF::setDefault(FF::BoolId::mirror_rtt,              true);
	FF::setDefault(FF::FloatId::mirror_rtt_density,     1024.0f);
	// Lightmap bake atlas params (LightmapBake_Static reads these). NOT
	// shadow_lightmap itself: that's the per-pixel SAMPLE gate the deferred
	// kernel reads for EVERY scene, so it leaks if set during the shared init
	// phase — it lives in GreetsApplyRunDefaults instead, and the bake is
	// force-enabled at spawn so it still runs here. res/planar/skip_animated
	// only matter while sampling (shadow_lightmap on), so they're inert on
	// other scenes and safe to leave at init.
	FF::setDefault(FF::IntId::shadow_lightmap_res,      128);
	FF::setDefault(FF::BoolId::shadow_lightmap_planar,  true);
	FF::setDefault(FF::BoolId::shadow_skip_animated,    true);
	// ATLAS SIZING, BOTH ARMS (2026-08-09, at the user's instruction). The line
	// above sizes EVERY face's lightmap patch at 128², so the store is
	// numFaces * res² * numOmnis BYTES and allocate() touches all of it: the
	// SHIPPING flat arm was paying 5.61 GB for 33 396 faces and a 1.13 s startup
	// bake, and it scales with FACE COUNT while the surface it describes does
	// not grow at all (--greets_displace made that absurd, at 19.36 GB, which is
	// where the flag came from). --shadow_lightmap_texel_density sizes each
	// MESH's atlas from its mean face AREA instead —
	// res = clamp(ceil(sqrt(meanFaceArea) * density), 8, shadow_lightmap_res) —
	// capped by the line above, so it can only ever REDUCE, never sharpen. The
	// unit is texels per WORLD UNIT: 14.2 is calibrated to leave the authored
	// wall quads at the 128 cap, so a surface keeps the shadow resolution per
	// world unit the flat wall already shipped with, whatever it is tessellated
	// into. 347 of the flat arm's 370 baked meshes fall under the cap at 14.2
	// (mean face edge 1.303 world -> res 19); the 23 that keep it are the big
	// authored quads, which is why nothing the review poses look at changes.
	// MEASURED on the FLAT arm, greets t=5743, 2026-08-09: atlas 5.61 -> 0.09 GB,
	// static bake 1104 -> 54 ms (min-of-9 interleaved, load 11-17), peak
	// footprint 7.44 -> 1.50 GB (/usr/bin/time -l), greets-entry join wait
	// 3497 -> 221 ms at load 31. Per-frame: t=5743 49.39 -> 49.47 ms and t=5780
	// 51.84 -> 50.08 ms (min-of-15 interleaved) — neutral to slightly better.
	// LOOK: NULL, measured, not assumed. Byte-identical at all 16 poses of
	// docs/greets_review_poses.txt AND at the pin pose, so the greets pin
	// 778fa6acd85a69cf241babefcdaf598e does NOT move (4/4); city and fountain
	// likewise (4/4 each). It is null because the shipping arm never SAMPLES the
	// atlas: DeferredSurfaceKernel.cpp's lmKernelEnabled is
	// `!shadow_dynamic() || shadow_lm_dynamic()`, greets sets shadow_dynamic and
	// shadow_lm_dynamic defaults 0, so every pixel takes the cube tap and
	// --no-shadow_lightmap renders byte-identical frames. Forcing the atlas live
	// with --shadow_lm_dynamic is ALSO byte-identical at all 16 poses, so the
	// resolution drop is invisible on the live path too. That the flat arm bakes
	// a 5.61 GB array it then never reads is a separate, bigger finding —
	// docs/OPTIMIZATION_BACKLOG.md.
	// --shadow_lightmap_texel_density=0 is the one-flag revert to the legacy
	// fixed-128 atlas (verified: reproduces the pin 4/4).
	FF::setDefault(FF::FloatId::shadow_lightmap_texel_density, 14.2f);
	// TESSELLATION ERGONOMICS (2026-08-06). --greets_displace alone paid a
	// per-frame shadow re-raster (and an env/lightmap bake) over ~87 k displaced
	// faces to produce relief that a PolyId cube map cannot resolve anyway. The
	// S1 offscreen proxy substitutes the ~226 flat pre-displacement faces in
	// every offscreen pass and is the single biggest free-ish win the arm has, so
	// --greets_displace now turns it on for itself: ONE flag gives the good
	// tessellated arm instead of a five-flag incantation.
	// MEASURED (t=5780 wall bench, 1920x1080, 12 threads, iters=20, 4 interleaved
	// rounds, min-of-arm, load 7-10): frame 67.95 -> 62.05 ms, of which BAKE
	// 9.18 -> 3.45 (flat POM's own BAKE is 3.26 at the same pose).
	// NOT LOOK-NEUTRAL, and not claimed to be: substituting flat casters changes
	// shadow occlusion at wall/floor junctions. Colour diff over the 13 review
	// poses (16 camera/t pairs, docs/greets_review_poses.txt): byte-identical at
	// 5 of 16; elsewhere 1 282-110 668 px >4/255, worst t=6097 with 58 021 px
	// >12/255 / 14 604 >32/255 / max 144, concentrated at the corner junction —
	// a broad low-amplitude occlusion shift (mean |d| on changed px 1.0-4.3), no
	// structural artifact. --no-greets_shadow_proxy restores the displaced
	// casters (setDefault, not a force). Inert without --greets_displace, so the
	// shipping flat-POM arm is untouched.
	// The second companion, same reasoning and the same measured size: a mirror
	// clone is MAIN-VIEW geometry, so Face_MainOnly never spares it the
	// tessellated wall and the reflection re-transforms + re-rasterises all of
	// it (measured 11.40 ms/frame of the displaced arm vs 3.31 ms of the flat
	// arm; the clone pushed MORE displaced faces than the direct view). Sourcing
	// it from the flat proxy is -5.91 ms and byte-IDENTICAL at both mirror review
	// poses (t=6133, t=6293); at t=5743 it moves 2 990 px >12/255 (0.14 % of the
	// frame, inside the mirror window) and the two frames are indistinguishable
	// side by side. Needs the proxy, which the line above just defaulted on.
	// A THIRD companion used to be defaulted here — the static shadow lightmap's
	// per-face atlas, which the displaced arm blew up to 19.36 GB / 115 346 faces
	// against the flat arm's 5.61 GB / 33 396 (peak footprint 22.97 vs 6.93 GB,
	// bake 6.2-11.7 s vs 1.13 s, /usr/bin/time -l on a 64 GB box). It is no
	// longer companion-scoped: --shadow_lightmap_texel_density=14.2 is now a
	// PLAIN GREETS DEFAULT, set unconditionally ~50 lines above, because the flat
	// arm was carrying the same waste in kind. The displaced arm still gets it —
	// it just gets it the same way the shipping arm does. Both arms revert with
	// --shadow_lightmap_texel_density=0.
	if (FF::greets_displace()) {
		FF::setDefault(FF::BoolId::greets_shadow_proxy, true);
		FF::setDefault(FF::BoolId::greets_displace_flat_mirror, true);
		std::fprintf(stderr, "[STONE] --greets_displace defaults its two perf "
			"companions ON: --greets_shadow_proxy=%d (offscreen passes rasterise "
			"the flat stone, -5.9 ms, DOES move shadow occlusion at wall "
			"junctions) and --greets_displace_flat_mirror=%d (reflections source "
			"the flat stone, -5.9 ms, byte-identical at both mirror review "
			"poses). Use --no-<flag> to compare either. The lightmap atlas rule "
			"--shadow_lightmap_texel_density=%.1f is no longer a companion: it is "
			"a greets default on BOTH arms now (see the [LM] line); "
			"--shadow_lightmap_texel_density=0 restores the fixed-128 atlas.\n",
			FF::greets_shadow_proxy() ? 1 : 0,
			FF::greets_displace_flat_mirror() ? 1 : 0,
			FF::shadow_lightmap_texel_density());
	}
	// --greets_displace_flat_mirror sources the reflection's stone from the flat
	// proxy; without a proxy there is nothing to source and the reflection would
	// simply lose its walls. Say so rather than render a hole.
	if (FF::greets_displace_flat_mirror() && !FF::greets_shadow_proxy())
		std::fprintf(stderr, "[MIRROR] WARNING: --greets_displace_flat_mirror "
			"without --greets_shadow_proxy — no flat stone exists to clone, so "
			"the reflection will have NO walls. Enable the proxy.\n");
}

static void GreetsApplyRunDefaults()
{
	if (GreetsScenePreempted()) return;
	using FF = fds::FeatureFlags;
	// Shadow sampling / dynamic bake (greets tick + deferred kernel).
	FF::setDefault(FF::BoolId::shadows,                 true);
	FF::setDefault(FF::BoolId::shadow_dynamic,          true);
	// Per-pixel lightmap SAMPLE gate — set at RUN (not init) so it doesn't
	// leak onto earlier scenes; the bake itself was force-enabled at init.
	FF::setDefault(FF::BoolId::shadow_lightmap,         true);
	// PBR shading stack (deferred surface kernel, global render path). This is
	// the set the user launches greets with every time; making them scene
	// defaults is a deliberate LOOK MOVE — the greets render changes.
	//   pbr               DeferredSurfaceKernel.cpp:1429 -> Cook-Torrance (GGX D +
	//                     Smith-Schlick G + Schlick F) instead of Blinn-Phong, in
	//                     BOTH the scalar per-light path (:2400) and the 8-wide vec
	//                     path (:2156). The flag's help still says "vec path only";
	//                     that is stale — greets' normal-mapped pixels take the
	//                     scalar branch and DO get GGX.
	//   env_brdf_analytic Karis split-sum env-BRDF (ek = (f0*A+B)*gain) in place of
	//                     the ad-hoc f90=1-rough Schlick weight (:1439).
	//   pbr_multiscatter  Fdez-Aguera energy compensation built from the A,B terms
	//                     env_brdf_analytic computes (:1440) — a NO-OP without it,
	//                     which is why the two ship together.
	//   diffuse_energy    (1-F) diffuse weighting using the same per-pixel Fresnel
	//                     the env compose already produced (:1442).
	//   sh_ambient        L2 SH irradiance in place of the flat Sc->Ambient constant
	//                     (:1445 + RENDER.CPP:489's one-shot probe bake).
	// The middle three need --env_refl to change a pixel; greets gets it for free
	// because its RVSM metallic-map imports (momy / amudim / screen emiter / ...)
	// call setDefault(env_refl,true) in MaterialImport at scene init — measured in
	// the init log, not assumed. metal_map / roughness_map / ao_map are already
	// compile-default ON (FeatureFlags.def), so they need nothing here.
	// Deliberately NOT included: pbr_roughness and deferred_vec_force ([test]
	// knobs — fixed-roughness override and a cost-measurement path that throws
	// away the normal map), and xpar_pbr, which is not a dependent of pbr at all
	// (the transparent kernel reads it independently at :2785 and never reads
	// pbr()) — that is a separate look call on the glass panels.
	FF::setDefault(FF::BoolId::pbr,                     true);
	FF::setDefault(FF::BoolId::env_brdf_analytic,       true);
	FF::setDefault(FF::BoolId::pbr_multiscatter,        true);
	FF::setDefault(FF::BoolId::diffuse_energy,          true);
	FF::setDefault(FF::BoolId::sh_ambient,              true);
	// HDR + bloom + cones (global render path).
	FF::setDefault(FF::BoolId::hdr,                     true);
	FF::setDefault(FF::BoolId::hdr_linear,              true);
	// Checkerboard (half-rate) over quarter: at the kernel-bound reference
	// frame (ts=491) it costs only ~0.5-1 ms more than quarter while shading
	// 2x the pixels (full-rate is +20 ms). An explicit --deferred_quarter
	// still wins — the kernel gives quarter precedence when both are set.
	FF::setDefault(FF::BoolId::deferred_checkerboard,   true);
	FF::setDefault(FF::BoolId::bloom,                   true);
	FF::setDefault(FF::FloatId::bloom_intensity,        2.0f);
	FF::setDefault(FF::FloatId::hdr_refl_gain,          4.0f);
	FF::setDefault(FF::FloatId::cone_strength,          2.0f);
	FF::setDefault(FF::BoolId::cone_fine_tiles,         true);
	FF::setDefault(FF::FloatId::disco_bloom,            0.0f);
	// Shatter feel. mirror_rtt / mirror_rtt_density USED to be set here and
	// were inert: GreetsMirror.cpp's `wantRtt` decision runs during
	// Initialize_Greets, i.e. BEFORE this run-phase block, so the default never
	// reached it and 0 RTT slots were ever built. They now live in
	// GreetsApplyInitDefaults, next to the other build/bake flags.
	FF::setDefault(FF::FloatId::greets_shard_fall_speed, 0.8f);
	FF::setDefault(FF::FloatId::greets_shard_randomness, 0.8f);
	// Pin greets' own exposure so it doesn't inherit a leaked --cinematic value
	// from the prior scene (crash sets one via setDefault; greets runs after it).
	FF::setDefault(FF::FloatId::hdr_exposure,           cine::kGreetsExposure);
	// --cinematic: greets keeps its own hotter HDR/bloom (set above) but gains
	// the same filmic post stack as the other scenes. The disco ball + omnis are
	// the brightest sources in the demo, so anamorphic streaks earn their keep.
	if (FF::cinematic()) {
		FF::setDefault(FF::BoolId::anamorphic, true);
		FF::setDefault(FF::BoolId::chromatic,  true);
		FF::setDefault(FF::BoolId::vignette,   true);
		FF::setDefault(FF::BoolId::grade,      true);
		FF::setDefault(FF::BoolId::grain,      true);
	}
}

// --greets-stone-tex helper: fully load a texture file (pixels + 32bpp + block
// mipmaps) so it can serve either as a diffuse or as a raw-luminance HEIGHT
// source for BakeNormalMapFromDiffuse. Mirrors the per-material setup
// Unify_Textures does. The height path is colour-space-agnostic — the bake
// reads raw luminance (no gamma), which is exactly what a height map wants.
static Texture *GreetsLoadFullTexture(const char *path)
{
	Texture *t = new Texture;
	t->FileName = strdup(path);
	t->BPP = 0;
	if (!Load_Texture(t)) {
		std::fprintf(stderr, "[GREETS] stone-tex: cannot load %s\n", path);
		delete t;
		return nullptr;
	}
	if (t->BPP != 32) BPPConvert_Texture(t, 32);
	t->Flags |= Txtr_Tiled;
	Generate_Mipmaps(t, DEFAULT_BLOCKSIZEX, DEFAULT_BLOCKSIZEY, 1);
	return t;
}

// --greets-stone-tex helper: load an AUTHORED tangent-space normal map and
// hand it straight to Mat->NormalMap. The kernel reads the normal as R->nmX,
// G->nmY, B->nmZ in [-1,1] (DeferredSurfaceKernel.cpp), which is exactly a
// standard tangent-space normal map, so a file's R/G/B channels map through
// 1:1. Optional green flip (--greets-nmap-flip-g) reconciles OpenGL (+Y up)
// vs DirectX (+Y down) authoring conventions. Returns null if the file is
// absent so callers can fall back to the Sobel-from-height bake.
static Texture *GreetsLoadNormalMap(const char *path, bool flipGreen)
{
	Texture *t = new Texture;
	t->FileName = strdup(path);
	t->BPP = 0;
	if (!Load_Texture(t)) { delete t; return nullptr; }   // absent -> fall back
	if (t->BPP != 32) BPPConvert_Texture(t, 32);
	if (flipGreen) {
		// Per-pixel green negate (G -> 255-G). Works on the swizzled 32bpp
		// block layout directly — it's a per-texel op, swizzle-agnostic.
		dword *px = (dword *)t->Data;
		const size_t n = size_t(t->SizeX) * size_t(t->SizeY);
		for (size_t i = 0; i < n; ++i) {
			const dword g = (px[i] >> 8) & 0xFF;
			px[i] = (px[i] & 0xFFFF00FFu) | ((255u - g) << 8);
		}
	}
	t->Flags |= Txtr_Tiled;
	Generate_Mipmaps(t, DEFAULT_BLOCKSIZEX, DEFAULT_BLOCKSIZEY, 1);
	return t;
}

// --greets-stone-tex floor retiling: the medieval pavement tiles many times
// over the big floor and reads as a regular grid. Two fixes, both on the floor
// face UVs (the planar projection is baked at FLD load, so we rescale in code):
//   • scale (greets_floor_uv_scale) — bigger tiles, fewer repetitions
//   • de-tile (greets_floor_detile) — a low-frequency world-space warp that
//     swirls the mapping so the grid stops reading as a grid (same trick city
//     water uses). Warp is a pure function of WORLD position, so vertices
//     shared between adjacent floor faces get the identical offset → seamless.
static inline float greetsDetileU(float wx, float wz, float amp) {
	const float f = 0.012f;
	return amp * (std::sin(wx*f + wz*f*0.7f) + 0.5f*std::sin(wx*f*1.9f - wz*f*1.3f));
}
static inline float greetsDetileV(float wx, float wz, float amp) {
	const float f = 0.012f;
	return amp * (std::cos(wz*f + wx*f*0.6f) + 0.5f*std::cos(wz*f*1.7f - wx*f*1.1f));
}
static void GreetsRetileFloor(Scene *sc)
{
	const float scale = fds::FeatureFlags::greets_floor_uv_scale();
	const float amp   = fds::FeatureFlags::greets_floor_detile();
	if (scale == 1.0f && amp == 0.0f) return;
	const float invScale = scale > 0.0f ? 1.0f / scale : 1.0f;
	int touched = 0;
	for (TriMesh *T = sc->TriMeshHead; T; T = T->Next) {
		if (!T->Faces) continue;
		for (DWord fi = 0; fi < T->FIndex; ++fi) {
			Face &F = T->Faces[fi];
			if (!F.Txtr || !F.Txtr->Name || strcmp(F.Txtr->Name, "floor")) continue;
			if (!F.A || !F.B || !F.C) continue;
			Vertex *vs[3] = { F.A, F.B, F.C };
			float *uu[3] = { &F.U1, &F.U2, &F.U3 };
			float *vv[3] = { &F.V1, &F.V2, &F.V3 };
			for (int k = 0; k < 3; ++k) {
				// World position of this face vertex (mesh transform).
				Vector lp = vs[k]->Pos, wp;
				MatrixXVector(T->RotMat, &lp, &wp);
				wp.x += T->IPos.x; wp.y += T->IPos.y; wp.z += T->IPos.z;
				// Scale around 0.5 (the planar-projection centre) so bigger
				// tiles, then add the world-space de-tile warp.
				*uu[k] = (*uu[k] - 0.5f) * invScale + 0.5f + greetsDetileU(wp.x, wp.z, amp);
				*vv[k] = (*vv[k] - 0.5f) * invScale + 0.5f + greetsDetileV(wp.x, wp.z, amp);
			}
			++touched;
		}
	}
	std::fprintf(stderr, "[GREETS] floor retile: scale=%.2f detile=%.3f (%d faces)\n",
	             scale, amp, touched);
}

// Fix the normal-map relief seam at UV-handedness boundaries. The per-vertex
// tangent (Lengyel) carries UV winding in its direction, but the deferred
// kernel reconstructs the bitangent as a fixed-sign N×T — wrong on faces with
// mirrored UVs (negative determinant), whose green/V normal detail inverts and
// seams against their non-mirrored neighbours. We can't flip B per face in the
// kernel (it's deferred, per-pixel), so split the negative-det faces of every
// normal-mapped material onto a handedness=-1 clone of that material; the kernel
// reads Mat->TbnHandedness and flips B accordingly. Must run after all face→
// material retargeting (robot bin-split etc.) and before Scene_RebuildMatTable.
static void GreetsFixBitangentHandedness(Scene *sc)
{
	if (!fds::FeatureFlags::greets_tbn_fix()) return;
	std::map<Material*, Material*> mir;   // source mat -> handedness=-1 clone
	int split = 0;
	for (TriMesh *T = sc->TriMeshHead; T; T = T->Next) {
		if (!T->Faces) continue;
		for (DWord fi = 0; fi < T->FIndex; ++fi) {
			Face &F = T->Faces[fi];
			Material *M = F.Txtr;
			if (!M || !M->NormalMap) continue;        // handedness only matters w/ nmap
			if (M->TbnHandedness < 0.0f) continue;    // already a mirror clone
			const float du1 = F.U2 - F.U1, dv1 = F.V2 - F.V1;
			const float du2 = F.U3 - F.U1, dv2 = F.V3 - F.V1;
			if (du1*dv2 - du2*dv1 >= 0.0f) continue;  // positive handedness: leave
			auto it = mir.find(M);
			if (it == mir.end()) {
				Material *c = new Material;
				*c = *M;                              // share Txtr / NormalMap / shadow id
				c->TbnHandedness = -1.0f;
				char nm[96];
				std::snprintf(nm, sizeof(nm), "%s::mirUV", M->Name ? M->Name : "mat");
				c->Name = strdup(nm);
				Material *tail = MatLib; while (tail->Next) tail = tail->Next;
				c->Prev = tail; c->Next = nullptr; tail->Next = c;
				it = mir.emplace(M, c).first;
			}
			F.Txtr = it->second;
			++split;
		}
	}
	std::fprintf(stderr, "[GREETS-TBN] split %d mirrored-UV faces onto %zu handedness clones\n",
	             split, mir.size());
}

// Re-project wall/floor/ceiling UVs from a per-FACE axis-aware planar map.
// The FLD authored 'rooms'/'siling'/'floor' with a single global projection
// axis (Material.TextureFlags), so faces whose plane is perpendicular to that
// axis get near-degenerate or inconsistent U — which yields flipped per-vertex
// tangents and a diagonal relief seam through each wall quad once a directional
// normal map is applied (--viz-tangent shows the wall split red↔green across
// it). Re-projecting each face onto the plane matching its OWN dominant normal
// axis (fixed U/V convention, same TextureSize/Center so the scale + offset are
// preserved) makes the tangent uniform → no seam. Caller recomputes tangents
// afterward. Operates on the per-vertex U/V and the cached per-face U1..V3.
// (GreetsReprojectWallUVs removed — the diagonal wall-relief seam was a
// per-face-vs-per-vertex UV tangent bug; fixed at source in
// Compute_Vertex_Tangents. See docs/ENGINE.md.)


// ── S1 offscreen shadow proxy (docs/ENVDYN_DISPLACEMENT_PLAN.md) ────────────
// The displaced stone (rooms/floor, ~81k detail faces under --greets_displace)
// is main-camera-only; every offscreen consumer (per-frame shadow bake, mirror
// RTT, env probe) rasterises this FLAT stand-in instead. Mortar relief is
// invisible in cast shadows and sub-texel in reflections, so the proxy carries
// the ~226 pre-displacement faces — cutting the per-frame shadow re-raster of
// moving disco spots from ~81k faces to ~226.
//
// Snapshot is captured BEFORE the displace/subdiv/smooth-weld runs (flat geom
// with final UVs); the mesh is built AFTER them so DisplaceStoneSmoothNormals'
// position-bucket weld (per rooms/floor material) can't fold the proxy verts
// into the displaced surface. The proxy is NOT Tri_Stationary (no per-vertex SL
// buffer needed) but IS Tri_Possessed (skip the animate spline walk) and
// Tri_OffscreenProxy (skip the main pass). ShadowMatIDs are stamped from the
// displaced walls' single-id cluster AFTER the Piramid clustering, so the
// deferred PolyId self-shadow test matches between the displaced G-buffer
// pixels and this flat caster.
// S1 diagnostics (chunk shadow-skip census).
int g_s1PureChunks = 0, g_s1PureChunkFaces = 0, g_s1MixedChunks = 0, g_s1MixedMainFaces = 0;

struct StoneProxySnapshot {
    std::vector<Vertex>   verts;
    std::vector<Face>     faces;   // A/B/C repointed at build time
    std::vector<uint32_t> fidx;    // 3 vert indices per face
    bool captured = false;
};

static void CaptureStoneProxySnapshot(Scene *Sc, StoneProxySnapshot &snap) {
    snap.verts.clear(); snap.faces.clear(); snap.fidx.clear(); snap.captured = false;
    TriMesh *piramid = nullptr;
    for (Object *Obj = Sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh || !Obj->Name) continue;
        if (std::strstr(Obj->Name, "Piramid")) { piramid = (TriMesh*)Obj->Data; break; }
    }
    if (!piramid) return;
    std::map<Vertex*, uint32_t> vmap;
    for (DWord i = 0; i < piramid->FIndex; ++i) {
        Face &f = piramid->Faces[i];
        if (!f.A || !f.B || !f.C || !f.Txtr || !f.Txtr->Name) continue;
        if (std::strcmp(f.Txtr->Name, "rooms") && std::strcmp(f.Txtr->Name, "floor"))
            continue;
        uint32_t vi[3];
        Vertex *src[3] = { f.A, f.B, f.C };
        for (int k = 0; k < 3; ++k) {
            auto it = vmap.find(src[k]);
            if (it == vmap.end()) {
                vi[k] = uint32_t(snap.verts.size());
                vmap[src[k]] = vi[k];
                snap.verts.push_back(*src[k]);
            } else vi[k] = it->second;
        }
        snap.faces.push_back(f);
        snap.fidx.push_back(vi[0]); snap.fidx.push_back(vi[1]); snap.fidx.push_back(vi[2]);
    }
    snap.captured = !snap.faces.empty();
}

static TriMesh *BuildStoneProxyMesh(Scene *Sc, const StoneProxySnapshot &snap) {
    if (!snap.captured) return nullptr;
    Object *piramidObj = nullptr;
    for (Object *Obj = Sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh || !Obj->Name) continue;
        if (std::strstr(Obj->Name, "Piramid")) { piramidObj = Obj; break; }
    }
    if (!piramidObj) return nullptr;
    TriMesh *piramid = (TriMesh*)piramidObj->Data;
    const uint32_t nV = uint32_t(snap.verts.size());
    const uint32_t nF = uint32_t(snap.faces.size());
    TriMesh *proxy = new TriMesh();
    proxy->VIndex = nV;
    proxy->FIndex = nF;
    proxy->Verts = new Vertex[nV];
    proxy->Faces = new Face[nF];
    std::memcpy(proxy->Verts, snap.verts.data(), nV * sizeof(Vertex));
    float xmin=FLT_MAX,xmax=-FLT_MAX,ymin=FLT_MAX,ymax=-FLT_MAX,zmin=FLT_MAX,zmax=-FLT_MAX;
    for (uint32_t i = 0; i < nF; ++i) {
        Face &d = proxy->Faces[i];
        d = snap.faces[i];
        d.A = &proxy->Verts[snap.fidx[3*i+0]];
        d.B = &proxy->Verts[snap.fidx[3*i+1]];
        d.C = &proxy->Verts[snap.fidx[3*i+2]];
        d.A_idx = snap.fidx[3*i+0]; d.B_idx = snap.fidx[3*i+1]; d.C_idx = snap.fidx[3*i+2];
        d.ParentTri = proxy;
        d.Flags &= ~Face_MainOnly;   // the PROXY casts; it must not be dropped offscreen
        for (Vertex *v : { d.A, d.B, d.C }) {
            if (v->Pos.x<xmin) xmin=v->Pos.x; if (v->Pos.x>xmax) xmax=v->Pos.x;
            if (v->Pos.y<ymin) ymin=v->Pos.y; if (v->Pos.y>ymax) ymax=v->Pos.y;
            if (v->Pos.z<zmin) zmin=v->Pos.z; if (v->Pos.z>zmax) zmax=v->Pos.z;
        }
    }
    proxy->BSphereCtr.x=(xmin+xmax)*0.5f; proxy->BSphereCtr.y=(ymin+ymax)*0.5f; proxy->BSphereCtr.z=(zmin+zmax)*0.5f;
    float r2max=0.0f;
    for (uint32_t i=0;i<nV;++i){ float dx=proxy->Verts[i].Pos.x-proxy->BSphereCtr.x,
        dy=proxy->Verts[i].Pos.y-proxy->BSphereCtr.y, dz=proxy->Verts[i].Pos.z-proxy->BSphereCtr.z;
        float r2=dx*dx+dy*dy+dz*dz; if (r2>r2max) r2max=r2; }
    proxy->BSphereRadius=std::sqrt(r2max); proxy->BSphereRad=r2max;
    proxy->IPos=piramid->IPos; proxy->IScale=piramid->IScale; proxy->IRot=piramid->IRot;
    Matrix_Copy(proxy->RotMat, piramid->RotMat);
    Matrix_Copy(proxy->UnscaledRotMat, piramid->UnscaledRotMat);
    // Inherit Piramid flags but: drop Tri_Stationary (no SL buffer allocated),
    // add Tri_Possessed (empty splines → skip Animate), add Tri_OffscreenProxy.
    proxy->Flags = (piramid->Flags & ~Tri_Stationary) | Tri_Possessed | Tri_OffscreenProxy;
    Object *proxyObj = new Object();
    proxyObj->Type = Obj_TriMesh;
    proxyObj->Data = proxy;
    proxyObj->Pos = new Vector(*piramidObj->Pos);
    struct MatrixHolder { Matrix m; };
    MatrixHolder *holder = new MatrixHolder();
    proxyObj->Rot = &holder->m;
    Matrix_Copy(*proxyObj->Rot, *piramidObj->Rot);
    proxyObj->Pivot = piramidObj->Pivot;
    proxyObj->Parent = piramidObj->Parent;
    proxyObj->ParentID = piramidObj->ParentID;
    proxyObj->Name = strdup("stone_shadow_proxy");   // no "Piramid" → clustering/chunk skip it
    proxyObj->Next = piramidObj->Next;
    proxyObj->Prev = piramidObj;
    if (piramidObj->Next) piramidObj->Next->Prev = proxyObj;
    piramidObj->Next = proxyObj;
    proxy->Next = Sc->TriMeshHead; proxy->Prev = nullptr;
    if (Sc->TriMeshHead) Sc->TriMeshHead->Prev = proxy;
    Sc->TriMeshHead = proxy;
    std::fprintf(stderr, "[STONE-PROXY] built flat shadow proxy: %u verts / %u faces "
        "(rooms+floor pre-displace) — offscreen-only\n", nV, nF);
    return proxy;
}

// Tag every displaced rooms/floor face main-only (skip in offscreen passes) and
// stamp the flat proxy's faces with the displaced walls' single shadow-cluster
// id so the PolyId self-shadow test matches. Called after the Piramid clustering.
static void FinalizeStoneProxy(Scene *Sc, TriMesh *proxy) {
    uint16_t roomsId = 0, floorId = 0;
    for (Object *Obj = Sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh || !Obj->Name) continue;
        if (!std::strstr(Obj->Name, "Piramid")) continue;
        TriMesh *T = (TriMesh*)Obj->Data;
        for (Face *F = T->Faces; F < T->Faces + T->FIndex; ++F) {
            if (!F->Txtr || !F->Txtr->Name) continue;
            if (!std::strcmp(F->Txtr->Name, "rooms")) { if (!roomsId) roomsId = F->ShadowMatID; }
            else if (!std::strcmp(F->Txtr->Name, "floor")) { if (!floorId) floorId = F->ShadowMatID; }
        }
    }
    if (proxy) {
        for (Face *F = proxy->Faces; F < proxy->Faces + proxy->FIndex; ++F) {
            if (!F->Txtr || !F->Txtr->Name) continue;
            if (!std::strcmp(F->Txtr->Name, "rooms")) F->ShadowMatID = roomsId;
            else if (!std::strcmp(F->Txtr->Name, "floor")) F->ShadowMatID = floorId;
        }
        std::fprintf(stderr, "[STONE-PROXY] stamped shadow ids rooms=0x%04x floor=0x%04x "
            "onto proxy caster\n", roomsId, floorId);
    }
}

void Initialize_Greets()
{
	InitTimelineMark("greets: Initialize_Greets begin");
	GreetsApplyInitDefaults();   // greets-specific build/bake flags, before any init read
	// --pom_rebuild (editor): defer a requested --pom_shell past init so the
	// pristine snapshot taken at the end of this function is genuinely pristine.
	// No-op without the flag (the shipping demo path is untouched).
	rev::DisplaceRebuild_BeginInit();
	GreetSc = loadSceneAligned("SCENES/GREETS.FLD");
	InitTimelineMark("greets: LoadFLD end");

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

	// --greets-stone-tex: before Preprocess loads textures, point the wall
	// ('rooms') + floor ('floor') diffuse at the 1024² Egyptian sandstone set
	// so they load through the normal pipeline (correct matID + mipmaps). For
	// the tangent-space normal we PREFER an authored map (greets_*_n.png) —
	// loaded straight onto Mat->NormalMap — and only Sobel-bake from a height
	// map (greets_*_h.png) when no authored map exists. Set on the Material so
	// the post-Preprocess luminance-bake below skips it (gates on !NormalMap).
	// Albedo stays sRGB — the HDR-linear path squares it.
	if (fds::FeatureFlags::greets_stone_tex()) {
		const bool flipG = fds::FeatureFlags::greets_nmap_flip_g();
		for (Material *M = MatLib; M; M = M->Next) {
			if (M->RelScene != GreetSc || !M->Name || !M->Txtr) continue;
			// albedo is RGBA with AO baked into the alpha channel (Mat_AoInAlpha):
			// the kernel reads occlusion straight from the already-fetched albedo
			// texel — no separate AO texture, no extra per-pixel fetch.
			const char *albedo = nullptr, *height = nullptr, *normal = nullptr, *rough = nullptr;
			float pxScale = 1.0f;
			if      (!strcmp(M->Name, "rooms")) { albedo = "TEXTURES/greets_wall.png";  height = "TEXTURES/greets_wall_h.png";  normal = "TEXTURES/greets_wall_n.png";  rough = "TEXTURES/greets_wall_r.png"; }
			else if (!strcmp(M->Name, "floor")) { albedo = "TEXTURES/greets_floor.png"; height = "TEXTURES/greets_floor_h.png"; normal = "TEXTURES/greets_floor_n.png"; rough = "TEXTURES/greets_floor_r.png"; pxScale = 0.25f; }  // floor: offset-parallax swims on the grazing, densely-tiled floor → dial it down
			if (!albedo) continue;
			M->Txtr->FileName = strdup(albedo);   // old name leaks (init-time, tiny)
			M->Txtr->BPP = 0;                     // force (re)load by Unify_Textures
			M->Flags |= Mat_AoInAlpha;            // albedo alpha = baked AO
			// Code-default parallax depth for the legacy stone-tex path — but only
			// when the surface DIDN'T author one. The editor persists per-material
			// parallaxScale into the LWO 'RVSF' sub-chunk (loaded into
			// Material::ParallaxScale at FLD time); stomping it here unconditionally
			// made editor parallax edits on 'rooms'/'floor' silently do nothing
			// (they saved to RVSF but never rendered — the "parallax changes aren't
			// being saved" report). Honor an authored value (!= the 1.0 default);
			// floor's unauthored default still gets its dialed-down 0.25.
			if (M->ParallaxScale == 1.0f) M->ParallaxScale = pxScale;
			// Roughness map (--roughness_map): per-pixel specular intensity. 8-bit
			// single-channel (MakeHeight8), same layout as albedo → same swizzled UV.
			if (rough) {
				if (Texture *r32 = GreetsLoadFullTexture(rough)) {
					Texture *r8 = MakeHeight8(r32);
					M->RoughnessMap = r8 ? r8 : r32;
					std::fprintf(stderr, "[GREETS] roughness '%s' -> %s (%s)\n",
					             M->Name, rough, r8 ? "8-bit" : "32-bit");
				}
			}
			// Parallax (--parallax): load the grayscale height map onto the
			// material so Mekalele can offset the UV by it. Shares the albedo's
			// tiled+mip layout (GreetsLoadFullTexture → Generate_Mipmaps), so the
			// swizzled UV/miplevel the rasterizer computes indexes it directly.
			if (fds::FeatureFlags::parallax() && height) {
				if (Texture *hm32 = GreetsLoadFullTexture(height)) {
					// Pack to 8-bit single-channel (¼ the memory; same layout so
					// the rasterizer's swizzled index still indexes it).
					Texture *hm = MakeHeight8(hm32);
					M->HeightMap = hm ? hm : hm32;
					std::fprintf(stderr, "[GREETS] parallax '%s' -> height %s (%dx%d mips=%u, %s)\n",
					             M->Name, height, M->HeightMap->SizeX, M->HeightMap->SizeY,
					             M->HeightMap->numMipmaps, hm ? "8-bit" : "32-bit fallback");
					// Tier-2 cone-step POM (--parallax_pom): offline-bake a cone map
					// companion (needs the 8-bit height layout). Only when the flag is
					// on → default-off pays nothing and is byte-identical.
					if (fds::FeatureFlags::parallax_pom_cone() && M->HeightMap->BPP == 8) {
						auto t0 = std::chrono::high_resolution_clock::now();
						// --pom_cone_exact (S1 P1): per-texel EGSR'24-style bake
						// through a disk cache, instead of the max-pooled 128²
						// grid. Default 0 = the legacy bake, byte-identical.
						const int coneExact = fds::FeatureFlags::pom_cone_exact();
						M->ConeMap = coneExact > 0
						    ? LoadOrBakeConeMapExact(M->HeightMap, coneExact, M->Name)
						    : MakeConeMap(M->HeightMap);
						double ms = std::chrono::duration<double, std::milli>(
							std::chrono::high_resolution_clock::now() - t0).count();
						std::fprintf(stderr, "[GREETS] parallax_pom '%s' -> cone map %s (exact=%d, %.0f ms)\n",
						             M->Name, M->ConeMap ? "baked" : "FAILED", coneExact, ms);
					}
					// S1c (--pom_horizon): bake the relief self-shadow horizon
					// HERE, off the FULL height field — the --greets_displace
					// path swaps M->HeightMap for the RESIDUAL further down, but
					// the geometry still carries the low band, so the shadow term
					// must model the whole relief in both paths. The amplitude is
					// the same UV number the shell geometry is built with.
					// The kernel addresses the horizon map with the ALBEDO's
					// swizzledUV + miplevel (exactly as it does the normal and AO
					// maps). The albedo is only RE-POINTED here (BPP=0 forces
					// Unify_Textures to reload it inside Preprocess_Scene), so it
					// is still the OLD 256² texture at this moment and cannot be
					// compared yet — the layout check runs post-Preprocess below.
					if (fds::FeatureFlags::pom_horizon() && M->HeightMap->BPP == 8) {
						M->PomHorizon = LoadOrBakeHorizonMap(
							M->HeightMap,
							fds::FeatureFlags::parallax_strength() * M->ParallaxScale,
							fds::FeatureFlags::pom_horizon_radius(), M->Name);
					}
				}
			}
			if (Texture *n = GreetsLoadNormalMap(normal, flipG)) {
				// --nmap_16bit: pack to 16-bit RG (Z reconstructed in-shader) —
				// half the memory; the kernel decode branches on BPP.
				if (fds::FeatureFlags::nmap_16bit()) {
					if (Texture *n16 = MakeNormal16(n)) n = n16;
				}
				M->NormalMap = n;
				std::fprintf(stderr, "[GREETS] stone-tex '%s' -> %s + AUTHORED normal %s "
				             "(flipG=%d, %dx%d mips=%u, %d-bit)\n",
				             M->Name, albedo, normal, flipG, n->SizeX, n->SizeY, n->numMipmaps, n->BPP);
			} else if (Texture *h = GreetsLoadFullTexture(height)) {
				M->NormalMap = BakeNormalMapFromDiffuse(h, 4.0f);
				std::fprintf(stderr, "[GREETS] stone-tex '%s' -> %s + baked normal from %s "
				             "(height %dx%d mips=%u, normal mips=%u)\n",
				             M->Name, albedo, height, h->SizeX, h->SizeY, h->numMipmaps,
				             M->NormalMap ? M->NormalMap->numMipmaps : 0);
			}
		}
	}

	Preprocess_Scene(GreetSc, "GREETS");
	InitTimelineMark("greets: Preprocess_Scene end (textures loaded)");

	// S1c layout check, now that Unify_Textures has actually LOADED the albedo
	// (before Preprocess the stone materials still point at their old 256²
	// texture — see the bake site above). The kernel indexes the horizon map
	// with the albedo's swizzled UV + miplevel, so a layout mismatch would read
	// a wrong texel silently; drop the map instead.
	for (Material *M = MatLib; M; M = M->Next) {
		if (M->RelScene != GreetSc || !M->PomHorizon) continue;
		const Texture *A = M->Txtr;
		const PomHorizonMap *Hz = M->PomHorizon;
		if (A && A->SizeX == Hz->sizeX && A->SizeY == Hz->sizeY
		    && A->blockSizeX == Hz->blockSizeX && A->blockSizeY == Hz->blockSizeY
		    && A->numMipmaps == Hz->numMipmaps)
			continue;
		std::fprintf(stderr, "[POM-HORIZON] '%s': height layout (%dx%d bs=%d,%d "
			"mips=%u) != albedo (%dx%d bs=%d,%d mips=%u) — the kernel addresses "
			"the horizon map with the albedo's swizzled UV, so it is DROPPED for "
			"this material\n", M->Name ? M->Name : "?",
			Hz->sizeX, Hz->sizeY, Hz->blockSizeX, Hz->blockSizeY, Hz->numMipmaps,
			A ? A->SizeX : 0, A ? A->SizeY : 0, A ? A->blockSizeX : 0,
			A ? A->blockSizeY : 0, A ? A->numMipmaps : 0);
		M->PomHorizon = nullptr;
	}

	// Refraction OPT-IN (Mat_Refractive) — CODE DEFAULTS: mark the greets glass
	// display panels so they keep refracting the room behind them under
	// --glass_refract now that refraction is per-material opt-in. These are the
	// transparent (Mat_Transparent from the FLD) half-silvered "amudim"/column
	// glass screens sharing the P_TEXT design plus the glass lamp cover — the
	// ONLY greets transparents that refracted under the old blanket gate
	// ('screen 4' is the display opposite the teleporter and the sole visible
	// refractor at the greets snapshot pose; 'screen2' is the column panel;
	// 'lamp light' the lamp glass). ('amudim' itself is opaque MARB4 marble —
	// not glass; the room walls / mirror-mask clones stay unmarked.)
	//
	// Code-default glass set: mark the half-silvered greets transparents as
	// Mat_Refractive so --glass_refract warps the scene behind them. Per-material
	// 'refractive' is otherwise authored in the LWO 'RVSF' sub-chunk (loaded into
	// Mat_Refractive at FLD time, FLD_MAT.CPP); this block is the scene's built-in
	// default for its known glass surfaces. FOUNTAIN.CPP marks its glass set the
	// same way. (This block used to live inside the normal-map-bake loop below,
	// which silently re-marked surfaces — kept up here, run once.)
	for (Material *M = MatLib; M; M = M->Next) {
		if (M->RelScene != GreetSc || !M->Name) continue;
		if (!std::strcmp(M->Name, "screen 4") || !std::strcmp(M->Name, "screen2") ||
		    !std::strcmp(M->Name, "lamp light"))
			M->Flags |= Mat_Refractive;
	}

	// Source-authored PBR map assignments (custom LWO 'RVSM' sub-chunk → FLD
	// Surf_RevMaps payload → FldRevMap* — sidecar-elim §1e). The successor to
	// the retired GREETS.MAT map lines: same MaterialImport_ApplyMapFile path,
	// driven from the FLD instead of the sidecar. Before the CLI overlay so an
	// explicit --material-import below still wins. No-op when the FLD carries no
	// RVSM payload. (The .MAT sidecar reader is retired — every per-surface value
	// the sidecar carried now rides the LWO RVSF/RVSM/SMAN sources.)
	fds::MaterialImport_ApplyRevMaps(GreetSc, "greets");

	// Authored per-surface smoothing angles (native LWO SMAN -> Material::
	// MaxSmoothingAngle -> the MeshOps registry, sidecar-elim §1.5). Seeds only
	// surfaces whose authored SMAN differs from greets' 89.5° global default (the
	// value every stock Piramid.lwo surface carries), reproducing the retired
	// `surface|smoothAngle|value` sidecar lines from the source. Runs before
	// MakeFacesIndependentByAngle below (which consumes the registry). INERT
	// until a surface's SMAN is authored away from 89.5° (registry stays empty).
	{
		const int nSeed = MeshOps_SeedAuthoredSmoothAngles(GreetSc, 89.5f, 0.05f);
		if (nSeed)
			std::fprintf(stderr, "[GREETS] seeded %d authored per-surface smoothing "
			             "angle(s) from native SMAN (sidecar-elim §1.5)\n", nSeed);
	}

	// CLI material import (--material-import=MAT:DIR): overlay PBR map folders
	// onto named materials for testing. After Preprocess (textures loaded), so
	// it replaces the loaded maps directly. No-op without the flag.
	fds::MaterialImport_Apply(GreetSc, "greets");

	// Floor de-tile + scale (face UVs exist now that Preprocess ran).
	if (fds::FeatureFlags::greets_stone_tex())
		GreetsRetileFloor(GreetSc);

	// also make the appropriate Layer 2 fillers,

	GreetSc->Ambient.R = 32;
	GreetSc->Ambient.G = 32;
	GreetSc->Ambient.B = 32;

	GreetSc->PathingMinVelocity = 0.01;

	// Geometry fix for the mummies ('momy-1' + its mirror 'momy-2'):
	// Phong-tessellate the coarse lathes to round the silhouette. DEFAULT OFF
	// (FDS_MOMY_SUBDIV=N to enable). The old "cracks at UV/duplicate-vertex
	// seams" note here was a MISDIAGNOSIS — the lathe is index-closed (no
	// unshared coincident verts; measured via FDS_SUBDIV_DIAG, boundary-edges
	// raw=0), and the holes were the unguarded Phong projection inverting
	// sub-triangles at the cap rim / pole where endpoint normals disagree.
	// Fixed 2026-07-30 in SubdivideMaterialFaces (crease-guarded projection);
	// SUBDIV=2 renders closed at the deterministic momy close-cam
	// (FDS_GREETS_CAM="-12.1,3.2,-27,0,-0.06,-1", which frames momy-2 — both
	// mummies are subdivided so that pose is a valid gate).
	{
		const char *lv = std::getenv("FDS_MOMY_SUBDIV");
		const int levels = lv ? std::atoi(lv) : 0;
		if (levels > 0) {
			SubdivideMaterialFaces(GreetSc, "momy-1", levels);
			SubdivideMaterialFaces(GreetSc, "momy-2", levels);
		}
	}

	// Stone displacement workstream (docs/ENVDYN_DISPLACEMENT_PLAN.md, B):
	// tessellate the wall ('rooms') + floor ('floor') for the height-map
	// displacement bake — LINEAR midpoints (pure density; PN rounding would
	// bow the flat walls near corners where smoothed vertex normals tilt) —
	// then (--greets_displace) push interior verts along their smooth normals
	// by the low-mip height sample. Same hook point as the momy subdiv:
	// per-vertex normals exist (Preprocess), per-face UVs are final
	// (GreetsRetileFloor ran), MakeFacesIndependentByAngle below re-derives
	// vertex normals + tangents on the displaced mesh, and the Piramid chunk
	// split further down wraps the displaced verts in its chunk bounds.
	// Defaults (subdiv 0, displace 0) = neither call runs (byte-identical).
	// S1 offscreen shadow proxy: captured here (rooms/floor still FLAT), built
	// after the displace/smooth passes below, finalized after the clustering.
	InitTimelineMark("greets: displace block begin");
	StoneProxySnapshot g_stoneProxySnap;
	TriMesh *g_stoneProxyMesh = nullptr;
	{
		const int  L        = fds::FeatureFlags::greets_stone_subdiv();
		const bool displace = fds::FeatureFlags::greets_displace();
		if (displace && fds::FeatureFlags::greets_shadow_proxy())
			CaptureStoneProxySnapshot(GreetSc, g_stoneProxySnap);
		if (!displace && L > 0) {
			// Legacy pure-density knob (no bake): linear midpoint split only.
			SubdivideMaterialFaces(GreetSc, "rooms", L, /*phong=*/false);
			SubdivideMaterialFaces(GreetSc, "floor", L, /*phong=*/false);
		}
		if (!displace && fds::FeatureFlags::displace_viz())
			std::fprintf(stderr, "[DISPLACE-VIZ] --displace_viz is on but "
				"--greets_displace is OFF — no bake runs, so the overlay has "
				"nothing to draw. Add --greets_displace.\n");
		if (displace) {
			const float amp   = fds::FeatureFlags::greets_displace_amp();
			const int   mip   = fds::FeatureFlags::greets_displace_mip();
			const float adapt = fds::FeatureFlags::greets_displace_adapt();
			const float cpb   = fds::FeatureFlags::greets_displace_cpb();
			// Symmetric adaptive subdivision + displacement in ONE pass (S1/S2
			// of the diagonal-grain fix — the old linear-subdiv + interior-
			// displace pair put a roof-ridge on every stone quad's diagonal).
			// greets_stone_subdiv > 0 forces that UNIFORM level (the cost-
			// comparison baseline); 0 = adaptive 0..3 by height-map busyness.
			// The displaced-material set (rooms+floor) lets the cross-material
			// seam pin (--greets_displace_neighbor_pin) tell a NON-displaced
			// neighbour (siling/lintel — pin against it) from the sibling
			// displaced junction (rooms<->floor — leave relieved).
			static const std::vector<std::string> kDisplacedMats = { "rooms", "floor" };
			DisplaceStoneSubdiv(GreetSc, "rooms", L, amp, mip, adapt, cpb, &kDisplacedMats);
			DisplaceStoneSubdiv(GreetSc, "floor", L, amp, mip, adapt, cpb, &kDisplacedMats);
			// B4 residual: geometry now carries the low band of the relief, so
			// POM must march the RESIDUAL (full − upsampled low band) or the
			// blocks get counted twice (geometry bump + parallax shift of the
			// same edges). Degenerate maps return null (the bake skipped them
			// — original map stays, e.g. the all-white shipping wall map).
			// Cone POM's map is rebaked from the residual heights when active.
			for (Material *M = MatLib; M; M = M->Next) {
				if (M->RelScene != GreetSc || !M->Name || !M->HeightMap) continue;
				if (std::strcmp(M->Name, "rooms") && std::strcmp(M->Name, "floor"))
					continue;
				if (Texture *resid = MakeResidualHeight(M->HeightMap, mip)) {
					M->HeightMap = resid;   // original leaks (init-time, matches loaders)
					if (M->ConeMap) {
						const int coneExact = fds::FeatureFlags::pom_cone_exact();
						M->ConeMap = coneExact > 0
						    ? LoadOrBakeConeMapExact(M->HeightMap, coneExact, M->Name)
						    : MakeConeMap(M->HeightMap);
						std::fprintf(stderr, "[DISPLACE] '%s' cone map rebaked "
						             "from the residual (exact=%d)\n", M->Name, coneExact);
					}
					std::fprintf(stderr, "[DISPLACE] '%s' POM input -> residual "
					             "height map\n", M->Name);
				}
			}
		}
	}

	// Auto-detect creased meshes (e.g. wall panels) and switch them to
	// per-face vertex normals; smooth meshes (curved letters) untouched.
	InitTimelineMark("greets: DisplaceStoneSubdiv block end");
	MakeFacesIndependentByAngle(GreetSc, 30.0f);
	InitTimelineMark("greets: MakeFacesIndependentByAngle end");

	// Displaced stone: re-SMOOTH the displaced surface normals. The pass above
	// (MakeFacesIndependentByAngle, 30°) facets the new relief — adjacent cell
	// faces exceed the crease threshold, split, and the triangulation shows as
	// hairline/facet seams (plus a split tangent basis → normal-map noise). A
	// scene-wide position-bucket weld averages the displaced face normals within
	// greets_displace_smooth degrees, so shading + tangents go continuous and
	// the seams vanish, while authored 90° corners (and material borders) stay
	// hard. No-op when --greets_displace is off.
	if (fds::FeatureFlags::greets_displace()) {
		const float sm = fds::FeatureFlags::greets_displace_smooth();
		DisplaceStoneSmoothNormals(GreetSc, "rooms", sm);
		DisplaceStoneSmoothNormals(GreetSc, "floor", sm);
	}

	// S1b PER-PIXEL SHELL (--pom_shell, docs/S1_PIXEL_DISPLACEMENT_PLAN.md):
	// push the stone out to the LID of a relief slab so the POM march has
	// something to enter and can DISCARD rays that leave the patch (= real
	// block-edge silhouettes with the geometry behind showing through). Runs
	// here because vertex normals are final (MakeFacesIndependentByAngle above
	// split the creased walls per face, so each wall's verts are its own) and
	// the Piramid chunk split further down copies Vertex/Face structs wholesale.
	// The campaign's per-pixel mode is --no-greets_displace (the march carries
	// the FULL height map); combined with --greets_displace the shell would sit
	// on the already-displaced surface with the RESIDUAL map as its input, which
	// is a different (unvalidated) configuration — warn but allow.
	if (fds::FeatureFlags::pom_shell()) {
		if (fds::FeatureFlags::greets_displace())
			std::fprintf(stderr, "[POM-SHELL] WARNING: --pom_shell together with "
				"--greets_displace — the shell is being built on the DISPLACED "
				"surface and marches the RESIDUAL map. The campaign's per-pixel "
				"mode is --no-greets_displace.\n");
		const bool pin = fds::FeatureFlags::pom_shell_pin();
		// --pom_shell_weld=3 (S1d-2e): PomShell_Build runs once per material,
		// so its position-bucket weld can only ever average normals WITHIN the
		// material being built and the wall/floor corners still tear. Snapshot
		// the pristine mesh ONCE here, across every material about to be
		// shelled, so both sides of a junction extrude along one direction and
		// land on one point. No-op (and byte-null) below --pom_shell_weld=3.
		const char *weldMats[2] = { nullptr, nullptr }; int nWeldMats = 0;
		{
			for (const char *nm : { "rooms", "floor" })
				for (Material *m = MatLib; m; m = m->Next)
					if (m->RelScene == GreetSc && m->Name && !std::strcmp(m->Name, nm)
					    && m->HeightMap) { weldMats[nWeldMats++] = nm; break; }
			PomShell_WeldPrepare(GreetSc, weldMats, nWeldMats);
			// R2: --pom_shell_slit_census. Pre-build position snapshot, so the
			// census below can attribute the delta each POSITION actually
			// received without instrumenting PomShell_Build itself. No-op and
			// byte-null with the flag off.
			PomShell_SlitSnapshot(GreetSc);
			// ── S1d-3 PRISM (--pom_prism, default 0 = OFF) ── BEGIN
			// Same pre-build snapshot, kept separately because the prism's
			// consumer runs much later (after the ::mirUV split) while the slit
			// census consumes and frees its own immediately below.
			PomShell_PrismSnapshot(GreetSc);
			// ── S1d-3 PRISM ── END
		}
		for (const char *nm : { "rooms", "floor" }) {
			Material *M = nullptr;
			for (Material *m = MatLib; m; m = m->Next)
				if (m->RelScene == GreetSc && m->Name && !std::strcmp(m->Name, nm)) { M = m; break; }
			if (!M || !M->HeightMap) {
				std::fprintf(stderr, "[POM-SHELL] '%s': no height map — no shell "
					"(needs --greets_stone_tex + --parallax)\n", nm);
				continue;
			}
			// The amplitude the march runs at, consumed once here.
			PomShell_Build(GreetSc, nm,
			               fds::FeatureFlags::parallax_strength() * M->ParallaxScale, pin);
		}
		PomShell_SlitCensus(GreetSc, weldMats, nWeldMats);
		PomShell_WeldReset();
	}

	// S1 offscreen proxy: NOW that the displace + face-independence + smooth-weld
	// passes are done, (1) tag every displaced rooms/floor face main-only so the
	// offscreen/bake passes skip the ~81k detail, and (2) build the flat proxy
	// mesh from the pre-displace snapshot so those passes rasterise the ~226 flat
	// faces instead. The tag propagates through the Piramid chunk split (per-face
	// struct copy). Both no-op unless --greets_displace captured a snapshot.
	if (g_stoneProxySnap.captured) {
		int tagged = 0;
		for (Object *Obj = GreetSc->ObjectHead; Obj; Obj = Obj->Next) {
			if (Obj->Type != Obj_TriMesh || !Obj->Name) continue;
			if (!std::strstr(Obj->Name, "Piramid")) continue;
			TriMesh *T = (TriMesh*)Obj->Data;
			for (Face *F = T->Faces; F < T->Faces + T->FIndex; ++F) {
				if (!F->Txtr || !F->Txtr->Name) continue;
				if (!std::strcmp(F->Txtr->Name, "rooms") ||
				    !std::strcmp(F->Txtr->Name, "floor")) { F->Flags |= Face_MainOnly; ++tagged; }
			}
		}
		g_stoneProxyMesh = BuildStoneProxyMesh(GreetSc, g_stoneProxySnap);
		std::fprintf(stderr, "[STONE-PROXY] tagged %d displaced rooms/floor faces "
			"Face_MainOnly (offscreen passes use the flat proxy)\n", tagged);
	}

	// Specular/Glossiness are AUTHORED IN THE LWO SOURCES now (Authoring/
	// greets — stairs/amudim/floor/hull/cockpit at spec 0.4 gloss 48, the
	// marble 'rooms' wall at spec 0.1 gloss 64; see tools/lwopatch.py). The
	// old code-side bumps that lived here were baked into the .lwo files and
	// deleted, so the browser editor's LWO write-back is authoritative — a
	// saved specular no longer gets stomped at the next scene load. The name
	// lists remain only to pick which surfaces get a Sobel-baked normal map.
	// THE SOBEL NORMAL-MAP NAME GATE IS GONE (2026-08-09, user's call:
	// "fuck the string match - it was a temp measure anyway - remove").
	//
	// It used to bake a tangent-space normal map FROM THE DIFFUSE for any
	// material whose NAME matched a substring list, which is a guess dressed
	// as authoring: it turned the mech's camouflage PAINT into geometric
	// dents (docs/SHADING_CONTRACT.md §11 row E8 — hull pixel (767,723) read
	// Y 131.2 shipped against 45.0 without it and 41.0 on the Metal arm).
	//
	// By the end it only reached 'siling'; every other name on the list was
	// already served by a real map through the stone-tex or RVSM path and
	// skipped by the `!M->NormalMap` guard. So nothing here is load-bearing.
	//
	// A surface that WANTS relief now gets it the one honest way: an authored
	// TEXTURES/PBR/<set>/normal.png reached by the LWO 'RVSM' sub-chunk, which
	// MaterialImport_ApplyRevMaps (DEMO/MaterialImport.cpp) loads with no code
	// change — the same route 'rooms', 'stairs' and 'amudim' already take.
	// BakeNormalMapFromDiffuse itself is kept: the editor still offers it as
	// an explicit per-surface action, which is authoring, not inference.

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
		// Materials whose faces should collapse to ONE shadow cluster regardless
		// of facet orientation. The per-plane clustering above is correct for flat
		// walls (distinct walls cross-shadow, coplanar faces self-match) but WRONG
		// for a smooth curved surface: every facet gets a different plane key →
		// a unique ShadowMatID per facet → at each poly edge the light-view and
		// camera-view faces carry different ids → the PolyId equality test reports
		// a spurious self-shadow → a dark line on every facet edge. The mummy
		// The two mummies ('momy-1' and 'momy-2') are identical curved lathes in
		// Piramid.lwo (mirror copies, 336 polys each), so force each whole surface
		// to a single id (it then self-matches everywhere → no acne lines). BOTH
		// belong here: the special-case is per-SURFACE (keyed on the material name
		// per face), and the 2026-07-30 split-bake made the second mummy a distinct
		// surface — without membership its facets get per-plane ids → the per-facet
		// self-shadow acne this list exists to kill. Same idea as the hull/robot
		// single-id merge. Add curved alcove mats here when we tackle those
		// (concave, so it's a softer trade-off — real self-shadowing inside the
		// alcove is lost along with the artifact).
		std::set<std::string> kSingleShadowIdMats = { "momy-1", "momy-2" };
		// Displaced stone: the per-(material, PLANE) clustering below assumes the
		// wall is coplanar (one flat 'rooms' plane => one ShadowMatID => no self-
		// shadow). Displacement tilts/offsets EVERY facet onto its own plane, so
		// each facet would get a UNIQUE ShadowMatID and self-shadow its neighbours
		// — the per-facet hairline acne on the subdivided walls (worst at grazing).
		//
		// PARENT-PLANE inheritance (--greets_displace_shadow_planes, default on):
		// key each displaced facet by its PARENT (pre-displacement authored)
		// plane — the bake stamped the parent's registry ordinal into
		// F->ShadowMatID (transient tag; see MeshOps_StoneParentPlane). Facets of
		// one flat wall share one id (no per-facet acne, same as the collapse
		// WITHIN a wall) but DIFFERENT walls get different ids, so an occluding
		// wall shadows a receiver wall again. The previous whole-material
		// single-id collapse made every rooms-vs-rooms occlusion self-match in
		// the deferred PolyId identity test — an omni behind a wall lit the next
		// wall straight through it (the reported light bleed). --no- restores
		// the collapse (momy-style) for A/B.
		const bool planeInherit = fds::FeatureFlags::greets_displace() &&
		                          fds::FeatureFlags::greets_displace_shadow_planes();
		if (fds::FeatureFlags::greets_displace() && !planeInherit) {
			kSingleShadowIdMats.insert("rooms");
			kSingleShadowIdMats.insert("floor");
		}
		auto isDisplacedStone = [&](const Material *M) {
			return planeInherit && M && M->Name &&
			       (!std::strcmp(M->Name, "rooms") || !std::strcmp(M->Name, "floor"));
		};

		int totalClusters = 0;
		int totalTaggedFaces = 0;
		// all meshes' clusters (the proxy stamping below needs cross-mesh lookup)
		std::map<std::pair<Material*, PlaneKey>, uint16_t> allClusters;
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
				// Curved single-id materials: constant key so every facet of the
				// surface lands in ONE cluster (one ShadowMatID) → self-matches
				// everywhere, killing the per-facet self-shadow acne lines.
				const bool singleId = src->Name && kSingleShadowIdMats.count(src->Name) > 0;
				PlaneKey key;
				if (const StoneParentPlane *pp = isDisplacedStone(src)
				        ? MeshOps_StoneParentPlane(src->Name, F->ShadowMatID) : nullptr) {
					// displaced stone: cluster by the PARENT authored plane the
					// bake stamped (facet planes are per-facet noise).
					key = PlaneKey{ quantize(pp->nx, kNormalQ),
					                quantize(pp->ny, kNormalQ),
					                quantize(pp->nz, kNormalQ),
					                quantize(pp->d,  kDistanceQ) };
				} else if (singleId || (isDisplacedStone(src) && F->ShadowMatID == 0)) {
					// single-id collapse; also the unstamped-displaced fallback
					// (degenerate parent normal) so those faces still self-match.
					key = PlaneKey{ 0, 0, 0, 0 };
				} else {
					key = PlaneKey{ quantize(nx, kNormalQ),
					                quantize(ny, kNormalQ),
					                quantize(nz, kNormalQ),
					                quantize(d,  kDistanceQ) };
				}
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
			allClusters.insert(clusters.begin(), clusters.end());
		}
		std::fprintf(stderr,
			"[PIRAMID-SPLIT] total clusters: %d  tagged faces: %d  "
			"(per-face F->ShadowMatID; no Material clones)\n",
			totalClusters, totalTaggedFaces);
		// S1 offscreen proxy under parent-plane inheritance: the flat proxy IS
		// the parent geometry, so each proxy face's OWN authored plane quantizes
		// to exactly the parent key its displaced facets clustered under — stamp
		// per face by lookup (the id must match per WALL now, not per material).
		if (g_stoneProxyMesh && planeInherit) {
			int hit = 0, miss = 0;
			for (Face *F = g_stoneProxyMesh->Faces;
			     F < g_stoneProxyMesh->Faces + g_stoneProxyMesh->FIndex; ++F) {
				if (!F->Txtr || !isDisplacedStone(F->Txtr)) continue;
				const float len = std::sqrt(F->N.x*F->N.x + F->N.y*F->N.y + F->N.z*F->N.z);
				if (!(len > 1e-4f)) continue;
				const float inv = 1.0f / len;
				// same d convention as the bake registry: n̂·A = −NormProd/|N|
				const float d = -F->NormProd * inv;
				const PlaneKey key{ quantize(F->N.x*inv, kNormalQ),
				                    quantize(F->N.y*inv, kNormalQ),
				                    quantize(F->N.z*inv, kNormalQ),
				                    quantize(d, kDistanceQ) };
				auto it = allClusters.find(std::make_pair(F->Txtr, key));
				if (it != allClusters.end()) { F->ShadowMatID = it->second; ++hit; }
				else ++miss;
			}
			std::fprintf(stderr, "[STONE-PROXY] parent-plane ids stamped onto the "
				"proxy caster: %d matched, %d unmatched\n", hit, miss);
		}
	}
	// S1 offscreen proxy (single-id collapse path): stamp the displaced walls'
	// single shadow cluster onto the flat proxy caster so the deferred PolyId
	// self-shadow test matches between the displaced G-buffer pixels and the
	// flat occluder. Must run AFTER the clustering above assigned the ids.
	// No-op unless the proxy was built; the parent-plane path stamped per-face
	// ids inside the block above instead.
	if (g_stoneProxyMesh && !(fds::FeatureFlags::greets_displace() &&
	                          fds::FeatureFlags::greets_displace_shadow_planes()))
		FinalizeStoneProxy(GreetSc, g_stoneProxyMesh);
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
		// Phase A (docs/VISIBILITY_PLAN.md §7): size-based granularity override.
		// >0 = near-cubic cells of ~chunkSize world units per side (resolution-
		// independent, so the cell size doesn't grow 20x under displacement). 0 =
		// legacy uniform N^3 grid (byte-identical). See greets_chunk_size help.
		const float chunkSize = fds::FeatureFlags::greets_chunk_size();
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
		// fds::DeferredPathEnabled(), NOT FeatureFlags::deferred(): the user's
		// interactive greets run reaches the deferred kernel via ForceDeferred /
		// shard_deferred without the bare --deferred flag ever being set, so this
		// test answered "forward" and skipped the split on the very path that
		// needs it — 59 556 displaced faces never got NoShadowCast, i.e. 15.10 ms
		// of shadow bake per frame. One predicate, defined once in RENDER.CPP.
		if ((chunkGrid > 0 || chunkSize > 0.0f) && fds::DeferredPathEnabled()) {
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
				// Per-axis cell counts. chunkSize==0 -> gx=gy=gz=chunkGrid, which
				// makes cellOf + the index formula BYTE-IDENTICAL to the legacy
				// uniform-N^3 path (same int() truncation, same clamp, same
				// (iz*N+iy)*N+ix). chunkSize>0 -> near-cubic cells sized in world
				// units, independent of the mesh's face count (Phase A).
				int gx = chunkGrid, gy = chunkGrid, gz = chunkGrid;
				if (chunkSize > 0.0f) {
					gx = std::max(1, int(std::ceil(spanX / chunkSize)));
					gy = std::max(1, int(std::ceil(spanY / chunkSize)));
					gz = std::max(1, int(std::ceil(spanZ / chunkSize)));
				}
				auto cellOf = [&](float fx, float fy, float fz) -> int64_t {
					int ix = std::clamp(int((fx - xmin) / spanX * float(gx)),
					                    0, gx - 1);
					int iy = std::clamp(int((fy - ymin) / spanY * float(gy)),
					                    0, gy - 1);
					int iz = std::clamp(int((fz - zmin) / spanZ * float(gz)),
					                    0, gz - 1);
					return (int64_t(iz) * gy + iy) * gx + ix;
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
				// --vis_stats: per-chunk face-count histogram (Phase A census).
				if (fds::FeatureFlags::vis_stats()) {
					// pow2 buckets: [1,2) [2,4) ... [2^k, 2^(k+1))
					int hist[16] = {0};
					size_t fmin = SIZE_MAX, fmax = 0, ftot = 0;
					for (const auto& kv : cellFaces) {
						const size_t n = kv.second.size();
						if (!n) continue;
						fmin = std::min(fmin, n); fmax = std::max(fmax, n); ftot += n;
						int b = 0; size_t v = n;
						while (v > 1 && b < 15) { v >>= 1; ++b; }
						++hist[b];
					}
					std::fprintf(stderr, "[CHUNK-HIST] cells=%zu faces min/mean/max=%zu/%.0f/%zu buckets(pow2):",
					    cellFaces.size(), fmin, cellFaces.empty()?0.0:double(ftot)/cellFaces.size(), fmax);
					for (int b = 0; b < 16; ++b)
						if (hist[b]) std::fprintf(stderr, " [%d-%d):%d", 1<<b, 1<<(b+1), hist[b]);
					std::fprintf(stderr, "\n");
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
					// S1: count displaced (Face_MainOnly) faces. A chunk that is
					// ENTIRELY displaced rooms/floor casts no shadow itself (the
					// flat proxy does) — so mark it Tri_NoShadowCast to skip its
					// per-vertex transform in the shadow bake too (the per-face
					// skip alone only drops the raster, not Phase-A transform).
					DWord mainOnlyFaces = 0;
					for (DWord i = 0; i < chunkFaces; ++i) {
						const Face &srcF = piramid->Faces[faceIndices[i]];
						Face &dstF = chunk->Faces[i];
						dstF = srcF;
						if (srcF.Flags & Face_MainOnly) ++mainOnlyFaces;
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
					float wmnx=FLT_MAX, wmny=FLT_MAX, wmnz=FLT_MAX;
					float wmxx=-FLT_MAX, wmxy=-FLT_MAX, wmxz=-FLT_MAX;
					for (DWord i = 0; i < chunkVerts; ++i) {
						MatrixXVector(chunk->RotMat, &chunk->Verts[i].Pos,
						              &chunk->worldVerts[i]);
						Vector_SelfAdd(&chunk->worldVerts[i], &chunk->IPos);
						const Vector &w = chunk->worldVerts[i];
						if (w.x<wmnx) wmnx=w.x; if (w.x>wmxx) wmxx=w.x;
						if (w.y<wmny) wmny=w.y; if (w.y>wmxy) wmxy=w.y;
						if (w.z<wmnz) wmnz=w.z; if (w.z>wmxz) wmxz=w.z;
					}
					// World + local AABB for the Phase-B chunk-occlusion cull
					// (docs/VISIBILITY_PLAN.md §7). Derived from worldVerts (the
					// exact transform the shipping cube-face cull uses), and marked
					// valid so WorldAabb_UpdateScene leaves these static boxes alone.
					// Inert unless --chunk_occlusion reads them.
					chunk->WorldAabbMin = { wmnx, wmny, wmnz };
					chunk->WorldAabbMax = { wmxx, wmxy, wmxz };
					chunk->WorldAabbValid = 1;
					chunk->LocalAabbMin = { cxmin, cymin, czmin };
					chunk->LocalAabbMax = { cxmax, cymax, czmax };
					chunk->LocalAabbValid = 1;
					chunk->Flags = piramid->Flags;
					// Tri_Possessed: skip TriMeshHead loop in Animate_Objects.
					// Without it, Spline_Calc_3D on the chunk's empty
					// (NumKeys=0) Pos spline derefs null Keys and hangs.
					chunk->Flags |= Tri_Possessed;
					// S1: a chunk that is 100% displaced rooms/floor doesn't cast
					// its own shadow — the flat proxy does. Skip the whole chunk
					// (incl. its per-vertex transform) from the shadow bake. Mixed
					// chunks (some non-target siling/momy) still cast; their
					// displaced faces are dropped per-face by Face_MainOnly.
					if (mainOnlyFaces > 0 && mainOnlyFaces == chunkFaces) {
						chunk->Flags |= Tri_NoShadowCast;
						++g_s1PureChunks; g_s1PureChunkFaces += int(chunkFaces);
					} else if (mainOnlyFaces > 0) {
						++g_s1MixedChunks; g_s1MixedMainFaces += int(mainOnlyFaces);
					}
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
				if (chunkSize > 0.0f)
					std::fprintf(stderr,
					    "[PIRAMID-CHUNKS] size=%.2f (grid=%dx%dx%d) → %d chunks  "
					    "(orig %u faces / %u verts → %u total chunk verts; "
					    "%u faces with null vertex skipped)\n",
					    chunkSize, gx, gy, gz, chunksCreated,
					    unsigned(origFaces), unsigned(origVerts),
					    unsigned(totalChunkVerts), unsigned(degen));
				else
					std::fprintf(stderr,
					    "[PIRAMID-CHUNKS] grid=%d³ → %d chunks  "
					    "(orig %u faces / %u verts → %u total chunk verts; "
					    "%u faces with null vertex skipped)\n",
					    chunkGrid, chunksCreated,
					    unsigned(origFaces), unsigned(origVerts),
					    unsigned(totalChunkVerts), unsigned(degen));
				if (g_stoneProxyMesh)
					std::fprintf(stderr, "[STONE-PROXY] shadow-skip census: %d pure "
					    "displaced chunks (%d faces) NoShadowCast; %d mixed chunks "
					    "keep %d displaced faces (Phase-A transform still paid)\n",
					    g_s1PureChunks, g_s1PureChunkFaces, g_s1MixedChunks, g_s1MixedMainFaces);
			}
		}
	}
	// Split mirrored-UV faces onto handedness clones (after all robot/face
	// retargeting, before the matID rebuild so the clones get IDs).
	InitTimelineMark("greets: chunking/clustering end");
	GreetsFixBitangentHandedness(GreetSc);
	// ── S1d-3 PRISM (--pom_prism, default 0 = OFF) ── BEGIN
	// Here, and not next to PomShell_Build, for two reasons: the ::mirUV split
	// above has just assigned each shelled face its final handedness material,
	// which the side quads copy verbatim; and the mirror clone build below has
	// not run yet, so the new faces are cloned into the mirrors like any other.
	// The chunk split above has already run, but its chunks all carry Piramid's
	// transform, so one side mesh in that space is exact.
	if (fds::FeatureFlags::pom_shell() && fds::FeatureFlags::pom_prism() > 0) {
		static const char *kPrismMats[2] = { "rooms", "floor" };
		PomShell_BuildPrism(GreetSc, kPrismMats, 2);
	}
	// ── S1d-3 PRISM ── END
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

	// Disco ball BEFORE the shadow-map allocation block (its rotating
	// spots cast per-frame shadows — created later they'd never get
	// map slots and beams shine through geometry) and before the
	// mirror build (the teleporter mirror clones the ball; its 2-key
	// Pos spline marks it dynamic → re-mirrored per frame).
	fds::BuildDiscoBall(GreetSc);
	InitTimelineMark("greets: BuildDiscoBall end");

	// Blaster-bolt test harness (--greets-blasters): build the bolt texture.
	if (fds::FeatureFlags::greets_blasters())
		fds::BlasterBolts_Init();

	// Shadow-map allocation + static cube bake + lightmap bake spawn.
	// MOVED here from GreetsScene::init() so it runs during glat (via
	// t1's Initialize_Greets) instead of waiting until the user reaches
	// the greets scene (which is after city/chase/fountain/crash playback).
	// The bake itself spawns its own thread and parallelizes with the
	// remaining scene playback.
	{
		// Omni range is AUTHORED (LWS "LightRange" → FLD Range envelope →
		// Omni::Range, evaluated per frame by Animate_Objects). Nothing
		// patches it by default any more — see the note in
		// SceneCorrections() for the two patches that used to.
		//
		// greets_omni_default_range survives only as an explicit tuning
		// dial, default 0 = inert. When > 0 it FORCES every Light_Omni's
		// range to that value: it rewrites the spline keys, because
		// writing IRange alone would be undone by the very next
		// Animate_Objects. Note this hits every Light_Omni present at
		// this point, which includes the disco-ball lights built just
		// above, not only the ten from the FLD.
		const float omniForceRange = fds::FeatureFlags::greets_omni_default_range();
		if (omniForceRange > 0.0f) {
			int forced = 0;
			for (Omni *O = GreetSc->OmniHead; O; O = O->Next) {
				if (O->Type != Light_Omni) continue;
				O->IRange = omniForceRange;
				O->rRange = 1.0f / omniForceRange;
				for (mword k = 0; k < O->Range.NumKeys && O->Range.Keys; ++k)
					O->Range.Keys[k].Pos.x = omniForceRange;
				++forced;
			}
			std::fprintf(stderr,
			    "[GREETS] --greets_omni_default_range: forced IRange=%g on %d omnis "
			    "(overriding the authored LWS ranges)\n", omniForceRange, forced);
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
		InitTimelineMark("greets: shadow-map block begin");
		ShadowMaps_Rebuild(GreetSc, 1024);
		InitTimelineMark("greets: ShadowMaps_Rebuild end");
		CubeShadowMaps_Rebuild(GreetSc, omniCubeRes);
		InitTimelineMark("greets: CubeShadowMaps_Rebuild end");
		// Force the static occluder bake even when --shadows isn't set yet:
		// greets turns --shadows on at RUN (GreetsApplyRunDefaults), AFTER this
		// init bake, so under bare --deferred shadows() is still off here. The
		// static-shadow lightmap bake spawned below is likewise force-enabled
		// (bakeForce) and READS these occluder maps; without the same force the
		// maps stay empty and every lightmap texel bakes 100% lit (no static
		// shadows). Same condition as bakeForce so the two stay in lockstep.
		ShadowMaps_BakeStatic(GreetSc, /*forceEnable=*/!GreetsScenePreempted());
		InitTimelineMark("greets: ShadowMaps_BakeStatic end");
		fds::LightmapStampOrigBary(GreetSc);
		InitTimelineMark("greets: LightmapStampOrigBary end");
		// NOTE: the lightmap bake thread is spawned LATER (after the mirror /
		// shatter setup below), not here — LightmapBake_Static reads Materials,
		// and BuildMirrorsByTextureName / BuildGreetsShatter rewrite the
		// Material table (Scene_RebuildMatTable). Spawning the bake before that
		// raced the read against the write (TSan, 2026-06-16).
	}

	// Build per-mirror clones. Each material name passed to BuildMirror
	// produces an independent Mirror (own plane, own clone mesh, own
	// cloned omnis). Multiple mirror surfaces share no state — the
	// Z-buffer + surrounding occluders sort out which mirror's
	// reflection shows at each pixel. Mirror-in-mirror (two facing)
	// would currently produce empty recursion past depth 1; if greets
	// ever needs that we'll add a depth cap.
	InitTimelineMark("greets: mirror build begin");
	if (fds::FeatureFlags::greets_mirror()) {
		g_greetsMirrors.clear();
		// One mirror per material name, plus one mirror per coplanar
		// CLUSTER of each texture filename. teleporter is matched by
		// name (a single material with no texture); P_TEXT.JPG is the
		// dynamic text texture shared across every greets display
		// screen — the screens face four directions at different
		// depths, so each coplanar cluster gets its own independent
		// mirror (plane, clone mesh, omni clones).
		const char *kMirrorMats[] = {
			"teleporter",  // back wall — opaque silver mirror
		};
		const char *kMirrorTextures[] = {
			"P_TEXT.JPG",  // greets text screens — half-silvered glass
		};
		// EXPLICIT mirror designation, authored on the LWO surface name.
		// Only these P_TEXT surfaces become reflective screens; the small
		// "amudim" screens ("screen 4") and column panels ("screen2") share
		// the P_TEXT texture but are NOT mirrors (they were spuriously caught
		// by the old texture+area heuristic). To make another screen a mirror,
		// add its surface name here (no LWO change needed — the name IS the
		// mark; rename the surface in Piramid.lwo if you want a distinct one).
		const std::vector<std::string> kMirrorScreenMats = {
			"screen 3",   // the big end display (far +x wall)
			"screen 4",   // the display opposite the teleporter (center-back)
			// "screen2" (central column panel) intentionally NOT a mirror.
		};
		for (const char *matName : kMirrorMats) {
			fds::Mirror m = fds::BuildMirror(GreetSc, matName);
			if (m.cloneMesh) g_greetsMirrors.push_back(std::move(m));
		}
		for (const char *txtrName : kMirrorTextures) {
			// rttSlots: sub-threshold clusters (the column panels)
			// become first-order RTT mirrors when --mirror-rtt is on.
			fds::BuildMirrorsByTextureName(GreetSc, txtrName,
			                               g_greetsMirrors,
			                               &g_greetsRttSlots,
			                               &kMirrorScreenMats);
		}
		// Tag original faces behind each mirror plane so room geometry
		// behind a (transparent) mirror can't leak through its footprint
		// and beat the reflected clones on Z.
		fds::TagFacesBehindMirrors(GreetSc, g_greetsMirrors);
		// --mirror-rtt: second-order reflections. One RTT slot per
		// (outer mirror, inner panel component); textures re-rendered
		// per frame in Run_Greets (RenderSecondOrderMirrors).
		fds::PrepareSecondOrderMirrorRtt(GreetSc, g_greetsMirrors,
		                                 g_greetsRttSlots);
		std::fprintf(stderr, "[GREETS-MIRROR] active mirrors: %zu\n", g_greetsMirrors.size());
		// Shatter shards for the largest wall screen — built hidden here so
		// GreetsScene::init's Polys count sizes FList for them.
		BuildGreetsShatter();
		// Apply the portal tint (#1) once at init so it's present on the
		// first frame and in the headless snapshot (which doesn't run the
		// tick where the per-frame re-apply lives).
		GreetsApplyLiveTints();
	}

	// Spawn the background lightmap bake AFTER all Material-table rewrites
	// (the mirror + shatter setup above call Scene_RebuildMatTable). The bake
	// thread reads Materials, so starting it earlier raced the read against
	// those writes (TSan: LightmapBake_Static vs Scene_RebuildMatTable). The
	// bake is still background — joined in Run_Greets before the first frame.
	// Unconditional (matches the prior behaviour); LightmapBake_Static no-ops
	// fast when --shadow-lightmap is off.
	{
		InitTimelineMark("greets: shadow/env bake end — spawning lightmap thread");
		std::fprintf(stderr, "[GREETS-BAKE] spawning lightmap bake thread\n");
		const auto bakeSpawnT = std::chrono::steady_clock::now();
		Scene * const greetScForBake = GreetSc;
		// Force the bake on: greets wants lightmaps, but the global
		// shadow_lightmap SAMPLE flag is now set at greets-RUN (so it doesn't
		// leak onto city/fountain), so the bake's own gate would otherwise
		// no-op here. Skip the force when greets won't render (preempted by
		// another --scene-X) — then the bake no-ops fast as before.
		const bool bakeForce = !GreetsScenePreempted();
		g_greetsBakeThread = std::thread([greetScForBake, bakeSpawnT, bakeForce]() {
			fds::LightmapBake_Static(greetScForBake, bakeForce);
			const auto bakeDoneT = std::chrono::steady_clock::now();
			const double ms = std::chrono::duration<double, std::milli>(
				bakeDoneT - bakeSpawnT).count();
			std::fprintf(stderr,
				"[GREETS-BAKE] lightmap bake done in %.1f ms (background)\n",
				ms);
		});
	}

	// --pom_rebuild (editor): snapshot the stone's PRISTINE geometry on the
	// FINAL rendered meshes (post chunk split, post mirror clone) and apply any
	// deferred shell mode through the same rebuild path the editor panel drives.
	// No-op without the flag. Last thing in init so the snapshot sees exactly
	// the vertices the rasterizer will read.
	rev::DisplaceRebuild_EndInit(GreetSc);
	InitTimelineMark("greets: Initialize_Greets END");
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
	int32_t partTime() const override { return CHPartTime; }
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
		// Unified-TBR transparent path (structural push: retire the legacy
		// per-mesh xpar peel). Greets never called TBR_Init — capability-wise
		// the TBR strips handle peeled multi-layer + additive (fountain), it
		// was only ever missing initialization here. Scn_SpriteTBR also
		// routes omni flares through the strip sort, which fixes the known
		// "flare draws over transparents" ordering bug (immediate flares
		// can only be fully-before or fully-after the xpar composite).
		// --no-deferred_unified_tbr restores the legacy peel exactly (no
		// SBufferHead, no flare divert).
		if (fds::FeatureFlags::deferred_unified_tbr()) {
			TBR_EnsureInit(GreetSc, 256);
			GreetSc->Flags |= Scn_SpriteTBR;
		}
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

		// Debug camera for headless snapshots. Two ways to aim it
		// (after View = CameraHead above, else it gets clobbered):
		//   * FDS_GREETS_CAM="px,py,pz,fx,fy,fz" — exact pose captured
		//     interactively via the F9 button ([GREETS-CAM] line).
		//   * else --greets-mirror-cam parks the camera in front of the
		//     first mirror's wall centroid (fallback).
		if (fds::FeatureFlags::greets_mirror_cam() || std::getenv("FDS_GREETS_CAM")) {
			const char *camEnv = std::getenv("FDS_GREETS_CAM");
			float px, py, pz, fx, fy, fz;
			bool posed = camEnv && std::sscanf(camEnv, "%f,%f,%f,%f,%f,%f",
			                                   &px, &py, &pz, &fx, &fy, &fz) == 6;
			// The scripted camera's IFOV is 0 at init (its FOV spline
			// hasn't been evaluated yet) — copying it gives CalcPersp a
			// degenerate (PerspX=inf) projection and a black frame. Use
			// the FOV spline's first key, fallback 75°.
			float fov = GreetSc->CameraHead->IFOV;
			if (fov < 1.0f && GreetSc->CameraHead->FOV.NumKeys > 0)
				fov = GreetSc->CameraHead->FOV.Keys[0].Pos.x;
			if (fov < 1.0f) fov = 75.0f;
			// Start from a FULL copy of the scripted camera so FC carries
			// every field the mirror reflection RTT reads (NZP/FZP, etc.) —
			// otherwise the teleporter clone reflection rendered black in
			// snapshots even though it's fine under the live scripted camera.
			if (GreetSc->CameraHead) FC = *GreetSc->CameraHead;
			if (posed) {
				FC.ISource = { px, py, pz };
				Vector look = { px + fx, py + fy, pz + fz };
				Kick_Camera(&FC.ISource, &look, 0.0f, FC.Mat);
				FC.IFOV = fov;
				CalcPersp(&FC);
				View = &FC;
				std::fprintf(stderr, "[GREETS-CAM] applied pose src=(%.2f,%.2f,%.2f)\n",
					px, py, pz);
			} else if (!g_greetsMirrors.empty()) {
				const fds::Mirror &m = g_greetsMirrors.front();
				const Vector c = m.plane.centroid;
				const Vector &N = m.plane.N;
				const float dist = 14.0f;
				FC.ISource = { c.x + N.x*dist, c.y + N.y*dist, c.z + N.z*dist };
				Vector look = c;
				Kick_Camera(&FC.ISource, &look, 0.0f, FC.Mat);
				FC.IFOV = fov;
				CalcPersp(&FC);
				View = &FC;
			}
		}

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

		// Mid-scene resize: the TBR strip count tracks YRes (no on_resize
		// hook here; the check is two compares when current).
		if (GreetSc->Flags & Scn_SpriteTBR) TBR_EnsureInit(GreetSc, 256);

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
			// Back on F3 (2026-08-06). It was parked on the untranslated ScF6
			// to free F3 for REV.CPP's TimerProc review scrub; that scrub is
			// gone (it fought SceneDriver for Timer and lost), so F3 is free
			// and this toggle is reachable again — in wasm too, now that
			// MainLoop's table translates F3. Feedback is the [SHADOW-MODE]
			// stderr line below; the old "Shadow: %s [F3]" screen overlay was
			// deleted, so nothing advertises a key that does not work.
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

		// Shared transport: pause (P/U), fast-forward/rewind (F2/F1), and
		// the race-free scene-time snapshot. See SceneDriver::tickSceneTimer.
		tickSceneTimer(TTrd, pause_mode);

		clearFrame();

		// PROFILER: ANIM phase. also includes Dynamic Camera manager
		prof.switchTo(PROF_ANIM);

		// Frame-pipeline Slice-0 decomposition (FDS_TAIL_PROF=1): how much
		// pure-serial tick-thread time precedes the render — that wall time
		// is the pipelining campaign's whole ceiling.
		long long _tickT = TailProf::nowNs();

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
		TailProf::mark("Tick-Anim", _tickT, 3); _tickT = TailProf::nowNs();

		// Mirror shatter (Y key): bake the room into a panorama from the
		// screen's centre, break it into reflecting shards, and close the
		// mirror. The physics step runs AFTER Animate_Objects (which resets
		// each shard to its rest pose) and BEFORE Transform_Objects.
		{
			static bool sYPrev = false;
			// FDS_GREETS_SHATTER=1 auto-breaks on the first eligible tick
			// (headless verification — the snapshot can't inject keys).
			static const bool sAuto = std::getenv("FDS_GREETS_SHATTER") != nullptr;
			const bool yNow = Keyboard[ScY] != 0 || sAuto;
			if (yNow && !sYPrev && g_greetsShatter.built()
			    && !g_greetsShatter.active() && g_greetsShatterMirror >= 0) {
				fds::Mirror &m = g_greetsMirrors[g_greetsShatterMirror];
				// Light now: the per-shard reflection bakes forward (reads
				// per-vertex LR/LG/LB). The tick's Lighting() runs later, so
				// the first (prime) pass would otherwise bake the room black.
				Lighting(GreetSc);
				g_greetsShatter.trigger(0.5f, 0.55f);
				// Build the world→text-UV affine from one screen wall face
				// (its world corners + authored P_TEXT UVs) so the shards carry
				// the screen's text, and find the live P_TEXT texture.
				Texture *ptext = nullptr;
				float A[8] = {0};
				bool haveText = false;
				if (!m.wallFaces.empty()) {
					for (Material *Mt = MatLib; Mt; Mt = Mt->Next)
						if (Mt->RelScene == GreetSc && Mt->Txtr && Mt->Txtr->FileName
						    && !strcmp(Mt->Txtr->FileName, "TEXTURES/P_TEXT.JPG")) { ptext = Mt->Txtr; break; }
					const Face *F = m.wallFaces[0];
					TriMesh *T = m.wallFaceMeshes.empty() ? nullptr : m.wallFaceMeshes[0];
					auto wp = [&](const Vertex *v) {
						Vector lp = v->Pos, w; if (T) { MatrixXVector(T->RotMat, &lp, &w); w += T->IPos; } else w = lp; return w;
					};
					// Plane basis.
					Vector N = m.plane.N; { float l=std::sqrt(N.x*N.x+N.y*N.y+N.z*N.z); if(l>1e-6f){N.x/=l;N.y/=l;N.z/=l;} }
					Vector ref = (std::fabs(N.y) > 0.9f) ? Vector{1,0,0} : Vector{0,1,0};
					auto cr = [](const Vector&a,const Vector&b){ return Vector{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; };
					auto nz = [](Vector v){ float l=std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z); if(l>1e-6f){v.x/=l;v.y/=l;v.z/=l;} return v; };
					Vector U = nz(cr(ref,N)), V = nz(cr(N,U));
					// Negate U (after V is derived) so the text is mirrored
					// horizontally only: the shards display through the
					// reflected camera (L-R flipped), so the painted-on text
					// must be globally mirrored to read forward in the main view.
					U = Vector{-U.x,-U.y,-U.z};
					const Vector w0=wp(F->A), w1=wp(F->B), w2=wp(F->C);
					const float p0u=w0.x*U.x+w0.y*U.y+w0.z*U.z, p0v=w0.x*V.x+w0.y*V.y+w0.z*V.z;
					const float p1u=w1.x*U.x+w1.y*U.y+w1.z*U.z, p1v=w1.x*V.x+w1.y*V.y+w1.z*V.z;
					const float p2u=w2.x*U.x+w2.y*U.y+w2.z*U.z, p2v=w2.x*V.x+w2.y*V.y+w2.z*V.z;
					const float d1u=p1u-p0u,d1v=p1v-p0v,d2u=p2u-p0u,d2v=p2v-p0v;
					const float det=d1u*d2v-d2u*d1v;
					if (ptext && std::fabs(det) > 1e-9f) {
						const float inv=1.0f/det;
						auto solve=[&](float t0,float t1,float t2,float&a0,float&a1,float&a2){
							a0=((t1-t0)*d2v-(t2-t0)*d1v)*inv;
							a1=((t2-t0)*d1u-(t1-t0)*d2u)*inv;
							a2=t0-a0*p0u-a1*p0v;
						};
						float au0,au1,au2, av0,av1,av2;
						solve(F->U1,F->U2,F->U3, au0,au1,au2);
						solve(F->V1,F->V2,F->V3, av0,av1,av2);
						// tu(world) = au0*(world·U) + au1*(world·V) + au2.
						A[0]=au0*U.x+au1*V.x; A[1]=au0*U.y+au1*V.y; A[2]=au0*U.z+au1*V.z; A[3]=au2;
						A[4]=av0*U.x+av1*V.x; A[5]=av0*U.y+av1*V.y; A[6]=av0*U.z+av1*V.z; A[7]=av2;
						haveText = true;
					}
				}
				// Each shard becomes its own live reflection camera. NOTE: the
				// bake currently renders FORWARD (unshadowed Gouraud off the
				// static T->SL bake), which diverges in HUE from the shadowed
				// deferred main view (greens the greets room). Gain only dims,
				// it can't fix the hue — the real fix is a deferred (shadowed)
				// bake, pending the render-state declobbering. Full gain for now.
				g_greetsShatter.setReflectionGain(1.0f);
				// Atlas was pre-baked at load (prepareReflectionAtlas); the
				// break only stamps the per-shard text fragments + arms the
				// per-frame bake — no allocation hitch here.
				g_greetsShatter.setShardText(haveText ? ptext : nullptr,
				                             haveText ? A : nullptr);
				g_greetsShatter.armReflectionCameras();
				m.broken = true;   // UpdateAllMirrors closes the reflection
				// Hide the panel surface itself (reflection + half-silvered
				// text) AND the screen box's side/back faces — the shards
				// replace the whole box. Per-face Txtr swap; the faces are
				// shared in the room mesh so we can't hide a mesh. The box
				// faces (side rims + back) were collected at build by
				// footprint + behind-front-plane test; without this they
				// survived as a dark strip on the wall after the break.
				if (g_greetsScreenHideMat) {
					for (Face *F : m.wallFaces)            F->Txtr = g_greetsScreenHideMat;
					for (Face *F : g_greetsScreenBoxFaces) F->Txtr = g_greetsScreenHideMat;
				}
				std::fprintf(stderr, "[GREETS-SHATTER] broke mirror %d\n",
				             g_greetsShatterMirror);
			}
			sYPrev = yNow;
			g_greetsShatter.update();
			// Per-shard reflection pass: hide the mirror clones first so a
			// shard doesn't reflect another mirror's reflection. Runs after
			// the physics update (current poses), before the main transform.
			if (g_greetsShatter.reflectionCamerasEnabled()) {
				std::vector<std::pair<TriMesh*, DWord>> hidden;
				for (fds::Mirror &mm : g_greetsMirrors) {
					if (!mm.cloneMesh) continue;
					hidden.push_back({ mm.cloneMesh, mm.cloneMesh->Flags });
					mm.cloneMesh->Flags &= ~HTrack_Visible;
				}
				g_greetsShatter.renderReflectionCameras(GreetSc);
				for (auto &h : hidden) h.first->Flags = h.second;
			}
		}

		// Mirror clone tracking: re-mirror dynamic meshes (Hull / legs)
		// and cloned omnis after Animate_Objects refreshes the source
		// poses, before Transform_Objects reads positions. Iterates
		// every mirror in g_greetsMirrors. No-op list when
		// --greets-mirror is off.
		// Disco ball spin + spot sweep: after Animate_Objects (the
		// ball is Tri_Possessed; spots' spline-stamped IPos/IDir get
		// overwritten here), before the mirrors re-mirror it.
		// g_FrameTime, not the raw atomic Timer — the latter keeps
		// ticking on the SDL timer thread, so reading it here would let
		// the ball spin/bob a hair while paused. g_FrameTime is the
		// frozen/scrubbed scene time CurFrame already uses.
		fds::UpdateDiscoBall(GreetSc, (float)g_FrameTime);

		// Blaster-bolt test (--greets-blasters): two emitters trade fire
		// across the corridor at the robot's height. Deterministic on
		// g_FrameTime so it's scrub-safe; billboards to the live camera.
		if (fds::FeatureFlags::greets_blasters()) {
			static int32_t sLastFire = -10000;
			const int32_t t = (int32_t)g_FrameTime;
			if (t - sLastFire >= 26 || t < sLastFire) {   // (t<last -> scrubbed back)
				sLastFire = t;
				const Vector eyeL{ -6.0f, 3.5f, -17.0f }, dirR{ 1.0f, 0.0f, -0.1f };
				const Vector eyeR{  6.0f, 3.5f, -19.0f }, dirL{ -1.0f, 0.0f, -0.1f };
				fds::BlasterBolts_Fire(eyeL, dirR, 0.45f, 1.0f, 0.20f, 0.10f);  // red
				fds::BlasterBolts_Fire(eyeR, dirL, 0.45f, 0.20f, 1.0f, 0.35f);  // green
			}
			float bdt = (float)dTime; if (bdt < 0.5f) bdt = 0.5f; if (bdt > 4.0f) bdt = 4.0f;
			fds::BlasterBolts_Update(bdt);
			// Emit transient bolt-lights AFTER Animate_Objects (ran above, sets
			// omni IPos from splines) and BEFORE gg->Render()'s deferred lighting.
			// Pass GreetSc (the rendered scene) — the deferred kernel reads that,
			// not the global CurScene.
			fds::BlasterBolts_EmitLights(GreetSc);
		}

		// Live tint knobs (shard silver #3 + portal tint #1): re-applied
		// every frame so the tuning console / greets.params can drive them.
		GreetsApplyLiveTints();

		fds::UpdateAllMirrors(GreetSc, g_greetsMirrors);

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

		TailProf::mark("Tick-Logic", _tickT, 3);

		// Second-order mirror RTT pass (no-op without --mirror-rtt):
		// re-render the visible mirror-in-mirror panels into their slot
		// textures. Must run AFTER Animate_Objects (current geometry,
		// camera pose from this frame's spline step) and BEFORE the
		// main Transform_Objects (the pass clobbers per-vertex
		// transforms, which the main transform recomputes).
		{ TailProf::ScopeTimer _tp("RTT", 3);
		fds::RenderSecondOrderMirrors(GreetSc, g_greetsMirrors,
		                              g_greetsRttSlots); }

		// Foundation F (docs/ENVDYN_DISPLACEMENT_PLAN.md): refresh world
		// AABBs after Animate_Objects (pose current) and before the main
		// Transform, so the frustum-vs-AABB machinery + the --draw_aabbs
		// overlay see this frame's boxes. Only walked when a consumer is
		// live (the debug overlay for now); flag-off = never called = byte-
		// null.
		if (fds::FeatureFlags::draw_aabbs())
			fds::WorldAabb_UpdateScene(GreetSc);

		// ENVDYN Workstream A3: live dynamic-mesh reflection overlay — the
		// mech rendered into flagged env probes, composited over their static
		// capture. Ordered here (post-Animate, post-RTT, PRE main Transform),
		// like RenderSecondOrderMirrors — NOT inside FramePrep. No-op unless
		// --env_dynamic and a flagged/relevant probe exists.
		fds::EnvDynamic_Overlay(GreetSc);

		// --bake_tick_overlap: dispatch the per-frame shadow bake NOW — its
		// pool tasks run underneath the serial Transform/Lighting/Radix
		// chunk below (complementary resources: the pool is otherwise
		// parked while the tick thread works). Joined explicitly before
		// gg->Render(). Guards: not while the shatter is active (fresh
		// shard meshes would race their first VertexScratch clone-init
		// copy against the concurrent main Transform) and not in the first
		// frames (clone warmup — steady state has no init copies). The
		// bake reads only Animate-written pose state (stable by now) + its
		// own clones; the serial chunk writes shared per-vertex projection
		// and colour fields the depth-only bake never reads.
		static int sBakeOverlapWarmup = 0;
		const bool bakeOverTick = fds::FeatureFlags::bake_tick_overlap()
		    && !fds::FeatureFlags::shadow_gbuffer_overlap()
		    && !g_greetsShatter.active()
		    && sBakeOverlapWarmup++ > 2;
		if (bakeOverTick) {
			prof.switchTo(PROF_BAKE);
			ShadowBake_DispatchGreets(GreetSc);
		}

		// PROFILER: XFRM phase.
		prof.switchTo(PROF_XFRM);

		// Phase B: arm the prev-frame chunk-occlusion cull (poses are current
		// post-Animate) BEFORE the transform. The depth capture itself happens
		// in ChunkOcclusion_EndFrame below (post-Render — ZPage16 is cleared
		// before Transform, so it can't be read here). AABB refresh cost lands
		// in XFRM; the min-pool lands in RNDR and is reported by --vis_stats.
		// No-op unless --chunk_occlusion (or --vis_stats).
		fds::ChunkOcclusion_BeginFrame(GreetSc);

		_tickT = TailProf::nowNs();
		Transform_Objects(GreetSc, fds::g_mainCamera, fds::g_mainFaces);
		TailProf::mark("Tick-Xfrm", _tickT, 3); _tickT = TailProf::nowNs();

		// PROFILER: LGHT phase.
		prof.switchTo(PROF_LGHT);

		// In deferred mode the only consumer of forward vertex colors
		// is the mirror-RTT offscreen pass (1-frame-stale by design).
		// When no RTT job rendered this frame, the colors won't be
		// read next frame either — skip the ~4ms pass. The first
		// frame a panel becomes visible again renders from colors at
		// most one skip stale, same staleness class as the design.
		// Same predicate correction as the chunk split above: with the bare
		// --deferred flag unset but the deferred path live (ForceDeferred /
		// shard_deferred), this read "forward" and ran the full forward
		// Lighting() pass every frame — 6.49 ms whose only consumer is the RTT
		// bake, which was not even running.
		const bool lightingConsumed =
		    !fds::DeferredPathEnabled() ||
		    !fds::FeatureFlags::mirror_rtt() ||
		    fds::g_rttJobsLastFrame > 0;
		if (lightingConsumed)
			Lighting(GreetSc);
		TailProf::mark("Tick-Light", _tickT, 3); _tickT = TailProf::nowNs();

		// PROFILER: SORT phase.
		prof.leave(PROF_LGHT);
		if (!CAll) return true;
		prof.enter(PROF_SORT);

		Radix_Sort(FList, SList, CAll);
		TailProf::mark("Tick-Radix", _tickT, 3);

		// PROFILER: RNDR phase.
		prof.switchTo(PROF_BAKE);

		// Per-frame shadow-map depth pre-pass (Omni_CastsShadow lights).
		// MUST run before gg->Render(): Render() invokes the deferred
		// lighting kernel, which samples shadow maps. If shadow render
		// runs after, the kernel reads PREVIOUS frame's shadow maps
		// (one-frame lag → flicker patches during motion and at startup,
		// frame 0 reads all-zero buffers → no shadows that frame).
		// City does it in this order (CITY.CPP:2230-2235 then Render at
		// :2278). Gated on env FDS_SHADOWS=1; no-op when off.
		// Runs the DynamicOmnis bake and, when --shadow-dynamic is set, the
		// DynamicMeshes bake into each static omni's dynamic buffer (lighting
		// samples max(static, dynamic) so moving meshes cast moving shadows).
		// With --shadow-gbuffer-overlap (Stage A, docs/THREADING_OVERLAP_PLAN.md)
		// these run on a dedicated thread overlapping the gbuffer fill in
		// gg->Render() below, and renderFrame joins before deferred lighting;
		// otherwise they run synchronously inline here (current behavior).
		if (!bakeOverTick)
			ShadowBake_DispatchGreets(GreetSc);
		else
			ShadowBake_JoinPending();   // tick-thread join: the bake never
			                            // contends the render's own waves
		prof.switchTo(PROF_RNDR);
		{ TailProf::ScopeTimer _tp("GreetsCodeStage", 3); gg->Render(); }
		// reset per-frame zbuffer statistics
		zPass = zReject = 0;
		// Diagnostic: identify shadow maps whose dynamic polyId hash isn't
		// changing across frames (= dynamic bake not reaching that map).
		// No-op unless toggled by T key in REV.CPP.
		::ShadowMap_TickStalenessTracker();
		// FDS_MIRROR_CLAMP_STATS=1: per-frame count of clone lanes the
		// wall-depth clamp rejected (Mekalele.h). Nonzero at a pose =
		// clone geometry was landing NEARER than the glass there (the
		// self-reflection moire); zero everywhere = the clamp is inert.
		if (meka::g_mirrorClampStats) {
			const long long n = meka::g_mirrorClampCount.exchange(0, std::memory_order_relaxed);
			const long long c = meka::g_mirrorCloneLanes.exchange(0, std::memory_order_relaxed);
			std::fprintf(stderr, "[MIRROR-CLAMP] clone-lanes=%lld rejected=%lld (t=%d)\n",
			             c, n, (int)g_FrameTime);
		}

		// Stamp gb.mirrorId with each mirror's id at its wall faces'
		// screen-space pixels — Mekalele's clone path tests this per
		// pixel and only commits writes where the matching mirror's
		// wall covers. Without this clones spill outside the mirror
		// surface footprint (worked for teleporter only because the
		// surrounding Piramid walls were the de facto occluder mask).
		{ TailProf::ScopeTimer _tp("StampMasks", 3);   fds::StampMirrorMasks(GreetSc, g_greetsMirrors); }
		{ TailProf::ScopeTimer _tp("ProbeMirrors", 3); fds::ProbeSecondOrderMirrors(GreetSc, g_greetsMirrors); }

		// The mirror clone-mask (Mekalele's per-pixel gb.mirrorId ==
		// ctx.mirrorTag) only runs on the deferred path. Forward sends
		// clone faces to the opaque pass un-masked and the surrounding
		// occluders Z-fail them — the user sees the silvered wall but
		// no reflection. Native default for FDS_DEFERRED is OFF
		// (FDS_DEFERRED_DEFAULT_ON=1 is only set in the EMSCRIPTEN
		// branch of FDS/CMakeLists.txt), so the explicit choice to
		// pass --greets-mirror implies "I want to see reflections" —
		// force the deferred path here rather than requiring the user
		// to also remember --deferred / FDS_DEFERRED=1.
		// FDS_RNDR_BENCH=1: time ForceForward vs ForceDeferred over the same
		// full-frame workload (FList/CAll already built) so we know the
		// per-render cost ratio before committing the shard bake to deferred.
		if (std::getenv("FDS_RNDR_BENCH")) {
			const int K = 20;
			auto tF0 = std::chrono::steady_clock::now();
			for (int i = 0; i < K; ++i) Render(RenderPath::ForceForward);
			auto tF1 = std::chrono::steady_clock::now();
			for (int i = 0; i < K; ++i) Render(RenderPath::ForceDeferred, /*skipVolumetric=*/false);
			auto tD1 = std::chrono::steady_clock::now();
			for (int i = 0; i < K; ++i) Render(RenderPath::ForceDeferred, /*skipVolumetric=*/true);
			auto tDS1 = std::chrono::steady_clock::now();
			const double fwd = std::chrono::duration<double,std::milli>(tF1-tF0).count()/K;
			const double def = std::chrono::duration<double,std::milli>(tD1-tF1).count()/K;
			const double defNV = std::chrono::duration<double,std::milli>(tDS1-tD1).count()/K;
			std::fprintf(stderr, "[RNDR-BENCH] fwd=%.2f  deferred=%.2f (%.1fx)  deferred-noVol=%.2f (%.1fx)  CAll=%d %dx%d\n",
			             fwd, def, def/fwd, defNV, defNV/fwd, int(CAll), int(XRes), int(YRes));
		}
		if (fds::FeatureFlags::greets_mirror()) {
			TailProf::ScopeTimer _tp("Render-3D", 3);
			Render(RenderPath::ForceDeferred);
		} else {
			Render();
		}

		// Phase B: capture this frame's final opaque depth (ZPage16 is live
		// right after Render) as the NEXT frame's occluder, run the
		// --chunk_occl_verify over-cull audit on this frame's culled chunks,
		// report --vis_stats, reset.
		fds::ChunkOcclusion_EndFrame(GreetSc);

		// F9: one-shot PPM frame dump to /tmp/greets_dump_N.ppm. Edge-
		// triggered so a held key only fires once. Lets the user fly
		// around in TAB free-cam, line up the teleporter or text
		// screens, and capture a frame for off-line inspection of the
		// mirror system.
		{
			static bool sF9Prev = false;
			static int  sDumpN  = 0;
			// 'G' (grab) as well as F9 — on macOS F9 is a system key
			// (Mission Control) and never reaches the app.
			const bool  f9Now   = Keyboard[ScF9] != 0 || Keyboard[ScG] != 0;
			if (f9Now && !sF9Prev) {
				const int dumpT = (int)g_FrameTime;   // scene time this frame
				char path[80];
				std::snprintf(path, sizeof(path),
				              "/tmp/greets_dump_%d_t%d.ppm", sDumpN++, dumpT);
				std::FILE *f = std::fopen(path, "wb");
				if (f) {
					const int xr = (int)XRes, yr = (int)YRes;
					std::fprintf(f, "P6\n%d %d\n255\n", xr, yr);
					std::vector<unsigned char> row(xr * 3);
					for (int y = 0; y < yr; ++y) {
						const dword *src = (const dword*)((const byte*)VPage
						                  + y * (int)VESA_BPSL);
						for (int x = 0; x < xr; ++x) {
							dword px = src[x];
							row[x*3+0] = (px >> 16) & 0xFF;
							row[x*3+1] = (px >>  8) & 0xFF;
							row[x*3+2] = (px      ) & 0xFF;
						}
						std::fwrite(row.data(), 1, row.size(), f);
					}
					std::fclose(f);
					std::fprintf(stderr, "[GREETS] dumped %s (t=%d)\n", path, dumpT);
				}
				// FDS_XPAR_TRACE: write the dumped frame's transparent composite
				// census next to the pixel dump (same tag) for z-order forensics.
				{
					char tp[96];
					std::snprintf(tp, sizeof tp, "/tmp/greets_xtrace_%d_t%d.txt",
					              sDumpN - 1, dumpT);
					Xtrace_WriteFile(tp);
				}
				// Also print the current camera pose + scene time so the
				// user can reproduce this exact view headless:
				//   FDS_GREETS_CAM="px,py,pz,fx,fy,fz" ./DEMO --snapshot=greets@t=<t>
				// pos = eye; fwd = the camera's forward (Mat row 2).
				// %.9g: full float round-trip precision. 3 decimals was enough to
				// re-frame a shot but NOT to reproduce facing-threshold bugs — a
				// grazing face's front/back test (N·A ~ 0) can flip inside the
				// truncation error, so the pinned headless repro landed on the
				// other side of the flip than the live run being reported.
				std::fprintf(stderr,
					"[GREETS-CAM] FDS_GREETS_CAM=\"%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\" t=%d\n",
					View->ISource.x, View->ISource.y, View->ISource.z,
					View->Mat[2][0], View->Mat[2][1], View->Mat[2][2], dumpT);
				std::fflush(stderr);
			}
			sF9Prev = f9Now;
		}

		// Disco-ball bloom: post-pass over the ball's screen disc
		// (no-op when --no-greets-disco / ball off-screen).
		// SUBSUMED by full-screen --bloom (the ball is Mat_HdrEmissive, a true
		// HDR source that over-blooms through the general bloom). Run this
		// legacy localized pass only when --bloom is OFF.
		if (!fds::FeatureFlags::bloom())
			fds::DiscoBloomPost();

		// Blaster bolts: additive screen-space draw over the finished frame.
		if (fds::FeatureFlags::greets_blasters())
			fds::BlasterBolts_Draw();

		// Optional mirror-mask debug overlay (--greets-mirror-debug-mask).
		if (fds::FeatureFlags::greets_mirror_debug_mask()) {
			fds::DebugOverlayMirrorMask(GreetSc);
		}

		// Foundation F world-AABB / frustum overlay (--draw_aabbs).
		if (fds::FeatureFlags::draw_aabbs())
			fds::WorldAabb_DrawOverlay(GreetSc);

		// Stone-displacement bake wireframe (--displace_viz): the geometry
		// DisplaceMaterialVertices added/moved, tinted by displacement
		// magnitude. No-op (byte-null) unless --displace_viz is on.
		if (fds::FeatureFlags::displace_viz())
			fds::DisplaceViz_DrawOverlay(GreetSc);

		// S1d-1 patch-boundary seam classification (--pom_seam_viz): every
		// classified boundary edge, coloured by class, depth-tested. No-op
		// (byte-null) unless --pom_seam_viz > 0.
		if (fds::FeatureFlags::pom_seam_viz() > 0)
			fds::PomSeamViz_DrawOverlay(GreetSc);

		// PROFILER: FLIP phase. also contains display of runtime stats
		prof.leave(PROF_RNDR);

		// RETIRED 2026-08-06: the persistent "Shadow: Depth|PolyId [F3]"
		// indicator that used to sit top-right. It was on screen in EVERY
		// greets capture to report a technique nobody switches mid-review, and
		// its "[F3]" had gone stale twice over (F3 briefly became a review
		// scrub, and the toggle it named had been moved to an untranslated
		// ScF6, so the overlay advertised a control that did nothing). The
		// toggle is back on F3 a few hundred lines up and prints
		// [SHADOW-MODE] to stderr when it fires — that is the feedback now.

		//draw_cones(sShadowMapView);

		// Live matID readout at screen center. Reads the G-buffer's
		// packed mat32 at the center pixel, extracts the matID byte,
		// looks up Material* via the scene's matTable, prints id + name.
		// Updates every frame so the value follows whatever's under the
		// crosshair as the camera moves. Suppressed in shadow-viz mode
		// where M overwrites the framebuffer.
		// BEHIND --mat_probe SINCE 2026-08-06, default OFF. It is genuinely
		// the way a material gets named from a screenshot, but it was
		// unconditional, so it also sat in the bottom-left of every capture
		// this campaign took. FDS_GREETS_FACEFIND implies it — that env var
		// drives the face census nested below and is the same investigation.
		if ((fds::FeatureFlags::mat_probe() || std::getenv("FDS_GREETS_FACEFIND"))
		    && g_gbuffer && !sShadowMapView) {
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
				// FDS_GREETS_FACEFIND="x,y": find the ACTUAL faces covering a
				// pixel by point-in-triangle over every transformed visible
				// face (their screen PX/PY are set even for forward/xpar faces
				// that only stamp the gbuffer sentinel). Reports the front-most
				// few so the black strip's real face/mesh/material is found.
				if (const char *ff = std::getenv("FDS_GREETS_FACEFIND")) {
					int qx = XRes/2, qy = YRes/2; std::sscanf(ff, "%d,%d", &qx, &qy);
					struct Hit { Face *f; TriMesh *T; float rz; };
					std::vector<Hit> hits;
					for (TriMesh *T = GreetSc->TriMeshHead; T; T = T->Next) {
						if (!(T->Flags & HTrack_Visible)) continue;
						for (DWord fi = 0; fi < T->FIndex; ++fi) {
							Face &F = T->Faces[fi];
							if (!F.A || !F.B || !F.C) continue;
							const float ax=F.A->PX, ay=F.A->PY, bx=F.B->PX, by=F.B->PY, cxx=F.C->PX, cyy=F.C->PY;
							const float d1=(qx-bx)*(ay-by)-(ax-bx)*(qy-by);
							const float d2=(qx-cxx)*(by-cyy)-(bx-cxx)*(qy-cyy);
							const float d3=(qx-ax)*(cyy-ay)-(cxx-ax)*(qy-ay);
							const bool neg=(d1<0)||(d2<0)||(d3<0), pos=(d1>0)||(d2>0)||(d3>0);
							if (neg && pos) continue;  // pixel outside triangle
							hits.push_back({ &F, T, (F.A->RZ+F.B->RZ+F.C->RZ)/3.0f });
						}
					}
					std::sort(hits.begin(), hits.end(), [](const Hit&a, const Hit&b){ return a.rz > b.rz; });
					{ const uint32_t m32 = g_gbuffer->txtr[size_t(qy)*size_t(XRes)+size_t(qx)];
					  const word z = ((const word*)MainSurf->Z16)[size_t(qy)*size_t(XRes)+size_t(qx)];
					  const MatTable mt = Scene_GetMatTable(GreetSc); const uint32_t id=(m32>>20)&0xFF;
					  std::fprintf(stderr,"[FACEFIND @%d,%d] gbuffer mat32=0x%x %s zEnc=%u\n", qx,qy, m32,
					    m32==0xFFFFFFFFu?"FORWARD/empty":((id<mt.count&&mt.data&&mt.data[id]&&mt.data[id]->Name)?mt.data[id]->Name:"?"), z); }
					std::fprintf(stderr, "[FACEFIND @%d,%d] %zu faces cover this pixel:\n", qx, qy, hits.size());
					for (size_t i = 0; i < hits.size() && i < 6; ++i) {
						Face *F = hits[i].f; TriMesh *T = hits[i].T;
						auto wp=[&](const Vertex*v){ Vector lp=v->Pos,w; MatrixXVector(T->RotMat,&lp,&w); w+=T->IPos; return w; };
						const Vector a=wp(F->A);
						std::fprintf(stderr, "  #%zu mat='%s' refl=%d xpar=%d maskTag=%u Flags=0x%x rz=%.4f N=(%.2f,%.2f,%.2f) worldA=(%.1f,%.1f,%.1f)\n"
						             "       scr A(%.0f,%.0f rz%.3f) B(%.0f,%.0f rz%.3f) C(%.0f,%.0f rz%.3f) mesh=%p\n",
							i, (F->Txtr&&F->Txtr->Name)?F->Txtr->Name:"?",
							(F->Flags&Face_Reflective)!=0, (F->Txtr&&(F->Txtr->Flags&Mat_Transparent))?1:0,
							(unsigned)F->mirrorMaskTag, (unsigned)F->Flags,
							hits[i].rz, F->N.x,F->N.y,F->N.z, a.x,a.y,a.z,
							F->A->PX,F->A->PY,F->A->RZ, F->B->PX,F->B->PY,F->B->RZ, F->C->PX,F->C->PY,F->C->RZ, (void*)T);
					}

					// WORLD-SPACE RAYCAST — the decisive version. The screen
					// point-in-triangle list above uses raw projected PX/PY,
					// which are garbage for faces straddling the near plane
					// (huge wall quads next to the camera — exactly the faces
					// an occlusion question is about). Cast the pixel's world
					// ray and Moller-Trumbore every visible face instead; the
					// t-ordered hit list is ground truth for "what SHOULD the
					// camera see first at this pixel".
					if (View) {
						Vector dv = { (float(qx) - CntrEX) / FOVX,
						              -(float(qy) - CntrEY) / FOVY, 1.0f };
						Vector rd = { View->Mat[0][0]*dv.x + View->Mat[1][0]*dv.y + View->Mat[2][0]*dv.z,
						              View->Mat[0][1]*dv.x + View->Mat[1][1]*dv.y + View->Mat[2][1]*dv.z,
						              View->Mat[0][2]*dv.x + View->Mat[1][2]*dv.y + View->Mat[2][2]*dv.z };
						Vector_Norm(&rd);
						const Vector ro = View->ISource;
						struct RHit { Face *f; TriMesh *T; float t; Vector p; };
						std::vector<RHit> rhits;
						for (TriMesh *T = GreetSc->TriMeshHead; T; T = T->Next) {
							if (!(T->Flags & HTrack_Visible)) continue;
							for (DWord fi = 0; fi < T->FIndex; ++fi) {
								Face &F = T->Faces[fi];
								if (!F.A || !F.B || !F.C) continue;
								auto wp=[&](const Vertex*v){ Vector lp=v->Pos,w; MatrixXVector(T->RotMat,&lp,&w); w+=T->IPos; return w; };
								const Vector A=wp(F.A), B=wp(F.B), C=wp(F.C);
								const Vector e1={B.x-A.x,B.y-A.y,B.z-A.z}, e2={C.x-A.x,C.y-A.y,C.z-A.z};
								const Vector pv={rd.y*e2.z-rd.z*e2.y, rd.z*e2.x-rd.x*e2.z, rd.x*e2.y-rd.y*e2.x};
								const float det = e1.x*pv.x+e1.y*pv.y+e1.z*pv.z;
								if (std::fabs(det) < 1e-9f) continue;
								const float invDet = 1.0f/det;
								const Vector tv={ro.x-A.x,ro.y-A.y,ro.z-A.z};
								const float u=(tv.x*pv.x+tv.y*pv.y+tv.z*pv.z)*invDet;
								if (u < -0.001f || u > 1.001f) continue;
								const Vector qv={tv.y*e1.z-tv.z*e1.y, tv.z*e1.x-tv.x*e1.z, tv.x*e1.y-tv.y*e1.x};
								const float v=(rd.x*qv.x+rd.y*qv.y+rd.z*qv.z)*invDet;
								if (v < -0.001f || u+v > 1.001f) continue;
								const float t=(e2.x*qv.x+e2.y*qv.y+e2.z*qv.z)*invDet;
								if (t <= 0.01f) continue;
								rhits.push_back({&F, T, t,
									{ro.x+rd.x*t, ro.y+rd.y*t, ro.z+rd.z*t}});
							}
						}
						std::sort(rhits.begin(), rhits.end(),
						          [](const RHit&a, const RHit&b){ return a.t < b.t; });
						std::fprintf(stderr, "[RAYCAST @%d,%d] ro=(%.2f,%.2f,%.2f) rd=(%.3f,%.3f,%.3f) %zu hits (near first):\n",
						             qx, qy, ro.x, ro.y, ro.z, rd.x, rd.y, rd.z, rhits.size());
						for (size_t i = 0; i < rhits.size() && i < 10; ++i) {
							Face *F = rhits[i].f; TriMesh *T = rhits[i].T;
							const float facing = F->N.x*rd.x + F->N.y*rd.y + F->N.z*rd.z;
							std::fprintf(stderr,
								"  t=%7.2f  hit=(%.1f,%.1f,%.1f)  mat='%s'  N=(%.2f,%.2f,%.2f) %s  maskTag=%u  mesh=%p\n",
								rhits[i].t, rhits[i].p.x, rhits[i].p.y, rhits[i].p.z,
								(F->Txtr&&F->Txtr->Name)?F->Txtr->Name:"?",
								F->N.x, F->N.y, F->N.z,
								facing < 0 ? "FRONT-facing" : "back-facing",
								(unsigned)F->mirrorMaskTag, (void*)T);
						}
					}
					std::fflush(stderr);
				}
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
			prof.noteCounters((double)g_renderedPolys, (double)FillerPixelcount);
			scroll = prof.drawOverlay(scroll);
			// (clip stats overlay removed — just noise.)
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
				for (uint32_t t : sm.packSD) {
					const uint16_t e = ShadowTexZ(t);
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
				const uint32_t *pRow = sm.packSD.data() + size_t(smY) * size_t(sm.xres);
				for (int px = 0; px < XRes; ++px) {
					const int smX = (px * sm.xres) / XRes;
					uint32_t pixel;
					if (sVizPolyId) {
						const uint32_t v = ShadowTexId(pRow[smX]);
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
						const uint16_t z = ShadowTexZ(pRow[smX]);
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
	GreetsApplyRunDefaults();   // global render/runtime flags — greets-exclusive entry
	return std::make_unique<GreetsScene>();
}

void Greets_JoinBakeThread()
{
	// Block until the background lightmap bake started in Initialize_Greets
	// has finished writing TriMesh::staticShadowLM. The first rendered
	// frame's deferred kernel samples it; sampling unwritten memory =
	// garbage or full-shadow. Equally important: the bake reads per-frame
	// geometry (Animate_Objects spline outputs) and the deferred shadow
	// maps (Render_DeferredShadowMaps), so any tick that runs before this
	// join races the bake (TSan, 2026-06-12). Every driver of the greets
	// scene must call this before its first tick.
	if (g_greetsBakeThread.joinable()) {
		const auto t0 = std::chrono::steady_clock::now();
		g_greetsBakeThread.join();
		const auto t1 = std::chrono::steady_clock::now();
		const double waitedMs = std::chrono::duration<double, std::milli>(
			t1 - t0).count();
		std::fprintf(stderr,
			"[GREETS-BAKE] waited %.1f ms for bake thread to join\n",
			waitedMs);
	}
}

void Run_Greets()
{
	InitTimelineMark("Run_Greets: begin (joining lightmap bake)");
	Greets_JoinBakeThread();
	auto scene = createGreetsScene();
	InitTimelineMark("Run_Greets: scene driver ready");
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