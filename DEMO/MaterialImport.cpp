#include "MaterialImport.h"

#include "MaterialEditor.h"          // rev::Editor_BaseSurfName (::mirUV collapse)
#include "MeshOps.h"                 // MakeHeight8, MakeNormal16, BakeNormalMapFromDiffuse
#include <Base/FDS_VARS.H>           // MatLib
#include <Base/FDS_DECS.H>           // Load_Texture, BPPConvert_Texture, Generate_Mipmaps
#include <Base/FDS_DEFS.H>           // DEFAULT_BLOCKSIZEX/Y, Txtr_Tiled, Mat_AoInAlpha
#include <Base/Texture.h>
#include <Base/Material.h>
#include <Base/Scene.h>
#include <Base/Object.h>             // Object tree (editor scale knob)
#include <Base/TriMesh.h>            // EditorScale (editor scale knob)
#include <Base/FeatureFlags.h>
#include <FLD/FLD_READ.H>            // FldRevMap* — LWO/FLD-authored PBR map roles (§1e)

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>
#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>

// PREPROC.CPP — recompute per-vertex tangents from current Faces + maps.
void Compute_Vertex_Tangents(TriMesh *T);

namespace fds {

namespace {

struct ImportSpec { std::string matName, dir; };
std::vector<ImportSpec> g_specs;
bool g_forceFlipNormal = false;
bool g_noMips          = false;

// Our deferred kernel decodes a tangent-space normal as nmY = G*2-1 with the
// bitangent B = N×T. We treat the engine as **OpenGL** convention: greets'
// authored maps (greets_*_n.png) load correct at flipG=0, and FreePBR/ambientCG
// ship -ogl by a wide margin, so a -ogl (or unmarked) source gets NO green flip
// and a -dx source gets one. This DEFAULT is a best guess, not a hard proof —
// absolute relief direction is hard to confirm in the complex-lit greets scene.
// If an imported set's relief looks inverted (mortar bulges out / bumps cave
// in), pass --material-import-flip-normal to XOR the decision (same escape hatch
// as --greets-nmap-flip-g). Confirm by eye on a flat, clearly-lit surface.
constexpr bool kEngineExpectsOGL = true;

std::string expandTilde(const std::string &p) {
	if (p.size() >= 2 && p[0] == '~' && p[1] == '/') {
		const char *home = std::getenv("HOME");
		if (home) return std::string(home) + p.substr(1);
	}
	return p;
}

std::string lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return char(std::tolower(c)); });
	return s;
}

// Stem (filename minus directory + extension), lowercased.
std::string stemLower(const std::string &fname) {
	size_t slash = fname.find_last_of("/\\");
	std::string base = slash == std::string::npos ? fname : fname.substr(slash + 1);
	size_t dot = base.find_last_of('.');
	if (dot != std::string::npos) base = base.substr(0, dot);
	return lower(base);
}

bool tokenHas(const std::string &stem, const char *tok) {
	// Substring with separator boundaries so "ao" doesn't match "halo". A
	// match is valid if the char before/after the hit is a separator or end.
	const std::string t = tok;
	size_t pos = 0;
	while ((pos = stem.find(t, pos)) != std::string::npos) {
		const bool lOk = pos == 0 || stem[pos-1]=='_' || stem[pos-1]=='-' || stem[pos-1]==' ';
		const size_t end = pos + t.size();
		const bool rOk = end == stem.size() || stem[end]=='_' || stem[end]=='-' || stem[end]==' '
		                 || (stem[end] >= '0' && stem[end] <= '9');   // normal2, normal-ogl
		if (lOk && rOk) return true;
		pos = end;
	}
	return false;
}

enum class Role { None, Albedo, Normal, Height, Roughness, Ao, Metallic, Skip };

Role classify(const std::string &stem) {
	if (tokenHas(stem,"preview") || tokenHas(stem,"thumb") || tokenHas(stem,"thumbnail")
	    || tokenHas(stem,"sphere") || tokenHas(stem,"render")) return Role::Skip;
	// Order matters: specific tokens first.
	if (tokenHas(stem,"basecolor")||tokenHas(stem,"base_color")||tokenHas(stem,"base-color")
	    ||tokenHas(stem,"albedo")||tokenHas(stem,"diffuse")||tokenHas(stem,"color")) return Role::Albedo;
	if (tokenHas(stem,"normal")||tokenHas(stem,"nrm")||tokenHas(stem,"nor")) return Role::Normal;
	if (tokenHas(stem,"roughness")||tokenHas(stem,"rough")||tokenHas(stem,"rgh")) return Role::Roughness;
	if (tokenHas(stem,"height")||tokenHas(stem,"disp")||tokenHas(stem,"displacement")
	    ||tokenHas(stem,"bump")) return Role::Height;
	if (tokenHas(stem,"metallic")||tokenHas(stem,"metalness")||tokenHas(stem,"metal")) return Role::Metallic;
	if (tokenHas(stem,"ao")||tokenHas(stem,"occlusion")||tokenHas(stem,"ambientocclusion")) return Role::Ao;
	return Role::None;
}

bool hasImageExt(const std::string &fname) {
	const std::string s = lower(fname);
	const char *ok[] = { ".png",".jpg",".jpeg",".tga",".bmp",".tif",".tiff" };
	for (const char *e : ok) if (s.size() > std::strlen(e) && s.rfind(e) == s.size()-std::strlen(e)) return true;
	return false;
}

// Detect the source normal convention from filename tokens. Returns true=OGL.
// Default OGL (FreePBR/ambientCG ship -ogl by a wide margin; plain "normal"
// is almost always OGL too).
bool normalSourceIsOGL(const std::string &stem) {
	if (tokenHas(stem,"dx")||tokenHas(stem,"directx")) return false;
	return true;  // -ogl, opengl, or unmarked
}

// Max texture dimension the deferred path can address. The G-buffer packs the
// swizzled UV into a 20-bit field (mat32 = miplevel:4 | matID:8 | swizzledUV:20),
// so the texel index must fit in 2^20 = 1024*1024. A larger texture overflows
// the field and samples garbage (the "black band" on imported 2048² PBR sets).
// greets' own authored textures are 1024², which is why the engine never hit
// this. Cap imported textures here.
constexpr int kMaxTexDim = 1024;

