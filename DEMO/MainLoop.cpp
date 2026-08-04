#ifdef __EMSCRIPTEN__

#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Base/FeatureFlags.h>
#include <Base/Omni.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include "MaterialEditor.h"

#include <emscripten.h>
#include <emscripten/bind.h>
#include <SDL.h>

#include "MainLoop.h"
#include "Rev.h"
#include "Resize.h"
#include "SDL2.h"
#include "SceneTick.h"
#include "Scenes.h"
#include "Threads.h"

// Set by pumpEvents on any pointer-down or key-down, consumed by the
// WAIT_GESTURE state to advance past the click-to-start hint.
static std::atomic<bool> g_userGesture{false};

// Per-scene init completion flags. Set by the background init thread as
// each Initialize_* returns; the state machine waits on the next one
// before instantiating that scene's driver.
static std::atomic<bool> g_glatoReady{false};
static std::atomic<bool> g_cityReady{false};
static std::atomic<bool> g_chaseReady{false};
static std::atomic<bool> g_fountainReady{false};
static std::atomic<bool> g_crashReady{false};
static std::atomic<bool> g_greetsReady{false};
static std::atomic<bool> g_initThreadDone{false};

static std::thread g_initThread;
static ModplayerHandle g_modHandle = nullptr;

// State machine over the demo's scene sequence. Each scene already
// exposes a SceneDriver factory; we walk the list one at a time,
// gating each transition on the corresponding init flag.
enum DemoState {
	WAIT_GESTURE,
	RUN_GLATO,
	RUN_CITY,
	RUN_CHASE,
	RUN_FOUNTAIN,
	RUN_CRASH,
	RUN_GREETS,
	FADE_OUT,   // animate the last frame of the outgoing scene to black
	            // via shader uniform; routes to g_postFadeState afterward.
	BLACK_OUT,  // one-tick yield so the black frame composites before
	            // DONE blocks main on audio/pool teardown. Only used
	            // when transitioning to DONE.
	DONE,
};
static int g_fadeFrame = 0;
static DemoState g_postFadeState = DONE;
// Inter-scene fade duration (~0.25s). End-of-demo uses the same since
// 0.25s is long enough to feel intentional but doesn't make ESC feel
// laggy.
static constexpr int kFadeFrames = 15;
static DemoState g_state = WAIT_GESTURE;
static std::unique_ptr<SceneDriver> g_currentDriver;
static bool g_currentDriverInitialized = false;

// LWO surface-editor mode (?editor in the URL → shell.html sets
// Module.revEditorMode). Instead of the demo sequence, init ONLY greets
// (mirror off — it clones+bakes every mesh, ~4.8 GB, past wasm's 4 GB cap) and
// render a frozen overview frame each tick so live surface edits show up.
static bool g_editorMode = false;
static int  g_editorFreezeTimer = 600;   // frozen animation/camera-spline time
static int  g_editorRenderFrames = 0;    // idle throttle: render only while > 0

// Orbit free-cam around the greets room centre. Mouse-drag = orbit, wheel =
// zoom (pumpEvents); applied to FC each rendered frame (updateEditorCamera).
static float  g_camYaw   = 0.6f;
static float  g_camPitch = 0.45f;
static float  g_camDist  = 48.0f;
static Vector g_camTarget;               // set in DemoBoot (greets room centre)

// Dynamic_Camera's momentum state + per-scene auto-calibrated base speed
// (FDS/CAMERAS/CAMERAS.CPP — plain globals, no header declares them). The
// editor tick zeroes FV/FT each frame so the free-cam responds instantly
// (see the editor-mode block in editorTick).
extern Vector FV, FT;
extern float  Vel_Speed;

// ── Editor scene registry ───────────────────────────────────────────────
// ?editor&scene=<name> picks which scene the editor boots (default greets).
// camTarget: fixed default orbit pivot; autoFrame=true computes it from the
// scene's world bbox after the first tick instead (greets keeps its known-
// good room-centre pose). joinGreetsBake: greets-only static-lightmap join.
struct EditorSceneDef {
	const char *name;
	void      (*init)();
	std::unique_ptr<SceneDriver> (*create)();
	bool        joinGreetsBake;
	bool        autoFrame;
	float       tx, ty, tz, dist;
};
static const EditorSceneDef kEditorScenes[] = {
	{ "greets",   &Initialize_Greets,   &createGreetsScene,   true,  false, 18.0f, 6.0f, -35.0f, 48.0f },
	{ "city",     &Initialize_City,     &createCityScene,     false, true,  0, 0, 0, 48.0f },
	{ "chase",    &Initialize_Chase,    &createChaseScene,    false, true,  0, 0, 0, 48.0f },
	{ "fountain", &Initialize_Fountain, &createFountainScene, false, true,  0, 0, 0, 48.0f },
	{ "crash",    &Initialize_Crash,    &createCrashScene,    false, true,  0, 0, 0, 48.0f },
	{ "pbrtest",  &Initialize_PBRTest,  &createPBRTestScene,  false, true,  0, 0, 0, 48.0f },
};
static const EditorSceneDef *g_editorScene = &kEditorScenes[0];
static std::atomic<bool> g_editorSceneReady{false};
static bool   g_editorCamSeeded = false; // first frame seeds orbit from scene cam
static bool   g_editorPlaying = false;   // play mode: scene time runs, View = scene camera

