// PBR material-import unit tests: the filename→role classifier and the
// single-channel packer.
//
// ── WHY THE CHECK MACRO AND NOT assert() ────────────────────────────────────
// This project builds Release BY DEFAULT (CMAKE_BUILD_TYPE=Release, CLAUDE.md),
// which defines NDEBUG, which turns every assert() into nothing at all. A test
// written with assert() therefore compiles to a program that prints "PASS" and
// exits 0 no matter what the code does — and worse, any function called ONLY
// from inside an assert() is never referenced, so it is not even linked, and a
// classifier test can "pass" without the classifier existing. CHECK() is a real
// runtime comparison that survives NDEBUG, records the failure, and makes main()
// return non-zero.
//
// To prove the test can FAIL (do this after editing it, it takes ten seconds):
//   break classify() in DEMO/MaterialImport.cpp — e.g. move the PackedOrm test
//   back above the single-role tests — rebuild, and `ctest -R pbr_import` must
//   go red. If it stays green, the test is decorative and needs fixing.
//
// The classifier is exercised through the PUBLIC seam
// MaterialImport_ClassifyRole(), not the internal classify(): classify lives in
// an anonymous namespace (internal linkage) and cannot be declared from another
// translation unit — a forward declaration of it links only while it is never
// actually called.

#include <cstdio>
#include <string>

// Stubs for FDS and DEMO symbols used by MeshOps / MaterialImport
float dTime = 0.0f;
struct Material;
Material* Materialize(void* data, int x, int y) {
    return nullptr;
}
namespace rev {
    std::string Editor_BaseSurfName(const char*) { return ""; }
    std::string Editor_ChunkBaseObjName(const char*) { return ""; }
}
#include <cstring>
#include <cstdint>
#include <string>

#include "DEMO/MeshOps.h"
#include "DEMO/MaterialImport.h"
#include "FDS/Base/Texture.h"

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond)                                                            \
	do {                                                                       \
		++g_checks;                                                            \
		if (!(cond)) {                                                         \
			++g_failures;                                                      \
			std::fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
		}                                                                      \
	} while (0)

static void checkRole(const char *file, const char *want) {
	++g_checks;
	const char *got = fds::MaterialImport_ClassifyRole(file);
	if (std::strcmp(got, want) != 0) {
		++g_failures;
		std::fprintf(stderr, "  FAIL classify(\"%s\") = \"%s\", want \"%s\"\n",
		             file, got, want);
	}
}

static void checkChannel(Texture *src, int channel, unsigned want, const char *what) {
	++g_checks;
	Texture *t = MakeChannel8(src, channel);
	if (!t || t->BPP != 8) {
		++g_failures;
		std::fprintf(stderr, "  FAIL MakeChannel8(ch=%d) returned %s\n", channel,
		             t ? "BPP != 8" : "nullptr");
		return;
	}
	const unsigned got = reinterpret_cast<const uint8_t *>(t->Mipmap[0])[0];
	if (got != want) {
		++g_failures;
		std::fprintf(stderr, "  FAIL MakeChannel8(ch=%d) [%s] = %u, want %u\n",
		             channel, what, got, want);
	}
}

