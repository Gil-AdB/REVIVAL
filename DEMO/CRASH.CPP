#include "Rev.h"
#include <Threads.h>

#include "CRASH.H"
#include "FrameProfiler.h"
#include "MaterialImport.h"
#include "MeshOps.h"
#include "SceneTick.h"
#include "Scenes.h"
#include <memory>

static Scene *CrashSc;

static int32_t CrPartTime = 0;// 10.0f * 100;

void Initialize_Crash()
{
	CrashSc = loadSceneAligned("SCENES/CRASH.FLD");
	CrPartTime = 1.5f*100.0f*(CrashSc->EndFrame-CrashSc->StartFrame)/30.0f;

	CrashSc->FZP = 2000.0f;
	CrashSc->NZP = 10.0f;
	CrashSc->Flags |= Scn_ZBuffer;
	// Matte-dominated crash scene — OuterVec is the better fit.
	CrashSc->PreferOuterVec = 1;

	CrashSc->Ambient.R = 64;
	CrashSc->Ambient.G = 64;
	CrashSc->Ambient.B = 64;
	tagSceneMaterials(CrashSc, /*restrictToScene=*/false);

	Preprocess_Scene(CrashSc, "CRASH");

	// Source-authored PBR map assignments (LWO RVSM → FLD; the .MAT sidecar
	// reader is retired, sidecar-elim endgame). No-op when none are authored.
	fds::MaterialImport_ApplyRevMaps(CrashSc, "crash");

	// Auto-detect creased meshes (laptop facade) and switch them to
	// per-face vertex normals; smooth meshes (if any) untouched.
	MakeFacesIndependentByAngle(CrashSc, 30.0f);
}

namespace {
struct CrashScene : SceneDriver {
	int32_t partTime() const override { return CrPartTime; }
	char MSGStr[128];
	FrameProfiler prof{"crash"};
	int32_t TTrd = 0;          // shared transport state (pause / scrub)
	bool    pause_mode = false;

	~CrashScene() override { Destroy_Scene(CrashSc); }

	void init() override {
		SetCurrentScene(CrashSc);
		Calibrate_FreeCamera_ForScene(CrashSc->FZP, CrashSc->CameraHead);
		for (Omni *O = CrashSc->OmniHead; O; O = O->Next)
			O->IRange = 2000.0f;
		setupFaceLists(CrashSc, /*includeOmnisInCount=*/true);

		Ambient_Factor = 1;
		Diffusive_Factor = 1;
		ImageSize = 1;
		TTrd = Timer;
	}

	bool tick() override {
		// ESC = exit demo (handled by runSceneBlocking); Backspace = skip scene.
		if (Keyboard[ScBackSpace] || Timer >= CrPartTime) return false;

		prof.beginFrame();
		prof.enter(PROF_ZCLR);

		// Shared transport: pause (P/U), fast-forward/rewind (F2/F1), and
		// the race-free scene-time snapshot. See SceneDriver::tickSceneTimer.
		tickSceneTimer(TTrd, pause_mode);

		tickTabToggle(CrashSc, "crash");
		clearFrame();

		CurFrame = CrashSc->StartFrame + (CrashSc->EndFrame-CrashSc->StartFrame) * ((float)g_FrameTime / (float)CrPartTime);

		prof.switchTo(PROF_ANIM);
		Dynamic_Camera();
		if (Keyboard[ScC]) { FC.ISource = View->ISource; Matrix_Copy(FC.Mat, View->Mat); FC.IFOV = View->IFOV; }

		Animate_Objects(CrashSc, View);

		prof.switchTo(PROF_XFRM);
		Transform_Objects(CrashSc, fds::g_mainCamera, fds::g_mainFaces);

		prof.switchTo(PROF_LGHT);
		Lighting(CrashSc);

		prof.leave(PROF_LGHT);
		if (!CAll) return true;
		prof.enter(PROF_SORT);
		Radix_Sort(FList, SList, CAll);

		prof.switchTo(PROF_RNDR);
		Render();
		prof.leave(PROF_RNDR);

		dword scroll = 0;
		snprintf(MSGStr, sizeof(MSGStr), "g_FrameTime %d", g_FrameTime);
		scroll += OutTextXY(VPage, 0, scroll, MSGStr, 255);
		snprintf(MSGStr, sizeof(MSGStr), "CrPartTime %d", CrPartTime);
		scroll += OutTextXY(VPage, 0, scroll + 15 * g_fontScale, MSGStr, 255);
		snprintf(MSGStr, sizeof(MSGStr), "Frame %f/%f", CurFrame, CrashSc->EndFrame);
		scroll += OutTextXY(VPage, 0, scroll + 15 * g_fontScale, MSGStr, 255);

		prof.enter(PROF_FLIP);
		Flip(MainSurf);
		prof.leave(PROF_FLIP);
		prof.endFrame();
		return true;
	}

	void cleanup() override {
		if (g_profilerActive) prof.dump();
		Timer -= CrPartTime;
		waitBackspaceRelease();
		if (Timer < 0) Timer = 0;
	}
};
} // anonymous namespace

std::unique_ptr<SceneDriver> createCrashScene()
{
	// Dark by design (the register/crash dump) → keep it moody, lift slightly.
	ApplyCinematicProfile(cine::kCrash);
	return std::make_unique<CrashScene>();
}

void Run_Crash()
{
	auto scene = createCrashScene();
	runSceneBlocking(*scene);
}