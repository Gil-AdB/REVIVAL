#include "MaterialImport.h"

#include "MaterialEditor.h"          // rev::Editor_BaseSurfName (::mirUV collapse)
#include "MeshOps.h"                 // MakeHeight8, MakeNormal16, BakeNormalMapFromDiffuse
#include <Base/FDS_VARS.H>           // MatLib
#include <Base/FDS_DECS.H>           // Load_Texture, BPPConvert_Texture, Generate_Mipmaps
#include <Base/FDS_DEFS.H>           // DEFAULT_BLOCKSIZEX/Y, Txtr_Tiled, Mat_AoInAlpha
#include <Base/Texture.h>
#include <Base/Material.h>
#include <Base/Omni.h>               // FlareScale (sidecar light: lines)
#include <Base/Scene.h>
#include <Base/FeatureFlags.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
			if (Texture *t = loadTiled(albedo, false)) {
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
			if (Texture *t = loadTiled(normal, flip, aw, ah)) {
				if (fds::FeatureFlags::nmap_16bit()) { if (Texture *t16 = MakeNormal16(t)) t = t16; }
				for (Material *m : mats) m->NormalMap = t;
				std::fprintf(stderr, "    normal    %s (src=%s, flipG=%d%s)\n", normal.c_str(),
				             srcOGL ? "OGL" : "DX", flip, g_forceFlipNormal ? ", forced" : "");
			}
		}
		if (!rough.empty()) {
			if (Texture *r32 = loadTiled(rough, false, aw, ah)) {
				Texture *r8 = MakeHeight8(r32);
				for (Material *m : mats) m->RoughnessMap = r8 ? r8 : r32;
				std::fprintf(stderr, "    roughness %s (%s)\n", rough.c_str(), r8 ? "8-bit" : "32-bit");
				// A roughness map implies the surface is meant to be specular, but
				// many FLD materials (e.g. greets 'momy') ship Specular=0 → the
				// roughness map would modulate a highlight that never appears. Give
				// the material a sensible base Specular/Glossiness so the map shows.
				// Only when the author left them at 0 (don't stomp a tuned value).
				if (M->Specular <= 0.0f) {
					M->Specular = 0.5f;
					if (M->Glossiness == 0) M->Glossiness = 32;
					std::fprintf(stderr, "    [spec] roughness map present + Specular was 0 -> "
					             "default Specular=0.5 Glossiness=%u (roughness map modulates it)\n",
					             M->Glossiness);
				}
			}
		}
		if (!height.empty()) {
			if (Texture *h32 = loadTiled(height, false, aw, ah)) {
				Texture *h8 = MakeHeight8(h32);
				for (Material *m : mats) m->HeightMap = h8 ? h8 : h32;
				std::fprintf(stderr, "    height    %s (%s)%s\n", height.c_str(), h8 ? "8-bit" : "32-bit",
				             fds::FeatureFlags::parallax() ? "" : "  [--parallax off: loaded but inactive]");
			}
		}
		if (!ao.empty()) {
			if (Texture *a32 = loadTiled(ao, false, aw, ah)) {
				Texture *a8 = MakeHeight8(a32);
				for (Material *m : mats) m->AoMap = a8 ? a8 : a32;
				std::fprintf(stderr, "    ao        %s (%s, separate AoMap)\n", ao.c_str(), a8 ? "8-bit" : "32-bit");
			}
		}
		if (!metallic.empty())
			std::fprintf(stderr, "    metallic  %s  [IGNORED — deferred path is diffuse+spec, no metallic workflow]\n",
			             metallic.c_str());

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

bool MaterialImport_SetSurfaceProp(Scene *sc, const char *surface,
                                   const char *prop, float value) {
	if (!sc || !surface || !prop) return false;
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
		else return false;   // unknown prop
		any = true;
	}
	return any;
}

static bool sidecarIsMapRole(const char *role) {
	return !std::strcmp(role, "albedo") || !std::strcmp(role, "normal")
	    || !std::strcmp(role, "height") || !std::strcmp(role, "roughness")
	    || !std::strcmp(role, "ao");
}

// Sidecar light lines: "light:<i>|<key>|<value>" — engine-only per-light
// extensions with no LWS/FLD field (currently flareScale). <i> indexes the
// scene-authored omnis in file order, the SAME mapping the editor and the
// LWS/FLD light patchers use.
static bool sidecarSetLightProp(Scene *sc, int index, const char *key, float value) {
	int i = 0;
	for (Omni *O = sc->OmniHead; O; O = O->Next) {
		if (!(O->Flags & Omni_SceneAuthored)) continue;
		if (i++ != index) continue;
		if (!std::strcmp(key, "flareScale")) { O->FlareScale = value; return true; }
		return false;   // unknown per-light sidecar key
	}
	return false;       // index out of range
}

