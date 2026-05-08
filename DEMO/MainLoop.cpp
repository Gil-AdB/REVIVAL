#ifdef __EMSCRIPTEN__

#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <atomic>
#include <thread>

#include <emscripten.h>
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
	DONE,
};
static DemoState g_state = WAIT_GESTURE;
static std::unique_ptr<SceneDriver> g_currentDriver;
static bool g_currentDriverInitialized = false;

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
		default:
			break;
		}
	}
	return quit;
}

void DemoBoot(ModplayerHandle modHandle)
{
	g_modHandle = modHandle;

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

bool DemoTick()
{
	if (pumpEvents()) {
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

	case RUN_GLATO: {
		if (!g_currentDriver) {
			if (!g_glatoReady.load()) break; // init thread still loading
			startScene(createGlatoScene());
		}
		if (!tickCurrentScene()) {
			cleanupCurrentScene();
			advanceFromState(RUN_CITY, nullptr);
		}
		break;
	}

	case RUN_CITY: {
		if (!g_currentDriver) {
			if (!g_cityReady.load()) break;
			startScene(createCityScene());
		}
		if (!tickCurrentScene()) {
			cleanupCurrentScene();
			advanceFromState(RUN_CHASE, nullptr);
		}
		break;
	}

	case RUN_CHASE: {
		if (!g_currentDriver) {
			if (!g_chaseReady.load()) break;
			startScene(createChaseScene());
		}
		if (!tickCurrentScene()) {
			cleanupCurrentScene();
			advanceFromState(RUN_FOUNTAIN, nullptr);
		}
		break;
	}

	case RUN_FOUNTAIN: {
		if (!g_currentDriver) {
			if (!g_fountainReady.load()) break;
			startScene(createFountainScene());
		}
		if (!tickCurrentScene()) {
			cleanupCurrentScene();
			advanceFromState(RUN_CRASH, nullptr);
		}
		break;
	}

	case RUN_CRASH: {
		if (!g_currentDriver) {
			if (!g_crashReady.load()) break;
			startScene(createCrashScene());
		}
		if (!tickCurrentScene()) {
			cleanupCurrentScene();
			advanceFromState(RUN_GREETS, nullptr);
		}
		break;
	}

	case RUN_GREETS: {
		if (!g_currentDriver) {
			if (!g_greetsReady.load()) break;
			startScene(createGreetsScene());
		}
		if (!tickCurrentScene()) {
			cleanupCurrentScene();
			g_state = DONE;
		}
		break;
	}

	case DONE: {
		cleanupCurrentScene();
		if (g_modHandle) {
			SDL2_StopMusic();
			Modplayer_Stop(g_modHandle);
			g_modHandle = nullptr;
		}
		if (g_initThread.joinable()) g_initThread.join();
		ThreadPool::instance().close();
		return false;
	}
	}
	return true;
}

#endif // __EMSCRIPTEN__
