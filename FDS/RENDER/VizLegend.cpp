#include "VizLegend.h"
#include "WorldAabb.h"          // (the wire/displace/seam providers live there)

#include <Base/FeatureFlags.h>
#include <FILLERS/Mekalele.h>   // meka::pom_path_color + the kPom* path codes

#include <cstdarg>
#include <cstdio>

namespace fds {

namespace {

using FF = FeatureFlags;

int addRow(VizLegendRow* rows, int n, int maxRows,
           const uint32_t* sw, int nsw, const char* fmt, ...) {
	if (n >= maxRows) return n;
	VizLegendRow& r = rows[n];
	if (nsw < 0) nsw = 0; else if (nsw > 8) nsw = 8;
	r.nsw = nsw;
	for (int i = 0; i < nsw; ++i) r.sw[i] = sw[i] & 0x00FFFFFFu;
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(r.text, sizeof r.text, fmt, ap);
	va_end(ap);
	return n + 1;
}

// One swatch, colour taken from the renderer's own palette function.
int pathRow(VizLegendRow* rows, int n, int maxRows, uint32_t code, const char* text) {
	const uint32_t c = meka::pom_path_color(code);
	return addRow(rows, n, maxRows, &c, 1, "%s", text);
}

// ── --pom_path_viz=1 ──────────────────────────────────────────────────────
// The mode that needs a legend most: 13 discrete classes, no ordering, and
// several of them mean "the march gave up" in ways that look like a normal
// result unless you know the colour. Every swatch here is
// meka::pom_path_color() applied to a representative code, so re-tuning that
// palette in Mekalele.h updates this legend on the next frame.
int pomPathLegend(VizLegendRow* rows, int maxRows) {
	using namespace meka;
	int n = 0;
	n = addRow(rows, n, maxRows, nullptr, 0,
	    "colour = (march kind, terminal action); an ACTION overrides the kind");
	// Terminal actions (bits [7:4]) — these win in pom_path_color.
	n = pathRow(rows, n, maxRows, kPomActClampFlat << 4,
	    "clamped to the FLAT surface (recess edge / lid edge)");
	n = pathRow(rows, n, maxRows, kPomActClampBox << 4,
	    "UV clamped back INTO the patch box");
	n = pathRow(rows, n, maxRows, kPomActSideLand << 4,
	    "landed on a leaning SIDE face");
	n = pathRow(rows, n, maxRows, kPomActDiscard << 4,
	    "DISCARDED - lane killed, so something BEHIND wins this pixel");
	// March kinds (bits [3:0]), action = keep.
	n = pathRow(rows, n, maxRows, kPomPathSingle,
	    "single shift - no march configured here");
	n = pathRow(rows, n, maxRows, kPomPathRowFar,
	    "LOD row-far: the march was skipped for this whole row");
	n = pathRow(rows, n, maxRows, kPomPathNaiveHit,  "naive march HIT");
	n = pathRow(rows, n, maxRows, kPomPathNaiveNo,
	    "naive march ran the slab and passed UNDER the stone");
	n = pathRow(rows, n, maxRows, kPomPathConeHit,   "cone march HIT");
	n = pathRow(rows, n, maxRows, kPomPathConeUnres,
	    "cone UNRESOLVED: out of steps, ray still in the slab -> NO SHIFT applied");
	n = pathRow(rows, n, maxRows, kPomPathConeMiss,
	    "cone MISSED - walked out of the slab bottom");
	{	// Reference march: two chips, one row.
		const uint32_t both[2] = { pom_path_color(kPomPathRefHit),
		                           pom_path_color(kPomPathRefMiss) };
		n = addRow(rows, n, maxRows, both, 2, "reference march hit / miss");
	}
	return n;
}

}  // namespace

int VizLegend_Build(VizLegendRow* rows, int maxRows, int* sigOut) {
	if (sigOut) *sigOut = 0;
	if (!rows || maxRows <= 0) return 0;

	// Priority order. Several vizzes can be on at once from the command line;
	// the cycle only ever selects one, and a stack of legends would bury the
	// frame, so the most specific mode wins and the rest stay in --help.
	// `sig` is (slot << 8) | value: enough for "the selection changed".
	int n = 0, sig = 0;

	if (FF::pom_path_viz() == 1) {
		// Mode 2 deliberately has NO legend: it records the path plane while
		// leaving the image alone, precisely so a sweep can be diffed for path
		// flips. Painting text into those frames would corrupt the instrument.
		n = pomPathLegend(rows, maxRows);
		sig = (1 << 8) | 1;
	} else if (FF::poly_viz()) {
		n = addRow(rows, n, maxRows, nullptr, 0,
		    "HUE = material id (12-hue palette): a foreign hue mid-surface is ANOTHER");
		n = addRow(rows, n, maxRows, nullptr, 0,
		    "surface's triangle covering this area, not this surface shading wrong");
		n = addRow(rows, n, maxRows, nullptr, 0,
		    "BRIGHT = a --pom_shell LID face, 45%% = not; +-20%% jitter per TRIANGLE");
		sig = (2 << 8) | 1;
	} else if (FF::pom_viz()) {
		n = addRow(rows, n, maxRows, nullptr, 0,
		    "grey = the HEIGHT FIELD at each pixel's final post-march UV:");
		n = addRow(rows, n, maxRows, nullptr, 0,
		    "black = valley, white = peak. Lighting still applies.");
		n = addRow(rows, n, maxRows, nullptr, 0,
		    "TERRACES in the grey are march step-banding, not relief.");
		sig = (3 << 8) | 1;
	} else if (FF::pom_mip_viz()) {
		// Palette source: the kMipPal literal in Mekalele.h's inner loop. Named
		// in prose rather than chipped because that array is function-local to
		// the raster kernel, and hoisting it to reach it from here would edit
		// the hot filler TU for a legend. Order below IS that array's order.
		n = addRow(rows, n, maxRows, nullptr, 0,
		    "per-FACE miplevel: 0 red, 1 green, 2 blue, 3 yellow,");
		n = addRow(rows, n, maxRows, nullptr, 0,
		    "4 magenta, 5 cyan, 6 orange, 7 grey");
		n = addRow(rows, n, maxRows, nullptr, 0,
		    "the colour boundaries ARE the mipmap-via-subdivision sub-face seams");
		sig = (4 << 8) | 1;
	} else if (FF::wire_viz() > 0) {
		n = WireViz_Legend(rows, maxRows);
		sig = (5 << 8) | FF::wire_viz();
	} else if (FF::displace_viz() > 0) {
		n = DisplaceViz_Legend(rows, maxRows);
		sig = (6 << 8) | FF::displace_viz();
	} else if (FF::pom_seam_viz() > 0) {
		n = PomSeamViz_Legend(rows, maxRows);
		sig = (7 << 8) | FF::pom_seam_viz();
	} else if (FF::nmap_viz() > 0) {
		switch (FF::nmap_viz()) {
		case 1:
			n = addRow(rows, n, maxRows, nullptr, 0,
			    "RGB = the G-buffer GEOMETRIC normal, n*0.5+0.5 (per-face, no map)");
			break;
		case 2:
			n = addRow(rows, n, maxRows, nullptr, 0,
			    "RGB = the FINAL shaded normal through the TBN - what lighting uses");
			n = addRow(rows, n, maxRows, nullptr, 0,
			    "relief that reads INVERTED here is a flipped-G normal map");
			break;
		default:
			n = addRow(rows, n, maxRows, nullptr, 0,
			    "RGB = the RAW normal-map texel at the sampled UV");
			n = addRow(rows, n, maxRows, nullptr, 0,
			    "dim flat grey = this material has no normal map");
			break;
		}
		sig = (8 << 8) | FF::nmap_viz();
	} else if (FF::shadow_lightmap_viz() > 0) {
		switch (FF::shadow_lightmap_viz()) {
		case 1:
			n = addRow(rows, n, maxRows, nullptr, 0,
			    "grey = lightmap meshLMId, dark to bright = low to high id; RED = id 0");
			break;
		case 2:
			n = addRow(rows, n, maxRows, nullptr, 0,
			    "colour = hash of the lightmap faceIdx owning the pixel");
			n = addRow(rows, n, maxRows, nullptr, 0,
			    "one flat colour over a patch = one lightmap face owns all of it");
			break;
		case 9: {
			// The four bands the viz itself paints (LightmapBake.cpp), in the
			// same order and with the same thresholds as the flag help.
			const uint32_t band[4] = { 0x0030E030u, 0x00E0E030u,
			                           0x00E09030u, 0x00E03030u };
			n = addRow(rows, n, maxRows, band, 4,
			    "|bary world - depth world|: <=0.1 / <=0.5 / <=1.0 / >1.0 world units");
			n = addRow(rows, n, maxRows, nullptr, 0,
			    "green is correct: bary points at the pixel's real 3D position");
			break;
		}
		default:
			n = addRow(rows, n, maxRows, nullptr, 0,
			    "see --help shadow_lightmap_viz for this mode's encoding");
			break;
		}
		sig = (9 << 8) | FF::shadow_lightmap_viz();
	} else if (FF::uv_viz() > 0) {
		switch (FF::uv_viz()) {
		case 2:
			{
				static const uint32_t hues[8] = { 0xE63C3Cu, 0xE69628u, 0xDCDC32u, 0x46C846u,
				                                  0x3CC8C8u, 0x3C64E6u, 0xA046DCu, 0xE650B4u };
				n = addRow(rows, n, maxRows, hues, 8,
				    "8 u-CELLS per tile of the surface's own diffuse UV, one HUE each (texel-exact)");
			}
			n = addRow(rows, n, maxRows, nullptr, 0,
			    "light/dark = alternate v rows; a HUE change along a junction = a u step");
			n = addRow(rows, n, maxRows, nullptr, 0,
			    "(a light/dark-only checker was blind to even-cell steps: H5981 mitre line 9)");
			break;
		case 3:
			n = addRow(rows, n, maxRows, nullptr, 0,
			    "grey saw = the U COLUMN: 16 stripes per tile, each restart = one stripe");
			break;
		case 4:
			n = addRow(rows, n, maxRows, nullptr, 0,
			    "grey saw = the V ROW: 16 stripes per tile, each restart = one stripe");
			break;
		default: {
			const uint32_t chips[2] = { 0x00FF0000u, 0x0000FF00u };
			n = addRow(rows, n, maxRows, chips, 2,
			    "RED = u, GREEN = v of the surface's diffuse UV here, wrapped to one tile");
			n = addRow(rows, n, maxRows, nullptr, 0,
			    "a wrap is a tile seam; a colour STEP along a junction = different UV per sheet");
			break; }
		}
		n = addRow(rows, n, maxRows, nullptr, 0,
		    "dark grey = nothing rasterised; mid grey = no diffuse map / forward-rendered");
		sig = (15 << 8) | FF::uv_viz();
	} else if (FF::pom_horizon_viz()) {
		n = addRow(rows, n, maxRows, nullptr, 0,
		    "grey = the horizon TERM itself, albedo and ambient removed:");
		n = addRow(rows, n, maxRows, nullptr, 0,
		    "white 1 = unoccluded, black 0 = below the horizon in every light's azimuth");
		sig = (10 << 8) | 1;
	} else if (FF::aa_viz()) {
		const uint32_t green = 0x0030FF30u;
		n = addRow(rows, n, maxRows, &green, 1,
		    "the pixels --aa blends: detected silhouettes + creases, over a dimmed frame");
		sig = (11 << 8) | 1;
	} else if (FF::ssao_debug()) {
		n = addRow(rows, n, maxRows, nullptr, 0,
		    "grey = the SSAO term: white 1 = unoccluded, black 0 = fully occluded");
		if (FF::hdr())
			n = addRow(rows, n, maxRows, nullptr, 0,
			    "WARNING --hdr tonemaps over this; relaunch with --no-hdr");
		sig = (12 << 8) | 1;
	} else if (FF::env_map_viz() > 0) {
		// The ENV-MAP INSPECTOR. The header block the painter draws under the
		// image already names the probe, its bake point, its res and its mip
		// chain (that is data, and it belongs next to the picture); what the
		// legend owes is the CONVENTIONS the picture is drawn in — the face
		// cell order, what the flat grey areas are, and the keys.
		const uint32_t voidGrey = 0x00202020u;
		switch (FF::env_map_viz()) {
		case 2:
			n = addRow(rows, n, maxRows, nullptr, 0,
			    "MIP CHAIN, level 0 leftmost, each level HALF the previous - the");
			n = addRow(rows, n, maxRows, nullptr, 0,
			    "size step IS the lobe the roughness select walks down (--env_mip_chain)");
			break;
		case 3:
			n = addRow(rows, n, maxRows, nullptr, 0,
			    "LEFT green frame = this build's probe; RIGHT orange = GpuBench's");
			n = addRow(rows, n, maxRows, nullptr, 0,
			    "gpuenv_*.ppm for the same material (sqrt-encoded linear: compare");
			n = addRow(rows, n, maxRows, nullptr, 0,
			    "STRUCTURE and which face holds what, not absolute brightness)");
			break;
		default:
			n = addRow(rows, n, maxRows, nullptr, 0,
			    "the probe's OWN texels, mip 0. Cell order is FDS_ENVBAKE_DUMP's:");
			n = addRow(rows, n, maxRows, nullptr, 0,
			    "top row +X -X +Y, bottom row -Y +Z -Z - each cell is labelled");
			break;
		}
		n = addRow(rows, n, maxRows, &voidGrey, 1,
		    "the bake's VOID colour: a direction with NO geometry, not dark geometry");
		n = addRow(rows, n, maxRows, nullptr, 0,
		    "F / Shift+F = next / previous probe. A magenta empty panel = NO DATA.");
		sig = (14 << 8) | FF::env_map_viz();
	} else if (FF::env_refl_viz() > 0) {
		n = addRow(rows, n, maxRows, nullptr, 0,
		    "top-right inset = baked env panorama #%d (1-based; past the count clamps)",
		    FF::env_refl_viz());
		sig = (13 << 8) | FF::env_refl_viz();
	}

	if (sigOut) *sigOut = n > 0 ? sig : 0;
	return n;
}

}  // namespace fds
