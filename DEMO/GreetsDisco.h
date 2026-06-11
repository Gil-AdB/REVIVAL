#ifndef DEMO_GREETS_DISCO_H
#define DEMO_GREETS_DISCO_H

// Mirrored disco ball for the greets corridor. 🪩
//
// A faceted env-mapped sphere hung from the corridor ceiling, slowly
// spinning, plus a ring of narrow spotlight cones rotating with it
// that paint moving light dots over the walls and floor.
//
// Geometry follows the engine's hand-built-mesh survival rules (see
// memory: engine-face-mesh-conventions): every Vertex and Face is a
// prototype COPY from a real loader mesh (the robot's chrome,
// Face_Reflective env faces) with positions/normals/UVs overwritten —
// from-scratch structs never rasterize. The facets stay flat (4
// private verts per quad) so each one samples a single patch of the
// equirectangular panorama and the ball sparkles as it turns.
//
// Call order: BuildDiscoBall BEFORE the greets mirror build (so the
// teleporter mirror clones the ball too), UpdateDiscoBall every tick
// AFTER Animate_Objects and BEFORE UpdateAllMirrors (it stamps the
// possessed mesh's RotMat that both Transform and the mirror
// re-mirror read).

struct Scene;

namespace fds {

// Returns true if the ball was built (flag on + a donor env face was
// found). Safe to call when --greets-disco is off: does nothing.
bool BuildDiscoBall(Scene *sc);

// timerTicks: the greets music timer (~100 ticks/s) — keeps the spin
// deterministic and scrub-safe.
void UpdateDiscoBall(Scene *sc, float timerTicks);

}  // namespace fds

#endif  // DEMO_GREETS_DISCO_H