static int ilog2(int v) { int l = 0; while ((1 << l) < v) ++l; return l; }

// Load an image → 32bpp → cap to kMaxTexDim → optional green flip → tile + mips.
// targetW/H (optional): force the final dimensions. The deferred kernel samples
// EVERY per-material map (normal/height/rough/ao) with the swizzled texel index
// + miplevel the rasterizer computed against the ALBEDO's layout — a map whose
// dimensions differ from the material's current albedo reads scrambled
// (per-pixel sparkle noise). Aux-map imports pass the albedo dims here.
Texture *loadTiled(const std::string &path, bool flipGreen,
                   int targetW = 0, int targetH = 0) {
	Texture *t = new Texture;
	t->FileName = strdup(path.c_str());
	t->BPP = 0;
	if (!Load_Texture(t)) { std::free(t->FileName); delete t; return nullptr; }
	if (t->BPP != 32) BPPConvert_Texture(t, 32);
	if (targetW > 0 && targetH > 0 && (t->SizeX != targetW || t->SizeY != targetH)) {
		Image img; img.FileName = nullptr; img.x = t->SizeX; img.y = t->SizeY;
		img.Data = (DWord*)_aligned_malloc(sizeof(DWord) * size_t(t->SizeX) * size_t(t->SizeY), 16);
		std::memcpy(img.Data, t->Data, sizeof(DWord) * size_t(t->SizeX) * size_t(t->SizeY));
		Scale_Image(&img, targetW, targetH);
		t->Data  = (byte*)img.Data;
		t->SizeX = targetW; t->SizeY = targetH;
		t->LSizeX = ilog2(targetW); t->LSizeY = ilog2(targetH);
		std::fprintf(stderr, "    [match] %s resampled to %dx%d (albedo texel layout)\n",
		             path.c_str(), targetW, targetH);
	}
	// Downsample if the source exceeds the deferred 20-bit UV budget. Scale_Image
	// mipmaps down + bilinear-resamples (any size, not just po2) and owns its
	// buffer; we leak the original t->Data (init-time, matches engine convention).
	if (t->SizeX > kMaxTexDim || t->SizeY > kMaxTexDim) {
		const int nx = std::min(t->SizeX, kMaxTexDim);
		const int ny = std::min(t->SizeY, kMaxTexDim);
		Image img; img.FileName = nullptr; img.x = t->SizeX; img.y = t->SizeY;
		img.Data = (DWord*)_aligned_malloc(sizeof(DWord) * size_t(t->SizeX) * size_t(t->SizeY), 16);
		std::memcpy(img.Data, t->Data, sizeof(DWord) * size_t(t->SizeX) * size_t(t->SizeY));
		Scale_Image(&img, nx, ny);   // frees+reallocs img.Data to nx*ny
		t->Data  = (byte*)img.Data;
		t->SizeX = nx; t->SizeY = ny;
		t->LSizeX = ilog2(nx); t->LSizeY = ilog2(ny);
		std::fprintf(stderr, "    [cap] %s downsampled to %dx%d (20-bit UV limit)\n", path.c_str(), nx, ny);
	}
	if (flipGreen) {
		dword *px = (dword *)t->Data;
		const size_t n = size_t(t->SizeX) * size_t(t->SizeY);
		for (size_t i = 0; i < n; ++i) {
			const dword g = (px[i] >> 8) & 0xFF;
			px[i] = (px[i] & 0xFFFF00FFu) | ((255u - g) << 8);
		}
	}
	t->Flags |= Txtr_Tiled;
	Generate_Mipmaps(t, DEFAULT_BLOCKSIZEX, DEFAULT_BLOCKSIZEY, g_noMips ? 0 : 1);
	return t;
}

// ── Imported-texture dedup cache ────────────────────────────────────────────
// Applying the SAME PBR map to multiple surfaces/objects used to `new Texture` +
// re-decode the PNG (Load_Texture) + re-tile/mip AND re-run MakeNormal16/
// MakeHeight8 on EVERY application — pure waste, and each redundant allocation is
// another chance for a wasm heap-grow to fire mid-atomic and trap ("unaligned
// memory access", emscripten#17816/#23806, under the editor's -pthread +
// ALLOW_MEMORY_GROWTH build). Cache the FULLY-PROCESSED texture keyed by every
// input that changes its decoded bytes: source path + role + green-flip + the
// target (albedo-matched) dimensions. A second application of the same map to a
// different surface reuses the cached Texture* — zero decode, zero alloc, and the
// two surfaces literally share one Texture object (true reuse).
//
// LIFETIME: process-global, never freed — matches the existing convention. The
// editor session is short-lived, Texture blocks aren't refcounted, and
// re-imports/reset already leak the previous Texture on purpose (see
// MaterialImport_ClearSurfaceMap). The cached texture is aliased across every
// surface that imports the same map; the one in-place mutation of a shared
// imported map is the normalFlip toggle (FlipNormalMapG), whose parity is tracked
// per-Texture (g_nmapFlipParity) — surfaces sharing one source normal map share
// its convention, which is correct (convention is a property of the source file).
std::unordered_map<std::string, Texture*> g_importCache;

// Load + role-convert a map, deduped by (path, role, flip, targetW, targetH).
// Returns the shared, ready-to-assign Texture: albedo -> tiled 32bpp; normal ->
// +MakeNormal16 when nmap_16bit; height/roughness/ao/metallic -> +MakeHeight8
// (falls back to the 32bpp texture if MakeHeight8 can't allocate).
Texture *loadRoleMapCached(const std::string &path, const char *role,
                           bool flipGreen, int targetW, int targetH) {
	char suffix[96];
	std::snprintf(suffix, sizeof suffix, "|%s|%d|%dx%d",
	              role, flipGreen ? 1 : 0, targetW, targetH);
	const std::string key = path + suffix;
	auto it = g_importCache.find(key);
	if (it != g_importCache.end()) {
		std::fprintf(stderr, "    [reuse] %s (%s%s%s) — cached, no re-decode\n",
		             path.c_str(), role, flipGreen ? ", flipG" : "",
		             (targetW > 0 && targetH > 0) ? ", albedo-matched" : "");
		return it->second;
	}
	Texture *t = loadTiled(path, flipGreen, targetW, targetH);
	if (!t) return nullptr;
	if (!std::strcmp(role, "normal")) {
		if (fds::FeatureFlags::nmap_16bit()) { if (Texture *t16 = MakeNormal16(t)) t = t16; }
	} else if (std::strcmp(role, "albedo") != 0) {
		// height / roughness / ao / metallic -> single-channel 8-bit.
		if (Texture *t8 = MakeHeight8(t)) t = t8;
	}
	g_importCache[key] = t;
	std::fprintf(stderr, "    [load] %s (%s) decoded + cached\n", path.c_str(), role);
	return t;
}