// Scancode translation: SDL2 reports HID, the legacy engine expects PS/2
// set-1. Same table as native main() loop. Returned legacy code or -1.
static int translateScancode(SDL_Scancode sc)
{
	switch (sc) {
	case SDL_SCANCODE_ESCAPE:    return ScESC;
	case SDL_SCANCODE_F1:        return ScF1;
	case SDL_SCANCODE_F2:        return ScF2;
	case SDL_SCANCODE_F11:       return ScF11;
	case SDL_SCANCODE_TAB:       return ScTab;
	case SDL_SCANCODE_SPACE:     return ScSpace;
	case SDL_SCANCODE_LCTRL:
	case SDL_SCANCODE_RCTRL:     return ScCtrl;
	case SDL_SCANCODE_LEFT:      return ScLeft;
	case SDL_SCANCODE_RIGHT:     return ScRight;
	case SDL_SCANCODE_UP:        return ScUp;
	case SDL_SCANCODE_DOWN:      return ScDown;
	case SDL_SCANCODE_HOME:      return ScHome;
	case SDL_SCANCODE_END:       return ScEnd;
	case SDL_SCANCODE_PAGEUP:    return ScPgUp;
	case SDL_SCANCODE_PAGEDOWN:  return ScPgDn;
	case SDL_SCANCODE_KP_MINUS:
	case SDL_SCANCODE_MINUS:     return ScGrayMinus;
	case SDL_SCANCODE_KP_PLUS:
	case SDL_SCANCODE_EQUALS:    return ScGrayPlus;
	case SDL_SCANCODE_LEFTBRACKET:  return ScOpenSq;
	case SDL_SCANCODE_RIGHTBRACKET: return ScCloseSq;
	case SDL_SCANCODE_COMMA:     return ScComma;
	case SDL_SCANCODE_PERIOD:    return ScPeriod;
	case SDL_SCANCODE_A:         return ScA;
	case SDL_SCANCODE_C:         return ScC;
	case SDL_SCANCODE_D:         return ScD;
	case SDL_SCANCODE_E:         return ScE;
	case SDL_SCANCODE_H:         return ScH;
	case SDL_SCANCODE_K:         return ScK;
	case SDL_SCANCODE_L:         return ScL;
	case SDL_SCANCODE_M:         return ScM;
	case SDL_SCANCODE_P:         return ScP;
	case SDL_SCANCODE_Q:         return ScQ;
	case SDL_SCANCODE_R:         return ScR;
	case SDL_SCANCODE_S:         return ScS;
	case SDL_SCANCODE_U:         return ScU;
	case SDL_SCANCODE_W:         return ScW;
	case SDL_SCANCODE_Y:         return ScY;
	case SDL_SCANCODE_Z:         return ScZ;
	default: return -1;
	}
}

// Drain SDL events at frame top: write keyboard, queue resize. Returns
// true if SDL_QUIT was received (caller should tear down the loop).
static bool pumpEvents()
{
	SDL_Event event;
	bool quit = false;
	while (SDL_PollEvent(&event)) {
		switch (event.type) {
		case SDL_QUIT:
			quit = true;
			break;
		case SDL_WINDOWEVENT:
			if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
			    event.window.event == SDL_WINDOWEVENT_RESIZED) {
				// Wasm: data1/data2 are physical pixels (JS pre-multiplies
				// by devicePixelRatio in SDL2_RequestSize).
				int px = event.window.data1, py = event.window.data2;
				if (px > 0 && py > 0) {
					g_pendingResize.store(pack_size(px, py));
						if (g_editorMode) rev::Editor_MarkDirty();
				}
			}
			break;
		case SDL_KEYDOWN:
		case SDL_KEYUP: {
			int legacy = translateScancode(event.key.keysym.scancode);
			if (legacy >= 0) Keyboard[legacy] = (event.type == SDL_KEYDOWN) ? 1 : 0;
			if (event.type == SDL_KEYDOWN) g_userGesture.store(true);
			break;
		}
		case SDL_MOUSEBUTTONDOWN:
		case SDL_FINGERDOWN:
			g_userGesture.store(true);
			break;
		case SDL_MOUSEMOTION:
			// Editor orbit: left-drag rotates the camera around the target.
			if (g_editorMode && (event.motion.state & SDL_BUTTON_LMASK)) {
				g_camYaw   -= event.motion.xrel * 0.008f;
				g_camPitch += event.motion.yrel * 0.008f;
				if (g_camPitch >  1.45f) g_camPitch =  1.45f;  // avoid pole flip
				if (g_camPitch < -1.45f) g_camPitch = -1.45f;
				rev::Editor_MarkDirty();
			}
			break;
		case SDL_MOUSEWHEEL:
			// Editor zoom is handled by the JS canvas wheel listener
			// (Module.editorZoom — float deltas). This SDL path used int
			// wheel.y: trackpad-pinch deltas < 1 truncated to 0, and the
			// (y > 0 ? 0.9 : 1.1) ternary turned EVERY pinch event into
			// zoom-OUT. Both handlers also fired per event, fighting each
			// other. SDL wheel is now ignored in editor mode.
			break;
		default:
			break;
		}
	}
	return quit;
}

