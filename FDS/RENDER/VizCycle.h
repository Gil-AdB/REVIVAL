#ifndef FDS_RENDER_VIZ_CYCLE_H_INCLUDED
#define FDS_RENDER_VIZ_CYCLE_H_INCLUDED

// Runtime debug-viz cycle — one key steps through every debug visualisation
// that can actually show something in THIS run.
//
// Why: every viz in FeatureFlags.def used to be a startup flag, so comparing
// two of them meant quitting, retyping a command line and flying back to the
// pose. That cost is why the vizzes were under-used. The cycle removes it:
//   X        step FORWARD  through { off, viz 1, viz 2, ... }
//   Shift+X  step BACKWARD
// bound in the SDL event pump (DEMO/REV.CPP native, DEMO/MainLoop.cpp wasm).
//
// Honesty rule: a mode is only offered when its data/storage exists in this
// run. Several vizzes need something committed BEFORE the frame the flag is
// read on — the filtered-albedo G-buffer plane, the displacement bake's
// records, a horizon/lightmap/env bake — and flipping such a flag live would
// be a silent no-op. VizCycle_Step drops those entries from the cycle and
// prints, once, what startup flag would bring each of them back (--viz_arm
// covers the plane + the displacement magnitudes).
//
// Selecting an entry goes through FeatureFlags::setParamFromText, the same
// path the tune console/web UI uses, so a cycled value behaves exactly like a
// CLI one. Values are written from the SDL event thread and read by the render
// thread: an aligned int/bool store, same latitude every other Keyboard-driven
// toggle in this tree already takes (see the note in REV.CPP's TimerProc).

struct Scene;

namespace fds {

// Name of the mode the CYCLE selected, or nullptr when the cycle is at rest.
// Only cycle selections set it — a viz enabled purely from the CLI keeps its
// screenshots byte-identical to before this feature existed.
extern const char* g_vizLabel;

// Step the cycle. dir > 0 = forward, dir < 0 = backward. Builds the active
// entry list (availability-filtered) on first use and prints the menu.
void VizCycle_Step(int dir);

// Per-frame overlay slot, called from the tail of RenderPipeline::renderFrame
// (post-tonemap, main view only): draws the --wire_viz wireframe and the
// cycle's mode-name label. Returns immediately when neither is active.
void VizCycle_Overlay(Scene* sc);

}  // namespace fds

#endif  // FDS_RENDER_VIZ_CYCLE_H_INCLUDED