// Mean roughness (0..1) of a loaded roughness texture's base level: 8-bit
// single-channel (MakeHeight8) or the 32bpp fallback (low/blue byte — the
// same byte the kernel's attenuation samples).
float roughnessMapMean(const Texture *t) {
	if (!t) return 0.5f;
	const byte *d = (t->numMipmaps > 0 && t->Mipmap[0]) ? t->Mipmap[0] : t->Data;
	if (!d) return 0.5f;
	const size_t n = size_t(t->SizeX) * size_t(t->SizeY);
	if (!n) return 0.5f;
	uint64_t sum = 0;
	if (t->BPP == 8)       for (size_t i = 0; i < n; ++i) sum += d[i];
	else if (t->BPP == 32) for (size_t i = 0; i < n; ++i) sum += d[i * 4];
	else return 0.5f;
	return float(sum) / (255.0f * float(n));
}

// DIELECTRIC specular seed for a surface whose author left Specular at 0 and
// that just gained a ROUGHNESS map (both import paths call this). The old
// blanket Specular=0.5/Glossiness=32 turned every matte surface SHINY the
// moment a rough dielectric set (Polyhaven sandstone) landed on it — 0.5 is
// ~12x a dielectric's F0 (4%), and gloss 32 is a tight lobe regardless of how
// rough the map says the surface is (the map only ATTENUATES intensity in the
// kernel's cheap tier; under --pbr the GGX lobe derives its roughness from
// Glossiness, not the map). Seed instead:
//   Specular   = 0.08  (~2x the 4% dielectric F0 — Blinn's unnormalized lobe
//                needs a little headroom to read at all; specMul is the dial)
//   Glossiness = from the MAP's mean roughness via the engine's own
//                documented mapping (rough = sqrt(2/(gloss+2)), inverted:
//                gloss = 2/rough^2 - 2), snapped to the nearest vectorized
//                spec-loop case so a big seeded surface stays on the vec path.
// Only fires when the author left BOTH at 0 — authored values always win.
void seedDielectricSpecular(Material *M, const Texture *rough, const char *ctx) {
	if (M->Specular > 0.0f) return;
	M->Specular = 0.08f;
	if (M->Glossiness == 0) {
		float r = roughnessMapMean(rough);
		if (r < 0.05f) r = 0.05f;
		const float g = 2.0f / (r * r) - 2.0f;
		static const unsigned short kVecCases[] = { 4, 8, 16, 32, 48, 64, 128 };
		unsigned short best = kVecCases[0];
		for (unsigned short c : kVecCases)
			if (std::fabs(float(c) - g) < std::fabs(float(best) - g)) best = c;
		M->Glossiness = best;
	}
	std::fprintf(stderr, "    [spec] roughness map + Specular was 0 -> dielectric "
	             "seed Specular=%.2f Glossiness=%u (gloss from the map's mean "
	             "roughness; %s)\n", M->Specular, M->Glossiness, ctx);
}

} // namespace

void MaterialImport_ParseArgs(int argc, const char *const *argv) {
	for (int i = 1; i < argc; ++i) {
		if (!argv[i]) continue;
		const std::string a = argv[i];
		if (a == "--material-import-flip-normal") { g_forceFlipNormal = true; continue; }
		if (a == "--material-import-no-mips")     { g_noMips = true; continue; }
		const char *pfx = "--material-import=";
		if (a.rfind(pfx, 0) != 0) continue;
		const std::string rest = a.substr(std::strlen(pfx));
		// Split on the FIRST ':' — material names carry no colon, dirs may
		// contain anything after.
		const size_t colon = rest.find(':');
		if (colon == std::string::npos) {
			std::fprintf(stderr, "[MAT-IMPORT] bad spec '%s' (want material:dir)\n", a.c_str());
			continue;
		}
		g_specs.push_back({ rest.substr(0, colon), expandTilde(rest.substr(colon + 1)) });
	}
}

bool MaterialImport_Active() { return !g_specs.empty(); }