void DemoBoot(ModplayerHandle modHandle)
{
	g_modHandle = modHandle;

	// Editor mode: shell.html sets Module.revEditorMode from the URL (?editor).
	// Skip the demo sequence + audio; init ONLY greets with the teleporter
	// mirror forced off (setParamFromText marks the flag explicitly-set so
	// greets's setDefault can't re-enable it), then render a frozen overview
	// frame each tick so live surface edits are visible.
	g_editorMode = EM_ASM_INT({ return (Module.revEditorMode ? 1 : 0); });
	if (g_editorMode) {
		// Apply the URL query string as feature flags via the SAME parser the CLI
		// uses (dash→underscore, =value, no-): paste your flag set, e.g.
		//   DEMO.html?editor&hdr&hdr-linear&greets-stone-tex&parallax&ssao&bloom
		// With no flags, fall back to a sensible full-look default. Two hard
		// overrides after parsing: deferred ON (the full material path — off by
		// default in wasm) and the teleporter mirror OFF (clones+bakes every
		// mesh, ~6 GB, past wasm's 4 GB cap). NOTE: --material-import reads native
		// filesystem paths that don't exist in the wasm VFS, so that PBR import is
		// native-only — it can't run here.
		std::vector<std::string> args;
		args.push_back("DEMO");
		// Full-pipeline defaults = the user's native greets flag set, so a plain
		// ?editor matches the native look (deferred-quarter + HDR + all the post
		// FX: anamorphic, lens ghosts, chromatic, vignette, DoF, SSAO/GTAO, AA,
		// PBR, omni/spot shadows, parallax). The teleporter mirror (--greets-mirror
		// / --mirror-rtt) is deliberately OMITTED: it clones+bakes every mesh (~6 GB,
		// past wasm's 4 GB cap) and isn't needed for editing. --material-import is
		// native-only (reads native FS paths); the browser PBR upload replaces it.
		static const char *def[] = {
			"--shadows", "--greets-omni-shadows", "--greets-omni-default-range=30",
			"--greets-omni-shadow-res=256", "--shadow-skip-animated", "--greets-spots",
			"--shadow-dynamic", "--shadow-lightmap-planar", "--shadow-lightmap-res=64",
			"--shadow-lightmap", "--cone-strength=2", "--bloom", "--disco-bloom=0",
			"--shard-deferred", "--greets-shard-fall-speed=0.8", "--greets-shard-randomness=0.8",
			"--hdr-linear", "--deferred-quarter", "--greets-shard-res=64", "--bloom-intensity=2",
			"--hdr-refl-gain=4", "--cone-fine-tiles", "--anamorphic", "--anamorphic_intensity=1.5",
			"--anamorphic_vert=0", "--anamorphic_decay=0.3", "--anamorphic_passes=3",
			"--lens_ghosts", "--lens_ghost_intensity=0.05", "--lens_ghost_count=0",
			"--lens_ghost_dispersal=0.01", "--lens_ghost_halo=0.01", "--chromatic",
			"--chromatic_amount=3", "--vignette", "--vignette_strength=1", "--dof",
			"--dof_range=20", "--dof_max=4", "--greets-stone-tex", "--ssao-downscale=2",
			"--ssao-gtao", "--ao_map_strength=1", "--parallax_strength=0.1", "--parallax",
			"--nmap_16bit", "--hdr", "--ssao", "--aa", "--pbr",
			// Editor-only default: env reflections ON, so the per-surface
			// 'reflection' slider works out of the box (the panorama bake
			// only fires for materials that actually set Reflection > 0 /
			// carry a metalness map — free otherwise). The demo sequence
			// keeps the flag's normal default (off). env_bake_fix = the
			// corrected auto-bake bundle (per-face projection, face-level
			// self-exclusion, metal neutralization) — kept off in the demo
			// only to preserve the pinned city baseline.
			"--env_refl", "--env_bake_fix",
			// Editor-only default: glass refraction ON so refractive-marked
			// surfaces (greets screens, fountain glass) show their real look
			// while editing. The demo keeps the flag's normal default (off).
			"--glass-refract=1",
			// Editor-only default: PBR-shaded transparents, so dialing the
			// transparency slider up on a PBR material keeps its normal-map/
			// AO/roughness/metallic look instead of degrading to a flat
			// transparent texture. The demo keeps the flag's default (off).
			"--xpar-pbr",
				// Editor-only default: arm the live displacement rebuild, so the
				// Displacement panel's mode buttons actually take. Without it
				// --pom_shell and the cone / horizon bake flags are consumed once
				// at scene init and a live edit changes a bool nothing re-reads.
				// The flag renders nothing differently on its own: it takes a
				// PRISTINE snapshot of the stone at the end of init and defers a
				// requested --pom_shell into the rebuild path, so repeated mode
				// switches cannot compound the lid offset. The demo keeps it off.
				"--pom_rebuild",
			// (city headlights: the front-row pairs are AUTHORED lights now —
			// 46 parented "city headlight L/R" spots in CITY1.LWS / CITY.FLD,
			// Omni_SceneAuthored, so they appear in the lights list grouped
			// under their vehicles and are LWS-persistable. The retired code
			// schemes remain behind --city-headlights /
			// --city-headlights-front for A/B comparison.)
		};
		for (const char *d : def) args.push_back(d);
		// URL query flags applied AFTER the defaults so ?editor&no-bloom&dof_range=8
		// overrides a default (later wins; setParamFromText marks explicitly-set).
		// scene=<name> picks the edited scene (registry above) and is excluded
		// from the flag list.
		if (const char *qs = emscripten_run_script_string("location.search.replace(/^[?]/,'')")) {
			std::string q = qs, tok;
			for (size_t i = 0; i <= q.size(); ++i) {
				if (i == q.size() || q[i] == '&') {
					if (tok.rfind("scene=", 0) == 0) {
						const std::string want = tok.substr(6);
						for (const EditorSceneDef &s : kEditorScenes)
							if (want == s.name) g_editorScene = &s;
					} else if (!tok.empty() && tok != "editor") {
						args.push_back("--" + tok);
					}
					tok.clear();
				} else tok.push_back(q[i]);
			}
		}
		std::vector<const char*> argv;
		for (auto &a : args) argv.push_back(a.c_str());
		fds::FeatureFlags::parseArgs((int)argv.size(), argv.data());
		// Mirrors default OFF in the editor (historical wasm-memory caution —
		// Editor default: mirrors ON (user request) — the clone/bake overhead is
		// ~15 MB now, well inside the wasm heap. ?editor&no-greets-mirror /
		// &no-mirror-rtt in the URL still win (isSet guards both directions).
		if (!fds::FeatureFlags::isSet(fds::FeatureFlags::BoolId::greets_mirror))
			fds::FeatureFlags::setParamFromText("greets_mirror", "1");
		if (!fds::FeatureFlags::isSet(fds::FeatureFlags::BoolId::mirror_rtt))
			fds::FeatureFlags::setParamFromText("mirror_rtt", "1");
		fprintf(stderr, "[EDITOR] %s editor: full native pipeline (%d flags), mirror %s\n",
		        g_editorScene->name, (int)args.size() - 1,
		        fds::FeatureFlags::greets_mirror() ? "ON (URL opt-in)" : "off (default)");
		// Default orbit pivot (greets: room-bbox centre; autoFrame scenes
		// recompute from the world bbox after the first tick).
		g_camTarget.x = g_editorScene->tx; g_camTarget.y = g_editorScene->ty; g_camTarget.z = g_editorScene->tz;
		g_camDist = g_editorScene->dist;
		// NOTE: the scene init thread is NOT spawned here — editorTick spawns
		// it once the shell's live-FLD fetch resolves (see editorMaybeSpawnInit),
		// so a freshly saved FLD/sidecar can be installed into MEMFS over the
		// link-time preloaded copies BEFORE the scene init opens them.
		return;
	}

	// Click-to-start hint. Browsers gate AudioContext on user gesture,
	// and we want audio in lockstep with scene playback.
	EM_ASM({
		var hint = document.createElement('div');
		hint.id = 'rev-click-hint';
		hint.textContent = 'Click or press any key to start';
		hint.style.cssText =
			'position:fixed;top:50%;left:50%;transform:translate(-50%,-50%);' +
			'padding:1em 2em;background:rgba(0,0,0,0.75);color:#fff;' +
			'font:18px/1.2 sans-serif;border-radius:4px;z-index:9999;' +
			'pointer-events:none;';
		document.body.appendChild(hint);
	});

	// Spawn one background thread that runs ALL Initialize_* in sequence.
	// Glato init is short and on the critical path (Run_Glato can't tick
	// until it's done); the rest finish during Glato playback. Each flag
	// fires immediately after its init returns so the state machine can
	// pick up the slack as soon as it's allowed to advance.
	g_initThread = std::thread([](){
		HintHighPerfThread();
		InitPolyStats(200);
		FPU_LPrecision();

		Initialize_Glato();
		g_glatoReady.store(true);
		Initialize_City();
		g_cityReady.store(true);
		Initialize_Chase();
		g_chaseReady.store(true);
		Initialize_Fountain();
		g_fountainReady.store(true);
		Initialize_Crash();
		g_crashReady.store(true);
		Initialize_Greets();
		g_greetsReady.store(true);
		g_initThreadDone.store(true);
		fprintf(stderr, "[DEMO] all inits complete\n");
	});
}

