// Synthetic test driver for FrustumClipper. Reproduces clipper bugs
// captured live from greets shadow rasterization (FDS_SHADOW_VALIDATE=1).
//
// Each test case constructs a triangle with known PX/PY/RZ, runs the
// clipper, and validates the output via a capturing filler. Reports
// PASS / FAIL with detailed diagnostics on FAIL.
//
// Build:  cmake --build build --target clipper_test
// Run:    ./build/tests/clipper_test
//
// Add new cases by appending to g_tests below — copy the verbatim
// "[SHADOW-CLIP] BAD" output from a live run, paste into a TestCase,
// fill in the expected polygon vertices (hand-computed against the
// clipper's intended behaviour).

#include "Base/FDS_VARS.H"
#include "Base/Scene.h"
#include "Base/Material.h"
#include "Base/Vertex.h"
#include "Base/Face.h"
#include "Base/BaseDefs.h"
#include "Base/FrameState.h"
#include "FRUSTRUM.H"

#include <cstdio>
#include <cstring>
#include <vector>

// Symbols defined in DEMO/REV.CPP that the FDS library references but
// the test binary doesn't link DEMO. Provide stubs.
float dTime = 0.0f;

namespace {

struct CapturedVert { float PX, PY, RZ; };
struct CapturedPoly { std::vector<CapturedVert> verts; };
std::vector<CapturedPoly> g_captured;

void capturing_filler(Face*, Vertex** V, dword numVerts, dword,
                       const fds::RenderTarget&,
                       const fds::CameraContext&) {
	CapturedPoly p;
	p.verts.reserve(numVerts);
	for (dword i = 0; i < numVerts; ++i) {
		p.verts.push_back({V[i]->PX, V[i]->PY, V[i]->RZ});
	}
	g_captured.push_back(std::move(p));
}

// Build a Vertex from screen-space (PX, PY) + 1/z (RZ). Re-derives
// TPos_AOS so the clipper's Near/Far checks have valid view-space z.
//   TPos_AOS.z = 1/RZ;  TPos_AOS.x = PX/RZ;  TPos_AOS.y = PY/RZ
// (matches the engine's perspective-baked storage: PX = TPos_AOS.x * RZ.)
Vertex makeVertex(float PX, float PY, float RZ) {
	Vertex v{};
	v.PX = PX;
	v.PY = PY;
	v.RZ = RZ;
	v.TPos_AOS.z = 1.0f / RZ;
	v.TPos_AOS.x = PX / RZ;
	v.TPos_AOS.y = PY / RZ;
	v.UZ = 0; v.VZ = 0; v.EUZ = 0; v.EVZ = 0;
	v.U = 0;  v.V = 0;  v.EU = 0;  v.EV = 0;
	v.Flags = 0;
	v.LR = 128; v.LG = 128; v.LB = 128;
	v.TN.x = 0; v.TN.y = 0; v.TN.z = 1;
	v.TTangent.x = 1; v.TTangent.y = 0; v.TTangent.z = 0;
	return v;
}

struct TestCase {
	const char *name;
	// Input triangle vertices in current-camera screen space.
	float aPX, aPY, aRZ;
	float bPX, bPY, bRZ;
	float cPX, cPY, cRZ;
	// Expected polygon vertex count after clipping. The test prints
	// the output regardless; this is just a quick PASS/FAIL gate.
	int expectedVertCount;
	const char *expectedDescription;
};

const TestCase g_tests[] = {
	{
		"case3_three_corner_clip",
		// Captured live from greets shadow pass:
		// input A: PX=-1054.34 PY=-6461.19 RZ=0.523696  (out: top + left)
		// input B: PX=906.298  PY=965.196  RZ=0.304567  (inside)
		// input C: PX=587.317  PY=1155.05  RZ=0.316048  (out: bottom)
		-1054.34f, -6461.19f, 0.523696f,
		 906.298f,   965.196f, 0.304567f,
		 587.317f,  1155.05f,  0.316048f,
		5,
		"5 verts: AB∩top(651.4,0), B(906.3,965), BC∩bot(807.5,1024), CA∩bot(559.1,1024), CA∩top(338.5,0)"
	},
	{
		"case4_two_off_screen",
		// input A: PX=494.73  PY=221.46    RZ=0.237095  (inside)
		// input B: PX=1839.16 PY=-3980.93  RZ=0.326386  (out: top + right)
		// input C: PX=-1054.34 PY=-6461.19 RZ=0.523696  (out: top + left)
		 494.73f,    221.46f,  0.237095f,
		1839.16f,  -3980.93f,  0.326386f,
		-1054.34f, -6461.19f,  0.523696f,
		4,  // upper portion of A cut by clip rect — 3 to 5 verts depending on right/top wedges
		"polygon around A bounded by top edge (PY=0)"
	},
	{
		"case5_top_clip_spurious_corner",
		// Captured live from greets w/ FDS_SHADOW_SKIP_MIPLEVEL=1:
		//   A: PX=284.261139  PY=-1082.21411  RZ=0.0937811658  (out: top)
		//   B: PX=163.447144  PY=282.083405   RZ=0.166472584   (inside)
		//   C: PX=340.614075  PY=169.561188   RZ=0.155231118   (inside)
		// Expected: 4 verts — AB∩top(188.4,0), B, C, CA∩top(332.98,0).
		// Live output: 5 verts including (0,0) corner — bug.
		 284.261139f, -1082.21411f,  0.0937811658f,
		 163.447144f,   282.083405f, 0.166472584f,
		 340.614075f,   169.561188f, 0.155231118f,
		4,
		"4 verts: AB∩top(~188,0), B, C, CA∩top(~333,0). Live bug inserts spurious (0,0)."
	},
};

// Shared FrustumClipper instance — accumulates state across runTest
// calls, mimicking the live shadow pass which calls clipper.Render
// for many faces on a single instance. Theory under investigation:
// state pollution between calls causes the live bug.
FrustumClipper g_sharedClipper;

// Run one test. Sets up scene/camera globals matching the shadow pass,
// builds the input Face, calls clipper.Render. Reports the captured
// polygon(s).
bool runTest(const TestCase &tc) {
	std::printf("\n=== %s ===\n", tc.name);
	std::printf("Input:\n");
	std::printf("  A: PX=%g PY=%g RZ=%g\n", tc.aPX, tc.aPY, tc.aRZ);
	std::printf("  B: PX=%g PY=%g RZ=%g\n", tc.bPX, tc.bPY, tc.bRZ);
	std::printf("  C: PX=%g PY=%g RZ=%g\n", tc.cPX, tc.cPY, tc.cRZ);
	std::printf("Expected: %s\n", tc.expectedDescription);

	// Scene globals matching the greets shadow camera.
	XRes = 1024;
	YRes = 1024;
	FOVX = 265.0f;
	FOVY = 265.0f;
	CntrEX = 511.5f;
	CntrEY = 511.5f;
	AspectRatio = 1.0f;

	Scene sc{};
	sc.NZP = 0.01f;
	sc.FZP = 30.0f;

	// Build the input vertices + face. Material set to a stub so
	// Render's `F->Txtr->Txtr` check doesn't null-deref. Txtr is
	// null inside the material so the `&&` is false → mip path skipped.
	Vertex A = makeVertex(tc.aPX, tc.aPY, tc.aRZ);
	Vertex B = makeVertex(tc.bPX, tc.bPY, tc.bRZ);
	Vertex C = makeVertex(tc.cPX, tc.cPY, tc.cRZ);
	Material stubMat{};  // Txtr = nullptr
	Face F{};
	F.A = &A; F.B = &B; F.C = &C;
	F.Txtr = &stubMat;
	F.Flags = 0;
	F.U1 = 0; F.V1 = 0;
	F.U2 = 1; F.V2 = 0;
	F.U3 = 0; F.V3 = 1;

	g_sharedClipper.InitViewport(&sc);
	g_sharedClipper.SetClippingExtents(0.0f, 0.0f, float(XRes), float(YRes));

	g_captured.clear();
	const auto rt  = fds::MainRenderTargetFromGlobals();
	const auto& cam = fds::g_mainCamera;
	g_sharedClipper.Render(&F, capturing_filler, /*isEnvCoords=*/false,
	                       rt, cam,
	                       /*skipMipLevel=*/true);

	std::printf("Output: %zu polygon(s) captured\n", g_captured.size());
	if (g_captured.empty()) {
		std::printf("  (clipper produced no output)\n");
		return tc.expectedVertCount == 0;
	}

	int total = 0;
	for (size_t pi = 0; pi < g_captured.size(); ++pi) {
		const auto &p = g_captured[pi];
		std::printf("  poly[%zu]: %zu verts\n", pi, p.verts.size());
		for (size_t vi = 0; vi < p.verts.size(); ++vi) {
			const auto &v = p.verts[vi];
			std::printf("    v%zu: PX=%g PY=%g RZ=%g\n",
			            vi, v.PX, v.PY, v.RZ);
		}
		total += int(p.verts.size());
	}
	const bool pass = (total == tc.expectedVertCount);
	std::printf("Result: %s  (got %d verts total, expected %d)\n",
	            pass ? "PASS" : "FAIL", total, tc.expectedVertCount);
	return pass;
}

}  // namespace

int main() {
	int passed = 0;
	int failed = 0;
	for (const TestCase &tc : g_tests) {
		if (runTest(tc)) ++passed; else ++failed;
	}
	std::printf("\n=== Summary: %d passed, %d failed ===\n", passed, failed);
	return failed == 0 ? 0 : 1;
}