void MaterialImport_Apply(Scene *sc, const char *sceneName) {
	if (g_specs.empty() || !sc) return;
	// Materials that gained a tangent-using map (normal/height) this call —
	// their meshes need per-vertex tangents recomputed (see the recompute pass
	// at the end). MaterialImport_Apply runs AFTER Preprocess_Scene, which is
	// where Compute_Vertex_Tangents normally runs; a material that had no
	// normal/height map at Preprocess time has zero tangents, so the kernel's
	// TBN (new_N = T·nmX + B·nmY + N·nmZ) degenerates to N·nmZ and goes black
	// at steep-relief texels. Recomputing tangents now fixes it.
	std::vector<Material*> needTangent;
	for (const ImportSpec &spec : g_specs) {
		// Every material drawing this surface: exact name + "::mirUV" handedness
		// clones (some surfaces render ONLY through their clone — see
		// MaterialImport_ApplyMapFile). Maps are loaded once and shared.
		std::vector<Material *> mats;
		for (Material *m = MatLib; m; m = m->Next)
			if (m->RelScene == sc && m->Name &&
			    (spec.matName == m->Name || rev::Editor_BaseSurfName(m->Name) == spec.matName))
				mats.push_back(m);
		Material *M = mats.empty() ? nullptr : mats[0];
		if (!M) {
			// Help the user: a material name that isn't in this scene is usually
			// a typo or wrong scene. List what IS available.
			std::fprintf(stderr, "[MAT-IMPORT] %s: material '%s' not found. Available:",
			             sceneName ? sceneName : "?", spec.matName.c_str());
			for (Material *m = MatLib; m; m = m->Next)
				if (m->RelScene == sc && m->Name) std::fprintf(stderr, " '%s'", m->Name);
			std::fprintf(stderr, "\n");
			continue;
		}
		// Scan the directory and bucket files by detected role (first match wins).
		std::string albedo, normal, height, rough, ao, metallic;
		DIR *d = opendir(spec.dir.c_str());
		if (!d) { std::fprintf(stderr, "[MAT-IMPORT] cannot open dir '%s'\n", spec.dir.c_str()); continue; }
		for (struct dirent *e; (e = readdir(d)); ) {
			const std::string fn = e->d_name;
			if (fn == "." || fn == ".." || !hasImageExt(fn)) continue;
			const std::string full = spec.dir + "/" + fn;
			const std::string stem = stemLower(fn);
			std::string *slot = nullptr;
			switch (classify(stem)) {
				case Role::Albedo:    slot = &albedo;   break;
				case Role::Normal:    slot = &normal;   break;
				case Role::Height:    slot = &height;   break;
				case Role::Roughness: slot = &rough;    break;
				case Role::Ao:        slot = &ao;       break;
				case Role::Metallic:  slot = &metallic; break;
				default: continue;
			}
			if (slot->empty()) *slot = full;   // first match wins
		}
		closedir(d);

		std::fprintf(stderr, "[MAT-IMPORT] %s: material '%s' <- %s\n",
		             sceneName ? sceneName : "?", spec.matName.c_str(), spec.dir.c_str());

		if (!albedo.empty()) {
			if (Texture *t = loadRoleMapCached(albedo, "albedo", false, 0, 0)) {
				for (Material *m : mats) m->Txtr = t;
				std::fprintf(stderr, "    albedo    %s (%dx%d)\n", albedo.c_str(), t->SizeX, t->SizeY);
			}
		} else {
			std::fprintf(stderr, "    (no albedo found — material keeps its existing texture)\n");
		}
		// Aux maps must match the albedo's texel layout (the kernel samples them
		// with the albedo-computed swizzled index) — resample to its dims.
		int aw = 0, ah = 0;
		if (M->Txtr) { aw = M->Txtr->SizeX; ah = M->Txtr->SizeY; }
		if (!normal.empty()) {
			const bool srcOGL = normalSourceIsOGL(stemLower(normal));
			bool flip = (srcOGL != kEngineExpectsOGL);
			if (g_forceFlipNormal) flip = !flip;
			if (Texture *t = loadRoleMapCached(normal, "normal", flip, aw, ah)) {
				for (Material *m : mats) m->NormalMap = t;
				std::fprintf(stderr, "    normal    %s (src=%s, flipG=%d%s)\n", normal.c_str(),
				             srcOGL ? "OGL" : "DX", flip, g_forceFlipNormal ? ", forced" : "");
			}
		}
		if (!rough.empty()) {
			if (Texture *t = loadRoleMapCached(rough, "roughness", false, aw, ah)) {
				for (Material *m : mats) m->RoughnessMap = t;
				std::fprintf(stderr, "    roughness %s (%s)\n", rough.c_str(), t->BPP == 8 ? "8-bit" : "32-bit");
				// A roughness map implies a specular response, but many FLD
				// materials ship Specular=0 — seed DIELECTRIC scalars from the
				// map so the response reads matte-correct (see the helper; the
				// old 0.5/32 seed made every matte target shiny).
				seedDielectricSpecular(M, t, "CLI dir-scan import");
			}
		}
		if (!height.empty()) {
			if (Texture *t = loadRoleMapCached(height, "height", false, aw, ah)) {
				for (Material *m : mats) m->HeightMap = t;
				std::fprintf(stderr, "    height    %s (%s)%s\n", height.c_str(), t->BPP == 8 ? "8-bit" : "32-bit",
				             fds::FeatureFlags::parallax() ? "" : "  [--parallax off: loaded but inactive]");
			}
		}
		if (!ao.empty()) {
			if (Texture *t = loadRoleMapCached(ao, "ao", false, aw, ah)) {
				for (Material *m : mats) m->AoMap = t;
				std::fprintf(stderr, "    ao        %s (%s, separate AoMap)\n", ao.c_str(), t->BPP == 8 ? "8-bit" : "32-bit");
			}
		}
		if (!metallic.empty()) {
			if (Texture *t = loadRoleMapCached(metallic, "metallic", false, aw, ah)) {
				for (Material *m : mats) m->MetallicMap = t;
				// A metal without env reflections renders as a black hole
				// (metalness kills diffuse; the env term needs --env_refl).
				// Auto-default BOTH flags so the import visibly works out of
				// the box — setDefault never overrides an explicit
				// --no-env_refl / --no-env_bake_fix.
				fds::FeatureFlags::setDefault(fds::FeatureFlags::BoolId::env_refl, true);
				fds::FeatureFlags::setDefault(fds::FeatureFlags::BoolId::env_bake_fix, true);
				std::fprintf(stderr, "    metallic  %s (%s — kills diffuse, tints spec+env by albedo; "
				             "env_refl %s, env_bake_fix %s)\n",
				             metallic.c_str(), t->BPP == 8 ? "8-bit" : "32-bit",
				             fds::FeatureFlags::env_refl() ? "on" : "OFF (user override)",
				             fds::FeatureFlags::env_bake_fix() ? "on" : "OFF (user override)");
			}
		}

		if (M->NormalMap || M->HeightMap)
			for (Material *m : mats) needTangent.push_back(m);
	}

	// Recompute per-vertex tangents for any mesh using a material that just
	// gained a normal/height map (Preprocess already ran, so those meshes have
	// zero tangents otherwise → black TBN). Cheap, init-time. NOTE: this does
	// NOT do the mirrored-UV handedness split (greets' FixNormalMapSeam) — if an
	// imported material lands on faces with negative-determinant UVs, those need
	// a handedness=-1 clone too; revisit if a relief seam appears.
	if (!needTangent.empty()) {
		for (TriMesh *T = sc->TriMeshHead; T; T = T->Next) {
			bool uses = false;
			for (int32_t i = 0; i < T->FIndex && !uses; ++i)
				for (Material *m : needTangent)
					if (T->Faces[i].Txtr == m) { uses = true; break; }
			if (uses) {
				Compute_Vertex_Tangents(T);
				std::fprintf(stderr, "[MAT-IMPORT] recomputed tangents for a mesh (%d faces)\n", T->FIndex);
			}
		}
	}
}