// State helpers --------------------------------------------------------

static void startScene(std::unique_ptr<SceneDriver> driver)
{
	g_currentDriver = std::move(driver);
	g_currentDriverInitialized = false;
	Timer = 0;
	// Symmetric with the FADE_OUT state: the next kFadeFrames V_Flip
	// calls will fade VPage up from black.
	EngineStartFadeIn(kFadeFrames);
}

static void advanceFromState(DemoState newState,
                             std::unique_ptr<SceneDriver> driver)
{
	g_state = newState;
	startScene(std::move(driver));
}

// Returns true if the current scene's tick produced a frame (or we're
// still waiting for its init); false if it's finished and we should
// advance.
static bool tickCurrentScene()
{
	if (!g_currentDriver) return false;
	if (!g_currentDriverInitialized) {
		g_currentDriver->init();
		g_currentDriverInitialized = true;
	}
	poll_pending_resize(g_currentDriver.get());
	return g_currentDriver->tick();
}

static void cleanupCurrentScene()
{
	if (!g_currentDriver) return;
	// Each scene's cleanup() ends with `while (Keyboard[ScESC]) continue;`
	// — a DOS-era debounce so the next scene doesn't immediately exit on
	// the same ESC press. On wasm main thread that loop blocks the rAF
	// callback (no events drain), freezing the page until the user
	// somehow makes Keyboard[ScESC] flip to 0 — which they can't, because
	// the keyup event is also stuck. Clear it pre-cleanup; we get the
	// debounce effect for free since the next scene's first tick sees 0.
	Keyboard[ScESC] = 0;
	g_currentDriver->cleanup();
	g_currentDriver.reset();
	g_currentDriverInitialized = false;
}

// Seed the orbit (yaw/pitch/dist/target) from whatever camera the scene used on
// the first rendered frame — greets's spline at the frozen time — so the editor
// opens framed exactly like the demo's t=600 shot instead of an arbitrary far
// pose. forward = Mat row 2 (same as the [CAM] dump); orbit eye→target = forward.
static void seedOrbitFromView()
{
	if (!View) return;
	const Vector eye = View->ISource;
	float fx = View->Mat[2][0], fy = View->Mat[2][1], fz = View->Mat[2][2];
	const float D = 26.0f;                 // approx eye→subject distance for greets
	g_camTarget.x = eye.x + fx * D;
	g_camTarget.y = eye.y + fy * D;
	g_camTarget.z = eye.z + fz * D;
	g_camDist  = D;
	float syp = -fy; if (syp < -1.0f) syp = -1.0f; if (syp > 1.0f) syp = 1.0f;
	g_camPitch = std::asin(syp);
	g_camYaw   = std::atan2(-fx, -fz);
	if (View->IFOV > 0.0f) FC.IFOV = View->IFOV;
}

// Build the orbit camera from (yaw,pitch,dist) around g_camTarget and pin View
// to it. With View==&FC, greets's Animate_Objects leaves the camera alone — only
// object animation tracks Timer, which we freeze — so the orbit is stable.
static void updateEditorCamera()
{
	const float cp = std::cos(g_camPitch), sp = std::sin(g_camPitch);
	const float cy = std::cos(g_camYaw),   sy = std::sin(g_camYaw);
	FC.ISource.x = g_camTarget.x + g_camDist * cp * sy;
	FC.ISource.y = g_camTarget.y + g_camDist * sp;
	FC.ISource.z = g_camTarget.z + g_camDist * cp * cy;
	Vector look = g_camTarget;
	Kick_Camera(&FC.ISource, &look, 0.0f, FC.Mat);
	if (FC.IFOV <= 0.0f) FC.IFOV = 65.0f;
	CalcPersp(&FC);
	View = &FC;
}

// True if any of Dynamic_Camera's fly/look/speed keys is held — keeps the idle
// throttle rendering while you fly.
static bool anyFreeCamKey()
{
	return Keyboard[ScA] || Keyboard[ScD] || Keyboard[ScW] || Keyboard[ScS] || Keyboard[ScZ] ||
	       Keyboard[ScQ] || Keyboard[ScE] || Keyboard[ScLeft] || Keyboard[ScRight] ||
	       Keyboard[ScUp] || Keyboard[ScDown] || Keyboard[ScHome] || Keyboard[ScPgUp] ||
	       Keyboard[ScPgDn] || Keyboard[ScEnd] || Keyboard[ScGrayPlus] || Keyboard[ScGrayMinus] ||
	       Keyboard[ScComma] || Keyboard[ScPeriod] || Keyboard[ScK] || Keyboard[ScL];
}

// After Dynamic_Camera flew FC, re-derive the orbit state from FC so a later
// mouse-orbit pivots around where you flew to. Keeps g_camDist; the pivot sits
// dist ahead along the new forward (FC.Mat row 2 = look direction).
static void syncOrbitFromFC()
{
	const float Fx = FC.Mat[2][0], Fy = FC.Mat[2][1], Fz = FC.Mat[2][2];
	g_camTarget.x = FC.ISource.x + Fx * g_camDist;
	g_camTarget.y = FC.ISource.y + Fy * g_camDist;
	g_camTarget.z = FC.ISource.z + Fz * g_camDist;
	float sp = -Fy; if (sp < -1.0f) sp = -1.0f; if (sp > 1.0f) sp = 1.0f;
	g_camPitch = std::asin(sp);
	g_camYaw   = std::atan2(-Fx, -Fz);
}

