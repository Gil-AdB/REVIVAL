#ifndef FDS_PROVENANCE_H_INCLUDED
#define FDS_PROVENANCE_H_INCLUDED

// Snapshot provenance sidecars.
//
// THE FAILURE THIS FIXES: renders under docs/img/ carried nothing about which
// binary, flags, camera and scene time produced them. Names like mbase / mdef /
// nsdef are unreadable a day later and the coordinator has repeatedly had to
// ask "which arm was this?" — a question no one can answer from the pixels.
//
// Every PPM the engine writes now gets a `<same-stem>.json` beside it holding
// the full argv, the git HEAD the binary was built from (+dirty), the binary's
// mtime, the scene, the scene time, the camera pose at %.9g (the same six-float
// FDS_GREETS_CAM form the interactive G-key prints, so it pastes straight back
// into a repro), the resolution, every FeatureFlag whose RESOLVED value differs
// from its compile-time default, and every FDS_/FNTSNAP_/CITYSNAP_ style env
// var that was set.
//
// The PPM bytes are untouched: the sidecar is a separate file. That is the gate
// this module is held to.
//
// tools/ppm2png.py folds the sidecar into the PNG as a `groundwork-provenance`
// tEXt chunk; tools/png_provenance.py prints it back out.

#include <string>

namespace fds {
namespace Provenance {

// Called once from main() before anything parses flags. Stores argv so every
// later sidecar can quote the exact invocation.
void SetArgv(int argc, const char *const *argv);

// Scene label for subsequent sidecars ("greets", "city", "xpartest", ...).
// Set once by ParseSnapshotArgs / the interactive dump path; the scene TIME is
// NOT set here — it is read live from the engine's Timer at write time, so a
// driver that ticks a sweep needs no per-timestamp bookkeeping.
void SetScene(const char *scene);

// Overrides the scene time that would otherwise be read from the Timer global.
// Pass a negative value to go back to reading Timer. Only needed where the
// image is not keyed by Timer at all (pose-indexed harnesses).
void SetTimeOverride(int t);

// Extra free-form key the harness can attach ("pose", "mode", ...). Cleared by
// ClearTags(). Values are JSON-escaped.
void SetTag(const char *key, const char *value);
void ClearTags();

// Writes `<stem-of-imagePath>.json` beside imagePath. Silent no-op-with-warning
// on failure — a provenance write must never take a render down.
// `format` is "ppm" / "pgm" / raw plane name, purely descriptive.
void WriteSidecar(const char *imagePath, int width, int height,
                  const char *format = "ppm");

// The sidecar body without writing it (used by the self-test and by callers
// that want to log it).
void BuildJson(std::string &out, const char *imagePath, int width, int height,
               const char *format);

} // namespace Provenance
} // namespace fds

#endif // FDS_PROVENANCE_H_INCLUDED
