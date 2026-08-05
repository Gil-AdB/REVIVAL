#include "DisplaceRebuild.h"

#include "MeshOps.h"
#include "MaterialEditor.h"        // rev::Editor_MarkDirty
#include <RENDER/GreetsMirror.h>   // fds::Mirror (clone meshes + cloneFaceSrc)
#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Base/FeatureFlags.h>
#include <Base/Material.h>
#include <Base/Scene.h>
#include <Base/Texture.h>
#include <Base/TriMesh.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/heap.h>
#endif

namespace rev {
namespace {

// The stone materials the greets shell is built on. Same pair
// Initialize_Greets passes to PomShell_Build — kept here (rather than shared)
// so this file adds no coupling to GREETS.CPP's internals.
const char *const kStoneMats[] = { "rooms", "floor" };

// A stone face may render through the BASE material or through its "::mirUV"
// handedness clone — GreetsFixBitangentHandedness re-points every negative-UV-
// determinant face onto a clone AFTER PomShell_Build has run, so the clone
// inherits the shell tables by struct copy. Post-init that means 'floor' has
// ZERO faces left under its own name (measured: 30 -> 0), so a naive rebuild
// silently builds no floor shell at all. Everything here therefore matches on
// the BASE name and the propagation step below re-syncs the clones.
bool sameSurface(const char *matName, const char *stone) {
	if (!matName) return false;
	if (!std::strcmp(matName, stone)) return true;
	const size_t n = std::strlen(stone);
	return !std::strncmp(matName, stone, n) && !std::strcmp(matName + n, "::mirUV");
}

struct VSnap { float x, y, z, shellH; };
struct FSnap { float nx, ny, nz, normProd; unsigned short group; };

struct MeshSnap {
	TriMesh              *T = nullptr;
	std::vector<unsigned> vIdx;
	std::vector<VSnap>    v;
	std::vector<unsigned> fIdx;
	std::vector<FSnap>    f;
	float                 bsRadius = 0.0f, bsRad = 0.0f;
};

// What the offline bakes were last produced with, per stone material. A cone
// map baked at pom_cone_exact=0 decodes on a different scale than one baked at
// 1/2 (Mekalele.h ctx.coneUnit reads the LIVE flag), so a mode switch that
// changes the flag MUST re-bake or the march reads garbage — that is one of the
// two "half-applies" this module exists to close.
struct BakeState {
	int   coneExact  = -1;      // -1 = no cone map baked
	float horizonAmp = 0.0f;    // 0 = no horizon map baked
	int   horizonRad = 0;
};

Scene                *g_sc = nullptr;
std::vector<MeshSnap> g_snap;
BakeState             g_bake[2];
DisplaceRebuildState  g_state;
bool                  g_captured = false;
// --pom_shell as the CALLER asked for it, stashed across scene init so the
// snapshot below is taken on geometry no shell has touched.
bool                  g_stashedShell = false;
bool                  g_stashActive  = false;

bool isStone(const Face *F) {
	if (!F || !F->Txtr || !F->Txtr->Name) return false;
	for (const char *n : kStoneMats)
		if (sameSurface(F->Txtr->Name, n)) return true;
	return false;
}

Material *stoneMat(Scene *sc, const char *name) {
	for (Material *M = MatLib; M; M = M->Next)
		if (M->RelScene == sc && M->Name && !std::strcmp(M->Name, name)) return M;
	return nullptr;
}

// The "__mirrorClone_*" meshes. They are built at the very END of scene init,
// long after PomShell_Build, from geometry that already carries the shell — so
// the rebuild must not let PomShell_Build see them or the patch grouping is a
// different problem than the one init solved.
void collectMirrorCloneMeshes(Scene *sc, std::vector<TriMesh*> &out) {
	for (Object *Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
		if (Obj->Type != Obj_TriMesh || !Obj->Name || !Obj->Data) continue;
		if (std::strncmp(Obj->Name, "__mirrorClone_", 14) != 0) continue;
		out.push_back((TriMesh*)Obj->Data);
	}
}

bool armed() {
	return fds::FeatureFlags::pom_rebuild() || fds::FeatureFlags::pom_rebuild_test() > 0;
}

long heapKb() {
#ifdef __EMSCRIPTEN__
	return long(emscripten_get_heap_size() / 1024);
#else
	return 0;
#endif
}

// Free + clear every table PomShell_Build publishes on a material, so a rebuild
// starts from the same blank slate scene load did. Leaving them would leak one
// domain table per toggle AND leave a stale table live for a mode that no
// longer builds one.
void clearShellTables(Material *M) {
	if (!M) return;
	delete[] M->PomShellDomains;     M->PomShellDomains     = nullptr;
	delete[] M->PomShellSibBoxes;    M->PomShellSibBoxes    = nullptr;
	delete[] M->PomShellSibOfs;      M->PomShellSibOfs      = nullptr;
	delete[] M->PomShellSideCls;     M->PomShellSideCls     = nullptr;
	delete[] M->PomShellSideLean;    M->PomShellSideLean    = nullptr;
	delete[] M->PomShellPatchUvAmp;  M->PomShellPatchUvAmp  = nullptr;
	M->PomShellDomainCount = 0;
	M->PomShellUvAmp       = 0.0f;
	M->PomShellWorldAmp    = 0.0f;
}

// The "::mirUV" clone ALIASES the base material's tables (GreetsFixBitangent-
// Handedness struct-copies the Material after the shell was built), so it must
// be nulled, never freed — and it must be nulled even when the new mode builds
// no shell, or it keeps pointing at storage clearShellTables just released.
void nullShellTables(Material *M) {
	if (!M) return;
	M->PomShellDomains     = nullptr;
	M->PomShellSibBoxes    = nullptr;
	M->PomShellSibOfs      = nullptr;
	M->PomShellSideCls     = nullptr;
	M->PomShellSideLean    = nullptr;
	M->PomShellPatchUvAmp  = nullptr;
	M->PomShellDomainCount = 0;
	M->PomShellUvAmp       = 0.0f;
	M->PomShellWorldAmp    = 0.0f;
}

}  // namespace

void DisplaceRebuild_Capture(Scene *sc)
{
	if (!armed() || g_captured || !sc) return;
	g_captured = true;
	g_sc       = sc;
	g_snap.clear();

	long bytes = 0;
	int  nV = 0, nF = 0;
	for (TriMesh *T = sc->TriMeshHead; T; T = T->Next) {
		if (!T->Faces || !T->Verts || T->FIndex == 0) continue;
		MeshSnap ms;
		std::vector<char> seen(size_t(T->VIndex), 0);
		for (int32_t i = 0; i < T->FIndex; ++i) {
			const Face &F = T->Faces[i];
			if (!F.A || !F.B || !F.C || !isStone(&F)) continue;
			ms.fIdx.push_back(unsigned(i));
			ms.f.push_back({ F.N.x, F.N.y, F.N.z, F.NormProd, F.PomShellGroup });
			const Vertex *pv[3] = { F.A, F.B, F.C };
			for (const Vertex *v : pv) {
				const unsigned vi = unsigned(v - T->Verts);
				if (vi >= unsigned(T->VIndex) || seen[vi]) continue;
				seen[vi] = 1;
				ms.vIdx.push_back(vi);
				ms.v.push_back({ v->Pos.x, v->Pos.y, v->Pos.z, v->ShellH });
			}
		}
		if (ms.f.empty()) continue;
		ms.T = T;
		ms.bsRadius = T->BSphereRadius;
		ms.bsRad    = T->BSphereRad;
		bytes += long(ms.v.size() * (sizeof(VSnap) + sizeof(unsigned))
		            + ms.f.size() * (sizeof(FSnap) + sizeof(unsigned)));
		nV += int(ms.v.size());
		nF += int(ms.f.size());
		g_snap.push_back(std::move(ms));
	}

	// Seed the bake bookkeeping from what scene init actually produced.
	for (int i = 0; i < 2; ++i) {
		Material *M = stoneMat(sc, kStoneMats[i]);
		g_bake[i] = BakeState{};
		if (!M) continue;
		if (M->ConeMap)
			g_bake[i].coneExact = fds::FeatureFlags::pom_cone_exact();
		if (M->PomHorizon) {
			g_bake[i].horizonAmp = fds::FeatureFlags::parallax_strength() * M->ParallaxScale;
			g_bake[i].horizonRad = fds::FeatureFlags::pom_horizon_radius();
		}
	}

	g_state.armed       = !g_snap.empty();
	g_state.tessellated = fds::FeatureFlags::greets_displace();
	g_state.shellBuilt  = false;
	g_state.meshes      = int(g_snap.size());
	g_state.verts       = nV;
	g_state.faces       = nF;
	g_state.bytes       = bytes;

	std::fprintf(stderr, "[DISPLACE-REBUILD] pristine snapshot: %d meshes / %d verts / "
		"%d stone faces, %.1f KB%s\n", g_state.meshes, nV, nF, bytes / 1024.0,
		g_state.tessellated ? "  — scene is TESSELLATED (--greets_displace): "
		                      "shell rebuild disabled" : "");
}

bool DisplaceRebuild_Apply()
{
	if (!g_state.armed || !g_sc) return false;
	if (g_state.tessellated) {
		std::fprintf(stderr, "[DISPLACE-REBUILD] refused: the scene was initialised "
			"with --greets_displace. The tessellation bake subdivides in place before "
			"the chunk split, so its geometry cannot be restored live — relaunch "
			"without it to use the shell modes.\n");
		return false;
	}
	const auto t0    = std::chrono::steady_clock::now();
	const long heap0 = heapKb();

	// ── 1. RESTORE PRISTINE ────────────────────────────────────────────────
	// Vertex::N is deliberately NOT restored: a live smoothing-angle edit
	// (MeshOps_ResmoothSurface) must survive a rebuild, and the shell is built
	// FROM the normals, so the rebuild should honour the current ones.
	for (MeshSnap &ms : g_snap) {
		TriMesh *T = ms.T;
		for (size_t k = 0; k < ms.vIdx.size(); ++k) {
			Vertex &V = T->Verts[ms.vIdx[k]];
			const VSnap &s = ms.v[k];
			V.Pos.x = s.x; V.Pos.y = s.y; V.Pos.z = s.z;
			V.ShellH = s.shellH;
		}
		for (size_t k = 0; k < ms.fIdx.size(); ++k) {
			Face &F = T->Faces[ms.fIdx[k]];
			const FSnap &s = ms.f[k];
			F.N.x = s.nx; F.N.y = s.ny; F.N.z = s.nz;
			F.NormProd = s.normProd;
			F.PomShellGroup = s.group;
		}
		T->BSphereRadius = ms.bsRadius;
		T->BSphereRad    = ms.bsRad;
	}
	for (const char *n : kStoneMats) {
		const std::string mir = std::string(n) + "::mirUV";
		nullShellTables(stoneMat(g_sc, mir.c_str()));   // aliases; null before free
		clearShellTables(stoneMat(g_sc, n));
	}

	// ── 2. RE-RUN THE INIT BAKE PATH WITH THE CURRENT FLAGS ────────────────
	// Same calls, same order, same arguments as Initialize_Greets — this file
	// deliberately owns no bake logic of its own.
	for (int i = 0; i < 2; ++i) {
		Material *M = stoneMat(g_sc, kStoneMats[i]);
		if (!M || !M->HeightMap) continue;

		// Cone map (--parallax_pom_cone / --pom_cone_exact). Re-baked only when
		// the requested bake differs from the one on the material; the flag also
		// selects the rasterizer's DECODE scale, so a mismatch is not cosmetic.
		if (fds::FeatureFlags::parallax_pom_cone() && M->HeightMap->BPP == 8) {
			const int want = fds::FeatureFlags::pom_cone_exact();
			if (!M->ConeMap || g_bake[i].coneExact != want) {
				const auto c0 = std::chrono::steady_clock::now();
				M->ConeMap = want > 0 ? LoadOrBakeConeMapExact(M->HeightMap, want, M->Name)
				                      : MakeConeMap(M->HeightMap);
				g_bake[i].coneExact = M->ConeMap ? want : -1;
				std::fprintf(stderr, "[DISPLACE-REBUILD] '%s' cone map %s (exact=%d, %.0f ms)\n",
					M->Name, M->ConeMap ? "baked" : "FAILED", want,
					std::chrono::duration<double, std::milli>(
						std::chrono::steady_clock::now() - c0).count());
			}
		}

		// Horizon map (--pom_horizon). Same layout guard scene init applies: the
		// kernel addresses it with the ALBEDO's swizzled UV.
		if (fds::FeatureFlags::pom_horizon() && M->HeightMap->BPP == 8) {
			const float amp = fds::FeatureFlags::parallax_strength() * M->ParallaxScale;
			const int   rad = fds::FeatureFlags::pom_horizon_radius();
			if (!M->PomHorizon || g_bake[i].horizonAmp != amp || g_bake[i].horizonRad != rad) {
				const auto h0 = std::chrono::steady_clock::now();
				M->PomHorizon = LoadOrBakeHorizonMap(M->HeightMap, amp, rad, M->Name);
				g_bake[i].horizonAmp = M->PomHorizon ? amp : 0.0f;
				g_bake[i].horizonRad = M->PomHorizon ? rad : 0;
				std::fprintf(stderr, "[DISPLACE-REBUILD] '%s' horizon map %s (%.0f ms)\n",
					M->Name, M->PomHorizon ? "baked" : "FAILED",
					std::chrono::duration<double, std::milli>(
						std::chrono::steady_clock::now() - h0).count());
				if (M->PomHorizon) {
					const Texture *A = M->Txtr;
					const PomHorizonMap *Hz = M->PomHorizon;
					if (!(A && A->SizeX == Hz->sizeX && A->SizeY == Hz->sizeY
					      && A->blockSizeX == Hz->blockSizeX
					      && A->blockSizeY == Hz->blockSizeY
					      && A->numMipmaps == Hz->numMipmaps)) {
						std::fprintf(stderr, "[DISPLACE-REBUILD] '%s': horizon layout != "
							"albedo — DROPPED (the kernel indexes it with the albedo UV)\n",
							M->Name);
						M->PomHorizon = nullptr;
						g_bake[i].horizonAmp = 0.0f;
					}
				}
			}
		}

	}

	// ── 3. THE SHELL, ON THE FACE SET SCENE INIT USED ──────────────────────
	// PomShell_Build runs at init BEFORE the Piramid chunk split, the "::mirUV"
	// handedness split and the mirror clone build. Re-running it on the final
	// scene therefore has to put those three back the way they were for the
	// duration of the call, or it solves a different problem:
	//   • mirror clones in the walk: 'rooms' goes 67 patches -> 113, the
	//     sibling lists CLAMP at kPomShellMaxSibs, and the domain test changes
	//     for the REAL walls too (measured: 58% of the frame).
	//   • "::mirUV" faces: 'floor' has no faces left under its own name, so
	//     nothing is built for it at all.
	if (fds::FeatureFlags::pom_shell()) {
		// (a) HIDE the clone meshes from PomShell_Build's walks. Both of them
		//     skip a mesh with FIndex == 0, so zeroing the count excludes the
		//     clone without touching the TriMesh chain at all. (An earlier
		//     version unlinked and relinked them; that is chain surgery whose
		//     ordering is only correct by accident, and measurement showed it
		//     bought nothing, so this is the cheaper and safer form.)
		std::vector<TriMesh*> clones;
		collectMirrorCloneMeshes(g_sc, clones);
		std::vector<int32_t> savedFIndex(clones.size());
		for (size_t k = 0; k < clones.size(); ++k) {
			savedFIndex[k] = clones[k]->FIndex;
			clones[k]->FIndex = 0;
		}
		// (b) re-point every "::mirUV" face back onto its base material
		std::vector<Face*>     splitFaces;
		std::vector<Material*> splitMats;
		for (TriMesh *T = g_sc->TriMeshHead; T; T = T->Next) {
			if (!T->Faces) continue;
			for (int32_t fi = 0; fi < T->FIndex; ++fi) {
				Face &F = T->Faces[fi];
				if (!F.Txtr || !F.Txtr->Name) continue;
				for (const char *n : kStoneMats) {
					if (std::strcmp(F.Txtr->Name, n) == 0) break;   // already base
					if (!sameSurface(F.Txtr->Name, n)) continue;
					Material *base = stoneMat(g_sc, n);
					if (!base) break;
					splitFaces.push_back(&F);
					splitMats.push_back(F.Txtr);
					F.Txtr = base;
					break;
				}
			}
		}
		// (c) build. --pom_shell_weld=3 needs ONE scene-wide position bucket
		//     taken before the first build moves a vertex (see MeshOps.h), and
		//     it must run AFTER (b) has folded the ::mirUV clones back onto
		//     their base material so the clone faces are counted too.
		{
			const char *weldMats[sizeof(kStoneMats)/sizeof(kStoneMats[0])];
			int nWeldMats = 0;
			for (const char *n : kStoneMats) {
				Material *M = stoneMat(g_sc, n);
				if (M && M->HeightMap) weldMats[nWeldMats++] = n;
			}
			PomShell_WeldPrepare(g_sc, weldMats, nWeldMats);
		}
		for (const char *n : kStoneMats) {
			Material *M = stoneMat(g_sc, n);
			if (!M || !M->HeightMap) continue;
			PomShell_Build(g_sc, n,
			               fds::FeatureFlags::parallax_strength() * M->ParallaxScale,
			               fds::FeatureFlags::pom_shell_pin());
		}
		PomShell_WeldReset();
		// (d) restore the handedness split and hand the clones the tables they
		//     inherited by struct copy at init (shared, not owned — only the
		//     base material's arrays are ever freed).
		for (size_t k = 0; k < splitFaces.size(); ++k) splitFaces[k]->Txtr = splitMats[k];
		for (const char *n : kStoneMats) {
			const Material *base = stoneMat(g_sc, n);
			if (!base) continue;
			std::string mir = std::string(n) + "::mirUV";
			Material *C = stoneMat(g_sc, mir.c_str());
			if (!C) continue;
			C->PomShellUvAmp       = base->PomShellUvAmp;
			C->PomShellWorldAmp    = base->PomShellWorldAmp;
			C->PomShellDomains     = base->PomShellDomains;
			C->PomShellDomainCount = base->PomShellDomainCount;
			C->PomShellSibBoxes    = base->PomShellSibBoxes;
			C->PomShellSibOfs      = base->PomShellSibOfs;
			C->PomShellSideCls     = base->PomShellSideCls;
			C->PomShellSideLean    = base->PomShellSideLean;
			C->PomShellPatchUvAmp  = base->PomShellPatchUvAmp;
		}
		// (e) restore the clone face counts and re-stamp each clone face's patch
		//     id from its SOURCE face (Mirror::cloneFaceSrc is parallel to
		//     cloneMesh->Faces). primed=false makes the next UpdateMirror do a
		//     full re-mirror, so a lid offset propagates into the reflection.
		for (size_t k = 0; k < clones.size(); ++k) clones[k]->FIndex = savedFIndex[k];
		if (std::vector<fds::Mirror> *mirrors = Greets_MirrorList()) {
			for (fds::Mirror &m : *mirrors) {
				if (!m.cloneMesh) continue;
				// Vertex::ShellH — the march's ENTRY HEIGHT. At scene init the
				// shell is built BEFORE BuildMirror, so the clone's vertex copies
				// carry the stamped 1.0 (recess) / 0.5+ndv/2 (lid). Here the
				// mirror already exists, and UpdateMirror re-mirrors Pos, N,
				// Tangent and colour but NOT ShellH — so without this the
				// reflection marches from the 0.5 default and the relief inside
				// the mirror is wrong. Measured at the t=5743 review pose: this
				// was the whole of the rebuild-vs-relaunch difference in
				// recess-only mode (11 178 px, all inside the teleporter panel).
				// Same source→clone vertex mapping UpdateMirror uses.
				for (const fds::ClonedMeshRange &r : m.meshRanges) {
					const TriMesh *T = r.sourceMesh;
					if (!T || !T->Verts) continue;
					const uint32_t n = std::min<uint32_t>(r.vCount, uint32_t(T->VIndex));
					for (uint32_t vi = 0; vi < n; ++vi)
						m.cloneMesh->Verts[r.vStart + vi].ShellH = T->Verts[vi].ShellH;
				}
				const size_t nf = std::min<size_t>(size_t(m.cloneMesh->FIndex),
				                                   m.cloneFaceSrc.size());
				for (size_t f = 0; f < nf; ++f)
					if (m.cloneFaceSrc[f].face)
						m.cloneMesh->Faces[f].PomShellGroup =
							m.cloneFaceSrc[f].face->PomShellGroup;
				m.primed = false;
			}
		}
	}

	g_state.shellBuilt = false;
	for (const char *n : kStoneMats) {
		const Material *M = stoneMat(g_sc, n);
		if (M && M->PomShellUvAmp > 0.0f) g_state.shellBuilt = true;
	}
	g_state.lastMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - t0).count();
	g_state.lastPeakKb = heapKb();
	++g_state.rebuilds;
	std::fprintf(stderr, "[DISPLACE-REBUILD] #%d done in %.1f ms — shell %s, recess=%d, "
		"cone=%d/%d, horizon=%d  heap %ld -> %ld KB\n",
		g_state.rebuilds, g_state.lastMs, g_state.shellBuilt ? "BUILT" : "off",
		fds::FeatureFlags::pom_recess_only() ? 1 : 0,
		g_bake[0].coneExact, g_bake[1].coneExact,
		fds::FeatureFlags::pom_horizon() ? 1 : 0, heap0, g_state.lastPeakKb);

