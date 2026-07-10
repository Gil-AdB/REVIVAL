#include "MaterialEditor.h"
#include "MaterialImport.h"   // MaterialImport_ApplyMapFile
#include "MeshOps.h"          // MeshOps_GetSurfaceSmoothAngle (live smoothing override)

#include <Base/FDS_VARS.H>   // MatLib, CurScene
#include <Base/Material.h>
#include <Base/Texture.h>
#include <Base/FDS_DECS.H>   // Scene_GetMatTable, MatTable
#include <Base/FDS_DEFS.H>   // Omni_SceneAuthored
#include <Base/Scene.h>      // Scene::Ambient, OmniHead
#include <Base/Omni.h>
#include <Base/FeatureFlags.h>
#include <FILLERS/Mekalele.h> // meka::GBuffer, g_gbuffer (matID G-buffer plane)
#include <RENDER/EnvBake.h>   // EnvReflection_Invalidate (rebake button)

#include <Base/TriMesh.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

// PREPROC.CPP — recompute per-vertex tangents from current Faces + maps.
void Compute_Vertex_Tangents(TriMesh *T);

namespace rev {

// See MaterialEditor.h — some surfaces render only through their "::mirUV"
// handedness clone, so all name-keyed ops collapse to the base name.
std::string Editor_BaseSurfName(const char* n)
{
	std::string s = n ? n : "";
	static const std::string suf = "::mirUV";
	if (s.size() > suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0)
		s.resize(s.size() - suf.size());
	return s;
}

static void jsonEscape(std::string& out, const char* s)
{
	if (!s) return;
	for (; *s; ++s) {
		char c = *s;
		if (c == '"' || c == '\\') { out += '\\'; out += c; }
		else if (c == '\n')        { out += "\\n"; }
		else if ((unsigned char)c < 0x20) { /* drop other control chars */ }
		else                       { out += c; }
	}
}

static void appendNum(std::string& out, const char* key, double v)
{
	char buf[80];
	std::snprintf(buf, sizeof buf, "\"%s\":%.6g", key, v);
	out += buf;
}

#ifndef __EMSCRIPTEN__
static void editorRunPickTestHook();   // defined below the pick core
#endif

std::string Editor_GetSurfacesJSON()
{
#ifndef __EMSCRIPTEN__
	// PICK_TEST native validation (see the definition below): the pick needs a
	// RENDERED frame (live ZPage16 / G-buffer / camera), and Snapshot.cpp's only
	// post-tick call into this file is the DUMP_SURFACES hook → this function.
	// So the check rides here: no-op unless PICK_TEST is set; fires once.
	editorRunPickTestHook();
#endif
	std::string out = "[";
	std::unordered_set<std::string> seen;
	bool first = true;
	for (Material* M = MatLib; M; M = M->Next) {
		if (M->RelScene != CurScene) continue;
		if (!M->Name) continue;
		std::string base = Editor_BaseSurfName(M->Name);
		if (!seen.insert(base).second) continue;   // de-dup by base name (::mirUV collapsed)

		if (!first) out += ",";
		first = false;
		out += "{\"name\":\"";
		jsonEscape(out, base.c_str());
		out += "\",";
		appendNum(out, "baseR",        M->BaseCol.R);    out += ",";
		appendNum(out, "baseG",        M->BaseCol.G);    out += ",";
		appendNum(out, "baseB",        M->BaseCol.B);    out += ",";
		appendNum(out, "diffuse",      M->Diffuse);      out += ",";
		appendNum(out, "specular",     M->Specular);     out += ",";
		appendNum(out, "glossiness",   M->Glossiness);   out += ",";
		appendNum(out, "luminosity",   M->Luminosity);   out += ",";
		appendNum(out, "transparency", M->Transparency); out += ",";
		appendNum(out, "reflection",   M->Reflection);   out += ",";
		appendNum(out, "aoStrength",   M->AoStrength);   out += ",";
		appendNum(out, "parallaxScale",M->ParallaxScale);out += ",";
		appendNum(out, "tintR",        M->TintR);        out += ",";
		appendNum(out, "tintG",        M->TintG);        out += ",";
		appendNum(out, "tintB",        M->TintB);        out += ",";
		appendNum(out, "normalFlip",   fds::MaterialImport_GetNormalFlip(M)); out += ",";
		// Glass refraction opt-in (Mat_Refractive). Under --glass_refract only
		// surfaces carrying this bit refract the opaque background; the editor
		// checkbox flips it (setProp -> MaterialImport_SetSurfaceProp), persisted
		// via the scene sidecar ('refractive' in SURF_SIDECAR_KEYS).
		appendNum(out, "refractive",   (M->Flags & Mat_Refractive) ? 1 : 0); out += ",";
		// Per-surface smoothing angle (degrees). Show the LIVE sidecar override
		// if one is registered (so it round-trips after Save + reload), else the
		// authored Material::MaxSmoothingAngle (stored in radians). Consumed by
		// MakeFacesIndependent at scene init (see MeshOps.cpp).
		{
			float smoothDeg;
			if (!MeshOps_GetSurfaceSmoothAngle(base.c_str(), smoothDeg))
				smoothDeg = M->MaxSmoothingAngle * (180.0f / 3.14159265358979323846f);
			appendNum(out, "smoothAngle", smoothDeg); out += ",";
		}
		out += M->NormalMap ? "\"hasNormalMap\":1," : "\"hasNormalMap\":0,";
		out += (M->AoMap || (M->Flags & Mat_AoInAlpha)) ? "\"hasAoMap\":1," : "\"hasAoMap\":0,";
		out += M->HeightMap ? "\"hasHeightMap\":1," : "\"hasHeightMap\":0,";
		out += M->RoughnessMap ? "\"hasRoughnessMap\":1," : "\"hasRoughnessMap\":0,";
		out += M->MetallicMap ? "\"hasMetallicMap\":1," : "\"hasMetallicMap\":0,";
		// Current UV mapping (as loaded / as last re-projected).
		{
			int proj = -1;   // -1 = unknown / not an image-mapped surface
			if (M->ColorTexture) {
				if      (!std::strncmp(M->ColorTexture, "Planar", 6))      proj = 0;
				else if (!std::strncmp(M->ColorTexture, "Cylindrical", 11)) proj = 1;
				else if (!std::strncmp(M->ColorTexture, "Spherical", 9))   proj = 2;
				else if (!std::strncmp(M->ColorTexture, "Cubic", 5))       proj = 3;
			}
			appendNum(out, "uvProj",   proj);              out += ",";
			appendNum(out, "uvScaleX", M->TextureSize.x);  out += ",";
			appendNum(out, "uvScaleY", M->TextureSize.y);  out += ",";
			appendNum(out, "uvScaleZ", M->TextureSize.z);  out += ",";
			appendNum(out, "uvAxis",   M->TextureFlags & 7); out += ",";
		}
		appendNum(out, "flags",        (double)M->Flags);out += ",";
		out += "\"texture\":\"";
		jsonEscape(out, (M->Txtr && M->Txtr->FileName) ? M->Txtr->FileName : "");
		out += "\"}";
	}
	out += "]";
	return out;
}

// Objects can't be derived from meshes here: greets' big per-round meshes mix
// room AND mech faces in one TriMesh (verified via the DUMP_MESHES snapshot
// hook), and surface names are reused across models (the room hull is also
// called "hull"). The one well-defined multi-part model is the robot-clone
// naming scheme itself: "X.lwo::surf[_body|_upper]" materials exist precisely
// to give a multi-mesh animated model per-part materials. So the hierarchy is
// name-derived: one parent object per clone-file family (the mech), with a
// child object per file (Hull.lwo, L_leg1.lwo, …), each listing its surfaces.
static void appendSurfArray(std::string& out, const std::set<std::string>& names)
{
	out += "\"surfaces\":[";
	bool first = true;
	for (const std::string& s : names) {
		if (!first) out += ",";
		first = false;
		out += "\"";
		jsonEscape(out, s.c_str());
		out += "\"";
	}
	out += "]";
}

std::string Editor_GetObjectsJSON()
{
	if (!CurScene) return "[]";
	// Clone-file → its surfaces (dedup'd editor base names).
	std::map<std::string, std::set<std::string>> parts;
	std::set<std::string> all;
	for (Material* M = MatLib; M; M = M->Next) {
		if (M->RelScene != CurScene || !M->Name) continue;
		const std::string base = Editor_BaseSurfName(M->Name);
		const size_t sep = base.find("::");
		if (sep == std::string::npos) continue;   // plain surface — not a model part
		parts[base.substr(0, sep)].insert(base);
		all.insert(base);
	}
	if (parts.empty()) return "[]";
	// Face tally per file so the model is named after its heaviest part's stem
	// ("Hull.lwo" → "Hull") — the engine has no authored object names.
	std::map<std::string, long> fileFaces;
	for (TriMesh* T = CurScene->TriMeshHead; T; T = T->Next)
		for (DWord f = 0; f < T->FIndex; ++f) {
			Material* M = T->Faces[f].Txtr;
			if (!M || !M->Name) continue;
			const std::string base = Editor_BaseSurfName(M->Name);
			const size_t sep = base.find("::");
			if (sep != std::string::npos) ++fileFaces[base.substr(0, sep)];
		}
	std::string heavy;
	long heavyFaces = -1;
	for (auto& [file, cnt] : fileFaces)
		if (cnt > heavyFaces) { heavyFaces = cnt; heavy = file; }
	if (heavy.empty()) heavy = parts.begin()->first;
	std::string stem = heavy;
	const size_t dot = stem.find_last_of('.');
	if (dot != std::string::npos) stem.resize(dot);

	std::string out = "[{\"name\":\"";
	jsonEscape(out, (stem + " (model)").c_str());
	out += "\",";
	char buf[48];
	std::snprintf(buf, sizeof buf, "\"meshes\":%zu,", parts.size());
	out += buf;
	appendSurfArray(out, all);
	out += ",\"children\":[";
	bool first = true;
	for (auto& [file, names] : parts) {
		if (!first) out += ",";
		first = false;
		out += "{\"name\":\"";
		jsonEscape(out, file.c_str());
		out += "\",";
		appendSurfArray(out, names);
		out += "}";
	}
	out += "]}]";
	return out;
}

bool Editor_SetSurfaceProp(const char* name, const char* key, float value)
{
	// Shared setter (also used by the sidecar's numeric prop lines): sets on
	// every CurScene material whose base name matches, ::mirUV clones included.
	const bool any = fds::MaterialImport_SetSurfaceProp(CurScene, name, key, value);
	if (any) Editor_MarkDirty();
	return any;
}

// LIVE per-surface smoothing-angle edit (slider drag). Registers the override
// AND re-smooths the current mesh normals so the shading updates next frame —
// the reason this exists separate from the generic setSurfaceProp path, which
// only registered the angle (it took effect at scene init / reload). Save still
// persists smoothAngle through the normal sidecar path (shell.html keeps it in
// EDITOR_SAVE_PROPS), so this changes only WHEN the re-smooth happens, not how
// it persists. Marks the view dirty so the idle-throttled loop repaints.
void Editor_SetSmoothAngleLive(const char* surface, float angleDeg)
{
	if (!surface || !*surface) return;
	MeshOps_SetSurfaceSmoothAngle(surface, angleDeg);   // register (round-trip + Save)
	MeshOps_ResmoothSurface(surface, angleDeg);         // rebuild normals on live topology
	Editor_MarkDirty();
}

std::string Editor_SplitInstances(const char* name)
{
	if (!CurScene || !name || !*name) return "[]";
	// Faces of this surface (any of its materials — base + ::mirUV clones),
	// with world-space centres.
	struct FRef { Face* F; float c[3]; };
	std::vector<FRef> refs;
	for (TriMesh* T = CurScene->TriMeshHead; T; T = T->Next)
		for (DWord i = 0; i < T->FIndex; ++i) {
			Face& F = T->Faces[i];
			if (!F.Txtr || !F.Txtr->Name || Editor_BaseSurfName(F.Txtr->Name) != name) continue;
			Vertex* vs[3] = { F.A, F.B, F.C };
			float c[3] = { 0, 0, 0 };
			int nv = 0;
			for (int k = 0; k < 3; ++k) {
				if (!vs[k]) continue;
				Vector w;
				MatrixXVector(T->RotMat, &vs[k]->Pos, &w);
				Vector_SelfAdd(&w, &T->IPos);
				c[0] += w.x; c[1] += w.y; c[2] += w.z;
				++nv;
			}
			if (!nv) continue;
			refs.push_back({ &F, { c[0]/nv, c[1]/nv, c[2]/nv } });
		}
	if (refs.size() < 2) return "[]";

	// Cluster radius from the union bbox (same scale the focus clustering uses).
	float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
	for (const FRef& r : refs)
		for (int a = 0; a < 3; ++a) {
			if (r.c[a] < lo[a]) lo[a] = r.c[a];
			if (r.c[a] > hi[a]) hi[a] = r.c[a];
		}
	const float dx = hi[0]-lo[0], dy = hi[1]-lo[1], dz = hi[2]-lo[2];
	const float R = std::max(std::sqrt(dx*dx + dy*dy + dz*dz) * 0.15f, 2.0f);

	// Single-linkage via a uniform grid of cell size R: faces in the same or
	// adjacent cells union (linear in faces — no O(n²) pass, splitting a big
	// surface like rooms stays instant in wasm).
	const size_t n = refs.size();
	std::vector<int> parent(n);
	for (size_t i = 0; i < n; ++i) parent[i] = int(i);
	auto find = [&](int a) { while (parent[a] != a) a = parent[a] = parent[parent[a]]; return a; };
	auto unite = [&](int a, int b) { a = find(a); b = find(b); if (a != b) parent[b] = a; };
	auto cellKey = [&](int gx, int gy, int gz) {
		return (long long)(gx + 0x40000) | ((long long)(gy + 0x40000) << 20) | ((long long)(gz + 0x40000) << 40);
	};
	std::map<long long, int> cellRep;   // cell → representative face idx
	for (size_t i = 0; i < n; ++i) {
		const int gx = int(std::floor(refs[i].c[0] / R));
		const int gy = int(std::floor(refs[i].c[1] / R));
		const int gz = int(std::floor(refs[i].c[2] / R));
		for (int ox = -1; ox <= 1; ++ox)
			for (int oy = -1; oy <= 1; ++oy)
				for (int oz = -1; oz <= 1; ++oz) {
					auto it = cellRep.find(cellKey(gx+ox, gy+oy, gz+oz));
					if (it != cellRep.end()) unite(it->second, int(i));
				}
		cellRep[cellKey(gx, gy, gz)] = find(int(i));
	}

	std::map<int, long> clusterSize;
	for (size_t i = 0; i < n; ++i) ++clusterSize[find(int(i))];
	if (clusterSize.size() < 2) return "[]";
	int primary = -1; long primarySize = -1;
	for (auto& [root, sz] : clusterSize)
		if (sz > primarySize) { primarySize = sz; primary = root; }

	// Next free "#k" suffix (repeat splits mustn't collide).
	int suffix = 2;
	for (bool taken = true; taken; ) {
		taken = false;
		char probe[160];
		std::snprintf(probe, sizeof probe, "%s#%d", name, suffix);
		for (Material* M = MatLib; M; M = M->Next)
			if (M->RelScene == CurScene && M->Name && Editor_BaseSurfName(M->Name) == probe) { taken = true; ++suffix; break; }
	}

	Material* tail = MatLib;
	while (tail && tail->Next) tail = tail->Next;

	// Stable numbering: every non-primary cluster gets the next "#k", in
	// first-seen face order.
	std::map<int, int> clusterK;
	for (size_t i = 0; i < n; ++i) {
		const int root = find(int(i));
		if (root != primary && !clusterK.count(root)) clusterK[root] = suffix++;
	}

	// Per (cluster, source material) clone. The ::mirUV suffix stays OUTSIDE
	// the "#k" ("momy#2::mirUV") so Editor_BaseSurfName collapses to "momy#2".
	std::map<std::pair<int, Material*>, Material*> clones;
	static const std::string mirSuf = "::mirUV";
	for (size_t i = 0; i < n; ++i) {
		const int root = find(int(i));
		if (root == primary) continue;
		Material* src = refs[i].F->Txtr;
		const auto key = std::make_pair(root, src);
		auto it = clones.find(key);
		if (it == clones.end()) {
			const std::string srcName = src->Name ? src->Name : "";
			const bool isMir = srcName.size() > mirSuf.size() &&
			                   srcName.compare(srcName.size() - mirSuf.size(), mirSuf.size(), mirSuf) == 0;
			char nm[200];
			std::snprintf(nm, sizeof nm, "%s#%d%s", name, clusterK[root], isMir ? mirSuf.c_str() : "");
			Material* C = new Material(*src);   // field-copy (shares Texture*s — intended)
			C->Name = strdup(nm);
			C->Next = nullptr;
			C->Prev = tail;
			if (tail) tail->Next = C;
			tail = C;
			it = clones.emplace(key, C).first;
		}
		refs[i].F->Txtr = it->second;
	}
	// New base names for the caller (dedup'd across mir/non-mir clones).
	std::set<std::string> newNames;
	for (auto& [key, C] : clones) newNames.insert(Editor_BaseSurfName(C->Name));
	std::string out = "[";
	bool first = true;
	for (const std::string& nn : newNames) {
		if (!first) out += ",";
		first = false;
		out += "\"";
		jsonEscape(out, nn.c_str());
		out += "\"";
	}
	out += "]";
	// New MatLib entries → matIDs + table (post-load materials are invisible
	// to the deferred kernel until the table is rebuilt — the greets mirror
	// "yellow tint" lesson).
	Scene_RebuildMatTable(CurScene);
	Editor_MarkDirty();
	std::fprintf(stderr, "[EDITOR] split '%s': %zu faces, %zu clusters (primary %ld faces) -> %s\n",
	             name, n, clusterSize.size(), primarySize, out.c_str());
	return out;
}

// ── Light editing (shared native/wasm — native uses it via the LIGHT_TEST
// snapshot hook) ────────────────────────────────────────────────────────────
// Scene-authored omnis only (Omni_SceneAuthored = the i-th FLD/LWS light, in
// list order — the same index the server's LWS/FLD patchers use).
//
// Mirror-clone omnis are EXCLUDED: GreetsMirror's BuildMirror memcpy()s each
// source omni (so the clone inherits Omni_SceneAuthored — the FDS_DEFS.H
// "code-created lights never carry it" comment predates that path) and then
// PREPENDS the clone to OmniHead. With mirrors on, a raw Omni_SceneAuthored
// walk therefore enumerates ~4×11 clones BEFORE the 11 authored lights —
// the "~50 lights" list, wrong LWS write-back indices, edits landing on
// phantom reflected lights. Clones carry Omni_MirrorClone + mirrorId>0
// (set only by mirror code); filtering them restores the FLD/LWS file order.
// (Bounce-cone spots REPLACE their Flags wholesale, so they never carry
// Omni_SceneAuthored — the extra tests are belt and braces.)
static bool isAuthoredNonCloneOmni(const Omni *O)
{
	return (O->Flags & Omni_SceneAuthored)
	    && !(O->Flags & (Omni_MirrorClone | Omni_BounceCone))
	    && O->mirrorId == 0;
}

static Omni *lightByIndex(int want)
{
	if (!CurScene) return nullptr;
	int i = 0;
	for (Omni *O = CurScene->OmniHead; O; O = O->Next) {
		if (!isAuthoredNonCloneOmni(O)) continue;
		if (i == want) return O;
		++i;
	}
	return nullptr;
}

// The flare sprite's COLOR is a texture baked from the light color (Init_-
// Flares → Generate_RGBFlare, deduped by color) — it does NOT read O->L at
// draw time. A live color edit therefore re-points the omni at a flare
// texture for the new color. Cached by packed RGB: a slider drag revisits
// colors and each 256² flare is ~256 KB — regenerate each distinct color once
// per session, reuse after.
static void retintFlare(Omni *O)
{
	if (!O->F.Txtr) return;   // no flare on this light — nothing to retint
	static std::map<unsigned, Material*> cache;
	const unsigned key = (unsigned(O->L.R) << 16) | (unsigned(O->L.G) << 8) | unsigned(O->L.B);
	auto it = cache.find(key);
	if (it == cache.end()) {
		Material *M = Generate_RGBFlare((unsigned char)O->L.R, (unsigned char)O->L.G,
		                                (unsigned char)O->L.B);
		M->RelScene = CurScene;   // same tagging Init_Flares gives flare materials
		it = cache.emplace(key, M).first;
	}
	O->F.Txtr = it->second;
}

// ── Runtime UV re-projection ────────────────────────────────────────────────
// UVs are BAKED at FLD load (Get_UV, FLD_MAT.CPP) from the material's
// LightWave projection ("Planar/Cylindrical/Spherical/Cubic Image Map"),
// TextureSize (world-units-per-tile scale), TextureCenter, and the axis flag.
// The engine Material keeps copies of all of those, and the render path reads
// FACE-level UVs (FRUSTRUM copies F->U1.. into the transformed verts), so a
// live re-projection just recomputes each face corner's UV from its
// OBJECT-SPACE position — the same math, then retangents affected meshes.
namespace {
constexpr float kPi   = 3.14159265358979323846f;
constexpr float kPiD2 = kPi * 0.5f;
constexpr float kPiM2 = kPi * 2.0f;

// Cartesian → cylinder heading / sphere heading+pitch (Get_UV's helpers).
float uvXyzToH(float x, float, float z)
{
	if (x == 0.0f && z == 0.0f) return 0.0f;
	if (z == 0.0f) return (x < 0.0f) ? kPiD2 : -kPiD2;
	if (z < 0.0f)  return -std::atan(x / z) + kPi;
	return -std::atan(x / z);
}
void uvXyzToHP(float x, float y, float z, float *h, float *p)
{
	if (x == 0.0f && z == 0.0f) {
		*h = 0.0f;
		*p = (y != 0.0f) ? ((y < 0.0f) ? -kPiD2 : kPiD2) : 0.0f;
		return;
	}
	*h = uvXyzToH(x, y, z);
	x = std::sqrt(x * x + z * z);
	*p = (x == 0.0f) ? ((y < 0.0f) ? -kPiD2 : kPiD2) : std::atan(y / x);
}

// One corner's (u,v) under `proj` (0=planar 1=cylindrical 2=spherical
// 3=cubic). pos is object space; axis bits = Texture_XAxis/YAxis (LWREAD.H).
// faceN = |face normal| (cubic picks its dominant axis per face).
void uvProject(int proj, const Vector &pos, const Vector &ctr, const Vector &size,
               unsigned axis, const Vector &faceN, float &u, float &v)
{
	const float vx = pos.x - ctr.x, vy = pos.y - ctr.y, vz = pos.z - ctr.z;
	float s, t, lon, lat;
	switch (proj) {
	default:
	case 0:   // planar
		s = (axis & 1) ? vz / size.z + 0.5f : vx / size.x + 0.5f;
		t = (axis & 2) ? -vz / size.z + 0.5f : -vy / size.y + 0.5f;
		u = s; v = t;
		return;
	case 1:   // cylindrical
		if (axis & 1)      { lon = uvXyzToH(vz, vx, -vy); t = -vx / size.x + 0.5f; }
		else if (axis & 2) { lon = uvXyzToH(-vx, vy, vz); t = -vy / size.y + 0.5f; }
		else               { lon = uvXyzToH(-vx, vz, -vy); t = -vz / size.z + 0.5f; }
		u = 1.0f - lon / kPiM2; v = t;
		return;
	case 2:   // spherical
		if (axis & 1)      uvXyzToHP(vz, vx, -vy, &lon, &lat);
		else if (axis & 2) uvXyzToHP(-vx, vy, vz, &lon, &lat);
		else               uvXyzToHP(-vx, vz, -vy, &lon, &lat);
		u = 1.0f - lon / kPiM2;
		v = 0.5f - lat / kPi;
		return;
	case 3: { // cubic: dominant face-normal axis picks the plane
		const bool X = faceN.x > faceN.y && faceN.x > faceN.z;
		const bool Y = !X && faceN.y > faceN.x && faceN.y > faceN.z;
		s = X ? vz / size.z : vx / size.x;
		t = Y ? -vz / size.z : -vy / size.y;
		u = s + 0.5f; v = t + 0.5f;
		return;
	}
	}
}
} // namespace

// LW projection-string names, indexed by the proj codes above (persisted into
// the material so the LWO/FLD writers can serialize the choice).
static const char *kProjName[4] = {
	"Planar Image Map", "Cylindrical Image Map",
	"Spherical Image Map", "Cubic Image Map",
};

std::string Editor_SetUVMapping(const char *name, int proj, float sx, float sy, float sz, int axis)
{
	if (!CurScene || !name || proj < 0 || proj > 3) return "{}";
	if (sx == 0.0f) sx = 1.0f;   // zero scale = division blowup
	if (sy == 0.0f) sy = 1.0f;
	if (sz == 0.0f) sz = 1.0f;
	// Update the mapping parameters on every material of the surface (base +
	// ::mirUV clones) so re-projection, persistence and re-enumeration agree.
	std::set<Material*> mats;
	for (Material *M = MatLib; M; M = M->Next)
		if (M->RelScene == CurScene && M->Name && Editor_BaseSurfName(M->Name) == name)
			mats.insert(M);
	if (mats.empty()) return "{}";
	const Vector size = { sx, sy, sz };
	for (Material *M : mats) {
		M->ColorTexture = strdup(kProjName[proj]);   // init-time-leak convention
		M->TextureSize = size;
		M->TextureFlags = (unsigned short)((M->TextureFlags & ~7u) | (axis & 7));
	}
	// Re-project every face using those materials. Face-level UVs only —
	// vertices are shared across surfaces, and the render path reads F->U1..
	long faces = 0;
	std::set<TriMesh*> touched;
	for (TriMesh *T = CurScene->TriMeshHead; T; T = T->Next) {
		for (DWord i = 0; i < T->FIndex; ++i) {
			Face &F = T->Faces[i];
			if (!F.Txtr || !mats.count(F.Txtr)) continue;
			if (!F.A || !F.B || !F.C) continue;
			Vector e1, e2, n;
			Vector_Sub(&F.B->Pos, &F.A->Pos, &e1);
			Vector_Sub(&F.C->Pos, &F.A->Pos, &e2);
			Cross_Product(&e1, &e2, &n);
			n.x = std::fabs(n.x); n.y = std::fabs(n.y); n.z = std::fabs(n.z);
			Material *M = F.Txtr;
			uvProject(proj, F.A->Pos, M->TextureCenter, size, axis, n, F.U1, F.V1);
			uvProject(proj, F.B->Pos, M->TextureCenter, size, axis, n, F.U2, F.V2);
			uvProject(proj, F.C->Pos, M->TextureCenter, size, axis, n, F.U3, F.V3);
			touched.insert(T);
			++faces;
		}
	}
	// New UVs = new tangent basis (normal/parallax relief direction).
	for (TriMesh *T : touched) Compute_Vertex_Tangents(T);
	Editor_MarkDirty();
	char buf[160];
	std::snprintf(buf, sizeof buf,
	              "{\"faces\":%ld,\"meshes\":%zu,\"proj\":\"%s\",\"size\":[%g,%g,%g],\"axis\":%d}",
	              faces, touched.size(), kProjName[proj], sx, sy, sz, axis);
	std::fprintf(stderr, "[EDITOR] uvmap '%s': %s\n", name, buf);
	return buf;
}

bool Editor_SetLightProp(int index, const char *key, float value)
{
	Omni *O = lightByIndex(index);
	if (!O) return false;
	if      (!std::strcmp(key, "r")) { O->L.R = value; retintFlare(O); }
	else if (!std::strcmp(key, "g")) { O->L.G = value; retintFlare(O); }
	else if (!std::strcmp(key, "b")) { O->L.B = value; retintFlare(O); }
	else if (!std::strcmp(key, "intensity")) {
		// Set every key so animated envelopes take the value uniformly (greets
		// lights are all single-key; multi-key edits flatten the curve — the
		// panel shows the key count so that's not a surprise).
		for (dword k = 0; k < O->Size.NumKeys; ++k) O->Size.Keys[k].Pos.x = value;
	} else if (!std::strcmp(key, "range")) {
		for (dword k = 0; k < O->Range.NumKeys; ++k) O->Range.Keys[k].Pos.x = value;
	} else if (!std::strcmp(key, "flareScale")) {
		O->FlareScale = value;   // 0 = legacy (track intensity 1:1)
	} else return false;
	Editor_MarkDirty();
	return true;
}

// ── Lights JSON (shared native/wasm) ────────────────────────────────────────
// Color lives in Omni::L (0-255); intensity/range are 1-key-per-value splines
// whose scalar sits in Keys[].Pos.x — Animate_Objects re-interpolates ISize/
// IRange from them every tick, so a key edit shows on the next rendered frame.
// Authored lights only (isAuthoredNonCloneOmni — mirror clones excluded, see
// above). Each entry carries TWO indices:
//   i    — authored index (this file's lightByIndex / the LWS write-back space)
//   rawI — the light's position in the legacy unfiltered Omni_SceneAuthored
//          walk, which is what MainLoop.cpp's editorFocusLight still counts.
//          With mirrors off they're equal; with mirrors on the shell must pass
//          rawI to editorFocusLight or it would focus a clone's phantom pos.
std::string Editor_GetLightsJSON()
{
	std::string out = "[";
	int i = 0, raw = 0;
	if (CurScene) for (Omni *O = CurScene->OmniHead; O; O = O->Next) {
		if (!(O->Flags & Omni_SceneAuthored)) continue;
		const int rawIdx = raw++;               // editorFocusLight's index space
		if (!isAuthoredNonCloneOmni(O)) continue;
		char buf[380];
		std::snprintf(buf, sizeof buf,
		  "%s{\"i\":%d,\"rawI\":%d,\"r\":%.0f,\"g\":%.0f,\"b\":%.0f,"
		  "\"intensity\":%.4g,\"range\":%.4g,\"flareScale\":%.4g,"
		  "\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,"
		  "\"type\":%d,\"shadow\":%d,\"posKeys\":%u,\"sizeKeys\":%u,\"rangeKeys\":%u}",
		  i ? "," : "", i, rawIdx, O->L.R, O->L.G, O->L.B,
		  O->Size.NumKeys ? O->Size.Keys[0].Pos.x : 0.0f,
		  O->Range.NumKeys ? O->Range.Keys[0].Pos.x : 0.0f,
		  O->FlareScale > 0.0f ? O->FlareScale : 1.0f,
		  O->IPos.x, O->IPos.y, O->IPos.z,
		  int(O->Type), (O->Flags & Omni_CastsShadow) ? 1 : 0,
		  (unsigned)O->Pos.NumKeys, (unsigned)O->Size.NumKeys, (unsigned)O->Range.NumKeys);
		out += buf;
		++i;
	}
	out += "]";
	return out;
}

// ── Click picking (shared native/wasm) ──────────────────────────────────────
// The (u,v) input is normalized [0,1] over the ENGINE surface. Pixel↔ray
// mapping is the canonical deferred-kernel math (docs/GRAPHICS_PIPELINE.md §5):
//   view-space z  = (0xFF80 - ZPage16[i]) / g_zscale        (0 = untouched/sky)
//   pixel → ray   d = ((px - CntrEX)/FOVX, (CntrEY - py)/FOVY, 1)
//   point → pixel spx = CntrEX + x/z·FOVX ; spy = CntrEY - y/z·FOVY
// with view = View->Mat · (world - View->ISource) and world = RotMat·Pos + IPos
// (the same chain Editor_SplitInstances / editorFocusSurface use).

// Möller–Trumbore in view space: ray origin (0,0,0), direction (dx,dy,1),
// TWO-SIDED (glass panels are Mat_TwoSided / viewed from either face). Since
// dir.z == 1, the returned t IS the hit's view-space z — directly comparable
// with the decoded opaque depth.
static bool rayHitsTriViewSpace(const Vector &A, const Vector &B, const Vector &C,
                                float dx, float dy, float &tOut)
{
	const float e1x = B.x - A.x, e1y = B.y - A.y, e1z = B.z - A.z;
	const float e2x = C.x - A.x, e2y = C.y - A.y, e2z = C.z - A.z;
	// p = d × e2
	const float px = dy * e2z - e2y;
	const float py = e2x - dx * e2z;
	const float pz = dx * e2y - dy * e2x;
	const float det = e1x * px + e1y * py + e1z * pz;
	if (std::fabs(det) < 1e-12f) return false;        // parallel / degenerate
	const float inv = 1.0f / det;
	const float sx = -A.x, sy = -A.y, sz = -A.z;      // origin - A
	const float uu = (sx * px + sy * py + sz * pz) * inv;
	if (uu < -1e-4f || uu > 1.0001f) return false;
	// q = s × e1
	const float qx = sy * e1z - sz * e1y;
	const float qy = sz * e1x - sx * e1z;
	const float qz = sx * e1y - sy * e1x;
	const float vv = (dx * qx + dy * qy + qz) * inv;
	if (vv < -1e-4f || uu + vv > 1.0001f) return false;
	const float t = (e2x * qx + e2y * qy + e2z * qz) * inv;
	if (t <= 0.05f) return false;                      // behind / at the eye
	tOut = t;
	return true;
}

// Nearest Mat_Transparent face along the view ray (dx,dy,1). Skips
// GreetsMirror's "__mirrorClone_*" meshes — they duplicate world geometry
// (including transparent screens) MIRRORED BEHIND the glass and would shadow
// real surfaces along rays that pass a mirror. Single-threaded editor pick
// reading the frame's immutable RotMat/IPos — race-free by construction.
static bool editorXparRayNearest(float dx, float dy, float &bestT, const Material *&bestMat)
{
	bestT = 3.0e38f;
	bestMat = nullptr;
	if (!CurScene || !View) return false;
	const TriMesh *cloneMeshes[64];
	int cloneCount = 0;
	for (Object *Obj = CurScene->ObjectHead; Obj; Obj = Obj->Next) {
		if (Obj->Type != Obj_TriMesh || !Obj->Name || !Obj->Data) continue;
		if (std::strncmp(Obj->Name, "__mirrorClone_", 14) != 0) continue;
		if (cloneCount < 64) cloneMeshes[cloneCount++] = (const TriMesh *)Obj->Data;
	}
	for (TriMesh *T = CurScene->TriMeshHead; T; T = T->Next) {
		bool isClone = false;
		for (int c = 0; c < cloneCount; ++c) if (cloneMeshes[c] == T) { isClone = true; break; }
		if (isClone) continue;
		for (DWord f = 0; f < T->FIndex; ++f) {
			Face &F = T->Faces[f];
			if (!F.Txtr || !(F.Txtr->Flags & Mat_Transparent)) continue;
			if (!F.A || !F.B || !F.C) continue;
			Vertex *vs[3] = { F.A, F.B, F.C };
			Vector vp[3];
			for (int k = 0; k < 3; ++k) {
				Vector w, rel;
				MatrixXVector(T->RotMat, &vs[k]->Pos, &w);   // object → world
				Vector_SelfAdd(&w, &T->IPos);
				Vector_Sub(&w, &View->ISource, &rel);        // world → view
				MatrixXVector(View->Mat, &rel, &vp[k]);
			}
			float t;
			if (rayHitsTriViewSpace(vp[0], vp[1], vp[2], dx, dy, t) && t < bestT) {
				bestT = t;
				bestMat = F.Txtr;
			}
		}
	}
	return bestMat != nullptr;
}

// Optional diagnostics for the native PICK_TEST hook.
struct PickDebug {
	float zOpaque = -1.0f;      // decoded opaque depth at the pixel (3e38 = sky)
	float tXpar = -1.0f;        // nearest transparent hit's view z (-1 = none)
	const Material *xparMat = nullptr;
	bool usedXpar = false;      // the returned name came from the fallback
	bool mirrorPx = false;      // pixel rejected by the mirrorId gate
};

static std::string editorPickCore(float u, float v, PickDebug *dbg)
{
	if (!CurScene) return "";
	const float pxf = u * float(XRes);
	const float pyf = v * float(YRes);
	const int x = int(pxf);
	const int y = int(pyf);
	if (x < 0 || y < 0 || x >= XRes || y >= YRes) return "";
	const size_t i = size_t(y) * size_t(XRes) + size_t(x);

	// Opaque depth at the click pixel. ZPage16 == 0 → rasterizer never touched
	// the pixel (sky) → "infinitely far", so glass against sky still picks.
	float zOpaque = 3.0e38f;
	if (ZPage16 && g_zscale > 0.0f) {
		const word zEnc = ZPage16[i];
		if (zEnc != 0) zOpaque = float(0xFF80 - zEnc) / g_zscale;
	}
	if (dbg) dbg->zOpaque = zOpaque;

	// Transparent fallback: transparent surfaces never write the opaque
	// G-buffer matID plane, so a click on glass historically fell through to
	// the wall behind. Cast the pixel's view ray at the scene's Mat_Transparent
	// faces; if the nearest hit is NEARER than the opaque depth, that surface
	// wins. Tolerance covers 16-bit depth quantization + glass mounted flush
	// against opaque geometry (panel ≈ wall depth).
	if (View && FOVX != 0.0f && FOVY != 0.0f) {
		const float dx = (pxf - CntrEX) / FOVX;
		const float dy = (CntrEY - pyf) / FOVY;
		float tHit;
		const Material *hitMat;
		if (editorXparRayNearest(dx, dy, tHit, hitMat)) {
			if (dbg) { dbg->tXpar = tHit; dbg->xparMat = hitMat; }
			const float tol = (g_zscale > 0.0f)
				? std::max(2.0f / g_zscale, zOpaque * 0.005f)
				: zOpaque * 0.005f;
			if (tHit <= zOpaque + tol && hitMat->Name) {
				if (dbg) dbg->usedXpar = true;
				return Editor_BaseSurfName(hitMat->Name);
			}
		}
	}

	// Opaque path — unchanged from the original G-buffer-only pick.
	if (!g_gbuffer || g_gbuffer->txtr.empty()) return "";
	if (i >= g_gbuffer->txtr.size()) return "";
	// Pixels inside a mirror reflection carry a nonzero mirrorId — they show
	// CLONE geometry (possibly of a surface behind you). Picking through the
	// glass selected whatever happened to be reflected; reject instead.
	if (!g_gbuffer->mirrorId.empty() && i < g_gbuffer->mirrorId.size()
	    && g_gbuffer->mirrorId[i] != 0) {
		if (dbg) dbg->mirrorPx = true;
		return "";
	}
	const unsigned mid = (g_gbuffer->txtr[i] >> 20) & 0xFF;
	MatTable mt = Scene_GetMatTable(CurScene);
	if (mid >= mt.count || !mt.data[mid] || !mt.data[mid]->Name) return "";
	return Editor_BaseSurfName(mt.data[mid]->Name);
}

std::string Editor_PickSurface(float u, float v)
{
	return editorPickCore(u, v, nullptr);
}

// Project one omni to pixel coordinates (false = behind the camera).
static bool projectOmniToScreen(const Omni *O, float &spx, float &spy)
{
	if (!View) return false;
	Vector rel, vp;
	Vector_Sub(const_cast<Vector *>(&O->IPos), &View->ISource, &rel);
	MatrixXVector(View->Mat, &rel, &vp);
	if (vp.z <= 0.05f) return false;
	spx = CntrEX + vp.x / vp.z * FOVX;
	spy = CntrEY - vp.y / vp.z * FOVY;
	return true;
}

// Light picking: nearest authored (non-clone) omni whose screen projection is
// within ~14 px (at 1080p, resolution-scaled) of the click. Returns the
// AUTHORED light index (Editor_GetLightsJSON's "i" / lightByIndex's space),
// -1 = no light near → caller falls back to the surface pick.
int Editor_PickLight(float u, float v)
{
	if (!CurScene || !View || FOVX == 0.0f || FOVY == 0.0f) return -1;
	const float pxf = u * float(XRes);
	const float pyf = v * float(YRes);
	const float radius = std::max(6.0f, 14.0f * float(YRes) / 1080.0f);
	float bestD2 = radius * radius;
	int best = -1, i = 0;
	for (Omni *O = CurScene->OmniHead; O; O = O->Next) {
		if (!isAuthoredNonCloneOmni(O)) continue;
		const int idx = i++;
		float spx, spy;
		if (!projectOmniToScreen(O, spx, spy)) continue;
		const float dx = spx - pxf, dy = spy - pyf;
		const float d2 = dx * dx + dy * dy;
		if (d2 < bestD2) { bestD2 = d2; best = idx; }
	}
	return best;
}

#ifndef __EMSCRIPTEN__
// PICK_TEST — native validation for the click-pick + lights paths, same env-
// hook convention as Snapshot.cpp's LIGHT_TEST/IMPORT_TEST (test hooks, not
// runtime tunables). Invoked from Editor_GetSurfacesJSON so it runs when the
// DUMP_SURFACES snapshot hook fires (post-tick — frame data is live).
//   PICK_TEST=scan          grid-sweep; prints every cell where the transparent
//                           fallback fired (locates glass panels headlessly)
//   PICK_TEST="u,v[;u,v..]" point probes with full diagnostics
// Always prints the raw-vs-filtered omni counts + lights JSON and a light-pick
// projection round-trip (run with --greets_mirror to exercise clone filtering).
static void editorRunPickTestHook()
{
	const char *spec = std::getenv("PICK_TEST");
	if (!spec) return;
	static bool done = false;
	if (done) return;
	done = true;
	{
		int total = 0, flagged = 0;
		if (CurScene) for (Omni *O = CurScene->OmniHead; O; O = O->Next) {
			++total;
			if (O->Flags & Omni_SceneAuthored) ++flagged;
		}
		const std::string lj = Editor_GetLightsJSON();
		int n = 0;
		for (size_t p = lj.find("{\"i\""); p != std::string::npos; p = lj.find("{\"i\"", p + 1)) ++n;
		std::fprintf(stderr, "[PICKTEST] omnis: total=%d sceneAuthored-flagged=%d authored(filtered)=%d\n",
		             total, flagged, n);
		std::fprintf(stderr, "[PICKTEST] lights json: %s\n", lj.c_str());
		// Light-pick round trip: project each authored light, pick at its pixel.
		int li = 0;
		if (CurScene) for (Omni *O = CurScene->OmniHead; O; O = O->Next) {
			if (!isAuthoredNonCloneOmni(O)) continue;
			const int idx = li++;
			float spx, spy;
			if (!projectOmniToScreen(O, spx, spy)) continue;
			if (spx < 0 || spy < 0 || spx >= float(XRes) || spy >= float(YRes)) continue;
			const int got = Editor_PickLight(spx / float(XRes), spy / float(YRes));
			std::fprintf(stderr, "[PICKTEST] light %d at px(%.0f,%.0f) -> pickLight=%d%s\n",
			             idx, spx, spy, got,
			             got == idx ? "" : "  (nearer light overlaps)");
		}
	}
	if (!std::strcmp(spec, "scan")) {
		int hits = 0;
		for (int gy = 1; gy < 24; ++gy)
			for (int gx = 1; gx < 40; ++gx) {
				const float u = gx / 40.0f, v = gy / 24.0f;
				PickDebug d;
				const std::string r = editorPickCore(u, v, &d);
				if (!d.usedXpar) continue;
				++hits;
				std::fprintf(stderr,
				    "[PICKSCAN] u=%.3f v=%.3f px(%d,%d) -> '%s' t=%.2f zOpq=%.2f\n",
				    u, v, int(u * float(XRes)), int(v * float(YRes)), r.c_str(), d.tXpar,
				    d.zOpaque >= 1e37f ? -1.0f : d.zOpaque);
			}
		std::fprintf(stderr, "[PICKSCAN] %d grid cells picked a transparent surface\n", hits);
		return;
	}
	std::string all = spec;
	size_t pos = 0;
	while (pos <= all.size()) {
		size_t semi = all.find(';', pos);
		if (semi == std::string::npos) semi = all.size();
		const std::string s = all.substr(pos, semi - pos);
		pos = semi + 1;
		if (s.empty()) continue;
		float u, v;
		if (std::sscanf(s.c_str(), "%f,%f", &u, &v) != 2) continue;
		PickDebug d;
		const std::string r = editorPickCore(u, v, &d);
		std::fprintf(stderr,
		    "[PICKTEST] u=%.4f v=%.4f -> '%s'%s  zOpq=%.2f tXpar=%.2f xparMat='%s'%s\n",
		    u, v, r.c_str(), d.usedXpar ? " [XPAR]" : "",
		    d.zOpaque >= 1e37f ? -1.0f : d.zOpaque, d.tXpar,
		    (d.xparMat && d.xparMat->Name) ? d.xparMat->Name : "",
		    d.mirrorPx ? " [mirrorPx]" : "");
	}
}
#endif // !__EMSCRIPTEN__

static std::atomic<bool> g_editorDirty{true};   // first frame renders
void Editor_MarkDirty()    { g_editorDirty.store(true, std::memory_order_relaxed); }
bool Editor_ConsumeDirty() { return g_editorDirty.exchange(false, std::memory_order_relaxed); }

bool Editor_ImportTexture(const char* surface, const char* role,
                          const char* filename, const unsigned char* data, unsigned long len)
{
	if (!surface || !role || !data || len == 0) return false;
	// Preserve the uploaded extension so the image loader picks the right codec.
	const char* dot = filename ? std::strrchr(filename, '.') : nullptr;
	std::string tmp = std::string("/tmp/ed_import") + (dot ? dot : ".png");
	FILE* f = std::fopen(tmp.c_str(), "wb");
	if (!f) { std::fprintf(stderr, "[EDITOR] import: can't open %s\n", tmp.c_str()); return false; }
	std::fwrite(data, 1, len, f);
	std::fclose(f);
	const bool ok = fds::MaterialImport_ApplyMapFile(CurScene, surface, role, tmp.c_str());
	if (ok && !std::strcmp(role, "metallic")) {
		// Metallic drives the ENV-reflection system (not the scalar
		// `reflection` slider): ApplyMapFile just auto-defaulted env_refl +
		// env_bake_fix on; drop the scene's baked panoramas so the next
		// frame's FramePrep re-bakes them all with this surface's new metal
		// look (and bakes a fresh probe for it). Without this the import
		// looked like it did nothing — the #1 "metallic has no effect"
		// report.
		fds::EnvReflection_Invalidate(CurScene);
		std::fprintf(stderr, "[EDITOR] metallic import on '%s': env_refl=%d "
		             "env_bake_fix=%d — panoramas invalidated, re-baking next frame\n",
		             surface, fds::FeatureFlags::env_refl() ? 1 : 0,
		             fds::FeatureFlags::env_bake_fix() ? 1 : 0);
	}
	if (ok) Editor_MarkDirty();
	return ok;
}

} // namespace rev

