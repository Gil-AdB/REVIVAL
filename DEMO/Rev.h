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

// Per-scene "skip to next" flag. Set by Backspace keydown. Cleared by
// runSceneBlocking() once it observes the flag and breaks out of the tick
// loop, so the next scene starts clean. Distinct from g_shouldQuit so that
// "skip" doesn't accidentally exit the program.
extern std::atomic<bool> g_skipScene;

enum
{
	PROF_ZCLR	=	0,
	PROF_SKY	=	1,  // RenderSkyCube — was inflating ZCLR in city/fountain
	PROF_ANIM	=	2,
	PROF_XFRM	=	3,
	PROF_LGHT   =	4,
	PROF_SORT	=	5,
	PROF_RNDR	=	6,
	PROF_FLIP	=	7,
	PROF_NUM	=	8
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

void Initialize_Crash();
void Run_Crash();

#endif