	Editor_MarkDirty();
	return true;
}

DisplaceRebuildState DisplaceRebuild_State() { return g_state; }

const char *DisplaceRebuild_StateJson()
{
	static std::string out;
	using FF = fds::FeatureFlags;
	char buf[1024];
	std::snprintf(buf, sizeof buf,
		"{\"armed\":%d,\"tessellated\":%d,\"shellBuilt\":%d,\"meshes\":%d,"
		"\"verts\":%d,\"faces\":%d,\"bytes\":%ld,\"lastMs\":%.1f,"
		"\"heapKb\":%ld,\"rebuilds\":%d,\"flags\":{",
		g_state.armed ? 1 : 0, g_state.tessellated ? 1 : 0,
		g_state.shellBuilt ? 1 : 0, g_state.meshes, g_state.verts, g_state.faces,
		g_state.bytes, g_state.lastMs, heapKb(), g_state.rebuilds);
	out = buf;
	auto addB = [&](const char *n, bool v, bool first = false) {
		out += first ? "" : ","; out += "\""; out += n; out += "\":"; out += v ? "1" : "0";
	};
	auto addN = [&](const char *n, double v) {
		char b[64]; std::snprintf(b, sizeof b, ",\"%s\":%.4f", n, v); out += b;
	};
	addB("pom_shell",        FF::pom_shell(), true);
	addB("pom_recess_only",  FF::pom_recess_only());
	addB("pom_normal",       FF::pom_normal());
	addB("pom_horizon",      FF::pom_horizon());
	addB("parallax",         FF::parallax());
	addB("parallax_pom_cone",FF::parallax_pom_cone());
	addB("pom_march_earlyout", FF::pom_march_earlyout());
	addB("pom_shell_world_amp", FF::pom_shell_world_amp());
	addB("greets_displace",  FF::greets_displace());
	addN("parallax_pom",           FF::parallax_pom());
	addN("parallax_strength",      FF::parallax_strength());
	addN("pom_shell_cap",          FF::pom_shell_cap());
	addN("pom_cone_exact",         FF::pom_cone_exact());
	addN("pom_cone_min_step",      FF::pom_cone_min_step());
	addN("pom_shell_world_amp_set",FF::pom_shell_world_amp_set());
	addN("pom_recess_edge",        FF::pom_recess_edge());
	addN("pom_normal_strength",    FF::pom_normal_strength());
	out += "}}";
	return out.c_str();
}

