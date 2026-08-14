// --mem_census reporters for the SCENE-OWNED allocations: geometry, texture
// pixel data + mip chains, the env-reflection probe stores, and the software
// framebuffer. These live here rather than beside their owners because they
// are reachable through public globals (CurScene / MatLib / MainSurf) and
// putting them in one file keeps them out of hot translation units.
//
// See FDS/Base/MemCensus.h for what the flag is for and how to add a
// subsystem. Nothing in this file allocates or mutates anything.

#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Base/FeatureFlags.h"
#include "Base/MemCensus.h"
#include "Base/Material.h"
#include "Base/Object.h"
#include "Base/Omni.h"
#include "Base/Scene.h"
#include "Base/Texture.h"
#include "Base/TriMesh.h"
#include "Base/Vertex.h"
#include "Base/Face.h"
#include "Base/VertexFrame.h"
#include "RENDER/EnvBake.h"

#include <algorithm>
#include <cstddef>
#include <unordered_set>
#include <vector>

namespace fds {
// Called by MemCensus::report(). WHY IT EXISTS: FDS is a static library and
// this TU defines nothing anyone references, so without an anchor the linker
// drops the whole object — and every reporter registered from it — silently.
// That is exactly what happened the first time this file was written: the
// census ran, printed 26 buffers, and simply had no geometry or texture rows.
void MemCensusScene_Anchor() {}
} // namespace fds

namespace {

// Exact byte size of a Texture's single Data block, reconstructed with the
// SAME walk Generate_Mipmaps used to size it (FDS/IMGCODE/IMGCODE.CPP): the
// whole mip chain is ONE allocation, block-tiled, and Mipmap[i] are views into
// it. A texture that never went through Generate_Mipmaps has numMipmaps <= 1
// and this collapses to SizeX*SizeY*CPP, which is correct for those too.
size_t textureBytes(const Texture *T) {
    if (!T || !T->Data || T->SizeX <= 0 || T->SizeY <= 0) return 0;
    const int bsx = T->blockSizeX, bsy = T->blockSizeY;
    long long X = T->SizeX >> bsx, Y = T->SizeY >> bsy;
    if (X <= 0 || Y <= 0) return 0;
    long long px = X * Y;
    const unsigned levels = T->numMipmaps ? T->numMipmaps : 1u;
    for (unsigned i = 1; i < levels; ++i) {
        X = (X + 1) >> 1; Y = (Y + 1) >> 1;
        px += X * Y;
    }
    px <<= (bsx + bsy);
    const long long cpp = ((long long)(T->BPP) + 7) >> 3;
    return size_t(px * (cpp > 0 ? cpp : 4));
}

// Number of texels in mip 0, used to size PomHorizonMap (which is
// kPomHorizonAzimuths bytes per texel over the WHOLE chain).
size_t textureChainTexels(const Texture *T) {
    const size_t b = textureBytes(T);
    const long long cpp = T ? (((long long)(T->BPP) + 7) >> 3) : 4;
    return cpp > 0 ? b / size_t(cpp) : 0;
}

// Mesh names live on Object, not TriMesh — build the reverse map once per
// report so the clone rows can name what they found.
const char *meshName(const Scene *Sc, const TriMesh *T) {
    if (!Sc) return nullptr;
    for (Object *O = Sc->ObjectHead; O; O = O->Next)
        if (O->Data == (const void*)T) return O->Name;
    return nullptr;
}

} // namespace

