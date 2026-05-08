/*
		 Flood Demo System - Flood scene material utility
		 -------------------------------------------------
*/

#include <stdio.h>
//#include <dos.h>
#include <stdlib.h>
//#include <conio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include <cstdint>
#include <string>
#include <unordered_map>

#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"

#include "LWREAD.H"
#include "FLD_READ.H"
//#define DebugMode

namespace {

// Dedup key for `Material`. The 1998 FLD authoring emits one Material per
// .lwo surface name per object — City alone has 424 entries, the vast
// majority of which are byte-identical except for `Name` (cosmetic) and
// `ID` (assigned later). The deferred G-buffer's mat32 packing has only
// 8 bits for matID, so we collapse runtime-equivalent Materials at FLD
// load and let multiple Faces share one canonical `Material*`.
//
// Equivalence is decided over the **runtime-read** field set: `Txtr`,
// `RelScene`, `Flags`, `BaseCol`, lighting params (`Diffuse`/`Specular`/
// `Reflection`/`Transparency`/`Luminosity`/`Glossiness`), and the env-
// texture pointer. The remaining Material fields (TextureSize, ColorTex
// strings, AAStrength, ZBufferWrite/Test, …) are written at FLD load
// and never read at render time — confirmed by a tree-wide grep — so
// excluding them from the key is safe and keeps dedup aggressive.
struct MatKey {
	Texture* txtr;
	Scene*   relScene;
	Texture* envTexture;
	DWord    flags;
	float    luminosity, diffuse, specular, reflection, transparency;
	uint32_t glossiness;
	float    baseR, baseG, baseB, baseA;
};

inline bool operator==(const MatKey& a, const MatKey& b) noexcept {
	return a.txtr == b.txtr
		&& a.relScene == b.relScene
		&& a.envTexture == b.envTexture
		&& a.flags == b.flags
		&& a.luminosity == b.luminosity
		&& a.diffuse == b.diffuse
		&& a.specular == b.specular
		&& a.reflection == b.reflection
		&& a.transparency == b.transparency
		&& a.glossiness == b.glossiness
		&& a.baseR == b.baseR
		&& a.baseG == b.baseG
		&& a.baseB == b.baseB
		&& a.baseA == b.baseA;
}

struct MatKeyHash {
	size_t operator()(const MatKey& k) const noexcept {
		auto mix = [](size_t h, size_t x) {
			return h ^ (x + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
		};
		auto fbits = [](float f) {
			uint32_t u; std::memcpy(&u, &f, sizeof(u)); return size_t(u);
		};
		size_t h = std::hash<const void*>{}(k.txtr);
		h = mix(h, std::hash<const void*>{}(k.relScene));
		h = mix(h, std::hash<const void*>{}(k.envTexture));
		h = mix(h, k.flags);
		h = mix(h, fbits(k.luminosity));
		h = mix(h, fbits(k.diffuse));
		h = mix(h, fbits(k.specular));
		h = mix(h, fbits(k.reflection));
		h = mix(h, fbits(k.transparency));
		h = mix(h, k.glossiness);
		h = mix(h, fbits(k.baseR));
		h = mix(h, fbits(k.baseG));
		h = mix(h, fbits(k.baseB));
		h = mix(h, fbits(k.baseA));
		return h;
	}
};

std::unordered_map<MatKey, Material*, MatKeyHash> g_dedupMap;

// Texture lookup is keyed on (filename, scene). Replaces the O(n²)
// linked-list walk that AddMaterial used to do per call.
std::unordered_map<std::string, Texture*> g_texByPath;

// Stats: how many AddMaterial calls collapsed onto an existing entry.
struct DedupStats { uint32_t calls = 0; uint32_t hits = 0; };
std::unordered_map<Scene*, DedupStats> g_dedupStats;

MatKey makeKey(const Material& M) {
	return MatKey{
		M.Txtr, M.RelScene, M.EnvTexture,
		M.Flags,
		M.Luminosity, M.Diffuse, M.Specular, M.Reflection, M.Transparency,
		uint32_t(M.Glossiness),
		M.BaseCol.R, M.BaseCol.G, M.BaseCol.B, M.BaseCol.A,
	};
}

Texture* loadOrLookupTexture(const char* uppercasePath, Scene* sceneGroup) {
	auto it = g_texByPath.find(uppercasePath);
	if (it != g_texByPath.end()) return it->second;

	Texture* Tx = new Texture;
	Tx->FileName = _strdup(uppercasePath);
#ifdef DebugMode
	printf("Attempting to identify texture \"%s\"...\n", uppercasePath);
#endif
	Identify_Texture(Tx);
	if (!Tx->BPP) {
		printf("Error identifying texture \"%s\"!\n", uppercasePath);
		delete Tx;
		g_texByPath.emplace(uppercasePath, nullptr);
		return nullptr;
	}
	g_texByPath.emplace(uppercasePath, Tx);
	return Tx;
}

} // namespace

Material* AddMaterial(FldMat *OrgMat, Scene *SceneGroup)
{
	if (!OrgMat) return nullptr;

	auto& stats = g_dedupStats[SceneGroup];
	++stats.calls;

	// Build a candidate Material in a stack-local. Don't link to MatLib
	// or insert into the dedup map until we know the candidate is unique.
	Material candidate;
	candidate.Name     = OrgMat->Name;
	candidate.Flags    = 0;
	candidate.Diffuse  = OrgMat->Diffuse * 0.01f;
	candidate.Specular = OrgMat->Specular * 0.01f;
	candidate.RelScene = SceneGroup;

	if (*OrgMat->TextureImage) {
		// Path-uppercasing rationale (preserved from the original): emscripten's
		// MEMFS is case-sensitive, and downstream string-match consumers (e.g.
		// GreetsGenerator::AttachMatToScene matching "TEXTURES/P_TEXT.JPG")
		// need a stable casing. macOS's default case-insensitive APFS hides
		// the load-side mismatch but exposes the strcmp-identity issue.
		std::string path;
		path.reserve(strlen(OrgMat->TextureImage) + 15);
		path = "TEXTURES/";
		path += OrgMat->TextureImage;
		for (size_t i = 9; i < path.size(); ++i)
			path[i] = (char)toupper((unsigned char)path[i]);

		candidate.Txtr = loadOrLookupTexture(path.c_str(), SceneGroup);

		// Copy the rest of OrgMat into candidate. Most of these fields are
		// effectively dead at render time (used at convert time for UV setup
		// in Get_UV, then never read), but we preserve the full copy so any
		// future reader sees the same structure as before.
		candidate.TFlags              = OrgMat->Flags;
		candidate.BaseCol.R           = OrgMat->Color.Red;
		candidate.BaseCol.G           = OrgMat->Color.Green;
		candidate.BaseCol.B           = OrgMat->Color.Blue;
		candidate.Luminosity          = OrgMat->Luminosity;
		candidate.Reflection          = OrgMat->Reflection;
		candidate.Transparency        = OrgMat->Transparency;
		candidate.Glossiness          = OrgMat->Glossiness;
		candidate.ReflectionMode      = OrgMat->ReflectionMode;
		candidate.ReflectionImage     = OrgMat->ReflectionImage;
		candidate.ReflectionSeamAngle = OrgMat->ReflectionSeamAngle;
		candidate.RefractiveIndex     = OrgMat->RefractiveIndex;
		candidate.EdgeTransparency    = OrgMat->EdgeTransparency;
		candidate.MaxSmoothingAngle   = OrgMat->MaxSmoothingAngle;
		candidate.ColorTexture        = OrgMat->ColorTexture;
		candidate.DiffuseTexture      = OrgMat->DiffuseTexture;
		candidate.SpecularTexture     = OrgMat->SpecularTexture;
		candidate.ReflectionTexture   = OrgMat->ReflectionTexture;
		candidate.TransparencyTexture = OrgMat->TransparencyTexture;
		candidate.BumpTexture         = OrgMat->BumpTexture;
		candidate.TextureFlags        = OrgMat->TextureFlags;
		candidate.TextureSize         = OrgMat->TextureSize;
		candidate.TextureCenter       = OrgMat->TextureCenter;
		candidate.TextureFallOff      = OrgMat->TextureFallOff;
		candidate.TextureVelocity     = OrgMat->TextureVelocity;
		candidate.TextureAlpha        = OrgMat->TextureAlpha;
		candidate.NoiseFrequencies    = OrgMat->NoiseFrequencies;
		candidate.TextureWrapX        = OrgMat->TextureWrapX;
		candidate.TextureWrapY        = OrgMat->TextureWrapY;
		candidate.AAStrength          = OrgMat->AAStrength;
		candidate.Opacity             = OrgMat->Opacity;
		if (candidate.TFlags & Surf_Smoothing)   candidate.Flags |= Mat_RGBInterp;
		if (candidate.TFlags & Surf_DoubleSided) candidate.Flags |= Mat_TwoSided;
		if (candidate.Transparency > 0.0)        candidate.Flags |= Mat_Transparent;

		if (candidate.TextureFlags & Texture_WorldCoords)
			printf("WARNING! World coordinates are currently unsupported!\n (%s)", candidate.Name);
	}

	// Dedup on the runtime-relevant subset. Two FldMat entries with
	// matching key collapse to one canonical Material* — the second
	// caller gets the first one back, no allocation, no MatLib link.
	MatKey key = makeKey(candidate);
	auto it = g_dedupMap.find(key);
	if (it != g_dedupMap.end()) {
		++stats.hits;
		return it->second;
	}

	Material* M = getAlignedType<Material>(16);
	*M = candidate;
	// Head-insert into MatLib. Order doesn't matter for any consumer
	// (Unify_Textures, IMGCODE walks, etc. iterate the full list); the
	// linked-list walk that ConvertFace used to do is gone.
	M->Next = MatLib;
	M->Prev = nullptr;
	if (MatLib) MatLib->Prev = M;
	MatLib = M;
	g_dedupMap.emplace(key, M);
	return M;
}

void DumpMaterialDedupStats() {
	for (const auto& [scene, stats] : g_dedupStats) {
		fprintf(stderr,
			"[MAT-DEDUP] scene=%p AddMaterial calls=%u dedup hits=%u "
			"(%.0f%% collapsed)\n",
			(void*)scene, stats.calls, stats.hits,
			stats.calls ? 100.0 * stats.hits / stats.calls : 0.0);
	}
}


// Cartesian Coordinates ==> Cylindrical Coordinates
void xyztoh(float x,float y,float z,float *h)
{
	if (x == 0.0f && z == 0.0f) *h = 0.0f;
	else
	{
		if (z == 0.0f)
			*h = (x < 0.0f) ? PI_D2 : -PI_D2;
		else if (z < 0.0f)
			*h = -atanf(x / z) + PI;
		else
			*h = -atanf(x / z);
	}
}

//Cartesian Coordinates ==> Spherical Coordinates
void xyztohp(float x,float y,float z,float *h,float *p)
{
	if (x == 0.0f && z == 0.0f)
	{
		*h = 0.0f;
		if (y != 0.0f)
			*p = (y < 0.0f) ? -PI_D2 : PI_D2;
		else
			*p = 0.0f;
	}
	else
	{
		if (z == 0.0f)
			*h = (x < 0.0f) ? PI_D2 : -PI_D2;
		else if (z < 0.0f)
			*h = -atanf(x / z) + PI;
		else
			*h = -atanf(x / z);
		x = sqrtf(x * x + z * z);
		if (x == 0.0f)
			*p = (y < 0.0f) ? -PI_D2 : PI_D2;
		else
			*p = atanf(y / x);
	}
}

float fract(float f)
{
/*	if (f >= 0.0 && f <= 1.0)
		return f*256.0;
	else
		return (f-floor(f))*256.0;*/
		return f;
}

void Get_UV(Face *F,Vertex *Vtx,FldMat *Mat)
{
	Vector W,U,V,N;
	float s,t,lon,lat;

	Vector_Sub(&Vtx->Pos,&Mat->TextureCenter,&V);

	if (!strcmp(Mat->ColorTexture,"Planar Image Map"))
	{
		s = (Mat->TextureFlags&Texture_XAxis) ? V.z / Mat->TextureSize.z + .5f :
			V.x / Mat->TextureSize.x + .5;
		t = (Mat->TextureFlags&Texture_YAxis) ? -V.z / Mat->TextureSize.z + .5f :
			-V.y / Mat->TextureSize.y + .5f;

		Vtx->U = fract(s);
		Vtx->V = fract(t);
#ifdef DebugMode
		printf("Mapping Coordinates (%f,%f) using Planar mapping...\n",Vtx->U,Vtx->V);
#endif
	}
	else if (!strcmp(Mat->ColorTexture,"Cylindrical Image Map"))
	{
		if (Mat->TextureFlags&Texture_XAxis) {
			xyztoh(V.z,V.x,-V.y,&lon);
			t = -V.x / Mat->TextureSize.x + .5f;
		}
		else if (Mat->TextureFlags&Texture_YAxis) {
			xyztoh(-V.x,V.y,V.z,&lon);
			t = -V.y / Mat->TextureSize.y + .5f;
		} else {
			xyztoh(-V.x,V.z,-V.y,&lon);
			t = -V.z / Mat->TextureSize.z + .5f;
		}
		lon = 1.0 - lon / PI_M2;
/*		if (Mat->TFP0 != 1.0)
			lon = fract(lon) * Mat->TFP0;*/
		Vtx->U = fract(lon);//(lon-floor(lon))*256.0;
		Vtx->V = fract(t);//(t-floor(t))*256.0;
#ifdef DebugMode
		printf("Mapping Coordinates (%f,%f) using Cylindrical mapping...\n",Vtx->U,Vtx->V);
#endif
	}
	else if (!strcmp(Mat->ColorTexture,"Spherical Image Map"))
	{
		if (Mat->TextureFlags&Texture_XAxis)
			xyztohp(V.z,V.x,-V.y,&lon,&lat);
		else if (Mat->TextureFlags&Texture_YAxis)
			xyztohp(-V.x,V.y,V.z,&lon,&lat);
		else
			xyztohp(-V.x,V.z,-V.y,&lon,&lat);
		lon = 1.0f - lon / PI_M2;
		lat = 0.5f - lat / PI;

/*		 if (Mat->TFP0 != 1.0)
			 lon = fract(lon) * Mat->TFP0;
		 if (Mat->TFP1 != 1.0)
			 lat = fract(lat) * Mat->TFP1;*/
		Vtx->U = fract(lon);//(lon-floor(lon))*256.0;
		Vtx->V = fract(lat);//(lat-floor(lat))*256.0;
#ifdef DebugMode
		printf("Mapping Coordinates (%f,%f) using Spherical mapping...\n",Vtx->U,Vtx->V);
#endif
	} else if (!strcmp(Mat->ColorTexture,"Cubic Image Map"))
	{
		Vector_Sub(&F->B->Pos,&F->A->Pos,&U);
		Vector_Sub(&F->C->Pos,&F->A->Pos,&W);
		Cross_Product(&U,&W,&N);
		N.x = fabsf(N.x); N.y = fabsf(N.y); N.z = fabsf(N.z);
		char XAxis=0,YAxis=0;

		if (N.x>N.y&&N.x>N.z) XAxis=1;
		else if (N.y>N.x&&N.y>N.z) YAxis=1;

		s = (XAxis) ?  V.z / Mat->TextureSize.z :  V.x / Mat->TextureSize.x;
		t = (YAxis) ? -V.z / Mat->TextureSize.z :	-V.y / Mat->TextureSize.y;

		s += .5f; t += .5f;

		Vtx->U = fract(s);
		Vtx->V = fract(t);

#ifdef DebugMode
		printf("Mapping Coordinates (%f,%f) using Spherical mapping...\n",Vtx->U,Vtx->V);
#endif
	}
/*	else if (Mat->ColorTexture[0]!=0)
		printf("Warning! Color texture \"%s\" is illegal!\n",Mat->ColorTexture);*/
}


void Get_Mapping(Face *F,FldMat *Mat)
{
	Get_UV(F,F->A,Mat);
	Get_UV(F,F->B,Mat);
	Get_UV(F,F->C,Mat);
	F->U1 = F->A->U;
	F->V1 = F->A->V;
	F->U2 = F->B->U;
	F->V2 = F->B->V;
	F->U3 = F->C->U;
	F->V3 = F->C->V;
}
