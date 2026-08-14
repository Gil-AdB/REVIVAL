#pragma once

// ── LIVE DISPLACEMENT REBUILD (editor) ──────────────────────────────────────
//
// The per-pixel displacement flags split into two populations, and until this
// existed only one of them worked from the editor:
//
//   LIVE       read per FACE / per PIXEL at raster time (Mekalele.h,
//              DeferredSurfaceKernel.cpp) → a knob edit shows on the next frame.
//   INIT-TIME  consumed ONCE inside Initialize_Greets — either because they
//              MOVE VERTICES (--pom_shell via PomShell_Build) or because they
//              select an offline BAKE whose product the kernel then reads
//              (--parallax_pom_cone / --pom_cone_exact → Material::ConeMap,
//              --pom_horizon → Material::PomHorizon).
//
// Flipping an INIT-TIME flag through the live param path either does nothing
// (the shell march is gated on Material::PomShellUvAmp, which only
// PomShell_Build stamps) or HALF-applies (e.g. --pom_cone_exact also picks the
// cone-byte DECODE scale, so toggling it live decodes the old bake with the new
// scale). This module is the missing half: restore the stone to the geometry
// the scene loaded with, then re-run the same bake path Initialize_Greets runs.
//
// SCOPE: the shell family only — OFF / POM / SHELL-recess / SHELL-lid. The
// tessellation bake (--greets_displace) subdivides the mesh IN PLACE before the
// Piramid chunk split, so undoing it means rebuilding topology, the shadow
// clustering, the chunks and the mirror clones; it stays a launch-time mode.
// When the scene was initialised with it on, the capture records that and the
// rebuild REFUSES (see DisplaceRebuild_State().tessellated) rather than
// producing a mesh that is neither one thing nor the other.
//
// Ownership: capture runs at the very end of Initialize_Greets, on the FINAL
// rendered meshes (post chunk split, post mirror clone), so a restore reaches
// exactly the vertices the rasterizer reads.

#include <vector>

struct Scene;
namespace fds { struct Mirror; }

// GREETS.CPP — the scene's live teleporter/screen mirrors, or null when
// --greets_mirror is off. The rebuild needs them for two reasons: their clone
// meshes must be EXCLUDED from PomShell_Build (at scene init they do not exist
// yet, so including them changes the patch grouping — measured: 'rooms' 67
// patches -> 113 and the sibling lists CLAMP), and afterwards each clone face's
// Face::PomShellGroup has to be re-stamped from its source face.
std::vector<fds::Mirror> *Greets_MirrorList();

namespace rev {

struct DisplaceRebuildState {
	bool  armed        = false;   // a pristine snapshot exists
	bool  tessellated  = false;   // scene was built with --greets_displace
	bool  shellBuilt   = false;   // a shell is currently stamped on the stone
	int   meshes       = 0;       // meshes covered by the snapshot
	int   verts        = 0;       // vertices covered
	int   faces        = 0;       // target faces covered
	long  bytes        = 0;       // snapshot heap footprint
	double lastMs      = 0.0;     // duration of the last rebuild
	long  lastPeakKb   = 0;       // transient heap high-water of the last rebuild
	int   rebuilds     = 0;       // how many rebuilds have run this session
};

// Snapshot the stone's PRISTINE (pre-shell) geometry. No-op unless
// --pom_rebuild (or --pom_rebuild_test) is on. Call at the end of scene init,
// with the shell NOT yet built — the caller is responsible for that (the editor
// and the self-test both force --pom_shell off across init and re-apply it here
// through DisplaceRebuild_Apply, which is what makes the snapshot pristine by
// construction). Safe to call twice; the second call is ignored.
void DisplaceRebuild_Capture(Scene *sc);

// Restore the snapshot and re-run the init bake path with the CURRENT flags:
//   • vertex Pos + Vertex::ShellH, face N / NormProd / PomShellGroup, mesh
//     bspheres  ← the snapshot (so repeated toggling cannot compound the offset)
//   • Material::PomShell* tables freed and cleared
//   • PomShell_Build (when --pom_shell) for each stone material
//   • cone map re-bake when --parallax_pom_cone / --pom_cone_exact changed
//   • horizon map bake when --pom_horizon is on and none is present
// Returns false (and changes nothing) when not armed or when the scene was
// built tessellated. Thread-affinity: main thread, between frames.
bool DisplaceRebuild_Apply();

DisplaceRebuildState DisplaceRebuild_State();

// JSON for the editor panel: state + the live value of every displacement flag
// the panel drives, each tagged "live" or "rebuild".
const char *DisplaceRebuild_StateJson();

// --pom_rebuild_test=N: run N restore+re-apply cycles right after init. The
// frame that follows must match a fresh launch in the same mode — that is the
// idempotency gate for "restore pristine geometry". Prints a per-cycle report.
void DisplaceRebuild_SelfTest();

// Scene-init hooks (Initialize_Greets calls exactly these two). BeginInit
// defers a requested --pom_shell past init so the snapshot EndInit takes is
// pristine by construction; EndInit captures, re-applies the deferred mode
// through DisplaceRebuild_Apply, and runs the self-test. Both no-op unless
// --pom_rebuild / --pom_rebuild_test is on, so the shipping demo is untouched.
void DisplaceRebuild_BeginInit();
void DisplaceRebuild_EndInit(Scene *sc);

}  // namespace rev