// ── Scene geometry: Vertex[], Face[], and their per-mesh companions ────────
// One authored polygon costs sizeof(Face)=162 B and one vertex 140 B, before
// the 72 B/vertex VertexFrame SoA slab and the 16 B/vertex prelit Color. So a
// mesh that gets tessellated pays ~374 B per new vertex + 162 B per new face —
// which is why displacement's face count shows up here before it shows up
// anywhere else.
static void MemCensus_SceneGeometry() {
    if (!CurScene) return;
    size_t verts = 0, faces = 0, edges = 0, sl = 0, wv = 0, frame = 0;
    size_t nV = 0, nF = 0, meshes = 0;
    size_t cloneV = 0, cloneF = 0, clones = 0;
    for (TriMesh *T = CurScene->TriMeshHead; T; T = T->Next) {
        ++meshes;
        nV += T->VIndex; nF += T->FIndex;
        if (T->Verts) verts += size_t(T->VIndex) * sizeof(Vertex);
        if (T->Faces) faces += size_t(T->FIndex) * sizeof(Face);
        if (T->Edges) edges += size_t(T->EIndex) * sizeof(Edge);
        if (T->SL)    sl    += size_t(T->VIndex) * sizeof(Color);
        if (T->worldVerts) wv += size_t(T->VIndex) * sizeof(Vector);
        if (T->frame) frame += size_t(T->frame->capacity) * 72u;
        // Mirror / shadow-proxy clones carry "Mirror" or "proxy" in the name;
        // they are full geometry duplicates and worth their own line.
        const char *n = meshName(CurScene, T);
        if (n && (std::strstr(n, "irror") || std::strstr(n, "roxy")
                  || std::strstr(n, "lone"))) {
            ++clones;
            cloneV += size_t(T->VIndex) * sizeof(Vertex);
            cloneF += size_t(T->FIndex) * sizeof(Face);
        }
    }
    fds::MemCensus::add("geometry", "Vertex[] (all meshes)", verts, true,
        "%zu meshes, %zu verts x sizeof(Vertex)=%zu", meshes, nV, sizeof(Vertex));
    fds::MemCensus::add("geometry", "Face[] (all meshes)", faces, true,
        "%zu faces x sizeof(Face)=%zu", nF, sizeof(Face));
    fds::MemCensus::add("geometry", "VertexFrame SoA slabs", frame, true,
        "per-mesh ceil8(VIndex) x 72 B (16 float + 2 u32 output fields)");
    fds::MemCensus::add("geometry", "prelit Color SL[]", sl, false,
        "verts x sizeof(Color)=%zu", sizeof(Color));
    fds::MemCensus::add("geometry", "worldVerts[]", wv, true,
        "static-chunk world positions: verts x sizeof(Vector)=%zu", sizeof(Vector));
    fds::MemCensus::add("geometry", "Edge[]", edges, false, "edges x sizeof(Edge)=%zu",
        sizeof(Edge));
    fds::MemCensus::add("geometry", "  of which mirror/proxy CLONES", cloneV + cloneF, true,
        "%zu clone meshes (name-matched) — full duplicates of source geometry", clones);
    if (CurScene->Pcl)
        fds::MemCensus::add("geometry", "particle pool", size_t(CurScene->NumOfParticles)
            * sizeof(Particle), true,
            "NumOfParticles=%d x sizeof(Particle)=%zu (each carries an inline "
            "Vertex TrailV[4] + Face TrailF[2])",
            CurScene->NumOfParticles, sizeof(Particle));
}
FDS_MEMCENSUS_REPORTER(MemCensus_SceneGeometry);