// Editor mode tick: bring greets up once (mirror off, bake joined). Camera is
// the engine free-cam (Dynamic_Camera: WASD/QE fly, arrow look, ,. / KL speed —
// the SAME handling as TAB in the demo) while keys are held, else the mouse
// orbit. Renders only while something changed (idle throttle).
// Editor scene init, deferred until the shell's live-FLD fetch settles: the
// dev server (tools/editor_server.py) serves the current Runtime/SCENES/
// GREETS.FLD; installing it over the link-time preloaded MEMFS copy is what
// makes save→reload pick up persisted LWO edits without relinking DEMO.data.
// Fetch failed (-1, e.g. static hosting without the server) → baked copy.
static bool g_editorInitSpawned = false;
static void editorMaybeSpawnInit()
{
	if (g_editorInitSpawned) return;
	const int st = EM_ASM_INT({ return (Module.editorFldReady | 0); });
	if (st == 0) return;                          // fetch still in flight
	if (st == 1) {
		EM_ASM({
			// Install every live-fetched file (FLD + PBR sidecar + the maps it
			// references) over the link-time preloaded MEMFS copies.
			(Module.editorFreshFiles || []).forEach(function(f) {
				try {
					var dir = f.path.substring(0, f.path.lastIndexOf('/'));
					if (dir) FS.mkdirTree(dir);
					FS.writeFile(f.path, f.data);
					console.log('[editor] live install ' + f.path + ' (' + f.data.length + ' bytes)');
				} catch (e) {
					console.warn('[editor] live install failed for ' + f.path + ':', e);
				}
			});
		});
	}
	g_editorInitSpawned = true;
	g_initThread = std::thread([](){
		HintHighPerfThread();
		InitPolyStats(200);
		FPU_LPrecision();
		g_editorScene->init();
		g_editorSceneReady.store(true);
		g_initThreadDone.store(true);
		fprintf(stderr, "[EDITOR] %s init complete\n", g_editorScene->name);
	});
}

// Mirror-clone meshes (GreetsMirror's "__mirrorClone_*" objects) duplicate
// world geometry MIRRORED BEHIND the glass and reuse the base surface
// names — any camera math that unions face/vertex positions by name must
// skip them or the pivot lands between the real object and its phantom
// reflection ("focus goes somewhere" with ?greets-mirror on).
static bool editorMeshIsMirrorClone(const TriMesh *T)
{
	if (!CurScene) return false;
	static const TriMesh *sCloneCache[64];
	static int  sCloneCount = -1;
	static const Scene *sCloneScene = nullptr;
	if (sCloneScene != CurScene || sCloneCount < 0) {
		sCloneScene = CurScene;
		sCloneCount = 0;
		for (Object *Obj = CurScene->ObjectHead; Obj; Obj = Obj->Next) {
			if (Obj->Type != Obj_TriMesh || !Obj->Name || !Obj->Data) continue;
			if (std::strncmp(Obj->Name, "__mirrorClone_", 14) != 0) continue;
			if (sCloneCount < 64)
				sCloneCache[sCloneCount++] = (const TriMesh *)Obj->Data;
		}
	}
	for (int i = 0; i < sCloneCount; ++i)
		if (sCloneCache[i] == T) return true;
	return false;
}

// Auto-frame: world bbox of every mesh (RotMat·Pos + IPos — valid after the
// first tick's Animate) → orbit pivot at the centre, distance to fit. Used by
// scenes without a hand-tuned default pose (city/chase/fountain).
static void editorAutoFrame()
{
	if (!CurScene) return;
	float lox = 1e30f, loy = 1e30f, loz = 1e30f, hix = -1e30f, hiy = -1e30f, hiz = -1e30f;
	long n = 0;
	for (TriMesh *T = CurScene->TriMeshHead; T; T = T->Next) {
		if (editorMeshIsMirrorClone(T)) continue;
		for (DWord v = 0; v < T->VIndex; ++v) {
			Vector w;
			MatrixXVector(T->RotMat, &T->Verts[v].Pos, &w);
			Vector_SelfAdd(&w, &T->IPos);
			if (w.x < lox) lox = w.x; if (w.y < loy) loy = w.y; if (w.z < loz) loz = w.z;
			if (w.x > hix) hix = w.x; if (w.y > hiy) hiy = w.y; if (w.z > hiz) hiz = w.z;
			++n;
		}
	}
	if (!n) return;
	g_camTarget.x = (lox + hix) * 0.5f;
	g_camTarget.y = (loy + hiy) * 0.5f;
	g_camTarget.z = (loz + hiz) * 0.5f;
	const float dx = hix - lox, dy = hiy - loy, dz = hiz - loz;
	float d = std::sqrt(dx*dx + dy*dy + dz*dz) * 0.6f;
	if (d < 6.0f) d = 6.0f;
	if (d > 600.0f) d = 600.0f;
	g_camDist = d;
	fprintf(stderr, "[EDITOR] auto-frame: %ld verts, centre (%.1f %.1f %.1f) dist %.1f\n",
	        n, g_camTarget.x, g_camTarget.y, g_camTarget.z, g_camDist);
}