void MaterialImport_ApplySidecar(Scene *sc, const char *path) {
	if (!sc || !path) return;
	FILE *f = std::fopen(path, "r");
	if (!f) return;   // no sidecar for this scene — fine
	char line[512];
	int applied = 0, failed = 0;
	while (std::fgets(line, sizeof line, f)) {
		// strip newline / CR; skip blanks + comments
		line[std::strcspn(line, "\r\n")] = 0;
		if (!line[0] || line[0] == '#') continue;
		char *sep1 = std::strchr(line, '|');
		char *sep2 = sep1 ? std::strchr(sep1 + 1, '|') : nullptr;
		if (!sep1 || !sep2) {
			std::fprintf(stderr, "[MAT-SIDECAR] bad line (want surface|key|value): %s\n", line);
			continue;
		}
		*sep1 = 0;
		*sep2 = 0;
		const char *surface = line, *key = sep1 + 1, *value = sep2 + 1;
		bool ok;
		if (!std::strncmp(surface, "light:", 6)) {
			ok = sidecarSetLightProp(sc, std::atoi(surface + 6), key,
			                         std::strtof(value, nullptr));
			if (!ok) std::fprintf(stderr, "[MAT-SIDECAR] %s.%s: no such light / key\n",
			                      surface, key);
		} else if (sidecarIsMapRole(key)) {
			ok = MaterialImport_ApplyMapFile(sc, surface, key, value);
		} else {
			ok = MaterialImport_SetSurfaceProp(sc, surface, key, std::strtof(value, nullptr));
			if (!ok) std::fprintf(stderr, "[MAT-SIDECAR] '%s'.%s: no match / unknown prop\n",
			                      surface, key);
		}
		if (ok) ++applied; else ++failed;
	}
	std::fclose(f);
	std::fprintf(stderr, "[MAT-SIDECAR] %s: %d entrie(s) applied%s\n",
	             path, applied, failed ? " (some FAILED, see above)" : "");
}

const char *MaterialImport_ClassifyRole(const char *filename) {
	if (!filename || !hasImageExt(filename)) return "";
	switch (classify(stemLower(filename))) {
		case Role::Albedo:    return "albedo";
		case Role::Normal:    return "normal";
		case Role::Height:    return "height";
		case Role::Roughness: return "roughness";
		case Role::Ao:        return "ao";
		default:              return "";   // Skip / Metallic / None — nothing to apply
	}
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
		if (Texture *t = loadTiled(path, false)) {
			for (Material *M : mats) {
				M->Txtr = t;
				// New albedo = new texel layout. Aux maps sized for the OLD
				// layout would read scrambled — drop them (re-import from the
				// pack restores them at the matching size; the pack flow
				// imports albedo first, so this is self-healing).
				auto dropIf = [&](Texture *&slot, const char *what) {
					if (slot && (slot->SizeX != t->SizeX || slot->SizeY != t->SizeY)) {
						std::fprintf(stderr, "    [drop] stale %s map (%dx%d vs new albedo %dx%d)\n",
						             what, slot->SizeX, slot->SizeY, t->SizeX, t->SizeY);
						slot = nullptr;
					}
				};
				dropIf(M->NormalMap, "normal");
				dropIf(M->HeightMap, "height");
				dropIf(M->RoughnessMap, "roughness");
				dropIf(M->AoMap, "ao");
			}
			ok = true;
		}
	} else if (r == "normal") {
		// No filename convention to sniff here; default to the engine (OGL)
		// convention, honoring the global --material-import-flip-normal override.
		if (Texture *t = loadTiled(path, g_forceFlipNormal, aw, ah)) {
			if (fds::FeatureFlags::nmap_16bit()) { if (Texture *t16 = MakeNormal16(t)) t = t16; }
			for (Material *M : mats) M->NormalMap = t;
			tangentMap = true; ok = true;
		}
	} else if (r == "height") {
		if (Texture *h32 = loadTiled(path, false, aw, ah)) { Texture *h8 = MakeHeight8(h32); for (Material *M : mats) M->HeightMap = h8 ? h8 : h32; tangentMap = true; ok = true; }
	} else if (r == "roughness") {
		if (Texture *r32 = loadTiled(path, false, aw, ah)) { Texture *r8 = MakeHeight8(r32); for (Material *M : mats) M->RoughnessMap = r8 ? r8 : r32; ok = true; }
	} else if (r == "ao") {
		if (Texture *a32 = loadTiled(path, false, aw, ah)) { Texture *a8 = MakeHeight8(a32); for (Material *M : mats) M->AoMap = a8 ? a8 : a32; ok = true; }
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
			if (uses) Compute_Vertex_Tangents(T);
		}
	}
	return ok;
}

} // namespace fds