// ── Textures: one Data block per texture holds its whole mip chain ─────────
// Deduped by pointer — a texture shared by twelve materials is one allocation.
// Broken out by ROLE because the derived maps are where the surprises are: a
// horizon map is kPomHorizonAzimuths (8) bytes per texel over the full chain,
// i.e. ~10.7x the source image's pixel count.
static void MemCensus_Textures() {
    std::unordered_set<const void*> seen;
    size_t diffuse = 0, normal = 0, height = 0, cone = 0, rough = 0, metal = 0;
    size_t ao = 0, env = 0, hemi = 0, horizon = 0, other = 0;
    size_t nTex = 0, nHorizon = 0, nHemi = 0;
    auto acc = [&](const Texture *T, size_t &bucket) {
        if (!T || !seen.insert(T).second) return;
        const size_t b = textureBytes(T);
        if (!b) return;
        bucket += b; ++nTex;
    };
    for (Material *M = MatLib; M; M = M->Next) {
        acc(M->Txtr, diffuse);
        acc(M->NormalMap, normal);
        acc(M->HeightMap, height);
        acc(M->ConeMap, cone);
        acc(M->RoughnessMap, rough);
        acc(M->MetallicMap, metal);
        acc(M->AoMap, ao);
        acc(M->EnvTexture, env);
        if (M->PomHorizon && M->PomHorizon->data && seen.insert(M->PomHorizon).second) {
            horizon += textureChainTexels(M->HeightMap) * size_t(kPomHorizonAzimuths);
            ++nHorizon;
        }
    }
    if (CurScene)
        for (TriMesh *T = CurScene->TriMeshHead; T; T = T->Next)
            for (int k = 0; k < 6; ++k)
                if (T->EnvHemiSheets[k] && seen.insert(T->EnvHemiSheets[k]).second) {
                    hemi += textureBytes(T->EnvHemiSheets[k]); ++nHemi;
                }
    (void)other;
    fds::MemCensus::add("texture", "diffuse (mip chains)", diffuse, true,
        "%zu unique textures; each = sum over mips of W*H x CPP, ~1.33x mip0", nTex);
    fds::MemCensus::add("texture", "normal maps", normal, true, "same chain formula");
    fds::MemCensus::add("texture", "height maps", height, true, "same chain formula");
    fds::MemCensus::add("texture", "cone maps (--parallax_pom)", cone, true,
        "1 B/texel over the chain, mirrors HeightMap's layout");
    fds::MemCensus::add("texture", "roughness maps", rough, true, "same chain formula");
    fds::MemCensus::add("texture", "metalness maps", metal, true, "same chain formula");
    fds::MemCensus::add("texture", "AO maps", ao, true, "same chain formula");
    fds::MemCensus::add("texture", "env textures", env, true, "same chain formula");
    fds::MemCensus::add("texture", "POM horizon maps", horizon, true,
        "%zu maps x chain texels x kPomHorizonAzimuths=%d B/texel — ~10.7x the "
        "source image", nHorizon, kPomHorizonAzimuths);
    fds::MemCensus::add("texture", "env paraboloid hemi sheets (city)", hemi, true,
        "%zu sheets = 6 per reflective building x sheetRes^2 x 4", nHemi);
}
FDS_MEMCENSUS_REPORTER(MemCensus_Textures);

// ── Env-reflection probe stores ────────────────────────────────────────────
// Walked through the public matID table so this file needs nothing from
// EnvBake.cpp's internals. Deduped by store pointer (materials share bakes).
static void MemCensus_EnvProbes() {
    if (!CurScene) return;
    const fds::EnvPanoLinear *const *tab = fds::EnvReflection_Table(CurScene);
    if (!tab) return;
    std::unordered_set<const void*> seen;
    size_t bytes = 0, stores = 0;
    int res = 0, mips = 0;
    bool cube = false;
    for (int i = 0; i < 256; ++i) {
        const fds::EnvPanoLinear *p = tab[i];
        if (!p || !seen.insert(p).second) continue;
        ++stores;
        res = std::max(res, p->W);
        mips = std::max(mips, p->numMips);
        cube = cube || p->isCube;
        for (int k = 0; k < p->numMips && k < fds::EnvPanoLinear::kMaxMips; ++k) {
            if (!p->mip[k]) continue;
            const size_t w = size_t(p->W >> k), h = size_t(p->H >> k);
            bytes += (p->isCube ? 6u * w * w : w * h) * sizeof(unsigned);
        }
    }
    fds::MemCensus::add("envprobe", "panorama mip chains", bytes, true,
        "%zu stores x %s res=%d^2 x %d mips x u32(4); cube costs 6 faces/level, "
        "chain sum ~1.33x level 0", stores, cube ? "CUBE" : "equirect", res, mips);
}
FDS_MEMCENSUS_REPORTER(MemCensus_EnvProbes);

// ── The software framebuffer and its depth buffers ────────────────────────
static void MemCensus_Framebuffer() {
    const size_t px = size_t(XRes) * size_t(YRes);
    if (!px) return;
    fds::MemCensus::add("surface", "VPage (colour)", size_t(PageSize), true,
        "PageSize = BPSL x YRes = %d*%d x 4 B/px", XRes, YRes);
    fds::MemCensus::add("surface", "ZPage16 (depth)", ZPage16 ? px * sizeof(word) : 0,
        true, "W*H=%d*%d x u16(2)", XRes, YRes);
}
FDS_MEMCENSUS_REPORTER(MemCensus_Framebuffer);