static void editorTick()
{
	if (!g_currentDriver) {
		editorMaybeSpawnInit();
		if (!g_editorSceneReady.load()) return;   // init thread still loading
		if (g_editorScene->joinGreetsBake)
			Greets_JoinBakeThread();              // finish the static lightmap bake
		g_currentDriver = g_editorScene->create();
		g_currentDriver->init();
		g_currentDriverInitialized = true;
		EngineStartFadeIn(kFadeFrames);
		g_editorRenderFrames = kFadeFrames + 1;   // play the fade-in
		Init_FreeCamera();
		if (CurScene) Calibrate_FreeCamera_ForScene(CurScene->FZP, CurScene->CameraHead);
		fprintf(stderr, "[EDITOR] %s up — fly: WASD/QE + arrows; mouse-drag orbit; wheel zoom; click a surface to focus\n",
		        g_editorScene->name);
	}

	Keyboard[ScESC] = 0;                          // don't let the scene self-exit
	if (anyFreeCamKey()) rev::Editor_MarkDirty();  // keep rendering while flying
	if (g_editorPlaying) rev::Editor_MarkDirty(); // play mode renders every frame

	// Idle throttle: render only while something changed (edit / camera / key);
	// otherwise RE-PRESENT the last frame so the WebGL canvas stays alive (it
	// goes black if nothing is drawn) without paying the deferred re-render.
	// Draw-only: the full V_Flip re-uploaded the whole framebuffer each rAF
	// (~300 MB/s of GPU-process work at fullscreen for identical pixels).
	if (rev::Editor_ConsumeDirty() && g_editorRenderFrames < 4) g_editorRenderFrames = 4;
	if (g_editorRenderFrames <= 0) {
		SDL2_Wasm_RepresentLast();
		return;
	}
	--g_editorRenderFrames;

	// Play mode: run the scene exactly like the demo does — Timer free-runs
	// (TimerProc keeps bumping it; the driver's tickSceneTimer derives dTime
	// from it) and View is the scene's spline camera, which Animate_Objects
	// moves along its FLD path. Loops back to t=0 at the scene's real end:
	// partTime() (Timer units — the `Timer >= PartTime` each scene exits
	// on), NOT Scene::EndFrame, which is FLD-spline frames that every scene
	// maps Timer onto through its own scale (greets looped ~4× early).
	// g_editorFreezeTimer tracks playback so stopping freezes where you are
	// (and the [ ] scrub / UI readout stay coherent).
	if (g_editorPlaying && g_editorCamSeeded && CurScene) {
		const int32_t endT = g_currentDriver->partTime();
		if (endT > 0 && Timer.load() >= endT)
			Timer = 0;
		if (CurScene->CameraHead) View = CurScene->CameraHead;
		poll_pending_resize(g_currentDriver.get());
		g_currentDriver->tick();
		g_editorFreezeTimer = Timer.load();
		return;
	}

	Timer = g_editorFreezeTimer;
	if (!g_editorCamSeeded) {
		// Open on the fixed default orbit (room centre). The earlier auto-seed
		// from the scene's spline camera produced a through-the-floor pose
		// (target below the floor) → black; the fixed default frames the room.
		updateEditorCamera();          // build FC from the default orbit
		poll_pending_resize(g_currentDriver.get());
		g_currentDriver->tick();
		g_editorCamSeeded = true;
		if (g_editorScene->autoFrame) {
			// Meshes now have valid RotMat/IPos (the tick ran Animate) —
			// reframe on the scene bbox and re-render next frame.
			editorAutoFrame();
			updateEditorCamera();
			rev::Editor_MarkDirty();
		}
	} else {
		if (anyFreeCamKey()) {
			// Fixed step in the free-cam's tuned regime (~0.25 Timer ticks per
			// frame — Rot_Speed_Base's comments assume it). The earlier 16.0
			// (a "milliseconds" value pasted into Timer-tick units) made one
			// held-arrow frame rotate 0.045*16 ≈ 41°: rotation read as broken.
			dTime = 0.25f;             // (Timer is frozen → no scene dTime)
			// EDITOR-ONLY instant response: Dynamic_Camera's exponential
			// velocity decay carries momentum across frames — fine at demo
			// frame rates, but under the editor's render-on-demand loop the
			// leftover FV from the LAST key burst lurches the camera in the
			// old direction when a new key lands. Zero the carried velocity
			// (translation FV + angular FT) so each tick's motion is rebuilt
			// from the CURRENT key state alone, and compensate the lost
			// steady-state accumulation by boosting the per-key add by
			// 1/(1-exp(-Vel_FallOff*dTime)) (Vel_FallOff = 5*Vel_Speed inside
			// Dynamic_Camera) — cruise speed matches the demo free-cam feel,
			// but direction changes and stops are immediate. The native demo
			// path (TAB free-cam) is untouched — this only runs in editorTick.
			FV.x = FV.y = FV.z = 0.0f;
			FT.x = FT.y = FT.z = 0.0f;
			const float velSaved = Vel_Speed;
			const float kDecay = 1.0f - std::exp(-5.0f * Vel_Speed * dTime);
			if (kDecay > 1e-6f && kDecay < 1.0f) Vel_Speed /= kDecay;
			Dynamic_Camera();          // keyboard fly + look (the TAB free-cam)
			Vel_Speed = velSaved;      // boost was for this tick's add only
			CalcPersp(&FC);
			View = &FC;
			syncOrbitFromFC();         // orbit pivot tracks where we flew
		} else {
			updateEditorCamera();      // mouse orbit/pan/zoom → FC
		}
		poll_pending_resize(g_currentDriver.get());
		g_currentDriver->tick();
	}
}

bool DemoTick()
{
	bool quit = pumpEvents();

	if (g_editorMode) {
		if (quit) return false;
		editorTick();
		return true;
	}

	if (quit) {
		g_state = DONE;
	}

	switch (g_state) {
	case WAIT_GESTURE: {
		if (!g_userGesture.load()) break;
		EM_ASM({
			var h = document.getElementById('rev-click-hint');
			if (h) h.remove();
		});
		fprintf(stderr, "[DEMO] user gesture observed, starting demo\n");
		// Editor mode is silent: it never runs the demo sequence, and starting
		// the modplayer feeds the SDL AudioWorklet, whose callback locks the
		// Rust modplayer mutex. Under -sALLOW_MEMORY_GROWTH + -pthread a
		// memory-growth event (baking env probes in a long editor session)
		// detaches the worklet thread's SharedArrayBuffer view, so the next
		// lock traps on an unaligned atomic ("operation does not support
		// unaligned accesses" in futex Mutex::lock). No music in the editor =
		// no worklet = no trap. (The comment at DemoBoot already intended
		// "skip audio" in editor mode; this is where it wasn't honored.)
		if (g_modHandle && !g_editorMode) {
			Modplayer_Start(g_modHandle);
			SDL2_StartMusic(g_modHandle);
		}
		Timer = 0;
		g_state = RUN_GLATO;
		break;
	}

	// Helper: end-of-scene transition. Cleans up the current driver, then
	// runs FADE_OUT for kFadeFrames before entering `next`. If next is
	// DONE the routing falls through BLACK_OUT first to give the browser
	// a chance to paint the all-black frame before the audio/pool
	// teardown blocks main.
	#define ADVANCE_AFTER_FADE(next) do {                              \
			cleanupCurrentScene();                                     \
			g_fadeFrame = 0;                                           \
			g_postFadeState = (next);                                  \
			g_state = FADE_OUT;                                        \
		} while (0)

	case RUN_GLATO: {
		if (!g_currentDriver) {
			if (!g_glatoReady.load()) break; // init thread still loading
			startScene(createGlatoScene());
		}
		if (!tickCurrentScene()) ADVANCE_AFTER_FADE(RUN_CITY);
		break;
	}

	case RUN_CITY: {
		if (!g_currentDriver) {
			if (!g_cityReady.load()) break;
			startScene(createCityScene());
		}
		if (!tickCurrentScene()) ADVANCE_AFTER_FADE(RUN_CHASE);
		break;
	}

	case RUN_CHASE: {
		if (!g_currentDriver) {
			if (!g_chaseReady.load()) break;
			startScene(createChaseScene());
		}
		if (!tickCurrentScene()) ADVANCE_AFTER_FADE(RUN_FOUNTAIN);
		break;
	}

	case RUN_FOUNTAIN: {
		if (!g_currentDriver) {
			if (!g_fountainReady.load()) break;
			startScene(createFountainScene());
		}
		if (!tickCurrentScene()) ADVANCE_AFTER_FADE(RUN_CRASH);
		break;
	}

	case RUN_CRASH: {
		if (!g_currentDriver) {
			if (!g_crashReady.load()) break;
			startScene(createCrashScene());
		}
		if (!tickCurrentScene()) ADVANCE_AFTER_FADE(RUN_GREETS);
		break;
	}

	case RUN_GREETS: {
		if (!g_currentDriver) {
			if (!g_greetsReady.load()) break;
			startScene(createGreetsScene());
		}
		if (!tickCurrentScene()) ADVANCE_AFTER_FADE(DONE);
		break;
	}

	case FADE_OUT: {
		// One step of the engine's in-place alpha-blend fade. Same
		// SIMD AlphaBlend primitive that Glat uses for its smear/
		// composite passes — applied to VPage in place, then Flip.
		// Cumulative per-frame factor gives a linear V_0→0 fade over
		// kFadeFrames calls.
		engineFadeStep(g_fadeFrame, kFadeFrames);
		if (++g_fadeFrame >= kFadeFrames) {
			if (g_postFadeState == DONE) {
				g_state = BLACK_OUT;
			} else {
				g_state = g_postFadeState;
			}
		}
		break;
	}

	case BLACK_OUT: {
		// One-tick yield so the final black frame composites before DONE
		// blocks main on audio/pool teardown.
		g_state = DONE;
		break;
	}

	case DONE: {
		cleanupCurrentScene();
		// Stop audio synchronously on main: SDL2_StopMusic on wasm is
		// just a JS call that disconnects the AudioWorkletNode + closes
		// the AudioContext. No proxy hop needed (we're already on
		// browser-main), no measurable delay. Doing this in the
		// detached teardown thread instead caused a window where
		// emscripten_cancel_main_loop had already fired but the
		// detached thread hadn't yet proxied the JS call back to main
		// — and the user kept hearing audio after the demo ended.
		if (g_modHandle) SDL2_StopMusic();
		// The slower bits — modplayer's internal stop + ThreadPool join
		// — go to a detached worker so they don't block main's rAF tick.
		// EXIT_RUNTIME=0 keeps the runtime alive until they finish.
		ModplayerHandle handleSnapshot = g_modHandle;
		std::thread teardown([handleSnapshot]() {
			if (handleSnapshot) Modplayer_Stop(handleSnapshot);
			if (g_initThread.joinable()) g_initThread.join();
			ThreadPool::instance().close();
			fprintf(stderr, "[DEMO] teardown complete\n");
		});
		teardown.detach();
		g_modHandle = nullptr;
		return false;
	}
	}
	return true;
}

