#ifndef REVIVAL_PARAMSCRIPT_H
#define REVIVAL_PARAMSCRIPT_H

namespace fds {

// Per-scene scripted parameters — see docs/PARAM_SCRIPTS.md.
//
// A scene's script lives at SCRIPTS/<scene>.params (relative to the demo
// CWD, i.e. Runtime/). Each line sets a FeatureFlags parameter by name,
// either constant for the scene or keyed over the scene timeline:
//
//   fast_fog = 1                  # constant override at scene start
//   fast_fog_density @ 800 = 8    # time key (Timer units); floats/ints
//                                 # lerp between keys, bools step
//
// Semantics:
//   • CLI/env-set flags WIN over the script (so A/B workflows keep
//     working); shadowed lines are reported once at load.
//   • Values touched by a script are RESTORED when the scene changes or
//     the script is edited/removed — scripts can't leak between scenes.
//   • The file is hot-reloaded when its mtime changes (poll, ~4×/s).
//   • Flags read once at init (grid allocations etc.) won't respond
//     mid-scene; most fog/render params are read per frame and do.
//
// ParamScript_SetScene is idempotent per name — scenes call it every tick
// (alongside tickTabToggle) and it only does work on a name change.
void ParamScript_SetScene(const char *sceneName);

// Evaluate the active script at `timer` and write the values into the
// FeatureFlags state. Call once per frame, demo thread, before rendering.
void ParamScript_Tick(float timer);

} // namespace fds

#endif // REVIVAL_PARAMSCRIPT_H