// Normal-G flip parity per texture (0 = as loaded from file). Keyed by
// texture (shared across a surface's material clones) so a flip lands once.
static std::map<Texture*, int> g_nmapFlipParity;

int MaterialImport_GetNormalFlip(const Material *M) {
	if (!M || !M->NormalMap) return 0;
	auto it = g_nmapFlipParity.find(M->NormalMap);
	return it == g_nmapFlipParity.end() ? 0 : it->second;
}

bool MaterialImport_SetSurfaceProp(Scene *sc, const char *surface,
                                   const char *prop, float value) {
	if (!sc || !surface || !prop) return false;
	// envBakeRes arrives as any number (sidecar strtof / editor select) but
	// the env-store mip chain + shift-indexed samplers need a power of two in
	// 64..1024 — sanitize ONCE up front (clamp + round down to a pow2, same
	// rule as EnvBake's envBakeResOverride) so every matching material clone
	// below gets the validated value. <= 0 stays 0 = unset (global chain).
	if (!std::strcmp(prop, "envBakeRes") && value > 0.0f) {
		const int want = (int)value;
		const int clamped = want < 64 ? 64 : (want > 1024 ? 1024 : want);
		int p2 = 64;
		while (p2 * 2 <= clamped) p2 <<= 1;
		if (p2 != want)
			std::fprintf(stderr, "[MAT-IMPORT] '%s' envBakeRes=%d invalid (want"
			             " a power of two in 64..1024) — using %d\n",
			             surface, want, p2);
		value = (float)p2;
	}
	bool any = false;
	for (Material *M = MatLib; M; M = M->Next) {
		if (M->RelScene != sc) continue;
		if (!M->Name || rev::Editor_BaseSurfName(M->Name) != surface) continue;
		if      (!std::strcmp(prop, "baseR"))        M->BaseCol.R = value;
		else if (!std::strcmp(prop, "baseG"))        M->BaseCol.G = value;
		else if (!std::strcmp(prop, "baseB"))        M->BaseCol.B = value;
		else if (!std::strcmp(prop, "diffuse"))      M->Diffuse = value;
		else if (!std::strcmp(prop, "specular"))     M->Specular = value;
		else if (!std::strcmp(prop, "glossiness"))   M->Glossiness = (unsigned short)(value < 0 ? 0 : value);
		else if (!std::strcmp(prop, "luminosity"))   M->Luminosity = value;
		else if (!std::strcmp(prop, "transparency")) {
			// Mat_Transparent is the ROUTING flag (derived from the value at
			// FLD load): both xpar passes re-check it per frame, so flipping
			// it live moves the faces between the opaque raster and the
			// transparent peel. The blend degree is the value itself (the
			// deferred xpar kernel blends behind*Transparency/100).
			M->Transparency = value;
			if (value > 0.0f) M->Flags |=  Mat_Transparent;
			else              M->Flags &= ~Mat_Transparent;
		}
		else if (!std::strcmp(prop, "reflection"))   M->Reflection = value;
		else if (!std::strcmp(prop, "refractive")) {
			// Screen-space glass refraction OPT-IN (Mat_Refractive). Engine-only
			// per-material flag (no LWO/FLD field) — persisted via the scene
			// sidecar so glass is editor-settable. Consumed by the deferred
			// transparent kernel + the TBR glass scheduler under --glass_refract.
			// >0 marks the surface as refracting glass; 0 clears it.
			if (value > 0.0f) M->Flags |=  Mat_Refractive;
			else              M->Flags &= ~Mat_Refractive;
		}
		// Engine-only per-material dials (no LWO/FLD field — persist via the
		// scene sidecar). Both multiply their global FeatureFlags strength.
		else if (!std::strcmp(prop, "aoStrength"))    M->AoStrength = value;
		else if (!std::strcmp(prop, "parallaxScale")) M->ParallaxScale = value;
		// Per-material specular RESPONSE multiplier (RVSF bit 0x800): scales
		// the deferred kernels' FINAL specular term (analytic + env-specular)
		// after roughness/metal modulation. 1 = authored default (byte-null);
		// clamp negatives (a negative response is nonsense).
		else if (!std::strcmp(prop, "specMul"))       M->SpecMul = value < 0.0f ? 0.0f : value;
		// Per-material glass-refraction IOR (engine-only, sidecar-persisted).
		// 0 = unset -> the kernel falls back to the global glass_refract_ior;
		// >0 = this material's Snell bend + Schlick F0 use this value. Only
		// meaningful on Mat_Refractive surfaces under --glass_refract.
		else if (!std::strcmp(prop, "refractIor"))    M->RefractIor = value < 0.0f ? 0.0f : value;
		// Tri-state env-reflection override (engine-only, sidecar-persisted):
		// -1 = never bake/publish an env probe for this material, 0 = auto
		// (the Reflection>0 || MetallicMap rule), 1 = force-bake. Values
		// arrive as FLOATS here (sidecar lines parse with strtof), so
		// classify by range instead of exact compare.
		else if (!std::strcmp(prop, "envRefl"))
			M->EnvReflMode = value < -0.5f ? int8_t(-1) : value > 0.5f ? int8_t(1) : int8_t(0);
		// Per-surface env-probe bake FACE resolution (engine-only, sidecar-
		// persisted; sanitized to a pow2 in 64..1024 above). 0 = unset -> the
		// global env_bake_res / legacy sizing chain. Read at bake time
		// (EnvBake.cpp); the live-editor path invalidates the scene's probes
		// on set so the next FramePrep re-bakes at the new size.
		else if (!std::strcmp(prop, "envBakeRes"))
			M->EnvBakeRes = value <= 0.0f ? 0 : (int)value;
		// Tri-state procedural-water override (engine-only, sidecar-persisted;
		// same -1/0/1 classification as envRefl). Only meaningful on the
		// scene's water material (SetDeferredWaterMatID); 0 = auto → the
		// global --water_procedural flag decides, so a scene with no sidecar
		// line renders byte-identically.
		else if (!std::strcmp(prop, "waterProcedural"))
			M->WaterProcMode = value < -0.5f ? int8_t(-1) : value > 0.5f ? int8_t(1) : int8_t(0);
		// Authored dynamic-env-reflection flag (ENVDYN Workstream A1). 0/1;
		// persisted via the LWO 'RVSF' sub-chunk (bit 0x400) → FLD, and set
		// live here for the editor's Material-panel checkbox. Marks this
		// material's env probe for the live dynamic-mesh overlay (A2/A3).
		else if (!std::strcmp(prop, "envDynamic"))
			M->EnvDynamic = value > 0.5f ? int8_t(1) : int8_t(0);
		// Per-surface smoothing (normal-averaging) angle. Engine-only, no
		// material field: recorded in the MeshOps registry and consumed when
		// MakeFacesIndependent rebuilds this surface's vertex normals. That
		// runs later at scene init (after the sidecar) — 180 = fully smooth,
		// 0 = faceted. Set live (editor) it registers but needs a scene reload
		// to re-run the normal build (topology is flattened once at init).
		else if (!std::strcmp(prop, "smoothAngle")) MeshOps_SetSurfaceSmoothAngle(surface, value);
		// Albedo tint (engine-only, sidecar-persisted): per-MATERIAL
		// multipliers applied at the deferred texel fetch — NOT a texture
		// mutation, since textures are deduped by filename and shared
		// across DIFFERENT surfaces (MECH_HUL.JPG = hull+canons+legs);
		// mutating pixels bled one surface's tint into the others.
		// Lossless/reversible; forward-path surfaces don't see it.
		else if (!std::strcmp(prop, "tintR")) M->TintR = value < 0.0f ? 0.0f : value;
		else if (!std::strcmp(prop, "tintG")) M->TintG = value < 0.0f ? 0.0f : value;
		else if (!std::strcmp(prop, "tintB")) M->TintB = value < 0.0f ? 0.0f : value;
		else if (!std::strcmp(prop, "normalFlip")) {
			// Green-channel convention toggle (OGL ↔ DX), value = desired
			// parity (0/1 vs the file as loaded). The flip mutates the
			// TEXTURE, which is shared across a surface's material clones —
			// parity is tracked per texture so the clone loop flips it
			// exactly once, and the sidecar's init-time apply and later
			// editor toggles stay consistent (GetNormalFlip reads it back).
			const int want = int(value) & 1;
			if (M->NormalMap && g_nmapFlipParity[M->NormalMap] != want) {
				FlipNormalMapG(M->NormalMap);
				g_nmapFlipParity[M->NormalMap] = want;
			}
		}
		else return false;   // unknown prop
		any = true;
	}
	return any;
}