// JS-driven editor camera (shell.html canvas handlers call these). SDL mouse
// events proved unreliable for the orbit in-browser, so the canvas drag/wheel is
// handled in JS and routed here via Embind. Updates the orbit state + marks the
// view dirty so the idle-throttled loop renders the move.
namespace {
void editorOrbit(float dxPixels, float dyPixels)
{
	g_camYaw   -= dxPixels * 0.008f;
	g_camPitch += dyPixels * 0.008f;
	if (g_camPitch >  1.45f) g_camPitch =  1.45f;
	if (g_camPitch < -1.45f) g_camPitch = -1.45f;
	rev::Editor_MarkDirty();
}
// Scene-time scrub: step the frozen animation clock (objects/splines evaluate
// at Timer = g_editorFreezeTimer each tick). Fwd/back through the scene's
// animation without unfreezing it. Returns the new time for the UI readout.
float editorTimeStep(float delta)
{
	g_editorFreezeTimer += (int)delta;
	if (g_editorFreezeTimer < 0) g_editorFreezeTimer = 0;
	if (g_editorPlaying) Timer = g_editorFreezeTimer;  // seek within playback
	rev::Editor_MarkDirty();
	return (float)g_editorFreezeTimer;
}
// Play/stop the scene from the demo's camera. Starting resumes from the
// current frozen time (compose with the [ ] scrub to pick a shot); stopping
// re-freezes at wherever playback got to and hands the view back to the
// editor orbit camera. Returns the scene time for the UI readout.
float editorPlayScene(bool on)
{
	if (on) {
		Timer = g_editorFreezeTimer;          // resume from the frozen t
	} else {
		g_editorFreezeTimer = Timer.load();
		// Continue editing from the shot you stopped on: re-seed the orbit
		// from the scene camera's pose instead of snapping back to the
		// pre-play editor pose. KEEP the editor's own FOV though —
		// seedOrbitFromView copies View->IFOV, and inheriting the scene
		// camera's FOV silently changed how every later click-to-focus
		// framed its subject.
		const float editorFov = FC.IFOV;
		seedOrbitFromView();
		FC.IFOV = editorFov;
	}
	g_editorPlaying = on;
	rev::Editor_MarkDirty();
	fprintf(stderr, "[EDITOR] play %s at t=%d\n", on ? "ON (scene camera)" : "off", g_editorFreezeTimer);
	return (float)g_editorFreezeTimer;
}
// Live scene-time readout while playing (g_editorFreezeTimer tracks playback).
float editorSceneTime() { return (float)g_editorFreezeTimer; }
// Camera-state dump for headless debugging (focus/orbit investigations).
std::string editorCamDebug()
{
	char buf[512];
	std::snprintf(buf, sizeof(buf),
		"{\"target\":[%.2f,%.2f,%.2f],\"yaw\":%.3f,\"pitch\":%.3f,\"dist\":%.2f,"
		"\"eye\":[%.2f,%.2f,%.2f],\"fwd\":[%.3f,%.3f,%.3f],\"ifov\":%.1f,"
		"\"viewIsFC\":%d,\"playing\":%d,\"seeded\":%d}",
		g_camTarget.x, g_camTarget.y, g_camTarget.z,
		g_camYaw, g_camPitch, g_camDist,
		FC.ISource.x, FC.ISource.y, FC.ISource.z,
		FC.Mat[2][0], FC.Mat[2][1], FC.Mat[2][2], FC.IFOV,
		View == &FC ? 1 : 0, g_editorPlaying ? 1 : 0, g_editorCamSeeded ? 1 : 0);
	return buf;
}
// Absolute seek (the progress-bar drag). Works frozen and mid-playback.
float editorTimeSet(float t)
{
	g_editorFreezeTimer = (int)t;
	if (g_editorFreezeTimer < 0) g_editorFreezeTimer = 0;
	if (g_editorPlaying) Timer = g_editorFreezeTimer;
	rev::Editor_MarkDirty();
	return (float)g_editorFreezeTimer;
}
// Scene duration in Timer units (the driver's *PartTime) — sizes the
// progress bar. 0 until the scene driver is up.
float editorSceneLength()
{
	return g_currentDriver ? (float)g_currentDriver->partTime() : 0.0f;
}
// Raw wheel deltaY → proportional, smooth zoom (trackpad fires many small
// events; a fixed per-event ratio made it lurch). exp() keeps it multiplicative.
void editorZoom(float wheelDeltaY)
{
	g_camDist *= std::exp(wheelDeltaY * 0.0015f);
	if (g_camDist <   3.0f) g_camDist =   3.0f;
	if (g_camDist > 600.0f) g_camDist = 600.0f;
	rev::Editor_MarkDirty();
}
// Pan the orbit target in the camera's view plane (right-drag / shift-drag).
// FC.Mat row 0 = camera right, row 1 = up; scale by distance so it feels the
// same near and far.
void editorPan(float dxPixels, float dyPixels)
{
	const float k = g_camDist * 0.0018f;
	g_camTarget.x += (-dxPixels * FC.Mat[0][0] + dyPixels * FC.Mat[1][0]) * k;
	g_camTarget.y += (-dxPixels * FC.Mat[0][1] + dyPixels * FC.Mat[1][1]) * k;
	g_camTarget.z += (-dxPixels * FC.Mat[0][2] + dyPixels * FC.Mat[1][2]) * k;
	rev::Editor_MarkDirty();
}
// After a focus set g_camTarget/g_camDist, aim the orbit FROM THE CURRENT
// CAMERA POSITION: yaw/pitch face the new target from where you already
// are, so focusing means "turn toward it and dolly in". Keeping the OLD
// yaw/pitch teleported the eye to `target + dist·old_direction` — an
// arbitrary vantage that could sit behind walls or view the object from
// its far side ("non-centered view from somewhere" until the first
// play-stop happened to reseed a sensible direction).
static void orbitFaceTargetFromHere()
{
	const float fx = g_camTarget.x - FC.ISource.x;
	const float fy = g_camTarget.y - FC.ISource.y;
	const float fz = g_camTarget.z - FC.ISource.z;
	const float len2 = fx*fx + fy*fy + fz*fz;
	if (len2 < 1e-4f) return;                 // on top of it — keep direction
	const float inv = 1.0f / std::sqrt(len2);
	float sp = -fy * inv;
	if (sp < -1.0f) sp = -1.0f; if (sp > 1.0f) sp = 1.0f;
	g_camPitch = std::asin(sp);
	if (g_camPitch >  1.45f) g_camPitch =  1.45f;
	if (g_camPitch < -1.45f) g_camPitch = -1.45f;
	g_camYaw = std::atan2(-fx * inv, -fz * inv);
}
// Frame the orbit on the SELECTED surface(s) IN CONTEXT: the shared focus
// core (rev::Editor_ComputeFocus — face gather + nearest-instance clustering,
// also exercised natively by the FOCUS_TEST hook) yields the object's world
// centre + bounding radius; the orbit pivots on the centre and dollies to
// 2.5× the radius along the direction the camera already was — the object
// centres at ~1/3 of the view with the rest of the scene still visible
// (nothing is hidden). `name` may be a ';'-separated list (object selection):
// clustering applies there too, so a multi-instance object (the city taxis)
// frames the instance nearest the camera instead of a mid-air union centroid.
void editorFocusSurface(std::string name)
{
	if (!CurScene) return;
	Vector c;
	float r = 0.0f;
	long used = 0;
	unsigned long total = 0;
	if (!rev::Editor_ComputeFocus(name.c_str(), FC.ISource, c, r, &used, &total)) {
		std::fprintf(stderr, "[EDITOR] focus '%s': no faces\n", name.c_str());
		return;
	}
	g_camTarget = c;
	g_camDist = r * 2.5f < 6.0f ? 6.0f : r * 2.5f;
	orbitFaceTargetFromHere();
	g_editorCamSeeded = true;   // don't let the first-frame seed override the focus
	std::fprintf(stderr, "[EDITOR] focus '%s': %ld/%lu faces, centre (%.1f %.1f %.1f) radius %.1f dist %.1f\n",
	             name.c_str(), used, total, c.x, c.y, c.z, r, g_camDist);
	rev::Editor_MarkDirty();
}
// Frame the orbit on a scene-authored light (same index space as
// editorGetLights / the LWS write-back): pivot at its position, distance from
// its range so the pool of light it casts is in view.
void editorFocusLight(int want)
{
	if (!CurScene) return;
	int i = 0;
	for (Omni *O = CurScene->OmniHead; O; O = O->Next) {
		if (!(O->Flags & Omni_SceneAuthored)) continue;
		if (i++ != want) continue;
		g_camTarget = O->IPos;
		const float range = O->Range.NumKeys ? O->Range.Keys[0].Pos.x : 0.0f;
		g_camDist = range > 1.0f ? range * 0.8f : 12.0f;
		if (g_camDist < 6.0f)   g_camDist = 6.0f;
		if (g_camDist > 200.0f) g_camDist = 200.0f;
		orbitFaceTargetFromHere();
		g_editorCamSeeded = true;
		std::fprintf(stderr, "[EDITOR] focus light %d at (%.1f %.1f %.1f) dist %.1f\n",
		             want, O->IPos.x, O->IPos.y, O->IPos.z, g_camDist);
		rev::Editor_MarkDirty();
		return;
	}
	std::fprintf(stderr, "[EDITOR] focus light %d: not found (%d authored)\n", want, i);
}
} // namespace

EMSCRIPTEN_BINDINGS(rev_editor_camera)
{
	emscripten::function("editorOrbit",         &editorOrbit);
	emscripten::function("editorZoom",          &editorZoom);
	emscripten::function("editorPan",           &editorPan);
	emscripten::function("editorFocusSurface",  &editorFocusSurface);
	emscripten::function("editorFocusLight",    &editorFocusLight);
	emscripten::function("editorTimeStep",      &editorTimeStep);
	emscripten::function("editorPlayScene",     &editorPlayScene);
	emscripten::function("editorSceneTime",     &editorSceneTime);
	emscripten::function("editorTimeSet",       &editorTimeSet);
	emscripten::function("editorSceneLength",   &editorSceneLength);
	emscripten::function("editorCamDebug",      &editorCamDebug);
}

#endif // __EMSCRIPTEN__
