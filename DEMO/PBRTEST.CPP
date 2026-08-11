#include "Rev.h"
#include <Threads.h>

#include "PBRTEST.H"
#include "FrameProfiler.h"
#include "MaterialImport.h"
#include "SceneTick.h"
#include "Scenes.h"
#include <memory>

// PBR / env-reflection test scene. Not part of the demo sequence — a
// deterministic material lab for the editor + headless snapshots. Sources
// in Authoring/pbrtest (regen recipe in its README); modeled on CRASH.CPP,
// the smallest driver.

static Scene *PbrSc;

static int32_t PbrPartTime = 0;

void Initialize_PBRTest()
{
	PbrSc = loadSceneAligned("SCENES/PBRTEST.FLD");
	// 240 frames @30fps → Timer units (100/s), same mapping the demo
	// scenes use: PartTime = 100 * frames / fps.
	PbrPartTime = 100.0f * (PbrSc->EndFrame - PbrSc->StartFrame) / 30.0f;

	PbrSc->FZP = 500.0f;
	PbrSc->NZP = 0.5f;
	PbrSc->Flags |= Scn_ZBuffer;

	PbrSc->Ambient.R = 48;
	PbrSc->Ambient.G = 48;
	PbrSc->Ambient.B = 48;
	tagSceneMaterials(PbrSc, /*restrictToScene=*/false);

	Preprocess_Scene(PbrSc, "PBRTEST");

	// Source-authored PBR map assignments (LWO RVSM → FLD; the .MAT sidecar
	// reader is retired, sidecar-elim endgame). No-op when none are authored.
	fds::MaterialImport_ApplyRevMaps(PbrSc, "pbrtest");
	// CLI `--material-import=<material>:<dir>` overrides, same call GREETS.CPP
	// makes after its own RevMaps pass. Without this, the ONE scene built to be
	// a material lab was the one scene where --material-import silently did
	// nothing: the flag parsed, the spec landed in g_specs, and no scene ever
	// consumed it. Byte-null when no --material-import= is on the command line
	// (MaterialImport_Apply early-returns on an empty spec list).
	fds::MaterialImport_Apply(PbrSc, "pbrtest");
}

namespace {
struct PBRTestScene : SceneDriver {
	int32_t partTime() const override { return PbrPartTime; }
	FrameProfiler prof{"pbrtest"};
	int32_t TTrd = 0;          // shared transport state (pause / scrub)
	bool    pause_mode = false;

	~PBRTestScene() override { Destroy_Scene(PbrSc); }

	void init() override {
		SetCurrentScene(PbrSc);
		Calibrate_FreeCamera_ForScene(PbrSc->FZP, PbrSc->CameraHead);
		setupFaceLists(PbrSc, /*includeOmnisInCount=*/true);

		Ambient_Factor = 1;
		Diffusive_Factor = 1;
		Specular_Factor = 1;   // deferred per-scene specular gate
		ImageSize = 1;
		TTrd = Timer;
	}

	bool tick() override {
		if (Keyboard[ScBackSpace] || Timer >= PbrPartTime) return false;

		prof.beginFrame();
		prof.enter(PROF_ZCLR);

		tickSceneTimer(TTrd, pause_mode);
		tickTabToggle(PbrSc, "pbrtest");
		clearFrame();

		CurFrame = PbrSc->StartFrame +
		           (PbrSc->EndFrame - PbrSc->StartFrame) * (float)g_FrameTime / (float)PbrPartTime;

		prof.switchTo(PROF_ANIM);
		Dynamic_Camera();
		Animate_Objects(PbrSc, View);

		prof.switchTo(PROF_XFRM);
		Transform_Objects(PbrSc, fds::g_mainCamera, fds::g_mainFaces);

		prof.switchTo(PROF_LGHT);
		Lighting(PbrSc);

		prof.leave(PROF_LGHT);
		if (!CAll) return true;
		prof.enter(PROF_SORT);
		Radix_Sort(FList, SList, CAll);

		prof.switchTo(PROF_RNDR);
		Render();
		prof.leave(PROF_RNDR);

		prof.enter(PROF_FLIP);
		Flip(MainSurf);
		prof.leave(PROF_FLIP);
		prof.endFrame();
		return true;
	}

	void cleanup() override {
		if (g_profilerActive) prof.dump();
		Timer -= PbrPartTime;
		waitBackspaceRelease();
		if (Timer < 0) Timer = 0;
	}
};
} // anonymous namespace

std::unique_ptr<SceneDriver> createPBRTestScene()
{
	// The whole point of this scene is exercising the deferred material
	// pipeline (PBR maps, env reflections) — native builds default
	// FDS_DEFERRED off, so opt in here. setDefault: explicit --no-deferred
	// / FDS_DEFERRED=0 still wins.
	using FF = fds::FeatureFlags;
	FF::setDefault(FF::BoolId::deferred, true);
	return std::make_unique<PBRTestScene>();
}

void Run_PBRTest()
{
	auto scene = createPBRTestScene();
	runSceneBlocking(*scene);
}
