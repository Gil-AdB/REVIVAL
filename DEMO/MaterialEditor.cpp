#include "MaterialEditor.h"

#include <Base/FDS_VARS.H>   // MatLib, CurScene
#include <Base/Material.h>
#include <Base/Texture.h>

#include <cstdio>
#include <cstring>
#include <unordered_set>

namespace rev {

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
		if (!seen.insert(M->Name).second) continue;   // de-dup by name

		if (!first) out += ",";
		first = false;
		out += "{\"name\":\"";
		jsonEscape(out, M->Name);
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
		if (!M->Name || std::strcmp(M->Name, name) != 0) continue;
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
	return any;
}

} // namespace rev