// ── Object-level overrides (the editor scale knob) ──────────────────────────
// Set the per-object uniform scale multiplier on every scene object whose
// chunk-collapsed name (Editor_ChunkBaseObjName — 'Piramid.lwo:c17' →
// 'Piramid.lwo') matches `objName`. Multi-instance objects (8 × taxi.lwo) are
// separate Objects sharing the name — all of them are set. Subtree semantics
// come free: Animate_Objects composes the parent's (scaled) rotation matrix
// into children, so scaling a model's ROOT object ('mech  null',
// 'tra_frnt.lwo') scales the whole assembly around the root's pivot.
// Returns the number of LIVE meshes set; static-baked (Tri_Possessed) meshes
// are still stamped but never re-run Animate_Objects, so they're reported on
// stderr instead of counted (greets' chunked room can't scale live).
int ObjectImport_SetObjectScale(Scene *sc, const char *objName, float scale) {
	if (!sc || !objName || !*objName) return 0;
	if (scale <= 0.0f) scale = 1.0f;
	const std::string want = objName;
	int applied = 0, possessed = 0;
	for (Object *Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
		if (Obj->Type != Obj_TriMesh || !Obj->Data || !Obj->Name) continue;
		if (rev::Editor_ChunkBaseObjName(Obj->Name) != want) continue;
		TriMesh *T = (TriMesh *)Obj->Data;
		T->EditorScale = scale;
		if (T->Flags & Tri_Possessed) ++possessed;
		else ++applied;
	}
	if (possessed)
		std::fprintf(stderr, "[OBJ-IMPORT] scale '%s'=%.3g: %d static-baked "
		             "(Tri_Possessed) mesh(es) skip Animate_Objects — live scale "
		             "can't reach them\n", objName, scale, possessed);
	return applied;
}

// Read-back for the editor's objects JSON: the object's current scale
// multiplier (first matching mesh; 0-sentinel resolved to 1.0).
float ObjectImport_GetObjectScale(Scene *sc, const char *objName) {
	if (!sc || !objName || !*objName) return 1.0f;
	const std::string want = objName;
	for (Object *Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
		if (Obj->Type != Obj_TriMesh || !Obj->Data || !Obj->Name) continue;
		if (rev::Editor_ChunkBaseObjName(Obj->Name) != want) continue;
		const TriMesh *T = (const TriMesh *)Obj->Data;
		return T->EditorScale > 0.0f ? T->EditorScale : 1.0f;
	}
	return 1.0f;
}

// True if a file exists + is readable (relative to the demo CWD = Runtime/).
static bool fileExists(const std::string &path) {
	if (FILE *f = std::fopen(path.c_str(), "rb")) { std::fclose(f); return true; }
	return false;
}

