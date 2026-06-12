#ifndef REVIVAL_TUNESERVER_H
#define REVIVAL_TUNESERVER_H

namespace fds {

// Live tuning console — a tiny embedded HTTP server (single detached
// thread, 127.0.0.1 only) serving a knob page over the FeatureFlags
// registry. Open http://localhost:<tune_port> while the demo runs:
// every flag appears grouped by category with sliders/checkboxes;
// changes apply immediately and are marked SET (CLI precedence — the
// per-scene param scripts yield to them); a release button hands a
// knob back to the script/default; copy buttons export the changed
// set as CLI flags or SCRIPTS/*.params lines.
//
// Gated by --tune_server (default on) / --tune_port. No-op on wasm.
void TuneServer_Start();

} // namespace fds

#endif // REVIVAL_TUNESERVER_H