// ── Browser (Embind) surface API ───────────────────────────────────────────
// Exposed to JS as Module.editorGetSurfaces() / Module.editorSetSurfaceProp().
// The shell.html surface panel calls these; edits land on the live Material and
// show on the next rendered frame. Wasm-only — native uses the rev:: functions
// directly (e.g. the DUMP_SURFACES snapshot hook).
#ifdef __EMSCRIPTEN__
#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <vector>

void Editor_SetHighlight(const char* name);   // RENDER.CPP — outline pass

namespace {
std::string js_editorGetSurfaces() { return rev::Editor_GetSurfacesJSON(); }
std::string js_editorGetObjects()  { return rev::Editor_GetObjectsJSON(); }
void js_editorHighlight(std::string name)
{
	Editor_SetHighlight(name.c_str());
	rev::Editor_MarkDirty();
}
bool js_editorSetSurfaceProp(std::string name, std::string key, float value)
{
	return rev::Editor_SetSurfaceProp(name.c_str(), key.c_str(), value);
}
// Live normal re-smooth for the smoothAngle slider (register + re-smooth +
// dirty). Save still persists via editorSetSurfaceProp/the sidecar.
void js_editorSetSmoothAngleLive(std::string name, float angleDeg)
{
	rev::Editor_SetSmoothAngleLive(name.c_str(), angleDeg);
}
std::string js_editorSplitInstances(std::string name)
{
	return rev::Editor_SplitInstances(name.c_str());
}
std::string js_editorSetUVMapping(std::string name, int proj,
                                  float sx, float sy, float sz, int axis)
{
	return rev::Editor_SetUVMapping(name.c_str(), proj, sx, sy, sz, axis);
}
// Drop every env-reflection panorama for the scene; the next frame's
// FramePrep re-bakes them with the CURRENT lights/materials/positions.
void js_editorRebakeEnv()
{
	fds::EnvReflection_Invalidate(CurScene);
	// Rebake re-renders with the CURRENT flag state — a session that
	// explicitly toggled env_bake_fix off keeps re-baking legacy sliver
	// panos, which reads as "rebake does nothing". Say so where it can be
	// seen (the shell mirrors this warning in the status line / button).
	std::fprintf(stderr, "[EDITOR] rebake refl: panoramas invalidated "
	             "(env_refl=%d env_bake_fix=%d%s)\n",
	             fds::FeatureFlags::env_refl() ? 1 : 0,
	             fds::FeatureFlags::env_bake_fix() ? 1 : 0,
	             fds::FeatureFlags::env_bake_fix()
	                 ? "" : " — LEGACY bake, expect sliver panos");
	rev::Editor_MarkDirty();
}

// Per-surface env-reflection state for the panel indicator + the pano
// viewer's jump-to-surface: is env_refl on, does any material of this
// surface carry a metalness map / Reflection > 0, and which baked store
// (0-based; -1 = none yet) does it map to.
std::string js_editorEnvInfo(std::string name)
{
	int store = -1, metal = 0, refl = 0;
	if (CurScene) {
		for (Material* M = MatLib; M; M = M->Next) {
			if (M->RelScene != CurScene || !M->Name) continue;
			if (rev::Editor_BaseSurfName(M->Name) != name) continue;
			if (M->MetallicMap) metal = 1;
			if (M->Reflection > 0.0f) refl = int(M->Reflection);
			const int idx = fds::EnvReflection_StoreIndex(CurScene, M);
			if (idx >= 0 && store < 0) store = idx;
		}
	}
	char buf[160];
	std::snprintf(buf, sizeof buf,
	  "{\"on\":%d,\"fix\":%d,\"metal\":%d,\"reflection\":%d,\"store\":%d,\"count\":%d}",
	  fds::FeatureFlags::env_refl() ? 1 : 0,
	  fds::FeatureFlags::env_bake_fix() ? 1 : 0,
	  metal, refl, store,
	  CurScene ? fds::EnvReflection_Count(CurScene) : 0);
	return buf;
}
// Which surfaces map to baked store `idx` (0-based) — the pano viewer's
// truth label ("pano 2/5: momy"). Dedup'd editor base names.
std::string js_editorEnvPanoInfo(int idx)
{
	std::set<std::string> names;
	if (CurScene)
		for (Material* M = MatLib; M; M = M->Next) {
			if (M->RelScene != CurScene || !M->Name) continue;
			if (fds::EnvReflection_StoreIndex(CurScene, M) == idx)
				names.insert(rev::Editor_BaseSurfName(M->Name));
		}
	std::string out = "[";
	bool first = true;
	for (const std::string& n : names) {
		if (!first) out += ",";
		first = false;
		out += "\"";
		rev::jsonEscape(out, n.c_str());
		out += "\"";
	}
	out += "]";
	return out;
}
std::string js_editorReadPixel(int x, int y)
{
	if (!VPage || x < 0 || y < 0 || x >= XRes || y >= YRes) return "oob";
	const dword p = reinterpret_cast<dword*>(VPage)[size_t(y) * XRes + x];
	char buf[48];
	std::snprintf(buf, sizeof buf, "%u,%u,%u", (p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF);
	return buf;
}
int js_editorEnvPanoCount()
{
	return CurScene ? fds::EnvReflection_Count(CurScene) : 0;
}
// bytes is a JS Uint8Array of the uploaded image file.
bool js_editorImportTexture(std::string surface, std::string role,
                            std::string filename, emscripten::val bytes)
{
	std::vector<unsigned char> buf = emscripten::vecFromJSArray<unsigned char>(bytes);
	return rev::Editor_ImportTexture(surface.c_str(), role.c_str(),
	                                 filename.c_str(), buf.data(), buf.size());
}
// Diagnostic: does the matTable instance the kernel renders == the MatLib
// instance the editor mutates?
std::string js_editorMatDebug(std::string name)
{
	Material* ml = nullptr;
	int mlCount = 0;
	for (Material* M = MatLib; M; M = M->Next)
		if (M->RelScene == CurScene && M->Name && name == M->Name) { if (!ml) ml = M; ++mlCount; }
	MatTable mt = Scene_GetMatTable(CurScene);
	Material* tt = nullptr;
	for (dword i = 0; i < mt.count; ++i)
		if (mt.data[i] && mt.data[i]->Name && name == mt.data[i]->Name) { tt = mt.data[i]; break; }
	char buf[360];
	std::snprintf(buf, sizeof buf,
	  "{\"matlib_lum\":%.2f,\"matlib_count\":%d,\"mattable_lum\":%.2f,\"mattable_count\":%u,\"same_ptr\":%d}",
	  ml ? ml->Luminosity : -1.0f, mlCount, tt ? tt->Luminosity : -1.0f, (unsigned)mt.count, (ml == tt) ? 1 : 0);
	return buf;
}
// ── Light enumeration ──────────────────────────────────────────────────────
// Authored (non-mirror-clone) lights only — see rev::Editor_GetLightsJSON for
// the index-space contract ("i" = LWS write-back order, "rawI" = the legacy
// unfiltered walk editorFocusLight counts).
std::string js_editorGetLights()
{
	return rev::Editor_GetLightsJSON();
}

bool js_editorSetLightProp(int index, std::string key, float value)
{
	return rev::Editor_SetLightProp(index, key.c_str(), value);
}

// ── Render knobs ───────────────────────────────────────────────────────────
// The full FeatureFlags registry (name/type/cat/help/value/default/set) as
// JSON, and name-keyed set/unset — the editor's Render section is data-driven
// from this, so any flag added to FeatureFlags.def shows up automatically.
std::string js_editorGetParams()
{
	std::string s;
	fds::FeatureFlags::dumpParamsJson(s);
	return s;
}
bool js_editorSetParam(std::string name, std::string value)
{
	const bool ok = fds::FeatureFlags::setParamFromText(name.c_str(), value.c_str());
	if (ok) rev::Editor_MarkDirty();
	return ok;
}
bool js_editorUnsetParam(std::string name)
{
	const bool ok = fds::FeatureFlags::unsetParam(name.c_str());
	if (ok) rev::Editor_MarkDirty();
	return ok;
}
// Pack upload: classify one filename into its map role with the native token
// rules (albedo/normal/height/roughness/ao, "" = skip) so the browser's
// load-a-whole-folder flow detects roles identically to --material-import.
std::string js_editorClassifyMap(std::string filename)
{
	return fds::MaterialImport_ClassifyRole(filename.c_str());
}
// Phase 2 click-to-pick: resolve the surface under a canvas click. (u,v) are
// normalized [0,1] over the ENGINE surface (shell.html undoes the canvas
// letterbox using Module.__floodTexW/H — the same math Wasm_PresentGL uses to
// draw it). Reads the last rendered frame's G-buffer matID plane, with a
// geometric ray-cast fallback for Mat_Transparent surfaces (which never write
// it) — see rev::Editor_PickSurface. Returns the base surface name ("" = no
// surface: sentinel pixel, forward-rendered, or out of range).
std::string js_editorPick(float u, float v)
{
	return rev::Editor_PickSurface(u, v);
}
// Light picking: authored-light index within a click radius of (u,v), or -1.
// shell.html tries this BEFORE editorPick and selects the light row on a hit.
int js_editorPickLight(float u, float v)
{
	return rev::Editor_PickLight(u, v);
}
// Diagnostic: the live render-flag state the kernel actually sees, so we can tell
// from JS whether the editor's flag overrides (deferred / shadow_lightmap off /
// hdr) really took effect — instead of inferring it from screenshots.
std::string js_editorFlags()
{
	using FF = fds::FeatureFlags;
	char buf[400];
	std::snprintf(buf, sizeof buf,
	  "{\"shadow_lightmap\":%d,\"shadow_dynamic\":%d,\"shadows\":%d,"
	  "\"deferred\":%d,\"deferred_quarter\":%d,\"hdr\":%d,\"hdr_linear\":%d,"
	  "\"ambientR\":%.2f,\"ambientG\":%.2f,\"ambientB\":%.2f}",
	  FF::shadow_lightmap()?1:0, FF::shadow_dynamic()?1:0, FF::shadows()?1:0,
	  FF::deferred()?1:0, FF::deferred_quarter()?1:0, FF::hdr()?1:0, FF::hdr_linear()?1:0,
	  CurScene ? CurScene->Ambient.R : -1.0f,
	  CurScene ? CurScene->Ambient.G : -1.0f,
	  CurScene ? CurScene->Ambient.B : -1.0f);
	return buf;
}
// Diagnostic: average the FINAL framebuffer (VPage, post-tonemap+post-FX) color
// over exactly the pixels whose G-buffer matID belongs to `name`. This reads the
// rendered output for one surface directly — no screenshot framing / fog noise /
// camera guessing. If a property edit doesn't move this average, the edit truly
// isn't reaching the pixels; if it does, the render responds and any "no effect"
// is a framing/perception issue.
std::string js_editorProbe(std::string name)
{
	MatTable mt = Scene_GetMatTable(CurScene);
	bool want[256] = { false };
	int  matIds[8]; int nIds = 0;
	float lum = -1, dif = -1, spec = -1; unsigned gloss = 0;
	for (dword i = 0; i < mt.count && i < 256; ++i)
		if (mt.data[i] && mt.data[i]->Name && rev::Editor_BaseSurfName(mt.data[i]->Name) == name) {
			want[i] = true;
			if (nIds < 8) matIds[nIds++] = int(i);
			lum = mt.data[i]->Luminosity; dif = mt.data[i]->Diffuse;
			spec = mt.data[i]->Specular;  gloss = mt.data[i]->Glossiness;
		}
	long count = 0; double sr = 0, sg = 0, sb = 0;
	// G-buffer diagnostics: how big is the txtr plane, how many pixels carry a
	// non-zero matID at all, and the highest matID seen — so we can tell an
	// encoding mismatch from an empty/unpopulated buffer.
	size_t gbSize = g_gbuffer ? g_gbuffer->txtr.size() : 0;
	long   gbNonZero = 0; int gbMaxId = -1;
	if (g_gbuffer && !g_gbuffer->txtr.empty() && VPage) {
		const uint32_t *tx = g_gbuffer->txtr.data();
		const uint32_t *vp = reinterpret_cast<const uint32_t *>(VPage);
		size_t n = size_t(XRes) * size_t(YRes);
		if (g_gbuffer->txtr.size() < n) n = g_gbuffer->txtr.size();
		for (size_t i = 0; i < n; ++i) {
			const int mid = int((tx[i] >> 20) & 0xFF);
			if (mid != 0) { ++gbNonZero; if (mid > gbMaxId) gbMaxId = mid; }
			if (mid < 256 && want[mid]) {
				const uint32_t c = vp[i];
				sb += double(c & 0xFF); sg += double((c >> 8) & 0xFF); sr += double((c >> 16) & 0xFF);
				++count;
			}
		}
	}
	char idbuf[64] = ""; int off = 0;
	for (int k = 0; k < nIds; ++k) off += std::snprintf(idbuf + off, sizeof(idbuf) - off, "%s%d", k ? "," : "", matIds[k]);
	char buf[360];
	std::snprintf(buf, sizeof buf,
	  "{\"px\":%ld,\"avgR\":%.1f,\"avgG\":%.1f,\"avgB\":%.1f,\"matIds\":[%s],"
	  "\"lum\":%.2f,\"dif\":%.2f,\"spec\":%.2f,\"gloss\":%u,"
	  "\"gbSize\":%zu,\"gbNonZero\":%ld,\"gbMaxId\":%d,\"xy\":\"%dx%d\"}",
	  count, count ? sr / count : -1.0, count ? sg / count : -1.0, count ? sb / count : -1.0,
	  idbuf, lum, dif, spec, gloss, gbSize, gbNonZero, gbMaxId, (int)XRes, (int)YRes);
	return buf;
}
} // namespace

EMSCRIPTEN_BINDINGS(rev_material_editor)
{
	emscripten::function("editorGetSurfaces",    &js_editorGetSurfaces);
	emscripten::function("editorGetObjects",     &js_editorGetObjects);
	emscripten::function("editorSetSurfaceProp", &js_editorSetSurfaceProp);
	emscripten::function("editorSetSmoothAngleLive", &js_editorSetSmoothAngleLive);
	emscripten::function("editorImportTexture",  &js_editorImportTexture);
	emscripten::function("editorMatDebug",       &js_editorMatDebug);
	emscripten::function("editorHighlight",      &js_editorHighlight);
	emscripten::function("editorFlags",          &js_editorFlags);
	emscripten::function("editorProbe",          &js_editorProbe);
	emscripten::function("editorPick",           &js_editorPick);
	emscripten::function("editorPickLight",      &js_editorPickLight);
	emscripten::function("editorClassifyMap",    &js_editorClassifyMap);
	emscripten::function("editorGetLights",      &js_editorGetLights);
	emscripten::function("editorSetLightProp",   &js_editorSetLightProp);
	emscripten::function("editorSplitInstances", &js_editorSplitInstances);
	emscripten::function("editorSetUVMapping",   &js_editorSetUVMapping);
	emscripten::function("editorRebakeEnv",      &js_editorRebakeEnv);
	emscripten::function("editorEnvPanoCount",   &js_editorEnvPanoCount);
	emscripten::function("editorEnvInfo",        &js_editorEnvInfo);
	emscripten::function("editorEnvPanoInfo",    &js_editorEnvPanoInfo);
	emscripten::function("editorReadPixel",      &js_editorReadPixel);
	emscripten::function("editorGetParams",      &js_editorGetParams);
	emscripten::function("editorSetParam",       &js_editorSetParam);
	emscripten::function("editorUnsetParam",     &js_editorUnsetParam);
}
#endif // __EMSCRIPTEN__