void MaterialImport_ApplyRevMaps(Scene *sc, const char *sceneName) {
	if (!sc) return;
	// Directory-per-set (§1e): a material names ONE texture SET; the engine
	// resolves TEXTURES/PBR/<set>/<role>.png and loads whichever roles exist.
	// albedo FIRST (it sets the texel layout aux maps resample to, and dropIf
	// keys off it); the rest in the retired sidecar's stable alphabetical order
	// so a scene that carried per-file sidecar paths reproduces byte-identically.
	static const char *const kRoles[] = {
		"albedo", "ao", "height", "metallic", "normal", "roughness" };
	const int n = FldRevMapCount();
	int applied = 0, missing = 0;
	for (int i = 0; i < n; ++i) {
		const RevMapAssignment *e = FldRevMapAt(i);
		if (!e || e->scene != sc || !e->matName || !e->set || !*e->set) continue;
		const std::string dir = std::string("TEXTURES/PBR/") + e->set;
		if (!fileExists(dir + "/albedo.png") && !fileExists(dir + "/normal.png")
		 && !fileExists(dir + "/height.png") && !fileExists(dir + "/roughness.png")
		 && !fileExists(dir + "/metallic.png") && !fileExists(dir + "/ao.png")) {
			std::fprintf(stderr, "[MAT-REVMAP] %s: '%s' -> set '%s' has NO role "
			             "files under %s — nothing applied\n",
			             sceneName ? sceneName : "?", e->matName, e->set, dir.c_str());
			++missing;
			continue;
		}
		for (const char *role : kRoles) {
			const std::string path = dir + "/" + role + ".png";
			if (fileExists(path)) {
				MaterialImport_ApplyMapFile(sc, e->matName, role, path.c_str());
				++applied;
			}
		}
		// normalFlip AFTER the normal map (it mutates the assigned NormalMap).
		if (e->normalFlip >= 0)
			MaterialImport_SetSurfaceProp(sc, e->matName, "normalFlip",
			                              (float)e->normalFlip);
	}
	if (applied || missing)
		std::fprintf(stderr, "[MAT-REVMAP] %s: %d LWO/FLD-authored map(s) applied"
		             "%s\n", sceneName ? sceneName : "?", applied,
		             missing ? " (some sets MISSING, see above)" : "");
}

const char *MaterialImport_ClassifyRole(const char *filename) {
	if (!filename || !hasImageExt(filename)) return "";
	switch (classify(stemLower(filename))) {
		case Role::Albedo:    return "albedo";
		case Role::Normal:    return "normal";
		case Role::Height:    return "height";
		case Role::Roughness: return "roughness";
		case Role::Ao:        return "ao";
		case Role::Metallic:  return "metallic";
		default:              return "";   // Skip / None — nothing to apply
	}
}

// Original-slot stash for the editor's "reset map": records a (material, role)
// slot's texture the FIRST time an import overrides it — whether from a live
// editor upload or the sidecar apply at scene init — so
// MaterialImport_ClearSurfaceMap can restore the authored default. emplace()
// keeps the first (authored) value across repeated re-imports of the slot.
static std::map<std::pair<Material *, std::string>, Texture *> s_mapOrig;
static void stashOrigMap(Material *M, const char *role, Texture *cur) {
	s_mapOrig.emplace(std::make_pair(M, std::string(role)), cur);
}
// Pre-import per-vertex tangents, stashed the first time an import triggers
// Compute_Vertex_Tangents on a mesh. A surface with no authored normal/height
// map ships with the loader's tangents (zeros); the recompute is one-way, and
// the glass-refraction path reads the tangent frame even without a map — so a
// texel-exact "reset map" must restore these, not recompute.
static std::map<TriMesh *, std::vector<Vector>> s_tanOrig;
static void stashOrigTangents(TriMesh *T) {
	auto &orig = s_tanOrig[T];
	if (!orig.empty() || T->VIndex <= 0) return;   // first import wins
	orig.resize(T->VIndex);
	for (int32_t v = 0; v < T->VIndex; ++v) orig[v] = T->Verts[v].Tangent;
}

bool MaterialImport_ApplyMapFile(Scene *sc, const char *matName,
                                 const char *role, const char *path) {
	if (!sc || !matName || !role || !path) return false;
	// Collect EVERY material drawing this surface: the exact name plus any
	// "::mirUV" handedness clones. Some surfaces render only through their
	// clone (greets floor), so assigning the map to the base material alone
	// changes nothing on screen.
	std::vector<Material *> mats;
	for (Material *M = MatLib; M; M = M->Next)
		if (M->RelScene == sc && M->Name &&
		    (matName == std::string(M->Name) || rev::Editor_BaseSurfName(M->Name) == matName))
			mats.push_back(M);
	if (mats.empty()) { std::fprintf(stderr, "[MAT-IMPORT] '%s' not in scene\n", matName); return false; }

	// Load/convert once, share the Texture* across all matching materials.
	// Aux maps are resampled to the material's CURRENT albedo dims (the kernel
	// samples them with the albedo-layout texel index — see loadTiled).
	const std::string r = role;
	int aw = 0, ah = 0;
	if (mats[0]->Txtr) { aw = mats[0]->Txtr->SizeX; ah = mats[0]->Txtr->SizeY; }
	bool tangentMap = false, ok = false;
	if (r == "albedo") {
		if (Texture *t = loadRoleMapCached(path, "albedo", false, 0, 0)) {
			for (Material *M : mats) {
				stashOrigMap(M, "albedo", M->Txtr);
				M->Txtr = t;
				// New albedo = new texel layout. Aux maps sized for the OLD
				// layout would read scrambled — drop them (re-import from the
				// pack restores them at the matching size; the pack flow
				// imports albedo first, so this is self-healing).
				auto dropIf = [&](Texture *&slot, const char *what) {
					if (slot && (slot->SizeX != t->SizeX || slot->SizeY != t->SizeY)) {
						std::fprintf(stderr, "    [drop] stale %s map (%dx%d vs new albedo %dx%d)\n",
						             what, slot->SizeX, slot->SizeY, t->SizeX, t->SizeY);
						stashOrigMap(M, what, slot);
						slot = nullptr;
					}
				};
				dropIf(M->NormalMap, "normal");
				dropIf(M->HeightMap, "height");
				dropIf(M->RoughnessMap, "roughness");
				dropIf(M->AoMap, "ao");
				dropIf(M->MetallicMap, "metallic");
			}
			ok = true;
		}
	} else if (r == "normal") {
		// No filename convention to sniff here; default to the engine (OGL)
		// convention, honoring the global --material-import-flip-normal override.
		if (Texture *t = loadRoleMapCached(path, "normal", g_forceFlipNormal, aw, ah)) {
			for (Material *M : mats) { stashOrigMap(M, "normal", M->NormalMap); M->NormalMap = t; }
			tangentMap = true; ok = true;
		}
	} else if (r == "height") {
		if (Texture *t = loadRoleMapCached(path, "height", false, aw, ah)) { for (Material *M : mats) { stashOrigMap(M, "height", M->HeightMap); M->HeightMap = t; } tangentMap = true; ok = true; }
	} else if (r == "roughness") {
		if (Texture *t = loadRoleMapCached(path, "roughness", false, aw, ah)) {
			for (Material *M : mats) {
				stashOrigMap(M, "roughness", M->RoughnessMap);
				M->RoughnessMap = t;
				// Same DIELECTRIC seeding as the CLI dir-scan path (shared
				// helper): only when the author left Specular at 0.
				seedDielectricSpecular(M, t, "editor/RVSM apply");
			}
			ok = true;
		}
	} else if (r == "ao") {
		if (Texture *t = loadRoleMapCached(path, "ao", false, aw, ah)) { for (Material *M : mats) { stashOrigMap(M, "ao", M->AoMap); M->AoMap = t; } ok = true; }
	} else if (r == "metallic") {
		if (Texture *t = loadRoleMapCached(path, "metallic", false, aw, ah)) {
			for (Material *M : mats) { stashOrigMap(M, "metallic", M->MetallicMap); M->MetallicMap = t; }
			// Metal without env reflections = black hole (diffuse killed,
			// env term needs --env_refl). Auto-default the reflection flags
			// so a metallic import — editor upload OR sidecar line at scene
			// init — visibly works; explicit --no-* still wins.
			fds::FeatureFlags::setDefault(fds::FeatureFlags::BoolId::env_refl, true);
			fds::FeatureFlags::setDefault(fds::FeatureFlags::BoolId::env_bake_fix, true);
			ok = true;
		}
	} else {
		std::fprintf(stderr, "[MAT-IMPORT] unknown role '%s'\n", role); return false;
	}
	std::fprintf(stderr, "[MAT-IMPORT] '%s' <- %s map %s (%s, %zu material(s))\n",
	             matName, role, path, ok ? "ok" : "FAILED", mats.size());
	// A material that gained a normal/height map needs its meshes' tangents
	// recomputed (same reason as the CLI path).
	if (ok && tangentMap) {
		for (TriMesh *T = sc->TriMeshHead; T; T = T->Next) {
			bool uses = false;
			for (int32_t i = 0; i < T->FIndex && !uses; ++i)
				for (Material *M : mats)
					if (T->Faces[i].Txtr == M) { uses = true; break; }
			if (uses) { stashOrigTangents(T); Compute_Vertex_Tangents(T); }
		}
	}
	return ok;
}

