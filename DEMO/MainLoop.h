#pragma once

#include <memory>

#include "../Modplayer/Modplayer.h"

// Wasm-only: rAF-driven state machine that replaces the blocking
// CodeEntry pthread on emscripten. main() runs on browser-main, kicks
// off background init via DemoBoot, registers DemoTick as the
// emscripten_set_main_loop callback, and returns. Scenes already use
// the SceneDriver pattern; we just sequence them without blocking.
//
// Native is unchanged: main() still spawns a CodeEntry thread that
// runs the legacy blocking sequence.

#ifdef __EMSCRIPTEN__

// Spawns the background init thread (Initialize_Glato + the rest in
// sequence) and starts modplayer playback (audio device + Modplayer_Start).
// Caller passes a created modplayer handle; ownership stays with caller.
// Posts the click-or-press hint overlay and arms the user-gesture wait.
void DemoBoot(ModplayerHandle modHandle);

// One frame's worth of state-machine work. Pumps SDL events + applies
// pending resize, then advances the scene driver (or waits for the
// next-scene init flag to flip). Returns true to keep ticking, false
// when DONE — caller should then call emscripten_cancel_main_loop.
bool DemoTick();

#endif