// ── init-time plumbing (called from Initialize_Greets) ──────────────────────
// The editor and the self-test both want the scene to come up with the stone
// PRISTINE so the snapshot is pristine by construction; the requested shell
// mode is then applied through DisplaceRebuild_Apply, i.e. through exactly the
// path the panel uses. That also means the very first thing the mode ever does
// is the thing the idempotency gate measures.
void DisplaceRebuild_BeginInit()
{
	g_stashActive = false;
	if (!armed()) return;
	g_stashedShell = fds::FeatureFlags::pom_shell();
	if (!g_stashedShell) return;
	fds::FeatureFlags::setParamFromText("pom_shell", "0");
	g_stashActive = true;
	std::fprintf(stderr, "[DISPLACE-REBUILD] --pom_shell deferred past scene init "
		"(--pom_rebuild): the shell is applied through the live rebuild path\n");
}

void DisplaceRebuild_EndInit(Scene *sc)
{
	if (!armed()) return;
	DisplaceRebuild_Capture(sc);
	if (g_stashActive) {
		fds::FeatureFlags::setParamFromText("pom_shell", "1");
		g_stashActive = false;
		DisplaceRebuild_Apply();
	}
	DisplaceRebuild_SelfTest();
}

void DisplaceRebuild_SelfTest()
{
	const int n = fds::FeatureFlags::pom_rebuild_test();
	if (n <= 0 || !g_state.armed) return;
	// A real MODE TOGGLE, not N re-applies of the same mode: drop to the
	// no-shell mode and come back, N times. That is what the editor's mode
	// buttons do, and it is the only version of the test that can catch a
	// failure to restore — re-applying the same mode from an already-correct
	// state would pass even if the restore were a no-op.
	const bool wantShell  = fds::FeatureFlags::pom_shell();
	const bool wantRecess = fds::FeatureFlags::pom_recess_only();
	std::fprintf(stderr, "[DISPLACE-REBUILD-TEST] %d mode toggles (shell %d/recess %d "
		"<-> no shell); the frame that follows must match a fresh launch in this mode\n",
		n, wantShell ? 1 : 0, wantRecess ? 1 : 0);
	for (int i = 0; i < n; ++i) {
		fds::FeatureFlags::setParamFromText("pom_shell", "0");
		fds::FeatureFlags::setParamFromText("pom_recess_only", "0");
		DisplaceRebuild_Apply();
		fds::FeatureFlags::setParamFromText("pom_shell", wantShell ? "1" : "0");
		fds::FeatureFlags::setParamFromText("pom_recess_only", wantRecess ? "1" : "0");
		DisplaceRebuild_Apply();
	}
	std::fprintf(stderr, "[DISPLACE-REBUILD-TEST] %d cycles done (%d rebuilds total)\n",
		n, g_state.rebuilds);
}

}  // namespace rev
