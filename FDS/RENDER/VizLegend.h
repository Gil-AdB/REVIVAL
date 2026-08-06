#ifndef FDS_RENDER_VIZ_LEGEND_H_INCLUDED
#define FDS_RENDER_VIZ_LEGEND_H_INCLUDED

// Per-mode LEGEND for the debug vizzes — the few lines that say what the
// colours on screen MEAN.
//
// Why this file exists: the viz cycle (VizCycle.cpp) already NAMES the active
// mode bottom-left, but a name is not a reading. "POM march paths" does not
// tell you that magenta means the cone march ran out of steps with the ray
// still inside the slab and therefore applied NO shift, and "WIRE facing" does
// not tell you that a red patch means back-facing on BOTH sides, i.e. an
// inverted normal. Those readings lived only in FeatureFlags.def help text and
// in comments, which is the wrong place to keep them when you are staring at
// the frame.
//
// DERIVATION RULE: a legend row's colour must come from the SAME code the
// renderer colours pixels with, and a legend row's numbers must come from the
// same constants/measurements the renderer used — never from a re-typed copy.
// So:
//   --pom_path_viz  rows call meka::pom_path_color() on a representative path
//                   code (FDS/FILLERS/Mekalele.h). Re-tune that palette and the
//                   legend follows on the next frame with no edit here.
//   --wire_viz      rows come from WireViz_Legend (FDS/RENDER/WorldAabb.cpp),
//                   beside the overlay that picks those colours, and quote the
//                   live --wire_viz_dim.
//   --displace_viz  rows come from DisplaceViz_Legend, likewise beside the
//                   overlay: they quote rampColor/divergeColor for the swatches
//                   and the bake's MEASURED g_dispMax / g_dispErrMax and the
//                   overlay's own match threshold for the numbers.
//   --pom_seam_viz  rows come from PomSeamViz_Legend, swatches from seamColor().
// Modes whose encoding is a plain scalar ramp or a fixed 3-way get a single
// literal line here, stating the range and what dark vs bright means. Modes
// that are self-evident get nothing — padding a legend teaches the eye to skip
// it.

#include <cstdint>

namespace fds {

// One legend line: zero or more colour chips, then text. Text is owned inline
// (not a pointer) because several rows are formatted from live measurements.
struct VizLegendRow {
	char     text[112];
	int      nsw;        // swatches in use, 0 = text only
	uint32_t sw[8];      // 0x00RRGGBB (alpha ignored)
};

constexpr int kVizLegendMaxRows = 20;

// Build the legend for whatever viz is active right now (reads FeatureFlags
// directly, so it covers CLI-set modes as well as cycled ones). Returns the
// number of rows written, 0 when no active mode has a legend.
// `sigOut` receives a small identifier for (mode, value) so a caller can tell
// "the selection changed" without diffing the text — VizCycle uses it to dump
// the full legend to stderr exactly once per selection.
int VizLegend_Build(VizLegendRow* rows, int maxRows, int* sigOut);

// Providers defined next to the overlays that own the colours/constants.
// Each returns the number of rows written.
int WireViz_Legend(VizLegendRow* rows, int maxRows);
int DisplaceViz_Legend(VizLegendRow* rows, int maxRows);
int PomSeamViz_Legend(VizLegendRow* rows, int maxRows);

}  // namespace fds

#endif  // FDS_RENDER_VIZ_LEGEND_H_INCLUDED
