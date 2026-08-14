#include "Rev.h"
#include <Threads.h>

#include "PBRTEST.H"
#include "FrameProfiler.h"
#include "MaterialImport.h"
#include "SceneTick.h"
#include "Scenes.h"
#include <RENDER/EnvBake.h>   // EnvReflection_RegisterCubeFaces (--pbrtest_studio)
#include <RENDER/EnvCube.h>   // EnvCube_FaceUVToDir
#include <cmath>
#include <memory>
#include <vector>

// PBR / env-reflection test scene. Not part of the demo sequence — a
// deterministic material lab for the editor + headless snapshots. Sources
// in Authoring/pbrtest (regen recipe in its README); modeled on CRASH.CPP,
// the smallest driver.

static Scene *PbrSc;

static int32_t PbrPartTime = 0;

// ── --pbrtest_studio: a synthetic STUDIO DOME instead of the room ────────────
//
// See the FeatureFlags.def entry for why this exists at all (short version: a
// conductor IS its reflection, and this scene's environment is a dim, top-dark,
// maximum-variance checkerboard, so a metal cannot be judged in it).
//
// The dome is ONE CLOSED-FORM FUNCTION OF DIRECTION. That is deliberate and it
// is the property the rendering test depends on: no scene state, no capture, no
// bake order, no probe placement — the same six faces every run, on any machine,
// reproducible from the constants below alone. Radiance is LINEAR 0-255, which
// is the EnvPanoLinear store's contract.
namespace {

// CALIBRATION, and why these numbers and not others: the dome is tuned so its
// whole-sphere mean radiance is 99.33/255 — the SAME as the room probe it
// replaces (ball_gloss16's six baked faces mean 99.30, measured via
// FDS_ENVBAKE_DUMP). Equal ENERGY, different DISTRIBUTION. That is the point:
// with brightness held fixed, any change in how a conductor reads is
// attributable to the environment's SHAPE — smooth and top-weighted here
// (up-hemisphere 145.0, down 53.6, ratio 2.70) against the room's dim top
// (its +Y face is 52.0, the darkest of the six) and checkerboard variance.
// A dome that was merely brighter would prove nothing.
//
// zenith + key = 0.90 < 1.0 BY CONSTRUCTION, so nothing clips (measured 0.000%
// of a 400k-direction sphere sample). Clipping would silently eat the key's
// energy and make the fixture resolution-dependent.
constexpr float kDomeGround  = 0.05f;   // below the horizon: no floor bounce,
                                        // no contact shadow — matching the
                                        // vendor reference, whose sphere bottom
                                        // sits at backdrop level
constexpr float kDomeHorizon = 0.30f;
constexpr float kDomeZenith  = 0.70f;
constexpr float kKeyX = 0.50f, kKeyY = 0.80f, kKeyZ = 0.33f;   // upper-front-right,
                                        // where the reference's own specular
                                        // hot spot puts it (elevation ~+55°)
constexpr float kKeyGain = 0.20f;
constexpr float kKeySharp = 8.0f;       // broad + soft: a softbox, not a sun

inline float smooth01(float e0, float e1, float x) {
	float t = (x - e0) / (e1 - e0);
	if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
	return t * t * (3.0f - 2.0f * t);
}

// Linear radiance (0..1) of the dome along unit direction d.
inline float StudioDomeRadiance(float dx, float dy, float dz) {
	// Sky gradient: ground below, horizon band, zenith above. The knee starts
	// slightly BELOW the horizon (-0.25) so the terminator is soft rather than
	// a hard line across every reflective surface.
	const float up = smooth01(-0.25f, 1.0f, dy);
	float L = kDomeGround + (kDomeHorizon - kDomeGround) * smooth01(-1.0f, -0.25f, dy);
	L += (kDomeZenith - kDomeHorizon) * up;
	// One broad key lobe.
	const float kn = 1.0f / std::sqrt(kKeyX*kKeyX + kKeyY*kKeyY + kKeyZ*kKeyZ);
	float c = dx*kKeyX*kn + dy*kKeyY*kn + dz*kKeyZ*kn;
	if (c > 0.0f) L += kKeyGain * std::pow(c, kKeySharp);
	return L;
}

// Build the six padded cube faces and hand them to the env store. Returns the
// number of materials that ended up pointing at the dome.
int PbrTest_ApplyStudioDome(Scene *sc, int faceRes) {
	if (!sc) return 0;
	std::vector<uint32_t> faces(size_t(6) * faceRes * faceRes);
	for (int f = 0; f < 6; ++f) {
		for (int y = 0; y < faceRes; ++y) {
			for (int x = 0; x < faceRes; ++x) {
				const float u = (x + 0.5f) / float(faceRes);
				const float v = (y + 0.5f) / float(faceRes);
				float dx, dy, dz;
				fds::EnvCube_FaceUVToDir(f, u, v, dx, dy, dz);
				float L = StudioDomeRadiance(dx, dy, dz) * 255.0f;
				if (L < 0.0f) L = 0.0f; else if (L > 255.0f) L = 255.0f;
				const uint32_t c = uint32_t(L + 0.5f);
				// Neutral grey dome — a colour cast here would be indistinguishable
				// from a material's own tint in the parity numbers.
				faces[(size_t(f) * faceRes + y) * faceRes + x] =
					0xFF000000u | (c << 16) | (c << 8) | c;
			}
		}
	}
	// Every material that would otherwise get a room probe (the same predicate
	// EnvBake uses: Reflection > 0 or a metalness map). The first registers the
	// store; the rest alias it, so the dome costs one store, not one per surface.
	int applied = 0, storeIdx = -1;
	const Vector bake(0.0f, 0.0f, 0.0f);
	for (Material *M = MatLib; M; M = M->Next) {
		if (M->RelScene != sc) continue;
		if (!(M->Reflection > 0.0f || M->MetallicMap)) continue;
		if (storeIdx < 0) {
			storeIdx = fds::EnvReflection_RegisterCubeFaces(
				sc, M, faces.data(), faceRes, faceRes, bake);
			if (storeIdx < 0) return 0;
		} else {
			fds::EnvReflection_AliasMaterial(sc, M, storeIdx);
		}
		++applied;
	}
	std::fprintf(stderr, "[PBRTEST-STUDIO] dome %d^2 x6 (ground %.2f horizon %.2f "
	             "zenith %.2f, key (%.2f %.2f %.2f) gain %.1f sharp %.0f) -> "
	             "%d material(s), 1 store\n", faceRes, kDomeGround, kDomeHorizon,
	             kDomeZenith, kKeyX, kKeyY, kKeyZ, kKeyGain, kKeySharp, applied);
	return applied;
}

} // namespace

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

	// --pbrtest_studio: swap the room probes for the synthetic dome. AFTER the
	// imports, because the qualifying predicate reads MetallicMap and a CLI
	// import is what puts one there. Default 0 = nothing registered = the room
	// bake happens exactly as before.
	if (const int studio = fds::FeatureFlags::pbrtest_studio()) {
		if (studio > 0) {
			using FF = fds::FeatureFlags;
			// The dome is only observable through the env lobe.
			FF::setDefault(FF::BoolId::env_refl, true);
			PbrTest_ApplyStudioDome(PbrSc, 256);
		}
	}
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
