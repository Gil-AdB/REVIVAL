#ifndef DEMO_H_INC
#define DEMO_H_INC

#include <atomic>

#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Raytracer.h"
#include <Base/Scene.h>
#include "../Modplayer/Modplayer.h"

// Global "stop the whole demo" flag. Set by:
//   - SDL_QUIT (window close button, OS shutdown, SDL's default SIGINT handler)
//   - ESC keydown (user wants to exit immediately)
//   - main thread before joining the demo worker on shutdown
// Polled by:
//   - runSceneBlocking() between ticks (SceneTick.h)
//   - the inter-scene busy-wait spots in CodeEntry()
// Once set, the flag stays set; scene tick()s will return false on the next
// frame and runSceneBlocking will tear down. Atomic so the SDL main thread can
// set it while a worker thread reads it.
extern std::atomic<bool> g_shouldQuit;

// --init_timeline (default OFF, byte-null, stderr only): stamp one startup
// milestone. Prints "[INIT-T] +<abs ms> (dt <ms>) [t<n>] <label>", where <abs>
// is measured from the FIRST mark of the run and <dt> from the previous mark on
// ANY thread — the init chain runs on the t1 worker while Run_Glato plays on the
// demo thread, so the marks legitimately interleave and only the absolute column
// is ordered. Exists because "the tessellation bake hangs the intro" is a
// DURATION question, not a threading one (Initialize_Greets already runs
// concurrently with the intro; it only stalls the demo if it outlasts it), and
// the two cannot be told apart without per-phase timestamps.
void InitTimelineMark(const char *label);

// Per-scene "skip to next" flag. Set by Backspace keydown. Cleared by
// runSceneBlocking() once it observes the flag and breaks out of the tick
// loop, so the next scene starts clean. Distinct from g_shouldQuit so that
// "skip" doesn't accidentally exit the program.
extern std::atomic<bool> g_skipScene;

// SHIFT held, as of the last key event. Written by both SDL event pumps
// (REV.CPP native, MainLoop.cpp wasm) from SDL_Keysym::mod; SDL updates its
// modifier state BEFORE it fills keysym.mod, so the shift key's own KEYDOWN
// already reads as held and its own KEYUP already reads as released.
// Read by SceneDriver::tickSceneTimer as the FAST arm of the F1/F2 scene-clock
// scrub (step x --scrub_speed while held).
// Deliberately NOT a Keyboard[] slot: ScLShift/ScRShift are defined in
// FDS_DEFS.H but have no consumers, and Keypressed() (FDS/ISR/ISR.CPP) scans
// the WHOLE array — routing shift through Keyboard[] would make "any key held"
// true whenever shift is down, which the legacy P-key timefreeze in
// RENDER.CPP busy-waits on.
extern std::atomic<bool> g_shiftHeld;

// Runtime mip-level debug knobs (toggled via N / Shift+N in REV.CPP).
//   g_forceMipLevel: -1 = auto (rasterizer-chosen), 0..7 = override.
//     When >= 0, every per-pixel kernel uses this value instead of the
//     miplevel bits from gb.mat32. Lets you A/B-compare a forced mip
//     against the auto path to see whether the rasterizer is actually
//     reaching high mips at distance.
//   g_vizMipLevel: when true, the deferred kernel paints each pixel
//     with a palette-indexed color by miplevel (mip0=red, mip1=orange,
//     ..., mip7=white). Texturing + lighting suppressed.
extern std::atomic<int>  g_forceMipLevel;
extern std::atomic<bool> g_vizMipLevel;

enum
{
	PROF_ZCLR	=	0,
	PROF_SKY	=	1,  // RenderSkyCube — was inflating ZCLR in city/fountain
	PROF_ANIM	=	2,
	PROF_XFRM	=	3,
	PROF_LGHT   =	4,
	PROF_SORT	=	5,
	PROF_BAKE	=	6,  // per-frame dynamic shadow bake — was folded into RNDR,
	                    // hiding ~5ms of bake behind the raster/lighting row
	PROF_RNDR	=	7,
	PROF_FLIP	=	8,
	PROF_NUM	=	9
};

extern ModplayerHandle g_RevModuleHandle;
extern dword g_profilerActive;

void Destroy_Scene(Scene *Sc);

void Run_Glato(void);

void Initialize_Credits();
void Run_Credits();

void Initialize_Glato();
void Initialize_City();
void Run_City();

void Initialize_Chase();
void Run_Chase();

void Initialize_Fountain();
void Run_Fountain();

void Initialize_Greets();
void Run_Greets();
// Block until the background lightmap-bake thread spawned by
// Initialize_Greets has finished writing TriMesh::staticShadowLM. MUST
// be called before the first tick of the greets scene: the bake reads
// per-frame geometry + shadow-map state, so ticking (animate + shadow
// bake + raster) while it runs is a data race that yields a garbage
// lightmap (TSan, 2026-06-12). Run_Greets calls it; any other driver
// of the greets scene (bench/snapshot harness) must too. No-op +
// timing log; safe when no bake was ever spawned.
void Greets_JoinBakeThread();
// Quit-time cleanup: if the user exits before Run_Greets has joined the
// background lightmap-bake thread, the std::thread destructor would call
// std::terminate. Call this at every demo_exit path to join whichever
// state the thread is in. Safe to call when no bake was ever spawned.
void Greets_ShutdownBakeThread();

void Initialize_Crash();
void Initialize_PBRTest();
void Run_Crash();

#endif