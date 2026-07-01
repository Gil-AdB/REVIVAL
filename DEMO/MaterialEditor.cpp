#include "MaterialEditor.h"
#include "MaterialImport.h"   // MaterialImport_ApplyMapFile

#include <Base/FDS_VARS.H>   // MatLib, CurScene
#include <Base/Material.h>
#include <Base/Texture.h>
#include <Base/FDS_DECS.H>   // Scene_GetMatTable, MatTable
#include <Base/Scene.h>      // Scene::Ambient
#include <Base/FeatureFlags.h>
#include <FILLERS/Mekalele.h> // meka::GBuffer, g_gbuffer (matID G-buffer plane)

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>

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

std::string Editor_GetSurfacesJSON()
{
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
		appendNum(out, "flags",        (double)M->Flags);out += ",";
		out += "\"texture\":\"";
		jsonEscape(out, (M->Txtr && M->Txtr->FileName) ? M->Txtr->FileName : "");
		out += "\"}";
	}
	out += "]";
	return out;
}

bool Editor_SetSurfaceProp(const char* name, const char* key, float value)
{
	if (!name || !key) return false;
	bool any = false;
	for (Material* M = MatLib; M; M = M->Next) {
		if (M->RelScene != CurScene) continue;
		if (!M->Name || Editor_BaseSurfName(M->Name) != name) continue;   // ::mirUV clones too
		if      (!std::strcmp(key, "baseR"))        M->BaseCol.R = value;
		else if (!std::strcmp(key, "baseG"))        M->BaseCol.G = value;
		else if (!std::strcmp(key, "baseB"))        M->BaseCol.B = value;
		else if (!std::strcmp(key, "diffuse"))      M->Diffuse = value;
		else if (!std::strcmp(key, "specular"))     M->Specular = value;
		else if (!std::strcmp(key, "glossiness"))   M->Glossiness = (unsigned short)(value < 0 ? 0 : value);
		else if (!std::strcmp(key, "luminosity"))   M->Luminosity = value;
		else if (!std::strcmp(key, "transparency")) M->Transparency = value;
		else if (!std::strcmp(key, "reflection"))   M->Reflection = value;
		else return false;   // unknown key — fail fast, don't silently no-op
		any = true;
	}
	if (any) Editor_MarkDirty();
	return any;
}

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
void js_editorHighlight(std::string name)
{
	Editor_SetHighlight(name.c_str());
	rev::Editor_MarkDirty();
}
bool js_editorSetSurfaceProp(std::string name, std::string key, float value)
{
	return rev::Editor_SetSurfaceProp(name.c_str(), key.c_str(), value);
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
	char buf[300];
	std::snprintf(buf, sizeof buf,
	  "{\"matlib_lum\":%.2f,\"matlib_count\":%d,\"mattable_lum\":%.2f,\"mattable_count\":%u,\"same_ptr\":%d}",
	  ml ? ml->Luminosity : -1.0f, mlCount, tt ? tt->Luminosity : -1.0f, (unsigned)mt.count, (ml == tt) ? 1 : 0);
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
// draw it). Reads the last rendered frame's G-buffer matID plane; returns the
// base surface name ("" = no surface: sentinel pixel, forward-rendered, or
// out of range).
std::string js_editorPick(float u, float v)
{
	if (!g_gbuffer || g_gbuffer->txtr.empty() || !CurScene) return "";
	const int x = int(u * float(XRes));
	const int y = int(v * float(YRes));
	if (x < 0 || y < 0 || x >= XRes || y >= YRes) return "";
	const size_t i = size_t(y) * size_t(XRes) + size_t(x);
	if (i >= g_gbuffer->txtr.size()) return "";
	const unsigned mid = (g_gbuffer->txtr[i] >> 20) & 0xFF;
	MatTable mt = Scene_GetMatTable(CurScene);
	if (mid >= mt.count || !mt.data[mid] || !mt.data[mid]->Name) return "";
	return rev::Editor_BaseSurfName(mt.data[mid]->Name);
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
	emscripten::function("editorSetSurfaceProp", &js_editorSetSurfaceProp);
	emscripten::function("editorImportTexture",  &js_editorImportTexture);
	emscripten::function("editorMatDebug",       &js_editorMatDebug);
	emscripten::function("editorHighlight",      &js_editorHighlight);
	emscripten::function("editorFlags",          &js_editorFlags);
	emscripten::function("editorProbe",          &js_editorProbe);
	emscripten::function("editorPick",           &js_editorPick);
	emscripten::function("editorClassifyMap",    &js_editorClassifyMap);
}
#endif // __EMSCRIPTEN__