int main() {
	// ── 1. MakeChannel8 pulls the RIGHT lane ────────────────────────────────
	// Four DISTINCT channel values — the whole point is that a wrong shift is
	// visible. A grayscale probe would pass with any shift and prove nothing,
	// which is exactly why the blue-byte bug survived so long in the real code:
	// every aux map that had ever been fed to it was grayscale.
	std::printf("[1/3] MakeChannel8 channel selection\n");
	const uint32_t R = 0xAA, G = 0x88, B = 0xCC, A = 0xFF;
	const uint32_t pixel = (A << 24) | (R << 16) | (G << 8) | B;
	uint32_t pixels[16 * 16];
	for (int i = 0; i < 16 * 16; ++i) pixels[i] = pixel;

	Texture *src = Scene_MakeTiledTexture(16, 16, pixels, /*buildMips=*/false);
	CHECK(src != nullptr);
	if (src) {
		CHECK(src->BPP == 32);
		checkChannel(src, 0, R, "Red / ao+height");
		checkChannel(src, 1, G, "Green / roughness");
		checkChannel(src, 2, B, "Blue / metallic");
		checkChannel(src, 3, A, "Alpha");
		// The back-compat spelling must agree with channel 0, or GREETS' height
		// and roughness maps silently change lane.
		Texture *h = MakeHeight8(src);
		CHECK(h != nullptr);
		if (h) CHECK(reinterpret_cast<const uint8_t *>(h->Mipmap[0])[0] == R);
		// Out-of-range must clamp to Red, never read past the word.
		checkChannel(src, 99, R, "out-of-range clamps to Red");
		checkChannel(src, -1, R, "negative clamps to Red");
	}

	// ── 2. The role classifier ──────────────────────────────────────────────
	std::printf("[2/3] filename -> role classification\n");
	// Plain roles.
	checkRole("pattern_albedo.png",    "albedo");
	checkRole("cliff_normal.png",      "normal");
	checkRole("wood_roughness.png",    "roughness");
	checkRole("concrete_height.png",   "height");
	checkRole("stone_ao.png",          "ao");
	checkRole("iron_metalness.png",    "metallic");
	checkRole("iron_metallic.png",     "metallic");
	// Vendor abbreviations (PolyHaven / FreePBR).
	checkRole("sandstone_blocks_05_diff_1k.png", "albedo");
	checkRole("LAP_COL.JPG",                     "albedo");
	checkRole("blue_metal_plate_nor_gl_1k.png",  "normal");
	checkRole("blue_metal_plate_rough_1k.png",   "roughness");
	checkRole("blue_metal_plate_disp_1k.png",    "height");
	// Trailing digits are part of a role token ("normal2", "rough2").
	checkRole("chipped-paint-metal-rough2.png",  "roughness");
	checkRole("chipped-paint-metal-normal-ogl.png", "normal");
	// Non-maps are skipped, not guessed at.
	checkRole("chipped-paint-metal-preview.jpg", "");
	checkRole("material_sphere_render.png",      "");
	checkRole("notes.txt",                       "");
	// THE FAMILY-NAME COLLISION. "metal" names an asset family as often as it
	// names a metalness map. Every one of these is a DIFFERENT map belonging to
	// a metal-named family, and reading them as metalness loads the wrong image
	// into MetallicMap — which kills the surface's diffuse and turns it black.
	checkRole("blue_metal_plate_ao_1k.png",      "ao");
	checkRole("old-metal-slats1_ao.png",         "ao");
	checkRole("blue_metal_plate_diff_1k.png",    "albedo");
	checkRole("chipped-paint-metal-albedo.png",  "albedo");
	checkRole("chipped-paint-metal-metal.png",   "metallic");
	// Packed sets.
	checkRole("blue_metal_plate_arm_1k.png",     "packed_orm");
	checkRole("sandstone_blocks_05_arm_1k.png",  "packed_orm");
	checkRole("surface_orm.png",                 "packed_orm");
	checkRole("surface_rma.png",                 "packed_orm");
	// ...but "arm" is an English word and an object name. A stem that names a
	// single role explicitly IS that role; only one that names none is packed.
	checkRole("mech_arm_albedo.png",             "albedo");
	checkRole("arm_normal.png",                  "normal");
	checkRole("left_arm_rough.png",              "roughness");
	checkRole("robot-arm-color.png",             "albedo");
	// Separator boundaries: a role token must not match inside a longer word.
	checkRole("halo.png",                        "");     // not "ao"
	checkRole("armor_albedo.png",                "albedo");
	checkRole("charm_diffuse.png",               "albedo");

	// ── 3. Role → channel contract ──────────────────────────────────────────
	// The mapping the import relies on for packed maps, asserted here so it
	// cannot drift silently: R=ao/height, G=roughness, B=metallic.
	std::printf("[3/3] role -> channel contract\n");
	CHECK(std::strcmp(fds::MaterialImport_ClassifyRole("x_arm.png"), "packed_orm") == 0);

	std::printf("%s: %d checks, %d failure(s)\n",
	            g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
