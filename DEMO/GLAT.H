#pragma once

#include <cstdint>

// Per-tick state captured by the Glat snapshot harness. CamX/Y/Z and Rx/Ry/Rz
// are read from GlatoScene at the end of tick(); ST is the dominant
// animation parameter (a piecewise function of Timer that drives almost
// every visual in Glat) and is captured at the moment it's computed.
struct GlatoTraceSample {
    int32_t timer;
    float st;
    float rx;
    float ry;
    float rz;
    float camX;
    float camY;
    float camZ;
};

// If non-null, GlatoScene::tick() invokes this once per tick with the
// current state. Snapshot mode wires it up to record a CSV trace; the
// production demo path leaves it null.
using GlatoTraceHook = void (*)(const GlatoTraceSample&);
extern GlatoTraceHook g_glatoTraceHook;
