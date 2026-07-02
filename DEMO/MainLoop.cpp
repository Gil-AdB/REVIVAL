#ifdef __EMSCRIPTEN__

#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Base/FeatureFlags.h>
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
static bool   g_editorCamSeeded = false; // first frame seeds orbit from scene cam

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
	case SDL_SCANCODE_M:         return ScM;
	case SDL_SCANCODE_P:         return ScP;
	case SDL_SCANCODE_R:         return ScR;
	case SDL_SCANCODE_U:         return ScU;
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
			if (g_editorMode) {
				g_camDist *= (event.wheel.y > 0) ? 0.9f : 1.1f;
				if (g_camDist <   4.0f) g_camDist =   4.0f;
				if (g_camDist > 400.0f) g_camDist = 400.0f;
				rev::Editor_MarkDirty();
			}
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
		};
		for (const char *d : def) args.push_back(d);
		// URL query flags applied AFTER the defaults so ?editor&no-bloom&dof_range=8
		// overrides a default (later wins; setParamFromText marks explicitly-set).
		if (const char *qs = emscripten_run_script_string("location.search.replace(/^[?]/,'')")) {
			std::string q = qs, tok;
			for (size_t i = 0; i <= q.size(); ++i) {
				if (i == q.size() || q[i] == '&') {
					if (!tok.empty() && tok != "editor") args.push_back("--" + tok);
					tok.clear();
				} else tok.push_back(q[i]);
			}
		}
		std::vector<const char*> argv;
		for (auto &a : args) argv.push_back(a.c_str());
		fds::FeatureFlags::parseArgs((int)argv.size(), argv.data());
		// Only hard override: the teleporter mirror stays off in wasm (memory).
		fds::FeatureFlags::setParamFromText("greets_mirror", "0");
		fds::FeatureFlags::setParamFromText("mirror_rtt", "0");
		fprintf(stderr, "[EDITOR] greets editor: full native pipeline (%d flags), mirror off (wasm mem)\n",
		        (int)args.size() - 1);
		// Orbit target = greets room-bbox centre (see the [DISCO] room-bbox log).
		g_camTarget.x = 18.0f; g_camTarget.y = 6.0f; g_camTarget.z = -35.0f;
		// NOTE: the greets init thread is NOT spawned here — editorTick spawns
		// it once the shell's live-FLD fetch resolves (see editorSpawnInit),
		// so a freshly saved GREETS.FLD can be installed into MEMFS over the
		// link-time preloaded copy BEFORE Initialize_Greets opens it.
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
		Initialize_Greets();
		g_greetsReady.store(true);
		g_initThreadDone.store(true);
		fprintf(stderr, "[EDITOR] greets init complete\n");
	});
}

static void editorTick()
{
	if (!g_currentDriver) {
		editorMaybeSpawnInit();
		if (!g_greetsReady.load()) return;        // init thread still loading
		Greets_JoinBakeThread();                  // finish the static lightmap bake
		g_currentDriver = createGreetsScene();
		g_currentDriver->init();
		g_currentDriverInitialized = true;
		EngineStartFadeIn(kFadeFrames);
		g_editorRenderFrames = kFadeFrames + 1;   // play the fade-in
		Init_FreeCamera();
		if (CurScene) Calibrate_FreeCamera_ForScene(CurScene->FZP, CurScene->CameraHead);
		fprintf(stderr, "[EDITOR] greets up — fly: WASD/QE + arrows; mouse-drag orbit; wheel zoom; click a surface to focus\n");
	}

	Keyboard[ScESC] = 0;                          // don't let the scene self-exit
	if (anyFreeCamKey()) rev::Editor_MarkDirty();  // keep rendering while flying

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

	Timer = g_editorFreezeTimer;
	if (!g_editorCamSeeded) {
		// Open on the fixed default orbit (room centre). The earlier auto-seed
		// from the scene's spline camera produced a through-the-floor pose
		// (target below the floor) → black; the fixed default frames the room.
		updateEditorCamera();          // build FC from the default orbit
		poll_pending_resize(g_currentDriver.get());
		g_currentDriver->tick();
		g_editorCamSeeded = true;
	} else {
		if (anyFreeCamKey()) {
			dTime = 16.0f;             // fixed step (Timer is frozen → no scene dTime)
			Dynamic_Camera();          // keyboard fly + look (the TAB free-cam)
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
		if (g_modHandle) {
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
// Frame the orbit on the SELECTED surface: world-space bbox of every face using
// that material (world = RotMat·Pos + IPos — the real interpolated transform),
// pivot at its centre, distance sized to fit. This is the "orbit around the
// actual object" fix — the pivot now tracks geometry, not a fixed guess.
void editorFocusSurface(std::string name)
{
	if (!CurScene) return;
	float lox=1e30f, loy=1e30f, loz=1e30f, hix=-1e30f, hiy=-1e30f, hiz=-1e30f;
	long n = 0;
	for (TriMesh *T = CurScene->TriMeshHead; T; T = T->Next) {
		for (DWord i = 0; i < T->FIndex; ++i) {
			Face &F = T->Faces[i];
			// Base-name match: floor's faces reference the "floor::mirUV"
			// handedness clone, so an exact compare finds no faces at all.
			if (!F.Txtr || !F.Txtr->Name || rev::Editor_BaseSurfName(F.Txtr->Name) != name) continue;
			Vertex *vs[3] = { F.A, F.B, F.C };
			for (int k = 0; k < 3; ++k) {
				Vertex *v = vs[k]; if (!v) continue;
				Vector w; MatrixXVector(T->RotMat, &v->Pos, &w); Vector_SelfAdd(&w, &T->IPos);
				if (w.x < lox) lox = w.x; if (w.y < loy) loy = w.y; if (w.z < loz) loz = w.z;
				if (w.x > hix) hix = w.x; if (w.y > hiy) hiy = w.y; if (w.z > hiz) hiz = w.z;
				++n;
			}
		}
	}
	if (n == 0) { std::fprintf(stderr, "[EDITOR] focus '%s': no faces\n", name.c_str()); return; }
	g_camTarget.x = (lox + hix) * 0.5f;
	g_camTarget.y = (loy + hiy) * 0.5f;
	g_camTarget.z = (loz + hiz) * 0.5f;
	const float dx = hix-lox, dy = hiy-loy, dz = hiz-loz;
	const float diag = std::sqrt(dx*dx + dy*dy + dz*dz);
	g_camDist = diag * 1.3f < 6.0f ? 6.0f : diag * 1.3f;
	g_editorCamSeeded = true;   // don't let the first-frame seed override the focus
	std::fprintf(stderr, "[EDITOR] focus '%s': %ld verts, centre (%.1f %.1f %.1f) dist %.1f\n",
	             name.c_str(), n, g_camTarget.x, g_camTarget.y, g_camTarget.z, g_camDist);
	rev::Editor_MarkDirty();
}
} // namespace

EMSCRIPTEN_BINDINGS(rev_editor_camera)
{
	emscripten::function("editorOrbit",         &editorOrbit);
	emscripten::function("editorZoom",          &editorZoom);
	emscripten::function("editorPan",           &editorPan);
	emscripten::function("editorFocusSurface",  &editorFocusSurface);
}

#endif // __EMSCRIPTEN__