// Editor "reset map": restore a surface's (role) texture slot to what it held
// BEFORE the first import override this run — the authored default, whether
// the override came from a live editor upload or the sidecar apply at scene
// init (the stash records the pre-sidecar value). A surface that was never
// overridden is a successful no-op ("already default"). The old override
// Texture is intentionally leaked, same as re-imports — Texture blocks aren't
// refcounted and a few MB until scene teardown beats a dangling shared ptr
// (mirUV clones share the Texture*).
bool MaterialImport_ClearSurfaceMap(Scene *sc, const char *matName,
                                    const char *role) {
	if (!sc || !matName || !role) return false;
	const std::string r = role;
	auto slotOf = [&r](Material *M) -> Texture ** {
		if (r == "albedo")    return &M->Txtr;
		if (r == "normal")    return &M->NormalMap;
		if (r == "height")    return &M->HeightMap;
		if (r == "roughness") return &M->RoughnessMap;
		if (r == "ao")        return &M->AoMap;
		if (r == "metallic")  return &M->MetallicMap;
		return nullptr;
	};
	// Same material collection as ApplyMapFile: exact name + handedness clones.
	std::vector<Material *> mats;
	for (Material *M = MatLib; M; M = M->Next)
		if (M->RelScene == sc && M->Name &&
		    (matName == std::string(M->Name) || rev::Editor_BaseSurfName(M->Name) == matName))
			mats.push_back(M);
	if (mats.empty()) { std::fprintf(stderr, "[MAT-IMPORT] reset: '%s' not in scene\n", matName); return false; }
	if (!slotOf(mats[0])) { std::fprintf(stderr, "[MAT-IMPORT] reset: unknown role '%s'\n", role); return false; }
	int restored = 0;
	for (Material *M : mats) {
		auto it = s_mapOrig.find(std::make_pair(M, r));
		if (it == s_mapOrig.end()) continue;   // never overridden — already default
		*slotOf(M) = it->second;
		s_mapOrig.erase(it);
		++restored;
	}
	std::fprintf(stderr, "[MAT-IMPORT] '%s' reset %s map to default (%d of %zu material(s) had an override)\n",
	             matName, role, restored, mats.size());
	// Undo the tangent side of the import: if no material on the mesh still
	// holds a LIVE normal/height override (s_mapOrig entries live until
	// cleared), restore the stashed pre-import tangents — texel-exact revert
	// (the glass-refraction frame reads tangents even without a map). If some
	// other surface on the mesh still has an override, recompute instead.
	if (restored && (r == "normal" || r == "height")) {
		for (TriMesh *T = sc->TriMeshHead; T; T = T->Next) {
			bool uses = false;
			for (int32_t i = 0; i < T->FIndex && !uses; ++i)
				for (Material *M : mats)
					if (T->Faces[i].Txtr == M) { uses = true; break; }
			if (!uses) continue;
			bool otherOverride = false;
			for (int32_t i = 0; i < T->FIndex && !otherOverride; ++i) {
				Material *FM = T->Faces[i].Txtr;
				if (!FM) continue;
				otherOverride = s_mapOrig.count(std::make_pair(FM, std::string("normal"))) != 0 ||
				                s_mapOrig.count(std::make_pair(FM, std::string("height"))) != 0;
			}
			auto ti = s_tanOrig.find(T);
			if (!otherOverride && ti != s_tanOrig.end() &&
			    (int32_t)ti->second.size() == T->VIndex) {
				for (int32_t v = 0; v < T->VIndex; ++v)
					T->Verts[v].Tangent = ti->second[v];
				s_tanOrig.erase(ti);
			} else {
				Compute_Vertex_Tangents(T);
			}
		}
	}
	return true;
}

} // namespace fds
