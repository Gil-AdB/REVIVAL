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
#include <RENDER/OffscreenView.h> // g_offscreenViewDepth (map-viz overlay guard)
#include <RENDER/RenderPipeline.h> // deferredWaterMatID (isWater in surfaces JSON)

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
static void editorRunSplitTestHook();  // defined below Editor_SplitInstances
static void editorRunClearMapTestHook();  // defined next to the split hook
static void editorRunFocusTestHook();  // defined below the focus core
static void editorRunObjScaleTestHook();  // defined below Editor_SetObjectScale
static void editorRunLightGroupTestHook();  // defined below Editor_GetLightsJSON
#endif

std::string Editor_GetSurfacesJSON()
{
#ifndef __EMSCRIPTEN__
	// PICK_TEST native validation (see the definition below): the pick needs a
	// RENDERED frame (live ZPage16 / G-buffer / camera), and Snapshot.cpp's only
	// post-tick call into this file is the DUMP_SURFACES hook → this function.
	// So the check rides here: no-op unless PICK_TEST is set; fires once.
	editorRunPickTestHook();
	// Same convention: SPLIT_TEST=<surface> runs Editor_SplitInstances once
	// post-tick (needs live meshes/materials) and prints the result JSON.
	editorRunSplitTestHook();
	// CLEARMAP_TEST=<surface>:<role> resets a map override once post-tick.
	editorRunClearMapTestHook();
	// FOCUS_TEST=<surface[;surface…]> validates the click-to-focus framing.
	editorRunFocusTestHook();
	// OBJSCALE_TEST=<object>:<scale> validates the per-object scale knob.
	editorRunObjScaleTestHook();
	// LIGHTGROUP_TEST=<object> validates light→object grouping + group edit.
	editorRunLightGroupTestHook();
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
		// Per-material glass-refraction IOR override (0 = unset -> the global
		// glass_refract_ior). Editor slider on refractive surfaces; persists
		// via the sidecar ('refractIor' in SURF_SIDECAR_KEYS).
		appendNum(out, "refractIor",   M->RefractIor); out += ",";
		// Tri-state env-reflection probe override (-1 off / 0 auto / 1 on) —
		// editor 3-way control near the reflection slider; sidecar 'envRefl'.
		appendNum(out, "envRefl",      M->EnvReflMode); out += ",";
		// Per-surface env-probe bake face resolution (0 = unset -> global
		// env_bake_res chain) — 'probe res' select next to the env probe
		// control; sidecar 'envBakeRes'.
		appendNum(out, "envBakeRes",   M->EnvBakeRes); out += ",";
		// Authored dynamic-env-reflection flag (0/1; ENVDYN Workstream A1) —
		// editor checkbox near the env probe controls; RVSF sub-chunk bit 0x400
		// ('envDynamic' in SURF_SIDECAR_KEYS/RVSF_SURF_KEYS).
		appendNum(out, "envDynamic",   M->EnvDynamic); out += ",";
		// Procedural-water composite override, tri-state like envRefl (-1 off /
		// 0 auto→global --water_procedural / 1 on); sidecar 'waterProcedural'.
		// isWater marks the scene's registered water material (the only surface
		// the toggle means anything on) so the panel can show it there only.
		appendNum(out, "waterProcedural", M->WaterProcMode); out += ",";
		appendNum(out, "isWater",
		          (int)M->ID == fds::RenderPipeline::instance().deferredWaterMatID() ? 1 : 0);
		out += ",";
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

// Strip the static-bake mesh-splitting chunk suffix ("Piramid.lwo:c17" →
// "Piramid.lwo") so all chunks of one authored object collapse into a single
// editor entry. Greets' room is split into ~130 such chunk objects at init
// (MeshOps chunking); scenes without splitting have no ':c<N>' names.
std::string Editor_ChunkBaseObjName(const char *n)
{
	std::string s = n ? n : "";
	const size_t colon = s.rfind(":c");
	if (colon != std::string::npos && colon + 2 < s.size()) {
		bool digits = true;
		for (size_t i = colon + 2; i < s.size(); ++i)
			if (s[i] < '0' || s[i] > '9') { digits = false; break; }
		if (digits) s.resize(colon);
	}
	return s;
}

// Engine-generated helper meshes ("__mirrorClone_*" phantoms are skipped
// outright; "__discoBall" and the unnamed 2-face shard_refl_atlas* meshes are
// foldable) — anything the FLD didn't author under a real object name.
static bool editorObjNameIsEngine(const char *n)
{
	return !n || !n[0] || (n[0] == '_' && n[1] == '_');
}

// UNIFIED object hierarchy for every scene: PRIMARY source is the FLD OBJECT
// TREE (Obj->Name + resolved Parent links), with collapsing rules that tame
// the noisy trees left behind by init-time mesh surgery:
//   • ':c<N>' chunk suffixes collapse (greets' ~130 'Piramid.lwo:c<N>' static
//     bake chunks → one 'Piramid' object);
//   • instances dedupe by (chunk-collapsed) object name; each object belongs
//     to its ROOT ancestor's group (follow Obj->Parent up); roots with
//     several part names become a "<stem> (model)" entry with one child per
//     part (city trains, the greets mech's 'mech  null' root), single-part
//     roots list their surfaces directly (ships, taxis, buildings);
//   • "__mirrorClone_*" phantoms and face-less helpers (camera targets) are
//     skipped;
//   • unnamed/engine-generated meshes (obj name empty or '__*') fold into the
//     single named object that already carries ALL their surfaces when that
//     is unambiguous; a NAMED code mesh ('__discoBall') whose surfaces no
//     authored part carries is genuinely selectable (faces + real materials)
//     and gets its own VISIBLE entry (name minus the '__') — hiding it in
//     the engine bucket made the disco ball unreachable; the rest (unnamed
//     shard atlases, ambiguous helpers) pool into one trailing '(engine)'
//     bucket the shell hides by default ("engine":1);
//   • ENRICHMENT: the clone-file material naming ("Hull.lwo::hull" — the
//     robot-clone scheme that gives greets' multi-mesh mech per-part
//     materials) is merged in, not discarded: a part whose object name
//     matches the family file gains the family's full surface set (covers
//     zero-face clone materials like 'Hull2.lwo::hull' that dedup onto
//     another file's Material), and families with no matching tree object
//     get their own entry.
// Output: [{name, obj, meshes, surfaces[, children:[{name, obj, surfaces}]]
//           [, engine:1]}] — `children` optional (the shell treats a missing
// array as a leaf and lists the surfaces); `obj` is the raw (chunk-collapsed)
// engine object name — the key Editor_SetObjectScale takes.
std::string Editor_GetObjectsJSON()
{
	if (!CurScene) return "[]";
	struct Part { std::set<std::string> surfaces; long meshes = 0; };
	std::map<std::string, std::map<std::string, Part>> roots;  // root → part → info
	std::vector<std::string> rootOrder;                        // scene file order
	struct EngMesh { std::string name; std::set<std::string> surfs; };
	std::vector<EngMesh> engineMeshes;                         // foldable helpers
	long engineBucketMeshes = 0;
	for (Object *Obj = CurScene->ObjectHead; Obj; Obj = Obj->Next) {
		if (Obj->Type != Obj_TriMesh || !Obj->Data) continue;
		if (Obj->Name && !std::strncmp(Obj->Name, "__mirrorClone_", 14)) continue;
		TriMesh *T = (TriMesh *)Obj->Data;
		if (!T->FIndex) continue;                   // camera targets, helpers
		std::set<std::string> surfs;
		for (DWord f = 0; f < T->FIndex; ++f)
			if (T->Faces[f].Txtr && T->Faces[f].Txtr->Name)
				surfs.insert(Editor_BaseSurfName(T->Faces[f].Txtr->Name));
		if (editorObjNameIsEngine(Obj->Name)) {
			if (!surfs.empty())
				engineMeshes.push_back({ Obj->Name ? Obj->Name : "",
				                         std::move(surfs) });
			continue;
		}
		const Object *R = Obj;                      // root ancestor (cycle-guarded)
		for (int hops = 0; R->Parent && hops < 64; ++hops) R = R->Parent;
		const std::string rootName = Editor_ChunkBaseObjName(
			(R->Name && R->Name[0]) ? R->Name : Obj->Name);
		if (!roots.count(rootName)) rootOrder.push_back(rootName);
		Part &P = roots[rootName][Editor_ChunkBaseObjName(Obj->Name)];
		++P.meshes;
		P.surfaces.insert(surfs.begin(), surfs.end());
	}
	// Clone-family ENRICHMENT: file → its material surfaces (dedup'd base
	// names). Merged into the matching tree part; leftover families become
	// their own entries (the tree may lack an object for a naming-only file).
	{
		std::map<std::string, std::set<std::string>> families;
		for (Material* M = MatLib; M; M = M->Next) {
			if (M->RelScene != CurScene || !M->Name) continue;
			const std::string base = Editor_BaseSurfName(M->Name);
			const size_t sep = base.find("::");
			if (sep == std::string::npos) continue;   // plain surface — not a model part
			families[base.substr(0, sep)].insert(base);
		}
		for (auto& [file, surfs] : families) {
			Part *hit = nullptr;
			for (auto& [rootName, parts] : roots) {
				auto it = parts.find(file);
				if (it != parts.end()) { hit = &it->second; break; }
			}
			if (hit) {
				hit->surfaces.insert(surfs.begin(), surfs.end());
			} else {
				if (!roots.count(file)) rootOrder.push_back(file);
				Part &P = roots[file][file];
				P.surfaces.insert(surfs.begin(), surfs.end());
			}
		}
	}
	// Fold engine helpers: a helper whose surfaces are ALL carried by exactly
	// one named part belongs to that part. Of the rest, a NAMED code mesh
	// ('__discoBall') is a real, selectable object the engine built at init —
	// it gets its own visible entry (dedup'd by name, '__' stripped for
	// display) so its surfaces stay clickable; only unnamed/ambiguous helpers
	// pool into the trailing '(engine)' bucket (hidden by default).
	std::set<std::string> engineBucket;
	struct Promoted { std::set<std::string> surfaces; long meshes = 0; };
	std::map<std::string, Promoted> promoted;   // raw '__name' → entry
	std::vector<std::string> promotedOrder;
	for (const EngMesh &em : engineMeshes) {
		Part *owner = nullptr;
		int owners = 0;
		for (auto& [rootName, parts] : roots)
			for (auto& [partName, P] : parts) {
				bool allIn = true;
				for (const std::string &s : em.surfs)
					if (!P.surfaces.count(s)) { allIn = false; break; }
				if (allIn) { ++owners; owner = &P; }
			}
		if (owners == 1) { ++owner->meshes; continue; }  // surfaces already listed there
		if (em.name.size() > 2) {                        // named: "__" + something
			if (!promoted.count(em.name)) promotedOrder.push_back(em.name);
			Promoted &P = promoted[em.name];
			++P.meshes;
			P.surfaces.insert(em.surfs.begin(), em.surfs.end());
			continue;
		}
		engineBucket.insert(em.surfs.begin(), em.surfs.end());
		++engineBucketMeshes;
	}
	if (rootOrder.empty() && promoted.empty() && engineBucket.empty()) {
		// Diagnostic for the wasm editor's "objects panel empty" reports: say
		// WHY the tree came back empty while surfaces are enumerable (visible
		// in the browser console via stderr).
		long mats = 0, objs = 0, tri = 0, named = 0, faced = 0;
		for (Material* M = MatLib; M; M = M->Next)
			if (M->RelScene == CurScene && M->Name) ++mats;
		for (Object *Obj = CurScene->ObjectHead; Obj; Obj = Obj->Next) {
			++objs;
			if (Obj->Type != Obj_TriMesh || !Obj->Data) continue;
			++tri;
			if (Obj->Name && Obj->Name[0]) ++named;
			if (((TriMesh *)Obj->Data)->FIndex) ++faced;
		}
		std::fprintf(stderr, "[EDITOR] objects: EMPTY tree (scene=%p mats=%ld "
		             "objects=%ld trimesh=%ld named=%ld withFaces=%ld) — scene "
		             "still initializing?\n", (void*)CurScene, mats, objs, tri,
		             named, faced);
		return "[]";
	}
	auto stem = [](const std::string &n) {
		const size_t dot = n.find_last_of('.');
		return dot == std::string::npos ? n : n.substr(0, dot);
	};
	std::string out = "[";
	bool firstObj = true;
	for (const std::string &rootName : rootOrder) {
		auto &parts = roots[rootName];
		long meshes = 0;
		std::set<std::string> all;
		for (auto& [part, P] : parts) {
			meshes += P.meshes;
			all.insert(P.surfaces.begin(), P.surfaces.end());
		}
		if (all.empty()) continue;
		if (!firstObj) out += ",";
		firstObj = false;
		const bool multi = parts.size() > 1;
		out += "{\"name\":\"";
		jsonEscape(out, (multi ? stem(rootName) + " (model)" : stem(rootName)).c_str());
		out += "\",\"obj\":\"";
		jsonEscape(out, rootName.c_str());
		out += "\",";
		char buf[64];
		std::snprintf(buf, sizeof buf, "\"meshes\":%ld,\"scale\":%.4g,", meshes,
		              fds::ObjectImport_GetObjectScale(CurScene, rootName.c_str()));
		out += buf;
		appendSurfArray(out, all);
		if (multi) {
			out += ",\"children\":[";
			bool firstKid = true;
			for (auto& [part, P] : parts) {
				if (!firstKid) out += ",";
				firstKid = false;
				out += "{\"name\":\"";
				jsonEscape(out, part.c_str());
				out += "\",\"obj\":\"";
				jsonEscape(out, part.c_str());
				out += "\",";
				std::snprintf(buf, sizeof buf, "\"scale\":%.4g,",
				              fds::ObjectImport_GetObjectScale(CurScene, part.c_str()));
				out += buf;
				appendSurfArray(out, P.surfaces);
				out += "}";
			}
			out += "]";
		}
		out += "}";
	}
	// Promoted code meshes ('__discoBall' → "discoBall"): visible leaf entries.
	// `obj` keeps the RAW engine name — Editor_SetObjectScale matches meshes by
	// chunk-collapsed Obj->Name, so the scale knob keys on it (Tri_Possessed
	// meshes report themselves live-unscalable on stderr, which is honest).
	for (const std::string &nm : promotedOrder) {
		const Promoted &P = promoted[nm];
		if (P.surfaces.empty()) continue;
		if (!firstObj) out += ",";
		firstObj = false;
		out += "{\"name\":\"";
		jsonEscape(out, nm.substr(2).c_str());
		out += "\",\"obj\":\"";
		jsonEscape(out, nm.c_str());
		out += "\",";
		char buf[64];
		std::snprintf(buf, sizeof buf, "\"meshes\":%ld,\"scale\":%.4g,", P.meshes,
		              fds::ObjectImport_GetObjectScale(CurScene, nm.c_str()));
		out += buf;
		appendSurfArray(out, P.surfaces);
		out += "}";
	}
	if (!engineBucket.empty()) {
		if (!firstObj) out += ",";
		firstObj = false;
		out += "{\"name\":\"(engine)\",\"obj\":\"\",\"engine\":1,";
		char buf[48];
		std::snprintf(buf, sizeof buf, "\"meshes\":%ld,", engineBucketMeshes);
		out += buf;
		appendSurfArray(out, engineBucket);
		out += "}";
	}
	out += "]";
	return out;
}

// LIVE per-object uniform scale (the objects-panel knob). Thin wrapper over
// ObjectImport_SetObjectScale on the rendered scene: sets every instance of
// the (chunk-collapsed) object name; Animate_Objects folds the multiplier in
// on the next tick, pivoting on the object pivot and composing into children
// (a model root scales the whole assembly). Returns the LIVE mesh count set
// (0 = no such object / fully static-baked). Persistence is the server's
// 'obj:<name>|scale|v' sidecar line — see MaterialImport.h.
int Editor_SetObjectScale(const char *objName, float scale)
{
	const int n = fds::ObjectImport_SetObjectScale(CurScene, objName, scale);
	if (n > 0) Editor_MarkDirty();
	std::fprintf(stderr, "[EDITOR] scale '%s' = %.4g -> %d live mesh(es)\n",
	             objName ? objName : "", scale, n);
	return n;
}

// TARGETED env-probe invalidation for a single edited surface. Drops ONLY
// this surface's baked probe store(s) — its base material, ::mirUV clone, and
// any co-located surface sharing the same store — so the next FramePrep
// re-bakes just that one probe instead of every reflective surface in the
// scene. That whole-scene re-bake (the old EnvReflection_Invalidate here) is
// what OOM'd the 4GB wasm editor: greets now carries many reflective surfaces
// and re-baking them ALL at once spiked past the ceiling ("memory access out
// of bounds"). Idempotent across a surface's clones (they collapse to one
// store drop). The surface's OWN probe still refreshes, so the "metallic has
// no effect" fix this invalidate originally provided is preserved.
static void editorInvalidateSurfaceEnv(const char* surface)
{
	if (!surface || !*surface || !CurScene) return;
	for (Material* M = MatLib; M; M = M->Next)
		if (M->RelScene == CurScene && M->Name &&
		    Editor_BaseSurfName(M->Name) == surface)
			fds::EnvReflection_InvalidateSurface(CurScene, M);
}

bool Editor_SetSurfaceProp(const char* name, const char* key, float value)
{
	// Shared setter (also used by the sidecar's numeric prop lines): sets on
	// every CurScene material whose base name matches, ::mirUV clones included.
	const bool any = fds::MaterialImport_SetSurfaceProp(CurScene, name, key, value);
	// envRefl flips which materials bake/publish env probes, envBakeRes their
	// bake size — same invalidation as the metallic import/reset paths
	// (Editor_ImportTexture) so the next frame's FramePrep re-bakes/drops
	// probes under the new rule. TARGETED to just this surface's store(s): a
	// per-surface edit must not force a whole-scene re-bake (the wasm OOM). The
	// sidecar's scene-INIT apply goes straight to MaterialImport_SetSurfaceProp
	// (nothing baked yet), so hooking the live-editor path here is sufficient.
	// envDynamic (ENVDYN A1) also changes what the store must retain (A2's
	// static Z + colour master) — drop the probe so FramePrep re-bakes it
	// under the new retention rule.
	if (any && (!std::strcmp(key, "envRefl") || !std::strcmp(key, "envBakeRes")
	            || !std::strcmp(key, "envDynamic")))
		editorInvalidateSurfaceEnv(name);
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

// GreetsMirror's BuildMirror clones every mesh as an "__mirrorClone_*" object
// whose faces REFERENCE THE ORIGINAL MATERIALS at positions mirrored behind
// the wall. Any editor pass that gathers faces by material must skip those
// meshes (same exclusion the pick fallback below and MainLoop's focus
// clustering use) or the phantom geometry contaminates the result.
static int editorCollectMirrorCloneMeshes(const TriMesh **buf, int max)
{
	int n = 0;
	if (!CurScene) return 0;
	for (Object *Obj = CurScene->ObjectHead; Obj; Obj = Obj->Next) {
		if (Obj->Type != Obj_TriMesh || !Obj->Name || !Obj->Data) continue;
		if (std::strncmp(Obj->Name, "__mirrorClone_", 14) != 0) continue;
		if (n < max) buf[n++] = (const TriMesh *)Obj->Data;
	}
	return n;
}

// JSON result helper for Editor_SplitInstances: {"clusters":C,"faces":F,
// "names":[...],"centroids":{name:[x,y,z]}} — names empty when nothing was
// split, so the UI can say WHY (C==1: all faces form one spatial cluster;
// C==0: surface not found). centroids = per-part WORLD-space face-centroid
// means; the editor ships them with the save payload so the server's split
// BAKE (lwopatch split_surface) can match its LWO polygon clusters to the
// live "#k" parts geometrically — engine face order differs from LWO poly
// order (init chunking/surgery reorders), so order-based matching would
// swap identical-size parts (the two-mummies tie).
static std::string splitResultJson(long clusters, long faces,
                                   const std::set<std::string> &names,
                                   const std::map<std::string, Vector> *centroids = nullptr)
{
	std::string out;
	char buf[96];
	std::snprintf(buf, sizeof buf, "{\"clusters\":%ld,\"faces\":%ld,\"names\":[", clusters, faces);
	out += buf;
	bool first = true;
	for (const std::string &nn : names) {
		if (!first) out += ",";
		first = false;
		out += "\"";
		jsonEscape(out, nn.c_str());
		out += "\"";
	}
	out += "]";
	if (centroids && !centroids->empty()) {
		out += ",\"centroids\":{";
		first = true;
		for (const auto &kv : *centroids) {
			if (!first) out += ",";
			first = false;
			out += "\"";
			jsonEscape(out, kv.first.c_str());
			std::snprintf(buf, sizeof buf, "\":[%.3f,%.3f,%.3f]",
			              kv.second.x, kv.second.y, kv.second.z);
			out += buf;
		}
		out += "}";
	}
	out += "}";
	return out;
}

std::string Editor_SplitInstances(const char* name)
{
	const std::set<std::string> none;
	if (!CurScene || !name || !*name) return splitResultJson(0, 0, none);
	// Mirror-clone meshes duplicate every face of this surface (same
	// Material*) MIRRORED BEHIND the wall: clustering them inflates the union
	// bbox (so R blows up) and chain-links the real instances through the
	// mirror into one blob — the "momy has nothing to split" regression with
	// mirrors ON. Skip their faces entirely; the clones keep rendering the
	// primary's material either way (they reference it by pointer).
	const TriMesh *cloneMeshes[64];
	const int cloneCount = editorCollectMirrorCloneMeshes(cloneMeshes, 64);
	long cloneFacesSkipped = 0;
	// Faces of this surface (any of its materials — base + ::mirUV clones),
	// with world-space centres.
	struct FRef { Face* F; float c[3]; };
	std::vector<FRef> refs;
	for (TriMesh* T = CurScene->TriMeshHead; T; T = T->Next) {
		bool isClone = false;
		for (int c = 0; c < cloneCount; ++c)
			if (cloneMeshes[c] == T) { isClone = true; break; }
		for (DWord i = 0; i < T->FIndex; ++i) {
			Face& F = T->Faces[i];
			if (!F.Txtr || !F.Txtr->Name || Editor_BaseSurfName(F.Txtr->Name) != name) continue;
			if (isClone) { ++cloneFacesSkipped; continue; }
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
	}
	if (cloneFacesSkipped)
		std::fprintf(stderr, "[EDITOR] split '%s': skipped %ld mirror-clone faces (%d clone meshes)\n",
		             name, cloneFacesSkipped, cloneCount);
	if (refs.size() < 2) return splitResultJson(refs.empty() ? 0 : 1, long(refs.size()), none);

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
	if (clusterSize.size() < 2) {
		std::fprintf(stderr, "[EDITOR] split '%s': %zu faces form 1 spatial cluster — nothing to split\n",
		             name, n);
		return splitResultJson(1, long(n), none);
	}
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
	// Symmetric naming (user request): the primary cluster does NOT keep the
	// bare name — its materials are renamed "<name>#1" (::mirUV suffix kept
	// OUTSIDE the "#k", as for the clones) so split parts read as siblings
	// (momy#1 / momy#2 / …). Saves still collapse: the server strips the whole
	// trailing (#k)+ chain back to the base surface (split_surface_sidecar_keys
	// and the FLD/UV patchers), so "#1" lands on "momy" like every other part.
	// The primary keeps the ORIGINAL Material* (mirror-clone faces reference it
	// by pointer, so the mirror keeps rendering the primary's look).
	static const std::string mirSuf2 = "::mirUV";
	for (Material* M = MatLib; M; M = M->Next) {
		if (M->RelScene != CurScene || !M->Name) continue;
		if (Editor_BaseSurfName(M->Name) != name) continue;
		const std::string old = M->Name;
		const bool isMir = old.size() > mirSuf2.size() &&
		                   old.compare(old.size() - mirSuf2.size(), mirSuf2.size(), mirSuf2) == 0;
		char nm[220];
		std::snprintf(nm, sizeof nm, "%s#1%s", name, isMir ? mirSuf2.c_str() : "");
		M->Name = strdup(nm);   // old name leaked — runtime edit, engine convention
	}
	// New base names for the caller (primary "#1" + dedup'd cluster clones).
	std::set<std::string> newNames;
	{
		char p1[200];
		std::snprintf(p1, sizeof p1, "%s#1", name);
		newNames.insert(p1);
	}
	for (auto& [key, C] : clones) newNames.insert(Editor_BaseSurfName(C->Name));
	// Per-part world centroids (face-centroid means) for the save-time bake's
	// geometric live↔LWO cluster matching (see splitResultJson).
	std::map<std::string, Vector> partCentroids;
	{
		std::map<int, Vector> sum;
		std::map<int, long>   cnt;
		for (size_t i = 0; i < n; ++i) {
			const int root = find(int(i));
			Vector &s = sum[root];
			s.x += refs[i].c[0]; s.y += refs[i].c[1]; s.z += refs[i].c[2];
			++cnt[root];
		}
		for (auto& [root, s] : sum) {
			char pn[220];
			std::snprintf(pn, sizeof pn, "%s#%d", name,
			              root == primary ? 1 : clusterK[root]);
			Vector c = { s.x / cnt[root], s.y / cnt[root], s.z / cnt[root] };
			partCentroids[pn] = c;
		}
	}
	const std::string out = splitResultJson(long(clusterSize.size()), long(n), newNames,
	                                        &partCentroids);
	// New MatLib entries → matIDs + table (post-load materials are invisible
	// to the deferred kernel until the table is rebuilt — the greets mirror
	// "yellow tint" lesson).
	Scene_RebuildMatTable(CurScene);
	Editor_MarkDirty();
	std::fprintf(stderr, "[EDITOR] split '%s': %zu faces, %zu clusters (primary %ld faces) -> %s\n",
	             name, n, clusterSize.size(), primarySize, out.c_str());
	return out;
}

#ifndef __EMSCRIPTEN__
// SPLIT_TEST=<surface> — native validation for the instance split (same env-
// hook convention as PICK_TEST below): runs Editor_SplitInstances once, post-
// tick (live meshes/materials), and prints the result JSON. Run with
// --greets_mirror to exercise the mirror-clone exclusion (the regression this
// verifies), e.g.:
//   SPLIT_TEST=momy DUMP_SURFACES=1 ./DEMO --deferred --greets-mirror \
//     --snapshot=greets@t=600
static void editorRunSplitTestHook()
{
	const char *spec = std::getenv("SPLIT_TEST");
	if (!spec || !*spec) return;
	static bool done = false;
	if (done) return;
	done = true;
	const std::string r = Editor_SplitInstances(spec);
	std::fprintf(stderr, "[SPLITTEST] split '%s' -> %s\n", spec, r.c_str());
}

// CLEARMAP_TEST=<surface>:<role> — native validation for the editor "reset
// map" (same env-hook convention as PICK_TEST/SPLIT_TEST): reset the role
// once, post-tick, and print the result. Pair with a sidecar map line to
// verify the frame reverts to the no-override baseline byte-for-byte.
static void editorRunClearMapTestHook()
{
	const char *spec = std::getenv("CLEARMAP_TEST");
	if (!spec || !*spec) return;
	static bool done = false;
	if (done) return;
	done = true;
	std::string s = spec;
	const size_t colon = s.rfind(':');
	if (colon == std::string::npos) {
		std::fprintf(stderr, "[CLEARTEST] bad spec '%s' (want surface:role)\n", spec);
		return;
	}
	const std::string surf = s.substr(0, colon), role = s.substr(colon + 1);
	const bool ok = Editor_ClearMap(surf.c_str(), role.c_str());
	std::fprintf(stderr, "[CLEARTEST] reset '%s' %s -> %s\n",
	             surf.c_str(), role.c_str(), ok ? "ok" : "FAILED");
}
#endif

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

// Each FLD light is wrapped in an Object node at load (FLD_CONV.CPP AddOmni:
// Type=Obj_Omni, Data=the Omni, Name=the authored light name, ParentID
// resolved to Obj->Parent) — the SAME tree the object hierarchy uses, so a
// light's owning object ("bilding flare" → b2.lwo) is real authored data,
// not a heuristic. Code-created omnis (camera light, bounce cones, mirror
// clones) have no wrapper and resolve to null.
static const Object *editorOmniWrapper(const Omni *O)
{
	if (!CurScene) return nullptr;
	for (Object *Obj = CurScene->ObjectHead; Obj; Obj = Obj->Next)
		if (Obj->Type == Obj_Omni && Obj->Data == (const void *)O) return Obj;
	return nullptr;
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
// plus the authored name ("bilding flare", "" when the FLD carries none) and
// parent — the owning OBJECT's chunk-collapsed name ("" = unparented), which
// is what the shell's light grouping and LIGHTGROUP_TEST key on.
std::string Editor_GetLightsJSON()
{
	std::string out = "[";
	int i = 0, raw = 0;
	if (CurScene) for (Omni *O = CurScene->OmniHead; O; O = O->Next) {
		if (!(O->Flags & Omni_SceneAuthored)) continue;
		const int rawIdx = raw++;               // editorFocusLight's index space
		if (!isAuthoredNonCloneOmni(O)) continue;
		const Object *W = editorOmniWrapper(O);
		std::string name, parent;
		if (W && W->Name) name = W->Name;
		if (W && W->Parent && W->Parent->Name)
			parent = Editor_ChunkBaseObjName(W->Parent->Name);
		char buf[380];
		std::snprintf(buf, sizeof buf,
		  "%s{\"i\":%d,\"rawI\":%d,\"r\":%.0f,\"g\":%.0f,\"b\":%.0f,"
		  "\"intensity\":%.4g,\"range\":%.4g,\"flareScale\":%.4g,"
		  "\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,"
		  "\"type\":%d,\"shadow\":%d,\"posKeys\":%u,\"sizeKeys\":%u,\"rangeKeys\":%u,",
		  i ? "," : "", i, rawIdx, O->L.R, O->L.G, O->L.B,
		  O->Size.NumKeys ? O->Size.Keys[0].Pos.x : 0.0f,
		  O->Range.NumKeys ? O->Range.Keys[0].Pos.x : 0.0f,
		  O->FlareScale > 0.0f ? O->FlareScale : 1.0f,
		  O->IPos.x, O->IPos.y, O->IPos.z,
		  int(O->Type), (O->Flags & Omni_CastsShadow) ? 1 : 0,
		  (unsigned)O->Pos.NumKeys, (unsigned)O->Size.NumKeys, (unsigned)O->Range.NumKeys);
		out += buf;
		out += "\"name\":\"";
		jsonEscape(out, name.c_str());
		out += "\",\"parent\":\"";
		jsonEscape(out, parent.c_str());
		out += "\"}";
		++i;
	}
	out += "]";
	return out;
}

// Authored-light indices belonging to `objName` (chunk-collapsed object name,
// ancestors included — a light parented to a train wagon groups under the
// train root too). Shared by LIGHTGROUP_TEST; the shell groups by the JSON's
// 'parent' field directly.
static int editorLightsOfObject(const char *objName, int *out, int max)
{
	int n = 0, i = 0;
	if (!CurScene || !objName || !*objName) return 0;
	for (Omni *O = CurScene->OmniHead; O; O = O->Next) {
		if (!isAuthoredNonCloneOmni(O)) continue;
		const int idx = i++;
		const Object *W = editorOmniWrapper(O);
		for (const Object *A = W ? W->Parent : nullptr; A; A = A->Parent)
			if (A->Name && Editor_ChunkBaseObjName(A->Name) == objName) {
				if (n < max) out[n] = idx;
				++n;
				break;
			}
	}
	return n;
}

#ifndef __EMSCRIPTEN__
// LIGHTGROUP_TEST=<object> — native validation for light→object grouping +
// group edit (PICK_TEST convention; rides the snapshot loops' DUMP_SURFACES
// hook). Prints the resolved authored-light indices for the object, then
// runs the group-edit smoke: r=255 on every member via Editor_SetLightProp
// and asserts Editor_GetLightsJSON reflects it on all of them. e.g.:
//   LIGHTGROUP_TEST="b2.lwo" DUMP_SURFACES=1 ./DEMO --deferred \
//     --snapshot=city@t=300
static void editorRunLightGroupTestHook()
{
	const char *spec = std::getenv("LIGHTGROUP_TEST");
	if (!spec || !*spec || !CurScene) return;
	static bool done = false;
	if (done) return;
	done = true;
	int idx[256];
	const int n = editorLightsOfObject(spec, idx, 256);
	std::string list;
	for (int k = 0; k < n && k < 256; ++k) {
		if (!list.empty()) list += ",";
		list += std::to_string(idx[k]);
	}
	std::fprintf(stderr, "[LIGHTGROUP] '%s': %d light(s): [%s]\n", spec, n, list.c_str());
	if (!n) return;
	for (int k = 0; k < n && k < 256; ++k)
		if (!Editor_SetLightProp(idx[k], "r", 255.0f))
			std::fprintf(stderr, "[LIGHTGROUP] set r on light %d FAILED\n", idx[k]);
	const std::string lj = Editor_GetLightsJSON();
	int ok = 0;
	for (int k = 0; k < n && k < 256; ++k) {
		char probe[32];
		std::snprintf(probe, sizeof probe, "{\"i\":%d,", idx[k]);
		const size_t at = lj.find(probe);
		bool good = false;
		if (at != std::string::npos) {
			const size_t rAt = lj.find("\"r\":", at);
			const size_t end = lj.find('}', at);
			if (rAt != std::string::npos && rAt < end)
				good = std::atoi(lj.c_str() + rAt + 4) == 255;
		}
		if (good) ++ok;
		else std::fprintf(stderr, "[LIGHTGROUP] light %d: r != 255 after group edit\n", idx[k]);
	}
	std::fprintf(stderr, "[LIGHTGROUP] group edit r=255: %d/%d reflected — %s\n",
	             ok, n, ok == n ? "PASS" : "FAIL");
}
#endif // !__EMSCRIPTEN__

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
	const int cloneCount = editorCollectMirrorCloneMeshes(cloneMeshes, 64);
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

// ── Focus framing core (shared native/wasm) ─────────────────────────────────
// World-space centre + bounding radius for a ';'-separated surface-name
// selection: gather every matching face's world bbox (base-name match, mirror-
// clone meshes excluded), then single-linkage-cluster from the face nearest
// `nearPos` and frame only that cluster. A surface like "lamp" — or a multi-
// instance city object like the eight taxis — appears all over the scene;
// framing the union parked the camera "from afar" over a mid-air pivot
// (nothing near it → the object read as isolated and the orbit felt broken).
// Clustering applies to GROUP selections too now: a connected model's parts
// sit within R of each other so its focus still covers the whole model (the
// greets mech), while scattered instances resolve to the nearest one.
// MainLoop.cpp's editorFocusSurface (wasm) and the FOCUS_TEST native hook both
// place the camera from this: eye = centre − viewDir·max(2.5·radius, 6), which
// centres the object at ~1/3 of the view WITHOUT hiding the rest of the scene.
bool Editor_ComputeFocus(const char *names, const Vector &nearPos,
                         Vector &outCenter, float &outRadius,
                         long *outUsedFaces, unsigned long *outTotalFaces)
{
	if (outUsedFaces)  *outUsedFaces = 0;
	if (outTotalFaces) *outTotalFaces = 0;
	if (!CurScene || !names || !*names) return false;
	std::vector<std::string> wants;
	const std::string all = names;
	for (size_t pos = 0; pos <= all.size(); ) {
		size_t semi = all.find(';', pos);
		if (semi == std::string::npos) semi = all.size();
		if (semi > pos) wants.push_back(all.substr(pos, semi - pos));
		pos = semi + 1;
	}
	const TriMesh *cloneMeshes[64];
	const int cloneCount = editorCollectMirrorCloneMeshes(cloneMeshes, 64);
	struct FBox { float lo[3], hi[3], c[3]; };
	std::vector<FBox> boxes;
	for (TriMesh *T = CurScene->TriMeshHead; T; T = T->Next) {
		bool isClone = false;
		for (int c = 0; c < cloneCount; ++c)
			if (cloneMeshes[c] == T) { isClone = true; break; }
		if (isClone) continue;
		for (DWord i = 0; i < T->FIndex; ++i) {
			Face &F = T->Faces[i];
			// Base-name match: floor's faces reference the "floor::mirUV"
			// handedness clone, so an exact compare finds no faces at all.
			if (!F.Txtr || !F.Txtr->Name) continue;
			const std::string base = Editor_BaseSurfName(F.Txtr->Name);
			if (std::find(wants.begin(), wants.end(), base) == wants.end()) continue;
			FBox b = { { 1e30f, 1e30f, 1e30f }, { -1e30f, -1e30f, -1e30f }, { 0, 0, 0 } };
			Vertex *vs[3] = { F.A, F.B, F.C };
			int nv = 0;
			for (int k = 0; k < 3; ++k) {
				Vertex *v = vs[k]; if (!v) continue;
				Vector w; MatrixXVector(T->RotMat, &v->Pos, &w); Vector_SelfAdd(&w, &T->IPos);
				const float p[3] = { w.x, w.y, w.z };
				for (int a = 0; a < 3; ++a) {
					if (p[a] < b.lo[a]) b.lo[a] = p[a];
					if (p[a] > b.hi[a]) b.hi[a] = p[a];
					b.c[a] += p[a];
				}
				++nv;
			}
			if (!nv) continue;
			for (int a = 0; a < 3; ++a) b.c[a] /= float(nv);
			boxes.push_back(b);
		}
	}
	if (boxes.empty()) return false;
	// Union bbox — the cluster threshold scale.
	float ulo[3] = { 1e30f, 1e30f, 1e30f }, uhi[3] = { -1e30f, -1e30f, -1e30f };
	for (const FBox &b : boxes)
		for (int a = 0; a < 3; ++a) {
			if (b.lo[a] < ulo[a]) ulo[a] = b.lo[a];
			if (b.hi[a] > uhi[a]) uhi[a] = b.hi[a];
		}
	// Single-linkage growth from the face nearest `nearPos`: include any face
	// whose centre is within R of the growing cluster bbox. R scales with the
	// union diagonal so far-apart instances stay separate.
	const float ud[3] = { uhi[0]-ulo[0], uhi[1]-ulo[1], uhi[2]-ulo[2] };
	const float uDiag = std::sqrt(ud[0]*ud[0] + ud[1]*ud[1] + ud[2]*ud[2]);
	const float R = std::max(uDiag * 0.15f, 2.0f), R2 = R * R;
	size_t seed = 0; float bestD = 1e30f;
	const float cam[3] = { nearPos.x, nearPos.y, nearPos.z };
	for (size_t i = 0; i < boxes.size(); ++i) {
		const float d0 = boxes[i].c[0]-cam[0], d1 = boxes[i].c[1]-cam[1], d2 = boxes[i].c[2]-cam[2];
		const float d = d0*d0 + d1*d1 + d2*d2;
		if (d < bestD) { bestD = d; seed = i; }
	}
	std::vector<char> in(boxes.size(), 0);
	in[seed] = 1;
	float lo[3], hi[3];
	for (int a = 0; a < 3; ++a) { lo[a] = boxes[seed].lo[a]; hi[a] = boxes[seed].hi[a]; }
	for (bool grew = true; grew; ) {
		grew = false;
		for (size_t i = 0; i < boxes.size(); ++i) {
			if (in[i]) continue;
			float d2sum = 0.0f;
			for (int a = 0; a < 3; ++a) {
				const float v = boxes[i].c[a] < lo[a] ? lo[a] - boxes[i].c[a]
				              : boxes[i].c[a] > hi[a] ? boxes[i].c[a] - hi[a] : 0.0f;
				d2sum += v * v;
			}
			if (d2sum > R2) continue;
			in[i] = 1; grew = true;
			for (int a = 0; a < 3; ++a) {
				if (boxes[i].lo[a] < lo[a]) lo[a] = boxes[i].lo[a];
				if (boxes[i].hi[a] > hi[a]) hi[a] = boxes[i].hi[a];
			}
		}
	}
	outCenter.x = (lo[0] + hi[0]) * 0.5f;
	outCenter.y = (lo[1] + hi[1]) * 0.5f;
	outCenter.z = (lo[2] + hi[2]) * 0.5f;
	const float dx = hi[0]-lo[0], dy = hi[1]-lo[1], dz = hi[2]-lo[2];
	outRadius = 0.5f * std::sqrt(dx*dx + dy*dy + dz*dz);
	if (outUsedFaces)  *outUsedFaces = long(std::count(in.begin(), in.end(), 1));
	if (outTotalFaces) *outTotalFaces = (unsigned long)boxes.size();
	return true;
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

// FOCUS_TEST=<surface[;surface;…]> — native validation for the click-to-focus
// framing (PICK_TEST convention; rides Editor_GetSurfacesJSON under the
// snapshot loops' DUMP_SURFACES hook). Runs the shared Editor_ComputeFocus
// core on the live scene, places the camera exactly the way MainLoop.cpp's
// editorFocusSurface does — eye = centre − viewDir·max(2.5·radius, 6),
// looking at the centre, approaching from the direction the camera already
// was — then RE-RENDERS the frame from that pose (the CITYSNAP_POS recipe:
// geometry only, no post passes) so the snapshot PPM written after this hook
// shows the focused view IN CONTEXT. Asserts (a) the object centre projects
// to the screen centre and (b) the camera sits OUTSIDE the bounding radius.
// Fires once.
static void editorRunFocusTestHook()
{
	const char *spec = std::getenv("FOCUS_TEST");
	if (!spec || !View) return;
	static bool done = false;
	if (done) return;
	done = true;
	Vector c; float r = 0; long used = 0; unsigned long total = 0;
	if (!Editor_ComputeFocus(spec, View->ISource, c, r, &used, &total)) {
		std::fprintf(stderr, "[FOCUSTEST] '%s': no faces — FAIL\n", spec);
		return;
	}
	const float dist = r * 2.5f < 6.0f ? 6.0f : r * 2.5f;
	Vector dir(c.x - View->ISource.x, c.y - View->ISource.y, c.z - View->ISource.z);
	const float len = std::sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
	if (len > 1e-4f) { dir.x /= len; dir.y /= len; dir.z /= len; }
	else             { dir.x = 0.0f; dir.y = 0.0f; dir.z = 1.0f; }
	FC.ISource = Vector(c.x - dir.x * dist, c.y - dir.y * dist, c.z - dir.z * dist);
	Vector look = c;
	Kick_Camera(&FC.ISource, &look, 0.0f, FC.Mat);
	if (FC.IFOV <= 0.0f) FC.IFOV = (View->IFOV > 0.0f) ? View->IFOV : 65.0f;
	CalcPersp(&FC);
	View = &FC;
	FOVX = View->PerspX;
	FOVY = View->PerspY;
	// Re-render from the focused pose — enough to SEE whether the scene
	// around the object stays visible in the written snapshot.
	if (VPage && ZPage16 && CurScene) {
		std::memset(VPage, 0, PageSize);
		std::memset(ZPage16, 0, size_t(XRes) * size_t(YRes) * sizeof(word));
		Transform_Objects(CurScene, fds::g_mainCamera, fds::g_mainFaces);
		if (CAll) { Radix_Sort(FList, SList, CAll); Render(); }
	}
	// Assertions: projection of the focus centre + camera outside the radius.
	Vector rel, vp;
	Vector_Sub(&c, &View->ISource, &rel);
	MatrixXVector(View->Mat, &rel, &vp);
	float spx = -1.0f, spy = -1.0f;
	const bool inFront = vp.z > 0.05f;
	if (inFront) {
		spx = CntrEX + vp.x / vp.z * FOVX;
		spy = CntrEY - vp.y / vp.z * FOVY;
	}
	const bool centered = inFront && std::fabs(spx - CntrEX) < 2.0f
	                              && std::fabs(spy - CntrEY) < 2.0f;
	const bool outside = dist > r;
	std::fprintf(stderr,
	    "[FOCUSTEST] '%s': %ld/%lu faces  centre (%.2f %.2f %.2f)  radius %.2f\n",
	    spec, used, total, c.x, c.y, c.z, r);
	std::fprintf(stderr,
	    "[FOCUSTEST] cam pos (%.2f %.2f %.2f) fwd (%.3f %.3f %.3f) dist %.2f (%.2fx radius)\n",
	    FC.ISource.x, FC.ISource.y, FC.ISource.z, dir.x, dir.y, dir.z,
	    dist, r > 0.0f ? dist / r : -1.0f);
	std::fprintf(stderr,
	    "[FOCUSTEST] centre -> px (%.1f, %.1f), screen centre (%.1f, %.1f): "
	    "centered %s; camera outside radius %s\n",
	    spx, spy, CntrEX, CntrEY, centered ? "PASS" : "FAIL",
	    outside ? "PASS" : "FAIL");
}

// OBJSCALE_TEST=<object>:<scale> — native validation for the per-object
// scale knob (PICK_TEST convention; rides the snapshot loops' DUMP_SURFACES
// hook, post-tick). Measures the FIRST matching instance's world AABB
// (subtree included — children compose the parent's scaled matrix), applies
// Editor_SetObjectScale, re-runs Animate_Objects (which folds EditorScale
// into the transform), measures again, and asserts the bounding radius grew
// by the requested factor (±5%). Instance-anchored on purpose: a name-keyed
// union over the 8 city taxis only grows marginally (each instance scales
// around its OWN pivot; their spacing doesn't). Then frames the camera on
// the PRE-scale bbox and re-renders (the FOCUS_TEST recipe: geometry only)
// so the snapshot PPM SHOWS the scaled object — px-diff an x1 vs xN run
// pair for pixel evidence. e.g.:
//   OBJSCALE_TEST="SHIP1.lwo:2" DUMP_SURFACES=1 ./DEMO --deferred \
//     --snapshot=city@t=300
static void editorRunObjScaleTestHook()
{
	const char *spec = std::getenv("OBJSCALE_TEST");
	if (!spec || !*spec || !CurScene || !View) return;
	static bool done = false;
	if (done) return;
	done = true;
	const std::string s = spec;
	const size_t colon = s.rfind(':');
	float scale = 0.0f;
	if (colon == std::string::npos
	    || std::sscanf(s.c_str() + colon + 1, "%f", &scale) != 1 || scale <= 0.0f) {
		std::fprintf(stderr, "[OBJSCALE] bad spec '%s' (want object:scale)\n", spec);
		return;
	}
	const std::string obj = s.substr(0, colon);
	// Anchor: the first Object carrying the (chunk-collapsed) name.
	const Object *anchor = nullptr;
	for (Object *O = CurScene->ObjectHead; O && !anchor; O = O->Next)
		if (O->Type == Obj_TriMesh && O->Data && O->Name
		    && Editor_ChunkBaseObjName(O->Name) == obj)
			anchor = O;
	if (!anchor) {
		std::fprintf(stderr, "[OBJSCALE] '%s': no such object — FAIL\n", obj.c_str());
		return;
	}
	// World AABB of the anchor's subtree (anchor + descendants; other
	// instances of the name are separate Objects and stay out).
	auto subtreeRadius = [&](Vector &centre) -> float {
		float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
		long nv = 0;
		for (Object *O = CurScene->ObjectHead; O; O = O->Next) {
			if (O->Type != Obj_TriMesh || !O->Data) continue;
			bool inSubtree = false;
			for (const Object *A = O; A; A = A->Parent)
				if (A == anchor) { inSubtree = true; break; }
			if (!inSubtree) continue;
			TriMesh *T = (TriMesh *)O->Data;
			for (DWord v = 0; v < T->VIndex; ++v) {
				Vector w;
				MatrixXVector(T->RotMat, &T->Verts[v].Pos, &w);
				Vector_SelfAdd(&w, &T->IPos);
				const float p[3] = { w.x, w.y, w.z };
				for (int a = 0; a < 3; ++a) {
					if (p[a] < lo[a]) lo[a] = p[a];
					if (p[a] > hi[a]) hi[a] = p[a];
				}
				++nv;
			}
		}
		if (!nv) return -1.0f;
		centre.x = (lo[0] + hi[0]) * 0.5f;
		centre.y = (lo[1] + hi[1]) * 0.5f;
		centre.z = (lo[2] + hi[2]) * 0.5f;
		const float dx = hi[0]-lo[0], dy = hi[1]-lo[1], dz = hi[2]-lo[2];
		return 0.5f * std::sqrt(dx*dx + dy*dy + dz*dz);
	};
	Vector c0, c1;
	const float r0 = subtreeRadius(c0);
	if (r0 <= 0.0f) {
		std::fprintf(stderr, "[OBJSCALE] '%s': no verts — FAIL\n", obj.c_str());
		return;
	}
	const int n = Editor_SetObjectScale(obj.c_str(), scale);
	// Fold the multiplier into RotMat/IPos (what the next tick would do).
	Animate_Objects(CurScene, nullptr);
	const float r1 = subtreeRadius(c1);
	const float ratio = r1 / r0;
	const bool pass = n > 0 && std::fabs(ratio - scale) <= 0.05f * scale;
	std::fprintf(stderr,
	    "[OBJSCALE] '%s' x%.3g: %d live mesh(es), radius %.3f -> %.3f "
	    "(x%.3f, want x%.3g) centre (%.1f %.1f %.1f)->(%.1f %.1f %.1f): %s\n",
	    obj.c_str(), scale, n, r0, r1, ratio, scale,
	    c0.x, c0.y, c0.z, c1.x, c1.y, c1.z, pass ? "PASS" : "FAIL");
	// Frame the camera on the PRE-scale bbox (c0/r0 → identical pose whatever
	// the scale, so an x1 vs x2 run pair px-diffs to just the object), then
	// re-render so the written snapshot SHOWS the scaled object.
	Vector dir(c0.x - View->ISource.x, c0.y - View->ISource.y, c0.z - View->ISource.z);
	const float len = std::sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
	if (len > 1e-4f) { dir.x /= len; dir.y /= len; dir.z /= len; }
	else             { dir.x = 0.0f; dir.y = 0.0f; dir.z = 1.0f; }
	const float dist = r0 * 2.5f < 6.0f ? 6.0f : r0 * 2.5f;
	FC.ISource = Vector(c0.x - dir.x * dist, c0.y - dir.y * dist, c0.z - dir.z * dist);
	Vector look = c0;
	Kick_Camera(&FC.ISource, &look, 0.0f, FC.Mat);
	if (FC.IFOV <= 0.0f) FC.IFOV = (View->IFOV > 0.0f) ? View->IFOV : 65.0f;
	CalcPersp(&FC);
	View = &FC;
	FOVX = View->PerspX;
	FOVY = View->PerspY;
	if (VPage && ZPage16 && CurScene) {
		std::memset(VPage, 0, PageSize);
		std::memset(ZPage16, 0, size_t(XRes) * size_t(YRes) * sizeof(word));
		Transform_Objects(CurScene, fds::g_mainCamera, fds::g_mainFaces);
		if (CAll) { Radix_Sort(FList, SList, CAll); Render(); }
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
	// CONTENT-ADDRESSED temp name. MaterialImport's loadRoleMapCached dedups by
	// the source PATH (path == texture identity — correct for the CLI dir scan and
	// the RVSM set-dir loader, whose paths are stable). A FIXED "/tmp/ed_import.EXT"
	// broke that invariant: a SECOND, DIFFERENT editor upload of the same extension
	// overwrote the file but produced the SAME cache key, so it reused the FIRST
	// upload's Texture — the "loading a second material shows the first" bug. Hash
	// the bytes into the name: distinct uploads → distinct paths → distinct cache
	// entries, while a re-upload of the SAME bytes still dedups (the reuse the
	// cache exists for — the wasm heap-grow OOM fix, SESSION_STATE editor stability).
	unsigned long long h = 1469598103934665603ULL;           // FNV-1a 64-bit
	for (unsigned long i = 0; i < len; ++i) { h ^= data[i]; h *= 1099511628211ULL; }
	char stem[24]; std::snprintf(stem, sizeof stem, "%016llx", h);
	std::string tmp = std::string("/tmp/ed_import_") + stem + (dot ? dot : ".png");
	FILE* f = std::fopen(tmp.c_str(), "wb");
	if (!f) { std::fprintf(stderr, "[EDITOR] import: can't open %s\n", tmp.c_str()); return false; }
	std::fwrite(data, 1, len, f);
	std::fclose(f);
	const bool ok = fds::MaterialImport_ApplyMapFile(CurScene, surface, role, tmp.c_str());
	if (ok && !std::strcmp(role, "metallic")) {
		// Metallic drives the ENV-reflection system (not the scalar
		// `reflection` slider): ApplyMapFile just auto-defaulted env_refl +
		// env_bake_fix on; drop THIS surface's baked probe store(s) so the
		// next frame's FramePrep re-bakes it with the new metal look (and
		// bakes a fresh probe for it). TARGETED, not whole-scene: re-baking
		// every reflective surface at once OOM'd the 4GB wasm editor on
		// greets. Without any invalidate the import looked like it did
		// nothing — the #1 "metallic has no effect" report.
		editorInvalidateSurfaceEnv(surface);
		std::fprintf(stderr, "[EDITOR] metallic import on '%s': env_refl=%d "
		             "env_bake_fix=%d — this surface's probe invalidated, "
		             "re-baking next frame\n",
		             surface, fds::FeatureFlags::env_refl() ? 1 : 0,
		             fds::FeatureFlags::env_bake_fix() ? 1 : 0);
	}
	if (ok) Editor_MarkDirty();
	return ok;
}

// Editor "reset map": restore a surface's (role) map slot to its authored
// default (MaterialImport keeps a pre-override stash). Mirrors the metallic
// side effect of Editor_ImportTexture: dropping a metallic map changes the
// env-reflection look, so the baked panoramas are invalidated for a re-bake.
bool Editor_ClearMap(const char* surface, const char* role)
{
	if (!surface || !role) return false;
	const bool ok = fds::MaterialImport_ClearSurfaceMap(CurScene, surface, role);
	if (ok && !std::strcmp(role, "metallic")) {
		editorInvalidateSurfaceEnv(surface);   // targeted, not whole-scene (wasm OOM)
		std::fprintf(stderr, "[EDITOR] metallic reset on '%s' — this surface's probe invalidated\n", surface);
	}
	if (ok) Editor_MarkDirty();
	return ok;
}

// ── Map-inspector overlay ("map viz") ───────────────────────────────────────
// Inspect a surface's maps on screen: blit the selected map's mip0 into the
// TOP-CENTER quarter of the final frame — the EnvReflection_DrawViz pano-
// viewer pattern (post-tonemap, pre-flip; RENDER.CPP calls Editor_DrawMapViz
// through the g_editorDrawMapViz hook installed at the bottom of this file).
// Off by default; Editor_SetMapViz("",...) / role "off" hides it again.
static std::string g_mapVizSurf;   // empty = overlay off
static std::string g_mapVizRole;

static const Texture *editorMapVizResolve()
{
	if (g_mapVizSurf.empty() || !CurScene) return nullptr;
	for (Material *M = MatLib; M; M = M->Next) {
		if (M->RelScene != CurScene || !M->Name) continue;
		if (Editor_BaseSurfName(M->Name) != g_mapVizSurf) continue;
		const Texture *t = nullptr;
		if      (g_mapVizRole == "albedo")                              t = M->Txtr;
		else if (g_mapVizRole == "normal")                              t = M->NormalMap;
		else if (g_mapVizRole == "height")                              t = M->HeightMap;
		else if (g_mapVizRole == "roughness" || g_mapVizRole == "rough") t = M->RoughnessMap;
		else if (g_mapVizRole == "ao")                                  t = M->AoMap;
		else if (g_mapVizRole == "metallic" || g_mapVizRole == "metal")  t = M->MetallicMap;
		if (t && t->Mipmap[0] && t->SizeX > 0 && t->SizeY > 0) return t;
	}
	return nullptr;   // surface has no such map (::mirUV clones share Texture*s)
}

std::string Editor_SetMapViz(const char *surface, const char *role)
{
	g_mapVizSurf = surface ? surface : "";
	g_mapVizRole = role ? role : "";
	if (g_mapVizSurf.empty() || g_mapVizRole.empty() || g_mapVizRole == "off") {
		g_mapVizSurf.clear();
		g_mapVizRole.clear();
		Editor_MarkDirty();
		return "off";
	}
	Editor_MarkDirty();
	const Texture *t = editorMapVizResolve();
	std::fprintf(stderr, "[EDITOR] map viz: '%s' %s -> %s\n",
	             g_mapVizSurf.c_str(), g_mapVizRole.c_str(),
	             t ? "on" : "no such map");
	if (t) return "on";
	// Keep the state armed anyway: an import can land the map a moment later
	// (the draw pass re-resolves every frame), but tell the caller the truth.
	return "no map";
}

// Linear (x,y) → Generate_Mipmaps' block-tiled texel index (outer loop block
// COLUMNS, inner block rows; rows-then-columns inside a block). Mirrors
// IMGCODE.CPP:1573-1594 / MeshOps.cpp::SwizzledOffset.
static inline size_t editorSwizzledOffset(int x, int y, int bsx, int bsy, int sizeY)
{
	const int BX = 1 << bsx, BY = 1 << bsy;
	const int blockRowsPerCol = sizeY >> bsy;
	const int bx = x >> bsx, by = y >> bsy;
	const int k = x & (BX - 1), j = y & (BY - 1);
	return (size_t(bx) * blockRowsPerCol + by) * size_t(BX * BY) + size_t(j) * BX + k;
}

void Editor_DrawMapViz()
{
#ifndef __EMSCRIPTEN__
	// MAPVIZ_TEST=surface:role — native headless validation (PICK_TEST env-
	// hook convention): arm the overlay once so a snapshot run captures it.
	{
		static bool armed = false;
		if (!armed) {
			armed = true;
			if (const char *spec = std::getenv("MAPVIZ_TEST")) {
				const std::string s = spec;
				const size_t c = s.find(':');
				if (c != std::string::npos) {
					const std::string st =
					    Editor_SetMapViz(s.substr(0, c).c_str(), s.substr(c + 1).c_str());
					std::fprintf(stderr, "[MAPVIZ] test hook '%s' -> %s\n", spec, st.c_str());
				} else {
					std::fprintf(stderr, "[MAPVIZ] want MAPVIZ_TEST=surface:role\n");
				}
			}
		}
	}
#endif
	if (g_mapVizSurf.empty()) return;
	if (!VPage || XRes <= 0 || YRes <= 0) return;
	// Never inside an offscreen render (env-probe bake / mirror RTT) — same
	// guard as EnvReflection_DrawViz, or the overlay bakes into every probe.
	if (fds::g_offscreenViewDepth > 0) return;
	const Texture *t = editorMapVizResolve();
	if (!t) return;
	const int srcW = t->SizeX, srcH = t->SizeY;
	// Fit the map into the top-center quarter (≤ half width, ≤ half height),
	// nearest sampling both ways (a 64² map upscales, a 1024² one downscales).
	int dw = XRes / 2;
	int dh = int((long long)dw * srcH / srcW);
	if (dh > YRes / 2) { dh = YRes / 2; dw = int((long long)dh * srcW / srcH); }
	if (dw < 2 || dh < 2) return;
	const int x0 = (XRes - dw) / 2, y0 = 8;
	if (x0 < 1 || y0 + dh + 1 >= YRes) return;
	const bool tiled = t->blockSizeX > 0 || t->blockSizeY > 0;
	const byte *src = t->Mipmap[0];
	dword *out = reinterpret_cast<dword *>(VPage);
	for (int y = 0; y < dh; ++y) {
		dword *row = out + size_t(y0 + y) * XRes + x0;
		const int sy = int((long long)y * srcH / dh);
		for (int x = 0; x < dw; ++x) {
			const int sx = int((long long)x * srcW / dw);
			const size_t idx = tiled
			    ? editorSwizzledOffset(sx, sy, t->blockSizeX, t->blockSizeY, srcH)
			    : size_t(sy) * srcW + sx;
			dword c;
			if (t->BPP == 32) {
				c = reinterpret_cast<const dword *>(src)[idx];   // BGRA as stored
			} else if (t->BPP == 16) {
				// MakeNormal16 RG pack (R | G<<8): show R/G raw, reconstruct
				// B (=Z) so the overlay reads like the source normal map.
				const uint16_t v = reinterpret_cast<const uint16_t *>(src)[idx];
				const float nX = float(v & 0xFF) * (1.0f / 127.5f) - 1.0f;
				const float nY = float((v >> 8) & 0xFF) * (1.0f / 127.5f) - 1.0f;
				const float z2 = 1.0f - nX * nX - nY * nY;
				const float nZ = z2 > 0.0f ? std::sqrt(z2) : 0.0f;
				const dword b = dword((nZ * 0.5f + 0.5f) * 255.0f);
				c = (dword(v & 0xFF) << 16) | (dword((v >> 8) & 0xFF) << 8) | b;
			} else {
				// 8-bit single-channel (MakeHeight8 height/rough/ao/metal).
				const dword g = src[idx];
				c = (g << 16) | (g << 8) | g;
			}
			row[x] = c | 0xFF000000u;
		}
	}
	// 1px ORANGE frame — the env pano viewer frames in green; keep them apart.
	const dword fc = 0xFFFF8800u;
	for (int x = -1; x <= dw; ++x) {
		out[size_t(y0 - 1) * XRes + x0 + x]  = fc;
		out[size_t(y0 + dh) * XRes + x0 + x] = fc;
	}
	for (int y = -1; y <= dh; ++y) {
		out[size_t(y0 + y) * XRes + x0 - 1]  = fc;
		out[size_t(y0 + y) * XRes + x0 + dw] = fc;
	}
}

} // namespace rev

// RENDER.CPP's post-tonemap tail calls the overlay through this hook so FDS
// never hard-references a DEMO symbol (FDS-only binaries — clipper_test —
// keep linking with the hook left null). Installed at static init; the pass
// is a no-op until Editor_SetMapViz arms it.
extern void (*g_editorDrawMapViz)();
[[maybe_unused]] static const bool s_mapVizHookInstalled = [] {
	g_editorDrawMapViz = &rev::Editor_DrawMapViz;
	return true;
}();

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
// Per-object uniform scale knob (objects panel). `name` is the entry's 'obj'
// field (raw chunk-collapsed engine object name). Returns the LIVE mesh count
// set — 0 means no such object or fully static-baked (the shell reports it).
int js_editorSetObjectScale(std::string name, float scale)
{
	return rev::Editor_SetObjectScale(name.c_str(), scale);
}
std::string js_editorSetUVMapping(std::string name, int proj,
                                  float sx, float sy, float sz, int axis)
{
	return rev::Editor_SetUVMapping(name.c_str(), proj, sx, sy, sz, axis);
}
// Map-inspector overlay: show `surface`'s map for `role` (albedo|normal|
// height|roughness|ao|metallic) as the top-center on-screen blit; role "off"
// (or an empty surface) hides it. Returns "on" / "off" / "no map".
std::string js_editorVizMap(std::string surface, std::string role)
{
	return rev::Editor_SetMapViz(surface.c_str(), role.c_str());
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
bool js_editorClearMap(std::string surface, std::string role)
{
	return rev::Editor_ClearMap(surface.c_str(), role.c_str());
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
// Scene-wide env-reflection defaults, EFFECTIVE values: the live flag
// (env_refl_scene_mode / env_bake_res_scene) when explicitly set, else the
// AUTHORED FLD scene-header value (Scene::EnvReflSceneMode/EnvBakeResScene,
// from the LWS FdsSceneEnvRefl/FdsSceneEnvBakeRes keywords). The editor's
// 'scene env defaults' row reads its truth from here; edits go through
// editorSetParam on the flags (live) and persist as payload.sceneEnv (the
// server patches the LWS + regens the FLD).
std::string js_editorGetSceneEnv()
{
	using FF = fds::FeatureFlags;
	int refl = 0, res = 0;
	if (FF::isSet(FF::IntId::env_refl_scene_mode)) refl = FF::env_refl_scene_mode();
	else if (CurScene) refl = (int)CurScene->EnvReflSceneMode;
	if (FF::isSet(FF::IntId::env_bake_res_scene)) res = FF::env_bake_res_scene();
	else if (CurScene) res = (int)CurScene->EnvBakeResScene;
	char buf[80];
	std::snprintf(buf, sizeof buf, "{\"refl\":%d,\"res\":%d}",
	              refl < 0 ? -1 : refl > 0 ? 1 : 0, res);
	return buf;
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
	emscripten::function("editorClearMap",       &js_editorClearMap);
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
	emscripten::function("editorSetObjectScale", &js_editorSetObjectScale);
	emscripten::function("editorSetUVMapping",   &js_editorSetUVMapping);
	emscripten::function("editorVizMap",         &js_editorVizMap);
	emscripten::function("editorRebakeEnv",      &js_editorRebakeEnv);
	emscripten::function("editorEnvPanoCount",   &js_editorEnvPanoCount);
	emscripten::function("editorEnvInfo",        &js_editorEnvInfo);
	emscripten::function("editorEnvPanoInfo",    &js_editorEnvPanoInfo);
	emscripten::function("editorReadPixel",      &js_editorReadPixel);
	emscripten::function("editorGetParams",      &js_editorGetParams);
	emscripten::function("editorSetParam",       &js_editorSetParam);
	emscripten::function("editorUnsetParam",     &js_editorUnsetParam);
	emscripten::function("editorGetSceneEnv",    &js_editorGetSceneEnv);
}
#endif // __EMSCRIPTEN__
